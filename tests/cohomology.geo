// The induction heating device of onelab_doc/models/Cohomology with a single
// turn and a coarse mesh, for the tests of cohomology.pro: a coil, whose
// terminals are on the boundary of the box of air, around a conducting tube.
// Gmsh computes the cuts of the T-Omega and A-v formulations (Cohomology).

lc = 0.05;
r = 0.11; // coil radius
rc = 0.01; // coil wire radius
hc = 0.2; // coil height
ht = 0.3; // tube height
rt1 = 0.081; // tube internal radius
rt2 = 0.092; // tube external radius
lb = 1; // box width

// coil: a circle extruded along a helix, then to the left side of the box
p = newp;
Point(p) = {0, -r, -hc/2, lc};
Point(p+1) = {0, -r+rc, -hc/2, lc};
Point(p+2) = {0, -r, -hc/2+rc, lc};
Point(p+3) = {0, -r-rc, -hc/2, lc};
Point(p+4) = {0, -r, -hc/2-rc, lc};
c = newl;
Circle(c) = {p+1, p, p+2};
Circle(c+1) = {p+2, p, p+3};
Circle(c+2) = {p+3, p, p+4};
Circle(c+3) = {p+4, p, p+1};
ll = newll;
Line Loop(ll) = {c, c+1, c+2, c+3};
s = news;
Plane Surface(s) = {ll};
tmp[] = {s};
vol_coil[] = {};
For j In {1:6} // a turn and a half, to end on the left
  tmp[] = Extrude { {0, 0, hc/4}, {0, 0, 1}, {0, 0, 0}, Pi/2 }
    { Surface{tmp[0]}; };
  vol_coil[] += tmp[1];
EndFor
tmp[] = Extrude {-lb/2, 0, 0} { Surface{tmp[0]}; };
vol_coil[] += tmp[1];
out = tmp[0];
tmp[] = Extrude {-lb/2, 0, 0} { Surface{s}; };
vol_coil[] += tmp[1];
in = tmp[0];

// tube
p = newp;
Point(p) = {0, 0, -ht/2, lc};
Point(p+1) = {rt1, 0, -ht/2, lc};
Point(p+2) = {0, rt1, -ht/2, lc};
Point(p+3) = {-rt1, 0, -ht/2, lc};
Point(p+4) = {0, -rt1, -ht/2, lc};
Point(p+5) = {rt2, 0, -ht/2, lc};
Point(p+6) = {0, rt2, -ht/2, lc};
Point(p+7) = {-rt2, 0, -ht/2, lc};
Point(p+8) = {0, -rt2, -ht/2, lc};
c = newc;
Circle(c) = {p+1, p, p+2};
Circle(c+1) = {p+2, p, p+3};
Circle(c+2) = {p+3, p, p+4};
Circle(c+3) = {p+4, p, p+1};
Circle(c+4) = {p+5, p, p+6};
Circle(c+5) = {p+6, p, p+7};
Circle(c+6) = {p+7, p, p+8};
Circle(c+7) = {p+8, p, p+5};
ll = newll;
Line Loop(ll) = {c+4, c+5, c+6, c+7, -c, -(c+1), -(c+2), -(c+3)};
s = news;
Plane Surface(s) = {ll};
tmp[] = Extrude {0, 0, ht} { Surface{s}; };
vol_tube = tmp[1];
pt = p + 5; // a point of the tube

// box, whose left side has the terminals of the coil
p = newp;
Point(p) = {-lb/2, -lb/2, -lb/2};
Point(p+1) = {lb/2, -lb/2, -lb/2};
Point(p+2) = {lb/2, lb/2, -lb/2};
Point(p+3) = {-lb/2, lb/2, -lb/2};
Point(p+4) = {-lb/2, -lb/2, lb/2};
Point(p+5) = {lb/2, -lb/2, lb/2};
Point(p+6) = {lb/2, lb/2, lb/2};
Point(p+7) = {-lb/2, lb/2, lb/2};
l = newl;
Line(l) = {p, p+1};
Line(l+1) = {p+1, p+2};
Line(l+2) = {p+2, p+3};
Line(l+3) = {p+3, p};
Line(l+4) = {p+4, p+5};
Line(l+5) = {p+5, p+6};
Line(l+6) = {p+6, p+7};
Line(l+7) = {p+7, p+4};
Line(l+8) = {p, p+4};
Line(l+9) = {p+1, p+5};
Line(l+10) = {p+2, p+6};
Line(l+11) = {p+3, p+7};
ll = newll;
Line Loop(ll) = Boundary{ Surface{in}; };
Line Loop(ll+1) = {l+8, -(l+7), -(l+11), l+3};
Line Loop(ll+2) = Boundary{ Surface{out}; };
Line Loop(ll+3) = {l+9, l+5, -(l+10), -(l+1)};
Line Loop(ll+4) = {l, l+1, l+2, l+3};
Line Loop(ll+5) = {l+4, l+5, l+6, l+7};
Line Loop(ll+6) = {l+2, l+11, -(l+6), -(l+10)};
Line Loop(ll+7) = {l, l+9, -(l+4), -(l+8)};
s = news;
Plane Surface(s) = {ll+1, ll, ll+2};
Plane Surface(s+1) = {ll+3};
Plane Surface(s+2) = {ll+4};
Plane Surface(s+3) = {ll+5};
Plane Surface(s+4) = {ll+6};
Plane Surface(s+5) = {ll+7};
skin_coil[] = CombinedBoundary{ Volume{vol_coil[]}; };
skin_coil[] -= {in, out};
sl = newsl;
Surface Loop(sl) = {s:s+5, skin_coil[]};
Surface Loop(sl+1) = CombinedBoundary{ Volume{vol_tube}; };
v = newv;
Volume(v) = {sl, sl+1};

Physical Volume(1000) = {vol_coil[]}; // coil
Physical Volume(1001) = {vol_tube}; // tube
Physical Volume(1002) = {v}; // air
Physical Surface(2000) = {skin_coil[]};
Physical Surface(2001) = CombinedBoundary{ Volume{vol_tube}; };
Physical Surface(2002) = in;
Physical Surface(2003) = out;
Physical Surface(2004) = {s:s+5}; // boundary of the box
// where phi and v are fixed (below 2005, the first tag of the cuts)
Physical Point(1003) = {p+6}; // a corner of the box
Physical Point(1004) = {pt}; // a point of the tube

// the cuts (physical groups 2005 to 2008)
// T-Omega: the complements of the coil and of the tube
Cohomology(1) {{1002, 1001}, {}};
Cohomology(1) {{1002, 1000}, {}};
// A-v: the coil relative to its terminals, and the tube
Cohomology(1) {{1000}, {2002, 2003}};
Cohomology(1) {{1001}, {}};
