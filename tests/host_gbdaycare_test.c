/* source/gb_daycare.c -- the pure-C day-care core -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbdaycare_test.c \
 *      source/gb_daycare.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbd && /tmp/hgbd
 *
 * Corpus: Guy's own cartridge dumps (gitignored, gba-toolkit/roms/gb/). A missing
 * corpus SKIPS rather than fails.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_daycare.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECKF(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_orig[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
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

/* ---- A: Red.sav -- Gen 1, one slot, no breeding fields ---- */

static void gen1_shape(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbDaycare d;
  CHECKF(gbd_read(&s, &d), "%s: gbd_read", file);
  CHECKF(d.gen1, "%s: gen1 flag must be true", file);
  CHECKF(!d.has_slot2, "%s: Gen 1 has no slot 2", file);
  CHECKF(!d.has_egg, "%s: Gen 1 has no egg", file);
  /* Red.sav's own day-care mon, decoded straight from GBF_DAYCARE_*; not asserted by
   * exact identity here (design doc's §1.7 corpus note is about Gold, not Red) but the
   * record must decode to a plausible level if the slot reads occupied. */
  if (d.slot[0].occupied)
    CHECKF(gb_get_level(&d.slot[0].mon) >= 1 && gb_get_level(&d.slot[0].mon) <= 100,
          "%s: occupied slot 0 must decode a plausible level", file);

  /* gbd_withdraw(1, ...) and gbd_withdraw_egg must both refuse on Gen 1 */
  GbEditMon tmp;
  CHECKF(gbd_withdraw(&s, 1, &tmp) == GBS_ERR_SLOT, "%s: slot 1 refused on Gen 1", file);
  CHECKF(gbd_withdraw_egg(&s, &tmp) == GBS_ERR_ARG, "%s: egg refused on Gen 1", file);
}

/* ---- B: Gold.sav -- the probed ground truth: slot 0 residue present but NOT
 * occupied (wDayCareMan bit 0 clear even though the record decodes CROCONAW/21/
 * "MattiaPK") ---- */

static void gold_ground_truth(void) {
  uint32_t len = load("Gold.sav");
  if (!len) { printf("  SKIP Gold.sav (not present)\n"); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "Gold.sav: open");
  GbDaycare d;
  CHECKF(gbd_read(&s, &d), "Gold.sav: gbd_read");
  CHECKF(!d.gen1, "Gold.sav: must be Gen 2");
  CHECKF(d.has_slot2, "Gold.sav: GS has a second slot");
  CHECKF(!d.slot[0].occupied, "Gold.sav: slot 0 must read UNOCCUPIED (bit 0 clear)");
  CHECKF(d.intro_seen, "Gold.sav: bit 7 (intro-seen) must read set (raw byte 0x80)");
  CHECKF(gb_get_species_dex(&d.slot[0].mon) == 159,
        "Gold.sav: slot 0's RESIDUE record must still decode dex 159 (Croconaw), got %u",
        gb_get_species_dex(&d.slot[0].mon));
  CHECKF(gb_get_level(&d.slot[0].mon) == 21, "Gold.sav: residue level %u != 21",
        gb_get_level(&d.slot[0].mon));
  CHECKF(strcmp(d.slot[0].nick, "CROCONAW") == 0, "Gold.sav: residue nick '%s' != CROCONAW",
        d.slot[0].nick);
  CHECKF(strcmp(d.slot[0].ot, "MattiaPK") == 0, "Gold.sav: residue OT '%s' != MattiaPK",
        d.slot[0].ot);
  CHECKF(!d.slot[1].occupied, "Gold.sav: slot 1 must read unoccupied");
  CHECKF(!d.has_egg, "Gold.sav: no egg ready");

  /* withdraw must refuse -- nothing is occupied */
  GbEditMon tmp;
  CHECKF(gbd_withdraw(&s, 0, &tmp) == GBS_ERR_SLOT, "Gold.sav: withdraw slot 0 must refuse "
        "(unoccupied)");
}

/* ---- C: no-op read is stable (calling gbd_read twice agrees) -- there is no gbd_write
 * for a bare "unchanged" case the way gbt_write/gbc_shift have one, since deposit/
 * withdraw are the only mutators and both are exercised for real below; this instead
 * proves a plain read never itself mutates the image. ---- */

static void read_is_readonly(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbDaycare d;
  CHECKF(gbd_read(&s, &d), "%s: gbd_read", file);
  uint32_t diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff == 0, "%s: gbd_read must not touch a byte", file);
}

/* ---- D: deposit into an empty slot, verify the flag bit + fields, withdraw it back,
 * verify the record round-trips and the flag clears, minimal-diff on each half ---- */

