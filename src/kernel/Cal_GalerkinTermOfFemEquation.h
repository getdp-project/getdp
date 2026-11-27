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

// Outside of the Galerkin terms (e.g. in a Print): declares that the point
// (u, v, w) of the element is the point Index of a set of points given, as a
// quadrature, for all the elements of its type, so that the values in the
// reference element of the functions at these points are computed once (as at
// the integration points). To be reset, with a NULL element, after the
// evaluations at the point.
void Cal_SetReferencePoint(struct Element *Element, struct Quadrature *Points,
                           int Index, double u, double v, double w);

// A number that changes each time the first point (Index 0) of a set is
// given for the element by Cal_SetReferencePoint, i.e. for each element and,
// in a Print, each time step; 0 if no such point is set. What only depends on
// the element and on the solution (the values of the Dofs) can be kept as long
// as it does not change.
unsigned long long Cal_GetReferencePointPass(struct Element *Element);

/* In F_MultiHar */
void Cal_InitGalerkinTermOfFemEquation_MHBilinear(
  struct EquationTerm *EquationTerm_P);
void Cal_GalerkinTermOfFemEquation_MHBilinear(
  struct Element *Element, struct EquationTerm *EquationTerm_P,
  struct QuantityStorage *QuantityStorage_P0);

#endif
