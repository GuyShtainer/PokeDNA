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

/* A step name for a CHECK message: a mutant can leave garbage in a name; the report must stay printable. */
static const char* safe(const char* n) {
  static char b[4][32];
  static int k;
  char* o = b[k++ & 3];
  int i;
  for (i = 0; i < 31 && n && n[i]; i++) o[i] = (n[i] >= 0x20 && n[i] < 0x7F) ? n[i] : '?';
  o[i] = 0;
  return o;
}
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
    CHECK(strcmp(rec.name, "Box move") == 0, "the scope's name is the step's name: '%s'", safe(rec.name));
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
          "the crossed drop's step is named 'Bank move', got '%s'", safe(rec.name));
    CHECK(R.name == 0, "the rename does not outlive its scope");
    img_scope_open(&R, "Box move");
    poke(2700, 6, 0x5E);
    (void)img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
    CHECK(img_scope_close(&F, &R, sv, slot), "close the next plain scope");
    CHECK(jrn_flush(&J) == JRN_OK, "flush plain");
    CHECK(jrn_find(&J, jrn_tip(&J), &rec) == 0 && strcmp(rec.name, "Box move") == 0,
          "a plain drop after it is still 'Box move', got '%s'", safe(rec.name));
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

/* D2 (#301): a step the journal could NOT record (B: more than a CHAIN of JRN_CHAIN_MAX records can hold -- since z9 a step merely
 * larger than one record is a chain, see t_bulk_chain_funnel) is a gap, and a gap is a floor. The record after it
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
  poke(G3_SECTOR_DATA_SIZE, 2300, 0x33);                                  /* B: 2,300 contiguous bytes = 11 records > JRN_CHAIN_MAX */
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
    sset(0, 1, 0x234, z, 700);                                 /* zd #320: an empty party area (count + 6 records) so a landing's spans are exactly its own bytes */
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
/* za #314a: drop_held's SWAP tail renames the step ("Swap", one-shot, dies with the scope) -- this is the older half of every swap below. */
static int stage_drop1(void) {
  int ok;
  img_scope_open(&RA, "Box move");
  img_rec_name(&RA, "Swap");
  ok = img_stage_sections(&F, &RA, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
  return img_scope_close(&F, &RA, sv, slot) && ok;
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
  if (!stage_drop1()) return 0;
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
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0, "#303 pair at the tip: ONE undo press, named Swap ('%s')", safe(nm));
  CHECK(jrnapp_cursor() == 1u, "#303 the press moved over BOTH halves: cursor %u", (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapS, sizeof sv) == 0, "#303 the image is byte-exact the pre-swap image (no half-swap state)");
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == tip, "#303 ONE redo press redoes BOTH ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapF, sizeof sv) == 0, "#303 redo: byte-exact the post-swap image");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Setup") == 0, "the step below the pair is a plain one (named '%s')", safe(nm));
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_NOTHING, "nothing left to undo at the root");
}

static void t_swap_pair_straddle(void) {
  char nm[25];
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(48, 49, 50), "stage a swap whose displaced slot (49) straddles sections 5/6 (and C=50 straddles nothing)");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && memcmp(sv, snapS, sizeof sv) == 0,
        "#303 a section-straddling slot still pairs and undoes byte-exact ('%s')", safe(nm));
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
        "#303 undo INTO the pair from above: the plain move above it is ONE step ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 1u && memcmp(sv, snapS, sizeof sv) == 0,
        "#303 pair mid-history: the next press takes BOTH halves ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 3u && memcmp(sv, snapF, sizeof sv) == 0,
        "#303 redo pairs mid-history too ('%s')", safe(nm));
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
  memset(slot_at(10), 0, MONB); memcpy(slot_at(11), x, MONB); CHECK(stage_drop1(), "move A->B (named Swap: only the replaced-slot rule may refuse the pair)");
  memset(slot_at(11), 0, MONB); memcpy(slot_at(12), x, MONB); CHECK(stage_pc("Box move"), "move B->C");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Box move") == 0 && jrnapp_cursor() == 2u, "#303 two plain moves are NOT a pair: one undo = one step (cursor %u)", (unsigned)jrnapp_cursor());
  /* (b) N clears Y (no replaced slot), N+1 adds the same Y elsewhere -- the discriminator is the REPLACED slot */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memset(slot_at(11), 0, MONB); CHECK(stage_drop1(), "clear B (named Swap: only the replaced-slot rule may refuse the pair)");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "add Y at C");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 no replaced slot in the older half: not a pair (cursor %u)", (unsigned)jrnapp_cursor());
  /* (c) a swap whose second drop places a DIFFERENT mon Z (not the displaced Y): bytes disagree, never paired. Z agrees with Y
   * in 75 of 80 bytes (>= the minimum), so ONLY the zero-mismatch rule keeps this from pairing. */
  memcpy(z, y, MONB); z[70] ^= 0x55; z[71] ^= 0x33; z[72] ^= 0x11; z[73] ^= 0x77; z[74] ^= 0x22;
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "drop 1");
  memcpy(slot_at(12), z, MONB); CHECK(stage_pc("Box move"), "drop 2 places Z != Y");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 the added bytes must equal the displaced bytes: a different mon is not a pair (cursor %u)", (unsigned)jrnapp_cursor());
  /* (d) a SPARSE second half (only 20 bytes non-zero: fewer than JA_MATCH_MIN compared) agrees but proves too little */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  memset(y + 20, 0, MONB - 20u); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup (Y is 20 bytes of data and zeros)");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "drop 1");
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
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "drop 1");
  img_rec_cross(&RA);
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2 (crossed)");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_CROSSED && jrnapp_cursor() == 3u, "#303 floor on the newer half: the honest refusal, nothing moved (cursor %u)", (unsigned)jrnapp_cursor());
  /* the OLDER half crossed: the newer undoes alone (as today), then the floor refuses */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  img_rec_cross(&RA);
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "drop 1 (crossed)");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 floor on the older half: no atomic treatment, ONE step (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_CROSSED, "and the next press is the floor's refusal");
  /* DIVERGED between the halves: an outside write between the drops makes drop 2 record crossed */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  CHECK(stage_pc("Setup"), "setup");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "drop 1");
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

/* z7 review: the newer half's two shape rules and the rollback-failure contract, each with teeth.
 * (e) N+1 REPLACES an occupied slot with the displaced bytes (C: Z -> Y): not "fills an EMPTY slot" -> not a pair.
 * (f) N+1 fills TWO empty slots (D: 0 -> W, then C: 0 -> Y, D first in span order): not "exactly ONE slot" -> not a pair.
 * (g) the second half refused AND the rollback refused (every card write fails from the press on): JRN_OK, "half a swap",
 *     the cursor on the older half and the image EXACTLY the post-drop-1 state -- the caller must re-derive (pdna_main.c). */
static void t_swap_pair_shape_and_rollback_failure(void) {
  char nm[25];
  uint8_t x[MONB], y[MONB], z[MONB], w[MONB];
  static uint8_t snapH[G3_SAVE_FILE_SIZE];
  mon_fill(x, 1); mon_fill(y, 2); mon_fill(z, 4); mon_fill(w, 3);
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB); CHECK(stage_pc("Setup"), "setup X, Y");
  memcpy(slot_at(12), z, MONB); CHECK(stage_pc("Setup"), "setup Z in C");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "drop 1");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "N+1 replaces Z with Y");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 3u, "#303 (e) a REPLACING second step is not a swap's second half (cursor %u, '%s')", (unsigned)jrnapp_cursor(), safe(nm));
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB); CHECK(stage_pc("Setup"), "setup X, Y");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "drop 1");
  memcpy(slot_at(9), w, MONB); memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "N+1 fills TWO empty slots (9 and 12)");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#303 (f) a two-slot second step is not a swap's second half (cursor %u, '%s')", (unsigned)jrnapp_cursor(), safe(nm));
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(10, 11, 12), "stage the swap");
  CHECK(jrnapp_step(-1, nm) == JRN_OK, "per-step undo of the 2nd half (to capture the half state)");
  memcpy(snapH, sv, sizeof sv);
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap(10, 11, 12), "stage the swap again");
  rd_fail_all_writes = 1;                                   /* the flush between the halves fails: recording stops, the rollback is refused */
  {
    int rc = jrnapp_step_pair(-1, nm);
    rd_fail_all_writes = 0;
    CHECK(rc == JRN_OK && strcmp(nm, "half a swap") == 0, "#303 (g) a refused rollback returns JRN_OK named 'half a swap' (the caller re-derives): rc %d '%s'", rc, safe(nm));
    CHECK(jrnapp_cursor() == 2u && memcmp(sv, snapH, sizeof sv) == 0, "#303 (g) cursor %u on the older half and the image EXACTLY the half state (journal and image agree)", (unsigned)jrnapp_cursor());
  }
}

/* za #314(a), the R1 shape: two same-shaped UNSCOPED PC commits -- an identity edit of the mon in slot A (A: X -> X'), then a paste of
 * the pre-edit copy into an EMPTY slot C (C: 0 -> X) -- satisfy every span condition of a swap, but they are two user actions. The older half
 * of a pair must carry the "Swap" name drop_held's SWAP tail gives it, which neither unscoped commit has: no pair, per-step, plain labels. */
static void t_swap_pair_r1_mislabel(void) {
  char nm[25];
  JaHist rows[8];
  int n, more = 0, fh = 0;
  uint8_t x[MONB], x2[MONB];
  mon_fill(x, 1); mon_fill(x2, 9);                                    /* the edit rewrites the whole mon (an identity/personality edit re-encrypts every data byte) */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB);
  CHECK(stage_pc("Setup"), "setup X in slot 10");
  memcpy(slot_at(10), x2, MONB); CHECK(stage_pc(0), "an unscoped identity edit (10: X -> X')");
  memcpy(slot_at(12), x, MONB);  CHECK(stage_pc(0), "an unscoped paste of the pre-edit copy into the empty slot 12");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 3 && strcmp(rows[0].name, "Box move") == 0 && strcmp(rows[1].name, "Box move") == 0, "#314a the R1 shape is NOT labelled 1/2 + 2/2 ('%s' '%s')", n > 1 ? safe(rows[0].name) : "", n > 1 ? safe(rows[1].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") != 0 && jrnapp_cursor() == 2u, "#314a the R1 shape is ONE step per press, not a 'Swap' ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") != 0 && jrnapp_cursor() == 3u, "#314a ... and redo too ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  /* the control: the SAME bytes, but the older half named "Swap" (what a real drop produces) DO pair */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB);
  CHECK(stage_pc("Setup"), "setup X in slot 10");
  memcpy(slot_at(10), x2, MONB); CHECK(stage_drop1(), "the same identity edit, but named Swap");
  memcpy(slot_at(12), x, MONB);  CHECK(stage_pc(0), "the same paste");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 1u, "#314a control: the Swap name on the older half is what pairs ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
}

/* ---- zc #321: a pair's two halves must be ADJACENT in sequence (newer.parent + 1 == newer.seq). The repro: Setup X,Y + a clone Y' -> Swap -> Box move; a
 * per-step undo puts the cursor ON the Swap; a NEW step copies Y' into an empty slot (parent = the Swap; the orphaned Box move and the undo's cursor marker hold the seqs between). The bytes
 * link (the Swap replaced Y, the new step adds Y) -- but they are two user actions with a sibling branch between them in sequence. Before zc: 'Box move 2/2'
 * + 'Swap 1/2' and one undo took the cursor 4 -> 1 named 'Swap'. Byte-exact either way; the defect was the label and the one-press span. */
