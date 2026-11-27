// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

// Legendre basis functions: hierarchical functions of arbitrary order of H1,
// H(curl), H(div) and L2, computed by Gmsh (the "H1Legendre", "HcurlLegendre",
// "HdivLegendre" and "L2Legendre" function spaces of its HierarchicalBasis
// module), on lines, triangles, quadrangles, tetrahedra, hexahedra and prisms.
//
// The functions of a space of order p are given by the name of the space and
// its order, on the entities of one type, or on all of them with Entity Auto:
//
//   { Name s; NameOfCoef u; Function BF_HcurlLegendre; Order p;
//     Support Omega; Entity Auto; }
//
// with BF_H1Legendre (p >= 1), BF_HcurlLegendre, BF_HdivLegendre and
// BF_L2Legendre (p >= 0), and their exterior derivatives BF_GradH1Legendre,
// BF_CurlHcurlLegendre and BF_DivHdivLegendre. Entity NodesOf[...],
// EdgesOf[...], FacetsOf[...] or VolumesOf[...] gives the functions of the
// space borne by these entities only, so that the order can differ from one
// type of entity or from one region to the other. A constraint on the
// coefficient (NameOfCoef u) applies to all the functions, each on its type
// of entity; the value of an Assign or Init constraint is given to the
// functions of lowest order (the nodal functions of H1, the Whitney functions
// of H(curl) and H(div)) and the other functions are set to zero.
//
// A basis function of GetDP is one function per entity (node, edge, facet or
// volume), whereas the number of functions per entity grows with the order:
// such a basis function stands for one basis function per function of an
// entity, named s, s_E1, s_F0, ..., with the coefficients u, u_E1, u_F0, ...
// (the first one keeps the given names). These functions can also be given one
// by one, by the type of their entity and their position among the functions
// of that entity in Gmsh, which does not depend on the order of the space:
//
//   BF_H1Legendre_N0       function of a node          (Entity NodesOf)
//   BF_H1Legendre_E<k>     k-th function of an edge    (Entity EdgesOf)
//   BF_H1Legendre_F<k>     k-th function of a facet    (Entity FacetsOf)
//   BF_H1Legendre_V<k>     k-th function of a volume   (Entity VolumesOf)
//   BF_HcurlLegendre_E<k>, BF_HcurlLegendre_F<k>, BF_HcurlLegendre_V<k>
//   BF_HdivLegendre_F<k>, BF_HdivLegendre_V<k>
//   BF_L2Legendre_V<k>     k-th function of an element (Entity VolumesOf)
//
// (or BF_H1Legendre_E_<k>, etc., as given by BF_H1Legendre_E~{k} in a loop).
// The triangular and quadrangular facets do not bear the same number of
// functions for a given order, and neither do the elements of different
// shapes: in a space of a given order, the function at a given position is
// only borne by the entities that have it at that order. Given one by one,
// the functions can be restricted to the entities of one shape, with FTri<k>
// and FQua<k> for the facets, and VTet<k>, VHex<k> and VPri<k> for the volumes
// (and VLin<k>, VTri<k> and VQua<k> for the elements of L2), e.g.
// BF_L2Legendre_VTet<k>: this is only needed for a space whose order differs
// from one shape to the other (e.g. the divergence of the functions of H(div)
// of order p, of order p - 1 on the tetrahedra and p on the other shapes).
//
// The functions of an entity are sorted by increasing order: the space of
// order p is made of the n first functions of each entity, with n given by
//
//                      H1                    H(curl)              H(div)
//   edge               p - 1                 p + 1                -
//   triangle           (p-2)(p-1)/2          (p-1)(p+1)           (p+1)(p+2)/2
//   quadrangle         (p-1)^2               2p(p+1)              (p+1)^2
//   tetrahedron        (p-3)(p-2)(p-1)/6     (p-2)(p-1)(p+1)/2    (p-1)(p+1)(p+2)/2
//   hexahedron         (p-1)^3               3p^2(p+1)            3p(p+1)^2
//   prism              (p-2)(p-1)^2/2        3(p-1)p(p+1)/2       p(p+1)(3p+4)/2
//
// with one function per node in H1, and for
// L2 p+1, (p+1)(p+2)/2, (p+1)^2, (p+1)(p+2)(p+3)/6, (p+1)^3 and (p+1)^2(p+2)/2
// functions on a line, a triangle, a quadrangle, a tetrahedron, a hexahedron
// and a prism. In H(curl), E0 is the Whitney function (BF_Edge) and on the
// simplices Ek, k > 0, is the gradient of order k; in H(div), F0 is the Whitney
// function (BF_Facet).
//
// The interior functions of a triangle or a quadrangle are facet functions,
// and those of a line edge functions, so that the functions of H1 and H(curl)
// on the lines, triangles and quadrangles are the traces of the functions of
// the elements they bound. The functions of H(div) are those of the volumes,
// and their normal traces on the triangles and quadrangles (a normal vector in
// the reference element, as BF_Facet): there are no functions of H(div) in the
// plane.
//
// Gmsh gives the functions on the reference element, and those of the facets
// for each orientation of a facet; the orientation of the edges and facets of
// an element is applied here, as in Gmsh, from the tags of the nodes.

