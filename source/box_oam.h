#ifndef BOX_OAM_H
#define BOX_OAM_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"      /* PkMon */
#include "hand_gate.h"     /* PDNA_HAND_ART_COMPILED */

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

/* Phase-1 ROM-gated icons: the app registers an open RomMon on the user's own ROM
 * (fused today; a registered SD file later) and the box streams the real 32x32
 * icons from it whenever the compiled icon art is absent. NULL clears. */
struct RomMon;
void boxoam_rom_icons(const struct RomMon* rm);
/* Phase 3 (ROM-gated glove, DESIGN.md Sec 1.3/4.5): the app registers an open
 * RomHand the same way. NULL clears. Compiled out entirely when the real poses are
 * linked in (hand_gate.h) — a full-art build never holds a RomHand at all, so it
 * costs this build literally nothing, not even a pointer store. */
struct RomHand;
#if !PDNA_HAND_ART_COMPILED
void boxoam_rom_hand(const struct RomHand* rh);
#endif
/* Phase 2 (ROM-art cache): register /PokeDNA/art/icons.bin as the box's FIRST icon
 * rung, ahead of the ROM stream (DESIGN.md Sec 4.1's ladder). path must already be
 * validated by the caller (art_session_icons_ready()) -- this function does not
 * re-check anything, it only opens the file and preloads its small metadata tail.
 * NULL closes/clears (a ROM re-registration whose cache turned out invalid, or none
 * present). Held open for the whole session, not re-opened per box entry. */
void boxoam_set_icon_cache(const char* path);
/* 1 if the grid can show real icons (compiled art OR a registered ROM) — the
 * artless name-chip fallback asks this before painting chips. */
int  boxoam_icons_available(void);

/* Blit grid slot `s`'s CURRENT icon straight into the Mode-3 bitmap at its own normal
 * grid position — same tile bytes, same live OBJ palette bank (pal_obj_mem) already
 * driving its sprite, so this is pixel-identical to what the OBJ itself shows (frame,
 * species, form, egg pose all included) rather than a second, independently-generated
 * bitmap icon table that cannot be guaranteed to agree with it pixel for pixel. For
 * pdna_box.c's party-strip bitmap restore: a grid slot the strip's own on-screen
 * column hides has nothing else that can stand in for it without this seam. A no-op
 * for an empty or art-less slot. */
void boxoam_slot_blit_bitmap(int s);

/* MUST-FIX 5 (2026-08-20 review, PC-box party PANEL): draw species/form/egg's own
 * icon as a Mode-3 BITMAP at (x,y), clipped to [cx0,cx1) x [cy0,cy1) — pixels outside
 * that rect are never written. Same icon lookup as boxoam_slot_blit_bitmap (compiled
 * art -> icons.bin cache -> the registered ROM, staged + read-verified the same way),
 * but parameterised by an arbitrary destination + clip rect instead of a grid slot,
 * because a bitmap blit CAN be clipped per-pixel where an OBJ sprite cannot. Safe to
 * use here specifically because party_strip_overlay (pdna_box.c) now hides the box's
 * own cursor hand + any carried mon/item for as long as the panel is open (MUST-FIX
 * 2) — that was the ONLY reason the panel's icons rode an OBJ instead of a bitmap in
 * the first place (an OBJ composites above BG2; a bitmap does not, and the hand/carry
 * sprites used to be able to cross the panel — see boxoam_strip_open's own comment).
 * Returns 0 (nothing drawn) if this source can't serve the icon (artless build, or a
 * species this source can't serve) — caller degrades exactly like the OBJ path did. */
int boxoam_icon_blit_clip(int x, int y, int cx0, int cy0, int cx1, int cy1,
                           uint16_t species, uint8_t form, bool egg);

/* MUST-FIX 2 (2026-08-20 review, PC-box party PANEL): hide the box's own cursor hand
 * (OE_HAND) and any carried mon/item (OE_GRAB/OE_CARRY/OE_CITEM) for one frame. These
 * are positioned off the BOX's cursor cell (`cur`), which the panel now covers for up
 * to 2/3 of the grid — render_full()'s own oam_sync() runs first each iteration and
 * positions them normally for the closed-grid case; call this right after it, before
 * boxoam_commit() flushes, to suppress them for as long as the panel is on screen.
 * party_strip_overlay calls oam_sync() once more at its own `out:` label (beside
 * boxoam_strip_close()) to bring them back once the panel closes. */
void boxoam_strip_hide_cursor(void);

/* Set the idle-bob vertical offset (0 or 1 px) applied uniformly to all 30 icon
 * sprites. Pure OAM write — call from the vblank tick. */
void boxoam_set_bob(int dy);

