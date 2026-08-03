/* Host (PC) test for gen3_fly — the Fly-destination (visited-town) flags.
 *
 * What this pins down (every one of these was a real trap found in research):
 *   1) Emerald has SEVENTEEN Fly destinations, not sixteen — the Battle Frontier
 *      uses a different flag family (0x8A8) far outside the contiguous town run;
 *   2) FRLG is a different FAMILY (FLAG_WORLD_MAP_*, 0x890..0x8A3), not "Emerald
 *      minus 0x60", and its run STOPS at 0x8A3 (0x8A4+ are dungeon preview flags);
 *   3) FRLG lists SEVEN ISLAND before SIX ISLAND;
 *   4) no row may exceed the game's real flags[] bound, or pk_flag_set no-ops;
 *   5) "mark all" must never silently turn on the Battle Frontier / Battle Tower;
 *   6) writing a Hoenn table into an FRLG save (or vice versa) is the only way this
 *      corrupts anything, so every access goes through the per-game table.
 *
 * Build + run (from the repo root):
 *   cc -I source tests/host_fly_test.c source/gen3_fly.c source/gen3_flags.c \
 *      source/gen3_save.c -o /tmp/hfly && /tmp/hfly [path/to/emerald.sav]
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_fly.h"
#include "gen3_flags.h"
#include "gen3_save.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

static uint8_t g_sb1[G3_SAVEBLOCK1_BYTES];
static uint8_t g_buf[G3_SAVE_FILE_SIZE];

static int count_kind(PkGame g, int kind) {
  const G3FlyDest* t; int n = g3fly_list(g, &t), c = 0;
  for (int i = 0; i < n; i++) if (t[i].kind == kind) c++;
  return c;
}

int main(int argc, char** argv) {
  /* ---- 1. table shape per game ---- */
  {
    const G3FlyDest* t;
    int ne = g3fly_list(PK_EMERALD, &t);
    CHECK(ne == 18, "Emerald: 16 towns + Frontier + League = 18 rows");
    CHECK(count_kind(PK_EMERALD, G3FLY_TOWN) == 16, "Emerald: 16 town rows");
    CHECK(count_kind(PK_EMERALD, G3FLY_FACILITY) == 1, "Emerald: Battle Frontier is its own kind");
    CHECK(count_kind(PK_EMERALD, G3FLY_CURSOR) == 1, "Emerald: League is cursor-only");
    /* the 17th flyable destination is NOT contiguous with the town run */
    CHECK(t[16].flag == 0x08A8, "Emerald: Battle Frontier = 0x8A8");
    CHECK(t[15].flag == 0x087E, "Emerald: last town = Ever Grande 0x87E");
    CHECK(t[16].flag - t[15].flag != 1, "Emerald: Frontier is NOT adjacent to the town run");

    int nr = g3fly_list(PK_RS, &t);
    CHECK(nr == 18, "R/S: 18 rows");
    CHECK(t[0].flag == 0x080F, "R/S: Littleroot = 0x80F (base 0x800, not Emerald's 0x860)");
    CHECK(t[16].flag == 0x0848, "R/S: Battle Tower = 0x848");

    int nf = g3fly_list(PK_FRLG, &t);
    CHECK(nf == 22, "FRLG: 20 destinations + 2 Sevii prereqs = 22 rows");
    CHECK(count_kind(PK_FRLG, G3FLY_TOWN) == 20, "FRLG: 20 town rows");
    CHECK(count_kind(PK_FRLG, G3FLY_PREREQ) == 2, "FRLG: 2 prerequisite rows");
    CHECK(t[0].flag == 0x0890, "FRLG: Pallet = 0x890 (a different flag FAMILY)");
    /* the run is contiguous 0x890..0x8A3 and stops there */
    for (int i = 0; i < 20; i++)
      CHECK(t[i].flag == (uint16_t)(0x0890 + i), "FRLG: world-map run is contiguous 0x890..0x8A3");
    CHECK(t[19].flag == 0x08A3, "FRLG: run STOPS at 0x8A3 (0x8A4+ are dungeon previews)");
    /* SEVEN before SIX */
    CHECK(t[16].flag == 0x08A0 && strcmp(t[16].name, "Seven Island") == 0, "FRLG: 0x8A0 is SEVEN Island");
    CHECK(t[17].flag == 0x08A1 && strcmp(t[17].name, "Six Island") == 0, "FRLG: 0x8A1 is SIX Island");
    CHECK(t[20].flag == 0x0845 && t[21].flag == 0x0846, "FRLG: Sevii map prereqs 0x845 / 0x846");

    CHECK(g3fly_list((PkGame)99, &t) == 0, "unknown game -> no rows");
  }

  /* ---- 2. every row is inside the game's real flags[] bound ---- */
  {
    for (int gi = 0; gi < 3; gi++) {
      PkGame g = (PkGame)gi;
      const G3FlyDest* t; int n = g3fly_list(g, &t);
      int lim = pk_flags_count(g);
      for (int i = 0; i < n; i++)
        CHECK(t[i].flag < lim, "every fly flag is inside the game's flags[] array");
      /* names must be non-empty and short enough for the 8px UI rows */
      for (int i = 0; i < n; i++) {
        CHECK(t[i].name && t[i].name[0], "every row has a name");
        CHECK(strlen(t[i].name) <= 20, "row name fits the list column");
      }
      /* no duplicate flag numbers inside a game */
      for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
          CHECK(t[i].flag != t[j].flag, "no duplicate flag in a game's table");
    }
  }

  /* ---- 3. get/set round-trips against the real bit layout ---- */
  {
    for (int gi = 0; gi < 3; gi++) {
      PkGame g = (PkGame)gi;
      const G3FlyDest* t; int n = g3fly_list(g, &t);
      memset(g_sb1, 0, sizeof g_sb1);
      for (int i = 0; i < n; i++) CHECK(!g3fly_get(g_sb1, g, i), "starts off");
      for (int i = 0; i < n; i++) {
        g3fly_set(g_sb1, g, i, true);
        CHECK(g3fly_get(g_sb1, g, i), "set then get");
        /* setting one row must not disturb any other */
        for (int j = 0; j < n; j++)
          if (j != i) CHECK(!g3fly_get(g_sb1, g, j) || j < i, "no neighbouring row flipped");
      }
      /* and the raw flag agrees with the table */
      for (int i = 0; i < n; i++)
        CHECK(pk_flag_get(g_sb1, g, t[i].flag), "raw flag matches the row");
      for (int i = 0; i < n; i++) g3fly_set(g_sb1, g, i, false);
      for (int i = 0; i < n; i++) CHECK(!g3fly_get(g_sb1, g, i), "cleared again");
      /* out-of-range rows are inert, not memory-corrupting */
      g3fly_set(g_sb1, g, -1, true);
      g3fly_set(g_sb1, g, n, true);
      g3fly_set(g_sb1, g, 9999, true);
      CHECK(!g3fly_get(g_sb1, g, -1) && !g3fly_get(g_sb1, g, n), "out-of-range rows inert");
    }
  }

  /* ---- 4. mark-all: towns + prereqs only, never the facility ---- */
  {
    for (int gi = 0; gi < 3; gi++) {
      PkGame g = (PkGame)gi;
      const G3FlyDest* t; int n = g3fly_list(g, &t);
      memset(g_sb1, 0, sizeof g_sb1);
      int changed = g3fly_mark_all(g_sb1, g);
      int expect = count_kind(g, G3FLY_TOWN) + count_kind(g, G3FLY_PREREQ);
      CHECK(changed == expect, "mark_all reports every bit it flipped");
      for (int i = 0; i < n; i++) {
        bool on = g3fly_get(g_sb1, g, i);
        if (t[i].kind == G3FLY_TOWN || t[i].kind == G3FLY_PREREQ)
          CHECK(on, "mark_all turned on every town/prereq");
        else
          CHECK(!on, "mark_all left the facility/cursor rows alone");
      }
      /* idempotent: a second pass changes nothing */
      CHECK(g3fly_mark_all(g_sb1, g) == 0, "mark_all is idempotent");
      int tot = 0, on = g3fly_count_on(g_sb1, g, &tot);
      CHECK(on == tot && tot == count_kind(g, G3FLY_TOWN), "count_on counts towns only");
    }
  }

  /* ---- 5. the Fly badge gate ---- */
  {
    CHECK(g3fly_badge_flag(PK_EMERALD) == 0x086C, "Emerald: Fly needs the Feather Badge (0x86C)");
    CHECK(g3fly_badge_flag(PK_RS)      == 0x080C, "R/S: Feather Badge 0x80C");
    CHECK(g3fly_badge_flag(PK_FRLG)    == 0x0822, "FRLG: Fly needs the Thunder Badge (0x822)");
    memset(g_sb1, 0, sizeof g_sb1);
    CHECK(!g3fly_badge_ok(g_sb1, PK_EMERALD), "no badge -> Fly unusable");
    pk_flag_set(g_sb1, PK_EMERALD, 0x086C, true);
    CHECK(g3fly_badge_ok(g_sb1, PK_EMERALD), "badge set -> gate passes");
    /* the badge flag must not collide with any destination row */
    for (int gi = 0; gi < 3; gi++) {
      PkGame g = (PkGame)gi;
      const G3FlyDest* t; int n = g3fly_list(g, &t);
      for (int i = 0; i < n; i++) CHECK(t[i].flag != g3fly_badge_flag(g), "badge flag is not a destination row");
    }
  }

  /* ---- 6. the Mauville side effect is surfaced ---- */
  {
    const G3FlyDest* t; g3fly_list(PK_EMERALD, &t);
    for (int i = 0; i < 18; i++)
      CHECK(g3fly_extra_effect(PK_EMERALD, i) == (t[i].flag == 0x0878),
            "only Mauville reports an extra effect (Record Corner)");
    CHECK(!g3fly_extra_effect(PK_FRLG, 0), "FRLG has no such row");
  }

  /* ---- 7. cross-game safety: Emerald numbers must not appear in the R/S table ---- */
  {
    const G3FlyDest *e, *r; int ne = g3fly_list(PK_EMERALD, &e), nr = g3fly_list(PK_RS, &r);
    for (int i = 0; i < ne; i++)
      for (int j = 0; j < nr; j++)
        CHECK(e[i].flag != r[j].flag, "no Emerald flag number is reused by R/S");
    (void)nr;
  }

  /* ---- 8. a REAL save: do the flags already match the towns actually visited? ----
   * This is the zero-cost validation of the whole numbering: on a genuine save the
   * visited bits must be a plausible prefix of progress, and the player's own game
   * must resolve. A wrong base would show 0 or 16 of 16. */
  {
    const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
    FILE* f = fopen(path, "rb");
    if (!f) {
      printf("(skipped real-save checks: %s not found)\n", path);
    } else {
      uint32_t n = (uint32_t)fread(g_buf, 1, sizeof g_buf, f);
      fclose(f);
      Gen3SaveInfo info;
      if (n >= G3_SLOT_BYTES && gen3_parse(g_buf, n, &info) && info.valid && info.sb1_ok) {
        /* reassemble SaveBlock1 from its four sections */
        memset(g_sb1, 0, sizeof g_sb1);
        for (int sid = G3_SID_SAVEBLOCK1_START; sid <= G3_SID_SAVEBLOCK1_END; sid++) {
          int s = gen3_find_section(g_buf, info.slot, sid);
          if (s < 0) continue;
          memcpy(g_sb1 + (uint32_t)(sid - 1) * G3_SECTOR_DATA_SIZE,
                 g_buf + (uint32_t)info.slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE,
                 G3_SECTOR_DATA_SIZE);
        }
        PkGame g = (info.version_guess == G3_VER_EMERALD) ? PK_EMERALD : PK_RS;
        const G3FlyDest* t; int nrows = g3fly_list(g, &t);
        int tot = 0, on = g3fly_count_on(g_sb1, g, &tot);
        printf("save \"%s\": %d/%d towns visited, Fly badge %s\n",
               info.trainer_name, on, tot, g3fly_badge_ok(g_sb1, g) ? "YES" : "no");
        printf("  ");
        for (int i = 0; i < nrows; i++)
          if (g3fly_get(g_sb1, g, i)) printf("%s, ", t[i].name);
        printf("\n");
        /* A real save should have visited SOME towns, and Littleroot (the starting
         * town) is set by the opening sequence on every save that got out the door. */
        CHECK(on > 0, "real save: at least one town visited");
        CHECK(on <= tot, "real save: visited count cannot exceed the table");
        CHECK(g3fly_get(g_sb1, g, 0), "real save: Littleroot (the home town) is visited");
      } else {
        printf("(skipped real-save checks: %s did not parse)\n", path);
      }
    }
  }

  if (g_fail == 0) printf("OK: host_fly_test (0 failures)\n");
  else             printf("host_fly_test: %d FAILURE(S)\n", g_fail);
  return g_fail ? 1 : 0;
}
