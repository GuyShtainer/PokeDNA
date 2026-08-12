/* source/gen1_write.c — the Generation-I write path — under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gen1write_test.c \
 *      tests/gen12_fixture.c source/gen1_save.c source/gen1_write.c -o /tmp/hg1w && /tmp/hg1w
 *
 * The bar this file has to clear is Guy's: never lose or silently alter a Pokemon. So
 * it is not enough to show that an edit lands. Every test below diffs the WHOLE 32 KiB
 * image and accounts for every single differing byte against a list written out in
 * advance; a write that changes one byte more than it should fails here.
 *
 * Two corpora, on purpose:
 *   - Guy's REAL Red.sav (a scratch COPY; the originals live outside the repo at
 *     gba-toolkit/roms/gb and are never written to). A synthetic fixture built from the
 *     same constants as the code under test cannot falsify those constants — that is
 *     exactly how a transposed 0x3D96/0x3D69 survived a full green test run on the
 *     Gen-2 side. The real save is the only thing that can say "no".
 *   - tests/gen12_fixture.c's synthetic image, which is adversarial in a way the real
 *     save is not: its stale bank copy of the current box holds DIFFERENT Pokemon from
 *     the live copy, which is what makes the current-box duality test meaningful.
 * A missing corpus SKIPS; a present corpus must pass perfectly.
 *
 * Ground truth that is neither the fixture nor the module: the six base-stat lines and
 * the growth rates below are read out of pret/pokered's own data/pokemon/base_stats,
 * and gen1_calc_stat / gen1_exp_for_level are asserted against the stat lines and EXP
 * values that Guy's real level-100 party actually stores.
 *
 * THESE TESTS WERE SHOWN TO FAIL. Twenty single-line mutations were applied to a COPY
 * of gen1_write.c and the suite re-run against each; nineteen were caught, including:
 * dropping INSERT's new terminator or its count bump; skipping DELETE's count
 * decrement, its species-list shift, its nickname shift, its OT shift or retail's
 * last-slot 0xFF stamp; writing only the primary copy of the open box, or only its bank
 * mirror; skipping the main or the bank checksums; substituting a lookalike glyph
 * instead of refusing an unrepresentable one; +5 -> +6 in the stat formula; a /4 -> /2
 * sqrt bonus; one digit of the Medium Slow curve; setting the level without its exp and
 * stats; setting a species without its stored types.
 *
 * The twentieth — deleting the gen1_write_verify_op call from gen1_write_apply — changed
 * nothing, which is the correct result for a redundant safety net over a correct
 * builder. Its wiring was proven by pairing it with a broken builder: with the gate in
 * place, gen1_write_apply REFUSES ("fixture delete" fails at the call); with the gate
 * removed, the same apply returns OK and the corruption reaches the image (only the
 * later content assertions fail). The gate is what stops a bad build from ever landing.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "gen1_save.h"
#include "gen1_write.h"
#include "gen12_fixture.h"

/* Guy's cartridge dumps. Opened "rb" and copied into RAM — this file never opens a
 * corpus save for writing, and every edit happens on the in-memory copy. Override with
 * PKDNA_GB_CORPUS to point at a scratch directory. */
#define ROMS_DEFAULT "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"
static const char* roms_dir(void) {
  const char* e = getenv("PKDNA_GB_CORPUS");
  return (e && *e) ? e : ROMS_DEFAULT;
}

