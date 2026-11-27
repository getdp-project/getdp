/* The fields of the PETSc preconditioner fieldsplit on the unit square of
   parallel.geo, run without MPI and on several ranks (see
   utils/misc/run_test.py):

   getdp fieldsplit.pro -msh parallel.msh -solve FieldSplit -pos FieldSplit

   With -pc_type fieldsplit, each field is made of the unknowns of a basis
   function (or of a global quantity), and is named by its coefficient. Here
   the potential v has three fields: "vn" (the first order functions), "v2" (the
   hierarchical functions of order 2) and a field of one unknown, which a single
   rank owns, for the global equation of the right side:
   - its charge "Q", if its voltage V is imposed: Q only appears in the global
     equation, which gives it from v once v is known
   - its voltage "vf" (the coefficient of the function of the group of nodes of
     the right side), if its charge is imposed (-setnumber charge 1): the global
     equation is then coupled with the other unknowns; the fields are then
     chosen with -getdp_fieldsplit_fields vn,v2, so that vf goes to the field
     "rest" of the other unknowns

   The problem is solved by the default direct solver and then by GMRES with
   fieldsplit, and the comparison stops getdp with an error if the solutions
   differ. The Prints are those of the solution of the direct solver. */

DefineConstant[ charge = 0 ];

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
          { GeoElement Triangle; NumberOfPoints 7; }
          { GeoElement Line; NumberOfPoints 4; }
        }
      }
    }
  }
}

Constraint {
  { Name Zero;
    Case {
      { Region Left; Value 0; }
      { Region Right; Value 0; }
    }
  }
  // the voltage or the charge of the right side
  { Name Voltage;
    Case {
      If(!charge) { Region Right; Value 1; } EndIf
    }
  }
  { Name Charge;
    Case {
      If(charge) { Region Right; Value 1; } EndIf
    }
  }
}

FunctionSpace {
  { Name H_v; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef vn; Function BF_Node; Support Domain;
        Entity NodesOf[Everything, Not Right]; }
      { Name s2; NameOfCoef v2; Function BF_Node_2E; Support Domain;
        Entity EdgesOf[Everything]; }
      { Name sf; NameOfCoef vf; Function BF_GroupOfNodes; Support Domain;
        Entity GroupsOfNodesOf[Right]; }
    }
    GlobalQuantity {
      { Name V; Type AliasOf; NameOfCoef vf; }
      { Name Q; Type AssociatedWith; NameOfCoef vf; }
    }
    Constraint {
      { NameOfCoef vn; EntityType NodesOf; NameOfConstraint Zero; }
      { NameOfCoef v2; EntityType EdgesOf; NameOfConstraint Zero; }
      { NameOfCoef V; EntityType GroupsOfNodesOf; NameOfConstraint Voltage; }
      { NameOfCoef Q; EntityType GroupsOfNodesOf; NameOfConstraint Charge; }
    }
  }
}

Formulation {
  // - Laplacian(v) = f, with v = 0 on the left side and v = V on the right
  // side, of which Q is the flux of grad(v)
  { Name FieldSplit; Type FemEquation;
    Quantity {
      { Name v; Type Local; NameOfSpace H_v; }
      { Name V; Type Global; NameOfSpace H_v [V]; }
      { Name Q; Type Global; NameOfSpace H_v [Q]; }
    }
    Equation {
      Galerkin { [ Dof{d v}, {d v} ]; In Domain; Jacobian Vol;
        Integration Int; }
      Galerkin { [ -f[], {v} ]; In Domain; Jacobian Vol; Integration Int; }
      GlobalTerm { [ -Dof{Q}, {V} ]; In Right; }
    }
  }
}

Resolution {
  { Name FieldSplit;
    System {
      { Name A; NameOfFormulation FieldSplit; }
    }
    Operation {
      Generate[A]; Solve[A]; CopySolution[A, "x_lu"];
      // the second solver (index 1), with the options of the fields "vn" and
      // "v2" (the field of the global equation has the default options of
      // PETSc)
      SetGlobalSolverOptions[
        StrCat["-ksp_type gmres -ksp_rtol 1e-12 -pc_type fieldsplit ",
               "-pc_fieldsplit_type multiplicative ",
               "-fieldsplit_vn_pc_type gamg -fieldsplit_v2_pc_type jacobi"]];
      If(charge)
        SetGlobalSolverOptions["-getdp_fieldsplit_fields vn,v2"];
      EndIf
      Solve[A, 1];
      // stops with an error if the solution differs from the one of the
      // direct solver, which becomes the solution again
      CopySolution[A, "x"];
      AddVector[A, 1, "x", -1, "x_lu", "diff"];
      GetNormSolution[A, $norm];
      CopySolution["diff", A];
      GetNormSolution[A, $diff];
      CopySolution["x_lu", A];
      Test[$diff > 1e-8 * $norm] {
        Error["The solution with fieldsplit differs from the direct one"];
      }
      SaveSolution[A];
    }
  }
}

PostProcessing {
  { Name FieldSplit; NameOfFormulation FieldSplit;
    Quantity {
      { Name v; Value { Term { [ {v} ]; In Domain; Jacobian Vol; } } }
      { Name V; Value { Term { [ {V} ]; In Right; } } }
      { Name Q; Value { Term { [ {Q} ]; In Right; } } }
    }
  }
}

PostOperation {
  { Name FieldSplit; NameOfPostProcessing FieldSplit;
    Operation {
      Print[ v, OnLine {{0.013, 0.021, 0} {0.987, 0.933, 0}} {20},
        Format Table, File "line_v.txt" ];
      Print[ V, OnRegion Right, Format Table, File "voltage.txt" ];
      Print[ Q, OnRegion Right, Format Table, File "charge.txt" ];
    }
  }
}
