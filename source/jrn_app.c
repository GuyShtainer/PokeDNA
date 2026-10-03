/* SPDX-License-Identifier: GPL-3.0-or-later
 * jrn_app.c -- see jrn_app.h. The machine-facing half of the #234 journal wiring. */
#include "jrn_app.h"

#include <string.h>

#include "gen3_save.h"
#include "journal.h"
#include "journal_fs.h"
#include "journal_int.h"   /* jrn_i_locate / jrn_i_src_read: READ-ONLY record access for the #303 swap-pair predicate (the frozen engine is not touched) */
#include "log.h"
#include "rmbl.h"
#include "sys.h"   /* EWRAM_BSS */

#define JA_ROOT "/PokeDNA/journal"
#define JA_GB_NREG  8u        /* slice 4: a Game Boy image = 8 regions ...                         */
#define JA_GB_REGSZ 4096u     /* ... of 4,096 bytes = the first 32,768 bytes (len is always >= that) */

/* Statics (all EWRAM_BSS: IWRAM holds the stack). The Jrn is 768 B on ARM (its pending buffer is the
 * design's 512). */
static Jrn      EWRAM_BSS s_j;
static ImgRec*  EWRAM_BSS s_r;          /* the recorder this session bound (NULL = none)            */
static uint64_t EWRAM_BSS s_key;        /* the key the journal was opened with (redirect compare)   */
static uint8_t  EWRAM_BSS s_state;      /* JA_* for the states the recorder cannot express          */
static uint32_t EWRAM_BSS s_saved;      /* the cursor seq the CARD's image sits on (slice 3: "recorded" vs "SAVED") */
static uint8_t  EWRAM_BSS s_ev_new;     /* an event is waiting for jrnapp_log_events                */
static int      EWRAM_BSS s_ev_rc;
static const char* EWRAM_BSS s_ev_what;
/* NAMED: `static struct {...} EWRAM_BSS x;` puts the attribute on the wrong side and lands in IWRAM (icon_store.c s_is).
 * slot >= 0: a Gen-3 image (regions = section IDs). slot < 0: a FLAT Game Boy image (slice 4): regions are
 * JA_GB_NREG consecutive JA_GB_REGSZ-byte windows of the first 32,768 bytes (the RTC tail is not journaled). */
typedef struct { uint8_t* save; int slot; } JaImg;
static JaImg EWRAM_BSS s_ai;   /* the image accessor's context */

static void ja_event(const char* what, int rc) { s_ev_what = what; s_ev_rc = rc; s_ev_new = 1; }

/* ---- the image accessor: regions are section IDs, spans are inside a section's 3,968 data bytes -- */
static uint8_t* ja_sec(int region) {
  int s;
  if (!s_ai.save || region < 0 || region > 13) return 0;
  s = gen3_find_section(s_ai.save, s_ai.slot, region);
  return s < 0 ? 0 : s_ai.save + (uint32_t)s_ai.slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE;
}

/* The flat (Game Boy) window: region*4096 + off .. + n, all inside the 8 regions. NULL = out of range. */
static uint8_t* ja_flat(int region, uint16_t off, uint16_t n) {
  if (!s_ai.save || s_ai.slot >= 0 || region < 0 || region >= (int)JA_GB_NREG) return 0;
  if ((uint32_t)off + n > JA_GB_REGSZ) return 0;
  return s_ai.save + (uint32_t)region * JA_GB_REGSZ + off;
}

static int ja_get(void* ctx, uint8_t region, uint16_t off, uint8_t* dst, uint16_t n) {
  const uint8_t* sec;
  (void)ctx;
  if (s_ai.slot < 0) {
    sec = ja_flat(region, off, n);
    if (!sec || !dst) return -1;
    memcpy(dst, sec, n);
    return 0;
  }
  sec = ja_sec(region);
  if (!sec || !dst || (uint32_t)off + n > G3_SECTOR_DATA_SIZE) return -1;
  memcpy(dst, sec + off, n);
  return 0;
}

/* A span patch keeps the sector's checksum honest exactly as gen3_write_full_section does (over the
 * 3,968 data bytes), so a re-applied or undone section still verifies. */
static int ja_set(void* ctx, uint8_t region, uint16_t off, const uint8_t* src, uint16_t n) {
  uint8_t* sec;
  uint16_t cs;
  (void)ctx;
  if (s_ai.slot < 0) {                                       /* flat image: the spans carry the checksum bytes themselves */
    sec = ja_flat(region, off, n);
    if (!sec || !src) return -1;
    memcpy(sec, src, n);
    return 0;
  }
  sec = ja_sec(region);
  if (!sec || !src || (uint32_t)off + n > G3_SECTOR_DATA_SIZE) return -1;
  memcpy(sec + off, src, n);
  cs = gen3_checksum(sec, G3_SECTOR_DATA_SIZE);
  sec[G3_OFF_CHECKSUM] = (uint8_t)(cs & 0xFFu);
  sec[G3_OFF_CHECKSUM + 1] = (uint8_t)(cs >> 8);
  return 0;
}

static uint64_t ja_key(const uint8_t* sb2, bool frlg) {
  uint16_t tid = (uint16_t)(sb2[SB2_OFF_TRAINER_ID] | (sb2[SB2_OFF_TRAINER_ID + 1] << 8));
  uint16_t sid = (uint16_t)(sb2[SB2_OFF_TRAINER_ID + 2] | (sb2[SB2_OFF_TRAINER_ID + 3] << 8));
  return jrn_key64(sb2 + SB2_OFF_PLAYER_NAME, 8, tid, sid, sb2[SB2_OFF_GENDER], frlg ? 1u : 0u);
}

static int ja_do_open(ImgRec* r) {
  JrnCfg c;
  memset(&c, 0, sizeof c);
  c.fs = &jrn_fatfs; c.root = JA_ROOT; c.key = s_key;
  if (s_ai.slot < 0) { c.nreg = (uint8_t)JA_GB_NREG; c.reg_size = (uint16_t)JA_GB_REGSZ; }
  else { c.nreg = 14; c.reg_size = G3_SECTOR_DATA_SIZE; }
  c.max_segs = s_j.max_segs;                                 /* 0 = the engine default; set by jrnapp_open from the Settings cap, kept across a re-open */
  c.readonly = 0;
  r->img.ctx = &s_ai; r->img.get = ja_get; r->img.set = ja_set;
  return jrn_open(&s_j, &c, &r->img);
}

static int jrnapp_chain(const char* name, int crossed, const JrnBlk* blk, uint8_t nblk);   /* the funnel's chain hook (below) */

/* The shared tail of both opens: the accessor is bound and s_key is set; open (rumble paused: a torn tail is zeroed in
 * place, an SD write), then bind the recorder. */
static int ja_open_bound(ImgRec* r) {
  int rc;
  rmbl_pause();                                             /* open can zero a torn tail: an SD write */
  rc = ja_do_open(r);
  rmbl_resume();
  if (rc == JRN_E_VERSION) { s_state = JA_FOREIGN; ja_event("open: foreign journal, read-only", rc); return JA_FOREIGN; }
  if (rc != JRN_OK) { s_state = JA_ERROR; ja_event("open failed, journal off", rc); return JA_ERROR; }
  r->j = &s_j;
  s_saved = jrn_cursor(&s_j);                               /* the loaded image IS the card's: its anchor is the saved point */
  r->flush = jrnapp_flush;
  r->chain = jrnapp_chain;
  r->state = IREC_OK;
  s_state = JA_OK;
  return JA_OK;
}

