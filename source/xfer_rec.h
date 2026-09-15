#ifndef XFER_REC_H
#define XFER_REC_H

#include <stdint.h>

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

#endif
