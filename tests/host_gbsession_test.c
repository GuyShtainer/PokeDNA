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
#include "gen1_write.h"   /* Gen1EditMon, gen1_edit_load, G1R_* -- the S3 Gen-1 conversion test */

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

/* ---- S3: list surgery (gbs_delete / gbs_move) ------------------------------
 * A second scratch buffer so a re-open session (proving the mutated image still parses)
 * never shares a live G2Writer scratch with the session that just wrote it, and three
 * more staging buffers so gbs_move's two-list contract never aliases gbs_delete's one. */
static uint8_t g_scratch2[GBS_SCRATCH_BYTES];
static uint8_t g_list3[GBS_LIST_BYTES];
static uint8_t g_list4[GBS_LIST_BYTES];
static uint8_t g_snap[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];

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

/* ============================================================================
 * S3 — list surgery: gbs_delete / gbs_move
 * ========================================================================== */

/* Do slot `sa` of `la` (box `boxa`) and slot `sb` of `lb` (box `boxb`) hold the SAME
 * Pokemon — species-list byte, record, OT name and nickname all byte-identical? Used to
 * prove compaction (a survivor now at index i is the original's index i+1) and to prove
 * a moved mon's bytes crossed intact. */
static bool slot_bytes_equal(uint8_t gen, const uint8_t* la, int boxa, int sa,
                             const uint8_t* lb, int boxb, int sb) {
  bool party = gb_box_is_party(gen, boxa);
  int  rsz   = gb_rec_size(gen, party);
  int spa = gb_off_species(gen, boxa, sa), spb = gb_off_species(gen, boxb, sb);
  int roa = gb_off_record(gen, boxa, sa),  rob = gb_off_record(gen, boxb, sb);
  int ota = gb_off_otname(gen, boxa, sa),  otb = gb_off_otname(gen, boxb, sb);
  int nka = gb_off_nickname(gen, boxa, sa),nkb = gb_off_nickname(gen, boxb, sb);
  if (spa < 0 || spb < 0 || roa < 0 || rob < 0 || ota < 0 || otb < 0 || nka < 0 || nkb < 0)
    return false;
  if (la[spa] != lb[spb]) return false;
  if (memcmp(la + roa, lb + rob, (size_t)rsz) != 0) return false;
  if (memcmp(la + ota, lb + otb, GB_NAME_BYTES) != 0) return false;
  if (memcmp(la + nka, lb + nkb, GB_NAME_BYTES) != 0) return false;
  return true;
}

/* Every remaining box/party commits its own unedited list back and changes nothing —
 * the composition-level no-op guarantee (one_save's check 1) re-asserted AFTER a whole
 * arc of S3 surgery, so a session that has just deleted and moved Pokemon around still
 * cannot be made to rewrite a byte by opening a box and backing out. */
static void s3_noop_after_surgery(GbSession* s, uint32_t len, const char* tag) {
  memcpy(g_snap, g_img, len);
  int nb = gbs_nboxes(s), pb = gbs_party_box(s), done = 0;
  for (int box = 0; box <= nb; box++) {
    int b = (box == nb) ? pb : box;
    if (gbs_load_list(s, b, g_list) != GBS_OK) continue;
    GbsStatus c = gbs_commit_list(s, b, g_list);
    if (c == GBS_ERR_UNWRITABLE) continue;
    CHECK(c == GBS_OK, "S3 post-surgery: a no-op commit is still accepted");
    done++;
  }
  CHECK(memcmp(g_img, g_snap, len) == 0, "S3 post-surgery: no-op commits changed zero bytes");
  CHECK(done > 0, tag);
}

