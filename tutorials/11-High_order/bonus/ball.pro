// This bonus tutorial applies the high order function spaces of tutorial 11 to
// a Poisson problem, "-div(grad u) = f", on the unit disk (2D) or the unit
// ball (3D), with "u = 0" on the boundary. The source "f" is chosen so that
// the solution is "u = cos(pi/2 r^2)", with "r" the distance to the center:
// the error can be measured.
//
// It complements tutorial 11 in two ways:
//  - the other spaces of the de Rham complex are used: "H1" for the classical
//    formulation in terms of "u"; "H(div)" and "L2" for the mixed formulation
//    in terms of "u" and of the flux "q = -grad u"; and "L2" alone for the
//    projection of a given function on discontinuous functions. All the
//    function spaces are defined automatically, in one line each;
//  - the domain is curved. The order of the mesh (the polynomial order of the
//    shape of the elements) and the order of the basis functions are two
//    independent choices, and both matter: on a mesh with straight edges the
//    error on the geometry dominates and increasing the order of the basis
//    functions is useless.

Group {
  Omega = Region[ 1 ];
  Gamma_D = Region[ 2 ];
  Vol = Region[ Omega ];
  Sur = Region[ Gamma_D ];
}
Include "ball_common.pro";
Function {
  DefineConstant[
    form = {0, Choices{0="Poisson in H1", 1="Mixed Poisson in H(div) x L2 (3D)",
        2="Projection in L2"},
      Name "Parameters/2Formulation"}
    p = {2, Min 0, Max 8, Step 1,
      Name "Parameters/3Order of the basis functions"}
  ];

  // The exact solution and the source "f = -div(grad u)":
  a = Pi / 2;
  r2[] = X[]^2 + Y[]^2 + Z[]^2;
  uExact[] = Cos[a * r2[]];
  f[] = 2 * a * dim * Sin[a * r2[]] + 4 * a^2 * r2[] * Cos[a * r2[]];
}
// The functions of "H(div)" used here are those of the volumes and their
// normal traces on the surfaces: there is no "H(div)" in the plane.
If(form == 1 && dim == 2)
  Error["The mixed formulation is only available in 3D"];
EndIf

// The Jacobian and the integration are automatic (see tutorial 5). On a
// curved element the Jacobian of the mapping from the reference element is
// not constant: the automatic integration takes the order of the mesh into
// account and uses more quadrature points than on a straight element for the
// same basis functions.

Constraint {
  { Name Dirichlet; Case { { Region Sur; Type Assign; Value 0.; } } }
}
// The orders of the function spaces:
//  - the lowest order is 1 for "H1" and 0 for the other spaces;
//  - in the mixed formulation the space of "u" must be the space of the
//    divergences of the fluxes. With "BF_HdivLegendre" of order "p" on
//    tetrahedra these are the polynomials of degree "p - 1" (of degree 0 for
//    "p = 0", the facet elements "BF_Facet"; the orders 0 and 1 thus use the
//    same piecewise constant "u"). A larger space for "u" would make the
//    problem singular.
pH1 = (p > 0) ? p : 1;
pL2 = (form == 1 && p > 0) ? p - 1 : p;

