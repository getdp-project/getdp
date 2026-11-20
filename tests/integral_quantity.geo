// Geometry of integral_quantity.pro: a disk (Core), its boundary (gamma) and
// the exterior, which is only used for post-processing

lc = 0.2;
lc2 = 2;

Point(1) = {0,0,0,lc};
Point(2) = {1,0,0,lc};
Point(3) = {-1,0,0,lc};
Point(4) = {0,1,0,lc};
Point(5) = {0,-1,0,lc};

Point(6) = {-10,-10,0,lc2};
Point(7) = {10,-10,0,lc2};
Point(8) = {10,10,0,lc2};
Point(9) = {-10,10,0,lc2};

Line(1) = {6,7};
Line(2) = {7,8};
Line(3) = {8,9};
Line(4) = {9,6};
Circle(5) = {2,1,4};
Circle(6) = {4,1,3};
Circle(7) = {3,1,5};
Circle(8) = {5,1,2};

Line Loop(9) = {5,6,7,8};
Plane Surface(10) = {9};
Line Loop(11) = {2,3,4,1};
Plane Surface(12) = {11,9};

Gamma = 1111 ;
Inf   = 2222 ;
Core  = 3333 ;
Ext   = 4444 ;

Physical Line(Gamma) = {-5,-6,-7,-8};
Physical Line(Inf) = {1,2,3,4};
Physical Surface(Core) = {10};
Physical Surface(Ext) = {12};

Physical Point(5555) = {2};
