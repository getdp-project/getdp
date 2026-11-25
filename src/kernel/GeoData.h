// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef GEO_DATA_H
#define GEO_DATA_H

#include "ListUtils.h"
#include "TreeUtils.h"
#include <map>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_set>

// The position in a list of the item (node, element) of each number: a table
// indexed by the number if the numbers are dense enough (the usual case), and
// otherwise (e.g. in a part of a mesh, which has few of the numbers of the
// whole mesh) a hash table.
struct Geo_NumberIndex {
  int size = -1; // number of items in the list when the index was built
  bool dense = true;
  std::vector<int> table;
  std::unordered_map<int, int> hash;
  // builds the index of the n items whose numbers are given by num(i)
  template <class Num> void build(int n, Num num)
  {
    size = n;
    table.clear();
    hash.clear();
    int min = 0, max = 0;
    for(int i = 0; i < n; i++) {
      min = std::min(min, num(i));
      max = std::max(max, num(i));
    }
    dense = (min >= 0 && max / 4 <= n + 1000);
    if(dense) {
      table.resize(max + 1, -1);
      for(int i = 0; i < n; i++) table[num(i)] = i;
    }
    else {
      hash.reserve(n);
      for(int i = 0; i < n; i++) hash[num(i)] = i;
    }
  }
  // the position of the item of a given number, or -1 if there is none
  int find(int num) const
  {
    if(dense) return (num < 0 || num >= (int)table.size()) ? -1 : table[num];
    auto it = hash.find(num);
    return (it == hash.end()) ? -1 : it->second;
  }
};

struct GeoData {
  int Num;
  char *Name;
  List_T *Nodes, *Elements;
  int NbrElementsWithEdges, NbrElementsWithFacets;
  int NumCurrentEdge, NumCurrentFacet;
  class GeoEntityTable *EdgesXNodes, *FacetsXEdges;

  Tree_T *NodesXElements;
  Tree_T *Normals;

  // the groups written in the .pre file: by the first rank, or by each rank in
  // its section for those that each one has its own part of (ByRank)
  List_T *GroupForPRE, *GroupForPREByRank;

  double Xmin, Xmax, Ymin, Ymax, Zmin, Zmax;
  double Dimension, CharacteristicLength;

  // the tree of the boxes of the elements, to find the element that contains
  // a point (built by the first search, see InWhichElement)
  struct ElementSearch *ElementSearch;

  double *H, *P;

  // slave node -> (master node, link), the link being identified by the index
  // of its slave group (filled from the facet links, see Generate_Link)
  std::map<int, std::pair<int, int> > *PeriodicNodes;

  int NbrPartitions;
  bool HasMovingBand;

  // position in Nodes of the node of each number, to find a node without a
  // search (see Geo_FindNode), and the same for the elements (see
  // Geo_GetGeoElementOfNum)
  struct Geo_NumberIndex *NodeIndex, *ElementIndex;

  // In parallel, with rank-local Dofs, only the first rank reads the mesh: it
  // sends to each other rank the part of the mesh this rank needs
  // (Geo_SendParts, Geo_ReceivePart). IsPart is set on the ranks that only
  // have their part, and HasParts on the first rank if it keeps the whole
  // mesh; EntitiesLevel tells which entities of the elements of the parts have
  // been numbered over the ranks (1: edges, 2: edges and facets).
  bool HasParts, IsPart;
  int EntitiesLevel;
  // the positions of the elements of the part of the first rank, as long as it
  // keeps the whole mesh to build the trees (see Geo_KeepPartOnFirstRank)
  std::vector<int> *FirstRankPart;
};

void Geo_GetLinkRegions(std::unordered_set<int> &regions);
void Geo_SetWholeMeshOnFirstRank(int whole);
int Geo_GetWholeMeshOnFirstRank();
void Geo_KeepPartOnFirstRank(struct GeoData *GeoData_P);
int Geo_RankOfPartition(int partition);
int Geo_AddGeoData(List_T *GeoData_L, char *Name_MshFile,
                   char *Name_DefaultMshFile, char *Name_AdaptFile,
                   char *Name_DefaultAdaptFile);
void Geo_InitGeoData(struct GeoData *GeoData_P, int Num, char *Name);
void Geo_FreeGeoData(struct GeoData *GeoData_P);

void Geo_SetCurrentGeoData(struct GeoData *GeoData_P);

void Geo_OpenFile(char *Name, const char *Mode);
void Geo_CloseFile(void);
void Geo_ReadFile(struct GeoData *GeoData_P);
void Geo_ReadFileAdapt(struct GeoData *GeoData_P);

