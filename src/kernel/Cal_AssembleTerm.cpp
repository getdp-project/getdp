// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//   Johan Gyselinck
//

#include "ProData.h"
#include "DofData.h"
#include "Message.h"
#include <math.h>

#define SQU(a) ((a) * (a))

extern struct CurrentData Current;

static int Warning_Dt = 0, Warning_DtStatic = 0;
static int Warning_DtDt = 0, Warning_DtDtStatic = 0, Warning_DtDtFirstOrder = 0;

/* ------------------------------------------------------------------------ */
/*  Matrices and vectors of GenerateSeparate                                */
/* ------------------------------------------------------------------------ */

// Creates the k-th matrix of GenerateSeparate (M1 to M7) and its vectors, if
// they do not exist yet. They are created the first time a term is assembled
// in them: which ones exist tells e.g. the eigensolvers the kind of problem.
void Cal_InitSeparate(int k)
{
  struct DofData *D = Current.DofData;
  if(D->Flag_Init[k]) return;
  D->Flag_Init[k] = 1;
  gMatrix *M[8] = {NULL,   &D->M1, &D->M2, &D->M3,
                   &D->M4, &D->M5, &D->M6, &D->M7};
  gVector *m[8] = {NULL,   &D->m1, &D->m2, &D->m3,
                   &D->m4, &D->m5, &D->m6, &D->m7};
  List_T **ms[8] = {NULL,    &D->m1s, &D->m2s, &D->m3s,
                    &D->m4s, &D->m5s, &D->m6s, &D->m7s};
  LinAlg_CreateMatrix(M[k], &D->Solver, D->NbrDof, D->NbrDof);
  LinAlg_CreateVector(m[k], &D->Solver, D->NbrDof);
  LinAlg_ZeroMatrix(M[k]);
  LinAlg_ZeroVector(m[k]);
  *ms[k] = List_Create(10, 10, sizeof(gVector));
  for(int i = 0; i < List_Nbr(D->TimeFunctionIndex); i++) {
    gVector v;
    LinAlg_CreateVector(&v, &D->Solver, D->NbrDof);
    LinAlg_ZeroVector(&v);
    List_Add(*ms[k], &v);
  }
}

// The matrix of GenerateSeparate in which a term with the given type of time
// derivative is assembled (0 if none)
int Cal_GetSeparateIndex(int TypeTimeDerivative)
{
  switch(TypeTimeDerivative) {
  case NODT_:
  case NEVERDT_: return 1;
  case DTDOF_:
  case NLEIG6DOF_: return 2;
  case DTDTDOF_:
  case NLEIG5DOF_: return 3;
  case DTDTDTDOF_:
  case NLEIG4DOF_: return 4;
  case DTDTDTDTDOF_:
  case NLEIG3DOF_: return 5;
  case DTDTDTDTDTDOF_:
  case NLEIG2DOF_: return 6;
  case NLEIG1DOF_: return 7;
  default: return 0;
  }
}

/* ------------------------------------------------------------------------ */
/*  No Time Derivative                                                      */
/* ------------------------------------------------------------------------ */

