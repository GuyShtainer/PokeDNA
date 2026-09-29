/* source/gb1_base_fallback.c alone (no generated table): the build still links and the
 * table reports ABSENT, so CREATE > FROM SCRATCH falls back to the ROM read (BACKLOG #276).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gb1base_fallback_test.c \
 *      source/gb1_base_tbl.c source/gb1_base_fallback.c -o /tmp/hgb1fb && /tmp/hgb1fb
 */
#include <stdio.h>
#include <string.h>
#include "gb1_base_tbl.h"

int main(void) {
  int fail = 0;
  uint8_t r[GB1_BASE_ROW_LEN]; memset(r, 0xAA, sizeof r);
  GbNewMonSrc s; memset(&s, 0x5A, sizeof s);
  if (gb1_base_table_present()) { printf("  !! FAIL: table present with only the fallback linked\n"); fail++; }
  if (gb1_base_gen_row(150, r) || r[0] != 0xAA) { printf("  !! FAIL: fallback row lookup\n"); fail++; }
  if (gb1_base_table_fill(150, &s) || s.base[0] != 0x5A || s.growth != 0x5A) {
    printf("  !! FAIL: fallback fill must refuse and leave src untouched\n"); fail++;
  }
  printf("%s: fallback reports the table absent\n", fail ? "FAIL" : "PASS");
  return fail ? 1 : 0;
}
