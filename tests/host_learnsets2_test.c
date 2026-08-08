/* Host test for the per-game move-source tables with levels (Legality V2).
 *   cc -std=c11 -I source tests/host_learnsets2_test.c source/learnsets2.c -o /tmp/hl2
 *   /tmp/hl2
 *
 * Needs no save file: the tables ARE the subject. Every hardcoded expectation
 * below was read out of the decomps during this test's authoring and is cited to
 * the file it came from, because "I remember Bulbasaur learns Vine Whip at 13" is
 * exactly the kind of Gen-1 memory that would silently bless a broken generator.
 * (It is 13 in Gen 1 and 10 in all of Gen 3.)
 *
 * source/learnsets2.c is generated and git-ignored — run tools/gen_learnsets2.py
 * first. Without it the weak fallbacks in learnsets2.h answer instead, and this
 * test says so and skips rather than failing a build step it cannot perform. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "learnsets2.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)
#define EQ(got, want, msg) do { int _g = (int)(got), _w = (int)(want); \
  if (_g != _w) { printf("  !! FAIL: %s (got %d, want %d)\n", msg, _g, _w); g_fail++; } } while (0)

/* species ids (internal, NOT National Dex — include/constants/species.h) */
#define SP_BULBASAUR  1
#define SP_VENUSAUR   3
#define SP_CHARIZARD  6
#define SP_BLASTOISE  9
#define SP_CHARMANDER 4
#define SP_CHARMELEON 5
#define SP_CATERPIE  10
#define SP_PIKACHU   25
#define SP_PICHU    172
#define SP_DEOXYS   410     /* internal 410; National Dex 386 */

/* move ids (include/constants/moves.h) */
#define MV_SWORDS_DANCE   14
#define MV_CUT            15
#define MV_VINE_WHIP      22
#define MV_TACKLE         33
#define MV_LEECH_SEED     73
#define MV_SOLAR_BEAM     76
#define MV_THUNDERBOLT    85
#define MV_LIGHT_SCREEN  113
#define MV_SNORE         173
#define MV_CURSE         174
#define MV_SYNTHESIS     235
#define MV_BLAST_BURN    307
#define MV_HYDRO_CANNON  308
#define MV_FRENZY_PLANT  338
#define MV_VOLT_TACKLE   344
#define MV_SUPERPOWER    276
#define MV_KNOCK_OFF     282
#define MV_DRAGON_DANCE  349
#define MV_PSYCHO_BOOST  354

static const PkGame ALL[3] = { PK_RS, PK_EMERALD, PK_FRLG };
static const char* GN[3] = { "RS", "Emerald", "FRLG" };

static bool list_has(PkGame g, uint16_t sp, uint16_t mv, bool egg) {
  const uint16_t* e;
  int n = egg ? lg2_egg_list(g, sp, &e) : lg2_levelup_list(g, sp, &e), i;
  for (i = 0; i < n; i++) if ((egg ? e[i] : LG2_LV_MOVE(e[i])) == mv) return true;
  return false;
}

