/*
 * bank_plant -- PDNA_DELTA-only synthetic native Bank cells (BACKLOG #150 S150-2
 * step 6). See bank_plant.h for the contract. The whole body sits under
 * #ifdef PDNA_DELTA so no symbol from this file exists in either gate build.
 */
#include "bank_plant.h"

#ifdef PDNA_DELTA
#include <string.h>

#include "gb_new_mon.h"
#include "bank_cell.h"
#include "data_tables.h"   /* pk_species_name */

#define PLANT_DEX_CHIKORITA 152
#define PLANT_DEX_PIKACHU    25

/* A Gen-2 CHIKORITA at `level`, freshly built (no ROM needed -- gb_new_mon does no
 * ROM I/O, GbNewMonSrc supplies everything it needs by hand). `seed` is gb_new_mon's
 * PID/IV seed, distinct per call so two plant slots never look byte-identical before
 * bc_pack's own bank_serial makes them so anyway. */
static void plant_gen2_chikorita(GbEditMon* e, uint8_t level, uint32_t seed) {
  GbNewMonSrc src; memset(&src, 0, sizeof src);
  src.growth = gb_growth_rate(PLANT_DEX_CHIKORITA);   /* cross-checked inside gb_new_mon */
  src.moves[0] = 33;                                  /* Tackle -- valid in every game */
  src.species_name = pk_species_name(PLANT_DEX_CHIKORITA);
  src.ot_name = "PLANT";
  src.ot_id = 12345;
  gb_new_mon(GB_GEN2, PLANT_DEX_CHIKORITA, level, &src, seed, e);
}

/* BACKLOG #150 S150-15 decision 13: the exact bytes bank_plant_box0() plants at slot
 * 0 -- extracted so xfer_plant.c can convert the SAME cell (not a fresh, potentially
 * divergent one) into the DOWN arm's shot-chain fixture. */
void bank_plant_cell0(uint8_t out80[80]) {
  if (!out80) return;
  GbEditMon e;
  plant_gen2_chikorita(&e, 12, 1u);
  bc_pack(&e, 0, BC_ORIGIN_GOLD, 0, 1u, out80);
}

