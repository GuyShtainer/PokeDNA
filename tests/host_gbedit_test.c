/* Host test for source/gb_edit.c — the Gen-1/2 record editor.
 *
 *   cc -std=c11 -I source -I tests tests/host_gbedit_test.c tests/gen12_fixture.c \
 *      source/gb_edit.c source/gen1_save.c source/gen2_save.c source/data_tables.c \
 *      -o /tmp/hgbe && /tmp/hgbe
 *
 * THREE KINDS OF EVIDENCE, deliberately, because the G/S mirror bug (a transposed digit
 * that every synthetic test agreed with, because the fixture was built from the same
 * constant) proved that one kind is not enough:
 *
 *  1. INDEPENDENT ORACLES — tests/gen12_fixture.c re-transcribes the EXP curves, the
 *     190-entry Gen-1 index map, the DV derivations and a name encoder from the primary
 *     sources, so "the module and the fixture agree" is evidence, not tautology. The
 *     four legacy growth curves are also recomputed here straight from the decomp's own
 *     `growth_rate a,b,c,d,e` table.
 *  2. REAL CARTRIDGE DATA — Guy's own dumps at roms/gb (outside the repo, gitignored,
 *     never copied in). Every record in every save is round-tripped, every name field is
 *     put through the get -> set cycle a UI performs, and the Gen-1 and Gen-2 stat
 *     formulas are checked against the stats those cartridges actually store. The 100
 *     .pk2 files name their own Unown letter in the filename, which makes them ground
 *     truth for a derivation nobody here wrote.
 *  3. NEGATIVE CONTROLS — whole sections that break the input on purpose and FAIL if the
 *     checks do not notice. A test that cannot fail proves nothing. Section 12 is the
 *     strongest form of it: it flips EVERY byte the slot owns, one at a time, and
 *     demands gb_verify_slot refuse each one — the gate has no blind spots or this fails.
 *
 * A missing corpus SKIPS (the files are Guy's and are not in the repo); a corpus that is
 * present must pass perfectly.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>

#include "gen12_fixture.h"
#include "gen1_save.h"
#include "gen2_save.h"
#include "gb_edit.h"
#include "data_tables.h"

#define ROMSGB "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

/* Big enough for the largest list blob EITHER generation has. Gen 1's box is the bigger
 * of the two at 0x462 = 1122 bytes (20 x 33-byte records), where Gen 2's is 1102 — so
 * G2_MAX_LIST_SIZE alone is 20 bytes short and overflows the stack on a Gen-1 box. */
#define MAX_LIST_BYTES 1200

static int g_fail = 0, g_check = 0;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", (msg)); g_fail++; } } while (0)
/* A negative control passes when the condition is FALSE — i.e. when the code under test
 * refuses, or the checker complains, about input we deliberately broke. */
#define REFUSED(c, msg) do { g_check++; if (c) { \
    printf("  !! FAIL (negative control did not trip): %s\n", (msg)); g_fail++; } } while (0)

static void head(const char* s) { printf("\n== %s ==\n", s); }

/* ========================================================================== */
/* 1. level <-> experience                                                     */
/* ========================================================================== */

/* The Game Boy growth-rate table, transcribed HERE from pokered/data/growth_rates.asm
 * (byte-identical to pokecrystal/data/growth_rates.asm):
 *      exp(n) = floor(n^3 * a / b) + c*n^2 + d*n - e        (c signed)
 * This is a third, independent computation — not the shipped table, not the fixture. */
static const struct { int a, b, c, d, e; } k_gb_curve[6] = {
  {1,1,   0,   0,   0},   /* 0 Medium Fast                              */
  {3,4,  10,   0,  30},   /* 1 Slightly Fast — no species uses it       */
  {3,4,  20,   0,  70},   /* 2 Slightly Slow — no species uses it       */
  {6,5, -15, 100, 140},   /* 3 Medium Slow                              */
  {4,5,   0,   0,   0},   /* 4 Fast                                     */
  {5,4,   0,   0,   0},   /* 5 Slow                                     */
};
static long gb_curve_exp(int g, int n) {
  return ((long)n*n*n*k_gb_curve[g].a)/k_gb_curve[g].b
       + (long)k_gb_curve[g].c*n*n + (long)k_gb_curve[g].d*n - k_gb_curve[g].e;
}
/* gen12_fixture.c numbers the curves its own way (GBF_MEDFAST/MEDSLOW/FAST/SLOW); map
 * the shipped GROWTH_* id onto it so its oracle can be consulted. -1 = the fixture has
 * no such curve, which for Gen-1/2 species never happens (see the growth note below). */
static int fixture_curve(uint8_t growth) {
  switch (growth) {
    case 0: return GBF_MEDFAST;
    case 3: return GBF_MEDSLOW;
    case 4: return GBF_FAST;
    case 5: return GBF_SLOW;
    default: return -1;
  }
}

static void test_exp_curves(void) {
  int used[6] = {0,0,0,0,0,0};
  int dex, lvl;
  head("level and EXP must agree, on every curve any Gen-1/2 species actually uses");

  for (dex = 1; dex <= 251; dex++) used[gb_growth_rate((uint16_t)dex)]++;
  printf("  species per growth curve: MedFast %d, SlightlyFast %d, SlightlySlow %d, "
         "MedSlow %d, Fast %d, Slow %d\n", used[0], used[1], used[2], used[3], used[4], used[5]);
  /* Curves 1 and 2 are the ONLY place the GB and Gen-3 growth-rate ids disagree (GB
   * Slightly Fast/Slow vs Gen-3 Erratic/Fluctuating). If no species uses them, reusing
   * pk_exp_for_level for a GB mon cannot reach the disagreement. */
  CHECK(used[1] == 0 && used[2] == 0,
        "no Gen-1/2 species may sit on a curve whose id means something else in Gen 3");

  for (dex = 1; dex <= 251; dex++) {
    uint8_t g = gb_growth_rate((uint16_t)dex);
    int fc = fixture_curve(g);
    for (lvl = 2; lvl <= 100; lvl++) {
      uint32_t got = gb_exp_for_level((uint16_t)dex, (uint8_t)lvl);
      if ((long)got != gb_curve_exp(g, lvl)) {
        printf("  !! dex %d lvl %d: gb_exp_for_level=%u decomp formula=%ld\n",
               dex, lvl, got, gb_curve_exp(g, lvl));
        g_fail++; g_check++; return;
      }
      if (fc >= 0 && got != gbf_exp_for((uint8_t)fc, (uint8_t)lvl)) {
        printf("  !! dex %d lvl %d: gb_exp_for_level=%u fixture oracle=%u\n",
               dex, lvl, got, gbf_exp_for((uint8_t)fc, (uint8_t)lvl));
        g_fail++; g_check++; return;
      }
      /* the property the editor actually relies on */
      if (gb_level_from_exp((uint16_t)dex, got) != lvl) {
        printf("  !! dex %d lvl %d does not survive exp -> level\n", dex, lvl);
        g_fail++; g_check++; return;
      }
    }
  }
  g_check += 3;
  printf("  251 species x levels 2..100: decomp formula, fixture oracle and "
         "level<->exp round-trip all agree\n");

  /* Level 1 is the one entry the GB formula cannot express (Medium Slow underflows). */
  CHECK(gb_exp_for_level(1, 1) == 0, "level 1 must be 0 EXP");
  CHECK(gb_level_from_exp(1, 0) == 1, "0 EXP must read back as level 1");
}

/* ========================================================================== */
/* 2. species numbering                                                        */
/* ========================================================================== */

static void test_species_map(void) {
  int dex, seen[152];
  head("Gen-1 internal index <-> National Dex");
  memset(seen, 0, sizeof seen);
  for (dex = 1; dex <= 151; dex++) {
    uint8_t idx = gb_index_from_dex(GB_GEN1, (uint16_t)dex);
    if (idx == 0) { printf("  !! dex %d has no Gen-1 index\n", dex); g_fail++; g_check++; continue; }
    if (gb_dex_from_index(GB_GEN1, idx) != dex) {
      printf("  !! dex %d -> idx %u -> dex %u\n", dex, idx, gb_dex_from_index(GB_GEN1, idx));
      g_fail++; g_check++;
    }
    /* the fixture's own copy of PokedexOrder, transcribed separately */
    if (gbf_gen1_dex(idx) != dex) {
      printf("  !! idx %u: module says dex %u, fixture oracle says %u\n",
             idx, gb_dex_from_index(GB_GEN1, idx), gbf_gen1_dex(idx));
      g_fail++; g_check++;
    }
    if (seen[dex]++) { printf("  !! dex %d claimed twice\n", dex); g_fail++; g_check++; }
  }
  g_check++;
  printf("  all 151 dex numbers map to a unique internal index and back, "
         "and agree with the fixture's independent table\n");
  CHECK(gb_index_from_dex(GB_GEN1, 152) == 0, "a Gen-2 species has no Gen-1 index");
  CHECK(gb_index_from_dex(GB_GEN2, 251) == 251, "Gen 2 indexes by dex number");
  CHECK(gb_index_from_dex(GB_GEN2, 252) == 0, "252 is past the Gen-2 dex");
  /* MissingNo: index 31/32 map to nothing, so a record holding one is not a Pokemon. */
  CHECK(gb_dex_from_index(GB_GEN1, 31) == 0 && gb_dex_from_index(GB_GEN1, 32) == 0,
        "the MissingNo indices must map to dex 0");
}

/* ========================================================================== */
/* 3. DVs: the derived HP DV, and what a DV change does                        */
/* ========================================================================== */

/* Build a bare Gen-2 box record in `rec` so the DV rules can be exercised without a save. */
static void mk_g2_rec(uint8_t rec[GB_MAX_REC], uint8_t species, uint8_t lvl,
                      uint8_t a, uint8_t d, uint8_t s, uint8_t c) {
  memset(rec, 0, GB_MAX_REC);
  rec[0x00] = species;
  rec[0x02] = 1;                     /* one move, so the structural gate is happy */
  rec[0x17] = pk_move_pp(1);
  rec[0x15] = (uint8_t)((a << 4) | d);
  rec[0x16] = (uint8_t)((s << 4) | c);
  rec[0x1F] = lvl;
  rec[0x08] = (uint8_t)(pk_exp_for_level(pk_species_growth(species), lvl) >> 16);
  rec[0x09] = (uint8_t)(pk_exp_for_level(pk_species_growth(species), lvl) >> 8);
  rec[0x0A] = (uint8_t)(pk_exp_for_level(pk_species_growth(species), lvl));
}

static void test_dv_rules(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES];
  GbEditMon e;
  GbDvEffects fx, fx2;
  uint8_t dv[4];
  int a, d, s, c;
  head("DVs: HP is derived, and gender/shiny/Unown follow the other four");

  memset(nm, 0x50, sizeof nm);
  mk_g2_rec(rec, 1, 20, 15, 14, 13, 12);
  CHECK(gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 1), "load a bare Gen-2 record");

  CHECK(gb_get_dv(&e, GB_ATK) == 15 && gb_get_dv(&e, GB_DEF) == 14
     && gb_get_dv(&e, GB_SPE) == 13 && gb_get_dv(&e, GB_SPC) == 12, "DV nibbles decode");
  /* HP DV = LSBs of Atk,Def,Spe,Spc: 1,0,1,0 -> 0b1010 = 10 */
  CHECK(gb_get_dv(&e, GB_HP) == 10, "HP DV is the parity of the other four");

  /* Exhaustive against the fixture's independent transcription of the same rules. */
  for (a = 0; a < 16; a++) for (d = 0; d < 16; d++)
    for (s = 0; s < 16; s++) for (c = 0; c < 16; c++) {
      dv[0] = (uint8_t)a; dv[1] = (uint8_t)d; dv[2] = (uint8_t)s; dv[3] = (uint8_t)c;
      if (gb_hp_dv(dv) != gbf_hp_dv(dv)) { CHECK(0, "HP DV disagrees with the oracle"); return; }
      gb_dv_effects(dv, 201, 0xFF, &fx);
      if (fx.shiny != gbf_is_shiny(dv)) { CHECK(0, "shininess disagrees with the oracle"); return; }
      if (fx.unown_letter != gbf_unown_letter(dv)) { CHECK(0, "Unown letter disagrees"); return; }
    }
  g_check += 3;
  printf("  all 65536 DV combinations: HP DV, shininess and Unown letter match the "
         "fixture's independent rules\n");

  /* THE SETTER MUST REFUSE THE HP DV. It is not stored anywhere; accepting it would be
   * a lie about what the save can hold. */
  REFUSED(gb_set_dv(&e, GB_HP, 7), "gb_set_dv must refuse the derived HP DV");
  CHECK(gb_get_dv(&e, GB_HP) == 10, "a refused HP-DV write must not have changed anything");
  REFUSED(gb_set_dv(&e, GB_ATK, 16), "a DV above 15 must be refused");

  /* Preview must describe the record the caller has NOT committed yet. */
  gb_dv_effects_of(&e, &fx);
  CHECK(gb_preview_dv(&e, GB_ATK, 2, &fx2), "preview a legal DV change");
  /* Atk 15->2 flips the top bit of the HP DV: parity of (2,14,13,12) is 0,0,1,0 = 2. */
  CHECK(fx2.hp_dv == 2, "preview reports the HP DV the change would produce");
  CHECK(gb_get_dv(&e, GB_HP) == fx.hp_dv && fx.hp_dv == 10, "preview must not mutate the record");

  /* The shiny pattern, stated as the games state it: Def=Spe=Spc=10 and Atk bit 1 set. */
  mk_g2_rec(rec, 25, 20, 10, 10, 10, 10);
  gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25);
  gb_dv_effects_of(&e, &fx);
  CHECK(fx.shiny, "Atk 10 / Def 10 / Spe 10 / Spc 10 is shiny");
  CHECK(gb_set_dv(&e, GB_ATK, 9), "set Atk DV 9");
  gb_dv_effects_of(&e, &fx);
  CHECK(!fx.shiny, "Atk 9 is not shiny");

  /* Gender comes off the Attack DV against the species ratio. Nidoran-F (dex 29) is
   * PERCENT_FEMALE(100) -> all-female; Pikachu (25) is 50/50 at DV <= 7. */
  mk_g2_rec(rec, 25, 20, 7, 0, 0, 0);
  gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25);
  gb_dv_effects_of(&e, &fx);
  CHECK(fx.gender == 1, "Pikachu with Atk DV 7 is female");
  CHECK(gb_set_dv(&e, GB_ATK, 8), "set Atk DV 8");
  gb_dv_effects_of(&e, &fx);
  CHECK(fx.gender == 0, "Pikachu with Atk DV 8 is male");
}

