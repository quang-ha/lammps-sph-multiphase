/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "mpi.h"
#include "stdlib.h"
#include "string.h"
#include "comm.h"
#include "universe.h"
#include "atom.h"
#include "atom_vec.h"
#include "force.h"
#include "pair.h"
#include "modify.h"
#include "fix.h"
#include "compute.h"
#include "domain.h"
#include "output.h"
#include "dump.h"
#include "group.h"
#include "procmap.h"
#include "accelerator_kokkos.h"
#include "memory.h"
#include "error.h"

#ifdef _OPENMP
#include "omp.h"
#endif

using namespace LAMMPS_NS;

#define BUFMIN 1000             // also in comm styles

enum{SINGLE,MULTI};             // same as in Comm sub-styles
enum{MULTIPLE};                   // same as in ProcMap
enum{ONELEVEL,TWOLEVEL,NUMA,CUSTOM};
enum{CART,CARTREORDER,XYZ};

/* ---------------------------------------------------------------------- */

Comm::Comm(LAMMPS *lmp) : Pointers(lmp)
{
  MPI_Comm_rank(world,&me);
  MPI_Comm_size(world,&nprocs);

  mode = 0;
  bordergroup = 0;
  cutghostuser = 0.0;
  ghost_velocity = 0;

  user_procgrid[0] = user_procgrid[1] = user_procgrid[2] = 0;
  coregrid[0] = coregrid[1] = coregrid[2] = 1;
  gridflag = ONELEVEL;
  mapflag = CART;
  customfile = NULL;
  outfile = NULL;
  recv_from_partition = send_to_partition = -1;
  otherflag = 0;
  maxexchange_atom = maxexchange_fix = 0;

  grid2proc = NULL;
  xsplit = ysplit = zsplit = NULL;
  rcbnew = 0;

  // use of OpenMP threads
  // query OpenMP for number of threads/process set by user at run-time
  // if the OMP_NUM_THREADS environment variable is not set, we default
  // to using 1 thread. This follows the principle of the least surprise,
  // while practically all OpenMP implementations violate it by using
  // as many threads as there are (virtual) CPU cores by default.

  nthreads = 1;
#ifdef _OPENMP
  if (lmp->kokkos) {
    nthreads = lmp->kokkos->num_threads * lmp->kokkos->numa;
  } else if (getenv("OMP_NUM_THREADS") == NULL) {
    nthreads = 1;
    if (me == 0)
      error->warning(FLERR,"OMP_NUM_THREADS environment is not set.");
  } else {
    nthreads = omp_get_max_threads();
  }

  // enforce consistent number of threads across all MPI tasks

  MPI_Bcast(&nthreads,1,MPI_INT,0,world);
  if (!lmp->kokkos) omp_set_num_threads(nthreads);

  if (me == 0) {
    if (screen)
      fprintf(screen,"  using %d OpenMP thread(s) per MPI task\n",nthreads);
    if (logfile)
      fprintf(logfile,"  using %d OpenMP thread(s) per MPI task\n",nthreads);
  }
#endif

}

/* ---------------------------------------------------------------------- */

Comm::~Comm()
{
  memory->destroy(grid2proc);
  memory->destroy(xsplit);
  memory->destroy(ysplit);
  memory->destroy(zsplit);
  delete [] customfile;
  delete [] outfile;
}

/* ----------------------------------------------------------------------
   deep copy of arrays from old Comm class to new one
   all public/protected vectors/arrays in parent Comm class must be copied
   called from alternate constructor of child classes
   when new comm style is created from Input
------------------------------------------------------------------------- */

void Comm::copy_arrays(Comm *oldcomm)
{
  if (oldcomm->grid2proc) {
    memory->create(grid2proc,procgrid[0],procgrid[1],procgrid[2],
                   "comm:grid2proc");
    memcpy(&grid2proc[0][0][0],&oldcomm->grid2proc[0][0][0],
           (procgrid[0]*procgrid[1]*procgrid[2])*sizeof(int));
    
    memory->create(xsplit,procgrid[0]+1,"comm:xsplit");
    memory->create(ysplit,procgrid[1]+1,"comm:ysplit");
    memory->create(zsplit,procgrid[2]+1,"comm:zsplit");
    memcpy(xsplit,oldcomm->xsplit,(procgrid[0]+1)*sizeof(double));
    memcpy(ysplit,oldcomm->ysplit,(procgrid[1]+1)*sizeof(double));
    memcpy(zsplit,oldcomm->zsplit,(procgrid[2]+1)*sizeof(double));
  }

  if (customfile) {
    int n = strlen(oldcomm->customfile) + 1;
    customfile = new char[n];
    strcpy(customfile,oldcomm->customfile);
  }
  if (outfile) {
    int n = strlen(oldcomm->outfile) + 1;
    outfile = new char[n];
    strcpy(outfile,oldcomm->outfile);
  }
}

