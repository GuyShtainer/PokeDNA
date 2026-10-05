/* museum_fill.c -- host fixture builder for BACKLOG #408 (not shipped).
 * Fill the EMPTY museum records (a real painting is kept) of an Emerald or Ruby/Sapphire
 * save from party[0] via gc_museum_set_raw (the same
 * writer pdna_contest.c/gen3_stars.c use), optionally set FLAG_*_PAINTING_MADE
 * (0xA0..0xA4) too, and warp the player to Lilycove Museum 2F (group 13, map 3)
 * with g3warp_apply. Writes a fresh .sav with valid section checksums.
 * usage: museum_fill IN.sav OUT.sav flags(0|1|core|clear) [x y]
 *   flags=0: no extra flag writes (since #408 gc_museum_set_raw sets the flag itself,
 *            so 0 and core now behave the same)
 *   flags=1: manually set flags after gc_museum_set_raw
 *   flags=core: let gc_museum_set_raw set flags
 *   flags=clear: fill exactly like core, then clear 0xA0..0xA4 for ALL five categories (incl. KEPT
 *            real wins) = a pre-#408 save (records without PAINTING_MADE flags), BACKLOG #422 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_contest.h"
#include "gen3_flags.h"
#include "gen3_warp.h"

static uint8_t g_save[262144];
static uint8_t g_sb1[G3_SAVEBLOCK1_BYTES];
static uint8_t g_sb2[G3_SECTOR_DATA_SIZE];

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s IN.sav OUT.sav flags(0|1|core|clear) [x y]\n", argv[0]); return 2; }
  int want_flags = 0;
  bool clear_flags = (strcmp(argv[3], "clear") == 0);
  if (strcmp(argv[3], "core") == 0 || clear_flags) {
    want_flags = -1;  /* -1 means "don't manually touch flags, gc_museum_set_raw does it" */
  } else {
    want_flags = atoi(argv[3]);
  }
  int wx = argc > 5 ? atoi(argv[4]) : 11, wy = argc > 5 ? atoi(argv[5]) : 7;
  FILE* f = fopen(argv[1], "rb"); if (!f) { perror("in"); return 1; }
  size_t sz = fread(g_save, 1, sizeof g_save, f); fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(g_save, (uint32_t)sz, &info)) { fprintf(stderr, "parse failed\n"); return 1; }
  PkGame game;
  if (info.version_guess == G3_VER_EMERALD) game = PK_EMERALD;
  else if (info.version_guess == G3_VER_RS) game = PK_RS;
  else { fprintf(stderr, "not Emerald/RS (guess %d)\n", info.version_guess); return 1; }
  if (!gen3_read_saveblock1(g_save, info.slot, g_sb1)) { fprintf(stderr, "sb1 read failed\n"); return 1; }
  int s0 = gen3_find_section(g_save, info.slot, 0);
  if (s0 < 0) { fprintf(stderr, "no section 0\n"); return 1; }
  memcpy(g_sb2, g_save + (uint32_t)info.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE, G3_SECTOR_DATA_SIZE);   /* s0 is a SECTOR INDEX */

  PkMon party[6]; bool frlg = false;
  int n = pk_read_party_auto(g_sb1, party, &frlg);
  if (n < 1 || frlg) { fprintf(stderr, "no party / frlg\n"); return 1; }
  printf("slot %d, party[0] = species %u '%s' pid %08X otId %08X\n", info.slot, party[0].species,
         party[0].nickname, party[0].personality, party[0].otId);
  for (int cat = 0; cat < GC_CATEGORY_COUNT; cat++) {
    const PkMon* d = &party[cat % n];
    bool flag_before = pk_flag_get(g_sb1, game, 0xA0 + cat);
    GcWinner prev; gc_museum_get(g_sb1, game, cat, &prev);
    if (prev.species) { printf("  museum cat %d KEPT real painting species %u '%s' / '%s', flag 0x%02X=%d\n", cat, prev.species,
                               prev.monName, prev.trainerName, 0xA0 + cat, flag_before); continue; }
    if (!gc_museum_set_raw(g_sb1, game, cat, d->species, d->personality, d->otId,
                           d->nickname, g_sb2 + SB2_OFF_PLAYER_NAME)) { fprintf(stderr, "museum set %d failed\n", cat); return 1; }
    GcWinner w; gc_museum_get(g_sb1, game, cat, &w);
    printf("  museum cat %d <- species %u (re-read %u)  flag 0x%02X before=%d", cat, d->species, w.species, 0xA0 + cat,
           flag_before);
    if (want_flags > 0) pk_flag_set(g_sb1, game, 0xA0 + cat, true);
    bool flag_after = pk_flag_get(g_sb1, game, 0xA0 + cat);
    printf(" after=%d%s\n", flag_after, want_flags == -1 ? (clear_flags ? " (clear mode: set by the core, cleared below)" : " (core mode: gc_museum_set_raw set the flag)") : "");
  }
  if (clear_flags)
    for (int cat = 0; cat < GC_CATEGORY_COUNT; cat++) {
      pk_flag_set(g_sb1, game, 0xA0 + cat, false);
      printf("  clear mode: flag 0x%02X now %d\n", 0xA0 + cat, pk_flag_get(g_sb1, game, 0xA0 + cat));
    }
  G3Warp w = { .group = 13, .num = 3, .x = (int16_t)wx, .y = (int16_t)wy, .centre = false };
  G3Warp cur; if (g3warp_get_current(g_sb1, &cur)) printf("current map %u/%u pos %d,%d\n", cur.group, cur.num, cur.x, cur.y);
  if (!g3warp_apply(g_sb1, g_sb2, game, &w)) { fprintf(stderr, "warp apply failed\n"); return 1; }
  G3Warp pend; if (g3warp_get_pending(g_sb1, g_sb2, game, &pend)) printf("pending warp %u/%u pos %d,%d\n", pend.group, pend.num, pend.x, pend.y);

  for (int id = 1; id <= 4; id++)
    if (gen3_write_full_section(g_save, info.slot, id, g_sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE) == 0xFFFFFFFFu) { fprintf(stderr, "write sec %d failed\n", id); return 1; }
  if (gen3_write_full_section(g_save, info.slot, 0, g_sb2) == 0xFFFFFFFFu) { fprintf(stderr, "write sec 0 failed\n"); return 1; }
  int fail = -1;
  if (!gen3_verify_full_checksums(g_save, info.slot, &fail)) { fprintf(stderr, "checksum fail sec %d\n", fail); return 1; }
  if (!gen3_section_checksum_ok(g_save, info.slot, 0, game == PK_RS ? 0x890 : 0xF2C)) { fprintf(stderr, "sec0 checksum bad at the game's SaveBlock2 length\n"); return 1; }
  f = fopen(argv[2], "wb"); fwrite(g_save, 1, sz, f); fclose(f);
  const char* flags_mode = clear_flags ? "clear" : (want_flags == -1) ? "core" : (want_flags ? "manual" : "none");
  printf("wrote %s (%zu B), game=%s, flags=%s, warp 13/3 @%d,%d\n", argv[2], sz, game == PK_RS ? "RS" : "Emerald", flags_mode, wx, wy);
  return 0;
}
