/* Test of integral quantities: a magnetostatic problem in a disk, with the
   exterior domain taken into account by integrals on its boundary (boundary
   element method). In parallel all the ranks define all the degrees of freedom,
   as an integral quantity relates unknowns of the whole boundary.

   getdp integral_quantity.pro -solve MagSta_phi -pos phi */

Group {
  gamma   = Region[ 1111 ];
  Inf     = Region[ 2222 ];
  Core    = Region[ 3333 ];
  Ext     = Region[ 4444 ];
  Sur_BEM = Region[ {gamma} ] ;
  Vol_Mag = Region[ Core ] ;
  Tot_Mag = Region[ {Core,gamma} ] ;
  Eval_BEM = Region[ {Ext} ] ;
}

Function {
  mu0 = 1. ;
  mur = 10. ;
  mu[Sur_BEM] = mu0 ;
  nu[Sur_BEM] = 1. / mu0 ;
  mu[Vol_Mag] = mur * mu0 ;
  nu[Vol_Mag] = 1. / (mur * mu0) ;
  BS[] = 2 * Vector[0,1,0] ;
}

Jacobian {
  { Name JVol ;
    Case {
      { Region All ; Jacobian Vol ; }
    }
  }
  { Name JSur ;
    Case {
      { Region All ; Jacobian Sur ; }
    }
  }
}

Integration {
  { Name I0 ;
    Case {
      { Type Gauss ;
        Case {
	  { GeoElement Point       ; NumberOfPoints  1 ; }
	  { GeoElement Line        ; NumberOfPoints  1 ; }
	  { GeoElement Triangle    ; NumberOfPoints  12 ; }
	}
      }
    }
  }
  { Name I1 ;
    Case {
      { Type Gauss ;
        Case {
	  { GeoElement Point       ; NumberOfPoints  1 ; }
	  { GeoElement Line        ; NumberOfPoints  3 ; }
	  { GeoElement Triangle    ; NumberOfPoints  3 ; }
	  { GeoElement Quadrangle  ; NumberOfPoints  4 ; }
	  { GeoElement Tetrahedron ; NumberOfPoints  4 ; }
	  { GeoElement Hexahedron  ; NumberOfPoints  6 ; }
	  { GeoElement Prism       ; NumberOfPoints  6 ; }
	}
      }
    }
  }
  { Name I2 ;
    Case {
      { Type Gauss ;
        Case {
	  { GeoElement Point       ; NumberOfPoints  1 ; }
	  { GeoElement Line        ; NumberOfPoints  4 ; }
	  { GeoElement Triangle    ; NumberOfPoints  7 ; }
	  { GeoElement Quadrangle  ; NumberOfPoints  7 ; }
	  { GeoElement Tetrahedron ; NumberOfPoints  15 ; }
	  { GeoElement Hexahedron  ; NumberOfPoints  34 ; }
	  { GeoElement Prism       ; NumberOfPoints  21 ; }
	}
      }
    }
  }
  { Name IA ;
    Case { { Type Analytic ; } }
  }
}

FunctionSpace {
  { Name Hgrad_phi_MagSta; Type Form0;
    BasisFunction {
      { Name sn; NameOfCoef phin; Function BF_Node; Support Tot_Mag;
        Entity NodesOf[All]; }
    }
  }
  { Name H_q_MagSta ; Type Form3 ;
    BasisFunction {
      { Name sv ; NameOfCoef qv ; Function BF_Volume ; Support Sur_BEM ;
        Entity VolumesOf[All] ; }
    }
  }
}

Formulation {
  { Name MagSta_phi; Type FemEquation;
    Quantity {
      { Name phi; Type Local; NameOfSpace Hgrad_phi_MagSta; }
      { Name q;   Type Local; NameOfSpace H_q_MagSta; }
      { Name G;  Type Integral; [ Laplace[]{2D} * Dof{q} ];
        In Sur_BEM; Integration IA; Jacobian JSur; }
      { Name dG; Type Integral; [ NPxGradLaplace[]{2D} * Dof{q} ];
        In Sur_BEM; Integration IA; Jacobian JSur; }
    }
    Equation {
      Galerkin { [ mu[] * Dof{d phi} , {d phi} ];
        In Vol_Mag; Integration I1; Jacobian JVol;  }

      Galerkin { [ Normal[] * BS[] , {phi} ];
        In Sur_BEM; Integration I1; Jacobian JSur;  }

      Galerkin { [ 0.5 * Dof{q} , {phi} ] ;
        In Sur_BEM; Integration I1; Jacobian JSur; }

      Galerkin { [ Dof{dG} , {phi} ] ;
        In Sur_BEM; Integration I2; Jacobian JSur; }

      Galerkin { [ Dof{phi} , {q} ] ;
        In Sur_BEM; Integration I1; Jacobian JSur; }

      Galerkin { [ - nu[] * Dof{G} , {q} ] ;
        In Sur_BEM; Integration I0; Jacobian JSur; }
    }
  }

}

Resolution {
  { Name MagSta_phi;
    System {
      { Name A; NameOfFormulation MagSta_phi; }
    }
    Operation { Generate A; Solve A; SaveSolution A; }
  }
}

PostProcessing {
  { Name MagSta_phi; NameOfFormulation MagSta_phi;
    Quantity {
      { Name phi;
        Value{
	  Local{ [ {phi} ] ; In Vol_Mag; Jacobian JVol; }
	}
      }
      { Name b;
        Value{
	  Local{ [ mu[] * {d phi} ] ; In Vol_Mag; Jacobian JVol; }
	}
      }
    }
  }
}

PostOperation {
  { Name phi ; NameOfPostProcessing MagSta_phi ;
    Operation {
      Print[ phi, OnElementsOf Vol_Mag , File "phi.pos" ] ;
      Print[ b,   OnElementsOf Vol_Mag , File "b_phi.pos" ] ;
    }
  }
}
