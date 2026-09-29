// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <stdlib.h>
#include "GetDPConfig.h"
#include "ProData.h"
#include "Message.h"
#include "BF.h"

#if defined(HAVE_KERNEL)
#include "GeoData.h"
#endif

extern struct CurrentData Current;

/* The non-symmetric facet functions are selected according to the NumIndex^th
   smallest global node number */

int fcmp_Int2(const void *a, const void *b)
{
  return ((struct TwoInt *)a)->Int2 - ((struct TwoInt *)b)->Int2;
}

int Get_FacetFunctionIndex(struct Element *Element, int NumEntity, int NumIndex)
{
#if !defined(HAVE_KERNEL)
  Message::Error("Get_FacetFunctionIndex requires Kernel");
  return 0;
#else

  int i, j, *NumNodes;

  if(Element->NumLastElementForSortedNodesByFacet != Element->Num) {
    for(i = 0; i < Element->GeoElement->NbrFacets; i++) {
      NumNodes = Geo_GetNodesOfFacetInElement(Element->GeoElement, i);
      j = 0;
      while(NumNodes[j]) {
        Element->SortedNodesByFacet[i][j].Int1 = NumNodes[j];
        int t = Element->GeoElement->NumNodes[NumNodes[j] - 1];
        // check if the node is a periodic copy of a master (reference) node; if
        // it is, use the global tag of the master node, so that the basis
        // function will be the same in the master facet and its periodic
        // counterpart
        if(Current.GeoData->PeriodicNodes) {
          auto it = Current.GeoData->PeriodicNodes->find(t);
          if(it != Current.GeoData->PeriodicNodes->end()) {
            t = it->second;
            Message::Debug("Found master node %d in BF_Edge_3F", t);
          }
        }
        Element->SortedNodesByFacet[i][j].Int2 = t;
        j++;
      }
      qsort(Element->SortedNodesByFacet[i], j, sizeof(struct TwoInt),
            fcmp_Int2);
    }

    Element->NumLastElementForSortedNodesByFacet = Element->Num;
  }

  return Element->SortedNodesByFacet[NumEntity - 1][NumIndex - 1].Int1;
#endif
}

/* Reference coordinates of the 8 nodes of the hexahedron.  Same table as
   Nodes_Hexahedron in kernel/GeoEntity.h; repeated here rather than pulling
   that header -- a pile of file-static incidence matrices -- into a basis
   function translation unit.  The numbering is gmsh's and does not move. */

static const double Hex_Nodes[8][3] = {
  {-1., -1., -1.}, {1., -1., -1.}, {1., 1., -1.}, {-1., 1., -1.},
  {-1., -1., 1.},  {1., -1., 1.},  {1., 1., 1.},  {-1., 1., 1.}};

/* The canonical frame of a quadrangular facet of a hexahedron.

   Two hexahedra share a facet with unrelated local parametrisations, so a face
   function written on the local (u,v,w) of one does not match the other's: the
   tangential trace jumps and H(curl) conformity is gone.  The frame is
   therefore built from the GLOBAL node numbers, the same device the
   tetrahedron uses through Get_FacetFunctionIndex:

     - the origin C is the facet node of smallest global number;
     - of the two facet nodes ADJACENT to C, the one of smaller global number
       gives +xi, the other +eta;
     - zeta = xi x eta closes a right-handed frame.

   All three are properties of the facet alone, so both elements agree on them,
   and the traces match.  Adjacency is read off the reference coordinates: a
   neighbour of C on the facet differs from it in exactly one coordinate, the
   diagonal node in two.

   On return (xi, eta) are the facet coordinates in [-1,1]^2, g is the
   transverse profile -- 1 on the facet, 0 on the opposite one -- and dg its
   derivative along zeta.  xiDir, etaDir and zetaDir are the frame axes in the
   reference element, each of them one of +-e_u, +-e_v, +-e_w.

   Returns 0 if the frame cannot be built (no kernel, or a facet that is not
   the quadrangle it should be), in which case nothing is written. */

static int Hex_FacetFrame(struct Element *Element, int NumEntity, double u,
                          double v, double w, double *xi, double *eta,
                          double *g, double *dg, double xiDir[3],
                          double etaDir[3], double zetaDir[3])
{
  const double X[3] = {u, v, w};
  int i, k, d, nbDir = 0, c = Get_FacetFunctionIndex(Element, NumEntity, 1);

  if(c < 1 || c > 8) return 0;
  const double *Xc = Hex_Nodes[c - 1];

  for(k = 2; k <= 4 && nbDir < 2; k++) {
    i = Get_FacetFunctionIndex(Element, NumEntity, k);
    if(i < 1 || i > 8) return 0;
    int nbDiff = 0;
    for(d = 0; d < 3; d++)
      if(Hex_Nodes[i - 1][d] != Xc[d]) nbDiff++;
    if(nbDiff != 1) continue; /* the diagonal node, not a neighbour */
    double *dir = nbDir ? etaDir : xiDir;
    for(d = 0; d < 3; d++) dir[d] = 0.5 * (Hex_Nodes[i - 1][d] - Xc[d]);
    nbDir++;
  }
  if(nbDir != 2) return 0;

  zetaDir[0] = xiDir[1] * etaDir[2] - xiDir[2] * etaDir[1];
  zetaDir[1] = xiDir[2] * etaDir[0] - xiDir[0] * etaDir[2];
  zetaDir[2] = xiDir[0] * etaDir[1] - xiDir[1] * etaDir[0];

  double xC = 0., eC = 0., zC = 0., xX = 0., eX = 0., zX = 0.;
  for(d = 0; d < 3; d++) {
    xC += xiDir[d] * Xc[d];   xX += xiDir[d] * X[d];
    eC += etaDir[d] * Xc[d];  eX += etaDir[d] * X[d];
    zC += zetaDir[d] * Xc[d]; zX += zetaDir[d] * X[d];
  }
  *xi = xX - xC - 1.;
  *eta = eX - eC - 1.;
  /* zC = +-1 says which side of the element the facet sits on, so that
     g = 1 there and 0 on the opposite facet. */
  *g = 0.5 * (1. + zC * zX);
  *dg = 0.5 * zC;
  return 1;
}

