// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef CAL_GALERKIN_TERM_OF_FEM_EQUATION_H
#define CAL_GALERKIN_TERM_OF_FEM_EQUATION_H

#include "ProData.h"

void Cal_InitGalerkinTermOfFemEquation(
  struct EquationTerm *EquationTerm_P,
  struct QuantityStorage *QuantityStorage_P0,
  struct QuantityStorage *QuantityStorageNoDof, struct Dof *DofForNoDof_P);

void Cal_GalerkinTermOfFemEquation(struct Element *Element,
                                   struct EquationTerm *EquationTerm_P,
                                   struct QuantityStorage *QuantityStorage_P0);

void Cal_EndGalerkinTermOfFemEquation();

// While the elementary matrix of a Galerkin term is computed by a numerical
// quadrature: the values in the reference element (3 components each) of the
// n given functions of the element at the current integration point, if the
// point (u, v, w) of the element is that point and the values are known (see
// the reference values in Cal_GalerkinTermOfFemEquation.cpp); NULL otherwise
const double *Cal_GetReferenceValuesAtIntegrationPoint(
  struct Element *Element, double u, double v, double w, int n,
  void (**Functions)(), struct QuantityStorage *QuantityStorage);

/* In F_MultiHar */
void Cal_InitGalerkinTermOfFemEquation_MHBilinear(
  struct EquationTerm *EquationTerm_P);
void Cal_GalerkinTermOfFemEquation_MHBilinear(
  struct Element *Element, struct EquationTerm *EquationTerm_P,
  struct QuantityStorage *QuantityStorage_P0);

#endif
