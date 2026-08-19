/* Host test: gen3_decode_char / gen3_encode_char round-trip.
 *   cc -std=c11 -I source tests/host_charmap_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/data_tables.c -o /tmp/hcm
 *   /tmp/hcm
 *
 * REGRESSION for a real bug: Guy's own Lugia is nicknamed "28/01/2026" and rendered as
 * "28,01,2026" -- the '/' silently decoded as ','. In the same row the HP numbers
 * "235/235" render a correct slash through the SAME proportional font, so the font has
 * a '/' glyph; only the DECODED NICKNAME showed commas. Root cause (source/gen3_save.c
 * gen3_decode_char, and its mirror source/gen3_edit.c gen3_encode_char): the Gen-3
 * charmap has ','=0xB8 and '/'=0xBA (pokeemerald charmap.txt; independently documented
 * in source/rom_text.c's own charmap() table, which flagged the exact swap in a comment
 * without fixing it here) -- decode had 0xBA mapped to ',' with no case for 0xB8 at all,
 * and encode mirrored the same mistake (plus had no case for '/' at all, silently
 * dropping it to a space). This test round-trips both directions and the exact nickname
 * from the bug report. */
#include <stdio.h>
#include <string.h>
#include "gen3_save.h"     /* gen3_decode_char */
#include "gen3_edit.h"     /* gen3_encode_char */

static int fails = 0, checks = 0;

static void expect_char(const char* what, char got, char want) {
  checks++;
  if (got != want) {
    fails++;
    printf("  FAIL %-46s got='%c' (0x%02x) want='%c' (0x%02x)\n",
           what, got ? got : '_', (unsigned char)got, want, (unsigned char)want);
  } else {
    printf("  ok   %-46s '%c'\n", what, got);
  }
}

static void expect_byte(const char* what, uint8_t got, uint8_t want) {
  checks++;
  if (got != want) {
    fails++;
    printf("  FAIL %-46s got=0x%02x want=0x%02x\n", what, got, want);
  } else {
    printf("  ok   %-46s 0x%02x\n", what, got);
  }
}

