/* Host test for the lossless edit core (gen3_edit.c).
 *   cc -std=c11 -I source tests/host_edit_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/data_tables.c -o /tmp/he
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
      /* BACKLOG #46: em_preview decodes into a local scratch[100] that is gone the
       * instant it returns -- pk_decode_mon() unconditionally sets m.raw to that
       * dead address, so em_preview must null it back out before handing `m` back. */
      CHECK(m.raw == NULL, "em_preview never hands out a dangling raw pointer");
    }
  }

  /* (4) MAX PP is PP Ups. A Gen-3 record stores no maximum: it stores a 2-bit PP-Up
   * count per slot and derives the maximum with CalculatePPWithBonus. So the editor's
   * "Max PP" row moves the Ups, and the invariant that matters for legality is that no
   * reachable state has a maximum outside the four the item can produce, or a current PP
   * above the maximum — gen3_legality2's check_moves flags exactly those. */
  {
    EditMon e; gen3_edit_load(sb1 + doff, true, &e);
    em_set_move(&e, 0, 57);                      /* Surf, base PP 15 */
    CHECK(em_get_ppups(&e, 0) == 0, "em_set_move clears the slot's PP Ups");
    CHECK(em_pp_max(57, 0, 0) == 15, "0 Ups -> base PP");
    printf("(4) Surf max PP by Ups: %u %u %u %u\n",
           (unsigned)em_pp_max(57, 0, 0), (unsigned)em_pp_max(57, 1, 0),
           (unsigned)em_pp_max(57, 2, 0), (unsigned)em_pp_max(57, 3, 0));
    CHECK(em_pp_max(57, 1, 0) == 18 && em_pp_max(57, 2, 0) == 21 &&
          em_pp_max(57, 3, 0) == 24, "Surf steps 15/18/21/24 with the Ups");

    /* raising the maximum carries CURRENT PP up by what the Up bought (the item's own
     * behaviour), and lowering it clamps current down to the new ceiling */
    em_set_pp(&e, 0, 15);
    em_set_ppups(&e, 0, 3);
    { PkMon m; em_preview(&e, &m);
      CHECK(m.pp[0] == 24, "full PP stays full when the maximum rises");
      CHECK(em_pp_max(m.moves[0], m.ppBonuses, 0) == 24, "maximum rose to 24"); }
    em_set_ppups(&e, 0, 1);
    { PkMon m; em_preview(&e, &m);
      CHECK(m.pp[0] == 18, "current PP clamps to the lowered maximum");
      CHECK(em_pp_max(m.moves[0], m.ppBonuses, 0) == 18, "maximum fell to 18"); }
    /* a partially-used move keeps its deficit rather than being topped up */
    em_set_ppups(&e, 0, 0);
    em_set_pp(&e, 0, 5);
    em_set_ppups(&e, 0, 3);
    { PkMon m; em_preview(&e, &m);
      CHECK(m.pp[0] == 14, "5/15 becomes 14/24 — the Up adds 9, it does not heal"); }

    /* the counter is two bits; nothing can push it past 3 */
    em_set_ppups(&e, 0, 200);
    CHECK(em_get_ppups(&e, 0) == 3, "PP Ups clamp at 3");
    em_set_ppups(&e, 5, 3);                      /* out-of-range slot is a no-op */
    CHECK(em_get_ppups(&e, 0) == 3, "an out-of-range slot changes nothing");

    /* an empty move slot can never hold Ups (the game clears them with the move) */
    em_set_move(&e, 3, 0);
    em_set_ppups(&e, 3, 3);
    CHECK(em_get_ppups(&e, 3) == 0, "an empty slot is forced to 0 Ups");
    CHECK(em_pp_max(0, 0xFF, 3) == 0, "an empty slot has no maximum");

    /* it survives the encrypt/decrypt round trip */
    uint8_t out[100]; gen3_edit_commit(&e, out);
    PkMon m; CHECK(pk_decode_mon(out, true, &m) && !m.isBadEgg, "PP-Up edit re-encodes");
    CHECK(((m.ppBonuses >> 0) & 3) == 3, "ppBonuses survives the commit");
    CHECK(m.pp[0] <= em_pp_max(m.moves[0], m.ppBonuses, 0), "current PP <= maximum");
  }

  /* (5) the invariant over the WHOLE move table: every reachable maximum is one of the
   * four the PP Up item can reach, and no slot's bits leak into another's. */
  {
    int worst = 0;
    for (uint16_t mv = 0; mv < 355; mv++) {
      uint8_t base = pk_move_pp(mv);
      for (int ups = 0; ups <= 3; ups++) {
        for (int slot = 0; slot < 4; slot++) {
          uint8_t bonuses = (uint8_t)(ups << (slot * 2));
          uint8_t got = em_pp_max(mv, bonuses, slot);
          uint8_t want = (uint8_t)(base + (unsigned)base * 20u * (unsigned)ups / 100u);
          if (got != want) { printf("  !! move %u ups %d slot %d: %u != %u\n",
                                    mv, ups, slot, got, want); g_fail++; }
          if (got > worst) worst = got;
          /* the other three slots read 0 Ups out of the same byte */
          for (int o = 0; o < 4; o++)
            if (o != slot) CHECK(em_pp_max(mv, bonuses, o) == base, "PP-Up bits do not cross slots");
        }
      }
    }
    printf("(5) highest reachable max PP over the whole move table: %d\n", worst);
    CHECK(worst == 64, "the ceiling is 40 base + 3 Ups = 64, the game's own maximum");
  }

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
