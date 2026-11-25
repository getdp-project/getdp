// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

// Legendre basis functions: hierarchical functions of arbitrary order of H1,
// H(curl), H(div) and L2, evaluated by Gmsh (the "H1Legendre", "HcurlLegendre",
// "HdivLegendre" and "L2Legendre" function spaces of its HierarchicalBasis
// module), on lines, triangles and tetrahedra.
//
// A basis function of GetDP is one function per entity (node, edge, facet or
// volume), whereas the number of functions per entity grows with the order.
// The functions are thus named after the type of their entity and their
// position among the functions of that entity in Gmsh, which does not depend
// on the order of the space:
//
//   BF_H1Legendre_N0       function of a node          (Entity NodesOf)
//   BF_H1Legendre_E<k>     k-th function of an edge    (Entity EdgesOf)
//   BF_H1Legendre_F<k>     k-th function of a facet    (Entity FacetsOf)
//   BF_H1Legendre_V<k>     k-th function of a volume   (Entity VolumesOf)
//   BF_HcurlLegendre_E<k>, BF_HcurlLegendre_F<k>, BF_HcurlLegendre_V<k>
//   BF_HdivLegendre_F<k>, BF_HdivLegendre_V<k>
//   BF_L2Legendre_V<k>     k-th function of an element (Entity VolumesOf)
//
// (or BF_H1Legendre_E_<k>, etc., as given by BF_H1Legendre_E~{k} in a loop),
// with their exterior derivatives BF_GradH1Legendre_..., BF_CurlHcurlLegendre_...
// and BF_DivHdivLegendre_... On simplices the full spaces of order p are made
// of the following functions, the functions of an entity being sorted by
// increasing order:
//
//   - H1 (polynomials of degree p, p >= 1): N0, the p - 1 first functions of
//     the edges, the (p - 2) (p - 1) / 2 first functions of the facets and the
//     (p - 3) (p - 2) (p - 1) / 6 first functions of the volumes;
//   - H(curl) (polynomials of degree p, p >= 0): the p + 1 first functions of
//     the edges (E0 is the Whitney function BF_Edge, and Ek, k > 0, the
//     gradient of order k), the (p - 1) (p + 1) first functions of the facets
//     and the (p - 2) (p - 1) (p + 1) / 2 first functions of the volumes;
//   - H(div) (polynomials of degree p, p >= 0): the (p + 1) (p + 2) / 2 first
//     functions of the facets (F0 is the Whitney function BF_Facet) and the
//     (p - 1) (p + 1) (p + 2) / 2 first functions of the volumes;
//   - L2 (polynomials of degree p, p >= 0): the p + 1, (p + 1) (p + 2) / 2 or
//     (p + 1) (p + 2) (p + 3) / 6 first functions of a line, a triangle or a
//     tetrahedron.
//
// The interior functions of a triangle are facet functions, and those of a line
// edge functions, so that the functions of H1 and H(curl) on the lines and
// triangles are the traces of the functions of the triangles and tetrahedra
// they bound. The functions of H(div) are those of the tetrahedra, and their
// normal traces on the triangles (a normal vector in the reference triangle,
// as BF_Facet): there are no functions of H(div) in the plane.

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
#define MAX_FUNCTIONS 2048

// the highest order of the function spaces of Gmsh
#define MAX_ORDER 40

// the points kept in the cache of each family of elements (the integration
// points, for each orientation of the elements; many more points in the
// post-processing)
#define MAX_CACHE 4096

namespace {

  enum Space { H1 = 0, HCURL = 1, HDIV = 2, L2 = 3 };

  // the names of a space, of its exterior derivative and of the part of the
  // space in the kernel of the derivative, in Gmsh
  const char *spaceName[4][3] = {
    {"H1Legendre", "GradH1Legendre", nullptr},
    {"HcurlLegendre", "CurlHcurlLegendre", "HcurlLegendreGrad"},
    {"HdivLegendre", "DivHdivLegendre", "HdivLegendreCurl"},
    {"L2Legendre", nullptr, nullptr}};

  // number of components of the functions of a space (0) and of their
  // derivatives (1)
  const int numComponents[4][2] = {{1, 3}, {3, 3}, {3, 1}, {1, 1}};

  // The basis functions of a space on a family of elements (points, lines,
  // triangles or tetrahedra), up to the highest order asked for so far
  class Family {
  public:
    // the functions of the given space on the elements of the given type in
    // Gmsh; if trace is set, the functions on the triangles are the normal
    // traces of the functions of the tetrahedra
    Family(Space space, int type, int numVertices, bool trace = false)
      : _space(space), _type(type), _numVertices(numVertices), _trace(trace)
    {
    }

