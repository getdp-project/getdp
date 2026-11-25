// multi-harmonic nonlinear diffusion: div(nu(grad u) grad u) + sigma dt u = f,
// with nu = 1 + |grad u|^2 and a source at the fundamental frequency, solved
// for the harmonics 1 and 3 with hierarchical H1 functions of order p
DefineConstant[ p = 2, Freq = 1 ];
Group { Omega = Region[1]; Bnd = Region[2]; Dom = Region[{Omega, Bnd}]; }
Function {
  h[] = (1 + SquNorm[$1]) * $1;
  dhdb[] = TensorDiag[1, 1, 1] * (1 + SquNorm[$1]) + 2 * SquDyadicProduct[$1];
  src[] = 20 * Sin[Pi*X[]] * Sin[Pi*Y[]]; // a real value: the first harmonic
  sigma = 1;
}
Constraint {
  { Name Dir; Case { { Region Bnd; Value 0; } } }
}
Jacobian { { Name Vol; Case { { Region All; Jacobian Vol; } } } }
Integration {
  { Name I; Case { { Type GaussLegendre; Case {
    { GeoElement Line; NumberOfPoints 10; }
    { GeoElement Triangle; NumberOfPoints 64; } } } } }
}
FunctionSpace {
  { Name H; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef un; Function BF_H1Legendre_N0; Support Dom; Entity NodesOf[All]; }
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
    }
    Constraint {
      { NameOfCoef un; EntityType NodesOf; NameOfConstraint Dir; }
      If(p > 1)
        For k In {0:p-2}
          { NameOfCoef ue~{k}; EntityType EdgesOf; NameOfConstraint Dir; }
        EndFor
      EndIf
    }
  }
}
Formulation {
  { Name F; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace H; } }
    Equation {
      Galerkin { [ MHTransform[ h[{d u}] ]{16}, {d u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { JacNL [ MHBilinear[ dhdb[{d u}] ]{16, 0} * Dof{d u}, {d u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { DtDof [ sigma * Dof{u}, {u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ -src[], {u} ]; In Omega; Jacobian Vol; Integration I; }
    }
  }
}
Resolution {
  { Name R;
    System { { Name A; NameOfFormulation F; Type ComplexValue; Frequency {Freq, 3*Freq}; } }
    Operation {
      InitSolution[A];
      IterativeLoop[30, 1e-8, 1] { GenerateJac[A]; SolveJac[A]; }
      SaveSolution[A];
    }
  }
}
PostProcessing {
  { Name P; NameOfFormulation F;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Omega; Jacobian Vol; } } }
      { Name e; Value { Integral { [ {u} * Sin[Pi*X[]] * Sin[Pi*Y[]] ]; In Omega; Jacobian Vol; Integration I; } } }
    }
  }
}
PostOperation {
  { Name P; NameOfPostProcessing P;
    Operation {
      Print[ e[Omega], OnGlobal, Format Table, File "e.txt" ];
      Print[ u, OnPoint {0.5, 0.5, 0}, Format Table, File "u.txt" ];
    }
  }
}
