/* source/gb_session.c — the RESIDENT-IMAGE editing pipeline — under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbsession_test.c \
 *      source/gb_session.c source/gb_edit.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/data_tables.c -o /tmp/hgbs && /tmp/hgbs
 *
 * WHAT THIS FILE IS FOR, and why the engines' own tests are not enough.
 * --------------------------------------------------------------------
 * host_gen1write_test.c and host_gen2write_test.c already prove each ENGINE against
 * Guy's real cartridge saves, byte for byte. What neither can prove is the COMPOSITION
 * this module introduces and the GBA glue will depend on:
 *
 *     open a resident image -> stage a box -> gb_edit patches ONE slot in place
 *       -> gbs_commit_list -> the image still parses, and only what we asked changed.
 *
 * In particular the Gen-2 half runs a G2Writer whose read AND write callbacks are memcpy
 * against a RAM buffer, which no previous test has ever done — gen2_write.c was written
 * to stream to a FILE, and its header warns that the two pipelines must be budgeted
 * separately rather than shimmed together. This file is the evidence that the shim
 * gb_session.c does write is honest.
 *
 * THE GUARANTEE THAT MATTERS MOST is the boring one: A NO-OP MUST CHANGE ZERO BYTES.
 * The learn KB measured that ~41% of real Gen-1/2 name fields get silently rewritten by
 * a naive open-then-confirm cycle (names collapse at the SEQUENCE level: 'P','K'
 * re-encodes to the single <PK> glyph), and 733 of 2738 real name fields carry junk after
 * their 0x50 terminator that only an untouched fast path preserves. So opening a box and
 * backing out must leave the 32 KiB image bit-identical, checksums included — and that is
 * asserted here over every box of every real save, not sampled.
 *
 * The corpus is Guy's own cartridge dumps. They live OUTSIDE the repo (gitignored at
 * gba-toolkit/roms/gb/) and are only ever read here — every mutation happens on an
 * in-memory copy — so a missing corpus SKIPS rather than fails, but a corpus that IS
 * present must pass perfectly.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_session.h"
#include "gb_edit.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

/* Two independent buffers so a mutation can always be diffed against the untouched
 * original. Sized for 32 KiB + the largest RTC tail either emulator appends. */
static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_orig[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_list[GBS_LIST_BYTES];
static uint8_t g_list2[GBS_LIST_BYTES];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];

static uint32_t load(const char* file) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(g_img, 1, sizeof g_img, f);
  fclose(f);
  memcpy(g_orig, g_img, n);
  return n;
}

/* Is `off` one of the fourteen Gen-1 box-bank checksum bytes (7 per bank: one over the
 * whole bank, then one per box)? Those are the bytes the game writes and never reads,
 * and that a Gen-1 edit deliberately normalises — see the assertion that uses this. */
static bool is_bank_sum_byte(uint32_t off) {
  return (off >= GEN1_OFF_BANK2_SUMS && off < GEN1_OFF_BANK2_SUMS + 7u) ||
         (off >= GEN1_OFF_BANK3_SUMS && off < GEN1_OFF_BANK3_SUMS + 7u);
}

/* Differing bytes that are NOT explained by bank-sum normalisation. */
static uint32_t diff_outside_bank_sums(uint32_t len, uint32_t* first) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i] && !is_bank_sum_byte(i)) { if (!n && first) *first = i; n++; }
  return n;
}

/* How many bytes of the image differ from the pristine copy, and where the first one is. */
static uint32_t diff_count(uint32_t len, uint32_t* first) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { if (!n && first) *first = i; n++; }
  return n;
}

/* ---------------------------------------------------------------- the tests */

