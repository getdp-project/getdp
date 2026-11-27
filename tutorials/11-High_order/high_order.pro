// This tutorial revisits the full-wave problem of tutorial 5 with high order
// basis functions. With the lowest order elements used so far the only way to
// improve the accuracy of a computation is to refine the mesh; and for wave
// problems the mesh must be refined faster than the wavelength decreases,
// because the error on the phase accumulates along the propagation (the
// "pollution" effect). The alternative is to enrich the function space on each
// element: with basis functions of order "p" the error on the field decreases
// as "h^(p+1)" (with "h" the mesh size) instead of "h", so that a given
// accuracy is reached with far fewer unknowns on a coarse mesh.
//
// GetDP provides hierarchical basis functions of arbitrary order for the four
// spaces of the de Rham complex introduced in tutorial 5: "BF_H1Legendre",
// "BF_HcurlLegendre", "BF_HdivLegendre" and "BF_L2Legendre", on lines,
// triangles, quadrangles, tetrahedra, hexahedra and prisms. (They are built
// from Legendre polynomials and are computed by Gmsh: GetDP must be built with
// the "HierarchicalBasis" module of Gmsh to run this tutorial.) "Hierarchical"
// means that the space of order "p + 1" is the space of order "p" plus some
// new functions: the functions of lower order are kept. For "H(curl)" the
// space of order 0 is thus the space of the Whitney edge elements of tutorial
// 5 ("BF_Edge").
//
// Three ways of defining the same function space are presented and can be
// selected at runtime:
//  - the automatic one, where the whole space of order "p" is requested in a
//    single line;
//  - by type of entity, where the functions associated with the edges, the
//    facets and the volumes are requested separately, e.g. to use different
//    orders;
//  - function by function, where each function is named explicitly, as was
//    done with the built-in functions in the previous tutorials.
//
// In order to measure the error, we consider a problem whose solution is
// known: a straight section of the rectangular waveguide of tutorial 5,
// excited by a mode on the input port, with a boundary condition on the
// output port that is transparent for this mode. The mode then propagates
// unchanged along the waveguide.

Group {
  Waveguide = Region[ 1000 ];
  BndPEC = Region[ 2000 ];
  BndPort = Region[ 2001 ];
  BndOut = Region[ 2002 ];
  Vol_Wav = Region[ Waveguide ];
  Sur_Port_Wav = Region[ BndPort ];
  Sur_Out_Wav = Region[ BndOut ];
}

Include "high_order_common.pro";

