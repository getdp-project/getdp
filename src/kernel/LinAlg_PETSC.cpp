// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//   David Colignon
//   Ruth Sabariego
//   Jose Geraldo A. Brito Neto
//

#include <climits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <complex>
#include <string>
#include <cstring>
#include <sstream>
#include <stdio.h>
#include "GetDPConfig.h"
#include "LinAlg.h"
#include "SparseRows.h"
#include "MallocUtils.h"
#include "Message.h"
#include "OS.h"

#if defined(HAVE_SLEPC)
#include <slepc.h>
#endif

// FIXME: this dependency should be removed
#include "ProData.h"
#include "DofData.h"
#include "GeoData.h"
#include "TreeUtils.h"
#include "ExtendedGroup.h"
extern struct Problem Problem_S;
extern struct CurrentData Current;

#if defined(HAVE_PETSC)

// Options for PETSc can be provided on the command line, or in the file
// ~/.petscrc.
//
// By default we use a direct solver (MUMPS, UMFPACK or the PETSc LU).
//
// All these options can be changed at runtime. For example you could
// use
//
//   -pc_type ilu
//   -pc_factor_levels 0
//   -ksp_type gmres
//   -ksp_rtol 1.e-6
//   -ksp_gmres_restart 500
//   -ksp_monitor
//
// for GMRES with ILU(0), with a restart of 500 and a stopping
// criterion of 1e-6.

static MPI_Comm MyComm = PETSC_COMM_SELF;
static PetscViewer MyPetscViewer;

static void _try(int ierr)
{
  CHKERRCONTINUE(ierr);
  if(PetscUnlikely(ierr)) {
    const char *text;
    PetscErrorMessage(ierr, &text, 0);
    // Do not produce an error in case of a PETSc-crash when we are in
    // TimeLoopAdaptive loop
    if(Message::GetOperatingInTimeLoopAdaptive())
      Message::Warning("PETSc error: %s", text);
    else
      Message::Error("PETSc error: %s", text);
    Message::SetLastPETScError(ierr);
  }
}

static int SolverInitialized = 0;

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 7)
#define PetscTruth PetscBool
#define PetscOptionsGetTruth(A, B, C, D) PetscOptionsGetBool(A, NULL, B, C, D)
#define PetscOptionsInsertFile(A, B, C) PetscOptionsInsertFile(A, NULL, B, C)
#define PetscOptionsGetInt(A, B, C, D) PetscOptionsGetInt(NULL, A, B, C, D);
#define PetscOptionsSetValue(A, B) PetscOptionsSetValue(NULL, A, B)
#define PetscOptionsInsertString(A) PetscOptionsInsertString(NULL, A)
#define PetscViewerSetFormat(A, B) PetscViewerPushFormat(A, B)
#elif((PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2))
#define PetscTruth PetscBool
#define PetscOptionsGetTruth PetscOptionsGetBool
#endif

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 9)
#define PCFactorSetMatSolverPackage PCFactorSetMatSolverType
#endif

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR > 19)
#undef PETSC_NULL
#define PETSC_NULL PETSC_NULLPTR
#endif

void LinAlg_InitializeSolver(int *argc, char ***argv)
{
  if(SolverInitialized) return;
  SolverInitialized = 1;

  // This function detects if MPI is initialized
  PetscInitialize(argc, argv, PETSC_NULL, PETSC_NULL);
  PetscPopSignalHandler();
  MyPetscViewer = PETSC_VIEWER_STDOUT_SELF;
  MyComm = PETSC_COMM_WORLD;
#if defined(HAVE_SLEPC)
  SlepcInitialize(argc, argv, PETSC_NULL, PETSC_NULL);
#endif

  // get additional petsc options from specified file (useful e.g. on
  // Windows where we don't know where to search for ~/.petscrc)
  for(int i = 0; i < *argc - 1; i++) {
    if(!strcmp((*argv)[i], "-solver")) {
#if(PETSC_VERSION_MAJOR == 2)
      PetscOptionsInsertFile((*argv)[i + 1]);
#else
      PetscOptionsInsertFile(MyComm, (*argv)[i + 1], PETSC_TRUE);
#endif
    }
  }
}

static void _clearGhosts();

void LinAlg_FinalizeSolver()
{
  if(SolverInitialized) {
    _clearGhosts();
#if defined(HAVE_SLEPC)
    SlepcFinalize();
#endif
    PetscFinalize();
    SolverInitialized = 0;
  }
}

void LinAlg_SetCommSelf()
{
  Message::Info("Set communicator to SELF");
  MyComm = PETSC_COMM_SELF;
  Message::SetIsCommWorld(0);
}

void LinAlg_SetCommWorld()
{
  Message::Info("Set communicator to WORLD");
  MyComm = PETSC_COMM_WORLD;
  Message::SetIsCommWorld(1);
}

void LinAlg_CreateSolver(gSolver *Solver, const char *SolverDataFileName)
{
  for(int i = 0; i < 10; i++) {
    Solver->ksp[i] = NULL;
    Solver->snes[i] = NULL;
  }
}

// whether each rank works on its own: on a single rank, or after SetCommSelf
static bool _isSequential()
{
  return Message::GetCommSize() == 1 || MyComm == PETSC_COMM_SELF;
}

static bool _usePartitions()
{
  if(_isSequential()) return false;

  if((int)Current.DofData->PartitionSplit.size() == Message::GetCommSize() + 1)
    return true; // local equations only

  if((int)Current.DofData->PartitionSplit.size() == Message::GetCommSize() + 2)
    return true; // local equations + non-partitioned/global equations

  return false; // partitioning does not match the number of ranks
}

static int _getLocalSize(int n)
{
  int s = PETSC_DECIDE;

  if(_usePartitions()) {
    if(*Current.DofData->PartitionSplit.rbegin() != n) {
      // the partition split of Current.DofData describes a system with a
      // different number of DoFs than the object being created; using its
      // local sizes would silently produce an inconsistent parallel layout
      // (PETSc only checks that the local sizes sum to the global size in
      // debug builds)
      Message::Warning("Partition split total %d of current system does not "
                       "match requested global size %d - using default layout",
                       (int)*Current.DofData->PartitionSplit.rbegin(), n);
      return s;
    }
    int r = Message::GetCommRank();
    if(r == Message::GetCommSize() - 1) {
      // add any non-partitioned/global equations in the last partition
      s = *Current.DofData->PartitionSplit.rbegin() -
        Current.DofData->PartitionSplit[r];
    }
    else {
      s = Current.DofData->PartitionSplit[r + 1] -
        Current.DofData->PartitionSplit[r];
    }
    Message::Debug("localsize = %d", s);
  }
  return s;
}

// whether row i of a vector or of a matrix is owned by this rank
static bool _isInLocalRange(gVector *V, int i)
{
  if(Message::GetCommSize() == 1) return true;
  int imin, imax;
  LinAlg_GetLocalVectorRange(V, &imin, &imax);
  return (i >= imin && i < imax);
}

static bool _isInLocalRange(gMatrix *M, int i)
{
  if(Message::GetCommSize() == 1) return true;
  int imin, imax;
  LinAlg_GetLocalMatrixRange(M, &imin, &imax);
  return (i >= imin && i < imax);
}

// In parallel the element loop adds all the entries of the elements of the rank,
// whoever owns their row (see Dof_IsPartitionOfThisRank); the other terms (e.g.
// global terms), which every rank computes, only add the entries of the rows
// the rank owns
template <class T> static bool _addsRow(T *MV, int i)
{
  return Current.AddAllRows || _isInLocalRange(MV, i);
}

// The matrices are assembled in a storage of our own (see SparseRows.h), and
// given to PETSc when they are assembled (LinAlg_AssembleMatrix). The element
// loop adds the entries of the elementary matrices one at a time: giving each
// of them to PETSc (MatSetValues) costs a search in the PETSc row for each
// entry, and PETSc needs the number of nonzeros of each row before the first
// entry is added, not to reallocate the matrix again and again. With the
// storage the nonzero pattern is built on the fly by each assembly, the PETSc
// matrix is preallocated exactly, and its rows are set in one call each.
//
// The rows of the non-local equations (global quantities, e.g. circuit
// unknowns coupled to all the edges of a conductor) can have millions of
// entries, which the element loop adds a few at a time in mesh order. Inserted
// directly, each new column shifts the rest of the row in the storage, so
// filling such a row costs its length squared. Their contributions are thus
// accumulated apart, sorted by column, and added to the storage when the
// matrix is assembled.
//
// In parallel the rows of the storage that belong to another rank and the
// non-local rows are first sent to their owner.
typedef SparseRows<PetscScalar, PetscInt> _Storage;

struct _Assembly {
  _Storage rows;
  std::unordered_map<int, std::map<int, PetscScalar> > nonLocalRows;
  bool isNew = false; // the PETSc matrix was just created: preallocate it
};

static std::map<Mat, _Assembly> _assembly;
static Mat _lastMat = PETSC_NULL;
static _Assembly *_lastAssembly = nullptr;

static _Assembly &_getAssembly(Mat M)
{
  if(M == _lastMat) return *_lastAssembly;
  auto it = _assembly.find(M);
  if(it == _assembly.end()) {
    PetscInt first, last;
    _try(MatGetOwnershipRange(M, &first, &last));
    it = _assembly.insert(std::make_pair(M, _Assembly())).first;
    it->second.rows = _Storage(first, last);
  }
  _lastMat = M;
  _lastAssembly = &it->second;
  return it->second;
}

static void _dropAssembly(Mat M)
{
  _assembly.erase(M);
  _lastMat = PETSC_NULL;
}

static void _addInMatrix(gMatrix *M, int i, int j, PetscScalar v)
{
  if(!Current.DofData->NonLocalEquations.empty() && Dof_IsNonLocalEquation(i))
    _getAssembly(M->M).nonLocalRows[i][j] += v;
  else
    _getAssembly(M->M).rows.add(i, j, v);
}

// the rank that owns row i, given the ownership ranges of the size ranks
static int _ownerOf(const PetscInt *ranges, int size, PetscInt i)
{
  return (int)(std::upper_bound(ranges, ranges + size + 1, i) - ranges - 1);
}

static void _setRow(Mat M, PetscInt i, _Storage::Row &r)
{
  if(r.cols.empty()) return;
  _try(MatSetValues(M, 1, &i, (PetscInt)r.cols.size(), r.cols.data(),
                    r.vals.data(), ADD_VALUES));
}

// Collective: sends the entries of the rows owned by other ranks (the rows of
// the storage that belong to other ranks and the non-local rows) to their
// owners, and adds the non-local rows owned by this rank to the storage. The
// non-local rows are gathered on their owner before they are added, sorted by
// column, since adding the pieces of such a row one after the other would
// shift the rest of the row for each new column.
static void _gatherRows(Mat M)
{
  MPI_Comm comm = PetscObjectComm((PetscObject)M);
  int size, rank;
  MPI_Comm_size(comm, &size);
  MPI_Comm_rank(comm, &rank);
  _Storage &S = _getAssembly(M).rows;
  auto &rows = _getAssembly(M).nonLocalRows;
  // a matrix on several ranks has the communicator of all the ranks, on which
  // Message::Exchange communicates
  if(size > 1) {
    const PetscInt *ranges;
    _try(MatGetOwnershipRanges(M, &ranges));
    // for each rank, the row and the column of each entry, and its value
    std::vector<std::vector<PetscInt> > idx(size), ridx;
    std::vector<std::vector<PetscScalar> > val(size), rval;
    for(auto it = rows.begin(); it != rows.end();) {
      int owner = _ownerOf(ranges, size, it->first);
      if(owner == rank) {
        ++it;
        continue;
      }
      for(auto &e : it->second) {
        idx[owner].push_back(it->first);
        idx[owner].push_back(e.first);
        val[owner].push_back(e.second);
      }
      it = rows.erase(it);
    }
    for(auto &row : S.others()) {
      int owner = _ownerOf(ranges, size, row.first);
      for(std::size_t k = 0; k < row.second.cols.size(); k++) {
        idx[owner].push_back(row.first);
        idx[owner].push_back(row.second.cols[k]);
        val[owner].push_back(row.second.vals[k]);
      }
    }
    S.others().clear();
    Message::Exchange(idx, ridx);
    Message::Exchange(val, rval);
    for(int r = 0; r < size; r++)
      for(std::size_t k = 0; k < rval[r].size(); k++)
        rows[ridx[r][2 * k]][ridx[r][2 * k + 1]] += rval[r][k];
  }
  for(auto &row : rows)
    for(auto &e : row.second) S.add(row.first, e.first, e.second);
  std::unordered_map<int, std::map<int, PetscScalar> >().swap(rows);
}

// Preallocates a PETSc matrix that was just created for the rows of the storage
static void _preallocate(Mat M, _Storage &S)
{
  PetscMPIInt size;
  MPI_Comm_size(PetscObjectComm((PetscObject)M), &size);
  PetscInt nr, nc, cfirst, clast;
  _try(MatGetSize(M, &nr, &nc));
  _try(MatGetOwnershipRangeColumn(M, &cfirst, &clast));
  std::vector<PetscInt> dnnz(S.last() - S.first() + 1), onnz(dnnz.size());
  for(PetscInt i = S.first(); i < S.last(); i++) {
    // PETSc (I)LU does not like matrices with empty diagonals
    if(size == 1 && i < nc) S.add(i, i, 0.);
    for(PetscInt j : S.row(i).cols) {
      if(j >= cfirst && j < clast)
        dnnz[i - S.first()]++;
      else
        onnz[i - S.first()]++;
    }
  }
  _try(MatXAIJSetPreallocation(M, 1, dnnz.data(), onnz.data(), PETSC_NULL,
                               PETSC_NULL));
  // the pattern of the matrix can still change, e.g. if a matrix is added to it
  _try(MatSetOption(M, MAT_NEW_NONZERO_ALLOCATION_ERR, PETSC_FALSE));
}

