// common definitions of the tests of the Legendre basis functions on the unit
// square (dim = 2) or cube (dim = 3) of legendre.geo
// per: 1 on a mesh periodic along x, with the unknowns at x = 1 (region 4)
// linked to those at x = 0 (region 3); 2 on a mesh also periodic along y, with
// the unknowns at y = 1 (region 6) linked to those at y = 0 (region 5); Bnd is
// the rest of the boundary (link = 0 solves the same problem without the
// links, with the boundary terms on the whole boundary, as a reference)
DefineConstant[ per = 0 ];
DefineConstant[ link = per ];
Group {
  Omega = Region[1]; Left = Region[3]; Right = Region[4];
  Bottom = Region[5]; Top = Region[6];
  If(per && !link)
    Bnd = Region[{2, 3, 4, 5, 6}];
  Else
    Bnd = Region[2];
  EndIf
  Dom = Region[{Omega, Bnd, Left, Right, Bottom, Top}];
}
// (only with periodicity: a Link constraint makes all the ranks keep its regions,
// and the mesh files of the partitions are then not used)
If(per)
Constraint {
  { Name Periodic; Case {
      { Region Right; Type Link; RegionRef Left; Coefficient 1;
        Function Vector[X[]-1, Y[], Z[]]; }
      If(per == 2)
        { Region Top; Type Link; RegionRef Bottom; Coefficient 1;
          Function Vector[X[], Y[]-1, Z[]]; }
      EndIf
    } }
}
EndIf
Function {
  // the outer normal
  If(dim == 2)
    n[] = Vector[(Fabs[X[]-1] < 1e-6) - (Fabs[X[]] < 1e-6),
                 (Fabs[Y[]-1] < 1e-6) - (Fabs[Y[]] < 1e-6), 0];
  Else
    n[] = Vector[(Fabs[X[]-1] < 1e-6) - (Fabs[X[]] < 1e-6),
                 (Fabs[Y[]-1] < 1e-6) - (Fabs[Y[]] < 1e-6),
                 (Fabs[Z[]-1] < 1e-6) - (Fabs[Z[]] < 1e-6)];
  EndIf
}
Jacobian {
  { Name Vol; Case { { Region All; Jacobian Vol; } } }
  { Name Sur; Case { { Region All; Jacobian Sur; } } }
}
DefineConstant[ ng = 8 ]; // Gauss points per direction
Integration {
  { Name I; Case { { Type GaussLegendre; Case {
    { GeoElement Line; NumberOfPoints ng; }
    { GeoElement Triangle; NumberOfPoints ng^2; }
    { GeoElement Quadrangle; NumberOfPoints ng^2; }
    { GeoElement Tetrahedron; NumberOfPoints ng^3; }
    { GeoElement Hexahedron; NumberOfPoints ng^3; }
    { GeoElement Prism; NumberOfPoints ng^3; }
    // second order meshes
    { GeoElement Line2; NumberOfPoints ng; }
    { GeoElement Triangle2; NumberOfPoints ng^2; }
    { GeoElement Quadrangle2; NumberOfPoints ng^2; }
    { GeoElement Tetrahedron2; NumberOfPoints ng^3; }
    { GeoElement Hexahedron2; NumberOfPoints ng^3; }
    { GeoElement Prism2; NumberOfPoints ng^3; } } } } }
}
