/* Host test for source/dex_detail_rule.h (BACKLOG #203) -- the pure logic of the Pokedex
 * DETAIL view: the L/R step through the grid's filtered list (wrapping), the status word
 * and the A-cycle. Header-only, so no object list.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_dexdetail_test.c -o /tmp/hdd && /tmp/hdd
 *
 * Also pins the internal-vs-national trap at the call sites: pdna_pick.c's g_list holds
 * INTERNAL species ids (Gen 3: 251..276 are the Hoenn fillers, national 252+ are shifted),
 * so the detail view must convert with pk_national_no() before it keys dex state or GB
 * art, and must NOT convert before pdna_origin_art_front_by_species(). That is a source
 * shape check (the two call sites are grepped below) -- the conversion itself lives in
 * data_tables, not here.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dex_detail_rule.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

static char* slurp(const char* path) {
  FILE* f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char* b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
  size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; fclose(f); return b;
}

int main(void) {
  /* L/R step: every index of an n-entry list, both directions, exact successor/predecessor */
  for (int n = 1; n <= 9; n++)
    for (int i = 0; i < n; i++) {
      CHECK(dex_detail_step(i, n, +1) == (i + 1) % n, "next = (i+1) mod n");
      CHECK(dex_detail_step(i, n, -1) == (i + n - 1) % n, "prev = (i-1) mod n");
      CHECK(dex_detail_step(dex_detail_step(i, n, +1), n, -1) == i, "next then prev is the identity");
    }
  CHECK(dex_detail_step(0, 5, -1) == 4, "L on the first entry wraps to the last");
  CHECK(dex_detail_step(4, 5, +1) == 0, "R on the last entry wraps to the first");
  CHECK(dex_detail_step(2, 5, +1) != 2 && dex_detail_step(2, 5, -1) != 2, "a 5-list moves off the entry");
  CHECK(dex_detail_step(0, 1, +1) == 0 && dex_detail_step(0, 1, -1) == 0, "a 1-list stays put");
  CHECK(dex_detail_step(0, 0, +1) == 0, "empty list -> 0");
  CHECK(dex_detail_step(7, 5, +1) == 0 && dex_detail_step(-1, 5, -1) == 0, "out-of-range index -> 0, never out of 0..n-1");
  CHECK(dex_detail_step(3, 5, 0) == 3, "dir 0 is a no-op");

  /* status word + cycle */
  CHECK(!strcmp(dex_detail_status_word(2), "CAUGHT"), "2 -> CAUGHT");
  CHECK(!strcmp(dex_detail_status_word(1), "SEEN"), "1 -> SEEN");
  CHECK(!strcmp(dex_detail_status_word(0), "--"), "0 -> --");
  CHECK(!strcmp(dex_detail_status_word(9), "--"), "junk reads as unseen");
  CHECK(dex_detail_cycle(0) == 1 && dex_detail_cycle(1) == 2 && dex_detail_cycle(2) == 0, "cycle 0->1->2->0");
  CHECK(dex_detail_cycle(7) == 0 && dex_detail_cycle(-1) == 0, "junk state cycles to 0");
  { int st = 1; for (int k = 0; k < 3; k++) st = dex_detail_cycle(st); CHECK(st == 1, "three A presses return to the start"); }

  /* call-site shape: national conversion at the dex-state / GB-art keys, internal id for the Gen-3 art */
  char* src = slurp("source/pdna_pick.c");
  CHECK(src != NULL, "read source/pdna_pick.c");
  if (src) {
    const char* d = strstr(src, "static bool dex_detail(int* sel_io");
    CHECK(d != NULL, "dex_detail present");
    const char* art = strstr(src, "static void dex_detail_art(");
    CHECK(art && strstr(art, "pdna_origin_art_front_by_species(in, &art)"), "Gen-3 art is keyed by the INTERNAL id");
    CHECK(art && strstr(art, "portrait_by_dex_gen(s_cell_art.detail_gen, (uint16_t)pk_national_no(in), &art)"), "GB art is keyed by the NATIONAL number");
    CHECK(d && strstr(d, "dex_dset((int)pk_national_no(in), dex_detail_cycle(st))"), "the cycle writes the NATIONAL number");
    free(src);
  }
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
