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
// number of functions per edge, triangle, quadrangle, tetrahedron, hexahedron
// and prism
nE = p-1; nTri = (p-2)*(p-1)/2; nQua = (p-1)^2;
nTet = (p-3)*(p-2)*(p-1)/6; nHex = (p-1)^3; nPri = (p-2)*(p-1)^2/2;
FunctionSpace {
  { Name H; Type Form0;
    BasisFunction {
      If(node == 0)
        { Name sn; NameOfCoef un; Function BF_H1Legendre_N0; Support Dom; Entity NodesOf[All]; }
      ElseIf(node == 1)
        // first order functions, whatever the order of the mesh
        { Name sn; NameOfCoef un; Function BF_Node_1N; Support Dom; Entity NodesOf[All]; }
      Else
        // functions of the order of the mesh
        { Name sn; NameOfCoef un; Function BF_Node; Support Dom; Entity NodesOf[All]; }
      EndIf
      If(nE > 0)
        For k In {0:nE-1}
          { Name se~{k}; NameOfCoef ue~{k}; Function BF_H1Legendre_E~{k}; Support Dom; Entity EdgesOf[All]; }
        EndFor
      EndIf
      If(nTri > 0)
        For k In {0:nTri-1}
          { Name st~{k}; NameOfCoef ut~{k}; Function BF_H1Legendre_FTri~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
      EndIf
      If(nQua > 0)
        For k In {0:nQua-1}
          { Name sq~{k}; NameOfCoef uq~{k}; Function BF_H1Legendre_FQua~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
      EndIf
      If(nTet > 0)
        For k In {0:nTet-1}
          { Name svt~{k}; NameOfCoef uvt~{k}; Function BF_H1Legendre_VTet~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nHex > 0)
        For k In {0:nHex-1}
          { Name svh~{k}; NameOfCoef uvh~{k}; Function BF_H1Legendre_VHex~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nPri > 0)
        For k In {0:nPri-1}
          { Name svp~{k}; NameOfCoef uvp~{k}; Function BF_H1Legendre_VPri~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
    }
    Constraint {
      If(link)
        { NameOfCoef un; EntityType NodesOf; NameOfConstraint Periodic; }
        If(nE > 0)
          For k In {0:nE-1}
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
