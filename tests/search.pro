/* Test of the search of the element that contains a point, in a 3D mesh with
   surfaces: a point is in a volume if there is one, even if it is close to a
   triangle of a surface, and otherwise in a surface only if it is on it. The
   test is run without MPI and on several ranks (see utils/misc/run_test.py),
   and the results must be the same:

   getdp search.pro -solve Laplace -pos Points

   A second resolution tests an assembly limited to a group of regions before
   the assembly of the whole system:

   getdp search.pro -pre Grouped, then getdp search.pro -cal */

Group {
  Core = Region[1];
  Shell = Region[2];
  CoreSkin = Region[3];
  Outside = Region[4];
  Domain = Region[{Core, Shell}];
  Everything = Region[{Domain, CoreSkin, Outside}];
}

Function {
  f[] = 10 * Sin[3 * X[]] * Cos[2 * Y[]] + Z[];
}

Jacobian {
  { Name Vol; Case { { Region Everything; Jacobian Vol; } } }
}

Integration {
  { Name Int;
    Case {
      { Type Gauss;
        Case {
          { GeoElement Tetrahedron; NumberOfPoints 4; }
          { GeoElement Triangle; NumberOfPoints 3; }
        }
      }
    }
  }
}

Constraint {
  { Name Dirichlet; Case { { Region Outside; Value 1; } } }
}

FunctionSpace {
  { Name H_u; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef un; Function BF_Node; Support Everything;
        Entity NodesOf[Everything]; }
    }
    Constraint {
      { NameOfCoef un; EntityType NodesOf; NameOfConstraint Dirichlet; }
    }
  }
}

Formulation {
  // - Laplacian(u) = f, with u given on the boundary of the ball
  { Name Laplace; Type FemEquation;
    Quantity {
      { Name u; Type Local; NameOfSpace H_u; }
    }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Domain; Jacobian Vol;
        Integration Int; }
      Galerkin { [ -f[], {u} ]; In Domain; Jacobian Vol; Integration Int; }
    }
  }
}

Resolution {
  { Name Laplace;
    System {
      { Name A; NameOfFormulation Laplace; }
    }
    Operation {
      Generate[A]; Solve[A]; SaveSolution[A];
    }
  }
  { Name Grouped;
    System {
      { Name A; NameOfFormulation Laplace; }
    }
    Operation {
      GenerateGroup[A, Core]; Generate[A]; Solve[A]; SaveSolution[A];
      // u is of the order of 1: if the terms of the shell were missing in
      // the second assembly the matrix would be singular
      PostOperation[Center];
      Test[ !($u_center > 0 && $u_center < 10) ]{
        Error["Wrong solution after GenerateGroup"];
      }
    }
  }
}

PostProcessing {
  { Name Laplace; NameOfFormulation Laplace;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; Jacobian Vol; } } }
      // only defined in the core: zero elsewhere
      { Name u_core; Value { Term { [ {u} ]; In Core; Jacobian Vol; } } }
      { Name grad_u; Value { Term { [ {d u} ]; In Domain; Jacobian Vol; } } }
    }
  }
}

// the lines and the planes avoid the nodes, the edges and the faces of the
// mesh (see parallel.pro), and go out of the ball: the points outside are
// close to the triangles of its boundary

PostOperation {
  { Name Points; NameOfPostProcessing Laplace;
    Operation {
      Print[ u, OnLine {{-1.1, 0.013, 0.021} {1.1, 0.017, 0.029}} {300},
        Format Table, File "line_u.txt" ];
      Print[ grad_u, OnLine {{-1.1, 0.013, 0.021} {1.1, 0.017, 0.029}} {300},
        Format Table, File "line_grad_u.txt" ];
      Print[ u,
        OnPlane {{-1.1, -1.1, 0.011} {1.1, -1.1, 0.013} {-1.1, 1.1, 0.017}}
        {80, 70}, Format Table, File "plane_u.txt" ];
      Print[ u_core,
        OnPlane {{-0.6, 0.019, -0.6} {0.6, 0.023, -0.6} {-0.6, 0.029, 0.6}}
        {80, 70}, Format Table, File "plane_u_core.txt" ];
      Print[ grad_u,
        OnPlane {{0.031, -1.1, -1.1} {0.037, 1.1, -1.1} {0.041, -1.1, 1.1}}
        {80, 70}, File "plane_grad_u.pos" ];
    }
  }
  { Name Center; NameOfPostProcessing Laplace;
    Operation {
      Print[ u, OnPoint {0.011, 0.013, 0.017}, Format Table,
        StoreInVariable $u_center, File "center_u.txt" ];
    }
  }
}
