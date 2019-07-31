#ifndef SCHLIEREN_POSTPROCESSOR_TEMPLATE_H
#define SCHLIEREN_POSTPROCESSOR_TEMPLATE_H

#include "helper.h"
#include "schlieren_postprocessor.h"

#include <boost/range/irange.hpp>

#include <atomic>

namespace grendel
{
  using namespace dealii;


  template <int dim>
  SchlierenPostprocessor<dim>::SchlierenPostprocessor(
      const MPI_Comm &mpi_communicator,
      dealii::TimerOutput &computing_timer,
      const grendel::OfflineData<dim> &offline_data,
      const std::string &subsection /*= "SchlierenPostprocessor"*/)
      : ParameterAcceptor(subsection)
      , mpi_communicator_(mpi_communicator)
      , computing_timer_(computing_timer)
      , offline_data_(&offline_data)
  {
    schlieren_beta_ = 10.;
    add_parameter("schlieren beta",
                  schlieren_beta_,
                  "Beta factor used in Schlieren-type postprocessor");

    schlieren_index_ = 0;
    add_parameter("schlieren index",
                  schlieren_index_,
                  "Use the corresponding component of the state vector for the "
                  "schlieren plot");
  }


  template <int dim>
  void SchlierenPostprocessor<dim>::prepare()
  {
    deallog << "SchlierenPostprocessor<dim>::prepare()" << std::endl;
    TimerOutput::Scope t(computing_timer_,
                         "schlieren_postprocessor - prepare scratch space");

    const auto &locally_owned = offline_data_->locally_owned();
    const auto &locally_extended = offline_data_->locally_extended();

    r_i_.reinit(locally_extended.n_elements());
    schlieren_.reinit(locally_owned, locally_extended, mpi_communicator_);
  }


  template <int dim>
  void SchlierenPostprocessor<dim>::compute_schlieren(const vector_type &U)
  {
    deallog << "SchlierenPostprocessor<dim>::compute_schlieren()" << std::endl;

    TimerOutput::Scope t(computing_timer_,
                         "schlieren_postprocessor - compute schlieren plot");

    const auto &locally_extended = offline_data_->locally_extended();
    const auto &locally_owned = offline_data_->locally_owned();
    const auto &sparsity = offline_data_->sparsity_pattern();
    const auto &lumped_mass_matrix = offline_data_->lumped_mass_matrix();
    const auto &cij_matrix = offline_data_->cij_matrix();
    const auto &boundary_normal_map = offline_data_->boundary_normal_map();

    const auto indices =
        boost::irange<unsigned int>(0, locally_extended.n_elements());

    /*
     * Step 1: Compute r_i and r_i_max, r_i_min:
     */

    std::atomic<double> r_i_max{0.};
    std::atomic<double> r_i_min{std::numeric_limits<double>::infinity()};

    {
      const auto on_subranges = [&](auto i1, const auto i2) {
        double r_i_max_on_subrange = 0.;
        double r_i_min_on_subrange = std::numeric_limits<double>::infinity();

        /* Translate the local index into a index set iterator:: */
        auto it_global =
            locally_extended.at(locally_extended.nth_index_in_set(*i1));

        for (; i1 < i2; ++i1, ++it_global) {
          const auto i = *i1;
          const auto i_global = *it_global;

          /* Skip constrained degrees of freedom */
          if (++sparsity.begin(i) == sparsity.end(i))
            continue;

          /* Only iterate over locally owned subset */
          if (!locally_owned.is_element(i_global))
            continue;

          Tensor<1, dim> r_i;

          for (auto jt = sparsity.begin(i); jt != sparsity.end(i); ++jt) {
            const auto j = jt->column();
            const auto j_global = locally_extended.nth_index_in_set(j);

            if (i == j)
              continue;

            const auto U_js = U[schlieren_index_][j_global];
            const auto c_ij = gather_get_entry(cij_matrix, jt);

            r_i += c_ij * U_js;
          }

          /* Fix up boundaries: */
          const auto bnm_it = boundary_normal_map.find(i);
          if (bnm_it != boundary_normal_map.end()) {
            const auto [normal, id, _] = bnm_it->second;
            if (id == Boundary::slip) {
              r_i -= 1. * (r_i * normal) * normal;
            } else {
              /*
               * FIXME: This is not particularly elegant. On all other
               * boundary types, we simply set r_i to zero.
               */
              r_i = 0.;
            }
          }

          const double m_i = lumped_mass_matrix.diag_element(i);
          r_i_[i] = r_i.norm() / m_i;

          r_i_max_on_subrange = std::max(r_i_max_on_subrange, r_i_[i]);
          r_i_min_on_subrange = std::min(r_i_min_on_subrange, r_i_[i]);
        }

        /* Synchronize over all threads: */

        double current_r_i_max = r_i_max.load();
        while (current_r_i_max < r_i_max_on_subrange &&
               !r_i_max.compare_exchange_weak(current_r_i_max,
                                              r_i_max_on_subrange))
          ;

        double current_r_i_min = r_i_min.load();
        while (current_r_i_min > r_i_min_on_subrange &&
               !r_i_min.compare_exchange_weak(current_r_i_min,
                                              r_i_min_on_subrange))
          ;
      };

      parallel::apply_to_subranges(
          indices.begin(), indices.end(), on_subranges, 4096);
    }

    /* And synchronize over all processors: */

    r_i_max.store(Utilities::MPI::max(r_i_max.load(), mpi_communicator_));
    r_i_min.store(Utilities::MPI::min(r_i_min.load(), mpi_communicator_));

    /*
     * Step 2: Compute schlieren:
     */

    {
      const auto on_subranges = [&](auto i1, const auto i2) {

        /* Translate the local index into a index set iterator:: */
        auto it_global =
            locally_extended.at(locally_extended.nth_index_in_set(*i1));

        for (; i1 < i2; ++i1, ++it_global) {
          const auto i = *i1;
          const auto i_global = *it_global;

          /* Skip constrained degrees of freedom */
          if (++sparsity.begin(i) == sparsity.end(i))
            continue;

          const auto r_i = r_i_[i];
          schlieren_[i_global] =
              1. - std::exp(-schlieren_beta_ * (r_i - r_i_min) /
                            (r_i_max - r_i_min));
        }
      };

      parallel::apply_to_subranges(
          indices.begin(), indices.end(), on_subranges, 4096);

      /* Fix up hanging nodes: */
      const auto &affine_constraints = offline_data_->affine_constraints();
      affine_constraints.distribute(schlieren_);
    }

    schlieren_.update_ghost_values();
  }

} /* namespace grendel */

#endif /* SCHLIEREN_POSTPROCESSOR_TEMPLATE_H */
