#ifndef GEN3_PIDIV_H
#define GEN3_PIDIV_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"   /* PkMon, PK_HP..PK_SPD */

/*
 * PIDIV — the PID/IV RNG-correlation test (Legality V2 check B1/B2).
 *
 * ---- what it proves ----------------------------------------------------------
 * Every wild/static/gift Pokemon in Gen 3 has its personality value AND its twelve
 * IVs drawn from ONE stream of a single 32-bit LCRNG:
 *
 *     gRngValue = 1103515245 * gRngValue + 24691;   Random() = gRngValue >> 16
 *     (pokeemerald include/random.h:16, src/random.c:11-16 — 0x41C64E6D / 0x6073)
 *
 * Four consecutive 16-bit outputs become the two PID halves and the two IV words
 * (pokeemerald src/pokemon.c:2277-2293 for the IV halves). So a legitimate mon's
 * PID and IVs are not independent: given the PID there are only ~1 seeds in the
 * whole 2^32 space that could have produced it, and those seeds pin the IVs
 * exactly. An editor that sets "31/31/31/31/31/31" on an arbitrary PID almost
 * always breaks that correlation, and this module detects it.
 *
 * ---- why it is cheap enough for a 16.78 MHz ARM7 -----------------------------
 * The reputation of PIDIV as a "PC-only" check comes from imagining a 2^32 seed
 * sweep. It is 2^16: the state that emitted the first PID half has that half AS
 * ITS OWN TOP 16 BITS, so only its low 16 bits are unknown. Enumerate those,
 * keep the ~1 candidate whose next output is the other PID half, and walk three
 * more steps to test the IV patterns. See gen3_pidiv.c for the shape of the loop.
 *
 * ---- the methods -------------------------------------------------------------
 * A VBlank interrupt landing between the generator's calls consumes an extra RNG
 * output, which is why more than one call pattern occurs on retail hardware:
 *
 *     Method 1:  r1,r2 = PID   r3 = IVword1   r4 = IVword2
 *     Method 2:  r1,r2 = PID   (r3 burned)    r4 = IVword1   r5 = IVword2
 *     Method 4:  r1,r2 = PID   r3 = IVword1   (r4 burned)    r5 = IVword2
 *     IVword1 = HP | Atk<<5 | Def<<10   IVword2 = Spe | SpA<<5 | SpD<<10
 *
 * ...and each of those exists in two PID-assembly orders, because Random32() is
 * `(Random() | (Random() << 16))` (pokeemerald include/random.h:12) and some
 * generators build the halves the other way round. MEASURED on Guy's own five
 * cartridge saves (488 non-exempt Pokemon, tests/host_pidiv_test.c): the
 * LOW-half-first order accounts for every single mon EXCEPT the 23 Unown, and
 * every one of those 23 — across three different carts, all originating in
 * FireRed at the Tanoby Chambers — matches only the HIGH-half-first order. That
 * is why the reversed pass is spent on species 201 and nothing else.
 *
 * The same run confirms all three gap patterns really do occur on retail, in
 * proportions that differ per cart (Emerald 53/96/15 for M1/M2/M4, Ruby
 * 66/51/54, FireRed 40/18/51): implementing only Method 1 would have called
 * two thirds of a legitimate Emerald box "suspect".
 *
 * Method 3 (a burn BETWEEN the two PID calls) is deliberately not searched: it
 * needs a second accumulator in the hot loop and the research plan
 * (docs/research-legality-v2.md §5) scopes B1 to methods 1/2/4. Omitting it can
 * only turn a legit mon into SUSPECT, never into INVALID. (To add it later:
 * carry a second running value stepping by LCRNG_A*LCRNG_A per iteration and
 * compare its high half to the second PID half.)
 *
 * ---- the result is EVIDENCE, not a verdict -----------------------------------
 * A match is strong evidence of legitimacy. A NON-match is SUSPECT and must never
 * be rendered as INVALID (docs/OVERNIGHT-DECISIONS.md §2): event distributions,
 * Colosseum/XD mons and in-game trades legitimately fail it, and a hacked mon can
 * always copy a real spread anyway. This is a negative filter, exactly as PKHeX's
 * is.
 *
 * ---- eggs are EXEMPT, and that is not optional --------------------------------
 * An egg's personality is built from TWO DIFFERENT RNG streams —
 * `(Random2() << 16) | ((Random() % 0xfffe) + 1)` (pokeemerald src/daycare.c:466)
 * — and its IVs are partly inherited from the parents, so no single-stream
 * correlation exists and the search would report SUSPECT on every bred Pokemon in
 * the box. pk_pidiv_check() enforces this (and the other exemptions) itself; the
 * raw pk_pidiv_search() does not, and its callers must.
 *
 * Pure C (no tonc, no GBA headers) so tests/host_pidiv_test.c runs it on the PC.
 */

