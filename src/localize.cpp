/*
 *                This source code is part of
 *
 *                     E  R  K  A  L  E
 *                             -
 *                       HF/DFT from Hel
 *
 * Written by Susi Lehtola, 2010-2011
 * Copyright (c) 2010-2011, Susi Lehtola
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "global.h"
#include "basis.h"
#include "checkpoint.h"
#include "mathf.h"
#include "stringutil.h"
#include "timer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifdef _OPENMP
#include <omp.h>
#endif

void localize_wrk(const BasisSet & basis, arma::mat & C, const std::vector<double> & occs) {
  // Orbitals to localize
  std::vector<size_t> locorb;
  for(size_t io=0;io<occs.size();io++)
    if(occs[io]!=0.0)
      locorb.push_back(io);
    
  // Loop over orbitals
  while(locorb.size()) {
    // Orbitals to treat in this run
    std::vector<size_t> orbidx;
    
    // Occupation number
    double occno=occs[locorb[0]];
    
    for(size_t io=locorb.size()-1;io<locorb.size();io--)
      // Degeneracy in occupation?
      if(fabs(occs[locorb[io]]-occno)<1e-6) {
	// Orbitals are degenerate; add to current batch
	  orbidx.push_back(locorb[io]);
	  locorb.erase(locorb.begin()+io);
      }
    
    std::sort(orbidx.begin(),orbidx.end());
    
    // Collect orbitals
    arma::mat Cwrk(C.n_rows,orbidx.size());
    for(size_t io=0;io<orbidx.size();io++)
      Cwrk.col(io)=C.col(orbidx[io]);
    
    // Localizing matrix
    arma::cx_mat U(orbidx.size(),orbidx.size());
    U.eye();
    double measure=1e-7;
    
    // Run localization
    printf("Localizing orbitals:");
    for(size_t io=0;io<orbidx.size();io++)
      printf(" %i",(int) orbidx[io]+1);
    printf("\n");
    
    localize(basis,Cwrk,measure,U,true);
    
    // Transform orbitals
    arma::mat Cloc=arma::real(Cwrk*U);
    
    // Update orbitals
    for(size_t io=0;io<orbidx.size();io++)
      C.col(orbidx[io])=Cloc.col(io);
  }
}


void localize(const BasisSet & basis, arma::mat & C, std::vector<double> occs, bool virt) {
  // Run localization, occupied space
  localize_wrk(basis,C,occs);

  // Run localization, virtual space
  if(virt) {
    for(size_t i=0;i<occs.size();i++)
      if(occs[i]==0.0)
	occs[i]=1.0;
      else
	occs[i]=0.0;
    localize_wrk(basis,C,occs);
  }
}


int main(int argc, char **argv) {
#ifdef _OPENMP
  printf("ERKALE - Localization from Hel, OpenMP version, running on %i cores.\n",omp_get_max_threads());
#else
  printf("ERKALE - Localization from Hel, serial version.\n");
#endif
  print_copyright();
  print_license();

  if(argc!=2 && argc!=3) {
    printf("Usage: %s checkpoint (dovirtual)\n",argv[0]);
    return 1;
  }

  int virt=0;
  if(argc==3)
    virt=atoi(argv[2]);

  // Open checkpoint in read-write mode, don't truncate
  Checkpoint chkpt(argv[1],true,false);
    
  // Basis set
  BasisSet basis;
  chkpt.read(basis);

  // Restricted run?
  bool restr;
  chkpt.read("Restricted",restr);

  if(restr) {
    // Orbitals
    arma::mat C;
    chkpt.read("C",C);

    // Occupation numbers
    std::vector<double> occs;
    chkpt.read("occs",occs);

    // Run localization
    localize(basis,C,occs,virt);

    chkpt.write("C",C);

  } else {
    // Orbitals
    arma::mat Ca, Cb;
    chkpt.read("Ca",Ca);
    chkpt.read("Cb",Cb);

    // Occupation numbers
    std::vector<double> occa, occb;
    chkpt.read("occa",occa);
    chkpt.read("occb",occb);

    // Run localization
    localize(basis,Ca,occa,virt);
    localize(basis,Cb,occb,virt);

    chkpt.write("Ca",Ca);
    chkpt.write("Cb",Cb);
  }

  return 0;
}