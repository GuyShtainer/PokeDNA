#ifndef BANK_PLANT_H
#define BANK_PLANT_H

#include <stdint.h>
#include <stdbool.h>

/* PDNA_DELTA-only test hook (BACKLOG #150 S150-2 step 6) -- plants synthetic native
 * Bank cells ("GBC1", source/bank_cell.h) into a box's raw record buffer so the
 * native-cell render path (box_decode_to/box_native_decode, source/pdna_box.c) can be
 * exercised in mGBA and on real hardware without needing a real Gen-1/2 save mounted
 * through the Bank at all. ZERO callers in the shipped build (PDNA_DELTA is never
 * defined there) -- precedent: the fused clip / fused GB corpus paths already under
 * PDNA_DELTA elsewhere in this tree. `recs` is a 2400-byte (30 * 80) box record
 * buffer, exactly source/pdna_bank.c's box_recs().
 *
 * ORCHESTRATOR-AUTHORIZED (D-Q7): this file's WHOLE body sits under #ifdef PDNA_DELTA;
 * a `typedef int bank_plant_no_empty_tu;` below keeps the shipped TU non-empty. */

#ifdef PDNA_DELTA
#include "gb_edit.h"   /* GbEditMon -- bank_plant_site2_seed's own argument */

/* Five directed cells at slots 0..4 (the rest of `recs` untouched -- the caller's own
 * memset(0, BOX_BYTES) on a failed read already left slots 5..29 as ordinary empty
 * Gen-3 box slots):
 *   0. Gen-2 CHIKORITA (dex 152) @ L12, origin GOLD          -> FULL
 *   1. Gen-1 PIKACHU (dex 25) @ L20, origin YELLOW            -> FULL
 *   2. the same Gen-2 mon as an EGG (BC_FLAG_EGG)              -> RELAXED-as-Egg
 *   3. the same Gen-2 mon HOLDING an item (BC_FLAG_HOLDS_ITEM) -> RELAXED
 *   4. a Gen-2 mon with an unrepresentable species (0xEE, a glitch index, never
 *      0xFF -- bc_pack refuses that one)                       -> NONE -> DMG chip
 */
void bank_plant_box0(uint8_t* recs);

/* BACKLOG #150 S150-15 decision 13: slot 0's own cell (Gen-2 CHIKORITA @ L12, origin
 * GOLD), built the SAME way bank_plant_box0() builds it -- called BY bank_plant_box0()
 * for its own slot 0, so the two can never drift apart. `out80` (80 bytes) receives
 * the packed cell. Exists so source/xfer_plant.c can convert the IDENTICAL bytes the
 * Bank's slot-0 VIEW shows, making S150-15's shot-chain pixel-parity pair
 * (00_bank_cell0_view vs 05_original_info_card) a real equality, not a coincidence. */
void bank_plant_cell0(uint8_t out80[80]);

/* BACKLOG #150 S150-15 review (fixture fix 1): bank_plant_cell0's own construction
 * (Gen-2 CHIKORITA @ L12, origin GOLD), with a CALLER-CHOSEN bank_serial instead of
 * the hard-coded 1u -- lets a caller pick a serial that gives a distinct xr_key_g3()
 * from every other PDNA_DELTA fixture's own planted key, without touching any byte a
 * summary card draws (the serial lives outside every drawn field). */
void bank_plant_cell0_serial(uint8_t out80[80], uint32_t serial);

/* The 30-NATIVE WORST CASE (SS11.9's Cost paragraph, SS11.13's S150-2 acceptance row):
 * slots 0-4 as bank_plant_box0 above (BACKLOG #150 S150-2 review F4: that is 2 FULL +
 * 2 RELAXED + 1 NONE/DMG, not five more FULL cells -- corrected here, this reuses
 * box0's own five directed cells rather than five fresh FULL ones); slots 5-29 = the
 * Gen-2 CHIKORITA cell repacked per slot (bank_serial 6..30, level 12 + (slot % 30)
 * clamped to 1..100, distinct bc_ident32 per cell, distinct level in the panel) and
 * EVERY ONE of those 25 takes the FULL rung, the expensive path (one gen12_convert --
 * a PID search -- per cell). Box-wide: 27 FULL (2 + 25), 2 RELAXED, 1 NONE across all
 * 30 native slots -- still the worst case §11.9 prices (every render at least attempts
 * gen12_can_convert, and 27/30 pay the full PID search). No new static: built in the
 * caller's own 80-byte scratch.
 *
 * BACKLOG #212 review D3(b): slot 7 (part of the generic 25-cell sweep above) is then
 * overwritten with bank_plant_gen2_badmoves_cell()'s own two-bad-move Bulbasaur -- the
 * --s150-8-bridge shot chain's move-swap-modal demonstration. */
