/* Host test for BACKLOG #234 slice 2: the staging funnel (source/img_stage.c) FEEDING THE JOURNAL
 * (source/journal*.c over the real FatFs on a RAM disk). What pdna_main.c's app_stage_sections does on the
 * console is exactly img_stage_sections(&g_rec, ...); this proves the recorder side on the PC:
 *
 *   1. a staged commit produces EXACTLY its spans (count, length) and the section bytes it wrote;
 *   2. undo restores the prior sections byte-for-byte, redo brings the commit back;
 *   3. a crossed op (img_rec_cross, the D6 epoch) floors the load-time offer: avail stops before it;
 *   4. a scope groups every stage of one user action into ONE step (the #300 coalescing);
 *   5. an identical re-stage records nothing (no step, no dirty churn); a NULL recorder still stages.
 *
 *   cc -std=c11 -Wall -Wextra -Wno-unused-function -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf \
 *      -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source tests/host_jrn_funnel_test.c \
 *      source/img_stage.c source/gen3_save.c source/journal.c source/journal_undo.c source/journal_fs.c source/jrn_app.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hjf && /tmp/hjf <saves...>
 *
 * tests/host_g3_stage_sites_test.py recompiles this against mutated copies of img_stage.c and requires
 * each mutant to fail (F1..F4). */
#include "jrn_harness.h"
#include "gen3_save.h"
#include "gen3_box.h"
#include "img_stage.h"
#include "jrn_app.h"

/* jrn_app.c is compiled in (its log/rumble deps stubbed): the load-time OFFER (jrnapp_offer) is pinned on the real source. */
void log_line(const char* fmt, ...) { (void)fmt; }
void rmbl_pause(void) {}
void rmbl_resume(void) {}

#define PCB G3_PC_BYTES
#define KEY 0x0123456789ABCDEFull

static uint8_t sv[G3_SAVE_FILE_SIZE], orig[G3_SAVE_FILE_SIZE];
static uint8_t pc[PCB];
static int slot;

typedef struct { uint8_t* save; int slot; } Acc;
static Acc acc;
static uint8_t* sec_of(int region) {
  int s = gen3_find_section(acc.save, acc.slot, region);
  return s < 0 ? 0 : acc.save + (uint32_t)acc.slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE;
}
static int sget(void* c, uint8_t region, uint16_t off, uint8_t* d, uint16_t n) {
  const uint8_t* s = sec_of(region);
  (void)c;
  if (!s || (uint32_t)off + n > G3_SECTOR_DATA_SIZE) return -1;
  memcpy(d, s + off, n);
  return 0;
}
static int sset(void* c, uint8_t region, uint16_t off, const uint8_t* src, uint16_t n) {
  uint8_t* s = sec_of(region);
  uint16_t cs;
  (void)c;
  if (!s || (uint32_t)off + n > G3_SECTOR_DATA_SIZE) return -1;
  memcpy(s + off, src, n);
  cs = gen3_checksum(s, G3_SECTOR_DATA_SIZE);
  s[G3_OFF_CHECKSUM] = (uint8_t)cs; s[G3_OFF_CHECKSUM + 1] = (uint8_t)(cs >> 8);
  return 0;
}

static Jrn J;
static ImgRec R;
static ImgFlags F;
static int flush_hook(void) { return jrn_flush(&J); }

static int fopen_journal(void) {
  JrnCfg c = cfg_for(KEY, 0, 0);
  R.img.ctx = &acc; R.img.get = sget; R.img.set = sset;
  return jrn_open(&J, &c, &R.img);
}

static int read_pc(uint8_t* dst) {
  int id;
  for (id = G3_SID_PKMN_STORAGE_START; id <= G3_SID_PKMN_STORAGE_END; id++) {
    const uint8_t* s = sec_of(id);
    if (!s) return 0;
    memcpy(dst + (uint32_t)(id - G3_SID_PKMN_STORAGE_START) * G3_SECTOR_DATA_SIZE, s, G3_SECTOR_DATA_SIZE);
  }
  return 1;
}

static int pc_equals(const uint8_t* want) {
  uint8_t cur[PCB];
  return read_pc(cur) && memcmp(cur, want, PCB) == 0;
}