#include <algorithm>
#include <array>
#include <limits.h>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <stdlib.h>
#include <string.h>
#include "GetDPConfig.h"
#include "ProData.h"
#include "BF.h"
#include "Message.h"

#if defined(HAVE_KERNEL)
#include "GeoData.h"
#endif

#if defined(HAVE_GMSH_HIERARCHICAL) && defined(HAVE_KERNEL)

#define BF_ARG                                                                 \
  struct Element *Element, int NumEntity, double u, double v, double w,        \
    double Value[]

#include <gmsh/GmshDefines.h>
#include "HierarchicalBasis.h"

// the number of different functions that can be used in a problem
#define MAX_FUNCTIONS 4096

// the highest order of the function spaces of Gmsh
#define MAX_ORDER 40

// the points kept in the cache of each family of elements (the integration
// points; many more points in the post-processing)
#define MAX_CACHE 2048

namespace {

  enum Space { H1 = 0, HCURL = 1, HDIV = 2, L2 = 3 };

  // the names of a space and of the part of the space in the kernel of the
  // exterior derivative, in Gmsh
  const char *spaceName[4][2] = {{"H1Legendre", nullptr},
                                 {"HcurlLegendre", "HcurlLegendreGrad"},
                                 {"HdivLegendre", "HdivLegendreCurl"},
                                 {"L2Legendre", nullptr}};

  // number of components of the functions of a space (0) and of their
  // derivatives (1)
  const int numComponents[4][2] = {{1, 3}, {3, 3}, {3, 1}, {1, 1}};

  // The shapes of the elements: the nodes of their edges and facets, as
  // numbered and ordered by the basis functions of Gmsh (see getEdgeSolin() and
  // getFaceSolin() in Gmsh), the quadrangular facets coming first
  enum { PNT = 0, LIN, TRI, QUA, TET, HEX, PRI, NUM_SHAPES };

  struct Shape {
    int type; // in Gmsh
    int dim, numVertices, numEdges, numFacets;
    int edges[12][2];
    int facets[6][4]; // -1 for the fourth node of a triangle
    bool simplex;
  };

