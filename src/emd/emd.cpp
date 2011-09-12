/*
 *                This source code is part of
 * 
 *                     E  R  K  A  L  E
 *                             -
 *                       DFT from Hel
 *
 * Written by Jussi Lehtola, 2010-2011
 * Copyright (c) 2010-2011, Jussi Lehtola
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */



#include <algorithm>
#include <complex>

#include "../timer.h"
#include "emd.h"
#include "mathf.h"
#include "spherical_expansion.h"
#include "spherical_harmonics.h"

extern "C" {
// For bessel functions
#include <gsl/gsl_sf_bessel.h>
}

// Value of moment of electron density in Fourier space
#define moment(i) (pow(dens[i].p,2+mom)*dens[i].d)
#define density(i) (pow(dens[i].p,2)*dens[i].d)

// Integration rules
#define roughdens(i) ((density(i-2)+4.0*density(i)+density(i+2))*(dens[i+2].p-dens[i-2].p)/6.0)
#define finedens(i)  ((density(i-2)+4.0*density(i-1)+2.0*density(i)+4.0*density(i+1)+density(i+2))*(dens[i+2].p-dens[i-2].p)/12.0)

#define roughmom(i)  ((moment(i-2)+4.0*moment(i)+moment(i+2))*(dens[i+2].p-dens[i-2].p)/6.0)
#define finemom(i)   ((moment(i-2)+4.0*moment(i-1)+2.0*moment(i)+4.0*moment(i+1)+moment(i+2))*(dens[i+2].p-dens[i-2].p)/12.0)

// Maximum number of points allowed for converging number of electrons
#define MAXPOINTS 10000
// Print out moments at maximum every N seconds
#define MAXPRINTFREQ 5

bool operator<(const onecenter_t & lhs, const onecenter_t & rhs) {
  // Sort first by exponent
  if(lhs.z<rhs.z)
    return 1;
  else if(lhs.z==rhs.z) {
    // then by power of p
    return lhs.pm<rhs.pm;
  }

  return 0;
}

bool operator==(const onecenter_t & lhs, const onecenter_t & rhs) {
  return (lhs.z==rhs.z) && (lhs.pm==rhs.pm);
}

bool operator<(const twocenter_contr_t & lhs, const twocenter_contr_t & rhs) {
  return lhs.dr<rhs.dr;
}

bool operator==(const twocenter_contr_t & lhs, const twocenter_contr_t & rhs) {
  return lhs.dr==rhs.dr;
}

bool operator<(const twocenter_t & lhs, const twocenter_t & rhs) {
  /// Sort first by exponent
  if(lhs.z<rhs.z)
    return 1;
  else if(lhs.z==rhs.z) {

    // then by power of p
    if(lhs.pm<rhs.pm)
      return 1;
    else if(lhs.pm==rhs.pm) {

      // then by index of Bessel function
      return lhs.l<rhs.l;
    }
  }

  return 0;
}

bool operator==(const twocenter_t & lhs, const twocenter_t & rhs) {
  return (lhs.z==rhs.z) && (lhs.pm==rhs.pm) && (lhs.l==rhs.l);
}


