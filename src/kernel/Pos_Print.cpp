// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <sstream>
#include <vector>
#include <queue>
#include <functional>
#include <algorithm>
#include <string.h>
#include <math.h>
#include "ProData.h"
#include "ProParser.h"
#include "GeoData.h"
#include "DofData.h"
#include "Cal_PostQuantity.h"
#include "Get_Geometry.h"
#include "Get_DofOfElement.h"
#include "Get_FunctionValue.h"
#include "ExtendedGroup.h"
#include "Cal_Quantity.h"
#include "Cal_Value.h"
#include "Pos_Formulation.h"
#include "Pos_Element.h"
#include "Pos_Search.h"
#include "Pos_Print.h"
#include "Pos_Format.h"
#include "Adapt.h"
#include "MallocUtils.h"
#include "Message.h"

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

#define SQU(a) ((a) * (a))

extern struct Problem Problem_S;
extern struct CurrentData Current;

extern int Flag_BIN;

extern FILE *PostStream;

/*
  Print OnElementsOf
  ------------------
  expl: plot on elements, belonging to the current mesh, where
        the solution was computed during the processing stage
  args: list of groups of region type

  Print OnSection
  ---------------
  expl: plot an a 'real' cut of the mesh, i.e. computation on the
        intersections of the mesh with a cutting entity (plane, line)
  args: 2 (not done) or 3 points, specifying the cutting line or the cutting
  plane

  Print OnGrid
  ------------
  expl: reinterpolate the solution on a grid
  args: - a list of groups of region type (belonging to a mesh, where the
      solution will be reinterpolated)
        - 3 expressions (using $S and $T) and 2 intervals for the parametric
      grid definition

  Print OnPoint, OnLine, OnPlane, OnBox
  -------------------------------------
  expl: reinterpolate the solution on a grid (particular cases)
  args: 1, 2, 3 or 4 points (0d, 1d, 2d or 3d grid) and the associated
        number of divisions

  Print OnRegion
  --------------
  expl: print Global Quantities associated with Regions
  args: list of groups of region type

*/

/* ------------------------------------------------------------------------ */
/*  P o s _ P r i n t O n E l e m e n t s O f                               */
/* ------------------------------------------------------------------------ */

struct CutEdge {
  double x[2], y[2], z[2];
  double xc, yc, zc;
  double uc, vc, wc;
};

struct xyzv {
  double x, y, z;
  struct Value v;
  /*int nbvals; for time domain -> malloc Value *v... */
  int nboccurences;
};

struct ValMinMax {
  struct Value Val, ValX, ValY, ValZ;
};

int CompareValue(const Value *valA_P, const Value *valB_P)
{
  double cmp = 0, VecLengthSquA, VecLengthSquB;

  // if (Current.NbrHar != 1)
  //   Message::Error("Cannot compare multi-harmonic values");
  //  -> we compare the real part in this case

  switch(valA_P->Type) {
  case SCALAR: cmp = valA_P->Val[0] - valB_P->Val[0]; break;
  case VECTOR:
    VecLengthSquA = valA_P->Val[0] * valA_P->Val[0] +
                    valA_P->Val[1] * valA_P->Val[1] +
                    valA_P->Val[2] * valA_P->Val[2];
    VecLengthSquB = valB_P->Val[0] * valB_P->Val[0] +
                    valB_P->Val[1] * valB_P->Val[1] +
                    valB_P->Val[2] * valB_P->Val[2];
    cmp = VecLengthSquA - VecLengthSquB;
    break;
  default: Message::Error("Cannot compare values other than SCALAR and VECTOR");
  }
  if(cmp > 1.e-16)
    return 1;
  else if(cmp < -1.e-16)
    return -1;
  else
    return 0;
}

void SetValMinMax(struct PostElement *PE_P, int iNode,
                  struct ValMinMax *ValueMinMax_P)
{
  Cal_CopyValue(&PE_P->Value[iNode], &ValueMinMax_P->Val);
  ValueMinMax_P->ValX.Val[0] = PE_P->x[iNode];
  ValueMinMax_P->ValY.Val[0] = PE_P->y[iNode];
  ValueMinMax_P->ValZ.Val[0] = PE_P->z[iNode];
}

void InitValMinMax(struct ValMinMax *ValueMinMax_P, struct PostElement *PE_P)
{
  // Init ValueMin and ValueMax
  ValueMinMax_P->ValX.Type = SCALAR;
  ValueMinMax_P->ValY.Type = SCALAR;
  ValueMinMax_P->ValZ.Type = SCALAR;
  Cal_ZeroValue(&ValueMinMax_P->ValX);
  Cal_ZeroValue(&ValueMinMax_P->ValY);
  Cal_ZeroValue(&ValueMinMax_P->ValZ);
  SetValMinMax(PE_P, 0, ValueMinMax_P);
}

void EvalMinMax(struct PostElement *PE_P, struct ValMinMax *ValueMin_P,
                struct ValMinMax *ValueMax_P)
{
  for(int iNode = 0; iNode < PE_P->NbrNodes; iNode++) {
    if(CompareValue(&PE_P->Value[iNode], &ValueMin_P->Val) < 0)
      SetValMinMax(PE_P, iNode, ValueMin_P);
    if(CompareValue(&PE_P->Value[iNode], &ValueMax_P->Val) > 0)
      SetValMinMax(PE_P, iNode, ValueMax_P);
  }
}

static int fcmp_xyzv(const void *a, const void *b)
{
  struct xyzv *p1, *p2;
  double TOL = Current.GeoData->CharacteristicLength * 1.e-8;
  p1 = (struct xyzv *)a;
  p2 = (struct xyzv *)b;
  if(p1->x - p2->x > TOL) return 1;
  if(p1->x - p2->x < -TOL) return -1;
  if(p1->y - p2->y > TOL) return 1;
  if(p1->y - p2->y < -TOL) return -1;
  if(p1->z - p2->z > TOL) return 1;
  if(p1->z - p2->z < -TOL) return -1;
  return 0;
}

static List_T *SkinPostElement_L;
static int SkinDepth;

static void Cut_SkinPostElement(void *a, void *b)
{
  struct PostElement *PE;

  PE = *(struct PostElement **)a;

  Cut_PostElement(PE, Geo_GetGeoElement(PE->Index), SkinPostElement_L,
                  PE->Index, SkinDepth, 0, 1);
}

static void Decompose_SkinPostElement(void *a, void *b)
{
  struct PostElement *PE, *PE2;

  PE = *(struct PostElement **)a;

  if(PE->Type != QUADRANGLE) return;
  /* change the quad to a tri */
  PE->Type = TRIANGLE;
  PE->NbrNodes = 3;
  /* create a second tri */
  PE2 = NodeCopy_PostElement(PE);
  PE2->NumNodes[1] = PE->NumNodes[2];
  PE2->u[1] = PE->u[2];
  PE2->x[1] = PE->x[2];
  PE2->v[1] = PE->v[2];
  PE2->y[1] = PE->y[2];
  PE2->w[1] = PE->w[2];
  PE2->z[1] = PE->z[2];
  PE2->NumNodes[2] = PE->NumNodes[3];
  PE2->u[2] = PE->u[3];
  PE2->x[2] = PE->x[3];
  PE2->v[2] = PE->v[3];
  PE2->y[2] = PE->y[3];
  PE2->w[2] = PE->w[3];
  PE2->z[2] = PE->z[3];
  List_Add(SkinPostElement_L, &PE2);
}

bool Pos_StoresMinMax(struct PostSubOperation *PSO_P)
{
  const struct PostSubOperation *p = PSO_P;
  return p->StoreMinInRegister >= 0 || p->StoreMaxInRegister >= 0 ||
         p->StoreMinXinRegister >= 0 || p->StoreMaxXinRegister >= 0 ||
         p->StoreMinYinRegister >= 0 || p->StoreMaxYinRegister >= 0 ||
         p->StoreMinZinRegister >= 0 || p->StoreMaxZinRegister >= 0;
}

// A local Print, which each rank can evaluate on its mesh partition: a Print of
// a single local quantity (neither cumulative nor combined with another one),
// which needs no elements of other partitions (Skin, Adapt) and stores nothing
// in a register, a variable or a mesh-based field
bool Pos_IsLocalPrint(struct PostSubOperation *PSO_P)
{
  const struct PostSubOperation *p = PSO_P;
  return p->Type == POP_PRINT && p->PostQuantityIndex[0] >= 0 &&
         p->PostQuantitySupport[0] < 0 && p->PostQuantityIndex[1] < 0 &&
         !p->Skin && !p->Adapt && p->StoreInMeshBasedField < 0 &&
         p->StoreInRegister < 0 && !p->StoreInVariable;
}

// With rank-local Dofs, a Print on the elements of the mesh (or on a section of
// them) that is not a view written by all the ranks (see Pos_IsParallelView) is
// evaluated by all the ranks, each one on its mesh partition, and written by
// the first rank, which gathers the evaluated elements of all the ranks and
// formats them in the order of their numbers, as without MPI. The Prints that
// need the elements of several partitions at once (Skin, Adapt), store their
// result or are written in a mesh-based Gmsh view other than by all the ranks
// are evaluated by the first rank alone (see Pos_IsOnFirstRankOnly).
bool Pos_IsGathered(struct PostSubOperation *PSO_P)
{
#if defined(HAVE_MPI)
  return Dof_UseLocalDofs() && Pos_IsLocalPrint(PSO_P) &&
         (PSO_P->SubType == PRINT_ONELEMENTSOF ||
          PSO_P->SubType == PRINT_ONSECTION_2D) &&
         !(PSO_P->Format == FORMAT_GMSH && Format_GmshMeshBased(PSO_P)) &&
         (PSO_P->StoreInField < 0 || !Pos_GetStoreFields());
#else
  return false;
#endif
}

// With rank-local Dofs, a Print on the elements that stores its values in a
// list-based field (StoreInField) is evaluated by each rank on its mesh
// partition, and each rank keeps the field of its partition: the field
// functions (Field[], ScalarField[], ...) are evaluated by a rank at the points
// of its elements (in the assembly, in a Print), where it has the field, if the
// field and the elements are on the same regions. If the Print also writes a
// file, the file is written as a gathered Print, while the fields are not
// stored (see Pos_Formulation). The Prints that need the elements of several
// partitions (Skin, Adapt, Smoothing, Sort), store other values, or store a
// mesh-based field (StoreInMeshBasedField, which needs the mesh in a Gmsh
// model, that the ranks with a part of the mesh do not have) still need all
// the Dofs.
static bool StoreFields = true;
void Pos_SetStoreFields(bool store) { StoreFields = store; }
bool Pos_GetStoreFields() { return StoreFields; }

bool Pos_IsStoredByPartition(struct PostSubOperation *PSO_P)
{
#if defined(HAVE_MPI)
  const struct PostSubOperation *p = PSO_P;
  return Dof_UseLocalDofs() && Pos_IsLocalPrint(PSO_P) &&
         p->SubType == PRINT_ONELEMENTSOF && p->Format == FORMAT_GMSH &&
         p->StoreInField >= 0 && !p->Smoothing && !p->Sort &&
         !Pos_StoresMinMax(PSO_P);
#else
  return false;
#endif
}