/* ---- delete: compaction, count, re-parse, and the empty-box / list-mismatch refusals */
static void s3_delete(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S3 delete)\n", file); return; }
  printf("  -- S3 delete: %s\n", file);

  GbSession s;
  GbsStatus os_open = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os_open == GBS_OK, "S3 delete: session opens");
  if (os_open != GBS_OK) return;

  int nb = gbs_nboxes(&s), tested = 0, empty_box = -1;
  for (int box = 0; box < nb; box++) {
    if (gbs_load_list(&s, box, g_list) != GBS_OK) continue;
    int cnt = gb_list_count(s.gen, g_list, box);
    if (cnt < 0) continue;
    if (cnt == 0) { if (empty_box < 0) empty_box = box; continue; }
    if (gbs_box_writable(&s, box) != GBS_OK) continue;   /* virgin bank: not this test */

    memcpy(g_list2, g_list, sizeof g_list2);              /* the pre-delete snapshot */
    GbsStatus st = gbs_delete(&s, box, 0, g_list);
    CHECK(st == GBS_OK, "S3 delete: an occupied slot is accepted");
    if (st != GBS_OK) continue;
    tested++;

    int newcnt = gb_list_count(s.gen, g_list, box);
    CHECK(newcnt == cnt - 1, "S3 delete: count dropped by exactly one");
    bool compact = true;
    for (int i = 0; i < newcnt; i++)
      if (!slot_bytes_equal(s.gen, g_list, box, i, g_list2, box, i + 1)) compact = false;
    CHECK(compact, "S3 delete: survivors compacted (each equals the original's next slot)");

    GbSession s2;
    CHECK(gbs_open(&s2, g_img, len, g_scratch2, sizeof g_scratch2) == GBS_OK,
          "S3 delete: the mutated image still parses");
    CHECK(s2.gen == expect_gen, "S3 delete: still the same generation after delete");
  }
  CHECK(tested > 0, "S3 delete: at least one box was exercised");

  if (empty_box >= 0)
    CHECK(gbs_delete(&s, empty_box, 0, g_list) == GBS_ERR_SLOT,
          "S3 delete: an empty box refuses with GBS_ERR_SLOT");
  else
    printf("     (no naturally empty box; empty-box refusal skipped)\n");

  s3_noop_after_surgery(&s, len, "S3 delete: post-surgery check exercised a box");
}

/* ---- the party floor: trim toward one member, then the next delete must be refused */
static void s3_party_floor(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S3 party floor)\n", file); return; }
  printf("  -- S3 party floor: %s\n", file);

  GbSession s;
  GbsStatus os_open = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os_open == GBS_OK, "S3 floor: session opens");
  if (os_open != GBS_OK) return;
  int pb = gbs_party_box(&s);
  if (gbs_load_list(&s, pb, g_list) != GBS_OK) { printf("     (party unreadable)\n"); return; }
  int cnt = gb_list_count(s.gen, g_list, pb);
  if (cnt <= 0) { printf("     (empty party; floor test skipped)\n"); return; }

  if (expect_gen == GB_GEN1) {
    int guard = GEN1_PARTY_CAPACITY + 1;              /* provable upper bound */
    while (cnt > 1 && guard-- > 0) {
      GbsStatus st = gbs_delete(&s, pb, 0, g_list);
      CHECK(st == GBS_OK, "S3 floor (Gen1): trimming toward one member");
      if (st != GBS_OK) return;
      cnt = gb_list_count(s.gen, g_list, pb);
    }
    CHECK(cnt == 1, "S3 floor (Gen1): trimmed to exactly one party member");
    CHECK(gbs_delete(&s, pb, 0, g_list) == GBS_ERR_PARTY_FLOOR,
          "S3 floor (Gen1): deleting the last party member is refused");
  } else {
    int guard = gb_list_capacity(GB_GEN2, pb) + 1;    /* provable upper bound */
    while (guard-- > 0) {
      cnt = gb_list_count(s.gen, g_list, pb);
      if (cnt <= 0) { printf("     (party emptied via Mail refusals)\n"); break; }
      int nonegg = -1, neggs = 0;
      for (int i = 0; i < cnt; i++) {
        int sp = gb_off_species(GB_GEN2, pb, i);
        if (sp >= 0 && g_list[sp] != G2_LIST_EGG) { neggs++; if (nonegg < 0) nonegg = i; }
      }
      if (nonegg < 0) { printf("     (only Eggs remain; floor test skipped)\n"); break; }
      GbsStatus st = gbs_delete(&s, pb, nonegg, g_list);
      if (neggs == 1) {
        CHECK(st == GBS_ERR_PARTY_FLOOR,
              "S3 floor (Gen2): deleting the last non-Egg member is refused");
        break;
      }
      /* A held Mail item in the real corpus is a legitimate reason to refuse — that is
       * A4's own gate firing correctly, not a test failure. Log it and stop rather than
       * assert a floor breach the data does not actually reach. */
      if (st != GBS_OK) {
        CHECK(st == GBS_ERR_MAIL, "S3 floor (Gen2): a non-floor refusal while trimming is Mail");
        printf("     (stopped trimming: %s)\n", gbs_status_text(st));
        break;
      }
    }
  }
  s3_noop_after_surgery(&s, len, "S3 floor: post-surgery check exercised a box");
}

