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
 * prev/next are symmetric at every glyph boundary of a mixed string. Each
 * comparison is its own CHECK (with the byte offsets in its label) rather than
 * one aggregate pass/fail, so a future regression names exactly which boundary
 * broke instead of just "symmetry failed somewhere". */
static void test_symmetry(void) {
  printf("\n== prev/next symmetry over a mixed ASCII/UTF-8/escape string ==\n");
  const char s[] = "P\xC3\xA9K{5D}\xE2\x99\x80X";  /* P, e-acute, K, {5D}, female sign, X */
  int n = (int)strlen(s);
  int bounds[16], nb = 0;
  for (int i = 0; i <= n; ) { bounds[nb++] = i; if (i >= n) break; i = u8w_next(s, i); }
  expect_int("6 glyphs in the mixed string", nb - 1, 6);

  for (int k = 1; k < nb; k++) {
    char what[64];
    snprintf(what, sizeof what, "u8w_prev(%d)==%d (glyph %d start)", bounds[k], bounds[k - 1], k - 1);
    expect_int(what, u8w_prev(s, bounds[k]), bounds[k - 1]);
  }
  for (int k = 0; k < nb - 1; k++) {
    char what[64];
    snprintf(what, sizeof what, "u8w_next(%d)==%d (glyph %d end)", bounds[k], bounds[k + 1], k);
    expect_int(what, u8w_next(s, bounds[k]), bounds[k + 1]);
  }
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

/* True if byte offset `p` is a boundary a forward walk from 0 would visit. */
static int on_boundary(const char* s, int p) {
  int i = 0;
  int guard = (int)strlen(s) + 2;
  while (guard-- > 0) {
    if (i == p) return 1;
    if (!s[i]) return 0;
    i = u8w_next(s, i);
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * u8w_prev / u8w_delete_before on MALFORMED input: reviewer-supplied repros.
 * Neither "{5AD}" (three characters between braces, not the two-hex escape
 * gb_name_decode ever emits) nor a bare 0xC3 lead byte followed by a stray
 * continuation byte (never produced by gen3_decode_char, gb_name_decode, or
 * FatFs's UTF-8 LFN decode -- and the OSK's own key grid has no '{', '}', or
 * non-ASCII key to type one by hand) can occur from any real producer in this
 * tree, so this pins the DOCUMENTED behaviour (in bounds, no crash, forward
 * progress, but NOT necessarily caret-on-boundary of the shortened string --
 * see utf8_walk.h) rather than a stronger invariant nothing here can promise. */
static void test_malformed_prev(void) {
  printf("\n== u8w_prev/u8w_delete_before on malformed input (documented, not crash) ==\n");

  /* "{5AD}" is NOT a real escape (3 chars between the braces) -- is_escape_at
   * requires exactly 2 -- so it walks as 5 separate ASCII glyphs. Deleting the
   * 'A' (byte 2) leaves "{5D}", which NOW parses as one real escape glyph, so
   * the caret this hands back (byte 2) lands mid-glyph of the new string. */
  char a[8] = "{5AD}";
  int nl_a, start_a = u8w_delete_before(a, 5, 3, &nl_a);
  expect_true("delete-before succeeds (in range, no refusal)", start_a >= 0);
  expect_int("caret lands at byte 2 (where 'A' was)", start_a, 2);
  expect_int("new length is 4", nl_a, 4);
  expect_str("bytes become the real escape \"{5D}\"", a, "{5D}");
  expect_true("DOCUMENTED gap: caret 2 is NOT a boundary of \"{5D}\" (escape now spans 0..4)",
              !on_boundary(a, start_a));

  /* 0xC3 (an orphaned 2-byte UTF-8 lead, no valid continuation follows it) then
   * 'A' then 0xA9 (a stray continuation byte on its own). Deleting the 'A'
   * (byte 1) leaves { 0xC3, 0xA9 }, which is a well-formed e-acute, so the
   * caret (byte 1) lands mid-glyph of the new string. */
  char b[8]; memcpy(b, "\xC3\x41\xA9", 4);   /* incl. the NUL memcpy's 4th byte copies */
  int nl_b, start_b = u8w_delete_before(b, 3, 2, &nl_b);
  expect_true("delete-before succeeds (in range, no refusal)", start_b >= 0);
  expect_int("caret lands at byte 1 (where 'A' was)", start_b, 1);
  expect_int("new length is 2", nl_b, 2);
  expect_true("bytes become the well-formed e-acute C3 A9",
              (unsigned char)b[0] == 0xC3 && (unsigned char)b[1] == 0xA9 && b[2] == 0);
  expect_true("DOCUMENTED gap: caret 1 is NOT a boundary of the merged e-acute",
              !on_boundary(b, start_b));

  /* Whatever u8w_prev/u8w_delete_before return here, they must still be IN
   * BOUNDS and terminate -- the one guarantee that DOES hold unconditionally. */
  expect_true("start_a in [0, nl_a]", start_a >= 0 && start_a <= nl_a);
  expect_true("start_b in [0, nl_b]", start_b >= 0 && start_b <= nl_b);
}

/* ---------------------------------------------------------------------------
 * u8w_insert_byte with cap != sizeof(buf) -- osk.c's own calling convention
 * after the review fix (osk_core passes `icap`, the tighter of buf's real
 * storage and the caller's `cap`, which is very often smaller than buf's
 * actual stack allocation). */
static void test_insert_cap_mismatch(void) {
  printf("\n== u8w_insert_byte / u8w_copy_capped with cap < sizeof(buf) (osk.c's own case) ==\n");

  /* buf has room for 16 bytes, but the caller's field only allows a cap of 6
   * (e.g. an 8-byte OT-name-style field passed down as `icap`): insertion must
   * stop at cap-1 = 5 content bytes even though buf itself has far more room. */
  char buf[16]; memset(buf, 0xAA, sizeof buf); buf[0] = 0;
  int cap = 6, len = 0;
  for (int i = 0; i < 10; i++) {
    int nl = u8w_insert_byte(buf, len, cap, len, (char)('A' + i));
    if (nl < 0) break;
    len = nl;
  }
  expect_int("stopped at cap-1 bytes despite buf holding 16", len, cap - 1);
  expect_int("strlen agrees", (int)strlen(buf), cap - 1);
  expect_true("bytes past the cap in buf's own storage are untouched",
              (unsigned char)buf[cap] == 0xAA);

  /* A multi-byte glyph that lands EXACTLY at cap-1 must be KEPT WHOLE by
   * u8w_copy_capped, not dropped the way a glyph that overruns the cap is
   * (test_copy_capped covers the overrun case; this is the exact-fit case). */
  const char src[] = "AB\xE2\x99\x82";              /* "AB" + male sign, 5 bytes total */
  char out[8];
  int n = u8w_copy_capped(out, (int)sizeof(src), src);   /* cap-1 == strlen(src) exactly */
  expect_str("glyph landing exactly at cap-1 is kept whole", out, src);
  expect_int("all 5 bytes copied", n, 5);
}

/* ---------------------------------------------------------------------------
 * BACKLOG #15 (Guy 2026-07-27: "truncates it and the rest is lost, instead of
 * letting me edit from the end"): the rename OSK must seed the FULL current
 * name (up to NAME_MAX-1 = 63 bytes, pdna_main.c's BrowseEntry.name / the file
 * browser's own cap -- OSK_MAXLEN mirrors it exactly), start the caret at the
 * very END of that seeded text, and let BACKSPACE remove from the end -- not
 * silently drop everything past some short byte count the way the pre-fix
 * OSK_MAXLEN==16 build did (a plain 23-char save filename like
 * "POKEMON_EMER_BPEE00.sav" already exceeded that). This mirrors exactly what
 * osk_core() does at seed time (`len = u8w_copy_capped(buf, icap, initial);
 * cpos = len;`) using the same two functions, so it is not a second copy of
 * osk_core's own logic. */
static void test_seed_full_and_caret_at_end(void) {
  printf("\n== BACKLOG #15: seeding a long name keeps every glyph, caret at the end ==\n");
  const int OSK_BUF_CAP = 64;    /* sizeof osk_core's `buf` == OSK_MAXLEN(63) + 1 */

  /* A 40-char name, comfortably past the OLD 16-byte cap this backlog item was
   * filed against, comfortably under the current 63-byte one. */
  const char* name40 = "POKEMON_EMERALD_BACKUP_2026_09_07_v2.sav";
  int n40 = (int)strlen(name40);
  expect_int("fixture is exactly 40 characters", n40, 40);

  char buf[64];
  int len = u8w_copy_capped(buf, OSK_BUF_CAP, name40);
  expect_int("all 40 bytes survive the seed (no truncation)", len, 40);
  expect_str("seeded buffer is byte-identical to the source name", buf, name40);
  expect_int("all 40 glyphs present (ASCII: glyph count == byte count)", u8w_count(buf), 40);

  /* osk_core's own next line: `cpos = len;` -- caret starts at the END of the
   * seeded text, not at 0 (which would make BACKSPACE a no-op / L a no-op and
   * every keypress edit the FRONT of the name instead of the end Guy typed). */
  int cpos = len;
  expect_int("caret starts at the end of the seeded text", cpos, n40);
  expect_true("caret is at a glyph boundary right after seeding", on_boundary(buf, cpos));

  /* One B (backspace) from there removes the LAST glyph -- '.sav's 'v' here --
   * proving edits land at the END, matching "letting me edit from the end". */
  int nl, start = u8w_delete_before(buf, len, cpos, &nl);
  expect_true("backspace-at-caret succeeds", start >= 0);
  expect_int("backspace removed exactly one byte (ASCII glyph)", nl, len - 1);
  expect_str("backspace dropped the trailing 'v', not the leading 'P'",
             buf, "POKEMON_EMERALD_BACKUP_2026_09_07_v2.sa");

  /* Committing an UNTOUCHED 40-char seed (no edits at all) must round-trip
   * byte-for-byte -- what f_rename actually receives when the user opens
   * RENAME and immediately presses START without changing anything. */
  char buf2[64];
  int len2 = u8w_copy_capped(buf2, OSK_BUF_CAP, name40);
  char out[64];
  u8w_copy_capped(out, (int)sizeof out, buf2);
  expect_str("untouched 40-char rename commits identical to the original name", out, name40);
  (void)len2;

  /* Exactly at the cap boundary: a 63-char name (OSK_MAXLEN, one under
   * NAME_MAX==64) must seed WHOLE, not lose its last character. */
  char name63[64];
  for (int i = 0; i < 63; i++) name63[i] = (char)('A' + (i % 26));
  name63[63] = 0;
  char bufcap[64];
  int lencap = u8w_copy_capped(bufcap, OSK_BUF_CAP, name63);
  expect_int("a 63-char name (the hard cap) seeds in full", lencap, 63);
  expect_str("63-char seed is byte-identical", bufcap, name63);
  expect_int("caret-at-end still equals the full 63", lencap, (int)strlen(bufcap));

  /* Past the cap: a FAT LFN longer than the browser's own NAME_MAX (64, i.e. a
   * name FatFs could in principle hand back, up to FF_MAX_LFN==255) must still
   * seed WHOLE GLYPHS up to the cap, never a corrupt/overrun buffer -- this is
   * the browser's own truncation (pdna_main.c's scan_dir, NAME_MAX-1), which
   * the OSK mirrors exactly (OSK_MAXLEN == NAME_MAX-1) rather than adding a
   * second, different limit of its own. */
  char name80[81];
  for (int i = 0; i < 80; i++) name80[i] = (char)('a' + (i % 26));
  name80[80] = 0;
  char bufover[64];
  int lenover = u8w_copy_capped(bufover, OSK_BUF_CAP, name80);
  expect_int("an 80-char name (past NAME_MAX) still seeds exactly 63 bytes", lenover, 63);
  expect_true("no overrun: seeded bytes are a prefix of the source",
              strncmp(bufover, name80, 63) == 0);
  expect_int("nothing beyond the cap leaks in: strlen matches the seeded length",
             (int)strlen(bufover), 63);
}

/* ---------------------------------------------------------------------------
 * osk_core-level: the pure edit loop (seed / A-insert / B-delete / L-R-caret /
 * commit) via u8w_apply_key + u8w_copy_capped -- the SAME functions osk.c calls,
 * so this is not a second copy of osk.c's dispatch that could drift from it. */
static void test_edit_loop_seed_commit(void) {
  printf("\n== edit loop: seed -> commit with NO edits returns identical bytes ==\n");
  const char* seeds[] = {
    "PIKACHU",                    /* plain ASCII (Gen-3 path)            */
    "caf\xC3\xA9",                /* e-acute                             */
    "NIDORAN\xE2\x99\x82",        /* male sign                           */
    "PIKA{5D}",                   /* hex escape                          */
    "P{8A}",                      /* gb_edit.h's own <PK>-merge example: 'P' next to */
                                   /* the escaped byte that would otherwise read "PK" */
  };
  for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
    char buf[64];
    int len = u8w_copy_capped(buf, (int)sizeof buf, seeds[s]);
    char out[48];
    u8w_copy_capped(out, (int)sizeof out, buf);
    char what[80];
    snprintf(what, sizeof what, "seed \"%s\" survives an untouched commit", seeds[s]);
    expect_str(what, out, seeds[s]);
    (void)len;
  }
}

/* Tiny deterministic LCG so the 1000-step sequence is reproducible run to run
 * (a real failure must be re-diagnosable from the printed step number alone). */
static unsigned rnd_next(unsigned long* state) {
  *state = *state * 1103515245u + 12345u;
  return (unsigned)(*state >> 8);
}

static void test_edit_loop_random(void) {
  printf("\n== edit loop: caret stays on a glyph boundary over 1000 random A/B/L/R ==\n");
  static const char kb[] =
    "1234567890qwertyuiopasdfghjklzxcvbnmQWERTYUIOPASDFGHJKLZXCVBNM -.,'!?";
  unsigned long rs = 0xC0FFEEu;

  int seed_all_ok = 1, len_ok = 1, range_ok = 1, boundary_ok = 1;
  int first_bad_step = -1;
  const int OSK_MAXLEN = 63, cap = 48;

  for (int run = 0; run < 5; run++) {
    /* Build a WELL-FORMED seed the way gb_name_decode actually would (ASCII,
     * e-acute, male/female sign, or a hex escape -- never a bare lead byte or
     * a lone continuation byte), matching what the OSK will really see. */
    char buf[64]; int len = 0;
    while (len < 40) {
      unsigned r = rnd_next(&rs) % 100; char g[4]; int n;
      if (r < 55) { g[0] = kb[rnd_next(&rs) % (sizeof kb - 1)]; n = 1; }
      else if (r < 70) { memcpy(g, "\xC3\xA9", 2); n = 2; }
      else if (r < 85) { memcpy(g, (rnd_next(&rs) & 1) ? "\xE2\x99\x80" : "\xE2\x99\x82", 3); n = 3; }
      else { const char* h = "0123456789ABCDEF";
             g[0] = '{'; g[1] = h[rnd_next(&rs) % 16]; g[2] = h[rnd_next(&rs) % 16]; g[3] = '}'; n = 4; }
      if (len + n > 40) break;
      memcpy(buf + len, g, (size_t)n); len += n;
      if (rnd_next(&rs) % 100 < 15) break;
    }
    buf[len] = 0;
    if (!on_boundary(buf, len)) seed_all_ok = 0;

    int cpos = len;
    for (int step = 0; step < 1000; step++) {
      unsigned op = rnd_next(&rs) % 4;
      char c = kb[rnd_next(&rs) % (sizeof kb - 1)];
      U8wOp ops[4] = { U8W_OP_INSERT, U8W_OP_DELETE, U8W_OP_LEFT, U8W_OP_RIGHT };
      u8w_apply_key(buf, &len, &cpos, OSK_MAXLEN, cap, ops[op], c);
      if ((int)strlen(buf) != len) { len_ok = 0; if (first_bad_step < 0) first_bad_step = step; }
      if (cpos < 0 || cpos > len) { range_ok = 0; if (first_bad_step < 0) first_bad_step = step; }
      if (!on_boundary(buf, cpos)) { boundary_ok = 0; if (first_bad_step < 0) first_bad_step = step; }
    }
  }

  expect_true("every random seed parses as whole glyphs", seed_all_ok);
  expect_true("strlen(buf) tracks `len` after every step", len_ok);
  expect_true("cpos stays in [0, len] after every step", range_ok);
  if (!boundary_ok) {
    char what[96];
    snprintf(what, sizeof what, "caret on a glyph boundary after every step (first break: step %d)", first_bad_step);
    expect_true(what, boundary_ok);
  } else {
    expect_true("caret on a glyph boundary after every one of 5x1000 steps", boundary_ok);
  }
}

int main(void) {
  test_ascii();
  test_utf8();
  test_escape();
  test_symmetry();
  test_edit_ops();
  test_copy_capped();
  test_malformed_prev();
  test_insert_cap_mismatch();
  test_seed_full_and_caret_at_end();
  test_edit_loop_seed_commit();
  test_edit_loop_random();

  printf("\n%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
