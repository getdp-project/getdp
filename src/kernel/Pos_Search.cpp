// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//   Jean-Francois Remacle
//

#include <math.h>
#include <vector>
#include "ProData.h"
#include "GeoData.h"
#include "Get_Geometry.h"
#include "Pos_Search.h"
#include "BoxTree.h"
#include "Get_DofOfElement.h"
#include "DofData.h"
#include "Message.h"

#define SQU(a) ((a) * (a))

extern struct CurrentData Current;

static struct Geo_Element *LastGeoElement;

// when all the ranks look for the same point, each one in the elements of its
// mesh partition
static bool InPartitionOfThisRank = false;
void Pos_SearchInPartitionOfThisRank(bool own) { InPartitionOfThisRank = own; }

/* ------------------------------------------------------------------------ */
/*  C o m p u t e E l e m e n t B o x                                       */
/* ------------------------------------------------------------------------ */

// Returns by how much the box of the nodes was made bigger in each direction.
static double ComputeElementBox(struct Element *Element,
                                struct ElementBox *ElementBox)
{
  int i;

  ElementBox->Xmin = ElementBox->Xmax = Element->x[0];
  ElementBox->Ymin = ElementBox->Ymax = Element->y[0];
  ElementBox->Zmin = ElementBox->Zmax = Element->z[0];
  for(i = 1; i < Element->GeoElement->NbrNodes; i++) {
    ElementBox->Xmin = std::min(ElementBox->Xmin, Element->x[i]);
    ElementBox->Xmax = std::max(ElementBox->Xmax, Element->x[i]);
    ElementBox->Ymin = std::min(ElementBox->Ymin, Element->y[i]);
    ElementBox->Ymax = std::max(ElementBox->Ymax, Element->y[i]);
    ElementBox->Zmin = std::min(ElementBox->Zmin, Element->z[i]);
    ElementBox->Zmax = std::max(ElementBox->Zmax, Element->z[i]);
  }
  double x = ElementBox->Xmax - ElementBox->Xmin;
  double y = ElementBox->Ymax - ElementBox->Ymin;
  double z = ElementBox->Zmax - ElementBox->Zmin;
  double diag = sqrt(x * x + y * y + z * z);
  // make box 1% bigger
  const double c = 0.01 * diag;
  ElementBox->Xmin -= c;
  ElementBox->Xmax += c;
  ElementBox->Ymin -= c;
  ElementBox->Ymax += c;
  ElementBox->Zmin -= c;
  ElementBox->Zmax += c;
  return c;
}

/* ------------------------------------------------------------------------ */
/*  P o i n t I n X X X                                                     */
/* ------------------------------------------------------------------------ */

static int PointInElementBox(struct ElementBox ElementBox, double x, double y,
                             double z)
{
  if(x > ElementBox.Xmax || x < ElementBox.Xmin ||
     y > ElementBox.Ymax || y < ElementBox.Ymin ||
     z > ElementBox.Zmax || z < ElementBox.Zmin) {
    return (0);
  }
  else {
    return (1);
  }
}

// A point is in an element if it is not further than tol outside of it, in the
// coordinates of the reference element (with a negative tol: at least that far
// inside).
static int PointInRefElement(struct Element *Element, double u, double v,
                             double w, double tol = 1.e-6)
{
  double ONE = 1. + tol;
  double ZERO = tol;

  switch(Element->Type) {
  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4:
    if(u < -ONE || u > ONE) { return (0); }
    return (1);
  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4:
    if(u < -ZERO || v < -ZERO || u > (ONE - v)) { return (0); }
    return (1);
  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4:
    if(u < -ONE || v < -ONE || u > ONE || v > ONE) { return (0); }
    return (1);
  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4:
    if(u < -ZERO || v < -ZERO || w < -ZERO || u > (ONE - v - w)) { return (0); }
    return (1);
  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    if(u < -ONE || v < -ONE || w < -ONE || u > ONE || v > ONE || w > ONE) {
      return (0);
    }
    return (1);
  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4:
    if(w > ONE || w < -ONE || u < -ZERO || v < -ZERO || u > (ONE - v)) {
      return (0);
    }
    return (1);
  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4 :
    if(u < (w - ONE) || u > (ONE - w) || v < (w - ONE) || v > (ONE - w) ||
       w < -ZERO || w > ONE) {
      return (0);
    }
    return (1);
  default: return (0);
  }
}

