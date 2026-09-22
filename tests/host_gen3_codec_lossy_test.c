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
  expect(unmapped == 179, "the unmapped-code population has not moved");
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
  test_nidoran_female_lossless();
  test_nidoran_male_lossless();
  test_bracket_is_lossy();
  test_punctuation_is_lossless();
  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
