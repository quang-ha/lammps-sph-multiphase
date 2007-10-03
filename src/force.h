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

#ifndef FORCE_H
#define FORCE_H

#include "pointers.h"

namespace LAMMPS_NS {

class Force : protected Pointers {
 public:
  double boltz;                      // Boltzmann constant (eng/degree-K)
  double mvv2e;                      // conversion of mv^2 to energy
  double ftm2v;                      // conversion of ft/m to velocity
  double nktv2p;                     // conversion of NkT/V to pressure
  double qqr2e;                      // conversion of q^2/r to energy
  double qe2f;                       // conversion of qE to force
  double dielectric;                 // dielectric constant
  double qqrd2e;                     // q^2/r to energy w/ dielectric constant

  int newton,newton_pair,newton_bond;   // Newton's 3rd law settings

  class Pair *pair;
  char *pair_style;

  class Bond *bond;
  char *bond_style;

  class Angle *angle;
  char *angle_style;

  class Dihedral *dihedral;
  char *dihedral_style;

  class Improper *improper;
  char *improper_style;

  class KSpace *kspace;
  char *kspace_style;
                             // index [0] is not used in these arrays
  double special_lj[4];      // 1-2, 1-3, 1-4 prefactors for LJ
  double special_coul[4];    // 1-2, 1-3, 1-4 prefactors for Coulombics

  Force(class LAMMPS *);
  ~Force();
  void init();

  void create_pair(const char *);
  class Pair *new_pair(const char *);
  class Pair *pair_match(const char *);

  void create_bond(const char *);
  class Bond *new_bond(const char *);
  class Bond *bond_match(const char *); 

  void create_angle(const char *);
  class Angle *new_angle(const char *);

  void create_dihedral(const char *);
  class Dihedral *new_dihedral(const char *);

  void create_improper(const char *);
  class Improper *new_improper(const char *);

  void create_kspace(int, char **);

  void set_special(int, char **);
  void bounds(char *, int, int &, int &);
  int memory_usage();
};

}

#endif
