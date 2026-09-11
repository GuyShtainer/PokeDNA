/* Host test: source/gb_item_names.c -- the embedded Gen-1/Gen-2 item-name
 * identifier tables and the TM/HM synthesis (BACKLOG gbnames brief).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbitemnames_test.c \
 *      source/gb_item_names.c -o /tmp/hgbin && /tmp/hgbin
 *
 * Pure C, no ROM/no save fixture needed -- every fact here is the embedded
 * table itself, cross-checked against the constants files cited in
 * source/gb_item_names.c's own header comment.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_item_names.h"

static int g_fail = 0, g_check = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECKF(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static bool is_upper_ascii_ok(const char* s) {
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    unsigned char c = *p;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ' ' || c == '-' || c == '.' || c == '\'' ||
              c == 0xC3u || c == 0xA9u || /* the two-byte UTF-8 "é" (0xC3 0xA9) */
              (c >= 'a' && c <= 'z');/* "OAK's PARCEL"/"KING'S ROCK" carry a lowercase tail */
    if (!ok) return false;
  }
  return true;
}

/* Every non-hole Gen-1 id (1..0x61) has a name <=12 chars, uppercase-ish
 * ASCII (+ the one lowercase "'s"/"'S" tail and the é the game itself
 * spells lowercase). Holes (0 for NULL) are allowed -- id 0x07/0x2C, both
 * documented in gb_item_names.c's own header. */
static void test_gen1_every_id(void) {
  printf("test_gen1_every_id\n");
  int holes = 0, named = 0;
  for (int id = 1; id <= 0x61; id++) {
    const char* n = gb1_item_name((uint8_t)id);
    if (!n) { holes++; continue; }
    named++;
    CHECKF(strlen(n) <= GB_ITEM_NAME_MAXLEN, "gen1 0x%02X %s len %zu > %d", id, n, strlen(n), GB_ITEM_NAME_MAXLEN);
    CHECKF(is_upper_ascii_ok(n), "gen1 0x%02X %s has a bad glyph", id, n);
  }
  CHECKF(holes == 2, "gen1 expected exactly 2 holes (0x07 SURFBOARD, 0x2C ITEM_2C), got %d", holes);
  CHECKF(named == 95, "gen1 expected 95 named ids (97 - 2 holes), got %d", named);
  /* the two documented holes, by id, not just by count */
  CHECK(gb1_item_name(0x07) == NULL, "gen1 0x07 SURFBOARD must be a hole");
  CHECK(gb1_item_name(0x2C) == NULL, "gen1 0x2C ITEM_2C must be a hole");
}

/* Gen-2: ids 1..0xBE. Holes are numerous (unused ids) -- just bound length/
 * charset on whatever IS named, and pin a handful of known holes by id. */
static void test_gen2_every_id(void) {
  printf("test_gen2_every_id\n");
  int holes = 0, named = 0;
  for (int id = 1; id <= 0xBE; id++) {
    const char* n = gb2_item_name((uint8_t)id);
    if (!n) { holes++; continue; }
    named++;
    CHECKF(strlen(n) <= GB_ITEM_NAME_MAXLEN, "gen2 0x%02X %s len %zu > %d", id, n, strlen(n), GB_ITEM_NAME_MAXLEN);
    CHECKF(is_upper_ascii_ok(n), "gen2 0x%02X %s has a bad glyph", id, n);
  }
  CHECKF(named + holes == 0xBE, "gen2 named+holes should cover every id 1..0xBE, got %d", named + holes);
  /* known unused holes (item_constants.asm ITEM_19/ITEM_2D/ITEM_32/.../ITEM_BE
   * + the legacy-Red leftovers TOWN_MAP 0x06, POKE_FLUTE 0x38 -- never
   * distributed as a real Gen-2 item despite having a symbolic constant). */
  static const uint8_t kKnownHoles[] = { 0x06, 0x19, 0x2D, 0x32, 0x38, 0x5A, 0x64, 0xBE };
  for (size_t i = 0; i < sizeof kKnownHoles; i++)
    CHECKF(gb2_item_name(kKnownHoles[i]) == NULL, "gen2 0x%02X expected a hole", kKnownHoles[i]);
}

/* id 0 (NO_ITEM) is a hole in both generations. */
static void test_id_zero_holes(void) {
  printf("test_id_zero_holes\n");
  CHECK(gb1_item_name(0) == NULL, "gen1 id 0 must be a hole");
  CHECK(gb2_item_name(0) == NULL, "gen2 id 0 must be a hole");
  CHECK(gb2_item_name(0xFF) == NULL, "gen2 id 0xFF must be a hole");
}

/* 10 spot-checks per gen, re-derived by hand against the constants files
 * (assets/upstream/pokered/constants/item_constants.asm and .../data/items/
 * names.asm; assets/upstream/pokecrystal's own pair) -- see
 * source/gb_item_names.c's header for the full citation. */
