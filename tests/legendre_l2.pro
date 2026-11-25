// L2 projection of a known function on the functions of L2 of order p
DefineConstant[ p = 1, dim = 2 ];
Include "legendre_common.pro";
Function { uex[] = Sin[2*X[]+Y[]+Z[]]; }
// number of functions per element
nLin = p+1; nTri = (p+1)*(p+2)/2; nQua = (p+1)^2;
nTet = (p+1)*(p+2)*(p+3)/6; nHex = (p+1)^3; nPri = (p+1)^2*(p+2)/2;
FunctionSpace {
  { Name L2; Type Form3;
    BasisFunction {
      If(nTri > 0)
        For k In {0:nTri-1}
          { Name st~{k}; NameOfCoef ut~{k}; Function BF_L2Legendre_VTri~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nQua > 0)
        For k In {0:nQua-1}
          { Name sq~{k}; NameOfCoef uq~{k}; Function BF_L2Legendre_VQua~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nTet > 0)
        For k In {0:nTet-1}
          { Name svt~{k}; NameOfCoef uvt~{k}; Function BF_L2Legendre_VTet~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nHex > 0)
        For k In {0:nHex-1}
          { Name svh~{k}; NameOfCoef uvh~{k}; Function BF_L2Legendre_VHex~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
      If(nPri > 0)
        For k In {0:nPri-1}
          { Name svp~{k}; NameOfCoef uvp~{k}; Function BF_L2Legendre_VPri~{k}; Support Omega; Entity VolumesOf[All]; }
        EndFor
      EndIf
    }
  }
}
Formulation {
  { Name F; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace L2; } }
    Equation {
      Galerkin { [ Dof{u}, {u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ -uex[], {u} ]; In Omega; Jacobian Vol; Integration I; }
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
    }
  }
}
PostOperation {
  { Name P; NameOfPostProcessing P;
    Operation {
      Print[ eu[Omega], OnGlobal, Format Table, File "eu.txt" ];
    }
  }
}
