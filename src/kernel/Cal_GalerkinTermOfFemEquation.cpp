// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//   Johan Gyselinck
//   Ruth Sabariego
//

#include <map>
#include <vector>
#include <math.h>
#include "GetDPConfig.h"
#include "ProData.h"
#include "ProDefine.h"
#include "GeoData.h"
#include "DofData.h"
#include "Cal_Quantity.h"
#include "Cal_Value.h"
#include "Cal_IntegralQuantity.h"
#include "Cal_AnalyticIntegration.h"
#include "Cal_AssembleTerm.h"
#include "Cal_GalerkinTermOfFemEquation.h"
#include "Get_DofOfElement.h"
#include "Get_ElementSource.h"
#include "Get_Geometry.h"
#include "Get_FunctionValue.h"
#include "Pos_Search.h"
#include "BF.h"
#include "Message.h"

extern struct Problem Problem_S;
extern struct CurrentData Current;

std::map<int, bool> assDiag_done;

/* ------------------------------------------------------------------------ */
/*  R e f e r e n c e   v a l u e s                                         */
/* ------------------------------------------------------------------------ */

// The values of the basis functions in the reference element, at the
// integration points, are the same for all the elements of one type that bear
// the same functions with the same orientation of their edges and facets.
// Evaluating each function at each integration point of each element is what
// costs the most with the high order elements, once the elementary matrices
// are computed and assembled as a whole: the values of the Legendre functions
// are thus kept, for each set of functions and each orientation met. (The
// other functions are cheap, and some of them depend on more than the type of
// the element.)

typedef void (*BF_Pointer)(struct Element *Element, int NumEntity, double u,
                            double v, double w, double Value[]);

namespace {

  class ReferenceValues {
  private:
    struct Key {
      int type, numPoints;
      void *quadrature;
      unsigned long long orientation;
      std::vector<std::pair<BF_Pointer, int> > functions;
      bool operator<(const Key &o) const
      {
        if(type != o.type) return type < o.type;
        if(numPoints != o.numPoints) return numPoints < o.numPoints;
        if(quadrature != o.quadrature) return quadrature < o.quadrature;
        if(orientation != o.orientation) return orientation < o.orientation;
        return functions < o.functions;
      }
      bool operator==(const Key &o) const
      {
        return type == o.type && numPoints == o.numPoints &&
               quadrature == o.quadrature && orientation == o.orientation &&
               functions == o.functions;
      }
    };
    std::map<Key, std::vector<double> > _tables;
    std::size_t _size = 0;
    Key _key;
    // the tables last used (the terms of a formulation use a few of them in
    // turn for each element)
    static const int _numLast = 8;
    std::map<Key, std::vector<double> >::iterator _last[_numLast];
    int _numLastUsed = 0, _nextLast = 0;
    // the tables are dropped beyond this number of values
    static const std::size_t _maxSize = 16 * 1024 * 1024;
    bool _full = false;

  public:
    // drops the tables if there was no room left for a new one; to be called
    // when none of them is in use
    void recycle()
    {
      if(!_full) return;
      _tables.clear();
      _size = 0;
      _numLastUsed = _nextLast = 0;
      _full = false;
    }

    // The values of the n functions (3 components each) at the points of the
    // quadrature, point after point, or NULL if they are not all Legendre
    // functions
    const double *get(struct Element *Element, struct Quadrature *Quadrature,
                      int n, BF_Pointer *Functions,
                      struct QuantityStorage *QuantityStorage)
    {
      if(!n || !BF_IsLegendre((void (*)())Functions[0])) return NULL;
      _key.type = Element->Type;
      _key.numPoints = Quadrature->NumberOfPoints;
      _key.quadrature = (void *)Quadrature->Function;
      _key.orientation = BF_GetLegendreOrientation(Element);
      _key.functions.resize(n);
      for(int i = 0; i < n; i++) {
        _key.functions[i].first = Functions[i];
        _key.functions[i].second =
          QuantityStorage->BasisFunction[i].NumEntityInElement;
      }
      for(int k = 0; k < _numLastUsed; k++)
        if(_last[k]->first == _key) return _last[k]->second.data();
      auto it = _tables.find(_key);
      if(it == _tables.end()) {
        for(int i = 1; i < n; i++)
          if(!BF_IsLegendre((void (*)())Functions[i])) return NULL;
        const std::size_t size = (std::size_t)_key.numPoints * n * 3;
        if(_size + size > _maxSize) {
          // (the tables in use cannot be dropped here: see recycle)
          _full = true;
          return NULL;
        }
        _size += size;
        it = _tables.insert(std::make_pair(_key, std::vector<double>(size)))
               .first;
        double *t = it->second.data();
        void (*point)(int, int, double *, double *, double *, double *) =
          (void (*)(int, int, double *, double *, double *,
                    double *))Quadrature->Function;
        for(int p = 0; p < _key.numPoints; p++) {
          double u, v, w, weight, value[MAX_DIM];
          point(_key.numPoints, p, &u, &v, &w, &weight);
          for(int i = 0; i < n; i++) {
            value[0] = value[1] = value[2] = 0.;
            Functions[i](Element, _key.functions[i].second + 1, u, v, w, value);
            for(int c = 0; c < 3; c++) *t++ = value[c];
          }
        }
      }
      _last[_nextLast] = it;
      if(_numLastUsed < _numLast) _numLastUsed++;
      _nextLast = (_nextLast + 1) % _numLast;
      return it->second.data();
    }
  };

  ReferenceValues referenceValues;

  // the integration point of the element whose Galerkin term is being
  // computed, if any
  struct IntegrationPoint {
    struct Element *Element = NULL;
    struct Quadrature *Quadrature = NULL;
    int index = 0;
    double u = 0., v = 0., w = 0.;
  } integrationPoint;

} // namespace

const double *Cal_GetReferenceValuesAtIntegrationPoint(
  struct Element *Element, double u, double v, double w, int n,
  void (**Functions)(), struct QuantityStorage *QuantityStorage)
{
  const IntegrationPoint &p = integrationPoint;
  if(!p.Element || p.Element != Element || p.u != u || p.v != v || p.w != w)
    return NULL;
  const double *values = referenceValues.get(
    Element, p.Quadrature, n, (BF_Pointer *)Functions, QuantityStorage);
  return values ? values + (std::size_t)p.index * n * 3 : NULL;
}

/* ------------------------------------------------------------------------ */
/*  E l e m e n t a r y   m a t r i x                                       */
/* ------------------------------------------------------------------------ */

// The elementary matrix is the sum over the integration points of the products
// of the test functions by the shape functions (with the rest of the term).
// The values at the integration points are stored in two dense tables, with
// one row per integration point and component: one column per test function
// in the first, one column per shape function and harmonic in the second. The
// elementary matrix is then the product of the transpose of the second by the
// first, computed by BLAS when available: this is what makes the cost of the
// high order elements acceptable, the number of products growing as the square
// of the number of functions times the number of integration points.

#if defined(HAVE_BLAS)
#if !defined(F77NAME)
#define F77NAME(x) (x##_)
#endif
extern "C" {
void F77NAME(dgemm)(const char *transa, const char *transb, int *m, int *n,
                    int *k, double *alpha, double *a, int *lda, double *b,
                    int *ldb, double *beta, double *c, int *ldc);
}
#endif

