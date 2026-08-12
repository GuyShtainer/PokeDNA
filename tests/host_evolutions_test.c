/* Host test for the forward evolution table (source/evolutions.h) and the legality
 * rule it unlocks (pk2_hook_evolution in source/gen3_legality_hooks.c).
 *
 *   cc -std=c11 -O2 -I source tests/host_evolutions_test.c source/evolutions.c \
 *      source/gen3_legality_hooks.c source/gen3_legality2.c source/learnsets2.c \
 *      source/encounters.c source/statics.c source/gen3_pidiv.c source/gen3_edit.c \
 *      source/gen3_mon.c \
 *      source/gen3_save.c source/gen3_box.c source/gen3_daycare.c source/data_tables.c \
 *      source/ui_font.c -o /tmp/hev && /tmp/hev
 *
 * source/evolutions.c, source/encounters.c and source/learnsets2.c are GENERATED and
 * git-ignored — run tools/gen_evolutions.py --from-rom, tools/gen_encounters.py
 * --from-rom and tools/gen_learnsets2.py first, or the link fails (the same contract
 * host_encounters_test.c and host_legality_hooks_test.c already carry).
 *
 * WHY THIS EXISTS. Half of what PokeDNA's create flow calls LEGAL was an evolved form
 * standing at level 5: 182 of the 361 species, including a Charizard, because the
 * checker had no evolution rule at all. This file is the proof that the table is right
 * and that the rule built on it neither passes a level-5 Charizard nor flags a
 * legitimately-caught Poliwhirl.
 *
 * Six parts:
 *   (A) THE TABLE — the generated forward index, checked for the invariants the lookup
 *       code assumes (sorted, in range, acyclic) and cross-checked BOTH WAYS against
 *       the pre-evolution table gen3_legality_hooks.c already shipped. Two independent
 *       derivations of the same 184 facts: one from the decomps, one from the carts.
 *   (B) THE FLOOR WALK — the minimum level per species, RE-DERIVED here by walking the
 *       forward index in the opposite direction from the generator, then compared. A
 *       shared bug would have to be made twice, in two languages, in two directions.
 *       Plus the pinned cases (Charizard 36, Crobat 22, Espeon 1, ...) by NAME, so a
 *       mistyped species id reads as "CROBAT expected 22, got 7" rather than passing.
 *   (C) THE RELAXATION — the retail wild tables put evolved forms BELOW their own
 *       evolution level. Every wild row in every cart is swept: the checker floor must
 *       never sit above a level the game itself hands out, or the rule would call a
 *       legitimately-caught Pokemon illegal.
 *   (D) THE RULE — a level-5 Charizard must be flagged and a level-36 one must not,
 *       each exemption must silence it, and the row must fit the 240 px screen row.
 *   (E) THE BITE — the rule must survive being aimed at every legitimate mon in Guy's
 *       five cartridge saves without producing a single new finding.
 *   (F) THE RE-MEASURE — the whole create flow swept again with the rule live, so the
 *       headline number is honest about what the checker now knows.
 *
 * Fail-open (a build WITHOUT the generated table must say "not checked", never "clean")
 * cannot be proven from a binary that links the table in; tests/host_evo_nodata_test.c
 * is the same catalogue linked without evolutions.c and asserts exactly that.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "evolutions.h"
#include "encounters.h"
#include "gen3_edit.h"
#include "gen3_mon.h"
#include "gen3_save.h"
#include "gen3_box.h"
#include "gen3_legality2.h"
#include "gen3_legality_hooks.h"
#include "data_tables.h"
#include "ui_font.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

#define NSPECIES     412
#define MAX_SPECIES  411

/* Internal species ids (gen3_mon.h:20), each pinned to its NAME in part (A) so a typo
 * here cannot quietly weaken a check. */
