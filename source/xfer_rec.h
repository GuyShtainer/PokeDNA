#ifndef XFER_REC_H
#define XFER_REC_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_sidecar.h"   /* GbscEntry -- see the entry layout comment there          */
#include "gb_edit.h"      /* GbEditMon                                                 */

/* BACKLOG #150 S150-6, decision 9/D-Q4: the PURE half of the transfer ledger --
 * no FatFs, no tonc/sys.h, so tests/host_xferrt_test.c (S150-9's merge core,
 * xr_merge_down) and every future test that wants xr_key_g3 alone can link this
 * file without dragging lib/fatfs/ff.c in. source/xfer_io.c is the FatFs layer
 * (path resolution, the one reader, the one-time migration).
 *
 * This slice adds ONLY xr_key_g3 here -- §11.12 puts xr_merge_down/xr_open in the
 * same file too, but S150-8b/S150-9 own those; splitting keeps this file's own
 * scope exactly what this slice needs. */

/* FNV-1a-64 over rec80[0..7] -- the SAME offset basis/prime gbsc_key() uses
 * (source/gb_sidecar.c/.h), applied to a DIFFERENT 8 bytes (the Gen-3 record's own
 * PID+OTID span, bytes 0..7 of the 80-byte box-shape record) so a reroll (which
 * rewrites the PID, source/gen3_edit.c's em_set_pid/em_reroll) changes this key --
 * exactly the property the reroll re-key guard (decision 8) needs. */
uint64_t xr_key_g3(const uint8_t rec80[80]);

/* BACKLOG #150 S150-8 decision 5: the pk_item_games() mask bit for a Gen-3 origin-game
 * id (1 Sapphire, 2 Ruby, 3 Emerald, 4 FireRed, 5 LeafGreen) -- bit0 RS, bit1 Emerald,
 * bit2 FRLG (source/pdna_pick.c documents the mask). 0 for an unknown id (0 or > 5). */
uint8_t xr_game_item_mask(uint8_t met_game);

/* BACKLOG #150 S150-8 decision 14, §11.20 item 12(a): Gen 2 -> Gen 1 is allowed under
 * the time-capsule rules -- species <= gb_max_species(GB_GEN1) (151) and every
 * non-empty move <= gb_max_move(GB_GEN1) (165, exact -- Gen 1 has precisely moves
 * 1..165 with no gaps, D-Q7). Returns 0 when it may travel; 1 = species, 2 = a move
 * (and *bad is the offending species dex or move id, for the exact-reason message).
 * Gen 1 -> Gen 2 is always allowed and returns 0 without looking. */
int xr_time_capsule_block(uint8_t src_gen, uint8_t dst_gen, uint16_t species_dex,
                          const uint16_t moves[4], uint16_t* bad);

/* BACKLOG #150 S150-8b, decision 12: gb_sidecar.h's GbscMergeReport (the UP direction's
 * report) is not reused here on purpose -- extending it would drag gb_sidecar.h (frozen
 * by S150-6/S150-8) into a change it does not need. Same six field names so the confirm
 * screen can read either shape through a two-line adapter, plus three fields this
 * direction alone needs: a legality refusal is PER MOVE SLOT (G-H8, the mixed case is
 * the common one), a missing nickname baseline degrades instead of refusing (decision
 * 11's `direction != XR_DIR_ABROAD_G3` case), and species is only ever REPORTED
 * (decision 10 -- xr_merge_down never applies an evolution). */
typedef struct {
  bool evolved;               /* species_written differs from the Gen-3 record's own
                               * national dex number -- REPORTED ONLY, never applied  */
  bool level_changed;
  bool moves_changed;
  bool renamed;
  bool rename_refused;
  bool gb_item_ignored;       /* shipped string reused: the Gen-2 held item stayed in
                               * the cell and comes back -- G-H7                       */
  bool move_refused[4];       /* per-slot: the Gen-3 move id exceeded gb_max_move(gen),
                               * that slot kept the home's own move (G-H8)             */
  bool nick_baseline_missing; /* e->direction != XR_DIR_ABROAD_G3 (a pre-#150 or
                               * mis-stamped entry) -- the nickname row is skipped,
                               * never refused (an un-merged nickname is cosmetic)     */
  bool species_kept;          /* always true on a successful call -- decision 10       */
} XrMergeReport;

/* BACKLOG #150 S150-8b, decisions 10 + 11: rebuild the NATIVE cell's Game Boy record
 * (`out`, box-shape) from its own `original80` (via bank_cell.h's bc_unpack -- the
 * home, unedited) folding in whatever changed on the GEN-3 SIDE (`g3_rec80`, the
 * currently-held 80-byte Gen-3 box record) since the DOWN edge wrote it. Mirrors
 * gb_sidecar.c's gbsc_merge_up() in shape (same three-field walk: moves+PP-Ups,
 * level/EXP, nickname) but in the OPPOSITE direction: gbsc_merge_up rebuilds a GEN-3
 * record from a GEN-3 original80 folding in a GAME BOY edit; xr_merge_down rebuilds a
 * NATIVE record from a NATIVE original80 folding in a GEN-3 edit. Neither ever sees
 * the other's original80 (G-F4/G-H6).
 *
 * SPECIES/EVOLUTION IS NEVER APPLIED (decision 10): doing so needs gb_set_species()
 * with a GbGen1Base only a registered Game Boy ROM can supply for a Gen-1 home
 * (source/gen3_to_gb.c:70's G3GB_ERR_NEEDS_BASE), i.e. ROM I/O a pure-C core (hard
 * rule 5) must not perform. `rep->evolved` names the loss; the mon comes home as
 * what left (KEEP ORIGINAL, §11.6's own default). S150-9's toggle screen is where a
 * ROM-fetch callback and the per-row offer belong.
 *
 * Returns false, writing nothing to `out`/`rep`'s non-report fields (rep itself is
 * still zeroed), when: any pointer argument is NULL, `e->original80` is not a native
 * cell (`bc_is_native` false -- REFUSE-1), `bc_unpack` fails, or `pk_decode_mon`
 * fails on `g3_rec80`. Never calls gbsc_merge_up, gen3_edit_load or any `em_*` --
 * this is a `bc_unpack` -> patch -> hand-back-a-GbEditMon core; the caller does the
 * `bc_pack`. */
bool xr_merge_down(const GbscEntry* e, const uint8_t g3_rec80[80], GbEditMon* out,
                   XrMergeReport* rep);

#endif
