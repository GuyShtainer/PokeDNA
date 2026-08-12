#ifndef STATICS_H
#define STATICS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Script-placed Pokemon — the half of "how is this Pokemon obtained" that the wild
 * encounter tables do not describe.
 *
 * ---- why this exists -------------------------------------------------------
 * source/encounters.h answers "can this species be met in the grass/water here, and at
 * what level". The legality encounter hook used to treat that as the WHOLE answer, and
 * on a species the wild table only half-describes it produced a false accusation.
 *
 * The Kecleon are the case that proved it. On Routes 119/120 a Kecleon is an invisible
 * object event; you reveal it with the DEVON SCOPE and the map script starts the battle
 * at a level it fixes itself — `setwildbattle SPECIES_KECLEON, 30`
 * (pokeemerald data/scripts/kecleon.inc:74, data/maps/Route120/scripts.inc:193). The
 * SAME routes also carry an ordinary wild Kecleon row at L25-25
 * (src/data/wild_encounters.json, MAP_ROUTE119/120 land_mons). So a Kecleon caught the
 * only way most players ever catch one is met at L30 in a place whose wild table says
 * 25 — and the hook called it suspect. Three of them were sitting in Guy's Emerald box 1
 * and two more in his Ruby, all caught in normal play.
 *
 * The same shape covers the Regis (L40), Groudon/Kyogre (L70 in Emerald, L45 in R/S),
 * Rayquaza (L70), Sudowoodo (L40), New Mauville's Voltorb (L25), the FRLG birds (L50),
 * Mewtwo (L70), Snorlax (L30) and the Hypno holding Lostelle in Berry Forest (L30) — and
 * its mirror image, the script GIFT: the Johto starters, Beldum, Castform, the fossils,
 * Lapras, Eevee, the Magikarp salesman's Magikarp.
 *
 * ---- what the table stores, and what it deliberately does not --------------
 * (species, level, kind) per game. NO MAP. A met location is a region-map SECTION, and
 * pinning a script byte to a section would need a walker that follows goto/call through
 * the shared scripts in data/scripts/ — kecleon.inc alone is reached from four maps. A
 * wrong map would produce a wrong verdict, which is the one thing this whole module
 * exists to prevent, so the table simply does not claim one.
 *
 * That is why the hook uses it ONLY to fall silent. "This species has a script placement
 * somewhere in its origin game" is enough to know the wild table is not the whole story
 * about it, and therefore enough to know the level/place reasoning cannot speak. The
 * levels are stored anyway because they are the evidence that the extraction found the
 * right thing (tests/host_statics_test.c asserts KECLEON L30, HYPNO L30, the Regis L40,
 * the version-exclusive Groudon/Kyogre split) and because they are the promotion path:
 * the day the map is attributable, pk_static_at_level lets the level window widen
 * instead of vanishing.
 *
 * NOT COMPLETE OVER EVERY ROUTE, and the hook's posture depends on knowing that. Pokemon
 * handed out by C code rather than by a script command are absent: the roamers
 * (CreateRoamerMon) and the FRLG Game Corner prizes. Eggs (met level 0) and in-game
 * trades (met location 0xFE) need no entry — the hook exempts them already.
 *
 * source/statics.c is GENERATED and git-ignored. `python3 tools/gen_statics.py
 * --from-rom` finds the event-script region in each of the five retail dumps and scans
 * it for the two script opcodes, gated on reproducing the pokeemerald checkout's script
 * sources EXACTLY. Reading the carts rather than a decomp is what makes the table
 * shippable (plain numbers measured from hardware the user owns) and is the only way to
 * get Ruby/Sapphire/FRLG data at all.
 *
 * Pure C — no tonc, no GBA headers, no FatFs. All tables are `const` (ROM, read in
 * place); this module adds zero EWRAM.
 */

