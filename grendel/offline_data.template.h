#ifndef OFFLINE_DATA_TEMPLATE_H
#define OFFLINE_DATA_TEMPLATE_H

#include "offline_data.h"
#include "scratch_data.h"
#include "sparse_matrix_simd.h"

#include <deal.II/base/graph_coloring.h>
#include <deal.II/base/parallel.h>
#include <deal.II/base/work_stream.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/la_parallel_vector.h>

#include <boost/range/irange.hpp>
#include <boost/range/iterator_range.hpp>

#include "helper.h"

namespace grendel
{
  using namespace dealii;


  template <int dim, typename Number>
  OfflineData<dim, Number>::OfflineData(
      const MPI_Comm &mpi_communicator,
      dealii::TimerOutput &computing_timer,
      const grendel::Discretization<dim> &discretization,
      const std::string &subsection /*= "OfflineData"*/)
      : ParameterAcceptor(subsection)
      , mpi_communicator_(mpi_communicator)
      , computing_timer_(computing_timer)
      , discretization_(&discretization)
  {
  }


  template <int dim, typename Number>
  void OfflineData<dim, Number>::setup()
  {
#ifdef DEBUG_OUTPUT
    deallog << "OfflineData<dim, Number>::setup()" << std::endl;
#endif

    IndexSet locally_owned;
    IndexSet locally_relevant;

    {
#ifdef DEBUG_OUTPUT
      deallog << "        distribute dofs" << std::endl;
#endif
      TimerOutput::Scope t(computing_timer_, "offline_data - distribute dofs");

      /* Initialize dof_handler and gather all locally owned indices: */

      dof_handler_.initialize(discretization_->triangulation(),
                              discretization_->finite_element());
      DoFRenumbering::Cuthill_McKee(dof_handler_);

      locally_owned = dof_handler_.locally_owned_dofs();
      n_locally_owned_ = locally_owned.n_elements();

      /*
       * Reorder indices:
       *
       * In order to traverse over multiple rows of a (to be constructed)
       * sparsity pattern simultaneously using SIMD instructions we reorder
       * all locally owned degrees of freedom to ensure that a local index
       * range [0, n_locally_internal_) \subset [0, n_locally_owned_) is
       * available that
       *
       *  - contains no boundary dof
       *
       *  - contains no foreign degree of freedom
       *
       *  - has "standard" connectivity, i.e. 2, 8, or 26 neighboring DoFs
       *    (in 1, 2, 3D).
       *
       *  - n_locally_owned_ is a multiple of n_array_elements
       */

      constexpr auto n_array_elements =
          VectorizedArray<Number>::n_array_elements;

      /* The locally owned index range has to be contiguous */

      Assert(locally_owned.is_contiguous() == true,
             ExcMessage("Need a contiguous set of locally owned indices."));

      /* Offset to translate from global to local index range */
      const auto offset = n_locally_owned_ != 0 ? *locally_owned.begin() : 0;

      const unsigned int dofs_per_cell =
          discretization_->finite_element().dofs_per_cell;
      std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

      /*
       * First pass: Accumulate how many cells are associated with a
       * given degree of freedom and mark all degrees of freedom shared
       * with a different number of cells than 2, 4, or 8 with
       * numbers::invalid_dof_index:
       */

      std::vector<types::global_dof_index> new_order(n_locally_owned_);

      for (auto cell : dof_handler_.active_cell_iterators()) {
        if (cell->is_artificial())
          continue;

        cell->get_dof_indices(local_dof_indices);

        for (unsigned int j = 0; j < dofs_per_cell; ++j) {
          const auto &index = local_dof_indices[j];
          if (!locally_owned.is_element(index))
            continue;

          Assert(index - offset < n_locally_owned_, ExcInternalError());
          new_order[index - offset] += 1;
        }
      }

      constexpr types::global_dof_index standard_number_of_neighbors =
          dim == 1 ? 2 : (dim == 2 ? 4 : 8);

      for (auto &it : new_order) {
        if (it == standard_number_of_neighbors)
          it = 0;
        else
          it = numbers::invalid_dof_index;
      }

      /* Second pass: Create renumbering. */

      types::global_dof_index index = offset;

      n_locally_internal_ = 0;
      for (auto &it : new_order)
        if (it != numbers::invalid_dof_index) {
          it = index++;
          n_locally_internal_++;
        }

      for (auto &it : new_order)
        if (it == numbers::invalid_dof_index)
          it = index++;

      dof_handler_.renumber_dofs(new_order);

      /* Round down to the nearest multiple of n_array_elements: */

      n_locally_internal_ =
          n_locally_internal_ - n_locally_internal_ % n_array_elements;

#ifndef USE_SIMD
      /*
       * If USE_SIMD is not set, we disable all SIMD instructions by
       * setting the [0, n_locally_internal) range to [0,0).
       */
      n_locally_internal_ = 0;
#endif
    }

    const auto dofs_per_cell = discretization_->finite_element().dofs_per_cell;

    {
#ifdef DEBUG_OUTPUT
      deallog << "        create partitioner and affine constraints"
              << std::endl;
#endif
      TimerOutput::Scope t(
          computing_timer_,
          "offline_data - create partitioner and affine constraints");

      /*
       * Create locally relevant index set:
       */

      locally_relevant.clear();

      DoFTools::extract_locally_relevant_dofs(dof_handler_, locally_relevant);
      n_locally_relevant_ = locally_relevant.n_elements();

      partitioner_.reset(new dealii::Utilities::MPI::Partitioner(
          locally_owned, locally_relevant, mpi_communicator_));

      /*
       * Create a temporary affine constraints object and populate it with
       * global indices:
       */

      AffineConstraints<Number> global_constraints(locally_relevant);


      if constexpr (dim != 1 && std::is_same<Number, double>::value) { // FIXME
        const auto n_periodic_faces =
            discretization_->triangulation().get_periodic_face_map().size();
        if (n_periodic_faces != 0) {
          /*
           * Enforce periodic boundary conditions. We assume that the mesh is
           * in "normal configuration". By convention we also omit enforcing
           * periodicity in x direction. This avoids accidentally glueing the
           * corner degrees of freedom together which leads to instability.
           */
          for (int i = 1; i < dim; ++i) /* omit x direction! */
            DoFTools::make_periodicity_constraints(dof_handler_,
                                                   /*b_id */ Boundary::periodic,
                                                   /*direction*/ i,
                                                   global_constraints);
        }
      }

      DoFTools::make_hanging_node_constraints(dof_handler_, global_constraints);

      global_constraints.close();

      /*
       * And translate into local indices:
       */

      affine_constraints_.clear();
      for (auto line : global_constraints.get_lines()) {

        /* translate into local index ranges: */
        line.index = partitioner_->global_to_local(line.index);
        std::transform(line.entries.begin(),
                       line.entries.end(),
                       line.entries.begin(),
                       [&](auto entry) {
                         return std::make_pair(
                             partitioner_->global_to_local(entry.first),
                             entry.second);
                       });

        affine_constraints_.add_line(line.index);
        affine_constraints_.add_entries(line.index, line.entries);
        affine_constraints_.set_inhomogeneity(line.index, line.inhomogeneity);
      }
      affine_constraints_.close();
    }

    /*
     * We need a local view of a couple of matrices. Because they are never
     * used in a matrix-vector product, and in order to avoid unnecessary
     * overhead, we simply assemble these parts into a local
     * dealii::SparseMatrix<dim>.
     *
     * These sparse matrices have to store values for all _locally
     * extended_ degrees of freedom that couple. While we are at it, we
     * also apply a translation between the global (distributed) degrees of
     * freedom numbering and the local index range [0,
     * locally_relevant_.n_elements()];
     */

    {
#ifdef DEBUG_OUTPUT
      deallog << "        create_sparsity_pattern" << std::endl;
#endif
      TimerOutput::Scope t(computing_timer_,
                           "offline_data - create sparsity pattern");

      DynamicSparsityPattern dsp(n_locally_relevant_, n_locally_relevant_);

      std::vector<types::global_dof_index> dof_indices(dofs_per_cell);

      for (auto cell : dof_handler_.active_cell_iterators()) {
        /* iterate over locally owned cells and the ghost layer */
        if (cell->is_artificial())
          continue;

        /* translate into local index ranges: */
        cell->get_dof_indices(dof_indices);
        std::transform(
            dof_indices.begin(),
            dof_indices.end(),
            dof_indices.begin(),
            [&](auto index) { return partitioner_->global_to_local(index); });

        affine_constraints_.add_entries_local_to_global(
            dof_indices, dsp, false);
      }

      sparsity_pattern_assembly_.copy_from(dsp);

      /*
       * Create a sparsity pattern where the only off-processor rows
       * are the one for which locally owned rows request the
       * transpose entries. This will be the one we finally compute on.
       */

      DynamicSparsityPattern dsp_minimal(n_locally_relevant_,
                                         n_locally_relevant_);
      const unsigned int n_owned_dofs = dof_handler_.n_locally_owned_dofs();
      for (unsigned int i = 0; i < n_owned_dofs; ++i) {
        for (auto it = dsp.begin(i); it != dsp.end(i); ++it) {
          const unsigned int col = it->column();
          dsp_minimal.add(i, col);
          if (col >= n_owned_dofs) {
            dsp_minimal.add(col, i);
          }
        }
      }

      sparsity_pattern_.copy_from(dsp_minimal);
    }

    /*
     * Next we can (re)initialize all local matrices:
     */

    {
#ifdef DEBUG_OUTPUT
      deallog << "        set up matrices" << std::endl;
#endif
      TimerOutput::Scope t(computing_timer_, "offline_data - set up matrices");

      lumped_mass_matrix_.reinit(partitioner_);
      lumped_mass_matrix_inverse_.reinit(partitioner_);

      sparsity_pattern_simd_.reinit(
          n_locally_internal_, sparsity_pattern_, *partitioner_);

      mass_matrix_.reinit(sparsity_pattern_simd_);
      betaij_matrix_.reinit(sparsity_pattern_simd_);
      cij_matrix_.reinit(sparsity_pattern_simd_);
    }
  }


