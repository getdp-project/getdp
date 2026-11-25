// (curl u, curl v) + (u, v) = (f, v) - <n x curl u, v>, with a known u
DefineConstant[ p = 2, dim = 2, whitney = 1, ng = 0 ];
Group {
  Omega = Region[1]; Bnd = Region[2];
  Dom = Region[{Omega, Bnd}];
}
Function {
  If(dim == 2)
    u0[] = Vector[Sin[2*Y[]], Sin[2*X[]], 0];
    g[] = Vector[Cos[X[]]*Sin[Y[]], Sin[X[]]*Cos[Y[]], 0];
    cu[] = Vector[0, 0, 2*Cos[2*X[]] - 2*Cos[2*Y[]]];
    f[] = 5*u0[] + g[];
    n[] = Vector[(Fabs[X[]-1] < 1e-6) - (Fabs[X[]] < 1e-6),
                 (Fabs[Y[]-1] < 1e-6) - (Fabs[Y[]] < 1e-6), 0];
  Else
    u0[] = Vector[Sin[2*Y[]+Z[]], Sin[2*Z[]+X[]], Sin[2*X[]+Y[]]];
    g[] = Vector[Cos[X[]]*Sin[Y[]]*Sin[Z[]], Sin[X[]]*Cos[Y[]]*Sin[Z[]],
                 Sin[X[]]*Sin[Y[]]*Cos[Z[]]];
    cu[] = Vector[Cos[2*X[]+Y[]] - 2*Cos[2*Z[]+X[]],
                  Cos[2*Y[]+Z[]] - 2*Cos[2*X[]+Y[]],
                  Cos[2*Z[]+X[]] - 2*Cos[2*Y[]+Z[]]];
    f[] = 6*u0[] + g[];
    n[] = Vector[(Fabs[X[]-1] < 1e-6) - (Fabs[X[]] < 1e-6),
                 (Fabs[Y[]-1] < 1e-6) - (Fabs[Y[]] < 1e-6),
                 (Fabs[Z[]-1] < 1e-6) - (Fabs[Z[]] < 1e-6)];
  EndIf
  uex[] = u0[] + g[];
}
Jacobian {
  { Name Vol; Case { { Region Dom; Jacobian Vol; } } }
  { Name Sur; Case { { Region Dom; Jacobian Sur; } } }
}
Integration {
  { Name I; Case { { Type Gauss; Case {
    { GeoElement Line; NumberOfPoints 10; }
    { GeoElement Triangle; NumberOfPoints 16; }
    { GeoElement Tetrahedron; NumberOfPoints 29; } } } } }
}
FunctionSpace {
  { Name H; Type Form1;
    BasisFunction {
      If(whitney)
        { Name se; NameOfCoef ae; Function BF_Edge; Support Dom; Entity EdgesOf[All]; }
      Else
        { Name se; NameOfCoef ae; Function BF_HcurlLegendre_E0; Support Dom; Entity EdgesOf[All]; }
      EndIf
      For k In {1:p}
        If(!ng || k < p)
        { Name se~{k}; NameOfCoef ae~{k}; Function BF_HcurlLegendre_E~{k}; Support Dom; Entity EdgesOf[All]; }
        EndIf
      EndFor
      If(p > 1)
        For q In {2:p}
        For k In {(q-2)*q:(q-1)*(q+1)-1}
          // without the gradients of the highest order: the first q - 1
          If(!ng || q < p || k >= (q-2)*q + q-1)
          { Name sf~{k}; NameOfCoef af~{k}; Function BF_HcurlLegendre_F~{k}; Support Dom; Entity FacetsOf[All]; }
          EndIf
        EndFor
        EndFor
      EndIf
      If(dim == 3 && p > 2)
        For k In {0:(p-2)*(p-1)*(p+1)/2-1}
          If(!ng || k >= 1) { Name sv~{k}; NameOfCoef av~{k}; Function BF_HcurlLegendre_V~{k}; Support Dom; Entity VolumesOf[All]; } EndIf
        EndFor
      EndIf
    }
  }
}
Formulation {
  { Name F; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace H; } }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ Dof{u}, {u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ -f[], {u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ n[] /\ cu[], {u} ]; In Bnd; Jacobian Sur; Integration I; }
    }
  }
}
Resolution {
  { Name R;
    System { { Name A; NameOfFormulation F; } }
    Operation { Generate[A]; Solve[A]; SaveSolution[A]; }
  }
}
PostProcessing {
  { Name P; NameOfFormulation F;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Omega; Jacobian Vol; } } }
      { Name eu; Value { Integral { [ SquNorm[{u} - uex[]] ]; In Omega; Jacobian Vol; Integration I; } } }
      { Name ec; Value { Integral { [ SquNorm[{d u} - cu[]] ]; In Omega; Jacobian Vol; Integration I; } } }
    }
  }
}
PostOperation {
  { Name P; NameOfPostProcessing P;
    Operation {
      Print[ eu[Omega], OnGlobal, Format Table, File "eu.txt" ];
      Print[ ec[Omega], OnGlobal, Format Table, File "ec.txt" ];
    }
  }
}
