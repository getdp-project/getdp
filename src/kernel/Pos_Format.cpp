// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#include <sstream>
#include <map>
#include <vector>
#include <string>
#include <list>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <set>
#include <climits>
#include <algorithm>
#include <array>
#include <unordered_map>

#include "GetDPVersion.h"
#include "GetDPConfig.h"
#include "ProData.h"
#include "ProDefine.h"
#include "ProParser.h"
#include "GeoData.h"
#include "DofData.h"
#include "Pos_Iso.h"
#include "Pos_Format.h"
#include "Pos_Element.h"
#include "Pos_Formulation.h"
#include "Pos_Print.h"
#include "F.h"
#include "Cal_Value.h"
#include "Cal_Quantity.h"
#include "MallocUtils.h"
#include "Message.h"
#include "kissfft.hh"

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

#if defined(HAVE_GMSH)
#include <gmsh/PView.h>
#include <gmsh/PViewDataList.h>
#include <gmsh/PViewDataGModel.h>
#endif

#define TWO_PI 6.2831853071795865

#define NBR_MAX_ISO 200

#define SQU(a) ((a) * (a))

extern struct Problem Problem_S;
extern struct CurrentData Current;

extern int Flag_BIN, Flag_GMSH_LIST_BASED;

extern FILE *PostStream;
extern char PostFileName[256];

static List_T *PostElement_L = NULL;
static List_T *TimeValue_L = NULL;

/* ------------------------------------------------------------------------ */
/*  Gmsh formats                                                            */
/* ------------------------------------------------------------------------ */

// global static lists for new-style Gmsh output (cannot be saved incrementally
// for each element)
static int Gmsh_StartNewView = 0;
// the values of the elements of each shape (point, line, triangle,
// quadrangle, tetrahedron, hexahedron, prism, pyramid) and of each kind
// (scalar, vector, tensor), with the number of elements in each list
static const int GmshNumShapes = 8, GmshNumKinds = 3;
static const int GmshNumComp[GmshNumKinds] = {1, 3, 9};
static int GmshNb[GmshNumShapes][GmshNumKinds];
static std::vector<double> GmshList[GmshNumShapes][GmshNumKinds];
// the strings (e.g. the legend) of a list-based view
static int NbT2;
static std::vector<double> T2D;
static std::vector<char> T2C;
static char CurrentName[256] = "";
static int CurrentPartitionNumber = 0;

// A Gmsh view is written either "mesh-based", i.e. as the values of the
// elements of the mesh, identified by their number ($ElementNodeData), or
// "list-based", i.e. as lists of independent elements given by the coordinates
// of their nodes. A Print on the elements of the mesh is mesh-based (unless
// -list-based); all the others, which create their own elements (cuts, grids,
// skins, iso-values, values at Gauss points, ...), move the nodes or need what
// only the list-based format has (several values per time step, legends),
// are list-based.
bool Format_GmshMeshBased(struct PostSubOperation *PSO_P)
{
  if(PSO_P->StoreInMeshBasedField >= 0) return true;
  return !Flag_GMSH_LIST_BASED && PSO_P->Type == POP_PRINT &&
         PSO_P->SubType == PRINT_ONELEMENTSOF && PSO_P->Depth == 1 &&
         PSO_P->StoreInField < 0 && !PSO_P->Skin && !PSO_P->Iso &&
         !PSO_P->DecomposeInSimplex && !PSO_P->Gauss &&
         PSO_P->HarmonicToTime <= 1 && PSO_P->ChangeOfCoordinates[0] < 0 &&
         PSO_P->Legend == LEGEND_NONE;
}

static void Gmsh_ResetStaticLists()
{
  for(int shape = 0; shape < GmshNumShapes; shape++) {
    for(int kind = 0; kind < GmshNumKinds; kind++) {
      GmshNb[shape][kind] = 0;
      GmshList[shape][kind].clear();
    }
  }
  NbT2 = 0;
  T2D.clear();
  T2C.clear();
  if(!TimeValue_L)
    TimeValue_L = List_Create(100, 1000000, sizeof(double));
  else
    List_Reset(TimeValue_L);
}

static void Gmsh_StringStart(int Format, double x, double y, double style)
{
  if(Flag_BIN) { /* bricolage: should use Format instead */
    T2D.push_back(x);
    T2D.push_back(y);
    T2D.push_back(style);
    T2D.push_back(T2C.size());
    NbT2++;
  }
  else if(PostStream) {
    fprintf(PostStream, "T2(%g,%g,%g){", x, y, style);
  }
}

static void Gmsh_StringAdd(int Format, int first, char *text)
{
  int i;
  if(Flag_BIN) { /* bricolage: should use Format instead */
    for(i = 0; i < (int)strlen(text) + 1; i++) T2C.push_back(text[i]);
  }
  else if(PostStream) {
    if(!first) fprintf(PostStream, ",");
    fprintf(PostStream, "\"%s\"", text);
  }
}

static void Gmsh_StringEnd(int Format)
{
  if(Flag_BIN) { /* bricolage: should use Format instead */
  }
  else if(PostStream) {
    fprintf(PostStream, "};\n");
  }
}

// In parallel a mesh-based view is evaluated by all the ranks, each one on the
// elements of its mesh partition (see Format_SetGmshParallel). Each rank
// formats its values as a $ElementNodeData section of its own, tagged with its
// rank as "partition", in GmshBuffer; the sections are then written one after
// the other in the file (see Message::WriteParallelSections).
// Gmsh assembles the sections of a view when it reads the file.
static bool GmshParallel = false;
static std::string GmshBuffer;

void Format_SetGmshParallel(bool parallel) { GmshParallel = parallel; }
bool Format_GetGmshParallel() { return GmshParallel; }

bool Format_CanGmshParallel(struct PostSubOperation *PSO_P)
{
#if defined(HAVE_MPI)
  return Message::GetCommSize() > 1 && Message::GetIsCommWorld() &&
         PSO_P->Format == FORMAT_GMSH && Format_GmshMeshBased(PSO_P) &&
         PSO_P->FileOut && PSO_P->StoreInMeshBasedField < 0 &&
         PSO_P->StoreInRegister < 0 && !PSO_P->StoreInVariable &&
         !PSO_P->SingleProcess && !PSO_P->Smoothing && !PSO_P->Adapt &&
         !PSO_P->Sort && !Pos_StoresMinMax(PSO_P);
#else
  return false;
#endif
}

static void Gmsh_Write(const void *data, size_t size)
{
  if(GmshParallel)
    GmshBuffer.append((const char *)data, size);
  else
    fwrite(data, 1, size, PostStream);
}

static void Gmsh_Printf(const char *fmt, ...)
{
  char str[1024];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(str, sizeof(str), fmt, args);
  va_end(args);
  Gmsh_Write(str, std::min(n, (int)sizeof(str) - 1));
}

static void Gmsh_WriteSizeTs(const std::size_t *v, int n, const char *sep)
{
  if(Flag_BIN)
    Gmsh_Write(v, n * sizeof(std::size_t));
  else
    for(int i = 0; i < n; i++)
      Gmsh_Printf("%zu%s", v[i], (i == n - 1) ? "\n" : sep);
}

// The values of a mesh-based view, as $ElementNodeData sections of MSH 4.2
// (one for each time step and harmonic): one block for the elements of each
// shape, with their tags and then their values.
static void Gmsh_PrintElementNodeData(struct PostSubOperation *PSO_P,
                                      int numTimeStep, int numComp,
                                      int Nb[GmshNumShapes],
                                      std::vector<double> *L[GmshNumShapes])
{
  if(!PostStream && !GmshParallel) return;
  std::size_t N = 0, numBlocks = 0, minTag = 0, maxTag = 0;
  for(int i = 0; i < GmshNumShapes; i++) {
    if(!Nb[i]) continue;
    N += Nb[i];
    numBlocks++;
    int stride = (*L[i]).size() / Nb[i];
    for(unsigned int j = 0; j < (*L[i]).size(); j += stride) {
      std::size_t num = (std::size_t)(*L[i])[j];
      minTag = (minTag && minTag < num) ? minTag : num;
      maxTag = std::max(maxTag, num);
    }
  }
  if(!N) return;
  int step = 0;
  for(int ts = 0; ts < numTimeStep; ts++) {
    Pos_InitAllSolutions(PSO_P->TimeStep_L, ts);
    for(int har = 0; har < Current.NbrHar; har++) {
      Gmsh_Printf("$ElementNodeData\n");
      Gmsh_Printf("1\n");
      Gmsh_Printf("\"%s\"\n", CurrentName);
      Gmsh_Printf("1\n");
      Gmsh_Printf("%.16g\n", Current.Time);
      Gmsh_Printf("4\n");
      Gmsh_Printf("%d\n", (PSO_P->OverrideTimeStepValue >= 0) ?
                            PSO_P->OverrideTimeStepValue :
                            (Current.NbrHar > 1 ? step : (int)Current.TimeStep));
      Gmsh_Printf("%d\n", numComp);
      Gmsh_Printf("%zu\n", N);
      Gmsh_Printf("%d\n", GmshParallel ? Message::GetCommRank() :
                                         CurrentPartitionNumber);
      std::size_t header[4] = {numBlocks, N, minTag, maxTag};
      Gmsh_WriteSizeTs(header, 4, " ");
      for(int i = 0; i < GmshNumShapes; i++) {
        if(!Nb[i]) continue;
        int stride = (*L[i]).size() / Nb[i];
        int mult = (stride - 1) / numTimeStep / Current.NbrHar / numComp;
        // (the element is in the mesh of the rank that evaluated it)
        struct Geo_Element *GE = Geo_GetGeoElementOfNum((int)(*L[i])[0]);
        int type[2] = {GetDP2Gmsh(GE->Type), mult};
        // (the elements of the order of the Print)
        if(PSO_P->ExportOrder) {
          const struct LagrangePostElement *LE =
            Get_LagrangePostElement(GE, PSO_P->ExportOrder);
          if(LE) type[0] = LE->GmshType;
        }
        std::size_t n = Nb[i];
        if(Flag_BIN) {
          Gmsh_Write(type, 2 * sizeof(int));
          Gmsh_Write(&n, sizeof(std::size_t));
        }
        else
          Gmsh_Printf("%d %d %zu\n", type[0], type[1], n);
        for(unsigned int j = 0; j < (*L[i]).size(); j += stride) {
          std::size_t num = (std::size_t)(*L[i])[j];
          Gmsh_WriteSizeTs(&num, 1, "");
        }
        for(unsigned int j = 0; j < (*L[i]).size(); j += stride) {
          double *values = &(*L[i])[j + 1 + step * mult * numComp];
          if(Flag_BIN)
            Gmsh_Write(values, sizeof(double) * mult * numComp);
          else {
            for(int k = 0; k < mult * numComp; k++)
              Gmsh_Printf((k ? " %.16g" : "%.16g"), values[k]);
            Gmsh_Printf("\n");
          }
        }
      }
      if(Flag_BIN) Gmsh_Printf("\n");
      Gmsh_Printf("$EndElementNodeData\n");
      step++;
    }
  }
}