static void one_save(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  printf("  -- %s (%u bytes)\n", file, (unsigned)len);

  GbSession s;
  GbsStatus st = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(st == GBS_OK, "gbs_open accepts a real save");
  if (st != GBS_OK) return;
  CHECK(s.gen == expect_gen, "the right generation was detected");

  const int nb = gbs_nboxes(&s);
  const int pb = gbs_party_box(&s);
  CHECK(nb == (expect_gen == GB_GEN1 ? 12 : 14), "box count matches the generation");

  /* ---- 1. THE NO-OP GUARANTEE, over every box AND the party ---------------
   * Stage a box, hand the very same bytes straight back, and require that the whole
   * image is untouched. This is the composition-level form of the promise gb_edit makes
   * per record, and it is what makes "browse a save and back out" safe. */
  int noop_boxes = 0;
  for (int box = 0; box <= nb; box++) {
    int b = (box == nb) ? pb : box;
    if (gbs_load_list(&s, b, g_list) != GBS_OK) continue;   /* unreadable box: not this test's subject */
    GbsStatus c = gbs_commit_list(&s, b, g_list);
    if (c == GBS_ERR_UNWRITABLE) continue;                  /* virgin bank: refused BY DESIGN */
    CHECK(c == GBS_OK, "a no-op commit is accepted");
    noop_boxes++;
  }
  uint32_t first = 0, nd = diff_count(len, &first);
  if (nd) printf("     first differing byte at 0x%04X (%u total)\n", (unsigned)first, (unsigned)nd);
  CHECK(nd == 0, "NO-OP COMMITS CHANGED ZERO BYTES of the whole image");
  CHECK(noop_boxes > 0, "at least one box was actually exercised");

  /* ---- 2. the per-slot lossless guarantee, on real data -------------------
   * gb_load -> gb_commit with no setter in between must be byte-identical for every
   * occupied slot. This is where a name codec that is a perfect per-BYTE inverse still
   * destroys names at the sequence level, so it is asserted on real name fields. */
  int slots = 0;
  for (int box = 0; box <= nb; box++) {
    int b = (box == nb) ? pb : box;
    if (gbs_load_list(&s, b, g_list) != GBS_OK) continue;
    int cnt = gb_list_count(s.gen, g_list, b);
    if (cnt <= 0) continue;
    memcpy(g_list2, g_list, sizeof g_list2);
    for (int slot = 0; slot < cnt; slot++) {
      CHECK(gb_roundtrip_ok(s.gen, g_list, b, slot), "gb_load->gb_commit is byte-identical");
      GbEditMon e;
      if (gb_load(&e, s.gen, g_list2, b, slot)) {
        CHECK(gb_commit(&e, g_list2, b, slot), "an unedited commit is accepted");
        slots++;
      }
    }
    CHECK(memcmp(g_list, g_list2, (size_t)gb_list_size(s.gen, b)) == 0,
          "a whole box round-tripped slot by slot is unchanged");
  }
  printf("     %d box(es) no-op'd, %d occupied slot(s) round-tripped\n", noop_boxes, slots);
  CHECK(diff_count(len, &first) == 0, "the round-trip pass left the image untouched");

  /* ---- 3. A REAL EDIT: it lands, it survives a re-open, nothing else moves -- */
  int ebox = -1, eslot = -1;
  for (int box = 0; box < nb && ebox < 0; box++) {
    if (gbs_box_writable(&s, box) != GBS_OK) continue;
    if (gbs_load_list(&s, box, g_list) != GBS_OK) continue;
    if (gb_list_count(s.gen, g_list, box) > 0) { ebox = box; eslot = 0; }
  }
  if (ebox < 0) { printf("     (no writable occupied box; edit test skipped)\n"); return; }

  GbEditMon e;
  CHECK(gb_load(&e, s.gen, g_list, ebox, eslot), "load the mon to edit");
  /* Defence DV: it is stored in both generations, it is not the HP DV (which is DERIVED
   * from the other four, so editing it is not expressible), and flipping it to a value
   * it does not already hold guarantees a real byte change. */
  uint8_t was = gb_get_dv(&e, GB_DEF);
  uint8_t now = (uint8_t)(was == 15 ? 14 : 15);
  CHECK(gb_set_dv(&e, GB_DEF, now), "set the Defence DV");
  CHECK(gb_commit(&e, g_list, ebox, eslot), "commit the edited slot into the list");
  CHECK(gbs_commit_list(&s, ebox, g_list) == GBS_OK, "the engine accepted the edit");

  /* The RTC tail is the emulator's, never ours: it must be untouched by any edit. */
  if (len > G2_SAVE_SIZE)
    CHECK(memcmp(g_img + G2_SAVE_SIZE, g_orig + G2_SAVE_SIZE, len - G2_SAVE_SIZE) == 0,
          "the RTC tail was not touched");

  /* Re-open the mutated image from scratch — a fresh parse, not the session's cached
   * view — and read the value back. This is what proves the edit reached the bytes the
   * GAME will read, not just the ones we happened to write. */
  GbSession s2;
  CHECK(gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK,
        "the edited image still parses (checksums valid)");
  CHECK(s2.gen == expect_gen, "the edited image is still the same generation");
  CHECK(gbs_load_list(&s2, ebox, g_list2) == GBS_OK, "re-stage the edited box");
  GbEditMon e2;
  CHECK(gb_load(&e2, s2.gen, g_list2, ebox, eslot), "re-load the edited mon");
  CHECK(gb_get_dv(&e2, GB_DEF) == now, "THE EDIT SURVIVED A FULL RE-OPEN");

  /* And nothing outside the boxes moved. The differing bytes must all sit in the box's
   * own destinations or in a checksum the game itself maintains; an edit that also
   * rewrote the player's name or the party would show up as a much larger diff. A Gen-1
   * open box is stored twice (0x30C0 and its bank slot), so allow for two copies of one
   * record plus the handful of checksum bytes. */
  nd = diff_count(len, &first);
  printf("     edit touched %u byte(s), first at 0x%04X\n", (unsigned)nd, (unsigned)first);
  CHECK(nd > 0, "the edit actually changed something");
  CHECK(nd <= 64, "the edit was surgical (no wholesale rewrite)");

  /* ---- 4. putting it back is also a real edit, and restores the bytes ------ */
  CHECK(gb_set_dv(&e2, GB_DEF, was), "set the Defence DV back");
  CHECK(gb_commit(&e2, g_list2, ebox, eslot), "commit the restoration");
  CHECK(gbs_commit_list(&s2, ebox, g_list2) == GBS_OK, "the engine accepted the restoration");
  /* EVERY Pokemon byte must be back. The only bytes allowed to differ are the Gen-1
   * box-bank checksums, which an edit normalises ON PURPOSE and an undo therefore does
   * not un-normalise: the game writes those fourteen bytes and never reads them, and ten
   * of the fourteen are already wrong in Guy's real Red.sav, so gen1_write leaves them
   * in "the state the game itself would leave". Asserting == 0 here would be asserting a
   * falsehood; asserting "nothing outside the bank sums moved" is the real invariant,
   * and it is strictly stronger than a byte-count bound. */
  nd = diff_count(len, &first);
  uint32_t outside = diff_outside_bank_sums(len, &first);
  if (nd) printf("     after restore: %u byte(s) differ, %u outside the bank sums\n",
                 (unsigned)nd, (unsigned)outside);
  CHECK(outside == 0, "EDIT THEN UNDO RESTORES EVERY BYTE EXCEPT THE NORMALISED BANK SUMS");
  if (s.gen == GB_GEN2)
    CHECK(nd == 0, "Gen 2 has no such exception: edit then undo is byte-exact");
}