/* ----------------------------------------------------------------------
   common to all Comm styles
------------------------------------------------------------------------- */

void Comm::init()
{
  triclinic = domain->triclinic;
  map_style = atom->map_style;

  // warn if any proc's sub-box is smaller than neigh skin
  // since may lead to lost atoms in exchange()
  // really should check every exchange() in case box size is shrinking
  // but seems overkill to do that

  int flag = 0;
  if (!triclinic) {
    if (domain->subhi[0] - domain->sublo[0] < neighbor->skin) flag = 1;
    if (domain->subhi[1] - domain->sublo[1] < neighbor->skin) flag = 1;
    if (domain->dimension == 3)
      if (domain->subhi[2] - domain->sublo[2] < neighbor->skin) flag = 1;
  } else {
    double delta = domain->subhi_lamda[0] - domain->sublo_lamda[0];
    if (delta*domain->prd[0] < neighbor->skin) flag = 1;
    delta = domain->subhi_lamda[1] - domain->sublo_lamda[1];
    if (delta*domain->prd[1] < neighbor->skin) flag = 1;
    if (domain->dimension == 3) {
      delta = domain->subhi_lamda[2] - domain->sublo_lamda[2];
      if (delta*domain->prd[2] < neighbor->skin) flag = 1;
    }
  }

  int flagall;
  MPI_Allreduce(&flag,&flagall,1,MPI_INT,MPI_SUM,world);
  if (flagall && me == 0) 
    error->warning(FLERR,"Proc sub-domain size < neighbor skin - "
                   "could lead to lost atoms");

  // comm_only = 1 if only x,f are exchanged in forward/reverse comm
  // comm_x_only = 0 if ghost_velocity since velocities are added

  comm_x_only = atom->avec->comm_x_only;
  comm_f_only = atom->avec->comm_f_only;
  if (ghost_velocity) comm_x_only = 0;

  // set per-atom sizes for forward/reverse/border comm
  // augment by velocity and fix quantities if needed

  size_forward = atom->avec->size_forward;
  size_reverse = atom->avec->size_reverse;
  size_border = atom->avec->size_border;

  if (ghost_velocity) size_forward += atom->avec->size_velocity;
  if (ghost_velocity) size_border += atom->avec->size_velocity;

  for (int i = 0; i < modify->nfix; i++)
    size_border += modify->fix[i]->comm_border;
  
  // per-atom limits for communication
  // maxexchange = max # of datums in exchange comm, set in exchange()
  // maxforward = # of datums in largest forward comm
  // maxreverse = # of datums in largest reverse comm
  // query pair,fix,compute,dump for their requirements
  // pair style can force reverse comm even if newton off
 	 
  maxforward = MAX(size_forward,size_border);
  maxreverse = size_reverse;

  if (force->pair) maxforward = MAX(maxforward,force->pair->comm_forward);
  if (force->pair) maxreverse = MAX(maxreverse,force->pair->comm_reverse);

  for (int i = 0; i < modify->nfix; i++) {
    maxforward = MAX(maxforward,modify->fix[i]->comm_forward);
    maxreverse = MAX(maxreverse,modify->fix[i]->comm_reverse);
  }

  for (int i = 0; i < modify->ncompute; i++) {
    maxforward = MAX(maxforward,modify->compute[i]->comm_forward);
    maxreverse = MAX(maxreverse,modify->compute[i]->comm_reverse);
  }

  for (int i = 0; i < output->ndump; i++) {
    maxforward = MAX(maxforward,output->dump[i]->comm_forward);
    maxreverse = MAX(maxreverse,output->dump[i]->comm_reverse);
  }

  if (force->newton == 0) maxreverse = 0;
  if (force->pair) maxreverse = MAX(maxreverse,force->pair->comm_reverse_off);
}

