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
#include "gen12_convert.h" /* gen12_convert -- decision 12(c)'s own conversion */
#include "gb_sidecar.h"    /* GbscEntry, gbsc_init/gbsc_add */
#include "xfer_rec.h"      /* xr_key_g3, xr_entry_for_down, XR_KIND_NATIVE_HOME/XR_STATE_CLAIMED */
/* decision 12's own note: this static belongs in EWRAM, not IWRAM (32 KiB,
 * stack-critical even on the delta build) -- EWRAM_BSS lives in lib/sys.h, which is
 * NOT on host_bankcell_test.c's own `-I source` include path (that test dual-
 * compiles this whole file with -DPDNA_DELTA on the host to exercise bank_plant_box0
 * -- see this file's own header comment). __arm__ is defined by devkitARM's real
 * compiler and nothing else on this tree's host toolchains, so this stays a no-op
 * off-target. */
#ifdef __arm__
#include "sys.h"           /* EWRAM_BSS */
#define BANK_PLANT_EWRAM_BSS EWRAM_BSS
#else
#define BANK_PLANT_EWRAM_BSS
#endif

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

void bank_plant_box0(uint8_t* recs) {
  if (!recs) return;
  GbEditMon e;
  uint8_t cell[80];

  /* slot 0: Gen-2 CHIKORITA @ L12, origin GOLD -> FULL */
  plant_gen2_chikorita(&e, 12, 1u);
  bc_pack(&e, 0, BC_ORIGIN_GOLD, 0, 1u, cell);
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

/* ---- BACKLOG #150 S150-9 decision 12: the planted-ledger read shim (#179(a)) ---
 * Three keyed slots, not one: the chain also needs the RESTORED/PENDING refusals
 * (decision 8) and the "nothing changed" skip (decision 7), each its own ledger
 * file keyed to its OWN Gen-3 record -- a single shared key could only ever show
 * one state at a time. */
#define BANK_PLANT_XFER_SLOTS 4
/* Each seeded ledger file ever holds exactly ONE entry (gbsc_init + one gbsc_add),
 * never GBSC_MAX_ENTRIES (8) -- header + one entry is the real footprint, not the
 * worst-case 1042 B GBSC_FILE_MAX four of would overflow EWRAM by ~3 KB on this
 * vehicle (found live: the first build of this shim did exactly that). The shim's
 * own caller (xr_open) still passes its OWN GBSC_FILE_MAX-sized buffer as `cap` --
 * only the STATIC storage here shrinks. */
#define BANK_PLANT_XFER_SLOT_CAP (GBSC_HEADER + GBSC_ENTRY)

static uint8_t  BANK_PLANT_EWRAM_BSS s_xfer_buf[BANK_PLANT_XFER_SLOTS][BANK_PLANT_XFER_SLOT_CAP];
static uint32_t s_xfer_len[BANK_PLANT_XFER_SLOTS];
static uint64_t s_xfer_key[BANK_PLANT_XFER_SLOTS];
static bool     s_xfer_valid[BANK_PLANT_XFER_SLOTS];

bool bank_plant_xfer_open(uint64_t key, uint8_t* buf, uint32_t cap, uint32_t* len) {
  if (!buf) return false;
  for (int i = 0; i < BANK_PLANT_XFER_SLOTS; i++) {
    if (!s_xfer_valid[i] || key != s_xfer_key[i]) continue;
    if (s_xfer_len[i] > cap) return false;
    memcpy(buf, s_xfer_buf[i], s_xfer_len[i]);
    if (len) *len = s_xfer_len[i];
    return true;
  }
  return false;
}

/* Builds a fresh Gen-2 CHIKORITA cell + its gen12_convert()'d Gen-3 record, `serial`
 * apart from bank_plant_box0()'s own slot 0 so every planted key is distinct (bc_
 * ident32 hashes the serial). `level` lets the caller vary the mon so slots 27/28/29
 * (below) are never byte-identical to each other. */
static bool plant_g3_pair(uint8_t g3_rec80[80], uint8_t cell80[80], uint8_t level,
                          uint32_t serial) {
  GbEditMon e;
  plant_gen2_chikorita(&e, level, serial);
  bc_pack(&e, 0, BC_ORIGIN_GOLD, 0, serial, cell80);
  GbEditMon home; BcMeta meta;
  if (!bc_unpack(cell80, &home, &meta)) return false;
  Gb12Mon view;
  if (!bc_view(&home, &meta, bc_ident32(cell80), &view)) return false;
  Gb12Target tgt; memset(&tgt, 0, sizeof tgt);
  tgt.met_game = 3;   /* Emerald -- this chain's own vehicle */
  Gb12Notes notes;
  return gen12_convert(&view, &tgt, g3_rec80, &notes) == GB12_OK;
}

/* Seeds slot `idx`'s ledger file, one entry, keyed by xr_key_g3(g3_rec80). `state`
 * is the entry's XR_STATE_* (decision 8); `alter` requests decision 12(b)'s
 * written_level-3/moves-swap so the screen has real toggle rows (false leaves the
 * entry byte-identical to the home -- decision 7's "nothing changed" skip case). */
static void plant_xfer_slot(int idx, const uint8_t g3_rec80[80], const uint8_t cell80[80],
                            uint8_t state, bool alter) {
  if (idx < 0 || idx >= BANK_PLANT_XFER_SLOTS) return;
  GbEditMon written; BcMeta meta;
  if (!bc_unpack(cell80, &written, &meta)) return;

  GbscEntry e;
  xr_entry_for_down(&e, &written, cell80, 0, XR_DIR_ABROAD_G3, g3_rec80 + 0x08);
  e.state = state;

  if (alter) {
    uint8_t lvl = e.written_level;
    e.written_level = (lvl > 3) ? (uint8_t)(lvl - 3) : lvl;
    e.moves_written[1] = (e.moves_written[1] == 1) ? 2 : 1;
  }

  uint64_t key = xr_key_g3(g3_rec80);
  uint32_t len = (uint32_t)gbsc_init(s_xfer_buf[idx], key);
  if (gbsc_add(s_xfer_buf[idx], &len, BANK_PLANT_XFER_SLOT_CAP, &e) < 0) return;
  s_xfer_key[idx] = key;
  s_xfer_len[idx] = len;
  s_xfer_valid[idx] = true;
}

/* Builds and seeds all four planted ledger slots, filling g3_out[0..3][80] with
 * their own converted Gen-3 records -- the caller (pdna_main.c's PC-storage mount
 * hook) places g3_out[i] at PC box 0 slot (29 - i):
 *   0 (slot 29): CLAIMED, ALTERED  -- the main chain, real LEVEL/MOVES rows
 *   1 (slot 28): CLAIMED, unaltered -- decision 7's "nothing changed" skip
 *   2 (slot 27): RESTORED           -- the ALREADY RESTORED refusal (decision 8)
 *   3 (slot 26): PENDING            -- the SAVE FIRST refusal (decision 8)
 * Slot 0's cell (level 12, serial 1) is byte-identical to bank_plant_box0()'s own
 * Bank box 0 slot 0 cell (same construction) -- decision 12(b)'s own requirement.
 * Returns the count of slots successfully seeded (0..4). */
int bank_plant_xfer_seed_all(uint8_t g3_out[4][80]) {
  static const uint8_t  levels[4]  = {12, 14, 15, 18};
  static const uint32_t serials[4] = {1u, 2u, 27u, 26u};
  static const uint8_t  states[4]  = {XR_STATE_CLAIMED, XR_STATE_CLAIMED,
                                      XR_STATE_RESTORED, XR_STATE_PENDING};
  static const bool     alters[4]  = {true, false, false, false};
  if (!g3_out) return 0;
  int n = 0;
  for (int i = 0; i < 4; i++) {
    uint8_t cell80[80];
    if (!plant_g3_pair(g3_out[i], cell80, levels[i], serials[i])) continue;
    plant_xfer_slot(i, g3_out[i], cell80, states[i], alters[i]);
    n++;
  }
  return n;
}

#else
typedef int bank_plant_no_empty_tu;
#endif /* PDNA_DELTA */