  template <int dim, typename Number>
  void OfflineData<dim, Number>::assemble()
  {
#ifdef DEBUG_OUTPUT
    deallog << "OfflineData<dim, Number>::assemble()" << std::endl;
#endif

    dealii::SparseMatrix<Number> mass_matrix_tmp;
    mass_matrix_tmp.reinit(sparsity_pattern_assembly_);
    std::array<dealii::SparseMatrix<Number>, dim> cij_matrix_tmp;
    for (auto &matrix : cij_matrix_tmp)
      matrix.reinit(sparsity_pattern_assembly_);
    dealii::SparseMatrix<Number> betaij_matrix_tmp;
    betaij_matrix_tmp.reinit(sparsity_pattern_assembly_);
    measure_of_omega_ = 0.;

    boundary_normal_map_.clear();

    const unsigned int dofs_per_cell =
        discretization_->finite_element().dofs_per_cell;

    const unsigned int n_q_points = discretization_->quadrature().size();

    /*
     * First pass: Assemble all matrices:
     */

    /* The local, per-cell assembly routine: */

    const auto local_assemble_system = [&](const auto &cell,
                                           auto &scratch,
                                           auto &copy) {
      /* iterate over locally owned cells and the ghost layer */

      auto &is_artificial = copy.is_artificial_;
      auto &local_dof_indices = copy.local_dof_indices_;

      auto &local_boundary_normal_map = copy.local_boundary_normal_map_;
      auto &cell_mass_matrix = copy.cell_mass_matrix_;
      auto &cell_betaij_matrix = copy.cell_betaij_matrix_;
      auto &cell_cij_matrix = copy.cell_cij_matrix_;
      auto &cell_measure = copy.cell_measure_;

      auto &fe_values = scratch.fe_values_;
      auto &fe_face_values = scratch.fe_face_values_;

      is_artificial = cell->is_artificial();
      if (is_artificial)
        return;

      cell_mass_matrix.reinit(dofs_per_cell, dofs_per_cell);
      cell_betaij_matrix.reinit(dofs_per_cell, dofs_per_cell);
      for (auto &matrix : cell_cij_matrix)
        matrix.reinit(dofs_per_cell, dofs_per_cell);

      fe_values.reinit(cell);

      local_dof_indices.resize(dofs_per_cell);
      cell->get_dof_indices(local_dof_indices);

      /* translate into local index ranges: */
      std::transform(
          local_dof_indices.begin(),
          local_dof_indices.end(),
          local_dof_indices.begin(),
          [&](auto index) { return partitioner_->global_to_local(index); });

      /* clear out copy data: */
      local_boundary_normal_map.clear();
      cell_mass_matrix = 0.;
      cell_betaij_matrix = 0.;
      for (auto &matrix : cell_cij_matrix)
        matrix = 0.;
      cell_measure = 0.;

      for (unsigned int q_point = 0; q_point < n_q_points; ++q_point) {
        const auto JxW = fe_values.JxW(q_point);

        if (cell->is_locally_owned())
          cell_measure += Number(JxW);

        for (unsigned int j = 0; j < dofs_per_cell; ++j) {

          const auto value_JxW = fe_values.shape_value(j, q_point) * JxW;
          const auto grad_JxW = fe_values.shape_grad(j, q_point) * JxW;

          for (unsigned int i = 0; i < dofs_per_cell; ++i) {

            const auto value = fe_values.shape_value(i, q_point);
            const auto grad = fe_values.shape_grad(i, q_point);

            cell_mass_matrix(i, j) += Number(value * value_JxW);

            cell_betaij_matrix(i, j) += Number(grad * grad_JxW);

            for (unsigned int d = 0; d < dim; ++d)
              cell_cij_matrix[d](i, j) += Number((value * grad_JxW)[d]);

          } /* for i */
        }   /* for j */
      }     /* for q */

      for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f) {
        const auto face = cell->face(f);
        const auto id = face->boundary_id();

        if (!face->at_boundary())
          continue;

        fe_face_values.reinit(cell, f);
        const unsigned int n_face_q_points = scratch.face_quadrature_.size();

        for (unsigned int j = 0; j < dofs_per_cell; ++j) {

          if (!discretization_->finite_element().has_support_on_face(j, f))
            continue;

          dealii::Tensor<1, dim, Number> normal;
          if (id == Boundary::slip) {
            /*
             * Only accumulate a normal if the boundary indicator is for
             * slip boundary conditions. Otherwise we create a wrong normal
             * in corners of the computational domain.
             */
            for (unsigned int q = 0; q < n_face_q_points; ++q)
              normal += fe_face_values.normal_vector(q) *
                        fe_face_values.shape_value(j, q);
          }

          const auto index = local_dof_indices[j];

          // FIXME: This is a bloody hack:
          Point<dim> position;
          const auto global_index = partitioner_->local_to_global(index);
          for (unsigned int v = 0; v < GeometryInfo<dim>::vertices_per_cell;
               ++v)
            if (cell->vertex_dof_index(v, 0) == global_index)
              position = cell->vertex(v);

          /*
           * Ensure that we record the highest boundary indicator for a
           * given degree of freedom (higher indicators take precedence):
           */
          const auto &[old_normal, old_id, _] =
              local_boundary_normal_map[index];
          local_boundary_normal_map[index] = std::make_tuple(
              old_normal + normal, std::max(old_id, id), position);
        } /* j */
      }   /* f */
    };