/* The same canonical frame for a QUADRANGLE, whose only facet is itself.  A
   quadrangle lying on a facet of a hexahedron (a boundary region carrying the
   facet dofs) then evaluates exactly the trace of the hexahedron's function:
   both frames are built from the same global node numbers.  In a purely 2D
   mesh the facet is interior and any frame would do, but this one costs
   nothing.

   On return (xi, eta) are the facet coordinates in [-1,1]^2, and xiDir and
   etaDir -- each of them one of +-e_u, +-e_v -- the frame axes in the
   reference element.  Returns 0 if the frame cannot be built. */

static const double Quad_Nodes[4][2] = {{-1., -1.}, {1., -1.}, {1., 1.}, {-1., 1.}};

static int Quad_FacetFrame(struct Element *Element, double u, double v,
                           double *xi, double *eta, double xiDir[2],
                           double etaDir[2])
{
  const double X[2] = {u, v};
  int i, k, d, nbDir = 0, c = Get_FacetFunctionIndex(Element, 1, 1);

  if(c < 1 || c > 4) return 0;
  const double *Xc = Quad_Nodes[c - 1];

  for(k = 2; k <= 4 && nbDir < 2; k++) {
    i = Get_FacetFunctionIndex(Element, 1, k);
    if(i < 1 || i > 4) return 0;
    int nbDiff = 0;
    for(d = 0; d < 2; d++)
      if(Quad_Nodes[i - 1][d] != Xc[d]) nbDiff++;
    if(nbDiff != 1) continue; /* the diagonal node, not a neighbour */
    double *dir = nbDir ? etaDir : xiDir;
    for(d = 0; d < 2; d++) dir[d] = 0.5 * (Quad_Nodes[i - 1][d] - Xc[d]);
    nbDir++;
  }
  if(nbDir != 2) return 0;

  double xC = 0., eC = 0., xX = 0., eX = 0.;
  for(d = 0; d < 2; d++) {
    xC += xiDir[d] * Xc[d];  xX += xiDir[d] * X[d];
    eC += etaDir[d] * Xc[d]; eX += etaDir[d] * X[d];
  }
  *xi = xX - xC - 1.;
  *eta = eX - eC - 1.;
  return 1;
}

/* The two facet functions on the reference square, in the canonical frame.
   These are the QUADRANGLE case below, so the trace of the hexahedral function
   on its facet IS the 2D one -- which is what the WARNING there asks for.
   Both have zero tangential trace on the four edges of the facet, and the
   transverse profile g keeps it zero on the other five facets of the element:
   f_xi vanishes at eta = +-1 and f_eta at xi = +-1. */

static int Hex_FacetFunction(int Index, double xi, double eta, double *f_xi,
                             double *f_eta, double *curl2d)
{
  switch(Index) {
  case 1: /* _a */
    *f_xi = 45. / 16. * (1. - xi) * (1. - eta * eta);
    *f_eta = 45. / 16. * (1. - eta) * (1. - xi * xi);
    *curl2d = 45. / 8. * (eta - xi);
    return 1;
  case 2: /* _b */
    *f_xi = 45. / 16. * (1. + xi) * (eta * eta - 1.);
    *f_eta = 45. / 16. * (1. - eta) * (1. - xi * xi);
    *curl2d = -45. / 8. * (eta + xi);
    return 1;
  /* _a and _b alone span only (1-eta^2) e_xi and a combination BIASED toward
     eta = -1, i.e. toward the facet node of smallest global number: the space
     then depends on the numbering, not on the geometry, and it breaks the
     symmetries of the mesh (on a straight stack it forces a zigzag of t along
     the sweep).  _c and _d complete it to the full set
       (1-eta^2) e_xi,  xi (1-eta^2) e_xi,  (1-xi^2) e_eta,  eta (1-xi^2) e_eta,
     which every symmetry of the square maps onto itself. */
  case 3: /* _c:  xi (1-eta^2) e_xi */
    *f_xi = 45. / 16. * xi * (1. - eta * eta);
    *f_eta = 0.;
    *curl2d = 45. / 8. * xi * eta;
    return 1;
  case 4: /* _d:  eta (1-xi^2) e_eta */
    *f_xi = 0.;
    *f_eta = 45. / 16. * eta * (1. - xi * xi);
    *curl2d = -45. / 8. * xi * eta;
    return 1;
  default: return 0;
  }
}

/* Derivatives of the two facet functions with respect to (xi, eta).  Split out
   of Hex_FacetFunction rather than folded into it because only the pyramid
   below needs them: the hexahedron's curl is g(z) times the 2D curl, and that
   one number is all it wants. */

static int Hex_FacetFunctionD(int Index, double xi, double eta, double *dfxi_dxi,
                              double *dfxi_deta, double *dfeta_dxi,
                              double *dfeta_deta)
{
  const double c = 45. / 16.;
  switch(Index) {
  case 1: /* _a:  f_xi = c (1-xi)(1-eta^2),  f_eta = c (1-eta)(1-xi^2) */
    *dfxi_dxi = -c * (1. - eta * eta);
    *dfxi_deta = -2. * c * (1. - xi) * eta;
    *dfeta_dxi = -2. * c * (1. - eta) * xi;
    *dfeta_deta = -c * (1. - xi * xi);
    return 1;
  case 2: /* _b:  f_xi = c (1+xi)(eta^2-1),  f_eta = c (1-eta)(1-xi^2) */
    *dfxi_dxi = c * (eta * eta - 1.);
    *dfxi_deta = 2. * c * (1. + xi) * eta;
    *dfeta_dxi = -2. * c * (1. - eta) * xi;
    *dfeta_deta = -c * (1. - xi * xi);
    return 1;
  case 3: /* _c:  f_xi = c xi (1-eta^2),  f_eta = 0 */
    *dfxi_dxi = c * (1. - eta * eta);
    *dfxi_deta = -2. * c * xi * eta;
    *dfeta_dxi = 0.;
    *dfeta_deta = 0.;
    return 1;
  case 4: /* _d:  f_xi = 0,  f_eta = c eta (1-xi^2) */
    *dfxi_dxi = 0.;
    *dfxi_deta = 0.;
    *dfeta_dxi = -2. * c * xi * eta;
    *dfeta_deta = c * (1. - xi * xi);
    return 1;
  default: return 0;
  }
}

