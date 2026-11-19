// Ball with a core for the tests of search.pro: the boundary of the ball and
// the surface of the core are made of triangles in all directions

SetFactory("OpenCASCADE");
Sphere(1) = {0, 0, 0, 0.4};
Sphere(2) = {0, 0, 0, 1};
BooleanFragments{ Volume{2}; Delete; }{ Volume{1}; Delete; }
Mesh.MeshSizeMax = 0.15;

Physical Volume("Core", 1) = {1};
Physical Volume("Shell", 2) = {2};
Physical Surface("CoreSkin", 3) = {Boundary{ Volume{1}; }};
Physical Surface("Outside", 4) = {CombinedBoundary{ Volume{1, 2}; }};
