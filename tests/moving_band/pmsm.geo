Include "pmsm_data.geo";

Solver.AutoShowLastStep = 1;
Mesh.Algorithm = 6;
Geometry.CopyMeshingMethod = 1;

fact_trans = Mesh.CharacteristicLengthFactor ;


// Mesh characteristic lengths
s = 0.4 ;
pR1=(rR2-rR1)/6.*s;
pR2=(rR2-rR1)/6.*s;

pS1=(rS7-rS1)/7.*s;
pS2=(rS7-rS1)/12.*s;
pS3=(rS6-rS3)/10.*s;

If(TotalMemory <= 2048)
  pR1 *= 2;
  pR2 *= 2;
  pS1 *= 2;
  pS2 *= 2;
  pS3 *= 2;
EndIf

NbrDivMB = 2*Ceil[2*Pi*rRext/8/pR1/fact_trans] ; //1/8 Moving band

//--------------------------------------------------------------------------------
//--------------------------------------------------------------------------------

cen = newp ; Point(cen)={0,0,0,pR1};
nicepos_rotor[] = {};
nicepos_stator[] = {};

Include "pmsm_rotor.geo";
Include "pmsm_stator.geo";

Physical Line("Nice view for post-processing", NICEPOS) = { nicepos_rotor[], nicepos_stator[] };