std::vector< std::vector<size_t> > find_identical_shells(const BasisSet & bas) {
  // Returned list of identical basis functions
  std::vector< std::vector<size_t> > ret;

  // Loop over shells
  for(size_t ish=0;ish<bas.get_Nshells();ish++) {
    // Get exponents, contractions and cartesian functions on shell
    std::vector<double> shell_z=bas.get_zetas(ish);
    std::vector<double> shell_c=bas.get_contr(ish);
    std::vector<shellf_t> shell_cart=bas.get_cart(ish);

    // Try to find the shell on the current list of identicals
    bool found=0;
    for(size_t iident=0;iident<ret.size();iident++) {

      // Check first cartesian part.
      std::vector<shellf_t> cmp_cart=bas.get_cart(ret[iident][0]);

      if(shell_cart.size()==cmp_cart.size()) {
	// Default value
	found=1;
	
	for(size_t icart=0;icart<shell_cart.size();icart++)
	  if(shell_cart[icart].l!=cmp_cart[icart].l || shell_cart[icart].m!=cmp_cart[icart].m || shell_cart[icart].n!=cmp_cart[icart].n)
	    found=0;
	
	// Check that usage of spherical harmonics matches, too
	if(bas.lm_in_use(ish) != bas.lm_in_use(ret[iident][0]))
	  found=0;
	
	// If cartesian parts match, check also exponents and contraction coefficients
	if(found) {
	  // Get exponents
	  std::vector<double> cmp_z=bas.get_zetas(ret[iident][0]);
	  std::vector<double> cmp_c=bas.get_contr(ret[iident][0]);

	  // Check exponents
	  if(shell_z.size()==cmp_z.size()) {
	    for(size_t iexp=0;iexp<shell_z.size();iexp++)
	      if(shell_z[iexp]!=cmp_z[iexp])
		found=0;

	    for(size_t iexp=0;iexp<shell_z.size();iexp++)
	      if(shell_c[iexp]!=cmp_c[iexp])
		found=0;
	  } else
	    found=0;
	}

	// If everything matches, add the function to the current list.
	if(found) {
	  ret[iident].push_back(ish);
	  // Stop iteration over list of identical functions
	  break;
	}
      }
    }

    // If the shell was not found on the list of identicals, add it
    if(!found) {
      std::vector<size_t> hlp;
      hlp.push_back(ish);
      ret.push_back(hlp);
    }
  }

  return ret;
}

EMDEvaluator::EMDEvaluator() {
}