int jrnapp_open(ImgRec* r, uint8_t* save, int slot, const uint8_t* sb2, bool frlg, bool write_ok) {
  if (!r) return JA_OFF;
  memset(r, 0, sizeof *r);
  s_r = r; s_state = JA_OFF; s_ev_new = 0;
  s_ai.save = save; s_ai.slot = slot;
  if (!write_ok || !save || !sb2 || slot < 0) return JA_OFF;   /* Everdrive / hack ROM / no image: never opens, never says "recorded" */
  s_key = ja_key(sb2, frlg);
  return ja_open_bound(r);
}

/* Slice 4: a Game Boy image. `key` = gb_journal_key() of the loaded save's identity; `cap` = the Settings retention cap in
 * segments (0 = the engine default). Everdrive / read-only: never opens. */
int jrnapp_open_gb(ImgRec* r, uint8_t* img, uint64_t key, uint8_t cap, bool write_ok) {
  if (!r) return JA_OFF;
  memset(r, 0, sizeof *r);
  s_r = r; s_state = JA_OFF; s_ev_new = 0;
  s_ai.save = img; s_ai.slot = -1;
  if (!write_ok || !img) return JA_OFF;
  s_key = key;
  s_j.max_segs = cap;                                       /* ja_do_open reads it (the engine clamps to 16, 0 = default) */
  return ja_open_bound(r);
}

/* #308: the Game Boy open with the OLD-key compatibility duty. `key` = gb_journal_key() (new), `legacy` = gb_journal_key_legacy()
 * (the key every earlier build used; equal to `key` for names without a '9', 0 = none). A journal created under the old key
 * must still be found and continued. MECHANISM = REDIRECT (the engine's own .pdr, the same one an identity edit writes), never a
 * copy or a rename: open under `key` (the engine resolves existing redirects, so a save that was adopted once re-opens
 * straight through its redirect); ONLY when that finds no journal at all (ring 0: no directory under the new key and no
 * redirect to one) and the old key differs, probe the old key's journal. It is adopted ONLY when its anchor says THIS image sits
 * on its history (JRN_ANCHOR_MATCH: the last cursor path, or BRANCH: an older record -- the same save lineage); an unrecognized
 * image (NEWROOT = another save that shares the old truncated key), an empty, foreign or unreadable journal is NOT adopted and
 * the journal opens fresh under `key`. An adopted journal keeps s_key = the old directory key, so the next safe moment
 * (jrnapp_prepare_key(r, key)) writes the redirect new -> old through ja_redirect_key, exactly the identity-edit path. */
int jrnapp_open_gb_compat(ImgRec* r, uint8_t* img, uint64_t key, uint64_t legacy, uint8_t cap, bool write_ok) {
  int st;
  if (!r) return JA_OFF;
  st = jrnapp_open_gb(r, img, key, cap, write_ok);
  if (st != JA_OK || !legacy || legacy == key || s_j.ring != 0) return st;   /* found under the new key (or no old key to look for) */
  s_key = legacy;                                                            /* probe the old key's journal over the same image */
  if (ja_open_bound(r) == JA_OK && s_j.ring != 0 && (s_j.anchor == JRN_ANCHOR_MATCH || s_j.anchor == JRN_ANCHOR_BRANCH)) {
    ja_event("gb: continuing the old-key journal (a redirect follows)", 0);
    return JA_OK;
  }
  return jrnapp_open_gb(r, img, key, cap, write_ok);                         /* not this save's: a fresh journal under the new key */
}

/* Settings > Clear history: delete this save's slot files + directory (rumble paused; Omega-only is the caller's gate),
 * then reopen the journal EMPTY under `cap` and keep recording from the image as it is now. Redirect files stay (a later
 * identity that redirects here just starts a new directory). Returns the files removed or a negative JRN_E_*; a failed
 * delete may have removed SOME slot files, so it turns the journal off and reads ERROR (a half ring must never be misread as a
 * history); a failed reopen does the same (never claims what is not recorded). */
int jrnapp_clear(ImgRec* r, uint8_t cap) {
  int n, rc;
  if (!r || !r->j || s_state != JA_OK || s_j.readonly) return JRN_E_ARG;
  rmbl_pause();
  n = jrnfs_clear_key(JA_ROOT, s_j.key);
  rmbl_resume();
  if (n < 0) { ja_event("clear history failed", n); r->j = 0; r->state = IREC_OFF; s_state = JA_ERROR; return n; }
  s_j.max_segs = cap;
  r->state = IREC_OK; r->lost = 0; r->epoch = 0; r->epoch_seen = 0; r->mask = 0; r->depth = 0; r->name = 0;
  rmbl_pause();
  rc = ja_do_open(r);
  rmbl_resume();
  if (rc != JRN_OK) { r->j = 0; r->state = IREC_OFF; s_state = JA_ERROR; ja_event("re-open after clear failed", rc); return rc; }
  s_saved = jrn_cursor(&s_j);
  return n;
}

/* Drop the recorder (the session it was bound to is over): nothing is written, the state reads OFF. */
void jrnapp_close(ImgRec* r) {
  if (r) { r->j = 0; r->state = IREC_OFF; r->depth = 0; r->mask = 0; }
  s_state = JA_OFF;
  s_ai.save = 0; s_ai.slot = 0;
}

int jrnapp_state(const ImgRec* r) {
  if (s_state != JA_OK) return s_state;
  if (!r || !r->j) return JA_OFF;
  if (r->state == IREC_STOPPED) return JA_STOPPED;
  if (r->state == IREC_GAP) return JA_GAP;
  return r->state == IREC_OK ? JA_OK : JA_OFF;
}

/* #339: read-only -- did a failed write/apply-verify latch the engine (j->stopped)? A stopped journal refuses every later verb
 * (JRN_E_STOPPED), so no second undo/redo press can follow. jrnapp_state() does NOT show it on the step path. */
bool jrnapp_stopped(void) { return s_j.stopped != 0; }

uint32_t jrnapp_cursor(void) { return jrn_cursor(&s_j); }
uint32_t jrnapp_tip(void) { return jrn_tip(&s_j); }

bool jrnapp_first_fill_owed(void) { return s_r && s_r->j && s_state == JA_OK && !s_j.ring; }

/* An identity edit (trainer rename / TID / SID / gender) moves the key: a redirect keeps the history. */
static void ja_redirect_key(uint64_t nk) {
  int rc;
  if (nk == s_key) return;
  rc = jrn_redirect_write(&jrn_fatfs, JA_ROOT, nk, s_j.key);
  if (rc == JRN_OK) s_key = nk; else ja_event("redirect write failed", rc);
}

static int ja_prepare(ImgRec* r, uint64_t nk, bool have_key) {
  int rc;
  if (!r || !r->j || s_state != JA_OK || s_j.readonly || s_j.stopped) return 0;
  rmbl_pause();
  rc = jrn_prepare(&s_j);
  if (rc == JRN_OK && have_key) ja_redirect_key(nk);
  rmbl_resume();
  if (rc != JRN_OK) {                                        /* no tail segment: recording cannot work this session */
    ja_event("prepare failed, journal off", rc);
    if (!s_j.tail_seg) { r->j = 0; r->state = IREC_OFF; s_state = JA_ERROR; }
  }
  return rc;
}

int jrnapp_prepare(ImgRec* r, const uint8_t* sb2, bool frlg) {
  return ja_prepare(r, sb2 ? ja_key(sb2, frlg) : 0u, sb2 != 0);
}

/* Slice 4: the same safe-moment work for a Game Boy image; `key` = gb_journal_key() of the CURRENT identity (a trainer
 * rename / ID edit moves it: the redirect keeps the history). */