    // the function at the given position on the node, edge, facet or volume
    // (type 0, 1, 2 or 3) number numEntity of the element (starting at 1, in
    // the numbering of GetDP), or its derivative, in the reference element
    void evaluate(struct Element *Element, int type, int position,
                  int numEntity, double u, double v, double w, int derivative,
                  double s[])
    {
      const int nc = numComponents[_space][derivative];
      for(int i = 0; i < nc; i++) s[i] = 0.;
      if(_trace) {
        // the normal trace on a triangle of the functions of the facet of a
        // tetrahedron, which only depends on the orientation of the facet: that
        // of the first facet (w = 0) of a tetrahedron whose fourth node has the
        // largest tag; the derivative has no trace
        if(derivative) return;
        int index = _find(2, 0, position);
        if(index < 0) return;
        const int *n = Element->GeoElement->NumNodes;
        int t[4] = {n[0], n[1], n[2], INT_MAX};
        s[2] = _values(0, _orientation(t, 4), u, v, 0.)[3 * index + 2];
        return;
      }
      // the functions of L2 are those of the element, whatever its dimension
      if(_space == L2)
        type = (_type == TYPE_LIN) ? 1 : (_type == TYPE_TRI) ? 2 : 3;
      int entity = _entity(Element, type, numEntity);
      if(entity < 0) return;
      int index = _find(type, entity, position);
      if(index < 0) return;
      const std::vector<double> &values = _values(
        derivative, _orientation(Element->GeoElement->NumNodes, _numVertices),
        u, v, w);
      for(int i = 0; i < nc; i++) s[i] = values[nc * index + i];
    }

    // the order in Gmsh of the function at the given position on an entity of
    // the given type (-1 if there is none), and whether it is in the kernel of
    // the derivative
    int order(int type, int position, bool &kernel)
    {
      kernel = false;
      if(_space == L2)
        type = (_type == TYPE_LIN) ? 1 : (_type == TYPE_TRI) ? 2 : 3;
      for(int p = (_space == H1) ? 1 : 0; p <= MAX_ORDER; p++) {
        if(!_has(spaceName[_space][0], p, type, position)) continue;
        if(p > 0 && spaceName[_space][2])
          kernel = _has(spaceName[_space][2], p, type, position);
        return p;
      }
      return -1;
    }

  private:
    Space _space;
    int _type, _numVertices;
    bool _trace;
    // the functions and their derivatives, at the current order
    int _order = -1;
    HierarchicalBasis *_basis[2] = {nullptr, nullptr};
    // the index of a function in the values, by type, entity and position
    std::map<std::array<int, 3>, int> _index;
    // the edges and facets in the numbering of Gmsh, by their number in GetDP
    std::vector<int> _edges, _facets;
    // the values of all the functions (0) and of their derivatives (1) at a
    // point (u, v, w) for an orientation of the element
    typedef std::array<double, 4> Key;
    std::map<Key, std::vector<double> > _cache[2];
    const std::vector<double> *_last[2] = {nullptr, nullptr};
    Key _lastKey[2];

    // the type of the elements of the functions in Gmsh
    int _basisType() const { return _trace ? TYPE_TET : _type; }

    bool _has(const char *name, int order, int type, int position)
    {
      HierarchicalBasis *b =
        HierarchicalBasis::create(name, _basisType(), order);
      if(!b) return false;
      bool found = false;
      for(auto &f : b->getFunctions())
        if(f.type == type && f.entity == 0 && f.position == position)
          found = true;
      delete b;
      return found;
    }

    bool _setOrder(int order)
    {
      for(int i = 0; i < 2; i++) {
        if(_basis[i]) delete _basis[i];
        _basis[i] = nullptr;
        // L2 has no derivative
        if(spaceName[_space][i])
          _basis[i] =
            HierarchicalBasis::create(spaceName[_space][i], _basisType(), order);
        _cache[i].clear();
        _last[i] = nullptr;
      }
      _index.clear();
      _order = -1;
      if(!_basis[0] || (spaceName[_space][1] && !_basis[1])) return false;
      const std::vector<HierarchicalBasis::Function> &f =
        _basis[0]->getFunctions();
      for(std::size_t i = 0; i < f.size(); i++)
        _index[{f[i].type, f[i].entity, f[i].position}] = i;
      _order = order;
      return true;
    }

