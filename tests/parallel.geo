// Unit square for the tests of parallel.pro; the mesh of the right side is a
// copy of the mesh of the left side (for the periodic problem)

SetFactory("OpenCASCADE");
Rectangle(1) = {0, 0, 0, 1, 1};
Periodic Curve {2} = {4} Translate {1, 0, 0};
Mesh.MeshSizeMax = 0.03;

Physical Surface("Domain", 1) = {1};
Physical Curve("Left", 2) = {4};
Physical Curve("Right", 3) = {2};