/* ---- move: box<->box round trip, with compaction and byte-exact arrival ---------- */
static void s3_move_box_to_box(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S3 move box<->box)\n", file); return; }
  printf("  -- S3 move box<->box: %s\n", file);
  (void)expect_gen;

  GbSession s;
  GbsStatus os_open = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os_open == GBS_OK, "S3 move: session opens");
  if (os_open != GBS_OK) return;
  int nb = gbs_nboxes(&s), moved = 0;

  for (int box = 0; box < nb && moved < 3; box++) {
    if (gbs_box_writable(&s, box) != GBS_OK) continue;
    if (gbs_load_list(&s, box, g_list) != GBS_OK) continue;
    int cnt = gb_list_count(s.gen, g_list, box);
    if (cnt <= 0) continue;
    memcpy(g_list2, g_list, sizeof g_list2);            /* source, before the move */

    int dst = -1, dcount0 = -1;
    for (int b2 = 0; b2 < nb; b2++) {
      if (b2 == box || gbs_box_writable(&s, b2) != GBS_OK) continue;
      if (gbs_load_list(&s, b2, g_list3) != GBS_OK) continue;
      int c2 = gb_list_count(s.gen, g_list3, b2);
      if (c2 >= 0 && c2 < gb_list_capacity(s.gen, b2)) { dst = b2; dcount0 = c2; break; }
    }
    if (dst < 0) continue;

    int to_slot = -1;
    GbsStatus st = gbs_move(&s, box, 0, dst, &to_slot, g_list, g_list3);
    CHECK(st == GBS_OK, "S3 move (box->box): accepted");
    if (st != GBS_OK) continue;
    moved++;

    CHECK(gb_list_count(s.gen, g_list3, dst) == dcount0 + 1,
          "S3 move: destination count is +1");
    CHECK(to_slot == dcount0, "S3 move: landed at the expected (appended) slot");
    CHECK(slot_bytes_equal(s.gen, g_list3, dst, to_slot, g_list2, box, 0),
          "S3 move: arrived byte-identical to the source's original slot 0");

    CHECK(gb_list_count(s.gen, g_list, box) == cnt - 1, "S3 move: source count is -1");
    bool compact = true;
    for (int i = 0; i < cnt - 1; i++)
      if (!slot_bytes_equal(s.gen, g_list, box, i, g_list2, box, i + 1)) compact = false;
    CHECK(compact, "S3 move: source box compacted");

    GbSession s2;
    CHECK(gbs_open(&s2, g_img, len, g_scratch2, sizeof g_scratch2) == GBS_OK,
          "S3 move: both boxes still parse after the move");

    int back_slot = -1;
    GbsStatus bst = gbs_move(&s, dst, to_slot, box, &back_slot, g_list3, g_list4);
    CHECK(bst == GBS_OK, "S3 move: moving it back is accepted");
    if (bst == GBS_OK) {
      CHECK(gb_list_count(s.gen, g_list4, box) == cnt, "S3 move: box count restored");
      CHECK(slot_bytes_equal(s.gen, g_list4, box, back_slot, g_list2, box, 0),
            "S3 move: the mon is back, byte-identical to where it started");
    }
  }
  CHECK(moved > 0, "S3 move: at least one box<->box round trip was exercised");
  s3_noop_after_surgery(&s, len, "S3 move: post-surgery check exercised a box");
}

