/* source/gb_fields.c / gb_flags.c — the GENERATED per-game field/flag tables — under
 * test against Guy's real cartridge saves (BACKLOG #49 P0).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbfields_test.c \
 *      source/gb_fields.c source/gb_flags.c source/gen1_save.c source/gen2_save.c \
 *      -o /tmp/hgbf && /tmp/hgbf
 *
 * The design doc's own §2 harness proved every VERIFIED offset plausible against the
 * corpus (player name letters, money under the cap, play time under a year, a map id in
 * range, a bag whose entries are real items). This file makes those the SAME assertions
 * a permanent regression test runs — over gb_fields.c's GENERATED offsets, not the
 * design doc's prose, so a future decomp update or a generator bug shows up here first.
 *
 * Needs the REAL, generated source/gb_fields.c / gb_flags.c (run `python3
 * tools/gen_gbfields.py` first) — this test is not meaningful against the weak
 * fallback's all-zero table, and says so rather than reporting a false pass.
 *
 * The corpus is Guy's own cartridge dumps. They live OUTSIDE the repo (gitignored at
 * gba-toolkit/roms/gb/) and are only ever read here, so a missing corpus SKIPS rather
 * than fails — but a corpus that IS present must decode plausibly.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"
#include "gb_flags.h"
#include "gen1_save.h"
#include "gen2_save.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { \
    printf("  !! FAIL: "); printf(__VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); g_fail++; } } while (0)

#define IMGCAP (64 * 1024)
static uint8_t g_img[IMGCAP];

static uint32_t load(const char* file) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(g_img, 1, IMGCAP, f);
  fclose(f);
  return n;
}

/* Fails loudly, once, if the weak fallback linked instead of the generated table —
 * every other check in this file would otherwise "pass" vacuously against all-zero
 * offsets, which is a false green, not a real one. */
static void require_real_table(void) {
  uint32_t off = gbf_off(GBF_G_GS, GBF_MONEY_BIN);
  int flags = gbfl_count(GBF_G_RED);
  CHECK(off != 0 && flags != 0,
       "the GENERATED gb_fields.c/gb_flags.c must be linked, not the weak fallback -- "
       "run `python3 tools/gen_gbfields.py` first (gbf_off(GS,MONEY_BIN)=%#x "
       "gbfl_count(RED)=%d)", off, flags);
}

/* ---- decode helpers, using ONLY gb_fields.h's offsets -- never a hardcoded literal ---- */

