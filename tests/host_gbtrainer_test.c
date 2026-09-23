/* source/gb_trainer.c -- the pure-C trainer-card core -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbtrainer_test.c \
 *      source/gb_trainer.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/item_map_g2g3.c source/item_map_g1g2.c source/gb_item_names.c source/gb_bag.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbt && /tmp/hgbt
 *
 * The corpus is Guy's own cartridge dumps (BACKLOG #49; docs/GEN12-PARITY-DESIGN.md).
 * They live OUTSIDE the repo (gitignored at gba-toolkit/roms/gb/) and are read only
 * here -- every mutation happens on an in-memory copy -- so a missing corpus SKIPS
 * rather than fails, but a corpus that IS present must match the design doc's own
 * VERIFIED numbers (Appendix A) exactly, not approximately.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "gb_trainer.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECKF(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_orig[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_protect[sizeof g_img];   /* 1 = this byte is allowed to have changed */

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

/* ---------------------------------------------------------------- A: readers */

/* Sentinel meaning "the design doc's Appendix A does not list this value for this
 * save -- check only the cap/plausibility bound, not an exact number." */
#define UNKNOWN16 0xFFFFu

static void expect_read(const char* file, uint8_t expect_gen,
                        const char* name, const char* rival,
                        uint16_t tid, uint32_t money, uint16_t coins,
                        int badges_or_johto, int kanto /* -1 = Gen 1, single BADGES */,
                        uint16_t h, uint8_t m, uint8_t sec, uint8_t f,
                        uint16_t dex_owned, uint16_t dex_seen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  printf("  -- %s (%u bytes)\n", file, (unsigned)len);

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == expect_gen, "%s: expected gen %d, got %d", file, expect_gen, s.gen);

  GbTrainer t;
  CHECKF(gbt_read(&s, &t), "%s: gbt_read", file);

  if (name) CHECKF(strcmp(t.name, name) == 0, "%s: name '%s' != '%s'", file, t.name, name);
  if (rival) CHECKF(strcmp(t.rival_name, rival) == 0, "%s: rival '%s' != '%s'", file, t.rival_name, rival);
  CHECKF(t.trainer_id == tid, "%s: tid %u != %u", file, t.trainer_id, tid);
  CHECKF(t.money == money, "%s: money %u != %u", file, t.money, money);
  CHECKF(t.money <= 999999u, "%s: money over cap", file);
  if (coins != UNKNOWN16)
    CHECKF(t.coins == coins, "%s: coins %u != %u", file, t.coins, coins);
  CHECKF(t.coins <= 9999u, "%s: coins over cap", file);
  if (kanto < 0) {
    CHECKF(t.badges == (uint8_t)badges_or_johto, "%s: badges 0x%02X != 0x%02X",
           file, t.badges, badges_or_johto);
  } else {
    CHECKF(t.badges_johto == (uint8_t)badges_or_johto, "%s: johto 0x%02X != 0x%02X",
           file, t.badges_johto, badges_or_johto);
    CHECKF(t.badges_kanto == (uint8_t)kanto, "%s: kanto 0x%02X != 0x%02X",
           file, t.badges_kanto, kanto);
  }
  CHECKF(t.playtime.hours == h, "%s: playtime h %u != %u", file, t.playtime.hours, h);
  CHECKF(t.playtime.minutes == m, "%s: playtime m %u != %u", file, t.playtime.minutes, m);
  CHECKF(t.playtime.seconds == sec, "%s: playtime s %u != %u", file, t.playtime.seconds, sec);
  CHECKF(t.playtime.frames == f, "%s: playtime f %u != %u", file, t.playtime.frames, f);
  CHECKF(t.playtime.hours < 999, "%s: playtime hours implausible", file);
  CHECKF(t.dex_owned == dex_owned, "%s: dex_owned %u != %u", file, t.dex_owned, dex_owned);
  CHECKF(t.dex_seen == dex_seen, "%s: dex_seen %u != %u", file, t.dex_seen, dex_seen);
}

