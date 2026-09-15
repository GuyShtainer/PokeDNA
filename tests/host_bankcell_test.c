/* Host test for source/bank_cell.{c,h} -- the tagged 80-byte native Bank cell codec
 * (BACKLOG #150 S150-1). Pure C, dual-compiles on the host; the module under test has
 * ZERO callers in the shipped build, so this file is the only thing exercising it.
 *
 *   cc -std=c11 -Wall -Wextra -I source -DPDNA_DELTA tests/host_bankcell_test.c \
 *      source/bank_cell.c source/gb_edit.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/gen3_save.c source/gen3_box.c \
 *      source/gen3_mon.c source/gen3_edit.c source/gen3_daycare.c source/gen3_clip.c \
 *      source/gen12_convert.c source/data_tables.c source/bank_plant.c \
 *      source/gb_new_mon.c source/gb_editor.c source/gb_session.c source/rom_gblearn.c \
 *      source/rom_gbbase.c source/rom_gbsprite.c source/gb_sprite_codec.c \
 *      source/ui_font.c source/gb_sidecar.c -o /tmp/hbc
 *   /tmp/hbc (Guy's five Gen-3 .sav files, positional argv -- run_host_tests.py hands
 *             them over automatically). -DPDNA_DELTA compiles section 8 (bank_plant,
 *             BACKLOG #150 S150-2 step 6) IN; it never affects the shipped GBA build
 *             (PDNA_DELTA is only ever defined by `make PDNA_TARGET=delta`, which
 *             neither gate target uses -- see the lane's delivery report for the nm
 *             proof that no bank_plant symbol reaches either gate ELF). Section 8
 *             additionally writes box15.box/box14.box to the CURRENT directory when
 *             the environment variable PDNA_EMIT_PLANT=1 is set -- never by default,
 *             never into git (.gitignore covers /box14.box and /box15.box).
 *
 * Two independent corpora, read-only:
 *   - Gen-1/2: Guy's own dumps at a FIXED path outside the repo (gitignored, the same
 *     convention host_gen3gb_test.c already uses) -- Red.sav/Yellow.sav (Gen 1),
 *     Gold.sav/Crystal.sav (Gen 2). Missing files SKIP rather than fail.
 *   - Gen-3: the five cartridge saves passed as argv, exactly like every other Gen-3
 *     host test; run_host_tests.py supplies them.
 *
 * What this file pins (docs/BANK-CROSSGEN-DESIGN.md SS11.1, SS11.13 row S150-1):
 *   1. pack -> unpack is byte-exact for EVERY Gen-1/Gen-2 box AND party record in the
 *      corpus, including eggs (Gen-2 list byte 0xFD) and item holders -- both are
 *      ordinary members of the full sweep, never filtered out.
 *   2. party -> box truncation matches gbs_move()'s own rule (gb_session.c): Gen-1
 *      syncs the box-level byte to the live level first; Gen-2 just drops the tail.
 *   3. a memset(0) cell is not native.
 *   4. NO Gen-3 corpus record (all five saves, every PC box slot and every party slot)
 *      is ever misread as native.
 *   5. byte 19 is 0x01 in every packed cell, and pk3_validate() (gen3_clip.c) rejects
 *      every one of them -- the old-build guard.
 *   6. a one-bit flip anywhere in bc_ident32()'s hashed span (bytes 8..10, 12..68,
 *      73..76) breaks bc_is_native(); flipping the flags byte (11) or rtc_epoch
 *      (69..72) does NOT; flipping the reserved tail (77..79) does NOT either --
 *      SS11.1 defines ident32 over exactly 8..10+12..68+73..76, so 77..79 sit outside
 *      the hash and outside every other bc_is_native() check the same way flags and
 *      rtc_epoch do (see bank_cell.h's header comment for the "(61 B)" arithmetic note).
 *   7. packing the SAME GbEditMon twice with two DIFFERENT bank_serial values yields
 *      two cells differing in bytes 0..7; the SAME bank_serial with a different flags
 *      or rtc_epoch leaves bytes 0..7 unchanged (G-M4).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>   /* getenv -- PDNA_EMIT_PLANT (section 8) */

#include "bank_cell.h"
#include "gb_edit.h"
#include "gb_editor.h"   /* gbe_press/gbe_adjust/gbe_set_move -- S150-14 section 10 */
#include "gen1_save.h"
#include "gen1_write.h"
#include "gen2_save.h"
#include "gen3_save.h"
#include "gen3_box.h"
#include "gen3_mon.h"
#include "gen3_clip.h"
#include "gb_sidecar.h"
#include "gb_session.h"   /* gbs_open/gbs_load_list -- S150-4-5b section 11 (raw bytes) */

#define GB_ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* ---- shared expectations ----------------------------------------------------- */

/* Independently re-derive the box-shape record bc_pack() must produce for `mon`,
 * so the test does not simply call the module under test twice and compare it with
 * itself. Mirrors gbs_move()'s own party->box rule (gb_session.c). */
static void expect_box_rec(const GbEditMon* mon, uint8_t exp33[GEN1_BOX_REC_BYTES]) {
  memset(exp33, 0, GEN1_BOX_REC_BYTES);
  if (mon->gen == GB_GEN1) {
    memcpy(exp33, mon->rec, GEN1_BOX_REC_BYTES);
    if (mon->is_party) exp33[G1R_BOXLEVEL] = mon->rec[G1R_LEVEL];
  } else {
    memcpy(exp33, mon->rec, G2_BOX_ENTRY);
  }
}

/* One record through pack -> unpack, checked byte-exact against expect_box_rec()'s
 * independent truncation and against a fresh set of BcMeta fields. `serial` varies
 * per call so a whole-box sweep also feeds test 7 (fresh serial -> different bytes
 * 0..7) implicitly through the ident32 uniqueness this loop never violates. */
static void roundtrip_one(const char* tag, const GbEditMon* mon, uint32_t serial) {
  uint8_t cell[BC_CELL_BYTES];
  int rc = bc_pack(mon, BC_FLAG_FROM_PARTY * (mon->is_party ? 1 : 0),
                   BC_ORIGIN_UNKNOWN, 0x1234u, serial, cell);
  CHECK(rc == 0, "%s: bc_pack failed", tag);
  if (rc != 0) return;

  CHECK(cell[BC_OFF_OLDBUILD] == BC_OLDBUILD_BYTE, "%s: byte 19 is not 0x01", tag);
  CHECK(pk3_validate(cell) == false, "%s: pk3_validate accepted a packed cell", tag);
  CHECK(bc_is_native(cell), "%s: bc_is_native false on a cell it just packed", tag);

  uint8_t exp33[GEN1_BOX_REC_BYTES];
  expect_box_rec(mon, exp33);
  uint8_t rec_len = (mon->gen == GB_GEN1) ? GEN1_BOX_REC_BYTES : (uint8_t)G2_BOX_ENTRY;

  GbEditMon back;
  BcMeta meta;
  CHECK(bc_unpack(cell, &back, &meta), "%s: bc_unpack failed on its own cell", tag);
  CHECK(back.gen == mon->gen, "%s: gen mismatch after unpack", tag);
  CHECK(back.is_party == false, "%s: unpacked mon is never party-shape", tag);
  CHECK(back.rec_len == rec_len, "%s: rec_len mismatch after unpack", tag);
  CHECK(memcmp(back.rec, exp33, rec_len) == 0,
        "%s: record bytes not byte-exact after pack->unpack", tag);
  CHECK(memcmp(back.otname, mon->otname, GB_NAME_BYTES) == 0,
        "%s: OT name not byte-exact", tag);
  CHECK(memcmp(back.nick, mon->nick, GB_NAME_BYTES) == 0,
        "%s: nickname not byte-exact", tag);
  CHECK(back.list_species == mon->list_species, "%s: list_species mismatch", tag);
  CHECK(meta.gen == mon->gen, "%s: meta.gen mismatch", tag);
  CHECK(meta.origin_game == BC_ORIGIN_UNKNOWN, "%s: meta.origin_game mismatch", tag);
  CHECK(meta.rtc_epoch == 0x1234u, "%s: meta.rtc_epoch mismatch", tag);
  CHECK(meta.bank_serial == serial, "%s: meta.bank_serial mismatch", tag);
}

