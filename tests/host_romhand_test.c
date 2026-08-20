/* Host (PC) test for rom_hand — the Gen-3 PC-storage POINTER GLOVE read from REAL
 * retail ROMs. rom_hand does all its I/O through the RomCtx callback, so the code
 * under test is byte-for-byte the code that runs on the GBA (fused today; a
 * registered SD file later).
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 * -DPDNA_HAND_ART_COMPILED=0 forces rom_hand.c to compile regardless of whether
 * this machine's source/ tree happens to have the generated (git-ignored)
 * hand_oam.c staged right now — see hand_gate.h.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -O2 -I source -DPDNA_HAND_ART_COMPILED=0 \
 *      tests/host_romhand_test.c source/rom_hand.c source/rom_map.c -o /tmp/hrhd && /tmp/hrhd
 *
 * What it proves:
 *   1) rom_hand_open() identifies Emerald/FireRed/LeafGreen (all pinned) and FAILS
 *      CLOSED on Ruby/Sapphire (a different, unpinned glove — DESIGN.md's honest gap);
 *   2) all four poses read, are non-blank, and are pairwise distinct (four real
 *      poses, not four copies of one frame);
 *   3) the sheet is byte-for-byte IDENTICAL across Emerald/FireRed/LeafGreen — the
 *      claim rom_hand.h's header comment makes about the pin table's provenance;
 *   4) the palette reads, is non-degenerate, and slot 0 is the transparent GBA
 *      convention (bit 15 clear, like every other entry — nothing special-cased);
 *   5) out-of-range frames are rejected;
 *   6) the verified read machinery heals a one-shot cart-bus-style garble and
 *      fails closed on a persistent one, the same exposure/cover pair
 *      host_rommon_test.c already proves for rom_mon's verified locate.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_map.h"
#include "rom_hand.h"

static int fails = 0, checks = 0;
static void chk(const char* rom, const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", rom, what); fails++; }
}

/* Same garble injector as host_rommon_test.c: reads at `g_off` come back with byte 0
 * flipped by +0x10 -- still perfectly in-bounds, exactly the failure ptr_ok cannot
 * see. g_every2 keeps hitting only the first read of each back-to-back pair (a
 * glitch that never lets the verify agree); without it the garble is spent after
 * `g_left` reads and a retry heals. */
typedef struct {
  FILE* f; int reads;
  uint32_t g_off; int g_left; int g_every2; int g_hits;
} FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->reads++;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  if (fc->g_left > 0 && off == fc->g_off) {
    int hit = fc->g_hits++;
    if (!fc->g_every2 || (hit % 2) == 0) { fc->g_left--; ((uint8_t*)dst)[0] ^= 0x10; }
  }
  return true;
}
static void garble(FileCtx* fc, uint32_t off, int n, int every2) {
  fc->g_off = off; fc->g_left = n; fc->g_every2 = every2; fc->g_hits = 0;
}
static void garble_off(FileCtx* fc) { fc->g_left = 0; fc->g_every2 = 0; fc->g_hits = 0; }

