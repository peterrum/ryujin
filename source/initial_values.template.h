//
// SPDX-License-Identifier: MIT
// Copyright (C) 2020 - 2021 by the ryujin authors
//

#pragma once

#include "initial_state.template.h"
#include "initial_values.h"
#include "simd.h"

#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/vector_tools.templates.h>

#include <random>

namespace ryujin
{
  using namespace dealii;

  template <int dim, typename Number>
  InitialValues<dim, Number>::InitialValues(
      const ProblemDescription &problem_description,
      const std::string &subsection)
      : ParameterAcceptor(subsection)
      , problem_description(problem_description)
  {
    ParameterAcceptor::parse_parameters_call_back.connect(std::bind(
        &InitialValues<dim, Number>::parse_parameters_callback, this));

    configuration_ = "uniform";
    add_parameter("configuration",
                  configuration_,
                  "The initial state configuration. Valid names are given by "
                  "any of the subsections defined below.");

    initial_direction_[0] = 1.;
    add_parameter(
        "direction",
        initial_direction_,
        "Initial direction of shock front, contrast, or vortex");

    initial_position_[0] = 1.;
    add_parameter(
        "position",
        initial_position_,
        "Initial position of shock front, contrast, or vortex");

    perturbation_ = 0.;
    add_parameter("perturbation",
                  perturbation_,
                  "Add a random perturbation of the specified magnitude to the "
                  "initial state.");

    using namespace InitialStates;
    initial_state_list_.emplace(std::make_unique<Uniform<dim, Number>>(
        problem_description, subsection));
    initial_state_list_.emplace(std::make_unique<RampUp<dim, Number>>(
        problem_description, subsection));
    initial_state_list_.emplace(std::make_unique<Contrast<dim, Number>>(
        problem_description, subsection));
    initial_state_list_.emplace(std::make_unique<ShockFront<dim, Number>>(
        problem_description, subsection));
    initial_state_list_.emplace(std::make_unique<IsentropicVortex<dim, Number>>(
        problem_description, subsection));
    initial_state_list_.emplace(std::make_unique<BeckerSolution<dim, Number>>(
        problem_description, subsection));
  }

  namespace
  {
    /**
     * An affine transformation:
     */
    template <int dim>
    inline DEAL_II_ALWAYS_INLINE dealii::Point<dim>
    affine_transform(const dealii::Tensor<1, dim> initial_direction,
                     const dealii::Point<dim> initial_position,
                     const dealii::Point<dim> x)
    {
      auto direction = x - initial_position;

      /* Roll third component of initial_direction onto xy-plane: */
      if constexpr (dim == 3) {
        auto n_x = initial_direction[0];
        auto n_z = initial_direction[2];
        const auto norm = std::sqrt(n_x * n_x + n_z * n_z);
        n_x /= norm;
        n_z /= norm;
        auto new_direction = direction;
        if (norm > 1.0e-14) {
          new_direction[0] = n_x * direction[0] + n_z * direction[2];
          new_direction[2] = -n_z * direction[0] + n_x * direction[2];
        }
        direction = new_direction;
      }

      /* Roll second component of initial_direction onto x-axis: */
      {
        auto n_x = initial_direction[0];
        auto n_y = initial_direction[1];
        const auto norm = std::sqrt(n_x * n_x + n_y * n_y);
        n_x /= norm;
        n_y /= norm;
        auto new_direction = direction;
        if (norm > 1.0e-14) {
          new_direction[0] = n_x * direction[0] + n_y * direction[1];
          new_direction[1] = -n_y * direction[0] + n_x * direction[1];
        }
        direction = new_direction;
      }

      return Point<dim>() + direction;
    }


    /**
     * Transform vector:
     */
    template <int dim, typename Number>
    inline DEAL_II_ALWAYS_INLINE dealii::Tensor<1, dim, Number>
    affine_transform_vector(const dealii::Tensor<1, dim> initial_direction,
                            dealii::Tensor<1, dim, Number> direction)
    {
      {
        auto n_x = initial_direction[0];
        auto n_y = initial_direction[1];
        const auto norm = std::sqrt(n_x * n_x + n_y * n_y);
        n_x /= norm;
        n_y /= norm;
        auto new_direction = direction;
        if (norm > 1.0e-14) {
          new_direction[0] = n_x * direction[0] - n_y * direction[1];
          new_direction[1] = n_y * direction[0] + n_x * direction[1];
        }
        direction = new_direction;
      }

      if constexpr (dim == 3) {
        auto n_x = initial_direction[0];
        auto n_z = initial_direction[2];
        const auto norm = std::sqrt(n_x * n_x + n_z * n_z);
        n_x /= norm;
        n_z /= norm;
        auto new_direction = direction;
        if (norm > 1.0e-14) {
          new_direction[0] = n_x * direction[0] - n_z * direction[2];
          new_direction[2] = n_z * direction[0] + n_x * direction[2];
        }
        direction = new_direction;
      }

      return direction;
    }
  } /* namespace */


