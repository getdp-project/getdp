/* Test of a moving mesh with a sliding surface (ChangeOfCoordinates and Link
   constraints that change at each step): the linear reluctance motor of the
   ONELAB model "SlidingSurface2DLin" (see https://onelab.info).

   getdp mrvl.pro -setstring R_ QuasiStatic -setnumber NumStep 6
     -solve QuasiStatic -pos Fields */

// ALSTOM linear reluctance motor
// Fr. Henrotte

Include "mrvl_common.pro";

DefineConstant[
  R_ = {"Static", Choices {"Static", "QuasiStatic"}, Visible 1,
	Name "GetDP/1ResolutionChoices"},
  C_ = {"-solve -v 4 -pos -2", Name "GetDP/9ComputeCommand", Visible 0}
  P_ = { "", Choices {"Fields", "Check_Periodicity"}, Visible 0,
	 Name "GetDP/2PostOperationChoices"}
];

Flag_QuasiStatic = !StrCmp[ R_,"QuasiStatic"] ;
MoverPosition = mm*
  DefineNumber[0, Name "Options/Mover position [mm]",
	       Visible !Flag_QuasiStatic];
TranslationStep = mm*
  DefineNumber[1, Name "Options/Translation step [mm]",
	       Visible Flag_QuasiStatic];
NumStep = 
  DefineNumber[20, Name "Options/Number of steps",
	       Visible Flag_QuasiStatic];

velocity = 1 ; // m/s 
Mag_DTime = TranslationStep/velocity ;
Mag_TimeMax = Mag_DTime*NumStep; 

Group {
  // Geometrical regions
  StatorCore = Region[ 1 ] ;
  MoverCore  = Region[ 2 ] ;
  AirStator  = Region[ 3 ] ;
  AirLayer   = Region[ 4 ] ;
  AirMover   = Region[ 5 ] ;

  CoilAP     = Region[ 6 ] ; 
  CoilAN     = Region[ 7 ] ; 
  CoilBP     = Region[ 8 ] ; 
  CoilBN     = Region[ 9 ] ; 
  CoilCP     = Region[ 10 ] ; 
  CoilCN     = Region[ 11 ] ; 

  NoFlux     = Region[ 12 ] ; 

  Sur_MoverPerMaster  = Region[ 13 ] ;
  Sur_MoverPerSlave   = Region[ 14 ] ;  
  Sur_StatorPerMaster = Region[ 15 ] ;
  Sur_StatorPerSlave  = Region[ 16 ] ;
  Sur_SlidingSlave    = Region[ 17 ] ;  
  Sur_SlidingMaster   = Region[ 18 ] ;
 
  NICEPOS = Region[ 19 ] ;

  Lin_SlidingSubslave  = Region[ 20 ] ;
  Lin_SlidingSubmaster = Region[ 21 ] ;

  // Abstract regions
  Inductors = Region[ {CoilAP, CoilBP, CoilCP, CoilAN, CoilBN, CoilCN} ] ;
  Iron = Region[ {StatorCore, MoverCore} ] ;
  Air = Region[ {AirStator, AirMover, AirLayer} ] ;

  Vol_Conductors  = Region[ { Inductors } ] ;
  Vol_Mag = Region[ { Air, Iron, Inductors } ] ;
  Sur_Dirichlet = Region[ { NoFlux } ] ;
  Vol_Moving = Region[ { MoverCore, AirMover, Inductors } ] ;

  Sur_Link = Region[ {Sur_SlidingMaster, Sur_SlidingSlave, 
		      Sur_StatorPerMaster, Sur_StatorPerSlave,
		      Sur_MoverPerMaster, Sur_MoverPerSlave} ] ; 

  Dom_Hcurl_a = Region[ {Vol_Mag, Sur_Dirichlet, Sur_Link} ] ;
}

Function {
  mu0 = 4.e-7*Pi ;
  murFer = 1000 ;
  nu [ Air ]  = 1. / mu0 ;
  nu [ Inductors ]  = 1. / mu0 ;
  nu [ Iron ]  = 1. / (murFer * mu0) ;

  NbTurns = 120 ;
  Iphase = 1; 
  phase = -60*deg;
  js[ CoilAP ] = Vector[ 0,0, Iphase * Cos[phase +   0*deg] * NbTurns / SurfaceArea[]{6} ];
  js[ CoilBP ] = Vector[ 0,0, Iphase * Cos[phase +  60*deg] * NbTurns / SurfaceArea[]{7} ];
  js[ CoilCP ] = Vector[ 0,0, Iphase * Cos[phase + 120*deg] * NbTurns / SurfaceArea[]{8} ];
  js[ CoilAN ] = Vector[ 0,0,-Iphase * Cos[phase +   0*deg] * NbTurns / SurfaceArea[]{6} ];
  js[ CoilBN ] = Vector[ 0,0,-Iphase * Cos[phase +  60*deg] * NbTurns / SurfaceArea[]{7} ];
  js[ CoilCN ] = Vector[ 0,0,-Iphase * Cos[phase + 120*deg] * NbTurns / SurfaceArea[]{8} ];
}


