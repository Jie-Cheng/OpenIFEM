#ifndef MPI_SOLID_SOLVER_SHARED
#define MPI_SOLID_SOLVER_SHARED

#include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/function.h>
#include <deal.II/base/index_set.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/quadrature_point_data.h>
#include <deal.II/base/tensor.h>
#include <deal.II/base/timer.h>
#include <deal.II/base/utilities.h>

#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe.h>
#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_tools.h>
#include <deal.II/fe/fe_values.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_in.h>
#include <deal.II/grid/grid_out.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>

#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/generic_linear_algebra.h>
#include <deal.II/lac/petsc_precondition.h>
#include <deal.II/lac/petsc_solver.h>
#include <deal.II/lac/petsc_sparse_matrix.h>
#include <deal.II/lac/petsc_vector.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/sparsity_tools.h>
#include <deal.II/lac/vector.h>

#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/data_postprocessor.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/solution_transfer.h>
#include <deal.II/numerics/vector_tools.h>

#include <deal.II/distributed/grid_refinement.h>
#include <deal.II/distributed/solution_transfer.h>
#include <deal.II/distributed/tria.h>

#include <experimental/filesystem>
#include <fstream>
#include <iostream>

#include "parameters.h"
#include "utilities.h"

namespace fs = std::experimental::filesystem;

namespace MPI
{
  template <int dim>
  class FSI;
}

namespace Solid
{
  namespace MPI
  {
    using namespace dealii;

    /// Base class for all parallel solid solvers.
    template <int dim>
    class SharedSolidSolver
    {
    public:
      //! FSI solver need access to the private members of this solver.
      friend ::MPI::FSI<dim>;

      SharedSolidSolver(Triangulation<dim> &,
                        const Parameters::AllParameters &);
      ~SharedSolidSolver();
      void run();
      PETScWrappers::MPI::Vector get_current_solution() const;

    protected:
      struct CellProperty;
      /**
       * Set up the DofHandler, reorder the grid, sparsity pattern.
       */
      virtual void setup_dofs();

      /**
       * Initialize the matrix, solution, and rhs. This is separated from
       * setup_dofs because if we may want to transfer solution from one grid
       * to another in the refine_mesh.
       */
      virtual void initialize_system();

      /**
       * Assemble both the system matrices and rhs.
       */
      virtual void assemble_system(bool) = 0;

      /**
       * Update the cached strain and stress to output.
       */
      virtual void update_strain_and_stress() = 0;

      /**
       * Run one time step.
       */
      virtual void run_one_step(bool) = 0;

      /**
       * Solve the linear system. Returns the number of
       * CG iterations and the final residual.
       */
      std::pair<unsigned int, double>
      solve(const PETScWrappers::MPI::SparseMatrix &,
            PETScWrappers::MPI::Vector &,
            const PETScWrappers::MPI::Vector &);

      /**
       * Output the time-dependent solution in vtu format.
       */
      void output_results(const unsigned int);

      /**
       * Refine mesh and transfer solution.
       */
      void refine_mesh(const unsigned int, const unsigned int);

      /**
       * Save the checkpoint for restart (only global refinement supported)
       */
      void save_checkpoint(const int);

      /**
       * Load from checkpoint to restart
       */
      bool load_checkpoint();

      Triangulation<dim> &triangulation;
      Parameters::AllParameters parameters;
      DoFHandler<dim> dof_handler;
      DoFHandler<dim> dg_dof_handler; //!< DoFHandler for nodal strain/stress
      FESystem<dim> fe;
      FESystem<dim> dg_fe; //!< DFE for nodal strain/stress.
      const QGauss<dim>
        volume_quad_formula; //!< Quadrature formula for volume integration.
      const QGauss<dim - 1>
        face_quad_formula; //!< Quadrature formula for face integration.

      /**
       * Constraints to handle both hanging nodes and Dirichlet boundary
       * conditions.
       */
      AffineConstraints<double> constraints;

      PETScWrappers::MPI::SparseMatrix
        system_matrix; //!< \f$ M + \beta{\Delta{t}}^2K \f$.
      PETScWrappers::MPI::SparseMatrix
        mass_matrix; //!< Required by hyperelastic solver.
      PETScWrappers::MPI::SparseMatrix
        stiffness_matrix; //!< The stiffness is used in the rhs.
      PETScWrappers::MPI::Vector system_rhs;

      /**
       * In the Newmark-beta method, acceleration is the variable to solve at
       * every
       * timestep. But displacement and velocity also contribute to the rhs of
       * the equation. For the sake of clarity, we explicitly store two sets of
       * accleration, velocity and displacement.
       */
      PETScWrappers::MPI::Vector current_acceleration;
      PETScWrappers::MPI::Vector current_velocity;
      PETScWrappers::MPI::Vector current_displacement;
      PETScWrappers::MPI::Vector previous_acceleration;
      PETScWrappers::MPI::Vector previous_velocity;
      PETScWrappers::MPI::Vector previous_displacement;

      /**
       * Nodal strain and stress are stored as vectors but viewed as tensors.
       * For each support point there are dim * dim components.
       */
      mutable PETScWrappers::MPI::Vector strain, stress;

      MPI_Comm mpi_communicator;
      const unsigned int n_mpi_processes;
      const unsigned int this_mpi_process;
      ConditionalOStream pcout;
      Utils::Time time;
      mutable TimerOutput timer;
      IndexSet locally_owned_dofs;
      IndexSet locally_owned_dg_dofs;
      IndexSet locally_relevant_dofs;
      mutable std::vector<std::pair<double, std::string>> times_and_names;

      CellDataStorage<typename Triangulation<dim>::cell_iterator, CellProperty>
        cell_property;

      /**
       * The fluid traction in FSI simulation, which should be set by the FSI.
       */
      struct CellProperty
      {
        Tensor<1, dim> fsi_traction;
      };
    };
  } // namespace MPI
} // namespace Solid

#endif
