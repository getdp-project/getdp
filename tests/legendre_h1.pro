// (grad u, grad v) + (u, v) = (f, v) + <n . grad u, v>, with a known u, in H1
// of order p
DefineConstant[ p = 2, dim = 2, node = 0 ];
Include "legendre_common.pro";
Function {
  If(per == 2 && dim == 2)
    uex[] = Sin[2*Pi*X[]]*Cos[2*Pi*Y[]];
    gu[] = 2*Pi*Vector[Cos[2*Pi*X[]]*Cos[2*Pi*Y[]], -Sin[2*Pi*X[]]*Sin[2*Pi*Y[]], 0];
    f[] = (8*Pi^2+1)*uex[];
  ElseIf(per == 2)
    uex[] = Sin[2*Pi*X[]]*Cos[2*Pi*Y[]]*Cos[Z[]];
    gu[] = Vector[2*Pi*Cos[2*Pi*X[]]*Cos[2*Pi*Y[]]*Cos[Z[]],
                  -2*Pi*Sin[2*Pi*X[]]*Sin[2*Pi*Y[]]*Cos[Z[]],
                  -Sin[2*Pi*X[]]*Cos[2*Pi*Y[]]*Sin[Z[]]];
    f[] = (8*Pi^2+2)*uex[];
  ElseIf(per && dim == 2)
    uex[] = Sin[2*Pi*X[]]*Cos[Y[]];
    gu[] = Vector[2*Pi*Cos[2*Pi*X[]]*Cos[Y[]], -Sin[2*Pi*X[]]*Sin[Y[]], 0];
    f[] = (4*Pi^2+2)*uex[];
  ElseIf(per)
    uex[] = Sin[2*Pi*X[]]*Cos[Y[]]*Cos[Z[]];
    gu[] = Vector[2*Pi*Cos[2*Pi*X[]]*Cos[Y[]]*Cos[Z[]],
                  -Sin[2*Pi*X[]]*Sin[Y[]]*Cos[Z[]],
                  -Sin[2*Pi*X[]]*Cos[Y[]]*Sin[Z[]]];
    f[] = (4*Pi^2+3)*uex[];
  ElseIf(dim == 2)
    uex[] = Sin[2*X[]+Y[]];
    gu[] = Vector[2, 1, 0] * Cos[2*X[]+Y[]];
    f[] = 6*uex[];
  Else
    uex[] = Sin[2*X[]+Y[]+Z[]];
    gu[] = Vector[2, 1, 1] * Cos[2*X[]+Y[]+Z[]];
    f[] = 7*uex[];
  EndIf
}
FunctionSpace {
  { Name H; Type Form0;
    BasisFunction {
      If(node == 0)
        { Name s; NameOfCoef u; Function BF_H1Legendre; Order p; Support Dom; Entity Auto; }
      Else
        // nodal functions (the first order ones whatever the order of the mesh
        // if node == 1, those of the order of the mesh otherwise), with the
        // functions of the space borne by the edges, the facets and the volumes
        If(node == 1)
          { Name s; NameOfCoef u; Function BF_Node_1N; Support Dom; Entity NodesOf[All]; }
        Else
          { Name s; NameOfCoef u; Function BF_Node; Support Dom; Entity NodesOf[All]; }
        EndIf
        { Name se; NameOfCoef ue; Function BF_H1Legendre; Order p; Support Dom; Entity EdgesOf[All]; }
        { Name sf; NameOfCoef uf; Function BF_H1Legendre; Order p; Support Dom; Entity FacetsOf[All]; }
        { Name sv; NameOfCoef uv; Function BF_H1Legendre; Order p; Support Dom; Entity VolumesOf[All]; }
      EndIf
    }
    Constraint {
      If(link)
        { NameOfCoef u; EntityType Auto; NameOfConstraint Periodic; }
        If(node)
          { NameOfCoef ue; EntityType Auto; NameOfConstraint Periodic; }
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
      Galerkin { [ -n[] * gu[], {u} ]; In Bnd; Jacobian Sur; Integration I; }
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
      { Name ed; Value { Integral { [ SquNorm[{d u} - gu[]] ]; In Omega; Jacobian Vol; Integration I; } } }
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
