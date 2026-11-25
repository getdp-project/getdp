// (curl u, curl v) + (u, v) = (f, v) - <n x curl u, v>, with a known u, in
// H(curl) of order p
DefineConstant[ p = 2, dim = 2, whitney = 0 ];
Include "legendre_common.pro";
Function {
  If(per == 2 && dim == 2)
    u0[] = Vector[Sin[2*Pi*Y[]], Sin[2*Pi*X[]], 0];
    g[] = 2*Pi*Vector[Cos[2*Pi*X[]]*Sin[2*Pi*Y[]], Sin[2*Pi*X[]]*Cos[2*Pi*Y[]], 0];
    cu[] = Vector[0, 0, 2*Pi*Cos[2*Pi*X[]] - 2*Pi*Cos[2*Pi*Y[]]];
    f[] = (4*Pi^2+1)*u0[] + g[];
  ElseIf(per == 2)
    u0[] = Vector[Sin[2*Pi*Y[]+Z[]], Sin[2*Z[]+2*Pi*X[]], Sin[2*Pi*X[]+2*Pi*Y[]]];
    g[] = Vector[2*Pi*Cos[2*Pi*X[]]*Sin[2*Pi*Y[]]*Sin[Z[]],
                 2*Pi*Sin[2*Pi*X[]]*Cos[2*Pi*Y[]]*Sin[Z[]],
                 Sin[2*Pi*X[]]*Sin[2*Pi*Y[]]*Cos[Z[]]];
    cu[] = Vector[2*Pi*Cos[2*Pi*X[]+2*Pi*Y[]] - 2*Cos[2*Z[]+2*Pi*X[]],
                  Cos[2*Pi*Y[]+Z[]] - 2*Pi*Cos[2*Pi*X[]+2*Pi*Y[]],
                  2*Pi*Cos[2*Z[]+2*Pi*X[]] - 2*Pi*Cos[2*Pi*Y[]+Z[]]];
    f[] = Vector[(4*Pi^2+2)*Sin[2*Pi*Y[]+Z[]], (4*Pi^2+5)*Sin[2*Z[]+2*Pi*X[]],
                 (8*Pi^2+1)*Sin[2*Pi*X[]+2*Pi*Y[]]] + g[];
  ElseIf(per && dim == 2)
    u0[] = Vector[Sin[2*Y[]], Sin[2*Pi*X[]], 0];
    g[] = Vector[2*Pi*Cos[2*Pi*X[]]*Sin[Y[]], Sin[2*Pi*X[]]*Cos[Y[]], 0];
    cu[] = Vector[0, 0, 2*Pi*Cos[2*Pi*X[]] - 2*Cos[2*Y[]]];
    f[] = Vector[5*Sin[2*Y[]], (4*Pi^2+1)*Sin[2*Pi*X[]], 0] + g[];
  ElseIf(per)
    u0[] = Vector[Sin[2*Y[]+Z[]], Sin[2*Z[]+2*Pi*X[]], Sin[2*Pi*X[]+Y[]]];
    g[] = Vector[2*Pi*Cos[2*Pi*X[]]*Sin[Y[]]*Sin[Z[]],
                 Sin[2*Pi*X[]]*Cos[Y[]]*Sin[Z[]],
                 Sin[2*Pi*X[]]*Sin[Y[]]*Cos[Z[]]];
    cu[] = Vector[Cos[2*Pi*X[]+Y[]] - 2*Cos[2*Z[]+2*Pi*X[]],
                  Cos[2*Y[]+Z[]] - 2*Pi*Cos[2*Pi*X[]+Y[]],
                  2*Pi*Cos[2*Z[]+2*Pi*X[]] - 2*Cos[2*Y[]+Z[]]];
    f[] = Vector[6*Sin[2*Y[]+Z[]], (4*Pi^2+5)*Sin[2*Z[]+2*Pi*X[]],
                 (4*Pi^2+2)*Sin[2*Pi*X[]+Y[]]] + g[];
  ElseIf(dim == 2)
    u0[] = Vector[Sin[2*Y[]], Sin[2*X[]], 0];
    g[] = Vector[Cos[X[]]*Sin[Y[]], Sin[X[]]*Cos[Y[]], 0];
    cu[] = Vector[0, 0, 2*Cos[2*X[]] - 2*Cos[2*Y[]]];
    f[] = 5*u0[] + g[];
  Else
    u0[] = Vector[Sin[2*Y[]+Z[]], Sin[2*Z[]+X[]], Sin[2*X[]+Y[]]];
    g[] = Vector[Cos[X[]]*Sin[Y[]]*Sin[Z[]], Sin[X[]]*Cos[Y[]]*Sin[Z[]],
                 Sin[X[]]*Sin[Y[]]*Cos[Z[]]];
    cu[] = Vector[Cos[2*X[]+Y[]] - 2*Cos[2*Z[]+X[]],
                  Cos[2*Y[]+Z[]] - 2*Cos[2*X[]+Y[]],
                  Cos[2*Z[]+X[]] - 2*Cos[2*Y[]+Z[]]];
    f[] = 6*u0[] + g[];
  EndIf
  uex[] = u0[] + g[];
}
// number of functions per edge (after the Whitney function), triangle,
// quadrangle, tetrahedron, hexahedron and prism
nE = p; nTri = (p-1)*(p+1); nQua = 2*p*(p+1);
nTet = (p-2)*(p-1)*(p+1)/2; nHex = 3*p^2*(p+1); nPri = 3*(p-1)*p*(p+1)/2;
FunctionSpace {
  { Name H; Type Form1;
    BasisFunction {
      If(whitney)
        { Name se; NameOfCoef ue; Function BF_Edge; Support Dom; Entity EdgesOf[All]; }
      Else
        { Name se; NameOfCoef ue; Function BF_HcurlLegendre_E0; Support Dom; Entity EdgesOf[All]; }
      EndIf
      If(nE > 0)
        For k In {1:nE}
          { Name se~{k}; NameOfCoef ue~{k}; Function BF_HcurlLegendre_E~{k}; Support Dom; Entity EdgesOf[All]; }
        EndFor
      EndIf
      If(nTri > 0)
        For k In {0:nTri-1}
          { Name st~{k}; NameOfCoef ut~{k}; Function BF_HcurlLegendre_FTri~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
      EndIf
      If(nQua > 0)
        For k In {0:nQua-1}
          { Name sq~{k}; NameOfCoef uq~{k}; Function BF_HcurlLegendre_FQua~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
      EndIf
      If(nTet > 0)
        For k In {0:nTet-1}
          { Name svt~{k}; NameOfCoef uvt~{k}; Function BF_HcurlLegendre_VTet~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nHex > 0)
        For k In {0:nHex-1}
          { Name svh~{k}; NameOfCoef uvh~{k}; Function BF_HcurlLegendre_VHex~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nPri > 0)
        For k In {0:nPri-1}
          { Name svp~{k}; NameOfCoef uvp~{k}; Function BF_HcurlLegendre_VPri~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
    }
    Constraint {
      If(link)
        { NameOfCoef ue; EntityType EdgesOf; NameOfConstraint Periodic; }
        If(nE > 0)
          For k In {1:nE}
            { NameOfCoef ue~{k}; EntityType EdgesOf; NameOfConstraint Periodic; }
          EndFor
        EndIf
        If(dim == 3)
          If(nTri > 0)
            For k In {0:nTri-1}
              { NameOfCoef ut~{k}; EntityType FacetsOf; NameOfConstraint Periodic; }
            EndFor
          EndIf
        EndIf
        If(dim == 3)
          If(nQua > 0)
            For k In {0:nQua-1}
              { NameOfCoef uq~{k}; EntityType FacetsOf; NameOfConstraint Periodic; }
            EndFor
          EndIf
        EndIf
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
  { Name R; System { { Name A; NameOfFormulation F; } }
    Operation { Generate[A]; Solve[A]; SaveSolution[A]; } }
}
PostProcessing {
  { Name P; NameOfFormulation F;
    Quantity {
      { Name eu; Value { Integral { [ SquNorm[{u} - uex[]] ]; In Omega; Jacobian Vol; Integration I; } } }
      { Name ed; Value { Integral { [ SquNorm[{d u} - cu[]] ]; In Omega; Jacobian Vol; Integration I; } } }
    }
  }
}
PostOperation {
  { Name P; NameOfPostProcessing P;
    Operation {
      Print[ eu[Omega], OnGlobal, Format Table, File "eu.txt" ];
      Print[ ed[Omega], OnGlobal, Format Table, File "ed.txt" ];
    }
  }
}
