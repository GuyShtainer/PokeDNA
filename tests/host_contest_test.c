/* Host test for gen3_contest.{c,h} + the ribbon-rank wrapper in gen3_edit.c --
 * BACKLOG #60 (Contests: museum paintings + per-mon ribbon rank), against Guy's own
 * Emerald/Ruby/Sapphire saves.
 *
 * What this pins down:
 *   1) the SB1 hall/museum offsets (0x2E90/0x2F90 Emerald, 0x2DFC/0x2EFC RS) read real
 *      data (species/category/rank all in range) on all three games;
 *   2) a museum write (gc_museum_set) round-trips through the FULL verified-write path
 *      (gen3_write_full_section over sections 1..4, same as app_commit_sb1) and the
 *      section checksums stay valid;
 *   3) that write touches ONLY the target record's 32 bytes of SaveBlock1 -- listed
 *      explicitly, not just asserted as "some change happened";
 *   4) a byte-identical re-write (same donor twice) is a true no-op at the byte level;
 *   5) the museum category byte is a CAPTION ID (3*category + variant), not the raw
 *      category -- gc_museum_get must recover the plain 0..4 category via /3, verified
 *      against a real save's museum win where one exists (Ruby's Tough slot);
 *   6) the ribbon rank wrapper (gen3_edit.c's em_set_ribbon_rank/em_get_ribbon_rank)
 *      round-trips 0..4 through EditMon's Misc substruct and gen3_edit_commit without
 *      disturbing any other byte of the 80/100-byte record.
 *
 * Build + run (repo root):
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_contest_test.c \
 *     source/gen3_contest.c source/gen3_flags.c source/gen3_save.c source/gen3_edit.c source/gen3_mon.c \
 *     source/data_tables.c source/gen3_daycare.c \
 *     -o /tmp/hcontest && /tmp/hcontest \
 *     [emerald.sav] [ruby.sav] [sapphire.sav]
 *
 * gen3_edit.c also references learnsets2.h/evolutions.h symbols (movesets/evo floor,
 * used by gen3_build_mon and friends, which this test never calls) -- those headers
 * carry __attribute__((weak)) fallbacks for exactly this case (their generated .c
 * files are git-ignored and not needed here), so they are deliberately NOT linked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gen3_trainer.h"
#include "gen3_contest.h"
#include "gen3_flags.h"  /* pk_flag_get for #408 tests */
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_edit.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static uint8_t g_save[262144];
static uint8_t g_sb1[G3_SAVEBLOCK1_BYTES];

/* One real party mon (slot 0) to use as a museum donor -- read once, reused as-is. */
static bool first_party_mon(const uint8_t* sb1, PkMon* out) {
  if (sb1[SB1_OFF_PARTY_COUNT] == 0) return false;
  return pk_decode_mon(sb1 + SB1_OFF_PARTY, true, out) && out->species != 0;
}

