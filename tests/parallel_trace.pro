// Terms on surfaces with the trace of the unknown of the volume, on the
// elements on one side of the surface: on the boundary (the normal flux, with
// a penalty for the value of the unknown) and on an interface inside the
// domain. The rank that assembles a surface element needs the Dofs of the
// volume element next to it, which is not always in its partition: on a mesh
// that GetDP partitions itself, the results used to depend on the number of
// ranks.
Group {
  Omega = Region[1]; Bnd = Region[2]; Mid = Region[3];
  Dom = Region[{Omega, Bnd, Mid}];
  OmegaBnd = ElementsOf[Omega, OnOneSideOf Bnd];
  OmegaMid = ElementsOf[Omega, OnPositiveSideOf Mid];
}
Function {
  uex[] = Sin[2*X[]+Y[]+Z[]];
  f[] = 7*uex[];
  n[] = Vector[(Fabs[X[]-1] < 1e-6) - (Fabs[X[]] < 1e-6),
               (Fabs[Y[]-1] < 1e-6) - (Fabs[Y[]] < 1e-6),
               (Fabs[Z[]-1] < 1e-6) - (Fabs[Z[]] < 1e-6)];
}
Jacobian {
  { Name Vol; Case { { Region All; Jacobian Vol; } } }
  { Name Sur; Case { { Region All; Jacobian Sur; } } }
}
Integration {
  { Name I; Case { { Type Gauss; Case {
    { GeoElement Triangle; NumberOfPoints 4; }
    { GeoElement Tetrahedron; NumberOfPoints 5; } } } } }
}
FunctionSpace {
  { Name H; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef un; Function BF_Node; Support Dom; Entity NodesOf[All]; }
      { Name se; NameOfCoef ue; Function BF_Node_2E; Support Dom; Entity EdgesOf[All]; }
    }
  }
}
Formulation {
  { Name F; Type FemEquation;
    Quantity {
      { Name u; Type Local; NameOfSpace H; }
      // the same unknown, for its trace on a surface from the volume (a
      // quantity cannot be both in the surface element and in its trace)
      { Name ut; Type Local; NameOfSpace H; }
    }
    Equation {
      Galerkin { [ Dof{d u}, {d u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ Dof{u}, {u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ -f[], {u} ]; In Omega; Jacobian Vol; Integration I; }
      Galerkin { [ -(Trace[Dof{d ut}, OmegaBnd] * n[]), {u} ];
        In Bnd; Jacobian Sur; Integration I; }
      Galerkin { [ 100 * Dof{u}, {u} ]; In Bnd; Jacobian Sur; Integration I; }
      Galerkin { [ -100 * uex[], {u} ]; In Bnd; Jacobian Sur; Integration I; }
      Galerkin { [ 5 * Trace[Dof{d ut}, OmegaMid] * Vector[0, 0, 1], {u} ];
        In Mid; Jacobian Sur; Integration I; }
    }
  }
}
Resolution {
  { Name Surfaces;
    System { { Name A; NameOfFormulation F; } }
    Operation { Generate[A]; Solve[A]; SaveSolution[A]; }
  }
}
PostProcessing {
  { Name Surfaces; NameOfFormulation F;
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Omega; Jacobian Vol; } } }
      { Name int; Value { Integral { [ {u} ]; In Omega; Jacobian Vol; Integration I; } } }
      { Name err; Value { Integral { [ SquNorm[{u} - uex[]] ]; In Omega; Jacobian Vol; Integration I; } } }
    }
  }
}
PostOperation {
  { Name Surfaces; NameOfPostProcessing Surfaces;
    Operation {
      Print[ int[Omega], OnGlobal, Format Table, File "parallel_trace_int.txt" ];
      Print[ err[Omega], OnGlobal, Format Table, File "parallel_trace_err.txt" ];
      Print[ u, OnPoint {0.5, 0.5, 0.25}, Format Table, File "parallel_trace_u.txt" ];
    }
  }
}