static void expect_gen2_extras(const char* file, uint32_t moms_money, int has_gender,
                               uint8_t gender) {
  uint32_t len = load(file);
  if (!len) return;
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK) return;
  GbTrainer t;
  if (!gbt_read(&s, &t)) return;
  CHECKF(t.has_mom, "%s: expected has_mom", file);
  CHECKF(t.moms_money == moms_money, "%s: moms_money %u != %u", file, t.moms_money, moms_money);
  CHECKF(t.moms_money <= 999999u, "%s: moms_money over cap", file);
  if (has_gender) {
    CHECKF(t.has_gender, "%s: expected has_gender", file);
    CHECKF(t.gender == gender, "%s: gender %u != %u", file, t.gender, gender);
  } else {
    CHECKF(!t.has_gender, "%s: did not expect has_gender", file);
  }
}

/* Virtual Console .sav.dat: identical first 0x8000 bytes to the cartridge save
 * (§1.10) -- a light sanity pass, not a re-derivation of every field. */
static void vc_sanity(const char* file, uint16_t expect_tid) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbTrainer t;
  CHECKF(gbt_read(&s, &t), "%s: gbt_read", file);
  CHECKF(t.trainer_id == expect_tid, "%s: tid %u != %u", file, t.trainer_id, expect_tid);
  CHECKF(t.money <= 999999u, "%s: money over cap", file);
  CHECKF(t.coins <= 9999u, "%s: coins over cap", file);
}

/* P1a review D1 (blocking) -- a pure gbt_read -> gbt_write must not change a
 * single byte, on any corpus save. Before the fix, gbt_write's unconditional
 * set_name() flattened every real name field's post-terminator residue to 0x50
 * fill (Red: 8 stray bytes, Yellow: 7, Gold/Crystal: 3) on every write, edited or
 * not; D5 was a consequence -- a corpus name whose residue happened to decode past
 * GB_OT_GLYPHS glyphs made gbt_write() refuse EVERY edit, including unrelated
 * ones. Includes both VC .sav.dat files (same field layout, different tail). */
/* `plant_overcap`: P1a re-verify D3 -- before the no-op read/write, force
 * money/coins/mom's-money/play-time-hours to values already OVER their caps
 * (0xABCDEF, 0xFFFF, 0xABCDEF, 1500h), through gbs_write_field()/gbs_finish() so
 * the planted state is itself a valid, checksummed save (not just poked bytes),
 * then re-baseline g_orig to THAT state. The bug this catches: an earlier version
 * of set_u_unless_same() compared the field's RAW stored value against an
 * ALREADY-CLAMPED target, so an over-cap stored value never matched and got
 * rewritten down to the cap by this same "no-op" -- measured on Gold.sav, editing
 * only a badge silently turned money 0xABCDEF into 0x0F423F. Gen 2 only (Gen 1's
 * money is BCD, which cannot represent an out-of-cap value without also being
 * invalid BCD -- money_ok already covers that case, D4). */
static void noop_zero_diff(const char* file, bool plant_overcap) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  bool planted = false;
  if (plant_overcap) {
    GbSession ps;
    if (gbs_open(&ps, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK && ps.gen == GB_GEN2) {
      GbGame pg = gbt_game(&ps);
      static const uint8_t money_bad[3] = { 0xAB, 0xCD, 0xEF };
      static const uint8_t coins_bad[2] = { 0xFF, 0xFF };
      static const uint8_t hours_bad[2] = { 0x05, 0xDC };   /* 1500, over the 999 cap */
      if (gbt_field_present(pg, GBF_MONEY_BIN))
        gbs_write_field(&ps, gbf_off(pg, GBF_MONEY_BIN), money_bad, 3);
      if (gbt_field_present(pg, GBF_COINS_BIN))
        gbs_write_field(&ps, gbf_off(pg, GBF_COINS_BIN), coins_bad, 2);
      if (gbt_field_present(pg, GBF_MOMS_MONEY))
        gbs_write_field(&ps, gbf_off(pg, GBF_MOMS_MONEY), money_bad, 3);
      if (gbt_field_present(pg, GBF_GAMETIME_HOURS))
        gbs_write_field(&ps, gbf_off(pg, GBF_GAMETIME_HOURS), hours_bad, 2);
      CHECKF(gbs_finish(&ps) == GBS_OK, "%s: gbs_finish after planting over-cap values", file);
      memcpy(g_orig, g_img, len);   /* the planted (valid, checksummed) state IS the baseline */
      planted = true;
    }
  }

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbTrainer t;
  CHECKF(gbt_read(&s, &t), "%s: gbt_read", file);

  if (planted)
    CHECKF(t.money > 999999u || t.coins > 9999u || t.moms_money > 999999u ||
          t.playtime.hours > 999u,
          "%s: planted over-cap values did not read back over-cap (setup broken)", file);

  GbsStatus st = gbt_write(&s, &t);
  CHECKF(st == GBS_OK, "%s: no-op gbt_write status %s", file, gbs_status_text(st));

  uint32_t diff = 0, first = 0;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { if (!diff) first = i; diff++; }
  CHECKF(diff == 0,
        "%s: no-op gbt_write changed %u byte(s), first at 0x%04X (0x%02X -> 0x%02X)",
        file, diff, first, g_orig[first], g_img[first]);
}