Function {
  eps0 = 8.8541878176e-12;
  mu0 = 4 * Pi * 1e-7;
  DefineConstant[
    f = { 8e9, Min 1e8, Max 1e10, Step 1e8,
      Name "Parameters/Frequency [Hz]"}
    p = { 2, Min 0, Max 8, Step 1,
      Name "Parameters/2Order of the basis functions"}
    def = { 0, Choices{0="Automatic", 1="By type of entity",
        2="Function by function"},
      Name "Parameters/3Definition of the function space"}
  ];
  epsilon[] = eps0;
  nu[] = 1 / mu0;
  i[] = Complex[0, 1];
  omega = 2 * Pi * f;

  // The waveguide occupies "0 < x < Wx" (and "0 < z < Wz" in 3D) and the wave
  // propagates along "y". We consider the first mode with an electric field
  // component along the direction of propagation, as in tutorial 5: with "k"
  // the wavenumber, "kx" and "kz" the transverse wavenumbers and "beta" the
  // propagation constant (real above the cutoff frequency), the electric
  // field is
  //
  //   e_x = -i beta kx / kc^2 cos(kx x) sin(kz z) exp(-i beta y)
  //   e_y = sin(kx x) sin(kz z) exp(-i beta y)
  //   e_z = -i beta kz / kc^2 sin(kx x) cos(kz z) exp(-i beta y)
  //
  // (without the dependence on "z" in 2D, where "e_z = 0"). Its tangential
  // component vanishes on the walls, and it satisfies
  //
  //   n x (nu curl e) = i nu k^2 / beta (n x e) x n
  //
  // on any cross-section of the waveguide with normal "n" along "y". Used as
  // boundary condition on the output port, this relation lets the mode leave
  // the domain without any reflection. (The Silver-Muller condition of
  // tutorial 5 is the same relation with "beta = k": it is transparent for a
  // plane wave, not for a mode of the waveguide.)
  k = omega * Sqrt[eps0 * mu0];
  kx = Pi / Wx;
  kz = (dim == 3) ? Pi / Wz : 0;
  kc = Sqrt[kx^2 + kz^2];
  beta = Sqrt[k^2 - kc^2];
  sz[] = (dim == 3) ? Sin[kz * Z[]] : 1;
  cz[] = (dim == 3) ? Cos[kz * Z[]] : 0;
  eExact[] = Vector[ -i[] * beta * kx / kc^2 * Cos[kx * X[]] * sz[],
                     Sin[kx * X[]] * sz[],
                     -i[] * beta * kz / kc^2 * Sin[kx * X[]] * cz[] ]
    * Complex[ Cos[beta * Y[]], -Sin[beta * Y[]] ];

  DefineConstant[
    lambda = {2 * Pi / beta * 100, ReadOnly 1, Highlight "LightGrey",
      Name "Parameters/}Wavelength in the waveguide [cm]"}
  ];
}

// As in tutorial 5, the Jacobian and the integration quadratures are chosen
// automatically: no Jacobian or Integration object is defined. This matters
// more here than with first order elements: the products of basis functions of
// order "p" are polynomials whose degree grows as "2 p", and the quadrature
// must follow, otherwise the benefit of the high order is lost (or the matrix
// becomes singular). The automatic integration selects for each element the
// quadrature with the fewest points that integrates these products exactly,
// whatever the order and the shape of the element.

// The boundary conditions are those of tutorial 5: the tangential component
// of the electric field vanishes on the walls, and it is given on the input
// port.
//
// On the walls the value of all the coefficients is simply zero. On the port,
// the coefficients of the high order basis functions are neither values at
// nodes nor circulations along edges: there is no simple formula to compute
// them from the given field. The technique introduced in tutorial 5 for the
// Whitney elements is precisely what is needed, at any order: an auxiliary
// resolution ("Proj_e") computes the L2 projection of the given field on the
// functions of the port, and its solution gives the values of the constraint.
//
// (If a value other than zero is given directly with "Assign" to a high order
// function space, it is only given to the functions of lowest order, and the
// other functions are set to zero: the constraint is then the interpolation
// of lowest order of the data.)
Constraint {
  { Name e_Wav;
    Case {
      { Region BndPEC; Type Assign; Value 0.; }
      { Region BndPort; Type AssignFromResolution; NameOfResolution Proj_e; }
    }
  }
}

Group{
  Dom_Hcurl_e_Wav =  Region[ {Vol_Wav, Sur_Port_Wav, Sur_Out_Wav} ];
}

// Number of functions per facet and per volume in the space of order "p", for
// the definition function by function (see below):
If(shape == 0)
  nF = (p - 1) * (p + 1); nV = (p - 2) * (p - 1) * (p + 1) / 2;
Else
  nF = 2 * p * (p + 1); nV = 3 * p^2 * (p + 1);
EndIf