/* Reference coordinates of the 5 nodes of the pyramid; same table as
   Nodes_Pyramid in kernel/GeoEntity.h.  The quadrangular base is at w = 0, the
   apex at w = 1, and the cross-section at height w is [-(1-w), (1-w)]^2. */

static const double Pyr_Nodes[5][3] = {
  {-1., -1., 0.}, {1., -1., 0.}, {1., 1., 0.}, {-1., 1., 0.}, {0., 0., 1.}};

#define PYR_BASE_FACET 2 /* Dfn_Pyramid: facet 2 = {1,4,3,2}, the quadrangle */

/* The canonical frame of the quadrangular BASE of a pyramid, and the gradients
   of its coordinates.

   Same device as Hex_FacetFrame -- origin at the base node of smallest global
   number, +xi towards the smaller of its two neighbours -- so a pyramid and the
   hexahedron glued to its base agree on (xi, eta) and their traces match.  Only
   the base is handled: the four triangles are shared with tetrahedra, whose
   order-2 family is a different one (grad(N_a N_b), not this), and no dof is
   ever put there.

   The coordinates are those of the SHRUNK section: a = u/(1-w), b = v/(1-w),
   which are constant along the rays to the apex and run over [-1,1] on every
   section.  That is what makes the lateral faces the level sets a = +-1 and
   b = +-1, i.e. xi = +-1 and eta = +-1, where the facet functions vanish.

   Returns 0 if this is not the base facet, if the frame cannot be built, or at
   the apex itself (w = 1), where a and b are 0/0. */

static int Pyr_BaseFrame(struct Element *Element, int NumEntity, double u,
                         double v, double w, double *xi, double *eta,
                         double xiDir[3], double etaDir[3], double gradXi[3],
                         double gradEta[3])
{
  int i, k, d, nbDir = 0;

  if(NumEntity != PYR_BASE_FACET) return 0;
  if(w >= 1.) return 0;

  int c = Get_FacetFunctionIndex(Element, NumEntity, 1);
  if(c < 1 || c > 4) return 0; /* the base carries nodes 1..4 only */
  const double *Xc = Pyr_Nodes[c - 1];

  for(k = 2; k <= 4 && nbDir < 2; k++) {
    i = Get_FacetFunctionIndex(Element, NumEntity, k);
    if(i < 1 || i > 4) return 0;
    int nbDiff = 0;
    for(d = 0; d < 2; d++)
      if(Pyr_Nodes[i - 1][d] != Xc[d]) nbDiff++;
    if(nbDiff != 1) continue; /* the diagonal node, not a neighbour */
    double *dir = nbDir ? etaDir : xiDir;
    for(d = 0; d < 3; d++) dir[d] = 0.5 * (Pyr_Nodes[i - 1][d] - Xc[d]);
    nbDir++;
  }
  if(nbDir != 2) return 0;

  /* a and b, and their gradients in the reference element */
  const double t = 1. - w;
  const double a = u / t, b = v / t;
  const double gradA[3] = {1. / t, 0., a / t};
  const double gradB[3] = {0., 1. / t, b / t};
  const double A[3] = {a, b, 0.};

  double xC = 0., eC = 0., xA = 0., eA = 0.;
  for(d = 0; d < 3; d++) {
    xC += xiDir[d] * Xc[d];  xA += xiDir[d] * A[d];
    eC += etaDir[d] * Xc[d]; eA += etaDir[d] * A[d];
  }
  *xi = xA - xC - 1.;
  *eta = eA - eC - 1.;

  /* xiDir and etaDir are each +-e_u or +-e_v, so the chain rule is just the
     matching gradient of a or b, signed. */
  for(d = 0; d < 3; d++) {
    gradXi[d] = xiDir[0] * gradA[d] + xiDir[1] * gradB[d];
    gradEta[d] = etaDir[0] * gradA[d] + etaDir[1] * gradB[d];
  }
  return 1;
}

/* The facet function of the pyramid's base, and its curl.

       F = f_xi xiHat + f_eta etaHat + (xi f_xi + eta f_eta) e_w

   The first two terms are the hexahedron's, evaluated on the shrunk section, so
   the trace on the base (w = 0, where the e_w term is purely normal) is exactly
   the hexahedron's and the two agree.

   The third term is what the hexahedron does NOT need.  Its facets are
   coordinate planes, so xiHat is the normal of the facet at xi = +-1 and
   contributes nothing tangential there; the pyramid's lateral faces are SLANTED
   -- the one at xi = +1 has reference normal xiHat + e_w -- so xiHat does have
   a tangential part on them, and f_xi does not vanish there for _b.  Requiring
   F to be parallel to the normal on each of the four lateral faces gives

       h(+1, eta) = +f_xi,  h(-1, eta) = -f_xi,
       h(xi, +1) = +f_eta,  h(xi, -1) = -f_eta,

   and h = xi f_xi + eta f_eta satisfies all four at once, for both _a and _b.
   So the tangential trace vanishes on all four triangles and the pyramid needs
   no agreement with the tetrahedra behind them.

   Writes the value if Curl is 0, the curl if it is 1.  Returns 0 if this is not
   the base facet or the function does not exist. */

