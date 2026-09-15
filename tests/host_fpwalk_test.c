/* Host test: the EZ-Flash page fingerprint (lib/ezflashomega/ezfo_fp.h).
 *
 * io_ezfo.c finds the rompage the CPU is running from by comparing 32 sampled windows
 * of the running image against each candidate page (see "identifying OUR page" there).
 * This walks the SAME code -- ezfo_fp.h is included, not re-typed -- over heap images
 * of the two shipped sizes and checks the properties the driver relies on:
 *   - an image fingerprints itself (self-compare MATCH);
 *   - every one of the 32 x 64 = 2,048 sampled bytes is load-bearing (a single-bit flip
 *     in any of them is caught, 2048/2048);
 *   - the last window ends exactly at the image's end, so a shorter or partial copy
 *     (bytes past a cut replaced by erased-NOR 0xFF) is rejected at every cut point;
 *   - a different image of the same size is rejected;
 *   - a tiny image takes the "first 64 B for every window" branch.
 *
 *   cc -std=c11 -I lib/ezflashomega tests/host_fpwalk_test.c -o /tmp/hfp && /tmp/hfp
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezfo_fp.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t xs;
static uint8_t* image(size_t n, uint32_t seed) {
  uint8_t* b = malloc(n);
  if (!b) { printf("  FAIL: malloc %zu\n", n); exit(1); }
  xs = seed;
  for (size_t i = 0; i < n; i++) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; b[i] = (uint8_t)xs; }
  return b;
}

static void check_image(size_t size) {
  uint8_t* img  = image(size, 1u);
  uint8_t* copy = malloc(size);
  unsigned long lo = (unsigned long)img, hi = lo + size;
  uint32_t fp[EZFO_FP_WINS];
  unsigned long hi_touched = 0;
  int caught = 0, total = 0;
  printf("== image of %zu B ==\n", size);

  ezfo_fp_walk(fp, 1, lo, hi);
  CHECK(ezfo_fp_walk(fp, 0, lo, hi) == 1, "self-compare must MATCH");

  /* the touched set, from the shipped window helper (not a re-derived formula) */
  for (int i = 0; i < EZFO_FP_WINS; i++) {
    unsigned long w = ezfo_fp_window(i, lo, hi);
    CHECK(w >= lo && w + EZFO_FP_BYTES <= hi, "window %d [%lu,+64) outside the image", i, w - lo);
    CHECK((w & 3ul) == 0, "window %d not 4-byte aligned", i);
    if (w + EZFO_FP_BYTES > hi_touched) hi_touched = w + EZFO_FP_BYTES;
    for (int b = 0; b < EZFO_FP_BYTES; b++) {
      size_t off = (size_t)(w - lo) + (size_t)b;
      img[off] ^= 0x01;                       /* one bit */
      total++;
      if (!ezfo_fp_walk(fp, 0, lo, hi)) caught++;
      img[off] ^= 0x01;
    }
  }
  CHECK(total == EZFO_FP_WINS * EZFO_FP_BYTES, "sampled bytes: %d", total);
  CHECK(caught == total, "single-bit flips caught %d/%d", caught, total);
  printf("  single-bit flips caught %d/%d\n", caught, total);
  CHECK(hi_touched - lo == size, "highest byte touched %lu != image size %zu", hi_touched - lo, size);
  printf("  highest byte touched = %lu (image size %zu)\n", hi_touched - lo, size);
  CHECK(ezfo_fp_walk(fp, 0, lo, hi) == 1, "image intact after the flips");

  /* truncation: a copy identical up to k, erased NOR (0xFF) after it */
  for (int j = 0; j < 10; j++) {
    size_t k = size / 10 * (size_t)j;         /* 0 %, 10 %, ... 90 % */
    memcpy(copy, img, size);
    memset(copy + k, 0xFF, size - k);
    CHECK(ezfo_fp_walk(fp, 0, (unsigned long)copy, (unsigned long)copy + size) == 0,
          "copy truncated at %zu (%d0%%) must be rejected", k, j);
  }
  /* the tightest cut: everything but the last 64 B intact */
  memcpy(copy, img, size);
  memset(copy + size - EZFO_FP_BYTES, 0xFF, EZFO_FP_BYTES);
  CHECK(ezfo_fp_walk(fp, 0, (unsigned long)copy, (unsigned long)copy + size) == 0,
        "copy missing only its last 64 B must be rejected");

  /* a different build of the same size */
  free(copy);
  copy = image(size, 2u);
  CHECK(ezfo_fp_walk(fp, 0, (unsigned long)copy, (unsigned long)copy + size) == 0,
        "a different same-size image must be rejected");
  /* ...even one that shares the whole header block */
  memcpy(copy, img, 0xC0);
  CHECK(ezfo_fp_walk(fp, 0, (unsigned long)copy, (unsigned long)copy + size) == 0,
        "a different image with an identical 0xC0 header must be rejected");
  free(copy);
  free(img);
}

int main(void) {
  /* the two shipped sizes at the time of writing (PokeDNA-artless.gba, PokeDNA.gba) */
  check_image(789224);
  check_image(7842064);

  /* tiny image: too small for 32 spread windows -> every window is the first 64 B */
  {
    size_t size = 2000;
    uint8_t* img = image(size, 3u);
    unsigned long lo = (unsigned long)img, hi = lo + size;
    uint32_t fp[EZFO_FP_WINS];
    printf("== tiny image of %zu B ==\n", size);
    for (int i = 0; i < EZFO_FP_WINS; i++)
      CHECK(ezfo_fp_window(i, lo, hi) == lo, "tiny: window %d must be the image start", i);
    ezfo_fp_walk(fp, 1, lo, hi);
    CHECK(ezfo_fp_walk(fp, 0, lo, hi) == 1, "tiny: self-compare must MATCH");
    img[0] ^= 0x80;
    CHECK(ezfo_fp_walk(fp, 0, lo, hi) == 0, "tiny: a flip in the first 64 B is caught");
    img[0] ^= 0x80;
    img[1000] ^= 0x80;
    CHECK(ezfo_fp_walk(fp, 0, lo, hi) == 1, "tiny: byte 1000 is NOT sampled (documented gap)");
    img[1000] ^= 0x80;
    free(img);
  }

  if (fails) { printf("\nhost_fpwalk_test: %d FAILED\n", fails); return 1; }
  printf("\nhost_fpwalk_test: all checks passed\n");
  return 0;
}
