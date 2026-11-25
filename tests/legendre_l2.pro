// L2 projection of a known function on the polynomials of degree p
DefineConstant[ p = 1, dim = 2 ];
Include "legendre_common.pro";
Function { uex[] = Sin[2*X[]+Y[]+Z[]]; }
FunctionSpace {
  { Name L2; Type Form3;
    BasisFunction {
      For k In {0:((dim == 2) ? (p+1)*(p+2)/2 : (p+1)*(p+2)*(p+3)/6)-1}
        { Name sv~{k}; NameOfCoef uv~{k}; Function BF_L2Legendre_V~{k}; Support Omega; Entity VolumesOf[All]; }
      EndFor
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
      Print[ eu[Omega], OnGlobal, Format Table, File "ec.txt" ];
    }
  }
}
