//
// SPDX-License-Identifier: MIT
// Copyright (C) 2020 - 2022 by the ryujin authors
//

#pragma once

#include <compile_time_options.h>

#include "convenience_macros.h"

#include <deal.II/base/parameter_acceptor.h>
#include <deal.II/base/tensor.h>

#include <string>

namespace ryujin
{
  namespace EulerAEOS
  {
    /**
     * A small abstract base class to group configuration options for an
     * equation of state.
     *
     * @ingroup EquationOfState
     */
    class EquationOfState : public dealii::ParameterAcceptor
    {
    public:
      /**
       * Constructor taking EOS name @p name and a subsection @p subsection
       * as an argument. The dealii::ParameterAcceptor is initialized with
       * the subsubsection `subsection + "/" + name`.
       */
      EquationOfState(const std::string &name, const std::string &subsection)
          : ParameterAcceptor(subsection + "/" + name)
          , name_(name)
      {
      }

      /**
       * Return the pressure given density (rho) and internal energy (rho *
       * e).
       */
      virtual double pressure(const double density,
                              const double internal_energy) = 0;

      /**
       * Return the specific internal energy (e) for a given density (rho)
       * and pressure (p).
       */
      virtual double specific_internal_energy(const double density,
                                              const double pressure) = 0;

    private:
      const std::string name_;

      /**
       * Return the name of the EOS as (const reference) std::string
       */
      ACCESSOR_READ_ONLY(name)
    };
  } // namespace EulerAEOS

  using namespace EulerAEOS;
} /* namespace ryujin */