EMDEvaluator::EMDEvaluator(const BasisSet & bas, const arma::mat & P) {
  // First, get list of identical shells.
  std::vector< std::vector<size_t> > idents=find_identical_shells(bas);

  // Form Fourier expansions of non-identical functions, [shell][function]
  std::vector< std::vector<GTO_Fourier_Ylm> > ylmexp;
  for(size_t i=0;i<idents.size();i++) {
    // Get exponents, contraction coefficients and cartesians
    std::vector<double> zetas=bas.get_zetas(idents[i][0]);
    std::vector<double> contr=bas.get_contr(idents[i][0]);
    std::vector<shellf_t> cart=bas.get_cart(idents[i][0]);

    // Form expansions of cartesian functions
    std::vector<GTO_Fourier_Ylm> cart_expansion;
    for(size_t icart=0;icart<cart.size();icart++) {
      // Expansion of current function
      GTO_Fourier_Ylm func;
      for(size_t iexp=0;iexp<contr.size();iexp++)
	func+=contr[iexp]*GTO_Fourier_Ylm(cart[icart].l,cart[icart].m,cart[icart].n,zetas[iexp]);
      // Plug in the normalization factor
      func=cart[icart].relnorm*func;
      // Clean out terms with zero contribution
      func.clean();
      // Add to list of cartesian expansions
      cart_expansion.push_back(func);
    }

    // If spherical harmonics are used, we need to transform the
    // functions into the spherical harmonics basis.
    if(bas.lm_in_use(idents[i][0])) {
      std::vector<GTO_Fourier_Ylm> sph_expansion;
      // Get transformation matrix
      arma::mat transmat=bas.get_trans(idents[i][0]);
      // Form expansion
      int l=bas.get_am(idents[i][0]);
      for(int m=-l;m<=l;m++) {
	// Expansion for current term
	GTO_Fourier_Ylm mcomp;
	// Form expansion
	for(size_t icart=0;icart<transmat.n_cols;icart++)
	  mcomp+=transmat(l+m,icart)*cart_expansion[icart];
	// clean it
	mcomp.clean();
	// and add it to the stack
	sph_expansion.push_back(mcomp);
      }
      // Now we have all components, add everything to the stack
      ylmexp.push_back(sph_expansion);
    } else
      // No need to transform, cartesians are used.
      ylmexp.push_back(cart_expansion);
  }

  /*
  for(size_t iident=0;iident<idents.size();iident++) {
    printf("\nIdentical group %lu:\n",iident);
    printf("Consists of shells: ");
    for(size_t i=0;i<idents[iident].size();i++)
      printf(" %lu",idents[iident][i]);
    printf("\n");
    printf("Transformation matrix:\n");
    bas.gettrans(idents[iident][0]).print();
    printf("Transforms of functions:\n");
    for(size_t i=0;i<ylmexp[iident].size();i++) {
      printf("Function %lu\n",i);
      ylmexp[iident][i].print();
    }
    printf("\n");
  }
  */

  // Get multiplication table
  SphericalExpansionMultiplicationTable mult(bas.get_max_am());

  // Imaginary unit
  std::complex<double> im(0.0,1.0);
	
  // Loop over shells of identical basis functions
  for(size_t iidsh=0;iidsh<idents.size();iidsh++)
    for(size_t jidsh=0;jidsh<idents.size();jidsh++) {

      // Loop over the basis functions on the shells to calculate the
      // angular part of the product
      for(size_t ifunc=0;ifunc<ylmexp[iidsh].size();ifunc++)
	for(size_t jfunc=0;jfunc<ylmexp[jidsh].size();jfunc++) {

	  // Calculate the product
	  GTO_Fourier_Ylm prod=mult.mult(ylmexp[iidsh][ifunc].conjugate(),ylmexp[jidsh][jfunc]);
	  //	  GTO_Fourier_Ylm prod=ylmexp[iidsh][ifunc].conjugate()*ylmexp[jidsh][jfunc];

	  // Get the expansion
	  std::vector<GTO_Fourier_Ylm_t> prodexp=prod.getexp();

	  // Now, loop over the identical shells, which share the same
	  // angular part.
	  for(size_t ibf=0;ibf<idents[iidsh].size();ibf++)
	    for(size_t jbf=0;jbf<idents[jidsh].size();jbf++) {
	      
	      // The global indices of the basis functions under
	      // consideration are
	      size_t ibas=bas.get_first_ind(idents[iidsh][ibf])+ifunc;
	      size_t jbas=bas.get_first_ind(idents[jidsh][jbf])+jfunc;

	      // Only do off-diagonal once
	      if(jbas>ibas)
		continue;
	
	      // Calculate the displacement between the basis functions
	      coords_t dr_vec=bas.get_shell_coords(idents[iidsh][ibf])-bas.get_shell_coords(idents[jidsh][jbf]);

	      // Express displacement in spherical coordinates
	      // (to compute value of spherical harmonic)
	      double dr=norm(dr_vec);

	      // Phi and cos(theta)
	      double phi, cth;
	      if(dr>0) {
		phi=atan2(dr_vec.y,dr_vec.x);
		cth=dr_vec.z/dr;
	      } else {
		phi=-1;
		cth=-1;
	      }

	      if(dr==0.0) {
		// One-center case
		for(size_t icomb=0;icomb<prodexp.size();icomb++) {
		  // Get the spherical harmonics coefficients
		  std::vector<ylmcoeff_t> sph=prodexp[icomb].ang.getcoeffs();
	  
		  for(size_t iang=0;iang<sph.size();iang++)
		    if(sph[iang].l==0 && sph[iang].m==0) {
		      // Found contributing term
		      
		      onecenter_t hlp;
		      hlp.pm=prodexp[icomb].pm;
		      hlp.z=prodexp[icomb].z;

		      // If we're off-diagonal, we get the real part twice
		      // (two different shells on same atom, or different functions on same shell)
		      if(ibas!=jbas)
			hlp.c=2.0*sph[iang].c.real()*P(ibas,jbas)*sqrt(4.0*M_PI);
		      else
			// On the diagonal we get it just once.
			hlp.c=sph[iang].c.real()*P(ibas,jbas)*sqrt(4.0*M_PI);

		      add_term(hlp);
		      
		      break;
		    }
		}
	      } else {

		// Two-center case
		for(size_t icomb=0;icomb<prodexp.size();icomb++) {

                  // Get the angular expansion
		  std::vector<ylmcoeff_t> sph=prodexp[icomb].ang.getcoeffs();
		  // Loop over angular part
		  for(size_t iang=0;iang<sph.size();iang++) {
		    // Compute value of spherical harmonics
		    std::complex<double> Ylm=spherical_harmonics(sph[iang].l,sph[iang].m,cth,phi);
		    // i^l
		    std::complex<double> il=pow(im,sph[iang].l);
		    // Full expansion coefficient is
		    std::complex<double> c=Ylm*il*sph[iang].c;
		    // Add the term. We are bound to be off-diagonal,
		    // so we get the real part twice.

		    twocenter_contr_t tmp;
		    tmp.dr=dr;
		    tmp.c=8.0*M_PI*c.real()*P(ibas,jbas);

		    twocenter_t hlp;
		    hlp.c.push_back(tmp);
		    hlp.z=prodexp[icomb].z;
		    hlp.pm=prodexp[icomb].pm;
		    hlp.l=sph[iang].l;
		      
		    add_term(hlp);
		  }
		}
		
	      }
	    }
	}
    }
  
  // Clean
  clean();

  // Print out structure
  print();
}

