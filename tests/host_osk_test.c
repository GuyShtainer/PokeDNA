/* Host test for source/utf8_walk.c — the glyph walker BACKLOG #38 added to osk.c so a
 * gb_name_decode()'d name (e-acute, the Male/Female signs, a "{5D}" hex escape) can be
 * shown, moved over and deleted as whole glyphs instead of raw bytes.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_osk_test.c source/utf8_walk.c \
 *      -o /tmp/hosk && /tmp/hosk
 *
 * osk.c itself is GBA UI (tonc key/render calls) and is not host-compilable, so this
 * exercises the exact pure-C functions osk.c calls for every edit it performs — there is
 * no second copy of the logic that could drift from what actually ships. */
#include <stdio.h>
#include <string.h>

#include "utf8_walk.h"

static int fails = 0, checks = 0;

static void expect_int(const char* what, int got, int want) {
  checks++;
  if (got != want) {
    fails++;
    printf("  FAIL %-58s got=%d want=%d\n", what, got, want);
  } else {
    printf("  ok   %-58s %d\n", what, got);
  }
}

static void expect_str(const char* what, const char* got, const char* want) {
  checks++;
  if (strcmp(got, want) != 0) {
    fails++;
    printf("  FAIL %-58s got=\"%s\" want=\"%s\"\n", what, got, want);
  } else {
    printf("  ok   %-58s \"%s\"\n", what, got);
  }
}

static void expect_true(const char* what, int cond) {
  checks++;
  if (!cond) { fails++; printf("  FAIL %-58s\n", what); }
  else        printf("  ok   %-58s\n", what);
}

/* ---------------------------------------------------------------------------
 * ASCII: byte count == glyph count, one glyph per byte throughout.
 * (BACKLOG #38 says Gen-3 names, which are ASCII through this same OSK path,
 * must be provably unaffected -- this section is that proof.) */
static void test_ascii(void) {
  printf("== ASCII: one glyph per byte (Gen-3 names go through this same path) ==\n");
  const char* s = "PIKACHU";
  expect_int("u8w_count(\"PIKACHU\")", u8w_count(s), 7);
  int i = 0;
  for (int g = 0; g < 7; g++) {
    expect_int("u8w_len_bytes at each ASCII glyph", u8w_len_bytes(s, i), 1);
    i = u8w_next(s, i);
  }
  expect_int("walked exactly strlen bytes", i, (int)strlen(s));
}

/* ---------------------------------------------------------------------------
 * UTF-8: e-acute (2-byte), Male/Female signs (3-byte), mixed with ASCII. */
static void test_utf8(void) {
  printf("\n== UTF-8 glyphs: e-acute (2B) and Male/Female signs (3B) ==\n");
  const char eacute[] = "caf\xC3\xA9";          /* "café" */
  expect_int("u8w_count(\"café\")", u8w_count(eacute), 4);
  int last = u8w_next(eacute, 3);               /* the e-acute glyph starts at byte 3 */
  expect_int("e-acute is 2 bytes", u8w_len_bytes(eacute, 3), 2);
  expect_int("e-acute glyph ends at strlen", last, (int)strlen(eacute));

  const char female[] = "NIDORAN\xE2\x99\x80";  /* "NIDORAN" + U+2640 (E2 99 80) */
  expect_int("u8w_count(\"NIDORAN\\u2640\")", u8w_count(female), 8);
  expect_int("female sign is 3 bytes", u8w_len_bytes(female, 7), 3);

  const char male[] = "NIDORAN\xE2\x99\x82";    /* U+2642 (E2 99 82) */
  expect_int("male sign is 3 bytes", u8w_len_bytes(male, 7), 3);
}

/* ---------------------------------------------------------------------------
 * The "{XX}" hex escape gb_name_decode() emits for an unrepresentable GB byte:
 * one glyph, four ASCII bytes, braces included. */
static void test_escape(void) {
  printf("\n== \"{XX}\" hex escape: one glyph, 4 bytes ==\n");
  const char s[] = "PIKA{5D}";
  expect_int("u8w_count(\"PIKA{5D}\")", u8w_count(s), 5);      /* P I K A {5D} */
  expect_int("{5D} is 4 bytes", u8w_len_bytes(s, 4), 4);
  expect_int("u8w_next lands exactly on '}'+1 (end of string)",
             u8w_next(s, 4), (int)strlen(s));

  /* One B press on "PIKA{5D}" must remove the WHOLE escape, not split it — the
   * regression this backlog item exists to fix. */
  char buf[16]; strcpy(buf, s);
  int nl, start = u8w_delete_before(buf, (int)strlen(buf), (int)strlen(buf), &nl);
  expect_true("delete-before on the escape succeeded", start >= 0);
  buf[nl] = 0;
  expect_str("one B press removes the whole {5D}, leaving \"PIKA\"", buf, "PIKA");

  /* A malformed escape (no closing brace) must not be mistaken for one, and must
   * not hang or run past the NUL. */
  const char bad[] = "AB{5D";
  expect_int("malformed escape: u8w_count makes forward progress", u8w_count(bad), 5);
  int j = u8w_next(bad, 2);
  expect_true("malformed escape: u8w_next still advances", j > 2 && j <= (int)strlen(bad));
}

/* ---------------------------------------------------------------------------
 * prev/next are symmetric at every glyph boundary of a mixed string. */
