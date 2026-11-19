// The mesh of parallel.geo partitioned by Gmsh, in a number of partitions that
// is a multiple of the numbers of ranks of the tests (GetDP merges them); run
// with "gmsh parallel_partitioned.geo -"

Include "parallel.geo";
Mesh 2;
PartitionMesh 6;
Save "parallel_partitioned.msh";
