/* Host test for source/gb12_render.{c,h} -- the Gen-1/2 -> Gen-3-box-record DISPLAY
 * LADDER factored out of pdna_gen12.c's gb_build_slot (BACKLOG #150 S150-2).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gb12render_test.c \
 *      source/gb12_render.c source/gen12_convert.c source/gen3_mon.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/gen3_save.c source/gen3_box.c source/data_tables.c \
 *      -o /tmp/hgbr && /tmp/hgbr
 *
 * Takes no arguments and reads no files -- every input is a synthetic Gb12Mon built by
 * hand in this file, one per rung of the ladder (docs/BANK-CROSSGEN-DESIGN.md SS11.5):
 *   1. a clean Gen-2 mon converts FULL.
 *   2. an egg converts RELAXED and is drawn back as an Egg (em_set_egg's mark survives
 *      the round trip through the placeholder/relaxed conversion).
 *   3. an item holder converts RELAXED with the item dropped (heldItem == 0 downstream --
 *      Gen 3's own PC-box record has no held-item field to carry it in anyway).
 *   4. a damaged move list (moves[0] == 0) with a real species converts PLACEHOLDER.
 *   5. dex 0 (with otherwise-valid fields, so it hits GB12_ERR_SPECIES rather than the
 *      EMPTY short-circuit) converts NONE, and rec80 comes back all-zero.
 *   6. a GOLDEN placeholder_pid value, hand-computed from decision 3's documented 8-byte
 *      FNV-1a sequence for box=3, slot=7 (id_salt = 3*20+7 = 67), proving the derivation
 *      is byte-exact and the moved code did not silently change the hash.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "gb12_render.h"
#include "gen12_convert.h"
#include "gen3_mon.h"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* A valid, convertible Gen-2 mon: species 1 (Bulbasaur), level 10, one real move,
 * no egg, no held item -- every field gen12_can_convert checks is inside range. */
static Gb12Mon base_mon(void) {
  Gb12Mon m; memset(&m, 0, sizeof m);
  m.gen = 2;
  m.species_dex = 1;
  m.exp = 1000;
  m.level = 10;
  m.dv_atk = 8; m.dv_def = 9; m.dv_spd = 10; m.dv_spc = 11;
  m.moves[0] = 33;   /* Tackle */
  m.ot_id = 12345;
  strcpy(m.ot_name, "RED");
  strcpy(m.nickname, "BULBA");
  return m;
}

static Gb12Target tgt3(void) { Gb12Target t; t.met_game = 3; return t; }

/* ---- 1. clean mon -> FULL ------------------------------------------------------ */
static void t_full(void) {
  Gb12Mon m = base_mon();
  Gb12Target t = tgt3();
  uint8_t rec[80]; uint8_t reason = 0xFF;
  int rung = gb12_render_rec(&m, &t, 42u, rec, &reason);
  CHECK(rung == GB_SHOW_FULL, "clean mon: expected GB_SHOW_FULL, got %d", rung);
  CHECK(reason == GB12_OK, "clean mon: expected GB12_OK, got %d", reason);
  PkMon out; bool ok = pk_decode_mon(rec, false, &out);
  CHECK(ok, "clean mon: pk_decode_mon failed on the FULL record");
  CHECK(out.species != 0, "clean mon: FULL record decoded to species 0");
}

/* ---- 2. egg -> RELAXED, drawn back as an Egg ------------------------------------ */
static void t_egg(void) {
  Gb12Mon m = base_mon();
  m.is_egg = true;
  Gb12Target t = tgt3();
  uint8_t rec[80]; uint8_t reason = 0xFF;
  int rung = gb12_render_rec(&m, &t, 7u, rec, &reason);
  CHECK(rung == GB_SHOW_RELAXED, "egg: expected GB_SHOW_RELAXED, got %d", rung);
  CHECK(reason == GB12_ERR_EGG, "egg: expected GB12_ERR_EGG, got %d", reason);
  PkMon out; bool ok = pk_decode_mon(rec, false, &out);
  CHECK(ok, "egg: pk_decode_mon failed on the RELAXED record");
  CHECK(out.isEgg, "egg: RELAXED record was not drawn back as an Egg");
}

/* ---- 3. item holder -> RELAXED, item zeroed ------------------------------------- */
static void t_item(void) {
  Gb12Mon m = base_mon();
  m.held_item = 12;   /* some non-zero Gen-2 item id */
  Gb12Target t = tgt3();
  uint8_t rec[80]; uint8_t reason = 0xFF;
  int rung = gb12_render_rec(&m, &t, 9u, rec, &reason);
  CHECK(rung == GB_SHOW_RELAXED, "item holder: expected GB_SHOW_RELAXED, got %d", rung);
  CHECK(reason == GB12_ERR_HELD_ITEM, "item holder: expected GB12_ERR_HELD_ITEM, got %d", reason);
  PkMon out; bool ok = pk_decode_mon(rec, false, &out);
  CHECK(ok, "item holder: pk_decode_mon failed on the RELAXED record");
  CHECK(out.heldItem == 0, "item holder: RELAXED record still carries a held item (%u)", out.heldItem);
}

