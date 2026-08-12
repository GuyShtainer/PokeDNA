/* Host test for the Legality V2 table-driven hooks (source/gen3_legality_hooks.c).
 *   cc -std=c11 -O2 -I source tests/host_legality_hooks_test.c source/gen3_legality_hooks.c \
 *      source/gen3_legality2.c source/learnsets2.c source/encounters.c source/statics.c \
 *      source/evolutions.c \
 *      source/gen3_pidiv.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_edit.c source/gen3_daycare.c \
 *      source/data_tables.c source/ui_font.c -o /tmp/hlh
 *   /tmp/hlh /path/to/Emerald.sav /path/to/Ruby.sav ...
 *
 * source/learnsets2.c, source/encounters.c and source/statics.c are GENERATED and
 * git-ignored — run tools/gen_learnsets2.py, tools/gen_encounters.py --from-rom and
 * tools/gen_statics.py --from-rom before this test, or the link above fails (same
 * contract as host_learnsets2_test.c / host_encounters_test.c / host_statics_test.c).
 * -O2 matters: part (C) runs the 65,536-iteration PIDIV search over the whole corpus.
 *
 * Six parts:
 *   (A) the pre-evolution table this module carries — sorted, acyclic, and spot-checked
 *       against the species NAMES, because a mistyped id would silently widen a check
 *       instead of failing anything;
 *   (B) every message the hooks can emit must fit the 240 px row in PokeDNA's 5x7
 *       proportional face (the standing rule of tests/host_textfit_test.c, applied here
 *       because these strings are built at runtime and cannot be pinned as literals);
 *   (C) the GOLDEN GATE: the full hooked catalogue over Guy's own cartridge saves must
 *       produce ZERO INVALID rows. An INVALID on a real Pokemon is a false positive and
 *       a bug. The SUSPECT/INFO distribution PER CHECK is printed — that table is the
 *       P5 calibration input;
 *   (D) negative tests: one deliberately-broken mon per check, each asserting that its
 *       OWN check fires and that the clean control stays silent;
 *   (E) the exemptions and the tri-state: bred / event / in-game-trade / Colosseum-XD /
 *       Smeargle mons must NOT be judged, and a missing table must produce no verdict;
 *   (F) THE TOOL'S OWN OUTPUT: a Pokemon straight out of gen3_build_mon must be judged
 *       by these hooks and not waved through. It used to be stamped met location 255
 *       (METLOC_FATEFUL_ENCOUNTER), which (E) exempts — so PokeDNA hid its own creations
 *       from its own checker. Every (F) assertion is paired with the same record at 255,
 *       which must go silent; that pairing is what proves the checker woke up rather
 *       than that a fixture happens to be clean.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "gen3_legality2.h"
#include "gen3_legality_hooks.h"
#include "gen3_pidiv.h"
#include "learnsets2.h"
#include "encounters.h"
#include "statics.h"
#include "data_tables.h"
#include "ui_font.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

/* species / move ids used by the fixtures (INTERNAL species ids, gen3_mon.h:20) */
#define SP_BULBASAUR    1
#define SP_CHARMANDER   4
#define SP_CHARIZARD    6
#define SP_SMEARGLE   235
#define SP_POOCHYENA  286
#define SP_MIGHTYENA  287
#define SP_KECLEON    317   /* the Devon-Scope static, the bug this all comes from */
#define SP_ELECTRODE  101   /* what New Mauville's static VOLTORB evolves into      */
#define SP_BELDUM     398
#define SP_LATIAS     407    /* the two RSE roamers                                */
/* (F) fixtures. One ability each (so no PID/ability row muddies the assertions) and
 * TACKLE — gen3_build_mon's placeholder move — learnable at L1, so the only rows they
 * can raise are the encounter ones under test. */
#define SP_SKITTY     315    /* wild in Emerald, NOT on Route 101                  */
#define SP_PIDGEY      16    /* Kanto Route 1, L2-5                                */
#define SP_CATERPIE    10    /* wild in FireRed, NOT on Route 1                    */
#define MV_POUND        1    /* nothing in Gen 3 teaches Pound to Bulbasaur        */
#define MV_PETAL_DANCE 80    /* Bulbasaur EGG move only                            */
#define MV_DRAGON_RAGE 82    /* Charmander L43 / Charizard L54 — the pre-evo case  */
#define MV_SYNTHESIS  235    /* Bulbasaur level-up ONLY, L39                       */
#define MAPSEC_R102  0x11    /* Emerald Route 102: POOCHYENA L3-4                  */
#define MAPSEC_R105  0x14    /* Emerald Route 105: water only, no POOCHYENA        */
#define MAPSEC_R120  0x23    /* Hoenn Route 120: wild KECLEON L25-25 AND the L30
                              * Devon-Scope static — the regression below         */
#define MAPSEC_R101  0x10    /* Hoenn Route 101 — what gen3_build_mon now stamps   */
#define MAPSEC_R1    0x65    /* Kanto Route 1 — ditto, for a FireRed/LeafGreen origin */
#define METLOC_FATEFUL 0xFF  /* METLOC_FATEFUL_ENCOUNTER — the stamp that was wrong */

/* ---- (B) text metrics: a copy of ui_ptext_w(), as host_textfit_test.c does ---- */
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
static void want_fits(const char* s, const char* what) {
  g_checks++;
  int w = pwidth(s), n = (int)strlen(s);
  if (n >= PK2_TEXT_LEN - 1 || w > ROW_BUDGET) {
    printf("  !! FAIL: %s — \"%s\" is %d chars / %d px (max %d / %d)\n",
           what, s, n, w, PK2_TEXT_LEN - 2, ROW_BUDGET);
    g_fail++;
  }
}

/* ---- fixtures ------------------------------------------------------------- */
/* One fixture = an EditMon plus the record its PkMon points into (PkMon.raw is a
 * back-reference, so the record must outlive it). Same shape as the fixtures in
 * tests/host_legality2_test.c. */
typedef struct { EditMon e; uint8_t rec[100]; PkMon m; } Fix;

/* Every fixture is pinned to Emerald Route 102 so that a case which cares about the met
 * location states its own, and one that does not is never accidentally reading whatever
 * gen3_build_mon happens to stamp today. (Part (F) is the exception: it tests that stamp,
 * so it builds without the override.) */
static void fix_new(Fix* x, uint16_t species, uint8_t lvl) {
  uint8_t base[80];
  gen3_build_mon(species, lvl, 0x1234ABCDu, 0x00010002u, "GUY", 3, base);
  gen3_edit_load(base, false, &x->e);
  em_set_metloc(&x->e, MAPSEC_R102);
  /* ...and pinned to a CAUGHT origin for the same reason. gen3_build_mon now stamps met
   * level 0 (= hatched) on every species with an egg route, which is a legitimate
   * exemption from the move level-window, the encounter hook and the PID/IV search — so
   * a fixture that inherited it would silently stop exercising all three. Cases that
   * want the bred exemption set it themselves (part (E) does). */
  em_set_metlevel(&x->e, lvl);
}
static Pk2Report fix_check_ex(Fix* x, uint8_t flags) {
  memset(x->rec, 0, sizeof x->rec);
  gen3_edit_commit(&x->e, x->rec);
  Pk2Report R;
  if (!pk_decode_mon(x->rec, x->e.is_party, &x->m)) { printf("  !! fixture decoded empty\n"); g_fail++; }
  pk_resolve(&x->m);
  pk_check_legality2_ex(&x->m, &R, flags);
  return R;
}
static Pk2Report fix_check(Fix* x) { return fix_check_ex(x, 0); }

