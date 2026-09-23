#include "item_map_g1g2.h"
#include "gb_item_names.h"   /* gb1_item_name / gb2_item_name */
#include "gb_bag.h"          /* gbb_is_g1_key_item */
#include <string.h>

/* Pure C, no tonc/GBA headers (hard rule 5) -- dual-compiles into
 * tests/host_itemmap_g1g2_test.c. Loop bound is a fixed 255 (golden rule 2: every
 * loop provably terminates), run at most once per Bank withdraw, never a hot path. */
uint8_t item_g2_to_g1(uint8_t g2_item) {
  if (g2_item == 0) return 0;
  const char* g2name = gb2_item_name(g2_item);
  if (!g2name || g2name[0] == '\0') return 0;

  uint8_t g1_match = 0;
  int g1_count = 0;
  for (int g1id = 1; g1id <= 255; g1id++) {
    if (gbb_is_g1_key_item((uint8_t)g1id)) continue;
    const char* g1name = gb1_item_name((uint8_t)g1id);
    if (!g1name || g1name[0] == '\0') continue;
    if (strcmp(g1name, g2name) == 0) {
      g1_match = (uint8_t)g1id;
      g1_count++;
    }
  }
  if (g1_count != 1) return 0;   /* no match, or ambiguous (matches >1 Gen-1 id) */

  /* "or vice versa": the one Gen-1 candidate's name must not ALSO belong to more than
   * one Gen-2 id -- a name shared by two Gen-2 items is not a clean 1:1 mapping either,
   * even though each of those two would individually resolve to the same g1_match. */
  int g2_count = 0;
  for (int g2id = 1; g2id <= 255; g2id++) {
    const char* n = gb2_item_name((uint8_t)g2id);
    if (n && n[0] != '\0' && strcmp(n, g2name) == 0) g2_count++;
  }
  if (g2_count != 1) return 0;

  return g1_match;
}