Function {
  // Sliding surface:
  // a1 is the span of Region and RegionRef. 
  // a0 is the mesh step of the meshes of the sliding surfaces
  // a2[] is the position of the mover
  // relative to its reference position (as in the msh file)
  // assumed to be aligned with the stator. 
  // a3[] is the shear angle of region AIRBM 
  // (smaller than half the discretization step of the sliding surface
  // to adapt to a2[] values that are not multiple of this step). 
  // AlignWithMaster[] maps a point of coordinates {X[], Y[], Z[]} onto
  // its image in the open set RegionRef-SubRegionRef by the symmetry mapping.
  // Coef[] is evaluated on Master nodes
  // fFloor[] is a safe version of Floor[] for real represented int variables
  // fRem[a,b] substracts from a multiple of b so that the result is in [0,1[

  Periodicity = 1 ;  // -1 for antiperiodicity, 1 for periodicity
  TranslatePZ[] = Vector[ X[]+$1,Y[], Z[]] ;
  Tol = 1e-8 ; fFloor[] = Floor[ $1 + Tol ] ; fRem[] = $1 - fFloor[ $1 / $2 ] * $2; 

  a1 = L ; 
  a0 = a1/NbrDiv ; // mesh stepsize on the sliding surface
  a2[] = $MoverPosition ;
  a3[] = ( ( fRem[ a2[], a0 ]#2 <= 0.5*a0 ) ? #2 : #2-a0 ) ;
  AlignWithMaster[] = TranslatePZ[ - fFloor[ ( X[] - a3[] )/a1 ] * a1 ] ;
  RestoreRef[] = XYZ[] - Vector[ 0, 0, SlidingSurfaceShift ] ;
  Coef[] = ( fFloor[ ((X[] - a2[] )/a1) ] % 2 ) ? Periodicity : 1. ;
}

Group {
  DefineGroup[ DomainInf ];
  DefineVariable[ Val_Rint, Val_Rext ];
}

Jacobian {
  { Name Vol ;
    Case { { Region DomainInf ;
             Jacobian VolSphShell {Val_Rint, Val_Rext} ; }
           { Region All ; Jacobian Vol ; }
    }
  }
  { Name Sur ;
    Case { { Region All ; Jacobian Sur ; }
    }
  }
}

Integration {
  { Name Int ;
    Case { {Type Gauss ;
	Case { 
	  { GeoElement Line        ; NumberOfPoints  4 ; }
	  { GeoElement Triangle    ; NumberOfPoints  4 ; }
	  { GeoElement Quadrangle  ; NumberOfPoints  4 ; } }
      }
    }
  }
}

Constraint {
  { Name MagneticVectorPotential_2D ; Type Assign ;
    Case {
      { Region Sur_Dirichlet ; Value 0. ; }

      // Periodicity boundary condition on lateral faces
      { Type Link ; Region Sur_StatorPerSlave ; RegionRef Sur_StatorPerMaster ; 
      	ToleranceFactor 1e-8; 
      	Coefficient Periodicity ; Function TranslatePZ[ -a1 ] ; }

      { Type Link ; Region Sur_MoverPerSlave ; RegionRef Sur_MoverPerMaster ; 
      	ToleranceFactor 1e-8; 
      	Coefficient Periodicity ; Function TranslatePZ[ -a1 ] ; }
     
      // Sliding surface
      { Type Link ; Region Sur_SlidingSlave ; SubRegion Lin_SlidingSubslave ;
        RegionRef Sur_SlidingMaster ; SubRegionRef Lin_SlidingSubmaster ; 
		ToleranceFactor 1e-8; 
		Coefficient Coef[] ; Function AlignWithMaster[] ; FunctionRef RestoreRef[] ; }
    }
  }
}

FunctionSpace {
  { Name Hcurl_a ; Type Form1P ;
    BasisFunction {
      { Name se ; NameOfCoef ae ; Function BF_PerpendicularEdge ;
        Support Dom_Hcurl_a ; Entity NodesOf[ All ] ; }
    }
    Constraint {
      { NameOfCoef ae ; EntityType NodesOf ; //EntitySubType Not ;
        NameOfConstraint MagneticVectorPotential_2D ; }
    }
  }
}

Formulation {
  { Name Magnetostatics ; Type FemEquation ;
    Quantity {
      { Name a  ; Type Local  ; NameOfSpace Hcurl_a ; }
    }
    Equation {
      Galerkin { [ nu[] * Dof{d a} , {d a} ] ; In Vol_Mag ;
                 Jacobian Vol ; Integration Int ; }
      Galerkin { [ -js[] , {a} ] ; In Vol_Conductors ;
                 Jacobian Vol ; Integration Int ; }
    }
  }
}

Resolution {
  { Name Static ;
    System {
      { Name Sys_Mag ; NameOfFormulation Magnetostatics ;}
    }
    Operation {
      Evaluate[$MoverPosition = MoverPosition];
      ChangeOfCoordinates[ NodesOf[ Vol_Moving ], TranslatePZ[ a2[] ] ] ;
      ChangeOfCoordinates[ NodesOf[ Sur_SlidingMaster ], TranslatePZ[ a3[] ] ] ;
      Evaluate[ $a2 = a2[] ];
      Evaluate[ $a3 = a3[] ];
      Evaluate[ $aa = Fmod[ a2[], a0 ] ];
      Evaluate[ $bb = fRem[ a2[], a0 ] ];
      Print[ {$MoverPosition, a0, $a3, $aa, $bb}, 
	     Format "wt=a2=%e a0=%e a3=%e %e %e"] ;

      UpdateConstraint[Sys_Mag];
      Generate Sys_Mag ; Solve Sys_Mag ; SaveSolution Sys_Mag ;
      PostOperation [Fields];
    }
  }

  { Name QuasiStatic ;
    System {
      { Name Sys_Mag ; NameOfFormulation Magnetostatics ; }
    }
    Operation {
      Evaluate[$MoverPosition = MoverPosition];
      Generate Sys_Mag ; Solve Sys_Mag ; SaveSolution Sys_Mag ;
      PostOperation [Fields];
      TimeLoopTheta{
		Time0 0 ; TimeMax Mag_TimeMax ; DTime Mag_DTime ; Theta 1. ;
		Operation {
		  Evaluate[$MoverPosition = MoverPosition + velocity*$Time];
		  ChangeOfCoordinates[ NodesOf[ Vol_Moving ], TranslatePZ[ velocity*$DTime ] ] ;
		  ChangeOfCoordinates[ NodesOf[ Sur_SlidingMaster ], TranslatePZ[ a3[] ] ] ;
		  UpdateConstraint[Sys_Mag];
		  Generate Sys_Mag ; Solve Sys_Mag ; SaveSolution Sys_Mag ;
		  PostOperation [Fields];
		  ChangeOfCoordinates[ NodesOf[ Sur_SlidingMaster ], TranslatePZ[ -a3[] ] ] ;
		}
      }
    }
  }
}

PostProcessing {
  { Name MagSta_a ; NameOfFormulation Magnetostatics ;
    PostQuantity {
      { Name a ; Value { Term { [ {a} ] ; 
	    In Vol_Mag ; Jacobian Vol ; } } }
      { Name az ; Value { Term { [ CompZ[{a}] ] ; 
	    In Vol_Mag ; Jacobian Vol ; } } }
      { Name b ; Value { Term { [ {d a} ] ; 
	    In Vol_Mag ; Jacobian Vol ; } } }
      { Name h ; Value { Term { [ nu[] * {d a} ] ; 
	    In Vol_Mag ; } } }
      { Name bsurf ; Value { 
	  Local { [ {d a} ]; In Sur_Link ; Jacobian Sur; } } }
      { Name un ; Value { Term { [ 1 ] ; 
	    In Vol_Mag ; Jacobian Vol ; } } }
    }
  }
}

PostOperation Fields UsingPost MagSta_a {

  Print[ b, OnElementsOf Region[ Vol_Mag ], 
	 LastTimeStepOnly, File "b.pos"] ;
  /*
  Echo[ Str["l=PostProcessing.NbViews-1;",
	    "View[l].ArrowSizeMax = 100;",
	    "View[l].CenterGlyphs = 1;",
	    "View[l].GlyphLocation = 1;",
	    "View[l].LineWidth = 2;",
	    "View[l].VectorType = 4;" ] ,
  	File "tmp.geo", LastTimeStepOnly] ;
  */

  Print[ un, OnElementsOf Region[ NICEPOS ], 
	 LastTimeStepOnly, File "un.pos"] ;
  Echo[ Str["l=PostProcessing.NbViews-1;",
			"View[l].LineWidth = 2;",
			"View[l].ColormapNumber = 0;",
			"View[l].ShowScale = 0;",
			"Geometry.Lines = 0;",
			"Geometry.Points = 0;"] ,
  	File "tmp.geo", LastTimeStepOnly] ;

  Print[ az, OnElementsOf Region[ Vol_Mag ], 
	 LastTimeStepOnly, File "a.pos"] ;
  Echo[ Str["l=PostProcessing.NbViews-1;",
	    "View[l].IntervalsType = 1;",
	    "View[l].NbIso = 40;"] ,
  	File "tmp.geo", LastTimeStepOnly] ;
}

PostOperation Check_Periodicity UsingPost MagSta_a {
  Print[ bsurf, OnElementsOf Region[ {Sur_SlidingMaster, Sur_SlidingSlave} ], 
	 LastTimeStepOnly, File "bs.pos"] ;
  Echo[ Str["l=PostProcessing.NbViews-1;",
	    "View[l].ArrowSizeMax = 100;",
	    "View[l].CenterGlyphs = 0;",
	    "View[l].GlyphLocation = 2;",
	    "View[l].VectorType = 2;" ] ,
  	File "tmp.geo", LastTimeStepOnly] ;
}