/* Rows carry runtime data ("SYNTHESIS: needs L39, mon is L5"), so most assertions
 * match a SUBSTRING rather than the whole row. */
static const Pk2Row* row_like(const Pk2Report* R, const char* frag) {
  for (int i = 0; i < R->n; i++) if (strstr(R->row[i].text, frag)) return &R->row[i];
  return 0;
}
static void dump(const Pk2Report* R) {
  for (int i = 0; i < R->n; i++)
    printf("       [%s] %-8s %s\n", pk2_cat_name(R->row[i].cat), pk2_sev_name(R->row[i].sev), R->row[i].text);
}
static void want_like(const Pk2Report* R, const char* frag, int sev, int cat, const char* what) {
  g_checks++;
  const Pk2Row* r = row_like(R, frag);
  if (!r || r->sev != sev || r->cat != cat) {
    printf("  !! FAIL: %s — wanted \"%s\" as %s/%s, got %s\n", what, frag,
           pk2_cat_name((uint8_t)cat), pk2_sev_name((uint8_t)sev),
           r ? pk2_sev_name(r->sev) : "nothing");
    dump(R);
    g_fail++;
  }
}
static void want_no_row(const Pk2Report* R, const char* frag, const char* what) {
  g_checks++;
  const Pk2Row* r = row_like(R, frag);
  if (r) {
    printf("  !! FAIL: %s — \"%s\" fired when it must not: %s\n", what, frag, r->text);
    g_fail++;
  }
}

/* ---- (C) calibration tally ------------------------------------------------- */
/* One bucket per CHECK, not per message: the move rows embed the move name, so
 * tallying raw text would produce a hundred one-row lines instead of the
 * distribution the calibration pass needs. */
enum {
  CK_MV_NOSRC = 0, CK_MV_LEVEL, CK_MV_EGG,
  CK_ENC_LEVEL, CK_ENC_PLACE,
  CK_PID_OK, CK_PID_ROAMER, CK_PID_NONE, CK_PID_EXEMPT,
  CK_OTHER, CK_N
};
static const char* ck_name[CK_N] = {
  "C3/4/5  move has no known source",
  "C2      move above its learn level",
  "C5      egg move on an unbred mon",
  "E2      met level outside wild range",
  "E2      species not wild at met place",
  "B1      PIDIV method matched",
  "B1      PIDIV roamer (weak) match",
  "B1      PIDIV no method matched",
  "B1      PIDIV not testable (exempt)",
  "(core, not a hook)",
};
static struct { int n[3]; char ex[3][80]; int nex; } g_ck[CK_N];

static int classify(const Pk2Row* r) {
  if (r->cat == PK2_CAT_MOVES) {
    if (strstr(r->text, ": no way to learn it")) return CK_MV_NOSRC;
    if (strstr(r->text, ": needs L"))            return CK_MV_LEVEL;
    if (strstr(r->text, ": egg move"))           return CK_MV_EGG;
  } else if (r->cat == PK2_CAT_MET) {
    if (strstr(r->text, "Met at L"))             return CK_ENC_LEVEL;
    if (strstr(r->text, "Not found wild"))       return CK_ENC_PLACE;
  } else if (r->cat == PK2_CAT_PID) {
    if (strstr(r->text, "not testable"))         return CK_PID_EXEMPT;
    if (strstr(r->text, "roamer"))               return CK_PID_ROAMER;
    if (strstr(r->text, "No PID/IV"))            return CK_PID_NONE;
    if (strstr(r->text, "seed 0x"))              return CK_PID_OK;
  }
  return CK_OTHER;
}
static void tally(const Pk2Row* r, const PkMon* m) {
  int c = classify(r);
  g_ck[c].n[r->sev]++;
  if (g_ck[c].nex < 3 && c != CK_PID_OK) {
    static const char* gname[6] = { "?", "S", "R", "E", "FR", "LG" };
    snprintf(g_ck[c].ex[g_ck[c].nex], sizeof g_ck[c].ex[0], "%s L%d %s/L%d %s: %s",
             pk_species_name(m->species), m->level,
             m->metGame < 6 ? gname[m->metGame] : "GC", m->metLevel,
             pk_location_name(m->metLocation), r->text);
    g_ck[c].nex++;
  }
}

/* ---- (C) the golden gate --------------------------------------------------- */
/* Exclusions are the ones tests/host_legality2_test.c already justified: garbage
 * slots (bad checksum / impossible species) and the LeafGreen party's hand-edited
 * mons (EV totals of 1512+, which no retail game can produce). You cannot calibrate
 * a false-positive gate against a Pokemon somebody built with an editor. */
static int g_saves, g_tested, g_invalid, g_excluded;
static int g_cov_evolved, g_cov_wildmatch, g_cov_bred, g_cov_pidiv_ran;

static void visit(PkMon* m) {
  pk_resolve(m);
  int evsum = 0;
  for (int i = 0; i < PK_NSTATS; i++) evsum += m->evs[i];
  if (m->isBadEgg || m->species < 1 || m->species > 411 || pk_national_no(m->species) == 0 ||
      evsum > 510) { g_excluded++; return; }

  Pk2Report R;
  pk_check_legality2_ex(m, &R, PK2_RUN_PIDIV);
  g_tested++;
  if (pk2_preevo(m->species)) g_cov_evolved++;
  if (!m->isEgg && m->metLevel > 0 && m->metGame >= 1 && m->metGame <= 5 &&
      m->metLocation < 0xFD &&
      pk2_line_wild_at(m->metGame, m->metLocation, m->species, 0, 0) == PK_WILD_YES)
    g_cov_wildmatch++;
  if (m->isEgg || m->metLevel == 0) g_cov_bred++;
  if (R.pidiv_ran) g_cov_pidiv_ran++;

  for (int i = 0; i < R.n; i++) {
    tally(&R.row[i], m);
    if (R.row[i].sev == PK2_INVALID && classify(&R.row[i]) != CK_OTHER) {
      g_invalid++;
      printf("    INVALID from a HOOK on a real mon: %-11s L%-3d met %s/L%d game %d -> %s\n",
             pk_species_name(m->species), m->level, pk_location_name(m->metLocation),
             m->metLevel, m->metGame, R.row[i].text);
    }
  }
}

