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
#include "gen3_edit.h"     /* gen3_build_mon, gen3_edit_load/commit, em_set_item --
                            * BACKLOG #260's item/Secret-ID fixtures */
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

/* BACKLOG #150 S150-15 review (fixture fix 1): same mon (species/level/DVs/moves/
 * OT), a CALLER-CHOSEN bank_serial -- bc_pack's ident32 hashes the serial, so a
 * distinct serial gives a distinct xr_key_g3() without changing any byte a summary
 * card draws (serial lives at cell bytes 4..7/73..76, outside every drawn field).
 * Exists so xfer_plant.c can pick a serial that does not collide with any other
 * PDNA_DELTA fixture's own planted key (source/xfer_plant.c's own XFER_PLANT_SERIAL,
 * distinct from S150-9's bank_plant_xfer_seed_all() slots). */
void bank_plant_cell0_serial(uint8_t out80[80], uint32_t serial) {
  if (!out80) return;
  GbEditMon e;
  plant_gen2_chikorita(&e, 12, 1u);
  bc_pack(&e, 0, BC_ORIGIN_GOLD, 0, serial, out80);
}

/* BACKLOG #150 S150-15 decision 13: the exact bytes bank_plant_box0() plants at slot
 * 0 -- extracted so xfer_plant.c can convert the SAME cell (not a fresh, potentially
 * divergent one) into the DOWN arm's shot-chain fixture. serial 1u, bank_plant_box0's
 * own slot-0 value. */
