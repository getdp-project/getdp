// (grad u, grad v) + (u, v) = (f, v) + <n . grad u, v>, with a known u
DefineConstant[ p = 2, dim = 2, node = 0 ];
Include "legendre_common.pro";
Function {
  If(dim == 2)
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
      If(node)
        { Name sn; NameOfCoef un; Function BF_Node; Support Dom; Entity NodesOf[All]; }
      Else
        { Name sn; NameOfCoef un; Function BF_H1Legendre_N0; Support Dom; Entity NodesOf[All]; }
      EndIf
      If(p > 1)
        For k In {0:p-2}
          { Name se~{k}; NameOfCoef ue~{k}; Function BF_H1Legendre_E~{k}; Support Dom; Entity EdgesOf[All]; }
        EndFor
      EndIf
      If(p > 2)
        For k In {0:(p-2)*(p-1)/2-1}
          { Name sf~{k}; NameOfCoef uf~{k}; Function BF_H1Legendre_F~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
      EndIf
      If(dim == 3 && p > 3)
        For k In {0:(p-3)*(p-2)*(p-1)/6-1}
          { Name sv~{k}; NameOfCoef uv~{k}; Function BF_H1Legendre_V~{k}; Support Dom; Entity VolumesOf[All]; }
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
      { Name ec; Value { Integral { [ SquNorm[{d u} - gu[]] ]; In Omega; Jacobian Vol; Integration I; } } }
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
