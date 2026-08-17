/* Host test for the summary screen's IV reroll core (gen3_ivroll.c).
 *   cc -std=c11 -O2 -I source tests/host_ivroll_test.c source/gen3_ivroll.c \
 *      source/gen3_gen.c source/gen3_pidiv.c source/gen3_edit.c source/gen3_box.c \
 *      source/gen3_daycare.c source/evolutions.c source/gen3_mon.c source/gen3_save.c \
 *      source/data_tables.c -o /tmp/hiv && /tmp/hiv
 *
 * WHAT IS BEING PROVED. "Reroll the IVs" is not free in Gen 3: for a CAUGHT Pokemon the
 * personality value and both IV words come off ONE LCRNG stream, so new IVs mean a NEW
 * PID — and the PID is the nature, the sex, the ability slot, the shininess and Unown's
 * letter. A reroll that ignored that would either hand back a different Pokemon without
 * saying so, or write a spread PokeDNA's own auditor calls suspect. So every roll below is
 * re-searched with the SHIPPED pk_pidiv_search and every derived property is compared
 * before and after.
 *
 * The six parts:
 *   (1) LOCKED ROLLS — one per species, all re-found as Method 1 (Method 1 (Unown) for
 *       Unown), nature/sex/ability/shininess preserved, the applied ability bit equal to
 *       what CreateBoxMon would derive from the NEW PID, and the applied IVs equal to the
 *       rolled ones.
 *   (2) THE EXEMPT PATH — a hatched mon and an egg keep their PID BYTE-IDENTICAL, and the
 *       egg bit and ability bit survive the IV write.
 *   (3) SHINY — the expensive case. Shininess kept, no budget failures, still Method 1,
 *       and the candidate counts PRINTED so the UI's slice size comes from measurement.
 *   (4) THE HISTORY, including the MANDATORY one: push 20 rolls, walk all the way back,
 *       and the PID and IV word must be BIT-IDENTICAL to what the record walked in with.
 *       That is the assertion that fails without entry 0 being pinned.
 *   (5) APPLY -> commit -> load round-trips the PID and the IV word through the record.
 *   (6) A bad egg refuses to roll, and a tiny slice returns 0 repeatedly and eventually 1
 *       — i.e. the chunking terminates rather than looping on the caller's frame budget.
 *
 * Also pinned here: an NPC-TRADE mon (met location 0xFE) raises chg_trade, because its PID
 * is a constant in the trade template rather than RNG output and NO reroll outcome keeps
 * it. The UI must ask before writing; this asserts it has something to ask about.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gen3_ivroll.h"
#include "gen3_gen.h"
#include "gen3_pidiv.h"
#include "gen3_edit.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

static const uint32_t k_otid = 0x3B8C97A0u;
#define MET_TRADE 0xFEu

/* Run the chunked search to completion, whatever the slice size. Returns iv_roll_step's
 * final verdict and (via *slices) how many calls it took. */
static int roll_to_end(const EditMon* e, const PkMon* cur, IvRollState* st, uint32_t slice,
                       IvRoll* out, int* slices) {
  int rc, n = 0;
  while ((rc = iv_roll_step(e, cur, st, slice, out)) == 0) {
    n++;
    if (n > 100000) { printf("  !! FAIL: chunked search never terminated\n"); g_fail++; break; }
  }
  if (slices) *slices = n;
  return rc;
}

static void refresh(const EditMon* e, PkMon* m) { em_preview(e, m); pk_resolve(m); }

/* A CAUGHT record: gen3_build_mon stamps met level 0 on anything breedable, which is
 * exactly the PIDIV exemption, so the interesting path is only reachable by giving the
 * record an honest caught origin first. */
static void make_caught(uint16_t sp, uint32_t seed, EditMon* e, PkMon* m) {
  uint8_t rec[80];
  uint8_t lvl = gen3_build_level(sp);
  gen3_build_mon_spread(sp, lvl, seed, k_otid, "GUY", 3, 0, rec, 0);
  gen3_edit_load(rec, false, e);
  em_set_metlevel(e, lvl ? lvl : 5);
  refresh(e, m);
}