static int g_fail = 0, g_check = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { \
    printf("  !! FAIL: "); printf(__VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* ------------------------------------------------------------------------- */
/* Species facts, straight out of pokered data/pokemon/base_stats/<name>.asm  */
/* ------------------------------------------------------------------------- */

#define T_ELECTRIC 0x17
#define T_PSYCHIC  0x18
#define T_GRASS    0x16
#define T_POISON   0x03
#define T_WATER    0x15
#define T_FIRE     0x14
#define T_FLYING   0x02
#define T_DRAGON   0x1A

typedef struct { uint8_t idx; const char* name; Gen1SpeciesInfo si; } Species;

/* internal index -> facts. Internal indexes verified via gen1_dex_from_index(). */
static const Species k_species[] = {
  /* idx  name        HP  Atk Def Spe Spc   growth                    t1 t2  catch */
  { 0x15, "MEW",      {{100,100,100,100,100}, GEN1_GROWTH_MEDIUM_SLOW, T_PSYCHIC, T_PSYCHIC,  45 } },
  { 0x83, "MEWTWO",   {{106,110, 90,130,154}, GEN1_GROWTH_SLOW,        T_PSYCHIC, T_PSYCHIC,   3 } },
  { 0x9A, "VENUSAUR", {{ 80, 82, 83, 80,100}, GEN1_GROWTH_MEDIUM_SLOW, T_GRASS,   T_POISON,   45 } },
  { 0x1C, "BLASTOISE",{{ 79, 83,100, 78, 85}, GEN1_GROWTH_MEDIUM_SLOW, T_WATER,   T_WATER,    45 } },
  { 0x42, "DRAGONITE",{{ 91,134, 95, 80,100}, GEN1_GROWTH_SLOW,        T_DRAGON,  T_FLYING,   45 } },
  { 0xB4, "CHARIZARD",{{ 78, 84, 78,100, 85}, GEN1_GROWTH_MEDIUM_SLOW, T_FIRE,    T_FLYING,   45 } },
  { 0x54, "PIKACHU",  {{ 35, 55, 30, 90, 50}, GEN1_GROWTH_MEDIUM_FAST, T_ELECTRIC,T_ELECTRIC,190 } },
};
static const Species* species_by_index(uint8_t idx) {
  for (size_t i = 0; i < sizeof k_species / sizeof k_species[0]; i++)
    if (k_species[i].idx == idx) return &k_species[i];
  return NULL;
}

/* ------------------------------------------------------------------------- */
/* Image helpers                                                              */
/* ------------------------------------------------------------------------- */

#define IMGCAP (64 * 1024)
typedef struct { uint8_t b[IMGCAP]; uint32_t len; } Image;

static bool load_image(Image* im, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  im->len = (uint32_t)fread(im->b, 1, IMGCAP, f);
  fclose(f);
  return im->len >= GEN1_SAVE_SIZE;
}

/* Every allowed differing region of one write, declared BEFORE the write happens. */
typedef struct { uint32_t from, to; const char* why; } Range;

/* Diff the whole image. Every differing byte must fall inside one of `ok`; every range
 * marked `must` must contain at least one differing byte (so a test cannot pass by
 * doing nothing). Prints the first few offenders. */
static bool diff_accounted(const uint8_t* a, const uint8_t* b, uint32_t n,
                           const Range* ok, int nok, const char* label) {
  int bad = 0, i;
  uint32_t o;
  int hits[16];
  bool clean = true;
  for (i = 0; i < nok && i < 16; i++) hits[i] = 0;
  for (o = 0; o < n; o++) {
    if (a[o] == b[o]) continue;
    for (i = 0; i < nok; i++)
      if (o >= ok[i].from && o < ok[i].to) { if (i < 16) hits[i]++; break; }
    if (i == nok) {
      if (bad < 8) printf("  !! %s: unexplained change at 0x%04X (%02X -> %02X)\n",
                          label, o, a[o], b[o]);
      bad++;
      clean = false;
    }
  }
  if (bad) printf("  !! %s: %d unexplained differing byte(s)\n", label, bad);
  for (i = 0; i < nok && i < 16; i++)
    if (ok[i].why && hits[i] == 0) {
      printf("  !! %s: nothing changed in the '%s' region (0x%04X..0x%04X) — "
             "the edit did not happen\n", label, ok[i].why, ok[i].from, ok[i].to);
      clean = false;
    }
  return clean;
}

/* Decode every slot of a box into a caller-owned array; returns the count. */
static int snapshot(const Gen1Save* s, const uint8_t* img, int box, Gen1Mon* out, int cap) {
  int n = gen1_count(s, box), i;
  if (n > cap) n = cap;
  for (i = 0; i < n; i++) gen1_decode_image(s, img, box, i, &out[i]);
  return n;
}

static uint32_t bank_slot_off(int box) {
  return (box < 6 ? GEN1_OFF_BANK2 : GEN1_OFF_BANK3) + (uint32_t)(box % 6) * GEN1_BOX_BYTES;
}

/* ------------------------------------------------------------------------- */
/* 0. The pure pieces: charset + the two arithmetic cores                     */
/* ------------------------------------------------------------------------- */

static void test_charset(void) {
  static const char* const ok[] = {
    "PIKACHU", "Mr Mime", "ABCDEFGHIJ", "A", "12345", "N.T", "IT'S", "HE'LL",
    "(A):B;", "[X]", "A-B", "WHY?", "YES!", "A/B", "A,B", "M\xE2\x99\x82", "F\xE2\x99\x80",
  };
  static const char* const bad[] = {
    "", "ABCDEFGHIJK",           /* empty / 11 glyphs                       */
    "A#B", "A@B", "A*B", "A+B", "A=B", "A\"B", "A_B", "A%B", "A&B", "A$B",
    "caf\xC3\xA9",               /* UTF-8 e-acute: the charset has no letter for it */
  };
  uint8_t f[GEN1_NAME_BYTES];
  char back[64];
  size_t i;

  printf("-- charset --\n");
  for (i = 0; i < sizeof ok / sizeof ok[0]; i++) {
    memset(f, 0xAA, sizeof f);
    CHECK(gen1_encode_name(f, ok[i], GEN1_NICK_GLYPHS), "'%s' must encode", ok[i]);
    gen1_decode_name(back, sizeof back, f, GEN1_NAME_BYTES);
    CHECK(strcmp(back, ok[i]) == 0, "'%s' round-tripped to '%s'", ok[i], back);
    CHECK(f[GEN1_NAME_BYTES - 1] == GEN1_TEXT_TERM, "'%s' must be terminator-padded", ok[i]);
  }
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    memset(f, 0xAA, sizeof f);
    CHECK(!gen1_encode_name(f, bad[i], GEN1_NICK_GLYPHS), "'%s' must be REFUSED", bad[i]);
    CHECK(f[0] == 0xAA, "a refused name must leave the field untouched ('%s')", bad[i]);
  }
  /* an OT field holds 7 glyphs, not 10 — the naming screen's PLAYER_NAME_LENGTH-1 */
  CHECK(gen1_encode_name(f, "ABCDEFG", GEN1_OT_GLYPHS), "a 7-glyph OT must fit");
  CHECK(!gen1_encode_name(f, "ABCDEFGH", GEN1_OT_GLYPHS), "an 8-glyph OT must be refused");
  /* The LOWERCASE contractions are one GB byte, so "ABCDEFGHI's" is 10 glyphs, not 11 —
   * while "'S" is a plain apostrophe plus S, because 0xBD decodes to lowercase "'s". */
  CHECK(gen1_encode_name(f, "ABCDEFGHI's", GEN1_NICK_GLYPHS),
        "10 glyphs where one is a contraction must fit");
  CHECK(!gen1_encode_name(f, "ABCDEFGHI'S", GEN1_NICK_GLYPHS),
        "the same string with an uppercase S is 11 glyphs and must be refused");
}

/* Hand-worked from the decomp formula, so this half of the arithmetic is pinned even
 * with no corpus present:
 *     stat = ((base + dv) * 2 + ceil(sqrt(statexp))/4) * level / 100 + 5
 *     HP   = ... + level + 10                          (home/move_mon.asm:54-232)
 */
static void test_stat_formula(void) {
  printf("-- stat formula --\n");
  /* (100+15)*2 + 63 = 293; +5 = 298 attack, +110 = 403 HP. 63, not 64: the game's
   * sqrt loop bails at 255 rather than reaching 256. */
  CHECK(gen1_calc_stat(G1_ATK, 100, 15, 65535, 100) == 298, "L100 Atk, maxed stat exp");
  CHECK(gen1_calc_stat(G1_HP,  100, 15, 65535, 100) == 403, "L100 HP, maxed stat exp");
  CHECK(gen1_calc_stat(G1_HP,  100, 13, 65535, 100) == 399, "…and with a 13 HP DV");
  /* low level: 90*5/100 floors to 4, so 9 and 19. */
  CHECK(gen1_calc_stat(G1_ATK,  45, 0, 0, 5) ==  9, "L5 Atk floors the division");
  CHECK(gen1_calc_stat(G1_HP,   45, 0, 0, 5) == 19, "L5 HP");
  /* the ceil(sqrt)/4 staircase: 256 -> 16 -> 4, 400 -> 20 -> 5 */
  CHECK(gen1_calc_stat(G1_ATK, 0, 0, 256, 100) ==  9, "stat exp 256 gives a bonus of 4");
  CHECK(gen1_calc_stat(G1_ATK, 0, 0, 257, 100) ==  9, "…and 257 still gives 4");
  CHECK(gen1_calc_stat(G1_ATK, 0, 0, 400, 100) == 10, "…and 400 gives 5");
  CHECK(gen1_calc_stat(G1_ATK, 0, 0,   0, 100) ==  5, "no stat exp, no bonus");
  /* the derived HP DV */
  CHECK(gen1_derive_hp_dv(15, 15, 15, 15) == 15, "all-odd DVs give HP DV 15");
  CHECK(gen1_derive_hp_dv(14, 14, 14, 14) ==  0, "all-even DVs give HP DV 0");
  CHECK(gen1_derive_hp_dv(1, 0, 1, 0) == 0x0A, "HP DV is Atk/Def/Spe/Spc low bits, in order");
}

static void test_exp_curves(void) {
  printf("-- exp curves --\n");
  /* pokered data/growth_rates.asm, evaluated at n=100. */
  CHECK(gen1_exp_for_level(GEN1_GROWTH_MEDIUM_FAST, 100) == 1000000, "MF@100");
  CHECK(gen1_exp_for_level(GEN1_GROWTH_MEDIUM_SLOW, 100) == 1059860, "MS@100");
  CHECK(gen1_exp_for_level(GEN1_GROWTH_FAST, 100)        ==  800000, "F@100");
  CHECK(gen1_exp_for_level(GEN1_GROWTH_SLOW, 100)        == 1250000, "S@100");
  CHECK(gen1_exp_for_level(GEN1_GROWTH_MEDIUM_FAST, 5)   ==     125, "MF@5");
  CHECK(gen1_exp_for_level(GEN1_GROWTH_SLOW, 5)          ==     156, "S@5 = floor(5/4*125)");
  /* level 1 is never asked for by the game and wraps negative on Medium Slow */
  CHECK(gen1_exp_for_level(GEN1_GROWTH_MEDIUM_SLOW, 1)   ==       0, "MS@1 must be 0, not a wrap");
  /* the inverse must agree with the forward curve at every level */
  for (int g = 0; g < GEN1_NUM_GROWTH_RATES; g++)
    for (int lv = 2; lv <= 100; lv++) {
      uint32_t e = gen1_exp_for_level((uint8_t)g, (uint8_t)lv);
      CHECK(gen1_level_from_exp((uint8_t)g, e) == lv, "growth %d level %d round-trip", g, lv);
      if (e > 0) CHECK(gen1_level_from_exp((uint8_t)g, e - 1) == lv - 1,
                       "growth %d: one exp below level %d must be level %d", g, lv, lv - 1);
    }
}

/* ------------------------------------------------------------------------- */
/* The real-save suite                                                        */
/* ------------------------------------------------------------------------- */

/* gen1_calc_stat against the five stored battle stats of every mon in a REAL party.
 * Nothing here comes from gen1_write.c: the base stats are read out of the decomp and
 * the expected numbers are the bytes in Guy's cartridge.
 *
 * 25 of the 30 stats match to the digit. The other five do not, and the reason is not a
 * bug: Gen 1 only recomputes a party mon's stats when it LEVELS UP or is withdrawn from
 * a box (the "box trick"), so a level-100 mon that has battled since carries a stat line
 * from an EARLIER, smaller stat exp. Every mismatch here is in that one direction, so
 * the assertion is the sharpest one that is actually true: the stored value must be a
 * value this formula produces — at the current stat exp, or at some smaller one. A
 * formula with a wrong constant or a wrong ceil-sqrt would miss that reachable set
 * entirely. */
static bool stat_reachable(int st, const Species* sp, const Gen1Mon* m, uint16_t stored) {
  uint32_t b;
  /* bonus b is reached by stat exp (4b)^2 (ceil(sqrt) == 4b), and (4b)^2 must not
   * exceed the stat exp the mon carries now — the lag only ever runs backwards. */
  for (b = 0; b <= 63; b++) {
    uint32_t se = (4u * b) * (4u * b);
    if (se > m->statexp[st]) break;
    if (gen1_calc_stat(st, sp->si.base[st], m->dv[st], (uint16_t)se, m->level) == stored)
      return true;
  }
  return false;
}

static void test_real_stats(const Image* im, const Gen1Save* s) {
  int i, exact = 0, lagged = 0;
  printf("-- gen1_calc_stat vs the real party --\n");
  for (i = 0; i < s->party_count; i++) {
    Gen1Mon m;
    const Species* sp;
    const uint8_t* rec;
    int st;
    if (!gen1_decode_image(s, im->b, GEN1_PARTY_BOX, i, &m)) continue;
    sp = species_by_index(m.species_idx);
    CHECK(sp != NULL, "party slot %d: species 0x%02X missing from the test table", i, m.species_idx);
    if (!sp) continue;
    rec = im->b + GEN1_OFF_PARTY + 8 + (uint32_t)i * GEN1_PARTY_REC_BYTES;
    for (st = 0; st < G1_NSTATS; st++) {
      uint16_t want = (uint16_t)((rec[G1R_STATS + st * 2] << 8) | rec[G1R_STATS + st * 2 + 1]);
      uint16_t got  = gen1_calc_stat(st, sp->si.base[st], m.dv[st], m.statexp[st], m.level);
      if (got == want) { exact++; continue; }
      lagged++;
      CHECK(want < got, "%s stat %d: cartridge %u is ABOVE the formula's %u — the stat-exp "
            "lag can only run the other way", sp->name, st, want, got);
      CHECK(stat_reachable(st, sp, &m, want),
            "%s stat %d: cartridge %u is not reachable by this formula at any stat exp "
            "<= %u (computed %u)", sp->name, st, want, m.statexp[st], got);
    }
    CHECK(m.exp == gen1_exp_for_level(sp->si.growth, m.level),
          "%s: stored exp %lu != curve exp %lu at level %u", sp->name,
          (unsigned long)m.exp, (unsigned long)gen1_exp_for_level(sp->si.growth, m.level), m.level);
  }
  printf("   %d/%d stored stats reproduced exactly; %d explained by the stat-exp lag\n",
         exact, exact + lagged, lagged);
  /* If the formula were wrong the exact count would collapse — pin it. */
  CHECK(exact >= (exact + lagged) * 2 / 3, "most stored stats must match to the digit");
}

/* 1. A no-op must not change one byte — checksums included. */
static void test_noop(const Image* base) {
  static const int boxes[] = { 0 /* the open box: two homes */, 1, 5, GEN1_PARTY_BOX };
  Gen1WriteScratch* sc = malloc(sizeof *sc);
  size_t bi;
  printf("-- 1. no-op edits are byte-identical --\n");
  for (bi = 0; bi < sizeof boxes / sizeof boxes[0]; bi++) {
    Image im = *base;
    Gen1Save s;
    int box = boxes[bi], n, slot;
    CHECK(gen1_open(im.b, im.len, &s) == GEN1_OK, "open");
    n = gen1_count(&s, box);
    for (slot = 0; slot < n; slot++) {
      Gen1EditMon e;
      Gen1Op op;
      Gen1WStatus st;
      uint32_t off = gen1_list_offset(&s, box);
      if (!gen1_edit_load(im.b + off, box, slot, &e)) {
        printf("  (box %d slot %d not loadable — species-list mismatch, read-only)\n", box, slot);
        continue;
      }
      op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = slot; op.mon = &e;
      st = gen1_write_apply(im.b, im.len, &s, &op, sc);
      CHECK(st == GEN1W_OK, "box %d slot %d no-op: %s", box, slot, gen1_write_status_text(st));
    }
    CHECK(memcmp(im.b, base->b, base->len) == 0,
          "box %d: %d no-op edits changed the image", box, n);
    printf("   box %-2d: %2d slots re-written identically, image untouched\n", box, n);
  }
  free(sc);
}

/* 2. One field at a time, with every differing byte in the file accounted for. */
static void test_field_edits(const Image* base) {
  Gen1WriteScratch* sc = malloc(sizeof *sc);
  Gen1Save s0;
  gen1_open(base->b, base->len, &s0);

  printf("-- 2. single-field edits, whole-image diff accounted --\n");

  /* --- 2a. nickname, in a BANKED box (one destination + that bank's checksums) --- */
  {
    Image im = *base;
    Gen1Save s;
    const int box = 5, slot = 3;
    Gen1EditMon e;
    Gen1Op op;
    Gen1Mon before[GEN1_BOX_CAPACITY], after[GEN1_BOX_CAPACITY];
    int n, i;
    uint32_t blob = bank_slot_off(box), nick;
    Range ok[2];

    gen1_open(im.b, im.len, &s);
    n = snapshot(&s, im.b, box, before, GEN1_BOX_CAPACITY);
    CHECK(gen1_edit_load(im.b + blob, box, slot, &e), "load box %d slot %d", box, slot);
    CHECK(g1e_set_nickname(&e, "ZAPPY"), "set nickname");
    op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = slot; op.mon = &e;
    CHECK(gen1_write_apply(im.b, im.len, &s, &op, sc) == GEN1W_OK, "nickname write");

    /* nickname array: rec_off + 20*33 + 20*11 = 902 */
    nick = blob + 902u + (uint32_t)slot * GEN1_NAME_BYTES;
    ok[0].from = nick; ok[0].to = nick + GEN1_NAME_BYTES; ok[0].why = "the nickname field";
    ok[1].from = GEN1_OFF_BANK2_SUMS; ok[1].to = GEN1_OFF_BANK2_SUMS + 7;
    ok[1].why = "bank-2 checksums";
    CHECK(diff_accounted(base->b, im.b, base->len, ok, 2, "2a nickname"), "2a diff");

    snapshot(&s, im.b, box, after, GEN1_BOX_CAPACITY);
    CHECK(strcmp(after[slot].nickname, "ZAPPY") == 0, "nickname reads back as ZAPPY (got '%s')",
          after[slot].nickname);
    /* and in the DECODED view the nickname is the only thing that moved: copy the old
     * name back over the new one and the two structs must be identical */
    {
      Gen1Mon probe = after[slot];
      memcpy(probe.nickname, before[slot].nickname, sizeof probe.nickname);
      CHECK(memcmp(&before[slot], &probe, sizeof probe) == 0,
            "2a: something other than the nickname changed on slot %d", slot);
    }
    for (i = 0; i < n; i++) {
      if (i == slot) continue;
      CHECK(memcmp(&before[i], &after[i], sizeof before[i]) == 0,
            "2a: box %d slot %d changed and should not have", box, i);
    }
    printf("   2a nickname: %d bytes changed, all accounted\n", GEN1_NAME_BYTES + 7);
  }

  /* --- 2b. level on a PARTY mon: level + boxlevel + exp + 5 stats + main checksum --- */
  {
    Image im = *base;
    Gen1Save s;
    const int box = GEN1_PARTY_BOX, slot = 0;
    Gen1EditMon e;
    Gen1Op op;
    Gen1Mon m;
    const Species* sp;
    uint32_t rec;
    Range ok[3];
    int i;

    gen1_open(im.b, im.len, &s);
    CHECK(gen1_decode_image(&s, im.b, box, slot, &m), "decode party slot 0");
    sp = species_by_index(m.species_idx);
    CHECK(sp != NULL, "party slot 0 species known");
    CHECK(gen1_edit_load(im.b + GEN1_OFF_PARTY, box, slot, &e), "load party slot 0");
    CHECK(g1e_set_level(&e, 50, &sp->si), "set level 50");
    op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = slot; op.mon = &e;
    CHECK(gen1_write_apply(im.b, im.len, &s, &op, sc) == GEN1W_OK, "level write");

    rec = GEN1_OFF_PARTY + 8u + (uint32_t)slot * GEN1_PARTY_REC_BYTES;
    ok[0].from = rec + G1R_BOXLEVEL; ok[0].to = rec + G1R_BOXLEVEL + 1; ok[0].why = "box level";
    ok[1].from = rec + G1R_EXP;      ok[1].to = rec + G1R_EXP + 3;      ok[1].why = "exp";
    /* current HP (0x01) may clamp; level (0x21) and the 5 stats (0x22..0x2B) move */
    ok[2].from = rec + G1R_HP;       ok[2].to = rec + G1R_HP + 2;       ok[2].why = NULL;
    {
      Range all[5];
      all[0] = ok[0]; all[1] = ok[1]; all[2] = ok[2];
      all[3].from = rec + G1R_LEVEL; all[3].to = rec + G1R_STATS + 10; all[3].why = "level + stats";
      all[4].from = GEN1_OFF_CHECKSUM; all[4].to = GEN1_OFF_CHECKSUM + 1; all[4].why = NULL;
      CHECK(diff_accounted(base->b, im.b, base->len, all, 5, "2b level"), "2b diff");
    }

    CHECK(gen1_decode_image(&s, im.b, box, slot, &m), "re-decode party slot 0");
    CHECK(m.level == 50, "level reads back as 50 (got %u)", m.level);
    CHECK(m.exp == gen1_exp_for_level(sp->si.growth, 50), "exp follows the curve");
    for (i = 0; i < G1_NSTATS; i++) {
      const uint8_t* r = im.b + rec;
      uint16_t got = (uint16_t)((r[G1R_STATS + i * 2] << 8) | r[G1R_STATS + i * 2 + 1]);
      uint16_t want = gen1_calc_stat(i, sp->si.base[i], m.dv[i], m.statexp[i], 50);
      CHECK(got == want, "party stat %d after level edit: %u != %u", i, got, want);
    }
    printf("   2b level: %s 100 -> 50, exp %lu, stats recomputed\n", sp->name,
           (unsigned long)m.exp);
  }

  /* --- 2c. DVs, moves and species in the OPEN box: BOTH homes must change --- */
  {
    Image im = *base;
    Gen1Save s;
    int box, slot = 2;
    Gen1EditMon e;
    Gen1Op op;
    Gen1Mon before[GEN1_BOX_CAPACITY], after[GEN1_BOX_CAPACITY];
    const Species* pika = species_by_index(0x54);
    uint32_t live, mirror, rec_in_blob;
    Range ok[6];
    int n, i;

    gen1_open(im.b, im.len, &s);
    box = s.current_box;
    live = GEN1_OFF_CURRENT_BOX;
    mirror = bank_slot_off(box);
    CHECK(gen1_list_offset(&s, box) == live, "the open box must resolve to 0x30C0");

    n = snapshot(&s, im.b, box, before, GEN1_BOX_CAPACITY);
    CHECK(gen1_edit_load(im.b + live, box, slot, &e), "load open-box slot %d", slot);
    g1e_set_dv(&e, G1_ATK, 15);
    g1e_set_dv(&e, G1_SPC, 15);
    g1e_set_move(&e, 3, 85 /* Thunderbolt */, 15);
    CHECK(g1e_set_species(&e, pika->idx, &pika->si), "set species Pikachu");
    op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = slot; op.mon = &e;
    CHECK(gen1_write_apply(im.b, im.len, &s, &op, sc) == GEN1W_OK, "open-box write");

    rec_in_blob = 22u + (uint32_t)slot * GEN1_BOX_REC_BYTES;   /* 1 + 21 + slot*33 */
    /* species list byte, the record's species/type/catch, moves, DVs — plus the SAME
     * bytes in the mirror, plus both checksums. */
    ok[0].from = live + 1 + (uint32_t)slot;   ok[0].to = ok[0].from + 1;  ok[0].why = "live species list";
    ok[1].from = live + rec_in_blob;          ok[1].to = live + rec_in_blob + GEN1_BOX_REC_BYTES;
    ok[1].why = "live record";
    ok[2].from = mirror + 1 + (uint32_t)slot; ok[2].to = ok[2].from + 1;  ok[2].why = "mirror species list";
    ok[3].from = mirror + rec_in_blob;        ok[3].to = mirror + rec_in_blob + GEN1_BOX_REC_BYTES;
    ok[3].why = "mirror record";
    ok[4].from = GEN1_OFF_CHECKSUM;           ok[4].to = GEN1_OFF_CHECKSUM + 1; ok[4].why = NULL;
    ok[5].from = GEN1_OFF_BANK2_SUMS;         ok[5].to = GEN1_OFF_BANK2_SUMS + 7; ok[5].why = "bank-2 checksums";
    CHECK(diff_accounted(base->b, im.b, base->len, ok, 6, "2c open box"), "2c diff");

    /* the duality, stated as bytes: the two homes are identical afterwards */
    CHECK(memcmp(im.b + live, im.b + mirror, GEN1_BOX_BYTES) == 0,
          "the live copy and its bank mirror must agree after a write");

    snapshot(&s, im.b, box, after, GEN1_BOX_CAPACITY);
    CHECK(after[slot].species_idx == pika->idx, "species reads back as Pikachu");
    CHECK(after[slot].dex == 25, "dex reads back as 25 (got %u)", after[slot].dex);
    CHECK(after[slot].dv[G1_ATK] == 15 && after[slot].dv[G1_SPC] == 15, "DVs read back");
    CHECK(after[slot].dv[G1_HP] == gen1_derive_hp_dv(15, after[slot].dv[G1_DEF],
                                                     after[slot].dv[G1_SPE], 15),
          "the derived HP DV follows the other four");
    CHECK(after[slot].moves[3] == 85 && after[slot].pp[3] == 15, "move 4 reads back");
    CHECK(after[slot].ppups[3] == 0, "a new move clears its PP Ups");
    CHECK(im.b[live + rec_in_blob + G1R_TYPE1] == T_ELECTRIC &&
          im.b[live + rec_in_blob + G1R_TYPE2] == T_ELECTRIC,
          "changing species must rewrite the stored types");
    CHECK(im.b[live + rec_in_blob + G1R_CATCH_RATE] == 190, "…and the stored catch rate");
    CHECK(strcmp(after[slot].nickname, before[slot].nickname) == 0,
          "the nickname must survive a species change");
    for (i = 0; i < n; i++) {
      if (i == slot) continue;
      CHECK(memcmp(&before[i], &after[i], sizeof before[i]) == 0,
            "2c: open-box slot %d changed and should not have", i);
    }
    printf("   2c open box: species+DVs+move landed in BOTH homes, %d neighbours intact\n", n - 1);
  }
  free(sc);
}

/* 3. Delete from the middle of a FULL box: all four structures compact. */
static void test_delete(const Image* base) {
  Gen1WriteScratch* sc = malloc(sizeof *sc);
  Image im = *base;
  Gen1Save s;
  int box = -1, b, slot = 7, i;
  Gen1Mon before[GEN1_BOX_CAPACITY], after[GEN1_BOX_CAPACITY];
  Gen1Op op;
  uint32_t blob;
  int n;

  printf("-- 3. delete from the middle of a full box --\n");
  gen1_open(im.b, im.len, &s);
  for (b = 0; b < GEN1_NUM_BOXES; b++)
    if (b != s.current_box && gen1_count(&s, b) == GEN1_BOX_CAPACITY) { box = b; break; }
  if (box < 0) { printf("  (no full banked box in this save — skipped)\n"); free(sc); return; }

  blob = bank_slot_off(box);
  n = snapshot(&s, im.b, box, before, GEN1_BOX_CAPACITY);
  CHECK(n == GEN1_BOX_CAPACITY, "box %d is full", box);
  CHECK(im.b[blob + 1 + GEN1_BOX_CAPACITY] == GEN1_LIST_TERM,
        "a full box's terminator sits just past the last slot");

  op.kind = GEN1_OP_DELETE; op.box = box; op.slot = slot; op.mon = NULL;
  CHECK(gen1_write_apply(im.b, im.len, &s, &op, sc) == GEN1W_OK, "delete");

  CHECK(gen1_count(&s, box) == GEN1_BOX_CAPACITY - 1, "the count dropped to 19");
  CHECK(im.b[blob] == GEN1_BOX_CAPACITY - 1, "…in the blob too");
  CHECK(im.b[blob + 1 + GEN1_BOX_CAPACITY - 1] == GEN1_LIST_TERM, "the terminator moved up one");
  CHECK(gen1_blob_check(im.b + blob, box) == GEN1W_OK, "the structural gate passes");

  snapshot(&s, im.b, box, after, GEN1_BOX_CAPACITY);
  for (i = 0; i < GEN1_BOX_CAPACITY - 1; i++) {
    int src = (i < slot) ? i : i + 1;
    CHECK(memcmp(&before[src], &after[i], sizeof after[i]) == 0,
          "after deleting slot %d, new slot %d must be old slot %d ('%s' vs '%s')",
          slot, i, src, before[src].nickname, after[i].nickname);
  }
  /* every one of the four structures moved, not just the records */
  CHECK(im.b[blob + 1 + slot] == before[slot + 1].species_idx, "the species LIST compacted");
  {
    char ot[24], nick[32];
    gen1_decode_name(ot,   sizeof ot,   im.b + blob + 682u + (uint32_t)slot * GEN1_NAME_BYTES,
                     GEN1_NAME_BYTES);
    gen1_decode_name(nick, sizeof nick, im.b + blob + 902u + (uint32_t)slot * GEN1_NAME_BYTES,
                     GEN1_NAME_BYTES);
    CHECK(strcmp(nick, before[slot + 1].nickname) == 0,
          "the NICKNAME array compacted ('%s' should be '%s')", nick, before[slot + 1].nickname);
    CHECK(strcmp(ot, before[slot + 1].otName) == 0, "the OT-NAME array compacted");
  }
  /* the last slot's OT field: retail stamps 0xFF over its first byte only when the very
   * last slot is removed, so here it must simply be the old slot 19 shifted nowhere */
  CHECK(im.b[blob + 682u + 19u * GEN1_NAME_BYTES] != 0xFFu ||
        base->b[blob + 682u + 19u * GEN1_NAME_BYTES] == 0xFFu,
        "deleting a middle slot must not stamp the last OT field");

  /* and the whole-file diff: one box blob + that bank's checksums, nothing else */
  {
    Range ok[2];
    ok[0].from = blob; ok[0].to = blob + GEN1_BOX_BYTES; ok[0].why = "the box blob";
    ok[1].from = GEN1_OFF_BANK2_SUMS; ok[1].to = GEN1_OFF_BANK2_SUMS + 7; ok[1].why = "bank-2 checksums";
    CHECK(diff_accounted(base->b, im.b, base->len, ok, 2, "3 delete"), "3 diff");
  }
  printf("   deleted '%s' from box %d slot %d; 19 remain, all four structures compacted\n",
         before[slot].nickname, box, slot);

  /* deleting the LAST slot of a full box takes retail's other path */
  {
    Image im2 = *base;
    Gen1Save s2;
    Gen1Op op2;
    gen1_open(im2.b, im2.len, &s2);
    op2.kind = GEN1_OP_DELETE; op2.box = box; op2.slot = GEN1_BOX_CAPACITY - 1; op2.mon = NULL;
    CHECK(gen1_write_apply(im2.b, im2.len, &s2, &op2, sc) == GEN1W_OK, "delete last slot");
    CHECK(im2.b[blob] == GEN1_BOX_CAPACITY - 1, "count dropped");
    CHECK(im2.b[blob + 682u + 19u * GEN1_NAME_BYTES] == 0xFFu,
          "removing the last slot stamps 0xFF over its OT field (remove_mon.asm:43)");
    CHECK(memcmp(im2.b + blob + 22u, base->b + blob + 22u,
                 (size_t)GEN1_BOX_CAPACITY * GEN1_BOX_REC_BYTES) == 0,
          "…and shifts no records at all");
  }
  free(sc);
}

/* 4. Insert where there is room; refuse where there is not. */
static void test_insert(const Image* base) {
  Gen1WriteScratch* sc = malloc(sizeof *sc);
  Gen1Save s;
  int roomy = -1, full = -1, b;

  printf("-- 4. insert --\n");
  gen1_open(base->b, base->len, &s);
  for (b = 0; b < GEN1_NUM_BOXES; b++) {
    int c = gen1_count(&s, b);
    if (b == s.current_box) continue;
    if (roomy < 0 && c > 0 && c < GEN1_BOX_CAPACITY) roomy = b;
    if (full  < 0 && c == GEN1_BOX_CAPACITY) full = b;
  }

  if (roomy >= 0) {
    Image im = *base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    Gen1Mon before[GEN1_BOX_CAPACITY], after[GEN1_BOX_CAPACITY];
    const Species* pika = species_by_index(0x54);
    uint32_t blob = bank_slot_off(roomy);
    int n, i;

    gen1_open(im.b, im.len, &s2);
    n = snapshot(&s2, im.b, roomy, before, GEN1_BOX_CAPACITY);

    /* build the newcomer out of an existing record so every field is plausible */
    CHECK(gen1_edit_load(im.b + blob, roomy, 0, &e), "load a donor record");
    CHECK(g1e_set_species(&e, pika->idx, &pika->si), "donor becomes Pikachu");
    CHECK(g1e_set_nickname(&e, "NEWBIE"), "nickname");
    CHECK(g1e_set_otname(&e, "GUY"), "OT name");
    CHECK(g1e_set_level(&e, 12, &pika->si), "level 12");

    op.kind = GEN1_OP_INSERT; op.box = roomy; op.slot = -1; op.mon = &e;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_OK, "insert");
    CHECK(op.slot == n, "the newcomer landed at slot %d (expected %d)", op.slot, n);
    CHECK(gen1_count(&s2, roomy) == n + 1, "the count went up by one");
    CHECK(im.b[blob + 1 + n] == pika->idx, "the species list gained an entry");
    CHECK(im.b[blob + 1 + n + 1] == GEN1_LIST_TERM, "the terminator moved down one");
    CHECK(gen1_blob_check(im.b + blob, roomy) == GEN1W_OK, "structural gate");

    snapshot(&s2, im.b, roomy, after, GEN1_BOX_CAPACITY);
    CHECK(strcmp(after[n].nickname, "NEWBIE") == 0, "nickname reads back (got '%s')",
          after[n].nickname);
    CHECK(strcmp(after[n].otName, "GUY") == 0, "OT reads back (got '%s')", after[n].otName);
    CHECK(after[n].level == 12, "level reads back");
    for (i = 0; i < n; i++)
      CHECK(memcmp(&before[i], &after[i], sizeof after[i]) == 0,
            "4: existing slot %d changed and should not have", i);
    printf("   inserted into box %d at slot %d (%d -> %d)\n", roomy, op.slot, n, n + 1);
  } else {
    printf("  (no partly-filled banked box — insert skipped)\n");
  }

  if (full >= 0) {
    Image im = *base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    Gen1WStatus st;
    gen1_open(im.b, im.len, &s2);
    CHECK(gen1_edit_load(im.b + bank_slot_off(full), full, 0, &e), "load a donor");
    op.kind = GEN1_OP_INSERT; op.box = full; op.slot = -1; op.mon = &e;
    st = gen1_write_apply(im.b, im.len, &s2, &op, sc);
    CHECK(st == GEN1W_ERR_FULL, "insert into a full box must be refused (got %s)",
          gen1_write_status_text(st));
    CHECK(memcmp(im.b, base->b, base->len) == 0, "a refused insert must change nothing");
    printf("   insert into full box %d refused, image untouched\n", full);
  }

  /* the party is six, not twenty */
  {
    Image im = *base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    gen1_open(im.b, im.len, &s2);
    if (gen1_count(&s2, GEN1_PARTY_BOX) == GEN1_PARTY_CAPACITY) {
      CHECK(gen1_edit_load(im.b + GEN1_OFF_PARTY, GEN1_PARTY_BOX, 0, &e), "load party 0");
      op.kind = GEN1_OP_INSERT; op.box = GEN1_PARTY_BOX; op.slot = -1; op.mon = &e;
      CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_FULL,
            "a seventh party member must be refused");
      CHECK(memcmp(im.b, base->b, base->len) == 0, "…changing nothing");
    }
  }
  free(sc);
}

