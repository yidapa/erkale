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

#include "checkpoint.h"
#include "basislibrary.h"
#include "global.h"
#include "mathf.h"
#include "scf.h"
#include "solidharmonics.h"
#include "timer.h"
#include "xyzutils.h"
#include "emd/emd.h"
#include "emd/emd_gto.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

/// Relative tolerance in total energy
const double tol=1e-6;
/// Absolute tolerance in orbital energies
const double otol=1e-5;
/// Absolute tolerance for dipole moment
const double dtol=1e-5;

/// Absolute tolerance for normalization of basis functions
const double normtol=1e-10;

/// Check orthogonality of spherical harmonics up to
const int Lmax=10;
/// Tolerance for orthonormality
const double orthtol=500*DBL_EPSILON;

/// Initial DFT grid tolerance
const double dft_initialtol=1e-4;
/// Final DFT grid tolerance
const double dft_finaltol=1e-6;

/// Initial convergence settings (for DFT)
const convergence_t init_conv={1e-4, 1e-4, 1e-6};
/// Final convergence settings
const convergence_t final_conv={1e-6, 1e-6, 1e-8};

/// To compute references instead of running tests
//#define COMPUTE_REFERENCE

/// Test indices
void testind() {
  for(int am=0;am<max_am;am++) {
    int idx=0;
    for(int ii=0;ii<=am;ii++)
      for(int jj=0;jj<=ii;jj++) {
	int l=am-ii;
	int m=ii-jj;
	int n=jj;

	int ind=getind(l,m,n);
	if(ind!=idx) {
	  ERROR_INFO();
	  printf("l=%i, m=%i, n=%i, ind=%i, idx=%i.\n",l,m,n,ind,idx);
	  throw std::runtime_error("Indexing error.\n");
	}

	idx++;
      }
  }

  printf("Indices OK.\n");
}

/// Compute relative difference \f$ (x-y)/y \f$
double rel_diff(double x, double y) {
  return (x-y)/y;
}

/// Check if \f$ | (x - y)/y | < \tau \f$
bool rel_compare(double x, double y, double tau) {
  // Compute relative difference
  double d=rel_diff(x,y);

  if(fabs(d)<tau) {
    //    printf("%e vs %e, difference %e, ok\n",x,y,d);
    return 1;
  } else {
    //    printf("%e vs %e, difference %e, fail\n",x,y,d);
    return 0;
  }
}

/// Check if \f$ | (x - y) | < \tau \f$
bool abs_compare(double x, double y, double tau) {
  // Compute relative difference
  double d=fabs(x-y);

  if(fabs(d)<tau) {
    //    printf("%e vs %e, difference %e, ok\n",x,y,d);
    return 1;
  } else {
    //    printf("%e vs %e, difference %e, fail\n",x,y,d);
    return 0;
  }
}

/// Check that x == y within precision tau.
bool compare(const arma::vec & x, const arma::vec & y, double tau, size_t & nsucc, size_t & nfail) {
  if(x.n_elem!=y.n_elem)
    throw std::runtime_error("Error - differing amount of computed and reference orbital energies!\n");

  size_t N=std::min(x.n_elem,y.n_elem);

  nsucc=0;
  nfail=0;

  bool ok=1;
  for(size_t i=0;i<N;i++) {
    double d=x(i)-y(i);

    if(fabs(d)>tau) {
      //      printf("%e vs %e, difference %e, fail\n",x(i),y(i),d);
      ok=0;
      nfail++;
    } else {
      //      printf("%e vs %e, difference %e, ok\n",x(i),y(i),d);
      nsucc++;
    }
  }

  return ok;
}

/// Compute maximum difference of elements of x and y
double max_diff(const arma::vec & x, const arma::vec & y) {
  if(x.n_elem!=y.n_elem)
    throw std::runtime_error("Error - differing amount of computed and reference orbital energies!\n");

  double m=0;
  for(size_t i=0;i<x.n_elem;i++) {
    double d=fabs(x(i)-y(i));
    if(d>m)
      m=d;
  }

  return m;
}

/// Convert units from Ångström to a.u.
atom_t convert_to_bohr(const atom_t & in) {
  atom_t ret=in;

  ret.x*=ANGSTROMINBOHR;
  ret.y*=ANGSTROMINBOHR;
  ret.z*=ANGSTROMINBOHR;

  return ret;
}

// Possible statuses
const char * stat[]={"fail","ok"};

// Check proper normalization of basis
void check_norm(const BasisSet & bas) {
  size_t Nbf=bas.get_Nbf();
  arma::mat S=bas.overlap();

  for(size_t i=0;i<Nbf;i++)
    if(fabs(S(i,i)-1.0)>=normtol) {
      std::ostringstream oss;
      ERROR_INFO();
      fflush(stdout);
      oss << "Function " << i+1 << " is not normalized: norm is " << S(i,i) << "!.\n";
      throw std::runtime_error(oss.str());
    }
}


// Check normalization of spherical harmonics
double cartint(int l, int m, int n) {
  // J. Comput. Chem. 27, 1009-1019 (2006)
  // \int x^l y^m z^n d\Omega =
  // 4 \pi (l-1)!! (m-1)!! (n-1)!! / (l+m+n+1)!! if l,m,n even,
  // 0 otherwise

  if(l%2==1 || m%2==1 || n%2==1)
    return 0.0;

  return 4.0*M_PI*doublefact(l-1)*doublefact(m-1)*doublefact(n-1)/doublefact(l+m+n+1);
}

// Check norm of Y_{l,m}.
void check_sph_orthonorm(int lmax) {

  // Left hand value of l
  for(int ll=0;ll<=lmax;ll++)
    // Right hand value of l
    for(int lr=ll;lr<=lmax;lr++) {

      // Loop over m values
      for(int ml=-ll;ml<=ll;ml++) {
	// Get the coefficients
	std::vector<double> cl=calcYlm_coeff(ll,ml);

	// Form the list of cartesian functions
	std::vector<shellf_t> cartl(((ll+1)*(ll+2))/2);
	size_t n=0;
	for(int i=0; i<=ll; i++) {
	  int nx = ll - i;
	  for(int j=0; j<=i; j++) {
	    int ny = i-j;
	    int nz = j;

	    cartl[n].l=nx;
	    cartl[n].m=ny;
	    cartl[n].n=nz;
	    cartl[n].relnorm=cl[n];
	    n++;
	  }
	}

	for(int mr=-lr;mr<=lr;mr++) {
	  // Get the coefficients
	  std::vector<double> cr=calcYlm_coeff(lr,mr);

	  // Form the list of cartesian functions
	  std::vector<shellf_t> cartr(((lr+1)*(lr+2))/2);
	  size_t N=0;
	  for(int i=0; i<=lr; i++) {
	    int nx = lr - i;
	    for(int j=0; j<=i; j++) {
	      int ny = i-j;
	      int nz = j;

	      cartr[N].l=nx;
	      cartr[N].m=ny;
	      cartr[N].n=nz;
	      cartr[N].relnorm=cr[N];
	      N++;
	    }
	  }

	  // Compute dot product
	  double norm=0.0;
	  for(size_t i=0;i<cartl.size();i++)
	    for(size_t j=0;j<cartr.size();j++)
	      norm+=cartl[i].relnorm*cartr[j].relnorm*cartint(cartl[i].l+cartr[j].l,cartl[i].m+cartr[j].m,cartl[i].n+cartr[j].n);

	  if( (ll==lr) && (ml==mr) ) {
	    if(fabs(norm-1.0)>orthtol) {
	      fprintf(stderr,"Square norm of (%i,%i) is %e, deviation %e from unity!\n",ll,ml,norm,norm-1.0);
	      throw std::runtime_error("Wrong norm.\n");
	    }
	  } else {
	    if(fabs(norm)>orthtol) {
	      fprintf(stderr,"Inner product of (%i,%i) and (%i,%i) is %e!\n",ll,ml,lr,mr,norm);
	      throw std::runtime_error("Functions not orthogonal.\n");
	    }
	  }
	}
      }
    }
}