/* A fresh card + the save + an opened, prepared journal + a recorder bound to it. */
static int world(const uint8_t* file) {
  memcpy(sv, file, G3_SAVE_FILE_SIZE);
  memcpy(orig, file, G3_SAVE_FILE_SIZE);
  acc.save = sv; acc.slot = slot;
  card_fresh(FM_FAT);
  rd_fattime_hook = jrn_fattime_filter;
  memset(&R, 0, sizeof R);
  imgf_clear(&F);
  R.flush = flush_hook;
  if (fopen_journal() != JRN_OK) return 0;
  if (jrn_prepare(&J) != JRN_OK) return 0;
  R.j = &J;
  R.state = IREC_OK;
  return read_pc(pc);
}

/* The same commit the app would stage: change `n` bytes of the PC image at `off`. */
static void poke(uint32_t off, uint32_t n, uint8_t x) {
  uint32_t i;
  for (i = 0; i < n; i++) pc[off + i] = (uint8_t)(pc[off + i] ^ x ^ (uint8_t)i);
}

static void t_commit_spans(void) {
  JrnRec rec;
  uint32_t o5 = 100, o6 = G3_SECTOR_DATA_SIZE + 2000;   /* region 5 (4 B) and region 6 (10 B) */
  poke(o5, 4, 0x5A); poke(o6, 10, 0xA5);
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "stage returns true");
  CHECK(pc_equals(pc), "the staged bytes are in the image");
  CHECK(jrn_pending(&J) == 1, "one step pending, got %d", jrn_pending(&J));
  CHECK(R.state == IREC_OK && R.lost == 0, "recording is honest: state %d lost %u", R.state, (unsigned)R.lost);
  CHECK(jrn_flush(&J) == JRN_OK, "flush");
  CHECK(jrn_find(&J, 1, &rec) == 0, "the step is on disk as seq 1");
  CHECK(rec.kind == JRN_KIND_STEP && rec.nspans == 2, "EXACTLY two spans (one per changed run), got %u", (unsigned)rec.nspans);
  CHECK(rec.len == JRN_REC_HDR + (JRN_SPAN_HDR + 2u * 4u) + (JRN_SPAN_HDR + 2u * 10u) + 4u, "record length is exactly its spans, got %u", (unsigned)rec.len);
  CHECK(!rec.crossed, "a plain commit is not crossed");
  CHECK(jrn_undo(&J, &R.img, 0) == JRN_OK, "undo");
  CHECK(pc_equals(pc) == 0, "undo took the bytes back");
  CHECK(memcmp(sv, orig, sizeof sv) == 0, "undo restores the image byte-for-byte (sections + checksums)");
  CHECK(jrn_redo(&J, &R.img, 0) == JRN_OK, "redo");
  CHECK(pc_equals(pc), "redo brings the commit back");
}

static void t_crossed_floors_offer(void) {
  Jrn j2;
  JrnCfg c = cfg_for(KEY, 0, 0);
  JrnImage im;
  uint32_t av = 99, total = 99;
  JrnRec stop;
  poke(300, 6, 0x11);
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "A");   /* seq 1 */
  img_rec_cross(&R);                                                       /* a cross-file op happened (D6) */
  poke(900, 6, 0x22);
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "B");   /* seq 2: crossed */
  poke(1500, 6, 0x33);
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "C");   /* seq 3 */
  CHECK(jrn_flush(&J) == JRN_OK, "flush");
  CHECK(jrn_find(&J, 2, &stop) == 0 && stop.crossed, "step 2 carries the crossed flag");
  CHECK(jrn_find(&J, 3, &stop) == 0 && !stop.crossed, "the flag is per-op: step 3 is not crossed");
  /* the power cut: the card kept the journal, the save on the card is the ORIGINAL image */
  memcpy(sv, orig, sizeof sv);
  im.ctx = &acc; im.get = sget; im.set = sset;
  CHECK(jrn_open(&j2, &c, &im) == JRN_OK, "reopen on the original image");
  CHECK(jrn_offer(&j2) == 1, "the load-time re-apply offer is up");
  CHECK(jrn_redo_info(&j2, &av, &total, &stop) == 1, "a crossed record cuts the chain");
  CHECK(total == 3 && av == 1, "3 steps, only 1 before the crossed one: total %u avail %u", (unsigned)total, (unsigned)av);
  CHECK(stop.seq == 2, "the chain stops AT the crossed step, got seq %u", (unsigned)stop.seq);
  CHECK(jrn_redo(&j2, &im, 0) == JRN_OK, "re-apply step 1");
}