static int Pyr_BaseFunction(struct Element *Element, int NumEntity, int Index,
                            double u, double v, double w, int Curl, double s[])
{
  double xi, eta, f_xi, f_eta, curl2d;
  double xiDir[3], etaDir[3], gradXi[3], gradEta[3];
  double dfxi_dxi, dfxi_deta, dfeta_dxi, dfeta_deta;
  int d;

  /* the apex itself: u/(1-w) is 0/0 there.  Only post-processing at the nodes
     ever asks (no Gauss point sits on it), and zero is as good a value as any */
  if(NumEntity == PYR_BASE_FACET && w >= 1.) {
    s[0] = s[1] = s[2] = 0.;
    return 1;
  }
  if(!Pyr_BaseFrame(Element, NumEntity, u, v, w, &xi, &eta, xiDir, etaDir,
                    gradXi, gradEta))
    return 0;
  if(!Hex_FacetFunction(Index, xi, eta, &f_xi, &f_eta, &curl2d)) return 0;

  if(!Curl) {
    const double h = xi * f_xi + eta * f_eta;
    for(d = 0; d < 3; d++) s[d] = f_xi * xiDir[d] + f_eta * etaDir[d];
    s[2] += h;
    return 1;
  }

  if(!Hex_FacetFunctionD(Index, xi, eta, &dfxi_dxi, &dfxi_deta, &dfeta_dxi,
                         &dfeta_deta))
    return 0;

  /* F in the reference axes, as F_k = f_xi xiDir[k] + f_eta etaDir[k] (+ h for
     k = w), and d/dx_k phi = phi_xi gradXi[k] + phi_eta gradEta[k]. */
  const double dh_dxi = f_xi + xi * dfxi_dxi + eta * dfeta_dxi;
  const double dh_deta = xi * dfxi_deta + f_eta + eta * dfeta_deta;

  double dF[3][3]; /* dF[k][l] = d F_k / d x_l */
  for(d = 0; d < 3; d++) {
    const double dfxi = dfxi_dxi * gradXi[d] + dfxi_deta * gradEta[d];
    const double dfeta = dfeta_dxi * gradXi[d] + dfeta_deta * gradEta[d];
    dF[0][d] = dfxi * xiDir[0] + dfeta * etaDir[0];
    dF[1][d] = dfxi * xiDir[1] + dfeta * etaDir[1];
    dF[2][d] = dfxi * xiDir[2] + dfeta * etaDir[2] +
               dh_dxi * gradXi[d] + dh_deta * gradEta[d];
  }
  s[0] = dF[2][1] - dF[1][2];
  s[1] = dF[0][2] - dF[2][0];
  s[2] = dF[1][0] - dF[0][1];
  return 1;
}

/* Facet functions on TRIANGULAR facets of elements other than the tetrahedron.

   The tetrahedron's function for the facet node k selected by
   Get_FacetFunctionIndex is N_i N_j grad N_k, with i and j the two other
   nodes of the facet (the cases of the TETRAHEDRON below).  Written with the
   element's own vertex functions it stays conforming on any element: on each
   face the vertex functions restrict to that face's vertex functions, and they
   vanish identically on the faces that do not contain their node.  So the
   trace on the facet is the tetrahedron's, and the tangential trace is zero
   on all the other faces -- whatever their shape.

   PYRAMID: N_i N_j grad N_k with the (rational) vertex functions of BF_Node.
   Its curl, grad(N_i N_j) x grad N_k, is bounded at the apex.

   PRISM: g(w) lambda_i lambda_j grad lambda_k, with lambda the barycentric
   coordinates of the triangle (u, v) and g = (1 -+ w)/2 the profile that is 1
   on the facet and 0 on the opposite one.  Same traces, but of degree 1 in w
   instead of the 3 that N_i N_j grad N_k would give.

   Writes the value if Curl is 0, the curl if it is 1.  Returns 0 if the
   function does not exist on this facet (Index 4, _d, exists on quadrangles
   only). */

static double Cross2(const double a[3], const double b[3])
{
  return a[0] * b[1] - a[1] * b[0];
}

static void Pri_Lambda(double u, double v, double lam[3], double dlam[3][3])
{
  lam[0] = 1. - u - v;
  lam[1] = u;
  lam[2] = v;
  dlam[0][0] = -1.; dlam[0][1] = -1.; dlam[0][2] = 0.;
  dlam[1][0] = 1.;  dlam[1][1] = 0.;  dlam[1][2] = 0.;
  dlam[2][0] = 0.;  dlam[2][1] = 1.;  dlam[2][2] = 0.;
}

static int Pyr_TriangleFunction(struct Element *Element, int NumEntity,
                                int Index, double u, double v, double w,
                                int Curl, double s[])
{
  int m, n[3], i = 0, j = 0, k;

  if(NumEntity == PYR_BASE_FACET || Index < 1 || Index > 3) return 0;
  for(m = 0; m < 3; m++) {
    n[m] = Get_FacetFunctionIndex(Element, NumEntity, m + 1);
    if(n[m] < 1 || n[m] > 5) return 0;
  }
  k = n[Index - 1];
  for(m = 0; m < 3; m++) {
    if(m == Index - 1) continue;
    if(!i) i = n[m];
    else j = n[m];
  }

  double Ni, Nj, dNi[3], dNj[3], dNk[3];
  BF_Node(Element, i, u, v, w, &Ni);
  BF_Node(Element, j, u, v, w, &Nj);
  BF_GradNode(Element, k, u, v, w, dNk);
  if(!Curl) {
    for(m = 0; m < 3; m++) s[m] = Ni * Nj * dNk[m];
    return 1;
  }
  BF_GradNode(Element, i, u, v, w, dNi);
  BF_GradNode(Element, j, u, v, w, dNj);
  double dP[3];
  for(m = 0; m < 3; m++) dP[m] = Nj * dNi[m] + Ni * dNj[m];
  s[0] = dP[1] * dNk[2] - dP[2] * dNk[1];
  s[1] = dP[2] * dNk[0] - dP[0] * dNk[2];
  s[2] = dP[0] * dNk[1] - dP[1] * dNk[0];
  return 1;
}

static int Pri_TriangleFunction(struct Element *Element, int NumEntity,
                                int Index, double u, double v, double w,
                                int Curl, double s[])
{
  double g, dg, lam[3], dlam[3][3];

  if(NumEntity == 2) { g = 0.5 * (1. - w); dg = -0.5; } /* {1, 3, 2}: w = -1 */
  else if(NumEntity == 5) { g = 0.5 * (1. + w); dg = 0.5; } /* {4, 5, 6} */
  else return 0;
  if(Index < 1 || Index > 3) return 0;
  const int kn = Get_FacetFunctionIndex(Element, NumEntity, Index);
  if(kn < 1 || kn > 6) return 0;
  const int k = (kn - 1) % 3, i = (k + 1) % 3, j = (k + 2) % 3;

  Pri_Lambda(u, v, lam, dlam);
  const double p = lam[i] * lam[j];
  if(!Curl) {
    for(int d = 0; d < 3; d++) s[d] = g * p * dlam[k][d];
    return 1;
  }
  /* curl(g Phi) = g curl Phi + grad g x Phi, Phi = p grad lambda_k in plane */
  double dp[3];
  for(int d = 0; d < 3; d++) dp[d] = lam[j] * dlam[i][d] + lam[i] * dlam[j][d];
  s[0] = -dg * p * dlam[k][1];
  s[1] = dg * p * dlam[k][0];
  s[2] = g * Cross2(dp, dlam[k]);
  return 1;
}

