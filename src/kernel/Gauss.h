// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef GAUSS_H
#define GAUSS_H

#define GAUSS_ARGS                                                             \
  int Nbr_Points, int Num_Point, double *u, double *v, double *w, double *wght

// The quadrature rules, with the degree of the polynomials that each one
// integrates exactly: the total degree on the simplices and on the pyramid,
// the degree in each variable on the quadrangle and the hexahedron, and both
// on the prism (the total degree in the plane of the triangles and the degree
// along the axis). The degrees were measured by integrating the monomials on
// the reference elements.

void Gauss_Point(GAUSS_ARGS);

// Gauss-Legendre rules with n = 1 to 20 points: degree 2n - 1
void Gauss_Line(GAUSS_ARGS);

// 1, 3, 4, 6, 7, 12, 13 and 16 points: degrees 1, 2, 3, 4, 5, 6, 7 and 8
void Gauss_Triangle(GAUSS_ARGS);
// n^2 points (the product rule of the square, collapsed): degree 2n - 2
void GaussLegendre_Triangle(GAUSS_ARGS);
void GaussSingularR_Triangle(GAUSS_ARGS);

// 1, 3, 4 and 7 points: degrees 1, 1, 3 and 3 in each variable (total
// degrees 1, 2, 3 and 5)
void Gauss_Quadrangle(GAUSS_ARGS);
// n^2 points: degree 2n - 1 in each variable
void GaussLegendre_Quadrangle(GAUSS_ARGS);
void GaussSingularR_Quadrangle(GAUSS_ARGS);

// 1, 4, 5, 15, 16, 17 and 29 points: degrees 1, 2, 3, 5, 4, 5 and 6
void Gauss_Tetrahedron(GAUSS_ARGS);
// n^3 points (the product rule of the cube, collapsed): degree 1 for n = 1
// and 2n - 3 for n >= 2
void GaussLegendre_Tetrahedron(GAUSS_ARGS);

// 6, 14, 34 and 77 points: degrees 1, 1, 3 and 3 in each variable (total
// degrees 3, 5, 7 and 9)
void Gauss_Hexahedron(GAUSS_ARGS);
// n^3 points: degree 2n - 1 in each variable
void GaussLegendre_Hexahedron(GAUSS_ARGS);

// 6, 9, 21 and 42 points: degrees 2, 2, 5 and 5 (total degree in the plane of
// the triangles, and degree along the axis)
void Gauss_Prism(GAUSS_ARGS);
// n^3 points: degree 2n - 2 (idem)
void GaussLegendre_Prism(GAUSS_ARGS);

// 8 points: degree 3
void Gauss_Pyramid(GAUSS_ARGS);

// The rule with the fewest points that is exact for the polynomials of the
// given degree on the elements of the given type (see Gauss_Auto.cpp); NULL
// if there is none
struct Quadrature *Get_AutomaticQuadrature(int ElementType, int Degree);

void Collocation_Line(GAUSS_ARGS);
void Collocation_Triangle(GAUSS_ARGS);

#undef GAUSS_ARGS

#define MAX_LINE_POINTS 100

void GaussLegendre(double x1, double x2, double x[], double w[], int n);

#endif
