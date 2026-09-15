/* Host test: source/item_map_g2g3.c -- the generated, name-keyed Gen-2<->Gen-3
 * held-item map (BACKLOG #150 S150-8-CORE).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_itemmap_test.c source/item_map_g2g3.c \
 *      source/gb_item_names.c source/data_tables.c -o /tmp/himap && /tmp/himap
 *
 * Pure C, no argv, no ROM/save fixture -- every fact here is either the shipped
 * table's own declared bounds or one of the numbers pinned in the brief (107
 * pairs, 58 no-counterpart Gen-2 items, 25 unused ids). Check (D) re-derives the
 * map from the two shipped name tables at RUNTIME with its OWN normaliser
 * (deliberately NOT shared with tools/gen_item_map.py or source/item_map_g2g3.c)
 * so a hand-edited table entry is caught here even if the Python drift guard
 * were somehow bypassed.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#include "item_map_g2g3.h"
#include "gb_item_names.h"
#include "data_tables.h"

static int g_fail = 0, g_check = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECKF(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* Own re-implementation of decision 3's normaliser + decision 4's two aliases --
 * intentionally NOT shared with tools/gen_item_map.py, so this test re-derives
 * the map from the shipped name tables independently of the Python generator. */
static void normalize_into(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)in; *p && o + 1 < cap; ) {
    if (p[0] == 0xC3u && p[1] == 0xA9u) { /* UTF-8 'e', used by both tables */
      out[o++] = 'E';
      p += 2;
      continue;
    }
    unsigned char c = (unsigned char)toupper(*p);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[o++] = (char)c;
    p += 1;
  }
  out[o] = '\0';
  if (strcmp(out, "ELIXER") == 0) { strncpy(out, "ELIXIR", cap - 1); out[cap - 1] = '\0'; }
  else if (strcmp(out, "MAXELIXER") == 0) { strncpy(out, "MAXELIXIR", cap - 1); out[cap - 1] = '\0'; }
}

/* The 58 Gen-2 item ids that are named but have NO Gen-3 counterpart, verbatim
 * from the brief's own parse of the shipped tables (decision 8 numbers / the
 * "numbers you must reproduce" section). */
static const uint8_t kNoCounterpart58[58] = {
  0x3C, 0x42, 0x43, 0x45, 0x46, 0x47, 0x4A, 0x4B, 0x4E, 0x4F,
  0x50, 0x53, 0x54, 0x55, 0x59, 0x5C, 0x5D, 0x61, 0x63, 0x65,
  0x67, 0x68, 0x6D, 0x72, 0x73, 0x74, 0x80, 0x81, 0x82, 0x86,
  0x96, 0x98, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA4, 0xA5, 0xA6,
  0xA7, 0xA8, 0xAA, 0xAD, 0xAE, 0xAF, 0xB1, 0xB2, 0xB4, 0xB5,
  0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD,
};

static bool in_58(uint8_t id) {
  for (size_t i = 0; i < sizeof(kNoCounterpart58); i++) {
    if (kNoCounterpart58[i] == id) return true;
  }
  return false;
}

static void test_bounds_and_zero(void) {
  CHECK(item_g2_to_g3(0) == 0, "item_g2_to_g3(0) must be 0");
  CHECK(item_g3_to_g2(0) == 0, "item_g3_to_g2(0) must be 0");
  CHECK(item_g2_to_g3(0xBFu) == 0, "0xBF (first Gen-2 TM id, out of ITEM_MAP_G2_MAX) must be 0");
  CHECK(item_g2_to_g3(0xC4u) == 0, "0xC4 must be 0");
  CHECK(item_g2_to_g3(0xF9u) == 0, "0xF9 (last Gen-2 HM id) must be 0");
  CHECK(item_g2_to_g3(0xFFu) == 0, "0xFF (a corrupt-record value) must be 0, not read past the table");
  CHECK(item_g3_to_g2(377u) == 0, "item_g3_to_g2(ITEM_MAP_G3_COUNT) must be 0 (exclusive bound)");
  CHECK(item_g3_to_g2(1000u) == 0, "item_g3_to_g2(1000) must be 0");
  CHECK(item_g3_to_g2(65535u) == 0, "item_g3_to_g2(65535) must be 0, not read past the table");
}