/* ============================================================================ */
/* bc_view() parity (BACKLOG #150 S150-2): gb_load -> bc_pack -> bc_unpack ->
 * bc_view must equal, field for field, the gen12_from_gen1/gen12_from_gen2 struct
 * the shipping GB grid builds for the SAME slot -- slot_salt excepted (the caller's
 * own value, never derived from the record). `check_caught` gates has_caught_data/
 * ot_gender; every call site below now passes true (finding 2's fix: bc_view
 * derives has_caught_data from the SAME byte-level rule gen12_from_gen2 does, never
 * from origin_game, so the comparison holds unconditionally -- kept as a parameter
 * rather than deleted so a future caller that genuinely cannot compare it has an
 * documented escape hatch, not a silently-relaxed default).
 * A mismatch means a native cell would render DIFFERENTLY from the same mon in a
 * GB session -- never a tolerance to relax, always a bug in bc_view. */
static void check_gb12_parity(const char* tag, const Gb12Mon* got, const Gb12Mon* want,
                              bool check_caught) {
  CHECK(got->gen == want->gen, "%s: bc_view gen mismatch (%u vs %u)", tag, got->gen, want->gen);
  CHECK(got->species_dex == want->species_dex, "%s: species_dex mismatch (%u vs %u)",
        tag, got->species_dex, want->species_dex);
  CHECK(got->exp == want->exp, "%s: exp mismatch (%u vs %u)", tag, got->exp, want->exp);
  CHECK(got->level == want->level, "%s: level mismatch (%u vs %u)", tag, got->level, want->level);
  CHECK(got->dv_atk == want->dv_atk, "%s: dv_atk mismatch", tag);
  CHECK(got->dv_def == want->dv_def, "%s: dv_def mismatch", tag);
  CHECK(got->dv_spd == want->dv_spd, "%s: dv_spd mismatch", tag);
  CHECK(got->dv_spc == want->dv_spc, "%s: dv_spc mismatch", tag);
  for (int i = 0; i < 4; i++) {
    CHECK(got->moves[i] == want->moves[i], "%s: moves[%d] mismatch (%u vs %u)",
          tag, i, got->moves[i], want->moves[i]);
    CHECK(got->pp_ups[i] == want->pp_ups[i], "%s: pp_ups[%d] mismatch", tag, i);
  }
  CHECK(got->ot_id == want->ot_id, "%s: ot_id mismatch (%u vs %u)", tag, got->ot_id, want->ot_id);
  CHECK(strcmp(got->ot_name, want->ot_name) == 0, "%s: ot_name mismatch (\"%s\" vs \"%s\")",
        tag, got->ot_name, want->ot_name);
  CHECK(strcmp(got->nickname, want->nickname) == 0, "%s: nickname mismatch (\"%s\" vs \"%s\")",
        tag, got->nickname, want->nickname);
  CHECK(got->held_item == want->held_item, "%s: held_item mismatch (%u vs %u)",
        tag, got->held_item, want->held_item);
  CHECK(got->friendship == want->friendship, "%s: friendship mismatch (%u vs %u)",
        tag, got->friendship, want->friendship);
  CHECK(got->pokerus == want->pokerus, "%s: pokerus mismatch (%u vs %u)", tag, got->pokerus, want->pokerus);
  CHECK(got->is_egg == want->is_egg, "%s: is_egg mismatch", tag);
  if (check_caught) {
    CHECK(got->has_caught_data == want->has_caught_data, "%s: has_caught_data mismatch", tag);
    CHECK(got->ot_gender == want->ot_gender, "%s: ot_gender mismatch", tag);
  }
}

/* pack `mon` under `origin`, unpack, bc_view with a fixed id_salt (irrelevant --
 * slot_salt is excepted from the comparison), and check it against `want`. */
static void view_parity_one(const char* tag, const GbEditMon* mon, uint8_t origin,
                            const Gb12Mon* want, bool check_caught, uint32_t serial) {
  uint8_t cell[BC_CELL_BYTES];
  int rc = bc_pack(mon, 0, origin, 0, serial, cell);
  CHECK(rc == 0, "%s: bc_pack failed (view parity)", tag);
  if (rc != 0) return;
  GbEditMon back; BcMeta meta;
  CHECK(bc_unpack(cell, &back, &meta), "%s: bc_unpack failed (view parity)", tag);
  Gb12Mon got;
  CHECK(bc_view(&back, &meta, 0, &got), "%s: bc_view failed", tag);
  check_gb12_parity(tag, &got, want, check_caught);
}

/* ============================================================================ */
/* 1. Gen-1 corpus sweep: Red.sav, Yellow.sav -- every box + the party.          */
/* ============================================================================ */

static uint32_t g_serial = 1;   /* strictly increasing across the whole run */

static void sweep_gen1(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[GEN1_SAVE_SIZE];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  Gen1Save s;
  Gen1Status gs = gen1_open(img, len, &s);
  CHECK(gs == GEN1_OK, "%s: gen1_open (%s)", file, gen1_status_text(gs));
  if (gs != GEN1_OK) return;

  int n = 0;
  for (int box = 0; box <= GEN1_PARTY_BOX; box++) {
    uint32_t off = gen1_list_offset(&s, box);
    int count = gen1_list_count(img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96];
      snprintf(tag, sizeof tag, "%s box%d slot%d", file, box, slot);
      CHECK(gb_load(&mon, GB_GEN1, img + off, box, slot), "%s: gb_load", tag);
      roundtrip_one(tag, &mon, g_serial++);

      /* bc_view parity: independently decode the SAME slot the shipping GB grid's
       * own way (gen1_decode -> gen12_from_gen1) and compare field for field. */
      Gen1Mon g1;
      CHECK(gen1_decode(img + off, box, slot, &g1), "%s: gen1_decode (view parity)", tag);
      Gb12Mon want;
      gen12_from_gen1(&g1, 0, &want);
      uint8_t origin = (strcmp(file, "Yellow.sav") == 0) ? BC_ORIGIN_YELLOW : BC_ORIGIN_RED;
      view_parity_one(tag, &mon, origin, &want, true, g_serial++);
      n++;
    }
  }
  printf("  %s: %d Gen-1 record(s) round-tripped\n", file, n);
}

/* ============================================================================ */
/* 2. Gen-2 corpus sweep: Gold.sav, Crystal.sav -- every box + the party.        */
/*    Covers eggs (list byte 0xFD) and item holders as ordinary sweep members.   */
/* ============================================================================ */

static void sweep_gen2(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  G2Save sv;
  CHECK(g2_detect(img, len, &sv), "%s: g2_detect", file);
  if (!sv.supported) { printf("  SKIP %s (unsupported)\n", file); return; }

  G2Header hd;
  CHECK(g2_read_header(img, &sv, &hd), "%s: g2_read_header", file);

  int n = 0, eggs = 0;
  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96];
      snprintf(tag, sizeof tag, "%s box%d slot%d", file, box, slot);
      CHECK(gb_load(&mon, GB_GEN2, img + off, box, slot), "%s: gb_load", tag);
      if (mon.list_species == G2_LIST_EGG) eggs++;
      roundtrip_one(tag, &mon, g_serial++);

      /* bc_view parity, same shape as the Gen-1 sweep. has_caught_data/ot_gender are
       * ALWAYS comparable now (S150-2 finding 2): bc_view derives has_caught_data
       * from the same byte-level rule gen12_from_gen2 does ((rec[0x1D]|rec[0x1E])
       * != 0), never from origin_game, so raw equality holds unconditionally --
       * including for Gold.sav, whose identical bytes are Unused1/Unused2 but
       * still compare bit-for-bit the same way on both sides. */
      G2Mon g2;
      CHECK(g2_list_mon(img + off, box, slot, &g2), "%s: g2_list_mon (view parity)", tag);
      Gb12Mon want;
      gen12_from_gen2(&g2, 0, &want);
      bool is_crystal = (strcmp(file, "Crystal.sav") == 0);
      uint8_t origin = is_crystal ? BC_ORIGIN_CRYSTAL : BC_ORIGIN_GOLD;
      view_parity_one(tag, &mon, origin, &want, /*check_caught=*/true, g_serial++);
      n++;
    }
  }
  printf("  %s: %d Gen-2 record(s) round-tripped (%d egg(s))\n", file, n, eggs);
}

