/* Host test for the shared Method-1 roller (pk_spread_*) and gen3_build_mon_spread.
 *   cc -std=c11 -O2 -I source tests/host_spread_test.c source/gen3_gen.c \
 *      source/gen3_pidiv.c source/gen3_edit.c source/gen3_legality2.c \
 *      source/gen3_legality_hooks.c source/learnsets2.c source/encounters.c \
 *      source/statics.c source/gen3_daycare.c source/evolutions.c source/gen3_mon.c \
 *      source/gen3_save.c source/data_tables.c -o /tmp/hsp && /tmp/hsp
 *
 * source/learnsets2.c and source/encounters.c are GENERATED and git-ignored — run
 * tools/gen_learnsets2.py and tools/gen_encounters.py --from-rom first, or the link
 * fails (the same contract host_legalbuild_test.c carries).
 *
 * WHY THIS EXISTS. Gen 3 draws a Pokemon's personality value AND both of its IV words
 * from ONE LCRNG stream (pokeemerald src/pokemon.c:2216, 2277-2293), so a hand-picked PID
 * sitting next to hand-picked IVs is a pair no seed could ever have produced — and
 * PokeDNA's own auditor says exactly that ("No PID/IV RNG method matches"). The create
 * flow used to write one: a PID from dc_seed() and six IVs left at zero. pk_spread_roll
 * rolls a SEED instead and takes the PID and the IVs it produces together, so the verify
 * step below is an ASSERTION (re-find the spread at its own seed) rather than a search
 * that is allowed to fail.
 *
 * The six parts:
 *   (A) round-trip: 20,000 forward rolls in EACH PID order, each re-found by the shipped
 *       pk_pidiv_search as Method 1 / Method 1 (Unown) at the exact seed. The LCRNG
 *       constants are RE-TYPED here (1103515245 / 24691), the discipline
 *       host_pidiv_test.c already uses, so a wrong shared constant fails instead of
 *       cancelling out against itself.
 *   (B) every constraint is reachable, and its cost is PRINTED so UI budgeting comes from
 *       measurement: all 25 natures, both genders on two ratios, all 28 Unown letters,
 *       shiny, an ability slot, an IV floor.
 *   (C) the cap FAILS SOFT and TERMINATES: an impossible ask stops at exactly its budget
 *       and still hands back a real Method-1 pair; cap = 0xFFFFFFFF cannot wrap.
 *   (D) the paired sweep: every real species x 3 origin games built the OLD way (a PID,
 *       zero IVs) and the NEW way (a rolled seed), both graded with PK2_RUN_PIDIV.
 *       PID/RNG suspects must go from "many" to ZERO, and no category may get worse.
 *   (E) UNOWN as the worked example — a created Unown must read "Method 1 (Unown)", not
 *       "Method 1", because every real one does. Plus the caught-origin species list.
 *   (F) Guy's Venusaur: L32, metLevel 0, PIDIV-exempt as HATCHED, IV sum > 0, LEGAL.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gen3_gen.h"
#include "gen3_pidiv.h"
#include "gen3_edit.h"
#include "gen3_mon.h"
#include "gen3_legality2.h"
#include "gen3_legality_hooks.h"
#include "learnsets2.h"
#include "encounters.h"
#include "evolutions.h"
#include "data_tables.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

/* RE-TYPED, deliberately: gen3_pidiv.c's LCRNG_A/LCRNG_C are what both the roller and the
 * search read, so asking the roller to check itself with them would prove nothing. */
#define T_A 1103515245u
#define T_C 24691u

static const uint32_t k_otid = 0x3B8C97A0u;   /* TID 0x97A0 / SID 0x3B8C — nothing special */

/* ============================ (A) THE ROUND TRIP ============================ */

/* The four LCRNG outputs a seed produces, assembled the way CreateBoxMon does. Written
 * from the decomp facts directly rather than by calling spread_fill. */
