# GetDP tutorial 11: High order basis functions

We revisit the full-wave model of the rectangular waveguide of
[tutorial 5](../05-Full_wave/), on a deliberately coarse mesh, with hierarchical
basis functions of arbitrary order. A straight section of the waveguide is
considered, with a transparent boundary condition for the excited mode on the
output port, so that the exact solution is known and the error can be
measured. The same function space is defined in three ways, which can be
selected at runtime: automatically (the whole space of a given order in one
line), by type of entity (edges, facets and volumes), and function by function.

## Features

 - High order hierarchical function spaces (`BF_HcurlLegendre`, and
   `BF_H1Legendre`, `BF_HdivLegendre`, `BF_L2Legendre`), on simplices,
   quadrangles and hexahedra
 - Automatic definition with `Order` and `Entity Auto`; constraints with
   `EntityType Auto`
 - Definition by type of entity and function by function
 - Quadrature rules for high order (`GaussLegendre`)
 - Error with respect to an exact solution; post-processing of high order
   fields with `Depth`

See the comments in `high_order.pro` and `high_order.geo` for details.

This tutorial requires GetDP to be built with the `HierarchicalBasis` module of
Gmsh.

## Running the tutorial

On the command line (2D analysis, order 2):
```
> gmsh high_order.geo -2
> getdp high_order.pro -solve Wav -pos Map
```

The order and the definition of the function space are selected with `-setnumber
p <order>` and `-setnumber def <0|1|2>`; the relative error is written in
`error.txt`. For example, with order 4:
```
> getdp high_order.pro -solve Wav -pos Map -setnumber p 4
```

On the command line (3D analysis, with hexahedra):
```
> gmsh high_order.geo -3 -setnumber dim 3 -setnumber shape 1
> getdp high_order.pro -solve Wav -pos Map -setnumber dim 3 -setnumber shape 1
```

Interactively with Gmsh: open `high_order.pro` with "File->Open", then press
"Run".

## Bonus

The [bonus](bonus/) subdirectory applies the high order function spaces of H1,
H(div) and L2 to a Poisson problem on a disk or a ball, with curved meshes.
