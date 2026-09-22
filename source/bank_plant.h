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
 * caller's own 80-byte scratch. */
void bank_plant_box_full(uint8_t* recs);

/* BACKLOG #150 S150-9 decision 12: the planted-ledger read shim (#179(a), the READ
 * half only -- the write side stays S150-13's). Serves up to FOUR in-RAM ".pds"
 * files (four 1042-B slots inside #ifdef PDNA_DELTA -- the delta image is not a
 * gate build, so this is not the 1,056-B EWRAM budget both gate builds share; both
 * gate ELFs' .bss/.sbss are byte-unchanged by this file, proven in the delivery
 * report with arm-none-eabi-size). `bank_plant_xfer_open` is xr_open()'s own shim
 * call: true + `buf`/`*len` filled only when `key` matches ONE of the seeded keys;
 * false (untouched `buf`/`*len`) for every other key, so a real ledger file (if one
 * ever existed on this vehicle, which it cannot -- no writable FAT) would not be
 * shadowed. */
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

#else
typedef int bank_plant_no_empty_tu;
#endif /* PDNA_DELTA */

#endif /* BANK_PLANT_H */