void bank_plant_box0(uint8_t* recs) {
  if (!recs) return;
  GbEditMon e;
  uint8_t cell[80];

  /* slot 0: Gen-2 CHIKORITA @ L12, origin GOLD -> FULL */
  bank_plant_cell0(cell);
  memcpy(recs + (uint32_t)0 * 80, cell, 80);

  /* slot 1: Gen-1 PIKACHU @ L20, origin YELLOW -> FULL. Real Gen-1 base stats
   * (HP/Atk/Def/Spe/Spc = 35/55/30/90/50) and type (0x17, Electric) -- Gen 1 is the
   * ONLY generation gb_new_mon needs base/type for (Gen 2's record stores neither). */
  {
    GbNewMonSrc src; memset(&src, 0, sizeof src);
    src.base[GB_HP] = 35; src.base[GB_ATK] = 55; src.base[GB_DEF] = 30;
    src.base[GB_SPE] = 90; src.base[GB_SPC] = 50;
    src.type1 = src.type2 = 0x17;
    src.growth = gb_growth_rate(PLANT_DEX_PIKACHU);
    src.moves[0] = 33;                                /* Tackle */
    src.species_name = pk_species_name(PLANT_DEX_PIKACHU);
    src.ot_name = "PLANT";
    src.ot_id = 12345;
    GbEditMon e1;
    gb_new_mon(GB_GEN1, PLANT_DEX_PIKACHU, 20, &src, 2u, &e1);
    bc_pack(&e1, 0, BC_ORIGIN_YELLOW, 0, 2u, cell);
    memcpy(recs + (uint32_t)1 * 80, cell, 80);
  }

  /* slot 2: the same Gen-2 mon as an EGG -> RELAXED-as-Egg */
  plant_gen2_chikorita(&e, 12, 3u);
  gb_set_egg(&e, true);
  bc_pack(&e, BC_FLAG_EGG, BC_ORIGIN_GOLD, 0, 3u, cell);
  memcpy(recs + (uint32_t)2 * 80, cell, 80);

  /* slot 3: the same Gen-2 mon HOLDING an item -> RELAXED */
  plant_gen2_chikorita(&e, 12, 4u);
  gb_set_held_item(&e, 19);                           /* any non-zero Gen-2 item id */
  bc_pack(&e, BC_FLAG_HOLDS_ITEM, BC_ORIGIN_GOLD, 0, 4u, cell);
  memcpy(recs + (uint32_t)3 * 80, cell, 80);

  /* slot 4: an unrepresentable species (a glitch index) -> GB_SHOW_NONE -> the DMG
   * chip. 0xFE, not 0xFF (bc_pack's own list-terminator refusal) and not 0xFD
   * (G2_LIST_EGG, gen2_save.h) -- and genuinely OUT of Gen 2's valid range, unlike
   * an earlier draft's 0xEE=238, which gb_dex_from_index()'s own rule (gb_edit.c:66,
   * "Gen 2 indexes BY dex number", 1..G2_SPECIES_MAX=251) accepts as SMOOCHUM, a
   * real species -- caught live in this lane's own mGBA shot (the chip read "SMO",
   * not "DMG"), fixed here rather than by relaxing anything downstream. */
  plant_gen2_chikorita(&e, 12, 5u);
  e.rec[0] = 0xFE;         /* R2_SPECIES, offset 0 -- gb_edit.c's own private enum,
                            * not exported; 0 is the same constant by inspection. */
  e.list_species = 0xFE;
  bc_pack(&e, 0, BC_ORIGIN_GOLD, 0, 5u, cell);
  memcpy(recs + (uint32_t)4 * 80, cell, 80);

  /* BACKLOG #150 S150-12 decision 17: two COPY cells (BC_FLAG_QUEUED_PC |
   * BC_FLAG_COPY, b2|b5), the artless/delta shot chain's own proof that decision 9
   * (a copy cell's DOWN skips the ledger write entirely) actually lands: without
   * these two planted cells, every DOWN on the delta build hits #179 (a)'s
   * "SIDECAR FOLDER / Nothing transferred" wall before ever reaching a real
   * xfer_down_write() call to prove skipped. Serials 6/7 are ALSO used by
   * bank_plant_box_full()'s own loop below for BOX 1 (a different buffer/box
   * entirely, called separately from pdna_bank.c) -- no same-box ident32 collision,
   * since S150-4's own collision scan is scoped to one box. */
  plant_gen2_chikorita(&e, 13, 6u);
  bc_pack(&e, (uint8_t)(BC_FLAG_QUEUED_PC | BC_FLAG_COPY), BC_ORIGIN_GOLD, 0, 6u, cell);
  memcpy(recs + (uint32_t)5 * 80, cell, 80);

  plant_gen2_chikorita(&e, 14, 7u);
  bc_pack(&e, (uint8_t)(BC_FLAG_QUEUED_PC | BC_FLAG_COPY), BC_ORIGIN_GOLD, 0, 7u, cell);
  memcpy(recs + (uint32_t)6 * 80, cell, 80);
}

void bank_plant_box_full(uint8_t* recs) {
  if (!recs) return;
  /* BACKLOG #150 S150-12: bank_plant_box0() now plants 7 slots (0-6, the two new
   * COPY cells included), but this function's OWN loop below still starts at 5 and
   * immediately overwrites slots 5/6 with its own plain (non-copy) Chikoritas at
   * the SAME serials 6/7 -- deliberately: this is a DIFFERENT box/buffer (box 1,
   * called separately from pdna_bank.c's box==1 branch, never against box 0's own
   * buffer -- confirmed at lane start: bank_plant_box0/bank_plant_box_full are
   * never both called against the same `recs`), and it is BOX 1 that is meant to
   * carry the generic 30-cell sweep, not box 0's two special COPY cells. Leaving
   * the loop start at 5 is correct, not stale; only this comment needed updating. */
  bank_plant_box0(recs);                              /* slots 0-6, as above */
  for (int slot = 5; slot < 30; slot++) {
    uint8_t level = (uint8_t)(12 + (slot % 30));
    if (level < 1) level = 1;
    if (level > 100) level = 100;
    uint32_t serial = (uint32_t)(slot + 1);            /* 6..30 */
    GbEditMon e; uint8_t cell[80];
    plant_gen2_chikorita(&e, level, serial);
    bc_pack(&e, 0, BC_ORIGIN_GOLD, 0, serial, cell);   /* distinct bc_ident32 per cell
                                                        * (bank_serial is inside its
                                                        * hashed span) */
    memcpy(recs + (uint32_t)slot * 80, cell, 80);
  }
}

#else
typedef int bank_plant_no_empty_tu;
#endif /* PDNA_DELTA */