/* Facet functions on the three QUADRANGULAR facets of a PRISM, conforming
   with the hexahedron (and the pyramid base, and the quadrangle) through the
   same canonical frame -- see Hex_FacetFrame.

   A lateral facet is the edge (a, b) of the triangle times w in [-1,1].  On it
   take sigma = lambda_b - lambda_a and w as coordinates, both in [-1,1].  Every
   facet function, in any frame, is there

       (1 - w^2)(alpha + beta sigma) d sigma + (1 - sigma^2)(gamma + delta w) d w

   and it is extended into the prism by

       F = (1 - w^2) [ 2 alpha W_ab - 2 beta grad(lambda_a lambda_b) ]
           + 4 lambda_a lambda_b (gamma + delta w) e_w ,

   W_ab = lambda_a grad lambda_b - lambda_b grad lambda_a the Whitney function
   of the edge.  On the facet W_ab = d sigma / 2, grad(lambda_a lambda_b) =
   -sigma/2 d sigma and 4 lambda_a lambda_b = 1 - sigma^2, so the trace is the
   one above.  W_ab and grad(lambda_a lambda_b) have no tangential trace on the
   two other lateral facets, where lambda_a lambda_b vanishes too, and 1 - w^2
   and e_w leave none on the triangles.

   (alpha, beta, gamma, delta) are read off Hex_FacetFunction in the canonical
   frame, whose axes are +-sigma and +-w.  Checked symbolically on the three
   facets, all 24 orderings of the global node numbers and the four functions:
   trace equal to the hexahedron's, zero tangential trace on the four other
   facets, and the curl below. */

static void Pri_FacetCoord(int Node, int a, double y[2])
{
  y[0] = ((Node - 1) % 3 == a) ? -1. : 1.;
  y[1] = (Node <= 3) ? -1. : 1.;
}

static int Pri_QuadrangleFunction(struct Element *Element, int NumEntity,
                                  int Index, double u, double v, double w,
                                  int Curl, double s[])
{
  int n[4], k, d, a = 3, b = -1, nbDir = 0;

  if(NumEntity != 1 && NumEntity != 3 && NumEntity != 4) return 0;
  for(k = 0; k < 4; k++) {
    n[k] = Get_FacetFunctionIndex(Element, NumEntity, k + 1);
    if(n[k] < 1 || n[k] > 6) return 0;
    const int t = (n[k] - 1) % 3;
    if(t < a) a = t;
    if(t > b) b = t;
  }
  if(a >= b) return 0;

  /* the canonical frame, in the facet coordinates (sigma, w) */
  double Yc[2], Yk[2], xiDir[2], etaDir[2];
  Pri_FacetCoord(n[0], a, Yc);
  for(k = 1; k < 4 && nbDir < 2; k++) {
    Pri_FacetCoord(n[k], a, Yk);
    if((Yk[0] != Yc[0]) + (Yk[1] != Yc[1]) != 1) continue; /* the diagonal */
    double *dir = nbDir ? etaDir : xiDir;
    for(d = 0; d < 2; d++) dir[d] = 0.5 * (Yk[d] - Yc[d]);
    nbDir++;
  }
  if(nbDir != 2) return 0;

  /* f_xi = (1-eta^2)(P0 + P1 xi), f_eta = (1-xi^2)(Q0 + Q1 eta) */
  double f00x, f00e, f10x, f10e, f01x, f01e, c2d;
  if(!Hex_FacetFunction(Index, 0., 0., &f00x, &f00e, &c2d) ||
     !Hex_FacetFunction(Index, 1., 0., &f10x, &f10e, &c2d) ||
     !Hex_FacetFunction(Index, 0., 1., &f01x, &f01e, &c2d))
    return 0;
  const double P0 = f00x, P1 = f10x - f00x, Q0 = f00e, Q1 = f01e - f00e;

  double al, be, ga, de;
  if(xiDir[0] != 0.) { /* xi = s1 sigma, eta = s2 w */
    al = xiDir[0] * P0; be = P1; ga = etaDir[1] * Q0; de = Q1;
  }
  else { /* xi = s1 w, eta = s2 sigma */
    al = etaDir[0] * Q0; be = Q1; ga = xiDir[1] * P0; de = P1;
  }

  double lam[3], dlam[3][3];
  Pri_Lambda(u, v, lam, dlam);
  const double la = lam[a], lb = lam[b];
  const double *dla = dlam[a], *dlb = dlam[b];
  const double h = 1. - w * w, dh = -2. * w, q = ga + de * w;

  double X[2]; /* 2 alpha W_ab - 2 beta grad(lambda_a lambda_b), in plane */
  for(d = 0; d < 2; d++)
    X[d] = 2. * al * (la * dlb[d] - lb * dla[d]) -
           2. * be * (la * dlb[d] + lb * dla[d]);

  if(!Curl) {
    s[0] = h * X[0];
    s[1] = h * X[1];
    s[2] = 4. * la * lb * q;
    return 1;
  }
  /* curl(h X) = h curl X + grad h x X, with curl W_ab = 2 grad lambda_a x
     grad lambda_b and curl grad = 0; curl(phi e_w) = (d_v phi, -d_u phi, 0) */
  const double dphi_du = 4. * q * (la * dlb[0] + lb * dla[0]);
  const double dphi_dv = 4. * q * (la * dlb[1] + lb * dla[1]);
  s[0] = -dh * X[1] + dphi_dv;
  s[1] = dh * X[0] - dphi_du;
  s[2] = h * 4. * al * Cross2(dla, dlb);
  return 1;
}

