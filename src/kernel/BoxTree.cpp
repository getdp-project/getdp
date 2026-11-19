// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "BoxTree.h"

// the largest number of boxes in a leaf
static const int MaxLeafSize = 8;

void BoxTree::add(const double min[3], const double max[3])
{
  // in single precision, rounded outwards so that the box is not smaller
  Box b;
  for(int k = 0; k < 3; k++) {
    b.min[k] = std::nextafter((float)min[k], -HUGE_VALF);
    b.max[k] = std::nextafter((float)max[k], HUGE_VALF);
  }
  _number.push_back((int)_number.size());
  _boxes.push_back(b);
}

// The bits of x spread to every third bit (21 bits in, 63 out): the Z-order
// code of a point interleaves the bits of its three integer coordinates.
static uint64_t spreadBits(uint64_t x)
{
  x &= 0x1fffff;
  x = (x | x << 32) & 0x1f00000000ffffULL;
  x = (x | x << 16) & 0x1f0000ff0000ffULL;
  x = (x | x << 8) & 0x100f00f00f00f00fULL;
  x = (x | x << 4) & 0x10c30c30c30c30c3ULL;
  x = (x | x << 2) & 0x1249249249249249ULL;
  return x;
}

// Make the node of the boxes [first, first + count) of the sorted sequence and
// the nodes below it; its bounding box is computed afterwards, from the
// leaves up. Returns its index.
int BoxTree::_buildNode(int first, int count)
{
  int index = (int)_nodes.size();
  _nodes.push_back(Node());
  if(count <= MaxLeafSize) {
    _nodes[index].first = first;
    _nodes[index].count = count;
    return index;
  }
  int half = count / 2;
  _buildNode(first, half); // the left child, just after the node
  int right = _buildNode(first + half, count - half);
  _nodes[index].first = right;
  _nodes[index].count = 0;
  return index;
}

void BoxTree::build()
{
  _nodes.clear();
  std::size_t num = _boxes.size();
  if(!num) return;

  // the position of the center of each box on a grid of 2^21 cells along
  // the largest extent of the whole, as a Z-order code
  float min[3], max[3];
  for(int k = 0; k < 3; k++) {
    min[k] = _boxes[0].min[k];
    max[k] = _boxes[0].max[k];
  }
  for(std::size_t i = 1; i < num; i++) {
    for(int k = 0; k < 3; k++) {
      min[k] = std::min(min[k], _boxes[i].min[k]);
      max[k] = std::max(max[k], _boxes[i].max[k]);
    }
  }
  double extent =
    std::max(std::max(max[0] - min[0], max[1] - min[1]), max[2] - min[2]);
  double scale = (extent > 0.) ? (double)((1 << 21) - 1) / extent : 0.;
  std::vector<std::pair<uint64_t, int> > codes(num);
  for(std::size_t i = 0; i < num; i++) {
    uint64_t code = 0;
    for(int k = 0; k < 3; k++) {
      double center = 0.5 * ((double)_boxes[i].min[k] + _boxes[i].max[k]);
      code |= spreadBits((uint64_t)((center - min[k]) * scale)) << k;
    }
    codes[i] = std::make_pair(code, (int)i);
  }

  // the boxes sorted along the curve (boxes with the same code stay in the
  // order they were added, the second member of the pair)
  std::sort(codes.begin(), codes.end());
  for(std::size_t i = 0; i < num; i++) _number[i] = codes[i].second;
  std::vector<std::pair<uint64_t, int> >().swap(codes);

  _nodes.reserve(num / (MaxLeafSize / 2) + 1);
  _buildNode(0, (int)num);

  // the bounding boxes of the nodes: the children come after their parent
  // in _nodes, so going backwards they are done before it
  for(int i = (int)_nodes.size() - 1; i >= 0; i--) {
    Node &n = _nodes[i];
    for(int j = 0; j < (n.count ? n.count : 2); j++) {
      const Box &b = n.count ? _boxes[_number[n.first + j]] :
                               _nodes[j ? n.first : i + 1].box;
      for(int k = 0; k < 3; k++) {
        n.box.min[k] = j ? std::min(n.box.min[k], b.min[k]) : b.min[k];
        n.box.max[k] = j ? std::max(n.box.max[k], b.max[k]) : b.max[k];
      }
    }
  }

  // only the boxes of the nodes are kept
  std::vector<Box>().swap(_boxes);
  std::vector<Node>(_nodes).swap(_nodes);
}

void BoxTree::search(double x, double y, double z,
                     std::vector<int> &found) const
{
  found.clear();
  if(_nodes.empty()) return;
  // the nodes that remain to be looked at: at most one per level of the
  // tree, whose depth is below log2(number of boxes) + 1
  int stack[64], n = 0;
  stack[n++] = 0;
  while(n) {
    int index = stack[--n];
    const Node &node = _nodes[index];
    const Box &b = node.box;
    if(x < b.min[0] || x > b.max[0] || y < b.min[1] || y > b.max[1] ||
       z < b.min[2] || z > b.max[2])
      continue;
    if(node.count) {
      for(int i = node.first; i < node.first + node.count; i++)
        found.push_back(_number[i]);
    }
    else {
      stack[n++] = node.first;
      stack[n++] = index + 1;
    }
  }
  std::sort(found.begin(), found.end());
}
