/*
 * $Id$
 * 
 *       This source code is part of
 * 
 *        G   R   O   M   A   C   S
 * 
 * GROningen MAchine for Chemical Simulations
 * 
 *               VERSION 2.0
 * 
 * Copyright (c) 1991-1999
 * BIOSON Research Institute, Dept. of Biophysical Chemistry
 * University of Groningen, The Netherlands
 * 
 * Please refer to:
 * GROMACS: A message-passing parallel molecular dynamics implementation
 * H.J.C. Berendsen, D. van der Spoel and R. van Drunen
 * Comp. Phys. Comm. 91, 43-56 (1995)
 * 
 * Also check out our WWW page:
 * http://md.chem.rug.nl/~gmx
 * or e-mail to:
 * gromacs@chem.rug.nl
 * 
 * And Hey:
 * GRowing Old MAkes el Chrono Sweat
 */
static char *SRCID_coupling_c = "$Id$";

#include "typedefs.h"
#include "smalloc.h"
#include "update.h"
#include "vec.h"
#include "macros.h"
#include "physics.h"
#include "names.h"
#include "fatal.h"
#include "txtdump.h"
#include "nrnb.h"

/* 
 * This file implements temperature and pressure coupling algorithms:
 * For now only the Weak coupling and the modified weak coupling.
 *
 * Furthermore computation of pressure and temperature is done here
 *
 */

void calc_pres(int eBox,matrix box,tensor ekin,tensor vir,tensor pres,real Elr)
{
  int  n,m;
  real fac,Plr;

  if (eBox == ebtNONE)
    clear_mat(pres);
  else {
    /* Uitzoeken welke ekin hier van toepassing is, zie Evans & Morris - E. 
     * Wrs. moet de druktensor gecorrigeerd worden voor de netto stroom in  
     * het systeem...       
     */
    
    /* Long range correction for periodic systems, see
     * Neumann et al. JCP
     * divide by 6 because it is multiplied by fac later on.
     * If Elr = 0, no correction is made.
     */

    /* This formula should not be used with Ewald or PME, 
     * where the full long-range virial is calculated. EL 990823
     */
    Plr = Elr/6.0;
    
    fac=PRESFAC*2.0/det(box);
    for(n=0; (n<DIM); n++)
      for(m=0; (m<DIM); m++)
	pres[n][m]=(ekin[n][m]-vir[n][m]+Plr)*fac;
	
    if (debug) {
      pr_rvecs(debug,0,"PC: pres",pres,DIM);
      pr_rvecs(debug,0,"PC: ekin",ekin,DIM);
      pr_rvecs(debug,0,"PC: vir ",vir, DIM);
      pr_rvecs(debug,0,"PC: box ",box, DIM);
    }
  }
}

real calc_temp(real ekin,real nrdf)
{
  return (2.0*ekin)/(nrdf*BOLTZ);
}

real run_aver(real old,real cur,int step,int nmem)
{
  nmem   = max(1,nmem);
  
  return ((nmem-1)*old+cur)/nmem;
}


