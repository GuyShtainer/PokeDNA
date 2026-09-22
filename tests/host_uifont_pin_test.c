/* Host (PC) pin test for BACKLOG #218's fix: source/pdna_summary.c's Spec./OT rows
 * on the summary's POKEMON INFO card switched from the raw tte_write (UTF-8, no
 * glyph bound -- an out-of-bounds font-cell read on a 3-byte gender sign, proven on
 * a delta frame: "Spec. NIDORAN" + a garbage cell, docs/shots/b216_218_) to
 * ui_ptext_fit against the app's OWN bounded font (source/ui.c's pnext()/ui_font.c),
 * with maxw=88 (the same 88 px hit-box reg() already registers for that field).
 *
 * ui_ptext_fit itself is GBA-only (writes to vid_mem), so this test re-derives the
 * SAME width walk pnext()/PADV() do (ui_font.c:110 ui_font_w[], source/ui.c:307-332's
 * documented rule -- ASCII 32..127 passes through, the two-byte 0xC3 0xA9 "e-acute"
 * sequence maps to code 127, anything else non-ASCII collapses to '?') over every
 * species name in the generated table (source/data_tables.c) and asserts the
 * widest one still fits in 88 px WITHOUT ui_ptext_fit's '~' truncation marker --
 * the fix must not spuriously clip an ordinary all-ASCII name that used to render
 * with unbounded ui_text. NIDORAN's own 3-byte gender sign (the bug's actual
 * trigger) is checked separately: pnext() turns it into exactly one '?' glyph, and
 * that shorter (not longer) string is even less likely to overflow than the ASCII
 * names, so its width is checked too but is not the widest-name candidate.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_uifont_pin_test.c \
 *      source/ui_font.c source/data_tables.c -o /tmp/huifp && /tmp/huifp
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ui_font.h"
#include "data_tables.h"

#define PADV(c) ((int)ui_font_w[(unsigned)(c) - 32])

/* Same byte walk as source/ui.c's static pnext() (duplicated here, not linked,
 * because pnext() is file-static in ui.c and ui.c itself is GBA-only). */
static unsigned pnext_host(const char** ps) {
  const unsigned char* p = (const unsigned char*)*ps;
  unsigned c = *p++;
  if (c == 0xC3u && *p == 0xA9u) { c = 127u; p++; }
  else if (c >= 0x80u) {
    while ((*p & 0xC0u) == 0x80u) p++;
    c = (unsigned)'?';
  }
  *ps = (const char*)p;
  return c;
}

static int ptext_w_host(const char* s) {
  int w = 0;
  while (*s) w += PADV(pnext_host(&s));
  return w;
}

int main(void) {
  int fail = 0;

  /* 1) widest species name in the generated table must fit the 88 px field
   * width with NO truncation -- the fix must not clip an ordinary ASCII name
   * that used to render unbounded. */
  int widest_w = 0; const char* widest_name = "";
  for (uint16_t sp = 1; sp < 412; sp++) {
    const char* name = pk_species_name(sp);
    if (!name || !name[0] || strcmp(name, "??????????") == 0) continue;
    int w = ptext_w_host(name);
    if (w > widest_w) { widest_w = w; widest_name = name; }
  }
  printf("widest species name: \"%s\" = %d px (field is 88 px)\n", widest_name, widest_w);
  if (widest_w > 88) {
    printf("FAIL: widest species name overflows the 88 px Spec./OT field -- "
           "ui_ptext_fit would silently '~'-truncate a name that used to render "
           "in full under the old unbounded ui_text\n");
    fail = 1;
  }

  /* 2) the actual BACKLOG #218 trigger: NIDORAN's 3-byte gender sign collapses to
   * exactly one '?' glyph (pnext_host's own non-ASCII branch), not two bytes of
   * garbage and not a truncation -- and the resulting string is well under 88 px. */
  const char* nf = pk_species_name(29);   /* NIDORAN female, data_tables.c index 29 */
  if (strcmp(nf, "NIDORAN\xE2\x99\x80") != 0) {
    printf("FAIL: species 29 is not NIDORAN\xE2\x99\x80 any more (table drifted) -- got \"%s\"\n", nf);
    fail = 1;
  } else {
    const char* p = nf;
    unsigned last = 0;
    int glyphs = 0;
    while (*p) { last = pnext_host(&p); glyphs++; }
    /* "NIDORAN" (7 ASCII glyphs) + 1 glyph for the 3-byte sign = 8 glyphs total,
     * and the LAST one must be the safe '?' fallback, not raw UTF-8 continuation
     * bytes walked past the end. */
    if (glyphs != 8 || last != (unsigned)'?') {
      printf("FAIL: NIDORAN\xE2\x99\x80 pnext walk = %d glyphs, last=%u (want 8 glyphs, last='?')\n",
             glyphs, last);
      fail = 1;
    }
    int w = ptext_w_host(nf);
    printf("NIDORAN(female) width = %d px (field is 88 px)\n", w);
    if (w > 88) {
      printf("FAIL: NIDORAN\xE2\x99\x80 (as '?') overflows the 88 px field\n");
      fail = 1;
    }
  }

  if (fail) { printf("host_uifont_pin_test: FAILED\n"); return 1; }
  printf("host_uifont_pin_test: all checks passed\n");
  return 0;
}