FunctionSpace {
  { Name Hcurl_e_Wav; Type Form1;
    BasisFunction {
      If(def == 0)
        // 1. The automatic definition.
        //
        // A function space of order "p" is requested by giving the name of
        // the space as "Function", together with its "Order". With "Entity
        // Auto" the functions are associated with all the entities that bear
        // functions in this space: the edges, the facets and the volumes of
        // the mesh for "H(curl)". This single line is all that is needed in
        // most cases, whatever the order and the shape of the elements:
        { Name se; NameOfCoef ee; Function BF_HcurlLegendre; Order p;
          Support Dom_Hcurl_e_Wav; Entity Auto; }
        // The same holds for the other spaces: "BF_H1Legendre" (of order "p
        // >= 1", associated with the nodes, edges, facets and volumes),
        // "BF_HdivLegendre" (facets and volumes) and "BF_L2Legendre"
        // (volumes). They are used in the bonus tutorial in the "bonus"
        // subdirectory, on curved meshes.

      ElseIf(def == 1)
        // 2. The definition by type of entity.
        //
        // What makes a field of "H(curl)" tangentially continuous is the way
        // its basis functions are associated with the entities of the mesh:
        //  - the functions of an edge have a tangential component along that
        //    edge, and are shared by all the elements around the edge;
        //  - the functions of a facet have no tangential component on the
        //    edges, but have one on the facet; they are shared by the two
        //    elements on both sides of the facet;
        //  - the functions of a volume have no tangential component on the
        //    boundary of the element: they live inside a single element.
        // (In 2D the "facets" are the triangles or quadrangles themselves,
        // and there are no "volumes"; the functions of the elements are thus
        // those of the facets.)
        //
        // With "EdgesOf", "FacetsOf" or "VolumesOf" as "Entity" instead of
        // "Auto", only the functions of the space of order "p" associated
        // with that type of entity are selected. The three following basis
        // functions thus define exactly the same space as above:
        { Name se; NameOfCoef ee; Function BF_HcurlLegendre; Order p;
          Support Dom_Hcurl_e_Wav; Entity EdgesOf[All]; }
        { Name sf; NameOfCoef ef; Function BF_HcurlLegendre; Order p;
          Support Dom_Hcurl_e_Wav; Entity FacetsOf[All]; }
        { Name sv; NameOfCoef ev; Function BF_HcurlLegendre; Order p;
          Support Dom_Hcurl_e_Wav; Entity VolumesOf[All]; }
        // The interest of this form is that each line is free to use another
        // order, another support or other entities. The tangential continuity
        // is preserved in all cases, since an entity always brings the same
        // functions to all the elements that share it. One can for instance
        //  - use a higher order in a part of the domain only, by giving the
        //    functions of higher order the edges, facets and volumes of that
        //    part ("EdgesOf[Region]", ...) in addition to the functions of
        //    lower order defined everywhere;
        //  - combine the functions with built-in ones. For "H1" this is how
        //    nodal functions are completed by high order functions:
        //
        //      { Name sn; NameOfCoef vn; Function BF_Node_1N;
        //        Support Dom; Entity NodesOf[All]; }
        //      { Name se; NameOfCoef ve; Function BF_H1Legendre; Order p;
        //        Support Dom; Entity EdgesOf[All]; }
        //      { Name sf; NameOfCoef vf; Function BF_H1Legendre; Order p;
        //        Support Dom; Entity FacetsOf[All]; }
        //      { Name sv; NameOfCoef vv; Function BF_H1Legendre; Order p;
        //        Support Dom; Entity VolumesOf[All]; }
        //
        //    ("BF_Node_1N" are the first order nodal functions associated
        //    with the vertices of the elements, whatever the order of the
        //    mesh; on a curved, high order mesh "BF_Node" would give the
        //    isoparametric functions of the order of the mesh instead, which
        //    cannot be completed in this way.)

      Else
        // 3. The definition function by function.
        //
        // Internally a basis function of GetDP always stands for one function
        // per entity, as with the built-in functions of the previous
        // tutorials. The two definitions above actually generate one such
        // basis function for each function of an edge, a facet or a volume.
        // They can also be given explicitly: the function number "j" of an
        // edge is "BF_HcurlLegendre_E<j>" (or "BF_HcurlLegendre_E_<j>", as
        // obtained with "~{j}" in a loop), and those of a facet and of a
        // volume are "BF_HcurlLegendre_F<j>" and "BF_HcurlLegendre_V<j>". The
        // functions of an entity are sorted by increasing order, so that the
        // space of order "p" is made of the first functions of each entity:
        //
        //                   edge     triangle       quadrangle
        //   H(curl)         p + 1    (p-1)(p+1)     2p(p+1)
        //
        //                   tetrahedron           hexahedron
        //   H(curl)         (p-2)(p-1)(p+1)/2     3p^2(p+1)
        //
        // The first function of an edge is the Whitney function: the
        // definition starts with the "BF_Edge" functions of tutorial 5
        // ("BF_HcurlLegendre_E0" is the same function), to which the other
        // functions are added.
        { Name se; NameOfCoef ee; Function BF_Edge;
          Support Dom_Hcurl_e_Wav; Entity EdgesOf[All]; }
        For j In {1:p}
          { Name se~{j}; NameOfCoef ee~{j}; Function BF_HcurlLegendre_E~{j};
            Support Dom_Hcurl_e_Wav; Entity EdgesOf[All]; }
        EndFor
        For j In {0:nF-1}
          { Name sf~{j}; NameOfCoef ef~{j}; Function BF_HcurlLegendre_F~{j};
            Support Dom_Hcurl_e_Wav; Entity FacetsOf[All]; }
        EndFor
        For j In {0:nV-1}
          { Name sv~{j}; NameOfCoef ev~{j}; Function BF_HcurlLegendre_V~{j};
            Support Dom_Hcurl_e_Wav; Entity VolumesOf[All]; }
        EndFor
        // This form gives full control over the space, e.g. to keep some of
        // the functions of an entity only, or to put some of them in a
        // "SubSpace". Its drawback is clear from the table above: the numbers
        // of functions must be known, and they depend on the shape of the
        // elements ("nF" and "nV" are set accordingly before the
        // FunctionSpace). On a mesh mixing several shapes the triangular and
        // quadrangular facets would need different loops (the functions are
        // then named "BF_HcurlLegendre_FTri<j>" and
        // "BF_HcurlLegendre_FQua<j>", and similarly "VTet", "VHex" and "VPri"
        // for the volumes), whereas the two definitions above handle this
        // automatically.
      EndIf
    }
    Constraint {
      If(def == 0)
        // A constraint on the coefficient of a function space defined by its
        // order applies to all its functions, each on its own type of entity:
        // "EntityType Auto" replaces the "EdgesOf" of tutorial 5. Here the
        // functions of the edges and of the facets (in 3D) of the walls and
        // of the port are constrained.
        { NameOfCoef ee; EntityType Auto; NameOfConstraint e_Wav; }
      ElseIf(def == 1)
        // The functions of the volumes are not concerned by a constraint on
        // a boundary:
        { NameOfCoef ee; EntityType Auto; NameOfConstraint e_Wav; }
        { NameOfCoef ef; EntityType Auto; NameOfConstraint e_Wav; }
      Else
        // Function by function, each coefficient is constrained explicitly:
        { NameOfCoef ee; EntityType EdgesOf; NameOfConstraint e_Wav; }
        For j In {1:p}
          { NameOfCoef ee~{j}; EntityType EdgesOf; NameOfConstraint e_Wav; }
        EndFor
        For j In {0:nF-1}
          { NameOfCoef ef~{j}; EntityType FacetsOf; NameOfConstraint e_Wav; }
        EndFor
      EndIf
    }
  }
}