static int Pri_FacetFunction(struct Element *Element, int NumEntity, int Index,
                             double u, double v, double w, int Curl,
                             double s[])
{
  if(NumEntity == 2 || NumEntity == 5)
    return Pri_TriangleFunction(Element, NumEntity, Index, u, v, w, Curl, s);
  return Pri_QuadrangleFunction(Element, NumEntity, Index, u, v, w, Curl, s);
}

/* ------------------------------------------------------------------------ */
/*  B F _ E d g e _ 3                                                       */
/* ------------------------------------------------------------------------ */

/* ------- */
/*  Edges  */
/* ------- */

#define WrongNumEntity Message::Error("Wrong Edge number in 'BF_Edge_3E'")

void BF_Edge_3E(struct Element *Element, int NumEntity, double u, double v,
                double w, double s[])
{
  Message::Error("You should never end up here!");
}

#undef WrongNumEntity

/* -------- */
/*  Facets  */
/* -------- */

#define WrongNumEntity Message::Error("Wrong Face number in 'BF_Edge_3F'")

void BF_Edge_3F(struct Element *Element, int NumEntity, int Index, double u,
                double v, double w, double s[])
{
  switch(Element->Type) {
  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4: Message::Error("You should never end up here!"); break;

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4:
    switch(NumEntity) {
    case 1:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 3:
        s[0] = 0.;
        s[1] = (1 - u - v) * u;
        s[2] = 0.;
        break;
      case 1:
        s[0] = -u * v;
        s[1] = -u * v;
        s[2] = 0.;
        break;
      case 2:
        s[0] = (1 - u - v) * v;
        s[1] = 0.;
        s[2] = 0.;
        break;
      }
      break;
    default: WrongNumEntity;
    }
    break;

  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4:
    switch(NumEntity) {
    case 1:
      /* The hexahedron's facet function, in the facet's canonical frame (see
         Quad_FacetFrame), so that it is the trace of the 3D one. */
      {
        double xi, eta, f_xi, f_eta, curl2d, xiDir[2], etaDir[2];
        if(!Quad_FacetFrame(Element, u, v, &xi, &eta, xiDir, etaDir) ||
           !Hex_FacetFunction(Index, xi, eta, &f_xi, &f_eta, &curl2d)) {
          s[0] = s[1] = s[2] = 0.;
          Message::Error("BF_Edge_3F_%c not available on QUADRANGLE",
                         'a' + Index - 1);
        }
        else {
          s[0] = f_xi * xiDir[0] + f_eta * etaDir[0];
          s[1] = f_xi * xiDir[1] + f_eta * etaDir[1];
          s[2] = 0.;
        }
      }
      break;
    default: WrongNumEntity;
    }
    break;

  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4:
    switch(NumEntity) {
    case 1:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 4:
        s[0] = 0.;
        s[1] = 0.;
        s[2] = u * (1. - u - v - w);
        break;
      case 1:
        s[0] = -u * w;
        s[1] = -u * w;
        s[2] = -u * w;
        break;
      case 2:
        s[0] = (1. - u - v - w) * w;
        s[1] = 0.;
        s[2] = 0.;
        break;
      }
      break;
    case 2:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 2:
        s[0] = v * (1 - u - v - w);
        s[1] = 0.;
        s[2] = 0.;
        break;
      case 1:
        s[0] = -u * v;
        s[1] = -u * v;
        s[2] = -u * v;
        break;
      case 3:
        s[0] = 0.;
        s[1] = u * (1. - u - v - w);
        s[2] = 0.;
        break;
      }
      break;
    case 3:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 3:
        s[0] = 0.;
        s[1] = (1. - u - v - w) * w;
        s[2] = 0.;
        break;
      case 1:
        s[0] = -v * w;
        s[1] = -v * w;
        s[2] = -v * w;
        break;
      case 4:
        s[0] = 0.;
        s[1] = 0.;
        s[2] = v * (1. - u - v - w);
        break;
      }
      break;
    case 4:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 4:
        s[0] = 0.;
        s[1] = 0.;
        s[2] = u * v;
        break;
      case 2:
        s[0] = v * w;
        s[1] = 0.;
        s[2] = 0.;
        break;
      case 3:
        s[0] = 0.;
        s[1] = u * w;
        s[2] = 0.;
        break;
      }
      break;
    default: WrongNumEntity;
    }
    break;

  /* s = g(zeta) [ f_xi xiHat + f_eta etaHat ], with (xi, eta, zeta) the
     canonical frame of the facet -- see Hex_FacetFrame.  Its trace on the
     facet is the QUADRANGLE function above, and it is tangentially zero on the
     other five facets, so the element stays H(curl) conforming.

     The same frame is used by the quadrangle, the base of the pyramid and the
     lateral facets of the prism, so the facet can be shared with any of them. */
  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    if(NumEntity < 1 || NumEntity > 6) {
      s[0] = s[1] = s[2] = 0.;
      WrongNumEntity;
    }
    else {
      double xi, eta, g, dg, f_xi, f_eta, curl2d;
      double xiDir[3], etaDir[3], zetaDir[3];
      if(!Hex_FacetFrame(Element, NumEntity, u, v, w, &xi, &eta, &g, &dg,
                         xiDir, etaDir, zetaDir) ||
         !Hex_FacetFunction(Index, xi, eta, &f_xi, &f_eta, &curl2d)) {
        s[0] = s[1] = s[2] = 0.;
        Message::Error("BF_Edge_3F_%c not available on HEXAHEDRON",
                       'a' + Index - 1);
      }
      else {
        for(int d = 0; d < 3; d++)
          s[d] = g * (f_xi * xiDir[d] + f_eta * etaDir[d]);
      }
    }
    break;

  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4:
    if(!Pri_FacetFunction(Element, NumEntity, Index, u, v, w, 0, s)) {
      s[0] = s[1] = s[2] = 0.;
      Message::Error("BF_Edge_3F_%c not available on facet %d of PRISM",
                     'a' + Index - 1, NumEntity);
    }
    break;

  /* The quadrangular base (see Pyr_BaseFunction) and the four triangles (see
     Pyr_TriangleFunction), conforming with hexahedra, prisms and pyramids on
     the one and with tetrahedra, prisms and pyramids on the others. */
  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    if(!(NumEntity == PYR_BASE_FACET ?
           Pyr_BaseFunction(Element, NumEntity, Index, u, v, w, 0, s) :
           Pyr_TriangleFunction(Element, NumEntity, Index, u, v, w, 0, s))) {
      s[0] = s[1] = s[2] = 0.;
      Message::Error("BF_Edge_3F_%c not available on facet %d of PYRAMID",
                     'a' + Index - 1, NumEntity);
    }
    break;

  default: Message::Error("Unknown type of Element in BF_Edge_3F"); break;
  }
}

