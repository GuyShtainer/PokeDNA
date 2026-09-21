/* source/gen1_save.c's gen1_detect_yellow()/gen1_detect_yellow_window() -- BACKLOG
 * #191b: stop asking "Red, Blue or Yellow?" when the save already proves Yellow.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gen1_yellow_detect_test.c \
 *      source/gen1_save.c -o /tmp/hg1yellow && /tmp/hg1yellow
 *
 * Checks:
 *   1. Corpus oracle: Yellow.sav -> 1, Red.sav -> -1 (the window is genuinely all
 *      zero there -- see gen1_save.h's own derivation comment for why "-1", never
 *      "0", is the honest answer for an all-zero window). Missing corpus SKIPs.
 *   2. Mutant A: a /tmp copy of Yellow.sav with the whole 128-byte window zeroed ->
 *      must stop being 1 (the exact mutation the detector exists to catch -- a test
 *      that still says 1 here would be worthless).
 *   3. Mutant B: a /tmp copy of Red.sav with ONE byte inside the window set non-zero
 *      (the same value pokeyellow's own InitPlayerData writes into wPikachuMood,
 *      0x80) -> flips from -1 to 1, exactly as the "any non-zero byte" rule says.
 *   4. Bounds/argument safety: NULL, a too-short buffer, and the windowed entry point
 *      called with `len` under GEN1_YELLOW_WIN_LEN all return -1, never crash, never 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gen1_save.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

static uint32_t load_sav(const char* file, uint8_t* dst, uint32_t cap) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(dst, 1, cap, f);
  fclose(f);
  return n;
}

static uint8_t g_img[GEN1_SAVE_SIZE];

int main(void) {
  printf("host_gen1_yellow_detect_test:\n");

  /* ---- Check 1: corpus oracle ------------------------------------------- */
  uint32_t ylen = load_sav("Yellow.sav", g_img, sizeof g_img);
  if (!ylen) {
    printf("  SKIP Yellow.sav (not present)\n");
  } else {
    int r = gen1_detect_yellow(g_img, ylen);
    CHECK(r == 1, "Yellow.sav must detect as Yellow (1), got %d", r);
    /* the corpus derivation this whole file is built on: the specific bytes cited
     * in gen1_save.h's own comment (0xCC at happiness, 0x80 -- pokeyellow's own
     * documented init byte -- at mood, immediately after). Pin them so a future
     * corpus swap can't silently agree with a wrong offset by accident. */
    CHECK(ylen >= GEN1_YELLOW_WIN_OFF + GEN1_YELLOW_WIN_LEN, "Yellow.sav too short to cover the window");
    if (ylen >= GEN1_YELLOW_WIN_OFF + GEN1_YELLOW_WIN_LEN) {
      uint8_t happiness = g_img[GEN1_YELLOW_WIN_OFF + 0x40];
      uint8_t mood       = g_img[GEN1_YELLOW_WIN_OFF + 0x41];
      CHECK(happiness == 0xCC, "wPikachuHappiness byte: expected 0xCC (Guy's corpus), got 0x%02X", happiness);
      CHECK(mood == 0x80, "wPikachuMood byte: expected 0x80 (pokeyellow's own init value), got 0x%02X", mood);
    }
  }

  uint32_t rlen = load_sav("Red.sav", g_img, sizeof g_img);
  uint8_t red_img[GEN1_SAVE_SIZE];
  if (!rlen) {
    printf("  SKIP Red.sav (not present)\n");
  } else {
    memcpy(red_img, g_img, rlen);
    int r = gen1_detect_yellow(g_img, rlen);
    CHECK(r == -1, "Red.sav: the window is genuinely all zero in the corpus, so the honest"
                   " answer is -1 (ambiguous), never 0 -- got %d", r);
    CHECK(rlen >= GEN1_YELLOW_WIN_OFF + GEN1_YELLOW_WIN_LEN, "Red.sav too short to cover the window");
    if (rlen >= GEN1_YELLOW_WIN_OFF + GEN1_YELLOW_WIN_LEN) {
      bool allzero = true;
      for (uint32_t i = 0; i < GEN1_YELLOW_WIN_LEN; i++)
        if (g_img[GEN1_YELLOW_WIN_OFF + i] != 0) { allzero = false; break; }
      CHECK(allzero, "Red.sav's window is expected all-zero in Guy's corpus (the premise check 3's mutant tests)");
    }
  }

  /* ---- Check 2: mutant A -- zero Yellow's window, real source, real mutation --- */
  if (ylen >= GEN1_YELLOW_WIN_OFF + GEN1_YELLOW_WIN_LEN) {
    uint8_t mut[GEN1_SAVE_SIZE];
    load_sav("Yellow.sav", mut, sizeof mut);
    memset(mut + GEN1_YELLOW_WIN_OFF, 0, GEN1_YELLOW_WIN_LEN);
    int r = gen1_detect_yellow(mut, ylen);
    CHECK(r != 1, "zeroing Yellow.sav's whole window must stop the detector saying 1, got %d", r);
    CHECK(r == -1, "and specifically -1 (all-zero is ambiguous, not '0 not Yellow'), got %d", r);
  } else {
    printf("  SKIP mutant A (no Yellow.sav)\n");
  }

  /* ---- Check 3: mutant B -- one non-zero byte in Red's window flips -1 -> 1 --- */
  if (rlen >= GEN1_YELLOW_WIN_OFF + GEN1_YELLOW_WIN_LEN) {
    uint8_t mut[GEN1_SAVE_SIZE];
    memcpy(mut, red_img, rlen);
    int before = gen1_detect_yellow(mut, rlen);
    CHECK(before == -1, "premise: Red.sav's window must start all-zero for this mutant to prove anything, got %d", before);
    mut[GEN1_YELLOW_WIN_OFF + 0x41] = 0x80;   /* pokeyellow's own wPikachuMood init byte */
    int after = gen1_detect_yellow(mut, rlen);
    CHECK(after == 1, "one non-zero byte inside the window must flip the answer to 1 (Yellow), got %d", after);
  } else {
    printf("  SKIP mutant B (no Red.sav)\n");
  }

  /* ---- Check 4: bounds/argument safety ----------------------------------- */
  CHECK(gen1_detect_yellow(NULL, GEN1_SAVE_SIZE) == -1, "NULL sav must return -1, not crash");
  CHECK(gen1_detect_yellow(g_img, GEN1_YELLOW_WIN_OFF) == -1,
        "a buffer shorter than the window's offset must return -1 (cannot even reach it)");
  CHECK(gen1_detect_yellow(g_img, GEN1_YELLOW_WIN_OFF + GEN1_YELLOW_WIN_LEN - 1) == -1,
        "a buffer one byte short of covering the whole window must return -1");
  CHECK(gen1_detect_yellow_window(NULL, GEN1_YELLOW_WIN_LEN) == -1, "NULL window must return -1, not crash");
  {
    uint8_t w[GEN1_YELLOW_WIN_LEN]; memset(w, 0xFF, sizeof w);
    CHECK(gen1_detect_yellow_window(w, GEN1_YELLOW_WIN_LEN - 1) == -1,
          "a short `len` on the windowed entry point must return -1 even though the buffer itself is non-zero");
    CHECK(gen1_detect_yellow_window(w, GEN1_YELLOW_WIN_LEN) == 1, "a full non-zero window must return 1");
  }

  if (g_fail) {
    printf("host_gen1_yellow_detect_test: %d/%d FAIL\n", g_fail, g_check);
    return 1;
  }
  printf("host_gen1_yellow_detect_test: %d/%d OK\n", g_check, g_check);
  return 0;
}