// C += B^T * A, with A (numRows x numA), B (numRows x numB) and C (numB x
// numA) stored by columns, in arrays with ldAB (A and B) and ldC (C) rows
static void Cal_AddTransposeProduct(int numRows, int numA, int numB, double *A,
                                    double *B, int ldAB, double *C, int ldC)
{
  if(!numRows || !numA || !numB) return;
#if defined(HAVE_BLAS)
  // (not worth the call for the small elements)
  if((double)numRows * numA * numB > 2000.) {
    double one = 1.;
    F77NAME(dgemm)("T", "N", &numB, &numA, &numRows, &one, B, &ldAB, A, &ldAB,
                   &one, C, &ldC);
    return;
  }
#endif
  for(int i = 0; i < numA; i++) {
    const double *a = A + (std::size_t)i * ldAB;
    double *c = C + (std::size_t)i * ldC;
    for(int j = 0; j < numB; j++) {
      const double *b = B + (std::size_t)j * ldAB;
      double v = 0.;
      for(int r = 0; r < numRows; r++) v += a[r] * b[r];
      c[j] += v;
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  C a l _ I n i t G a l e r k i n T e r m O f F e m E q u a t i o n       */
/* ------------------------------------------------------------------------ */

void Cal_InitGalerkinTermOfFemEquation(
  struct EquationTerm *EquationTerm_P,
  struct QuantityStorage *QuantityStorage_P0,
  struct QuantityStorage *QuantityStorageNoDof, struct Dof *DofForNoDof_P)
{
  struct FemLocalTermActive *FI;
  extern int MHMoving_assemblyType;

  FI = EquationTerm_P->Case.LocalTerm.Active;

  FI->QuantityStorageEqu_P =
    QuantityStorage_P0 +
    EquationTerm_P->Case.LocalTerm.Term.DefineQuantityIndexEqu;

  Get_InitFunctionValue(EquationTerm_P->Case.LocalTerm.Term.TypeOperatorEqu,
                        FI->QuantityStorageEqu_P, &FI->Type_FormEqu);

  if(EquationTerm_P->Case.LocalTerm.Term.DefineQuantityIndexDof >= 0) {
    FI->QuantityStorageDof_P =
      QuantityStorage_P0 +
      EquationTerm_P->Case.LocalTerm.Term.DefineQuantityIndexDof;
    FI->Type_DefineQuantityDof = FI->QuantityStorageDof_P->DefineQuantity->Type;
  }
  else {
    FI->QuantityStorageDof_P = QuantityStorageNoDof;
    FI->Type_DefineQuantityDof = NODOF;
    FI->DofForNoDof_P = DofForNoDof_P;
    Dof_InitDofForNoDof(DofForNoDof_P, Current.NbrHar);
    QuantityStorage_Reserve(QuantityStorageNoDof, 1);
    QuantityStorageNoDof->BasisFunction[0].Dof = DofForNoDof_P;
  }

  assDiag_done.clear();

  // The test and the shape functions are the same functions. This is only
  // used by the analytic integration (and by the MHBilinear terms): with a
  // numerical quadrature the shape functions and the whole elementary matrix
  // are always computed, which costs next to nothing compared to the rest
  // and avoids having to find out whether the term is symmetric.
  FI->SymmetricalMatrix =
    (EquationTerm_P->Case.LocalTerm.Term.DefineQuantityIndexEqu ==
     EquationTerm_P->Case.LocalTerm.Term.DefineQuantityIndexDof) &&
    (EquationTerm_P->Case.LocalTerm.Term.TypeOperatorEqu ==
     EquationTerm_P->Case.LocalTerm.Term.TypeOperatorDof);

  switch(FI->Type_DefineQuantityDof) {
  case LOCALQUANTITY:
    Get_InitFunctionValue(EquationTerm_P->Case.LocalTerm.Term.TypeOperatorDof,
                          FI->QuantityStorageDof_P, &FI->Type_FormDof);
    break;
  case INTEGRALQUANTITY:
    if(EquationTerm_P->Case.LocalTerm.Term.TypeOperatorDof != NOOP) {
      Message::Error("No operator can act on an Integral Quantity");
    }
    FI->Type_FormDof = VECTOR; /* we don't know the type a priori */
    FI->IntegralQuantityActive.IntegrationCase_L =
      ((struct IntegrationMethod *)List_Pointer(
         Problem_S.IntegrationMethod,
         FI->QuantityStorageDof_P->DefineQuantity->IntegralQuantity
           .IntegrationMethodIndex))
        ->IntegrationCase;
    FI->IntegralQuantityActive.CriterionIndex =
      ((struct IntegrationMethod *)List_Pointer(
         Problem_S.IntegrationMethod,
         FI->QuantityStorageDof_P->DefineQuantity->IntegralQuantity
           .IntegrationMethodIndex))
        ->CriterionIndex;
    FI->IntegralQuantityActive.JacobianCase_L =
      ((struct JacobianMethod *)List_Pointer(
         Problem_S.JacobianMethod, FI->QuantityStorageDof_P->DefineQuantity
                                     ->IntegralQuantity.JacobianMethodIndex))
        ->JacobianCase;
    break;
  case NODOF: FI->Type_FormDof = SCALAR; break;
  }

  FI->Type_ValueDof = Get_ValueFromForm(FI->Type_FormDof);

  /*  G e t   I n t e g r a t i o n   M e t h o d   */
  /*  --------------------------------------------  */

  if(EquationTerm_P->Case.LocalTerm.IntegrationMethodIndex < 0) {
    Message::Error("Integration method missing in equation term");
    FI->IntegrationCase_L = 0;
  }
  else {
    FI->IntegrationCase_L =
      ((struct IntegrationMethod *)List_Pointer(
         Problem_S.IntegrationMethod,
         EquationTerm_P->Case.LocalTerm.IntegrationMethodIndex))
        ->IntegrationCase;

    FI->CriterionIndex =
      ((struct IntegrationMethod *)List_Pointer(
         Problem_S.IntegrationMethod,
         EquationTerm_P->Case.LocalTerm.IntegrationMethodIndex))
        ->CriterionIndex;
  }

  /*  G e t   J a c o b i a n   M e t h o d   */
  /*  --------------------------------------  */

  if(EquationTerm_P->Case.LocalTerm.JacobianMethodIndex < 0) {
    Message::Error("Jacobian method missing in equation term");
    FI->JacobianCase_L = 0;
  }
  else {
    FI->JacobianCase_L = ((struct JacobianMethod *)List_Pointer(
                            Problem_S.JacobianMethod,
                            EquationTerm_P->Case.LocalTerm.JacobianMethodIndex))
                           ->JacobianCase;

    FI->JacobianCase_P0 =
      (FI->NbrJacobianCase = List_Nbr(FI->JacobianCase_L)) ?
        (struct JacobianCase *)List_Pointer(FI->JacobianCase_L, 0) :
        NULL;
  }

  FI->Flag_ChangeCoord =
    (!(((FI->Type_FormEqu == FORM0 || FI->Type_FormEqu == FORM0P) &&
        (FI->Type_FormDof == FORM3 || FI->Type_FormDof == FORM3P)) ||
       /*
         ( (FI->Type_FormEqu == FORM1 || FI->Type_FormEqu == FORM1P)  &&
         (FI->Type_FormDof == FORM2 || FI->Type_FormDof == FORM2P) ) ||
         ( (FI->Type_FormEqu == FORM2 || FI->Type_FormEqu == FORM2P)  &&
         (FI->Type_FormDof == FORM1 || FI->Type_FormDof == FORM1P) ) ||
       */
       ((FI->Type_FormEqu == FORM3 || FI->Type_FormEqu == FORM3P) &&
        (FI->Type_FormDof == FORM0 ||
         FI->Type_FormDof ==
           FORM0P)))) || /* For operators on VECTOR's (To be extended) */
    (FI->Type_FormEqu == VECTOR || FI->Type_FormDof == VECTOR) ||
    (FI->Type_DefineQuantityDof == INTEGRALQUANTITY);

  if(FI->Flag_ChangeCoord) {
    FI->Flag_InvJac =
      ((FI->Type_FormEqu == FORM1) ||
       (FI->Type_FormDof == FORM1) ||
       /* For operators on VECTOR's (To be extended) */
       (FI->Type_FormEqu == VECTOR || FI->Type_FormDof == VECTOR) ||
       (EquationTerm_P->Case.LocalTerm.Term.QuantityIndexPost == 1));
  }

  /*  G e t   C h a n g e O f C o o r d i n a t e s   */
  /*  ----------------------------------------------  */

  FI->xChangeOfCoordinatesEqu =
    (void (*)(struct Element *, double *, double *))Get_ChangeOfCoordinates(
      FI->Flag_ChangeCoord, FI->Type_FormEqu);

  FI->xChangeOfCoordinatesDof =
    (void (*)(struct Element *, double *, double *))Get_ChangeOfCoordinates(
      FI->Flag_ChangeCoord, FI->Type_FormDof);

  /*  G e t   C a l _ P r o d u c t x  */
  /*  -------------------------------- */

  switch(FI->Type_FormEqu) {
  case FORM1:
  case FORM1S:
  case FORM2:
  case FORM2P:
  case FORM2S:
  case VECTOR: FI->Cal_Productx = (double (*)())Cal_Product123; break;
  case FORM1P:
  case VECTORP: FI->Cal_Productx = (double (*)())Cal_Product3; break;
  case FORM0:
  case FORM3:
  case FORM3P:
  case SCALAR: FI->Cal_Productx = (double (*)())Cal_Product1; break;
  default:
    Message::Error("Unknown type of Form (%d)", FI->Type_FormEqu);
    FI->Cal_Productx = (double (*)())Cal_Product123;
    break;
  }

  /*  G e t   F u n c t i o n _ A s s e m b l e T e r m  */
  /*  -------------------------------------------------  */

  switch(EquationTerm_P->Case.LocalTerm.Term.TypeTimeDerivative) {
  case NODT_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NoDt;
    break;
  case DT_: FI->Function_AssembleTerm = Cal_AssembleTerm_Dt; break;
  case DTDOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtDof;
    break;
  case DTDT_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtDt;
    break;
  case DTDTDOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtDtDof;
    break;
  case DTDTDTDOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtDtDtDof;
    break;
  case DTDTDTDTDOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtDtDtDtDof;
    break;
  case DTDTDTDTDTDOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtDtDtDtDtDof;
    break;
  case JACNL_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_JacNL;
    break;
  case DTDOFJACNL_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtDofJacNL;
    break;
  case NEVERDT_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NeverDt;
    break;
  case DTNL_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_DtNL;
    break;
  // nleigchange
  case NLEIG1DOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NLEig1Dof;
    break;
  case NLEIG2DOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NLEig2Dof;
    break;
  case NLEIG3DOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NLEig3Dof;
    break;
  case NLEIG4DOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NLEig4Dof;
    break;
  case NLEIG5DOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NLEig5Dof;
    break;
  case NLEIG6DOF_:
    FI->Function_AssembleTerm = Cal_AssembleTerm_NLEig6Dof;
    break;

  default:
    Message::Error("Unknown type of Operator for Galerkin term (%d)",
                   EquationTerm_P->Case.LocalTerm.Term.TypeTimeDerivative);
    FI->Function_AssembleTerm = Cal_AssembleTerm_NoDt;
    break;
  }

  if(MHMoving_assemblyType)
    FI->Function_AssembleTerm = Cal_AssembleTerm_MHMoving;

  // TODO: if JACNL_, say to Cal_Init to assemble later in Jac, otherwise
  // assemble in the system matrix

  // initialisation of MHBilinear-term (nonlinear multi-harmonics)
  Cal_InitGalerkinTermOfFemEquation_MHBilinear(EquationTerm_P);

  if(EquationTerm_P->Case.LocalTerm.Full_Matrix) {
    FI->Full_Matrix = 1;
    FI->FirstElements = List_Create(20, 10, sizeof(struct FirstElement));
  }
}

/* ------------------------------------------------------------------------ */
/*  C a l _ E n d G a l e r k i n T e r m O f F e m E q u a t i o n         */
/* ------------------------------------------------------------------------ */

void Cal_EndGalerkinTermOfFemEquation() { assDiag_done.clear(); }

/* ------------------------------------------------------------------------ */
/*  C a l _ a p p l y M e t r i c T e n s o r                               */
/* ------------------------------------------------------------------------ */

void Cal_applyMetricTensor(struct EquationTerm *EquationTerm_P,
                           struct FemLocalTermActive *FI,
                           struct QuantityStorage *QuantityStorage_P0,
                           int Nbr_Dof, struct Value vBFxDof[])
{
  int j;
  int mi;
  struct Value S;
  struct Value detS;

  mi = EquationTerm_P->Case.LocalTerm.ExpressionIndexForMetricTensor;
  if(mi == -1) return;
  Get_ValueOfExpression(
    (struct Expression *)List_Pointer(Problem_S.Expression, mi),
    QuantityStorage_P0, Current.u, Current.v, Current.w, &S);

  if(S.Type == SCALAR) {
    S.Type = TENSOR_DIAG;
    S.Val[1] = S.Val[0];
    S.Val[2] = S.Val[0];
  }
  if(S.Type != TENSOR_SYM && S.Type != TENSOR && S.Type != TENSOR_DIAG) {
    Message::Error("Cannot interpret field type %s as metric tensor",
                   Get_StringForDefine(Field_Type, S.Type));
    return;
  }

  Cal_DetValue(&S, &detS);
  detS.Val[0] = sqrt(fabs(detS.Val[0]));

  switch(FI->Type_FormDof) {
  case FORM1:
  case FORM1S:
  case FORM1P:
    Cal_InvertValue(&S, &S);
    for(j = 0; j < Nbr_Dof; j++) {
      Cal_ProductValue(&S, &vBFxDof[j], &vBFxDof[j]);
      Cal_ProductValue(&detS, &vBFxDof[j], &vBFxDof[j]);
    }
    break;
  case FORM2:
  case FORM2S:
  case FORM2P:
    Cal_InvertValue(&detS, &detS);
    for(j = 0; j < Nbr_Dof; j++) {
      Cal_ProductValue(&S, &vBFxDof[j], &vBFxDof[j]);
      Cal_ProductValue(&detS, &vBFxDof[j], &vBFxDof[j]);
    }
    break;
  case FORM3:
  case FORM3S:
  case FORM3P:
    Cal_InvertValue(&detS, &detS);
    for(j = 0; j < Nbr_Dof; j++) {
      Cal_ProductValue(&detS, &vBFxDof[j], &vBFxDof[j]);
    }
    break;
  case FORM0:
  case FORM0S:
  case FORM0P:
    for(j = 0; j < Nbr_Dof; j++) {
      Cal_ProductValue(&detS, &vBFxDof[j], &vBFxDof[j]);
    }
    break;
  default:
    Message::Error("Cannot use metric tensor with field type %s",
                   Get_StringForDefine(Field_Type, FI->Type_FormDof));
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  C a l _ v B F x D o f                                                   */
/* ------------------------------------------------------------------------ */

int Cal_vBFxDof(struct EquationTerm *EquationTerm_P,
                struct FemLocalTermActive *FI,
                struct QuantityStorage *QuantityStorage_P0,
                struct QuantityStorage *QuantityStorageDof_P, int Nbr_Dof,
                void (*xFunctionBFDof[])(struct Element *Element,
                                         int NumEntity, double u, double v,
                                         double w, double Value[]),
                struct Value vBFxDof[], const double *RefDof,
                double (*SameAsEqu)[MAX_DIM])
{
  StackArray<double[MAX_DIM], NBR_BASISFUNCTIONS_ON_STACK> vBFuDof;
  double u, v, w;
  struct Value CoefPhys;
  struct Element *E;

  vBFuDof.resize(Nbr_Dof);

  // initialize vBFxDof to zero; this allows to perform e.g. [0, {d u}] without
  // having to explicitly use [Vector[0,0,0], {d u}] ; if this is too slow, we
  // should check vBFxDof[j].Type against FI->Type_FormEqu before calling
  // FI->Cal_Productx to report errors
  for(int j = 0; j < Nbr_Dof; j++) Cal_ZeroValue(&vBFxDof[j]);

  if(EquationTerm_P->Case.LocalTerm.Term.DofInTrace) {
    E = Current.Element->ElementTrace;
    if(!PointInElement(E, nullptr, Current.x, Current.y, Current.z, &Current.ut,
                       &Current.vt, &Current.wt)) {
      return 0; // we're done, no contribution from this Trace element
    }
    u = Current.ut;
    v = Current.vt;
    w = Current.wt;
  }
  else {
    E = Current.Element;
    u = Current.u;
    v = Current.v;
    w = Current.w;
  }

  // shape functions, integral quantity or dummy

  switch(FI->Type_DefineQuantityDof) {
  case LOCALQUANTITY:
    // for the functions of the Dof that need the entity of each basis
    // function (NumEntity)
    Current.QuantityStorageDof = QuantityStorageDof_P;
    if(SameAsEqu) {
      // the shape functions are the test functions, already computed
      const int n = (FI->Type_ValueDof == SCALAR) ? 1 : 3;
      for(int j = 0; j < Nbr_Dof; j++) {
        for(int c = 0; c < n; c++) vBFxDof[j].Val[c] = SameAsEqu[j][c];
        vBFxDof[j].Type = FI->Type_ValueDof;
        if(Current.NbrHar > 1) Cal_SetHarmonicValue(&vBFxDof[j]);
      }
      break;
    }
    for(int j = 0; j < Nbr_Dof; j++) {
      if(RefDof) {
        // the values in the reference element are known
        const double *r = RefDof + (std::size_t)j * 3;
        vBFuDof[j][0] = r[0];
        vBFuDof[j][1] = r[1];
        vBFuDof[j][2] = r[2];
      }
      else
        xFunctionBFDof[j](
          E, QuantityStorageDof_P->BasisFunction[j].NumEntityInElement + 1, u,
          v, w, vBFuDof[j]);
      FI->xChangeOfCoordinatesDof(E, vBFuDof[j], vBFxDof[j].Val);
      vBFxDof[j].Type = FI->Type_ValueDof;
      if(Current.NbrHar > 1) Cal_SetHarmonicValue(&vBFxDof[j]);
    }
    break;
  case INTEGRALQUANTITY:
    if(FI->IntegralQuantityActive.IntegrationCase_P->Type == ANALYTIC)
      Cal_AnalyticIntegralQuantity(Current.Element, QuantityStorageDof_P,
                                   Nbr_Dof, (void (**)())xFunctionBFDof,
                                   vBFxDof);

    else
      Cal_NumericalIntegralQuantity(
        Current.Element, &FI->IntegralQuantityActive, QuantityStorage_P0,
        QuantityStorageDof_P, FI->Type_DefineQuantityDof, Nbr_Dof,
        (void (**)())xFunctionBFDof, vBFxDof);
    FI->Type_ValueDof = FI->Type_FormDof =
      vBFxDof[0].Type; /* now this type is correct */
    break;
  case NODOF: break;
  }

  /* Compute remaining factors in the term */

  if(EquationTerm_P->Case.LocalTerm.Term.CanonicalWholeQuantity ==
     CWQ_EXP_TIME_DOF) {
    Get_ValueOfExpression(
      (struct Expression *)List_Pointer(
        Problem_S.Expression,
        EquationTerm_P->Case.LocalTerm.Term.ExpressionIndexForCanonical),
      QuantityStorage_P0, Current.u, Current.v, Current.w, &CoefPhys);
    for(int j = 0; j < Nbr_Dof; j++)
      Cal_ProductValue(&CoefPhys, &vBFxDof[j], &vBFxDof[j]);
  }
  else if(EquationTerm_P->Case.LocalTerm.Term.CanonicalWholeQuantity !=
          CWQ_DOF) {
    Cal_WholeQuantity(
      Current.Element, QuantityStorage_P0,
      EquationTerm_P->Case.LocalTerm.Term.WholeQuantity, Current.u, Current.v,
      Current.w, EquationTerm_P->Case.LocalTerm.Term.DofIndexInWholeQuantity,
      Nbr_Dof, vBFxDof);
  }

  /* Compute using metric tensor if defined */
  Cal_applyMetricTensor(EquationTerm_P, FI, QuantityStorage_P0, Nbr_Dof,
                        vBFxDof);

  return 1;
}

/* ------------------------------------------------------------------------ */
/*  C a l _ G a l e r k i n T e r m O f F e m E q u a t i o n               */
/* ------------------------------------------------------------------------ */

typedef double (*Jacobian_Pointer)(struct Element *, MATRIX3x3 *);
typedef void (*ChangeOfCoord_Pointer)(struct Element *, double *, double *);

namespace {

  // Whether the Jacobian matrix is the same at all the points of the element:
  // the straight simplices, without transformation of the coordinates
  bool isJacobianConstant(int ElementType, Jacobian_Pointer Jacobian)
  {
    if(ElementType != LINE && ElementType != TRIANGLE &&
       ElementType != TETRAHEDRON)
      return false;
    return Jacobian == JacobianVol1D || Jacobian == JacobianVol2D ||
           Jacobian == JacobianVol3D || Jacobian == JacobianSur2D ||
           Jacobian == JacobianSur3D || Jacobian == JacobianLin3D;
  }

  // The change of coordinates of n functions, from their values u in the
  // reference element to their values x in the element (the usual ones
  // without a call per function)
  void changeOfCoordinates(ChangeOfCoord_Pointer Function,
                           struct Element *Element, int n,
                           double (*u)[MAX_DIM], double (*x)[MAX_DIM])
  {
    if(Function == ChangeOfCoord_No1) {
      for(int i = 0; i < n; i++) x[i][0] = u[i][0];
    }
    else if(Function == ChangeOfCoord_No123) {
      for(int i = 0; i < n; i++) {
        x[i][0] = u[i][0];
        x[i][1] = u[i][1];
        x[i][2] = u[i][2];
      }
    }
    else if(Function == ChangeOfCoord_Form1) {
      const MATRIX3x3 J = Element->InvJac;
      for(int i = 0; i < n; i++) {
        const double a = u[i][0], b = u[i][1], c = u[i][2];
        x[i][0] = a * J.c11 + b * J.c12 + c * J.c13;
        x[i][1] = a * J.c21 + b * J.c22 + c * J.c23;
        x[i][2] = a * J.c31 + b * J.c32 + c * J.c33;
      }
    }
    else if(Function == ChangeOfCoord_Form2 && Element->DetJac) {
      const MATRIX3x3 J = Element->Jac;
      const double d = Element->DetJac;
      for(int i = 0; i < n; i++) {
        const double a = u[i][0], b = u[i][1], c = u[i][2];
        x[i][0] = (a * J.c11 + b * J.c21 + c * J.c31) / d;
        x[i][1] = (a * J.c12 + b * J.c22 + c * J.c32) / d;
        x[i][2] = (a * J.c13 + b * J.c23 + c * J.c33) / d;
      }
    }
    else if(Function == ChangeOfCoord_Form3 && Element->DetJac) {
      const double d = Element->DetJac;
      for(int i = 0; i < n; i++) x[i][0] = u[i][0] / d;
    }
    else {
      for(int i = 0; i < n; i++) Function(Element, u[i], x[i]);
    }
  }

  // A Galerkin term on an element: its elementary matrix, computed by a
  // numerical quadrature or analytically, and its assembly
  class GalerkinTerm {
  private:
    struct Element *Element;
    struct EquationTerm *EquationTerm_P;
    struct QuantityStorage *QuantityStorage_P0;
    struct FemLocalTermActive *FI;
    struct QuantityStorage *QuantityStorageEqu_P, *QuantityStorageDof_P;
    struct IntegrationCase *IntegrationCase_P = NULL;
    Jacobian_Pointer Get_Jacobian = NULL;
    int Type_Dimension = 0;

    int Nbr_Equ = 0, Nbr_Dof = 0;
    const int NbrHar;
    // the functions, their values in the reference element (u) and in the
    // element (x); the values of the shape functions with the rest of the
    // term are general values when they are not computed directly
    StackArray<BF_Pointer, NBR_BASISFUNCTIONS_ON_STACK> xFunctionBFEqu,
      xFunctionBFDof;
    StackArray<double[MAX_DIM], NBR_BASISFUNCTIONS_ON_STACK> vBFuEqu, vBFxEqu,
      vBFuDof, vBFxDofDirect;
    StackArray<struct Value, NBR_BASISFUNCTIONS_ON_STACK> vBFxDof;
    // the elementary matrix: the row of each test function, with Nbr_Ek
    // columns of NbrHar values
    StackArray<double, 2 * NBR_BASISFUNCTIONS_ON_STACK *
                         NBR_BASISFUNCTIONS_ON_STACK> Ek;
    int Nbr_Ek = 0;
    double *EK(int i, int j)
    {
      return &Ek[((std::size_t)i * Nbr_Ek + j) * NbrHar];
    }

    const char *integrationName()
    {
      return ((struct IntegrationMethod *)List_Pointer(
                Problem_S.IntegrationMethod,
                EquationTerm_P->Case.LocalTerm.IntegrationMethodIndex))
        ->Name;
    }

    // get room for Nbr_Dof shape functions
    void resizeDof()
    {
      xFunctionBFDof.resize(Nbr_Dof);
      vBFxDof.resize(Nbr_Dof);
      Nbr_Ek = (Nbr_Dof > Nbr_Equ) ? Nbr_Dof : Nbr_Equ;
      Ek.resize((std::size_t)Nbr_Ek * Nbr_Ek * NbrHar);
    }

    // the Jacobian of the region of the element
    bool setJacobian()
    {
      int i = 0, j;
      while((i < FI->NbrJacobianCase) &&
            ((j = (FI->JacobianCase_P0 + i)->RegionIndex) >= 0) &&
            !List_Search(
              ((struct Group *)List_Pointer(Problem_S.Group, j))->InitialList,
              &Element->Region, fcmp_int))
        i++;
      if(i == FI->NbrJacobianCase) {
        Message::Error("Undefined Jacobian in Region %d", Element->Region);
        return false;
      }
      Element->JacobianCase = FI->JacobianCase_P0 + i;
      Get_Jacobian = (Jacobian_Pointer)Get_JacobianFunction(
        Element->JacobianCase->TypeJacobian, Element->Type, &Type_Dimension);

      // always get node coordinates so that we can compute current.{x, y, z}
      Get_NodesCoordinatesOfElement(Element);

      if(Element->JacobianCase->CoefficientIndex < 0) { FI->CoefJac = 1.; }
      else {
        struct Value Coef;
        Get_ValueOfExpressionByIndex(Element->JacobianCase->CoefficientIndex,
                                     NULL, 0., 0., 0., &Coef);
        FI->CoefJac = Coef.Val[0];
      }
      return true;
    }

    // The test functions at the current point, in the element, from their
    // values in the reference element (vBFuEqu), with the expression that
    // modifies them if any
    void testFunctions()
    {
      auto &Term = EquationTerm_P->Case.LocalTerm.Term;
      changeOfCoordinates(FI->xChangeOfCoordinatesEqu,
                          Element, Nbr_Equ, vBFuEqu.data(), vBFxEqu.data());
      if(Term.CanonicalWholeQuantity_Equ == CWQ_NONE) return;

      struct Value Expression, V1, V2;
      if(Term.CanonicalWholeQuantity_Equ == CWQ_EXP_TIME_DOF)
        Get_ValueOfExpressionByIndex(Term.ExpressionIndexForCanonical_Equ,
                                     QuantityStorage_P0, Current.u, Current.v,
                                     Current.w, &Expression);
      for(int i = 0; i < Nbr_Equ; i++) {
        V1.Type = Get_ValueFromForm(FI->Type_FormEqu);
        V1.Val[0] = vBFxEqu[i][0];
        V1.Val[1] = vBFxEqu[i][1];
        V1.Val[2] = vBFxEqu[i][2];
        V1.Val[MAX_DIM] = 0;
        V1.Val[MAX_DIM + 1] = 0;
        V1.Val[MAX_DIM + 2] = 0;
        if(Term.CanonicalWholeQuantity_Equ == CWQ_EXP_TIME_DOF) {
          switch(Term.OperatorTypeForCanonical_Equ) {
          case OP_TIME: Cal_ProductValue(&Expression, &V1, &V2); break;
          case OP_CROSSPRODUCT:
            Cal_CrossProductValue(&Expression, &V1, &V2);
            break;
          default: Message::Error("Unknown operation in Equation"); break;
          }
        }
        else if(Term.CanonicalWholeQuantity_Equ == CWQ_FCT_DOF) {
          ((void (*)(struct Function *, struct Value *,
                     struct Value *))Term.BuiltInFunction_Equ)(NULL, &V1, &V2);
        }
        vBFxEqu[i][0] = V2.Val[0];
        vBFxEqu[i][1] = V2.Val[1];
        vBFxEqu[i][2] = V2.Val[2];
      }
    }

    // The shape functions at the current point with the rest of the term,
    // for the terms 'Dof{}' and 'expression[] * Dof{}': the NbrComp
    // components from FirstComp of the function j and the harmonic k are
    // stored at Table[(j * NbrHar + k) * Stride + c]. The values x of the
    // shape functions in the element are plain numbers and the expression,
    // which is the same for all the functions, is applied to them at once: a
    // general value per function, with the type of each operand looked up for
    // each product, costs much more than the products themselves. Returns
    // false if the term has to be computed with general values after all.
    bool shapeFunctionsDirect(double (*x)[MAX_DIM], double *Table,
                              std::size_t Stride, int FirstComp, int NbrComp)
    {
      auto &Term = EquationTerm_P->Case.LocalTerm.Term;
      if(Term.CanonicalWholeQuantity == CWQ_DOF) {
        // as Cal_SetHarmonicValue: the real parts
        for(int j = 0; j < Nbr_Dof; j++)
          for(int k = 0; k < NbrHar; k++) {
            double *t = Table + ((std::size_t)j * NbrHar + k) * Stride;
            for(int c = 0; c < NbrComp; c++)
              t[c] = (k % 2) ? 0. : x[j][FirstComp + c];
          }
        return true;
      }

      // the expression times each unit value: the term is linear in the Dof
      struct Value Coef, Unit[3];
      Get_ValueOfExpression(
        (struct Expression *)List_Pointer(Problem_S.Expression,
                                          Term.ExpressionIndexForCanonical),
        QuantityStorage_P0, Current.u, Current.v, Current.w, &Coef);
      const int n = (FI->Type_ValueDof == SCALAR) ? 1 : 3;
      for(int d = 0; d < n; d++) {
        Cal_ZeroValue(&Unit[d]);
        Unit[d].Type = FI->Type_ValueDof;
        Unit[d].Val[d] = 1.;
        if(NbrHar > 1) Cal_SetHarmonicValue(&Unit[d]);
        Cal_ProductValue(&Coef, &Unit[d], &Unit[d]);
        // (e.g. a scalar product: not the type of the shape functions)
        if(Unit[d].Type != FI->Type_ValueDof) return false;
      }
      for(int j = 0; j < Nbr_Dof; j++)
        for(int k = 0; k < NbrHar; k++) {
          double *t = Table + ((std::size_t)j * NbrHar + k) * Stride;
          for(int c = 0; c < NbrComp; c++) {
            double v = 0.;
            for(int d = 0; d < n; d++)
              v += x[j][d] * Unit[d].Val[MAX_DIM * k + FirstComp + c];
            t[c] = v;
          }
        }
      return true;
    }

    /*  -------------------------------------  */
    /*  Q U A D R A T U R E                    */
    /*  -------------------------------------  */

    void quadrature()
    {
      auto &Term = EquationTerm_P->Case.LocalTerm.Term;
      // For Integration Auto: the degree of the test functions times the
      // shape functions. If the term has no shape functions of a local
      // quantity, what multiplies the test functions is
      //  - the quantities of the term, if its expression has some (e.g. the
      //    residual of a nonlinear problem): their degree;
      //  - given data otherwise (a source or a boundary term), which is not
      //    a polynomial in general: it is integrated as if it had one degree
      //    more than the test functions of H1, and two more than the others.
      //    With less the error on the data shows in the solution, e.g. for
      //    the gradient part of a field of H(curl), which is only controlled
      //    by such terms.
      int Degree = -1;
      if(IntegrationCase_P->Auto) {
        const int DegreeEqu = Get_IntegrationDegree(
          Element, QuantityStorageEqu_P, Term.TypeOperatorEqu);
        int DegreeDof;
        if(FI->Type_DefineQuantityDof == LOCALQUANTITY && !Term.DofInTrace)
          DegreeDof = Get_IntegrationDegree(Element, QuantityStorageDof_P,
                                            Term.TypeOperatorDof);
        else {
          bool quantities = (Term.NbrQuantityIndex > 1);
          for(int i = 0; !quantities && i < List_Nbr(Term.WholeQuantity); i++)
            quantities = (((struct WholeQuantity *)List_Pointer(
                             Term.WholeQuantity, i))
                            ->Type == WQ_OPERATORANDQUANTITY);
          if(quantities) {
            DegreeDof = 0;
            for(int i = 0; i < Term.NbrQuantityIndex; i++) {
              struct QuantityStorage *Q =
                QuantityStorage_P0 + Term.QuantityIndexTable[i];
              if(!Q->DefineQuantity || Q->DefineQuantity->Type != LOCALQUANTITY)
                continue;
              int d = Get_IntegrationDegree(Element, Q, NOOP);
              if(d > DegreeDof) DegreeDof = d;
            }
          }
          else
            DegreeDof =
              DegreeEqu +
              ((FI->Type_FormEqu == FORM0 || FI->Type_FormEqu == FORM0P) ? 1 : 2);
        }
        Degree = DegreeEqu + DegreeDof + Get_GeometricIntegrationDegree(Element);
      }
      struct Quadrature *Quadrature_P =
        Get_QuadratureOfElement(IntegrationCase_P, Element, Degree);
      if(!Quadrature_P) {
        Message::Error(
          "Unknown type of Element (%s) for Integration method (%s)",
          Get_StringForDefine(Element_Type, Element->Type), integrationName());
        return;
      }
      const int Nbr_IntPoints = Quadrature_P->NumberOfPoints;
      void (*Get_IntPoint)(int, int, double *, double *, double *, double *) =
        (void (*)(int, int, double *, double *, double *,
                  double *))Quadrature_P->Function;

      // The values of the test functions (times the weights) and of the shape
      // functions at the integration points are stored in two tables, with
      // one row per point and component (the three components of the
      // functions, the third one or the first one), until their products are
      // added to the elementary matrix (see Cal_AddTransposeProduct)
      const int FirstComp =
        (FI->Cal_Productx == (double (*)())Cal_Product3) ? 2 : 0;
      const int NbrComp =
        (FI->Cal_Productx == (double (*)())Cal_Product123) ? 3 : 1;
      // (the tables are kept from one element to the next, unless this is a
      // term computed while another one is: none in practice)
      static std::vector<double> StaticEqu, StaticDof;
      static bool busy = false;
      std::vector<double> LocalEqu, LocalDof;
      const bool nested = busy;
      if(!nested) referenceValues.recycle();
      const IntegrationPoint outerPoint = integrationPoint;
      std::vector<double> &TableEqu = nested ? LocalEqu : StaticEqu;
      std::vector<double> &TableDof = nested ? LocalDof : StaticDof;
      busy = true;
      int NbrRowsMax = 65536 / (Nbr_Ek * NbrHar);
      if(NbrRowsMax < 4 * NbrComp) NbrRowsMax = 4 * NbrComp;
      if(NbrRowsMax > Nbr_IntPoints * NbrComp)
        NbrRowsMax = Nbr_IntPoints * NbrComp;
      int NbrRows = 0;
      if(TableEqu.size() < (std::size_t)NbrRowsMax * Nbr_Equ)
        TableEqu.resize((std::size_t)NbrRowsMax * Nbr_Equ);
      if(TableDof.size() < (std::size_t)NbrRowsMax * Nbr_Dof * NbrHar)
        TableDof.resize((std::size_t)NbrRowsMax * Nbr_Dof * NbrHar);
      auto flush = [&]() {
        Cal_AddTransposeProduct(NbrRows, Nbr_Equ, Nbr_Dof * NbrHar,
                                TableEqu.data(), TableDof.data(), NbrRowsMax,
                                Ek.data(), Nbr_Ek * NbrHar);
        NbrRows = 0;
      };

      // the values of the test functions in the reference element, if they
      // are known for all the integration points
      const double *RefEqu = referenceValues.get(
        Element, Quadrature_P, Nbr_Equ, xFunctionBFEqu.data(),
        QuantityStorageEqu_P);

      const bool Local = (FI->Type_DefineQuantityDof == LOCALQUANTITY) &&
                         !Term.DofInTrace;
      // The shape functions are the test functions (the same quantity with
      // the same operator, in the same element, the test functions being used
      // as they are): their values are reused, whatever the rest of the term
      const bool SameFunctions = Local && FI->SymmetricalMatrix &&
                                 Term.CanonicalWholeQuantity_Equ == CWQ_NONE;
      // (otherwise, the values of the shape functions of the same element)
      const double *RefDof =
        (Local && !SameFunctions) ?
          referenceValues.get(Element, Quadrature_P, Nbr_Dof,
                              xFunctionBFDof.data(), QuantityStorageDof_P) :
          NULL;
      // the terms whose shape functions are computed directly (see
      // shapeFunctionsDirect)
      const bool Direct =
        Local &&
        (Term.CanonicalWholeQuantity == CWQ_DOF ||
         Term.CanonicalWholeQuantity == CWQ_EXP_TIME_DOF) &&
        EquationTerm_P->Case.LocalTerm.ExpressionIndexForMetricTensor == -1;
      if(Direct && !SameFunctions) {
        vBFuDof.resize(Nbr_Dof);
        vBFxDofDirect.resize(Nbr_Dof);
      }

      // on the straight simplices the Jacobian matrix is computed once
      const bool ConstantJacobian =
        isJacobianConstant(Element->Type, Get_Jacobian);

      for(int i_IntPoint = 0; i_IntPoint < Nbr_IntPoints; i_IntPoint++) {
        double weight;
        Current.QuadraturePointIndex = i_IntPoint;
        Get_IntPoint(Nbr_IntPoints, i_IntPoint, &Current.u, &Current.v,
                     &Current.w, &weight);
        // (for the quantities interpolated at this point by the expressions
        // of the term)
        integrationPoint.Element = Element;
        integrationPoint.Quadrature = Quadrature_P;
        integrationPoint.index = i_IntPoint;
        integrationPoint.u = Current.u;
        integrationPoint.v = Current.v;
        integrationPoint.w = Current.w;

        /* Geometry */

        if(ConstantJacobian && i_IntPoint) {
          // (the derivatives of the geometrical functions are constant too)
          for(int i = 0; i < Element->GeoElement->NbrNodes; i++)
            BF_Node(Element, i + 1, Current.u, Current.v, Current.w,
                    &(Element->n[i]));
        }
        else
          Get_BFGeoElement(Element, Current.u, Current.v, Current.w);

        Current.x = Current.y = Current.z = 0.;
        for(int i = 0; i < Element->GeoElement->NbrNodes; i++) {
          Current.x += Element->x[i] * Element->n[i];
          Current.y += Element->y[i] * Element->n[i];
          Current.z += Element->z[i] * Element->n[i];
        }

        if(FI->Flag_ChangeCoord && !(ConstantJacobian && i_IntPoint)) {
          Element->DetJac = Get_Jacobian(Element, &Element->Jac);
          if(FI->Flag_InvJac)
            Get_InverseMatrix(Type_Dimension, Element->Type, Element->DetJac,
                              &Element->Jac, &Element->InvJac);
        }

        /* Test functions */

        if(RefEqu) {
          const double *r = RefEqu + (std::size_t)i_IntPoint * Nbr_Equ * 3;
          for(int i = 0; i < Nbr_Equ; i++, r += 3) {
            vBFuEqu[i][0] = r[0];
            vBFuEqu[i][1] = r[1];
            vBFuEqu[i][2] = r[2];
          }
        }
        else
          for(int i = 0; i < Nbr_Equ; i++)
            xFunctionBFEqu[i](
              Element,
              QuantityStorageEqu_P->BasisFunction[i].NumEntityInElement + 1,
              Current.u, Current.v, Current.w, vBFuEqu[i]);
        testFunctions();

        /* Shape functions, with the rest of the term */

        Current.Element = Element;
        double *RowsDof = &TableDof[NbrRows];
        const double *RefDofPoint =
          RefDof ? RefDof + (std::size_t)i_IntPoint * Nbr_Dof * 3 : NULL;
        bool done = false;
        if(Direct) {
          double (*x)[MAX_DIM] = vBFxEqu.data();
          if(!SameFunctions) {
            // for the functions of the Dof that need the entity of each basis
            // function (NumEntity)
            Current.QuantityStorageDof = QuantityStorageDof_P;
            if(RefDofPoint) {
              const double *r = RefDofPoint;
              for(int j = 0; j < Nbr_Dof; j++, r += 3) {
                vBFuDof[j][0] = r[0];
                vBFuDof[j][1] = r[1];
                vBFuDof[j][2] = r[2];
              }
            }
            else
              for(int j = 0; j < Nbr_Dof; j++)
                xFunctionBFDof[j](
                  Element,
                  QuantityStorageDof_P->BasisFunction[j].NumEntityInElement + 1,
                  Current.u, Current.v, Current.w, vBFuDof[j]);
            changeOfCoordinates(
              FI->xChangeOfCoordinatesDof, Element,
              Nbr_Dof, vBFuDof.data(), vBFxDofDirect.data());
            x = vBFxDofDirect.data();
          }
          else
            Current.QuantityStorageDof = QuantityStorageDof_P;
          done = shapeFunctionsDirect(x, RowsDof, NbrRowsMax, FirstComp,
                                      NbrComp);
        }
        if(!done) {
          // general values
          if(!Cal_vBFxDof(EquationTerm_P, FI, QuantityStorage_P0,
                          QuantityStorageDof_P, Nbr_Dof, xFunctionBFDof.data(),
                          vBFxDof.data(), RefDofPoint,
                          SameFunctions ? vBFxEqu.data() : NULL))
            continue; // no contribution from this point
          for(int j = 0; j < Nbr_Dof; j++)
            for(int k = 0; k < NbrHar; k++) {
              double *t = RowsDof + ((std::size_t)j * NbrHar + k) * NbrRowsMax;
              for(int c = 0; c < NbrComp; c++)
                t[c] = vBFxDof[j].Val[MAX_DIM * k + FirstComp + c];
            }
        }

        /* The test functions, times the weight */

        const double Factor =
          FI->CoefJac *
          ((FI->Flag_ChangeCoord) ? weight * fabs(Element->DetJac) : weight);
        for(int i = 0; i < Nbr_Equ; i++) {
          double *t = &TableEqu[(std::size_t)i * NbrRowsMax + NbrRows];
          for(int c = 0; c < NbrComp; c++)
            t[c] = Factor * vBFxEqu[i][FirstComp + c];
        }

        NbrRows += NbrComp;
        if(NbrRows + NbrComp > NbrRowsMax) flush();
      }
      flush();
      integrationPoint = outerPoint;
      if(!nested) busy = false;
    }

    /*  -------------------------------------  */
    /*  A N A L Y T I C                        */
    /*  -------------------------------------  */

    void analytic()
    {
      auto &Term = EquationTerm_P->Case.LocalTerm.Term;
      double Factor = 1.;
      if(Term.CanonicalWholeQuantity == CWQ_EXP_TIME_DOF) {
        if(Term.ExpressionIndexForCanonical >= 0) {
          struct Value Coef;
          Get_ValueOfExpression(
            (struct Expression *)List_Pointer(
              Problem_S.Expression, Term.ExpressionIndexForCanonical),
            QuantityStorage_P0, 0., 0., 0., &Coef);
          Factor = Coef.Val[0];
        }
      }
      else if(Term.CanonicalWholeQuantity != CWQ_DOF) {
        Message::Error("Bad expression for full analytic integration");
      }

      if(FI->SymmetricalMatrix) {
        // the same test and shape functions: half of the matrix
        for(int i = 0; i < Nbr_Equ; i++)
          for(int j = 0; j <= i; j++)
            EK(j, i)[0] = EK(i, j)[0] =
              Factor * Cal_AnalyticIntegration(
                         Element, (void (*)())xFunctionBFEqu[i],
                         (void (*)())xFunctionBFEqu[j], i, j, FI->Cal_Productx);
      }
      else if(FI->Type_DefineQuantityDof == LOCALQUANTITY) {
        for(int i = 0; i < Nbr_Equ; i++)
          for(int j = 0; j < Nbr_Dof; j++)
            EK(i, j)[0] = Factor * Cal_AnalyticIntegration(
                                     Element, (void (*)())xFunctionBFEqu[i],
                                     (void (*)())xFunctionBFDof[j], i, j,
                                     FI->Cal_Productx);
      }
      else {
        Message::Error("Exterior analytical integration not implemented");
      }
    }

    void print()
    {
      printf("Galerkin = ");
      for(int j = 0; j < Nbr_Dof; j++)
        Print_DofNumber(QuantityStorageDof_P->BasisFunction[j].Dof);
      printf("\n");
      for(int i = 0; i < Nbr_Equ; i++) {
        Print_DofNumber(QuantityStorageEqu_P->BasisFunction[i].Dof);
        printf("[ ");
        for(int j = 0; j < Nbr_Dof; j++) {
          printf("(");
          for(int k = 0; k < NbrHar; k++) printf("% .5e ", EK(i, j)[k]);
          printf(")");
        }
        printf("]\n");
      }
    }

  public:
    GalerkinTerm(struct Element *Element, struct EquationTerm *EquationTerm_P,
                 struct QuantityStorage *QuantityStorage_P0)
      : Element(Element), EquationTerm_P(EquationTerm_P),
        QuantityStorage_P0(QuantityStorage_P0),
        FI(EquationTerm_P->Case.LocalTerm.Active),
        QuantityStorageEqu_P(FI->QuantityStorageEqu_P),
        QuantityStorageDof_P(FI->QuantityStorageDof_P), NbrHar(Current.NbrHar)
    {
    }

    // The test and shape functions of the element, its integration method and
    // its Jacobian; returns false if there is nothing to compute
    bool init()
    {
      auto &Term = EquationTerm_P->Case.LocalTerm.Term;
      if(!(Nbr_Equ = QuantityStorageEqu_P->NbrElementaryBasisFunction))
        return false;
      xFunctionBFEqu.resize(Nbr_Equ);
      vBFuEqu.resize(Nbr_Equ);
      vBFxEqu.resize(Nbr_Equ);
      Get_FunctionValue(Nbr_Equ, (void (**)())xFunctionBFEqu.data(),
                        Term.TypeOperatorEqu, QuantityStorageEqu_P,
                        &FI->Type_FormEqu);

      switch(FI->Type_DefineQuantityDof) {
      case LOCALQUANTITY:
        Nbr_Dof = QuantityStorageDof_P->NbrElementaryBasisFunction;
        resizeDof();
        Get_FunctionValue(Nbr_Dof, (void (**)())xFunctionBFDof.data(),
                          Term.TypeOperatorDof, QuantityStorageDof_P,
                          &FI->Type_FormDof);
        break;
      case INTEGRALQUANTITY:
        Get_InitElementSource(
          Element,
          QuantityStorageDof_P->DefineQuantity->IntegralQuantity.InIndex);
        break;
      case NODOF: Nbr_Dof = 1; break;
      }
      resizeDof();

      IntegrationCase_P =
        Get_IntegrationCase(Element, FI->IntegrationCase_L, FI->CriterionIndex);
      return setJacobian();
    }

    // With an integral quantity, the shape functions are those of each source
    // element in turn: returns false when there are none left
    bool nextSource()
    {
      if(!Get_NextElementSource(Element->ElementSource)) return false;
      Get_DofOfElement(Element->ElementSource,
                       QuantityStorageDof_P->FunctionSpace,
                       QuantityStorageDof_P, NULL);
      Get_NodesCoordinatesOfElement(Element->ElementSource);
      Nbr_Dof = QuantityStorageDof_P->NbrElementaryBasisFunction;
      resizeDof();
      Get_FunctionValue(
        Nbr_Dof, (void (**)())xFunctionBFDof.data(),
        QuantityStorageDof_P->DefineQuantity->IntegralQuantity.TypeOperatorDof,
        QuantityStorageDof_P, &FI->IntegralQuantityActive.Type_FormDof);
      Cal_InitIntegralQuantity(Element, &FI->IntegralQuantityActive,
                               QuantityStorageDof_P);
      return true;
    }

    // the elementary matrix
    void compute()
    {
      for(int i = 0; i < Nbr_Equ; i++)
        for(int j = 0; j < Nbr_Dof; j++)
          for(int k = 0; k < NbrHar; k++) EK(i, j)[k] = 0.;

      switch(IntegrationCase_P->Type) {
      case GAUSS:
      case GAUSSLEGENDRE:
      case COLLOCATION: quadrature(); break;
      case ANALYTIC: analytic(); break;
      default:
        Message::Error("Unknown type of Integration method (%s)",
                       integrationName());
        break;
      }
      if(Message::GetVerbosity() == 10) print();
    }

    // the assembly of the elementary matrix in the global matrix
    void assemble()
    {
      void (*Assemble)(struct Dof *, struct Dof *, double *) =
        FI->Function_AssembleTerm;
      auto equ = [&](int i) {
        return QuantityStorageEqu_P->BasisFunction[i].Dof;
      };
      auto dof = [&](int j) {
        return QuantityStorageDof_P->BasisFunction[j].Dof;
      };

      if(!Current.flagAssDiag) { /*+++prov*/
        gMatrix *Mat;
        gVector *Vec;
        List_T *Vecs;
        double Factor[2];
        if(Cal_GetAssembleTermMatrix(Assemble, &Mat, &Vec, &Vecs, Factor)) {
          // the whole elementary matrix at once
          StackArray<struct Dof *, NBR_BASISFUNCTIONS_ON_STACK> Equ, Dof;
          Equ.resize(Nbr_Equ);
          Dof.resize(Nbr_Dof);
          for(int i = 0; i < Nbr_Equ; i++) Equ[i] = equ(i);
          for(int j = 0; j < Nbr_Dof; j++) Dof[j] = dof(j);
          Dof_AssembleElementaryMatrix(Nbr_Equ, Equ.data(), Nbr_Dof,
                                       Dof.data(), NbrHar, Ek.data(), Nbr_Ek,
                                       Factor, Mat, Vec, Vecs);
        }
        else
          for(int i = 0; i < Nbr_Equ; i++)
            for(int j = 0; j < Nbr_Dof; j++)
              Assemble(equ(i), dof(j), EK(i, j));
      }
      else if(Current.flagAssDiag == 1) {
        // the diagonal only
        for(int i = 0; i < Nbr_Equ; i++) Assemble(equ(i), dof(i), EK(i, i));
      }
      else if(Current.flagAssDiag == 2) {
        // ones on the diagonal, once per unknown
        for(int i = 0; i < Nbr_Equ; i++) {
          if(equ(i)->Type != DOF_UNKNOWN) continue;
          int num = equ(i)->Case.Unknown.NumDof - 1;
          if(assDiag_done.find(num) != assDiag_done.end()) continue;
          assDiag_done[num] = true;
          EK(i, i)[0] = 1.;
          for(int k = 1; k < NbrHar; k++) EK(i, i)[k] = 0.;
          Assemble(equ(i), dof(i), EK(i, i));
        }
      }
    }
  };

} // namespace

void Cal_GalerkinTermOfFemEquation(struct Element *Element,
                                   struct EquationTerm *EquationTerm_P,
                                   struct QuantityStorage *QuantityStorage_P0)
{
  extern int Flag_RHS;
  struct FemLocalTermActive *FI = EquationTerm_P->Case.LocalTerm.Active;

  Current.flagAssDiag = 0; /*+++prov*/

  /* the terms of the Jacobian matrix are not computed if there is none */
  if((FI->Function_AssembleTerm == Cal_AssembleTerm_JacNL ||
      FI->Function_AssembleTerm == Cal_AssembleTerm_DtDofJacNL ||
      (FI->MHBilinear && FI->MHBilinear_JacNL)) &&
     !Cal_HasJacobianMatrix())
    return;

  /* treatment of MHBilinear-term in separate routine */
  if(FI->MHBilinear) {
    /* if only the RHS of the system is to be calculated
       (in case of adaptive relaxation of the Newton-Raphson scheme)
       the (expensive and redundant) calculation of this term can be skipped */
    if(!Flag_RHS)
      Cal_GalerkinTermOfFemEquation_MHBilinear(Element, EquationTerm_P,
                                               QuantityStorage_P0);
    return;
  }

  /* skip computation completely if term does not contribute to RHS. This is OK,
     but the speed-up is not the best, due to the time-consuming--but
     necessary-- initializations that still need to be done before arriving at
     this point in the assembly process. For best performance use
     GenerateRHSGroup instead of GenerateRHS, and include any RHS-contributions
     (elements containing fixed dofs or non-dof expressions) in the given
     groups  */
  if(Current.DofData->Flag_RHS &&
     FI->Type_DefineQuantityDof == LOCALQUANTITY) {
    struct QuantityStorage *Q = FI->QuantityStorageDof_P;
    bool skip = true;
    for(int j = 0; j < Q->NbrElementaryBasisFunction; j++) {
      if(Q->BasisFunction[j].Dof->Type != DOF_UNKNOWN) {
        skip = false;
        break;
      }
    }
    if(skip) return;
  }

  GalerkinTerm Term(Element, EquationTerm_P, QuantityStorage_P0);
  if(!Term.init()) return;

  if(FI->Type_DefineQuantityDof == INTEGRALQUANTITY) {
    // one elementary matrix per source element
    while(Term.nextSource()) {
      Term.compute();
      Term.assemble();
    }
  }
  else {
    Term.compute();
    Term.assemble();
  }

  Current.flagAssDiag = 0; /*+++prov*/
}