/* Swap the box icons to bob frame 0/1 (the real Gen-3 2-frame pose animation) by
 * DMA-uploading that frame's tiles for every occupied icon. Call from the vblank tick.
 * Returns 1 if it animated, 0 if this box cannot pose-swap (ROM-streamed icons have no
 * frame-1 source in RAM and nowhere to cache one) — callers should fall back to
 * boxoam_set_bob so the grid still shows life. */
int boxoam_set_frame(int frame);

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
enum { BOXOAM_POSE_NORMAL = 0, BOXOAM_POSE_REACH = 1, BOXOAM_POSE_BOUNCE = 2 };
void boxoam_hand_pose(int pose);
void boxoam_cursor_dy(int dy);          /* 0 = rest; +8 = dipped onto the cell */
void boxoam_cursor_dxy(int dx, int dy); /* the 6-frame cursor slide (retail #4)  */

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

/* PC-box party PANEL (pdna_box.c's party_strip_overlay): a big framed panel holding
 * SIX party tiles (5 in a right-hand column + 1 offset "slot 1") pops up OVER the box
 * grid, which stays visible and alive behind it. The panel's own chrome (bevel/dither
 * fill/tile borders/CANCEL) is drawn to the BG2 bitmap, same as every other panel on
 * this screen — but BG2 sits at priority 3, the lowest, UNDER every OBJ sprite (see
 * boxoam_enter), so any leftover box icon under the panel's rectangle would show
 * through it. bitmap mode's OBJ tile memory is spent to the exact tile (this file's
 * header: 30 grid icons + hand + region B = 512), so the panel's own icons cannot be
 * drawn as NEW sprites either — there is nothing left to draw them with.
 *
 * boxoam_strip_open(x0, x1) resolves both problems by hiding the grid icons whose cell
 * falls in [x0, x1): every hidden slot is unavailable to the box grid instantly (no
 * OBJ can bleed through the panel there), and up to TWO hidden slots per box row are
 * reserved — the first's now-unused 16-tile region and OAM entry are repointed at
 * whatever boxoam_strip_slot() draws for that row (the column's 5 tiles, one per box
 * row); a SECOND reservation, taken from wherever it's first found across the whole
 * scan (there is no shortage — the panel now hides 4 of the box's 6 columns, so every
 * row has spares), backs boxoam_strip_slot1() for the panel's 6th, offset icon. Grid
 * icons OUTSIDE [x0, x1) are never touched: they stay visible, in place, and still
 * animate/cursor normally. boxoam_strip_close() hands every hidden/reused slot back
 * via restore_slot() — the same machinery boxoam_chunk_end() already uses to uncover a
 * cell — so a slot always comes back exactly as boxoam_load_box() last left it
 * (species, frame, palette bank, selection mark).
 *
 * A full-screen view opened WHILE the panel is up (a warning dialog, the party action
 * menu) still needs its OWN boxoam_suspend()/boxoam_resume() bracket around just that
 * call, same as every other popup in pdna_box.c — those two are pure DISPCNT toggles
 * and never disturb this bookkeeping, so nesting them here is safe. */
void boxoam_strip_open(int x0, int x1);
/* Draw (or clear) the panel's OWN column icon for box row `row` (0..4, party slots
 * 2-6) at OAM position (x, y): species==0 && !egg hides that row's icon (an empty
 * party slot — the "+ add" target). Call every redraw, followed by boxoam_commit()
 * like any other OAM update. A row whose box column had nothing to reuse
 * (boxoam_strip_open found no intersecting grid cell for it) silently draws nothing;
 * that cannot happen at this screen's own measured geometry (every row's column
 * intersects), but degrading to "no icon" rather than an out-of-range OAM/VRAM write
 * is the safe failure if the geometry ever changes.
 *
 * MUST-FIX 5 (2026-08-20 review): pdna_box.c now calls this with species==0 (and the
 * companion boxoam_icon_blit_clip() above for the actual visible icon, a bitmap that
 * CAN be clipped to the tile) UNCONDITIONALLY, every draw — never with a real
 * species/form any more. It still matters: it's what hides a borrowed slot's OWN OBJ
 * so a stale sprite from an earlier iteration/species can never show through. */
void boxoam_strip_slot(int row, int x, int y, uint16_t species, uint8_t form, bool egg);
/* Same contract as boxoam_strip_slot, for the panel's 6th tile: the offset, alone
 * "slot 1" (party index 0), drawn at its own measured position (PDNA_PCP_S1_*,
 * pdna_layout.h) rather than a column row. */
void boxoam_strip_slot1(int x, int y, uint16_t species, uint8_t form, bool egg);
void boxoam_strip_close(void);

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
