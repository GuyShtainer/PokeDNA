/* Host test for the Legality V2 check catalogue (gen3_legality2.c).
 *   cc -std=c11 -I source tests/host_legality2_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_legality2.c source/gen3_edit.c source/gen3_daycare.c source/data_tables.c \
 *      -o /tmp/hl2
 *   /tmp/hl2 /path/to/Emerald.sav /path/to/Ruby.sav ...
 *
 * Every argument is a save to check; with none it falls back to one fixture.
 * (tests/run_host_tests.py hands a test the whole 5-save corpus only if the source
 * mentions argv[1] — hence the literal reference here and in main().)
 *
 * Two halves:
 *   (A) the GOLDEN GATE — every LEGITIMATE mon in every save handed to us must come
 *       out with ZERO INVALID verdicts. Those saves are Guy's own cartridges: an
 *       INVALID on a real Pokemon is by definition a false positive and a bug in the
 *       checker, and this gate is what authorises (or forbids) promoting a SUSPECT
 *       check to INVALID. Two populations are excluded from "legitimate" and asserted
 *       to be CAUGHT instead — see the comment above the counters. SUSPECT/INFO counts
 *       are printed per message: that is the calibration data.
 *   (B) NEGATIVE tests — records deliberately broken one field at a time with
 *       gen3_edit.c, each asserting that its OWN check fires (and, where it matters,
 *       that the clean control does not).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "gen3_legality2.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

/* ---- SUSPECT/INFO tally across the whole corpus (calibration) ------------- */
#define MAX_TALLY 48
static struct { char text[PK2_TEXT_LEN]; int sev, n; } g_tally[MAX_TALLY];
static int g_ntally = 0;

static void tally(const Pk2Row* r) {
  for (int i = 0; i < g_ntally; i++)
    if (!strcmp(g_tally[i].text, r->text)) { g_tally[i].n++; return; }
  if (g_ntally >= MAX_TALLY) return;
  strcpy(g_tally[g_ntally].text, r->text);
  g_tally[g_ntally].sev = r->sev;
  g_tally[g_ntally].n = 1;
  g_ntally++;
}

static int row_sev(const Pk2Report* R, const char* text) {
  for (int i = 0; i < R->n; i++)
    if (!strcmp(R->row[i].text, text)) return R->row[i].sev;
  return -1;
}

/* ---- (A) the golden gate -------------------------------------------------- */

/* The corpus is NOT uniformly legitimate, which the gate has to account for or it
 * can never pass. Two populations are excluded from "must be clean" and instead
 * asserted to be CAUGHT:
 *
 *   - garbage slots: species out of range / unused internal slot / failed record
 *     checksum. The Emerald and FireRed saves each hold a couple (V1's test knows
 *     about these too).
 *   - hand-edited mons: the LeafGreen save's party is a save-editor test bench —
 *     four mons with EV totals of 1512..1530 (max EVs in all six stats), all-31
 *     IVs, OT "poop", a level-10 Deoxys with a Sapphire origin byte. An EV total
 *     above 510 is impossible on retail and V1 already calls it illegal, so it is
 *     a sound, non-circular way to recognise them. You cannot calibrate a
 *     false-positive gate against a mon somebody built with an editor.
 *
 * Everything else — 647 of the 651 mons — must produce ZERO INVALID rows. */
static int g_saves_read;
static int g_gate_tested, g_gate_invalid;
static int g_cov_eggs, g_cov_party, g_cov_l100, g_cov_jp, g_cov_2abil;
static int g_garbage_total, g_garbage_caught;
static int g_edited_total, g_edited_caught;

