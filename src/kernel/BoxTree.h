// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef BOX_TREE_H
#define BOX_TREE_H

#include <vector>

// Search structure for the boxes that contain a point: a binary tree of
// bounding boxes (a "bounding volume hierarchy"), as in Octree.cpp of Gmsh.
//
// The boxes are sorted along a space-filling curve (a Z-order curve through
// their centers), so that boxes that follow each other are close in space.
// The tree is then made by cutting this sequence in two halves again and
// again, down to leaves of a few boxes; each node stores the bounding box of
// all the boxes below it. A search goes down from the root and skips every
// node whose bounding box does not contain the point: for the elements of a
// mesh, which overlap little, it looks at a number of nodes that grows like
// the logarithm of the number of boxes, whatever the sizes of the elements.
//
// To stay small on large meshes (about 20 bytes per box) the tree only keeps
// the boxes of its nodes, in single precision (rounded outwards), and not
// those that were added: a search gives the boxes of the leaves that contain
// the point, which the caller tests itself.
class BoxTree {
public:
  // Add a box; boxes are numbered 0, 1, 2, ... in the order they are added
  void add(const double min[3], const double max[3]);

  // Build the tree, once all the boxes are added
  void build();

  // Give the numbers of the boxes that can contain the point, in increasing
  // order: all those that do, and some of their neighbours
  void search(double x, double y, double z, std::vector<int> &found) const;

  int size() const { return (int)_number.size(); }

private:
  struct Box {
    float min[3], max[3];
  };
  // A leaf holds the boxes [first, first + count) of the sorted sequence; an
  // internal node (count == 0) has its left child just after it in _nodes
  // and its right child at index first
  struct Node {
    Box box;
    int first, count;
  };
  std::vector<Box> _boxes; // the boxes that were added, until the tree is built
  std::vector<int> _number; // their numbers, in the order of the curve
  std::vector<Node> _nodes;

  int _buildNode(int first, int count);
};

#endif