    const auto copy_local_to_global = [&](const auto &copy) {
      const auto &is_artificial = copy.is_artificial_;
      const auto &local_dof_indices = copy.local_dof_indices_;
      const auto &local_boundary_normal_map = copy.local_boundary_normal_map_;
      const auto &cell_mass_matrix = copy.cell_mass_matrix_;
      const auto &cell_cij_matrix = copy.cell_cij_matrix_;
      const auto &cell_betaij_matrix = copy.cell_betaij_matrix_;
      const auto &cell_measure = copy.cell_measure_;

      if (is_artificial)
        return;

      for (const auto &it : local_boundary_normal_map) {
        auto &[normal, id, position] = boundary_normal_map_[it.first];
        auto &[new_normal, new_id, new_position] = it.second;

        normal += new_normal;
        /*
         * Ensure that we record the highest boundary indicator for a given
         * degree of freedom (higher indicators take precedence):
         */
        id = std::max(id, new_id);
        position = new_position;
      }

      affine_constraints_.distribute_local_to_global(
          cell_mass_matrix, local_dof_indices, mass_matrix_tmp);

      for (int k = 0; k < dim; ++k) {
        affine_constraints_.distribute_local_to_global(
            cell_cij_matrix[k], local_dof_indices, cij_matrix_tmp[k]);
      }

      affine_constraints_.distribute_local_to_global(
          cell_betaij_matrix, local_dof_indices, betaij_matrix_tmp);

      measure_of_omega_ += cell_measure;
    };


