mm = 1.e-3 ;
deg = Pi/180.;

lc = mm * DefineNumber[2, Name "Options/Global mesh size (mm)"];
If(lc == 0)
  Error("Glabal mesh size cannot be zero. Reset to default."); lc=2*mm;
EndIf


// Geometrical parameters
wd = 40*mm;
ag =  2*mm; // 1*mm

// Rail-stator
ws = 15*mm;
cs = 15*mm;
vs = 21*mm;
hs = 10*mm;
Ps = 36*mm;

// Translateur - rotor
wt = 12*mm;
ct = 12*mm;
vt = 12*mm;
ht = 33*mm;
Pt = 24*mm;

// Bobines - windings
hw = 20*mm; // max 30*mm
vw = 4*mm; // max 5.5*mm

// Air box
hb = ag+ht+ct+20*mm;

NbrStaPer = 7;
L = NbrStaPer*(ws+vs);
Lm = 6*wt+5*vt;
ym = cs+hs+ag;


// Number of elements on the sliding surface in rotation direction
NbrDiv = L/lc; 

SlidingSurfaceShift = 1.e-4;