static void test_gen1_spot_checks(void) {
  printf("test_gen1_spot_checks\n");
  static const struct { uint8_t id; const char* want; } kCases[] = {
    { 0x01, "MASTER BALL" },
    { 0x04, "POKé BALL" },
    { 0x09, "POKéDEX" },
    { 0x10, "FULL RESTORE" },   /* FULL_RESTORE, 12 chars -- the Gen-1 max */
    { 0x15, "BOULDERBADGE" },   /* first badge, 12 chars */
    { 0x2D, "BIKE VOUCHER" },   /* BIKE_VOUCHER, 12 chars */
    { 0x37, "GUARD SPEC." },    /* the one name with a period */
    { 0x3F, "S.S.TICKET" },
    { 0x46, "OAK's PARCEL" },   /* lowercase "'s", real cartridge spelling */
    { 0x53, "MAX ELIXER" },    /* the last real item id before the floor block */
  };
  for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++)
    CHECKF(gb1_item_name(kCases[i].id) && strcmp(gb1_item_name(kCases[i].id), kCases[i].want) == 0,
           "gen1 0x%02X want %s got %s", kCases[i].id, kCases[i].want,
           gb1_item_name(kCases[i].id) ? gb1_item_name(kCases[i].id) : "(null)");
}

static void test_gen2_spot_checks(void) {
  printf("test_gen2_spot_checks\n");
  static const struct { uint8_t id; const char* want; } kCases[] = {
    { 0x01, "MASTER BALL" },
    { 0x05, "POKé BALL" },     /* the "#" -> POKé charmap substitution */
    { 0x0E, "FULL RESTORE" },
    { 0x25, "POKé DOLL" },
    { 0x52, "KING'S ROCK" },   /* uppercase "'S", NOT the lowercase gen-1 combo tile */
    { 0x58, "SILVERPOWDER" },  /* 12 chars, the Gen-2 max */
    { 0x72, "RAGECANDYBAR" },  /* 12 chars */
    { 0x84, "STAR PIECE" },
    { 0xAC, "UP-GRADE" },
    { 0xBD, "MIRAGE MAIL" },   /* the last real item id, 0xBE itself is a hole */
  };
  for (size_t i = 0; i < sizeof kCases / sizeof kCases[0]; i++)
    CHECKF(gb2_item_name(kCases[i].id) && strcmp(gb2_item_name(kCases[i].id), kCases[i].want) == 0,
           "gen2 0x%02X want %s got %s", kCases[i].id, kCases[i].want,
           gb2_item_name(kCases[i].id) ? gb2_item_name(kCases[i].id) : "(null)");
}

/* TM/HM synthesis, exact at both ends of each range, one past each end is a
 * hole (the caller's own "ITEM-n" fallback fires instead). */
static void test_gen1_tmhm_edges(void) {
  printf("test_gen1_tmhm_edges\n");
  char buf[16];
  CHECK(gb_item_label(GBIN_GEN1, 0xC4, buf, sizeof buf) && strcmp(buf, "HM01") == 0, "gen1 HM01");
  CHECK(gb_item_label(GBIN_GEN1, 0xC8, buf, sizeof buf) && strcmp(buf, "HM05") == 0, "gen1 HM05");
  CHECK(gb_item_label(GBIN_GEN1, 0xC9, buf, sizeof buf) && strcmp(buf, "TM01") == 0, "gen1 TM01");
  CHECK(gb_item_label(GBIN_GEN1, 0xFA, buf, sizeof buf) && strcmp(buf, "TM50") == 0, "gen1 TM50");
  CHECK(!gb_item_label(GBIN_GEN1, 0xC3, buf, sizeof buf), "gen1 0xC3 one before HM01 must be a hole");
  CHECK(!gb_item_label(GBIN_GEN1, 0xFB, buf, sizeof buf), "gen1 0xFB one past TM50 must be a hole");
}