/// Test checkpoints
void test_checkpoint() {
  // Temporary file name
  char *tmpfile=tempnam("./",".chk");

  {
    // Dummy checkpoint
    Checkpoint chkpt(tmpfile,true);
    
    // Size of vectors and matrices
    size_t N=5000, M=300;
    
    /* Vectors */
    
    // Random vector
    arma::vec randvec=randu_mat(N,1);
    chkpt.write("randvec",randvec);
    
    arma::vec randvec_load;
    chkpt.read("randvec",randvec_load);
    
    double vecnorm=arma::norm(randvec-randvec_load,"fro")/N;
    if(vecnorm>DBL_EPSILON) {
      printf("Vector read/write norm %e.\n",vecnorm);
      ERROR_INFO();
      throw std::runtime_error("Error in vector read/write.\n");
    }
    
    // Complex vector
    arma::cx_vec crandvec=randu_mat(N,1)+std::complex<double>(0.0,1.0)*randu_mat(N,1);
    chkpt.cwrite("crandvec",crandvec);
    arma::cx_vec crandvec_load;
    chkpt.cread("crandvec",crandvec_load);
    
    double cvecnorm=arma::norm(crandvec-crandvec_load,"fro")/N;
    if(cvecnorm>DBL_EPSILON) {
      printf("Complex vector read/write norm %e.\n",cvecnorm);
      ERROR_INFO();
      throw std::runtime_error("Error in complex vector read/write.\n");
    }
    
    /* Matrices */
    arma::mat randmat=randn_mat(N,M);
    chkpt.write("randmat",randmat);
    arma::mat randmat_load;
    chkpt.read("randmat",randmat_load);

    double matnorm=arma::norm(randmat-randmat_load,"fro")/(M*N);
    if(matnorm>DBL_EPSILON) {
      printf("Matrix read/write norm %e.\n",matnorm);
      ERROR_INFO();
      throw std::runtime_error("Error in matrix read/write.\n");
    }

  }

  remove(tmpfile);
  free(tmpfile);
}


/// Test checkpoints
void test_checkpoint_basis(const BasisSet & bas) {
  // Temporary file name
  char *tmpfile=tempnam("./",".chk");

  {
    // Dummy checkpoint
    Checkpoint chkpt(tmpfile,true);
    
    // Write basis
    chkpt.write(bas);

    // Read basis
    BasisSet loadbas;
    chkpt.read(loadbas);

    // Get overlap matrices
    arma::mat S=bas.overlap();
    arma::mat Sload=loadbas.overlap();

    double matnorm=rms_norm(S-Sload);
    if(matnorm>DBL_EPSILON) {
      printf("Basis set read-write error %e.\n",matnorm);
      ERROR_INFO();
      throw std::runtime_error("Error in basis set read/write.\n");
    }
  }

  remove(tmpfile);
  free(tmpfile);
}


/// Test restricted solution
#ifdef COMPUTE_REFERENCE
#define restr_test(at,baslib,set,Etot,Eorb,label,dipmom,emd) restr_test_run(at,baslib,set,Etot,Eorb,label,dipmom,emd); printf(#set ".set_string(\"Method\",\"%s\");\n",set.get_string("Method").c_str()); printf("restr_test(" #at "," #baslib "," #set "," #Etot "," #Eorb "," #label "," #dipmom "," #emd ");\n\n");
#else
#define restr_test(at,baslib,set,Etot,Eorb,label,dipmom,emd) restr_test_run(at,baslib,set,Etot,Eorb,label,dipmom,emd);
#endif
void restr_test_run(const std::vector<atom_t> & at, const BasisSetLibrary & baslib, Settings set, double Etot, const arma::vec & Eorb, const std::string & label, double dipmom, bool doemd) {
  Timer t;

#ifndef COMPUTE_REFERENCE
  printf("%s, ",label.c_str());
  fflush(stdout);
#endif

  // Construct the basis set
  BasisSet bas=construct_basis(at,baslib,set);
  // Check normalization of basis
  check_norm(bas);
  // and checkpoints
  test_checkpoint_basis(bas);

  // Temporary file name
  char *tmpfile=tempnam("./",".chk");
  set.set_string("SaveChk",tmpfile);

  // Run the calculation
  calculate(bas,set);

  // Density matrix
  arma::mat P;
  // The orbital energies
  arma::vec Eo;
  // and the total energy
  energy_t en;
  // and the amount of electrons
  int Nel;
  {
    // Open the checkpoint
    Checkpoint chkpt(tmpfile,false);

    // Load everything necessary
    chkpt.read("P",P);
    chkpt.read("E",Eo);
    chkpt.read(en);
    chkpt.read("Nel",Nel);
  }

  // Get rid of the temporary file
  remove(tmpfile);
  free(tmpfile);

  // Compute norm of density matrix
  arma::mat S=bas.overlap();
  double Nelnum=arma::trace(P*S);
  if(fabs(Nelnum-Nel)>1e-7) {
    bas.print(true);
    P.print("P");
    S.print("S");
    fprintf(stderr,"Nelnum=%e, Nel=%i, Nelnum-Nel=%e\n",Nelnum,Nel,Nelnum-Nel);
    throw std::runtime_error("Problem with density matrix.\n");
  }

  // Compute dipole moment
  double dip=dip_mom(P,bas);

  // Check EMD
  bool emdok=true;
  if(doemd) {
    GaussianEMDEvaluator eval(bas,P);
    EMD emd(&eval, &eval, Nel);
    emd.initial_fill(false);
    emd.find_electrons(false);
    emd.optimize_moments(false);

    // Get moments
    arma::mat mom=emd.moments();

    // Compare <p^2> with T and <p^0> with tr(P*S)
    emdok=((fabs(mom(4,1)-2.0*en.Ekin)<=10*mom(4,2)) && (fabs(mom(2,1)-arma::trace(P*S))<=10*mom(2,2)));

    if(!emdok) {
      printf("<p^2> = %e, 2Ekin=%e, diff %e vs %e\n",mom(4,1),2.0*en.Ekin,mom(4,1)-2.0*en.Ekin,mom(4,2));
      printf("<p^0> = %e, Tr(PS)=%e, diff %e vs %e\n",mom(2,1),arma::trace(P*S),mom(2,1)-arma::trace(P*S),mom(2,2));
    }
  }

#ifdef COMPUTE_REFERENCE
  if(!set.get_bool("Direct")) {
    printf("Etot=%.16e;\n",en.E);
    printf("dip=%.16e;\n",dip);
    printf("Eorb=\"");
    for(size_t i=0;i<Eo.n_elem;i++)
      printf("%.16e ",Eo(i));
    printf("\";\n");
  }

  // Dummy statements
  (void) emdok;
  (void) Etot;
  (void) Eorb;
  (void) label;
  (void) dipmom;

#else
  // Compare results
  bool Eok=1, Dok=1, ok=1;
  size_t nsucc=0, nfail=0;
  compare(Eo,Eorb,otol,nsucc,nfail); // Compare orbital energies
  Eok=rel_compare(en.E,Etot,tol); // Compare total energies
  Dok=abs_compare(dip,dipmom,dtol); // Compare dipole moments
  ok=(Eok && Dok && emdok);
  printf("E=%f %s, dp=%f %s, orbital energies %i ok, %i failed (%s)\n",en.E,stat[Eok],dip,stat[Dok],(int) nsucc, (int) nfail,t.elapsed().c_str());
  printf("Relative difference of total energy is %e, difference in dipole moment is %e.\nMaximum difference of orbital energy is %e.",rel_diff(en.E,Etot),dip-dipmom,max_diff(Eo,Eorb));
  if(doemd) {
    if(emdok)
      printf(" EMD ok.\n");
    else
      printf(" EMD failed.\n");
  } else
    printf("\n");

  if(!ok) {
    std::ostringstream oss;
    ERROR_INFO();
    fflush(stdout);
    oss << "Test " << label << " failed.\n";
    throw std::runtime_error(oss.str());
  }
#endif

  fflush(stdout);
}

