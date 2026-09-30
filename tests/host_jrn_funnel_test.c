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
 *      source/img_stage.c source/gen3_save.c source/journal.c source/journal_undo.c source/journal_fs.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hjf && /tmp/hjf <saves...>
 *
 * tests/host_g3_stage_sites_test.py recompiles this against mutated copies of img_stage.c and requires
 * each mutant to fail (F1..F4). */
#include "jrn_harness.h"
#include "gen3_save.h"
#include "gen3_box.h"
#include "img_stage.h"

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
    CHECK(world(file), "world"); t_scope_is_one_step();
    CHECK(world(file), "world"); t_identical_and_null();
  }
  printf("%lu checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