#define SP_CHARMANDER   4
#define SP_CHARMELEON   5
#define SP_CHARIZARD    6
#define SP_ALAKAZAM    65
#define SP_MAGIKARP   129
#define SP_GYARADOS   130
#define SP_CROBAT     169
#define SP_ESPEON     196
#define SP_BLISSEY    242
#define SP_JYNX       124
#define SP_POLIWHIRL   61
#define SP_SHEDINJA   303
#define SP_MILOTIC    329
#define SP_GENGAR      94

#define MET_SPECIAL_EGG   0xFD
#define MET_INGAME_TRADE  0xFE
#define MET_FATEFUL       0xFF
#define RIBBON_FATEFUL    0x80000000u

/* ---- text metrics: the same copy of ui_ptext_w() host_textfit_test.c uses ---- */
#define ROW_BUDGET 220       /* 240 px screen minus the severity glyph + gutter */
static int pwidth(const char* s) {
  int w = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    unsigned c = *p;
    if (c < 32u || c > 127u) c = '?';
    w += ui_font_w[c - 32];
  }
  return w;
}

/* ================================ (A) THE TABLE ============================== */

/* {species, its name, its expected evolution floor}. The names are the whole point:
 * they turn "the floor of 169 is 22" into "the floor of CROBAT is 22", so a shifted
 * species numbering fails here instead of silently checking the wrong Pokemon. */
static const struct { uint16_t sp; const char* name; int floor; const char* why; }
k_pinned[] = {
  { SP_CHARIZARD, "CHARIZARD", 36, "Charmander -> L16 Charmeleon -> L36 Charizard" },
  { SP_CHARMELEON,"CHARMELEON",16, "Charmander -> L16" },
  { SP_CROBAT,    "CROBAT",    22, "Zubat -> L22 Golbat, then FRIENDSHIP (no level)" },
  { SP_ESPEON,    "ESPEON",     1, "Eevee is a base form and FRIENDSHIP has no level" },
  { SP_BLISSEY,   "BLISSEY",    1, "Chansey is a base form; FRIENDSHIP has no level" },
  { SP_MILOTIC,   "MILOTIC",    1, "Feebas is a base form; BEAUTY has no level" },
  { SP_ALAKAZAM,  "ALAKAZAM",  16, "Abra -> L16 Kadabra, then TRADE (no level)" },
  { SP_SHEDINJA,  "SHEDINJA",  20, "Nincada -> L20 via EVO_LEVEL_SHEDINJA" },
  { SP_GYARADOS,  "GYARADOS",  20, "Magikarp -> L20" },
};

