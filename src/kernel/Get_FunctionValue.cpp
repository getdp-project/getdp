// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <stdlib.h>
#include <math.h>
#include "ProData.h"
#include "GeoData.h"
#include "DofData.h"
#include "Cal_Quantity.h"
#include "Get_Geometry.h"
#include "BF.h"
#include "Gauss.h"
#include "Message.h"

#define SQU(a) ((a) * (a))

extern struct Problem Problem_S;
extern struct CurrentData Current;

/* ------------------------------------------------------------------------ */
/*  G e t _ V a l u e F r o m F o r m                                       */
/* ------------------------------------------------------------------------ */

int Get_ValueFromForm(int Form)
{
  switch(Form) {
  case FORM0:
  case FORM3:
  case FORM3P:
  case SCALAR: return (SCALAR);

  case FORM1:
  case FORM1P:
  case FORM1S:
  case FORM2:
  case FORM2P:
  case FORM2S:
  case VECTOR:
  case VECTORP: return (VECTOR);

  default:
    Message::Error("Unknown Form type in 'Get_ValueFromForm'");
    return (-1);
  }
}

/* ------------------------------------------------------------------------ */
/*  G e t _ I n t e g r a t i o n C a s e                                   */
/* ------------------------------------------------------------------------ */

/*
   Il faudrait reorganiser les 'Current.XXX'
   Ca devient un peu le bordel.
   */

struct IntegrationCase *Get_IntegrationCase(struct Element *Element,
                                            List_T *IntegrationCase_L,
                                            int CriterionIndex)
{
  struct Value Criterion;

  if(CriterionIndex >= 0) {
    Current.Element = Element;
    Current.ElementSource = Element->ElementSource;
    Get_ValueOfExpression(
      (struct Expression *)List_Pointer(Problem_S.Expression, CriterionIndex),
      NULL, 0., 0., 0., &Criterion);
    if(Criterion.Val[0] < 0 || Criterion.Val[0] >= List_Nbr(IntegrationCase_L))
      Message::Error("Integration criterion out of range");
  }
  else {
    if(List_Nbr(IntegrationCase_L) > 1)
      Message::Error("Missing integration criterion");
    Criterion.Val[0] = 0;
  }

  return ((struct IntegrationCase *)List_Pointer(IntegrationCase_L,
                                                 (int)Criterion.Val[0]));
}

/* ------------------------------------------------------------------------ */
/*  G e t _ Q u a d r a t u r e O f E l e m e n t                           */
/* ------------------------------------------------------------------------ */

// The quadrature of an integration case for an element: the one given for
// the type of the element or, with Integration Auto, the smallest rule that
// is exact for the polynomials of the given degree (a negative Degree means
// that the degree of the integrand is not known). Returns NULL if there is
// none.
struct Quadrature *Get_QuadratureOfElement(
  struct IntegrationCase *IntegrationCase_P, struct Element *Element,
  int Degree)
{
  if(!IntegrationCase_P->Auto)
    return (struct Quadrature *)List_PQuery(IntegrationCase_P->Case,
                                            &Element->Type, fcmp_int);
  if(Degree < 0) {
    Message::Error("Integration Auto cannot be used here (the degree of the "
                   "integrand is not known): define an Integration method");
    return NULL;
  }
  return Get_AutomaticQuadrature(Element->Type,
                                 Degree + IntegrationCase_P->AutoExtra);
}

// the order of the geometry of the elements of a type, and whether they are
// simplices
int Get_GeometricOrder(int Type, bool *Simplex)
{
  *Simplex = false;
  switch(Type) {
  case POINT_ELEMENT:
  case LINE:
  case TRIANGLE:
  case TETRAHEDRON: *Simplex = true; return 1;
  case LINE_2:
  case TRIANGLE_2:
  case TETRAHEDRON_2: *Simplex = true; return 2;
  case LINE_3:
  case TRIANGLE_3:
  case TETRAHEDRON_3: *Simplex = true; return 3;
  case LINE_4:
  case TRIANGLE_4:
  case TETRAHEDRON_4: *Simplex = true; return 4;
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case PRISM_2:
  case PRISM_2_15N:
  case PYRAMID_2:
  case PYRAMID_2_13N: return 2;
  case QUADRANGLE_3:
  case HEXAHEDRON_3:
  case PRISM_3:
  case PYRAMID_3: return 3;
  case QUADRANGLE_4:
  case HEXAHEDRON_4:
  case PRISM_4: return 4;
  default: return 1;
  }
}

