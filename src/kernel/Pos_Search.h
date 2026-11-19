// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef POS_SEARCH_H
#define POS_SEARCH_H

#include "ProData.h"
#include "GeoData.h"
#include "ListUtils.h"

struct ElementBox {
  double Xmin, Xmax;
  double Ymin, Ymax;
  double Zmin, Zmax;
};

struct PointElement {
  double d;
  double xp, yp, zp;
  int ElementIndex;
};

void Free_ElementSearch(struct GeoData *GeoData);
void Pos_SearchInPartitionOfThisRank(bool own);
int DimensionOfElementType(int Type);

void InWhichElement(struct GeoData *GeoData, List_T *ExcludeRegion,
                    struct Element *Element, int Dim, double x, double y,
                    double z, double *u, double *v, double *w);

int PointInElement(struct Element *Element, List_T *ExcludeRegion_L, double x,
                   double y, double z, double *u, double *v, double *w,
                   double tol);

void xyz2uvwInAnElement(struct Element *Element, double x, double y, double z,
                        double *u, double *v, double *w);

#endif
