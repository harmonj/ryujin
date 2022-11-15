//
// SPDX-License-Identifier: MIT
// Copyright (C) 2020 - 2022 by the ryujin authors
//

#pragma once

namespace ryujin
{
  namespace EulerAEOS
  {
    namespace EquationOfStateLibrary
    {
      /**
       * The polytropic gas equation of state
       *
       * @ingroup EquationOfState
       */
      class PolytropicGas : public EquationOfState
      {
      public:
        PolytropicGas(const std::string subsection)
            : EquationOfState("polytropic gas", subsection)
        {
          gamma_ = 7. / 5.;
          this->add_parameter("gamma", gamma_, "The ratio of specific heats");
        }


        virtual double pressure(const double /*rho*/,
                                const double internal_energy) final override
        {
          /*
           * p = (\gamma - 1) * \rho * e
           */

          return (gamma_ - 1.) * internal_energy;
        }


        virtual double
        specific_internal_energy(const double rho,
                                 const double pressure) final override
        {
          const double denom = rho * (gamma_ - 1.);
          return pressure / denom;
        }

      private:
        double gamma_;
      };
    } // namespace EquationOfStateLibrary
  }   // namespace EulerAEOS
} // namespace ryujin