EMDEvaluator::~EMDEvaluator() {
}

void EMDEvaluator::add_term(const onecenter_t & t) {
  if(onec.size()==0) {
    onec.push_back(t);
  } else {
    // Get upper bound
    std::vector<onecenter_t>::iterator high;
    high=std::upper_bound(onec.begin(),onec.end(),t);

    // Corresponding index is
    size_t ind=high-onec.begin();

    if(ind>0 && onec[ind-1]==t)
      // Found it.
      onec[ind-1].c+=t.c;
    else {
      // Term does not exist, add it
      onec.insert(high,t);
    }
  }
}

void EMDEvaluator::add_term(const twocenter_t & t) {
  if(twoc.size()==0) {
    twoc.push_back(t);
  } else {
    // Get upper bound
    std::vector<twocenter_t>::iterator high;
    high=std::upper_bound(twoc.begin(),twoc.end(),t);
    // Corresponding index is
    size_t ind=high-twoc.begin();
    
    if(ind>0 && twoc[ind-1]==t)
      // Loop over terms in t
      for(size_t it=0;it<t.c.size();it++)
        add_contr(ind-1,t.c[it]);
    else {
      // Term does not exist, add it
      twoc.insert(high,t);
    }
  }
}

void EMDEvaluator::add_contr(size_t ind, const twocenter_contr_t & t) {
  if(twoc[ind].c.size()==0) {
    twoc[ind].c.push_back(t);
  } else {
    // Get upper bound
    std::vector<twocenter_contr_t>::iterator hi;
    hi=std::upper_bound(twoc[ind].c.begin(),twoc[ind].c.end(),t);

    size_t indt=hi-twoc[ind].c.begin();
    if(indt>0 && twoc[ind].c[indt-1] == t)
      // Found it!
      twoc[ind].c[indt-1].c+=t.c;
    else
      // Need to add the term.
      twoc[ind].c.insert(hi,t);
  }
}

double EMDEvaluator::eval_onec(double p) const {
  // Evaluate one-center terms
  double d_onec=0.0;
  for(size_t i=0;i<onec.size();i++)
    d_onec+=onec[i].c*pow(p,onec[i].pm)*exp(-onec[i].z*p*p);

  return d_onec;
}

double EMDEvaluator::eval_twoc(double p) const {
  // Evaluate two-center terms
  double jlcontr;
  double d_twoc=0.0;
  for(size_t i=0;i<twoc.size();i++) {
    jlcontr=0.0;
    for(size_t j=0;j<twoc[i].c.size();j++)
      jlcontr+=twoc[i].c[j].c*gsl_sf_bessel_jl(twoc[i].l,p*twoc[i].c[j].dr);
    d_twoc+=jlcontr*pow(p,twoc[i].pm)*exp(-twoc[i].z*p*p);
  }
  return d_twoc;
}

double EMDEvaluator::eval(double p) const {
  double dens=eval_onec(p)+eval_twoc(p);
  if(dens<0) {
    throw std::domain_error("Error - negative momentum density encountered!\n");
  }
  return dens;
}

size_t EMDEvaluator::getN_onec() const {
  return onec.size();
}

size_t EMDEvaluator::getN_twoc() const {
  return twoc.size();
}

size_t EMDEvaluator::getN_twoc_total() const {
  size_t N=0;
  for(size_t i=0;i<twoc.size();i++)
    N+=twoc[i].c.size();
  return N;
}

