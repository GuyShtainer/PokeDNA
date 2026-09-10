#ifndef PDNA_GBFLY_H
#define PDNA_GBFLY_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession */

/* Gen-1/2 Fly-destination screen (BACKLOG #90) -- mirrors source/pdna_fly.c's shape
 * (a scrolling list, A toggles, B asks to save) over gb_fly.h's pure-C bitfield core
 * (gbfy_get/gbfy_set/gbfy_count/gbfy_game) instead of gen3_fly.h.
 *
 * NAMES: cited against the real games' own decomps (assets/upstream/, reference
 * only) -- pokered's map_const list (constants/map_constants.asm, NUM_CITY_MAPS=11,
 * town_map.asm's BuildFlyLocationsList confirms bit i == map id i, LSB-first) for
 * Gen 1, and pokecrystal's SPAWN_* enum (constants/map_data_constants.asm,
 * NUM_SPAWNS=28, data/events/engine_flags.asm's engine_flag wVisitedSpawns list
 * confirms bit i == SPAWN_* value i) for Gen 2. Gen 2's own Fly MENU (Flypoints,
 * data/maps/flypoints.asm) never lists SPAWN_HOME, SPAWN_DEBUG, SPAWN_UNION_CAVE or
 * SPAWN_FAST_SHIP as a destination -- those four are respawn-only spots (whiteout /
 * boarding the ship), not Town Map Fly choices, even though wVisitedSpawns has a real
 * bit for each. Rather than hide 4 of the 28 bits gbfy_count() exposes (gb_fly.h's
 * own header: this core deliberately does not curate a "fly-able" subset), this
 * screen shows all 28 with a "spn" tag on those four (mirrors pdna_fly.c's own
 * kind_tag column for G3FLY_FACILITY/CURSOR/PREREQ) so a player can still see/clear
 * them without being told they are Town Map stops when the cartridge itself never
 * offers them as one.
 *
 * `can_edit`: true for the editable session (gb_nav_from_start passes &g_ed->s,
 * true, same convention as pdna_gbtrainer.h), false for a view-only visit -- U/D
 * still scrolls so every destination can be read, A does nothing. Commits via
 * gb_persist() (pdna_gen12.h) on B, only when at least one bit actually changed. */
void pdna_gb_fly(GbSession* s, bool can_edit);

#endif /* PDNA_GBFLY_H */