  const Shape shapes[NUM_SHAPES] = {
    {TYPE_PNT, 0, 1, 0, 0, {}, {}, true},
    {TYPE_LIN, 1, 2, 1, 0, {{0, 1}}, {}, true},
    {TYPE_TRI, 2, 3, 3, 1, {{0, 1}, {1, 2}, {2, 0}}, {{0, 1, 2, -1}}, true},
    {TYPE_QUA,
     2,
     4,
     4,
     1,
     {{0, 1}, {1, 2}, {3, 2}, {0, 3}},
     {{0, 1, 3, 2}},
     false},
    {TYPE_TET,
     3,
     4,
     6,
     4,
     {{0, 1}, {1, 2}, {2, 0}, {0, 3}, {2, 3}, {1, 3}},
     {{0, 1, 2, -1}, {0, 1, 3, -1}, {0, 2, 3, -1}, {1, 2, 3, -1}},
     true},
    {TYPE_HEX,
     3,
     8,
     12,
     6,
     {{0, 1},
      {0, 3},
      {0, 4},
      {1, 2},
      {1, 5},
      {3, 2},
      {2, 6},
      {3, 7},
      {4, 5},
      {4, 7},
      {5, 6},
      {7, 6}},
     {{0, 1, 3, 2},
      {0, 1, 4, 5},
      {0, 3, 4, 7},
      {1, 2, 5, 6},
      {3, 2, 7, 6},
      {4, 5, 7, 6}},
     false},
    {TYPE_PRI,
     3,
     6,
     9,
     5,
     {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 4}, {2, 5}, {3, 4}, {3, 5}, {4, 5}},
     {{0, 1, 3, 4}, {0, 2, 3, 5}, {1, 2, 4, 5}, {0, 1, 2, -1}, {3, 4, 5, -1}},
     false}};

  int getShape(int elementType)
  {
    switch(elementType) {
    case POINT_ELEMENT: return PNT;
    case LINE:
    case LINE_2:
    case LINE_3:
    case LINE_4: return LIN;
    case TRIANGLE:
    case TRIANGLE_2:
    case TRIANGLE_3:
    case TRIANGLE_4: return TRI;
    case QUADRANGLE:
    case QUADRANGLE_2:
    case QUADRANGLE_2_8N:
    case QUADRANGLE_3:
    case QUADRANGLE_4: return QUA;
    case TETRAHEDRON:
    case TETRAHEDRON_2:
    case TETRAHEDRON_3:
    case TETRAHEDRON_4: return TET;
    case HEXAHEDRON:
    case HEXAHEDRON_2:
    case HEXAHEDRON_2_20N:
    case HEXAHEDRON_3:
    case HEXAHEDRON_4: return HEX;
    case PRISM:
    case PRISM_2:
    case PRISM_2_15N:
    case PRISM_3:
    case PRISM_4: return PRI;
    }
    return -1;
  }

  // the types of elements of each shape, in GetDP
  const int elementTypes[NUM_SHAPES] = {
    POINT_ELEMENT,
    LINE | LINE_2 | LINE_3 | LINE_4,
    TRIANGLE | TRIANGLE_2 | TRIANGLE_3 | TRIANGLE_4,
    QUADRANGLE | QUADRANGLE_2 | QUADRANGLE_2_8N | QUADRANGLE_3 | QUADRANGLE_4,
    TETRAHEDRON | TETRAHEDRON_2 | TETRAHEDRON_3 | TETRAHEDRON_4,
    HEXAHEDRON | HEXAHEDRON_2 | HEXAHEDRON_2_20N | HEXAHEDRON_3 | HEXAHEDRON_4,
    PRISM | PRISM_2 | PRISM_2_15N | PRISM_3 | PRISM_4};

  // The orientation of a facet for the basis functions of Gmsh, from the tags
  // of its nodes (see MFace::getOrientationFlagForFace() in Gmsh)
  void facetFlags(const long long *t, int n, int flags[3])
  {
    long long s[4]; // the sorted tags
    for(int i = 0; i < n; i++) s[i] = t[i];
    std::sort(s, s + n);
    flags[2] = 1;
    if(n == 3) {
      if(s[0] == t[0]) {
        flags[0] = 0;
        flags[1] = (s[1] == t[1]) ? 1 : -1;
      }
      else if(t[1] == s[0]) {
        flags[0] = 1;
        flags[1] = (t[0] == s[2]) ? 1 : -1;
      }
      else {
        flags[0] = 2;
        flags[1] = (t[1] == s[2]) ? 1 : -1;
      }
      return;
    }
    int c = 0;
    for(int i = 0; i < 4; i++)
      if(s[0] == t[i]) c = i;
    long long opposed = t[3 - c];
    long long a = s[0], b = (s[1] == opposed) ? s[2] : s[1];
    if(a == t[0]) {
      flags[0] = 1;
      flags[1] = 1;
      flags[2] = (b == t[1]) ? 1 : -1;
    }
    else if(a == t[1]) {
      flags[0] = -1;
      flags[1] = 1;
      flags[2] = (b == t[0]) ? 1 : -1;
    }
    else if(a == t[2]) {
      flags[0] = 1;
      flags[1] = -1;
      flags[2] = (b == t[3]) ? 1 : -1;
    }
    else {
      flags[0] = -1;
      flags[1] = -1;
      flags[2] = (b == t[2]) ? 1 : -1;
    }
  }

  // an index for the orientation of a facet
  int facetIndex(const int flags[3], bool triangle)
  {
    if(triangle) return flags[0] + (flags[1] == -1 ? 3 : 0);
    return (flags[0] == -1 ? 1 : 0) | (flags[1] == -1 ? 2 : 0) |
           (flags[2] == -1 ? 4 : 0);
  }

  // the value or the derivative of a function
  void store(Space space, int derivative, const Dual &f, double s[])
  {
    if(derivative && space == H1)
      for(int i = 0; i < 3; i++) s[i] = f.d[i];
    else
      s[0] = f.v;
  }
  void store(Space space, int derivative, const DualVector &f, double s[])
  {
    if(!derivative)
      for(int i = 0; i < 3; i++) s[i] = f.c[i].v;
    else if(space == HCURL)
      for(int i = 0; i < 3; i++) s[i] = f.curl(i);
    else
      s[0] = f.div();
  }
  // the component along w, for the normal traces
  double normal(const Dual &) { return 0.; }
  double normal(const DualVector &f) { return f.c[2].v; }
  Dual scale(double a, const Dual &f) { return a * f; }
  DualVector scale(double a, const DualVector &f) { return a * f; }

  // The basis functions of a space on a family of elements (points, lines,
  // triangles, ...), up to the highest order asked for so far
  class Family {
  public:
    virtual ~Family() {}
    // the function at the given position on the node, edge, facet or volume
    // (type 0, 1, 2 or 3) number numEntity of the element (starting at 1, in
    // the numbering of GetDP), or its derivative, in the reference element
    virtual void evaluate(struct Element *Element, int type, int position,
                          int numEntity, double u, double v, double w,
                          int derivative, double s[]) = 0;
    // the order in Gmsh of the function at the given position on an entity of
    // the given type with the given number of nodes if it is a facet (-1 if
    // there is none), and whether it is known to be in the kernel of the
    // derivative
    virtual int order(int type, int numNodes, int position, bool &kernel) = 0;
    // the number of functions, in the space of the given order, of an entity
    // of the given type, with the given number of nodes if it is a facet
    virtual int count(int type, int numNodes, int order) = 0;
  };

  // E is the type of the functions: Dual for the scalar functions of H1 and
  // L2, DualVector for the vector functions of H(curl) and H(div)
  template <class E> class FamilyOf : public Family {
  public:
    // the functions of the given space on the elements of the given shape; if
    // volume is not negative, the functions are the normal traces of the
    // functions of the first facet of the elements of that shape
    FamilyOf(Space space, int shape, int volume = -1)
      : _space(space), _element(&shapes[shape]),
        _shape(&shapes[(volume < 0) ? shape : volume]), _trace(volume >= 0)
    {
    }

    void evaluate(struct Element *Element, int type, int position,
                  int numEntity, double u, double v, double w, int derivative,
                  double s[])
    {
      const int nc = numComponents[_space][derivative];
      for(int i = 0; i < nc; i++) s[i] = 0.;
      int entity = 0;
      if(_trace) {
        // the normal trace of the functions of the first facet (w = 0 in a
        // tetrahedron, w = -1 in a hexahedron) of a volume whose other nodes
        // have the largest tags: it only depends on the orientation of the
        // facet; the derivative has no trace
        if(derivative) return;
        type = 2;
        w = (_shape->type == TYPE_TET) ? 0. : -1.;
      }
      else {
        // the functions of L2 are those of the element, whatever its dimension
        if(_space == L2) type = _element->dim;
        entity = _entity(Element, type, numEntity);
        if(entity < 0) return;
      }
      int index = _index(type, entity, position);
      if(index < 0) return;
      _orient(Element);
      Point &p = _point(u, v, w);
      if(type == 0)
        store(_space, derivative, p.vertex[index], s);
      else if(type == 1) {
        if(_reversed[entity] &&
           _basis->getEdgeFunctionSignForReversedEdge(position) < 0)
          store(_space, derivative, scale(-1., p.edge[index]), s);
        else
          store(_space, derivative, p.edge[index], s);
      }
      else if(type == 2) {
        const E &f = _facet(p, entity)[index];
        if(_trace)
          s[2] = normal(f);
        else
          store(_space, derivative, f, s);
      }
      else
        store(_space, derivative, p.bubble[index], s);
    }

    int order(int type, int numNodes, int position, bool &kernel)
    {
      kernel = false;
      if(_space == L2) type = _shape->dim;
      for(int p = (_space == H1) ? 1 : 0; p <= MAX_ORDER; p++) {
        if(!_has(spaceName[_space][0], p, type, numNodes, position)) continue;
        // only the simplices give the kernel of the derivative for all spaces
        if(p > 0 && spaceName[_space][1] && _shape->simplex)
          kernel = _has(spaceName[_space][1], p, type, numNodes, position);
        return p;
      }
      return -1;
    }

    int count(int type, int numNodes, int order)
    {
      if(_space == L2) type = _shape->dim;
      HierarchicalBasis *b =
        HierarchicalBasis::create(spaceName[_space][0], _shape->type, order);
      if(!b) return 0;
      // the first entity, or the first facet with that number of nodes
      int entity = (type == 2 && numNodes == 3) ? b->getNumQuadFace() : 0;
      int n = 0;
      for(auto &f : b->getFunctions())
        if(f.type == type && f.entity == entity) n++;
      delete b;
      return n;
    }

  private:
    Space _space;
    // the shape of the elements, and of the elements of the functions (a
    // volume for the traces)
    const Shape *_element, *_shape;
    bool _trace;
    // the functions at the current order: the numbers of functions per vertex,
    // edge, quadrangular and triangular facet, and in the volume
    int _order = -1;
    HierarchicalBasis *_basis = nullptr;
    int _perVertex = 0, _perEdge = 0, _perQuad = 0, _perTri = 0, _perBubble = 0;
    // the edges and facets in the numbering of Gmsh, by their number in GetDP
    std::vector<int> _edges, _facets;
    // the orientation of the edges and facets of the last element
    struct Geo_Element *_oriented = nullptr;
    bool _reversed[12];
    int _flags[6][3], _flagsIndex[6];
    // the functions at a point of the reference element, and those of each
    // facet for the orientations of the facet met so far
    struct Point {
      std::vector<E> vertex, edge, facet, bubble;
      std::vector<E> oriented[6][8];
    };
    typedef std::array<double, 3> Key;
    std::map<Key, Point> _cache;
    Point *_last = nullptr;
    Key _lastKey;

    bool _has(const char *name, int order, int type, int numNodes,
              int position)
    {
      HierarchicalBasis *b = HierarchicalBasis::create(name, _shape->type, order);
      if(!b) return false;
      bool found = false;
      for(auto &f : b->getFunctions()) {
        if(f.type != type || f.position != position) continue;
        // the first entity, or the first facet with that number of nodes
        if(type == 2 && numNodes) {
          if(f.entity == ((numNodes == 4) ? 0 : b->getNumQuadFace()))
            found = true;
        }
        else if(f.entity == 0)
          found = true;
      }
      delete b;
      return found;
    }

    bool _setOrder(int order)
    {
      if(_basis) delete _basis;
      _cache.clear();
      _last = nullptr;
      _order = -1;
      _basis =
        HierarchicalBasis::create(spaceName[_space][0], _shape->type, order);
      if(!_basis) return false;
      const int ne = _basis->getNumEdge(), nq = _basis->getNumQuadFace(),
                nt = _basis->getNumTriFace();
      _perVertex = _basis->getNumVertexFunction() ? 1 : 0;
      _perEdge = ne ? _basis->getNumEdgeFunction() / ne : 0;
      _perQuad = nq ? _basis->getNumQuadFaceFunction() / nq : 0;
      _perTri = nt ? _basis->getNumTriFaceFunction() / nt : 0;
      _perBubble = _basis->getNumBubbleFunction();
      _order = order;
      return true;
    }

    // whether the facet is a triangle
    bool _triangle(int facet) const { return _shape->facets[facet][3] < 0; }

    // the index of a function among those of the entities of its type (-1 if
    // there is none at the current order)
    int _indexAtOrder(int type, int entity, int position) const
    {
      if(_order < 0) return -1;
      switch(type) {
      case 0: return (position < _perVertex) ? entity : -1;
      case 1: return (position < _perEdge) ? entity * _perEdge + position : -1;
      case 2:
        // the functions of the quadrangular facets come first
        if(_triangle(entity)) {
          int nq = _basis->getNumQuadFace();
          return (position < _perTri) ?
                   nq * _perQuad + (entity - nq) * _perTri + position :
                   -1;
        }
        return (position < _perQuad) ? entity * _perQuad + position : -1;
      default: return (position < _perBubble) ? position : -1;
      }
    }

    int _index(int type, int entity, int position)
    {
      int index = _indexAtOrder(type, entity, position);
      if(index >= 0) return index;
      // not in the space of the current order: raise the order
      int old = _order;
      for(int p = std::max(old + 1, (_space == H1) ? 1 : 0); p <= MAX_ORDER;
          p++) {
        if(!_setOrder(p)) break;
        index = _indexAtOrder(type, entity, position);
        if(index >= 0) return index;
      }
      static const char *names[4] = {"nodes", "edges", "facets", "volume"};
      Message::Error("No function at position %d on the %s of this type of "
                     "element in the basis of %s",
                     position, names[type], spaceName[_space][0]);
      if(old >= 0) _setOrder(old);
      return -1;
    }

    // the edges and facets of an element are not numbered in the same way in
    // GetDP and in Gmsh: match them through their nodes
    void _numberEntities(struct Element *Element)
    {
      struct Geo_Element *ge = Element->GeoElement;
      _edges = {0};
      _facets = {0};
      if(_shape->dim < 2) return;
      if(ge->NbrEdges == 0) Geo_CreateEdgesOfElement(ge);
      _edges.assign(ge->NbrEdges, -1);
      for(int i = 0; i < ge->NbrEdges; i++) {
        int *n = Geo_GetNodesOfEdgeInElement(ge, i);
        int mask = (1 << (abs(n[0]) - 1)) | (1 << (abs(n[1]) - 1));
        for(int j = 0; j < _shape->numEdges; j++)
          if(mask ==
             ((1 << _shape->edges[j][0]) | (1 << _shape->edges[j][1])))
            _edges[i] = j;
      }
      if(_shape->dim < 3) return;
      if(ge->NbrFacets == 0) Geo_CreateFacetsOfElement(ge);
      _facets.assign(ge->NbrFacets, -1);
      for(int i = 0; i < ge->NbrFacets; i++) {
        int *n = Geo_GetNodesOfFacetInElement(ge, i);
        int mask = 0;
        for(int k = 0; k < 4 && n[k]; k++) mask |= (1 << (abs(n[k]) - 1));
        for(int j = 0; j < _shape->numFacets; j++) {
          int m = 0;
          for(int k = 0; k < 4; k++)
            if(_shape->facets[j][k] >= 0) m |= (1 << _shape->facets[j][k]);
          if(mask == m) _facets[i] = j;
        }
      }
    }

    int _entity(struct Element *Element, int type, int numEntity)
    {
      if(type == 3) return 0;
      if(type == 0) { // the nodes have the same numbers
        if(numEntity >= 1 && numEntity <= _shape->numVertices)
          return numEntity - 1;
        // no function on the nodes of the high order elements
        return -1;
      }
      if(_edges.empty()) _numberEntities(Element);
      const std::vector<int> &e = (type == 1) ? _edges : _facets;
      if(numEntity < 1 || numEntity > (int)e.size() || e[numEntity - 1] < 0) {
        Message::Error("Wrong %s number %d in the basis of %s",
                       (type == 1) ? "edge" : "facet", numEntity,
                       spaceName[_space][0]);
        return -1;
      }
      return e[numEntity - 1];
    }

    // The orientation of the edges and facets of the element, from the order
    // of their nodes (see Geo_NodeKey: the order of the tags of the nodes, or
    // of their master nodes on periodic meshes, so that an entity and its
    // periodic counterpart bear the same functions): an edge is reversed if
    // its first node is not the first one in that order
    void _orient(struct Element *Element)
    {
      struct Geo_Element *ge = Element->GeoElement;
      if(ge == _oriented) return;
      _oriented = ge;
      // the rank of the nodes of the element in the order of their keys (the
      // tags of their master nodes on periodic meshes), then the other nodes
      // of the volume of a trace
      long long t[8];
      const int n = _element->numVertices;
      struct Geo_NodeKey key[8];
      for(int i = 0; i < n; i++) Geo_GetNodeKey(ge->NumNodes[i], key[i]);
      for(int i = 0; i < n; i++) {
        t[i] = 0;
        for(int j = 0; j < n; j++)
          if(Geo_IsNodeKeyBefore(key[j], key[i])) t[i]++;
      }
      for(int i = n; i < _shape->numVertices; i++) t[i] = i;
      for(int i = 0; i < _shape->numEdges; i++)
        _reversed[i] = (t[_shape->edges[i][0]] > t[_shape->edges[i][1]]);
      for(int i = 0; i < _shape->numFacets; i++) {
        long long f[4];
        int n = _triangle(i) ? 3 : 4;
        for(int k = 0; k < n; k++) f[k] = t[_shape->facets[i][k]];
        facetFlags(f, n, _flags[i]);
        _flagsIndex[i] = facetIndex(_flags[i], n == 3);
      }
    }

    // the functions at a point of the reference element
    Point &_point(double u, double v, double w)
    {
      Key key = {u, v, w};
      if(_last && key == _lastKey) return *_last;
      auto it = _cache.find(key);
      if(it == _cache.end()) {
        if(_cache.size() >= MAX_CACHE) _cache.clear();
        it = _cache.insert({key, Point()}).first;
        Point &p = it->second;
        p.vertex.resize(_basis->getNumVertexFunction());
        p.edge.resize(_basis->getNumEdgeFunction());
        p.facet.resize(_basis->getNumQuadFaceFunction() +
                       _basis->getNumTriFaceFunction());
        p.bubble.resize(_basis->getNumBubbleFunction());
        const Dual x[3] = {Dual::coordinate(0, u), Dual::coordinate(1, v),
                           Dual::coordinate(2, w)};
        _basis->functions(x, p.vertex, p.edge, p.facet, p.bubble);
      }
      _last = &it->second;
      _lastKey = key;
      return it->second;
    }

    // the functions of the facets at a point, with those of the given facet
    // oriented as in the current element
    const std::vector<E> &_facet(Point &p, int facet)
    {
      std::vector<E> &f = p.oriented[facet][_flagsIndex[facet]];
      if(f.empty()) {
        f = p.facet;
        const Dual x[3] = {Dual::coordinate(0, _lastKey[0]),
                           Dual::coordinate(1, _lastKey[1]),
                           Dual::coordinate(2, _lastKey[2])};
        _basis->faceFunctions(x, _flags[facet][0], _flags[facet][1],
                              _flags[facet][2], facet, f);
      }
      return f;
    }
  };

  // the functions of a space on the elements of the given type in GetDP, if
  // any
  Family *getFamily(Space space, int shape)
  {
    static Family *families[4][NUM_SHAPES];
    static bool init = false;
    if(!init) {
      init = true;
      for(int s = 0; s < NUM_SHAPES; s++) {
        families[H1][s] = new FamilyOf<Dual>(H1, s);
        families[HCURL][s] = (s == PNT) ? nullptr :
                                          new FamilyOf<DualVector>(HCURL, s);
        families[HDIV][s] =
          (s < TET) ? nullptr : new FamilyOf<DualVector>(HDIV, s);
        families[L2][s] = (s == PNT) ? nullptr : new FamilyOf<Dual>(L2, s);
      }
      // the normal traces of the functions of H(div)
      families[HDIV][TRI] = new FamilyOf<DualVector>(HDIV, TRI, TET);
      families[HDIV][QUA] = new FamilyOf<DualVector>(HDIV, QUA, HEX);
    }
    return (shape >= 0) ? families[space][shape] : nullptr;
  }

  // A basis function does not know which function it is: each function used
  // in the problem (a space or its derivative, a type of entity and a
  // position) is given one of a set of functions with the signature of the
  // basis functions
  struct Slot {
    Space space;
    int derivative, type, position;
  };
  std::vector<Slot> slots;

  template <int I> void BF(BF_ARG)
  {
    const Slot &s = slots[I];
    Family *family = getFamily(s.space, getShape(Element->Type));
    if(!family) {
      for(int i = 0; i < numComponents[s.space][s.derivative]; i++)
        Value[i] = 0.;
      Message::Error("Unknown type of Element in the basis of %s",
                     spaceName[s.space][0]);
      return;
    }
    family->evaluate(Element, s.type, s.position, NumEntity, u, v, w,
                     s.derivative, Value);
  }

  typedef void (*BF_Pointer)(BF_ARG);

  template <std::size_t... I>
  const BF_Pointer *getTable(std::index_sequence<I...>)
  {
    static const BF_Pointer table[] = {BF<I>...};
    return table;
  }

  void (*getFunction(Space space, int derivative, int type, int position))()
  {
    std::size_t i = 0;
    for(; i < slots.size(); i++)
      if(slots[i].space == space && slots[i].derivative == derivative &&
         slots[i].type == type && slots[i].position == position)
        break;
    if(i == slots.size()) {
      if(i == MAX_FUNCTIONS) {
        Message::Error("Too many Legendre basis functions (maximum %d)",
                       MAX_FUNCTIONS);
        return nullptr;
      }
      slots.push_back({space, derivative, type, position});
    }
    return (void (*)())getTable(std::make_index_sequence<MAX_FUNCTIONS>())[i];
  }

} // namespace

