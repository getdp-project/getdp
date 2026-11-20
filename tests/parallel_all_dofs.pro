/* Tests of what makes GetDP define all the degrees of freedom on all the MPI
   ranks (as with -all-dofs), on the problems of parallel.pro:

   getdp parallel_all_dofs.pro -msh parallel.msh -solve Laplace -pos Mirror
     a quantity evaluated at given coordinates: the point can be anywhere in
     the mesh, in the partition of another rank

   (They are in a file of their own: such a quantity makes all the runs of the
   problem use all the degrees of freedom.) */

Include "parallel.pro";

PostProcessing {
  { Name Mirror; NameOfFormulation Laplace;
    Quantity {
      // u at the point symmetric to the current one
      { Name u_mirror; Value {
          Term { [ {u}[1 - X[], 1 - Y[], 0] ]; In Domain; Jacobian Vol; } } }
      { Name int_u_mirror; Value {
          Integral { [ {u}[1 - X[], 1 - Y[], 0] ]; In Domain; Jacobian Vol;
            Integration Int; } } }
    }
  }
}

PostOperation {
  { Name Mirror; NameOfPostProcessing Mirror;
    Operation {
      Print[ u_mirror, OnElementsOf Domain, File "u_mirror.pos" ];
      Print[ int_u_mirror[Domain], OnGlobal, Format Table,
        File "integral_u_mirror.txt" ];
    }
  }
}