/* ---------------------------------------------------------------- B: round trip */

/* Mark [off, off+len) protected in g_protect. */
static void protect(uint32_t off, uint32_t len) {
  for (uint32_t i = 0; i < len && off + i < sizeof g_protect; i++) g_protect[off + i] = 1;
}

/* Also protect wherever [off,len) lands inside a Gen-2 mirror source region, in its
 * BACKUP destination -- g2w_finish() re-mirrors every write before it re-stamps the
 * checksums, so a field inside a mirrored span changes in two places, not one. */
static void protect_mirrored(G2Version ver, uint32_t off, uint32_t len) {
  const G2MirrorRegion* map;
  int n = g2_mirror_map(ver, &map);
  for (int r = 0; r < n; r++) {
    uint32_t from = map[r].from, to = map[r].to; /* inclusive */
    uint32_t lo = off, hi = off + len; /* [lo,hi) */
    uint32_t clo = lo > from ? lo : from;
    uint32_t chi = hi < to + 1 ? hi : to + 1;
    if (clo < chi) protect(map[r].dest + (clo - from), chi - clo);
  }
}

static void protect_field(GbGame g, uint8_t gen, G2Version ver, GbField f) {
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len) return;
  protect(off, len);
  if (gen == GB_GEN2) protect_mirrored(ver, off, len);
}

/* One full round trip: open, edit money/coins/badges/playtime/name, gbs_finish
 * (inside gbt_write), re-open on the SAME buffer, read back, and prove the only
 * bytes that moved are the fields just written (plus their Gen-2 mirror and
 * whichever checksum bytes the engine re-stamps). */