void bank_plant_cell0(uint8_t out80[80]) {
  bank_plant_cell0_serial(out80, 1u);
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

/* BACKLOG #212 review D3(b): a Gen-2 cell with TWO moves out of range for Gen 1
 * (200 and 230, both > gb_max_move(GB_GEN1)=165 but still <= gb_max_move(GB_GEN2)
 * =251, i.e. legal on this cell's own generation) -- the --s150-8-bridge shot
 * chain's own demonstration of the per-slot swap-row modal (BACKLOG #150 S150-10's
 * rule, applied to the bridge by #212) when the record has TWO bad slots, not the
 * ONE reachable refusal box0's own CHIKORITA plant (dex 152, always caught by the
 * species-floor check first) can ever show.
 *
 * Species dex 1 (Bulbasaur), NOT dex 25 (Pikachu): box0/box1 slot 1 already plants
 * a Gen-1 PIKACHU with the SAME OT "PLANT"/id 12345/level 20 bank_plant_box0() uses
 * everywhere else in this file, so a same-species badmoves cell would be visually
 * indistinguishable from that unrelated cell in a screenshot (same name/level/OT --
 * only the GB1/GB2 badge and move list would differ). Bulbasaur (dex 1, well under
 * gb_max_species(GB_GEN1)=151) clears the Gen-1 species floor exactly the same way,
 * with a distinct OT ("BADMOVE"/id 9999) so a shot's caption never has to lean on
 * the badge alone to prove which cell is on screen. */
#define PLANT_DEX_BADMOVES 1
void bank_plant_gen2_badmoves_cell(uint8_t out80[80]) {
  if (!out80) return;
  GbNewMonSrc src; memset(&src, 0, sizeof src);
  src.growth = gb_growth_rate(PLANT_DEX_BADMOVES);
  src.moves[0] = 200;    /* > gb_max_move(GB_GEN1)=165, <= gb_max_move(GB_GEN2)=251 */
  src.moves[1] = 230;    /* > gb_max_move(GB_GEN1)=165, <= gb_max_move(GB_GEN2)=251 */
  src.species_name = pk_species_name(PLANT_DEX_BADMOVES);
  src.ot_name = "BADMOVE";
  src.ot_id = 9999;   /* uint16_t field -- 99999 overflowed it (caught by -Wconstant-conversion) */
  GbEditMon e;
  gb_new_mon(GB_GEN2, PLANT_DEX_BADMOVES, 20, &src, 8u, &e);
  /* xfer-items fix pass F7: an item (id 19, the same "any non-zero Gen-2 item id"
   * bank_plant_box0's own slot-3 comment uses) so this cell -- the ONE planted
   * fixture that both clears the Gen-1 species floor (dex 1) AND reaches
   * gb_paste_loss_screen on a bridge (run_s150_8_bridge's own leg (3)) -- can also
   * demonstrate the item/Secret-ID row's "stays behind" text (bank_down_convert.c's
   * own decision 15: a Gen-2 item can NEVER reach Gen 1 on the bridge arm, so this
   * is unconditional, not merely likely). Only run_s150_8_bridge (tools/dgb_shots.py)
   * consumes this cell -- see this function's own header/BACKLOG #212 review D3(b)
   * comment above -- so this is scoped to that one chain's own frames. */
  gb_set_held_item(&e, 19);
  bc_pack(&e, 0, BC_ORIGIN_GOLD, 0, 8u, out80);
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

  /* BACKLOG #212 review D3(b): slot 7 -- overwrites the generic sweep's own plain
   * Chikorita there (no shot chain names box 1's generic sweep slots individually;
   * they are only ever described in bulk, "25 fresh non-copy FULL cells") with the
   * two-bad-move cell above. Box 0's own slots 0-6 (bank_plant_box0) and PC-storage
   * slots 26-29 (bank_plant_xfer_seed_all) are both already taken -- this is the
   * free slot the brief's own fallback names. */
  {
    uint8_t cell[80];
    bank_plant_gen2_badmoves_cell(cell);
    memcpy(recs + (uint32_t)7 * 80, cell, 80);
  }
}

/* ---- BACKLOG #150 S150-9 decision 12: the planted-ledger read shim (#179(a)) ---
 * Three keyed slots, not one: the chain also needs the RESTORED/PENDING refusals
 * (decision 8) and the "nothing changed" skip (decision 7), each its own ledger
 * file keyed to its OWN Gen-3 record -- a single shared key could only ever show
 * one state at a time. */
#define BANK_PLANT_XFER_SLOTS 5   /* was 4 -- slot 4 = BACKLOG #209 site 2, gbsc_key-keyed */
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

/* BACKLOG #246 (#104 Phase 1): plant_g3_pair's own species (dex 152, CHIKORITA) has
 * NO Gen-1 form at all (Gen 1's own dex tops out at 151) -- fine for plant_g3_pair's
 * existing callers (bank_plant_xfer_seed_all's own PC-storage fixture never lands on
 * a Gen-1 save), wrong for this one: BACKLOG #246's shot chain needs to prove a
 * landing on BOTH Game Boy generations, Red (Gen 1) included, so the planted species
 * must clear the Gen-1 species floor too. A local, parallel builder -- dex 1
 * (BULBASAUR), never touching plant_gen2_chikorita/plant_g3_pair (bank_plant_xfer_
 * seed_all's own fixture stays byte-for-byte unchanged) -- with a single legal move
 * (Tackle, id 33, valid in every generation) so a Red landing never needs a per-slot
 * move fill either. */
#define PLANT_G3_DEX_BULBASAUR 1
static bool plant_g3_pair_bulbasaur(uint8_t g3_rec80[80], uint8_t cell80[80],
                                    uint8_t level, uint32_t serial) {
  GbNewMonSrc src; memset(&src, 0, sizeof src);
  src.growth = gb_growth_rate(PLANT_G3_DEX_BULBASAUR);
  src.moves[0] = 33;                                  /* Tackle -- valid everywhere */
  src.species_name = pk_species_name(PLANT_G3_DEX_BULBASAUR);
  src.ot_name = "PLANT";
  src.ot_id = 12345;
  GbEditMon e;
  gb_new_mon(GB_GEN2, PLANT_G3_DEX_BULBASAUR, level, &src, serial, &e);
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

/* BACKLOG #260 (item-shots lane): the held item cases B/C/F3 need -- POTION, Gen-3
 * item id 13 (data_tables.c s_item[13]). Verified end to end before use, not guessed:
 * item_map_g2g3.c's kG3ToG2[13] == 18, and gb_item_names.c's kGen2ItemName[18] ==
 * "POTION" -- item_g2_to_g1()'s 1:1 name search then resolves that same "POTION"
 * string to a real, non-key Gen-1 item id, so this id reaches Gen 1 through the whole
 * item_g3_to_g2()/item_g2_to_g1() chain g3gb_item_ladder() (source/gen3_to_gb.c)
 * walks -- one of the 48 the brief names, picked by checking the table, not by name
 * alone. */
#define PLANT_G3_ITEM_POTION 13u

/* Case F3's own otId: SID 0xBEEF (nonzero upper 16 bits) / TID 0x3039 (12345,
 * matching plant_g3_pair_bulbasaur's own ot_id so the two box-2 fixtures share a
 * trainer). */
#define PLANT_G3_SID_OTID 0xBEEF3039u

/* BACKLOG #246 (#104 Phase 1): a PLAIN Gen-3 Bank cell -- NEVER native/GBC1 -- for a
 * DEDICATED box (box 2, wired below in pdna_bank.c's box_load()), not box 0/1: every
 * existing shot chain's pixel captions and counts ("BANK 1  7/30" etc.) key off
 * bank_plant_box0()/bank_plant_box_full()'s own byte-for-byte content, and this lane
 * must not move either. Reuses plant_g3_pair_bulbasaur() above (the SAME shape as
 * plant_g3_pair's own Gen-2 -> gen12_convert() -> Gen-3 pipeline, just a Gen-1-legal
 * species) so the planted cell is a real, decodable Gen-3 box record built with no
 * new construction path -- this is the first Bank-visited PDNA_DELTA scenario that
 * has ever needed a PLAIN Gen-3 cell (every earlier plant in this file is a native
 * "GBC1" cell). serial 300 -- distinct from every other planted serial in this file
 * (box0 1..7, box_full 6..30, S150-9 xfer {1,2,26,27}, site2 200).
 *
 * BACKLOG #260 (item-shots lane) adds the held item -- em_set_item() patches the
 * Growth substruct's item field AFTER gen12_convert() already ran and re-encodes with
 * gen3_edit_commit(), so nothing about the up-conversion pipeline itself changes and
 * the record's SID stays 0 (gen12_convert.h's own documented "PokeDNA GB import"
 * fingerprint is untouched) -- this is deliberately NOT the Secret-ID fixture (see
 * plant_g3_sid_item() below, slot 1, for that). Neither of the two existing shot
 * chains against this box (tools/b246_g3_to_gb_shot.py, tools/b246_g3_to_gb_vsd.py)
 * asserts on the record's item byte or the box's slot count -- checked before this
 * edit, not assumed -- so no VARIANT function was needed; this fixture's own bytes
 * were free to move. */
#define PLANT_G3_SERIAL 300u

/* BACKLOG #260 (item-shots lane), case F3's own fixture: a genuine Gen-3-NATIVE
 * record (gen3_build_mon() directly, NOT plant_g3_pair_bulbasaur's gen12_convert
 * up-conversion pipeline -- that pipeline's own documented fingerprint,
 * gen12_convert.h's "SID == 0", would make a nonzero Secret ID here a lie about
 * where the record came from). otId's upper 16 bits (the SID half) are forced
 * nonzero so gen3_to_gb.c's own `loss->secret_id = (m->otId >> 16) != 0` reads true
 * and loss_item_text()'s "+ Secret ID" suffix has something real to report; the item
 * (same PLANT_G3_ITEM_POTION as slot 0) makes item_outcome land on BAG/PC too, so the
 * frame reads the FULL composite suffix ("POTION -> bag + Secret ID"), not just the
 * bare Secret-ID row. Slot 1 of box 2 -- bank_plant_g3_box's own header comment
 * documents both slots now; slots 2..29 stay the caller's memset-zero empty cells,
 * unchanged. */
static bool plant_g3_sid_item(uint8_t g3_rec80[80]) {
  if (!g3_rec80) return false;
  gen3_build_mon(PLANT_G3_DEX_BULBASAUR, 14, 0x2468ACE0u, PLANT_G3_SID_OTID,
                "PLANT2", 3, g3_rec80);
  EditMon e;
  gen3_edit_load(g3_rec80, false, &e);
  em_set_item(&e, PLANT_G3_ITEM_POTION);
  gen3_edit_commit(&e, g3_rec80);
  return true;
}

void bank_plant_g3_box(uint8_t* recs) {
  if (!recs) return;
  uint8_t g3_rec80[80], cell80[80];   /* cell80 discarded -- this slot's whole point
                                       * is the plain Gen-3 RECORD, not a native cell */
  if (!plant_g3_pair_bulbasaur(g3_rec80, cell80, 14, PLANT_G3_SERIAL)) { memset(recs, 0, 80); return; }
  EditMon e;                             /* BACKLOG #260: cases B/C's held item */
  gen3_edit_load(g3_rec80, false, &e);
  em_set_item(&e, PLANT_G3_ITEM_POTION);
  gen3_edit_commit(&e, g3_rec80);
  memcpy(recs, g3_rec80, 80);   /* slot 0 of this box */

  uint8_t sid_rec80[80];
  if (plant_g3_sid_item(sid_rec80)) memcpy(recs + 80, sid_rec80, 80);  /* slot 1: case F3 */
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

/* BACKLOG #209: an arbitrary label distinct from every other PDNA_DELTA fixture's own
 * planted bank_serial (box0 1..7, box_full 6..30, S150-9 xfer {1,2,26,27}, S150-15
 * xfer_plant 150) -- purely documentation here, since this cell is never memcpy'd
 * into any real Bank box buffer (unlike those), so it cannot actually collide with
 * any of them inside a bank_ident32_collision() scan; only s_xfer_key[]'s own exact-
 * match lookup (a completely different hash domain, gbsc_key vs xr_key_g3) matters. */
#define XFER_PLANT_SITE2_SERIAL 200u

void bank_plant_site2_seed(const GbEditMon* mon) {
  if (!mon) return;
  /* BACKLOG #206/#209 review D2: the entry's HOME must be a CHANGED copy of the
   * live mon, not the live mon itself -- gb_lift_restore's probe compares the
   * entry's baseline (written_level / nick_written) against the mon still ON THE
   * CARD (unedited by this seed); if both sides are built from the same `mon`,
   * nothing ever reads as changed and app_xfer_merge_screen never draws (decision
   * 7: XR_MERGE_DOWN skips the screen when no row exists). Renaming to "OLDNAME"
   * and dropping the level by 5 gives the probe two real rows (renamed=1,
   * level_changed=1) without touching gbsc_key's own fields (gen/otid16/dv4/
   * otname) -- the key this entry is looked up by stays the live mon's own. */
  GbEditMon seed_mon = *mon;
  (void)gb_set_nickname(&seed_mon, "OLDNAME");
  uint8_t live_level = gb_get_level(mon);
  uint8_t seed_level = (live_level > 5) ? (uint8_t)(live_level - 5) : live_level;
  (void)gb_set_level(&seed_mon, seed_level);

  uint8_t cell[80];
  uint8_t origin = (mon->gen == GB_GEN1) ? BC_ORIGIN_RED : BC_ORIGIN_GOLD;
  if (bc_pack(&seed_mon, 0, origin, 0, XFER_PLANT_SITE2_SERIAL, cell) != 0) return;
  GbEditMon written; BcMeta meta;
  if (!bc_unpack(cell, &written, &meta)) return;

  /* gbsc_entry_from() alone (not xr_entry_for_down()) -- there is no companion
   * converted Gen-3 record for this entry (gb_lift_restore() merges directly
   * between the entry and the Game Boy mon being lifted, never consults a Gen-3
   * side at all), so xr_entry_for_down()'s nick_g3 override does not apply; its
   * default (nick_written = written->nick) is exactly what gbsc_entry_from() does
   * on its own. kind/state/direction/claimed mirror plant_xfer_slot()'s own stamps
   * for a CLAIMED, unaltered NATIVE_HOME entry. */
  GbscEntry e;
  gbsc_entry_from(&e, &written, cell, 0);
  e.kind = XR_KIND_NATIVE_HOME;
  e.state = XR_STATE_CLAIMED;
  /* BACKLOG #206/#209 review D2: gb_lift_restore's probe is xr_merge_down_gb_sel
   * (source/xfer_rec.c:245 `if (e->direction != XR_DIR_ABROAD_GB) return false;`),
   * not xr_merge_down_sel -- site 2 is a GB lift, so the seeded entry must claim to
   * have come from the OTHER Game Boy generation, never XR_DIR_ABROAD_G3 (that tag
   * is site 1's, a Gen-3 record). The old XR_DIR_ABROAD_G3 here meant every --s150-
   * 9-site2 run died silently at this probe, never at pdna_bank_next_serial as
   * frame 02's caption claimed. */
  e.direction = XR_DIR_ABROAD_GB;
  e.claimed = 1;

  uint8_t dv4[4] = {
    gb_get_dv(mon, GB_ATK), gb_get_dv(mon, GB_DEF),
    gb_get_dv(mon, GB_SPE), gb_get_dv(mon, GB_SPC)
  };
  uint64_t key = gbsc_key(mon->gen, gb_get_otid(mon), dv4, mon->otname);
  uint32_t len = (uint32_t)gbsc_init(s_xfer_buf[4], key);
  if (gbsc_add(s_xfer_buf[4], &len, BANK_PLANT_XFER_SLOT_CAP, &e) < 0) return;
  s_xfer_key[4] = key;
  s_xfer_len[4] = len;
  s_xfer_valid[4] = true;
}

bool bank_plant_xfer_has(uint64_t key) {
  for (int i = 0; i < BANK_PLANT_XFER_SLOTS; i++)
    if (s_xfer_valid[i] && key == s_xfer_key[i]) return true;
  return false;
}

#else
typedef int bank_plant_no_empty_tu;
#endif /* PDNA_DELTA */