#endif

#if defined(HAVE_GMSH_HIERARCHICAL) && defined(HAVE_KERNEL)

namespace {

  // the first type of entity bearing functions: the nodes for H1, the edges
  // for H(curl), the facets for H(div) and the elements for L2
  int firstType(Space space) { return (space == L2) ? 3 : (int)space; }

  // the shape giving the functions of the triangular (numNodes = 3) or
  // quadrangular facets: H(div) has no functions in the plane
  int facetShape(Space space, int numNodes)
  {
    if(space == HDIV) return (numNodes == 4) ? HEX : TET;
    return (numNodes == 4) ? QUA : TRI;
  }

  // Set the function at the given position on the entities of the given type,
  // borne by the elements of the given types (the order of the function is
  // that on the given shape, with numNodes nodes per facet)
  bool setFunction(Space space, int derivative, int type, int position,
                   int elementType, int entityFilter, int shape, int numNodes,
                   struct BasisFunction *bf)
  {
    Family *family = getFamily(space, shape);
    bool kernel = false;
    int order = family ? family->order(type, numNodes, position, kernel) : -1;
    if(order < 0) return false;
    void (*f)() = getFunction(space, 0, type, position);
    // L2 has no derivative: the function itself, as BF_Volume
    void (*df)() = (space == L2) ? f : getFunction(space, 1, type, position);
    if(!f || !df) return false;
    bf->Function = derivative ? df : f;
    bf->dFunction = (space == L2) ? f : derivative ? (void (*)())BF_Zero : df;
    bf->dInvFunction = derivative ? f : (void (*)())BF_Zero;
    // The orders of GetDP. For H1 and L2, the order in Gmsh. For H(curl) and
    // H(div) on the simplices, 0.5 for the Whitney functions, p for the
    // functions of order p in the kernel of the derivative (the gradients and
    // the curls), which complete the polynomials of degree p, and p - 0.5 for
    // the other functions of order p (see the function space types in Gmsh);
    // on the other shapes, where the orders of GetDP do not match those of
    // Gmsh, p + 0.5. One order less for the derivatives.
    double o;
    if(space == H1 || space == L2)
      o = order;
    else if(shapes[shape].simplex)
      o = kernel ? order : (order > 0) ? order - 0.5 : 0.5;
    else
      o = order + 0.5;
    if(derivative) o = (o >= 1.) ? o - 1. : 0.;
    bf->Order = o;
    bf->ElementType = elementType;
    // the functions are oriented here, from the tags of the nodes
    bf->Orient = 0;
    bf->EntityFilter = entityFilter;
    bf->HigherOrder = (type != firstType(space) || position > 0);
    return true;
  }

} // namespace