static void one(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("skip %s (not found)\n", path); return; }
  size_t sz = fread(g_save, 1, sizeof g_save, f); fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(g_save, (uint32_t)sz, &info) || !gen3_read_saveblock1(g_save, info.slot, g_sb1)) {
    printf("skip %s (unreadable)\n", path); return; }

  /* This test's caller (tests/run_host_tests.py) passes the WHOLE 5-game corpus as
   * argv, not just RS/Emerald saves in a fixed order -- so the game is DETECTED per
   * file, the same way pdna_main.c's own load path does (pk_read_party_auto, which
   * tries both the RSE and FRLG party offsets), not assumed from argv position. */
  PkMon party_scratch[6]; bool is_frlg = false;
  pk_read_party_auto(g_sb1, party_scratch, &is_frlg);
  if (is_frlg) { printf("== %s: FireRed/LeafGreen -- no Contests, skipping ==\n", path); return; }
  PkGame game = (info.version_guess == G3_VER_RS) ? PK_RS : PK_EMERALD;
  const char* label = (game == PK_RS) ? "Ruby/Sapphire" : "Emerald";
  printf("== %s (%s, slot %d) ==\n", path, label, info.slot);

  /* ---- (1) hall + museum reads: every field in range ---- */
  int nh = gc_hall_count(game);
  CHECK(nh == (game == PK_EMERALD ? 6 : 8), "gc_hall_count matches the game");
  for (int i = 0; i < nh; i++) {
    GcWinner w;
    CHECK(gc_hall_get(g_sb1, game, i, &w), "gc_hall_get succeeds in range");
    CHECK(w.category < GC_CATEGORY_COUNT, "hall category is plain 0..4 (not a caption id)");
    CHECK(w.rank <= 4, "hall rank is 0..4");
  }

  /* ---- (1b) D2 regression: the hall/museum rank scale is CONTEST_RANK_*, not
   * GC_RANK_* -- and on RS a hall slot's rank comes from ITS OWN SLOT INDEX
   * (RS_SLOT_RANK), not a stored field. Pinned against Guy's own saves' real bytes,
   * not synthetic ones, so a regression that puts the ribbon scale (with its extra
   * "None") back in place, or drops the RS slot-index mapping, fails here. */
  if (strstr(path, "Emerald.sav")) {
    static const uint8_t want[6] = { 0, 2, 0, 3, 1, 2 };  /* Normal/Hyper/Normal/Master/Super/Hyper */
    for (int i = 0; i < 6; i++) {
      GcWinner w; gc_hall_get(g_sb1, game, i, &w);
      CHECK(w.rank == want[i], "Emerald hall rank matches the raw contestRank byte");
      if (i == 3) printf("  (D2) Emerald hall slot 3 = rank %u (want 3, Master)\n", (unsigned)w.rank);
    }
  }
  if (strstr(path, "Ruby.sav")) {
    GcWinner w0, w5;
    gc_hall_get(g_sb1, game, 0, &w0);
    gc_hall_get(g_sb1, game, 5, &w5);
    CHECK(w0.rank == (uint8_t)CONTEST_RANK_NORMAL, "Ruby hall slot 0 -> rank 0 (Normal, RS_SLOT_RANK)");
    CHECK(w5.rank == (uint8_t)CONTEST_RANK_MASTER, "Ruby hall slot 5 -> rank 3 (Master, RS_SLOT_RANK)");
  }

  bool found_museum_win = false;
  for (int c = 0; c < GC_MUSEUM_COUNT; c++) {
    GcWinner w;
    CHECK(gc_museum_get(g_sb1, game, c, &w), "gc_museum_get succeeds in range");
    /* An EMPTY slot's category byte carries no real meaning (new_game.c zeroes the
     * whole record and the game never stamps a category on a slot nobody won) -- the
     * recovery check only means something once a real win set the caption id. */
    if (w.species) {
      CHECK((int)w.category == c, "museum category recovers the SLOT's own category (caption id / 3)");
      found_museum_win = true;
      printf("  museum[%d] %s owns a real painting: %s / %s (rank %u)\n",
             c, label, w.monName, w.trainerName, (unsigned)w.rank);
    }
  }
  if (found_museum_win) printf("  (5): caption-id division verified against a REAL win, not just a synthetic one\n");

  /* ---- (2)+(3) a real write through the full verified-write path ---- */
  PkMon donor;
  if (!first_party_mon(g_sb1, &donor)) { printf("  skip write test: empty party\n"); return; }

  int cat = GC_TOUGH;   /* slot 4 -- distinct from the categories checked above */
  uint32_t off = gc_museum_offset(game, cat);
  CHECK(off != 0, "gc_museum_offset resolves for a supported game");

  uint8_t sb1_before[G3_SAVEBLOCK1_BYTES];
  memcpy(sb1_before, g_sb1, sizeof sb1_before);

  CHECK(gc_museum_set(g_sb1, game, cat, donor.species, donor.personality, donor.otId,
                      donor.nickname, donor.otName), "gc_museum_set succeeds");

  CHECK(pk_flag_get(g_sb1, game, 0xA0 + cat), "#408: the category's PAINTING_MADE flag is set with the record");

  /* Exact changed-bytes list, SaveBlock1-relative: the 32-byte record plus EXACTLY the one
   * flags[] byte that carries FLAG_<cat>_PAINTING_MADE (#408) -- nothing else. */
  bool flag_was_set = pk_flag_get(sb1_before, game, 0xA0 + cat);
  int record_changed = 0, outside_changed = 0;
  uint32_t outside_off = 0;
  for (uint32_t i = 0; i < sizeof sb1_before; i++) {
    if (sb1_before[i] == g_sb1[i]) continue;
    if (i >= off && i < off + GC_RECORD_BYTES) { record_changed++; continue; }
    outside_changed++;
    outside_off = i;
  }
  printf("  write: %d record byte(s) changed (0x%04X..0x%04X), %d byte(s) outside (flag byte 0x%04X, flag was %s)\n",
         record_changed, off, off + GC_RECORD_BYTES - 1, outside_changed, outside_off, flag_was_set ? "set" : "clear");
  CHECK(record_changed > 0, "the record actually changed");
  CHECK(outside_changed == (flag_was_set ? 0 : 1),
        "#408: outside the record exactly the PAINTING_MADE flag byte changed (none if the flag was already set)");
  if (outside_changed == 1)
    CHECK((uint8_t)(sb1_before[outside_off] ^ g_sb1[outside_off]) == (uint8_t)(1u << ((0xA0 + cat) % 8)),
          "#408: that byte differs by exactly the category's flag bit");

  /* ---- full verified-write round trip: sections 1..4, then re-parse + checksum ---- */
  uint8_t save_copy[262144];
  memcpy(save_copy, g_save, sz);
  for (int id = 1; id <= 4; id++)
    gen3_write_full_section(save_copy, info.slot, id, g_sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE);
  int fail_id = -1;
  CHECK(gen3_verify_full_checksums(save_copy, info.slot, &fail_id),
       "every SB1 section checksum is valid after the write");
  if (fail_id >= 0) printf("  checksum FAILED on section %d\n", fail_id);

  Gen3SaveInfo info2;
  uint8_t sb1_reread[G3_SAVEBLOCK1_BYTES];
  CHECK(gen3_parse(save_copy, (uint32_t)sz, &info2) &&
       gen3_read_saveblock1(save_copy, info2.slot, sb1_reread),
       "the written file re-parses");
  CHECK(memcmp(sb1_reread, g_sb1, sizeof sb1_reread) == 0,
       "re-reading the committed file recovers exactly what was written");
  GcWinner check;
  gc_museum_get(sb1_reread, game, cat, &check);
  CHECK(check.species == donor.species, "the re-read painting shows the donor species");
  /* CONTEST_RANK_MASTER is 3, not GC_RANK_MASTER's 4 -- the ribbon word's scale has an
   * extra "None" at 0 that the save-file rank byte does not (D2). On Emerald this is
   * the literal contestRank byte gc_museum_set just wrote; on RS there is no field at
   * all, so gc_museum_get always reports Master for a museum slot. */
  CHECK(check.rank == (uint8_t)CONTEST_RANK_MASTER, "the re-read painting is Master rank (3)");

  /* ---- (4) a byte-identical re-write is a true no-op ---- */
  uint8_t sb1_snap[G3_SAVEBLOCK1_BYTES];
  memcpy(sb1_snap, g_sb1, sizeof sb1_snap);
  gc_museum_set(g_sb1, game, cat, donor.species, donor.personality, donor.otId,
               donor.nickname, donor.otName);
  CHECK(memcmp(sb1_snap, g_sb1, sizeof sb1_snap) == 0,
       "writing the SAME donor twice is byte-identical (a real no-op)");

  /* ---- (6) ribbon rank round-trips through EditMon, nothing else in the record moves --- */
  uint8_t rec_before[100], rec_after[100];
  memcpy(rec_before, g_sb1 + SB1_OFF_PARTY, 100);
  EditMon e;
  gen3_edit_load(rec_before, true, &e);
  CHECK(em_get_ribbon_rank(&e, GC_COOL) == gc_ribbon_get(donor.ribbons, GC_COOL),
       "em_get_ribbon_rank agrees with the decoded PkMon.ribbons");
  for (uint8_t rank = 0; rank <= 4; rank++) {
    em_set_ribbon_rank(&e, GC_COOL, rank);
    CHECK(em_get_ribbon_rank(&e, GC_COOL) == rank, "em_set/get_ribbon_rank round-trips 0..4");
  }
  em_set_ribbon_rank(&e, GC_COOL, (uint8_t)gc_ribbon_get(donor.ribbons, GC_COOL));  /* restore */
  gen3_edit_commit(&e, rec_after);
  CHECK(memcmp(rec_before, rec_after, 100) == 0,
       "restoring the original rank re-encodes byte-identical (no-op through commit)");
  /* Pick a target rank that DIFFERS from the donor's current Cool rank, so the write
   * is guaranteed to actually change something (a donor that already has Master would
   * make a "set to Master" no-op legitimately, and that would not exercise this check). */
  uint8_t cur = gc_ribbon_get(donor.ribbons, GC_COOL);
  uint8_t target = (cur == GC_RANK_MASTER) ? GC_RANK_NORMAL : GC_RANK_MASTER;
  em_set_ribbon_rank(&e, GC_COOL, target);
  gen3_edit_commit(&e, rec_after);
  int rec_changed = 0;
  for (int i = 0; i < 100; i++) if (rec_before[i] != rec_after[i]) rec_changed++;
  printf("  ribbon rank %u->%u write: %d/100 record bytes changed\n", cur, target, rec_changed);
  CHECK(rec_changed > 0 && rec_changed <= 4,
       "a ribbon-rank edit touches only the ribbons word (<=4 bytes), nothing else");
}

