/* Host test: DOES gen3_build_mon PRODUCE A POKEMON POKEDNA'S OWN CHECKER ACCEPTS?
 *
 *   cc -std=c11 -O2 -I source tests/host_legalbuild_test.c source/gen3_edit.c \
 *      source/gen3_legality2.c source/gen3_legality_hooks.c source/learnsets2.c \
 *      source/encounters.c source/statics.c source/gen3_pidiv.c source/gen3_daycare.c \
 *      source/evolutions.c \
 *      source/gen3_mon.c source/gen3_save.c source/data_tables.c -o /tmp/hlb && /tmp/hlb
 *
 * source/learnsets2.c and source/encounters.c are GENERATED and git-ignored — run
 * tools/gen_learnsets2.py and tools/gen_encounters.py --from-rom first, or the link
 * fails (same contract as host_learnsets2_test.c / host_legality_hooks_test.c).
 *
 * WHY THIS EXISTS. A tool that audits other people's save data must not write records
 * its own auditor rejects. It used to: gen3_build_mon stamped met location 255
 * (METLOC_FATEFUL_ENCOUNTER), which is both a false provenance claim and an EXEMPTION —
 * gen3_legality_hooks.c skips its move and encounter reasoning for met >= 0xFD, so the
 * tool was hiding its output from its own checker. With an honest MAPSEC in place of
 * the 255 the checker started talking, and at the level app_create_mon uses only 44 of
 * the 386 species graded LEGAL. Two real causes: a hard-coded Tackle that 263 species
 * cannot learn, and a caught origin whose PID/IV pair matches no RNG seed.
 *
 * AND THE SECOND HALF OF THE SAME ARGUMENT, added when T_evo shipped: the builder must
 * not hand out a Pokemon that cannot EXIST either. It used to build every species at
 * level 5, so "create a Charizard" produced a L5 Charizard — impossible, since Charmeleon
 * does not become one until L36 — and the checker duly flagged it. gen3_build_level()
 * now gives each species its own floor, so the sweep below runs at a PER-SPECIES level
 * (build_lvl()) rather than at a single hard-coded 5. Part (E) rolls that back along with
 * the other defaults and proves the flag returns.
 *
 * The seven parts:
 *   (A) THE ORIGIN GATE — gen3_species_can_hatch() must refuse exactly the species Gen 3
 *       cannot hatch, pinned by name. A false positive here is the failure that matters:
 *       it would make a legendary claim it came from an egg.
 *   (A2) THE LEVEL FLOOR — gen3_build_level() must be 5 where 5 is possible, the species'
 *       own pk_evo_floor where it is not, and 5 again when T_evo is absent (fail open).
 *   (B) NO EXEMPTION MARKERS — the four escape hatches (met 0xFD/0xFE/0xFF, ribbon bit
 *       31) each make the checker skip a whole family of reasoning. The builder must
 *       write none of them, on any species, in any origin game.
 *   (C) STRUCTURE — moves come from the species' OWN learnset at its OWN level, the
 *       ability slot is the one CreateBoxMon would derive, and the record still passes
 *       the lossless edit round-trip (the pre-write safety gate).
 *   (D) THE SWEEP — all 386 species x 3 origin games x both PID parities, re-decoded
 *       from the built bytes (never the in-memory struct) and graded by
 *       pk_check_legality2_ex with PK2_RUN_PIDIV, which is what pdna_legality.c runs
 *       when the user asks for the RNG check. Histogram + every reason, then a FLOOR.
 *   (E) THE BITE — the same fixtures rebuilt with the pre-fix defaults (Tackle, met
 *       level = build level, no ability bit) must FAIL. Without this, part (D) could be
 *       passing because the checker went quiet rather than because the record got legal.
 *   (F) THE ONE THING THE CHECKER STILL CANNOT SEE, asserted so it is documented in
 *       code rather than only in prose.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gen3_edit.h"
#include "gen3_mon.h"
#include "gen3_legality2.h"
#include "gen3_legality_hooks.h"
#include "gen3_daycare.h"
#include "evolutions.h"
#include "learnsets2.h"
#include "encounters.h"
#include "data_tables.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

/* The level app_create_mon actually passes — gen3_build_level(sp), the species' own
 * floor — and the three origin bytes it can pass (2 Ruby / 3 Emerald / 4 FireRed).
 * BUILD_LVL survives as the OLD hard-coded level, used only by part (E)'s rollback and
 * by the level-floor fixtures; nothing in the sweep may use it directly again. */
