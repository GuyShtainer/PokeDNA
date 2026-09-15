/* Host test for source/xfer_gate.{c,h} -- BACKLOG #120 S2: the six gate predicates that
 * close every Gen-3 write path a Bank visit from a Game Boy session exposes, plus
 * BACKLOG #150 S150-3's two escape-route predicates.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_xfergate_test.c \
 *      source/xfer_gate.c source/bank_cell.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      -o /tmp/hxgt && /tmp/hxgt
 *
 * Covers: the full truth table (all 4 input combinations, by value) for each of the five
 * boolean-pair predicates, plus all 9 scope pairs for xg_drop_denied, plus (H)/(I) below. */
#include <stdio.h>
#include <string.h>

#include "xfer_gate.h"
#include "bank_cell.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

#define BOXSCOPE_PC   0u
#define BOXSCOPE_BANK 1u
/* BOXSCOPE_GB's value (2) is NOT redefined here -- the single source of truth is
 * source/pdna_box.c's own _Static_assert(BOXSCOPE_GB == 2, ...) right after its
 * "#include "xfer_gate.h"", which fails the build if source/pdna_box.h's
 * BOXSCOPE_GB and source/xfer_gate.c's hard-coded XG_SCOPE_GB ever drift apart.
 * This test still needs the literal value (xfer_gate.h deliberately does not
 * include pdna_box.h), so it is spelled out at each use below instead of a second
 * local #define that could itself go stale unnoticed (BACKLOG #120 S2 F2). */

static void test_xg_pc_live(void) {
  CHECK(xg_pc_live(false, false) == false, "pc_live: no save, arena free -> false");
  CHECK(xg_pc_live(false, true)  == false, "pc_live: no save, arena held -> false");
  CHECK(xg_pc_live(true,  false) == true,  "pc_live: save + arena free -> true");
  CHECK(xg_pc_live(true,  true)  == false, "pc_live: save but arena held -> false");
  printf("(A) xg_pc_live: full 2x2 truth table\n");
}

static void test_xg_togame_row(void) {
  for (int is_bank = 0; is_bank <= 1; is_bank++)
    for (int pc_live = 0; pc_live <= 1; pc_live++)
      for (int have_pc = 0; have_pc <= 1; have_pc++) {
        bool want = is_bank && pc_live && have_pc;
        CHECK(xg_togame_row(is_bank, pc_live, have_pc) == want,
              "togame_row: true iff Bank cell AND live Gen-3 PC AND something in hand");
      }
  printf("(B) xg_togame_row: full 2x2x2 truth table\n");
}

static void test_xg_paste_row(void) {
  CHECK(xg_paste_row(false, false) == false, "paste_row: nothing clipped, no PC -> false");
  CHECK(xg_paste_row(false, true)  == false, "paste_row: nothing clipped, live PC -> false");
  CHECK(xg_paste_row(true,  false) == false, "paste_row: clipped but no live PC -> false");
  CHECK(xg_paste_row(true,  true)  == true,  "paste_row: clipped AND live PC -> true");
  printf("(C) xg_paste_row: full 2x2 truth table\n");
}

static void test_xg_create_row(void) {
  CHECK(xg_create_row(false, false) == true,  "create_row: PC/party cell, no live PC -> still true (unaffected)");
  CHECK(xg_create_row(false, true)  == true,  "create_row: PC/party cell, live PC -> true");
  CHECK(xg_create_row(true,  false) == false, "create_row: Bank cell, no live Gen-3 PC -> false");
  CHECK(xg_create_row(true,  true)  == true,  "create_row: Bank cell, live Gen-3 PC -> true");
  printf("(G) xg_create_row: full 2x2 truth table\n");
}

static void test_xg_inject_refuse(void) {
  CHECK(xg_inject_refuse(false, false) == true,  "inject_refuse: arena free, no save -> refuse");
  CHECK(xg_inject_refuse(false, true)  == false, "inject_refuse: arena free, live save -> allow");
  CHECK(xg_inject_refuse(true,  false) == true,  "inject_refuse: arena held, no save -> refuse");
  CHECK(xg_inject_refuse(true,  true)  == true,  "inject_refuse: arena held even with a save -> refuse");
  printf("(D) xg_inject_refuse: full 2x2 truth table\n");
}

static void test_xg_clear_carry_on_gb_exit(void) {
  CHECK(xg_clear_carry_on_gb_exit(false) == false, "clear_carry: PC/Bank-scope carry survives exit");
  CHECK(xg_clear_carry_on_gb_exit(true)  == true,  "clear_carry: GB-scope carry is dropped on exit");
  printf("(E) xg_clear_carry_on_gb_exit: both inputs\n");
}

/* BACKLOG #150 S150-4 decision 9/step 6(b): the full 3x3x2 = 18-combination truth
 * table -- false for EXACTLY ONE (dst=BANK, src=GB, have_xfer=true), the UP edge this
 * lane adds. Every other combination stays denied, including have_xfer=true on any
 * OTHER scope pair (a vtable existing says nothing about whether GB is even
 * involved) and (dst=BANK, src=GB, have_xfer=false) (S2's "no lift path yet" case,
 * still denied when the real vtable isn't installed). */