/* 5. After a write the save still validates end to end. */
static void test_integrity(const Image* base) {
  Gen1WriteScratch* sc = malloc(sizeof *sc);
  Image im = *base;
  Gen1Save s;
  Gen1EditMon e;
  Gen1Op op;
  int b, box = -1;

  printf("-- 5. checksums and the structural gate after a write --\n");
  gen1_open(im.b, im.len, &s);
  for (b = 0; b < GEN1_NUM_BOXES; b++)
    if (b != s.current_box && gen1_count(&s, b) > 0) { box = b; break; }
  if (box < 0) { free(sc); return; }

  CHECK(gen1_edit_load(im.b + bank_slot_off(box), box, 0, &e), "load");
  CHECK(g1e_set_nickname(&e, "CHECKSUM"), "nickname");
  op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = 0; op.mon = &e;
  CHECK(gen1_write_apply(im.b, im.len, &s, &op, sc) == GEN1W_OK, "write");

  {
    Gen1Save chk;
    CHECK(gen1_open(im.b, im.len, &chk) == GEN1_OK, "the save still parses");
    CHECK(chk.checksum_stored == chk.checksum_calc, "the main checksum validates");
    for (b = 0; b <= GEN1_NUM_BOXES; b++)
      CHECK(gen1_blob_check(im.b + gen1_list_offset(&chk, b), b) == GEN1W_OK,
            "structural gate on list %d", b);
  }
  /* the bank we touched now carries CORRECT box checksums — ten of Red.sav's fourteen
   * were wrong before, and we rewrite the ones we touch exactly as the game does */
  {
    Gen1Save chk;
    uint8_t want[7];
    int i;
    uint32_t base_off = (box < 6) ? GEN1_OFF_BANK2 : GEN1_OFF_BANK3;
    uint32_t sums     = (box < 6) ? GEN1_OFF_BANK2_SUMS : GEN1_OFF_BANK3_SUMS;
    Gen1Sum whole;
    gen1_open(im.b, im.len, &chk);
    gen1_sum_init(&whole);
    for (i = 0; i < 6; i++) {
      Gen1Sum one;
      gen1_sum_init(&one);
      gen1_sum_feed(&one,   im.b + base_off + (uint32_t)i * GEN1_BOX_BYTES, GEN1_BOX_BYTES);
      gen1_sum_feed(&whole, im.b + base_off + (uint32_t)i * GEN1_BOX_BYTES, GEN1_BOX_BYTES);
      want[1 + i] = gen1_sum_final(&one);
    }
    want[0] = gen1_sum_final(&whole);
    CHECK(memcmp(im.b + sums, want, 7) == 0, "the touched bank's 7 checksum bytes are correct");
  }
  free(sc);
}

