/* source/gb_origin.c -- BACKLOG #266, the static-encounter level floor for CREATE.
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gborigin_test.c source/gb_origin.c \
 *      -o /tmp/hgborigin && /tmp/hgborigin
 *
 * The resolver's answers for the five Gen-1 legendaries plus controls that must NOT
 * move: an ordinary breedable-era species stays at the caller's floor (5), an evolved
 * species keeps its own evolution floor, a higher caller floor is never lowered, Gen 2
 * and out-of-range inputs pass through untouched.
 */
#include <stdio.h>
#include <stdint.h>
#include "gb_origin.h"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

int main(void) {
  /* the five legendaries, from the caller's ordinary floor of 5 */
  CHECK(gb_origin_level_floor(1, 144, 5) == 50, "Articuno -> 50");
  CHECK(gb_origin_level_floor(1, 145, 5) == 50, "Zapdos -> 50");
  CHECK(gb_origin_level_floor(1, 146, 5) == 50, "Moltres -> 50");
  CHECK(gb_origin_level_floor(1, 150, 5) == 70, "Mewtwo -> 70");
  CHECK(gb_origin_level_floor(1, 151, 5) == 5,  "Mew: no in-game placement, stays 5");
  /* controls */
  CHECK(gb_origin_level_floor(1, 1, 5) == 5,    "Bulbasaur stays 5");
  CHECK(gb_origin_level_floor(1, 6, 36) == 36,  "Charizard keeps its evolution floor");
  CHECK(gb_origin_level_floor(1, 144, 60) == 60, "a higher caller floor is never lowered");
  CHECK(gb_origin_level_floor(2, 144, 5) == 5,  "Gen 2 is untouched (Articuno is not static there)");
  CHECK(gb_origin_level_floor(2, 150, 5) == 5,  "Gen 2 Mewtwo untouched");
  CHECK(gb_origin_level_floor(0, 144, 5) == 5,  "gen 0 passes through");
  CHECK(gb_origin_level_floor(1, 0, 5) == 5,    "dex 0 passes through");
  CHECK(gb_origin_static_level(1, 144) == 50 && gb_origin_static_level(1, 25) == 0, "static_level");
  printf("%d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