/* ---- Gen 1: party -> box (allowed, BOXLEVEL synced) and box -> party (refused, untouched) */
static void s3_gen1_party_box(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S3 Gen1 party<->box)\n", file); return; }
  printf("  -- S3 Gen1 party<->box: %s\n", file);

  GbSession s;
  GbsStatus os_open = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os_open == GBS_OK, "S3 Gen1 conv: session opens");
  if (os_open != GBS_OK) return;
  CHECK(s.gen == GB_GEN1, "S3 Gen1 conv: this file really is Gen 1");
  int pb = gbs_party_box(&s);
  if (gbs_load_list(&s, pb, g_list) != GBS_OK || gb_list_count(s.gen, g_list, pb) < 2) {
    printf("     (fewer than 2 party members; party->box test skipped)\n"); return;
  }
  Gen1EditMon e;
  CHECK(gen1_edit_load(g_list, pb, 0, &e), "S3 Gen1 conv: load the mon to move");
  uint8_t live_level = e.rec[G1R_LEVEL];

  int nb = gbs_nboxes(&s), dst = -1;
  for (int b = 0; b < nb; b++) {
    if (gbs_box_writable(&s, b) != GBS_OK) continue;
    if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
    int c = gb_list_count(s.gen, g_list3, b);
    if (c >= 0 && c < gb_list_capacity(s.gen, b)) { dst = b; break; }
  }
  if (dst < 0) { printf("     (no writable box with room; skipped)\n"); return; }

  int to_slot = -1;
  GbsStatus st = gbs_move(&s, pb, 0, dst, &to_slot, g_list, g_list3);
  CHECK(st == GBS_OK, "S3 Gen1 conv: party -> box is accepted");
  if (st == GBS_OK) {
    int ro = gb_off_record(GB_GEN1, dst, to_slot);
    CHECK(ro >= 0 && g_list3[ro + G1R_BOXLEVEL] == live_level,
          "S3 Gen1 conv: the box record's BOXLEVEL was synced to the live level");
    GbSession s2;
    CHECK(gbs_open(&s2, g_img, len, g_scratch2, sizeof g_scratch2) == GBS_OK,
          "S3 Gen1 conv: image parses after party->box");

    memcpy(g_snap, g_img, len);
    int t2 = -1;
    GbsStatus st2 = gbs_move(&s, dst, to_slot, pb, &t2, g_list3, g_list4);
    CHECK(st2 == GBS_ERR_NEEDS_BASE, "S3 Gen1 conv: box -> party is refused (no base stats)");
    CHECK(memcmp(g_img, g_snap, len) == 0, "S3 Gen1 conv: the refused move touched nothing");
  }
  s3_noop_after_surgery(&s, len, "S3 Gen1 conv: post-surgery check exercised a box");
}

