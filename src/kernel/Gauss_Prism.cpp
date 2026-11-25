// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <math.h>
#include "Gauss.h"
#include "Gauss_Prism.h"
#include "Message.h"

/* Gauss integration over a prism */

void Gauss_Prism(int Nbr_Points, int Num, double *u, double *v, double *w,
                 double *wght)
{
  switch(Nbr_Points) {
  case 6:
    *u = upri6[Num];
    *v = vpri6[Num];
    *w = wpri6[Num];
    *wght = ppri6[Num];
    break;

  case 9:
    *u = upri9[Num];
    *v = vpri9[Num];
    *w = wpri9[Num];
    *wght = ppri9[Num];
    break;

  case 21:
    *u = upri21[Num];
    *v = vpri21[Num];
    *w = wpri21[Num];
    *wght = ppri21[Num];
    break;

  case 42:
    *u = upri42[Num];
    *v = vpri42[Num];
    *w = wpri42[Num];
    *wght = ppri42[Num];
    break;

  default:
    Message::Error("Wrong number of Gauss points for Prism: "
                   "valid choices: 6, 9, 21, 42");
    break;
  }
}

/* Gauss-Legendre integration over a prism, with n^3 points: the product of
   the degenerate scheme with n^2 points on the triangle and of the scheme with
   n points on the line; the other numbers of points are those of Gauss_Prism,
   which the prisms used to get with the GaussLegendre type */

void GaussLegendre_Prism(int Nbr_Points, int Num, double *u, double *v,
                         double *w, double *wght)
{
  int nb = (int)(cbrt((double)Nbr_Points) + 0.5);

  if(nb * nb * nb != Nbr_Points) {
    Gauss_Prism(Nbr_Points, Num, u, v, w, wght);
    return;
  }
  if(nb > MAX_LINE_POINTS) {
    Message::Error("Number of points should be n^3 with n in [1,%d]",
                   MAX_LINE_POINTS);
    return;
  }

  double dum, wt1, wt2;
  GaussLegendre_Triangle(nb * nb, Num / nb, u, v, &dum, &wt1);
  Gauss_Line(nb, Num % nb, w, &dum, &dum, &wt2);
  *wght = wt1 * wt2;
}
