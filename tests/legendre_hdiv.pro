// q = - grad phi, div q = f in H(div) x L2, phi given weakly on the boundary:
// (q, t) - (phi, div t) = - <phi, t . n>, (div q, v) = (f, v)
// rt = 0: full H(div) of order p (BDM) with L2 of order p - 1 (p >= 1)
// rt = 1: without the curls of order p (RT of order p - 1) with L2 of order p - 1
DefineConstant[ p = 1, dim = 3, rt = 0, whitney = 0 ];
Include "legendre_common.pro";
Function {
  phiex[] = Sin[2*X[]+Y[]+Z[]];
  qex[] = -Vector[2, 1, 1] * Cos[2*X[]+Y[]+Z[]];
  f[] = 6*phiex[];
}
FunctionSpace {
  { Name Hdiv; Type Form2;
    BasisFunction {
      If(whitney)
        { Name sf; NameOfCoef qf; Function BF_Facet; Support Dom; Entity FacetsOf[All]; }
      Else
        { Name sf; NameOfCoef qf; Function BF_HdivLegendre_F0; Support Dom; Entity FacetsOf[All]; }
      EndIf
      // the facet functions of order o >= 1 are all curls, as are the first
      // o^2 - 1 volume functions of order o >= 2
      For o In {1:p}
        If(!rt || o < p)
        For k In {o*(o+1)/2:(o+1)*(o+2)/2-1}
          { Name sf~{k}; NameOfCoef qf~{k}; Function BF_HdivLegendre_F~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
        EndIf
      EndFor
      If(p > 1)
        For o In {2:p}
        For k In {(o-2)*o*(o+1)/2:(o-1)*(o+1)*(o+2)/2-1}
          If(!rt || o < p || k >= (o-2)*o*(o+1)/2 + o*o-1)
          { Name sv~{k}; NameOfCoef qv~{k}; Function BF_HdivLegendre_V~{k}; Support Dom; Entity VolumesOf[All]; }
          EndIf
        EndFor
        EndFor
      EndIf
    }
  }
  { Name L2; Type Form3;
    BasisFunction {
      For k In {0:p*(p+1)*(p+2)/6-1}
        { Name sv~{k}; NameOfCoef pv~{k}; Function BF_L2Legendre_V~{k}; Support Omega; Entity VolumesOf[All]; }
      EndFor
    }
  }
}
Formulation {
  { Name F; Type FemEquation;
    Quantity {
      { Name q; Type Local; NameOfSpace Hdiv; }
      { Name phi; Type Local; NameOfSpace L2; }
    }
    Equation {
      Galerkin { [ Dof{q}, {q} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ -Dof{phi}, {d q} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ phiex[] * n[], {q} ]; In Bnd; Jacobian Sur; Integration I; }
      Galerkin { [ Dof{d q}, {phi} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ -f[], {phi} ]; In Omega; Jacobian Vol; Integration I; }
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
      { Name eu; Value { Integral { [ SquNorm[{q} - qex[]] ]; In Omega; Jacobian Vol; Integration I; } } }
      { Name ec; Value { Integral { [ SquNorm[{phi} - phiex[]] ]; In Omega; Jacobian Vol; Integration I; } } }
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