/* ---- Gen 2: box -> party (stats computed, full HP) and party -> box (first 32 bytes) */
static void s3_gen2_party_box(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S3 Gen2 party<->box)\n", file); return; }
  printf("  -- S3 Gen2 party<->box: %s\n", file);

  GbSession s;
  GbsStatus os_open = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os_open == GBS_OK, "S3 Gen2 conv: session opens");
  if (os_open != GBS_OK) return;
  CHECK(s.gen == GB_GEN2, "S3 Gen2 conv: this file really is Gen 2");
  int pb = gbs_party_box(&s);
  if (gbs_load_list(&s, pb, g_list) != GBS_OK) { printf("     (party unreadable)\n"); return; }
  int pcount = gb_list_count(s.gen, g_list, pb);
  if (pcount < 0 || pcount >= gb_list_capacity(s.gen, pb)) {
    printf("     (party full or unreadable; box->party test skipped)\n"); return;
  }

  /* box -> party, ONLY if no current member holds Mail (else GBS_ERR_MAIL is the correct,
   * expected answer and there is nothing more to prove here). */
  bool has_mail = false;
  for (int i = 0; i < pcount; i++) {
    GbEditMon e;
    if (gb_load(&e, GB_GEN2, g_list, pb, i) && gb_get_held_item(&e) >= 0x9e) has_mail = true;
  }
  int nb = gbs_nboxes(&s), srcbox = -1;
  for (int b = 0; b < nb && !has_mail; b++) {
    if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
    if (gb_list_count(s.gen, g_list3, b) > 0) { srcbox = b; break; }
  }
  if (has_mail || srcbox < 0) {
    printf("     (Mail present or no source mon; box->party test skipped)\n");
  } else {
    int to_slot = -1;
    GbsStatus st = gbs_move(&s, srcbox, 0, pb, &to_slot, g_list3, g_list);
    CHECK(st == GBS_OK, "S3 Gen2 conv: box -> party is accepted");
    if (st == GBS_OK) {
      G2Mon m;
      CHECK(g2_list_mon(g_list, pb, to_slot, &m), "S3 Gen2 conv: the new party slot decodes");
      bool nonzero = true;
      for (int i = 0; i < 6; i++) if (m.stats[i] == 0) nonzero = false;
      CHECK(nonzero, "S3 Gen2 conv: computed stats are non-zero");
      CHECK(m.cur_hp == m.stats[0], "S3 Gen2 conv: a freshly converted mon is at full HP");
      GbSession s2;
      CHECK(gbs_open(&s2, g_img, len, g_scratch2, sizeof g_scratch2) == GBS_OK,
            "S3 Gen2 conv: image parses after box->party");
    }
  }

  /* party -> box: the box record is the first 32 bytes of the party record. */
  if (gbs_load_list(&s, pb, g_list) == GBS_OK && gb_list_count(s.gen, g_list, pb) > 0) {
    G2Slot before;
    if (g2w_get(g_list, pb, 0, &before) == G2W_OK) {
      int dst = -1;
      for (int b = 0; b < nb; b++) {
        if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
        int c = gb_list_count(s.gen, g_list3, b);
        if (c >= 0 && c < gb_list_capacity(s.gen, b)) { dst = b; break; }
      }
      if (dst >= 0) {
        int to_slot = -1;
        GbsStatus st = gbs_move(&s, pb, 0, dst, &to_slot, g_list, g_list3);
        CHECK(st == GBS_OK, "S3 Gen2 conv: party -> box is accepted");
        if (st == GBS_OK) {
          int ro = gb_off_record(GB_GEN2, dst, to_slot);
          CHECK(ro >= 0 && memcmp(g_list3 + ro, before.rec, 32) == 0,
                "S3 Gen2 conv: the box record is the party record's first 32 bytes");
        }
      } else {
        printf("     (no writable box with room; party->box skipped)\n");
      }
    }
  }
  s3_noop_after_surgery(&s, len, "S3 Gen2 conv: post-surgery check exercised a box");
}