#define BUILD_LVL   5
static uint8_t build_lvl(uint16_t sp) { return gen3_build_level(sp); }
static const uint8_t k_games[3] = { 2, 3, 4 };

/* Two PIDs of opposite parity: the ability slot is `personality & 1`, so a builder that
 * forgets the bit passes on even PIDs and fails on odd ones. dc_seed() gives either. */
static const uint32_t k_pids[2] = { 0x1234ABCDu /* odd */, 0x1234ABCEu /* even */ };

#define EGG_UNDISCOVERED 15
#define MET_SPECIAL_EGG  0xFD
#define RIBBON_FATEFUL   0x80000000u

/* ---- reason tally ---------------------------------------------------------- */
static char g_reason[64][PK2_TEXT_LEN];
static int  g_rn[64], g_rsev[64], g_nreason = 0;
static void tally(const Pk2Row* r) {
  for (int i = 0; i < g_nreason; i++)
    if (!strcmp(g_reason[i], r->text)) { g_rn[i]++; return; }
  if (g_nreason >= 64) return;
  snprintf(g_reason[g_nreason], PK2_TEXT_LEN, "%s", r->text);
  g_rsev[g_nreason] = r->sev;
  g_rn[g_nreason++] = 1;
}

/* A box record stores no level — it stores EXP, and the level is derived from it with the
 * species' growth rate (exactly what gen3_legality2.c:406 does). PkMon.level stays 0 on a
 * box decode, so every level assertion here has to go through this. */
static uint8_t decoded_level(uint16_t sp, const PkMon* m) {
  return pk_level_from_exp(pk_species_growth(sp), m->experience);
}

/* Build, then RE-DECODE from the bytes. Never grade the EditMon that produced them: the
 * whole point is to judge what actually lands in the save. */
static void build_and_decode(uint16_t sp, uint8_t lvl, uint32_t pid, uint8_t game,
                             uint8_t rec[80], PkMon* m) {
  gen3_build_mon(sp, lvl, pid, 0x00010002u, "GUY", game, rec);
  memset(m, 0, sizeof *m);
  pk_decode_mon(rec, false, m);
}

/* ================================ (A) THE ORIGIN GATE ======================== */

/* The species Gen 3 has NO egg route for, by internal id. Every one of them is either a
 * legendary (its whole line is egg group UNDISCOVERED and it evolves into nothing that
 * escapes it) or Unown, whose only source is the Tanoby Ruins. This list is the test's
 * OWN fixture, written from the egg-group table's UNDISCOVERED rows minus the baby
 * forms — it is not read from the code under test, so a change to the rule shows up
 * here as a diff instead of silently agreeing with itself. */
static const uint16_t k_no_egg_route[22] = {
  144, 145, 146,            /* ARTICUNO ZAPDOS MOLTRES     */
  150, 151,                 /* MEWTWO MEW                  */
  201,                      /* UNOWN                       */
  243, 244, 245,            /* RAIKOU ENTEI SUICUNE        */
  249, 250, 251,            /* LUGIA HO-OH CELEBI          */
  401, 402, 403,            /* REGIROCK REGICE REGISTEEL   */
  404, 405, 406,            /* KYOGRE GROUDON RAYQUAZA     */
  407, 408,                 /* LATIAS LATIOS               */
  409, 410,                 /* JIRACHI DEOXYS              */
};
static int no_egg_route(uint16_t sp) {
  for (unsigned i = 0; i < sizeof k_no_egg_route / sizeof k_no_egg_route[0]; i++)
    if (k_no_egg_route[i] == sp) return 1;
  return 0;
}

/* The nine baby forms: egg group UNDISCOVERED (they cannot breed) yet they are exactly
 * what comes OUT of an egg. Getting these wrong is the interesting half of the gate. */
static const uint16_t k_babies[9] = { 172, 173, 174, 175, 236, 238, 239, 240, 350 };

