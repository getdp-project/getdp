// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef POS_FORMAT_H
#define POS_FORMAT_H

#include <vector>
#include "GetDPConfig.h"
#include "ProData.h"
#include "ListUtils.h"
#include "Message.h"

#if defined(HAVE_MPI)
#include <mpi.h>
// replaces a vector by the vectors of all the ranks on the first rank (in the
// order of the ranks), and by nothing on the others
template <class T>
inline void GatherOnFirstRank(std::vector<T> &v, MPI_Datatype type)
{
  int np = Message::GetCommSize(), n = (int)v.size(), total = 0;
  std::vector<int> counts(np), displs(np, 0);
  MPI_Gather(&n, 1, MPI_INT, &counts[0], 1, MPI_INT, 0, MPI_COMM_WORLD);
  if(!Message::GetCommRank())
    for(int i = 0; i < np; i++) {
      displs[i] = total;
      total += counts[i];
    }
  std::vector<T> all(total);
  MPI_Gatherv(v.data(), n, type, all.data(), &counts[0], &displs[0], type, 0,
              MPI_COMM_WORLD);
  v.swap(all);
}
#endif

bool Format_GmshMeshBased(struct PostSubOperation *PSO_P);
bool Format_CanGmshParallel(struct PostSubOperation *PSO_P);
void Format_SetGmshParallel(bool parallel);
bool Format_GetGmshParallel();
void Format_PostFormat(struct PostSubOperation *PSO_P);

void Format_PostHeader(struct PostSubOperation *PSO_P, int NbTimeStep,
                       int Order, char *Name1, char *Name2);

void Format_PostFooter(struct PostSubOperation *PSO_P, int Store,
                       bool SendToServer = true);

void Format_PostElement(struct PostSubOperation *PSO_P, int Contour, int Store,
                        double Time, int TimeStep, int NbTimeStep,
                        int NbrHarmonics, int HarmonicToTime, double *Dummy,
                        struct PostElement *PE);

void Format_PostValue(struct PostQuantity *PQ_P, struct PostSubOperation *PSO_P,
                      int Format, char *Comma, int Group_FunctionType,
                      int iTime, double Time, int NbrTimeStep, int iRegion,
                      int numRegion, int NbrRegion, int NbrHarmonics,
                      int HarmonicToTime, int FourierTransform,
                      int Flag_NoNewLine, struct Value *Value);

void Pos_FourierTransform(int NbrTimeStep, int NbrRegion, double *Times,
                          struct Value *TmpValues, int Size, int TypeOutput,
                          int Nb_Freq_Select_0, int *NbrFreq,
                          double **Frequencies, struct Value **OutValues);

#endif
