// unit square or cube; shape: 0 simplices, 1 quadrangles/hexahedra, 2 prisms
// (3D), 3 mixed (triangles and quadrangles; tetrahedra and prisms), 4 mixed
// (hexahedra and prisms, 3D), 5 structured quadrangles/hexahedra
SetFactory("OpenCASCADE");
// per: 1 for a mesh periodic along x (regions 3 at x = 0 and 4 at x = 1), 2
// for a mesh periodic along x and y (regions 5 at y = 0 and 6 at y = 1)
DefineConstant[ dim = 2, lc = 0.25, shape = 0, per = 0 ];
e = 1e-3;
Mesh.MeshSizeMax = lc; Mesh.MeshSizeMin = lc;
n = Ceil[1/lc];
If(dim == 2)
  If(shape != 3)
    Rectangle(1) = {0, 0, 0, 1, 1};
    If(shape == 5)
      Transfinite Curve{:} = n + 1;
      Transfinite Surface{1};
      Recombine Surface{1};
    EndIf
    If(shape == 1)
      Mesh.SubdivisionAlgorithm = 1; // all quadrangles
      Mesh.MeshSizeMax = 2*lc; Mesh.MeshSizeMin = 2*lc;
    EndIf
  Else
    Rectangle(1) = {0, 0, 0, 0.5, 1};
    Rectangle(2) = {0.5, 0, 0, 0.5, 1};
    BooleanFragments{ Surface{1}; Delete; }{ Surface{2}; Delete; }
    Recombine Surface{1};
  EndIf
  bnd() = Abs(CombinedBoundary{ Surface{:}; });
  If(per)
    left() = Curve In BoundingBox{-e, -e, -e, e, 1+e, e};
    right() = Curve In BoundingBox{1-e, -e, -e, 1+e, 1+e, e};
    Periodic Curve{right()} = {left()} Translate{1, 0, 0};
    bnd() -= left(); bnd() -= right();
    Physical Curve(3) = left(); Physical Curve(4) = right();
  EndIf
  If(per == 2)
    bottom() = Curve In BoundingBox{-e, -e, -e, 1+e, e, e};
    top() = Curve In BoundingBox{-e, 1-e, -e, 1+e, 1+e, e};
    Periodic Curve{top()} = {bottom()} Translate{0, 1, 0};
    bnd() -= bottom(); bnd() -= top();
    Physical Curve(5) = bottom(); Physical Curve(6) = top();
  EndIf
  Physical Surface(1) = Surface{:};
  Physical Curve(2) = bnd();
Else
  If(shape == 0 || shape == 1)
    Box(1) = {0, 0, 0, 1, 1, 1};
    If(shape == 1)
      Mesh.SubdivisionAlgorithm = 2; // all hexahedra
      Mesh.MeshSizeMax = 2*lc; Mesh.MeshSizeMin = 2*lc;
    EndIf
  ElseIf(shape == 2)
    Rectangle(1) = {0, 0, 0, 1, 1};
    Extrude {0, 0, 1} { Surface{1}; Layers{n}; Recombine; }
  ElseIf(shape == 5)
    Rectangle(1) = {0, 0, 0, 1, 1};
    Transfinite Curve{:} = n + 1;
    Transfinite Surface{1};
    Recombine Surface{1};
    Extrude {0, 0, 1} { Surface{1}; Layers{n}; Recombine; }
  ElseIf(shape == 3)
    Box(1) = {0, 0, 0, 1, 1, 0.5};
    mid() = Surface In BoundingBox{-0.1, -0.1, 0.49, 1.1, 1.1, 0.51};
    Extrude {0, 0, 0.5} { Surface{mid()}; Layers{Ceil[n/2]}; Recombine; }
  Else
    Rectangle(1) = {0, 0, 0, 0.5, 1};
    Rectangle(2) = {0.5, 0, 0, 0.5, 1};
    BooleanFragments{ Surface{1}; Delete; }{ Surface{2}; Delete; }
    Recombine Surface{1};
    Extrude {0, 0, 1} { Surface{1, 2}; Layers{n}; Recombine; }
  EndIf
  bnd() = Abs(CombinedBoundary{ Volume{:}; });
  If(per)
    If(shape == 0 || shape == 1 || shape == 3)
      // the faces of the box
      zmax = (shape == 3) ? 0.5 : 1;
      l() = Surface In BoundingBox{-e, -e, -e, e, 1+e, zmax+e};
      r() = Surface In BoundingBox{1-e, -e, -e, 1+e, 1+e, zmax+e};
      Periodic Surface{r()} = {l()} Translate{1, 0, 0};
    Else
      // the curves of the extruded surface: the extruded faces follow
      l() = Curve In BoundingBox{-e, -e, -e, e, 1+e, e};
      r() = Curve In BoundingBox{1-e, -e, -e, 1+e, 1+e, e};
      Periodic Curve{r()} = {l()} Translate{1, 0, 0};
    EndIf
    left() = Surface In BoundingBox{-e, -e, -e, e, 1+e, 1+e};
    right() = Surface In BoundingBox{1-e, -e, -e, 1+e, 1+e, 1+e};
    bnd() -= left(); bnd() -= right();
    Physical Surface(3) = left(); Physical Surface(4) = right();
  EndIf
  If(per == 2)
    If(shape == 0 || shape == 1 || shape == 3)
      zmax = (shape == 3) ? 0.5 : 1;
      b() = Surface In BoundingBox{-e, -e, -e, 1+e, e, zmax+e};
      t() = Surface In BoundingBox{-e, 1-e, -e, 1+e, 1+e, zmax+e};
      Periodic Surface{t()} = {b()} Translate{0, 1, 0};
    Else
      b() = Curve In BoundingBox{-e, -e, -e, 1+e, e, e};
      t() = Curve In BoundingBox{-e, 1-e, -e, 1+e, 1+e, e};
      Periodic Curve{t()} = {b()} Translate{0, 1, 0};
    EndIf
    bottom() = Surface In BoundingBox{-e, -e, -e, 1+e, e, 1+e};
    top() = Surface In BoundingBox{-e, 1-e, -e, 1+e, 1+e, 1+e};
    bnd() -= bottom(); bnd() -= top();
    Physical Surface(5) = bottom(); Physical Surface(6) = top();
  EndIf
  Physical Volume(1) = Volume{:};
  Physical Surface(2) = bnd();
EndIf