static void visit(PkMon* m) {
  pk_resolve(m);
  Pk2Report R;
  pk_check_legality2(m, &R);

  int evsum = 0;
  for (int i = 0; i < PK_NSTATS; i++) evsum += m->evs[i];
  int garbage = m->isBadEgg || m->species < 1 || m->species > 411 ||
                pk_national_no(m->species) == 0;
  if (garbage) {
    g_garbage_total++;
    if (R.grade == PK2_ILLEGAL) g_garbage_caught++;
    return;
  }
  if (evsum > 510) {
    g_edited_total++;
    if (R.grade == PK2_ILLEGAL) g_edited_caught++;
    printf("    edited fixture: %-11s L%-3d EV total %d ->", pk_species_name(m->species), m->level, evsum);
    for (int i = 0; i < R.n; i++) printf(" {%s: %s}", pk2_sev_name(R.row[i].sev), R.row[i].text);
    printf("\n");
    return;
  }

  g_gate_tested++;
  /* Coverage: a gate that passes because it never reached the interesting branches
   * proves nothing. These counters show the real saves actually exercise the egg,
   * party-level/EXP and EXP-cap paths. */
  if (m->isEgg)   g_cov_eggs++;
  if (m->isParty) g_cov_party++;
  if (m->level == 100) g_cov_l100++;
  if (m->language == 1) g_cov_jp++;
  if (pk_species_ability(m->species, 1) != 0) g_cov_2abil++;

  for (int i = 0; i < R.n; i++) {
    if (R.row[i].sev == PK2_INVALID) {
      g_gate_invalid++;
      printf("    INVALID on a real mon: %-11s L%-3d met %d/%d game %d -> %s\n",
             pk_species_name(m->species), m->level, m->metLocation, m->metLevel,
             m->metGame, R.row[i].text);
    } else {
      tally(&R.row[i]);
    }
  }
}

static void run_save(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  (skip, cannot open %s)\n", path); return; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("  parse FAILED: %s\n", path); g_fail++; return; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES], pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);

  g_saves_read++;
  const char* base = strrchr(path, '/');
  printf("  %s\n", base ? base + 1 : path);
  int before_valid = g_gate_tested, before_bad = g_gate_invalid;
  PkMon party[6]; bool frlg = false;
  int np = pk_read_party_auto(sb1, party, &frlg);
  for (int i = 0; i < np; i++) visit(&party[i]);
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    PkMon box[30];
    pk_read_box(pc, b, box);
    for (int s = 0; s < 30; s++) if (box[s].species != 0) visit(&box[s]);
  }
  printf("    %3d mons in the gate, %d INVALID\n", g_gate_tested - before_valid,
         g_gate_invalid - before_bad);
}

/* ---- (B) synthetic fixtures ----------------------------------------------- */

/* One fixture = an EditMon plus the 100-byte record its PkMon points into. The
 * record must outlive the PkMon (PkMon.raw is a back-reference into it), so the
 * caller owns both. */
typedef struct { EditMon e; uint8_t rec[100]; PkMon m; } Fix;

static void fix_new(Fix* x, uint16_t species, uint8_t lvl) {
  uint8_t base[80];
  gen3_build_mon(species, lvl, 0x1234ABCDu, 0x00010002u, "GUY", 3, base);
  gen3_edit_load(base, false, &x->e);
}
static Pk2Report fix_check(Fix* x) {
  memset(x->rec, 0, sizeof x->rec);
  gen3_edit_commit(&x->e, x->rec);
  Pk2Report R;
  if (!pk_decode_mon(x->rec, x->e.is_party, &x->m)) { printf("  !! fixture decoded as empty\n"); g_fail++; }
  pk_resolve(&x->m);
  pk_check_legality2(&x->m, &R);
  return R;
}
/* Commit first, then poke a PLAINTEXT byte (0x00..0x1F is outside the encrypted
 * 48-byte block and outside the record checksum), then decode. */
static Pk2Report fix_check_poke(Fix* x, int off, uint8_t val) {
  memset(x->rec, 0, sizeof x->rec);
  gen3_edit_commit(&x->e, x->rec);
  x->rec[off] = val;
  Pk2Report R;
  pk_decode_mon(x->rec, x->e.is_party, &x->m);
  pk_resolve(&x->m);
  pk_check_legality2(&x->m, &R);
  return R;
}