void EMDEvaluator::clean() {
  // Two-center terms
  for(size_t i=0;i<twoc.size();i++)
    for(size_t j=twoc[i].c.size()-1;j<twoc[i].c.size();j--)
      if(twoc[i].c[j].c==0) {
	twoc[i].c.erase(twoc[i].c.begin()+j);
      }

  // One-center terms
  for(size_t i=onec.size()-1;i<onec.size();i--)
    if(onec[i].c==0)
      onec.erase(onec.begin()+i);
}

void EMDEvaluator::print() const {
  // Print out terms

  /*
  if(onec.size()) {
    printf("One-center terms:\n");
    for(size_t i=0;i<onec.size();i++) {
      printf("%lu\tpm=%i, z=%e, c=%e\n",i,onec[i].pm,onec[i].z,onec[i].c);
    }
  }
  if(twoc.size()) {
    printf("Two-center terms\n");
    for(size_t i=0;i<twoc.size();i++) {
      printf("%lu\tl=%i, pm=%i, z=%e\n",i,twoc[i].l,twoc[i].pm,twoc[i].z);
      for(size_t j=0;j<twoc[i].c.size();j++)
	printf("\tdr=%e, c=%e\n",twoc[i].dr[j],twoc[i].c[j]);
    }
  }
  */
  
  //  printf("EMD has %lu one-center and %lu two-center terms, that contain %lu contractions.\n",getN_onec(),getN_twoc(),getN_twoc_total());
}

EMD::EMD(const BasisSet & bas, const arma::mat & P, bool verbose) {
  // Calculate norm of density matrix
  dmnorm=arma::trace(P*bas.overlap());
  // Number of electrons is (probably)
  Nel=(int) round(dmnorm);

  if(verbose)
    printf("\nNumber of electrons is %i, from which norm of DM differs by %e.\n",Nel,dmnorm-Nel);
  
  // Initialize evaluator
  eval=EMDEvaluator(bas,P);
}

EMD::~EMD() {
}

void EMD::initial_fill(bool verbose) {
  
  // Initial dp
  double idp=0.25;
  // Helpers
  double p, dp;

  if(verbose) {
    printf("\nFilling in initial grid ... ");
    fflush(stdout);
  }

  emd_t hlp;
  emd_t hlparr[4];

  // Add origin
  hlp.p=0.0;
  hlp.d=eval.eval(hlp.p);
  dens.push_back(hlp);

  do {
    // Calculate value of dp to use
    p=dens[dens.size()-1].p;
    dp=(1.0+2.0*p)*idp;

#ifdef _OPENMP
#pragma omp parallel for ordered
#endif
    for(int ipoint=0;ipoint<4;ipoint++) {
      hlparr[ipoint].p=p+(ipoint+1)*dp;
      hlparr[ipoint].d=eval.eval(hlparr[ipoint].p);
#ifdef _OPENMP
#pragma omp ordered
#endif
      dens.push_back(hlparr[ipoint]);
    }
  } while(dens[dens.size()-1].d>0.0);

  if(verbose)
    printf("done.\n");
}

void EMD::add4(size_t loc) {
  // Add 4 points around loc
  emd_t integ[4];

  //  printf("Adding more points at %lu.\n",loc);
  
#ifdef _OPENMP
#pragma omp parallel for ordered
#endif
  for(int ipoint=2;ipoint>-2;ipoint--) {
    // Value of p is
    integ[2-ipoint].p=0.5*(dens[loc+ipoint].p+dens[loc+ipoint-1].p);
    // Value of the density at this point is
    integ[2-ipoint].d=eval.eval(integ[2-ipoint].p);
    // Add value to the list
#ifdef _OPENMP
#pragma omp ordered
#endif
    dens.insert(dens.begin()+loc+ipoint,integ[2-ipoint]);
  }
}