static void forward(uint32_t seed, int rev, uint32_t* pid, uint8_t ivs[PK_NSTATS]) {
  uint32_t s = seed;
  s = T_A * s + T_C; uint16_t r1 = (uint16_t)(s >> 16);
  s = T_A * s + T_C; uint16_t r2 = (uint16_t)(s >> 16);
  s = T_A * s + T_C; uint16_t r3 = (uint16_t)(s >> 16);
  s = T_A * s + T_C; uint16_t r4 = (uint16_t)(s >> 16);
  *pid = rev ? (((uint32_t)r1 << 16) | r2) : (((uint32_t)r2 << 16) | r1);
  ivs[PK_HP]  = (uint8_t)( r3        & 31u);
  ivs[PK_ATK] = (uint8_t)((r3 >>  5) & 31u);
  ivs[PK_DEF] = (uint8_t)((r3 >> 10) & 31u);
  ivs[PK_SPE] = (uint8_t)( r4        & 31u);
  ivs[PK_SPA] = (uint8_t)((r4 >>  5) & 31u);
  ivs[PK_SPD] = (uint8_t)((r4 >> 10) & 31u);
}

static int part_a(void) {
  printf("(A) 20,000 rolls in each PID order, re-found by pk_pidiv_search\n");
  for (int rev = 0; rev < 2; rev++) {
    int notfound = 0, wrongseed = 0, allzero = 0, mismatch = 0;
    const uint8_t want_m = rev ? PK_PIDIV_M1_REV : PK_PIDIV_M1;
    uint32_t seed = 0x12345678u ^ (rev ? 0xA5A5A5A5u : 0u);
    for (int i = 0; i < 20000; i++) {
      seed = T_A * seed + T_C;                        /* a fresh, unrelated seed each time */
      PkSpread s;
      pk_spread_from_seed(seed, rev ? PK_SPREAD_REVERSED : 0u, &s);

      /* the roller agrees with the locally re-typed generator */
      uint32_t pid_ref; uint8_t iv_ref[PK_NSTATS];
      forward(seed, rev, &pid_ref, iv_ref);
      if (s.pid != pid_ref || memcmp(s.ivs, iv_ref, PK_NSTATS) != 0) mismatch++;

      int sum = 0; for (int k = 0; k < PK_NSTATS; k++) sum += s.ivs[k];
      if (sum == 0) allzero++;      /* anti-vacuity: a roller returning zeros would "pass" */

      PkPidiv r;
      pk_pidiv_search(s.pid, s.ivs, PK_PIDIV_OPT_REVERSED, &r);
      if (!(r.methods & (1u << (want_m - 1)))) { notfound++; continue; }
      /* the reported seed is the state BEFORE the first call, which is what we rolled */
      if (r.method == want_m && r.seed != s.seed) wrongseed++;
    }
    printf("     %-8s not-found=%d wrong-seed=%d all-zero-IVs=%d generator-mismatch=%d\n",
           rev ? "reversed" : "normal", notfound, wrongseed, allzero, mismatch);
    CHECK(mismatch == 0, "pk_spread_from_seed matches the re-typed LCRNG generator");
    CHECK(notfound == 0, "every rolled spread is re-found as Method 1 by the shipped search");
    CHECK(wrongseed == 0, "the search reports the seed the roll came from");
    CHECK(allzero < 3, "rolled IV spreads are not systematically zero");
  }
  return 0;
}

/* ============================ (B) EVERY CONSTRAINT =========================== */