static void part_a_origin_gate(void) {
  printf("(A) the origin gate\n");

  /* THE CLAUSE THAT MATTERS: nothing without an egg route may be called hatchable. */
  for (unsigned i = 0; i < sizeof k_no_egg_route / sizeof k_no_egg_route[0]; i++) {
    uint16_t sp = k_no_egg_route[i];
    char w[80];
    snprintf(w, sizeof w, "%s must NOT be called hatchable", pk_species_name(sp));
    CHECK(!gen3_species_can_hatch(sp), w);
  }

  /* ...and the babies, which the plain "egg group != UNDISCOVERED" test gets wrong. */
  for (unsigned i = 0; i < sizeof k_babies / sizeof k_babies[0]; i++) {
    uint16_t sp = k_babies[i];
    char w[80];
    CHECK(pk_egg_group(sp, 0) == EGG_UNDISCOVERED, "baby form is UNDISCOVERED (fixture)");
    snprintf(w, sizeof w, "%s (baby) IS hatchable", pk_species_name(sp));
    CHECK(gen3_species_can_hatch(sp), w);
  }

  CHECK(!gen3_species_can_hatch(0), "species 0 is not hatchable");
  CHECK(!gen3_species_can_hatch(9999), "out-of-range species is not hatchable");

  /* Print the complete refusal list, so the 3 known under-counts stay visible. */
  int refused = 0;
  printf("     refused (built as caught, and flagged):");
  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    if (gen3_species_can_hatch(sp)) continue;
    if (refused % 6 == 0) printf("\n       ");
    printf(" %-11s", pk_species_name(sp));
    refused++;
  }
  printf("\n     total refused: %d of 386\n", refused);
  /* 22 with no egg route at all + WYNAUT / NIDORINA / NIDOQUEEN, which really are egg
   * obtainable but need the forward evolution table to prove it (T_evo, not built). */
  CHECK(refused == 25, "exactly 25 species are refused an egg origin");
}

/* ============================== (A2) THE LEVEL FLOOR ========================= */

/* Species pinned BY NAME, so a table regeneration that quietly changes a floor shows up
 * here as a diff rather than as a silently different create flow. The right-hand column
 * is pk_evo_floor, which is min(evolution floor, lowest wild level) — that is why
 * GYARADOS is 5 and not the 20 Magikarp evolves at (Sootopolis' Super Rod), and why
 * POLIWHIRL is 20 and not the 25 Poliwag evolves at (FireRed's Safari Zone). Building at
 * the WILD-relaxed number is deliberate: it is the number the checker judges against,
 * and the game itself hands those out. */
static const struct { uint16_t sp; uint8_t want; const char* why; } k_level_pins[] = {
  {   1,  5, "BULBASAUR   base form, hatches at 5"      },
  {   4,  5, "CHARMANDER  base form"                    },
  {   5, 16, "CHARMELEON  evolves at 16"                },
  {   6, 36, "CHARIZARD   evolves at 36 — Guy's case"   },
  { 149, 55, "DRAGONITE   evolves at 55"                },
  { 282, 36, "BLAZIKEN    Hoenn starter's final stage"  },
  { 130,  5, "GYARADOS    wild at L5 in Sootopolis"     },
  {  61, 20, "POLIWHIRL   wild below its evolution"     },
  { 201,  5, "UNOWN       no evolution at all"          },
  { 350,  5, "AZURILL     baby form, comes from an egg" },
  { 405,  5, "GROUDON     legendary, no evolution"      },
};