void EMD::find_electrons(bool verbose, double tol) {
  // Integral and its estimated error
  double integral, error;
  // Integral slices
  double rough=0.0, fine=0.0;
  // Location of maximum error
  double maxerror=0;
  size_t maxind=0;

  if(verbose) {
    printf("Continuing fill of grid to find electrons ... ");
    fflush(stdout);
  }

  size_t iter=0;

  // Tighten grid adaptively
  do {
    iter++;
    integral=0;
    error=0;
    
    maxerror=0;
    // Calculate integral and find maximum error
    for(size_t i=dens.size()-3;i<dens.size();i-=4) {
      // The rough value is (3-point Simpson)
      rough=roughdens(i);
      // The fine value is (5-point Simpson)
      fine=finedens(i);
      
      // Increment total integrals
      integral+=fine;
      error+=fabs(fine-rough)/15.0;
      
      // Is the maximum error here?
      if(fabs(rough-fine)>maxerror) {
        maxerror=fabs(rough-fine);
        maxind=i;
      }
    }

    if(fabs(dmnorm-integral)>tol) {
      // Check that the calculation will actually converge at some point..
      if(dens.size()>MAXPOINTS)
	throw std::domain_error("Error in find_electrons: maximum allowed number of points reached.\n");

      // Add points to area of maximum error
      add4(maxind);
    }
    
  } while(fabs(1.0*Nel-integral)>tol);

  if(verbose)
    printf("done.\n");
}

void EMD::optimize_moments(bool verbose, double tol) {
  // Integrals
  double rough, fine;
  
  // Moments of density
  int mom;
  int Nmom=7;
  int moms[]={-2, -1, 0, 1, 2, 3, 4};
  double momval[Nmom];
  double momerr[Nmom];
  size_t mommaxerrloc[Nmom];
  
  // Temporary helper
  double maxerr;
  
  // Maximum relative error
  double errel;
  int errelind;

  // Timer for printouts
  Timer t;
  size_t iter=0;
  
  if(verbose)
    printf("Optimizing the moments of the EMD.\n");

  do {
    iter++;
    
    // Calculate momentums and error estimates
    for(int imom=0;imom<Nmom;imom++) {
      mom=moms[imom];
      momval[imom]=0.0;
      momerr[imom]=0.0;
      
      maxerr=0;
      mommaxerrloc[imom]=-1;

      // Calculate <p^k>
      for(size_t i=dens.size()-3;i<dens.size();i-=4) {
	rough=roughmom(i);
	fine=finemom(i);
	
	momval[imom]+=fine;
	momerr[imom]+=fabs(fine-rough)/15.0;

	// Determine location of maximum error for each value of k
	if(fabs(rough-fine)>maxerr) {
	  mommaxerrloc[imom]=i;
	  maxerr=fabs(rough-fine);
	}
      }
    }

    // Find out which moment has maximum error and where it is
    errel=0;
    errelind=-1;
    for(int imom=0;imom<Nmom;imom++)
      if(momerr[imom]/momval[imom]>errel) {
        errel=momerr[imom]/momval[imom];
        errelind=imom;
      }

    // Print out current values if necessary
    if(verbose && (iter==1 || t.get()>MAXPRINTFREQ || errel<=tol)) {
      t.set();
      printf("\nUsing %lu points, charge differs from norm of DM by %e.\n",dens.size(),momval[2]-dmnorm);
      printf("Current values of moments are:\n");
      printf("\tk\t<p^k>\t\t\\Delta <p^k>\t \\Delta <p^k> / <p^k>\n");
      for(int imom=0;imom<Nmom;imom++)
        printf("\t%i\t%e\t%e\t%e\n",moms[imom],momval[imom],momerr[imom],momerr[imom]/momval[imom]);
    }

    // If tolerance has not been reached, add more points
    if(errel>tol)
        add4(mommaxerrloc[errelind]);

  } while(errel>tol);
}

std::vector<emd_t> EMD::get() const {
  return dens;
}

void EMD::save(const char * fname) const {
  FILE *out=fopen(fname,"w");
  for(size_t i=0;i<dens.size();i++)
    fprintf(out,"%.16e\t%.16e\n",dens[i].p,dens[i].d);
  fclose(out);
}

