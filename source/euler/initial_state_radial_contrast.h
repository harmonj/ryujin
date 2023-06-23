//
// SPDX-License-Identifier: MIT
// Copyright (C) 2020 - 2022 by the ryujin authors
//

#pragma once

#include "hyperbolic_system.h"
#include <initial_state.h>

namespace ryujin
{
  namespace Euler
  {
    /**
     * A slight modification of the "contrast" initial state. Now, we have
     * an initial state formed by a contrast of a given "left" and "right"
     * primitive state where the "left" state is "inside" the radius, R, and the
     * "right" state is outside.
     *
     * @note This class does not evolve a possible shock front in time. If
     * you need correct time-dependent Dirichlet data use @ref ShockFront
     * instead.
     *
     * @ingroup EulerEquations
     */
    template <int dim, typename Number, typename state_type>
    class RadialContrast : public InitialState<dim, Number, state_type>
    {
    public:
      RadialContrast(const HyperbolicSystem &hyperbolic_system,
                     const std::string subsection)
          : InitialState<dim, Number, state_type>("radial contrast", subsection)
          , hyperbolic_system(hyperbolic_system)
      {
        primitive_left_[0] = hyperbolic_system.gamma();
        primitive_left_[1] = 0.0;
        primitive_left_[2] = 1.;
        this->add_parameter(
            "primitive state left",
            primitive_left_,
            "Initial 1d primitive state (rho, u, p) on the left");

        primitive_right_[0] = hyperbolic_system.gamma();
        primitive_right_[1] = 0.0;
        primitive_right_[2] = 1.;
        this->add_parameter(
            "primitive state right",
            primitive_right_,
            "Initial 1d primitive state (rho, u, p) on the right");

        radius_ = 0.5;
        this->add_parameter("radius", radius_, "Radius of radial area");
      }

      state_type compute(const dealii::Point<dim> &point, Number /*t*/) final
      {
        const auto temp = hyperbolic_system.from_primitive_state(
            point.norm() > radius_ ? primitive_right_ : primitive_left_);
        return hyperbolic_system.template expand_state<dim>(temp);
      }

    private:
      const HyperbolicSystem &hyperbolic_system;

      dealii::Tensor<1, 3, Number> primitive_left_;
      dealii::Tensor<1, 3, Number> primitive_right_;
      double radius_;
    };
  } // namespace Euler
} // namespace ryujin