static void test_symmetry(void) {
  printf("\n== prev/next symmetry over a mixed ASCII/UTF-8/escape string ==\n");
  const char s[] = "P\xC3\xA9K{5D}\xE2\x99\x80X";  /* P, e-acute, K, {5D}, female sign, X */
  int n = (int)strlen(s);
  int bounds[16], nb = 0;
  for (int i = 0; i <= n; ) { bounds[nb++] = i; if (i >= n) break; i = u8w_next(s, i); }
  expect_int("6 glyphs in the mixed string", nb - 1, 6);

  int all_ok = 1;
  for (int k = 1; k < nb; k++) {
    if (u8w_prev(s, bounds[k]) != bounds[k - 1]) all_ok = 0;
  }
  for (int k = 0; k < nb - 1; k++) {
    if (u8w_next(s, bounds[k]) != bounds[k + 1]) all_ok = 0;
  }
  expect_true("u8w_prev(next(i))==i and u8w_next(prev(j))==j at every boundary", all_ok);
}

/* ---------------------------------------------------------------------------
 * Edit-buffer ops osk.c actually calls: insert-at-caret, delete-before-caret. */
static void test_edit_ops(void) {
  printf("\n== edit-buffer ops (the exact functions osk.c calls) ==\n");

  /* Delete-at-end removes exactly one glyph, byte-width matching the glyph. */
  char buf[32] = "NIDORAN\xE2\x99\x82";  /* NIDORAN + male sign (3 bytes) */
  int len = (int)strlen(buf);
  int nl, start = u8w_delete_before(buf, len, len, &nl);
  expect_true("delete-at-end on a 3-byte glyph succeeded", start == 7);
  expect_int("new length dropped by exactly 3 bytes", nl, len - 3);
  buf[nl] = 0;
  expect_str("male sign fully removed, ASCII tail intact", buf, "NIDORAN");

  /* delete-before at pos==0 is refused, not a silent no-op. */
  char e[4] = "AB"; int nl2;
  int r = u8w_delete_before(e, 2, 0, &nl2);
  expect_true("delete-before(pos=0) refused", r < 0);
  expect_int("buffer length unchanged on refusal", nl2, 2);

  /* insert keeps the cap honored in GLYPH units when every glyph is 1 byte (the
   * only case the OSK's own key grid can ever type: it has no key that emits a
   * multi-byte glyph) — filling to cap-1 succeeds, the next insert is refused,
   * and glyph count == byte count == cap-1, so counting in glyphs or bytes agree. */
  char g[6]; g[0] = 0;
  int glen = 0, cap = (int)sizeof(g);
  for (int i = 0; i < cap; i++) {
    int r2 = u8w_insert_byte(g, glen, cap, glen, (char)('A' + i));
    if (r2 < 0) break;
    glen = r2;
  }
  expect_int("filled to exactly cap-1 bytes", glen, cap - 1);
  expect_int("glyph count equals byte count for ASCII fill", u8w_count(g), cap - 1);
  int overflow = u8w_insert_byte(g, glen, cap, glen, 'Z');
  expect_true("one more insert past the cap is refused", overflow < 0);
  expect_int("buffer length unchanged after the refused insert", (int)strlen(g), cap - 1);
}

/* ---------------------------------------------------------------------------
 * u8w_copy_capped: the seed/commit truncation, glyph-boundary-safe. */
static void test_copy_capped(void) {
  printf("\n== u8w_copy_capped: truncates on a glyph boundary, never mid-sequence ==\n");

  /* Untouched round trip: fits entirely, so every byte survives (the "seeding
   * keeps every byte" / "committing an untouched name returns identical bytes"
   * guarantee this backlog item is under). */
  char out[32];
  int n = u8w_copy_capped(out, sizeof out, "PIKA{5D}");
  expect_str("fits entirely: byte-identical copy", out, "PIKA{5D}");
  expect_int("returned length matches strlen", n, 8);

  /* Too small to fit the trailing 3-byte glyph whole: it must be dropped
   * ENTIRELY, never left as a truncated 1- or 2-byte fragment. "NIDORAN" is 7
   * bytes; cap=8 leaves room for the 7 bytes + NUL but not the 3-byte sign. */
  char small[8];
  int n2 = u8w_copy_capped(small, sizeof small, "NIDORAN\xE2\x99\x82");
  expect_str("female sign dropped whole, ASCII kept", small, "NIDORAN");
  expect_int("no orphaned continuation bytes", n2, 7);

  /* A cap that lands exactly one byte into a 4-byte "{XX}" escape must drop the
   * whole escape too, not leave "{5" dangling. */
  char small2[6]; /* "PIKA" (4) + NUL fits; the escape (4 more bytes) does not */
  int n3 = u8w_copy_capped(small2, sizeof small2, "PIKA{5D}");
  expect_str("escape dropped whole when it can't fit", small2, "PIKA");
  expect_int("length stops before the escape", n3, 4);

  /* cap==1: only room for the NUL. */
  char tiny[1];
  int n4 = u8w_copy_capped(tiny, sizeof tiny, "X");
  expect_str("cap==1 yields an empty string", tiny, "");
  expect_int("cap==1 writes zero content bytes", n4, 0);
}

int main(void) {
  test_ascii();
  test_utf8();
  test_escape();
  test_symmetry();
  test_edit_ops();
  test_copy_capped();

  printf("\n%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
