/* SolveNL (PETSc SNES) on the unit square of parallel.geo, run without MPI and
   on several ranks (see utils/misc/run_test.py):

   getdp solve_nl.pro -msh parallel.msh -solve Linear -pos Linear
     a linear problem, solved by SolveNL and by Solve, which must give the
     same solution

   getdp solve_nl.pro -msh parallel.msh -solve NonLinear -pos NonLinear
     a nonlinear problem with a JacNL term, solved by SolveNL and then by
     Newton iterations (GenerateJac and SolveJac), which must not move the
     solution of SolveNL

   getdp solve_nl.pro -msh parallel.msh -solve Picard -pos Picard
     the same nonlinear problem, solved by fixed point iterations (Generate
     and Solve: the JacNL term is discarded, as there is no Jacobian matrix
     yet) and then by Newton iterations, which must not move the solution

   SolveNL starts from zero, and the comparison stops getdp with an error if
   the solutions differ. The Prints are those of the solution of SolveNL. */

Group {
  Domain = Region[1];
  Left = Region[2];
  Right = Region[3];
  Everything = Region[{Domain, Left, Right}];
}

Function {
  f[] = 10 * Sin[3 * X[]] * Cos[2 * Y[]];
  // conductivity of the nonlinear problem, and its derivative
  k[] = 1 + $1^2;
  dk[] = 2 * $1;
}

Jacobian {
  { Name Vol; Case { { Region Everything; Jacobian Vol; } } }
}

Integration {
  { Name Int;
    Case {
      { Type Gauss;
        Case {
          { GeoElement Triangle; NumberOfPoints 4; }
          { GeoElement Line; NumberOfPoints 2; }
        }
      }
    }
  }
}

Constraint {
  { Name Dirichlet;
    Case {
      { Region Left; Value 0; }
      { Region Right; Value 1; }
    }
  }
}

FunctionSpace {
  { Name H_u; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef un; Function BF_Node; Support Domain;
        Entity NodesOf[Everything]; }
    }
    Constraint {
      { NameOfCoef un; EntityType NodesOf; NameOfConstraint Dirichlet; }
    }
  }
}

Formulation {
  // - Laplacian(u) = f
  { Name Linear; Type FemEquation;
    Quantity {
      { Name u; Type Local; NameOfSpace H_u; }
    }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Domain; Jacobian Vol;
        Integration Int; }
      Galerkin { [ -f[], {u} ]; In Domain; Jacobian Vol; Integration Int; }
    }
  }
  // - div(k(u) grad(u)) = f, whose Jacobian has the term dk(u) grad(u) du
  { Name NonLinear; Type FemEquation;
    Quantity {
      { Name u; Type Local; NameOfSpace H_u; }
    }
    Equation {
      Galerkin { [ k[{u}] * Dof{d u}, {d u} ]; In Domain; Jacobian Vol;
        Integration Int; }
      Galerkin { JacNL [ dk[{u}] * {d u} * Dof{u}, {d u} ]; In Domain;
        Jacobian Vol; Integration Int; }
      Galerkin { [ -f[], {u} ]; In Domain; Jacobian Vol; Integration Int; }
    }
  }
}

// stops with an error if the solution differs from the one copied in "x_nl"
// by more than the relative tolerance $tol, then makes "x_nl" the solution
Macro CompareWithSolveNL
  CopySolution[A, "x"];
  AddVector[A, 1, "x", -1, "x_nl", "diff"];
  GetNormSolution[A, $norm];
  CopySolution["diff", A];
  GetNormSolution[A, $diff];
  CopySolution["x_nl", A];
  Test[$diff > $tol * $norm] {
    Error["The solution of SolveNL differs from the reference solution"];
  }
Return

Resolution {
  { Name Linear;
    System {
      { Name A; NameOfFormulation Linear; }
    }
    Operation {
      InitSolution[A]; SolveNL[A]; CopySolution[A, "x_nl"];
      Generate[A]; Solve[A];
      Evaluate[$tol = 1e-10];
      Call CompareWithSolveNL;
      SaveSolution[A];
    }
  }
  { Name NonLinear;
    System {
      { Name A; NameOfFormulation NonLinear; }
    }
    Operation {
      InitSolution[A]; SolveNL[A]; CopySolution[A, "x_nl"];
      IterativeLoop[20, 1e-12, 1] { GenerateJac[A]; SolveJac[A]; }
      Evaluate[$tol = 1e-8];
      Call CompareWithSolveNL;
      SaveSolution[A];
    }
  }
  { Name Picard;
    System {
      { Name A; NameOfFormulation NonLinear; }
    }
    Operation {
      InitSolution[A];
      For i In {1:40}
        Generate[A]; Solve[A];
      EndFor
      CopySolution[A, "x_nl"];
      IterativeLoop[20, 1e-12, 1] { GenerateJac[A]; SolveJac[A]; }
      Evaluate[$tol = 1e-8];
      Call CompareWithSolveNL;
      SaveSolution[A];
    }
  }
}

PostProcessing {
  { Name Linear; NameOfFormulation Linear;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; Jacobian Vol; } } }
      { Name int_u; Value {
          Integral { [ {u} ]; In Domain; Jacobian Vol; Integration Int; } } }
    }
  }
  { Name NonLinear; NameOfFormulation NonLinear;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; Jacobian Vol; } } }
      { Name int_u; Value {
          Integral { [ {u} ]; In Domain; Jacobian Vol; Integration Int; } } }
    }
  }
}

PostOperation {
  { Name Linear; NameOfPostProcessing Linear;
    Operation {
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format Table, File "line_u.txt" ];
      Print[ int_u[Domain], OnGlobal, Format Table, File "integral_u.txt" ];
    }
  }
  { Name NonLinear; NameOfPostProcessing NonLinear;
    Operation {
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format Table, File "line_u.txt" ];
      Print[ int_u[Domain], OnGlobal, Format Table, File "integral_u.txt" ];
    }
  }
  { Name Picard; NameOfPostProcessing NonLinear;
    Operation {
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format Table, File "line_u.txt" ];
      Print[ int_u[Domain], OnGlobal, Format Table, File "integral_u.txt" ];
    }
  }
}