// The degree of the polynomials that the basis functions of a quantity (or
// their derivative, for another operator than NoOp) are in the reference
// element: the total degree on the simplices, the degree in each variable on
// the other elements.
int Get_IntegrationDegree(struct Element *Element,
                          struct QuantityStorage *QuantityStorage_P,
                          int Type_Operator)
{
  bool simplex;
  const int geometric = Get_GeometricOrder(Element->Type, &simplex);
  int degree = 0;
  for(int i = 0; i < QuantityStorage_P->NbrElementaryBasisFunction; i++) {
    struct BasisFunction *BF = QuantityStorage_P->BasisFunction[i].BasisFunction;
    if(!BF) continue;
    // the orders of the vector functions are half integers for the
    // incomplete polynomials (e.g. 0.5 for the Whitney functions, of degree
    // 1)
    int d = (int)ceil(BF->Order - 1.e-6);
    // the nodal functions have the order of the element
    if(BF->Function == (void (*)())BF_Node && d < geometric) d = geometric;
    if(d > degree) degree = d;
  }
  // the derivative loses one degree on the simplices only: on the other
  // elements the degree in the other variables is kept
  if(Type_Operator != NOOP && simplex && degree > 0) degree--;
  return degree;
}

// What the geometry adds to the degree of an integrand: nothing on the
// straight simplices, whose Jacobian is constant; one degree on the other
// straight elements, and two per order of the geometry beyond the first on
// the curved ones (the integrands are then rational functions: these are
// rules of thumb)
int Get_GeometricIntegrationDegree(struct Element *Element)
{
  bool simplex;
  const int geometric = Get_GeometricOrder(Element->Type, &simplex);
  return (simplex ? 0 : 1) + 2 * (geometric - 1);
}

/* ------------------------------------------------------------------------ */
/*  G e t _ F u n c t i o n V a l u e                                       */
/* ------------------------------------------------------------------------ */

