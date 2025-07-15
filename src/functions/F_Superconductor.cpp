// GetDP - Copyright (C) 1997-2025 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//   Erik Schnaubelt
//   Louis Denis
//   Inspired from CernGetDP - https://gitlab.cern.ch/steam/cerngetdp

#include <math.h>
#include <limits>
#include "ProData.h"
#include "F.h"
#include "Message.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

extern struct CurrentData Current;

/* ------------------------------------------------------------------------ */

// power law
// rho = Ecrit/Jc * (J/Jc)^(n - 1)
void F_ResistivityPowerLaw(F_ARG) {
    double Jnorm = (double) (A)->Val[0];
    double Jcrit = (double) (A + 1)->Val[0];
    double n = (double) (A + 2)->Val[0];

    double Ecrit = (double) Fct->Para[0];

    double rho;
    if (Jcrit > 0) {
        rho = Ecrit/Jcrit * pow(Jnorm/Jcrit, n - 1);
    } else {
        rho = std::numeric_limits<double>::max();
    }
    
    V->Val[0] = rho;
    V->Type   = SCALAR;
}

// diadic product of derivative of the power law with respect to the current density with the current density itself
// because current density is a vector, its derivative will be a vector and the dyadic product will be second-order tensor
void F_drhodj_times_j_PowerLaw(F_ARG) {
    double Jx = (double) (A)->Val[0];
    double Jy = (double) (A)->Val[1];
    double Jz = (double) (A)->Val[2];
    
    double Jcrit = (double) (A + 1)->Val[0];
    double n = (double) (A + 2)->Val[0];

    double Ecrit = (double) Fct->Para[0];

    double commonFactor;
    double jNorm = sqrt(Jx*Jx+Jy*Jy+Jz*Jz);
    if (jNorm>1e-10) {
        commonFactor = Ecrit/pow(Jcrit, 3) * (n - 1) * pow(MIN(1E99, jNorm)/Jcrit, n - 3);
    } else{
        commonFactor = 0.;
    }
    V->Val[0] = commonFactor * Jx * Jx; // t11
    V->Val[1] = commonFactor * Jx * Jy; // t12
    V->Val[2] = commonFactor * Jx * Jz; // t13
    V->Val[3] = commonFactor * Jy * Jy; // t22
    V->Val[4] = commonFactor * Jy * Jz; // t23
    V->Val[5] = commonFactor * Jz * Jz; // t33

    V->Type = TENSOR_SYM;
}

void F_dedj_PowerLaw(F_ARG) {
    double Jx = (double) (A)->Val[0];
    double Jy = (double) (A)->Val[1];
    double Jz = (double) (A)->Val[2];
    
    double Jcrit = (double) (A + 1)->Val[0];
    double n = (double) (A + 2)->Val[0];

    double Ecrit = (double) Fct->Para[0];

    double commonFactor;
    double jNorm = sqrt(Jx*Jx+Jy*Jy+Jz*Jz);
    if (jNorm>1e-10) {
        commonFactor = Ecrit/pow(Jcrit, 3) * (n - 1) * pow(MIN(1E99, jNorm)/Jcrit, n - 3);
    } else{
        commonFactor = 0.;
    }
    double rho = Ecrit/Jcrit * pow(MIN(1E99, jNorm)/Jcrit, n - 1);
    V->Val[0] = rho + commonFactor * Jx * Jx; // t11
    V->Val[1] = commonFactor * Jx * Jy; // t12
    V->Val[2] = commonFactor * Jx * Jz; // t13
    V->Val[3] = rho + commonFactor * Jy * Jy; // t22
    V->Val[4] = commonFactor * Jy * Jz; // t23
    V->Val[5] = rho + commonFactor * Jz * Jz; // t33

    V->Type = TENSOR_SYM;
}

/* ------------------------------------------------------------------------ */
/*  Current-sharing Homogenization factor 
    paper: L. Bortot et al. https://doi.org/10.1109/TASC.2020.2969476
    Function credits: CernGetDP - https://gitlab.cern.ch/steam/cerngetdp
    */
/* ------------------------------------------------------------------------ */

void F_LambdaCurrentSharingHom(F_ARG) {
  // Global index
  int iter_i;

  // Binary search algorithm
  double alpha;
  double lambda = 0;
  double lambda_MIN;
  double lambda_MAX;
  double fun;
  double err;

  if(A->Type != SCALAR || (A+1)->Type != SCALAR || (A+2)->Type != SCALAR || (A+3)->Type != SCALAR)
    Message::Error("Wrong type of arg. for current sharing!");

	double j = (double)A->Val[0]; // Current density in homogenized conductor
	double jc = (double)(A + 1)->Val[0]; // critical current density
	double rhoNC  = (double)(A + 2)->Val[0]; // resistivity of normal conductor
	double n  = (double)(A + 3)->Val[0]; // n-Value power-law

	double ec = Fct->Para[0]; // Critical electric field 1e-4 [V/m]


  // error = (b-a)/2^n where (b-a)=1 and n=NO_OF_MAX_ITER
  // n = 20 -> error < 1e-6
  double ABS_TOL        = Fct->Para[1];
  int    NO_OF_MAX_ITER = Fct->Para[2];

  // thicknesses
  double thickness_NC  = Fct->Para[3];
  double thickness_HTS = Fct->Para[4];

  double rhoHTS;

  // Quench
	if (jc<=0) {
		lambda = 0;
	}
	// Current Sharing Regime
	else {
		// Binary search algorithm
		lambda_MIN = 0;
		lambda_MAX = 1;

		rhoHTS = (ec/jc) * pow((j/jc),(n-1)) * (thickness_HTS/(thickness_HTS+thickness_NC)); // j and jc are engineering quantities, necessity to rescale first jc back to obtain true jc (old code)
		alpha  = (rhoHTS/thickness_HTS) / (rhoNC/thickness_NC);

		// Inner iterations
		for (iter_i = 0; iter_i < NO_OF_MAX_ITER; iter_i++) {

			lambda = (lambda_MIN+lambda_MAX)*0.5;
			fun    = alpha*pow(lambda,n)+lambda-1;
			err    = lambda_MAX-lambda_MIN;

			if (err>ABS_TOL){     // Update boundaries
				if (fun>=0){      // too much current in HTS
					lambda_MAX = lambda;
					}
				else if (fun < 0) {  // not enough current in HTS
					lambda_MIN = lambda;
					}
				}
			else{ // Converged
				break;
			}
		}
    // Consistency check: number of iterations
   if (iter_i == NO_OF_MAX_ITER)
     Message::Warning("Maximum number of bisection iterations reached!");
}

 double output = lambda;

 if(isnan(output))
     Message::Warning("Output is nan");

 V->Val[0] = output;
 V->Type   = SCALAR;
}