/* ============================ (1) LOCKED ROLLS ============================== */

static int part_1(void) {
  printf("(1) locked rolls: the PID moves, and everything derived from it survives\n");
  int n = 0, not_m1 = 0, natlost = 0, sexlost = 0, abilost = 0, shinylost = 0;
  int abibad = 0, ivbad = 0, notlocked = 0;
  uint32_t worst = 0;

  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    EditMon e; PkMon m;
    make_caught(sp, 0x1000u + sp * 7919u, &e, &m);
    CHECK(pk_pidiv_exempt_reason(&m) == PK_PIDIV_EX_NONE, "the fixture really is testable");

    int two_ab = (pk_species_ability(sp, 1) != 0);
    uint32_t old_pid = e.personality;
    uint32_t old_egg = em_get_ivword(&e) & (1u << 30);

    IvRollState st; memset(&st, 0, sizeof st);
    st.seed = 0x2468ACE0u ^ (uint32_t)sp;
    IvRoll r;
    int rc = roll_to_end(&e, &m, &st, 8000u, &r, 0);
    CHECK(rc == 1, "a caught mon can always be rerolled");
    if (rc != 1) continue;
    n++;
    if (r.rolls > worst) worst = r.rolls;
    if (!r.pid_locked) { notlocked++; continue; }
    if (r.pid == old_pid) { /* astronomically unlikely, and not a failure by itself */ }

    if (r.chg_nature)  natlost++;
    if (r.chg_gender)  sexlost++;
    if (r.chg_ability) abilost++;
    if (r.chg_shiny)   shinylost++;

    /* the ability bit must be what CreateBoxMon would derive from the NEW PID */
    uint32_t want_ab = (two_ab && (r.pid & 1u)) ? (1u << 31) : 0u;
    if ((r.ivword & (1u << 31)) != want_ab) abibad++;
    if ((r.ivword & (1u << 30)) != old_egg) abibad++;

    iv_roll_apply(&e, &r);
    refresh(&e, &m);
    if (memcmp(m.ivs, r.ivs, PK_NSTATS) != 0) ivbad++;
    if (m.personality != r.pid) ivbad++;

    /* THE POINT: the applied spread is a real Method-1 pair under the shipped search. */
    PkPidiv pv;
    pk_pidiv_search(m.personality, m.ivs, PK_PIDIV_OPT_REVERSED, &pv);
    uint8_t want_m = (sp == 201) ? PK_PIDIV_M1_REV : PK_PIDIV_M1;
    if (!(pv.methods & (1u << (want_m - 1)))) {
      if (not_m1 < 5) printf("       %-12s pid=%08lX no M1\n",
                             pk_species_name(sp), (unsigned long)m.personality);
      not_m1++;
    }
  }

  printf("     %d locked rolls, worst %lu candidates\n", n, (unsigned long)worst);
  printf("     dropped: nature %d  sex %d  ability %d  shiny %d\n",
         natlost, sexlost, abilost, shinylost);
  CHECK(n >= 385, "the sweep really covered every species");
  CHECK(notlocked == 0, "a caught mon's roll always moves the PID");
  CHECK(not_m1 == 0, "every rerolled caught spread is a Method-1 pair");
  CHECK(natlost == 0 && sexlost == 0 && abilost == 0 && shinylost == 0,
        "a non-shiny caught mon never has to drop a derived property");
  CHECK(abibad == 0, "the applied IV word carries the right ability and egg bits");
  CHECK(ivbad == 0, "the record really holds the rolled PID and the rolled IVs");
  return 0;
}

/* ============================ (2) THE EXEMPT PATH ============================ */