int PointInElement(struct Element *Element, List_T *ExcludeRegion_L, double x,
                   double y, double z, double *u, double *v, double *w)
{
  struct ElementBox ElementBox;

  if(InPartitionOfThisRank &&
     !Dof_IsPartitionOfThisRank(Element->GeoElement->Partition))
    return (0);

  if(ExcludeRegion_L)
    if(List_Search(ExcludeRegion_L, &Element->GeoElement->Region, fcmp_int)) {
      return (0);
    }

  Element->Num = Element->GeoElement->Num;
  Element->Type = Element->GeoElement->Type;
  Element->Region = Element->GeoElement->Region;

  Get_NodesCoordinatesOfElement(Element);
  double c = ComputeElementBox(Element, &ElementBox);

  if(!PointInElementBox(ElementBox, x, y, z)) {
    return (0);
  }

  xyz2uvwInAnElement(Element, x, y, z, u, v, w);

  if(!PointInRefElement(Element, *u, *v, *w)) {
    // Message::Info("Point was in box, but not in actual element");
    return (0);
  }

  // (u, v, w) is the point of a surface or of a curve that is the closest to
  // (x, y, z), which can be far from it: accept the same distance as the box
  // does for a surface or a curve along the axes
  if(DimensionOfElementType(Element->Type) + 1 < Current.GeoData->Dimension) {
    Get_BFGeoElement(Element, *u, *v, *w);
    double d[3] = {x, y, z};
    for(int i = 0; i < Element->GeoElement->NbrNodes; i++) {
      d[0] -= Element->x[i] * Element->n[i];
      d[1] -= Element->y[i] * Element->n[i];
      d[2] -= Element->z[i] * Element->n[i];
    }
    if(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > c * c) return (0);
  }

  return (1);
}

/* ------------------------------------------------------------------------ */
/*  E l e m e n t S e a r c h                                               */
/* ------------------------------------------------------------------------ */

// The elements of a mesh and the tree of their bounding boxes. The boxes are
// numbered in the order the elements must be tested: the volumes, then the
// surfaces, then the curves, each in the order of the mesh.

struct ElementSearch {
  BoxTree Tree;
  std::vector<struct Geo_Element *> Elements;
  int NbrOfDim[3]; // number of curves, surfaces and volumes
};

// 0 for a curve, 1 for a surface, 2 for a volume, -1 otherwise
int DimensionOfElementType(int Type)
{
  switch(Type) {
  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4: return 0;
  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4:
  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4: return 1;
  case TETRAHEDRON:
  case TETRAHEDRON_2:
  case TETRAHEDRON_3:
  case TETRAHEDRON_4:
  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4:
  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4 :
    return 2;
  default: return -1;
  }
}

static void Init_ElementSearch(struct GeoData *GeoData)
{
  LastGeoElement = NULL;

  Message::Info("Building the search tree of the elements...");

  struct ElementSearch *S = GeoData->ElementSearch = new ElementSearch;
  struct Element Element;
  Get_InitDofOfElement(&Element);
  int NbrGeoElements = Geo_GetNbrGeoElements();
  int Region = Current.Region;

  for(int dim = 2; dim >= 0; dim--) {
    S->NbrOfDim[dim] = 0;
    for(int iElm = 0; iElm < NbrGeoElements; iElm++) {
      Element.GeoElement = Geo_GetGeoElement(iElm);
      Element.Num = Element.GeoElement->Num;
      Element.Type = Element.GeoElement->Type;
      Current.Region = Element.Region = Element.GeoElement->Region;
      if(!Element.Region || DimensionOfElementType(Element.Type) != dim)
        continue;
      struct ElementBox Box;
      Get_NodesCoordinatesOfElement(&Element);
      ComputeElementBox(&Element, &Box);
      double min[3] = {Box.Xmin, Box.Ymin, Box.Zmin};
      double max[3] = {Box.Xmax, Box.Ymax, Box.Zmax};
      S->Tree.add(min, max);
      S->Elements.push_back(Element.GeoElement);
      S->NbrOfDim[dim]++;
    }
  }
  S->Tree.build();
  Current.Region = Region;

  Message::Info("...done: %d elements", S->Tree.size());
}

