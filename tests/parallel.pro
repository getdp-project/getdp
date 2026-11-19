/* Tests of what GetDP does differently on several MPI ranks, on small scalar
   problems on the unit square. Each test is run without MPI and on several
   ranks (see utils/misc/run_test.py), and the results must be the same:

   getdp parallel.pro -solve Laplace -pos Points
     the evaluations at points, computed by all the ranks, each one for the
     points in its part of the mesh

   getdp parallel.pro -solve Laplace -pos Elements
     the field maps in Gmsh format, also computed by all the ranks, and the
     Prints that the first rank still does alone, for which it gets all the
     degrees of freedom

   getdp parallel.pro -solve Transfer
     TransferSolution, done by each rank for its degrees of freedom

   getdp parallel.pro -solve Periodic -pos Periodic
     a Link constraint, which relates degrees of freedom of different ranks */

Group {
  Domain = Region[1];
  Left = Region[2];
  Right = Region[3];
  Everything = Region[{Domain, Left, Right}];
}

Function {
  f[] = 10 * Sin[3 * X[]] * Cos[2 * Y[]];
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
  { Name Periodicity;
    Case {
      // u on the right side is u at the same height on the left side
      { Region Right; Type Link; RegionRef Left; Coefficient 1;
        Function Vector[X[] - 1, Y[], Z[]]; }
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
  { Name H_u_periodic; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef un; Function BF_Node; Support Domain;
        Entity NodesOf[Everything]; }
    }
    Constraint {
      { NameOfCoef un; EntityType NodesOf; NameOfConstraint Periodicity; }
    }
  }
}

Formulation {
  // - Laplacian(u) = f, with u given on the left and right sides
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
  // a second system on the same function space, to transfer the solution to
  { Name Mass; Type FemEquation;
    Quantity {
      { Name u; Type Local; NameOfSpace H_u; }
    }
    Equation {
      Galerkin { [ Dof{u}, {u} ]; In Domain; Jacobian Vol; Integration Int; }
    }
  }
  // - Laplacian(u) + u = f, with u periodic from left to right
  { Name Periodic; Type FemEquation;
    Quantity {
      { Name u; Type Local; NameOfSpace H_u_periodic; }
    }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Domain; Jacobian Vol;
        Integration Int; }
      Galerkin { [ Dof{u}, {u} ]; In Domain; Jacobian Vol; Integration Int; }
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
  { Name Transfer;
    System {
      { Name A; NameOfFormulation Laplace; DestinationSystem B; }
      { Name B; NameOfFormulation Mass; }
    }
    Operation {
      Generate[A]; Solve[A]; TransferSolution[A]; SaveSolution[B];
      PostOperation[Transferred];
    }
  }
  { Name Periodic;
    System {
      { Name A; NameOfFormulation Periodic; }
    }
    Operation {
      Generate[A]; Solve[A]; SaveSolution[A];
    }
  }
}

PostProcessing {
  { Name Laplace; NameOfFormulation Laplace;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; Jacobian Vol; } } }
      { Name grad_u; Value { Term { [ {d u} ]; In Domain; Jacobian Vol; } } }
      { Name int_u; Value {
          Integral { [ {u} ]; In Domain; Jacobian Vol; Integration Int; } } }
    }
  }
  { Name Transferred; NameOfFormulation Mass; NameOfSystem B;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; Jacobian Vol; } } }
      { Name int_u2; Value {
          Integral { [ {u} * {u} ]; In Domain; Jacobian Vol;
            Integration Int; } } }
    }
  }
  { Name Periodic; NameOfFormulation Periodic;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; Jacobian Vol; } } }
      { Name int_u; Value {
          Integral { [ {u} ]; In Domain; Jacobian Vol; Integration Int; } } }
    }
  }
}

// the lines and the planes avoid the nodes and the edges of the mesh: a point
// on the boundary between two elements can be evaluated in either of them,
// which changes the value of a discontinuous quantity like grad_u

PostOperation {
  { Name Points; NameOfPostProcessing Laplace;
    Operation {
      Print[ u, OnPoint {0.31, 0.47, 0}, Format Table, File "point_u.txt" ];
      Print[ grad_u, OnPoint {0.31, 0.47, 0}, Format Table,
        File "point_grad_u.txt" ];
      // a point and a line that are partly outside the mesh
      Print[ u, OnPoint {1.5, 0.47, 0}, Format Table,
        File "point_outside.txt" ];
      Print[ u, OnLine {{-0.2, 0.5, 0} {1.2, 0.5, 0}} {50}, Format Table,
        File "line_outside.txt" ];
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {200}, Format Table,
        File "line_u.txt" ];
      Print[ grad_u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {200},
        Format Table, File "line_grad_u.txt" ];
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {200}, Format Gmsh,
        File "line_u.pos" ];
      Print[ grad_u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {200},
        File "line_grad_u.pos" ];
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format SimpleTable, File "line_simpletable.txt" ];
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format TimeTable, File "line_timetable.txt" ];
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format Gnuplot, File "line_gnuplot.txt" ];
      Print[ u,
        OnPlane {{0.011, 0.012, 0} {0.991, 0.013, 0} {0.012, 0.993, 0}} {40, 30},
        Format Table, File "plane_u.txt" ];
      Print[ grad_u,
        OnPlane {{0.011, 0.012, 0} {0.991, 0.013, 0} {0.012, 0.993, 0}} {40, 30},
        File "plane_grad_u.pos" ];
      Print[ u,
        OnPlane {{0.011, 0.012, 0} {0.991, 0.013, 0} {0.012, 0.993, 0}} {40, 30},
        Depth 2, File "plane_depth.pos" ];
      Print[ int_u[Domain],
        OnPlane {{0.011, 0.012, 0} {0.991, 0.013, 0} {0.012, 0.993, 0}} {4, 3},
        Format Table, File "plane_integral.txt" ];
      Print[ u,
        OnGrid {0.5 + 0.4 * $A * Cos[$B], 0.5 + 0.4 * $A * Sin[$B], 0}
        {0.1:1:0.1, 0:2*Pi:Pi/16, 0},
        Format Table, File "grid_u.txt" ];
      Print[ int_u[Domain], OnGlobal, Format Table, File "integral_u.txt" ];
    }
  }
  { Name Elements; NameOfPostProcessing Laplace;
    Operation {
      Print[ u, OnElementsOf Domain, File "u.pos" ];
      Print[ grad_u, OnElementsOf Domain, File "grad_u.pos" ];
      Print[ grad_u, OnElementsOf Domain, Format Table,
        File "elements_grad_u.txt" ];
      Print[ u, OnSection {{0.2, 0, 0} {0.7, 1, 0} {0.2, 0, 1}}, Format Table,
        File "section_u.txt" ];
    }
  }
  { Name Transferred; NameOfPostProcessing Transferred;
    Operation {
      Print[ int_u2[Domain], OnGlobal, Format Table, File "integral_u2.txt" ];
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20}, Format Table,
        File "line_u.txt" ];
      Print[ u, OnElementsOf Domain, File "u.pos" ];
    }
  }
  { Name Periodic; NameOfPostProcessing Periodic;
    Operation {
      Print[ int_u[Domain], OnGlobal, Format Table, File "integral_u.txt" ];
      Print[ u, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {200}, Format Table,
        File "line_u.txt" ];
      Print[ u, OnElementsOf Domain, File "u.pos" ];
    }
  }
}
