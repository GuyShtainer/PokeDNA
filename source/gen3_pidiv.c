/* PIDIV reverse search. See gen3_pidiv.h for the mechanic, the methods and why an
 * egg must never reach this code. */
#include "gen3_pidiv.h"

/* pokeemerald include/random.h:16 — 1103515245 / 24691, the numbers every RNG tool
 * quotes as 0x41C64E6D / 0x6073. */
#define LCRNG_A     0x41C64E6Du
#define LCRNG_C     0x6073u
/* prev(x) = AINV*x + CINV, with AINV = A^-1 mod 2^32 and CINV = -AINV*C mod 2^32.
 * A is odd, so the step is a bijection and every state has exactly one pre-image.
 * Used ONLY to report the seed as the state before the first call (the convention
 * RNG Reporter and PKHeX print), never inside the loop. */
#define LCRNG_AINV  0xEEB9EB65u
#define LCRNG_CINV  0x0A3561A1u

/* Bit 15 of each 16-bit output is DISCARDED: CreateBoxMon takes only three 5-bit
 * fields out of it (pokeemerald src/pokemon.c:2277-2293). Comparing the full 16
 * bits would reject half of every method's legitimate seeds. */
#define IV_MASK     0x7FFFu

/* Method ids by [reversed][pattern]; the reversed row is the same three patterns
 * with the PID halves taken out of the stream in the opposite order. */
static const uint8_t k_method[2][3] = {
  { PK_PIDIV_M1,     PK_PIDIV_M2,     PK_PIDIV_M4     },
  { PK_PIDIV_M1_REV, PK_PIDIV_M2_REV, PK_PIDIV_M4_REV },
};

static void note(PkPidiv* r, uint8_t method, uint32_t seed) {
  r->methods |= (uint8_t)(1u << (method - 1));
  if (r->nmatch < 255) r->nmatch++;
  /* Report the lowest-numbered method. When two patterns fit the same record the
   * record itself cannot tell them apart (the burned call leaves no trace), so
   * `methods` carries the full set and the UI shows the first. */
  if (r->method == PK_PIDIV_NONE || method < r->method) {
    r->method = method;
    r->seed   = seed;
  }
}

/* How many candidates the PID stage can EVER return, proved rather than guessed.
 * The candidate states are (first<<16)|lo for lo in 0..65535, and the LCRNG maps
 * them to K + A*lo where K = A*(first<<16)+C. So the survivors are exactly the
 * points of the fixed set { A*lo mod 2^32 : lo in 0..65535 } that land inside one
 * 65,536-wide window. Sweeping every window over that point set gives a maximum of
 * THREE (an exhaustive computation, not an estimate); 4 leaves a slot of margin and
 * makes the index a cheap power-of-two mask. host_pidiv_test.c watches the observed
 * maximum, so a wrong proof shows up as a test failure rather than as corruption. */
#define PIDIV_MAX_CAND 4

/* THE HOT LOOP — THE ONLY PLACE IN POKEDNA WHERE THE CONSTANT FACTOR MATTERS. It
 * runs 65,536 times per pass on a 16.78 MHz ARM7 with no cache, so it is kept in
 * its own function containing NOTHING but the filter. That is not stylistic: with
 * the survivor walk written inline, gcc -O2 strength-reduces the third, fourth and
 * fifth LCRNG states into induction variables and updates all of them EVERY
 * iteration, turning a 5-instruction loop into a ~30-instruction one. Splitting
 * the phases takes the choice away from the optimiser.
 *
 *   - No multiply per iteration: consecutive candidates differ by 1, the LCRNG is
 *     affine, so the second state advances by exactly LCRNG_A. (A 32x32 multiply
 *     is 1S+mI on ARM7TDMI with m up to 4 for this multiplier; the add is 1S.)
 *   - `(x2 >> 16) == want` is one ARM instruction — CMP with a shifted operand —
 *     which is why it is written that way rather than as a mask-and-compare.
 *   - It counts DOWN to zero and hands back the STATE, not the candidate's low
 *     half. Both save a register: the decrement sets the flags the loop branch
 *     needs (no separate CMP), and nothing but `x2` and the counter has to stay
 *     live, which is what keeps the multiplier in a register instead of being
 *     re-loaded from the literal pool every iteration in Thumb. The caller steps
 *     back to x1 with one multiply, on the ~1 survivor.
 *   - The store index is MASKED rather than bounds-checked, so the rare path costs
 *     no compare in the loop and a wrong proof above would overwrite a slot
 *     instead of the stack.
 *
 * Everything is uint32_t, so the wrap at 2^32 is the modulus, not undefined
 * behaviour. Returns how many candidates survived (clamped to the array). */