// The post elements that a rank has evaluated, with their values at each time
// step: for each one, its number, type, depth, number of nodes and node numbers,
// then the type of its values at each node and time step (in ints), and its
// coordinates (u, v, w, x, y, z) and values (in reals)
struct EvaluatedPostElements {
  std::vector<int> ints;
  std::vector<double> reals;
  void addElement(struct PostElement *PE, int num)
  {
    ints.insert(ints.end(), {num, PE->Type, PE->Depth, PE->NbrNodes});
    ints.insert(ints.end(), PE->NumNodes, PE->NumNodes + PE->NbrNodes);
    for(double *c : {PE->u, PE->v, PE->w, PE->x, PE->y, PE->z})
      reals.insert(reals.end(), c, c + PE->NbrNodes);
  }
  void addValues(struct PostElement *PE)
  {
    const int nbVal = Current.NbrHar * MAX_DIM;
    for(int i = 0; i < PE->NbrNodes; i++) {
      ints.push_back(PE->Value[i].Type);
      reals.insert(reals.end(), PE->Value[i].Val, PE->Value[i].Val + nbVal);
    }
  }
  // the first rank calls value(PE, iTime) for the elements of all the ranks,
  // in the order of their numbers (each one was evaluated by a single rank, in
  // the order of the Print without MPI), at each time step, and then
  // element(PE), which returns whether it keeps PE. The first rank merges the
  // elements of the ranks, each of which sends them in the order of their
  // numbers, by blocks, when the first rank asks for the next one: it thus
  // holds at most a block of each rank (64 MB in all, and at least 64 KB per
  // rank), and not all the elements.
  template <class V, class E>
  void gatherAndFormat(int NbTimeStep, List_T *TimeStep_L, V value, E element)
  {
    const int nbVal = Current.NbrHar * MAX_DIM;
    // the sizes of an element in ints and in reals
    auto sizes = [&](const int *I) {
      std::size_t n = I[3];
      return std::make_pair(4 + n + NbTimeStep * n,
                            6 * n + NbTimeStep * n * nbVal);
    };
    // the elements of this rank (their starts in ints and reals), in the order
    // of their numbers
    std::vector<std::pair<std::size_t, std::size_t> > start;
    for(std::size_t i = 0, r = 0; i < ints.size();) {
      start.push_back({i, r});
      auto s = sizes(&ints[i]);
      i += s.first;
      r += s.second;
    }
    std::stable_sort(start.begin(), start.end(),
                     [this](const std::pair<std::size_t, std::size_t> &a,
                            const std::pair<std::size_t, std::size_t> &b) {
                       return ints[a.first] < ints[b.first];
                     });
    // the element (ints I, reals R) made and given to value and element
    auto format = [&](const int *I, const double *R) {
      const int n = I[3];
      struct PostElement *PE = Create_PostElement(NO_ELEMENT, I[1], n, I[2]);
      PE->NumElement = I[0];
      for(int i = 0; i < n; i++) PE->NumNodes[i] = I[4 + i];
      double *c[6] = {PE->u, PE->v, PE->w, PE->x, PE->y, PE->z};
      for(int j = 0; j < 6; j++) memcpy(c[j], R + j * n, n * sizeof(double));
      for(int iTime = 0; iTime < NbTimeStep; iTime++) {
        for(int i = 0; i < n; i++) {
          PE->Value[i].Type = I[4 + n + iTime * n + i];
          memcpy(PE->Value[i].Val, R + 6 * n + (iTime * n + i) * nbVal,
                 nbVal * sizeof(double));
        }
        Pos_InitAllSolutions(TimeStep_L, iTime);
        value(PE, iTime);
      }
      if(!element(PE)) Destroy_PostElement(PE);
    };
    const int np = Message::GetCommSize(), rank = Message::GetCommRank();
#if defined(HAVE_MPI)
    enum { REQUEST = 7301, INTS, REALS };
    if(np > 1 && rank) {
      // send the next block of elements each time the first rank asks for
      // it, until an empty one
      const std::size_t block = std::max<std::size_t>(1 << 16, (1 << 26) / np);
      for(std::size_t k = 0;;) {
        int request;
        MPI_Recv(&request, 1, MPI_INT, 0, REQUEST, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
        std::vector<int> bi;
        std::vector<double> br;
        while(k < start.size() &&
              bi.size() * sizeof(int) + br.size() * sizeof(double) < block) {
          const int *I = &ints[start[k].first];
          const double *R = &reals[start[k].second];
          auto s = sizes(I);
          bi.insert(bi.end(), I, I + s.first);
          br.insert(br.end(), R, R + s.second);
          k++;
        }
        MPI_Send(bi.data(), (int)bi.size(), MPI_INT, 0, INTS, MPI_COMM_WORLD);
        MPI_Send(br.data(), (int)br.size(), MPI_DOUBLE, 0, REALS,
                 MPI_COMM_WORLD);
        if(bi.empty()) return;
      }
    }
#endif
    if(rank) return;
    // the next element of each rank: its ints and reals, and where it is in
    // the elements of this rank or in the block received from another
    struct Next {
      std::vector<int> I;
      std::vector<double> R;
      std::size_t i = 0, r = 0;
    };
    std::vector<Next> next(np);
    std::size_t k0 = 0; // (in start, for this rank)
    auto current = [&](int q) {
      if(!q) return std::make_pair(&ints[start[k0].first],
                                   &reals[start[k0].second]);
      return std::make_pair(&next[q].I[next[q].i], &next[q].R[next[q].r]);
    };
    // whether rank q has another element (the next block is asked for if
    // needed)
    auto more = [&](int q) -> bool {
      if(!q) return k0 < start.size();
      Next &x = next[q];
      if(x.i < x.I.size()) return true;
#if defined(HAVE_MPI)
      int request = 1, n;
      MPI_Status status;
      MPI_Send(&request, 1, MPI_INT, q, REQUEST, MPI_COMM_WORLD);
      MPI_Probe(q, INTS, MPI_COMM_WORLD, &status);
      MPI_Get_count(&status, MPI_INT, &n);
      x.I.resize(n);
      MPI_Recv(x.I.data(), n, MPI_INT, q, INTS, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
      MPI_Probe(q, REALS, MPI_COMM_WORLD, &status);
      MPI_Get_count(&status, MPI_DOUBLE, &n);
      x.R.resize(n);
      MPI_Recv(x.R.data(), n, MPI_DOUBLE, q, REALS, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
      x.i = x.r = 0;
      return !x.I.empty();
#else
      return false;
#endif
    };
    // merge: the smallest number first, of the first rank if several
    typedef std::pair<int, int> NumberRank;
    std::priority_queue<NumberRank, std::vector<NumberRank>,
                        std::greater<NumberRank> >
      queue;
    for(int q = 0; q < np; q++)
      if(more(q)) queue.push({current(q).first[0], q});
    while(!queue.empty()) {
      const int q = queue.top().second;
      queue.pop();
      auto c = current(q);
      format(c.first, c.second);
      if(!q)
        k0++;
      else {
        auto s = sizes(c.first);
        next[q].i += s.first;
        next[q].r += s.second;
      }
      if(more(q)) queue.push({current(q).first[0], q});
    }
  }
};

// The Prints evaluated by all the ranks and written later (POS_EVALUATE), with
// what each rank has evaluated, in the order of the post-operation
static int GatherPhase = POS_EVALUATE_AND_WRITE;
static std::vector<std::pair<struct PostSubOperation *, EvaluatedPostElements> >
  EvaluatedLater;

void Pos_SetGatherPhase(int phase) { GatherPhase = phase; }
int Pos_GetGatherPhase() { return GatherPhase; }

static void Pos_KeepEvaluated(struct PostSubOperation *PSO_P,
                              EvaluatedPostElements &evaluated)
{
  EvaluatedLater.push_back({PSO_P, std::move(evaluated)});
}

static EvaluatedPostElements Pos_TakeEvaluated(struct PostSubOperation *PSO_P)
{
  EvaluatedPostElements evaluated;
  for(auto it = EvaluatedLater.begin(); it != EvaluatedLater.end(); it++) {
    if(it->first != PSO_P) continue;
    evaluated = std::move(it->second);
    EvaluatedLater.erase(it);
    break;
  }
  return evaluated;
}

// A gathered Print is written later if it writes to a file of its own and
// stores nothing (the minimum and maximum are found when it is written)
bool Pos_IsWrittenLater(struct PostSubOperation *PSO_P)
{
  const struct PostSubOperation *p = PSO_P;
  return Pos_IsGathered(PSO_P) && p->FileOut && !p->CatFile &&
         !Pos_StoresMinMax(PSO_P);
}

// The Prints to be written later must be written before an operation that is
// not a Print (which could use their files), a Print that stores something
// (which their formatting could use), or a Print to the terminal or to one of
// their files
bool Pos_MustWriteBefore(struct PostSubOperation *PSO_P,
                         const std::vector<struct PostSubOperation *> &later)
{
  const struct PostSubOperation *p = PSO_P;
  if(later.empty()) return false;
  if(p->Type != POP_PRINT || !p->FileOut || p->CatFile ||
     p->StoreInRegister >= 0 || p->StoreInVariable || p->StoreInField >= 0 ||
     p->StoreInMeshBasedField >= 0 || Pos_StoresMinMax(PSO_P))
    return true;
  for(auto q : later)
    if(!strcmp(q->FileOut, p->FileOut)) return true;
  return false;
}

// Whether the element GE is in the group of a Print on the elements (in its
// regions, or in its list of elements)
static bool Pos_IsInGroup(struct Group *Group_P, struct Geo_Element *GE)
{
  if(Group_P->Type != ELEMENTLIST)
    return List_Search(Group_P->InitialList, &GE->Region, fcmp_int);
  return Check_IsEntityInExtendedGroup(Group_P, GE->Num, 0);
}

// The skin of the post elements of PostElement_L (those given by
// Fill_PostElement with Skin that are not shared by two elements), decomposed
// into triangles (DecomposeInSimplex) and divided to the Depth of the Print:
// returns its list, which is PostElement_L or replaces it
static List_T *Pos_Skin(struct PostSubOperation *PSO_P, List_T *PostElement_L)
{
  Tree_T *PostElement_T;
  struct PostElement *PE;
  int iPost;

  PostElement_T = Tree_Create(sizeof(struct PostElement *), fcmp_PostElement);

  Message::ResetProgressMeter();
  for(iPost = 0; iPost < List_Nbr(PostElement_L); iPost++) {
    PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);
    if(Tree_PQuery(PostElement_T, &PE)) {
      Tree_Suppress(PostElement_T, &PE);
      Destroy_PostElement(PE);
    }
    else
      Tree_Add(PostElement_T, &PE);
    Message::ProgressMeter(iPost + 1, List_Nbr(PostElement_L),
                           "Post-processing (Skin)");
    if(Message::GetErrorCount()) break;
  }

  /* only decompose in simplices (triangles!) now */
  if(PSO_P->DecomposeInSimplex) {
    List_Reset(PostElement_L);
    SkinPostElement_L = PostElement_L;
    Tree_Action(PostElement_T, Decompose_SkinPostElement);
    for(iPost = 0; iPost < List_Nbr(SkinPostElement_L); iPost++)
      Tree_Add(PostElement_T,
               (struct PostElement **)List_Pointer(SkinPostElement_L, iPost));
  }

  if(PSO_P->Depth > 1) {
    List_Reset(PostElement_L);
    SkinPostElement_L = PostElement_L;
    SkinDepth = PSO_P->Depth;
    Tree_Action(PostElement_T, Cut_SkinPostElement);
  }
  else {
    List_Delete(PostElement_L);
    PostElement_L = Tree2List(PostElement_T);
  }

  Tree_Delete(PostElement_T);
  return PostElement_L;
}

// Stores the minimum and the maximum of the values, and their coordinates, in
// the registers of the Print (StoreMinInRegister, StoreMaxXinRegister, ...) if
// store, and sends them from the first rank to the others if broadcast
static void Pos_StoreMinMaxInRegisters(struct PostSubOperation *PSO_P,
                                       struct ValMinMax *ValueMin,
                                       struct ValMinMax *ValueMax, bool store,
                                       bool broadcast)
{
  const int registers[8] = {
    PSO_P->StoreMinInRegister,  PSO_P->StoreMinXinRegister,
    PSO_P->StoreMinYinRegister, PSO_P->StoreMinZinRegister,
    PSO_P->StoreMaxInRegister,  PSO_P->StoreMaxXinRegister,
    PSO_P->StoreMaxYinRegister, PSO_P->StoreMaxZinRegister};
  struct Value *values[8] = {&ValueMin->Val,  &ValueMin->ValX, &ValueMin->ValY,
                             &ValueMin->ValZ, &ValueMax->Val,  &ValueMax->ValX,
                             &ValueMax->ValY, &ValueMax->ValZ};
  for(int i = 0; i < 8; i++) {
    if(registers[i] < 0) continue;
    if(store) Cal_StoreInRegister(values[i], registers[i]);
    // (from the first rank, which evaluated them with the gathered elements)
    if(broadcast) Cal_BroadcastValueInRegister(registers[i], 0);
  }
}

// Smoothing: the value at each node of the post elements is replaced by the
// mean of the values at the nodes at the same position (see fcmp_xyzv)
static void Pos_Smooth(List_T *PostElement_L, int NbrPost)
{
  struct PostElement *PE;
  struct xyzv xyzv, *xyzv_P;
  Tree_T *xyzv_T;
  double x1, x2;
  int iPost, iNode;

  Message::Info("Smoothing (phase 1)");

  xyzv_T = Tree_Create(sizeof(struct xyzv), fcmp_xyzv);

  for(iPost = 0; iPost < NbrPost; iPost++) {
    PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);
    for(iNode = 0; iNode < PE->NbrNodes; iNode++) {
      xyzv.x = PE->x[iNode];
      xyzv.y = PE->y[iNode];
      xyzv.z = PE->z[iNode];
      if((xyzv_P = (struct xyzv *)Tree_PQuery(xyzv_T, &xyzv))) {
        x1 =
          (double)(xyzv_P->nboccurences) / (double)(xyzv_P->nboccurences + 1.);
        x2 = 1. / (double)(xyzv_P->nboccurences + 1);
        Cal_AddMultValue2(&xyzv_P->v, x1, &PE->Value[iNode], x2);
        xyzv_P->nboccurences++;
      }
      else {
        Cal_CopyValue(&PE->Value[iNode], &xyzv.v);
        xyzv.nboccurences = 1;
        Tree_Add(xyzv_T, &xyzv);
      }
    }
  }

  Message::Info("Smoothing (phase 2)");

  for(iPost = 0; iPost < NbrPost; iPost++) {
    PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);
    for(iNode = 0; iNode < PE->NbrNodes; iNode++) {
      xyzv.x = PE->x[iNode];
      xyzv.y = PE->y[iNode];
      xyzv.z = PE->z[iNode];
      if((xyzv_P = (struct xyzv *)Tree_PQuery(xyzv_T, &xyzv))) {
        Cal_CopyValue(&xyzv_P->v, &PE->Value[iNode]);
      }
      else
        Message::Warning("Node (%g,%g,%g) not found", xyzv.x, xyzv.y, xyzv.z);
    }
  }

