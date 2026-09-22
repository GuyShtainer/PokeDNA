/* Host test: BACKLOG #183 -- the Gen-3 conversion arm's ♂/♀ and punctuation codec.
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gen3_codec_lossy_test.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/gen12_convert.c source/data_tables.c \
 *      -o /tmp/hgcl && /tmp/hgcl
 *
 * Three things this pins:
 *   (1) gen3_decode_char / gen3_encode_char are exact inverses of each other for every
 *       byte the decoder maps to a printable glyph (no decoder glyph without an encoder
 *       inverse), plus the unmapped-code population (190 codes that gen3_decode_char
 *       maps to '?', not matching any encoder case) -- a full 0..254 sweep (255 is the
 *       Gen-3 string terminator, not a glyph), not just the handful host_charmap_test.c
 *       already names.
 *   (2) gen12_convert()'s up-conversion (Gen 1/2 -> Gen 3) now round-trips the gender
 *       signs through the real Gen-3 byte (0xB5/0xB6) instead of silently dropping to
 *       '?', and reports it via Gb12Notes.nick_lossy == false -- a synthesised Gen-1
 *       NIDORAN♀ is the canonical case (review the BACKLOG item's own example).
 *   (3) a glyph Gen 3 genuinely cannot store (the brackets -- gen3_save.c's own comment
 *       says why) raises Gb12Notes.nick_lossy == true instead of silently turning into
 *       a space.
 */
#include <stdio.h>
#include <string.h>
#include "gen3_save.h"      /* gen3_decode_char */
#include "gen3_edit.h"      /* gen3_encode_char */
#include "gen3_mon.h"       /* gen3_decode_name */
#include "gen12_convert.h"  /* gen12_convert, Gb12Mon, Gb12Notes */

static int fails = 0, checks = 0;

static void expect(bool cond, const char* what) {
  checks++;
  if (!cond) { fails++; printf("  FAIL %s\n", what); }
  else       { printf("  ok   %s\n", what); }
}

/* ---- (1) full 0..254 sweep -------------------------------------------------- */
static void test_full_sweep(void) {
  printf("== (1) 256-code decode->encode->decode idempotence (0x00..0xFE) ==\n");
  int tested = 0;
  int unmapped = 0;
  for (int c = 0; c <= 0xFE; c++) {
    char ch1 = gen3_decode_char((uint8_t)c);
    /* Every non-terminator byte decodes to SOME printable glyph -- gen3_decode_char
     * has no other path to 0. A code that broke this would mean a new terminator
     * alias snuck in, which is its own bug. */
    if (ch1 == 0) { fails++; checks++; printf("  FAIL 0x%02x decoded to NUL (not a terminator)\n", c); continue; }
    if (ch1 == '?' && c != 0xAC) { unmapped++; continue; }   /* not an inverse claim */
    uint8_t b2  = gen3_encode_char(ch1);
    char    ch2 = gen3_decode_char(b2);
    checks++; tested++;
    if (ch2 != ch1) {
      fails++;
      printf("  FAIL 0x%02x: decode='%c' -> encode=0x%02x -> decode='%c' (want '%c')\n",
             c, ch1, b2, ch2 ? ch2 : '_', ch1);
    }
  }
  printf("  swept %d codes, %d unmapped\n", tested, unmapped);
  /* BACKLOG #216: 179 -> 174 -- gen3_decode_char gained 5 plain-ASCII cases
   * (0x2E '+', 0x35 '=', 0x5B '%', 0x85 '<', 0x86 '>'; source/gen3_save.c). The
   * lowercase e-acute (0x1B) is NOT one of these five: gen3_decode_char itself
   * stays untouched for it (same reason the gender signs do -- a `char` return
   * cannot carry 2 UTF-8 bytes), so it is still counted "unmapped" by THIS sweep
   * even though source/gen3_mon.c's decode_name now special-cases it, exactly
   * like 0xB5/0xB6 already are (see test_eacute_roundtrip below, which exercises
   * that path directly instead of through gen3_decode_char). */
  expect(unmapped == 174, "the unmapped-code population moved by exactly the 5 "
                          "new plain-ASCII cases (BACKLOG #216)");
}

