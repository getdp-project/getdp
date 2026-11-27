/* The T-Omega and A-v formulations of the induction heating device of
   onelab_doc/models/Cohomology, in the frequency domain, with the cuts that
   Gmsh computes in cohomology.geo; run without MPI and on several ranks (see
   utils/misc/run_test.py):

   getdp cohomology.pro -solve TOmega -pos TOmega
     h = t + grad phi in the conductors, h = grad phi in the air, plus the
     global basis functions of the cuts, whose coefficients are the currents
     of the coil (imposed) and of the tube

   getdp cohomology.pro -solve Av -pos Av
     a with a tree gauge, which the ranks build together, and e = grad v in
     the conductors, plus the global basis functions of the cuts, whose
     coefficients are the voltages of the coil and of the tube

   With -setnumber voltage 1 the voltage of the coil is imposed instead of its
   current: the global equation of the coil is then coupled with the fields, its
   current being an unknown.

   The potentials phi and v are only defined up to a constant (in the air for
   phi, in the tube for v), which is fixed at one point so that the systems are
   not singular. */

DefineConstant[ voltage = 0 ];

Group {
  Coil = Region[1000];
  Tube = Region[1001];
  Air = Region[1002];
  Conductors = Region[{Coil, Tube}];
  Omega = Region[{Conductors, Air}];
  SkinConductors = Region[{2000, 2001}];
  Terminals = Region[{2002, 2003}];
  CornerOfBox = Region[1003];
  PointOfTube = Region[1004];
  Boundary = Region[{2002, 2003, 2004}];
  // the cuts of the coil and of the tube
  Cut1TO = Region[2008];
  Cut2TO = Region[2007];
  Cut1AV = Region[2005];
  Cut2AV = Region[2006];
}

Function {
  mu0 = 4 * Pi * 1e-7;
  mu[] = mu0;
  sigma[Coil] = 6e7;
  sigma[Tube] = 3e7;
  Freq = 50;
  I = 1000; // current of the coil
  V = 0.1; // voltage of the coil
}

Jacobian {
  { Name Vol; Case { { Region All; Jacobian Vol; } } }
}

Integration {
  { Name Int;
    Case {
      { Type Gauss;
        Case {
          { GeoElement Tetrahedron; NumberOfPoints 4; }
          { GeoElement Triangle; NumberOfPoints 3; }
          { GeoElement Line; NumberOfPoints 2; }
          { GeoElement Point; NumberOfPoints 1; }
        }
      }
    }
  }
}

Constraint {
  // the current or the voltage of the coil, and no voltage in the tube
  { Name CurrentTO;
    Case { If(!voltage) { Region Cut1TO; Value I; } EndIf } }
  { Name VoltageTO;
    Case { If(voltage) { Region Cut1TO; Value V; } EndIf
           { Region Cut2TO; Value 0; } } }
  { Name CurrentAV;
    Case { If(!voltage) { Region Cut1AV; Value I; } EndIf } }
  { Name VoltageAV;
    Case { If(voltage) { Region Cut1AV; Value V; } EndIf
           { Region Cut2AV; Value 0; } } }
  { Name Gauge; Case { { Region Omega; SubRegion Boundary; Value 0; } } }
  // phi and v, which are only defined up to a constant
  { Name PotentialTO; Case { { Region CornerOfBox; Value 0; } } }
  { Name PotentialAV; Case { { Region PointOfTube; Value 0; } } }
}

