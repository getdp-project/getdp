// GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
//
// See the LICENSE.txt file for license information. Please report all
// issues on https://gitlab.onelab.info/getdp/getdp/issues.

#ifndef POS_PRINT_H
#define POS_PRINT_H

#include <vector>
#include "ProData.h"

#define ARG                                                                    \
  struct PostQuantity *NCPQ_P, struct PostQuantity *CPQ_P, int Order,          \
    struct DefineQuantity *DefineQuantity_P0,                                  \
    struct QuantityStorage *QuantityStorage_P0,                                \
    struct PostSubOperation *PostSubOperation_P

void Pos_PrintOnRegion(ARG);
void Pos_PrintOnElementsOf(ARG);
void Pos_PrintOnSection(ARG);
void Pos_PrintOnGrid(ARG);
void Pos_PrintWithArgument(ARG);

#undef ARG

bool Pos_IsGathered(struct PostSubOperation *PostSubOperation_P);

// The phases of a Print gathered on the first rank: evaluated and written at
// once, or evaluated by all the ranks and written later, when the
// post-operation reaches a point where the files must be complete (see
// Treatment_PostOperation)
enum { POS_EVALUATE_AND_WRITE, POS_EVALUATE, POS_WRITE };
void Pos_SetGatherPhase(int phase);
int Pos_GetGatherPhase();
bool Pos_IsWrittenLater(struct PostSubOperation *PostSubOperation_P);
bool Pos_MustWriteBefore(struct PostSubOperation *PostSubOperation_P,
                         const std::vector<struct PostSubOperation *> &later);

void Pos_PrintGroup(struct PostSubOperation *PostSubOperation_P);
void Pos_PrintExpression(struct PostSubOperation *PostSubOperation_P);

#endif