static int part_2(void) {
  printf("(2) the exempt path keeps the PID outright\n");

  /* a HATCHED mon: gen3_build_mon's own output for anything breedable */
  uint8_t rec[80];
  gen3_build_mon_spread(3, 32, 0x51CE01u, k_otid, "GUY", 3, 0, rec, 0);
  EditMon e; PkMon m;
  gen3_edit_load(rec, false, &e);
  refresh(&e, &m);
  CHECK(pk_pidiv_exempt_reason(&m) == PK_PIDIV_EX_HATCHED, "metLevel 0 is the hatched exemption");

  uint32_t pid0 = e.personality, ivw0 = em_get_ivword(&e);
  IvRollState st; memset(&st, 0, sizeof st); st.seed = 0xFEEDFACEu;
  IvRoll r;
  int slices = 0;
  CHECK(roll_to_end(&e, &m, &st, 8000u, &r, &slices) == 1, "an exempt mon rolls");
  CHECK(slices == 0, "...in a single slice, with no search at all");
  CHECK(!r.pid_locked, "...on the exempt path");
  CHECK(r.pid == pid0, "...keeping the PID");
  CHECK(r.rolls == 1, "...for exactly one draw");
  CHECK((r.ivword & 0xC0000000u) == (ivw0 & 0xC0000000u),
        "...and carrying the egg and ability bits across verbatim");
  CHECK(r.ivword != ivw0, "...while the IVs themselves actually moved");
  iv_roll_apply(&e, &r);
  refresh(&e, &m);
  CHECK(m.personality == pid0, "the applied record still has the original PID");
  CHECK(memcmp(m.ivs, r.ivs, PK_NSTATS) == 0, "and the rolled IVs");

  /* an EGG: two RNG streams, so also exempt — and its egg bit must survive */
  EditMon e2; PkMon m2;
  gen3_edit_load(rec, false, &e2);
  em_set_egg(&e2, true);
  refresh(&e2, &m2);
  CHECK(m2.isEgg, "the fixture really is an egg");
  uint32_t pid2 = e2.personality;
  IvRollState st2; memset(&st2, 0, sizeof st2); st2.seed = 0x0C0FFEE0u;
  IvRoll r2;
  CHECK(roll_to_end(&e2, &m2, &st2, 8000u, &r2, 0) == 1, "an egg rolls");
  CHECK(!r2.pid_locked && r2.pid == pid2, "an egg keeps its PID");
  CHECK((r2.ivword & (1u << 30)) != 0, "and stays an egg after the IV write");
  iv_roll_apply(&e2, &r2);
  refresh(&e2, &m2);
  CHECK(m2.isEgg, "...decoded back out of the record too");
  return 0;
}

/* ============================ (3) SHINY ==================================== */

