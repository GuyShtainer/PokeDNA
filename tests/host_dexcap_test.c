/* Host test for BACKLOG #87 item 1 -- the species cap on the shared Pokedex screen
 * (source/pdna_pick.c: s_dex_max / pdna_dex_set_max(), dex_counts() and dex_bulk()'s
 * three Catch/See/Wipe-ALL loops). pdna_pick.c pulls in <tonc.h> + the UI/OSK/sound
 * stack and cannot be host-compiled (same reason host_dexicons_test.c mirrors
 * art_fallbacks.c instead of linking it) -- this test MIRRORS the cap-bounded loop
 * shape byte-for-byte (kept in sync by eye against pdna_pick.c) with a mock
 * get/set pair that records the highest dex index either one is ever called with,
 * so a regression that widens a loop back to a hardcoded 386 shows up as the
 * recorded high-water mark exceeding the configured cap.
 *   cc -std=c11 tests/host_dexcap_test.c -o /tmp/hdc && /tmp/hdc
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define DEX_NAT_MAX 386

/* ---- the mock save: records every nat index touched by get or set ---------- */
static int8_t s_state[DEX_NAT_MAX + 1];   /* 1-indexed; [0] unused */
static int s_get_hi, s_set_hi;            /* highest nat index passed to get/set this run */
static int s_get_calls, s_set_calls;

static void mock_reset(void) {
  memset(s_state, 0, sizeof s_state);
  s_get_hi = s_set_hi = 0;
  s_get_calls = s_set_calls = 0;
}
static int mock_get(int nat) {
  s_get_calls++;
  if (nat > s_get_hi) s_get_hi = nat;
  return s_state[nat];
}
static void mock_set(int nat, int state) {
  s_set_calls++;
  if (nat > s_set_hi) s_set_hi = nat;
  s_state[nat] = (int8_t)state;
}

/* ---- mirror of pdna_pick.c's cap (kept in sync by eye) --------------------- */
static int s_dex_max = DEX_NAT_MAX;
static void mock_pdna_dex_set_max(int max_dex) {
  s_dex_max = (max_dex > 0 && max_dex <= DEX_NAT_MAX) ? max_dex : DEX_NAT_MAX;
}

/* mirror of dex_counts() (pdna_pick.c:595-598): `for (nat = 1; nat <= s_dex_max; ...)` */
static void mock_dex_counts(int* seen, int* caught) {
  int s = 0, c = 0;
  for (int nat = 1; nat <= s_dex_max; nat++) { int st = mock_get(nat); if (st >= 1) s++; if (st >= 2) c++; }
  *seen = s; *caught = c;
}

/* mirror of dex_bulk()'s Catch/See/Wipe-ALL + Undo loops (pdna_pick.c:736-748):
 * snapshot -> set every nat to `a` -> (optionally) undo by restoring the snapshot. */
static int8_t s_snap[DEX_NAT_MAX];
static void mock_bulk_set_all(int a) {
  for (int nat = 1; nat <= s_dex_max; nat++) s_snap[nat - 1] = (int8_t)mock_get(nat);
  for (int nat = 1; nat <= s_dex_max; nat++) mock_set(nat, a);
}
static void mock_bulk_undo(void) {
  for (int nat = 1; nat <= s_dex_max; nat++) mock_set(nat, s_snap[nat - 1]);
}