static void t_scope_is_one_step(void) {
  JrnRec rec;
  poke(50, 8, 0x0F);                                        /* the place */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "seed");
  CHECK(jrn_flush(&J) == JRN_OK, "flush seed");
  {
    uint32_t base = jrn_tip(&J);
    img_scope_open(&R, "Box move");
    poke(700, 8, 0x3C);                                     /* the place */
    (void)img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
    CHECK(jrn_pending(&J) == 0 && jrn_tip(&J) == base, "inside a scope nothing is recorded yet");
    CHECK(!pc_equals(pc), "inside a scope the image is not written yet (deferred)");
    poke(1300, 8, 0xC3);                                    /* clear_origin */
    (void)img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
    CHECK(img_scope_close(&F, &R, sv, slot), "close");
    CHECK(pc_equals(pc), "the close wrote the final bytes");
    CHECK(jrn_tip(&J) == base + 1u, "the whole scope is ONE step: tip %u -> %u", (unsigned)base, (unsigned)jrn_tip(&J));
    CHECK(jrn_flush(&J) == JRN_OK, "flush scope");
    CHECK(jrn_find(&J, jrn_tip(&J), &rec) == 0 && rec.nspans == 2, "its two runs are its two spans, got %u", (unsigned)rec.nspans);
    CHECK(strcmp(rec.name, "Box move") == 0, "the scope's name is the step's name: '%s'", rec.name);
  }
  /* BACKLOG #306: a CROSSED drop (Bank <-> PC) opens the same "Box move" scope as a plain drop, then
   * renames it at the cross site (app_step_name -> img_rec_name). The recorded step must carry the
   * RENAME, and the rename must not leak into the next scope. */
  {
    uint32_t base = jrn_tip(&J);
    img_scope_open(&R, "Box move");
    img_rec_name(&R, "Bank move");
    img_rec_cross(&R);
    poke(2600, 6, 0x4D);
    (void)img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
    CHECK(img_scope_close(&F, &R, sv, slot), "close the renamed scope");
    CHECK(jrn_tip(&J) == base + 1u, "the renamed scope is one step");
    CHECK(jrn_flush(&J) == JRN_OK, "flush renamed");
    CHECK(jrn_find(&J, jrn_tip(&J), &rec) == 0 && strcmp(rec.name, "Bank move") == 0,
          "the crossed drop's step is named 'Bank move', got '%s'", rec.name);
    CHECK(R.name == 0, "the rename does not outlive its scope");
    img_scope_open(&R, "Box move");
    poke(2700, 6, 0x5E);
    (void)img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
    CHECK(img_scope_close(&F, &R, sv, slot), "close the next plain scope");
    CHECK(jrn_flush(&J) == JRN_OK, "flush plain");
    CHECK(jrn_find(&J, jrn_tip(&J), &rec) == 0 && strcmp(rec.name, "Box move") == 0,
          "a plain drop after it is still 'Box move', got '%s'", rec.name);
  }
}

static void t_identical_and_null(void) {
  uint32_t tip;
  poke(2000, 5, 0x77);
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "stage");
  CHECK(jrn_flush(&J) == JRN_OK, "flush");
  tip = jrn_tip(&J);
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "identical re-stage");
  CHECK(jrn_tip(&J) == tip && jrn_pending(&J) == 0, "an identical stage records nothing");
  poke(2200, 5, 0x66);
  CHECK(img_stage_sections(&F, 0, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "NULL recorder stages");
  CHECK(pc_equals(pc), "the NULL recorder still writes the image");
  CHECK(jrn_tip(&J) == tip && jrn_pending(&J) == 0, "and records nothing");
}

/* D2 (#301): a step the journal could NOT record (B: > one record) is a gap, and a gap is a floor. The record after it
 * (C) is crossed, so the load-time re-apply offer stops before C instead of applying C on a base missing B. */