static int blank(const uint8_t* p, int n) {
  for (int i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

/* Emerald's four poses, captured once and compared against every other pinned
 * game's -- the cross-game half of check 3 ("byte-for-byte IDENTICAL across
 * Emerald/FireRed/LeafGreen", rom_hand.h's own provenance claim, not merely
 * self-consistent with a raw re-read of the same file). */
static uint8_t g_emerald_frames[ROM_HAND_FRAMES][ROM_HAND_FRAME_BYTES];
static int g_have_emerald = 0;

static void run_rom(const char* path, const char* name, int expect_pinned) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0, 0, 0, 0, 0 };
  RomCtx rc;
  if (!rom_open(&rc, file_read, &fc, (uint32_t)sz)) {
    chk(name, "rom_open accepts the retail dump", 0);
    fclose(f); return;
  }

  RomHand rh;
  int ok = rom_hand_open(&rh, &rc);
  chk(name, expect_pinned ? "pinned revision opens" : "fails CLOSED (unpinned/different glove)",
      ok == expect_pinned);
  if (!ok) { fclose(f); return; }

  /* 2) all four poses read, are non-blank, and pairwise distinct */
  uint8_t frames[ROM_HAND_FRAMES][ROM_HAND_FRAME_BYTES];
  for (uint8_t p = 0; p < ROM_HAND_FRAMES; p++) {
    char what[40]; sprintf(what, "pose %u reads", p);
    chk(name, what, rom_hand_frame(&rh, p, frames[p]));
    sprintf(what, "pose %u is non-blank", p);
    chk(name, what, !blank(frames[p], ROM_HAND_FRAME_BYTES));
  }
  int distinct = 1;
  for (int a = 0; a < ROM_HAND_FRAMES && distinct; a++)
    for (int b = a + 1; b < ROM_HAND_FRAMES; b++)
      if (memcmp(frames[a], frames[b], ROM_HAND_FRAME_BYTES) == 0) { distinct = 0; break; }
  chk(name, "the four poses are pairwise distinct", distinct);

  /* 3) the sheet is byte-identical across games (rom_hand.h's provenance claim) */
  if (!g_have_emerald) {
    for (int p = 0; p < ROM_HAND_FRAMES; p++)
      memcpy(g_emerald_frames[p], frames[p], ROM_HAND_FRAME_BYTES);
    g_have_emerald = 1;
  } else {
    int same = 1;
    for (int p = 0; p < ROM_HAND_FRAMES; p++)
      if (memcmp(g_emerald_frames[p], frames[p], ROM_HAND_FRAME_BYTES) != 0) same = 0;
    chk(name, "sheet is byte-identical to Emerald's (one PNG, three carts)", same);
  }

  /* 4) the palette */
  uint16_t pal[16];
  chk(name, "palette reads", rom_hand_pal(&rh, pal));
  int pal_distinct = 0;
  for (int i = 1; i < 16; i++) if (pal[i] != pal[0]) { pal_distinct = 1; break; }
  chk(name, "palette is non-degenerate", pal_distinct);
  int bit15_clear = 1;
  for (int i = 0; i < 16; i++) if (pal[i] & 0x8000) bit15_clear = 0;
  chk(name, "every palette entry has bit 15 clear", bit15_clear);

  /* 4b) MUST-FIX 1 (2026-08-20 review): the check above never discriminates a wrong
   * palette from a right one -- "non-degenerate" and "bit 15 clear" both pass on the
   * WRONG pinned palette (0x085723DC/0x08E9C3F8/0x08E9C478), which decodes this exact
   * sheet's glove BODY as 0x2D4A dark grey, not the retail white. This is the
   * assertion that actually would have caught it: frame 0 pixel (20,6) is raw 4bpp
   * index 3 on all three pinned games (the sheet is byte-identical, check 3 above --
   * confirmed against Guy's own dumps), and index 3 is the 72-pixel glove BODY. It
   * must decode to WHITE (0x7FFF), matching source/hand_oam.c's compiled
   * hand_oam_pal[3] -- that exact value is snapshotted here (HAND_BODY_WHITE) rather
   * than linking the generated, git-ignored hand_oam.c, so this test still builds on
   * a tree that never ran the art-extraction step. Proven not vacuous: reverting
   * rom_hand.c's k_pins to the old palette addresses turns this line red (pal[3]
   * reads 0x2D4A there), while every check above it still passes. */
  #define HAND_BODY_WHITE 0x7FFF
  {
    int tx = 20 >> 3, ty = 6 >> 3;
    const uint8_t* t = frames[0] + (ty * 4 + tx) * 32 + (6 & 7) * 4;
    uint8_t b = t[(20 & 7) >> 1];
    uint8_t nib = (20 & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
    chk(name, "frame-0 pixel (20,6) is raw index 3 (the sheet's own body colour)", nib == 3);
    chk(name, "glove BODY (palette index 3) decodes to white, not grey", pal[3] == HAND_BODY_WHITE);
  }

  /* 5) out-of-range frame rejected */
  uint8_t junk[ROM_HAND_FRAME_BYTES];
  chk(name, "frame 4 (out of range) rejected", !rom_hand_frame(&rh, 4, junk));

  /* 6) the verified read machinery */
  {
    uint8_t good[ROM_HAND_FRAME_BYTES];
    garble_off(&fc);
    chk(name, "clean pose-0 read for the garble baseline", rom_hand_frame(&rh, 0, good));

    /* the exposure: an UNVERIFIED read would show garbled bytes as if real */
    uint8_t once[ROM_HAND_FRAME_BYTES];
    RomHand rh_unverified = rh;
    rom_hand_set_verify(&rh_unverified, 0);
    garble(&fc, rh.sheet - ROM_BASE, 1, 0);
    chk(name, "unverified read accepts a garbled byte (the exposure)",
        rom_hand_frame(&rh_unverified, 0, once) && once[0] != good[0]);
    garble_off(&fc);

    /* one-shot glitch on the verified path: the retry heals it */
    garble(&fc, rh.sheet - ROM_BASE, 1, 0);
    uint8_t healed[ROM_HAND_FRAME_BYTES];
    chk(name, "verified read heals a transient garble",
        rom_hand_frame(&rh, 0, healed) && memcmp(healed, good, sizeof good) == 0);
    garble_off(&fc);

    /* a glitch that keeps hitting the first pass: never agrees -> fail closed */
    garble(&fc, rh.sheet - ROM_BASE, 99, 1);
    chk(name, "verified read fails closed on a glitch it can never agree on",
        !rom_hand_frame(&rh, 0, healed));
    garble_off(&fc);

    /* the palette gets the same cover */
    garble(&fc, rh.pal - ROM_BASE, 99, 1);
    chk(name, "verified palette read fails closed the same way", !rom_hand_pal(&rh, pal));
    garble_off(&fc);
  }

  printf("  %s: ok (kind=%s rev=%u)\n", name, rom_kind_name(rc.kind), rc.version);
  fclose(f);
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }
  char p[512];
  sprintf(p, "%s/Emerald.gba", dir);   run_rom(p, "Emerald",   1);
  sprintf(p, "%s/FireRed.gba", dir);   run_rom(p, "FireRed",   1);
  sprintf(p, "%s/LeafGreen.gba", dir); run_rom(p, "LeafGreen", 1);
  sprintf(p, "%s/Ruby.gba", dir);      run_rom(p, "Ruby",      0);
  sprintf(p, "%s/Sapphire.gba", dir);  run_rom(p, "Sapphire",  0);
  printf("rom_hand test: %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