/* ========================================================================== */
/* 4. moves, PP and PP Ups                                                     */
/* ========================================================================== */

static void test_pp(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES];
  GbEditMon e;
  GbIssues iss;
  head("moves, PP and PP Ups");
  memset(nm, 0x50, sizeof nm);
  mk_g2_rec(rec, 1, 20, 5, 5, 5, 5);
  gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 1);

  /* POUND is 35 PP: bonus per Up = 35/5 = 7, so 3 Ups give 56. */
  CHECK(gb_max_pp(GB_GEN2, 1, 0) == 35 && gb_max_pp(GB_GEN2, 1, 3) == 56,
        "Pound: 35 base, 56 with 3 PP Ups");
  /* A 40-PP move would reach 64 and overflow the 6-bit field, so both games cap the
   * per-Up bonus at 7 — the origin of the famous maximum of 61. TACKLE (33) is 35 PP;
   * GROWL (45) is 40, which is the case that needs the cap. */
  CHECK(pk_move_pp(45) == 40, "Growl is a 40-PP move (the case the cap exists for)");
  CHECK(gb_max_pp(GB_GEN2, 45, 3) == 61, "a 40-PP move maxes at 61, not 64");

  /* STRUGGLE IS PER GENERATION. Parsing the PP column of all 165 entries of
   * pokered/data/moves/moves.asm against pokecrystal/data/moves/moves.asm gives exactly
   * one difference over the shared range, and this is it:
   *   pokered/data/moves/moves.asm:178      move STRUGGLE, ..., 100, 10
   *   pokecrystal/data/moves/moves.asm:181  move STRUGGLE, ..., 100,  1,   0
   * One number for both generations was wrong for one of them by construction. */
  CHECK(gb_move_base_pp(GB_GEN1, 165) == 10, "Struggle is 10 PP in Gen 1 (pokered)");
  CHECK(gb_move_base_pp(GB_GEN2, 165) == 1,  "Struggle is 1 PP in Gen 2 (pokecrystal)");
  CHECK(pk_move_pp(165) == 1, "Gen 3 agrees with Gen 2, which is why only Gen 1 is special");
  CHECK(gb_max_pp(GB_GEN1, 165, 3) == 16, "Gen-1 Struggle with 3 PP Ups: 10 + 3*2");
  CHECK(gb_max_pp(GB_GEN2, 165, 3) == 1,  "Gen-2 Struggle cannot be raised: 1 + 3*0");
  /* A move id past the generation's own maximum is garbage, not a Gen-3 move. */
  CHECK(gb_move_base_pp(GB_GEN1, 200) == 0, "Gen 1 has no move 200, so it has no PP");
  CHECK(gb_move_base_pp(GB_GEN2, 200) == pk_move_pp(200), "Gen 2 does have move 200");
  CHECK(gb_move_base_pp(GB_GEN1, 0) == 0 && gb_move_base_pp(GB_GEN2, 0) == 0,
        "an empty slot has no base PP");

  CHECK(gb_set_move(&e, 0, 45), "set move 0 = Growl");
  CHECK(gb_get_pp(&e, 0) == 40 && gb_get_ppup(&e, 0) == 0, "a new move arrives at full PP, no Ups");
  CHECK(gb_set_ppup(&e, 0, 3), "apply 3 PP Ups");
  CHECK(gb_set_pp(&e, 0, 61), "set PP to the new maximum");
  CHECK(gb_get_pp(&e, 0) == 61 && gb_get_ppup(&e, 0) == 3, "61/3 stored in one byte");
  REFUSED(gb_set_pp(&e, 0, 62) && gb_get_pp(&e, 0) == 62, "PP above the maximum must be clamped");
  CHECK(gb_get_pp(&e, 0) == 61, "clamped to 61");
  /* Removing PP Ups must pull current PP down with them, or the record is impossible. */
  CHECK(gb_set_ppup(&e, 0, 0), "remove the PP Ups");
  CHECK(gb_get_pp(&e, 0) == 40, "current PP follows the maximum down");
  /* PP Ups belong to the move, not the slot. */
  CHECK(gb_set_ppup(&e, 0, 2), "re-apply 2 PP Ups");
  CHECK(gb_set_move(&e, 0, 1), "learn a different move in that slot");
  CHECK(gb_get_ppup(&e, 0) == 0, "learning a move loses that slot's PP Ups");
  REFUSED(gb_set_move(&e, 0, 252), "a move id Gen 2 does not have must be refused");
  REFUSED(gb_set_ppup(&e, 0, 4), "more than 3 PP Ups must be refused");

  /* AN EMPTY MOVE SLOT HAS NO PP AND NO PP UPS.
   * gb_max_pp is 0 there, so storing ups<<6 wrote 0xC0 into a byte every real cartridge
   * leaves at 0x00 — all 908 empty move slots across Guy's five saves, without one
   * exception (measured, not assumed). Both setters must refuse, and the structural
   * gate must notice a byte that got there some other way. */
  CHECK(gb_get_move(&e, 3) == 0, "slot 3 is empty");
  REFUSED(gb_set_ppup(&e, 3, 3), "PP Ups on an empty move slot must be refused");
  CHECK(e.rec[0x17 + 3] == 0, "and must have left the PP byte at 0");
  REFUSED(gb_set_pp(&e, 3, 5), "current PP on an empty move slot must be refused");
  CHECK(e.rec[0x17 + 3] == 0, "still 0");
  e.rec[0x17 + 3] = 0xC0;                     /* exactly what the old bug produced */
  REFUSED(gb_check(&e, &iss), "a PP byte on an empty move slot must fail the gate");
  CHECK(iss.pp_on_empty, "and be reported as exactly that");
  CHECK(gb_set_move(&e, 3, 0), "clearing the slot is how you zero it");
  CHECK(e.rec[0x17 + 3] == 0, "gb_set_move(.., 0) writes a clean 0 PP byte");
  CHECK(gb_check(&e, &iss), "clean again");
}

/* ========================================================================== */
/* 5. names: encoding, decoding, and the round trip a UI actually performs      */
/* ========================================================================== */

static void test_names(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES], ref[GB_NAME_BYTES];
  GbEditMon e1, e2;
  char out[GB_TEXT_MAX];
  int i;
  head("name encoding");
  memset(nm, 0x50, sizeof nm);
  mk_g2_rec(rec, 1, 20, 5, 5, 5, 5);
  gb_load_parts(&e2, GB_GEN2, false, rec, nm, nm, 1);
  gb_load_parts(&e1, GB_GEN1, false, rec, nm, nm, 1);

  CHECK(gb_set_nickname(&e2, "BULBASAUR"), "set a plain nickname");
  gb_get_nickname(&e2, out, sizeof out);
  CHECK(strcmp(out, "BULBASAUR") == 0, "plain ASCII round-trips");

  /* Against the fixture's independently written encoder, for the glyphs it covers. */
  gbf_encode_name(ref, GB_NAME_BYTES, "NIDORAN\xE2\x99\x80");
  CHECK(gb_set_nickname(&e2, "NIDORAN\xE2\x99\x80"), "set a name containing the female sign");
  CHECK(memcmp(e2.nick, ref, 8) == 0, "the gender sign encodes as the fixture encodes it");
  gb_get_nickname(&e2, out, sizeof out);
  CHECK(strcmp(out, "NIDORAN\xE2\x99\x80") == 0, "the female sign survives the round trip");

  /* THE CONTRACTIONS ARE THE WHOLE REASON THE ENCODER IS PER-GENERATION. Each is one
   * cartridge byte and two ASCII characters, and Gen 1 keeps them at 0xBB-0xBF plus
   * 0xE4/0xE5 where Gen 2 keeps them at 0xD0-0xD6 (pokered/constants/charmap.asm vs
   * pokecrystal/constants/charmap.asm). Writing the wrong generation's byte renders as
   * katakana on a real cartridge, so all seven are pinned, both ways. */
  {
    static const struct { const char* s; uint8_t g1, g2; } k_contract[7] = {
      { "'d", 0xBBu, 0xD0u }, { "'l", 0xBCu, 0xD1u }, { "'m", 0xE5u, 0xD2u },
      { "'r", 0xE4u, 0xD3u }, { "'s", 0xBDu, 0xD4u }, { "'t", 0xBEu, 0xD5u },
      { "'v", 0xBFu, 0xD6u },
    };
    for (i = 0; i < 7; i++) {
      uint8_t b1 = 0, b2 = 0;
      char t1[GB_GLYPH_MAX], t2[GB_GLYPH_MAX];
      CHECK(gb_char_encode(GB_GEN1, k_contract[i].s, &b1) == 2, "a contraction is 2 bytes in");
      CHECK(gb_char_encode(GB_GEN2, k_contract[i].s, &b2) == 2, "...and 1 byte out");
      CHECK(b1 == k_contract[i].g1, "Gen-1 contraction code point");
      CHECK(b2 == k_contract[i].g2, "Gen-2 contraction code point");
      gb_char_decode(GB_GEN1, k_contract[i].g1, t1);
      gb_char_decode(GB_GEN2, k_contract[i].g2, t2);
      CHECK(strcmp(t1, k_contract[i].s) == 0 && strcmp(t2, k_contract[i].s) == 0,
            "and each decodes back to the same two characters");
    }
    printf("  all 7 apostrophe contractions encode to each generation's own code point\n");
  }

  /* The contraction glyphs are ONE byte on the cartridge and two ASCII characters, and
   * the two generations put them at different code points. */
  CHECK(gb_set_nickname(&e2, "IT'S"), "Gen-2 name with an apostrophe before an uppercase S");
  CHECK(e2.nick[2] == 0xE0u && e2.nick[3] == 0x92u,
        "uppercase S after an apostrophe is a bare quote plus 'S', as FARFETCH'D is stored");
  CHECK(gb_set_nickname(&e2, "it's"), "Gen-2 name with the lowercase contraction");
  CHECK(e2.nick[2] == 0xD4u, "Gen 2 puts 's at 0xD4");
  CHECK(gb_set_nickname(&e1, "it's"), "the same name in Gen 1");
  CHECK(e1.nick[2] == 0xBDu, "Gen 1 puts 's at 0xBD — a different byte for the same glyph");
  gb_get_nickname(&e1, out, sizeof out);
  CHECK(strcmp(out, "it's") == 0, "Gen-1 contraction round-trips");

  /* Truncation is by GLYPH and the field is always fully written. */
  CHECK(gb_set_nickname(&e2, "ABCDEFGHIJKLMNOP"), "set an over-long nickname");
  gb_get_nickname(&e2, out, sizeof out);
  CHECK(strcmp(out, "ABCDEFGHIJ") == 0, "a nickname is capped at 10 glyphs");
  CHECK(e2.nick[10] == 0x50u, "the field is terminated");
  CHECK(gb_set_otname(&e2, "ABCDEFGHIJ"), "set an over-long OT name");
  gb_get_otname(&e2, out, sizeof out);
  CHECK(strcmp(out, "ABCDEFG") == 0, "an OT name is capped at 7 glyphs");
  for (i = 7; i < GB_NAME_BYTES; i++)
    if (e2.otname[i] != 0x50u) { CHECK(0, "the tail of a name field must be filled"); break; }

  /* A GLYPH THE CARTRIDGE CANNOT HOLD IS REFUSED, NOT SILENTLY DROPPED. The old behaviour
   * turned it into a space and returned true, so the player confirmed a name and got a
   * different one back with nothing said. Now the setter writes nothing and says no, and
   * the caller can find out which character was at fault and offer the substitution. */
  {
    char bad[GB_GLYPH_MAX];
    uint8_t keep[GB_NAME_BYTES];
    CHECK(gb_set_nickname(&e2, "OK"), "a plain name is accepted");
    memcpy(keep, e2.nick, GB_NAME_BYTES);

    REFUSED(gb_set_nickname(&e2, "A\xE2\x82\xAC" "B"), "a euro sign has no GB glyph");
    CHECK(memcmp(e2.nick, keep, GB_NAME_BYTES) == 0,
          "and a refused name must leave every byte of the field alone");
    CHECK(gb_text_lossy(GB_GEN2, "A\xE2\x82\xAC" "B", GB_NICK_GLYPHS, bad) == 1,
          "gb_text_lossy counts exactly the one unrepresentable glyph");
    CHECK(strcmp(bad, "\xE2\x82\xAC") == 0, "and names it, so the UI can say which");

    /* The knowing caller can still take the substitution. */
    CHECK(gb_set_nickname_lossy(&e2, "A\xE2\x82\xAC" "B"), "the _lossy variant accepts it");
    gb_get_nickname(&e2, out, sizeof out);
    CHECK(strcmp(out, "A B") == 0, "and substitutes a space, as it always did");

    /* Clean text must not be flagged — including a REAL space, which encodes to 0x7F the
     * same way a substitution does and is the one case that is not a loss. */
    CHECK(gb_text_lossy(GB_GEN2, "A B", GB_NICK_GLYPHS, 0) == 0, "a typed space is not a loss");
    CHECK(gb_text_lossy(GB_GEN2, "MattiaPK", GB_OT_GLYPHS, 0) == 0, "<PK> is representable");
    CHECK(gb_text_lossy(GB_GEN2, "{5D}", GB_NICK_GLYPHS, 0) == 0, "so is an escape");
    /* Past the glyph cap is truncation, a separate and documented loss — not this one. */
    CHECK(gb_text_lossy(GB_GEN2, "ABCDEFG\xE2\x82\xAC", GB_OT_GLYPHS, 0) == 0,
          "a bad glyph past the cap is truncated away, not a reason to refuse");

    /* The two generations disagree about what is representable, and that must show. */
    CHECK(gb_text_lossy(GB_GEN2, "R&D", GB_NICK_GLYPHS, 0) == 0, "Gen 2 has '&' at 0xE9");
    CHECK(gb_text_lossy(GB_GEN1, "R&D", GB_NICK_GLYPHS, bad) == 1,
          "Gen 1 does NOT — 0xE9 is katakana there");
    CHECK(strcmp(bad, "&") == 0, "and the offending glyph is the ampersand");
    REFUSED(gb_set_nickname(&e1, "R&D"), "so Gen 1 refuses the name outright");
    CHECK(gb_text_lossy(GB_GEN1, "M\xC3\xBC" "LLER", GB_NICK_GLYPHS, 0) == 1,
          "a Gen-1 umlaut transliterates to 'u' — an approximation, so still a loss");
    CHECK(gb_text_lossy(GB_GEN2, "M\xC3\xBC" "LLER", GB_NICK_GLYPHS, 0) == 0,
          "Gen 2 stores it exactly");
  }
}

