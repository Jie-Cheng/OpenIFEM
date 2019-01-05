#include "mpi_shared_linear_elasticity.h"

namespace Solid
{
  namespace MPI
  {
    using namespace dealii;

    template <int dim>
    SharedLinearElasticity<dim>::SharedLinearElasticity(
      Triangulation<dim> &tria, const Parameters::AllParameters &parameters)
      : SharedSolidSolver<dim>(tria, parameters)
    {
      material.resize(parameters.n_solid_parts, LinearElasticMaterial<dim>());
      for (unsigned int i = 0; i < parameters.n_solid_parts; ++i)
        {
          LinearElasticMaterial<dim> tmp(
            parameters.E[i], parameters.nu[i], parameters.solid_rho);
          material[i] = tmp;
        }
    }

    template <int dim>
    void SharedLinearElasticity<dim>::assemble_system(const bool is_initial)
    {
      TimerOutput::Scope timer_section(timer, "Assemble system");

      double gamma = 0.5 + parameters.damping;
      double beta = gamma / 2;

      system_matrix = 0;
      stiffness_matrix = 0;
      system_rhs = 0;

      FEValues<dim> fe_values(fe,
                              volume_quad_formula,
                              update_values | update_gradients |
                                update_quadrature_points | update_JxW_values);

      FEFaceValues<dim> fe_face_values(
        fe,
        face_quad_formula,
        update_values | update_quadrature_points | update_normal_vectors |
          update_JxW_values);

      SymmetricTensor<4, dim> elasticity;
      const double rho = material[0].get_density();
      const double dt = time.get_delta_t();

      const unsigned int dofs_per_cell = fe.dofs_per_cell;
      const unsigned int n_q_points = volume_quad_formula.size();
      const unsigned int n_f_q_points = face_quad_formula.size();

      FullMatrix<double> local_matrix(dofs_per_cell, dofs_per_cell);
      FullMatrix<double> local_stiffness(dofs_per_cell, dofs_per_cell);
      Vector<double> local_rhs(dofs_per_cell);

      std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

      // The symmetric gradients of the displacement shape functions at a
      // certain point. There are dofs_per_cell shape functions so the size is
      // dofs_per_cell.
      std::vector<SymmetricTensor<2, dim>> symmetric_grad_phi(dofs_per_cell);
      // The shape functions at a certain point.
      std::vector<Tensor<1, dim>> phi(dofs_per_cell);
      // A "viewer" to describe the nodal dofs as a vector.
      FEValuesExtractors::Vector displacements(0);

      // Loop over cells
      for (auto cell = dof_handler.begin_active(); cell != dof_handler.end();
           ++cell)
        {
          // Only operates on the locally owned cells
          if (cell->subdomain_id() == this_mpi_process)
            {
              auto p = cell_property.get_data(cell);
              int mat_id = cell->material_id();
              if (material.size() == 1)
                mat_id = 1;
              elasticity = material[mat_id - 1].get_elasticity();
              Assert(p.size() ==
                       n_f_q_points * GeometryInfo<dim>::faces_per_cell,
                     ExcMessage("Wrong number of cell data!"));
              local_matrix = 0;
              local_stiffness = 0;
              local_rhs = 0;

              fe_values.reinit(cell);

              // Loop over quadrature points
              for (unsigned int q = 0; q < n_q_points; ++q)
                {
                  // Loop over the dofs once, to calculate the grad_ph_u
                  for (unsigned int k = 0; k < dofs_per_cell; ++k)
                    {
                      symmetric_grad_phi[k] =
                        fe_values[displacements].symmetric_gradient(k, q);
                      phi[k] = fe_values[displacements].value(k, q);
                    }
                  // Loop over the dofs again, to assemble
                  for (unsigned int i = 0; i < dofs_per_cell; ++i)
                    {
                      for (unsigned int j = 0; j < dofs_per_cell; ++j)
                        {
                          if (is_initial)
                            {
                              local_matrix[i][j] +=
                                rho * phi[i] * phi[j] * fe_values.JxW(q);
                            }
                          else
                            {
                              local_matrix[i][j] +=
                                (rho * phi[i] * phi[j] +
                                 symmetric_grad_phi[i] * elasticity *
                                   symmetric_grad_phi[j] * beta * dt * dt) *
                                fe_values.JxW(q);
                              local_stiffness[i][j] +=
                                symmetric_grad_phi[i] * elasticity *
                                symmetric_grad_phi[j] * fe_values.JxW(q);
                            }
                        }
                      // zero body force
                      Tensor<1, dim> gravity;
                      local_rhs[i] += phi[i] * gravity * rho * fe_values.JxW(q);
                    }
                }

              cell->get_dof_indices(local_dof_indices);

              // Traction or Pressure
              for (unsigned int face = 0;
                   face < GeometryInfo<dim>::faces_per_cell;
                   ++face)
                {
                  if (cell->face(face)->at_boundary())
                    {
                      unsigned int id = cell->face(face)->boundary_id();
                      if (!cell->face(face)->at_boundary())
                        {
                          // Not a Neumann boundary
                          continue;
                        }

                      if (parameters.simulation_type != "FSI" &&
                          parameters.solid_neumann_bcs.find(id) ==
                            parameters.solid_neumann_bcs.end())
                        {
                          // Traction-free boundary, do nothing
                          continue;
                        }

                      std::vector<double> value;
                      if (parameters.simulation_type != "FSI")
                        {
                          // In stand-alone simulation, the boundary value
                          // is prescribed by the user.
                          value = parameters.solid_neumann_bcs[id];
                        }
                      Tensor<1, dim> traction;
                      if (parameters.simulation_type != "FSI" &&
                          parameters.solid_neumann_bc_type == "Traction")
                        {
                          for (unsigned int i = 0; i < dim; ++i)
                            {
                              traction[i] = value[i];
                            }
                        }

                      fe_face_values.reinit(cell, face);
                      for (unsigned int q = 0; q < n_f_q_points; ++q)
                        {
                          if (parameters.simulation_type != "FSI" &&
                              parameters.solid_neumann_bc_type == "Pressure")
                            {
                              // The normal is w.r.t. reference
                              // configuration!
                              traction = fe_face_values.normal_vector(q);
                              traction *= value[0];
                            }
                          else if (parameters.simulation_type == "FSI")
                            {
                              traction =
                                p[face * n_f_q_points + q]->fsi_traction;
                            }
                          for (unsigned int j = 0; j < dofs_per_cell; ++j)
                            {
                              const unsigned int component_j =
                                fe.system_to_component_index(j).first;
                              // +external force
                              local_rhs(j) += fe_face_values.shape_value(j, q) *
                                              traction[component_j] *
                                              fe_face_values.JxW(q);
                            }
                        }
                    }
                }

              // Now distribute local data to the system, and apply the
              // hanging node constraints at the same time.
              constraints.distribute_local_to_global(local_matrix,
                                                     local_rhs,
                                                     local_dof_indices,
                                                     system_matrix,
                                                     system_rhs);
              constraints.distribute_local_to_global(
                local_stiffness, local_dof_indices, stiffness_matrix);
            }
        }
      // Synchronize with other processors.
      system_matrix.compress(VectorOperation::add);
      system_rhs.compress(VectorOperation::add);
      stiffness_matrix.compress(VectorOperation::add);
    }