void Cal_AssembleTerm_NoDt(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  double tmp[2];

  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(1);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M1, &Current.DofData->m1,
                        Current.DofData->m1s);
    }
  }
  else {
    if(Current.NbrHar == 1) {
      switch(Current.TypeTime) {
      case TIME_STATIC:
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, &Val[0],
                          &Current.DofData->A, &Current.DofData->b);
        break;
      case TIME_THETA:
        tmp[0] = Val[0] * Current.Theta;
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->A,
                          &Current.DofData->b);
        tmp[0] = Val[0] * (Current.Theta - 1.);
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 1,
          &(Current.DofData->CurrentSolution - 1)->x, &Current.DofData->b);
        break;
      case TIME_NEWMARK:
        tmp[0] = Val[0] * Current.Beta;
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->A,
                          &Current.DofData->b);
        tmp[0] = Val[0] * (2 * Current.Beta - Current.Gamma - 0.5);
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 1,
          &(Current.DofData->CurrentSolution - 1)->x, &Current.DofData->b);
        tmp[0] = Val[0] * (Current.Gamma - Current.Beta - 0.5);
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 2,
          &(Current.DofData->CurrentSolution - 2)->x, &Current.DofData->b);
        break;
      case TIME_GEAR:
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, Val, &Current.DofData->A,
                          &Current.DofData->b);
        break;
      }
    }
    else {
      for(k = 0; k < Current.NbrHar; k += 2) {
        int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
        Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                          &Current.DofData->A, &Current.DofData->b);
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  First order Time Derivative                                             */
/* ------------------------------------------------------------------------ */

void Cal_AssembleTerm_DtDof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  double tmp[2];

  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(2);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M2, &Current.DofData->m2,
                        Current.DofData->m2s);
    }
  }
  else {
    if(Current.NbrHar == 1) {
      switch(Current.TypeTime) {
      case TIME_STATIC:
        if(!Warning_DtStatic) {
          Message::Info(3, "Discarded DtDof term in static analysis");
          Warning_DtStatic = 1;
        }
        break;
      case TIME_THETA:
        tmp[0] = Val[0] / Current.DTime;
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->A,
                          &Current.DofData->b);
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 1,
          &(Current.DofData->CurrentSolution - 1)->x, &Current.DofData->b);
        break;
      case TIME_NEWMARK:
        tmp[0] = Val[0] * Current.Gamma / Current.DTime;
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->A,
                          &Current.DofData->b);
        tmp[0] = Val[0] * (2. * Current.Gamma - 1.) / Current.DTime;
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 1,
          &(Current.DofData->CurrentSolution - 1)->x, &Current.DofData->b);
        tmp[0] = Val[0] * (1. - Current.Gamma) / Current.DTime;
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 2,
          &(Current.DofData->CurrentSolution - 2)->x, &Current.DofData->b);
        break;
      case TIME_GEAR:
        tmp[0] = Val[0] / (Current.bCorrCoeff * Current.DTime);
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->A,
                          &Current.DofData->b);
        for(int i = 0; i < Current.CorrOrder; i++) {
          tmp[0] = Val[0] * Current.aCorrCoeff[i] /
                   (Current.bCorrCoeff * Current.DTime);
          Dof_AssembleInVec(Equ, Dof, Current.NbrHar, tmp,
                            Current.DofData->CurrentSolution - 1 - i,
                            &(Current.DofData->CurrentSolution - 1 - i)->x,
                            &Current.DofData->b);
        }
        break;
      }
    }
    else {
      for(k = 0; k < Current.NbrHar; k += 2) {
        // printf("====hola idiota=== k %d pul %g \n", k,
        // Current.DofData->Val_Pulsation[k/2]);
        tmp[0] = -Val[k + 1] * Current.DofData->Val_Pulsation[k / 2];
        tmp[1] = Val[k] * Current.DofData->Val_Pulsation[k / 2];
        int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
        Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, tmp,
                          &Current.DofData->A, &Current.DofData->b);
      }
    }
  }
}

void Cal_AssembleTerm_Dt(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  if(!Warning_Dt) {
    Message::Warning("Dt not implemented, using DtDof instead");
    Warning_Dt = 1;
  }
  Cal_AssembleTerm_DtDof(Equ, Dof, Val);
}

/* En preparation ... */
void Cal_AssembleTerm_DtNL(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  double tmp[2];

  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Message::Error("DtNL not implemented for separate assembly");
  }
  else {
    if(Current.NbrHar == 1) {
      switch(Current.TypeTime) {
      case TIME_STATIC:
        if(!Warning_DtStatic) {
          Message::Info(3, "Discarded DtDof term in static analysis");
          Warning_DtStatic = 1;
        }
        break;
      case TIME_THETA:
        tmp[0] = Val[0] / Current.DTime;
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->A,
                          &Current.DofData->b);
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 1,
          &(Current.DofData->CurrentSolution - 1)->x, &Current.DofData->b);
        break;
      case TIME_NEWMARK:
        Message::Error(
          "DtNL not implemented for separate assembly with Newmark scheme");
        return;
      case TIME_GEAR:
        Message::Error("DtNL not implemented for Gear's method");
        return;
      }
    }
    else {
      Message::Error(
        "DtNL not implemented for separate assembly in harmonic analysis");
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  Second order Time Derivative                                            */
/* ------------------------------------------------------------------------ */

void Cal_AssembleTerm_DtDtDof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  double tmp[2];

  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(3);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M3, &Current.DofData->m3,
                        Current.DofData->m3s);
    }
  }
  else {
    if(Current.NbrHar == 1) {
      switch(Current.TypeTime) {
      case TIME_STATIC:
        if(!Warning_DtDtStatic) {
          Message::Info(3, "Discarded DtDtDof term in static analysis");
          Warning_DtDtStatic = 1;
        }
        break;
      case TIME_THETA:
        if(!Warning_DtDtFirstOrder) {
          Message::Info(3, "Discarded DtDtDof term in Theta time scheme");
          Warning_DtDtFirstOrder = 1;
        }
        break;
      case TIME_GEAR:
        Message::Error("DtDtDof not implemented for Gear's method");
        return;
      case TIME_NEWMARK:
        tmp[0] = Val[0] / SQU(Current.DTime);
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->A,
                          &Current.DofData->b);
        tmp[0] = 2 * Val[0] / SQU(Current.DTime);
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 1,
          &(Current.DofData->CurrentSolution - 1)->x, &Current.DofData->b);
        tmp[0] = -Val[0] / SQU(Current.DTime);
        Dof_AssembleInVec(
          Equ, Dof, Current.NbrHar, tmp, Current.DofData->CurrentSolution - 2,
          &(Current.DofData->CurrentSolution - 2)->x, &Current.DofData->b);
        break;
      }
    }
    else {
      for(k = 0; k < Current.NbrHar; k += 2) {
        tmp[0] = -Val[k] * SQU(Current.DofData->Val_Pulsation[k / 2]);
        tmp[1] = -Val[k + 1] * SQU(Current.DofData->Val_Pulsation[k / 2]);
        int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
        Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, tmp,
                          &Current.DofData->A, &Current.DofData->b);
      }
    }
  }
}