/// Test unrestricted solution
#ifdef COMPUTE_REFERENCE
#define unrestr_test(at,baslib,set,Etot,Eorba,Eorbb,label,dipmom,emd) unrestr_test_run(at,baslib,set,Etot,Eorba,Eorbb,label,dipmom,emd);  printf(#set ".set_string(\"Method\",\"%s\");\n",set.get_string("Method").c_str()); printf("unrestr_test(" #at "," #baslib "," #set "," #Etot "," #Eorba "," #Eorbb "," #label "," #dipmom "," #emd ");\n\n");
#else
#define unrestr_test(at,baslib,set,Etot,Eorba,Eorbb,label,dipmom,emd) unrestr_test_run(at,baslib,set,Etot,Eorba,Eorbb,label,dipmom,emd);
#endif
void unrestr_test_run(const std::vector<atom_t> & at, const BasisSetLibrary & baslib, Settings set, double Etot, const arma::vec & Eorba, const arma::vec & Eorbb, const std::string & label, double dipmom, bool doemd) {
  Timer t;

#ifndef COMPUTE_REFERENCE
  printf("%s, ",label.c_str());
  fflush(stdout);
#endif

  // Construct basis set
  BasisSet bas=construct_basis(at,baslib,set);
  // Check normalization of basis
  check_norm(bas);

  // Temporary file name
  char *tmpfile=tempnam("./",".chk");
  set.set_string("SaveChk",tmpfile);

  // Run the calculation
  calculate(bas,set);
  // Density matrix
  arma::mat P;
  // The orbital energies
  arma::vec Eao, Ebo;
  // and the total energy
  energy_t en;
  // and the amount of electrons
  int Nel;
  {
    // Open the checkpoint
    Checkpoint chkpt(tmpfile,false);

    // Load everything necessary
    chkpt.read("P",P);
    chkpt.read("Ea",Eao);
    chkpt.read("Eb",Ebo);
    chkpt.read(en);
    chkpt.read("Nel",Nel);
  }

  // Get rid of the temporary file
  remove(tmpfile);
  free(tmpfile);

  // Compute dipole moment
  double dip=dip_mom(P,bas);

  // Check EMD
  bool emdok=true;
  if(doemd) {
    arma::mat S=bas.overlap();
    GaussianEMDEvaluator eval(bas,P);
    EMD emd(&eval, &eval, Nel);
    emd.initial_fill(false);
    emd.find_electrons(false);
    emd.optimize_moments(false);

    // Get moments
    arma::mat mom=emd.moments();

    // Compare <p^2> with T and <p^0> with tr(P*S)
    emdok=((fabs(mom(4,1)-2.0*en.Ekin)<=10*mom(4,2)) && (fabs(mom(2,1)-arma::trace(P*S))<=10*mom(2,2)));
  }

#ifdef COMPUTE_REFERENCE
  if(!set.get_bool("Direct")) {
    printf("Etot=%.16e;\n",en.E);
    printf("dip=%.16e;\n",dip);
    printf("Eorba=\"");
    for(size_t i=0;i<Eao.n_elem;i++)
      printf("%.16e ",Eao(i));
    printf("\";\n");
    printf("Eorbb=\"");
    for(size_t i=0;i<Ebo.n_elem;i++)
      printf("%.16e ",Ebo(i));
    printf("\";\n");
  }

  // Dummy statements
  (void) emdok;
  (void) Etot;
  (void) Eorba;
  (void) Eorbb;
  (void) label;
  (void) dipmom;

#else
  // Compare results
  bool Eok=1, Dok=1, ok=1;
  size_t nsucca=0, nfaila=0;
  size_t nsuccb=0, nfailb=0;
  compare(Eao,Eorba,otol,nsucca,nfaila); // Compare orbital energies
  compare(Ebo,Eorbb,otol,nsuccb,nfailb); // Compare orbital energies
  size_t nsucc=nsucca+nsuccb;
  size_t nfail=nfaila+nfailb;

  Eok=rel_compare(en.E,Etot,tol); // Compare total energies
  Dok=abs_compare(dip,dipmom,dtol); // Compare dipole moments
  ok=(Eok && Dok && emdok);
  printf("E=%f %s, dp=%f %s, orbital energies %i ok, %i failed (%s)\n",en.E,stat[Eok],dip,stat[Dok],(int) nsucc, (int) nfail,t.elapsed().c_str());
  printf("Relative difference of total energy is %e, difference in dipole moment is %e.\nMaximum differences of orbital energies are %e and %e.",rel_diff(en.E,Etot),dip-dipmom,max_diff(Eao,Eorba),max_diff(Ebo,Eorbb));
  if(doemd) {
    if(emdok)
      printf(" EMD ok.\n");
    else
      printf(" EMD failed.\n");
  } else
    printf("\n");

  if(!ok) {
    std::ostringstream oss;
    ERROR_INFO();
    fflush(stdout);
    oss << "Test " << label << " failed.\n";
    throw std::runtime_error(oss.str());
  }
#endif

  fflush(stdout);
}