// Gives the entries added to the storage of M since the last hand-over to the
// PETSc matrix (collective).
static void _handOver(Mat M)
{
  _Assembly &A = _getAssembly(M);
  _gatherRows(M);
  if(A.isNew) _preallocate(M, A.rows);
  A.isNew = false;
  for(PetscInt i = A.rows.first(); i < A.rows.last(); i++)
    _setRow(M, i, A.rows.row(i));
  A.rows.clear();
}

// The element loop also adds the entries of the right-hand side one at a time,
// several times for most of them. Between LinAlg_StartElementCache() and
// LinAlg_FlushElementCache(), i.e. while the terms of one element are
// assembled, they are summed here by vector and added with one call per vector
// by the flush. Outside these calls they are added directly.
static bool _elementCache = false;

// beyond this size a vector buffer is no longer searched for an existing entry
// (duplicates are summed by PETSc)
static const std::size_t _maxSearch = 128;

struct _VecBuf {
  Vec V = PETSC_NULL;
  std::size_t last = 0;
  std::vector<PetscInt> rows;
  std::vector<PetscScalar> vals;
};

static std::vector<_VecBuf> _vecBufs;

static void _flushVecBufs()
{
  for(auto &b : _vecBufs) {
    if(!b.rows.empty())
      _try(VecSetValues(b.V, (PetscInt)b.rows.size(), b.rows.data(),
                        b.vals.data(), ADD_VALUES));
    b.rows.clear();
    b.vals.clear();
    b.last = 0;
  }
}

// forget the entries accumulated for vector V (if V is destroyed or zeroed)
static void _dropVecBuf(Vec V)
{
  for(std::size_t k = 0; k < _vecBufs.size(); k++) {
    if(_vecBufs[k].V == V) {
      _vecBufs.erase(_vecBufs.begin() + k);
      return;
    }
  }
}

static void _addInVector(Vec V, PetscInt i, PetscScalar v)
{
  if(!_elementCache) {
    _try(VecSetValues(V, 1, &i, &v, ADD_VALUES));
    return;
  }
  static std::size_t lastBuf = 0;
  if(lastBuf >= _vecBufs.size() || _vecBufs[lastBuf].V != V) {
    for(lastBuf = 0; lastBuf < _vecBufs.size(); lastBuf++)
      if(_vecBufs[lastBuf].V == V) break;
    if(lastBuf == _vecBufs.size()) {
      _vecBufs.emplace_back();
      _vecBufs.back().V = V;
    }
  }
  _VecBuf &b = _vecBufs[lastBuf];
  std::size_t n = b.rows.size(), k = b.last;
  if(k < n && b.rows[k] == i) {
    b.vals[k] += v;
    return;
  }
  if(++k < n && b.rows[k] == i) {
    b.vals[k] += v;
    b.last = k;
    return;
  }
  if(n <= _maxSearch) {
    for(k = 0; k < n; k++) {
      if(b.rows[k] == i) {
        b.vals[k] += v;
        b.last = k;
        return;
      }
    }
  }
  b.rows.push_back(i);
  b.vals.push_back(v);
  b.last = n;
}

// The element loop also reads the solution vectors one entry at a time: while
// the element cache is active the arrays of the vectors that are read stay
// open, instead of being requested and released for each entry
static std::vector<std::pair<Vec, const PetscScalar *> > _readArrays;

static const PetscScalar *_readArray(Vec V)
{
  const PetscScalar *a;
  if(_elementCache) {
    for(auto &r : _readArrays)
      if(r.first == V) return r.second;
    _try(VecGetArrayRead(V, &a));
    _readArrays.push_back(std::make_pair(V, a));
    return a;
  }
  _try(VecGetArrayRead(V, &a));
  return a;
}

static void _doneArray(Vec V, const PetscScalar *a)
{
  if(!_elementCache) _try(VecRestoreArrayRead(V, &a));
}

static void _closeReadArrays()
{
  for(auto &r : _readArrays) _try(VecRestoreArrayRead(r.first, &r.second));
  _readArrays.clear();
}

void LinAlg_StartElementCache() { _elementCache = true; }

void LinAlg_FlushElementCache()
{
  _flushVecBufs();
  _closeReadArrays();
  _elementCache = false;
}

// A distributed vector is read one entry at a time, by the number of an
// unknown, by the ranks that have this unknown among their Dofs. A rank owns a
// range of entries, stored in V; the entries of the other ranks that it reads,
// its "ghosts", are copied in the sequential vector Vseq. The ghosts of a rank
// are the unknowns of its Dofs that it does not own: all the others if it has
// all the Dofs (the first rank when it evaluates Prints alone, all the ranks
// with -all-dofs), otherwise those on the boundary of its part of the mesh.
struct gGhosts {
  PetscInt lo, hi; // range of entries owned by the rank
  bool all; // all the other entries are ghosts, in increasing order
  std::vector<PetscInt> num; // otherwise their sorted numbers
  VecScatter scatter; // copies the ghosts from V to Vseq
  int users; // number of vectors that use it (plus one if it is the current one)
  std::vector<PetscInt> key; // what it was built for (see _getGhosts)
};

// the ghosts of the vectors of each system
static std::map<struct DofData *, gGhosts *> _ghosts;

static void _releaseGhosts(gGhosts *g)
{
  if(--g->users) return;
  if(g->scatter) _try(VecScatterDestroy(&g->scatter));
  delete g;
}

static void _clearGhosts()
{
  for(auto &g : _ghosts) _releaseGhosts(g.second);
  _ghosts.clear();
}

// Returns the ghosts of a vector V of the current system (Current.DofData).
// They are computed again when the Dofs of a rank have changed, by all the
// ranks, as they share the scatter.
static gGhosts *_getGhosts(Vec V)
{
  struct DofData *D = Current.DofData;
  PetscInt n, lo, hi;
  _try(VecGetSize(V, &n));
  _try(VecGetOwnershipRange(V, &lo, &hi));
  List_T *dofs = D->DofList;
  if(!dofs && D->DofTree) dofs = Tree2List(D->DofTree);
  std::vector<PetscInt> key = {n, lo, hi, (PetscInt)List_Nbr(dofs)};
  auto it = _ghosts.find(D);
  int same = (it != _ghosts.end() && it->second->key == key), all = same;
  MPI_Allreduce(&same, &all, 1, MPI_INT, MPI_MIN, PetscObjectComm((PetscObject)V));
  if(!all) {
    if(it != _ghosts.end()) _releaseGhosts(it->second);
    gGhosts *g = new gGhosts;
    g->lo = lo;
    g->hi = hi;
    g->scatter = NULL;
    g->users = 1;
    g->key = key;
    // a vector that is not indexed by the unknowns of the system has them all
    g->all = (D->NbrDof != n || !dofs);
    for(int i = 0; !g->all && i < List_Nbr(dofs); i++) {
      struct Dof *d = (struct Dof *)List_Pointer(dofs, i);
      if(d->Type != DOF_UNKNOWN && d->Type != DOF_UNKNOWN_INIT &&
         d->Type != DOF_FIXEDWITHASSOCIATE &&
         d->Type != DOF_FIXEDWITHASSOCIATE_SOLVE)
        continue;
      PetscInt num = d->Case.Unknown.NumDof - 1;
      if(num >= 0 && num < n && (num < lo || num >= hi)) g->num.push_back(num);
    }
    std::sort(g->num.begin(), g->num.end());
    g->num.erase(std::unique(g->num.begin(), g->num.end()), g->num.end());
    if((PetscInt)g->num.size() == n - (hi - lo)) g->all = true;
    if(g->all) g->num.clear();
    Message::Debug("Vectors of %d entries: %d owned and %d from other ranks",
                   (int)n, (int)(hi - lo), (int)(g->all ? n - (hi - lo) : g->num.size()));
    _ghosts[D] = g;
  }
  if(dofs != D->DofList) List_Delete(dofs);
  gGhosts *g = _ghosts[D];
  g->users++;
  return g;
}

static PetscInt _getNumGhosts(Vec V, const gGhosts *g)
{
  if(!g->all) return g->num.size();
  PetscInt n;
  _try(VecGetSize(V, &n));
  return n - (g->hi - g->lo);
}

// the vectors with ghosts, identified by Vseq (see _fillseq below)
static long _seqCount = 0;
static std::map<Vec, long> _seqNum; // Vseq -> creation number
static std::map<long, gVector> _seqStale; // number -> vector

void LinAlg_CreateVector(gVector *V, gSolver *Solver, int n, bool sequential)
{
  V->Vseq = NULL;
  V->G = NULL;

  if(sequential) {
    _try(VecCreateSeq(PETSC_COMM_SELF, n, &V->V));
    return;
  }

  _try(VecCreate(MyComm, &V->V));
  _try(VecSetSizes(V->V, _getLocalSize(n), n));

  // override the default options with the ones from the option
  // database (if any)
  _try(VecSetFromOptions(V->V));

  if(!_isSequential()) {
    V->G = _getGhosts(V->V);
    _try(VecCreateSeq(PETSC_COMM_SELF, _getNumGhosts(V->V, V->G), &V->Vseq));
    _seqNum[V->Vseq] = _seqCount++;
  }
}

// Copies the ghosts of a vector from the ranks that own them.
static void _fillseq(gVector *V)
{
  gGhosts *g = V->G;
  if(!g) return;
  if(!g->scatter) {
    IS is;
    if(g->all) {
      PetscInt n = _getNumGhosts(V->V, g);
      std::vector<PetscInt> num(n);
      for(PetscInt i = 0; i < n; i++) num[i] = (i < g->lo) ? i : i + g->hi - g->lo;
      _try(ISCreateGeneral(PETSC_COMM_SELF, n, num.data(), PETSC_COPY_VALUES, &is));
    }
    else
      _try(ISCreateGeneral(PETSC_COMM_SELF, g->num.size(), g->num.data(),
                           PETSC_COPY_VALUES, &is));
    _try(VecScatterCreate(V->V, is, V->Vseq, NULL, &g->scatter));
    _try(ISDestroy(&is));
  }
  _try(VecScatterBegin(g->scatter, V->V, V->Vseq, INSERT_VALUES, SCATTER_FORWARD));
  _try(VecScatterEnd(g->scatter, V->V, V->Vseq, INSERT_VALUES, SCATTER_FORWARD));
  auto it = _seqNum.find(V->Vseq);
  if(it != _seqNum.end()) _seqStale.erase(it->second);
}

// The ghosts of a vector are copied at once after an assembly or a solve, but
// only marked as out of date after the arithmetic operations:
// LinAlg_UpdateGhosts(), called by all ranks at the start of each
// resolution operation, then copies all the stale ones. (The copy cannot be
// done when a ghost is read, as the reading functions need not be called on
// all ranks.) gVectors are copied by value, so the vectors are identified by
// their Vseq, and updated in the order of their creation, which is the same on
// all ranks.
static bool _isStaleSeq(gVector *V)
{
  auto it = _seqNum.find(V->Vseq);
  return it != _seqNum.end() && _seqStale.count(it->second);
}

static void _setStaleSeq(gVector *V, bool stale)
{
  auto it = _seqNum.find(V->Vseq);
  if(it == _seqNum.end()) return;
  if(stale)
    _seqStale[it->second] = *V;
  else
    _seqStale.erase(it->second);
}

static void _markseq(gVector *V)
{
  if(V->G) _setStaleSeq(V, true);
}

void LinAlg_UpdateGhosts()
{
  std::map<long, gVector> stale = _seqStale;
  for(auto &s : stale) _fillseq(&s.second);
  _seqStale.clear();
}

// Returns the entry i of a vector, owned by the rank or one of its ghosts.
static PetscScalar _getValue(gVector *V, PetscInt i)
{
  Vec VV = V->V;
  const gGhosts *g = V->G;
  if(g) {
    if(i >= g->lo && i < g->hi)
      i -= g->lo;
    else {
      VV = V->Vseq;
      if(g->all) {
        if(i >= g->hi) i -= g->hi - g->lo;
      }
      else {
        auto it = std::lower_bound(g->num.begin(), g->num.end(), i);
        if(it == g->num.end() || *it != i) {
          Message::Error("The value of unknown %d is not available on rank %d "
                         "(use -all-dofs)", (int)i + 1, Message::GetCommRank());
          return 0.;
        }
        i = it - g->num.begin();
      }
    }
  }
  const PetscScalar *a = _readArray(VV);
  PetscScalar s = a[i];
  _doneArray(VV, a);
  return s;
}

// Gets all the entries of a vector on the first rank, where it returns true.
// All the ranks have to call it for a distributed vector.
static bool _gather(gVector *V, std::vector<PetscScalar> &all)
{
  PetscInt n, nloc;
  _try(VecGetSize(V->V, &n));
  _try(VecGetLocalSize(V->V, &nloc));
  all.resize(n);
  const PetscScalar *a;
  if(!V->G) {
    _try(VecGetArrayRead(V->V, &a));
    std::copy(a, a + n, all.begin());
    _try(VecRestoreArrayRead(V->V, &a));
    return true;
  }
  if(MyComm == PETSC_COMM_SELF) {
    // the ranks work on their own (SetCommSelf): each one has all the entries
    for(PetscInt i = 0; i < n; i++) all[i] = _getValue(V, i);
    return true;
  }
  MPI_Comm comm = PetscObjectComm((PetscObject)V->V);
  PetscMPIInt np;
  MPI_Comm_size(comm, &np);
  const PetscInt *ranges;
  _try(VecGetOwnershipRanges(V->V, &ranges));
  std::vector<int> counts(np), displs(np);
  for(int r = 0; r < np; r++) {
    counts[r] = ranges[r + 1] - ranges[r];
    displs[r] = ranges[r];
  }
  _try(VecGetArrayRead(V->V, &a));
  MPI_Gatherv(a, nloc, MPIU_SCALAR, all.data(), counts.data(), displs.data(),
              MPIU_SCALAR, 0, comm);
  _try(VecRestoreArrayRead(V->V, &a));
  if(!Message::GetCommRank()) return true;
  all.clear();
  return false;
}

