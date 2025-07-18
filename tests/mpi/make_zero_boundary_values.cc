// ------------------------------------------------------------------------
//
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010 - 2023 by the deal.II authors
//
// This file is part of the deal.II library.
//
// Part of the source code is dual licensed under Apache-2.0 WITH
// LLVM-exception OR LGPL-2.1-or-later. Detailed license information
// governing the source code and code contributions can be found in
// LICENSE.md and CONTRIBUTING.md at the top level directory of deal.II.
//
// ------------------------------------------------------------------------



// Test DoFTools::make_zero_boundary_constraints for parallel DoFHandlers

#include <deal.II/distributed/tria.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>

#include <deal.II/grid/grid_generator.h>

#include "../tests.h"



template <int dim>
void
test()
{
  parallel::distributed::Triangulation<dim> tr(MPI_COMM_WORLD);

  GridGenerator::hyper_ball(tr);
  tr.refine_global(2);

  const FE_Q<dim> fe(2);
  DoFHandler<dim> dofh(tr);
  dofh.distribute_dofs(fe);

  {
    AffineConstraints<double> boundary_values;
    DoFTools::make_zero_boundary_constraints(dofh, boundary_values);
    if (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0)
      boundary_values.print(deallog.get_file_stream());
  }

  // the result of extract_boundary_dofs is supposed to be a subset of the
  // locally relevant dofs, so do the test again with that
  {
    const IndexSet relevant_set = DoFTools::extract_locally_relevant_dofs(dofh);
    AffineConstraints<double> boundary_values(dofh.locally_owned_dofs(),
                                              relevant_set);
    DoFTools::make_zero_boundary_constraints(dofh, boundary_values);
    if (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0)
      boundary_values.print(deallog.get_file_stream());
  }
}


int
main(int argc, char *argv[])
{
  Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv, 1);

  unsigned int myid = Utilities::MPI::this_mpi_process(MPI_COMM_WORLD);


  deallog.push(Utilities::int_to_string(myid));

  if (myid == 0)
    {
      initlog();

      deallog.push("2d");
      test<2>();
      deallog.pop();

      deallog.push("3d");
      test<3>();
      deallog.pop();
    }
  else
    {
      deallog.push("2d");
      test<2>();
      deallog.pop();

      deallog.push("3d");
      test<3>();
      deallog.pop();
    }
}
