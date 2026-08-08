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

/* Swap the box icons to bob frame 0/1 (the real Gen-3 2-frame pose animation) by
 * DMA-uploading that frame's tiles for every occupied icon. Call from the vblank tick. */
void boxoam_set_frame(int frame);

/* Position/show the cursor hand. on_title -> parked over the banner. mode picks the
 * normal/orange(MOVE)/translucent(ITEM) look. cur is the 0..29 grid cell. */
void boxoam_cursor(int cur, bool on_title, int mode);

/* Retail's grab beat is pose + motion, not blending: the hand dips 8 px at 1 px/frame
 * wide-OPEN over the mon (which stays in its cell), closes at the bottom, and rises
 * carrying it (pokemon_storage_system.c MonPlaceChange_Grab). These two knobs are that
 * animation's whole contract: the pose picks which tiles region A shows for the HAND
 * (the closed fist already exists as the carry sprite), and dy shifts the hand AND the
 * carry sprites vertically so one driver animates both phases. Both reset on
 * boxoam_enter so a stray mid-beat state can never leak across screens. */
enum { BOXOAM_POSE_NORMAL = 0, BOXOAM_POSE_REACH = 1 };
void boxoam_hand_pose(int pose);
void boxoam_cursor_dy(int dy);          /* 0 = rest; +8 = dipped onto the cell */

/* MOVE-mode carry (mon-in-hand): the held mon's icon (species/form) rides cursor cell
 * `cur` front-most in region A, an orange grab fist behind it; the cursor hand is hidden.
 * boxoam_carry_end() stops carrying; boxoam_hide_slot() lift-hides the origin cell. */
void boxoam_carry_held(int cur, uint16_t species, uint8_t form, bool egg);   /* egg=1 -> the Egg icon rides the glove */
void boxoam_carry_end(void);
void boxoam_hide_slot(int s);
void boxoam_show_slot(int s);   /* undo a lift-hide (grab-dip: mon stays visible) */

/* Rubber-band selection highlight (Emerald whitens the chosen; no rectangle). Every
 * OCCUPIED slot with sel[s]!=0 gets ATTR0_BLEND + the ITEM-mode alpha registers, so
 * ONLY the chosen icons go ghost-translucent against the wallpaper — the "picked up"
 * cue. (The Emerald-exact brightness whiten renders no effect on the real Omega/SP
 * display — proven on HW 2026-07-18.) Pure OAM attrs + 2 registers; zero VRAM. */
void boxoam_select_mark(const uint8_t sel[30]);
void boxoam_select_cursor(void);   /* hide the glove while rubber-banding (it would
                                    * mask the corner mon's ghost — obj can't blend
                                    * over obj); the ghosted block IS the cursor  */
void boxoam_select_clear(void);

/* Chunk carry (Emerald multi-move): render the WHOLE lifted group, not a
 * representative. Carried mon i floats at grid cell (tr+rr, tc+cc), lifted `lift` px
 * (8 = carry height; the grab/place beats animate 0..8),
 * priority 1, SOLID when `fit` (ghost-translucent = blocked), on OAM entry 34+i,
 * BORROWING the 16-tile VRAM region of the grid slot it covers (that slot's own
 * icon is occluded by the block, so its grid entry is hidden and its region holds
 * the carried art; uncovered slots are restored from the per-slot bookkeeping).
 * The orange grab fist rides cell (fist_r,fist_c) — the cell A was released on.
 * Re-call after every anchor move and after boxoam_load_box (a reload invalidates
 * the borrowed regions). boxoam_chunk_end() restores everything. */
typedef struct { uint8_t rr, cc;            /* footprint-relative row/col          */
                 uint16_t species; uint8_t form; uint8_t egg; } BoxOamChunkMon;
void boxoam_chunk_carry(int tr, int tc, int fist_r, int fist_c,
                        const BoxOamChunkMon* mons, int n, bool fit, int lift);
void boxoam_chunk_end(void);

/* "Bitmap understudy" hooks (§12b) — box_oam.c owns the covered-cell bookkeeping and
 * fires these exactly on cover/uncover TRANSITIONS of the chunk block (<= a footprint
 * of cells per keypress): under_show when the block newly covers an OCCUPIED cell
 * (its OBJ is hidden — the box screen blits that mon's icon INTO the Mode-3 bitmap so
 * the ATTR0_BLEND block alpha-blends over it, Emerald's see-through look), under_hide
 * when the block leaves it (the box screen restores just that cell's wallpaper rect).
 * Strong impls live in pdna_box.c (they need the decoded box + the verified wallpaper
 * staging); box_oam.c carries weak no-ops so it links standalone. */
void boxoam_under_show(int slot);
void boxoam_under_hide(int slot);

/* ITEM mode: show a small held-item marker on every occupied cell that holds an
 * item (uses `box` for occupancy/heldItem). Call when entering ITEM mode / after an
 * item edit. show=false hides all markers. */
void boxoam_item_markers(const PkMon box[30], bool show);

/* ITEM mode carry: show/hide the carried item icon riding cursor cell `cur`.
 * item != 0 shows that item's icon (falls back to a generic glyph if it has none);
 * item == 0 hides the carried-item sprite. */
void boxoam_carry_item(int cur, uint16_t item, bool full);   /* full=grab (32x32 over the mon); !full=hover (16x16 bottom-left) */

/* Flush the OAM shadow to hardware OAM. Call once per frame in the vblank window
 * (after VBlankIntrWait, before the beam starts) so updates never tear. */
void boxoam_commit(void);

#endif /* BOX_OAM_H */