/* Which call pattern matched. The _REV entries are the same three patterns with
 * the two PID halves read out of the stream in the opposite order. */
enum {
  PK_PIDIV_NONE = 0,
  PK_PIDIV_M1,
  PK_PIDIV_M2,
  PK_PIDIV_M4,
  PK_PIDIV_M1_REV,
  PK_PIDIV_M2_REV,
  PK_PIDIV_M4_REV,
  PK_PIDIV_NMETHOD          /* 7 — one past the last method id */
};

/* Why the search did not run. 0 means it ran and `method` is meaningful. */
enum {
  PK_PIDIV_EX_NONE = 0,
  PK_PIDIV_EX_BADEGG,       /* record checksum failed: PID/IVs are garbage        */
  PK_PIDIV_EX_EGG,          /* unhatched egg — two RNG streams (daycare.c:466)    */
  PK_PIDIV_EX_HATCHED,      /* metLevel == 0, i.e. it WAS an egg: same reason     */
  PK_PIDIV_EX_FATEFUL,      /* obedience/fateful bit 31: event distribution        */
  PK_PIDIV_EX_GAMECUBE,     /* origin 15 (Colosseum/XD): a different, GC LCG      */
  PK_PIDIV_EX_ORIGIN,       /* origin game outside 1..5: not a retail GBA encounter*/
  PK_PIDIV_EX_TRADE         /* caller matched an in-game trade (B3): PID is fixed  */
};

typedef struct {
  uint8_t  method;    /* PK_PIDIV_* — lowest-numbered match, or PK_PIDIV_NONE     */
  uint8_t  exempt;    /* PK_PIDIV_EX_* — nonzero means the search never ran       */
  uint8_t  methods;   /* bitmask of ALL matches: bit (method-1)                   */
  uint8_t  nmatch;    /* (seed, method) pairs matched; saturates at 255           */
  uint8_t  ncand;     /* seeds that survived the PID stage (diagnostic; sat. 255) */
  uint32_t seed;      /* state BEFORE the first RNG call, for `method`            */
} PkPidiv;

/* Search options for pk_pidiv_search(). */
#define PK_PIDIV_OPT_REVERSED 0x1u   /* also try the high-half-first (Unown) order */
#define PK_PIDIV_OPT_ROAMER   0x2u   /* RSE roamer: 8-bit IV compare — see below   */

/* THE RAW SEARCH. `ivs` is in PkMon order (HP, Atk, Def, Spe, SpA, SpD).
 *
 * PRECONDITION the caller MUST honour: this is only meaningful for a non-egg,
 * never-been-an-egg, GBA-origin Pokemon. It happily "answers" for an egg, and the
 * answer is noise. Use pk_pidiv_check() unless you are a test or you have already
 * applied the exemptions yourself.
 *
 * PK_PIDIV_OPT_ROAMER: the RSE roaming Latias/Latios only preserve the low 8 bits
 * of the first IV word in the save (HP plus the low 3 bits of Attack; everything
 * else reads 0). The compare is therefore 8 bits instead of 30 and only the
 * Method-1 pattern is tested — a genuinely WEAK result that says little more than
 * "some seed makes this PID". Treat a roamer match as absence of evidence.
 * (Guy's Ruby holds both roamers, with IVs 12/7/0/0/0/0 and 7/0/0/0/0/0 — the
 * glitch's fingerprint. They are the only two legitimately-caught mons in the
 * whole corpus that fail the normal search, and both pass under this flag.) */
void pk_pidiv_search(uint32_t pid, const uint8_t ivs[PK_NSTATS], uint32_t opts,
                     PkPidiv* out);

/* THE SAFE ENTRY POINT: applies every exemption from the research plan before it
 * searches, so an egg or a hatched or a GameCube mon can never be tested by
 * accident. Enables the reversed order for Unown (species 201) only. `exempt`
 * says which rule fired; `method` is PK_PIDIV_NONE then.
 *
 * The one exemption it cannot see is B3 (in-game trade, whose PID is fixed at
 * conception): the trade table lives in the encounter module, so that caller
 * should skip the call and set PK_PIDIV_EX_TRADE itself. */
void pk_pidiv_check(const PkMon* m, PkPidiv* out);

/* The exemption that WOULD fire for this mon, without running any search
 * (PK_PIDIV_EX_NONE if the search would run). Lets the box-wide sweep decide
 * cheaply whether a mon is even worth the 65,536-iteration pass. */
uint8_t pk_pidiv_exempt_reason(const PkMon* m);

/* Fixed UI strings: "Method 1", "Method 4 (Unown)", "no method", "egg", ...
 * Never NULL — an out-of-range id returns "?". */
const char* pk_pidiv_method_name(uint8_t method);
const char* pk_pidiv_exempt_name(uint8_t exempt);