static void t_gap_is_a_floor(void) {
  Jrn j2;
  JrnCfg c = cfg_for(KEY, 0, 0);
  JrnImage im;
  uint32_t av = 99, total = 99;
  JrnRec tip;
  poke(100, 4, 0x5A);                                                     /* A: region 5 */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "A");
  CHECK(jrn_flush(&J) == JRN_OK, "flush A");
  poke(G3_SECTOR_DATA_SIZE, 700, 0x33);                                   /* B: 700 contiguous bytes > one record */
  (void)img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
  CHECK(R.state == IREC_GAP, "B could not be recorded: state %d", R.state);
  poke(2u * G3_SECTOR_DATA_SIZE + 50, 4, 0x77);                           /* C: region 7 */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "C");
  CHECK(jrn_flush(&J) == JRN_OK, "flush C");
  CHECK(jrn_find(&J, jrn_tip(&J), &tip) == 0 && tip.crossed, "the record after a gap is crossed");
  memcpy(sv, orig, sizeof sv);                                            /* the power cut */
  im.ctx = &acc; im.get = sget; im.set = sset;
  CHECK(jrn_open(&j2, &c, &im) == JRN_OK, "reopen on the original image");
  CHECK(jrn_redo_info(&j2, &av, &total, 0) == 1 && total == 2 && av == 1,
        "the chain stops before C: rc-cut total %u avail %u", (unsigned)total, (unsigned)av);
}

/* #301 resync floor: a write OUTSIDE the funnel (test code pokes the save buffer directly: no funnel, no epoch bump)
 * moves the image under the journal. The next staged step (C) hits JRN_E_DIVERGED, rec_step resyncs the running
 * hashes -- and the resynced base is NOT the recorded chain's base, so C must be crossed: the load-time re-apply
 * offer stops before C instead of applying C on a base that is missing the outside write. */
static void t_diverged_is_a_floor(void) {
  Jrn j2;
  JrnCfg c = cfg_for(KEY, 0, 0);
  JrnImage im;
  uint32_t av = 99, total = 99;
  JrnRec tip;
  uint8_t out[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
  poke(100, 4, 0x5A);                                                     /* A: region 5 */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "A");
  CHECK(jrn_flush(&J) == JRN_OK, "flush A");
  CHECK(sset(0, 6, 1000, out, 4) == 0, "the outside write lands in region 6");   /* NOT through the funnel */
  CHECK(read_pc(pc), "the app-side image follows the outside write");
  poke(G3_SECTOR_DATA_SIZE + 2000, 6, 0x21);                             /* C: region 6 again (elsewhere in it) */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "C");
  CHECK(jrn_flush(&J) == JRN_OK, "flush C");
  CHECK(R.lost >= 1, "the divergence is counted honestly, lost %u", (unsigned)R.lost);
  CHECK(jrn_find(&J, jrn_tip(&J), &tip) == 0 && tip.crossed == 1, "C records crossed 1 (got %u)", (unsigned)tip.crossed);
  memcpy(sv, orig, sizeof sv);                                            /* the power cut */
  im.ctx = &acc; im.get = sget; im.set = sset;
  CHECK(jrn_open(&j2, &c, &im) == JRN_OK, "reopen on the original image");
  CHECK(jrn_redo_info(&j2, &av, &total, 0) == 1 && total == 2 && av == 1,
        "the chain stops before C: total %u avail %u", (unsigned)total, (unsigned)av);
}

/* #302 reuse gate: the outside write lands in a region the next step does NOT touch (old == new there, but old no
 * longer hashes to crc[region]). step_region must still take the DIVERGED resync on that untouched region -- never
 * reuse the stale crc -- so C records crossed and the re-apply offer stops before it. */
static void t_diverged_untouched_region(void) {
  Jrn j2;
  JrnCfg c = cfg_for(KEY, 0, 0);
  JrnImage im;
  uint32_t av = 99, total = 99;
  JrnRec tip;
  uint8_t out[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
  poke(100, 4, 0x5A);                                                     /* A: region 5 */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "uA");
  CHECK(jrn_flush(&J) == JRN_OK, "flush uA");
  CHECK(sset(0, 6, 1000, out, 4) == 0, "the outside write lands in region 6 (untouched by C)");
  CHECK(read_pc(pc), "the app-side image follows the outside write");
  poke(2000, 6, 0x21);                                                    /* C: region 5 ONLY */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "uC");
  CHECK(jrn_flush(&J) == JRN_OK, "flush uC");
  CHECK(R.lost >= 1, "untouched diverged region: the divergence is counted, lost %u", (unsigned)R.lost);
  CHECK(jrn_find(&J, jrn_tip(&J), &tip) == 0 && tip.crossed == 1, "untouched diverged region: C records crossed 1 (got %u)", (unsigned)tip.crossed);
  memcpy(sv, orig, sizeof sv);
  im.ctx = &acc; im.get = sget; im.set = sset;
  CHECK(jrn_open(&j2, &c, &im) == JRN_OK, "untouched: reopen on the original image");
  CHECK(jrn_redo_info(&j2, &av, &total, 0) == 1 && total == 2 && av == 1,
        "untouched diverged region: the chain stops before C: total %u avail %u", (unsigned)total, (unsigned)av);
}


