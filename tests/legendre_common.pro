Group {
  Omega = Region[1]; Bnd = Region[2];
  Dom = Region[{Omega, Bnd}];
}
Function {
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
Integration {
  { Name I; Case { { Type GaussLegendre; Case {
    { GeoElement Line; NumberOfPoints 10; }
    { GeoElement Triangle; NumberOfPoints 64; }
    { GeoElement Tetrahedron; NumberOfPoints 216; } } } } }
}