static void part_a_table(void) {
  printf("(A) the generated forward index\n");
  CHECK(pk_evo_have_data(), "evolutions.c is linked in (else this measures nothing)");
  int n = pk_evo_count();
  printf("     %d links\n", n);
  CHECK(n == 184, "184 links, the number gEvolutionTable holds in all five carts");

  /* Invariants pk_evo_list's binary search depends on. */
  int level_links = 0;
  for (int i = 0; i < n; i++) {
    const PkEvoLink* e = pk_evo_row(i);
    CHECK(e != 0, "pk_evo_row in range");
    if (!e) continue;
    if (i) CHECK(pk_evo_row(i - 1)->from <= e->from, "s_evo is sorted by source species");
    CHECK(e->from >= 1 && e->from <= MAX_SPECIES, "source species in range");
    CHECK(e->into >= 1 && e->into <= MAX_SPECIES, "target species in range");
    CHECK(e->from != e->into, "nothing evolves into itself");
    CHECK(e->method >= 1 && e->method <= PK_EVO_BEAUTY, "method id in range");
    if (pk_evo_method_is_level(e->method)) {
      level_links++;
      CHECK(e->param >= 1 && e->param <= 100, "a level method's param is a level");
    }
  }
  CHECK(pk_evo_row(-1) == 0 && pk_evo_row(n) == 0, "pk_evo_row rejects out of range");
  printf("     %d of them impose a level\n", level_links);
  CHECK(level_links == 141, "141 level-gated links (LEVEL 134 + the 7 conditional ones)");

  /* pk_evo_list must find every row, and only that species' rows. */
  int listed = 0;
  for (uint16_t sp = 0; sp < NSPECIES; sp++) {
    const PkEvoLink* rows = 0;
    int k = pk_evo_list(sp, &rows);
    listed += k;
    for (int i = 0; i < k; i++) CHECK(rows[i].from == sp, "pk_evo_list slice is one species");
  }
  CHECK(listed == n, "pk_evo_list partitions the whole table");
  CHECK(pk_evo_list(60000, 0) == 0, "pk_evo_list is safe above the species range");

  /* ---- the cross-check that matters: BOTH directions against s_preevo -------
   * gen3_legality_hooks.c's s_preevo came from the pret decomps (tools/gen_legality.py);
   * this table came from Guy's cartridges. Two sources, one fact — if they disagree the
   * shape scan located the wrong bytes, and neither may ship. Checked both ways so a
   * table that is merely a SUBSET cannot pass. */
  int fwd_missing = 0, back_missing = 0, disagree = 0;
  for (int i = 0; i < n; i++) {
    const PkEvoLink* e = pk_evo_row(i);
    uint16_t pre = pk2_preevo(e->into);
    if (pre == 0) { back_missing++; printf("     !! ROM: %s <- %s, s_preevo has nothing\n",
                                           pk_species_name(e->into), pk_species_name(e->from)); }
    else if (pre != e->from) {
      /* Not automatically an error: three Gen-3 species are reachable from one
       * pre-evolution only (Wurmple, Nincada, Tyrogue splits), but no species has two
       * DIFFERENT pre-evolutions, so this really is a disagreement. */
      disagree++;
      printf("     !! %s: ROM says <- %s, s_preevo says <- %s\n", pk_species_name(e->into),
             pk_species_name(e->from), pk_species_name(pre));
    }
  }
  for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++) {
    uint16_t pre = pk2_preevo(sp);
    if (!pre) continue;
    const PkEvoLink* rows = 0;
    int k = pk_evo_list(pre, &rows), hit = 0;
    for (int i = 0; i < k; i++) if (rows[i].into == sp) hit = 1;
    if (!hit) { fwd_missing++; printf("     !! s_preevo: %s <- %s, the ROM table has no such link\n",
                                      pk_species_name(sp), pk_species_name(pre)); }
  }
  printf("     cross-check vs s_preevo: %d missing forward, %d missing backward, %d disagreements\n",
         fwd_missing, back_missing, disagree);
  CHECK(fwd_missing == 0 && back_missing == 0 && disagree == 0,
        "the cart table and the shipped pre-evolution table agree exactly");

  /* Species ids really are the ones the pinned floors below think they are. */
  for (unsigned i = 0; i < sizeof k_pinned / sizeof k_pinned[0]; i++) {
    char w[96];
    snprintf(w, sizeof w, "species %d is %s (it is \"%s\")", k_pinned[i].sp,
             k_pinned[i].name, pk_species_name(k_pinned[i].sp));
    CHECK(strcmp(pk_species_name(k_pinned[i].sp), k_pinned[i].name) == 0, w);
  }
}

/* ================================ (B) THE FLOOR WALK ========================= */

/* Re-derive the floor from the FORWARD index, propagating downstream — the opposite
 * direction from the generator, which memoised backwards from each species to its base.
 * Same answer from the other end, or one of the two walks is wrong. */