/* 6. The gates fire. Each of these would be a silently corrupted save. */
static void test_gates_fire(const Image* base) {
  Gen1WriteScratch* sc = malloc(sizeof *sc);
  Gen1Save s;
  int b, box = -1;
  uint32_t blob;

  printf("-- 6. the verification gates fire --\n");
  gen1_open(base->b, base->len, &s);
  for (b = 0; b < GEN1_NUM_BOXES; b++)
    if (b != s.current_box && gen1_count(&s, b) >= 3) { box = b; break; }
  if (box < 0) { free(sc); return; }
  blob = bank_slot_off(box);

  /* (a) the semantic gate: a candidate that quietly changed a neighbour */
  {
    uint8_t before[GEN1_BOX_BYTES], after[GEN1_BOX_BYTES];
    Gen1EditMon e;
    Gen1Op op;
    memcpy(before, base->b + blob, GEN1_BOX_BYTES);
    memcpy(after,  before, GEN1_BOX_BYTES);
    gen1_edit_load(before, box, 1, &e);
    g1e_set_nickname(&e, "OK");
    op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = 1; op.mon = &e;
    CHECK(gen1_blob_apply(after, &op) == GEN1W_OK, "build the candidate");
    CHECK(gen1_write_verify_op(before, after, box, &op), "a good candidate must verify");

    after[902u + 0u * GEN1_NAME_BYTES] ^= 0x01u;          /* slot 0's nickname */
    CHECK(!gen1_write_verify_op(before, after, box, &op),
          "a candidate that altered an untouched NICKNAME must be refused");
    after[902u + 0u * GEN1_NAME_BYTES] ^= 0x01u;
    after[22u + 2u * GEN1_BOX_REC_BYTES + G1R_DVS] ^= 0x10u;   /* slot 2's DVs */
    CHECK(!gen1_write_verify_op(before, after, box, &op),
          "a candidate that altered an untouched RECORD must be refused");
    after[22u + 2u * GEN1_BOX_REC_BYTES + G1R_DVS] ^= 0x10u;
    after[1 + 3] ^= 0x01u;                                /* slot 3's species-list byte */
    CHECK(!gen1_write_verify_op(before, after, box, &op),
          "a candidate that altered an untouched SPECIES-LIST byte must be refused");
    after[1 + 3] ^= 0x01u;
    after[0]++;                                           /* the count */
    CHECK(!gen1_write_verify_op(before, after, box, &op),
          "a candidate whose COUNT moved must be refused");
  }

  /* (b) the structural gate: count, terminator, a 0xFF inside the list */
  {
    uint8_t blb[GEN1_BOX_BYTES];
    int count;
    memcpy(blb, base->b + blob, GEN1_BOX_BYTES);
    count = blb[0];
    CHECK(gen1_blob_check(blb, box) == GEN1W_OK, "a real box passes the structural gate");
    blb[1 + count] = 0x99u;
    CHECK(gen1_blob_check(blb, box) == GEN1W_ERR_STRUCT, "a missing terminator is refused");
    blb[1 + count] = GEN1_LIST_TERM;
    blb[1 + 1] = GEN1_LIST_TERM;
    CHECK(gen1_blob_check(blb, box) == GEN1W_ERR_STRUCT, "an early terminator is refused");
    blb[1 + 1] = base->b[blob + 1 + 1];
    blb[1 + 1] = 0x00u;
    CHECK(gen1_blob_check(blb, box) == GEN1W_ERR_STRUCT, "species 0 in the used list is refused");
    blb[1 + 1] = base->b[blob + 1 + 1];
    blb[0] = GEN1_BOX_CAPACITY + 1;
    CHECK(gen1_blob_check(blb, box) == GEN1W_ERR_STRUCT, "an over-capacity count is refused");
  }

  /* (c) end to end: poison a structure in the IMAGE, then ask for a write */
  {
    Image im = *base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    Gen1WStatus st;
    Image poisoned;
    int count;

    gen1_open(im.b, im.len, &s2);
    CHECK(gen1_edit_load(im.b + blob, box, 1, &e), "load");
    CHECK(g1e_set_nickname(&e, "NOPE"), "nickname");
    count = im.b[blob];
    im.b[blob + 1 + count] = 0x42u;                  /* break the terminator */
    gen1_write_fix_main_checksum(im.b);              /* keep the save otherwise valid */
    poisoned = im;

    op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = 1; op.mon = &e;
    st = gen1_write_apply(im.b, im.len, &s2, &op, sc);
    CHECK(st == GEN1W_ERR_STRUCT, "a write onto a broken species list must be refused (got %s)",
          gen1_write_status_text(st));
    CHECK(memcmp(im.b, poisoned.b, im.len) == 0, "a refused write must change nothing at all");
  }

  /* (d) the destination gate catches a mirror that did not land */
  {
    Image im = *base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    uint8_t expect[GEN1_BOX_BYTES];
    int cbox;

    gen1_open(im.b, im.len, &s2);
    cbox = s2.current_box;
    CHECK(gen1_edit_load(im.b + GEN1_OFF_CURRENT_BOX, cbox, 0, &e), "load open-box slot 0");
    CHECK(g1e_set_nickname(&e, "MIRROR"), "nickname");
    op.kind = GEN1_OP_REPLACE; op.box = cbox; op.slot = 0; op.mon = &e;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_OK, "write to the open box");
    memcpy(expect, im.b + GEN1_OFF_CURRENT_BOX, GEN1_BOX_BYTES);
    CHECK(gen1_write_verify_image_box(im.b, im.len, &s2, cbox, expect) == GEN1W_OK,
          "both homes hold the blob");
    im.b[bank_slot_off(cbox) + 902u] ^= 0x01u;       /* nudge one byte of the MIRROR only */
    CHECK(gen1_write_verify_image_box(im.b, im.len, &s2, cbox, expect) == GEN1W_ERR_VERIFY,
          "a mirror that lost one byte must be caught");
  }

  /* (e) arguments that are simply not allowed */
  {
    Image im = *base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    gen1_open(im.b, im.len, &s2);
    gen1_edit_load(im.b + blob, box, 0, &e);

    op.kind = GEN1_OP_REPLACE; op.box = box; op.slot = 0; op.mon = &e;
    e.rec[G1R_SPECIES] = 0;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_SPECIES, "species 0 refused");
    e.rec[G1R_SPECIES] = 191;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_SPECIES, "species 191 refused");
    gen1_edit_load(im.b + blob, box, 0, &e);

    op.slot = gen1_count(&s2, box);
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_EMPTY,
          "replacing an empty slot refused");
    op.slot = 0; op.box = GEN1_NUM_BOXES + 1;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_ARG, "bad box refused");
    op.box = GEN1_PARTY_BOX;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_ARG,
          "a box record must not be written into the party");
    /* a save whose checksum does not validate is never written to */
    op.box = box;
    im.b[GEN1_OFF_CHECKSUM] ^= 0xFFu;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_SAVE,
          "a save with a bad checksum is never written to");
    CHECK(memcmp(im.b + blob, base->b + blob, GEN1_BOX_BYTES) == 0, "…and nothing moved");
  }
  free(sc);
}

