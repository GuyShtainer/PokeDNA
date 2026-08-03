/*
 * Fly-destination (visited-town) flags. See gen3_fly.h for the mechanism and why
 * flipping these bits is safe. Pure C — host-testable.
 *
 * Flag numbers are absolute and were resolved from each game's own
 * include/constants/flags.h (the bases are Emerald SYSTEM_FLAGS 0x860, Ruby/Sapphire
 * SYSTEM_FLAGS 0x800, FireRed/LeafGreen SYS_FLAGS 0x800). They are written out
 * literally rather than computed from a base so a wrong base can't silently shift a
 * whole table into someone else's flags.
 */
#include "gen3_fly.h"
#include "gen3_flags.h"

/* ---- Hoenn: Emerald (SYSTEM_FLAGS 0x860) ---------------------------------- */
static const G3FlyDest k_em[] = {
  { 0x086F, "Littleroot Town",  G3FLY_TOWN },
  { 0x0870, "Oldale Town",      G3FLY_TOWN },
  { 0x0871, "Dewford Town",     G3FLY_TOWN },
  { 0x0872, "Lavaridge Town",   G3FLY_TOWN },
  { 0x0873, "Fallarbor Town",   G3FLY_TOWN },
  { 0x0874, "Verdanturf Town",  G3FLY_TOWN },
  { 0x0875, "Pacifidlog Town",  G3FLY_TOWN },
  { 0x0876, "Petalburg City",   G3FLY_TOWN },
  { 0x0877, "Slateport City",   G3FLY_TOWN },
  { 0x0878, "Mauville City",    G3FLY_TOWN },   /* also the Record Corner gate */
  { 0x0879, "Rustboro City",    G3FLY_TOWN },
  { 0x087A, "Fortree City",     G3FLY_TOWN },
  { 0x087B, "Lilycove City",    G3FLY_TOWN },
  { 0x087C, "Mossdeep City",    G3FLY_TOWN },
  { 0x087D, "Sootopolis City",  G3FLY_TOWN },
  { 0x087E, "Ever Grande City", G3FLY_TOWN },
  /* The 17th destination. NOT part of the contiguous run above — a list built as
   * "16 towns" silently misses the one Emerald players most often want. */
  { 0x08A8, "Battle Frontier",  G3FLY_FACILITY },
  { 0x08B4, "Pokemon League",   G3FLY_CURSOR },
};

/* ---- Hoenn: Ruby/Sapphire (SYSTEM_FLAGS 0x800) ---------------------------- */
static const G3FlyDest k_rs[] = {
  { 0x080F, "Littleroot Town",  G3FLY_TOWN },
  { 0x0810, "Oldale Town",      G3FLY_TOWN },
  { 0x0811, "Dewford Town",     G3FLY_TOWN },
  { 0x0812, "Lavaridge Town",   G3FLY_TOWN },
  { 0x0813, "Fallarbor Town",   G3FLY_TOWN },
  { 0x0814, "Verdanturf Town",  G3FLY_TOWN },
  { 0x0815, "Pacifidlog Town",  G3FLY_TOWN },
  { 0x0816, "Petalburg City",   G3FLY_TOWN },
  { 0x0817, "Slateport City",   G3FLY_TOWN },
  { 0x0818, "Mauville City",    G3FLY_TOWN },
  { 0x0819, "Rustboro City",    G3FLY_TOWN },
  { 0x081A, "Fortree City",     G3FLY_TOWN },
  { 0x081B, "Lilycove City",    G3FLY_TOWN },
  { 0x081C, "Mossdeep City",    G3FLY_TOWN },
  { 0x081D, "Sootopolis City",  G3FLY_TOWN },
  { 0x081E, "Ever Grande City", G3FLY_TOWN },
  { 0x0848, "Battle Tower",     G3FLY_FACILITY },
  { 0x0854, "Pokemon League",   G3FLY_CURSOR },
};

/* ---- Kanto + Sevii: FireRed/LeafGreen (SYS_FLAGS 0x800) -------------------
 * A different flag FAMILY, not a rebased Hoenn table. The run is 0x890..0x8A3
 * and STOPS there: 0x8A4 onward are dungeon map-preview flags (GetDungeonMapsecType),
 * not Fly destinations. Note SEVEN ISLAND comes BEFORE SIX ISLAND, in both the
 * flag order and the MAPSEC ids — a hand-typed table gets this backwards and
 * silently marks the wrong island. */