// The formulation is that of tutorial 5: nothing in it depends on the order of
// the function space. Only the term on the output port differs, with the
// transparent condition for the mode.
Formulation {
  { Name FullWave_e; Type FemEquation;
    Quantity {
      { Name e; Type Local; NameOfSpace Hcurl_e_Wav; }
    }
    Equation {
      Integral { [ nu[] * Dof{d e} , {d e} ];
        In Vol_Wav; }
      Integral { [ -omega^2 * epsilon[] * Dof{e} , {e} ];
        In Vol_Wav; }
      Integral { [ i[] * nu[] * k^2 / beta * Dof{e} , {e} ];
        In Sur_Out_Wav; }
    }
  }
  { Name Projection_e; Type FemEquation;
    Quantity {
      { Name e; Type Local; NameOfSpace Hcurl_e_Wav; }
    }
    Equation {
      Integral { [ Dof{e} , {e} ];
        In Sur_Port_Wav; }
      Integral { [ -eExact[] , {e} ];
        In Sur_Port_Wav; }
    }
  }
}

Resolution {
  { Name Wav;
    System {
      { Name Sys_Wav; NameOfFormulation FullWave_e; Type Complex; }
    }
    Operation {
      Generate[Sys_Wav]; Solve[Sys_Wav]; SaveSolution[Sys_Wav];
    }
  }
  { Name Proj_e;
    System {
      { Name Sys_Proj; NameOfFormulation Projection_e; Type Complex;
        DestinationSystem Sys_Wav; }
    }
    Operation {
      Generate[Sys_Proj]; Solve[Sys_Proj]; TransferSolution[Sys_Proj];
    }
  }
}

