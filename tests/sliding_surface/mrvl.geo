Include "mrvl_common.pro";

Mesh.SurfaceEdges=0;

lc1 = lc; // fine
lc2 = 3*lc;
lc3 = 8*lc; // coarse

// 1. R A I L - S T A T O R
bnd_slft() = {};
bnd_srgh() = {};
bnd_sbot() = {};

// 1.1 Stator core
pA = newp; Point(pA) = {0,0,0,lc3};
pB = newp; Point(pB) = {L,0,0,lc3};
lAB = newl; Line(lAB) = {pA,pB};
bnd_sbot() += { lAB };
pD = newp; Point(pD) = {L,cs,0,lc2};
lBD = newl; Line(lBD) = {pB,pD};
bnd_srgh() += { lBD };

p5 = pD;
For i In {0:NbrStaPer-1}
  p1 = newp; Point(p1) = {L-i*(ws+vs)-vs/2,cs,0,lc2};
  Line(newl) = {p5,p1};
  p2 = newp; Point(p2) = {L-i*(ws+vs)-vs/2,cs+hs,0,lc1};
  Line(newl) = {p1,p2};
  p3 = newp; Point(p3) = {L-i*(ws+vs)-vs/2-ws,cs+hs,0,lc1};
  Line(newl) = {p2,p3};
  p4 = newp; Point(p4) = {L-i*(ws+vs)-vs/2-ws,cs,0,lc2};
  Line(newl) = {p3,p4};
  p5 = newp; Point(p5) = {L-i*(ws+vs)-vs-ws,cs,0,lc2};
  Line(newl) = {p4,p5};
EndFor
pC = p5;
lCA = newl; Line(lCA)={pC,pA};
bnd_slft() += { lCA };

ll_sc = newll; Line Loop(ll_sc) = { lAB, lBD ... lCA };
sStatorCore = news; Plane Surface(sStatorCore) = {ll_sc};

// 1.2 Air stator
pE = newp; Point(pE) = {0,cs+hs+ag/3,0,lc};
pF = newp; Point(pF) = {L,cs+hs+ag/3,0,lc};
lDF = newl; Line(lDF)={pD,pF};
lFE = newl; Line(lFE)={pF,pE};
lEC = newl; Line(lEC)={pE,pC};
bnd_srgh() += {lDF};
bnd_slft() += {lEC};
lags = lFE;

ll_as = newll; Line Loop(ll_as) = { (-(lCA-1)) ... (-(lBD+1)), lDF, lFE, lEC  };
sAirStator = news; Plane Surface(sAirStator) = {ll_as};

pGz = newp; Point(pGz) = {0,cs+hs+2*ag/3,SlidingSurfaceShift,lc1};
pHz = newp; Point(pHz) = {L,cs+hs+2*ag/3,SlidingSurfaceShift,lc1};

l1 = -lFE;
l2 = newl; Line(l2) = {pF,pHz};
l3 = newl; Line(l3) = {pHz,pGz};
l4 = newl; Line(l4) = {pGz,pE};
bnd_srgh() += {l2};
bnd_slft() += {l4};
lags  = l1;
lagsz = l3;

ll_al = newll; Line Loop(ll_al) = { l1, l2 ... l4 };
sAirLayer = news; Plane Surface(sAirLayer) = {ll_al};

// 2. T R A N S L A T E U R  ( M O V E R )
bnd_mrlft() = {};
bnd_mrgh() = {};
bnd_mtop() = {};


// 2.1 Air gap layer
pG = newp; Point(pG) = {0,cs+hs+2*ag/3,0,lc1};
pH = newp; Point(pH) = {L,cs+hs+2*ag/3,0,lc1};
pI = newp; Point(pI) = {0,cs+hs+hb ,0,lc3};
pJ = newp; Point(pJ) = {L,cs+hs+hb,0,lc3};
l1 = newl; Line(l1) = {pG,pH};
l2 = newl; Line(l2) = {pH,pJ};
l3 = newl; Line(l3) = {pJ,pI};
l4 = newl; Line(l4) = {pI,pG};
bnd_mrgh() += {l2};
bnd_mtop() += {l3};
bnd_mlft() += {l4};
lagm  = l1;

ll_am = newll; Line Loop(ll_am) = { l1, l2 ... l4 };

// 2.2 Mover core and coils
ym = cs+hs+ag;
Lm = 6*wt+5*vt;

pI = newp; Point(pI) = {(L-Lm)/2,ym+ht+ct,0,lc2};
pJ = newp; Point(pJ) = {(L+Lm)/2,ym+ht+vt,0,lc2};

llCoils() = {};
llCore() = {};