#undef WrongNumEntity

void BF_Edge_3F_a(struct Element *Element, int NumEntity, double u, double v,
                  double w, double s[])
{
  BF_Edge_3F(Element, NumEntity, 1, u, v, w, s);
}

void BF_Edge_3F_b(struct Element *Element, int NumEntity, double u, double v,
                  double w, double s[])
{
  BF_Edge_3F(Element, NumEntity, 2, u, v, w, s);
}

void BF_Edge_3F_c(struct Element *Element, int NumEntity, double u, double v,
                  double w, double s[])
{
  BF_Edge_3F(Element, NumEntity, 3, u, v, w, s);
}

void BF_Edge_3F_d(struct Element *Element, int NumEntity, double u, double v,
                  double w, double s[])
{
  BF_Edge_3F(Element, NumEntity, 4, u, v, w, s);
}

/* -------- */
/*  Volume  */
/* -------- */

void BF_Edge_3V(struct Element *Element, int NumEntity, double u, double v,
                double w, double s[])
{
  Message::Error("You should never end up here!");
}

/* The six interior functions of the hexahedron, which complete the second
   order Nedelec space of the first kind, Q(1,2,2) x Q(2,1,2) x Q(2,2,1), of
   dimension 54 = 12 BF_Edge + 12 BF_Edge_2E (or BF_GradNode_2E) + 24
   BF_Edge_3F_a..d + 6 of these.  For each direction i, with j and k the two
   others,

       _a, _c, _e = (1-x_j^2)(1-x_k^2) e_i,   _b, _d, _f = x_i (1-x_j^2)(1-x_k^2) e_i,

   i = u for _a/_b, v for _c/_d, w for _e/_f.  x_i e_i is tangential to the
   facets x_j = +-1 and x_k = +-1, where the profile vanishes, and normal to the
   facets x_i = +-1: the tangential trace is zero on the whole boundary, so the
   functions are purely local (Entity VolumesOf) and need no frame.

   Writes the value if Curl is 0, the curl if it is 1.  Returns 0 if the index
   does not exist. */

static int Hex_VolumeFunction(int Index, double u, double v, double w,
                              int Curl, double s[])
{
  if(Index < 1 || Index > 6) return 0;
  const double X[3] = {u, v, w};
  const int i = (Index - 1) / 2, j = (i + 1) % 3, k = (i + 2) % 3;
  const double p = (Index % 2) ? 1. : X[i]; /* 1 for _a/_c/_e, x_i otherwise */
  const double Pj = 1. - X[j] * X[j], Pk = 1. - X[k] * X[k];

  s[0] = s[1] = s[2] = 0.;
  if(!Curl) {
    s[i] = p * Pj * Pk;
    return 1;
  }
  /* F = phi e_i with phi = p Pj Pk, and (i, j, k) a cyclic permutation:
     curl F = grad phi x e_i = d_k phi e_j - d_j phi e_k (p does not depend on
     x_j or x_k) */
  s[j] = p * Pj * (-2. * X[k]);
  s[k] = -p * (-2. * X[j]) * Pk;
  return 1;
}

static void BF_Edge_3V_Hex(struct Element *Element, int Index, double u,
                           double v, double w, int Curl, double s[])
{
  switch(Element->Type) {
  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    if(Hex_VolumeFunction(Index, u, v, w, Curl, s)) return;
    break;
  default: break;
  }
  s[0] = s[1] = s[2] = 0.;
  Message::Error("BF_%sEdge_3V_%c is only available on HEXAHEDRON",
                 Curl ? "Curl" : "", 'a' + Index - 1);
}

#define BF_EDGE_3V(X, I)                                                       \
  void BF_Edge_3V_##X(struct Element *Element, int NumEntity, double u,         \
                      double v, double w, double s[])                           \
  {                                                                            \
    BF_Edge_3V_Hex(Element, I, u, v, w, 0, s);                                  \
  }                                                                            \
  void BF_CurlEdge_3V_##X(struct Element *Element, int NumEntity, double u,     \
                          double v, double w, double s[])                       \
  {                                                                            \
    BF_Edge_3V_Hex(Element, I, u, v, w, 1, s);                                  \
  }

BF_EDGE_3V(a, 1)
BF_EDGE_3V(b, 2)
BF_EDGE_3V(c, 3)
BF_EDGE_3V(d, 4)
BF_EDGE_3V(e, 5)
BF_EDGE_3V(f, 6)

#undef BF_EDGE_3V

/* ------------------------------------------------------------------------ */
/*  B F _ C u r l E d g e _ 3                                               */
/* ------------------------------------------------------------------------ */

/* ------- */
/*  Edges  */
/* ------- */

#define WrongNumEntity Message::Error("Wrong Edge number in 'BF_CurlEdge_3E'")

void BF_CurlEdge_3E(struct Element *Element, int NumEntity, double u, double v,
                    double w, double s[])
{
  Message::Error("You should never end up here!");
}

#undef WrongNumEntity

/* -------- */
/*  Facets  */
/* -------- */

#define WrongNumEntity Message::Error("Wrong Face number in 'BF_CurlEdge_3F'")