int main(void) {
  int gi, i, s;

  if (!lg2_have_data()) {
    printf("learnsets2.c not generated (weak fallbacks active) — "
           "run: python3 tools/gen_learnsets2.py\n");
    return 77;                     /* distinct from 0/1: nothing was tested */
  }

  /* ---- (1) level-up levels, the whole point of this table -----------------
   * reference/pokeemerald_data/src/data/pokemon/level_up_learnsets.h:3-15 and
   * the same block in the pokefirered / pokeruby files: Bulbasaur's Gen-3
   * learnset is identical in all three game groups. */
  printf("== level-up levels (Bulbasaur, all three game groups) ==\n");
  for (gi = 0; gi < 3; gi++) {
    PkGame g = ALL[gi];
    EQ(lg2_min_levelup_level(g, SP_BULBASAUR, MV_TACKLE), 1, "Bulbasaur Tackle @1");
    EQ(lg2_min_levelup_level(g, SP_BULBASAUR, MV_VINE_WHIP), 10, "Bulbasaur Vine Whip @10");
    EQ(lg2_min_levelup_level(g, SP_BULBASAUR, MV_LEECH_SEED), 7, "Bulbasaur Leech Seed @7");
    EQ(lg2_min_levelup_level(g, SP_BULBASAUR, MV_SYNTHESIS), 39, "Bulbasaur Synthesis @39");
    EQ(lg2_min_levelup_level(g, SP_BULBASAUR, MV_SOLAR_BEAM), 46, "Bulbasaur Solar Beam @46");
    EQ(lg2_min_levelup_level(g, SP_BULBASAUR, MV_THUNDERBOLT), -1,
       "Bulbasaur never learns Thunderbolt by level-up");
    EQ(lg2_levelup_list(g, SP_BULBASAUR, NULL), 11, "Bulbasaur has 11 level-up entries");
  }
  {
    const uint16_t* e;
    lg2_levelup_list(PK_EMERALD, SP_BULBASAUR, &e);
    EQ(LG2_LV_LEVEL(e[0]), 1, "first entry level");
    EQ(LG2_LV_MOVE(e[0]), MV_TACKLE, "first entry move");
  }

  /* ---- (2) TM/HM per species — what the V1 union deliberately gave up ---- */
  printf("== TM/HM compatibility ==\n");
  EQ(lg2_tmhm_index(MV_THUNDERBOLT), 23, "Thunderbolt is TM24 (0-based 23)");
  EQ(lg2_tmhm_index(MV_CUT), 50, "Cut is HM01, i.e. the 51st machine");
  EQ(lg2_tmhm_index(MV_SYNTHESIS), -1, "Synthesis is on no machine");
  for (gi = 0; gi < 3; gi++) {
    PkGame g = ALL[gi];
    CHECK(lg2_tmhm(g, SP_PIKACHU, MV_THUNDERBOLT), "Pikachu is TM24-compatible");
    CHECK(lg2_tmhm(g, SP_BULBASAUR, MV_SOLAR_BEAM), "Bulbasaur is TM22-compatible");
    CHECK(!lg2_tmhm(g, SP_BULBASAUR, MV_THUNDERBOLT), "Bulbasaur is NOT TM24-compatible");
  }
  /* Caterpie is one of the 14 Gen-3 species with an entirely empty machine row
   * (tmhm_learnsets.h has [SPECIES_CATERPIE] with no fields set). */
  {
    int any = 0;
    for (i = 1; i <= 354; i++) if (lg2_tmhm(PK_EMERALD, SP_CATERPIE, (uint16_t)i)) any++;
    EQ(any, 0, "Caterpie can learn no TM/HM at all");
  }

  /* ---- (3) tutors — per game, which is what makes them worth a table ------
   * Emerald has 30 tutors, FRLG the first 15 of the same list. Bulbasaur is on
   * Emerald's Snore tutor but not FRLG's (tutor_learnsets.h, both games). */
  printf("== move tutors ==\n");
  EQ(lg2_tutor_index(MV_SWORDS_DANCE), 1, "Swords Dance is tutor slot 1");
  CHECK(lg2_tutor(PK_EMERALD, SP_BULBASAUR, MV_SNORE), "Emerald tutors Bulbasaur Snore");
  CHECK(!lg2_tutor(PK_FRLG, SP_BULBASAUR, MV_SNORE), "FRLG does NOT tutor Bulbasaur Snore");
  CHECK(lg2_tutor(PK_EMERALD, SP_BULBASAUR, MV_SWORDS_DANCE), "E tutors Bulbasaur Swords Dance");
  CHECK(lg2_tutor(PK_FRLG, SP_BULBASAUR, MV_SWORDS_DANCE), "FRLG tutors Bulbasaur Swords Dance");
  CHECK(!lg2_tutor(PK_EMERALD, SP_BULBASAUR, MV_CUT), "Cut is an HM, never a tutor move");

  /* The Cape Brink "ultimate" tutor is FRLG code, not a learnset table
   * (pokefirered/src/party_menu.c GetTutorMove + CanLearnTutorMove), and it gates
   * on exactly one species each — the Kanto starters, not all nine Gen-3 starters
   * as gen_legality.py:183-186 assumes. pokeemerald has no such tutor at all, so
   * these three moves are FRLG-only in Gen 3. Without the overlay a legitimate
   * FireRed Blastoise holding Hydro Cannon would come back with NO source. */
  CHECK(lg2_tutor(PK_FRLG, SP_BLASTOISE, MV_HYDRO_CANNON), "FRLG tutors Hydro Cannon");
  CHECK(lg2_tutor(PK_FRLG, SP_CHARIZARD, MV_BLAST_BURN), "FRLG tutors Blast Burn");
  CHECK(lg2_tutor(PK_FRLG, SP_VENUSAUR, MV_FRENZY_PLANT), "FRLG tutors Frenzy Plant");
  CHECK(!lg2_tutor(PK_EMERALD, SP_BLASTOISE, MV_HYDRO_CANNON),
        "Emerald has no ultimate-move tutor");
  CHECK(!lg2_tutor(PK_FRLG, SP_CHARIZARD, MV_HYDRO_CANNON), "wrong starter, wrong move");
  CHECK(lg2_tutor_index(MV_HYDRO_CANNON) >= 0, "Hydro Cannon is a tutor move somewhere");

  /* ---- (4) egg moves ------------------------------------------------------
   * egg_moves.h is byte-identical across the three decomps (the generator
   * re-checks this every run), so all three game groups must agree. */
  printf("== egg moves ==\n");
  for (gi = 0; gi < 3; gi++) {
    PkGame g = ALL[gi];
    CHECK(list_has(g, SP_CHARMANDER, MV_DRAGON_DANCE, true), "Charmander egg: Dragon Dance");
    EQ(lg2_egg_list(g, SP_CHARMANDER, NULL), 8, "Charmander has 8 egg moves");
    CHECK(list_has(g, SP_BULBASAUR, MV_CURSE, true), "Bulbasaur egg: Curse");
    CHECK(list_has(g, SP_BULBASAUR, MV_LIGHT_SCREEN, true), "Bulbasaur egg: Light Screen");
    EQ(lg2_egg_list(g, SP_BULBASAUR, NULL), 8, "Bulbasaur has 8 egg moves");
    /* egg moves belong to the lowest breeding stage only */
    EQ(lg2_egg_list(g, SP_CHARMELEON, NULL), 0, "Charmeleon has no egg moves of its own");
  }
  /* Volt Tackle has no egg_moves.h entry: GiveVoltTackleIfLightBall() in
   * src/daycare.c hands it to the hatchling when a parent holds a Light Ball.
   * pokeruby's daycare.c has no such function, so Ruby/Sapphire really cannot
   * breed one — a per-game distinction only this table can express. */
  CHECK(list_has(PK_EMERALD, SP_PICHU, MV_VOLT_TACKLE, true), "Emerald: Pichu egg Volt Tackle");
  CHECK(list_has(PK_FRLG, SP_PICHU, MV_VOLT_TACKLE, true), "FRLG: Pichu egg Volt Tackle");
  CHECK(!list_has(PK_RS, SP_PICHU, MV_VOLT_TACKLE, true), "RS cannot breed Volt Tackle");

  /* ---- (5) the level-window rule (research-legality-v2.md §3 C2) ----------
   * Synthesis is on no machine and no tutor list, and is not a Bulbasaur egg
   * move, so level-up is its ONLY source — the case where "min level > current
   * level" is a hard impossibility rather than a guess. */
  printf("== move sources / level window ==\n");
  {
    Lg2Sources s;
    lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_SYNTHESIS, 30, &s);
    EQ(s.srcs, 0, "Synthesis unreachable on a level-30 Bulbasaur");
    EQ(s.levelup_at, 39, "...but the level is still reported, for the message");

    lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_SYNTHESIS, 39, &s);
    EQ(s.srcs, LG2_SRC_LEVELUP, "Synthesis is level-up-only, and legal at 39");

    lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_SYNTHESIS, 0, &s);
    EQ(s.srcs, LG2_SRC_LEVELUP, "level 0 means 'ignore the window'");

    /* Solar Beam is BOTH a level-46 move and TM22: below 46 the TM still

     * supplies it, which is precisely why C2 may only fire on level-up-only
     * moves. A checker that ignored the source mask would call this illegal. */
    lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_SOLAR_BEAM, 30, &s);
    EQ(s.srcs, LG2_SRC_TMHM, "Solar Beam at level 30: TM only");
    lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_SOLAR_BEAM, 46, &s);
    EQ(s.srcs, LG2_SRC_LEVELUP | LG2_SRC_TMHM, "Solar Beam at 46: level-up AND TM");

    lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_CURSE, 100, &s);
    EQ(s.srcs, LG2_SRC_EGG, "Curse on Bulbasaur is egg-only");

    lg2_move_sources(PK_FRLG, SP_BLASTOISE, MV_HYDRO_CANNON, 100, &s);
    EQ(s.srcs, LG2_SRC_TUTOR, "Hydro Cannon on Blastoise is FRLG-tutor-only");
    lg2_move_sources(PK_EMERALD, SP_PICHU, MV_VOLT_TACKLE, 5, &s);
    EQ(s.srcs, LG2_SRC_EGG, "Volt Tackle on Pichu is egg-only");

    lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_SNORE, 100, &s);
    EQ(s.srcs, LG2_SRC_TUTOR, "Snore on Bulbasaur is Emerald-tutor-only");
    lg2_move_sources(PK_FRLG, SP_BULBASAUR, MV_SNORE, 100, &s);
    EQ(s.srcs, 0, "...and unreachable in FRLG");

    EQ(lg2_move_sources(PK_EMERALD, SP_BULBASAUR, MV_CURSE, 100, NULL), LG2_SRC_EGG,
       "return value equals out->srcs, and out may be NULL");
  }

  /* ---- (6) FireRed vs LeafGreen: Deoxys ----------------------------------
   * pokefirered defines sDeoxysLevelUpLearnset twice, under #if defined(FIRERED)
   * / #elif defined(LEAFGREEN) (level_up_learnsets.h:5657-5691), because Deoxys'
   * forme is version-specific. PK_FRLG is ONE game group, so the table must hold
   * the union: Superpower (FireRed only) AND Knock Off (LeafGreen only). A
   * generator that let the second definition overwrite the first would drop 12
   * entries and then call a legal FireRed Deoxys illegal. */
  printf("== FireRed/LeafGreen Deoxys variants ==\n");
  EQ(lg2_min_levelup_level(PK_FRLG, SP_DEOXYS, MV_SUPERPOWER), 30, "FR Deoxys Superpower @30");
  EQ(lg2_min_levelup_level(PK_FRLG, SP_DEOXYS, MV_KNOCK_OFF), 15, "LG Deoxys Knock Off @15");
  EQ(lg2_min_levelup_level(PK_FRLG, SP_DEOXYS, MV_PSYCHO_BOOST), 45, "FRLG Deoxys Psycho Boost @45");
  EQ(lg2_min_levelup_level(PK_EMERALD, SP_DEOXYS, MV_SUPERPOWER), -1,
     "Emerald Deoxys never learns Superpower");
  EQ(lg2_min_levelup_level(PK_EMERALD, SP_DEOXYS, MV_KNOCK_OFF), 15, "E Deoxys Knock Off @15");
  EQ(lg2_min_levelup_level(PK_RS, SP_DEOXYS, MV_SUPERPOWER), -1,
     "RS Deoxys never learns Superpower");

  /* ---- (7) honesty about approximated tables ------------------------------
   * Asserted as an INVARIANT, not as today's disk state: level-up and egg data
   * must be real for every game group, and whatever lg2_exact_sources() omits
   * must show up in Lg2Sources.approx so a caller can refuse to conclude. */
  printf("== data provenance ==\n");
  for (gi = 0; gi < 3; gi++) {
    PkGame g = ALL[gi];
    uint8_t ex = lg2_exact_sources(g);
    Lg2Sources s;
    printf("  %-8s exact sources: %s%s%s%s\n", GN[gi],
           (ex & LG2_SRC_LEVELUP) ? "levelup " : "",
           (ex & LG2_SRC_TMHM) ? "tmhm " : "-tmhm(approx) ",
           (ex & LG2_SRC_TUTOR) ? "tutor " : "-tutor(approx) ",
           (ex & LG2_SRC_EGG) ? "egg" : "");
    EQ(ex & (LG2_SRC_LEVELUP | LG2_SRC_EGG), LG2_SRC_LEVELUP | LG2_SRC_EGG,
       "level-up and egg data are never approximated");
    lg2_move_sources(g, SP_BULBASAUR, MV_TACKLE, 50, &s);
    EQ(s.approx, (uint8_t)((LG2_SRC_TMHM | LG2_SRC_TUTOR) & ~ex),
       "approx mask matches exactly the tables that were borrowed");
  }
  /* A borrowed table may only ever WIDEN the answer, never narrow it. Swept over
   * every species x every machine and every tutor slot, because the dangerous
   * way to "approximate" a missing table is to fill it with zeros: that would
   * leave every RS mon's TM move with no explanation and flag legit Pokemon.
   * This is the check that has teeth — the mask comparison above only proves the
   * generator is self-consistent. */
  {
    uint16_t tmv[64], tut[64];
    int ntm = 0, ntut = 0, narrowed_tm = 0, narrowed_tut = 0;
    for (i = 1; i <= 354; i++) {
      if (lg2_tmhm_index((uint16_t)i) >= 0 && ntm < 64) tmv[ntm++] = (uint16_t)i;
      if (lg2_tutor_index((uint16_t)i) >= 0 && ntut < 64) tut[ntut++] = (uint16_t)i;
    }
    EQ(ntm, 58, "58 machines: TM01..TM50 + HM01..HM08");
    EQ(ntut, 33, "30 data-file tutors + 3 code-taught Cape Brink ultimates");
    for (gi = 0; gi < 3; gi++) {
      PkGame g = ALL[gi];
      uint8_t ex = lg2_exact_sources(g);
      int gj;
      for (gj = 0; gj < 3; gj++) {
        if (gj == gi) continue;
        for (s = 1; s <= 411; s++) {
          if (!(ex & LG2_SRC_TMHM))
            for (i = 0; i < ntm; i++)
              if (lg2_tmhm(ALL[gj], (uint16_t)s, tmv[i]) &&
                  !lg2_tmhm(g, (uint16_t)s, tmv[i])) narrowed_tm++;
          if (!(ex & LG2_SRC_TUTOR))
            for (i = 0; i < ntut; i++)
              if (lg2_tutor(ALL[gj], (uint16_t)s, tut[i]) &&
                  !lg2_tutor(g, (uint16_t)s, tut[i])) narrowed_tut++;
        }
      }
    }
    EQ(narrowed_tm, 0, "no borrowed TM row is narrower than a real one");
    EQ(narrowed_tut, 0, "no borrowed tutor row is narrower than a real one");
  }

  /* ---- (8) bad arguments never read out of bounds ------------------------ */
  printf("== bounds ==\n");
  {
    Lg2Sources s;
    EQ(lg2_levelup_list(PK_EMERALD, 0, NULL), 0, "species 0");
    EQ(lg2_levelup_list(PK_EMERALD, 412, NULL), 0, "species 412 (SPECIES_EGG)");
    EQ(lg2_levelup_list(PK_EMERALD, 65535, NULL), 0, "species 65535");
    EQ(lg2_egg_list((PkGame)3, SP_BULBASAUR, NULL), 0, "game 3");
    EQ(lg2_min_levelup_level(PK_EMERALD, SP_BULBASAUR, 0), -1, "move 0");
    EQ(lg2_tmhm_index(0), -1, "move 0 is no machine");
    CHECK(!lg2_tutor((PkGame)9, SP_BULBASAUR, MV_SNORE), "game 9");
    EQ(lg2_move_sources((PkGame)3, SP_BULBASAUR, MV_TACKLE, 5, &s), 0, "sources, game 3");
    EQ(s.levelup_at, -1, "...and the struct is still initialised");
    EQ(lg2_exact_sources((PkGame)7), 0, "exact_sources out of range");
  }

  /* ---- (9) whole-table sweep: structural invariants over every species ---- */
  printf("== full sweep ==\n");
  {
    long lv_total = 0, egg_total = 0;
    int bad_level = 0, bad_move = 0, unsorted = 0, bad_min = 0, bad_egg = 0;
    for (gi = 0; gi < 3; gi++) {
      PkGame g = ALL[gi];
      for (s = 1; s <= 411; s++) {
        const uint16_t* e;
        int n = lg2_levelup_list(g, (uint16_t)s, &e);
        lv_total += n;
        for (i = 0; i < n; i++) {
          int L = LG2_LV_LEVEL(e[i]);
          uint16_t m = LG2_LV_MOVE(e[i]);
          if (L < 1 || L > 100) bad_level++;
          if (m < 1 || m > 354) bad_move++;
          if (i && LG2_LV_LEVEL(e[i - 1]) > L) unsorted++;
          if (lg2_min_levelup_level(g, (uint16_t)s, m) > L) bad_min++;
        }
        n = lg2_egg_list(g, (uint16_t)s, &e);
        egg_total += n;
        for (i = 0; i < n; i++) if (e[i] < 1 || e[i] > 354) bad_egg++;
      }
    }
    EQ(bad_level, 0, "every level-up level is in 1..100");
    EQ(bad_move, 0, "every level-up move id is in 1..354");
    EQ(unsorted, 0, "every level-up list is ascending by level");
    EQ(bad_min, 0, "lg2_min_levelup_level is <= every listed level for that move");
    EQ(bad_egg, 0, "every egg move id is in 1..354");
    printf("  %ld level-up entries, %ld egg entries across the three game groups\n",
           lv_total, egg_total);
    CHECK(lv_total > 11000, "the level-up tables are actually populated");
    EQ(egg_total, 2921, "973 egg moves x 3 groups + Volt Tackle in E and FRLG");
  }

  if (g_fail) printf("\nlearnsets2: %d check(s) FAILED\n", g_fail);
  else        printf("\nlearnsets2: all checks passed\n");
  return g_fail ? 1 : 0;
}