static void part_b_floor(void) {
  printf("(B) the minimum-level walk, re-derived from the forward index\n");

  static int mine[NSPECIES];
  for (int i = 0; i < NSPECIES; i++) mine[i] = 1;     /* base forms are unconstrained */

  /* Relax to a fixpoint. Gen-3 lines are at most 3 deep, so this settles in <= 3
   * sweeps; the bound is a guard against a table that a bad scan made cyclic. */
  int sweeps = 0, changed = 1;
  while (changed && sweeps < 16) {
    changed = 0; sweeps++;
    for (int i = 0; i < pk_evo_count(); i++) {
      const PkEvoLink* e = pk_evo_row(i);
      /* A level method drags the child's floor up to its param; a stone / trade /
       * friendship / beauty method leaves the parent's floor exactly as it is. */
      int cand = pk_evo_method_is_level(e->method)
                   ? (mine[e->from] > (int)e->param ? mine[e->from] : (int)e->param)
                   : mine[e->from];
      /* Several routes can reach one species (Nidoqueen, Gallade-style splits do not
       * exist in Gen 3, but Poliwrath/Politoed does): the cheapest route wins. */
      if (mine[e->into] == 1 || cand < mine[e->into]) {
        if (mine[e->into] != cand) { mine[e->into] = cand; changed = 1; }
      }
    }
  }
  CHECK(sweeps < 16, "the floor walk reached a fixpoint (the graph is acyclic)");

  int diffs = 0, constrained = 0;
  for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++) {
    if (pk_national_no(sp) == 0) continue;
    int theirs = pk_evo_min_level(sp);
    if (mine[sp] > 1) constrained++;
    if (theirs != mine[sp]) {
      if (diffs < 10)
        printf("     !! %-11s generated floor %d, re-derived %d\n",
               pk_species_name(sp), theirs, mine[sp]);
      diffs++;
    }
  }
  printf("     %d species carry an evolution floor above L1; %d disagreements\n",
         constrained, diffs);
  CHECK(diffs == 0, "the independent walk reproduces every generated floor");

  for (unsigned i = 0; i < sizeof k_pinned / sizeof k_pinned[0]; i++) {
    char w[160];
    int got = pk_evo_min_level(k_pinned[i].sp);
    snprintf(w, sizeof w, "%s floor is %d, got %d (%s)", k_pinned[i].name,
             k_pinned[i].floor, got, k_pinned[i].why);
    CHECK(got == k_pinned[i].floor, w);
    printf("     %-11s floor L%-3d  %s\n", k_pinned[i].name, got, k_pinned[i].why);
  }

  CHECK(pk_evo_min_level(60000) == PK_EVO_NO_DATA, "out-of-range species -> NO_DATA");
  CHECK(pk_evo_min_level(0) == 1, "SPECIES_NONE is not claimed to be constrained");
}

/* ================================ (C) THE RELAXATION ========================= */

/* THE RULE MUST NEVER CONTRADICT THE CARTRIDGE. If the game itself puts a species in a
 * wild slot at level L, then level L is legal for it, full stop — and Gen 3 really does
 * place evolved forms below their evolution level. This sweeps every wild row in every
 * generated table and asserts the checker floor sits at or below it. */
