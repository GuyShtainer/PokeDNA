#ifndef GEN3_FLY_H
#define GEN3_FLY_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/*
 * Fly destinations — which towns the region map will let you Fly to.
 *
 * A Gen-3 Fly destination is unlocked by EXACTLY ONE save bit and nothing else:
 * no heal-location record, no derived state, no extra checksum. The destination
 * warp itself comes from a ROM table (sMapHealLocations / sMapFlyDestinations);
 * the only save data the fly map consults is the flag (plus playerGender for the
 * Littleroot special case). The game sets the bit merely by walking into the town
 * (MAP_SCRIPT_ON_TRANSITION), and only two pieces of code ever read it — the
 * region-map renderer and, for Mauville alone, the Cable Club's Record Corner
 * gate. So flipping these externally is safe: the risk is sequence-breaking, never
 * corruption.
 *
 * Hoenn (Ruby/Sapphire/Emerald) uses FLAG_VISITED_*, a contiguous run parallel to
 * the MAPSEC ids. Kanto (FireRed/LeafGreen) uses a COMPLETELY DIFFERENT family,
 * FLAG_WORLD_MAP_* — it is NOT "Emerald minus 0x60", and getting that wrong writes
 * into FLAG_SYS_* territory.
 *
 * Pure C so tests/host_fly_test.c dual-compiles it on the PC.
 */

/* What a row is, which decides whether "mark all" may touch it. */
enum {
  G3FLY_TOWN   = 0,   /* a real Fly destination; safe, included in "mark all"      */
  G3FLY_FACILITY = 1, /* Battle Frontier / Battle Tower — a much bigger story skip */
  G3FLY_CURSOR = 2,   /* Pokemon League: only positions the map cursor, not a fly  */
  G3FLY_PREREQ = 3,   /* FRLG Sevii map unlocks — without these the island rows do
                       * nothing at all, because the region map cannot even switch
                       * to the Sevii views */
};

typedef struct {
  uint16_t    flag;
  const char* name;
  uint8_t     kind;
} G3FlyDest;

/* The destination rows for `game`, in the order they should be displayed.
 * Returns the row count and points *out at a static table. 0 rows = unsupported. */
int g3fly_list(PkGame game, const G3FlyDest** out);

/* The badge flag that gates FIELD-USE of Fly (Feather Badge in Hoenn, Thunder
 * Badge in Kanto). Marking towns visited does NOT grant Fly — the player still
 * needs this badge AND a party Pokemon that knows the move. */
int  g3fly_badge_flag(PkGame game);
bool g3fly_badge_ok(const uint8_t* sb1, PkGame game);

/* Read / write one destination. */
bool g3fly_get(const uint8_t* sb1, PkGame game, int row);
void g3fly_set(uint8_t* sb1, PkGame game, int row, bool on);

/* Turn on every G3FLY_TOWN row (and, on FRLG, the G3FLY_PREREQ rows, without which
 * the island towns are inert). Deliberately does NOT touch G3FLY_FACILITY, which
 * skips far more of the story and gets its own confirmation in the UI.
 * Returns how many bits actually changed. */
int  g3fly_mark_all(uint8_t* sb1, PkGame game);

/* How many G3FLY_TOWN rows are currently on (for an "8 / 16" style summary). */
int  g3fly_count_on(const uint8_t* sb1, PkGame game, int* total);

/* True iff this row unlocks something beyond flying — currently only Mauville,
 * whose flag doubles as the Cable Club "Record Corner unlocked" gate. Surfacing
 * this in the UI keeps a benign but surprising side effect from looking like a bug. */
bool g3fly_extra_effect(PkGame game, int row);

#endif /* GEN3_FLY_H */