static void part_a2_level_floor(void) {
  printf("(A2) the level floor: gen3_build_level()\n");

  for (unsigned i = 0; i < sizeof k_level_pins / sizeof k_level_pins[0]; i++) {
    uint16_t sp = k_level_pins[i].sp;
    int fl = pk_evo_floor(sp);
    char w[128];
    snprintf(w, sizeof w, "%s -> L%u (got L%u, floor %d)", k_level_pins[i].why,
             (unsigned)k_level_pins[i].want, (unsigned)gen3_build_level(sp), fl);
    CHECK(gen3_build_level(sp) == k_level_pins[i].want, w);
  }

  /* The RULE, over all 386, rather than only the pinned rows: exactly 5, or exactly the
   * species' own floor, and never something in between or below. */
  int raised = 0, maxlvl = 0;
  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    int fl = pk_evo_floor(sp), got = gen3_build_level(sp);
    char w[96];
    snprintf(w, sizeof w, "%s: build level is never below 5", pk_species_name(sp));
    CHECK(got >= BUILD_LVL, w);
    if (fl == PK_EVO_NO_DATA || fl <= BUILD_LVL) {
      snprintf(w, sizeof w, "%s: floor <= 5 keeps the egg default", pk_species_name(sp));
      CHECK(got == BUILD_LVL, w);
    } else {
      snprintf(w, sizeof w, "%s: build level == its floor (%d)", pk_species_name(sp), fl);
      CHECK(got == fl, w);
      raised++;
      if (got > maxlvl) maxlvl = got;
    }
    /* The whole point: the mon it builds is never below the level its species can hold.
     * This is the assertion that would have caught the L5 Charizard. */
    if (fl != PK_EVO_NO_DATA) {
      snprintf(w, sizeof w, "%s: built at or above its own floor", pk_species_name(sp));
      CHECK(got >= fl, w);
    }
  }
  printf("     %d of 386 species now build above L5 (highest L%d)\n", raised, maxlvl);
  CHECK(raised > 100, "the floor moves a substantial part of the roster, not a handful");

  /* And the record really carries it. A BOX record stores no level at all — the level
   * IS the exp (gen3_legality2.c:406 derives it exactly this way), so this is the
   * assertion that a builder writing one without the other would fail. */
  uint8_t rec[80]; PkMon m;
  build_and_decode(6 /* CHARIZARD */, build_lvl(6), k_pids[0], 3, rec, &m);
  CHECK(decoded_level(6, &m) == 36, "a CREATED Charizard decodes as L36");
  CHECK(m.experience == pk_exp_for_level(pk_species_growth(6), 36), "...with the exp for L36");
  CHECK(m.metLevel == 0, "...and still hatched (met level 0), which the PID/IV exemption needs");

  /* lvl 0 means "you pick"; an explicit level is honoured exactly, including one below
   * the floor — gen12_convert.c depends on that to import a Pokemon at its real level. */
  uint8_t r0[80], r5[80]; PkMon m0, m5;
  gen3_build_mon(6, 0, k_pids[0], 0x00010002u, "GUY", 3, r0);
  memset(&m0, 0, sizeof m0); pk_decode_mon(r0, false, &m0);
  CHECK(decoded_level(6, &m0) == 36, "lvl 0 = 'you pick' -> the species floor");
  gen3_build_mon(6, BUILD_LVL, k_pids[0], 0x00010002u, "GUY", 3, r5);
  memset(&m5, 0, sizeof m5); pk_decode_mon(r5, false, &m5);
  CHECK(decoded_level(6, &m5) == BUILD_LVL,
        "an EXPLICIT level is never promoted (the Gen-1/2 importer)");
}

/* ============== (B) NO EXEMPTION MARKERS + (C) STRUCTURE ===================== */