/* ---- 5b. the decoder is the encoder's exact inverse ---------------------- */

/* THE DEFECT THIS SECTION EXISTS FOR. An editing UI only ever does one thing with a
 * name: gb_get_nickname into a buffer, show it, and gb_set_nickname it back when the
 * player confirms. Before this pair was made reversible, that cycle destroyed 1374 of
 * the 2738 name fields in Guy's five real saves — 50.2% — because the shipping decoders
 * spell several distinct bytes the same way and gb_char_encode could only pick one:
 * Gen-1 0x5D <TRAINER> (the OT of every in-game trade) came back as "?" -> 0xE6, Gen-2
 * 0xE1 <PK> came back as " " -> 0x7F, é/×/the umlauts collapsed onto lookalikes, and the
 * decimal point 0xF2 collapsed onto the full stop 0xE8. */
static void test_charset_bijection(void) {
  int gen, c, bad = 0, escaped[2] = {0,0};
  head("every byte a name can hold must survive decode -> encode, in both generations");

  for (gen = GB_GEN1; gen <= GB_GEN2; gen++) {
    for (c = 0; c < 256; c++) {
      char txt[GB_GLYPH_MAX];
      uint8_t back = 0;
      int n, used;
      if (c == 0x50) continue;                    /* the terminator has no glyph */
      n = gb_char_decode((uint8_t)gen, (uint8_t)c, txt);
      if (n <= 0 || n > GB_GLYPH_MAX - 1) {
        printf("  !! gen %d byte 0x%02X decoded to %d chars\n", gen, c, n); bad++; continue;
      }
      if (txt[0] == '{') escaped[gen - 1]++;
      used = gb_char_encode((uint8_t)gen, txt, &back);
      if (used != n || back != (uint8_t)c) {
        printf("  !! gen %d: 0x%02X -> \"%s\" -> 0x%02X (consumed %d of %d)\n",
               gen, c, txt, back, used, n);
        bad++;
      }
    }
  }
  g_check++;
  if (bad) { printf("  !! FAIL: %d byte values are not reversible\n", bad); g_fail++; }
  else printf("  all 255 non-terminator byte values round-trip in both generations "
              "(%d need the {XX} escape in Gen 1, %d in Gen 2)\n", escaped[0], escaped[1]);

  /* The specific glyphs the shipping readers lose, named one by one so a regression
   * says WHICH one broke. Each is "the byte -> what gb_edit says -> the byte again". */
  {
    static const struct { uint8_t gen, byte; const char* txt; const char* what; } k[] = {
      { GB_GEN1, 0x5Du, "{5D}", "Gen-1 <TRAINER>, the OT of every in-game trade" },
      { GB_GEN1, 0xBAu, "\xC3\xA9", "Gen-1 e-acute (gen1_save.c flattens it to \"e\")" },
      { GB_GEN1, 0xE1u, "PK",   "Gen-1 <PK>" },
      { GB_GEN1, 0xE2u, "MN",   "Gen-1 <MN>" },
      { GB_GEN1, 0xF1u, "\xC3\x97", "Gen-1 multiplication sign" },
      { GB_GEN1, 0xF2u, "{F2}", "Gen-1 decimal point (a DIFFERENT tile from 0xE8 '.')" },
      { GB_GEN2, 0x5Du, "{5D}", "Gen-2 <TRAINER>" },
      { GB_GEN2, 0xC0u, "\xC3\x84", "Gen-2 A-umlaut (g2_glyph flattens it to \"A\")" },
      { GB_GEN2, 0xC5u, "\xC3\xBC", "Gen-2 u-umlaut" },
      { GB_GEN2, 0xE1u, "PK",   "Gen-2 <PK> (g2_glyph has no case for it at all)" },
      { GB_GEN2, 0xE2u, "MN",   "Gen-2 <MN>" },
      { GB_GEN2, 0xEAu, "\xC3\xA9", "Gen-2 e-acute" },
      { GB_GEN2, 0xF1u, "\xC3\x97", "Gen-2 multiplication sign (not the letter x)" },
      { GB_GEN2, 0xF2u, "{F2}", "Gen-2 decimal point" },
      { GB_GEN2, 0xEBu, "{EB}", "Gen-2 right arrow" },
    };
    size_t k_i;
    for (k_i = 0; k_i < sizeof k / sizeof k[0]; k_i++) {
      char txt[GB_GLYPH_MAX];
      uint8_t back = 0;
      gb_char_decode(k[k_i].gen, k[k_i].byte, txt);
      CHECK(strcmp(txt, k[k_i].txt) == 0, k[k_i].what);
      gb_char_encode(k[k_i].gen, txt, &back);
      CHECK(back == k[k_i].byte, k[k_i].what);
    }
    printf("  the 15 glyphs the shipping readers lose are each spelled reversibly\n");
  }

  /* The escape must be a real escape: unreachable by accident, refused for 0x50. */
  {
    uint8_t b = 0;
    CHECK(gb_char_encode(GB_GEN2, "{5D}", &b) == 4 && b == 0x5Du, "{5D} encodes to 0x5D");
    CHECK(gb_char_encode(GB_GEN2, "{00}", &b) == 4 && b == 0x00u, "{00} encodes to 0x00");
    CHECK(gb_char_encode(GB_GEN2, "{5d}", &b) == 4 && b == 0x5Du, "lower-case hex is accepted");
    b = 0xAA;
    REFUSED(gb_char_encode(GB_GEN2, "{50}", &b) == 4 && b == 0x50u,
            "{50} must be refused — a terminator mid-name orphans the rest of the field");
    CHECK(gb_char_encode(GB_GEN2, "{ZZ}", &b) == 1 && b == 0x7Fu,
          "a malformed escape is just an unmappable character");
    /* The GB charset has no braces, so no name a player can enter contains one. */
    CHECK(gb_char_encode(GB_GEN1, "{", &b) == 1 && b == 0x7Fu, "a bare brace is a space");
  }

  /* And the disagreement with the shipping readers is deliberate — assert it, so nobody
   * "fixes" gb_name_decode back into agreement and silently reopens the hole. */
  {
    uint8_t f[GB_NAME_BYTES];
    char mine[GB_TEXT_MAX], theirs[GB_TEXT_MAX];
    memset(f, 0x50, sizeof f);
    f[0] = 0x5Du;
    gb_name_decode(GB_GEN1, mine, sizeof mine, f, GB_NAME_BYTES);
    gen1_decode_name(theirs, sizeof theirs, f, GB_NAME_BYTES);
    CHECK(strcmp(mine, "{5D}") == 0 && strcmp(theirs, "?") == 0,
          "gb_name_decode deliberately disagrees with gen1_decode_name on <TRAINER>");
    f[0] = 0xE1u;
    gb_name_decode(GB_GEN2, mine, sizeof mine, f, GB_NAME_BYTES);
    g2_decode_text(f, G2_NAME_CHARS, theirs, sizeof theirs);
    CHECK(strcmp(mine, "PK") == 0 && strcmp(theirs, " ") == 0,
          "...and with g2_decode_text on <PK>");
  }
}

/* ---- 5c. reversible per BYTE is not reversible per NAME ------------------- */

/* THE DEFECT THIS SECTION EXISTS FOR, and it is a different one from 5b. Section 5b proves
 * every single byte survives decode -> encode. That is necessary and NOT sufficient: seven
 * GB glyphs spell themselves with two ASCII characters, and the same two characters can be
 * two one-character glyphs standing next to each other. Both spell "PK"; an encoder can
 * only pick one. Before gb_name_decode escaped the collision, all nine two-byte sequences
 * below silently lost a byte — 'P','K' came back as the single glyph <PK>, shortening the
 * name and shifting everything after it.
 *
 * Exhaustively, not by example: every 2-byte and every 3-byte field the format can hold. */
static void test_sequence_reversibility(void) {
  int gen, a, b;
  head("a NAME must survive decode -> encode, not merely each byte of it");

  /* The nine specific pairs, named so a regression says which spelling broke. */
  {
    static const struct { uint8_t a, b; const char* what; } k[9] = {
      { 0x8Fu, 0x8Au, "'P' then 'K' must not collapse into the single <PK> glyph" },
      { 0x8Cu, 0x8Du, "'M' then 'N' must not collapse into <MN>" },
      { 0xE0u, 0xA3u, "an apostrophe then 'd' must not collapse into the 'd contraction" },
      { 0xE0u, 0xABu, "...nor 'l" }, { 0xE0u, 0xACu, "...nor 'm" },
      { 0xE0u, 0xB1u, "...nor 'r" }, { 0xE0u, 0xB2u, "...nor 's" },
      { 0xE0u, 0xB3u, "...nor 't" }, { 0xE0u, 0xB5u, "...nor 'v" },
    };
    size_t i;
    for (gen = GB_GEN1; gen <= GB_GEN2; gen++) {
      for (i = 0; i < 9; i++) {
        uint8_t src[2] = { k[i].a, k[i].b }, back[4];
        char txt[GB_TEXT_MAX];
        gb_name_decode((uint8_t)gen, txt, sizeof txt, src, 2);
        gb_name_encode((uint8_t)gen, back, 4, 10, txt);
        CHECK(back[0] == src[0] && back[1] == src[1], k[i].what);
      }
    }
  }

  /* The common glyph keeps the READABLE spelling and the rare literal pair takes the
   * escape — the trade-off is decided by Guy's own data (645 fields hold <PK>, none hold
   * a literal 'P','K'), so pin the direction or a future "cleanup" will silently flip it
   * and make 645 real OT names display as "Mattia{E1}". */
  {
    uint8_t f[GB_NAME_BYTES];
    char txt[GB_TEXT_MAX];
    memset(f, 0x50, sizeof f);
    f[0] = 0x8Cu; f[1] = 0xA0u; f[2] = 0xB3u; f[3] = 0xB3u;
    f[4] = 0xA8u; f[5] = 0xA0u; f[6] = 0xE1u;          /* "Mattia" + <PK>, from Gold.sav */
    gb_name_decode(GB_GEN2, txt, sizeof txt, f, GB_NAME_BYTES);
    CHECK(strcmp(txt, "MattiaPK") == 0,
          "a real OT name ending in <PK> stays readable — the glyph is NOT escaped");
    memset(f, 0x50, sizeof f);
    f[0] = 0x8Fu; f[1] = 0x8Au;                         /* the literal letters P, K */
    gb_name_decode(GB_GEN2, txt, sizeof txt, f, GB_NAME_BYTES);
    CHECK(strcmp(txt, "P{8A}") == 0, "the rare literal pair is what takes the escape");
  }

  /* EXHAUSTIVE. Every 2-byte field, both generations. */
  for (gen = GB_GEN1; gen <= GB_GEN2; gen++) {
    int bad = 0;
    for (a = 0; a < 256; a++) {
      if (a == 0x50) continue;
      for (b = 0; b < 256; b++) {
        uint8_t src[2], back[4];
        char txt[GB_TEXT_MAX];
        if (b == 0x50) continue;
        src[0] = (uint8_t)a; src[1] = (uint8_t)b;
        gb_name_decode((uint8_t)gen, txt, sizeof txt, src, 2);
        gb_name_encode((uint8_t)gen, back, 4, 10, txt);
        if (back[0] != src[0] || back[1] != src[1]) {
          if (bad < 4)
            printf("  !! gen %d: %02X %02X -> \"%s\" -> %02X %02X\n",
                   gen, a, b, txt, back[0], back[1]);
          bad++;
        }
      }
    }
    g_check++;
    if (bad) { printf("  !! FAIL: gen %d, %d two-byte sequences are not reversible\n",
                      gen, bad); g_fail++; }
    else printf("  gen %d: all %d two-byte name fields survive decode -> encode\n",
                gen, 255 * 255);
  }
}

/* ========================================================================== */
/* 6. every setter, verified by re-parsing with the shipping parser            */
/* ========================================================================== */

static uint8_t g_img[GBF_MAX_BYTES];

/* Gen-1 base data, transcribed from pokered's per-species base_stats files, for the six
 * species Guy's Red.sav party actually holds, plus Venusaur for the species-edit test.
 * Test data, not shipped code — gb_edit.c deliberately has no Gen-1 base-stat table. */