/* ============================================================================ */
/* 2b. NAMED regression: five real Crystal.sav box7 slots with legitimately       */
/*     all-zero capture bytes (S150-2 finding 2) -- has_caught_data must be       */
/*     FALSE for these even though origin_game is Crystal, because it is the      */
/*     converter's own byte-level rule (gen2_save.c:535's caught_valid), never    */
/*     origin_game. Pinned by slot number so a future regression here fails       */
/*     loud, not silently inside the general sweep's loop.                       */
/* ============================================================================ */

static void regress_crystal_uncaught_box7(void) {
  char path[512];
  snprintf(path, sizeof path, "%s/Crystal.sav", GB_ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP Crystal.sav uncaught-box7 regression (not present)\n"); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP Crystal.sav uncaught-box7 regression (unsupported)\n"); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP Crystal.sav uncaught-box7 regression (no header)\n"); return; }
  /* box 7 (0-indexed, an ordinary PC box -- NOT G2_BOX_PARTY, which is 14): the
   * original sweep tagged these failures "Crystal.sav box7 slotN". */
  const int kBox = 7;
  uint32_t off = g2_list_offset(&sv, kBox, hd.current_box);
  if (off == 0) { printf("  SKIP Crystal.sav uncaught-box7 regression (no box7 list)\n"); return; }

  static const int kSlots[] = { 3, 4, 5, 9, 10 };
  int n = 0;
  for (unsigned i = 0; i < sizeof kSlots / sizeof kSlots[0]; i++) {
    int slot = kSlots[i];
    char tag[64]; snprintf(tag, sizeof tag, "Crystal.sav box7 slot%d (named regression)", slot);
    GbEditMon mon;
    CHECK(gb_load(&mon, GB_GEN2, img + off, kBox, slot), "%s: gb_load", tag);
    /* precondition: the record's own capture bytes really are both zero */
    CHECK((mon.rec[0x1D] | mon.rec[0x1E]) == 0, "%s: precondition failed -- capture bytes are NOT zero, this slot no longer regresses", tag);

    uint8_t cell[BC_CELL_BYTES];
    CHECK(bc_pack(&mon, 0, BC_ORIGIN_CRYSTAL, 0, 90000u + (uint32_t)i, cell) == 0, "%s: bc_pack", tag);
    GbEditMon back; BcMeta meta;
    CHECK(bc_unpack(cell, &back, &meta), "%s: bc_unpack", tag);
    Gb12Mon got;
    CHECK(bc_view(&back, &meta, 0, &got), "%s: bc_view", tag);
    CHECK(got.has_caught_data == false, "%s: has_caught_data must be false (origin_game plays no part)", tag);
    CHECK(got.ot_gender == 0, "%s: ot_gender must be 0", tag);
    n++;
  }
  printf("  Crystal.sav: %d named uncaught-box7 regression slot(s) checked\n", n);
}

/* ============================================================================ */
/* 3. An all-zero cell is not native.                                            */
/* ============================================================================ */

static void test_zero_cell(void) {
  uint8_t cell[BC_CELL_BYTES];
  memset(cell, 0, sizeof cell);
  CHECK(bc_is_native(cell) == false, "an all-zero cell must not be native");
  CHECK(bc_kind(cell) == 0, "bc_kind must be 0 for a non-native cell");
}

/* ============================================================================ */
/* 4. No Gen-3 corpus record is ever misread as native.                         */
/* ============================================================================ */

static void sweep_gen3_native_check(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (cannot open)\n", path); return; }
  static uint8_t save[1 << 17];
  uint32_t n = (uint32_t)fread(save, 1, sizeof save, f);
  fclose(f);

  Gen3SaveInfo info;
  static uint8_t sb1scratch[G3_SAVEBLOCK1_BYTES];
  if (!gen3_parse_into(save, n, &info, sb1scratch)) {
    printf("  SKIP %s (parse failed)\n", path);
    return;
  }

  static uint8_t pc[G3_PC_BYTES];
  CHECK(gen3_read_pc_storage(save, info.slot, pc) == G3_PC_BYTES, "%s: gen3_read_pc_storage", path);

  int checked = 0;
  for (int box = 0; box < G3_TOTAL_BOXES; box++) {
    for (int slot = 0; slot < G3_IN_BOX; slot++) {
      uint8_t* rec = pk_box_slot(pc, box, slot);
      CHECK(bc_is_native(rec) == false, "%s: box %d slot %d misread as native", path, box, slot);
      checked++;
    }
  }

  uint8_t count = sb1scratch[SB1_OFF_PARTY_COUNT];
  if (count > G3_PARTY_SIZE) count = G3_PARTY_SIZE;
  for (int i = 0; i < count; i++) {
    const uint8_t* mon = sb1scratch + SB1_OFF_PARTY + (uint32_t)i * G3_MON_SIZE;
    CHECK(bc_is_native(mon) == false, "%s: party slot %d misread as native", path, i);
    checked++;
  }
  printf("  %s: %d Gen-3 record(s) confirmed non-native\n", path, checked);
}

/* ============================================================================ */
/* 5. Mutation: a one-bit flip in the hashed span breaks bc_is_native(); the     */
/*    excluded bytes (flags, rtc_epoch) and the reserved tail do not.           */
/* ============================================================================ */

static bool byte_is_hashed(unsigned off) {
  return (off >= 8 && off <= 10) || (off >= 12 && off <= 68) || (off >= 73 && off <= 76);
}

static void test_mutation(void) {
  GbEditMon mon;
  memset(&mon, 0, sizeof mon);
  mon.gen = GB_GEN1;
  mon.is_party = false;
  for (int i = 0; i < GEN1_BOX_REC_BYTES; i++) mon.rec[i] = (uint8_t)(i * 7 + 3);
  mon.list_species = 4;
  memcpy(mon.otname, "GUY\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  memcpy(mon.nick,   "MON\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);

  uint8_t base[BC_CELL_BYTES];
  CHECK(bc_pack(&mon, 0x00, BC_ORIGIN_RED, 0x99u, 42u, base) == 0, "mutation: bc_pack base");
  CHECK(bc_is_native(base), "mutation: base cell must be native before any flip");

  int broke = 0, hashed_bytes = 0;
  for (unsigned off = 8; off < BC_CELL_BYTES; off++) {
    if (off == BC_OFF_FLAGS) continue;                  /* checked separately below */
    if (off >= BC_OFF_RTC_EPOCH && off < BC_OFF_RTC_EPOCH + 4) continue;
    bool hashed = byte_is_hashed(off);
    if (hashed) hashed_bytes++;
    for (int bit = 0; bit < 8; bit++) {
      uint8_t cell[BC_CELL_BYTES];
      memcpy(cell, base, sizeof cell);
      cell[off] ^= (uint8_t)(1u << bit);
      bool native = bc_is_native(cell);
      if (hashed) {
        CHECK(!native, "mutation: byte %u bit %d (hashed) must break bc_is_native", off, bit);
        if (!native) broke++;
      } else {
        /* reserved tail (77..79): outside the hash and every other check */
        CHECK(native, "mutation: reserved byte %u bit %d must NOT break bc_is_native", off, bit);
      }
    }
  }
  CHECK(hashed_bytes == 3 + 57 + 4, "mutation: hashed-byte count must be 64 (3+57+4)");
  printf("  mutation: %d/%d hashed single-bit flips broke bc_is_native\n", broke, hashed_bytes * 8);

  /* Flipping the flags byte (11) does not change nativeness. */
  for (int bit = 0; bit < 8; bit++) {
    uint8_t cell[BC_CELL_BYTES];
    memcpy(cell, base, sizeof cell);
    cell[BC_OFF_FLAGS] ^= (uint8_t)(1u << bit);
    CHECK(bc_is_native(cell), "mutation: flags-byte bit %d flip must not break bc_is_native", bit);
  }
  /* Flipping rtc_epoch (69..72) does not change nativeness. */
  for (unsigned off = BC_OFF_RTC_EPOCH; off < BC_OFF_RTC_EPOCH + 4; off++) {
    for (int bit = 0; bit < 8; bit++) {
      uint8_t cell[BC_CELL_BYTES];
      memcpy(cell, base, sizeof cell);
      cell[off] ^= (uint8_t)(1u << bit);
      CHECK(bc_is_native(cell), "mutation: rtc_epoch byte %u bit %d flip must not break bc_is_native", off, bit);
    }
  }
}