/* Which script command placed it. Kept because they are different claims: a
 * setwildbattle is a battle you can flee or fail, a givemon is handed over. Nothing in
 * the current checker branches on it — it is here so the table stays auditable. */
#define PK_STATIC_BATTLE  1   /* setwildbattle: the static/scripted wild battle */
#define PK_STATIC_GIFT    2   /* givemon:       an NPC hands it over            */

typedef struct {
  uint16_t species;   /* Gen-3 INTERNAL id, 1..411 — same numbering as PkMon.species */
  uint8_t  level;     /* the literal level byte in the script command                */
  uint8_t  kind;      /* PK_STATIC_BATTLE / PK_STATIC_GIFT                           */
} PkStaticEntry;      /* 4 bytes, no padding */

/* Tri-state, for the same reason encounters.h has one: "no table is linked in" and
 * "this species genuinely has no script placement" must never collapse into one answer.
 * The first must produce no verdict; only the second is evidence. */
#define PK_STATIC_NO_DATA (-1)
#define PK_STATIC_NO      0
#define PK_STATIC_YES     1

/* `game` is the record's own origin/metGame byte: 1 Sapphire, 2 Ruby, 3 Emerald,
 * 4 FireRed, 5 LeafGreen — the numbering of PkEncGame in encounters.h. Taken as a plain
 * uint8_t so this module stays independent of that header. */

/* False when source/statics.c was not generated. The encounter hook refuses to speak at
 * all in that case: without this half of the picture it cannot tell a Devon-Scope
 * Kecleon from a forged one, and guessing is how the false accusation came back. */
bool pk_static_have_data(void);

/* Rows in one game's table (0 if that ROM was missing when the table was generated). */
int pk_static_count(uint8_t game);

/* Does `species` have ANY script placement in `game`? This is the question the legality
 * hook actually asks. */
int pk_static_any(uint8_t game, uint16_t species);

/* Is there a placement of `species` at exactly `level`? Not used by the hook today (see
 * the header comment on why the level cannot be trusted as a bound yet); it is the
 * accessor a future, map-aware version widens the wild window with. */
int pk_static_at_level(uint8_t game, uint16_t species, uint8_t level);

/* Every placement of one species, as a contiguous slice (the table is sorted by
 * (species, level)). Returns the row count; *out is set only when that is > 0. For the
 * "where does this come from" UI and for the host tests. */
int pk_static_list(uint8_t game, uint16_t species, const PkStaticEntry** out);

#if defined(__GNUC__) && !defined(PK_STATICS_IMPL)
/*
 * Weak no-data fallbacks, so a clone with no generated table still links and behaves
 * sanely — the same posture as source/encounters.h and source/art_fallbacks.c. The
 * generated statics.c defines PK_STATICS_IMPL before including this header, so it never
 * collides with its own strong definitions; every other translation unit carries a weak
 * copy that the linker discards the moment the real table exists.
 *
 * Every fallback answers "I do not know", never "no" — a missing table must not be able
 * to manufacture a legality complaint, and must not be able to silence one either
 * (pk2_encounter_data_ok requires this table, so an absent one disarms the hook and the
 * UI reports it as absent instead of showing a clean bill of health).
 */
__attribute__((weak)) bool pk_static_have_data(void) { return false; }
__attribute__((weak)) int  pk_static_count(uint8_t game) { (void)game; return 0; }
__attribute__((weak)) int  pk_static_any(uint8_t game, uint16_t species) {
  (void)game; (void)species; return PK_STATIC_NO_DATA;
}
__attribute__((weak)) int  pk_static_at_level(uint8_t game, uint16_t species, uint8_t level) {
  (void)game; (void)species; (void)level; return PK_STATIC_NO_DATA;
}
__attribute__((weak)) int  pk_static_list(uint8_t game, uint16_t species,
                                          const PkStaticEntry** out) {
  (void)game; (void)species; (void)out; return 0;
}
#endif

#endif /* STATICS_H */
