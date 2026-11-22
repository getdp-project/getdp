// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <string.h>
#include <math.h>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include "GetDPConfig.h"
#include "GeoData.h"
#include "DofData.h"
#include "GeoEntityTable.h"
#include "ProData.h"
#include "ProDefine.h"
#include "Pos_Search.h"
#include "MallocUtils.h"
#include "Message.h"
#include "OS.h"

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

#if defined(HAVE_GMSH)
#include "gmsh.h"
#include "GModel.h"
#endif

#define SQU(a) ((a) * (a))

extern double Flag_ORDER, Flag_MSH_SCALING;
extern int Flag_CALLED_WITH_ONELAB_SERVER;
extern int Flag_PRE, Flag_LOCAL_DOFS;
extern struct Problem Problem_S;

FILE *File_GEO;
char *name;

struct GeoData *CurrentGeoData;

// Whether the ranks read their part of the mesh together (the numbers of the
// elements that Geo_ReadFileWithGmsh creates then depend on the whole mesh)
#if defined(HAVE_MPI)
static bool ReadsPart = false;
#endif
static void Geo_ReadFileWithGmsh(struct GeoData *GeoData_P, bool loaded = false);

static void swapBytes(char *array, int size, int n)
{
  int i, c;

  char *x = (char *)Malloc(size * sizeof(char));
  for(i = 0; i < n; i++) {
    char *a = &array[i * size];
    memcpy(x, a, size);
    for(c = 0; c < size; c++) a[size - 1 - c] = x[c];
  }
  Free(x);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ A d d G e o D a t a                                             */
/* ------------------------------------------------------------------------ */

// Recursive coordinate bisection: assigns the elements e[begin, end) to the
// partitions first, ..., first + n - 1, by splitting them along the direction in
// which their barycenters (x[d][element]) are the most spread out
static void Geo_Bisect(struct GeoData *GeoData_P, const std::vector<double> x[3],
                       std::vector<int> &e, int begin, int end, int first, int n)
{
  if(n == 1) {
    for(int i = begin; i < end; i++)
      ((struct Geo_Element *)List_Pointer(GeoData_P->Elements, e[i]))
        ->Partition = first;
    return;
  }
  int dir = 0;
  double size = -1.;
  for(int d = 0; d < 3; d++) {
    double min = 1e200, max = -1e200;
    for(int i = begin; i < end; i++) {
      min = std::min(min, x[d][e[i]]);
      max = std::max(max, x[d][e[i]]);
    }
    if(max - min > size) {
      size = max - min;
      dir = d;
    }
  }
  int mid = begin + (int)((long long)(end - begin) * (n / 2) / n);
  std::nth_element(e.begin() + begin, e.begin() + mid, e.begin() + end,
                   [&](int a, int b) { return x[dir][a] < x[dir][b]; });
  Geo_Bisect(GeoData_P, x, e, begin, mid, first, n / 2);
  Geo_Bisect(GeoData_P, x, e, mid, end, first + n / 2, n - n / 2);
}

// The node of a given number: its position in the list of the nodes is
// tabulated (see Geo_NumberIndex), which replaces a binary search for each
// node of each element by a direct access.
static struct Geo_Node *Geo_FindNode(struct GeoData *GeoData_P, int Num)
{
  struct Geo_NumberIndex &index = *GeoData_P->NodeIndex;
  int n = List_Nbr(GeoData_P->Nodes);
  if(index.size != n) {
    // the positions must not change afterwards: a search sorts the list
    List_Sort(GeoData_P->Nodes, fcmp_Nod);
    struct Geo_Node *nodes =
      n ? (struct Geo_Node *)List_Pointer(GeoData_P->Nodes, 0) : NULL;
    index.build(n, [&](int i) { return nodes[i].Num; });
  }
  int i = index.find(Num);
  return (i < 0) ? NULL : (struct Geo_Node *)List_Pointer(GeoData_P->Nodes, i);
}

// In parallel each mesh element is assembled by the rank of its mesh partition,
// and the Dofs are numbered by partition, so that a rank owns most of the rows
// of its elements. If the mesh does not come with one partition for each rank,
// its elements are thus partitioned here, by recursive coordinate bisection (or
// by merging its partitions, if their number is a multiple of the number of
// ranks). Partitioning the mesh with Gmsh gives better partitions.
static void Geo_SetPartitions(struct GeoData *GeoData_P)
{
  int np = Message::GetCommSize(), n = List_Nbr(GeoData_P->Elements);
  if(np == 1 || !n) return;
  int nbr = 0;
  for(int i = 0; i < n; i++)
    nbr = std::max(
      nbr, ((struct Geo_Element *)List_Pointer(GeoData_P->Elements, i))->Partition);
  if(nbr == np) return;
  if(nbr > np && nbr % np == 0) {
    Message::Info("Merging the %d mesh partitions for %d ranks", nbr, np);
    for(int i = 0; i < n; i++) {
      struct Geo_Element *e =
        (struct Geo_Element *)List_Pointer(GeoData_P->Elements, i);
      if(e->Partition > 0) e->Partition = (e->Partition - 1) % np + 1;
    }
  }
  else {
    Message::Info("Partitioning the mesh for %d ranks (it has %d partitions)",
                  np, nbr);
    std::vector<double> x[3];
    std::vector<int> e(n);
    for(int d = 0; d < 3; d++) x[d].resize(n, 0.);
    for(int i = 0; i < n; i++) {
      e[i] = i;
      struct Geo_Element *ele =
        (struct Geo_Element *)List_Pointer(GeoData_P->Elements, i);
      for(int j = 0; j < ele->NbrNodes; j++) {
        struct Geo_Node *node_P;
        if((node_P = Geo_FindNode(GeoData_P, abs(ele->NumNodes[j])))) {
          x[0][i] += node_P->x / ele->NbrNodes;
          x[1][i] += node_P->y / ele->NbrNodes;
          x[2][i] += node_P->z / ele->NbrNodes;
        }
      }
    }
    Geo_Bisect(GeoData_P, x, e, 0, n, 1, np);
  }
  GeoData_P->NbrPartitions = np;
}

// The regions of the Link constraints of the problem (slave and reference)
void Geo_GetLinkRegions(std::unordered_set<int> &regions)
{
  for(int i = 0; i < List_Nbr(Problem_S.Constraint); i++) {
    struct Constraint *c =
      (struct Constraint *)List_Pointer(Problem_S.Constraint, i);
    for(int j = 0; j < List_Nbr(c->ConstraintPerRegion); j++) {
      struct ConstraintPerRegion *cpr =
        (struct ConstraintPerRegion *)List_Pointer(c->ConstraintPerRegion, j);
      if(cpr->Type != CST_LINK && cpr->Type != CST_LINKCPLX) continue;
      int groups[2] = {cpr->RegionIndex, cpr->Case.Link.RegionRefIndex};
      for(int g = 0; g < 2; g++) {
        if(groups[g] < 0) continue;
        List_T *l =
          ((struct Group *)List_Pointer(Problem_S.Group, groups[g]))->InitialList;
        for(int k = 0; k < List_Nbr(l); k++)
          regions.insert(*(int *)List_Pointer(l, k));
      }
    }
  }
}

// In parallel, with rank-local Dofs, the other ranks do not read the mesh: the
// first rank sends to each of them the part of the mesh it needs, i.e.
//
// - the elements whose Dofs it defines: those of its mesh partition, which it
//   assembles, and those that touch a region of a Link constraint, whose Dofs
//   are defined by all the ranks (see Treatment_FemFormulation);
// - the elements that share a node with them: to know to which regions the
//   nodes, edges and facets of these elements belong (e.g. whether a node is
//   on a surface with a boundary condition), a rank must have all the elements
//   that contain them;
//
// with their nodes. The first rank then only keeps its own part, unless the
// problem has something that it still does alone on the whole mesh: the
// post-processing that is not parallel, SaveMesh (WholeMeshOnFirstRank = 2),
// or the trees, which it builds in the pre-processing and sends to the others
// (1: it only keeps its part after that); see Geo_SetWholeMeshOnFirstRank.
static int WholeMeshOnFirstRank = 2;
void Geo_SetWholeMeshOnFirstRank(int whole) { WholeMeshOnFirstRank = whole; }
int Geo_GetWholeMeshOnFirstRank() { return WholeMeshOnFirstRank; }

static bool Geo_CanDistributeMesh(char *Name_AdaptFile)
{
#if defined(HAVE_MPI)
  // in the run that does the pre-processing, or in a later one that reads the
  // Dofs that each rank had
  return (Flag_PRE || Dof_ReadsOwnSectionsPRE()) && Flag_LOCAL_DOFS &&
         !Name_AdaptFile && Message::GetCommSize() > 1 &&
         Message::GetIsCommWorld();
#else
  return false;
#endif
}

// Leaves on the first rank the elements of its part of the mesh (those that
// Geo_SendParts found for it), with their nodes, edges and facets.
void Geo_KeepPartOnFirstRank(struct GeoData *GeoData_P)
{
  if(!GeoData_P->HasParts || !GeoData_P->FirstRankPart) return;
  const int nElements = List_Nbr(GeoData_P->Elements);
  const int nNodes = List_Nbr(GeoData_P->Nodes);
  struct Geo_Node *node0 =
    nNodes ? (struct Geo_Node *)List_Pointer(GeoData_P->Nodes, 0) : NULL;
  std::vector<char> keepElement(nElements, 0), keepNode(nNodes, 0);
  std::vector<char> keepEdge(GeoData_P->NumCurrentEdge + 1, 0),
    keepFacet(GeoData_P->NumCurrentFacet + 1, 0);
  for(int e : *GeoData_P->FirstRankPart) {
    keepElement[e] = 1;
    struct Geo_Element *ge =
      (struct Geo_Element *)List_Pointer(GeoData_P->Elements, e);
    for(int k = 0; k < ge->NbrNodes; k++)
      keepNode[Geo_FindNode(GeoData_P, abs(ge->NumNodes[k])) - node0] = 1;
    for(int k = 0; k < ge->NbrEdges; k++) keepEdge[abs(ge->NumEdges[k])] = 1;
    for(int k = 0; k < ge->NbrFacets; k++) keepFacet[abs(ge->NumFacets[k])] = 1;
  }
  List_T *elements = List_Create(GeoData_P->FirstRankPart->size() + 1, 1000,
                                 sizeof(struct Geo_Element));
  for(int e = 0; e < nElements; e++) {
    struct Geo_Element *ge =
      (struct Geo_Element *)List_Pointer(GeoData_P->Elements, e);
    if(keepElement[e])
      List_Add(elements, ge);
    else {
      Free(ge->NumNodes);
      Free(ge->NumEdges);
      Free(ge->NumFacets);
    }
  }
  List_T *nodes = List_Create(List_Nbr(elements) + 1, 1000, sizeof(struct Geo_Node));
  for(int i = 0; i < nNodes; i++)
    if(keepNode[i]) List_Add(nodes, &node0[i]);
  {
    // (with -cpu a message asks all the ranks for their time and memory)
    MessageLocalCpu localCpu(true);
    Message::Info("The first rank keeps its part of the mesh (%d elements out "
                  "of %d)", List_Nbr(elements), nElements);
  }
  List_Delete(GeoData_P->Elements);
  List_Delete(GeoData_P->Nodes);
  GeoData_P->Elements = elements;
  GeoData_P->Nodes = nodes;
  GeoData_P->NodeIndex->size = GeoData_P->ElementIndex->size = -1;
  if(GeoData_P->EdgesXNodes) GeoData_P->EdgesXNodes->keepOnly(keepEdge);
  if(GeoData_P->FacetsXEdges) GeoData_P->FacetsXEdges->keepOnly(keepFacet);
  delete GeoData_P->FirstRankPart;
  GeoData_P->FirstRankPart = NULL;
  GeoData_P->HasParts = false;
  GeoData_P->IsPart = true;
}

#if defined(HAVE_MPI)
static int Geo_RankOfPartition(int partition)
{
  int np = Message::GetCommSize(); // as Dof_IsPartitionOfThisRank
  return (partition > 0) ? (partition - 1) % np : np - 1;
}

// The positions of the elements of the part of the mesh of each rank (of the
// ranks below last), in the whole mesh
static std::vector<std::vector<int> >
Geo_ElementsOfRanks(struct GeoData *GeoData_P, int last)
{
  const int np = Message::GetCommSize();
  const int nElements = List_Nbr(GeoData_P->Elements);
  const int nNodes = List_Nbr(GeoData_P->Nodes);
  struct Geo_Node *node0 = nNodes ?
    (struct Geo_Node *)List_Pointer(GeoData_P->Nodes, 0) : NULL;
  auto element = [&](int e) {
    return (struct Geo_Element *)List_Pointer(GeoData_P->Elements, e);
  };
  auto nodeIndex = [&](int num) {
    return (int)(Geo_FindNode(GeoData_P, abs(num)) - node0);
  };

  // the nodes of the regions of the Link constraints
  std::vector<char> onLink(nNodes, 0);
  std::unordered_set<int> linkRegions;
  Geo_GetLinkRegions(linkRegions);
  for(int e = 0; e < nElements && !linkRegions.empty(); e++) {
    struct Geo_Element *ge = element(e);
    if(!linkRegions.count(ge->Region)) continue;
    for(int k = 0; k < ge->NbrNodes; k++) onLink[nodeIndex(ge->NumNodes[k])] = 1;
  }

  // the ranks that define the Dofs of each element: the one of its partition,
  // or all of them if it touches a region of a Link constraint; this gives the
  // ranks that have each node in such an element ("all the ranks" is a flag)
  std::vector<std::vector<int> > ranksOfNode(nNodes);
  std::vector<char> allRanksOfNode(nNodes, 0);
  for(int e = 0; e < nElements; e++) {
    struct Geo_Element *ge = element(e);
    int rank = Geo_RankOfPartition(ge->Partition);
    bool all = false;
    for(int k = 0; k < ge->NbrNodes && !all; k++)
      all = onLink[nodeIndex(ge->NumNodes[k])];
    for(int k = 0; k < ge->NbrNodes; k++) {
      int i = nodeIndex(ge->NumNodes[k]);
      if(all)
        allRanksOfNode[i] = 1;
      else {
        std::vector<int> &r = ranksOfNode[i];
        if(std::find(r.begin(), r.end(), rank) == r.end()) r.push_back(rank);
      }
    }
  }

  // the elements of each rank: those that share a node with an element whose
  // Dofs it defines
  std::vector<std::vector<int> > elementsOfRank(last);
  std::vector<char> has(np);
  for(int e = 0; e < nElements; e++) {
    struct Geo_Element *ge = element(e);
    bool all = false;
    std::fill(has.begin(), has.end(), 0);
    for(int k = 0; k < ge->NbrNodes && !all; k++) {
      int i = nodeIndex(ge->NumNodes[k]);
      if(allRanksOfNode[i]) all = true;
      for(int rank : ranksOfNode[i]) has[rank] = 1;
    }
    for(int rank = 0; rank < last; rank++)
      if(all || has[rank]) elementsOfRank[rank].push_back(e);
  }
  return elementsOfRank;
}

// The first rank keeps the whole mesh, or only until the trees are built (they
// are read from the .pre file after the pre-processing), or not at all
static void Geo_SetFirstRankPart(struct GeoData *GeoData_P,
                                 const std::vector<int> &part)
{
  GeoData_P->HasParts = true;
  if(WholeMeshOnFirstRank == 2) return;
  GeoData_P->FirstRankPart = new std::vector<int>(part);
  if(WholeMeshOnFirstRank == 0 || !Flag_PRE) Geo_KeepPartOnFirstRank(GeoData_P);
}

static void Geo_SendParts(struct GeoData *GeoData_P)
{
  const int np = Message::GetCommSize();
  const int nNodes = List_Nbr(GeoData_P->Nodes);
  struct Geo_Node *node0 = nNodes ?
    (struct Geo_Node *)List_Pointer(GeoData_P->Nodes, 0) : NULL;
  auto element = [&](int e) {
    return (struct Geo_Element *)List_Pointer(GeoData_P->Elements, e);
  };
  auto nodeIndex = [&](int num) {
    return (int)(Geo_FindNode(GeoData_P, abs(num)) - node0);
  };
  std::vector<std::vector<int> > elementsOfRank =
    Geo_ElementsOfRanks(GeoData_P, np);

  std::vector<int> stamp(nNodes, 0);
  for(int rank = 1; rank < np; rank++) {
    std::vector<int> ints, nodes;
    for(int e : elementsOfRank[rank]) {
      struct Geo_Element *ge = element(e);
      int head[6] = {ge->Num, ge->Type, ge->Region, ge->ElementaryRegion,
                     ge->Partition, ge->NbrNodes};
      ints.insert(ints.end(), head, head + 6);
      ints.insert(ints.end(), ge->NumNodes, ge->NumNodes + ge->NbrNodes);
      for(int k = 0; k < ge->NbrNodes; k++) {
        int i = nodeIndex(ge->NumNodes[k]);
        if(stamp[i] != rank) {
          stamp[i] = rank;
          nodes.push_back(i);
        }
      }
    }
    std::sort(nodes.begin(), nodes.end());
    std::vector<int> nums(nodes.size());
    std::vector<double> reals(3 * nodes.size() + 7);
    for(std::size_t i = 0; i < nodes.size(); i++) {
      nums[i] = node0[nodes[i]].Num;
      reals[3 * i] = node0[nodes[i]].x;
      reals[3 * i + 1] = node0[nodes[i]].y;
      reals[3 * i + 2] = node0[nodes[i]].z;
    }
    double box[7] = {GeoData_P->Xmin, GeoData_P->Xmax, GeoData_P->Ymin,
                     GeoData_P->Ymax, GeoData_P->Zmin, GeoData_P->Zmax,
                     GeoData_P->CharacteristicLength};
    std::copy(box, box + 7, reals.end() - 7);
    long long sizes[5] = {(long long)elementsOfRank[rank].size(),
                          (long long)ints.size(), (long long)nodes.size(),
                          (long long)GeoData_P->Dimension,
                          (long long)GeoData_P->NbrPartitions};
    if(ints.size() > 2000000000u || reals.size() > 2000000000u) {
      Message::Error("The part of the mesh of rank %d is too large to be sent",
                     rank);
      return;
    }
    MPI_Send(sizes, 5, MPI_LONG_LONG, rank, 0, MPI_COMM_WORLD);
    MPI_Send(ints.empty() ? NULL : &ints[0], (int)ints.size(), MPI_INT, rank, 0,
             MPI_COMM_WORLD);
    MPI_Send(nums.empty() ? NULL : &nums[0], (int)nums.size(), MPI_INT, rank, 0,
             MPI_COMM_WORLD);
    MPI_Send(&reals[0], (int)reals.size(), MPI_DOUBLE, rank, 0, MPI_COMM_WORLD);
  }
  Geo_SetFirstRankPart(GeoData_P, elementsOfRank[0]);
}

static void Geo_ReceivePart(struct GeoData *GeoData_P)
{
  long long sizes[5];
  MPI_Recv(sizes, 5, MPI_LONG_LONG, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  std::vector<int> ints(sizes[1] + 1), nums(sizes[2] + 1);
  std::vector<double> reals(3 * sizes[2] + 7);
  MPI_Recv(&ints[0], (int)sizes[1], MPI_INT, 0, 0, MPI_COMM_WORLD,
           MPI_STATUS_IGNORE);
  MPI_Recv(&nums[0], (int)sizes[2], MPI_INT, 0, 0, MPI_COMM_WORLD,
           MPI_STATUS_IGNORE);
  MPI_Recv(&reals[0], (int)reals.size(), MPI_DOUBLE, 0, 0, MPI_COMM_WORLD,
           MPI_STATUS_IGNORE);

  GeoData_P->Nodes = List_Create(sizes[2] + 1, 1000, sizeof(struct Geo_Node));
  for(long long i = 0; i < sizes[2]; i++) {
    struct Geo_Node node;
    node.Num = nums[i];
    node.x = reals[3 * i];
    node.y = reals[3 * i + 1];
    node.z = reals[3 * i + 2];
    List_Add(GeoData_P->Nodes, &node);
  }
  const double *box = &reals[3 * sizes[2]];
  GeoData_P->Xmin = box[0];
  GeoData_P->Xmax = box[1];
  GeoData_P->Ymin = box[2];
  GeoData_P->Ymax = box[3];
  GeoData_P->Zmin = box[4];
  GeoData_P->Zmax = box[5];
  GeoData_P->CharacteristicLength = box[6];
  GeoData_P->Dimension = (int)sizes[3];
  GeoData_P->NbrPartitions = (int)sizes[4];

  GeoData_P->Elements =
    List_Create(sizes[0] + 1, 1000, sizeof(struct Geo_Element));
  const int *p = &ints[0];
  for(long long e = 0; e < sizes[0]; e++) {
    struct Geo_Element ge;
    ge.Num = p[0];
    ge.Type = p[1];
    ge.Region = p[2];
    ge.ElementaryRegion = p[3];
    ge.Partition = p[4];
    ge.NbrNodes = p[5];
    ge.NumNodes = (int *)Malloc(ge.NbrNodes * sizeof(int));
    memcpy(ge.NumNodes, p + 6, ge.NbrNodes * sizeof(int));
    ge.NbrEdges = ge.NbrFacets = 0;
    ge.NumEdges = ge.NumFacets = NULL;
    List_Add(GeoData_P->Elements, &ge);
    p += 6 + ge.NbrNodes;
  }
  GeoData_P->IsPart = true;
}

// A mesh that Gmsh has partitioned in one file for each partition, with one
// layer of overlap (gmsh -part n -part_split -overlap 1, which writes
// name_1.msh, ..., name_n.msh for name.msh), is read without the first rank:
// each rank reads the file of its partition, which has its elements and those
// of the other partitions that share a node with them, i.e. the part of the
// mesh that Geo_SendParts gives it.
//
// These files are used if there is one for each rank, unless the problem needs
// more: the whole mesh on the first rank (see Geo_SetWholeMeshOnFirstRank),
// which these files do not give it, or the regions of the Link constraints on
// all the ranks.
static std::string Geo_NameOfPartFile(const char *Name, int part)
{
  std::string base(Name);
  std::size_t dot = base.rfind('.');
  std::string ext = (dot == std::string::npos) ? "" : base.substr(dot);
  if(dot != std::string::npos) base.resize(dot);
  return base + "_" + std::to_string(part) + ext;
}

static bool Geo_FileExists(const std::string &name)
{
  FILE *fp = FOpen(name.c_str(), "rb");
  if(fp) fclose(fp);
  return fp != NULL;
}

// Whether the first rank reads the whole mesh while the others read their part:
// to build the trees in the pre-processing (see Generate_TreesOnFirstRank)
static bool Geo_FirstRankReadsWholeMesh()
{
  return WholeMeshOnFirstRank == 1 && Flag_PRE;
}

// Whether the problem lets the ranks read their part of the mesh, the first
// rank possibly reading the whole mesh if canReadWhole (on the first rank,
// which tells it to the user if not)
static bool Geo_CanReadPart(const char *what, bool canReadWhole)
{
  std::unordered_set<int> linkRegions;
  Geo_GetLinkRegions(linkRegions);
  const char *need =
    (WholeMeshOnFirstRank == 2) ? "the whole mesh on the first rank" :
    (Geo_FirstRankReadsWholeMesh() && !canReadWhole) ?
                                  "a tree, built on the whole mesh" :
    !linkRegions.empty()        ? "a Link constraint" :
                                  NULL;
  if(need)
    Message::Info("The %s %s not used (the problem has %s)", what,
                  strstr(what, "files") ? "are" : "is", need);
  return !need;
}

static bool Geo_HasPartFiles(const char *Name)
{
  int use = 0;
  if(!Message::GetCommRank()) {
    int np = Message::GetCommSize();
    if(Geo_FileExists(Geo_NameOfPartFile(Name, np)) &&
       !Geo_FileExists(Geo_NameOfPartFile(Name, np + 1)))
      use = Geo_CanReadPart("mesh files of the partitions", false);
  }
  MPI_Bcast(&use, 1, MPI_INT, 0, MPI_COMM_WORLD);
  return use;
}

// Completes the part of the mesh that a rank has read, with its elements (of
// its partition) and those of the overlap (of other partitions), or the whole
// mesh that the first rank has read (whole), in which it finds its part
static void Geo_EndReadPart(struct GeoData *GeoData_P, const char *Name,
                            const char *how, bool whole)
{
  const int np = Message::GetCommSize(), me = Message::GetCommRank();
  long long count[2] = {0, 0};
  for(int e = 0; e < List_Nbr(GeoData_P->Elements); e++)
    count[Geo_RankOfPartition(((struct Geo_Element *)List_Pointer(
                                 GeoData_P->Elements, e))
                                ->Partition) == me ? 0 : 1]++;
  long long mine = count[0];
  MPI_Allreduce(MPI_IN_PLACE, count, 2, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
  if(!mine || !count[1])
    Message::Error("'%s' is not the mesh of partition %d out of %d with one "
                   "layer of overlap (%s)", Name, me + 1, np, how);
  Message::Info("%lld elements in the %d partitions", count[0], np);

  // the box and the dimension of the whole mesh
  double low[3] = {GeoData_P->Xmin, GeoData_P->Ymin, GeoData_P->Zmin};
  double high[3] = {GeoData_P->Xmax, GeoData_P->Ymax, GeoData_P->Zmax};
  if(!List_Nbr(GeoData_P->Nodes))
    for(int i = 0; i < 3; i++) {
      low[i] = 1e200;
      high[i] = -1e200;
    }
  MPI_Allreduce(MPI_IN_PLACE, low, 3, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
  MPI_Allreduce(MPI_IN_PLACE, high, 3, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
  MPI_Allreduce(MPI_IN_PLACE, &GeoData_P->Dimension, 1, MPI_DOUBLE, MPI_MAX,
                MPI_COMM_WORLD);
  GeoData_P->Xmin = low[0];
  GeoData_P->Ymin = low[1];
  GeoData_P->Zmin = low[2];
  GeoData_P->Xmax = high[0];
  GeoData_P->Ymax = high[1];
  GeoData_P->Zmax = high[2];
  GeoData_P->CharacteristicLength =
    sqrt(SQU(high[0] - low[0]) + SQU(high[1] - low[1]) + SQU(high[2] - low[2]));
  if(!GeoData_P->CharacteristicLength) GeoData_P->CharacteristicLength = 1.;
  GeoData_P->NbrPartitions = np;
  if(whole)
    Geo_SetFirstRankPart(GeoData_P, Geo_ElementsOfRanks(GeoData_P, 1)[0]);
  else
    GeoData_P->IsPart = true;
}

static void Geo_ReadPartFile(struct GeoData *GeoData_P, const char *Name)
{
  std::string partName = Geo_NameOfPartFile(Name, Message::GetCommRank() + 1);
  int np = Message::GetCommSize();
  std::string how = "gmsh -part " + std::to_string(np) +
                    " -part_split -overlap 1";
  ReadsPart = true;
  Geo_OpenFile((char *)partName.c_str(), "rb");
  Geo_ReadFile(GeoData_P);
  Geo_CloseFile();
  ReadsPart = false;
  Geo_EndReadPart(GeoData_P, partName.c_str(), how.c_str(), false);
}

#if defined(HAVE_GMSH)
// A mesh that Gmsh has partitioned in a single MSH 4 file with ghost cells
// (gmsh -part n -part_ghosts), with one partition for each rank, is read in
// parallel: each rank reads its partition and the elements of the other
// partitions that share a node with it (gmsh::model::mesh::merge), i.e. the
// part of the mesh that Geo_SendParts gives it. If the problem has trees, the
// first rank reads the whole mesh in the pre-processing to build them, and
// only keeps its part after that.

// Whether the file is a partitioned MSH 4 file (whose $PartitionedEntities
// section follows $Entities)
static bool Geo_IsPartitionedMSH4(const char *Name)
{
  FILE *fp = FOpen(Name, "rb");
  if(!fp) return false;
  char line[256];
  bool partitioned = false;
  while(fgets(line, sizeof(line), fp)) {
    double version;
    if(!strncmp(line, "$MeshFormat", 11)) {
      if(!fgets(line, sizeof(line), fp) || sscanf(line, "%lf", &version) != 1 ||
         version < 4)
        break;
    }
    else if(!strncmp(line, "$PartitionedEntities", 20)) {
      partitioned = true;
      break;
    }
    else if(!strncmp(line, "$Nodes", 6) || !strncmp(line, "$Elements", 9))
      break;
  }
  fclose(fp);
  return partitioned;
}

// Reads the partition of the rank with its ghost cells in the current Gmsh
// model; returns the number of partitions of the file, or 0 without ghost cells
static int Geo_MergePartition(const char *Name)
{
  int part = Message::GetCommRank() + 1;
  gmsh::clear();
  gmsh::model::mesh::merge(Name, {part}, true);
  std::vector<std::size_t> ghosts;
  std::vector<int> partitions;
  gmsh::model::mesh::getGhostElements(part, ghosts, partitions);
  return ghosts.empty() ? 0 : gmsh::model::getNumberOfPartitions();
}

static bool Geo_ReadPartition(struct GeoData *GeoData_P, char *Name)
{
  const int np = Message::GetCommSize();
  const bool whole = !Message::GetCommRank() && Geo_FirstRankReadsWholeMesh();
  int use = 0;
  if(!Message::GetCommRank() && !Flag_CALLED_WITH_ONELAB_SERVER &&
     Geo_IsPartitionedMSH4(Name) &&
     Geo_CanReadPart("partitions of the mesh file", true)) {
    // the first rank tries first, so that the others do not read the whole
    // mesh if the file has no ghost cells
    use = (Geo_MergePartition(Name) == np);
    if(!use) {
      Message::Info("The partitions of the mesh file are not used (they are "
                    "not %d with ghost cells: gmsh -part %d -part_ghosts)", np,
                    np);
      gmsh::clear();
    }
  }
  MPI_Bcast(&use, 1, MPI_INT, 0, MPI_COMM_WORLD);
  if(!use) return false;
  if(whole) {
    Message::Info("The first rank reads the whole mesh to build the trees");
    gmsh::clear();
    gmsh::merge(Name);
  }
  else if(Message::GetCommRank())
    Geo_MergePartition(Name);
  name = Name;
  ReadsPart = true;
  Geo_ReadFileWithGmsh(GeoData_P, true);
  ReadsPart = false;
  gmsh::clear();
  std::string how = "gmsh -part " + std::to_string(np) + " -part_ghosts";
  Geo_EndReadPart(GeoData_P, Name, how.c_str(), whole);
  return true;
}
#endif
#endif

int Geo_AddGeoData(List_T *GeoData_L, char *Name_MshFile,
                   char *Name_DefaultMshFile, char *Name_AdaptFile,
                   char *Name_DefaultAdaptFile)
{
  struct GeoData GeoData_S;
  int i;

  if(!Name_MshFile) Name_MshFile = Name_DefaultMshFile;

  if((i = List_ISearchSeq(GeoData_L, Name_MshFile, fcmp_GeoData_Name)) < 0) {
    Message::Info("Loading Geometric data '%s'", Name_MshFile);
    i = List_Nbr(GeoData_L);
    Geo_InitGeoData(&GeoData_S, i, Name_MshFile);
    if(Message::GetInfoCpu()) Message::MarkOperationCpu("ReadMeshFile");
    bool distribute = Geo_CanDistributeMesh(
      Name_AdaptFile ? Name_AdaptFile : Name_DefaultAdaptFile);
    // the messages of the first rank (with -cpu they ask all the ranks for
    // their time and memory) are then not reached by the others
    MessageLocalCpu localCpu(distribute);
    bool partFiles = false;
#if defined(HAVE_MPI)
    if(distribute && (partFiles = Geo_HasPartFiles(Name_MshFile)))
      Geo_ReadPartFile(&GeoData_S, Name_MshFile);
#if defined(HAVE_GMSH)
    else if(distribute)
      partFiles = Geo_ReadPartition(&GeoData_S, Name_MshFile);
#endif
#endif
    if(!partFiles && (!distribute || !Message::GetCommRank())) {
      Geo_OpenFile(Name_MshFile, "rb");
      Geo_ReadFile(&GeoData_S);
      Geo_CloseFile();
      Geo_SetPartitions(&GeoData_S);
    }
#if defined(HAVE_MPI)
    if(distribute && !partFiles) {
      if(!Message::GetCommRank()) {
        Message::Info("Sending their part of the mesh to the other ranks");
        Geo_SendParts(&GeoData_S);
      }
      else
        Geo_ReceivePart(&GeoData_S);
    }
#endif
    if(Message::GetInfoCpu()) Message::MarkOperationCpu("");

    if(!Name_AdaptFile) Name_AdaptFile = Name_DefaultAdaptFile;
    if(Name_AdaptFile) {
      Message::Info("Loading Adaptation data '%s'", Name_AdaptFile);
      Geo_OpenFile(Name_AdaptFile, "r");
      Geo_SetCurrentGeoData(&GeoData_S);
      Geo_ReadFileAdapt(&GeoData_S);
      Geo_CloseFile();
    }
    List_Add(GeoData_L, &GeoData_S);
  }

  return (i);
}

int fcmp_GeoData_Name(const void *a, const void *b)
{
  return (strcmp((char *)a, ((struct GeoData *)b)->Name));
}

/* ------------------------------------------------------------------------ */
/*  G e o _ I n i t G e o D a t a                                           */
/* ------------------------------------------------------------------------ */

void Geo_InitGeoData(struct GeoData *GeoData_P, int Num, char *Name)
{
  GeoData_P->Num = Num;
  GeoData_P->Name = Name;

  GeoData_P->Nodes = NULL;
  GeoData_P->Elements = NULL;

  GeoData_P->NbrElementsWithEdges = GeoData_P->NbrElementsWithFacets = 0;
  GeoData_P->NumCurrentEdge = GeoData_P->NumCurrentFacet = 0;
  GeoData_P->EdgesXNodes = new GeoEntityTable;
  GeoData_P->FacetsXEdges = new GeoEntityTable;

  GeoData_P->NodesXElements = NULL;
  GeoData_P->Normals = Tree_Create(sizeof(struct EntityXVector), fcmp_EXVector);

  GeoData_P->GroupForPRE = NULL;

  GeoData_P->ElementSearch = NULL;

  GeoData_P->H = GeoData_P->P = NULL;

  GeoData_P->PeriodicNodes = NULL;

  GeoData_P->HasMovingBand = false;

  GeoData_P->NodeIndex = new Geo_NumberIndex;
  GeoData_P->ElementIndex = new Geo_NumberIndex;

  GeoData_P->HasParts = GeoData_P->IsPart = false;
  GeoData_P->FirstRankPart = NULL;
  GeoData_P->EntitiesLevel = 0;
}

/* ------------------------------------------------------------------------ */
/*  G e o _ F r e e G e o D a t a                                           */
/* ------------------------------------------------------------------------ */

void Geo_FreeGeoData(struct GeoData *GeoData_P)
{
  Message::Debug("Freeing GeoData %d", GeoData_P->Num);
  List_Delete(GeoData_P->Nodes);
  if(GeoData_P->Elements) {
    for(int i = 0; i < List_Nbr(GeoData_P->Elements); i++) {
      Geo_Element *e = (Geo_Element *)List_Pointer(GeoData_P->Elements, i);
      Free(e->NumNodes);
      Free(e->NumEdges);
      Free(e->NumFacets);
    }
    List_Delete(GeoData_P->Elements);
  }
  delete GeoData_P->EdgesXNodes;
  delete GeoData_P->FacetsXEdges;
  delete GeoData_P->NodeIndex;
  delete GeoData_P->ElementIndex;
  delete GeoData_P->FirstRankPart;
  if(GeoData_P->NodesXElements) {
    Tree_Action(GeoData_P->NodesXElements, free_E2XE1);
    Tree_Delete(GeoData_P->NodesXElements);
  }
  Tree_Delete(GeoData_P->Normals);
  List_Delete(GeoData_P->GroupForPRE);
  Free_ElementSearch(GeoData_P);
  Free(GeoData_P->H);
  Free(GeoData_P->P);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ S e t C u r r e n t G e o D a t a B a s e                       */
/* ------------------------------------------------------------------------ */

void Geo_SetCurrentGeoData(struct GeoData *GeoData_P)
{
  CurrentGeoData = GeoData_P;
}

/* ------------------------------------------------------------------------ */
/*  G e o _ O p e n F i l e                                                 */
/* ------------------------------------------------------------------------ */

void Geo_OpenFile(char *Name, const char *Mode)
{
  File_GEO = FOpen(Name, Mode);
  name = Name;

  if(!File_GEO) Message::Error("Unable to open file '%s'", Name);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ C l o s e F i l e                                               */
/* ------------------------------------------------------------------------ */

void Geo_CloseFile(void) { fclose(File_GEO); }

/* ------------------------------------------------------------------------ */
/*  G e o _ R e a d F i l e                                                 */
/* ------------------------------------------------------------------------ */

int Geo_GetNbNodesPerElement(int Type)
{
  switch(Type) {
  case POINT_ELEMENT: return 1;

  case LINE: return 2;
  case TRIANGLE: return 3;
  case QUADRANGLE: return 4;
  case TETRAHEDRON: return 4;
  case HEXAHEDRON: return 8;
  case PRISM: return 6;
  case PYRAMID: return 5;

  case LINE_2: return 3;
  case TRIANGLE_2: return 6;
  case QUADRANGLE_2: return 9;
  case TETRAHEDRON_2: return 10;
  case HEXAHEDRON_2: return 27;
  case PRISM_2: return 18;
  case PYRAMID_2: return 13;
  case QUADRANGLE_2_8N: return 8;
  case HEXAHEDRON_2_20N: return 20;
  case PRISM_2_15N: return 15;
  case PYRAMID_2_13N: return 13;

  case LINE_3: return 4;
  case TRIANGLE_3: return 10;
  case QUADRANGLE_3: return 16;
  case TETRAHEDRON_3: return 20;
  case HEXAHEDRON_3: return 64;
  case PRISM_3: return 40;
  case PYRAMID_3: return 30;

  case LINE_4: return 5;
  case TRIANGLE_4: return 15;
  case QUADRANGLE_4: return 25;
  case TETRAHEDRON_4: return 35;
  case HEXAHEDRON_4: return 125;
  case PRISM_4:
    return 75;
    // case PYRAMID_4     : return 55;

  default: Message::Error("Unknown type of Element"); return -1;
  }
}

int Geo_GetDimOfElement(int Type)
{
  switch(Type) {
  case POINT_ELEMENT: return 0;

  case LINE: return 1;
  case TRIANGLE: return 2;
  case QUADRANGLE: return 2;
  case TETRAHEDRON: return 3;
  case HEXAHEDRON: return 3;
  case PRISM: return 3;
  case PYRAMID: return 3;

  case LINE_2: return 1;
  case TRIANGLE_2: return 2;
  case QUADRANGLE_2: return 2;
  case TETRAHEDRON_2: return 3;
  case HEXAHEDRON_2: return 3;
  case PRISM_2: return 3;
  case PYRAMID_2: return 3;
  case QUADRANGLE_2_8N: return 2;
  case HEXAHEDRON_2_20N: return 3;
  case PRISM_2_15N: return 3;
  case PYRAMID_2_13N: return 3;

  case LINE_3: return 1;
  case TRIANGLE_3: return 2;
  case QUADRANGLE_3: return 2;
  case TETRAHEDRON_3: return 3;
  case HEXAHEDRON_3: return 3;
  case PRISM_3: return 3;
  case PYRAMID_3: return 3;

  case LINE_4: return 1;
  case TRIANGLE_4: return 2;
  case QUADRANGLE_4: return 2;
  case TETRAHEDRON_4: return 3;
  case HEXAHEDRON_4: return 3;
  case PRISM_4: return 3;
  // case PYRAMID_4     : return 3;
  default: Message::Error("Unknown type of Element"); return -1;
  }
}

void Geo_ReverseElement(Geo_Element *Geo_Element)
{
  int tmp;
  switch(Geo_Element->Type) {
  case POINT_ELEMENT: break;
  case LINE:
  case LINE_2:
    tmp = Geo_Element->NumNodes[0];
    Geo_Element->NumNodes[0] = Geo_Element->NumNodes[1];
    Geo_Element->NumNodes[1] = tmp;
    break;
  case TRIANGLE:
    tmp = Geo_Element->NumNodes[1];
    Geo_Element->NumNodes[1] = Geo_Element->NumNodes[2];
    Geo_Element->NumNodes[2] = tmp;
    break;
  case TRIANGLE_2:
    tmp = Geo_Element->NumNodes[1];
    Geo_Element->NumNodes[1] = Geo_Element->NumNodes[2];
    Geo_Element->NumNodes[2] = tmp;
    tmp = Geo_Element->NumNodes[3 + 0];
    Geo_Element->NumNodes[3] = Geo_Element->NumNodes[5];
    Geo_Element->NumNodes[5] = tmp;
    break;
  case QUADRANGLE:
    tmp = Geo_Element->NumNodes[1];
    Geo_Element->NumNodes[1] = Geo_Element->NumNodes[3];
    Geo_Element->NumNodes[3] = tmp;
    break;
  case QUADRANGLE_2:
  case QUADRANGLE_2_8N:
    tmp = Geo_Element->NumNodes[1];
    Geo_Element->NumNodes[1] = Geo_Element->NumNodes[3];
    Geo_Element->NumNodes[3] = tmp;
    tmp = Geo_Element->NumNodes[4 + 0];
    Geo_Element->NumNodes[4] = Geo_Element->NumNodes[7];
    Geo_Element->NumNodes[7] = tmp;
    tmp = Geo_Element->NumNodes[5];
    Geo_Element->NumNodes[5] = Geo_Element->NumNodes[6];
    Geo_Element->NumNodes[6] = tmp;
    break;
  default:
    Message::Error("Unknown type of element (%d) to reverse",
                   Geo_Element->Type);
    break;
  }
}

void Geo_SaveMesh(struct GeoData *GeoData_P, List_T *InitialList,
                  char *FileName)
{
  FILE *file;
  struct Geo_Node Geo_Node;
  struct Geo_Node *Geo_Node_P;
  struct Geo_Element Geo_Element;
  struct GeoData GeoData;
  int fcmp_int(const void *a, const void *b);

  GeoData.Nodes = List_Create(1000, 1000, sizeof(struct Geo_Node));
  GeoData.Elements = List_Create(1000, 1000, sizeof(struct Geo_Element));

  int maxdim = 0;
  for(int i = 0; i < List_Nbr(GeoData_P->Elements); i++) {
    List_Read(GeoData_P->Elements, i, &Geo_Element);
    maxdim = std::max(maxdim, Geo_GetDimOfElement(Geo_Element.Type));
    if(List_Search(InitialList, &Geo_Element.Region, fcmp_int)) {
      List_Add(GeoData.Elements, &Geo_Element);
      for(int j = 0; j < Geo_Element.NbrNodes; j++)
        if(!List_Search(GeoData.Nodes,
                        Geo_Node_P =
                          Geo_GetGeoNodeOfNum(Geo_Element.NumNodes[j]),
                        fcmp_Nod))
          List_Add(GeoData.Nodes, Geo_Node_P);
    }
  }

  file = FOpen(FileName, "w");
  if(!file) {
    Message::Error("Could not open file '%s'", FileName);
    return;
  }
  Message::Info("Saving mesh in file \"%s\" (%d nodes, %d elements)", FileName,
                List_Nbr(GeoData.Nodes), List_Nbr(GeoData.Elements));
  fprintf(file, "$NOD\n%d\n", List_Nbr(GeoData.Nodes));
  for(int i = 0; i < List_Nbr(GeoData.Nodes); i++) {
    List_Read(GeoData.Nodes, i, &Geo_Node);
    fprintf(file, "%d %.16g %.16g %.16g\n", Geo_Node.Num, Geo_Node.x,
            Geo_Node.y, Geo_Node.z);
  }
  fprintf(file, "$ENDNOD\n$ELM\n%d\n", List_Nbr(GeoData.Elements));
  for(int i = 0; i < List_Nbr(GeoData.Elements); i++) {
    List_Read(GeoData.Elements, i, &Geo_Element);
    int Type = GetDP2Gmsh(Geo_Element.Type);
    fprintf(file, "%d %d %d %d %d ", Geo_Element.Num, Type, Geo_Element.Region,
            Geo_Element.Region, Geo_Element.NbrNodes);
    for(int j = 0; j < Geo_Element.NbrNodes; j++)
      fprintf(file, "%d ", Geo_Element.NumNodes[j]);
    fprintf(file, "\n");
  }
  fprintf(file, "$ENDELM\n");
  fclose(file);

  List_Delete(GeoData.Nodes);
  List_Delete(GeoData.Elements);
}

static std::string ExtractDoubleQuotedString(const char *str, int len)
{
  char *c = strstr((char *)str, "\"");
  if(!c) return "";
  std::string ret;
  for(int i = 1; i < len; i++) {
    if(c[i] == '"' || c[i] == EOF || c[i] == '\n' || c[i] == '\r') break;
    ret.push_back(c[i]);
  }
  return ret;
}

static void Geo_SnapNodes(struct GeoData *GeoData_P)
{
  // 2D meshes used for axisymmetric calculations should have the minimum
  // x-coordinate *exactly* equal to 0: snap x coordinate of all vertices of 2D
  // meshes with fabs(x) < 1e-13 to 0.
  if(GeoData_P->Dimension != DIM_2D || fabs(GeoData_P->Xmin) > 1e-13) return;
  int snaps = 0;
  for(int i = 0; i < List_Nbr(GeoData_P->Nodes); i++) {
    Geo_Node *n = (Geo_Node *)List_Pointer(GeoData_P->Nodes, i);
    if(fabs(n->x) < 1e-13) {
      n->x = 0.;
      snaps++;
    }
  }
  if(snaps)
    Message::Info(3, "Snapped x coordinate of %d node%s to 0", snaps,
                  (snaps > 1) ? "s" : "");
}

static void Geo_ReadFileWithGmsh(struct GeoData *GeoData_P, bool loaded)
{
#if defined(HAVE_GMSH)
  // within the program that runs the ONELAB server (the mobile and web apps),
  // read the mesh in a model of its own, and leave the one Gmsh shows alone,
  // made current again also when an error throws
  struct restoreModel {
    GModel *shown = nullptr;
    ~restoreModel()
    {
      if(!shown) return;
      delete GModel::current();
      GModel::setCurrent(shown);
    }
  } restore;
  if(loaded) {} // the part of the mesh of the rank (Geo_ReadPartition)
  else if(Flag_CALLED_WITH_ONELAB_SERVER) {
    restore.shown = GModel::current();
    GModel::setCurrent(new GModel());
    gmsh::merge(name);
  }
  else
    gmsh::open(name);

  /* N O D E S */

  struct Geo_Node Geo_Node;

  std::vector<std::size_t> nodeTags;
  std::vector<double> coord;
  std::vector<double> parametricCoord;
  gmsh::model::mesh::getNodes(nodeTags, coord, parametricCoord, -1, -1);

  if(GeoData_P->Nodes == NULL)
    GeoData_P->Nodes =
      List_Create(nodeTags.size(), 1000, sizeof(struct Geo_Node));

  for(unsigned int i = 0; i < nodeTags.size(); i++) {
    Geo_Node.Num = nodeTags[i];
    Geo_Node.x = coord[3 * i + 0];
    Geo_Node.y = coord[3 * i + 1];
    Geo_Node.z = coord[3 * i + 2];
    if(Flag_MSH_SCALING != 1.0) {
      Geo_Node.x *= Flag_MSH_SCALING;
      Geo_Node.y *= Flag_MSH_SCALING;
      Geo_Node.z *= Flag_MSH_SCALING;
    }
    List_Add(GeoData_P->Nodes, &Geo_Node);

    if(!i) {
      GeoData_P->Xmin = GeoData_P->Xmax = Geo_Node.x;
      GeoData_P->Ymin = GeoData_P->Ymax = Geo_Node.y;
      GeoData_P->Zmin = GeoData_P->Zmax = Geo_Node.z;
    }
    else {
      GeoData_P->Xmin = std::min(GeoData_P->Xmin, Geo_Node.x);
      GeoData_P->Xmax = std::max(GeoData_P->Xmax, Geo_Node.x);
      GeoData_P->Ymin = std::min(GeoData_P->Ymin, Geo_Node.y);
      GeoData_P->Ymax = std::max(GeoData_P->Ymax, Geo_Node.y);
      GeoData_P->Zmin = std::min(GeoData_P->Zmin, Geo_Node.z);
      GeoData_P->Zmax = std::max(GeoData_P->Zmax, Geo_Node.z);
    }
  }

  if(GeoData_P->Xmin != GeoData_P->Xmax && GeoData_P->Ymin != GeoData_P->Ymax &&
     GeoData_P->Zmin != GeoData_P->Zmax)
    GeoData_P->Dimension = DIM_3D;
  else if(GeoData_P->Xmin != GeoData_P->Xmax &&
          GeoData_P->Ymin != GeoData_P->Ymax)
    GeoData_P->Dimension = DIM_2D;
  else if(GeoData_P->Xmin != GeoData_P->Xmax &&
          GeoData_P->Zmin != GeoData_P->Zmax)
    GeoData_P->Dimension = DIM_2D;
  else if(GeoData_P->Ymin != GeoData_P->Ymax &&
          GeoData_P->Zmin != GeoData_P->Zmax)
    GeoData_P->Dimension = DIM_2D;
  else if(GeoData_P->Xmin != GeoData_P->Xmax)
    GeoData_P->Dimension = DIM_1D;
  else if(GeoData_P->Ymin != GeoData_P->Ymax)
    GeoData_P->Dimension = DIM_1D;
  else if(GeoData_P->Zmin != GeoData_P->Zmax)
    GeoData_P->Dimension = DIM_1D;
  else
    GeoData_P->Dimension = DIM_0D;

  GeoData_P->CharacteristicLength =
    sqrt(SQU(GeoData_P->Xmax - GeoData_P->Xmin) +
         SQU(GeoData_P->Ymax - GeoData_P->Ymin) +
         SQU(GeoData_P->Zmax - GeoData_P->Zmin));
  if(!GeoData_P->CharacteristicLength) GeoData_P->CharacteristicLength = 1.;

  coord.clear();
  parametricCoord.clear();

  /*  E L E M E N T S  */

  struct Geo_Element Geo_Element;

  std::vector<int> elementTypes;
  std::vector<std::vector<std::size_t> > elementTags;
  std::vector<std::vector<std::size_t> > elementNodeTags;
  gmsh::model::mesh::getElements(elementTypes, elementTags, elementNodeTags, -1,
                                 -1);

  std::size_t nbr = 0, maxTag = 0;
  for(unsigned int i = 0; i < elementTypes.size(); i++) {
    nbr += elementTags[i].size();
    for(unsigned int j = 0; j < elementTags[i].size(); j++)
      maxTag = std::max(maxTag, elementTags[i][j]);
  }

#if defined(HAVE_MPI)
  if(ReadsPart) {
    long long max = maxTag;
    MPI_Allreduce(MPI_IN_PLACE, &max, 1, MPI_LONG_LONG, MPI_MAX, MPI_COMM_WORLD);
    maxTag = max;
  }
#endif

  if(GeoData_P->Elements == NULL)
    GeoData_P->Elements = List_Create(nbr, 1000, sizeof(struct Geo_Element));

  Geo_Element.NbrEdges = Geo_Element.NbrFacets = 0;
  Geo_Element.NumEdges = Geo_Element.NumFacets = NULL;

  GeoData_P->NbrPartitions = gmsh::model::getNumberOfPartitions();

  gmsh::vectorpair dimTags;
  gmsh::model::getEntities(dimTags, -1);
  for(unsigned int entity = 0; entity < dimTags.size(); entity++) {
    gmsh::model::mesh::getElements(elementTypes, elementTags, elementNodeTags,
                                   dimTags[entity].first,
                                   dimTags[entity].second);
    std::vector<int> physicalsTags;
    gmsh::model::getPhysicalGroupsForEntity(
      dimTags[entity].first, dimTags[entity].second, physicalsTags);

    std::vector<int> partitions;
    int elementary = dimTags[entity].second;
    if(GeoData_P->NbrPartitions > 1) {
      gmsh::model::getPartitions(dimTags[entity].first, dimTags[entity].second,
                                 partitions);
      // the elementary region of a partition entity is its parent entity
      int parentDim, parentTag;
      gmsh::model::getParent(dimTags[entity].first, dimTags[entity].second,
                             parentDim, parentTag);
      if(parentDim == dimTags[entity].first && parentTag > 0)
        elementary = parentTag;
    }

    for(unsigned int phys = 0; phys < physicalsTags.size(); phys++) {
      for(unsigned int i = 0; i < elementTypes.size(); i++) {
        Geo_Element.Type = Gmsh2GetDP(elementTypes[i]);
        Geo_Element.NbrNodes =
          elementNodeTags[i].size() / elementTags[i].size();
        for(unsigned int j = 0; j < elementTags[i].size(); j++) {
          // if more than one physical group, create new elements (with new
          // tags) for all additional groups - this is consistent with the
          // behavior of the old MSH2 file format; their tags only depend on
          // the element, also when each rank reads its part of the mesh
          Geo_Element.Num = elementTags[i][j] + phys * maxTag;
          Geo_Element.Region = physicalsTags[phys];
          Geo_Element.ElementaryRegion = elementary;
          Geo_Element.Partition = partitions.empty() ? 0 : partitions[0];
          Geo_Element.NumNodes =
            (int *)Malloc(Geo_Element.NbrNodes * sizeof(int));
          for(int k = 0; k < Geo_Element.NbrNodes; k++)
            Geo_Element.NumNodes[k] =
              elementNodeTags[i][Geo_Element.NbrNodes * j + k];
          if(Geo_Element.Region < 0) {
            Geo_ReverseElement(&Geo_Element);
            Geo_Element.Region = -Geo_Element.Region;
          }
          List_Add(GeoData_P->Elements, &Geo_Element);
        }
      }
    }
  }

  List_Sort(GeoData_P->Elements, fcmp_Elm);

  Geo_SnapNodes(GeoData_P);

#else
  Message::Error("You need to compile GetDP with Gmsh support to open '%s'",
                 name);
#endif
}

void Geo_ReadFile(struct GeoData *GeoData_P)
{
  int Nbr, i, j, Type, iDummy, Format, Size, NbTags;
  double Version = 1.0;
  struct Geo_Node Geo_Node;
  struct Geo_Element Geo_Element;
  char String[256] = "";
  int binary = 0, swap = 0;
  std::map<int, std::vector<int> > entities[4];

  while(1) {
    do {
      if(!fgets(String, sizeof(String), File_GEO) || feof(File_GEO)) break;
    } while(String[0] != '$');

    if(feof(File_GEO)) break;

    /*  F O R M A T  */

    if(!strncmp(&String[1], "MeshFormat", 10)) {
      if(!fgets(String, sizeof(String), File_GEO)) return;
      if(sscanf(String, "%lf %d %d", &Version, &Format, &Size) != 3) return;
      if(Version < 2.0) {
        Message::Error("Unsupported or unknown mesh file version (%g)",
                       Version);
        return;
      }
      else if(Version > 3.1) {
        Geo_ReadFileWithGmsh(GeoData_P);
        return;
      }

      if(Format) {
        binary = 1;
        Message::Info("Mesh is in binary format");
        int one;
        if(fread(&one, sizeof(int), 1, File_GEO) != 1) return;
        if(one != 1) {
          swap = 1;
          Message::Info("Swapping bytes from binary file");
        }
      }
    }

    /* P H Y S I C A L   N A M E S */

    else if(!strncmp(&String[1], "PhysicalNames", 13)) {
      // GetDP does not currently use the name information

      if(!fgets(String, sizeof(String), File_GEO)) return;
      int numNames;
      if(sscanf(String, "%d", &numNames) != 1) return;
      for(int i = 0; i < numNames; i++) {
        int dim = -1, num;
        if(Version > 2.0) {
          if(fscanf(File_GEO, "%d", &dim) != 1) return;
        }
        if(fscanf(File_GEO, "%d", &num) != 1) return;
        if(!fgets(String, sizeof(String), File_GEO)) return;
        std::string name = ExtractDoubleQuotedString(String, 256);
        Message::Debug("Physical group %d (dim %d) has name %s", num, dim,
                       name.c_str());
      }
    }

    /* E N T I T I E S */

    else if(!strncmp(&String[1], "Entities", 8)) {
      if(!fgets(String, sizeof(String), File_GEO)) return;
      int num[4];
      if(sscanf(String, "%d %d %d %d", &num[0], &num[1], &num[2], &num[3]) != 4)
        return;
      for(int dim = 0; dim < 4; dim++) {
        for(int j = 0; j < num[dim]; j++) {
          int num;
          if(fscanf(File_GEO, "%d", &num) != 1) return;
          int nbound = 0;
          if(dim > 0) {
            if(fscanf(File_GEO, "%d", &nbound) != 1) return;
            for(int k = 0; k < nbound; k++) {
              int dummy;
              if(fscanf(File_GEO, "%d", &dummy) != 1) return;
            }
          }
          int nphys;
          if(fscanf(File_GEO, "%d", &nphys) != 1) return;
          std::vector<int> physicals(nphys);
          for(int k = 0; k < nphys; k++) {
            if(fscanf(File_GEO, "%d", &physicals[k]) != 1) return;
          }
          entities[dim][num] = physicals;
          if(nphys > 1) {
            Message::Error(
              "GetDP does not support multiple physical groups per element:"
              " elementary entity %d belongs to %d physical groups",
              num, nphys);
            return;
          }
        }
      }
    }

    /*  N O D E S  */

    else if(!strncmp(&String[1], "NOE", 3) || !strncmp(&String[1], "NOD", 3) ||
            !strncmp(&String[1], "Nodes", 5) ||
            !strncmp(&String[1], "ParametricNodes", 15)) {
      bool parametric =
        !strncmp(&String[1], "ParametricNodes", 15) || (Version >= 3.0);

      if(!fgets(String, sizeof(String), File_GEO)) return;
      if(sscanf(String, "%d", &Nbr) != 1) return;
      Message::Debug("%d nodes", Nbr);

      if(GeoData_P->Nodes == NULL)
        GeoData_P->Nodes = List_Create(Nbr, 1000, sizeof(struct Geo_Node));

      for(i = 0; i < Nbr; i++) {
        if(!parametric) {
          if(!binary) {
            if(fscanf(File_GEO, "%d %lf %lf %lf", &Geo_Node.Num, &Geo_Node.x,
                      &Geo_Node.y, &Geo_Node.z) != 4)
              return;
            if(Flag_MSH_SCALING != 1.0) {
              Geo_Node.x *= Flag_MSH_SCALING;
              Geo_Node.y *= Flag_MSH_SCALING;
              Geo_Node.z *= Flag_MSH_SCALING;
            }
          }
          else {
            if(fread(&Geo_Node.Num, sizeof(int), 1, File_GEO) != 1) return;
            if(swap) swapBytes((char *)&Geo_Node.Num, sizeof(int), 1);
            double xyz[3];
            if(fread(xyz, sizeof(double), 3, File_GEO) != 3) return;
            if(swap) swapBytes((char *)xyz, sizeof(double), 3);
            Geo_Node.x = xyz[0];
            Geo_Node.y = xyz[1];
            Geo_Node.z = xyz[2];
            if(Flag_MSH_SCALING != 1.0) {
              Geo_Node.x *= Flag_MSH_SCALING;
              Geo_Node.y *= Flag_MSH_SCALING;
              Geo_Node.z *= Flag_MSH_SCALING;
            }
          }
        }
        else {
          int dim = -1, entity;
          if(!binary) {
            if(Version < 3.0) {
              if(fscanf(File_GEO, "%d %lf %lf %lf %d %d", &Geo_Node.Num,
                        &Geo_Node.x, &Geo_Node.y, &Geo_Node.z, &dim,
                        &entity) != 6)
                return;
              if(Flag_MSH_SCALING != 1.0) {
                Geo_Node.x *= Flag_MSH_SCALING;
                Geo_Node.y *= Flag_MSH_SCALING;
                Geo_Node.z *= Flag_MSH_SCALING;
              }
            }
            else {
              if(fscanf(File_GEO, "%d %lf %lf %lf %d", &Geo_Node.Num,
                        &Geo_Node.x, &Geo_Node.y, &Geo_Node.z, &entity) != 5)
                return;
              if(Flag_MSH_SCALING != 1.0) {
                Geo_Node.x *= Flag_MSH_SCALING;
                Geo_Node.y *= Flag_MSH_SCALING;
                Geo_Node.z *= Flag_MSH_SCALING;
              }
              if(entity) {
                if(fscanf(File_GEO, "%d", &dim) != 1) return;
              }
            }
          }
          else {
            if(fread(&Geo_Node.Num, sizeof(int), 1, File_GEO) != 1) return;
            if(swap) swapBytes((char *)&Geo_Node.Num, sizeof(int), 1);
            double xyz[3];
            if(fread(xyz, sizeof(double), 3, File_GEO) != 3) return;
            if(swap) swapBytes((char *)xyz, sizeof(double), 3);
            if(Version < 3.0) {
              if(fread(&dim, sizeof(int), 1, File_GEO) != 1) return;
              if(swap) swapBytes((char *)&dim, sizeof(int), 1);
              if(fread(&entity, sizeof(int), 1, File_GEO) != 1) return;
              if(swap) swapBytes((char *)&entity, sizeof(int), 1);
            }
            else {
              if(fread(&entity, sizeof(int), 1, File_GEO) != 1) return;
              if(swap) swapBytes((char *)&entity, sizeof(int), 1);
              if(entity) {
                if(fread(&dim, sizeof(int), 1, File_GEO) != 1) return;
                if(swap) swapBytes((char *)&dim, sizeof(int), 1);
              }
            }
            Geo_Node.x = xyz[0];
            Geo_Node.y = xyz[1];
            Geo_Node.z = xyz[2];
            if(Flag_MSH_SCALING != 1.0) {
              Geo_Node.x *= Flag_MSH_SCALING;
              Geo_Node.y *= Flag_MSH_SCALING;
              Geo_Node.z *= Flag_MSH_SCALING;
            }
          }
          if(dim == 1 && (Version < 3.0 || entity)) {
            double u[1];
            if(!binary) {
              if(fscanf(File_GEO, "%lf", &u[0]) != 1) return;
            }
            else {
              if(fread(u, sizeof(double), 1, File_GEO) != 1) return;
              if(swap) swapBytes((char *)u, sizeof(double), 1);
            }
          }
          else if(dim == 2 && (Version < 3.0 || entity)) {
            double uv[2];
            if(!binary) {
              if(fscanf(File_GEO, "%lf %lf", &uv[0], &uv[1]) != 2) return;
            }
            else {
              if(fread(uv, sizeof(double), 2, File_GEO) != 2) return;
              if(swap) swapBytes((char *)uv, sizeof(double), 2);
            }
          }
          else if(dim == 3 && Version >= 3.0 && entity) {
            double uvw[3];
            if(!binary) {
              if(fscanf(File_GEO, "%lf %lf %lf", &uvw[0], &uvw[1], &uvw[2]) !=
                 3)
                return;
            }
            else {
              if(fread(uvw, sizeof(double), 3, File_GEO) != 2) return;
              if(swap) swapBytes((char *)uvw, sizeof(double), 3);
            }
          }
        }
        List_Add(GeoData_P->Nodes, &Geo_Node);
        if(!i) {
          GeoData_P->Xmin = GeoData_P->Xmax = Geo_Node.x;
          GeoData_P->Ymin = GeoData_P->Ymax = Geo_Node.y;
          GeoData_P->Zmin = GeoData_P->Zmax = Geo_Node.z;
        }
        else {
          GeoData_P->Xmin = std::min(GeoData_P->Xmin, Geo_Node.x);
          GeoData_P->Xmax = std::max(GeoData_P->Xmax, Geo_Node.x);
          GeoData_P->Ymin = std::min(GeoData_P->Ymin, Geo_Node.y);
          GeoData_P->Ymax = std::max(GeoData_P->Ymax, Geo_Node.y);
          GeoData_P->Zmin = std::min(GeoData_P->Zmin, Geo_Node.z);
          GeoData_P->Zmax = std::max(GeoData_P->Zmax, Geo_Node.z);
        }
      }

      if(GeoData_P->Xmin != GeoData_P->Xmax &&
         GeoData_P->Ymin != GeoData_P->Ymax &&
         GeoData_P->Zmin != GeoData_P->Zmax)
        GeoData_P->Dimension = DIM_3D;
      else if(GeoData_P->Xmin != GeoData_P->Xmax &&
              GeoData_P->Ymin != GeoData_P->Ymax)
        GeoData_P->Dimension = DIM_2D;
      else if(GeoData_P->Xmin != GeoData_P->Xmax &&
              GeoData_P->Zmin != GeoData_P->Zmax)
        GeoData_P->Dimension = DIM_2D;
      else if(GeoData_P->Ymin != GeoData_P->Ymax &&
              GeoData_P->Zmin != GeoData_P->Zmax)
        GeoData_P->Dimension = DIM_2D;
      else if(GeoData_P->Xmin != GeoData_P->Xmax)
        GeoData_P->Dimension = DIM_1D;
      else if(GeoData_P->Ymin != GeoData_P->Ymax)
        GeoData_P->Dimension = DIM_1D;
      else if(GeoData_P->Zmin != GeoData_P->Zmax)
        GeoData_P->Dimension = DIM_1D;
      else
        GeoData_P->Dimension = DIM_0D;

      GeoData_P->CharacteristicLength =
        sqrt(SQU(GeoData_P->Xmax - GeoData_P->Xmin) +
             SQU(GeoData_P->Ymax - GeoData_P->Ymin) +
             SQU(GeoData_P->Zmax - GeoData_P->Zmin));
      if(!GeoData_P->CharacteristicLength) GeoData_P->CharacteristicLength = 1.;
    }

    /*  E L E M E N T S  */

    else if(!strncmp(&String[1], "ELM", 3) ||
            !strncmp(&String[1], "Elements", 8)) {
      if(!fgets(String, sizeof(String), File_GEO)) return;
      if(sscanf(String, "%d", &Nbr) != 1) return;
      Message::Debug("%d elements", Nbr);

      if(GeoData_P->Elements == NULL)
        GeoData_P->Elements =
          List_Create(Nbr, 1000, sizeof(struct Geo_Element));

      Geo_Element.NbrEdges = Geo_Element.NbrFacets = 0;
      Geo_Element.NumEdges = Geo_Element.NumFacets = NULL;
      Geo_Element.Partition = 0;

      if(!binary) {
        for(i = 0; i < Nbr; i++) {
          if(Version == 1.0) {
            if(fscanf(File_GEO, "%d %d %d %d %d", &Geo_Element.Num, &Type,
                      &Geo_Element.Region, &Geo_Element.ElementaryRegion,
                      &Geo_Element.NbrNodes) != 5)
              return;
            Geo_Element.Type = Gmsh2GetDP(Type);
          }
          else if(Version < 3.0) {
            if(fscanf(File_GEO, "%d %d %d", &Geo_Element.Num, &Type, &NbTags) !=
               3)
              return;
            Geo_Element.Region = Geo_Element.ElementaryRegion = 1;
            for(j = 0; j < NbTags; j++) {
              if(fscanf(File_GEO, "%d", &iDummy) != 1) return;
              if(j == 0)
                Geo_Element.Region = iDummy;
              else if(j == 1)
                Geo_Element.ElementaryRegion = iDummy;
              /* ignore any other tags for now */
            }
            Geo_Element.Type = Gmsh2GetDP(Type);
            Geo_Element.NbrNodes = Geo_GetNbNodesPerElement(Geo_Element.Type);
          }
          else {
            int numData;
            if(fscanf(File_GEO, "%d %d %d %d", &Geo_Element.Num, &Type,
                      &Geo_Element.ElementaryRegion, &numData) != 4)
              return;
            Geo_Element.Type = Gmsh2GetDP(Type);
            Geo_Element.NbrNodes = Geo_GetNbNodesPerElement(Geo_Element.Type);
            std::vector<int> phys =
              entities[Geo_GetDimOfElement(Geo_Element.Type)]
                      [Geo_Element.ElementaryRegion];
            if(phys.empty()) {
              Message::Error("No physical group provided for element %d",
                             Geo_Element.Num);
              return;
            }
            else
              Geo_Element.Region = phys[0];

            /* ignore any other tags for now */
            for(j = 0; j < numData - Geo_Element.NbrNodes; j++) {
              if(fscanf(File_GEO, "%d", &iDummy) != 1) return;
            }
          }

          Geo_Element.NumNodes =
            (int *)Malloc(Geo_Element.NbrNodes * sizeof(int));
          for(j = 0; j < Geo_Element.NbrNodes; j++) {
            if(fscanf(File_GEO, "%d", &Geo_Element.NumNodes[j]) != 1) return;
          }
          if(Geo_Element.Region < 0) {
            Geo_ReverseElement(&Geo_Element);
            Geo_Element.Region = -Geo_Element.Region;
          }

          List_Add(GeoData_P->Elements, &Geo_Element);
        }
      }
      else {
        if(Version < 3.0) {
          int numElementsPartial = 0;
          while(numElementsPartial < Nbr) {
            int header[3];
            if(fread(header, sizeof(int), 3, File_GEO) != 3) return;
            if(swap) swapBytes((char *)header, sizeof(int), 3);
            Type = header[0];
            int numElms = header[1];
            int numTags = header[2];
            Geo_Element.Type = Gmsh2GetDP(Type);
            Geo_Element.NbrNodes = Geo_GetNbNodesPerElement(Geo_Element.Type);
            unsigned int n = 1 + numTags + Geo_Element.NbrNodes;
            int *data = (int *)Malloc(n * sizeof(int));
            for(i = 0; i < numElms; i++) {
              if(fread(data, sizeof(int), n, File_GEO) != n) return;
              if(swap) swapBytes((char *)data, sizeof(int), n);
              Geo_Element.Num = data[0];
              Geo_Element.Region = (numTags > 0) ? data[1] : 0;
              Geo_Element.ElementaryRegion = (numTags > 1) ? data[2] : 0;
              Geo_Element.NumNodes =
                (int *)Malloc(Geo_Element.NbrNodes * sizeof(int));
              for(j = 0; j < Geo_Element.NbrNodes; j++)
                Geo_Element.NumNodes[j] = data[numTags + 1 + j];
              List_Add(GeoData_P->Elements, &Geo_Element);
            }
            Free(data);
            numElementsPartial += numElms;
          }
        }
        else {
          for(i = 0; i < Nbr; i++) {
            int numData;
            if(fread(&Geo_Element.Num, sizeof(int), 1, File_GEO) != 1) return;
            if(swap) swapBytes((char *)&Geo_Element.Num, sizeof(int), 1);
            if(fread(&Type, sizeof(int), 1, File_GEO) != 1) return;
            if(swap) swapBytes((char *)&Type, sizeof(int), 1);
            if(fread(&Geo_Element.ElementaryRegion, sizeof(int), 1, File_GEO) !=
               1)
              return;
            if(swap)
              swapBytes((char *)&Geo_Element.ElementaryRegion, sizeof(int), 1);
            if(fread(&numData, sizeof(int), 1, File_GEO) != 1) return;
            if(swap) swapBytes((char *)&numData, sizeof(int), 1);
            std::vector<int> data;
            if(numData > 0) {
              data.resize(numData);
              if((int)fread(&data[0], sizeof(int), numData, File_GEO) !=
                 numData)
                return;
              if(swap) swapBytes((char *)&data[0], sizeof(int), numData);
            }
            Geo_Element.Type = Gmsh2GetDP(Type);
            Geo_Element.NbrNodes = Geo_GetNbNodesPerElement(Geo_Element.Type);
            Geo_Element.NumNodes =
              (int *)Malloc(Geo_Element.NbrNodes * sizeof(int));
            if((int)data.size() >= Geo_Element.NbrNodes) {
              for(j = 0; j < Geo_Element.NbrNodes; j++) {
                Geo_Element.NumNodes[j] =
                  data[numData - Geo_Element.NbrNodes + j];
              }
            }
            else {
              Message::Error("Missing node tags in element %d",
                             Geo_Element.Num);
              return;
            }
            std::vector<int> phys =
              entities[Geo_GetDimOfElement(Geo_Element.Type)]
                      [Geo_Element.ElementaryRegion];
            if(phys.empty()) {
              Message::Error("No physical group provided for element %d",
                             Geo_Element.Num);
              return;
            }
            else
              Geo_Element.Region = phys[0];

            if(Geo_Element.Region < 0) {
              Geo_ReverseElement(&Geo_Element);
              Geo_Element.Region = -Geo_Element.Region;
            }

            List_Add(GeoData_P->Elements, &Geo_Element);
          }
        }
      }

      List_Sort(GeoData_P->Elements, fcmp_Elm);
    }

    do {
      if(!fgets(String, sizeof(String), File_GEO) || feof(File_GEO)) break;
    } while(String[0] != '$');

  } /* while 1 ... */

  GeoData_P->NbrPartitions = 1;

  Geo_SnapNodes(GeoData_P);
}

void Geo_ReadFileAdapt(struct GeoData *GeoData_P)
{
  struct Geo_Element Geo_Element, *Geo_Element_P;
  int Nbr, i, Index_GeoElement;
  double E, H, P, Max_Order = -1.0;
  char String[256];

  Nbr = List_Nbr(GeoData_P->Elements);

  if(!GeoData_P->H) {
    GeoData_P->H = (double *)Malloc((Nbr + 2) * sizeof(double));
    for(i = 0; i < Nbr; i++) GeoData_P->H[i + 1] = -1.0;
  }
  if(!GeoData_P->P) {
    GeoData_P->P = (double *)Malloc((Nbr + 2) * sizeof(double));
    for(i = 0; i < Nbr; i++) GeoData_P->P[i + 1] = -1.0;
  }

  while(1) {
    do {
      if(!fgets(String, sizeof(String), File_GEO) || feof(File_GEO)) break;
    } while(String[0] != '$');

    if(feof(File_GEO)) break;

    if(!strncmp(&String[1], "Adapt", 5)) {
      if(fscanf(File_GEO, "%d", &Nbr) != 1) {
        Message::Error("Could not read adapation data");
        break;
      }
      for(i = 0; i < Nbr; i++) {
        if(fscanf(File_GEO, "%d %lf %lf %lf", &Geo_Element.Num, &E, &H, &P) !=
           4) {
          Message::Error("Could not read adaptation data");
          break;
        }
        if(!(Geo_Element_P = (struct Geo_Element *)List_PQuery(
               GeoData_P->Elements, &Geo_Element, fcmp_Elm))) {
          Message::Error("Element %d not found in database", Geo_Element.Num);
          break;
        }
        Index_GeoElement = Geo_GetGeoElementIndex(Geo_Element_P);
        GeoData_P->H[Index_GeoElement + 1] = H;
        GeoData_P->P[Index_GeoElement + 1] = P;
        if(P > Max_Order) Max_Order = P;
      }
    }

    do {
      if(!fgets(String, sizeof(String), File_GEO) || feof(File_GEO)) break;
    } while(String[0] != '$');

  } /* while 1 ... */

  if(Flag_ORDER < 0) Flag_ORDER = Max_Order;

  Message::Info("Maximum interpolation order = %g", Flag_ORDER);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t N b r P a r t i t i o n s                                 */
/* ------------------------------------------------------------------------ */

int Geo_GetNbrPartitions(void) { return CurrentGeoData->NbrPartitions; }

/* ------------------------------------------------------------------------ */
/*  f c m p _ E l m   &   f c m p _ N o d                                   */
/* ------------------------------------------------------------------------ */

int fcmp_Elm(const void *a, const void *b)
{
  return ((struct Geo_Element *)a)->Num - ((struct Geo_Element *)b)->Num;
}

int fcmp_Nod(const void *a, const void *b)
{
  return ((struct Geo_Node *)a)->Num - ((struct Geo_Node *)b)->Num;
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t N b r G e o E l e m e n t s                               */
/* ------------------------------------------------------------------------ */

int Geo_GetNbrGeoElements(void) { return (List_Nbr(CurrentGeoData->Elements)); }

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t G e o E l e m e n t                                       */
/* ------------------------------------------------------------------------ */

struct Geo_Element *Geo_GetGeoElement(int Index_Element)
{
  return ((struct Geo_Element *)List_Pointer(CurrentGeoData->Elements,
                                             Index_Element));
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t G e o E l e m e n t I n d e x                             */
/* ------------------------------------------------------------------------ */

int Geo_GetGeoElementIndex(struct Geo_Element *GeoElement)
{
  return (GeoElement -
          (struct Geo_Element *)List_Pointer(CurrentGeoData->Elements, 0));
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t G e o E l e m e n t O f N u m                             */
/* ------------------------------------------------------------------------ */

// The element of a given number: as for the nodes (see Geo_FindNode), the
// positions of the elements are tabulated, instead of a binary search (which
// first sorts the list, each time)
struct Geo_Element *Geo_GetGeoElementOfNum(int Num_Element)
{
  struct GeoData *GeoData_P = CurrentGeoData;
  struct Geo_NumberIndex &index = *GeoData_P->ElementIndex;
  int n = List_Nbr(GeoData_P->Elements);
  if(index.size != n) {
    struct Geo_Element *elements =
      n ? (struct Geo_Element *)List_Pointer(GeoData_P->Elements, 0) : NULL;
    index.build(n, [&](int i) { return elements[i].Num; });
  }
  int i = index.find(Num_Element);
  return (i < 0) ? NULL :
                   (struct Geo_Element *)List_Pointer(GeoData_P->Elements, i);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t N b r G e o N o d e s                                     */
/* ------------------------------------------------------------------------ */

int Geo_GetNbrGeoNodes(void) { return (List_Nbr(CurrentGeoData->Nodes)); }

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t G e o N o d e                                             */
/* ------------------------------------------------------------------------ */

struct Geo_Node *Geo_GetGeoNode(int Index_Node)
{
  return ((struct Geo_Node *)List_Pointer(CurrentGeoData->Nodes, Index_Node));
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t G e o N o d e O f N u m                                   */
/* ------------------------------------------------------------------------ */

struct Geo_Node *Geo_GetGeoNodeOfNum(int Num_Node)
{
  return Geo_FindNode(CurrentGeoData, Num_Node);
}

/* ------------------------------------------------------------------------ */
/*  G e o _ G e t N o d e s C o o r d i n a t e s                           */
/* ------------------------------------------------------------------------ */

void Geo_GetNodesCoordinates(int Nbr_Node, int *Num_Node, double *x, double *y,
                             double *z)
{
  int i;
  struct Geo_Node *Geo_Node_P;

  for(i = 0; i < Nbr_Node; i++) {
    if(!(Geo_Node_P = Geo_FindNode(CurrentGeoData, abs(Num_Node[i])))) {
      Message::Error("Node %d does not exist", abs(Num_Node[i]));
      break;
    }

    x[i] = Geo_Node_P->x;
    y[i] = Geo_Node_P->y;
    z[i] = Geo_Node_P->z;
  }
}

/* ------------------------------------------------------------------------ */
/*  G e o _ S e t N o d e s C o o r d i n a t e s                           */
/* ------------------------------------------------------------------------ */

void Geo_SetNodesCoordinates(int Nbr_Node, int *Num_Node, double *x, double *y,
                             double *z)
{
  int i;
  struct Geo_Node *Geo_Node_P;

  for(i = 0; i < Nbr_Node; i++) {
    if(!(Geo_Node_P = Geo_FindNode(CurrentGeoData, abs(Num_Node[i])))) {
      Message::Error("Node %d does not exist", abs(Num_Node[i]));
      break;
    }

    Geo_Node_P->x = x[i];
    Geo_Node_P->y = y[i];
    Geo_Node_P->z = z[i];
  }
}

void Geo_SetNodesCoordinatesX(int Nbr_Node, int *Num_Node, double *x)
{
  int i;
  struct Geo_Node *Geo_Node_P;

  for(i = 0; i < Nbr_Node; i++) {
    if(!(Geo_Node_P = Geo_FindNode(CurrentGeoData, abs(Num_Node[i])))) {
      Message::Error("Node %d does not exist", abs(Num_Node[i]));
      break;
    }

    Geo_Node_P->x = x[i];
  }
}

void Geo_SetNodesCoordinatesY(int Nbr_Node, int *Num_Node, double *y)
{
  int i;
  struct Geo_Node *Geo_Node_P;

  for(i = 0; i < Nbr_Node; i++) {
    if(!(Geo_Node_P = Geo_FindNode(CurrentGeoData, abs(Num_Node[i])))) {
      Message::Error("Node %d does not exist", abs(Num_Node[i]));
      break;
    }

    Geo_Node_P->y = y[i];
  }
}

void Geo_SetNodesCoordinatesZ(int Nbr_Node, int *Num_Node, double *z)
{
  int i;
  struct Geo_Node *Geo_Node_P;

  for(i = 0; i < Nbr_Node; i++) {
    if(!(Geo_Node_P = Geo_FindNode(CurrentGeoData, abs(Num_Node[i])))) {
      Message::Error("Node %d does not exist", abs(Num_Node[i]));
      break;
    }

    Geo_Node_P->z = z[i];
  }
}
