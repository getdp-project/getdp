// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//

#include "ProData.h"
#include "DofData.h"
#include "SolvingOperations.h"
#include "Message.h"
/* ------------------------------------------------------------------------ */
/*  O p e r a t i o n _ S o l v e J a c L i n e S e a r c h                 */
/* ------------------------------------------------------------------------ */

extern struct CurrentData Current;

void Operation_SolveJacLineSearch
  (Resolution *Resolution_P, Operation *Operation_P, DofData *DofData_P,
   DofData *DofData_P0, GeoData *GeoData_P0)
{
  if(DofData_P->Flag_Init[0] < 2) {
    Message::Error("Jacobian not initialized (missing GenerateJac?)");
    return;
  }

  if(!DofData_P->CurrentSolution) {
    Message::Error("No current solution available");
    return;
  }

  Message::Info("Hello doing SolveJacLineSearch with super parameter %g!",
                Operation_P->Case.SolveJacLineSearch.SuperParameter);

  double c1 = Operation_P->Case.SolveJacLineSearch.SuperParameter;
  // dx <- Jac^-1 res
  LinAlg_Solve(&DofData_P->Jac, &DofData_P->res, &DofData_P->Solver,
               &DofData_P->dx);

  // keep copy of current solution
  gVector x_saved;
  LinAlg_CreateVector(&x_saved, &DofData_P->Solver, DofData_P->NbrDof);
  LinAlg_CopyVector(&DofData_P->CurrentSolution->x, &x_saved);

  // compute the residual and norm of previous x
  double Norm_old;
  double f_old;
  LinAlg_ProdMatrixVector(&DofData_P->A, &DofData_P->CurrentSolution->x,
                      &DofData_P->res);
  LinAlg_SubVectorVector(&DofData_P->b, &DofData_P->res, &DofData_P->res);
  LinAlg_VectorNorm2(&DofData_P->res, &Norm_old);
  f_old = (1./2.)*Norm_old*Norm_old;

  /* Compute descent*/
  double Descent = (-1.) * Norm_old*Norm_old;
  Message::Info("Descent: %f",Descent);

  double step = 1.;
  double epsilon_linesearch = 1e-12;
  int NbrSteps_relax = 20;
  double Norm;
  double f_new;

  /*Armijo backtracking w.r.t the merit function f(x) = (1/2) ||r(x)||^2 */
  for(int it = 0; it < NbrSteps_relax; it++) {
    //compute trial step
    LinAlg_CopyVector(&x_saved, &DofData_P->CurrentSolution->x);
    LinAlg_AddVectorProdVectorDouble(&DofData_P->CurrentSolution->x,
                              &DofData_P->dx, step,
                              &DofData_P->CurrentSolution->x);

    //compute residual and norm of new step x = x + step dx
    Treatment_Operation(Resolution_P,
                        Operation_P->Case.SolveJacLineSearch.Operation,
                        DofData_P0, GeoData_P0, NULL, NULL);
    LinAlg_ProdMatrixVector(&DofData_P->A, &DofData_P->CurrentSolution->x,
                &DofData_P->res);
    LinAlg_SubVectorVector(&DofData_P->b, &DofData_P->res, &DofData_P->res);
    LinAlg_VectorNorm2(&DofData_P->res, &Norm);
    f_new = (1./2.)*Norm*Norm;

    Message::Info("f_new = %g", f_new);

    /* termination criteria*/
    if(f_new <  (f_old + c1 * step * Descent + epsilon_linesearch*f_old) ) {
      Message::Info("Lineserach complete");
      Message::Info("fnew: %g, fold: %g, step: %g",f_new,f_old,step);
      Current.RelativeDifference = Norm;
      break;
    }
    else{
      step = step / 2;
    }
    if(it == NbrSteps_relax - 1) {
      //Message::Warning(
      //  "SolveJacAdapt: LineSearch failed at TimeStep %g iter %g with istep: %g and NbrSteps_relax: %g",
      //  Current.TimeStep, Current.Iteration,it,NbrSteps_relax);
      //Current.SolveJacAdaptFailed = 1;
    }
  }
  LinAlg_DestroyVector(&x_saved);
}