static void GmshParsed_PrintElement(double Time, int TimeStep, int NbTimeStep,
                                    int NbHarmonic, int HarmonicToTime,
                                    int Type, int NbrNodes, double *x,
                                    double *y, double *z, struct Value *Value)
{
  int i, j, k, jj;
  double TimeMH;
  struct Value TmpValue;
  int symIndex[9] = {0, 1, 2, 1, 3, 4, 2, 4, 5};
  int diagIndex[9] = {0, -1, -1, -1, 1, -1, -1, -1, 2};

  if(Gmsh_StartNewView) {
    Gmsh_StartNewView = 0;
    Gmsh_ResetStaticLists();
  }

  if(HarmonicToTime == 1) {
    if(NbHarmonic == 2 && NbTimeStep == 1) { // classical complex case
      double zero = 0., one = 1.;
      List_Put(TimeValue_L, 0, &zero);
      List_Put(TimeValue_L, 1, &one);
    }
    else {
      for(k = 0; k < NbHarmonic; k++)
        List_Put(TimeValue_L, NbHarmonic * TimeStep + k, &Time);
    }
  }
  else
    for(k = 0; k < HarmonicToTime; k++)
      List_Put(TimeValue_L, HarmonicToTime * TimeStep + k, &Time);

  if(!PostStream) return;

  switch(Value[0].Type) {
  case SCALAR:

    if(TimeStep == 0) {
      switch(Type) {
      case POINT_ELEMENT: fprintf(PostStream, "SP("); break;
      case LINE: fprintf(PostStream, "SL("); break;
      case TRIANGLE: fprintf(PostStream, "ST("); break;
      case QUADRANGLE: fprintf(PostStream, "SQ("); break;
      case TETRAHEDRON: fprintf(PostStream, "SS("); break;
      case HEXAHEDRON: fprintf(PostStream, "SH("); break;
      case PRISM: fprintf(PostStream, "SI("); break;
      case PYRAMID: fprintf(PostStream, "SY("); break;
      }
      for(i = 0; i < NbrNodes; i++) {
        if(i) fprintf(PostStream, ",");
        fprintf(PostStream, "%.16g,%.16g,%.16g", x[i], y[i], z[i]);
      }
      fprintf(PostStream, "){");
    }

    if(HarmonicToTime == 1) {
      for(k = 0; k < NbHarmonic; k++) {
        if(k || TimeStep) fprintf(PostStream, ",");
        for(i = 0; i < NbrNodes; i++) {
          if(i) fprintf(PostStream, ",");
          fprintf(PostStream, "%.16g", Value[i].Val[MAX_DIM * k]);
        }
      }
    }
    else {
      for(k = 0; k < HarmonicToTime; k++) {
        if(k || TimeStep) fprintf(PostStream, ",");
        for(i = 0; i < NbrNodes; i++) {
          if(i) fprintf(PostStream, ",");
          F_MHToTime0(k + i, &Value[i], &TmpValue, k, HarmonicToTime, &TimeMH);
          fprintf(PostStream, "%.16g", TmpValue.Val[0]);
        }
      }
    }

    if(TimeStep == NbTimeStep - 1) { fprintf(PostStream, "};\n"); }
    break;

  case VECTOR:

    if(TimeStep == 0) {
      switch(Type) {
      case POINT_ELEMENT: fprintf(PostStream, "VP("); break;
      case LINE: fprintf(PostStream, "VL("); break;
      case TRIANGLE: fprintf(PostStream, "VT("); break;
      case QUADRANGLE: fprintf(PostStream, "VQ("); break;
      case TETRAHEDRON: fprintf(PostStream, "VS("); break;
      case HEXAHEDRON: fprintf(PostStream, "VH("); break;
      case PRISM: fprintf(PostStream, "VI("); break;
      case PYRAMID: fprintf(PostStream, "VY("); break;
      }
      for(i = 0; i < NbrNodes; i++) {
        if(i) fprintf(PostStream, ",");
        fprintf(PostStream, "%.16g,%.16g,%.16g", x[i], y[i], z[i]);
      }
      fprintf(PostStream, "){");
    }

    if(HarmonicToTime == 1) {
      for(k = 0; k < NbHarmonic; k++) {
        if(k || TimeStep) fprintf(PostStream, ",");
        for(i = 0; i < NbrNodes; i++) {
          if(i) fprintf(PostStream, ",");
          for(j = 0; j < 3; j++) {
            if(j) fprintf(PostStream, ",");
            fprintf(PostStream, "%.16g", Value[i].Val[MAX_DIM * k + j]);
          }
        }
      }
    }
    else {
      for(k = 0; k < HarmonicToTime; k++) {
        if(k || TimeStep) fprintf(PostStream, ",");
        for(i = 0; i < NbrNodes; i++) {
          if(i) fprintf(PostStream, ",");
          F_MHToTime0(k + i, &Value[i], &TmpValue, k, HarmonicToTime, &TimeMH);
          for(j = 0; j < 3; j++) {
            if(j) fprintf(PostStream, ",");
            fprintf(PostStream, "%.16g", TmpValue.Val[j]);
          }
        }
      }
    }

    if(TimeStep == NbTimeStep - 1) { fprintf(PostStream, "};\n"); }
    break;

  case TENSOR_DIAG:
  case TENSOR_SYM:
  case TENSOR:

    if(TimeStep == 0) {
      switch(Type) {
      case POINT_ELEMENT: fprintf(PostStream, "TP("); break;
      case LINE: fprintf(PostStream, "TL("); break;
      case TRIANGLE: fprintf(PostStream, "TT("); break;
      case QUADRANGLE: fprintf(PostStream, "TQ("); break;
      case TETRAHEDRON: fprintf(PostStream, "TS("); break;
      case HEXAHEDRON: fprintf(PostStream, "TH("); break;
      case PRISM: fprintf(PostStream, "TI("); break;
      case PYRAMID: fprintf(PostStream, "TY("); break;
      }
      for(i = 0; i < NbrNodes; i++) {
        if(i) fprintf(PostStream, ",");
        fprintf(PostStream, "%.16g,%.16g,%.16g", x[i], y[i], z[i]);
      }
      fprintf(PostStream, "){");
    }

    if(HarmonicToTime == 1) {
      for(k = 0; k < NbHarmonic; k++) {
        if(k || TimeStep) fprintf(PostStream, ",");
        for(i = 0; i < NbrNodes; i++) {
          if(i) fprintf(PostStream, ",");
          for(j = 0; j < 9; j++) {
            if(j) fprintf(PostStream, ",");
            if(Value[0].Type != TENSOR_DIAG) {
              if(Value[0].Type == TENSOR_SYM)
                jj = symIndex[j];
              else
                jj = j;
              fprintf(PostStream, "%.16g", Value[i].Val[MAX_DIM * k + jj]);
            }
            else {
              jj = diagIndex[j];
              if(jj == -1)
                fprintf(PostStream, "%.16g", 0.);
              else
                fprintf(PostStream, "%.16g", Value[i].Val[MAX_DIM * k + jj]);
            }
          }
        }
      }
    }
    else {
      for(k = 0; k < HarmonicToTime; k++) {
        if(k || TimeStep) fprintf(PostStream, ",");
        for(i = 0; i < NbrNodes; i++) {
          if(i) fprintf(PostStream, ",");
          F_MHToTime0(k + i, &Value[i], &TmpValue, k, HarmonicToTime, &TimeMH);
          for(j = 0; j < 9; j++) {
            if(j) fprintf(PostStream, ",");
            if(Value[0].Type != TENSOR_DIAG) {
              if(Value[0].Type == TENSOR_SYM)
                jj = symIndex[j];
              else
                jj = j;
              jj = symIndex[j];
              fprintf(PostStream, "%.16g", TmpValue.Val[jj]);
            }
            else {
              jj = diagIndex[j];
              if(jj == -1)
                fprintf(PostStream, "%.16g", 0.);
              else
                fprintf(PostStream, "%.16g", TmpValue.Val[jj]);
            }
          }
        }
      }
    }

    if(TimeStep == NbTimeStep - 1) { fprintf(PostStream, "};\n"); }
    break;
  }
}

static int Gmsh_ShapeOfElement(int Type)
{
  switch(Type) {
  case POINT_ELEMENT: return 0;
  case LINE: case LINE_2: case LINE_3: case LINE_4: return 1;
  case TRIANGLE: case TRIANGLE_2: case TRIANGLE_3: case TRIANGLE_4: return 2;
  case QUADRANGLE: case QUADRANGLE_2: case QUADRANGLE_2_8N: case QUADRANGLE_3:
  case QUADRANGLE_4: return 3;
  case TETRAHEDRON: case TETRAHEDRON_2: case TETRAHEDRON_3: case TETRAHEDRON_4:
    return 4;
  case HEXAHEDRON: case HEXAHEDRON_2: case HEXAHEDRON_2_20N: case HEXAHEDRON_3:
  case HEXAHEDRON_4: return 5;
  case PRISM: case PRISM_2: case PRISM_2_15N: case PRISM_3: case PRISM_4:
    return 6;
  case PYRAMID: case PYRAMID_2: case PYRAMID_2_13N: case PYRAMID_3: return 7;
  default: return -1;
  }
}

// Adds the values of an element, for one time step, to the list of its shape
// and kind. A list-based view (meshBased false) stores for each element the
// coordinates of its nodes, then its values for all the steps; a mesh-based one
// stores the number of the element instead of the coordinates.
static void Gmsh_PrintElement(double Time, int TimeStep, int NbTimeStep,
                              int NbHarmonic, int HarmonicToTime, int Type,
                              int ElementNum, int NbrNodes, double *x,
                              double *y, double *z, struct Value *Value,
                              struct PostSubOperation *PSO_P, int Store)
{
  if(Gmsh_StartNewView) {
    Gmsh_StartNewView = 0;
    Gmsh_ResetStaticLists();
  }

  int shape = Gmsh_ShapeOfElement(Type), kind;
  switch(Value[0].Type) {
  case SCALAR: kind = 0; break;
  case VECTOR: kind = 1; break;
  case TENSOR_DIAG:
  case TENSOR_SYM:
  case TENSOR: kind = 2; break;
  default: return;
  }
  if(shape < 0) return;
  std::vector<double> &list = GmshList[shape][kind];

  if(TimeStep == 0) {
    GmshNb[shape][kind]++;
    if(!Format_GmshMeshBased(PSO_P)) {
      for(int i = 0; i < NbrNodes; i++) list.push_back(x[i]);
      for(int i = 0; i < NbrNodes; i++) list.push_back(y[i]);
      for(int i = 0; i < NbrNodes; i++) list.push_back(z[i]);
    }
    else {
      list.push_back(ElementNum);
    }
  }

  // component j of a value, a tensor being always written with 9 components
  static const int symIndex[9] = {0, 1, 2, 1, 3, 4, 2, 4, 5};
  static const int diagIndex[9] = {0, -1, -1, -1, 1, -1, -1, -1, 2};
  int valueType = Value[0].Type;
  auto component = [&](const double *val, int j) {
    if(valueType == TENSOR_SYM) return val[symIndex[j]];
    if(valueType == TENSOR_DIAG)
      return (diagIndex[j] < 0) ? 0. : val[diagIndex[j]];
    return val[j];
  };

  if(HarmonicToTime == 1) {
    for(int k = 0; k < NbHarmonic; k++) {
      List_Put(TimeValue_L, NbHarmonic * TimeStep + k, &Time);
      for(int i = 0; i < NbrNodes; i++)
        for(int j = 0; j < GmshNumComp[kind]; j++)
          list.push_back(component(&Value[i].Val[MAX_DIM * k], j));
    }
  }
  else {
    for(int k = 0; k < HarmonicToTime; k++) {
      List_Put(TimeValue_L, HarmonicToTime * TimeStep + k, &Time);
      for(int i = 0; i < NbrNodes; i++) {
        struct Value TmpValue;
        double TimeMH;
        F_MHToTime0(k + i, &Value[i], &TmpValue, k, HarmonicToTime, &TimeMH);
        for(int j = 0; j < GmshNumComp[kind]; j++)
          list.push_back(component(TmpValue.Val, j));
      }
    }
  }

  // reduce memory requirements by automatically partitioning large
  // output views into chunks not larger than 1Gb
  // (not in parallel, where each rank only has a part of the view and the
  // footer is an exchange between the ranks)
  if(Format_GmshMeshBased(PSO_P) && !GmshParallel && TimeStep == NbTimeStep - 1 &&
     list.size() > (int)(1024 * 1024 * 1024 / sizeof(double))) {
    Format_PostFooter(PSO_P, Store);
    CurrentPartitionNumber++;
    Gmsh_StartNewView = 1;
  }
}

static void dVecWrite(std::vector<double> &v, FILE *fp, bool binary)
{
  if(v.empty()) return;
  if(binary)
    fwrite(&v[0], sizeof(double), v.size(), fp);
  else
    for(unsigned i = 0; i < v.size(); i++) fprintf(fp, " %.16g", v[i]);
}

static void cVecWrite(std::vector<char> &v, FILE *fp, bool binary)
{
  if(v.empty()) return;
  if(binary)
    fwrite(&v[0], sizeof(char), v.size(), fp);
  else
    for(unsigned i = 0; i < v.size(); i++) fputc(v[i], fp);
}

/* ------------------------------------------------------------------------ */
/*  Gnuplot format                                                          */
/* ------------------------------------------------------------------------ */

static void Gnuplot_PrintElement(int Format, double Time, int TimeStep,
                                 int NbrTimeSteps, int NbrHarmonics,
                                 int HarmonicToTime, int ElementType,
                                 int NumElement, int NbrNodes, double *x,
                                 double *y, double *z, double *Dummy,
                                 struct Value *Value)
{
  static int Size, TmpIndex;
  static double *TmpValues;
  int i, j, k, t, i2, k2;
  double TimeMH;
  struct Value TmpValue;

  if(!PostStream) return;

  if(TimeStep == 0) {
    switch(Value->Type) {
    case SCALAR: Size = 1; break;
    case VECTOR: Size = 3; break;
    case TENSOR_DIAG: Size = 3; break;
    case TENSOR_SYM: Size = 6; break;
    case TENSOR: Size = 9; break;
    }
    TmpValues = (double *)Malloc(NbrTimeSteps * NbrNodes * NbrHarmonics * Size *
                                 sizeof(double));
    TmpIndex = 0;
  }

  for(i = 0; i < NbrNodes; i++)
    for(k = 0; k < NbrHarmonics; k++)
      for(j = 0; j < Size; j++)
        TmpValues[TmpIndex++] = Value[i].Val[MAX_DIM * k + j];

