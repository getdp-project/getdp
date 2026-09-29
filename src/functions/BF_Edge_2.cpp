// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include "ProData.h"
#include "Message.h"

/* ------------------------------------------------------------------------ */
/*  B F _ E d g e _ 2                                                       */
/* ------------------------------------------------------------------------ */

/* ------- */
/*  Edges  */
/* ------- */

#define WrongNumEntity Message::Error("Wrong Edge number in 'BF_Edge_2E'")

/* WARNING: the element cases below are NOT one family.  Along an edge, with xi
   in [-1,1] its reference coordinate, the tangential trace per d xi is

       LINE                     xi
       TRIANGLE, TETRAHEDRON    -xi/2     = grad(N_a N_b)
       QUADRANGLE, HEXAHEDRON   3 xi/2    = 3 xi_e w_e

   and PRISM and PYRAMID have no case at all.  So BF_Edge_2E is NOT H(curl)
   conforming on a mesh that mixes simplices with quadrangles or hexahedra --
   including hexahedra bridged to tetrahedra by pyramids -- nor between a
   triangle and a line carrying its trace.

   On such meshes use BF_GradNode_2E in the Form1 space instead, on ALL the
   elements in a single BasisFunction entry (on simplices it is the very same
   function as BF_Edge_2E).  It is grad(N_a N_b) on every element type, which
   is conforming.  Two things to know:
     - it is a pure gradient: it adds nothing to the curl, and wherever only
       the curl is assembled (no conductivity) its dofs must be gauged away,
       i.e. constrained to zero -- a tree on the Whitney dofs does not do it;
     - on quadrangles and hexahedra, together with BF_Edge_3F_a..d (and
       BF_Edge_3V_a..f), it spans the same second order space as 3 xi_e w_e.
       Without the facet functions, 3 xi_e w_e also carries some curl, which
       grad(N_a N_b) does not.

   Making BF_Edge_2E itself grad(N_a N_b) everywhere would fix the family, but
   it changes the upstream QUADRANGLE and LINE functions (and so BF_CurlEdge_2E,
   BF_GroupOfEdges_2E and BF_PerpendicularFacet_2E); it has not been done. */

