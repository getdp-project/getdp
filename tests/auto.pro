/* Automatic Jacobian and integration, on the unit square of parallel.geo, run
   without MPI and on several ranks (see utils/misc/run_test.py):

   getdp auto.pro -msh parallel.msh -solve Check -pos Check

   A diffusion problem with a source, a Dirichlet condition on the left side
   and a Robin condition on the right side (a term on line elements) is solved
   three times: with explicit Jacobian and Integration methods, with none (the
   automatic ones are the default), and with 'Jacobian Auto' in a Jacobian
   method and 'Integration Auto{2}'. The three solutions must agree, the
   explicit quadratures being more accurate than needed: the comparison stops
   getdp with an error otherwise. The Prints are those of the second
   solution. */

Group {
  Domain = Region[1];
  Left = Region[2];
  Right = Region[3];
  Everything = Region[{Domain, Left, Right}];
}

Function {
  f[] = 10 * Sin[3 * X[]] * Cos[2 * Y[]];
  h[] = 2;
  g[] = 1 + Y[]^2;
}

Jacobian {
  { Name Vol; Case { { Region All; Jacobian Vol; } } }
  { Name Sur; Case { { Region All; Jacobian Sur; } } }
  { Name Mixed; Case { { Region Right; Jacobian Sur; }
      { Region All; Jacobian Auto; } } }
}

Integration {
  { Name Int;
    Case {
      { Type Gauss;
        Case {
          { GeoElement Triangle; NumberOfPoints 16; }
          { GeoElement Line; NumberOfPoints 6; }
        }
      }
    }
  }
}

Constraint {
  { Name Dirichlet; Case { { Region Left; Value 0; } } }
}

FunctionSpace {
  For i In {1:3}
    { Name H~{i}; Type Form0;
      BasisFunction {
        { Name sn; NameOfCoef un; Function BF_Node; Support Everything;
          Entity NodesOf[All]; }
      }
      Constraint {
        { NameOfCoef un; EntityType NodesOf; NameOfConstraint Dirichlet; }
      }
    }
  EndFor
}

Formulation {
  // explicit Jacobian and Integration
  { Name F_1; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace H_1; } }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Domain; Jacobian Vol;
        Integration Int; }
      Galerkin { [ -f[], {u} ]; In Domain; Jacobian Vol; Integration Int; }
      Galerkin { [ h[] * Dof{u}, {u} ]; In Right; Jacobian Sur;
        Integration Int; }
      Galerkin { [ -g[], {u} ]; In Right; Jacobian Sur; Integration Int; }
    }
  }
  // none: automatic
  { Name F_2; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace H_2; } }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Domain; }
      Galerkin { [ -f[], {u} ]; In Domain; }
      Galerkin { [ h[] * Dof{u}, {u} ]; In Right; }
      Galerkin { [ -g[], {u} ]; In Right; }
    }
  }
  // Jacobian Auto in a Jacobian method, and automatic integration with two
  // degrees more
  { Name F_3; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace H_3; } }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Domain; Jacobian Mixed;
        Integration Auto; }
      Galerkin { [ -f[], {u} ]; In Domain; Jacobian Auto;
        Integration Auto{2}; }
      Galerkin { [ h[] * Dof{u}, {u} ]; In Right; Jacobian Mixed;
        Integration Auto{2}; }
      Galerkin { [ -g[], {u} ]; In Right; Jacobian Mixed; }
    }
  }
}

// stops with an error if the solution of the system differs from the one
// copied in "x_1" by more than the relative tolerance 1e-5
Macro CompareWithFirst
  CopySolution[A~{i}, "x"];
  AddVector[A~{i}, 1, "x", -1, "x_1", "diff"];
  GetNormSolution[A~{i}, $norm];
  CopySolution["diff", A~{i}];
  GetNormSolution[A~{i}, $diff];
  CopySolution["x", A~{i}];
  Test[$diff > 1e-5 * $norm] {
    Error["The solution with automatic Jacobian and integration differs"];
  }
Return

Resolution {
  { Name Check;
    System {
      For i In {1:3}
        { Name A~{i}; NameOfFormulation F~{i}; }
      EndFor
    }
    Operation {
      Generate[A_1]; Solve[A_1]; CopySolution[A_1, "x_1"];
      For i In {2:3}
        Generate[A~{i}]; Solve[A~{i}];
        Call CompareWithFirst;
      EndFor
      SaveSolution[A_2];
    }
  }
}

PostProcessing {
  { Name Check; NameOfFormulation F_2;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; } } }
      { Name int_u; Value { Integral { [ {u} ]; In Domain; } } }
      { Name int_r; Value { Integral { [ {u} ]; In Right; } } }
    }
  }
}

PostOperation {
  { Name Check; NameOfPostProcessing Check;
    Operation {
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format Table, File "line_u.txt" ];
      Print[ int_u[Domain], OnGlobal, Format Table, File "integral_u.txt" ];
      Print[ int_r[Right], OnGlobal, Format Table, File "integral_r.txt" ];
    }
  }
}
