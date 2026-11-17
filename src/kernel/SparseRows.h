// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef SPARSE_ROWS_H
#define SPARSE_ROWS_H

#include <algorithm>
#include <map>
#include <vector>

// A sparse matrix stored by rows, in which the finite element assembly adds its
// entries one at a time: add(i, j, v) adds v to the entry of row i and column
// j, which is created if it does not exist yet. The nonzero pattern of the
// matrix is thus built on the fly.
//
// Each row is the list of its columns, sorted, with the list of their values:
// an entry is found by a binary search in the row. The rows [first, last), the
// ones the MPI rank owns in a parallel computation, are stored in a vector; the
// entries added to any other row, which the rank has to send to their owner,
// are kept on the side.

template <class Scalar, class Index = int> class SparseRows {
public:
  struct Row {
    std::vector<Index> cols;
    std::vector<Scalar> vals;
    void add(Index j, Scalar v)
    {
      auto it = std::lower_bound(cols.begin(), cols.end(), j);
      std::size_t k = it - cols.begin();
      if(it != cols.end() && *it == j)
        vals[k] += v;
      else {
        cols.insert(it, j);
        vals.insert(vals.begin() + k, v);
      }
    }
  };

private:
  Index _first, _last;
  std::vector<Row> _rows;
  std::map<Index, Row> _others;

public:
  SparseRows(Index first = 0, Index last = 0)
    : _first(first), _last(last), _rows(last - first)
  {
  }
  Index first() const { return _first; }
  Index last() const { return _last; }
  Row &row(Index i)
  {
    return (i >= _first && i < _last) ? _rows[i - _first] : _others[i];
  }
  void add(Index i, Index j, Scalar v) { row(i).add(j, v); }
  // the entries added to rows outside [first, last)
  std::map<Index, Row> &others() { return _others; }
  // removes all the entries; the rows keep their memory for the next assembly
  void clear()
  {
    for(auto &r : _rows) {
      r.cols.clear();
      r.vals.clear();
    }
    _others.clear();
  }
};

#endif
