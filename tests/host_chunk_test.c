/* Host test for the pure-C multi-select chunk geometry (source/gen3_chunk.c).
 *   cc -std=c11 -I source tests/host_chunk_test.c source/gen3_chunk.c -o /tmp/hc && /tmp/hc
 * No .sav fixture needed — this is grid logic only.
 */
#include <stdio.h>
#include <string.h>
#include "gen3_chunk.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

/* occupancy from a 30-char map ('.'=empty, anything else=occupied), row-major 6x5 */
static void occ_from(const char* map, uint8_t occ[30]) {
  for (int i = 0; i < 30; i++) occ[i] = (map[i] && map[i] != '.') ? 1 : 0;
}

int main(void) {
  uint8_t occ[30], dest[30], tgt[30];
  Chunk c;

  /* ---- 1. full box selected: 30 occupied, footprint 5x6, n=30 ---- */
  occ_from("oooooo" "oooooo" "oooooo" "oooooo" "oooooo", occ);
  CHECK(chunk_build(&c, occ, 0, 29) == 30, "full box -> n=30");
  CHECK(c.h == 5 && c.w == 6, "full box footprint 5x6");
  /* drops into an empty box at (0,0), nowhere else (footprint fills the grid) */
  memset(dest, 0, sizeof dest);
  CHECK(chunk_can_drop(&c, 0, 0, dest, NULL, tgt), "full chunk fits empty box @0,0");
  CHECK(!chunk_can_drop(&c, 1, 0, dest, NULL, tgt), "full chunk cannot shift down");
  CHECK(tgt[0] == 0 && tgt[29] == 29, "full chunk targets identity");
  /* one occupied cell in an otherwise empty dest blocks the full-box drop */
  memset(dest, 0, sizeof dest); dest[15] = 1;
  CHECK(!chunk_can_drop(&c, 0, 0, dest, NULL, tgt), "occupied dest cell blocks full drop");

  /* ---- 2. sparse 3x2 rectangle: only 3 of its 6 cells occupied (+1 decoy outside) ---- */
  /* rectangle rows 1..3, cols 2..3 ; corners = slot(1,2)=8 and slot(3,3)=21.
   * The 'o' at (1,4) is a decoy OUTSIDE the rect and must be excluded. */
  occ_from("......" "..o.o." "...o.." "..o..." "......", occ);
  int n = chunk_build(&c, occ, 8, 21);
  CHECK(n == 3, "sparse rect -> 3 mons (only occupied inside the rect counted)");
  CHECK(c.h == 3 && c.w == 2, "sparse rect footprint 3x2");
  /* the occupied members inside rows1..3 cols2..3: (1,2),(2,3),(3,2) -> rel (0,0),(1,1),(2,0) */
  CHECK(c.rr[0] == 0 && c.cc[0] == 0 && c.src[0] == g3_slot(1, 2), "member0 rel/src");
  CHECK(c.rr[1] == 1 && c.cc[1] == 1 && c.src[1] == g3_slot(2, 3), "member1 rel/src");
  CHECK(c.rr[2] == 2 && c.cc[2] == 0 && c.src[2] == g3_slot(3, 2), "member2 rel/src");

  /* anchor clamp range for a 3x2 footprint */
  CHECK(chunk_anchor_rmax(&c) == 2 && chunk_anchor_cmax(&c) == 4, "3x2 anchor range");

  /* ---- 3. drop of the 3x2 chunk into a fresh empty box ---- */
  memset(dest, 0, sizeof dest);
  CHECK(chunk_can_drop(&c, 0, 0, dest, NULL, tgt), "3x2 fits empty @0,0");
  CHECK(tgt[0] == g3_slot(0, 0) && tgt[1] == g3_slot(1, 1) && tgt[2] == g3_slot(2, 0),
        "3x2 targets @0,0 preserve geometry");
  CHECK(chunk_can_drop(&c, 2, 4, dest, NULL, tgt), "3x2 fits bottom-right corner");
  CHECK(!chunk_can_drop(&c, 3, 4, dest, NULL, tgt), "3x2 off bottom edge rejected");
  CHECK(!chunk_can_drop(&c, 0, 5, dest, NULL, tgt), "3x2 off right edge rejected");

  /* a blocking mon exactly on a target cell (mon1 lands at (1,1) when anchored @0,0) */
  memset(dest, 0, sizeof dest); dest[g3_slot(1, 1)] = 1;
  CHECK(!chunk_can_drop(&c, 0, 0, dest, NULL, tgt), "occupied target cell blocks 3x2 drop");
  /* ...but an occupied cell that is NOT a target (footprint has a hole) does not block */
  memset(dest, 0, sizeof dest); dest[g3_slot(0, 1)] = 1;   /* the empty corner of the footprint */
  CHECK(chunk_can_drop(&c, 0, 0, dest, NULL, tgt), "occupied non-target cell ignored");

  /* ---- 4. same-box shift: source cells count as vacating ---- */
  /* a 1x3 row chunk at row0 cols0..2, shift right by 1 (overlaps its own sources) */
  occ_from("ooo..." "......" "......" "......" "......", occ);
  CHECK(chunk_build(&c, occ, 0, 2) == 3, "row chunk n=3");
  uint8_t vac[30]; memset(vac, 0, sizeof vac);
  for (int i = 0; i < c.n; i++) vac[c.src[i]] = 1;          /* its own sources will empty */
  memcpy(dest, occ, sizeof dest);                          /* dest == source box */
  CHECK(!chunk_can_drop(&c, 0, 1, dest, NULL, tgt), "shift-right blocked without vacating mask");
  CHECK(chunk_can_drop(&c, 0, 1, dest, vac, tgt), "shift-right allowed with vacating mask");
  CHECK(tgt[0] == g3_slot(0, 1) && tgt[2] == g3_slot(0, 3), "shifted targets");

  /* ---- 5. single-mon degenerate chunk (1x1) still works ---- */
  occ_from("o....." "......" "......" "......" "......", occ);
  CHECK(chunk_build(&c, occ, 0, 0) == 1 && c.h == 1 && c.w == 1, "1x1 single chunk");
  memset(dest, 0, sizeof dest);
  CHECK(chunk_can_drop(&c, 4, 5, dest, NULL, tgt) && tgt[0] == 29, "single fits far corner");

  /* ---- 6. empty rectangle -> n=0, never drops ---- */
  memset(occ, 0, sizeof occ);
  CHECK(chunk_build(&c, occ, 0, 29) == 0, "empty rect -> n=0");
  memset(dest, 0, sizeof dest);
  CHECK(!chunk_can_drop(&c, 0, 0, dest, NULL, tgt), "empty chunk never drops");

  printf("%s: host_chunk_test (%d failure%s)\n", fails ? "FAIL" : "OK", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