int jrnapp_prepare_key(ImgRec* r, uint64_t key) { return ja_prepare(r, key, true); }

/* The funnel's chain hook (z9, D10): a step beyond one record, recorded straight to the tail segment as a chain. Rumble paused for the card
 * transfers (this can retire + re-activate the spare, or make one); a card error stops recording exactly as a failed flush does. */
static int jrnapp_chain(const char* name, int crossed, const JrnBlk* blk, uint8_t nblk) {
  int rc;
  if (!s_r || !s_r->j || s_state != JA_OK) return JRN_E_ARG;
  rmbl_pause();
  rc = jrn_chain_record(&s_j, name, crossed, blk, nblk);
  rmbl_resume();
  if (rc != JRN_OK && rc != JRN_NOOP && rc != JRN_E_TOOBIG && rc != JRN_E_FULL && rc != JRN_E_DIVERGED) ja_event("chained step failed", rc);
  if (s_j.stopped) s_r->state = IREC_STOPPED;                 /* the UI must never say "recorded" again */
  return rc;
}

int jrnapp_flush(void) {
  int rc;
  if (!s_r || !s_r->j || s_state != JA_OK) return 0;
  if (!jrn_pending(&s_j)) return 0;
  rmbl_pause();                                              /* no motor on the cart bus mid-transfer */
  rc = jrn_flush(&s_j);
  rmbl_resume();
  if (rc) {
    ja_event("flush failed", rc);
    if (s_j.stopped) s_r->state = IREC_STOPPED;              /* the UI must never say "recorded" again */
  }
  return rc;
}

void jrnapp_idle(void) {
  if (s_r && s_r->j && jrn_flush_wanted(&s_j)) (void)jrnapp_flush();
}

void jrnapp_log_events(ImgRec* r) {
  if (r && r->last_what) {
    log_line("journal: %s (rc %d)", r->last_what, r->last_rc);
    r->last_what = 0;
  }
  if (s_ev_new) {
    s_ev_new = 0;
    log_line("journal: %s (rc %d, state %d)", s_ev_what ? s_ev_what : "?", s_ev_rc, (int)s_state);
  }
}

/* ---- load-time re-apply ----------------------------------------------------------------------------- */
/* #406: the re-apply must never end on the OLDER half of a swap whose newer half is not on the card. A swap is two drops (drop 1 = the step named "Swap", #314a; the
 * displaced mon sits only in the RAM hand until drop 2, a "Box move"/"Party add"): re-applying drop 1 alone deletes the displaced mon from the image. The step named
 * "Swap" (ja_older_eligible: plain, uncrossed, Gen-3) IS the older half by construction -- every pair/chain link below the newest carries it -- so a trailing run of
 * them (the newest `av` re-appliable steps, walked back from the tip) has no partner on the card and is cut. *cut = how many. Game Boy images never carry the name. */
static int ja_older_eligible(const JrnRec* r);
static int ja_cut_tail(uint32_t av, uint32_t total, uint32_t* cut) {
  JrnRec w;
  uint32_t t = jrn_tip(&s_j), idx = total, n = 0;
  int rc;
  *cut = 0;
  if (s_ai.slot < 0 || !av || av > total) return 0;
  while (idx > av) {                                          /* from the tip down to the newest re-appliable step */
    if (!t) return JRN_E_DIVERGED;
    rc = jrn_find(&s_j, t, &w);
    if (rc) return rc;
    t = w.parent; idx--;
  }
  while (n < av) {
    if (!t) return JRN_E_DIVERGED;
    rc = jrn_find(&s_j, t, &w);
    if (rc) return rc;
    if (!ja_older_eligible(&w)) break;
    n++; t = w.parent;
  }
  *cut = n;
  return 0;
}

/* How many trailing half-swap steps the offer is holding back (0 = none). Only meaningful after jrnapp_offer returned 0: the load turns it into the "cut off" note. */
uint32_t jrnapp_cutoff(void) {
  uint32_t av = 0, total = 0, cut = 0;
  if (!s_r || !s_r->j || s_state != JA_OK) return 0;
  if (!jrn_offer(&s_j) && jrn_tip(&s_j) == jrn_cursor(&s_j)) return 0;
  if (jrn_redo_info(&s_j, &av, &total, 0) < 0 || ja_cut_tail(av, total, &cut) != 0) return 0;
  return cut;
}

uint32_t jrnapp_offer(uint32_t* avail, char stop[25]) {
  JrnRec st;
  uint32_t av = 0, total = 0, cut = 0;
  int rc;
  if (avail) *avail = 0;
  if (stop) stop[0] = 0;
  if (!s_r || !s_r->j || s_state != JA_OK) return 0;
  if (!jrn_offer(&s_j) && jrn_tip(&s_j) == jrn_cursor(&s_j)) return 0;   /* an UNDONE tail (cursor < tip) is offered too */
  memset(&st, 0, sizeof st);
  rc = jrn_redo_info(&s_j, &av, &total, &st);
  if (rc < 0 || !total) return 0;
  if (ja_cut_tail(av, total, &cut) != 0) cut = av;                  /* a read fault: offer nothing rather than a half swap */
  av -= cut; total -= cut;
  if (!total) return 0;
  if (avail) *avail = av;
  if (stop && rc == 1) { memcpy(stop, st.name, 24); stop[24] = 0; }
  return total;
}

int jrnapp_reapply(void) {
  uint32_t av = 0, total = 0, i, cut = 0;
  int rc, n = 0, retried = 0;
  if (!s_r || !s_r->j || s_state != JA_OK) return JRN_E_ARG;
  rc = jrn_redo_info(&s_j, &av, &total, 0);
  if (rc < 0) return rc;
  if (ja_cut_tail(av, total, &cut) != 0) cut = av;            /* #406: never re-apply a swap's older half without its partner (a fault: nothing) */
  av -= cut;
  for (i = 0; i < av; i++) {
    rc = jrn_redo(&s_j, &s_r->img, 0);
    if (rc == JRN_E_FULL && !retried) { retried = 1; (void)jrnapp_flush(); rc = jrn_redo(&s_j, &s_r->img, 0); }
    if (rc == JRN_E_TORN) {                                  /* a chained step failed AND its rollback failed (z9): the image holds part of it */
      img_rec_cross(s_r);                                    /* the next recorded step is a floor: the offer never re-applies across it */
      ja_event("re-apply: a chain's rollback failed, image PARTIAL", rc);
      return JRN_E_TORN;                                     /* NOT a count: the caller re-derives its copies AND says the image is partial */
    }
    if (rc != JRN_OK) { ja_event("re-apply stopped", rc); break; }
    n++;
  }
  if (cut && (uint32_t)n == av) jrnapp_decline();             /* #406: the held-back half is thrown away (marked discarded): redo / the next load never offers a lone half */
  return (n == 0 && rc < 0) ? rc : n;
}

void jrnapp_decline(void) {
  int rc;
  if (!s_r || !s_r->j || s_state != JA_OK) return;
  rc = jrn_mark_discarded(&s_j);
  if (rc == JRN_E_FULL) { (void)jrnapp_flush(); rc = jrn_mark_discarded(&s_j); }
  if (rc != JRN_OK) { ja_event("discard marker failed", rc); return; }
  (void)jrnapp_flush();
}

