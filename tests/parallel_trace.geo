// unit cube with an interface (region 3) at z = 0.5, for parallel_trace.pro
SetFactory("OpenCASCADE");
Mesh.MeshSizeMax = 0.2; Mesh.MeshSizeMin = 0.2;
Box(1) = {0, 0, 0, 1, 1, 0.5};
Box(2) = {0, 0, 0.5, 1, 1, 0.5};
BooleanFragments{ Volume{1}; Delete; }{ Volume{2}; Delete; }
mid() = Surface In BoundingBox{-0.1, -0.1, 0.49, 1.1, 1.1, 0.51};
Physical Volume(1) = Volume{:};
Physical Surface(2) = Abs(CombinedBoundary{ Volume{:}; });
Physical Surface(3) = mid();
