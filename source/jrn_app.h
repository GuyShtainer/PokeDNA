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

/* ---- slice 4: a Game Boy image on the SAME engine (design D8) ------------------------------------------------ */
/* Bind the recorder to a resident Game Boy image (32,768+ bytes, edited in place) and open its journal. The image is
 * journaled as 8 regions of 4,096 bytes over its first 32,768 bytes (the RTC tail is never edited); undo/redo patch the
 * bytes raw, checksums included (a step's spans carry them). `key` = gb_journal_key(); `cap` = the retention cap in
 * segments (0 = the engine default, 16; clamped to 16). Never creates files (jrnapp_prepare_key does). */
int  jrnapp_open_gb(ImgRec* r, uint8_t* img, uint64_t key, uint8_t cap, bool write_ok);
/* #308: jrnapp_open_gb + the old-key duty. `legacy` = gb_journal_key_legacy() (0 = none). See jrn_app.c for the mechanism. */
int  jrnapp_open_gb_compat(ImgRec* r, uint8_t* img, uint64_t key, uint64_t legacy, uint8_t cap, bool write_ok);
/* The Game Boy safe-moment work: the ring/tail/spare, plus the redirect when the identity key moved. */
int  jrnapp_prepare_key(ImgRec* r, uint64_t key);
/* Settings > Clear history (see jrn_app.c): delete this save's journal files, reopen empty under `cap`. Files removed or < 0. */
int  jrnapp_clear(ImgRec* r, uint8_t cap);
/* The session the recorder was bound to is over: unbind (writes nothing; the state reads JA_OFF). */
void jrnapp_close(ImgRec* r);

/* Safe moment (load, after a verified exit save): make the ring, the tail segment and the spare exist,
 * write a redirect if the identity changed this session. Returns 0 or a JRN_E_*; a first-fill failure
 * leaves the journal off (state JA_ERROR) so the UI never claims what is not recorded. */
int  jrnapp_prepare(ImgRec* r, const uint8_t* sb2, bool frlg);
/* The engine's cursor and tip (for the load log line only). */
uint32_t jrnapp_cursor(void);
uint32_t jrnapp_tip(void);
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

/* ---- slice 3: undo / redo / the history screen (design D5/D7) ----------------------------------------- */
/* The card's image now sits at the journal's current cursor (a verified save happened): "recorded" steps past it
 * stay "recorded", everything at or below it reads SAVED. Cheap; call after every verified .sav write. */
void jrnapp_mark_saved(void);
/* ONE undo (dir < 0) or redo (dir > 0) through the engine's cursor rule (verify every span, then patch the image
 * through the accessor, then a cursor marker). name = the step it undid/redid. Returns JRN_OK or the engine's
 * refusal (JRN_E_CROSSED = a floor, JRN_E_DIVERGED = the image no longer matches, JRN_E_NOTHING / JRN_NOOP = nothing
 * to do, JRN_E_ARG = the journal is not usable, JRN_E_STATE = a scope is open). Nothing is patched unless 0. On
 * JRN_E_CROSSED `name` is the FLOOR's step (undo: the step at the cursor; redo: the crossed step redo stops before). The
 * CALLER re-derives its decoded copies and sets image-dirty. */
int  jrnapp_step(int dir, char name[25]);

/* #303: the chord's press. A swap is two journal steps (two drops); when the step at the cursor (undo) or the next step
 * toward the tip (redo) is the second half of a swap whose first half is its parent, BOTH undo/redo together (name "Swap"),
 * all-or-nothing, so one press never strands the image on the half-swap state. Gen-3 only (a Game Boy swap is refused
 * outright). Same returns/name contract as jrnapp_step; History and jump stay per-step. The predicate is in jrn_app.c. */
int  jrnapp_step_pair(int dir, char name[25]);

typedef struct JaHist {
  uint32_t seq;
  uint8_t  crossed;    /* a floor: undo/redo cannot pass it                                   */
  uint8_t  at_cursor;  /* the image sits exactly here                                         */
  uint8_t  ahead;      /* newer than the cursor: undone, redoable                             */
  uint8_t  saved;      /* at or below the step the CARD holds (else only "recorded")          */
  char     name[25];            /* a swap's halves read "Box move 1/2" / "Box move 2/2" (#303) */
} JaHist;
/* The current branch newest-first (tip down the parent chain), up to `max` rows. *more = 1 when older steps exist
 * past the window; *floor_hit = 1 when the chain ended at a compacted (retired) parent. Returns the row count
 * (0 = nothing recorded / the journal is not usable). Other branches are NOT enumerated (the engine has no
 * children walk): only the chain the redo tip sits on is listed. */
int  jrnapp_history(JaHist* rows, int max, int* more, int* floor_hit);
/* Undo/redo along that branch until the cursor sits on `target` (a row's seq; 0 = before the first step),
 * stopping at a floor. *moved = steps applied; `stop` = the crossed step's name when a floor stopped it. Returns 0
 * on arrival or the JRN_E_* that stopped the chain. */
int  jrnapp_jump(uint32_t target, char stop[25], int* moved);

#endif /* JRN_APP_H */
