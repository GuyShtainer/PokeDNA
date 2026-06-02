/* Host test for the lossless edit core (gen3_edit.c).
 *   cc -std=c11 -I source tests/host_edit_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/data_tables.c -o /tmp/he
 *   /tmp/he tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

/* round-trip a record if it's a real, non-corrupt mon. returns 1 if tested. */
static int rt(const uint8_t* rec, bool party) {
  PkMon m;
  if (!pk_decode_mon(rec, party, &m)) return 0;   /* empty */
  if (m.isBadEgg) return 0;                        /* skip corrupt/hacked */
  if (!gen3_edit_roundtrip_ok(rec, party)) {
    printf("  !! ROUND-TRIP DIFF (%s) species=%u\n", party ? "party" : "box", m.species);
    g_fail++;
  }
  return 1;
}

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("parse FAILED\n"); return 1; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  static uint8_t pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);

  PkMon party[6]; bool frlg = false;
  pk_read_party_auto(sb1, party, &frlg);
  uint16_t coff = frlg ? 0x0034 : 0x0234, doff = frlg ? 0x0038 : 0x0238;
  uint8_t count = sb1[coff];
  if (count > 6) count = 6;

  printf("== %s ==\n", path);

  /* (1) lossless no-op round-trip for every valid party + box mon */
  int tested = 0;
  for (int i = 0; i < count; i++) tested += rt(sb1 + doff + (uint32_t)i * 100, true);
  for (int b = 0; b < G3_TOTAL_BOXES; b++)
    for (int s = 0; s < 30; s++)
      tested += rt(pc + 0x0004 + ((uint32_t)b * 30 + s) * 80, false);
  printf("(1) no-op round-trip: %d valid mons tested, %d diffs\n", tested, g_fail);

  if (count == 0) { printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail); return g_fail ? 1 : 0; }

  /* (2) single-field edits on party slot 0 -> decode back, assert new values + checksum valid */
  {
    const uint8_t* rec = sb1 + doff;
    EditMon e; gen3_edit_load(rec, true, &e);
    em_set_iv(&e, PK_HP, 31);
    em_set_iv(&e, PK_SPE, 0);
    em_set_ev(&e, PK_ATK, 252);
    em_set_item(&e, 197);            /* Lucky Egg */
    em_set_move(&e, 1, 57);          /* Surf */
    em_set_friendship(&e, 200);
    em_set_level(&e, 50);
    em_set_nickname(&e, "EDITTEST");
    uint8_t out[100]; gen3_edit_commit(&e, out);
    PkMon m; bool ok = pk_decode_mon(out, true, &m);
    printf("(2) single-field edits: species=%u Lv%u nick=\"%s\"\n", m.species, m.level, m.nickname);
    CHECK(ok && !m.isBadEgg, "edited mon decodes with valid checksum");
    CHECK(m.ivs[PK_HP] == 31, "IV HP = 31");
    CHECK(m.ivs[PK_SPE] == 0, "IV Spe = 0");
    CHECK(m.evs[PK_ATK] == 252, "EV Atk = 252");
    CHECK(m.heldItem == 197, "item = 197");
    CHECK(m.moves[1] == 57, "move[1] = 57");
    CHECK(m.friendship == 200, "friendship = 200");
    CHECK(m.level == 50, "level = 50");
    CHECK(strcmp(m.nickname, "EDITTEST") == 0, "nickname round-trips");
  }

  /* (3) PID reroll -> nature/shiny/gender match the request */
  {
    EditMon e; gen3_edit_load(sb1 + doff, true, &e);
    uint16_t sp = rd16(e.sub[0]);
    uint8_t ratio = pk_species_gender_ratio(sp);
    bool ok = em_reroll(&e, 5 /*Bold*/, 1 /*shiny*/, -1, ratio);
    CHECK(ok, "reroll found a Bold + shiny PID");
    if (ok) {
      PkMon m; em_preview(&e, &m);
      printf("(3) reroll: nature=%u shiny=%d (wanted Bold=5, shiny=1)\n", m.nature, m.isShiny);
      CHECK(m.nature == 5, "rerolled nature = Bold");
      CHECK(m.isShiny, "rerolled mon is shiny");
    }
  }

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