static void t_swap_seq_adjacency(void) {
  char nm[25];
  JaHist rows[8];
  int n, more = 0, fh = 0;
  uint8_t x[MONB], y[MONB];
  mon_fill(x, 1); mon_fill(y, 2);
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); CHECK(stage_pc("Setup"), "setup X");                           /* one Setup step per mon (3 mons would exceed ONE record) */
  memcpy(slot_at(11), y, MONB); CHECK(stage_pc("Setup"), "setup Y");
  memcpy(slot_at(13), y, MONB); CHECK(stage_pc("Setup"), "setup Y' (slot 13), a byte-identical clone of Y");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "Swap (X onto Y's slot)");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "Box move (Y into the empty slot 12)");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_tip() == 5u, "tip 5 (3 Setups, Swap, Box move): %u", (unsigned)jrnapp_tip());
  /* control: the REAL pair (adjacent halves) still groups: one press takes both */
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 5 && strcmp(rows[0].name, "Box move 2/2") == 0 && strcmp(rows[1].name, "Swap 1/2") == 0, "#321 control: adjacent halves ARE labelled 2/2 + 1/2 ('%s' '%s')", n > 1 ? safe(rows[0].name) : "", n > 1 ? safe(rows[1].name) : "");
  CHECK(jrnapp_step(-1, nm) == JRN_OK && jrnapp_cursor() == 4u, "per-step undo: the cursor is ON the Swap (seq 4)");
  CHECK(read_pc(pc), "re-derive the PC image");
  memcpy(slot_at(14), y, MONB); CHECK(stage_pc("Box move"), "a NEW 'Box move': Y' (== Y) into the empty slot 14");
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_tip() == 7u, "flush; tip 7, parent the Swap (seq 4); the orphaned Box move (5) and the undo's cursor marker (6) sit between");
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 5 && strcmp(rows[0].name, "Box move") == 0 && strcmp(rows[1].name, "Swap") == 0, "#321 History: NO pair label across a sequence gap ('%s' '%s')", n > 1 ? safe(rows[0].name) : "", n > 1 ? safe(rows[1].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Box move") == 0 && jrnapp_cursor() == 4u, "#321 ONE step per undo press, not a 'Swap' pair ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") != 0 && jrnapp_cursor() == 7u, "#321 ... and redo too ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
}

/* The second link of a chained group: Swap_1(4), an unrelated Edit(5) undone (the cursor marker takes 6), then Swap_2(7, parent 4) + Box move(8): (Swap_2, Box move) are adjacent and
 * pair; Swap_1 is NOT adjacent to Swap_2 (rn.parent + 1 != rm.parent) so the group is the PAIR, never three. */
static void t_swap_seq_adjacency_chain(void) {
  char nm[25];
  JaHist rows[10];
  int n, more = 0, fh = 0;
  uint8_t m[3][MONB];
  unsigned i;
  for (i = 0; i < 3; i++) mon_fill(m[i], 1 + i);
  CHECK(app_world_reset(), "world");
  for (i = 0; i < 3; i++) { memcpy(slot_at(10 + i), m[i], MONB); CHECK(stage_pc("Setup"), "setup M%u", i); }   /* seqs 1..3 */
  memcpy(slot_at(11), m[0], MONB); memset(slot_at(10), 0, MONB); CHECK(stage_drop1(), "Swap_1 (seq 4)");
  pc[30000u] = (uint8_t)(pc[30000u] + 1u); CHECK(stage_pc("Edit"), "an unrelated Edit (seq 5)");
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_step(-1, nm) == JRN_OK && jrnapp_cursor() == 4u, "undo the Edit: cursor on Swap_1");
  CHECK(read_pc(pc), "re-derive the PC image");
  memcpy(slot_at(12), m[1], MONB); CHECK(stage_drop1(), "Swap_2 (seq 7, parent 4)");
  memcpy(slot_at(30), m[2], MONB); CHECK(stage_pc("Box move"), "Box move (seq 8)");
  CHECK(jrnapp_flush() == JRN_OK && jrnapp_tip() == 8u, "flush; tip 8 (the undo's cursor marker took seq 6)");
  n = jrnapp_history(rows, 10, &more, &fh);
  CHECK(n >= 3 && strcmp(rows[0].name, "Box move 2/2") == 0 && strcmp(rows[1].name, "Swap 1/2") == 0 && strcmp(rows[2].name, "Swap") == 0,
        "#321 History: only the adjacent PAIR is labelled; the non-adjacent Swap_1 stays plain ('%s' '%s' '%s')", n > 2 ? safe(rows[0].name) : "", n > 2 ? safe(rows[1].name) : "", n > 2 ? safe(rows[2].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 4u, "#321 the press is the PAIR (cursor %u, want 4), not three steps", (unsigned)jrnapp_cursor());
}

/* ---- zc #323: a plain press that moves ONE half of a swap toasts the marked name "Swap (half)" (REDO only: a plain undo of the older half lands on the whole
 * pre-swap image). Instance 1, the party landing (D7 ruling 10): Setup, Swap, then a step that is no Box move (an "Edit" stands in for the unscoped landing
 * write) -- the redo from below pairs nothing, so the press applies the Swap alone. Instance 2, the D = 65 hop cap (t_swap_redo_hop_cap, checked there).
 * Controls: the WHOLE pair / group press keeps the plain "Swap", the plain undo of the lone Swap keeps "Swap", a plain "Box move" press is untouched. */
static int stage_chain(unsigned k, unsigned wrong_link);
static void t_swap_half_toast(void) {
  char nm[25];
  {
    uint8_t x[MONB], y[MONB];
    mon_fill(x, 1); mon_fill(y, 2);
    CHECK(app_world_reset(), "world");
    memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
    CHECK(stage_pc("Setup"), "Setup X, Y");
    memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB);
    CHECK(stage_drop1(), "the Swap (drop 1; the displaced Y goes to the PARTY, so no Box move follows)");
  }
  pc[30000u] = (uint8_t)(pc[30000u] + 1u);
  CHECK(stage_pc("Edit") && jrnapp_flush() == JRN_OK, "the landing stand-in (a non-Box-move step above the Swap)");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Edit") == 0, "undo the landing stand-in ('%s')", safe(nm));
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 1u,
        "#323 control: a plain UNDO of the lone Swap lands on the whole pre-swap image and keeps the plain name ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap (half)") == 0 && jrnapp_cursor() == 2u,
        "#323 the party-landing instance: the REDO that applies the Swap alone toasts 'Swap (half)' ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Edit") == 0, "#323 control: the next redo (a plain non-Swap step) is untouched ('%s')", safe(nm));
  CHECK(app_world_reset() && stage_swap(10, 11, 12), "world + the swap again");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 1u, "#323 control: the WHOLE pair undo keeps 'Swap' ('%s')", safe(nm));
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 3u, "#323 control: the WHOLE pair redo keeps 'Swap' ('%s')", safe(nm));
  CHECK(app_world_reset() && stage_chain(2, 0), "world + a chained swap");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0, "#323 control: the WHOLE 3-step group keeps 'Swap' both ways ('%s')", safe(nm));
}

/* ---- zd #320: the PARTY landing pairs. A swap whose displaced mon Y is dropped into the PARTY (not an empty PC slot) ends with ONE step named "Party add"
 * (pdna_main.c party_place_held's ADD arm, a scope): the party count byte (its own 1-byte span: SB1 +0x234) and Y's 100-byte party record at +0x238 + 100n (ONE span
 * of >= 80 added bytes whose first 80 ARE Y's box record). The pair walk accepts it as the final half when those 80 bytes equal the older Swap's replaced slot. ---- */
#define PT_CNT  0x234u
#define PT_REC  0x238u
static uint8_t sb1buf[4u * G3_SECTOR_DATA_SIZE];
static int read_sb1(void) {
  int id;
  for (id = 1; id <= 4; id++) {
    const uint8_t* sc = sec_of(id);
    if (!sc) return 0;
    memcpy(sb1buf + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE, sc, G3_SECTOR_DATA_SIZE);
  }
  return 1;
}
/* Y's party record: the first 80 bytes are the box record `m`, the 20-byte tail is the derived plaintext (non-zero here). `nbytes` < 100 writes only a prefix. */
static int stage_party_land(const char* name, const uint8_t* m, unsigned nbytes, unsigned n, int also_pc) {
  uint8_t rec[100];
  unsigned i;
  int ok;
  memcpy(rec, m, MONB);
  for (i = MONB; i < 100u; i++) rec[i] = (uint8_t)(0x31u + i);
  if (!read_sb1()) return 0;
  sb1buf[PT_CNT] = (uint8_t)(n + 1u);
  memcpy(sb1buf + PT_REC + n * 100u, rec, nbytes);
  if (name) img_scope_open(&RA, name);
  if (also_pc) { pc[30000u] = (uint8_t)(pc[30000u] + 1u); (void)img_stage_sections(&F, &RA, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc); }
  ok = img_stage_sections(&F, &RA, sv, slot, 1, 4, sb1buf);
  if (name) ok = img_scope_close(&F, &RA, sv, slot) && ok;
  return ok;
}
/* Setup X (slot 10) + Y (slot 11), the Swap (X -> 11, slot 10 cleared; Y displaced into the hand), then the landing; returns with the tip at the landing, flushed. */
static int stage_party_swap(const char* name, unsigned nbytes, unsigned n, int flip79, int also_pc) {
  uint8_t x[MONB], y[MONB];
  mon_fill(x, 1); mon_fill(y, 2);
  if (!app_world_reset()) return 0;
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
  if (!stage_pc("Setup")) return 0;
  memcpy(snapS, sv, sizeof sv);
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB);
  if (!stage_drop1()) return 0;
  if (flip79) y[79] ^= 0x01;
  if (!stage_party_land(name, y, nbytes, n, also_pc)) return 0;
  memcpy(snapF, sv, sizeof sv);
  return jrnapp_flush() == JRN_OK;
}
static void t_party_add_pairs(void) {
  char nm[25];
  JaHist rows[8];
  int n, more = 0, fh = 0;
  unsigned pn;
  /* the shape the pair walk relies on (ruling 10's stop-licence: report the real span shape) */
  CHECK(stage_party_swap("Party add", 100, 2, 0, 0), "stage Setup, Swap, Party add");
  jrnapp_step_name(3, nm);
  CHECK(jrnapp_tip() == 3u && strcmp(nm, "Party add") == 0, "#320 the landing is ONE step named 'Party add': tip %u '%s'", (unsigned)jrnapp_tip(), safe(nm));
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 3 && strcmp(rows[0].name, "Party add 2/2") == 0 && strcmp(rows[1].name, "Swap 1/2") == 0,
        "#320 History labels the pair ('%s' '%s')", n > 1 ? safe(rows[0].name) : "", n > 1 ? safe(rows[1].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 1u, "#320 ONE undo press takes the Party add + the Swap ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapS, sizeof sv) == 0, "#320 the image is byte-exact the pre-swap image: both mons back, the party count and record gone");
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 3u && memcmp(sv, snapF, sizeof sv) == 0, "#320 ONE redo press redoes both, byte-exact ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  /* the party offset is the SPAN's, never hard-coded: every party slot position pairs */
  for (pn = 1; pn <= 5; pn += 2) {
    CHECK(stage_party_swap("Party add", 100, pn, 0, 0), "stage a landing at party slot %u", pn);
    CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 1u && memcmp(sv, snapS, sizeof sv) == 0, "#320 party slot %u pairs (cursor %u)", pn, (unsigned)jrnapp_cursor());
  }
  /* the prefix boundary: exactly 80 added bytes pair; 79 do not; a landing whose byte 79 differs does not */
  CHECK(stage_party_swap("Party add", 80, 2, 0, 0), "stage an 80-byte landing");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 1u, "#320 boundary: an 80-byte span pairs (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(stage_party_swap("Party add", 79, 2, 0, 0), "stage a 79-byte landing");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#320 boundary: a 79-byte span NEVER pairs (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(stage_party_swap("Party add", 100, 2, 1, 0), "stage a landing whose byte 79 differs from the Swap's replaced slot");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#320 boundary: 79 matching bytes + 1 mismatch NEVER pairs (cursor %u)", (unsigned)jrnapp_cursor());
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n >= 2 && strcmp(rows[0].name, "Party add") == 0 && strcmp(rows[1].name, "Swap") == 0, "#320 ... and History does not label it ('%s' '%s')", n > 1 ? safe(rows[0].name) : "", n > 1 ? safe(rows[1].name) : "");
  /* NO name = no pair: an unrelated SaveBlock1 edit with the same bytes, and the R1-style same-bytes steps under other names */
  {
    static const char* const other[] = { 0, "Edit", "Box move", "Save block 1" };
    unsigned k;
    for (k = 0; k < 4; k++) {
      CHECK(stage_party_swap(other[k], 100, 2, 0, 0), "stage the same bytes under the name '%s'", other[k] ? other[k] : "(unscoped)");
      CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u && strcmp(nm, "Swap") != 0, "#320 the name is required: '%s' never pairs ('%s', cursor %u)", other[k] ? other[k] : "(unscoped)", safe(nm), (unsigned)jrnapp_cursor());
      CHECK(jrnapp_step_pair(1, nm) == JRN_OK && jrnapp_cursor() == 3u, "#320 ... and redo is one step too");
    }
  }
  /* the landing must not touch the PC */
  CHECK(stage_party_swap("Party add", 100, 2, 0, 1), "stage a 'Party add' that ALSO changes the PC");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#320 a 'Party add' that touches the PC is not the landing shape (cursor %u)", (unsigned)jrnapp_cursor());
  /* the n = 0 limit: count byte + record merge into ONE span that starts at the count byte -> the prefix compare cannot match -> per-step (conservative; retail keeps >= 1 party mon) */
  CHECK(stage_party_swap("Party add", 100, 0, 0, 0), "stage a landing into an EMPTY party (n = 0)");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 2u, "#320 LIMIT: an empty party's landing never pairs (conservative, cursor %u)", (unsigned)jrnapp_cursor());
}