typedef struct { uint16_t dex; GbGen1Base b; } G1Row;
static const G1Row k_g1[] = {
  /*              HP  Atk  Def  Spe  Spc     type1 type2   (0x03 POISON, 0x02 FLYING,
   *                                                        0x14 FIRE, 0x15 WATER,
   *                                                        0x16 GRASS, 0x18 PSYCHIC,
   *                                                        0x1A DRAGON) */
  {   3, { {  80,  82,  83,  80, 100 }, 0x16, 0x03 } },   /* Venusaur   */
  {  25, { {  35,  55,  30,  90,  50 }, 0x17, 0x17 } },   /* Pikachu    */
  {   6, { {  78,  84,  78, 100,  85 }, 0x14, 0x02 } },   /* Charizard  */
  {   9, { {  79,  83, 100,  78,  85 }, 0x15, 0x15 } },   /* Blastoise  */
  { 149, { {  91, 134,  95,  80, 100 }, 0x1A, 0x02 } },   /* Dragonite  */
  { 150, { { 106, 110,  90, 130, 154 }, 0x18, 0x18 } },   /* Mewtwo     */
  { 151, { { 100, 100, 100, 100, 100 }, 0x18, 0x18 } },   /* Mew        */
};
static const GbGen1Base* g1_base_for(uint16_t dex) {
  size_t i;
  for (i = 0; i < sizeof k_g1 / sizeof k_g1[0]; i++) if (k_g1[i].dex == dex) return &k_g1[i].b;
  return 0;
}

/* Apply a battery of edits to slot 0 of `box`, commit, and prove with the SHIPPING
 * parser that every one of them landed where it was meant to. `crystal` only matters
 * for gen == GB_GEN2: whether this fixture's own save is Crystal (gb_set_caught may
 * write) or G/S (it must refuse -- BACKLOG #95 review C1, GbEditMon.has_caught). */
static void edit_and_verify(uint8_t gen, uint8_t* list, int box, const char* what,
                            bool crystal) {
  GbEditMon e, back;
  GbIssues iss;
  uint16_t new_dex = (gen == GB_GEN1) ? 3u : 251u;
  char nick[GB_TEXT_MAX], ot[GB_TEXT_MAX];
  int i;

  if (!gb_load(&e, gen, list, box, 0)) { CHECK(0, "load slot 0"); return; }
  CHECK(gb_roundtrip_ok(gen, list, box, 0), "load -> commit must be byte-identical");

  if (gen == GB_GEN2) {
    gb_set_caught_available(&e, crystal);
    /* Box 5 slot 0 of this fixture is an adversarial Egg (TOGEPI, GBF_F_EGG,
     * tests/gen12_fixture.c) -- gb_set_held_item now refuses a non-zero item on an
     * Egg (BACKLOG #95 review C5), so un-egg it first: this generic battery is
     * about every OTHER setter, and test_egg() below already covers the Egg case
     * on its own, including that it refuses an item. */
    if (gb_is_egg(&e))
      CHECK(gb_set_egg(&e, false), "un-egg for the generic setter battery");
  }

  CHECK(gb_set_species(&e, new_dex, g1_base_for(new_dex)), "set species");
  CHECK(gb_set_level(&e, 57), "set level");
  CHECK(gb_set_dv(&e, GB_ATK, 13), "set Atk DV");
  CHECK(gb_set_dv(&e, GB_DEF, 6),  "set Def DV");
  CHECK(gb_set_dv(&e, GB_SPE, 3),  "set Spe DV");
  CHECK(gb_set_dv(&e, GB_SPC, 9),  "set Spc DV");
  for (i = 0; i < GB_NSTATS; i++)
    CHECK(gb_set_statexp(&e, i, (uint16_t)(1000 + i * 777)), "set stat exp");
  CHECK(gb_set_move(&e, 0, 33), "move 0");
  CHECK(gb_set_move(&e, 1, 45), "move 1");
  CHECK(gb_set_move(&e, 2, 0),  "move 2 empty");
  CHECK(gb_set_move(&e, 3, 0),  "move 3 empty");
  CHECK(gb_set_ppup(&e, 1, 2), "2 PP Ups on move 1");
  CHECK(gb_set_pp(&e, 0, 7), "current PP on move 0");
  gb_set_otid(&e, 0xBEEF);
  CHECK(gb_set_nickname(&e, "EDITED\xE2\x99\x82"), "nickname");
  CHECK(gb_set_otname(&e, "GUY"), "OT name");
  if (gen == GB_GEN2) {
    CHECK(gb_set_held_item(&e, 0x2A), "held item");
    CHECK(gb_set_friendship(&e, 200), "friendship");
    CHECK(gb_set_pokerus(&e, 0x34), "pokerus");
    if (crystal)
      CHECK(gb_set_caught(&e, 2, 41, 12, 1), "caught data");
    else
      REFUSED(gb_set_caught(&e, 2, 41, 12, 1),
              "Gold/Silver: bytes 0x1D/0x1E are Unused1/Unused2, not a capture record");
    REFUSED(gb_set_gen1_base(&e, g1_base_for(3)), "Gen-1 base data on a Gen-2 record");
  } else {
    REFUSED(gb_set_held_item(&e, 5), "a Gen-1 record has no held item");
    REFUSED(gb_set_caught(&e, 1, 1, 1, 0), "a Gen-1 record has no caught data");
    REFUSED(gb_set_egg(&e, true), "Gen 1 has no eggs");
    CHECK(gb_set_gen1_catch_rate(&e, 45), "catch rate is settable, just never implicit");
  }
  if (e.is_party) CHECK(gb_recalc_stats(&e), "recalculate the party stats");

  CHECK(gb_check(&e, &iss), gb_issue_text(&iss) ? gb_issue_text(&iss) : "structural gate");
  /* THE DOCUMENTED WRITE PATH: commit and gate in one all-or-nothing move. */
  CHECK(gb_commit_checked(&e, list, box, 0), "gb_commit_checked");
  /* THE GATE on its own: re-read the slot with gen1_decode / g2_list_mon, which compute
   * the four parallel arrays' offsets themselves. */
  CHECK(gb_verify_slot(&e, list, box, 0), "the shipping parser must read back what was set");

  /* And independently, re-load through this module and compare the getters. */
  CHECK(gb_load(&back, gen, list, box, 0), "re-load");
  CHECK(gb_get_species_dex(&back) == new_dex, "species survived");
  CHECK(gb_get_level(&back) == 57, "level survived");
  CHECK(gb_get_exp(&back) == gb_exp_for_level(new_dex, 57), "EXP agrees with the level");
  CHECK(gb_get_dv(&back, GB_ATK) == 13 && gb_get_dv(&back, GB_SPC) == 9, "DVs survived");
  CHECK(gb_get_statexp(&back, GB_SPE) == 1000 + 3 * 777, "stat exp survived");
  CHECK(gb_get_move(&back, 1) == 45 && gb_get_ppup(&back, 1) == 2, "move and PP Ups survived");
  CHECK(gb_get_otid(&back) == 0xBEEF, "OT id survived");
  gb_get_nickname(&back, nick, sizeof nick);
  gb_get_otname(&back, ot, sizeof ot);
  CHECK(strcmp(nick, "EDITED\xE2\x99\x82") == 0, "nickname survived");
  CHECK(strcmp(ot, "GUY") == 0, "OT name survived");
  if (gen == GB_GEN2) {
    CHECK(gb_get_held_item(&back) == 0x2A, "held item survived");
    CHECK(back.rec[0x1B] == 200 && back.rec[0x1C] == 0x34, "friendship and pokerus survived");
    /* BACKLOG #95: the new read-only companions to gb_set_caught(2, 41, 12, 1) above --
     * same round trip, through the getters a new GBE_MET* row will actually call.
     * Crystal-only: the set itself was refused on G/S above, so there is nothing to
     * have survived (the getters still just read whatever bytes are there -- they
     * are not has_caught-gated, only the setter is -- but asserting specific values
     * against a refused write would be asserting fixture noise, not this module). */
    if (crystal) {
      CHECK(gb_get_caught_time(&back) == 2, "caught time survived");
      CHECK(gb_get_caught_level(&back) == 41, "caught level survived");
      CHECK(gb_get_caught_loc(&back) == 12, "caught location survived");
      CHECK(gb_get_caught_ot_gender(&back) == 1, "caught OT gender survived");
    }
  }
  printf("  %s: all setters verified through the shipping parser\n", what);
}

static void test_setters_on_fixture(void) {
  head("every setter, on synthetic saves full of adversarial junk");

  /* --- Gen 1 --- */
  {
    Gen1Save s;
    uint32_t n = gbf_build(GBF_RBY, g_img, 0);
    CHECK(gen1_open(g_img, n, &s) == GEN1_OK, "the fixture's R/B/Y save parses");
    edit_and_verify(GB_GEN1, g_img + gen1_list_offset(&s, gbf_current_box(GBF_RBY)),
                    gbf_current_box(GBF_RBY), "Gen 1, the live current box", false);
    edit_and_verify(GB_GEN1, g_img + GEN1_OFF_PARTY, GEN1_PARTY_BOX, "Gen 1, the party", false);
  }
  /* --- Gen 2, both versions --- */
  {
    int gi;
    const GbfGame games[2] = { GBF_GS, GBF_CRYSTAL };
    const char* nm[2] = { "Gen 2 (G/S)", "Gen 2 (Crystal)" };
    for (gi = 0; gi < 2; gi++) {
      G2Save sv; G2Header hd;
      uint32_t n = gbf_build(games[gi], g_img, 0);
      char buf[64];
      bool crystal = (games[gi] == GBF_CRYSTAL);
      CHECK(g2_detect(g_img, n, &sv), "the fixture's Gen-2 save parses");
      CHECK(g2_read_header(g_img, &sv, &hd), "read the Gen-2 header");
      snprintf(buf, sizeof buf, "%s, the live current box", nm[gi]);
      edit_and_verify(GB_GEN2, g_img + g2_list_offset(&sv, hd.current_box, hd.current_box),
                      hd.current_box, buf, crystal);
      snprintf(buf, sizeof buf, "%s, the party", nm[gi]);
      edit_and_verify(GB_GEN2, g_img + g2_list_offset(&sv, G2_BOX_PARTY, hd.current_box),
                      G2_BOX_PARTY, buf, crystal);
    }
  }
}

/* ========================================================================== */
/* 6b. the stat formula, pinned to hand-computed numbers                       */
/* ========================================================================== */

/* The real-save check below is ground truth but a blunt instrument: every Gen-2 party mon
 * Guy owns has all five stat-exp values at the 65535 cap, which hides the ceil-vs-floor
 * square root and makes Special-Defence's stat-exp input indistinguishable from
 * Defence's. So here are two records with a DIFFERENT stat exp per stat, chosen so the
 * square root lands just past a multiple of four, and the expected stats worked out by
 * hand from the decomp formula.
 *
 * stat exp 962 is the interesting one: ceil(sqrt(962)) = 32 -> bonus 8, but
 * floor(sqrt(962)) = 31 -> bonus 7, so the two spellings give different HP. */
static void test_stat_formula(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES], base[6];
  static const uint16_t k_sexp[GB_NSTATS] = { 962, 5000, 17223, 0, 40000 };
  /* Alakazam, from pokecrystal/data/pokemon/base_stats/alakazam.asm — every base stat
   * different, so a wrong index cannot hide. */
  static const uint8_t k_alakazam[6] = { 55, 50, 45, 120, 135, 85 };
  /* Mewtwo, from pokered/data/pokemon/base_stats/mewtwo.asm (one Special). */
  static const GbGen1Base k_mewtwo = { { 106, 110, 90, 130, 154 }, 0x18, 0x18 };
  static const uint16_t k_want_g2[6] = { 126, 75, 73, 128, 174, 124 };
  static const uint16_t k_want_g1[5] = { 134, 100, 86, 111, 141 };
  GbEditMon e;
  int i;
  head("the stat formula against hand-computed values (one stat exp per stat)");

  memset(nm, 0x50, sizeof nm);
  pk_base_stats(65, base);
  CHECK(memcmp(base, k_alakazam, 6) == 0,
        "pk_base_stats must equal pokecrystal's own numbers — the whole reuse claim");

  /* --- Gen 2, level 50, DVs 12/7/3/9 (HP DV = parity 0,1,1,1 = 7) --- */
  memset(rec, 0, sizeof rec);
  gb_load_parts(&e, GB_GEN2, true, rec, nm, nm, 0);
  CHECK(gb_set_species(&e, 65, 0), "Alakazam");
  CHECK(gb_set_level(&e, 50), "level 50");
  CHECK(gb_set_dv(&e, GB_ATK, 12) && gb_set_dv(&e, GB_DEF, 7)
     && gb_set_dv(&e, GB_SPE, 3)  && gb_set_dv(&e, GB_SPC, 9), "DVs");
  CHECK(gb_get_dv(&e, GB_HP) == 7, "derived HP DV");
  for (i = 0; i < GB_NSTATS; i++) CHECK(gb_set_statexp(&e, i, k_sexp[i]), "stat exp");
  CHECK(gb_recalc_stats(&e), "recalculate");
  for (i = 0; i < 6; i++) {
    if (gb_get_stat(&e, i) != k_want_g2[i]) {
      printf("  !! Gen-2 stat %d: got %u, hand-computed %u\n", i, gb_get_stat(&e, i), k_want_g2[i]);
      g_fail++;
    }
    g_check++;
  }
  printf("  Gen 2 Alakazam L50: %u %u %u %u %u %u (HP Atk Def Spe SpA SpD)\n",
         gb_get_stat(&e, 0), gb_get_stat(&e, 1), gb_get_stat(&e, 2),
         gb_get_stat(&e, 3), gb_get_stat(&e, 4), gb_get_stat(&e, 5));

  /* --- Gen 1, level 37, DVs 11/4/14/6 (HP DV = parity 1,0,0,0 = 8) --- */
  memset(rec, 0, sizeof rec);
  gb_load_parts(&e, GB_GEN1, true, rec, nm, nm, 0);
  CHECK(gb_set_species(&e, 150, &k_mewtwo), "Mewtwo");
  CHECK(gb_set_level(&e, 37), "level 37");
  CHECK(gb_set_dv(&e, GB_ATK, 11) && gb_set_dv(&e, GB_DEF, 4)
     && gb_set_dv(&e, GB_SPE, 14) && gb_set_dv(&e, GB_SPC, 6), "DVs");
  CHECK(gb_get_dv(&e, GB_HP) == 8, "derived HP DV");
  for (i = 0; i < GB_NSTATS; i++) CHECK(gb_set_statexp(&e, i, k_sexp[i]), "stat exp");
  CHECK(gb_recalc_stats(&e), "recalculate");
  for (i = 0; i < 5; i++) {
    if (gb_get_stat(&e, i) != k_want_g1[i]) {
      printf("  !! Gen-1 stat %d: got %u, hand-computed %u\n", i, gb_get_stat(&e, i), k_want_g1[i]);
      g_fail++;
    }
    g_check++;
  }
  printf("  Gen 1 Mewtwo L37: %u %u %u %u %u (HP Atk Def Spe Spc)\n",
         gb_get_stat(&e, 0), gb_get_stat(&e, 1), gb_get_stat(&e, 2),
         gb_get_stat(&e, 3), gb_get_stat(&e, 4));
  CHECK(e.rec[0x05] == 0x18 && e.rec[0x06] == 0x18,
        "the species edit wrote the Gen-1 types into the record");
  /* Without base data a Gen-1 recompute must refuse rather than guess. */
  {
    GbEditMon bare;
    memset(rec, 0, sizeof rec);
    gb_load_parts(&bare, GB_GEN1, true, rec, nm, nm, 0);
    bare.rec[0x00] = 131;                                /* Mewtwo's internal index */
    bare.rec[0x03] = 37; bare.rec[0x21] = 37;
    REFUSED(gb_recalc_stats(&bare), "a Gen-1 recompute with no base stats must refuse");
  }
}

