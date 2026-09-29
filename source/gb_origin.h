#ifndef GB_ORIGIN_H
#define GB_ORIGIN_H

#include <stdint.h>

/*
 * BACKLOG #266: the lowest LEGAL level a freshly created Gen-1/2 Pokemon may start at,
 * given how that species can actually be obtained in its game.
 *
 * rom_gblearn_min_level() answers only the EVOLUTION half ("an evolved form is at least
 * as high as the level it evolves at, else 5 = the hatch/starter floor"). That is right
 * for a breedable or ordinary wild species and WRONG for one whose only origin is a
 * fixed-level script encounter: Articuno cannot be bred or found in grass, it is met at
 * L50, so a created L5 Articuno is a Pokemon no game can produce. This module is the
 * missing half -- a tiny const table of those one-origin static encounters -- and a
 * single pure function that folds it into the evolution floor.
 *
 * Gen 1 rows (level = the literal level of the object_event in the game's own map
 * data, identical in Red/Blue/Yellow): Articuno/Zapdos/Moltres 50, Mewtwo 70.
 * Deliberately NOT here: Mew (no in-game placement exists in Gen 1 -- there is nothing
 * to derive a level from, so it keeps the ordinary floor), and every Gen-2 legendary
 * (their placements are not in the reference data this tree carries, and a guessed
 * level would be a false legality claim -- the exact thing this module exists to stop).
 *
 * Gen 1/2 records carry no met location or ball, so unlike Gen 3 there is nothing else
 * for the table to decide; Gen 2's caught-data stays 0 for a created mon (gb_new_mon.h).
 *
 * Pure C; const data only (ROM, zero EWRAM).
 */

/* `floor` is the level the caller already computed (rom_gblearn_min_level, or the
 * fallback 5). Returns max(floor, the species' static-encounter level in `gen`), or
 * `floor` unchanged when `dex` has no static-only origin there. `gen` is GB_GEN1/2
 * (gb_edit.h's numbering: 1 or 2); any other value, or dex 0, returns `floor`. */
uint8_t gb_origin_level_floor(uint8_t gen, uint16_t dex, uint8_t floor);

/* The static-encounter level for `dex` in `gen`, or 0 when it has none. Exposed for
 * tests and for a future "why this level" line. */
uint8_t gb_origin_static_level(uint8_t gen, uint16_t dex);

#endif /* GB_ORIGIN_H */