void Cal_AssembleTerm_DtDt(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  if(!Warning_DtDt) {
    Message::Warning("DtDt not implemented, using DtDtDof instead");
    Warning_DtDt = 1;
  }
  Cal_AssembleTerm_DtDtDof(Equ, Dof, Val);
}

/* ------------------------------------------------- */
/*  higher order Time Derivative for Polynomial EVP  */
/* ------------------------------------------------- */
void Cal_AssembleTerm_DtDtDtDof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(4);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M4, &Current.DofData->m4,
                        Current.DofData->m4s);
    }
  }
  else {
    Message::Error("DtDtDtDof only available with GenerateSeparate");
    return;
  }
}

void Cal_AssembleTerm_DtDtDtDtDof(struct Dof *Equ, struct Dof *Dof,
                                  double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(5);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M5, &Current.DofData->m5,
                        Current.DofData->m5s);
    }
  }
  else {
    Message::Error("DtDtDtDtDof only available with GenerateSeparate");
    return;
  }
}

void Cal_AssembleTerm_DtDtDtDtDtDof(struct Dof *Equ, struct Dof *Dof,
                                    double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(6);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M6, &Current.DofData->m6,
                        Current.DofData->m6s);
    }
  }
  else {
    Message::Error("DtDtDtDtDtDof only available with GenerateSeparate");
    return;
  }
}

// nleigchange
void Cal_AssembleTerm_NLEig1Dof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(7);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M7, &Current.DofData->m7,
                        Current.DofData->m7s);
    }
  }
  else {
    Message::Error("NLEig1Dof only available with GenerateSeparate");
    return;
  }
}

void Cal_AssembleTerm_NLEig2Dof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(6);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M6, &Current.DofData->m6,
                        Current.DofData->m6s);
    }
  }
  else {
    Message::Error("NLEig3Dof only available with GenerateSeparate");
    return;
  }
}

void Cal_AssembleTerm_NLEig3Dof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(5);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M5, &Current.DofData->m5,
                        Current.DofData->m5s);
    }
  }
  else {
    Message::Error("NLEig3Dof only available with GenerateSeparate");
    return;
  }
}

void Cal_AssembleTerm_NLEig4Dof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(4);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M4, &Current.DofData->m4,
                        Current.DofData->m4s);
    }
  }
  else {
    Message::Error("NLEig4Dof only available with GenerateSeparate");
    return;
  }
}

void Cal_AssembleTerm_NLEig5Dof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(3);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M3, &Current.DofData->m3,
                        Current.DofData->m3s);
    }
  }
  else {
    Message::Error("NLEig5Dof only available with GenerateSeparate");
    return;
  }
}

void Cal_AssembleTerm_NLEig6Dof(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;
  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(2);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M2, &Current.DofData->m2,
                        Current.DofData->m2s);
    }
  }
  else {
    Message::Error("NLEig6Dof only available with GenerateSeparate");
    return;
  }
}

/* ------------------------------------------------------------------------ */
/*  Jacobian NonLinear                                                      */
/* ------------------------------------------------------------------------ */

void Cal_AssembleTerm_JacNL(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;

  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Message::Error("JacNL not implemented for separate assembly");
  }
  else {
    if(Current.NbrHar == 1) {
      switch(Current.TypeTime) {
      case TIME_STATIC:
      case TIME_THETA:
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, &Val[0],
                          &Current.DofData->Jac, NULL);
        break;
      case TIME_GEAR:
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, &Val[0],
                          &Current.DofData->Jac, NULL);
        break;
      case TIME_NEWMARK:
        Message::Error("JacNL not implemented for Newmark's method");
        return;
      }
    }
    else {
      for(k = 0; k < Current.NbrHar; k += 2) {
        int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
        Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                          &Current.DofData->Jac, NULL);
      }
    }
  }
}