static void part_c_relaxation(void) {
  printf("(C) the wild relaxation: the floor may never exceed a real wild level\n");
  CHECK(pk_wild_have_data(), "encounters.c is linked in (else this measures nothing)");

  int rows = 0, violations = 0, relaxed = 0, shown = 0;
  for (int g = PK_ENC_SAPPHIRE; g <= PK_ENC_LEAFGREEN; g++) {
    for (int ms = 0; ms < 256; ms++) {
      const PkWildEntry* e = 0;
      int k = pk_wild_mapsec_list((PkEncGame)g, (uint8_t)ms, &e);
      for (int i = 0; i < k; i++) {
        rows++;
        int floor = pk_evo_floor(e[i].species);
        if (floor > e[i].minlvl) {
          violations++;
          if (shown++ < 10)
            printf("     !! %s is wild at L%d in game %d but the floor says L%d\n",
                   pk_species_name(e[i].species), e[i].minlvl, g, floor);
        }
      }
    }
  }
  for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++) {
    int ev = pk_evo_min_level(sp), w = pk_evo_wild_min(sp);
    CHECK(pk_evo_floor(sp) <= ev, "the checker floor never exceeds the evolution floor");
    if (w > 0 && w < ev) {
      relaxed++;
      CHECK(pk_evo_floor(sp) == w, "...and drops to the wild level when there is one");
    } else {
      CHECK(pk_evo_floor(sp) == ev, "...and equals it when no wild row is lower");
    }
  }
  printf("     %d wild rows swept, %d contradictions; %d species are catchable below "
         "their evolution floor\n", rows, violations, relaxed);
  CHECK(violations == 0, "no wild row in any cart contradicts the checker floor");
  CHECK(relaxed >= 20, "the relaxation is load-bearing (it is not a no-op)");

  /* The two cases that make the point, named. */
  printf("     %-11s evolves at L%d, wild from L%d -> checker floor L%d\n",
         pk_species_name(SP_POLIWHIRL), pk_evo_min_level(SP_POLIWHIRL),
         pk_evo_wild_min(SP_POLIWHIRL), pk_evo_floor(SP_POLIWHIRL));
  printf("     %-11s evolves at L%d, wild from L%d -> checker floor L%d\n",
         pk_species_name(SP_GYARADOS), pk_evo_min_level(SP_GYARADOS),
         pk_evo_wild_min(SP_GYARADOS), pk_evo_floor(SP_GYARADOS));
  CHECK(pk_evo_floor(SP_POLIWHIRL) < pk_evo_min_level(SP_POLIWHIRL),
        "FireRed's Safari Zone Poliwhirl is below its own evolution level");
  CHECK(pk_evo_floor(SP_GYARADOS) < pk_evo_min_level(SP_GYARADOS),
        "Sootopolis' Super Rod Gyarados is below its own evolution level");
}

/* ================================ (D) THE RULE =============================== */

/* Build a record for `sp` at `lvl` with an ordinary caught origin, then judge the
 * RE-DECODED bytes — never the struct that produced them. */
static void judge(uint16_t sp, uint8_t lvl, uint8_t metloc, uint8_t game,
                  uint32_t ribbons, Pk2Report* R, PkMon* out) {
  uint8_t rec[80];
  /* Build with a real origin game, then stamp the one under test — gen3_build_mon
   * only speaks the five GBA ids, and origin 15 (Colosseum/XD) is one of the cases
   * this part has to exercise. */
  gen3_build_mon(sp, lvl, 0x1234ABCDu, 0x00010002u, "GUY", game <= 5 ? game : 3, rec);
  EditMon e;
  gen3_edit_load(rec, false, &e);
  em_set_metloc(&e, metloc);
  em_set_metlevel(&e, lvl < 5 ? lvl : 5);
  em_set_metgame(&e, game);
  /* Ribbons are the Misc substruct's u32 at +0x08, so bit 31 is the top byte of
   * sub[3][11] — the same reach-in host_legality_hooks_test.c:517 uses, because no
   * em_set_ribbons() exists (and should not: nothing in the tool writes that bit). */
  if (ribbons) e.sub[3][11] = 0x80;
  gen3_edit_commit(&e, rec);
  PkMon m;
  memset(&m, 0, sizeof m);
  pk_decode_mon(rec, false, &m);
  if (out) *out = m;
  pk_check_legality2(&m, R);
}

static int has_evo_row(const Pk2Report* R, char* text, int cap) {
  for (int i = 0; i < R->n; i++)
    if (strncmp(R->row[i].text, "Evolves at L", 12) == 0) {
      if (text) snprintf(text, cap, "%s", R->row[i].text);
      return 1;
    }
  return 0;
}