/* zd #320 chained variant: Swap, Swap, Party add -- a 3-group whose final half is a landing. M0 at 10, M1 at 11, M2 at 12; M2 is the mon that goes to the party. */
static void t_party_add_chain(void) {
  char nm[25];
  JaHist rows[8];
  int n, more = 0, fh = 0;
  uint8_t m[3][MONB];
  mon_fill(m[0], 1); mon_fill(m[1], 2); mon_fill(m[2], 3);
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), m[0], MONB); CHECK(stage_pc("Setup"), "Setup M0");
  memcpy(slot_at(11), m[1], MONB); CHECK(stage_pc("Setup"), "Setup M1");
  memcpy(slot_at(12), m[2], MONB); CHECK(stage_pc("Setup"), "Setup M2 (one Setup step per mon: all three would exceed ONE record)");
  memcpy(snapS, sv, sizeof sv);
  memcpy(slot_at(11), m[0], MONB); memset(slot_at(10), 0, MONB);
  CHECK(stage_drop1(), "Swap_1 (M0 onto M1's slot; M1 displaced)");
  memcpy(slot_at(12), m[1], MONB);
  CHECK(stage_drop1(), "Swap_2 (M1 onto M2's slot; M2 displaced)");
  CHECK(stage_party_land("Party add", m[2], 100, 3, 0), "Party add (M2)");
  memcpy(snapF, sv, sizeof sv);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 6 && strcmp(rows[0].name, "Party add 3/3") == 0 && strcmp(rows[1].name, "Swap 2/3") == 0 && strcmp(rows[2].name, "Swap 1/3") == 0,
        "#320 chained: History labels 1/3 2/3 3/3 ('%s' '%s' '%s')", n > 2 ? safe(rows[0].name) : "", n > 2 ? safe(rows[1].name) : "", n > 2 ? safe(rows[2].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 3u && memcmp(sv, snapS, sizeof sv) == 0, "#320 chained: ONE undo press takes all three, byte-exact ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 6u && memcmp(sv, snapF, sizeof sv) == 0, "#320 chained: ONE redo press redoes all three, byte-exact ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  /* a wrong link: the landing places a mon that is NOT the one Swap_2 displaced */
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), m[0], MONB); CHECK(stage_pc("Setup"), "Setup M0");
  memcpy(slot_at(11), m[1], MONB); CHECK(stage_pc("Setup"), "Setup M1");
  memcpy(slot_at(12), m[2], MONB); CHECK(stage_pc("Setup"), "Setup M2");
  memcpy(slot_at(11), m[0], MONB); memset(slot_at(10), 0, MONB);
  CHECK(stage_drop1(), "Swap_1");
  memcpy(slot_at(12), m[1], MONB);
  CHECK(stage_drop1(), "Swap_2");
  CHECK(stage_party_land("Party add", m[0], 100, 3, 0) && jrnapp_flush() == JRN_OK, "Party add of the WRONG mon");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 5u, "#320 chained: a landing of a different mon never pairs (cursor %u)", (unsigned)jrnapp_cursor());
}

/* ---- za #314(b): CHAINED swaps. The displaced mon dropped on another OCCUPIED slot is itself a Swap drop (named "Swap", one slot REPLACED), so a chain of k
 * swaps is k "Swap" steps and a last "Box move" into an empty slot; each link is byte-matched (the replaced bytes of step i == the bytes step i+1 adds).
 * stage_chain(k): M0 at slot 10, M1..Mk at slots 11..10+k (k in 2..4), empty slot 30. Steps: Setup, Swap_1 (M0 -> slot 11, slot 10 cleared, M1 displaced),
 * Swap_2 (M1 -> slot 12, M2 displaced), ..., Box move (Mk -> slot 30). snapS = before Swap_1, snapF = after the Box move. */
static unsigned g_chain_base;   /* the cursor after the Setup steps = the step the chain's undo lands on */
static int stage_chain(unsigned k, unsigned wrong_link) {
  uint8_t m[6][MONB], w[MONB];
  unsigned i;
  for (i = 0; i <= k; i++) mon_fill(m[i], 1 + i);
  mon_fill(w, 40);
  memset(slot_at(30), 0, MONB);
  for (i = 0; i <= k; i++) {                                  /* one Setup step per mon: all together would exceed ONE record (a chain, +seqs) */
    memcpy(slot_at(10 + i), m[i], MONB);
    if (!stage_pc("Setup")) return 0;
  }
  g_chain_base = k + 1u;
  memcpy(snapS, sv, sizeof sv);
  for (i = 0; i < k; i++) {                                   /* Swap_{i+1}: the held M_i replaces M_{i+1} at slot 11+i */
    memcpy(slot_at(11 + i), (wrong_link && i == 1) ? w : m[i], MONB);
    if (i == 0) memset(slot_at(10), 0, MONB);
    if (!stage_drop1()) return 0;
  }
  memcpy(slot_at(30), m[k], MONB);
  if (!stage_pc("Box move")) return 0;
  memcpy(snapF, sv, sizeof sv);
  return jrnapp_flush() == JRN_OK;
}

