#ifndef EVOLUTIONS_H
#define EVOLUTIONS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * The FORWARD evolution index — "what does this species turn into, how, and at what
 * level" — and the minimum level each evolution stage can stand at.
 *
 * source/evolutions.c is GENERATED and git-ignored. `python3 tools/gen_evolutions.py
 * --from-rom` locates gEvolutionTable by shape in each of the five retail cartridge
 * dumps and emits it; this header is the committed API. Reading the carts rather than a
 * decomp is what makes the table shippable (plain numbers measured from hardware the
 * user owns) and is the same posture as source/encounters.h.
 *
 * ---- why this exists ---------------------------------------------------------
 * PokeDNA already ships the BACKWARD map (gen3_legality_hooks.c's s_preevo[184]),
 * which is all the move and encounter checks need. Nothing could answer the other
 * direction, and the cost was measured: a level-5 Charizard graded LEGAL with zero
 * findings, because no rule knew Charmeleon does not become Charizard until L36. Half
 * of what the create flow builds is an evolved form standing at level 5.
 *
 * The two derivations agree: the ROM table decodes to exactly the 184 links s_preevo
 * carries, with zero missing and zero disagreements, and the decoded table is
 * byte-identical across all five games — one table, not five.
 *
 * ---- the two floors, and why there are two ----------------------------------
 * pk_evo_min_level() is the EVOLUTION floor: walk the chain from the base form, raising
 * the floor at every level-gated method and leaving it alone at every stone / trade /
 * friendship / beauty one. That is the number a *builder* wants — "create a Charizard"
 * must mean at least L36 — and it is what Guy asked for in those words.
 *
 * pk_evo_floor() is the CHECKER floor, and it is deliberately lower, because the
 * evolution floor on its own is NOT a legality rule. The retail wild tables place
 * evolved forms below their own evolution levels: FireRed's Safari Zone has Poliwhirl
 * at L20 when Poliwag evolves at 25, and Sootopolis' Super Rod has Gyarados at L5 when
 * Magikarp evolves at 20. 23 species are catchable below their evolution floor across
 * the five carts. A checker that used pk_evo_min_level() would call every one of those
 * legitimately-caught Pokemon illegal — the one mistake this project forbids — so
 * pk_evo_floor() takes min(evolution floor, lowest wild level in any cart).
 *
 * Deliberately NOT modelled: friendship and beauty evolutions physically happen on the
 * level-up AFTER the condition is met, so Crobat's true floor is 23, not the 22 this
 * reports. Over-accepting by one level cannot call a legitimate Pokemon illegal;
 * under-accepting can, so the +1 is not applied.
 *
 * Pure C — no tonc, no GBA headers, no FatFs — so tests/host_evolutions_test.c runs it
 * against the real ROMs on the PC. All tables are `const` (ROM, read in place); this
 * module adds zero EWRAM (hard convention 2).
 */

/* Evolution methods: the games' own EVO_* ids (constants/pokemon.h). Kept as the raw
 * numbers so the emitted table is a transcription rather than an interpretation. */
#define PK_EVO_FRIENDSHIP        1   /* levels up with friendship >= 220            */
#define PK_EVO_FRIENDSHIP_DAY    2
#define PK_EVO_FRIENDSHIP_NIGHT  3
#define PK_EVO_LEVEL             4   /* reaches `param`                             */
#define PK_EVO_TRADE             5
#define PK_EVO_TRADE_ITEM        6
#define PK_EVO_ITEM              7   /* `param` is an item id (the stones)          */
#define PK_EVO_LEVEL_ATK_GT_DEF  8   /* the three Hitmon splits                     */
#define PK_EVO_LEVEL_ATK_EQ_DEF  9
#define PK_EVO_LEVEL_ATK_LT_DEF 10
#define PK_EVO_LEVEL_SILCOON    11   /* the Wurmple PID split                       */
#define PK_EVO_LEVEL_CASCOON    12
#define PK_EVO_LEVEL_NINJASK    13   /* Nincada -> Ninjask                          */
#define PK_EVO_LEVEL_SHEDINJA   14   /* ...and the spare body                       */
#define PK_EVO_BEAUTY           15   /* Feebas; `param` is the beauty threshold     */

typedef struct {
  uint16_t from;      /* Gen-3 INTERNAL species id, 1..411 — as PkMon.species        */
  uint16_t into;
  uint16_t param;     /* level / item id / beauty threshold, per `method`            */
  uint8_t  method;    /* PK_EVO_*                                                    */
  uint8_t  pad;
} PkEvoLink;          /* 8 bytes */

/* Tri-state, for the same reason encounters.h has one: "no table is linked in" and
 * "this species has no evolution constraint" must never collapse into one answer. */
#define PK_EVO_NO_DATA (-1)

/* False when source/evolutions.c was not generated. Every caller that could turn a
 * silence into a verdict MUST consult this first. */
bool pk_evo_have_data(void);

/* Total links in the table (184 on a full generation) and raw row access, for the
 * host tests and for any UI that wants to list a line. */
int pk_evo_count(void);
const PkEvoLink* pk_evo_row(int i);

/* THE FORWARD INDEX: how many ways `species` evolves, and where they start. Returns 0
 * for a final stage (and for every species when no table is linked in — check
 * pk_evo_have_data() if the difference matters). */
int pk_evo_list(uint16_t species, const PkEvoLink** out);

/* The EVOLUTION floor: lowest level a Pokemon of this species can have reached by
 * evolving, walking from the base form. 1 = nothing about its evolution stage
 * constrains it. PK_EVO_NO_DATA for an out-of-range species or an unlinked table.
 * This is the builder's number ("a Charizard must be at least L36"). */
int pk_evo_min_level(uint16_t species);

/* Lowest level this species appears at in any of the five carts' wild tables; 0 means
 * never wild. Exposed on its own because it is the surprising half of pk_evo_floor. */
int pk_evo_wild_min(uint16_t species);

/* The CHECKER floor: min(pk_evo_min_level, a non-zero pk_evo_wild_min). A mon below
 * THIS could not exist; a mon between this and pk_evo_min_level was caught, not
 * evolved. See the header comment for why the two differ. */
int pk_evo_floor(uint16_t species);

/* "LEVEL", "ITEM", "FRIENDSHIP", ... for the UI and the tests. Never NULL. */
const char* pk_evo_method_name(uint8_t method);

/* Does this method impose a level requirement? True for EVO_LEVEL and its five
 * conditional variants, false for stone / trade / friendship / beauty. This is the
 * single rule that makes the floor walk correct, so it is exported rather than
 * re-derived by every caller. */
bool pk_evo_method_is_level(uint8_t method);

#if defined(__GNUC__) && !defined(PK_EVOLUTIONS_IMPL)
/*
 * Weak no-data fallbacks, so a clone with no generated table still links and behaves
 * sanely — the same posture as encounters.h and source/art_fallbacks.c. The generated
 * evolutions.c defines PK_EVOLUTIONS_IMPL before including this header, so it never
 * collides with its own strong definitions.
 *
 * Every fallback answers "I do not know", never "no constraint": pk_evo_min_level and
 * pk_evo_floor return PK_EVO_NO_DATA rather than 1, so a table-less build cannot
 * manufacture a clean bill of health by silence.
 */
__attribute__((weak)) bool pk_evo_have_data(void) { return false; }
__attribute__((weak)) int  pk_evo_count(void) { return 0; }
__attribute__((weak)) const PkEvoLink* pk_evo_row(int i) { (void)i; return 0; }
__attribute__((weak)) int  pk_evo_list(uint16_t species, const PkEvoLink** out) {
  (void)species; (void)out; return 0;
}
__attribute__((weak)) int  pk_evo_min_level(uint16_t species) {
  (void)species; return PK_EVO_NO_DATA;
}
__attribute__((weak)) int  pk_evo_wild_min(uint16_t species) {
  (void)species; return PK_EVO_NO_DATA;
}
__attribute__((weak)) int  pk_evo_floor(uint16_t species) {
  (void)species; return PK_EVO_NO_DATA;
}
/* These two describe the METHOD ENUM, not the data, so the fallback is the real answer
 * rather than a shrug — there is nothing for a missing table to be wrong about. */
__attribute__((weak)) bool pk_evo_method_is_level(uint8_t method) {
  return method == PK_EVO_LEVEL ||
         (method >= PK_EVO_LEVEL_ATK_GT_DEF && method <= PK_EVO_LEVEL_SHEDINJA);
}
__attribute__((weak)) const char* pk_evo_method_name(uint8_t method) {
  switch (method) {
    case PK_EVO_FRIENDSHIP:       return "FRIENDSHIP";
    case PK_EVO_FRIENDSHIP_DAY:   return "FRIENDSHIP DAY";
    case PK_EVO_FRIENDSHIP_NIGHT: return "FRIENDSHIP NIGHT";
    case PK_EVO_LEVEL:            return "LEVEL";
    case PK_EVO_TRADE:            return "TRADE";
    case PK_EVO_TRADE_ITEM:       return "TRADE W/ ITEM";
    case PK_EVO_ITEM:             return "ITEM";
    case PK_EVO_LEVEL_ATK_GT_DEF: return "LEVEL ATK>DEF";
    case PK_EVO_LEVEL_ATK_EQ_DEF: return "LEVEL ATK=DEF";
    case PK_EVO_LEVEL_ATK_LT_DEF: return "LEVEL ATK<DEF";
    case PK_EVO_LEVEL_SILCOON:    return "LEVEL (SILCOON PID)";
    case PK_EVO_LEVEL_CASCOON:    return "LEVEL (CASCOON PID)";
    case PK_EVO_LEVEL_NINJASK:    return "LEVEL (NINJASK)";
    case PK_EVO_LEVEL_SHEDINJA:   return "LEVEL (SHEDINJA)";
    case PK_EVO_BEAUTY:           return "BEAUTY";
    default:                      return "?";
  }
}
#endif

#endif /* EVOLUTIONS_H */