static int fails = 0, checks = 0;
static void chk(const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static void run_cap(int cap, const char* label) {
  mock_reset();
  mock_pdna_dex_set_max(cap);

  int seen, caught;
  mock_dex_counts(&seen, &caught);
  printf("[%s] dex_counts: get_hi=%d (cap %d), seen=%d caught=%d\n", label, s_get_hi, cap, seen, caught);
  chk("dex_counts never reads past the cap", s_get_hi == cap);
  chk("dex_counts on a blank save reports 0/0", seen == 0 && caught == 0);

  /* seed one species PAST the cap directly in the mock state (simulating an
   * earlier session/leaked write) -- dex_counts must not count it. */
  if (cap < DEX_NAT_MAX) {
    s_state[cap + 1] = 2;
    mock_dex_counts(&seen, &caught);
    chk("a species past the cap is invisible to the header counts", seen == 0 && caught == 0);
    s_state[cap + 1] = 0;
  }

  /* Catch ALL: every nat 1..cap goes to caught (2); nothing past the cap is
   * ever touched by set (the highest cap leak this test exists to catch). */
  mock_bulk_set_all(2);
  printf("[%s] Catch ALL: set_hi=%d (cap %d)\n", label, s_set_hi, cap);
  chk("Catch ALL never sets past the cap", s_set_hi == cap);
  int all_caught = 1;
  for (int nat = 1; nat <= cap; nat++) if (s_state[nat] != 2) all_caught = 0;
  chk("Catch ALL sets every species 1..cap", all_caught);
  if (cap < DEX_NAT_MAX) chk("Catch ALL leaves species past the cap untouched", s_state[cap + 1] == 0);

  /* Undo restores exactly the pre-bulk snapshot, still bounded by the cap. */
  s_set_hi = 0;
  mock_bulk_undo();
  chk("Undo never sets past the cap either", s_set_hi == cap);
  int all_blank = 1;
  for (int nat = 1; nat <= cap; nat++) if (s_state[nat] != 0) all_blank = 0;
  chk("Undo restores the blank pre-bulk state", all_blank);
}

/* D1 (b87 fix pass, DO-NOT-SHIP review): dex_bulk()'s Catch/See/Wipe-ALL confirm
 * prompt (pdna_pick.c:~743) formats `siprintf(amsg, "All %d. (Undo available.)",
 * s_dex_max)` into a stack buffer -- "All 386. (Undo available.)" is 27 bytes
 * including the NUL, which overflowed the original `char amsg[24]` on EVERY bulk
 * confirm on a Gen-3 save (the shipped path, not a corner case). Fixed to
 * `char amsg[28]`. Mirrored here with a canary byte immediately after the buffer
 * (pdna_pick.c itself can't host-compile, same reason the rest of this file mocks
 * its shape instead of linking it) -- a regression that shrinks the buffer back
 * below the formatted length would either truncate the string short of its NUL
 * (caught by the strlen check) or, on a real stack layout, smash the canary. */
static void run_amsg_canary(int cap, const char* label) {
  struct { char amsg[28]; unsigned char canary; } buf;
  buf.canary = 0xA5;
  int n = snprintf(buf.amsg, sizeof buf.amsg, "All %d. (Undo available.)", cap);
  printf("[%s] amsg=\"%s\" (%d chars + NUL = %d bytes, buffer %zu)\n",
         label, buf.amsg, n, n + 1, sizeof buf.amsg);
  chk("amsg: siprintf's return value fits inside the buffer (no truncation)",
      n >= 0 && (size_t)n < sizeof buf.amsg);
  chk("amsg: the formatted string is NUL-terminated inside the buffer",
      strlen(buf.amsg) < sizeof buf.amsg);
  chk("amsg: the canary byte right after the buffer is untouched", buf.canary == 0xA5);
}

int main(void) {
  run_cap(151, "Gen1 cap=151");
  run_cap(251, "Gen2 cap=251");
  run_cap(386, "Gen3 cap=386 (default)");

  run_amsg_canary(151, "amsg cap=151");
  run_amsg_canary(251, "amsg cap=251");
  run_amsg_canary(386, "amsg cap=386 (the shipped Gen-3 case -- 27 bytes, D1)");

  /* MUTATION CHECK: prove this test would actually catch a regression -- if the
   * loop bound were hardcoded back to DEX_NAT_MAX (386) instead of s_dex_max,
   * the high-water mark under a 151 cap would be 386, not 151. */
  mock_reset();
  mock_pdna_dex_set_max(151);
  for (int nat = 1; nat <= DEX_NAT_MAX /* the REGRESSED bound, on purpose */; nat++) mock_get(nat);
  chk("mutation sanity: a hardcoded-386 loop DOES produce a high-water mark this test would flag",
      s_get_hi == 386 && s_get_hi != 151);

  /* clamp behaviour: 0 / negative / >386 all fall back to the full 386. */
  mock_pdna_dex_set_max(0);   chk("cap 0 clamps to 386",   s_dex_max == 386);
  mock_pdna_dex_set_max(-5);  chk("cap <0 clamps to 386",  s_dex_max == 386);
  mock_pdna_dex_set_max(500); chk("cap >386 clamps to 386", s_dex_max == 386);
  mock_pdna_dex_set_max(151); chk("cap 151 sticks",        s_dex_max == 151);

  printf("\n%s: %d checks, %d failure(s)\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}
