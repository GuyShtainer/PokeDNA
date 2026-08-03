/* Host (PC) test for gen3_warp — the player teleport writer.
 *
 * This is the most save-destructive code in PokeDNA, so the test is about what it
 * must NOT do as much as what it does:
 *   1) it writes EXACTLY 8 bytes of SaveBlock1 (continueGameWarp at 0x0C) and one
 *      byte of SaveBlock2 — pos, location, lastHealLocation, escapeWarp,
 *      mapLayoutId, mapView and everything past 0x33 must be byte-identical after;
 *      (past 0x33 matters because FRLG keeps playerPartyCount at 0x34 where RSE
 *      keeps mapView — a stray write there destroys the party);
 *   2) warpId is 0xFF (WARP_ID_NONE = -1), not 0x7F;
 *   3) Ruby/Sapphire SET the whole specialSaveWarp byte to 1 (the game tests == 1),
 *      while Emerald/FRLG OR in bit 0 and must preserve POKECENTER_SAVEWARP etc.;
 *   4) an unknown (group,num) is refused — the game dereferences gMapGroups with
 *      zero bounds checking, so a bad pair is an unrecoverable boot hang;
 *   5) the undo snapshot restores the location block exactly.
 *
 * Build + run (from the repo root):
 *   cc -I source tests/host_warp_test.c source/gen3_warp.c -o /tmp/hw && /tmp/hw
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_warp.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

#define SB1_SIZE 15872
#define SB2_SIZE 3968

static uint8_t sb1[SB1_SIZE], sb2[SB2_SIZE], ref1[SB1_SIZE], ref2[SB2_SIZE];

/* fill both blocks with a recognisable pattern so ANY stray write shows up */
static void seed(void) {
  for (int i = 0; i < SB1_SIZE; i++) sb1[i] = (uint8_t)(i * 7 + 3);
  for (int i = 0; i < SB2_SIZE; i++) sb2[i] = (uint8_t)(i * 11 + 5);
  memcpy(ref1, sb1, SB1_SIZE);
  memcpy(ref2, sb2, SB2_SIZE);
}

/* every byte outside [lo,hi) must be untouched */
static int diff_outside(const uint8_t* a, const uint8_t* b, int n, int lo, int hi) {
  int d = 0;
  for (int i = 0; i < n; i++) if (i < lo || i >= hi) if (a[i] != b[i]) d++;
  return d;
}