// The L2 norm of the difference between the computed and the exact electric
// field is computed with an "Integral" quantity. The two integrals are stored
// in run-time variables ("$error2" and "$norm2") by the PostOperation, which
// are then combined in the "error" quantity.
PostProcessing {
  { Name Wav; NameOfFormulation FullWave_e;
    Quantity {
      { Name e; Value { Term{ [ {e} ]; In Vol_Wav; } } }
      { Name eExact;
        Value { Term{ [ eExact[] ]; In Vol_Wav; } } }
      { Name error2;
        Value {
          Integral { [ SquNorm[{e} - eExact[]] ];
            In Vol_Wav; }
        }
      }
      { Name norm2;
        Value {
          Integral { [ SquNorm[eExact[]] ];
            In Vol_Wav; }
        }
      }
      { Name error;
        Value { Term { Type Global; [ Sqrt[$error2 / $norm2] ]; In Vol_Wav; } }
      }
    }
  }
}

PostOperation {
  { Name Map; NameOfPostProcessing Wav;
    Operation {
      // A field of order "p" is not well represented by its values at the
      // nodes of a coarse mesh. With "Depth" each element is recursively
      // subdivided before the field is evaluated, which gives a faithful
      // picture of what was actually computed (the subdivision is not
      // available for hexahedra, and is limited in 3D to keep the size of the
      // file reasonable):
      If(dim == 2)
        Print [ e, OnElementsOf Vol_Wav, Depth 3, File "e.pos" ];
      ElseIf(shape == 0)
        Print [ e, OnElementsOf Vol_Wav, Depth 2, File "e.pos" ];
      Else
        Print [ e, OnElementsOf Vol_Wav, File "e.pos" ];
      EndIf
      Print [ error2[Vol_Wav], OnGlobal, Format Table,
        StoreInVariable $error2, File > "error.txt" ];
      Print [ norm2[Vol_Wav], OnGlobal, Format Table,
        StoreInVariable $norm2, File > "error.txt" ];
      // The relative error is printed and sent to the Gmsh graphical
      // interface. With the default mesh, which has about four elements per
      // wavelength, it is around 30% in 2D at order 0: this mesh is useless
      // for the Whitney elements of tutorial 5. The error is 2% at order 2,
      // 0.1% at order 3 and falls by more than an order of magnitude with
      // each further order. (Order 1 is not better than order 0 on such a
      // coarse mesh; it becomes so as soon as the mesh is refined. Divide the
      // mesh size by two to check that the error is divided by about
      // "2^(p+1)".)
      Print [ error, OnRegion Vol_Wav, Format Table,
        SendToServer "Output/Relative L2 error on e", File "error.txt" ];
    }
  }
}
