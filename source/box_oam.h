#ifndef BOX_OAM_H
#define BOX_OAM_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"      /* PkMon */

/*
 * Hardware-OAM layer for the PC/Bank box screen (source/pdna_box.c).
 *
 * The 6x5 grid of 32x32 box icons — and everything that sits ON TOP of them
 * (cursor hand, move-mode carried icon + grab fist, ITEM-mode held-item markers +
 * carried item) — are drawn as GBA OBJ sprites instead of software Mode-3 blits.
 * The GPU composites all of them every frame for free, so the idle "bob" is a
 * trivial in-vblank OAM nudge (all icons move together, zero flicker, ~0 CPU) and
 * the cursor is never blocked. The wallpaper / banner / left PKMN-DATA panel / tabs
 * / footer stay SOFTWARE on the BG2 bitmap (sprites render above them).
 *
 * OBJ-VRAM is HALVED in bitmap modes: only tile ids 512..1023 (charblock 5,
 * 0x06014000) are usable. Budget: 30 icons x 16 tiles = 480 (ids 512..991), the
 * cursor hand = 16 tiles (ids 992..1007), and a 16-tile shared region (ids
 * 1008..1023) time-shared between the move-mode grab fist and the ITEM-mode item
 * glyphs (those modes never coexist). Exactly 512 tiles — fits the window.
 *
 * OBJ is enabled ONLY between boxoam_enter()/boxoam_exit(); every other PokeDNA
 * screen stays pure Mode 3 (OBJ off, OAM cleared).
 */

/* Cursor look (mirrors pdna_box's CM_NORMAL/CM_MOVE/CM_ITEM). */
enum { BOXOAM_HAND_NORMAL = 0, BOXOAM_HAND_MOVE, BOXOAM_HAND_ITEM };

/* Enable OBJ, init OAM, upload the hand/grab tiles + fixed palettes. Call on box
 * entry (after ui_init has set Mode 3). */
void boxoam_enter(void);

/* Hide all sprites and DISABLE OBJ in DISPCNT + clear OAM. Call on box EXIT so the
 * summary/dex/trainer/menu screens (all pure Mode 3) are unaffected. */
void boxoam_exit(void);

/* Turn OBJ display off / back on WITHOUT touching OAM or VRAM — wrap any modal or
 * sub-screen drawn over the box (action menu, box-options, summary) so the icon
 * sprites don't bleed through the software panel. Cheap (a DISPCNT bit). */
void boxoam_suspend(void);
void boxoam_resume(void);

/* Upload the occupied icons of `box` (30 PkMon, species 0 = empty) into OBJ VRAM
 * (tiles + per-icon palettes) and lay out the 30 grid OAM entries; empty cells are
 * hidden. Call on entry, on box switch (L/R), and after any edit that changes the
 * box contents. */
void boxoam_load_box(const PkMon box[30]);

/* Set the idle-bob vertical offset (0 or 1 px) applied uniformly to all 30 icon
 * sprites. Pure OAM write — call from the vblank tick. */
void boxoam_set_bob(int dy);

/* Position/show the cursor hand. on_title -> parked over the banner. mode picks the
 * normal/orange(MOVE)/translucent(ITEM) look. cur is the 0..29 grid cell. */
void boxoam_cursor(int cur, bool on_title, int mode);

/* MOVE-mode carry: show the held icon (from grid slot `from`) and the grab fist
 * riding cursor cell `cur`; hides the cursor hand. Pass from<0 to clear carry. */
void boxoam_carry(int cur, int from);

/* ITEM mode: show a small held-item marker on every occupied cell that holds an
 * item (uses `box` for occupancy/heldItem). Call when entering ITEM mode / after an
 * item edit. show=false hides all markers. */
void boxoam_item_markers(const PkMon box[30], bool show);

/* ITEM mode carry: show/hide the carried item icon riding cursor cell `cur`.
 * item != 0 shows that item's icon (falls back to a generic glyph if it has none);
 * item == 0 hides the carried-item sprite. */
void boxoam_carry_item(int cur, uint16_t item);

/* Flush the OAM shadow to hardware OAM. Call once per frame in the vblank window
 * (after VBlankIntrWait, before the beam starts) so updates never tear. */
void boxoam_commit(void);

#endif /* BOX_OAM_H */