/* ---- 6c. carry_hp, by name and by every branch --------------------------- */

/* The static gb_edit.c calls carry_hp() has five distinct behaviours and the test suite
 * never once named it. Each branch below is the rule the games themselves follow when a
 * mon's maximum HP changes under it (pokered/engine/pokemon/evos_moves.asm:186-203 adds
 * the delta rather than refilling; add_mon.asm:131 fills from the fresh maximum the
 * first time). An editor that got this wrong would silently heal, silently revive, or
 * leave current HP above the maximum — none of which a save can legally hold. */
static uint16_t cur_hp_g2(const GbEditMon* e) { return (uint16_t)((e->rec[0x22] << 8) | e->rec[0x23]); }

static void test_carry_hp(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES];
  GbEditMon e;
  uint16_t max50, max60, max20;
  head("carry_hp: what happens to current HP when the maximum moves under it");
  memset(nm, 0x50, sizeof nm);

  /* (1) FIRST EVER: a record whose stats have never been computed (old max 0) is filled
   *     from the fresh maximum, exactly as AddPartyMon does. */
  memset(rec, 0, sizeof rec);
  gb_load_parts(&e, GB_GEN2, true, rec, nm, nm, 0);
  CHECK(gb_set_species(&e, 65, 0) && gb_set_level(&e, 50), "Alakazam at 50");
  CHECK(gb_recalc_stats(&e), "first-ever stat computation");
  max50 = gb_get_stat(&e, 0);
  CHECK(max50 > 0, "it has a maximum");
  CHECK(cur_hp_g2(&e) == max50, "a first-ever computation fills current HP from the maximum");

  /* (2) FULL STAYS FULL: at full HP the delta rule lands exactly on the new maximum. */
  CHECK(gb_set_level(&e, 60) && gb_recalc_stats(&e), "level up while at full HP");
  max60 = gb_get_stat(&e, 0);
  CHECK(max60 > max50, "the maximum went up");
  CHECK(cur_hp_g2(&e) == max60, "a full-HP Pokemon stays full");

  /* (3) DAMAGED KEEPS ITS DAMAGE: the missing HP is preserved, not the ratio. */
  e.rec[0x22] = 0; e.rec[0x23] = 40;
  CHECK(gb_set_level(&e, 61) && gb_recalc_stats(&e), "level up while damaged");
  CHECK(cur_hp_g2(&e) == (uint16_t)(40 + (gb_get_stat(&e, 0) - max60)),
        "current HP moves by the max-HP delta, not by a heal");
  CHECK(cur_hp_g2(&e) < gb_get_stat(&e, 0), "and is still below the maximum");

  /* (4) FAINTED STAYS FAINTED: 0 HP is a state, and an edit must not revive. */
  e.rec[0x22] = 0; e.rec[0x23] = 0;
  CHECK(gb_set_level(&e, 70) && gb_recalc_stats(&e), "level up while fainted");
  CHECK(cur_hp_g2(&e) == 0, "a fainted Pokemon is never revived by an edit");

  /* (5) THE MAXIMUM DROPS. For a mon that was at full HP the delta rule lands on the new
   *     maximum by itself, so this branch on its own proves nothing — see (6). */
  e.rec[0x22] = 0; e.rec[0x23] = 1;
  CHECK(gb_set_level(&e, 71) && gb_recalc_stats(&e), "restore a living, nearly-dead mon");
  CHECK(cur_hp_g2(&e) >= 1, "1 HP does not become 0 on the way");
  {
    uint16_t before = gb_get_stat(&e, 0);
    e.rec[0x22] = (uint8_t)(before >> 8); e.rec[0x23] = (uint8_t)before;   /* full again */
    CHECK(gb_set_level(&e, 20) && gb_recalc_stats(&e), "level DOWN by 51");
    max20 = gb_get_stat(&e, 0);
    CHECK(max20 < before, "the maximum dropped a long way");
    CHECK(cur_hp_g2(&e) == max20, "a full-HP mon is still full after the maximum shrinks");
  }

  /* (6) THE CLAMP, isolated. cur + (new_max - old_max) only exceeds new_max when current
   *     HP was ALREADY above the stored maximum — an over-full record, which is exactly
   *     what a save arrives in when its stats were never recalculated after a species
   *     swap, or when it has been hacked. The delta rule alone cannot fix that, so the
   *     upper clamp is the only thing standing between the editor and a record no game
   *     can hold. (Without this case the clamp could be deleted and every other carry_hp
   *     assertion above would still pass — verified with a sabotage run.) */
  {
    uint16_t mx = gb_get_stat(&e, 0);
    uint16_t over = (uint16_t)(mx + 100);
    e.rec[0x22] = (uint8_t)(over >> 8); e.rec[0x23] = (uint8_t)over;
    CHECK(gb_recalc_stats(&e), "recalculate a record whose current HP is above its maximum");
    CHECK(gb_get_stat(&e, 0) == mx, "the maximum itself did not move");
    CHECK(cur_hp_g2(&e) == mx, "and current HP was clamped DOWN onto it, not left above");
  }

  /* (7) THE FLOOR, isolated. A living mon on 1 HP whose maximum collapses would go to
   *     zero or negative under the raw delta — and zero means fainted, which is a state
   *     an edit must never invent any more than it may cure one. */
  {
    CHECK(gb_set_level(&e, 100) && gb_recalc_stats(&e), "take it to level 100");
    e.rec[0x22] = 0; e.rec[0x23] = 1;                      /* alive, on 1 HP */
    CHECK(gb_set_level(&e, 5) && gb_recalc_stats(&e), "collapse the maximum to level 5");
    CHECK(cur_hp_g2(&e) == 1, "1 HP stays 1 HP: an edit may not faint a living Pokemon");
  }
  printf("  first-fill, full, damaged, fainted, shrinking maxima, the clamp and the floor\n");
}

/* An Egg's list byte is 0xFD while its record keeps the real species — the one case
 * where the two are allowed to disagree. */
static void test_egg(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES];
  GbEditMon e;
  GbIssues iss;
  head("Gen-2 Eggs: the list byte and the record deliberately disagree");
  memset(nm, 0x50, sizeof nm);
  mk_g2_rec(rec, 152, 5, 5, 5, 5, 5);
  gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 152);
  CHECK(gb_check(&e, &iss), "a plain Chikorita is structurally clean");
  CHECK(gb_set_egg(&e, true), "make it an Egg");
  CHECK(e.list_species == 0xFD && gb_get_species_raw(&e) == 152,
        "the list says Egg, the record still says Chikorita");
  CHECK(gb_check(&e, &iss), "and that is NOT a species mismatch");
  CHECK(gb_set_species(&e, 155, 0), "change the species of an Egg");
  CHECK(e.list_species == 0xFD, "which must not un-Egg it");
  CHECK(gb_set_egg(&e, false), "hatch it");
  CHECK(e.list_species == 155, "the list byte picks the record's species back up");
}

/* ========================================================================== */
/* 7. real cartridge saves                                                     */
/* ========================================================================== */

static uint8_t g_sav[64 * 1024];
static uint32_t load_file(const char* dir, const char* name, uint8_t* buf, uint32_t cap) {
  char p[512]; FILE* f; uint32_t n;
  snprintf(p, sizeof p, "%s/%s", dir, name);
  f = fopen(p, "rb");
  if (!f) return 0;
  n = (uint32_t)fread(buf, 1, cap, f);
  fclose(f);
  return n;
}

static int g_ran_real = 0;
/* running totals for the name round trip, across every save AND the 100 .pk2 files */
static int g_name_fields = 0, g_name_lost = 0;
/* THE COUNTER-FACTUAL, measured on the same bytes: how many of those fields the LOSSY
 * path would have damaged. This is what makes "0 of N" a result rather than a tautology —
 * if it ever reaches 0 too, the corpus stopped containing the glyphs and the headline
 * number proves nothing. See test_corpus_names_verdict. */
static int g_lossy_lost = 0;
/* ...and the same fields with mechanism 1 DELIBERATELY BYPASSED. The preserve-on-unchanged
 * fast path compares through the very decoder under test, so it reports "nothing changed"
 * even when that decoder is broken — it masks the bug it sits in front of. A player who
 * actually edits a name gets no such protection: the field is re-encoded from text, and
 * every OTHER glyph in it has to survive that.
 *
 * Verified by breaking the decoder on purpose: spell the {XX} escape as a space again, the
 * way the shipping readers do, and this counter goes to 11 (the 9 <TRAINER> bytes and 2
 * decimal points Guy's Red.sav really holds) while g_name_lost stays 0 — the fast path
 * hides it completely. The OTHER half of the codec fix, the sequence escape, has ZERO
 * occurrences in this corpus and cannot be caught here at all; that one is defended by the
 * exhaustive sweep in test_sequence_reversibility, which is why both tests exist. */
static int g_edit_lost = 0;

/* THE ROUND TRIP AN EDITING UI PERFORMS, on one loaded record: read both names out as
 * text, hand the same text straight back, and require every one of the 22 bytes to be
 * where it was. This is the check that was failing on 36.7% of Guy's name fields. */
static void name_cycle(const GbEditMon* e, const char* tag, int box, int slot) {
  GbEditMon w = *e;
  char txt[GB_TEXT_MAX];
  gb_get_nickname(e, txt, sizeof txt);
  if (!gb_set_nickname(&w, txt)) { CHECK(0, "gb_set_nickname refused its own output"); return; }
  gb_get_otname(e, txt, sizeof txt);
  if (!gb_set_otname(&w, txt)) { CHECK(0, "gb_set_otname refused its own output"); return; }
  g_name_fields += 2;
  if (memcmp(w.nick, e->nick, GB_NAME_BYTES) != 0) {
    if (g_name_lost < 4)
      printf("  !! %s box %d slot %d: nickname changed by a no-op edit\n", tag, box, slot);
    g_name_lost++;
  }
  if (memcmp(w.otname, e->otname, GB_NAME_BYTES) != 0) {
    if (g_name_lost < 4)
      printf("  !! %s box %d slot %d: OT name changed by a no-op edit\n", tag, box, slot);
    g_name_lost++;
  }

  /* THE REAL EDIT. Decode the field and re-encode it with no fast path in the way, which
   * is what happens the moment the player changes any character: everything they did NOT
   * change still has to come back byte for byte. Only the LIVE bytes are compared — up to
   * and including the 0x50 terminator — because a genuine edit legitimately 0x50-fills the
   * junk tail (gb_name_encode says so), and that tail is what mechanism 1 protects. */
  {
    int which;
    for (which = 0; which < 2; which++) {
      const uint8_t* f = which ? e->otname : e->nick;
      uint8_t back[GB_NAME_BYTES];
      char txt[GB_TEXT_MAX];
      int live = GB_NAME_BYTES, i;
      for (i = 0; i < GB_NAME_BYTES; i++) if (f[i] == 0x50u) { live = i + 1; break; }
      gb_name_decode(e->gen, txt, sizeof txt, f, GB_NAME_BYTES);
      gb_name_encode(e->gen, back, GB_NAME_BYTES,
                     which ? GB_OT_GLYPHS : GB_NICK_GLYPHS, txt);
      if (memcmp(back, f, (size_t)live) != 0) {
        if (g_edit_lost < 4)
          printf("  !! %s box %d slot %d: a REAL edit would rewrite the %s\n",
                 tag, box, slot, which ? "OT name" : "nickname");
        g_edit_lost++;
      }
    }
  }

  /* Now the same cycle through the SHIPPING readers — the exact path this module was
   * written to replace. gen1_decode_name spells <TRAINER> "?" and é "e"; g2_decode_text
   * has no case for <PK>/<MN> at all and flattens the umlauts and the decimal point. Feed
   * their output to the encoder and the field is rewritten. */
  {
    GbEditMon v = *e;
    char theirs[GB_TEXT_MAX];
    if (e->gen == GB_GEN1) gen1_decode_name(theirs, sizeof theirs, e->nick, GB_NAME_BYTES);
    else                   g2_decode_text(e->nick, G2_NAME_CHARS, theirs, sizeof theirs);
    gb_set_nickname_lossy(&v, theirs);
    if (memcmp(v.nick, e->nick, GB_NAME_BYTES) != 0) g_lossy_lost++;

    v = *e;
    if (e->gen == GB_GEN1) gen1_decode_name(theirs, sizeof theirs, e->otname, GB_NAME_BYTES);
    else                   g2_decode_text(e->otname, G2_NAME_CHARS, theirs, sizeof theirs);
    gb_set_otname_lossy(&v, theirs);
    if (memcmp(v.otname, e->otname, GB_NAME_BYTES) != 0) g_lossy_lost++;
  }
}