void LinAlg_GetVectorOnFirstRank(gVector *V, std::vector<gScalar> &values)
{
  std::vector<PetscScalar> all;
  values.clear();
  if(!_gather(V, all)) return;
  values.resize(all.size());
  for(std::size_t i = 0; i < all.size(); i++) values[i].s = all[i];
}

void LinAlg_CreateMatrix(gMatrix *M, gSolver *Solver, int n, int m)
{
  Message::Debug("Creating matrix with MPI_CommSize=%d and MPI_IsCommWorld=%d",
                 Message::GetCommSize(), Message::GetIsCommWorld());

  // the matrix is preallocated when it is assembled for the first time, once
  // its nonzero pattern is known (see _handOver)
  bool seq = _isSequential();
  PetscInt nloc = seq ? n : _getLocalSize(n);
  PetscInt mloc = seq ? m : nloc;
  _try(MatCreate(seq ? PETSC_COMM_SELF : MyComm, &M->M));
  _try(MatSetSizes(M->M, nloc, mloc, n, m));
  _try(MatSetType(M->M, MATAIJ));
  _try(MatSeqAIJSetPreallocation(M->M, 0, PETSC_NULL));
  _try(MatMPIAIJSetPreallocation(M->M, 0, PETSC_NULL, 0, PETSC_NULL));
  _try(MatSetOption(M->M, MAT_NEW_NONZERO_ALLOCATION_ERR, PETSC_FALSE));

  // override the default options with the ones from the option
  // database (if any)
  _try(MatSetFromOptions(M->M));

  _getAssembly(M->M).isNew = true;
}

// Destroys and creates the matrix again, as LinAlg_DestroyMatrix followed by
// LinAlg_CreateMatrix, but keeps the memory of its storage for the next
// assembly
void LinAlg_RecreateMatrix(gMatrix *M, gSolver *Solver, int n, int m)
{
  _Storage rows = std::move(_getAssembly(M->M).rows);
  rows.clear();
  LinAlg_DestroyMatrix(M);
  LinAlg_CreateMatrix(M, Solver, n, m);
  _Storage &S = _getAssembly(M->M).rows;
  if(rows.first() == S.first() && rows.last() == S.last()) S = std::move(rows);
}

void LinAlg_DestroySolver(gSolver *Solver)
{
  for(int i = 0; i < 10; i++) {
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
    if(Solver->ksp[i]) {
      _try(KSPDestroy(&Solver->ksp[i]));
      Solver->ksp[i] = NULL;
    }
    if(Solver->snes[i]) {
      _try(SNESDestroy(&Solver->snes[i]));
      Solver->snes[i] = NULL;
    }
#else
    if(Solver->ksp[i]) {
      _try(KSPDestroy(Solver->ksp[i]));
      Solver->ksp[i] = NULL;
    }
    if(Solver->snes[i]) {
      _try(SNESDestroy(Solver->snes[i]));
      Solver->snes[i] = NULL;
    }
#endif
  }
}

void LinAlg_DestroyVector(gVector *V)
{
  _dropVecBuf(V->V);
  if(V->G) {
    auto it = _seqNum.find(V->Vseq);
    if(it != _seqNum.end()) {
      _seqStale.erase(it->second);
      _seqNum.erase(it);
    }
    _releaseGhosts(V->G);
  }
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
  _try(VecDestroy(&V->V));
  if(V->G) _try(VecDestroy(&V->Vseq));
#else
  _try(VecDestroy(V->V));
  if(V->G) _try(VecDestroy(V->Vseq));
#endif
}

void LinAlg_DestroyMatrix(gMatrix *M)
{
  _dropAssembly(M->M);
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
  _try(MatDestroy(&M->M));
#else
  _try(MatDestroy(M->M));
#endif
}

void LinAlg_CopyScalar(gScalar *S1, gScalar *S2) { S1->s = S2->s; }

void LinAlg_CopyVector(gVector *V1, gVector *V2)
{
  _try(VecCopy(V1->V, V2->V));
  if(!V2->G) return;
  if(V1->G != V2->G || _isStaleSeq(V1))
    _setStaleSeq(V2, true);
  else {
    _try(VecCopy(V1->Vseq, V2->Vseq));
    _setStaleSeq(V2, false);
  }
}

void LinAlg_SwapVector(gVector *V1, gVector *V2)
{
  _try(VecSwap(V1->V, V2->V));
  if(V1->G && V1->G == V2->G) {
    _try(VecSwap(V1->Vseq, V2->Vseq));
    bool stale1 = _isStaleSeq(V1), stale2 = _isStaleSeq(V2);
    _setStaleSeq(V1, stale2);
    _setStaleSeq(V2, stale1);
  }
  else {
    _markseq(V1);
    _markseq(V2);
  }
}

void LinAlg_CopyMatrix(gMatrix *M1, gMatrix *M2)
{
  if(_getAssembly(M2->M).isNew) {
    // a matrix that was just created is not preallocated
    _try(MatSetUp(M2->M));
    _try(MatAssemblyBegin(M2->M, MAT_FINAL_ASSEMBLY));
    _try(MatAssemblyEnd(M2->M, MAT_FINAL_ASSEMBLY));
    _try(MatAXPY(M2->M, 1., M1->M, DIFFERENT_NONZERO_PATTERN));
    _getAssembly(M2->M).isNew = false;
    return;
  }
  _try(MatCopy(M1->M, M2->M, DIFFERENT_NONZERO_PATTERN));
}

void LinAlg_ZeroScalar(gScalar *S) { S->s = 0.; }

void LinAlg_ZeroVector(gVector *V)
{
  PetscScalar zero = 0.0;

  _dropVecBuf(V->V);
  _try(VecSet(V->V, zero));
  if(V->G) {
    _try(VecSet(V->Vseq, zero));
    _setStaleSeq(V, false);
  }
}

void LinAlg_ZeroMatrix(gMatrix *M)
{
  _getAssembly(M->M).rows.clear();
  _getAssembly(M->M).nonLocalRows.clear();
  _try(MatZeroEntries(M->M));
}

void LinAlg_ScanScalar(FILE *file, gScalar *S)
{
#if defined(PETSC_USE_COMPLEX)
  double a, b;
  if(fscanf(file, "%lf %lf", &a, &b) != 2)
    Message::Error("Could not scan scalar");
  S->s = a + PETSC_i * b;
#else
  if(fscanf(file, "%lf", &S->s) != 1) Message::Error("Could not scan scalar");
#endif
}

void LinAlg_ScanVector(FILE *file, gVector *V)
{
  PetscInt n, lo, hi;
  _try(VecGetSize(V->V, &n));
  _try(VecGetOwnershipRange(V->V, &lo, &hi));
  for(PetscInt i = 0; i < n; i++) {
    PetscScalar tmp;
#if defined(PETSC_USE_COMPLEX)
    double a, b;
    if(fscanf(file, "%lf %lf", &a, &b) != 2)
      Message::Error("Could not read data in vector");
    tmp = a + PETSC_i * b;
#else
    double a;
    if(fscanf(file, "%lf", &a) != 1)
      Message::Error("Could not read data in vector");
    tmp = a;
#endif
    // each rank reads the whole vector and keeps the entries that it owns
    if(i >= lo && i < hi)
      _try(VecSetValues(V->V, 1, &i, &tmp, INSERT_VALUES));
  }
  LinAlg_AssembleVector(V);
}

void LinAlg_ScanMatrix(FILE *file, gMatrix *M)
{
  Message::Error("ScanMatrix not yet implemented");
}

void LinAlg_ReadScalar(FILE *file, gScalar *S)
{
  Message::Error("ReadScalar not yet implemented");
}

void LinAlg_ReadVector(FILE *file, gVector *V)
{
  // each rank reads the entries that it owns
  PetscInt n, lo, hi;
  _try(VecGetSize(V->V, &n));
  _try(VecGetOwnershipRange(V->V, &lo, &hi));
  std::vector<PetscScalar> tmp(hi - lo);
  std::vector<PetscInt> num(hi - lo);
  for(PetscInt i = lo; i < hi; i++) num[i - lo] = i;
  long start = ftell(file);
  if(fseek(file, start + (long)lo * sizeof(PetscScalar), SEEK_SET) ||
     fread(tmp.data(), sizeof(PetscScalar), hi - lo, file) !=
       (std::size_t)(hi - lo) ||
     fseek(file, start + (long)n * sizeof(PetscScalar), SEEK_SET)) {
    Message::Error("Could not read vector");
    return;
  }
  _try(VecSetValues(V->V, hi - lo, num.data(), tmp.data(), INSERT_VALUES));
  LinAlg_AssembleVector(V);
}

void LinAlg_ReadMatrix(FILE *file, gMatrix *M)
{
  Message::Error("ReadMatrix not yet implemented");
}

void LinAlg_PrintScalar(FILE *file, gScalar *S)
{
#if defined(PETSC_USE_COMPLEX)
  fprintf(file, "%.16g %.16g", real(S->s), imag(S->s));
#else
  fprintf(file, "%.16g", S->s);
#endif
}

void LinAlg_PrintVector(FILE *file, gVector *V, bool matlab,
                        const char *fileName, const char *varName)
{
  if(!matlab) {
    std::vector<PetscScalar> tmp;
    if(!_gather(V, tmp) || !file) return;
    for(std::size_t i = 0; i < tmp.size(); i++) {
#if defined(PETSC_USE_COMPLEX)
      fprintf(file, "%.16g %.16g\n", real(tmp[i]), imag(tmp[i]));
#else
      fprintf(file, "%.16g\n", tmp[i]);
#endif
    }
    fflush(file);
  }
  else {
    PetscViewer fd;
    _try(PetscViewerASCIIOpen(MyComm, fileName, &fd));
    _try(PetscViewerSetFormat(fd, PETSC_VIEWER_ASCII_MATLAB));
    _try(PetscObjectSetName((PetscObject)V->V, varName));
    _try(VecView(V->V, fd));
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
    _try(PetscViewerDestroy(&fd));
#else
    _try(PetscViewerDestroy(fd));
#endif
  }
}

void LinAlg_PrintMatrix(FILE *file, gMatrix *M, bool matlab,
                        const char *fileName, const char *varName)
{
  if(!matlab) {
    PetscInt n, m;
    _try(MatGetSize(M->M, &n, &m));
    for(int i = 0; i < n; i++) {
      PetscInt ncols;
      const PetscInt *cols;
      const PetscScalar *vals;
      _try(MatGetRow(M->M, i, &ncols, &cols, &vals));
      for(int j = 0; j < m; j++) {
#if defined(PETSC_USE_COMPLEX)
        fprintf(file, "[%d, %d] %.16g %.16g\n", i, j, real(vals[j]),
                imag(vals[j]));
#else
        fprintf(file, "[%d, %d] %.16g\n", i, j, vals[j]);
#endif
      }
      _try(MatRestoreRow(M->M, i, &ncols, &cols, &vals));
    }
  }
  else {
    // ASCII
    PetscViewer fd;
    _try(PetscViewerASCIIOpen(MyComm, fileName, &fd));
    _try(PetscViewerSetFormat(fd, PETSC_VIEWER_ASCII_MATLAB));
    _try(PetscObjectSetName((PetscObject)M->M, varName));
    _try(MatView(M->M, fd));

    // Binary
    PetscViewer fd2;
    std::string tmp(fileName);
    _try(PetscViewerBinaryOpen(MyComm, (tmp + ".bin").c_str(), FILE_MODE_WRITE,
                               &fd2));
    _try(PetscViewerSetFormat(fd2, PETSC_VIEWER_DEFAULT));
    _try(MatView(M->M, fd2));

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
    _try(PetscViewerDestroy(&fd));
    _try(PetscViewerDestroy(&fd2));
#else
    _try(PetscViewerDestroy(fd));
    _try(PetscViewerDestroy(fd2));
#endif
  }
}

void LinAlg_WriteScalar(FILE *file, gScalar *S)
{
  Message::Error("WriteScalar not yet implemented");
}

bool LinAlg_PrintOwnedEntries(gVector *V, bool binary, std::string &text)
{
  PetscInt n;
  const PetscScalar *a;
  _try(VecGetLocalSize(V->V, &n));
  _try(VecGetArrayRead(V->V, &a));
  if(binary)
    text.append((const char *)a, n * sizeof(PetscScalar));
  else {
    char str[64];
    for(PetscInt i = 0; i < n; i++) {
#if defined(PETSC_USE_COMPLEX)
      snprintf(str, sizeof(str), "%.16g %.16g\n", real(a[i]), imag(a[i]));
#else
      snprintf(str, sizeof(str), "%.16g\n", a[i]);
#endif
      text += str;
    }
  }
  _try(VecRestoreArrayRead(V->V, &a));
  return V->G != NULL && MyComm != PETSC_COMM_SELF;
}

void LinAlg_WriteVector(FILE *file, gVector *V)
{
  std::vector<PetscScalar> tmp;
  if(!_gather(V, tmp) || !file) return;
  fwrite(tmp.data(), sizeof(PetscScalar), tmp.size(), file);
  fprintf(file, "\n");
}

void LinAlg_WriteMatrix(FILE *file, gMatrix *M)
{
  Message::Error("WriteMatrix not yet implemented");
}

void LinAlg_GetVectorSize(gVector *V, int *i)
{
  PetscInt t;
  _try(VecGetSize(V->V, &t));
  if(t > INT_MAX) Message::Error("Problem too big");
  *i = t;
}

void LinAlg_GetLocalVectorRange(gVector *V, int *low, int *high)
{
  PetscInt tlow, thigh;
  _try(VecGetOwnershipRange(V->V, &tlow, &thigh));
  if(tlow > INT_MAX || thigh > INT_MAX) Message::Error("Problem too big");
  *low = tlow;
  *high = thigh;
}