void BF_Edge_2E(struct Element *Element, int NumEntity, double u, double v,
                double w, double s[])
{
  switch(Element->Type) {
  case LINE:
  case LINE_2:
  case LINE_3:
  case LINE_4:
    switch(NumEntity) {
    case 1:
      s[0] = u;
      s[1] = 0.;
      s[2] = 0.;
      break;
    default: WrongNumEntity;
    }
    break;

  case TRIANGLE:
  case TRIANGLE_2:
  case TRIANGLE_3:
  case TRIANGLE_4:
    switch(NumEntity) {
    case 1:
      s[0] = -2.0 * u + 1.0 - v;
      s[1] = -u;
      s[2] = 0.;
      break;
    case 2:
      s[0] = -v;
      s[1] = -2.0 * v + 1.0 - u;
      s[2] = 0.;
      break;
    case 3:
      s[0] = v;
      s[1] = u;
      s[2] = 0.;
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
      s[0] = 0.75 * (1.0 - v) * u;
      s[1] = 0.;
      s[2] = 0.;
      break;
    case 2:
      s[0] = 0.;
      s[1] = 0.75 * (1.0 - u) * v;
      s[2] = 0.;
      break;
    case 3:
      s[0] = 0.;
      s[1] = 0.75 * (1.0 + u) * v;
      s[2] = 0.;
      break;
    case 4:
      s[0] = 0.75 * (1.0 + v) * u;
      s[1] = 0.;
      s[2] = 0.;
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
      s[0] = -2.0 * u + 1.0 - v - w;
      s[1] = -u;
      s[2] = -u;
      break;
    case 2:
      s[0] = -v;
      s[1] = -2.0 * v + 1.0 - u - w;
      s[2] = -v;
      break;
    case 3:
      s[0] = -w;
      s[1] = -w;
      s[2] = -2.0 * w + 1.0 - u - v;
      break;
    case 4:
      s[0] = v;
      s[1] = u;
      s[2] = 0.;
      break;
    case 5:
      s[0] = w;
      s[1] = 0.;
      s[2] = u;
      break;
    case 6:
      s[0] = 0.;
      s[1] = w;
      s[2] = v;
      break;
    default: WrongNumEntity;
    }
    break;

  /* The hexahedron follows the QUADRANGLE above, not the simplices: the
     order 2 edge function is the Whitney function of the edge times three
     times the coordinate ALONG that edge,

         BF_Edge_2E = 3 xi_e w_e ,

     whose trace on a face of the hexahedron is exactly the QUADRANGLE function
     of the same edge (checked: the trace of case 1 on w = -1 is the
     quadrangle's case 1), so hexahedra and quadrangles are conforming with
     each other.  With simplices they are NOT: see the WARNING above, and use
     BF_GradNode_2E on hybrid meshes.  Its curl is not zero, so BF_CurlEdge_2E
     below has a hexahedron case of its own.

     xi_e and w_e both change sign when the edge is traversed the other way, so
     the product does not: the function is orientation free, as the Orient = 0
     of its entry in BF_Function requires. */
  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    switch(NumEntity) {
    case 1:
      s[0] = 0.375 * u * (1. - v) * (1. - w);
      s[1] = 0.;
      s[2] = 0.;
      break;
    case 2:
      s[0] = 0.;
      s[1] = 0.375 * v * (1. - u) * (1. - w);
      s[2] = 0.;
      break;
    case 3:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = 0.375 * w * (1. - u) * (1. - v);
      break;
    case 4:
      s[0] = 0.;
      s[1] = 0.375 * v * (1. + u) * (1. - w);
      s[2] = 0.;
      break;
    case 5:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = 0.375 * w * (1. + u) * (1. - v);
      break;
    case 6:
      s[0] = 0.375 * u * (1. + v) * (1. - w);
      s[1] = 0.;
      s[2] = 0.;
      break;
    case 7:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = 0.375 * w * (1. + u) * (1. + v);
      break;
    case 8:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = 0.375 * w * (1. - u) * (1. + v);
      break;
    case 9:
      s[0] = 0.375 * u * (1. - v) * (1. + w);
      s[1] = 0.;
      s[2] = 0.;
      break;
    case 10:
      s[0] = 0.;
      s[1] = 0.375 * v * (1. - u) * (1. + w);
      s[2] = 0.;
      break;
    case 11:
      s[0] = 0.;
      s[1] = 0.375 * v * (1. + u) * (1. + w);
      s[2] = 0.;
      break;
    case 12:
      s[0] = 0.375 * u * (1. + v) * (1. + w);
      s[1] = 0.;
      s[2] = 0.;
      break;
    default: WrongNumEntity;
    }
    break;

  /* Not implemented.  s[] is zeroed rather than left alone: Message::Error does
     not stop GetDP here, so an untouched s[] would hand the assembly whatever
     was on the stack -- which is how a hexahedral conductor bridged to a
     tetrahedral air by PYRAMIDS (gmsh puts them there by itself) came out with
     twice the right inductance instead of stopping. */
  case PRISM:
  case PRISM_2:
  case PRISM_2_15N:
  case PRISM_3:
  case PRISM_4:
    s[0] = s[1] = s[2] = 0.;
    Message::Error("BF_Edge_2E not ready for PRISM");
    break;

  case PYRAMID:
  case PYRAMID_2:
  case PYRAMID_2_13N:
  case PYRAMID_3: // case PYRAMID_4
    s[0] = s[1] = s[2] = 0.;
    Message::Error("BF_Edge_2E not ready for PYRAMID");
    break;

  default:
    s[0] = s[1] = s[2] = 0.;
    Message::Error("Unknown type of Element in BF_Edge_2E");
    break;
  }
}

#undef WrongNumEntity

/* ------- */
/*  Faces  */
/* ------- */

#define WrongNumEntity Message::Error("Wrong Face number in 'BF_Edge_2F'")

void BF_Edge_2F(struct Element *Element, int NumEntity, double u, double v,
                double w, double s[])
{
  Message::Error("You should never end up here!");
}

#undef WrongNumEntity

/* -------- */
/*  Volume  */
/* -------- */

