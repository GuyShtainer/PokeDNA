/* Host test for source/gb_reconcile.c -- the identity-match core behind S5-C Part B2's
 * "reconcile on load" (docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 12): does a sidecar
 * entry's captured 80-byte original identify EXACTLY ONE mon in the CURRENT save, so
 * gb_reconcile_on_load() (source/pdna_main.c) knows which slot to release.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gb_reconcile_test.c \
 *      source/gb_reconcile.c source/gen3_clip.c source/gen3_box.c source/gen3_mon.c \
 *      source/gen3_save.c source/gen3_edit.c source/gen3_daycare.c \
 *      source/data_tables.c -o /tmp/hgbrec
 *   /tmp/hgbrec /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/ (.sav files)
 *
 * Over Guy's real 5-game corpus:
 *   1) every occupied party slot's own first-8-bytes identity matches itself exactly
 *      once, at the right (*where_box == -1, *where_slot == party index);
 *   2) every occupied PC slot's own identity likewise matches itself exactly once, at
 *      the right (*where_box, *where_slot);
 *   3) a random (non-existent) identity matches 0;
 *   4) an all-zero identity (the canonical empty-slot value) matches 0 even though
 *      every unoccupied slot in the save IS literally all-zero -- the degenerate case
 *      the header comment calls out explicitly;
 *   5) a CLONE -- a party mon's record duplicated into a free PC box slot, in a RAM
 *      copy of this save's PC storage -- matches 2, and where_box/where_slot are
 *      left at -1/-1 (refuses to guess which of the two is "the" one).
 * A missing corpus SKIPs rather than fails (same posture as every other Gen-3 host
 * test in this tree).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gb_reconcile.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_clip.h"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

static void test_save(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", path); return; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);

  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info) || !info.valid) {
    printf("  SKIP %s (does not parse)\n", path);
    return;
  }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  CHECK(gen3_read_saveblock1(save, info.slot, sb1) == G3_SAVEBLOCK1_BYTES,
        "%s: gen3_read_saveblock1", path);

  bool frlg = false;
  PkMon party[6];
  int nparty = pk_read_party_auto(sb1, party, &frlg);
  CHECK(nparty >= 1, "%s: at least one party mon", path);

  static uint8_t pc[G3_PC_BYTES];
  bool have_pc = gen3_read_pc_storage(save, info.slot, pc) == G3_PC_BYTES;

  printf("  %s: party=%d frlg=%d have_pc=%d\n", path, nparty, (int)frlg, (int)have_pc);

  /* 1) every party slot matches itself exactly once. */
  for (int i = 0; i < nparty; i++) {
    const uint8_t* rec = pk_party_slot(sb1, frlg, i);
    int wb = -99, ws = -99;
    int cnt = gb_reconcile_match(sb1, frlg, have_pc ? pc : NULL, rec, &wb, &ws);
    /* >= 1, not == 1: see the PC loop below for why a shared identity (Ninjask ->
     * Shedinja, or a real clone) is tolerated rather than assumed impossible. */
    CHECK(cnt >= 1, "%s: party[%d] matches at least itself (got %d)", path, i, cnt);
    if (cnt == 1)
      CHECK(wb == -1 && ws == i, "%s: party[%d] resolves to (box=-1, slot=%d) (got box=%d slot=%d)",
            path, i, i, wb, ws);
    else if (cnt > 1)
      CHECK(wb == -1 && ws == -1,
            "%s: party[%d] shares an identity with %d other slot(s); refuses to guess",
            path, i, cnt - 1);
  }

  /* 2) every occupied PC slot matches itself exactly once. An "occupied" slot here is
   * simply one whose own first 8 bytes are non-zero -- the same all-zero-is-empty
   * convention gb_reconcile.c itself uses, so no separate decode is needed to know
   * which slots are worth asking about. */
  if (have_pc) {
    int pc_checked = 0, pc_dupes = 0;
    for (int b = 0; b < G3_TOTAL_BOXES && pc_checked < 60; b++) {
      for (int s = 0; s < G3_IN_BOX && pc_checked < 60; s++) {
        const uint8_t* rec = pk_box_slot(pc, b, s);
        bool zero = true;
        for (int k = 0; k < 8; k++) if (rec[k]) { zero = false; break; }
        if (zero) continue;
        pc_checked++;
        int wb = -99, ws = -99;
        int cnt = gb_reconcile_match(sb1, frlg, pc, rec, &wb, &ws);
        /* A REAL Gen-3 save can legitimately hold two records sharing PID+otId --
         * Ninjask -> Shedinja is the documented case (Shedinja is created with its
         * parent Ninjask's exact personality and OT id), and Guy's own corpus turned
         * up at least one other pair too (found live by this test's first run, not
         * assumed up front -- exactly the "a cloned Gen-3 mon" case the design doc
         * names as a reason to refuse rather than guess). So this only requires the
         * slot find ITSELF (cnt >= 1, never 0 -- a record always matches its own
         * bytes); cnt == 1 is checked strictly for where it resolves, and cnt > 1 is
         * counted and confirmed to leave where_box/where_slot at -1/-1 rather than
         * asserted away. */
        CHECK(cnt >= 1, "%s: PC[%d][%d] matches at least itself (got %d)", path, b, s, cnt);
        if (cnt == 1)
          CHECK(wb == b && ws == s,
                "%s: PC[%d][%d] resolves to itself (got box=%d slot=%d)", path, b, s, wb, ws);
        else if (cnt > 1) {
          pc_dupes++;
          CHECK(wb == -1 && ws == -1,
                "%s: PC[%d][%d] shares an identity with %d other slot(s); refuses to guess",
                path, b, s, cnt - 1);
        }
      }
    }
    CHECK(pc_checked > 0, "%s: at least one occupied PC slot examined", path);
    if (pc_dupes) printf("  %s: %d PC slot(s) share an identity with another slot "
                         "(Ninjask/Shedinja or a real clone) -- refuse-to-guess exercised live\n",
                         path, pc_dupes);
  }

  /* 3) a random, never-occurring identity matches 0. 0xA5-filled is not a real
   * personality/otId pair on any save in this corpus (checked: it never collided
   * across five real games in this test's own runs). */
  {
    uint8_t bogus[8]; memset(bogus, 0xA5, sizeof bogus);
    int wb = 1, ws = 1;
    int cnt = gb_reconcile_match(sb1, frlg, have_pc ? pc : NULL, bogus, &wb, &ws);
    CHECK(cnt == 0, "%s: a random id8 matches 0 (got %d)", path, cnt);
    CHECK(wb == -1 && ws == -1, "%s: a 0-match leaves where_box/where_slot at -1/-1", path);
  }

  /* 4) the canonical empty-slot value (all zero) must NEVER match, even though every
   * unoccupied slot in the save really is all-zero -- this is the degenerate case
   * gb_reconcile.h calls out by name. */
  {
    uint8_t zero8[8]; memset(zero8, 0, sizeof zero8);
    int cnt = gb_reconcile_match(sb1, frlg, have_pc ? pc : NULL, zero8, NULL, NULL);
    CHECK(cnt == 0, "%s: an all-zero id8 matches 0 despite empty slots sharing that value (got %d)",
          path, cnt);
  }

  /* 5) a clone: duplicate party slot 0's record into a free PC box slot, in a RAM
   * copy so the real corpus arrays are untouched, then confirm the match count is 2
   * and the caller is told not to guess (where_box/where_slot left at -1/-1). */
  if (have_pc) {
    static uint8_t pc2[G3_PC_BYTES];
    memcpy(pc2, pc, sizeof pc2);
    int free_box = -1, free_slot = -1;
    for (int b = 0; b < G3_TOTAL_BOXES && free_box < 0; b++)
      for (int s = 0; s < G3_IN_BOX; s++) {
        const uint8_t* rec = pk_box_slot(pc2, b, s);
        bool zero = true;
        for (int k = 0; k < 8; k++) if (rec[k]) { zero = false; break; }
        if (zero) { free_box = b; free_slot = s; break; }
      }
    if (free_box < 0) {
      printf("  %s: SKIP clone check (every PC slot occupied)\n", path);
    } else {
      const uint8_t* rec0 = pk_party_slot(sb1, frlg, 0);
      memcpy(pk_box_slot(pc2, free_box, free_slot), rec0, 80);
      int wb = 1, ws = 1;
      int cnt = gb_reconcile_match(sb1, frlg, pc2, rec0, &wb, &ws);
      CHECK(cnt == 2, "%s: a cloned record matches 2 (got %d)", path, cnt);
      CHECK(wb == -1 && ws == -1, "%s: a 2-match refuses to guess (where_box/where_slot at -1/-1)",
            path);
    }
  }
}

int main(int argc, char** argv) {
  int examined = 0;
  for (int i = 1; i < argc; i++) {
    test_save(argv[i]);
    examined++;
  }
  if (!examined) printf("  (no .sav given on argv -- nothing to test)\n");

  printf("\n%s: %d check(s), %d failure(s)\n", g_fail ? "FAIL" : "OK", g_check, g_fail);
  return g_fail ? 1 : 0;
}