void LinAlg_GetMatrixSize(gMatrix *M, int *i, int *j)
{
  PetscInt ti, tj;
  _try(MatGetSize(M->M, &ti, &tj));
  if(ti > INT_MAX || tj > INT_MAX) Message::Error("Problem too big");
  *i = ti;
  *j = tj;
}

void LinAlg_GetLocalMatrixRange(gMatrix *M, int *low, int *high)
{
  PetscInt tlow, thigh;
  _try(MatGetOwnershipRange(M->M, &tlow, &thigh));
  if(tlow > INT_MAX || thigh > INT_MAX) Message::Error("Problem too big");
  *low = tlow;
  *high = thigh;
}

void LinAlg_GetDoubleInScalar(double *d, gScalar *S)
{
#if defined(PETSC_USE_COMPLEX)
  *d = real(S->s);
#else
  *d = S->s;
#endif
}

void LinAlg_GetComplexInScalar(double *d1, double *d2, gScalar *S)
{
#if defined(PETSC_USE_COMPLEX)
  *d1 = real(S->s);
  *d2 = imag(S->s);
#else
  Message::Error("'LinAlg_GetComplexInScalar' not available with this Solver");
#endif
}

void LinAlg_GetScalarInVector(gScalar *S, gVector *V, int i)
{
  S->s = _getValue(V, i);
}

void LinAlg_GetDoubleInVector(double *d, gVector *V, int i)
{
  *d = PetscRealPart(_getValue(V, i));
}

void LinAlg_GetAbsDoubleInVector(double *d, gVector *V, int i)
{
  *d = fabs(PetscRealPart(_getValue(V, i)));
}

void LinAlg_GetComplexInVector(double *d1, double *d2, gVector *V, int i, int j)
{
#if defined(PETSC_USE_COMPLEX)
  PetscScalar s = _getValue(V, i);
  *d1 = real(s);
  *d2 = imag(s);
#else
  *d1 = _getValue(V, i);
  *d2 = _getValue(V, j);
#endif
}

void LinAlg_GetScalarInMatrix(gScalar *S, gMatrix *M, int i, int j)
{
  if(!_isInLocalRange(M, i)) return;
  PetscInt ti = i, tj = j;
  _try(MatGetValues(M->M, 1, &ti, 1, &tj, &S->s));
}

void LinAlg_GetDoubleInMatrix(double *d, gMatrix *M, int i, int j)
{
  if(!_isInLocalRange(M, i)) return;
  PetscInt ti = i, tj = j;
  _try(MatGetValues(M->M, 1, &ti, 1, &tj, (PetscScalar *)d));
}

void LinAlg_GetComplexInMatrix(double *d1, double *d2, gMatrix *M, int i, int j,
                               int k, int l)
{
#if defined(PETSC_USE_COMPLEX)
  PetscScalar tmp;
  PetscInt ti = i, tj = j;
  if(_isInLocalRange(M, i)) {
    _try(MatGetValues(M->M, 1, &ti, 1, &tj, &tmp));
    *d1 = real(tmp);
    *d2 = imag(tmp);
  }
#else
  PetscInt ti = i, tj = j, tk = k, tl = l;
  if(_isInLocalRange(M, i))
    _try(MatGetValues(M->M, 1, &ti, 1, &tj, (PetscScalar *)d1));
  if(_isInLocalRange(M, k))
    _try(MatGetValues(M->M, 1, &tk, 1, &tl, (PetscScalar *)d2));
#endif
}

void LinAlg_GetColumnInMatrix(gMatrix *M, int col, gVector *V1)
{
  Message::Error("GetColumnInMatrix not yet implemented");
}

void LinAlg_SetScalar(gScalar *S, double *d)
{
#if defined(PETSC_USE_COMPLEX)
  S->s = d[0] + (PETSC_i * d[1]);
#else
  S->s = d[0];
#endif
}

void LinAlg_SetVector(gVector *V, double *v)
{
  PetscScalar tmp = *v;
  _try(VecSet(V->V, tmp));
  if(V->G) {
    _try(VecSet(V->Vseq, tmp));
    _setStaleSeq(V, false);
  }
}

void LinAlg_SetScalarInVector(gScalar *S, gVector *V, int i)
{
  if(!_isInLocalRange(V, i)) return;
  PetscInt ti = i;
  _try(VecSetValues(V->V, 1, &ti, &S->s, INSERT_VALUES));
}

void LinAlg_SetScalarInVectorOffProcess(gScalar *S, gVector *V, int i)
{
  PetscInt ti = i;
  _try(VecSetValues(V->V, 1, &ti, &S->s, INSERT_VALUES));
}

void LinAlg_SetDoubleInVector(double d, gVector *V, int i)
{
  if(!_isInLocalRange(V, i)) return;
  PetscScalar tmp = d;
  PetscInt ti = i;
  _try(VecSetValues(V->V, 1, &ti, &tmp, INSERT_VALUES));
}

void LinAlg_SetComplexInVector(double d1, double d2, gVector *V, int i, int j)
{
  PetscScalar tmp;
#if defined(PETSC_USE_COMPLEX)
  if(_isInLocalRange(V, i)) {
    PetscInt ti = i;
    tmp = d1 + PETSC_i * d2;
    _try(VecSetValues(V->V, 1, &ti, &tmp, INSERT_VALUES));
  }
#else
  PetscInt ti = i, tj = j;
  if(_isInLocalRange(V, i)) {
    tmp = d1;
    _try(VecSetValues(V->V, 1, &ti, &tmp, INSERT_VALUES));
  }
  if(_isInLocalRange(V, j)) {
    tmp = d2;
    _try(VecSetValues(V->V, 1, &tj, &tmp, INSERT_VALUES));
  }
#endif
}

void LinAlg_AddScalarScalar(gScalar *S1, gScalar *S2, gScalar *S3)
{
  S3->s = S1->s + S2->s;
}

void LinAlg_DummyVector(gVector *V)
{
  PetscInt n;
  PetscScalar zero = 0.0;

  if(Current.DofData->DummyDof == NULL) return;

  _try(VecGetSize(V->V, &n));
  for(PetscInt i = 0; i < n; i++)
    if(Current.DofData->DummyDof[i] == 1)
      _try(VecSetValues(V->V, 1, &i, &zero, INSERT_VALUES));
}

void LinAlg_AddScalarInVector(gScalar *S, gVector *V, int i)
{
  if(!_addsRow(V, i)) return;

  if(Current.DofData->DummyDof)
    if(Current.DofData->DummyDof[i] == 1) return;

  PetscInt ti = i;
  _addInVector(V->V, ti, S->s);
}

void LinAlg_AddDoubleInVector(double d, gVector *V, int i)
{
  if(!_addsRow(V, i)) return;

  if(Current.DofData->DummyDof)
    if(Current.DofData->DummyDof[i] == 1) return;

  PetscScalar tmp = d;
  PetscInt ti = i;
  _addInVector(V->V, ti, tmp);
}

void LinAlg_AddComplexInVector(double d1, double d2, gVector *V, int i, int j)
{
  PetscScalar tmp;
  int iok = 1, jok = 1;

  if(Current.DofData->DummyDof) {
    if(Current.DofData->DummyDof[i] == 1) iok = 0;
    if(Current.DofData->DummyDof[j] == 1) jok = 0;
  }

#if defined(PETSC_USE_COMPLEX)
  if(_addsRow(V, i) && iok && jok) {
    PetscInt ti = i;
    tmp = d1 + PETSC_i * d2;
    _addInVector(V->V, ti, tmp);
  }
#else
  PetscInt ti = i, tj = j;
  if(_addsRow(V, i) && iok) {
    tmp = d1;
    _addInVector(V->V, ti, tmp);
  }
  if(_addsRow(V, j) && jok) {
    tmp = d2;
    _addInVector(V->V, tj, tmp);
  }
#endif
}

void LinAlg_AddScalarInMatrix(gScalar *S, gMatrix *M, int i, int j)
{
  if(!_addsRow(M, i)) return;

  if(Current.DofData->DummyDof)
    if((Current.DofData->DummyDof[i] == 1 ||
        Current.DofData->DummyDof[j] == 1) &&
       (i != j))
      return;

  _addInMatrix(M, i, j, S->s);
}

void LinAlg_AddDoubleInMatrix(double d, gMatrix *M, int i, int j)
{
  if(!_addsRow(M, i)) return;

  if(Current.DofData->DummyDof)
    if((Current.DofData->DummyDof[i] == 1 ||
        Current.DofData->DummyDof[j] == 1) &&
       (i != j))
      return;

  _addInMatrix(M, i, j, d);
}

void LinAlg_AddComplexInMatrix(double d1, double d2, gMatrix *M, int i, int j,
                               int k, int l)
{
  PetscScalar tmp;
#if defined(PETSC_USE_COMPLEX)
  if(_addsRow(M, i)) {
    tmp = d1 + PETSC_i * d2;
    _addInMatrix(M, i, j, tmp);
  }
#else
  bool iok = _addsRow(M, i);
  bool kok = _addsRow(M, k);
  if(d1) {
    tmp = d1;
    if(iok) _addInMatrix(M, i, j, tmp);
    if(kok) _addInMatrix(M, k, l, tmp);
  }
  if(d2) {
    if(iok) _addInMatrix(M, i, l, -d2);
    if(kok) _addInMatrix(M, k, j, d2);
  }
#endif
}

void LinAlg_AddVectorVector(gVector *V1, gVector *V2, gVector *V3)
{
  PetscScalar tmp = 1.0;
  if(V3 == V1) {
    _try(VecAXPY(V1->V, tmp, V2->V));
    _markseq(V1);
  }
  else if(V3 == V2) {
    _try(VecAXPY(V2->V, tmp, V1->V));
    _markseq(V2);
  }
  else
    Message::Error("Wrong arguments in 'LinAlg_AddVectorVector'");
}

void LinAlg_AddVectorProdVectorDouble(gVector *V1, gVector *V2, double d,
                                      gVector *V3)
{
  PetscScalar tmp = d;
  if(V3 == V1) {
    _try(VecAXPY(V1->V, tmp, V2->V));
    _markseq(V1);
  }
  else if(V3 == V2) {
    _try(VecAYPX(V2->V, tmp, V1->V));
    _markseq(V2);
  }
  else
    Message::Error("Wrong arguments in 'LinAlg_AddVectorProdVectorDouble'");
}

void LinAlg_AddProdVectorDoubleProdVectorDouble(double alpha, gVector *V1,
                                                double beta, gVector *V2,
                                                gVector *V3)
{
  PetscScalar alpha1 = alpha, beta1 = beta;
  PetscScalar gamma1 = 0.0;
  _try(VecAXPBYPCZ(V3->V, alpha1, beta1, gamma1, V1->V, V2->V));
  _markseq(V3);
}

// Nonzero structure of X relative to Y (collective): SAME_NONZERO_PATTERN if X
// and Y have the same nonzeros, SUBSET_NONZERO_PATTERN if those of X are a
// subset of those of Y, DIFFERENT_NONZERO_PATTERN otherwise. Telling PETSc
// that the pattern is the same or a subset keeps the nonzero structure of Y,
// and thus allows solvers to reuse their symbolic factorization of Y.
static MatStructure _getStructure(Mat X, Mat Y)
{
  if(X == Y) return SAME_NONZERO_PATTERN;
  PetscInt mx, nx, my, ny, sx, ex, sy, ey;
  _try(MatGetSize(X, &mx, &nx));
  _try(MatGetSize(Y, &my, &ny));
  _try(MatGetOwnershipRange(X, &sx, &ex));
  _try(MatGetOwnershipRange(Y, &sy, &ey));
  int same = 1, subset = 1;
  if(mx != my || nx != ny || sx != sy || ex != ey) same = subset = 0;
  for(PetscInt r = sx; r < ex && subset; r++) {
    PetscInt ncx, ncy;
    const PetscInt *cx, *cy;
    _try(MatGetRow(X, r, &ncx, &cx, PETSC_NULL));
    _try(MatGetRow(Y, r, &ncy, &cy, PETSC_NULL));
    if(ncx != ncy || memcmp(cx, cy, ncx * sizeof(PetscInt))) {
      same = 0;
      // columns are sorted: X's must all appear in Y's
      PetscInt k = 0;
      for(PetscInt i = 0; i < ncx && subset; i++) {
        while(k < ncy && cy[k] < cx[i]) k++;
        if(k == ncy || cy[k] != cx[i]) subset = 0;
      }
    }
    _try(MatRestoreRow(X, r, &ncx, &cx, PETSC_NULL));
    _try(MatRestoreRow(Y, r, &ncy, &cy, PETSC_NULL));
  }
  int loc[2] = {same, subset}, glob[2];
  MPI_Allreduce(loc, glob, 2, MPI_INT, MPI_MIN,
                PetscObjectComm((PetscObject)Y));
  return glob[0] ? SAME_NONZERO_PATTERN :
         glob[1] ? SUBSET_NONZERO_PATTERN :
                   DIFFERENT_NONZERO_PATTERN;
}

void LinAlg_AddMatrixMatrix(gMatrix *M1, gMatrix *M2, gMatrix *M3)
{
  PetscScalar tmp = 1.0;
  if(M3 == M1)
    _try(MatAXPY(M1->M, tmp, M2->M, _getStructure(M2->M, M1->M)));
  else if(M3 == M2)
    _try(MatAXPY(M2->M, tmp, M1->M, _getStructure(M1->M, M2->M)));
  else
    Message::Error("Wrong arguments in 'LinAlg_AddMatrixMatrix'");
}

void LinAlg_AddMatrixProdMatrixDouble(gMatrix *M1, gMatrix *M2, double d,
                                      gMatrix *M3)
{
  PetscScalar tmp = d;
  if(M3 == M1)
    _try(MatAXPY(M1->M, tmp, M2->M, _getStructure(M2->M, M1->M)));
  else if(M3 == M2)
#if(PETSC_VERSION_MAJOR == 2) && (PETSC_VERSION_MINOR == 3) &&                 \
  (PETSC_VERSION_SUBMINOR < 2)
    _try(MatAYPX(M2->M, tmp, M1->M));
#else
    _try(MatAYPX(M2->M, tmp, M1->M, _getStructure(M1->M, M2->M)));
#endif
  else
    Message::Error("Wrong arguments in 'LinAlg_AddMatrixProdMatrixDouble'");
}