void Geo_SaveMesh(struct GeoData *GeoData_P, List_T *InitialList,
                  char *FileName);

int Geo_GetNbrPartitions(void);

int Geo_GetNbrGeoElements(void);
struct Geo_Element *Geo_GetGeoElement(int Index_Element);
int Geo_GetGeoElementIndex(struct Geo_Element *GeoElement);
struct Geo_Element *Geo_GetGeoElementOfNum(int Num_Element);
int Geo_GetDimOfElement(int Type);
int Geo_GetNbCornerNodes(int Type, int NbrNodes);
// Calls f(lower, top) for each element and each element of higher dimension
// that contains all its corner nodes, those of the highest dimension first,
// then in the order of the elements; f returns false to go to the next element
void Geo_ForEachElementContaining(struct GeoData *GeoData_P,
                                  const std::function<bool(int, int)> &f);

int Geo_GetNbrGeoNodes(void);
struct Geo_Node *Geo_GetGeoNode(int Index_Node);
struct Geo_Node *Geo_GetGeoNodeOfNum(int Num_Node);

void Geo_GetNodesCoordinates(int Nbr_Node, int *Num_Node, double *x, double *y,
                             double *z);
void Geo_SetNodesCoordinates(int Nbr_Node, int *Num_Node, double *x, double *y,
                             double *z);

void Geo_SetNodesCoordinatesX(int Nbr_Node, int *Num_Node, double *x);
void Geo_SetNodesCoordinatesY(int Nbr_Node, int *Num_Node, double *y);
void Geo_SetNodesCoordinatesZ(int Nbr_Node, int *Num_Node, double *z);

double *Geo_GetNodes_uvw(int Type, int *nbn);
double *Geo_GetBarycenter_uvw(int Type);

void Geo_CreateEdgesOfElement(struct Geo_Element *Geo_Element);
int *Geo_GetNodesOfEdgeInElement(struct Geo_Element *Geo_Element, int Num_Edge);
void Geo_CreateFacetsOfElement(struct Geo_Element *Geo_Element);
void Geo_CreateEdgesAndFacets(bool facets);
int *Geo_GetNodesOfFacetInElement(struct Geo_Element *Geo_Element,
                                  int Num_Facet);

int *Geo_GetIM_Den(int Type_Element, int *Nbe);
int *Geo_GetIM_Dfe(int Type_Element, int *Nbf);
int *Geo_GetIM_Dfn(int Type_Element, int *Nbf);
int *Geo_GetIM_Den_Xp(int Type_Element, int *Nbe, int *Nbn);
int *Geo_GetIM_Dfe_Xp(int Type_Element, int *Nbf, int *Nbe);

void Geo_CreateEntitiesOfElement(
  int Nbr_Entities2, int *D_Element, int Geo_Element_NbrEntities1,
  int *Geo_Element_NumEntities1, int *Geo_Element_NbrEntities2,
  int **Geo_Element_NumEntities2, int *Geo_NbrElementsWithEntities2,
  int *Geo_NumCurrentEntity2, class GeoEntityTable *Geo_Entities2XEntities1);

void Geo_GenerateEdgesOfTree(List_T *InitialList, bool isInitialListEL,
                             List_T *InitialSuppList, bool isInitialSuppListEL,
                             List_T *InitialSuppList2,
                             bool isInitialSuppList2EL, int SuppListType2,
                             List_T **ExtendedList);
void Geo_GenerateEdgesOfTreeOverRanks(List_T *InitialList,
                                      List_T *InitialSuppList,
                                      List_T **ExtendedList);
void Geo_GenerateFacetsOfTree(List_T *InitialList, List_T *InitialSuppList,
                              List_T *InitialSuppList2, List_T **ExtendedList);

int fcmp_GeoData_Name(const void *a, const void *b);
int fcmp_Elm(const void *a, const void *b);
int fcmp_Nod(const void *a, const void *b);
int fcmp_E2XE1(const void *a, const void *b);
void free_E2XE1(void *a, void *b);
int fcmp_EXVector(const void *a, const void *b);

void Geo_WriteFilePRE(struct GeoData *GeoData_P, List_T *Group_L);
void Geo_ReadFilePRE(struct GeoData *GeoData_P, int NbrGeoData,
                     List_T *Group_L);

void Geo_WriteEntities2XEntities1(void *a, void *b);

void Geo_AddGroupForPRE(int Num, bool byRank = false);

void Geo_CreateNodesXElements(int NumNode, int InIndex, int *NbrElements,
                              int **NumElements);

void Geo_CreateNormal(int Type, double *x, double *y, double *z, double *N);

void Geo_CreateNormalOfElement(struct Geo_Element *GeoElement, double *Normal);

#endif