static int pidiv_candidates(uint16_t first, uint16_t second, uint32_t out[PIDIV_MAX_CAND]) {
  const uint32_t want = (uint32_t)second;
  uint32_t x2 = LCRNG_A * ((uint32_t)first << 16) + LCRNG_C;   /* candidate lo = 0 */
  int n = 0;
  for (uint32_t i = 0x10000u; i != 0; i--, x2 += LCRNG_A)
    if ((x2 >> 16) == want) out[n++ & (PIDIV_MAX_CAND - 1)] = x2;
  return n < PIDIV_MAX_CAND ? n : PIDIV_MAX_CAND;
}

/* One pass: collect the candidates, then walk each of the ~1 survivors three steps
 * further and test the IV patterns. `first`/`second` are the 16-bit values the
 * first and second RNG calls must produce — which PID half is which is the
 * caller's choice, and that choice is what "reversed" means. */
static void pidiv_pass(uint16_t first, uint16_t second, uint16_t iv1, uint16_t iv2,
                       int reversed, int roamer, PkPidiv* r) {
  uint32_t cand[PIDIV_MAX_CAND];
  int n = pidiv_candidates(first, second, cand);
  const uint8_t* k = k_method[reversed ? 1 : 0];

  for (int i = 0; i < n; i++) {
    if (r->ncand < 255) r->ncand++;
    uint32_t x2 = cand[i];
    uint32_t x1 = LCRNG_AINV * x2 + LCRNG_CINV;      /* one step back to the PID call */
    uint32_t x3 = LCRNG_A * x2 + LCRNG_C;
    uint32_t x4 = LCRNG_A * x3 + LCRNG_C;
    uint32_t x5 = LCRNG_A * x4 + LCRNG_C;
    uint32_t r3 = (x3 >> 16) & IV_MASK;
    uint32_t r4 = (x4 >> 16) & IV_MASK;
    uint32_t r5 = (x5 >> 16) & IV_MASK;
    /* The seed as printed elsewhere: the state BEFORE the first PID call. */
    uint32_t seed = LCRNG_AINV * x1 + LCRNG_CINV;

    if (roamer) {
      /* Only 8 bits of the first IV word survive in the roamer slot, and the
       * roamer is a plain Method-1 creation, so there is exactly one pattern to
       * test and the second IV word carries no information at all. */
      if ((r3 & 0xFFu) == (iv1 & 0xFFu)) note(r, k[0], seed);
      continue;
    }
    if (r3 == iv1 && r4 == iv2) note(r, k[0], seed);   /* Method 1 */
    if (r4 == iv1 && r5 == iv2) note(r, k[1], seed);   /* Method 2: r3 burned */
    if (r3 == iv1 && r5 == iv2) note(r, k[2], seed);   /* Method 4: r4 burned */
  }
}

void pk_pidiv_search(uint32_t pid, const uint8_t ivs[PK_NSTATS], uint32_t opts,
                     PkPidiv* out) {
  if (!out) return;
  out->method = PK_PIDIV_NONE; out->exempt = PK_PIDIV_EX_NONE;
  out->methods = 0; out->nmatch = 0; out->ncand = 0; out->seed = 0;
  if (!ivs) return;

  /* The two words exactly as CreateBoxMon splits them (pokemon.c:2277-2293).
   * PkMon's ivs[] is already in the save-native HP/Atk/Def/Spe/SpA/SpD order, so
   * the first three fields are word 1 and the last three are word 2. */
  uint16_t iv1 = (uint16_t)((ivs[PK_HP]  & 31) | ((ivs[PK_ATK] & 31) << 5) | ((ivs[PK_DEF] & 31) << 10));
  uint16_t iv2 = (uint16_t)((ivs[PK_SPE] & 31) | ((ivs[PK_SPA] & 31) << 5) | ((ivs[PK_SPD] & 31) << 10));

  uint16_t lo = (uint16_t)(pid & 0xFFFFu);
  uint16_t hi = (uint16_t)(pid >> 16);
  int roamer = (opts & PK_PIDIV_OPT_ROAMER) ? 1 : 0;

  pidiv_pass(lo, hi, iv1, iv2, 0, roamer, out);
  /* The reversed pass is a different 65,536-candidate range (it starts from the
   * OTHER PID half), so it costs a second full pass — except when the halves are
   * equal, where it would be the same search and would double-count every hit. */
  if ((opts & PK_PIDIV_OPT_REVERSED) && lo != hi)
    pidiv_pass(hi, lo, iv1, iv2, 1, roamer, out);
}