void jrnapp_after_discard(ImgRec* r) {
  int rc;
  if (!r || !r->j || s_state != JA_OK || s_j.stopped) return;
  if (r->depth) r->depth = 0;
  r->mask = 0;
  (void)jrnapp_flush();                                      /* the thrown-away steps stay in the history, marked */
  if (s_j.stopped) return;
  rmbl_pause();
  rc = ja_do_open(r);                                        /* re-anchor against the restored card image */
  rmbl_resume();
  if (rc != JRN_OK) { ja_event("re-open after discard failed", rc); r->j = 0; r->state = IREC_OFF; s_state = JA_ERROR; return; }
  s_saved = jrn_cursor(&s_j);                                /* the card holds the restored image */
  jrnapp_decline();
}

void jrnapp_step_name(uint32_t seq, char out[25]) {
  JrnRec rec;
  out[0] = 0;
  if (!s_r || !s_r->j) return;
  if (jrn_find(&s_j, seq, &rec) == 0) { memcpy(out, rec.name, 24); out[24] = 0; }
}

/* ---- slice 3: undo / redo / history (design D5/D7) ----------------------------------------------------- */
void jrnapp_mark_saved(void) {
  if (s_r && s_r->j && s_state == JA_OK) s_saved = jrn_cursor(&s_j);
}

/* One undo (dir < 0) or redo (dir > 0) through the engine's cursor rule. A full pending buffer flushes once and
 * retries. Returns JRN_OK (name = the step, "" when unknown) or a JRN_E_* / JRN_NOOP: nothing was patched unless 0. */
int jrnapp_step(int dir, char name[25]) {
  JrnRec rec;
  int rc, retried = 0;
  if (name) name[0] = 0;
  if (!s_r || !s_r->j || s_state != JA_OK) return JRN_E_ARG;
  if (s_r->depth) return JRN_E_STATE;                        /* a scope is open: a staged copy is mid-edit */
  memset(&rec, 0, sizeof rec);
  /* DEVIATION FROM THE BRIEF (isolated in its own commit, one line to revert): the engine pops a still-PENDING record on
   * undo with no SD I/O, which throws the redo target away -- and a swap is TWO drops, so with the first drop on disk and
   * the second pending an undo/redo round trip lands on the half-swap (the displaced mon in nobody's hands, 29/30) with
   * no way to redo the second half. One verified flush before an undo keeps every step redoable. */
  if (dir < 0 && jrn_pending(&s_j)) (void)jrnapp_flush();
  for (;;) {
    rc = dir < 0 ? jrn_undo(&s_j, &s_r->img, &rec) : jrn_redo(&s_j, &s_r->img, &rec);
    if (rc == JRN_E_FULL && !retried) { retried = 1; (void)jrnapp_flush(); continue; }
    break;
  }
  /* #319 RULING: recording while the PARTIAL latch is set is SAFE AND WANTED -- do NOT refuse records on a latched image.
   * (1) the epoch crosses on TORN (here and in jrnapp_reapply) so the NEXT record is a floor: offer/redo stop before it
   * (journal_undo.c); (2) re-apply memcmp-verifies every byte against the image it was recorded over (span_walk,
   * journal_undo.c:49-52) and stops DIVERGED, never patching; (3) GB relatches its baseline (gb_relatch), so only the user's
   * own edit is recorded. Refusing would strip undo coverage from the user's post-partial edits for no safety gain. The
   * latch clears only on a FULL-image discard (app_discard_staged's short-read predicate, #319) or a fresh load. */
  if (rc == JRN_E_TORN) {                                    /* z9: a chained undo/redo failed part way and its rollback failed too: the image is PARTIAL */
    img_rec_cross(s_r);                                      /* floor: the next recorded step is crossed, so a later re-apply stops before it */
    ja_event(dir < 0 ? "undo: chain rollback failed, image PARTIAL" : "redo: chain rollback failed, image PARTIAL", rc);
    if (name) memcpy(name, "partial step", 13);              /* #303's contract: JRN_OK + a distinct name, the caller re-derives its copies */
    return JRN_OK;
  }
  if (rc == JRN_OK && name) { memcpy(name, rec.name, 24); name[24] = 0; }
  if (rc == JRN_E_CROSSED && name) {                         /* name the floor: undo stops AT the cursor's step, redo BEFORE the next crossed one */
    if (dir < 0) jrnapp_step_name(jrn_cursor(&s_j), name);
    else {
      uint32_t av = 0, tot = 0;
      if (jrn_redo_info(&s_j, &av, &tot, &rec) == 1) { memcpy(name, rec.name, 24); name[24] = 0; }
    }
  }
  if (rc != JRN_OK && rc != JRN_NOOP) ja_event(dir < 0 ? "undo refused" : "redo refused", rc);
  return rc;
}

/* ---- #303: a swap is two journal steps; the chords treat the pair as one -------------------------------------------
 * THE PREDICATE (derived from what the recorder writes, not guessed). A swap = two drops (pdna_box.c drop_held, SWAP tail):
 *   drop 1  step N  : the held mon X lands on the occupied PC slot B (slot B: Y -> X), the source slot A is cleared
 *                     (A: X -> 0), the displaced Y goes into the RAM hand (it is in NO image);
 *   drop 2  step N+1: Y is placed into an EMPTY slot C (C: 0 -> Y) -- by the A press (a "Box move" scope) or by the B put-
 *                     away (unscoped; the funnel's default name for a PC-only mask is also "Box move").
 * Both steps are named "Box move" and carry no other name or scope tag, so the NAME alone cannot pair them (two plain moves
 * in a row look identical). The SPANS can: step N+1 is a swap's second half iff, in the PC regions (sections 5..13,
 * 80-byte mon slots from byte 4 of the reassembled PC buffer):
 *   (1) N+1 is a plain step (not crossed), named "Box move", and N is its parent, also plain and "Box move";
 *   (2) N+1 touches exactly ONE mon slot, every BEFORE byte there is zero (an empty slot) and nothing outside the mon area;
 *   (3) N has exactly ONE REPLACED slot (a slot whose spans hold both a non-zero before byte and a non-zero after byte --
 *       a plain move into an empty slot has no such slot), and touches at most 4 slots;
 *   (4) wherever both are known (a byte inside a span of both), N's BEFORE bytes of the replaced slot equal N+1's AFTER bytes
 *       of its slot, with zero mismatches and at least JA_MATCH_MIN bytes compared.
 * Bytes outside every span are UNKNOWN (never assumed). The chord pairs only along the parent chain, so a floor between the
 * halves (a crossed record) fails (1) and the press behaves exactly as before. Gen-3 only: a Game Boy image has no 80-byte
 * slots and its swaps are refused outright (the dialog says why), so a GB session never pairs. */
#define JA_PC_FIRST    5u
#define JA_PC_LAST    13u
#define JA_MON_BASE    4u
#define JA_MON_BYTES  80u
#define JA_MON_AREA   (14u * 30u * JA_MON_BYTES)
#define JA_PAIR_SLOTS  4u
#define JA_MATCH_MIN  48u
#define JA_REDO_HOPS  64u   /* redo walks tip -> cursor to find the next two steps; farther than this = no pairing, a plain redo */

typedef struct { uint8_t region; uint16_t off, len, before, after; } JaSpan;
typedef struct { int32_t slot; uint8_t ok; uint8_t after[JA_MON_BYTES]; uint8_t known[JA_MON_BYTES / 8u + 2u]; } JaSig;
/* EWRAM_BSS: the record read once (<= 512 B) + the newer half's signature (~100 B). No stack, no IWRAM. */
static uint8_t EWRAM_BSS s_rec[JRN_REC_MAX] __attribute__((aligned(4)));   /* aligned: the History tree reads JrnKid / u32 views of it (#304) */
static JaSig   EWRAM_BSS s_sig;