void LinAlg_SubScalarScalar(gScalar *S1, gScalar *S2, gScalar *S3)
{
  S3->s = S1->s - S2->s;
}

void LinAlg_SubVectorVector(gVector *V1, gVector *V2, gVector *V3)
{
  PetscScalar tmp = -1.0;
  if(V3 == V1) {
    _try(VecAXPY(V1->V, tmp, V2->V)); // V1->V = V1->V - V2->V
    _markseq(V1);
  }
  else if(V3 == V2) {
    _try(VecAYPX(V2->V, tmp, V1->V)); // V2->V = V1->V - V2->V
    _markseq(V2);
  }
  else
    Message::Error("Wrong arguments in 'LinAlg_SubVectorVector'");
}

void LinAlg_SubMatrixMatrix(gMatrix *M1, gMatrix *M2, gMatrix *M3)
{
  PetscScalar tmp = -1.0;
  if(M3 == M1) // M1->M = M1->M - M2->M
    _try(MatAXPY(M1->M, tmp, M2->M, _getStructure(M2->M, M1->M)));
  else if(M3 == M2) // M2->M = M1->M - M2->M
    _try(MatAYPX(M2->M, tmp, M1->M, _getStructure(M1->M, M2->M)));
  else
    Message::Error("Wrong arguments in 'LinAlg_SubMatrixMatrix'");
}

void LinAlg_ProdScalarScalar(gScalar *S1, gScalar *S2, gScalar *S3)
{
  S3->s = S1->s * S2->s;
}

void LinAlg_ProdScalarDouble(gScalar *S1, double d, gScalar *S2)
{
  S2->s = S1->s * d;
}

void LinAlg_ProdScalarComplex(gScalar *S, double d1, double d2, double *d3,
                              double *d4)
{
#if defined(PETSC_USE_COMPLEX)
  PetscScalar tmp;
#endif

#if defined(PETSC_USE_COMPLEX)
  tmp = S->s * (d1 + PETSC_i * d2);
  *d3 = real(tmp);
  *d4 = imag(tmp);
#else
  *d3 = S->s * d1;
  *d4 = S->s * d2;
#endif
}

void LinAlg_ProdVectorScalar(gVector *V1, gScalar *S, gVector *V2)
{
  if(V2 == V1) {
    _try(VecScale(V1->V, S->s));
    _markseq(V1);
  }
  else
    Message::Error("Wrong arguments in 'LinAlg_ProdVectorScalar'");
}

void LinAlg_ProdVectorDouble(gVector *V1, double d, gVector *V2)
{
  PetscScalar tmp = d;
  if(V2 == V1) {
    _try(VecScale(V1->V, tmp));
    _markseq(V1);
  }
  else
    Message::Error("Wrong arguments in 'LinAlg_ProdVectorDouble'");
}

void LinAlg_ProdVectorComplex(gVector *V1, double d1, double d2, gVector *V2)
{
  Message::Error("ProdVectorComplex not yet implemented");
}

void LinAlg_ProdVectorVector(gVector *V1, gVector *V2, double *d)
{
  PetscScalar tmp;
  _try(VecDot(V1->V, V2->V, &tmp));
#if defined(PETSC_USE_COMPLEX)
  *d = real(tmp);
#else
  *d = tmp;
#endif
}

void LinAlg_ProdMatrixVector(gMatrix *M, gVector *V1, gVector *V2)
{
  if(V2 == V1)
    Message::Error("Wrong arguments in 'LinAlg_ProdMatrixVector'");
  else {
    _try(MatMult(M->M, V1->V, V2->V));
    _markseq(V2);
  }
}

void LinAlg_ProdMatrixScalar(gMatrix *M1, gScalar *S, gMatrix *M2)
{
  if(M2 == M1)
    _try(MatScale(M1->M, S->s));
  else
    Message::Error("Wrong arguments in 'LinAlg_ProdMatrixScalar'");
}

void LinAlg_ProdMatrixDouble(gMatrix *M1, double d, gMatrix *M2)
{
  PetscScalar tmp = d;
  if(M2 == M1)
    _try(MatScale(M1->M, tmp));
  else
    Message::Error("Wrong arguments in 'LinAlg_ProdMatrixDouble'");
}

void LinAlg_ProdMatrixComplex(gMatrix *M1, double d1, double d2, gMatrix *M2)
{
#if defined(PETSC_USE_COMPLEX)
  if(M2 == M1) {
    PetscScalar tmp = d1 + (PETSC_i * d2);
    _try(MatScale(M1->M, tmp));
  }
  else
    Message::Error("Wrong arguments in 'LinAlg_ProdMatrixDouble'");
#else
  Message::Error("ProdMatrixComplex not yet implemented");
#endif
}

void LinAlg_DivScalarScalar(gScalar *S1, gScalar *S2, gScalar *S3)
{
  S3->s = S1->s / S2->s;
}

void LinAlg_DivScalarDouble(gScalar *S1, double d, gScalar *S2)
{
  S2->s = S1->s / d;
}

void LinAlg_VectorNorm2(gVector *V1, double *norm)
{
  PetscReal tmp;
  _try(VecNorm(V1->V, NORM_2, &tmp));
  *norm = tmp;
}

void LinAlg_VectorNormInf(gVector *V1, double *norm)
{
  PetscReal tmp;
  _try(VecNorm(V1->V, NORM_INFINITY, &tmp));
  *norm = tmp;
}

void LinAlg_AssembleMatrix(gMatrix *M)
{
  _handOver(M->M);
  Message::Barrier();
  _try(MatAssemblyBegin(M->M, MAT_FINAL_ASSEMBLY));
  _try(MatAssemblyEnd(M->M, MAT_FINAL_ASSEMBLY));
}

void LinAlg_AssembleVector(gVector *V)
{
  _flushVecBufs();
  Message::Barrier();
  _try(VecAssemblyBegin(V->V));
  _try(VecAssemblyEnd(V->V));
  _fillseq(V);
}

#if defined(HAVE_ZITSOL)

extern "C" {
int getdp_zitsol(int n, int nnz, int *row, int *col, double *valr, double *vali,
                 double *rhsr, double *rhsi, double *solr, double *soli,
                 int precond, int lfil, double tol0, double tol, int im,
                 int maxits);
}

static void _zitsol(gMatrix *A, gVector *B, gVector *X)
{
  int precond = 1, lfil = 30, im = 100, maxits = 200;
  double tol0 = 0.01, tol = 1e-10;
  PetscTruth set;
  PetscOptionsGetInt(PETSC_NULL, "-zitsol_precond", &precond, &set);
  PetscOptionsGetInt(PETSC_NULL, "-zitsol_lfil", &lfil, &set);
  PetscOptionsGetInt(PETSC_NULL, "-zitsol_im", &im, &set);
  PetscOptionsGetInt(PETSC_NULL, "-zitsol_maxits", &maxits, &set);
  PetscOptionsGetReal(PETSC_NULL, "-zitsol_tol0", &tol0, &set);
  PetscOptionsGetReal(PETSC_NULL, "-zitsol_tol", &tol, &set);

  MatInfo info;
  _try(MatGetInfo(A->M, MAT_LOCAL, &info));
  int nnz = info.nz_used;
  // int n = info.rows_local;
  PetscInt n;
  _try(VecGetLocalSize(B->V, &n));

  Current.KSPSystemSize = n;

  int *row = (int *)Malloc(nnz * sizeof(int));
  int *col = (int *)Malloc(nnz * sizeof(int));
  double *valr = (double *)Malloc(nnz * sizeof(double));
  double *vali = (double *)Malloc(nnz * sizeof(double));
  double *rhsr = (double *)Malloc(n * sizeof(double));
  double *rhsi = (double *)Malloc(n * sizeof(double));
  double *solr = (double *)Malloc(n * sizeof(double));
  double *soli = (double *)Malloc(n * sizeof(double));

  int k = 0;
  for(int i = 0; i < n; i++) {
    PetscInt ncols;
    const PetscInt *cols;
    const PetscScalar *vals;
    _try(MatGetRow(A->M, i, &ncols, &cols, &vals));
    for(int j = 0; j < ncols; j++) {
      if(k >= nnz) {
        Message::Error("Something wrong in nnz: %d >= %d", k, nnz);
        return;
      }
      row[k] = i;
      col[k] = cols[j];
      Message::Debug("A[%d][%d] = ", row[k], col[k]);
#if defined(PETSC_USE_COMPLEX)
      valr[k] = real(vals[j]);
      vali[k] = imag(vals[j]);
      Message::Debug("%g+i*%g", valr[k], vali[k]);
#else
      valr[k] = vals[j];
      vali[k] = 0.;
      Message::Debug("%g", valr[k]);
#endif
      k++;
    }
    _try(MatRestoreRow(A->M, i, &ncols, &cols, &vals));
  }

  Message::Info("n = %d, nnz = %d (check k = %d)", n, nnz, k);

  PetscScalar *b, *x;
  _try(VecGetArray(B->V, &b));
  _try(VecGetArray(X->V, &x));
  for(int i = 0; i < n; i++) {
#if defined(PETSC_USE_COMPLEX)
    rhsr[i] = real(b[i]);
    rhsi[i] = imag(b[i]);
    solr[i] = real(x[i]);
    soli[i] = imag(x[i]);
#else
    rhsr[i] = b[i];
    rhsi[i] = 0.;
    solr[i] = x[i];
    soli[i] = 0.;
#endif
  }
  _try(VecRestoreArray(B->V, &b));
  _try(VecRestoreArray(X->V, &x));

  int its = getdp_zitsol(n, nnz, row, col, valr, vali, rhsr, rhsi, solr, soli,
                         precond, lfil, tol0, tol, im, maxits);
  if(its >= maxits)
    Message::Error("Did not converge in %d iterations", maxits);
  else
    Message::Info("Converged in %d iterations", its);

  Current.KSPIterations = its;

  for(PetscInt i = 0; i < n; i++) {
    PetscScalar d;
#if defined(PETSC_USE_COMPLEX)
    d = solr[i] + PETSC_i * soli[i];
#else
    d = solr[i];
#endif
    _try(VecSetValues(X->V, 1, &i, &d, INSERT_VALUES));
  }

  Free(row);
  Free(col);
  Free(valr);
  Free(vali);
  Free(rhsr);
  Free(rhsi);
  Free(solr);
  Free(soli);
}

#endif

static PetscErrorCode _myKspMonitor(KSP ksp, PetscInt it, PetscReal rnorm,
                                    void *mctx)
{
  Message::Info("%3ld KSP Residual norm %14.12e", (long)it, rnorm);
  Current.KSPIteration = it;
  Current.KSPResidual = rnorm;
  return 0;
}

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 5)
// Operator to give to an existing KSP for solving with matrix M. Systems are
// regenerated in new matrices (see ZeroMatrix in SolvingOperations.cpp), which
// makes PETSc redo the symbolic factorization (e.g. the ordering and analysis
// of MUMPS) at each Generate. If the matrix the KSP was set up with is only
// referenced by the KSP anymore (its gMatrix has been destroyed) and has the
// same nonzero pattern as M, copy the values of M into it and keep it: only the
// numerical factorization is then redone.
static Mat _getOperator(KSP ksp, Mat M)
{
  Mat Amat = PETSC_NULL, Pmat = PETSC_NULL;
  _try(KSPGetOperators(ksp, &Amat, &Pmat));
  if(!Pmat || Pmat == M || Amat != Pmat) return M;
  // the PC holds 2 references (Amat and Pmat): any other means the matrix is
  // still in use elsewhere. Both matrices must also be assembled to compare
  // their structures: e.g. the line search of SolveJac_AdaptRelax regenerates
  // the system without reassembling the Jacobian matrix the KSP was set up with
  PetscInt refct;
  PetscBool assembledP, assembledM;
  _try(PetscObjectGetReference((PetscObject)Pmat, &refct));
  _try(MatAssembled(Pmat, &assembledP));
  _try(MatAssembled(M, &assembledM));
  int reuse = (refct <= 2 && assembledP && assembledM) ? 1 : 0, glob;
  MPI_Allreduce(&reuse, &glob, 1, MPI_INT, MPI_MIN,
                PetscObjectComm((PetscObject)M));
  if(!glob || _getStructure(M, Pmat) != SAME_NONZERO_PATTERN) return M;
  _try(MatCopy(M, Pmat, SAME_NONZERO_PATTERN));
  return Pmat;
}
#endif

#if defined(PETSC_HAVE_HYPRE)
// hypre's AMS (Hiptmair-Xu) preconditioner for edge unknowns needs the discrete
// gradient G, from the nodes to the edges, and the coordinates of the nodes
// (see _setupAMS)

// The mesh edges of the current system, with their row (-1 if not an
// unknown), and their nodes (G has 1 in the column of the first node and -1 in
// the column of the second); a node is outside the space spanned by G when one
// of its edges is not an unknown.
struct AMSEdges {
  struct Edge {
    int num, row, n[2];
    bool own; // in an element of the mesh partition of this rank
  };
  std::vector<Edge> edges;
  std::unordered_map<int, int> nodeIndex; // node number -> index in nodes
  std::vector<int> nodes, minRow;
  std::vector<char> outside;
};

// Asks each rank r about the numbers ask[r] (all the ranks call it): rank r
// gives answer(number) for each of them, and these answers replace ask[r]
template <class F>
static void _askRanks(std::vector<std::vector<int> > &ask, F answer)
{
  std::vector<std::vector<int> > recv;
  Message::Exchange(ask, recv);
  for(auto &numbers : recv)
    for(int &num : numbers) num = answer(num);
  Message::Exchange(recv, ask);
}