#endif

// the names of the spaces: the space and whether it is the derivative
static const struct {
  const char *name;
  int space, derivative;
} legendreNames[] = {{"BF_H1Legendre", 0, 0},    {"BF_GradH1Legendre", 0, 1},
                     {"BF_HcurlLegendre", 1, 0}, {"BF_CurlHcurlLegendre", 1, 1},
                     {"BF_HdivLegendre", 2, 0},  {"BF_DivHdivLegendre", 2, 1},
                     {"BF_L2Legendre", 3, 0}};

bool BF_IsLegendreSpace(const char *Name)
{
  for(auto &n : legendreNames)
    if(!strcmp(Name, n.name)) return true;
  return false;
}

bool BF_GetLegendreSpace(const char *Name, int Order, int EntityType,
                         std::vector<struct BasisFunction> &Functions)
{
  Functions.clear();
  int sp = -1, derivative = 0;
  for(auto &n : legendreNames) {
    if(!strcmp(Name, n.name)) {
      sp = n.space;
      derivative = n.derivative;
    }
  }
  if(sp < 0 || EntityType < 0 || EntityType > 3) return false;

#if defined(HAVE_GMSH_HIERARCHICAL) && defined(HAVE_KERNEL)
  Space space = (Space)sp;
  if(Order < ((space == H1) ? 1 : 0) || Order > MAX_ORDER) {
    Message::Error("Wrong order %d for '%s' (%d to %d)", Order, Name,
                   (space == H1) ? 1 : 0, MAX_ORDER);
    return false;
  }
  const int type = EntityType;
  // no functions on the entities of that type
  if(type < firstType(space)) return true;

  // the shapes of the entities of that type, with the number of nodes of the
  // facets, the shape giving their functions and the elements bearing them
  const int *e = elementTypes;
  struct Bearer {
    int shape, numNodes, elementType, entityFilter, count;
  };
  std::vector<Bearer> bearers;
  if(space == L2) {
    for(int s = LIN; s <= PRI; s++)
      bearers.push_back({s, 0, e[s], ENTITYFILTER_NONE, 0});
  }
  else if(type == 0) {
    // no function on the high order nodes of an element
    bearers.push_back(
      {LIN, 0, e[PNT] | e[LIN] | e[TRI] | e[QUA] | e[TET] | e[HEX] | e[PRI],
       ENTITYFILTER_VERTICES, 0});
  }
  else if(type == 1) {
    bearers.push_back(
      {LIN, 0, e[LIN] | e[TRI] | e[QUA] | e[TET] | e[HEX] | e[PRI],
       ENTITYFILTER_NONE, 0});
  }
  else if(type == 2) {
    bearers.push_back({facetShape(space, 3), 3, e[TRI] | e[TET] | e[PRI],
                       ENTITYFILTER_TRIANGLES, 0});
    bearers.push_back({facetShape(space, 4), 4, e[QUA] | e[HEX] | e[PRI],
                       ENTITYFILTER_QUADRANGLES, 0});
  }
  else {
    for(int s = TET; s <= PRI; s++)
      bearers.push_back({s, 0, e[s], ENTITYFILTER_NONE, 0});
  }
  int n = 0;
  for(auto &b : bearers) {
    Family *family = getFamily(space, b.shape);
    b.count = family ? family->count(type, b.numNodes, Order) : 0;
    n = std::max(n, b.count);
  }

  // a function is borne by the entities with at least that many functions at
  // that order: the triangular and quadrangular facets do not bear the same
  // number of functions, nor do the elements of different shapes
  for(int position = 0; position < n; position++) {
    int elementType = 0, entityFilter = ENTITYFILTER_NONE, num = 0;
    const Bearer *first = nullptr;
    for(auto &b : bearers) {
      if(position >= b.count) continue;
      elementType |= b.elementType;
      entityFilter = b.entityFilter;
      if(!first) first = &b;
      num++;
    }
    if(num > 1) entityFilter = ENTITYFILTER_NONE;
    struct BasisFunction bf;
    if(!setFunction(space, derivative, type, position, elementType,
                    entityFilter, first->shape, first->numNodes, &bf)) {
      Message::Error("No function at position %d for '%s' of order %d",
                     position, Name, Order);
      return false;
    }
    Functions.push_back(bf);
  }
  return true;
#else
  (void)derivative;
  (void)Order;
  Message::Error("'%s' requires the HierarchicalBasis module of Gmsh", Name);
  return false;
#endif
}

