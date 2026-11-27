// Print with Order: a field exported on Lagrange elements of a given order,
// in a mesh made on the fly. A smooth function is exported at orders 1 and 3
// on a mesh of first order (on all the shapes of elements), and its nodal
// interpolation at the order of the mesh ("Order Auto"). The views are read
// back by Gmsh, which interpolates them at the order of their elements: the
// error with respect to the function must be much smaller at order 3 than at
// order 1, and the view of order "Auto" must be the nodal interpolation.
// "Depth" is also applied to all the shapes.
Group { Omega = Region[1]; }
Function {
  f[] = Sin[2*X[] + Y[] + Z[]];
}
FunctionSpace {
  { Name H; Type Form0;
    BasisFunction {
      { Name s; NameOfCoef u; Function BF_Node; Support Omega;
        Entity NodesOf[All]; }
    }
  }
}
Formulation {
  { Name F; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace H; } }
    Equation {
      Galerkin { [ Dof{u}, {u} ]; In Omega; }
      Galerkin { [ -f[], {u} ]; In Omega; }
    }
  }
}
Resolution {
  { Name Check;
    System { { Name A; NameOfFormulation F; } }
    Operation {
      Generate[A]; Solve[A]; SaveSolution[A];
      PostOperation[Export];
      GmshRead["print_order_1.pos", 1];
      GmshRead["print_order_3.pos", 3];
      GmshRead["print_order_auto.pos", 4];
      PostOperation[Errors];
      Print[{$e1, $e3, $e4}, Format "Errors at order 1: %g, at order 3: %g, of the nodal interpolation: %g"];
      Test[$e3 > 0.02 * $e1 || $e1 > 0.1] {
        Error["Wrong error of the field exported at order 3"];
      }
      Test[$e4 > 1e-8] {
        Error["Wrong field exported at the order of its elements"];
      }
    }
  }
}
PostProcessing {
  { Name P; NameOfFormulation F;
    Quantity {
      { Name f; Value { Term { [ f[] ]; In Omega; } } }
      { Name u; Value { Term { [ {u} ]; In Omega; } } }
      { Name e1; Value { Integral { [ SquNorm[ScalarField[XYZ[]]{1} - f[]] ];
            In Omega; Integration Auto{4}; } } }
      { Name e3; Value { Integral { [ SquNorm[ScalarField[XYZ[]]{3} - f[]] ];
            In Omega; Integration Auto{4}; } } }
      { Name e4; Value { Integral { [ SquNorm[ScalarField[XYZ[]]{4} - {u}] ];
            In Omega; Integration Auto{4}; } } }
    }
  }
}
PostOperation {
  { Name Export; NameOfPostProcessing P;
    Operation {
      Print[ f, OnElementsOf Omega, Order 1, File "print_order_1.pos" ];
      Print[ f, OnElementsOf Omega, Order 3, File "print_order_3.pos" ];
      Print[ u, OnElementsOf Omega, Order Auto, File "print_order_auto.pos" ];
      Print[ f, OnElementsOf Omega, Depth 3, File "print_order_depth.pos" ];
    }
  }
  { Name Errors; NameOfPostProcessing P;
    Operation {
      Print[ e1[Omega], OnGlobal, Format Table, StoreInVariable $e1 ];
      Print[ e3[Omega], OnGlobal, Format Table, StoreInVariable $e3 ];
      Print[ e4[Omega], OnGlobal, Format Table, StoreInVariable $e4 ];
    }
  }
}
