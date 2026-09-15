/* source/gb_session.c — the RESIDENT-IMAGE editing pipeline — under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbsession_test.c \
 *      source/gb_session.c source/gb_edit.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/gen3_edit.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_save.c source/gen3_daycare.c -o /tmp/hgbs && /tmp/hgbs
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
#include "gen3_to_gb.h"   /* S5-B review fix #8: gen3_to_gb -- the production down converter */
#include "gb_sidecar.h"   /* S5-B review fix #8: gbsc_entry_from/gbsc_merge_up               */
#include "gen3_edit.h"    /* gen3_build_mon -- a synthetic but LEGAL Gen-3 record             */

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
/* A THIRD full image + scratch: s3_gen2_party_box's box->party probe needs to free a
 * party slot when the real save is already 6/6 (both Gold.sav and Crystal.sav are), and
 * doing that on its own isolated copy means the probe can never disturb the party->box
 * half of the same test, which keeps using the untouched `s`/g_img. */
static uint8_t g_img2[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_scratch3[GBS_SCRATCH_BYTES];

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

  /* Measured BEFORE this test writes anything: does this G/S/Crystal file's stored
   * backup checksum already disagree with the actual bytes at the mirror destinations?
   * Guy's own Gold.sav (and its VC twin) genuinely does right now — region 1
   * (sPlayerData2) carries 253 stale bytes that nothing in this write surface ever
   * touches (BACKLOG #49 P0; source/gen2_save.c's k_gs_mirror comment has the full
   * derivation). refresh_checksums() re-DESCRIBES whatever is currently at the mirror
   * destinations rather than copying bytes there, so on such a file the VERY FIRST
   * commit below honestly changes the 2-byte backup checksum — a real, one-time,
   * expected event, not a bug. */
  bool pre_stale_backup = false;
  uint32_t backup_off = 0;
  if (expect_gen == GB_GEN2) {
    G2Save sv0;
    g2_detect(g_orig, len, &sv0);
    if (sv0.version != G2_VER_NONE) {
      backup_off = g2_checksum_backup_off(sv0.version);
      pre_stale_backup = !sv0.backup_ok;
    }
  }

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
  if (!pre_stale_backup) {
    CHECK(nd == 0, "NO-OP COMMITS CHANGED ZERO BYTES of the whole image");
  } else {
    CHECK(nd == 2 && (first == backup_off || first == backup_off + 1),
          "no-op commits on a pre-stale backup change ONLY its 2-byte checksum");
    /* Adopt the honestly-resynced checksum as the reference from here on, so this
     * already-proven, already-explained difference does not pollute every check below —
     * they are testing DIFFERENT things (per-slot fidelity, edit surgicalness, undo
     * exactness), not this one. */
    memcpy(g_orig + backup_off, g_img + backup_off, 2);
    nd = diff_count(len, &first);
  }
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

/* gbs_is_mail_item's own boundary: FLOWER_MAIL (0x9e) is isolated, so its immediate
 * neighbour 0x9f (LEVEL_BALL, item_constants.asm:167) must NOT read as Mail; the
 * contiguous run is 0xb5..0xbd (SURF_MAIL..MIRAGE_MAIL, lines 189-197), so 0xb4
 * (BRICK_PIECE, line 188) is the other just-outside id, and 0xb5/0xbd are the run's own
 * two ends. This is what makes the "NOT one contiguous range" claim testable rather
 * than asserted. */
static void s3_mail_predicate_boundaries(void) {
  CHECK(gbs_is_mail_item(0x9e), "gbs_is_mail_item: FLOWER_MAIL (0x9e) is Mail");
  CHECK(!gbs_is_mail_item(0x9f), "gbs_is_mail_item: 0x9f (LEVEL_BALL) is NOT Mail");
  CHECK(!gbs_is_mail_item(0xb4), "gbs_is_mail_item: 0xb4 (BRICK_PIECE) is NOT Mail");
  CHECK(gbs_is_mail_item(0xb5), "gbs_is_mail_item: SURF_MAIL (0xb5) is Mail");
  CHECK(gbs_is_mail_item(0xbd), "gbs_is_mail_item: MIRAGE_MAIL (0xbd) is Mail");
  CHECK(!gbs_is_mail_item(0x9d), "gbs_is_mail_item: 0x9d (HEAVY_BALL) is NOT Mail");
  CHECK(!gbs_is_mail_item(0xbe), "gbs_is_mail_item: 0xbe (ITEM_BE) is NOT Mail");
}

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

  /* Same pre-existing-staleness exception as one_save() above: if THIS session has not
   * yet resynced a stale G/S backup (region 1 / sPlayerData2, BACKLOG #49 P0), the first
   * commit below still will, honestly, by 2 bytes. */
  bool pre_stale_backup = false;
  uint32_t backup_off = 0;
  if (s->gen == GB_GEN2) {
    G2Save sv0;
    g2_detect(g_snap, len, &sv0);
    if (sv0.version != G2_VER_NONE) {
      backup_off = g2_checksum_backup_off(sv0.version);
      pre_stale_backup = !sv0.backup_ok;
    }
  }

  int nb = gbs_nboxes(s), pb = gbs_party_box(s), done = 0;
  for (int box = 0; box <= nb; box++) {
    int b = (box == nb) ? pb : box;
    if (gbs_load_list(s, b, g_list) != GBS_OK) continue;
    GbsStatus c = gbs_commit_list(s, b, g_list);
    if (c == GBS_ERR_UNWRITABLE) continue;
    CHECK(c == GBS_OK, "S3 post-surgery: a no-op commit is still accepted");
    done++;
  }
  if (pre_stale_backup) memcpy(g_snap + backup_off, g_img + backup_off, 2);
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
/* box -> party, run on an ISOLATED copy of the image (its own session, its own g_img2)
 * so it can never disturb the party -> box half below or the shared no-op check that
 * follows it, both of which keep using the caller's own `s`/g_img. Frees a party slot
 * first when the real save's party is already full (both Gold.sav and Crystal.sav are
 * 6/6, which used to skip this whole branch on the corpus) -- deleting a member is
 * gbs_delete(), already proven correct by s3_delete/s3_party_floor, and its own
 * party-floor/Mail refusals apply exactly as they would to a player doing this in-game. */
static void s3_gen2_box_to_party(const char* file, uint32_t len) {
  memcpy(g_img2, g_img, len);
  GbSession bp;
  GbsStatus bo = gbs_open(&bp, g_img2, len, g_scratch3, sizeof g_scratch3);
  CHECK(bo == GBS_OK, "S3 Gen2 conv: isolated box->party probe session opens");
  if (bo != GBS_OK) return;
  int pb = gbs_party_box(&bp), nb = gbs_nboxes(&bp);

  uint8_t plist[GBS_LIST_BYTES];
  if (gbs_load_list(&bp, pb, plist) != GBS_OK) {
    printf("     (%s: party unreadable; box->party skipped)\n", file); return;
  }
  int pcount = gb_list_count(bp.gen, plist, pb);
  if (pcount < 0) { printf("     (%s: party unreadable; box->party skipped)\n", file); return; }

  int freed = -1;
  if (pcount >= gb_list_capacity(bp.gen, pb)) {
    /* Mail on ANY member refuses the WHOLE restructure (gbs_delete's own gate), so try
     * every slot in turn -- a real save can hold Mail on one slot without holding it
     * on all six. */
    for (int i = 0; i < pcount && freed < 0; i++) {
      uint8_t dl[GBS_LIST_BYTES];
      if (gbs_delete(&bp, pb, i, dl) == GBS_OK) freed = i;
    }
    CHECK(freed >= 0, "S3 Gen2 conv: freed a full party's slot to make room for box->party");
    if (freed < 0) {
      printf("     (%s: every party member holds Mail; box->party skipped)\n", file); return;
    }
    if (gbs_load_list(&bp, pb, plist) != GBS_OK) return;
    pcount = gb_list_count(bp.gen, plist, pb);
  }

  bool has_mail = false;
  for (int i = 0; i < pcount; i++) {
    GbEditMon e;
    if (gb_load(&e, GB_GEN2, plist, pb, i) && gbs_is_mail_item(gb_get_held_item(&e))) has_mail = true;
  }
  int srcbox = -1;
  for (int b = 0; b < nb && !has_mail; b++) {
    uint8_t l[GBS_LIST_BYTES];
    if (gbs_load_list(&bp, b, l) != GBS_OK) continue;
    if (gb_list_count(bp.gen, l, b) > 0) { srcbox = b; break; }
  }
  if (has_mail || srcbox < 0) {
    printf("     (%s: Mail present or no source mon; box->party skipped)\n", file);
    return;
  }

  uint8_t srclist[GBS_LIST_BYTES], dstlist[GBS_LIST_BYTES];
  int to_slot = -1;
  GbsStatus st = gbs_move(&bp, srcbox, 0, pb, &to_slot, srclist, dstlist);
  CHECK(st == GBS_OK, "S3 Gen2 conv: box -> party is accepted");
  if (st != GBS_OK) return;

  /* The real comparison: recompute the landed slot's stats independently (a FRESH
   * gb_recalc_stats over a copy of what gbs_move actually wrote) rather than merely
   * asserting the six numbers are non-zero, which a bad conversion could satisfy too. */
  GbEditMon landed;
  CHECK(gb_load(&landed, bp.gen, dstlist, pb, to_slot), "S3 Gen2 conv: the new party slot loads");
  GbEditMon recompute = landed;
  CHECK(gb_recalc_stats(&recompute), "S3 Gen2 conv: recalculating its stats succeeds");
  bool stats_match = true;
  for (int i = 0; i < 6; i++)
    if (gb_get_stat(&landed, i) != gb_get_stat(&recompute, i)) stats_match = false;
  CHECK(stats_match, "S3 Gen2 conv: the landed stats equal a fresh gb_recalc_stats");

  G2Mon m;
  CHECK(g2_list_mon(dstlist, pb, to_slot, &m),
        "S3 Gen2 conv: the new party slot decodes via the shipping parser");
  CHECK(m.cur_hp == m.stats[0], "S3 Gen2 conv: a freshly converted mon is at full HP");
  CHECK(m.status == 0, "S3 Gen2 conv: a freshly converted mon has healthy status");

  uint8_t scratch4[GBS_SCRATCH_BYTES];
  GbSession s2;
  CHECK(gbs_open(&s2, g_img2, len, scratch4, sizeof scratch4) == GBS_OK,
        "S3 Gen2 conv: image parses after box->party");

  printf("     %s: box->party RAN (freed slot %d, landed at party slot %d)\n",
         file, freed, to_slot);
}

static void s3_gen2_party_box(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S3 Gen2 party<->box)\n", file); return; }
  printf("  -- S3 Gen2 party<->box: %s\n", file);

  GbSession s;
  GbsStatus os_open = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os_open == GBS_OK, "S3 Gen2 conv: session opens");
  if (os_open != GBS_OK) return;
  CHECK(s.gen == GB_GEN2, "S3 Gen2 conv: this file really is Gen 2");
  int pb = gbs_party_box(&s), nb = gbs_nboxes(&s);

  s3_gen2_box_to_party(file, len);

  /* party -> box: the box record is the first 32 bytes of the party record. Always
   * runs on the untouched `s`/g_img, regardless of what the isolated probe above did. */
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