static int part_b(void) {
  printf("(B) every constraint reached, with its measured cost\n");
  uint32_t seed = 0xDEADBEEFu;
  PkSpread s;
  PkSpreadWant w;

  /* nothing wanted: exactly one trial */
  pk_spread_want_init(&w); w.otId = k_otid;
  CHECK(pk_spread_roll(seed, &w, &s), "an unconstrained roll always succeeds");
  CHECK(s.tries == 1, "an unconstrained roll costs one trial");

  uint32_t nat_tot = 0, nat_max = 0;
  for (int n = 0; n < 25; n++) {
    pk_spread_want_init(&w); w.otId = k_otid; w.nature = (int8_t)n;
    seed = T_A * seed + T_C;
    CHECK(pk_spread_roll(seed, &w, &s), "every nature is reachable");
    CHECK((int)(s.pid % 25u) == n, "the rolled PID carries the asked-for nature");
    nat_tot += s.tries; if (s.tries > nat_max) nat_max = s.tries;
  }
  printf("     nature      avg %lu  worst %lu\n",
         (unsigned long)(nat_tot / 25), (unsigned long)nat_max);

  /* two ratios: 127 (even split) and 31 (mostly female). Fixed-gender species are not
   * constrained at all — the PID cannot change their sex — so they are not tested here. */
  const uint8_t ratios[2] = { 127, 31 };
  for (int ri = 0; ri < 2; ri++) {
    for (int g = 0; g < 2; g++) {
      pk_spread_want_init(&w); w.otId = k_otid;
      w.gender = (int8_t)g; w.gender_ratio = ratios[ri];
      seed = T_A * seed + T_C;
      CHECK(pk_spread_roll(seed, &w, &s), "both genders are reachable on a split ratio");
      CHECK(pk_gender_from(s.pid, ratios[ri]) == (uint8_t)g, "the rolled PID has that sex");
      printf("     gender %d ratio %-3u tries %lu\n", g, ratios[ri], (unsigned long)s.tries);
    }
  }

  uint32_t frm_max = 0;
  for (int f = 0; f < 28; f++) {
    pk_spread_want_init(&w); w.otId = k_otid;
    w.unown_form = (uint8_t)f; w.opts = PK_SPREAD_REVERSED;
    seed = T_A * seed + T_C;
    CHECK(pk_spread_roll(seed, &w, &s), "every Unown letter is reachable");
    CHECK(pk_unown_form(s.pid) == (uint8_t)f, "the rolled PID carries that letter");
    if (s.tries > frm_max) frm_max = s.tries;
  }
  printf("     Unown letter worst %lu\n", (unsigned long)frm_max);

  pk_spread_want_init(&w); w.otId = k_otid; w.shiny = 1;
  CHECK(pk_spread_roll(0x0BADF00Du, &w, &s), "a shiny spread is reachable");
  CHECK(pk_is_shiny(s.pid, (uint16_t)k_otid, (uint16_t)(k_otid >> 16)), "and it is shiny");
  printf("     shiny        tries %lu\n", (unsigned long)s.tries);

  pk_spread_want_init(&w); w.otId = k_otid; w.shiny = 0;
  CHECK(pk_spread_roll(0x0BADF00Du, &w, &s), "refusing shiny is reachable");
  CHECK(!pk_is_shiny(s.pid, (uint16_t)k_otid, (uint16_t)(k_otid >> 16)), "and it is not shiny");

  for (int a = 0; a < 2; a++) {
    pk_spread_want_init(&w); w.otId = k_otid; w.ability = (uint8_t)a;
    seed = T_A * seed + T_C;
    CHECK(pk_spread_roll(seed, &w, &s), "both ability slots are reachable");
    CHECK((int)(s.pid & 1u) == a, "the rolled PID carries that ability slot");
  }

  pk_spread_want_init(&w); w.otId = k_otid; w.min_iv_sum = 150;
  CHECK(pk_spread_roll(0x1u, &w, &s), "a 150+ IV floor is reachable");
  { int sum = 0; for (int k = 0; k < PK_NSTATS; k++) sum += s.ivs[k];
    CHECK(sum >= 150, "the rolled spread meets the IV floor");
    printf("     IV floor 150 tries %lu (sum %d)\n", (unsigned long)s.tries, sum); }

  /* min_iv_sum is CLAMPED to 6x31 rather than taken literally: an unclamped 255 would ask
   * for something no spread can satisfy, so the roll would always burn the whole cap and
   * hand back a soft-fail. Clamped, "255" and "186" are the SAME ask — which is what this
   * asserts, from one seed, rather than trying to actually find a 6x31 spread (1 in 10^9). */
  PkSpread s186, s255;
  pk_spread_want_init(&w); w.otId = k_otid; w.min_iv_sum = 186; w.cap = 20000u;
  bool ok186 = pk_spread_roll(0x2u, &w, &s186);
  pk_spread_want_init(&w); w.otId = k_otid; w.min_iv_sum = 255; w.cap = 20000u;
  bool ok255 = pk_spread_roll(0x2u, &w, &s255);
  CHECK(ok186 == ok255 && s186.pid == s255.pid && s186.tries == s255.tries,
        "min_iv_sum > 186 is clamped to 186, not left unsatisfiable");
  return 0;
}

/* ============================ (C) THE CAP =================================== */