static void run_save(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  (skip, cannot open %s)\n", path); return; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("  parse FAILED: %s\n", path); g_fail++; return; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES], pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);
  g_saves++;

  const char* base = strrchr(path, '/');
  int t0 = g_tested, i0 = g_invalid;
  PkMon party[6]; bool frlg = false;
  int np = pk_read_party_auto(sb1, party, &frlg);
  for (int i = 0; i < np; i++) visit(&party[i]);
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    PkMon box[30];
    pk_read_box(pc, b, box);
    for (int s = 0; s < 30; s++) if (box[s].species != 0) visit(&box[s]);
  }
  printf("  %-28s %3d mons checked, %d INVALID from a hook\n", base ? base + 1 : path,
         g_tested - t0, g_invalid - i0);
}

/* ---- (D)/(E) negatives and exemptions -------------------------------------- */

/* The LCRNG, written from the decomp facts (include/random.h:12-16, src/random.c:11-16)
 * rather than by calling gen3_pidiv.c, so a wrong constant there cannot cancel out. */
static uint16_t lcrng(uint32_t* s) { *s = *s * 1103515245u + 24691u; return (uint16_t)(*s >> 16); }

static void negatives(void) {
  Fix x;
  Pk2Report R;

  /* The wiring itself: linking this module must make the two sweep hooks STRONG, so
   * the catalogue stops reporting them as absent (gen3_legality2.h:119-126). The
   * PIDIV bit stays clear because an unrun hook never sets it. */
  fix_new(&x, SP_BULBASAUR, 5);
  R = fix_check(&x);
  CHECK(R.hooks_absent == 0, "the moves, encounter and evolution hooks are linked in");
  /* gen3_build_mon's Tackle-at-L5 Bulbasaur is legal in every table, so the sweep
   * hooks must be silent on it. (The PIDIV hook is NOT expected to be: a hand-built
   * PID with all-zero IVs is exactly the uncorrelated pair it exists to notice.) */
  if (R.n) { printf("  !! FAIL: a clean built mon raised %d hook row(s):\n", R.n); dump(&R); g_fail++; }
  g_checks++;
  R = fix_check_ex(&x, PK2_RUN_PIDIV);
  CHECK(R.hooks_absent == 0, "the PIDIV hook is linked in too");

  /* ---- C2: the level window ------------------------------------------------
   * Bulbasaur's SYNTHESIS is level-up-only for the whole line and is learned at
   * L39 in every game group, so a level-5 Bulbasaur cannot have it. */
  CHECK(pk2_line_sources(SP_BULBASAUR, MV_SYNTHESIS) == LG2_SRC_LEVELUP,
        "fixture assumption: SYNTHESIS is level-up-only for Bulbasaur");
  CHECK(pk2_line_min_levelup(SP_BULBASAUR, MV_SYNTHESIS) == 39,
        "fixture assumption: Bulbasaur learns SYNTHESIS at L39");

  fix_new(&x, SP_BULBASAUR, 5); em_set_move(&x.e, 1, MV_SYNTHESIS);
  R = fix_check(&x);
  want_like(&R, "SYNTHESIS: needs L39", PK2_INVALID, PK2_CAT_MOVES, "C2 level window");
  CHECK(R.grade == PK2_ILLEGAL, "C2 grades the mon ILLEGAL");

  fix_new(&x, SP_BULBASAUR, 39); em_set_move(&x.e, 1, MV_SYNTHESIS);
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "C2 control: at exactly the learn level it is fine");

  fix_new(&x, SP_BULBASAUR, 40); em_set_move(&x.e, 1, MV_SYNTHESIS);
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "C2 control: above the learn level it is fine");

  /* ...and the pre-evolution half of the rule. Charizard learns DRAGON RAGE at
   * L54, Charmander at L43, so a level-50 Charizard that learned it before
   * evolving is LEGAL. Without the pre-evo chain this is the false INVALID the
   * whole table exists to prevent. */
  CHECK(pk2_line_min_levelup(SP_CHARIZARD, MV_DRAGON_RAGE) == 43 &&
        lg2_min_levelup_level(PK_EMERALD, SP_CHARIZARD, MV_DRAGON_RAGE) > 50,
        "fixture assumption: DRAGON RAGE is L43 on Charmander, >L50 on Charizard");
  fix_new(&x, SP_CHARIZARD, 50); em_set_move(&x.e, 1, MV_DRAGON_RAGE);
  R = fix_check(&x);
  want_no_row(&R, "DRAGON RAGE", "C2 honours the pre-evolution's earlier learn level");
  fix_new(&x, SP_CHARIZARD, 42); em_set_move(&x.e, 1, MV_DRAGON_RAGE);
  R = fix_check(&x);
  want_like(&R, "DRAGON RAGE: needs L43", PK2_INVALID, PK2_CAT_MOVES,
            "C2 still fires below the pre-evolution's level");

  /* ---- C3/C4/C5: no known source ------------------------------------------ */
  fix_new(&x, SP_BULBASAUR, 50); em_set_move(&x.e, 1, MV_POUND);
  R = fix_check(&x);
  want_like(&R, "POUND: no way to learn it", PK2_SUSPECT, PK2_CAT_MOVES, "no-source move");
  CHECK(R.grade == PK2_QUESTIONABLE, "a sourceless move grades QUESTIONABLE, not ILLEGAL");

  /* ---- C5: an egg move on a mon that never was an egg ---------------------- */
  CHECK(pk2_line_sources(SP_BULBASAUR, MV_PETAL_DANCE) == LG2_SRC_EGG,
        "fixture assumption: PETAL DANCE reaches Bulbasaur only as an egg move");
  fix_new(&x, SP_BULBASAUR, 50); em_set_move(&x.e, 1, MV_PETAL_DANCE);
  R = fix_check(&x);
  want_like(&R, "PETAL DANCE: egg move", PK2_SUSPECT, PK2_CAT_MOVES, "egg-only move, unbred");

  /* ...and the same mon hatched (metLevel 0) must be silent. */
  fix_new(&x, SP_BULBASAUR, 50); em_set_move(&x.e, 1, MV_PETAL_DANCE); em_set_metlevel(&x.e, 0);
  R = fix_check(&x);
  want_no_row(&R, "PETAL DANCE", "a hatched mon may know its egg moves");

  /* ---- E2: species at met location + level window -------------------------- */
  {
    uint8_t lo = 0, hi = 0;
    CHECK(pk2_line_wild_at(PK_ENC_EMERALD, MAPSEC_R102, SP_POOCHYENA, &lo, &hi) == PK_WILD_YES &&
          lo == 3 && hi == 4, "fixture assumption: Emerald Route 102 has POOCHYENA L3-4");
    CHECK(pk2_line_wild_at(PK_ENC_EMERALD, MAPSEC_R105, SP_POOCHYENA, 0, 0) == PK_WILD_NO,
          "fixture assumption: Emerald Route 105 has no POOCHYENA");
    CHECK(pk_wild_anywhere(PK_ENC_EMERALD, SP_BELDUM) == PK_WILD_NO,
          "fixture assumption: BELDUM is wild nowhere in Emerald");
  }

  fix_new(&x, SP_POOCHYENA, 10);
  em_set_metgame(&x.e, 3); em_set_metloc(&x.e, MAPSEC_R102); em_set_metlevel(&x.e, 3);
  R = fix_check(&x);
  want_no_row(&R, "wild", "E2 control: caught where and when it could be");

  fix_new(&x, SP_POOCHYENA, 30);
  em_set_metgame(&x.e, 3); em_set_metloc(&x.e, MAPSEC_R102); em_set_metlevel(&x.e, 30);
  R = fix_check(&x);
  want_like(&R, "Met at L30, wild there is L3-4", PK2_SUSPECT, PK2_CAT_MET, "E2 level window");

  fix_new(&x, SP_POOCHYENA, 10);
  em_set_metgame(&x.e, 3); em_set_metloc(&x.e, MAPSEC_R105); em_set_metlevel(&x.e, 5);
  R = fix_check(&x);
  want_like(&R, "Not found wild at its met location", PK2_SUSPECT, PK2_CAT_MET, "E2 wrong place");

  /* An evolved mon carries the met data of the form that was CAUGHT: Mightyena on
   * Route 102 at L3 is a Poochyena that evolved, and must stay silent. */
  fix_new(&x, SP_MIGHTYENA, 30);
  em_set_metgame(&x.e, 3); em_set_metloc(&x.e, MAPSEC_R102); em_set_metlevel(&x.e, 3);
  R = fix_check(&x);
  want_no_row(&R, "wild", "E2 honours the pre-evolution's encounter row");

  /* ---- E2 REGRESSION: THE DEVON-SCOPE KECLEON -------------------------------
   * Reported from hardware, in these words: "The sweep is a cool feature but it catches
   * legit keckleons i caught in a legit way." Three of them in Guy's Emerald box 1 and
   * two more in his Ruby, every one caught in normal play, every one flagged "Met at
   * L30, wild there is L25-25".
   *
   * The flag was not a threshold that needed tuning — the hook was reasoning from data
   * that does not describe how a Kecleon is obtained. Routes 118-123 carry an ordinary
   * wild KECLEON row at L25-25, but the Kecleon a player actually meets there is the
   * invisible object event revealed with the Devon Scope, which a map script starts at
   * its own level: `setwildbattle SPECIES_KECLEON, 30` (pokeemerald
   * data/scripts/kecleon.inc:74, data/maps/Route120/scripts.inc:193).
   *
   * Both fixture assumptions below are the ones that made the bug: the wild row really
   * does exist and really does say 25-25, so this is NOT the "wild nowhere" case the
   * next block covers — the hook had a row, matched it, and drew the wrong conclusion. */
  {
    uint8_t lo = 0, hi = 0;
    CHECK(pk2_line_wild_at(PK_ENC_EMERALD, MAPSEC_R120, SP_KECLEON, &lo, &hi) == PK_WILD_YES &&
          lo == 25 && hi == 25,
          "fixture assumption: Emerald Route 120 has a wild KECLEON row at L25-25");
    CHECK(pk2_line_scripted(PK_ENC_EMERALD, SP_KECLEON) == PK_STATIC_YES,
          "fixture assumption: KECLEON is script-placed in Emerald");

    /* Guy's mon, reconstructed: Emerald, Route 120, met at L30. Must be SILENT. */
    for (int game = 1; game <= 3; game++) {          /* Sapphire, Ruby, Emerald */
      fix_new(&x, SP_KECLEON, 30);
      em_set_metgame(&x.e, (uint8_t)game);
      em_set_metloc(&x.e, MAPSEC_R120);
      em_set_metlevel(&x.e, 30);
      R = fix_check(&x);
      want_no_row(&R, "Met at L", "a Devon-Scope KECLEON is not accused of its met level");
      want_no_row(&R, "Not found wild", "...nor of its met place");
      g_checks++;
      if (R.grade != PK2_LEGAL) {
        printf("  !! FAIL: a legitimately caught KECLEON graded %s\n", pk2_grade_name(R.grade));
        dump(&R);
        g_fail++;
      }
    }

    /* THE CONTROL, and the load-bearing half of this block: the suppression must be
     * about KECLEON, not about the level window switching itself off. The same met
     * data on an ordinary route species still gets the row. */
    fix_new(&x, SP_POOCHYENA, 30);
    em_set_metgame(&x.e, 3); em_set_metloc(&x.e, MAPSEC_R102); em_set_metlevel(&x.e, 30);
    R = fix_check(&x);
    want_like(&R, "Met at L30, wild there is L3-4", PK2_SUSPECT, PK2_CAT_MET,
              "control: an ordinary species is still judged by the wild table");

    /* And the chain: New Mauville places a VOLTORB at L25, so the ELECTRODE that Voltorb
     * became inherits the same silence — its met data belongs to the form that was
     * caught (the reason every other helper here is a pk2_line_* too). */
    CHECK(pk2_line_scripted(PK_ENC_EMERALD, SP_ELECTRODE) == PK_STATIC_YES,
          "the script placement of VOLTORB covers the ELECTRODE it evolves into");
  }

  /* A species that is wild NOWHERE in the origin game is a gift/static/fossil, so it
   * must produce silence, not a SUSPECT. (The species-with-a-script-placement case is
   * handled earlier and for a stronger reason; this gate still carries the routes
   * statics.h cannot see — roamers and the FRLG Game Corner prizes.) */
  fix_new(&x, SP_BELDUM, 20);
  em_set_metgame(&x.e, 3); em_set_metloc(&x.e, MAPSEC_R105); em_set_metlevel(&x.e, 5);
  R = fix_check(&x);
  want_no_row(&R, "wild", "a never-wild species is not judged by the wild table");

  /* ...and a section with NO wild slots at all (a building, most towns) says nothing
   * by omission: everything obtained there came from a gift or a trade. */
  {
    const PkWildEntry* rows;
    uint8_t empty = 0;
    for (int ms = 1; ms <= 0xD5; ms++)
      if (pk_wild_mapsec_list(PK_ENC_EMERALD, (uint8_t)ms, &rows) == 0) { empty = (uint8_t)ms; break; }
    CHECK(empty != 0, "the Emerald table has a mapsec with no wild slots to test with");
    fix_new(&x, SP_POOCHYENA, 20);
    em_set_metgame(&x.e, 3); em_set_metloc(&x.e, empty); em_set_metlevel(&x.e, 5);
    R = fix_check(&x);
    want_no_row(&R, "Not found wild", "a section with no wild slots produces no verdict");
  }

  /* ---- B1: PIDIV ----------------------------------------------------------- */
  {
    /* Forge a Method-1 mon: one seed, four consecutive outputs, PID low half first. */
    uint32_t seed = 0x00C0FFEEu, s = seed;
    uint16_t lo = lcrng(&s), hi = lcrng(&s), iv1 = lcrng(&s), iv2 = lcrng(&s);
    uint32_t pid = (uint32_t)lo | ((uint32_t)hi << 16);
    uint8_t base[80];
    gen3_build_mon(SP_BULBASAUR, 20, pid, 0x00010002u, "GUY", 3, base);
    Fix p; gen3_edit_load(base, false, &p.e);
    em_set_metlevel(&p.e, 20);   /* caught, not the builder's hatched default: a bred mon
                                  * is PID/IV-exempt and there would be nothing to search */
    em_set_iv(&p.e, PK_HP,  (uint8_t)(iv1 & 31));
    em_set_iv(&p.e, PK_ATK, (uint8_t)((iv1 >> 5) & 31));
    em_set_iv(&p.e, PK_DEF, (uint8_t)((iv1 >> 10) & 31));
    em_set_iv(&p.e, PK_SPE, (uint8_t)(iv2 & 31));
    em_set_iv(&p.e, PK_SPA, (uint8_t)((iv2 >> 5) & 31));
    em_set_iv(&p.e, PK_SPD, (uint8_t)((iv2 >> 10) & 31));

    R = fix_check_ex(&p, 0);
    CHECK(R.pidiv_ran == 0, "PIDIV does not run in the fast sweep");
    want_no_row(&R, "PID/IV", "no PID/IV row without PK2_RUN_PIDIV");

    R = fix_check_ex(&p, PK2_RUN_PIDIV);
    CHECK(R.pidiv_ran == 1, "the PIDIV hook reports that it ran");
    want_like(&R, "Method 1, seed 0x00C0FFEE", PK2_INFO, PK2_CAT_PID, "PIDIV match");
    CHECK(R.pidiv_method == 1 && R.pidiv_seed == seed, "PIDIV method/seed reach the report");
    CHECK(R.grade == PK2_LEGAL, "an INFO PIDIV row leaves the mon LEGAL");

    /* Break the correlation: same PID, perfect IVs — the editor's fingerprint. */
    for (int i = 0; i < PK_NSTATS; i++) em_set_iv(&p.e, i, 31);
    R = fix_check_ex(&p, PK2_RUN_PIDIV);
    want_like(&R, "No PID/IV RNG method matches", PK2_SUSPECT, PK2_CAT_PID, "PIDIV mismatch");
    CHECK(R.pidiv_method == 0, "a mismatch reports no method");
    CHECK(R.grade == PK2_QUESTIONABLE, "a PIDIV mismatch is never worse than QUESTIONABLE");
  }

  /* The RSE roamer. The glitch keeps only the low 8 bits of the first IV word —
   * HP plus the low 3 bits of Attack — and zeroes the rest (gen3_pidiv.h:122-132),
   * so the ordinary 30-bit compare cannot match a legitimately-caught Latias. Build
   * one from a real Method-1 stream, truncated exactly as the save would hold it. */
  {
    uint32_t seed = 0x0BADF00Du, s = seed;
    uint16_t lo = lcrng(&s), hi = lcrng(&s), iv1 = lcrng(&s);
    uint32_t pid = (uint32_t)lo | ((uint32_t)hi << 16);
    uint8_t base[80];
    gen3_build_mon(SP_LATIAS, 40, pid, 0x00010002u, "GUY", 2 /* Ruby */, base);
    Fix p; gen3_edit_load(base, false, &p.e);
    em_set_metloc(&p.e, 0x19);                     /* Route 110, where Guy caught his */
    em_set_iv(&p.e, PK_HP,  (uint8_t)(iv1 & 31));
    em_set_iv(&p.e, PK_ATK, (uint8_t)((iv1 >> 5) & 7));   /* only 3 bits survive */
    R = fix_check_ex(&p, PK2_RUN_PIDIV);
    want_like(&R, "PID/IV roamer match (weak evidence)", PK2_INFO, PK2_CAT_PID, "roamer retry");
    want_no_row(&R, "No PID/IV RNG method matches", "a roamer is not called suspect");

    /* ...and the retry is Latias/Latios-only: the same truncated spread on any other
     * species must NOT get the 8-bit pass. */
    gen3_build_mon(SP_BULBASAUR, 40, pid, 0x00010002u, "GUY", 2, base);
    gen3_edit_load(base, false, &p.e);
    em_set_metloc(&p.e, MAPSEC_R102);
    em_set_metlevel(&p.e, 40);   /* caught: LATIAS above is one of the species the builder
                                  * cannot hatch, so it needs no pin — BULBASAUR does */
    em_set_iv(&p.e, PK_HP,  (uint8_t)(iv1 & 31));
    em_set_iv(&p.e, PK_ATK, (uint8_t)((iv1 >> 5) & 7));
    R = fix_check_ex(&p, PK2_RUN_PIDIV);
    want_like(&R, "No PID/IV RNG method matches", PK2_SUSPECT, PK2_CAT_PID,
              "the roamer retry is not granted to other species");
  }

  /* A hatched mon has no single-stream correlation to test (daycare.c:466). */
  fix_new(&x, SP_BULBASAUR, 20); em_set_metlevel(&x.e, 0);
  R = fix_check_ex(&x, PK2_RUN_PIDIV);
  want_like(&R, "PID/IV not testable (hatched)", PK2_INFO, PK2_CAT_PID, "PIDIV exemption is stated");
  want_no_row(&R, "No PID/IV RNG method matches", "an exempt mon is not called suspect");

  /* B3: an in-game trade's PID comes from a fixed template, so it must be exempted
   * by met location — the exemption gen3_pidiv.c cannot apply itself. The fixture
   * PID here is arbitrary, i.e. it would otherwise be a guaranteed SUSPECT. */
  fix_new(&x, SP_BULBASAUR, 20); em_set_metloc(&x.e, 0xFE);
  R = fix_check_ex(&x, PK2_RUN_PIDIV);
  want_like(&R, "PID/IV not testable (in-game trade)", PK2_INFO, PK2_CAT_PID, "PIDIV trade exemption");
  want_no_row(&R, "No PID/IV RNG method matches", "an in-game trade is not called suspect");
}