/* ============================================================================ */
/* 6. Fresh bank_serial changes bytes 0..7; a flags/epoch-only re-pack does not. */
/* ============================================================================ */

static void test_serial_and_flags(void) {
  GbEditMon mon;
  memset(&mon, 0, sizeof mon);
  mon.gen = GB_GEN2;
  mon.is_party = false;
  for (int i = 0; i < G2_BOX_ENTRY; i++) mon.rec[i] = (uint8_t)(i * 11 + 5);
  mon.list_species = 7;
  memcpy(mon.otname, "GUY\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  memcpy(mon.nick,   "MON\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);

  uint8_t a[BC_CELL_BYTES], b[BC_CELL_BYTES], c[BC_CELL_BYTES];
  CHECK(bc_pack(&mon, 0x00, BC_ORIGIN_GOLD, 100u, 1u, a) == 0, "serial: pack a");
  CHECK(bc_pack(&mon, 0x00, BC_ORIGIN_GOLD, 100u, 2u, b) == 0, "serial: pack b (different serial)");
  CHECK(memcmp(a, b, 8) != 0, "serial: two different bank_serial values must differ in bytes 0..7");

  CHECK(bc_pack(&mon, BC_FLAG_QUEUED_PC | BC_FLAG_HAS_XFER_REC, BC_ORIGIN_GOLD, 555u, 1u, c) == 0,
        "serial: pack c (same serial, different flags/epoch)");
  CHECK(memcmp(a, c, 8) == 0,
        "serial: same bank_serial with different flags/epoch must NOT change bytes 0..7");
  CHECK(c[BC_OFF_FLAGS] == (BC_FLAG_QUEUED_PC | BC_FLAG_HAS_XFER_REC),
        "serial: flags byte itself still carries the caller's value");

  BcMeta meta;
  GbEditMon back;
  CHECK(bc_unpack(c, &back, &meta), "serial: unpack c");
  CHECK(meta.flags == (BC_FLAG_QUEUED_PC | BC_FLAG_HAS_XFER_REC), "serial: meta.flags round-trips");
  CHECK(meta.rtc_epoch == 555u, "serial: meta.rtc_epoch round-trips");
  CHECK(meta.bank_serial == 1u, "serial: meta.bank_serial round-trips");
}

/* ============================================================================ */
/* 7b. Directed egg + item-holder coverage. The real corpus (test 2 above) happened */
/*     to hold zero Gen-2 eggs on this run, so the acceptance list's "incl eggs and */
/*     item holders" is additionally pinned here with hand-built records rather    */
/*     than left to corpus luck -- the codec copies raw bytes regardless of what   */
/*     they mean, so a hand-built egg/held-item pattern exercises the same code    */
/*     path a corpus one would. */
/* ============================================================================ */

