// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.
//
// Contributor(s):
//   Johan Gyselinck
//   Ruth Sabariego
//

#include <map>
#include <stdarg.h>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <stdint.h>
#include <stdlib.h>
#include <set>
#include <complex>
#include <sstream>
#include <string.h>
#include <math.h>
#include "GetDPVersion.h"
#include "ProData.h"
#include "DofData.h"
#include "GeoData.h"
#include "ExtendedGroup.h"
#include "ListUtils.h"
#include "TreeUtils.h"
#include "MallocUtils.h"
#include "Message.h"
#include "ProParser.h"
#include "OS.h"

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

extern int Flag_LOCAL_DOFS;

#define TWO_PI 6.2831853071795865

extern struct Problem Problem_S;
extern struct CurrentData Current;

FILE *File_PRE = 0, *File_RES = 0, *File_TMP = 0;

struct DofData *CurrentDofData;

// Hash index of the Dofs of a DofData, i.e. of its DofTree while the Dofs are
// being defined and of its sorted DofList afterwards: looking Dofs up in the
// avl tree or by binary search in the list is cache-unfriendly and dominated
// the pre-processing, assembly and post-processing times on large meshes. The
// index is rebuilt (lazily) when its container changes, and grown when Dofs are
// added to the tree with Dof_Add.
struct DofIndex {
  const void *container; // DofTree or DofList
  int n; // number of Dofs in the index
  std::vector<struct Dof *> slots; // open addressing, linear probing
};

static inline uint64_t Dof_Hash(const struct Dof *Dof_P)
{
  uint64_t h = (uint64_t)(uint32_t)Dof_P->Entity * 0x9E3779B97F4A7C15ULL;
  h ^= ((uint64_t)(uint32_t)Dof_P->NumType << 32 |
        (uint64_t)(uint32_t)Dof_P->Harmonic) * 0xC2B2AE3D27D4EB4FULL;
  return h ^ (h >> 29);
}

static void DofIndex_Insert(struct DofIndex *I, struct Dof *Dof_P)
{
  size_t mask = I->slots.size() - 1, i = Dof_Hash(Dof_P) & mask;
  while(I->slots[i]) i = (i + 1) & mask;
  I->slots[i] = Dof_P;
  I->n++;
}

static void DofIndex_Reserve(struct DofIndex *I, int n)
{
  // keep the load factor below 1/2
  size_t size = 1024;
  while(size < 2 * (size_t)n) size *= 2;
  if(size <= I->slots.size()) return;
  std::vector<struct Dof *> old;
  old.swap(I->slots);
  I->slots.assign(size, (struct Dof *)NULL);
  I->n = 0;
  for(auto Dof_P : old)
    if(Dof_P) DofIndex_Insert(I, Dof_P);
}

static struct DofIndex *DofIndex_Reset(struct DofData *DofData_P,
                                       const void *container, int n)
{
  if(!DofData_P->DofIndex) DofData_P->DofIndex = new struct DofIndex;
  struct DofIndex *I = DofData_P->DofIndex;
  I->container = container;
  I->n = 0;
  I->slots.clear();
  DofIndex_Reserve(I, n);
  return I;
}

static struct DofIndex *DofIndex_Tree;
static void DofIndex_InsertFromTree(void *a, void *b)
{
  DofIndex_Insert(DofIndex_Tree, (struct Dof *)a);
}

// index of the DofTree (if tree is true) or of the DofList of DofData_P, valid
// as long as the container is not modified other than by Dof_Add
static struct DofIndex *Dof_GetIndex(struct DofData *DofData_P, bool tree)
{
  struct DofIndex *I = DofData_P->DofIndex;
  if(tree) {
    if(!DofData_P->DofTree) return NULL;
    int n = Tree_Nbr(DofData_P->DofTree);
    if(I && I->container == DofData_P->DofTree && I->n == n) return I;
    I = DofIndex_Reset(DofData_P, DofData_P->DofTree, n);
    DofIndex_Tree = I;
    Tree_Action(DofData_P->DofTree, DofIndex_InsertFromTree);
  }
  else {
    if(!DofData_P->DofList) return NULL;
    int n = List_Nbr(DofData_P->DofList);
    if(I && I->container == DofData_P->DofList && I->n == n) return I;
    I = DofIndex_Reset(DofData_P, DofData_P->DofList, n);
    for(int i = 0; i < n; i++)
      DofIndex_Insert(I, (struct Dof *)List_Pointer(DofData_P->DofList, i));
  }
  return I;
}

// replaces Tree_PQuery(DofData_P->DofTree, Dof_P) if tree is true, and
// List_PQuery(DofData_P->DofList, Dof_P, fcmp_Dof) otherwise
static struct Dof *Dof_Find(struct DofData *DofData_P, struct Dof *Dof_P,
                            bool tree)
{
  struct DofIndex *I = Dof_GetIndex(DofData_P, tree);
  if(!I) return NULL;
  size_t mask = I->slots.size() - 1, i = Dof_Hash(Dof_P) & mask;
  while(struct Dof *D = I->slots[i]) {
    if(D->NumType == Dof_P->NumType && D->Entity == Dof_P->Entity &&
       D->Harmonic == Dof_P->Harmonic)
      return D;
    i = (i + 1) & mask;
  }
  return NULL;
}

// replaces Tree_Add(DofData_P->DofTree, Dof_P), keeping the index up to date
static struct Dof *Dof_Add(struct DofData *DofData_P, struct Dof *Dof_P)
{
  struct DofIndex *I = Dof_GetIndex(DofData_P, true);
  struct Dof *D = (struct Dof *)Tree_Add(DofData_P->DofTree, Dof_P);
  DofIndex_Reserve(I, I->n + 1);
  DofIndex_Insert(I, D);
  return D;
}

int fcmp_Dof(const void *a, const void *b)
{
  int Result;

  if((Result = ((struct Dof *)a)->NumType - ((struct Dof *)b)->NumType) != 0)
    return Result;
  if((Result = ((struct Dof *)a)->Entity - ((struct Dof *)b)->Entity) != 0)
    return Result;
  return ((struct Dof *)a)->Harmonic - ((struct Dof *)b)->Harmonic;
}

/* ------------------------------------------------------------------------ */
/*  D o f _ I n i t D o f D a t a                                           */
/* ------------------------------------------------------------------------ */

void Dof_InitDofData(struct DofData *DofData_P, int Num, int ResolutionIndex,
                     int SystemIndex, char *Name_SolverDataFile)
{
  int Index;

  DofData_P->Num = Num;

  DofData_P->ResolutionIndex = ResolutionIndex;
  DofData_P->SystemIndex = SystemIndex;

  DofData_P->FunctionSpaceIndex = NULL;
  DofData_P->TimeFunctionIndex = List_Create(10, 5, sizeof(int));
  Index = 0;
  List_Add(DofData_P->TimeFunctionIndex, &Index);
  DofData_P->Pulsation = NULL;
  DofData_P->Val_Pulsation = NULL;
  DofData_P->NbrHar = 1;

  DofData_P->NbrAnyDof = 0;
  DofData_P->NbrDof = 0;
  DofData_P->DofTree = Tree_Create(sizeof(struct Dof), fcmp_Dof);
  DofData_P->DofList = NULL;
  DofData_P->DofIndex = NULL;

  DofData_P->SolverDataFileName = Name_SolverDataFile;
  DofData_P->Flag_RHS = 0;
  DofData_P->Flag_ListOfRHS = 0;
  DofData_P->CounterOfRHS = 0;
  DofData_P->TotalNumberOfRHS = 0;

  DofData_P->Flag_Init[0] = 0;
  DofData_P->Flag_Init[1] = 0;
  DofData_P->Flag_Init[2] = 0;
  DofData_P->Flag_Init[3] = 0;
  DofData_P->Flag_Init[4] = 0;
  DofData_P->Flag_Init[5] = 0;
  DofData_P->Flag_Init[6] = 0;
  DofData_P->Flag_Init[7] = 0;

  DofData_P->Flag_Only = 0;
  DofData_P->Flag_InitOnly[0] = 0;
  DofData_P->Flag_InitOnly[1] = 0;
  DofData_P->Flag_InitOnly[2] = 0;

  DofData_P->OnlyTheseMatrices = NULL;

  DofData_P->Solutions = NULL;
  DofData_P->CurrentSolution = NULL;

  DofData_P->CorrectionSolutions.Flag = 0;
  DofData_P->CorrectionSolutions.AllSolutions = NULL;

  DofData_P->DummyDof = NULL;

}

/* ------------------------------------------------------------------------ */
/*  D o f _ F r e e D o f D a t a                                           */
/* ------------------------------------------------------------------------ */

void Dof_FreeDofData(struct DofData *DofData_P)
{
  Message::Debug("Freeing DofData %d", DofData_P->Num);

  List_Delete(DofData_P->FunctionSpaceIndex);
  List_Delete(DofData_P->TimeFunctionIndex);
  List_Delete(DofData_P->Pulsation);
  Tree_Delete(DofData_P->DofTree);
  List_Delete(DofData_P->DofList);
  delete DofData_P->DofIndex;
  DofData_P->DofIndex = NULL;
  Free(DofData_P->DummyDof);

  if(DofData_P->Solutions) {
    for(int i = 0; i < List_Nbr(DofData_P->Solutions); i++) {
      Solution *Solution_P =
        (struct Solution *)List_Pointer(DofData_P->Solutions, i);
      if(Solution_P->SolutionExist) {
        LinAlg_DestroyVector(&Solution_P->x);
        Free(Solution_P->TimeFunctionValues);
        Solution_P->TimeFunctionValues = NULL;
        Solution_P->SolutionExist = 0;
      }
    }
    List_Delete(DofData_P->Solutions);
  }

  List_Delete(DofData_P->OnlyTheseMatrices);

  if(DofData_P->Flag_Init[0] == 1 || DofData_P->Flag_Init[0] == 2) {
    LinAlg_DestroyMatrix(&DofData_P->A);
    LinAlg_DestroyVector(&DofData_P->b);
    LinAlg_DestroyVector(&DofData_P->res);
    LinAlg_DestroyVector(&DofData_P->dx);
    LinAlg_DestroySolver(&DofData_P->Solver);
  }

  if(DofData_P->Flag_Init[0] == 2) { LinAlg_DestroyMatrix(&DofData_P->Jac); }

  if(DofData_P->Flag_Init[1] == 1) {
    LinAlg_DestroyMatrix(&DofData_P->M1);
    LinAlg_DestroyVector(&DofData_P->m1);
    for(int i = 0; i < List_Nbr(DofData_P->m1s); i++)
      LinAlg_DestroyVector((gVector *)List_Pointer(DofData_P->m1s, i));
    List_Delete(DofData_P->m1s);
  }

  if(DofData_P->Flag_Init[2] == 1) {
    LinAlg_DestroyMatrix(&DofData_P->M2);
    LinAlg_DestroyVector(&DofData_P->m2);
    for(int i = 0; i < List_Nbr(DofData_P->m2s); i++)
      LinAlg_DestroyVector((gVector *)List_Pointer(DofData_P->m2s, i));
    List_Delete(DofData_P->m2s);
  }

  if(DofData_P->Flag_Init[3] == 1) {
    LinAlg_DestroyMatrix(&DofData_P->M3);
    LinAlg_DestroyVector(&DofData_P->m3);
    for(int i = 0; i < List_Nbr(DofData_P->m3s); i++)
      LinAlg_DestroyVector((gVector *)List_Pointer(DofData_P->m3s, i));
    List_Delete(DofData_P->m3s);
  }

  if(DofData_P->Flag_Init[4] == 1) {
    LinAlg_DestroyMatrix(&DofData_P->M4);
    LinAlg_DestroyVector(&DofData_P->m4);
    for(int i = 0; i < List_Nbr(DofData_P->m4s); i++)
      LinAlg_DestroyVector((gVector *)List_Pointer(DofData_P->m4s, i));
    List_Delete(DofData_P->m4s);
  }

  if(DofData_P->Flag_Init[5] == 1) {
    LinAlg_DestroyMatrix(&DofData_P->M5);
    LinAlg_DestroyVector(&DofData_P->m5);
    for(int i = 0; i < List_Nbr(DofData_P->m5s); i++)
      LinAlg_DestroyVector((gVector *)List_Pointer(DofData_P->m5s, i));
    List_Delete(DofData_P->m5s);
  }

  if(DofData_P->Flag_Init[6] == 1) {
    LinAlg_DestroyMatrix(&DofData_P->M6);
    LinAlg_DestroyVector(&DofData_P->m6);
    for(int i = 0; i < List_Nbr(DofData_P->m6s); i++)
      LinAlg_DestroyVector((gVector *)List_Pointer(DofData_P->m6s, i));
    List_Delete(DofData_P->m6s);
  }
  // nleigchange
  if(DofData_P->Flag_Init[7] == 1) {
    LinAlg_DestroyMatrix(&DofData_P->M7);
    LinAlg_DestroyVector(&DofData_P->m7);
    for(int i = 0; i < List_Nbr(DofData_P->m7s); i++)
      LinAlg_DestroyVector((gVector *)List_Pointer(DofData_P->m7s, i));
    List_Delete(DofData_P->m7s);
  }

  if(DofData_P->Flag_Only) {
    if(DofData_P->Flag_InitOnly[0] == 1) {
      LinAlg_DestroyMatrix(&DofData_P->A1);
      LinAlg_DestroyVector(&DofData_P->b1);
    }
    if(DofData_P->Flag_InitOnly[1] == 1) {
      LinAlg_DestroyMatrix(&DofData_P->A2);
      LinAlg_DestroyVector(&DofData_P->b2);
    }
    if(DofData_P->Flag_InitOnly[2] == 1) {
      LinAlg_DestroyMatrix(&DofData_P->A3);
      LinAlg_DestroyVector(&DofData_P->b3);
    }
  }


  // TODO: handle MH data and CorrectionSolutions
}

/* ------------------------------------------------------------------------ */
/*  D o f _ S e t C u r r e n t D o f D a t a                               */
/* ------------------------------------------------------------------------ */

void Dof_SetCurrentDofData(struct DofData *DofData_P)
{
  CurrentDofData = DofData_P;
}

/* ------------------------------------------------------------------------ */
/*  F i l e s   . . .                                                       */
/* ------------------------------------------------------------------------ */

// In parallel, when each rank only has the Dofs and the elements of its part of
// the mesh, the .pre file is made of sections written by the ranks, one after
// the other: for each system the Dofs of each rank, then the edges and facets
// of the elements of each rank (see Geo_WriteFilePRE). The number of sections
// follows the number of systems in the file (it is 1 without it); a Dof that
// several ranks have is in the section of each of them. The ranks prepare
// their sections in SectionPRE and write them at the same time (MPI-IO).
static std::string Name_PRE, SectionPRE;
static int NbrSectionsPRE = 1;

