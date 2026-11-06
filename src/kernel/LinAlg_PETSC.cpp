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
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <complex>
#include <string>
#include <cstring>
#include <sstream>
#include <stdio.h>
#include "GetDPConfig.h"
#include "LinAlg.h"
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
extern struct GeoData *CurrentGeoData;
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

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
// The scatter contexts that gather the distributed vectors, by layout (global
// size and local range): creating one allocates (and zeroes) a sequential
// vector of the global size, which costs as much as the gather itself
static std::map<std::vector<PetscInt>, VecScatter> _toAll;
#endif

void LinAlg_FinalizeSolver()
{
  if(SolverInitialized) {
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
    for(auto &s : _toAll) VecScatterDestroy(&s.second);
    _toAll.clear();
#endif
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

static bool _usePartitions()
{
  if(Message::GetCommSize() == 1 || MyComm == PETSC_COMM_SELF)
    return false; // sequential

  if((int)Current.DofData->PartitionSplit.size() == Message::GetCommSize() + 1)
    return true; // local equations only

  if((int)Current.DofData->PartitionSplit.size() == Message::GetCommSize() + 2)
    return true; // local equations + non-partitioned/global equations

  return false; // partitioning does not match the number of ranks
}

static int _getLocalSize(int n = -1)
{
  int s = PETSC_DECIDE;

  if(_usePartitions()) {
    if(n >= 0 && *Current.DofData->PartitionSplit.rbegin() != n) {
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

// The rows of the non-local equations (global quantities, e.g. circuit
// unknowns coupled to all the edges of a conductor) can have millions of
// entries, which the element loop adds a few at a time in mesh order. Inserted
// directly, each new column shifts the rest of the row in the AIJ storage, so
// filling such a row costs its length squared. Their contributions are thus
// accumulated here, sorted by column, and inserted in one call per row when the
// matrix is assembled.
static std::map<Mat, std::unordered_map<int, std::map<int, PetscScalar> > >
  _nonLocalRows;

// Collective: the rows are first gathered on their owner, since the pieces of a
// row sent through the PETSc stash would be inserted one after the other, each
// new column again shifting the rest of the row
static void _flushNonLocalRows(Mat M)
{
  MPI_Comm comm = PetscObjectComm((PetscObject)M);
  int size, rank;
  MPI_Comm_size(comm, &size);
  MPI_Comm_rank(comm, &rank);
  auto &rows = _nonLocalRows[M];
  if(size > 1) {
    const PetscInt *ranges;
    _try(MatGetOwnershipRanges(M, &ranges));
    std::vector<std::vector<PetscInt> > idx(size);
    std::vector<std::vector<PetscScalar> > val(size);
    for(auto it = rows.begin(); it != rows.end();) {
      int owner = std::upper_bound(ranges, ranges + size + 1, it->first) -
                  ranges - 1;
      if(owner == rank) {
        ++it;
        continue;
      }
      for(auto &c : it->second) {
        idx[owner].push_back(it->first);
        idx[owner].push_back(c.first);
        val[owner].push_back(c.second);
      }
      it = rows.erase(it);
    }
    std::vector<int> nsend(size), nrecv(size), sdispl(size), rdispl(size);
    for(int i = 0; i < size; i++) nsend[i] = (int)val[i].size();
    MPI_Alltoall(nsend.data(), 1, MPI_INT, nrecv.data(), 1, MPI_INT, comm);
    int ns = 0, nr = 0;
    std::vector<PetscInt> sidx, ridx;
    std::vector<PetscScalar> sval, rval;
    for(int i = 0; i < size; i++) {
      sdispl[i] = ns;
      rdispl[i] = nr;
      ns += nsend[i];
      nr += nrecv[i];
    }
    sidx.reserve(2 * (std::size_t)ns);
    sval.reserve(ns);
    for(int i = 0; i < size; i++) {
      sidx.insert(sidx.end(), idx[i].begin(), idx[i].end());
      sval.insert(sval.end(), val[i].begin(), val[i].end());
      std::vector<PetscInt>().swap(idx[i]);
      std::vector<PetscScalar>().swap(val[i]);
    }
    ridx.resize(2 * (std::size_t)nr);
    rval.resize(nr);
    MPI_Alltoallv(sval.data(), nsend.data(), sdispl.data(), MPIU_SCALAR,
                  rval.data(), nrecv.data(), rdispl.data(), MPIU_SCALAR, comm);
    // the indices come in pairs
    for(int i = 0; i < size; i++) {
      nsend[i] *= 2;
      nrecv[i] *= 2;
      sdispl[i] *= 2;
      rdispl[i] *= 2;
    }
    MPI_Alltoallv(sidx.data(), nsend.data(), sdispl.data(), MPIU_INT,
                  ridx.data(), nrecv.data(), rdispl.data(), MPIU_INT, comm);
    for(int i = 0; i < nr; i++) rows[ridx[2 * i]][ridx[2 * i + 1]] += rval[i];
  }
  std::vector<PetscInt> cols;
  std::vector<PetscScalar> vals;
  for(auto &row : rows) {
    cols.clear();
    vals.clear();
    for(auto &c : row.second) {
      cols.push_back(c.first);
      vals.push_back(c.second);
    }
    PetscInt ti = row.first;
    _try(MatSetValues(M, 1, &ti, (PetscInt)cols.size(), cols.data(),
                      vals.data(), ADD_VALUES));
  }
  _nonLocalRows.erase(M);
}

// In a partitioned assembly, only one rank adds the contributions of an element
// to the non-local rows, which can thus belong to another rank (see
// Current.NonLocalRows); otherwise each rank adds its own rows
static bool _addsRow(bool local, int i)
{
  if(!Current.NonLocalRows || !Dof_IsNonLocalEquation(i)) return local;
  return Current.NonLocalRows > 0;
}

static void _addInMatrix(gMatrix *M, int i, int j, PetscScalar v)
{
  if(Dof_IsNonLocalEquation(i)) {
    _nonLocalRows[M->M][i][j] += v;
    return;
  }
  PetscInt ti = i, tj = j;
  _try(MatSetValues(M->M, 1, &ti, 1, &tj, &v, ADD_VALUES));
}

// sequential copies of the distributed vectors (see _fillseq below)
static long _seqCount = 0;
static std::map<Vec, long> _seqNum; // Vseq -> creation number
static std::map<long, std::pair<Vec, Vec> > _seqStale; // number -> (V, Vseq)

void LinAlg_CreateVector(gVector *V, gSolver *Solver, int n, bool sequential)
{
  if(sequential) {
    _try(VecCreateSeq(PETSC_COMM_SELF, n, &V->V));
    V->haveSeq = 0; // no Vseq is created!
    return;
  }

  _try(VecCreate(MyComm, &V->V));

  int nloc = _getLocalSize(n);
  if(nloc > n) {
    Message::Debug("Creating distributed vector with n (%d) < nloc (%d)",
                   n, nloc);
    nloc = n;
  }
  _try(VecSetSizes(V->V, nloc, n));

  // override the default options with the ones from the option
  // database (if any)
  _try(VecSetFromOptions(V->V));

  // create sequential vector that will contain all the values on all
  // the procs
  if(Message::GetCommSize() > 1 && MyComm != PETSC_COMM_SELF) {
    _try(VecCreateSeq(PETSC_COMM_SELF, n, &V->Vseq));
    V->haveSeq = 1;
    _seqNum[V->Vseq] = _seqCount++;
  }
  else {
    V->haveSeq = 0;
  }
}

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)

static VecScatter _getToAll(Vec &V)
{
  PetscInt n, lo, hi;
  _try(VecGetSize(V, &n));
  _try(VecGetOwnershipRange(V, &lo, &hi));
  std::vector<PetscInt> key = {n, lo, hi};
  auto it = _toAll.find(key);
  // the creation is collective: create a context unless all ranks have one
  int have = (it != _toAll.end()), all = have;
  MPI_Allreduce(&have, &all, 1, MPI_INT, MPI_MIN, PetscObjectComm((PetscObject)V));
  if(all) return it->second;
  if(have) _try(VecScatterDestroy(&it->second));
  VecScatter ctx;
  _try(VecScatterCreateToAll(V, &ctx, NULL));
  _toAll[key] = ctx;
  return ctx;
}
#endif

void _fillseq(Vec &V, Vec &Vseq)
{
  // collect all the values from the parallel petsc vector into a sequential
  // vector on each processor
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
  VecScatter ctx = _getToAll(V);
  _try(VecScatterBegin(ctx, V, Vseq, INSERT_VALUES, SCATTER_FORWARD));
  _try(VecScatterEnd(ctx, V, Vseq, INSERT_VALUES, SCATTER_FORWARD));
#else
  VecScatter ctx;
  VecScatterCreateToAll(V, &ctx, NULL);
#if(PETSC_VERSION_MAJOR == 2) && (PETSC_VERSION_MINOR == 3) &&                 \
  (PETSC_VERSION_SUBMINOR < 3)
  VecScatterBegin(V, Vseq, INSERT_VALUES, SCATTER_FORWARD, ctx);
  VecScatterEnd(V, Vseq, INSERT_VALUES, SCATTER_FORWARD, ctx);
#else
  VecScatterBegin(ctx, V, Vseq, INSERT_VALUES, SCATTER_FORWARD);
  VecScatterEnd(ctx, V, Vseq, INSERT_VALUES, SCATTER_FORWARD);
#endif
  VecScatterDestroy(ctx);
#endif
}

// The sequential copy Vseq of a distributed vector is gathered at once after
// an assembly or a solve, but only marked as out of date after the arithmetic
// operations: LinAlg_UpdateSequentialVectors(), called by all ranks at the
// start of each resolution operation, then gathers all the stale copies. (The
// gathering cannot be done when Vseq is read, as the reading functions need
// not be called on all ranks.) gVectors are copied by value, so the copies are
// identified by their Vseq, and gathered in the order of their creation, which
// is the same on all ranks.
static void _fillseq(gVector *V)
{
  if(!V->haveSeq) return;
  _fillseq(V->V, V->Vseq);
  auto it = _seqNum.find(V->Vseq);
  if(it != _seqNum.end()) _seqStale.erase(it->second);
}

static bool _isStaleSeq(gVector *V)
{
  auto it = _seqNum.find(V->Vseq);
  return it != _seqNum.end() && _seqStale.count(it->second);
}

static void _setStaleSeq(gVector *V, bool stale)
{
  auto it = _seqNum.find(V->Vseq);
  if(it == _seqNum.end()) {
    if(stale) _fillseq(V->V, V->Vseq); // not registered: gather now
    return;
  }
  if(stale)
    _seqStale[it->second] = std::make_pair(V->V, V->Vseq);
  else
    _seqStale.erase(it->second);
}

static void _markseq(gVector *V)
{
  if(V->haveSeq) _setStaleSeq(V, true);
}

void LinAlg_UpdateSequentialVectors()
{
  for(auto &s : _seqStale) _fillseq(s.second.first, s.second.second);
  _seqStale.clear();
}

void LinAlg_CreateMatrix(gMatrix *M, gSolver *Solver, int n, int m, bool silent)
{
  Message::Debug("Creating matrix with MPI_CommSize=%d and MPI_IsCommWorld=%d",
                 Message::GetCommSize(), Message::GetIsCommWorld());

  if(Current.DofData->SparsityPattern->size() &&
     (Message::GetCommSize() == 1 || MyComm == PETSC_COMM_SELF)) {
    Message::Debug("... SeqAIJ matrix with exact sparsity pattern");
    // we add 1 to account for the diagonal element enforced below
    std::vector<PetscInt> nnz;
    nnz.resize(n, 1);
    for(auto p : *Current.DofData->SparsityPattern) {
      nnz[p.first]++;
    }
    _try(MatCreateSeqAIJ(PETSC_COMM_SELF, n, m, 0, &nnz[0], &M->M));
    // PETSc (I)LU does not like matrices with empty (non assembled) diagonals
    for(int i = 0; i < n; i++) {
      PetscInt ti = i;
      PetscScalar d = 0.;
      _try(MatSetValues(M->M, 1, &ti, 1, &ti, &d, INSERT_VALUES));
    }
    _try(MatAssemblyBegin(M->M, MAT_FLUSH_ASSEMBLY));
    _try(MatAssemblyEnd(M->M, MAT_FLUSH_ASSEMBLY));
  }
  else if(Current.DofData->SparsityPattern->size() && _usePartitions()) {
    Message::Debug("... MPIAIJ matrix with exact sparsity pattern");
    PetscInt nloc = _getLocalSize();
    PetscInt mloc = nloc;
    std::vector<PetscInt> d_nnz, o_nnz;
    d_nnz.resize(nloc, 1);
    o_nnz.resize(nloc, 0);
    int r = Message::GetCommRank();
    int istart = Current.DofData->PartitionSplit[r];
    int iend = (r == Message::GetCommSize() - 1) ?
      *Current.DofData->PartitionSplit.rbegin() :
      Current.DofData->PartitionSplit[r + 1];
    for(auto p : *Current.DofData->SparsityPattern) {
      if(p.first >= istart && p.first < iend) {
        int i = p.first - istart;
        if(p.second >= istart && p.second < iend)
          d_nnz[i]++;
        else
          o_nnz[i]++;
      }
    }
#if((PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 3))
    _try(MatCreateAIJ(MyComm, nloc, mloc, n, m, 0, &d_nnz[0],
                      0, &o_nnz[0], &M->M));
#else
    _try(MatCreateMPIAIJ(MyComm, nloc, mloc, n, m, 0, &d_nnz[0],
                         0, &o_nnz[0], &M->M));
#endif
  }
  else {
    // use heuristics
    PetscInt nloc = (Message::GetCommSize() == 1 || MyComm == PETSC_COMM_SELF) ?
      n : _getLocalSize();
    PetscInt mloc = nloc;
    PetscInt prealloc = 100.;
    PetscInt prealloc_full = (mloc > 0) ? mloc : n;
    int nonloc = Current.DofData->NonLocalEquations.size();

    // heuristic for global rows: don't prelloc more than 500 MB
    double limit = 500. * 1024. * 1024. / (gSCALAR_SIZE * sizeof(double));
    double estim = (double)nonloc * (double)mloc;
    if(estim > limit) {
      prealloc_full = (int)(limit / nonloc);
      Message::Debug("Heuristic PETSc prealloc_full changed to %d", prealloc_full);
    }

    PetscTruth set_prealloc, set_prealloc_full;
    PetscOptionsGetInt(PETSC_NULL, "-petsc_prealloc", &prealloc,
                       &set_prealloc);
    PetscOptionsGetInt(PETSC_NULL, "-petsc_prealloc_full", &prealloc_full,
                       &set_prealloc_full);

    // prealloc cannot be bigger than the number of columns!
    prealloc = (mloc > 0 && prealloc > mloc) ? mloc :
      (prealloc > n) ? n : prealloc;
    prealloc_full = (mloc > 0 && prealloc_full > mloc) ? mloc :
      (prealloc_full > n) ? n : prealloc_full;

    if(!silent && set_prealloc)
      Message::Info("Setting PETSc prealloc to %d", prealloc);
    if(!silent && set_prealloc_full)
      Message::Info("Setting PETSc prealloc_full to %d", prealloc_full);

    if(Message::GetCommSize() == 1 || MyComm == PETSC_COMM_SELF) {
      Message::Debug("... SeqAIJ matrix with heuristic allocation");
      std::vector<PetscInt> nnz;
      nnz.resize(n, prealloc);
      for(int i = 0; i < nonloc; i++)
        nnz[Current.DofData->NonLocalEquations[i] - 1] = prealloc_full;
      _try(MatCreateSeqAIJ(PETSC_COMM_SELF, n, m, 0, &nnz[0], &M->M));
      // PETSc (I)LU does not like matrices with empty (non assembled) diagonals
      for(int i = 0; i < n; i++) {
        PetscInt ti = i;
        PetscScalar d = 0.;
        _try(MatSetValues(M->M, 1, &ti, 1, &ti, &d, INSERT_VALUES));
      }
      _try(MatAssemblyBegin(M->M, MAT_FLUSH_ASSEMBLY));
      _try(MatAssemblyEnd(M->M, MAT_FLUSH_ASSEMBLY));
    }
    else {
      Message::Debug("... MPIAIJ matrix with heuristic allocation");
#if((PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 3))
      _try(MatCreateAIJ(MyComm, nloc, mloc, n, m, prealloc, PETSC_NULL,
                        prealloc, PETSC_NULL, &M->M));
#else
      _try(MatCreateMPIAIJ(MyComm, nloc, mloc, n, m, prealloc, PETSC_NULL,
                           prealloc, PETSC_NULL, &M->M));
#endif
    }
  }

#if((PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 3))
  // Preallocation routines automatically set this to true, which causes a
  // problem when the mask of the matrix changes (e.g. moving band), where we
  // must allow (some) new allocations
  _try(MatSetOption(M->M, MAT_NEW_NONZERO_ALLOCATION_ERR, PETSC_FALSE));
#endif

  // override the default options with the ones from the option
  // database (if any)
  _try(MatSetFromOptions(M->M));
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
  if(V->haveSeq) {
    auto it = _seqNum.find(V->Vseq);
    if(it != _seqNum.end()) {
      _seqStale.erase(it->second);
      _seqNum.erase(it);
    }
  }
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 2)
  _try(VecDestroy(&V->V));
  if(V->haveSeq) _try(VecDestroy(&V->Vseq));
#else
  _try(VecDestroy(V->V));
  if(V->haveSeq) _try(VecDestroy(V->Vseq));
#endif
}

void LinAlg_DestroyMatrix(gMatrix *M)
{
  _nonLocalRows.erase(M->M);
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
  if(V1->haveSeq) {
    if(_isStaleSeq(V1))
      _setStaleSeq(V2, true);
    else {
      _try(VecCopy(V1->Vseq, V2->Vseq));
      _setStaleSeq(V2, false);
    }
  }
}

void LinAlg_SwapVector(gVector *V1, gVector *V2)
{
  _try(VecSwap(V1->V, V2->V));
  if(V1->haveSeq) {
    _try(VecSwap(V1->Vseq, V2->Vseq));
    bool stale1 = _isStaleSeq(V1), stale2 = _isStaleSeq(V2);
    _setStaleSeq(V1, stale2);
    _setStaleSeq(V2, stale1);
  }
}

void LinAlg_CopyMatrix(gMatrix *M1, gMatrix *M2)
{
  _try(MatCopy(M1->M, M2->M, DIFFERENT_NONZERO_PATTERN));
}

void LinAlg_ZeroScalar(gScalar *S) { S->s = 0.; }

void LinAlg_ZeroVector(gVector *V)
{
  PetscScalar zero = 0.0;

  _try(VecSet(V->V, zero));
  if(V->haveSeq) {
    _try(VecSet(V->Vseq, zero));
    _setStaleSeq(V, false);
  }
}

void LinAlg_ZeroMatrix(gMatrix *M)
{
  _nonLocalRows.erase(M->M);
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
  PetscInt n;
  _try(VecGetSize(V->V, &n));
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
  PetscInt n;
  _try(VecGetSize(V->V, &n));
  PetscScalar *tmp = (PetscScalar *)Malloc(n * sizeof(PetscScalar));
  if(!fread(tmp, sizeof(PetscScalar), n, file)) {
    Message::Error("Could not read vector");
    return;
  }
  for(PetscInt i = 0; i < n; i++) {
    _try(VecSetValues(V->V, 1, &i, &tmp[i], INSERT_VALUES));
  }
  LinAlg_AssembleVector(V);
  Free(tmp);
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
    PetscInt n;
    _try(VecGetSize(V->V, &n));
    Vec VV = V->haveSeq ? V->Vseq : V->V;
    PetscScalar *tmp;
    _try(VecGetArray(VV, &tmp));
    for(int i = 0; i < n; i++) {
#if defined(PETSC_USE_COMPLEX)
      fprintf(file, "%.16g %.16g\n", real(tmp[i]), imag(tmp[i]));
#else
      fprintf(file, "%.16g\n", tmp[i]);
#endif
    }
    fflush(file);
    _try(VecRestoreArray(VV, &tmp));
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

void LinAlg_WriteVector(FILE *file, gVector *V)
{
  PetscInt n;
  _try(VecGetSize(V->V, &n));
  Vec VV = V->haveSeq ? V->Vseq : V->V;
  PetscScalar *tmp;
  _try(VecGetArray(VV, &tmp));
  fwrite(tmp, sizeof(PetscScalar), n, file);
  fprintf(file, "\n");
  _try(VecRestoreArray(VV, &tmp));
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

static bool _isInLocalRange(gVector *V, int i)
{
  if(Message::GetCommSize() == 1) return true;
  int imin, imax;
  LinAlg_GetLocalVectorRange(V, &imin, &imax);
  return (i >= imin && i < imax);
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

static bool _isInLocalRange(gMatrix *M, int i)
{
  if(Message::GetCommSize() == 1) return true;
  int imin, imax;
  LinAlg_GetLocalMatrixRange(M, &imin, &imax);
  return (i >= imin && i < imax);
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
  Vec VV = V->haveSeq ? V->Vseq : V->V;
  PetscScalar *tmp;
  _try(VecGetArray(VV, &tmp));
  S->s = tmp[i];
  _try(VecRestoreArray(VV, &tmp));
}

void LinAlg_GetDoubleInVector(double *d, gVector *V, int i)
{
  Vec VV = V->haveSeq ? V->Vseq : V->V;
  PetscScalar *tmp;
  _try(VecGetArray(VV, &tmp));
#if defined(PETSC_USE_COMPLEX)
  *d = real(tmp[i]);
#else
  *d = tmp[i];
#endif
  _try(VecRestoreArray(VV, &tmp));
}

void LinAlg_GetAbsDoubleInVector(double *d, gVector *V, int i)
{
  Vec VV = V->haveSeq ? V->Vseq : V->V;
  PetscScalar *tmp;
  _try(VecGetArray(VV, &tmp));
#if defined(PETSC_USE_COMPLEX)
  *d = fabs(real(tmp[i]));
#else
  *d = fabs(tmp[i]);
#endif
  _try(VecRestoreArray(VV, &tmp));
}

void LinAlg_GetComplexInVector(double *d1, double *d2, gVector *V, int i, int j)
{
  Vec VV = V->haveSeq ? V->Vseq : V->V;
  PetscScalar *tmp;
  _try(VecGetArray(VV, &tmp));
#if defined(PETSC_USE_COMPLEX)
  *d1 = real(tmp[i]);
  *d2 = imag(tmp[i]);
#else
  *d1 = (double)tmp[i];
  *d2 = (double)tmp[j];
#endif
  _try(VecRestoreArray(VV, &tmp));
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
  if(V->haveSeq) {
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

void LinAlg_SetScalarInMatrix(gScalar *S, gMatrix *M, int i, int j)
{
  if(!_isInLocalRange(M, i)) return;
  PetscInt ti = i, tj = j;
  _try(MatSetValues(M->M, 1, &ti, 1, &tj, &S->s, INSERT_VALUES));
}

void LinAlg_SetDoubleInMatrix(double d, gMatrix *M, int i, int j)
{
  if(!_isInLocalRange(M, i)) return;
  PetscInt ti = i, tj = j;
  _try(MatSetValues(M->M, 1, &ti, 1, &tj, (PetscScalar *)&d, INSERT_VALUES));
}

void LinAlg_SetComplexInMatrix(double d1, double d2, gMatrix *M, int i, int j,
                               int k, int l)
{
  PetscScalar tmp;
#if defined(PETSC_USE_COMPLEX)
  PetscInt ti = i, tj = j;
  if(_isInLocalRange(M, i)) {
    tmp = d1 + PETSC_i * d2;
    _try(MatSetValues(M->M, 1, &ti, 1, &tj, &tmp, INSERT_VALUES));
  }
#else
  PetscInt ti = i, tj = j, tk = k, tl = l;
  if(d1) {
    tmp = d1;
    if(_isInLocalRange(M, i))
      _try(MatSetValues(M->M, 1, &ti, 1, &tj, &tmp, INSERT_VALUES));
    if(_isInLocalRange(M, k))
      _try(MatSetValues(M->M, 1, &tk, 1, &tl, &tmp, INSERT_VALUES));
  }
  if(d2) {
    if(_isInLocalRange(M, i)) {
      tmp = -d2;
      _try(MatSetValues(M->M, 1, &ti, 1, &tl, &tmp, INSERT_VALUES));
    }
    if(_isInLocalRange(M, k)) {
      tmp = d2;
      _try(MatSetValues(M->M, 1, &tk, 1, &tj, &tmp, INSERT_VALUES));
    }
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
  if(!_addsRow(_isInLocalRange(V, i), i)) return;

  if(Current.DofData->DummyDof)
    if(Current.DofData->DummyDof[i] == 1) return;

  PetscInt ti = i;
  _try(VecSetValues(V->V, 1, &ti, &S->s, ADD_VALUES));
}

void LinAlg_AddDoubleInVector(double d, gVector *V, int i)
{
  if(!_addsRow(_isInLocalRange(V, i), i)) return;

  if(Current.DofData->DummyDof)
    if(Current.DofData->DummyDof[i] == 1) return;

  PetscScalar tmp = d;
  PetscInt ti = i;
  _try(VecSetValues(V->V, 1, &ti, &tmp, ADD_VALUES));
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
  if(_addsRow(_isInLocalRange(V, i), i) && iok && jok) {
    PetscInt ti = i;
    tmp = d1 + PETSC_i * d2;
    _try(VecSetValues(V->V, 1, &ti, &tmp, ADD_VALUES));
  }
#else
  PetscInt ti = i, tj = j;
  if(_addsRow(_isInLocalRange(V, i), i) && iok) {
    tmp = d1;
    _try(VecSetValues(V->V, 1, &ti, &tmp, ADD_VALUES));
  }
  if(_addsRow(_isInLocalRange(V, j), j) && jok) {
    tmp = d2;
    _try(VecSetValues(V->V, 1, &tj, &tmp, ADD_VALUES));
  }
#endif
}

void LinAlg_AddScalarInMatrix(gScalar *S, gMatrix *M, int i, int j)
{
  if(!_addsRow(_isInLocalRange(M, i), i)) return;

  if(Current.DofData->DummyDof)
    if((Current.DofData->DummyDof[i] == 1 ||
        Current.DofData->DummyDof[j] == 1) &&
       (i != j))
      return;

  _addInMatrix(M, i, j, S->s);
}

void LinAlg_AddDoubleInMatrix(double d, gMatrix *M, int i, int j)
{
  if(!_addsRow(_isInLocalRange(M, i), i)) return;

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
  if(_addsRow(_isInLocalRange(M, i), i)) {
    tmp = d1 + PETSC_i * d2;
    _addInMatrix(M, i, j, tmp);
  }
#else
  bool iok = _addsRow(_isInLocalRange(M, i), i);
  bool kok = _addsRow(_isInLocalRange(M, k), k);
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
  _flushNonLocalRows(M->M);
  Message::Barrier();
  _try(MatAssemblyBegin(M->M, MAT_FINAL_ASSEMBLY));
  _try(MatAssemblyEnd(M->M, MAT_FINAL_ASSEMBLY));
}

void LinAlg_AssembleVector(gVector *V)
{
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
  // still in use elsewhere
  PetscInt refct;
  _try(PetscObjectGetReference((PetscObject)Pmat, &refct));
  int reuse = (refct <= 2) ? 1 : 0, glob;
  MPI_Allreduce(&reuse, &glob, 1, MPI_INT, MPI_MIN,
                PetscObjectComm((PetscObject)M));
  if(!glob || _getStructure(M, Pmat) != SAME_NONZERO_PATTERN) return M;
  _try(MatCopy(M, Pmat, SAME_NONZERO_PATTERN));
  return Pmat;
}
#endif

#if defined(PETSC_HAVE_HYPRE)
// Give hypre's AMS (Hiptmair-Xu) preconditioner, selected with -pc_type hypre
// -pc_hypre_type ams, the discrete gradient and the node coordinates of the
// current system, whose unknowns must all be edge unknowns of one basis
// function. G has one row per edge unknown and one column per node whose edges
// are all unknowns (the gradient of the other nodes is not in the space, e.g.
// on Dirichlet boundaries). Every rank holds the whole mesh and Dof list, so
// all ranks number the nodes the same way; each sets its own rows. Options read
// here: -getdp_ams_zero_beta for a matrix without mass term, -getdp_ams_check to
// print |A G x| / (|A| |G x|), which is round-off for a correct G.
// The mesh edges of the current system, with their row (-1 if not an
// unknown), and their nodes; a node is outside the space spanned by G when one
// of its edges is not an unknown.
struct AMSEdges {
  struct Edge {
    int num, row, n[2], s[2];
  };
  std::vector<Edge> edges;
  std::unordered_map<int, int> nodeIndex; // node number -> index in nodes
  std::vector<int> nodes, minRow;
  std::vector<char> outside;
};

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
  List_T *E = Tree2List(CurrentGeoData->EdgesXNodes);
  for(int i = 0; i < List_Nbr(E); i++) {
    struct Entity2XEntity1 *e = (struct Entity2XEntity1 *)List_Pointer(E, i);
    if(e->NbrEntities != 2) continue;
    struct Dof *Dof_P = Dof_GetDofStruct(DofData_P, NumType, e->Num, 0);
    AMSEdges::Edge ed;
    ed.num = e->Num;
    ed.row = (Dof_P && (Dof_P->Type == DOF_UNKNOWN ||
                        Dof_P->Type == DOF_UNKNOWN_INIT)) ?
               Dof_P->Case.Unknown.NumDof - 1 :
               -1;
    for(int j = 0; j < 2; j++) {
      int num = abs(e->NumEntities[j]);
      ed.s[j] = e->NumEntities[j] > 0 ? 1 : -1;
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
  List_Delete(E);
  return true;
}

static void _setupAMS(PC pc, Mat A)
{
  AMSEdges a;
  if(!_getAMSEdges(a)) return;
  std::vector<AMSEdges::Edge> &edges = a.edges;
  std::unordered_map<int, int> &nodeIndex = a.nodeIndex;
  std::vector<int> &nodes = a.nodes, &minRow = a.minRow;
  std::vector<char> &outside = a.outside;

  // columns of G: the nodes in the space, owned by the rank of their first
  // edge unknown and numbered by rank, then by node number
  PetscMPIInt size, rank;
  MPI_Comm comm = PetscObjectComm((PetscObject)A);
  MPI_Comm_size(comm, &size);
  MPI_Comm_rank(comm, &rank);
  const PetscInt *ranges;
  _try(MatGetOwnershipRanges(A, &ranges));
  std::vector<std::pair<int, int> > cols; // (owner, node number) -> index
  for(std::size_t i = 0; i < nodes.size(); i++) {
    if(outside[i] || minRow[i] == INT_MAX) continue;
    int owner = std::upper_bound(ranges, ranges + size + 1, minRow[i]) - ranges - 1;
    cols.push_back(std::make_pair(owner, nodes[i]));
  }
  std::sort(cols.begin(), cols.end());
  std::vector<int> col(nodes.size(), -1);
  PetscInt nloc = 0, cstart = -1;
  for(std::size_t i = 0; i < cols.size(); i++) {
    col[nodeIndex[cols[i].second]] = i;
    if(cols[i].first == rank) {
      if(cstart < 0) cstart = i;
      nloc++;
    }
  }

  PetscInt rstart, rend;
  _try(MatGetOwnershipRange(A, &rstart, &rend));
  Mat G;
  _try(MatCreateAIJ(comm, rend - rstart, nloc, PETSC_DETERMINE, cols.size(), 2,
                    PETSC_NULL, 2, PETSC_NULL, &G));
  for(auto &ed : edges) {
    if(ed.row < rstart || ed.row >= rend) continue;
    for(int j = 0; j < 2; j++) {
      PetscInt c = col[ed.n[j]];
      PetscScalar v = ed.s[j];
      if(c >= 0) _try(MatSetValues(G, 1, &ed.row, 1, &c, &v, INSERT_VALUES));
    }
  }
  _try(MatAssemblyBegin(G, MAT_FINAL_ASSEMBLY));
  _try(MatAssemblyEnd(G, MAT_FINAL_ASSEMBLY));

  std::vector<PetscReal> xyz(3 * nloc);
  for(PetscInt i = 0; i < nloc; i++) {
    struct Geo_Node *Geo_Node = Geo_GetGeoNodeOfNum(cols[cstart + i].second);
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
  Message::Info("AMS: discrete gradient with %d edge unknowns and %d nodes%s",
                (int)(ranges[size]), (int)cols.size(),
                zeroBeta ? ", no mass term" : "");

  if(check) {
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
  _try(PetscObjectCompose((PetscObject)pc, "getdp_ams_G", (PetscObject)G));
  _try(MatDestroy(&G)); // referenced by the preconditioner
}

// With -getdp_ams_tree_group <group> (a Group defined with EdgesOfTreeIn),
// bring the solution of the ungauged problem back to the gauge of that tree:
// a <- a - G psi, with psi such that a - G psi vanishes on the tree edges,
// computed by walking the tree from the nodes outside G (psi = 0 there, e.g.
// on Dirichlet boundaries). The curl is unchanged, and the result is the
// solution of the problem gauged by the tree. Every rank has the whole
// solution in Vseq after the solve, so all compute the same psi.
static void _gaugeOnTreeForAMS(PC pc, gVector *X)
{
  char name[256];
  PetscBool set = PETSC_FALSE;
  _try(PetscOptionsGetString(PETSC_NULL, PETSC_NULL, "-getdp_ams_tree_group",
                             name, sizeof(name), &set));
  PetscObject obj = PETSC_NULL;
  _try(PetscObjectQuery((PetscObject)pc, "getdp_ams_G", &obj));
  if(!set || !obj) return;
  int i = List_ISearchSeq(Problem_S.Group, name, fcmp_Group_Name);
  if(i < 0) {
    Message::Error("Unknown group '%s' for -getdp_ams_tree_group", name);
    return;
  }
  struct Group *Group_P = (struct Group *)List_Pointer(Problem_S.Group, i);
  if(Group_P->FunctionType != EDGESOFTREEIN) {
    Message::Error("Group '%s' is not defined with EdgesOfTreeIn", name);
    return;
  }
  if(!Group_P->ExtendedList) Generate_ExtendedGroup(Group_P);
  std::set<int> tree;
  for(int j = 0; j < List_Nbr(Group_P->ExtendedList); j++) {
    int num;
    List_Read(Group_P->ExtendedList, j, &num);
    tree.insert(abs(num));
  }

  AMSEdges a;
  if(!_getAMSEdges(a)) return;
  Vec VV = X->haveSeq ? X->Vseq : X->V;
  const PetscScalar *x;
  _try(VecGetArrayRead(VV, &x));
  // tree adjacency, then psi by breadth-first walks from the nodes outside G
  std::vector<std::vector<int> > adj(a.nodes.size());
  for(std::size_t e = 0; e < a.edges.size(); e++) {
    if(!tree.count(a.edges[e].num)) continue;
    adj[a.edges[e].n[0]].push_back(e);
    adj[a.edges[e].n[1]].push_back(e);
  }
  std::vector<double> psi(a.nodes.size(), 0.);
  std::vector<char> done(a.nodes.size(), 0);
  std::vector<int> queue;
  for(int pass = 0; pass < 2; pass++) {
    // first from the nodes outside G, then from any node left (a tree of its
    // own, with psi = 0 at its first node)
    for(std::size_t n = 0; n < a.nodes.size(); n++) {
      if(done[n] || (!pass && !a.outside[n])) continue;
      done[n] = 1;
      queue.assign(1, n);
      for(std::size_t q = 0; q < queue.size(); q++) {
        int k = queue[q];
        for(int e : adj[k]) {
          const AMSEdges::Edge &ed = a.edges[e];
          int j = (ed.n[0] == k) ? 1 : 0, u = ed.n[j];
          if(done[u]) continue;
          done[u] = 1;
          // (G psi)_e = s_k psi_k + s_u psi_u must equal a_e (0 if fixed),
          // and psi = 0 at nodes outside G
          if(!a.outside[u]) {
            double ae = (ed.row >= 0) ? PetscRealPart(x[ed.row]) : 0.;
            double pk = a.outside[k] ? 0. : psi[k];
            psi[u] = (ae - ed.s[1 - j] * pk) / ed.s[j];
          }
          queue.push_back(u);
        }
      }
    }
  }
  // a <- a - G psi on the local rows
  PetscInt rstart, rend;
  _try(VecGetOwnershipRange(X->V, &rstart, &rend));
  std::vector<PetscInt> rows;
  std::vector<PetscScalar> vals;
  double treeMax = 0.;
  for(auto &ed : a.edges) {
    if(ed.row < rstart || ed.row >= rend) continue;
    double g = 0.;
    for(int j = 0; j < 2; j++)
      if(!a.outside[ed.n[j]]) g += ed.s[j] * psi[ed.n[j]];
    double v = PetscRealPart(x[ed.row]) - g;
    if(tree.count(ed.num)) treeMax = std::max(treeMax, fabs(v));
    rows.push_back(ed.row);
    vals.push_back(v);
  }
  _try(VecRestoreArrayRead(VV, &x));
  _try(VecSetValues(X->V, rows.size(), rows.data(), vals.data(), INSERT_VALUES));
  _try(VecAssemblyBegin(X->V));
  _try(VecAssemblyEnd(X->V));
  _fillseq(X);
  double glob;
  MPI_Allreduce(&treeMax, &glob, 1, MPI_DOUBLE, MPI_MAX,
                PetscObjectComm((PetscObject)X->V));
  Message::Info("AMS: solution gauged on tree '%s' (%d edges), max |a| on the "
                "tree %g", name, (int)tree.size(), glob);
}

// Without a gauge the curl-curl matrix is singular, its kernel being the
// gradients G x, and CG needs a compatible right-hand side, G^T b = 0. With
// -getdp_ams_clean_rhs, remove the incompatible part of b (e.g. from a source
// current density that is not exactly divergence-free):
// b <- b - G (G^T G)^-1 G^T b, with CG + BoomerAMG on the nodal Laplacian G^T G
// (options prefix -getdp_ams_clean_)
static void _cleanRHSForAMS(PC pc, Vec b)
{
  PetscBool clean = PETSC_FALSE;
  _try(PetscOptionsGetBool(PETSC_NULL, PETSC_NULL, "-getdp_ams_clean_rhs",
                           &clean, PETSC_NULL));
  PetscObject obj = PETSC_NULL;
  _try(PetscObjectQuery((PetscObject)pc, "getdp_ams_G", &obj));
  if(!clean || !obj) return;
  Mat G = (Mat)obj, L;
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

  PC pc;

  if(!Solver->ksp[kspIndex]) {
    _try(KSPCreate(MyComm, &Solver->ksp[kspIndex]));
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 5)
    _try(KSPSetOperators(Solver->ksp[kspIndex], A->M, A->M));
#else
    _try(KSPSetOperators(Solver->ksp[kspIndex], A->M, A->M,
                         DIFFERENT_NONZERO_PATTERN));
#endif
    _try(KSPMonitorSet(Solver->ksp[kspIndex], _myKspMonitor, PETSC_NULL,
                       PETSC_NULL));
    _try(KSPGetPC(Solver->ksp[kspIndex], &pc));

    // set some default options: use direct solver (PARDISO, MUMPS, UMFPACK, or
    // native PETSc LU)
    _try(KSPSetType(Solver->ksp[kspIndex], "preonly"));
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
  else if(precond) {
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 5)
    _try(KSPSetReusePreconditioner(Solver->ksp[kspIndex], PETSC_FALSE));
    Mat op = _getOperator(Solver->ksp[kspIndex], A->M);
    _try(KSPSetOperators(Solver->ksp[kspIndex], op, op));
#else
    _try(KSPSetOperators(Solver->ksp[kspIndex], A->M, A->M,
                         DIFFERENT_NONZERO_PATTERN));
#endif
    _try(KSPGetPC(Solver->ksp[kspIndex], &pc));
  }
  else {
#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 5)
    _try(KSPSetReusePreconditioner(Solver->ksp[kspIndex], PETSC_TRUE));
    Mat op = _getOperator(Solver->ksp[kspIndex], A->M);
    _try(KSPSetOperators(Solver->ksp[kspIndex], op, op));
#endif
    _try(KSPGetPC(Solver->ksp[kspIndex], &pc));
  }

  // override the default options with the ones from the option database (if
  // any)
  _try(KSPSetFromOptions(Solver->ksp[kspIndex]));

#if defined(PETSC_HAVE_HYPRE)
  {
    // set up AMS once per preconditioner (_setupAMS attaches G to it)
    PetscBool isHypre;
    PetscObject G = PETSC_NULL;
    _try(PetscObjectTypeCompare((PetscObject)pc, PCHYPRE, &isHypre));
    if(isHypre) _try(PetscObjectQuery((PetscObject)pc, "getdp_ams_G", &G));
    if(isHypre && !G) {
      const char *hypreType = "";
      _try(PCHYPREGetType(pc, &hypreType));
      if(!strcmp(hypreType, "ams")) _setupAMS(pc, A->M);
    }
  }
#endif

#if(PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR >= 4)
  const char *ksptype = "";
  _try(KSPGetType(Solver->ksp[kspIndex], &ksptype));
  const char *pctype = "";
  _try(PCGetType(pc, &pctype));
#else
  const KSPType ksptype;
  _try(KSPGetType(Solver->ksp[kspIndex], &ksptype));
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
  if(!strcmp(pctype, PCHYPRE)) {
    _cleanRHSForAMS(pc, B->V);
    _markseq(B);
  }
#endif
  _try(KSPSolve(Solver->ksp[kspIndex], B->V, X->V));

  if(stype && !strcmp(stype, "mumps")) {
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

  // copy result on all procs
  _fillseq(X);

#if defined(PETSC_HAVE_HYPRE)
  if(!strcmp(pctype, PCHYPRE)) _gaugeOnTreeForAMS(pc, X);
#endif

  if(Message::GetVerbosity() > 5) {
    _try(KSPView(Solver->ksp[kspIndex], MyPetscViewer));
  }

  PetscInt its;
  _try(KSPGetIterationNumber(Solver->ksp[kspIndex], &its));
  if(its > 1) Message::Info("%d iterations", its);
  Current.KSPIterations = its;

  KSPConvergedReason reason;
  _try(KSPGetConvergedReason(Solver->ksp[kspIndex], &reason));
  Current.KSPConvergedReason = (double)reason;

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
  gx.haveSeq = 0;
  gf.V = f;
  gf.haveSeq = 0;
  Generate_Residual(&gx, &gf);

  PetscScalar *ff;
  _try(VecGetArray(gf.V, &ff));
  PetscInt n;
  _try(VecGetSize(f, &n));
  for(PetscInt i = 0; i < n; i++)
    _try(VecSetValues(f, 1, &i, &ff[i], INSERT_VALUES));
  _try(VecGetArray(f, &ff));
  return 0;
}

#if(PETSC_VERSION_MAJOR == 2) ||                                               \
  ((PETSC_VERSION_MAJOR == 3) && (PETSC_VERSION_MINOR < 5))
static PetscErrorCode _NLFormJacobian(SNES snes, Vec x, Mat *J, Mat *PC,
                                      MatStructure *flag, void *mctx)
{
  gVector gx;
  gx.V = x;
  gx.haveSeq = 0;
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
static PetscErrorCode _NLFormJacobian(SNES snes, Vec x, Mat J, Mat PC,
                                      void *mctx)
{
  gVector gx;
  gx.V = x;
  gx.haveSeq = 0;
  gMatrix gJ;
  Generate_FullJacobian(&gx, &gJ);
  // J = gJ.M;
  MatCopy(gJ.M, J, SAME_NONZERO_PATTERN);
  // Message::Barrier();
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

  // copy result on all procs
  _fillseq(X);

  if(Message::GetVerbosity() > 5)
    _try(SNESView(Solver->snes[solverIndex], MyPetscViewer));

  PetscInt its;
  _try(SNESGetIterationNumber(Solver->snes[solverIndex], &its));
  Message::Info("Number of Newton iterations %d", its);
}

void LinAlg_SolveNL(gMatrix *A, gVector *B, gMatrix *J, gVector *R,
                    gSolver *Solver, gVector *X, int solverIndex)
{
  _solveNL(A, B, J, R, Solver, X, 1, solverIndex);
}

#endif
