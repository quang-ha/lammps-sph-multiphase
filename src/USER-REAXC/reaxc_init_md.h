/*----------------------------------------------------------------------
  PuReMD - Purdue ReaxFF Molecular Dynamics Program
  
  Copyright (2010) Purdue University
  Hasan Metin Aktulga, haktulga@cs.purdue.edu
  Joseph Fogarty, jcfogart@mail.usf.edu
  Sagar Pandit, pandit@usf.edu
  Ananth Y Grama, ayg@cs.purdue.edu

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of 
  the License, or (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
  See the GNU General Public License for more details:
  <http://www.gnu.org/licenses/>.
  ----------------------------------------------------------------------*/

#ifndef __INIT_MD_H_
#define __INIT_MD_H_

#include "reaxc_types.h"

#if defined(PURE_REAX)
void Initialize( reax_system*, control_params*, simulation_data*,
		 storage*, reax_list**, output_controls*, mpi_datatypes* );
#elif defined(LAMMPS_REAX)
void Initialize( reax_system*, control_params*, simulation_data*, storage*, 
		 reax_list**, output_controls*, mpi_datatypes*, MPI_Comm );
#endif

#endif
