/* Host (PC) pin for source/ui_ascii.c -- the pure-C UTF-8 collapse the bounded ui_text()/
 * ui_text_sel() (source/ui.c, BACKLOG #222) run every draw through so libtonc's
 * unbounded tte_get_glyph_id (tonc_tte.h:611, no charCount check) never sees a raw byte
 * >= 0x80. Same walk backs source/ui.c's proportional-font pnext() too (ui.c's pnext()
 * is now a one-line call into ui_ascii_next()), so this test is the single place both
 * paths are pinned.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_ui_ascii_test.c \
 *      source/ui_ascii.c -o /tmp/huat && /tmp/huat
 */
#include <stdio.h>
#include <string.h>
#include "ui_ascii.h"

static int fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fail = 1; } } while (0)

/* 1) ASCII passes through untouched, byte for byte, NUL included. */
static void t_ascii_untouched(void) {
  char out[64];
  size_t n = ui_ascii_bound(out, sizeof out, "Hello, PokeDNA!");
  CHECK(n == strlen("Hello, PokeDNA!"), "ascii n=%zu", n);
  CHECK(strcmp(out, "Hello, PokeDNA!") == 0, "ascii mismatch: \"%s\"", out);
}

/* 2) every 2-byte UTF-8 e-acute sequence (C3 A9, "POKe" -> "POK\xC3\xA9") collapses to
 * EXACTLY one output byte: code 127 (DEL) -- the same cell pnext()/ui_ptext already use,
 * and a cell sys8Font actually has (tonc_tte.h:339, ' '-127 inclusive). */
static void t_eacute_one_byte(void) {
  char out[64];
  size_t n = ui_ascii_bound(out, sizeof out, "POK\xC3\xA9MON");
  CHECK(n == 7, "e-acute output length: got %zu want 7 (\"POK\", 127, \"MON\")", n);
  CHECK(out[0] == 'P' && out[1] == 'O' && out[2] == 'K', "prefix wrong: \"%.3s\"", out);
  CHECK((unsigned char)out[3] == 127u, "e-acute glyph: got %u want 127", (unsigned char)out[3]);
  CHECK(strcmp(out + 4, "MON") == 0, "suffix wrong: \"%s\"", out + 4);
}

/* 3) every OTHER non-ASCII lead byte collapses to exactly one '?', and every UTF-8
 * continuation byte after it is consumed (not walked past, not emitted). Covers the
 * actual #222 trigger: NIDORAN's 3-byte gender signs (E2 99 80 / E2 99 82). */
static void t_gender_signs_one_qmark(void) {
  char out[64];
  /* "NIDORAN" + female sign (E2 99 80) */
  size_t n = ui_ascii_bound(out, sizeof out, "NIDORAN\xE2\x99\x80");
  CHECK(n == 8, "female sign output length: got %zu want 8", n);
  CHECK(strcmp(out, "NIDORAN?") == 0, "female sign: \"%s\"", out);

  /* "NIDORAN" + male sign (E2 99 82) */
  n = ui_ascii_bound(out, sizeof out, "NIDORAN\xE2\x99\x82");
  CHECK(n == 8, "male sign output length: got %zu want 8", n);
  CHECK(strcmp(out, "NIDORAN?") == 0, "male sign: \"%s\"", out);
}

/* 4) a lone/truncated lead byte at the very end of the string (no continuation bytes
 * at all, or fewer than the sequence needs) still collapses to one '?' and does not
 * read past the input's own NUL -- ui_ascii_bound must terminate, not hang or fault. */
static void t_truncated_sequence_safe(void) {
  char out[64];
  size_t n = ui_ascii_bound(out, sizeof out, "AB\xC3");           /* bare lead byte */
  CHECK(n == 3, "bare lead byte: got n=%zu want 3", n);
  CHECK(strcmp(out, "AB?") == 0, "bare lead byte: \"%s\"", out);

  n = ui_ascii_bound(out, sizeof out, "AB\xE2\x99");              /* 3-byte seq missing last byte */
  CHECK(n == 3, "short 3-byte seq: got n=%zu want 3", n);
  CHECK(strcmp(out, "AB?") == 0, "short 3-byte seq: \"%s\"", out);
}

/* 5) exhaustive sweep, every byte 0x00..0xFF as a single-byte "string" (paired with a
 * terminator so the walk always has a defined next byte): ASCII (0x00 as an empty
 * string is skipped since the loop never enters on NUL; 0x01..0x7F) must round-trip
 * unchanged, everything >= 0x80 that is not the exact "\xC3\xA9" pair must become '?'. */