    int _find(int type, int entity, int position)
    {
      auto it = _index.find({type, entity, position});
      if(it != _index.end()) return it->second;
      // not in the space of the current order: raise the order
      int old = _order;
      for(int p = std::max(old + 1, (_space == H1) ? 1 : 0); p <= MAX_ORDER;
          p++) {
        if(!_setOrder(p)) break;
        it = _index.find({type, entity, position});
        if(it != _index.end()) return it->second;
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
      static const int edges[6][2] = {{0, 1}, {1, 2}, {2, 0},
                                      {0, 3}, {2, 3}, {1, 3}};
      static const int facets[4][3] = {
        {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
      struct Geo_Element *ge = Element->GeoElement;
      _edges = {0};
      _facets = {0};
      if(_type != TYPE_TRI && _type != TYPE_TET) return;
      if(ge->NbrEdges == 0) Geo_CreateEdgesOfElement(ge);
      _edges.assign(ge->NbrEdges, -1);
      for(int i = 0; i < ge->NbrEdges; i++) {
        int *n = Geo_GetNodesOfEdgeInElement(ge, i);
        int mask = (1 << (abs(n[0]) - 1)) | (1 << (abs(n[1]) - 1));
        for(int j = 0; j < ((_type == TYPE_TRI) ? 3 : 6); j++)
          if(mask == ((1 << edges[j][0]) | (1 << edges[j][1]))) _edges[i] = j;
      }
      if(_type == TYPE_TRI) return;
      if(ge->NbrFacets == 0) Geo_CreateFacetsOfElement(ge);
      _facets.assign(ge->NbrFacets, -1);
      for(int i = 0; i < ge->NbrFacets; i++) {
        int *n = Geo_GetNodesOfFacetInElement(ge, i);
        int mask = 0;
        for(int k = 0; k < 3; k++) mask |= (1 << (abs(n[k]) - 1));
        for(int j = 0; j < 4; j++)
          if(mask == ((1 << facets[j][0]) | (1 << facets[j][1]) |
                      (1 << facets[j][2])))
            _facets[i] = j;
      }
    }

    int _entity(struct Element *Element, int type, int numEntity)
    {
      if(type == 3) return 0;
      if(type == 0) { // the nodes have the same numbers
        if(numEntity >= 1 && numEntity <= _numVertices) return numEntity - 1;
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

    // The orientation of an element in Gmsh: the rank of the order of the tags
    // of its vertices among the permutations of the vertices, in lexicographic
    // order. FIXME: on periodic meshes the tags of the master nodes should be
    // used, as in Get_FacetFunctionIndex()
    int _orientation(const int *t, int n)
    {
      int rank = 0, f = 1;
      for(int i = n - 2; i >= 0; i--) {
        int smaller = 0;
        for(int j = i + 1; j < n; j++)
          if(t[j] < t[i]) smaller++;
        f *= (n - 1 - i);
        rank += smaller * f;
      }
      return rank;
    }

    const std::vector<double> &_values(int derivative, int orientation,
                                       double u, double v, double w)
    {
      const int d = derivative;
      Key key = {(double)orientation, u, v, w};
      if(_last[d] && key == _lastKey[d]) return *_last[d];
      auto it = _cache[d].find(key);
      if(it == _cache[d].end()) {
        if(_cache[d].size() >= MAX_CACHE) _cache[d].clear();
        it = _cache[d].insert({key, std::vector<double>()}).first;
        _basis[d]->evaluate({u, v, w}, {orientation}, it->second);
      }
      _last[d] = &it->second;
      _lastKey[d] = key;
      return it->second;
    }
  };

  // the functions of a space on the elements of the given type in GetDP, if
  // any
  Family *getFamily(Space space, int elementType)
  {
    static Family *families[4][4];
    static bool init = false;
    if(!init) {
      init = true;
      for(int s = 0; s < 4; s++) {
        families[s][0] = nullptr;
        families[s][1] = new Family((Space)s, TYPE_LIN, 2);
        families[s][2] = new Family((Space)s, TYPE_TRI, 3);
        families[s][3] = new Family((Space)s, TYPE_TET, 4);
      }
      families[H1][0] = new Family(H1, TYPE_PNT, 1);
      // no functions of H(div) on lines, and normal traces on triangles
      delete families[HDIV][1];
      families[HDIV][1] = nullptr;
      delete families[HDIV][2];
      families[HDIV][2] = new Family(HDIV, TYPE_TRI, 3, true);
    }
    switch(elementType) {
    case POINT_ELEMENT: return families[space][0];
    case LINE:
    case LINE_2:
    case LINE_3:
    case LINE_4: return families[space][1];
    case TRIANGLE:
    case TRIANGLE_2:
    case TRIANGLE_3:
    case TRIANGLE_4: return families[space][2];
    case TETRAHEDRON:
    case TETRAHEDRON_2:
    case TETRAHEDRON_3:
    case TETRAHEDRON_4: return families[space][3];
    }
    return nullptr;
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
    Family *family = getFamily(s.space, Element->Type);
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

bool BF_GetLegendre(const char *Name, void (**Function)(),
                    void (**dFunction)(), void (**dInvFunction)(),
                    double *Order, int *ElementType, int *Orient)
{
  // the names of the functions: the space and whether it is the derivative
  static const struct {
    const char *prefix;
    int space, derivative;
  } names[] = {{"BF_H1Legendre_", 0, 0},        {"BF_GradH1Legendre_", 0, 1},
               {"BF_HcurlLegendre_", 1, 0},     {"BF_CurlHcurlLegendre_", 1, 1},
               {"BF_HdivLegendre_", 2, 0},      {"BF_DivHdivLegendre_", 2, 1},
               {"BF_L2Legendre_", 3, 0}};
  const char *s = nullptr;
  int sp = 0, derivative = 0;
  for(auto &n : names) {
    if(!strncmp(Name, n.prefix, strlen(n.prefix))) {
      s = Name + strlen(n.prefix);
      sp = n.space;
      derivative = n.derivative;
    }
  }
  if(!s) return false;
  int type = (s[0] == 'N') ? 0 : (s[0] == 'E') ? 1 : (s[0] == 'F') ? 2 :
             (s[0] == 'V') ? 3 : -1;
  if(type < 0) return false;
  // the position can follow an underscore, as in the names built in a loop
  // (BF_HcurlLegendre_E~{k})
  s += (s[1] == '_') ? 2 : 1;
  if(!s[0] || strlen(s) > 4) return false;
  for(const char *c = s; *c; c++)
    if(*c < '0' || *c > '9') return false;
  int position = atoi(s);

#if defined(HAVE_GMSH_HIERARCHICAL) && defined(HAVE_KERNEL)
  Space space = (Space)sp;
  // the entities bearing functions: all of them for H1, from the edges for
  // H(curl), from the facets for H(div), and the elements for L2
  if(type < ((space == L2) ? 3 : (int)space)) {
    Message::Error("No function '%s': wrong type of entity for this space",
                   Name);
    return false;
  }
  // the functions are defined on the entity of the lowest dimension bearing
  // them and on the elements it bounds
  const int lin = LINE | LINE_2 | LINE_3 | LINE_4;
  const int tri = TRIANGLE | TRIANGLE_2 | TRIANGLE_3 | TRIANGLE_4;
  const int tet = TETRAHEDRON | TETRAHEDRON_2 | TETRAHEDRON_3 | TETRAHEDRON_4;
  if(space == L2)
    *ElementType = lin | tri | tet;
  else
    *ElementType = (type == 0) ? (POINT_ELEMENT | lin | tri | tet) :
                   (type == 1) ? (lin | tri | tet) :
                   (type == 2) ? (tri | tet) :
                                 tet;
  // the order of the function in Gmsh, on the elements of the lowest dimension
  // on which it is a function of the space (the tetrahedra for L2, where the
  // order of a function depends on the dimension of the element)
  Family *family = getFamily(
    space, (space == L2 || space == HDIV) ? TETRAHEDRON :
           (type < 2) ? LINE :
           (type == 2) ? TRIANGLE :
                         TETRAHEDRON);
  bool kernel;
  int order = family->order(type, position, kernel);
  if(order < 0) {
    Message::Error("No function '%s' up to order %d", Name, MAX_ORDER);
    return false;
  }
  void (*f)() = getFunction(space, 0, type, position);
  // L2 has no derivative: the function itself, as BF_Volume
  void (*df)() = (space == L2) ? f : getFunction(space, 1, type, position);
  if(!f || !df) return false;
  *Function = derivative ? df : f;
  *dFunction = (space == L2) ? f : derivative ? (void (*)())BF_Zero : df;
  *dInvFunction = derivative ? f : (void (*)())BF_Zero;
  // The orders of GetDP. For H1 and L2, the order in Gmsh. For H(curl) and
  // H(div), 0.5 for the Whitney functions, p for the functions of order p in
  // the kernel of the derivative (the gradients and the curls), which complete
  // the polynomials of degree p, and p - 0.5 for the other functions of order p
  // (see the function space types in Gmsh). One order less for the derivatives.
  double o = (space == H1 || space == L2) ? order :
             kernel ? order :
             (order > 0) ? order - 0.5 :
                           0.5;
  if(derivative) o = (o >= 1.) ? o - 1. : 0.;
  *Order = o;
  // the functions are oriented by Gmsh, from the tags of the nodes
  *Orient = 0;
  return true;
#else
  (void)sp;
  (void)derivative;
  (void)position;
  Message::Error("'%s' requires the HierarchicalBasis module of Gmsh",
                 Name);
  return false;
#endif
}