  if(TimeStep == NbrTimeSteps - 1) {
    for(i = 0; i <= NbrNodes;
        i++) { /* New line for each node, closed loop for tri/qua */

      if(i != NbrNodes)
        i2 = i;
      else {
        if(NbrNodes < 3)
          break;
        else
          i2 = 0;
      }

      fprintf(PostStream, "%d %d ", GetDP2Gmsh(ElementType), NumElement);
      fprintf(PostStream, " %.16g %.16g %.16g ", x[i2], y[i2], z[i2]);
      if(Dummy) {
        if(Dummy[3] < 0) {
          if(!i)
            fprintf(PostStream, " %.16g %.16g 0 ", Dummy[0], Dummy[2]);
          else
            fprintf(PostStream, " %.16g %.16g 0 ", Dummy[1], Dummy[2]);
        }
        else
          fprintf(PostStream, " %.16g %.16g %.16g ", Dummy[0], Dummy[1],
                  Dummy[2]);
      }
      else
        fprintf(PostStream, " 0 0 0 ");

      for(t = 0; t < NbrTimeSteps; t++) {
        if(HarmonicToTime == 1) {
          for(k = 0; k < NbrHarmonics; k++) {
            for(j = 0; j < Size; j++) {
              fprintf(PostStream, " %.16g",
                      TmpValues[t * NbrNodes * NbrHarmonics * Size +
                                i2 * NbrHarmonics * Size + k * Size + j]);
            }
            fprintf(PostStream, " ");
          }
        }
        else {
          TmpValue.Type = Value->Type;
          for(k = 0; k < HarmonicToTime; k++) {
            for(k2 = 0; k2 < NbrHarmonics; k2++)
              for(j = 0; j < Size; j++)
                TmpValue.Val[MAX_DIM * k2 + j] =
                  TmpValues[t * NbrNodes * NbrHarmonics * Size +
                            i2 * NbrHarmonics * Size + k2 * Size + j];

            F_MHToTime0(k, &TmpValue, &TmpValue, k, HarmonicToTime, &TimeMH);
            for(j = 0; j < Size; j++)
              fprintf(PostStream, "%.16g", TmpValue.Val[0]);
            fprintf(PostStream, " ");
          }
        }
        fprintf(PostStream, " ");

      } /* for t */
      fprintf(PostStream, "\n");

    } /* for i */
    if(NbrNodes > 1) fprintf(PostStream, "\n");

    Free(TmpValues);
  }
}

/* ------------------------------------------------------------------------ */
/*  Tabular format                                                          */
/* ------------------------------------------------------------------------ */

// the values of a table, kept in GetDPNumbers (see Format_PostFooter)
static int TableList_StartNew = 0;
static std::vector<double> TableList;

static void Tabular_PrintElement(struct PostSubOperation *PSO_P, int Format,
                                 double Time, int TimeStep, int NbrTimeSteps,
                                 int NbrHarmonics, int HarmonicToTime,
                                 int ElementType, int NumElement, int NbrNodes,
                                 double *x, double *y, double *z, double *Dummy,
                                 struct Value *Value)
{
  static int Size;
  int i, j, k;
  double TimeMH;
  struct Value TmpValue;

  if(TableList_StartNew) {
    TableList_StartNew = 0;
    TableList.clear();
  }

  if(!PostStream) return;

  if(TimeStep == 0) {
    switch(Value->Type) {
    case SCALAR: Size = 1; break;
    case VECTOR: Size = 3; break;
    case TENSOR_DIAG: Size = 3; break;
    case TENSOR_SYM: Size = 6; break;
    case TENSOR: Size = 9; break;
    }
  }

  if(Format == FORMAT_SPACE_TABLE || Format == FORMAT_SIMPLE_SPACE_TABLE ||
     Format == FORMAT_VALUE_ONLY) {
    if(TimeStep == 0) {
      if(Format != FORMAT_SIMPLE_SPACE_TABLE && Format != FORMAT_VALUE_ONLY) {
        fprintf(PostStream, "%d %d  ", GetDP2Gmsh(ElementType), NumElement);
        TableList.push_back(GetDP2Gmsh(ElementType));
        TableList.push_back(NumElement);
      }
      if(Format != FORMAT_VALUE_ONLY)
        for(i = 0; i < NbrNodes; i++) {
          fprintf(PostStream, "%.16g %.16g %.16g  ", x[i], y[i], z[i]);
          TableList.push_back(x[i]);
          TableList.push_back(y[i]);
          TableList.push_back(z[i]);
        }
      if(Format != FORMAT_SIMPLE_SPACE_TABLE && Format != FORMAT_VALUE_ONLY) {
        if(Dummy) {
          fprintf(PostStream, "%.16g %.16g %.16g  ", Dummy[0], Dummy[1],
                  Dummy[2]);
          TableList.push_back(Dummy[0]);
          TableList.push_back(Dummy[1]);
          TableList.push_back(Dummy[2]);
        }
        else {
          fprintf(PostStream, "0 0 0  ");
          TableList.push_back(0);
          TableList.push_back(0);
          TableList.push_back(0);
        }
      }
    }
  }

  if(HarmonicToTime == 1) {
    if(Format == FORMAT_TIME_TABLE) {
      fprintf(PostStream, "%d %.16g ", TimeStep, Time);
      TableList.push_back(TimeStep);
      TableList.push_back(Time);
      for(i = 0; i < NbrNodes; i++) {
        fprintf(PostStream, " %.16g %.16g %.16g ", x[i], y[i], z[i]);
        TableList.push_back(x[i]);
        TableList.push_back(y[i]);
        TableList.push_back(z[i]);
      }
    }
    for(k = 0; k < NbrHarmonics; k++) {
      for(i = 0; i < NbrNodes; i++) {
        for(j = 0; j < Size; j++) {
          if(Format != FORMAT_VALUE_ONLY) {
            fprintf(PostStream, " %.16g", Value[i].Val[MAX_DIM * k + j]);
            TableList.push_back(Value[i].Val[MAX_DIM * k + j]);
          }
          else {
            if(PSO_P->ValueName)
              fprintf(PostStream, " %s_%d_%d = %.16g", PSO_P->ValueName, j,
                      PSO_P->ValueIndex, Value[i].Val[MAX_DIM * k + j]);
            else
              fprintf(PostStream, " %.16g", Value[i].Val[MAX_DIM * k + j]);
            TableList.push_back(Value[i].Val[MAX_DIM * k + j]);
            if(j < Size - 1) fprintf(PostStream, "\n");
          }
        }
        fprintf(PostStream, " ");
      }
      fprintf(PostStream, " ");
    }
  }
  else {
    for(k = 0; k < HarmonicToTime; k++) {
      for(i = 0; i < NbrNodes; i++) {
        F_MHToTime0(k + i, &Value[i], &TmpValue, k, HarmonicToTime, &TimeMH);
        if(!i && Format == FORMAT_TIME_TABLE) {
          fprintf(PostStream, "%d %.16g ", TimeStep, TimeMH);
          TableList.push_back(TimeStep);
          TableList.push_back(TimeMH);
          for(i = 0; i < NbrNodes; i++) {
            fprintf(PostStream, " %.16g %.16g %.16g ", x[i], y[i], z[i]);
            TableList.push_back(x[i]);
            TableList.push_back(y[i]);
            TableList.push_back(z[i]);
          }
        }
        for(j = 0; j < Size; j++) {
          fprintf(PostStream, " %.16g", TmpValue.Val[j]);
          TableList.push_back(TmpValue.Val[j]);
        }
        fprintf(PostStream, " ");
      }
      if(Format == FORMAT_TIME_TABLE) fprintf(PostStream, "\n");
    }
  }

  if(TimeStep == NbrTimeSteps - 1 || Format == FORMAT_TIME_TABLE)
    fprintf(PostStream, "\n");
}

/* ------------------------------------------------------------------------ */
/*  NodeTable format                                                        */
/* ------------------------------------------------------------------------ */

// global static map for node table output (cannot be saved incrementally for
// each element)
static int NodeTable_StartNew = 0;
static std::map<int, std::vector<double> > NodeTable;
static std::map<int, std::vector<int> > ElementNodeTable;

