// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

// Automatic choice of a quadrature rule: the rule with the fewest points that
// integrates exactly the polynomials of a given degree on an element (the
// total degree on the simplices and on the pyramid, the degree in each
// variable on the quadrangle and the hexahedron, and both on the prism: the
// total degree in the plane of the triangles and the degree along the axis).
//
// The rules are those of Gmsh when GetDP is built with Gmsh (symmetric rules
// for the simplices, the prisms and the pyramids, of any degree, with many
// fewer points than the product rules), and the rules of GetDP otherwise (see
// Gauss.h for their degrees).

#include <map>
#include <utility>
#include <vector>
#include "GetDPConfig.h"
#include "ProData.h"
#include "Gauss.h"
#include "Message.h"

#if defined(HAVE_GMSH)
#include <gmsh/GaussIntegration.h>
#endif

namespace {

  enum Shape { PNT, LIN, TRI, QUA, TET, HEX, PRI, PYR, NUM_SHAPES };

  int getShape(int ElementType)
  {
    switch(ElementType) {
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
    case PYRAMID:
    case PYRAMID_2:
    case PYRAMID_2_13N:
    case PYRAMID_3: return PYR;
    }
    return -1;
  }

  typedef void (*Rule)(int, int, double *, double *, double *, double *);

#if defined(HAVE_GMSH)

  // The rules of Gmsh, by shape and number of points (a quadrature of GetDP
  // is a function giving the i-th point of the rule with a given number of
  // points)
  struct Points {
    std::vector<double> u, v, w, weight;
  };
  std::map<int, Points> gmshRules[NUM_SHAPES];

  template <int S> void GaussGmsh(int Nbr_Points, int Num, double *u, double *v,
                                  double *w, double *wght)
  {
    static const Points *last = nullptr;
    static int lastNbr = -1;
    if(Nbr_Points != lastNbr) {
      auto it = gmshRules[S].find(Nbr_Points);
      if(it == gmshRules[S].end()) {
        Message::Error("No automatic quadrature with %d points", Nbr_Points);
        *u = *v = *w = *wght = 0.;
        return;
      }
      last = &it->second;
      lastNbr = Nbr_Points;
    }
    *u = last->u[Num];
    *v = last->v[Num];
    *w = last->w[Num];
    *wght = last->weight[Num];
  }

  const Rule gmshRule[NUM_SHAPES] = {
    Gauss_Point,    GaussGmsh<LIN>, GaussGmsh<TRI>, GaussGmsh<QUA>,
    GaussGmsh<TET>, GaussGmsh<HEX>, GaussGmsh<PRI>, GaussGmsh<PYR>};

  // the rule of Gmsh for the degree; returns its number of points
  int getRule(int shape, int degree, Rule *rule)
  {
    *rule = gmshRule[shape];
    if(shape == PNT) return 1;
    int n = 0;
    IntPt *p = nullptr;
    switch(shape) {
    case LIN:
      n = getNGQLPts(degree);
      p = getGQLPts(degree);
      break;
    case TRI:
      n = getNGQTPts(degree);
      p = getGQTPts(degree);
      break;
    case QUA: // (the product rules: the degree is the one in each variable)
      n = getNGQQPts(degree, true);
      p = getGQQPts(degree, true);
      break;
    case TET:
      n = getNGQTetPts(degree);
      p = getGQTetPts(degree);
      break;
    case HEX:
      n = getNGQHPts(degree, true);
      p = getGQHPts(degree, true);
      break;
    case PRI:
      n = getNGQPriPts(degree);
      p = getGQPriPts(degree);
      break;
    case PYR:
      n = getNGQPyrPts(degree);
      p = getGQPyrPts(degree);
      break;
    }
    if(n <= 0 || !p) return 0;
    Points &points = gmshRules[shape][n];
    if(points.u.empty()) {
      points.u.resize(n);
      points.v.resize(n);
      points.w.resize(n);
      points.weight.resize(n);
      for(int i = 0; i < n; i++) {
        points.u[i] = p[i].pt[0];
        points.v[i] = p[i].pt[1];
        points.w[i] = p[i].pt[2];
        points.weight[i] = p[i].weight;
      }
    }
    return n;
  }

#else

  // the rule of GetDP for the degree (see Gauss.h); returns its number of
  // points
  int getRule(int shape, int degree, Rule *rule)
  {
    // the smallest of the n given rules with at least the degree, if any
    auto pick = [&](int n, const int points[], const int degrees[]) {
      for(int i = 0; i < n; i++)
        if(degrees[i] >= degree) return points[i];
      return 0;
    };
    int n, m;
    switch(shape) {
    case PNT:
      *rule = Gauss_Point;
      return 1;
    case LIN:
      *rule = Gauss_Line;
      n = degree / 2 + 1;
      return (n <= 20) ? n : 0;
    case TRI: {
      static const int points[] = {1, 3, 4, 6, 7, 12, 13, 16};
      static const int degrees[] = {1, 2, 3, 4, 5, 6, 7, 8};
      if((n = pick(8, points, degrees))) {
        *rule = Gauss_Triangle;
        return n;
      }
      *rule = GaussLegendre_Triangle;
      m = (degree + 3) / 2;
      return m * m;
    }
    case QUA:
      *rule = GaussLegendre_Quadrangle;
      m = degree / 2 + 1;
      return m * m;
    case TET: {
      static const int points[] = {1, 4, 5, 15, 29};
      static const int degrees[] = {1, 2, 3, 5, 6};
      if((n = pick(5, points, degrees))) {
        *rule = Gauss_Tetrahedron;
        return n;
      }
      *rule = GaussLegendre_Tetrahedron;
      m = (degree + 4) / 2;
      return m * m * m;
    }
    case HEX:
      *rule = GaussLegendre_Hexahedron;
      m = degree / 2 + 1;
      return m * m * m;
    case PRI: {
      static const int points[] = {6, 21};
      static const int degrees[] = {2, 5};
      if((n = pick(2, points, degrees))) {
        *rule = Gauss_Prism;
        return n;
      }
      *rule = GaussLegendre_Prism;
      m = (degree + 3) / 2;
      return m * m * m;
    }
    case PYR:
      *rule = Gauss_Pyramid;
      return (degree <= 3) ? 8 : 0;
    }
    return 0;
  }

#endif

} // namespace

struct Quadrature *Get_AutomaticQuadrature(int ElementType, int Degree)
{
  static std::map<std::pair<int, int>, struct Quadrature> quadratures;
  if(Degree < 0) Degree = 0;
  auto key = std::make_pair(ElementType, Degree);
  auto it = quadratures.find(key);
  if(it != quadratures.end()) return &it->second;

  int shape = getShape(ElementType);
  Rule rule = nullptr;
  int n = (shape >= 0) ? getRule(shape, Degree, &rule) : 0;
  if(n <= 0) {
    Message::Error("No automatic quadrature of degree %d for this type of "
                   "element (%d)", Degree, ElementType);
    return NULL;
  }
  struct Quadrature Q;
  Q.ElementType = ElementType;
  Q.NumberOfPoints = Q.MaxNumberOfPoints = n;
  Q.NumberOfDivisions = Q.MaxNumberOfDivisions = 1;
  Q.StoppingCriterion = 1.e-12;
  Q.Function = (void (*)())rule;
  return &quadratures.insert(std::make_pair(key, Q)).first->second;
}
