// The mesh of scaling.geo, and the same mesh partitioned by Gmsh in one file
// for each of 3 partitions (parallel_part_files_1.msh, ...), with the ghost
// cells that a rank needs to read the file of its partition alone (the elements
// of the other partitions that share a node with it), and only the entities of
// the model that the partition needs; run with
// "gmsh parallel_part_files.geo -"

Include "scaling.geo";
Mesh 3;
Save "parallel_part_files.msh";
Mesh.PartitionCreateGhostCells = 1;
PartitionMesh 3;
Mesh.PartitionSplitMeshFiles = 1;
Mesh.PartitionSplitLocalBrep = 1;
Save "parallel_part_files.msh";