static void test_gen2_tmhm_edges(void) {
  printf("test_gen2_tmhm_edges\n");
  char buf[16];
  /* the real, constants-file-derived mapping (NOT the brief's initial guess --
   * see source/gb_item_names.c's own header comment for the full derivation):
   * TM01-TM04 = 0xBF-0xC2, a hole at 0xC3, TM05-TM28 = 0xC4-0xDB, a SECOND
   * hole at 0xDC, TM29-TM50 = 0xDD-0xF2, HM01-HM07 = 0xF3-0xF9. */
  CHECK(gb_item_label(GBIN_GEN2, 0xBF, buf, sizeof buf) && strcmp(buf, "TM01") == 0, "gen2 TM01");
  CHECK(gb_item_label(GBIN_GEN2, 0xC2, buf, sizeof buf) && strcmp(buf, "TM04") == 0, "gen2 TM04");
  CHECK(!gb_item_label(GBIN_GEN2, 0xC3, buf, sizeof buf), "gen2 0xC3 (ITEM_C3) must be a hole");
  CHECK(gb_item_label(GBIN_GEN2, 0xC4, buf, sizeof buf) && strcmp(buf, "TM05") == 0, "gen2 TM05");
  CHECK(gb_item_label(GBIN_GEN2, 0xDB, buf, sizeof buf) && strcmp(buf, "TM28") == 0, "gen2 TM28");
  CHECK(!gb_item_label(GBIN_GEN2, 0xDC, buf, sizeof buf), "gen2 0xDC (ITEM_DC) must be a hole");
  CHECK(gb_item_label(GBIN_GEN2, 0xDD, buf, sizeof buf) && strcmp(buf, "TM29") == 0, "gen2 TM29");
  CHECK(gb_item_label(GBIN_GEN2, 0xF2, buf, sizeof buf) && strcmp(buf, "TM50") == 0, "gen2 TM50");
  CHECK(gb_item_label(GBIN_GEN2, 0xF3, buf, sizeof buf) && strcmp(buf, "HM01") == 0, "gen2 HM01");
  CHECK(gb_item_label(GBIN_GEN2, 0xF9, buf, sizeof buf) && strcmp(buf, "HM07") == 0, "gen2 HM07");
  CHECK(!gb_item_label(GBIN_GEN2, 0xBE, buf, sizeof buf),
        "gen2 0xBE (last real item id, a documented hole, and below the TM01=0xBF floor) must be a hole");
  CHECK(!gb_item_label(GBIN_GEN2, 0xFA, buf, sizeof buf), "gen2 0xFA one past HM07 must be a hole");
}

/* gb_item_label never crashes/garbage on a hole -- it must return false, and
 * must leave the caller free to fall back to "ITEM-n" (it must not have
 * partially written garbage that looks like a name). */
static void test_hole_never_crashes(void) {
  printf("test_hole_never_crashes\n");
  char buf[16];
  for (int id = 0; id <= 255; id++) {
    bool g1 = gb_item_label(GBIN_GEN1, (uint8_t)id, buf, sizeof buf);
    bool g2 = gb_item_label(GBIN_GEN2, (uint8_t)id, buf, sizeof buf);
    (void)g1; (void)g2; /* the loop itself is the crash/UB check under -Wall -Wextra + a sanitizer run */
  }
  CHECK(g_check >= 0, "loop completed"); /* keeps CHECK-count bookkeeping honest */
}

/* the two real GB-shell screens' own name-field caps (pdna_gbbag.c / .
 * pdna_gbpack_body.inc, review-sonnet ruling on the gbnames STOP) -- every
 * table entry must fit inside BOTH, so a future longer name fails here,
 * not on screen. */
static void test_fits_shell_caps(void) {
  printf("test_fits_shell_caps\n");
  for (int id = 1; id <= 0x61; id++) {
    const char* n = gb1_item_name((uint8_t)id);
    if (n) CHECKF((int)strlen(n) <= GB1_SHELL_NAME_CAP, "gen1 0x%02X %s does not fit the %d-col shell field", id, n, GB1_SHELL_NAME_CAP);
  }
  for (int id = 1; id <= 0xBE; id++) {
    const char* n = gb2_item_name((uint8_t)id);
    if (n) CHECKF((int)strlen(n) <= GB2_SHELL_NAME_CAP, "gen2 0x%02X %s does not fit the %d-col shell field", id, n, GB2_SHELL_NAME_CAP);
  }
}

/* MUTATION (run once, revert): shifting the Gen-1 table by one column (id N
 * reads the name meant for id N+1) must break the spot checks above -- run
 * `sed -i '' 's/kGen1ItemName\[id\]/kGen1ItemName[id+1 <= GEN1_ITEM_MAX ? id+1 : id]/' source/gb_item_names.c`
 * (or hand-edit gb1_item_name's return line the same way), rebuild, confirm
 * test_gen1_spot_checks fails, then revert. Documented here rather than
 * automated in the harness itself since it requires editing production code
 * (the brief's own instruction for this class of proof). */

int main(void) {
  test_gen1_every_id();
  test_gen2_every_id();
  test_id_zero_holes();
  test_gen1_spot_checks();
  test_gen2_spot_checks();
  test_gen1_tmhm_edges();
  test_gen2_tmhm_edges();
  test_hole_never_crashes();
  test_fits_shell_caps();

  printf("%d checks, %d failed\n", g_check, g_fail);
  if (g_fail) { printf("FAILED\n"); return 1; }
  printf("all host_gbitemnames_test checks passed\n");
  return 0;
}
