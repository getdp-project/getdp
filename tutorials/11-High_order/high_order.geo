// Gmsh script describing a straight section of the rectangular waveguide of
// tutorial 5: a rectangle (2D) or a box (3D), with the input port at "y = 0"
// and the output port at "y = L".

SetFactory("OpenCASCADE");

Include "high_order_common.pro";

// The mesh is deliberately coarse: by default the mesh size is about one
// fourth of the wavelength, which is far too coarse for the lowest order
// elements of tutorial 5. The accuracy will come from the order of the basis
// functions, not from the mesh.
DefineConstant[
  res = {1, Min 1e-1, Max 5, Step 1e-1,
    Name "Parameters/Mesh size [cm]"}
];
res = res / 100;

If(dim == 2)
  Rectangle(1) = {0, 0, 0, Wx, L};
Else
  // (OpenCASCADE boxes are aligned with the axes; the waveguide is along "y")
  Box(1) = {0, 0, 0, Wx, L, Wz};
EndIf

MeshSize{:} = res;

If(shape == 1)
  // Structured mesh of quadrangles (2D) or hexahedra (3D):
  cx() = Curve In BoundingBox {-1, -1, -1, 1, 1, 1};
  For i In {0 : #cx() - 1}
    bb() = BoundingBox Curve{cx(i)};
    len = Sqrt[(bb(3) - bb(0))^2 + (bb(4) - bb(1))^2 + (bb(5) - bb(2))^2];
    Transfinite Curve{cx(i)} = Ceil[len / res] + 1;
  EndFor
  Transfinite Surface{:};
  Recombine Surface{:};
  If(dim == 3)
    Transfinite Volume{:};
  EndIf
EndIf

// The boundary: the input port at "y = 0", the output port at "y = L" and the
// perfectly conducting walls.
tol = 1e-6;
sPort() = GeoEntity{dim - 1} In BoundingBox {
  -tol, -tol, -tol, Wx + tol, tol, Wz + tol};
sOut() = GeoEntity{dim - 1} In BoundingBox {
  -tol, L - tol, -tol, Wx + tol, L + tol, Wz + tol};
sPEC() = CombinedBoundary{ GeoEntity{dim}{:}; };
sPEC() -= {sPort(), sOut()};

Physical GeoEntity{dim}("Waveguide", 1000) = {1};
Physical GeoEntity{dim - 1}("PEC", 2000) = {sPEC()};
Physical GeoEntity{dim - 1}("Input port", 2001) = {sPort()};
Physical GeoEntity{dim - 1}("Output port", 2002) = {sOut()};