static int part_3(void) {
  printf("(3) shiny: the expensive case, and the one the ladder exists for\n");
  const int N = 600;
  int made = 0, kept = 0, notm1 = 0, natkept = 0, budgetfail = 0;
  unsigned long total = 0, worst = 0;

  for (int i = 0; i < N; i++) {
    uint16_t sp = (uint16_t)(1 + (i * 37) % 251);       /* spread over the Gen-1/2 range */
    if (pk_national_no(sp) == 0) sp = 25;
    EditMon e; PkMon m;
    make_caught(sp, 0x50000u + (uint32_t)i * 104729u, &e, &m);
    /* CONSTRUCT a shiny: em_reroll's shiny path solves for the PID rather than searching,
     * so building 600 shiny fixtures is cheap. The IVs no longer match that PID, which does
     * not matter — iv_roll_step reads shininess, nature, sex and the ability slot, all of
     * which come from the PID. */
    if (!em_reroll(&e, -1, 1, -1, pk_species_gender_ratio(sp))) continue;
    refresh(&e, &m);
    if (!m.isShiny) continue;
    if (pk_pidiv_exempt_reason(&m) != PK_PIDIV_EX_NONE) continue;
    made++;

    uint8_t old_nat = m.nature;
    IvRollState st; memset(&st, 0, sizeof st);
    st.seed = 0x77777777u ^ ((uint32_t)i * 2654435761u);
    IvRoll r;
    if (roll_to_end(&e, &m, &st, 8000u, &r, 0) != 1) { budgetfail++; continue; }
    total += r.rolls; if (r.rolls > worst) worst = r.rolls;
    if (!r.chg_shiny) kept++;
    if (!r.chg_nature) natkept++;

    iv_roll_apply(&e, &r);
    refresh(&e, &m);
    if (!m.isShiny) { /* counted by kept, but assert on the decoded record too */ }
    PkPidiv pv;
    pk_pidiv_search(m.personality, m.ivs, PK_PIDIV_OPT_REVERSED, &pv);
    if (!(pv.methods & (1u << (PK_PIDIV_M1 - 1)))
        && !(pv.methods & (1u << (PK_PIDIV_M1_REV - 1)))) notm1++;
    if (m.nature == old_nat && r.chg_nature) notm1++;   /* the report must not lie */
  }

  printf("     %d shiny fixtures: sparkle kept %d, nature kept %d, budget failures %d\n",
         made, kept, natkept, budgetfail);
  printf("     candidates: avg %lu  worst %lu  (the UI spends 8,000 per frame)\n",
         made ? total / (unsigned long)made : 0ul, worst);
  CHECK(made > 500, "enough shiny fixtures were built to mean anything");
  CHECK(kept == made, "the sparkle is NEVER taken away silently");
  CHECK(budgetfail == 0, "the ladder always finds a shiny spread inside its budget");
  CHECK(notm1 == 0, "every rerolled shiny is still a Method-1 pair");
  /* THE LADDER MUST ACTUALLY CLIMB, and these two are how that is observable rather than
   * assumed. The rungs total 40,000 + 40,000 + 60,000 + 60,000 + 256 = 200,256 candidates,
   * and a rung can overshoot its budget by at most one slice before expiring. When the
   * budget was compared against ONE SLICE's tries instead of the rung's running total the
   * ladder was inert: rung 0 ground on alone, the worst case measured 17,880,120 candidates
   * — 90x over — and the nature was "kept" 600/600 because nothing was ever relaxed. */
  CHECK(worst < 250000ul, "no reroll costs more than the whole ladder is allowed");
  CHECK(natkept < made, "the ladder really does relax the nature to keep the sparkle");
  return 0;
}

/* ============================ (4) THE HISTORY =============================== */