void BF_CurlEdge_3F(struct Element *Element, int NumEntity, int Index, double u,
                    double v, double w, double s[])
{
  switch(Element->Type) {
  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4: Message::Error("You should never end up here!"); break;

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4:
    switch(NumEntity) {
    case 1:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 3:
        s[0] = 0.;
        s[1] = 0.;
        s[2] = -2.0 * u + 1.0 - v;
        break;
      case 1:
        s[0] = 0.;
        s[1] = 0.;
        s[2] = -v + u;
        break;
      case 2:
        s[0] = 0.;
        s[1] = 0.;
        s[2] = 2.0 * v - 1.0 + u;
        break;
      }
      break;
    default: WrongNumEntity;
    }
    break;

  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4:
    switch(NumEntity) {
    case 1:
      /* curl2d is taken in (xi, eta); d xi ^ d eta = det du ^ dv, with det
         = +-1 the orientation of the frame with respect to (u, v). */
      {
        double xi, eta, f_xi, f_eta, curl2d, xiDir[2], etaDir[2];
        if(!Quad_FacetFrame(Element, u, v, &xi, &eta, xiDir, etaDir) ||
           !Hex_FacetFunction(Index, xi, eta, &f_xi, &f_eta, &curl2d)) {
          s[0] = s[1] = s[2] = 0.;
          Message::Error("BF_CurlEdge_3F_%c not available on QUADRANGLE",
                         'a' + Index - 1);
        }
        else {
          s[0] = 0.;
          s[1] = 0.;
          s[2] = curl2d * (xiDir[0] * etaDir[1] - xiDir[1] * etaDir[0]);
        }
      }
      break;
    default: WrongNumEntity;
    }
    break;

  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4:
    switch(NumEntity) {
    case 1:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 4:
        s[0] = -u;
        s[1] = -1. + 2. * u + v + w;
        s[2] = 0.;
        break;
      case 1:
        s[0] = u;
        s[1] = -u + w;
        s[2] = -w;
        break;
      case 2:
        s[0] = 0.;
        s[1] = 1. - u - v - 2. * w;
        s[2] = w;
        break;
      }
      break;
    case 2:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 2:
        s[0] = 0.;
        s[1] = -v;
        s[2] = -1. + u + 2. * v + w;
        break;
      case 1:
        s[0] = -u;
        s[1] = v;
        s[2] = u - v;
        break;
      case 3:
        s[0] = u;
        s[1] = 0.;
        s[2] = 1. - 2. * u - v - w;
        break;
      }
      break;
    case 3:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 3:
        s[0] = -1. + u + v + 2. * w;
        s[1] = 0.;
        s[2] = -w;
        break;
      case 1:
        s[0] = v - w;
        s[1] = -v;
        s[2] = w;
        break;
      case 4:
        s[0] = 1. - u - 2. * v - w;
        s[1] = v;
        s[2] = 0.;
        break;
      }
      break;
    case 4:
      switch(Get_FacetFunctionIndex(Element, NumEntity, Index)) {
      case 4:
        s[0] = u;
        s[1] = -v;
        s[2] = 0.;
        break;
      case 2:
        s[0] = 0.;
        s[1] = v;
        s[2] = -w;
        break;
      case 3:
        s[0] = -u;
        s[1] = 0.;
        s[2] = w;
        break;
      }
      break;
    default: WrongNumEntity;
    }
    break;

  /* curl of the above, in the reference element.  With the frame right-handed
     by construction and F = g(z) [f_xi xiHat + f_eta etaHat], z = zetaHat . X,

       curl F = -g'(z) f_eta xiHat + g'(z) f_xi etaHat
                + g(z) (d_xi f_eta - d_eta f_xi) zetaHat ,

     whose zeta component is the 2D curl of the QUADRANGLE case. */
  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    if(NumEntity < 1 || NumEntity > 6) {
      s[0] = s[1] = s[2] = 0.;
      WrongNumEntity;
    }
    else {
      double xi, eta, g, dg, f_xi, f_eta, curl2d;
      double xiDir[3], etaDir[3], zetaDir[3];
      if(!Hex_FacetFrame(Element, NumEntity, u, v, w, &xi, &eta, &g, &dg,
                         xiDir, etaDir, zetaDir) ||
         !Hex_FacetFunction(Index, xi, eta, &f_xi, &f_eta, &curl2d)) {
        s[0] = s[1] = s[2] = 0.;
        Message::Error("BF_CurlEdge_3F_%c not available on HEXAHEDRON",
                       'a' + Index - 1);
      }
      else {
        for(int d = 0; d < 3; d++)
          s[d] = dg * (-f_eta * xiDir[d] + f_xi * etaDir[d]) +
                 g * curl2d * zetaDir[d];
      }
    }
    break;

  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4:
    if(!Pri_FacetFunction(Element, NumEntity, Index, u, v, w, 1, s)) {
      s[0] = s[1] = s[2] = 0.;
      Message::Error("BF_CurlEdge_3F_%c not available on facet %d of PRISM",
                     'a' + Index - 1, NumEntity);
    }
    break;

  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    if(!(NumEntity == PYR_BASE_FACET ?
           Pyr_BaseFunction(Element, NumEntity, Index, u, v, w, 1, s) :
           Pyr_TriangleFunction(Element, NumEntity, Index, u, v, w, 1, s))) {
      s[0] = s[1] = s[2] = 0.;
      Message::Error("BF_CurlEdge_3F_%c not available on facet %d of PYRAMID",
                     'a' + Index - 1, NumEntity);
    }
    break;

  default: Message::Error("Unknown type of Element in BF_CurlEdge_3F"); break;
  }
}

#undef WrongNumEntity

void BF_CurlEdge_3F_a(struct Element *Element, int NumEntity, double u,
                      double v, double w, double s[])
{
  BF_CurlEdge_3F(Element, NumEntity, 1, u, v, w, s);
}

void BF_CurlEdge_3F_b(struct Element *Element, int NumEntity, double u,
                      double v, double w, double s[])
{
  BF_CurlEdge_3F(Element, NumEntity, 2, u, v, w, s);
}

void BF_CurlEdge_3F_c(struct Element *Element, int NumEntity, double u,
                      double v, double w, double s[])
{
  BF_CurlEdge_3F(Element, NumEntity, 3, u, v, w, s);
}

void BF_CurlEdge_3F_d(struct Element *Element, int NumEntity, double u,
                      double v, double w, double s[])
{
  BF_CurlEdge_3F(Element, NumEntity, 4, u, v, w, s);
}

/* -------- */
/*  Volume  */
/* -------- */

void BF_CurlEdge_3V(struct Element *Element, int NumEntity, double u, double v,
                    double w, double s[])
{
  Message::Error("You should never end up here!");
}