  template <int dim, typename Number>
  void InitialValues<dim, Number>::parse_parameters_callback()
  {
    constexpr auto problem_dimension =
        ProblemDescription::problem_dimension<dim>;

    /* First, let's normalize the direction: */

    AssertThrow(
        initial_direction_.norm() != 0.,
        ExcMessage("Initial shock front direction is set to the zero vector."));
    initial_direction_ /= initial_direction_.norm();

    /* Populate std::function object: */

    {
      bool initialized = false;
      for (auto &it : initial_state_list_)
        if (it->name() == configuration_) {
          initial_state_ = [this, &it](const dealii::Point<dim> &point,
                                       Number t) {
            const auto transformed_point =
                affine_transform(initial_direction_, initial_position_, point);
            auto state = it->compute(transformed_point, t);
            auto M = problem_description.momentum(state);
            M = affine_transform_vector(initial_direction_, M);
            for (unsigned int d = 0; d < dim; ++d)
              state[1 + d] = M[d];
            return state;
          };
          initialized = true;
          break;
        }

      AssertThrow(
          initialized,
          ExcMessage(
              "Could not find an initial state description with name \"" +
              configuration_ + "\""));
    }

    /* Add a random perturbation to the original function object: */

    if (perturbation_ != 0.) {
      initial_state_ = [old_state = this->initial_state_,
                        perturbation = this->perturbation_](
                           const dealii::Point<dim> &point, Number t) {
        static std::default_random_engine generator;
        static std::uniform_real_distribution<Number> distribution(-1., 1.);
        auto draw = std::bind(distribution, generator);

        auto state = old_state(point, t);
        for (unsigned int i = 0; i < problem_dimension; ++i)
          state[i] *= (Number(1.) + perturbation * draw());

        return state;
      };
    }
  }


  template <int dim, typename Number>
  typename InitialValues<dim, Number>::vector_type
  InitialValues<dim, Number>::interpolate(
      const OfflineData<dim, Number> &offline_data, Number t)
  {
#ifdef DEBUG_OUTPUT
    std::cout << "InitialValues<dim, Number>::interpolate(t = " << t << ")"
              << std::endl;
#endif

    vector_type U;
    U.reinit(offline_data.vector_partitioner());

    constexpr auto problem_dimension =
        ProblemDescription::problem_dimension<dim>;

    using scalar_type = typename OfflineData<dim, Number>::scalar_type;

    const auto callable = [&](const auto &p) { return initial_state(p, t); };

    scalar_type temp;
    const auto scalar_partitioner = offline_data.scalar_partitioner();
    temp.reinit(scalar_partitioner);

    for (unsigned int d = 0; d < problem_dimension; ++d) {
      VectorTools::interpolate(offline_data.dof_handler(),
                               to_function<dim, Number>(callable, d),
                               temp);
      U.insert_component(temp, d);
    }

    const auto &boundary_map = offline_data.boundary_map();
    const unsigned int n_owned = offline_data.n_locally_owned();

    /*
     * Cosmetic fix up: Ensure that the initial state is compatible with
     * slip and no_slip boundary conditions. This ensures that nothing is
     * ever transported out of slip and no slip boundaries - even if
     * initial conditions happen to be set incorrectly.
     */

    for (auto entry : boundary_map) {
      const auto i = entry.first;
      if (i >= n_owned)
        continue;

      const auto &[normal, id, position] = entry.second;

      if (id == Boundary::slip) {
        /* Remove normal component of velocity: */
        auto U_i = U.get_tensor(i);
        auto m = problem_description.momentum(U_i);
        m -= 1. * (m * normal) * normal;
        for (unsigned int k = 0; k < dim; ++k)
          U_i[k + 1] = m[k];
        U.write_tensor(U_i, i);

      } else if (id == Boundary::no_slip) {

        /* Set velocity to zero: */
        auto U_i = U.get_tensor(i);
        for (unsigned int k = 0; k < dim; ++k)
          U_i[k + 1] = Number(0.);
        U.write_tensor(U_i, i);
      }
    }

    U.update_ghost_values();
    return U;
  }

} /* namespace ryujin */
