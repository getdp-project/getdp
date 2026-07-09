// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
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
#include "Cal_Value.h"
#include "Message.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

extern struct CurrentData Current;

/* Legendre Polynomials Namespace */
namespace Legendre {

// Standard Legendre polynomial P_n(x)
// Uses recurrence relation: (n+1)*P_{n+1}(x) = (2n+1)*x*P_n(x) - n*P_{n-1}(x)
inline double poly(int n, double x) {
  if(n < 0) n = 0; // Handle negative degree
  if(x < -1.0) x = -1.0;
  if(x > 1.0) x = 1.0;

  if(n == 0) return 1.0;
  if(n == 1) return x;

  double p_prev = 1.0;      // P_0(x)
  double p_curr = x;        // P_1(x)
  double p_next;

  for(int i = 1; i < n; ++i) {
    p_next = ((2.0*i + 1.0)*x*p_curr - i*p_prev) / (i + 1.0);
    p_prev = p_curr;
    p_curr = p_next;
  }

  return p_curr;
}

// Associated Legendre polynomial P_m^n(x)
// Handles both standard and associated cases
inline double poly_assoc(int n, int m, double x) {
  if(n < 0) n = 0;
  if(m < 0) m = -m;
  if(m > n) return 0.0;
  if(x < -1.0) x = -1.0;
  if(x > 1.0) x = 1.0;

  double pmm = 1.0;
  if(m > 0) {
    double somx2 = std::sqrt((1.0 - x)*(1.0 + x));
    double fact = 1.0;
    for(int i = 1; i <= m; ++i) {
      pmm *= -fact * somx2;
      fact += 2.0;
    }
  }

  if(n == m) return pmm;

  double pmmp1 = x * (2.0*m + 1.0) * pmm;
  if(n == m + 1) return pmmp1;

  double p_prev = pmm;
  double p_curr = pmmp1;
  double p_next;

  for(int i = m + 2; i <= n; ++i) {
    p_next = (x * (2.0*i - 1.0) * p_curr - (i + m - 1.0) * p_prev) / (i - m);
    p_prev = p_curr;
    p_curr = p_next;
  }

  return p_curr;
}

// Derivative of standard Legendre polynomial dP_n/dx
inline double poly_deriv(int n, double x) {
  if(n == 0) return 0.0;
  if(x < -1.0) x = -1.0;
  if(x > 1.0) x = 1.0;

  if(std::fabs(x) >= 1.0 - 1e-14) {
    // Near endpoints
    return (n * (n + 1.0)) / 2.0 * (x > 0 ? 1.0 : (n % 2 == 0 ? 1.0 : -1.0));
  }

  double pn = poly(n, x);
  double pn1 = poly(n - 1, x);
  double denom = x*x - 1.0;

  if(std::fabs(denom) < 1e-14) return 0.0;
  return (n * (x * pn - pn1)) / denom;
}

} // namespace Legendre

/* ------------------------------------------------------------------------ */

// power law
// rho = Ecrit/Jc * (J/Jc)^(n - 1)
void F_ResistivityPowerLaw(F_ARG)
{
  double Jnorm = (double)(A)->Val[0];
  double Jcrit = (double)(A + 1)->Val[0];
  double n = (double)(A + 2)->Val[0];

  double Ecrit = (double)Fct->Para[0];

  double rho;
  if(Jcrit > 0) { rho = Ecrit / Jcrit * pow(Jnorm / Jcrit, n - 1); }
  else {
    rho = std::numeric_limits<double>::max();
  }

  V->Val[0] = rho;
  V->Type = SCALAR;
}