static void t_swap_chain3(void) {
  char nm[25];
  JaHist rows[8];
  int n, more = 0, fh = 0;
  CHECK(app_world_reset(), "world");
  CHECK(stage_chain(2, 0), "stage a chained swap (setup, Swap, Swap, Box move)");
  CHECK(jrnapp_tip() == g_chain_base + 3u, "Setup steps + Swap, Swap, Box move: tip %u", (unsigned)jrnapp_tip());
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 6 && strcmp(rows[0].name, "Box move 3/3") == 0 && strcmp(rows[1].name, "Swap 2/3") == 0 && strcmp(rows[2].name, "Swap 1/3") == 0 && strcmp(rows[3].name, "Setup") == 0,
        "#314b History labels the three halves 1/3 2/3 3/3 ('%s' '%s' '%s')", n > 2 ? safe(rows[0].name) : "", n > 2 ? safe(rows[1].name) : "", n > 2 ? safe(rows[2].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == g_chain_base, "#314b ONE undo press takes all three steps ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapS, sizeof sv) == 0, "#314b the image is byte-exact the pre-chain image (no half state)");
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == g_chain_base + 3u, "#314b ONE redo press redoes all three ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapF, sizeof sv) == 0, "#314b redo: byte-exact the post-chain image");
  /* mid-chain: per-step undo of the Box move + Swap_2 (the History screen can), then the press redoes the remaining PAIR (Swap_2 is not Box move: it is the
   * chain's tail, c1 = Swap_2 c2 = Box move) */
  CHECK(jrnapp_step(-1, nm) == JRN_OK && jrnapp_step(-1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base + 1u, "per-step undo down to Swap_1");
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == g_chain_base + 3u && memcmp(sv, snapF, sizeof sv) == 0, "#314b mid-chain: the redo press pairs Swap_2 + Box move ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base, "#314b and the undo from the tip is the whole chain again");
  jrnapp_flush();
  CHECK(app_reopen() == 1, "reopen");
}

/* a chain of FOUR (3 Swaps + Box move): the group is capped at three steps; the press never reaches past the cap */
static void t_swap_chain_cap(void) {
  char nm[25];
  CHECK(app_world_reset(), "world");
  CHECK(stage_chain(3, 0), "stage 3 Swaps + a Box move");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base + 1u, "#314b a 4-step chain: the first press takes the newest THREE steps (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base, "#314b the second press takes the remaining Swap alone (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(memcmp(sv, snapS, sizeof sv) == 0, "the image is byte-exact the pre-chain image");
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base + 1u, "#314b redo: the lone Swap first (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base + 4u && memcmp(sv, snapF, sizeof sv) == 0, "#314b then the three-step group, byte-exact (cursor %u)", (unsigned)jrnapp_cursor());
}

/* the false-chain negative: Swap_2 drops a DIFFERENT mon than Swap_1 displaced -> the link is refuted by the bytes; only (Swap_2, Box move) pairs */
static void t_swap_chain_wrong_link(void) {
  char nm[25];
  JaHist rows[8];
  int n, more = 0, fh = 0;
  CHECK(app_world_reset(), "world");
  CHECK(stage_chain(2, 1), "stage Swap, Swap (places a DIFFERENT mon), Box move");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base + 1u, "#314b a refuted link: the press is the plain PAIR, not three steps (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && jrnapp_cursor() == g_chain_base + 3u, "#314b ... and redo likewise (cursor %u)", (unsigned)jrnapp_cursor());
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 6 && strcmp(rows[0].name, "Box move 2/2") == 0 && strcmp(rows[1].name, "Swap 1/2") == 0 && strcmp(rows[2].name, "Swap") == 0,
        "#314b History: only the pair is labelled ('%s' '%s' '%s')", n > 2 ? safe(rows[0].name) : "", n > 2 ? safe(rows[1].name) : "", n > 2 ? safe(rows[2].name) : "");
}

/* all-or-nothing across THREE steps: the third refused -> the two already applied are rolled back; and a refused rollback is the half-swap contract */
static void t_swap_chain_rollback(void) {
  char nm[25];
  uint8_t before[G3_SAVE_FILE_SIZE], b;
  static uint8_t snapH[G3_SAVE_FILE_SIZE];
  CHECK(app_world_reset(), "world");
  CHECK(stage_chain(2, 0), "stage the chain");
  CHECK(sget(0, 5, 4 + 11 * MONB + 10, &b, 1) == 0, "read a byte of slot 11 (Swap_1's replaced slot)");
  b ^= 0x40;
  CHECK(sset(0, 5, 4 + 11 * MONB + 10, &b, 1) == 0, "corrupt it behind the journal's back: the OLDEST step's span no longer matches");
  memcpy(before, sv, sizeof sv);
  CHECK(jrnapp_step_pair(-1, nm) == JRN_E_DIVERGED, "#314b the third step refused: DIVERGED");
  CHECK(jrnapp_cursor() == g_chain_base + 3u && memcmp(sv, before, sizeof sv) == 0, "#314b both applied steps were rolled back: image byte-identical, cursor %u", (unsigned)jrnapp_cursor());
  CHECK(app_world_reset(), "world");
  CHECK(stage_chain(2, 0), "stage the chain again");
  CHECK(jrnapp_step(-1, nm) == JRN_OK, "per-step undo of the newest step (the state the refused press leaves: one step applied)");
  memcpy(snapH, sv, sizeof sv);
  CHECK(app_world_reset(), "world");
  CHECK(stage_chain(2, 0), "stage the chain a third time");
  rd_fail_all_writes = 1;
  {
    int rc = jrnapp_step_pair(-1, nm);
    rd_fail_all_writes = 0;
    CHECK(rc == JRN_OK && strcmp(nm, "half a swap") == 0, "#314b a refused rollback in a 3-step group is the half-swap contract: rc %d '%s'", rc, safe(nm));
    CHECK(jrnapp_cursor() == g_chain_base + 2u && memcmp(sv, snapH, sizeof sv) == 0, "#314b cursor %u (one step applied) and the image exactly the one-step-undone state (journal and image agree)", (unsigned)jrnapp_cursor());
  }
}

/* za review A4: the 3-step "half a swap" contract under EVERY fault point of ONE press. A Swap + Swap + Box move group, pressed
 * (undo, or redo from below the chain) with (a) no planned refusal, or (b) a planned DIVERGED refusal of the LAST step (d = 2: two
 * steps already applied); every sector read of the press is then failed in turn, once (a flaky contact) and for good (a dead card).
 * Contract, checked against the clean image at each cursor (region data, not checksums): a refused press moved NOTHING (cursor and
 * image unchanged); a JRN_OK press left the image exactly the state AT its cursor, named "Swap" only at the group's far end and
 * "half a swap" whenever a rollback failed part way. Before this pin a rollback loop that keeps going after a failure, a "half a
 * swap" name or JRN_OK return reserved for d = 1, all survived the suite (z7 H1..H3). Runs on the first save only (cost). */
#define ZA_DATA (14u * G3_SECTOR_DATA_SIZE)
static void za_digest(uint8_t* out) {
  unsigned r;
  for (r = 0; r < 14u; r++) (void)sget(0, (uint8_t)r, 0, out + r * G3_SECTOR_DATA_SIZE, G3_SECTOR_DATA_SIZE);
}
static uint32_t za_flip_at(unsigned slotn) { return 4u + slotn * MONB + 10u; }   /* PC byte offset of the planted corruption */
static void za_flip_img(uint32_t p) {
  uint8_t b;
  uint8_t region = (uint8_t)(5u + p / G3_SECTOR_DATA_SIZE);
  uint16_t o = (uint16_t)(p % G3_SECTOR_DATA_SIZE);
  (void)sget(0, region, o, &b, 1); b ^= 0x40; (void)sset(0, region, o, &b, 1);
}
static void t_swap_chain_rollback_sweep(void) {
  static int done;
  static uint8_t snap[4][ZA_DATA], img[ZA_DATA], exp[ZA_DATA], pre[G3_SAVE_FILE_SIZE];
  char nm[25];
  unsigned scen, mode, bad = 0, half = 0, presses = 0, deg_swap = 0, deg_box = 0, refused = 0;
  int i;
  if (done) return;
  done = 1;
  CHECK(app_world_reset() && stage_chain(2, 0), "sweep: the clean chain");
  za_digest(snap[3]);
  for (i = 2; i >= 0; i--) { CHECK(jrnapp_step(-1, nm) == JRN_OK, "sweep: clean per-step undo"); za_digest(snap[i]); }   /* snap[c - g_chain_base] */
  for (scen = 0; scen < 4; scen++) {                          /* 0 undo, 1 undo + the oldest refused, 2 redo, 3 redo + the newest refused */
    const int dir = scen < 2 ? -1 : 1;
    const uint32_t p = scen == 1 ? za_flip_at(11) : za_flip_at(30);   /* slot 11: only Swap_1 writes it; slot 30: only the Box move */
    long k, kmax = 0;
    CHECK(app_world_reset() && stage_chain(2, 0), "sweep: stage");   /* staged ONCE per scenario; every press restores card + image */
    if (dir > 0) { for (i = 0; i < 3; i++) (void)jrnapp_step(-1, nm); }
    CHECK(jrnapp_flush() == JRN_OK, "sweep: flush");
    memcpy(pre, sv, sizeof sv);
    rd_snapshot();
    for (mode = 0; mode < 2; mode++) {
      for (k = 0; k <= kmax + 2; k++) {
        uint32_t start, c;
        int rc;
        rd_restore(); memcpy(sv, pre, sizeof sv); card_remount();
        CHECK(app_reopen() && jrnapp_cursor() == (dir < 0 ? g_chain_base + 3u : g_chain_base), "sweep: reopen at the press's start");
        if (scen & 1u) za_flip_img(p);
        start = jrnapp_cursor();
        if (k == 0 && mode == 0) {                            /* the healthy press sizes the sweep */
          unsigned long r0 = rd_reads;
          rc = jrnapp_step_pair(dir, nm);
          kmax = (long)(rd_reads - r0);
          CHECK(scen & 1u ? rc != JRN_OK && jrnapp_cursor() == start : rc == JRN_OK && strcmp(nm, "Swap") == 0,
                "sweep scen %u: the healthy press (rc %d '%s')", scen, rc, safe(nm));
          continue;
        }
        if (mode == 0) rd_fail_read_at = k - 1; else rd_fail_reads_after = k - 1;
        rc = jrnapp_step_pair(dir, nm);
        rd_fail_read_at = -1; rd_fail_reads_after = -1;
        presses++;
        c = jrnapp_cursor();
        if (c < g_chain_base || c > g_chain_base + 3u) { bad++; continue; }
        memcpy(exp, snap[c - g_chain_base], ZA_DATA);
        if (scen & 1u) exp[(5u + p / G3_SECTOR_DATA_SIZE) * G3_SECTOR_DATA_SIZE + p % G3_SECTOR_DATA_SIZE] ^= 0x40;
        za_digest(img);
        if (memcmp(img, exp, ZA_DATA) != 0) { bad++; printf("  sweep scen %u mode %u k %ld: image is not the state at cursor %u\n", scen, mode, k, (unsigned)c); continue; }
        if (rc != JRN_OK) { refused++; if (c != start) { bad++; printf("  sweep scen %u mode %u k %ld: rc %d says nothing moved, cursor %u -> %u\n", scen, mode, k, rc, (unsigned)start, (unsigned)c); } continue; }
        if (c == (dir < 0 ? g_chain_base : g_chain_base + 3u)) { if (strcmp(nm, "Swap") != 0) bad++; continue; }
        if (strcmp(nm, "half a swap") == 0) half++;
        else if (strcmp(nm, "Swap") == 0) deg_swap++;        /* #322: a walk read fault DEGRADED the group (a smaller group / a plain step) and the press still toasted success */
        else deg_box++;
      }
    }
  }
  CHECK(bad == 0 && half > 0, "za A4: %u of %u faulted 3-step presses broke the contract (%u loud half-swaps)", bad, presses, half);
  CHECK(deg_swap == 0 && deg_box == 0, "#322 a walk read fault REFUSES the press, never a smaller group with a success toast: %u 'Swap' (subgroup/plain) + %u 'Box move' degraded presses", deg_swap, deg_box);
  printf("  za A4 sweep: %u faulted 3-step presses, %u contract breaks, %u loud 'half a swap', %u refused, %u degraded 'Swap', %u degraded 'Box move'\n", presses, bad, half, refused, deg_swap, deg_box);
}

/* ---- zc #322: the TWO-step pair under every read fault of ONE press (the 3-step group is t_swap_chain_rollback_sweep), and the History walk under every read fault.
 * Contract: a refused press moved NOTHING; a JRN_OK press left the image exactly the state AT its cursor and is named "Swap" only at the pair's far end
 * ("half a swap" when a rollback failed) -- never a plain step toasting success on a half state (the pre-zc degrade: "Box move" / "Swap" from a smaller group).
 * History under a fault: the rows are always a clean PREFIX of the healthy rows (same seq/parent; a label may lose its " x/y" pair suffix, never gain or change),
 * never a wrong row. */
static void t_swap_pair_fault_sweep(void) {
  static uint8_t snap[4][ZA_DATA], img[ZA_DATA], pre[G3_SAVE_FILE_SIZE];
  static JaHist clean[16], got[16];
  char nm[25];
  unsigned scen, mode, bad = 0, half = 0, presses = 0, deg = 0, refused = 0, hbad = 0, hruns = 0;
  int i, cn, n, more, fh;
  CHECK(app_world_reset() && stage_swap(10, 11, 12), "pair sweep: the clean swap");
  za_digest(snap[3]);
  for (i = 2; i >= 1; i--) { CHECK(jrnapp_step(-1, nm) == JRN_OK, "pair sweep: clean per-step undo"); za_digest(snap[i]); }
  for (scen = 0; scen < 2; scen++) {                          /* 0 undo from the tip, 1 redo from the Setup */
    const int dir = scen ? 1 : -1;
    long k, kmax = 0;
    CHECK(app_world_reset() && stage_swap(10, 11, 12), "pair sweep: stage");
    if (dir > 0) { CHECK(jrnapp_step(-1, nm) == JRN_OK && jrnapp_step(-1, nm) == JRN_OK, "pair sweep: down to the Setup"); }
    CHECK(jrnapp_flush() == JRN_OK, "pair sweep: flush");
    memcpy(pre, sv, sizeof sv);
    rd_snapshot();
    for (mode = 0; mode < 2; mode++) {
      for (k = 0; k <= kmax + 2; k++) {
        uint32_t start, c;
        int rc;
        rd_restore(); memcpy(sv, pre, sizeof sv); card_remount();
        CHECK(app_reopen() && jrnapp_cursor() == (dir < 0 ? 3u : 1u), "pair sweep: reopen at the press's start");
        start = jrnapp_cursor();
        if (k == 0 && mode == 0) {
          unsigned long r0 = rd_reads;
          rc = jrnapp_step_pair(dir, nm);
          kmax = (long)(rd_reads - r0);
          CHECK(rc == JRN_OK && strcmp(nm, "Swap") == 0, "pair sweep scen %u: the healthy press (rc %d '%s')", scen, rc, safe(nm));
          continue;
        }
        if (mode == 0) rd_fail_read_at = k - 1; else rd_fail_reads_after = k - 1;
        rc = jrnapp_step_pair(dir, nm);
        rd_fail_read_at = -1; rd_fail_reads_after = -1;
        presses++;
        c = jrnapp_cursor();
        if (c < 1u || c > 3u) { bad++; continue; }
        za_digest(img);
        if (memcmp(img, snap[c], ZA_DATA) != 0) { bad++; printf("  pair sweep scen %u mode %u k %ld: image is not the state at cursor %u\n", scen, mode, k, (unsigned)c); continue; }
        if (rc != JRN_OK) { refused++; if (c != start) bad++; continue; }
        if (c == (dir < 0 ? 1u : 3u)) { if (strcmp(nm, "Swap") != 0) bad++; continue; }
        if (strcmp(nm, "half a swap") == 0) half++; else deg++;
      }
    }
  }
  CHECK(bad == 0 && deg == 0, "#322 pair sweep: %u contract breaks, %u degraded (success toast on a half state) of %u faulted presses", bad, deg, presses);
  printf("  zc pair sweep: %u faulted 2-step presses, %u contract breaks, %u refused, %u loud 'half a swap', %u degraded\n", presses, bad, refused, half, deg);
  /* History under every read fault */
  CHECK(app_world_reset() && stage_swap(10, 11, 12), "history sweep: the clean swap");
  more = 0; fh = 0;
  cn = jrnapp_history_tree(clean, 16, &more, &fh, 0, 0);
  CHECK(cn == 3 && strcmp(clean[0].name, "Box move 2/2") == 0, "history sweep: the clean tree has 3 rows (%d, '%s')", cn, cn > 0 ? safe(clean[0].name) : "");
  memcpy(pre, sv, sizeof sv);
  rd_snapshot();
  {
    long k, kmax = 0;
    for (k = 0; k <= kmax + 2; k++) {
      unsigned long r0;
      rd_restore(); memcpy(sv, pre, sizeof sv); card_remount();
      CHECK(app_reopen(), "history sweep: reopen");
      r0 = rd_reads;
      if (k) rd_fail_read_at = k - 1;
      more = 0; fh = 0;
      n = jrnapp_history_tree(got, 16, &more, &fh, 0, 0);
      rd_fail_read_at = -1;
      if (!k) { kmax = (long)(rd_reads - r0); continue; }
      hruns++;
      if (n < 0 || n > cn) { hbad++; continue; }
      for (i = 0; i < n; i++) {
        size_t bl = strlen(clean[i].name);
        if (bl >= 4u && clean[i].name[bl - 4u] == ' ' && clean[i].name[bl - 2u] == '/') bl -= 4u;
        if (got[i].seq != clean[i].seq || got[i].parent != clean[i].parent || strncmp(got[i].name, clean[i].name, bl) != 0) { hbad++; break; }
      }
    }
  }
  CHECK(hbad == 0, "#322 History under a read fault: the rows are always a clean prefix (%u of %u faulted walks showed a wrong row)", hbad, hruns);
  printf("  zc history sweep: %u faulted History walks, %u wrong rows\n", hruns, hbad);
}


/* za review A3: a journal recorded BEFORE #314a (main's drop_held never renamed drop 1: both halves "Box move", as stage_pc()
 * writes them) stays per-step: plain History labels, one step per press both ways, a plain chained swap too, and the load-time
 * offer still restores a stranded half byte-exact. */
static void t_swap_pair_old_journal(void) {
  char nm[25], stop[25];
  JaHist rows[8];
  uint32_t av = 0, tot;
  int n, more = 0, fh = 0;
  uint8_t x[MONB], y[MONB], z[MONB];
  mon_fill(x, 1); mon_fill(y, 2); mon_fill(z, 3);
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB); CHECK(stage_pc("Setup"), "setup");
  memcpy(snapS, sv, sizeof sv);
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_pc("Box move"), "a pre-za drop 1 (named Box move)");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2");
  memcpy(snapF, sv, sizeof sv);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 3 && strcmp(rows[0].name, "Box move") == 0 && strcmp(rows[1].name, "Box move") == 0, "za A3 an old swap keeps plain labels ('%s' '%s')", n > 1 ? safe(rows[0].name) : "", n > 1 ? safe(rows[1].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Box move") == 0 && jrnapp_cursor() == 2u, "za A3 an old swap: ONE step per undo press (cursor %u)", (unsigned)jrnapp_cursor());
  CHECK(jrnapp_flush() == JRN_OK && app_reopen(), "reopen on the stranded half");
  tot = jrnapp_offer(&av, stop);
  CHECK(tot == 1u && av == 1u && jrnapp_reapply() == 1 && memcmp(sv, snapF, sizeof sv) == 0, "za A3 the offer restores the old half byte-exact (total %u avail %u)", (unsigned)tot, (unsigned)av);
  CHECK(app_world_reset(), "world");
  memcpy(slot_at(10), x, MONB); CHECK(stage_pc("Setup"), "s"); memcpy(slot_at(11), y, MONB); CHECK(stage_pc("Setup"), "s");
  memcpy(slot_at(12), z, MONB); CHECK(stage_pc("Setup"), "s");
  memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB); CHECK(stage_pc("Box move"), "old S1");
  memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "old S2 (replaces)");
  memcpy(slot_at(30), z, MONB); CHECK(stage_pc("Box move"), "B3");
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == 5u && jrnapp_step_pair(1, nm) == JRN_OK && jrnapp_cursor() == 6u,
        "za A3 an old CHAINED swap: one step per press both ways (cursor %u)", (unsigned)jrnapp_cursor());
}