bool BF_GetLegendre(const char *Name, struct BasisFunction *Function)
{
  const char *s = nullptr;
  int sp = 0, derivative = 0;
  for(auto &n : legendreNames) {
    size_t l = strlen(n.name);
    if(!strncmp(Name, n.name, l) && Name[l] == '_') {
      s = Name + l + 1;
      sp = n.space;
      derivative = n.derivative;
    }
  }
  if(!s) return false;
  int type = (s[0] == 'N') ? 0 : (s[0] == 'E') ? 1 : (s[0] == 'F') ? 2 :
             (s[0] == 'V') ? 3 : -1;
  if(type < 0) return false;
  s++;
  // the shape of the entities bearing the function, if given: 1 to 6 for
  // lines, triangles, quadrangles, tetrahedra, hexahedra and prisms
  static const char *tags[7] = {"", "Lin", "Tri", "Qua", "Tet", "Hex", "Pri"};
  int tag = 0;
  for(int i = 1; i < 7; i++)
    if(!strncmp(s, tags[i], 3)) tag = i;
  if(tag) s += 3;
  // the position can follow an underscore, as in the names built in a loop
  // (BF_HcurlLegendre_E~{k})
  if(s[0] == '_') s++;
  if(!s[0] || strlen(s) > 4) return false;
  for(const char *c = s; *c; c++)
    if(*c < '0' || *c > '9') return false;
  int position = atoi(s);

#if defined(HAVE_GMSH_HIERARCHICAL) && defined(HAVE_KERNEL)
  Space space = (Space)sp;
  if(type < firstType(space)) {
    Message::Error("No function '%s': wrong type of entity for this space",
                   Name);
    return false;
  }
  // the shapes that can be given for the entity: triangles and quadrangles
  // for the facets, volumes for the volumes, any element for L2
  bool validTag = !tag || (space == L2 && type == 3) ||
                  (type == 2 && (tag == TRI || tag == QUA)) ||
                  (type == 3 && tag >= TET);
  if(!validTag) {
    Message::Error("No function '%s': wrong shape for this entity", Name);
    return false;
  }
  // the function is defined on the entity bearing it and on the elements it
  // bounds, whatever their shape if none is given; its order is then that on
  // the simplices
  const int *e = elementTypes;
  int elementType, entityFilter = ENTITYFILTER_NONE, shape;
  if(tag && type == 3) {
    elementType = e[tag];
    shape = tag;
  }
  else if(tag == TRI) {
    elementType = e[TRI] | e[TET] | e[PRI];
    entityFilter = ENTITYFILTER_TRIANGLES;
    shape = facetShape(space, 3);
  }
  else if(tag == QUA) {
    elementType = e[QUA] | e[HEX] | e[PRI];
    entityFilter = ENTITYFILTER_QUADRANGLES;
    shape = facetShape(space, 4);
  }
  else if(space == L2) {
    elementType = e[LIN] | e[TRI] | e[QUA] | e[TET] | e[HEX] | e[PRI];
    shape = TET;
  }
  else if(type == 0) {
    elementType = e[PNT] | e[LIN] | e[TRI] | e[QUA] | e[TET] | e[HEX] | e[PRI];
    // no function on the high order nodes of an element
    entityFilter = ENTITYFILTER_VERTICES;
    shape = LIN;
  }
  else if(type == 1) {
    elementType = e[LIN] | e[TRI] | e[QUA] | e[TET] | e[HEX] | e[PRI];
    shape = LIN;
  }
  else if(type == 2) {
    elementType = e[TRI] | e[QUA] | e[TET] | e[HEX] | e[PRI];
    shape = facetShape(space, 3);
  }
  else {
    elementType = e[TET] | e[HEX] | e[PRI];
    shape = TET;
  }
  if(!setFunction(space, derivative, type, position, elementType, entityFilter,
                  shape, (tag == QUA) ? 4 : 3, Function)) {
    Message::Error("No function '%s' up to order %d", Name, MAX_ORDER);
    return false;
  }
  return true;
#else
  (void)sp;
  (void)derivative;
  (void)position;
  Message::Error("'%s' requires the HierarchicalBasis module of Gmsh", Name);
  return false;
#endif
}
