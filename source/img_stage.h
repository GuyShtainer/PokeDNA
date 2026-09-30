/* SPDX-License-Identifier: GPL-3.0-or-later
 * img_stage.h -- BACKLOG #234: the pure-C body of the staging funnel.
 *
 * pdna_main.c's app_stage_sections()/app_mark_pc_dirty()/app_save_finalize fold are thin
 * wrappers over these, so tests/host_imgstage_test.c pins the REAL logic (not a copy) on the
 * PC. tonc/FatFs-free: only gen3_save + img_flags + journal (itself pure C).
 *
 * SLICE 2 adds the RECORDER (ImgRec): the funnel diffs every staged block against g_save and hands
 * the difference to the journal as ONE step per scope (jrn_step_begin/region/end), then writes the
 * sections. A SCOPE (img_scope_open/close) groups every stage of one user action -- a box drop, a
 * screen commit -- into one step: inside a scope the funnel DEFERS (it records which sections are
 * ahead and the buffer that holds their new bytes; g_save is written when the outermost scope
 * closes, one diff and one write per section however often it was staged). That deferral is also
 * the #300 fix: a swap used to stage the 9 PC sections twice (the place, then clear_origin).
 * The deferred pointers are ONLY ever the app's static g_sb2/g_sb1/g_pc, never a stack buffer. */
#ifndef IMG_STAGE_H
#define IMG_STAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "img_flags.h"
#include "journal.h"

/* Recording state, as far as the UI may honestly say it (slice 3 reads it). */
enum {
  IREC_OFF = 0,      /* the journal never opened (Everdrive / read-only / disabled / foreign / error) */
  IREC_OK = 1,       /* every staged step so far is in the journal (pending or on disk)              */
  IREC_GAP = 2,      /* at least one step could NOT be recorded (too big, out-of-funnel divergence)  */
  IREC_STOPPED = 3   /* a flush failed its verify: recording stopped for this session                */
};

typedef struct ImgRec {
  int          (*flush)(void);  /* app hook: rumble-paused jrn_flush + logging; 0 = ok. FIRST FIELD ON PURPOSE:
                                 * tools/stack_edges.txt declares `ImgRec.flush @0` and the guard reads the offset
                                 * off this header, which cannot evaluate JRN_NREG_MAX for the fields below. */
  Jrn*           j;             /* NULL = not recording                                              */
  JrnImage       img;           /* the accessor over g_save (jrn_recompute after a divergence)       */
  const char*    name;          /* one-shot step name for the next step (static ASCII), NULL = default */
  const uint8_t* blk[JRN_NREG_MAX];   /* deferred: the buffer holding each pending section's new bytes */
  uint16_t       mask;          /* bit id set = section id is deferred                               */
  uint8_t        depth;         /* open scopes                                                      */
  uint8_t        pc_deferred;   /* this scope raised pc_unstaged: the close clears it                */
  uint8_t        state;         /* IREC_*                                                            */
  uint16_t       epoch, epoch_seen;   /* crossed epoch: bumped by every cross-file op (D6)           */
  uint16_t       lost;          /* steps NOT recorded this session                                   */
  int            last_rc;       /* the last non-OK journal result (for the caller's log line)        */
  const char*    last_what;     /* ... and what it was doing                                         */
} ImgRec;

/* Copy `block` (sections lo..hi, 3,968 B each, addressed by section ID) into `save`'s slot via
 * gen3_write_full_section and mark the image dirty; with a recorder the difference becomes a journal
 * step first (deferred to the scope's close while a scope is open). Returns false (and touches
 * nothing) on a NULL argument or a bad section range. `r` may be NULL (no journal). */
bool img_stage_sections(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, int sect_lo, int sect_hi,
                        const uint8_t* block);

/* A PC box edit reached `pc` (30 boxes' worth, sections 5..13). When `can_stage` the whole PC
 * is staged into `save` at once (the eager stage at the drop; deferred inside a scope); otherwise
 * only pc_unstaged is recorded. Either way the image is dirty and a PC edit is pending. */
void img_pc_edited(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, const uint8_t* pc, bool can_stage);

/* The finalize fold: stage `pc` into `save` ONLY if it is genuinely ahead (pc_unstaged), then
 * clear that flag. Returns true iff it folded. */
bool img_fold_pc(ImgFlags* f, ImgRec* r, uint8_t* save, int slot, const uint8_t* pc);

/* ---- scopes (one step per user action) ------------------------------------------------------ */
void img_scope_open(ImgRec* r, const char* name);
/* Close one scope; the OUTERMOST close applies everything deferred (journal step, then the
 * sections). Returns true when nothing was pending or the apply staged every deferred section. */
bool img_scope_close(ImgFlags* f, ImgRec* r, uint8_t* save, int slot);
/* Apply whatever is deferred NOW without changing the depth (finalize's defence in depth). Returns
 * true when something was pending (the caller logs a BUG line: a write ran inside a scope). */
bool img_scope_flush(ImgFlags* f, ImgRec* r, uint8_t* save, int slot);
/* Bump the crossed epoch: the NEXT recorded step is marked crossed (an undo/redo/re-apply floor). */
void img_rec_cross(ImgRec* r);
/* Set the one-shot name of the next step (static ASCII string, at most 24 chars). */
void img_rec_name(ImgRec* r, const char* name);

#endif /* IMG_STAGE_H */