static void part_bc_markers_and_structure(void) {
  printf("(B) no exemption markers, (C) structure\n");
  int nsp = 0;
  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    nsp++;
    for (int gi = 0; gi < 3; gi++) {
      for (int pi = 0; pi < 2; pi++) {
        uint8_t rec[80];
        PkMon m;
        uint8_t lvl = build_lvl(sp);
        build_and_decode(sp, lvl, k_pids[pi], k_games[gi], rec, &m);
        char w[96];

        /* (B) the four escape hatches, research-legal-generator.md §5.2. Each one makes
         * the checker exempt the record; writing one would buy a green banner with a
         * lie. 0xFD/0xFE/0xFF are all >= MET_SPECIAL_EGG; bit 31 is the fateful ribbon;
         * origin 15 is Colosseum/XD, which gen3_legality2.c:410 exempts wholesale. */
        snprintf(w, sizeof w, "%s: met location is a real place (< 0xFD)", pk_species_name(sp));
        CHECK(m.metLocation < MET_SPECIAL_EGG, w);
        snprintf(w, sizeof w, "%s: fateful ribbon bit is clear", pk_species_name(sp));
        CHECK((m.ribbons & RIBBON_FATEFUL) == 0, w);
        snprintf(w, sizeof w, "%s: origin game is a real GBA game", pk_species_name(sp));
        CHECK(m.metGame >= 1 && m.metGame <= 5, w);

        /* (C) moves. Slot 0 must be filled (a move-less mon is INVALID), no duplicates,
         * and every move must have a real source for THIS line — which is exactly the
         * question pk2_hook_moves asks, asked here per move so a failure names it. */
        CHECK(m.moves[0] != 0, "slot 0 is filled");
        for (int i = 0; i < 4; i++) {
          if (!m.moves[i]) continue;
          for (int j = 0; j < i; j++) CHECK(m.moves[j] != m.moves[i], "no duplicate move");
          uint8_t srcs = pk2_line_sources(sp, m.moves[i]);
          snprintf(w, sizeof w, "%s: %s has a source", pk_species_name(sp), pk_move_name(m.moves[i]));
          CHECK(srcs != 0, w);
          /* and it is a LEVEL-UP move this species already has at this level, which is
           * the strictly stronger statement the builder actually promises. */
          int need = pk2_line_min_levelup(sp, m.moves[i]);
          snprintf(w, sizeof w, "%s: %s is learnable by L%u", pk_species_name(sp),
                   pk_move_name(m.moves[i]), (unsigned)lvl);
          CHECK(need >= 0 && need <= (int)lvl, w);
        }

        /* (C) ability slot: CreateBoxMon writes personality & 1, and ONLY when the
         * species has a second ability (src/pokemon.c:2296-2300). */
        if (pk_species_ability(sp, 1) != 0) {
          snprintf(w, sizeof w, "%s: ability slot = PID & 1", pk_species_name(sp));
          CHECK(m.abilityNum == (uint8_t)(k_pids[pi] & 1u), w);
        } else {
          CHECK(m.abilityNum == 0, "one-ability species keeps slot 0");
        }

        /* (C) met level says exactly what the origin gate decided, and nothing else. */
        if (gen3_species_can_hatch(sp)) CHECK(m.metLevel == 0, "hatchable -> met level 0");
        else CHECK(m.metLevel == lvl, "no egg route -> met at the build level");

        /* (C) the level itself, out of the record's own bytes: a box mon stores no level,
         * so it IS the exp — this is the one assertion that catches a builder writing the
         * level and the exp out of step, and the one that would have caught the L5
         * Charizard had the builder ever been asked for a Charizard's own level. */
        CHECK(decoded_level(sp, &m) == lvl, "the record decodes at the level it was built at");

        /* (C) the pre-write safety gate still holds for the new record shape. */
        CHECK(gen3_edit_roundtrip_ok(rec, false), "lossless edit round-trip");
      }
    }
  }
  printf("     %d species x 3 origin games x 2 PID parities\n", nsp);
  CHECK(nsp == 386, "the sweep really covered 386 species");
}

/* ================================ (D) THE SWEEP ============================== */