void Get_FunctionValue(int Nbr_Function, void (*xFunctionBF[])(),
                       int Type_Operator,
                       struct QuantityStorage *QuantityStorage_P,
                       int *Type_Form)
{
  int i;

  switch(Type_Operator) {
  case NOOP:
    *Type_Form = QuantityStorage_P->TypeQuantity;
    for(i = 0; i < Nbr_Function; i++)
      xFunctionBF[i] =
        QuantityStorage_P->BasisFunction[i].BasisFunction->Function;
    break;

  case EXTDER:
    *Type_Form = QuantityStorage_P->TypeQuantity + 1;
    for(i = 0; i < Nbr_Function; i++)
      xFunctionBF[i] =
        QuantityStorage_P->BasisFunction[i].BasisFunction->dFunction;
    break;

  case EXTDERINV:
    *Type_Form = QuantityStorage_P->TypeQuantity - 1;
    for(i = 0; i < Nbr_Function; i++)
      xFunctionBF[i] =
        QuantityStorage_P->BasisFunction[i].BasisFunction->dInvFunction;
    break;

  case GRAD:
    if(QuantityStorage_P->TypeQuantity == FORM0) {
      *Type_Form = QuantityStorage_P->TypeQuantity + 1;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dFunction;
    }
    else if(QuantityStorage_P->TypeQuantity == SCALAR) {
      *Type_Form = VECTOR;
    }
    else {
      Message::Error("Cannot apply Grad operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case CURL:
    if((QuantityStorage_P->TypeQuantity == FORM1) ||
       (QuantityStorage_P->TypeQuantity == FORM1P)) {
      *Type_Form = QuantityStorage_P->TypeQuantity + 1;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dFunction;
    }
    else if(QuantityStorage_P->TypeQuantity == VECTOR) {
      *Type_Form = VECTOR;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dFunction;
    }
    else {
      Message::Error("Cannot apply Curl operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case DIV:
    if(QuantityStorage_P->TypeQuantity == FORM2) {
      *Type_Form = QuantityStorage_P->TypeQuantity + 1;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dFunction;
    }
    else if(QuantityStorage_P->TypeQuantity == VECTOR) {
      *Type_Form = SCALAR;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dInvFunction;
    }
    else {
      Message::Error("Cannot apply Div operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case GRADINV:
    if(QuantityStorage_P->TypeQuantity == FORM1) {
      *Type_Form = QuantityStorage_P->TypeQuantity - 1;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dInvFunction;
    }
    else if(QuantityStorage_P->TypeQuantity == VECTOR) {
      *Type_Form = SCALAR;
    }
    else {
      Message::Error("Cannot apply GradInv operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case CURLINV:
    if(QuantityStorage_P->TypeQuantity == FORM2) {
      *Type_Form = QuantityStorage_P->TypeQuantity - 1;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dInvFunction;
    }
    else if(QuantityStorage_P->TypeQuantity == VECTOR) {
      *Type_Form = VECTOR;
    }
    else {
      Message::Error("Cannot apply CurlInv operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case DIVINV:
    if((QuantityStorage_P->TypeQuantity == FORM3) ||
       (QuantityStorage_P->TypeQuantity == FORM3P)) {
      *Type_Form = QuantityStorage_P->TypeQuantity - 1;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dInvFunction;
    }
    else if(QuantityStorage_P->TypeQuantity == SCALAR) {
      *Type_Form = VECTOR;
    }
    else {
      Message::Error("Cannot apply DivInv operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case OP_D1:
    if(QuantityStorage_P->TypeQuantity == VECTOR) {
      *Type_Form = VECTOR;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dFunction;
    }
    else {
      Message::Error("Cannot apply D1 operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case OP_D2:
    if(QuantityStorage_P->TypeQuantity == VECTOR) {
      *Type_Form = VECTOR;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dInvFunction;
    }
    else {
      Message::Error("Cannot apply D2 operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  case OP_D3:
    if(QuantityStorage_P->TypeQuantity == VECTOR) {
      *Type_Form = VECTOR;
      for(i = 0; i < Nbr_Function; i++)
        xFunctionBF[i] =
          QuantityStorage_P->BasisFunction[i].BasisFunction->dPlusFunction;
    }
    else {
      Message::Error("Cannot apply D3 operator to quantity type %d",
                     QuantityStorage_P->TypeQuantity);
    }
    break;

  default: Message::Error("Unknown operator in 'Get_FunctionValue'"); break;
  }
}

/* ------------------------------------------------------------------------ */
/*  G e t _ I n i t F u n c t i o n V a l u e                               */
/* ------------------------------------------------------------------------ */

void Get_InitFunctionValue(int Type_Operator,
                           struct QuantityStorage *QuantityStorage_P,
                           int *Type_Form)
{
  switch(Type_Operator) {
  case NOOP: *Type_Form = QuantityStorage_P->TypeQuantity; break;

  case EXTDER: *Type_Form = QuantityStorage_P->TypeQuantity + 1; break;

  case EXTDERINV: *Type_Form = QuantityStorage_P->TypeQuantity - 1; break;

  case GRAD:
    if(QuantityStorage_P->TypeQuantity == FORM0)
      *Type_Form = QuantityStorage_P->TypeQuantity + 1;
    else if(QuantityStorage_P->TypeQuantity == SCALAR)
      *Type_Form = VECTOR;
    break;

  case CURL:
    if((QuantityStorage_P->TypeQuantity == FORM1) ||
       (QuantityStorage_P->TypeQuantity == FORM1P))
      *Type_Form = QuantityStorage_P->TypeQuantity + 1;
    else if(QuantityStorage_P->TypeQuantity == VECTOR)
      *Type_Form = VECTOR;
    break;

  case DIV:
    if(QuantityStorage_P->TypeQuantity == FORM2)
      *Type_Form = QuantityStorage_P->TypeQuantity + 1;
    else if(QuantityStorage_P->TypeQuantity == VECTOR)
      *Type_Form = SCALAR;
    break;

  case GRADINV:
    if(QuantityStorage_P->TypeQuantity == FORM1)
      *Type_Form = QuantityStorage_P->TypeQuantity - 1;
    else if(QuantityStorage_P->TypeQuantity == VECTOR)
      *Type_Form = SCALAR;
    break;

  case CURLINV:
    if(QuantityStorage_P->TypeQuantity == FORM2)
      *Type_Form = QuantityStorage_P->TypeQuantity - 1;
    else if(QuantityStorage_P->TypeQuantity == VECTOR)
      *Type_Form = VECTOR;
    break;

  case DIVINV:
    if((QuantityStorage_P->TypeQuantity == FORM3) ||
       (QuantityStorage_P->TypeQuantity == FORM3P))
      *Type_Form = QuantityStorage_P->TypeQuantity - 1;
    else if(QuantityStorage_P->TypeQuantity == SCALAR)
      *Type_Form = VECTOR;
    break;

  case OP_D1:
  case OP_D2:
  case OP_D3:
    if(QuantityStorage_P->TypeQuantity == VECTOR)
      *Type_Form = VECTOR;
    else
      *Type_Form = VECTOR;
    break;

  default: Message::Error("Unknown operator in 'Get_InitFunctionValue'"); break;
  }
}

/* ------------------------------------------------------------------------ */
/*  C a l _ I n t e r p o l a t i o n O r d e r                             */
/* ------------------------------------------------------------------------ */

double Cal_InterpolationOrder(struct Element *Element,
                              struct QuantityStorage *QuantityStorage)
{
  int i;
  double Order = 0.0;

  for(i = 0; i < QuantityStorage->NbrElementaryBasisFunction; i++)
    if(QuantityStorage->BasisFunction[i].Dof->Type == DOF_UNKNOWN)
      Order =
        std::max(QuantityStorage->BasisFunction[i].BasisFunction->Order, Order);

  return (Order);
}

/* ------------------------------------------------------------------------ */
/*  C a l _ M a x E d g e L e n g t h                                       */
/* ------------------------------------------------------------------------ */

double Cal_MaxEdgeLength(struct Element *Element)
{
  int i, *IM, *N, NbrEdges;
  double l, lmax = 0.0;

  IM = Geo_GetIM_Den(Element->Type, &NbrEdges);
  for(i = 0; i < NbrEdges; i++) {
    N = IM + i * NBR_MAX_SUBENTITIES_IN_ELEMENT;
    l = sqrt(SQU(Element->x[abs(N[1]) - 1] - Element->x[abs(N[0]) - 1]) +
             SQU(Element->y[abs(N[1]) - 1] - Element->y[abs(N[0]) - 1]) +
             SQU(Element->z[abs(N[1]) - 1] - Element->z[abs(N[0]) - 1]));
    lmax = std::max(lmax, l);
  }

  return (lmax);
}