/* An assignment is an rvalue in C, so these two wrap "re-run and point at it". */
#define CHK        (R = fix_check(&x), &R)
#define CHKP(o, v) (R = fix_check_poke(&x, (o), (v)), &R)

/* assert that `text` is present at severity `sev` */
static void want(const Pk2Report* R, const char* text, int sev, const char* what) {
  int got = row_sev(R, text);
  if (got != sev) {
    printf("  !! FAIL: %s — expected \"%s\" at %s, got %s\n", what, text,
           pk2_sev_name((uint8_t)sev), got < 0 ? "nothing" : pk2_sev_name((uint8_t)got));
    for (int i = 0; i < R->n; i++)
      printf("       row: [%s] %-8s %s\n", pk2_cat_name(R->row[i].cat),
             pk2_sev_name(R->row[i].sev), R->row[i].text);
    g_fail++;
  }
}
static void want_absent(const Pk2Report* R, const char* text, const char* what) {
  if (row_sev(R, text) >= 0) { printf("  !! FAIL: %s — \"%s\" fired on a clean mon\n", what, text); g_fail++; }
}

static void negatives(void) {
  Fix x;

  /* B-party — the three PARTY-ONLY structural checks. These grade a Pokemon ILLEGAL,
   * and review found all three had NO test at all: deleting them left the suite green.
   * A party record stores its level in PLAINTEXT at 0x54, so it can disagree with EXP;
   * a box record derives the level and cannot. gen3_build_mon makes a box record, so
   * these fixtures flip is_party and poke 0x54 directly (still plaintext, still
   * outside the encrypted block and the record checksum). */
  {
    Fix p; Pk2Report PR;
    fix_new(&p, 1, 50); p.e.is_party = true;

    PR = fix_check_poke(&p, 0x54, 0);            /* level 0 */
    want(&PR, "Level out of 1..100", PK2_INVALID, "party level 0");
    PR = fix_check_poke(&p, 0x54, 101);          /* level 101 */
    want(&PR, "Level out of 1..100", PK2_INVALID, "party level 101");

    PR = fix_check_poke(&p, 0x54, 49);           /* in range, but EXP says 50 */
    want(&PR, "Level does not match EXP", PK2_INVALID, "party level disagrees with EXP");

    PR = fix_check_poke(&p, 0x54, 50);           /* the control: level 50 EXP 50 */
    want_absent(&PR, "Level out of 1..100", "level-range check on a correct party mon");
    want_absent(&PR, "Level does not match EXP", "level/EXP check on a correct party mon");

    /* EXP above the level-100 cap — reachable on a BOX record too, so no party flag.
     * There is no em_set_exp, and EXP lives INSIDE the encrypted Growth substruct, so
     * poking the committed bytes would break the checksum. Write it through the
     * EditMon's own substruct before committing, which is what em_set_level does. */
    Fix c; fix_new(&c, 1, 100);
    c.e.sub[0][4] = 0xFF; c.e.sub[0][5] = 0xFF;
    c.e.sub[0][6] = 0xFF; c.e.sub[0][7] = 0x00;   /* 0x00FFFFFF, far past any cap */
    Pk2Report CR = fix_check(&c);
    want(&CR, "EXP above the level-100 cap", PK2_INVALID, "EXP past the cap");
  }

  /* B-stat — the party stat-formula cross-check gen3_mon.h:72-75 documents but nothing
   * exercised before this batch. gen3_edit.c's own mutators (em_set_level etc.) always
   * recompute the plaintext stats correctly, so the only way to produce a mismatch is
   * to poke a stat byte directly after commit — same idiom as the level tests above. */
  {
    Fix sp; Pk2Report SR;
    fix_new(&sp, 1, 50); em_set_party_flag(&sp.e, true);
    SR = fix_check(&sp);
    want_absent(&SR, "Max HP does not match the stat formula", "correctly-committed party stats");
    want_absent(&SR, "A stat does not match the formula", "correctly-committed party stats");

    SR = fix_check_poke(&sp, 0x58, 1);   /* Max HP low byte -> implausible for L50 */
    want(&SR, "Max HP does not match the stat formula", PK2_INVALID, "hex-edited Max HP");

    SR = fix_check_poke(&sp, 0x5A, 1);   /* Attack low byte */
    want(&SR, "A stat does not match the formula", PK2_INVALID, "hex-edited Attack stat");

    /* A BOX record's stats are something WE compute in pk_resolve — comparing them
     * to the same formula would be tautological, so the check must not run there. */
    Fix bx; fix_new(&bx, 1, 50);
    Pk2Report BR = fix_check(&bx);
    want_absent(&BR, "Max HP does not match the stat formula", "box records are not checked");
  }

  /* B0 — the clean control. gen3_build_mon must produce a mon with NOTHING to say. */
  fix_new(&x, 1, 5);
  Pk2Report R = fix_check(&x);
  if (R.n != 0) {
    printf("  !! FAIL: clean built mon raised %d row(s):\n", R.n);
    for (int i = 0; i < R.n; i++)
      printf("       [%s] %-8s %s\n", pk2_cat_name(R.row[i].cat), pk2_sev_name(R.row[i].sev), R.row[i].text);
    g_fail++;
  }
  CHECK(R.grade == PK2_LEGAL, "clean built mon grades LEGAL");
  /* This translation unit links NONE of the hook modules, so every family that runs by
   * default must report itself absent. PK2_HOOK_EVO joined the list when the evolution
   * rule landed (source/evolutions.h). */
  CHECK(R.hooks_absent == (PK2_HOOK_MOVES | PK2_HOOK_ENCOUNTER | PK2_HOOK_EVO),
        "absent moves/encounter/evolution hooks are reported (PIDIV is not run by default)");
  pk_check_legality2_ex(&x.m, &R, PK2_RUN_PIDIV);
  CHECK(R.hooks_absent == (PK2_HOOK_MOVES | PK2_HOOK_ENCOUNTER | PK2_HOOK_EVO | PK2_HOOK_PIDIV),
        "PK2_RUN_PIDIV reaches the (absent) PIDIV hook");
  CHECK(R.pidiv_ran == 0, "an absent PIDIV hook leaves pidiv_ran clear");

  /* B1 — structure */
  fix_new(&x, 1, 5);
  for (int i = 0; i < 6; i++) em_set_ev(&x.e, i, 255);
  want(CHK, "EV total over 510", PK2_INVALID, "EV cap");

  fix_new(&x, 1, 5);
  R = fix_check(&x); x.m.ivs[PK_HP] = 35;        /* only reachable on a hand-built PkMon */
  pk_check_legality2(&x.m, &R);
  want(&R, "IV over 31", PK2_INVALID, "IV bound");

  fix_new(&x, 1, 50);
  R = fix_check(&x); x.m.experience = 0xF0000000u;
  pk_check_legality2(&x.m, &R);
  want(&R, "EXP above the level-100 cap", PK2_INVALID, "EXP cap");

  fix_new(&x, 1, 5); em_set_item(&x.e, 400);
  want(CHK, "Held item id out of range", PK2_INVALID, "item id bound");

  {
    uint16_t ghost = 0;
    for (uint16_t it = 1; it <= 376 && !ghost; it++) if (pk_item_games(it) == 0) ghost = it;
    if (ghost) {
      fix_new(&x, 1, 5); em_set_item(&x.e, ghost);
      want(CHK, "Held item exists in no Gen-3 game", PK2_SUSPECT, "placeholder item");
    } else printf("  (no placeholder item id in the table — check not exercised)\n");
  }

  /* Ability slot: Bulbasaur (internal 1) has abilities[1] == ABILITY_NONE, Rattata
   * (internal 19) has two, so the same bit is illegal on one and fine on the other. */
  CHECK(pk_species_ability(1, 1) == 0 && pk_species_ability(19, 1) != 0,
        "fixture species have the ability counts this test assumes");
  fix_new(&x, 1, 5); em_set_ability(&x.e, 1);
  want(CHK, "2nd ability but species has one", PK2_INVALID, "ability slot");
  fix_new(&x, 19, 5); em_set_ability(&x.e, 1);
  want_absent(CHK, "2nd ability but species has one", "2-ability species");

  /* Ability slot vs PID on a two-ability species. The fixture PID is 0x1234ABCD, so
   * PID&1 == 1 and slot 0 is the mismatch; slot 1 matches; an in-game-trade met
   * location is exempt (the trade template writes the slot by hand — src/trade.c:4570).
   *
   * The mismatch has to be MADE now: gen3_build_mon derives the slot from the PID the
   * way CreateBoxMon does (src/pokemon.c:2296-2300), so its output no longer supplies
   * one for free. Clearing it by hand is also the honest fixture — the check is about a
   * record whose slot contradicts its PID, not about what the builder happens to leave. */
  fix_new(&x, 19, 5); em_set_ability(&x.e, 0);
  want(CHK, "Ability slot does not match the PID", PK2_SUSPECT, "ability vs PID");
  fix_new(&x, 19, 5); em_set_ability(&x.e, 1);
  want_absent(CHK, "Ability slot does not match the PID", "slot matches PID");
  /* Both exemptions keep the mismatch in place — otherwise they would be asserting the
   * absence of a row that had no reason to appear, and would pass with the check gone. */
  fix_new(&x, 19, 5); em_set_ability(&x.e, 0); em_set_metloc(&x.e, 0xFE);
  want_absent(CHK, "Ability slot does not match the PID", "in-game-trade exemption");
  fix_new(&x, 19, 5); em_set_ability(&x.e, 0); em_set_metgame(&x.e, 15);
  want_absent(CHK, "Ability slot does not match the PID", "Colosseum/XD exemption");

  fix_new(&x, 260, 5);   /* internal 252..276 = the unused slots */
  want(CHK, "Unused species slot", PK2_INVALID, "unused species");
  fix_new(&x, 1, 5); em_set_species(&x.e, 500);
  want(CHK, "Species id out of range", PK2_INVALID, "species bound");
  fix_new(&x, 1, 5);
  want(CHKP(0x13, 0x01), "Bad egg (checksum failed)", PK2_INVALID, "bad-egg flag");

  /* B2 — moves */
  fix_new(&x, 1, 5); em_set_move(&x.e, 0, 500);
  want(CHK, "Move id out of range", PK2_INVALID, "move id bound");
  fix_new(&x, 1, 5); em_set_move(&x.e, 1, 33);          /* slot 0 is already Tackle */
  want(CHK, "Duplicate move", PK2_INVALID, "duplicate move");
  fix_new(&x, 1, 5); em_set_pp(&x.e, 0, 200);
  want(CHK, "PP above maximum", PK2_INVALID, "PP bound");
  fix_new(&x, 1, 5); em_set_move(&x.e, 0, 0);
  want(CHK, "No moves", PK2_INVALID, "empty moveset");

  /* B3 — met */
  fix_new(&x, 1, 5); em_set_metlevel(&x.e, 50);
  want(CHK, "Met level above current level", PK2_INVALID, "met level vs level");
  fix_new(&x, 1, 100); em_set_metlevel(&x.e, 120);
  want(CHK, "Met level above 100", PK2_INVALID, "met level bound");
  fix_new(&x, 1, 5); em_set_metgame(&x.e, 0);
  want(CHK, "Unusual origin game", PK2_SUSPECT, "origin game");
  fix_new(&x, 1, 5); em_set_metloc(&x.e, 0xE0);
  want(CHK, "Invalid met location", PK2_SUSPECT, "met-location dead zone");
  fix_new(&x, 1, 5); em_set_metloc(&x.e, 0xFF);
  want_absent(CHK, "Invalid met location", "fateful met location");
  fix_new(&x, 1, 5); em_set_ball(&x.e, 0);
  want(CHK, "Unusual Poke Ball id", PK2_SUSPECT, "ball id");

  /* B3b — E4, Safari Ball <-> Safari Zone (doc §3 E4). 0x39 is RSE's SAFARI ZONE
   * MAPSEC, 0x10 an ordinary Hoenn route (data_tables.c s_location). */
  fix_new(&x, 1, 5); em_set_ball(&x.e, 5); em_set_metloc(&x.e, 0x10);
  want(CHK, "Safari Ball outside a Safari Zone", PK2_INVALID, "Safari Ball met elsewhere");
  /* A CAUGHT mon (met level > 0) in a Poke Ball with a Safari met place is suspect... */
  fix_new(&x, 1, 5); em_set_ball(&x.e, 4); em_set_metloc(&x.e, 0x39); em_set_metlevel(&x.e, 5);
  want(CHK, "Safari Zone catch, wrong Ball", PK2_SUSPECT, "Safari Zone met without a Safari Ball");
  /* ...but a HATCHED one (met level 0) is exactly Ruby.sav's Skitty: Poke Ball, met where the
   * player stood (egg_hatch.c:388-389) -- legitimate, never flagged (review 2026-09-05). */
  fix_new(&x, 1, 5); em_set_ball(&x.e, 4); em_set_metloc(&x.e, 0x39); em_set_metlevel(&x.e, 0);
  want_absent(CHK, "Safari Zone catch, wrong Ball", "hatched inside the Safari Zone");
  fix_new(&x, 1, 5); em_set_ball(&x.e, 5); em_set_metloc(&x.e, 0x39);
  want_absent(CHK, "Safari Ball outside a Safari Zone", "matched Safari Ball and Zone");
  want_absent(CHK, "Safari Zone catch, wrong Ball", "matched Safari Ball and Zone");

  /* B4 — eggs. Build a CLEAN egg first (level 5, metLevel 0, Poke Ball, hatch
   * counter <= 120, Japanese nickname タマゴ = 60 6F 8B FF with language 1) and
   * require silence; then break one field at a time. */
  #define CLEAN_EGG(lvl) do {                                  \
      fix_new(&x, 1, (lvl));                                   \
      em_set_egg(&x.e, true);                                  \
      em_set_metlevel(&x.e, 0);                                \
      em_set_friendship(&x.e, 20);                             \
      x.e.raw[0x12] = 1;                                       \
      x.e.raw[0x08] = 0x60; x.e.raw[0x09] = 0x6F;              \
      x.e.raw[0x0A] = 0x8B; x.e.raw[0x0B] = 0xFF;              \
    } while (0)

  CLEAN_EGG(5);
  R = fix_check(&x);
  if (R.n != 0) {
    printf("  !! FAIL: clean egg raised %d row(s):\n", R.n);
    for (int i = 0; i < R.n; i++)
      printf("       [%s] %-8s %s\n", pk2_cat_name(R.row[i].cat), pk2_sev_name(R.row[i].sev), R.row[i].text);
    g_fail++;
  }
  CLEAN_EGG(5); em_set_ev(&x.e, PK_HP, 4);
  want(CHK, "Egg has EVs", PK2_INVALID, "egg EVs");
  CLEAN_EGG(5); em_set_contest(&x.e, 0, 10);
  want(CHK, "Egg has contest stats", PK2_INVALID, "egg contest stats");
  CLEAN_EGG(20);
  want(CHK, "Egg level is not 5", PK2_INVALID, "egg level");
  CLEAN_EGG(5); em_set_metlevel(&x.e, 5);
  want(CHK, "Egg met level is not 0", PK2_INVALID, "egg met level");
  CLEAN_EGG(5); em_set_ball(&x.e, 3);
  want(CHK, "Egg is not in a Poke Ball", PK2_SUSPECT, "egg ball");
  CLEAN_EGG(5); em_set_friendship(&x.e, 200);
  want(CHK, "Egg hatch counter over 120", PK2_INVALID, "egg hatch counter");
  CLEAN_EGG(5); x.e.sub[3][8] = 0x01;                    /* Misc bytes 8..11 = ribbons */
  want(CHK, "Egg has ribbons", PK2_SUSPECT, "egg ribbons");
  CLEAN_EGG(5); x.e.sub[3][11] = 0x80;                   /* bit 31 = fateful, legal on the
                                                          * Mystery Gift Surf Pichu egg */
  want_absent(CHK, "Egg has ribbons", "fateful bit on an egg");
  CLEAN_EGG(5); x.e.raw[0x12] = 2;
  want(CHK, "Egg language is not Japanese", PK2_SUSPECT, "egg language");
  CLEAN_EGG(5); x.e.raw[0x08] = 0xBB;                    /* 'A' — no longer タマゴ */
  want(CHK, "Egg name is not the JP egg name", PK2_SUSPECT, "egg nickname");

  /* a hatched mon that kept the egg nickname */
  fix_new(&x, 1, 5);
  x.e.raw[0x08] = 0x60; x.e.raw[0x09] = 0x6F; x.e.raw[0x0A] = 0x8B; x.e.raw[0x0B] = 0xFF;
  want(CHK, "Not an egg but has the egg name", PK2_SUSPECT, "egg name on a non-egg");

  /* B5 — flags */
  fix_new(&x, 1, 5);
  want(CHKP(0x12, 0), "Language byte invalid", PK2_INVALID, "language 0");
  fix_new(&x, 1, 5);
  want(CHKP(0x12, 9), "Language byte invalid", PK2_INVALID, "language 9");
  fix_new(&x, 1, 5);
  want(CHKP(0x12, 6), "Language 6 (Korean) unused in Gen 3", PK2_SUSPECT, "Korean");
  fix_new(&x, 1, 5);
  want(CHKP(0x12, 1), "Name too long for a JP-language mon", PK2_SUSPECT, "JP name length");
  fix_new(&x, 1, 5);
  want(CHKP(0x08, 0xFF), "Nickname is empty", PK2_SUSPECT, "empty nickname");
  fix_new(&x, 1, 5);
  want(CHKP(0x14, 0xFF), "OT name is empty", PK2_SUSPECT, "empty OT name");
  fix_new(&x, 1, 5);
  want(CHKP(0x08, 0xFA), "Control byte in nickname", PK2_SUSPECT, "control byte");
  fix_new(&x, 1, 5);
  want(CHKP(0x14, 0xFC), "Control byte in OT name", PK2_SUSPECT, "OT control byte");

  fix_new(&x, 151, 30);                                   /* Mew */
  want(CHK, "Mew/Deoxys lacks the event flag", PK2_SUSPECT, "Mew without the flag");
  fix_new(&x, 151, 30); x.e.sub[3][11] = 0x80;
  want_absent(CHK, "Mew/Deoxys lacks the event flag", "Mew with the flag");
  fix_new(&x, 1, 5); x.e.sub[3][11] = 0x80;
  want(CHK, "Event/fateful flag is set", PK2_INFO, "fateful on a normal species");
  CHECK((R = fix_check(&x)).grade == PK2_LEGAL, "an INFO row still grades LEGAL");

  fix_new(&x, 1, 5); x.e.sub[3][0] = 0x05;                /* strain 0, 5 days left */
  want(CHK, "Pokerus days but no strain", PK2_INVALID, "pokerus w/o strain");
  fix_new(&x, 1, 5); x.e.sub[3][0] = 0x14;                /* strain 1 -> max 2 days, has 4 */
  want(CHK, "Pokerus days above the strain max", PK2_INVALID, "pokerus day cap");
  fix_new(&x, 1, 5); x.e.sub[3][0] = 0x81;                /* strain 8 is never rolled */
  want(CHK, "Pokerus strain 8 cannot be rolled", PK2_SUSPECT, "pokerus strain 8");
  fix_new(&x, 1, 5); x.e.sub[3][0] = 0x23;                /* strain 2, 3 days = the max */
  want_absent(CHK, "Pokerus days above the strain max", "legal pokerus");
  fix_new(&x, 1, 5); x.e.sub[3][0] = 0x20;                /* cured: strain kept, 0 days */
  want_absent(CHK, "Pokerus days but no strain", "cured pokerus");

  /* B6 — the report itself */
  fix_new(&x, 1, 5);
  for (int i = 0; i < 6; i++) em_set_ev(&x.e, i, 255);
  em_set_metlevel(&x.e, 50);
  R = fix_check(&x);
  CHECK(R.grade == PK2_ILLEGAL, "any INVALID grades the mon ILLEGAL");
  CHECK(R.n_invalid == 2, "both INVALID rows counted");
  CHECK(R.cat_worst[PK2_CAT_STRUCT] == PK2_INVALID && R.cat_worst[PK2_CAT_MET] == PK2_INVALID,
        "per-category worst severity is tracked");
  CHECK(R.cat_n[PK2_CAT_EGG] == 0, "a clean category reports zero rows");
  for (int i = 0; i < R.n; i++)
    CHECK(strlen(R.row[i].text) < PK2_TEXT_LEN, "every row fits the fixed buffer");
}

