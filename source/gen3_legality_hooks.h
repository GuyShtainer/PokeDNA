#ifndef GEN3_LEGALITY_HOOKS_H
#define GEN3_LEGALITY_HOOKS_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_legality2.h"

/* Legality V2 — the four TABLE-DRIVEN check families, wired into the catalogue.
 *
 * gen3_legality2.c ships every check that needs no data beyond the tables PokeDNA
 * already had, and leaves four WEAK no-op hooks for the families that need new
 * data (gen3_legality2.h:119-138). This translation unit defines those four
 * symbols STRONGLY, so simply adding it to the link enables:
 *
 *   pk2_hook_moves      docs/research-legality-v2.md §3 C2-C5, on source/learnsets2.h
 *   pk2_hook_encounter  §3 E2 + the flat "wild anywhere" gate, on source/encounters.h
 *                       AND source/statics.h — the script-placed species the wild
 *                       tables do not describe, whose absence produced this checker's
 *                       one confirmed false-accusation class (the Devon-Scope Kecleon)
 *   pk2_hook_evolution  research-legal-generator.md §3 T_evo, on source/evolutions.h
 *   pk2_hook_pidiv      §3 B1/B2, on source/gen3_pidiv.h (on demand only)
 *
 * There is no registration call and no RAM: the linker picks the strong symbol.
 * Nothing else in the program needs to know this file exists — which is why the
 * UI can keep calling pk_check_legality2() and simply gets deeper answers.
 *
 * Pure C (hard convention 5): <stdint.h>/<stdbool.h> plus the pure-C cores it sits
 * on. tests/host_legality_hooks_test.c compiles and runs the whole thing on the PC.
 *
 * ---- severity policy, restated because this file is where it is easiest to get
 * ---- wrong ------------------------------------------------------------------
 * ONE check here can say INVALID: the level-window rule (C2), and only after five
 * exemptions are ruled out (bred / event / in-game trade / Colosseum-XD / Smeargle).
 * Everything else is SUSPECT or INFO. A missing table produces NO row at all — the
 * tri-state in encounters.h and lg2_have_data() exist precisely so that "we have no
 * data" can never turn into a verdict (OVERNIGHT-DECISIONS.md §2).
 *
 * COST, measured with the project's own flags (arm-none-eabi-gcc -mthumb -O2):
 * .text 2520 B + .rodata 764 B (the 184-entry pre-evolution table, 736 B of it) +
 * 241 B of strings = ~3.5 KiB of ROM. .data and .bss are BOTH ZERO — no statics, no
 * allocation, so nothing lands in the ~1.5 KiB of EWRAM the build has left. Deepest
 * stack path is pk2_hook_moves -> pk2_line_sources, 152 B plus its callee's frame
 * (-fstack-usage), which the 32 KiB IWRAM stack absorbs without comment.
 *
 * SPEED: the two sweep hooks add ~0.9 us per mon on the host benchmark, against
 * 0.05 us for the table-free core — i.e. a 30-mon box sweep grows by a few
 * milliseconds on hardware, not by a frame. The PIDIV hook is the expensive one
 * (65,536 LCRNG steps, 55-180 ms on the ARM7 per gen3_pidiv.h) and is why the core
 * only calls it under PK2_RUN_PIDIV. */

/* ---- the pre-evolution chain ------------------------------------------------
 * Both hooks need it and neither of the two data modules owns it (learnsets2.h:41
 * says the caller walks the chain itself; encounters.h has no T_preevo). Gen-3
 * mons keep the moves they learned as a pre-evolution AND keep the met location of
 * the pre-evolution they were caught as, so a checker that ignores evolution
 * false-flags a Charizard for knowing a move Charmander learns at L43 — measured:
 * 554 (species, move) pairs in the real tables have a lower minimum somewhere up
 * the chain than on the species itself.
 *
 * `pk2_preevo` returns the IMMEDIATE pre-evolution's internal species id, or 0 for
 * a base form / unknown id. Two Gen-3 species share one pre-evolution in three
 * places (Nincada -> Ninjask + Shedinja, Wurmple -> Silcoon + Cascoon, Tyrogue ->
 * Hitmonlee + Hitmonchan); this direction of the map is still a function. */