void Free_ElementSearch(struct GeoData *GeoData)
{
  LastGeoElement = NULL;
  if(!GeoData->ElementSearch) return;
  delete GeoData->ElementSearch;
  GeoData->ElementSearch = NULL;
}

/* ------------------------------------------------------------------------ */
/*  I n W h i c h E l e m e n t                                             */
/* ------------------------------------------------------------------------ */

// Find the element of the mesh that contains the point (x, y, z), among the
// elements of dimension Dim (DIM_1D, DIM_2D, DIM_3D, or DIM_ALL: a volume if
// there is one, otherwise a surface, otherwise a curve) that are not in
// ExcludeRegion_L. On return the element and the coordinates (u, v, w) of the
// point in it are set, or Element->Num is NO_ELEMENT if there is none.

void InWhichElement(struct GeoData *GeoData, List_T *ExcludeRegion_L,
                    struct Element *Element, int Dim, double x, double y,
                    double z, double *u, double *v, double *w)
{
  /* Note: Il est garanti en sortie que les fcts de forme geometriques sont
     initialisees en u,v,w */

  if(!GeoData->ElementSearch) Init_ElementSearch(GeoData);
  struct ElementSearch *S = GeoData->ElementSearch;

  // the boxes are numbered by decreasing dimension: those of the dimension
  // we look for have the numbers first, ..., last - 1; top is the dimension
  // of the elements that are tested first
  int first = 0, last = S->Tree.size();
  int top = S->NbrOfDim[2] ? 2 : S->NbrOfDim[1] ? 1 : 0;
  switch(Dim) {
  case DIM_3D:
    last = S->NbrOfDim[2];
    top = 2;
    break;
  case DIM_2D:
    first = S->NbrOfDim[2];
    last = first + S->NbrOfDim[1];
    top = 1;
    break;
  case DIM_1D:
    first = S->NbrOfDim[2] + S->NbrOfDim[1];
    top = 0;
    break;
  default: break;
  }

  // Successive points are often in the same element. A surface is not tried
  // first if there are volumes, since it would hide the volume of the point.
  // A point close to the boundary of the element is also within the tolerance
  // of its neighbours: the search below then gives the one with the lowest
  // number, whatever the previous point and the mesh partition were.
  if(LastGeoElement && DimensionOfElementType(LastGeoElement->Type) == top) {
    Element->GeoElement = LastGeoElement;
    if(PointInElement(Element, ExcludeRegion_L, x, y, z, u, v, w) &&
       PointInRefElement(Element, *u, *v, *w, -1.e-3)) {
      return;
    }
  }

  // (the boxes are found in increasing order of their numbers, i.e. of the
  // numbers of the elements of each dimension)
  static std::vector<int> found;
  S->Tree.search(x, y, z, found);
  for(std::size_t i = 0; i < found.size(); i++) {
    if(found[i] < first || found[i] >= last) continue;
    Element->GeoElement = S->Elements[found[i]];
    if(PointInElement(Element, ExcludeRegion_L, x, y, z, u, v, w)) {
      LastGeoElement = Element->GeoElement;
      return;
    }
  }

  Element->Num = NO_ELEMENT;
  Element->Region = NO_REGION;
}

/* ------------------------------------------------------------------------ */
/*  x y z 2 u v w I n A n E l e m e n t                                     */
/* ------------------------------------------------------------------------ */

#define NR_PRECISION 1.e-6 /* a comparer a l'intervalle de variation de uvw */
#define NR_MAX_ITER 50

