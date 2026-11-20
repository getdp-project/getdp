// The mesh of scaling.geo partitioned by Gmsh in a single file with ghost cells
// (the elements of the other partitions that share a node with a partition),
// from which each of 3 ranks reads its partition; run with
// "gmsh parallel_ghosts.geo -"

Include "scaling.geo";
Mesh 3;
Mesh.PartitionCreateGhostCells = 1;
PartitionMesh 3;
Mesh.Binary = 1;
Save "parallel_ghosts.msh";