static void name_roundtrip(uint8_t gen, const uint8_t* list, int box, int slot,
                           const char* file) {
  GbEditMon e;
  if (!gb_load(&e, gen, list, box, slot)) return;
  name_cycle(&e, file, box, slot);
}

/* The stat formula, transcribed HERE straight from pokered/home/move_mon.asm:54 (CalcStat)
 * and pokecrystal/engine/pokemon/move_mon.asm:1424 (CalcMonStatC) — a fourth independent
 * copy, so agreeing with gb_edit.c means something. */
static uint8_t t_ceil_sqrt(uint16_t v) { uint32_t b = 0; while (b < 255u) { b++; if (b*b >= v) break; } return (uint8_t)b; }
static uint16_t t_stat(uint8_t base, uint8_t dv, uint8_t bonus, uint8_t lvl, int is_hp) {
  uint32_t v = (((uint32_t)base + dv) * 2u + bonus) * (uint32_t)lvl / 100u;
  v += is_hp ? ((uint32_t)lvl + 10u) : 5u;
  return v > 999u ? 999u : (uint16_t)v;
}

/* WHY A STORED PARTY STAT CAN LAG ITS STAT EXP, AND WHAT THAT LETS US ASSERT.
 *
 * Gen 1 and Gen 2 only recalculate a party mon's stats on level-up or when it comes back
 * out of the PC — a mon that keeps battling at level 100 keeps banking stat exp that its
 * stored stats do not yet reflect. (That gap IS the Gen-1 "box trick": deposit and
 * withdraw and the stats jump.) Guy's Red.sav shows it: 4 of the 6 party mons reproduce
 * byte-for-byte, and the other two — Dragonite's Special and every Charizard stat — sit
 * exactly where the formula puts them for a SMALLER stat-exp value than the one now
 * stored. So "stored == recomputed" is not a law of the format.
 *
 * What IS a law: the stored stat must lie between the value with no stat-exp bonus at all
 * and the value with the bonus the record currently holds, because the only term that can
 * have drifted is the stat-exp one and stat exp only ever grows. That brackets the base
 * stat, the DV, the level, the /100 and the +5 / +level+10 terms exactly — get any of
 * them wrong by one and the bracket collapses. Returns 1 if the stat is bracketed, and
 * sets *exact when the recomputation reproduces the cartridge's byte outright. */
static int stat_bracketed(uint16_t stored, uint8_t base, uint8_t dv, uint16_t statexp,
                          uint8_t lvl, int is_hp, int* exact) {
  uint16_t lo = t_stat(base, dv, 0, lvl, is_hp);
  uint16_t hi = t_stat(base, dv, (uint8_t)(t_ceil_sqrt(statexp) >> 2), lvl, is_hp);
  *exact = (stored == hi);
  return stored >= lo && stored <= hi;
}

static void real_gen1(const char* file) {
  Gen1Save s;
  int box, slot, records = 0, rt = 0, clean = 0, statchk = 0, statok = 0;
  uint32_t n = load_file(ROMSGB, file, g_sav, sizeof g_sav);
  if (!n) { printf("  SKIP %s\n", file); return; }
  g_ran_real++;
  CHECK(gen1_open(g_sav, n, &s) == GEN1_OK, "the real Gen-1 save parses");

  for (box = 0; box <= GEN1_NUM_BOXES; box++) {
    uint32_t off = gen1_list_offset(&s, box);
    int cnt = gen1_count(&s, box);
    if (!off || cnt <= 0) continue;
    for (slot = 0; slot < cnt; slot++) {
      GbEditMon e; GbIssues iss;
      records++;
      if (gb_roundtrip_ok(GB_GEN1, g_sav + off, box, slot)) rt++;
      name_roundtrip(GB_GEN1, g_sav + off, box, slot, file);
      if (!gb_load(&e, GB_GEN1, g_sav + off, box, slot)) continue;
      if (gb_check(&e, &iss)) clean++;
      /* GROUND TRUTH FOR THE STAT FORMULA: for the party species whose Gen-1 base stats
       * are transcribed above, check the cartridge's own stored stats against it. */
      if (e.is_party) {
        const GbGen1Base* b = g1_base_for(gb_get_species_dex(&e));
        if (b) {
          int st, allexact = 1;
          uint8_t lvl = gb_get_level(&e);
          statchk++;
          for (st = 0; st < GB_NSTATS; st++) {
            int exact = 0;
            if (!stat_bracketed(gb_get_stat(&e, st), b->base[st], gb_get_dv(&e, st),
                                gb_get_statexp(&e, st), lvl, st == GB_HP, &exact)) {
              printf("  !! %s party %d stat %d: stored %u is outside the formula's range\n",
                     file, slot, st, gb_get_stat(&e, st));
              g_fail++; g_check++;
            }
            if (!exact) allexact = 0;
          }
          if (allexact) statok++;
        }
      }
    }
  }
  printf("  %-18s %d records, %d round-tripped, %d structurally clean, "
         "%d/%d party mons reproduce their stats exactly (the rest lag their stat exp)\n",
         file, records, rt, clean, statok, statchk);
  CHECK(records > 0, "a played save has Pokemon in it");
  CHECK(rt == records, "EVERY real record must survive load -> commit unchanged");
  CHECK(clean == records, "every real Gen-1 record must pass the structural gate");
  CHECK(statchk > 0, "the party has species whose Gen-1 base stats this test knows");
  CHECK(statok > 0, "at least one real Gen-1 party mon must reproduce ALL its stats exactly");
}

static void real_gen2(const char* file) {
  G2Save sv; G2Header hd;
  int box, slot, records = 0, rt = 0, clean = 0, statchk = 0, statok = 0;
  uint32_t n = load_file(ROMSGB, file, g_sav, sizeof g_sav);
  if (!n) { printf("  SKIP %s\n", file); return; }
  g_ran_real++;
  CHECK(g2_detect(g_sav, n, &sv), "the real Gen-2 save is supported");
  CHECK(g2_read_header(g_sav, &sv, &hd), "read its header");

  for (box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    int cnt;
    if (!off) continue;
    cnt = g2_list_count(g_sav + off, box);
    for (slot = 0; slot < cnt; slot++) {
      GbEditMon e, w; GbIssues iss;
      uint8_t before[GB_MAX_REC];
      records++;
      if (gb_roundtrip_ok(GB_GEN2, g_sav + off, box, slot)) rt++;
      name_roundtrip(GB_GEN2, g_sav + off, box, slot, file);
      if (!gb_load(&e, GB_GEN2, g_sav + off, box, slot)) continue;
      if (gb_check(&e, &iss)) clean++;
      if (e.is_party) {
        /* Gen 2 needs no transcribed base stats: pk_base_stats() is claimed to be the
         * same data pokecrystal holds, and demanding the cartridge's own party stats
         * back is what makes that claim testable rather than asserted. */
        statchk++;
        w = e;
        memcpy(before, e.rec, GB_MAX_REC);
        if (gb_recalc_stats(&w) && memcmp(w.rec, before, w.rec_len) == 0) statok++;
        else {
          uint8_t base[6], lvl = gb_get_level(&e);
          int st, bad = 0;
          static const int k_in[6] = { GB_HP, GB_ATK, GB_DEF, GB_SPE, GB_SPC, GB_SPC };
          pk_base_stats(gb_get_species_dex(&e), base);
          for (st = 0; st < 6; st++) {
            int exact = 0;
            if (!stat_bracketed(gb_get_stat(&e, st), base[st], gb_get_dv(&e, k_in[st]),
                                gb_get_statexp(&e, k_in[st]), lvl, st == 0, &exact)) bad++;
          }
          if (bad) { printf("  !! %s party %d: %d stats outside the formula's range\n",
                            file, slot, bad); g_fail++; g_check++; }
        }
      }
    }
  }
  printf("  %-18s %d records, %d round-tripped, %d structurally clean, "
         "%d/%d party mons reproduce their stats exactly\n",
         file, records, rt, clean, statok, statchk);
  CHECK(records > 0, "a played save has Pokemon in it");
  CHECK(rt == records, "EVERY real record must survive load -> commit unchanged");
  CHECK(clean == records, "every real Gen-2 record must pass the structural gate");
  CHECK(statchk > 0 && statok == statchk,
        "the Gen-2 stat formula must reproduce the cartridge's own stored stats");
}

/* ========================================================================== */
/* 8. the 100 real .pk2 files — ground truth for the Unown letter              */
/* ========================================================================== */

/* A .pk2 is a one-slot Gen-2 PARTY list: count(1) + species list(2, 0xFF-terminated) +
 * a 48-byte record + an 11-byte OT name + an 11-byte nickname = 73 bytes. Verified by
 * decoding one: species 0xC9 = 201 Unown, move 0xED = 237 Hidden Power at 15 PP,
 * friendship 70, level 5 — every field lands where the Gen-2 record layout says. */
#define PK2_SIZE 73

static void test_pk2_corpus(void) {
  DIR* d;
  struct dirent* de;
  char path[1024];
  int files = 0, matched = 0, badsize = 0;
  int letters_seen[26];
  head("100 real .pk2 Unown, whose filenames name the letter this code has to derive");
  memset(letters_seen, 0, sizeof letters_seen);
  snprintf(path, sizeof path, "%s/pk2", ROMSGB);
  d = opendir(path);
  if (!d) { printf("  SKIP (%s not present)\n", path); return; }

  while ((de = readdir(d)) != 0) {
    uint8_t buf[PK2_SIZE + 8];
    GbEditMon e;
    GbDvEffects fx;
    const char* tag;
    char want;
    uint32_t n;
    if (!strstr(de->d_name, ".pk2")) continue;
    files++;
    n = load_file(path, de->d_name, buf, sizeof buf);
    if (n != PK2_SIZE) { badsize++; continue; }
    /* the letter the FILENAME asserts, e.g. "Gold-0201-B - UNOWN - 0F04.pk2" */
    tag = strstr(de->d_name, "0201-");
    if (!tag) continue;
    want = tag[5];

    gb_load_parts(&e, GB_GEN2, true, buf + 3, buf + 3 + 48, buf + 3 + 48 + 11, buf[1]);
    /* These 200 name fields belong in the round-trip count too — a .pk2 is a real Pokemon
     * out of a real cartridge, and its OT field is where the <PK> glyph actually lives. */
    name_cycle(&e, "pk2", 0, 0);
    if (gb_get_species_dex(&e) != 201) { CHECK(0, "a .pk2 must decode as Unown"); continue; }
    gb_dv_effects_of(&e, &fx);
    if (fx.unown && fx.unown_letter == (want - 'A')) {
      matched++;
      letters_seen[want - 'A']++;
    } else {
      printf("  !! %s: derived letter %c, filename says %c\n",
             de->d_name, (char)('A' + fx.unown_letter), want);
      g_fail++;
    }
    g_check++;
  }
  closedir(d);
  {
    int distinct = 0, i;
    for (i = 0; i < 26; i++) if (letters_seen[i]) distinct++;
    printf("  %d files, %d wrong size, %d letters derived correctly, %d distinct letters\n",
           files, badsize, matched, distinct);
    CHECK(files == 100, "the corpus is 100 files");
    CHECK(badsize == 0, "every .pk2 is the 73-byte one-slot list this test assumes");
    CHECK(matched == files, "every Unown's letter must come out of its DVs");
    CHECK(distinct >= 25, "the corpus covers essentially the whole alphabet");
  }
}

/* ========================================================================== */
/* 8b. THE HEADLINE NUMBER — every name field of every real save AND every .pk2 */
/* ========================================================================== */

/* THE PROPERTY gb_edit.h PROMISES: get followed by set, with no edit in between, changes
 * zero bytes — for every name in every slot of every one of Guy's real saves.
 *
 * A bare "0 damaged" is not evidence on its own: a corpus with no interesting glyphs, or
 * a decoder that returned "" for everything, would score 0 as well. So the same fields are
 * ALSO put through the lossy path this module replaced, and that number has to be LARGE.
 * Two counters over one set of bytes: one must be 0, the other must not be. */
static void test_corpus_names_verdict(void) {
  head("the get -> set round trip over every real name field Guy owns");
  if (!g_ran_real) { printf("  SKIP (no corpus present)\n"); return; }
  printf("  %d name fields put through the get -> set cycle a UI performs\n", g_name_fields);
  printf("    through gb_edit's own codec ..... %d damaged\n", g_name_lost);
  printf("    ...with the no-op fast path off .. %d damaged  (a REAL edit)\n", g_edit_lost);
  printf("    through the shipping readers .... %d damaged  (%d%%)\n",
         g_lossy_lost, g_name_fields ? g_lossy_lost * 100 / g_name_fields : 0);
  CHECK(g_name_fields > 1700, "the corpus really does have thousands of name fields");
  CHECK(g_name_lost == 0,
        "NO real name field may be changed by reading it out and handing it back");
  CHECK(g_edit_lost == 0,
        "and every OTHER glyph must survive a name the player really does edit — the "
        "check the preserve-on-unchanged fast path cannot make for itself");
  /* The negative control, on real data: if this ever drops to 0 the corpus has stopped
   * exercising the bug and the line above has stopped meaning anything. */
  CHECK(g_lossy_lost > 500,
        "...and the lossy path must still visibly damage the same fields, or the "
        "headline number is measuring nothing");
}