static void part_d_rule(void) {
  printf("(D) the rule itself\n");
  Pk2Report R;
  char t[PK2_TEXT_LEN];

  /* THE HEADLINE. This exact record graded LEGAL with zero rows before the table. */
  judge(SP_CHARIZARD, 5, 0x11 /* a real mapsec */, 3, 0, &R, 0);
  CHECK(has_evo_row(&R, t, sizeof t), "a L5 CHARIZARD is flagged");
  CHECK(R.grade != PK2_LEGAL, "...and no longer grades LEGAL");
  CHECK((R.hooks_absent & PK2_HOOK_EVO) == 0, "...with the evolution hook present");
  printf("     L5  CHARIZARD -> %-13s \"%s\"\n", pk2_grade_name(R.grade), t);

  /* The boundary, both sides of it. 36 is legal, 35 is not. */
  judge(SP_CHARIZARD, 35, 0x11, 3, 0, &R, 0);
  CHECK(has_evo_row(&R, 0, 0), "L35 CHARIZARD is still flagged");
  judge(SP_CHARIZARD, 36, 0x11, 3, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "L36 CHARIZARD is NOT flagged");
  printf("     L35 flagged, L36 clean — the boundary is exactly the evolution level\n");

  /* A base form is never touched, whatever its level. */
  judge(SP_CHARMANDER, 5, 0x11, 3, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "a L5 CHARMANDER is not flagged");

  /* The relaxation, end to end: a Poliwhirl at the level FireRed's Safari Zone
   * actually hands out must survive the rule. Without pk_evo_floor's wild term this
   * is a false positive on a legitimately-caught Pokemon. */
  judge(SP_POLIWHIRL, (uint8_t)pk_evo_wild_min(SP_POLIWHIRL), 0x65, 4, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "a Poliwhirl at its real wild level is not flagged");
  judge(SP_GYARADOS, 5, 0x11, 3, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "a L5 Gyarados is not flagged (Sootopolis Super Rod)");

  /* ---- the exemptions, one at a time -------------------------------------- */
  /* An in-game trade hands over a Pokemon at the level of the one you gave away
   * (src/trade.c:4552), so FireRed's Poliwhirl-for-Jynx trade really can produce a
   * Jynx below Smoochum's L30. Met 0xFE must silence the rule. */
  judge(SP_JYNX, 5, MET_INGAME_TRADE, 4, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "met 0xFE (in-game trade) exempts the rule");
  judge(SP_CHARIZARD, 5, MET_FATEFUL, 3, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "met 0xFF (fateful/event) exempts the rule");
  judge(SP_CHARIZARD, 5, MET_SPECIAL_EGG, 3, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "met 0xFD (special egg) exempts the rule");
  judge(SP_CHARIZARD, 5, 0x11, 15, 0, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "Colosseum/XD origin exempts the rule");
  judge(SP_CHARIZARD, 5, 0x11, 3, RIBBON_FATEFUL, &R, 0);
  CHECK(!has_evo_row(&R, 0, 0), "the fateful ribbon bit exempts the rule");
  /* ...and the control: the same record WITHOUT an exemption is still flagged, which
   * is what proves the five above went quiet because of the exemption and not because
   * the fixture happens to be clean. */
  judge(SP_CHARIZARD, 5, 0x11, 3, 0, &R, 0);
  CHECK(has_evo_row(&R, 0, 0), "the un-exempted control is still flagged");

  /* Every message the rule can emit has to fit the row it is drawn into. The text is
   * built at runtime, so it cannot be pinned as a literal in host_textfit_test.c. */
  int worst_px = 0, worst_ch = 0;
  for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++) {
    int fl = pk_evo_floor(sp);
    if (fl <= 1) continue;
    judge(sp, 2, 0x11, 3, 0, &R, 0);
    if (!has_evo_row(&R, t, sizeof t)) continue;
    int px = pwidth(t), ch = (int)strlen(t);
    if (px > worst_px) worst_px = px;
    if (ch > worst_ch) worst_ch = ch;
    if (px > ROW_BUDGET || ch >= PK2_TEXT_LEN - 1) {
      printf("     !! \"%s\" is %d chars / %d px\n", t, ch, px);
      CHECK(0, "an evolution row overflows the screen row");
    }
  }
  printf("     widest row: %d chars / %d px (budget %d / %d)\n",
         worst_ch, worst_px, PK2_TEXT_LEN - 2, ROW_BUDGET);
  CHECK(worst_px > 0, "the width sweep actually produced rows");
}

