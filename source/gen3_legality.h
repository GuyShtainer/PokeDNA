#ifndef GEN3_LEGALITY_H
#define GEN3_LEGALITY_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"

/* Structural legality (V1): cheap checks computed from a decoded PkMon + the data
 * tables ONLY (no encounter/learnset data — that's V2). It flags impossible or
 * tampered values (bad-egg checksum, illegal EV total, level/EXP mismatch, PP
 * over max, duplicate moves, etc.). It does NOT (yet) check whether a species can
 * legitimately appear at its met location or learn its moves.
 *
 * sev: 0 = warning, 1 = illegal. `ok` is false iff any illegal issue is present. */
typedef struct {
  bool ok;
  int  n;
  struct { uint8_t sev; char text[40]; } issue[24];
} PkLegality;

PkLegality pk_check_legality(const PkMon* m);

#endif /* GEN3_LEGALITY_H */
