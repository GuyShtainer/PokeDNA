/* Host test (BACKLOG #202 review A4): every string the HoF CARD (source/pdna_gbhof.c's
 * hof_card_* functions) paints with gbscr_text() must survive gb_char_encode() with
 * `lost == false` on the generation it is drawn for -- a lost glyph does not error or
 * crash, it silently becomes the blank tile (gb_edit.c's enc_one(), "no GB glyph -> a
 * space"), which is exactly how the OLD "*" shiny mark showed NOTHING on the card (the
 * bug this review found). gb_text_lossy() (gb_edit.h) is the real function every GB
 * screen's own nickname/name validators already call for this same question -- this
 * test asks it the SAME thing about every fixed/semi-fixed string this screen emits,
 * plus the WHOLE species-name table (data_tables.c), which pdna_gbhof.c is NOT
 * pure-C-testable on its own (tonc/gbscr headers) so its literal format strings are
 * hand-mirrored here rather than imported -- see this file's own WHERE THE NUMBERS
 * COME FROM note below for why that is an accepted gap, not a silent one.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbhof_glyph_test.c \
 *      source/gb_edit.c source/data_tables.c source/gen1_save.c source/gen2_save.c \
 *      source/gen1_write.c source/gen2_write.c source/gb_session.c \
 *      source/item_map_g2g3.c source/item_map_g1g2.c source/gb_item_names.c source/gb_bag.c source/gb_fields.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbglyph && /tmp/hgbglyph
 *
 * WHERE THE NUMBERS COME FROM: the fixed/format strings below are hand-typed to match
 * pdna_gbhof.c's hof_card_* functions' own literals EXACTLY (kHofMenuLbl[]'s four
 * strings, hof_card_paint_list/_detail/_menu's own siprintf() formats at their own
 * worst-case field widths) -- a change to one of those literals in pdna_gbhof.c does
 * NOT automatically fail this test (the same accepted gap host_textfit_test.c's own
 * header documents for strings a pure-C test cannot reach); the species-name loop
 * below IS live (pk_species_name() is the real function, data_tables.c), so that half
 * of the pin cannot silently drift.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_edit.h"
#include "data_tables.h"

static int g_fail = 0, g_check = 0;

/* Checks `s` against `gen`'s own font -- fails loudly (prints the first lost glyph)
 * rather than just counting, so a review reading test output sees exactly what broke. */
static void check(uint8_t gen, const char* label, const char* s) {
  g_check++;
  char first_bad[GB_GLYPH_MAX];
  int n = gb_text_lossy(gen, s, 64, first_bad);
  if (n != 0) {
    g_fail++;
    printf("FAIL %s (gen %d): %d lossy glyph(s) in \"%s\" -- first bad: \"%s\"\n",
           label, gen, n, s, first_bad);
  }
}

int main(void) {
  /* ---- fixed/semi-fixed strings, both generations (hand-mirrored, see header) ---- */
  static const char* const kBothGens[] = {
    "HALL OF FAME", "TEAMS", "No teams yet.", "READ ONLY", "L/R PAGE",
    "HOF MENU", "CLEAR ALL", "SET COUNT", "ADD TEAM", "DELETE TEAM",
    /* hof_card_paint_list's header, worst-case field widths (99 teams is never
     * reachable -- GBH_G1_CAPACITY/200 cap lower -- but pins the format itself) */
    "99 teams (life 200)",
    /* hof_card_list_row's "N: ..." label, worst case (2-digit team number,
     * 6 mons, 3-digit level span) */
    "10: 6mon Lv100-100", "10: --",
    /* hof_card_paint_detail's title + the Gen-2 wins line */
    "TEAM 10", "Wins: 200",
  };
  for (size_t i = 0; i < sizeof kBothGens / sizeof kBothGens[0]; i++) {
    check(GB_GEN1, "fixed string", kBothGens[i]);
    check(GB_GEN2, "fixed string", kBothGens[i]);
  }

  /* A4's own regression pin: the shiny suffix, worst-case (Lv100 + the mark). The
   * OLD "*" mark is deliberately checked here TOO (expect_lossy) so a future revert
   * of the A4 fix fails this test immediately instead of only failing "by eye" on a
   * screenshot. */
  check(GB_GEN2, "shiny suffix (S)", " (S)");
  {
    char first_bad[GB_GLYPH_MAX];
    int n = gb_text_lossy(GB_GEN2, "*", 64, first_bad);
    if (n == 0) {
      g_fail++;
      printf("FAIL regression pin: '*' now has a font tile on Gen 2 -- if a real GB "
             "glyph was added, that is fine, but hof_card_paint_detail()'s own comment "
             "explaining the ' (S)' choice needs updating to match\n");
    }
    g_check++;
  }

  /* ---- the whole species-name table, both generations' own cap (gb_max_species) --
   * the one part of this pin that is genuinely LIVE (pk_species_name is the real
   * function): every species name this card can ever print, both gens. */
  for (uint8_t gen = GB_GEN1; gen <= GB_GEN2; gen++) {
    uint8_t max = gb_max_species(gen);
    for (uint16_t dex = 1; dex <= max; dex++) {
      const char* nm = pk_species_name(dex);
      if (!nm || !nm[0]) continue;
      check(gen, "species name", nm);
    }
  }

  printf("host_gbhof_glyph_test: %d checked, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