/* ---- S5-B: gbs_insert() — append an already-built BOX-kind record ----------------
 * The design brief allows substituting "a copy of an existing GB slot" for a live
 * gen3_to_gb() conversion when pulling that converter's own dependencies into this
 * test's link line would drag too much: gbs_insert() never looks past `mon->gen` /
 * `mon->is_party` and the four pieces gb_commit_parts() writes, so a GbEditMon loaded
 * straight out of an existing occupied box slot exercises exactly the same path a
 * freshly converted one would. */
static void s5_insert(const char* file, uint8_t expect_gen) {
  /* (a) into a box with room: OK, count+1, gb_verify_slot() agrees it landed intact. */
  {
    uint32_t len = load(file);
    if (!len) { printf("  SKIP %s (S5 insert)\n", file); return; }
    printf("  -- S5 insert: %s\n", file);
    (void)expect_gen;

    GbSession s;
    CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK,
          "S5 insert: session opens");

    int nb = gbs_nboxes(&s), pb = gbs_party_box(&s);
    int srcbox = -1;
    GbEditMon mon;
    for (int b = 0; b < nb; b++) {
      if (b == pb) continue;
      if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
      if (gb_list_count(s.gen, g_list3, b) > 0 && gb_load(&mon, s.gen, g_list3, b, 0)) {
        srcbox = b; break;
      }
    }
    if (srcbox < 0) { printf("     (no box mon to copy; skipped)\n"); return; }
    CHECK(!mon.is_party, "S5 insert: the source record is BOX-kind");

    int dst = -1, dcount0 = -1;
    for (int b = 0; b < nb; b++) {
      if (b == pb || gbs_box_writable(&s, b) != GBS_OK) continue;
      if (gbs_load_list(&s, b, g_list) != GBS_OK) continue;
      int c = gb_list_count(s.gen, g_list, b);
      if (c >= 0 && c < gb_list_capacity(s.gen, b)) { dst = b; dcount0 = c; break; }
    }
    if (dst < 0) { printf("     (no writable box with room; skipped)\n"); return; }

    int slot = -1;
    GbsStatus st = gbs_insert(&s, dst, &mon, &slot, g_list);
    CHECK(st == GBS_OK, "S5 insert: accepted into a box with room");
    if (st == GBS_OK) {
      CHECK(gbs_load_list(&s, dst, g_list4) == GBS_OK, "S5 insert: destination reloads");
      CHECK(gb_list_count(s.gen, g_list4, dst) == dcount0 + 1,
            "S5 insert: destination count is +1");
      CHECK(slot == dcount0, "S5 insert: landed at the expected (appended) slot");
      CHECK(gb_verify_slot(&mon, g_list4, dst, slot),
            "S5 insert: the shipping parser agrees the record landed intact");
    }
  }

  /* (b) into a full box: GBS_ERR_FULL, image byte-identical. */
  {
    uint32_t len = load(file);
    if (!len) return;
    GbSession s;
    if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK) return;
    int nb = gbs_nboxes(&s), pb = gbs_party_box(&s), dst = -1;
    for (int b = 0; b < nb; b++)
      if (b != pb && gbs_box_writable(&s, b) == GBS_OK) { dst = b; break; }
    if (dst < 0) return;
    int cap = gb_list_capacity(s.gen, dst);

    /* Fill `dst` to capacity exactly like s3_full_destination -- a no-op loop when
     * `dst` (the first writable box) already starts full, which is the common case on
     * Guy's real saves. */
    int guard = cap + nb + 2;                             /* provable upper bound */
    while (guard-- > 0) {
      if (gbs_load_list(&s, dst, g_list) != GBS_OK) break;
      if (gb_list_count(s.gen, g_list, dst) >= cap) break;
      int srcbox = -1;
      for (int b = 0; b < nb; b++) {
        if (b == dst || b == pb || gbs_box_writable(&s, b) != GBS_OK) continue;
        if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
        if (gb_list_count(s.gen, g_list3, b) > 0) { srcbox = b; break; }
      }
      if (srcbox < 0) break;
      int to_slot = -1;
      if (gbs_move(&s, srcbox, 0, dst, &to_slot, g_list3, g_list) != GBS_OK) break;
    }
    if (gbs_load_list(&s, dst, g_list) != GBS_OK || gb_list_count(s.gen, g_list, dst) < cap) {
      printf("     (could not fill a box; S5 full-destination skipped)\n");
      return;
    }

    /* THEN, separately, find any mon to attempt the (refused) insert with -- it does
     * not have to be one that took part in filling `dst` above. */
    GbEditMon mon; bool have_mon = false; int srcbox2 = -1;
    for (int b = 0; b < nb; b++) {
      if (b == dst || b == pb || gbs_box_writable(&s, b) != GBS_OK) continue;
      if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
      if (gb_list_count(s.gen, g_list3, b) > 0) { srcbox2 = b; break; }
    }
    if (srcbox2 >= 0) have_mon = gb_load(&mon, s.gen, g_list3, srcbox2, 0);
    if (!have_mon) { printf("     (no mon left to attempt the overflow insert; skipped)\n"); return; }

    memcpy(g_snap, g_img, len);
    int slot = -1;
    GbsStatus st = gbs_insert(&s, dst, &mon, &slot, g_list4);
    CHECK(st == GBS_ERR_FULL, "S5 insert: a full box is refused");
    CHECK(memcmp(g_img, g_snap, len) == 0, "S5 insert: the refused insert changed nothing");
  }

  /* (c) into the party pseudo-box: GBS_ERR_ARG, image byte-identical. */
  {
    uint32_t len = load(file);
    if (!len) return;
    GbSession s;
    if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK) return;
    int nb = gbs_nboxes(&s), pb = gbs_party_box(&s);
    GbEditMon mon; bool have_mon = false;
    for (int b = 0; b < nb; b++) {
      if (b == pb) continue;
      if (gbs_load_list(&s, b, g_list3) != GBS_OK) continue;
      if (gb_list_count(s.gen, g_list3, b) > 0) {
        have_mon = gb_load(&mon, s.gen, g_list3, b, 0); break;
      }
    }
    if (!have_mon) return;

    memcpy(g_snap, g_img, len);
    int slot = -1;
    GbsStatus st = gbs_insert(&s, pb, &mon, &slot, g_list);
    CHECK(st == GBS_ERR_ARG, "S5 insert: the party pseudo-box is refused");
    CHECK(memcmp(g_img, g_snap, len) == 0,
          "S5 insert: the refused party insert changed nothing");
  }
}