/* ================================ (E) THE BITE ============================== */

/* The gate host_legality_hooks_test.c part (C) holds every check to, applied to this
 * one: Guy's cartridge saves are 650-odd real Pokemon, and a new rule that condemns one
 * of them on its own evidence is a false positive, not a discovery.
 *
 * The gate is "never the SOLE finding", not "never fires", and that is the honest
 * shape. The corpus is not all legitimate: LeafGreen's party is Guy's editor bench,
 * five deliberately injected mons. One of them — a level-5 GENGAR — is exactly what
 * this rule exists to catch (Haunter does not appear until L25, and Gengar is wild
 * nowhere in Gen 3), and the pre-existing checker independently condemns the same
 * record for a move it cannot learn. Demanding zero firings would mean demanding the
 * rule stay blind to a real forgery; demanding zero UNCORROBORATED firings is the
 * statement that actually protects legitimate Pokemon. */
static int has_other_finding(const Pk2Report* R) {
  for (int i = 0; i < R->n; i++)
    if (R->row[i].sev != PK2_INFO && strncmp(R->row[i].text, "Evolves at L", 12) != 0)
      return 1;
  return 0;
}

/* Saves arrive as argv[1] .. argv[argc-1]; tests/run_host_tests.py hands every test
 * that mentions argv[1] the local five-cart corpus (and the checked-in fixtures when
 * the carts are not on this machine). */
static int part_e_corpus(int argc, char** argv) {
  printf("(E) the corpus gate: %d save(s)\n", argc - 1);
  if (argc < 2) {
    printf("     (no saves supplied — pass *.sav to run this part)\n");
    return 0;
  }
  int mons = 0, fired = 0, alone = 0;
  for (int a = 1; a < argc; a++) {
    FILE* fp = fopen(argv[a], "rb");
    if (!fp) { printf("     (cannot open %s)\n", argv[a]); continue; }
    static uint8_t save[G3_SAVE_FILE_SIZE];
    size_t got = fread(save, 1, sizeof save, fp);
    fclose(fp);
    Gen3SaveInfo info;
    if (!gen3_parse(save, (uint32_t)got, &info)) {
      printf("     (%s is not a Gen-3 save)\n", argv[a]);
      continue;
    }
    static uint8_t sb1[G3_SAVEBLOCK1_BYTES], pc[G3_PC_BYTES];
    gen3_read_saveblock1(save, info.slot, sb1);
    gen3_read_pc_storage(save, info.slot, pc);

    const char* base = strrchr(argv[a], '/');
    base = base ? base + 1 : argv[a];

    int here = 0;
    PkMon party[6], box[30];
    bool frlg = false;
    int np = pk_read_party_auto(sb1, party, &frlg);
    for (int b = -1; b < G3_TOTAL_BOXES; b++) {
      PkMon* set = party;
      int cnt = np;
      if (b >= 0) { pk_read_box(pc, b, box); set = box; cnt = 30; }
      for (int s = 0; s < cnt; s++) {
        if (set[s].species == 0) continue;
        mons++;
        Pk2Report R;
        pk_check_legality2(&set[s], &R);
        char t[PK2_TEXT_LEN];
        if (!has_evo_row(&R, t, sizeof t)) continue;
        here++;
        int solo = !has_other_finding(&R);
        alone += solo;
        printf("     %s %s %s %d: %-11s %-32s %s\n", solo ? "!!" : "  ", base,
               b < 0 ? "party" : "box  ", b < 0 ? s : b, pk_species_name(set[s].species),
               t, solo ? "NOTHING ELSE FLAGS IT" : "(another check flags it too)");
      }
    }
    fired += here;
    printf("     %-28s %s\n", base, here ? "fired" : "silent");
  }
  printf("     %d real Pokemon, %d evolution findings, %d of them uncorroborated\n",
         mons, fired, alone);
  CHECK(mons > 100, "the corpus really was read (else this part proves nothing)");
  CHECK(alone == 0, "the rule never condemns a real Pokemon on its own evidence");
  /* A broken wild relaxation does not show up as an uncorroborated finding — it shows
   * up as DOZENS of findings, because 23 species are catchable below their evolution
   * level and Guy's carts are full of them. This bound is what makes part (E) able to
   * fail if pk_evo_floor stops taking the wild term. */
  CHECK(fired <= 5, "the rule is not firing wholesale on real cartridge data");
  return mons;
}

