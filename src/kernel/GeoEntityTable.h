// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef GEO_ENTITY_TABLE_H
#define GEO_ENTITY_TABLE_H

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>
#include "ProData.h"
#include "MallocUtils.h"

// The edges (resp. the facets) of the mesh, each one defined by the sorted list
// of its nodes (resp. of its edges). They are created when the edges of the
// elements are first needed: get() returns the entity with the given list,
// which is added to the table if it is not there yet. The entities are stored
// in the order of their creation and found through a hash table of their
// lists.

class GeoEntityTable {
private:
  std::vector<struct Entity2XEntity1> _entities;
  struct Hash {
    const std::vector<struct Entity2XEntity1> *e;
    std::size_t operator()(int i) const
    {
      const struct Entity2XEntity1 &x = (*e)[i];
      std::size_t h = x.NbrEntities;
      for(int k = 0; k < x.NbrEntities; k++)
        h = (h ^ (std::size_t)(unsigned int)x.NumEntities[k]) *
            0x9E3779B97F4A7C15ULL;
      return h ^ (h >> 29);
    }
  };
  struct Equal {
    const std::vector<struct Entity2XEntity1> *e;
    bool operator()(int i, int j) const
    {
      const struct Entity2XEntity1 &x = (*e)[i], &y = (*e)[j];
      return x.NbrEntities == y.NbrEntities &&
             !memcmp(x.NumEntities, y.NumEntities, x.NbrEntities * sizeof(int));
    }
  };
  std::unordered_set<int, Hash, Equal> _index; // of positions in _entities
  GeoEntityTable(const GeoEntityTable &);

public:
  GeoEntityTable() : _index(1024, Hash{&_entities}, Equal{&_entities}) {}
  ~GeoEntityTable()
  {
    for(auto &x : _entities) Free(x.NumEntities);
  }
  std::size_t size() const { return _entities.size(); }
  // the entities in the order of their creation (their lists must not change)
  std::vector<struct Entity2XEntity1> &entities() { return _entities; }
  // the entity defined by the n numbers in list; if it is new, created is set
  // and its number is left to the caller
  struct Entity2XEntity1 &get(int n, int *list, bool &created)
  {
    struct Entity2XEntity1 x;
    x.Num = 0;
    x.NbrEntities = n;
    x.NumEntities = list;
    _entities.push_back(x);
    int i = (int)_entities.size() - 1;
    auto r = _index.insert(i);
    created = r.second;
    if(!created) {
      _entities.pop_back();
      return _entities[*r.first];
    }
    _entities[i].NumEntities = (int *)Malloc(n * sizeof(int));
    memcpy(_entities[i].NumEntities, list, n * sizeof(int));
    return _entities[i];
  }
  // all the entities, sorted by number of sub-entities and then by list
  std::vector<struct Entity2XEntity1 *> sorted()
  {
    std::vector<struct Entity2XEntity1 *> s;
    s.reserve(_entities.size());
    for(auto &x : _entities) s.push_back(&x);
    std::sort(s.begin(), s.end(),
              [](const struct Entity2XEntity1 *a,
                 const struct Entity2XEntity1 *b) {
                if(a->NbrEntities != b->NbrEntities)
                  return a->NbrEntities < b->NbrEntities;
                return std::lexicographical_compare(
                  a->NumEntities, a->NumEntities + a->NbrEntities,
                  b->NumEntities, b->NumEntities + b->NbrEntities);
              });
    return s;
  }
};

#endif
