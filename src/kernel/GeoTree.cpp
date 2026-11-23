// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>
#include "GeoData.h"
#include "ProData.h"
#include "TreeUtils.h"
#include "Get_Geometry.h"
#include "Message.h"
#include "GetDPConfig.h"

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

extern struct GeoData *CurrentGeoData;

static int Tree_IndexToChange, Tree_NewIndex;

static void Geo_ChangeTreeIndex(void *a, void *b)
{
  if(((struct EntityInTree *)a)->Index == Tree_IndexToChange)
    ((struct EntityInTree *)a)->Index = Tree_NewIndex;
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e n e r a t e E d g e s O f T r e e                           */
/* ------------------------------------------------------------------------ */

static bool testEdgeAlignedWith(double *x, double *y, double *z, int *Entity_P,
                                int SuppListType2)
{
  bool aligned = false;
  double val1 = 0, val2 = 0;
  double tol = 1e-7;

  switch(SuppListType2) {
  case -1: // aligned with Cartesian X direction "X"
    val1 = x[abs(Entity_P[0]) - 1];
    val2 = x[abs(Entity_P[1]) - 1];
    break;
  case -2: // aligned with Cartesian Y direction "Y"
    val1 = y[abs(Entity_P[0]) - 1];
    val2 = y[abs(Entity_P[1]) - 1];
    break;
  case -3: // aligned with Cartesian Z direction "Z"
    val1 = z[abs(Entity_P[0]) - 1];
    val2 = z[abs(Entity_P[1]) - 1];
    break;
  case -4: // aligned with radius around X axis "Rx"
    val1 = atan2(y[abs(Entity_P[0]) - 1], z[abs(Entity_P[0]) - 1]);
    val2 = atan2(y[abs(Entity_P[1]) - 1], z[abs(Entity_P[1]) - 1]);
    break;
  case -5: // aligned with radius around Y axis "Ry"
    val1 = atan2(z[abs(Entity_P[0]) - 1], x[abs(Entity_P[0]) - 1]);
    val2 = atan2(z[abs(Entity_P[1]) - 1], x[abs(Entity_P[1]) - 1]);
    break;
  case -6: // aligned with radius around Z axis "Rz"
    val1 = atan2(x[abs(Entity_P[0]) - 1], y[abs(Entity_P[0]) - 1]);
    val2 = atan2(x[abs(Entity_P[1]) - 1], y[abs(Entity_P[1]) - 1]);
    break;
  default: Message::Error("Unknown 'AlignedWith parameter' %d", SuppListType2);
  }
  aligned = fabs(val2 - val1) < tol;
  return aligned;
}

// Keep component labels separate from disjoint-set roots: a label removed by
// a merge can be used again by a later element or StartingOn pass.
struct Geo_TreeComponents {
  std::unordered_map<int, int> indices;
  std::vector<int> parents, labels;

  int root(int i)
  {
    int r = i;
    while(parents[r] >= 0) r = parents[r];
    while(i != r) {
      int next = parents[i];
      parents[i] = r;
      i = next;
    }
    return r;
  }

  int index(int label)
  {
    std::unordered_map<int, int>::iterator it = indices.find(label);
    if(it != indices.end()) return root(it->second);
    int i = parents.size();
    indices[label] = i;
    parents.push_back(-1);
    labels.push_back(label);
    return i;
  }

  bool merge(int target, int source)
  {
    target = root(target);
    source = root(source);
    if(target == source) return false;
    int label = labels[target];
    indices.erase(labels[source]);
    if(parents[target] > parents[source]) std::swap(target, source);
    parents[target] += parents[source];
    parents[source] = target;
    labels[target] = label;
    indices[label] = target;
    return true;
  }
};

static void Geo_GenerateTreeOnSlidingSurface(List_T *InitialList,
                                             List_T *ExtendedList,
                                             Tree_T *EntitiesInTree_T,
                                             Geo_TreeComponents &components,
                                             int SuppListType2)
{
  int Nbr_Element, Nbr_Edges, i_Element, i_Edge, Num_Edge;
  struct Geo_Element *Geo_Element;
  int *D_Element, *Entity_P;
  struct EntityInTree EntityInTree_S;
  double x[NBR_MAX_NODES_IN_ELEMENT];
  double y[NBR_MAX_NODES_IN_ELEMENT];
  double z[NBR_MAX_NODES_IN_ELEMENT];

  Tree_T *EdgesInTree_T;
  EdgesInTree_T = Tree_Create(sizeof(int), fcmp_int);

  Nbr_Element = Geo_GetNbrGeoElements();
  for(i_Element = 0; i_Element < Nbr_Element; i_Element++) {
    Geo_Element = Geo_GetGeoElement(i_Element);

    if(List_Search(InitialList, &Geo_Element->Region, fcmp_int)) {
      Geo_GetNodesCoordinates(Geo_Element->NbrNodes, Geo_Element->NumNodes, x,
                              y, z);

      if(Geo_Element->NbrEdges == 0) Geo_CreateEdgesOfElement(Geo_Element);
      D_Element = Geo_GetIM_Den(Geo_Element->Type, &Nbr_Edges);

      for(i_Edge = 0; i_Edge < Geo_Element->NbrEdges; i_Edge++) {
        Entity_P = D_Element + i_Edge * NBR_MAX_SUBENTITIES_IN_ELEMENT;

        if(testEdgeAlignedWith(x, y, z, Entity_P, SuppListType2)) {
          EntityInTree_S.Index = components.index(Geo_Element->Region);
          EntityInTree_S.Num = abs(Geo_Element->NumNodes[abs(Entity_P[0]) - 1]);
          Tree_Add(EntitiesInTree_T, &EntityInTree_S);
          EntityInTree_S.Num = abs(Geo_Element->NumNodes[abs(Entity_P[1]) - 1]);
          Tree_Add(EntitiesInTree_T, &EntityInTree_S);

          Num_Edge = abs(Geo_Element->NumEdges[i_Edge]);
          Tree_Insert(EdgesInTree_T, &Num_Edge);
        }
      }
    }
  }

  List_Copy(Tree2List(EdgesInTree_T), ExtendedList);
}

// Nodes already in the tree, with the index of their component: in the AVL tree
// shared with the sliding surface pass, which can add a node more than once
struct Geo_TreeNodesInTree {
  Tree_T *tree;
  int *find(int num)
  {
    struct EntityInTree *e = (struct EntityInTree *)Tree_PQuery(tree, &num);
    return e ? &e->Index : NULL;
  }
  int *add(int num, int index)
  {
    struct EntityInTree e = {num, index};
    return &((struct EntityInTree *)Tree_Add(tree, &e))->Index;
  }
};

// ... or, without a sliding surface, in an array indexed by node number (or
// in a hash table if the node numbers are too sparse for an array)
struct Geo_TreeNodesInArray {
  bool dense;
  std::vector<int> index;
  std::unordered_map<int, int> hash;
  Geo_TreeNodesInArray()
  {
    int n = Geo_GetNbrGeoNodes(), max = 0;
    for(int i = 0; i < n; i++) max = std::max(max, Geo_GetGeoNode(i)->Num);
    dense = (max / 4 <= n + 1000);
    if(dense) index.resize(max + 1, -1);
  }
  int *find(int num)
  {
    if(dense) return index[num] < 0 ? NULL : &index[num];
    auto it = hash.find(num);
    return (it == hash.end()) ? NULL : &it->second;
  }
  int *add(int num, int i)
  {
    if(!dense) return &(hash[num] = i);
    index[num] = i;
    return &index[num];
  }
};

template <class Nodes>
static void Geo_GenerateEdgesOfTreeByDimension(int dim, List_T *List,
                                               bool isElementList,
                                               List_T *ExtendedList,
                                               Nodes &EntitiesInTree,
                                               Geo_TreeComponents &components)
{
  int Nbr_Element, Num_Element, i_Element, Nbr_Entities2, i, Num_Entity1,
    Dim_Element;
  struct Geo_Element *Geo_Element;
  int i_Entity2, Num_Entity2, *D_Element, *Entity_P, Entity, Flag_Change;
  int *EntitiesInTree_P[NBR_MAX_ENTITIES_IN_ELEMENT];
  struct EntityInTree EntityInTree_S;

  Nbr_Element = isElementList ? List_Nbr(List) : Geo_GetNbrGeoElements();

  for(i_Element = 0; i_Element < Nbr_Element; i_Element++) {
    if(isElementList) {
      List_Read(List, i_Element, &Num_Element);
      Geo_Element = Geo_GetGeoElementOfNum(Num_Element);
    }
    else {
      Geo_Element = Geo_GetGeoElement(i_Element);
      if(!List_Search(List, &Geo_Element->Region, fcmp_int)) continue;
    }

    Get_JacobianFunction(JACOBIAN_VOL, Geo_Element->Type, &Dim_Element);
    if(dim < 0 || Dim_Element == dim) {
      if(Geo_Element->NbrEdges == 0) Geo_CreateEdgesOfElement(Geo_Element);
      D_Element = Geo_GetIM_Den(Geo_Element->Type, &Nbr_Entities2);

      for(i = 0; i < Geo_Element->NbrNodes; i++) {
        Num_Entity1 = abs(Geo_Element->NumNodes[i]);
        EntitiesInTree_P[i] = EntitiesInTree.find(Num_Entity1);
      }

      for(i_Entity2 = 0; i_Entity2 < Geo_Element->NbrEdges; i_Entity2++) {
        Entity_P = D_Element + i_Entity2 * NBR_MAX_SUBENTITIES_IN_ELEMENT;

        i = 0;
        EntityInTree_S.Index = -1;
        while((Entity = abs(Entity_P[i++])) && (EntityInTree_S.Index < 0))
          if(EntitiesInTree_P[Entity - 1] != NULL)
            EntityInTree_S.Index = *EntitiesInTree_P[Entity - 1];
        if(EntityInTree_S.Index < 0)
          EntityInTree_S.Index = components.index(Geo_Element->Num);

        Flag_Change = 0;

        while((Entity = abs(*(Entity_P++)))) {
          if(EntitiesInTree_P[--Entity] != NULL) {
            if(components.merge(EntityInTree_S.Index,
                                *EntitiesInTree_P[Entity]))
              Flag_Change = 1;
          }
          else {
            EntityInTree_S.Num = abs(Geo_Element->NumNodes[Entity]);
            EntitiesInTree_P[Entity] =
              EntitiesInTree.add(EntityInTree_S.Num, EntityInTree_S.Index);
            Flag_Change = 1;
          }
        }

        if(Flag_Change) {
          Num_Entity2 = abs(Geo_Element->NumEdges[i_Entity2]);
          List_Add(ExtendedList, &Num_Entity2);
        }
      } /* for i_Entity2 ... */
    } /* if (Dim) */
  } /* for i_Element ... */
}

template <class Nodes>
static void Geo_GenerateEdgesOfTreeInRegions(
  List_T *InitialList, bool isInitialListEL, List_T *InitialSuppList,
  bool isInitialSuppListEL, List_T *ExtendedList, Nodes &nodes,
  Geo_TreeComponents &components)
{
  if(InitialSuppList != NULL) { // SubRegion
    Geo_GenerateEdgesOfTreeByDimension(1, InitialSuppList, isInitialSuppListEL,
                                       ExtendedList, nodes, components);
    Geo_GenerateEdgesOfTreeByDimension(2, InitialSuppList, isInitialSuppListEL,
                                       ExtendedList, nodes, components);
  }
  if(InitialList != NULL) { // Region
    Geo_GenerateEdgesOfTreeByDimension(-1, InitialList, isInitialListEL,
                                       ExtendedList, nodes, components);
  }
}

void Geo_GenerateEdgesOfTree(List_T *InitialList, bool isInitialListEL,
                             List_T *InitialSuppList, bool isInitialSuppListEL,
                             List_T *InitialSuppList2,
                             bool isInitialSuppList2EL, int SuppListType2,
                             List_T **ExtendedList)
{
  Geo_TreeComponents components;

  *ExtendedList = List_Create(2000, 2000, sizeof(int));

  if(InitialSuppList2 != NULL) { // SubRegion2
    Geo_TreeNodesInTree nodes = {Tree_Create(2 * sizeof(int), fcmp_int)};
    Geo_GenerateTreeOnSlidingSurface(InitialSuppList2, *ExtendedList, nodes.tree,
                                     components, SuppListType2);
    Geo_GenerateEdgesOfTreeInRegions(InitialList, isInitialListEL,
                                     InitialSuppList, isInitialSuppListEL,
                                     *ExtendedList, nodes, components);
    Tree_Delete(nodes.tree);
  }
  else {
    Geo_TreeNodesInArray nodes;
    Geo_GenerateEdgesOfTreeInRegions(InitialList, isInitialListEL,
                                     InitialSuppList, isInitialSuppListEL,
                                     *ExtendedList, nodes, components);
  }

  List_Sort(*ExtendedList, fcmp_int);
}

#if defined(HAVE_MPI)
// A tree of edges built over the ranks, each one having only its part of the
// mesh: the elements of its partition and those of the other partitions that
// share a node with them, so that a rank has all the elements around the
// nodes of its partition. The tree is the one of the serial algorithm.
//
// The serial algorithm takes the edges in the order of the elements (those of
// the lines and then of the surfaces of the StartingOn region, then those of
// the region, each in the order of their numbers) and keeps an edge if it
// connects two nodes not yet connected. This is Kruskal's algorithm, with the
// position of the first element of an edge as its weight: the tree is the
// minimum spanning forest for these weights, which is unique. This forest is
// built in two steps:
//
// 1. Each edge is treated by one rank, its owner: the lowest rank of the
//    partitions of the elements that contain it, which has all of them and
//    thus knows its weight. Each rank builds the minimum spanning forest of
//    the edges it owns. An edge that is not in it is the heaviest of a cycle,
//    and is thus not in the tree.
//
// 2. A node is shared if the elements around it are in the partitions of
//    several ranks; the other nodes are only reached by the edges of one rank.
//    A cycle of the union of the forests of the ranks thus goes from rank to
//    rank through shared nodes, along paths of each forest between shared
//    nodes. Each rank removes from its forest the branches without shared
//    nodes (which are in the tree), and sends each remaining path between
//    shared nodes or forks to the first rank, as a link between its two ends
//    with the weight of its heaviest edge. The first rank applies Kruskal's
//    algorithm to these links, and a rank removes the heaviest edge of each
//    path whose link is not kept.
//
// Each rank then has the edges of the tree that it owns. For the other edges
// of its part of the mesh, it asks a rank chosen from the number of the edge,
// to which the owners send their edges.

namespace {
struct Geo_UnionFind {
  std::vector<int> parent;
  int add()
  {
    parent.push_back((int)parent.size());
    return (int)parent.size() - 1;
  }
  int root(int i)
  {
    while(parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
  }
  bool merge(int i, int j)
  {
    i = root(i);
    j = root(j);
    if(i == j) return false;
    parent[j] = i;
    return true;
  }
};

// the index of a node from its number, in a union-find structure
struct Geo_NodesOfForest {
  std::unordered_map<int, int> index;
  std::vector<int> num;
  Geo_UnionFind sets;
  int get(int n)
  {
    auto it = index.find(n);
    if(it != index.end()) return it->second;
    num.push_back(n);
    return index[n] = sets.add();
  }
};
} // namespace

void Geo_GenerateEdgesOfTreeOverRanks(List_T *InitialList,
                                      List_T *InitialSuppList,
                                      List_T **ExtendedList)
{
  const int np = Message::GetCommSize(), me = Message::GetCommRank();
  const int nElements = Geo_GetNbrGeoElements();

  // the lowest rank of the elements of the part of the mesh that contain each
  // edge (its owner if it is this rank, which then has all these elements),
  // and the rank of the elements around each node (-1 if several)
  std::unordered_map<int, int> lowest, rankOfNode;
  for(int i = 0; i < nElements; i++) {
    struct Geo_Element *ge = Geo_GetGeoElement(i);
    int rank = Geo_RankOfPartition(ge->Partition);
    for(int k = 0; k < ge->NbrEdges; k++) {
      auto it = lowest.emplace(abs(ge->NumEdges[k]), rank).first;
      it->second = std::min(it->second, rank);
    }
    for(int k = 0; k < ge->NbrNodes; k++) {
      auto it = rankOfNode.emplace(abs(ge->NumNodes[k]), rank).first;
      if(it->second != rank) it->second = -1;
    }
  }

  // 1. the minimum spanning forest of the edges of this rank, with as weight
  // of an edge its first position (pass, element number, edge in the element),
  // the edges being taken in this order
  struct Edge {
    long long weight;
    int a, b, num; // its nodes (in nodes) and its number
  };
  std::vector<Edge> forest;
  Geo_NodesOfForest nodes;
  auto pass = [&](int p, int dim, List_T *List) {
    for(int i = 0; i < nElements; i++) {
      struct Geo_Element *ge = Geo_GetGeoElement(i);
      if(!ge->NbrEdges || !List_Search(List, &ge->Region, fcmp_int)) continue;
      int d, n;
      Get_JacobianFunction(JACOBIAN_VOL, ge->Type, &d);
      if(dim >= 0 && d != dim) continue;
      int *D = Geo_GetIM_Den(ge->Type, &n);
      for(int k = 0; k < ge->NbrEdges; k++) {
        int num = abs(ge->NumEdges[k]);
        if(lowest[num] != me) continue;
        int *ends = D + k * NBR_MAX_SUBENTITIES_IN_ELEMENT;
        int a = nodes.get(abs(ge->NumNodes[abs(ends[0]) - 1]));
        int b = nodes.get(abs(ge->NumNodes[abs(ends[1]) - 1]));
        if(nodes.sets.merge(a, b))
          forest.push_back({((long long)p << 50) | ((long long)ge->Num << 4) | k,
                            a, b, num});
      }
    }
  };
  if(InitialSuppList) { // StartingOn
    pass(0, 1, InitialSuppList);
    pass(1, 2, InitialSuppList);
  }
  if(InitialList) pass(2, -1, InitialList);

  // 2. the paths of the forest between shared nodes or forks, after removing
  // the branches without shared nodes: degree counts the edges of a node that
  // are still on such paths
  const int nNodes = (int)nodes.num.size();
  auto shared = [&](int i) { return rankOfNode[nodes.num[i]] < 0; };
  std::vector<std::vector<int> > edgesOfNode(nNodes);
  for(int e = 0; e < (int)forest.size(); e++) {
    edgesOfNode[forest[e].a].push_back(e);
    edgesOfNode[forest[e].b].push_back(e);
  }
  auto other = [&](int e, int i) {
    return (forest[e].a == i) ? forest[e].b : forest[e].a;
  };
  std::vector<int> degree(nNodes), leaves;
  std::vector<char> onPath(forest.size(), 1);
  for(int i = 0; i < nNodes; i++) {
    degree[i] = (int)edgesOfNode[i].size();
    if(degree[i] == 1 && !shared(i)) leaves.push_back(i);
  }
  while(!leaves.empty()) {
    int i = leaves.back();
    leaves.pop_back();
    for(int e : edgesOfNode[i]) {
      if(!onPath[e]) continue;
      onPath[e] = 0;
      degree[i]--;
      int j = other(e, i);
      if(--degree[j] == 1 && !shared(j)) leaves.push_back(j);
    }
  }
  auto end = [&](int i) { return shared(i) || degree[i] != 2; };
  std::vector<int> links, heaviest; // the ends of each path, its heaviest edge
  std::vector<long long> weights;
  std::vector<char> walked(forest.size(), 0);
  for(int i = 0; i < nNodes; i++) {
    if(!degree[i] || !end(i)) continue;
    for(int e0 : edgesOfNode[i]) {
      if(!onPath[e0] || walked[e0]) continue;
      int j = i, e = e0, max = e0;
      while(1) {
        walked[e] = 1;
        if(forest[e].weight > forest[max].weight) max = e;
        j = other(e, j);
        if(end(j)) break;
        for(int f : edgesOfNode[j])
          if(onPath[f] && f != e) {
            e = f;
            break;
          }
      }
      links.push_back(nodes.num[i]);
      links.push_back(nodes.num[j]);
      heaviest.push_back(max);
      weights.push_back(forest[max].weight);
    }
  }

  // the first rank applies Kruskal's algorithm to the links of all the ranks,
  // and tells each rank which ones it keeps
  int count = (int)weights.size(), total = 0;
  std::vector<int> counts(np), displs(np), allLinks;
  std::vector<long long> allWeights;
  MPI_Gather(&count, 1, MPI_INT, &counts[0], 1, MPI_INT, 0, MPI_COMM_WORLD);
  if(!me) {
    for(int r = 1; r < np; r++) displs[r] = displs[r - 1] + counts[r - 1];
    total = displs[np - 1] + counts[np - 1];
  }
  allWeights.resize(total + 1);
  MPI_Gatherv(weights.data(), count, MPI_LONG_LONG, &allWeights[0], &counts[0],
              &displs[0], MPI_LONG_LONG, 0, MPI_COMM_WORLD);
  for(int r = 0; r < np; r++) {
    counts[r] *= 2;
    displs[r] *= 2;
  }
  allLinks.resize(2 * total + 1);
  MPI_Gatherv(links.data(), 2 * count, MPI_INT, &allLinks[0], &counts[0],
              &displs[0], MPI_INT, 0, MPI_COMM_WORLD);
  std::vector<int> keep(total + 1), kept(count + 1), order(total);
  for(int j = 0; j < total; j++) order[j] = j;
  std::sort(order.begin(), order.end(),
            [&](int x, int y) { return allWeights[x] < allWeights[y]; });
  Geo_NodesOfForest ends;
  for(int j : order)
    keep[j] = ends.sets.merge(ends.get(allLinks[2 * j]),
                              ends.get(allLinks[2 * j + 1]));
  for(int r = 0; r < np; r++) {
    counts[r] /= 2;
    displs[r] /= 2;
  }
  MPI_Scatterv(&keep[0], &counts[0], &displs[0], MPI_INT, &kept[0], count,
               MPI_INT, 0, MPI_COMM_WORLD);
  std::vector<int> tree;
  std::vector<char> removed(forest.size(), 0);
  long long stats[3] = {0, count, 0}; // edges, paths, paths cut
  for(int j = 0; j < count; j++) {
    if(kept[j]) continue;
    removed[heaviest[j]] = 1;
    stats[2]++;
  }
  for(std::size_t e = 0; e < forest.size(); e++)
    if(!removed[e]) tree.push_back(forest[e].num);
  stats[0] = tree.size();
  MPI_Allreduce(MPI_IN_PLACE, stats, 3, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
  Message::Info("  %lld edges in the tree, %lld paths between the ranks "
                "checked (%lld cut)", stats[0], stats[1], stats[2]);

  // the edges of the part of the mesh owned by other ranks: each one is asked
  // to the rank chosen from its number, to which its owner sent it if it is
  // in the tree
  const long long nEdges = CurrentGeoData->NumCurrentEdge;
  // (in 64 bits: the number of edges times the number of ranks overflows an
  // int, e.g. 6M edges on 384 ranks)
  auto rankOfEdge = [&](int num) {
    return (int)((long long)(num - 1) * np / nEdges);
  };
  std::vector<std::vector<int> > send(np), recv;
  if(nEdges) {
    for(int num : tree) send[rankOfEdge(num)].push_back(num);
    Message::Exchange(send, recv);
    std::vector<int> known;
    for(int r = 0; r < np; r++)
      known.insert(known.end(), recv[r].begin(), recv[r].end());
    std::sort(known.begin(), known.end());
    for(int r = 0; r < np; r++) send[r].clear();
    for(auto &it : lowest)
      if(it.second != me) send[rankOfEdge(it.first)].push_back(it.first);
    Message::Exchange(send, recv);
    for(int r = 0; r < np; r++) {
      send[r].clear();
      for(int num : recv[r])
        if(std::binary_search(known.begin(), known.end(), num))
          send[r].push_back(num);
    }
    Message::Exchange(send, recv);
    for(int r = 0; r < np; r++)
      tree.insert(tree.end(), recv[r].begin(), recv[r].end());
  }
  std::sort(tree.begin(), tree.end());
  *ExtendedList = List_Create((int)tree.size() + 1, 1000, sizeof(int));
  for(int num : tree) List_Add(*ExtendedList, &num);
}
#endif

/* ------------------------------------------------------------------------ */
/*  G e o _ G e n e r a t e F a c e t s O f T r e e                         */
/* ------------------------------------------------------------------------ */

static void Geo_GenerateFacetsOfSubTree(List_T *InitialList,
                                        List_T *ExtendedList,
                                        Tree_T *EntitiesInTree_T)
{
  int Nbr_Element, i_Element, Nbr_Entities2, i, Num_Entity1;
  struct Geo_Element *Geo_Element;
  int i_Entity2, Num_Entity2, *D_Element, *Entity_P, Entity, Flag_Change;
  struct EntityInTree *EntitiesInTree_P[NBR_MAX_ENTITIES_IN_ELEMENT];
  struct EntityInTree EntityInTree_S;

  Nbr_Element = Geo_GetNbrGeoElements();
  for(i_Element = 0; i_Element < Nbr_Element; i_Element++) {
    Geo_Element = Geo_GetGeoElement(i_Element);

    if(List_Search(InitialList, &Geo_Element->Region, fcmp_int)) {
      if(Geo_Element->NbrEdges == 0) Geo_CreateEdgesOfElement(Geo_Element);
      if(Geo_Element->NbrFacets == 0) Geo_CreateFacetsOfElement(Geo_Element);
      D_Element = Geo_GetIM_Dfe(Geo_Element->Type, &Nbr_Entities2);

      for(i = 0; i < Geo_Element->NbrEdges; i++) {
        Num_Entity1 = abs(Geo_Element->NumEdges[i]);
        EntitiesInTree_P[i] =
          (struct EntityInTree *)Tree_PQuery(EntitiesInTree_T, &Num_Entity1);
      }

      for(i_Entity2 = 0; i_Entity2 < Geo_Element->NbrFacets; i_Entity2++) {
        Entity_P = D_Element + i_Entity2 * NBR_MAX_SUBENTITIES_IN_ELEMENT;

        i = 0;
        EntityInTree_S.Index = -1;
        while((Entity = abs(Entity_P[i++])) && (EntityInTree_S.Index < 0))
          if(EntitiesInTree_P[Entity - 1] != NULL)
            EntityInTree_S.Index = EntitiesInTree_P[Entity - 1]->Index;
        if(EntityInTree_S.Index < 0) EntityInTree_S.Index = Geo_Element->Num;

        Flag_Change = 0;

        while((Entity = abs(*(Entity_P++)))) {
          if(EntitiesInTree_P[--Entity] != NULL) {
            if(EntitiesInTree_P[Entity]->Index != EntityInTree_S.Index) {
              Tree_IndexToChange = EntitiesInTree_P[Entity]->Index;
              Tree_NewIndex = EntityInTree_S.Index;
              Tree_Action(EntitiesInTree_T, Geo_ChangeTreeIndex);
              Flag_Change = 1;
            }
            else if(Geo_Element->NbrFacets == 1)
              Flag_Change = 1;
          }
          else {
            EntityInTree_S.Num = abs(Geo_Element->NumEdges[Entity]);
            EntitiesInTree_P[Entity] = (struct EntityInTree *)Tree_Add(
              EntitiesInTree_T, &EntityInTree_S);
            Flag_Change = 1;
          }
        }

        if(Flag_Change) {
          Num_Entity2 = abs(Geo_Element->NumFacets[i_Entity2]);
          List_Add(ExtendedList, &Num_Entity2);
        }

      } /* for i_Entity2 ... */
    } /* if Region ... */

  } /* for i_Element ... */
}

void Geo_GenerateFacetsOfTree(List_T *InitialList, List_T *InitialSuppList,
                              List_T *InitialSuppList2, List_T **ExtendedList)
{
  Tree_T *EntitiesInTree_T;

  *ExtendedList = List_Create(2000, 2000, sizeof(int));

  EntitiesInTree_T = Tree_Create(2 * sizeof(int), fcmp_int);

  if(InitialSuppList2 != NULL)
    Geo_GenerateFacetsOfSubTree(InitialSuppList2, *ExtendedList,
                                EntitiesInTree_T);
  if(InitialSuppList != NULL)
    Geo_GenerateFacetsOfSubTree(InitialSuppList, *ExtendedList,
                                EntitiesInTree_T);
  if(InitialList != NULL)
    Geo_GenerateFacetsOfSubTree(InitialList, *ExtendedList, EntitiesInTree_T);

  Tree_Delete(EntitiesInTree_T);

  List_Sort(*ExtendedList, fcmp_int);
}