static int part_c(void) {
  printf("(C) the cap fails soft and terminates\n");
  PkSpread s; PkSpreadWant w;

  /* Hardy AND a perfect 6x31 spread: about 1 in 25 * 32^6 — unreachable in 5,000 seeds. */
  pk_spread_want_init(&w);
  w.otId = k_otid; w.nature = 0; w.min_iv_sum = 186; w.cap = 5000u;
  bool ok = pk_spread_roll(0x55555555u, &w, &s);
  CHECK(!ok, "an impossible ask returns false");
  CHECK(s.ok == 0, "...and says so in out->ok");
  CHECK(s.tries == 5000u, "...after exactly its budget, not one seed more");
  CHECK(s.pid != 0, "...and still hands back a real PID");
  { /* the fallback is STILL a matched Method-1 pair — that is the whole promise */
    PkPidiv r; pk_pidiv_search(s.pid, s.ivs, 0, &r);
    CHECK((r.methods & (1u << (PK_PIDIV_M1 - 1))) != 0,
          "the soft-fail spread is still a Method-1 pair");
  }

  /* cap = 0xFFFFFFFF with an easy want: proves `t < cap` cannot wrap into a hang. */
  pk_spread_want_init(&w); w.otId = k_otid; w.nature = 7; w.cap = 0xFFFFFFFFu;
  CHECK(pk_spread_roll(0x99999999u, &w, &s), "cap = 0xFFFFFFFF terminates on an easy want");
  CHECK((int)(s.pid % 25u) == 7, "...with the right nature");
  printf("     soft-fail tries=5000, huge-cap tries=%lu\n", (unsigned long)s.tries);
  return 0;
}

/* ============================ (D) THE PAIRED SWEEP =========================== */

/* dc_seed()'s shape: pdna_main.c ORs bit 0 in, so the OLD create flow's PID was always
 * odd — which is also why every two-ability created mon used to land on slot 1. */
static uint32_t old_pid(uint16_t sp, uint8_t game) {
  return ((uint32_t)sp * 0x9E3779B9u + (uint32_t)game * 0x85EBCA6Bu) | 1u;
}

static int has_pid_suspect(const Pk2Report* R) {
  for (int i = 0; i < R->n; i++)
    if (R->row[i].sev != PK2_INFO && strstr(R->row[i].text, "PID/IV RNG")) return 1;
  return 0;
}
static int n_suspect(const Pk2Report* R) {
  int n = 0;
  for (int i = 0; i < R->n; i++) if (R->row[i].sev != PK2_INFO) n++;
  return n;
}

