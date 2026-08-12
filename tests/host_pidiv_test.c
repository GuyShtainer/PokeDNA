/* Host test for the PIDIV reverse search (gen3_pidiv.c).
 *   cc -std=c11 -O2 -I source tests/host_pidiv_test.c source/gen3_pidiv.c \
 *      source/gen3_mon.c source/gen3_save.c source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c \
 *      source/data_tables.c -o /tmp/hp
 *   /tmp/hp /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/Emerald.sav ...
 *
 * The forward LCRNG below is written from the decomp facts DIRECTLY (constants
 * re-typed, call orders spelled out) rather than by calling into gen3_pidiv.c, so
 * a wrong constant or a swapped word there fails the round-trip instead of
 * cancelling out. The one thing a round-trip can never prove is that the METHOD
 * DEFINITIONS match retail — only real cartridge saves can, which is what (4) is
 * for: if the call order were wrong, real wild-caught Pokemon would match at
 * chance level (~0) instead of the rates it reports.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "gen3_pidiv.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

/* ---- the generator, forward ------------------------------------------------
 * pokeemerald include/random.h:16 (1103515245 * v + 24691) and src/random.c:11-16
 * (Random() returns gRngValue >> 16). */
static uint32_t g_state;
static uint16_t rnd(void) { g_state = 1103515245u * g_state + 24691u; return (uint16_t)(g_state >> 16); }

/* Build the (PID, IVs) a given seed produces under a given method. `rev` swaps
 * which PID half the first call supplies. IV order is PkMon's: HP Atk Def Spe SpA
 * SpD (pokeemerald src/pokemon.c:2277-2293 splits the two words that way). */
static void gen_forward(uint32_t seed, int pattern, int rev, uint32_t* pid, uint8_t ivs[6]) {
  g_state = seed;
  uint16_t a = rnd(), b = rnd();
  *pid = rev ? (((uint32_t)a << 16) | b) : (((uint32_t)b << 16) | a);
  uint16_t w1, w2;
  if (pattern == 0) {            /* Method 1: r3, r4          */
    w1 = rnd(); w2 = rnd();
  } else if (pattern == 1) {     /* Method 2: burn r3, r4, r5 */
    (void)rnd(); w1 = rnd(); w2 = rnd();
  } else {                       /* Method 4: r3, burn r4, r5 */
    w1 = rnd(); (void)rnd(); w2 = rnd();
  }
  ivs[PK_HP]  = (uint8_t)( w1        & 31);
  ivs[PK_ATK] = (uint8_t)((w1 >>  5) & 31);
  ivs[PK_DEF] = (uint8_t)((w1 >> 10) & 31);
  ivs[PK_SPE] = (uint8_t)( w2        & 31);
  ivs[PK_SPA] = (uint8_t)((w2 >>  5) & 31);
  ivs[PK_SPD] = (uint8_t)((w2 >> 10) & 31);
}

static const uint8_t k_expect[2][3] = {
  { PK_PIDIV_M1,     PK_PIDIV_M2,     PK_PIDIV_M4     },
  { PK_PIDIV_M1_REV, PK_PIDIV_M2_REV, PK_PIDIV_M4_REV },
};
static const char* k_pattern[3] = { "Method 1", "Method 2", "Method 4" };

/* xorshift32 — the test's own PRNG, deliberately NOT the LCRNG under test, so a
 * "random" pair can never accidentally be a legitimate spread. */