/* A file that is the right SIZE but is not a Game Boy save at all must be refused —
 * the browser forks on size alone, so this is the guard that stands behind that. */
static void rejects_garbage(void) {
  for (uint32_t i = 0; i < G2_SAVE_SIZE; i++) g_img[i] = (uint8_t)(i * 7u + (i >> 5));
  GbSession s;
  CHECK(gbs_open(&s, g_img, G2_SAVE_SIZE, g_scratch, sizeof g_scratch) == GBS_ERR_NOT_GB,
        "a 32 KiB file of noise is refused, not parsed");
  memset(g_img, 0, G2_SAVE_SIZE);
  CHECK(gbs_open(&s, g_img, G2_SAVE_SIZE, g_scratch, sizeof g_scratch) == GBS_ERR_NOT_GB,
        "an all-zero 32 KiB file is refused");
  /* And the argument gates hold. */
  CHECK(gbs_open(&s, g_img, G2_SAVE_SIZE, g_scratch, 8) == GBS_ERR_ARG,
        "too small a scratch is refused at open, not later");
  CHECK(gbs_open(&s, NULL, G2_SAVE_SIZE, g_scratch, sizeof g_scratch) == GBS_ERR_ARG,
        "a NULL image is refused");
}

int main(void) {
  printf("gb_session (resident-image edit pipeline)\n");

  rejects_garbage();

  one_save("Red.sav",     GB_GEN1);
  one_save("Yellow.sav",  GB_GEN1);
  one_save("Gold.sav",    GB_GEN2);   /* 32816 bytes — the 48-byte RTC tail case */
  one_save("Crystal.sav", GB_GEN2);

  if (!g_ran) printf("  (no corpus present — structural checks only)\n");
  printf("%s: %d/%d checks passed over %d save(s)\n",
         g_fail ? "FAIL" : "ok", g_check - g_fail, g_check, g_ran);
  return g_fail ? 1 : 0;
}