/* The load-time offer (jrnapp_offer, the app half): an UNDONE tail (cursor < tip, the cursor marker on disk) is offered
 * like a half-swap; a DISCARDED tail is not; a crossed record still floors avail. Uses the real jrn_app.c over the
 * same RAM card (its own root/key, from a real SaveBlock2). */
static ImgRec RA;
static uint8_t app_file[G3_SAVE_FILE_SIZE];
static int zero_pc;
static uint8_t sb2[G3_SECTOR_DATA_SIZE];
static int app_world(const uint8_t* file) {
  if (file != app_file) memcpy(app_file, file, G3_SAVE_FILE_SIZE);
  memcpy(sv, app_file, G3_SAVE_FILE_SIZE);
  acc.save = sv; acc.slot = slot;
  if (zero_pc) {                                            /* #303 swap tests: an empty mon area so every staged step is small (the 512-B record cap) */
    static uint8_t z[G3_SECTOR_DATA_SIZE];
    sset(0, 5, 0, z, sizeof z); sset(0, 6, 0, z, sizeof z);
  }
  memcpy(orig, sv, G3_SAVE_FILE_SIZE);
  card_fresh(FM_FAT);
  rd_fattime_hook = jrn_fattime_filter;
  imgf_clear(&F);
  if (!sec_of(0)) return 0;
  memcpy(sb2, sec_of(0), sizeof sb2);
  if (jrnapp_open(&RA, sv, slot, sb2, false, true) != JA_OK) return 0;
  if (jrnapp_prepare(&RA, sb2, false) != JRN_OK) return 0;
  return read_pc(pc);
}
static int app_world_reset(void);
static int app_reopen(void) { return jrnapp_open(&RA, sv, slot, sb2, false, true) == JA_OK; }
static int stageA(uint32_t off) {
  poke(off, 6, 0x11);
  return img_stage_sections(&F, &RA, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
}

static int app_world_reset(void) { int r; zero_pc = 1; r = app_world(app_file); zero_pc = 0; return r; }   /* a fresh card + the SAME original save with an EMPTY mon area (slots 0..59), mid-test */
static void t_undone_tail_is_offered(void) {
  uint32_t av = 99, total;
  char stop[25], nm[25];
  CHECK(stageA(300) && stageA(900), "stage A, B");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step(-1, nm) == JRN_OK, "undo B");
  CHECK(jrnapp_flush() == JRN_OK, "the cursor marker lands");
  CHECK(app_reopen(), "simulated reopen");
  CHECK(jrnapp_cursor() < jrnapp_tip(), "cursor %u < tip %u", (unsigned)jrnapp_cursor(), (unsigned)jrnapp_tip());
  total = jrnapp_offer(&av, stop);
  CHECK(total >= 1 && av >= 1, "an UNDONE tail is offered: total %u avail %u", (unsigned)total, (unsigned)av);
  /* a DISCARDED tail does not offer */
  jrnapp_decline();
  CHECK(app_reopen(), "reopen after decline");
  CHECK(jrnapp_offer(&av, stop) == 0 && av == 0, "a discarded tail is not offered (avail %u)", (unsigned)av);
}

static void t_crossed_still_floors_app_offer(void) {
  uint32_t av = 99, total;
  char stop[25];
  CHECK(stageA(300), "A");
  img_rec_cross(&RA);
  CHECK(stageA(900) && stageA(1500), "B (crossed), C");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  memcpy(sv, orig, sizeof sv);                              /* the power cut: the card kept the original image */
  CHECK(app_reopen(), "reopen on the original image");
  total = jrnapp_offer(&av, stop);
  CHECK(total == 3 && av == 1, "3 steps, avail floored at 1: total %u avail %u", (unsigned)total, (unsigned)av);
  CHECK(stop[0] != 0, "the crossed step is named");
}

/* ---- #303: a swap's two halves are one chord press. The staging below is EXACTLY what drop_held does to the PC image:
 * drop 1 (scope "Box move"): the held X lands on the occupied slot B (B: Y -> X), the source slot A is cleared (A: X -> 0);
 * drop 2 (scope "Box move", or the unscoped B put-away whose funnel default is also "Box move"): the displaced Y lands in the
 * empty slot C (C: 0 -> Y). Mons are 80 non-zero bytes at pc offset 4 + slot*80; slot 49 straddles sections 5/6. */