/* ---- S5-B review fix #8: round-trip the PRODUCTION COMPOSITION -------------------
 * host_gen3gb_test.c already proves gen3_to_gb() + gbsc_merge_up() agree in isolation,
 * over an in-memory list buffer neither ever touches. What it CANNOT prove is that
 * they still agree once gbs_insert()'s real engine calls -- gen1_blob_apply's
 * GEN1_OP_INSERT or g2w_append, both of which move bytes, shift the terminator, and
 * re-verify against a REAL cartridge save image -- sit between them. This is that one
 * test: gen3_to_gb() -> gbs_insert() -> gb_load() of the slot it actually landed at ->
 * gbsc_entry_from() -> gbsc_merge_up() -> byte-identical to the ORIGINAL 80 bytes,
 * because nothing changed on the Game Boy side between the insert and the merge. */
static void s8_roundtrip(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (S8 production round trip)\n", file); return; }
  printf("  -- S8 production round trip: %s\n", file);

  GbSession s;
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "S8: session opens");
  CHECK(s.gen == expect_gen, "S8: the right generation was detected");
  if (s.gen != GB_GEN2) {
    printf("     (Gen 1 has no gen3_to_gb() target yet -- S5-C; skipped)\n");
    return;
  }

  int nb = gbs_nboxes(&s), pb = gbs_party_box(&s), dst = -1;
  for (int b = 0; b < nb; b++) {
    if (b == pb || gbs_box_writable(&s, b) != GBS_OK) continue;
    if (gbs_load_list(&s, b, g_list) != GBS_OK) continue;
    int c = gb_list_count(s.gen, g_list, b);
    if (c >= 0 && c < gb_list_capacity(s.gen, b)) { dst = b; break; }
  }
  if (dst < 0) { printf("     (no writable box with room; skipped)\n"); return; }

  uint8_t rec80[80];
  gen3_build_mon(1 /* Bulbasaur */, 10, 0x87654321u, 0xBEEF0007u, "S8TEST", 3, rec80);

  GbEditMon mon;
  Gen3ToGbLoss loss;
  G3GbStatus cst = gen3_to_gb(rec80, GB_GEN2, true, NULL, &mon, &loss);
  CHECK(cst == G3GB_OK, "S8: gen3_to_gb accepts the synthetic mon");
  if (cst != G3GB_OK) return;

  int slot = -1;
  GbsStatus ist = gbs_insert(&s, dst, &mon, &slot, g_list);
  CHECK(ist == GBS_OK, "S8: gbs_insert lands it");
  if (ist != GBS_OK) return;

  CHECK(gbs_load_list(&s, dst, g_list) == GBS_OK, "S8: destination reloads");
  GbEditMon landed;
  CHECK(gb_load(&landed, s.gen, g_list, dst, slot), "S8: gb_load reads the landed slot back");

  GbscEntry e;
  gbsc_entry_from(&e, &mon, rec80, 0);

  uint8_t back80[80];
  GbscMergeReport rep;
  CHECK(gbsc_merge_up(&e, &landed, back80, &rep), "S8: gbsc_merge_up succeeds");
  CHECK(memcmp(back80, rec80, 80) == 0,
        "S8: the full production chain round-trips byte-identical");
  CHECK(!rep.evolved && !rep.level_changed && !rep.moves_changed && !rep.renamed &&
        !rep.rename_refused && !rep.gb_item_ignored,
        "S8: the merge report is all-false -- a true no-op");
}