/* ========================================================================== */
/* 9. NEGATIVE CONTROLS — break things on purpose                              */
/* ========================================================================== */

/* Re-set every occupied move to itself, which resets its PP to that move's real base. */
static void normalise_pp(GbEditMon* e) {
  int i;
  for (i = 0; i < 4; i++) gb_set_move(e, i, gb_get_move(e, i));
}

static void test_negative_controls(void) {
  Gen1Save s;
  uint32_t n;
  uint8_t* list;
  int box;
  GbEditMon e, e2;
  GbIssues iss;
  head("negative controls: every check below MUST notice the damage");

  n = gbf_build(GBF_RBY, g_img, 0);
  CHECK(gen1_open(g_img, n, &s) == GEN1_OK, "fixture save parses");
  box  = gbf_current_box(GBF_RBY);
  list = g_img + gen1_list_offset(&s, box);

  /* (0) the fixture is itself a negative control we did not plant: it writes a flat 30 PP
   *     on every move regardless of that move's base PP, so its records are structurally
   *     impossible and the gate says so. Everything below therefore starts by
   *     normalising the PP, or it would be testing the fixture's flaw instead. */
  CHECK(gb_load(&e, GB_GEN1, list, box, 0), "load a fixture record");
  REFUSED(gb_check(&e, &iss), "the fixture's flat 30 PP must be caught");
  CHECK(iss.pp_over, "reported as PP over the maximum");
  normalise_pp(&e);
  CHECK(gb_check(&e, &iss), "and it is clean once the PP is legal");
  CHECK(gb_commit(&e, list, box, 0), "write the normalised baseline back");

  /* (a) a byte flipped after the commit must break the re-parse gate */
  CHECK(gb_load(&e, GB_GEN1, list, box, 0), "load");
  CHECK(gb_set_level(&e, 40) && gb_commit(&e, list, box, 0), "edit and commit");
  CHECK(gb_verify_slot(&e, list, box, 0), "the gate passes on an honest write");
  list[gb_off_record(GB_GEN1, box, 0) + 0x1B] ^= 0x10;      /* corrupt one DV nibble */
  REFUSED(gb_verify_slot(&e, list, box, 0), "a flipped DV nibble must fail the gate");
  list[gb_off_record(GB_GEN1, box, 0) + 0x1B] ^= 0x10;
  CHECK(gb_verify_slot(&e, list, box, 0), "and pass again once it is put back");

  /* (b) the OFFSET arithmetic is what the gate really protects: write the names one slot
   *     over and the parser attaches them to the wrong Pokemon */
  CHECK(gb_set_nickname(&e, "MARKER") && gb_commit(&e, list, box, 0), "rename slot 0");
  CHECK(gb_verify_slot(&e, list, box, 0), "gate passes");
  memcpy(list + gb_off_nickname(GB_GEN1, box, 0),
         list + gb_off_nickname(GB_GEN1, box, 1), GB_NAME_BYTES);
  REFUSED(gb_verify_slot(&e, list, box, 0),
          "a nickname written into the wrong slot must fail the gate");

  /* (c) level and EXP forced out of agreement */
  CHECK(gb_load(&e, GB_GEN1, list, box, 0), "reload");
  normalise_pp(&e);
  CHECK(gb_set_level(&e, 30), "level 30");
  CHECK(gb_check(&e, &iss), "clean at level 30");
  e.rec[0x03] = 31;                                        /* level byte only */
  REFUSED(gb_check(&e, &iss), "a level that disagrees with the EXP must be caught");
  CHECK(iss.level_exp_bad, "and it must be reported as exactly that");

  /* (d) the growth curve matters: EXP computed on the WRONG curve must be rejected.
   *     Bulbasaur (3) is Medium Slow, Caterpie (10) is Medium Fast. */
  CHECK(gb_growth_rate(3) != gb_growth_rate(10), "those two species differ in curve");
  CHECK(gb_load(&e2, GB_GEN1, list, box, 0), "reload");
  normalise_pp(&e2);
  CHECK(gb_set_species(&e2, 3, g1_base_for(3)) && gb_set_level(&e2, 44), "Venusaur at 44");
  CHECK(gb_check(&e2, &iss), "clean");
  {
    uint32_t wrong = pk_exp_for_level(pk_species_growth(10), 44);
    e2.rec[0x0E] = (uint8_t)(wrong >> 16);
    e2.rec[0x0F] = (uint8_t)(wrong >> 8);
    e2.rec[0x10] = (uint8_t)wrong;
    REFUSED(gb_check(&e2, &iss), "EXP taken from the wrong growth curve must be caught");
  }

  /* (e) structural nonsense in the moves */
  CHECK(gb_load(&e, GB_GEN1, list, box, 0), "reload");
  normalise_pp(&e);
  CHECK(gb_set_move(&e, 0, 0) || 1, "empty the first move slot");
  gb_set_move(&e, 1, 33);
  REFUSED(gb_check(&e, &iss), "an empty first move slot must be caught");
  CHECK(iss.move_empty && iss.move_hole, "reported as both an empty slot and a hole");
  CHECK(gb_set_move(&e, 0, 33) && gb_set_move(&e, 1, 33), "the same move twice");
  REFUSED(gb_check(&e, &iss), "a duplicated move must be caught");

  /* (f) PP beyond the maximum, written behind the setter's back */
  CHECK(gb_load(&e, GB_GEN1, list, box, 0), "reload");
  CHECK(gb_set_move(&e, 0, 45), "Growl, 40 PP");
  e.rec[0x1D] = 62;                                        /* PP 62, no PP Ups */
  REFUSED(gb_check(&e, &iss), "PP above the move's maximum must be caught");
  CHECK(iss.pp_over, "reported as PP over the maximum");

  /* (g) a species change with no Gen-1 base data must be REFUSED, not guessed */
  CHECK(gb_load(&e, GB_GEN1, list, box, 0), "reload");
  {
    uint8_t before[GB_MAX_REC];
    memcpy(before, e.rec, GB_MAX_REC);
    REFUSED(gb_set_species(&e, 6, 0), "Gen-1 species change without types must be refused");
    CHECK(memcmp(before, e.rec, GB_MAX_REC) == 0, "and must have changed nothing at all");
  }

  /* (h) stale party stats must be visible, not silent */
  {
    G2Save sv; G2Header hd;
    uint32_t m = gbf_build(GBF_CRYSTAL, g_img, 0);
    CHECK(g2_detect(g_img, m, &sv) && g2_read_header(g_img, &sv, &hd), "fixture Crystal save");
    CHECK(gb_load(&e, GB_GEN2, g_img + g2_list_offset(&sv, G2_BOX_PARTY, hd.current_box),
                  G2_BOX_PARTY, 0), "load a party mon");
    normalise_pp(&e);
    CHECK(gb_recalc_stats(&e), "start from stats that match the record");
    CHECK(gb_set_dv(&e, GB_ATK, 1), "change a DV");
    REFUSED(gb_check(&e, &iss), "a stat-affecting edit without a recalc must be flagged");
    CHECK(iss.stats_stale, "reported as stale stats");
    CHECK(gb_recalc_stats(&e), "recalculate");
    CHECK(gb_check(&e, &iss), "clean afterwards");
  }

  /* (i) a Gen-1 record must never be edited through a Gen-2 API, and vice versa */
  n = gbf_build(GBF_RBY, g_img, 0);
  CHECK(gen1_open(g_img, n, &s) == GEN1_OK, "rebuild the fixture save");
  list = g_img + gen1_list_offset(&s, box);
  CHECK(gb_load(&e, GB_GEN1, list, box, 0), "reload a Gen-1 mon");
  REFUSED(gb_set_friendship(&e, 200), "friendship does not exist in Gen 1");
  REFUSED(gb_set_pokerus(&e, 1), "pokerus does not exist in Gen 1");
  REFUSED(gb_commit(&e, list, GEN1_PARTY_BOX, 0), "a box record must not commit into the party");

  /* (j) the fixture itself must be able to fail. Feed the wrong generation's charset. */
  {
    uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES];
    GbEditMon g1, g2;
    memset(nm, 0x50, sizeof nm);
    mk_g2_rec(rec, 1, 20, 5, 5, 5, 5);
    gb_load_parts(&g1, GB_GEN1, false, rec, nm, nm, 1);
    gb_load_parts(&g2, GB_GEN2, false, rec, nm, nm, 1);
    gb_set_nickname(&g1, "don't");
    gb_set_nickname(&g2, "don't");
    REFUSED(memcmp(g1.nick, g2.nick, GB_NAME_BYTES) == 0,
            "the two generations must NOT encode the same contraction to the same byte");
  }

  /* (k) the parts API must not half-write on a bad generation. gb_commit_parts used to
   *     return early leaving *list_species untouched, which arm-none-eabi-gcc flagged as
   *     a -Wmaybe-uninitialized read inside gb_roundtrip_ok. */
  {
    GbEditMon bad;
    uint8_t rec[GB_MAX_REC], nmf[GB_NAME_BYTES], sp = 0xAA;
    memset(rec, 0x11, sizeof rec); memset(nmf, 0x22, sizeof nmf);
    REFUSED(gb_load_parts(&bad, 3, false, rec, nmf, nmf, 7), "gen 3 is not a Game Boy game");
    REFUSED(gb_commit_parts(&bad, rec, nmf, nmf, &sp), "and it cannot be committed either");
    CHECK(sp == 0xAA, "a refused gb_commit_parts must leave the out-parameter untouched");
    CHECK(rec[0] == 0x11 && nmf[0] == 0x22, "and must not have written the buffers");
  }
}

/* ========================================================================== */
/* 10. the 0xFF terminator: the byte gb_commit is not allowed to reach         */
/* ========================================================================== */

/* THE DEFECT THIS SECTION EXISTS FOR. gb_commit wrote list[species_slot] unconditionally.
 * For any box that is not full, the first free slot's species byte IS the 0xFF
 * terminator, so committing into it left the count saying N while the list described
 * N+1 — and Gen 2's own reader then refuses to open the box at all (g2_list_count
 * returns -1, gen2_save.c:334). Both games instead grow a list as one indivisible move:
 * bump the count, write the species where the terminator was, put the terminator back
 * one slot along (pokered/engine/pokemon/add_mon.asm:12-28, pokecrystal/engine/pokemon/
 * move_mon.asm:14-35 — "The terminator is usually here, but it'll be back"). */
static void test_terminator(uint8_t gen, uint8_t* list, int box, const char* what) {
  GbEditMon e;
  int cnt = gb_list_count(gen, list, box);
  int cap = gb_list_capacity(gen, box);
  int term = gb_off_terminator(gen, list, box);
  uint8_t whole[MAX_LIST_BYTES];
  int size = gb_list_size(gen, box);

  if (cnt < 1 || cnt >= cap) { printf("  SKIP %s (count %d of %d)\n", what, cnt, cap); return; }
  CHECK(term == gb_off_species(gen, box, cnt), "the terminator sits at species index count");
  CHECK(list[term] == 0xFF, "and it really is 0xFF");
  memcpy(whole, list, (size_t)size);

  /* Loading the append slot is fine — reading never hurts, and the write modules need it. */
  CHECK(gb_load(&e, gen, list, box, cnt), "the append slot can still be LOADED");

  /* Committing into it must be refused, and must have written nothing at all.
   * Pikachu is dex 25 in both generations; g1_base_for supplies pokered's own numbers. */
  CHECK(gb_set_species(&e, 25, g1_base_for(25)), "fill the loaded slot with a Pikachu");
  REFUSED(gb_commit(&e, list, box, cnt), "gb_commit into the append slot must be refused");
  REFUSED(gb_commit_checked(&e, list, box, cnt), "and so must gb_commit_checked");
  CHECK(list[term] == 0xFF, "THE TERMINATOR MUST STILL BE THERE");
  CHECK(gb_list_count(gen, list, box) == cnt, "and the count must still read back");
  CHECK(memcmp(whole, list, (size_t)size) == 0, "a refused commit changes NOT ONE BYTE");

  /* A slot past the append slot is refused too — it is not free space to scribble in. */
  if (cnt + 1 < cap)
    REFUSED(gb_commit(&e, list, box, cnt + 1), "nor may a slot beyond the append slot");

  /* Now the append the WRITE MODULE performs, in the games' own order: count first, then
   * the species where the terminator was, then the terminator one slot along. Once the
   * slot is occupied, gb_commit is exactly the right tool for it. */
  list[0] = (uint8_t)(cnt + 1);
  list[gb_off_species(gen, box, cnt)] = gb_get_species_raw(&e);
  list[term + 1] = 0xFF;
  CHECK(gb_list_count(gen, list, box) == cnt + 1, "the list now holds one more");
  CHECK(gb_commit(&e, list, box, cnt), "and NOW the slot commits");
  CHECK(gb_verify_slot(&e, list, box, cnt), "and passes the gate");
  CHECK(list[gb_off_terminator(gen, list, box)] == 0xFF, "the terminator moved with it");

  /* The terminator's own value must never be written into an occupied slot either: an
   * 0xFF mid-list truncates the box for the game's own walker. */
  e.list_species = 0xFF;
  REFUSED(gb_commit(&e, list, box, cnt), "a list species of 0xFF must be refused");

  memcpy(list, whole, (size_t)size);          /* leave the blob as we found it */
  printf("  %s: terminator protected, and a real append still works\n", what);
}

