/* SPDX-License-Identifier: GPL-3.0-or-later
 * img_flags.h -- BACKLOG #234 slice 0: the dirty-flag state machine of the open Gen-3 image.
 *
 * PURE C (stdbool only; no tonc/FatFs) so tests/host_imgflags_test.c can pin it on the PC.
 *
 * The old single flag g_pc_dirty meant three different things at once. It is now:
 *   pc_unstaged  g_pc holds bytes g_save does NOT decode to (an edit reached g_pc but has not
 *                been staged into g_save). Gates the finalize fold of g_pc into g_save. While
 *                the EWRAM arena lends g_pc out (icons / tileset) g_pc holds FOREIGN bytes, so
 *                folding it on any write would put them in every box -- pc_unstaged is never
 *                set by that loan, only by a PC edit that could not stage.
 *   image_dirty  g_save differs from the card (anything was staged and not yet written).
 *                Gates the exit prompt.
 *   pc_moved     slice-0 compatibility only: a PC box edit is pending (== the old g_pc_dirty).
 *                It keeps the arena refusal and the "SAVE FIRST" message byte-identical to
 *                today, because with eager staging pc_unstaged alone would silently drop them.
 *                It retires together with those two prompts.
 * Nothing here touches memory it does not own; every function tolerates a NULL pointer.
 */
#ifndef IMG_FLAGS_H
#define IMG_FLAGS_H

#include <stdbool.h>

typedef struct {
  bool pc_unstaged;
  bool image_dirty;
  bool pc_moved;
} ImgFlags;

/* Every g_save section write (the funnel) marks the image dirty. */
static inline void imgf_staged(ImgFlags* f) { if (f) f->image_dirty = true; }

/* A PC box edit happened. `staged` = it was pushed into g_save at once (the normal case);
 * false = it could not be (no parsed save / arena lent out), so g_pc is now ahead of g_save. */
static inline void imgf_pc_edited(ImgFlags* f, bool staged) {
  if (!f) return;
  f->image_dirty = true;
  f->pc_moved    = true;
  if (!staged) f->pc_unstaged = true;
}

/* g_pc was just pushed into g_save (the finalize fold, or an explicit whole-PC stage). */
static inline void imgf_pc_folded(ImgFlags* f) { if (f) f->pc_unstaged = false; }

/* The whole image reached the card (verified write) or was thrown away: nothing pending. */
static inline void imgf_clear(ImgFlags* f) {
  if (!f) return;
  f->pc_unstaged = false; f->image_dirty = false; f->pc_moved = false;
}

/* Should app_save_finalize() fold g_pc into g_save before the checksum pass? Only when g_pc is
 * genuinely ahead of g_save -- NEVER merely because the image is dirty. */
static inline bool imgf_fold_needed(const ImgFlags* f) { return f ? f->pc_unstaged : false; }

/* May the arena take g_pc over? (slice 0: refuse while any PC edit is pending, as today.) */
static inline bool imgf_arena_ok(const ImgFlags* f) { return f ? (!f->pc_unstaged && !f->pc_moved) : false; }

/* Does leaving the save need the "save or discard" prompt? Anything staged -- box moves,
 * Day-Care, dex, or a SaveBlock2-only change -- not only PC moves. */
static inline bool imgf_exit_prompt(const ImgFlags* f) { return f ? f->image_dirty : false; }

/* Wording of that prompt: name what is pending. Box moves keep the historic line. */
static inline const char* imgf_exit_line(const ImgFlags* f) {
  return (f && f->pc_moved) ? "Save the moved Pokemon?" : "Save the staged changes?";
}

#endif /* IMG_FLAGS_H */