static void t_sweep_0x00_0xff(void) {
  for (unsigned b = 1; b <= 0xFF; b++) {
    char in[2] = { (char)b, '\0' };
    char out[8];
    size_t n = ui_ascii_bound(out, sizeof out, in);
    CHECK(n == 1, "sweep byte 0x%02X: n=%zu want 1", b, n);
    if (b < 0x80u) {
      CHECK((unsigned char)out[0] == b, "sweep ASCII byte 0x%02X: got 0x%02X", b, (unsigned char)out[0]);
    } else {
      CHECK(out[0] == '?', "sweep non-ASCII byte 0x%02X: got 0x%02X want '?'", b, (unsigned char)out[0]);
    }
  }
}

/* 6) truncation at the bound: longer than outcap always leaves a NUL inside the
 * buffer, and the cut never splits a multi-byte source sequence (each output byte
 * corresponds to one whole consumed glyph). */
static void t_truncation_keeps_nul(void) {
  char out[5];  /* room for 4 glyphs + NUL */
  size_t n = ui_ascii_bound(out, sizeof out, "ABCDEFGH");
  CHECK(n == 4, "plain truncation length: got %zu want 4", n);
  CHECK(memcmp(out, "ABCD", 4) == 0 && out[4] == '\0', "plain truncation content: \"%s\"", out);

  /* the same 4-byte cap, but the 4th SOURCE glyph is the 2-byte e-acute pair -- must
   * still land as ONE output byte at out[3], not a half-consumed lead byte. */
  n = ui_ascii_bound(out, sizeof out, "AB\xC3\xA9""CDEF");
  CHECK(n == 4, "mid-sequence truncation length: got %zu want 4", n);
  CHECK(out[0] == 'A' && out[1] == 'B' && (unsigned char)out[2] == 127u && out[3] == 'C' && out[4] == '\0',
        "mid-sequence truncation content: %02x %02x %02x %02x nul=%d",
        (unsigned char)out[0], (unsigned char)out[1], (unsigned char)out[2], (unsigned char)out[3], out[4] == '\0');

  /* outcap == 1: only room for the NUL, zero glyphs. */
  char one[1];
  n = ui_ascii_bound(one, sizeof one, "XYZ");
  CHECK(n == 0 && one[0] == '\0', "outcap=1: n=%zu byte=0x%02x", n, (unsigned char)one[0]);
}

/* 7) NULL guards: out==NULL/outcap==0 is a safe no-op; in==NULL yields an empty
 * NUL-terminated out. */
static void t_null_guards(void) {
  char out[8] = { 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X' };
  size_t n = ui_ascii_bound(NULL, sizeof out, "abc");
  CHECK(n == 0, "out=NULL: n=%zu", n);
  n = ui_ascii_bound(out, 0, "abc");
  CHECK(n == 0, "outcap=0: n=%zu", n);

  n = ui_ascii_bound(out, sizeof out, NULL);
  CHECK(n == 0 && out[0] == '\0', "in=NULL: n=%zu out[0]=0x%02x", n, (unsigned char)out[0]);
}

/* 8) ui_ascii_next() directly, matching the old pnext() contract byte-for-byte (this is
 * what host_uifont_pin_test.c's pnext_host duplicate re-derives independently; here we
 * pin the shared function those two now both call through, via pnext() in source/ui.c). */
static void t_next_matches_old_pnext_contract(void) {
  const char* nf = "NIDORAN\xE2\x99\x80";
  const char* p = nf;
  unsigned last = 0;
  int glyphs = 0;
  while (*p) { last = ui_ascii_next(&p); glyphs++; }
  CHECK(glyphs == 8 && last == (unsigned)'?', "NIDORAN female glyph walk: glyphs=%d last=%u", glyphs, last);
}

/* MUTATION: flip the e-acute lead-byte check from 0xC3 to 0xC2 (a real, easy-to-make
 * off-by-one on the UTF-8 lead byte) and show the e-acute test above would then fail --
 * proves t_eacute_one_byte is actually discriminating, not vacuously true. Run manually:
 *   sed -e 's/c == 0xC3u/c == 0xC2u/' source/ui_ascii.c > /tmp/ui_ascii_mut.c
 *   cc -std=c11 -Wall -Wextra -I source -I /tmp -o /tmp/huat_mut \
 *      tests/host_ui_ascii_test.c /tmp/ui_ascii_mut.c && /tmp/huat_mut
 * (documented here rather than driven from this binary so the real test object list
 * used by tests/run_host_tests.py stays exactly what's declared in the cc line above;
 * the mutation run itself is quoted in the lane report, not shipped as code). */

int main(void) {
  t_ascii_untouched();
  t_eacute_one_byte();
  t_gender_signs_one_qmark();
  t_truncated_sequence_safe();
  t_sweep_0x00_0xff();
  t_truncation_keeps_nul();
  t_null_guards();
  t_next_matches_old_pnext_contract();

  if (fail) { printf("host_ui_ascii_test: FAILED\n"); return 1; }
  printf("host_ui_ascii_test: all checks passed\n");
  return 0;
}