/* ---- za #314(c): the two caps the z7 review left unpinned -------------------------------------------------------------------------------------------------
 * M5, the redo walk's 64-hop cap (JA_REDO_HOPS): a redo pairs only while the cursor is within 64 steps of the tip (D = steps above the cursor); at D = 64 it
 * pairs, at D = 65 it is a plain one-step redo. P plain "Edit" steps (a byte toggled outside every mon slot) sit above a swap; the cursor is put on the Setup
 * step with per-step undos, so D = P + 2.
 * M7, the four-slot cap (JA_PAIR_SLOTS): an OLDER half that touches at most 4 distinct mon slots can pair; a fifth refuses it. */
static int stage_hops(unsigned plain) {
  unsigned i;
  if (!stage_swap(10, 11, 12)) return 0;
  for (i = 0; i < plain; i++) {
    pc[30000u] = (uint8_t)(pc[30000u] + 1u);                                         /* region 11, past the mon area: never a swap half, never a mon slot */
    if (!stage_pc("Edit")) return 0;
  }
  return jrnapp_flush() == JRN_OK;
}
static void t_swap_redo_hop_cap(void) {
  char nm[25];
  unsigned plain, d, i, want_cur;
  for (plain = 62; plain <= 63; plain++) {
    CHECK(app_world_reset(), "world");
    CHECK(stage_hops(plain), "stage a swap with %u plain steps above it", plain);
    d = plain + 2u;                                                         /* steps above the Setup step */
    CHECK(jrnapp_tip() == 1u + 2u + plain, "tip %u", (unsigned)jrnapp_tip());
    for (i = 0; i < d; i++) { if (jrnapp_step(-1, nm) != JRN_OK) break; }
    CHECK(i == d && jrnapp_cursor() == 1u, "per-step undo down to the Setup step (%u undone, cursor %u)", i, (unsigned)jrnapp_cursor());
    want_cur = d <= 64u ? 3u : 2u;                                          /* paired (cursor over both halves) or plain (one step) */
    CHECK(jrnapp_step_pair(1, nm) == JRN_OK && jrnapp_cursor() == want_cur,
          "#314c D = %u: the redo %s (cursor %u, want %u)", d, d <= 64u ? "PAIRS" : "is a plain step", (unsigned)jrnapp_cursor(), want_cur);
    CHECK(strcmp(nm, d <= 64u ? "Swap" : "Swap (half)") == 0, "#323 D = %u: the toast is '%s' (got '%s')", d, d <= 64u ? "Swap" : "Swap (half)", safe(nm));   /* the hop-cap instance */
  }
}

static void t_swap_slot_cap(void) {
  char nm[25];
  uint8_t x[MONB], y[MONB], e[MONB];
  unsigned extra;
  mon_fill(x, 1); mon_fill(y, 2); mon_fill(e, 7);
  for (extra = 2; extra <= 3; extra++) {                                    /* the older half touches 2 + extra slots: A, B and `extra` more (4 or 5) */
    unsigned i;
    CHECK(app_world_reset(), "world");
    memcpy(slot_at(10), x, MONB); memcpy(slot_at(11), y, MONB);
    CHECK(stage_pc("Setup"), "setup X, Y");
    for (i = 0; i < extra; i++) { memcpy(slot_at(20 + i), e, MONB); memset(slot_at(20 + i), 0, 8); }   /* extras whose first 8 bytes are zero: a 0 -> non-zero fill is no REPLACED slot */
    CHECK(stage_pc("Setup"), "setup the extras");
    memcpy(slot_at(11), x, MONB); memset(slot_at(10), 0, MONB);
    for (i = 0; i < extra; i++) memcpy(slot_at(20 + i), e, MONB);           /* the older step fills those 8 bytes in each extra slot (a SPARSE touch: the step stays ONE record) */
    CHECK(stage_drop1(), "the older half touches %u distinct slots", 2u + extra);
    memcpy(slot_at(12), y, MONB); CHECK(stage_pc("Box move"), "drop 2");
    CHECK(jrnapp_flush() == JRN_OK, "flush");
    CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && jrnapp_cursor() == (extra == 2u ? 2u : 3u),
          "#314c an older half touching %u slots %s (cursor %u)", 2u + extra, extra == 2u ? "pairs (the cap is 4)" : "is refused (a fifth slot)", (unsigned)jrnapp_cursor());
  }
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
  CHECK(n == 4 && strcmp(rows[0].name, "Box move") == 0, "the plain move stays 'Box move' ('%s')", safe(rows[0].name));
  CHECK(n == 4 && strcmp(rows[1].name, "Box move 2/2") == 0, "#303 the swap's newer half reads 'Box move 2/2' ('%s')", safe(rows[1].name));
  CHECK(n == 4 && strcmp(rows[2].name, "Swap 1/2") == 0, "#303 the swap's older half reads 'Swap 1/2' ('%s')", safe(rows[2].name));
  CHECK(n == 4 && strcmp(rows[3].name, "Setup") == 0, "the setup is unlabelled ('%s')", safe(rows[3].name));
}