static int part_d(void) {
  printf("(D) the paired sweep: old (PID, zero IVs) vs new (a rolled seed)\n");
  CHECK(lg2_have_data(), "learnsets2.c is linked in (else this measures nothing)");
  CHECK(pk_wild_have_data(), "encounters.c is linked in (else this measures nothing)");

  static const uint8_t games[3] = { 2, 3, 4 };   /* Ruby / Emerald / FireRed */
  int built = 0, nsp = 0;
  int old_pidsus = 0, new_pidsus = 0;
  int old_any = 0, new_any = 0;
  int old_ill = 0, new_ill = 0;
  int roundtrip_bad = 0, zeroiv = 0, abilbad = 0, paircarry_bad = 0;

  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0) continue;
    nsp++;
    uint8_t lvl = gen3_build_level(sp);
    for (int gi = 0; gi < 3; gi++) {
      uint8_t recO[80], recN[80];
      PkMon mO, mN;

      gen3_build_mon(sp, lvl, old_pid(sp, games[gi]), k_otid, "GUY", games[gi], recO);
      memset(&mO, 0, sizeof mO); pk_decode_mon(recO, false, &mO);

      Gen3BuildInfo info;
      gen3_build_mon_spread(sp, lvl, old_pid(sp, games[gi]), k_otid, "GUY", games[gi],
                            0, recN, &info);
      memset(&mN, 0, sizeof mN); pk_decode_mon(recN, false, &mN);
      built++;

      /* the record really carries the pair that was rolled */
      if (mN.personality != info.spread.pid) paircarry_bad++;
      if (memcmp(mN.ivs, info.spread.ivs, PK_NSTATS) != 0) paircarry_bad++;
      { int sum = 0; for (int k = 0; k < PK_NSTATS; k++) sum += mN.ivs[k];
        if (sum == 0) zeroiv++; }
      /* the ability bit survived the six em_set_iv read-modify-writes */
      if (pk_species_ability(sp, 1) != 0 && mN.abilityNum != (uint8_t)(mN.personality & 1u))
        abilbad++;
      if (!gen3_edit_roundtrip_ok(recN, false)) roundtrip_bad++;

      Pk2Report RO, RN;
      pk_check_legality2_ex(&mO, &RO, PK2_RUN_PIDIV);
      pk_check_legality2_ex(&mN, &RN, PK2_RUN_PIDIV);
      old_pidsus += has_pid_suspect(&RO);
      new_pidsus += has_pid_suspect(&RN);
      old_any += (n_suspect(&RO) > 0);
      new_any += (n_suspect(&RN) > 0);
      old_ill += (RO.grade == PK2_ILLEGAL);
      new_ill += (RN.grade == PK2_ILLEGAL);
      CHECK(RN.n_invalid == 0, "no INVALID row on any spread-built record");
    }
  }

  printf("     built %d (%d species x 3 origin games)\n", built, nsp);
  printf("     PID/RNG suspects: old %d -> new %d\n", old_pidsus, new_pidsus);
  printf("     any suspect:      old %d -> new %d\n", old_any, new_any);
  printf("     ILLEGAL:          old %d -> new %d\n", old_ill, new_ill);
  CHECK(old_pidsus > 0, "the OLD build really did produce unmatched PID/IV pairs");
  CHECK(new_pidsus == 0, "the NEW build produces none");
  CHECK(new_any <= old_any, "no category got worse");
  CHECK(new_ill <= old_ill, "nothing newly grades ILLEGAL");
  CHECK(paircarry_bad == 0, "every record carries exactly the pair that was rolled");
  CHECK(zeroiv == 0, "no spread-built record has an all-zero IV spread");
  CHECK(abilbad == 0, "the ability bit still matches the PID after the IV writes");
  CHECK(roundtrip_bad == 0, "every spread-built record survives the lossless round-trip");
  return 0;
}

/* ============================ (E) UNOWN ==================================== */

/* The species Gen 3 gives no egg route get a CAUGHT origin from gen3_build_mon (met level
 * > 0), which is the only case where the PID/IV pair is actually tested — everything else
 * is PIDIV-exempt as hatched whichever way it is built. These fifteen are the test's own
 * fixture: each is asserted to be caught-origin, so a change to gen3_species_can_hatch
 * shows up here as a diff rather than silently agreeing with itself. It is a REQUIRED
 * SUBSET, not the whole set — the sweep below prints and checks every member. */
static const uint16_t k_caught_origin[15] = {
   30,  31,                 /* NIDORINA NIDOQUEEN */
  144, 145, 146,            /* ARTICUNO ZAPDOS MOLTRES */
  150, 151,                 /* MEWTWO MEW */
  201,                      /* UNOWN */
  243, 244, 245,            /* RAIKOU ENTEI SUICUNE */
  249, 250, 251,            /* LUGIA HO-OH CELEBI */
  360,                      /* WYNAUT */
};