static bool _getAMSEdges(AMSEdges &a)
{
  struct DofData *DofData_P = Current.DofData;
  int NumType = 0;
  for(int i = 0; i < List_Nbr(DofData_P->DofList); i++) {
    struct Dof *Dof_P = (struct Dof *)List_Pointer(DofData_P->DofList, i);
    if(!i) NumType = Dof_P->NumType;
    else if(Dof_P->NumType != NumType) {
      Message::Error("AMS needs a system with the edge unknowns of a single "
                     "basis function");
      return false;
    }
  }
  auto rowOf = [&](int num) {
    struct Dof *Dof_P = Dof_GetDofStruct(DofData_P, NumType, num, 0);
    return (Dof_P && (Dof_P->Type == DOF_UNKNOWN ||
                      Dof_P->Type == DOF_UNKNOWN_INIT)) ?
             Dof_P->Case.Unknown.NumDof - 1 :
             -1;
  };
  // the edges of the elements, from their first to their second node in
  // increasing order of the node numbers, sorted by nodes (they are taken from
  // the elements, as the table of the edges is not there when the edges were
  // read from the .pre file): their number, whether they are in an element of
  // the partition of this rank, and otherwise the rank of an element that
  // has them
  struct EdgeInfo {
    int num = 0, other = -1;
    bool own = false;
  };
  const int np = Message::GetCommSize();
  std::map<std::pair<int, int>, EdgeInfo> edges;
  for(int i = 0; i < Geo_GetNbrGeoElements(); i++) {
    struct Geo_Element *ge = Geo_GetGeoElement(i);
    const bool own = Dof_IsPartitionOfThisRank(ge->Partition);
    for(int k = 0; k < ge->NbrEdges; k++) {
      int *local = Geo_GetNodesOfEdgeInElement(ge, k);
      if(!local[0] || !local[1] || local[2]) continue;
      int n0 = abs(ge->NumNodes[abs(local[0]) - 1]);
      int n1 = abs(ge->NumNodes[abs(local[1]) - 1]);
      EdgeInfo &e = edges[std::make_pair(std::min(n0, n1), std::max(n0, n1))];
      e.num = abs(ge->NumEdges[k]);
      if(own) e.own = true;
      e.other = Geo_RankOfPartition(ge->Partition);
    }
  }

  // the rows of the edges: with rank-local Dofs a rank only has the Dofs of
  // the edges of its own elements, and asks the rows of the others to the
  // rank of an element that has them
  std::vector<int> rows(edges.size(), -1);
  std::vector<std::vector<int> > send(np), asked(np);
  int i = 0;
  for(auto &e : edges) {
    if(e.second.own || !Dof_UseLocalDofs())
      rows[i] = rowOf(e.second.num);
    else {
      send[e.second.other].push_back(e.second.num);
      asked[e.second.other].push_back(i);
    }
    i++;
  }
  if(Dof_UseLocalDofs()) {
    _askRanks(send, rowOf);
    for(int r = 0; r < np; r++)
      for(std::size_t j = 0; j < send[r].size(); j++)
        rows[asked[r][j]] = send[r][j];
  }

  i = 0;
  for(auto &e : edges) {
    const int nodes[2] = {e.first.first, e.first.second};
    AMSEdges::Edge ed;
    ed.num = e.second.num;
    ed.own = e.second.own;
    ed.row = rows[i++];
    for(int j = 0; j < 2; j++) {
      int num = nodes[j];
      auto it = a.nodeIndex.find(num);
      if(it == a.nodeIndex.end()) {
        it = a.nodeIndex.insert(std::make_pair(num, (int)a.nodes.size())).first;
        a.nodes.push_back(num);
        a.minRow.push_back(INT_MAX);
        a.outside.push_back(0);
      }
      ed.n[j] = it->second;
      if(ed.row < 0)
        a.outside[it->second] = 1;
      else
        a.minRow[it->second] = std::min(a.minRow[it->second], ed.row);
    }
    a.edges.push_back(ed);
  }
  return true;
}

// The columns of G: the nodes in the space, owned by the rank of their first
// edge unknown and numbered by rank, then by node number. Gets the column of
// each node of a, or -1, and the nodes of the columns this rank owns, and
// returns the number of columns (all the ranks call it).
static PetscInt _numberAMSNodes(Mat A, const AMSEdges &a, std::vector<int> &col,
                                std::vector<int> &myNodes)
{
  const std::vector<int> &nodes = a.nodes, &minRow = a.minRow;
  const std::vector<char> &outside = a.outside;
  PetscMPIInt size, rank;
  MPI_Comm comm = PetscObjectComm((PetscObject)A);
  MPI_Comm_size(comm, &size);
  MPI_Comm_rank(comm, &rank);
  const PetscInt *ranges;
  _try(MatGetOwnershipRanges(A, &ranges));
  col.assign(nodes.size(), -1);
  PetscInt ncols = 0;
  if(Dof_UseLocalDofs()) {
    // A rank may only have a part of the mesh: it knows all the edges of the
    // nodes of its own elements, and thus whether these nodes are in the space
    // and who owns them. Each rank numbers the nodes it owns after those of
    // the previous ranks, and asks the owners the numbers of the other nodes
    // of the edges of its rows.
    std::vector<char> complete(nodes.size(), 0);
    for(int e = 0; e < Geo_GetNbrGeoElements(); e++) {
      struct Geo_Element *ge = Geo_GetGeoElement(e);
      if(!Dof_IsPartitionOfThisRank(ge->Partition)) continue;
      for(int k = 0; k < ge->NbrNodes; k++) {
        auto it = a.nodeIndex.find(abs(ge->NumNodes[k]));
        if(it != a.nodeIndex.end()) complete[it->second] = 1;
      }
    }
    for(std::size_t i = 0; i < nodes.size(); i++)
      if(complete[i] && !outside[i] && minRow[i] >= ranges[rank] &&
         minRow[i] < ranges[rank + 1])
        myNodes.push_back(nodes[i]);
    std::sort(myNodes.begin(), myNodes.end());
    PetscInt nloc = myNodes.size(), first = 0;
    MPI_Exscan(&nloc, &first, 1, MPIU_INT, MPI_SUM, comm);
    if(!rank) first = 0;
    MPI_Allreduce(&nloc, &ncols, 1, MPIU_INT, MPI_SUM, comm);
    auto columnOf = [&](int num) {
      auto it = std::lower_bound(myNodes.begin(), myNodes.end(), num);
      return (it == myNodes.end() || *it != num) ?
               -1 :
               (int)(first + (it - myNodes.begin()));
    };
    std::vector<std::vector<int> > send(size), asked(size);
    std::vector<char> requested(nodes.size(), 0);
    for(auto &ed : a.edges) {
      if(ed.row < ranges[rank] || ed.row >= ranges[rank + 1]) continue;
      for(int j = 0; j < 2; j++) {
        int k = ed.n[j];
        if(outside[k] || requested[k]) continue;
        requested[k] = 1;
        int owner = _ownerOf(ranges, size, minRow[k]);
        if(owner == rank)
          col[k] = columnOf(nodes[k]);
        else {
          send[owner].push_back(nodes[k]);
          asked[owner].push_back(k);
        }
      }
    }
    _askRanks(send, columnOf);
    for(int r = 0; r < size; r++)
      for(std::size_t j = 0; j < send[r].size(); j++)
        col[asked[r][j]] = send[r][j];
  }
  else {
    // (a node without any edge unknown is outside the space)
    std::vector<std::pair<int, int> > cols; // (owner, node number) -> index
    for(std::size_t i = 0; i < nodes.size(); i++) {
      if(outside[i]) continue;
      int owner = _ownerOf(ranges, size, minRow[i]);
      cols.push_back(std::make_pair(owner, nodes[i]));
    }
    std::sort(cols.begin(), cols.end());
    ncols = cols.size();
    for(std::size_t i = 0; i < cols.size(); i++) {
      col[a.nodeIndex.at(cols[i].second)] = i;
      if(cols[i].first == rank) myNodes.push_back(cols[i].second);
    }
  }
  return ncols;
}

// With -getdp_ams_check: prints |A G x| / (|A| |G x|) for a random x, which is
// round-off for a correct G
static void _checkAMS(Mat A, Mat G)
{
  Vec x, Gx, AGx;
  _try(MatCreateVecs(G, &x, &Gx));
  _try(VecDuplicate(Gx, &AGx));
  _try(VecSetRandom(x, PETSC_NULL));
  _try(MatMult(G, x, Gx));
  _try(MatMult(A, Gx, AGx));
  PetscReal nA, nGx, nAGx;
  _try(MatNorm(A, NORM_INFINITY, &nA));
  _try(VecNorm(Gx, NORM_INFINITY, &nGx));
  _try(VecNorm(AGx, NORM_INFINITY, &nAGx));
  Message::Info("AMS check: |A G x| / (|A| |G x|) = %g", nAGx / (nA * nGx));
  _try(VecDestroy(&x));
  _try(VecDestroy(&Gx));
  _try(VecDestroy(&AGx));
}

// Gives hypre's AMS (Hiptmair-Xu) preconditioner, selected with -pc_type hypre
// -pc_hypre_type ams, the discrete gradient and the node coordinates of the
// current system, whose unknowns must all be edge unknowns of one basis
// function. G has one row per edge unknown and one column per node whose edges
// are all unknowns (the gradient of the other nodes is not in the space, e.g.
// on Dirichlet boundaries). Each rank sets its own rows; the nodes are numbered
// the same way by all the ranks, or by their owner (see _numberAMSNodes).
// Options read here: -getdp_ams_zero_beta for a matrix without mass term,
// -getdp_ams_check (see _checkAMS).
static void _setupAMS(PC pc, Mat A)
{
  AMSEdges a;
  if(!_getAMSEdges(a)) return;
  std::vector<int> col, myNodes;
  PetscInt ncols = _numberAMSNodes(A, a, col, myNodes);
  PetscInt nloc = myNodes.size();

  PetscInt rstart, rend;
  _try(MatGetOwnershipRange(A, &rstart, &rend));
  Mat G;
  _try(MatCreateAIJ(PetscObjectComm((PetscObject)A), rend - rstart, nloc,
                    PETSC_DETERMINE, ncols, 2, PETSC_NULL, 2, PETSC_NULL, &G));
  for(auto &ed : a.edges) {
    if(ed.row < rstart || ed.row >= rend) continue;
    for(int j = 0; j < 2; j++) {
      PetscInt r = ed.row, c = col[ed.n[j]];
      PetscScalar v = j ? -1. : 1.;
      if(c >= 0) _try(MatSetValues(G, 1, &r, 1, &c, &v, INSERT_VALUES));
    }
  }
  _try(MatAssemblyBegin(G, MAT_FINAL_ASSEMBLY));
  _try(MatAssemblyEnd(G, MAT_FINAL_ASSEMBLY));

  std::vector<PetscReal> xyz(3 * nloc);
  for(PetscInt i = 0; i < nloc; i++) {
    struct Geo_Node *Geo_Node = Geo_GetGeoNodeOfNum(myNodes[i]);
    xyz[3 * i] = Geo_Node->x;
    xyz[3 * i + 1] = Geo_Node->y;
    xyz[3 * i + 2] = Geo_Node->z;
  }
  _try(PCHYPRESetDiscreteGradient(pc, G));
  _try(PCSetCoordinates(pc, 3, nloc, xyz.data()));
  PetscBool zeroBeta = PETSC_FALSE, check = PETSC_FALSE;
  _try(PetscOptionsGetBool(PETSC_NULL, PETSC_NULL, "-getdp_ams_zero_beta",
                           &zeroBeta, PETSC_NULL));
  _try(PetscOptionsGetBool(PETSC_NULL, PETSC_NULL, "-getdp_ams_check", &check,
                           PETSC_NULL));
  if(zeroBeta) _try(PCHYPRESetBetaPoissonMatrix(pc, PETSC_NULL));
  PetscInt nrows;
  _try(MatGetSize(A, &nrows, PETSC_NULL));
  Message::Info("AMS: discrete gradient with %d edge unknowns and %d nodes%s",
                (int)nrows, (int)ncols, zeroBeta ? ", no mass term" : "");
  if(check) _checkAMS(A, G);
  _try(PetscObjectCompose((PetscObject)pc, "getdp_ams_G", (PetscObject)G));
  _try(MatDestroy(&G)); // referenced by the preconditioner
}

// Whether pc is hypre's AMS and has not been set up yet: _setupAMS is done once
// per preconditioner, to which it attaches G
// whether the preconditioner is one of hypre's
static bool _isHypre(PC pc)
{
  PetscBool hypre;
  _try(PetscObjectTypeCompare((PetscObject)pc, PCHYPRE, &hypre));
  return hypre;
}

// The discrete gradient given to AMS by _setupAMS, or NULL
static Mat _getAMSGradient(PC pc)
{
  PetscObject G = PETSC_NULL;
  _try(PetscObjectQuery((PetscObject)pc, "getdp_ams_G", &G));
  return (Mat)G;
}

static bool _needsAMSSetup(PC pc)
{
  if(!_isHypre(pc) || _getAMSGradient(pc)) return false;
  const char *hypreType = "";
  _try(PCHYPREGetType(pc, &hypreType));
  return !strcmp(hypreType, "ams");
}

// The numbers of the edges of the group name, which must be defined with
// EdgesOfTreeIn
static bool _getTreeEdges(char *name, std::set<int> &tree)
{
  int i = List_ISearchSeq(Problem_S.Group, name, fcmp_Group_Name);
  if(i < 0) {
    Message::Error("Unknown group '%s' for -getdp_ams_tree_group", name);
    return false;
  }
  struct Group *Group_P = (struct Group *)List_Pointer(Problem_S.Group, i);
  if(Group_P->FunctionType != EDGESOFTREEIN) {
    Message::Error("Group '%s' is not defined with EdgesOfTreeIn", name);
    return false;
  }
  if(!Group_P->ExtendedList) Generate_ExtendedGroup(Group_P);
  for(int j = 0; j < List_Nbr(Group_P->ExtendedList); j++) {
    int num;
    List_Read(Group_P->ExtendedList, j, &num);
    tree.insert(abs(num));
  }
  return true;
}

