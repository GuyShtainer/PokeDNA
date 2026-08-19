/* Met-location catalogue: region + per-game scoping + the picker's list build.
 * See gen3_places.h for where every id range comes from. Pure C. */
#include "gen3_places.h"
#include "data_tables.h"   /* pk_location_name — read in ROM, never copied */

#include <string.h>

/* ---- region boundaries (gen3_places.h documents the source) ---- */
#define HOENN_END      87    /* 0..87    Hoenn, all three RSE carts        */
#define KANTO_END     142    /* 88..142  Kanto, FRLG                      */
#define SEVII_END     196    /* 143..196 Sevii, FRLG                      */
#define EX_HOENN_LO   197    /* 197..212 Hoenn, Emerald only              */
#define EX_HOENN_HI   212
#define LOC_NONE      213    /* MAPSEC_NONE                               */
#define HOLE_LO       214    /* 214..252 emitted by no Gen-3 game         */
#define HOLE_HI       252
#define LOC_EGG       253
#define LOC_FATEFUL   255

static const char* const RGN_NAME[G3_RGN_COUNT] = {
  "Hoenn", "Kanto", "Sevii Isles", "Special"
};
static const char* const PSORT_NAME[G3_PSORT_COUNT] = {
  "No. (id)", "A-Z (name)", "Region"
};
static const char* const PGAME_NAME[G3_PGAME_COUNT] = {
  "All", "Ruby/Sapph", "Emerald", "FireRed/LG"
};

const char* g3_region_name(int r)     { return (r >= 0 && r < G3_RGN_COUNT)   ? RGN_NAME[r]   : "?"; }
const char* g3_place_sort_name(int s) { return (s >= 0 && s < G3_PSORT_COUNT) ? PSORT_NAME[s] : "?"; }
const char* g3_place_game_name(int g) { return (g >= 0 && g < G3_PGAME_COUNT) ? PGAME_NAME[g] : "?"; }

bool g3_place_valid(uint16_t loc) {
  if (loc <= LOC_NONE) return true;
  return loc >= LOC_EGG && loc <= LOC_FATEFUL;
}

int g3_region_of(uint16_t loc) {
  if (loc <= HOENN_END)                                return G3_RGN_HOENN;
  if (loc <= KANTO_END)                                return G3_RGN_KANTO;
  if (loc <= SEVII_END)                                return G3_RGN_SEVII;
  if (loc >= EX_HOENN_LO && loc <= EX_HOENN_HI)        return G3_RGN_HOENN;
  if (loc == LOC_NONE)                                 return G3_RGN_SPECIAL;
  if (loc >= LOC_EGG && loc <= LOC_FATEFUL)            return G3_RGN_SPECIAL;
  return -1;                                            /* the 214..252 hole */
}

int g3_game_filter_for(uint8_t metgame) {
  switch (metgame) {
    case 1: case 2: return G3_PGAME_RS;
    case 3:         return G3_PGAME_EMERALD;
    case 4: case 5: return G3_PGAME_FRLG;
    default:        return G3_PGAME_ALL;   /* 0, Colo/XD (15), or garbage: don't narrow */
  }
}

bool g3_place_in_game(uint16_t loc, int gamef) {
  if (!g3_place_valid(loc)) return false;
  if (g3_region_of(loc) == G3_RGN_SPECIAL) return true;   /* markers travel with the mon */
  switch (gamef) {
    case G3_PGAME_RS:      return loc <= HOENN_END;
    case G3_PGAME_EMERALD: return loc <= HOENN_END ||
                                  (loc >= EX_HOENN_LO && loc <= EX_HOENN_HI);
    case G3_PGAME_FRLG:    return loc >= HOENN_END + 1 && loc <= SEVII_END;
    default:               return true;
  }
}

/* ---- search: the species picker's two rules, kept in step ---- */
static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static bool ci_contains(const char* hay, const char* ndl) {
  if (!ndl || !ndl[0]) return true;
  for (; *hay; hay++) {
    const char* a = hay;
    const char* b = ndl;
    while (*b && up1(*a) == up1(*b)) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

static bool all_digits(const char* s) {
  if (!s || !*s) return false;
  for (; *s; s++) if (*s < '0' || *s > '9') return false;
  return true;
}

/* "10" matches 10, 100..109 — the id typed as a prefix. */
static bool num_prefix(unsigned val, const char* q) {
  char b[8];
  int n = 0;
  if (val == 0) b[n++] = '0';
  else { char t[8]; int m = 0; while (val) { t[m++] = (char)('0' + val % 10); val /= 10; }
         while (m) b[n++] = t[--m]; }
  b[n] = 0;
  int ql = (int)strlen(q);
  return ql <= n && strncmp(b, q, (size_t)ql) == 0;
}

static bool matches(uint16_t loc, int region, int gamef, const char* search) {
  if (!g3_place_valid(loc)) return false;
  if (region >= 0 && g3_region_of(loc) != region) return false;
  if (!g3_place_in_game(loc, gamef)) return false;
  if (!search || !search[0]) return true;
  if (all_digits(search)) return num_prefix(loc, search);
  return ci_contains(pk_location_name(loc), search);
}

/* Sort key comparison: "does a come AFTER b". Region sort falls back to the id so the
 * order is total (a stable-looking list the cursor can be restored into). */
static bool place_gt(uint16_t a, uint16_t b, int sort) {
  if (sort == G3_PSORT_NAME) {
    int c = strcmp(pk_location_name(a), pk_location_name(b));
    return c > 0 || (c == 0 && a > b);       /* duplicate names (two SAFARI ZONEs) by id */
  }
  if (sort == G3_PSORT_REGION) {
    int ra = g3_region_of(a), rb = g3_region_of(b);
    return ra > rb || (ra == rb && a > b);
  }
  return a > b;
}

int g3_place_list(uint16_t* out, int cap, int region, int gamef,
                  const char* search, int sort) {
  if (!out || cap <= 0) return 0;
  int n = 0;
  for (unsigned loc = 0; loc <= LOC_FATEFUL && n < cap; loc++)
    if (matches((uint16_t)loc, region, gamef, search)) out[n++] = (uint16_t)loc;
  if (sort != G3_PSORT_ID) {                 /* insertion sort; n <= 217, rebuild only */
    for (int i = 1; i < n; i++) {
      uint16_t v = out[i];
      int j = i - 1;
      while (j >= 0 && place_gt(out[j], v, sort)) { out[j + 1] = out[j]; j--; }
      out[j + 1] = v;
    }
  }
  return n;
}

uint16_t g3_place_step(uint16_t loc, int dir, int region, int gamef) {
  if (dir == 0) return loc;
  int step = (dir > 0) ? 1 : -1;
  int v = (int)loc;
  for (int guard = 0; guard <= LOC_FATEFUL + 1; guard++) {
    v += step;
    if (v > LOC_FATEFUL) v = 0;
    if (v < 0) v = LOC_FATEFUL;
    if ((uint16_t)v == loc) break;                       /* full circle, nothing else */
    if (matches((uint16_t)v, region, gamef, NULL)) return (uint16_t)v;
  }
  return loc;
}

uint16_t g3_place_first(int region, int gamef, uint16_t fallback) {
  for (unsigned loc = 0; loc <= LOC_FATEFUL; loc++)
    if (matches((uint16_t)loc, region, gamef, NULL)) return (uint16_t)loc;
  return fallback;
}
