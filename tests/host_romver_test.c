/* Host test for the pure-C ROM self-check core (no cart, no tonc).
 *   cc -std=c11 -Wall -I source tests/host_romver_test.c source/pdna_romver.c \
 *      -o /tmp/hrv && /tmp/hrv
 *
 * Covers the things that would silently DISARM the instrument, and the things that
 * would make it fire on a healthy cart:
 *   1. the CRC32 flavour (must equal zlib.crc32, or every stamped value is wrong) and
 *      the 16-entry nibble table agreeing with the classic check value;
 *   2. word path == byte path, so the cartridge's aligned fast path cannot drift from
 *      the host verifier;
 *   3. good / one-flipped-byte / unstamped / artless verdicts;
 *   4. CORRUPT DESCRIPTOR cases - a wild len and a wild base_addr must be skipped, not
 *      obeyed. On the cartridge an obeyed len=0xFFFFFFFF is a 4 GiB read at boot: a hang
 *      manufactured by the anti-hang instrument itself;
 *   5. the bus-vs-image discrimination (pdna_rv_classify), which decides whether the
 *      tool tells the user "re-copy the file" or "your cart bus is lying", and whether
 *      it latches read-only. There is no way to make host memory read unstably, so the
 *      classifier is exercised directly with attempt vectors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdna_romver.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define IMG_BYTES 0x20000u
#define FAKE_BASE 0x08000000u

static void stamp_two(PdnaRomVerify* d, const unsigned char* img, uint32_t* tab) {
  unsigned i;
  for (i = 0; i < 2; i++)
    d->w[i].crc = pdna_rv_crc32(tab, img + (d->w[i].base_addr + d->w[i].off - FAKE_BASE),
                                d->w[i].len);
}

int main(void) {
  static uint32_t tab[16];
  unsigned char*  img = malloc(IMG_BYTES);
  PdnaRomVerify   d;
  PdnaRvResult    r;
  unsigned        i;

  CHECK(img != NULL, "out of memory");
  if (!img) return 1;

  CHECK(sizeof(PdnaRomVerify) == PDNA_RV_SIZE, "descriptor is %u bytes, expected %u",
        (unsigned)sizeof(PdnaRomVerify), (unsigned)PDNA_RV_SIZE);
  CHECK(sizeof(PdnaRvWindow) == 24u, "window is %u bytes", (unsigned)sizeof(PdnaRvWindow));

  pdna_rv_crc32_table(tab);

  /* 1. the standard CRC32 check value - this is the contract with zlib.crc32, and the
   *    proof that the 16-entry nibble table computes the same function as the 256-entry
   *    byte table it replaced. */
  CHECK(pdna_rv_crc32(tab, "123456789", 9) == 0xCBF43926u,
        "CRC32(\"123456789\") = %08x, expected cbf43926",
        pdna_rv_crc32(tab, "123456789", 9));
  CHECK(pdna_rv_crc32(tab, "", 0) == 0u, "CRC32(\"\") = %08x, expected 0",
        pdna_rv_crc32(tab, "", 0));
  CHECK(pdna_rv_crc32(tab, "a", 1) == 0xE8B7BE43u, "CRC32(\"a\") = %08x, expected e8b7be43",
        pdna_rv_crc32(tab, "a", 1));

  for (i = 0; i < IMG_BYTES; i++) img[i] = (unsigned char)(i * 31u + (i >> 5));

  /* 2. word path == byte path (the aligned fast path must not change the answer) */
  {
    unsigned char* un = malloc(4096 + 1);
    CHECK(un != NULL, "out of memory");
    if (un) {
      memcpy(un + 1, img + 0x1000, 4096);
      CHECK(pdna_rv_crc32(tab, img + 0x1000, 4096) == pdna_rv_crc32(tab, un + 1, 4096),
            "word path and byte path disagree");
      free(un);
    }
  }

  /* 3. a stamped descriptor over a good image */
  memset(&d, 0, sizeof d);
  d.magic0 = PDNA_RV_MAGIC0; d.magic1 = PDNA_RV_MAGIC1;
  d.version = PDNA_RV_VERSION; d.stamped = PDNA_RV_STAMPED;
  d.n_windows = 3; d.image_bytes = IMG_BYTES;
  d.w[0].base_addr = FAKE_BASE + 0x1000; d.w[0].len = 4096;
  memcpy(d.w[0].tag, "AAAA", 4);
  d.w[1].base_addr = FAKE_BASE + 0x8000; d.w[1].off = 0x1000; d.w[1].len = 4096;
  memcpy(d.w[1].tag, "BBBB", 4);
  d.w[2].base_addr = 0;                  d.w[2].len = 4096;   /* absent anchor */
  memcpy(d.w[2].tag, "GONE", 4);
  stamp_two(&d, img, tab);

  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_OK, "good image: verdict %d", (int)r.verdict);
  CHECK(r.n_checked == 2, "absent anchor was counted: n_checked=%d", (int)r.n_checked);
  CHECK(r.n_skipped == 0, "a well-formed window was skipped: %d", (int)r.n_skipped);

  /* 3b. a Thumb-function anchor arrives with bit 0 set (R_ARM_ABS32 interworking bit).
   *     The check must mask it, or every .text window silently reads one byte early. */
  d.w[0].base_addr |= 1u;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_OK && r.n_checked == 2,
        "thumb-bit anchor not masked: verdict %d n_checked %d",
        (int)r.verdict, (int)r.n_checked);
  d.w[0].base_addr &= ~1u;

  /* 4. one flipped byte inside window 1 -> BAD_IMAGE, and it names the window */
  img[0x9000 + 77] ^= 0x01;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_BAD_IMAGE, "flipped byte: verdict %d", (int)r.verdict);
  CHECK(r.n_bad == 1 && r.first_bad == 1, "n_bad=%d first_bad=%d",
        (int)r.n_bad, (int)r.first_bad);
  CHECK(memcmp(r.bad_tag, "BBBB", 4) == 0, "wrong tag reported: %.4s", r.bad_tag);
  CHECK(r.bad_got == r.bad_got2, "stable image reported as an unstable bus");
  CHECK(r.bad_tries == PDNA_RV_TRIES, "a failing window must retry %d times, did %d",
        PDNA_RV_TRIES, (int)r.bad_tries);
  img[0x9000 + 77] ^= 0x01;

  /* 5. unstamped -> no opinion, NOT a failure */
  d.stamped = 0;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_UNSTAMPED, "unstamped: verdict %d", (int)r.verdict);
  d.stamped = PDNA_RV_STAMPED;

  /* 6. wrong magic / wrong version -> no opinion */
  d.magic1 ^= 0xFFu;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_UNSTAMPED, "bad magic: verdict %d", (int)r.verdict);
  d.magic1 ^= 0xFFu;
  d.version = 99;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_UNSTAMPED, "bad version: verdict %d", (int)r.verdict);
  d.version = PDNA_RV_VERSION;

  /* ---- CORRUPT DESCRIPTOR. The descriptor lives in the ROM we are accusing, so a
   * flipped bit in it is exactly the input that must not be obeyed. Each of these,
   * unclamped, is a read far outside the image. ---- */

  /* 7a. a wild length */
  d.w[1].len = 0xFFFFFFFFu;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.n_checked == 1 && r.n_skipped == 1, "wild len not skipped: checked=%d skipped=%d",
        (int)r.n_checked, (int)r.n_skipped);
  CHECK(r.verdict == PDNA_RV_OK, "wild len should skip, not accuse: verdict %d",
        (int)r.verdict);
  /* 7b. a length just over the clamp */
  d.w[1].len = PDNA_RV_MAX_WINLEN + 4u;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.n_skipped == 1, "len just over the clamp not skipped: %d", (int)r.n_skipped);
  /* 7c. a length that is legal on its own but runs off the end of the image */
  d.w[1].len = 4096;
  d.w[1].off = IMG_BYTES - 2048u - 0x8000u;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.n_skipped == 1, "window running past image_bytes not skipped: %d",
        (int)r.n_skipped);
  d.w[1].off = 0x1000;

  /* 7d. a wild base_addr - outside the cart window entirely */
  d.w[1].base_addr = 0x0E00BEEFu;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.n_skipped == 1 && r.n_checked == 1, "wild base not skipped: checked=%d skipped=%d",
        (int)r.n_checked, (int)r.n_skipped);
  /* 7e. a base BELOW rom_base */
  d.w[1].base_addr = 0x02000000u;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.n_skipped == 1, "base below rom_base not skipped: %d", (int)r.n_skipped);
  /* 7f. base + off overflowing 32 bits */
  d.w[1].base_addr = 0xFFFFF000u; d.w[1].off = 0x2000u;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.n_skipped == 1, "base+off overflow not skipped: %d", (int)r.n_skipped);
  d.w[1].base_addr = FAKE_BASE + 0x8000; d.w[1].off = 0x1000;

  /* 7g. a wild image_bytes must stop the whole check, not become a 32-bit bound */
  d.image_bytes = 0xDEADBEEFu;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_BAD_DESC, "wild image_bytes: verdict %d", (int)r.verdict);
  CHECK(r.n_checked == 0, "wild image_bytes still read %d window(s)", (int)r.n_checked);
  d.image_bytes = 0;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_BAD_DESC, "zero image_bytes: verdict %d", (int)r.verdict);
  d.image_bytes = IMG_BYTES;

  /* 7h. n_windows past the array must be clamped, not walked off */
  d.n_windows = 9999;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.n_checked == 2, "n_windows was not clamped: n_checked=%d", (int)r.n_checked);
  d.n_windows = 3;

  /* 8. artless: every anchor absent -> NOWINDOWS, still not a failure */
  d.w[0].base_addr = d.w[1].base_addr = 0;
  pdna_rv_check(&d, img, FAKE_BASE, tab, &r);
  CHECK(r.verdict == PDNA_RV_NOWINDOWS, "artless: verdict %d", (int)r.verdict);
  d.w[0].base_addr = FAKE_BASE + 0x1000;
  d.w[1].base_addr = FAKE_BASE + 0x8000;

  /* ---- 9. bus vs image: the classifier that decides the message AND the latch ---- */
  {
    uint32_t g[PDNA_RV_TRIES];
    g[0] = 0x11111111u;
    CHECK(pdna_rv_classify(0x11111111u, g, 1) == PDNA_RVC_MATCH, "first-read match");

    g[0] = 0x22222222u; g[1] = 0x11111111u;
    CHECK(pdna_rv_classify(0x11111111u, g, 2) == PDNA_RVC_RECOVERED,
          "a retry that matches must be RECOVERED (a pass), like icopy_verified");

    g[0] = 0x22222222u; g[1] = 0x33333333u; g[2] = 0x44444444u; g[3] = 0x55555555u;
    CHECK(pdna_rv_classify(0x11111111u, g, 4) == PDNA_RVC_UNSTABLE,
          "four disagreeing reads must be UNSTABLE (bus), not a bad image");

    g[0] = g[1] = g[2] = g[3] = 0x22222222u;
    CHECK(pdna_rv_classify(0x11111111u, g, 4) == PDNA_RVC_STABLE_BAD,
          "four identical wrong reads must be STABLE_BAD (image), not a bus fault");

    CHECK(pdna_rv_classify(0x11111111u, g, 0) == PDNA_RVC_UNSTABLE,
          "no attempts must never be reported as a bad image");
  }

  free(img);
  printf(fails ? "FAILED (%d)\n" : "all pass\n", fails);
  return fails ? 1 : 0;
}