/* ---- 4. damaged move list, real species -> PLACEHOLDER -------------------------- */
static void t_placeholder(void) {
  Gb12Mon m = base_mon();
  m.moves[0] = 0;   /* GB12_ERR_MOVE: an empty first move slot on an otherwise-real mon */
  Gb12Target t = tgt3();
  uint8_t rec[80]; uint8_t reason = 0xFF;
  int rung = gb12_render_rec(&m, &t, 3u, rec, &reason);
  CHECK(rung == GB_SHOW_PLACEHOLDER, "damaged moves: expected GB_SHOW_PLACEHOLDER, got %d", rung);
  CHECK(reason == GB12_ERR_MOVE, "damaged moves: expected GB12_ERR_MOVE, got %d", reason);
  PkMon out; bool ok = pk_decode_mon(rec, false, &out);
  CHECK(ok, "damaged moves: pk_decode_mon failed on the PLACEHOLDER record");
  CHECK(out.species != 0, "damaged moves: PLACEHOLDER record decoded to species 0");
}

/* ---- 5. dex 0 -> NONE, all-zero rec80 ------------------------------------------- */
static void t_none(void) {
  Gb12Mon m = base_mon();
  m.species_dex = 0;   /* level/exp/moves[0] stay non-zero: GB12_ERR_SPECIES, not EMPTY */
  Gb12Target t = tgt3();
  uint8_t rec[80]; uint8_t reason = 0xFF;
  memset(rec, 0xAA, sizeof rec);   /* poison first: a real zero has to come FROM the ladder */
  int rung = gb12_render_rec(&m, &t, 5u, rec, &reason);
  CHECK(rung == GB_SHOW_NONE, "dex 0: expected GB_SHOW_NONE, got %d", rung);
  CHECK(reason == GB12_ERR_SPECIES, "dex 0: expected GB12_ERR_SPECIES, got %d", reason);
  uint8_t zero[80]; memset(zero, 0, sizeof zero);
  CHECK(memcmp(rec, zero, 80) == 0, "dex 0: rec80 is not all-zero");
}

/* ---- 6. golden placeholder_pid, box=3 slot=7 (id_salt=67) ----------------------- */
/* Independent re-derivation of decision 3's documented sequence -- never a second call
 * into the module under test. seq = { box, slot, dex_lo, dex_hi, otid_lo, otid_hi,
 * level, gen }, FNV-1a-32, `h ? h : 1u`. */
static uint32_t golden_pid(uint8_t box, uint8_t slot, const Gb12Mon* m) {
  uint32_t h = 2166136261u;
  const uint8_t seq[8] = {
    box, slot,
    (uint8_t)m->species_dex, (uint8_t)(m->species_dex >> 8),
    (uint8_t)m->ot_id, (uint8_t)(m->ot_id >> 8),
    m->level, m->gen
  };
  for (int i = 0; i < 8; i++) { h ^= seq[i]; h *= 16777619u; }
  return h ? h : 1u;
}

static void t_golden_pid(void) {
  Gb12Mon m = base_mon();
  m.moves[0] = 0;                      /* force the PLACEHOLDER rung, which is what pays out a PID */
  uint32_t id_salt = 3u * 20u + 7u;    /* box=3, slot=7 */
  CHECK(id_salt == 67u, "golden pid: id_salt derivation itself is wrong (%u)", id_salt);
  uint32_t want = golden_pid(3, 7, &m);

  Gb12Target t = tgt3();
  uint8_t rec[80]; uint8_t reason = 0xFF;
  int rung = gb12_render_rec(&m, &t, id_salt, rec, &reason);
  CHECK(rung == GB_SHOW_PLACEHOLDER, "golden pid: expected GB_SHOW_PLACEHOLDER, got %d", rung);
  PkMon out; bool ok = pk_decode_mon(rec, false, &out);
  CHECK(ok, "golden pid: pk_decode_mon failed");
  CHECK(out.personality == want,
        "golden pid: placeholder PID %08x != hand-derived %08x", out.personality, want);

  /* box/slot round-trips out of id_salt exactly (decision 3's div/mod claim). */
  CHECK((uint8_t)(id_salt / 20u) == 3, "golden pid: id_salt/20 != box");
  CHECK((uint8_t)(id_salt % 20u) == 7, "golden pid: id_salt%%20 != slot");
}

int main(void) {
  t_full();
  t_egg();
  t_item();
  t_placeholder();
  t_none();
  t_golden_pid();
  printf("host_gb12render_test: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