static void test_terminators(void) {
  head("the 0xFF species-list terminator");
  {
    Gen1Save s;
    uint32_t n = gbf_build(GBF_RBY, g_img, 0);
    CHECK(gen1_open(g_img, n, &s) == GEN1_OK, "fixture R/B/Y");
    test_terminator(GB_GEN1, g_img + gen1_list_offset(&s, gbf_current_box(GBF_RBY)),
                    gbf_current_box(GBF_RBY), "Gen 1, a box");
  }
  {
    G2Save sv; G2Header hd;
    uint32_t n = gbf_build(GBF_CRYSTAL, g_img, 0);
    CHECK(g2_detect(g_img, n, &sv) && g2_read_header(g_img, &sv, &hd), "fixture Crystal");
    test_terminator(GB_GEN2, g_img + g2_list_offset(&sv, hd.current_box, hd.current_box),
                    hd.current_box, "Gen 2, a box");
    test_terminator(GB_GEN2, g_img + g2_list_offset(&sv, G2_BOX_PARTY, hd.current_box),
                    G2_BOX_PARTY, "Gen 2, the party");
  }
  /* A Gen-2 list whose count and terminator disagree is not writable at all — that is
   * the exact state the old gb_commit produced, and the reader refuses it outright. */
  {
    G2Save sv; G2Header hd;
    uint32_t n = gbf_build(GBF_CRYSTAL, g_img, 0);
    uint8_t* list;
    GbEditMon e;
    CHECK(g2_detect(g_img, n, &sv) && g2_read_header(g_img, &sv, &hd), "fixture Crystal");
    list = g_img + g2_list_offset(&sv, hd.current_box, hd.current_box);
    CHECK(gb_load(&e, GB_GEN2, list, hd.current_box, 0), "load slot 0");
    list[gb_off_terminator(GB_GEN2, list, hd.current_box)] = 1;   /* the old bug's damage */
    REFUSED(gb_list_count(GB_GEN2, list, hd.current_box) >= 0,
            "a Gen-2 list with no terminator has no usable count");
    REFUSED(gb_commit(&e, list, hd.current_box, 0),
            "and nothing may be committed into it until it is repaired");
  }
}

/* ========================================================================== */
/* 11. gb_commit_checked is all-or-nothing                                     */
/* ========================================================================== */

static void test_commit_checked(void) {
  G2Save sv; G2Header hd;
  uint32_t n;
  uint8_t* list;
  uint8_t whole[MAX_LIST_BYTES];
  GbEditMon e;
  GbSlotSnapshot snap;
  int size, box;
  head("gb_commit_checked: on failure the blob must be byte-identical");

  n = gbf_build(GBF_CRYSTAL, g_img, 0);
  CHECK(g2_detect(g_img, n, &sv) && g2_read_header(g_img, &sv, &hd), "fixture Crystal");
  box  = hd.current_box;
  list = g_img + g2_list_offset(&sv, box, box);
  size = gb_list_size(GB_GEN2, box);

  CHECK(gb_load(&e, GB_GEN2, list, box, 0), "load slot 0");
  normalise_pp(&e);
  CHECK(gb_commit_checked(&e, list, box, 0), "an honest edit commits and verifies");
  memcpy(whole, list, (size_t)size);

  /* Force the gate to fail on a commit that gb_commit itself will happily perform: a
   * record whose species byte is not a species at all. gen1_decode / g2_list_mon refuse
   * it, so gb_verify_slot refuses it, so the write must be rolled back whole. */
  e.rec[0x00] = 0;                       /* species 0 — g2_list_mon returns false */
  REFUSED(gb_commit_checked(&e, list, box, 0), "a write the gate rejects must fail");
  CHECK(memcmp(whole, list, (size_t)size) == 0,
        "AND MUST HAVE LEFT NOTHING BEHIND — every byte back as it was");

  /* The bare gb_commit, by contrast, is documented as leaving the mutation in place;
   * that is exactly why gb_commit_checked exists. Show the difference rather than
   * assume it. */
  CHECK(gb_commit(&e, list, box, 0), "bare gb_commit accepts the same edit");
  REFUSED(gb_verify_slot(&e, list, box, 0), "the gate would have refused it");
  REFUSED(memcmp(whole, list, (size_t)size) == 0,
          "and the caller's blob is left mutated — no undo, which is the defect");
  memcpy(list, whole, (size_t)size);

  /* The snapshot primitive on its own, including that it cannot be aimed elsewhere. */
  CHECK(gb_slot_save(&snap, GB_GEN2, list, box, 1), "snapshot slot 1");
  list[gb_off_record(GB_GEN2, box, 1) + 3] ^= 0xFF;
  list[gb_off_nickname(GB_GEN2, box, 1) + 0] ^= 0xFF;
  list[gb_off_species(GB_GEN2, box, 1)] ^= 0x01;
  REFUSED(memcmp(whole, list, (size_t)size) == 0, "the blob is now different");
  CHECK(gb_slot_restore(&snap, list), "restore");
  CHECK(memcmp(whole, list, (size_t)size) == 0, "and it is byte-identical again");
  REFUSED(gb_slot_save(&snap, GB_GEN2, list, box, 999), "a slot past the capacity");
  REFUSED(gb_slot_restore(&snap, list), "an invalid snapshot restores nothing");
}

/* ========================================================================== */
/* 12. the gate has NO blind spots: flip every byte the slot owns              */
/* ========================================================================== */

/* THE DEFECT THIS SECTION EXISTS FOR. gb_verify_slot compared the parsers' structs field
 * by field, and Gen1Mon has no stats[], no current HP, no type1/type2 and no box-level
 * byte — so 16 of a Gen-1 party record's 44 bytes were never looked at, precisely the
 * ones gb_recalc_stats, gb_set_species/gb_set_gen1_base and gb_set_level write. Four of
 * six mutations aimed at those bytes survived the old suite. This sweep leaves nowhere
 * to hide: every byte of the record, both whole 11-byte name fields (past their
 * terminators too) and the species-list byte, one at a time, twice. */
static void sweep_slot(uint8_t gen, uint8_t* list, int box, int slot, const char* what) {
  GbEditMon e;
  int i, checked = 0, missed = 0;
  static const uint8_t k_flip[2] = { 0x01u, 0xFFu };   /* one bit, then all eight */
  struct { int off; int len; const char* name; } part[4];

  if (!gb_load(&e, gen, list, box, slot)) { CHECK(0, "sweep: load"); return; }
  normalise_pp(&e);
  if (!gb_commit_checked(&e, list, box, slot)) { CHECK(0, "sweep: baseline commit"); return; }

  part[0].off = gb_off_record(gen, box, slot);   part[0].len = e.rec_len;      part[0].name = "record";
  part[1].off = gb_off_otname(gen, box, slot);   part[1].len = GB_NAME_BYTES;  part[1].name = "OT name";
  part[2].off = gb_off_nickname(gen, box, slot); part[2].len = GB_NAME_BYTES;  part[2].name = "nickname";
  part[3].off = gb_off_species(gen, box, slot);  part[3].len = 1;              part[3].name = "species list byte";

  for (i = 0; i < 4; i++) {
    int j, f;
    for (j = 0; j < part[i].len; j++) for (f = 0; f < 2; f++) {
      uint8_t* p = list + part[i].off + j;
      uint8_t  save = *p;
      *p ^= k_flip[f];                                /* XOR always changes the byte */
      checked++;
      if (gb_verify_slot(&e, list, box, slot)) {
        if (missed < 8)
          printf("  !! BLIND: %s %s byte %d (0x%02X -> 0x%02X) passes the gate\n",
                 what, part[i].name, j, save, *p);
        missed++;
      }
      *p = save;
      if (!gb_verify_slot(&e, list, box, slot)) {
        printf("  !! %s %s byte %d: gate fails even after the byte is restored\n",
               what, part[i].name, j);
        missed++;
      }
    }
  }
  g_check++;
  if (missed) { printf("  !! FAIL: %s — %d of %d mutations slipped past the gate\n",
                       what, missed, checked); g_fail++; }
  else printf("  %-28s %d single-byte mutations, every one refused\n", what, checked);
}

static void test_gate_sweep(void) {
  head("gb_verify_slot must refuse EVERY single-byte change to the slot it guards");
  {
    Gen1Save s;
    uint32_t n = gbf_build(GBF_RBY, g_img, 0);
    CHECK(gen1_open(g_img, n, &s) == GEN1_OK, "fixture R/B/Y");
    sweep_slot(GB_GEN1, g_img + gen1_list_offset(&s, gbf_current_box(GBF_RBY)),
               gbf_current_box(GBF_RBY), 0, "Gen 1 box record (33 B)");
    sweep_slot(GB_GEN1, g_img + GEN1_OFF_PARTY, GEN1_PARTY_BOX, 0,
               "Gen 1 party record (44 B)");
  }
  {
    G2Save sv; G2Header hd;
    uint32_t n = gbf_build(GBF_CRYSTAL, g_img, 0);
    CHECK(g2_detect(g_img, n, &sv) && g2_read_header(g_img, &sv, &hd), "fixture Crystal");
    sweep_slot(GB_GEN2, g_img + g2_list_offset(&sv, hd.current_box, hd.current_box),
               hd.current_box, 0, "Gen 2 box record (32 B)");
    sweep_slot(GB_GEN2, g_img + g2_list_offset(&sv, G2_BOX_PARTY, hd.current_box),
               G2_BOX_PARTY, 0, "Gen 2 party record (48 B)");
  }

  /* The six mutations the adversarial verifier aimed at the old gate's blind spots,
   * spelled out one by one so a regression names the byte that got through. All six sit
   * in a Gen-1 PARTY record, the shape Gen1Mon can say least about. */
  {
    Gen1Save s;
    uint32_t n = gbf_build(GBF_RBY, g_img, 0);
    uint8_t* list = g_img + GEN1_OFF_PARTY;
    GbEditMon e;
    int ro, i;
    static const struct { int off; const char* what; } k_blind[] = {
      { 0x22, "max HP, a computed party stat" },
      { 0x24, "the Attack stat" },
      { 0x2A, "the Special stat" },
      { 0x01, "current HP" },
      { 0x05, "type1, which a species edit writes" },
      { 0x06, "type2" },
      { 0x03, "the box-level byte, which gb_set_level writes" },
    };
    CHECK(gen1_open(g_img, n, &s) == GEN1_OK, "fixture R/B/Y");
    CHECK(gb_load(&e, GB_GEN1, list, GEN1_PARTY_BOX, 0), "load a party mon");
    normalise_pp(&e);
    CHECK(gb_commit_checked(&e, list, GEN1_PARTY_BOX, 0), "baseline");
    ro = gb_off_record(GB_GEN1, GEN1_PARTY_BOX, 0);
    for (i = 0; i < (int)(sizeof k_blind / sizeof k_blind[0]); i++) {
      list[ro + k_blind[i].off] ^= 0x11;
      REFUSED(gb_verify_slot(&e, list, GEN1_PARTY_BOX, 0), k_blind[i].what);
      list[ro + k_blind[i].off] ^= 0x11;
    }
    CHECK(gb_verify_slot(&e, list, GEN1_PARTY_BOX, 0), "and clean once all are restored");
  }

  /* The Gen-2 species LIST byte, which the old gate never looked at from the parser's
   * side: an 0xFF there truncates the box for the game's own walker. */
  {
    G2Save sv; G2Header hd;
    uint32_t n = gbf_build(GBF_CRYSTAL, g_img, 0);
    uint8_t* list;
    GbEditMon e;
    int sp;
    CHECK(g2_detect(g_img, n, &sv) && g2_read_header(g_img, &sv, &hd), "fixture Crystal");
    list = g_img + g2_list_offset(&sv, hd.current_box, hd.current_box);
    CHECK(gb_load(&e, GB_GEN2, list, hd.current_box, 1), "load slot 1");
    normalise_pp(&e);
    CHECK(gb_commit_checked(&e, list, hd.current_box, 1), "baseline");
    sp = gb_off_species(GB_GEN2, hd.current_box, 1);
    list[sp] = 0xFF;
    REFUSED(gb_verify_slot(&e, list, hd.current_box, 1),
            "an 0xFF in a mid-list species byte must fail the gate");
    list[sp] = 99;
    REFUSED(gb_verify_slot(&e, list, hd.current_box, 1),
            "and so must any other wrong species in the list");
    list[sp] = 0xFD;
    REFUSED(gb_verify_slot(&e, list, hd.current_box, 1),
            "and the Egg marker on a mon the editor does not hold as an Egg");
    list[sp] = gb_get_species_raw(&e);
    CHECK(gb_verify_slot(&e, list, hd.current_box, 1), "clean once it is put back");
  }
}

/* ========================================================================== */

int main(void) {
  printf("== gb_edit: the Gen-1/2 record editor ==\n");
  test_exp_curves();
  test_species_map();
  test_dv_rules();
  test_pp();
  test_names();
  test_charset_bijection();
  test_sequence_reversibility();
  test_setters_on_fixture();
  test_stat_formula();
  test_carry_hp();
  test_egg();
  test_terminators();
  test_commit_checked();
  test_gate_sweep();

  head("real cartridge saves (roms/gb — Guy's own dumps, never in the repo)");
  real_gen1("Red.sav");
  real_gen2("Gold.sav");
  real_gen2("Crystal.sav");
  real_gen2("Gold-VC.sav.dat");
  real_gen2("Crystal-VC.sav.dat");
  if (!g_ran_real) printf("  (no corpus present — the synthetic tests still ran)\n");

  test_pk2_corpus();
  test_corpus_names_verdict();
  test_negative_controls();

  printf("\ngb_edit test: %d checks, %d failure(s)\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