static void exemptions(void) {
  Fix x;
  Pk2Report R;

  /* Smeargle SKETCHES anything: no move it holds is evidence. */
  fix_new(&x, SP_SMEARGLE, 20); em_set_move(&x.e, 1, MV_SYNTHESIS); em_set_move(&x.e, 2, MV_POUND);
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "Smeargle is exempt from the level window");
  want_no_row(&R, "POUND", "Smeargle is exempt from the source check");

  /* Bred: BuildEggMoveset can hand a baby any move in its level-up list, whatever
   * the level (daycare.c:704-717) — so a hatched mon escapes C2. */
  fix_new(&x, SP_BULBASAUR, 5); em_set_move(&x.e, 1, MV_SYNTHESIS); em_set_metlevel(&x.e, 0);
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "a hatched mon is exempt from the level window");
  fix_new(&x, SP_BULBASAUR, 5); em_set_move(&x.e, 1, MV_SYNTHESIS); em_set_egg(&x.e, true);
  em_set_metlevel(&x.e, 0);
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "an egg is exempt from the level window");

  /* Event distribution (fateful bit 31), in-game trade (met 0xFE) and Colosseum/XD
   * (origin 15) all carry movesets no learnset table describes. */
  fix_new(&x, SP_BULBASAUR, 5); em_set_move(&x.e, 1, MV_SYNTHESIS); x.e.sub[3][11] = 0x80;
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "an event mon is exempt from the level window");
  fix_new(&x, SP_BULBASAUR, 5); em_set_move(&x.e, 1, MV_SYNTHESIS); em_set_metloc(&x.e, 0xFE);
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "an in-game trade is exempt from the level window");
  fix_new(&x, SP_BULBASAUR, 5); em_set_move(&x.e, 1, MV_SYNTHESIS); em_set_metgame(&x.e, 15);
  R = fix_check(&x);
  want_no_row(&R, "SYNTHESIS", "a Colosseum/XD mon is exempt from the level window");

  /* The same four exemptions on the encounter side. */
  const struct { const char* what; int kind; } enc[] = {
    {"an event mon", 0}, {"an in-game trade", 1}, {"a Colosseum/XD mon", 2}, {"a hatched mon", 3},
  };
  for (unsigned i = 0; i < sizeof enc / sizeof enc[0]; i++) {
    fix_new(&x, SP_POOCHYENA, 30);
    em_set_metgame(&x.e, 3); em_set_metloc(&x.e, MAPSEC_R102); em_set_metlevel(&x.e, 30);
    switch (enc[i].kind) {
      case 0: x.e.sub[3][11] = 0x80; break;
      case 1: em_set_metloc(&x.e, 0xFE); break;
      case 2: em_set_metgame(&x.e, 15); break;
      case 3: em_set_metlevel(&x.e, 0); break;
    }
    R = fix_check(&x);
    want_no_row(&R, "Met at L", enc[i].what);
  }

  /* THE TRI-STATE. A table that is not there must produce no verdict, ever: the
   * wrappers return PK_WILD_NO_DATA and the hook returns without a row. Origin 0
   * and 6..14 have no table by construction, which is the case a real build hits
   * when a save carries a corrupt origin byte. */
  g_checks += 3;
  if (pk2_line_wild_at(0, MAPSEC_R102, SP_POOCHYENA, 0, 0) != PK_WILD_NO_DATA ||
      pk2_line_wild_at(9, MAPSEC_R102, SP_POOCHYENA, 0, 0) != PK_WILD_NO_DATA ||
      pk2_line_wild_anywhere(9, SP_POOCHYENA) != PK_WILD_NO_DATA) {
    printf("  !! FAIL: an absent game table must answer PK_WILD_NO_DATA\n"); g_fail++;
  }
  for (uint8_t game = 6; game <= 14; game += 4) {
    fix_new(&x, SP_POOCHYENA, 30);
    em_set_metgame(&x.e, game); em_set_metloc(&x.e, MAPSEC_R102); em_set_metlevel(&x.e, 30);
    R = fix_check(&x);
    want_no_row(&R, "Met at L", "an unknown origin game yields no encounter verdict");
    want_no_row(&R, "Not found wild", "an unknown origin game yields no location verdict");
  }
}