// diadic product of derivative of the power law with respect to the current
// density with the current density itself because current density is a vector,
// its derivative will be a vector and the dyadic product will be second-order
// tensor
void F_drhodj_times_j_PowerLaw(F_ARG)
{
  double Jx = (double)(A)->Val[0];
  double Jy = (double)(A)->Val[1];
  double Jz = (double)(A)->Val[2];

  double Jcrit = (double)(A + 1)->Val[0];
  double n = (double)(A + 2)->Val[0];

  double Ecrit = (double)Fct->Para[0];

  double commonFactor;
  double jNorm = sqrt(Jx * Jx + Jy * Jy + Jz * Jz);
  if(jNorm > 1e-10) {
    commonFactor =
      Ecrit / pow(Jcrit, 3) * (n - 1) * pow(MIN(1E99, jNorm) / Jcrit, n - 3);
  }
  else {
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

void F_dedj_PowerLaw(F_ARG)
{
  double Jx = (double)(A)->Val[0];
  double Jy = (double)(A)->Val[1];
  double Jz = (double)(A)->Val[2];

  double Jcrit = (double)(A + 1)->Val[0];
  double n = (double)(A + 2)->Val[0];

  double Ecrit = (double)Fct->Para[0];

  double commonFactor;
  double jNorm = sqrt(Jx * Jx + Jy * Jy + Jz * Jz);
  if(jNorm > 1e-10) {
    commonFactor =
      Ecrit / pow(Jcrit, 3) * (n - 1) * pow(MIN(1E99, jNorm) / Jcrit, n - 3);
  }
  else {
    commonFactor = 0.;
  }
  double rho = Ecrit / Jcrit * pow(MIN(1E99, jNorm) / Jcrit, n - 1);
  V->Val[0] = rho + commonFactor * Jx * Jx; // t11
  V->Val[1] = commonFactor * Jx * Jy; // t12
  V->Val[2] = commonFactor * Jx * Jz; // t13
  V->Val[3] = rho + commonFactor * Jy * Jy; // t22
  V->Val[4] = commonFactor * Jy * Jz; // t23
  V->Val[5] = rho + commonFactor * Jz * Jz; // t33

  V->Type = TENSOR_SYM;
}

// for thin shell 2D
void F_drhodj_times_j_PowerLaw_TS_2D(F_ARG)
{
  double jNorm = (double)(A)->Val[0];
  double Jcrit = (double)(A + 1)->Val[0];
  double n = (double)(A + 2)->Val[0];

  double Ecrit = (double)Fct->Para[0];

  double res;
  if(jNorm > 1e-10) {
    res = Ecrit / pow(Jcrit, 3) * (n - 1) *
          pow(MIN(1E99, jNorm) / Jcrit, n - 3) * jNorm * jNorm;
  }
  else {
    res = 0.;
  }
  V->Val[0] = res;
  V->Type = SCALAR;
}

/* ------------------------------------------------------------------------ */
/*  Current-sharing Homogenization factor
    paper: L. Bortot et al. https://doi.org/10.1109/TASC.2020.2969476
    Function credits: CernGetDP - https://gitlab.cern.ch/steam/cerngetdp
    */
/* ------------------------------------------------------------------------ */

void F_LambdaCurrentSharingHom(F_ARG)
{
  // Global index
  int iter_i;

  // Binary search algorithm
  double alpha;
  double lambda = 0;
  double lambda_MIN;
  double lambda_MAX;
  double fun;
  double err;

  if(A->Type != SCALAR || (A + 1)->Type != SCALAR || (A + 2)->Type != SCALAR ||
     (A + 3)->Type != SCALAR)
    Message::Error("Wrong type of arg. for current sharing!");

  double j = (double)A->Val[0]; // Current density in homogenized conductor
  double jc = (double)(A + 1)->Val[0]; // critical current density
  double rhoNC = (double)(A + 2)->Val[0]; // resistivity of normal conductor
  double n = (double)(A + 3)->Val[0]; // n-Value power-law

  double ec = Fct->Para[0]; // Critical electric field 1e-4 [V/m]

  // error = (b-a)/2^n where (b-a)=1 and n=NO_OF_MAX_ITER
  // n = 20 -> error < 1e-6
  double ABS_TOL = Fct->Para[1];
  int NO_OF_MAX_ITER = Fct->Para[2];

  // thicknesses
  double thickness_NC = Fct->Para[3];
  double thickness_HTS = Fct->Para[4];

  double rhoHTS;

  // Quench
  if(jc <= 0) { lambda = 0; }
  // Current Sharing Regime
  else {
    // Binary search algorithm
    lambda_MIN = 0;
    lambda_MAX = 1;

    rhoHTS =
      (ec / jc) * pow((j / jc), (n - 1)) *
      (thickness_HTS /
       (thickness_HTS +
        thickness_NC)); // j and jc are engineering quantities, necessity to
                        // rescale first jc back to obtain true jc (old code)
    alpha = (rhoHTS / thickness_HTS) / (rhoNC / thickness_NC);

    // Inner iterations
    for(iter_i = 0; iter_i < NO_OF_MAX_ITER; iter_i++) {
      lambda = (lambda_MIN + lambda_MAX) * 0.5;
      fun = alpha * pow(lambda, n) + lambda - 1;
      err = lambda_MAX - lambda_MIN;

      if(err > ABS_TOL) { // Update boundaries
        if(fun >= 0) { // too much current in HTS
          lambda_MAX = lambda;
        }
        else if(fun < 0) { // not enough current in HTS
          lambda_MIN = lambda;
        }
      }
      else { // Converged
        break;
      }
    }
    // Consistency check: number of iterations
    if(iter_i == NO_OF_MAX_ITER)
      Message::Warning("Maximum number of bisection iterations reached!");
  }

  double output = lambda;

  if(isnan(output)) Message::Warning("Output is nan");

  V->Val[0] = output;
  V->Type = SCALAR;
}

// anisotropic resistivity tensor for homogenized stacks of HTS tapes
// in the local coordinates of the tape, we use power-law resistivity along the
// tape and constant rho0 resistivity in the transverse direction for now: only
// rotations around the z-axis are supported
void F_AnisotropicResistivityTensorPowerLaw(F_ARG)
{
  double Jx = (double)(A)->Val[0];
  double Jy = (double)(A)->Val[1];
  double Jz = (double)(A)->Val[2];
  double Jcrit = (double)(A + 1)->Val[0];
  double n = (double)(A + 2)->Val[0];
  double rho0 =
    (double)(A + 3)->Val[0]; // resistivity in perpendicular direction
  double angle =
    (double)(A + 4)->Val[0]; // tape rotation angle around z-axis in radians
                             // (tape normal aligned with (1,0,0) is theta=0)

  double Ecrit = (double)Fct->Para[0];

  // rotation tensor
  double cc, sc;
  struct Value Rot;
  cc = cos(angle);
  sc = sin(angle);
  Rot.Type = TENSOR;
  Cal_ZeroValue(&Rot);
  Rot.Val[0] = cc;
  Rot.Val[1] = -sc;
  Rot.Val[2] = 0.0;
  Rot.Val[3] = sc;
  Rot.Val[4] = cc;
  Rot.Val[5] = 0.0;
  Rot.Val[6] = 0.0;
  Rot.Val[7] = 0.0;
  Rot.Val[8] = 1.0;

  double jNorm = sqrt(Jx * Jx + Jy * Jy + Jz * Jz);

  double rhoPL;
  if(Jcrit > 0) { rhoPL = Ecrit / Jcrit * pow(jNorm / Jcrit, n - 1); }
  else {
    rhoPL = std::numeric_limits<double>::max();
  }

  // resistivity tensor in the local coordinates of the tape (normal along
  // (1,0,0))
  struct Value RhoTensor;
  RhoTensor.Type = TENSOR_DIAG;
  Cal_ZeroValue(&RhoTensor);
  RhoTensor.Val[0] = rho0;
  RhoTensor.Val[1] = rhoPL;
  RhoTensor.Val[2] = rhoPL;

  // rotate the resistivity tensor
  Cal_RotateValue(&Rot, &RhoTensor, V);
}

void F_AnisotropicDEDJTensorPowerLaw(F_ARG)
{
  double Jx = (double)(A)->Val[0];
  double Jy = (double)(A)->Val[1];
  double Jz = (double)(A)->Val[2];
  double Jcrit = (double)(A + 1)->Val[0];
  double n = (double)(A + 2)->Val[0];
  double rho0 =
    (double)(A + 3)->Val[0]; // resistivity in perpendicular direction
  double angle =
    (double)(A + 4)->Val[0]; // tape rotation angle around z-axis in radians
                             // (tape normal aligned with (1,0,0) is theta=0)

  double Ecrit = (double)Fct->Para[0];

  // rotation tensor
  double cc, sc;
  struct Value Rot;
  cc = cos(angle);
  sc = sin(angle);
  Rot.Type = TENSOR;
  Cal_ZeroValue(&Rot);
  Rot.Val[0] = cc;
  Rot.Val[1] = -sc;
  Rot.Val[2] = 0.0;
  Rot.Val[3] = sc;
  Rot.Val[4] = cc;
  Rot.Val[5] = 0.0;
  Rot.Val[6] = 0.0;
  Rot.Val[7] = 0.0;
  Rot.Val[8] = 1.0;

  // in local coordinates
  // (de_dj)i,j = (rho)i,j + ec/jc^3 * (n-1) * (Norm(Jlocal)/jc)^(n-3) *
  // Jlocal_i * Jlocal_j

  // computing the first term
  double jNorm = sqrt(Jx * Jx + Jy * Jy + Jz * Jz);

  double rhoPL;
  if(Jcrit > 0) { rhoPL = Ecrit / Jcrit * pow(jNorm / Jcrit, n - 1); }
  else {
    rhoPL = std::numeric_limits<double>::max();
  }

  // compute the second term (first in local coordinates)

  // J in global coordinates
  struct Value JGlobal;
  JGlobal.Type = VECTOR;
  Cal_ZeroValue(&JGlobal);
  JGlobal.Val[0] = Jx;
  JGlobal.Val[1] = Jy;
  JGlobal.Val[2] = Jz;

  // rotate J to local coordinates
  struct Value JLocal;
  JLocal.Type = VECTOR;
  Cal_ZeroValue(&JLocal);
  Cal_RotateValue(&Rot, &JGlobal, &JLocal);
  double JLocalX = 0.0; // no normal current in the local coordinates
  double JLocalY = JLocal.Val[1];
  double JLocalZ = JLocal.Val[2];

  double jLocalNorm =
    sqrt(JLocalX * JLocalX + JLocalY * JLocalY + JLocalZ * JLocalZ);

  double commonFactor;
  if(jLocalNorm > 1e-10) {
    commonFactor = Ecrit / pow(Jcrit, 3) * (n - 1) *
                   pow(MIN(1E99, jLocalNorm) / Jcrit, n - 3);
  }
  else {
    commonFactor = 0.;
  }

  // de_dj tensor in the local coordinates of the tape (normal along (1,0,0))
  struct Value DEDJLocal;
  DEDJLocal.Type = TENSOR;
  Cal_ZeroValue(&DEDJLocal);
  DEDJLocal.Val[0] = rho0 + commonFactor * JLocalX * JLocalX; // t11
  DEDJLocal.Val[1] = commonFactor * JLocalX * JLocalY; // t12
  DEDJLocal.Val[2] = commonFactor * JLocalX * JLocalZ; // t13
  DEDJLocal.Val[3] = commonFactor * JLocalY * JLocalX; // t21
  DEDJLocal.Val[4] = rhoPL + commonFactor * JLocalY * JLocalY; // t22
  DEDJLocal.Val[5] = commonFactor * JLocalY * JLocalZ; // t23
  DEDJLocal.Val[6] = commonFactor * JLocalZ * JLocalX; // t31
  DEDJLocal.Val[7] = commonFactor * JLocalZ * JLocalY; // t32
  DEDJLocal.Val[8] = rhoPL + commonFactor * JLocalZ * JLocalZ; // t33

  // finally, we need to rotate the tensor back to the global coordinates
  Cal_RotateValue(&Rot, &DEDJLocal, V);
}

// returns scalar quantity: (u - u0)/(u1 - u0)^m, useful for foil winding
// polynomial basis functions if coord_type = 0: the normal to stack of tapes is
// along x -> x is the curvilinear variable u
void F_FoilWindingPolynomialBF(F_ARG)
{
  int m = (int)Fct->Para[0];
  double u0 = (double)Fct->Para[1];
  double u1 = (double)Fct->Para[2];
  int coord_type = (int)Fct->Para[3]; // 0:x, 1:y, 2:z

  double u = 0;
  switch(coord_type) {
  case 0: u = Current.x; break;
  case 1: u = Current.y; break;
  case 2: u = Current.z; break;
  }

  if(!(u1 - u0)) {
    Message::Error("FoilWindingPolynomialBF: Wrong parameters! Para[1] "
                   "is foil coordinate with regard to 0, Para[2] is the "
                   "width of the foil plus the parameter with regard to zero");
  }

  if(coord_type != 0 && coord_type != 1 && coord_type != 2) {
    Message::Error(
      "FoilWindingPolynomialBF: Wrong coordinate type! Should be 0, 1, 2.");
  }

  double u_shifted = (u - u0) / (u1 - u0);
  double res = pow(u_shifted, m);

  // Normalize coordinate from [u0, u1] to [-1, 1]
  // double u_normalized = 2.0 * (u - u0) / (u1 - u0) - 1.0;
  // double res = pow(u_normalized, m);

  V->Val[0] = res;
  V->Type = SCALAR;
}

// returns scalar quantity: (u - u0)/(u1 - u0)^m, useful for foil winding
// polynomial basis functions if coord_type = 0: the normal to stack of tapes is
// along x -> x is the curvilinear variable u
void F_FoilWindingLegendrePolynomialBF(F_ARG)
{
  int m = (int)Fct->Para[0];
  double u0 = (double)Fct->Para[1];
  double u1 = (double)Fct->Para[2];
  int coord_type = (int)Fct->Para[3]; // 0:x, 1:y, 2:z

  double u = 0;
  switch(coord_type) {
  case 0: u = Current.x; break;
  case 1: u = Current.y; break;
  case 2: u = Current.z; break;
  }

  if(!(u1 - u0)) {
    Message::Error("FoilWindingLegendrePolynomialBF: Wrong parameters! Para[1] "
                   "is foil coordinate with regard to 0, Para[2] is the "
                   "width of the foil plus the parameter with regard to zero");
  }

  if(coord_type != 0 && coord_type != 1 && coord_type != 2) {
    Message::Error(
      "FoilWindingPolynomialBF: Wrong coordinate type! Should be 0, 1, 2.");
  }

  double u_shifted = (u - u0) / (u1 - u0);

  // Use m-th order Legendre polynomial
  double res = Legendre::poly(m, u_shifted);

  V->Val[0] = res;
  V->Type = SCALAR;
}

// Legendre Polynomial Wrappers
// Standard Legendre polynomial P_n(x)
// Parameters: Para[0] = n (order), Arg[0] = x (value in [-1, 1])
void F_LegendrePoly(F_ARG)
{
  if(A->Type != SCALAR)
    Message::Error("LegendrePoly: Argument must be scalar");

  int n = (int)Fct->Para[0];
  double x = (double)A->Val[0];

  V->Val[0] = Legendre::poly(n, x);
  V->Type = SCALAR;
}

// Associated Legendre polynomial P_m^n(x)
// Parameters: Para[0] = n (degree), Para[1] = m (order), Arg[0] = x
void F_LegendrePolyAssoc(F_ARG)
{
  if(A->Type != SCALAR)
    Message::Error("LegendrePolyAssoc: Argument must be scalar");

  int n = (int)Fct->Para[0];
  int m = (int)Fct->Para[1];
  double x = (double)A->Val[0];

  V->Val[0] = Legendre::poly_assoc(n, m, x);
  V->Type = SCALAR;
}

// Derivative of Legendre polynomial dP_n/dx
// Parameters: Para[0] = n (order), Arg[0] = x (value in [-1, 1])
void F_LegendrePolyDeriv(F_ARG)
{
  if(A->Type != SCALAR)
    Message::Error("LegendrePolyDeriv: Argument must be scalar");

  int n = (int)Fct->Para[0];
  double x = (double)A->Val[0];

  V->Val[0] = Legendre::poly_deriv(n, x);
  V->Type = SCALAR;
}