FunctionSpace {
  { Name HSpace; Type Form1;
    BasisFunction {
      { Name sn; NameOfCoef phi; Function BF_GradNode;
        Support Omega; Entity NodesOf[Air]; }
      { Name se; NameOfCoef t; Function BF_Edge;
        Support Conductors; Entity EdgesOf[All, Not SkinConductors]; }
      { Name sc1; NameOfCoef I1; Function BF_GroupOfEdges;
        Support Omega; Entity GroupsOfEdgesOf[Cut1TO]; }
      { Name sc2; NameOfCoef I2; Function BF_GroupOfEdges;
        Support Omega; Entity GroupsOfEdgesOf[Cut2TO]; }
    }
    GlobalQuantity {
      { Name Current1; Type AliasOf; NameOfCoef I1; }
      { Name Current2; Type AliasOf; NameOfCoef I2; }
      { Name Voltage1; Type AssociatedWith; NameOfCoef I1; }
      { Name Voltage2; Type AssociatedWith; NameOfCoef I2; }
    }
    Constraint {
      { NameOfCoef phi; EntityType NodesOf; NameOfConstraint PotentialTO; }
      { NameOfCoef Current1; EntityType GroupsOfEdgesOf;
        NameOfConstraint CurrentTO; }
      { NameOfCoef Voltage1; EntityType GroupsOfEdgesOf;
        NameOfConstraint VoltageTO; }
      { NameOfCoef Voltage2; EntityType GroupsOfEdgesOf;
        NameOfConstraint VoltageTO; }
    }
  }
  { Name ASpace; Type Form1;
    BasisFunction {
      { Name se; NameOfCoef a; Function BF_Edge;
        Support Omega; Entity EdgesOf[All, Not Boundary]; }
    }
    Constraint {
      { NameOfCoef a; EntityType EdgesOfTreeIn; EntitySubType StartingOn;
        NameOfConstraint Gauge; }
    }
  }
  { Name ESpace; Type Form1;
    BasisFunction {
      { Name sn; NameOfCoef v; Function BF_GradNode;
        Support Conductors; Entity NodesOf[All, Not Terminals]; }
      { Name sc1; NameOfCoef V1; Function BF_GroupOfEdges;
        Support Conductors; Entity GroupsOfEdgesOf[Cut1AV]; }
      { Name sc2; NameOfCoef V2; Function BF_GroupOfEdges;
        Support Conductors; Entity GroupsOfEdgesOf[Cut2AV]; }
    }
    GlobalQuantity {
      { Name Voltage1; Type AliasOf; NameOfCoef V1; }
      { Name Current1; Type AssociatedWith; NameOfCoef V1; }
      { Name Voltage2; Type AliasOf; NameOfCoef V2; }
      { Name Current2; Type AssociatedWith; NameOfCoef V2; }
    }
    Constraint {
      { NameOfCoef v; EntityType NodesOf; NameOfConstraint PotentialAV; }
      { NameOfCoef Current1; EntityType GroupsOfEdgesOf;
        NameOfConstraint CurrentAV; }
      { NameOfCoef Voltage1; EntityType GroupsOfEdgesOf;
        NameOfConstraint VoltageAV; }
      { NameOfCoef Voltage2; EntityType GroupsOfEdgesOf;
        NameOfConstraint VoltageAV; }
    }
  }
}

Formulation {
  { Name TOmega; Type FemEquation;
    Quantity {
      { Name t; Type Local; NameOfSpace HSpace; }
      { Name I1; Type Global; NameOfSpace HSpace[Current1]; }
      { Name I2; Type Global; NameOfSpace HSpace[Current2]; }
      { Name V1; Type Global; NameOfSpace HSpace[Voltage1]; }
      { Name V2; Type Global; NameOfSpace HSpace[Voltage2]; }
    }
    Equation {
      Galerkin { DtDof[ mu[] * Dof{t}, {t} ];
        In Omega; Integration Int; Jacobian Vol; }
      Galerkin { [ 1 / sigma[] * Dof{d t}, {d t} ];
        In Conductors; Integration Int; Jacobian Vol; }
      GlobalTerm { [ Dof{V1}, {I1} ]; In Cut1TO; }
      GlobalTerm { [ Dof{V2}, {I2} ]; In Cut2TO; }
    }
  }
  { Name Av; Type FemEquation;
    Quantity {
      { Name a; Type Local; NameOfSpace ASpace; }
      { Name e; Type Local; NameOfSpace ESpace; }
      { Name I1; Type Global; NameOfSpace ESpace[Current1]; }
      { Name V1; Type Global; NameOfSpace ESpace[Voltage1]; }
      { Name I2; Type Global; NameOfSpace ESpace[Current2]; }
      { Name V2; Type Global; NameOfSpace ESpace[Voltage2]; }
    }
    Equation {
      Galerkin { [ 1 / mu[] * Dof{d a}, {d a} ];
        In Omega; Integration Int; Jacobian Vol; }
      Galerkin { DtDof[ sigma[] * Dof{a}, {a} ];
        In Conductors; Integration Int; Jacobian Vol; }
      Galerkin { [ sigma[] * Dof{e}, {a} ];
        In Conductors; Integration Int; Jacobian Vol; }
      Galerkin { DtDof[ sigma[] * Dof{a}, {e} ];
        In Conductors; Integration Int; Jacobian Vol; }
      Galerkin { [ sigma[] * Dof{e}, {e} ];
        In Conductors; Integration Int; Jacobian Vol; }
      GlobalTerm { [ -Dof{I1}, {V1} ]; In Cut1AV; }
      GlobalTerm { [ -Dof{I2}, {V2} ]; In Cut2AV; }
    }
  }
}