#define MONB 80u
static void mon_fill(uint8_t* m, unsigned seed) {
  unsigned i;
  for (i = 0; i < MONB; i++) m[i] = (uint8_t)(((seed * 131u + i * 29u + (seed >> 1) * 7u) % 251u) + 1u);
}
static uint8_t* slot_at(unsigned slot) { return pc + 4u + slot * MONB; }
static int stage_pc(const char* scope) {
  int ok;
  if (scope) img_scope_open(&RA, scope);
  ok = img_stage_sections(&F, &RA, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
  if (scope) ok = img_scope_close(&F, &RA, sv, slot) && ok;
  return ok;
}
static uint8_t snapS[G3_SAVE_FILE_SIZE], snapF[G3_SAVE_FILE_SIZE];

/* setup (name "Setup": never a pair candidate) then the swap: returns with the tip at the swap's 2nd half, flushed.
 * snapS = the image before the swap, snapF = after both halves. */
static int stage_swap(unsigned a, unsigned b, unsigned c) {
  uint8_t x[MONB], y[MONB];
  mon_fill(x, 1); mon_fill(y, 2);
  memset(slot_at(a), 0, MONB); memset(slot_at(b), 0, MONB); memset(slot_at(c), 0, MONB);
  memcpy(slot_at(a), x, MONB); memcpy(slot_at(b), y, MONB);
  if (!stage_pc("Setup")) return 0;
  memcpy(snapS, sv, sizeof sv);
  memcpy(slot_at(b), x, MONB); memset(slot_at(a), 0, MONB);               /* drop 1 */
  if (!stage_pc("Box move")) return 0;
  memcpy(slot_at(c), y, MONB);                                             /* drop 2 */
  if (!stage_pc("Box move")) return 0;
  memcpy(snapF, sv, sizeof sv);
  return jrnapp_flush() == JRN_OK;
}

static void t_swap_pair_chords(void) {
  char nm[25];
  uint32_t tip;
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(10, 11, 12), "stage the swap (setup + 2 halves)");
  tip = jrnapp_tip();
  CHECK(jrnapp_cursor() == tip && tip == 3u, "three steps recorded (setup + two halves), tip %u", (unsigned)tip);
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0, "#303 pair at the tip: ONE undo press, named Swap ('%s')", nm);
  CHECK(jrnapp_cursor() == 1u, "#303 the press moved over BOTH halves: cursor %u", (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapS, sizeof sv) == 0, "#303 the image is byte-exact the pre-swap image (no half-swap state)");
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == tip, "#303 ONE redo press redoes BOTH ('%s', cursor %u)", nm, (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapF, sizeof sv) == 0, "#303 redo: byte-exact the post-swap image");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Setup") == 0, "the step below the pair is a plain one (named '%s')", nm);
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_NOTHING, "nothing left to undo at the root");
}

static void t_swap_pair_straddle(void) {
  char nm[25];
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(48, 49, 50), "stage a swap whose displaced slot (49) straddles sections 5/6 (and C=50 straddles nothing)");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && memcmp(sv, snapS, sizeof sv) == 0,
        "#303 a section-straddling slot still pairs and undoes byte-exact ('%s')", nm);
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && memcmp(sv, snapF, sizeof sv) == 0, "and redoes byte-exact");
  jrnapp_flush();
  CHECK(app_reopen() == 1, "reopen");
}

static void t_swap_pair_mid_history(void) {
  char nm[25];
  uint8_t y[MONB];
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(10, 11, 12), "stage the swap");
  mon_fill(y, 2);                                                          /* a PLAIN move of Y: slot 12 -> slot 20 (named Box move) */
  memset(slot_at(12), 0, MONB); memcpy(slot_at(20), y, MONB);
  CHECK(stage_pc("Box move"), "a plain move above the pair");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Box move") == 0 && jrnapp_cursor() == 3u,
        "#303 undo INTO the pair from above: the plain move above it is ONE step ('%s', cursor %u)", nm, (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 1u && memcmp(sv, snapS, sizeof sv) == 0,
        "#303 pair mid-history: the next press takes BOTH halves ('%s', cursor %u)", nm, (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 3u && memcmp(sv, snapF, sizeof sv) == 0,
        "#303 redo pairs mid-history too ('%s')", nm);
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Box move") == 0 && jrnapp_cursor() == 4u, "then the plain step above is redone alone");
}