  Tree_Delete(xyzv_T);
}

// Adaptation (Adapt): the size (H) and the interpolation order (P) of the
// NbrGeo elements of the mesh, from their errors, the mean value of the
// quantity on their post elements (Target for the others): returns the errors
// (by element index + 1)
static double *Pos_Adapt(struct PostSubOperation *PSO_P, int NbrGeo,
                         List_T *PostElement_L, int NbrPost)
{
  struct Element Element;
  struct PostElement *PE;
  double *Error, d;
  int iGeo, iPost, iNode, jj;

  Get_InitDofOfElement(&Element);

  if(!Current.GeoData->H)
    Current.GeoData->H = (double *)Malloc((NbrGeo + 2) * sizeof(double));

  if(!Current.GeoData->P)
    Current.GeoData->P = (double *)Malloc((NbrGeo + 2) * sizeof(double));

  Error = (double *)Malloc((NbrGeo + 1) * sizeof(double));

  /* All elements are perfect... */
  for(iGeo = 0; iGeo < NbrGeo; iGeo++) {
    Element.GeoElement = Geo_GetGeoElement(iGeo);
    Element.Num = Element.GeoElement->Num;
    Element.Type = Element.GeoElement->Type;
    Element.Region = Element.GeoElement->Region;
    Get_NodesCoordinatesOfElement(&Element);

    Current.GeoData->H[iGeo + 1] = Cal_MaxEdgeLength(&Element);
    Current.GeoData->P[iGeo + 1] = 1.;
    Error[iGeo + 1] = PSO_P->Target;
  }

  /* ...except those we want to optimize */
  for(iPost = 0; iPost < NbrPost; iPost++) {
    PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);
    Error[PE->Index + 1] = 0.;
    for(iNode = 0; iNode < PE->NbrNodes; iNode++)
      Error[PE->Index + 1] += PE->Value[iNode].Val[0];
    Error[PE->Index + 1] /= (double)PE->NbrNodes;
  }

  Adapt(NbrGeo, PSO_P->Adapt, PSO_P->Dimension, Error, Current.GeoData->H,
        Current.GeoData->P, PSO_P->Target);

  /* Clean up the interpolation orders to fit to what's available */
  if(List_Nbr(PSO_P->Value_L)) {
    for(iGeo = 0; iGeo < NbrGeo; iGeo++) {
      for(jj = List_Nbr(PSO_P->Value_L) - 1; jj >= 0; jj--) {
        d = *(double *)List_Pointer(PSO_P->Value_L, jj);
        if(Current.GeoData->P[iGeo + 1] > d || jj == 0) {
          Current.GeoData->P[iGeo + 1] = d;
          break;
        }
      }
    }
  }

  return Error;
}

// Prints the stored post elements, sorted (Sort), with the values of the
// adaptation (Adapt, with its Error) or their curvilinear coordinate (Sort by
// connectivity)
static void Pos_PrintStoredPostElements(struct PostSubOperation *PSO_P,
                                        List_T *PostElement_L, int NbrPost,
                                        double *Error)
{
  struct PostElement *PE;
  double Dummy[5];
  int iPost, iNode;

  /* Sort the elements */

  switch(PSO_P->Sort) {
  case SORT_BY_POSITION: List_Sort(PostElement_L, fcmp_PostElement); break;
  case SORT_BY_CONNECTIVITY:
    Sort_PostElement_Connectivity(PostElement_L);
    break;
  }

  Dummy[0] = Dummy[1] = Dummy[2] = Dummy[3] = Dummy[4] = 0.;

  for(iPost = 0; iPost < NbrPost; iPost++) {
    PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);

    /* Get the values from adaption */
    if(PSO_P->Adapt) {
      Dummy[0] = Geo_GetGeoElement(PE->Index)->Num;
      Dummy[1] = Error[PE->Index + 1];
      Dummy[2] = Current.GeoData->H[PE->Index + 1];
      Dummy[3] = Current.GeoData->P[PE->Index + 1];
      Dummy[4] = iPost ? 0 : NbrPost;

      for(iNode = 0; iNode < PE->NbrNodes; iNode++) {
        PE->Value[iNode].Type = SCALAR;
        if(PSO_P->Adapt == ADAPT_H1 || PSO_P->Adapt == ADAPT_H2)
          PE->Value[iNode].Val[0] = Dummy[2];
        else
          PE->Value[iNode].Val[0] = Dummy[3];
      }
    }

    /* Compute curvilinear coord if connection sort */
    if(PSO_P->Sort == SORT_BY_CONNECTIVITY) {
      Dummy[0] = Dummy[1];
      Dummy[1] =
        Dummy[0] + sqrt(SQU(PE->x[1] - PE->x[0]) + SQU(PE->y[1] - PE->y[0]) +
                        SQU(PE->z[1] - PE->z[0]));
      Dummy[2] = PE->v[0];
      Dummy[3] = -1.;
    }

    Format_PostElement(PSO_P, PSO_P->Iso, 1, Current.Time, 0, 1, Current.NbrHar,
                       PSO_P->HarmonicToTime, Dummy, PE);
  }
}

void Pos_PrintOnElementsOf(struct PostQuantity *NCPQ_P,
                           struct PostQuantity *CPQ_P, int Order,
                           struct DefineQuantity *DefineQuantity_P0,
                           struct QuantityStorage *QuantityStorage_P0,
                           struct PostSubOperation *PSO_P)
{
  List_T *PostElement_L, *Region_L;
  struct Group *Group_P;

  struct Element Element;
  struct PostElement *PE;
  struct Value *CumulativeValues;
  struct ValMinMax ValueMin, ValueMax;
  double *Error = NULL;
  int NbrGeo, iGeo, incGeo, NbrPost = 0, iPost;
  int NbrTimeStep, iTime, iNode;
  int Store = 0, DecomposeInSimplex = 0, Depth;
  bool StoreMinMax;
  const bool gather = Pos_IsGathered(PSO_P);
  const int phase = gather ? GatherPhase : POS_EVALUATE_AND_WRITE;
  const bool evaluate = (phase != POS_WRITE), write = (phase != POS_EVALUATE);
  const bool printHere = write && (!gather || !Message::GetCommRank());
  const bool byPartition = Pos_IsStoredByPartition(PSO_P) && StoreFields;
  EvaluatedPostElements evaluated;

  /* Do we have to store min. and max. values? */
  StoreMinMax = Pos_StoresMinMax(PSO_P);

  /* Select the TimeSteps */

  NbrTimeStep = Pos_InitTimeSteps(PSO_P);

  /* Print the header */

  NbrGeo = Geo_GetNbrGeoElements();

  if(printHere)
    Format_PostHeader(PSO_P, NbrTimeStep, Order,
                      PSO_P->Label ? PSO_P->Label :
                                     (NCPQ_P ? NCPQ_P->Name : NULL),
                      PSO_P->Label ? NULL : (CPQ_P ? CPQ_P->Name : NULL));

  /* Get the region */

  Group_P = (struct Group *)List_Pointer(Problem_S.Group,
                                         PSO_P->Case.OnRegion.RegionIndex);
  Region_L = Group_P->InitialList;
  Get_InitDofOfElement(&Element);

  /* Compute the Cumulative quantity, if any */

  if(CPQ_P) {
    Cal_PostCumulativeQuantity(Region_L, PSO_P->PostQuantitySupport[Order],
                               PSO_P->TimeStep_L, CPQ_P, DefineQuantity_P0,
                               QuantityStorage_P0, &CumulativeValues);
  }

  /* If we compute a skin, apply smoothing, sort the results, or
     perform adaption, we'll need to store all the PostElements */

  if(PSO_P->Smoothing || PSO_P->Skin || PSO_P->Adapt || PSO_P->Sort) Store = 1;
  // (the gathered elements are stored by the first rank, see below)
  if(gather) Store = 0;

  /* Check if everything is OK for Adaption */

  if(PSO_P->Adapt) {
    if(PSO_P->Dimension == DIM_ALL) {
      Message::Error(
        "You have to specify a Dimension for the adaptation (2 or 3)");
      return;
    }
    if(PSO_P->Target < 0.) {
      Message::Error(
        "You have to specify a Target for the adaptation (e.g. 0.01)");
      return;
    }
    if(NbrTimeStep > 1) {
      Message::Error("Adaption not ready with more than one time step");
      return;
    }
  }

  /* Check if we should decompose all PostElements to simplices */

  if(!PSO_P->Skin && PSO_P->DecomposeInSimplex) DecomposeInSimplex = 1;

  /* Check for de-refinement */

  if(PSO_P->Depth < 0)
    incGeo = -PSO_P->Depth;
  else
    incGeo = 1;

  /* Create the list of PostElements */

  PostElement_L =
    List_Create(Store ? NbrGeo / 10 : 10, Store ? NbrGeo / 10 : 10,
                sizeof(struct PostElement *));

  if(Store) {
    /* If we have a Skin, we will divide after the skin extraction */

    if(PSO_P->Skin && PSO_P->Depth > 1)
      Depth = 1;
    else
      Depth = PSO_P->Depth;

    /* Generate all PostElements */

    Message::ResetProgressMeter();
    for(iGeo = 0; iGeo < NbrGeo; iGeo += incGeo) {
      Element.GeoElement = Geo_GetGeoElement(iGeo);
      if(Pos_IsInGroup(Group_P, Element.GeoElement)) {
        Fill_PostElement(Element.GeoElement, PostElement_L, iGeo, Depth,
                         PSO_P->Skin, DecomposeInSimplex, 0, PSO_P->Gauss);
      }
      Message::ProgressMeter(iGeo + 1, NbrGeo, "Post-processing (Generate)");
      if(Message::GetErrorCount()) break;
    }

    /* Compute the skin */

    if(PSO_P->Skin) PostElement_L = Pos_Skin(PSO_P, PostElement_L);

  } /* if Store */

  // with the values of an evaluated element at a time step: the minimum and
  // the maximum are updated (if stored) and the element is printed (unless
  // stored)
  bool ValueMinMaxInitialized = false;
  auto minMaxAndFormat = [&](struct PostElement *PE, int iTime) {
    if(StoreMinMax) {
      if(!ValueMinMaxInitialized) {
        // Init ValueMin and ValueMax
        InitValMinMax(&ValueMin, PE);
        InitValMinMax(&ValueMax, PE);
        ValueMinMaxInitialized = true;
      }
      EvalMinMax(PE, &ValueMin, &ValueMax);
    }
    if(!Store)
      Format_PostElement(PSO_P, PSO_P->Iso, 0, Current.Time, iTime, NbrTimeStep,
                         Current.NbrHar, PSO_P->HarmonicToTime, NULL, PE);
  };

  /* Loop on GeoElements */

