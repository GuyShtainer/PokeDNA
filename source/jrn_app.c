/* SPDX-License-Identifier: GPL-3.0-or-later
 * jrn_app.c -- see jrn_app.h. The machine-facing half of the #234 journal wiring. */
#include "jrn_app.h"

#include <string.h>

#include "gen3_save.h"
#include "journal.h"
#include "journal_fs.h"
#include "log.h"
#include "rmbl.h"
#include "sys.h"   /* EWRAM_BSS */

#define JA_ROOT "/PokeDNA/journal"

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
typedef struct { uint8_t* save; int slot; } JaImg;   /* NAMED: `static struct {...} EWRAM_BSS x;` puts the attribute on the wrong side and lands in IWRAM (icon_store.c s_is) */
static JaImg EWRAM_BSS s_ai;   /* the image accessor's context */

static void ja_event(const char* what, int rc) { s_ev_what = what; s_ev_rc = rc; s_ev_new = 1; }

/* ---- the image accessor: regions are section IDs, spans are inside a section's 3,968 data bytes -- */
static uint8_t* ja_sec(int region) {
  int s;
  if (!s_ai.save || region < 0 || region > 13) return 0;
  s = gen3_find_section(s_ai.save, s_ai.slot, region);
  return s < 0 ? 0 : s_ai.save + (uint32_t)s_ai.slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE;
}

static int ja_get(void* ctx, uint8_t region, uint16_t off, uint8_t* dst, uint16_t n) {
  const uint8_t* sec = ja_sec(region);
  (void)ctx;
  if (!sec || !dst || (uint32_t)off + n > G3_SECTOR_DATA_SIZE) return -1;
  memcpy(dst, sec + off, n);
  return 0;
}

/* A span patch keeps the sector's checksum honest exactly as gen3_write_full_section does (over the
 * 3,968 data bytes), so a re-applied or undone section still verifies. */
static int ja_set(void* ctx, uint8_t region, uint16_t off, const uint8_t* src, uint16_t n) {
  uint8_t* sec = ja_sec(region);
  uint16_t cs;
  (void)ctx;
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
  c.nreg = 14; c.reg_size = G3_SECTOR_DATA_SIZE; c.max_segs = 0; c.readonly = 0;
  r->img.ctx = &s_ai; r->img.get = ja_get; r->img.set = ja_set;
  return jrn_open(&s_j, &c, &r->img);
}

int jrnapp_open(ImgRec* r, uint8_t* save, int slot, const uint8_t* sb2, bool frlg, bool write_ok) {
  int rc;
  if (!r) return JA_OFF;
  memset(r, 0, sizeof *r);
  s_r = r; s_state = JA_OFF; s_ev_new = 0;
  s_ai.save = save; s_ai.slot = slot;
  if (!write_ok || !save || !sb2) return JA_OFF;            /* Everdrive / hack ROM / no image: never opens, never says "recorded" */
  s_key = ja_key(sb2, frlg);
  rmbl_pause();                                             /* open can zero a torn tail: an SD write */
  rc = ja_do_open(r);
  rmbl_resume();
  if (rc == JRN_E_VERSION) { s_state = JA_FOREIGN; ja_event("open: foreign journal, read-only", rc); return JA_FOREIGN; }
  if (rc != JRN_OK) { s_state = JA_ERROR; ja_event("open failed, journal off", rc); return JA_ERROR; }
  r->j = &s_j;
  s_saved = jrn_cursor(&s_j);                               /* the loaded image IS the card's: its anchor is the saved point */
  r->flush = jrnapp_flush;
  r->state = IREC_OK;
  s_state = JA_OK;
  return JA_OK;
}

int jrnapp_state(const ImgRec* r) {
  if (s_state != JA_OK) return s_state;
  if (!r || !r->j) return JA_OFF;
  if (r->state == IREC_STOPPED) return JA_STOPPED;
  if (r->state == IREC_GAP) return JA_GAP;
  return r->state == IREC_OK ? JA_OK : JA_OFF;
}

uint32_t jrnapp_cursor(void) { return jrn_cursor(&s_j); }
uint32_t jrnapp_tip(void) { return jrn_tip(&s_j); }

bool jrnapp_first_fill_owed(void) { return s_r && s_r->j && s_state == JA_OK && !s_j.ring; }

