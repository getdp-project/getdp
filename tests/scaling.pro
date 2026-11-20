/* Scaling benchmark: a scalar or a vector problem on the unit cube, with its
   post-processing, to measure how the time and the memory of each phase of a
   computation grow with the size of the problem and the number of MPI ranks
   (see utils/misc/scaling.py, which runs it for several numbers of ranks).

     Space = 0: - div(nu grad u) + u = 1, with u = 0 on the boundary (nodal
       unknowns), e.g. with -ksp_type cg -pc_type hypre -pc_hypre_type boomeramg
       (see utils/misc/scaling.py for options that make it faster in 3D)

     Space = 1: curl(nu curl a) + a = j, with n x a = 0 on the boundary (edge
       unknowns), e.g. with -ksp_type cg -pc_type hypre -pc_hypre_type ams

   getdp scaling.pro -setnumber Space 1 -solve Solve -cpu
     prints the time spent in each operation ("Cumulative performance summary")
     and the memory used by the ranks */

DefineConstant[ Space = 0 ];

Group {
  Domain = Region[1];
  Boundary = Region[2];
}

Function {
  nu[] = 1 + 0.5 * X[];
  j[] = Vector[1, 2, 3];
}

Constraint {
  { Name Dirichlet; Case { { Region Boundary; Value 0; } } }
}

Jacobian {
  { Name Vol; Case { { Region All; Jacobian Vol; } } }
}

Integration {
  { Name Int;
    Case {
      { Type Gauss;
        Case {
          { GeoElement Triangle; NumberOfPoints 3; }
          { GeoElement Tetrahedron; NumberOfPoints 4; }
        }
      }
    }
  }
}

FunctionSpace {
  { Name H1; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef un; Function BF_Node;
        Support Region[{Domain, Boundary}]; Entity NodesOf[All]; }
    }
    Constraint {
      { NameOfCoef un; EntityType NodesOf; NameOfConstraint Dirichlet; }
    }
  }
  { Name Hcurl; Type Form1;
    BasisFunction {
      { Name se; NameOfCoef ae; Function BF_Edge;
        Support Region[{Domain, Boundary}]; Entity EdgesOf[All]; }
    }
    Constraint {
      { NameOfCoef ae; EntityType EdgesOf; NameOfConstraint Dirichlet; }
    }
  }
}

Formulation {
  { Name Problem; Type FemEquation;
    Quantity {
      If(Space == 0)
        { Name u; Type Local; NameOfSpace H1; }
      Else
        { Name u; Type Local; NameOfSpace Hcurl; }
      EndIf
    }
    Equation {
      Galerkin { [ nu[] * Dof{d u}, {d u} ]; In Domain; Jacobian Vol;
        Integration Int; }
      Galerkin { [ Dof{u}, {u} ]; In Domain; Jacobian Vol; Integration Int; }
      If(Space == 0)
        Galerkin { [ -1, {u} ]; In Domain; Jacobian Vol; Integration Int; }
      Else
        Galerkin { [ -j[], {u} ]; In Domain; Jacobian Vol; Integration Int; }
      EndIf
    }
  }
}

// the post-processing is done in the resolution, one kind of Print at a time,
// so that each one has its own line in the performance summary

Resolution {
  { Name Solve;
    System {
      { Name A; NameOfFormulation Problem; }
    }
    Operation {
      Generate[A]; Solve[A]; SaveSolution[A];
      PostOperation[Energy];
      PostOperation[FieldMap];
      PostOperation[Points];
    }
  }
}

PostProcessing {
  { Name Problem; NameOfFormulation Problem;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Domain; Jacobian Vol; } } }
      { Name energy; Value {
          Integral { [ nu[] * SquNorm[{d u}] + SquNorm[{u}] ]; In Domain;
            Jacobian Vol; Integration Int; } } }
    }
  }
}

PostOperation {
  // a number that characterizes the solution, to compare the runs
  { Name Energy; NameOfPostProcessing Problem;
    Operation {
      Print[ energy[Domain], OnGlobal, Format Table, File "energy.txt" ];
    }
  }
  { Name FieldMap; NameOfPostProcessing Problem;
    Operation {
      Print[ u, OnElementsOf Domain, File "u.pos" ];
    }
  }
  { Name Points; NameOfPostProcessing Problem;
    Operation {
      Print[ u, OnPlane {{0.01, 0.02, 0.503} {0.99, 0.02, 0.507}
          {0.01, 0.98, 0.511}} {300, 300}, File "u_plane.pos" ];
    }
  }
}