    template <int dim>
    void SharedLinearElasticity<dim>::run_one_step(bool first_step)
    {
      std::cout.precision(6);
      std::cout.width(12);

      double gamma = 0.5 + parameters.damping;
      double beta = gamma / 2;

      if (first_step)
        {
          // Neet to compute the initial acceleration, \f$ Ma_n = F \f$,
          // at this point set system_matrix to mass_matrix.
          assemble_system(true);
          this->solve(system_matrix, previous_acceleration, system_rhs);
          // Update the system_matrix
          assemble_system(false);
          this->output_results(time.get_timestep());
        }

      else if (parameters.simulation_type == "FSI")
        assemble_system(false);

      const double dt = time.get_delta_t();

      PETScWrappers::MPI::Vector tmp1(locally_owned_dofs, mpi_communicator);
      PETScWrappers::MPI::Vector tmp2(locally_owned_dofs, mpi_communicator);

      PETScWrappers::MPI::Vector tmp3(locally_owned_dofs, mpi_communicator);

      time.increment();
      pcout << std::string(91, '*') << std::endl
            << "Time step = " << time.get_timestep()
            << ", at t = " << std::scientific << time.current() << std::endl;

      // Modify the RHS
      tmp1 = system_rhs;
      tmp2 = previous_displacement;
      tmp2.add(
        dt, previous_velocity, (0.5 - beta) * dt * dt, previous_acceleration);
      stiffness_matrix.vmult(tmp3, tmp2);
      tmp1 -= tmp3;

      auto state = this->solve(system_matrix, current_acceleration, tmp1);

      // update the current velocity
      // \f$ v_{n+1} = v_n + (1-\gamma)\Delta{t}a_n + \gamma\Delta{t}a_{n+1}
      // \f$
      current_velocity = previous_velocity;
      current_velocity.add(dt * (1 - gamma),
                           previous_acceleration,
                           dt * gamma,
                           current_acceleration);

      // update the current displacement
      current_displacement = previous_displacement;
      current_displacement.add(dt, previous_velocity);
      current_displacement.add(dt * dt * (0.5 - beta),
                               previous_acceleration,
                               dt * dt * beta,
                               current_acceleration);

      // update the previous values
      previous_acceleration = current_acceleration;
      previous_velocity = current_velocity;
      previous_displacement = current_displacement;

      pcout << std::scientific << std::left << " CG iteration: " << std::setw(3)
            << state.first << " CG residual: " << state.second << std::endl;

      // strain and stress
      update_strain_and_stress();

      if (time.time_to_output())
        {
          this->output_results(time.get_timestep());
        }

      if (time.time_to_refine())
        {
          this->refine_mesh(1, 4);
          tmp1.reinit(locally_owned_dofs, mpi_communicator);
          tmp2.reinit(locally_owned_dofs, mpi_communicator);
          tmp3.reinit(locally_owned_dofs, mpi_communicator);
          assemble_system(false);
        }

      if (parameters.simulation_type == "Solid" && time.time_to_save())
        {
          this->save_checkpoint(time.get_timestep());
        }
    }