/* ============================================================================
 * BACKLOG #64: gbs_open_streamed() -- a read-only session over a read callback,
 * proven equivalent to the resident-image session over the SAME bytes, and proven
 * to refuse every write entry point without touching the file.
 * ========================================================================== */

/* Mirrors gb_read()'s own shape (source/pdna_gen12.c) -- a plain stdio pread, no
 * seek-then-read races because this test never runs two reads concurrently. */
static bool stdio_rd(void* ctx, uint32_t off, void* buf, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  return fread(buf, 1, len, f) == len;
}

/* Streamed vs resident: same file, two sessions, same answers. */
static void s64_streamed_vs_resident(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (BACKLOG #64 streamed/resident)\n", file); return; }
  g_ran++;
  printf("  -- BACKLOG #64 streamed vs resident: %s\n", file);

  GbSession sr;
  CHECK(gbs_open(&sr, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK,
        "S64: resident session opens");

  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, file);
  FILE* f = fopen(path, "rb");
  CHECK(f != NULL, "S64: the corpus file reopens for the streamed session");
  if (!f) return;

  GbSession ss;
  GbsStatus sst = gbs_open_streamed(&ss, stdio_rd, f, len, g_scratch2, sizeof g_scratch2);
  CHECK(sst == GBS_OK, "S64: streamed session opens over the same bytes");
  CHECK(ss.img == NULL, "S64: a streamed session's img stays NULL (the invariant)");

  CHECK(sr.gen == expect_gen && ss.gen == expect_gen,
        "S64: both sessions detect the same generation");
  CHECK(gbs_nboxes(&sr) == gbs_nboxes(&ss), "S64: gbs_nboxes agrees");
  CHECK(gbs_party_box(&sr) == gbs_party_box(&ss), "S64: gbs_party_box agrees");
  CHECK(gb_session_is_crystal(&sr) == gb_session_is_crystal(&ss),
        "S64: gb_session_is_crystal agrees");

  /* A spread of offsets: trainer name, money, the dex-owned block, a box list, and
   * the last valid byte -- per-generation, since the two engines place these at
   * different fixed offsets (gen1_save.h / gen2_save.h's G2Offsets). */
  uint32_t name_off, money_off, dex_off, list_off;
  if (expect_gen == GB_GEN1) {
    name_off  = GEN1_OFF_PLAYER_NAME;
    money_off = 0x25F3u;          /* docs/GEN12-PARITY-DESIGN.md §1.1, RBY money */
    dex_off   = 0x25A3u;          /* source/gb_fields.c:73 GBF_DEX_OWNED, RBY column */
    list_off  = gen1_list_offset(&sr.g1, 0);
  } else {
    G2Offsets go; g2_offsets(sr.g2w.sv.version, &go);
    name_off  = go.player_name;
    money_off = go.money;
    dex_off   = go.dex_owned;
    list_off  = go.current_box_list;
  }
  struct { const char* tag; uint32_t off; uint32_t n; } spots[] = {
    { "trainer name", name_off,  8   },
    { "money",        money_off, 3   },
    { "dex block",    dex_off,   19  },
    { "box list",     list_off,  16  },
    { "last byte",    len - 1,   1   },
  };
  for (unsigned i = 0; i < sizeof spots / sizeof spots[0]; i++) {
    uint8_t br[19], bs[19];
    CHECK(spots[i].n <= sizeof br, "S64: test buffer wide enough");
    GbsStatus rr = gbs_read_field(&sr, spots[i].off, br, spots[i].n);
    GbsStatus rs = gbs_read_field(&ss, spots[i].off, bs, spots[i].n);
    CHECK(rr == GBS_OK && rs == GBS_OK, spots[i].tag);
    CHECK(memcmp(br, bs, spots[i].n) == 0, "S64: streamed and resident read the same bytes");
  }

  /* gbs_load_list agrees too, over the party AND box 0. */
  int pb = gbs_party_box(&sr);
  int boxes_to_check[2] = { 0, pb };
  for (int i = 0; i < 2; i++) {
    GbsStatus lr = gbs_load_list(&sr, boxes_to_check[i], g_list);
    GbsStatus ls = gbs_load_list(&ss, boxes_to_check[i], g_list2);
    CHECK(lr == ls, "S64: gbs_load_list agrees on status");
    if (lr == GBS_OK && ls == GBS_OK) {
      int n = gb_list_size(expect_gen, boxes_to_check[i]);
      CHECK(n > 0 && memcmp(g_list, g_list2, (size_t)n) == 0,
            "S64: streamed and resident load the identical list bytes");
    }
  }

  /* Out-of-range gbs_read_field, both kinds, on BOTH sessions. */
  uint8_t junk1[4];
  CHECK(gbs_read_field(&sr, len, junk1, 1) == GBS_ERR_ARG,
        "S64: resident refuses off == len");
  CHECK(gbs_read_field(&ss, len, junk1, 1) == GBS_ERR_ARG,
        "S64: streamed refuses off == len");
  CHECK(gbs_read_field(&sr, 0, junk1, len + 1) == GBS_ERR_ARG,
        "S64: resident refuses n == len+1");
  CHECK(gbs_read_field(&ss, 0, junk1, len + 1) == GBS_ERR_ARG,
        "S64: streamed refuses n == len+1");

  /* ---- mutation half: every write entry point refuses, and the file is untouched */
  long fsz_before = 0;
  { fseek(f, 0, SEEK_END); fsz_before = ftell(f); }
  uint8_t before_bytes[64];
  CHECK(gbs_read_field(&ss, 0, before_bytes, sizeof before_bytes) == GBS_OK,
        "S64: read the header before the mutation attempts");

  CHECK(gbs_box_writable(&ss, 0) == GBS_ERR_UNWRITABLE,
        "S64: gbs_box_writable refuses on a streamed session");
  CHECK(gbs_commit_list(&ss, 0, g_list) == GBS_ERR_UNWRITABLE,
        "S64: gbs_commit_list refuses on a streamed session");
  CHECK(gbs_delete(&ss, 0, 0, g_list) == GBS_ERR_UNWRITABLE,
        "S64: gbs_delete refuses on a streamed session");
  int to_slot = -1;
  CHECK(gbs_move(&ss, 0, 0, 1, &to_slot, g_list, g_list2) == GBS_ERR_UNWRITABLE,
        "S64: gbs_move refuses on a streamed session");
  CHECK(gbs_finish(&ss) == GBS_ERR_UNWRITABLE,
        "S64: gbs_finish refuses on a streamed session");
  uint8_t w3[3] = { 1, 2, 3 };
  CHECK(gbs_write_field(&ss, money_off, w3, 3) == GBS_ERR_UNWRITABLE,
        "S64: gbs_write_field refuses on a streamed session");
  CHECK(gbs_write_outside_sum(&ss, 0, w3, 1) == GBS_ERR_UNWRITABLE,
        "S64: gbs_write_outside_sum refuses on a streamed session");
  {
    GbEditMon dummy; memset(&dummy, 0, sizeof dummy);
    dummy.gen = expect_gen; dummy.is_party = false;
    int slot_out = -1;
    CHECK(gbs_insert(&ss, 0, &dummy, &slot_out, g_list) == GBS_ERR_UNWRITABLE,
          "S64: gbs_insert refuses on a streamed session");
  }

  uint8_t after_bytes[64];
  CHECK(gbs_read_field(&ss, 0, after_bytes, sizeof after_bytes) == GBS_OK,
        "S64: read the header again after the mutation attempts");
  CHECK(memcmp(before_bytes, after_bytes, sizeof before_bytes) == 0,
        "S64: the header is byte-identical after every refused write");

  long fsz_after = 0;
  { fseek(f, 0, SEEK_END); fsz_after = ftell(f); }
  CHECK(fsz_before == fsz_after, "S64: the backing file's SIZE is unchanged");

  /* And re-read the WHOLE file straight off disk (bypassing both sessions' own
   * caches entirely) to prove the mutation attempts never reached the card. */
  fseek(f, 0, SEEK_SET);
  uint8_t* disk = g_img2;   /* reuse s3_gen2_party_box's own third image buffer -- g_img/g_orig
                             * are this test's own resident copy and must stay untouched */
  uint32_t got = (uint32_t)fread(disk, 1, len, f);
  CHECK(got == len, "S64: re-read the whole file from disk");
  CHECK(memcmp(disk, g_orig, len) == 0,
        "S64: THE BACKING FILE ON DISK IS BYTE-IDENTICAL AFTER EVERY REFUSED WRITE");

  fclose(f);
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

/* ============================================================================
 * gbs_read_field / gbs_write_field / gbs_finish (BACKLOG #49 P0)
 * ========================================================================== */

/* The lifted API's whole point: the SAME three calls work across both generations. Money
 * (docs/GEN12-PARITY-DESIGN.md §1.1, VERIFIED(4)): Red/Blue/Yellow 0x25F3 (3 B BE BCD),
 * Gold/Silver 0x23DB (3 B BE binary) — round-tripped here as raw bytes (this tree has no
 * decoded money reader yet; P1's trainer card is the next slice), which is exactly the
 * primitive's own promise: the bytes it was handed, back out unchanged. */
static void test_field_write(const char* file, uint8_t expect_gen, uint32_t money_off) {
  uint32_t len = load(file);
  if (!len) { printf("  -- SKIP %s (field write)\n", file); return; }
  g_ran++;
  printf("  -- field write: %s\n", file);

  GbSession s;
  GbsStatus os = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch);
  CHECK(os == GBS_OK, "field write: session opens");
  if (os != GBS_OK) return;
  CHECK(s.gen == expect_gen, "field write: right generation detected");

  const uint8_t want[3] = { 0x12, 0x34, 0x56 };
  uint8_t before[3], back[3];
  CHECK(gbs_read_field(&s, money_off, before, 3) == GBS_OK, "field write: read money before");

  CHECK(gbs_write_field(&s, money_off, want, 3) == GBS_OK, "field write: money write accepted");
  CHECK(gbs_finish(&s) == GBS_OK, "field write: gbs_finish accepts it");

  CHECK(gbs_read_field(&s, money_off, back, 3) == GBS_OK, "field write: read money after");
  CHECK(memcmp(back, want, 3) == 0, "field write: money reads back exactly as written");

  /* Re-open the mutated image FROM SCRATCH — a fresh parse, not the session's cached
   * view — proving the edit reached the bytes the GAME will read, not just the ones the
   * session happened to remember, and that BOTH stored checksums (Gen 2) / the main
   * checksum (Gen 1) still validate. */
  {
    GbSession s2;
    CHECK(gbs_open(&s2, g_img, len, g_scratch2, sizeof g_scratch2) == GBS_OK,
          "field write: the edited image still parses (checksums valid)");
    CHECK(s2.gen == expect_gen, "field write: still the same generation after the edit");
    uint8_t fresh[3];
    CHECK(gbs_read_field(&s2, money_off, fresh, 3) == GBS_OK, "field write: re-read after reopen");
    CHECK(memcmp(fresh, want, 3) == 0, "field write: …and it survived a full re-open");
  }

  /* A no-op — writing back the value already there — must change zero bytes, exactly
   * gbs_commit_list's own "open a box and back out" guarantee, extended to a field.
   * Reuses this file's own g_snap global (already sized for a whole image, see its
   * declaration above) rather than a new stack array of the same size. */
  memcpy(g_snap, g_img, len);
  CHECK(gbs_write_field(&s, money_off, want, 3) == GBS_OK,
        "field write: writing the SAME value again is accepted");
  CHECK(gbs_finish(&s) == GBS_OK, "field write: gbs_finish on the no-op still accepts");
  CHECK(memcmp(g_img, g_snap, len) == 0,
        "field write: a no-op field write changes ZERO bytes, checksums included");

  /* And the refusals carry through the lifted API too — a Pokemon-shaped range is not a
   * field, on either generation. */
  {
    GbsStatus st;
    uint8_t junk[8] = { 0 };
    if (s.gen == GB_GEN1) {
      st = gbs_write_field(&s, GEN1_OFF_PARTY, junk, 4);
    } else {
      G2Offsets go; g2_offsets(s.g2w.sv.version, &go);
      st = gbs_write_field(&s, go.party_list, junk, 4);
    }
    CHECK(st != GBS_OK, "field write: the party list is not reachable through gbs_write_field");
  }

  /* P0 review D6: gbs_write_field must accept a field wider than gen1_write_range's
   * 64-byte convenience cap -- source/gb_fields.c's own GBF_EVENT_FLAGS_BASE is 320 B,
   * a real field this design ships. gbs_write_field routes Gen 1 through
   * gen1_write_range_ex with the session's OWN scratch as the rollback buffer (not a
   * new allocation -- see gb_session.h's own comment on this call), which is exactly
   * what makes 320 B possible through the lifted API and not just the raw engine. */
  if (s.gen == GB_GEN1) {
    uint8_t payload[200], readback[200];
    for (int i = 0; i < 200; i++) payload[i] = (uint8_t)(0x30 + (i % 40));
    /* GEN1_OFF_PLAYER_NAME+100 .. +300: inside the header span, clear of the party
     * blob / open box / current-box byte -- same offset host_gen1write_test.c's own
     * D6 proof uses. */
    uint32_t wide_off = GEN1_OFF_PLAYER_NAME + 100u;
    CHECK(gbs_write_field(&s, wide_off, payload, sizeof payload) == GBS_OK,
         "field write: a 200-byte Gen-1 field write is accepted (past the 64-B "
         "convenience wrapper's own cap)");
    CHECK(gbs_finish(&s) == GBS_OK, "field write: gbs_finish accepts the wide write");
    CHECK(gbs_read_field(&s, wide_off, readback, sizeof readback) == GBS_OK,
         "field write: the 200 bytes read back");
    CHECK(memcmp(readback, payload, sizeof payload) == 0,
         "field write: …exactly as written");
  }
}

int main(void) {
  printf("gb_session (resident-image edit pipeline)\n");

  rejects_garbage();
  s3_mail_predicate_boundaries();

  one_save("Red.sav",     GB_GEN1);
  one_save("Yellow.sav",  GB_GEN1);
  one_save("Gold.sav",    GB_GEN2);   /* 32816 bytes — the 48-byte RTC tail case */
  one_save("Crystal.sav", GB_GEN2);

  /* gbs_read_field / gbs_write_field / gbs_finish (BACKLOG #49 P0): the SAME three calls,
   * on Red.sav (Gen 1) AND Gold.sav (Gen 2), per docs/GEN12-PARITY-DESIGN.md §1.1's money
   * offsets (Red/Blue/Yellow 0x25F3, Gold/Silver 0x23DB). */
  test_field_write("Red.sav",  GB_GEN1, 0x25F3u);
  test_field_write("Gold.sav", GB_GEN2, 0x23DBu);

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

  s5_insert("Red.sav",     GB_GEN1);
  s5_insert("Yellow.sav",  GB_GEN1);
  s5_insert("Gold.sav",    GB_GEN2);
  s5_insert("Crystal.sav", GB_GEN2);

  s8_roundtrip("Gold.sav",    GB_GEN2);
  s8_roundtrip("Crystal.sav", GB_GEN2);

  /* BACKLOG #64: the read-only STREAMED session, over every corpus save. */
  s64_streamed_vs_resident("Red.sav",     GB_GEN1);
  s64_streamed_vs_resident("Yellow.sav",  GB_GEN1);
  s64_streamed_vs_resident("Gold.sav",    GB_GEN2);
  s64_streamed_vs_resident("Crystal.sav", GB_GEN2);

  if (!g_ran) printf("  (no corpus present — structural checks only)\n");
  printf("%s: %d/%d checks passed over %d save(s)\n",
         g_fail ? "FAIL" : "ok", g_check - g_fail, g_check, g_ran);
  return g_fail ? 1 : 0;
}