uint16_t pk2_preevo(uint16_t species);

/* Minimum level at which `species` OR ANY PRE-EVOLUTION learns `move` by level-up,
 * taking the lowest over all three game groups (a mon may have visited any cart, and
 * the Move Reminder reteaches from the current game's list). -1 = never by level-up.
 * This is the number behind the C2 rule and the one the UI should show. */
int pk2_line_min_levelup(uint16_t species, uint16_t move);

/* Union of LG2_SRC_* over the whole pre-evolution chain and all three game groups:
 * every way `move` could have reached `species`. 0 = no known source at all. Level
 * windows are deliberately ignored here — lg2_move_sources' `level` gate would make
 * the answer depend on the mon, and the C2 rule wants the level separately. */
uint8_t pk2_line_sources(uint16_t species, uint16_t move);

/* Is `species` (or a pre-evolution) wild-obtainable in game `g` — and, with a
 * mapsec, at that place? Returns the encounters.h tri-state (PK_WILD_NO_DATA /
 * PK_WILD_NO / PK_WILD_YES) so a caller cannot accidentally read "no table" as
 * "not obtainable". On PK_WILD_YES, `lo` and `hi` (either may be NULL) receive the
 * WIDEST level window over the matching rows of the chain. */
int pk2_line_wild_at(uint8_t game, uint8_t mapsec, uint16_t species,
                     uint8_t* lo, uint8_t* hi);
int pk2_line_wild_anywhere(uint8_t game, uint16_t species);

/* Does `species` (or a pre-evolution) have a SCRIPT placement in game `g` — a
 * setwildbattle static or a givemon gift (source/statics.h)? Returns the statics.h
 * tri-state (PK_STATIC_NO_DATA / PK_STATIC_NO / PK_STATIC_YES).
 *
 * This is the question that decides whether the encounter hook is allowed to speak at
 * all. A wild table describes grass, water, rocks and fishing; it does not describe the
 * Kecleon you reveal with the Devon Scope, and comparing that Kecleon's L30 against the
 * route's L25-25 wild row is how a legitimately-caught Pokemon got called suspect. YES
 * here means the wild table is not a complete account of this species, so no verdict
 * derived from it is safe to publish. */
int pk2_line_scripted(uint8_t game, uint16_t species);

/* Whether each family has enough real data to be allowed to speak. The hooks call
 * these first and mark themselves ABSENT in Pk2Report.hooks_absent when they are
 * false, so the UI says "not checked" instead of showing a clean bill of health for
 * a check that never ran. */
bool pk2_moves_data_ok(void);       /* learnsets2.c generated AND all four source
                                     * families backed by real data somewhere      */
bool pk2_encounter_data_ok(void);   /* encounters.c AND statics.c generated — both
                                     * halves, because the wild table alone cannot
                                     * account for a script-placed Pokemon           */
bool pk2_evolution_data_ok(void);   /* evolutions.c generated (forward index +
                                     * per-species floor + the wild relaxation)     */

/* Lowest level `species` itself can stand at (PK_EVO_NO_DATA if no table is linked).
 *
 * Deliberately NOT a pk2_line_* helper, and that is the whole point: every other
 * helper in this file takes a minimum over the pre-evolution chain, because a mon
 * keeps its pre-evolution's moves and met location. Evolution level is the one fact
 * that does NOT travel down the chain — Charmander being catchable at L5 says nothing
 * about Charizard. Taking a chain minimum here would silently disable the rule for
 * every species whose base form is a low-level wild encounter, i.e. for almost all of
 * them. This wrapper exists so that trap is written down where the next reader
 * expecting a pk2_line_* will hit it. */
int pk2_evo_floor(uint16_t species);

/* Pk2Report.pidiv_method uses the enum documented in gen3_legality2.h:79-80
 * (1/2/4 = Method 1/2/4, 5 = a reversed-order match, 0 = none). gen3_pidiv.h has a
 * finer one (6 ids). This maps the latter to the former; the report row still spells
 * the full name out, so nothing is lost on screen. */
uint8_t pk2_pidiv_report_method(uint8_t pidiv_method);

#endif /* GEN3_LEGALITY_HOOKS_H */