  Message::ResetProgressMeter();
  for(iGeo = 0; evaluate && iGeo < NbrGeo; iGeo += incGeo) {
    if(Store) {
      if(iGeo) break;
    }
    else {
      List_Reset(PostElement_L);
      Element.GeoElement = Geo_GetGeoElement(iGeo);
      // a view evaluated by all the ranks, gathered on the first rank or
      // stored by each rank: each rank evaluates its mesh partition
      if((Format_GetGmshParallel() || gather || byPartition) &&
         !Dof_IsPartitionOfThisRank(Element.GeoElement->Partition))
        continue;
      if(Pos_IsInGroup(Group_P, Element.GeoElement)) {
        int HighOrder =
          (PSO_P->Format == FORMAT_GMSH &&
           (PSO_P->StoreInField >= 0 || Format_GmshMeshBased(PSO_P))) ?
            1 :
            0;
        Fill_PostElement(Element.GeoElement, PostElement_L, iGeo, PSO_P->Depth,
                         PSO_P->Skin, DecomposeInSimplex, HighOrder,
                         PSO_P->Gauss);
      }
    }

    NbrPost = List_Nbr(PostElement_L);

    /* Loop on PostElements */

    for(iPost = 0; iPost < NbrPost; iPost++) {
      PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);

      if(!NCPQ_P) { /* Only one Cumulative */
        for(iTime = 0; iTime < NbrTimeStep; iTime++) {
          for(iNode = 0; iNode < PE->NbrNodes; iNode++)
            Cal_CopyValue(&CumulativeValues[iTime], &PE->Value[iNode]);
          if(!Store)
            Format_PostElement(PSO_P, PSO_P->Iso, 0, Current.Time, iTime,
                               NbrTimeStep, Current.NbrHar,
                               PSO_P->HarmonicToTime, NULL, PE);
        }
      }
      else { /* There is one non-cumulative */

        if(PSO_P->SubType == PRINT_ONGRID) { /* We re-interpolate */
          for(iTime = 0; iTime < NbrTimeStep; iTime++) {
            Pos_InitAllSolutions(PSO_P->TimeStep_L, iTime);
            for(iNode = 0; iNode < PE->NbrNodes; iNode++) {
              InWhichElement(Current.GeoData, Region_L, &Element,
                             PSO_P->Dimension, PE->x[iNode], PE->y[iNode],
                             PE->z[iNode], &PE->u[iNode], &PE->v[iNode],
                             &PE->w[iNode]);
              Current.Region = Element.Region;
              Current.x = PE->x[iNode];
              Current.y = PE->y[iNode];
              Current.z = PE->z[iNode];
              Cal_PostQuantity(NCPQ_P, DefineQuantity_P0, QuantityStorage_P0,
                               PSO_P->SingleProcess,
                               NULL, &Element, PE->u[iNode], PE->v[iNode],
                               PE->w[iNode], &PE->Value[iNode]);
              if(CPQ_P)
                Combine_PostQuantity(PSO_P->CombinationType, Order,
                                     &PE->Value[iNode],
                                     &CumulativeValues[iNode]);
            }
            minMaxAndFormat(PE, iTime);
          }
        }
        else { /* PRINT_ONELEMENTSOF: we work on the real mesh */
          Element.GeoElement = Geo_GetGeoElement(PE->Index);
          Element.Num = Element.GeoElement->Num;
          Element.Type = Element.GeoElement->Type;
          Current.Region = Element.Region = Element.GeoElement->Region;
          Get_NodesCoordinatesOfElement(&Element);
          if(gather) evaluated.addElement(PE, Element.Num);

          for(iTime = 0; iTime < NbrTimeStep; iTime++) {
            Pos_InitAllSolutions(PSO_P->TimeStep_L, iTime);

            for(iNode = 0; iNode < PE->NbrNodes; iNode++) {
              Current.x = PE->x[iNode];
              Current.y = PE->y[iNode];
              Current.z = PE->z[iNode];
              Cal_PostQuantity(NCPQ_P, DefineQuantity_P0, QuantityStorage_P0,
                               PSO_P->SingleProcess,
                               NULL, &Element, PE->u[iNode], PE->v[iNode],
                               PE->w[iNode], &PE->Value[iNode]);
              if(CPQ_P)
                Combine_PostQuantity(PSO_P->CombinationType, Order,
                                     &PE->Value[iNode],
                                     &CumulativeValues[iTime]);
            }
            if(gather)
              evaluated.addValues(PE);
            else
              minMaxAndFormat(PE, iTime);
          }
        }
      }

      if(!Store) Destroy_PostElement(PE);
    }
    Message::ProgressMeter(iGeo + 1, NbrGeo, "Post-processing (Compute)");
    if(Message::GetErrorCount()) break;
  } /* for iGeo */

  if(gather) {
    List_Reset(PostElement_L); // (the elements of the loop are destroyed)
    if(!write) { // written later, see Treatment_PostOperation
      Pos_KeepEvaluated(PSO_P, evaluated);
      List_Delete(PostElement_L);
      return;
    }
    if(!evaluate) evaluated = Pos_TakeEvaluated(PSO_P);
    // the elements of all the ranks are stored (with their values at the
    // last time step) for the Smoothing or the Sort, as without MPI
    if(printHere) Store = (PSO_P->Smoothing || PSO_P->Sort) ? 1 : 0;
    evaluated.gatherAndFormat(NbrTimeStep, PSO_P->TimeStep_L, minMaxAndFormat,
                              [&](struct PostElement *PE) {
                                if(Store) List_Add(PostElement_L, &PE);
                                return Store != 0;
                              });
    NbrPost = List_Nbr(PostElement_L);
  }

  /* Store minimum or maximum value in register */
  if(StoreMinMax)
    Pos_StoreMinMaxInRegisters(PSO_P, &ValueMin, &ValueMax, printHere, gather);

  /* Perform Smoothing */
  if(PSO_P->Smoothing) Pos_Smooth(PostElement_L, NbrPost);

  /* Perform Adaption */
  if(PSO_P->Adapt) Error = Pos_Adapt(PSO_P, NbrGeo, PostElement_L, NbrPost);

  /* Print everything if we are in Store mode */
  if(Store) Pos_PrintStoredPostElements(PSO_P, PostElement_L, NbrPost, Error);

  if(printHere) Format_PostFooter(PSO_P, Store);

  if(Store)
    for(iPost = 0; iPost < NbrPost; iPost++) {
      PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);
      Destroy_PostElement(PE);
    }

  List_Delete(PostElement_L);

  if(CPQ_P) Free(CumulativeValues);
  if(PSO_P->Adapt) Free(Error);
}

/* ------------------------------------------------------------------------ */
/*  P o s _ P r i n t O n S e c t i o n                                     */
/* ------------------------------------------------------------------------ */

double Plane(double a, double b, double c, double d, double x, double y,
             double z)
{
  return (a * x + b * y + c * z + d);
}

static double DIRX[3], DIRY[3], DIRZ[3], XCP, YCP;

int fcmp_Angle(const void *a, const void *b)
{
  struct CutEdge *q, *w;
  double x1, y1, x2, y2, ang1, ang2;

  q = (struct CutEdge *)a;
  w = (struct CutEdge *)b;

  x1 = q->xc * DIRX[0] + q->yc * DIRX[1] + q->zc * DIRX[2];
  y1 = q->xc * DIRY[0] + q->yc * DIRY[1] + q->zc * DIRY[2];
  x2 = w->xc * DIRX[0] + w->yc * DIRX[1] + w->zc * DIRX[2];
  y2 = w->xc * DIRY[0] + w->yc * DIRY[1] + w->zc * DIRY[2];

  ang1 = atan2(y1 - YCP, x1 - XCP);
  ang2 = atan2(y2 - YCP, x2 - XCP);

  if(ang1 > ang2) return 1;
  return -1;
}

void prodvec(double *a, double *b, double *c)
{
  c[0] = a[1] * b[2] - a[2] * b[1];
  c[1] = a[2] * b[0] - a[0] * b[2];
  c[2] = a[0] * b[1] - a[1] * b[0];
}

void normvec(double *a)
{
  double mod;
  mod = sqrt(SQU(a[0]) + SQU(a[1]) + SQU(a[2]));
  a[0] /= mod;
  a[1] /= mod;
  a[2] /= mod;
}

#define NBR_MAX_CUT 10

// The points where the edges of an element cross the plane Ax+By+Cz+D=0, in e
// (at most NBR_MAX_CUT), with their coordinates in the reference element if
// uvw, sorted by angle around their center if there are more than 3: returns
// their number
static int Pos_CutElement(struct Element *Element, double A, double B, double C,
                          double D, bool uvw, struct CutEdge *e)
{
  int NbCut, iCut, iEdge, *NumNodes;
  double d1, d2, u, xcg, ycg, zcg;

  Get_NodesCoordinatesOfElement(Element);

  if(Element->GeoElement->NbrEdges == 0)
    Geo_CreateEdgesOfElement(Element->GeoElement);

  NbCut = 0;

  for(iEdge = 0; iEdge < Element->GeoElement->NbrEdges; iEdge++) {
    NumNodes = Geo_GetNodesOfEdgeInElement(Element->GeoElement, iEdge);
    e[NbCut].x[0] = Element->x[abs(NumNodes[0]) - 1];
    e[NbCut].y[0] = Element->y[abs(NumNodes[0]) - 1];
    e[NbCut].z[0] = Element->z[abs(NumNodes[0]) - 1];
    e[NbCut].x[1] = Element->x[abs(NumNodes[1]) - 1];
    e[NbCut].y[1] = Element->y[abs(NumNodes[1]) - 1];
    e[NbCut].z[1] = Element->z[abs(NumNodes[1]) - 1];
    d1 = Plane(A, B, C, D, e[NbCut].x[0], e[NbCut].y[0], e[NbCut].z[0]);
    d2 = Plane(A, B, C, D, e[NbCut].x[1], e[NbCut].y[1], e[NbCut].z[1]);

    if(d1 * d2 <= 0) {
      if(d1 * d2 < 0.)
        u = -d2 / (d1 - d2);
      else if(d1 == 0.)
        u = 1.;
      else
        u = 0.;
      e[NbCut].xc = u * e[NbCut].x[0] + (1. - u) * e[NbCut].x[1];
      e[NbCut].yc = u * e[NbCut].y[0] + (1. - u) * e[NbCut].y[1];
      e[NbCut].zc = u * e[NbCut].z[0] + (1. - u) * e[NbCut].z[1];

      if(uvw)
        xyz2uvwInAnElement(Element, e[NbCut].xc, e[NbCut].yc, e[NbCut].zc,
                           &e[NbCut].uc, &e[NbCut].vc, &e[NbCut].wc);
      NbCut++;
    }
  }

  if(NbCut > 3) {
    xcg = ycg = zcg = 0.;
    for(iCut = 0; iCut < NbCut; iCut++) {
      xcg += e[iCut].xc;
      ycg += e[iCut].yc;
      zcg += e[iCut].zc;
    }
    xcg /= (double)NbCut;
    ycg /= (double)NbCut;
    zcg /= (double)NbCut;
    DIRZ[0] = A;
    DIRY[0] = xcg - e[0].xc;
    DIRZ[1] = B;
    DIRY[1] = ycg - e[0].yc;
    DIRZ[2] = C;
    DIRY[2] = zcg - e[0].zc;
    normvec(DIRZ);
    normvec(DIRY);
    prodvec(DIRY, DIRZ, DIRX);
    normvec(DIRX);
    XCP = xcg * DIRX[0] + ycg * DIRX[1] + zcg * DIRX[2];
    YCP = xcg * DIRY[0] + ycg * DIRY[1] + zcg * DIRY[2];
    qsort(e, NbCut, sizeof(struct CutEdge), fcmp_Angle);
  }

  return NbCut;
}

// The post element of the cut of the element iGeo through the n (2 or 3) points
// c: a line or a triangle, or, with a Depth < 1, the point at their center
static struct PostElement *Pos_CutPostElement(int iGeo, int Depth, int n,
                                              struct CutEdge *c[])
{
  struct PostElement *PE;
  if(Depth > 0) {
    PE = Create_PostElement(iGeo, (n == 3) ? TRIANGLE : LINE, n, 1);
    for(int i = 0; i < n; i++) {
      PE->x[i] = c[i]->xc;
      PE->y[i] = c[i]->yc;
      PE->z[i] = c[i]->zc;
      PE->u[i] = c[i]->uc;
      PE->v[i] = c[i]->vc;
      PE->w[i] = c[i]->wc;
    }
  }
  else {
    PE = Create_PostElement(iGeo, POINT_ELEMENT, 1, 0);
    PE->x[0] = c[0]->xc;
    PE->y[0] = c[0]->yc;
    PE->z[0] = c[0]->zc;
    PE->u[0] = c[0]->uc;
    PE->v[0] = c[0]->vc;
    PE->w[0] = c[0]->wc;
    for(int i = 1; i < n; i++) {
      PE->x[0] += c[i]->xc;
      PE->y[0] += c[i]->yc;
      PE->z[0] += c[i]->zc;
      PE->u[0] += c[i]->uc;
      PE->v[0] += c[i]->vc;
      PE->w[0] += c[i]->wc;
    }
    PE->x[0] /= n;
    PE->y[0] /= n;
    PE->z[0] /= n;
    PE->u[0] /= n;
    PE->v[0] /= n;
    PE->w[0] /= n;
  }
  return PE;
}

void Pos_PrintOnSection(struct PostQuantity *NCPQ_P, struct PostQuantity *CPQ_P,
                        int Order, struct DefineQuantity *DefineQuantity_P0,
                        struct QuantityStorage *QuantityStorage_P0,
                        struct PostSubOperation *PSO_P)
{
  struct CutEdge e[NBR_MAX_CUT];
  struct Element Element;
  struct Value *CumulativeValues;
  List_T *PE_L;