/* ----------------------------------------------------------------------
   modify communication params
   invoked from input script by comm_modify command
------------------------------------------------------------------------- */

void Comm::modify_params(int narg, char **arg)
{
  if (narg < 1) error->all(FLERR,"Illegal comm_modify command");

  int iarg = 0;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"mode") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal comm_modify command");
      if (strcmp(arg[iarg+1],"single") == 0) mode = SINGLE;
      else if (strcmp(arg[iarg+1],"multi") == 0) mode = MULTI;
      else error->all(FLERR,"Illegal comm_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"group") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal comm_modify command");
      bordergroup = group->find(arg[iarg+1]);
      if (bordergroup < 0)
        error->all(FLERR,"Invalid group in comm_modify command");
      if (bordergroup && (atom->firstgroupname == NULL ||
                          strcmp(arg[iarg+1],atom->firstgroupname) != 0))
        error->all(FLERR,"Comm_modify group != atom_modify first group");
      iarg += 2;
    } else if (strcmp(arg[iarg],"cutoff") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal comm_modify command");
      cutghostuser = force->numeric(FLERR,arg[iarg+1]);
      if (cutghostuser < 0.0)
        error->all(FLERR,"Invalid cutoff in comm_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"vel") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal comm_modify command");
      if (strcmp(arg[iarg+1],"yes") == 0) ghost_velocity = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) ghost_velocity = 0;
      else error->all(FLERR,"Illegal comm_modify command");
      iarg += 2;
    } else error->all(FLERR,"Illegal comm_modify command");
  }
}

/* ----------------------------------------------------------------------
   set dimensions for 3d grid of processors, and associated flags
   invoked from input script by processors command
------------------------------------------------------------------------- */