pA = newp+1;
For i In {0:5}
  p1 = newp; Point(p1) = {(L-Lm)/2-vw   +i*(wt+vt),ym+ht,0,lc};
  If(i>0)
    l0 = newl; Line(l0) = {p4,p1}; // connect with previous tooth
  EndIf
  p2 = newp; Point(p2) = {(L-Lm)/2      +i*(wt+vt),ym+ht,0,lc2};
  p3 = newp; Point(p3) = {(L-Lm)/2+wt   +i*(wt+vt),ym+ht,0,lc2};
  p4 = newp; Point(p4) = {(L-Lm)/2+wt+vw+i*(wt+vt),ym+ht,0,lc2};
  p5 = newp; Point(p5) = {(L-Lm)/2-vw   +i*(wt+vt),ym+ht-hw,0,lc2};
  p6 = newp; Point(p6) = {(L-Lm)/2      +i*(wt+vt),ym+ht-hw,0,lc2};
  p7 = newp; Point(p7) = {(L-Lm)/2+wt   +i*(wt+vt),ym+ht-hw,0,lc2};
  p8 = newp; Point(p8) = {(L-Lm)/2+wt+vw+i*(wt+vt),ym+ht-hw,0,lc2};
  p9 = newp; Point(p9) = {(L-Lm)/2   +i*(wt+vt),ym,0,lc1};
  p10 = newp; Point(p10) = {(L-Lm)/2+wt+i*(wt+vt),ym,0,lc1};

  l1 = newl; Line(l1) = {p5,p6};
  l2 = newl; Line(l2) = {p6,p2};
  l3 = newl; Line(l3) = {p2,p1};
  l4 = newl; Line(l4) = {p1,p5};
  ll1 = newll; Line Loop(ll1) = { l1 ... l4 };

  l5 = newl; Line(l5) = {p7,p8};
  l6 = newl; Line(l6) = {p8,p4};
  l7 = newl; Line(l7) = {p4,p3};
  l8 = newl; Line(l8) = {p3,p7};
  ll2 = newll; Line Loop(ll2) = { l5 ... l8 };

  l9 = newl; Line(l9) = {p6,p9};
  l10 = newl; Line(l10) = {p9,p10};
  l11 = newl; Line(l11) = {p10,p7};

  llCoils() += {ll1, ll2};

  llCore() += { -l2, l9 ... l11, -l8};
  If(i>0)
    llCore() += {l0,-l3};
  EndIf
  If(i<5)
	llCore() += {-l7};
  EndIf
EndFor
pB = p3;
l1 = newl; Line(l1) = {pB,pJ};
l2 = newl; Line(l2) = {pJ,pI};
l3 = newl; Line(l3) = {pI,pA};

sCoils() = {};
For i In {0:#llCoils()-1}
  sc = news; Plane Surface(sc) = {llCoils(i)};
  sCoils() += {sc};
EndFor

llc = newll; Line Loop(llc) = { llCore(), l1 ... l3 };
sMoverCore = news; Plane Surface(sMoverCore) = {llc()};

sAirMover = news; Plane Surface(sAirMover) = {ll_am,-llc,-llCoils()};

Transfinite Line{ lags, lagsz, lagm } = NbrDiv+1 Using Power 1.;

// P H Y S I C A L   R E G I O N S

Physical Surface("StatorCore", 1) = {sStatorCore};
Physical Surface("MoverCore" , 2) = {sMoverCore};
Physical Surface("AirStator" , 3) = {sAirStator};
Physical Surface("AirLayer"  , 4) = {sAirLayer};
Physical Surface("AirMover"  , 5) = {sAirMover};

Physical Surface("COILAP",  6) = {sCoils(0),sCoils(7)};
Physical Surface("COILAN",  7) = {sCoils(1),sCoils(6)};
Physical Surface("COILBP",  8) = {sCoils(2),sCoils(9)};
Physical Surface("COILBN",  9) = {sCoils(3),sCoils(8)};
Physical Surface("COILCP", 10) = {sCoils(4),sCoils(11)};
Physical Surface("COILCN", 11) = {sCoils(5),sCoils(10)};


Physical Line("Noflux" , 12) = {bnd_sbot(),bnd_mtop()};

Physical Line("MoverPerMaster" , 13) = {bnd_mlft()};
Physical Line("MoverPerSlave"  , 14) = {bnd_mrgh()};
Physical Line("StatorPerMaster", 15) = {bnd_slft()};
Physical Line("StatorPerSlave" , 16) = {bnd_srgh()};
Physical Line("SlidingMaster"  , 17) = {lagm()};
Physical Line("SlidingSlave"   , 18) = {lagsz()};

Physical Point("Sliding_Submaster", 20) = {pHz};
Physical Point("Sliding_Subslave",  21) = {pH};


// Ensure identical meshes on the periodic faces
For num In {0:#bnd_mlft()-1}
  Periodic Line { bnd_mrgh(num) } 
                 = { bnd_mlft(num) } Translate {L,0,0} ;
EndFor
For num In {0:#bnd_slft()-1}
  Periodic Line { bnd_srgh(num) } 
                 = { bnd_slft(num) } Translate {L,0,0} ;
EndFor

nicepos[] = Boundary{ Surface { Surface '*'};};
Physical Line("NICEPOS", 19) = {nicepos[]};
