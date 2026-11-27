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
// Each row is the list of its columns with the list of their values. The
// first columns of a row are sorted: an entry is looked for among them by a
// binary search. The entries that are not found are appended at the end of the
// row, unsorted (the same column can then appear several times), and are
// merged with the sorted ones when they become as many, or when the row is
// read: inserting each new entry at its place would shift the rest of the row
// each time, which is too slow for the long rows of the high order elements.
// The rows [first, last), the ones the MPI rank owns in a parallel
// computation, are stored in a vector; the entries added to any other row,
// which the rank has to send to their owner, are kept on the side.

template <class Scalar, class Index = int> class SparseRows {
public:
  class Row {
  private:
    std::vector<Index> _cols;
    std::vector<Scalar> _vals;
    std::size_t _sorted = 0; // number of sorted columns
    void _merge()
    {
      if(_sorted == _cols.size()) return;
      const std::size_t n = _cols.size();
      // the unsorted entries, in the order of their columns
      std::vector<std::size_t> order(n - _sorted);
      for(std::size_t k = 0; k < order.size(); k++) order[k] = _sorted + k;
      std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return _cols[a] < _cols[b];
      });
      std::vector<Index> cols;
      std::vector<Scalar> vals;
      cols.reserve(n);
      vals.reserve(n);
      std::size_t a = 0, b = 0;
      while(a < _sorted || b < order.size()) {
        std::size_t k;
        if(b == order.size() || (a < _sorted && _cols[a] < _cols[order[b]]))
          k = a++;
        else
          k = order[b++];
        if(!cols.empty() && cols.back() == _cols[k])
          vals.back() += _vals[k];
        else {
          cols.push_back(_cols[k]);
          vals.push_back(_vals[k]);
        }
      }
      _cols.swap(cols);
      _vals.swap(vals);
      _sorted = _cols.size();
    }

  public:
    void add(Index j, Scalar v)
    {
      auto end = _cols.begin() + _sorted;
      auto it = std::lower_bound(_cols.begin(), end, j);
      if(it != end && *it == j) {
        _vals[it - _cols.begin()] += v;
        return;
      }
      if(_sorted == _cols.size() &&
         (_cols.size() < 64 || _cols.back() < j)) {
        // a short row, or a column after the last one: insert it at its
        // place
        const std::size_t k = it - _cols.begin();
        _cols.insert(it, j);
        _vals.insert(_vals.begin() + k, v);
        _sorted++;
        return;
      }
      _cols.push_back(j);
      _vals.push_back(v);
      if(_cols.size() - _sorted > std::max<std::size_t>(_sorted, 32)) _merge();
    }
    // adds n entries given in the order of their columns (the same column
    // can be given several times), in one pass over the row
    void addSorted(std::size_t n, const Index *cols, const Scalar *vals)
    {
      if(!n) return;
      if(n <= 8) {
        // a few entries: one search each
        for(std::size_t k = 0; k < n; k++) add(cols[k], vals[k]);
        return;
      }
      _merge();
      const std::size_t size = _cols.size();
      // the entries already in the row are added in place
      std::size_t a = 0, missing = 0;
      for(std::size_t k = 0; k < n; k++) {
        while(a < size && _cols[a] < cols[k]) a++;
        if(a == size || _cols[a] != cols[k]) {
          if(!missing || cols[k] != cols[k - 1]) missing++;
        }
      }
      if(!missing) {
        a = 0;
        for(std::size_t k = 0; k < n; k++) {
          while(_cols[a] < cols[k]) a++;
          _vals[a] += vals[k];
        }
        return;
      }
      // make room for the new columns and merge from the end, in place: the
      // row is not reallocated each time an element brings a new column
      _cols.resize(size + missing);
      _vals.resize(size + missing);
      std::size_t w = size + missing; // one past the next entry to write
      a = size; // one past the next entry of the row to place
      std::size_t b = n; // one past the next entry to add
      while(b > 0) {
        const Index c = cols[b - 1];
        if(a > 0 && _cols[a - 1] > c) {
          // an entry of the row comes last
          --w;
          --a;
          _cols[w] = _cols[a];
          _vals[w] = _vals[a];
          continue;
        }
        // the entries to add with that column, and that of the row if any
        Scalar v = vals[--b];
        while(b > 0 && cols[b - 1] == c) v += vals[--b];
        if(a > 0 && _cols[a - 1] == c) v += _vals[--a];
        --w;
        _cols[w] = c;
        _vals[w] = v;
      }
      // (the entries of the row before the first new column are in place)
      _sorted = _cols.size();
    }
    // the columns of the row, sorted, and their values
    const std::vector<Index> &cols()
    {
      _merge();
      return _cols;
    }
    const std::vector<Scalar> &vals()
    {
      _merge();
      return _vals;
    }
    void clear()
    {
      _cols.clear();
      _vals.clear();
      _sorted = 0;
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
    for(auto &r : _rows) r.clear();
    _others.clear();
  }
};

#endif
