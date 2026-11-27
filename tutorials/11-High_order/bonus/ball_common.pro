// Parameters shared by Gmsh and GetDP: model dimension and order of the mesh.

DefineConstant[
  dim = {2, Choices{2="2D (disk)", 3="3D (ball)"},
    Name "Parameters/0Model dimension"}
  order = {1, Min 1, Max 4, Step 1,
    Name "Parameters/1Order of the mesh"}
];