static int part_d_sweep(void) {
  printf("(D) the sweep: pk_check_legality2_ex(PK2_RUN_PIDIV) on the RE-DECODED bytes\n");
  CHECK(lg2_have_data(), "learnsets2.c is linked in (else this measures nothing)");
  CHECK(pk_wild_have_data(), "encounters.c is linked in (else this measures nothing)");

  int per_game_legal[3] = { 0, 0, 0 };
  int legal = 0, quest = 0, ill = 0, invalid_rows = 0, n = 0;
  int legal_nopidiv = 0;

  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    for (int gi = 0; gi < 3; gi++) {
      for (int pi = 0; pi < 2; pi++) {
        uint8_t rec[80];
        PkMon m;
        build_and_decode(sp, build_lvl(sp), k_pids[pi], k_games[gi], rec, &m);

        Pk2Report R;
        pk_check_legality2_ex(&m, &R, PK2_RUN_PIDIV);
        n++;
        if (R.grade == PK2_LEGAL) { legal++; per_game_legal[gi] += (pi == 0); }
        else if (R.grade == PK2_QUESTIONABLE) quest++;
        else ill++;
        /* Name the survivors once (Emerald origin, odd PID) — a count with no species
         * attached is not something anyone can act on. */
        if (R.grade != PK2_LEGAL && k_games[gi] == 3 && pi == 0) {
          printf("       %-12s L%-3u %-13s", pk_species_name(sp), (unsigned)build_lvl(sp),
                 pk2_grade_name(R.grade));
          for (int i = 0; i < R.n; i++)
            if (R.row[i].sev != PK2_INFO) printf(" | %s", R.row[i].text);
          printf("\n");
        }
        invalid_rows += R.n_invalid;
        for (int i = 0; i < R.n; i++) if (R.row[i].sev != PK2_INFO) tally(&R.row[i]);

        /* ...and the same record under the flags the box sweep uses (no RNG search). */
        Pk2Report R0;
        pk_check_legality2_ex(&m, &R0, 0);
        if (R0.grade == PK2_LEGAL) legal_nopidiv++;

        /* Nothing this tool builds may ever be IMPOSSIBLE. A SUSPECT is an honest
         * "PokeDNA cannot prove this one"; an INVALID is a record retail could not
         * hold, and writing one into a user's save is the failure mode with teeth. */
        CHECK(R.n_invalid == 0, "no INVALID row on any built record");
      }
    }
  }

  printf("     builds=%d   LEGAL=%d  QUESTIONABLE=%d  ILLEGAL=%d  (INVALID rows: %d)\n",
         n, legal, quest, ill, invalid_rows);
  printf("     per origin game, of 386 (odd PID): Ruby=%d  Emerald=%d  FireRed=%d\n",
         per_game_legal[0], per_game_legal[1], per_game_legal[2]);
  printf("     without PK2_RUN_PIDIV: LEGAL=%d of %d\n", legal_nopidiv, n);
  printf("     reasons still reported:\n");
  for (int i = 0; i < g_nreason; i++)
    printf("       %4d  [%s] %s\n", g_rn[i], pk2_sev_name((uint8_t)g_rsev[i]), g_reason[i]);

  /* THE FLOOR — and the one legitimate reason it ever moves DOWN.
   *
   * This number measures "species the builder emits that the CHECKER accepts", so it has
   * two inputs, and only one of them falling is a regression. When T_evo shipped it went
   * 361 -> 212 per origin game: the builder did not get worse, the auditor got sharper
   * (a L5 Charizard was now correctly caught). Teaching the BUILDER the same table took
   * it back up — the species that were only failing on "Evolves at L36, this one is L5"
   * are now built at L36 and have nothing to answer for. Lowering a floor to make a
   * change pass is exactly the anti-pattern this comment used to warn about, so the count
   * alone is no longer the guard — the categorical assertion below is. Every non-LEGAL
   * species must be non-LEGAL for a reason we can NAME:
   *     - it has no egg route, so its caught origin carries an unforged PID/IV pair, or
   *     - it is Mew/Deoxys, which want an event flag no table ships.
   * "Its evolution floor is above the build level" USED to be on that list and has been
   * struck off deliberately: the builder now builds at that floor, so a species that
   * still fails on it is a real failure, not an excuse.
   * A species that goes non-LEGAL for any OTHER reason is a real regression, and that
   * fires here no matter which way the raw count moved. */
  CHECK(ill == 0, "no built record grades ILLEGAL");

  int unexplained = 0;
  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    uint8_t rec2[80]; PkMon m2; Pk2Report R2;
    build_and_decode(sp, build_lvl(sp), k_pids[0], 3, rec2, &m2);
    pk_check_legality2_ex(&m2, &R2, PK2_RUN_PIDIV);
    if (R2.grade == PK2_LEGAL) continue;
    int no_egg   = !gen3_species_can_hatch(sp);
    int event    = (sp == 151 || sp == 410);          /* MEW, DEOXYS */
    if (!no_egg && !event) {
      printf("  !! UNEXPLAINED non-LEGAL: %s (%u, built L%u)\n", pk_species_name(sp), sp,
             (unsigned)build_lvl(sp));
      for (int i = 0; i < R2.n; i++)
        if (R2.row[i].sev != PK2_INFO) printf("       | %s\n", R2.row[i].text);
      unexplained++;
    }
  }
  CHECK(unexplained == 0, "every non-LEGAL species is non-LEGAL for a NAMED reason");

  /* THE MEASURED FLOOR. 361, and it is not an arbitrary number: it is 386 minus exactly
   * the 25 species part (A) refuses an egg origin, which is the same set part (D)'s
   * categorical assertion just enumerated. Every species the builder CAN give an honest
   * hatched origin to now grades LEGAL — the evolution excuse is gone because the builder
   * builds at the floor. The history of this line: 361 (before T_evo) -> 212 (T_evo shipped
   * and the checker started catching the builder's L5 evolved forms) -> 361 again (the
   * builder learned the same table). Reaching the old number the honest way, not by
   * lowering the bar.
   *
   * The number is deliberately NOT keyed to whether T_evo is linked: it is 361 either way
   * (measured), because a table-less build turns the checker's evolution hook off and the
   * builder's floor off together. Only the message below distinguishes them. */
  int have_evo = (pk_evo_floor(6) != PK_EVO_NO_DATA);
  int want = 361;
  for (int gi = 0; gi < 3; gi++)
    CHECK(per_game_legal[gi] >= want, "the per-game LEGAL count holds at its floor");
  CHECK(legal >= want * 6, "the LEGAL count holds across every game x parity combination");
  printf("     floor: >= %d/386 per game (%s)\n",
         want, have_evo ? "T_evo linked" : "T_evo ABSENT — weak fallback");
  return legal;
}