void Dof_PrintfPRE(const char *fmt, ...)
{
  char str[1024];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(str, sizeof(str), fmt, args);
  va_end(args);
  SectionPRE.append(str, std::min(n, (int)sizeof(str) - 1));
}

// writes the sections prepared by all the ranks (byRank), or the one of the
// rank that writes the file
void Dof_WriteSectionPRE(bool byRank)
{
  if(byRank)
    Message::WriteParallelSections(File_PRE, Name_PRE.c_str(), SectionPRE);
  else if(File_PRE && !SectionPRE.empty())
    fwrite(&SectionPRE[0], 1, SectionPRE.size(), File_PRE);
  std::string().swap(SectionPRE);
}

static bool Dof_SectionsByRankPRE()
{
  return Dof_UseLocalDofs() && !Dof_GetAllDofsOnFirstRank();
}

/* ------------------------------------------------------------------------ */
/*  D o f _ O p e n F i l e                                                 */
/* ------------------------------------------------------------------------ */

void Dof_OpenFile(int Type, char *Name, const char *Mode)
{
  // all the ranks need the name of the file they write together
  if(Type == DOF_PRE) Name_PRE = std::string(Name) + ".pre";

  if((Message::GetIsCommWorld() && Message::GetCommRank()) &&
     (Mode[0] == 'w' || Mode[0] == 'a')) {
    switch(Type) {
    case DOF_PRE: File_PRE = 0; break;
    case DOF_RES: File_RES = 0; break;
    case DOF_TMP: File_RES = 0; break;
    default: break;
    }
    return;
  }

  const char *Extension;
  char FileName[256];
  FILE *File_X;

  switch(Type) {
  case DOF_PRE: Extension = ".pre"; break;
  case DOF_RES: Extension = ""; break;
  case DOF_TMP: Extension = ""; break;
  default: Extension = ".pre"; break;
  }

  strcpy(FileName, Name);
  strcat(FileName, Extension);

  if(!(File_X = FOpen(FileName, Mode)))
    Message::Error("Unable to open file '%s'", FileName);

  switch(Type) {
  case DOF_PRE: File_PRE = File_X; break;
  case DOF_RES: File_RES = File_X; break;
  case DOF_TMP:
    File_TMP = File_RES;
    File_RES = File_X;
    break;
  default: break;
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ C l o s e F i l e                                               */
/* ------------------------------------------------------------------------ */

void Dof_CloseFile(int Type)
{
  switch(Type) {
  case DOF_PRE:
    if(File_PRE) fclose(File_PRE);
    break;
  case DOF_RES:
    if(File_RES) fclose(File_RES);
    break;
  case DOF_TMP:
    if(File_RES) fclose(File_RES);
    File_RES = File_TMP;
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ F l u s h F i l e                                               */
/* ------------------------------------------------------------------------ */

void Dof_FlushFile(int Type)
{
  switch(Type) {
  case DOF_PRE: fflush(File_PRE); break;
  case DOF_RES: fflush(File_RES); break;
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ W r i t e F i l e P R E 0                                       */
/* ------------------------------------------------------------------------ */

void Dof_WriteFilePRE0(int Num_Resolution, char *Name_Resolution,
                       int Nbr_DofData)
{
  if(Message::GetIsCommWorld() && Message::GetCommRank()) return;

  fprintf(File_PRE, "$Resolution /* '%s' */\n", Name_Resolution);
  fprintf(File_PRE, "%d %d %d\n", Num_Resolution, Nbr_DofData,
          Dof_SectionsByRankPRE() ? Message::GetCommSize() : 1);
  fprintf(File_PRE, "$EndResolution\n");
}

/* ------------------------------------------------------------------------ */
/*  D o f _ R e a d F i l e P R E 0                                         */
/* ------------------------------------------------------------------------ */

void Dof_ReadFilePRE0(int *Num_Resolution, int *Nbr_DofData)
{
  Message::Barrier();

  char String[256];

  do {
    if(!fgets(String, sizeof(String), File_PRE)) break;
    if(feof(File_PRE)) break;
  } while(String[0] != '$');

  if(feof(File_PRE)) {
    Message::Error("$Resolution field not found in file");
    return;
  }

  if(!strncmp(&String[1], "Resolution", 10)) {
    NbrSectionsPRE = 1;
    if(!fgets(String, sizeof(String), File_PRE) ||
       sscanf(String, "%d %d %d", Num_Resolution, Nbr_DofData,
              &NbrSectionsPRE) < 2) {
      Message::Error("Could not read resolution data");
      return;
    }
  }

  do {
    if(!fgets(String, sizeof(String), File_PRE) || feof(File_PRE)) {
      Message::Error("Premature end of file");
      break;
    }
  } while(String[0] != '$');
}

// Binary format of the Dofs in .pre files (written with -bin): for each Dof,
// the 6 integers {NumType, Entity, Harmonic, Type, a, b}, followed by the
// doubles d of its type, scalars being always stored as (real, imaginary):
//   DOF_UNKNOWN:             a = NumDof, b = PartitionOrNonLocal
//   DOF_FIXEDWITHASSOCIATE:  a = NumDof, b = TimeFunctionIndex, d = Val
//   DOF_FIXED:               b = TimeFunctionIndex, d = Val
//   DOF_FIXED_SOLVE:         b = TimeFunctionIndex
//   DOF_UNKNOWN_INIT:        a = NumDof, b = PartitionOrNonLocal, d = Val, Val2
//   DOF_LINK, DOF_LINKCPLX:  b = EntityRef, d = Coef, Coef2
static int Dof_BinaryPRE = 0;

static int Dof_NbrDoublesPRE(int Type)
{
  switch(Type) {
  case DOF_FIXEDWITHASSOCIATE:
  case DOF_FIXED:
  case DOF_LINK:
  case DOF_LINKCPLX: return 2;
  case DOF_UNKNOWN_INIT: return 4;
  default: return 0;
  }
}

static void Dof_GetScalarPRE(gScalar *S, double *d)
{
  d[1] = 0.;
  if(gSCALAR_SIZE == 2)
    LinAlg_GetComplexInScalar(&d[0], &d[1], S);
  else
    LinAlg_GetDoubleInScalar(&d[0], S);
}

static void Dof_WriteDofBinaryPRE(struct Dof *Dof_P)
{
  int i[6] = {Dof_P->NumType, Dof_P->Entity, Dof_P->Harmonic, Dof_P->Type, 0,
              0};
  double d[4] = {0., 0., 0., 0.};
  switch(Dof_P->Type) {
  case DOF_UNKNOWN:
    i[4] = Dof_P->Case.Unknown.NumDof;
    i[5] = Dof_P->Case.Unknown.PartitionOrNonLocal;
    break;
  case DOF_FIXEDWITHASSOCIATE:
    i[4] = Dof_P->Case.FixedAssociate.NumDof;
    i[5] = Dof_P->Case.FixedAssociate.TimeFunctionIndex;
    Dof_GetScalarPRE(&Dof_P->Val, d);
    break;
  case DOF_FIXED:
    i[5] = Dof_P->Case.FixedAssociate.TimeFunctionIndex;
    Dof_GetScalarPRE(&Dof_P->Val, d);
    break;
  case DOF_FIXED_SOLVE:
    i[5] = Dof_P->Case.FixedAssociate.TimeFunctionIndex;
    break;
  case DOF_UNKNOWN_INIT:
    i[4] = Dof_P->Case.Unknown.NumDof;
    i[5] = Dof_P->Case.Unknown.PartitionOrNonLocal;
    Dof_GetScalarPRE(&Dof_P->Val, d);
    Dof_GetScalarPRE(&Dof_P->Val2, d + 2);
    break;
  case DOF_LINK:
  case DOF_LINKCPLX:
    i[5] = Dof_P->Case.Link.EntityRef;
    d[0] = Dof_P->Case.Link.Coef;
    d[1] = (Dof_P->Type == DOF_LINKCPLX) ? Dof_P->Case.Link.Coef2 : 0.;
    break;
  }
  SectionPRE.append((const char *)i, 6 * sizeof(int));
  SectionPRE.append((const char *)d,
                    Dof_NbrDoublesPRE(Dof_P->Type) * sizeof(double));
}

static bool Dof_ReadDofBinaryPRE(struct Dof *Dof_P)
{
  int i[6];
  double d[4];
  if(fread(i, sizeof(int), 6, File_PRE) != 6) return false;
  Dof_P->NumType = i[0];
  Dof_P->Entity = i[1];
  Dof_P->Harmonic = i[2];
  Dof_P->Type = i[3];
  int n = Dof_NbrDoublesPRE(Dof_P->Type);
  if(n && (int)fread(d, sizeof(double), n, File_PRE) != n) return false;
  switch(Dof_P->Type) {
  case DOF_UNKNOWN:
    Dof_P->Case.Unknown.NumDof = i[4];
    Dof_P->Case.Unknown.PartitionOrNonLocal = i[5];
    break;
  case DOF_FIXEDWITHASSOCIATE:
    Dof_P->Case.FixedAssociate.NumDof = i[4];
    Dof_P->Case.FixedAssociate.TimeFunctionIndex = i[5];
    LinAlg_SetScalar(&Dof_P->Val, d);
    break;
  case DOF_FIXED:
  case DOF_FIXED_SOLVE:
    Dof_P->Case.FixedAssociate.TimeFunctionIndex = i[5];
    if(n) LinAlg_SetScalar(&Dof_P->Val, d);
    break;
  case DOF_UNKNOWN_INIT:
    Dof_P->Case.Unknown.NumDof = i[4];
    Dof_P->Case.Unknown.PartitionOrNonLocal = i[5];
    LinAlg_SetScalar(&Dof_P->Val, d);
    LinAlg_SetScalar(&Dof_P->Val2, d + 2);
    break;
  case DOF_LINK:
  case DOF_LINKCPLX:
    Dof_P->Case.Link.EntityRef = i[5];
    Dof_P->Case.Link.Coef = d[0];
    Dof_P->Case.Link.Coef2 = d[1];
    Dof_P->Case.Link.Dof = NULL;
    break;
  }
  return true;
}

/* ------------------------------------------------------------------------ */
/*  D o f _ W r i t e F i l e P R E                                         */
/* ------------------------------------------------------------------------ */

void Dof_WriteFilePRE(struct DofData *DofData_P)
{
  // each rank writes its Dofs, or the rank that writes the file writes them all
  const bool byRank = Dof_SectionsByRankPRE();
  if(!byRank && Message::GetIsCommWorld() && Message::GetCommRank()) return;

  int i, Nbr_Index;
  struct Dof *Dof_P0;

  Dof_PrintfPRE("$DofData /* #%d */\n", DofData_P->Num);

  Dof_PrintfPRE("%d %d\n", DofData_P->ResolutionIndex,
          DofData_P->SystemIndex);

  Nbr_Index = List_Nbr(DofData_P->FunctionSpaceIndex);
  Dof_PrintfPRE("%d", Nbr_Index);
  for(i = 0; i < Nbr_Index; i++)
    Dof_PrintfPRE(" %d",
            *((int *)List_Pointer(DofData_P->FunctionSpaceIndex, i)));
  Dof_PrintfPRE("\n");

  Nbr_Index = List_Nbr(DofData_P->TimeFunctionIndex);
  Dof_PrintfPRE("%d", Nbr_Index);
  for(i = 0; i < Nbr_Index; i++)
    Dof_PrintfPRE(" %d",
            *((int *)List_Pointer(DofData_P->TimeFunctionIndex, i)));
  Dof_PrintfPRE("\n");

  Dof_PrintfPRE("%d", 1);
  for(i = 0; i < 1; i++) Dof_PrintfPRE(" %d", 0);
  Dof_PrintfPRE("\n");

  // with -bin, the Dofs are written in binary (see Dof_WriteDofPRE)
  extern int Flag_BIN;
  Dof_BinaryPRE = Flag_BIN;
  Dof_PrintfPRE("%d %d%s\n",
          (DofData_P->DofTree) ? Tree_Nbr(DofData_P->DofTree) :
                                 DofData_P->NbrAnyDof,
          DofData_P->NbrDof, Dof_BinaryPRE ? " binary" : "");

  if(DofData_P->DofTree)
    Tree_Action(DofData_P->DofTree, Dof_WriteDofPRE);
  else {
    if(DofData_P->NbrAnyDof) {
      Dof_P0 = (struct Dof *)List_Pointer(DofData_P->DofList, 0);
      for(i = 0; i < DofData_P->NbrAnyDof; i++)
        Dof_WriteDofPRE(Dof_P0 + i, NULL);
    }
  }
  if(Dof_BinaryPRE) Dof_PrintfPRE("\n");
  Dof_PrintfPRE("$EndDofData\n");
  Dof_WriteSectionPRE(byRank);
}

/* ------------------------------- */
/*  D o f _ W r i t e D o f P R E  */
/* ------------------------------- */

static void Dof_PrintScalarPRE(gScalar *S)
{
  double d[2];
  Dof_GetScalarPRE(S, d);
  if(gSCALAR_SIZE == 2)
    Dof_PrintfPRE("%.16g %.16g", d[0], d[1]);
  else
    Dof_PrintfPRE("%.16g", d[0]);
}

void Dof_WriteDofPRE(void *a, void *b)
{
  struct Dof *Dof_P;

  Dof_P = (struct Dof *)a;

  if(Dof_BinaryPRE) {
    Dof_WriteDofBinaryPRE(Dof_P);
    return;
  }

  Dof_PrintfPRE("%d %d %d %d ", Dof_P->NumType, Dof_P->Entity,
          Dof_P->Harmonic, Dof_P->Type);

  switch(Dof_P->Type) {
  case DOF_UNKNOWN:
    Dof_PrintfPRE("%d %d\n", Dof_P->Case.Unknown.NumDof,
            Dof_P->Case.Unknown.PartitionOrNonLocal);
    break;
  case DOF_FIXEDWITHASSOCIATE:
    Dof_PrintfPRE("%d ", Dof_P->Case.FixedAssociate.NumDof);
    Dof_PrintScalarPRE(&Dof_P->Val);
    Dof_PrintfPRE(" %d\n", Dof_P->Case.FixedAssociate.TimeFunctionIndex);
    break;
  case DOF_FIXED:
    Dof_PrintScalarPRE(&Dof_P->Val);
    Dof_PrintfPRE(" %d\n", Dof_P->Case.FixedAssociate.TimeFunctionIndex);
    break;
  case DOF_FIXED_SOLVE:
    Dof_PrintfPRE("%d\n", Dof_P->Case.FixedAssociate.TimeFunctionIndex);
    break;
  case DOF_UNKNOWN_INIT:
    Dof_PrintfPRE("%d ", Dof_P->Case.Unknown.NumDof);
    Dof_PrintScalarPRE(&Dof_P->Val);
    Dof_PrintfPRE(" ");
    Dof_PrintScalarPRE(&Dof_P->Val2);
    Dof_PrintfPRE(" %d\n", Dof_P->Case.Unknown.PartitionOrNonLocal);
    break;
  case DOF_LINK:
    Dof_PrintfPRE("%.16g %d\n", Dof_P->Case.Link.Coef,
            Dof_P->Case.Link.EntityRef);
    break;
  case DOF_LINKCPLX:
    Dof_PrintfPRE("%.16g %.16g %d\n", Dof_P->Case.Link.Coef,
            Dof_P->Case.Link.Coef2, Dof_P->Case.Link.EntityRef);
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ R e a d F i l e P R E                                           */
/* ------------------------------------------------------------------------ */

// Reads the Dofs of a section and adds them to the list; last is the last
// unknown of each mesh partition.
static bool Dof_ReadSectionPRE(struct DofData *DofData_P, bool first,
                               std::map<int, int> &last)
{
  int i, Nbr_Index, Int, Dummy, Nbr_Dof;
  struct Dof Dof;
  char String[256];

  auto unknown = [&](struct Dof &Dof) {
    if(Dof.Case.Unknown.PartitionOrNonLocal < 0)
      DofData_P->NonLocalEquations.push_back(Dof.Case.Unknown.NumDof);
    else {
      int &l = last[Dof.Case.Unknown.PartitionOrNonLocal];
      l = std::max(l, Dof.Case.Unknown.NumDof);
    }
  };

  do {
    if(!fgets(String, sizeof(String), File_PRE)) break;
    if(feof(File_PRE)) break;
  } while(String[0] != '$');

  if(feof(File_PRE)) {
    Message::Error("$DofData field not found in file");
    return false;
  }

  if(!strncmp(&String[1], "DofData", 7)) {
    if(fscanf(File_PRE, "%d %d", &DofData_P->ResolutionIndex,
              &DofData_P->SystemIndex) != 2) {
      Message::Error("Could not read Resolution and DofData indices");
      return false;
    }

    if(fscanf(File_PRE, "%d", &Nbr_Index) != 1) {
      Message::Error("Could not read number of function spaces");
      return false;
    }
    if(!first) List_Delete(DofData_P->FunctionSpaceIndex);
    DofData_P->FunctionSpaceIndex = List_Create(Nbr_Index, 1, sizeof(int));
    for(i = 0; i < Nbr_Index; i++) {
      if(fscanf(File_PRE, "%d", &Int) != 1) {
        Message::Error("Could not read FunctionSpace index");
        return false;
      }
      List_Add(DofData_P->FunctionSpaceIndex, &Int);
    }

    if(fscanf(File_PRE, "%d", &Nbr_Index) != 1) {
      Message::Error("Could not read number of time functions");
      return false;
    }
    if(!first) List_Delete(DofData_P->TimeFunctionIndex);
    DofData_P->TimeFunctionIndex = List_Create(Nbr_Index, 1, sizeof(int));
    for(i = 0; i < Nbr_Index; i++) {
      if(fscanf(File_PRE, "%d", &Int) != 1) {
        Message::Error("Could not read TimeFunction index");
        return false;
      }
      List_Add(DofData_P->TimeFunctionIndex, &Int);
    }

    if(fscanf(File_PRE, "%d", &Dummy) != 1) {
      Message::Error("Format error");
      return false;
    }
    for(i = 0; i < 1; i++) {
      if(fscanf(File_PRE, "%d", &Dummy) != 1) {
        Message::Error("Format error");
        return false;
      }
    }

    if(fscanf(File_PRE, "%d %d", &Nbr_Dof, &DofData_P->NbrDof) != 2) {
      Message::Error("Could not read number of dofs");
      return false;
    }
    // the rest of the line tells if the Dofs are in binary
    if(!fgets(String, sizeof(String), File_PRE)) {
      Message::Error("Could not read number of dofs");
      return false;
    }
    bool binary = strstr(String, "binary");

    if(first) {
      DofData_P->DofList = List_Create(Nbr_Dof + 1, 1, sizeof(struct Dof));
      Tree_Delete(DofData_P->DofTree);
      DofData_P->DofTree = NULL;
      delete DofData_P->DofIndex;
      DofData_P->DofIndex = NULL;
    }

    for(i = 0; i < Nbr_Dof; i++) {
      if(binary) {
        if(!Dof_ReadDofBinaryPRE(&Dof)) {
          Message::Error("Could not read dof");
          return false;
        }
        if(Dof.Type == DOF_UNKNOWN || Dof.Type == DOF_UNKNOWN_INIT) unknown(Dof);
        List_Add(DofData_P->DofList, &Dof);
        continue;
      }

      if(fscanf(File_PRE, "%d %d %d %d", &Dof.NumType, &Dof.Entity,
                &Dof.Harmonic, &Dof.Type) != 4) {
        Message::Error("Could not dof");
        return false;
      }

      switch(Dof.Type) {
      case DOF_UNKNOWN:
        if(fscanf(File_PRE, "%d %d", &Dof.Case.Unknown.NumDof,
                  &Dof.Case.Unknown.PartitionOrNonLocal) != 2) {
          Message::Error("Could not read DOF_UNKNOWN");
          return false;
        }
        unknown(Dof);
        break;
      case DOF_FIXEDWITHASSOCIATE:
        if(fscanf(File_PRE, "%d", &Dof.Case.FixedAssociate.NumDof) != 1) {
          Message::Error("Could not read DOF_FIXEDWITHASSOCIATE");
          return false;
        }
        LinAlg_ScanScalar(File_PRE, &Dof.Val);
        if(fscanf(File_PRE, "%d", &Dof.Case.FixedAssociate.TimeFunctionIndex) !=
           1) {
          Message::Error("Could not read DOF_FIXEDWITHASSOCIATE");
          return false;
        }
        break;
      case DOF_FIXED:
        LinAlg_ScanScalar(File_PRE, &Dof.Val);
        if(fscanf(File_PRE, "%d", &Dof.Case.FixedAssociate.TimeFunctionIndex) !=
           1) {
          Message::Error("Could not read DOF_FIXED");
          return false;
        }
        break;
      case DOF_FIXED_SOLVE:
        if(fscanf(File_PRE, "%d", &Dof.Case.FixedAssociate.TimeFunctionIndex) !=
           1) {
          Message::Error("Could not read DOF_FIXED_SOLVED");
          return false;
        }
        break;
      case DOF_UNKNOWN_INIT:
        if(fscanf(File_PRE, "%d", &Dof.Case.Unknown.NumDof) != 1) {
          Message::Error("Could not read DOF_UNKNOWN_INIT");
          return false;
        }
        LinAlg_ScanScalar(File_PRE, &Dof.Val);
        LinAlg_ScanScalar(File_PRE, &Dof.Val2);
        if(fscanf(File_PRE, "%d", &Dof.Case.Unknown.PartitionOrNonLocal) != 1) {
          Message::Error("Could not read DOF_UNKNOWN_INIT");
          return false;
        }
        unknown(Dof);
        break;
      case DOF_LINK:
        if(fscanf(File_PRE, "%lf %d", &Dof.Case.Link.Coef,
                  &Dof.Case.Link.EntityRef) != 2) {
          Message::Error("Could not read DOF_LINK");
          return false;
        }
        Dof.Case.Link.Dof = NULL;
        break;
      case DOF_LINKCPLX:
        if(fscanf(File_PRE, "%lf %lf %d", &Dof.Case.Link.Coef,
                  &Dof.Case.Link.Coef2, &Dof.Case.Link.EntityRef) != 3) {
          Message::Error("Could not read DOF_LINKCPLX");
          return false;
        }
        Dof.Case.Link.Dof = NULL;
        break;
      }

      List_Add(DofData_P->DofList, &Dof);
    }
  }

  do {
    if(!fgets(String, sizeof(String), File_PRE) || feof(File_PRE)) {
      Message::Error("Premature end of file");
      return false;
    }
  } while(String[0] != '$');
  return true;
}

void Dof_ReadFilePRE(struct DofData *DofData_P)
{
  Message::Barrier();

  std::map<int, int> last;
  for(int i = 0; i < NbrSectionsPRE; i++)
    if(!Dof_ReadSectionPRE(DofData_P, i == 0, last)) return;

  if(NbrSectionsPRE > 1) {
    // a Dof is in the section of each rank that has it
    List_Sort(DofData_P->DofList, fcmp_Dof);
    int n = List_Nbr(DofData_P->DofList), k = 0;
    struct Dof *d = n ? (struct Dof *)List_Pointer(DofData_P->DofList, 0) : NULL;
    for(int i = 0; i < n; i++)
      if(!i || fcmp_Dof(&d[k - 1], &d[i])) d[k++] = d[i];
    List_T *all = List_Create(k + 1, 1, sizeof(struct Dof));
    for(int i = 0; i < k; i++) List_Add(all, &d[i]);
    List_Delete(DofData_P->DofList);
    DofData_P->DofList = all;
    std::vector<int> &nl = DofData_P->NonLocalEquations;
    std::sort(nl.begin(), nl.end());
    nl.erase(std::unique(nl.begin(), nl.end()), nl.end());
  }
  DofData_P->NbrAnyDof = List_Nbr(DofData_P->DofList);

  DofData_P->PartitionSplit.clear();
  DofData_P->PartitionSplit.push_back(0);
  // add last dof num for each partition
  for(auto &p : last) {
    if(p.first > 0) DofData_P->PartitionSplit.push_back(p.second);
  }
  DofData_P->PartitionSplit.push_back(DofData_P->NbrDof);

  if(DofData_P->PartitionSplit.size() > 2) {
    std::ostringstream sstream;
    for(auto p : DofData_P->PartitionSplit) sstream << p << " ";
    Message::Info("Dofs partitioning: %s", sstream.str().c_str());
  }

  Dof_InitDofType(DofData_P);
}

/* ------------------------------------------------------------------------ */
/*  D o f _ W r i t e F i l e R E S 0                                       */
/* ------------------------------------------------------------------------ */

void Dof_WriteFileRES0(char *Name_File, int Format)
{
  if(Message::GetIsCommWorld() && Message::GetCommRank()) return;

  Dof_OpenFile(DOF_RES, Name_File, (char *)(Format ? "wb" : "w"));
  fprintf(File_RES, "$ResFormat /* GetDP %s, %s */\n", GETDP_VERSION,
          Format ? "binary" : "ascii");
  fprintf(File_RES, "1.1 %d\n", Format);
  fprintf(File_RES, "$EndResFormat\n");
  Dof_CloseFile(DOF_RES);
}

/* ------------------------------------------------------------------------ */
/*  D o f _ W r i t e F i l e R E S _ E x t e n d M H                       */
/* ------------------------------------------------------------------------ */

void Dof_WriteFileRES_ExtendMH(char *Name_File, struct DofData *DofData_P,
                               int Format, int NbrH)
{
  if(Message::GetIsCommWorld() && Message::GetCommRank()) return;

  if(!DofData_P->CurrentSolution) {
    Message::Warning("No ExtendMH solution to save");
    return;
  }

  gVector x;
  double d;
  int i, inew;

  Dof_OpenFile(DOF_RES, Name_File, (char *)(Format ? "ab" : "a"));

  fprintf(File_RES, "$Solution  /* DofData #%d */\n", DofData_P->Num);
  fprintf(File_RES, "%d 0 0 0 \n", DofData_P->Num);

  LinAlg_CreateVector(&x, &DofData_P->Solver,
                      (DofData_P->NbrDof / Current.NbrHar) * NbrH,
                      true); // sequential!

  LinAlg_ZeroVector(&x);

  for(i = 0; i < DofData_P->NbrDof; i++) {
    LinAlg_GetDoubleInVector(&d, &DofData_P->CurrentSolution->x, i);
    inew = (i / Current.NbrHar) * NbrH + i % Current.NbrHar;
    LinAlg_SetDoubleInVector(d, &x, inew);
  }

  LinAlg_AssembleVector(&x);

  Format ? LinAlg_WriteVector(File_RES, &x) : LinAlg_PrintVector(File_RES, &x);

  fprintf(File_RES, "$EndSolution\n");

  Dof_CloseFile(DOF_RES);

  LinAlg_DestroyVector(&x);
}

void Dof_WriteFileRES_MHtoTime(char *Name_File, struct DofData *DofData_P,
                               int Format, List_T *Time_L)
{
  if(Message::GetIsCommWorld() && Message::GetCommRank()) return;

  if(!DofData_P->CurrentSolution) {
    Message::Warning("No solution to save");
    return;
  }

  gVector x;
  double Time, d1, d2, d, *Pulsation;
  int iT, i, j, k;

  Dof_OpenFile(DOF_RES, Name_File, (char *)(Format ? "ab" : "a"));

  for(iT = 0; iT < List_Nbr(Time_L); iT++) {
    List_Read(Time_L, iT, &Time);

    fprintf(File_RES, "$Solution  /* DofData #%d */\n", DofData_P->Num);
    fprintf(File_RES, "%d %e 0 %d \n", DofData_P->Num, Time, iT);

    Pulsation = DofData_P->Val_Pulsation;

    LinAlg_CreateVector(&x, &DofData_P->Solver,
                        DofData_P->NbrDof / Current.NbrHar,
                        true); // sequential!

    LinAlg_ZeroVector(&x);

    for(i = 0; i < DofData_P->NbrDof / Current.NbrHar; i++) {
      d = 0;
      for(k = 0; k < Current.NbrHar / 2; k++) {
        j = i * Current.NbrHar + 2 * k;
        LinAlg_GetDoubleInVector(&d1, &DofData_P->CurrentSolution->x, j);
        LinAlg_GetDoubleInVector(&d2, &DofData_P->CurrentSolution->x, j + 1);
        d += d1 * cos(Pulsation[k] * Time) - d2 * sin(Pulsation[k] * Time);
      }
      LinAlg_SetDoubleInVector(d, &x, i);
    }

    LinAlg_AssembleVector(&x);

    Format ? LinAlg_WriteVector(File_RES, &x) :
             LinAlg_PrintVector(File_RES, &x);

    fprintf(File_RES, "$EndSolution\n");
  }

  Dof_CloseFile(DOF_RES);

  LinAlg_DestroyVector(&x);
}

/* ------------------------------------------------------------------------ */
/*  D o f _ W r i t e F i l e R E S                                         */
/* ------------------------------------------------------------------------ */

void Dof_WriteFileRES(char *Name_File, struct DofData *DofData_P, int Format,
                      double Val_Time, double Val_TimeImag, int Val_TimeStep)
{
  // the first rank writes the file, with the entries that the others send it
  const bool write = !(Message::GetIsCommWorld() && Message::GetCommRank());

  if(!DofData_P->CurrentSolution) {
    if(write) Message::Warning("No solution to save");
    return;
  }

  if(write) {
    Dof_OpenFile(DOF_RES, Name_File, (char *)(Format ? "ab" : "a"));
    fprintf(File_RES, "$Solution  /* DofData #%d */\n", DofData_P->Num);
    fprintf(File_RES, "%d %.16g %.16g %d\n", DofData_P->Num, Val_Time,
            Val_TimeImag, Val_TimeStep);
  }

  FILE *file = write ? File_RES : NULL;
  Format ? LinAlg_WriteVector(file, &DofData_P->CurrentSolution->x) :
           LinAlg_PrintVector(file, &DofData_P->CurrentSolution->x);

  if(write) {
    fprintf(File_RES, "$EndSolution\n");
    Dof_CloseFile(DOF_RES);
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ W r i t e F i l e R E S _ W i t h E n t i t y N u m             */
/* ------------------------------------------------------------------------ */

void Dof_WriteFileRES_WithEntityNum(char *Name_File, struct DofData *DofData_P,
                                    struct GeoData *GeoData_P0,
                                    struct Group *Group_P, bool saveFixed)
{
  if(Message::GetIsCommWorld() && Message::GetCommRank()) return;

  if(!DofData_P->CurrentSolution) {
    Message::Warning("No solution to save");
    return;
  }

  std::map<int, std::map<int, std::complex<double> > > unknowns;

  List_T *l = !DofData_P->DofList ? Tree2List(DofData_P->DofTree) : 0;
  int N = l ? List_Nbr(l) : List_Nbr(DofData_P->DofList);
  for(int i = 0; i < N; i++) {
    struct Dof *dof;
    if(l)
      dof = (Dof *)List_Pointer(l, i);
    else
      dof = (Dof *)List_Pointer(DofData_P->DofList, i);
    if(dof->Type == DOF_UNKNOWN || dof->Type == DOF_UNKNOWN_INIT) {
      gScalar s;
      LinAlg_GetScalarInVector(&s, &DofData_P->CurrentSolution->x,
                               dof->Case.Unknown.NumDof - 1);
      unknowns[dof->NumType][dof->Entity] = s.s;
    }
    if(saveFixed && dof->Type == DOF_FIXED) {
      unknowns[dof->NumType][dof->Entity] = dof->Val.s;
    }
  }

  for(std::map<int, std::map<int, std::complex<double> > >::iterator it =
        unknowns.begin();
      it != unknowns.end(); it++) {
    // create files that can be interpreted by ListFromFile and
    // Value/VectorFromIndex
    char FileRe[256], FileIm[256];
    if(unknowns.size() > 1) {
      sprintf(FileRe, "%s_%d", Name_File, it->first);
      sprintf(FileIm, "%s_%d", Name_File, it->first);
    }
    else {
      strcpy(FileRe, Name_File);
      strcpy(FileIm, Name_File);
    }
    if(Current.NbrHar > 1) {
      strcat(FileRe, "_Re.txt");
      strcat(FileIm, "_Im.txt");
    }
    else {
      strcat(FileRe, ".txt");
      strcat(FileIm, ".txt");
    }
    FILE *fpRe = FOpen(FileRe, "w");
    if(!fpRe) {
      Message::Error("Unable to open file '%s'", FileRe);
      return;
    }
    FILE *fpIm = 0;
    if(Current.NbrHar > 1) {
      fpIm = FOpen(FileIm, "w");
      if(!fpIm) {
        Message::Error("Unable to open file '%s'", FileIm);
        return;
      }
    }

    // create vectors that can be shared as lists
    std::vector<double> exportRe, exportIm;

    if(!Group_P) {
      int n = (int)it->second.size();
      fprintf(fpRe, "%d\n", n);
      exportRe.push_back(n);
      if(fpIm) {
        fprintf(fpIm, "%d\n", n);
        exportIm.push_back(n);
      }
      for(std::map<int, std::complex<double> >::iterator it2 =
            it->second.begin();
          it2 != it->second.end(); it2++) {
        fprintf(fpRe, "%d %.16g\n", it2->first, it2->second.real());
        exportRe.push_back(it2->first);
        exportRe.push_back(it2->second.real());
        if(fpIm) {
          fprintf(fpIm, "%d %.16g\n", it2->first, it2->second.imag());
          exportIm.push_back(it2->first);
          exportIm.push_back(it2->second.imag());
        }
      }
    }
    else {
      Message::Info("Writing solution for all entities in group '%s'",
                    Group_P->Name);

      // force generation of extended list (necessary when using multiple
      // meshes)
      List_Delete(Group_P->ExtendedList);
      Generate_ExtendedGroup(Group_P);
      int n = List_Nbr(Group_P->ExtendedList);
      fprintf(fpRe, "%d\n", n);
      exportRe.push_back(n);
      if(fpIm) {
        fprintf(fpIm, "%d\n", n);
        exportIm.push_back(n);
      }
      for(int i = 0; i < List_Nbr(Group_P->ExtendedList); i++) {
        int num;
        List_Read(Group_P->ExtendedList, i, &num);
        if(!Group_P->InitialSuppList ||
           (!List_Search(Group_P->ExtendedSuppList, &num, fcmp_int))) {
          // SuppList assumed to be "Not"!
          if(it->second.count(num)) {
            std::complex<double> s = it->second[num];
            fprintf(fpRe, "%d %.16g\n", num, s.real());
            exportRe.push_back(num);
            exportRe.push_back(s.real());
            if(fpIm) {
              fprintf(fpIm, "%d %.16g\n", num, s.imag());
              exportIm.push_back(num);
              exportIm.push_back(s.imag());
            }
          }
          else {
            // yes, write zero: that's on purpose for the iterative schemes
            fprintf(fpRe, "%d 0\n", num);
            exportRe.push_back(num);
            exportRe.push_back(0);
            if(fpIm) {
              fprintf(fpIm, "%d 0\n", num);
              exportIm.push_back(num);
              exportIm.push_back(0);
            }
          }
        }
      }
    }
    fclose(fpRe);
    GetDPNumbers[FileRe] = exportRe;
    if(fpIm) {
      fclose(fpIm);
      GetDPNumbers[FileIm] = exportIm;
    }
  }

  List_Delete(l);
}

/* ------------------------------------------------------------------------ */
/*  D o f _ R e a d F i l e R E S                                           */
/* ------------------------------------------------------------------------ */

void Dof_ReadFileRES(List_T *DofData_L, struct DofData *Read_DofData_P,
                     int Read_DofData, double *Time, double *TimeImag,
                     double *TimeStep)
{
  Message::Barrier();

  int Num_DofData, Val_TimeStep, Format = 0, Read;
  double Val_Time, Val_TimeImag = 0., Version = 0.;
  struct DofData *DofData_P = NULL;
  struct Solution Solution_S;
  char String[256];

  while(1) {
    do {
      if(!fgets(String, sizeof(String), File_RES)) break;
      if(feof(File_RES)) break;
    } while(String[0] != '$');

    if(feof(File_RES)) break;

    /*  F o r m a t  */

    if(!strncmp(&String[1], "ResFormat", 9)) {
      if(fscanf(File_RES, "%lf %d\n", &Version, &Format) != 2) {
        Message::Error("Could not read ResFormat");
        return;
      }
    }

    /*  S o l u t i o n  */

    if(!strncmp(&String[1], "Solution", 8)) {
      /* don't use fscanf directly on the stream here: the data that
     follows can be binary, and the first character could be
     e.g. 0x0d, which would cause fscanf to eat-up one character
     too much, leading to an offset in fread */
      if(!fgets(String, sizeof(String), File_RES)) {
        Message::Error("Could not read Solution");
        return;
      }
      if(Version <= 1.0)
        sscanf(String, "%d %lf %d", &Num_DofData, &Val_Time, &Val_TimeStep);
      else
        sscanf(String, "%d %lf %lf %d", &Num_DofData, &Val_Time, &Val_TimeImag,
               &Val_TimeStep);

      if(Read_DofData < 0) {
        Read = 1;
        DofData_P = (struct DofData *)List_Pointer(DofData_L, Num_DofData);
      }
      else if(Num_DofData == Read_DofData) {
        Read = 1;
        DofData_P = Read_DofData_P;
      }
      else {
        Read = 0;
      }

      if(Read) {
        Current.DofData = DofData_P; // required otherwise seg fault in LinAlg_CreateVector -> getLocalSize

        Solution_S.Time = Val_Time;
        Solution_S.TimeImag = Val_TimeImag;
        Solution_S.TimeStep = Val_TimeStep;
        Solution_S.SolutionExist = 1;
        Solution_S.TimeFunctionValues = NULL;

        LinAlg_CreateVector(&Solution_S.x, &DofData_P->Solver,
                            DofData_P->NbrDof);
        Format ? LinAlg_ReadVector(File_RES, &Solution_S.x) :
                 LinAlg_ScanVector(File_RES, &Solution_S.x);

        if(DofData_P->Solutions == NULL)
          DofData_P->Solutions = List_Create(20, 20, sizeof(struct Solution));
        List_Add(DofData_P->Solutions, &Solution_S);
      }
    }

    do {
      if(!fgets(String, sizeof(String), File_RES) || feof(File_RES)) {
        Message::Warning("Premature end of file (Time Step %d)", Val_TimeStep);
        break;
      }
    } while(String[0] != '$');

  } /* while 1 ... */

  *Time = Val_Time;
  *TimeImag = Val_TimeImag;
  *TimeStep = (double)Val_TimeStep;
}

/* ------------------------------------------------------------------------ */
/*  D o f _ T r a n s f e r D o f T r e e T o L i s t                       */
/* ------------------------------------------------------------------------ */

// With rank-local Dofs, what the first rank does alone on the whole mesh needs
// the Dofs of all the ranks: the Prints that are not evaluated by all the ranks
// and the constraints that they use. The first rank only keeps them all, at the
// cost of the memory of one rank, if the problem has such a Print (see
// WhatNeedsAllDofsOnFirstRank); otherwise it gets them when it needs them, to
// transfer a solution that the ranks cannot transfer by themselves (each rank
// writes its own Dofs in the .pre file).
static bool AllDofsOnFirstRank = true;
bool Dof_GetAllDofsOnFirstRank() { return AllDofsOnFirstRank; }
void Dof_SetAllDofsOnFirstRank(bool all) { AllDofsOnFirstRank = all; }

// Returns the Dofs of all the ranks on the first rank, sorted (NULL on the
// others). Each Dof is sent by each rank that has it.
List_T *Dof_GatherDofsOnFirstRank(struct DofData *DofData_P)
{
  List_T *all = NULL;
#if defined(HAVE_MPI)
  const int me = Message::GetCommRank(), np = Message::GetCommSize();
  const long long chunk = 1 << 30;
  long long n = List_Nbr(DofData_P->DofList);
  std::vector<long long> counts(np);
  MPI_Gather(&n, 1, MPI_LONG_LONG, &counts[0], 1, MPI_LONG_LONG, 0,
             MPI_COMM_WORLD);
  if(me) {
    char *p = (char *)List_Pointer(DofData_P->DofList, 0);
    for(long long left = n * sizeof(struct Dof); left > 0; left -= chunk) {
      MPI_Send(p, (int)std::min(left, chunk), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
      p += chunk;
    }
    return NULL;
  }
  long long total = 0;
  for(int i = 0; i < np; i++) total += counts[i];
  std::vector<char> buf(total * sizeof(struct Dof));
  if(n) memcpy(&buf[0], List_Pointer(DofData_P->DofList, 0), n * sizeof(struct Dof));
  char *p = total ? &buf[0] + n * sizeof(struct Dof) : NULL;
  for(int i = 1; i < np; i++) {
    for(long long left = counts[i] * sizeof(struct Dof); left > 0; left -= chunk) {
      MPI_Recv(p, (int)std::min(left, chunk), MPI_BYTE, i, 0, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
      p += std::min(left, chunk);
    }
  }
  if(total) qsort(&buf[0], total, sizeof(struct Dof), fcmp_Dof);
  all = List_Create(std::max(total, 1LL), 1, sizeof(struct Dof));
  struct Dof *last = NULL;
  for(long long i = 0; i < total; i++) {
    struct Dof *d = (struct Dof *)&buf[i * sizeof(struct Dof)];
    if(last && !fcmp_Dof(last, d)) continue;
    List_Add(all, d);
    last = d;
  }
#endif
  return all;
}

void Dof_TransferDofTreeToList(struct DofData *DofData_P)
{
  if(DofData_P->DofTree) {
    DofData_P->DofList = Tree2List(DofData_P->DofTree);
    Tree_Delete(DofData_P->DofTree);
    DofData_P->DofTree = NULL;
    delete DofData_P->DofIndex;
    DofData_P->DofIndex = NULL;
    if(Dof_UseLocalDofs() && AllDofsOnFirstRank) {
      List_T *all = Dof_GatherDofsOnFirstRank(DofData_P);
      if(all) {
        List_Delete(DofData_P->DofList);
        DofData_P->DofList = all;
      }
    }
    DofData_P->NbrAnyDof = List_Nbr(DofData_P->DofList);
  }

  Dof_InitDofType(DofData_P);
}

/* ------------------------------------------------------------------------ */
/*  D o f _ I n i t D o f T y p e                                           */
/* ------------------------------------------------------------------------ */

void Dof_InitDofType(struct DofData *DofData_P)
{
  struct Dof *Dof_P, *Dof_P0;
  int i;

  if(!DofData_P->NbrAnyDof) { return; }

  Dof_P0 = (struct Dof *)List_Pointer(DofData_P->DofList, 0);

  for(i = 0; i < DofData_P->NbrAnyDof; i++) {
    Dof_P = Dof_P0 + i;

    switch(Dof_P->Type) {
    case DOF_LINK:
    case DOF_LINKCPLX:
      Dof_P->Case.Link.Dof = Dof_GetDofStruct(
        DofData_P, Dof_P->NumType, Dof_P->Case.Link.EntityRef, Dof_P->Harmonic);
      if(Dof_P->Case.Link.Dof == NULL ||
         Dof_P->Case.Link.Dof == Dof_GetDofStruct(DofData_P, Dof_P->NumType,
                                                  Dof_P->Entity,
                                                  Dof_P->Harmonic)) {
        Dof_P->Case.Link.Dof = /* Attention: bricolage ... */
          Dof_GetDofStruct(DofData_P, Dof_P->NumType - 1,
                           Dof_P->Case.Link.EntityRef, Dof_P->Harmonic);
        if(Dof_P->Case.Link.Dof == NULL)
          Message::Error(
            "Wrong Link Constraint: reference Dof (%d %d %d) does not exist",
            Dof_P->NumType, Dof_P->Case.Link.EntityRef, Dof_P->Harmonic);
      }
      /*
      if(Dof_P->Case.Link.Dof == NULL)
    Message::Error("Wrong Link Constraint: reference Dof (%d %d %d) does not
    exist", Dof_P->NumType, Dof_P->Case.Link.EntityRef, Dof_P->Harmonic);
      */
      break;
    default: break;
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  P R E P R O C E S S I N G   ( C o d e s   i n   T r e e )               */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/*  D o f _ A d d F u n c t i o n S p a c e I n d e x                       */
/* ------------------------------------------------------------------------ */

void Dof_AddFunctionSpaceIndex(int Index_FunctionSpace)
{
  if(CurrentDofData->FunctionSpaceIndex == NULL)
    CurrentDofData->FunctionSpaceIndex = List_Create(10, 5, sizeof(int));

  if(List_PQuery(CurrentDofData->FunctionSpaceIndex, &Index_FunctionSpace,
                 fcmp_int) == NULL) {
    List_Add(CurrentDofData->FunctionSpaceIndex, &Index_FunctionSpace);
    List_Sort(CurrentDofData->FunctionSpaceIndex, fcmp_int);
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ A d d T i m e F u n c t i o n I n d e x                         */
/* ------------------------------------------------------------------------ */

void Dof_AddTimeFunctionIndex(int Index_TimeFunction)
{
  if(List_PQuery(CurrentDofData->TimeFunctionIndex, &Index_TimeFunction,
                 fcmp_int) == NULL) {
    List_Add(CurrentDofData->TimeFunctionIndex, &Index_TimeFunction);
    List_Sort(CurrentDofData->TimeFunctionIndex, fcmp_int);
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ A d d P u l s a t i o n                                         */
/* ------------------------------------------------------------------------ */

void Dof_AddPulsation(struct DofData *DofData_P, double Val_Pulsation)
{
  if(DofData_P->Pulsation == NULL)
    DofData_P->Pulsation = List_Create(1, 2, sizeof(double));
  List_Add(DofData_P->Pulsation, &Val_Pulsation);
  /*
  if(List_PQuery
      (DofData_P->Pulsation, &Val_Pulsation, fcmp_double) == NULL) {
    List_Add(DofData_P->Pulsation, &Val_Pulsation) ;
    List_Sort(DofData_P->Pulsation, fcmp_double) ;
  }
  */
}

/* ------------------------------------------------------------------------ */
/*  D o f _ D e f i n e A s s i g n F i x e d D o f                         */
/* ------------------------------------------------------------------------ */

void Dof_DefineAssignFixedDof(int D1, int D2, int NbrHar, double *Val,
                              int Index_TimeFunction)
{
  struct Dof Dof, *Dof_P;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(!(Dof_P = (struct Dof *)Dof_Find(CurrentDofData, &Dof, true))) {
      Dof.Type = DOF_FIXED;
      LinAlg_SetScalar(&Dof.Val, &Val[k]);
      Dof.Case.FixedAssociate.TimeFunctionIndex = Index_TimeFunction + 1;
      Dof_AddTimeFunctionIndex(Index_TimeFunction + 1);
      Dof_Add(CurrentDofData, &Dof);
    }
    else if(Dof_P->Type == DOF_UNKNOWN) {
      if(Message::GetVerbosity() == 10)
        Message::Info("Overriding unknown Dof with fixed Dof");
      Dof_P->Type = DOF_FIXED;
      LinAlg_SetScalar(&Dof_P->Val, &Val[k]);
      Dof_P->Case.FixedAssociate.TimeFunctionIndex = Index_TimeFunction + 1;
      Dof_AddTimeFunctionIndex(Index_TimeFunction + 1);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ D e f i n e A s s i g n S o l v e D o f                         */
/* ------------------------------------------------------------------------ */

void Dof_DefineAssignSolveDof(int D1, int D2, int NbrHar,
                              int Index_TimeFunction)
{
  struct Dof Dof;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(!Dof_Find(CurrentDofData, &Dof, true)) {
      Dof.Type = DOF_FIXED_SOLVE;
      Dof.Case.FixedAssociate.TimeFunctionIndex = Index_TimeFunction + 1;
      Dof_AddTimeFunctionIndex(Index_TimeFunction + 1);
      Dof_Add(CurrentDofData, &Dof);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ D e f i n e I n i t F i x e d D o f                             */
/* ------------------------------------------------------------------------ */

void Dof_DefineInitFixedDof(int D1, int D2, int NbrHar, double *Val,
                            double *Val2, int PartitionOrNonLocal)
{
  struct Dof Dof;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(!Dof_Find(CurrentDofData, &Dof, true)) {
      Dof.Type = DOF_UNKNOWN_INIT;
      LinAlg_SetScalar(&Dof.Val, &Val[k]);
      LinAlg_SetScalar(&Dof.Val2, &Val2[k]);
      // old version: number as we go
      // Dof.Case.Unknown.NumDof = ++(CurrentDofData->NbrDof) ;
      Dof.Case.Unknown.NumDof = -1;
      Dof.Case.Unknown.PartitionOrNonLocal = PartitionOrNonLocal;
      Dof_Add(CurrentDofData, &Dof);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ D e f i n e I n i t S o l v e D o f                             */
/* ------------------------------------------------------------------------ */

void Dof_DefineInitSolveDof(int D1, int D2, int NbrHar,
                            int PartitionOrNonLocal)
{
  struct Dof Dof;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(!Dof_Find(CurrentDofData, &Dof, true)) {
      Dof.Type = DOF_UNKNOWN_INIT;
      // the values are set by the resolution that computes them; zero them
      // until then, so that the .pre file does not contain uninitialized data
      LinAlg_ZeroScalar(&Dof.Val);
      LinAlg_ZeroScalar(&Dof.Val2);
      // old version: number as we go
      // Dof.Case.Unknown.NumDof = ++(CurrentDofData->NbrDof) ;
      Dof.Case.Unknown.NumDof = -1;
      Dof.Case.Unknown.PartitionOrNonLocal = PartitionOrNonLocal;
      Dof_Add(CurrentDofData, &Dof);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ D e f i n e L i n k D o f                                       */
/* ------------------------------------------------------------------------ */

void Dof_DefineLinkDof(int D1, int D2, int NbrHar, double Value[], int D2_Link)
{
  struct Dof Dof;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(!Dof_Find(CurrentDofData, &Dof, true)) {
      Dof.Type = DOF_LINK;
      Dof.Case.Link.Coef = Value[0];
      Dof.Case.Link.EntityRef = D2_Link;
      Dof.Case.Link.Dof = NULL;
      Dof_Add(CurrentDofData, &Dof);
    }
  }
}

void Dof_DefineLinkCplxDof(int D1, int D2, int NbrHar, double Value[],
                           int D2_Link)
{
  struct Dof Dof;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(!Dof_Find(CurrentDofData, &Dof, true)) {
      Dof.Type = DOF_LINKCPLX;
      Dof.Case.Link.Coef = Value[0];
      Dof.Case.Link.Coef2 = Value[1];
      Dof.Case.Link.EntityRef = D2_Link;
      Dof.Case.Link.Dof = NULL;
      Dof_Add(CurrentDofData, &Dof);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ D e f i n e U n k n o w n D o f                                 */
/* ------------------------------------------------------------------------ */

void Dof_DefineUnknownDof(int D1, int D2, int NbrHar, int PartitionOrNonLocal)
{
  struct Dof Dof;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(!Dof_Find(CurrentDofData, &Dof, true)) {
      Dof.Type = DOF_UNKNOWN;
      // old version: number as we go
      // Dof.Case.Unknown.NumDof = ++(CurrentDofData->NbrDof) ;
      Dof.Case.Unknown.NumDof = -1;
      Dof.Case.Unknown.PartitionOrNonLocal = PartitionOrNonLocal;
      Dof_Add(CurrentDofData, &Dof);
    }
  }
}

// the unknown Dofs that have no number yet, by mesh partition (0 for the
// elements without partition, -1 for the non-local, e.g. global, Dofs)
static std::map<int, std::vector<struct Dof *>> UnnumberedUnknownDofs;

static void GetUnnumberedUnknownDofs(void *a, void *b)
{
  struct Dof *Dof_P = (struct Dof *)a;
  if((Dof_P->Type == DOF_UNKNOWN || Dof_P->Type == DOF_UNKNOWN_INIT) &&
     Dof_P->Case.Unknown.NumDof == -1) {
    UnnumberedUnknownDofs[Dof_P->Case.Unknown.PartitionOrNonLocal].push_back(Dof_P);
  }
}

bool Dof_UseLocalDofs()
{
  return Flag_LOCAL_DOFS && Message::GetCommSize() > 1 &&
         Message::GetIsCommWorld();
}

// In parallel each element is assembled by a single rank, the one of its mesh
// partition, which adds all its contributions, whoever owns the rows (they are
// sent to their owner when the matrix or the vector is assembled). Elements
// without partition go to the last rank, like the global equations.
bool Dof_IsPartitionOfThisRank(int partition)
{
  int np = Message::GetCommSize();
  return ((partition > 0) ? (partition - 1) % np : np - 1) ==
         Message::GetCommRank();
}

// Copies the values of the unknowns of a system (in x1) to the same unknowns
// of another one (in x2). With rank-local Dofs each rank transfers the Dofs
// that it has in both systems; if a rank only has one of them in the first
// system, the first rank gets the Dofs of all the ranks and transfers them all.
void Dof_TransferSolution(struct DofData *DofData1_P, gVector *x1,
                          struct DofData *DofData2_P, gVector *x2)
{
  const bool localDofs = Dof_UseLocalDofs();
  List_T *from = DofData1_P->DofList, *to = DofData2_P->DofList;
  std::vector<gScalar> all; // the entries of x1, on the first rank
  for(int pass = 0; pass < 2; pass++) {
    int missing = 0;
    for(int i = 0; i < List_Nbr(from); i++) {
      struct Dof *Dof1_P = (struct Dof *)List_Pointer(from, i);
      if(Dof1_P->Type != DOF_UNKNOWN) continue;
      struct Dof *Dof2_P = (struct Dof *)List_PQuery(to, Dof1_P, fcmp_Dof);
      if(!Dof2_P) {
        missing++;
        continue;
      }
      gScalar tmp;
      if(pass)
        tmp = all[Dof1_P->Case.Unknown.NumDof - 1];
      else
        LinAlg_GetScalarInVector(&tmp, x1, Dof1_P->Case.Unknown.NumDof - 1);
      if(localDofs)
        LinAlg_SetScalarInVectorOffProcess(&tmp, x2,
                                           Dof2_P->Case.Unknown.NumDof - 1);
      else
        LinAlg_SetScalarInVector(&tmp, x2, Dof2_P->Case.Unknown.NumDof - 1);
      Dof2_P->Type = DOF_UNKNOWN;
    }
    if(!localDofs || pass) {
      if(missing) Message::Warning("Unknown Dof in TransferSolution");
      break;
    }
#if defined(HAVE_MPI)
    MPI_Allreduce(MPI_IN_PLACE, &missing, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
#endif
    if(!missing) break;
    from = Dof_GatherDofsOnFirstRank(DofData1_P);
    to = Dof_GatherDofsOnFirstRank(DofData2_P);
    LinAlg_GetVectorOnFirstRank(x1, all);
  }
  if(from != DofData1_P->DofList) {
    List_Delete(from);
    List_Delete(to);
  }
  LinAlg_AssembleVector(x2);
}

static bool PostOnRootOnly = false;
bool Dof_GetPostOnRootOnly() { return PostOnRootOnly; }
void Dof_SetPostOnRootOnly(bool rootOnly) { PostOnRootOnly = rootOnly; }

// A pre-resolution is requested by the first element that needs it, which a rank
// may not have: make all the ranks run the same ones, in the same order
void Dof_SyncPreResolutions(List_T *PreResolutionInfo_L)
{
#if defined(HAVE_PETSC)
  int n = List_Nbr(PreResolutionInfo_L), np = Message::GetCommSize();
  std::vector<int> mine(2 * n), counts(np), displs(np);
  for(int i = 0; i < n; i++) {
    struct PreResolutionInfo *p =
      (struct PreResolutionInfo *)List_Pointer(PreResolutionInfo_L, i);
    mine[2 * i] = p->Index;
    mine[2 * i + 1] = p->Type;
  }
  int count = 2 * n, total = 0;
  MPI_Allgather(&count, 1, MPI_INT, counts.data(), 1, MPI_INT, MPI_COMM_WORLD);
  for(int r = 0; r < np; r++) {
    displs[r] = total;
    total += counts[r];
  }
  std::vector<int> all(total);
  MPI_Allgatherv(mine.data(), count, MPI_INT, all.data(), counts.data(),
                 displs.data(), MPI_INT, MPI_COMM_WORLD);
  List_Reset(PreResolutionInfo_L);
  for(int i = 0; i < total; i += 2) {
    if(all[i + 1] == PR_GLOBALBASISFUNCTION)
      Message::Error("Rank-local Dofs (use -all-dofs) do not handle pre-resolutions of global "
                     "basis functions yet");
    if(List_ISearchSeq(PreResolutionInfo_L, &all[i], fcmp_int) >= 0) continue;
    struct PreResolutionInfo info;
    info.Index = all[i];
    info.Type = all[i + 1];
    List_Add(PreResolutionInfo_L, &info);
  }
#endif
}

#if defined(HAVE_MPI)
static std::vector<struct Dof *> LocalUnknownDofs;
static int NbrUnsupportedLocalDofs = 0;

static void GetLocalUnknownDofs(void *a, void *b)
{
  struct Dof *Dof_P = (struct Dof *)a;
  switch(Dof_P->Type) {
  case DOF_UNKNOWN:
  case DOF_UNKNOWN_INIT:
    if(Dof_P->Case.Unknown.NumDof == -1) LocalUnknownDofs.push_back(Dof_P);
    break;
  case DOF_FIXEDWITHASSOCIATE_SOLVE:
  case DOF_FIXED_SOLVE: NbrUnsupportedLocalDofs++; break;
  default: break;
  }
}

// Numbers the unknown Dofs with rank-local Dofs, when each rank has only
// defined the Dofs of its mesh partition. The Dofs are sent to a "home" rank chosen from
// their entity, which keeps the entities of neighboring numbers together.
//
// The home rank knows all the ranks that have a Dof: this gives it one owner.
// As for the numbering by partition with all the Dofs on all the ranks, the
// equations of each partition are consecutive, and the global Dofs come last. The numbers only
// depend on the blocks counted on each home rank, so that they can be computed
// there and sent back without a second exchange.
//
// Global Dofs: a rank whose partition has no element in the region of a global
// Dof has not defined it, but it needs it, e.g. for the global equations; all
// the ranks thus get them all, with the same numbers. The non-local equations
// belong to the last rank.
static void Dof_NumberUnknownDofLocal(void)
{
  const int np = Message::GetCommSize();
  const int blocks = np + 1;

  LocalUnknownDofs.clear();
  NbrUnsupportedLocalDofs = 0;
  Tree_Action(CurrentDofData->DofTree, GetLocalUnknownDofs);
  int bad = NbrUnsupportedLocalDofs;
  MPI_Allreduce(MPI_IN_PLACE, &bad, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  if(bad) {
    Message::Error("Rank-local Dofs (use -all-dofs) do not handle "
                   "AssignFromResolution yet (%d such Dofs)", bad);
    return;
  }

  // The associated Dofs of global quantities have already been numbered, first
  // (see Dof_DefineAssociateDof), and they belong to the first rank as with
  // all the Dofs on all the ranks. All the ranks treat the global terms, in the same order, so
  // that they must all have given the same numbers.
  int base[2] = {CurrentDofData->NbrDof, -CurrentDofData->NbrDof};
  MPI_Allreduce(MPI_IN_PLACE, base, 2, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
  if(base[0] != -base[1]) {
    Message::Error("Rank-local Dofs (use -all-dofs): the ranks have numbered different associated "
                   "Dofs (between %d and %d)", base[0], -base[1]);
    return;
  }

  int maxEntity = 0;
  for(auto d : LocalUnknownDofs)
    if(d->Case.Unknown.PartitionOrNonLocal > 0)
      maxEntity = std::max(maxEntity, d->Entity);
  MPI_Allreduce(MPI_IN_PLACE, &maxEntity, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);

  // send the keys of the Dofs to their home rank
  auto home = [&](int entity) {
    if(entity < 0) entity = -entity;
    return (int)std::min((long long)np - 1,
                         (long long)entity * np / ((long long)maxEntity + 1));
  };
  std::vector<int> scounts(np, 0), rcounts(np), sdispls(np, 0), rdispls(np, 0);
  for(auto d : LocalUnknownDofs) scounts[home(d->Entity)]++;
  MPI_Alltoall(&scounts[0], 1, MPI_INT, &rcounts[0], 1, MPI_INT, MPI_COMM_WORLD);
  int nrecv = rcounts[0];
  for(int i = 1; i < np; i++) {
    sdispls[i] = sdispls[i - 1] + scounts[i - 1];
    rdispls[i] = rdispls[i - 1] + rcounts[i - 1];
    nrecv += rcounts[i];
  }
  struct Key {
    int type, entity, harmonic, partition;
  };
  std::vector<Key> skeys(LocalUnknownDofs.size() + 1), rkeys(nrecv + 1);
  std::vector<int> pos(LocalUnknownDofs.size()), fill(sdispls);
  for(std::size_t i = 0; i < LocalUnknownDofs.size(); i++) {
    struct Dof *d = LocalUnknownDofs[i];
    pos[i] = fill[home(d->Entity)]++;
    Key k = {d->NumType, d->Entity, d->Harmonic,
             d->Case.Unknown.PartitionOrNonLocal};
    skeys[pos[i]] = k;
  }
  MPI_Datatype keyType, answerType;
  MPI_Type_contiguous(4, MPI_INT, &keyType);
  MPI_Type_commit(&keyType);
  MPI_Type_contiguous(2, MPI_INT, &answerType);
  MPI_Type_commit(&answerType);
  MPI_Alltoallv(&skeys[0], &scounts[0], &sdispls[0], keyType, &rkeys[0],
                &rcounts[0], &rdispls[0], keyType, MPI_COMM_WORLD);

  // group the keys of each Dof and find its partition: non-local if it is for
  // one rank, else the lowest partition that has it, 0 without any partition
  std::vector<int> order(nrecv);
  for(int i = 0; i < nrecv; i++) order[i] = i;
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    const Key &x = rkeys[a], &y = rkeys[b];
    if(x.type != y.type) return x.type < y.type;
    if(x.entity != y.entity) return x.entity < y.entity;
    if(x.harmonic != y.harmonic) return x.harmonic < y.harmonic;
    return a < b;
  });
  auto same = [&](int a, int b) {
    const Key &x = rkeys[a], &y = rkeys[b];
    return x.type == y.type && x.entity == y.entity && x.harmonic == y.harmonic;
  };
  std::vector<int> partitionOf; // of each group
  std::vector<int> count(blocks, 0), allCounts(blocks * np);
  auto blockOf = [&](int p) { return p > 0 ? std::min(p, np) - 1 : np; };
  for(int i = 0; i < nrecv;) {
    int j = i, part = 0;
    bool nonLocal = false;
    for(; j < nrecv && same(order[i], order[j]); j++) {
      int p = rkeys[order[j]].partition;
      if(p < 0) nonLocal = true;
      else if(p > 0 && (!part || p < part)) part = p;
    }
    if(nonLocal) part = -1;
    partitionOf.push_back(part);
    count[blockOf(part)]++;
    i = j;
  }
  MPI_Allgather(&count[0], blocks, MPI_INT, &allCounts[0], blocks, MPI_INT,
                MPI_COMM_WORLD);

  // the first number of each block on this home rank, and the partition split
  const int me = Message::GetCommRank();
  std::vector<int> next(blocks);
  std::vector<int> split(1, 0);
  int total = CurrentDofData->NbrDof;
  for(int b = 0; b < blocks; b++) {
    next[b] = total;
    for(int h = 0; h < np; h++) {
      if(h < me) next[b] += allCounts[h * blocks + b];
      total += allCounts[h * blocks + b];
    }
    if(b < np) split.push_back(total);
  }

  // number the Dofs and answer in the order of the received keys
  std::vector<int> ranswers(2 * nrecv + 2), sanswers(2 * skeys.size());
  int group = 0;
  for(int i = 0; i < nrecv; group++) {
    int part = partitionOf[group];
    int num = ++next[blockOf(part)];
    int j = i;
    for(; j < nrecv && same(order[i], order[j]); j++) {
      ranswers[2 * order[j]] = num;
      ranswers[2 * order[j] + 1] = part;
    }
    i = j;
  }
  MPI_Alltoallv(&ranswers[0], &rcounts[0], &rdispls[0], answerType, &sanswers[0],
                &scounts[0], &sdispls[0], answerType, MPI_COMM_WORLD);
  MPI_Type_free(&keyType);
  MPI_Type_free(&answerType);

  std::vector<struct Dof> globals;
  for(std::size_t i = 0; i < LocalUnknownDofs.size(); i++) {
    struct Dof *d = LocalUnknownDofs[i];
    d->Case.Unknown.NumDof = sanswers[2 * pos[i]];
    d->Case.Unknown.PartitionOrNonLocal = sanswers[2 * pos[i] + 1];
    if(d->Case.Unknown.PartitionOrNonLocal <= 0) globals.push_back(*d);
  }
  LocalUnknownDofs.clear();

  // all the ranks get the global Dofs
  int nb = (int)(globals.size() * sizeof(struct Dof));
  std::vector<int> nbs(np), displs(np, 0);
  MPI_Allgather(&nb, 1, MPI_INT, &nbs[0], 1, MPI_INT, MPI_COMM_WORLD);
  int nbTotal = nbs[0];
  for(int i = 1; i < np; i++) {
    displs[i] = displs[i - 1] + nbs[i - 1];
    nbTotal += nbs[i];
  }
  std::vector<char> allGlobals(nbTotal + 1);
  MPI_Allgatherv(globals.empty() ? NULL : (void *)&globals[0], nb, MPI_BYTE,
                 &allGlobals[0], &nbs[0], &displs[0], MPI_BYTE, MPI_COMM_WORLD);
  std::set<int> nonLocalEquations;
  for(int i = 0; i < nbTotal / (int)sizeof(struct Dof); i++) {
    struct Dof d;
    memcpy((void *)&d, &allGlobals[i * sizeof(struct Dof)], sizeof(struct Dof));
    if(!Dof_Find(CurrentDofData, &d, true)) Dof_Add(CurrentDofData, &d);
    if(d.Case.Unknown.PartitionOrNonLocal < 0)
      nonLocalEquations.insert(d.Case.Unknown.NumDof);
  }
  for(int n : nonLocalEquations) CurrentDofData->NonLocalEquations.push_back(n);

  CurrentDofData->NbrDof = total;
  CurrentDofData->PartitionSplit = split;
  CurrentDofData->PartitionSplit.push_back(total);

  std::ostringstream sstream;
  for(auto p : CurrentDofData->PartitionSplit) sstream << p << " ";
  Message::Info("Dofs partitioning: %s", sstream.str().c_str());
}
#endif

void Dof_NumberUnknownDof(void)
{
#if defined(HAVE_MPI)
  if(Dof_UseLocalDofs()) {
    Dof_NumberUnknownDofLocal();
    return;
  }
#endif
  UnnumberedUnknownDofs.clear();
  if(CurrentDofData->DofTree)
    Tree_Action(CurrentDofData->DofTree, GetUnnumberedUnknownDofs);
  else
    List_Action(CurrentDofData->DofList, GetUnnumberedUnknownDofs);

  CurrentDofData->PartitionSplit.clear();
  CurrentDofData->PartitionSplit.push_back(0);
  // number dofs by partition and keep track of last dof in each partition
  for(auto &p : UnnumberedUnknownDofs) {
    if(p.first > 0) {
      Message::Debug("Numbering %lu dofs in partition %d", p.second.size(),
                     p.first);
      for(auto &d : p.second) {
        d->Case.Unknown.NumDof = ++(CurrentDofData->NbrDof);
      }
      CurrentDofData->PartitionSplit.push_back(CurrentDofData->NbrDof);
    }
  }

  // number remaining (global and non-partitioned) dofs
  for(auto &p : UnnumberedUnknownDofs) {
    if(p.first <= 0) {
      Message::Debug("Numbering %lu global or non-partitioned dofs",
                     p.second.size());
      for(auto &d : p.second) {
        d->Case.Unknown.NumDof = ++(CurrentDofData->NbrDof);
        if(p.first < 0)
          CurrentDofData->NonLocalEquations.push_back(CurrentDofData->NbrDof);
      }
    }
  }

  CurrentDofData->PartitionSplit.push_back(CurrentDofData->NbrDof);
  UnnumberedUnknownDofs.clear();

  if(CurrentDofData->PartitionSplit.size() > 2) {
    std::ostringstream sstream;
    for(auto p : CurrentDofData->PartitionSplit) sstream << p << " ";
    Message::Info("Dofs partitioning: %s", sstream.str().c_str());
  }
}


/* ------------------------------------------------------------------------ */
/*  D o f _ D e f i n e A s s o c i a t e D o f                             */
/* ------------------------------------------------------------------------ */

void Dof_DefineAssociateDof(int E1, int E2, int D1, int D2, int NbrHar,
                            int init, double *Val)
{
  struct Dof Dof, Equ, *Equ_P;
  int k;

  Equ.NumType = E1;
  Equ.Entity = E2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Equ.Harmonic = k;
    if((Equ_P = (struct Dof *)Dof_Find(CurrentDofData, &Equ, true))) {
      switch(Equ_P->Type) {
      case DOF_FIXED:
        Equ_P->Type = DOF_FIXEDWITHASSOCIATE;
        Equ_P->Case.FixedAssociate.NumDof = ++(CurrentDofData->NbrDof);
        /* To be modified (Patrick): strange to define a new NumDof for Equ if
           associate-Dof already exists */
        Dof.NumType = D1;
        Dof.Entity = D2;
        Dof.Harmonic = k;
        if(!Dof_Find(CurrentDofData, &Dof, true)) {
          if(!init) { Dof.Type = DOF_UNKNOWN; }
          else {
            Dof.Type = DOF_UNKNOWN_INIT;
            LinAlg_SetScalar(&Dof.Val, &Val[k]);
            LinAlg_ZeroScalar(&Dof.Val2);
          }
          Dof.Case.Unknown.NumDof = CurrentDofData->NbrDof;
          Dof.Case.Unknown.PartitionOrNonLocal = -1;
          CurrentDofData->NonLocalEquations.push_back(Dof.Case.Unknown.NumDof);
          Dof_Add(CurrentDofData, &Dof);
        }
        else{
          Message::Warning("Fixed with associate Equ for non-existing Dof!");
        }
        break;
      case DOF_FIXED_SOLVE:
        Equ_P->Type = DOF_FIXEDWITHASSOCIATE_SOLVE;
        Equ_P->Case.FixedAssociate.NumDof = ++(CurrentDofData->NbrDof);
        Dof.NumType = D1;
        Dof.Entity = D2;
        Dof.Harmonic = k;
        if(!Dof_Find(CurrentDofData, &Dof, true)) {
          if(!init) { Dof.Type = DOF_UNKNOWN; }
          else {
            Dof.Type = DOF_UNKNOWN_INIT;
            LinAlg_SetScalar(&Dof.Val, &Val[k]);
            LinAlg_ZeroScalar(&Dof.Val2);
          }
          Dof.Case.Unknown.NumDof = CurrentDofData->NbrDof;
          Dof.Case.Unknown.PartitionOrNonLocal = -1;
          CurrentDofData->NonLocalEquations.push_back(Dof.Case.Unknown.NumDof);
          Dof_Add(CurrentDofData, &Dof);
        }
        else{
          Message::Warning("Fixed with associate Equ for non-existing Dof!");
        }
        break;
      case DOF_UNKNOWN:
      case DOF_UNKNOWN_INIT: Dof_DefineUnknownDof(D1, D2, NbrHar); break;
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  P R O C E S S I N G   ( C o d e s   i n   L i s t )                     */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/*  D o f _ G e t D o f S t r u c t                                         */
/* ------------------------------------------------------------------------ */

struct Dof *Dof_GetDofStruct(struct DofData *DofData_P, int D1, int D2, int D3)
{
  struct Dof Dof;

  Dof.NumType = D1;
  Dof.Entity = D2;
  Dof.Harmonic = D3;

  return Dof_Find(DofData_P, &Dof, false);
}

/* ------------------------------------------------------------------------ */
/*  D o f _ U p d a t e A s s i g n F i x e d D o f                         */
/* ------------------------------------------------------------------------ */

void Dof_UpdateAssignFixedDof(int D1, int D2, int NbrHar, double *Val,
                              double *Val2)
{
  struct Dof Dof, *Dof_P;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(CurrentDofData->DofTree)
      Dof_P = (struct Dof *)Dof_Find(CurrentDofData, &Dof, true);
    else
      Dof_P =
        (struct Dof *)Dof_Find(CurrentDofData, &Dof, false);
    LinAlg_SetScalar(&Dof_P->Val, &Val[Dof_P->Harmonic]);
    LinAlg_SetScalar(&Dof_P->Val2, &Val2[Dof_P->Harmonic]);
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ U p d a t e L i n k D o f                                       */
/* ------------------------------------------------------------------------ */

void Dof_UpdateLinkDof(int D1, int D2, int NbrHar, double Value[], int D2_Link)
{
  struct Dof Dof, *Dof_P;
  int k;

  Dof.NumType = D1;
  Dof.Entity = D2;

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    Dof.Harmonic = k;
    if(CurrentDofData->DofTree)
      Dof_P = (struct Dof *)Dof_Find(CurrentDofData, &Dof, true);
    else
      Dof_P =
        (struct Dof *)Dof_Find(CurrentDofData, &Dof, false);

    if(Dof_P->Type == DOF_LINK || Dof_P->Type == DOF_LINKCPLX) {
      /*
        fprintf(stderr,"===> %d %d %.16g\n", Dof_P->NumType, Dof_P->Entity,
        Value[0]) ;
      */
      Dof_P->Case.Link.Coef = Value[0];
      if(Dof_P->Type == DOF_LINKCPLX) Dof_P->Case.Link.Coef2 = Value[1];
      Dof_P->Case.Link.EntityRef = D2_Link;
      Dof_P->Case.Link.Dof = NULL;
    }
  }
}

// Whether the Dofs are numbered by mesh partition, with one partition for each
// MPI rank (plus possibly the non-partitioned Dofs, which go to the last rank)
bool Dof_HasRankPartitions()
{
  int np = Message::GetCommSize();
  if(np == 1 || !Message::GetIsCommWorld() || !Current.DofData) return false;
  int ns = (int)Current.DofData->PartitionSplit.size();
  return ns == np + 1 || ns == np + 2;
}

// Whether equation n (starting at 0) is non-local, i.e. a global quantity
// coupled to many entities, whose equation belongs to the last rank
bool Dof_IsNonLocalEquation(int n)
{
  static const DofData *dd = NULL;
  static std::size_t nb = 0;
  static std::unordered_set<int> equ;
  if(!Current.DofData) return false;
  const std::vector<int> &nl = Current.DofData->NonLocalEquations;
  if(nl.empty()) return false;
  if(dd != Current.DofData || nb != nl.size()) {
    dd = Current.DofData;
    nb = nl.size();
    equ.clear();
    for(int e : nl) equ.insert(e - 1); // equation numbers start at 1
  }
  return equ.count(n) > 0;
}

/* ------------------------------------------------------------------------ */
/*  D o f _ A s s e m b l e I n M a t                                       */
/* ------------------------------------------------------------------------ */

void Dof_AssembleInMat(struct Dof *Equ_P, struct Dof *Dof_P, int NbrHar,
                       double *Val, gMatrix *Mat, gVector *Vec, List_T *Vecs)
{
  gScalar tmp, tmp2;
  double valtmp[2], d1, d2;

  switch(Equ_P->Type) {
  case DOF_UNKNOWN:
  case DOF_FIXEDWITHASSOCIATE:

    switch(Dof_P->Type) {
    case DOF_UNKNOWN:
      if(Current.DofData->Flag_RHS) break;
      if(NbrHar == 1) {
        LinAlg_AddDoubleInMatrix(Val[0], Mat, Equ_P->Case.Unknown.NumDof - 1,
                                 Dof_P->Case.Unknown.NumDof - 1);
      }
      else
        LinAlg_AddComplexInMatrix(
          Val[0], Val[1], Mat, Equ_P->Case.Unknown.NumDof - 1,
          Dof_P->Case.Unknown.NumDof - 1,
          (gSCALAR_SIZE == 1) ? ((Equ_P + 1)->Case.Unknown.NumDof - 1) : -1,
          (gSCALAR_SIZE == 1) ? ((Dof_P + 1)->Case.Unknown.NumDof - 1) : -1);
      break;

    case DOF_FIXED:
    case DOF_FIXEDWITHASSOCIATE:
      if(Vec) {
        if(NbrHar == 1) {
          if(Val[0]) {
            LinAlg_ProdScalarDouble(
              &Dof_P->Val,
              CurrentDofData->CurrentSolution->TimeFunctionValues
                [Dof_P->Case.FixedAssociate.TimeFunctionIndex],
              &tmp);
            LinAlg_ProdScalarDouble(&tmp, -Val[0], &tmp);
            LinAlg_AddScalarInVector(&tmp, Vec, Equ_P->Case.Unknown.NumDof - 1);
            if(Vecs) { // experimental
              int index = List_ISearchSeq(
                Current.DofData->TimeFunctionIndex,
                &Dof_P->Case.FixedAssociate.TimeFunctionIndex, fcmp_int);
              if(index >= 0 && index < List_Nbr(Vecs)) {
                gVector *v = (gVector *)List_Pointer(Vecs, index);
                LinAlg_AddScalarInVector(&tmp, v,
                                         Equ_P->Case.Unknown.NumDof - 1);
              }
              else {
                Message::Error("Something wrong in multi-vec assembly");
              }
            }
          }
        }
        else {
          LinAlg_ProdScalarDouble(
            &Dof_P->Val,
            CurrentDofData->CurrentSolution->TimeFunctionValues
              [Dof_P->Case.FixedAssociate.TimeFunctionIndex],
            &tmp);
          if(gSCALAR_SIZE == 2) {
            LinAlg_ProdScalarComplex(&tmp, -Val[0], -Val[1], &valtmp[0],
                                     &valtmp[1]);
          }
          else {
            LinAlg_GetDoubleInScalar(&d1, &tmp);
            LinAlg_ProdScalarDouble(
              &(Dof_P + 1)->Val,
              CurrentDofData->CurrentSolution->TimeFunctionValues
                [Dof_P->Case.FixedAssociate.TimeFunctionIndex],
              &tmp2);
            LinAlg_GetDoubleInScalar(&d2, &tmp2);
            valtmp[0] = -d1 * Val[0] + d2 * Val[1];
            valtmp[1] = -d1 * Val[1] - d2 * Val[0];
          }
          LinAlg_AddComplexInVector(
            valtmp[0], valtmp[1], Vec, Equ_P->Case.Unknown.NumDof - 1,
            (gSCALAR_SIZE == 1) ? ((Equ_P + 1)->Case.Unknown.NumDof - 1) : -1);
        }
      }
      break;

    case DOF_LINK:
      if(NbrHar == 1)
        valtmp[0] = Val[0] * Dof_P->Case.Link.Coef;
      else {
        valtmp[0] = Val[0] * Dof_P->Case.Link.Coef;
        valtmp[1] = Val[1] * Dof_P->Case.Link.Coef;
      }
      Dof_AssembleInMat(Equ_P, Dof_P->Case.Link.Dof, NbrHar, valtmp, Mat, Vec,
                        Vecs);
      break;

    case DOF_LINKCPLX:
      if(NbrHar == 1)
        Message::Error("LinkCplx only valid for Complex systems");
      else {
        valtmp[0] =
          Val[0] * Dof_P->Case.Link.Coef - Val[1] * Dof_P->Case.Link.Coef2;
        valtmp[1] =
          Val[1] * Dof_P->Case.Link.Coef + Val[0] * Dof_P->Case.Link.Coef2;
      }
      Dof_AssembleInMat(Equ_P, Dof_P->Case.Link.Dof, NbrHar, valtmp, Mat, Vec,
                        Vecs);
      break;

    case DOF_FIXED_SOLVE:
    case DOF_FIXEDWITHASSOCIATE_SOLVE:
      Message::Error("Wrong Constraints: "
                     "remaining Dof(s) waiting to be fixed by a Resolution");
      break;

    case DOF_UNKNOWN_INIT:
      Message::Error("Wrong Initial Constraints: "
                     "remaining Dof(s) with non-fixed initial conditions");
      break;
    }

    break;

  case DOF_LINK:
    if(NbrHar == 1)
      valtmp[0] = Val[0] * Equ_P->Case.Link.Coef;
    else {
      valtmp[0] = Val[0] * Equ_P->Case.Link.Coef;
      valtmp[1] = Val[1] * Equ_P->Case.Link.Coef;
    }
    Dof_AssembleInMat(Equ_P->Case.Link.Dof, Dof_P, NbrHar, valtmp, Mat, Vec,
                      Vecs);
    break;

  case DOF_LINKCPLX:
    if(NbrHar == 1)
      Message::Error("LinkCplx only valid for Complex systems");
    else { /* Warning: conjugate! */
      valtmp[0] =
        Val[0] * Equ_P->Case.Link.Coef + Val[1] * Equ_P->Case.Link.Coef2;
      valtmp[1] =
        Val[1] * Equ_P->Case.Link.Coef - Val[0] * Equ_P->Case.Link.Coef2;
    }
    Dof_AssembleInMat(Equ_P->Case.Link.Dof, Dof_P, NbrHar, valtmp, Mat, Vec,
                      Vecs);
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ A s s e m b l e I n V e c                                       */
/* ------------------------------------------------------------------------ */

void Dof_AssembleInVec(struct Dof *Equ_P, struct Dof *Dof_P, int NbrHar,
                       double *Val, struct Solution *OtherSolution,
                       gVector *Vec0, gVector *Vec)
{
  gScalar tmp;
  double valtmp[2];
  double a, b, c, d;

  switch(Equ_P->Type) {
  case DOF_UNKNOWN:
  case DOF_FIXEDWITHASSOCIATE:

    switch(Dof_P->Type) {
    case DOF_UNKNOWN:
      if(NbrHar == 1) {
        if(Val[0]) {
          LinAlg_GetDoubleInVector(&a, Vec0, Dof_P->Case.Unknown.NumDof - 1);
          a *= Val[0];
          LinAlg_AddDoubleInVector(a, Vec, Equ_P->Case.Unknown.NumDof - 1);
        }
      }
      else {
        LinAlg_GetComplexInVector(
          &a, &b, Vec0, Dof_P->Case.Unknown.NumDof - 1,
          (gSCALAR_SIZE == 1) ? ((Dof_P + 1)->Case.Unknown.NumDof - 1) : -1);
        c = a * Val[0] - b * Val[1];
        d = a * Val[1] + b * Val[0];
        LinAlg_AddComplexInVector(
          c, d, Vec, Equ_P->Case.Unknown.NumDof - 1,
          (gSCALAR_SIZE == 1) ? ((Equ_P + 1)->Case.Unknown.NumDof - 1) : -1);
      }
      break;

    case DOF_FIXED:
    case DOF_FIXEDWITHASSOCIATE:
      if(NbrHar == 1) {
        if(Val[0]) {
          LinAlg_ProdScalarDouble(
            &Dof_P->Val,
            Val[0] *
              OtherSolution->TimeFunctionValues[Dof_P->Case.FixedAssociate
                                                  .TimeFunctionIndex],
            &tmp);
          LinAlg_AddScalarInVector(&tmp, Vec, Equ_P->Case.Unknown.NumDof - 1);
        }
      }
      else {
        if(gSCALAR_SIZE == 2) {
          LinAlg_ProdScalarComplex(
            &Dof_P->Val,
            Val[0] *
              OtherSolution->TimeFunctionValues[Dof_P->Case.FixedAssociate
                                                  .TimeFunctionIndex],
            Val[1] *
              OtherSolution->TimeFunctionValues[Dof_P->Case.FixedAssociate
                                                  .TimeFunctionIndex],
            &a, &b);
          LinAlg_AddComplexInVector(
            a, b, Vec, Equ_P->Case.Unknown.NumDof - 1,
            (gSCALAR_SIZE == 1) ? ((Equ_P + 1)->Case.Unknown.NumDof - 1) : -1);
        }
        else {
          Message::Error("Assemby in vectors with more than one harmonic not "
                         "yet implemented");
        }
      }
      break;

    case DOF_LINK:
      if(NbrHar == 1)
        valtmp[0] = Val[0] * Dof_P->Case.Link.Coef;
      else {
        valtmp[0] = Val[0] * Dof_P->Case.Link.Coef;
        valtmp[1] = Val[1] * Dof_P->Case.Link.Coef;
      }
      Dof_AssembleInVec(Equ_P, Dof_P->Case.Link.Dof, NbrHar, valtmp,
                        OtherSolution, Vec0, Vec);
      break;

    case DOF_LINKCPLX:
      if(NbrHar == 1)
        Message::Error("LinkCplx only valid for Complex systems");
      else {
        valtmp[0] =
          Val[0] * Dof_P->Case.Link.Coef - Val[1] * Dof_P->Case.Link.Coef2;
        valtmp[1] =
          Val[1] * Dof_P->Case.Link.Coef + Val[0] * Dof_P->Case.Link.Coef2;
      }
      Dof_AssembleInVec(Equ_P, Dof_P->Case.Link.Dof, NbrHar, valtmp,
                        OtherSolution, Vec0, Vec);
      break;

    case DOF_FIXED_SOLVE:
    case DOF_FIXEDWITHASSOCIATE_SOLVE:
      Message::Error("Wrong Constraints: "
                     "remaining Dof(s) waiting to be fixed by a Resolution");
      break;

    case DOF_UNKNOWN_INIT:
      Message::Error("Wrong Initial Constraints: "
                     "remaining Dof(s) with non-fixed initial conditions");
      break;
    }
    break;

  case DOF_LINK:
    if(NbrHar == 1)
      valtmp[0] = Val[0] * Equ_P->Case.Link.Coef;
    else {
      valtmp[0] = Val[0] * Equ_P->Case.Link.Coef;
      valtmp[1] = Val[1] * Equ_P->Case.Link.Coef;
    }
    Dof_AssembleInVec(Equ_P->Case.Link.Dof, Dof_P, NbrHar, valtmp,
                      OtherSolution, Vec0, Vec);
    break;

  case DOF_LINKCPLX:
    if(NbrHar == 1)
      Message::Error("LinkCplx only valid for Complex systems");
    else { /* Warning: conjugate! */
      valtmp[0] =
        Val[0] * Equ_P->Case.Link.Coef + Val[1] * Equ_P->Case.Link.Coef2;
      valtmp[1] =
        Val[1] * Equ_P->Case.Link.Coef - Val[0] * Equ_P->Case.Link.Coef2;
    }
    Dof_AssembleInVec(Equ_P->Case.Link.Dof, Dof_P, NbrHar, valtmp,
                      OtherSolution, Vec0, Vec);
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ T r a n s f e r S o l u t i o n T o C o n s t r a i n t         */
/* ------------------------------------------------------------------------ */

void Dof_TransferSolutionToConstraint(struct DofData *DofData_P)
{
  struct Dof *Dof_P, *Dof_P0;
  int i;

  if(!DofData_P->NbrAnyDof) { return; }

  if(!DofData_P->CurrentSolution) {
    Message::Warning("No solution to transfer to constraint - skipping");
    return;
  }

  if(!DofData_P->CurrentSolution->SolutionExist) {
    Message::Error("Empty solution in DofData %d", DofData_P->Num);
    return;
  }

  Dof_P0 = (struct Dof *)List_Pointer(DofData_P->DofList, 0);

  for(i = 0; i < DofData_P->NbrAnyDof; i++) {
    Dof_P = Dof_P0 + i;

    switch(Dof_P->Type) {
    case DOF_UNKNOWN:
      Dof_P->Type = DOF_FIXED;
      LinAlg_GetScalarInVector(&Dof_P->Val, &DofData_P->CurrentSolution->x,
                               Dof_P->Case.Unknown.NumDof - 1);
      Dof_P->Case.FixedAssociate.TimeFunctionIndex = 0;
      break;

    case DOF_FIXED:
    case DOF_FIXEDWITHASSOCIATE:
    case DOF_LINK:
    case DOF_LINKCPLX: break;

    default: break;
    }
  }

  DofData_P->NbrDof = 0;
}

/* ------------------------------------------------------------------------ */
/*  D o f _ G e t D o f V a l u e                                           */
/* ------------------------------------------------------------------------ */

gScalar Dof_GetDofValue(struct DofData *DofData_P, struct Dof *Dof_P)
{
  gScalar tmp;

  switch(Dof_P->Type) {
  case DOF_UNKNOWN:
    if(!DofData_P->CurrentSolution->SolutionExist)
      Message::Error("Empty solution in DofData %d", DofData_P->Num);
    else
      LinAlg_GetScalarInVector(&tmp, &DofData_P->CurrentSolution->x,
                               Dof_P->Case.Unknown.NumDof - 1);
    break;

  case DOF_FIXED:
  case DOF_FIXEDWITHASSOCIATE:
    LinAlg_ProdScalarDouble(
      &Dof_P->Val,
      ((Dof_P->Case.FixedAssociate.TimeFunctionIndex) ?
         DofData_P->CurrentSolution
           ->TimeFunctionValues[Dof_P->Case.FixedAssociate.TimeFunctionIndex] :
         1.),
      &tmp);
    break;

  case DOF_LINK:
    tmp = Dof_GetDofValue(DofData_P, Dof_P->Case.Link.Dof);
    LinAlg_ProdScalarDouble(&tmp, Dof_P->Case.Link.Coef, &tmp);
    break;

  case DOF_LINKCPLX:
    /* Too soon to treat LinkCplx: we need the real and imaginary parts */
    Message::Error("Cannot call Dof_GetDofValue for LinkCplx");
    break;

  default: LinAlg_ZeroScalar(&tmp); break;
  }

  return tmp;
}

void Dof_GetRealDofValue(struct DofData *DofData_P, struct Dof *Dof_P,
                         double *d)
{
  gScalar tmp;

  if(Dof_P->Type == DOF_LINKCPLX) {
    Message::Error("Cannot call Dof_GetRealDofValue for LinkCplx");
    return;
  }

  tmp = Dof_GetDofValue(DofData_P, Dof_P);
  LinAlg_GetDoubleInScalar(d, &tmp);
}

void Dof_GetComplexDofValue(struct DofData *DofData_P, struct Dof *Dof_P,
                            double *d1, double *d2)
{
  gScalar tmp1, tmp2;
  double valtmp[2];

  if(gSCALAR_SIZE == 1) {
    if(Dof_P->Type == DOF_LINKCPLX) { /* Can only be done here */
      if(Dof_P->Case.Link.Dof->Type == DOF_LINKCPLX) { /* recurse */
        Dof_GetComplexDofValue(DofData_P, Dof_P->Case.Link.Dof, d1, d2);
      }
      else {
        tmp1 = Dof_GetDofValue(DofData_P, Dof_P->Case.Link.Dof);
        tmp2 = Dof_GetDofValue(DofData_P, (Dof_P + 1)->Case.Link.Dof);
        LinAlg_GetDoubleInScalar(d1, &tmp1);
        LinAlg_GetDoubleInScalar(d2, &tmp2);
      }
    }
    else {
      tmp1 = Dof_GetDofValue(DofData_P, Dof_P);
      tmp2 = Dof_GetDofValue(DofData_P, Dof_P + 1);
      LinAlg_GetDoubleInScalar(d1, &tmp1);
      LinAlg_GetDoubleInScalar(d2, &tmp2);
    }
  }
  else {
    if(Dof_P->Type == DOF_LINKCPLX) { /* Can only be done here */
      if(Dof_P->Case.Link.Dof->Type == DOF_LINKCPLX) { /* recurse */
        Dof_GetComplexDofValue(DofData_P, Dof_P->Case.Link.Dof, d1, d2);
      }
      else {
        tmp1 = Dof_GetDofValue(DofData_P, Dof_P->Case.Link.Dof);
        LinAlg_GetComplexInScalar(d1, d2, &tmp1);
      }
    }
    else {
      tmp1 = Dof_GetDofValue(DofData_P, Dof_P);
      LinAlg_GetComplexInScalar(d1, d2, &tmp1);
    }
  }

  if(Dof_P->Type == DOF_LINKCPLX) {
    valtmp[0] = Dof_P->Case.Link.Coef * (*d1) - Dof_P->Case.Link.Coef2 * (*d2);
    valtmp[1] = Dof_P->Case.Link.Coef * (*d2) + Dof_P->Case.Link.Coef2 * (*d1);
    *d1 = valtmp[0];
    *d2 = valtmp[1];
  }
}

/* ------------------------------------------------------------------------- */
/*  D o f _ D e f i n e Unknown D o f F r o m Solve o r Init D o f           */
/* ------------------------------------------------------------------------- */

void Dof_DefineUnknownDofFromSolveOrInitDof(struct DofData **DofData_P)
{
  int i, Nbr_AnyDof;
  struct Dof *Dof_P;

  Nbr_AnyDof = List_Nbr((*DofData_P)->DofList);

  for(i = 0; i < Nbr_AnyDof; i++) {
    Dof_P = (struct Dof *)List_Pointer((*DofData_P)->DofList, i);

    switch(Dof_P->Type) {
    case DOF_FIXED_SOLVE:
    case DOF_FIXEDWITHASSOCIATE_SOLVE:
      Dof_P->Type = DOF_UNKNOWN;
      Dof_P->Case.Unknown.NumDof = ++((*DofData_P)->NbrDof);
      break;
    case DOF_UNKNOWN_INIT: Dof_P->Type = DOF_UNKNOWN; break;
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ T r a n s f e r D o f                                           */
/* ------------------------------------------------------------------------ */

void Dof_TransferDof(struct DofData *DofData_P1, struct DofData **DofData_P2)
{
  int i, Nbr_AnyDof;
  struct Dof Dof, *Dof_P;
  struct Solution *Solutions_P0;

  Nbr_AnyDof = List_Nbr(DofData_P1->DofList);
  Solutions_P0 = (struct Solution *)List_Pointer(DofData_P1->Solutions, 0);
  DofData_P1->CurrentSolution = Solutions_P0;

  for(i = 0; i < Nbr_AnyDof; i++) {
    Dof = *(struct Dof *)List_Pointer(DofData_P1->DofList, i);
    if((Dof_P = (struct Dof *)Dof_Find(*DofData_P2, &Dof, true))) {
      switch(Dof_P->Type) {
      case DOF_FIXED_SOLVE:
        Dof_P->Type = DOF_FIXED;
        Dof_P->Val = Dof_GetDofValue(DofData_P1, &Dof);
        break;
      case DOF_FIXEDWITHASSOCIATE_SOLVE:
        Dof_P->Type = DOF_FIXEDWITHASSOCIATE;
        Dof_P->Val = Dof_GetDofValue(DofData_P1, &Dof);
        break;
      case DOF_UNKNOWN_INIT:
        /* A DOF_UNKNOWN_INIT will always use the value obtained by
           pre-resolution even if a simple Init contraint is given; we should
           introduce DOF_UNKNOWN_INIT_SOLVE */
        Dof_P->Val = Dof_GetDofValue(DofData_P1, &Dof);
        if((DofData_P1->CurrentSolution - Solutions_P0) > 0) {
          DofData_P1->CurrentSolution -= 1;
          Dof_P->Val2 = Dof_GetDofValue(DofData_P1, &Dof);
          DofData_P1->CurrentSolution += 1;
        }
        else {
          LinAlg_ZeroScalar(&Dof_P->Val2);
        }
        break;
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  D o f _ I n i t D o f F o r N o D o f                                   */
/* ------------------------------------------------------------------------ */

void Dof_InitDofForNoDof(struct Dof *DofForNoDof, int NbrHar)
{
  int k;
  double Val[2] = {1., 0.};

  for(k = 0; k < NbrHar; k += gSCALAR_SIZE) {
    int incr = (gSCALAR_SIZE == 2) ? k / 2 : k;
    struct Dof *D = DofForNoDof + incr;
    D->Type = DOF_FIXED;
    LinAlg_SetScalar(&D->Val, &Val[k % 2]);
    D->Case.FixedAssociate.TimeFunctionIndex = 0;
  }
}

/* ------------------------------------------------------- */
/*  P r i n t _  D o f N u m b e r                         */
/* ------------------------------------------------------- */

void Print_DofNumber(struct Dof *Dof_P)
{
  switch(Dof_P->Type) {
  case DOF_UNKNOWN:
    printf("%d(%d) ", Dof_P->Case.Unknown.NumDof, Dof_P->Entity);
    break;
  case DOF_FIXED: printf("Fixed(%d) ", Dof_P->Entity); break;
  case DOF_FIXEDWITHASSOCIATE:
    printf("Assoc-%d ", Dof_P->Case.FixedAssociate.NumDof);
    break;
  case DOF_LINK:
    printf("Link-");
    Print_DofNumber(Dof_P->Case.Link.Dof);
    break;
  case DOF_LINKCPLX:
    printf("LinkCplx-");
    Print_DofNumber(Dof_P->Case.Link.Dof);
    break;
  default: printf(" ? "); break;
  }
}

/* ------------------------------------------------------- */
/*  D u m m y  D o f s                                     */
/* ------------------------------------------------------- */

void Dof_GetDummies(struct DefineSystem *DefineSystem_P,
                    struct DofData *DofData_P)
{
  struct Formulation *Formulation_P;
  struct DefineQuantity *DefineQuantity_P;
  struct FunctionSpace *FunctionSpace_P;
  struct BasisFunction *BasisFunction_P;
  struct GlobalQuantity *GlobalQuantity_P;
  struct Dof *Dof_P;

  int i, j, k, l, iDof, ii, iit, iNum, iHar;
  int Nbr_Formulation, Index_Formulation;
  int *DummyDof;
  double FrequencySpectrum, *Val_Pulsation;

  if(!(Val_Pulsation = Current.DofData->Val_Pulsation)) {
    Message::Error("Dof_GetDummies can only be used for harmonic problems");
    return;
  }

  DummyDof = DofData_P->DummyDof =
    (int *)Malloc(DofData_P->NbrDof * sizeof(int));
  for(iDof = 0; iDof < DofData_P->NbrDof; iDof++) DummyDof[iDof] = 0;

  Nbr_Formulation = List_Nbr(DefineSystem_P->FormulationIndex);

  for(i = 0; i < Nbr_Formulation; i++) {
    List_Read(DefineSystem_P->FormulationIndex, i, &Index_Formulation);
    Formulation_P = (struct Formulation *)List_Pointer(Problem_S.Formulation,
                                                       Index_Formulation);
    for(j = 0; j < List_Nbr(Formulation_P->DefineQuantity); j++) {
      DefineQuantity_P =
        (struct DefineQuantity *)List_Pointer(Formulation_P->DefineQuantity, j);
      for(l = 0; l < List_Nbr(DefineQuantity_P->FrequencySpectrum); l++) {
        FrequencySpectrum =
          *(double *)List_Pointer(DefineQuantity_P->FrequencySpectrum, l);

        iHar = -1;
        for(k = 0; k < Current.NbrHar / 2; k++)
          if(fabs(Val_Pulsation[k] - TWO_PI * FrequencySpectrum) <=
             1e-10 * Val_Pulsation[k]) {
            iHar = 2 * k;
            break;
          }
        if(iHar >= 0) {
          FunctionSpace_P = (struct FunctionSpace *)List_Pointer(
            Problem_S.FunctionSpace, DefineQuantity_P->FunctionSpaceIndex);

          for(k = 0; k < List_Nbr(FunctionSpace_P->BasisFunction); k++) {
            BasisFunction_P = (struct BasisFunction *)List_Pointer(
              FunctionSpace_P->BasisFunction, k);
            iNum = ((struct BasisFunction *)BasisFunction_P)->Num;
            ii = iit = 0;
            for(iDof = 0; iDof < List_Nbr(DofData_P->DofList); iDof++) {
              Dof_P = (struct Dof *)List_Pointer(DofData_P->DofList, iDof);
              if(Dof_P->Type == DOF_UNKNOWN && Dof_P->NumType == iNum) {
                iit++;
                if(Dof_P->Harmonic == iHar || Dof_P->Harmonic == iHar + 1) {
                  DummyDof[Dof_P->Case.Unknown.NumDof - 1] = 1;
                  ii++;
                }
              }
            }
            if(ii)
              Message::Info(
                "Freq %4lg (%d/%d) Formulation %s Quantity %s "
                "(BF %d)  #DofsNotInFreqSpectrum %d/%d",
                Val_Pulsation[iHar / 2] / TWO_PI, iHar / 2, Current.NbrHar / 2,
                Formulation_P->Name, DefineQuantity_P->Name,
                ((struct BasisFunction *)BasisFunction_P)->Num, ii, iit);
          }

          for(k = 0; k < List_Nbr(FunctionSpace_P->GlobalQuantity); k++) {
            GlobalQuantity_P = (struct GlobalQuantity *)List_Pointer(
              FunctionSpace_P->GlobalQuantity, k);
            iNum = ((struct GlobalQuantity *)GlobalQuantity_P)->Num;
            ii = iit = 0;
            for(iDof = 0; iDof < List_Nbr(DofData_P->DofList); iDof++) {
              Dof_P = (struct Dof *)List_Pointer(DofData_P->DofList, iDof);
              if(Dof_P->Type == DOF_UNKNOWN && Dof_P->NumType == iNum) {
                iit++;
                if(Dof_P->Harmonic == iHar || Dof_P->Harmonic == iHar + 1) {
                  DummyDof[Dof_P->Case.Unknown.NumDof - 1] = 1;
                  ii++;
                }
              }
            }
            if(ii)
              Message::Info(
                "Freq %4lg (%d/%d) Formulation %s  GlobalQuantity %s "
                "(BF %d)  #DofsNotInFrequencySpectrum %d/%d",
                Val_Pulsation[iHar / 2] / TWO_PI, iHar / 2, Current.NbrHar / 2,
                Formulation_P->Name, GlobalQuantity_P->Name,
                ((struct GlobalQuantity *)GlobalQuantity_P)->Num, ii, iit);
          }

        } /*  end FrequencySpectrum in DofData */
      } /* end FrequencySpectrum in Quantity */
    } /* end Quantity */
  } /* end Formulation */

  i = 0;
  for(iDof = 0; iDof < DofData_P->NbrDof; iDof++) {
    if(DummyDof[iDof]) i++;

    if(Message::GetVerbosity() == 99) {
      Dof_P = (struct Dof *)List_Pointer(DofData_P->DofList, iDof);
      Message::Debug("Dof Num iHar, Entity %d %d %d", iDof, Dof_P->NumType,
                     Dof_P->Harmonic, Dof_P->Entity);
    }
  }

  Message::Info("N: %d - N with FrequencySpectrum: %d", DofData_P->NbrDof, i);
}
