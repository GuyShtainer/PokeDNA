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

/* BACKLOG #150 S150-9, decision 1: accept masks -- a per-field PATCH applied to a COPY
 * of the home bytes through the shipped setters, never by rebuilding the record. The
 * REPORT is mask-independent (rep->level_changed etc. mean "the field differs abroad
 * from the written baseline", whether or not the bit is set); only the APPLY is gated.
 * `accept == 0` therefore returns the home byte-for-byte, structurally. XR_ACCEPT_SPECIES
 * is meaningful ONLY to gbsc_merge_up_sel (the Gen-3-home direction) -- both
 * xr_merge_down_sel and xr_merge_down_gb_sel ignore it (decision 4: species is always
 * reported-only on the native-home side). */
#define XR_ACCEPT_LEVEL   0x01u   /* level (+EXP recomputed under the home growth rate) */
#define XR_ACCEPT_MOVES   0x02u   /* moves + PP-Ups (+ current PP clamped)              */
#define XR_ACCEPT_NICK    0x04u   /* nickname                                            */
#define XR_ACCEPT_SPECIES 0x08u   /* Gen-3-home direction ONLY (gbsc_merge_up_sel);
                                   * xr_merge_down*_sel IGNORE it -- decision 4        */
#define XR_ACCEPT_ALL     0x0Fu

/* BACKLOG #150 S150-8b, decision 12: gb_sidecar.h's GbscMergeReport (the UP direction's
 * report) is not reused here on purpose -- extending it would drag gb_sidecar.h (frozen
 * by S150-6/S150-8) into a change it does not need. Same six field names so the confirm
 * screen can read either shape through a two-line adapter, plus three fields this
 * direction alone needs: a legality refusal is PER MOVE SLOT (G-H8, the mixed case is
 * the common one), a missing nickname baseline degrades instead of refusing (decision
 * 11's `direction != XR_DIR_ABROAD_G3` case), and species is only ever REPORTED
 * (decision 10 -- xr_merge_down never applies an evolution).
 * S150-9 decision 5 adds level_from/level_to (baseline / abroad, 0 when no level row)
 * and abroad_item_dropped (a GB-side held item that cannot ride onto the OTHER Game Boy
 * generation -- xr_merge_down_gb only; folds the lane's out-of-band g3_item probe). */
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
  uint8_t level_from, level_to; /* S150-9 decision 5 -- 0/0 when there is no level row */
  bool abroad_item_dropped;   /* S150-9 decision 2 -- xr_merge_down_gb only            */
} XrMergeReport;

/* BACKLOG #246 D3 fix: the "two-line adapter" this file's own decision-12 comment
 * above already promised -- gbsc_merge_up_sel's GbscMergeReport (the Gen-3-home/UP
 * direction) read through the SAME app_xfer_merge_screen (pdna_app.h) every other
 * direction already shares. Moved here (was a pdna_main.c static, app_paste_gb_lookup's
 * own private helper) because BACKLOG #246's Bank-UP lift (source/pdna_gen12.c
 * gb_lift_restore) needs the identical conversion and pdna_gen12.c cannot reach a
 * pdna_main.c static; xfer_rec.h already includes gb_sidecar.h (GbscMergeReport), so no
 * new include and no circular-include risk (this header's own note above). A straight
 * field copy, never lossy: both reports share the same six names by design (decision 12). */
void xr_report_from_gbsc(const GbscMergeReport* g, XrMergeReport* x);

/* BACKLOG #246 review F2 fix: the ONE entry-resolve decision both #246 arms need --
 * gb_lift_restore_g3home (the Bank-UP lift) and gb_release_g3home (the matching
 * release, source/pdna_gen12.c) used to carry two hand-copied loops of this same
 * tiebreak. Walks every `want_kind` match gbsc_find() returns (include_claimed =
 * true -- a #246 entry is always claimed=false by construction, F3, but the walk
 * itself must not filter on it) and prefers the one whose species_written equals
 * `nowdex`, falling back to the first match, exactly as both original loops did.
 * Returns the winning index, or -1 if `want_kind` has no match in `buf`. Pure C
 * (only gb_sidecar.h's gbsc_find/gbsc_get) so tests/host_xfer_roundtrip_test.c
 * links the REAL decision instead of re-implementing it (F2's own point: a
 * structural test that calls gbsc_find directly proves nothing about this
 * tiebreak, and a mutation of pdna_gen12.c's old copy passed the suite green). */
int xr_resolve_home(const uint8_t* buf, uint32_t len, const GbEditMon* mon,
                    int want_kind, uint16_t nowdex);

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
 * `bc_pack`.
 *
 * S150-9 decision 1: `accept` gates which fields the walk APPLIES (species is never
 * applied regardless of `accept` -- decision 4); `rep` is filled identically no matter
 * what `accept` is. `xr_merge_down` is the ACCEPT_ALL wrapper every s150-8b caller and
 * test already expects. */
