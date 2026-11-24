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

// whether a Print stores the minimum or the maximum of its values, or their
// coordinates, in registers (StoreMinInRegister, StoreMaxXinRegister, ...)
bool Pos_StoresMinMax(struct PostSubOperation *PostSubOperation_P);

// whether a Print can be evaluated by each rank on its mesh partition (see
// Pos_Print.cpp), the condition shared by Pos_IsGathered,
// Pos_IsStoredByPartition and Pos_IsParallelView
bool Pos_IsLocalPrint(struct PostSubOperation *PostSubOperation_P);

bool Pos_IsGathered(struct PostSubOperation *PostSubOperation_P);

// A field stored by a Print on the elements, evaluated by each rank on its
// mesh partition (see Pos_Print.cpp); while the file of such a Print is
// written, the fields are not stored (Pos_SetStoreFields(false))
bool Pos_IsStoredByPartition(struct PostSubOperation *PostSubOperation_P);
void Pos_SetStoreFields(bool store);
bool Pos_GetStoreFields();

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
