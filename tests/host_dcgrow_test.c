/* Host test for the Day-Care take-out growth core (BACKLOG #373): gen3_dc_preview / gen3_dc_apply
 * in source/gen3_daycare.c + em_set_exp in source/gen3_edit.c. Pure C, no corpus needed.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_dcgrow_test.c source/gen3_daycare.c \
 *      source/gen3_edit.c source/learnsets2.c source/data_tables.c source/evolutions.c \
 *      source/gen3_mon.c source/gen3_save.c source/gen3_trainer.c -o /tmp/hdcg && /tmp/hdcg
 *
 * Internal ids: Bulbasaur 1 (Growth MEDIUM_SLOW; Emerald list L1 Tackle33 L4 Growl45 L7 LeechSeed73
 * L10 VineWhip22 L15 PoisonPowder77+SleepPowder79 L20 RazorLeaf75 L25 230 L32 74 L39 235 L46 76),
 * Deoxys 410 (the one species whose RS and Emerald lists differ: L10 Teleport100 vs DoubleTeam104). */
#include <stdio.h>
#include <string.h>
#include "gen3_daycare.h"
#include "gen3_edit.h"
#include "data_tables.h"

static int fails = 0, checks = 0;
static void eq(const char* what, long got, long want) {
  checks++;
  if (got != want) { printf("FAIL %s: got %ld want %ld\n", what, got, want); fails++; }
}

#define BULBA 1
#define DEOXYS 410

/* An 80-byte box record of `sp` at `lvl`; `mv` (up to 4, 0 = leave the build's own) REPLACES its
 * moves with explicit PP/PP-Ups so the tests control every byte the shift must carry. */
static void mk(uint16_t sp, uint8_t lvl, const uint16_t* mv, const uint8_t* pp, const uint8_t* ups,
               uint8_t out[80]) {
  gen3_build_mon(sp, lvl, 0x12345678u, 0xABCD1234u, "TEST", 3, out);
  if (!mv) return;
  EditMon e; gen3_edit_load(out, false, &e);
  for (int i = 0; i < 4; i++) {
    em_set_move(&e, i, mv[i]);
    if (ups) em_set_ppups(&e, i, ups[i]);
    if (pp) em_set_pp(&e, i, pp[i]);
  }
  gen3_edit_commit(&e, out);
}

static uint32_t steps_for(uint16_t sp, uint8_t from, uint8_t to) {
  uint8_t gr = pk_species_growth(sp);
  return pk_exp_for_level(gr, to) - pk_exp_for_level(gr, from);
}

