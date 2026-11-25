SetFactory("OpenCASCADE");
DefineConstant[ dim = 2, lc = 0.25 ];
Mesh.MeshSizeMax = lc; Mesh.MeshSizeMin = lc;
If(dim == 2)
  Rectangle(1) = {0, 0, 0, 1, 1};
  Physical Surface(1) = {1};
  Physical Curve(2) = Boundary{ Surface{1}; };
Else
  Box(1) = {0, 0, 0, 1, 1, 1};
  Physical Volume(1) = {1};
  Physical Surface(2) = Boundary{ Volume{1}; };
EndIf
