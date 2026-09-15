/* Host test for source/bank_cell.{c,h} -- the tagged 80-byte native Bank cell codec
 * (BACKLOG #150 S150-1). Pure C, dual-compiles on the host; the module under test has
 * ZERO callers in the shipped build, so this file is the only thing exercising it.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_bankcell_test.c \
 *      source/bank_cell.c source/gb_edit.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/gen3_save.c source/gen3_box.c \
 *      source/gen3_mon.c source/gen3_edit.c source/gen3_daycare.c source/gen3_clip.c \
 *      source/gen12_convert.c source/data_tables.c -o /tmp/hbc
 *   /tmp/hbc (Guy's five Gen-3 .sav files, positional argv -- run_host_tests.py hands
 *             them over automatically)
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

#include "bank_cell.h"
#include "gb_edit.h"
#include "gen1_save.h"
#include "gen1_write.h"
#include "gen2_save.h"
#include "gen3_save.h"
#include "gen3_box.h"
#include "gen3_mon.h"
#include "gen3_clip.h"

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

  printf("\n%d check(s), %s\n", g_check, g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