static void test_count(void) {
  unsigned n_g2 = 0, n_g3 = 0;
  for (unsigned id = 1; id <= ITEM_MAP_G2_MAX; id++) {
    if (item_g2_to_g3((uint8_t)id) != 0) n_g2++;
  }
  for (unsigned id = 1; id < ITEM_MAP_G3_COUNT; id++) {
    if (item_g3_to_g2((uint16_t)id) != 0) n_g3++;
  }
  CHECKF(n_g2 == ITEM_MAP_PAIRS, "probing 1..ITEM_MAP_G2_MAX found %u non-zero results, want %u", n_g2, ITEM_MAP_PAIRS);
  CHECKF(n_g3 == ITEM_MAP_PAIRS, "probing 1..ITEM_MAP_G3_COUNT-1 found %u non-zero results, want %u", n_g3, ITEM_MAP_PAIRS);
}

static void test_bijection(void) {
  unsigned checked = 0;
  for (unsigned id = 1; id <= ITEM_MAP_G2_MAX; id++) {
    uint16_t g3 = item_g2_to_g3((uint8_t)id);
    if (g3 == 0) continue;
    uint8_t back = item_g3_to_g2(g3);
    CHECKF(back == id, "item_g3_to_g2(item_g2_to_g3(0x%02X)=%u) = 0x%02X, want 0x%02X", id, g3, back, id);
    checked++;
  }
  CHECKF(checked == ITEM_MAP_PAIRS, "bijection loop only visited %u mapped ids, want %u", checked, ITEM_MAP_PAIRS);
}

static void test_rederivation(void) {
  /* For every mapped id, the shipped Gen-2 name and the shipped Gen-3 name must
   * normalise (this test's OWN normaliser, not the table's) to the same key. */
  unsigned checked = 0;
  for (unsigned id = 1; id <= ITEM_MAP_G2_MAX; id++) {
    uint16_t g3 = item_g2_to_g3((uint8_t)id);
    if (g3 == 0) continue;
    const char* g2_name = gb2_item_name((uint8_t)id);
    const char* g3_name = pk_item_name(g3);
    CHECKF(g2_name != NULL, "mapped Gen-2 id 0x%02X has a NULL name", id);
    CHECKF(g3_name != NULL, "mapped Gen-3 id %u has a NULL name", g3);
    char key2[32], key3[32];
    normalize_into(g2_name ? g2_name : "", key2, sizeof(key2));
    normalize_into(g3_name ? g3_name : "", key3, sizeof(key3));
    CHECKF(strcmp(key2, key3) == 0,
           "0x%02X %s (key %s) mapped to %u %s (key %s) -- normalised keys differ",
           id, g2_name ? g2_name : "?", key2, g3, g3_name ? g3_name : "?", key3);
    checked++;
  }
  CHECKF(checked == ITEM_MAP_PAIRS, "re-derivation loop only visited %u mapped ids, want %u", checked, ITEM_MAP_PAIRS);
}

static void test_no_tmhm(void) {
  for (unsigned id = 1; id <= ITEM_MAP_G2_MAX; id++) {
    uint16_t g3 = item_g2_to_g3((uint8_t)id);
    if (g3 == 0) continue;
    CHECKF(!(g3 >= 289 && g3 <= 346), "mapped Gen-3 id %u (from Gen-2 0x%02X) falls in the TM/HM range [289,346]", g3, id);
    const char* name = pk_item_name(g3);
    bool looks_tmhm = name && strlen(name) == 4 &&
      (name[0] == 'T' || name[0] == 'H') && name[1] == 'M' &&
      isdigit((unsigned char)name[2]) && isdigit((unsigned char)name[3]);
    CHECKF(!looks_tmhm, "mapped Gen-3 name %s (id %u, from Gen-2 0x%02X) looks like a TM/HM label", name ? name : "?", g3, id);
  }
}