/* ---- z9 (#301 v2 / D10): a bulk step too big for ONE record is recorded as a CHAIN, through the funnel --------------------------------- */
static void t_bulk_chain_funnel(void) {
  JrnRec rec;
  uint8_t post[G3_SAVE_FILE_SIZE];
  poke(3500, 1000, 0x5A);                                                 /* 1,000 contiguous bytes across the section 5/6 seam: > one record */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "stage the bulk edit");
  CHECK(R.state == IREC_OK && R.lost == 0, "the bulk step IS recorded: state %d lost %u (was a GAP before z9)", R.state, (unsigned)R.lost);
  CHECK(jrn_pending(&J) == 0, "a chain goes straight to the tail, not through the pending buffer");
  CHECK(jrn_find(&J, jrn_tip(&J), &rec) == 0 && rec.kind == JRN_KIND_STEP && rec.aux >= 2u && !rec.crossed, "the tip is a chain HEAD (aux %u records), not crossed", (unsigned)rec.aux);
  CHECK(pc_equals(pc), "the staged bytes are in the image");
  memcpy(post, sv, sizeof post);
  CHECK(jrn_undo(&J, &R.img, 0) == JRN_OK && memcmp(sv, orig, sizeof sv) == 0, "ONE undo restores the image byte-for-byte (sections + checksums)");
  CHECK(jrn_redo(&J, &R.img, 0) == JRN_OK && memcmp(sv, post, sizeof sv) == 0, "ONE redo brings the bulk edit back byte-for-byte");
  poke(100, 4, 0x77);                                                     /* a small step above the chain */
  CHECK(img_stage_sections(&F, &R, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "a plain step after the chain");
  CHECK(jrn_flush(&J) == JRN_OK && R.state == IREC_OK, "flush");
  CHECK(jrn_undo(&J, &R.img, 0) == JRN_OK && jrn_undo(&J, &R.img, 0) == JRN_OK && memcmp(sv, orig, sizeof sv) == 0, "two undos (plain, then the chain) land on the original image");
  CHECK(jrn_undo(&J, &R.img, 0) == JRN_E_NOTHING, "nothing below the chain");
}

/* The same through the app half (jrnapp, which binds R.chain = the rumble-paused hook): History shows the chain as ONE row, undo/redo press
 * once per chain, and after a power cut the load-time offer counts and re-applies it as ONE step. */
static void t_chain_through_app(void) {
  JaHist rows[6];
  char nm[25], stop[25];
  uint32_t av = 0, total;
  static uint8_t post[G3_SAVE_FILE_SIZE], mid[G3_SAVE_FILE_SIZE];
  int n, more = 0, fh = 0;
  CHECK(RA.chain != 0, "the app bound the chain hook");
  CHECK(stageA(300), "A (plain)");
  memcpy(mid, sv, sizeof mid);
  poke(3500, 1000, 0x5A);
  CHECK(img_stage_sections(&F, &RA, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "B (bulk)");
  CHECK(RA.state == IREC_OK && RA.lost == 0, "recorded, not a gap: state %d lost %u", RA.state, (unsigned)RA.lost);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  memcpy(post, sv, sizeof post);
  n = jrnapp_history(rows, 6, &more, &fh);
  CHECK(n == 2 && !more && !fh && rows[0].seq > rows[1].seq && strcmp(rows[0].name, "Box move") == 0, "History: the chain is ONE row above the plain step (%d rows, '%s')", n, safe(rows[0].name));
  CHECK(jrnapp_step(-1, nm) == JRN_OK && memcmp(sv, mid, sizeof sv) == 0, "one undo press = the whole chain ('%s')", safe(nm));
  CHECK(jrnapp_step(1, nm) == JRN_OK && memcmp(sv, post, sizeof sv) == 0, "one redo press = the whole chain");
  CHECK(jrnapp_flush() == JRN_OK, "flush the markers");
  memcpy(sv, orig, sizeof sv);                                            /* the power cut: the card kept the original save */
  CHECK(app_reopen(), "reopen on the original image");
  total = jrnapp_offer(&av, stop);
  CHECK(total == 2u && av == 2u, "the offer counts the chain as ONE step: total %u avail %u (plain + chain = 2)", (unsigned)total, (unsigned)av);
  CHECK(jrnapp_reapply() == 2 && memcmp(sv, post, sizeof sv) == 0, "re-apply restores plain + chain byte-exact");
}

/* The #303 contract for a chain: when a chained undo fails part way AND the rollback fails, jrnapp_step reports JRN_OK named "partial step" (the caller
 * re-derives its copies), the image is genuinely partial, and the crossed epoch moved so the next recorded step is a floor. */
static void t_chain_torn_contract(void) {
  static uint8_t post[G3_SAVE_FILE_SIZE], pre[G3_SAVE_FILE_SIZE];
  char nm[25];
  unsigned long r0, Rn;
  long k;
  unsigned torn = 0, loud = 0, ok = 0, bad = 0;
  memcpy(pre, sv, sizeof pre);
  poke(3500, 1000, 0x5A);
  CHECK(img_stage_sections(&F, &RA, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc), "bulk");
  memcpy(post, sv, sizeof post);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  CHECK(app_reopen(), "reopen on the post image (the chain is read from the card)");
  rd_snapshot();
  r0 = rd_reads;
  CHECK(jrnapp_step(-1, nm) == JRN_OK && memcmp(sv, pre, sizeof sv) == 0, "the healthy undo");
  Rn = rd_reads - r0;
  for (k = 0; k < (long)Rn + 1; k++) {
    uint16_t ep0;
    int rc;
    rd_restore(); memcpy(sv, post, sizeof sv); card_remount();
    CHECK(app_reopen(), "reopen k=%ld", k);
    ep0 = RA.epoch;
    rd_fail_reads_after = k;
    rc = jrnapp_step(-1, nm);
    rd_fail_reads_after = -1;
    if (rc == JRN_OK && strcmp(nm, "partial step") == 0) {
      torn++;
      if (memcmp(sv, pre, sizeof sv) == 0 || memcmp(sv, post, sizeof sv) == 0) bad++;
      if (RA.epoch == ep0) bad++;
    } else if (rc == JRN_OK) { ok++; if (memcmp(sv, pre, sizeof sv) != 0) bad++; }
    else { loud++; if (memcmp(sv, post, sizeof sv) != 0) bad++; }
  }
  CHECK(bad == 0, "%u fault points broke the contract (TORN needs a partial image + a crossed epoch; a loud refusal needs the image UNTOUCHED)", bad);
  CHECK(torn > 0 && loud > 0, "the sweep reaches the TORN path (%u), the loud path (%u) and completes (%u)", torn, loud, ok);
}

/* A swap whose older half is itself a CHAIN (its drop also changed 600 unrelated bytes) is NOT paired: the head holds only part of the spans, so the
 * predicate must not judge it (aux != 0 is ineligible). The press is per-step; History keeps plain names. The control (t_swap_pair_chords) pairs. */
static int stage_swap_chained(unsigned a, unsigned b, unsigned c) {
  uint8_t x[MONB], y[MONB];
  unsigned i;
  mon_fill(x, 1); mon_fill(y, 2);
  memset(slot_at(a), 0, MONB); memset(slot_at(b), 0, MONB); memset(slot_at(c), 0, MONB);
  memcpy(slot_at(a), x, MONB); memcpy(slot_at(b), y, MONB);
  if (!stage_pc("Setup")) return 0;
  memcpy(snapS, sv, sizeof sv);
  memcpy(slot_at(b), x, MONB); memset(slot_at(a), 0, MONB);               /* drop 1 ... */
  for (i = 0; i < 600; i++) pc[34000u + i] = (uint8_t)(pc[34000u + i] ^ 0x5Au ^ (uint8_t)i);   /* ... plus 600 bytes elsewhere (region 13, past the mon area): > one record */
  if (!stage_drop1()) return 0;
  memcpy(slot_at(c), y, MONB);                                             /* drop 2 */
  if (!stage_pc("Box move")) return 0;
  memcpy(snapF, sv, sizeof sv);
  return jrnapp_flush() == JRN_OK && RA.state == IREC_OK && RA.lost == 0;
}
static void t_chain_never_pairs(void) {
  char nm[25];
  JaHist rows[8];
  int n, more = 0, fh = 0;
  CHECK(app_world_reset(), "world");
  CHECK(stage_swap_chained(10, 11, 12), "stage the swap whose first drop is a chain (recorded, not a gap)");
  n = jrnapp_history(rows, 8, &more, &fh);
  CHECK(n == 3 && strcmp(rows[0].name, "Box move") == 0 && strcmp(rows[1].name, "Swap") == 0 && strcmp(rows[2].name, "Setup") == 0,
        "History keeps plain names (no 1/2, 2/2): %d rows '%s' '%s'", n, n > 1 ? safe(rows[0].name) : "", n > 1 ? safe(rows[1].name) : "");
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Box move") == 0 && jrnapp_cursor() == 2u, "a chained older half is not paired: ONE press undoes ONE step ('%s', cursor %u)", safe(nm), (unsigned)jrnapp_cursor());
  CHECK(jrnapp_step_pair(-1, nm) == JRN_OK && strcmp(nm, "Swap") == 0 && jrnapp_cursor() == 1u && memcmp(sv, snapS, sizeof sv) == 0, "the next press undoes the whole chain: byte-exact the pre-swap image ('%s')", safe(nm));
  CHECK(jrnapp_step_pair(1, nm) == JRN_OK && jrnapp_step_pair(1, nm) == JRN_OK && memcmp(sv, snapF, sizeof sv) == 0, "and two redo presses restore the post-swap image byte-exact");
}


/* z9 review D1: the two OTHER TORN consumers. The load-time re-apply (jrnapp_reapply) and the History jump (jrnapp_jump) must report a chain
 * whose rollback failed as JRN_E_TORN -- never as a step count / "nothing further changed" -- with the epoch crossed. Persistent read faults are
 * swept across the whole press; every point that leaves a PARTIAL image must say TORN. */
static void t_chain_torn_reapply_and_jump(void) {
  static uint8_t pre[G3_SAVE_FILE_SIZE], mid[G3_SAVE_FILE_SIZE], post[G3_SAVE_FILE_SIZE];
  uint32_t av = 0; char stop[25]; long k; unsigned long r0, Rn; int n, moved; unsigned partial = 0, bad = 0, jpartial = 0, jbad = 0, discarded = 0;
  memcpy(pre, sv, sizeof pre);
  CHECK(stageA(300), "A (plain)");
  memcpy(mid, sv, sizeof mid);
  poke(3500, 1000, 0x5A);
  CHECK(img_stage_sections(&F, &RA, sv, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc) && RA.lost == 0, "B (chain)");
  memcpy(post, sv, sizeof post);
  CHECK(jrnapp_flush() == JRN_OK, "flush");
  memcpy(sv, pre, sizeof sv); card_remount();
  CHECK(app_reopen() && jrnapp_offer(&av, stop) == 2u && av == 2u, "the offer: 2/2");
  rd_snapshot(); r0 = rd_reads;
  CHECK(jrnapp_reapply() == 2 && memcmp(sv, post, sizeof sv) == 0, "the healthy re-apply");
  Rn = rd_reads - r0;
  for (k = 0; k <= (long)Rn; k++) {
    uint16_t ep0;
    rd_restore(); memcpy(sv, pre, sizeof sv); card_remount();
    CHECK(app_reopen(), "reopen k=%ld", k);
    (void)jrnapp_offer(&av, stop);
    ep0 = RA.epoch;
    rd_fail_reads_after = k; n = jrnapp_reapply(); rd_fail_reads_after = -1;
    if (memcmp(sv, pre, sizeof sv) && memcmp(sv, mid, sizeof sv) && memcmp(sv, post, sizeof sv)) {
      ImgFlags L; int fl = -1;
      partial++;
      if (n != JRN_E_TORN || RA.epoch == ep0) { bad++; if (bad <= 3) printf("  re-apply k=%ld: a PARTIAL image reported as %d (epoch %u -> %u)\n", k, n, (unsigned)ep0, (unsigned)RA.epoch); }
      /* z9 fixpass / D10 ruling 9.1+9.2: model the app's TORN branch at the offer. The latch is set (and the image staged); the DISCARD
       * re-reads the card's .sav (`pre`: the bytes the card holds at the offer) over the partial image and ONLY THEN clears the latch;
       * a failed re-read leaves it set. The reconcile that follows must see the CARD's bytes, with valid checksums. */
      memset(&L, 0, sizeof L);
      imgf_partial_set(&L);
      if (!imgf_partial(&L) || !imgf_exit_prompt(&L)) { bad++; printf("  latch k=%ld: set did not latch + stage\n", k); }
      imgf_clear(&L);                                              /* the failed-discard shape: the flags reset, the re-read never happened */
      if (!imgf_partial(&L)) { bad++; printf("  latch k=%ld: imgf_clear cleared the PARTIAL latch (a failed discard must stay latched)\n", k); }
      memcpy(sv, pre, sizeof sv);                                  /* the discard: the card's image is back in full */
      imgf_partial_clear(&L);
      if (imgf_partial(&L) || memcmp(sv, pre, sizeof sv) != 0 || !gen3_verify_full_checksums(sv, slot, &fl)) {
        bad++; printf("  discard k=%ld: the image is not the card's clean bytes after the discard (partial=%d, sect %d)\n", k, imgf_partial(&L), fl); }
      else discarded++;
    }
  }
  CHECK(discarded == partial && partial > 0, "z9 D10/9.1+9.2: at every one of the %u PARTIAL points the modelled offer latches, stays latched through a failed discard, and after the discard the image IS the card's bytes with valid checksums (%u ok)", partial, discarded);
  CHECK(partial > 0 && bad == 0, "D1 re-apply: every PARTIAL image says JRN_E_TORN with the epoch crossed (%u partial points, %u silent)", partial, bad);
  /* the History jump: post -> the root, through the chain's undo */
  rd_restore(); memcpy(sv, post, sizeof sv); card_remount();
  CHECK(app_reopen(), "reopen on post");
  rd_snapshot(); r0 = rd_reads;
  CHECK(jrnapp_jump(0, stop, &moved) == 0 && moved == 2 && memcmp(sv, pre, sizeof sv) == 0, "the healthy jump to the root");
  Rn = rd_reads - r0;
  for (k = 0; k <= (long)Rn; k++) {
    int rc;
    rd_restore(); memcpy(sv, post, sizeof sv); card_remount();
    CHECK(app_reopen(), "reopen k=%ld", k);
    rd_fail_reads_after = k; rc = jrnapp_jump(0, stop, &moved); rd_fail_reads_after = -1;
    if (memcmp(sv, pre, sizeof sv) && memcmp(sv, mid, sizeof sv) && memcmp(sv, post, sizeof sv)) {
      jpartial++;
      if (rc != JRN_E_TORN) { jbad++; if (jbad <= 3) printf("  jump k=%ld: a PARTIAL image reported as rc %d moved %d\n", k, rc, moved); }
    }
  }
  CHECK(jpartial > 0 && jbad == 0, "D1 jump: every PARTIAL image stops the jump with JRN_E_TORN (%u partial points, %u silent)", jpartial, jbad);
}
/* ---- #304: the History TREE (jrnapp_history_tree). Forks on the current branch collapse into ONE summary row under the on-branch step whose parent is the fork
 * point; opened forks add their siblings' first steps; the 48-row window accounts for them; the cost is one shared pass. ---- */
static unsigned g_tseed = 1;
static uint32_t TS(const char* nm, unsigned k) {              /* ONE mon-sized step (a ~230-byte record) in slot k, a fresh mon every time; returns its seq, flushed */
  mon_fill(slot_at(k), 100u + ++g_tseed);
  CHECK(stage_pc(nm), "stage %s", nm);
  CHECK(jrnapp_flush() == JRN_OK, "flush %s", nm);
  return jrnapp_cursor();
}
static void TU(void) { char nm[25]; CHECK(jrnapp_step(-1, nm) == JRN_OK && jrnapp_flush() == JRN_OK, "undo + flush"); }
static int rows_kinds_ok(const JaHist* r, int n, const char* want) {   /* want: one char per row, S F B (step / fork / sibling) */
  int i;
  for (i = 0; i < n; i++) {
    char c = r[i].kind == JH_STEP ? 'S' : r[i].kind == JH_FORK ? 'F' : r[i].kind == JH_SIB ? 'B' : '?';
    if (want[i] != c) return 0;
  }
  return want[n] == 0;
}
static const char* kinds(const JaHist* r, int n) {
  static char b[4][64];
  static int k;
  char* o = b[k++ & 3];
  int i;
  for (i = 0; i < n && i < 62; i++) o[i] = r[i].kind == JH_STEP ? 'S' : r[i].kind == JH_FORK ? 'F' : r[i].kind == JH_SIB ? 'B' : '?';
  o[i] = 0;
  return o;
}

static void t_tree_shapes(void) {
  JaHist rows[48], ref[48];
  uint32_t s1, s2, s3, nw, nw2, op[1];
  int n, nr, more = 0, fh = 0, m2 = 0, f2 = 0;
  CHECK(app_world_reset(), "world");
  s1 = TS("s1", 10); s2 = TS("s2", 11); s3 = TS("s3", 12);
  n = jrnapp_history_tree(rows, 48, &more, &fh, 0, 0);
  nr = jrnapp_history(ref, 48, &m2, &f2);
  CHECK(n == nr && n == 3 && rows_kinds_ok(rows, n, "SSS") && !more == !m2, "TREE: a LINEAR journal has no fork rows and equals jrnapp_history (n %d, kinds %s)", n, kinds(rows, n));
  { int i, same = 1; for (i = 0; i < n; i++) if (rows[i].seq != ref[i].seq || strcmp(rows[i].name, ref[i].name) || rows[i].saved != ref[i].saved || rows[i].ahead != ref[i].ahead || rows[i].at_cursor != ref[i].at_cursor) same = 0;
    CHECK(same, "TREE: every linear row is field-for-field the jrnapp_history row"); }
  TU(); TU();                                                  /* cursor s1, tip s3 */
  nw = TS("NEW", 13);                                          /* NEW's parent is s1: s2-s3 are orphaned */
  n = jrnapp_history_tree(rows, 48, &more, &fh, 0, 0);
  CHECK(n == 3 && rows_kinds_ok(rows, n, "SFS") && rows[0].seq == nw && strcmp(rows[0].name, "NEW") == 0, "FORK-2: NEW, one collapsed summary, s1 (kinds %s)", kinds(rows, n));
  CHECK(rows[1].parent == s1 && rows[1].nsib == 1 && rows[1].open == 0 && rows[1].seq == 0, "FORK-2: the summary names the fork point s1 (%u) and counts 1 (nsib %u open %u)", (unsigned)rows[1].parent, (unsigned)rows[1].nsib, (unsigned)rows[1].open);
  CHECK(rows[2].seq == s1 && rows[0].parent == s1, "FORK-2: the summary sits BELOW the branch child (whose parent is the fork point) and above the fork point itself");
  op[0] = s1;
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1);
  CHECK(n == 4 && rows_kinds_ok(rows, n, "SFBS") && rows[1].open == 1 && rows[1].nsib == 1, "OPEN: NEW, summary(open), one sibling, s1 (kinds %s)", kinds(rows, n));
  CHECK(rows[2].seq == s2 && strcmp(rows[2].name, "s2") == 0 && rows[2].parent == s1 && rows[2].crossed == 0, "OPEN: the sibling row is s2 -- the orphaned tail's FIRST step (seq %u '%s'), never NEW itself nor s3", (unsigned)rows[2].seq, safe(rows[2].name));
  op[0] = s2;
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1);
  CHECK(n == 3 && rows_kinds_ok(rows, n, "SFS") && rows[1].open == 0, "OPEN of an UNRELATED fork point leaves this fork collapsed (kinds %s open %u)", kinds(rows, n), (unsigned)rows[1].open);
  TU();                                                        /* cursor s1 again */
  nw2 = TS("NEW2", 14);                                        /* s1 now has THREE children: s2, NEW, NEW2 */
  op[0] = s1;
  n = jrnapp_history_tree(rows, 48, &more, &fh, 0, 0);
  CHECK(n == 3 && rows_kinds_ok(rows, n, "SFS") && rows[0].seq == nw2 && rows[1].nsib == 2, "FORK-3: the summary counts BOTH other children (nsib %u)", (unsigned)rows[1].nsib);
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1);
  CHECK(n == 5 && rows_kinds_ok(rows, n, "SFBBS") && rows[2].seq == s2 && rows[3].seq == nw, "FORK-3 OPEN: the siblings are s2 then NEW in seq order, NEW2 (the branch child) excluded (kinds %s: %u %u)", kinds(rows, n), (unsigned)rows[2].seq, (unsigned)rows[3].seq);
  (void)s3;
}

