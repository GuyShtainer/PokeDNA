/* Host test for the Pokéblock case core. The synthetic round-trip always runs; the
 * real-save sanity check runs only when a local Emerald .sav is present (portable). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "gen3_pokeblock.h"
#include "gen3_save.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void) {
  /* ---- synthetic round-trip (offset-agnostic) ---- */
  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  memset(sb1, 0xAB, sizeof sb1);                          /* so we can check the pad byte is preserved */
  PkPokeblock in = { 5, 10, 20, 30, 40, 50, 12 }, out;
  pk_pokeblock_set(sb1, PK_EMERALD, 3, &in);
  CHECK(pk_pokeblock_get(sb1, PK_EMERALD, 3, &out), "get after set");
  CHECK(out.color==5 && out.spicy==10 && out.dry==20 && out.sweet==30 &&
        out.bitter==40 && out.sour==50 && out.feel==12, "round-trip fields");
  uint32_t base = pk_pokeblock_offset(PK_EMERALD);
  CHECK(sb1[base + 3*PK_POKEBLOCK_STRIDE + 7] == 0xAB, "pad byte (7) preserved on set");
  CHECK(pk_pokeblock_occupied(&out), "occupied when color != 0");
  pk_pokeblock_clear(sb1, PK_EMERALD, 3);
  pk_pokeblock_get(sb1, PK_EMERALD, 3, &out);
  CHECK(!pk_pokeblock_occupied(&out), "empty after clear");
  CHECK(sb1[base + 3*PK_POKEBLOCK_STRIDE + 7] == 0, "clear zeroes the whole 8-byte slot");
  CHECK(pk_pokeblock_offset(PK_FRLG) == 0, "FRLG has no pokéblock case");
  CHECK(pk_pokeblock_offset(PK_RS) == 0x7F8 && pk_pokeblock_offset(PK_EMERALD) == 0x848, "per-game offsets");

  /* ---- real-save sanity (only if the local Emerald .sav is present) ---- */
  FILE* f = fopen("daycare map/POKEMON_EMER_BPEE00.sav", "rb");
  if (f) {
    static uint8_t save[G3_SAVE_FILE_SIZE];
    size_t n = fread(save, 1, sizeof save, f); fclose(f);
    Gen3SaveInfo gi;
    if (n >= G3_SAVE_FILE_SIZE && gen3_parse(save, (uint32_t)n, &gi) && gi.valid) {
      static uint8_t sb[G3_SAVEBLOCK1_BYTES];
      gen3_read_saveblock1(save, gi.slot, sb);
      int occ = 0, bad = 0;
      for (int i = 0; i < PK_POKEBLOCK_COUNT; i++) {
        PkPokeblock p; pk_pokeblock_get(sb, PK_EMERALD, i, &p);
        if (p.color > 14) bad++;                          /* wrong offset/stride -> garbage colors */
        if (pk_pokeblock_occupied(&p)) occ++;
      }
      printf("real sav: %d pokeblocks present, %d slots with a bad colour\n", occ, bad);
      CHECK(bad == 0, "all 40 slots have a sane colour (offset/stride correct)");
    } else printf("real sav: present but parse failed (skipped)\n");
  } else printf("real sav: absent (skipped)\n");

  printf("host_pokeblock_test: %s\n", fails ? "FAILURES" : "ALL PASS");
  return fails ? 1 : 0;
}