/* ================================ (F) THE RE-MEASURE ======================== */

/* The level app_create_mon passes and the three origin bytes it can pass — the same
 * fixture space host_legalbuild_test.c sweeps, re-run with the evolution rule live so
 * the create flow's headline number stops resting on the checker not knowing. */
#define BUILD_LVL 5
static const uint8_t k_games[3] = { 2, 3, 4 };

static void part_f_remeasure(void) {
  printf("(F) the create flow, re-measured with the rule live\n");
  int legal = 0, quest = 0, ill = 0, n = 0, evo_only = 0, per_game[3] = { 0, 0, 0 };
  int floor_gt5 = 0;

  for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++) {
    if (pk_national_no(sp) == 0) continue;
    if (pk_evo_floor(sp) > BUILD_LVL) floor_gt5++;
    for (int gi = 0; gi < 3; gi++) {
      uint8_t rec[80];
      gen3_build_mon(sp, BUILD_LVL, 0x1234ABCDu, 0x00010002u, "GUY", k_games[gi], rec);
      PkMon m;
      memset(&m, 0, sizeof m);
      pk_decode_mon(rec, false, &m);
      Pk2Report R;
      pk_check_legality2_ex(&m, &R, PK2_RUN_PIDIV);
      n++;
      if (R.grade == PK2_LEGAL) { legal++; per_game[gi]++; }
      else if (R.grade == PK2_QUESTIONABLE) quest++;
      else ill++;
      /* How many of the losses are THIS rule and nothing else — i.e. how much of the
       * old headline number was the missing evolution table. */
      if (R.grade != PK2_LEGAL && R.n_suspect == 1 && R.n_invalid == 0 &&
          has_evo_row(&R, 0, 0)) evo_only++;
    }
  }
  printf("     builds=%d  LEGAL=%d  QUESTIONABLE=%d  ILLEGAL=%d\n", n, legal, quest, ill);
  printf("     per origin game, of 386: Ruby=%d  Emerald=%d  FireRed=%d\n",
         per_game[0], per_game[1], per_game[2]);
  printf("     %d species cannot stand at L%d at all; %d builds lost to the evolution "
         "rule ALONE\n", floor_gt5, BUILD_LVL, evo_only);
  CHECK(ill == 0, "the builder still never produces an ILLEGAL record");
  CHECK(floor_gt5 >= 140, "the rule has real reach (>=140 species cannot be L5)");
  /* The old floor was 361 of 386 per game. It MUST have fallen: a checker that got
   * sharper reports fewer passes. Guarding both ends means neither a regression that
   * re-silences the rule nor a build change that quietly re-inflates the number can
   * slip through unnoticed. */
  for (int gi = 0; gi < 3; gi++) {
    CHECK(per_game[gi] < 361, "the LEGAL count FELL — the checker got sharper");
    CHECK(per_game[gi] >= 200, "...but not to nothing; the other 200+ still build clean");
  }
}

int main(int argc, char** argv) {
  part_a_table();
  part_b_floor();
  part_c_relaxation();
  part_d_rule();
  part_e_corpus(argc, argv);
  part_f_remeasure();

  printf("host_evolutions_test: %d checks, %d failures\n", g_checks, g_fail);
  printf("host_evolutions_test: %s\n", g_fail ? "FAILURES" : "ALL PASS");
  return g_fail ? 1 : 0;
}