  int NbGeoElement, NbTimeStep, NbCut;
  int iGeo, iCut;
  double A, B, C, D;
  double x[3], y[3], z[3];

  NbTimeStep = Pos_InitTimeSteps(PSO_P);

  // (see Pos_IsGathered)
  const bool gather = Pos_IsGathered(PSO_P);
  const int phase = gather ? GatherPhase : POS_EVALUATE_AND_WRITE;
  const bool evaluate = (phase != POS_WRITE), write = (phase != POS_EVALUATE);
  const bool printHere = write && (!gather || !Message::GetCommRank());
  EvaluatedPostElements evaluated;

  PE_L = List_Create(10, 10, sizeof(struct PostElement *));

  if(printHere)
    Format_PostHeader(PSO_P, NbTimeStep, Order,
                    PSO_P->Label ? PSO_P->Label :
                                   (NCPQ_P ? NCPQ_P->Name : NULL),
                    PSO_P->Label ? NULL : (CPQ_P ? CPQ_P->Name : NULL));

  if(CPQ_P) {
    Cal_PostCumulativeQuantity(NULL, PSO_P->PostQuantitySupport[Order],
                               PSO_P->TimeStep_L, CPQ_P, DefineQuantity_P0,
                               QuantityStorage_P0, &CumulativeValues);
  }

  // evaluates the quantity on PE, the post element of a cut of Element (or on
  // its subdivisions, with a Depth > 1), and prints it, or keeps it to be
  // gathered; the post elements are then destroyed
  auto printCut = [&](struct PostElement *PE) {
    List_Reset(PE_L);
    if(PSO_P->Depth < 2)
      List_Add(PE_L, &PE);
    else
      Cut_PostElement(PE, Element.GeoElement, PE_L, PE->Index, PSO_P->Depth, 0,
                      1);
    for(int iPost = 0; iPost < List_Nbr(PE_L); iPost++) {
      PE = *(struct PostElement **)List_Pointer(PE_L, iPost);
      if(gather) evaluated.addElement(PE, Element.Num);
      for(int iTime = 0; iTime < NbTimeStep; iTime++) {
        Pos_InitAllSolutions(PSO_P->TimeStep_L, iTime);
        for(int iNode = 0; iNode < PE->NbrNodes; iNode++) {
          if(NCPQ_P) {
            Current.x = PE->x[iNode];
            Current.y = PE->y[iNode];
            Current.z = PE->z[iNode];
            Cal_PostQuantity(NCPQ_P, DefineQuantity_P0, QuantityStorage_P0,
                             PSO_P->SingleProcess, NULL, &Element, PE->u[iNode],
                             PE->v[iNode], PE->w[iNode], &PE->Value[iNode]);
            if(CPQ_P)
              Combine_PostQuantity(PSO_P->CombinationType, Order,
                                   &PE->Value[iNode], &CumulativeValues[iTime]);
          }
          else
            Cal_CopyValue(&CumulativeValues[iTime], &PE->Value[iNode]);
        }
        if(gather)
          evaluated.addValues(PE);
        else
          Format_PostElement(PSO_P, PSO_P->Iso, 0, Current.Time, iTime,
                             NbTimeStep, Current.NbrHar, PSO_P->HarmonicToTime,
                             NULL, PE);
      }
    }
    for(int iPost = 0; iPost < List_Nbr(PE_L); iPost++)
      Destroy_PostElement(*(struct PostElement **)List_Pointer(PE_L, iPost));
  };

  switch(PSO_P->SubType) {
  case PRINT_ONSECTION_1D:
    Message::Error("Print on 1D cuts not done (use Print OnLine instead)");
    break;

  case PRINT_ONSECTION_2D:

    /* Ax+By+Cz+D=0  from  (x0,y0,z0),(x1,y1,z1),(x2,y2,z2) */

    x[0] = PSO_P->Case.OnSection.x[0];
    y[0] = PSO_P->Case.OnSection.y[0];
    z[0] = PSO_P->Case.OnSection.z[0];
    x[1] = PSO_P->Case.OnSection.x[1];
    y[1] = PSO_P->Case.OnSection.y[1];
    z[1] = PSO_P->Case.OnSection.z[1];
    x[2] = PSO_P->Case.OnSection.x[2];
    y[2] = PSO_P->Case.OnSection.y[2];
    z[2] = PSO_P->Case.OnSection.z[2];
    A = (y[1] - y[0]) * (z[2] - z[0]) - (z[1] - z[0]) * (y[2] - y[0]);
    B = -(x[1] - x[0]) * (z[2] - z[0]) + (z[1] - z[0]) * (x[2] - x[0]);
    C = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
    D = -A * x[0] - B * y[0] - C * z[0];

    /* Cut each element */

    NbGeoElement = Geo_GetNbrGeoElements();

    Message::ResetProgressMeter();
    for(iGeo = 0; evaluate && iGeo < NbGeoElement; iGeo++) {
      Element.GeoElement = Geo_GetGeoElement(iGeo);
      if(gather && !Dof_IsPartitionOfThisRank(Element.GeoElement->Partition))
        continue;
      Element.Num = Element.GeoElement->Num;
      Element.Type = Element.GeoElement->Type;
      Current.Region = Element.Region = Element.GeoElement->Region;

      if((PSO_P->Dimension == DIM_ALL &&
          (Element.GeoElement->Type != POINT_ELEMENT)) ||
         (PSO_P->Dimension == DIM_3D &&
          (Element.GeoElement->Type &
           (TETRAHEDRON | HEXAHEDRON | PRISM | PYRAMID))) ||
         (PSO_P->Dimension == DIM_2D &&
          (Element.GeoElement->Type & (TRIANGLE | QUADRANGLE))) ||
         (PSO_P->Dimension == DIM_1D && (Element.GeoElement->Type & LINE))) {
        NbCut = Pos_CutElement(&Element, A, B, C, D, NCPQ_P != NULL, e);

        // the cut: a polygon, divided into triangles from its first point, or
        // a segment (or their centers, with a Depth < 1)
        for(iCut = 2; iCut < NbCut; iCut++) {
          struct CutEdge *c[3] = {&e[0], &e[iCut - 1], &e[iCut]};
          printCut(Pos_CutPostElement(iGeo, PSO_P->Depth, 3, c));
        }

        if(NbCut == 2) {
          struct CutEdge *c[2] = {&e[0], &e[1]};
          printCut(Pos_CutPostElement(iGeo, PSO_P->Depth, 2, c));
        }
      }
      Message::ProgressMeter(iGeo + 1, NbGeoElement, "Post-processing (Cut)");
      if(Message::GetErrorCount()) break;
    }
    if(gather && !write) { // written later, see Treatment_PostOperation
      Pos_KeepEvaluated(PSO_P, evaluated);
      break;
    }
    if(gather && !evaluate) evaluated = Pos_TakeEvaluated(PSO_P);
    if(gather)
      evaluated.gatherAndFormat(
        NbTimeStep, PSO_P->TimeStep_L,
        [&](struct PostElement *PE, int iTime) {
          Format_PostElement(PSO_P, PSO_P->Iso, 0, Current.Time, iTime,
                             NbTimeStep, Current.NbrHar,
                             PSO_P->HarmonicToTime, NULL, PE);
        },
        [](struct PostElement *) { return false; });
    if(printHere) Format_PostFooter(PSO_P, 0);
    break;

  default: Message::Error("Unknown operation in Print OnSection"); break;
  }

  List_Delete(PE_L);
  if(CPQ_P) Free(CumulativeValues);
}

#undef NBR_MAX_CUT

/* ------------------------------------------------------------------------ */
/*  P o s _ P r i n t O n G r i d                                           */
/* ------------------------------------------------------------------------ */

// The value of harmonic k at time step t at the point (i, j) of the grid of a
// Print OnPlane with Depth > 1, kept in Array to be printed on the triangles of
// the grid
#define ARRAY(i, j, k, t)                                                      \
  Array[(((t) * Current.NbrHar + (k)) * (PSO_P->Case.OnGrid.n[1] + 1) + (j)) * \
          (PSO_P->Case.OnGrid.n[0] + 1) +                                      \
        (i)]

// Prints the points of a line (Print OnLine), from its first point to the
// second, with their distance to the first in Normal[0]
template <class P>
static void Pos_PrintOnLine(struct PostSubOperation *PSO_P, double *Normal,
                            P &printPoint)
{
  const double *X = PSO_P->Case.OnGrid.x, *Y = PSO_P->Case.OnGrid.y,
               *Z = PSO_P->Case.OnGrid.z;
  const int *N = PSO_P->Case.OnGrid.n;
  double Length, S[1];

  Normal[1] = Normal[2] = 0.0;
  Length = sqrt(SQU(X[1] - X[0]) + SQU(Y[1] - Y[0]) + SQU(Z[1] - Z[0]));
  for(int i1 = 0; i1 <= N[0]; i1++) {
    S[0] = (double)i1 / (double)(N[0] ? N[0] : 1);
    Normal[0] = Length * S[0];
    Current.x = X[0] + (X[1] - X[0]) * S[0];
    Current.y = Y[0] + (Y[1] - Y[0]) * S[0];
    Current.z = Z[0] + (Z[1] - Z[0]) * S[0];
    printPoint(0, 0);
  }
}

// The unit normal to the plane of a Print OnPlane (through its 3 points), in
// Normal: returns false if it is null
static bool Pos_NormalOfPlane(struct PostSubOperation *PSO_P, double *Normal)
{
  const double *X = PSO_P->Case.OnGrid.x, *Y = PSO_P->Case.OnGrid.y,
               *Z = PSO_P->Case.OnGrid.z;
  double Length, S[3], N[3];

  S[0] = X[1] - X[0];
  S[1] = Y[1] - Y[0];
  S[2] = Z[1] - Z[0];
  N[0] = X[2] - X[0];
  N[1] = Y[2] - Y[0];
  N[2] = Z[2] - Z[0];
  prodvec(S, N, Normal);
  Length = sqrt(SQU(Normal[0]) + SQU(Normal[1]) + SQU(Normal[2]));
  if(!Length) {
    Message::Warning("Bad plane (null normal)");
    return false;
  }
  Normal[0] /= Length;
  Normal[1] /= Length;
  Normal[2] /= Length;
  return true;
}

// Prints the points of the grid of a plane (Print OnPlane), from its first
// point along the directions to the second (i1) and to the third (i2)
template <class P>
static void Pos_PrintOnPlane(struct PostSubOperation *PSO_P, bool print,
                             P &printPoint)
{
  const double *X = PSO_P->Case.OnGrid.x, *Y = PSO_P->Case.OnGrid.y,
               *Z = PSO_P->Case.OnGrid.z;
  const int *N = PSO_P->Case.OnGrid.n;
  double S[2];

  for(int i1 = 0; i1 <= N[0]; i1++) {
    S[0] = (double)i1 / (double)(N[0] ? N[0] : 1);
    for(int i2 = 0; i2 <= N[1]; i2++) {
      S[1] = (double)i2 / (double)(N[1] ? N[1] : 1);
      Current.x = X[0] + (X[1] - X[0]) * S[0] + (X[2] - X[0]) * S[1];
      Current.y = Y[0] + (Y[1] - Y[0]) * S[0] + (Y[2] - Y[0]) * S[1];
      Current.z = Z[0] + (Z[1] - Z[0]) * S[0] + (Z[2] - Z[0]) * S[1];
      printPoint(i1, i2);
    }
    if(print && PostStream && PSO_P->Depth < 2 && !Flag_BIN)
      fprintf(PostStream, "\n");
  }
}

