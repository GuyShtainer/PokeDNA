/* gb_moves_legal.h -- BACKLOG #150 S150-10: the per-slot MAKE-LEGAL move rule.
 *
 * PURE C: <stdint.h>/<stdbool.h> plus gb_edit.h only. No tonc, no FatFs, no sys.h.
 * The bound is gb_max_move(gen) (source/gb_edit.c:68, 165 for GB_GEN1 / 251 for
 * GB_GEN2) -- never a literal 165/251 anywhere in this file. Struggle (165) is
 * refused as a FILL, per gb_edit.h:555-558's own note ("a battle-only fallback that
 * no save stores") -- the learnset reader never emits it, so the guard makes "MAKE
 * LEGAL never invents Struggle" a property of this file, not of the ROM. What the
 * Game Boy games do with a hole in the move list justifies the pack rule below:
 * assets/upstream/pokecrystal/engine/pokemon/mon_stats.asm:438-442 (Gen 2's move
 * lister stops at the first zero) and assets/upstream/pokered/engine/pokemon/
 * status_screen.asm:334-338 (Gen 1's PP loop does the same) -- reference only,
 * numbers not code.
 */
#ifndef GB_MOVES_LEGAL_H
#define GB_MOVES_LEGAL_H

#include <stdint.h>

#include "gb_edit.h"   /* GbEditMon, gb_max_move, gb_get_move/set_move/set_ppup/set_pp */

/* Per-slot legality over a raw 4-move array (Gen-3 ids, which share Gen-1/2's id
 * space 1..gb_max_move(gen)). Sets bad4[i] = (moves[i] != 0 && moves[i] >
 * gb_max_move(gen)) for i = 0..3 and returns the count of bad slots (0..4), or -1 on
 * a bad `gen` (not GB_GEN1/GB_GEN2) or a NULL `moves`/`bad4`. */
int g3gb_moves_ok(const uint16_t moves[4], uint8_t gen, uint8_t bad4[4]);

/* Same, decoded from a raw 80-byte Gen-3 record (pk_decode_mon + the above). -1 on a
 * decode failure or an Egg (mirrors gb_clip_dex's own refusal set). */
int g3gb_moves_ok_rec(const uint8_t rec80[80], uint8_t gen, uint8_t bad4[4]);

/* Fill each bad slot (bad4[i] != 0) with the first entry of `learn4` (in order) that
 * is non-zero, is not already one of `out`'s KEPT (non-bad) moves, is not already
 * assigned to an earlier bad slot in this same call, and is not Struggle (165).
 * Written with gb_set_move (fresh base PP, 0 PP-Ups -- a newly learned move has never
 * had a PP Up used on it). A bad slot with no eligible fill is left empty (fill4[i] =
 * 0); fill4[i] is 0 for every non-bad slot too. Calls g3gb_moves_pack() last, so the
 * result never has a hole before a kept-or-filled move.
 * Returns the count of slots that received a real move (0..4), or -1 on NULL/a `gen`
 * mismatch between `out` and the bound this file must use. */
int g3gb_moves_fill(GbEditMon* out, const uint8_t bad4[4], const uint8_t learn4[4],
                     uint8_t fill4[4]);

/* Move every non-empty slot down over any empty one, preserving relative order and
 * carrying the slot's PP + PP-Ups (never its "changed" identity -- this is a pure
 * position compaction). Returns true if anything moved. */
bool g3gb_moves_pack(GbEditMon* e);

#endif