static uint32_t xs = 0x2545F491u;
static uint32_t xr(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

/* ---- (1) round-trip -------------------------------------------------------- */
static void test_roundtrip(void) {
  printf("(1) round-trip: forward-generate, then reverse-search\n");
  const int N = 400;                       /* 400 seeds x 3 patterns x 2 orders */
  for (int rev = 0; rev < 2; rev++) {
    for (int p = 0; p < 3; p++) {
      int found = 0, right_method = 0, right_seed = 0;
      for (int i = 0; i < N; i++) {
        uint32_t seed = xr();
        uint32_t pid; uint8_t ivs[6];
        gen_forward(seed, p, rev, &pid, ivs);
        PkPidiv r;
        pk_pidiv_search(pid, ivs, PK_PIDIV_OPT_REVERSED, &r);
        if (r.method != PK_PIDIV_NONE) found++;
        if (r.methods & (1u << (k_expect[rev][p] - 1))) right_method++;
        /* The reported seed belongs to r.method; when several patterns match the
         * same record the seed is still one that generates it, so only require
         * the expected pattern to be present and the seed to regenerate the PID. */
        g_state = r.seed; uint16_t a = rnd(), b = rnd();
        uint32_t back = (r.method >= PK_PIDIV_M1_REV) ? (((uint32_t)a << 16) | b)
                                                      : (((uint32_t)b << 16) | a);
        if (r.method != PK_PIDIV_NONE && back == pid) right_seed++;
      }
      printf("  %s%-8s: %3d/%d found, %3d/%d expected-method, %3d/%d seed regenerates PID\n",
             rev ? "rev " : "    ", k_pattern[p], found, N, right_method, N, right_seed, N);
      CHECK(found == N, "every synthetic spread is found");
      CHECK(right_method == N, "the generating method is among the matches");
      CHECK(right_seed == N, "the reported seed regenerates the PID");
    }
  }

  /* the exact seed is recoverable, not merely "a" seed: spot-check one. */
  uint32_t pid; uint8_t ivs[6];
  gen_forward(0x1234ABCDu, 0, 0, &pid, ivs);
  PkPidiv r; pk_pidiv_search(pid, ivs, 0, &r);
  printf("  spot: seed 0x1234ABCD -> PID %08X ivs %d/%d/%d/%d/%d/%d -> %s seed %08X (cands %d)\n",
         pid, ivs[0], ivs[1], ivs[2], ivs[3], ivs[4], ivs[5],
         pk_pidiv_method_name(r.method), r.seed, r.ncand);
  CHECK(r.seed == 0x1234ABCDu, "the exact generating seed is recovered");

  /* roamer mode: the same Method-1 spread with only the low 8 IV bits preserved
   * (RSE Latias/Latios) must still find the seed. */
  uint8_t roam[6] = { ivs[PK_HP], (uint8_t)(ivs[PK_ATK] & 7), 0, 0, 0, 0 };
  PkPidiv rr; pk_pidiv_search(pid, roam, PK_PIDIV_OPT_ROAMER, &rr);
  printf("  roamer (8-bit compare): %s seed %08X\n", pk_pidiv_method_name(rr.method), rr.seed);
  CHECK(rr.method == PK_PIDIV_M1 && rr.seed == 0x1234ABCDu, "roamer truncated compare finds the seed");
}

/* ---- (2) false-positive rate ----------------------------------------------- */
static void test_false_positives(void) {
  const int N = 100000;
  int hits = 0, maxcand = 0; long cands = 0;
  printf("(2) false positives over %d random (PID, IV) pairs\n", N);
  for (int i = 0; i < N; i++) {
    uint32_t pid = xr();
    uint8_t ivs[6];
    for (int s = 0; s < 6; s++) ivs[s] = (uint8_t)(xr() & 31);
    PkPidiv r;
    pk_pidiv_search(pid, ivs, 0, &r);
    cands += r.ncand;
    if (r.ncand > maxcand) maxcand = r.ncand;
    if (r.method != PK_PIDIV_NONE) hits++;
  }
  /* gen3_pidiv.c sizes its candidate array from a proof that no (first, second)
   * pair can ever yield more than THREE survivors. Watch the observed maximum:
   * if this ever prints 4 the proof — and the array — are wrong. */
  printf("  max PID-stage candidates seen: %d (the array is sized for 4; proof says 3)\n", maxcand);
  CHECK(maxcand <= 3, "the candidate-count bound holds");
  double per = (double)cands / N;
  /* Each surviving candidate is tested against 3 patterns, each demanding a
   * 30-bit IV agreement, so the analytic rate is per*3*2^-30. Printed next to the
   * measured count because at ~1e-8 a 100k sample can only ever show 0. */
  printf("  matched: %d/%d   mean PID-stage candidates/pair: %.4f\n", hits, N, per);
  printf("  analytic FP rate = %.4f cand * 3 patterns * 2^-30 = %.3e per mon\n",
         per, per * 3.0 / 1073741824.0);
  CHECK(hits <= 1, "random spreads essentially never match");
  CHECK(per > 0.8 && per < 1.2, "PID stage leaves ~1 candidate (65536 * 2^-16)");
}

/* ---- (2b) the negative control the round-trip needs ------------------------- */
static void test_broken_input(void) {
  printf("(2b) a real spread with ONE IV bumped must stop matching\n");
  int still = 0;
  for (int i = 0; i < 200; i++) {
    uint32_t seed = xr(), pid; uint8_t ivs[6];
    gen_forward(seed, 0, 0, &pid, ivs);
    ivs[PK_SPD] = (uint8_t)((ivs[PK_SPD] + 1) & 31);      /* break it */
    PkPidiv r; pk_pidiv_search(pid, ivs, PK_PIDIV_OPT_REVERSED, &r);
    if (r.method != PK_PIDIV_NONE) still++;
  }
  printf("  200 spreads with SpD+1: %d still match (want 0)\n", still);
  CHECK(still == 0, "a single wrong IV breaks the correlation");
}

/* ---- (3) cost -------------------------------------------------------------- */
static void test_timing(void) {
  const int N = 200;
  uint32_t sink = 0;
  clock_t t0 = clock();
  for (int i = 0; i < N; i++) {
    uint32_t pid = xr(); uint8_t ivs[6];
    for (int s = 0; s < 6; s++) ivs[s] = (uint8_t)(xr() & 31);
    PkPidiv r; pk_pidiv_search(pid, ivs, 0, &r);
    sink += r.ncand;
  }
  double ms1 = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / N;
  t0 = clock();
  for (int i = 0; i < N; i++) {
    uint32_t pid = xr(); uint8_t ivs[6];
    for (int s = 0; s < 6; s++) ivs[s] = (uint8_t)(xr() & 31);
    PkPidiv r; pk_pidiv_search(pid, ivs, PK_PIDIV_OPT_REVERSED, &r);
    sink += r.ncand;
  }
  double ms2 = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / N;
  printf("(3) host: %.3f ms/search (1 pass), %.3f ms (2 passes, Unown). sink=%u\n", ms1, ms2, sink);
  /* Host milliseconds say nothing about a 16.78 MHz ARM7 — they only prove the
   * search is O(65536) and not something worse. The GBA number comes from
   * counting the instructions the ARM compiler emits for the loop; see the
   * report accompanying this module. */
  printf("    iterations are fixed: 65536 per pass (131072 for Unown), data-independent.\n");
  CHECK(ms1 < 50.0, "one pass is sane on the host (guards against an accidental 2^32 sweep)");
}

/* ---- (4) real saves --------------------------------------------------------- */

/* A Method-3 probe (burn between the two PID calls), implemented HERE and not in
 * the shipped module: the plan scopes B1 to methods 1/2/4, and the point of this
 * counter is to answer "would adding Method 3 recover many of the no-match mons?"
 * with a number instead of a guess. */
static int method3_matches(uint32_t pid, const uint8_t ivs[6]) {
  const uint32_t A = 1103515245u, C = 24691u;
  uint16_t first = (uint16_t)(pid & 0xFFFFu), second = (uint16_t)(pid >> 16);
  uint16_t iv1 = (uint16_t)((ivs[PK_HP] & 31) | ((ivs[PK_ATK] & 31) << 5) | ((ivs[PK_DEF] & 31) << 10));
  uint16_t iv2 = (uint16_t)((ivs[PK_SPE] & 31) | ((ivs[PK_SPA] & 31) << 5) | ((ivs[PK_SPD] & 31) << 10));
  for (int order = 0; order < 2; order++) {
    uint32_t base = (uint32_t)(order ? second : first) << 16;
    uint32_t want = (uint32_t)(order ? first : second);
    for (uint32_t lo = 0; lo < 0x10000u; lo++) {
      uint32_t x1 = base | lo;
      uint32_t x3 = A * (A * x1 + C) + C;          /* r2 burned */
      if ((x3 >> 16) != want) continue;
      uint32_t x4 = A * x3 + C, x5 = A * x4 + C;
      if (((x4 >> 16) & 0x7FFF) == iv1 && ((x5 >> 16) & 0x7FFF) == iv2) return 1;
    }
  }
  return 0;
}

typedef struct { int n, ex[8], meth[PK_PIDIV_NMETHOD], nomatch, m3, ambiguous, trade, roam,
                 eggs_searched; } Tally;

static void tally_mon(PkMon* m, Tally* t) {
  t->n++;
  PkPidiv r;
  pk_pidiv_check(m, &r);
  if (r.exempt) { t->ex[r.exempt]++; return; }
  /* An egg that reaches the search is a leak of the exemption, and a PIDIV verdict on
   * an egg is meaningless (Emerald's egg PID mixes two RNG streams). Counted so the
   * real-data section can assert on it rather than merely printing a tally. */
  if (m->isEgg) t->eggs_searched++;
  t->meth[r.method]++;
  if (r.nmatch > 1) t->ambiguous++;
  if (r.method == PK_PIDIV_NONE) {
    t->nomatch++;
    if (method3_matches(m->personality, m->ivs)) t->m3++;
    /* 0xFE is METLOC_IN_GAME_TRADE: the B3 exemption this module cannot apply
     * itself (the trade table lives in the encounter module). Counted separately
     * so the "unexplained" number is the one that matters. */
    if (m->metLocation == 0xFE) t->trade++;
    /* The other known-legit way to fail: the RSE roaming Latias/Latios, whose
     * save slot keeps only 8 IV bits (B5). Diagnostic only — the encounter module
     * decides who is a roamer; here it just explains a no-match. */
    PkPidiv rm; pk_pidiv_search(m->personality, m->ivs, PK_PIDIV_OPT_ROAMER, &rm);
    int isroam = (rm.method != PK_PIDIV_NONE);
    if (isroam) t->roam++;
    if (t->nomatch <= 12)
      printf("      no match: %-11s pid=%08X ivs=%2d/%2d/%2d/%2d/%2d/%2d game=%d loc=%3d metlvl=%3d%s%s\n",
             pk_species_name(m->species), m->personality,
             m->ivs[0], m->ivs[1], m->ivs[2], m->ivs[3], m->ivs[4], m->ivs[5],
             m->metGame, m->metLocation, m->metLevel,
             m->metLocation == 0xFE ? "  [in-game trade]" : "",
             isroam ? "  [fits the roamer 8-bit compare]" : "");
  }
}

static void run_save(const char* path) {
  static uint8_t save[G3_SAVE_FILE_SIZE];
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  (cannot open %s)\n", path); return; }
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("  %s: parse FAILED\n", path); g_fail++; return; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES], pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);

  printf("  %s\n", path);
  Tally t; memset(&t, 0, sizeof t);
  PkMon party[6]; bool frlg = false;
  int np = pk_read_party_auto(sb1, party, &frlg);
  for (int i = 0; i < np; i++) { pk_resolve(&party[i]); tally_mon(&party[i], &t); }
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    PkMon box[30]; pk_read_box(pc, b, box);
    for (int s = 0; s < 30; s++)
      if (box[s].species >= 1 && box[s].species <= 411) { pk_resolve(&box[s]); tally_mon(&box[s], &t); }
  }
  int checked = t.n; for (int i = 1; i < 8; i++) checked -= t.ex[i];
  printf("    %d mons, %d searched -> M1 %d  M2 %d  M4 %d  Unown-order %d  |  NO MATCH %d"
         " (%d in-game trades, %d fit the roamer compare, %d would fit Method 3), ambiguous %d\n",
         t.n, checked,
         t.meth[PK_PIDIV_M1], t.meth[PK_PIDIV_M2], t.meth[PK_PIDIV_M4],
         t.meth[PK_PIDIV_M1_REV] + t.meth[PK_PIDIV_M2_REV] + t.meth[PK_PIDIV_M4_REV],
         t.nomatch, t.trade, t.roam, t.m3, t.ambiguous);
  printf("    exempt: egg %d, hatched %d, event %d, Colo/XD %d, origin %d, bad-egg %d\n",
         t.ex[PK_PIDIV_EX_EGG], t.ex[PK_PIDIV_EX_HATCHED], t.ex[PK_PIDIV_EX_FATEFUL],
         t.ex[PK_PIDIV_EX_GAMECUBE], t.ex[PK_PIDIV_EX_ORIGIN], t.ex[PK_PIDIV_EX_BADEGG]);
  /* The DISTRIBUTION is calibration input, not a gate — a legit save legitimately
   * contains events, trades and GC imports. But three properties of the MACHINERY
   * must hold on real data, and asserting nothing at all let this whole section run
   * while proving nothing (caught in review):
   *   1. it actually ran (a silent early return would print all-zeros and "pass");
   *   2. the egg exemption holds on real records, not just synthetic ones;
   *   3. the search is deterministic — an answer that moves between runs cannot
   *      support any verdict. */
  CHECK(t.n == 0 || checked > 0, "mons present but nothing was searched");
  CHECK(t.eggs_searched == 0, "an egg reached the PIDIV search — the exemption leaked");
  {
    Tally again; memset(&again, 0, sizeof again);
    for (int i = 0; i < np; i++) tally_mon(&party[i], &again);
    for (int b = 0; b < G3_TOTAL_BOXES; b++) {
      PkMon box[30]; pk_read_box(pc, b, box);
      for (int s2 = 0; s2 < 30; s2++)
        if (box[s2].species >= 1 && box[s2].species <= 411) { pk_resolve(&box[s2]); tally_mon(&box[s2], &again); }
    }
    CHECK(memcmp(&again.meth, &t.meth, sizeof t.meth) == 0 && again.nomatch == t.nomatch,
          "the PIDIV search is not deterministic across runs");
  }
}