// Prints the values at the points of the grid of a plane, kept in Array (Print
// OnPlane with Depth > 1), on the two triangles of each cell of the grid
static void Pos_PrintOnPlaneTriangles(struct PostSubOperation *PSO_P,
                                      int NbTimeStep, float *Array,
                                      double *Normal)
{
  const double *X = PSO_P->Case.OnGrid.x, *Y = PSO_P->Case.OnGrid.y,
               *Z = PSO_P->Case.OnGrid.z;
  const int *N = PSO_P->Case.OnGrid.n;
  struct PostElement *PE2;
  double S[4], tmp[3];
  int i1, i2, k, ts;

  PE2 = Create_PostElement(0, TRIANGLE, 3, 0);
  PE2->Value[0].Type = PE2->Value[1].Type = PE2->Value[2].Type = SCALAR;
  for(i1 = 0; i1 < N[0]; i1++) {
    S[0] = (double)i1 / (double)(N[0] ? N[0] : 1);
    S[2] = (double)(i1 + 1) / (double)(N[0] ? N[0] : 1);
    for(i2 = 0; i2 < N[1]; i2++) {
      S[1] = (double)i2 / (double)(N[1] ? N[1] : 1);
      S[3] = (double)(i2 + 1) / (double)(N[1] ? N[1] : 1);
      PE2->x[0] = X[0] + (X[1] - X[0]) * S[0] + (X[2] - X[0]) * S[1];
      PE2->y[0] = Y[0] + (Y[1] - Y[0]) * S[0] + (Y[2] - Y[0]) * S[1];
      PE2->z[0] = Z[0] + (Z[1] - Z[0]) * S[0] + (Z[2] - Z[0]) * S[1];
      PE2->x[1] = X[0] + (X[1] - X[0]) * S[2] + (X[2] - X[0]) * S[1];
      PE2->y[1] = Y[0] + (Y[1] - Y[0]) * S[2] + (Y[2] - Y[0]) * S[1];
      PE2->z[1] = Z[0] + (Z[1] - Z[0]) * S[2] + (Z[2] - Z[0]) * S[1];
      PE2->x[2] = X[0] + (X[1] - X[0]) * S[0] + (X[2] - X[0]) * S[3];
      PE2->y[2] = Y[0] + (Y[1] - Y[0]) * S[0] + (Y[2] - Y[0]) * S[3];
      PE2->z[2] = Z[0] + (Z[1] - Z[0]) * S[0] + (Z[2] - Z[0]) * S[3];
      for(ts = 0; ts < NbTimeStep; ts++) {
        for(k = 0; k < Current.NbrHar; k++) {
          PE2->Value[0].Val[MAX_DIM * k] = ARRAY(i1, i2, k, ts);
          PE2->Value[1].Val[MAX_DIM * k] = ARRAY(i1 + 1, i2, k, ts);
          PE2->Value[2].Val[MAX_DIM * k] = ARRAY(i1, i2 + 1, k, ts);
        }
        Format_PostElement(PSO_P, PSO_P->Iso, 0, Current.Time, ts, NbTimeStep,
                           Current.NbrHar, PSO_P->HarmonicToTime, Normal, PE2);
      }
      PE2->x[0] = X[0] + (X[1] - X[0]) * S[2] + (X[2] - X[0]) * S[3];
      PE2->y[0] = Y[0] + (Y[1] - Y[0]) * S[2] + (Y[2] - Y[0]) * S[3];
      PE2->z[0] = Z[0] + (Z[1] - Z[0]) * S[2] + (Z[2] - Z[0]) * S[3];
      tmp[0] = PE2->x[1];
      PE2->x[1] = PE2->x[2];
      PE2->x[2] = tmp[0];
      tmp[1] = PE2->y[1];
      PE2->y[1] = PE2->y[2];
      PE2->y[2] = tmp[1];
      tmp[2] = PE2->z[1];
      PE2->z[1] = PE2->z[2];
      PE2->z[2] = tmp[2];
      for(ts = 0; ts < NbTimeStep; ts++) {
        for(k = 0; k < Current.NbrHar; k++) {
          PE2->Value[0].Val[MAX_DIM * k] = ARRAY(i1 + 1, i2 + 1, k, ts);
          PE2->Value[1].Val[MAX_DIM * k] = ARRAY(i1, i2 + 1, k, ts);
          PE2->Value[2].Val[MAX_DIM * k] = ARRAY(i1 + 1, i2, k, ts);
        }
        Format_PostElement(PSO_P, PSO_P->Iso, 0, Current.Time, ts, NbTimeStep,
                           Current.NbrHar, PSO_P->HarmonicToTime, Normal, PE2);
      }
    }
  }
  Destroy_PostElement(PE2);
}

// Prints the points of the grid of a box (Print OnBox), from its first point
// along the directions to the second (i1), the third (i2) and the fourth (i3)
template <class P>
static void Pos_PrintOnBox(struct PostSubOperation *PSO_P, bool print,
                           double *Normal, P &printPoint)
{
  const double *X = PSO_P->Case.OnGrid.x, *Y = PSO_P->Case.OnGrid.y,
               *Z = PSO_P->Case.OnGrid.z;
  const int *N = PSO_P->Case.OnGrid.n;
  double S[3];

  Normal[0] = Normal[1] = Normal[2] = 0.0;
  for(int i1 = 0; i1 <= N[0]; i1++) {
    S[0] = (double)i1 / (double)(N[0] ? N[0] : 1);
    for(int i2 = 0; i2 <= N[1]; i2++) {
      S[1] = (double)i2 / (double)(N[1] ? N[1] : 1);
      for(int i3 = 0; i3 <= N[2]; i3++) {
        S[2] = (double)i3 / (double)(N[2] ? N[2] : 1);
        Current.x = X[0] + (X[1] - X[0]) * S[0] + (X[2] - X[0]) * S[1] +
                    (X[3] - X[0]) * S[2];
        Current.y = Y[0] + (Y[1] - Y[0]) * S[0] + (Y[2] - Y[0]) * S[1] +
                    (Y[3] - Y[0]) * S[2];
        Current.z = Z[0] + (Z[1] - Z[0]) * S[0] + (Z[2] - Z[0]) * S[1] +
                    (Z[3] - Z[0]) * S[2];
        printPoint(0, 0);
      }
      if(print && PostStream && !Flag_BIN) fprintf(PostStream, "\n");
    }
    if(print && PostStream && !Flag_BIN) fprintf(PostStream, "\n\n");
    /*  two blanks lines for -index in gnuplot  */
  }
}

// Prints the points of a parametric grid (Print OnGrid): the points given by 3
// expressions of the parameters $A, $B and $C, which are put in Normal
template <class P>
static void Pos_PrintOnParamGrid(struct PostSubOperation *PSO_P, bool print,
                                 double *Normal, P &printPoint)
{
  struct Value Value;

  for(int i1 = 0; i1 < List_Nbr(PSO_P->Case.OnParamGrid.ParameterValue[0]);
      i1++) {
    List_Read(PSO_P->Case.OnParamGrid.ParameterValue[0], i1, &Current.a);
    for(int i2 = 0; i2 < List_Nbr(PSO_P->Case.OnParamGrid.ParameterValue[1]);
        i2++) {
      List_Read(PSO_P->Case.OnParamGrid.ParameterValue[1], i2, &Current.b);
      for(int i3 = 0; i3 < List_Nbr(PSO_P->Case.OnParamGrid.ParameterValue[2]);
          i3++) {
        List_Read(PSO_P->Case.OnParamGrid.ParameterValue[2], i3, &Current.c);
        Get_ValueOfExpressionByIndex(PSO_P->Case.OnParamGrid.ExpressionIndex[0],
                                     NULL, 0., 0., 0., &Value);
        Current.x = Value.Val[0];
        Get_ValueOfExpressionByIndex(PSO_P->Case.OnParamGrid.ExpressionIndex[1],
                                     NULL, 0., 0., 0., &Value);
        Current.y = Value.Val[0];
        Get_ValueOfExpressionByIndex(PSO_P->Case.OnParamGrid.ExpressionIndex[2],
                                     NULL, 0., 0., 0., &Value);
        Current.z = Value.Val[0];
        Normal[0] = Current.a;
        Normal[1] = Current.b;
        Normal[2] = Current.c;
        printPoint(0, 0);
      }
      if(print && PostStream &&
         List_Nbr(PSO_P->Case.OnParamGrid.ParameterValue[2]) > 1 && !Flag_BIN)
        fprintf(PostStream, "\n");
    }
    if(print && PostStream &&
       List_Nbr(PSO_P->Case.OnParamGrid.ParameterValue[1]) > 1 && !Flag_BIN)
      fprintf(PostStream, "\n\n");
    /*  two blanks lines for -index in gnuplot  */
  }
}

void Pos_PrintOnGrid(struct PostQuantity *NCPQ_P, struct PostQuantity *CPQ_P,
                     int Order, struct DefineQuantity *DefineQuantity_P0,
                     struct QuantityStorage *QuantityStorage_P0,
                     struct PostSubOperation *PSO_P)
{
  struct Element Element;
  struct Value *CumulativeValues = NULL;
  struct PostElement *PE;

  int NbTimeStep;
  float *Array = NULL;
  double Normal[4] = {0., 0., 0., 0.};

  Get_InitDofOfElement(&Element);

  NbTimeStep = Pos_InitTimeSteps(PSO_P);

  if(CPQ_P) {
    Cal_PostCumulativeQuantity(NULL, PSO_P->PostQuantitySupport[Order],
                               PSO_P->TimeStep_L, CPQ_P, DefineQuantity_P0,
                               QuantityStorage_P0, &CumulativeValues);
  }

  // With rank-local Dofs the points are visited twice. All the ranks first
  // evaluate the points that they find in an element of their mesh partition
  // (pass 0); the first rank then gets what they computed and prints all the
  // points (pass 1), those that no rank found as without MPI. A point between
  // two partitions is taken from the element with the lowest number, as
  // without MPI (see InWhichElement).
  const bool overRanks = Dof_UseLocalDofs();
  const bool printHere = !overRanks || !Message::GetCommRank();
  const int nbVal = Current.NbrHar * MAX_DIM; // numbers stored for a value
  int pass = 0, point = 0; // index of the current point
  bool print = false; // whether the points are printed in this pass
  // what this rank evaluated, then (on the first rank) what they all did: for
  // each point evaluated by a rank, its index, the number, the region and the
  // dimension of the element found, and its values (their type and numbers,
  // for each time step)
  std::vector<int> points, elements, regions, dims, types;
  std::vector<double> vals;
  std::vector<int> evaluated; // where is each point in these vectors (or -1)

  // evaluates the quantity at the current point and prints it, or stores it in
  // Array at the point (i1, i2) of the grid (for OnPlane with Depth > 1)
  auto printPoint = [&](int i1, int i2) {
    const int other = (overRanks && pass == 1) ? evaluated[point] : -1;
    point++;
    PE->x[0] = Current.xp = Current.x;
    PE->y[0] = Current.yp = Current.y;
    PE->z[0] = Current.zp = Current.z;

    auto output = [&](int ts) {
      if(PSO_P->Depth < 2 || PSO_P->SubType != PRINT_ONGRID_2D)
        Format_PostElement(PSO_P, PSO_P->Iso, 0, Current.Time, ts, NbTimeStep,
                           Current.NbrHar, PSO_P->HarmonicToTime, Normal, PE);
      else if(PE->Value[0].Type != SCALAR)
        Message::Error(
          "Print OnPlane not designed for non scalars with Depth > 1");
      else
        for(int k = 0; k < Current.NbrHar; k++)
          ARRAY(i1, i2, k, ts) = (float)PE->Value[0].Val[MAX_DIM * k];
    };

    if(!NCPQ_P) { // the same cumulative value at all the points
      for(int ts = 0; ts < NbTimeStep && print; ts++) {
        PE->Value[0] = CumulativeValues[ts];
        output(ts);
      }
      return;
    }

    double u = 0., v = 0., w = 0.;
    if(other < 0) {
      InWhichElement(Current.GeoData, NULL, &Element, PSO_P->Dimension,
                     Current.x, Current.y, Current.z, &u, &v, &w);
      if(!print) {
        if(Element.Num == NO_ELEMENT) return; // not in the partition of this rank
        points.push_back(point - 1);
        elements.push_back(Element.Num);
        regions.push_back(Element.Region);
        dims.push_back(DimensionOfElementType(Element.Type));
      }
      Current.Region = Element.Region;
      PE->Index = (Element.Num != NO_ELEMENT) ?
                    Geo_GetGeoElementIndex(Element.GeoElement) :
                    NO_ELEMENT;
      PE->NumElement = 0;
    }
    else {
      // the element of another rank, which the first rank need not have
      Current.Region = regions[other];
      PE->Index = NO_ELEMENT;
      PE->NumElement = elements[other];
    }

    for(int ts = 0; ts < NbTimeStep; ts++) {
      Pos_InitAllSolutions(PSO_P->TimeStep_L, ts);
      if(other < 0) {
        Cal_PostQuantity(NCPQ_P, DefineQuantity_P0, QuantityStorage_P0,
                         PSO_P->SingleProcess, NULL, &Element, u, v, w,
                         &PE->Value[0]);
        if(CPQ_P)
          Combine_PostQuantity(PSO_P->CombinationType, Order, &PE->Value[0],
                               &CumulativeValues[ts]);
      }
      else {
        PE->Value[0].Type = types[other * NbTimeStep + ts];
        memcpy(PE->Value[0].Val, &vals[(other * NbTimeStep + ts) * nbVal],
               nbVal * sizeof(double));
      }
      if(print)
        output(ts);
      else {
        types.push_back(PE->Value[0].Type);
        vals.insert(vals.end(), PE->Value[0].Val, PE->Value[0].Val + nbVal);
      }
    }
  };

