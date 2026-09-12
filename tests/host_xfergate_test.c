/* Host test for source/xfer_gate.{c,h} -- BACKLOG #120 S2: the six gate predicates that
 * close every Gen-3 write path a Bank visit from a Game Boy session exposes.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_xfergate_test.c \
 *      source/xfer_gate.c -o /tmp/hxgt && /tmp/hxgt
 *
 * Covers: the full truth table (all 4 input combinations, by value) for each of the five
 * boolean-pair predicates, plus all 9 scope pairs for xg_drop_denied. */
#include <stdio.h>

#include "xfer_gate.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

#define BOXSCOPE_PC   0u
#define BOXSCOPE_BANK 1u
#define BOXSCOPE_GB   2u

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

static void test_xg_drop_denied(void) {
  uint8_t scopes[3] = { BOXSCOPE_PC, BOXSCOPE_BANK, BOXSCOPE_GB };
  for (int d = 0; d < 3; d++)
    for (int s = 0; s < 3; s++) {
      bool want = (scopes[d] == BOXSCOPE_GB) || (scopes[s] == BOXSCOPE_GB);
      CHECK(xg_drop_denied(scopes[d], scopes[s]) == want,
            "drop_denied: true iff either side is BOXSCOPE_GB");
    }
  printf("(F) xg_drop_denied: all 9 scope pairs\n");
}

int main(void) {
  test_xg_pc_live();
  test_xg_togame_row();
  test_xg_paste_row();
  test_xg_inject_refuse();
  test_xg_clear_carry_on_gb_exit();
  test_xg_drop_denied();

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