/* An identity edit (trainer rename / TID / SID / gender) moves the key: a redirect keeps the history. */
static void ja_redirect(const uint8_t* sb2, bool frlg) {
  uint64_t nk = ja_key(sb2, frlg);
  int rc;
  if (nk == s_key) return;
  rc = jrn_redirect_write(&jrn_fatfs, JA_ROOT, nk, s_j.key);
  if (rc == JRN_OK) s_key = nk; else ja_event("redirect write failed", rc);
}

int jrnapp_prepare(ImgRec* r, const uint8_t* sb2, bool frlg) {
  int rc;
  if (!r || !r->j || s_state != JA_OK || s_j.readonly || s_j.stopped) return 0;
  rmbl_pause();
  rc = jrn_prepare(&s_j);
  if (rc == JRN_OK && sb2) ja_redirect(sb2, frlg);
  rmbl_resume();
  if (rc != JRN_OK) {                                        /* no tail segment: recording cannot work this session */
    ja_event("prepare failed, journal off", rc);
    if (!s_j.tail_seg) { r->j = 0; r->state = IREC_OFF; s_state = JA_ERROR; }
  }
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
uint32_t jrnapp_offer(uint32_t* avail, char stop[25]) {
  JrnRec st;
  uint32_t av = 0, total = 0;
  int rc;
  if (avail) *avail = 0;
  if (stop) stop[0] = 0;
  if (!s_r || !s_r->j || s_state != JA_OK || !jrn_offer(&s_j)) return 0;
  memset(&st, 0, sizeof st);
  rc = jrn_redo_info(&s_j, &av, &total, &st);
  if (rc < 0 || !total) return 0;
  if (avail) *avail = av;
  if (stop && rc == 1) { memcpy(stop, st.name, 24); stop[24] = 0; }
  return total;
}

int jrnapp_reapply(void) {
  uint32_t av = 0, total = 0, i;
  int rc, n = 0, retried = 0;
  if (!s_r || !s_r->j || s_state != JA_OK) return JRN_E_ARG;
  rc = jrn_redo_info(&s_j, &av, &total, 0);
  if (rc < 0) return rc;
  for (i = 0; i < av; i++) {
    rc = jrn_redo(&s_j, &s_r->img, 0);
    if (rc == JRN_E_FULL && !retried) { retried = 1; (void)jrnapp_flush(); rc = jrn_redo(&s_j, &s_r->img, 0); }
    if (rc != JRN_OK) { ja_event("re-apply stopped", rc); break; }
    n++;
  }
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
  for (;;) {
    rc = dir < 0 ? jrn_undo(&s_j, &s_r->img, &rec) : jrn_redo(&s_j, &s_r->img, &rec);
    if (rc == JRN_E_FULL && !retried) { retried = 1; (void)jrnapp_flush(); continue; }
    break;
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

/* The current branch, newest first: from the tip down the parent chain, at most `max` steps. */
int jrnapp_history(JaHist* rows, int max, int* more, int* floor_hit) {
  JrnRec rec;
  uint32_t t, cur, hops = 0;
  int n = 0, ahead = 1, saved_seen = 0;
  if (more) *more = 0;
  if (floor_hit) *floor_hit = 0;
  if (!rows || max < 1 || !s_r || !s_r->j || s_state != JA_OK) return 0;
  t = jrn_tip(&s_j); cur = jrn_cursor(&s_j);
  while (t && n < max && hops++ < JRN_WALK_MAX) {
    if (jrn_find(&s_j, t, &rec) != 0) { if (floor_hit) *floor_hit = 1; return n; }
    if (t == cur) ahead = 0;                                 /* this row and every older one are IN the image */
    if (t == s_saved) saved_seen = 1;
    rows[n].seq = t;
    rows[n].crossed = rec.crossed ? 1 : 0;
    rows[n].at_cursor = (t == cur) ? 1 : 0;
    rows[n].ahead = (uint8_t)ahead;
    rows[n].saved = (uint8_t)saved_seen;
    memcpy(rows[n].name, rec.name, 24); rows[n].name[24] = 0;
    n++;
    t = rec.parent;
  }
  if (t && more) *more = 1;
  return n;
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
    if (jrn_find(&s_j, t, &rec) != 0) break;
    t = rec.parent;
  }
  if (target == 0) dir = -1;
  for (hops = 0; hops < 4096u && jrn_cursor(&s_j) != target; hops++) {
    if (dir < 0 && jrn_cursor(&s_j) == 0) break;
    rc = jrnapp_step(dir, 0);
    if (rc != JRN_OK) break;
    n++;
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