/* ---- (5) the exemptions really are enforced -------------------------------- */
static void test_exemptions(void) {
  printf("(5) exemptions\n");
  PkMon m; memset(&m, 0, sizeof m);
  uint8_t ivs[6]; uint32_t pid;
  gen_forward(0xDEADBEEFu, 0, 0, &pid, ivs);
  m.species = 25; m.personality = pid; memcpy(m.ivs, ivs, 6);
  m.metLevel = 5; m.metGame = 3;

  PkPidiv r; pk_pidiv_check(&m, &r);
  printf("  baseline non-egg: exempt=%s method=%s\n",
         pk_pidiv_exempt_name(r.exempt), pk_pidiv_method_name(r.method));
  CHECK(r.exempt == PK_PIDIV_EX_NONE && r.method == PK_PIDIV_M1, "a plain wild mon is searched and matches");

  /* An egg carries a PERFECTLY VALID Method-1 spread here, so if the exemption
   * were missing this would report a match — the test proves the gate fires
   * before the search, not that the search happens to fail. */
  PkMon e = m; e.isEgg = true;
  pk_pidiv_check(&e, &r);
  printf("  same spread, isEgg: exempt=%s method=%s\n",
         pk_pidiv_exempt_name(r.exempt), pk_pidiv_method_name(r.method));
  CHECK(r.exempt == PK_PIDIV_EX_EGG && r.method == PK_PIDIV_NONE, "an egg is never searched");

  PkMon h = m; h.metLevel = 0;
  pk_pidiv_check(&h, &r);
  CHECK(r.exempt == PK_PIDIV_EX_HATCHED && r.method == PK_PIDIV_NONE, "a hatched mon is never searched");

  PkMon g = m; g.metGame = 15;
  pk_pidiv_check(&g, &r);
  CHECK(r.exempt == PK_PIDIV_EX_GAMECUBE, "a Colosseum/XD mon is never searched");

  PkMon fa = m; fa.ribbons = 0x80000000u;
  pk_pidiv_check(&fa, &r);
  CHECK(r.exempt == PK_PIDIV_EX_FATEFUL, "an event (fateful) mon is never searched");

  PkMon b = m; b.isBadEgg = true;
  pk_pidiv_check(&b, &r);
  CHECK(r.exempt == PK_PIDIV_EX_BADEGG, "a bad egg is never searched");

  PkMon o = m; o.metGame = 7;
  pk_pidiv_check(&o, &r);
  CHECK(r.exempt == PK_PIDIV_EX_ORIGIN, "an impossible origin game is never searched");

  /* Unown gets the second (reversed-order) pass; nothing else pays for it. */
  uint32_t upid; uint8_t uivs[6];
  gen_forward(0x0BADF00Du, 0, 1, &upid, uivs);       /* reversed-order spread */
  PkMon u = m; u.species = 201; u.personality = upid; memcpy(u.ivs, uivs, 6);
  pk_pidiv_check(&u, &r);
  printf("  Unown reversed-order spread: %s\n", pk_pidiv_method_name(r.method));
  CHECK(r.method == PK_PIDIV_M1_REV, "Unown's reversed PID order is searched");
  PkMon nu = u; nu.species = 202;                     /* not Unown -> no 2nd pass */
  pk_pidiv_check(&nu, &r);
  CHECK(r.method == PK_PIDIV_NONE, "the reversed order is NOT tried for other species");
}

int main(int argc, char** argv) {
  test_roundtrip();
  test_broken_input();
  test_false_positives();
  test_timing();
  test_exemptions();
  printf("(4) real cartridge saves (report only, no pass/fail)\n");
  /* run_host_tests.py hands the whole 5-cart corpus to any test that mentions
   * argv[1], so take every path it is given, not just the first. */
  const char** saves = (const char**)&argv[1];
  int nsaves = argc - 1;
  for (int i = 0; i < nsaves; i++) run_save(saves[i]);
  if (nsaves == 0) printf("  (no .sav given)\n");
  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
