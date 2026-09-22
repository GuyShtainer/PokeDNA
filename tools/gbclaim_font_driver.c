/* gbclaim_font_driver.c -- the C half of tools/gb_claims.py's claim_gb=
 * matcher (BACKLOG #214 item 2, the section below check()).
 *
 * Renders each requested claim string EXACTLY the way source/pdna_gbscreen.c's
 * gbscr_text()/gbscr_tile_pixels() draws it inside the GB-shell card screens
 * (Hall of Fame, trainer card): one gb_char_encode() glyph per column, each
 * non-blank glyph's 8x8 1bpp bitmap read straight out of the FONT block
 * rom_gbui_open() located in the user's own ROM dump via rom_gbui_glyph().
 * This is the SHIPPED locator/decoder/charmap code, not a re-implementation --
 * same posture tools/gbui_dump.py's own header comment documents ("this script
 * does not locate anything itself -- it only invokes the already-reviewed C
 * locator"). PokeDNA ships no Game Freak art or text; every byte here is a
 * transient read of a ROM file the caller already owns, nothing is embedded.
 *
 * Usage: gbclaim_font_driver <rom-path> <text> [<text> ...]
 * For each <text> argument (matched to gb_char_encode()'s own `gen`, detected
 * from the ROM), prints one line:
 *   OK<TAB><glyph0>,<glyph1>,...
 * where each <glyphN> is either the literal "BLANK" (a space/0x7F glyph --
 * source/pdna_gbscreen.c's GBSCR_BLANK_COLOR, solid background) or 16 lower-
 * case hex digits: the glyph's 8 row bytes (bit 7 = LEFTMOST pixel, 1 = ink --
 * the SAME 1bpp layout rom_gbui_tile()'s own `b = 7u - x` extraction uses).
 * A text this ROM's font/charmap cannot render at all (open failed, or a
 * glyph gb_char_encode() rejects) prints a FAIL line instead; the caller
 * treats that as "claim cannot be checked on this ROM", never a silent match.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rom_gbui.h"
#include "gb_edit.h"

static FILE* g_f;
static bool rd(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  if (fseek(g_f, (long)off, SEEK_SET)) return false;
  return fread(dst, 1, len, g_f) == len;
}
static uint32_t fsize(FILE* f) {
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); return (uint32_t)n;
}

/* Raw 8-row 1bpp bitmap for GB font glyph `tile` (0..127), read directly out
 * of the located font block -- the same bytes rom_gbui_tile() itself reads
 * for bpp==1 before it expands them into DMG_SHADE RGB15 (rom_gbui.c: "hi =
 * lo" for 1bpp, idx = bit ? 3 : 0 -- i.e. the raw bit IS the ink/background
 * decision, so this driver skips the RGB round-trip entirely and hands the
 * bit pattern straight to the Python side). */
static int font_glyph_bits(RomGbUi* gu, uint8_t tile, uint8_t out[8]) {
  if (!gu->ok || !gu->font) return 0;
  uint32_t toff = gu->font + (uint32_t)tile * 8u;
  if (toff < gu->font || toff >= gu->size || 8u > gu->size - toff) return 0;
  return rd(NULL, toff, out, 8) ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <rom> <text> [<text> ...]\n", argv[0]);
    return 2;
  }
  g_f = fopen(argv[1], "rb");
  if (!g_f) { printf("FAIL\topen\n"); return 1; }
  uint32_t sz = fsize(g_f);
  static uint8_t scratch[ROM_GBUI_SCRATCH_MIN];
  RomGbUi gu;
  int ok = rom_gbui_open(&gu, rd, NULL, sz, scratch, sizeof scratch);
  if (!ok || !gu.font) {
    printf("FAIL\tlocate\n");
    fclose(g_f);
    return 1;
  }

  int rc = 0;
  for (int i = 2; i < argc; i++) {
    const char* text = argv[i];
    char line[8192];
    size_t used = 0;
    line[0] = 0;
    int failed = 0;
    while (*text) {
      uint8_t b;
      int consumed = gb_char_encode(gu.gen, text, &b);
      if (consumed <= 0) { failed = 1; break; }
      text += consumed;
      char tok[24];
      if (b == 0x7Fu) {
        snprintf(tok, sizeof tok, "BLANK");
      } else if (b < 0x80u) {
        /* gb_char_encode() only ever emits 0x7F or [0x80,0xFF] (gb_edit.c's
         * own enc_one() contract) -- a byte below 0x80 that isn't the space
         * sentinel would mean this driver's assumption about the charmap is
         * stale, so fail closed rather than silently mis-render it. */
        failed = 1; break;
      } else {
        uint8_t bits[8];
        if (!font_glyph_bits(&gu, (uint8_t)(b - 0x80u), bits)) { failed = 1; break; }
        snprintf(tok, sizeof tok, "%02x%02x%02x%02x%02x%02x%02x%02x",
                  bits[0], bits[1], bits[2], bits[3], bits[4], bits[5], bits[6], bits[7]);
      }
      size_t tlen = strlen(tok);
      if (used + tlen + 2 >= sizeof line) { failed = 1; break; }
      if (used) line[used++] = ',';
      memcpy(line + used, tok, tlen);
      used += tlen;
      line[used] = 0;
    }
    if (failed) {
      printf("FAIL\t%s\n", argv[i]);
      rc = 1;
    } else {
      printf("OK\t%s\n", line);
    }
  }

  fclose(g_f);
  return rc;
}