uint8_t pk_pidiv_exempt_reason(const PkMon* m) {
  if (!m) return PK_PIDIV_EX_BADEGG;
  if (m->isBadEgg)   return PK_PIDIV_EX_BADEGG;
  if (m->isEgg)      return PK_PIDIV_EX_EGG;
  /* metLevel 0 is how Gen 3 records "this hatched from an egg" — every wild,
   * static, gift and traded mon stores the level it was met at instead. So this
   * is the exemption that catches a mon which is no longer an egg but was one. */
  if (m->metLevel == 0) return PK_PIDIV_EX_HATCHED;
  /* Bit 31 of the ribbons word is the obedience/fateful flag, set by the event
   * distributions (Mew, Deoxys, ...). Those use distribution-specific generation
   * that this search does not model, so they are evidence-free by construction. */
  if (m->ribbons >> 31) return PK_PIDIV_EX_FATEFUL;
  if (m->metGame == 15) return PK_PIDIV_EX_GAMECUBE;
  if (m->metGame < 1 || m->metGame > 5) return PK_PIDIV_EX_ORIGIN;
  return PK_PIDIV_EX_NONE;
}

void pk_pidiv_check(const PkMon* m, PkPidiv* out) {
  if (!out) return;
  out->method = PK_PIDIV_NONE; out->methods = 0;
  out->nmatch = 0; out->ncand = 0; out->seed = 0;
  out->exempt = pk_pidiv_exempt_reason(m);
  if (out->exempt != PK_PIDIV_EX_NONE) return;
  /* Unown is the one species that assembles the PID halves the other way round
   * (FRLG's Tanoby chambers), so it — and only it — pays for the second pass.
   * Not a guess: over Guy's five saves the reversed order matched 23 mons and all
   * 23 were Unown, while no Unown matched the normal order (host_pidiv_test.c). */
  uint32_t opts = (m->species == 201) ? PK_PIDIV_OPT_REVERSED : 0u;
  pk_pidiv_search(m->personality, m->ivs, opts, out);
}

const char* pk_pidiv_method_name(uint8_t method) {
  switch (method) {
    case PK_PIDIV_NONE:   return "no method";
    case PK_PIDIV_M1:     return "Method 1";
    case PK_PIDIV_M2:     return "Method 2";
    case PK_PIDIV_M4:     return "Method 4";
    case PK_PIDIV_M1_REV: return "Method 1 (Unown)";
    case PK_PIDIV_M2_REV: return "Method 2 (Unown)";
    case PK_PIDIV_M4_REV: return "Method 4 (Unown)";
    default:              return "?";
  }
}

const char* pk_pidiv_exempt_name(uint8_t exempt) {
  switch (exempt) {
    case PK_PIDIV_EX_NONE:     return "checked";
    case PK_PIDIV_EX_BADEGG:   return "bad egg";
    case PK_PIDIV_EX_EGG:      return "egg";
    case PK_PIDIV_EX_HATCHED:  return "hatched";
    case PK_PIDIV_EX_FATEFUL:  return "event mon";
    case PK_PIDIV_EX_GAMECUBE: return "Colosseum/XD";
    case PK_PIDIV_EX_ORIGIN:   return "unknown origin";
    case PK_PIDIV_EX_TRADE:    return "in-game trade";
    default:                   return "?";
  }
}