bool xr_merge_down_sel(const GbscEntry* e, const uint8_t g3_rec80[80], uint8_t accept,
                       GbEditMon* out, XrMergeReport* rep);
bool xr_merge_down(const GbscEntry* e, const uint8_t g3_rec80[80], GbEditMon* out,
                   XrMergeReport* rep);

/* BACKLOG #150 S150-9 decision 2: site 2 of the restore (a GB lift of a mon whose
 * ledger entry has a NATIVE home) -- rebuild the NATIVE home from e->original80
 * folding in whatever changed in the OTHER-generation Game Boy save `now` lives in
 * (the bridge wrote it: direction XR_DIR_ABROAD_GB, written = the GB record as
 * written, nick_written/otname_written = raw GB bytes). Mirrors xr_merge_down_sel
 * field for field; see source/xfer_rec.c's table comment for the exact per-field
 * baseline/compare/apply rules. otname_written is NOT compared (identity field).
 *
 * Refuses (false, `out` untouched, `rep` zeroed) on NULL, on
 * `!bc_is_native(e->original80)`, on a `bc_unpack` failure, on
 * `e->direction != XR_DIR_ABROAD_GB`, or on `now->gen != e->gen` (the entry's `gen`
 * is the RESIDENCE generation -- gbsc_entry_from() copies `written->gen`). */
bool xr_merge_down_gb_sel(const GbscEntry* e, const GbEditMon* now, uint8_t accept,
                          GbEditMon* out, XrMergeReport* rep);
bool xr_merge_down_gb(const GbscEntry* e, const GbEditMon* now, GbEditMon* out,
                      XrMergeReport* rep); /* ACCEPT_ALL wrapper */

/* BACKLOG #206 review R1: gbpc_restore_up's (source/pdna_box.c) pick loop +
 * RESTORED/PENDING refusals, extracted pure so a host test can exercise them
 * directly instead of a synthetic bank_restore_from_entry(e, ...) call that never
 * touches the pick/state logic at all (the pre-extraction regression test's own
 * gap). S150-9 decision 8's tiebreak: the HIGHEST index among the ledger's entries
 * whose kind is XR_KIND_NATIVE_HOME and whose original80 is bc_is_native() (the
 * newest cycle; belt-and-braces for pre-#150 entries missing the kind byte).
 * XR_PICK_NONE: no matching entry (buf has only Gen-3-home entries, or none) --
 * caller treats the cell as already exact. XR_PICK_REFUSE_RESTORED/PENDING: the
 * picked entry's own state refuses the restore outright, decision 8's "before the
 * screen" order (§3.2) -- `out` is left untouched on both refusals and XR_PICK_NONE.
 * XR_PICK_LIVE: `out` holds the picked entry (CLAIMED, or the pre-state-byte NONE
 * case gbpc_restore_up itself still logs and treats as CLAIMED); the caller
 * still does the probe / confirm screen / bank_restore_from_entry as before -- this
 * function makes NO identity comparison (nickname/species) against any abroad
 * record and never will; that is exactly the regression BACKLOG #206 pinned against. */
typedef enum {
  XR_PICK_NONE = 0,
  XR_PICK_LIVE,
  XR_PICK_REFUSE_RESTORED,
  XR_PICK_REFUSE_PENDING,
} XrRestorePick;

XrRestorePick xr_restore_pick_basic(const uint8_t* buf, uint32_t len, int count,
                                    GbscEntry* out);

/* #270 (Guy 2026-09-29, the Bank is a pass-through): the generation (GB_GEN1/GB_GEN2)
 * of the native original the SAME pick loop as xr_restore_pick_basic would choose
 * (the LAST NATIVE_HOME entry whose original80 is a native cell), whatever its state;
 * 0 when there is none or the cell does not unpack. The target-drop restore compares
 * it with the destination save's generation BEFORE it offers any screen. */
uint8_t xr_home_gen(const uint8_t* buf, uint32_t len, int count);

/* BACKLOG #150 S150-9 decision 11: the entry builder xfer_down_write() inlined
 * (source/pdna_gen12.c) -- pure gbsc_entry_from() + five field stores + one memcpy,
 * moved here so the flagship host round-trip test runs the SAME logic the DOWN edge
 * really writes, not a synthetic copy. `nick_g3` is the 10 raw Gen-3 nickname bytes
 * for XR_DIR_ABROAD_G3 (NULL for XR_DIR_ABROAD_GB, where `written->nick` is used
 * instead -- gbsc_entry_from()'s own default). Sets kind = XR_KIND_NATIVE_HOME,
 * state = XR_STATE_PENDING, claimed = 1, direction = `direction`. */
void xr_entry_for_down(GbscEntry* e, const GbEditMon* written, const uint8_t cell80[80],
                       uint32_t epoch, uint8_t direction, const uint8_t nick_g3[10]);

#endif
