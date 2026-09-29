#ifndef GB1_BASE_TBL_INCLUDED
#define GB1_BASE_TBL_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#include "gb_edit.h"       /* GbGen1Base, GB_NSTATS */
#include "gb_new_mon.h"    /* GbNewMonSrc           */

/*
 * The Gen-1 base-stat / type / growth table for dex 1..151 (BACKLOG #276), so CREATE >
 * FROM SCRATCH on a Gen-1 save needs no ROM. The DATA is not in the tree: it is emitted
 * by tools/gen_gb1_base.py from the pret pokered decomp into the GIT-IGNORED
 * source/gb1_base_gen.c (numbers only -- same generate-locally policy as
 * data_tables.c). source/gb1_base_fallback.c is the COMMITTED weak default, so a clone
 * that never ran the generator still links and simply reports "no table".
 *
 * PURE C: <stdint.h>/<stdbool.h> only. No I/O of any kind -- a caller that gets `true`
 * has touched no file, by construction.
 */

/* One generated row: hp, atk, def, spe, spc, type1, type2, growth (raw Gen-1 type ids,
 * growth 0..5 == gen1_write.h GEN1_GROWTH_*). */
#define GB1_BASE_ROW_LEN 8

/* Generated (strong) or weak fallback (returns false). false also for dex outside 1..151
 * or a NULL `out`; `out` is untouched on false. */
bool gb1_base_gen_row(uint16_t dex, uint8_t out[GB1_BASE_ROW_LEN]);

/* Is the generated table linked at all? (dex 1 resolves.) */
bool gb1_base_table_present(void);

/* Fill `src`'s base/type1/type2/growth from the table; leaves every other field of `src`
 * (moves, names, ids) alone. false, `src` untouched, when the table is absent or `dex`
 * is outside 1..151. */
bool gb1_base_table_fill(uint16_t dex, GbNewMonSrc* src);

#endif