    template <int dim>
    void SharedLinearElasticity<dim>::update_strain_and_stress()
    {
      // Strain and stress at cell level are stored as vectors in order to match
      strain = 0;
      stress = 0;
      // the global ones.
      Vector<double> cell_strain(dg_fe.dofs_per_cell);
      Vector<double> cell_stress(dg_fe.dofs_per_cell);
      // Strain and stress at quadrature level are vectorized
      const unsigned n = dim * dim;
      Vector<double> quad_strain(volume_quad_formula.size() * n);
      Vector<double> quad_stress(volume_quad_formula.size() * n);
      // The projection matrix from quadrature points to the dofs.
      FullMatrix<double> qpt_to_dof(dg_fe.dofs_per_cell,
                                    n * volume_quad_formula.size());
      FullMatrix<double> tmp(qpt_to_dof.m() / n, qpt_to_dof.n() / n);
      FETools::compute_projection_from_quadrature_points_matrix(
        dg_fe.get_sub_fe(0, 1), volume_quad_formula, volume_quad_formula, tmp);
      // Expand tmp to get qpt_to_dof
      for (unsigned int i = 0; i < n; ++i)
        {
          qpt_to_dof.fill(tmp, i * tmp.m(), i * tmp.n(), 0, 0);
        }
      // Displacement gradients at quadrature points.
      std::vector<Tensor<2, dim>> current_displacement_gradients(
        volume_quad_formula.size());
      SymmetricTensor<4, dim> elasticity;
      const FEValuesExtractors::Vector displacements(0);
      FEValues<dim> fe_values(fe,
                              volume_quad_formula,
                              update_values | update_gradients |
                                update_quadrature_points | update_JxW_values);
      auto cell = dof_handler.begin_active();
      auto dg_cell = dg_dof_handler.begin_active();
      Vector<double> localized_current_displacement(current_displacement);
      for (; cell != dof_handler.end(); ++cell, ++dg_cell)
        {
          if (cell->subdomain_id() == this_mpi_process)
            {
              fe_values.reinit(cell);
              fe_values[displacements].get_function_gradients(
                localized_current_displacement, current_displacement_gradients);
              int mat_id = cell->material_id();
              if (material.size() == 1)
                mat_id = 1;
              elasticity = material[mat_id - 1].get_elasticity();
              for (unsigned int q = 0; q < volume_quad_formula.size(); ++q)
                {
                  SymmetricTensor<2, dim> tmp_strain, tmp_stress;
                  for (unsigned int i = 0; i < dim; ++i)
                    {
                      for (unsigned int j = 0; j < dim; ++j)
                        {
                          tmp_strain[i][j] =
                            (current_displacement_gradients[q][i][j] +
                             current_displacement_gradients[q][j][i]) /
                            2;
                        }
                    }
                  tmp_stress = elasticity * tmp_strain;
                  for (unsigned int k = 0; k < n; ++k)
                    {
                      auto index =
                        Tensor<2, dim>::unrolled_to_component_indices(k);
                      quad_strain[k * volume_quad_formula.size() + q] =
                        tmp_strain[index[0]][index[1]];
                      quad_stress[k * volume_quad_formula.size() + q] =
                        tmp_stress[index[0]][index[1]];
                    }
                }
              qpt_to_dof.vmult(cell_strain, quad_strain);
              qpt_to_dof.vmult(cell_stress, quad_stress);
              dg_cell->set_dof_values(cell_strain, strain);
              dg_cell->set_dof_values(cell_stress, stress);
            }
        }
      strain.compress(VectorOperation::insert);
      stress.compress(VectorOperation::insert);
    }

    template class SharedLinearElasticity<2>;
    template class SharedLinearElasticity<3>;
  } // namespace MPI
} // namespace Solid
