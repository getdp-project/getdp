// The mesh of scaling.geo, and the same mesh partitioned by Gmsh in one file
// for each of 3 partitions (parallel_part_files_1.msh, ...), with the layer of
// overlap that a rank needs to read the file of its partition alone; run with
// "gmsh parallel_part_files.geo -"

Include "scaling.geo";
Mesh 3;
Save "parallel_part_files.msh";
PartitionMesh 3;
CreateOverlaps 1;
Mesh.PartitionSplitMeshFiles = 1;
Save "parallel_part_files.msh";