static void test_58_and_25(void) {
  unsigned mapped = 0, no_counterpart = 0, unused = 0;
  for (unsigned id = 1; id <= ITEM_MAP_G2_MAX; id++) {
    uint16_t g3 = item_g2_to_g3((uint8_t)id);
    const char* name = gb2_item_name((uint8_t)id);
    if (g3 != 0) {
      mapped++;
      continue;
    }
    if (in_58((uint8_t)id)) {
      CHECKF(name != NULL, "0x%02X is in the no-counterpart-58 list but gb2_item_name is NULL", id);
      no_counterpart++;
    } else {
      CHECKF(name == NULL, "0x%02X is not in the no-counterpart-58 list and not mapped, but gb2_item_name is non-NULL (%s) -- should be a NULL hole", id, name ? name : "?");
      unused++;
    }
  }
  CHECKF(mapped == ITEM_MAP_PAIRS, "mapped=%u, want %u", mapped, ITEM_MAP_PAIRS);
  CHECKF(no_counterpart == 58, "no_counterpart=%u, want 58", no_counterpart);
  CHECKF(unused == 25, "unused=%u, want 25", unused);
  CHECKF(mapped + no_counterpart + unused == 190, "%u + %u + %u != 190", mapped, no_counterpart, unused);
}

static void test_spot_values(void) {
  static const struct { uint8_t g2; uint16_t g3; const char* label; } kSpot[] = {
    { 0xA3, 202, "LIGHT BALL" }, { 0x92, 200, "LEFTOVERS" }, { 0x52, 187, "KING'S ROCK" },
    { 0x70, 195, "EVERSTONE" }, { 0x8B, 44, "BERRY JUICE" }, { 0x03, 179, "BRIGHTPOWDER" },
    { 0x49, 183, "QUICK CLAW" }, { 0x77, 196, "FOCUS BAND" }, { 0x1E, 222, "LUCKY PUNCH" },
    { 0x23, 223, "METAL POWDER" }, { 0x76, 224, "THICK CLUB" }, { 0x69, 225, "STICK" },
    { 0xAC, 218, "UP-GRADE" },
    { 0x41, 36, "ELIXER -> ELIXIR alias" }, { 0x15, 37, "MAX ELIXER -> MAX ELIXIR alias" },
    /* punctuation-only matches (decision 3 is load-bearing) */
    { 0x39, 182, "EXP.SHARE -> EXP. SHARE" }, { 0x44, 265, "S.S.TICKET -> S.S. TICKET" },
    { 0x62, 207, "BLACKBELT -> BLACK BELT" }, { 0x29, 73, "GUARD SPEC. -> GUARD SPEC." },
  };
  for (size_t i = 0; i < sizeof(kSpot) / sizeof(kSpot[0]); i++) {
    uint16_t got = item_g2_to_g3(kSpot[i].g2);
    CHECKF(got == kSpot[i].g3, "0x%02X (%s): item_g2_to_g3 = %u, want %u", kSpot[i].g2, kSpot[i].label, got, kSpot[i].g3);
  }
}

static void test_mail_never_travels(void) {
  /* Gen-2 mail is already refused as a holder (gb_session.c:392
   * gbs_is_mail_item()) -- none of it should ever appear mapped. */
  CHECK(item_g2_to_g3(0x9E) == 0, "0x9E FLOWER MAIL must map to 0");
  for (unsigned id = 0xB5; id <= 0xBD; id++) {
    CHECKF(item_g2_to_g3((uint8_t)id) == 0, "0x%02X (mail) must map to 0", id);
  }
}

int main(void) {
  test_bounds_and_zero();
  printf("  (A) bounds + zero            ok\n");
  test_count();
  printf("  (B) count                    ok\n");
  test_bijection();
  printf("  (C) bijection                ok\n");
  test_rederivation();
  printf("  (D) re-derivation            ok\n");
  test_no_tmhm();
  printf("  (E) no TM/HM                 ok\n");
  test_58_and_25();
  printf("  (F) 58 + 25                  ok\n");
  test_spot_values();
  printf("  (G) spot values              ok\n");
  test_mail_never_travels();
  printf("  (H) mail never travels       ok\n");

  printf("%d checks, %d failed\n", g_check, g_fail);
  if (g_fail) { printf("FAILED\n"); return 1; }
  printf("all host_itemmap_test checks passed\n");
  return 0;
}