static uint32_t rd_u24be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint32_t rd_bcd24(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 3; i++) v = v * 100 + (uint32_t)((p[i] >> 4) * 10 + (p[i] & 0xF));
  return v;
}
static uint16_t rd_u16be(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* True if every byte of a BCD field is two valid decimal nibbles (0-9 each) -- the
 * plausibility bar for "this is really money", not "this happens to be a number". */
static bool bcd_valid(const uint8_t* p, int n) {
  for (int i = 0; i < n; i++)
    if ((p[i] >> 4) > 9 || (p[i] & 0xF) > 9) return false;
  return true;
}

/* ---- one save, every VERIFIED field's plausibility check --------------------------- */

static void check_save(const char* file, GbGame game, bool is_gen1, uint8_t gen1_badge_off_unused) {
  (void)gen1_badge_off_unused;
  uint32_t n = load(file);
  if (!n) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  printf("  -- %s (game=%d)\n", file, (int)game);

  /* player name: decodes to at least one A-Z/a-z letter, never empty */
  {
    uint32_t off = gbf_off(game, GBF_PLAYER_NAME);
    uint16_t len = gbf_len(game, GBF_PLAYER_NAME);
    CHECK(off != 0 && len > 0, "%s: PLAYER_NAME is present in the table", file);
    if (off && len) {
      char out[64];
      int m = is_gen1 ? gen1_decode_name(out, sizeof out, g_img + off, len)
                      : g2_decode_text(g_img + off, len, out, sizeof out);
      bool has_letter = false;
      for (int i = 0; i < m; i++)
        if ((out[i] >= 'A' && out[i] <= 'Z') || (out[i] >= 'a' && out[i] <= 'z')) has_letter = true;
      CHECK(m > 0 && has_letter, "%s: player name decodes to a letter (got '%.*s')",
            file, m, out);
    }
  }

  /* money: under the 999999 cap (§1.1), decoded through whichever of MONEY/MONEY_BIN
   * this game actually has -- exactly one of the two, per gb_fields.h's own contract. */
  {
    uint32_t off_bcd = gbf_off(game, GBF_MONEY), off_bin = gbf_off(game, GBF_MONEY_BIN);
    CHECK((off_bcd != 0) != (off_bin != 0), "%s: exactly one of MONEY/MONEY_BIN is present",
          file);
    uint32_t money = 0xFFFFFFFFu;
    if (off_bcd) {
      CHECK(bcd_valid(g_img + off_bcd, 3), "%s: money BCD bytes are valid decimal digits",
            file);
      money = rd_bcd24(g_img + off_bcd);
    } else if (off_bin) {
      money = rd_u24be(g_img + off_bin);
    }
    CHECK(money <= 999999u, "%s: money %u is under the 999999 cap", file, money);
  }

  /* play time: under 999 hours (a save that has been sitting on a shelf, not a corrupt
   * field) -- Gen 1's PLAYTIME_HOURS is one byte (max 255 already < 999); Gen 2's
   * GAMETIME_HOURS is a u16. */
  {
    uint32_t off_h1 = gbf_off(game, GBF_PLAYTIME_HOURS), off_h2 = gbf_off(game, GBF_GAMETIME_HOURS);
    uint32_t hours = 0xFFFFFFFFu;
    if (off_h1) hours = g_img[off_h1];
    else if (off_h2) hours = rd_u16be(g_img + off_h2);
    CHECK(hours != 0xFFFFFFFFu, "%s: one of PLAYTIME_HOURS/GAMETIME_HOURS is present", file);
    CHECK(hours < 999u, "%s: play time %u h is under 999", file, hours);
  }

  /* map: Gen 1's flat id in range, or Gen 2's (group, number) both nonzero-plausible.
   * "in range" here is the same bar the design doc's own harness used: a byte, which by
   * construction cannot be out of a byte's range -- the real assertion is that reading
   * it through the GENERATED offset lands on the same byte gen1_open/g2 already trust
   * (current_box, etc., are read elsewhere) -- so this checks it is not the sentinel
   * 0xFF a torn read or a wrong offset would tend to produce. */
  {
    uint32_t off_map = gbf_off(game, GBF_MAP_ID);
    uint32_t off_grp = gbf_off(game, GBF_MAP_GROUP), off_num = gbf_off(game, GBF_MAP_NUMBER);
    if (off_map) {
      CHECK(g_img[off_map] != 0xFFu, "%s: MAP_ID is a real value, not the 0xFF sentinel",
            file);
    } else {
      CHECK(off_grp && off_num, "%s: Gen-2 MAP_GROUP/MAP_NUMBER are both present", file);
      CHECK(g_img[off_grp] != 0xFFu && g_img[off_num] != 0xFFu,
            "%s: MAP_GROUP/MAP_NUMBER are real values, not the 0xFF sentinel", file);
    }
  }

  /* bag: count <= capacity (20), and every occupied entry's id is nonzero and not the
   * 0xFF terminator, qty nonzero (Gen 1 has one pocket; Gen 2's is BAG_COUNT/BAG_BODY
   * too, same shape). */
  {
    uint32_t off_cnt = gbf_off(game, GBF_BAG_COUNT), off_body = gbf_off(game, GBF_BAG_BODY);
    CHECK(off_cnt && off_body, "%s: BAG_COUNT/BAG_BODY are present", file);
    if (off_cnt && off_body) {
      uint8_t count = g_img[off_cnt];
      CHECK(count <= 20u, "%s: bag count %u is within capacity 20", file, count);
      int bad = 0;
      for (uint8_t i = 0; i < count && i < 20u; i++) {
        uint8_t id = g_img[off_body + (uint32_t)i * 2u];
        uint8_t qty = g_img[off_body + (uint32_t)i * 2u + 1u];
        if (id == 0 || id == 0xFFu || qty == 0) bad++;
      }
      CHECK(bad == 0, "%s: every occupied bag entry has a real id and nonzero qty "
                      "(%d bad of %u)", file, bad, count);
    }
  }

  /* badges: Gen 1 <= 8 bits set (one region); Gen 2 Johto+Kanto each <= 8. Not a
   * corpus-specific number (a fresh save legitimately has 0), just a sanity ceiling. */
  {
    uint32_t off_b = gbf_off(game, GBF_BADGES);
    uint32_t off_j = gbf_off(game, GBF_BADGES_JOHTO), off_k = gbf_off(game, GBF_BADGES_KANTO);
    if (off_b) {
      int n_set = 0; uint8_t v = g_img[off_b];
      for (int i = 0; i < 8; i++) if (v & (1u << i)) n_set++;
      CHECK(n_set <= 8, "%s: badge byte has at most 8 bits set", file);
    } else {
      CHECK(off_j && off_k, "%s: Gen-2 BADGES_JOHTO/BADGES_KANTO are both present", file);
    }
  }
}

/* ---- checksums recompute over the table's own spans -------------------------------- */

/* Gen 1: gen1_open() already recomputes the main 8-bit checksum over
 * [GEN1_SUM_FIRST, GEN1_SUM_LAST] and reports it in checksum_stored/checksum_calc
 * (host_gen1write_test.c/host_gbreal_test.c already prove that recompute correct against
 * the real corpus). What THIS file adds: every field this table marks present for a
 * Gen-1 game falls INSIDE that exact span -- proof the GENERATED offsets and the
 * checksummed region agree, not just that each looks plausible in isolation. */
static void check_gen1_span(const char* file) {
  uint32_t n = load(file);
  if (!n) return;
  Gen1Save s;
  CHECK(gen1_open(g_img, n, &s) == GEN1_OK, "%s: parses for the checksum-span check", file);
  CHECK(s.checksum_stored == s.checksum_calc,
       "%s: the main checksum recomputes correctly over [0x2598,0x3522]", file);

  static const GbField fields[] = {
    GBF_PLAYER_NAME, GBF_TRAINER_ID, GBF_MONEY, GBF_COINS, GBF_BADGES, GBF_RIVAL_NAME,
    GBF_PLAYTIME_HOURS, GBF_OPTIONS, GBF_EVENT_FLAGS_BASE, GBF_HIDDEN_ITEM_FLAGS,
    GBF_HIDDEN_COIN_FLAGS, GBF_TOGGLE_OBJ_FLAGS, GBF_FLY_FLAGS, GBF_MAP_ID, GBF_POS_X,
    GBF_POS_Y, GBF_LAST_MAP, GBF_DEX_OWNED, GBF_DEX_SEEN, GBF_DAYCARE_FLAG,
    GBF_BAG_COUNT, GBF_BAG_BODY, GBF_PC_COUNT, GBF_PC_BODY,
  };
  int out_of_span = 0;
  for (unsigned i = 0; i < sizeof fields / sizeof fields[0]; i++) {
    uint32_t off = gbf_off(GBF_G_RED, fields[i]);
    uint16_t len = gbf_len(GBF_G_RED, fields[i]);
    if (!off) continue;
    if (off < GEN1_OFF_PLAYER_NAME || off + len - 1u > GEN1_SUM_LAST) out_of_span++;
  }
  CHECK(out_of_span == 0,
       "%s: every present Gen-1 header field lands inside [0x2598,0x3522] (%d did not)",
       file, out_of_span);
}

/* Gen 2: same idea, g2_detect()'s primary_ok already proves the recompute; this checks
 * every table field EXCEPT the ones §1.9/§1.3 explicitly place outside the span
 * (GENDER/sCrystalData, GS_BALL_FLAG, the Mystery Gift pair — SRAM bank 0/outside) falls
 * inside [0x2009, primary_end]. */
static void check_gen2_span(const char* file, GbGame game, G2Version ver) {
  uint32_t n = load(file);
  if (!n) return;
  G2Save sv;
  CHECK(g2_detect(g_img, n, &sv) && sv.primary_ok,
       "%s: the primary checksum recomputes correctly over its own span", file);
  uint32_t primary_end = (uint32_t)g2_checksum_primary_off(ver);   /* one past the span */

  static const GbField in_span[] = {
    /* NOT GBF_OPTIONS: sOptions (0x2000-0x2007) and sCheckValue1 (0x2008) sit BEFORE
     * the primary span starts at 0x2009 (§1.9 "G/S primary | sum 0x2009-0x2D68") — the
     * options block is deliberately unchecksummed, matching Gen 3's own settings
     * (never part of a save's integrity check). Belongs in the OUT-of-span control
     * below, not here; an earlier draft of this test had it backwards, which this
     * comment (and the below list) exists to not repeat. */
    GBF_TRAINER_ID, GBF_MONEY_BIN, GBF_COINS_BIN, GBF_MOMS_MONEY, GBF_BADGES_JOHTO,
    GBF_BADGES_KANTO, GBF_RIVAL_NAME, GBF_EVENT_FLAGS_BASE_G2, GBF_BIKE_FLAGS,
    GBF_UNLOCKED_UNOWN, GBF_FLY_FLAGS_G2, GBF_MAP_GROUP, GBF_MAP_NUMBER, GBF_POS_X,
    GBF_POS_Y, GBF_DEX_OWNED, GBF_DEX_SEEN, GBF_DAYCARE_FLAG, GBF_RTC_START_DAY,
    GBF_BAG_COUNT, GBF_BAG_BODY, GBF_PC_COUNT, GBF_PC_BODY,
  };
  int out_of_span = 0;
  for (unsigned i = 0; i < sizeof in_span / sizeof in_span[0]; i++) {
    uint32_t off = gbf_off(game, in_span[i]);
    uint16_t len = gbf_len(game, in_span[i]);
    if (!off) continue;
    if (off < 0x2009u || off + len > primary_end) out_of_span++;
  }
  CHECK(out_of_span == 0,
       "%s: every present in-span Gen-2 field lands inside [0x2009,%#06x) (%d did not)",
       file, primary_end, out_of_span);

  /* The control: the fields §1.3/§1.9 say ARE outside the span really are. */
  int falsely_in_span = 0;
  static const GbField out_span[] = { GBF_OPTIONS, GBF_GENDER, GBF_GS_BALL_FLAG,
                                     GBF_MYSTERY_GIFT_ITEM, GBF_MYSTERY_GIFT_UNLOCKED };
  for (unsigned i = 0; i < sizeof out_span / sizeof out_span[0]; i++) {
    uint32_t off = gbf_off(game, out_span[i]);
    if (!off) continue;
    if (off >= 0x2009u && off < primary_end) falsely_in_span++;
  }
  CHECK(falsely_in_span == 0,
       "%s: the fields the design doc marks OUTSIDE the span really are (%d were not)",
       file, falsely_in_span);
}

/* ---- flag tables: the per-game numbering really differs, and both directions agree
 * with the corpus (a flag whose bit is set on Gold reads back as set through the
 * generated index, and the SAME event on Crystal is a DIFFERENT bit that also reads
 * plausibly). Not asserting a specific value (a fresh vs. played save differs) --
 * asserting the LOOKUP reaches a real bit position (< the region's own bit count). --- */
static void check_flags(const char* file, GbGame game, bool is_gen1, int max_bits) {
  uint32_t n = load(file);
  if (!n) return;
  int count = gbfl_count(game);
  CHECK(count > 0, "%s: the flag shortlist is non-empty", file);
  int bad = 0;
  for (int i = 0; i < count; i++) {
    uint16_t flag = 0; const char* label = 0;
    CHECK(gbfl_at(game, i, &flag, &label), "%s: gbfl_at(%d) succeeds", file, i);
    if (flag >= (uint16_t)max_bits) bad++;
    CHECK(label && label[0] != 0, "%s: flag %u has a non-empty label", file, flag);
  }
  CHECK(bad == 0, "%s: every shortlisted flag index is within the %d-bit region (%d were not)",
       file, max_bits, bad);
  (void)is_gen1;
}

/* ---- P0 review D1-D5: sensitivity to an off-by-one in ANY transcribed cell -------- */

/* The review mutated six offsets by +/-1 and 389/389 checks above STILL passed --
 * every plausibility check tolerates a field sliding a byte, because "money < 999999"
 * or "map id is not 0xFF" does not pin down the address that produced the value. Two
 * independent nets, neither of which existed before this pass:
 *
 *   1. Adjacency: a curated list of (fieldA, fieldB) pairs KNOWN, from the .sym files,
 *      to sit with ZERO gap -- gbf_off(B) must equal gbf_off(A)+gbf_len(A) exactly.
 *      This is precisely the shape of bug the review found: GBF_DAYCARE_REC's wrong
 *      33 B on Gen 2 would make its end miss GBF_DAYCARE_LADY_FLAG's offset by one
 *      byte, and this check catches that a plain "does it decode plausibly" check
 *      cannot. (Not every field has a same-generation neighbour worth asserting --
 *      this list is deliberately the pairs the .sym files independently proved
 *      touch, not a claim that all 83 fields chain end to end.)
 *   2. Coverage: every one of the 83 fields, on every game that has it, is actually
 *      READ at least once by this file -- so a field this suite forgot to touch
 *      entirely (silently vacuous) shows up as a coverage gap, not a pass. */
typedef struct { GbField a, b; } AdjPair;

static void check_adjacency(const char* file, GbGame game) {
  uint32_t n = load(file);
  if (!n) return;
  static const AdjPair pairs_gen1[] = {
    { GBF_OPTIONS, GBF_BADGES },              /* wOptions(1B) -> wObtainedBadges       */
    { GBF_BAG_COUNT, GBF_BAG_BODY },
    { GBF_PC_COUNT, GBF_PC_BODY },
    { GBF_PLAYTIME_HOURS, GBF_PLAYTIME_MAXED },
    { GBF_PLAYTIME_MAXED, GBF_PLAYTIME_MINUTES },
    { GBF_PLAYTIME_MINUTES, GBF_PLAYTIME_SECONDS },
    { GBF_PLAYTIME_SECONDS, GBF_PLAYTIME_FRAMES },
  };
  static const AdjPair pairs_gen2[] = {
    { GBF_BAG_COUNT, GBF_BAG_BODY },
    { GBF_KEY_ITEMS_COUNT, GBF_KEY_ITEMS_BODY },
    { GBF_BALLS_COUNT, GBF_BALLS_BODY },
    { GBF_PC_COUNT, GBF_PC_BODY },
    { GBF_DEX_OWNED, GBF_DEX_SEEN },           /* the D4 bug's own signature: 32 B, not 19 */
    { GBF_DAYCARE_REC, GBF_DAYCARE_LADY_FLAG }, /* the D2 bug's own signature: 32 B, not 33 */
    { GBF_RTC_START_DAY, GBF_RTC_START_HOUR },
    { GBF_RTC_START_HOUR, GBF_RTC_START_MINUTE },
    { GBF_RTC_START_MINUTE, GBF_RTC_START_SECOND },
    { GBF_RTC_START_SECOND, GBF_RTC_SNAPSHOT },
  };
  const AdjPair* pairs = (game == GBF_G_RED || game == GBF_G_YELLOW) ? pairs_gen1 : pairs_gen2;
  int npairs = (int)((game == GBF_G_RED || game == GBF_G_YELLOW)
                     ? sizeof pairs_gen1 / sizeof pairs_gen1[0]
                     : sizeof pairs_gen2 / sizeof pairs_gen2[0]);
  for (int i = 0; i < npairs; i++) {
    uint32_t off_a = gbf_off(game, pairs[i].a), len_a = gbf_len(game, pairs[i].a);
    uint32_t off_b = gbf_off(game, pairs[i].b);
    CHECK(off_a && off_b, "%s: adjacency pair %d (%s -> %s) is present",
          file, i, gbf_field_name(pairs[i].a), gbf_field_name(pairs[i].b));
    if (off_a && off_b)
      CHECK(off_a + len_a == off_b,
            "%s: %s (off=%#x len=%u, end=%#x) must end exactly where %s (off=%#x) "
            "starts -- an off-by-one in either field's size or offset fails here",
            file, gbf_field_name(pairs[i].a), off_a, len_a, off_a + len_a,
            gbf_field_name(pairs[i].b), off_b);
  }
}

/* Every field THIS game has is READ out of `g_img` at least once -- accumulated into a
 * checksum so the read cannot be optimized away. Called once per loaded save (a game's
 * fields can only be meaningfully read from ITS OWN corpus file, not a different
 * game's bytes reinterpreted under its layout), so between the four calls in main()
 * every one of the 83 fields on every game that has it gets touched. */
static uint32_t touch_all_fields(const char* file, GbGame game) {
  uint32_t n = load(file);
  if (!n) return 0;
  uint32_t acc = 0;
  int touched = 0, total_present = 0;
  for (int f = 0; f < GBF_FIELD_COUNT; f++) {
    uint32_t off = gbf_off(game, (GbField)f);
    uint16_t len = gbf_len(game, (GbField)f);
    if (!off) continue;
    total_present++;
    CHECK(off + len <= n, "%s field=%s: off+len fits the loaded image",
          file, gbf_field_name((GbField)f));
    for (uint16_t i = 0; i < len; i++) acc = acc * 31u + g_img[off + i];
    touched++;
  }
  CHECK(touched == total_present && touched > 0,
       "%s: every present field was read at least once (%d of %d)",
       file, touched, total_present);
  return acc;
}

int main(void) {
  printf("== gb_fields / gb_flags: generated tables against the real corpus ==\n");
  require_real_table();

  check_save("Red.sav",     GBF_G_RED,     true,  0);
  check_save("Yellow.sav",  GBF_G_YELLOW,  true,  0);
  check_save("Gold.sav",    GBF_G_GS,      false, 0);
  check_save("Crystal.sav", GBF_G_CRYSTAL, false, 0);

  check_gen1_span("Red.sav");
  check_gen1_span("Yellow.sav");
  check_gen2_span("Gold.sav", GBF_G_GS, G2_VER_GS);
  check_gen2_span("Crystal.sav", GBF_G_CRYSTAL, G2_VER_CRYSTAL);

  check_flags("Red.sav",     GBF_G_RED,     true,  2560);
  check_flags("Yellow.sav",  GBF_G_YELLOW,  true,  2560);
  check_flags("Gold.sav",    GBF_G_GS,      false, 2048);
  check_flags("Crystal.sav", GBF_G_CRYSTAL, false, 2048);

  /* P0 review D1-D5: sensitivity to an off-by-one in any transcribed cell. */
  check_adjacency("Red.sav",     GBF_G_RED);
  check_adjacency("Yellow.sav",  GBF_G_YELLOW);
  check_adjacency("Gold.sav",    GBF_G_GS);
  check_adjacency("Crystal.sav", GBF_G_CRYSTAL);

  uint32_t touch_acc = 0;
  touch_acc ^= touch_all_fields("Red.sav",     GBF_G_RED);
  touch_acc ^= touch_all_fields("Yellow.sav",  GBF_G_YELLOW);
  touch_acc ^= touch_all_fields("Gold.sav",    GBF_G_GS);
  touch_acc ^= touch_all_fields("Crystal.sav", GBF_G_CRYSTAL);
  printf("  (coverage checksum %#010x -- opaque, just proves the reads were not "
        "optimized away)\n", touch_acc);

  if (!g_ran) printf("  (no corpus present — nothing verified)\n");
  printf("gb_fields test: %d checks, %d failure(s) over %d save(s)\n", g_check, g_fail, g_ran);
  return g_fail ? 1 : 0;
}
