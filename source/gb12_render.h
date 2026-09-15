#ifndef GB12_RENDER_H
#define GB12_RENDER_H

#include <stdint.h>
#include "gen12_convert.h"   /* Gb12Mon, Gb12Target, Gb12Result */

/* The Gen-1/2 -> Gen-3-box-record DISPLAY LADDER, pure C (BACKLOG #150 S150-2).
 *
 * Factored out of pdna_gen12.c's gb_build_slot (pdna_gen12.c:249-311 on main
 * 2161d74) so a second caller -- the native Bank cell's own render path in
 * pdna_box.c -- can share it byte-for-byte instead of re-implementing the ladder.
 * Both callers are tonc TUs; this header/TU pair has no tonc/sys.h dependency so
 * tests/host_gb12render_test.c can drive it directly on the host.
 *
 * `id_salt` replaces gb_build_slot's separate (box, slot) pair: the ONE value that
 * must feed both Gb12Mon.slot_salt (determinism/twin-distinguishing, see
 * gen12_convert.h) and the placeholder PID (nature/gender/shininess -- visible
 * pixels), so a caller cannot pass two different values by mistake. The GB grid
 * passes box*20+slot (unchanged: gb_slot_mon already computes that same salt into
 * Gb12Mon.slot_salt); the Bank passes bc_ident32() so a native cell's placeholder
 * is stable per-cell instead of per-grid-position.
 */

/* How a record is PRESENTED in the grid. Verbatim from pdna_gen12.c:203-206. */
enum { GB_SHOW_FULL, GB_SHOW_RELAXED, GB_SHOW_PLACEHOLDER, GB_SHOW_NONE };

/* Decide the presentation WITHOUT running a PID search, so a whole-save census (or a
 * Bank occupancy scan) stays cheap. Was static gb_presentation, pdna_gen12.c:211. */
int gb12_presentation(const Gb12Mon* in, Gb12Result r);

/* Build the 80-byte record for one already-decoded Gb12Mon and its refusal reason.
 * `rec80` is memset(0) by this function itself at entry (decision 2: the pre-refactor
 * contract was "the caller zeroes it", pdna_gen12.c:248 -- the Bank caller's scratch is
 * NOT pre-zeroed, so the obligation moves here instead of growing a second copy of it).
 * `id_salt` is used both to distinguish otherwise-identical records (Gb12Mon.slot_salt)
 * and to derive the placeholder PID (decision 3/4) -- the function makes an internal
 * copy of `*in` and forces `m.slot_salt = id_salt` so the two can never drift apart.
 * Returns the GB_SHOW_* rung actually taken. */
int gb12_render_rec(const Gb12Mon* in, const Gb12Target* tgt, uint32_t id_salt,
                    uint8_t rec80[80], uint8_t* reason);

#endif /* GB12_RENDER_H */