    {
#ifdef DEBUG_OUTPUT
      deallog << "        assemble mass matrices, beta_ij, and c_ijs"
              << std::endl;
#endif
      TimerOutput::Scope t(
          computing_timer_,
          "offline_data - assemble mass matrices, beta_ij, and c_ij");

      WorkStream::run(dof_handler_.begin_active(),
                      dof_handler_.end(),
                      local_assemble_system,
                      copy_local_to_global,
                      AssemblyScratchData<dim>(*discretization_),
                      AssemblyCopyData<dim, Number>());
    }

    measure_of_omega_ =
        Utilities::MPI::sum(measure_of_omega_, mpi_communicator_);

    /*
     * Second part: We have to import the "ghost" layer of the lumped mass
     * matrix in order to compute the b_ij matrices correctly.
     */
    Vector<Number> one(mass_matrix_tmp.m());
    one = 1;
    Vector<Number> local_lumped_mass_matrix(mass_matrix_tmp.m());
    mass_matrix_tmp.vmult(local_lumped_mass_matrix, one);
    for (unsigned int i = 0; i < partitioner_->local_size(); ++i) {
      lumped_mass_matrix_.local_element(i) = local_lumped_mass_matrix(i);
      lumped_mass_matrix_inverse_.local_element(i) =
          1. / lumped_mass_matrix_.local_element(i);
    }
    lumped_mass_matrix_.update_ghost_values();
    lumped_mass_matrix_inverse_.update_ghost_values();