void Comm::set_processors(int narg, char **arg)
{
  if (narg < 3) error->all(FLERR,"Illegal processors command");

  if (strcmp(arg[0],"*") == 0) user_procgrid[0] = 0;
  else user_procgrid[0] = force->inumeric(FLERR,arg[0]);
  if (strcmp(arg[1],"*") == 0) user_procgrid[1] = 0;
  else user_procgrid[1] = force->inumeric(FLERR,arg[1]);
  if (strcmp(arg[2],"*") == 0) user_procgrid[2] = 0;
  else user_procgrid[2] = force->inumeric(FLERR,arg[2]);

  if (user_procgrid[0] < 0 || user_procgrid[1] < 0 || user_procgrid[2] < 0)
    error->all(FLERR,"Illegal processors command");

  int p = user_procgrid[0]*user_procgrid[1]*user_procgrid[2];
  if (p && p != nprocs)
    error->all(FLERR,"Specified processors != physical processors");

  int iarg = 3;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"grid") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal processors command");

      if (strcmp(arg[iarg+1],"onelevel") == 0) {
        gridflag = ONELEVEL;

      } else if (strcmp(arg[iarg+1],"twolevel") == 0) {
        if (iarg+6 > narg) error->all(FLERR,"Illegal processors command");
        gridflag = TWOLEVEL;

        ncores = force->inumeric(FLERR,arg[iarg+2]);
        if (strcmp(arg[iarg+3],"*") == 0) user_coregrid[0] = 0;
        else user_coregrid[0] = force->inumeric(FLERR,arg[iarg+3]);
        if (strcmp(arg[iarg+4],"*") == 0) user_coregrid[1] = 0;
        else user_coregrid[1] = force->inumeric(FLERR,arg[iarg+4]);
        if (strcmp(arg[iarg+5],"*") == 0) user_coregrid[2] = 0;
        else user_coregrid[2] = force->inumeric(FLERR,arg[iarg+5]);

        if (ncores <= 0 || user_coregrid[0] < 0 ||
            user_coregrid[1] < 0 || user_coregrid[2] < 0)
          error->all(FLERR,"Illegal processors command");
        iarg += 4;

      } else if (strcmp(arg[iarg+1],"numa") == 0) {
        gridflag = NUMA;

      } else if (strcmp(arg[iarg],"custom") == 0) {
        if (iarg+3 > narg) error->all(FLERR,"Illegal processors command");
        gridflag = CUSTOM;
        delete [] customfile;
        int n = strlen(arg[iarg+2]) + 1;
        customfile = new char[n];
        strcpy(customfile,arg[iarg+2]);
        iarg += 1;

      } else error->all(FLERR,"Illegal processors command");
      iarg += 2;

    } else if (strcmp(arg[iarg],"map") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal processors command");
      if (strcmp(arg[iarg+1],"cart") == 0) mapflag = CART;
      else if (strcmp(arg[iarg+1],"cart/reorder") == 0) mapflag = CARTREORDER;
      else if (strcmp(arg[iarg+1],"xyz") == 0 ||
               strcmp(arg[iarg+1],"xzy") == 0 ||
               strcmp(arg[iarg+1],"yxz") == 0 ||
               strcmp(arg[iarg+1],"yzx") == 0 ||
               strcmp(arg[iarg+1],"zxy") == 0 ||
               strcmp(arg[iarg+1],"zyx") == 0) {
        mapflag = XYZ;
        strcpy(xyz,arg[iarg+1]);
      } else error->all(FLERR,"Illegal processors command");
      iarg += 2;

    } else if (strcmp(arg[iarg],"part") == 0) {
      if (iarg+4 > narg) error->all(FLERR,"Illegal processors command");
      if (universe->nworlds == 1)
        error->all(FLERR,
                   "Cannot use processors part command "
                   "without using partitions");
      int isend = force->inumeric(FLERR,arg[iarg+1]);
      int irecv = force->inumeric(FLERR,arg[iarg+2]);
      if (isend < 1 || isend > universe->nworlds ||
          irecv < 1 || irecv > universe->nworlds || isend == irecv)
        error->all(FLERR,"Invalid partitions in processors part command");
      if (isend-1 == universe->iworld) {
        if (send_to_partition >= 0)
          error->all(FLERR,
                     "Sending partition in processors part command "
                     "is already a sender");
        send_to_partition = irecv-1;
      }
      if (irecv-1 == universe->iworld) {
        if (recv_from_partition >= 0)
          error->all(FLERR,
                     "Receiving partition in processors part command "
                     "is already a receiver");
        recv_from_partition = isend-1;
      }

      // only receiver has otherflag dependency

      if (strcmp(arg[iarg+3],"multiple") == 0) {
        if (universe->iworld == irecv-1) {
          otherflag = 1;
          other_style = MULTIPLE;
        }
      } else error->all(FLERR,"Illegal processors command");
      iarg += 4;

    } else if (strcmp(arg[iarg],"file") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal processors command");
      delete [] outfile;
      int n = strlen(arg[iarg+1]) + 1;
      outfile = new char[n];
      strcpy(outfile,arg[iarg+1]);
      iarg += 2;

    } else error->all(FLERR,"Illegal processors command");
  }

  // error checks

  if (gridflag == NUMA && mapflag != CART)
    error->all(FLERR,"Processors grid numa and map style are incompatible");
  if (otherflag && (gridflag == NUMA || gridflag == CUSTOM))
    error->all(FLERR,
               "Processors part option and grid style are incompatible");
}

/* ----------------------------------------------------------------------
   create a 3d grid of procs based on Nprocs and box size & shape
   map processors to grid, setup xyz split for a uniform grid
------------------------------------------------------------------------- */

