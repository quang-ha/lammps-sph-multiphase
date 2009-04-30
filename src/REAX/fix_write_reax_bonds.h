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

#ifndef FIX_WRITE_REAX_BONDS_H
#define FIX_WRITE_REAX_BONDS_H

#include "stdio.h"
#include "fix.h"

namespace LAMMPS_NS {

class FixWriteReaxBonds : public Fix {
 public:
  FixWriteReaxBonds(class LAMMPS *, int, char **);
  ~FixWriteReaxBonds();
  int setmask();
  void init();
  void setup(int);
  void end_of_step();
  double memory_usage();
  void OutputReaxBonds(int, FILE*);
  int nint(const double&);

 private:
  int me;
  int nfreq;
  FILE *fp;

};

}

#endif