static void roundtrip(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  printf("  -- roundtrip %s\n", file);

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == expect_gen, "%s: gen", file);
  GbGame g = gbt_game(&s);
  G2Version ver = (expect_gen == GB_GEN2) ? s.g2w.sv.version : G2_VER_NONE;

  GbTrainer t;
  CHECKF(gbt_read(&s, &t), "%s: gbt_read before edit", file);

  t.money            = 123456;
  t.coins            = 555;
  t.playtime.hours   = 10;
  t.playtime.minutes = 20;
  t.playtime.seconds = 30;
  t.playtime.frames  = 5;
  strcpy(t.name, "TESTR");
  if (expect_gen == GB_GEN1) {
    t.badges = 0x3C;
  } else {
    t.badges_johto = 0x0F;
    t.badges_kanto = 0xF0;
  }

  GbsStatus st = gbt_write(&s, &t);
  CHECKF(st == GBS_OK, "%s: gbt_write status %s", file, gbs_status_text(st));

  /* Re-open a FRESH session over the same (now edited) buffer -- proves the image
   * still parses and its checksum(s) are valid, exactly like a real reload.
   *
   * P1a review D9: gate every s2-using CHECK on the open (and read) actually having
   * succeeded, rather than letting a failed gbs_open()/gbt_read() cascade into a
   * pile of misleading "did not round-trip" failures on a session that was never
   * valid to read from in the first place. */
  GbSession s2;
  bool s2_ok = gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK;
  CHECKF(s2_ok, "%s: re-open after edit", file);
  GbTrainer t2;
  bool t2_ok = s2_ok && gbt_read(&s2, &t2);
  CHECKF(t2_ok, "%s: gbt_read after edit", file);

  if (t2_ok) {
    CHECKF(t2.money == 123456, "%s: money did not round-trip", file);
    CHECKF(t2.coins == 555, "%s: coins did not round-trip", file);
    CHECKF(t2.playtime.hours == 10 && t2.playtime.minutes == 20 &&
          t2.playtime.seconds == 30 && t2.playtime.frames == 5,
          "%s: playtime did not round-trip", file);
    CHECKF(strcmp(t2.name, "TESTR") == 0, "%s: name did not round-trip ('%s')", file, t2.name);
    if (expect_gen == GB_GEN1) {
      CHECKF(t2.badges == 0x3C, "%s: badges did not round-trip", file);
    } else {
      CHECKF(t2.badges_johto == 0x0F && t2.badges_kanto == 0xF0,
            "%s: badges did not round-trip", file);
    }
  }

  /* ---- byte-diff proof: only the written fields (+ mirror + checksum) moved ---- */
  memset(g_protect, 0, sizeof g_protect);
  protect_field(g, expect_gen, ver, GBF_PLAYER_NAME);
  if (gbt_field_present(g, GBF_MONEY))     protect_field(g, expect_gen, ver, GBF_MONEY);
  if (gbt_field_present(g, GBF_MONEY_BIN)) protect_field(g, expect_gen, ver, GBF_MONEY_BIN);
  if (gbt_field_present(g, GBF_COINS))     protect_field(g, expect_gen, ver, GBF_COINS);
  if (gbt_field_present(g, GBF_COINS_BIN)) protect_field(g, expect_gen, ver, GBF_COINS_BIN);
  if (expect_gen == GB_GEN1) {
    protect_field(g, expect_gen, ver, GBF_BADGES);
    protect_field(g, expect_gen, ver, GBF_PLAYTIME_HOURS);
    protect_field(g, expect_gen, ver, GBF_PLAYTIME_MAXED);
    protect_field(g, expect_gen, ver, GBF_PLAYTIME_MINUTES);
    protect_field(g, expect_gen, ver, GBF_PLAYTIME_SECONDS);
    protect_field(g, expect_gen, ver, GBF_PLAYTIME_FRAMES);
    protect(GEN1_OFF_CHECKSUM, 1);
  } else {
    protect_field(g, expect_gen, ver, GBF_BADGES_JOHTO);
    protect_field(g, expect_gen, ver, GBF_BADGES_KANTO);
    protect_field(g, expect_gen, ver, GBF_GAMETIME_HOURS);
    protect_field(g, expect_gen, ver, GBF_GAMETIME_MINUTES);
    protect_field(g, expect_gen, ver, GBF_GAMETIME_SECONDS);
    protect_field(g, expect_gen, ver, GBF_GAMETIME_FRAMES);
    protect(g2_checksum_primary_off(ver), 2);
    protect(g2_checksum_backup_off(ver), 2);
  }

  uint32_t unexplained = 0, first = 0;
  for (uint32_t i = 0; i < len; i++) {
    if (g_img[i] != g_orig[i] && !g_protect[i]) {
      if (!unexplained) first = i;
      unexplained++;
    }
  }
  CHECKF(unexplained == 0,
        "%s: %u byte(s) changed outside the written fields, first at 0x%04X (0x%02X -> 0x%02X)",
        file, unexplained, first, g_orig[first], g_img[first]);
}

/* ------------------------------------------------------- B2: single-field write
 * (P1a re-verify D11)
 *
 * The whole no-op/round-trip/clamp story above rests on ONE flag: gbt_write()'s
 * local `changed`, which gates whether gbs_finish() runs at all (see that
 * function's own comment -- skipping it is what keeps a true no-op from moving a
 * Gen-2 save's stored checksums). Nothing before this point actually EXERCISES a
 * write that changes exactly one field while every set_u_... / set_name_... call for every
 * OTHER field takes its own "unchanged" early-return -- every roundtrip() case
 * above changes five-plus fields at once, and so does every retail-gate case, so a
 * single call site that writes via bare set_u() without ever raising `*changed`
 * would still pass the whole suite and the gate 38/0 (proven: dropping the flag
 * from one branch by hand, deliberately, made exactly this test fail with a
 * failed-to-reopen "stale checksum" error, and nothing else in either suite
 * noticed). one_field_only() closes that hole: change EXACTLY one field, write,
 * and require gbs_open() on the same buffer to still succeed (Gen 2: this is
 * where a skipped gbs_finish() shows up, as a checksum the game would reject) and
 * a fresh gbt_read() to show the new value. */