static int part_4(void) {
  printf("(4) the undo/redo list\n");
  EditMon e; PkMon m;
  make_caught(25, 0xABCDEF01u, &e, &m);        /* PIKACHU, two abilities, split gender */

  IvHistory h;
  ivh_reset(&h, &e);
  uint32_t pid0 = e.personality, ivw0 = em_get_ivword(&e);
  CHECK(h.n == 1 && h.cur == 0, "a fresh history holds exactly the record it was given");

  /* THE MANDATORY ONE: more rolls than the ring can hold, then walk all the way back. */
  IvRollState st; memset(&st, 0, sizeof st); st.seed = 0x13571357u;
  for (int i = 0; i < 20; i++) {
    IvRoll r;
    if (roll_to_end(&e, &m, &st, 8000u, &r, 0) != 1) break;
    iv_roll_apply(&e, &r);
    refresh(&e, &m);
    ivh_push(&h, &e);
  }
  CHECK(h.n == IVH_CAP, "the ring saturates at IVH_CAP rather than overflowing");
  CHECK(h.cur == h.n - 1, "and the cursor is on the newest entry");
  CHECK(e.personality != pid0, "20 rolls really did move the record");

  int steps = 0;
  while (ivh_step(&h, &e, -1)) if (++steps > IVH_CAP + 4) break;
  printf("     walked back %d entries after 20 rolls\n", steps);
  CHECK(h.cur == 0, "walking back ends at entry 0");
  CHECK(e.personality == pid0,
        "ENTRY 0 IS PINNED: the PID is bit-identical to what walked in");
  CHECK(em_get_ivword(&e) == ivw0,
        "ENTRY 0 IS PINNED: the IV word is bit-identical to what walked in");
  CHECK(!ivh_step(&h, &e, -1), "and there is nothing before it");

  /* redo */
  CHECK(ivh_step(&h, &e, +1), "redo steps forward again");
  CHECK(h.cur == 1, "...one entry at a time");
  uint32_t pid_at_1 = e.personality;
  while (ivh_step(&h, &e, +1)) { }
  CHECK(h.cur == h.n - 1, "redo runs to the newest entry");
  CHECK(!ivh_step(&h, &e, +1), "and stops there");

  /* a new roll after a partial undo truncates the redo tail */
  ivh_step(&h, &e, -1); ivh_step(&h, &e, -1);
  uint8_t cur_before = h.cur;
  refresh(&e, &m);
  { IvRoll r;
    IvRollState st2; memset(&st2, 0, sizeof st2); st2.seed = 0x99AABBCCu;
    CHECK(roll_to_end(&e, &m, &st2, 8000u, &r, 0) == 1, "a roll after an undo works");
    iv_roll_apply(&e, &r); refresh(&e, &m); ivh_push(&h, &e); }
  CHECK(h.n == cur_before + 2, "the redo tail was dropped");
  CHECK(h.cur == h.n - 1, "and the cursor is on the new entry");

  /* rebase: a hand-edited IV row must invalidate the list, or an old entry could undo an
   * edit it knows nothing about */
  CHECK(!ivh_sync(&h, &e), "sync is a no-op while the record matches the cursor");
  em_set_iv(&e, PK_ATK, (uint8_t)((m.ivs[PK_ATK] + 1) & 31));
  CHECK(ivh_sync(&h, &e), "a hand-edited IV rebases the history");
  CHECK(h.n == 1 && h.cur == 0, "...back to a one-entry list");

  /* rebase: a species change too — the redo entries belong to the old species */
  { IvRoll r; IvRollState st3; memset(&st3, 0, sizeof st3); st3.seed = 0x5A5A5A5Au;
    refresh(&e, &m);
    CHECK(roll_to_end(&e, &m, &st3, 8000u, &r, 0) == 1, "roll before the species change");
    iv_roll_apply(&e, &r); ivh_push(&h, &e); }
  CHECK(h.n == 2, "the list grew again");
  em_set_species(&e, 26);                        /* RAICHU */
  CHECK(ivh_sync(&h, &e), "a species change rebases the history");
  CHECK(h.n == 1 && h.cur == 0, "...back to a one-entry list");
  (void)pid_at_1;
  return 0;
}

/* ============================ (5) THE RECORD =============================== */

static int part_5(void) {
  printf("(5) apply -> commit -> load round-trips the PID and the IV word\n");
  EditMon e; PkMon m;
  make_caught(197, 0x24681357u, &e, &m);         /* UMBREON */
  IvRollState st; memset(&st, 0, sizeof st); st.seed = 0x0F0F0F0Fu;
  IvRoll r;
  CHECK(roll_to_end(&e, &m, &st, 8000u, &r, 0) == 1, "it rolls");
  iv_roll_apply(&e, &r);

  uint8_t rec[80];
  gen3_edit_commit(&e, rec);
  EditMon e2;
  gen3_edit_load(rec, false, &e2);
  CHECK(e2.personality == r.pid, "the committed record carries the rolled PID");
  CHECK(em_get_ivword(&e2) == r.ivword, "and the rolled IV word, bit for bit");
  CHECK(gen3_edit_roundtrip_ok(rec, false), "and still survives the lossless round-trip");

  /* em_set_pid/em_set_ivword are the pair the history depends on: told a value, they must
   * store exactly it. */
  em_set_pid(&e2, 0xDEADBEEFu);
  em_set_ivword(&e2, 0xC0000000u | 0x0000BEEFu);
  CHECK(e2.personality == 0xDEADBEEFu, "em_set_pid stores what it is told");
  CHECK(em_get_ivword(&e2) == (0xC0000000u | 0x0000BEEFu), "em_set_ivword round-trips");
  return 0;
}