/* ================================ (E) THE BITE =============================== */

/* Re-create the exact record gen3_build_mon used to emit, from the record it emits now:
 * the Tackle placeholder, met level = the build level, no ability-slot bit, and the
 * hard-coded level 5. If the checker stays quiet on THAT, part (D) proves nothing. */
static void part_e_bite(void) {
  printf("(E) the bite: the pre-fix defaults must still be caught\n");
  int old_bad = 0, new_bad = 0, tackle_rows = 0, ability_rows = 0, met_rows = 0, evo_rows = 0;

  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    uint8_t rec[80];
    PkMon m;
    build_and_decode(sp, build_lvl(sp), k_pids[0], 3, rec, &m);

    Pk2Report Rn;
    pk_check_legality2_ex(&m, &Rn, PK2_RUN_PIDIV);
    if (Rn.grade != PK2_LEGAL) new_bad++;

    /* roll the four fixes back, one record at a time */
    EditMon e;
    gen3_edit_load(rec, false, &e);
    em_set_move(&e, 0, 33);                       /* TACKLE, the old placeholder */
    for (int i = 1; i < 4; i++) em_set_move(&e, i, 0);
    em_set_metlevel(&e, BUILD_LVL);               /* the old caught origin */
    em_set_ability(&e, 0);                        /* the old unset slot */
    em_set_level(&e, BUILD_LVL);                  /* the old hard-coded level 5 */
    uint8_t old[80];
    gen3_edit_commit(&e, old);
    PkMon om;
    memset(&om, 0, sizeof om);
    pk_decode_mon(old, false, &om);

    Pk2Report Ro;
    pk_check_legality2_ex(&om, &Ro, PK2_RUN_PIDIV);
    if (Ro.grade != PK2_LEGAL) old_bad++;
    for (int i = 0; i < Ro.n; i++) {
      if (Ro.row[i].sev == PK2_INFO) continue;
      if (strstr(Ro.row[i].text, "TACKLE")) tackle_rows++;
      else if (strstr(Ro.row[i].text, "Ability slot")) ability_rows++;
      else if (strstr(Ro.row[i].text, "Evolves at")) evo_rows++;
      else if (strstr(Ro.row[i].text, "wild") || strstr(Ro.row[i].text, "Met at")) met_rows++;
    }
  }
  printf("     pre-fix: %d of 386 species non-LEGAL   (TACKLE rows %d, ability rows %d,"
         " encounter rows %d, evolution-level rows %d)\n",
         old_bad, tackle_rows, ability_rows, met_rows, evo_rows);
  printf("     post-fix: %d of 386 species non-LEGAL\n", new_bad);

  /* Each of the four fixes must be independently load-bearing. The evolution row is the
   * newest one and it is Guy's bug verbatim: put the level back to 5 and "Evolves at
   * L36, this one is L5" comes back for every species that has a level-gated stage. */
  CHECK(tackle_rows  > 200, "reintroducing Tackle brings back its rows");
  CHECK(ability_rows > 100, "unsetting the ability slot brings back its rows");
  CHECK(met_rows     > 100, "the caught origin brings back the encounter rows");
  CHECK(evo_rows     > 100, "putting the level back to 5 brings back the evolution rows");
  /* How many species the BUILDER's own fixes recover. Stated as a difference, not as an
   * absolute post-fix count, because the post-fix number also moves whenever the CHECKER
   * gains a rule — T_evo took it from 25 to 174 non-LEGAL without the builder changing at
   * all. The difference isolates the builder, which is what part (E) is here to measure. */
  CHECK(old_bad >= 380, "the pre-fix builder produced almost nothing the checker accepts");
  CHECK(old_bad - new_bad >= 200, "the builder's fixes recover 200+ species on their own");
}