static uint32_t g_want_num;
static bool     g_want_bool;
static char     g_want_str[GB_TEXT_MAX];

typedef void (*Mutator)(GbTrainer* t);
typedef bool (*Matcher)(const GbTrainer* t);

static void mut_name(GbTrainer* t) {
  const char* target = (strcmp(t->name, "ZQXW") == 0) ? "ZQXWY" : "ZQXW";
  strncpy(g_want_str, target, sizeof g_want_str - 1);
  g_want_str[sizeof g_want_str - 1] = 0;
  strcpy(t->name, target);
}
static bool match_name(const GbTrainer* t) { return strcmp(t->name, g_want_str) == 0; }

static void mut_tid(GbTrainer* t) { t->trainer_id ^= 1u; g_want_num = t->trainer_id; }
static bool match_tid(const GbTrainer* t) { return t->trainer_id == g_want_num; }

static void mut_money(GbTrainer* t) {
  g_want_num = (t->money == 111111u) ? 222222u : 111111u;
  t->money = g_want_num;
}
static bool match_money(const GbTrainer* t) { return t->money == g_want_num; }

static void mut_coins(GbTrainer* t) {
  g_want_num = (t->coins == 555u) ? 777u : 555u;
  t->coins = (uint16_t)g_want_num;
}
static bool match_coins(const GbTrainer* t) { return t->coins == g_want_num; }

static void mut_moms(GbTrainer* t) {
  g_want_num = (t->moms_money == 54321u) ? 12345u : 54321u;
  t->moms_money = g_want_num;
}
static bool match_moms(const GbTrainer* t) { return t->moms_money == g_want_num; }

static void mut_mombits(GbTrainer* t) {
  t->mom_saving_bits = (uint8_t)(t->mom_saving_bits ^ 0x01u);
  g_want_num = t->mom_saving_bits;
}
static bool match_mombits(const GbTrainer* t) { return t->mom_saving_bits == g_want_num; }

static void mut_momactive(GbTrainer* t) { t->mom_active = !t->mom_active; g_want_bool = t->mom_active; }
static bool match_momactive(const GbTrainer* t) { return t->mom_active == g_want_bool; }

static void mut_badges(GbTrainer* t) {
  t->badges = (uint8_t)(t->badges ^ 0x01u);
  g_want_num = t->badges;
}
static bool match_badges(const GbTrainer* t) { return t->badges == g_want_num; }

static void mut_johto(GbTrainer* t) {
  t->badges_johto = (uint8_t)(t->badges_johto ^ 0x01u);
  g_want_num = t->badges_johto;
}
static bool match_johto(const GbTrainer* t) { return t->badges_johto == g_want_num; }

static void mut_kanto(GbTrainer* t) {
  t->badges_kanto = (uint8_t)(t->badges_kanto ^ 0x01u);
  g_want_num = t->badges_kanto;
}
static bool match_kanto(const GbTrainer* t) { return t->badges_kanto == g_want_num; }

static void mut_hours(GbTrainer* t) {
  g_want_num = (t->playtime.hours == 5u) ? 6u : 5u;
  t->playtime.hours = (uint16_t)g_want_num;
}
static bool match_hours(const GbTrainer* t) { return t->playtime.hours == g_want_num; }

static void mut_maxed(GbTrainer* t) { t->playtime.maxed = !t->playtime.maxed; g_want_bool = t->playtime.maxed; }
static bool match_maxed(const GbTrainer* t) { return t->playtime.maxed == g_want_bool; }

static void mut_minutes(GbTrainer* t) {
  g_want_num = (t->playtime.minutes == 5u) ? 6u : 5u;
  t->playtime.minutes = (uint8_t)g_want_num;
}
static bool match_minutes(const GbTrainer* t) { return t->playtime.minutes == g_want_num; }