void Cal_AssembleTerm_DtDofJacNL(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  double tmp[2];

  if(Current.TypeAssembly == ASSEMBLY_SEPARATE)
    Message::Error("DtDofJacNL not implemented for separate assembly");
  else {
    if(Current.NbrHar == 1) {
      switch(Current.TypeTime) {
      case TIME_STATIC:
        if(!Warning_DtStatic) {
          Message::Info(3, "Discarded DtDofJacNL term in static analysis");
          Warning_DtStatic = 1;
        }
        break;
      case TIME_THETA:
        if(fabs(Current.Theta - 1.0) > 1e-3) {
          Message::Error(
            "Theta method not implemented for nonlinear problems when "
            "Theta != 1.0");
          return;
        }
        tmp[0] = Val[0] / Current.DTime;
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->Jac,
                          NULL);
        break;
      case TIME_NEWMARK:
        Message::Error("DtDofJacNL not implemented for Newmark scheme");
        return;
      case TIME_GEAR:
        tmp[0] = Val[0] / (Current.bCorrCoeff * Current.DTime);
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, tmp, &Current.DofData->Jac,
                          NULL);
        break;
      }
    }
    else
      Message::Error("DtDofJacNL not implemented for harmonic analysis");
  }
}

/* ------------------------------------------------------------------------ */
/*  Never Time Derivative  (provisoire mais tres important ... Patrick)     */
/* ------------------------------------------------------------------------ */

void Cal_AssembleTerm_NeverDt(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  int k;

  if(Current.TypeAssembly == ASSEMBLY_SEPARATE) {
    Cal_InitSeparate(1);
    for(k = 0; k < Current.NbrHar; k += 2) {
      int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
      Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                        &Current.DofData->M1, &Current.DofData->m1,
                        Current.DofData->m1s);
    }
  }
  else {
    if(Current.NbrHar == 1) {
      switch(Current.TypeTime) {
      case TIME_STATIC:
      case TIME_THETA:
      case TIME_NEWMARK:
      case TIME_GEAR:
        Dof_AssembleInMat(Equ, Dof, Current.NbrHar, &Val[0],
                          &Current.DofData->A, &Current.DofData->b);
        break;
      }
    }
    else {
      for(k = 0; k < Current.NbrHar; k += 2) {
        int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
        Dof_AssembleInMat(Equ + incr, Dof + incr, Current.NbrHar, &Val[k],
                          &Current.DofData->A, &Current.DofData->b);
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  Multi-Harmonic with movement                                            */
/* ------------------------------------------------------------------------ */

void Cal_AssembleTerm_MHMoving(struct Dof *Equ, struct Dof *Dof, double Val[])
{
  // MHMoving_assemblyType = 1 => Use current system A,b
  // MHMoving_assemblyType = 2 => Use dedicated system A_MH_Moving, b_MH_Moving
  // MHMoving_assemblyType = 3 => Look for unknowns and constraints in Moving
  // Group

  extern int MHMoving_assemblyType;
  extern double **MH_Moving_Matrix;
  extern Tree_T *DofTree_MH_moving;

  // FIXME: this cannot work in complex arithmetic: AssembleInMat will
  // need to assemble both real and imaginary parts at once -- See
  // Cal_AssembleTerm for an example

  if(MHMoving_assemblyType == 1) {
    for(int k = 0; k < Current.NbrHar; k++)
      for(int l = 0; l < Current.NbrHar; l++) {
        double tmp = Val[0] * MH_Moving_Matrix[k][l];
        // if (k==l)
        Dof_AssembleInMat(Equ + k, Dof + l, 1, &tmp, &Current.DofData->A,
                          &Current.DofData->b);
      }
  }

  if(MHMoving_assemblyType == 2) {
    for(int k = 0; k < Current.NbrHar; k++)
      for(int l = 0; l < Current.NbrHar; l++) {
        double tmp = Val[0] * MH_Moving_Matrix[k][l];
        // if (k==l)
        Dof_AssembleInMat(Equ + k, Dof + l, 1, &tmp,
                          &Current.DofData->A_MH_moving, NULL);
        // &Current.DofData->A_MH_moving, &Current.DofData->b_MH_moving) ;
      }
  }

  if(MHMoving_assemblyType == 3) {
    if(Dof->Type == DOF_UNKNOWN && !Tree_PQuery(DofTree_MH_moving, Dof))
      Tree_Add(DofTree_MH_moving, Dof);
    else if(Dof->Type == DOF_LINK &&
            !Tree_PQuery(DofTree_MH_moving, Dof->Case.Link.Dof))
      Tree_Add(DofTree_MH_moving, Dof->Case.Link.Dof);

    if(Equ->Type == DOF_UNKNOWN && !Tree_PQuery(DofTree_MH_moving, Equ))
      Tree_Add(DofTree_MH_moving, Equ);
    else if(Equ->Type == DOF_LINK &&
            !Tree_PQuery(DofTree_MH_moving, Equ->Case.Link.Dof))
      Tree_Add(DofTree_MH_moving, Equ->Case.Link.Dof);
  }
}