int main(int argc, char** argv) {
  if (argc > 1) {
    for (int i = 1; i < argc; i++) one(argv[i]);   /* the whole corpus, any order/count */
  } else {
    one("/tmp/pokedna-b60-sav/Emerald.sav");
    one("/tmp/pokedna-b60-sav/Ruby.sav");
    one("/tmp/pokedna-b60-sav/Sapphire.sav");
  }

  /* FRLG: unsupported everywhere, no offsets to disturb. */
  CHECK(!gc_supported(PK_FRLG), "gc_supported: FRLG is false");
  CHECK(gc_hall_count(PK_FRLG) == 0, "gc_hall_count: FRLG is 0");
  GcWinner w;
  CHECK(!gc_museum_get(0, PK_FRLG, GC_COOL, &w), "gc_museum_get refuses FRLG");
  CHECK(gc_museum_offset(PK_FRLG, GC_COOL) == 0, "gc_museum_offset: FRLG is 0");

  /* Ribbon bit-layout unit checks (no save needed): cumulative-by-construction. */
  uint32_t r = 0;
  for (int cat = 0; cat < GC_CATEGORY_COUNT; cat++)
    CHECK(gc_ribbon_get(r, cat) == 0, "fresh ribbons word: every category is 0 (None)");
  r = gc_ribbon_set(r, GC_TOUGH, GC_RANK_MASTER);
  CHECK(gc_ribbon_get(r, GC_TOUGH) == GC_RANK_MASTER, "gc_ribbon_set/get round-trips Master");
  CHECK(gc_ribbon_get(r, GC_COOL) == 0, "setting Tough does not touch Cool (separate 3-bit fields)");
  r = gc_ribbon_set(r, GC_TOUGH, 9);   /* out of range -- must clamp, not corrupt neighbours */
  CHECK(gc_ribbon_get(r, GC_TOUGH) == GC_RANK_MASTER, "gc_ribbon_set clamps an out-of-range rank to Master");
  CHECK(!gc_ribbon_flag_get(r, GC_RFLAG_ARTIST), "Artist ribbon starts off");
  r = gc_ribbon_flag_set(r, GC_RFLAG_ARTIST, true);
  CHECK(gc_ribbon_flag_get(r, GC_RFLAG_ARTIST), "Artist ribbon flag sets");
  CHECK(gc_ribbon_get(r, GC_TOUGH) == GC_RANK_MASTER, "setting a named flag does not disturb the rank fields");
  r = gc_ribbon_flag_set(r, GC_RFLAG_ARTIST, false);
  CHECK(!gc_ribbon_flag_get(r, GC_RFLAG_ARTIST), "Artist ribbon flag clears");

  printf("\n%s (%d failure%s)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED",
        fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