/* ---- main ----------------------------------------------------------------- */

int main(int argc, char** argv) {
  printf("== legality V2: golden gate over the real saves ==\n");
  if (argc > 1) { run_save(argv[1]); for (int i = 2; i < argc; i++) run_save(argv[i]); }
  else run_save("tests/fixtures/POKEMON_EMER_BPEE00.sav");

  printf("(A) %d legitimate mons checked, %d INVALID (want 0)\n", g_gate_tested, g_gate_invalid);
  printf("    excluded: %d garbage slot(s) (%d caught), %d hand-edited mon(s) (%d caught)\n",
         g_garbage_total, g_garbage_caught, g_edited_total, g_edited_caught);
  printf("    coverage: %d egg(s), %d party mon(s), %d at level 100, %d JP-language, "
         "%d two-ability species\n", g_cov_eggs, g_cov_party, g_cov_l100, g_cov_jp, g_cov_2abil);
  CHECK(g_gate_tested > 0, "the corpus produced at least one mon to check");
  /* One lone fixture may legitimately hold no egg, so only demand egg coverage
   * when a real corpus was supplied. */
  if (g_saves_read > 1) CHECK(g_cov_eggs > 0, "the corpus exercises the egg-coherence checks");
  CHECK(g_cov_party > 0, "the corpus exercises the party level-vs-EXP check");
  CHECK(g_gate_invalid == 0, "NO false INVALID on any real Pokemon");
  CHECK(g_garbage_caught == g_garbage_total, "every garbage slot is graded ILLEGAL");
  CHECK(g_edited_caught == g_edited_total, "every hand-edited mon is graded ILLEGAL");

  printf("\n-- SUSPECT/INFO calibration on real data (per check) --\n");
  if (!g_ntally) printf("   (none)\n");
  for (int i = 0; i < g_ntally; i++)
    printf("   %-8s %4d  %s\n", pk2_sev_name((uint8_t)g_tally[i].sev), g_tally[i].n, g_tally[i].text);

  /* Every row must fit the 240 px screen; the checker truncates, so a message that
   * needed truncating is an authoring bug, not a runtime one. */
  for (int i = 0; i < g_ntally; i++)
    CHECK(strlen(g_tally[i].text) < PK2_TEXT_LEN - 1, "message fits a report row");

  printf("\n== negative tests (deliberately broken records) ==\n");
  negatives();

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
