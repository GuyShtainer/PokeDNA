/* SPDX-License-Identifier: GPL-3.0-or-later
 * jrn_app.h -- the app side of the #234 undo journal (slice 2 of docs/briefs/234-UNDO-DESIGN.md).
 *
 * journal.c is the pure engine; img_stage.c is the pure funnel that diffs into it. THIS file owns
 * what touches the machine: the one Jrn instance (EWRAM), the FatFs binding, the image accessor over
 * g_save, the rumble-paused flush, the load anchor and the re-apply chain. pdna_main.c reaches it
 * through the thin app_journal_* wrappers (the guard allows g_save writes only there).
 *
 * WHAT THE UI MAY SAY. Only jrnapp_state() decides. "recorded" is honest only for JA_OK; every other
 * state (off, gap, stopped, foreign, error) must never be worded as recorded (design D1/D3). */
#ifndef JRN_APP_H
#define JRN_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "img_stage.h"

enum {
  JA_OFF = 0,        /* never opened: Everdrive / hack / disabled / no card / no Gen-3 image          */
  JA_OK = 1,         /* recording, every step so far is in the journal (pending or on disk)            */
  JA_GAP = 2,        /* recording, but at least one step could not be recorded                         */
  JA_STOPPED = 3,    /* a flush failed its verify: recording stopped for this session                  */
  JA_FOREIGN = 4,    /* a journal this build does not understand (JRN_E_VERSION): read-only, untouched */
  JA_ERROR = 5       /* the open hit a card error: the journal is not used this session                */
};

/* Bind the recorder to the image and open the journal for this save. Call at load, BEFORE any reconcile
 * stages a step. `sb2` = SaveBlock2 (the identity for the key); `write_ok` = app_can_edit(). Never
 * creates files: the first fill is jrnapp_prepare(). Returns the JA_* state. */
int  jrnapp_open(ImgRec* r, uint8_t* save, int slot, const uint8_t* sb2, bool frlg, bool write_ok);
int  jrnapp_state(const ImgRec* r);

/* Safe moment (load, after a verified exit save): make the ring, the tail segment and the spare exist,
 * write a redirect if the identity changed this session. Returns 0 or a JRN_E_*; a first-fill failure
 * leaves the journal off (state JA_ERROR) so the UI never claims what is not recorded. */
int  jrnapp_prepare(ImgRec* r, const uint8_t* sb2, bool frlg);
/* True while the FIRST fill of the ring is still owed (the caller shows one status line before it). */
bool jrnapp_first_fill_owed(void);

/* Rest-point flush (screen exit, box-screen leave, before any SAVE): rumble-paused, content-verified by
 * the engine. Returns 0 (or nothing to do) / a JRN_E_*; a stop is folded into the state. */
int  jrnapp_flush(void);
/* Flush only when the engine asked for it (pending buffer nearly full): the idle-frame poll. */
void jrnapp_idle(void);
/* Emit the events the hooks recorded, from a shallow call site (the funnel and the flush hook never log). */
void jrnapp_log_events(ImgRec* r);

/* ---- the load-time re-apply offer (design D1/D6) ------------------------------------------------ */
/* Steps the journal holds that the loaded image does not: 0 = no offer. *avail = how many of them come
 * BEFORE the first crossed record (re-apply stops there); *stop = that crossed step's name ("" = none). */
uint32_t jrnapp_offer(uint32_t* avail, char stop[25]);
/* Re-apply up to `avail` steps through the cursor rule, patching the image via the accessor. Returns
 * the number applied, or a negative JRN_E_* (nothing half-applied: the engine verifies each step
 * against the image before it touches a byte). */
int  jrnapp_reapply(void);
/* The user declined: write the discarded marker (and flush it). */
void jrnapp_decline(void);
/* The exit-B discard put the card's image back: re-anchor and mark the thrown-away steps discarded. */
void jrnapp_after_discard(ImgRec* r);

/* The name of the step at seq (for the offer's "stops at <step>" line): "" when unknown. */
void jrnapp_step_name(uint32_t seq, char out[25]);

#endif /* JRN_APP_H */