static void mut_seconds(GbTrainer* t) {
  g_want_num = (t->playtime.seconds == 5u) ? 6u : 5u;
  t->playtime.seconds = (uint8_t)g_want_num;
}
static bool match_seconds(const GbTrainer* t) { return t->playtime.seconds == g_want_num; }

static void mut_frames(GbTrainer* t) {
  g_want_num = (t->playtime.frames == 5u) ? 6u : 5u;
  t->playtime.frames = (uint8_t)g_want_num;
}
static bool match_frames(const GbTrainer* t) { return t->playtime.frames == g_want_num; }

/* `probe_b` is GBF_FIELD_COUNT for a field that has only one possible backing id
 * across every game (GBF_FIELD_COUNT itself is never a real field, gb_fields.h's
 * own sentinel-by-construction: it is declared one past every real enumerator). */
static void one_field(const char* file, const char* label, GbField probe_a, GbField probe_b,
                      Mutator mutate, Matcher matches) {
  uint32_t len = load(file);
  if (!len) return;   /* corpus file absent: other sections already report this */
  g_ran++;

  GbSession s;
  bool open_ok = gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK;
  CHECKF(open_ok, "%s/%s: open", file, label);
  if (!open_ok) return;

  GbGame g = gbt_game(&s);
  bool present = gbt_field_present(g, probe_a) ||
                (probe_b != GBF_FIELD_COUNT && gbt_field_present(g, probe_b));
  if (!present) return;   /* this generation lacks the field: not a failure */

  GbTrainer t;
  bool read_ok = gbt_read(&s, &t);
  CHECKF(read_ok, "%s/%s: gbt_read", file, label);
  if (!read_ok) return;

  mutate(&t);

  GbsStatus st = gbt_write(&s, &t);
  CHECKF(st == GBS_OK, "%s/%s: gbt_write status %s", file, label, gbs_status_text(st));

  /* The load-bearing check: a real single-field write that skipped gbs_finish()
   * (a dropped `changed = true`) leaves a Gen-2 save's stored checksums stale --
   * gbs_open() on the very same buffer is where that surfaces. */
  GbSession s2;
  bool reopen_ok = gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK;
  CHECKF(reopen_ok, "%s/%s: re-open after single-field write (stale checksum?)", file, label);
  if (!reopen_ok) return;

  GbTrainer t2;
  bool reread_ok = gbt_read(&s2, &t2);
  CHECKF(reread_ok, "%s/%s: gbt_read after single-field write", file, label);
  if (!reread_ok) return;

  CHECKF(matches(&t2), "%s/%s: the changed field did not read back as changed", file, label);
}

static void one_field_only(void) {
  static const char* const files[] = {
    "Red.sav", "Yellow.sav", "Gold.sav", "Crystal.sav",
    "Gold-VC.sav.dat", "Crystal-VC.sav.dat"
  };
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    const char* file = files[i];
    one_field(file, "name",        GBF_PLAYER_NAME,      GBF_FIELD_COUNT,     mut_name,      match_name);
    one_field(file, "TID",         GBF_TRAINER_ID,       GBF_FIELD_COUNT,     mut_tid,       match_tid);
    one_field(file, "money",       GBF_MONEY,            GBF_MONEY_BIN,       mut_money,     match_money);
    one_field(file, "coins",       GBF_COINS,            GBF_COINS_BIN,       mut_coins,     match_coins);
    one_field(file, "mom's money", GBF_MOMS_MONEY,       GBF_FIELD_COUNT,     mut_moms,      match_moms);
    one_field(file, "mom bits",    GBF_MOM_SAVING_FLAG,  GBF_FIELD_COUNT,     mut_mombits,   match_mombits);
    one_field(file, "mom active",  GBF_MOM_SAVING_FLAG,  GBF_FIELD_COUNT,     mut_momactive, match_momactive);
    one_field(file, "badges",      GBF_BADGES,           GBF_FIELD_COUNT,     mut_badges,    match_badges);
    one_field(file, "Johto",       GBF_BADGES_JOHTO,     GBF_FIELD_COUNT,     mut_johto,     match_johto);
    one_field(file, "Kanto",       GBF_BADGES_KANTO,     GBF_FIELD_COUNT,     mut_kanto,     match_kanto);
    one_field(file, "hours",       GBF_PLAYTIME_HOURS,   GBF_GAMETIME_HOURS,  mut_hours,     match_hours);
    one_field(file, "maxed",       GBF_PLAYTIME_MAXED,   GBF_GAMETIME_CAP,    mut_maxed,     match_maxed);
    one_field(file, "minutes",     GBF_PLAYTIME_MINUTES, GBF_GAMETIME_MINUTES,mut_minutes,   match_minutes);
    one_field(file, "seconds",     GBF_PLAYTIME_SECONDS, GBF_GAMETIME_SECONDS,mut_seconds,   match_seconds);
    one_field(file, "frames",      GBF_PLAYTIME_FRAMES,  GBF_GAMETIME_FRAMES, mut_frames,    match_frames);
  }
}