/* a fork must not move any JH_STEP row, and a jump still works along the branch only */
static void t_tree_jump_unchanged(void) {
  JaHist rows[48];
  uint32_t s1, s2, op[1];
  int n, more = 0, fh = 0, moved = 0, rc;
  char stop[25];
  CHECK(app_world_reset(), "world");
  s1 = TS("s1", 10); s2 = TS("s2", 11); (void)TS("s3", 12);
  TU(); TU();
  (void)TS("NEW", 13);
  op[0] = s1;
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1);
  CHECK(n == 4 && rows[3].kind == JH_STEP && rows[3].seq == s1, "rows: NEW, summary, sibling, s1");
  rc = jrnapp_jump(rows[3].seq, stop, &moved);                  /* A-jump on the current branch: unchanged */
  CHECK(rc == 0 && jrnapp_cursor() == s1 && moved == 1, "the branch jump to s1 still works (rc %d cursor %u moved %d)", rc, (unsigned)jrnapp_cursor(), moved);
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1);
  CHECK(n == 4 && rows[0].ahead == 1 && rows[3].at_cursor == 1 && rows[2].kind == JH_SIB && rows[2].seq == s2, "after the jump the branch marks follow the cursor; the sibling row is unchanged");
}

/* SAVED: a sibling that IS the saved step carries it; the summary row above a SAVED branch row carries nothing */
static void t_tree_saved(void) {
  JaHist rows[48];
  uint32_t s1, s2, s3, op[1];
  int n, more = 0, fh = 0;
  CHECK(app_world_reset(), "world");
  s1 = TS("s1", 10); s2 = TS("s2", 11); s3 = TS("s3", 12);
  jrnapp_mark_saved();                                         /* the card holds s3 */
  TU(); TU();
  (void)TS("NEW", 13);                                         /* s1's second child NEW; s2-s3 orphaned */
  op[0] = s1;
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1);
  CHECK(n == 4 && rows[2].kind == JH_SIB && rows[2].seq == s2 && rows[2].saved == 0, "the sibling s2 is not the saved step");
  CHECK(rows[0].saved == 0 && rows[1].saved == 0 && rows[3].saved == 0, "nothing on the current branch is SAVED (the card holds an orphaned step)");
  TU(); TU();                                                  /* cursor s1 .. wait: NEW undone -> s1, then s1 undone -> 0 */
  CHECK(jrnapp_cursor() == 0u, "cursor before the first step");
  CHECK(app_world_reset(), "world");
  s1 = TS("s1", 10); s2 = TS("s2", 11);
  jrnapp_mark_saved();                                         /* the card holds s2 */
  TU();                                                        /* cursor s1 */
  (void)TS("N3", 12);                                          /* N3: s2's sibling (parent s1): s2 is SAVED and orphaned */
  op[0] = s1;
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1);
  CHECK(n == 4 && rows_kinds_ok(rows, n, "SFBS") && rows[2].seq == s2 && rows[2].saved == 1, "a sibling that IS the saved step shows SAVED (kinds %s saved %u)", kinds(rows, n), (unsigned)rows[2].saved);
  /* the fork row directly above a SAVED branch row */
  CHECK(app_world_reset(), "world");
  s1 = TS("s1", 10); s2 = TS("s2", 11); s3 = TS("s3", 12);
  TU();                                                        /* cursor s2 */
  jrnapp_mark_saved();                                         /* the card holds s2 */
  (void)TS("N3", 13);                                          /* N3 hangs off s2 beside s3 */
  n = jrnapp_history_tree(rows, 48, &more, &fh, 0, 0);
  CHECK(n == 4 && rows_kinds_ok(rows, n, "SFSS") && rows[2].seq == s2 && rows[2].saved == 1 && rows[1].saved == 0 && rows[3].saved == 1 && rows[0].saved == 0,
        "a fork directly above a SAVED mark: the summary carries no mark, s2 and older read SAVED, NEW (above) does not (kinds %s)", kinds(rows, n));
  (void)s3;
}