/* ---- (F) the met location PokeDNA stamps on the mons it creates ------------
 *
 * gen3_build_mon used to write 255 = METLOC_FATEFUL_ENCOUNTER. Two things were wrong
 * with that. It is the marker the retail game puts on an event distribution
 * (data/scripts/gift_pichu.inc:33), so every Pokemon the tool made lied about where it
 * came from. And gen3_legality_hooks.c:195 and :300 exempt every met location >= 0xFD,
 * so the tool's output walked straight past the tool's own checker — part (E) proves
 * that exemption exists, and this part proves PokeDNA is no longer standing inside it.
 *
 * Every case below is asserted twice: once as gen3_build_mon leaves it, and once with
 * 255 put back. The second assertion is the load-bearing one — silence at 255 and a
 * verdict at the real stamp is the difference between "this fixture is clean" and "the
 * checker actually ran". */

/* A fixture EXACTLY as gen3_build_mon leaves it — deliberately no fix_new override. */
static void built(Fix* x, uint16_t species, uint8_t lvl, uint8_t metgame, uint16_t extra_move) {
  uint8_t base[80];
  gen3_build_mon(species, lvl, 0x1234ABCDu, 0x00010002u, "GUY", metgame, base);
  gen3_edit_load(base, false, &x->e);
  /* The met LOCATION — this part's whole subject — is left exactly as the builder wrote
   * it. The met LEVEL is pinned to the caught value because the builder now writes 0
   * (hatched) for any species with an egg route, and "was bred" legitimately exempts the
   * move and encounter hooks (gen3_legality2.c:409-415). That exemption is EARNED, not
   * the 0xFF lie this part exists to catch; without the pin, F3-F6 would go silent for
   * the right reason and stop testing the wrong one. The no-egg-route species — the
   * legendaries and Unown — still come out of the builder caught exactly like this.
   * Coverage of the hatched default lives in tests/host_legalbuild_test.c. */
  em_set_metlevel(&x->e, lvl);
  if (extra_move) em_set_move(&x->e, 1, extra_move);
}