int main(void) {
  /* ---- 1. the write touches exactly continueGameWarp + one SB2 byte ---- */
  {
    seed();
    G3Warp w = { 12, 3, 25, 40, false };
    CHECK(g3warp_apply(sb1, sb2, PK_EMERALD, &w), "apply succeeds");

    CHECK(sb1[0x0C] == 12, "mapGroup at 0x0C");
    CHECK(sb1[0x0D] == 3,  "mapNum at 0x0D");
    CHECK(sb1[0x0E] == 0xFF, "warpId is 0xFF (WARP_ID_NONE = -1), not 0x7F");
    CHECK(sb1[0x10] == 25 && sb1[0x11] == 0, "x written as s16 LE");
    CHECK(sb1[0x12] == 40 && sb1[0x13] == 0, "y written as s16 LE");

    CHECK(diff_outside(sb1, ref1, SB1_SIZE, 0x0C, 0x14) == 0,
          "SaveBlock1: nothing outside continueGameWarp changed");
    CHECK(diff_outside(sb2, ref2, SB2_SIZE, 0x09, 0x0A) == 0,
          "SaveBlock2: nothing outside specialSaveWarpFlags changed");

    /* the specific neighbours that would brick things */
    CHECK(memcmp(sb1 + 0x00, ref1 + 0x00, 4) == 0,  "pos untouched");
    CHECK(memcmp(sb1 + 0x04, ref1 + 0x04, 8) == 0,  "location untouched");
    CHECK(memcmp(sb1 + 0x1C, ref1 + 0x1C, 8) == 0,  "lastHealLocation untouched (white-out respawn)");
    CHECK(memcmp(sb1 + 0x24, ref1 + 0x24, 8) == 0,  "escapeWarp untouched");
    CHECK(memcmp(sb1 + 0x32, ref1 + 0x32, 2) == 0,  "mapLayoutId untouched");
    /* FRLG keeps playerPartyCount at 0x34 where RSE keeps mapView — a stray write
     * here would delete Pokemon on an FRLG save. */
    CHECK(memcmp(sb1 + 0x34, ref1 + 0x34, 512) == 0,
          "nothing past 0x33 touched (FRLG party lives at 0x34)");
  }

  /* ---- 2. centre-of-map sentinel ---- */
  {
    seed();
    G3Warp w = { 0, 9, 999, 999, true };        /* x/y ignored when centre is set */
    CHECK(g3warp_apply(sb1, sb2, PK_EMERALD, &w), "centre apply succeeds");
    CHECK(sb1[0x10] == 0xFF && sb1[0x11] == 0xFF, "centre writes x = -1");
    CHECK(sb1[0x12] == 0xFF && sb1[0x13] == 0xFF, "centre writes y = -1");
    G3Warp got;
    CHECK(g3warp_get_pending(sb1, sb2, PK_EMERALD, &got), "pending warp is armed");
    CHECK(got.centre, "read back as centre");
    CHECK(got.group == 0 && got.num == 9, "destination reads back");
  }

  /* ---- 3. the per-game specialSaveWarp semantics ---- */
  {
    /* Emerald: OR, preserving other bits (POKECENTER_SAVEWARP = 1<<1 etc.) */
    seed();
    sb2[0x09] = 0x82;                            /* CHAMPION | POKECENTER */
    G3Warp w = { 1, 1, 5, 5, false };
    g3warp_apply(sb1, sb2, PK_EMERALD, &w);
    CHECK(sb2[0x09] == 0x83, "Emerald ORs bit 0 and preserves the other flags");

    /* FRLG: same bitfield treatment */
    seed(); sb2[0x09] = 0x02;
    g3warp_apply(sb1, sb2, PK_FRLG, &w);
    CHECK(sb2[0x09] == 0x03, "FRLG ORs bit 0");

    /* Ruby/Sapphire: the game tests `== 1`, so anything else must be replaced */
    seed(); sb2[0x09] = 0x82;
    g3warp_apply(sb1, sb2, PK_RS, &w);
    CHECK(sb2[0x09] == 0x01, "R/S SETS the byte to exactly 1 (tested with ==, not a bit test)");

    /* and the armed-check mirrors that asymmetry */
    seed(); sb2[0x09] = 0x03;
    G3Warp got;
    CHECK(g3warp_get_pending(sb1, sb2, PK_EMERALD, &got), "Emerald: bit set => armed");
    CHECK(!g3warp_get_pending(sb1, sb2, PK_RS, &got), "R/S: 0x03 is NOT the armed value");
    sb2[0x09] = 0x01;
    CHECK(g3warp_get_pending(sb1, sb2, PK_RS, &got), "R/S: 0x01 => armed");
    sb2[0x09] = 0x00;
    CHECK(!g3warp_get_pending(sb1, sb2, PK_EMERALD, &got), "Emerald: bit clear => not armed");
  }

  /* ---- 4. destination validation ---- */
  {
    G3MapBounds b = { 12, 3, 40, 30, true };
    G3Warp ok      = { 12, 3, 39, 29, false };
    G3Warp edge0   = { 12, 3, 0, 0, false };
    G3Warp oobx    = { 12, 3, 40, 10, false };
    G3Warp ooby    = { 12, 3, 10, 30, false };
    G3Warp neg     = { 12, 3, -1, 10, false };
    G3Warp wrongm  = { 12, 4, 10, 10, false };
    G3Warp centre  = { 12, 3, 9999, 9999, true };
    CHECK(g3warp_check(&b, &ok)     == G3W_OK, "in-bounds accepted");
    CHECK(g3warp_check(&b, &edge0)  == G3W_OK, "origin accepted");
    CHECK(g3warp_check(&b, &oobx)   == G3W_OUT_OF_BOUNDS, "x == width rejected");
    CHECK(g3warp_check(&b, &ooby)   == G3W_OUT_OF_BOUNDS, "y == height rejected");
    CHECK(g3warp_check(&b, &neg)    == G3W_OUT_OF_BOUNDS, "negative x rejected");
    CHECK(g3warp_check(&b, &wrongm) == G3W_BAD_MAP, "bounds for a different map rejected");
    CHECK(g3warp_check(&b, &centre) == G3W_OK, "centre skips the range check (always in bounds)");
    b.known = false;
    CHECK(g3warp_check(&b, &ok) == G3W_BAD_MAP, "unknown map ALWAYS rejected");
    b.known = true; b.width = 0;
    CHECK(g3warp_check(&b, &ok) == G3W_BAD_MAP, "zero-size map rejected");
    CHECK(g3warp_check(0, &ok) == G3W_BAD_MAP, "NULL bounds rejected");
    CHECK(g3warp_check(&b, 0) == G3W_BAD_MAP, "NULL warp rejected");
  }

  /* ---- 5. risky-map advisory ---- */
  {
    CHECK(g3warp_risky_map(PK_EMERALD, 25, 0), "Emerald group 25 (secret bases/link) is risky");
    CHECK(g3warp_risky_map(PK_EMERALD, 26, 0), "Emerald group 26 (Battle Frontier) is risky");
    CHECK(g3warp_risky_map(PK_EMERALD, 27, 5), "Emerald group 27 (Battle Pyramid) is risky");
    CHECK(!g3warp_risky_map(PK_EMERALD, 0, 9), "Littleroot is not risky");
    CHECK(!g3warp_risky_map(PK_RS, 25, 0), "advisory is Emerald-only (group numbers differ)");
  }

  /* ---- 6. bad input writes nothing ---- */
  {
    seed();
    G3Warp w = { 1, 1, 1, 1, false };
    CHECK(!g3warp_apply(0, sb2, PK_EMERALD, &w), "NULL sb1 refused");
    CHECK(!g3warp_apply(sb1, 0, PK_EMERALD, &w), "NULL sb2 refused");
    CHECK(!g3warp_apply(sb1, sb2, PK_EMERALD, 0), "NULL warp refused");
    CHECK(memcmp(sb1, ref1, SB1_SIZE) == 0 && memcmp(sb2, ref2, SB2_SIZE) == 0,
          "a refused apply wrote nothing at all");
  }

  /* ---- 7. undo snapshot round-trips ---- */
  {
    seed();
    uint8_t snap[G3W_SNAPSHOT_SIZE];
    g3warp_snapshot(sb1, sb2, snap);
    G3Warp w = { 7, 2, 11, 12, false };
    g3warp_apply(sb1, sb2, PK_EMERALD, &w);
    CHECK(memcmp(sb1, ref1, 0x34) != 0, "the edit did change the location block");
    CHECK(g3warp_restore(sb1, sb2, snap), "restore succeeds");
    CHECK(memcmp(sb1, ref1, SB1_SIZE) == 0, "SaveBlock1 fully restored");
    CHECK(memcmp(sb2, ref2, SB2_SIZE) == 0, "SaveBlock2 fully restored");
    CHECK(G3W_SNAPSHOT_SIZE == 0x35, "snapshot is 53 bytes (0x34 + the warp flag byte)");
  }

  /* ---- 8. current position reads from `location` + `pos`, not the pending warp -- */
  {
    seed();
    sb1[0x00] = 20; sb1[0x01] = 0;      /* pos.x = 20 */
    sb1[0x02] = 31; sb1[0x03] = 0;      /* pos.y = 31 */
    sb1[0x04] = 3;  sb1[0x05] = 6;      /* location group 3, map 6 */
    G3Warp cur;
    CHECK(g3warp_get_current(sb1, &cur), "current position reads");
    CHECK(cur.group == 3 && cur.num == 6, "current map from `location`");
    CHECK(cur.x == 20 && cur.y == 31, "current coords from `pos` (no MAP_OFFSET applied)");
    /* and it must not be confused by a pending warp elsewhere */
    G3Warp w = { 9, 9, 1, 1, false };
    g3warp_apply(sb1, sb2, PK_EMERALD, &w);
    CHECK(g3warp_get_current(sb1, &cur) && cur.group == 3 && cur.num == 6,
          "a pending warp does not change where the player currently IS");
  }

  if (g_fail == 0) printf("OK: host_warp_test (0 failures)\n");
  else             printf("host_warp_test: %d FAILURE(S)\n", g_fail);
  return g_fail ? 1 : 0;
}
