/* source/gb1_base_tbl.c + the GENERATED source/gb1_base_gen.c (tools/gen_gb1_base.py) --
 * the ROM-free Gen-1 base-stat/type/growth table CREATE > FROM SCRATCH uses (BACKLOG #276).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gb1base_test.c \
 *      source/gb1_base_tbl.c source/gb1_base_gen.c source/rom_gbbase.c source/rom_gbsprite.c \
 *      source/gb_sprite_codec.c -o /tmp/hgb1base && /tmp/hgb1base
 *
 * Three independent derivations: (1) hand-known facts (Mewtwo's Special is 154, Chansey's
 * HP is 250, Charizard is Fire/Flying = 0x14/0x02, Mew is mono-Psychic 0x18, Charmander's
 * Special is 50 not Gen 3's 60); (2) EVERY row 1..151 against the base-stat row a real
 * Red.gb and Yellow.gb cartridge carries, read through rom_gbbase.c (the table came from
 * the decomp text, the ROM rows from the game's own bytes -- a parser slip in the
 * generator shows up as a mismatch); (3) the accessor contract (bounds, NULL, "untouched
 * on false"). The ROMs are Guy's own dumps outside the repo: absent -> that half SKIPs.
 * The whole test SKIPs (not passes) when the generated table is not linked.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb1_base_tbl.h"
#include "rom_gbbase.h"
#include "rom_gbsprite.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { \
    printf("  !! FAIL: "); printf(__VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); g_fail++; } } while (0)

static void expect(const char* nm, uint16_t dex, uint8_t hp, uint8_t atk, uint8_t def,
                   uint8_t spe, uint8_t spc, uint8_t t1, uint8_t t2, uint8_t growth) {
  uint8_t r[GB1_BASE_ROW_LEN];
  bool ok = gb1_base_gen_row(dex, r);
  CHECK(ok, "%s: row %u resolves", nm, dex);
  if (!ok) return;
  const uint8_t want[GB1_BASE_ROW_LEN] = { hp, atk, def, spe, spc, t1, t2, growth };
  for (int i = 0; i < GB1_BASE_ROW_LEN; i++)
    CHECK(r[i] == want[i], "%s: field %d == %u (got %u)", nm, i, want[i], r[i]);
}

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
}

static uint8_t g_scratch[4096];

static int cross_check(const char* file) {
  char path[512]; snprintf(path, sizeof path, "%s/%s", ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return 0; }
  fseek(f, 0, SEEK_END); uint32_t sz = (uint32_t)ftell(f);
  RomGbSprite gs;
  int ok = rom_gbsprite_open(&gs, file_read, f, sz, g_scratch, sizeof g_scratch, GB_ROM_NONE);
  CHECK(ok && gs.gen == GB_ROM_GEN1, "%s opens as a Gen-1 ROM", file);
  int compared = 0;
  if (ok && gs.gen == GB_ROM_GEN1) {
    for (uint16_t d = 1; d <= 151; d++) {
      RomGb1Species sp; uint8_t r[GB1_BASE_ROW_LEN];
      bool a = rom_gbbase_gen1(&gs, file_read, f, d, &sp), b = gb1_base_gen_row(d, r);
      CHECK(a && b, "%s dex %u: ROM row and table row both resolve", file, d);
      if (!(a && b)) continue;
      compared++;
      CHECK(memcmp(r, sp.base.base, GB_NSTATS) == 0, "%s dex %u: base stats table==ROM", file, d);
      CHECK(r[5] == sp.base.type1 && r[6] == sp.base.type2, "%s dex %u: types table==ROM", file, d);
      CHECK(r[7] == sp.growth, "%s dex %u: growth table==ROM (%u vs %u)", file, d, r[7], sp.growth);
    }
  }
  fclose(f);
  printf("  %s: %d/151 rows cross-checked against the cartridge\n", file, compared);
  return compared;
}

int main(void) {
  if (!gb1_base_table_present()) {
    printf("SKIP (source/gb1_base_gen.c not linked -- run tools/gen_gb1_base.py)\n");
    return 0;
  }
  /* Hand-known: hp atk def spe spc | type1 type2 | growth (0 MEDIUM_FAST .. 3 MEDIUM_SLOW, 4 FAST, 5 SLOW) */
  expect("Bulbasaur (dual Grass/Poison)", 1, 45, 49, 49, 45, 65, 0x16, 0x03, 3);
  expect("Charmander (Spc 50, not Gen 3's 60)", 4, 39, 52, 43, 65, 50, 0x14, 0x14, 3);
  expect("Charizard (Fire/Flying)", 6, 78, 84, 78, 100, 85, 0x14, 0x02, 3);
  expect("Chansey (HP 250, mono Normal)", 113, 250, 5, 5, 50, 105, 0x00, 0x00, 4);
  expect("Mewtwo (Spc 154, mono Psychic)", 150, 106, 110, 90, 130, 154, 0x18, 0x18, 5);
  expect("Mew (mono Psychic)", 151, 100, 100, 100, 100, 100, 0x18, 0x18, 3);

  /* accessor contract */
  uint8_t r[GB1_BASE_ROW_LEN]; memset(r, 0xAA, sizeof r);
  CHECK(!gb1_base_gen_row(0, r) && !gb1_base_gen_row(152, r), "dex 0 and 152 refused");
  CHECK(r[0] == 0xAA && r[7] == 0xAA, "a refused lookup leaves `out` untouched");
  CHECK(!gb1_base_gen_row(1, NULL), "NULL out refused");
  GbNewMonSrc s; memset(&s, 0x5A, sizeof s);
  CHECK(gb1_base_table_fill(150, &s), "fill(150) ok");
  CHECK(s.base[GB_SPC] == 154 && s.base[GB_HP] == 106 && s.type1 == 0x18 && s.type2 == 0x18 && s.growth == 5,
        "fill(150) wrote Mewtwo's row");
  CHECK(s.moves[0] == 0x5A && s.ot_id == 0x5A5A && s.species_name == (const char*)0x5A5A5A5A5A5A5A5AULL,
        "fill leaves moves/names/ids alone");
  GbNewMonSrc z; memset(&z, 0x5A, sizeof z);
  CHECK(!gb1_base_table_fill(0, &z) && !gb1_base_table_fill(152, &z) && !gb1_base_table_fill(1, NULL),
        "fill refuses dex 0/152/NULL");
  CHECK(z.base[0] == 0x5A && z.type1 == 0x5A && z.growth == 0x5A, "a refused fill leaves src untouched");

  int n = cross_check("Red.gb") + cross_check("Yellow.gb");
  if (n == 0) printf("  (no cartridge dump present: the ROM cross-check half did not run)\n");

  printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_check, g_fail);
  return g_fail ? 1 : 0;
}