/* ============ (F) WHAT THE CHECKER STILL CANNOT SEE ========================== */

static void part_f_known_blind_spots(void) {
  printf("(F) documented blind spots (asserted so they cannot be forgotten)\n");

  /* THE CHARIZARD, both ways round — the whole history of this bug in four assertions.
   *
   * Once, the builder emitted every species at L5 and the checker had no evolution rule,
   * so a L5 Charizard graded LEGAL (research-legal-generator.md §1). T_evo closed the
   * CHECKER half: the L5 record is now caught. Guy then found the other half on hardware
   * — "the charizard is lvl 5 ... though it does come out questionable" — because the
   * builder was still hard-coding 5 and letting the checker complain about its own
   * output. gen3_build_level closed that one.
   *
   * Both directions are pinned, because either one silently reverting recreates the bug:
   * the L5 record must still be CAUGHT (or the checker went blind) and the CREATED
   * record must be LEGAL at L36 (or the builder went back to 5). */
  uint8_t rec[80];
  PkMon m;
  Pk2Report R;

  build_and_decode(6 /* CHARIZARD */, BUILD_LVL, k_pids[0], 3, rec, &m);
  pk_check_legality2_ex(&m, &R, PK2_RUN_PIDIV);
  /* Note how this nearly failed to fire once: it was written to notice T_evo's arrival,
   * but this test's cc line did not list source/evolutions.c, so the weak no-data
   * fallback answered and the assertion stayed green while asserting something that had
   * become false. A tripwire is only armed if it is LINKED against the thing it watches. */
  CHECK(R.grade != PK2_LEGAL, "an explicitly-L5 CHARIZARD is still CAUGHT (floor is L36)");
  printf("     L5 CHARIZARD grades %s (floor %d)\n",
         pk2_grade_name(R.grade), pk_evo_floor(6));

  build_and_decode(6, build_lvl(6), k_pids[0], 3, rec, &m);
  pk_check_legality2_ex(&m, &R, PK2_RUN_PIDIV);
  CHECK(decoded_level(6, &m) == 36, "a CREATED CHARIZARD is L36, not L5");
  CHECK(R.grade == PK2_LEGAL, "...and grades LEGAL with nothing to answer for");
  printf("     CREATED CHARIZARD is L%u and grades %s\n",
         (unsigned)decoded_level(6, &m), pk2_grade_name(R.grade));

  /* And the honest half: for the species with no egg route the checker is NOT silent.
   * A built GROUDON is flagged, which is the whole point of refusing to fake its
   * origin — a SUSPECT the user can see beats a green banner that lied. Groudon does not
   * evolve, so its floor is 1 and it is still built at the base level 5; app_create_mon
   * shows its NO EGG ROUTE warning BEFORE the editor opens. */
  CHECK(build_lvl(405) == BUILD_LVL, "GROUDON has no evolution floor, so it stays at L5");
  build_and_decode(405 /* GROUDON */, build_lvl(405), k_pids[0], 3, rec, &m);
  pk_check_legality2_ex(&m, &R, PK2_RUN_PIDIV);
  CHECK(R.grade == PK2_QUESTIONABLE, "a built GROUDON is flagged, not waved through");
  CHECK(m.metLevel == BUILD_LVL && m.metLocation < MET_SPECIAL_EGG,
        "...and it claims no exemption to get there");
  printf("     GROUDON grades %s with %d suspect row(s)\n", pk2_grade_name(R.grade), R.n_suspect);
}

int main(void) {
  part_a_origin_gate();
  part_a2_level_floor();
  part_bc_markers_and_structure();
  int legal = part_d_sweep();
  part_e_bite();
  part_f_known_blind_spots();

  printf("host_legalbuild_test: %d checks, %d failures  (LEGAL builds: %d)\n",
         g_checks, g_fail, legal);
  printf("host_legalbuild_test: %s\n", g_fail ? "FAILURES" : "ALL PASS");
  return g_fail ? 1 : 0;
}
