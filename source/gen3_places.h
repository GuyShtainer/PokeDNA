#ifndef GEN3_PLACES_H
#define GEN3_PLACES_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Gen-3 MET LOCATIONS as a browsable catalogue: which region a met-location id belongs
 * to, which origin games can legitimately stamp it, and the sorted/filtered list a
 * picker draws. Pure C — no tonc, no GBA headers — so tests/host_places_test.c runs the
 * identical code on the PC.
 *
 * NO NAME TABLE LIVES HERE. The 256 location names are in the generated (git-ignored)
 * data_tables.c, in ROM, and this module only ever calls pk_location_name() to read one
 * in place. Nothing is copied into RAM: a picker holds a list of uint16_t IDS.
 *
 * ---- the id space ----------------------------------------------------------
 * The record's met location is a region-map SECTION id (pokeemerald src/pokemon.c:2257
 * stores GetCurrentRegionMapSectionId()), and the sections are one shared numbering
 * across the family (source/region_map.h documents the ranges measured off the retail
 * ROMs):
 *
 *     0  .. 87    Hoenn                       — Ruby, Sapphire and Emerald
 *     88 ..142    Kanto                       — FireRed / LeafGreen
 *     143..196    Sevii Islands               — FireRed / LeafGreen
 *     197..212    Hoenn, Emerald-only places  — Emerald (Marine/Terra Cave, Artisan
 *                                               Cave, Navel Rock, Trainer Hill, …)
 *     213         NONE                        — the "no place" section (MAPSEC_NONE)
 *     214..252    unused — no Gen-3 game emits one; a value here is a hacked record
 *     253/254/255 EGG / TRADE / FATEFUL       — markers, not places
 *
 * gen3_legality.c already refuses 214..252 (`metLocation > 0xD5 && < 0xFD`); this module
 * is the constructive side of the same rule — a picker built on it cannot OFFER one.
 *
 * ---- why the game scoping matters ------------------------------------------
 * A FireRed mon met on "ROUTE 119" is not a subtle problem, it is an impossible one: the
 * section does not exist in that cartridge. g3_place_in_game() is what keeps a picker
 * from handing the user that trap, and it is why the location list defaults to the
 * record's own origin game rather than to "all".
 *
 * ---- one wart, stated rather than hidden -----------------------------------
 * Ruby/Sapphire's MAPSEC_NONE is 0x58 (88), the slot Emerald/FRLG give to PALLET TOWN;
 * the shared name table can only render one of those. Scoping RS to 0..87 means the
 * picker never offers 88 to an RS record, so it cannot MAKE the confusion — but an RS
 * record that already holds 88 will still read as "PALLET TOWN" on screen.
 */

/* Regions, in the order the picker lists them. */
enum {
  G3_RGN_HOENN = 0,
  G3_RGN_KANTO,
  G3_RGN_SEVII,
  G3_RGN_SPECIAL,      /* NONE / EGG / TRADE / FATEFUL — markers, not geography */
  G3_RGN_COUNT
};

/* Sort orders. */
enum { G3_PSORT_ID = 0, G3_PSORT_NAME, G3_PSORT_REGION, G3_PSORT_COUNT };

/* Game filters, in the order the filter menu lists them. G3_PGAME_ORIGIN means "use the
 * record's own metGame", which the caller resolves before calling in. */
enum {
  G3_PGAME_ALL = 0,
  G3_PGAME_RS,         /* Ruby + Sapphire   (origin bytes 1, 2) */
  G3_PGAME_EMERALD,    /* Emerald           (origin byte 3)     */
  G3_PGAME_FRLG,       /* FireRed/LeafGreen (origin bytes 4, 5) */
  G3_PGAME_COUNT
};

/* Every valid id: 0..213 plus the three markers. The cap a caller's index array needs. */
#define G3_PLACE_MAX 217

const char* g3_region_name(int region);        /* "Hoenn" … ; "?" out of range   */
const char* g3_place_sort_name(int sort);      /* for the filter menu row        */
const char* g3_place_game_name(int gamef);     /* for the filter menu row        */

/* True for an id a Gen-3 game can actually hold (0..213, 253..255). */
bool g3_place_valid(uint16_t loc);

/* Region of an id, or -1 for the unused 214..252 hole. */
int  g3_region_of(uint16_t loc);

/* Origin-game byte (1 Sapphire … 5 LeafGreen, 15 Colo/XD) -> G3_PGAME_*; anything this
 * module cannot place — 0, 15, or a garbage byte — maps to G3_PGAME_ALL, because a
 * filter must never be stricter than the knowledge behind it. */
int  g3_game_filter_for(uint8_t metgame);

/* Can origin game `gamef` (a G3_PGAME_*) stamp this location? */
bool g3_place_in_game(uint16_t loc, int gamef);

/* Build the display list into `out` (cap >= G3_PLACE_MAX for the unfiltered case).
 *   region  — a G3_RGN_*, or <0 for all
 *   gamef   — a G3_PGAME_*
 *   search  — NULL/"" for none; case-insensitive substring of the NAME, or, for an
 *             all-digit query, a prefix match on the ID (the species picker's rule)
 *   sort    — a G3_PSORT_*
 * Returns how many ids were written. */
int  g3_place_list(uint16_t* out, int cap, int region, int gamef,
                   const char* search, int sort);

/* Step to the next/prev valid id (dir +1/-1) inside `region` (<0 = any) and `gamef`,
 * wrapping. Returns `loc` unchanged when nothing else qualifies — this is what the
 * editor's LEFT/RIGHT does, so it must never land on the 214..252 hole. */
uint16_t g3_place_step(uint16_t loc, int dir, int region, int gamef);

/* The first valid id of a region for a game (what "switch region" jumps to), or the
 * unchanged fallback when that region is empty for this game. */
uint16_t g3_place_first(int region, int gamef, uint16_t fallback);

#endif /* GEN3_PLACES_H */