void bank_plant_box_full(uint8_t* recs);

/* BACKLOG #212 review D3(b): a Gen-2 BULBASAUR (dex 1, clears the Gen-1 species
 * floor) with TWO moves (200, 230) out of range for Gen 1 (> gb_max_move(GB_GEN1)
 * =165) but legal on its own generation (<= gb_max_move(GB_GEN2)=251) -- bridged
 * to a Gen-1 session, this reaches gb_bank_down_bridge's per-slot move clip and
 * fill instead of the species-floor refusal box0's CHIKORITA plant always hits.
 * Planted by bank_plant_box_full() at box 1 slot 7. xfer-items fix pass F7: also
 * holds item id 19 -- decision 15's own rule (bank_down_convert.c) means a Gen-2
 * item NEVER reaches Gen 1 on the bridge arm, so this is the one planted cell that
 * can demonstrate the item/Secret-ID row's "stays behind" text unconditionally. */
void bank_plant_gen2_badmoves_cell(uint8_t out80[80]);

/* BACKLOG #246 (#104 Phase 1): a PLAIN Gen-3 cell (never native "GBC1") at slot 0 of
 * a DEDICATED box (box 2 -- wired in pdna_bank.c's box_load(), never box 0/1, so
 * every existing shot chain keyed on bank_plant_box0()/bank_plant_box_full()'s own
 * byte-for-byte content is untouched). This is the ONE cell type this whole file
 * never had a case for before this lane: every earlier plant is a native cell built
 * for the "native cell going DOWN" arms (S150-7/S150-8); this is the mirror -- a
 * Gen-3 record for BACKLOG #246's new "Gen-3 cell going DOWN into a GB save" arm.
 * `recs` is the same 2400-byte box buffer bank_plant_box0 takes; slot 0 (bytes 0..79)
 * holds the plain cell above, now carrying a POTION (BACKLOG #260, item-shots lane --
 * cases B/C's fixture). Slot 1 (bytes 80..159, BACKLOG #260) is a SEPARATE genuine
 * Gen-3-native record (gen3_build_mon, not the gen12_convert up-conversion pipeline)
 * with a nonzero Secret ID and the same item, for case F3's "+ Secret ID" suffix.
 * Slots 2..29 stay the caller's own memset-zero empty cells. */
void bank_plant_g3_box(uint8_t* recs);

/* BACKLOG #150 S150-9 decision 12: the planted-ledger read shim (#179(a), the READ
 * half only -- the write side stays S150-13's). Serves up to FIVE in-RAM ".pds"
 * files (BACKLOG #209 added the fifth; five 1042-B slots inside #ifdef PDNA_DELTA
 * -- the delta image is not a
 * gate build, so this is not the 1,056-B EWRAM budget both gate builds share; both
 * gate ELFs' .bss/.sbss are byte-unchanged by this file, proven in the delivery
 * report with arm-none-eabi-size). `bank_plant_xfer_open` is xr_open()'s own shim
 * call: true + `buf`/`*len` filled only when `key` matches ONE of the seeded keys;
 * false (untouched `buf`/`*len`) for every other key.
 *
 * CORRECTED (BACKLOG #175c/#226 review, 2026-09-23): the "no writable FAT" premise
 * above was already stale -- a VSD-attached delta build DOES have a real, writable
 * FAT (BACKLOG #179's virtual SD), and app_xfer_save_now() really does land a real
 * ledger file there. The bug this shim used to have was the OTHER direction:
 * xr_open() tried this shim BEFORE ever reading the real file, so once a real write
 * landed it stayed permanently shadowed by the stale planted bytes -- attempt 1 of
 * a restore's SAVE NOW? loop kept re-reading the SAME PENDING plant forever, and
 * "save-now succeeds -> the restore proceeds" (the loop's own happy path) could
 * never execute at either restore site. xr_open() now tries the real file FIRST
 * and only falls back to this shim on a genuine SF_ERR_OPEN, so a real write DOES
 * shadow the plant once it exists -- exactly the shadowing this comment used to
 * (wrongly) say was structurally impossible. */
