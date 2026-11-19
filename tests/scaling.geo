// Unit cube for the scaling benchmark (scaling.pro, utils/misc/scaling.py),
// meshed with 6 N^3 tetrahedra

SetFactory("OpenCASCADE");
DefineConstant[ N = 8 ];

Box(1) = {0, 0, 0, 1, 1, 1};
Transfinite Curve{:} = N + 1;
Transfinite Surface{:};
Transfinite Volume{:};

Physical Volume("Domain", 1) = {1};
Physical Surface("Boundary", 2) = {1, 2, 3, 4, 5, 6};