void EMD::moments(const char * fname) const {
  // Three and five point Simpson
  double rough, fine;
  
  // Moments calculated by Hart & Thakkar
  int Nm=7;
  int m[]={-2, -1, 0, 1, 2, 3, 4};
  
  // Moments and errors
  double moms[Nm];
  double err[Nm];
  
  // Helper variables
  const size_t N=dens.size();
  double p[N], integrand[N];

  // Fill in momentum grid
  for(size_t i=0;i<N;i++)
    p[i]=dens[i].p;

  // Calculate the moments
  for(int mi=0;mi<Nm;mi++) {
    // Fill in helper grid
    for(size_t i=0;i<N;i++)
      integrand[i]=pow(p[i],2+m[mi])*dens[i].d;
    
    // Zero out old values
    moms[mi]=0;
    err[mi]=0;
    
    // Calculate moments & errors
    rough=0;
    fine=0;
    for(size_t i=N-3;i<N;i-=4) {
      // Sum from infinity to get more accurate integrals
      rough=(integrand[i-2]+4.0*integrand[i]+integrand[i+2])/6.0*(p[i+2]-p[i-2]);
      fine=(integrand[i-2]+4.0*integrand[i-1]+2.0*integrand[i]+4.0*integrand[i+1]+integrand[i+2])/12.0*(p[i+2]-p[i-2]);
      
      moms[mi]+=fine;
      err[mi]+=fabs(fine-rough)/15.0;
    }
  }
  
  // Print out the moments
  FILE *out=fopen(fname,"w");
  for(int mi=0;mi<Nm;mi++)
    fprintf(out,"\t%i\t%.12e\t%.12e\n",m[mi],moms[mi],err[mi]);
  fclose(out);
}

void EMD::compton_profile(const char * fname_raw, const char * fname_interp) const {

  double rough, fine, Jint, Jerr;
  double integrand[dens.size()];

  size_t N=(dens.size()-1)/4;
  std::vector<double> p(N);
  std::vector<double> J(N);
  std::vector<double> dJ(N);
  
  // Calculate integrand
  for(size_t i=0;i<dens.size();i++)
    integrand[i]=dens[i].p*dens[i].d;
  
  // Calculate the Compton profile
  rough=0.0;
  fine=0.0;
  Jint=0.0;
  Jerr=0.0;
  size_t n=N-1;

  // Calculate integral
  for(size_t i=dens.size()-3;i<dens.size();i-=4) {
    rough=(integrand[i-2]+4.0*integrand[i]+integrand[i+2])/6.0*(dens[i+2].p-dens[i-2].p);
    fine=(integrand[i-2]+4.0*integrand[i-1]+2.0*integrand[i]+4.0*integrand[i+1]+integrand[i+2])/12.0*(dens[i+2].p-dens[i-2].p);

    Jint+=fine;
    Jerr+=fabs(fine-rough)/15.0;

    // Save profile
    p[n]=dens[i-2].p; // Must be i-1 to get J(0) = 1st moment of density / 2
    J[n]=0.5*Jint; // J = 1/2 \int_{|q|}^\infty 
    dJ[n]=0.5*Jerr;
    n--;
  }

  // Print out profile
  FILE *out=fopen(fname_raw,"w");
  for(size_t n=0;n<N;n++)
    fprintf(out,"%.12e\t%.12e\t%.12e\n",p[n],J[n],dJ[n]);
  fclose(out);

  // Interpolate profile to p = 0 .. pmax with spacing dp
  const int Nreg=2;
  double pmax[Nreg]={10.0, 40.0};
  double dp[Nreg]={0.01, 0.5};
  int Npoints[Nreg];
  
  Npoints[0]=(int) round(pmax[0]/dp[0]);
  for(int i=1;i<Nreg;i++)
    Npoints[i]=(int) round((pmax[i]-pmax[i-1])/dp[i]);
  // Add one final point for "safety"
  Npoints[Nreg-1]++;

  // Interpolated p values.
  std::vector<double> p_interp;
  for(int i=0;i<Nreg;i++)
    for(int j=0;j<Npoints[i];j++) {
      if(i>0)
	p_interp.push_back(pmax[i-1]+j*dp[i]);
      else
	p_interp.push_back(j*dp[i]);
    }

  // Get interpolated Compton profile and its error
  std::vector<double> J_interp=spline_interpolation(p,J,p_interp);
  std::vector<double> dJ_interp=spline_interpolation(p,dJ,p_interp);
  
  // Save output
  out=fopen(fname_interp,"w");
  for(size_t i=0;i<p_interp.size();i++)
    fprintf(out,"%.12e\t%.12e\t%.12e\n",p_interp[i],J_interp[i],dJ_interp[i]);
  fclose(out);
}