/* ---- (1b) BACKLOG #216: the 5 new plain-ASCII codes round-trip too -------------- */
static void test_new_ascii_codes(void) {
  printf("\n== (1b) the 5 new plain-ASCII Gen-3 codes round-trip ==\n");
  static const struct { char ch; uint8_t code; } NEW[] = {
    {'+', 0x2E}, {'=', 0x35}, {'%', 0x5B}, {'<', 0x85}, {'>', 0x86},
  };
  for (size_t i = 0; i < sizeof NEW / sizeof NEW[0]; i++) {
    char what[64];
    snprintf(what, sizeof what, "decode(0x%02x) == '%c'", NEW[i].code, NEW[i].ch);
    checks++;
    if (gen3_decode_char(NEW[i].code) != NEW[i].ch) { fails++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
    snprintf(what, sizeof what, "encode('%c') == 0x%02x", NEW[i].ch, NEW[i].code);
    checks++;
    if (gen3_encode_char(NEW[i].ch) != NEW[i].code) { fails++; printf("  FAIL %s\n", what); }
    else printf("  ok   %s\n", what);
  }
}

/* ---- (1c) BACKLOG #216: e-acute round-trips through decode_name/encode_name --- */
static void test_eacute_roundtrip(void) {
  printf("\n== (1c) e-acute (0x1B) round-trips through gen3_decode_name/encode_name ==\n");
  /* decode_name/encode_name are the functions that actually own this special case
   * (same shape as the gender signs) -- gen3_decode_char itself is untouched for
   * 0x1B, so this exercises the real path a nickname takes, not the byte table. */
  Gb12Mon in;
  memset(&in, 0, sizeof in);
  in.gen = 1;
  in.species_dex = 1;      /* Bulbasaur */
  in.exp = 0; in.level = 1;
  in.dv_atk = in.dv_def = in.dv_spd = in.dv_spc = 8;
  in.moves[0] = 1;
  in.ot_id = 1;
  strcpy(in.nickname, "CAF\xC3\xA9");   /* "CAFé" -- e-acute, UTF-8 C3 A9 */
  strcpy(in.ot_name, "GUY");

  Gb12Target tgt = { .met_game = 3 };
  uint8_t rec[80];
  Gb12Notes notes;
  Gb12Result r = gen12_convert(&in, &tgt, rec, &notes);
  expect(r == GB12_OK, "e-acute fixture converts");
  if (r != GB12_OK) return;

  expect(rec[0x0B] == 0x1B, "nickname's 4th glyph (offset 0x0B, after C-A-F) is 0x1B (e-acute)");
  expect(!notes.nick_lossy, "notes.nick_lossy is false -- e-acute round-trips exactly");
}

/* ---- (1d) BACKLOG #216: a Gen-1 <PK> ligature name overflowing the Gen-3 field - */
static void test_ligature_overflow_lossy(void) {
  printf("\n== (1d) a Gen-1 <PK> ligature nickname that overflows the Gen-3 cap is "
         "flagged lossy ==\n");
  /* gen1_decode_name expands 0xE1 (<PK>) to 2 ASCII chars ('P','K') each -- 6 raw
   * GB bytes decode to "PKPKPKPKPKPK" (12 chars), past the Gen-3 nickname field's
   * 10-glyph cap. This is what "note_spelling_loss is blind to the GB-decode hop"
   * (BACKLOG #216) asks to be proven NOT blind to: the comparison already runs at
   * the Gen-3 byte level (`written` is decoded straight from out80), so the
   * truncation shows up as written != intended and gets flagged. */
  Gb12Mon in;
  memset(&in, 0, sizeof in);
  in.gen = 1;
  in.species_dex = 25;      /* Pikachu */
  in.exp = 1000; in.level = 10;
  in.dv_atk = in.dv_def = in.dv_spd = in.dv_spc = 10;
  in.moves[0] = 33;
  in.ot_id = 12345;
  strcpy(in.nickname, "PKPKPKPKPKPK");
  strcpy(in.ot_name, "GUY");

  Gb12Target tgt = { .met_game = 3 };
  uint8_t rec[80];
  Gb12Notes notes;
  Gb12Result r = gen12_convert(&in, &tgt, rec, &notes);
  expect(r == GB12_OK, "ligature-overflow fixture converts");
  if (r != GB12_OK) return;

  char written[32];
  gen3_decode_name(written, sizeof written, rec + 0x08, 10);
  checks++;
  if (strlen(written) != 10) { fails++; printf("  FAIL written nickname is %d chars, want 10 "
                                                "(truncated at the Gen-3 field cap)\n", (int)strlen(written)); }
  else printf("  ok   written nickname truncated to 10 chars: \"%s\"\n", written);
  expect(notes.nick_lossy, "notes.nick_lossy is true -- the ligature expansion overflowed "
                            "the Gen-3 field and got silently truncated");
}

/* ---- shared fixture: a minimal, always-convertible Gb12Mon ------------------ */
static void make_base_mon(Gb12Mon* in) {
  memset(in, 0, sizeof *in);
  in->gen          = 1;
  in->species_dex  = 29;     /* NIDORAN♀ */
  in->exp          = 0;      /* level 1 */
  in->level        = 1;
  in->dv_atk = in->dv_def = in->dv_spd = in->dv_spc = 8;
  in->moves[0]     = 1;      /* Pound -- exists in every generation */
  in->ot_id        = 12345;
  in->slot_salt    = 0;
}

/* ---- (2) NIDORAN female, synthesised Gen-1, lands as 0xB6 -------------------- */
static void test_nidoran_female_lossless(void) {
  printf("\n== (2) synthesised Gen-1 NIDORAN♀ -> Gen-3 byte 0xB6, nick_lossy == false ==\n");
  Gb12Mon in;
  make_base_mon(&in);
  /* Same UTF-8 spelling data_tables.c's own species table uses for the glyph
   * (source/data_tables.c: "NIDORAN\xE2\x99\x80..."). */
  strcpy(in.nickname, "NIDORAN\xE2\x99\x80");
  strcpy(in.ot_name, "GUY");

  Gb12Target tgt = { .met_game = 3 };
  uint8_t rec[80];
  Gb12Notes notes;
  Gb12Result r = gen12_convert(&in, &tgt, rec, &notes);
  expect(r == GB12_OK, "NIDORAN fixture converts");
  if (r != GB12_OK) return;

  expect(rec[0x0F] == 0xB6, "nickname's 8th glyph (mon record offset 0x0F, after N-I-D-O-R-A-N) is 0xB6 (female sign)");
  expect(!notes.nick_lossy, "notes.nick_lossy is false -- the gender sign round-trips");
  expect(!notes.otname_lossy, "notes.otname_lossy is false -- 'GUY' is plain ASCII");
}

/* ---- (2b) male sign too, for symmetry ---------------------------------------- */
static void test_nidoran_male_lossless(void) {
  printf("\n== (2b) male gender sign -> Gen-3 byte 0xB5, nick_lossy == false ==\n");
  Gb12Mon in;
  make_base_mon(&in);
  in.species_dex = 32;  /* NIDORAN♂ */
  strcpy(in.nickname, "NIDORAN\xE2\x99\x82");
  strcpy(in.ot_name, "GUY");

  Gb12Target tgt = { .met_game = 3 };
  uint8_t rec[80];
  Gb12Notes notes;
  Gb12Result r = gen12_convert(&in, &tgt, rec, &notes);
  expect(r == GB12_OK, "NIDORAN male fixture converts");
  if (r != GB12_OK) return;

  expect(rec[0x0F] == 0xB5, "nickname's 8th glyph (offset 0x0F) is 0xB5 (male sign)");
  expect(!notes.nick_lossy, "notes.nick_lossy is false");
}

/* ---- (3) a bracketed nickname: Gen 3 genuinely cannot store '[' -------------- */
static void test_bracket_is_lossy(void) {
  printf("\n== (3) nickname holding '[' -> nick_lossy == true (Gen 3 has no code point) ==\n");
  Gb12Mon in;
  make_base_mon(&in);
  strcpy(in.nickname, "PIKA[X]");
  strcpy(in.ot_name, "GUY");

  Gb12Target tgt = { .met_game = 3 };
  uint8_t rec[80];
  Gb12Notes notes;
  Gb12Result r = gen12_convert(&in, &tgt, rec, &notes);
  expect(r == GB12_OK, "bracket fixture converts");
  if (r != GB12_OK) return;

  expect(notes.nick_lossy, "notes.nick_lossy is true -- '[' and ']' silently became spaces");
  expect(!notes.otname_lossy, "notes.otname_lossy stays false -- the OT name has no bracket");
}

/* ---- (3b) the punctuation this fix actually ADDED must NOT be lossy ---------- */
static void test_punctuation_is_lossless(void) {
  printf("\n== (3b) ( ) : ; & now round-trip -- nick_lossy == false ==\n");
  Gb12Mon in;
  make_base_mon(&in);
  strcpy(in.nickname, "A&B(C):D;");
  strcpy(in.ot_name, "GUY");

  Gb12Target tgt = { .met_game = 3 };
  uint8_t rec[80];
  Gb12Notes notes;
  Gb12Result r = gen12_convert(&in, &tgt, rec, &notes);
  expect(r == GB12_OK, "punctuation fixture converts");
  if (r != GB12_OK) return;

  expect(!notes.nick_lossy, "notes.nick_lossy is false -- every glyph in \"A&B(C):D;\" has a real Gen-3 byte");
}

int main(void) {
  test_full_sweep();
  test_new_ascii_codes();
  test_eacute_roundtrip();
  test_ligature_overflow_lossy();
  test_nidoran_female_lossless();
  test_nidoran_male_lossless();
  test_bracket_is_lossy();
  test_punctuation_is_lossless();
  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