// Replaces the values vals of each rank of comm, with 4 integers each in ints,
// by those of all the ranks, one rank after the other, on the first rank, and
// by nothing on the others
static void _gatherOnFirstRank(MPI_Comm comm, std::vector<int> &ints,
                               std::vector<double> &vals)
{
  PetscMPIInt np, me;
  MPI_Comm_size(comm, &np);
  MPI_Comm_rank(comm, &me);
  std::vector<int> counts(np), displs(np);
  int count = (int)vals.size(), total = 0;
  MPI_Gather(&count, 1, MPI_INT, counts.data(), 1, MPI_INT, 0, comm);
  for(int r = 0; r < np; r++) {
    displs[r] = total;
    total += counts[r];
  }
  std::vector<int> allInts(me ? 0 : 4 * total);
  std::vector<double> allVals(me ? 0 : total);
  MPI_Gatherv(vals.data(), count, MPI_DOUBLE, allVals.data(), counts.data(),
              displs.data(), MPI_DOUBLE, 0, comm);
  for(int r = 0; r < np; r++) {
    counts[r] *= 4;
    displs[r] *= 4;
  }
  MPI_Gatherv(ints.data(), 4 * count, MPI_INT, allInts.data(), counts.data(),
              displs.data(), MPI_INT, 0, comm);
  ints.swap(allInts);
  vals.swap(allVals);
}

// On the first rank: the nodes of the tree, sorted by number, and psi by
// breadth-first walks from the nodes outside G, then from any node left (a
// tree of its own, with psi = 0 at its first node). The tree edge e has 4
// integers in allInts, the numbers of its two nodes (replaced here by their
// index in nodes) and whether these are outside G, and the value of a in
// allVals[e].
static void _walkTree(std::vector<int> &allInts,
                      const std::vector<double> &allVals,
                      std::vector<int> &nodes, std::vector<double> &psi)
{
  const int total = (int)allVals.size();
  for(int e = 0; e < total; e++) {
    nodes.push_back(allInts[4 * e]);
    nodes.push_back(allInts[4 * e + 1]);
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  auto index = [&](int num) {
    return (int)(std::lower_bound(nodes.begin(), nodes.end(), num) -
                 nodes.begin());
  };
  std::vector<char> outside(nodes.size(), 0), done(nodes.size(), 0);
  std::vector<std::vector<int> > adj(nodes.size());
  for(int e = 0; e < total; e++) {
    for(int j = 0; j < 2; j++) {
      int k = index(allInts[4 * e + j]);
      allInts[4 * e + j] = k;
      if(allInts[4 * e + 2 + j]) outside[k] = 1;
      adj[k].push_back(e);
    }
  }
  psi.assign(nodes.size(), 0.);
  std::vector<int> queue;
  for(int pass = 0; pass < 2; pass++) {
    for(std::size_t n = 0; n < nodes.size(); n++) {
      if(done[n] || (!pass && !outside[n])) continue;
      done[n] = 1;
      queue.assign(1, n);
      for(std::size_t q = 0; q < queue.size(); q++) {
        int k = queue[q];
        for(int e : adj[k]) {
          // an edge goes from its first node (sign 1) to its second (-1)
          int j = (allInts[4 * e] == k) ? 1 : 0, u = allInts[4 * e + j];
          if(done[u]) continue;
          done[u] = 1;
          // (G psi)_e = s_k psi_k + s_u psi_u must equal a_e (0 if fixed),
          // and psi = 0 at nodes outside G
          if(!outside[u]) {
            double sk = j ? 1. : -1., su = -sk;
            double pk = outside[k] ? 0. : psi[k];
            psi[u] = (allVals[e] - sk * pk) / su;
          }
          queue.push_back(u);
        }
      }
    }
  }
}

// With -getdp_ams_tree_group <group> (a Group defined with EdgesOfTreeIn),
// bring the solution of the ungauged problem back to the gauge of that tree:
// a <- a - G psi, with psi such that a - G psi vanishes on the tree edges,
// computed by walking the tree from the nodes outside G (psi = 0 there, e.g.
// on Dirichlet boundaries). The curl is unchanged, and the result is the
// solution of the problem gauged by the tree.
//
// The walk is done by the first rank, on the tree alone: each rank sends it
// the tree edges of the elements of its partition, with the value of a, and
// gets psi at the nodes of the tree, with which it corrects the rows of the
// edges of its elements.
static void _gaugeOnTreeForAMS(PC pc, gVector *X)
{
  char name[256];
  PetscBool set = PETSC_FALSE;
  _try(PetscOptionsGetString(PETSC_NULL, PETSC_NULL, "-getdp_ams_tree_group",
                             name, sizeof(name), &set));
  if(!set || !_getAMSGradient(pc)) return;
  std::set<int> tree;
  if(!_getTreeEdges(name, tree)) return;

  AMSEdges a;
  if(!_getAMSEdges(a)) return;
  auto value = [&](const AMSEdges::Edge &ed) {
    return (ed.row >= 0) ? PetscRealPart(_getValue(X, ed.row)) : 0.;
  };

  // the tree edges of this rank: the numbers of their two nodes, whether
  // these are outside G, and the value of a
  std::vector<int> ints;
  std::vector<double> vals;
  for(auto &ed : a.edges) {
    if(!ed.own || !tree.count(ed.num)) continue;
    for(int j = 0; j < 2; j++) ints.push_back(a.nodes[ed.n[j]]);
    for(int j = 0; j < 2; j++) ints.push_back(a.outside[ed.n[j]]);
    vals.push_back(value(ed));
  }
  MPI_Comm comm = PetscObjectComm((PetscObject)X->V);
  PetscMPIInt me;
  MPI_Comm_rank(comm, &me);
  _gatherOnFirstRank(comm, ints, vals);

  // psi at the nodes of the tree, computed by the first rank
  std::vector<int> nodes;
  std::vector<double> psi;
  if(!me) _walkTree(ints, vals, nodes, psi);
  int nbNodes = (int)nodes.size();
  MPI_Bcast(&nbNodes, 1, MPI_INT, 0, comm);
  nodes.resize(nbNodes);
  psi.resize(nbNodes);
  MPI_Bcast(nodes.data(), nbNodes, MPI_INT, 0, comm);
  MPI_Bcast(psi.data(), nbNodes, MPI_DOUBLE, 0, comm);
  auto psiOf = [&](int num) {
    auto it = std::lower_bound(nodes.begin(), nodes.end(), num);
    return (it == nodes.end() || *it != num) ? 0. : psi[it - nodes.begin()];
  };

  // a <- a - G psi on the rows of the edges of the elements of this rank
  std::vector<PetscInt> rows;
  std::vector<PetscScalar> newVals;
  double treeMax = 0.;
  for(auto &ed : a.edges) {
    if(!ed.own || ed.row < 0) continue;
    double g = 0.;
    for(int j = 0; j < 2; j++)
      if(!a.outside[ed.n[j]]) g += (j ? -1. : 1.) * psiOf(a.nodes[ed.n[j]]);
    double v = value(ed) - g;
    if(tree.count(ed.num)) treeMax = std::max(treeMax, fabs(v));
    rows.push_back(ed.row);
    newVals.push_back(v);
  }
  _try(VecSetValues(X->V, rows.size(), rows.data(), newVals.data(),
                    INSERT_VALUES));
  _try(VecAssemblyBegin(X->V));
  _try(VecAssemblyEnd(X->V));
  _fillseq(X);
  double glob;
  MPI_Allreduce(&treeMax, &glob, 1, MPI_DOUBLE, MPI_MAX, comm);
  Message::Info("AMS: solution gauged on tree '%s' (%d edges), max |a| on the "
                "tree %g", name, (int)tree.size(), glob);
}

// Without a gauge the curl-curl matrix is singular, its kernel being the
// gradients G x, and CG needs a compatible right-hand side, G^T b = 0. With
// -getdp_ams_clean_rhs, remove the incompatible part of b (e.g. from a source
// current density that is not exactly divergence-free):
// b <- b - G (G^T G)^-1 G^T b, with CG + BoomerAMG on the nodal Laplacian G^T G
// (options prefix -getdp_ams_clean_)
static void _cleanRHSForAMS(PC pc, gVector *B)
{
  PetscBool clean = PETSC_FALSE;
  _try(PetscOptionsGetBool(PETSC_NULL, PETSC_NULL, "-getdp_ams_clean_rhs",
                           &clean, PETSC_NULL));
  Mat G = _getAMSGradient(pc), L;
  if(!clean || !G) return;
  Vec b = B->V;
  Vec r, phi, Gphi;
  _try(MatCreateVecs(G, &r, &Gphi));
  _try(VecDuplicate(r, &phi));
  _try(MatMultTranspose(G, b, r));
  PetscReal nb, nr0, nr1;
  _try(VecNorm(b, NORM_2, &nb));
  _try(VecNorm(r, NORM_2, &nr0));
  _try(MatTransposeMatMult(G, G, MAT_INITIAL_MATRIX, PETSC_DETERMINE, &L));
  KSP ksp;
  PC lpc;
  _try(KSPCreate(PetscObjectComm((PetscObject)G), &ksp));
  _try(KSPSetOperators(ksp, L, L));
  _try(KSPSetType(ksp, KSPCG));
  _try(KSPGetPC(ksp, &lpc));
  _try(PCSetType(lpc, PCHYPRE));
  _try(PCHYPRESetType(lpc, "boomeramg"));
  _try(KSPSetTolerances(ksp, 1e-12, PETSC_DEFAULT, PETSC_DEFAULT, 1000));
  _try(KSPSetOptionsPrefix(ksp, "getdp_ams_clean_"));
  _try(KSPSetFromOptions(ksp));
  _try(KSPSolve(ksp, r, phi));
  PetscInt its;
  _try(KSPGetIterationNumber(ksp, &its));
  _try(MatMult(G, phi, Gphi));
  _try(VecAXPY(b, -1., Gphi));
  _markseq(B);
  _try(MatMultTranspose(G, b, r));
  _try(VecNorm(r, NORM_2, &nr1));
  Message::Info("AMS: |G^T b| / |b| = %g -> %g (%d iterations)", nr0 / nb,
                nr1 / nb, (int)its);
  _try(KSPDestroy(&ksp));
  _try(MatDestroy(&L));
  _try(VecDestroy(&r));
  _try(VecDestroy(&phi));
  _try(VecDestroy(&Gphi));
}
#endif

// Creates the KSP of a solver for the matrix M, with the default options
static void _createKSP(KSP *ksp, Mat M)
{
  _try(KSPCreate(MyComm, ksp));
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 5)
  _try(KSPSetOperators(*ksp, M, M));
#else
  _try(KSPSetOperators(*ksp, M, M, DIFFERENT_NONZERO_PATTERN));
#endif
  _try(KSPMonitorSet(*ksp, _myKspMonitor, PETSC_NULL, PETSC_NULL));
  PC pc;
  _try(KSPGetPC(*ksp, &pc));

  // set some default options: use direct solver (PARDISO, MUMPS, UMFPACK, or
  // native PETSc LU)
  _try(KSPSetType(*ksp, "preonly"));
  _try(PCSetType(pc, PCLU));
#if(PETSC_VERSION_MAJOR > 2) && defined(PETSC_HAVE_MUMPS)
  _try(PCFactorSetMatSolverPackage(pc, "mumps"));
#elif(PETSC_VERSION_MAJOR > 2) && defined(PETSC_HAVE_MKL_PARDISO)
  _try(PCFactorSetMatSolverPackage(pc, "mkl_pardiso"));
#elif(PETSC_VERSION_MAJOR > 2) &&                                              \
  (defined(PETSC_HAVE_UMFPACK) || defined(PETSC_HAVE_SUITESPARSE))
  _try(PCFactorSetMatSolverPackage(pc, "umfpack"));
#else
  _try(PetscOptionsSetValue("-pc_factor_nonzeros_along_diagonal", "1e-12"));
#if(PETSC_VERSION_MAJOR == 2) && (PETSC_VERSION_MINOR == 3) &&                 \
  (PETSC_VERSION_SUBMINOR < 3)
  _try(PCFactorSetMatOrdering(pc, MATORDERING_RCM));
#else
  _try(PCFactorSetMatOrderingType(pc, MATORDERINGRCM));
#endif
#endif
}

// Gives the matrix M to an existing KSP, whose preconditioner is computed again
// if precond, and kept otherwise. Returns true if the matrix the KSP was set up
// with is used again (see _getOperator).
static bool _setOperators(KSP ksp, Mat M, int precond)
{
  bool reused = false;
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 5)
  _try(KSPSetReusePreconditioner(ksp, precond ? PETSC_FALSE : PETSC_TRUE));
  Mat op = _getOperator(ksp, M);
  reused = (op != M);
  _try(KSPSetOperators(ksp, op, op));
#else
  if(precond) _try(KSPSetOperators(ksp, M, M, DIFFERENT_NONZERO_PATTERN));
#endif
  return reused;
}

#if defined(PETSC_HAVE_MUMPS)
// Warns about the error or the warning MUMPS reported in the last solve with
// the preconditioner pc, if any
static void _checkMumps(PC pc)
{
  Mat mat;
  PCFactorGetMatrix(pc, &mat);
  PetscInt ival;
  MatMumpsGetInfo(mat, 1, &ival);
  if(ival < 0) {
    std::ostringstream sstream;
    sstream << "MUMPS error " << ival;
    switch(ival) {
    case -6:
      sstream << " (matrix singular in structure)";
      break;
    case -8: case -9: case -17: case -20:
      sstream << " (work array too small: increase -mat_mumps_icntl_14)";
      break;
    case -10:
      sstream << " (matrix numerically singular or zero pivot)";
      break;
    }
    Message::Warning("%s", sstream.str().c_str());
  }
  else if(ival > 0) {
    Message::Warning("MUMPS warning %d", ival);
  }
}
#endif