static void test_egg_and_item_directed(void) {
  /* Gen-2 egg: list byte 0xFD, record species field still the real species
   * (gen2_save.h: "the record keeps the real species"). */
  GbEditMon egg;
  memset(&egg, 0, sizeof egg);
  egg.gen = GB_GEN2;
  egg.is_party = false;
  egg.rec[0] = 1;                 /* species */
  egg.rec[1] = 0;                 /* held item: none */
  egg.list_species = G2_LIST_EGG; /* 0xFD */
  memcpy(egg.otname, "EGG\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  memcpy(egg.nick,   "EGG\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  roundtrip_one("directed Gen-2 egg", &egg, g_serial++);

  /* Gen-2 item holder: record byte 1 is the held-item id (gen2_save.h G2Mon.held_item). */
  GbEditMon holder2;
  memset(&holder2, 0, sizeof holder2);
  holder2.gen = GB_GEN2;
  holder2.is_party = false;
  holder2.rec[0] = 25;   /* Pikachu */
  holder2.rec[1] = 0x8D; /* Light Ball, per gb_sidecar/legacy item tables */
  holder2.list_species = 25;
  memcpy(holder2.otname, "GUY\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  memcpy(holder2.nick,   "PIKA\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  roundtrip_one("directed Gen-2 item holder", &holder2, g_serial++);

  /* Gen-1 "item holder": record byte 0x07 (catch_rate) doubling as a Gen-2 held item
   * after a Time Capsule trade (gb_edit.h's GbGen1Base comment) -- still just a raw
   * byte the codec must carry byte-exact regardless of what it means. */
  GbEditMon holder1;
  memset(&holder1, 0, sizeof holder1);
  holder1.gen = GB_GEN1;
  holder1.is_party = false;
  holder1.rec[0] = 3;    /* species internal index */
  holder1.rec[7] = 83;   /* catch_rate slot repurposed as an item post-trade */
  holder1.list_species = 3;
  memcpy(holder1.otname, "GUY\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  memcpy(holder1.nick,   "MON\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
  roundtrip_one("directed Gen-1 item-byte holder", &holder1, g_serial++);
}

/* ============================================================================ */
/* 7. bc_pack refuses the 0xFF list terminator (gb_commit's own rule).          */
/* ============================================================================ */

static void test_terminator_refused(void) {
  GbEditMon mon;
  memset(&mon, 0, sizeof mon);
  mon.gen = GB_GEN1;
  mon.list_species = GB_LIST_TERMINATOR;
  uint8_t cell[BC_CELL_BYTES];
  memset(cell, 0xAA, sizeof cell);
  int rc = bc_pack(&mon, 0, 0, 0, 0, cell);
  CHECK(rc < 0, "bc_pack must refuse list_species == 0xFF");
}

/* ============================================================================ */
/* 8. BACKLOG #150 S150-6, G-H6/review 1+8a: gbsc_merge_up refuses a bc_pack-      */
/*    produced original80; review 8a's kind-filter cross-resolve check.           */
/* ============================================================================ */

static void test_sidecar_native_refusal(void) {
  printf("== 8. gbsc_merge_up refuses a native original80 (G-H6) ==\n");

  GbEditMon mon; memset(&mon, 0, sizeof mon);
  mon.gen = GB_GEN2;
  mon.list_species = 1;
  mon.otname[0] = 0x81; memset(mon.otname + 1, 0x50, GB_NAME_BYTES - 1);
  mon.nick[0]   = 0x81; memset(mon.nick + 1,   0x50, GB_NAME_BYTES - 1);

  uint8_t cell[BC_CELL_BYTES];
  CHECK(bc_pack(&mon, 0, BC_ORIGIN_GOLD, 0x1234u, 1u, cell) == 0,
        "native-refusal: bc_pack builds a native cell");
  CHECK(bc_is_native(cell), "native-refusal: the packed cell IS native (precondition)");

  GbscEntry e; memset(&e, 0, sizeof e);
  e.gen = GB_GEN2;
  memcpy(e.original80, cell, 80);   /* a native cell masquerading as a sidecar's original */

  uint8_t out80[80]; memset(out80, 0xAA, sizeof out80);
  GbscMergeReport rep;
  bool ok = gbsc_merge_up(&e, &mon, out80, &rep);
  CHECK(!ok, "native-refusal: gbsc_merge_up returns false on a native original80");
  {
    uint8_t untouched[80]; memset(untouched, 0xAA, sizeof untouched);
    CHECK(memcmp(out80, untouched, 80) == 0,
          "native-refusal: out80 is left untouched (still the 0xAA sentinel)");
  }

  /* A real (non-native) original80 must still merge up normally -- the refusal is
   * specific to bc_is_native(), not a general breakage. */
  uint8_t real80[80]; memset(real80, 0, sizeof real80);   /* an all-zero cell is not native */
  CHECK(!bc_is_native(real80), "native-refusal: the all-zero control is NOT native (precondition)");
  GbscEntry e2 = e;
  memcpy(e2.original80, real80, 80);
  uint8_t out80b[80];
  GbscMergeReport rep2;
  CHECK(gbsc_merge_up(&e2, &mon, out80b, &rep2),
        "native-refusal: a non-native original80 still merges up normally");
}

/* review 8a: two same-key entries of different `kind` must never cross-resolve
 * through gbsc_find's want_kind filter. */
static void test_sidecar_kind_filter(void) {
  printf("== 8a. gbsc_find's want_kind filter never cross-resolves by kind ==\n");

  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = (uint32_t)gbsc_init(buf, 0x55u);
  uint8_t dv4[4] = { 1, 2, 3, 4 };
  uint8_t otname[GB_NAME_BYTES] = { 0x81, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50 };

  GbscEntry e0; memset(&e0, 0, sizeof e0);
  e0.gen = GB_GEN2; e0.otid16 = 0x2222; e0.kind = XR_KIND_G3_HOME;
  memcpy(e0.dv4, dv4, 4); memcpy(e0.otname_written, otname, GB_NAME_BYTES);
  CHECK(gbsc_add(buf, &len, sizeof buf, &e0) == 0, "kind-filter: G3_HOME entry added at 0");

  GbscEntry e1 = e0; e1.kind = XR_KIND_NATIVE_HOME;
  CHECK(gbsc_add(buf, &len, sizeof buf, &e1) == 1, "kind-filter: NATIVE_HOME entry added at 1 (same key)");

  GbEditMon probe; memset(&probe, 0, sizeof probe);
  probe.gen = GB_GEN2;
  gb_set_otid(&probe, 0x2222);
  gb_set_dv(&probe, GB_ATK, dv4[0]); gb_set_dv(&probe, GB_DEF, dv4[1]);
  gb_set_dv(&probe, GB_SPE, dv4[2]); gb_set_dv(&probe, GB_SPC, dv4[3]);
  gb_set_otname_raw(&probe, otname);

  CHECK(gbsc_find(buf, len, &probe, 0, true, XR_KIND_G3_HOME) == 0,
        "kind-filter: want_kind=G3_HOME resolves to entry 0, not 1");
  CHECK(gbsc_find(buf, len, &probe, 0, true, XR_KIND_NATIVE_HOME) == 1,
        "kind-filter: want_kind=NATIVE_HOME resolves to entry 1, not 0");
  CHECK(gbsc_find(buf, len, &probe, 0, true, -1) == 0,
        "kind-filter: want_kind<0 (any) still finds entry 0 first, same as before this parameter existed");
  CHECK(gbsc_find(buf, len, &probe, 1, true, -1) == 1,
        "kind-filter: any-kind walk from start=1 finds entry 1");

  /* the tiebreak-invariance check: want_kind >= 0 removes candidates, never
   * re-orders the survivors -- confirmed by re-walking with only ONE kind present. */
  uint8_t buf2[GBSC_FILE_MAX];
  uint32_t len2 = (uint32_t)gbsc_init(buf2, 0x66u);
  GbscEntry g0 = e0; g0.kind = XR_KIND_G3_HOME;
  GbscEntry g1 = e0; g1.kind = XR_KIND_G3_HOME;
  CHECK(gbsc_add(buf2, &len2, sizeof buf2, &g0) == 0, "kind-filter: same-kind entry 0 added");
  CHECK(gbsc_add(buf2, &len2, sizeof buf2, &g1) == 1, "kind-filter: same-kind entry 1 added");
  for (int start = 0; start <= 2; start++) {
    int any = gbsc_find(buf2, len2, &probe, start, true, -1);
    int kind = gbsc_find(buf2, len2, &probe, start, true, XR_KIND_G3_HOME);
    CHECK(any == kind,
          "kind-filter: start=%d -- want_kind filter (all-matching) gives the SAME index as any-kind (%d vs %d)",
          start, any, kind);
  }
}

/* ============================================================================ */
/* 11. BACKLOG #150 S150-4-5b item 1: raw-bytes section. gbs_open -> gbs_load_list
 *     -> gb_load -- the SAME session-open path gb_lift_up_hook's own
 *     gb_copy_native_hook uses (source/pdna_gen12.c) -- then bc_pack -> bc_unpack,
 *     byte-compared against the corpus save's OWN bytes at the slot's FILE offset.
 *     Offsets are independently re-derived HERE (never called out of
 *     gen1_save.c/gen2_save.c) from the .sym symbol tables -- REFERENCE ONLY,
 *     clean-room: numbers only, no symbol names carried into shipped code, no
 *     decomp source copied. Cross-checked against this codebase's own already-
 *     published constants:
 *       Gen-1 (assets/upstream/pokered/symbols/pokered.sym), bank:addr -> file
 *       offset = bank*0x2000 + (addr-0xA000):
 *         01:af2c -> 0x2F2C (party list start)      == GEN1_OFF_PARTY
 *         01:b0c0 -> 0x30C0 (live current-box copy)  == GEN1_OFF_CURRENT_BOX
 *         02:a000 -> 0x4000 (box 1, SRAM bank 2)      == GEN1_OFF_BANK2
 *       Gen-2 (assets/upstream/pokegold/symbols/pokegold.sym):
 *         02:a000 -> 0x4000 (box 1, SRAM bank 2)      == gen2_save.c's k_box_off[0]
 *     Per-slot placement within a list blob (species area / record / OT name /
 *     nickname) reuses gb_off_species/gb_off_record/gb_off_otname/gb_off_nickname
 *     (gb_edit.h) -- the SAME list-relative math gb_load itself already runs, so
 *     it is not re-derived a second time; the part actually independent here is
 *     the FILE-level list-start offset above, which gbs_load_list must land on
 *     through a wholly different path (Gen1Save/G2Writer parsing) than a direct
 *     img+off pointer (sections 1/2's sweep).
 *
 *     The Gen-1 party->box BOXLEVEL sync (G1R_BOXLEVEL <- G1R_LEVEL, gen1_write.h,
 *     the same rule gbs_move() applies) is the ONE known, named, documented
 *     difference between a party slot's raw box-shape prefix and its packed
 *     cell -- a mon that leveled up in the field legitimately carries a stale
 *     byte at G1R_BOXLEVEL in the raw save until its next deposit. Applied below
 *     from raw file bytes only (never from gb_load's own fields), so the check
 *     stays independent of bank_cell.c's own copy of the same rule.
 *
 *     NOT carried by the cell (BACKLOG #150 S150-1's own record shape) --
 *     documented, not asserted, because there is nothing IN the cell to compare
 *     it against:
 *       - the party TAIL: Gen-1 rec[33..43] / Gen-2 rec[32..47] (status, HP, the
 *         five battle stats) -- the cell only ever holds the box-shape PREFIX, so
 *         a party slot's raw comparison below is a prefix match (33 / 32 bytes).
 *       - Gen-2 Mail (sPartyMail and its five sPartyMonNMail sub-blocks, SRAM
 *         bank 0 offset 0x600) -- a wholly separate SRAM block outside every
 *         party/box record; gb_load never reads it and bc_pack never packs it.
 * ============================================================================ */

/* Gen-1 list-start FILE offset -- independently re-derived from the three .sym
 * addresses in the header comment above, not called out of gen1_save.c. */
static uint32_t raw_g1_list_off(int current_box, int box) {
  if (box == GEN1_PARTY_BOX) return 0x2F2Cu;
  if (box == current_box)    return 0x30C0u;
  return (box < 6 ? 0x4000u : 0x6000u) + (uint32_t)(box % 6) * GEN1_BOX_BYTES;
}

/* One Gen-1 slot: raw file bytes at `list_off` vs. gb_load's `mon` vs. bc_pack's
 * cell, all three compared. `cmp_len` is always the 33-byte box-shape prefix --
 * even for a party slot, whose own rec[] is 44 bytes wide (the extra 11-byte tail
 * is the documented exclusion above). */
static void raw_check_g1_slot(const char* file, int box, int slot, uint32_t list_off,
                              const uint8_t* img, uint32_t len, const GbEditMon* mon) {
  char tag[96];
  snprintf(tag, sizeof tag, "%s RAW box%d/slot%d", file, box, slot);

  int sp_off  = gb_off_species(GB_GEN1, box, slot);
  int rec_off = gb_off_record(GB_GEN1, box, slot);
  int ot_off  = gb_off_otname(GB_GEN1, box, slot);
  int nk_off  = gb_off_nickname(GB_GEN1, box, slot);
  CHECK(sp_off >= 0 && rec_off >= 0 && ot_off >= 0 && nk_off >= 0,
        "%s: gb_off_* resolved", tag);
  if (sp_off < 0 || rec_off < 0 || ot_off < 0 || nk_off < 0) return;

  uint32_t sp_file = list_off + (uint32_t)sp_off;
  uint32_t rec_file = list_off + (uint32_t)rec_off;
  uint32_t ot_file = list_off + (uint32_t)ot_off;
  uint32_t nk_file = list_off + (uint32_t)nk_off;
  CHECK(sp_file < len && rec_file + GEN1_PARTY_REC_BYTES <= len &&
        ot_file + GB_NAME_BYTES <= len && nk_file + GB_NAME_BYTES <= len,
        "%s: raw offsets in range", tag);
  if (sp_file >= len || rec_file + GEN1_PARTY_REC_BYTES > len ||
      ot_file + GB_NAME_BYTES > len || nk_file + GB_NAME_BYTES > len) return;

  CHECK(img[sp_file] == mon->list_species, "%s: species (list) byte mismatch", tag);

  /* Independently-built expected 33-byte box-shape prefix: raw bytes verbatim,
   * with the ONE documented party->box transform applied from raw bytes only. */
  uint8_t expect33[GEN1_BOX_REC_BYTES];
  memcpy(expect33, img + rec_file, GEN1_BOX_REC_BYTES);
  if (box == GEN1_PARTY_BOX) expect33[G1R_BOXLEVEL] = img[rec_file + G1R_LEVEL];

  uint8_t cell[BC_CELL_BYTES];
  CHECK(bc_pack(mon, mon->is_party ? BC_FLAG_FROM_PARTY : 0, BC_ORIGIN_RED, 0,
               g_serial++, cell) == 0, "%s: bc_pack", tag);
  GbEditMon back; BcMeta meta;
  CHECK(bc_unpack(cell, &back, &meta), "%s: bc_unpack", tag);

  CHECK(memcmp(expect33, back.rec, GEN1_BOX_REC_BYTES) == 0,
        "%s: cell rec[0..33) mismatch vs. raw save bytes", tag);
  CHECK(memcmp(img + ot_file, back.otname, GB_NAME_BYTES) == 0,
        "%s: cell OT name mismatch vs. raw save bytes", tag);
  CHECK(memcmp(img + nk_file, back.nick, GB_NAME_BYTES) == 0,
        "%s: cell nickname mismatch vs. raw save bytes", tag);
  printf("  RAW: %s box%d/slot%d ok\n", file, box, slot);
}

static void raw_sweep_gen1(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (raw-bytes, not present)\n", file); return; }
  static uint8_t img[GEN1_SAVE_SIZE];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  GbSession sess; memset(&sess, 0, sizeof sess);
  static uint8_t scratch[GBS_SCRATCH_BYTES];
  GbsStatus st = gbs_open(&sess, img, len, scratch, sizeof scratch);
  CHECK(st == GBS_OK, "%s: raw gbs_open (%s)", file, gbs_status_text(st));
  if (st != GBS_OK) return;

  int nboxes = gbs_nboxes(&sess);
  int party_box = gbs_party_box(&sess);
  static uint8_t list[GBS_LIST_BYTES];
  int n = 0;
  for (int box = 0; box <= nboxes; box++) {
    int b = (box == nboxes) ? party_box : box;
    if (gbs_load_list(&sess, b, list) != GBS_OK) continue;
    int count = gb_list_count(GB_GEN1, list, b);
    if (count < 0) continue;
    uint32_t list_off = raw_g1_list_off(sess.g1.current_box, b);
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96]; snprintf(tag, sizeof tag, "%s box%d slot%d", file, b, slot);
      CHECK(gb_load(&mon, GB_GEN1, list, b, slot), "%s: gb_load", tag);
      raw_check_g1_slot(file, b, slot, list_off, img, len, &mon);
      n++;
    }
  }
  printf("  %s: %d raw-bytes Gen-1 slot(s) checked\n", file, n);
}

/* Gen-2 list-start FILE offset -- independently re-derived from the ONE .sym
 * cross-check in the header comment (sBox1 -> 0x4000) plus the per-version
 * header-field addresses g2_offsets() already exports (party_list/
 * current_box_list are themselves read STRAIGHT off g2_offsets(), the only
 * per-version constants this module has -- box 1..14's own stride, 0x450, is
 * this codebase's own already-published constant, not re-derived a second time
 * from raw .sym addresses within the time available). */
static uint32_t raw_g2_list_off(G2Version ver, int current_box, int box) {
  G2Offsets o; g2_offsets(ver, &o);
  if (box == G2_BOX_PARTY) return o.party_list;
  if (box == current_box)  return o.current_box_list;
  uint32_t bank_base = (box < 7) ? 0x4000u : 0x6000u;
  return bank_base + (uint32_t)(box % 7) * 0x450u;
}

static void raw_check_g2_slot(const char* file, int box, int slot, uint32_t list_off,
                              const uint8_t* img, uint32_t len, const GbEditMon* mon) {
  char tag[96];
  snprintf(tag, sizeof tag, "%s RAW box%d/slot%d", file, box, slot);

  int sp_off  = gb_off_species(GB_GEN2, box, slot);
  int rec_off = gb_off_record(GB_GEN2, box, slot);
  int ot_off  = gb_off_otname(GB_GEN2, box, slot);
  int nk_off  = gb_off_nickname(GB_GEN2, box, slot);
  CHECK(sp_off >= 0 && rec_off >= 0 && ot_off >= 0 && nk_off >= 0,
        "%s: gb_off_* resolved", tag);
  if (sp_off < 0 || rec_off < 0 || ot_off < 0 || nk_off < 0) return;

  uint32_t sp_file = list_off + (uint32_t)sp_off;
  uint32_t rec_file = list_off + (uint32_t)rec_off;
  uint32_t ot_file = list_off + (uint32_t)ot_off;
  uint32_t nk_file = list_off + (uint32_t)nk_off;
  CHECK(sp_file < len && rec_file + G2_PARTY_ENTRY <= len &&
        ot_file + GB_NAME_BYTES <= len && nk_file + GB_NAME_BYTES <= len,
        "%s: raw offsets in range", tag);
  if (sp_file >= len || rec_file + G2_PARTY_ENTRY > len ||
      ot_file + GB_NAME_BYTES > len || nk_file + GB_NAME_BYTES > len) return;

  CHECK(img[sp_file] == mon->list_species, "%s: species (list) byte mismatch", tag);

  uint8_t cell[BC_CELL_BYTES];
  CHECK(bc_pack(mon, mon->is_party ? BC_FLAG_FROM_PARTY : 0, BC_ORIGIN_GOLD, 0,
               g_serial++, cell) == 0, "%s: bc_pack", tag);
  GbEditMon back; BcMeta meta;
  CHECK(bc_unpack(cell, &back, &meta), "%s: bc_unpack", tag);

  /* Gen-2 keeps the first 32 bytes verbatim -- no boxlevel-style sync exists on
   * this generation (bank_cell.h's own header comment), so the raw prefix
   * compares directly with no transform. */
  CHECK(memcmp(img + rec_file, back.rec, G2_BOX_ENTRY) == 0,
        "%s: cell rec[0..32) mismatch vs. raw save bytes", tag);
  CHECK(memcmp(img + ot_file, back.otname, GB_NAME_BYTES) == 0,
        "%s: cell OT name mismatch vs. raw save bytes", tag);
  CHECK(memcmp(img + nk_file, back.nick, GB_NAME_BYTES) == 0,
        "%s: cell nickname mismatch vs. raw save bytes", tag);
  printf("  RAW: %s box%d/slot%d ok\n", file, box, slot);
}

static void raw_sweep_gen2(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (raw-bytes, not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  GbSession sess; memset(&sess, 0, sizeof sess);
  static uint8_t scratch[GBS_SCRATCH_BYTES];
  GbsStatus st = gbs_open(&sess, img, len, scratch, sizeof scratch);
  CHECK(st == GBS_OK, "%s: raw gbs_open (%s)", file, gbs_status_text(st));
  if (st != GBS_OK) return;

  int nboxes = gbs_nboxes(&sess);
  int party_box = gbs_party_box(&sess);
  static uint8_t list[GBS_LIST_BYTES];
  int n = 0;
  for (int box = 0; box <= nboxes; box++) {
    int b = (box == nboxes) ? party_box : box;
    if (gbs_load_list(&sess, b, list) != GBS_OK) continue;
    int count = gb_list_count(GB_GEN2, list, b);
    if (count < 0) continue;
    uint32_t list_off = raw_g2_list_off(sess.g2w.sv.version, sess.g2w.current_box, b);
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96]; snprintf(tag, sizeof tag, "%s box%d slot%d", file, b, slot);
      CHECK(gb_load(&mon, GB_GEN2, list, b, slot), "%s: gb_load", tag);
      raw_check_g2_slot(file, b, slot, list_off, img, len, &mon);
      n++;
    }
  }
  printf("  %s: %d raw-bytes Gen-2 slot(s) checked\n", file, n);
}

