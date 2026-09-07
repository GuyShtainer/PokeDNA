/* Host test for the flags / counters / item-bag data layer.
 *   cc -std=c11 -I source tests/host_data_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_trainer.c source/gen3_items.c source/gen3_flags.c \
 *      source/gen3_edit.c source/gen3_daycare.c source/data_tables.c source/data_desc.c \
 *      -o /tmp/hd
 *   /tmp/hd tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_trainer.h"
#include "gen3_items.h"
#include "gen3_flags.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("parse FAILED\n"); return 1; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES], sb2[G3_SECTOR_DATA_SIZE];
  gen3_read_saveblock1(save, info.slot, sb1);
  int s0 = gen3_find_section(save, info.slot, 0);
  memcpy(sb2, save + (uint32_t)info.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE, G3_SECTOR_DATA_SIZE);

  PkMon party[6]; bool frlg = false;
  pk_read_party_auto(sb1, party, &frlg);
  PkGame g = frlg ? PK_FRLG : (info.version_guess == G3_VER_RS ? PK_RS : PK_EMERALD);
  printf("== %s ==  game=%d\n", path, g);

  /* (1) item bag: the Items pocket should hold real items (validates the offsets) */
  int valid = 0;
  for (int i = 0; i < pk_pocket_cap(g, POCKET_ITEMS); i++) {
    uint16_t id = pk_bag_item(sb1, g, POCKET_ITEMS, i);
    uint16_t q  = pk_bag_qty(sb1, sb2, g, POCKET_ITEMS, i);
    if (id == 0) continue;
    if (id <= 376 && q >= 1 && q <= 999) { if (valid < 4) printf("   %2d: %-14s x%u\n", i, pk_item_name(id), q); valid++; }
  }
  printf("(1) Items pocket: %d valid item slots\n", valid);
  CHECK(valid >= 1, "Items pocket decodes to real items (offsets + qty XOR correct)");

  /* (2) flags round-trip (restore after) */
  {
    int fn = 0x120;
    bool was = pk_flag_get(sb1, g, fn);
    pk_flag_set(sb1, g, fn, !was);
    CHECK(pk_flag_get(sb1, g, fn) == !was, "flag toggles");
    pk_flag_set(sb1, g, fn, was);
    CHECK(pk_flag_get(sb1, g, fn) == was, "flag restores");
  }

  /* (3) game-stat round-trip through the XOR (restore after) */
  {
    int stat = 5;                                   /* GAME_STAT_STEPS */
    uint32_t was = pk_game_stat(sb1, sb2, g, stat);
    pk_set_game_stat(sb1, sb2, g, stat, was + 12345);
    CHECK(pk_game_stat(sb1, sb2, g, stat) == was + 12345, "game stat write/read through XOR");
    pk_set_game_stat(sb1, sb2, g, stat, was);
    printf("(3) GAME_STAT[%d] '%s' = %u\n", stat, pk_game_stat_name(stat), was);
  }

  /* (4) named flags table: well-formed, in range, badge1 matches the known value,
   *     and a real named flag round-trips (restore after). */
  {
    const NamedFlag* nf; int nc = pk_named_flags(g, &nf);
    int N = pk_flags_count(g), reals = 0, headers = 0, first_real = -1;
    uint16_t exp_b1 = (g == PK_EMERALD) ? 0x867 : (g == PK_FRLG) ? 0x820 : 0x807;
    uint16_t got_b1 = 0;
    for (int i = 0; i < nc; i++) {
      if (nf[i].num == NAMED_FLAG_HEADER) { headers++; continue; }
      reals++;
      if (first_real < 0) first_real = i;
      CHECK(nf[i].num < N, "named flag number is in range");
      CHECK(nf[i].name && nf[i].name[0], "named flag has a label");
      if (got_b1 == 0) got_b1 = nf[i].num;          /* first real row == badge 1 */
    }
    printf("(4) named flags: %d rows (%d named, %d headers); badge1=0x%03X\n", nc, reals, headers, got_b1);
    CHECK(nc >= 10 && reals >= 8 && headers >= 1, "named-flag table is populated with categories");
    CHECK(got_b1 == exp_b1, "badge-1 flag number matches the known per-game value");
    if (first_real >= 0) {
      int fn = nf[first_real].num;
      bool was = pk_flag_get(sb1, g, fn);
      pk_flag_set(sb1, g, fn, !was);
      CHECK(pk_flag_get(sb1, g, fn) == !was, "named flag toggles");
      pk_flag_set(sb1, g, fn, was);
      CHECK(pk_flag_get(sb1, g, fn) == was, "named flag restores");
    }
  }

  /* (5) BACKLOG #2b: the Elite-Four question. Guy saw the E4 per-trainer flags OFF
   * despite having beaten them. Per pokeemerald's data/scripts/hall_of_fame.inc:7 (`call EverGrandeCity_HallOfFame_EventScript_ResetEliteFour`, read from assets/upstream/pokeemerald, reference only)
   * (ResetEliteFour), the HoF script clears all four member flags so the E4 can be
   * rechallenged -- that is GAME TRUTH, not a PokeDNA bug. What actually records
   * "you beat the league" is FLAG_SYS_GAME_CLEAR (0x864 for every one of the three
   * games -- SYSTEM_FLAGS+4, same base+offset in E/RS/FRLG). tools/gen_data.py
   * (commit 1b85e3e) already retitles the section "Elite Four (reset @HoF)" and adds
   * a "League beaten (HoF)" row for exactly this reason; this pins that the
   * generated table actually reflects it, and prints the live evidence from
   * whichever real save the corpus hands us (Emerald.sav sorts first, so this is
   * Guy's own Emerald file when the 5-game corpus is present -- see
   * tests/run_host_tests.py). */
  {
    const NamedFlag* nf; int nc = pk_named_flags(g, &nf);
    int elite_hdr = -1, game_clear_row = -1;
    for (int i = 0; i < nc; i++) {
      if (nf[i].num == NAMED_FLAG_HEADER && strstr(nf[i].name, "Elite Four")) { elite_hdr = i; break; }
    }
    CHECK(elite_hdr >= 0, "an \"Elite Four\" section exists in the named-flag table");
    if (elite_hdr >= 0) {
      CHECK(strstr(nf[elite_hdr].name, "HoF") != NULL,
            "the section is honestly annotated: rechallenge resets it (not a display bug)");
      for (int i = elite_hdr + 1; i < nc && nf[i].num != NAMED_FLAG_HEADER; i++) {
        if (strstr(nf[i].name, "League beaten") || strstr(nf[i].name, "HoF")) { game_clear_row = i; break; }
      }
      CHECK(game_clear_row >= 0,
            "a \"League beaten\"/HoF row sits in the Elite Four section (the flag that actually records the win)");
    }

    /* Evidence, printed unconditionally: no hard assertion on Guy's own save
     * progress (a fixture .sav that hasn't beaten the E4 yet must not fail this
     * test), but this is exactly the read the backlog item asked for -- "show
     * which flags are set". FLAG_SYS_GAME_CLEAR is 0x864 in Emerald (SYSTEM_FLAGS 0x860 + 0x4), 0x804 in R/S
     * (0x800 + 0x4) and 0x82C in FR/LG (SYS_FLAGS 0x800 + 0x2C) -- the table carries the
     * right per-game number, which is why this looks the row up by label (review D3). */
    int game_clear_num = -1;
    printf("(5) Elite Four section (evidence for BACKLOG #2b):\n");
    if (elite_hdr >= 0) {
      for (int i = elite_hdr + 1; i < nc && nf[i].num != NAMED_FLAG_HEADER; i++) {
        bool set = pk_flag_get(sb1, g, nf[i].num);
        printf("    0x%03X  %-20s %s\n", nf[i].num, nf[i].name, set ? "ON" : "off");
        if (strstr(nf[i].name, "League beaten")) game_clear_num = nf[i].num;
      }
    }
    if (game_clear_num >= 0) {
      bool cleared = pk_flag_get(sb1, g, game_clear_num);
      printf("    -> League beaten (HoF) = %s; per-member E4 flags are expected OFF once "
             "this is ON (the game resets them for a rematch)\n", cleared ? "ON" : "off");
    }
  }

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