/* ============================ FORWARD GENERATION ============================
 * The inverse of the search above, and the only honest way to give a Pokemon PokeDNA
 * creates a PID and IVs at the same time.
 *
 * They are ONE object in Gen 3. CreateBoxMon draws the PID and then both IV words from a
 * single LCRNG stream (pokeemerald src/pokemon.c:2216, 2277-2293), so a hand-picked PID
 * next to hand-picked IVs is a pair no seed can produce and pk_pidiv_search says exactly
 * that. Rolling a SEED and taking whatever falls out makes the verify step an assertion
 * instead of a search that can fail: tests/host_spread_test.c re-finds every generated
 * spread at its own seed.
 *
 * METHOD 1 ONLY, deliberately. Methods 2 and 4 exist because a VBlank interrupt burned an
 * extra RNG call on real hardware; emitting one would be claiming an interrupt happened.
 * Method 3 is not even searched by this module, so it must never be emitted either.
 *
 * THIS IS THE ONE ROLLER. The summary's IV reroll (gen3_ivroll.c) drives the SAME
 * pk_spread_roll with progressively relaxed constraints rather than keeping its own copy
 * of the LCRNG walk — a generator and an auditor that disagreed about the constants would
 * be the worst possible bug in this file. */

#define PK_SPREAD_REVERSED   0x1u   /* assemble the PID high-half-first (Unown order) */
#define PK_SPREAD_ANY_FORM   0xFFu  /* PkSpreadWant.unown_form: don't care            */
#define PK_SPREAD_IV_MAXSUM  186u   /* 6 x 31 — the largest satisfiable min_iv_sum    */
#define PK_SPREAD_CAP        400000u

typedef struct {
  uint32_t seed;              /* state BEFORE the first call — what PkPidiv.seed prints */
  uint32_t next;              /* state to RESUME from: lets a caller chunk a long search
                               * across vblanks, and lets two presses never repeat        */
  uint32_t pid;
  uint8_t  ivs[PK_NSTATS];    /* HP, Atk, Def, Spe, SpA, SpD — the save-native order     */
  uint32_t tries;             /* seeds consumed (1 when nothing was constrained)         */
  uint8_t  ok;                /* 1 every constraint met; 0 the cap was hit               */
} PkSpread;

/* What the caller wants out of the roll. Use pk_spread_want_init(): "don't care" is -1 for
 * the three signed fields and PK_SPREAD_ANY_FORM for the letter, so a zeroed struct would
 * silently mean "nature HARDY, gender male, not shiny, Unown A". */
typedef struct {
  int8_t   nature;        /* 0..24, -1 don't care                                      */
  int8_t   gender;        /* 0 M, 1 F, 2 genderless, -1 don't care                     */
  int8_t   shiny;         /* 1 want, 0 refuse, -1 don't care                           */
  uint8_t  gender_ratio;  /* pk_species_gender_ratio(species); read only if gender >= 0*/
  uint8_t  unown_form;    /* 0..27, PK_SPREAD_ANY_FORM = don't care                    */
  uint8_t  min_iv_sum;    /* 0 don't care; else sum(ivs) >= this (clamped to 186)      */
  uint8_t  ability;       /* 0/1 required ability slot, 0xFF = don't care              */
  uint32_t otId;          /* the trainer the shiny test is against                     */
  uint32_t opts;          /* PK_SPREAD_REVERSED                                        */
  uint32_t cap;           /* seed budget; 0 -> PK_SPREAD_CAP                           */
} PkSpreadWant;

void pk_spread_want_init(PkSpreadWant* w);

/* One Method-1 roll: no search, no constraints, four multiplies. */
void pk_spread_from_seed(uint32_t seed, uint32_t opts, PkSpread* out);

/* One LCRNG step — so a caller can advance a seed without duplicating the constants. */
uint32_t pk_lcrng_next(uint32_t s);

/* Walk seeds from `seed0` (one LCRNG step per trial, i.e. the next "frame") until the
 * spread satisfies `w`, or the cap is reached. Measured trial counts: nothing 1 · nature
 * avg 22 worst 61 · Unown letter worst 122 · gender 2-8 · shiny ~8,800 · shiny+nature
 * ~200k (worst observed 864k) — so anything asking for BOTH shiny and a nature needs
 * chunking or a smaller cap.
 *
 * FAILS SOFT, and callers rely on it: on a cap miss this returns false, sets out->ok = 0,
 * still leaves a REAL Method-1 spread in *out (the first one it rolled), and sets
 * out->next so the caller can resume. A created Pokemon then has the wrong nature, never
 * a broken pair, and never a hang. */
bool pk_spread_roll(uint32_t seed0, const PkSpreadWant* w, PkSpread* out);

#endif /* GEN3_PIDIV_H */