/* ...and the same record with the old stamp back on it: what the checker used to see. */
static Pk2Report as_before(Fix* x, uint16_t species, uint8_t lvl, uint8_t metgame,
                           uint16_t extra_move) {
  built(x, species, lvl, metgame, extra_move);
  em_set_metloc(&x->e, METLOC_FATEFUL);
  return fix_check(x);
}

static void built_met_location(void) {
  Fix x;
  Pk2Report R;

  /* F1 — the byte itself follows the origin game's own region, and is never one of the
   * three non-places (0xFD special egg / 0xFE in-game trade / 0xFF fateful) that the
   * hooks exempt. The loop runs over every origin byte the record can hold, because a
   * default that fell through to 0 for an odd origin would be a silent regression. */
  const struct { uint8_t game; uint8_t want; const char* what; } stamp[] = {
    { 0, MAPSEC_R101, "origin 0 (defaults to Emerald) -> Route 101" },
    { 1, MAPSEC_R101, "Sapphire  -> Route 101" },
    { 2, MAPSEC_R101, "Ruby      -> Route 101" },
    { 3, MAPSEC_R101, "Emerald   -> Route 101" },
    { 4, MAPSEC_R1,   "FireRed   -> Route 1"   },
    { 5, MAPSEC_R1,   "LeafGreen -> Route 1"   },
  };
  for (unsigned i = 0; i < sizeof stamp / sizeof stamp[0]; i++) {
    built(&x, SP_POOCHYENA, 3, stamp[i].game, 0);
    fix_check(&x);
    g_checks++;
    if (x.m.metLocation != stamp[i].want)
      printf("  !! FAIL: %s, got 0x%02X\n", stamp[i].what, x.m.metLocation), g_fail++;
  }
  for (uint8_t game = 0; game <= 15; game++) {
    built(&x, SP_POOCHYENA, 3, game, 0);
    fix_check(&x);
    g_checks++;
    if (x.m.metLocation >= 0xFD) {
      printf("  !! FAIL: origin %u is stamped 0x%02X, a marker the hooks exempt\n",
             game, x.m.metLocation);
      g_fail++;
    }
  }

  /* Fixture assumptions, stated so a table regeneration that moved these rows fails
   * here instead of quietly hollowing out the assertions below. */
  uint8_t lo = 0, hi = 0;
  CHECK(pk2_line_wild_at(PK_ENC_EMERALD, MAPSEC_R101, SP_POOCHYENA, &lo, &hi) == PK_WILD_YES &&
        lo == 2 && hi == 3, "fixture: Emerald Route 101 has POOCHYENA L2-3");
  CHECK(pk2_line_wild_at(PK_ENC_FIRERED, MAPSEC_R1, SP_PIDGEY, &lo, &hi) == PK_WILD_YES &&
        lo == 2 && hi == 5, "fixture: FireRed Route 1 has PIDGEY L2-5");
  CHECK(pk2_line_wild_at(PK_ENC_EMERALD, MAPSEC_R101, SP_SKITTY, 0, 0) == PK_WILD_NO &&
        pk2_line_wild_anywhere(PK_ENC_EMERALD, SP_SKITTY) == PK_WILD_YES,
        "fixture: SKITTY is wild in Emerald but not on Route 101");
  CHECK(pk2_line_wild_at(PK_ENC_FIRERED, MAPSEC_R1, SP_CATERPIE, 0, 0) == PK_WILD_NO &&
        pk2_line_wild_anywhere(PK_ENC_FIRERED, SP_CATERPIE) == PK_WILD_YES,
        "fixture: CATERPIE is wild in FireRed but not on Route 1");

  /* F2 — a created mon whose story ADDS UP is still silent. The fix must not blanket-
   * flag everything the tool makes; a L3 POOCHYENA met at L3 on Route 101 is exactly
   * what that place produces. */
  built(&x, SP_POOCHYENA, 3, 3, 0);
  R = fix_check(&x);
  if (R.n) { printf("  !! FAIL: a coherent created mon raised %d row(s):\n", R.n); dump(&R); g_fail++; }
  g_checks++;
  CHECK(R.grade == PK2_LEGAL, "a coherent created mon still grades LEGAL");

  /* F3 — the level window, Hoenn. Route 101 tops out at L3, so a L30 build cannot have
   * been met there. This row can ONLY come from the encounter hook reading the stamp. */
  built(&x, SP_POOCHYENA, 30, 3, 0);
  R = fix_check(&x);
  want_like(&R, "Met at L30, wild there is L2-3", PK2_SUSPECT, PK2_CAT_MET,
            "F3 the encounter hook judges a created mon's met level");
  R = as_before(&x, SP_POOCHYENA, 30, 3, 0);
  want_no_row(&R, "Met at L", "F3 control: at the old stamp 255 the hook said nothing");

  /* F4 — the place, Hoenn. SKITTY is wild in Emerald but not on Route 101. */
  built(&x, SP_SKITTY, 5, 3, 0);
  R = fix_check(&x);
  want_like(&R, "Not found wild at its met location", PK2_SUSPECT, PK2_CAT_MET,
            "F4 the encounter hook judges a created mon's met place");
  R = as_before(&x, SP_SKITTY, 5, 3, 0);
  want_no_row(&R, "Not found wild", "F4 control: at the old stamp 255 the hook said nothing");

  /* F5 — the same two, for a Kanto origin. This also proves the stamp FOLLOWS the origin
   * game: these verdicts come from the FireRed table at Route 1, a mapsec that does not
   * even exist in the Hoenn games. */
  built(&x, SP_PIDGEY, 3, 4, 0);
  R = fix_check(&x);
  want_no_row(&R, "Met at L", "F5 a coherent Kanto build is silent");
  want_no_row(&R, "Not found wild", "F5 a coherent Kanto build is silent");
  built(&x, SP_PIDGEY, 30, 4, 0);
  R = fix_check(&x);
  want_like(&R, "Met at L30, wild there is L2-5", PK2_SUSPECT, PK2_CAT_MET,
            "F5 Kanto level window");
  R = as_before(&x, SP_PIDGEY, 30, 4, 0);
  want_no_row(&R, "Met at L", "F5 control: 255 muted the Kanto level window too");
  built(&x, SP_CATERPIE, 5, 4, 0);
  R = fix_check(&x);
  want_like(&R, "Not found wild at its met location", PK2_SUSPECT, PK2_CAT_MET,
            "F5 Kanto met place");
  R = as_before(&x, SP_CATERPIE, 5, 4, 0);
  want_no_row(&R, "Not found wild", "F5 control: 255 muted the Kanto place check too");

  /* F6 — the MOVES hook was muted by the same byte, and that is the defect that was
   * actually caught in the wild: a probe built with an impossible moveset came back
   * LEGAL. A L5 BULBASAUR cannot know SYNTHESIS (level-up only, L39 in every game
   * group) unless it was bred, traded or distributed — and a created mon is none of
   * those. INVALID, i.e. the whole mon grades ILLEGAL. */
  built(&x, SP_BULBASAUR, 5, 3, MV_SYNTHESIS);
  R = fix_check(&x);
  want_like(&R, "SYNTHESIS: needs L39", PK2_INVALID, PK2_CAT_MOVES,
            "F6 the moves hook judges a created mon's moveset");
  CHECK(R.grade == PK2_ILLEGAL, "F6 an impossible created moveset grades ILLEGAL");
  R = as_before(&x, SP_BULBASAUR, 5, 3, MV_SYNTHESIS);
  want_no_row(&R, "SYNTHESIS", "F6 control: at the old stamp 255 the moveset went unflagged");
  CHECK(R.grade == PK2_LEGAL, "F6 control: the old stamp graded that same mon LEGAL");
}