int main(void) {
  uint8_t rec[80];
  DcGrow g;

  /* ---- growth: level + exp ---- */
  mk(BULBA, 5, NULL, NULL, NULL, rec);
  eq("preview ok", gen3_dc_preview(PK_EMERALD, rec, false, steps_for(BULBA, 5, 8), &g), 1);
  eq("lv_before", g.lv_before, 5);
  eq("lv_after", g.lv_after, 8);
  eq("exp_after", (long)g.exp_after, (long)pk_exp_for_level(pk_species_growth(BULBA), 8));
  eq("exp_before", (long)g.exp_before, (long)pk_exp_for_level(pk_species_growth(BULBA), 5));
  eq("species", g.species, BULBA);
  /* free slot: Tackle+Growl known, Leech Seed (L7) lands in slot 2 */
  eq("n_learn L5->8", g.n_learn, 1);
  eq("learn0 move", g.learn[0].newmove, 73);
  eq("learn0 replaced (free slot)", g.learn[0].replaced, 0);
  eq("learn0 at_level", g.learn[0].at_level, 7);
  eq("moves_after[2]", g.moves_after[2], 73);
  eq("moves_after[3] still empty", g.moves_after[3], 0);
  eq("new move base PP", g.pp_after[2], pk_move_pp(73));
  eq("no overflow", g.overflow, 0);

  /* steps 0: nothing changes */
  eq("preview steps0", gen3_dc_preview(PK_EMERALD, rec, false, 0, &g), 1);
  eq("steps0 lv", g.lv_after, 5);
  eq("steps0 exp", (long)g.exp_after, (long)g.exp_before);
  eq("steps0 n_learn", g.n_learn, 0);
  /* steps that stay inside the level: exp grows, level and moves do not */
  gen3_dc_preview(PK_EMERALD, rec, false, 1, &g);
  eq("1 step exp", (long)g.exp_after, (long)g.exp_before + 1);
  eq("1 step lv", g.lv_after, 5);

  /* Lv 100 untouched */
  uint8_t r100[80]; mk(BULBA, 100, NULL, NULL, NULL, r100);
  eq("preview L100", gen3_dc_preview(PK_EMERALD, r100, false, 99999, &g), 1);
  eq("L100 lv", g.lv_after, 100);
  eq("L100 exp unchanged", (long)g.exp_after, (long)g.exp_before);
  eq("L100 n_learn", g.n_learn, 0);

  /* a Lv-100 record whose stored exp is ABOVE the level-100 total is left exactly as found (retail's
   * "!= MAX_LEVEL" guard; clamping it down would be a change retail never makes) */
  { EditMon oe; gen3_edit_load(r100, false, &oe);
    em_set_exp(&oe, pk_exp_for_level(pk_species_growth(BULBA), 100) + 500);
    uint8_t ro[80]; gen3_edit_commit(&oe, ro);
    gen3_dc_preview(PK_EMERALD, ro, false, 1000, &g);
    eq("L100 over-cap exp untouched", (long)g.exp_after, (long)g.exp_before); }

  /* exp cap: Lv 99 + a huge step count (also proves the 64-bit sum: 0xFFFFFFFF would wrap) */
  uint8_t r99[80]; mk(BULBA, 99, NULL, NULL, NULL, r99);
  gen3_dc_preview(PK_EMERALD, r99, false, 0xFFFFFFFFu, &g);
  eq("cap exp", (long)g.exp_after, (long)pk_exp_for_level(pk_species_growth(BULBA), 100));
  eq("cap lv", g.lv_after, 100);

  /* ---- shift: four known moves, oldest out, PP/PP-Ups carried ---- */
  const uint16_t four[4] = { 1, 2, 3, 4 };        /* Pound, Karate Chop, Double Slap, Comet Punch: none in Bulbasaur's list */
  const uint8_t  pp4[4]  = { 3, 4, 5, 6 };
  const uint8_t  up4[4]  = { 0, 1, 2, 3 };
  uint8_t rs[80]; mk(BULBA, 5, four, pp4, up4, rs);
  EditMon chk; gen3_edit_load(rs, false, &chk);
  for (int i = 0; i < 4; i++) { eq("seed ppups", em_get_ppups(&chk, i), up4[i]); eq("seed pp", chk.sub[1][8 + i], pp4[i]); }
  gen3_dc_preview(PK_EMERALD, rs, false, steps_for(BULBA, 5, 8), &g);
  eq("shift n_learn", g.n_learn, 1);
  eq("shift replaced = oldest", g.learn[0].replaced, 1);
  eq("shift m0", g.moves_after[0], 2); eq("shift m1", g.moves_after[1], 3);
  eq("shift m2", g.moves_after[2], 4); eq("shift m3", g.moves_after[3], 73);
  eq("shift pp0 carried", g.pp_after[0], 4); eq("shift pp1 carried", g.pp_after[1], 5);
  eq("shift pp2 carried", g.pp_after[2], 6); eq("shift pp3 base", g.pp_after[3], pk_move_pp(73));
  eq("shift ups0 carried", g.ppups_after[0], 1); eq("shift ups1 carried", g.ppups_after[1], 2);
  eq("shift ups2 carried", g.ppups_after[2], 3); eq("shift ups3 zero", g.ppups_after[3], 0);

  /* a level that teaches two moves (L15: PoisonPowder 77 + Sleep Powder 79), both shift */
  gen3_dc_preview(PK_EMERALD, rs, false, steps_for(BULBA, 5, 15), &g);
  /* L7 LeechSeed, L10 VineWhip, then both L15 moves: 4 events, each at its own level */
  eq("two-at-15 n_learn", g.n_learn, 4);
  eq("ev2 at_level", g.learn[2].at_level, 15); eq("ev3 at_level", g.learn[3].at_level, 15);
  eq("ev2 move", g.learn[2].newmove, 77); eq("ev3 move", g.learn[3].newmove, 79);
  eq("ev2 replaced", g.learn[2].replaced, 3);    /* after 2 shifts the window is 3,4,73,22 -> 3 out */
  eq("ev3 replaced", g.learn[3].replaced, 4);
  eq("final m0", g.moves_after[0], 73); eq("final m1", g.moves_after[1], 22);
  eq("final m2", g.moves_after[2], 77); eq("final m3", g.moves_after[3], 79);

  /* known move is skipped: carry Leech Seed already */
  const uint16_t known[4] = { 73, 1, 2, 3 };
  uint8_t rk[80]; mk(BULBA, 5, known, NULL, NULL, rk);
  gen3_dc_preview(PK_EMERALD, rk, false, steps_for(BULBA, 5, 8), &g);
  eq("known skipped", g.n_learn, 0);
  eq("known m0 unchanged", g.moves_after[0], 73);

  /* overflow: L5 -> L100 is 9 events (7,10,15,15,20,25,32,39,46); 8 recorded, count keeps going */
  gen3_dc_preview(PK_EMERALD, rec, false, 0xFFFFFFF0u, &g);
  eq("ovf lv", g.lv_after, 100);
  eq("ovf n_learn", g.n_learn, 9);
  eq("ovf flag", g.overflow, 1);
  eq("ovf learn[7] recorded", g.learn[7].newmove, 235);
  eq("ovf final m0", g.moves_after[0], 230);   /* learn[8] is never written: it would land on moves_after */
  eq("ovf final m1", g.moves_after[1], 74);
  eq("ovf final m2", g.moves_after[2], 235);
  eq("ovf final m3", g.moves_after[3], 76);

  /* ---- learnset_game: RS vs Emerald differ for Deoxys at L10 ---- */
  uint8_t rd[80]; mk(DEOXYS, 5, NULL, NULL, NULL, rd);
  DcGrow ge;
  gen3_dc_preview(PK_RS, rd, false, steps_for(DEOXYS, 5, 10), &g);
  gen3_dc_preview(PK_EMERALD, rd, false, steps_for(DEOXYS, 5, 10), &ge);
  eq("RS learns Teleport", g.learn[0].newmove, 100);
  eq("Emerald learns Double Team", ge.learn[0].newmove, 104);

  /* ---- apply ---- */
  gen3_dc_preview(PK_EMERALD, rs, false, steps_for(BULBA, 5, 8), &g);
  EditMon e; gen3_edit_load(rs, false, &e);
  gen3_dc_apply(&e, &g, false);
  eq("keep: exp written", (long)(e.sub[0][4] | e.sub[0][5] << 8 | e.sub[0][6] << 16 | (long)e.sub[0][7] << 24), (long)g.exp_after);
  eq("keep: attacks substruct identical", memcmp(e.sub[1], chk.sub[1], 12), 0);
  eq("keep: ppups byte identical", e.sub[0][8], chk.sub[0][8]);

  gen3_edit_load(rs, false, &e);
  gen3_dc_apply(&e, &g, true);
  for (int i = 0; i < 4; i++) {
    eq("learn: move", e.sub[1][i * 2] | e.sub[1][i * 2 + 1] << 8, g.moves_after[i]);
    eq("learn: pp exact (no PP-Up double count)", e.sub[1][8 + i], g.pp_after[i]);
    eq("learn: ppups", em_get_ppups(&e, i), g.ppups_after[i]);
  }
  uint8_t back[80]; gen3_edit_commit(&e, back);
  gen3_edit_load(back, false, &e);
  eq("learn: survives commit/reload pp3", e.sub[1][11], pk_move_pp(73));

  /* party record: plaintext level + stats follow the grown exp */
  uint8_t rp[100]; memset(rp, 0, sizeof rp);
  { EditMon pe; gen3_edit_load(rec, false, &pe); em_set_party_flag(&pe, true); gen3_edit_commit(&pe, rp); }
  eq("party preview", gen3_dc_preview(PK_EMERALD, rp, true, steps_for(BULBA, 5, 8), &g), 1);
  EditMon pe; gen3_edit_load(rp, true, &pe);
  gen3_dc_apply(&pe, &g, true);
  eq("party level byte", pe.raw[0x54], 8);

  /* ---- refusals ---- */
  uint8_t egg[80]; mk(BULBA, 5, NULL, NULL, NULL, egg);
  { EditMon ee; gen3_edit_load(egg, false, &ee); em_set_egg(&ee, true); gen3_edit_commit(&ee, egg); }
  eq("egg refused", gen3_dc_preview(PK_EMERALD, egg, false, 1000, &g), 0);
  uint8_t nosp[80]; mk(BULBA, 5, NULL, NULL, NULL, nosp);
  { EditMon ne; gen3_edit_load(nosp, false, &ne); em_set_species(&ne, 0); gen3_edit_commit(&ne, nosp); }
  eq("species 0 refused", gen3_dc_preview(PK_EMERALD, nosp, false, 1000, &g), 0);
  eq("NULL rec refused", gen3_dc_preview(PK_EMERALD, NULL, false, 1, &g), 0);

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