static const G3FlyDest k_fr[] = {
  { 0x0890, "Pallet Town",     G3FLY_TOWN },
  { 0x0891, "Viridian City",   G3FLY_TOWN },
  { 0x0892, "Pewter City",     G3FLY_TOWN },
  { 0x0893, "Cerulean City",   G3FLY_TOWN },
  { 0x0894, "Lavender Town",   G3FLY_TOWN },
  { 0x0895, "Vermilion City",  G3FLY_TOWN },
  { 0x0896, "Celadon City",    G3FLY_TOWN },
  { 0x0897, "Fuchsia City",    G3FLY_TOWN },
  { 0x0898, "Cinnabar Island", G3FLY_TOWN },
  { 0x0899, "Indigo Plateau",  G3FLY_TOWN },
  { 0x089A, "Saffron City",    G3FLY_TOWN },
  { 0x089B, "One Island",      G3FLY_TOWN },
  { 0x089C, "Two Island",      G3FLY_TOWN },
  { 0x089D, "Three Island",    G3FLY_TOWN },
  { 0x089E, "Four Island",     G3FLY_TOWN },
  { 0x089F, "Five Island",     G3FLY_TOWN },
  { 0x08A0, "Seven Island",    G3FLY_TOWN },   /* SEVEN is 0x8A0 ... */
  { 0x08A1, "Six Island",      G3FLY_TOWN },   /* ... and SIX is 0x8A1 */
  { 0x08A2, "Route 4 Center",  G3FLY_TOWN },
  { 0x08A3, "Route 10 Center", G3FLY_TOWN },
  { 0x0845, "Sevii map 1-2-3",   G3FLY_PREREQ },
  { 0x0846, "Sevii map 4-5-6-7", G3FLY_PREREQ },
};

int g3fly_list(PkGame game, const G3FlyDest** out) {
  switch (game) {
    case PK_EMERALD: if (out) *out = k_em; return (int)(sizeof k_em / sizeof k_em[0]);
    case PK_RS:      if (out) *out = k_rs; return (int)(sizeof k_rs / sizeof k_rs[0]);
    case PK_FRLG:    if (out) *out = k_fr; return (int)(sizeof k_fr / sizeof k_fr[0]);
    default:         if (out) *out = 0;    return 0;
  }
}

/* FIELD_MOVE_FLY is 5 in Hoenn and 2 in Kanto; the game tests
 * FlagGet(FLAG_BADGE01_GET + fieldMove). pk_badge_flag already resolves the per-game
 * badge base, so this is just the right index. */
int g3fly_badge_flag(PkGame game) {
  return pk_badge_flag(game, game == PK_FRLG ? 2 : 5);
}

bool g3fly_badge_ok(const uint8_t* sb1, PkGame game) {
  int f = g3fly_badge_flag(game);
  return (sb1 && f >= 0) ? pk_flag_get(sb1, game, f) : false;
}

bool g3fly_get(const uint8_t* sb1, PkGame game, int row) {
  const G3FlyDest* t; int n = g3fly_list(game, &t);
  if (!sb1 || row < 0 || row >= n) return false;
  return pk_flag_get(sb1, game, t[row].flag);
}

void g3fly_set(uint8_t* sb1, PkGame game, int row, bool on) {
  const G3FlyDest* t; int n = g3fly_list(game, &t);
  if (!sb1 || row < 0 || row >= n) return;
  pk_flag_set(sb1, game, t[row].flag, on);
}

int g3fly_mark_all(uint8_t* sb1, PkGame game) {
  const G3FlyDest* t; int n = g3fly_list(game, &t);
  int changed = 0;
  if (!sb1) return 0;
  for (int i = 0; i < n; i++) {
    /* Towns are safe. Prereqs come along because on FRLG the island rows are inert
     * without them — marking islands but not the Sevii map unlocks looks broken. */
    if (t[i].kind != G3FLY_TOWN && t[i].kind != G3FLY_PREREQ) continue;
    if (!pk_flag_get(sb1, game, t[i].flag)) {
      pk_flag_set(sb1, game, t[i].flag, true);
      changed++;
    }
  }
  return changed;
}

int g3fly_count_on(const uint8_t* sb1, PkGame game, int* total) {
  const G3FlyDest* t; int n = g3fly_list(game, &t);
  int on = 0, tot = 0;
  for (int i = 0; i < n; i++) {
    if (t[i].kind != G3FLY_TOWN) continue;
    tot++;
    if (sb1 && pk_flag_get(sb1, game, t[i].flag)) on++;
  }
  if (total) *total = tot;
  return on;
}

bool g3fly_extra_effect(PkGame game, int row) {
  const G3FlyDest* t; int n = g3fly_list(game, &t);
  if (row < 0 || row >= n) return false;
  /* FLAG_VISITED_MAUVILLE_CITY also gates the Cable Club's Record Corner
   * (data/scripts/cable_club.inc) — turning it on quietly unlocks Record Mixing. */
  return (game == PK_EMERALD && t[row].flag == 0x0878) ||
         (game == PK_RS      && t[row].flag == 0x0818);
}