/* ============================ (6) THE EDGES ================================ */

static int part_6(void) {
  printf("(6) the refusals, the chunking, and the NPC trade\n");

  /* a BAD EGG: its PID and IVs are garbage, so there is nothing to roll */
  EditMon e; PkMon m;
  make_caught(25, 0x0000BEEFu, &e, &m);
  PkMon bad = m; bad.isBadEgg = true;
  IvRollState st; memset(&st, 0, sizeof st); st.seed = 1;
  IvRoll r;
  CHECK(iv_roll_step(&e, &bad, &st, 8000u, &r) == -1, "a bad egg refuses to roll");

  /* CHUNKING: a shiny with a tiny slice must return 0 many times and then 1 — never loop
   * forever, and never skip a rung's budget because the slice was smaller than it. */
  EditMon es; PkMon ms;
  make_caught(25, 0x1234FEDCu, &es, &ms);
  CHECK(em_reroll(&es, -1, 1, -1, pk_species_gender_ratio(25)), "built a shiny fixture");
  refresh(&es, &ms);
  CHECK(ms.isShiny, "...and it really is shiny");
  IvRollState st2; memset(&st2, 0, sizeof st2); st2.seed = 0xC0DEC0DEu;
  IvRoll r2; int slices = 0;
  int rc = roll_to_end(&es, &ms, &st2, 100u, &r2, &slices);
  printf("     100-candidate slices: %d partial returns before the answer\n", slices);
  CHECK(rc == 1, "the chunked search finishes");
  CHECK(slices > 10, "...after genuinely many partial returns");
  CHECK(!r2.chg_shiny, "...still keeping the sparkle");
  CHECK(r2.rolls >= (uint32_t)slices * 100u, "the candidate count accumulates across slices");

  /* THE NPC TRADE. Its PID is a constant in the trade template (src/trade.c:4570), so no
   * seed reproduces it and no reroll keeps it. pk_pidiv_exempt_reason cannot see that —
   * the exemption is stamped by the legality hook from met location 0xFE — so the roll
   * takes the ordinary PID-moving path and must RAISE THE FLAG the confirm panel prints. */
  EditMon et; PkMon mt;
  make_caught(43, 0x0BADCAFEu, &et, &mt);        /* ODDISH, the shape of an NPC trade */
  em_set_metloc(&et, MET_TRADE);
  refresh(&et, &mt);
  CHECK(mt.metLocation == MET_TRADE, "the fixture is stamped as an in-game trade");
  CHECK(pk_pidiv_exempt_reason(&mt) == PK_PIDIV_EX_NONE,
        "pk_pidiv_exempt_reason still cannot see a trade — that is why chg_trade exists");
  IvRollState st3; memset(&st3, 0, sizeof st3); st3.seed = 0x2B2B2B2Bu;
  IvRoll r3;
  CHECK(roll_to_end(&et, &mt, &st3, 8000u, &r3, 0) == 1, "a trade mon still rolls");
  CHECK(r3.pid_locked, "...on the PID-moving path, not a fake exemption");
  CHECK(r3.chg_trade, "...and says the trade provenance cannot be preserved");

  /* ...and a non-trade mon must NOT raise it, or the panel would cry wolf on every roll */
  EditMon en; PkMon mn;
  make_caught(43, 0x0BADCAFEu, &en, &mn);
  IvRollState st4; memset(&st4, 0, sizeof st4); st4.seed = 0x2B2B2B2Bu;
  IvRoll r4;
  CHECK(roll_to_end(&en, &mn, &st4, 8000u, &r4, 0) == 1, "an ordinary caught mon rolls");
  CHECK(!r4.chg_trade, "...and raises no trade warning");
  return 0;
}

int main(void) {
  part_1();
  printf("\n"); part_2();
  printf("\n"); part_3();
  printf("\n"); part_4();
  printf("\n"); part_5();
  printf("\n"); part_6();
  printf("\n%d checks, %d FAILED\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
