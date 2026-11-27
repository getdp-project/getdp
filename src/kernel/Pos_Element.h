// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef POS_ELEMENT_H
#define POS_ELEMENT_H

#include <vector>
#include <utility>
#include "ProData.h"
#include "ListUtils.h"

/* ------------------------------------------------------------------------ */
/*  P o s t E l e m e n t                                                   */
/* ------------------------------------------------------------------------ */

struct PostElement {
  // Index is the position of the element in the mesh of the rank; NumElement
  // is the number of the element if it is not in this mesh (0 otherwise)
  int Index, NumElement, Type, Depth;
  int NbrNodes, *NumNodes;
  double *u, *v, *w, *x, *y, *z;
  struct Value *Value;
};

// The Lagrange element of a given order on an element of the mesh, for the
// export of the fields on a mesh of that order made on the fly (Print with
// Order): its type in Gmsh, its nodes in the reference element, the geometric
// shape functions of the element of the mesh at these nodes (NumNodes x
// NbrNodes of the element), and, for each node, the vertices of the element
// (first NbrVertices nodes) on which it depends, with integer weights: a node
// on a vertex, an edge or a face is identified by the vertices and the weights,
// whatever the element.
struct LagrangePostElement {
  int GmshType, Dimension, NbrNodes, NbrVertices;
  std::vector<double> u, v, w, n;
  std::vector<std::vector<std::pair<int, int> > > Vertices;
  // (the nodes as the points of a quadrature, see Get_LagrangePostPoints)
  struct Quadrature Points;
};

// (NULL if there is none, e.g. without Gmsh)
const struct LagrangePostElement *
Get_LagrangePostElement(struct Geo_Element *GE, int Order);

// The nodes of a Lagrange element as a set of points, for
// Cal_SetReferencePoint: to be asked for each element that uses them
struct Quadrature *Get_LagrangePostPoints(const struct LagrangePostElement *L);

struct PostElement *Create_PostElement(int Index, int Type, int NbrNodes,
                                       int Depth);
void Destroy_PostElement(struct PostElement *PostElement);
struct PostElement *NodeCopy_PostElement(struct PostElement *PostElement);
struct PostElement *PartialCopy_PostElement(struct PostElement *PostElement);

void Fill_PostElement(struct Geo_Element *GE, List_T *PostElement_L, int Index,
                      int Depth, int Skin, int DecomposeInSimplex,
                      int HighOrder, int Gauss, int ExportOrder = 0);
void Cut_PostElement(struct PostElement *PE, struct Geo_Element *GE,
                     List_T *PE_L, int Index, int Depth, int Skin,
                     int DecomposeInSimplex);
void Sort_PostElement_Connectivity(List_T *PostElement_L);

int fcmp_PostElement(const void *a, const void *b);
int fcmp_PostElement_v0(const void *a, const void *b);
int fcmp_PostElement_absu0(const void *a, const void *b);

#endif