static int ja_rec_load(const JrnSrc* src, const JrnRec* r) {
  if (!src || !r || r->len < JRN_REC_MIN || r->len > JRN_REC_MAX) return -1;
  return jrn_i_src_read(&s_j, src, 0, s_rec, r->len);
}

/* The next span of the record in s_rec: 0 ok, -1 malformed (never trust a length that leaves the record). */
static int ja_span_at(const JrnRec* r, uint16_t* pos, JaSpan* sp) {
  uint16_t end = (uint16_t)(r->len - 4u);
  if (*pos + JRN_SPAN_HDR > end) return -1;
  sp->region = s_rec[*pos];
  sp->off = jrn_rd16(s_rec + *pos + 2u);
  sp->len = jrn_rd16(s_rec + *pos + 4u);
  sp->before = (uint16_t)(*pos + JRN_SPAN_HDR);
  sp->after = (uint16_t)(sp->before + sp->len);
  if ((uint32_t)sp->after + sp->len > end) return -1;
  *pos = (uint16_t)(sp->after + sp->len);
  return 0;
}

/* Byte i of a span -> 0 = not a PC region (ignored), 1 = a mon byte (*slot, *q set), 2 = a PC byte outside the mon area. */
static int ja_mon_at(const JaSpan* sp, uint16_t i, uint32_t* slot, uint8_t* q) {
  uint32_t p;
  if (sp->region < JA_PC_FIRST || sp->region > JA_PC_LAST) return 0;
  p = (uint32_t)(sp->region - JA_PC_FIRST) * G3_SECTOR_DATA_SIZE + sp->off + i;
  if (p < JA_MON_BASE || p >= JA_MON_BASE + JA_MON_AREA) return 2;
  p -= JA_MON_BASE;
  *slot = p / JA_MON_BYTES; *q = (uint8_t)(p % JA_MON_BYTES);
  return 1;
}

/* A record that may be a pair's half: a plain step (not crossed, not a chain head: aux != 0 holds only part of the spans, D10) on a Gen-3 image.
 * The NEWER half is named "Box move" (the drop-2 scope or the unscoped put-away default); the OLDER half carries the name drop_held's SWAP tail
 * gives it ("Swap", #314a) -- the name that separates a real swap from two same-shaped unscoped commits (the R1 mislabel). */
static int ja_plain_step(const JrnRec* r) {
  return s_ai.slot >= 0 && r->kind == JRN_KIND_STEP && !r->aux && !r->crossed;
}
/* #320: the NEWER (last) half is a "Box move" (a mon into an empty PC slot) OR a "Party add" (the displaced mon landing in the PARTY: pdna_main.c's party_place_held
 * ADD arm names that step, only that tail does). The name picks the signature maker: a Party add has no empty-slot rule and no PC slot. */
static int ja_party_step(const JrnRec* r) { return ja_plain_step(r) && strcmp(r->name, "Party add") == 0; }
static int ja_newer_eligible(const JrnRec* r) { return ja_plain_step(r) && (strcmp(r->name, "Box move") == 0 || strcmp(r->name, "Party add") == 0); }
static int ja_older_eligible(const JrnRec* r) { return ja_plain_step(r) && strcmp(r->name, "Swap") == 0; }

/* From the record in s_rec (the NEWER half): fill s_sig. Returns s_sig.ok = "this step adds one mon into ONE slot" -- an EMPTY one when need_empty (a swap's
 * last half), any one for a chained Swap (#314b: the displaced mon dropped on another occupied slot; the replaced slot's own pairing is ja_older_pairs's job). */
static int ja_sig_make(const JrnRec* r, int need_empty) {
  JaSpan sp;
  uint16_t pos = JRN_REC_HDR, i;
  uint32_t slot = 0;
  uint8_t sn, q = 0;
  int k;
  memset(&s_sig, 0, sizeof s_sig);
  s_sig.slot = -1; s_sig.ok = 1;
  for (sn = 0; sn < r->nspans; sn++) {
    if (ja_span_at(r, &pos, &sp)) { s_sig.ok = 0; return 0; }
    for (i = 0; i < sp.len; i++) {
      k = ja_mon_at(&sp, i, &slot, &q);
      if (k == 0) continue;
      if (k == 2) { s_sig.ok = 0; return 0; }
      if (s_sig.slot < 0) s_sig.slot = (int32_t)slot;
      else if ((uint32_t)s_sig.slot != slot) { s_sig.ok = 0; return 0; }
      if (need_empty && s_rec[sp.before + i] != 0) { s_sig.ok = 0; return 0; }   /* not an empty slot (the final Box move; a chained Swap replaces an occupied one) */
      s_sig.after[q] = s_rec[sp.after + i];
      s_sig.known[q >> 3] = (uint8_t)(s_sig.known[q >> 3] | (1u << (q & 7u)));
    }
  }
  if (s_sig.slot < 0) s_sig.ok = 0;
  return s_sig.ok;
}

/* #320: the signature of a PARTY landing (the record in s_rec, named "Party add"): the displaced mon's 100-byte party record, whose first 80 bytes ARE its box record
 * (box_to_party widens the box record; the prefix is byte-identical -- host-checked over species 1..411). Span-located, NO game knowledge: the landing's ONE SaveBlock1
 * span of >= 80 added bytes carries the record (the party count byte is its own 1-byte span, the dex flags sit in section 0), and its FIRST 80 added bytes are the
 * signature (all 80 known). A landing that touches a PC section, or holds two such spans, is not this shape. The empty-slot rule does NOT apply here (a stale party slot
 * may hold bytes); the Box move path keeps it through ja_sig_make(r, 1). */
#define JA_SB1_FIRST 1u
#define JA_SB1_LAST  4u
#define JA_PARTY_PREFIX JA_MON_BYTES
static int ja_sig_party(const JrnRec* r) {
  JaSpan sp;
  uint16_t pos = JRN_REC_HDR, i;
  uint8_t sn;
  int found = 0;
  memset(&s_sig, 0, sizeof s_sig);
  s_sig.slot = -1;
  for (sn = 0; sn < r->nspans; sn++) {
    if (ja_span_at(r, &pos, &sp)) return 0;
    if (sp.region >= JA_PC_FIRST && sp.region <= JA_PC_LAST) return 0;           /* a landing never touches the PC */
    if (sp.region < JA_SB1_FIRST || sp.region > JA_SB1_LAST || sp.len < JA_PARTY_PREFIX) continue;
    if (found) return 0;
    found = 1;
    for (i = 0; i < JA_PARTY_PREFIX; i++) {
      s_sig.after[i] = s_rec[sp.after + i];
      s_sig.known[i >> 3] = (uint8_t)(s_sig.known[i >> 3] | (1u << (i & 7u)));
    }
  }
  if (!found) return 0;
  s_sig.slot = -2; s_sig.ok = 1;
  return 1;
}
/* The signature of the NEWEST half of a group, by its name. */
static int ja_sig_newest(const JrnRec* r) { return ja_party_step(r) ? ja_sig_party(r) : ja_sig_make(r, 1); }