// Reports the iterations and the convergence of the last solve with ksp, and
// returns false if the solver failed (an iterative solver that did not
// converge has not failed)
static bool _report(KSP ksp)
{
  if(Message::GetVerbosity() > 5) {
    _try(KSPView(ksp, MyPetscViewer));
  }

  PetscInt its;
  _try(KSPGetIterationNumber(ksp, &its));
  if(its > 1) Message::Info("%d iterations", its);
  Current.KSPIterations = its;

  KSPConvergedReason reason;
  _try(KSPGetConvergedReason(ksp, &reason));
  Current.KSPConvergedReason = (double)reason;

  return !(reason < 0 && reason != KSP_DIVERGED_ITS &&
           reason != KSP_DIVERGED_DTOL);
}

static void _solve(gMatrix *A, gVector *B, gSolver *Solver, gVector *X,
                   int precond, int kspIndex)
{
#if defined(HAVE_ZITSOL)
  // testing Yousef's new preconditioners and solvers
  PetscTruth set, zitsol = PETSC_FALSE;
  PetscOptionsGetTruth(PETSC_NULL, "-zitsol", &zitsol, &set);
  if(zitsol) {
    _zitsol(A, B, X);
    return;
  }
#endif
  if(kspIndex < 0 || kspIndex > 9) {
    Message::Error("Linear Solver index out of range (%d)", kspIndex);
    return;
  }

  PetscInt i, j;
  _try(MatGetSize(A->M, &i, &j));
  Current.KSPSystemSize = i;
  if(!i) {
    Message::Warning("Zero-size system: skipping solve!");
    return;
  }

  if(kspIndex != 0) Message::Info("Using solver index %d", kspIndex);

  KSP &ksp = Solver->ksp[kspIndex];
  bool reused = false; // the matrix the KSP was set up with is used again
  if(!ksp)
    _createKSP(&ksp, A->M);
  else
    reused = _setOperators(ksp, A->M, precond);
  PC pc;
  _try(KSPGetPC(ksp, &pc));

  // override the default options with the ones from the option database (if
  // any)
  _try(KSPSetFromOptions(ksp));

#if defined(PETSC_HAVE_HYPRE)
  if(_needsAMSSetup(pc)) _setupAMS(pc, A->M);
#endif

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 4)
  const char *ksptype = "";
  _try(KSPGetType(ksp, &ksptype));
  const char *pctype = "";
  _try(PCGetType(pc, &pctype));
#else
  const KSPType ksptype;
  _try(KSPGetType(ksp, &ksptype));
  const PCType pctype;
  _try(PCGetType(pc, &pctype));
#endif

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 9)
  MatSolverType stype;
  _try(PCFactorGetMatSolverType(pc, &stype));
#elif(PETSC_VERSION_MAJOR > 2)
  const MatSolverPackage stype;
  _try(PCFactorGetMatSolverPackage(pc, &stype));
#else
  const char *stype = "";
#endif

  Message::Info("N: %ld - %s %s %s", long(i), ksptype, pctype, stype);

#if defined(PETSC_HAVE_HYPRE)
  if(_isHypre(pc)) _cleanRHSForAMS(pc, B);
#endif
  _try(KSPSolve(ksp, B->V, X->V));

#if defined(PETSC_HAVE_MUMPS)
  if(stype && !strcmp(stype, "mumps")) _checkMumps(pc);
#endif

  // get the ghosts of the solution from the ranks that own them
  _fillseq(X);

#if defined(PETSC_HAVE_HYPRE)
  if(_isHypre(pc)) _gaugeOnTreeForAMS(pc, X);
#endif

  if(!_report(ksp)) {
    // A solver that failed is not used again: e.g. PETSc skips the numerical
    // factorizations of MUMPS as long as MUMPS reports an error, i.e. until a
    // new symbolic factorization. If the failed solve reused the matrix of a
    // previous one, and thus its symbolic factorization, which can be the
    // cause of the failure (e.g. a work array that is now too small), the
    // system is solved again from scratch.
    _try(KSPDestroy(&ksp));
    ksp = PETSC_NULL;
    if(reused) {
      Message::Info("Linear solver failed: solving again without reusing the "
                    "previous factorization");
      _solve(A, B, Solver, X, precond, kspIndex);
    }
    return;
  }

  PetscTruth set, kspfree = PETSC_FALSE;
  PetscOptionsGetTruth(PETSC_NULL, "-kspfree", &kspfree, &set);
  if(kspfree) {
    Message::Info("Freeing KSP solver");
    LinAlg_DestroySolver(Solver);
  }
}

void LinAlg_Solve(gMatrix *A, gVector *B, gSolver *Solver, gVector *X,
                  int solverIndex)
{
  _solve(A, B, Solver, X, 1, solverIndex);
}

void LinAlg_SolveAgain(gMatrix *A, gVector *B, gSolver *Solver, gVector *X,
                       int solverIndex)
{
  _solve(A, B, Solver, X, 0, solverIndex);
}

void LinAlg_SetGlobalSolverOptions(const std::string &opt)
{
  _try(PetscOptionsInsertString(opt.c_str()));
}

extern void Generate_Residual(gVector *x, gVector *f);
extern void Generate_FullJacobian(gVector *x, gMatrix *Jac);

static PetscErrorCode _NLFormFunction(SNES snes, Vec x, Vec f, void *mctx)
{
  gVector gx, gf;
  gx.V = x;
  gx.G = NULL;
  gf.V = f;
  gf.G = NULL;
  Generate_Residual(&gx, &gf);
  return 0;
}

#if(PETSC_VERSION_MAJOR == 2) ||                                               \
  ((PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR < 5))
static PetscErrorCode _NLFormJacobian(SNES snes, Vec x, Mat *J, Mat *PC,
                                      MatStructure *flag, void *mctx)
{
  gVector gx;
  gx.V = x;
  gx.G = NULL;
  gMatrix gJ;
  gJ.M = *J;
  Generate_FullJacobian(&gx, &gJ);
  *J = gJ.M;
  *flag = DIFFERENT_NONZERO_PATTERN;
  Message::Barrier();
  _try(MatAssemblyBegin(*J, MAT_FINAL_ASSEMBLY));
  _try(MatAssemblyEnd(*J, MAT_FINAL_ASSEMBLY));
  if(*PC != *J) {
    _try(MatAssemblyBegin(*PC, MAT_FINAL_ASSEMBLY));
    _try(MatAssemblyEnd(*PC, MAT_FINAL_ASSEMBLY));
  }
  return 0;
}
#else
// Copies the Jacobian X assembled by GetDP into the matrix Y of SNES
// (collective). The Jacobian is assembled in a new matrix at each iteration
// (Generate_Residual regenerates the system, see ZeroMatrix in
// SolvingOperations.cpp), while SNES keeps the matrix it was given when it was
// created: the Jacobian of the system at that time, which is not assembled. If
// Y has the nonzero pattern of X only the values are copied, and the solver
// keeps its symbolic factorization; otherwise Y is preallocated for the pattern
// of X and filled row by row, and the preconditioner is reset: PETSc does not
// always see a new pattern in a matrix that is preallocated again (in parallel
// its nonzero state is that of its new local parts).
static void _copyJacobian(SNES snes, Mat X, Mat Y)
{
  if(X == Y) return;
  PetscBool assembled;
  _try(MatAssembled(Y, &assembled));
  if(assembled && _getStructure(X, Y) == SAME_NONZERO_PATTERN) {
    _try(MatCopy(X, Y, SAME_NONZERO_PATTERN));
    return;
  }
  PetscInt first, last, cfirst, clast, nc;
  const PetscInt *cols;
  const PetscScalar *vals;
  _try(MatGetOwnershipRange(X, &first, &last));
  _try(MatGetOwnershipRangeColumn(X, &cfirst, &clast));
  std::vector<PetscInt> dnnz(last - first, 0), onnz(last - first, 0);
  for(PetscInt i = first; i < last; i++) {
    _try(MatGetRow(X, i, &nc, &cols, PETSC_NULL));
    for(PetscInt k = 0; k < nc; k++) {
      if(cols[k] >= cfirst && cols[k] < clast)
        dnnz[i - first]++;
      else
        onnz[i - first]++;
    }
    _try(MatRestoreRow(X, i, &nc, &cols, PETSC_NULL));
  }
  _try(MatXAIJSetPreallocation(Y, 1, dnnz.data(), onnz.data(), PETSC_NULL,
                               PETSC_NULL));
  for(PetscInt i = first; i < last; i++) {
    _try(MatGetRow(X, i, &nc, &cols, &vals));
    _try(MatSetValues(Y, 1, &i, nc, cols, vals, INSERT_VALUES));
    _try(MatRestoreRow(X, i, &nc, &cols, &vals));
  }
  _try(MatAssemblyBegin(Y, MAT_FINAL_ASSEMBLY));
  _try(MatAssemblyEnd(Y, MAT_FINAL_ASSEMBLY));
  KSP ksp;
  PC pc;
  _try(SNESGetKSP(snes, &ksp));
  _try(KSPGetPC(ksp, &pc));
  _try(PCReset(pc));
}

static PetscErrorCode _NLFormJacobian(SNES snes, Vec x, Mat J, Mat P,
                                      void *mctx)
{
  gVector gx;
  gx.V = x;
  gx.G = NULL;
  gMatrix gJ;
  Generate_FullJacobian(&gx, &gJ);
  _copyJacobian(snes, gJ.M, J);
  if(P != J) _copyJacobian(snes, gJ.M, P);
  return 0;
}
#endif

static PetscErrorCode _mySnesMonitor(SNES snes, PetscInt it, PetscReal rnorm,
                                     void *mctx)
{
  Message::Info("%3ld SNES Residual norm %14.12e", (long)it, rnorm);
  return 0;
}

static void _solveNL(gMatrix *A, gVector *B, gMatrix *J, gVector *R,
                     gSolver *Solver, gVector *X, int precond, int solverIndex)
{
  if(solverIndex < 0 || solverIndex > 9) {
    Message::Error("NonLinear Solver index out of range (%d)", solverIndex);
    return;
  }

  PetscInt n, m;
  _try(MatGetSize(J->M, &n, &m));
  if(!n) {
    Message::Warning("Zero-size jacobian: skipping solve!");
    return;
  }

  Message::Info("N: %ld", (long)n);

  if(solverIndex != 0)
    Message::Info("Using nonlinear solver index %d", solverIndex);

  // Setting nonlinear solver defaults
  if(!Solver->snes[solverIndex]) {
    _try(SNESCreate(MyComm, &Solver->snes[solverIndex]));
    _try(SNESMonitorSet(Solver->snes[solverIndex], _mySnesMonitor, PETSC_NULL,
                        PETSC_NULL));
    _try(SNESSetTolerances(Solver->snes[solverIndex], 1.e-12, PETSC_DEFAULT,
                           PETSC_DEFAULT, PETSC_DEFAULT, PETSC_DEFAULT));

    // override default options with those from database (if any)
    _try(SNESSetFromOptions(Solver->snes[solverIndex]));

    PetscTruth fd_jacobian = PETSC_FALSE, snes_fd = PETSC_FALSE;
    PetscOptionsGetTruth(PETSC_NULL, "-fd_jacobian", &fd_jacobian, 0);
    PetscOptionsGetTruth(PETSC_NULL, "-snes_fd", &snes_fd, 0);
    if(fd_jacobian || snes_fd) {
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 4)
      _try(SNESSetJacobian(Solver->snes[solverIndex], J->M, J->M,
                           SNESComputeJacobianDefault, PETSC_NULL));
#else
      _try(SNESSetJacobian(Solver->snes[solverIndex], J->M, J->M,
                           SNESDefaultComputeJacobian, PETSC_NULL));
#endif
    }
    else {
      Message::Info("Jacobian computed by GetDP");
      _try(SNESSetJacobian(Solver->snes[solverIndex], J->M, J->M,
                           _NLFormJacobian, PETSC_NULL));
    }
    _try(SNESSetFunction(Solver->snes[solverIndex], R->V, _NLFormFunction,
                         PETSC_NULL)); // R(x) = A(x)*x-b
  }

  KSP ksp;
  SNESGetKSP(Solver->snes[solverIndex], &ksp);
  PC pc;
  _try(KSPGetPC(ksp, &pc));
  _try(KSPSetType(ksp, "preonly"));
  _try(PCSetType(pc, PCLU));
#if(PETSC_VERSION_MAJOR > 2) && defined(PETSC_HAVE_MUMPS)
  _try(PCFactorSetMatSolverPackage(pc, "mumps"));
#endif
  _try(SNESSolve(Solver->snes[solverIndex], PETSC_NULL, X->V));

  // get the ghosts of the solution from the ranks that own them
  _fillseq(X);

  if(Message::GetVerbosity() > 5)
    _try(SNESView(Solver->snes[solverIndex], MyPetscViewer));

  PetscInt its;
  _try(SNESGetIterationNumber(Solver->snes[solverIndex], &its));
  Message::Info("Number of Newton iterations %d", its);

  // SNES keeps the last iterate as the solution when it does not converge
  // (e.g. when the line search fails); the reason is the same on all the ranks
  // of the solver
  SNESConvergedReason reason;
  _try(SNESGetConvergedReason(Solver->snes[solverIndex], &reason));
  if(reason < 0 && (MyComm == PETSC_COMM_SELF || !Message::GetCommRank()))
    Message::Warning("SolveNL did not converge (%s after %d iterations)",
                     SNESConvergedReasons[reason], (int)its);
}

void LinAlg_SolveNL(gMatrix *A, gVector *B, gMatrix *J, gVector *R,
                    gSolver *Solver, gVector *X, int solverIndex)
{
  _solveNL(A, B, J, R, Solver, X, 1, solverIndex);
}

#endif