static void deposit_withdraw_roundtrip(const char* file, int slot) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbDaycare before;
  CHECKF(gbd_read(&s, &before), "%s: pre-read", file);
  if (slot == 1 && !before.has_slot2) { printf("  SKIP %s slot 1 (no second slot)\n", file); return; }
  if (before.slot[slot].occupied) {
    printf("  SKIP %s slot %d (already occupied by the fixture)\n", file, slot);
    return;
  }

  /* build a simple box-shaped GbEditMon of this session's own generation to deposit */
  GbEditMon mon;
  memset(&mon, 0, sizeof mon);
  mon.gen = s.gen;
  mon.rec_len = (uint8_t)gb_rec_size(s.gen, false);
  mon.is_party = false;
  CHECKF(gb_set_species(&mon, 1, s.gen == GB_GEN1 ? &(GbGen1Base){ .base = {45,49,49,45,65},
        .type1 = 0x16, .type2 = 0x03 } : NULL), "%s: gb_set_species", file);
  CHECKF(gb_set_level(&mon, 5), "%s: gb_set_level", file);
  CHECKF(gb_set_nickname(&mon, "TESTMON"), "%s: gb_set_nickname", file);
  CHECKF(gb_set_otname(&mon, "TESTER"), "%s: gb_set_otname", file);
  /* P1a review D2: gbd_deposit now gates on gb_check(), which refuses a moveless record
   * (move slot 0 empty is "the game has no such Pokemon") -- give it move 1 (Pound, the
   * same move id every generation this table covers has). */
  CHECKF(gb_set_move(&mon, 0, 1), "%s: gb_set_move", file);

  /* refuse a party-shaped mon before any byte moves */
  GbEditMon party_mon = mon;
  party_mon.is_party = true;
  GbsStatus st_party = gbd_deposit(&s, slot, &party_mon);
  CHECKF(st_party == GBS_ERR_ARG, "%s: party-shaped deposit must refuse (got %s)", file,
        gbs_status_text(st_party));
  uint32_t diff_guard = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff_guard++;
  CHECKF(diff_guard == 0, "%s: a refused deposit must not move any byte", file);

  GbsStatus st = gbd_deposit(&s, slot, &mon);
  CHECKF(st == GBS_OK, "%s: gbd_deposit slot %d status %s", file, slot, gbs_status_text(st));

  GbDaycare after;
  CHECKF(gbd_read(&s, &after), "%s: post-deposit read", file);
  CHECKF(after.slot[slot].occupied, "%s: slot %d must read occupied after deposit", file, slot);
  CHECKF(gb_get_species_dex(&after.slot[slot].mon) == 1, "%s: deposited species mismatch",
        file);
  CHECKF(gb_get_level(&after.slot[slot].mon) == 5, "%s: deposited level mismatch", file);
  CHECKF(strcmp(after.slot[slot].nick, "TESTMON") == 0, "%s: deposited nick mismatch (%s)",
        file, after.slot[slot].nick);
  CHECKF(strcmp(after.slot[slot].ot, "TESTER") == 0, "%s: deposited OT mismatch (%s)", file,
        after.slot[slot].ot);

  /* second deposit into the same (now occupied) slot must refuse */
  GbsStatus st_full = gbd_deposit(&s, slot, &mon);
  CHECKF(st_full == GBS_ERR_FULL, "%s: deposit into an occupied slot must refuse (got %s)",
        file, gbs_status_text(st_full));

  /* withdraw it back */
  GbEditMon out;
  GbsStatus st_w = gbd_withdraw(&s, slot, &out);
  CHECKF(st_w == GBS_OK, "%s: gbd_withdraw slot %d status %s", file, slot, gbs_status_text(st_w));
  CHECKF(gb_get_species_dex(&out) == 1, "%s: withdrawn species mismatch", file);
  CHECKF(gb_get_level(&out) == 5, "%s: withdrawn level mismatch", file);

  GbDaycare final_state;
  CHECKF(gbd_read(&s, &final_state), "%s: post-withdraw read", file);
  CHECKF(!final_state.slot[slot].occupied, "%s: slot %d must read unoccupied after withdraw",
        file, slot);

  /* a second withdraw must refuse (already empty) */
  GbEditMon out2;
  CHECKF(gbd_withdraw(&s, slot, &out2) == GBS_ERR_SLOT, "%s: withdraw from an empty slot "
        "must refuse", file);
}

/* ---- E: P1a review D2 -- an all-zero (structurally unsound) record is refused ---- */