/* The record in s_rec is the OLDER half: does it pair with the newer half's signature in s_sig? (predicate 3 + 4) */
static int ja_older_pairs(const JrnRec* r) {
  struct { int32_t slot; uint8_t bnz, anz; } tab[JA_PAIR_SLOTS];
  JaSpan sp;
  uint16_t pos, i;
  uint32_t slot = 0, match = 0;
  uint8_t sn, q = 0, nt = 0, t, nrep = 0, rep = 0;
  int k;
  if (!s_sig.ok) return 0;
  memset(tab, 0, sizeof tab);
  pos = JRN_REC_HDR;
  for (sn = 0; sn < r->nspans; sn++) {
    if (ja_span_at(r, &pos, &sp)) return 0;
    for (i = 0; i < sp.len; i++) {
      k = ja_mon_at(&sp, i, &slot, &q);
      if (k != 1) continue;
      for (t = 0; t < nt && tab[t].slot != (int32_t)slot; t++) {}
      if (t == nt) { if (nt >= JA_PAIR_SLOTS) return 0; tab[nt].slot = (int32_t)slot; nt++; }
      if (s_rec[sp.before + i]) tab[t].bnz = 1;
      if (s_rec[sp.after + i]) tab[t].anz = 1;
    }
  }
  for (t = 0; t < nt; t++) if (tab[t].bnz && tab[t].anz) { nrep++; rep = t; }
  if (nrep != 1u) return 0;
  pos = JRN_REC_HDR;
  for (sn = 0; sn < r->nspans; sn++) {
    if (ja_span_at(r, &pos, &sp)) return 0;
    for (i = 0; i < sp.len; i++) {
      if (ja_mon_at(&sp, i, &slot, &q) != 1 || tab[rep].slot != (int32_t)slot) continue;
      if (!(s_sig.known[q >> 3] & (1u << (q & 7u)))) continue;
      if (s_rec[sp.before + i] != s_sig.after[q]) return 0;                    /* a mismatch refutes the pair outright */
      match++;
    }
  }
  return match >= JA_MATCH_MIN;
}

static void ja_label(char name[25], const char* suffix) {
  size_t n = strlen(name);
  if (n + strlen(suffix) <= 24u) memcpy(name + n, suffix, strlen(suffix) + 1u);
}

static void ja_relabel(char name[25], const char* suffix) {   /* swap the 4-char " x/y" tail a pair label put there */
  size_t n = strlen(name);
  if (n >= 4u && name[n - 4u] == ' ' && name[n - 2u] == '/') name[n - 4u] = 0;
  ja_label(name, suffix);
}

#define JA_GROUP_MAX 3u   /* a press takes at most Swap + Swap + Box move (#314b): a longer chain is taken in groups, never past the cap */

/* #322: the walk's reads. 0 = ok; 1 = the step is simply not a usable half (compacted away, malformed record -- a legitimate "no group"); JRN_E_IO = a read
 * FAULT, which the press must refuse (a fault read as "no group" would take a smaller group with a success toast, on a half state). */
static int ja_walk_locate(uint32_t seq, JrnRec* r, JrnSrc* s) {
  int rc = jrn_i_locate(&s_j, seq, r, s);
  return rc == 0 ? 0 : (rc == JRN_E_IO ? JRN_E_IO : 1);
}
static int ja_walk_load(const JrnSrc* src, const JrnRec* r) {
  int rc;
  if (!src || !r || r->len < JRN_REC_MIN || r->len > JRN_REC_MAX) return 1;
  rc = jrn_i_src_read(&s_j, src, 0, s_rec, r->len);
  return rc == 0 ? 0 : (rc == JRN_E_IO ? JRN_E_IO : 1);
}

/* The swap group whose NEWEST step is `newest` and whose steps all lie on its parent chain: 0 = none, 2 = Swap + Box move (#303), 3 = Swap + Swap + Box move
 * (#314b, only when max >= 3); a negative return is a read fault (JRN_E_IO, #322: the press is refused, nothing moves). Every link is byte-matched (the replaced bytes of the older step == the bytes the newer one adds, zero mismatches, >= JA_MATCH_MIN),
 * so two unrelated steps that merely carry the names never group: a Swap's displaced mon can only be placed by the NEXT drop, and the bytes say it was. One record
 * buffer, read in turn newest -> oldest: each older_pairs() runs against the signature of the step above it BEFORE that signature is replaced.
 * #321: every link must also be ADJACENT in sequence (older.seq + 1 == newer.seq): a real swap is two back-to-back commits, so a step with another branch's
 * records (an orphaned sibling, a cursor marker) between its seq and its parent's is two user actions that merely byte-match, and never pairs. */
static int ja_group_at(uint32_t newest, unsigned max, uint32_t* below) {
  JrnRec rm, rn, rp;
  JrnSrc sm, sn, sp;
  int rc;
  if (below) *below = 0;
  rc = ja_walk_locate(newest, &rm, &sm);
  if (rc) return rc < 0 ? rc : 0;
  if (!ja_newer_eligible(&rm)) return 0;
  rc = ja_walk_load(&sm, &rm);
  if (rc) return rc < 0 ? rc : 0;
  if (!ja_sig_newest(&rm)) return 0;
  if (!rm.parent || rm.parent + 1u != newest) return 0;
  rc = ja_walk_locate(rm.parent, &rn, &sn);
  if (rc) return rc < 0 ? rc : 0;
  if (!ja_older_eligible(&rn)) return 0;
  rc = ja_walk_load(&sn, &rn);
  if (rc) return rc < 0 ? rc : 0;
  if (!ja_older_pairs(&rn)) return 0;
  if (below) *below = rn.parent;                              /* #325: the step an undo of this pair LANDS on */
  if (max < 3u || !rn.parent || rn.parent + 1u != rm.parent) return 2;
  rc = ja_walk_locate(rn.parent, &rp, &sp);
  if (rc) return rc < 0 ? rc : 2;
  if (!ja_older_eligible(&rp)) return 2;
  if (!ja_sig_make(&rn, 0)) return 2;                         /* s_rec still holds rn: its own signature, the chain link's newer side */
  rc = ja_walk_load(&sp, &rp);
  if (rc) return rc < 0 ? rc : 2;
  if (!ja_older_pairs(&rp)) return 2;
  if (below) *below = rp.parent;
  return 3;
}

/* How many steps does the NEXT chord press take (0 = one plain step; < 0 = a read fault, refuse -- #322)? dir < 0: the group at the cursor, newest-first; dir > 0: the group that BEGINS at the first
 * step toward the tip (c1): (c1,c2) when c2 is a Box move, (c1,c2,c3) when c3 is. The caller has flushed (an undo of a pending record pops it with no SD I/O:
 * see jrnapp_step). */
/* #325: *land (an undo only) = 1 when the step the press LANDS on is named "Swap" -- an applied older half whose partner the press undoes -- so the image is on a half swap.
 * It is resolved HERE, before anything moves: a read fault in the lookup is a refusal (JRN_E_IO, nothing moved), never a wrong toast. */
static int ja_land_swap(uint32_t landing, int* land) {
  JrnRec w;
  int rc;
  if (!landing) return 0;                                     /* the base image: nothing applied below */
  rc = jrn_find(&s_j, landing, &w);
  if (rc == JRN_E_IO) return rc;
  if (rc == 0 && strcmp(w.name, "Swap") == 0) *land = 1;
  return 0;
}
static int ja_chord_pair(int dir, int* land) {
  uint32_t t, cur = jrn_cursor(&s_j), p2 = 0, p3 = 0, hops, p1 = 0, below = 0;
  int g, rc;
  *land = 0;
  if (s_ai.slot < 0) return 0;                                /* Game Boy: no 80-byte slots, swaps are refused outright */
  if (dir < 0) {
    if (!cur) return 0;
    g = ja_group_at(cur, JA_GROUP_MAX, &below);
    if (g < 0) return g;
    if (g < 2) {                                              /* a plain step: it lands on its own parent */
      JrnRec w;
      rc = jrn_find(&s_j, cur, &w);
      if (rc == JRN_E_IO) return rc;
      below = rc == 0 ? w.parent : 0;
    }
    rc = ja_land_swap(below, land);
    return rc ? rc : g;
  }
  for (t = jrn_tip(&s_j), hops = 0; t != cur; hops++) {
    JrnRec w;
    if (!t || hops >= JA_REDO_HOPS) return 0;
    p3 = p2; p2 = p1; p1 = t;                                 /* ends with p1 = c1 (just above the cursor), p2 = c2, p3 = c3 */
    rc = jrn_find(&s_j, t, &w);
    if (rc != 0) return rc == JRN_E_IO ? rc : 0;
    t = w.parent;
  }
  if (!p1) return 0;
  if (p3) {
    g = ja_group_at(p3, JA_GROUP_MAX, 0);
    if (g < 0 || g == 3) return g;                            /* (p3,p2,p1) are exactly the three newest-first parent links; a fault refuses */
  }
  if (p2) {
    g = ja_group_at(p2, 2u, 0);
    if (g < 0) return g;
    if (g == 2) return 2;
  }
  return 0;
}