/* 9. bank_plant (BACKLOG #150 S150-2 step 6): the PDNA_DELTA-only test plant.   */
/*    Compiled in only when the cc line above defines -DPDNA_DELTA; the shipped  */
/*    build never does, and D-Q7's own nm proof (the lane's delivery report)     */
/*    confirms no bank_plant symbol reaches either gate ELF.                    */
/* ============================================================================ */
#ifdef PDNA_DELTA
#include "bank_plant.h"

#define PLANT_BOX_BYTES (30 * BC_CELL_BYTES)   /* 2400 */

static void check_plant_box0(const uint8_t recs[PLANT_BOX_BYTES]) {
  /* slots 0,1,2,3 are native and each converts to something (FULL/RELAXED); slot 4
   * is native but unrepresentable (GB_SHOW_NONE) -- bc_is_native is still true for
   * ALL FIVE (that is the whole point of G-H2: occupancy comes from the raw bytes,
   * not from whether a stand-in could be built). Slots 5..29 are untouched (all-zero
   * -- ordinary empty Gen-3 slots). */
  for (int s = 0; s < 5; s++)
    CHECK(bc_is_native(recs + (uint32_t)s * BC_CELL_BYTES), "plant box0 slot %d must be native", s);
  for (int s = 5; s < 30; s++) {
    uint8_t zero[BC_CELL_BYTES]; memset(zero, 0, sizeof zero);
    CHECK(memcmp(recs + (uint32_t)s * BC_CELL_BYTES, zero, BC_CELL_BYTES) == 0,
          "plant box0 slot %d must be untouched (all-zero)", s);
  }
  /* slot 4's own bytes decode back to the glitch species + BC_ORIGIN_GOLD this file
   * packed it with -- an independent re-check that bc_pack really did carry the
   * 0xFE glitch through, not just "is native". */
  GbEditMon back; BcMeta meta;
  CHECK(bc_unpack(recs + (uint32_t)4 * BC_CELL_BYTES, &back, &meta), "plant box0 slot 4: bc_unpack");
  CHECK(back.list_species == 0xFE, "plant box0 slot 4: list_species must be the 0xFE glitch index");
  CHECK(meta.origin_game == BC_ORIGIN_GOLD, "plant box0 slot 4: origin_game must be GOLD");
}