void Comm::set_proc_grid(int outflag)
{
  // recv 3d proc grid of another partition if my 3d grid depends on it

  if (recv_from_partition >= 0) {
    MPI_Status status;
    if (me == 0) {
      MPI_Recv(other_procgrid,3,MPI_INT,
               universe->root_proc[recv_from_partition],0,
               universe->uworld,&status);
      MPI_Recv(other_coregrid,3,MPI_INT,
               universe->root_proc[recv_from_partition],0,
               universe->uworld,&status);
    }
    MPI_Bcast(other_procgrid,3,MPI_INT,0,world);
    MPI_Bcast(other_coregrid,3,MPI_INT,0,world);
  }

  // create ProcMap class to create 3d grid and map procs to it

  ProcMap *pmap = new ProcMap(lmp);

  // create 3d grid of processors
  // produces procgrid and coregrid (if relevant)

  if (gridflag == ONELEVEL) {
    pmap->onelevel_grid(nprocs,user_procgrid,procgrid,
                        otherflag,other_style,other_procgrid,other_coregrid);

  } else if (gridflag == TWOLEVEL) {
    pmap->twolevel_grid(nprocs,user_procgrid,procgrid,
                        ncores,user_coregrid,coregrid,
                        otherflag,other_style,other_procgrid,other_coregrid);

  } else if (gridflag == NUMA) {
    pmap->numa_grid(nprocs,user_procgrid,procgrid,coregrid);

  } else if (gridflag == CUSTOM) {
    pmap->custom_grid(customfile,nprocs,user_procgrid,procgrid);
  }

  // error check on procgrid
  // should not be necessary due to ProcMap

  if (procgrid[0]*procgrid[1]*procgrid[2] != nprocs)
    error->all(FLERR,"Bad grid of processors");
  if (domain->dimension == 2 && procgrid[2] != 1)
    error->all(FLERR,"Processor count in z must be 1 for 2d simulation");

  // grid2proc[i][j][k] = proc that owns i,j,k location in 3d grid

  if (grid2proc) memory->destroy(grid2proc);
  memory->create(grid2proc,procgrid[0],procgrid[1],procgrid[2],
                 "comm:grid2proc");

  // map processor IDs to 3d processor grid
  // produces myloc, procneigh, grid2proc

  if (gridflag == ONELEVEL) {
    if (mapflag == CART)
      pmap->cart_map(0,procgrid,myloc,procneigh,grid2proc);
    else if (mapflag == CARTREORDER)
      pmap->cart_map(1,procgrid,myloc,procneigh,grid2proc);
    else if (mapflag == XYZ)
      pmap->xyz_map(xyz,procgrid,myloc,procneigh,grid2proc);

  } else if (gridflag == TWOLEVEL) {
    if (mapflag == CART)
      pmap->cart_map(0,procgrid,ncores,coregrid,myloc,procneigh,grid2proc);
    else if (mapflag == CARTREORDER)
      pmap->cart_map(1,procgrid,ncores,coregrid,myloc,procneigh,grid2proc);
    else if (mapflag == XYZ)
      pmap->xyz_map(xyz,procgrid,ncores,coregrid,myloc,procneigh,grid2proc);

  } else if (gridflag == NUMA) {
    pmap->numa_map(0,coregrid,myloc,procneigh,grid2proc);

  } else if (gridflag == CUSTOM) {
    pmap->custom_map(procgrid,myloc,procneigh,grid2proc);
  }

  // print 3d grid info to screen and logfile

  if (outflag && me == 0) {
    if (screen) {
      fprintf(screen,"  %d by %d by %d MPI processor grid\n",
              procgrid[0],procgrid[1],procgrid[2]);
      if (gridflag == NUMA || gridflag == TWOLEVEL)
        fprintf(screen,"  %d by %d by %d core grid within node\n",
                coregrid[0],coregrid[1],coregrid[2]);
    }
    if (logfile) {
      fprintf(logfile,"  %d by %d by %d MPI processor grid\n",
              procgrid[0],procgrid[1],procgrid[2]);
      if (gridflag == NUMA || gridflag == TWOLEVEL)
        fprintf(logfile,"  %d by %d by %d core grid within node\n",
                coregrid[0],coregrid[1],coregrid[2]);
    }
  }

  // print 3d grid details to outfile

  if (outfile) pmap->output(outfile,procgrid,grid2proc);

  // free ProcMap class

  delete pmap;

  // set xsplit,ysplit,zsplit for uniform spacings

  memory->destroy(xsplit);
  memory->destroy(ysplit);
  memory->destroy(zsplit);

  memory->create(xsplit,procgrid[0]+1,"comm:xsplit");
  memory->create(ysplit,procgrid[1]+1,"comm:ysplit");
  memory->create(zsplit,procgrid[2]+1,"comm:zsplit");

  for (int i = 0; i < procgrid[0]; i++) xsplit[i] = i * 1.0/procgrid[0];
  for (int i = 0; i < procgrid[1]; i++) ysplit[i] = i * 1.0/procgrid[1];
  for (int i = 0; i < procgrid[2]; i++) zsplit[i] = i * 1.0/procgrid[2];

  xsplit[procgrid[0]] = ysplit[procgrid[1]] = zsplit[procgrid[2]] = 1.0;

  // set lamda box params after procs are assigned
  // only set once unless load-balancing occurs

  if (domain->triclinic) domain->set_lamda_box();

  // send my 3d proc grid to another partition if requested

  if (send_to_partition >= 0) {
    if (me == 0) {
      MPI_Send(procgrid,3,MPI_INT,
               universe->root_proc[send_to_partition],0,
               universe->uworld);
      MPI_Send(coregrid,3,MPI_INT,
               universe->root_proc[send_to_partition],0,
               universe->uworld);
    }
  }
}

