// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//   Michael Asam

#include <stdio.h>
#include <limits>
#include <math.h>
#include <vector>
#include "GetDPConfig.h"
#include "ProData.h"
#include "DofData.h"
#include "SolvingOperations.h"
#include "Message.h"

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

// The entries of a distributed vector are read by the rank that owns them,
// and what is computed on them is then combined over the ranks. Returns the
// range of entries of this rank, and true if the vector is distributed (the
// ranks can also all have the same sequential vector).
static bool Get_OwnedRange(gVector *x, int *low, int *high)
{
  LinAlg_GetLocalVectorRange(x, low, high);
#if defined(HAVE_MPI)
  if(Message::GetCommSize() > 1 && Message::GetIsCommWorld()) {
    int n;
    LinAlg_GetVectorSize(x, &n);
    long long owned = *high - *low, total = 0;
    MPI_Allreduce(&owned, &total, 1, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
    return total == n;
  }
#endif
  return false;
}

static void Combine(bool distributed, double *sums, int nsums, double *max)
{
#if defined(HAVE_MPI)
  if(!distributed) return;
  MPI_Allreduce(MPI_IN_PLACE, sums, nsums, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  if(max)
    MPI_Allreduce(MPI_IN_PLACE, max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
#endif
}

static double Get_AbsInVector(gVector *x, int i)
{
  double re, im = 0.;
  if(gSCALAR_SIZE == 1)
    LinAlg_GetAbsDoubleInVector(&re, x, i);
  else
    LinAlg_GetComplexInVector(&re, &im, x, i, -1);
  return sqrt(re * re + im * im);
}

void Cal_SolutionErrorRatio(gVector *dx, gVector *x, double reltol,
                            double abstol, int NormType, double *ErrorRatio)
{
  // dx is usually the result of arithmetic vector operations done just before
  // this call (e.g. xCurrent - xPrevious)
  LinAlg_UpdateGhosts();

  int xLength, low, high;
  LinAlg_GetVectorSize(dx, &xLength);
  bool distributed = Get_OwnedRange(dx, &low, &high);

  // sum, sum of squares, number of invalid values and largest value of the
  // error ratios
  double sums[3] = {0., 0., 0.}, max = 0.;
  for(int i = low; i < high; i++) {
    double ratio =
      Get_AbsInVector(dx, i) / (abstol + reltol * Get_AbsInVector(x, i));
    if(ratio != ratio || // Solution is NaN
       ratio == -std::numeric_limits<double>::infinity() || // Solution is -Inf
       ratio == std::numeric_limits<double>::infinity()) { // Solution is Inf
      sums[2] += 1.;
      continue;
    }
    sums[0] += ratio;
    sums[1] += ratio * ratio;
    if(ratio > max) max = ratio;
  }
  Combine(distributed, sums, 3, &max);

  if(sums[2]) {
    Message::Warning("No valid solution found (NaN or Inf)!");
    *ErrorRatio = std::numeric_limits<double>::quiet_NaN();
    return;
  }

  switch(NormType) {
  case LINFNORM: *ErrorRatio = max; break;
  case L1NORM: *ErrorRatio = sums[0]; break;
  case MEANL1NORM: *ErrorRatio = sums[0] / xLength; break;
  case L2NORM: *ErrorRatio = sqrt(sums[1]); break;
  case MEANL2NORM: *ErrorRatio = sqrt(sums[1] / xLength); break;
  default:
    *ErrorRatio = 0.;
    Message::Error("Wrong error norm in Cal_SolutionErrorRatio");
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  C a l _ S o l u t i o n E r r o r                                       */
/* ------------------------------------------------------------------------ */

static void Get_ComplexInVector(gVector *x, int i, double *re, double *im)
{
  *im = 0.;
  if(gSCALAR_SIZE == 1)
    LinAlg_GetDoubleInVector(re, x, i);
  else
    LinAlg_GetComplexInVector(re, im, x, i, -1);
}

void Cal_SolutionError(gVector *dx, gVector *x, int diff, double *MeanError)
{
  // This is not a very good implementation: it should be replaced with
  // Cal_SolutionErrorRatio above

  // see Cal_SolutionErrorRatio (dx may just have been scaled)
  LinAlg_UpdateGhosts();

  int n, low, high;
  LinAlg_GetVectorSize(dx, &n);
  bool distributed = Get_OwnedRange(dx, &low, &high);

  // the norm of each entry of x and of dx (or of dx - x)
  std::vector<double> nx(high - low), ndx(high - low);
  double moy[2] = {0., 0.};
  for(int i = low; i < high; i++) {
    double valx, valxi, valdx, valdxi;
    Get_ComplexInVector(x, i, &valx, &valxi);
    Get_ComplexInVector(dx, i, &valdx, &valdxi);
    nx[i - low] = sqrt(valx * valx + valxi * valxi);
    if(gSCALAR_SIZE == 1)
      ndx[i - low] = diff ? fabs(fabs(valdx) - fabs(valx)) : fabs(valdx);
    else if(diff)
      ndx[i - low] = sqrt((valdx - valx) * (valdx - valx) +
                          (valdxi - valxi) * (valdxi - valxi));
    else
      ndx[i - low] = sqrt(valdx * valdx + valdxi * valdxi);
    moy[0] += nx[i - low];
    moy[1] += (gSCALAR_SIZE == 1 && diff) ? (fabs(valdx) - fabs(valx)) :
                                             ndx[i - low];
  }
  Combine(distributed, moy, 2, NULL);
  double xmoy = moy[0] / (double)n, dxmoy = moy[1] / (double)n;

  if(xmoy > 1.e-30) {
    double tol = xmoy * 1.e-10, errsqr = 0.;
    for(int i = low; i < high; i++)
      errsqr += (nx[i - low] > tol) ? ndx[i - low] / nx[i - low] : ndx[i - low];
    Combine(distributed, &errsqr, 1, NULL);
    *MeanError = errsqr / (double)n;
  }
  else {
    if(dxmoy > 1.e-30)
      *MeanError = 1.;
    else
      *MeanError = 0.;
  }
}
