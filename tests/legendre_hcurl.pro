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
FunctionSpace {
  { Name H; Type Form1;
    BasisFunction {
      If(!whitney)
        { Name s; NameOfCoef u; Function BF_HcurlLegendre; Order p; Support Dom; Entity Auto; }
      Else
        // the Whitney functions, with the other functions of the edges one by
        // one, and the functions of the space borne by the facets and the
        // volumes
        { Name s; NameOfCoef u; Function BF_Edge; Support Dom; Entity EdgesOf[All]; }
        For k In {1:p}
          { Name se~{k}; NameOfCoef ue~{k}; Function BF_HcurlLegendre_E~{k}; Support Dom; Entity EdgesOf[All]; }
        EndFor
        { Name sf; NameOfCoef uf; Function BF_HcurlLegendre; Order p; Support Dom; Entity FacetsOf[All]; }
        { Name sv; NameOfCoef uv; Function BF_HcurlLegendre; Order p; Support Dom; Entity VolumesOf[All]; }
      EndIf
    }
    Constraint {
      If(link)
        { NameOfCoef u; EntityType Auto; NameOfConstraint Periodic; }
        If(whitney)
          For k In {1:p}
            { NameOfCoef ue~{k}; EntityType Auto; NameOfConstraint Periodic; }
          EndFor
          { NameOfCoef uf; EntityType Auto; NameOfConstraint Periodic; }
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
