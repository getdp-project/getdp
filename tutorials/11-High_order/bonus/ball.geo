// Gmsh script for the unit disk (2D) or the unit ball (3D), meshed with
// triangles or tetrahedra of order 1 to 4.

SetFactory("OpenCASCADE");

Include "ball_common.pro";

DefineConstant[
  res = {0.5, Min 0.05, Max 1, Step 0.05,
    Name "Parameters/Mesh size"}
];

If(dim == 2)
  Disk(1) = {0, 0, 0, 1};
Else
  Sphere(1) = {0, 0, 0, 1};
EndIf

// A uniform, coarse mesh (the mesh size is half the radius by default):
Mesh.MeshSizeFromCurvature = 0;
Mesh.MeshSizeMin = res;
Mesh.MeshSizeMax = res;

// The order of the mesh is the order of the polynomials that describe the
// shape of the elements. Elements of order 1 have straight edges: the disk is
// then approximated by a polygon. With elements of order 2 or more the edges
// (and the faces) on the boundary are curved, with additional nodes placed on
// the circle (or on the sphere). The elements are optimized to make sure that
// they remain valid once curved.
Mesh.ElementOrder = order;
Mesh.HighOrderOptimize = (order > 1) ? 2 : 0;

Physical GeoEntity{dim}("Domain", 1) = {1};
bnd() = CombinedBoundary{ GeoEntity{dim}{1}; };
Physical GeoEntity{dim - 1}("Boundary", 2) = {bnd()};