/* ---- (A) the pre-evolution table ------------------------------------------- */
static void preevo_table(void) {
  int links = 0;
  uint16_t prev = 0;
  for (uint16_t sp = 1; sp <= 411; sp++) {
    uint16_t pre = pk2_preevo(sp);
    if (!pre) continue;
    links++;
    g_checks++;
    if (pre > 411 || pre == sp || pk_national_no(pre) == 0) {
      printf("  !! FAIL: bad pre-evolution link %u -> %u\n", sp, pre); g_fail++;
    }
    /* Walking up must terminate (a cycle would hang the chain walk on hardware) AND
     * must fit the module's CHAIN_MAX of 4 — a chain the walk truncates would lose
     * an ancestor's earlier learn level, which is exactly how a false INVALID is
     * born. Longest real Gen-3 line is 3 (Azurill/Marill/Azumarill). */
    uint16_t cur = pre;
    int depth = 1;
    for (int guard = 0; guard < 8 && cur; guard++) { cur = pk2_preevo(cur); if (cur) depth++; }
    if (cur) { printf("  !! FAIL: pre-evolution chain from %u does not terminate\n", sp); g_fail++; }
    if (depth + 1 > 4) {
      printf("  !! FAIL: %s has a chain of %d, deeper than CHAIN_MAX\n", pk_species_name(sp), depth + 1);
      g_fail++;
    }
    prev = sp;
  }
  (void)prev;
  CHECK(links == 184, "the pre-evolution table has all 184 Gen-3 links");

  /* Spot-checks by NAME, so a transposed id cannot hide. The last three are the
   * cases a naive parse gets wrong: one pre-evolution with two evolutions, a baby
   * form introduced in Gen 2, and an evolution that is not adjacent by id. */
  const struct { uint16_t evo; const char* evo_name; const char* pre_name; } want[] = {
    {  3, "VENUSAUR",   "IVYSAUR"   },
    {  6, "CHARIZARD",  "CHARMELEON"},
    { 26, "RAICHU",     "PIKACHU"   },
    { 25, "PIKACHU",    "PICHU"     },
    {303, "SHEDINJA",   "NINCADA"   },
    {302, "NINJASK",    "NINCADA"   },
    {169, "CROBAT",     "GOLBAT"    },
    {184, "AZUMARILL",  "MARILL"    },
    {183, "MARILL",     "AZURILL"   },
    {242, "BLISSEY",    "CHANSEY"   },
    {202, "WOBBUFFET",  "WYNAUT"    },
    {287, "MIGHTYENA",  "POOCHYENA" },
  };
  for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
    g_checks++;
    uint16_t pre = pk2_preevo(want[i].evo);
    if (strcmp(pk_species_name(want[i].evo), want[i].evo_name) ||
        strcmp(pk_species_name(pre), want[i].pre_name)) {
      printf("  !! FAIL: %s(%u) <- %s, wanted %s <- %s\n", pk_species_name(want[i].evo),
             want[i].evo, pk_species_name(pre), want[i].evo_name, want[i].pre_name);
      g_fail++;
    }
  }
  CHECK(pk2_preevo(SP_BULBASAUR) == 0, "a base form has no pre-evolution");
  CHECK(pk2_preevo(0) == 0 && pk2_preevo(9999) == 0, "out-of-range species are safe");
}