  if(printHere)
    Format_PostHeader(PSO_P, NbTimeStep, Order,
                      PSO_P->Label ? PSO_P->Label :
                                     (NCPQ_P ? NCPQ_P->Name : NULL),
                      PSO_P->Label ? NULL : (CPQ_P ? CPQ_P->Name : NULL));

  PE = Create_PostElement(0, POINT_ELEMENT, 1, 0);

  Pos_SearchInPartitionOfThisRank(overRanks);

  for(pass = 0; pass < (overRanks ? 2 : 1); pass++) {
    print = !overRanks || pass == 1;
#if defined(HAVE_MPI)
    if(overRanks && pass == 1) {
      Message::Gather(points, false);
      Message::Gather(elements, false);
      Message::Gather(regions, false);
      Message::Gather(dims, false);
      Message::Gather(types, false);
      Message::Gather(vals, false);
      if(!printHere) break;
      // a rank without the volume that contains a point can find it in a
      // surface or a curve close to it: the volumes come first, as without MPI
      evaluated.assign(point, -1);
      // and among them the element with the lowest number, as without MPI
      for(int i = 0; i < (int)points.size(); i++) {
        int &first = evaluated[points[i]];
        if(first < 0 || dims[i] > dims[first] ||
           (dims[i] == dims[first] && elements[i] < elements[first]))
          first = i;
      }
    }
#endif
    point = 0;

    switch(PSO_P->SubType) {
    case PRINT_ONGRID_0D:
      Current.x = PSO_P->Case.OnGrid.x[0];
      Current.y = PSO_P->Case.OnGrid.y[0];
      Current.z = PSO_P->Case.OnGrid.z[0];
      Normal[0] = Normal[1] = Normal[2] = 0.0;
      printPoint(0, 0);

      if(print && PSO_P->StoreInRegister >= 0)
        Cal_StoreInRegister(&PE->Value[0], PSO_P->StoreInRegister);
      if(print && PSO_P->StoreInVariable)
        Cal_StoreInVariable(&PE->Value[0], PSO_P->StoreInVariable);
      break;

    case PRINT_ONGRID_1D: Pos_PrintOnLine(PSO_P, Normal, printPoint); break;

    case PRINT_ONGRID_2D:
      if(!Pos_NormalOfPlane(PSO_P, Normal)) {
        Pos_SearchInPartitionOfThisRank(false);
        return;
      }
      if(PSO_P->Depth > 1 && print)
        Array = (float *)Malloc(
          NbTimeStep * Current.NbrHar *
          ((PSO_P->Case.OnGrid.n[0] + 1) * (PSO_P->Case.OnGrid.n[1] + 1)) *
          sizeof(float));
      Pos_PrintOnPlane(PSO_P, print, printPoint);
      if(PSO_P->Depth > 1 && print) {
        Pos_PrintOnPlaneTriangles(PSO_P, NbTimeStep, Array, Normal);
        Free(Array);
        Array = NULL;
      }
      break;

    case PRINT_ONGRID_3D:
      Pos_PrintOnBox(PSO_P, print, Normal, printPoint);
      break;

    case PRINT_ONGRID_PARAM:
      Pos_PrintOnParamGrid(PSO_P, print, Normal, printPoint);
      break;
    }
  }

  Pos_SearchInPartitionOfThisRank(false);

  // the value at a point, stored by the first rank, is sent to the others
  if(overRanks && PSO_P->SubType == PRINT_ONGRID_0D) {
    if(PSO_P->StoreInRegister >= 0)
      Cal_BroadcastValueInRegister(PSO_P->StoreInRegister, 0);
    if(PSO_P->StoreInVariable)
      Cal_BroadcastValueInVariable(PSO_P->StoreInVariable, 0);
  }

  Destroy_PostElement(PE);

  if(printHere) Format_PostFooter(PSO_P, 0);

  if(CPQ_P) Free(CumulativeValues);
}

#undef ARRAY

/* ------------------------------------------------------------------------ */
/*  P o s _ P r i n t O n R e g i o n                                       */
/* ------------------------------------------------------------------------ */

void Pos_PrintOnRegion(struct PostQuantity *NCPQ_P, struct PostQuantity *CPQ_P,
                       int Order, struct DefineQuantity *DefineQuantity_P0,
                       struct QuantityStorage *QuantityStorage_P0,
                       struct PostSubOperation *PSO_P)
{
  struct Element Element;
  struct Value Value, ValueSummed;
  struct PostQuantity *PQ_P;
  struct Group *Group_P;

  List_T *Region_L, *Support_L;
  int i, iTime, NbrTimeStep;
  int Nbr_Region = 0, Num_Region, Group_FunctionType;
  int Type_Evaluation = 0;
  double u, v, w;

  NbrTimeStep = Pos_InitTimeSteps(PSO_P);

  if(CPQ_P && NCPQ_P) {
    Message::Error("Only one PostProcessing Quantity allowed in PostOperation");
    return;
  }

  if(CPQ_P) {
    PQ_P = CPQ_P;
    /*
       If the PostQuantityTerm is an Integral quantity to be integrated
       over a support in this 'Print' PostOperation
       (i.e. syntax: PQ[ Support ]), the InitialList (list of regions) of the
       group 'Support' is affected to Support_L.
       If however the group 'Support' is of type ELEMENTLIST,
       the latter is substituted to the ->InIndex of the PostQuantityTerm here,
       i.e., just before evaluation.

       For consistency, it should be checked that all ellements
       in the ELEMENTLIST Support are indeed in the region
       PostQuantityTerm->InIndex. This is not done.
    */

    /* code original - enlever apres vérification FH
    Support_L = // for e.g. PQ[ Support ] ...
      ((struct Group *)
       List_Pointer(Problem_S.Group,
                    PSO_P->PostQuantitySupport[Order]))->InitialList ;
    */

    // FIXME reuse Group_P instead of definig a specific variable ?
    struct Group *SupportGroup_P = (struct Group *)List_Pointer(
      Problem_S.Group, PSO_P->PostQuantitySupport[Order]);

    Support_L = SupportGroup_P->InitialList; // for e.g. PQ[ Support ] ...

    if(SupportGroup_P->Type == ELEMENTLIST) {
      ((struct PostQuantityTerm *)List_Pointer(PQ_P->PostQuantityTerm, 0))
        ->InIndex = SupportGroup_P->Num;
      // FIXME What if PostQuantity has several PostQuantityTerm's ?
      // Here only the first term (index 0) is considered.
    }
  }
  else {
    PQ_P = NCPQ_P;
    Support_L = NULL;
  }

  Format_PostHeader(PSO_P, NbrTimeStep, Order,
                    PSO_P->Label ? PSO_P->Label :
                                   (NCPQ_P ? NCPQ_P->Name : NULL),
                    PSO_P->Label ? NULL : (CPQ_P ? CPQ_P->Name : NULL));

  Group_P = (PSO_P->Case.OnRegion.RegionIndex < 0) ?
              NULL :
              (struct Group *)List_Pointer(Problem_S.Group,
                                           PSO_P->Case.OnRegion.RegionIndex);

  Region_L = Group_P ? Group_P->InitialList : NULL;
  Group_FunctionType = Group_P ? Group_P->FunctionType : REGION;

  if(!Support_L && List_Nbr(NCPQ_P->PostQuantityTerm) &&
     (((struct PostQuantityTerm *)List_Pointer(NCPQ_P->PostQuantityTerm, 0))
          ->Type == LOCALQUANTITY &&
      ((struct PostQuantityTerm *)List_Pointer(NCPQ_P->PostQuantityTerm, 0))
          ->EvaluationType == LOCAL)) {
    if(Group_FunctionType == REGION) {
      Message::Error(
        "Print OnRegion not valid for PostProcessing Quantity '%s'",
        NCPQ_P->Name);
      return;
    }
    else
      Type_Evaluation = LOCAL;
  }
  else
    Type_Evaluation = GLOBAL;

  if(Region_L) {
    if(Group_P->FunctionType == REGION) {
      List_Sort(Region_L, fcmp_int);
      Nbr_Region = List_Nbr(Region_L);

      if(!PSO_P->NoTitle && PSO_P->Format != FORMAT_SPACE_TABLE &&
         PSO_P->Format != FORMAT_VALUE_ONLY && PSO_P->Format != FORMAT_GETDP) {
        std::ostringstream sstream;
        if(PSO_P->Format == FORMAT_GMSH)
          sstream << "// ";
        else
          sstream << "# ";
        sstream << (PSO_P->ValueName ? PSO_P->ValueName : PQ_P->Name) << " on";
        for(i = 0; i < Nbr_Region; i++) {
          List_Read(Region_L, i, &Num_Region);
          sstream << " " << Num_Region;
        }
        if(PostStream == stdout || PostStream == stderr)
          Message::Direct(sstream.str().c_str());
        else if(PostStream)
          fprintf(PostStream, "%s\n", sstream.str().c_str());
      }
      else if(!PSO_P->NoTitle && PSO_P->Format == FORMAT_VALUE_ONLY) {
        std::ostringstream sstream;
        sstream << (PSO_P->ValueName ? PSO_P->ValueName : PQ_P->Name);
        for(i = 0; i < Nbr_Region; i++) {
          List_Read(Region_L, i, &Num_Region);
          sstream << "_" << Num_Region;
        }
        sstream << PSO_P->Comma;
        if(PostStream) fprintf(PostStream, "%s", sstream.str().c_str());
      }
    }
    else if(Group_P->FunctionType == NODESOF) {
      if(!Group_P->ExtendedList) Generate_ExtendedGroup(Group_P);
      Region_L = Group_P->ExtendedList; /* Attention: new Region_L */
      Nbr_Region = List_Nbr(Region_L);
    }
    else {
      Message::Error("Function type (%d) not allowed for PrintOnRegion",
                     Group_P->FunctionType);
      return;
    }
  }
  else {
    Nbr_Region = 1;
    if(PostStream && !PSO_P->NoTitle && PSO_P->Format == FORMAT_VALUE_ONLY) {
      fprintf(PostStream, "%s%s", (PSO_P->ValueName ? PSO_P->ValueName :
                                   PQ_P->Name), PSO_P->Comma);
    }
  }

  for(iTime = 0; iTime < NbrTimeStep; iTime++) {
    Pos_InitAllSolutions(PSO_P->TimeStep_L, iTime);

    if(PSO_P->Format == FORMAT_REGION_VALUE ||
       PSO_P->Format == FORMAT_FREQUENCY_REGION_VALUE) {
      Cal_ZeroValue(&ValueSummed);
    }

    if(Nbr_Region > 1) Message::ResetProgressMeter();

    for(i = 0; i < Nbr_Region; i++) {
      if(Region_L)
        List_Read(Region_L, i, &Num_Region);
      else
        Num_Region = NO_REGION;
      Current.SubRegion =
        Num_Region; /* Region being a GlobalQuantity Entity no */

      Current.NumEntity = Num_Region; /* for OnRegion NodesOf */

      Element.GeoElement = NULL;
      Element.Num = NO_ELEMENT;
      Element.Type = -1;
      Current.Region = Element.Region = Num_Region;
      Current.x = Current.y = Current.z = 0.;

      if(Type_Evaluation == GLOBAL) {
        Cal_PostQuantity(PQ_P, DefineQuantity_P0, QuantityStorage_P0, PSO_P->SingleProcess,
                         Support_L, &Element, 0., 0., 0., &Value);
      }
      else {
        if(Group_FunctionType == NODESOF)
          Geo_GetNodesCoordinates(1, &Num_Region, &Current.x, &Current.y,
                                  &Current.z);
        InWhichElement(Current.GeoData, NULL, &Element, PSO_P->Dimension,
                       Current.x, Current.y, Current.z, &u, &v, &w);

        Cal_PostQuantity(PQ_P, DefineQuantity_P0, QuantityStorage_P0, PSO_P->SingleProcess,
                         Support_L, &Element, u, v, w, &Value);
      }

      if(PSO_P->Format != FORMAT_REGION_VALUE &&
         PSO_P->Format != FORMAT_FREQUENCY_REGION_VALUE) {
        if(PSO_P->StoreInRegister >= 0)
          Cal_StoreInRegister(&Value, PSO_P->StoreInRegister);
        if(PSO_P->StoreInVariable)
          Cal_StoreInVariable(&Value, PSO_P->StoreInVariable);
        if(PSO_P->SendToServer && strcmp(PSO_P->SendToServer, "No")) {
          std::vector<double> v;
          Export_Value(&Value, v, PSO_P->SendToServerList);
          Message::AddOnelabNumberChoice(PSO_P->SendToServer, v, PSO_P->Color,
                                         PSO_P->Units, PSO_P->Label,
                                         PSO_P->Visible, PSO_P->Closed);
        }
      }

      Format_PostValue(PQ_P, PSO_P, PSO_P->Format, PSO_P->Comma,
                       Group_FunctionType, iTime, Current.Time, NbrTimeStep, i,
                       Current.NumEntity, Nbr_Region, Current.NbrHar,
                       PSO_P->HarmonicToTime, PSO_P->FourierTransform,
                       PSO_P->NoNewLine, &Value);

      if(PSO_P->Format == FORMAT_REGION_VALUE ||
         PSO_P->Format == FORMAT_FREQUENCY_REGION_VALUE) {
        ValueSummed.Type = Value.Type;
        Cal_AddValue(&ValueSummed, &Value, &ValueSummed);
      }

      if(Nbr_Region > 1)
        Message::ProgressMeter(i + 1, Nbr_Region, "Post-processing (OnRegion)");
    }

    if(PostStream && (PSO_P->Format == FORMAT_REGION_VALUE ||
                      PSO_P->Format == FORMAT_FREQUENCY_REGION_VALUE)) {
      fprintf(PostStream, "%s", Print_Value_ToString(&ValueSummed).c_str());
      if(PSO_P->StoreInRegister >= 0)
        Cal_StoreInRegister(&ValueSummed, PSO_P->StoreInRegister);
      if(PSO_P->StoreInVariable)
        Cal_StoreInVariable(&ValueSummed, PSO_P->StoreInVariable);
      if(PSO_P->SendToServer && strcmp(PSO_P->SendToServer, "No")) {
        std::vector<double> v;
        Export_Value(&ValueSummed, v, PSO_P->SendToServerList);
        Message::AddOnelabNumberChoice(PSO_P->SendToServer, v, PSO_P->Color,
                                       PSO_P->Units, PSO_P->Label,
                                       PSO_P->Visible, PSO_P->Closed);
      }
    }
  }

