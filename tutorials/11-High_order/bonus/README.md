# GetDP tutorial 11 bonus: High order in H1, H(div) and L2 on curved meshes

A Poisson problem with a known solution is solved on the unit disk (2D) or the
unit ball (3D), with the high order function spaces of
[tutorial 11](../) defined automatically. Three formulations can be selected
at runtime: the classical one in H1, the mixed one in H(div) x L2 (3D only),
and the projection of the solution in L2. The order of the mesh can be chosen
independently of the order of the basis functions, to show that curved
elements are needed for high order basis functions to pay off on a curved
domain.

## Features

 - `BF_H1Legendre`, `BF_HdivLegendre` and `BF_L2Legendre` function spaces
 - Mixed formulation of the Poisson problem
 - Curved (high order) meshes vs. high order basis functions
 - Quadrature rules on curved elements

See the comments in `ball.pro` and `ball.geo` for details.

This tutorial requires GetDP to be built with the `HierarchicalBasis` module of
Gmsh.

## Running the tutorial

On the command line (2D, H1, basis functions of order 2 on a mesh of order 1,
then on a mesh of order 2):
```
> gmsh ball.geo -2
> getdp ball.pro -solve Sol -pos Map
> gmsh ball.geo -2 -setnumber order 2
> getdp ball.pro -solve Sol -pos Map -setnumber order 2
```

The relative error is written in `error.txt`. The order of the basis functions
is selected with `-setnumber p <order>` and the formulation with `-setnumber
form <0|1|2>`. For example, the mixed formulation in 3D:
```
> gmsh ball.geo -3 -setnumber dim 3 -setnumber order 2
> getdp ball.pro -solve Sol -pos Map -setnumber dim 3 -setnumber order 2 -setnumber form 1 -setnumber p 3
```

Interactively with Gmsh: open `ball.pro` with "File->Open", then press "Run".