/* ---------------------------------------------------------- B3: BACKLOG #126b */

/* has_pokedex must fail OPEN on Gen 2, not closed: a session whose
 * GBF_STATUS_FLAGS byte is unreadable (present in the field table, but out
 * of THIS session's own bounds) must leave has_pokedex at its default
 * (true), never silently flip it to false and hide a real save's #DEX row.
 *
 * No corpus file needed -- a synthetic GbSession built directly (not through
 * gbs_open(), which would refuse a buffer too short for Gen 2's own checksum
 * coverage) with `len` cut short right after PLAYER_NAME/TRAINER_ID (both
 * near 0x2009-0x2016 on GS/Crystal) but well before GBF_STATUS_FLAGS
 * (0x23d9/0x23da): gbt_field_present() still reports it present (a static
 * per-game table lookup, independent of any one session's buffer size), so
 * get_u() is the thing that fails here, exactly the "field IS present but
 * this read failed" case the brief distinguishes from "this game lacks the
 * field entirely" (which correctly stays false via has_gender's own
 * identical posture, untouched by this fix). */
static void has_pokedex_fail_open(void) {
  g_ran++;
  static uint8_t buf[0x2100];
  memset(buf, 0, sizeof buf);   /* PLAYER_NAME/TRAINER_ID content is unchecked by
                                  * get_name/get_u -- any bytes decode */

  GbSession s;
  memset(&s, 0, sizeof s);
  s.img = buf;
  s.len = sizeof buf;           /* covers TRAINER_ID (0x2009+2) and PLAYER_NAME
                                  * (0x200b+11) but NOT GBF_STATUS_FLAGS (0x23d9/
                                  * 0x23da) -- deliberately short of the real
                                  * checksum-covered length gbs_open() would demand */
  s.open = true;
  s.gen  = GB_GEN2;              /* g2w left zeroed: gbt_game() reads .version == 0,
                                  * != G2_VER_CRYSTAL, so this is the GS field row --
                                  * same STATUS_FLAGS-past-len shape as Crystal's row */

  GbTrainer t;
  bool ok = gbt_read(&s, &t);
  CHECK(ok, "fail-open: gbt_read (only hard-required fields, both in-bounds)");
  if (ok) {
    CHECK(t.has_pokedex, "fail-open: has_pokedex stays true when GBF_STATUS_FLAGS "
                          "is present but out of this session's bounds (BACKLOG #126b)");
  }
}

/* ---------------------------------------------------------------- C: refusals */

