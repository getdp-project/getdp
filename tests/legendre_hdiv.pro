// q = - grad phi, div q = f in H(div) x L2, phi given weakly on the boundary:
// (q, t) - (phi, div t) = - <phi, t . n>, (div q, v) = (f, v), with H(div) of
// order p; the divergence of the functions of order p has the order p on the
// hexahedra and prisms, and p - 1 on the tetrahedra (0 if p = 0)
DefineConstant[ p = 1, dim = 3, whitney = 0 ];
Include "legendre_common.pro";
Function {
  If(per)
    phiex[] = Sin[2*Pi*X[]]*Cos[Y[]]*Cos[Z[]];
    qex[] = -Vector[2*Pi*Cos[2*Pi*X[]]*Cos[Y[]]*Cos[Z[]],
                    -Sin[2*Pi*X[]]*Sin[Y[]]*Cos[Z[]],
                    -Sin[2*Pi*X[]]*Cos[Y[]]*Sin[Z[]]];
    f[] = (4*Pi^2+2)*phiex[];
  Else
    phiex[] = Sin[2*X[]+Y[]+Z[]];
    qex[] = -Vector[2, 1, 1] * Cos[2*X[]+Y[]+Z[]];
    f[] = 6*phiex[];
  EndIf
}
// number of functions per triangle and quadrangle (after the Whitney
// function), tetrahedron, hexahedron and prism
nTri = (p+1)*(p+2)/2 - 1; nQua = (p+1)^2 - 1;
nTet = (p-1)*(p+1)*(p+2)/2; nHex = 3*p*(p+1)^2;
nPri = p*(p+1)*(3*p+4)/2;
// number of functions of L2
lTet = (p > 0) ? p*(p+1)*(p+2)/6 : 1; lHex = (p+1)^3;
lPri = (p+1)^2*(p+2)/2;
FunctionSpace {
  { Name Hdiv; Type Form2;
    BasisFunction {
      If(whitney)
        { Name sf; NameOfCoef qf; Function BF_Facet; Support Dom; Entity FacetsOf[All]; }
      Else
        { Name sf; NameOfCoef qf; Function BF_HdivLegendre_F0; Support Dom; Entity FacetsOf[All]; }
      EndIf
      If(nTri > 0)
        For k In {1:nTri}
          { Name st~{k}; NameOfCoef qt~{k}; Function BF_HdivLegendre_FTri~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
      EndIf
      If(nQua > 0)
        For k In {1:nQua}
          { Name sq~{k}; NameOfCoef qq~{k}; Function BF_HdivLegendre_FQua~{k}; Support Dom; Entity FacetsOf[All]; }
        EndFor
      EndIf
      If(nTet > 0)
        For k In {0:nTet-1}
          { Name svt~{k}; NameOfCoef qvt~{k}; Function BF_HdivLegendre_VTet~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nHex > 0)
        For k In {0:nHex-1}
          { Name svh~{k}; NameOfCoef qvh~{k}; Function BF_HdivLegendre_VHex~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nPri > 0)
        For k In {0:nPri-1}
          { Name svp~{k}; NameOfCoef qvp~{k}; Function BF_HdivLegendre_VPri~{k}; Support Dom; Entity VolumesOf[All]; }
        EndFor
      EndIf
    }
    Constraint {
      If(link)
        { NameOfCoef qf; EntityType FacetsOf; NameOfConstraint Periodic; }
        If(nTri > 0)
          For k In {1:nTri}
            { NameOfCoef qt~{k}; EntityType FacetsOf; NameOfConstraint Periodic; }
          EndFor
        EndIf
        If(nQua > 0)
          For k In {1:nQua}
            { NameOfCoef qq~{k}; EntityType FacetsOf; NameOfConstraint Periodic; }
          EndFor
        EndIf
      EndIf
    }
  }
  { Name L2; Type Form3;
    BasisFunction {
      If(lTet > 0)
        For k In {0:lTet-1}
          { Name svt~{k}; NameOfCoef pvt~{k}; Function BF_L2Legendre_VTet~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(lHex > 0)
        For k In {0:lHex-1}
          { Name svh~{k}; NameOfCoef pvh~{k}; Function BF_L2Legendre_VHex~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(lPri > 0)
        For k In {0:lPri-1}
          { Name svp~{k}; NameOfCoef pvp~{k}; Function BF_L2Legendre_VPri~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
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
      { Name ed; Value { Integral { [ SquNorm[{phi} - phiex[]] ]; In Omega; Jacobian Vol; Integration I; } } }
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
