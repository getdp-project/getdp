// Unit cube for the scaling benchmark (scaling.pro, utils/misc/scaling.py),
// meshed with 6 N^3 tetrahedra (N layers of N layers of N segments)

DefineConstant[ N = 8 ];

Point(1) = {0, 0, 0};
Point(2) = {1, 0, 0};
Line(1) = {1, 2};
Transfinite Curve{1} = N + 1;
s[] = Extrude {0, 1, 0} { Curve{1}; Layers{N}; };
v[] = Extrude {0, 0, 1} { Surface{s[1]}; Layers{N}; };

Physical Volume("Domain", 1) = {v[1]};
Physical Surface("Boundary", 2) = Abs(Boundary{ Volume{v[1]}; });