static void refusals(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == expect_gen, "%s: gen", file);
  GbTrainer t;
  CHECKF(gbt_read(&s, &t), "%s: gbt_read", file);

  /* name too long: more than GB_OT_GLYPHS (7) glyphs */
  memcpy(g_orig, g_img, len);
  GbTrainer bad = t;
  strcpy(bad.name, "TOOLONGNAME");
  GbsStatus st = gbt_write(&s, &bad);
  CHECKF(st == GBS_ERR_ARG, "%s: name-too-long refused with GBS_ERR_ARG, got %s",
        file, gbs_status_text(st));
  CHECKF(memcmp(g_img, g_orig, len) == 0, "%s: name-too-long left the image touched", file);

  /* bad glyph: a character this generation's GB charset cannot store at all */
  bad = t;
  strcpy(bad.name, "\xe6\x97\xa5");   /* U+65E5, not a GB-charset glyph either gen */
  st = gbt_write(&s, &bad);
  CHECKF(st == GBS_ERR_ARG, "%s: bad-glyph name refused with GBS_ERR_ARG, got %s",
        file, gbs_status_text(st));
  CHECKF(memcmp(g_img, g_orig, len) == 0, "%s: bad-glyph name left the image touched", file);

  /* money over cap: CLAMPED to 999999, not refused (source/gb_trainer.h's own
   * contract: "Setters clamp to the caps"). */
  GbTrainer big = t;
  big.money = 5000000;
  big.coins = 50000;
  st = gbt_write(&s, &big);
  CHECKF(st == GBS_OK, "%s: over-cap money/coins should clamp, not refuse (got %s)",
        file, gbs_status_text(st));
  /* P1a review D9: same open/read guard as roundtrip()'s s2 -- do not chase a
   * failed gbs_open()/gbt_read() with clamp assertions that would just misreport
   * "did not clamp" for a session that was never valid to read. */
  GbSession s3;
  bool s3_ok = gbs_open(&s3, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK;
  CHECKF(s3_ok, "%s: re-open after clamp", file);
  GbTrainer after;
  bool after_ok = s3_ok && gbt_read(&s3, &after);
  CHECKF(after_ok, "%s: gbt_read after clamp", file);
  if (after_ok) {
    CHECKF(after.money == 999999, "%s: money did not clamp to 999999 (got %u)",
          file, after.money);
    CHECKF(after.coins == 9999, "%s: coins did not clamp to 9999 (got %u)",
          file, after.coins);
  }
}

/* ---------------------------------------------------------------- main */

int main(void) {
  printf("== A: reads against docs/GEN12-PARITY-DESIGN.md Appendix A ==\n");
  expect_read("Red.sav", GB_GEN1, "ASH", "GARY", 12607, 798798, 695,
             0xFF, -1, 201, 30, 7, 42, 151, 151);
  expect_read("Yellow.sav", GB_GEN1, "RoC", "Kenny", 28226, 500000, UNKNOWN16,
             0xFF, -1, 19, 6, 44, 10, 151, 151);
  expect_read("Gold.sav", GB_GEN2, "MattiaPK", "ARGENTO", 46116, 999999, 9999,
             0xFF, 0xFF, 55, 58, 45, 8, 251, 251);
  expect_read("Crystal.sav", GB_GEN2, "MattiaPK", NULL, 8872, 502769, 1084,
             0xFF, 0xFF, 79, 32, 56, 20, 251, 251);
  expect_gen2_extras("Gold.sav", 85854, 0, 0);
  expect_gen2_extras("Crystal.sav", 977199, 1, 0);
  vc_sanity("Gold-VC.sav.dat", 46116);
  vc_sanity("Crystal-VC.sav.dat", 8872);

  printf("== B0: no-op read->write is a zero-byte diff (P1a review D1) ==\n");
  noop_zero_diff("Red.sav", false);
  noop_zero_diff("Yellow.sav", false);
  noop_zero_diff("Gold.sav", false);
  noop_zero_diff("Crystal.sav", false);
  noop_zero_diff("Gold-VC.sav.dat", false);
  noop_zero_diff("Crystal-VC.sav.dat", false);
  noop_zero_diff("Gold.sav", true);   /* P1a re-verify D3: planted over-cap values */

  printf("== B: round trips ==\n");
  roundtrip("Red.sav", GB_GEN1);
  roundtrip("Yellow.sav", GB_GEN1);
  roundtrip("Gold.sav", GB_GEN2);
  roundtrip("Crystal.sav", GB_GEN2);

  printf("== B2: single-field write (P1a re-verify D11) ==\n");
  one_field_only();

  printf("== B3: has_pokedex fails open, not closed (BACKLOG #126b) ==\n");
  has_pokedex_fail_open();

  printf("== C: refusals ==\n");
  refusals("Red.sav", GB_GEN1);
  refusals("Gold.sav", GB_GEN2);
  refusals("Crystal.sav", GB_GEN2);

  if (!g_ran) printf("  (no corpus present -- structural checks only)\n");
  printf("%s: %d/%d checks passed over %d save use(s)\n",
        g_fail ? "FAIL" : "ok", g_check - g_fail, g_check, g_ran);
  return g_fail ? 1 : 0;
}