/* ---- a full destination is refused, and the refusal changes nothing -------------- */
static void s3_full_destination(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S3 full destination)\n", file); return; }
  printf("  -- S3 full destination: %s\n", file);
  (void)expect_gen;

  GbSession s;
  GbsStatus os_open = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os_open == GBS_OK, "S3 full: session opens");
  if (os_open != GBS_OK) return;
  int nb = gbs_nboxes(&s), dst = -1;
  for (int b = 0; b < nb; b++) if (gbs_box_writable(&s, b) == GBS_OK) { dst = b; break; }
  if (dst < 0) { printf("     (no writable box; skipped)\n"); return; }
  int cap = gb_list_capacity(s.gen, dst);

  int guard = cap + nb + 2;                             /* provable upper bound */
  while (guard-- > 0) {
    if (gbs_load_list(&s, dst, g_list) != GBS_OK) break;
    if (gb_list_count(s.gen, g_list, dst) >= cap) break;
    int srcbox = -1;
    for (int b = 0; b < nb; b++) {
      if (b == dst || gbs_box_writable(&s, b) != GBS_OK) continue;
      if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
      if (gb_list_count(s.gen, g_list3, b) > 0) { srcbox = b; break; }
    }
    if (srcbox < 0) break;
    int to_slot = -1;
    if (gbs_move(&s, srcbox, 0, dst, &to_slot, g_list3, g_list) != GBS_OK) break;
  }

  if (gbs_load_list(&s, dst, g_list) != GBS_OK || gb_list_count(s.gen, g_list, dst) < cap) {
    printf("     (not enough Pokemon across boxes to fill one; skipped)\n");
    s3_noop_after_surgery(&s, len, "S3 full: post-surgery check exercised a box");
    return;
  }

  int srcbox = -1;
  for (int b = 0; b < nb; b++) {
    if (b == dst) continue;
    if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
    if (gb_list_count(s.gen, g_list3, b) > 0) { srcbox = b; break; }
  }
  if (srcbox < 0) { printf("     (nothing left to attempt the overflow move; skipped)\n"); return; }

  memcpy(g_snap, g_img, len);
  int to_slot = -1;
  GbsStatus st = gbs_move(&s, srcbox, 0, dst, &to_slot, g_list3, g_list4);
  CHECK(st == GBS_ERR_FULL, "S3 full: moving into a full box is refused");
  CHECK(memcmp(g_img, g_snap, len) == 0, "S3 full: the refused move changed nothing");
  s3_noop_after_surgery(&s, len, "S3 full: post-surgery check exercised a box");
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

  /* ---- S3: list surgery -- each helper does its own fresh load(), so none of these
   * can interfere with another's assertions. */
  s3_delete("Red.sav",     GB_GEN1);
  s3_delete("Yellow.sav",  GB_GEN1);
  s3_delete("Gold.sav",    GB_GEN2);
  s3_delete("Crystal.sav", GB_GEN2);

  s3_party_floor("Red.sav",     GB_GEN1);
  s3_party_floor("Yellow.sav",  GB_GEN1);
  s3_party_floor("Gold.sav",    GB_GEN2);
  s3_party_floor("Crystal.sav", GB_GEN2);

  s3_move_box_to_box("Red.sav",     GB_GEN1);
  s3_move_box_to_box("Yellow.sav",  GB_GEN1);
  s3_move_box_to_box("Gold.sav",    GB_GEN2);
  s3_move_box_to_box("Crystal.sav", GB_GEN2);

  s3_gen1_party_box("Red.sav");
  s3_gen1_party_box("Yellow.sav");
  s3_gen2_party_box("Gold.sav");
  s3_gen2_party_box("Crystal.sav");

  s3_full_destination("Red.sav",     GB_GEN1);
  s3_full_destination("Yellow.sav",  GB_GEN1);
  s3_full_destination("Gold.sav",    GB_GEN2);
  s3_full_destination("Crystal.sav", GB_GEN2);

  if (!g_ran) printf("  (no corpus present — structural checks only)\n");
  printf("%s: %d/%d checks passed over %d save(s)\n",
         g_fail ? "FAIL" : "ok", g_check - g_fail, g_check, g_ran);
  return g_fail ? 1 : 0;
}