/* The negatives: shapes that LOOK like a swap's neighbours but are not -- each must stay per-step. */
static void t_swap_pair_negatives(void) {
  char nm[25];
  uint8_t x[MONB], y[MONB], z[MONB];
  mon_fill(x, 1); mon_fill(y, 2); mon_fill(z, 3);
  /* (a) two plain moves of the SAME mon in a row (A -> B, then B -> C): N removed X, N+1 re-adds X, but N replaced nothing */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memset(slot_at(10), 0, MONB); memcpy(slot_at(11), x, MONB); CHECK(stage_pc("Box move"), "move A->B");
  memset(slot_at(11), 0, MONB); memcpy(slot_at(12), x, MONB); CHECK(stage_pc("Box move"), "move B->C");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Box move") == 0 && jrnapp_cursor() == 2u, "#303 two plain moves are NOT a pair: one undo = one step (cursor %u)", (unsigned)jrnapp_cursor());
  /* (b) N clears Y (no replaced slot), N+1 adds the same Y elsewhere -- the discriminator is the REPLACED slot */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memset(slot_at(11), 0, MONB); CHECK(stage_pc("Box move"), "clear B");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "add Y at C");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 no replaced slot in the older half: not a pair (cursor %u)", (unsigned)jrnapp_cursor());
  /* (c) a swap whose second drop places a DIFFERENT mon Z (not the displaced Y): bytes disagree, never paired. Z agrees with Y
   * in 75 of 80 bytes (>= the minimum), so ONLY the zero-mismatch rule keeps this from pairing. */
  memcpy(z, y, MONB); z[70] ^= 0x55; z[71] ^= 0x33; z[72] ^= 0x11; z[73] ^= 0x77; z[74] ^= 0x22;
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_pc("Box move"), "drop 1");
  memcpy(slot_at(12), z, MONB); CHECK(stage_pc("Box move"), "drop 2 places Z != Y");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 the added bytes must equal the displaced bytes: a different mon is not a pair (cursor %u)", (unsigned)jrnapp_cursor());
  /* (d) a SPARSE second half (only 20 bytes non-zero: fewer than JA_MATCH_MIN compared) agrees but proves too little */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  memset(y + 20, 0, MONB - 20u); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup (Y is 20 bytes of data and zeros)");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_pc("Box move"), "drop 1");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2 adds the sparse Y");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 fewer than the minimum compared bytes: not a pair (cursor %u)", (unsigned)jrnapp_cursor());
}

/* A floor BETWEEN the halves: no atomic treatment, the press behaves exactly as today. */
static void t_swap_pair_floors(void) {
  char nm[25];
  uint8_t x[MONB], y[MONB];
  mon_fill(x, 1); mon_fill(y, 2);
  /* the NEWER half crossed (img_rec_cross before drop 2) */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_pc("Box move"), "drop 1");
  img_rec_cross(&RA);
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2 (crossed)");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_CROSSED && jrnapp_cursor() == 3u, "#303 floor on the newer half: the honest refusal, nothing moved (cursor %u)", (unsigned)jrnapp_cursor());
  /* the OLDER half crossed: the newer undoes alone (as today), then the floor refuses */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  img_rec_cross(&RA);
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_pc("Box move"), "drop 1 (crossed)");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 floor on the older half: no atomic treatment, ONE step (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_CROSSED, "and the next press is the floor's refusal");
  /* DIVERGED between the halves: an outside write between the drops makes drop 2 record crossed */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_pc("Box move"), "drop 1");
  CHECK(jrnapp_flush() == JRN_OK, "flush drop 1");
  { uint8_t o[4] = { 0xDE, 0xAD, 0xBE, 0xEF }; CHECK(sset(0, 7, 1000, o, 4) == 0 && read_pc(pc), "an outside write lands in region 7"); }
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2 after the divergence");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_CROSSED && jrnapp_cursor() == 3u, "#303 a diverged gap between the halves is a floor: refusal, nothing moved");
}

