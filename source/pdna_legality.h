#ifndef PDNA_LEGALITY_H
#define PDNA_LEGALITY_H

#include "gen3_mon.h"

/* Read-only legality card: runs pk_check_legality and shows "Legal" or a
 * scrollable list of issues. B returns. */
void pdna_legality_show(const PkMon* m);

#endif /* PDNA_LEGALITY_H */
