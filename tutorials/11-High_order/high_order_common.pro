// Parameters shared by Gmsh and GetDP: model dimension, shape of the mesh
// elements and waveguide dimensions.

DefineConstant[
  dim = {2, Choices{2="2D", 3="3D"},
    Name "Parameters/0Model dimension"}
  shape = {0, Choices{0="Triangles / tetrahedra", 1="Quadrangles / hexahedra"},
    Name "Parameters/1Shape of the mesh elements"}
  Wx = {4, Min 1, Max 100, Step 1,
    Name "Parameters/Waveguide width [cm]"}
  Wz = {3, Min 1, Max 100, Step 1, Visible (dim == 3),
    Name "Parameters/Waveguide height [cm]"}
  L = {9, Min 1, Max 100, Step 1,
    Name "Parameters/Waveguide length [cm]"}
];

// back to meters:
Wx = Wx / 100;
Wz = Wz / 100;
L = L / 100;