Resolution {
  { Name TOmega;
    System {
      { Name A; NameOfFormulation TOmega; Type ComplexValue; Frequency Freq; }
    }
    Operation {
      // with the real-valued PETSc, the complex system is solved in its real
      // form, whose rows of phi have no diagonal (the term in the air is
      // imaginary): MUMPS then needs more than its estimated work space
      SetGlobalSolverOptions["-mat_mumps_icntl_14 100"];
      Generate[A]; Solve[A]; SaveSolution[A];
    }
  }
  { Name Av;
    System {
      { Name A; NameOfFormulation Av; Type ComplexValue; Frequency Freq; }
    }
    Operation { Generate[A]; Solve[A]; SaveSolution[A]; }
  }
}

PostProcessing {
  { Name TOmega; NameOfFormulation TOmega;
    Quantity {
      { Name b; Value { Local { [ mu[] * {t} ]; In Omega; Jacobian Vol; } } }
      // time average of the losses
      { Name P; Value { Integral { [ 0.5 / sigma[] * SquNorm[{d t}] ];
            In Tube; Integration Int; Jacobian Vol; } } }
      { Name I1; Value { Term { [ {I1} ]; In Cut1TO; } } }
      { Name V1; Value { Term { [ {V1} ]; In Cut1TO; } } }
      { Name I2; Value { Term { [ {I2} ]; In Cut2TO; } } }
    }
  }
  { Name Av; NameOfFormulation Av;
    Quantity {
      { Name b; Value { Local { [ {d a} ]; In Omega; Jacobian Vol; } } }
      { Name P; Value { Integral {
            [ 0.5 * sigma[] * SquNorm[Dt[{a}] + {e}] ];
            In Tube; Integration Int; Jacobian Vol; } } }
      { Name I1; Value { Term { [ {I1} ]; In Cut1AV; } } }
      { Name V1; Value { Term { [ {V1} ]; In Cut1AV; } } }
      { Name I2; Value { Term { [ {I2} ]; In Cut2AV; } } }
    }
  }
}

PostOperation {
  { Name TOmega; NameOfPostProcessing TOmega;
    Operation {
      Print[ b, OnLine {{-0.2, 0.01, 0.02} {0.2, 0.01, 0.02}} {40},
        Format Table, File "line_b.txt" ];
      Print[ P[Tube], OnGlobal, Format Table, File "losses.txt" ];
      Print[ I1, OnRegion Cut1TO, Format Table, File "I1.txt" ];
      Print[ V1, OnRegion Cut1TO, Format Table, File "V1.txt" ];
      Print[ I2, OnRegion Cut2TO, Format Table, File "I2.txt" ];
    }
  }
  { Name Av; NameOfPostProcessing Av;
    Operation {
      Print[ b, OnLine {{-0.2, 0.01, 0.02} {0.2, 0.01, 0.02}} {40},
        Format Table, File "line_b.txt" ];
      Print[ P[Tube], OnGlobal, Format Table, File "losses.txt" ];
      Print[ I1, OnRegion Cut1AV, Format Table, File "I1.txt" ];
      Print[ V1, OnRegion Cut1AV, Format Table, File "V1.txt" ];
      Print[ I2, OnRegion Cut2AV, Format Table, File "I2.txt" ];
    }
  }
}