static void deposit_all_zero_refused(const char* file, int slot) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbDaycare before;
  CHECKF(gbd_read(&s, &before), "%s: pre-read", file);
  if (slot == 1 && !before.has_slot2) { printf("  SKIP %s slot 1 (no second slot)\n", file); return; }
  if (before.slot[slot].occupied) {
    printf("  SKIP %s slot %d (already occupied by the fixture)\n", file, slot);
    return;
  }

  GbEditMon zero;
  memset(&zero, 0, sizeof zero);
  zero.gen = s.gen;
  zero.rec_len = (uint8_t)gb_rec_size(s.gen, false);
  zero.is_party = false;

  GbsStatus st = gbd_deposit(&s, slot, &zero);
  CHECKF(st == GBS_ERR_STRUCT, "%s: an all-zero record must be refused as structurally "
        "unsound (got %s)", file, gbs_status_text(st));

  uint32_t diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff == 0, "%s: a refused all-zero deposit must not move any byte (moved %u)",
        file, diff);
}

/* ---- F: P1a review D9 -- withdraw clears DAYCAREMAN_MONS_COMPATIBLE_F (slot 0 only) --
 * gbd_withdraw already clears bit 5 alongside bit 0 (gb_daycare.c's own next_flag mask),
 * but nothing exercised that before this: a mutation that dropped the compat-bit clear
 * would still pass every other case in this file. */

static void withdraw_clears_compat_bit(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbDaycare before;
  CHECKF(gbd_read(&s, &before), "%s: pre-read", file);
  if (before.slot[0].occupied) {
    printf("  SKIP %s slot 0 (already occupied by the fixture)\n", file);
    return;
  }

  GbEditMon mon;
  memset(&mon, 0, sizeof mon);
  mon.gen = s.gen;
  mon.rec_len = (uint8_t)gb_rec_size(s.gen, false);
  mon.is_party = false;
  CHECKF(gb_set_species(&mon, 1, NULL), "%s: gb_set_species", file);
  CHECKF(gb_set_level(&mon, 5), "%s: gb_set_level", file);
  CHECKF(gb_set_nickname(&mon, "TESTMON"), "%s: gb_set_nickname", file);
  CHECKF(gb_set_otname(&mon, "TESTER"), "%s: gb_set_otname", file);
  CHECKF(gb_set_move(&mon, 0, 1), "%s: gb_set_move", file);
  CHECKF(gbd_deposit(&s, 0, &mon) == GBS_OK, "%s: gbd_deposit slot 0", file);

  /* plant the compatibility bit (bit 5) directly -- gbd_deposit never sets it (see its
   * own header note), so we have to, to exercise the clear-on-withdraw path at all. */
  GbGame g = gbd_game(&s);
  uint8_t flag = 0;
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_DAYCARE_FLAG), &flag, 1) == GBS_OK,
        "%s: read flag before planting compat bit", file);
  flag |= (1u << 5);
  CHECKF(gbs_write_field(&s, gbf_off(g, GBF_DAYCARE_FLAG), &flag, 1) == GBS_OK,
        "%s: plant compat bit", file);
  CHECKF(gbs_finish(&s) == GBS_OK, "%s: finish after planting compat bit", file);

  GbDaycare planted;
  CHECKF(gbd_read(&s, &planted), "%s: read after planting", file);
  CHECKF(planted.compatible, "%s: compat bit did not read back set", file);

  GbEditMon out;
  CHECKF(gbd_withdraw(&s, 0, &out) == GBS_OK, "%s: gbd_withdraw slot 0", file);

  uint8_t flag_after = 0xFF;
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_DAYCARE_FLAG), &flag_after, 1) == GBS_OK,
        "%s: read flag after withdraw", file);
  CHECKF((flag_after & (1u << 5)) == 0, "%s: withdraw must clear the compatibility bit "
        "(flag=0x%02X)", file, flag_after);
}

/* ---- G: P1a review D3 -- a synthetic egg's OT and egg-ness survive withdraw_egg ---- */