static void check_plant_box_full(const uint8_t recs[PLANT_BOX_BYTES]) {
  int n = 0;
  uint32_t idents[30];
  for (int s = 0; s < 30; s++) {
    const uint8_t* cell = recs + (uint32_t)s * BC_CELL_BYTES;
    CHECK(bc_is_native(cell), "plant box_full slot %d must be native (the 30-NATIVE worst case)", s);
    idents[s] = bc_ident32(cell);
    if (bc_is_native(cell)) n++;
  }
  CHECK(n == 30, "plant box_full must have 30/30 native slots, got %d", n);
  for (int a = 0; a < 30; a++)
    for (int b = a + 1; b < 30; b++)
      CHECK(idents[a] != idents[b], "plant box_full: slots %d and %d share an ident32 (bank_serial collision)", a, b);
}

/* PDNA_EMIT_PLANT=1 (never by default, never into the repo -- .gitignore covers
 * /box14.box and /box15.box): write the raw 2400-byte buffers to the CURRENT
 * directory as box15.box (the five directed cells) and box14.box (the 30-native
 * worst case) for a real-hardware SD-card copy to /PokeDNA/bank/box15.box and
 * /PokeDNA/bank/box14.box -- BANK_BOXES is 16 (pdna_bank.c) and box_path is
 * "box%02d.box", so those are BANK 16 and BANK 15, the two highest boxes. */
static void maybe_emit_plant(const char* name, const uint8_t recs[PLANT_BOX_BYTES]) {
  if (!getenv("PDNA_EMIT_PLANT") || strcmp(getenv("PDNA_EMIT_PLANT"), "1") != 0) return;
  FILE* f = fopen(name, "wb");
  if (!f) { printf("  !! could not open %s for PDNA_EMIT_PLANT\n", name); return; }
  size_t wr = fwrite(recs, 1, PLANT_BOX_BYTES, f);
  fclose(f);
  printf("  PDNA_EMIT_PLANT: wrote %s (%zu bytes)\n", name, wr);
}

static void test_bank_plant(void) {
  static uint8_t box0[PLANT_BOX_BYTES];
  static uint8_t boxfull[PLANT_BOX_BYTES];
  memset(box0, 0, sizeof box0);
  memset(boxfull, 0, sizeof boxfull);

  bank_plant_box0(box0);
  check_plant_box0(box0);
  maybe_emit_plant("box15.box", box0);

  bank_plant_box_full(boxfull);
  check_plant_box_full(boxfull);
  maybe_emit_plant("box14.box", boxfull);
}
#endif /* PDNA_DELTA */

/* ============================================================================ */
/* 10. BACKLOG #150 S150-14: unpack -> mutate through the EDITOR'S OWN API      */
/*     (gbe_press/gbe_adjust/gbe_set_move, never a direct rec[] poke) -> re-pack */
/*     per decision 4 -- the exact sequence gb_native_summary_open() now runs   */
/*     on a confirmed edit. */
/* ============================================================================ */

/* decision 4's re-pack, verbatim -- kept as one helper so every case below runs
 * the SAME flag-derivation gb_native_summary_open() itself uses. */
static int native_repack(const GbEditMon* e, const BcMeta* meta, uint8_t out80[BC_CELL_BYTES]) {
  uint8_t nf = (uint8_t)(meta->flags & (BC_FLAG_FROM_PARTY | BC_FLAG_HAS_XFER_REC | BC_FLAG_QUEUED_PC));
  if (gb_is_egg(e))        nf |= BC_FLAG_EGG;
  if (gb_get_held_item(e)) nf |= BC_FLAG_HOLDS_ITEM;
  return bc_pack(e, nf, meta->origin_game, meta->rtc_epoch, meta->bank_serial, out80);
}