void BF_Edge_2V(struct Element *Element, int NumEntity, double u, double v,
                double w, double s[])
{
  Message::Error("You should never end up here!");
}

/* ------------------------------------------------------------------------ */
/*  B F _ C u r l E d g e _ 2                                               */
/* ------------------------------------------------------------------------ */

/* ------- */
/*  Edges  */
/* ------- */

#define WrongNumEntity Message::Error("Wrong Edge number in 'BF_CurlEdge_2E'")

void BF_CurlEdge_2E(struct Element *Element, int NumEntity, double u, double v,
                    double w, double s[])
{
  switch(Element->Type) {
  case QUADRANGLE:
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
  case QUADRANGLE_3:
  case QUADRANGLE_4:
    switch(NumEntity) {
    case 1:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = 0.75 * u;
      break;
    case 2:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = - 0.75 * v;
      break;
    case 3:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = 0.75 * v;
      break;
    case 4:
      s[0] = 0.;
      s[1] = 0.;
      s[2] = - 0.75 * u;
      break;
    default: WrongNumEntity;
    }
    break;
  /* Curl of the hexahedron functions added to BF_Edge_2E above.  Each of them
     has a single non-zero component f along its own axis, so the curl is
     grad(f) x e_axis; checked against a numerical curl of BF_Edge_2E. */
  case HEXAHEDRON:
  case HEXAHEDRON_2:
  case HEXAHEDRON_2_20N:
  case HEXAHEDRON_3:
  case HEXAHEDRON_4:
    switch(NumEntity) {
    case 1:
      s[0] = 0.;
      s[1] = -0.375 * u * (1. - v);
      s[2] = 0.375 * u * (1. - w);
      break;
    case 2:
      s[0] = 0.375 * v * (1. - u);
      s[1] = 0.;
      s[2] = -0.375 * v * (1. - w);
      break;
    case 3:
      s[0] = -0.375 * w * (1. - u);
      s[1] = 0.375 * w * (1. - v);
      s[2] = 0.;
      break;
    case 4:
      s[0] = 0.375 * v * (1. + u);
      s[1] = 0.;
      s[2] = 0.375 * v * (1. - w);
      break;
    case 5:
      s[0] = -0.375 * w * (1. + u);
      s[1] = -0.375 * w * (1. - v);
      s[2] = 0.;
      break;
    case 6:
      s[0] = 0.;
      s[1] = -0.375 * u * (1. + v);
      s[2] = -0.375 * u * (1. - w);
      break;
    case 7:
      s[0] = 0.375 * w * (1. + u);
      s[1] = -0.375 * w * (1. + v);
      s[2] = 0.;
      break;
    case 8:
      s[0] = 0.375 * w * (1. - u);
      s[1] = 0.375 * w * (1. + v);
      s[2] = 0.;
      break;
    case 9:
      s[0] = 0.;
      s[1] = 0.375 * u * (1. - v);
      s[2] = 0.375 * u * (1. + w);
      break;
    case 10:
      s[0] = -0.375 * v * (1. - u);
      s[1] = 0.;
      s[2] = -0.375 * v * (1. + w);
      break;
    case 11:
      s[0] = -0.375 * v * (1. + u);
      s[1] = 0.;
      s[2] = 0.375 * v * (1. + w);
      break;
    case 12:
      s[0] = 0.;
      s[1] = 0.375 * u * (1. + v);
      s[2] = -0.375 * u * (1. + w);
      break;
    default: WrongNumEntity;
    }
    break;

  default: // for all other defined element types
    s[0] = 0.;
    s[1] = 0.;
    s[2] = 0.;
  }
}

#undef WrongNumEntity

/* ------- */
/*  Faces  */
/* ------- */

void BF_CurlEdge_2F(struct Element *Element, int NumEntity, double u, double v,
                    double w, double s[])
{
  s[0] = 0.;
  s[1] = 0.;
  s[2] = 0.;
}

/* -------- */
/*  Volume  */
/* -------- */

void BF_CurlEdge_2V(struct Element *Element, int NumEntity, double u, double v,
                    double w, double s[])
{
  s[0] = 0.;
  s[1] = 0.;
  s[2] = 0.;
}