/* #303/#314b: ONE chord press. A swap's halves (ja_chord_pair: two steps, or three for a chained swap) undo/redo together -- one toast, name "Swap" -- so a press
 * does not strand the image on a swap's half state (the limits are listed in jrn_app.h). All-or-nothing: if a later half is refused after earlier ones moved, every
 * one already applied is rolled back through the opposite step; only if THAT fails is the half state reported (name "half a swap", JRN_OK so the caller re-derives
 * its copies; each step is whole, so the image is a CONSISTENT half state, never a torn one). Everything else is exactly jrnapp_step. */
int jrnapp_step_pair(int dir, char name[25]) {
  char n1[25], n2[25];
  int rc, rb = JRN_OK;
  unsigned d, k;
  int g, land = 0;
  if (name) name[0] = 0;
  if (!s_r || !s_r->j || s_state != JA_OK) return JRN_E_ARG;
  if (s_r->depth) return JRN_E_STATE;
  if (dir < 0 && jrn_pending(&s_j)) (void)jrnapp_flush();      /* same single flush jrnapp_step does, BEFORE the lookup (the src pointers must outlive it) */
  g = ja_chord_pair(dir, &land);
  if (g < 0) { ja_event("swap pair: a read fault in the group walk, the press is refused", g); return g; }   /* #322: nothing moved, never a smaller group */
  if (g < 2) {
    rc = jrnapp_step(dir, name);
    /* #323/#325: a whole swap press (g >= 2) names itself "Swap" below. A plain REDO that moved a step carrying the older half's name ("Swap", given only by drop_held's
     * SWAP tail) applied ONE half without its partner (the redo hop cap, a History jump): the image sits on a half swap, so the toast says so (#323). A plain UNDO of a
     * Swap lands pre-swap -- a whole image, plain name -- UNLESS the step it lands on is itself a Swap (a 4+-step chain, a History jump into a mid-chain Swap): that Swap is applied
     * while its partner is undone, a half. ja_chord_pair resolved the landing step's name BEFORE anything moved (`land`; a read fault there refused the press), so the undo marks it
     * the same way (#325). The party landing no longer needs the exception: it is a "Party add" and pairs (#320). History's own labels are untouched. */
    if (rc == JRN_OK && name && strcmp(name, "Swap") == 0 && (dir > 0 || land)) memcpy(name, "Swap (half)", 12);
    return rc;
  }
  rc = jrnapp_step(dir, n1);
  if (rc != JRN_OK) { if (name) memcpy(name, n1, 25); return rc; }   /* nothing moved */
  for (d = 1; d < (unsigned)g; d++) {                                    /* g <= JA_GROUP_MAX: the loop is bounded */
    rc = jrnapp_step(dir, n2);
    if (rc == JRN_OK) continue;
    for (k = 0; k < d && rb == JRN_OK; k++) rb = jrnapp_step(-dir, 0);   /* roll back every step already applied, newest first */
    ja_event("swap pair: a later half refused, the applied ones rolled back", rc);
    if (rb != JRN_OK) {
      ja_event("swap pair: rollback failed, image on the half-swap", rb);
      if (name) { memcpy(name, "half a swap", 12); }
      return JRN_OK;                                           /* the image DID change: the caller must re-derive */
    }
    if (name) memcpy(name, n2, 25);
    return rc;
  }
  if (name) { if (dir < 0 && land) memcpy(name, "Swap (half)", 12); else memcpy(name, "Swap", 5); }   /* #325: a group undo that lands ON a Swap leaves a half */
  return JRN_OK;
}

/* The current branch, newest first: from the tip down the parent chain, at most `max` steps. A swap's two halves are
 * labelled "Box move 1/2" (older) and "Box move 2/2" (newer) when ja_older_pairs() says they are one swap (#303). */
int jrnapp_history(JaHist* rows, int max, int* more, int* floor_hit) {
  JrnRec rec;
  JrnSrc src;
  uint32_t t, cur, hops = 0, prev_parent = 0;
  int n = 0, ahead = 1, saved_seen = 0, sig_ok = 0, sig_kind = 0, gl_prev = 0, gl_head = 0;
  if (more) *more = 0;
  if (floor_hit) *floor_hit = 0;
  if (!rows || max < 1 || !s_r || !s_r->j || s_state != JA_OK) return 0;
  t = jrn_tip(&s_j); cur = jrn_cursor(&s_j);
  while (t && n < max && hops++ < JRN_WALK_MAX) {
    if (jrn_i_locate(&s_j, t, &rec, &src) != 0) { if (floor_hit) *floor_hit = 1; return n; }
    if (t == cur) ahead = 0;                                 /* this row and every older one are IN the image */
    if (t == s_saved) saved_seen = 1;
    rows[n].seq = t;
    rows[n].parent = rec.parent;
    rows[n].kind = JH_STEP; rows[n].open = 0; rows[n].nsib = 0;
    rows[n].crossed = rec.crossed ? 1 : 0;
    rows[n].at_cursor = (t == cur) ? 1 : 0;
    rows[n].ahead = (uint8_t)ahead;
    rows[n].saved = (uint8_t)saved_seen;
    memcpy(rows[n].name, rec.name, 24); rows[n].name[24] = 0;
    if ((ja_older_eligible(&rec) || ja_newer_eligible(&rec)) && ja_rec_load(&src, &rec) == 0) {   /* one record read serves both the older-half test and the next newer-half signature */
      int linked = sig_ok && prev_parent == t && n > 0 && rows[n - 1].seq == t + 1u && ja_older_eligible(&rec) && ja_older_pairs(&rec);
      gl_prev = 0;
      if (linked && sig_kind == 1) {                         /* Swap + Box move */
        ja_label(rows[n - 1].name, " 2/2");
        ja_label(rows[n].name, " 1/2");
        gl_prev = 1;
      } else if (linked && sig_kind == 2 && gl_head) {       /* Swap + Swap + Box move (#314b): relabel the pair as the tail of a triple */
        ja_relabel(rows[n - 2].name, " 3/3");
        ja_relabel(rows[n - 1].name, " 2/3");
        ja_label(rows[n].name, " 1/3");
      }
      sig_kind = 0;
      if (ja_newer_eligible(&rec) && ja_sig_newest(&rec)) sig_kind = 1;
      else if (gl_prev && ja_sig_make(&rec, 0)) sig_kind = 2;   /* the head of a pair may itself be the newer side of a chain link */
      sig_ok = sig_kind != 0;
      prev_parent = rec.parent;
      gl_head = gl_prev;
    } else { sig_ok = 0; sig_kind = 0; gl_head = 0; }
    n++;
    t = rec.parent;
  }
  if (t && more) *more = 1;
  return n;
}

