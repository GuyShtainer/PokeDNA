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
 *   partial      (z9, D10 ruling 9.2) a mid-session TORN step: the image holds PART of a step. EVERY commit path refuses
 *                while it is set; ONLY a full re-read of the card image clears it (imgf_partial_clear on a successful
 *                discard / a fresh load). imgf_clear deliberately does NOT touch it: a failed discard re-read must stay latched.
 *   (pc_moved, the slice-0 compatibility flag, RETIRED in slice 2: a staged box drop survives the arena
 *   loan because g_pc is re-derived from g_save on release, so the arena gate is pc_unstaged alone.)
 * Nothing here touches memory it does not own; every function tolerates a NULL pointer.
 */
#ifndef IMG_FLAGS_H
#define IMG_FLAGS_H

#include <stdbool.h>

typedef struct {
  bool pc_unstaged;
  bool image_dirty;
  bool partial;       /* D10 ruling 9: a TORN undo/redo/jump left PART of a step in the image; no commit may write it */
} ImgFlags;

/* Every g_save section write (the funnel) marks the image dirty. */
static inline void imgf_staged(ImgFlags* f) { if (f) f->image_dirty = true; }

/* A PC box edit happened. `staged` = it was pushed into g_save at once (the normal case);
 * false = it could not be (no parsed save / arena lent out), so g_pc is now ahead of g_save. */
static inline void imgf_pc_edited(ImgFlags* f, bool staged) {
  if (!f) return;
  f->image_dirty = true;
  if (!staged) f->pc_unstaged = true;
}

/* g_pc was just pushed into g_save (the finalize fold, or an explicit whole-PC stage). */
static inline void imgf_pc_folded(ImgFlags* f) { if (f) f->pc_unstaged = false; }

/* A TORN step: latch the partial image. It is also staged (dirty) so the exit prompt still appears and names it. */
static inline void imgf_partial_set(ImgFlags* f) { if (!f) return; f->partial = true; f->image_dirty = true; }
/* Only after the card image was re-read in full (discard / fresh load). */
static inline void imgf_partial_clear(ImgFlags* f) { if (f) f->partial = false; }
/* May a commit write the image? false while latched. A NULL pointer answers "latched" (never trust nothing). */
static inline bool imgf_partial(const ImgFlags* f) { return f ? f->partial : true; }

/* The whole image reached the card (verified write) or was thrown away: nothing pending. The PARTIAL latch is NOT cleared here. */
static inline void imgf_clear(ImgFlags* f) {
  if (!f) return;
  f->pc_unstaged = false; f->image_dirty = false;
}

/* Should app_save_finalize() fold g_pc into g_save before the checksum pass? Only when g_pc is
 * genuinely ahead of g_save -- NEVER merely because the image is dirty. */
static inline bool imgf_fold_needed(const ImgFlags* f) { return f ? f->pc_unstaged : false; }

/* May the arena take g_pc over? Refused only while g_pc is genuinely AHEAD of g_save (pc_unstaged): a staged drop
 * is already in g_save, and the release re-derives g_pc from it (#234 s2: the pc_moved compat flag retired). */
static inline bool imgf_arena_ok(const ImgFlags* f) { return f ? !f->pc_unstaged : false; }

/* Does leaving the save need the "save or discard" prompt? Anything staged -- box moves,
 * Day-Care, dex, or a SaveBlock2-only change -- not only PC moves. */
static inline bool imgf_exit_prompt(const ImgFlags* f) { return f ? f->image_dirty : false; }

/* Wording of that prompt: it names what is pending -- every staged edit, not only box moves. */
static inline const char* imgf_exit_line(const ImgFlags* f) {
  (void)f;
  return "Save the staged changes?";
}

#endif /* IMG_FLAGS_H */