/* THE WINDOW. A branch of 4 steps with two forks (at N3 and at s2); every row budget 3..12, collapsed and fully opened, against an independent cost model. */
static void t_tree_window(void) {
  JaHist rows[48];
  uint32_t s1, s2, s3, s4, n3, n4, n5, m4, op[4];
  int n, max, open, more = 0, fh = 0;
  CHECK(app_world_reset(), "world");
  s1 = TS("s1", 10); s2 = TS("s2", 11); s3 = TS("s3", 12); s4 = TS("s4", 13);
  TU(); TU();                                                  /* cursor s2 */
  n3 = TS("N3", 14); n4 = TS("N4", 15); n5 = TS("N5", 16);
  TU(); TU();                                                  /* cursor N3 */
  m4 = TS("M4", 17);                                           /* branch: M4, N3, s2, s1 ; forks: at N3 (N4), at s2 (s3) */
  op[0] = n3; op[1] = s2;
  for (open = 0; open <= 1; open++) {
    for (max = 3; max <= 12; max++) {
      /* the model: branch rows in order with the rows each carries below it */
      int cost[4], k, tot = 0, keep = 0, want_total;
      cost[0] = 1 + 1 + (open ? 1 : 0);                        /* M4 + its summary (+ sibling N4) */
      cost[1] = 1 + 1 + (open ? 1 : 0);                        /* N3 + its summary (+ sibling s3) */
      cost[2] = 1;                                             /* s2 */
      cost[3] = 1;                                             /* s1 */
      for (k = 0; k < 4; k++) { if (tot + cost[k] > max && keep > 0) break; tot += cost[k]; keep++; }
      want_total = tot;
      n = jrnapp_history_tree(rows, max, &more, &fh, open ? op : 0, open ? 2 : 0);
      CHECK(n == want_total && n <= max, "WINDOW open=%d max=%d: %d rows (model %d, never over the budget)", open, max, n, want_total);
      { int i, steps = 0; for (i = 0; i < n; i++) if (rows[i].kind == JH_STEP) steps++;
        CHECK(steps == keep, "WINDOW open=%d max=%d: %d branch rows kept (model %d) -- no row lost beyond the budget, none split from its fork rows", open, max, steps, keep); }
      CHECK(rows[0].kind == JH_STEP && rows[0].seq == m4, "WINDOW open=%d max=%d: the newest branch row is always row 0", open, max);
      CHECK(!more == (keep == 4), "WINDOW open=%d max=%d: more=%d iff older branch rows were dropped (kept %d of 4)", open, max, more, keep);
      { int i, bad = 0; for (i = 0; i < n; i++) if (rows[i].kind == JH_FORK && !(i > 0 && rows[i - 1].kind == JH_STEP && rows[i - 1].parent == rows[i].parent)) bad++;
        CHECK(bad == 0, "WINDOW open=%d max=%d: every summary directly follows the on-branch row whose parent is its fork point", open, max); }
      { int i, bad = 0; for (i = 0; i < n; i++) if (rows[i].kind == JH_SIB && !(i > 0 && (rows[i - 1].kind == JH_FORK || rows[i - 1].kind == JH_SIB) && rows[i].seq != 0 && rows[i].parent == rows[i - 1].parent)) bad++;
        CHECK(bad == 0, "WINDOW open=%d max=%d: every sibling row follows its own summary (right fork point, a real seq)", open, max); }
    }
  }
  n = jrnapp_history_tree(rows, 48, &more, &fh, op, 2);
  CHECK(n == 8 && rows_kinds_ok(rows, n, "SFBSFBSS") && rows[2].seq == n4 && rows[5].seq == s3 && rows[1].parent == n3 && rows[4].parent == s2,
        "TWO opened forks, each with ITS OWN sibling: N4 under the N3 fork, s3 under the s2 fork (kinds %s; %u %u)", kinds(rows, n), (unsigned)rows[2].seq, (unsigned)rows[5].seq);
  (void)s1; (void)s4; (void)n5;
}

/* THE COST: reads of jrnapp_history (main) vs jrnapp_history_tree on journals of 14 / 42 / 70 steps, one fork at the newest end; after a reopen so every read is the card's */
static void t_tree_cost(void) {
  static const int Nrows[3] = { 14, 42, 70 };
  int c;
  for (c = 0; c < 3; c++) {
    JaHist rows[48];
    int i, n, more = 0, fh = 0;
    unsigned long r_main, r_tree, r_open;
    uint32_t op[1];
    CHECK(app_world_reset(), "world");
    for (i = 0; i < Nrows[c]; i++) (void)TS("s", (unsigned)(i % 50));
    TU();
    (void)TS("NEW", 50);                                        /* one fork at the newest end */
    jrnapp_flush();
    CHECK(app_reopen(), "reopen (every read is a card read)");
    rd_reads = 0; n = jrnapp_history(rows, 48, &more, &fh); r_main = rd_reads;
    rd_reads = 0; n = jrnapp_history_tree(rows, 48, &more, &fh, 0, 0); r_tree = rd_reads;
    op[0] = rows[1].parent;
    rd_reads = 0; n = jrnapp_history_tree(rows, 48, &more, &fh, op, 1); r_open = rd_reads;
    printf("  tree cost, %d steps + 1 fork: jrnapp_history %lu sector reads; tree collapsed %lu (+%lu); tree with the fork OPEN %lu (+%lu)  (%d rows)\n",
           Nrows[c], r_main, r_tree, r_tree - r_main, r_open, r_open - r_main, n);
    CHECK(r_tree >= r_main && r_tree - r_main <= 2048u, "TREE cost: the shared pass adds at most one segment-span of reads (%lu extra)", r_tree - r_main);
  }
}

/* ---- zc #324(a): the max > 48 clamp (JA_TREE_ROWS). A 60-step branch asked for max = 64 rows: the tree clamps the request to 48 (the count arrays in s_rec
 * are 48 wide), so at most 48 rows come back, the NEWEST is row 0, `more` says older rows were dropped, and a fork at the newest end still reads right. */
static void t_tree_max_clamp(void) {
  JaHist rows[64];
  uint32_t tip;
  int i, n, more = 0, fh = 0, steps = 0, mc;
  CHECK(app_world_reset(), "world");
  for (i = 0; i < 60; i++) (void)TS("s", (unsigned)(i % 50));
  tip = jrnapp_tip();
  CHECK(tip >= 60u, "60 steps staged (tip %u)", (unsigned)tip);
  memset(rows, 0, sizeof rows);
  n = jrnapp_history_tree(rows, 64, &more, &fh, 0, 0);
  CHECK(n == 48, "#324a max=64 on a 60-step branch: exactly 48 rows come back, not 60/64 (%d)", n);
  CHECK(n > 0 && rows[0].seq == tip && rows[0].kind == JH_STEP, "#324a the newest step is row 0 (seq %u, tip %u)", n > 0 ? (unsigned)rows[0].seq : 0u, (unsigned)tip);
  CHECK(more == 1, "#324a more=1: older steps were dropped (%d)", more);
  for (i = 0; i < 64; i++) if (rows[i].seq || rows[i].name[0]) steps++;
  CHECK(steps == 48, "#324a nothing was written past row 47 (%d rows touched)", steps);
  for (mc = 0, i = 1; i < n; i++) if (rows[i].seq + 1u != rows[i - 1].seq && rows[i].seq != rows[i - 1].parent) mc++;
  CHECK(mc == 0, "#324a the 48 rows are one unbroken parent chain");
  /* the same with ONE fork at the newest end (the tree logic engages): 48 rows total INCLUDING the summary, the newest branch row still first */
  CHECK(app_world_reset(), "world");
  for (i = 0; i < 59; i++) (void)TS("s", (unsigned)(i % 50));
  TU();
  tip = TS("NEW", 50);
  memset(rows, 0, sizeof rows);
  n = jrnapp_history_tree(rows, 64, &more, &fh, 0, 0);
  CHECK(n <= 48 && n >= 47, "#324a max=64 with a fork: at most 48 rows (%d)", n);
  CHECK(n > 1 && rows[0].seq == tip && rows[0].kind == JH_STEP && rows[1].kind == JH_FORK, "#324a the newest branch row, then its collapsed summary (kinds %s)", kinds(rows, n < 4 ? n : 4));
  CHECK(more == 1, "#324a more=1 with a fork too (%d)", more);
}

int main(int argc, char** argv) {
  int a;
  static uint8_t file[G3_SAVE_FILE_SIZE];
  setvbuf(stdout, 0, _IONBF, 0);                              /* a mutant that CRASHES (a row overrun) must still leave its FAIL lines behind */
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
    CHECK(world(file), "world"); t_bulk_chain_funnel();
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
    CHECK(app_world(file), "app world"); t_swap_pair_r1_mislabel();
    CHECK(app_world(file), "app world"); t_swap_seq_adjacency();
    CHECK(app_world(file), "app world"); t_swap_seq_adjacency_chain();
    CHECK(app_world(file), "app world"); t_swap_half_toast();
    CHECK(app_world(file), "app world"); t_party_add_pairs();
    CHECK(app_world(file), "app world"); t_party_add_chain();
    CHECK(app_world(file), "app world"); t_swap_redo_hop_cap();
    CHECK(app_world(file), "app world"); t_swap_slot_cap();
    CHECK(app_world(file), "app world"); t_swap_chain3();
    CHECK(app_world(file), "app world"); t_swap_chain_cap();
    CHECK(app_world(file), "app world"); t_swap_chain_wrong_link();
    CHECK(app_world(file), "app world"); t_swap_chain_rollback();
    CHECK(app_world(file), "app world"); t_swap_chain_rollback_sweep();
    CHECK(app_world(file), "app world"); t_swap_pair_fault_sweep();
    CHECK(app_world(file), "app world"); t_swap_pair_old_journal();
    CHECK(app_world(file), "app world"); t_swap_pair_shape_and_rollback_failure();
    CHECK(app_world(file), "app world"); t_chain_through_app();
    CHECK(app_world(file), "app world"); t_chain_torn_contract();
    CHECK(app_world(file), "app world"); t_chain_torn_reapply_and_jump();
    CHECK(app_world(file), "app world"); t_chain_never_pairs();
    CHECK(app_world(file), "app world"); t_tree_shapes();
    CHECK(app_world(file), "app world"); t_tree_jump_unchanged();
    CHECK(app_world(file), "app world"); t_tree_saved();
    CHECK(app_world(file), "app world"); t_tree_window();
    CHECK(app_world(file), "app world"); t_tree_cost();
    CHECK(app_world(file), "app world"); t_tree_max_clamp();
  }
  printf("%lu checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