static int part_e(void) {
  printf("(E) UNOWN is generated in the reversed PID order\n");
  CHECK(gen3_spread_opts(201) == PK_SPREAD_REVERSED, "Unown rolls high-half-first");
  CHECK(gen3_spread_opts(25) == 0u, "everything else rolls low-half-first");

  uint8_t rec[80]; PkMon m; Gen3BuildInfo info;
  PkSpreadWant w; pk_spread_want_init(&w);
  w.unown_form = 25;                                  /* letter Z, the create flow's picker */
  gen3_build_mon_spread(201, gen3_build_level(201), 0xC0FFEE11u, k_otid, "GUY", 3,
                        &w, rec, &info);
  memset(&m, 0, sizeof m); pk_decode_mon(rec, false, &m);
  CHECK(pk_unown_form(m.personality) == 25, "the created Unown is the letter that was asked for");

  /* NOT R.pidiv_method — that is a REPORT code (pk2_pidiv_report_method collapses all
   * three reversed methods to 5), so it cannot tell Method 1 (Unown) from Method 2
   * (Unown). Ask the search itself, which is what the ORIGIN screen's wording comes
   * from. */
  PkPidiv pv;
  pk_pidiv_search(m.personality, m.ivs, PK_PIDIV_OPT_REVERSED, &pv);
  Pk2Report R;
  pk_check_legality2_ex(&m, &R, PK2_RUN_PIDIV);
  printf("     unown %c (create flow) grade=%s method=%s (report code %u)\n",
         "ABCDEFGHIJKLMNOPQRSTUVWXYZ!?"[pk_unown_form(m.personality)],
         pk2_grade_name(R.grade), pk_pidiv_method_name(pv.method),
         (unsigned)R.pidiv_method);
  CHECK(pv.method == PK_PIDIV_M1_REV,
        "a created Unown reads \"Method 1 (Unown)\", which is what every real one reads");
  CHECK(R.pidiv_method == 5, "and the report calls it the reversed order");
  CHECK(!has_pid_suspect(&R), "and carries no PID/IV suspicion");

  /* The caught-origin set is the one where any of this is observable at all. */
  for (unsigned i = 0; i < sizeof k_caught_origin / sizeof k_caught_origin[0]; i++)
    CHECK(!gen3_species_can_hatch(k_caught_origin[i]),
          "every pinned species really is caught-origin");

  int caught = 0, caught_sus = 0;
  printf("     caught-origin species (no egg route):");
  for (uint16_t sp = 1; sp <= 411; sp++) {
    if (pk_national_no(sp) == 0 || gen3_species_can_hatch(sp)) continue;
    caught++;
    printf("%s%s", (caught % 6 == 1) ? "\n       " : " ", pk_species_name(sp));
    uint8_t r2[80]; PkMon m2;
    gen3_build_mon_spread(sp, gen3_build_level(sp), 0x1234u + sp, k_otid, "GUY", 3, 0, r2, 0);
    memset(&m2, 0, sizeof m2); pk_decode_mon(r2, false, &m2);
    CHECK(pk_pidiv_exempt_reason(&m2) == PK_PIDIV_EX_NONE,
          "a caught-origin build really is PID/IV-testable");
    Pk2Report R2; pk_check_legality2_ex(&m2, &R2, PK2_RUN_PIDIV);
    if (has_pid_suspect(&R2)) caught_sus++;
  }
  printf("\n     %d caught-origin species, all PID/IV-testable, %d still PID-suspect\n",
         caught, caught_sus);
  CHECK(caught_sus == 0, "every caught-origin build matches an RNG method");
  return 0;
}

/* ============================ (F) GUY'S VENUSAUR ============================ */

static int part_f(void) {
  printf("(F) Guy's Venusaur: metLevel 0 is HATCHED, not an error\n");
  uint8_t rec[80]; PkMon m; Gen3BuildInfo info;
  gen3_build_mon_spread(3, 32, 0x51CE01u, k_otid, "GUY", 3, 0, rec, &info);
  memset(&m, 0, sizeof m); pk_decode_mon(rec, false, &m);

  int sum = 0; for (int k = 0; k < PK_NSTATS; k++) sum += m.ivs[k];
  uint8_t ex = pk_pidiv_exempt_reason(&m);
  Pk2Report R; pk_check_legality2_ex(&m, &R, PK2_RUN_PIDIV);
  printf("     Venusaur: L%u metLv=%u IVs sum %d pidiv-exempt='%s' grade=%s\n",
         (unsigned)info.level, (unsigned)m.metLevel, sum, pk_pidiv_exempt_name(ex),
         pk2_grade_name(R.grade));
  CHECK(info.level == 32, "the level asked for is the level built");
  CHECK(info.hatched == 1, "Venusaur has an egg route, so the record claims hatched");
  CHECK(m.metLevel == 0, "met level 0 STAYS 0 — it is how Gen 3 records a hatch");
  CHECK(ex == PK_PIDIV_EX_HATCHED, "and it is what makes the record PIDIV-exempt");
  CHECK(sum > 0, "the IVs are no longer all zero — the bug Guy reported");
  CHECK(R.grade == PK2_LEGAL, "and the whole record grades LEGAL");
  return 0;
}

int main(void) {
  part_a();
  printf("\n"); part_b();
  printf("\n"); part_c();
  printf("\n"); part_d();
  printf("\n"); part_e();
  printf("\n"); part_f();
  printf("\n%d checks, %d FAILED\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
