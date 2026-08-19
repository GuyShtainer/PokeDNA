/* Host test for the met-location catalogue (source/gen3_places.c) — the data behind the
 * met-location and region pickers.
 *
 *   cc -std=c11 -I source tests/host_places_test.c source/gen3_places.c \
 *      source/data_tables.c -o /tmp/hpl
 *   /tmp/hpl
 *
 * The point of this test is the LEGALITY TRAP the pickers exist to avoid: a picker that
 * offers Hoenn routes to a FireRed record, or Emerald's Marine Cave to a Ruby one, hands
 * the user an impossible mon and calls it an edit. So the assertions below are mostly of
 * the form "this id must NOT be offered to that game".
 *
 * Ids and names come from source/data_tables.c, which is GENERATED from the user's own
 * cartridge dumps and git-ignored; nothing is re-typed here except a handful of spot ids
 * whose names the test then reads back out of that table to prove the mapping is real.
 */
#include <stdio.h>
#include <string.h>

#include "gen3_places.h"
#include "data_tables.h"

static int fails = 0, checks = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

/* Spot ids, with the name each must resolve to. If data_tables.c is ever regenerated
 * with a different layout this is the check that goes red first, which is the point. */
struct Spot { uint16_t id; const char* name; int region; };
static const struct Spot SPOT[] = {
  {   0, "LITTLEROOT TOWN", G3_RGN_HOENN   },
  {  16, "ROUTE 101",       G3_RGN_HOENN   },
  {  34, "ROUTE 119",       G3_RGN_HOENN   },
  {  87, "DYNAMIC",         G3_RGN_HOENN   },
  {  88, "PALLET TOWN",     G3_RGN_KANTO   },
  { 101, "ROUTE 1",         G3_RGN_KANTO   },
  { 142, "POWER PLANT",     G3_RGN_KANTO   },
  { 143, "ONE ISLAND",      G3_RGN_SEVII   },
  { 196, "SPECIAL AREA",    G3_RGN_SEVII   },
  { 197, "AQUA HIDEOUT",    G3_RGN_HOENN   },   /* Emerald-only Hoenn extras */
  { 203, "MARINE CAVE",     G3_RGN_HOENN   },
  { 212, "TRAINER HILL",    G3_RGN_HOENN   },
  { 213, "NONE",            G3_RGN_SPECIAL },
  { 253, "EGG",             G3_RGN_SPECIAL },
  { 254, "TRADE",           G3_RGN_SPECIAL },
  { 255, "FATEFUL",         G3_RGN_SPECIAL },
};