void do_pcoupl(t_inputrec *ir,int step,tensor pres,
	       matrix box,int start,int nr_atoms,
	       rvec x[],unsigned short cFREEZE[],
	       t_nrnb *nrnb,ivec nFreeze[])
{
  static bool bFirst=TRUE;
  static rvec PPP;
  int    n,d,g;
  real   scalar_pressure, xy_pressure, p_corr_z;
  rvec   factor,mu;
  char   *ptr,buf[STRLEN];
  
  /*
   *  PRESSURE SCALING 
   *  Step (2P)
   */
  if (bFirst) {
    /* Initiate the pressure to the reference one */
    for(d=0; d<DIM; d++)
      PPP[d] = ir->ref_p[d];
    bFirst=FALSE;
  }
  scalar_pressure=0;
  xy_pressure=0;
  for(d=0; d<DIM; d++) {
    PPP[d]           = run_aver(PPP[d],pres[d][d],step,ir->npcmemory);
    scalar_pressure += PPP[d]/DIM;
    if (d != ZZ)
      xy_pressure += PPP[d]/(DIM-1);
  }
  
  /* Pressure is now in bar, everywhere. */
  if (scalar_pressure != 0.0) {
    for(d=0; d<DIM; d++)
      factor[d] = ir->compress[d]*ir->delta_t/ir->tau_p;
    clear_rvec(mu);
    switch (ir->epc) {
    case epcNO:
      /* do_pcoupl should not be called in this case to save some work */
      for(d=0; d<DIM; d++)
	mu[d] = 1.0;
      break;
    case epcISOTROPIC:
      for(d=0; d<DIM; d++)
	mu[d] = pow(1.0-factor[d]*(ir->ref_p[d]-scalar_pressure),1.0/DIM);
      break;
    case epcSEMIISOTROPIC:
      for(d=0; d<ZZ; d++)
	mu[d] = pow(1.0-factor[d]*(ir->ref_p[d]-xy_pressure),1.0/DIM);
      mu[ZZ] = pow(1.0-factor[ZZ]*(ir->ref_p[ZZ] - PPP[ZZ]),1.0/DIM);
      break;
    case epcANISOTROPIC:
      for (d=0; d<DIM; d++)
	mu[d] = pow(1.0-factor[d]*(ir->ref_p[d] - PPP[d]),1.0/DIM);
      break;
    case epcSURFACETENSION:
      /* ir->ref_p[0/1] is the reference surface-tension times *
       * the number of surfaces                                */
      if (ir->compress[ZZ])
	p_corr_z = ir->delta_t/ir->tau_p*(ir->ref_p[ZZ] - PPP[ZZ]);
      else
	/* when the compressibity is zero, set the pressure correction   *
	 * in the z-direction to zero to get the correct surface tension */
	p_corr_z = 0;
      mu[ZZ] = 1.0 - ir->compress[ZZ]*p_corr_z;
      for(d=0; d<ZZ; d++)
	mu[d] = sqrt(1.0+factor[d]*(ir->ref_p[d]/(mu[ZZ]*box[ZZ][ZZ]) - 
	(PPP[ZZ]+p_corr_z - xy_pressure)));
      break;
    case epcTRICLINIC:
    default:
      fatal_error(0,"Pressure coupling type %s not supported yet\n",
		  EPCOUPLTYPE(ir->epc));
    }
    if (debug) {
      pr_rvecs(debug,0,"PC: PPP ",&PPP,1);
      pr_rvecs(debug,0,"PC: fac ",&factor,1);
      pr_rvecs(debug,0,"PC: mu  ",&mu,1);
    }
    
    if (mu[XX]<0.99 || mu[XX]>1.01 ||
	mu[YY]<0.99 || mu[YY]>1.01 ||
	mu[ZZ]<0.99 || mu[ZZ]>1.01) {
      sprintf(buf,"\nStep %d  Warning: pressure scaling more than 1%%, "
	      "mu: %g %g %g\n",step,mu[XX],mu[YY],mu[ZZ]);
      fprintf(stdlog,"%s",buf);
      fprintf(stderr,"%s",buf);
    }

    /* Scale the positions */
    for (n=start; n<start+nr_atoms; n++) {
      g=cFREEZE[n];
      
      if (!nFreeze[g][XX])
	x[n][XX] *= mu[XX];
      if (!nFreeze[g][YY])
	x[n][YY] *= mu[YY];
      if (!nFreeze[g][ZZ])
	x[n][ZZ] *= mu[ZZ];
    }
    /* compute final boxlengths */
    for (n=0; n<DIM; n++)
      for (d=0; d<DIM; d++)
	box[n][d] *= mu[d];

    ptr = check_box(box);
    if (ptr)
      fatal_error(0,ptr);

    inc_nrnb(nrnb,eNR_PCOUPL,nr_atoms);
  }
}

void tcoupl(bool bTC,t_grpopts *opts,t_groups *grps,
	    real dt,real SAfactor,int step,int nmem)
{
  static real *Told=NULL;
  int    i;
  real   T,reft,lll;

  if (!Told) {
    snew(Told,opts->ngtc);
    for(i=0; (i<opts->ngtc); i++) 
      Told[i]=opts->ref_t[i]*SAfactor;
  }
  
  for(i=0; (i<opts->ngtc); i++) {
    reft=opts->ref_t[i]*SAfactor;
    if (reft < 0)
      reft=0;
    
    Told[i] = run_aver(Told[i],grps->tcstat[i].T,step,nmem);
    T       = Told[i];
    
    if ((bTC) && (T != 0.0)) {
      lll=sqrt(1.0 + (dt/opts->tau_t[i])*(reft/T-1.0));
      grps->tcstat[i].lambda=max(min(lll,1.25),0.8);
    }
    else
      grps->tcstat[i].lambda=1.0;
#ifdef DEBUGTC
    fprintf(stdlog,"group %d: T: %g, Lambda: %g\n",
	    i,T,grps->tcstat[i].lambda);
#endif
  }
}


