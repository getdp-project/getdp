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

  // dx <- Jac^-1 res
  LinAlg_Solve(&DofData_P->Jac, &DofData_P->res, &DofData_P->Solver,
               &DofData_P->dx);

  // keep copy of current solution
  gVector x_saved;
  LinAlg_CreateVector(&x_saved, &DofData_P->Solver, DofData_P->NbrDof);
  LinAlg_CopyVector(&DofData_P->CurrentSolution->x, &x_saved);

  double step = 1.;

  for(int i = 0; i < 42; i++) {
    // typical operations you'll need:

    // call the SolveJacLineSearch operations (Python[], GenerateJac[], ...)
    Treatment_Operation(Resolution_P,
                        Operation_P->Case.SolveJacLineSearch.Operation,
                        DofData_P0, GeoData_P0, NULL, NULL);

    // compute the residual
    LinAlg_ProdMatrixVector(&DofData_P->A, &DofData_P->CurrentSolution->x,
                            &DofData_P->res);
    LinAlg_SubVectorVector(&DofData_P->b, &DofData_P->res, &DofData_P->res);

    // ...
  }

  // restore solution
  LinAlg_CopyVector(&x_saved, &DofData_P->CurrentSolution->x);

  // update with chosen step
  LinAlg_AddVectorProdVectorDouble(&DofData_P->CurrentSolution->x,
                                   &DofData_P->dx, step,
                                   &DofData_P->CurrentSolution->x);

}