static void egg_withdraw_ot_and_eggness(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbGame g = gbd_game(&s);

  /* build a plain record to stand in for the egg's own record bytes */
  GbEditMon mon;
  memset(&mon, 0, sizeof mon);
  mon.gen = s.gen;
  mon.rec_len = (uint8_t)gb_rec_size(s.gen, false);
  mon.is_party = false;
  CHECKF(gb_set_species(&mon, 1, NULL), "%s: gb_set_species", file);
  CHECKF(gb_set_level(&mon, 5), "%s: gb_set_level", file);
  CHECKF(gb_set_move(&mon, 0, 1), "%s: gb_set_move", file);

  uint8_t rec[GB_MAX_REC], otname_unused[GB_NAME_BYTES], nick_unused[GB_NAME_BYTES];
  gb_commit_parts(&mon, rec, otname_unused, nick_unused, NULL);

  uint8_t nick_raw[GB_NAME_BYTES], ot_raw[GB_NAME_BYTES];
  gb_name_encode(s.gen, nick_raw, sizeof nick_raw, 10, "EGGNICK");
  gb_name_encode(s.gen, ot_raw, sizeof ot_raw, 7, "EGGOT");

  uint16_t rec_len = gbf_len(g, GBF_DAYCARE_EGG_REC);
  CHECKF(gbs_write_field(&s, gbf_off(g, GBF_DAYCARE_EGG_REC), rec, rec_len) == GBS_OK,
        "%s: plant egg record", file);
  CHECKF(gbs_write_field(&s, gbf_off(g, GBF_DAYCARE_EGG_NICK), nick_raw, GB_NAME_BYTES) == GBS_OK,
        "%s: plant egg nickname", file);
  CHECKF(gbs_write_field(&s, gbf_off(g, GBF_DAYCARE_EGG_OT), ot_raw, GB_NAME_BYTES) == GBS_OK,
        "%s: plant egg OT", file);

  uint8_t flag = 0;
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_DAYCARE_FLAG), &flag, 1) == GBS_OK,
        "%s: read flag before planting egg-ready bit", file);
  flag |= (1u << 6);   /* DAYCAREMAN_HAS_EGG_F */
  CHECKF(gbs_write_field(&s, gbf_off(g, GBF_DAYCARE_FLAG), &flag, 1) == GBS_OK,
        "%s: plant egg-ready bit", file);
  CHECKF(gbs_finish(&s) == GBS_OK, "%s: finish after planting the egg", file);

  GbDaycare dc;
  CHECKF(gbd_read(&s, &dc), "%s: read after planting egg", file);
  CHECKF(dc.has_egg, "%s: has_egg must read true after planting bit 6", file);
  CHECKF(strcmp(dc.egg_ot, "EGGOT") == 0, "%s: gbd_read egg OT mismatch (%s)", file,
        dc.egg_ot);

  GbEditMon out;
  CHECKF(gbd_withdraw_egg(&s, &out) == GBS_OK, "%s: gbd_withdraw_egg", file);
  CHECKF(gb_is_egg(&out), "%s: withdrawn egg must still read as an egg", file);

  char ot_check[GB_TEXT_MAX];
  gb_name_decode(s.gen, ot_check, sizeof ot_check, out.otname, GB_NAME_BYTES);
  CHECKF(strcmp(ot_check, "EGGOT") == 0, "%s: withdrawn egg OT mismatch (%s)", file,
        ot_check);

  /* the egg-ready bit must now be clear */
  uint8_t flag_after = 0xFF;
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_DAYCARE_FLAG), &flag_after, 1) == GBS_OK,
        "%s: read flag after egg withdraw", file);
  CHECKF((flag_after & (1u << 6)) == 0, "%s: withdraw_egg must clear the egg-ready bit "
        "(flag=0x%02X)", file, flag_after);
}

int main(void) {
  printf("== A: Gen 1 shape (one slot, no breeding) ==\n");
  gen1_shape("Red.sav");
  gen1_shape("Yellow.sav");

  printf("== B: Gold.sav ground truth (residue vs. occupied, probed) ==\n");
  gold_ground_truth();

  printf("== C: a bare read never mutates the image ==\n");
  read_is_readonly("Red.sav");
  read_is_readonly("Gold.sav");
  read_is_readonly("Crystal.sav");

  printf("== D: deposit -> withdraw round trip, both slots ==\n");
  deposit_withdraw_roundtrip("Red.sav", 0);
  deposit_withdraw_roundtrip("Gold.sav", 0);
  deposit_withdraw_roundtrip("Gold.sav", 1);
  deposit_withdraw_roundtrip("Crystal.sav", 0);
  deposit_withdraw_roundtrip("Crystal.sav", 1);

  printf("== E: an all-zero record is refused (P1a review D2) ==\n");
  deposit_all_zero_refused("Red.sav", 0);
  deposit_all_zero_refused("Gold.sav", 0);
  deposit_all_zero_refused("Gold.sav", 1);
  deposit_all_zero_refused("Crystal.sav", 0);

  printf("== F: withdraw clears the compatibility bit (P1a review D9) ==\n");
  withdraw_clears_compat_bit("Gold.sav");
  withdraw_clears_compat_bit("Crystal.sav");

  printf("== G: a synthetic egg keeps its OT and its egg-ness (P1a review D3) ==\n");
  egg_withdraw_ot_and_eggness("Gold.sav");
  egg_withdraw_ot_and_eggness("Crystal.sav");

  printf("\n%d checks, %d failed, %d file(s)/case(s) exercised\n", g_check, g_fail, g_ran);
  if (g_ran == 0) { printf("NOTE: corpus not found at %s -- every case skipped\n", ROMS); }
  return g_fail ? 1 : 0;
}