static void NodeTable_PrintElement(int TimeStep, int NbTimeStep,
                                   int NbrHarmonics, struct PostElement *PE,
                                   bool StoreElementNodes = false)
{
  if(NodeTable_StartNew) {
    NodeTable_StartNew = 0;
    NodeTable.clear();
    ElementNodeTable.clear();
  }
  if(StoreElementNodes) {
    if(PE->Index >= 0 && PE->Index < Geo_GetNbrGeoElements()) {
      int numEle = Geo_GetGeoElement(PE->Index)->Num;
      ElementNodeTable[numEle].resize(PE->NbrNodes);
      for(int i = 0; i < PE->NbrNodes; i++)
        ElementNodeTable[numEle][i] = PE->NumNodes[i];
    }
  }
  for(int i = 0; i < PE->NbrNodes; i++) {
    int n = PE->NumNodes[i];
    int Size = 0;
    switch(PE->Value[0].Type) {
    case SCALAR: Size = 1; break;
    case VECTOR: Size = 3; break;
    case TENSOR_DIAG: Size = 3; break;
    case TENSOR_SYM: Size = 6; break;
    case TENSOR: Size = 9; break;
    }
    if(n > 0 && Size) { // we have data on an actual node
      NodeTable[n].resize(NbTimeStep * NbrHarmonics * Size, 0.);
      for(int k = 0; k < NbrHarmonics; k++) {
        for(int j = 0; j < Size; j++) {
          double val = PE->Value[i].Val[MAX_DIM * k + j];
          int idx = NbrHarmonics * Size * TimeStep + k * Size + j;
          NodeTable[n][idx] = val;
        }
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  ElementTable format */
/* ------------------------------------------------------------------------ */

// global static map for element table output (cannot be saved incrementally for
// each element)
static int ElementTable_StartNew = 0;
static std::map<int, std::vector<double> > ElementTable;

static void ElementTable_PrintElement(int TimeStep, int NbTimeStep,
                                      int NbrHarmonics, struct PostElement *PE)
{
  if(ElementTable_StartNew) {
    ElementTable_StartNew = 0;
    ElementTable.clear();
  }
  int numEle = -1;
  if(PE->NumElement || (PE->Index >= 0 && PE->Index < Geo_GetNbrGeoElements())) {
    numEle = PE->NumElement ? PE->NumElement : Geo_GetGeoElement(PE->Index)->Num;
    int Size = 0;
    switch(PE->Value[0].Type) {
    case SCALAR: Size = 1; break;
    case VECTOR: Size = 3; break;
    case TENSOR_DIAG: Size = 3; break;
    case TENSOR_SYM: Size = 6; break;
    case TENSOR: Size = 9; break;
    }
    if(Size) {
      ElementTable[numEle].resize(
        NbTimeStep * PE->NbrNodes * NbrHarmonics * Size, 0.);
      for(int i = 0; i < PE->NbrNodes; i++) {
        for(int k = 0; k < NbrHarmonics; k++) {
          for(int j = 0; j < Size; j++) {
            double val = PE->Value[i].Val[MAX_DIM * k + j];
            int idx = TimeStep * PE->NbrNodes * NbrHarmonics * Size +
                      i * NbrHarmonics * Size + k * Size + j;
            ElementTable[numEle][idx] = val;
          }
        }
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  S t o r e P o s t O p R e s u l t                                       */
/* ------------------------------------------------------------------------ */

static List_T *PostOpResults_L = NULL;

static void StorePostOpResult(int NbrHarmonics, struct PostElement *PE)
{
  int Size;
  double val;

  if(!PostOpResults_L)
    PostOpResults_L = List_Create(1000, 1000, sizeof(double));

  for(int i = 0; i < PE->NbrNodes; i++) {
    Size = 0;
    switch(PE->Value[0].Type) {
    case SCALAR: Size = 1; break;
    case VECTOR: Size = 3; break;
    case TENSOR_DIAG: Size = 3; break;
    case TENSOR_SYM: Size = 6; break;
    case TENSOR: Size = 9; break;
    }
    if(Size) { // we have data
      for(int k = 0; k < NbrHarmonics; k++) {
        for(int j = 0; j < Size; j++) {
          val = PE->Value[i].Val[MAX_DIM * k + j];
          List_Add(PostOpResults_L, &val);
        }
      }
    }
  }
}

static void StorePostOpResult(int NbrHarmonics, struct Value *Value)
{
  int Size;
  double val;

  if(!PostOpResults_L)
    PostOpResults_L = List_Create(1000, 1000, sizeof(double));

  Size = 0;
  switch(Value[0].Type) {
  case SCALAR: Size = 1; break;
  case VECTOR: Size = 3; break;
  case TENSOR_DIAG: Size = 3; break;
  case TENSOR_SYM: Size = 6; break;
  case TENSOR: Size = 9; break;
  }
  if(Size) { // we have data
    for(int k = 0; k < NbrHarmonics; k++) {
      for(int j = 0; j < Size; j++) {
        val = Value[0].Val[MAX_DIM * k + j];
        List_Add(PostOpResults_L, &val);
      }
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  UNV format                                                              */
/* ------------------------------------------------------------------------ */

#if !defined(HAVE_NX)
#define NX                                                                     \
  {                                                                            \
    Message::Error("UNV export not available in this version");                \
  }
#else
#define NX ;
#endif

double NXUnv_UnitFactor = 1;
int NXUnv_DatasetLocation =
  3; // 1: Data at nodes, 2: Data on elements, 3: Data at nodes on elements

void Unv_PrintHeader(FILE *PostStream, char *name, double Time, int TimeStep,
                     double &NXUnv_UnitFactor, int &NXUnv_DatasetLocation) NX
void Unv_PrintFooter(FILE *PostStream) NX
void Unv_PrintElement(FILE *PostStream, int Num_Element, int NbrNodes,
                      struct Value *Value, int NbrHarmonics,
                      int &NXUnv_DatasetLocation, double &NXUnv_UnitFactor) NX
void Unv_PrintNodeTable(FILE *PostStream,
                        std::map<int, std::vector<double> > &NodeTable,
                        double &NXUnv_UnitFactor) NX
void Unv_PrintRegion(FILE *PostStream, int Flag_Comma, int numRegion,
                     int NbrHarmonics, int Size, struct Value *Value,
                     double &NXUnv_UnitFactor) NX
#undef NX

/* ------------------------------------------------------------------------ */
/*  VTU (Unstructured VTK) format                                           */
/* ------------------------------------------------------------------------ */

static std::string VTU_FieldName;

void VTU_PrintHeader(FILE *PostStream, char *name, double Time, int TimeStep)
{
  fprintf(PostStream, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  fprintf(PostStream, "<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" "
          "byte_order=\"LittleEndian\">\n");
  fprintf(PostStream, "<UnstructuredGrid>\n");
  VTU_FieldName = name;
}

void VTU_PrintFooter(FILE *PostStream)
{
  fprintf(PostStream, "</UnstructuredGrid>\n");
  fprintf(PostStream, "</VTKFile>\n");
}

void VTU_PrintNodeTable(FILE *PostStream, bool multiComponent)
{
  if(NodeTable.empty()) {
    Message::Warning("Empty node data for VTU export");
    return;
  }

  fprintf(PostStream, "<Piece NumberOfPoints=\"%zu\" NumberOfCells=\"%zu\">\n",
          NodeTable.size(), ElementNodeTable.size());

  fprintf(PostStream, "<PointData>\n");

  fprintf(PostStream, "<DataArray type=\"Float64\" "
          "Name=\"Material_settings\" Format=\"ascii\">\n");
  // TODO
  for(std::size_t i = 0; i < NodeTable.size(); i++) {
    fprintf(PostStream, "0\n");
  }
  fprintf(PostStream, "</DataArray>\n");

  int numComp = NodeTable.begin()->second.size();
  if(multiComponent) {
    if(numComp > 1)
      fprintf(PostStream, "<DataArray type=\"Float64\" NumberOfComponents=\"%d\" "
              "Name=\"%s\" Format=\"ascii\">\n", numComp, VTU_FieldName.c_str());
    else
      fprintf(PostStream, "<DataArray type=\"Float64\" "
              "Name=\"%s\" Format=\"ascii\">\n", VTU_FieldName.c_str());
    for(auto &it : NodeTable) {
      for(std::size_t i = 0; i < it.second.size(); i++) {
        fprintf(PostStream, " %.16g", it.second[i]);
      }
      fprintf(PostStream, "\n");
    }
    fprintf(PostStream, "</DataArray>\n");
  }
  else{
    for(int comp = 0; comp < numComp; comp++) {
      fprintf(PostStream, "<DataArray type=\"Float64\" "
              "Name=\"%s,_%s_component\" Format=\"ascii\">\n",
              VTU_FieldName.c_str(), (comp == 0) ? "x" : (comp == 1) ? "y" : "z");
      for(auto &it : NodeTable) {
        fprintf(PostStream, "%.16g\n", it.second[comp]);
      }
      fprintf(PostStream, "</DataArray>\n");
    }
  }

  fprintf(PostStream, "</PointData>\n");

  //fprintf(PostStream, "<CellData>\n");
  //fprintf(PostStream, "</CellData>\n");

  fprintf(PostStream, "<Points>\n");

  fprintf(PostStream, "<DataArray type=\"Float64\" NumberOfComponents=\"3\" "
          "Format=\"ascii\">\n");
  std::map<int, int> nmap;
  int index = 0;
  for(auto &it : NodeTable) {
    int num = it.first;
    Geo_Node *n = Geo_GetGeoNodeOfNum(num);
    fprintf(PostStream, "%.16g %.16g %.16g\n", n->x, n->y, n->z);
    nmap[num] = index++;
  }
  fprintf(PostStream, "</DataArray>\n");

  fprintf(PostStream, "</Points>\n");

  fprintf(PostStream, "<Cells>\n");

  fprintf(PostStream, "<DataArray type=\"Int32\" Name=\"connectivity\" "
          "Format=\"ascii\">\n");
  std::vector<int> offsets, types;
  int o = 0;
  for(auto &it : ElementNodeTable) {
    int num = it.first;
    Geo_Element *e = Geo_GetGeoElementOfNum(num);
    switch(e->Type) {
    case POINT_ELEMENT: types.push_back(8); break;
    case LINE: types.push_back(3); break;
    case TRIANGLE: types.push_back(5); break;
    case QUADRANGLE: types.push_back(9); break;
    case TETRAHEDRON: types.push_back(10); break;
    case HEXAHEDRON: types.push_back(12); break;
    case PRISM: types.push_back(13); break;
    case PYRAMID: types.push_back(14); break;
    default: types.push_back(0); break;
    }
    o += e->NbrNodes;
    offsets.push_back(o);
    for(int i = 0; i < e->NbrNodes; i++)
      fprintf(PostStream, " %d", nmap[e->NumNodes[i]]);
    fprintf(PostStream, "\n");
  }
  fprintf(PostStream, "</DataArray>\n");
  fprintf(PostStream, "<DataArray type=\"Int32\" Name=\"offsets\" "
          "Format=\"ascii\">\n");
  for(auto o : offsets) fprintf(PostStream, "%d\n", o);
  fprintf(PostStream, "</DataArray>\n");
  fprintf(PostStream, "<DataArray type=\"UInt8\" Name=\"types\" "
          "Format=\"ascii\">\n");
  for(auto t : types) fprintf(PostStream, "%d\n", t);
  fprintf(PostStream, "</DataArray>\n");

  fprintf(PostStream, "</Cells>\n");
  fprintf(PostStream, "</Piece>\n");
}

/* ------------------------------------------------------------------------ */
/*  F o r m a t _ P o s t F o r m a t                                       */
/* ------------------------------------------------------------------------ */

// The mesh of a mesh-based view, as the blocks of the $Nodes and $Elements
// sections of MSH 4.2: one block for the elements of each entity and type, and
// one for the nodes of each entity. The entities (dimension, tag) are given in
// entities, with their physical groups: as one record (dimension, tag,
// physical group, x, y, z) for each physical group of each entity, x, y, z
// being the coordinates of a point. In parallel (GmshParallel) each rank gives
// the elements of its mesh partition and the nodes that it owns (it is the
// lowest rank of the elements that contain them: a rank has all the elements
// that contain the nodes of its own, see Geo_SendParts). nodeHeader and
// elementHeader get the headers of the sections, over all the ranks.
static void Gmsh_BlockHeader(std::string &text, int a, int b, int c,
                             std::size_t n)
{
  char str[100];
  if(Flag_BIN) {
    int abc[3] = {a, b, c};
    text.append((const char *)abc, sizeof(abc));
    text.append((const char *)&n, sizeof(n));
  }
  else
    text.append(str, snprintf(str, sizeof(str), "%d %d %d %zu\n", a, b, c, n));
}

static void Gmsh_Tag(std::string &text, std::size_t tag, const char *end)
{
  if(Flag_BIN)
    text.append((const char *)&tag, sizeof(tag));
  else
    text += std::to_string(tag) + end;
}

static void Gmsh_GetMesh(struct PostSubOperation *PSO_P, std::string &nodeText,
                         std::string &elementText, std::size_t nodeHeader[4],
                         std::size_t elementHeader[4],
                         std::vector<double> &entities)
{
  List_T *Region_L = NULL;
  if(PSO_P->SubType == PRINT_ONELEMENTSOF)
    Region_L = ((struct Group *)List_Pointer(Problem_S.Group,
                                             PSO_P->Case.OnRegion.RegionIndex))
                 ->InitialList;
  const int me = GmshParallel ? Message::GetCommRank() : 0;

  // With an order (ExportOrder) the elements are those of the Lagrange
  // elements of that order on the elements of the mesh, with new nodes (after
  // the nodes of the mesh) other than the vertices: the nodes on the edges and
  // the faces are shared by the elements (they are identified by the vertices
  // on which they depend, see LagrangePostElement). In parallel each rank
  // shares the new nodes among its elements, and numbers each one from the
  // number of the first of its elements that has it: the ranks need not
  // exchange anything for their numbers to differ (the numbers are sparse),
  // and only the nodes on the interfaces of the partitions are duplicated.
  const int order = PSO_P->ExportOrder;
  for(int i = 0; i < 4; i++) nodeHeader[i] = elementHeader[i] = 0;
  auto lagrange = [&](Geo_Element *e) {
    const struct LagrangePostElement *L = Get_LagrangePostElement(e, order);
    if(!L)
      Message::Error("No element of order %d for elements of type %s", order,
                     Get_StringForDefine(Element_Type, e->Type));
    return L;
  };

  // the elements of this rank by entity (dimension, tag) and type, and the
  // lowest rank of the elements that contain each node
  std::map<std::vector<int>, std::vector<Geo_Element *> > elements;
  std::map<int, int> lowest;
  for(int i = 0; i < Geo_GetNbrGeoElements(); i++) {
    Geo_Element *e = Geo_GetGeoElement(i);
    if(Region_L && !List_Search(Region_L, &e->Region, fcmp_int)) continue;
    int rank = !GmshParallel ? 0 : Geo_RankOfPartition(e->Partition);
    if(rank == me) {
      int type = GetDP2Gmsh(e->Type);
      if(order) {
        const struct LagrangePostElement *L = lagrange(e);
        if(!L) return;
        type = L->GmshType;
      }
      elements[{Geo_GetDimOfElement(e->Type), e->ElementaryRegion, type}]
        .push_back(e);
    }
    for(int j = 0; j < e->NbrNodes; j++) {
      auto it = lowest.insert({abs(e->NumNodes[j]), rank}).first;
      it->second = std::min(it->second, rank);
    }
  }

  // the first new node, and the numbers kept for the new nodes of each
  // element in parallel
  std::size_t newNode = 0;
  const std::size_t newPerElement =
    (std::size_t)(order + 1) * (order + 1) * (order + 1);
  if(order) {
    unsigned long long maxNode = 0;
    for(int i = 0; i < Geo_GetNbrGeoNodes(); i++)
      maxNode =
        std::max(maxNode, (unsigned long long)Geo_GetGeoNode(i)->Num);
#if defined(HAVE_MPI)
    if(GmshParallel)
      MPI_Allreduce(MPI_IN_PLACE, &maxNode, 1, MPI_UNSIGNED_LONG_LONG, MPI_MAX,
                    MPI_COMM_WORLD);
#endif
    newNode = maxNode + 1;
  }
  // the new nodes shared by the elements: the vertices on which they depend,
  // with their weights, sorted
  typedef std::array<int, 8> SharedNode;
  struct HashSharedNode {
    std::size_t operator()(const SharedNode &n) const
    {
      std::size_t h = 0;
      for(int i : n) h = h * 1000003u + (std::size_t)i + 0x9e3779b9u;
      return h;
    }
  };
  std::unordered_map<SharedNode, std::size_t, HashSharedNode> shared;

  // the elements, and the nodes of this rank, in the entity of the first of
  // its elements that has them (tags and coordinates), and the physical groups
  // of the entities
  std::map<std::vector<int>,
           std::pair<std::vector<std::size_t>, std::vector<double> > >
    nodes;
  std::set<int> done;
  std::set<std::vector<int> > physicals;
  std::size_t min[2] = {SIZE_MAX, SIZE_MAX}, max[2] = {0, 0};
  std::size_t numNodes = 0;
  std::vector<std::size_t> tags;
  std::vector<double> x, y, z;
  elementHeader[0] = elements.size();
  elementHeader[1] = 0;
  for(auto &b : elements) {
    auto &entityNodes = nodes[{b.first[0], b.first[1]}];
    auto addNode = [&](std::size_t tag, double X, double Y, double Z) {
      entityNodes.first.push_back(tag);
      entityNodes.second.insert(entityNodes.second.end(), {X, Y, Z});
      min[0] = std::min(min[0], tag);
      max[0] = std::max(max[0], tag);
      numNodes++;
    };
    Gmsh_BlockHeader(elementText, b.first[0], b.first[1], b.first[2],
                     b.second.size());
    for(auto e : b.second) {
      // (the elements made by GetDP, e.g. in a moving band, have none)
      if(e->Region > 0 &&
         physicals.insert({b.first[0], b.first[1], e->Region}).second) {
        Geo_Node *n = Geo_GetGeoNodeOfNum(abs(e->NumNodes[0]));
        double record[6] = {(double)b.first[0], (double)b.first[1],
                            (double)e->Region, n->x, n->y, n->z};
        entities.insert(entities.end(), record, record + 6);
      }
      const struct LagrangePostElement *L = order ? lagrange(e) : NULL;
      const int numVertices = L ? L->NbrVertices : e->NbrNodes;
      for(int j = 0; j < numVertices; j++) {
        int num = abs(e->NumNodes[j]);
        if(lowest[num] == me && done.insert(num).second) {
          Geo_Node *n = Geo_GetGeoNodeOfNum(num);
          addNode(num, n->x, n->y, n->z);
        }
      }
      tags.resize(L ? L->NbrNodes : e->NbrNodes);
      if(!L)
        for(int j = 0; j < e->NbrNodes; j++) tags[j] = abs(e->NumNodes[j]);
      else {
        x.resize(e->NbrNodes);
        y.resize(e->NbrNodes);
        z.resize(e->NbrNodes);
        Geo_GetNodesCoordinates(e->NbrNodes, e->NumNodes, x.data(), y.data(),
                                z.data());
        for(int k = 0; k < L->NbrNodes; k++) {
          const std::vector<std::pair<int, int> > &v = L->Vertices[k];
          if(v.size() == 1) {
            tags[k] = abs(e->NumNodes[v[0].first]);
            continue;
          }
          // (in parallel, from the number of the element, which no other
          // rank has)
          auto newTag = [&]() {
            return GmshParallel ?
                     newNode + (std::size_t)e->Num * newPerElement + k :
                     newNode++;
          };
          std::size_t *sharedTag = NULL;
          if(L->Dimension == 3 && (int)v.size() == L->NbrVertices)
            tags[k] = newTag();
          else {
            std::pair<int, int> s[4];
            for(std::size_t i = 0; i < v.size() && i < 4; i++)
              s[i] = {abs(e->NumNodes[v[i].first]), v[i].second};
            std::sort(s, s + std::min<std::size_t>(v.size(), 4));
            SharedNode key = {{0, 0, 0, 0, 0, 0, 0, 0}};
            for(std::size_t i = 0; i < v.size() && i < 4; i++) {
              key[2 * i] = s[i].first;
              key[2 * i + 1] = s[i].second;
            }
            auto it = shared.insert({key, 0});
            if(!it.second) {
              tags[k] = it.first->second;
              continue;
            }
            sharedTag = &it.first->second;
            tags[k] = newTag();
          }
          if(sharedTag) *sharedTag = tags[k];
          const double *n = &L->n[(std::size_t)k * e->NbrNodes];
          double X = 0., Y = 0., Z = 0.;
          for(int j = 0; j < e->NbrNodes; j++) {
            X += x[j] * n[j];
            Y += y[j] * n[j];
            Z += z[j] * n[j];
          }
          addNode(tags[k], X, Y, Z);
        }
      }
      Gmsh_Tag(elementText, e->Num, " ");
      for(std::size_t j = 0; j < tags.size(); j++)
        Gmsh_Tag(elementText, tags[j], (j == tags.size() - 1) ? "\n" : " ");
      min[1] = std::min(min[1], (std::size_t)e->Num);
      max[1] = std::max(max[1], (std::size_t)e->Num);
      elementHeader[1]++;
    }
  }

  nodeHeader[0] = 0;
  nodeHeader[1] = numNodes;
  for(auto &b : nodes) {
    if(b.second.first.empty()) continue;
    nodeHeader[0]++;
    Gmsh_BlockHeader(nodeText, b.first[0], b.first[1], 0,
                     b.second.first.size());
    for(std::size_t tag : b.second.first) Gmsh_Tag(nodeText, tag, "\n");
    if(Flag_BIN)
      nodeText.append((const char *)b.second.second.data(),
                      b.second.second.size() * sizeof(double));
    else
      for(std::size_t i = 0; i < b.second.second.size(); i += 3) {
        char str[100];
        nodeText.append(str, snprintf(str, sizeof(str), "%.16g %.16g %.16g\n",
                                      b.second.second[i],
                                      b.second.second[i + 1],
                                      b.second.second[i + 2]));
      }
  }
#if defined(HAVE_MPI)
  if(GmshParallel) {
    unsigned long long n[4] = {nodeHeader[0], nodeHeader[1], elementHeader[0],
                               elementHeader[1]};
    unsigned long long m[4] = {min[0], min[1], max[0], max[1]};
    MPI_Allreduce(MPI_IN_PLACE, n, 4, MPI_UNSIGNED_LONG_LONG, MPI_SUM,
                  MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, m, 2, MPI_UNSIGNED_LONG_LONG, MPI_MIN,
                  MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, m + 2, 2, MPI_UNSIGNED_LONG_LONG, MPI_MAX,
                  MPI_COMM_WORLD);
    nodeHeader[0] = n[0];
    nodeHeader[1] = n[1];
    elementHeader[0] = n[2];
    elementHeader[1] = n[3];
    min[0] = m[0];
    min[1] = m[1];
    max[0] = m[2];
    max[1] = m[3];
  }
#endif
  nodeHeader[2] = nodeHeader[1] ? min[0] : 0;
  nodeHeader[3] = max[0];
  elementHeader[2] = elementHeader[1] ? min[1] : 0;
  elementHeader[3] = max[1];
}

// Writes the $Entities section, from the records of Gmsh_GetMesh of all the
// ranks (a point is at the coordinates of its node, the other entities have
// no bounding box nor bounding entities)
static void Gmsh_WriteEntities(std::vector<double> &records)
{
#if defined(HAVE_MPI)
  if(GmshParallel) Message::Gather(records, false);
#endif
  if(!PostStream) return;
  // (dimension, tag) -> physical groups and coordinates
  std::map<std::pair<int, int>, std::pair<std::set<int>, std::vector<double> > >
    entities;
  for(std::size_t i = 0; i < records.size(); i += 6) {
    auto &e = entities[{(int)records[i], (int)records[i + 1]}];
    e.first.insert((int)records[i + 2]);
    e.second.assign(&records[i + 3], &records[i + 6]);
  }
  std::size_t num[4] = {0, 0, 0, 0};
  for(auto &e : entities) num[e.first.first]++;
  fprintf(PostStream, "$Entities\n");
  if(Flag_BIN)
    fwrite(num, sizeof(std::size_t), 4, PostStream);
  else
    fprintf(PostStream, "%zu %zu %zu %zu\n", num[0], num[1], num[2], num[3]);
  for(auto &e : entities) {
    int dim = e.first.first, tag = e.first.second;
    std::vector<int> phys(e.second.first.begin(), e.second.first.end());
    // a point: its coordinates; another entity: an empty box
    std::vector<double> box = e.second.second;
    if(dim) box.assign(6, 0.);
    std::size_t nphys = phys.size(), zero[2] = {0, 0};
    if(Flag_BIN) {
      fwrite(&tag, sizeof(int), 1, PostStream);
      fwrite(box.data(), sizeof(double), box.size(), PostStream);
      fwrite(&nphys, sizeof(std::size_t), 1, PostStream);
      fwrite(phys.data(), sizeof(int), nphys, PostStream);
      // no bounding entities, no embedded entities
      if(dim) fwrite(zero, sizeof(std::size_t), 2, PostStream);
    }
    else {
      fprintf(PostStream, "%d", tag);
      for(double x : box) fprintf(PostStream, " %.16g", x);
      fprintf(PostStream, " %zu", nphys);
      for(int p : phys) fprintf(PostStream, " %d", p);
      fprintf(PostStream, dim ? " 0 0\n" : "\n");
    }
  }
  fprintf(PostStream, Flag_BIN ? "\n$EndEntities\n" : "$EndEntities\n");
}

// Writes a section (e.g. "Nodes") with its header and the blocks of all the
// ranks
static void Gmsh_WriteSection(const char *name, const std::size_t header[4],
                              std::string &blocks)
{
  if(PostStream) {
    fprintf(PostStream, "$%s\n", name);
    if(Flag_BIN)
      fwrite(header, sizeof(std::size_t), 4, PostStream);
    else
      fprintf(PostStream, "%zu %zu %zu %zu\n", header[0], header[1], header[2],
              header[3]);
  }
  if(GmshParallel)
    Message::WriteParallelSections(PostStream, PostFileName, blocks);
  else if(PostStream && !blocks.empty())
    fwrite(&blocks[0], 1, blocks.size(), PostStream);
  if(PostStream) fprintf(PostStream, Flag_BIN ? "\n$End%s\n" : "$End%s\n", name);
}

void Format_PostFormat(struct PostSubOperation *PSO_P)
{
  // in parallel the mesh of a mesh-based view is written by all the ranks
  if((!PostStream && !GmshParallel) || PSO_P->Type == POP_EXPRESSION) return;

  int Format = PSO_P->Format;
  int NoMesh = PSO_P->NoMesh;

  switch(Format) {
  case FORMAT_GMSH:
    if((PSO_P->StoreInField >= 0 || PSO_P->StoreInMeshBasedField >= 0) &&
       !PSO_P->FileOut)
      break;
    if(Format_GmshMeshBased(PSO_P)) {
      if(PostStream) {
        fprintf(PostStream, "$MeshFormat\n");
        fprintf(PostStream, "4.2 %d %d\n", Flag_BIN, (int)sizeof(std::size_t));
        if(Flag_BIN) {
          int one = 1;
          fwrite(&one, sizeof(int), 1, PostStream);
          fprintf(PostStream, "\n");
        }
        fprintf(PostStream, "$EndMeshFormat\n");
      }
      if(!NoMesh) {
        std::string nodes, elements;
        std::size_t nodeHeader[4], elementHeader[4];
        std::vector<double> entities;
        Gmsh_GetMesh(PSO_P, nodes, elements, nodeHeader, elementHeader,
                     entities);
        Gmsh_WriteEntities(entities);
        Gmsh_WriteSection("Nodes", nodeHeader, nodes);
        Gmsh_WriteSection("Elements", elementHeader, elements);
      }
    }
    else if(PostStream && Flag_BIN) { /* bricolage */
      fprintf(PostStream, "$PostFormat /* Gmsh 1.2, %s */\n",
              Flag_BIN ? "binary" : "ascii");
      fprintf(PostStream, "1.2 %d %d\n", Flag_BIN, (int)sizeof(double));
      fprintf(PostStream, "$EndPostFormat\n");
    }
    break;
  case FORMAT_GNUPLOT:
    fprintf(PostStream, "# GetDP %s, %s\n", GETDP_VERSION,
            Flag_BIN ? "binary" : "ascii");
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  F o r m a t _ P o s t H e a d e r                                       */
/* ------------------------------------------------------------------------ */

void Format_PostHeader(struct PostSubOperation *PSO_P, int NbTimeStep,
                       int Order, char *Name1, char *Name2)
{
  int Format = PSO_P->Format;
  double Time = Current.Time;
  int TimeStep = Current.TimeStep;
  int Contour = PSO_P->Iso;
  int Type = PSO_P->CombinationType;

  char name[256];

  CurrentPartitionNumber = 0;

  if(Contour) {
    if(!PostElement_L)
      PostElement_L = List_Create(20, 20, sizeof(struct PostElement *));
    else
      List_Reset(PostElement_L);
  }

  if(Name1 && Name2) {
    strcpy(name, Order ? Name1 : Name2);
    strcat(name, Get_StringForDefine(PostSubOperation_CombinationType, Type));
    strcat(name, Order ? Name2 : Name1);
  }
  else if(Name1)
    strcpy(name, Name1);
  else if(Name2)
    strcpy(name, Name2);
  else
    strcpy(name, "unnamed");

  strcpy(CurrentName, name);

  switch(Format) {
  case FORMAT_GMSH_PARSED:
    if(PostStream) fprintf(PostStream, "View \"%s\" {\n", name);
    Gmsh_StartNewView = 1;
    break;
  case FORMAT_GMSH:
    Gmsh_StartNewView = 1;
    if((PSO_P->StoreInField >= 0 || PSO_P->StoreInMeshBasedField >= 0) &&
       !PSO_P->FileOut)
      break;
    if(PostStream && !Format_GmshMeshBased(PSO_P)) {
      if(Flag_BIN) { /* bricolage */
        fprintf(PostStream, "$View /* %s */\n", name);
        // old binary format does not support names with whitespace:
        for(int i = 0; i < strlen(name); i++) {
          if(name[i] == ' ') name[i] = '^';
        }
        fprintf(PostStream, "%s ", name);
      }
      else {
        fprintf(PostStream, "View \"%s\" {\n", name);
      }
    }
    break;
  case FORMAT_NXUNV:
    if(PostStream) {
      NodeTable_StartNew = 1;
      Unv_PrintHeader(PostStream, name, Time, TimeStep, NXUnv_UnitFactor,
                      NXUnv_DatasetLocation);
    }
    break;
  case FORMAT_NODE_VTU:
  case FORMAT_NODE_VTU_COMPONENT:
    if(PostStream) {
      NodeTable_StartNew = 1;
      VTU_PrintHeader(PostStream, name, Time, TimeStep);
    }
    break;
  case FORMAT_GNUPLOT:
    if(PostStream) {
      fprintf(PostStream, "# PostData '%s'\n", name);
      fprintf(PostStream, "# Type Num  X Y Z  N1 N2 N3  Values  <Values>...\n");
    }
    break;
  case FORMAT_NODE_TABLE: NodeTable_StartNew = 1; break;
  case FORMAT_ELEMENT_TABLE: ElementTable_StartNew = 1; break;
  case FORMAT_SPACE_TABLE:
  case FORMAT_TIME_TABLE:
  case FORMAT_SIMPLE_SPACE_TABLE:
  case FORMAT_VALUE_ONLY: TableList_StartNew = 1; break;
  case FORMAT_ADAPT:
    if(PostStream) fprintf(PostStream, "$Adapt /* %s */\n", name);
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  F o r m a t _ P o s t F o o t e r                                       */
/* ------------------------------------------------------------------------ */

static void pruneHarmonics(std::vector<double> &v, List_T *harmonics)
{
  std::vector<double> vh;
  std::vector<int> har;
  for(int i = 0; i < List_Nbr(harmonics); i++)
    har.push_back((int)*(double *)List_Pointer(harmonics, i));
  for(std::size_t i = 0; i < v.size(); i += Current.NbrHar) {
    for(std::size_t j = 0; j < har.size(); j++) {
      vh.push_back(v[i + har[j]]);
    }
  }
  v = vh;
}

void Format_PostFooter(struct PostSubOperation *PSO_P, int Store,
                       bool SendToServer)
{
  List_T *Iso_L[NBR_MAX_ISO], *Solutions_L;
  double IsoMin = 1.e200, IsoMax = -1.e200, IsoVal = 0.0, freq, valr, vali;
  int NbrIso = 0;
  int iPost, iNode, iIso, iTime, One = 1, i, j, NbTimeStep;
  char tmp[1024];
  bool PostOpSolutionGenerated;
  struct PostElement *PE;
  struct Solution *Solution_P = NULL, Solution_S;

  if(!(NbTimeStep = List_Nbr(PSO_P->TimeStep_L)))
    NbTimeStep = List_Nbr(Current.DofData->Solutions);

  if((PSO_P->Format == FORMAT_GMSH || PSO_P->Format == FORMAT_GMSH_PARSED) &&
     !Format_GmshMeshBased(PSO_P)) {
    switch(PSO_P->Legend) {
    case LEGEND_TIME:
      Gmsh_StringStart(PSO_P->Format, PSO_P->LegendPosition[0],
                       PSO_P->LegendPosition[1], PSO_P->LegendPosition[2]);
      for(i = 0; i < NbTimeStep; i++) {
        Pos_InitAllSolutions(PSO_P->TimeStep_L, i);
        valr = Current.DofData->CurrentSolution->Time;
        for(j = 0; j < Current.NbrHar; j++) {
          sprintf(tmp, "Step %d/%d: Time = %g", i + 1, NbTimeStep, valr);
          Gmsh_StringAdd(PSO_P->Format, (!i && !j), tmp);
        }
      }
      Gmsh_StringEnd(PSO_P->Format);
      break;

    case LEGEND_FREQUENCY:
      if(Current.NbrHar > 1) {
        Gmsh_StringStart(PSO_P->Format, PSO_P->LegendPosition[0],
                         PSO_P->LegendPosition[1], PSO_P->LegendPosition[2]);
        for(i = 0; i < NbTimeStep; i++) {
          Pos_InitAllSolutions(PSO_P->TimeStep_L, i);
          for(j = 0; j < Current.NbrHar; j += 2) {
            freq = 0.5 * Current.DofData->Val_Pulsation[j / 2] / M_PI;
            sprintf(tmp, "%g Hz (real part: COSINUS)", freq);
            Gmsh_StringAdd(PSO_P->Format, (!i && !j), tmp);
            sprintf(tmp, "%g Hz (imag part: -SINUS)", freq);
            Gmsh_StringAdd(PSO_P->Format, 0, tmp);
          }
        }
        Gmsh_StringEnd(PSO_P->Format);
      }
      break;

    case LEGEND_EIGENVALUES:
      Gmsh_StringStart(PSO_P->Format, PSO_P->LegendPosition[0],
                       PSO_P->LegendPosition[1], PSO_P->LegendPosition[2]);
      for(i = 0; i < NbTimeStep; i++) {
        Pos_InitAllSolutions(PSO_P->TimeStep_L, i);
        valr = Current.DofData->CurrentSolution->Time;
        vali = Current.DofData->CurrentSolution->TimeImag;
        if(Current.NbrHar == 1) {
          sprintf(tmp, "Eigenvalue %d/%d: %g", i + 1, NbTimeStep, valr);
          Gmsh_StringAdd(PSO_P->Format, !i, tmp);
        }
        else {
          for(j = 0; j < Current.NbrHar; j++) {
            if(!(j % 2)) {
              sprintf(tmp, "Eigenvalue %d/%d: %g %s i * %g (Real Part)", i + 1,
                      NbTimeStep, valr, (vali > 0) ? "+" : "-",
                      (vali > 0) ? vali : -vali);
              Gmsh_StringAdd(PSO_P->Format, (!i && !j), tmp);
            }
            else {
              sprintf(tmp, "Eigenvalue %d/%d: %g %s i * %g (Imaginary Part)",
                      i + 1, NbTimeStep, valr, (vali > 0) ? "+" : "-",
                      (vali > 0) ? vali : -vali);
              Gmsh_StringAdd(PSO_P->Format, 0, tmp);
            }
          }
        }
      }
      Gmsh_StringEnd(PSO_P->Format);
      break;
    }
  }

  if(PSO_P->Iso) {
    for(iPost = 0; iPost < List_Nbr(PostElement_L); iPost++) {
      PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);
      for(iNode = 0; iNode < PE->NbrNodes; iNode++) {
        IsoMin = std::min(IsoMin, PE->Value[iNode].Val[0]);
        IsoMax = std::max(IsoMax, PE->Value[iNode].Val[0]);
      }
    }

    if((NbrIso = PSO_P->Iso) < 0) NbrIso = List_Nbr(PSO_P->Iso_L);
    if(NbrIso > NBR_MAX_ISO) {
      Message::Error("Too many Iso values");
      NbrIso = NBR_MAX_ISO;
    }

    if(PostStream && PSO_P->Format == FORMAT_GNUPLOT)
      fprintf(PostStream, "# NbIso = %d, Min = %g, Max = %g\n", NbrIso, IsoMin,
              IsoMax);

    for(iIso = 0; iIso < NbrIso; iIso++)
      Iso_L[iIso] = List_Create(10, 10, sizeof(struct PostElement *));

    for(iPost = 0; iPost < List_Nbr(PostElement_L); iPost++) {
      PE = *(struct PostElement **)List_Pointer(PostElement_L, iPost);
      for(iIso = 0; iIso < NbrIso; iIso++) {
        if(PSO_P->Iso > 0) {
          Cal_Iso(PE, Iso_L[iIso],
                  IsoMin + iIso * (IsoMax - IsoMin) / (double)(NbrIso - 1),
                  IsoMin, IsoMax, PSO_P->DecomposeInSimplex);
        }
        else {
          List_Read(PSO_P->Iso_L, iIso, &IsoVal);
          Cal_Iso(PE, Iso_L[iIso], IsoVal, IsoMin, IsoMax,
                  PSO_P->DecomposeInSimplex);
        }
      }
      if(!Store) Destroy_PostElement(PE);
    }

    for(iIso = 0; iIso < NbrIso; iIso++) {
      for(iPost = 0; iPost < List_Nbr(Iso_L[iIso]); iPost++) {
        PE = *(struct PostElement **)List_Pointer(Iso_L[iIso], iPost);
        Format_PostElement(PSO_P, 0, 0, Current.Time, 0, 1, Current.NbrHar,
                           PSO_P->HarmonicToTime, NULL, PE);
        Destroy_PostElement(PE);
      }
      List_Delete(Iso_L[iIso]);
      if(PostStream && PSO_P->Format == FORMAT_GNUPLOT)
        fprintf(PostStream, "\n");
    }
  }

  switch(PSO_P->Format) {
  case FORMAT_GMSH_PARSED:
    if(PostStream && List_Nbr(TimeValue_L) > 1) {
      fprintf(PostStream, "TIME{");
      for(iTime = 0; iTime < List_Nbr(TimeValue_L); iTime++) {
        if(iTime) fprintf(PostStream, ",");
        fprintf(PostStream, "%.16g",
                *(double *)List_Pointer(TimeValue_L, iTime));
      }
      fprintf(PostStream, "};\n");
    }
    fprintf(PostStream, "};\n");
    break;
  case FORMAT_GMSH:
    if(Gmsh_StartNewView) Gmsh_ResetStaticLists(); // nothing to print!
    // (not while the file of a field stored by partition is written)
    if((PSO_P->StoreInField >= 0 || PSO_P->StoreInMeshBasedField >= 0) &&
       Pos_GetStoreFields()) {
#if defined(HAVE_GMSH)
      int field = (PSO_P->StoreInField >= 0) ? PSO_P->StoreInField :
                                               PSO_P->StoreInMeshBasedField;
      Message::Info("Storing data in field %d (%s)", field,
                    PSO_P->StoreInField >= 0 ? "list-based" : "mesh-based");
      int NS[GmshNumShapes * GmshNumKinds];
      std::vector<double> *LS[GmshNumShapes * GmshNumKinds];
      for(int shape = 0; shape < GmshNumShapes; shape++) {
        for(int kind = 0; kind < GmshNumKinds; kind++) {
          NS[GmshNumKinds * shape + kind] = GmshNb[shape][kind];
          LS[GmshNumKinds * shape + kind] = &GmshList[shape][kind];
        }
      }
      PViewData *data;
      if(PSO_P->StoreInField >= 0)
        data = new PViewDataList();
      else
        data = new PViewDataGModel(PViewDataGModel::ElementNodeData);
      data->importLists(NS, LS);
      new PView(data, field);
#else
      Message::Error(
        "GetDP must be compiled with Gmsh support to store data as field");
#endif
      if(!PSO_P->FileOut) break;
    }
    if(Format_GmshMeshBased(PSO_P)) {
      for(int kind = 0; kind < GmshNumKinds; kind++) {
        int N[GmshNumShapes];
        std::vector<double> *L[GmshNumShapes];
        for(int shape = 0; shape < GmshNumShapes; shape++) {
          N[shape] = GmshNb[shape][kind];
          L[shape] = &GmshList[shape][kind];
        }
        Gmsh_PrintElementNodeData(PSO_P, NbTimeStep, GmshNumComp[kind], N, L);
      }
      if(GmshParallel)
        Message::WriteParallelSections(PostStream, PostFileName, GmshBuffer);
    }
    else if(PostStream && Flag_BIN) { /* bricolage */
      fprintf(PostStream, "%d ", List_Nbr(TimeValue_L));
      for(int shape = 0; shape < GmshNumShapes; shape++)
        for(int kind = 0; kind < GmshNumKinds; kind++)
          fprintf(PostStream, "%d ", GmshNb[shape][kind]);
      fprintf(PostStream, "%d %d 0 0\n", NbT2, (int)T2C.size());
      fwrite(&One, sizeof(int), 1, PostStream);
      List_WriteToFile(TimeValue_L, PostStream, LIST_FORMAT_BINARY);
      bool f = true;
      for(int shape = 0; shape < GmshNumShapes; shape++)
        for(int kind = 0; kind < GmshNumKinds; kind++)
          dVecWrite(GmshList[shape][kind], PostStream, f);
      dVecWrite(T2D, PostStream, f);
      cVecWrite(T2C, PostStream, f);
      fprintf(PostStream, "\n");
      fprintf(PostStream, "$EndView\n");
    }
    else if(PostStream) {
      if(List_Nbr(TimeValue_L) > 1) {
        fprintf(PostStream, "TIME{");
        for(iTime = 0; iTime < List_Nbr(TimeValue_L); iTime++) {
          if(iTime) fprintf(PostStream, ",");
          fprintf(PostStream, "%.16g",
                  *(double *)List_Pointer(TimeValue_L, iTime));
        }
        fprintf(PostStream, "};\n");
      }
      fprintf(PostStream, "};\n");
    }
    break;
  case FORMAT_ADAPT:
    if(PostStream) fprintf(PostStream, "$EndAdapt\n");
    break;
  case FORMAT_NXUNV:
    if(PostStream) {
      if(NXUnv_DatasetLocation == 1) // Data at nodes
        Unv_PrintNodeTable(PostStream, NodeTable, NXUnv_UnitFactor);
      Unv_PrintFooter(PostStream);
    }
    break;
  case FORMAT_NODE_VTU:
  case FORMAT_NODE_VTU_COMPONENT:
    if(PostStream) {
      VTU_PrintNodeTable(PostStream, PSO_P->Format == FORMAT_NODE_VTU);
      VTU_PrintFooter(PostStream);
    }
    break;
  case FORMAT_NODE_TABLE:
    if(PostStream && NodeTable.size()) {
      fprintf(PostStream, "%d\n", (int)NodeTable.size());
      for(std::map<int, std::vector<double> >::iterator it = NodeTable.begin();
          it != NodeTable.end(); it++) {
        fprintf(PostStream, "%d", it->first);
        for(unsigned int i = 0; i < it->second.size(); i++)
          fprintf(PostStream, " %.16g", it->second[i]);
        fprintf(PostStream, "\n");
      }
    }
    {
      std::vector<double> exp;
      exp.push_back(NodeTable.size());
      for(std::map<int, std::vector<double> >::iterator it = NodeTable.begin();
          it != NodeTable.end(); it++) {
        exp.push_back(it->first);
        for(unsigned int i = 0; i < it->second.size(); i++)
          exp.push_back(it->second[i]);
      }
      GetDPNumbers[CurrentName] = exp;
      GetDPNumbersMap[CurrentName] = NodeTable;
      if(SendToServer && PSO_P->SendToServer &&
         strcmp(PSO_P->SendToServer, "No"))
        Message::AddOnelabNumberChoice(PSO_P->SendToServer, exp, PSO_P->Color,
                                       PSO_P->Units, PSO_P->Label,
                                       PSO_P->Visible, PSO_P->Closed);
    }
    break;
  case FORMAT_ELEMENT_TABLE:
    if(PostStream) {
      fprintf(PostStream, "%d\n", (int)ElementTable.size());
      for(std::map<int, std::vector<double> >::iterator it =
            ElementTable.begin();
          it != ElementTable.end(); it++) {
        fprintf(PostStream, "%d", it->first);
        for(unsigned int i = 0; i < it->second.size(); i++)
          fprintf(PostStream, " %.16g", it->second[i]);
        fprintf(PostStream, "\n");
      }
    }
    {
      std::vector<double> exp;
      exp.push_back(ElementTable.size());
      for(std::map<int, std::vector<double> >::iterator it =
            ElementTable.begin();
          it != ElementTable.end(); it++) {
        exp.push_back(it->first);
        for(unsigned int i = 0; i < it->second.size(); i++)
          exp.push_back(it->second[i]);
      }
      GetDPNumbers[CurrentName] = exp;
      GetDPNumbersMap[CurrentName] = ElementTable;
      if(SendToServer && PSO_P->SendToServer &&
         strcmp(PSO_P->SendToServer, "No"))
        Message::AddOnelabNumberChoice(PSO_P->SendToServer, exp, PSO_P->Color,
                                       PSO_P->Units, PSO_P->Label,
                                       PSO_P->Visible, PSO_P->Closed);
    }
    break;
  case FORMAT_SPACE_TABLE:
  case FORMAT_TIME_TABLE:
  case FORMAT_SIMPLE_SPACE_TABLE:
  case FORMAT_VALUE_ONLY: {
    if(TableList.size()) {
      // (moved, not copied: a table on the elements can be large)
      std::vector<double> &numbers = GetDPNumbers[CurrentName];
      numbers.swap(TableList);
      std::vector<double>().swap(TableList);
      if(SendToServer && PSO_P->SendToServer &&
         strcmp(PSO_P->SendToServer, "No")) {
        std::vector<double> v(numbers);
        if(PSO_P->Format == FORMAT_VALUE_ONLY && PSO_P->SendToServerList)
          pruneHarmonics(v, PSO_P->SendToServerList);
        Message::AddOnelabNumberChoice(PSO_P->SendToServer, v, PSO_P->Color,
                                       PSO_P->Units, PSO_P->Label,
                                       PSO_P->Visible, PSO_P->Closed);
      }
    }
  } break;
  case FORMAT_LOOP_ERROR:
    Solutions_L = ((struct PostOpSolutions *)List_Pointer(
                     Current.PostOpData_L, Current.PostOpDataIndex))
                    ->Solutions_L;
    PostOpSolutionGenerated = false;
    if(List_Nbr(Solutions_L) > 0) {
      Solution_P =
        (struct Solution *)List_Pointer(Solutions_L, List_Nbr(Solutions_L) - 1);
      PostOpSolutionGenerated = (Solution_P->TimeStep == (int)Current.TimeStep);
    }
    if(!PostOpSolutionGenerated) {
      Solution_S.Time = Current.Time;
      Solution_S.TimeImag = Current.TimeImag;
      Solution_S.TimeStep = Current.TimeStep;
      Solution_S.SolutionExist = 1;
      Solution_S.TimeFunctionValues = NULL;
      LinAlg_CreateVector(&Solution_S.x, &Current.DofData->Solver,
                          List_Nbr(PostOpResults_L), true); // sequential!
      for(int i = 0; i < List_Nbr(PostOpResults_L); i++) {
        List_Read(PostOpResults_L, i, &valr);
        LinAlg_SetDoubleInVector(valr, &Solution_S.x, i);
      }
      LinAlg_AssembleVector(&Solution_S.x);
      List_Add(Solutions_L, &Solution_S);
    }
    else {
      for(int i = 0; i < List_Nbr(PostOpResults_L); i++) {
        List_Read(PostOpResults_L, i, &valr);
        LinAlg_SetDoubleInVector(valr, &Solution_P->x, i);
      }
      LinAlg_AssembleVector(&Solution_P->x);
    }
    List_Delete(PostOpResults_L);
    PostOpResults_L = NULL;
    break;
  }
}

/* ------------------------------------------------------------------------ */
/*  F o r m a t _ P o s t E l e m e n t                                     */
/* ------------------------------------------------------------------------ */

void Format_PostElement(struct PostSubOperation *PSO_P, int Contour, int Store,
                        double Time, int TimeStep, int NbTimeStep,
                        int NbrHarmonics, int HarmonicToTime, double *Dummy,
                        struct PostElement *PE)
{
  int i, j, k, l, Num_Element;
  struct PostElement *PE2;
  struct Value Value;

  static int Warning_FirstHarmonic = 0;

  /* TODO

  static int  Size = 0 ;

  int flag_storeAllTimeResults, indexInTmpValues;
  static struct Value  TmpValue, *TmpValues ;
  static double *Times ;
  struct Value *FourierValues;

  flag_storeAllTimeResults = PSO_P->TimeToHarmonic ;
  indexInTmpValues = flag_storeAllTimeResults? iTime * NbrRegion : 0 ;

  if(1){
    switch(PE->Value[0].Type){
    case SCALAR      : Size = 1 ; break ;
    case VECTOR      : Size = 3 ; break ;
    case TENSOR_DIAG : Size = 3 ; break ;
    case TENSOR_SYM  : Size = 6 ; break ;
    case TENSOR      : Size = 9 ; break ;
    default          : Size = 9 ; break ;
    }
  }
  */

  if(PE->NumElement)
    Num_Element = PE->NumElement;
  else if(PE->Index != NO_ELEMENT)
    Num_Element = Geo_GetGeoElement(PE->Index)->Num;
  else
    Num_Element = 0;

  if(Contour) {
    if(PE->Value[0].Type != SCALAR) {
      Message::Error("Non scalar Element %d in contour creation", Num_Element);
      return;
    }
    if(NbTimeStep != 1) {
      Message::Error("Contour creation not allowed for multiple time steps");
      return;
    }
    if(Current.NbrHar != 1 && !Warning_FirstHarmonic) {
      Message::Warning(
        "Contour creation done only for first harmonic (use Re[] or Im[])");
      Warning_FirstHarmonic = 1;
    }
    if(Store)
      List_Add(PostElement_L, &PE);
    else {
      PE2 = PartialCopy_PostElement(PE);
      List_Add(PostElement_L, &PE2);
    }
    return;
  }

  struct Element *old = Current.Element;
  Current.Element = nullptr; // to force expression evaluator to use Current.{x,y,z}

  if(PSO_P->ChangeOfCoordinates[0] >= 0) {
    for(i = 0; i < PE->NbrNodes; i++) {
      Current.x = PE->x[i];
      Current.y = PE->y[i];
      Current.z = PE->z[i];
      for(j = 0; j < 9; j++) Current.Val[j] = PE->Value[i].Val[j];
      Get_ValueOfExpressionByIndex(PSO_P->ChangeOfCoordinates[0], NULL, 0., 0.,
                                   0., &Value);
      PE->x[i] = Value.Val[0];
      Get_ValueOfExpressionByIndex(PSO_P->ChangeOfCoordinates[1], NULL, 0., 0.,
                                   0., &Value);
      PE->y[i] = Value.Val[0];
      Get_ValueOfExpressionByIndex(PSO_P->ChangeOfCoordinates[2], NULL, 0., 0.,
                                   0., &Value);
      PE->z[i] = Value.Val[0];
    }
  }

  if(PSO_P->ChangeOfValues && List_Nbr(PSO_P->ChangeOfValues) > 0) {
    for(i = 0; i < PE->NbrNodes; i++) {
      Current.x = PE->x[i];
      Current.y = PE->y[i];
      Current.z = PE->z[i];
      for(k = 0; k < Current.NbrHar; k++) {
        for(j = 0; j < 9; j++)
          Current.Val[j] = PE->Value[i].Val[MAX_DIM * k + j];
        for(l = 0; l < List_Nbr(PSO_P->ChangeOfValues); l++) {
          Get_ValueOfExpressionByIndex(
            *(int *)List_Pointer(PSO_P->ChangeOfValues, l), NULL, 0., 0., 0.,
            &Value);
          PE->Value[i].Val[MAX_DIM * k + l] = Value.Val[0];
        }
      }
    }
  }

  Current.Element = old;

  switch(PSO_P->Format) {
  case FORMAT_GMSH_PARSED:
    GmshParsed_PrintElement(Time, TimeStep, NbTimeStep, NbrHarmonics,
                            HarmonicToTime, PE->Type, PE->NbrNodes, PE->x,
                            PE->y, PE->z, PE->Value);
    break;
  case FORMAT_NXUNV:
    if(PostStream) {
      if(NXUnv_DatasetLocation == 1) // Data at nodes
        NodeTable_PrintElement(TimeStep, NbTimeStep, NbrHarmonics, PE);
      else if(NXUnv_DatasetLocation == 2 ||
              NXUnv_DatasetLocation ==
                3) // Data at elements or nodes on elements
        Unv_PrintElement(PostStream, Num_Element, PE->NbrNodes, PE->Value,
                         NbrHarmonics, NXUnv_DatasetLocation, NXUnv_UnitFactor);
    }
    break;
  case FORMAT_NODE_VTU:
  case FORMAT_NODE_VTU_COMPONENT:
    if(PostStream) {
      NodeTable_PrintElement(TimeStep, NbTimeStep, NbrHarmonics, PE, true);
    }
    break;
  case FORMAT_GMSH:
    if(PSO_P->StoreInField >= 0 || PSO_P->StoreInMeshBasedField >= 0) {
      Gmsh_PrintElement(Time, TimeStep, NbTimeStep, NbrHarmonics,
                        HarmonicToTime, PE->Type, Num_Element, PE->NbrNodes,
                        PE->x, PE->y, PE->z, PE->Value, PSO_P, Store);
      if(!PSO_P->FileOut || Format_GmshMeshBased(PSO_P) || Flag_BIN) break;
    }
    if(Format_GmshMeshBased(PSO_P) || Flag_BIN) { /* bricolage */
      Gmsh_PrintElement(Time, TimeStep, NbTimeStep, NbrHarmonics,
                        HarmonicToTime, PE->Type, Num_Element, PE->NbrNodes,
                        PE->x, PE->y, PE->z, PE->Value, PSO_P, Store);
    }
    else {
      GmshParsed_PrintElement(Time, TimeStep, NbTimeStep, NbrHarmonics,
                              HarmonicToTime, PE->Type, PE->NbrNodes, PE->x,
                              PE->y, PE->z, PE->Value);
    }
    break;
  case FORMAT_GNUPLOT:
    Gnuplot_PrintElement(PSO_P->Format, Time, TimeStep, NbTimeStep,
                         NbrHarmonics, HarmonicToTime, PE->Type, Num_Element,
                         PE->NbrNodes, PE->x, PE->y, PE->z, Dummy, PE->Value);
    break;
  case FORMAT_SPACE_TABLE:
  case FORMAT_TIME_TABLE:
  case FORMAT_SIMPLE_SPACE_TABLE:
  case FORMAT_VALUE_ONLY:
    Tabular_PrintElement(PSO_P, PSO_P->Format, Time, TimeStep, NbTimeStep,
                         NbrHarmonics, HarmonicToTime, PE->Type, Num_Element,
                         PE->NbrNodes, PE->x, PE->y, PE->z, Dummy, PE->Value);
    break;
  case FORMAT_NODE_TABLE:
    NodeTable_PrintElement(TimeStep, NbTimeStep, NbrHarmonics, PE);
    break;
  case FORMAT_ELEMENT_TABLE:
    ElementTable_PrintElement(TimeStep, NbTimeStep, NbrHarmonics, PE);
    break;
  case FORMAT_LOOP_ERROR: StorePostOpResult(NbrHarmonics, PE); break;
  case FORMAT_ADAPT:
    if(PostStream) {
      if(Dummy[4]) fprintf(PostStream, "%d\n", (int)Dummy[4]);
      fprintf(PostStream, "%d %g %g %g\n", (int)Dummy[0], Dummy[1], Dummy[2],
              Dummy[3]);
    }
    break;
  default: Message::Error("Unknown format in Format_PostElement");
  }

  if(PE->NbrNodes == 1 &&
     PSO_P->Format != FORMAT_NODE_TABLE &&
     PSO_P->Format != FORMAT_ELEMENT_TABLE &&
     PSO_P->Format != FORMAT_SPACE_TABLE &&
     PSO_P->Format != FORMAT_TIME_TABLE &&
     PSO_P->Format != FORMAT_SIMPLE_SPACE_TABLE &&
     PSO_P->Format != FORMAT_VALUE_ONLY) {
    if(PSO_P->SendToServer && strcmp(PSO_P->SendToServer, "No")) {
      std::vector<double> v;
      Export_Value(&PE->Value[0], v, PSO_P->SendToServerList);
      Message::AddOnelabNumberChoice(PSO_P->SendToServer, v, PSO_P->Color,
                                     PSO_P->Units, PSO_P->Label, PSO_P->Visible,
                                     PSO_P->Closed);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  F o r m a t _ P o s t V a l u e                                         */
/* ------------------------------------------------------------------------ */

void Format_PostValue(struct PostQuantity *PQ_P, struct PostSubOperation *PSO_P,
                      int Format, char *Comma, int Group_FunctionType,
                      int iTime, double Time, int NbrTimeStep, int iRegion,
                      int numRegion, int NbrRegion, int NbrHarmonics,
                      int HarmonicToTime, int FourierTransform,
                      int Flag_NoNewLine, struct Value *Value)
{
  static int Size;
  int j, k;
  double TimeMH, Freq;
  double x, y, z;
  int flag_storeAllTimeResults, indexInTmpValues;
  static struct Value TmpValue, *TmpValues;
  static double *Times;

  flag_storeAllTimeResults = FourierTransform || PSO_P->TimeToHarmonic;
  indexInTmpValues = flag_storeAllTimeResults ? iTime * NbrRegion : 0;

  if(iRegion == 0) {
    switch(Value->Type) {
    case SCALAR: Size = 1; break;
    case VECTOR: Size = 3; break;
    case TENSOR_DIAG: Size = 3; break;
    case TENSOR_SYM: Size = 6; break;
    case TENSOR: Size = 9; break;
    }
  }

  if(Format == FORMAT_REGION_TABLE) {
    if(iRegion == 0) {
      if(PostStream == stdout || PostStream == stderr) {
        Message::Direct("%d%s", NbrRegion, Comma ? Comma : "");
      }
      else if(PostStream) {
        fprintf(PostStream, "%d%s\n", NbrRegion, Comma ? Comma : "");
      }
    }
    std::ostringstream sstream;
    sstream.precision(16);
    sstream << numRegion;
    for(k = 0; k < NbrHarmonics; k++) {
      for(j = 0; j < Size; j++) {
        sstream << " " << Value->Val[MAX_DIM * k + j];
        if(Comma) sstream << Comma;
      }
    }
    if(PostStream == stdout || PostStream == stderr)
      Message::Direct(sstream.str().c_str());
    else if(PostStream)
      fprintf(PostStream, "%s\n", sstream.str().c_str());
  }
  else if(Format == FORMAT_GETDP) {
    std::ostringstream sstream;
    sstream.precision(16);
    sstream << (PSO_P->ValueName ? PSO_P->ValueName : PQ_P->Name);
    if(numRegion != NO_REGION) sstream << "~{" << numRegion << "}";

    if(NbrHarmonics >= 2 || Size > 1)
      sstream << "() = {";
    else
      sstream << " =";

    for(k = 0; k < NbrHarmonics; k++) {
      for(j = 0; j < Size; j++) {
        if(k || j) sstream << ",";
        sstream << " " << Value->Val[MAX_DIM * k + j];
      }
    }

    if(NbrHarmonics >= 2 || Size > 1) sstream << " };";

    if(PostStream == stdout || PostStream == stderr)
      Message::Direct(sstream.str().c_str());
    else if(PostStream)
      fprintf(PostStream, "%s\n", sstream.str().c_str());
  }
  else if(Format == FORMAT_GMSH && !Format_GmshMeshBased(PSO_P)) {
    if(Group_FunctionType == NODESOF)
      Geo_GetNodesCoordinates(1, &numRegion, &x, &y, &z);
    else {
      x = y = z = 0.;
      Message::Warning(
        "Post Format \'Gmsh\' not adapted for global quantities supported"
        " by Regions. Zero coordinates are considered.");
    }
    GmshParsed_PrintElement(Time, 0, 1, NbrHarmonics, HarmonicToTime,
                            POINT_ELEMENT, 1, &x, &y, &z, Value);
  }
  else if(Format == FORMAT_NXUNV) {
    if(PostStream)
      Unv_PrintRegion(PostStream, Comma ? 1 : 0, numRegion, NbrHarmonics, Size,
                      Value, NXUnv_UnitFactor);
  }
  else if(Format == FORMAT_LOOP_ERROR) {
    StorePostOpResult(NbrHarmonics, Value);
  }
  else if(Format == FORMAT_NODE_TABLE) {
    // FIXME: this leads to output files without the total number of nodes at
    // the beginning (i.e. not compatible with NodeTable obtained e.g. for
    // OnElementsOf)
    fprintf(PostStream, "%d", numRegion);
    Geo_GetNodesCoordinates(1, &numRegion, &x, &y, &z);
    fprintf(PostStream, " %.16g %.16g %.16g", x, y, z);
    for(k = 0; k < NbrHarmonics; k++) {
      for(j = 0; j < Size; j++) {
        fprintf(PostStream, " %.16g", Value->Val[MAX_DIM * k + j]);
      }
    }
    fprintf(PostStream, "\n");
  }
  // else, for other FORMATs, e.g., FORMAT_FREQUENCY_TABLE
  else {
    if(iRegion == 0) {
      if(!flag_storeAllTimeResults)
        TmpValues = (struct Value *)Malloc(NbrRegion * sizeof(struct Value));
      else {
        if(iTime == 0) {
          TmpValues = (struct Value *)Malloc(NbrTimeStep * NbrRegion *
                                             sizeof(struct Value));
          Times = (double *)Malloc(NbrTimeStep * sizeof(double));
        }
        Times[iTime] = Time;
      }
    }

    Cal_CopyValue(Value, &TmpValues[indexInTmpValues + iRegion]);

    if(!flag_storeAllTimeResults && iRegion == NbrRegion - 1) {
      if(PostStream && HarmonicToTime == 1) {
        switch(Format) {
        case FORMAT_FREQUENCY_TABLE:
        case FORMAT_FREQUENCY_REGION_VALUE:
          if(NbrHarmonics == 1) {
            Message::Error(
              "FrequencyTable format not allowed (only one harmonic)");
            return;
          }
          break;
        case FORMAT_VALUE_ONLY: break;
        default:
          fprintf(PostStream, " %.16g", Time);
          if(Comma) fprintf(PostStream, "%s", Comma);
          break;
        }
        for(iRegion = 0; iRegion < NbrRegion; iRegion++) {
          for(k = 0; k < NbrHarmonics; k++) {
            if((Format == FORMAT_FREQUENCY_TABLE ||
                Format == FORMAT_FREQUENCY_REGION_VALUE) &&
               !(k % 2) && iRegion == 0) {
              Freq = Current.DofData->Val_Pulsation[0] / TWO_PI;
              fprintf(PostStream, " %.16g", Freq);
              if(Comma) fprintf(PostStream, "%s", Comma);
            }
            for(j = 0; j < Size; j++) {
              if(Format != FORMAT_REGION_VALUE &&
                 Format != FORMAT_FREQUENCY_REGION_VALUE) {
                fprintf(
                  PostStream, " %.16g",
                  TmpValues[indexInTmpValues + iRegion].Val[MAX_DIM * k + j]);
                if(Comma) fprintf(PostStream, "%s", Comma);
              }
            }
          }
        }
        if(Flag_NoNewLine || Format == FORMAT_REGION_VALUE ||
           Format == FORMAT_FREQUENCY_REGION_VALUE)
          fprintf(PostStream, " ");
        else
          fprintf(PostStream, "\n");
      }
      else if(PostStream) {
        for(k = 0; k < HarmonicToTime; k++) {
          for(iRegion = 0; iRegion < NbrRegion; iRegion++) {
            F_MHToTime0(k + iRegion, &TmpValues[indexInTmpValues + iRegion],
                        &TmpValue, k, HarmonicToTime, &TimeMH);
            if(iRegion == 0) {
              fprintf(PostStream, " %.16g", TimeMH);
              if(Comma) fprintf(PostStream, "%s", Comma);
            }
            for(j = 0; j < Size; j++) {
              fprintf(PostStream, " %.16g", TmpValue.Val[j]);
              if(Comma) fprintf(PostStream, "%s", Comma);
            }
          }
          fprintf(PostStream, "\n");
        }
      }

      if(flag_storeAllTimeResults) Free(Times);
      Free(TmpValues);
    }

    else if(flag_storeAllTimeResults && iTime == NbrTimeStep - 1 &&
            iRegion == NbrRegion - 1) {
      Pos_FourierTransform(NbrTimeStep, NbrRegion, Times, TmpValues, Size, 1,
                           PSO_P->TimeToHarmonic, NULL, NULL, NULL);

      Free(Times);
      Free(TmpValues);
    }
  }
}

/* ------------------------------------------------------------------------ */
/*  P o s _ F o u r i e r T r a n s f o r m                                 */
/* ------------------------------------------------------------------------ */

void Pos_FourierTransform(int NbrTimeStep, int NbrRegion, double *Times,
                          struct Value *TmpValues, int Size, int TypeOutput,
                          int Nb_Freq_Select_0, int *NbrFreq,
                          double **Frequencies, struct Value **OutValues)
{
#if NEW_CODE
  *NbrFreq = (NbrTimeStep - 1) / 2 + 1;
  *Frequencies = (double *)Malloc(*NbrFreq * sizeof(double));
  *OutValues = (struct Value *)Malloc(*NbrFreq * sizeof(struct Value));

  int nfft = *NbrFreq;
  kissfft<double> fft(nfft, false);
  std::vector<std::complex<double> > inbuf(nfft);
  std::vector<std::complex<double> > outbuf(nfft);
  for(int k = 0; k < nfft; ++k)
    inbuf[k] = std::complex<double>(rand() / (double)RAND_MAX - .5,
                                    rand() / (double)RAND_MAX - .5);
  fft.transform(&inbuf[0], &outbuf[0]);
#else

  int iTime, iRegion, k_fc, i_k, j, k;
  int N, Nhalf, NbrFourierComps;
  double *val_FourierComps;
  double val, val_r, val_i, norm, Period, w, v_cos, v_sin;

  N = NbrTimeStep - 1;
  Nhalf = N / 2;
  //  Nhalf = 2;
  NbrFourierComps = Nhalf * 2;

  Period = Times[NbrTimeStep - 1] - Times[0];
  w = TWO_PI / Period;

  val_FourierComps =
    (double *)Malloc(NbrFourierComps * MAX_DIM * 2 * sizeof(double));

  for(k_fc = -Nhalf; k_fc < Nhalf; k_fc++) {
    i_k = Nhalf + k_fc;
    for(k = 0; k < 2; k++) {
      for(j = 0; j < Size; j++) {
        val_FourierComps[(2 * i_k + k) * MAX_DIM + j] = 0.;
      }
    }
  }

  for(iTime = 0; iTime < N; iTime++) {
    iRegion = 0; // only for 1 region now!

    for(k_fc = -Nhalf; k_fc < Nhalf; k_fc++) {
      i_k = Nhalf + k_fc;

      v_cos = cos(k_fc * w * Times[iTime]);
      v_sin = sin(-k_fc * w * Times[iTime]);

      for(j = 0; j < Size; j++) {
        val = TmpValues[iTime * NbrRegion + iRegion].Val[j];

        val_FourierComps[(2 * i_k + 0) * MAX_DIM + j] += val * v_cos;
        val_FourierComps[(2 * i_k + 1) * MAX_DIM + j] += val * v_sin;
      }
    }
  }

  for(k_fc = -Nhalf; k_fc < Nhalf; k_fc++) {
    i_k = Nhalf + k_fc;
    for(k = 0; k < 2; k++) {
      for(j = 0; j < Size; j++) {
        val_FourierComps[(2 * i_k + k) * MAX_DIM + j] /= N;
      }
    }
  }

  if(!PostStream) TypeOutput = 2;

  int Nb_Freq_Select = (Nb_Freq_Select_0 > 0) ? Nb_Freq_Select_0 : Nhalf - 1;

  // Limited to cosine transform now
  if(TypeOutput == 2) {
    *NbrFreq = Nhalf + 1;
    *Frequencies = (double *)Malloc(*NbrFreq * sizeof(double));
    *OutValues = (struct Value *)Malloc(*NbrFreq * sizeof(struct Value));
  }

  //  for (k_fc=-Nhalf; k_fc<Nhalf; k_fc++){
  for(k_fc = 0; k_fc <= Nb_Freq_Select; k_fc++) {
    i_k = Nhalf + k_fc;

    if(TypeOutput == 1) { fprintf(PostStream, "%.16g", k_fc * w / TWO_PI); }
    else if(TypeOutput == 2) {
      (*Frequencies)[k_fc] = k_fc * w / TWO_PI;
      (*OutValues)[k_fc].Type = TmpValues[0].Type;
    }

    for(k = 0; k < 2; k++) {
      for(j = 0; j < Size; j++) {
        /*
        if (k_fc != 0)
          val = ((k==0)?1:-1) * 2 * val_FourierComps[(2*i_k+k)*MAX_DIM + j];
        else
          val = val_FourierComps[(2*i_k+k)*MAX_DIM + j];
        */

        if(k_fc != 0) {
          val_r = 2 * val_FourierComps[(2 * i_k + 0) * MAX_DIM + j];
          val_i = -2 * val_FourierComps[(2 * i_k + 1) * MAX_DIM + j];
          norm = sqrt(SQU(val_r) + SQU(val_i));
          // val = (k==0)? norm : -asin(val_i/norm); // Phase for
          // CosineTransform
          val =
            (k == 0) ? norm : atan2(val_i, val_r); // Phase for FourierTransform
        }
        else {
          val = (k == 0) ? val_FourierComps[(2 * i_k + k) * MAX_DIM + j] : 0.;
        }

        if(TypeOutput == 1) { fprintf(PostStream, " %.16g", val); }
        if(k == 0 && TypeOutput == 2) { (*OutValues)[k_fc].Val[j] = val; }
      }
    }
    if(TypeOutput == 1) { fprintf(PostStream, "\n"); }
  }

  Free(val_FourierComps);
#endif
}