/* ----------------------------------------------------------------------
   communicate inbuf around full ring of processors with messtag
   nbytes = size of inbuf = n datums * nper bytes
   callback() is invoked to allow caller to process/update each proc's inbuf
   if self=1 (default), then callback() is invoked on final iteration
     using original inbuf, which may have been updated
   for non-NULL outbuf, final updated inbuf is copied to it
     outbuf = inbuf is OK
------------------------------------------------------------------------- */

void Comm::ring(int n, int nper, void *inbuf, int messtag,
                void (*callback)(int, char *), void *outbuf, int self)
{
  MPI_Request request;
  MPI_Status status;

  int nbytes = n*nper;
  int maxbytes;
  MPI_Allreduce(&nbytes,&maxbytes,1,MPI_INT,MPI_MAX,world);

  char *buf,*bufcopy;
  memory->create(buf,maxbytes,"comm:buf");
  memory->create(bufcopy,maxbytes,"comm:bufcopy");
  memcpy(buf,inbuf,nbytes);

  int next = me + 1;
  int prev = me - 1;
  if (next == nprocs) next = 0;
  if (prev < 0) prev = nprocs - 1;

  for (int loop = 0; loop < nprocs; loop++) {
    if (me != next) {
      MPI_Irecv(bufcopy,maxbytes,MPI_CHAR,prev,messtag,world,&request);
      MPI_Send(buf,nbytes,MPI_CHAR,next,messtag,world);
      MPI_Wait(&request,&status);
      MPI_Get_count(&status,MPI_CHAR,&nbytes);
      memcpy(buf,bufcopy,nbytes);
    }
    if (self || loop < nprocs-1) callback(nbytes/nper,buf);
  }

  if (outbuf) memcpy(outbuf,buf,nbytes);

  memory->destroy(buf);
  memory->destroy(bufcopy);
}

/* ----------------------------------------------------------------------
   proc 0 reads Nlines from file into buf and bcasts buf to all procs
   caller allocates buf to max size needed
   each line is terminated by newline, even if last line in file is not
   return 0 if successful, 1 if get EOF error before read is complete
------------------------------------------------------------------------- */

int Comm::read_lines_from_file(FILE *fp, int nlines, int maxline, char *buf)
{
  int m;

  if (me == 0) {
    m = 0;
    for (int i = 0; i < nlines; i++) {
      if (!fgets(&buf[m],maxline,fp)) {
	m = 0;
	break;
      }
      m += strlen(&buf[m]);
    }
    if (m) {
      if (buf[m-1] != '\n') strcpy(&buf[m++],"\n");
      m++;
    }
  }

  MPI_Bcast(&m,1,MPI_INT,0,world);
  if (m == 0) return 1;
  MPI_Bcast(buf,m,MPI_CHAR,0,world);
  return 0;
}

/* ----------------------------------------------------------------------
   proc 0 reads Nlines from file into buf and bcasts buf to all procs
   caller allocates buf to max size needed
   each line is terminated by newline, even if last line in file is not
   return 0 if successful, 1 if get EOF error before read is complete
------------------------------------------------------------------------- */

int Comm::read_lines_from_file_universe(FILE *fp, int nlines, int maxline,
                                        char *buf)
{
  int m;

  int me_universe = universe->me;
  MPI_Comm uworld = universe->uworld;

  if (me_universe == 0) {
    m = 0;
    for (int i = 0; i < nlines; i++) {
      if (!fgets(&buf[m],maxline,fp)) {
	m = 0;
	break;
      }
      m += strlen(&buf[m]);
    }
    if (m) {
      if (buf[m-1] != '\n') strcpy(&buf[m++],"\n");
      m++;
    }
  }

  MPI_Bcast(&m,1,MPI_INT,0,uworld);
  if (m == 0) return 1;
  MPI_Bcast(buf,m,MPI_CHAR,0,uworld);
  return 0;
}