int main(void) {
  printf("== gen3 charmap: the specific bug (comma/slash swap) ==\n");
  /* The two bytes that were swapped, checked directly against the real charmap
   * (pokeemerald charmap.txt: ','=0xB8, '/'=0xBA). */
  expect_char("decode(0xB8) == ','", gen3_decode_char(0xB8), ',');
  expect_char("decode(0xBA) == '/'", gen3_decode_char(0xBA), '/');
  expect_byte("encode(',') == 0xB8", gen3_encode_char(','), 0xB8);
  expect_byte("encode('/') == 0xBA", gen3_encode_char('/'), 0xBA);

  printf("\n== round-trip: encode then decode must return the original char ==\n");
  /* Every printable character PokeDNA's Gen-3 charmap tables actually support,
   * round-tripped through BOTH directions -- this is what would have caught the bug:
   * encode(',') used to produce 0xBA (the SLASH byte) and decode(0xBA) used to return
   * ',' -- self-consistently wrong, so a naive round-trip of ',' alone (encode then
   * decode) would have stayed green even with the swap. Testing every character this
   * table claims to support, independently, is what catches a swap rather than a
   * single missing case. */
  static const char CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    " !?.-,/";
  for (size_t i = 0; i + 1 < sizeof CHARS; i++) {
    char c = CHARS[i];
    uint8_t enc = gen3_encode_char(c);
    char dec = gen3_decode_char(enc);
    char what[64];
    snprintf(what, sizeof what, "round-trip '%c' (encode 0x%02x, decode back)", c, enc);
    expect_char(what, dec, c);
  }

  printf("\n== the exact reported nickname: \"28/01/2026\" ==\n");
  {
    const char* nick = "28/01/2026";
    uint8_t raw[16];
    size_t n = strlen(nick);
    for (size_t i = 0; i < n; i++) raw[i] = gen3_encode_char(nick[i]);
    raw[n] = 0xFF;   /* Gen-3 string terminator */
    char decoded[16] = {0};
    size_t k = 0;
    for (; k < n; k++) {
      char ch = gen3_decode_char(raw[k]);
      if (!ch) break;
      decoded[k] = ch;
    }
    decoded[k] = 0;
    checks++;
    if (strcmp(decoded, nick) != 0) {
      fails++;
      printf("  FAIL nickname round-trip: got \"%s\" want \"%s\"\n", decoded, nick);
    } else {
      printf("  ok   nickname round-trip: \"%s\" -> raw bytes -> \"%s\"\n", nick, decoded);
    }
    /* The specific WRONG output the bug produced, so this test fails loudly (not just
     * "not equal") if the swap ever comes back. */
    checks++;
    if (strcmp(decoded, "28,01,2026") == 0) {
      fails++;
      printf("  FAIL nickname decoded as the OLD buggy output \"28,01,2026\" -- the "
             "comma/slash swap is back\n");
    } else {
      printf("  ok   nickname did not regress to the old buggy \"28,01,2026\"\n");
    }
  }

  printf("\n== other bytes near the swap, checked while auditing the whole table ==\n");
  /* Not fixed (reported, not forced -- see the analysis this test's author delivered
   * with these two fixes): 0xAF middle-dot, 0xB0 ellipsis, 0xB1/0xB2 curly double
   * quotes, 0xB5/0xB6 gender symbols, 0xB7 yen sign, 0xB9 multiplication sign all have
   * NO case in gen3_decode_char and fall to the safe '?' default. None of these appear
   * in an ASCII nickname typed on the in-game keyboard (the bug this test guards
   * against), and gen3_decode_char's own '?' default is exactly the safe fallback the
   * rest of this codebase already relies on for genuinely unrepresentable bytes -- so
   * this is left as a documented gap, not silently unaudited: every one of these IS
   * checked here to still return '?' (not corrupt data), and any of them decoding to
   * something else would mean the table changed without this comment being updated. */
  expect_char("0xAF (middle dot) still falls to '?'", gen3_decode_char(0xAF), '?');
  expect_char("0xB0 (ellipsis) still falls to '?'",    gen3_decode_char(0xB0), '?');
  expect_char("0xB1 (open curly quote) still '?'",     gen3_decode_char(0xB1), '?');
  expect_char("0xB2 (close curly quote) still '?'",    gen3_decode_char(0xB2), '?');
  expect_char("0xB5 (male symbol) still '?'",          gen3_decode_char(0xB5), '?');
  expect_char("0xB6 (female symbol) still '?'",        gen3_decode_char(0xB6), '?');
  expect_char("0xB7 (yen sign) still '?'",              gen3_decode_char(0xB7), '?');
  expect_char("0xB9 (multiplication sign) still '?'",  gen3_decode_char(0xB9), '?');
  /* The apostrophe fold: decode merges BOTH 0xB3 (open curly quote) and 0xB4 (close
   * curly quote / the real charmap's own straight-apostrophe alias) to a plain '\''
   * for display -- a reasonable simplification since PokeDNA's font has no separate
   * curly-quote glyphs. ENCODE only has one real target, and it is 0xB4 (the charmap's
   * own straight-apostrophe alias), not 0xB3 (which has none) -- checked here since it
   * is the same class of "which of two decode sources is the real encode target" fix
   * as the comma/slash swap, just far lower-impact (single mon nicknames rarely carry
   * an apostrophe at all). */
  expect_char("decode(0xB3) folds to '\\''", gen3_decode_char(0xB3), '\'');
  expect_char("decode(0xB4) folds to '\\''", gen3_decode_char(0xB4), '\'');
  expect_byte("encode('\\'') targets 0xB4 (not 0xB3)", gen3_encode_char('\''), 0xB4);

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