/* ------------------------------------------------------------------------- */
/* The synthetic suite: the cases a real save does not happen to contain      */
/* ------------------------------------------------------------------------- */

static void test_fixture(void) {
  Gen1WriteScratch* sc = malloc(sizeof *sc);
  Image base;
  Gen1Save s;

  printf("-- fixture: the adversarial synthetic image --\n");
  base.len = gbf_build(GBF_RBY, base.b, 0);
  CHECK(gen1_open(base.b, base.len, &s) == GEN1_OK, "the fixture image parses");

  /* (a) THE DUALITY, at its sharpest. The fixture deliberately plants DIFFERENT
   *     Pokemon in the stale bank copy of the current box. A writer that updated only
   *     one home would leave them different; ours must leave them identical. */
  {
    Image im = base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    int cbox;
    uint32_t live = GEN1_OFF_CURRENT_BOX, mirror;

    gen1_open(im.b, im.len, &s2);
    cbox = s2.current_box;
    mirror = bank_slot_off(cbox);
    CHECK(memcmp(im.b + live, im.b + mirror, GEN1_BOX_BYTES) != 0,
          "the fixture's stale bank copy really is different to start with");
    CHECK(gen1_edit_load(im.b + live, cbox, 0, &e), "load the open box");
    CHECK(g1e_set_nickname(&e, "DUALITY"), "nickname");
    op.kind = GEN1_OP_REPLACE; op.box = cbox; op.slot = 0; op.mon = &e;
    CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_OK, "write to the open box");
    CHECK(memcmp(im.b + live, im.b + mirror, GEN1_BOX_BYTES) == 0,
          "after the write both homes must hold the same box");
    {
      Gen1Mon m;
      Gen1Save chk;
      gen1_open(im.b, im.len, &chk);
      CHECK(gen1_decode_image(&chk, im.b, cbox, 0, &m), "decode");
      CHECK(strcmp(m.nickname, "DUALITY") == 0, "reads back as DUALITY (got '%s')", m.nickname);
    }
  }

  /* (b) THE VIRGIN BANK. Clear BIT_HAS_CHANGED_BOXES and the eleven stored boxes are
   *     un-erased SRAM that the player's first CHANGE BOX will wipe, so an edit there
   *     is not corrupt — it is doomed. It must be refused, while the open box (which
   *     lives in the checksummed main block) stays editable with ONE destination. */
  {
    Image im = base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    uint32_t t[GEN1_WRITE_MAX_TARGETS];
    int n = 0, cbox, other = -1, b;

    im.b[GEN1_OFF_CURRENT_NO] &= 0x7Fu;
    gen1_write_fix_main_checksum(im.b);
    CHECK(gen1_open(im.b, im.len, &s2) == GEN1_OK, "still a valid save");
    cbox = s2.current_box;
    for (b = 0; b < GEN1_NUM_BOXES; b++)
      if (b != cbox && gen1_count(&s2, b) > 0) { other = b; break; }

    CHECK(gen1_write_targets(im.b, &s2, cbox, t, &n) == GEN1W_OK, "the open box stays writable");
    CHECK(n == 1 && t[0] == GEN1_OFF_CURRENT_BOX,
          "…with exactly one destination (no mirror into a bank that is about to be wiped)");
    if (other >= 0) {
      Image before = im;
      CHECK(gen1_write_targets(im.b, &s2, other, t, &n) == GEN1W_ERR_VIRGIN,
            "a stored box is refused while the banks are virgin");
      CHECK(gen1_edit_load(im.b + bank_slot_off(other), other, 0, &e), "load");
      op.kind = GEN1_OP_REPLACE; op.box = other; op.slot = 0; op.mon = &e;
      CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_ERR_VIRGIN,
            "…and so is the write");
      CHECK(memcmp(im.b, before.b, im.len) == 0, "…changing nothing");
    }
  }

  /* (c) insert / delete / no-op on the synthetic image too */
  {
    Image im = base;
    Gen1Save s2;
    Gen1EditMon e;
    Gen1Op op;
    int b, box = -1, n;
    gen1_open(im.b, im.len, &s2);
    for (b = 0; b < GEN1_NUM_BOXES; b++)
      if (b != s2.current_box && gen1_count(&s2, b) >= 2) { box = b; break; }
    if (box >= 0) {
      uint32_t blob = bank_slot_off(box);
      Gen1Mon before[GEN1_BOX_CAPACITY], after[GEN1_BOX_CAPACITY];
      n = snapshot(&s2, im.b, box, before, GEN1_BOX_CAPACITY);
      CHECK(gen1_edit_load(im.b + blob, box, 0, &e), "load");
      op.kind = GEN1_OP_INSERT; op.box = box; op.slot = -1; op.mon = &e;
      CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_OK, "fixture insert");
      op.kind = GEN1_OP_DELETE; op.box = box; op.slot = 1; op.mon = NULL;
      CHECK(gen1_write_apply(im.b, im.len, &s2, &op, sc) == GEN1W_OK, "fixture delete");
      CHECK(gen1_count(&s2, box) == n, "insert then delete returns to the same count");
      snapshot(&s2, im.b, box, after, GEN1_BOX_CAPACITY);
      CHECK(memcmp(&before[0], &after[0], sizeof after[0]) == 0, "slot 0 survived both");
      for (b = 1; b < n - 1; b++)
        CHECK(memcmp(&before[b + 1], &after[b], sizeof after[b]) == 0,
              "slot %d is old slot %d after the delete", b, b + 1);
      CHECK(memcmp(&after[n - 1], &before[0], sizeof after[0]) == 0,
            "the inserted copy of slot 0 is now last");
    }
  }
  free(sc);
}