static void test_native_edit_roundtrip(void) {
  /* ---- Gen-1: a DV change through gbe_press (0<->15 extreme jump). b0 (from-party)
   * and b1 (has-xfer-record) must survive verbatim (Bank/ledger state a GbEditMon
   * cannot carry); bank_serial/origin_game/rtc_epoch must survive; ident32 must
   * change (the edit touches rec[], inside the hashed span). */
  {
    GbEditMon mon;
    memset(&mon, 0, sizeof mon);
    mon.gen = GB_GEN1;
    mon.is_party = false;
    for (int i = 0; i < GEN1_BOX_REC_BYTES; i++) mon.rec[i] = (uint8_t)(i * 5 + 1);
    mon.rec[0] = 1;                        /* species (arbitrary, non-zero) */
    mon.list_species = 1;
    memcpy(mon.otname, "GUY\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
    memcpy(mon.nick,   "MON\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);

    uint8_t before[BC_CELL_BYTES];
    uint8_t flags_in = (uint8_t)(BC_FLAG_FROM_PARTY | BC_FLAG_HAS_XFER_REC);
    CHECK(bc_pack(&mon, flags_in, BC_ORIGIN_RED, 0x1234u, 77u, before) == 0, "10 g1: bc_pack base");
    CHECK(bc_is_native(before), "10 g1: base cell native");

    GbEditMon e; BcMeta meta;
    CHECK(bc_unpack(before, &e, &meta), "10 g1: bc_unpack");
    CHECK(meta.origin_game == BC_ORIGIN_RED, "10 g1: meta.origin_game round-trips");
    CHECK(meta.rtc_epoch == 0x1234u, "10 g1: meta.rtc_epoch round-trips");
    CHECK(meta.bank_serial == 77u, "10 g1: meta.bank_serial round-trips");

    CHECK(gbe_press(&e, GBE_DVA), "10 g1: gbe_press(GBE_DVA) must change the record");

    uint8_t after[BC_CELL_BYTES];
    CHECK(native_repack(&e, &meta, after) == 0, "10 g1: re-pack");
    CHECK(bc_is_native(after), "10 g1: re-packed cell still native");

    CHECK(memcmp(before + BC_OFF_BANK_SERIAL, after + BC_OFF_BANK_SERIAL, 4) == 0,
          "10 g1: bank_serial (bytes 73..76) identical");
    CHECK(before[BC_OFF_ORIGIN_GAME] == after[BC_OFF_ORIGIN_GAME],
          "10 g1: origin_game (byte 68) identical");
    CHECK(memcmp(before + BC_OFF_RTC_EPOCH, after + BC_OFF_RTC_EPOCH, 4) == 0,
          "10 g1: rtc_epoch (bytes 69..72) identical");
    CHECK(memcmp(before + BC_OFF_IDENT32, after + BC_OFF_IDENT32, 4) != 0,
          "10 g1: ident32 (bytes 4..7) CHANGED by the DV edit");
    CHECK((after[BC_OFF_FLAGS] & (BC_FLAG_FROM_PARTY | BC_FLAG_HAS_XFER_REC)) ==
          (BC_FLAG_FROM_PARTY | BC_FLAG_HAS_XFER_REC),
          "10 g1: b0/b1 (from-party, has-xfer-record) preserved -- Bank/ledger state a GbEditMon cannot carry");
    CHECK(after[BC_OFF_OLDBUILD] == BC_OLDBUILD_BYTE, "10 g1: byte 19 still 0x01 (the old-build guard) after re-pack");

    GbEditMon back; BcMeta meta2;
    CHECK(bc_unpack(after, &back, &meta2), "10 g1: second bc_unpack");
    CHECK(memcmp(&back, &e, sizeof back) == 0,
          "10 g1: a second bc_unpack returns the mutated mon field-for-field");
  }

  /* ---- Gen-2 item holder: b2 (queued-for-PC) must survive; clearing the held item
   * through gbe_press(GBE_ITEM) must clear b4 (holds-item) on re-pack -- b3/b4 are
   * the two bits the editor can really flip and must be re-derived, not carried. */
  {
    GbEditMon mon;
    memset(&mon, 0, sizeof mon);
    mon.gen = GB_GEN2;
    mon.is_party = false;
    for (int i = 0; i < G2_BOX_ENTRY; i++) mon.rec[i] = (uint8_t)(i * 3 + 2);
    mon.rec[0] = 25;      /* Pikachu */
    mon.rec[1] = 0x8D;    /* held item -- Light Ball */
    mon.list_species = 25;
    memcpy(mon.otname, "GUY\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
    memcpy(mon.nick,   "PIKA\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);

    uint8_t before[BC_CELL_BYTES];
    uint8_t flags_in = (uint8_t)(BC_FLAG_QUEUED_PC | BC_FLAG_HOLDS_ITEM);
    CHECK(bc_pack(&mon, flags_in, BC_ORIGIN_GOLD, 9u, 200u, before) == 0, "10 g2: bc_pack base");
    CHECK((before[BC_OFF_FLAGS] & BC_FLAG_HOLDS_ITEM) != 0, "10 g2: base cell has b4 (holds-item) set");

    GbEditMon e; BcMeta meta;
    CHECK(bc_unpack(before, &e, &meta), "10 g2: bc_unpack");
    CHECK(gb_get_held_item(&e) != 0, "10 g2: unpacked mon really is holding an item");

    CHECK(gbe_press(&e, GBE_ITEM), "10 g2: gbe_press(GBE_ITEM) must change the record");
    CHECK(gb_get_held_item(&e) == 0, "10 g2: item now cleared on `e`");

    uint8_t after[BC_CELL_BYTES];
    CHECK(native_repack(&e, &meta, after) == 0, "10 g2: re-pack");

    CHECK((after[BC_OFF_FLAGS] & BC_FLAG_QUEUED_PC) != 0, "10 g2: b2 (queued-for-PC) preserved");
    CHECK((after[BC_OFF_FLAGS] & BC_FLAG_HOLDS_ITEM) == 0,
          "10 g2: b4 (holds-item) re-derived to 0 -- the item really was cleared");
    CHECK(memcmp(before + BC_OFF_BANK_SERIAL, after + BC_OFF_BANK_SERIAL, 4) == 0,
          "10 g2: bank_serial identical");
    CHECK(before[BC_OFF_ORIGIN_GAME] == after[BC_OFF_ORIGIN_GAME], "10 g2: origin_game identical");
    CHECK(memcmp(before + BC_OFF_RTC_EPOCH, after + BC_OFF_RTC_EPOCH, 4) == 0, "10 g2: rtc_epoch identical");
    CHECK(memcmp(before + BC_OFF_IDENT32, after + BC_OFF_IDENT32, 4) != 0,
          "10 g2: ident32 CHANGED (the item-clear touches rec[1], inside the hashed span)");
  }

  /* ---- A flags/epoch-only re-pack (no editor mutation of `e` at all) leaves
   * ident32 UNCHANGED -- the G-M4 invariant, still true through this exact
   * unpack -> [no edit] -> re-pack call shape. */
  {
    GbEditMon mon;
    memset(&mon, 0, sizeof mon);
    mon.gen = GB_GEN1;
    mon.is_party = false;
    for (int i = 0; i < GEN1_BOX_REC_BYTES; i++) mon.rec[i] = (uint8_t)(i * 13 + 9);
    mon.rec[0] = 4;
    mon.list_species = 4;
    memcpy(mon.otname, "GUY\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);
    memcpy(mon.nick,   "MON\x50\x50\x50\x50\x50\x50\x50\x50", GB_NAME_BYTES);

    uint8_t before[BC_CELL_BYTES];
    CHECK(bc_pack(&mon, BC_FLAG_FROM_PARTY, BC_ORIGIN_RED, 1u, 50u, before) == 0,
          "10 flags-only: bc_pack base");

    GbEditMon e; BcMeta meta;
    CHECK(bc_unpack(before, &e, &meta), "10 flags-only: bc_unpack");
    meta.rtc_epoch += 1u;                       /* epoch-only bump, still outside the hash */
    uint8_t after[BC_CELL_BYTES];
    CHECK(native_repack(&e, &meta, after) == 0, "10 flags-only: re-pack (epoch bumped, nothing else)");
    CHECK(memcmp(before + BC_OFF_IDENT32, after + BC_OFF_IDENT32, 4) == 0,
          "10 flags-only: ident32 unchanged by a flags/epoch-only re-pack (G-M4)");
  }
}

int main(int argc, char** argv) {
  printf("== 1. Gen-1 corpus sweep ==\n");
  sweep_gen1("Red.sav");
  sweep_gen1("Yellow.sav");

  printf("== 2. Gen-2 corpus sweep ==\n");
  sweep_gen2("Gold.sav");
  sweep_gen2("Crystal.sav");

  printf("== 2b. named regression: Crystal.sav uncaught party slots ==\n");
  regress_crystal_uncaught_box7();

  printf("== 3. all-zero cell ==\n");
  test_zero_cell();

  printf("== 4. Gen-3 corpus: never misread as native ==\n");
  for (int i = 1; i < argc; i++) sweep_gen3_native_check(argv[i]);
  if (argc < 2) printf("  (no Gen-3 .sav given on argv -- run via run_host_tests.py)\n");

  printf("== 5. mutation ==\n");
  test_mutation();

  printf("== 6. fresh bank_serial vs. flags/epoch-only re-pack ==\n");
  test_serial_and_flags();

  printf("== 7b. directed egg + item-holder coverage ==\n");
  test_egg_and_item_directed();

  printf("== 7. 0xFF list terminator refused ==\n");
  test_terminator_refused();

  test_sidecar_native_refusal();
  test_sidecar_kind_filter();
#ifdef PDNA_DELTA
  printf("== 9. bank_plant (PDNA_DELTA-only) ==\n");
  test_bank_plant();
#endif

  printf("== 10. S150-14 native-cell EDIT round trip (editor API -> re-pack) ==\n");
  test_native_edit_roundtrip();

  printf("== 11. raw-bytes: gbs_open -> gbs_load_list -> gb_load -> bc_pack vs. the save's own file bytes ==\n");
  raw_sweep_gen1("Red.sav");
  raw_sweep_gen1("Yellow.sav");
  raw_sweep_gen2("Gold.sav");
  raw_sweep_gen2("Crystal.sav");

  printf("\n%d check(s), %s\n", g_check, g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