/* All-or-nothing: the first half moved, the second is refused (the image no longer matches) -> the first is rolled back. */
static void t_swap_pair_rollback(void) {
  char nm[25];
  uint8_t before[G3_SAVE_FILE_SIZE];
  uint8_t b;
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(10, 11, 12), "stage the swap");
  CHECK(sget(0, 5, 4 + 11 * MONB + 10, &b, 1) == 0, "read a byte of slot B");
  b ^= 0x40;
  CHECK(sset(0, 5, 4 + 11 * MONB + 10, &b, 1) == 0, "corrupt it (checksum kept honest) behind the journal's back: the OLDER half's span no longer matches");
  memcpy(before, sv, sizeof sv);
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_DIVERGED, "#303 second half refused: DIVERGED (nothing half-done)");
  CHECK(jrnapp_cursor() == jrnapp_tip() && jrnapp_cursor() == 3u, "the first half was rolled back: cursor %u", (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, before, sizeof sv) == 0, "#303 the image is byte-identical to before the press");
}

/* The load-time net with the cursor INSIDE a pair: still offered, restores the displaced mon byte-exact. */
static void t_swap_pair_offer(void) {
  uint32_t av = 0, total;
  char stop[25], nm[25];
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(10, 11, 12), "stage the swap");
  CHECK(jrnapp_step(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "per-step undo of the 2nd half only (the History screen can do this): cursor inside the pair");
  CHECK(jrnapp_flush() == JRN_OK, "the cursor marker lands");
  CHECK(app_reopen(), "reopen");
  total = jrnapp_offer(&av, stop);
  CHECK(total >= 1 && av >= 1, "#303 the undone tail inside a pair is still offered: total %u avail %u", (unsigned)total, (unsigned)av);
  CHECK(jrnapp_reapply() == 1 && memcmp(sv, snapF, sizeof sv) == 0, "#303 re-apply restores the displaced mon byte-exact");
}

static void t_swap_pair_history_labels(void) {
  JaHist rows[8];
  int n, more = 0, fh = 0;
  uint8_t y[MONB];
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(10, 11, 12), "stage the swap");
  mon_fill(y, 2);
  memset(slot_at(12), 0, MONB); memcpy(slot_at(20), y, MONB);
  CHECK(stage_pc("Box move"), "plain move above");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 4, "four rows, got %d", n);
  CHECK(n == 4 && strcmp(rows[0].name, "Box move") == 0, "the plain move stays 'Box move' ('%s')", rows[0].name);
  CHECK(n == 4 && strcmp(rows[1].name, "Box move 2/2") == 0, "#303 the swap's newer half reads 'Box move 2/2' ('%s')", rows[1].name);
  CHECK(n == 4 && strcmp(rows[2].name, "Box move 1/2") == 0, "#303 the swap's older half reads 'Box move 1/2' ('%s')", rows[2].name);
  CHECK(n == 4 && strcmp(rows[3].name, "Setup") == 0, "the setup is unlabelled ('%s')", rows[3].name);
}

int main(int argc, char** argv) {
  int a;
  static uint8_t file[G3_SAVE_FILE_SIZE];
  for (a = 1; a < argc; a++) {
    FILE* fp = fopen(argv[a], "rb");
    Gen3SaveInfo info;
    static uint8_t scratch[G3_SECTOR_DATA_SIZE * 4];
    if (!fp) continue;
    if (fread(file, 1, sizeof file, fp) != sizeof file) { fclose(fp); continue; }
    fclose(fp);
    if (!gen3_parse_into(file, sizeof file, &info, scratch)) continue;
    slot = info.slot;
    printf("== %s (slot %d)\n", argv[a], slot);
    CHECK(world(file), "world"); t_commit_spans();
    CHECK(world(file), "world"); t_crossed_floors_offer();
    CHECK(world(file), "world"); t_gap_is_a_floor();
    CHECK(world(file), "world"); t_diverged_is_a_floor();
    CHECK(world(file), "world"); t_diverged_untouched_region();
    CHECK(world(file), "world"); t_scope_is_one_step();
    CHECK(world(file), "world"); t_identical_and_null();
    CHECK(app_world(file), "app world"); t_undone_tail_is_offered();
    CHECK(app_world(file), "app world"); t_crossed_still_floors_app_offer();
    CHECK(app_world(file), "app world"); t_swap_pair_chords();
    CHECK(app_world(file), "app world"); t_swap_pair_straddle();
    CHECK(app_world(file), "app world"); t_swap_pair_mid_history();
    CHECK(app_world(file), "app world"); t_swap_pair_negatives();
    CHECK(app_world(file), "app world"); t_swap_pair_floors();
    CHECK(app_world(file), "app world"); t_swap_pair_rollback();
    CHECK(app_world(file), "app world"); t_swap_pair_offer();
    CHECK(app_world(file), "app world"); t_swap_pair_history_labels();
  }
  printf("%lu checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