/* ------------------------------------------------------------------------- */

int main(void) {
  Image base;
  Gen1Save s;
  char path[512];

  printf("== gen1_write: the Generation-I write path ==\n");
  test_charset();
  test_stat_formula();
  test_exp_curves();
  test_fixture();

  snprintf(path, sizeof path, "%s/Red.sav", roms_dir());
  if (!load_image(&base, path)) {
    printf("\n  SKIP: %s not present — the real-cartridge half of this suite did not run.\n", path);
    printf("gen1_write test: %d checks, %d failure(s)\n", g_check, g_fail);
    return g_fail ? 1 : 0;
  }
  printf("\n== Guy's real Red.sav (%u bytes, READ-ONLY: every test works on a copy) ==\n", base.len);
  if (gen1_open(base.b, base.len, &s) != GEN1_OK) {
    printf("  !! FAIL: the real save no longer parses\n");
    return 1;
  }
  printf("   player '%s'  TID %u  party %d  open box %d\n",
         s.player_name, s.trainer_id, s.party_count, s.current_box);

  test_real_stats(&base, &s);
  test_noop(&base);
  test_field_edits(&base);
  test_delete(&base);
  test_insert(&base);
  test_integrity(&base);
  test_gates_fire(&base);

  printf("\ngen1_write test: %d checks, %d failure(s)\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