    /*
     * Second pass: Fix up boundary cijs:
     */

    /* The local, per-cell assembly routine: */

    const auto local_assemble_system_cij = [&](const auto &cell,
                                               auto &scratch,
                                               auto &copy) {
      /* iterate over locally owned cells and the ghost layer */

      auto &is_artificial = copy.is_artificial_;
      auto &local_dof_indices = copy.local_dof_indices_;

      auto &cell_cij_matrix = copy.cell_cij_matrix_;

      auto &fe_face_values = scratch.fe_face_values_;

      is_artificial = cell->is_artificial();
      if (is_artificial)
        return;

      for (auto &matrix : cell_cij_matrix)
        matrix.reinit(dofs_per_cell, dofs_per_cell);

      local_dof_indices.resize(dofs_per_cell);
      cell->get_dof_indices(local_dof_indices);
      std::transform(
          local_dof_indices.begin(),
          local_dof_indices.end(),
          local_dof_indices.begin(),
          [&](auto index) { return partitioner_->global_to_local(index); });

      /* clear out copy data: */
      for (auto &matrix : cell_cij_matrix)
        matrix = 0.;

      for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f) {
        const auto face = cell->face(f);
        const auto id = face->boundary_id();

        if (!face->at_boundary())
          continue;

        if (id != Boundary::slip)
          continue;

        fe_face_values.reinit(cell, f);
        const unsigned int n_face_q_points = scratch.face_quadrature_.size();

        for (unsigned int q = 0; q < n_face_q_points; ++q) {

          const auto JxW = fe_face_values.JxW(q);
          const auto normal_q = fe_face_values.normal_vector(q);

          for (unsigned int j = 0; j < dofs_per_cell; ++j) {
            if (!discretization_->finite_element().has_support_on_face(j, f))
              continue;

            const auto &[normal_j, _1, _2] =
                boundary_normal_map_[local_dof_indices[j]];

            const auto value_JxW = fe_face_values.shape_value(j, q) * JxW;

            for (unsigned int i = 0; i < dofs_per_cell; ++i) {
              const auto value = fe_face_values.shape_value(i, q);

              for (unsigned int d = 0; d < dim; ++d)
                cell_cij_matrix[d](i, j) +=
                    Number((normal_j[d] - normal_q[d]) * (value * value_JxW));
            } /* i */
          }   /* j */
        }     /* q */
      }       /* f */
    };

    const auto copy_local_to_global_cij = [&](const auto &copy) {
      const auto &is_artificial = copy.is_artificial_;
      const auto &local_dof_indices = copy.local_dof_indices_;
      const auto &cell_cij_matrix = copy.cell_cij_matrix_;

      if (is_artificial)
        return;

      for (int k = 0; k < dim; ++k) {
        affine_constraints_.distribute_local_to_global(
            cell_cij_matrix[k], local_dof_indices, cij_matrix_tmp[k]);
      }
    };

    {
#ifdef DEBUG_OUTPUT
      deallog << "        fix boundary c_ijs" << std::endl;
#endif
      TimerOutput::Scope t(computing_timer_,
                           "offline_data - fix boundary c_ijs");

      /*
       * Normalize our boundary normals:
       */
      for (auto &it : boundary_normal_map_) {
        auto &[normal, id, _] = it.second;
        normal /= (normal.norm() + std::numeric_limits<Number>::epsilon());
      }

      WorkStream::run(dof_handler_.begin_active(),
                      dof_handler_.end(),
                      local_assemble_system_cij,
                      copy_local_to_global_cij,
                      AssemblyScratchData<dim>(*discretization_),
                      AssemblyCopyData<dim, Number>());
    }

    {
#ifdef DEBUG_OUTPUT
      deallog << "        set up SIMD matrices" << std::endl;
#endif
      TimerOutput::Scope t(computing_timer_,
                           "offline_data - set up SIMD matrices");

      betaij_matrix_.read_in(betaij_matrix_tmp);
      mass_matrix_.read_in(mass_matrix_tmp);
      cij_matrix_.read_in(cij_matrix_tmp);
    }
  }

} /* namespace grendel */

#endif /* OFFLINE_DATA_TEMPLATE_H */