static void test_xg_drop_denied(void) {
  uint8_t scopes[3] = { BOXSCOPE_PC, BOXSCOPE_BANK, 2u /* BOXSCOPE_GB, see the comment above */ };
  int allowed = 0;
  for (int d = 0; d < 3; d++)
    for (int s = 0; s < 3; s++)
      for (int hx = 0; hx <= 1; hx++) {
        bool have_xfer = (bool)hx;
        bool allow = (scopes[d] == BOXSCOPE_BANK) && (scopes[s] == 2u) && have_xfer;
        bool want = !allow && ((scopes[d] == 2u) || (scopes[s] == 2u));
        if (allow) allowed++;
        CHECK(xg_drop_denied(scopes[d], scopes[s], have_xfer) == want,
              "drop_denied: denied unless dst=BANK, src=GB, have_xfer -- the one S150-4 allow-rule");
      }
  CHECK(allowed == 1, "drop_denied: exactly ONE of the 18 combinations is allowed");
  printf("(F) xg_drop_denied: all 3x3x2 = 18 scope/have_xfer combinations, exactly one allowed\n");
}

/* BACKLOG #150 S150-3 step 1: build a native fixture WITHOUT a corpus -- fill 80 bytes
 * with a recognisable pattern, stamp the magic + the old-build byte-19 guard + a
 * plausible gen/rec_len, then let bc_ident32() (works on any 80-byte buffer, bank_cell.h
 * :143-144) fill in its own identity hash at bytes 4..7 so bc_is_native() reads it back
 * as true. The all-zero cell (memset(0)) must read false (no magic). */
static void make_native_fixture(uint8_t out80[80]) {
  for (int i = 0; i < 80; i++) out80[i] = (uint8_t)(0x40 + i);   /* recognisable, non-zero pattern */
  out80[0] = 'G'; out80[1] = 'B'; out80[2] = 'C'; out80[3] = '1';
  out80[8] = 1;    /* gen = GB_GEN1 */
  out80[9] = 33;   /* rec_len = Gen-1 box record (GEN1_BOX_REC_BYTES) */
  out80[19] = 0x01;   /* the old-build guard byte, BC_OLDBUILD_BYTE */
  uint32_t id = bc_ident32(out80);
  out80[4] = (uint8_t)(id & 0xFF);
  out80[5] = (uint8_t)((id >> 8) & 0xFF);
  out80[6] = (uint8_t)((id >> 16) & 0xFF);
  out80[7] = (uint8_t)((id >> 24) & 0xFF);
}

/* A real Gen-3 corpus record has none of these bytes right, so it is enough to build
 * one with a plausible-but-wrong shape (no "GBC1" magic) rather than pull in a whole
 * .sav -- bc_is_native()'s FIRST check is the magic (bank_cell.h:130-132), so any
 * buffer that fails it is treated identically regardless of what follows. */
static void make_gen3_like_fixture(uint8_t out80[80]) {
  memset(out80, 0, 80);
  out80[0] = 0x34; out80[1] = 0x12; out80[2] = 0xAB; out80[3] = 0xCD;   /* personality, not "GBC1" */
  out80[19] = 0x00;                                                     /* no old-build guard bit */
}

#define XG_SCOPE_GB 2u

static void test_xg_native_escape_denied(void) {
  uint8_t native[80]; make_native_fixture(native);
  uint8_t allzero[80]; memset(allzero, 0, 80);
  uint8_t gen3like[80]; make_gen3_like_fixture(gen3like);
  if (!(bc_is_native(native) && !bc_is_native(allzero) && !bc_is_native(gen3like)))
    { printf("  !! FAIL: fixture setup: native/all-zero/gen3-like bc_is_native() disagree with the plan\n"); fails++; }
  checks++;

  const uint8_t* cells[3] = { native, allzero, gen3like };
  const char* names[3] = { "native", "all-zero", "gen3-like" };
  const uint8_t scopes[3] = { BOXSCOPE_PC, BOXSCOPE_BANK, XG_SCOPE_GB };
  const char* snames[3] = { "PC", "BANK", "GB" };
  for (int c = 0; c < 3; c++)
    for (int s = 0; s < 3; s++) {
      bool want = (c == 0) && (scopes[s] != BOXSCOPE_BANK);   /* denied only for native x {PC, GB} */
      char msg[96];
      snprintf(msg, sizeof msg, "native_escape_denied: %s cell -> %s scope", names[c], snames[s]);
      CHECK(xg_native_escape_denied(cells[c], scopes[s]) == want, msg);
    }
  printf("(H) xg_native_escape_denied: 3 cells x 3 dst scopes = 9 assertions, true only native x {PC, GB}\n");
}

static void test_xg_chunk_crossgen_denied(void) {
  /* Matches pdna_box.c's own `if (src->scope == BOXSCOPE_GB || s_ch_scope == BOXSCOPE_GB)`
   * (drop_chunk's cross-generation refusal) exactly -- all 9 scope pairs. */
  uint8_t scopes[3] = { BOXSCOPE_PC, BOXSCOPE_BANK, XG_SCOPE_GB };
  for (int d = 0; d < 3; d++)
    for (int s = 0; s < 3; s++) {
      bool want = (scopes[d] == XG_SCOPE_GB) || (scopes[s] == XG_SCOPE_GB);
      CHECK(xg_chunk_crossgen_denied(scopes[d], scopes[s]) == want,
            "chunk_crossgen_denied: true iff either scope is BOXSCOPE_GB (== 2)");
    }
  printf("(I) xg_chunk_crossgen_denied: all 9 scope pairs\n");
}

int main(void) {
  test_xg_pc_live();
  test_xg_togame_row();
  test_xg_paste_row();
  test_xg_create_row();
  test_xg_inject_refuse();
  test_xg_clear_carry_on_gb_exit();
  test_xg_drop_denied();
  test_xg_native_escape_denied();
  test_xg_chunk_crossgen_denied();

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