bool bank_plant_xfer_open(uint64_t key, uint8_t* buf, uint32_t cap, uint32_t* len);

/* Builds and seeds all four planted ledger entries (decisions 7/8/12(b)/12(c)),
 * filling g3_out[0..3][80] with their own gen12_convert()'d Gen-3 records -- the
 * caller (pdna_main.c's PC-storage mount hook) places g3_out[i] at PC box 0 slot
 * (29 - i). g3_out[0] (slot 29, CLAIMED+ALTERED -- the main chain) is built from
 * the SAME cell construction as bank_plant_box0()'s own Bank box 0 slot 0 (level
 * 12, seed 1), so the two are byte-identical, exactly what a real DOWN write would
 * have produced. See bank_plant.c's own comment for slots 1-3 (the "nothing
 * changed" skip, the ALREADY RESTORED refusal, the SAVE FIRST refusal). Returns
 * the count of slots successfully seeded (0..4). */
int bank_plant_xfer_seed_all(uint8_t g3_out[4][80]);

/* BACKLOG #209: a FIFTH planted ledger slot, keyed by gbsc_key() (gen/otid16/dv4/
 * otname -- the NATIVE identity the restore (gb_lift_restore() until #280; now gb_bridge_restore_up()) computes from a Game Boy
 * grid mon, source/pdna_gen12.c), never xr_key_g3() (the four slots above, keyed by
 * a Gen-3 record's PID+otId) -- a completely different key space, so none of those
 * four could ever serve site 2 (the Red-grid restore lift). Seeded at MOUNT TIME
 * (not a boot-time constant): `mon` must be whatever the fused save ACTUALLY holds
 * at its own box 0 slot 0 (tools/fuse_sav.py's choice, not this file's), or the key
 * this plants would not match what gb_lift_restore() computes when the player lifts
 * that exact cell. Call once, right after the fused save's bytes are decoded and
 * before the session mounts (source/pdna_main.c). A byte-identical restore (CLAIMED,
 * unaltered) -- the simplest, most legible first proof that gb_lift_restore ever
 * ran on this vehicle at all. */
void bank_plant_site2_seed(const GbEditMon* mon);

/* BACKLOG #280 (lane y9-280): the planted vehicle for the two TARGET-drop restores -- five Gen-2
 * BULBASAUR cells for Bank box 3 and each one's ledger FILE (case 0,1 G3_HOME; 2 bridge CLAIMED; 3
 * bridge RESTORED; 4 bridge PENDING -- see bank_plant.c). The ledger is written into the --vsd
 * image by tests/y9_mkledger.c, never planted on the device. */
bool bank_plant_y9_cell(int which, uint8_t out80[80]);
int  bank_plant_y9_ledger(int which, uint8_t* file, uint32_t cap, uint64_t* key_out);
void bank_plant_y9_box(uint8_t* recs);

/* BACKLOG #209: bank_plant_xfer_open()'s own existence-only twin -- true iff `key`
 * matches ONE of the (now five) seeded slots, touching neither `buf` nor `len`. Lets
 * xr_path_for_key_hint() (source/xfer_io.c) answer gb_has_sidecar()'s plain "does a
 * ledger file exist for this key" question without needing a scratch buffer of its
 * own (xr_path_for_key_hint has none to spare -- it only ever builds a path string). */
bool bank_plant_xfer_has(uint64_t key);

#else
typedef int bank_plant_no_empty_tu;
#endif /* PDNA_DELTA */

#endif /* BANK_PLANT_H */