  // prevent SendToServer here, as we have alredy done it
  Format_PostFooter(PSO_P, 0, false);
}

/* ------------------------------------------------------------------------ */
/*  P o s _ P r i n t W i t h A r g u m e n t                               */
/* ------------------------------------------------------------------------ */

void Pos_PrintWithArgument(struct PostQuantity *NCPQ_P,
                           struct PostQuantity *CPQ_P, int Order,
                           struct DefineQuantity *DefineQuantity_P0,
                           struct QuantityStorage *QuantityStorage_P0,
                           struct PostSubOperation *PSO_P)
{
  struct Element Element;
  struct Value Value;

  struct Expression *Expression_P;
  List_T *Region_L;
  int i, N, Num_Region;
  double X[2], S, x;

  if(CPQ_P) {
    Message::Error(
      "Cumulative PostProcessing Quantity in PrintWithArgument not done");
    return;
  }

  X[0] = PSO_P->Case.WithArgument.x[0];
  X[1] = PSO_P->Case.WithArgument.x[1];
  N = PSO_P->Case.WithArgument.n;

  Expression_P = (struct Expression *)List_Pointer(
    Problem_S.Expression, PSO_P->Case.WithArgument.ArgumentIndex);

  Region_L = ((struct Group *)List_Pointer(
                Problem_S.Group, PSO_P->Case.WithArgument.RegionIndex))
               ->InitialList;

  if(List_Nbr(Region_L))
    List_Read(Region_L, 0, &Num_Region);
  else
    Num_Region = NO_REGION;

  for(i = 0; i <= N; i++) {
    S = (double)i / (double)(N ? N : 1);
    x = X[0] + (X[1] - X[0]) * S;
    Expression_P->Case.Constant = x;

    Element.GeoElement = NULL;
    Element.Num = NO_ELEMENT;
    Element.Type = -1;
    Current.Region = Element.Region = Num_Region;
    Current.x = Current.y = Current.z = 0.;

    Cal_PostQuantity(NCPQ_P, DefineQuantity_P0, QuantityStorage_P0, PSO_P->SingleProcess,
                     NULL, &Element, 0., 0., 0., &Value);

    Format_PostValue(NCPQ_P, PSO_P, PSO_P->Format, PSO_P->Comma, REGION, 0, x,
                     1, 0, 0, 1, Current.NbrHar, PSO_P->HarmonicToTime,
                     PSO_P->FourierTransform, PSO_P->NoNewLine, &Value);
  }
}

/* ------------------------------------------------------------------------ */
/*  P o s _ P r i n t E x p r e s s i o n                                   */
/* ------------------------------------------------------------------------ */

void Pos_PrintExpression(struct PostSubOperation *PSO_P)
{
  int NbrTimeStep, iTime;
  struct Value Value;
  char *str = PSO_P->Case.Expression.String;
  char *str2 = PSO_P->Case.Expression.String2;
  List_T *expr = PSO_P->Case.Expression.Expressions;

  if((!str || !strlen(str)) && (!str2 || !strlen(str2)) && !List_Nbr(expr))
    return; // nothing to print; useful to request merging an existing file

  NbrTimeStep = Pos_InitTimeSteps(PSO_P);

  for(iTime = 0; iTime < NbrTimeStep; iTime++) {
    Pos_InitAllSolutions(PSO_P->TimeStep_L, iTime);
    if(List_Nbr(expr) &&
       str2) { // old style, unformatted single expession output
      int j;
      List_Read(expr, 0, &j);
      Get_ValueOfExpressionByIndex(j, NULL, 0., 0., 0., &Value);
      if(PostStream) {
        if(str) fprintf(PostStream, "%s", str);
        fprintf(PostStream, "%s", Print_Value_ToString(&Value).c_str());
      }
    }
    else if(List_Nbr(expr) &&
            str) { // new style, same syntax as in resolution operations
      List_T *list = List_Create(10, 10, sizeof(double));
      for(int i = 0; i < List_Nbr(expr); i++) {
        int j;
        List_Read(expr, i, &j);
        Get_ValueOfExpressionByIndex(j, NULL, 0., 0., 0., &Value);
        List_Add(list, &Value.Val[0]);
      }
      std::string buffer;
      Print_ListOfDouble(str, list, buffer);
      if(PostStream) fprintf(PostStream, "%s", buffer.c_str());
      List_Delete(list);
    }
    else if(str2) {
      if(PostStream) {
        if(str) fprintf(PostStream, "%s", str);
        fprintf(PostStream, "%s", str2);
      }
    }
    else if(str) {
      if(PostStream) fprintf(PostStream, "%s", str);
    }

    if(PostStream) {
      if(PSO_P->NoNewLine)
        fprintf(PostStream, " ");
      else
        fprintf(PostStream, "\n");
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  P o s _ P r i n t G r o u p                                             */
/* ------------------------------------------------------------------------ */

void Pos_PrintGroup(struct PostSubOperation *PSO_P)
{
  struct Group *Group_P;
  struct Geo_Element *GeoElement;
  struct PostElement *SL, *ST, *SQ;
  List_T *Region_L;
  int i, NbrGeo, iGeo, *NumNodes;
  double x[NBR_MAX_NODES_IN_ELEMENT];
  double y[NBR_MAX_NODES_IN_ELEMENT];
  double z[NBR_MAX_NODES_IN_ELEMENT];

  NbrGeo = Geo_GetNbrGeoElements();

  Format_PostHeader(PSO_P, 1, 0, PSO_P->Label, NULL);

  Region_L = ((struct Group *)List_Pointer(Problem_S.Group,
                                           PSO_P->Case.Group.GroupIndex))
               ->InitialList;
  Group_P = (struct Group *)List_Pointer(Problem_S.Group,
                                         PSO_P->Case.Group.ExtendedGroupIndex);

  SL = Create_PostElement(0, LINE, 2, 1);
  ST = Create_PostElement(0, TRIANGLE, 3, 1);
  SQ = Create_PostElement(0, QUADRANGLE, 4, 1);

  if(!Group_P->ExtendedList) Generate_ExtendedGroup(Group_P);

  Message::ResetProgressMeter();
  for(iGeo = 0; iGeo < NbrGeo; iGeo++) {
    GeoElement = Geo_GetGeoElement(iGeo);
    if(List_Search(Region_L, &GeoElement->Region, fcmp_int)) {
      Geo_GetNodesCoordinates(GeoElement->NbrNodes, GeoElement->NumNodes, x, y,
                              z);

      switch(Group_P->FunctionType) {
      case EDGESOF:
      case EDGESOFTREEIN:
        if(!GeoElement->NbrEdges) Geo_CreateEdgesOfElement(GeoElement);
        for(i = 0; i < GeoElement->NbrEdges; i++) {
          if(List_Search(Group_P->ExtendedList, &GeoElement->NumEdges[i],
                         fcmp_absint)) {
            NumNodes = Geo_GetNodesOfEdgeInElement(GeoElement, i);
            SL->Index = iGeo;
            SL->x[0] = x[abs(NumNodes[0]) - 1];
            SL->x[1] = x[abs(NumNodes[1]) - 1];
            SL->y[0] = y[abs(NumNodes[0]) - 1];
            SL->y[1] = y[abs(NumNodes[1]) - 1];
            SL->z[0] = z[abs(NumNodes[0]) - 1];
            SL->z[1] = z[abs(NumNodes[1]) - 1];
            SL->Value[0].Type = SL->Value[1].Type = SCALAR;
            SL->Value[0].Val[0] = SL->Value[1].Val[0] = GeoElement->NumEdges[i];
            Format_PostElement(PSO_P, PSO_P->Iso, 0, 0, 0, 1, 1, 1, NULL, SL);
          }
        }
        break;

      case FACETSOFTREEIN:
        if(!GeoElement->NbrFacets) Geo_CreateFacetsOfElement(GeoElement);
        for(i = 0; i < GeoElement->NbrFacets; i++) {
          if(List_Search(Group_P->ExtendedList, &GeoElement->NumFacets[i],
                         fcmp_absint)) {
            NumNodes = Geo_GetNodesOfFacetInElement(GeoElement, i);
            if(!NumNodes[3]) { // we have triangle
              ST->Index = iGeo;
              ST->x[0] = x[abs(NumNodes[0]) - 1];
              ST->x[1] = x[abs(NumNodes[1]) - 1];
              ST->y[0] = y[abs(NumNodes[0]) - 1];
              ST->y[1] = y[abs(NumNodes[1]) - 1];
              ST->z[0] = z[abs(NumNodes[0]) - 1];
              ST->z[1] = z[abs(NumNodes[1]) - 1];
              ST->x[2] = x[abs(NumNodes[2]) - 1];
              ST->y[2] = y[abs(NumNodes[2]) - 1];
              ST->z[2] = z[abs(NumNodes[2]) - 1];
              ST->Value[0].Type = ST->Value[1].Type = ST->Value[2].Type =
                SCALAR;
              ST->Value[0].Val[0] = ST->Value[1].Val[0] = ST->Value[2].Val[0] =
                GeoElement->NumFacets[i];
              Format_PostElement(PSO_P, PSO_P->Iso, 0, 0, 0, 1, 1, 1, NULL, ST);
            }
            else { // we have a quad
              SQ->Index = iGeo;
              SQ->x[0] = x[abs(NumNodes[0]) - 1];
              SQ->x[1] = x[abs(NumNodes[1]) - 1];
              SQ->y[0] = y[abs(NumNodes[0]) - 1];
              SQ->y[1] = y[abs(NumNodes[1]) - 1];
              SQ->z[0] = z[abs(NumNodes[0]) - 1];
              SQ->z[1] = z[abs(NumNodes[1]) - 1];
              SQ->x[2] = x[abs(NumNodes[2]) - 1];
              SQ->x[3] = x[abs(NumNodes[3]) - 1];
              SQ->y[2] = y[abs(NumNodes[2]) - 1];
              SQ->y[3] = y[abs(NumNodes[3]) - 1];
              SQ->z[2] = z[abs(NumNodes[2]) - 1];
              SQ->z[3] = z[abs(NumNodes[3]) - 1];
              SQ->Value[0].Type = SQ->Value[1].Type = SQ->Value[2].Type =
                SQ->Value[3].Type = SCALAR;
              SQ->Value[0].Val[0] = SQ->Value[1].Val[0] = SQ->Value[2].Val[0] =
                SQ->Value[3].Val[0] = GeoElement->NumFacets[i];
              Format_PostElement(PSO_P, PSO_P->Iso, 0, 0, 0, 1, 1, 1, NULL, SQ);
            }
          }
        }
        break;

      default:
        Message::Error("Print function not implemented for this kind of Group");
        break;
      }
    }
    Message::ProgressMeter(iGeo + 1, NbrGeo, "Post-processing (Compute)");
    if(Message::GetErrorCount()) break;
  }

  Destroy_PostElement(SL);

  Format_PostFooter(PSO_P, 0);
}
