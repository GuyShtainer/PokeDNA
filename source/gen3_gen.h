#ifndef GEN3_GEN_H
#define GEN3_GEN_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_pidiv.h"   /* PkSpread, PkSpreadWant, PK_SPREAD_* */

/* CREATE A POKEMON WHOSE PID AND IVs BELONG TOGETHER. Pure C — no tonc, no GBA headers —
 * so tests/host_spread_test.c runs the whole thing on the PC.
 *
 * WHY THIS IS A SEPARATE FUNCTION AND gen3_build_mon KEEPS ITS SIGNATURE. The other caller
 * of gen3_build_mon is the Gen-1/2 importer, which has already SOLVED for a specific PID
 * to preserve the imported Pokemon's gender, nature and Unown letter (gen12_convert.c:347)
 * and then writes the IVs from the GB record's own DVs (:369-374). Rolling either there
 * would overwrite the user's data with noise. Also, putting the roll inside gen3_edit.c
 * would force gen3_pidiv.c onto the cc line of eight existing host tests; this file is a
 * leaf, so the blast radius is zero.
 *
 * Guy's report was "the venusaur ... had 0 IVs ... simply randomize a PID and use it to
 * rightfully calculate its IVs". The substance is right; one word needs correcting. Gen-3
 * IVs are NOT derived from the PID — they are stored in the record — but they come out of
 * the SAME LCRNG stream, in the two calls immediately after it (src/pokemon.c:2216,
 * 2277-2293). So the operation is "roll a seed and take the PID and the IVs it produces
 * together", which is pk_spread_roll, and which pk_pidiv_search then re-finds as Method 1. */

typedef struct {
  PkSpread spread;
  uint8_t  level;
  uint8_t  hatched;     /* 1 = the record claims met level 0, "hatched at"  */
  uint8_t  spread_ok;   /* 0 = the constraint search hit its cap            */
} Gen3BuildInfo;

/* gen3_build_mon, plus the IVs of the seed that produced its PID. `want` may be NULL for
 * "any spread" — one roll, four multiplies. Its otId and Unown-order bit are overwritten
 * from the arguments so a caller cannot shiny-test against the wrong trainer or emit a
 * normal-order Unown. Returns false ONLY when the constraint search hit its cap; the
 * record is still written and still carries a matched Method-1 pair. */
bool gen3_build_mon_spread(uint16_t species, uint8_t lvl, uint32_t seed, uint32_t otId,
                           const char* otName, uint8_t metgame,
                           const PkSpreadWant* want, uint8_t out[80], Gen3BuildInfo* info);

/* The PID-assembly order this species must be generated in. */
uint32_t gen3_spread_opts(uint16_t species);

#endif /* GEN3_GEN_H */