/* ---- #304: the TREE. See jrn_app.h. s_rec is idle here (the branch walk is over), so it holds the three small arrays of jrn_kid_counts
 * (par u32[48] | skip u32[48] | cnt u16[48] = 480 B) and, later, the JrnKid page (16 x 32 B): no new static, no stack. ---- */
#define JA_TREE_ROWS 48
_Static_assert(JA_TREE_ROWS * (4 + 4 + 2) <= (int)JRN_REC_MAX, "the tree's count arrays must fit s_rec");
_Static_assert(16 * sizeof(JrnKid) <= JRN_REC_MAX, "one page of 16 JrnKid must fit s_rec");

static int ja_is_open(const uint32_t* open_forks, int nopen, uint32_t p) {
  int i;
  for (i = 0; i < nopen && i < (int)JA_OPEN_MAX; i++) if (open_forks[i] == p) return 1;
  return 0;
}
/* How many rows a fork point's branch row carries BELOW it: 0 (no fork), 1 (the collapsed summary), or 1 + the siblings shown (opened). */
static int ja_ins(uint16_t nsib, int open, int max) {
  int shown;
  if (!nsib) return 0;
  if (!open) return 1;
  shown = nsib < JA_SIB_SHOW ? (int)nsib : (int)JA_SIB_SHOW;
  if (shown > max - 2) shown = max - 2;                       /* the newest branch row + its summary always fit */
  return shown < 0 ? 1 : 1 + shown;
}

int jrnapp_history_tree(JaHist* rows, int max, int* more, int* floor_hit, const uint32_t* open_forks, int nopen) {
  uint32_t* par = (uint32_t*)(void*)s_rec;
  uint32_t* skip = par + JA_TREE_ROWS;
  uint16_t* cnt = (uint16_t*)(void*)(skip + JA_TREE_ROWS);
  int nb, i, total, pre, mm;
  if (max > JA_TREE_ROWS) max = JA_TREE_ROWS;
  nb = jrnapp_history(rows, max, more, floor_hit);
  if (!open_forks || nopen < 0) nopen = 0;
  if (nb <= 0 || max < 3) return nb;
  for (i = 0; i < nb; i++) { par[i] = rows[i].parent; skip[i] = rows[i].seq; }
  if (jrn_kid_counts(&s_j, par, skip, cnt, (uint8_t)nb) != JRN_OK) { ja_event("history tree: the children walk failed (branch only)", 0); return nb; }
  for (total = nb, i = 0; i < nb; i++) total += ja_ins(cnt[i], ja_is_open(open_forks, nopen, par[i]), max);
  while (total > max && nb > 1) {                              /* the window: older branch rows give way, each with the rows hanging under it */
    nb--;
    total -= 1 + ja_ins(cnt[nb], ja_is_open(open_forks, nopen, par[nb]), max);
    if (more) *more = 1;
  }
  for (pre = total - nb, i = nb - 1; i >= 0; i--) {            /* back to front, in place: a row only ever moves to a HIGHER index */
    int ins = ja_ins(cnt[i], ja_is_open(open_forks, nopen, par[i]), max), dest;
    pre -= ins;
    dest = i + pre;
    if (dest != i) memmove(&rows[dest], &rows[i], sizeof rows[0]);
    if (ins) {
      JaHist* f = &rows[dest + 1];
      memset(f, 0, sizeof *f);
      f->kind = JH_FORK; f->parent = par[i]; f->nsib = cnt[i];
      f->open = ins > 1 ? 1 : 0;
      for (mm = 1; mm < ins; mm++) {                           /* placeholders, filled below */
        JaHist* sb = &rows[dest + 1 + mm];
        memset(sb, 0, sizeof *sb);
        sb->kind = JH_SIB; sb->parent = par[i];
      }
    }
  }
  for (i = 0; i < total; i++) {                                /* fill each opened fork's sibling rows: one short pass per fork (pages of 16) */
    int m, filled = 0;
    if (rows[i].kind != JH_FORK || !rows[i].open) continue;
    for (m = 0; i + 1 + m < total && rows[i + 1 + m].kind == JH_SIB; m++) {}
    while (filled < m) {
      JrnKid* kid = (JrnKid*)(void*)s_rec;
      uint8_t got = 0;
      int k, want = m - filled > 16 ? 16 : m - filled;
      if (jrn_kid_list(&s_j, rows[i].parent, rows[i - 1].seq, (uint16_t)filled, kid, (uint8_t)want, &got) != JRN_OK || !got) break;
      for (k = 0; k < (int)got; k++, filled++) {
        JaHist* sb = &rows[i + 1 + filled];
        sb->seq = kid[k].seq;
        sb->crossed = kid[k].crossed;
        sb->at_cursor = (kid[k].seq == jrn_cursor(&s_j)) ? 1 : 0;
        sb->saved = (kid[k].seq == s_saved) ? 1 : 0;
        memcpy(sb->name, kid[k].name, 24); sb->name[24] = 0;
      }
    }
    for (; filled < m; filled++) strcpy(rows[i + 1 + filled].name, "(gone)");   /* the journal changed under us: never a hole with a stale seq */
  }
  return total;
}

/* Undo or redo along the current branch until the cursor sits on `target`, stopping at a floor. *moved = steps
 * applied. Returns 0 (arrived), or the JRN_E_* / JRN_NOOP that stopped the chain (`stop` = the step it stopped at). */
int jrnapp_jump(uint32_t target, char stop[25], int* moved) {
  JrnRec rec;
  uint32_t t, hops;
  int rc = 0, dir, n = 0;
  if (stop) stop[0] = 0;
  if (moved) *moved = 0;
  if (!s_r || !s_r->j || s_state != JA_OK) return JRN_E_ARG;
  if (target == jrn_cursor(&s_j)) return 0;
  dir = 1;                                                   /* an ancestor of the cursor means UNDO, else REDO */
  for (t = jrn_cursor(&s_j), hops = 0; t && hops < 4096u; hops++) {
    if (t == target) { dir = -1; break; }
    rc = jrn_find(&s_j, t, &rec);
    if (rc == JRN_E_IO) { ja_event("history jump: a read fault in the direction probe, the jump is refused", rc); return rc; }   /* #326: a fault is NOT "not an ancestor" -- that would jump the wrong way; nothing moved */
    if (rc != 0) break;                                      /* unlocatable (compacted / malformed): not an ancestor -> redo */
    t = rec.parent;
  }
  rc = 0;
  if (target == 0) dir = -1;
  for (hops = 0; hops < 4096u && jrn_cursor(&s_j) != target; hops++) {
    char nm[25];
    if (dir < 0 && jrn_cursor(&s_j) == 0) break;
    rc = jrnapp_step(dir, nm);
    if (rc != JRN_OK) break;
    n++;
    if (strcmp(nm, "partial step") == 0) { rc = JRN_E_TORN; break; }   /* z9: the image holds PART of a chain: stop and say so */
  }
  if (moved) *moved = n;
  if (rc == JRN_E_CROSSED && stop && dir < 0) jrnapp_step_name(jrn_cursor(&s_j), stop);   /* the floor is the step at the cursor */
  if (rc == JRN_E_CROSSED && stop && dir > 0) {              /* redo stopped before a crossed record: name it */
    uint32_t av = 0, tot = 0;
    JrnRec st;
    memset(&st, 0, sizeof st);
    if (jrn_redo_info(&s_j, &av, &tot, &st) == 1) { memcpy(stop, st.name, 24); stop[24] = 0; }
  }
  if (rc == JRN_OK && jrn_cursor(&s_j) != target) rc = JRN_E_NOTHING;
  return rc;
}