void xyz2uvwInAnElement(struct Element *Element, double x, double y, double z,
                        double *u, double *v, double *w)
{
  double x_est, y_est, z_est;
  double u_new, v_new, w_new;
  double Error = 1.0;
  int i, iter = 1;
  int ChainDim = DIM_3D, Type_Dimension, Type_Jacobian;
  double (*Get_Jacobian)(struct Element *, MATRIX3x3 *);

  *u = *v = *w = 0.0;

  if(Element->Type &
     (TETRAHEDRON | TETRAHEDRON_2 | TETRAHEDRON_3 | TETRAHEDRON_4 | HEXAHEDRON |
      HEXAHEDRON_2 | HEXAHEDRON_2_20N | HEXAHEDRON_3 | HEXAHEDRON_4 | PRISM |
      PRISM_2 | PRISM_2_15N | PRISM_3 | PRISM_4 | PYRAMID | PYRAMID_2 |
      PYRAMID_2_13N | PYRAMID_3 /*|PYRAMID_4*/))
    ChainDim = DIM_3D;
  else if(Element->Type &
          (TRIANGLE | TRIANGLE_2 | TRIANGLE_3 | TRIANGLE_4 | QUADRANGLE |
           QUADRANGLE_2 | QUADRANGLE_2_8N | QUADRANGLE_3 | QUADRANGLE_4))
    ChainDim = DIM_2D;
  else if(Element->Type & (LINE | LINE_2 | LINE_3 | LINE_4))
    ChainDim = DIM_1D;
  else if(Element->Type & POINT_ELEMENT)
    ChainDim = DIM_0D;
  else {
    Message::Error("Unknown type of element in xyz2uvwInAnElement");
    return;
  }

  if(ChainDim == DIM_1D && Current.GeoData->Dimension == DIM_3D)
    Type_Jacobian = JACOBIAN_LIN;
  else if((ChainDim == DIM_1D && Current.GeoData->Dimension == DIM_2D) ||
          (ChainDim == DIM_2D && Current.GeoData->Dimension == DIM_3D))
    Type_Jacobian = JACOBIAN_SUR;
  else
    Type_Jacobian = JACOBIAN_VOL;

  while(Error > NR_PRECISION && iter < NR_MAX_ITER) {
    iter++;

    Get_BFGeoElement(Element, *u, *v, *w);
    Get_Jacobian =
      (double (*)(struct Element *, MATRIX3x3 *))Get_JacobianFunction(
        Type_Jacobian, Element->Type, &Type_Dimension);

    Element->DetJac = Get_Jacobian(Element, &Element->Jac);

    if(Element->DetJac != 0) {
      Get_InverseMatrix(Type_Dimension, Element->Type, Element->DetJac,
                        &Element->Jac, &Element->InvJac);

      x_est = y_est = z_est = 0.;
      for(i = 0; i < Element->GeoElement->NbrNodes; i++) {
        x_est += Element->x[i] * Element->n[i];
        y_est += Element->y[i] * Element->n[i];
        z_est += Element->z[i] * Element->n[i];
      }

      u_new = *u + Element->InvJac.c11 * (x - x_est) +
              Element->InvJac.c21 * (y - y_est) +
              Element->InvJac.c31 * (z - z_est);
      v_new = *v + Element->InvJac.c12 * (x - x_est) +
              Element->InvJac.c22 * (y - y_est) +
              Element->InvJac.c32 * (z - z_est);
      w_new = *w + Element->InvJac.c13 * (x - x_est) +
              Element->InvJac.c23 * (y - y_est) +
              Element->InvJac.c33 * (z - z_est);

      // a surface or a curve only has the coordinates u, v or u: what the
      // others get is the distance to the element, which does not converge
      if(Type_Jacobian != JACOBIAN_VOL) {
        w_new = 0.;
        if(ChainDim == DIM_1D) v_new = 0.;
      }

      Error = SQU(u_new - *u) + SQU(v_new - *v) + SQU(w_new - *w);

      *u = u_new;
      *v = v_new;
      *w = w_new;
    }
    else {
      Message::Warning("Zero determinant in 'xyz2uvwInAnElement'");
      break;
    }
  }

  if(iter == NR_MAX_ITER)
    Message::Warning(
      "Maximum number of iterations exceeded in xyz2uvwInAnElement");

#if 0
  Message::Info("%d iterations in xyz2uvw", iter);
#endif
}