FunctionSpace {
  // The space of "H1", with functions associated with the nodes, the edges,
  // the facets and the volumes. The functions are polynomials of degree "p" on
  // the reference element, whatever the order of the mesh, unlike the
  // isoparametric "BF_Node" functions whose order is that of the mesh. The
  // Dirichlet condition constrains all the functions of the boundary: those of
  // its nodes and edges, and of its facets in 3D.
  { Name H1_u; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef un; Function BF_H1Legendre; Order pH1;
        Support Region[{Vol, Sur}]; Entity Auto; }
    }
    Constraint {
      { NameOfCoef un; EntityType Auto; NameOfConstraint Dirichlet; }
    }
  }
  // The space of "H(div)", with functions associated with the facets (they
  // carry the normal component, which is continuous) and the volumes. No
  // constraint is needed: in the mixed formulation the condition "u = 0" is a
  // natural boundary condition.
  { Name Hdiv_q; Type Form2;
    BasisFunction {
      { Name sf; NameOfCoef qf; Function BF_HdivLegendre; Order p;
        Support Vol; Entity Auto; }
    }
  }
  // The space of "L2", with functions associated with the elements only: the
  // functions are discontinuous from one element to the next. ("Form3" fields
  // are densities: they are divided by the Jacobian when mapped from the
  // reference element, which is what makes the divergence of the fluxes
  // belong to this space on curved elements as well.)
  { Name L2_u; Type Form3;
    BasisFunction {
      { Name sv; NameOfCoef uv; Function BF_L2Legendre; Order pL2;
        Support Vol; Entity Auto; }
    }
  }
}
Formulation {
  // "(grad u, grad u') = (f, u')":
  { Name Poisson_H1; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace H1_u; } }
    Equation {
      Integral { [ Dof{d u} , {d u} ]; In Vol; }
      Integral { [ -f[] , {u} ]; In Vol; }
    }
  }
  // "q = -grad u" and "div q = f", i.e. "(q, q') - (u, div q') = 0" (after
  // integration by parts, with "u = 0" on the boundary) and "(div q, u') =
  // (f, u')":
  { Name Poisson_Mixed; Type FemEquation;
    Quantity {
      { Name q; Type Local; NameOfSpace Hdiv_q; }
      { Name u; Type Local; NameOfSpace L2_u; }
    }
    Equation {
      Integral { [ Dof{q} , {q} ]; In Vol; }
      Integral { [ -Dof{u} , {d q} ]; In Vol; }
      Integral { [ Dof{d q} , {u} ]; In Vol; }
      Integral { [ -f[] , {u} ]; In Vol; }
    }
  }
  // "(u, u') = (uExact, u')": the best approximation of the exact solution by
  // discontinuous polynomials of degree "p":
  { Name Projection_L2; Type FemEquation;
    Quantity { { Name u; Type Local; NameOfSpace L2_u; } }
    Equation {
      Integral { [ Dof{u} , {u} ]; In Vol; }
      Integral { [ -uExact[] , {u} ]; In Vol; }
    }
  }
}
Resolution {
  { Name Sol;
    System {
      If(form == 0)
        { Name A; NameOfFormulation Poisson_H1; }
      ElseIf(form == 1)
        { Name A; NameOfFormulation Poisson_Mixed; }
      Else
        { Name A; NameOfFormulation Projection_L2; }
      EndIf
    }
    Operation { Generate[A]; Solve[A]; SaveSolution[A]; }
  }
}
PostProcessing {
  If(form == 0)
  { Name Post; NameOfFormulation Poisson_H1;
  ElseIf(form == 1)
  { Name Post; NameOfFormulation Poisson_Mixed;
  Else
  { Name Post; NameOfFormulation Projection_L2;
  EndIf
    Quantity {
      { Name u; Value { Term { [ {u} ]; In Vol; } } }
      { Name error2;
        Value {
          Integral { [ SquNorm[{u} - uExact[]] ];
            In Vol; }
        }
      }
      { Name norm2;
        Value {
          Integral { [ SquNorm[uExact[]] ];
            In Vol; }
        }
      }
      { Name error;
        Value { Term { Type Global; [ Sqrt[$error2 / $norm2] ]; In Vol; } }
      }
    }
  }
}
PostOperation {
  { Name Map; NameOfPostProcessing Post;
    Operation {
      Print [ u, OnElementsOf Vol, Depth ((dim == 2) ? 3 : 1), File "u.pos" ];
      Print [ error2[Vol], OnGlobal, Format Table,
        StoreInVariable $error2, File > "error.txt" ];
      Print [ norm2[Vol], OnGlobal, Format Table,
        StoreInVariable $norm2, File > "error.txt" ];
      // The relative error on "u". With the default 2D mesh and the "H1"
      // formulation, on a mesh of order 1 the error stays around 9% whatever
      // the order of the basis functions: this is the error due to the
      // polygonal boundary. On a mesh of order 2 it falls to 0.3% for "p =
      // 2" and 0.03% for "p = 4", where it stalls in turn; a mesh of order 3
      // or 4 is needed to benefit from higher orders. The mixed formulation
      // in 3D behaves in the same way. The projection in "L2" is not affected
      // by the order of the mesh: it involves no boundary condition, and the
      // exact solution is simply approximated on the domain covered by the
      // mesh.
      Print [ error, OnRegion Vol, Format Table,
        SendToServer "Output/Relative L2 error on u", File "error.txt" ];
    }
  }
}