int main(void) {
  static uint16_t idx[G3_PLACE_MAX];

  printf("== met-location catalogue ==\n");

  /* (1) the id space: what exists, what is a hole. */
  {
    int nvalid = 0;
    for (unsigned i = 0; i <= 255; i++) if (g3_place_valid((uint16_t)i)) nvalid++;
    printf("(1) valid ids: %d (cap %d)\n", nvalid, G3_PLACE_MAX);
    CHECK(nvalid == G3_PLACE_MAX, "valid-id count == G3_PLACE_MAX");
    for (unsigned i = 214; i <= 252; i++) {
      CHECK(!g3_place_valid((uint16_t)i), "214..252 is a hole, never valid");
      CHECK(g3_region_of((uint16_t)i) < 0, "hole ids have no region");
    }
    /* gen3_legality.c's rule, restated constructively: exactly the ids it calls invalid
     * are the ids this module refuses. */
    for (unsigned i = 0; i <= 255; i++) {
      int legality_bad = (i > 0xD5 && i < 0xFD);
      CHECK(g3_place_valid((uint16_t)i) == !legality_bad,
            "g3_place_valid agrees with gen3_legality's met-location rule");
    }
  }

  /* (2) spot ids: name + region. */
  for (unsigned s = 0; s < sizeof SPOT / sizeof SPOT[0]; s++) {
    const char* nm = pk_location_name(SPOT[s].id);
    int r = g3_region_of(SPOT[s].id);
    if (strcmp(nm, SPOT[s].name) != 0 || r != SPOT[s].region)
      printf("  !! id %u -> \"%s\" region %d (wanted \"%s\" region %d)\n",
             SPOT[s].id, nm, r, SPOT[s].name, SPOT[s].region);
    CHECK(strcmp(nm, SPOT[s].name) == 0, "spot id resolves to its expected name");
    CHECK(r == SPOT[s].region, "spot id lands in its expected region");
  }
  printf("(2) %u spot ids checked\n", (unsigned)(sizeof SPOT / sizeof SPOT[0]));

  /* (3) per-game scoping — the legality trap. */
  {
    CHECK(g3_game_filter_for(1) == G3_PGAME_RS,      "Sapphire  -> RS filter");
    CHECK(g3_game_filter_for(2) == G3_PGAME_RS,      "Ruby      -> RS filter");
    CHECK(g3_game_filter_for(3) == G3_PGAME_EMERALD, "Emerald   -> Emerald filter");
    CHECK(g3_game_filter_for(4) == G3_PGAME_FRLG,    "FireRed   -> FRLG filter");
    CHECK(g3_game_filter_for(5) == G3_PGAME_FRLG,    "LeafGreen -> FRLG filter");
    CHECK(g3_game_filter_for(15) == G3_PGAME_ALL,    "Colo/XD does not narrow");
    CHECK(g3_game_filter_for(0)  == G3_PGAME_ALL,    "unknown origin does not narrow");

    /* FireRed must never be offered Hoenn, and RSE must never be offered Kanto/Sevii. */
    CHECK(!g3_place_in_game(16,  G3_PGAME_FRLG),    "ROUTE 101 not in FRLG");
    CHECK(!g3_place_in_game(34,  G3_PGAME_FRLG),    "ROUTE 119 not in FRLG");
    CHECK(!g3_place_in_game(203, G3_PGAME_FRLG),    "MARINE CAVE not in FRLG");
    CHECK(!g3_place_in_game(88,  G3_PGAME_RS),      "PALLET TOWN not in RS");
    CHECK(!g3_place_in_game(88,  G3_PGAME_EMERALD), "PALLET TOWN not in Emerald");
    CHECK(!g3_place_in_game(143, G3_PGAME_EMERALD), "ONE ISLAND not in Emerald");
    /* the Emerald-only Hoenn extras */
    for (unsigned i = 197; i <= 212; i++) {
      CHECK(!g3_place_in_game((uint16_t)i, G3_PGAME_RS), "197..212 not in Ruby/Sapphire");
      CHECK(g3_place_in_game((uint16_t)i, G3_PGAME_EMERALD), "197..212 are in Emerald");
    }
    /* shared Hoenn, and the markers, travel everywhere */
    CHECK(g3_place_in_game(16, G3_PGAME_RS) && g3_place_in_game(16, G3_PGAME_EMERALD),
          "ROUTE 101 is in RS and Emerald");
    for (unsigned i = 253; i <= 255; i++)
      for (int g = 0; g < G3_PGAME_COUNT; g++)
        CHECK(g3_place_in_game((uint16_t)i, g), "EGG/TRADE/FATEFUL exist for every game");
    CHECK(g3_place_in_game(213, G3_PGAME_FRLG), "NONE exists for every game");
    /* a hole id is offered to nobody */
    for (int g = 0; g < G3_PGAME_COUNT; g++)
      CHECK(!g3_place_in_game(230, g), "a hole id is in no game");
  }

  /* (4) list build: counts, region scoping, and the game default. */
  {
    int all = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "", G3_PSORT_ID);
    printf("(4) all=%d", all);
    CHECK(all == G3_PLACE_MAX, "unfiltered list is every valid id");
    for (int i = 1; i < all; i++) CHECK(idx[i] > idx[i - 1], "PSORT_ID is ascending");

    int hoenn = g3_place_list(idx, G3_PLACE_MAX, G3_RGN_HOENN, G3_PGAME_ALL, "", G3_PSORT_ID);
    int kanto = g3_place_list(idx, G3_PLACE_MAX, G3_RGN_KANTO, G3_PGAME_ALL, "", G3_PSORT_ID);
    int sevii = g3_place_list(idx, G3_PLACE_MAX, G3_RGN_SEVII, G3_PGAME_ALL, "", G3_PSORT_ID);
    int spec  = g3_place_list(idx, G3_PLACE_MAX, G3_RGN_SPECIAL, G3_PGAME_ALL, "", G3_PSORT_ID);
    printf(" hoenn=%d kanto=%d sevii=%d special=%d\n", hoenn, kanto, sevii, spec);
    CHECK(hoenn == 88 + 16, "Hoenn = 0..87 plus the 16 Emerald extras");
    CHECK(kanto == 55,      "Kanto = 88..142");
    CHECK(sevii == 54,      "Sevii = 143..196");
    CHECK(spec  == 4,       "Special = NONE + EGG + TRADE + FATEFUL");
    CHECK(hoenn + kanto + sevii + spec == all, "the regions partition the id space");

    /* the picker's default for a FireRed record: no Hoenn at all */
    int frlg_hoenn = g3_place_list(idx, G3_PLACE_MAX, G3_RGN_HOENN,
                                   g3_game_filter_for(4), "", G3_PSORT_ID);
    CHECK(frlg_hoenn == 0, "a FireRed record is offered no Hoenn place");
    int rs_all = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_RS, "", G3_PSORT_ID);
    CHECK(rs_all == 88 + 4, "Ruby/Sapphire sees 0..87 plus the four markers");
    for (int i = 0; i < rs_all; i++)
      CHECK(idx[i] <= 87 || idx[i] >= 213, "nothing outside RS's own range leaks in");

    /* cap is honoured (a caller with a short array must not be overrun) */
    static uint16_t small[8];
    int n = g3_place_list(small, 8, -1, G3_PGAME_ALL, "", G3_PSORT_ID);
    CHECK(n == 8, "cap clamps the count");
  }

  /* (5) sorting. */
  {
    int n = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "", G3_PSORT_NAME);
    CHECK(n == G3_PLACE_MAX, "name sort keeps every entry");
    for (int i = 1; i < n; i++) {
      int c = strcmp(pk_location_name(idx[i - 1]), pk_location_name(idx[i]));
      CHECK(c < 0 || (c == 0 && idx[i - 1] < idx[i]), "PSORT_NAME is A-Z, ties by id");
    }
    n = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "", G3_PSORT_REGION);
    CHECK(n == G3_PLACE_MAX, "region sort keeps every entry");
    for (int i = 1; i < n; i++) {
      int ra = g3_region_of(idx[i - 1]), rb = g3_region_of(idx[i]);
      CHECK(ra < rb || (ra == rb && idx[i - 1] < idx[i]), "PSORT_REGION groups then ids");
    }
    printf("(5) sorts ok\n");
  }

  /* (6) search: substring on the name, prefix on the id. */
  {
    int n = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "route", G3_PSORT_ID);
    printf("(6) \"route\"=%d", n);
    CHECK(n > 40, "a lower-case \"route\" query is case-insensitive and finds the routes");
    for (int i = 0; i < n; i++)
      CHECK(strstr(pk_location_name(idx[i]), "ROUTE") != NULL, "every hit contains ROUTE");

    /* the infix queries a first-letter jump could not express */
    int u = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "UNDERWATER", G3_PSORT_ID);
    int c = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "CAVE", G3_PSORT_ID);
    printf(" \"UNDERWATER\"=%d \"CAVE\"=%d", u, c);
    CHECK(u >= 8, "UNDERWATER is a shared infix, not a prefix");
    CHECK(c >= 8, "CAVE likewise");

    /* an all-digit query filters by id prefix */
    n = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "16", G3_PSORT_ID);
    printf(" \"16\"=%d\n", n);
    CHECK(n == 11, "\"16\" matches 16 and 160..169");
    CHECK(idx[0] == 16, "and 16 itself comes first");
    n = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "255", G3_PSORT_ID);
    CHECK(n == 1 && idx[0] == 255, "\"255\" matches only FATEFUL");
    /* a query that matches nothing must yield an empty list, not garbage */
    n = g3_place_list(idx, G3_PLACE_MAX, -1, G3_PGAME_ALL, "ZZQQ", G3_PSORT_ID);
    CHECK(n == 0, "no match -> empty list");
  }

  /* (7) stepping: the editor's LEFT/RIGHT must never land on a hole. */
  {
    for (unsigned i = 0; i <= 255; i++) {
      if (!g3_place_valid((uint16_t)i)) continue;
      uint16_t up = g3_place_step((uint16_t)i, +1, -1, G3_PGAME_ALL);
      uint16_t dn = g3_place_step((uint16_t)i, -1, -1, G3_PGAME_ALL);
      CHECK(g3_place_valid(up) && g3_place_valid(dn), "stepping stays on valid ids");
    }
    CHECK(g3_place_step(213, +1, -1, G3_PGAME_ALL) == 253, "up from NONE skips the hole");
    CHECK(g3_place_step(253, -1, -1, G3_PGAME_ALL) == 213, "down from EGG skips the hole");
    CHECK(g3_place_step(255, +1, -1, G3_PGAME_ALL) == 0,   "up from FATEFUL wraps to 0");
    CHECK(g3_place_step(0, -1, -1, G3_PGAME_ALL) == 255,   "down from 0 wraps to FATEFUL");
    /* scoped stepping stays inside its scope */
    CHECK(g3_place_step(87, +1, G3_RGN_HOENN, G3_PGAME_RS) == 0,
          "RS Hoenn wraps 87 -> 0, never into Kanto");
    CHECK(g3_place_step(87, +1, -1, G3_PGAME_EMERALD) == 197,
          "Emerald steps 87 -> its own extras, skipping Kanto");
    /* a scope with exactly one member cannot move */
    CHECK(g3_place_step(16, +1, G3_RGN_HOENN, G3_PGAME_FRLG) == 16,
          "an empty scope leaves the value alone");
    printf("(7) stepping ok\n");
  }

  /* (8) first-of-region, which is where "switch region" lands. */
  {
    CHECK(g3_place_first(G3_RGN_HOENN,   G3_PGAME_RS,      0xFFFF) == 0,   "Hoenn/RS -> 0");
    CHECK(g3_place_first(G3_RGN_KANTO,   G3_PGAME_FRLG,    0xFFFF) == 88,  "Kanto/FRLG -> 88");
    CHECK(g3_place_first(G3_RGN_SEVII,   G3_PGAME_FRLG,    0xFFFF) == 143, "Sevii/FRLG -> 143");
    CHECK(g3_place_first(G3_RGN_SPECIAL, G3_PGAME_ALL,     0xFFFF) == 213, "Special -> NONE");
    CHECK(g3_place_first(G3_RGN_HOENN,   G3_PGAME_FRLG,    0x1234) == 0x1234,
          "an empty region returns the caller's fallback");
    printf("(8) first-of-region ok\n");
  }

  /* (9) the name tables the pickers print are all in range. */
  {
    for (int r = 0; r < G3_RGN_COUNT; r++)  CHECK(g3_region_name(r)[0], "every region has a name");
    for (int s = 0; s < G3_PSORT_COUNT; s++) CHECK(g3_place_sort_name(s)[0], "every sort has a name");
    for (int g = 0; g < G3_PGAME_COUNT; g++) CHECK(g3_place_game_name(g)[0], "every game filter has a name");
    CHECK(strcmp(g3_region_name(-1), "?") == 0 &&
          strcmp(g3_region_name(G3_RGN_COUNT), "?") == 0, "out-of-range region name is \"?\"");
  }

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