/* ---- (B) every message the hooks can build --------------------------------- */
static void text_fits(void) {
  /* The widest move name in the table decides the worst case for all three move
   * rows, and the widest level pair ("needs L100, mon is L100") the rest. */
  uint16_t widest = 1;
  for (uint16_t mv = 1; mv <= 354; mv++)
    if (pwidth(pk_move_name(mv)) > pwidth(pk_move_name(widest))) widest = mv;
  printf("  widest move name: \"%s\" (%d px)\n", pk_move_name(widest), pwidth(pk_move_name(widest)));

  char t[PK2_TEXT_LEN * 2];
  snprintf(t, sizeof t, "%s: no way to learn it", pk_move_name(widest));       want_fits(t, "C3/4/5 row");
  snprintf(t, sizeof t, "%s: needs L100, mon is L100", pk_move_name(widest));  want_fits(t, "C2 row");
  snprintf(t, sizeof t, "%s: egg move, was not bred", pk_move_name(widest));   want_fits(t, "C5 row");
  want_fits("Met at L100, wild there is L100-100", "E2 level row");
  want_fits("Not found wild at its met location", "E2 place row");
  want_fits("No PID/IV RNG method matches", "PIDIV mismatch row");
  want_fits("PID/IV roamer match (weak evidence)", "PIDIV roamer row");
  for (uint8_t i = 0; i < PK_PIDIV_NMETHOD; i++) {
    snprintf(t, sizeof t, "%s, seed 0xFFFFFFFF", pk_pidiv_method_name(i));
    want_fits(t, "PIDIV match row");
  }
  for (uint8_t i = 0; i <= PK_PIDIV_EX_TRADE; i++) {
    snprintf(t, sizeof t, "PID/IV not testable (%s)", pk_pidiv_exempt_name(i));
    want_fits(t, "PIDIV exemption row");
  }
}

int main(int argc, char** argv) {
  printf("== legality V2 hooks ==\n");
  printf("  learnsets2: %s (exact RS=0x%02X E=0x%02X FRLG=0x%02X)   moves hook %s\n",
         lg2_have_data() ? "generated" : "ABSENT", lg2_exact_sources(PK_RS),
         lg2_exact_sources(PK_EMERALD), lg2_exact_sources(PK_FRLG),
         pk2_moves_data_ok() ? "armed" : "DISARMED");
  printf("  encounters: %s (S %d / R %d / E %d / FR %d / LG %d rows)   encounter hook %s\n",
         pk_wild_have_data() ? "generated" : "ABSENT",
         pk_wild_count(PK_ENC_SAPPHIRE), pk_wild_count(PK_ENC_RUBY), pk_wild_count(PK_ENC_EMERALD),
         pk_wild_count(PK_ENC_FIRERED), pk_wild_count(PK_ENC_LEAFGREEN),
         pk2_encounter_data_ok() ? "armed" : "DISARMED");
  if (!pk2_moves_data_ok() || !pk2_encounter_data_ok()) {
    printf("\n  !! the generated tables are missing — run tools/gen_learnsets2.py and\n"
           "     tools/gen_encounters.py --from-rom. Refusing to pretend this test ran.\n");
    return 1;
  }

  printf("\n-- (A) pre-evolution table --\n");
  preevo_table();

  printf("\n-- (B) every hook message fits a %d px row --\n", ROW_BUDGET);
  text_fits();

  printf("\n-- (C) golden gate: the hooked catalogue over the real saves --\n");
  if (argc > 1) for (int i = 1; i < argc; i++) run_save(argv[i]);
  else run_save("tests/fixtures/POKEMON_EMER_BPEE00.sav");

  printf("  %d mons from %d save(s); %d excluded (garbage slots + editor-built mons)\n",
         g_tested, g_saves, g_excluded);
  printf("  coverage: %d evolved, %d matched a wild row, %d bred, %d reached the PIDIV hook\n",
         g_cov_evolved, g_cov_wildmatch, g_cov_bred, g_cov_pidiv_ran);
  CHECK(g_tested > 0, "the corpus produced mons to check");
  CHECK(g_cov_evolved > 0, "the corpus exercises the pre-evolution chain");
  CHECK(g_cov_wildmatch > 0, "the corpus exercises the wild-encounter match");
  CHECK(g_cov_pidiv_ran > 0, "the corpus reaches the PIDIV hook");
  CHECK(g_invalid == 0, "NO hook calls a real Pokemon INVALID");

  printf("\n-- P5 calibration: what the hooks say about %d real Pokemon --\n", g_tested);
  printf("   %-38s %8s %8s %8s\n", "check", "INVALID", "suspect", "info");
  for (int c = 0; c < CK_OTHER; c++)
    printf("   %-38s %8d %8d %8d\n", ck_name[c],
           g_ck[c].n[PK2_INVALID], g_ck[c].n[PK2_SUSPECT], g_ck[c].n[PK2_INFO]);
  printf("   %-38s %8d %8d %8d\n", "(rows from the core, not the hooks)",
         g_ck[CK_OTHER].n[PK2_INVALID], g_ck[CK_OTHER].n[PK2_SUSPECT], g_ck[CK_OTHER].n[PK2_INFO]);
  printf("\n   examples (up to 3 per check):\n");
  for (int c = 0; c < CK_OTHER; c++)
    for (int i = 0; i < g_ck[c].nex; i++)
      printf("     %-14.14s %s\n", ck_name[c], g_ck[c].ex[i]);

  printf("\n-- (D) negative tests --\n");
  negatives();
  printf("\n-- (E) exemptions and the tri-state --\n");
  exemptions();
  printf("\n-- (F) the checker judges PokeDNA's own creations --\n");
  built_met_location();

  printf("\n%s: %d checks, %d failure(s)\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