/// Run unit tests by comparing calculations to ones that should be OK
int main(void) {
  testind();

  // Initialize libint
  init_libint_base();

  // First, check norms of spherical harmonics.
  check_sph_orthonorm(Lmax);
  printf("Solid harmonics OK.\n");

  // Then, check checkpoint utilities
  test_checkpoint();
  printf("Checkpointing OK.\n");

  // Load basis sets

  // Redirect stderr to file, since scf routines print out info there.
#ifdef COMPUTE_REFERENCE
  FILE *errors;
  errors=freopen("errors.log","w",stderr);
  // Avoid unused variable warning
  (void) errors;
#endif

  printf("****** Loading basis sets *******\n");

  BasisSetLibrary b3_21G;
  b3_21G.load_gaussian94("3-21G");

  BasisSetLibrary b6_31Gpp;
  b6_31Gpp.load_gaussian94("6-31G**");

  BasisSetLibrary cc_pVDZ;
  cc_pVDZ.load_gaussian94("cc-pVDZ");

  BasisSetLibrary cc_pVTZ;
  cc_pVTZ.load_gaussian94("cc-pVTZ");

  BasisSetLibrary cc_pVQZ;
  cc_pVQZ.load_gaussian94("cc-pVQZ");

  BasisSetLibrary aug_cc_pVDZ;
  aug_cc_pVDZ.load_gaussian94("aug-cc-pVDZ");

  /*
    BasisSetLibrary cc_pV5Z;
    cc_pV5Z.load_gaussian94("cc-pV5Z");

    BasisSetLibrary cc_pV6Z;
    cc_pV6Z.load_gaussian94("cc-pV6Z");

    BasisSetLibrary aug_cc_pVTZ;
    aug_cc_pVTZ.load_gaussian94("aug-cc-pVTZ");

    BasisSetLibrary aug_cc_pVQZ;
    aug_cc_pVQZ.load_gaussian94("aug-cc-pVQZ");
  */

  // Helper structure
  atom_t at;

  // Neon atom
  std::vector<atom_t> Ne;
  at.el="Ne"; at.x=0.0; at.y=0.0; at.z=0.0; Ne.push_back(convert_to_bohr(at));

  // Oxyen atom
  std::vector<atom_t> O;
  at.el="O"; at.x=0.0; at.y=0.0; at.z=0.0; O.push_back(convert_to_bohr(at));

  // Hydrogen molecule
  std::vector<atom_t> H2;
  at.el="H"; at.x=0.0; at.y=0.0; at.z=-0.37; H2.push_back(convert_to_bohr(at));
  at.el="H"; at.x=0.0; at.y=0.0; at.z= 0.37; H2.push_back(convert_to_bohr(at));

  // Water monomer optimized at B3LYP/aug-cc-pVTZ level
  std::vector<atom_t> h2o;
  at.el="O"; at.x= 0.000000; at.y= 0.117030; at.z=0.000000; h2o.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 0.763404; at.y=-0.468123; at.z=0.000000; h2o.push_back(convert_to_bohr(at));
  at.el="H"; at.x=-0.763404; at.y=-0.468123; at.z=0.000000; h2o.push_back(convert_to_bohr(at));

  // Cadmium complex
  std::vector<atom_t> cdcplx;
  at.el="Cd"; at.x= 0.000000; at.y= 0.000000; at.z= 0.000000; cdcplx.push_back(convert_to_bohr(at));
  at.el="N";  at.x= 0.000000; at.y= 0.000000; at.z=-2.260001; cdcplx.push_back(convert_to_bohr(at));
  at.el="N";  at.x=-0.685444; at.y= 0.000000; at.z=-4.348035; cdcplx.push_back(convert_to_bohr(at));
  at.el="C";  at.x= 0.676053; at.y= 0.000000; at.z=-4.385069; cdcplx.push_back(convert_to_bohr(at));
  at.el="C";  at.x= 1.085240; at.y= 0.000000; at.z=-3.091231; cdcplx.push_back(convert_to_bohr(at));
  at.el="C";  at.x=-1.044752; at.y= 0.000000; at.z=-3.060220; cdcplx.push_back(convert_to_bohr(at));
  at.el="H";  at.x= 1.231530; at.y= 0.000000; at.z=-5.300759; cdcplx.push_back(convert_to_bohr(at));
  at.el="H";  at.x= 2.088641; at.y= 0.000000; at.z=-2.711077; cdcplx.push_back(convert_to_bohr(at));
  at.el="H";  at.x=-2.068750; at.y= 0.000000; at.z=-2.726515; cdcplx.push_back(convert_to_bohr(at));
  at.el="H";  at.x=-1.313170; at.y= 0.000000; at.z=-5.174718; cdcplx.push_back(convert_to_bohr(at));

  // 1-decanol
  std::vector<atom_t> decanol;
  at.el="C"; at.x= 3.951300; at.y= 3.953900; at.z= 3.422300; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 5.374200; at.y= 3.650100; at.z= 2.991900; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 5.462800; at.y= 3.376900; at.z= 1.498000; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 3.868100; at.y= 4.261800; at.z= 4.909900; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 6.848500; at.y= 2.886000; at.z= 1.103900; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 2.476900; at.y= 4.734000; at.z= 5.303900; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 7.148200; at.y= 1.524300; at.z= 1.712000; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 2.137100; at.y= 6.069500; at.z= 4.659400; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 8.591100; at.y= 1.087300; at.z= 1.479900; decanol.push_back(convert_to_bohr(at));
  at.el="C"; at.x= 0.698900; at.y= 6.461200; at.z= 4.906200; decanol.push_back(convert_to_bohr(at));
  at.el="O"; at.x= 9.420700; at.y= 1.797400; at.z= 2.371700; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 3.287600; at.y= 3.101000; at.z= 3.176500; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 3.544800; at.y= 4.820400; at.z= 2.851400; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 5.764700; at.y= 2.762000; at.z= 3.541600; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 6.048100; at.y= 4.485600; at.z= 3.267600; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 5.215200; at.y= 4.295200; at.z= 0.930600; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 4.700900; at.y= 2.627700; at.z= 1.202600; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 4.616500; at.y= 5.034600; at.z= 5.179100; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 4.142500; at.y= 3.363300; at.z= 5.496700; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 6.926800; at.y= 2.833000; at.z= 0.000000; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 7.613500; at.y= 3.620500; at.z= 1.424900; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 2.408100; at.y= 4.818800; at.z= 6.406100; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 1.724700; at.y= 3.972500; at.z= 5.015100; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 6.912400; at.y= 1.558800; at.z= 2.801200; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 6.470000; at.y= 0.761200; at.z= 1.282600; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 2.344300; at.y= 6.002200; at.z= 3.566900; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 2.817300; at.y= 6.856200; at.z= 5.040700; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 8.698800; at.y= 0.000000; at.z= 1.663000; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 8.897200; at.y= 1.276900; at.z= 0.431600; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 0.461600; at.y= 7.423200; at.z= 4.435000; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 0.000000; at.y= 5.716800; at.z= 4.502200; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x= 0.486000; at.y= 6.559000; at.z= 5.979000; decanol.push_back(convert_to_bohr(at));
  at.el="H"; at.x=10.311700; at.y= 1.558300; at.z= 2.157100; decanol.push_back(convert_to_bohr(at));

  // Construct settings
  Settings sph;
  sph.add_scf_settings();
  sph.add_string("SaveChk","Save checkpoint to","erkale.chk");
  sph.add_string("LoadChk","Load checkpoint","");
  sph.add_bool("FreezeCore","Freeze the cores of the atoms",false);
  sph.add_bool("ForcePol","Force polarized calculation?",false);
  sph.set_bool("Verbose",false);
  // Use core guess, and no density fitting for tests by default
  sph.set_string("Guess","Core");
  sph.set_bool("DensityFitting",false);

  // No spherical harmonics
  Settings cart(sph);
  cart.set_bool("UseLM",false);

  // Direct calculation
  Settings direct(sph);
  direct.set_bool("Direct",true);

  // Polarized calculation
  Settings pol(sph);
  pol.set_int("Multiplicity",3);

  // DFT tests

  // Settings for DFT
  Settings dftsph(sph); // Normal settings
  dftsph.add_dft_settings();
  dftsph.set_bool("DensityFitting",true);

  Settings dftcart(cart); // Cartesian basis
  dftcart.add_dft_settings();
  dftcart.set_bool("DensityFitting",true);

  Settings dftnofit(dftsph); // No density fitting
  dftnofit.set_bool("DensityFitting",false);

  Settings dftcart_nofit(dftcart);
  dftcart_nofit.set_bool("DensityFitting",false);

  Settings dftdirect(dftsph); // Direct calculation
  dftdirect.set_bool("Direct",true);

  Settings dftpol(pol); // Polarized calculation
  dftpol.add_dft_settings();
  dftpol.set_double("DFTInitialTol",1e-4);

  Settings dftpol_nofit(dftpol); // Polarized calculation, no density fitting
  dftpol_nofit.set_bool("DensityFitting",false);

  printf("****** Running calculations *******\n");
  Timer t;

  // Reference total energy
  double Etot;
  // Reference dipole moment
  double dip;
  // Reference orbital energies
  arma::vec Eorb;
  arma::vec Eorba;
  arma::vec Eorbb;

Etot=-1.2848877555174067e+02;
dip=1.4435368920606835e-15;
Eorb="-3.2765635418561679e+01 -1.9187982340180849e+00 -8.3209725199365270e-01 -8.3209725199364915e-01 -8.3209725199364648e-01 1.6945577282676405e+00 1.6945577282676423e+00 1.6945577282676483e+00 2.1594249508220069e+00 5.1967114014294031e+00 5.1967114014294040e+00 5.1967114014294067e+00 5.1967114014294093e+00 5.1967114014294093e+00 ";
sph.set_string("Method","HF");
restr_test(Ne,cc_pVDZ,sph,Etot,Eorb,"Neon, HF/cc-pVDZ",dip,true);

Etot=-1.2848886617203749e+02;
dip=1.4892866464273328e-16;
Eorb="-3.2765400790493644e+01 -1.9190111524789615e+00 -8.3228219664038128e-01 -8.3228219664037995e-01 -8.3228219664037706e-01 1.6944247000888928e+00 1.6944247000888970e+00 1.6944247000889030e+00 1.9905987688841935e+00 5.1964246005617438e+00 5.1964246005617465e+00 5.1964246005617598e+00 5.1964246005617607e+00 5.1964246005617607e+00 1.0383358436726828e+01 ";
cart.set_string("Method","HF");
restr_test(Ne,cc_pVDZ,cart,Etot,Eorb,"Neon, HF/cc-pVDZ cart",dip,false);

Etot=-1.2853186163632131e+02;
dip=5.8312819502639884e-16;
Eorb="-3.2769110712152042e+01 -1.9270833031683197e+00 -8.4541550865788417e-01 -8.4541550865787585e-01 -8.4541550865787496e-01 1.0988680360351128e+00 1.0988680360351157e+00 1.0988680360351200e+00 1.4176388082541262e+00 2.8142175664037499e+00 2.8142175664037548e+00 2.8142175664037605e+00 2.8142175664037632e+00 2.8142175664037690e+00 6.1558667278236863e+00 6.1558667278236969e+00 6.1558667278237138e+00 9.6473695832596817e+00 9.6473695832596977e+00 9.6473695832596995e+00 9.6473695832597137e+00 9.6473695832597244e+00 9.6473695832597297e+00 9.6473695832597368e+00 1.1227312686901302e+01 1.1227312686901307e+01 1.1227312686901314e+01 1.1227312686901330e+01 1.1227312686901342e+01 1.1744558071719036e+01 ";
sph.set_string("Method","HF");
restr_test(Ne,cc_pVTZ,sph,Etot,Eorb,"Neon, HF/cc-pVTZ",dip,false);

Etot=-1.2853200998517821e+02;
dip=2.4329736205285537e-15;
Eorb="-3.2769827640709053e+01 -1.9274545143567681e+00 -8.4572301628317992e-01 -8.4572301628317226e-01 -8.4572301628315216e-01 8.8038911948251719e-01 1.0282198420182489e+00 1.0282198420182707e+00 1.0282198420182722e+00 2.8138968477417854e+00 2.8138968477418000e+00 2.8138968477418045e+00 2.8138968477418058e+00 2.8138968477418200e+00 4.1362240337699347e+00 4.6398467080899151e+00 4.6398467080900350e+00 4.6398467080901815e+00 9.6470056650341380e+00 9.6470056650341416e+00 9.6470056650341629e+00 9.6470056650341700e+00 9.6470056650341860e+00 9.6470056650341895e+00 9.6470056650342055e+00 1.1226914499834864e+01 1.1226914499834871e+01 1.1226914499834875e+01 1.1226914499834894e+01 1.1226914499834907e+01 1.1317534803037177e+01 1.1317534803037349e+01 1.1317534803037709e+01 1.6394442679994782e+01 2.8816114661587473e+01 ";
cart.set_string("Method","HF");
restr_test(Ne,cc_pVTZ,cart,Etot,Eorb,"Neon, HF/cc-pVTZ cart",dip,false);

Etot=-1.2854346965912171e+02;
dip=2.7477945390451142e-15;
Eorb="-3.2771496243111322e+01 -1.9293376422732940e+00 -8.4895896378891900e-01 -8.4895896378888902e-01 -8.4895896378888402e-01 8.0890413586315835e-01 8.0890413586317678e-01 8.0890413586321575e-01 9.3559988660381355e-01 1.9978112775310954e+00 1.9978112775311092e+00 1.9978112775311148e+00 1.9978112775311316e+00 1.9978112775311374e+00 3.9328189019111854e+00 3.9328189019111943e+00 3.9328189019112134e+00 5.8106845378505456e+00 5.9042211348650326e+00 5.9042211348650468e+00 5.9042211348650557e+00 5.9042211348650664e+00 5.9042211348650735e+00 5.9042211348650850e+00 5.9042211348650993e+00 6.7616951495719997e+00 6.7616951495720077e+00 6.7616951495720299e+00 6.7616951495720494e+00 6.7616951495720539e+00 1.4903626157334642e+01 1.4903626157334665e+01 1.4903626157334669e+01 1.4903626157334674e+01 1.4903626157334696e+01 1.4903626157334701e+01 1.4903626157334717e+01 1.4903626157334717e+01 1.4903626157334722e+01 1.5804420577980073e+01 1.5804420577980075e+01 1.5804420577980245e+01 1.9794585636890652e+01 1.9794585636890666e+01 1.9794585636890709e+01 1.9794585636890726e+01 1.9794585636890741e+01 1.9794585636890762e+01 1.9794585636890780e+01 2.0954549898741444e+01 2.0954549898741472e+01 2.0954549898741519e+01 2.0954549898741536e+01 2.0954549898741647e+01 6.6550956466383184e+01 ";
sph.set_string("Method","HF");
restr_test(Ne,cc_pVQZ,sph,Etot,Eorb,"Neon, HF/cc-pVQZ",dip,false);

Etot=-1.2854353449722294e+02;
dip=1.7596570386005006e-15;
Eorb="-3.2771625129700624e+01 -1.9294942843276175e+00 -8.4906688477862047e-01 -8.4906688477861170e-01 -8.4906688477860959e-01 5.8690441461999887e-01 7.1271797740018561e-01 7.1271797740023557e-01 7.1271797740027754e-01 1.9879845920567851e+00 1.9879845920567909e+00 1.9879845920567973e+00 1.9879845920568091e+00 1.9879845920568122e+00 2.5105148501578611e+00 2.7214792300253277e+00 2.7214792300261448e+00 2.7214792300262007e+00 5.9040962886937942e+00 5.9040962886938431e+00 5.9040962886938546e+00 5.9040962886938555e+00 5.9040962886938884e+00 5.9040962886939239e+00 5.9040962886939363e+00 6.4115733389271821e+00 6.5684069300985541e+00 6.5684069300985639e+00 6.5684069300985701e+00 6.5684069300986305e+00 6.5684069300986652e+00 6.7659165999902626e+00 6.7659165999913631e+00 6.7659165999914999e+00 1.4004805313393502e+01 1.4903514354528957e+01 1.4903514354528992e+01 1.4903514354529001e+01 1.4903514354529028e+01 1.4903514354529056e+01 1.4903514354529090e+01 1.4903514354529108e+01 1.4903514354529113e+01 1.4903514354529133e+01 1.8145155385055318e+01 1.8145155385056160e+01 1.8145155385056658e+01 1.8145155385057031e+01 1.8145155385057887e+01 1.8540067452283736e+01 1.8540067452284212e+01 1.8540067452284642e+01 1.9794449045074092e+01 1.9794449045074110e+01 1.9794449045074128e+01 1.9794449045074142e+01 1.9794449045074174e+01 1.9794449045074199e+01 1.9794449045074220e+01 2.9727979556776525e+01 3.9089870735998431e+01 3.9089870736019819e+01 3.9089870736030790e+01 3.9089870736037255e+01 3.9089870736045292e+01 3.9551871549889093e+01 3.9551871549901385e+01 3.9551871549915269e+01 5.8376821811612182e+01 2.0568373997964792e+02 ";
cart.set_string("Method","HF");
restr_test(Ne,cc_pVQZ,cart,Etot,Eorb,"Neon, HF/cc-pVQZ cart",dip,true);

Etot=-7.4792166058284067e+01;
dip=3.4344830232595253e-16;
Eorba="-2.0701943695993123e+01 -1.4074987051338466e+00 -6.9587846183402946e-01 -6.9587846183402702e-01 -5.9925089560884759e-01 1.0662784436084738e+00 1.0662784436084756e+00 1.1271102280694969e+00 1.3256591041237153e+00 2.7668883663420645e+00 2.7668883663420667e+00 2.8335371595476402e+00 2.8335371595476460e+00 2.8571229225448382e+00 ";
Eorbb="-2.0622950399976215e+01 -1.0713933178520889e+00 -5.1253576227370423e-01 1.3264309954546061e-01 1.3264309954546122e-01 1.1716793667478982e+00 1.2890217069719898e+00 1.2890217069719907e+00 1.4286221065862399e+00 2.9578090750437096e+00 2.9690360661897794e+00 2.9690360661897834e+00 3.0082343864140899e+00 3.0082343864140966e+00 ";
pol.set_string("Method","HF");
unrestr_test(O,cc_pVDZ,pol,Etot,Eorba,Eorbb,"Oxygen, HF/cc-pVDZ polarized",dip,false);

Etot=-7.4769675003679453e+01;
dip=2.5451416079860364e-16;
Eorba="-2.0798493205901305e+01 -1.4321166621871166e+00 -6.8986628426742402e-01 -6.8986628426742092e-01 -6.4287405686663890e-01 1.0523318685870862e+00 1.0523318685870870e+00 1.0966478415083978e+00 1.2997635370041067e+00 2.7385640480159665e+00 2.7385640480159679e+00 2.8008025715006801e+00 2.8008025715006837e+00 2.8213563576178160e+00 ";
Eorbb="-2.0739294814212233e+01 -1.1284737599757098e+00 -5.6490947011784964e-01 1.0441566742595045e-01 1.0441566742595090e-01 1.1287297708405049e+00 1.2309666216013506e+00 1.2309666216013526e+00 1.3907737627978429e+00 2.9183833001822261e+00 2.9294797180622481e+00 2.9294797180622525e+00 2.9636114691955004e+00 2.9636114691955009e+00 ";
pol.set_string("Method","ROHF");
unrestr_test(O,cc_pVDZ,pol,Etot,Eorba,Eorbb,"Oxygen, ROHF/cc-pVDZ",dip,false);

Etot=-7.5068497045081543e+01;
dip=7.0007840861678976e-17;
Eorba="-1.9272592789384010e+01 -1.0033703517619745e+00 -4.4143749622045114e-01 -4.4143749307442420e-01 -3.6470598458017289e-01 8.1822881898544975e-01 8.1822882031943034e-01 8.6597763543497630e-01 1.0724567996756316e+00 2.4418113868295102e+00 2.4418113876845911e+00 2.5002406389128549e+00 2.5002406423418515e+00 2.5208884480453491e+00 ";
Eorbb="-1.9222758933790324e+01 -8.4514459382942397e-01 -3.1322750049603143e-01 -1.5995821223224227e-01 -1.5995773851181089e-01 8.9585454605052528e-01 9.3243704496896107e-01 9.3243841664188920e-01 1.1541887867455800e+00 2.5911584613858438e+00 2.5911586345475315e+00 2.5984375747354553e+00 2.6155688435860958e+00 2.6155691869069866e+00 ";
dftpol_nofit.set_string("Method","hyb_gga_xc_b3lyp");
unrestr_test(O,cc_pVDZ,dftpol_nofit,Etot,Eorba,Eorbb,"Oxygen, B3LYP/cc-pVDZ polarized",dip,false);

Etot=-1.1287000934441984e+00;
dip=1.5642885608343942e-15;
Eorb="-5.9241098912564860e-01 1.9744005746884530e-01 4.7932104726693309e-01 9.3732369228383794e-01 1.2929037097169978e+00 1.2929037097169984e+00 1.9570226089424216e+00 2.0435200542818750e+00 2.0435200542818759e+00 3.6104742345513303e+00 ";
sph.set_string("Method","HF");
restr_test(H2,cc_pVDZ,sph,Etot,Eorb,"Hydrogen molecule, HF/cc-pVDZ",dip,false);

Etot=-1.1676141283675028e+00;
dip=5.3291101326793995e-14;
Eorb="-3.9201514888589417e-01 3.6521072523743606e-02 2.9071323764621282e-01 6.5833097792975903e-01 9.7502317133004190e-01 9.7502317133004646e-01 1.6066122955337188e+00 1.7001809433754596e+00 1.7001809433754633e+00 3.1926496372470909e+00 ";
dftsph.set_string("Method","lda_x-lda_c_vwn_rpa");
restr_test(H2,cc_pVDZ,dftsph,Etot,Eorb,"Hydrogen molecule, SVWN(RPA)/cc-pVDZ",dip,false);

Etot=-1.1603962482031753e+00;
dip=4.8619633367467044e-14;
Eorb="-3.7849163125977237e-01 5.3523362639534812e-02 3.0277113010558249e-01 6.6374838851958373e-01 9.9246490733396076e-01 9.9246490733396509e-01 1.6235426909719661e+00 1.7198881174136378e+00 1.7198881174136407e+00 3.2019311302451947e+00 ";
dftsph.set_string("Method","gga_x_pbe-gga_c_pbe");
restr_test(H2,cc_pVDZ,dftsph,Etot,Eorb,"Hydrogen molecule, PBEPBE/cc-pVDZ",dip,false);

Etot=-7.6056825377225593e+01;
dip=7.9472744276874829e-01;
Eorb="-2.0555281284436191e+01 -1.3428635581881403e+00 -7.0828437270164446e-01 -5.7575384678426200e-01 -5.0391498180529337e-01 1.4187951465435203e-01 2.0351537856745977e-01 5.4324870285539717e-01 5.9753586610169185e-01 6.6949546704656226e-01 7.8747678832686629e-01 8.0274150051143889e-01 8.0481260684507294e-01 8.5898803204152485e-01 9.5702121612019131e-01 1.1344778940622866e+00 1.1928203488145193e+00 1.5241753050882469e+00 1.5579529853127803e+00 2.0324408675126748e+00 2.0594682909626054e+00 2.0654407641694146e+00 2.1686553940739159e+00 2.2363161848524631e+00 2.5909431251492063e+00 2.9581971168606840e+00 3.3610002602115596e+00 3.4914002724773150e+00 3.5741938436058329e+00 3.6463660378170735e+00 3.7977214193539579e+00 3.8739670169202336e+00 3.8824466748504896e+00 3.9569498219465662e+00 4.0199059026375581e+00 4.0760332586986694e+00 4.1862021889925805e+00 4.3092789354737846e+00 4.3875716367832069e+00 4.5640073731841975e+00 4.6817931153441643e+00 4.8550947781427505e+00 5.1380848582693481e+00 5.2500191158527132e+00 5.5275547738585677e+00 6.0402478768076469e+00 6.5453259362984699e+00 6.9113516599311868e+00 6.9366142636390498e+00 7.0003720369213269e+00 7.0078239220784351e+00 7.0609382550385469e+00 7.1598075600051123e+00 7.2256524644090181e+00 7.4561719737015295e+00 7.7799625467767344e+00 8.2653639941755692e+00 1.2804358854138068e+01 ";
sph.set_string("Method","HF");
restr_test(h2o,cc_pVTZ,sph,Etot,Eorb,"Water, HF/cc-pVTZ",dip,true);

direct.set_string("Method","HF");
restr_test(h2o,cc_pVTZ,direct,Etot,Eorb,"Water, HF/cc-pVTZ direct",dip,false);

Etot=-7.6064480528901825e+01;
dip=7.8765852559032001e-01;
Eorb="-2.0560341493316596e+01 -1.3467109708922640e+00 -7.1286865340819383e-01 -5.7999183591919190e-01 -5.0759009329738514e-01 1.1677961649322943e-01 1.7061237310613497e-01 4.4878631112627221e-01 4.6240974120054934e-01 4.9860081839166087e-01 5.8389461716416924e-01 6.0602248995272778e-01 6.1386901262992877e-01 6.5509670218942440e-01 7.1940581420515326e-01 8.5193265926825823e-01 9.1760642725796271e-01 1.1091391711813636e+00 1.1559117527191221e+00 1.3479064842198669e+00 1.4144381954586611e+00 1.4776186111725542e+00 1.4856774092278802e+00 1.5814608159270527e+00 1.6854835716881027e+00 1.9096187743148207e+00 2.0727777019578704e+00 2.1976502963082263e+00 2.2888869958663491e+00 2.3588905629590187e+00 2.4246094527526165e+00 2.4837778780196742e+00 2.5224544346049003e+00 2.5800657946142560e+00 2.5803867670205700e+00 2.6507304490819998e+00 2.6683130232088423e+00 2.8407379947518514e+00 2.8643130329061615e+00 3.0412098303680843e+00 3.1190680431193272e+00 3.2889763516989374e+00 3.3518967320350748e+00 3.4467312460558803e+00 3.6214003282870664e+00 3.8285931980501329e+00 3.9968185328443702e+00 4.1278766620605936e+00 4.1879463042126952e+00 4.2176486592865690e+00 4.4343620887175579e+00 4.4925098807533423e+00 4.6832772417557864e+00 4.7403725803385814e+00 4.8079058224933640e+00 4.9140701220581180e+00 5.3503959760316011e+00 5.4039303374457317e+00 5.9860940825841702e+00 6.1030498791342982e+00 6.2449376398314094e+00 6.3029981800476662e+00 6.7000543793001190e+00 6.7926548550320938e+00 7.0589633235130398e+00 7.2683601238985922e+00 7.3171930255045075e+00 7.3671378813948403e+00 7.4371264991062809e+00 7.5184752948647340e+00 7.5458434087300956e+00 7.5694204738253763e+00 8.0046360196224899e+00 8.0708295744573402e+00 8.0987711874714687e+00 8.1338237901843478e+00 8.1523664193748484e+00 8.2695443413563279e+00 8.3150962695932318e+00 8.3485048857952346e+00 8.4164827915179465e+00 8.6181288293117575e+00 8.8336406091326136e+00 8.9048326524843446e+00 8.9437734440710575e+00 9.2166367010041803e+00 9.3761387927925224e+00 9.3791690862171926e+00 9.9423093877647553e+00 1.0035594105372642e+01 1.0257561213476848e+01 1.0425629824216507e+01 1.0646599821508335e+01 1.0757780269207244e+01 1.0806846320258858e+01 1.1272406011053342e+01 1.1390414021676927e+01 1.1595907195588794e+01 1.1644666360585591e+01 1.1693629520822261e+01 1.1844883877673793e+01 1.2158546424496949e+01 1.2320144576794815e+01 1.2398213793136600e+01 1.2413264235730084e+01 1.2465013639021217e+01 1.3602019539678913e+01 1.3763660841134584e+01 1.4247885734372069e+01 1.4614348064191816e+01 1.4639079465477462e+01 1.4826337839071392e+01 1.6435472097103613e+01 1.6799107186200136e+01 4.4322817454271004e+01 ";
sph.set_string("Method","HF");
restr_test(h2o,cc_pVQZ,sph,Etot,Eorb,"Water, HF/cc-pVQZ",dip,false);

direct.set_string("Method","HF");
restr_test(h2o,cc_pVQZ,direct,Etot,Eorb,"Water, HF/cc-pVQZ direct",dip,false);

Etot=-7.6372961223969938e+01;
dip=7.3249327858270985e-01;
Eorb="-1.8738953507100160e+01 -9.1586785684202843e-01 -4.7160107244322713e-01 -3.2507288070279516e-01 -2.4816656326287970e-01 8.4534565263456249e-03 8.1125364911131120e-02 3.3806711781857185e-01 3.7949127836901597e-01 4.6548870905144252e-01 5.4539135164314334e-01 5.9072338390687540e-01 5.9687803625007896e-01 6.4398044096816665e-01 7.4418029530168794e-01 8.8306549796821487e-01 9.7381887154089197e-01 1.2412762117035903e+00 1.2611338684124365e+00 1.6999121798043471e+00 1.7261707862385667e+00 1.7619584426859225e+00 1.8256258245235544e+00 1.9002743771654016e+00 2.1846823421941406e+00 2.5326905494993106e+00 2.9875043480643804e+00 3.1260225268639594e+00 3.1892007420217592e+00 3.2799583603324343e+00 3.2859664610319106e+00 3.4399388032091962e+00 3.5114853994823254e+00 3.5697879976695859e+00 3.6175833222173783e+00 3.6363368585815712e+00 3.6947695837101304e+00 3.9240129101571961e+00 3.9512251140904855e+00 4.1557724926477597e+00 4.1932317668601575e+00 4.4292385786197963e+00 4.6459620107230419e+00 4.7303864360810088e+00 4.9898268687329042e+00 5.4868943604049463e+00 5.9838983407363795e+00 6.2843629353000106e+00 6.2901656545438325e+00 6.3781111512012219e+00 6.4202760127329990e+00 6.4811650937209446e+00 6.5329728466701189e+00 6.6594412355143575e+00 6.8404601974366788e+00 7.1724946449741047e+00 7.6259352246519141e+00 1.1962240165446625e+01 ";
dftnofit.set_string("Method","gga_x_pbe-gga_c_pbe");
restr_test(h2o,cc_pVTZ,dftnofit,Etot,Eorb,"Water, PBEPBE/cc-pVTZ no fitting",dip,false);

Etot=-7.6373025078090350e+01;
dip=7.3223619222150083e-01;
Eorb="-1.8739067718669279e+01 -9.1594855803909792e-01 -4.7168659183776129e-01 -3.2515022698106016e-01 -2.4824135248221560e-01 7.6869222256369871e-03 8.0201108731684248e-02 3.3767207117004633e-01 3.7911616296213041e-01 4.6520447893521144e-01 5.4523065297803897e-01 5.9029251268147387e-01 5.9663404070000536e-01 6.4384844396709295e-01 7.4405492265982642e-01 8.8293502611182195e-01 9.7364986943879095e-01 1.2410681672970225e+00 1.2610076780064423e+00 1.6997990541684123e+00 1.7260539419026639e+00 1.7615703298824881e+00 1.8254642100232823e+00 1.8999177839921182e+00 2.1845241534036317e+00 2.5325349247917601e+00 2.9874617514367454e+00 3.1259288069314222e+00 3.1891620051652443e+00 3.2797685538021994e+00 3.2858678121317575e+00 3.4399087669429318e+00 3.5114590781544326e+00 3.5697300758288901e+00 3.6174584641460812e+00 3.6361205533282073e+00 3.6945923105802447e+00 3.9240163455490724e+00 3.9511494054836236e+00 4.1557167060036715e+00 4.1932269726518703e+00 4.4291609150673983e+00 4.6459391757070634e+00 4.7302938379510886e+00 4.9896645755568025e+00 5.4868660702648429e+00 5.9839044121463161e+00 6.2842935402312881e+00 6.2900613248491704e+00 6.3780017514170773e+00 6.4202853480323778e+00 6.4811643144743680e+00 6.5328557571461934e+00 6.6594818234889761e+00 6.8404421439285743e+00 7.1725297454174761e+00 7.6257028303104626e+00 1.1962121371310047e+01 ";
dftsph.set_string("Method","gga_x_pbe-gga_c_pbe");
restr_test(h2o,cc_pVTZ,dftsph,Etot,Eorb,"Water, PBEPBE/cc-pVTZ",dip,false);

dftdirect.set_string("Method","gga_x_pbe-gga_c_pbe");
restr_test(h2o,cc_pVTZ,dftdirect,Etot,Eorb,"Water, PBEPBE/cc-pVTZ direct",dip,false);

Etot=-7.6374645231629671e+01;
dip=7.3272055170695882e-01;
Eorb="-1.8741546120498271e+01 -9.1788065747314584e-01 -4.7348528244433030e-01 -3.2745825122706712e-01 -2.5054888754343979e-01 2.7902704366593546e-03 7.8041337318986756e-02 3.2383359240686460e-01 3.5924025465542286e-01 4.5242229685007568e-01 5.1413395771673176e-01 5.7767377624023963e-01 5.8423686372824335e-01 6.4253357165928604e-01 6.5976492533605258e-01 7.4241931551980578e-01 9.7186975543429310e-01 1.1822077615129800e+00 1.2023225455274469e+00 1.5759172639340711e+00 1.6360776220789905e+00 1.6982201400820562e+00 1.7245167580817702e+00 1.8628724317731493e+00 1.9081332422139756e+00 2.1641868205462838e+00 2.3473125296620037e+00 2.7892764426584238e+00 3.0049313662841777e+00 3.0831686610210314e+00 3.1876656880756893e+00 3.2328093552556507e+00 3.4168951194662203e+00 3.4579185915480330e+00 3.5049998520395440e+00 3.5282503337730833e+00 3.5683149774795782e+00 3.5772257968884991e+00 3.8379020298678403e+00 3.9226525845165994e+00 4.0867013204251910e+00 4.0926885532644395e+00 4.3310827268857626e+00 4.4154604442197201e+00 4.4322656001743201e+00 4.6027454790846472e+00 5.1266096994466519e+00 5.2200923638948558e+00 5.4840610550370723e+00 6.1494592212775485e+00 6.2799826543112136e+00 6.2885803709742270e+00 6.3502580452089186e+00 6.4059374447290285e+00 6.4358015119354732e+00 6.6570174306984855e+00 6.7153040549415390e+00 6.7372414755803964e+00 6.9398791708961918e+00 7.3406783757516312e+00 8.2789338675841684e+00 8.3551996946729385e+00 9.3390714498965881e+00 1.4480083330959808e+01 1.5822331895266428e+01 ";
dftcart.set_string("Method","gga_x_pbe-gga_c_pbe");
restr_test(h2o,cc_pVTZ,dftcart,Etot,Eorb,"Water, PBEPBE/cc-pVTZ cart",dip,false);

Etot=-5.6637319431552369e+03;
dip=4.1660088090369891e+00;
Eorb="-9.4985623400032819e+02 -1.4146635095149188e+02 -1.3119294651368236e+02 -1.3119287266562918e+02 -1.3119259492732471e+02 -2.7676547682082603e+01 -2.3232980782832914e+01 -2.3232722797265993e+01 -2.3231117082693874e+01 -1.6049049275763927e+01 -1.6049045517061440e+01 -1.6047827618878131e+01 -1.6047723988230487e+01 -1.6047713419750806e+01 -1.5604531125257036e+01 -1.5532249523428034e+01 -1.1296661985825732e+01 -1.1249243916938257e+01 -1.1232665102549554e+01 -4.3970789260728953e+00 -2.8992751974266051e+00 -2.8986598108582893e+00 -2.8951186639376703e+00 -1.4177741085501214e+00 -1.2312596934161417e+00 -1.0610694402408463e+00 -8.7645300679268989e-01 -8.5303384287836392e-01 -8.1305177487715907e-01 -7.2468343297425963e-01 -7.1752257321982194e-01 -7.1751283794778609e-01 -7.1453923692272747e-01 -7.1369020362820257e-01 -6.5649509295785380e-01 -6.5484509906862787e-01 -6.4819563026372484e-01 -6.1951447644134594e-01 -5.1149278087078531e-01 -4.5694083415931785e-01 -3.6925757457981895e-01 -1.8059222555067139e-01 6.9314384249512945e-02 7.4011367553273824e-02 1.1409015223080185e-01 1.4993230358585774e-01 1.8266979196192978e-01 1.9355783534267443e-01 2.1197839626452206e-01 2.5237136197605720e-01 2.7656210772929041e-01 2.8532362345259937e-01 3.0336607379395125e-01 3.3343210605484253e-01 3.3688908992111394e-01 3.9652955214790148e-01 4.2174259412260368e-01 5.4893794755567371e-01 5.6113635052477806e-01 6.8232567997122406e-01 8.8548529742544613e-01 9.2615819812182654e-01 9.2670939403649599e-01 9.6328467849836308e-01 9.8346700867185100e-01 9.9887403095852823e-01 1.0364505364395722e+00 1.0834412221814158e+00 1.0936564325537135e+00 1.1989337347315308e+00 1.2617669936784817e+00 1.2818433227191679e+00 1.3193949625433865e+00 1.3895935209379413e+00 1.4308893017086139e+00 1.4702798346779864e+00 1.4945329053524128e+00 1.5683750138976600e+00 1.5822512402103641e+00 1.6271532076272557e+00 1.6323133215639616e+00 1.6700777070915354e+00 1.7294530555651035e+00 1.8374560548002734e+00 1.9460155940148556e+00 1.9779608109936873e+00 2.0568938245422150e+00 2.2440133865844216e+00 2.9829355441601271e+00 3.0788481815683717e+00 5.2757403757669952e+00 2.1121787323024640e+02 ";
cart.set_string("Method","HF");
restr_test(cdcplx,b3_21G,cart,Etot,Eorb,"Cadmium complex, HF/3-21G",dip,true);

Etot=-5.6676815794910062e+03;
dip=3.7002288572638546e+00;
Eorb="-9.3984197883152422e+02 -1.3753207526128716e+02 -1.2775494111509420e+02 -1.2775484175329724e+02 -1.2775461964987596e+02 -2.5877644481693522e+01 -2.1698212854411725e+01 -2.1697968311197453e+01 -2.1696704197757899e+01 -1.4963813830827933e+01 -1.4963803404721112e+01 -1.4963010191615423e+01 -1.4962863979349118e+01 -1.4962827590802233e+01 -1.4360306729706886e+01 -1.4284297990671018e+01 -1.0222101843810561e+01 -1.0189092835364132e+01 -1.0172543511231540e+01 -3.7205486789741111e+00 -2.3708089249461595e+00 -2.3701439735386560e+00 -2.3667607658208647e+00 -1.0858782799993769e+00 -9.3089328948317118e-01 -7.9260852647880697e-01 -6.6325335201513491e-01 -6.4256578028255651e-01 -6.0923655992406545e-01 -4.9709259967252223e-01 -4.8503224595898259e-01 -4.8171575012093526e-01 -4.7251857666423847e-01 -4.6879512943718199e-01 -4.6872803770750820e-01 -4.6436502041850697e-01 -4.6398096810951056e-01 -4.5266952208248212e-01 -3.3969176544628188e-01 -3.2987594414746663e-01 -2.7191017001682755e-01 -1.3325528286562871e-01 -4.5384607783807629e-03 -2.5448996497686159e-03 7.9041961049099892e-03 3.3666462129940330e-02 3.7585742960135463e-02 6.3638840466117313e-02 9.5522085997526832e-02 1.3206152607431149e-01 1.3440144334180715e-01 1.5776390993459302e-01 1.6224138644651576e-01 1.8802867276790672e-01 2.0172817362459616e-01 2.1971847349872003e-01 2.4645546946309882e-01 3.6713697161200420e-01 3.7513123020295763e-01 4.5357894592522691e-01 6.5944643038140438e-01 6.7596335363336979e-01 6.7892116630363419e-01 7.2415268896701668e-01 7.3375297565915232e-01 7.3583951620491750e-01 7.7451977377203229e-01 8.4047814951782207e-01 8.5273295656548764e-01 9.2809644272736802e-01 9.7420656774047087e-01 1.0043092081561964e+00 1.0256246003191585e+00 1.0797444016421909e+00 1.1228254047584565e+00 1.1979813933708245e+00 1.2156262793560149e+00 1.2858830846627436e+00 1.3032976492206656e+00 1.3297849596638287e+00 1.3368291360176601e+00 1.3822188315422292e+00 1.4204705963416999e+00 1.5533759819290196e+00 1.6676753278083805e+00 1.6794892295265778e+00 1.7665908026982367e+00 1.9160562365052261e+00 2.6597651242237861e+00 2.7556240571310266e+00 4.7845673656370922e+00 2.0810560199551350e+02 ";
dftcart_nofit.set_string("Method","hyb_gga_xc_b3lyp");
restr_test(cdcplx,b3_21G,dftcart_nofit,Etot,Eorb,"Cadmium complex, B3LYP/3-21G",dip,false);

Etot=-4.6701319124867109e+02;
dip=5.6560644579894070e-01;
Eorb="-1.8627156169418161e+01 -9.8468881848768106e+00 -9.8000391931378612e+00 -9.7985011705700131e+00 -9.7964365038904173e+00 -9.7961796327454778e+00 -9.7961538267418558e+00 -9.7957600083426595e+00 -9.7955835585876159e+00 -9.7955233095481535e+00 -9.7902004297148366e+00 -9.4290507193818940e-01 -7.5219615668664930e-01 -7.3523073541423778e-01 -7.0339287984315080e-01 -6.7053973765758468e-01 -6.3196274922786799e-01 -5.8307099805173379e-01 -5.5339031004997219e-01 -5.4222235083196901e-01 -5.1586039993886246e-01 -5.0988659154004323e-01 -4.8254337360094690e-01 -4.3597693021237643e-01 -4.3184615338659205e-01 -4.1614323421118948e-01 -4.0900625508347604e-01 -4.0310191991085104e-01 -3.8729586025052037e-01 -3.7610047312408845e-01 -3.7081073754530675e-01 -3.4983178890328465e-01 -3.4595203895218662e-01 -3.3597043109800157e-01 -3.2455678737000165e-01 -3.2107025356365598e-01 -3.1361137247703680e-01 -2.9876296037826849e-01 -2.9369139655614945e-01 -2.9084575346810543e-01 -2.8883500264048012e-01 -2.8096974806083097e-01 -2.7633808415859062e-01 -2.6485287999090823e-01 -2.2609543890487963e-01 2.9806024763640554e-02 4.3502575295855425e-02 4.7426613144643366e-02 5.3710692205429601e-02 6.2797594354891351e-02 6.8714654258739835e-02 7.6075944411921545e-02 8.0543339100648578e-02 9.0742400963786357e-02 1.0571659482672913e-01 1.1202392506808952e-01 1.1622968275387860e-01 1.2071489679117979e-01 1.3141022986462964e-01 1.3642695594118270e-01 1.3882267069679008e-01 1.4089043519856223e-01 1.4347227175189498e-01 1.4890057699450557e-01 1.5392868039889729e-01 1.6458457942825791e-01 1.6999276745192130e-01 1.7439441466381744e-01 1.8135063289293996e-01 1.9191605486415136e-01 1.9907682309756011e-01 2.0636278999400395e-01 2.1896430543496948e-01 2.2823882429337974e-01 2.4034563283397975e-01 2.4854968138291222e-01 2.5464923220846414e-01 4.2493469348228924e-01 4.2851182854193859e-01 4.4064831506595337e-01 4.5736256334889153e-01 4.6331887897532037e-01 4.6892707817571683e-01 4.7902890882667998e-01 4.8942033438469446e-01 5.0243457357075649e-01 5.1194658843583729e-01 5.1904662873299245e-01 5.3632918346273195e-01 5.4651586163633459e-01 5.7350568013922032e-01 5.9413185923436307e-01 6.0274009175293886e-01 6.0615103766143363e-01 6.1192053131150614e-01 6.1545342471150422e-01 6.3140451054857727e-01 6.4954855442533754e-01 6.7503527312966283e-01 6.8567611171591358e-01 6.9676232856972065e-01 7.1482448311833302e-01 7.2341450068715385e-01 7.4573448792406871e-01 7.5077756586380862e-01 7.6086344138490336e-01 7.7072184040759961e-01 7.7609728152013557e-01 7.8186577537208757e-01 7.9605291333014494e-01 8.0733024477777449e-01 8.1699587423056141e-01 8.2988593867826743e-01 8.3747005435519983e-01 8.3872119407474077e-01 8.4332421033377647e-01 8.4899282369846618e-01 8.5771959165119049e-01 8.6352081340874387e-01 8.6889361315859892e-01 8.7768117958582381e-01 8.8613715613917832e-01 8.9531587998614881e-01 9.0580539198031784e-01 9.1529345037291732e-01 9.2211821681867034e-01 9.4122941634648272e-01 9.6566484095628280e-01 9.7153245382760389e-01 9.8203200387845813e-01 1.0177359507822161e+00 1.0490683861846857e+00 1.0974976079788903e+00 1.1473874138729130e+00 1.1642858604317232e+00 1.2116508922373164e+00 1.2321590126453998e+00 1.2665603924906104e+00 1.2725684240165509e+00 1.3173849859227789e+00 1.3344565869651059e+00 1.3696820532871710e+00 1.4032275716762310e+00 1.4066934434007101e+00 1.4522682774650948e+00 1.4859434263573963e+00 1.4994160095465772e+00 1.5182767248972457e+00 1.5407584379129005e+00 1.5551903768556046e+00 1.5718410975394728e+00 1.5854149326816265e+00 1.6035523619067977e+00 1.6248642732039895e+00 1.6295959334407544e+00 1.6386262044184903e+00 1.6518538418364674e+00 1.6708612197239487e+00 1.7082446296778604e+00 1.7240850502174871e+00 1.7309952461237650e+00 1.7768187470173509e+00 1.7799403089757708e+00 1.7966945467729352e+00 1.7986817194323859e+00 1.8208995180896930e+00 1.8372012936964026e+00 1.8486145939006313e+00 1.8627514050454441e+00 1.8684098463480858e+00 1.8910560761533579e+00 1.9068270266154654e+00 1.9273968065881972e+00 1.9366445623573654e+00 1.9517985676134246e+00 1.9711844466987307e+00 1.9748132680072443e+00 1.9784542984592788e+00 2.0029278696769426e+00 2.0163943433850040e+00 2.0242114390214594e+00 2.0282109898326262e+00 2.0446485249083874e+00 2.0506337134387160e+00 2.0622360367439181e+00 2.0764528083213381e+00 2.0982718570279721e+00 2.1124508950964525e+00 2.1473843899934710e+00 2.1546266996980523e+00 2.1669074853935713e+00 2.1723427150291417e+00 2.1811756615424640e+00 2.1987629708552490e+00 2.2110769675060320e+00 2.2189959107573789e+00 2.2523882133704807e+00 2.2601866511078406e+00 2.2680781522068343e+00 2.2959489029053941e+00 2.3105475173723486e+00 2.3159945746318589e+00 2.3268620022348077e+00 2.3486729415137613e+00 2.3828969085347476e+00 2.3876853416994464e+00 2.4069231022369482e+00 2.4220207627223833e+00 2.4322427238041535e+00 2.4627682320617521e+00 2.4929222749689792e+00 2.5133883601593108e+00 2.5312880009137739e+00 2.5380553797740122e+00 2.5674716230084096e+00 2.5816435159721798e+00 2.5894761768490215e+00 2.6092019863917160e+00 2.6302176912694772e+00 2.6355319395521786e+00 2.6434881780008568e+00 2.6604218654953073e+00 2.6727749698248928e+00 2.6917620413470824e+00 2.6952996953107600e+00 2.7073942923414633e+00 2.7113429961479114e+00 2.7285878754856521e+00 2.7487931881699987e+00 2.7749376910823309e+00 2.7823799529510169e+00 2.7848199028035761e+00 2.7958607983819581e+00 2.8014817972827939e+00 2.8080216646012324e+00 2.8118711035345001e+00 2.8150035365699098e+00 2.8202130222286916e+00 2.8419227769280551e+00 2.8601401219438900e+00 2.8723562928886359e+00 2.9059065364374570e+00 3.0865702215423170e+00 3.1217735063309942e+00 3.1464492863798732e+00 3.1676579973958350e+00 3.1811713700711279e+00 3.1906305744563377e+00 3.1946230564798612e+00 3.2130866902323540e+00 3.2280264911726477e+00 3.2455319023810572e+00 3.2648013487959675e+00 3.2857855619156471e+00 3.3101643340803442e+00 3.3528874172810146e+00 3.3823787277427897e+00 3.3896326092962967e+00 3.3912089819927482e+00 3.4239217906230124e+00 3.4639876552005444e+00 3.4735426998607539e+00 3.4830955640556125e+00 3.4844494736213059e+00 3.8198801766271600e+00 4.1566348964251016e+00 4.2134221024681047e+00 4.2755968669994173e+00 4.3733062110853549e+00 4.4240204348135448e+00 4.4487041773312122e+00 4.5000884423294583e+00 4.5662887500840004e+00 4.6275754088998289e+00 4.7059779871082794e+00 ";
dftcart.set_string("Method","lda_x-lda_c_vwn_rpa");
restr_test(decanol,b6_31Gpp,dftcart,Etot,Eorb,"1-decanol, SVWN(RPA)/6-31G**",dip,true);


#ifndef COMPUTE_REFERENCE
  printf("****** Tests completed in %s *******\n",t.elapsed().c_str());
#endif

  return 0;
}
