/* Host test for the FULL-IMAGE region grid (pure C, no cart, no tonc).
 *   cc -std=c11 -Wall -I source tests/host_romgrid_test.c source/pdna_romver.c \
 *      -o /tmp/hrg && /tmp/hrg
 *
 * The grid is the instrument that decides whether Guy re-copies a 12.5 MB file or goes
 * looking for a software bug, so the things worth testing are the ones that would make it
 * LIE. Two failure directions, and both are covered here:
 *
 *   IT MUST FIRE on a real difference. A single flipped BIT anywhere outside the
 *   exclusion zones has to turn exactly one region red, with the right index and the
 *   right file offset — because "region 74 @ 0x004A0000" is the entire deliverable.
 *
 *   IT MUST NOT FIRE on a healthy load. Three separate ways this instrument could cry
 *   wolf on every single SD boot, each of which would train the user to ignore it:
 *     1. the EZ-Flash kernel rewriting every aligned 0x03007FFC / 0x03FFFFFC word to
 *        0x03007FF4 in PSRAM (PatchInternal) — canonicalisation must make the region CRC
 *        invariant to that, in BOTH directions;
 *     2. the zones (ROM word 0, the descriptor's own bytes, the post-link locator
 *        records, the tail drop zone) being compared instead of read as zeroes;
 *     3. the streaming walk (real bytes / zeroes / real bytes, cut at zone boundaries)
 *        disagreeing with the way tools/stamp_rom_windows.py computes the same number,
 *        which is "zero the zones, canonicalise, then plain zlib.crc32 the slice". The
 *        stamp in this test is produced by THAT independent method on purpose: if the two
 *        ever drift the test fails on the Mac instead of on the cartridge.
 *
 * Plus the descriptor-is-not-trusted rules: the grid half of a descriptor is read out of
 * the very image under suspicion, so a wild region_shift / n_regions / zone list must
 * produce "no opinion", never a wild read and never an accusation.
 */
#include <stdio.h>
#include <string.h>
#include "pdna_romver.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define IMG_BYTES  (512u * 1024u)
#define RB         (64u * 1024u)
#define NREG       (IMG_BYTES / RB)          /* 8 */
#define HOLE_OFF   0x30000u
#define TAIL_OFF   (IMG_BYTES - PDNA_RV_TAIL_GUARD)

static unsigned char img[IMG_BYTES] __attribute__((aligned(4)));
static unsigned char ref[IMG_BYTES] __attribute__((aligned(4)));
static uint32_t      tab[16];

static uint32_t rd32(const unsigned char* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static void wr32(unsigned char* p, uint32_t v) {
  p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* The stamper's method, reimplemented independently of the walk under test: zero the
 * zones, canonicalise every aligned IRQ word, then take a plain CRC32 per region slice.
 * (zlib.crc32 == pdna_rv_crc32 with canon off; host_romver_test.c pins that.) */
static void stamp_grid(PdnaRomVerify* d) {
  uint32_t i, off;
  memcpy(ref, img, IMG_BYTES);
  for (i = 0; i < d->n_zones; i++)
    memset(ref + d->z[i].off, 0, d->z[i].len);
  for (off = 0; off + 4u <= IMG_BYTES; off += 4u) {
    uint32_t w = rd32(ref + off);
    if (w == PDNA_RV_PW0 || w == PDNA_RV_PW1) wr32(ref + off, PDNA_RV_PWC);
  }
  for (i = 0; i < d->n_regions; i++) {
    uint32_t a = i * RB, n = (IMG_BYTES - a < RB) ? (IMG_BYTES - a) : RB;
    d->rcrc[i] = pdna_rv_crc32(tab, ref + a, n);
    d->excl[i >> 5] &= ~(1u << (i & 31));
    if (i == NREG - 1) d->excl[i >> 5] |= 1u << (i & 31);   /* all tail zone */
  }
}

static void build(PdnaRomVerify* d) {
  uint32_t i;
  uint32_t s = 0x12345678u;
  for (i = 0; i < IMG_BYTES; i++) {           /* deterministic pseudo-random filling */
    s = s * 1103515245u + 12345u;
    img[i] = (unsigned char)(s >> 19);
  }
  memset(d, 0, sizeof *d);
  d->magic0 = PDNA_RV_MAGIC0; d->magic1 = PDNA_RV_MAGIC1;
  d->version = PDNA_RV_VERSION; d->stamped = PDNA_RV_STAMPED;
  d->image_bytes = IMG_BYTES;
  d->n_windows = 0; d->n_present = 0;
  d->hole_off = HOLE_OFF; d->hole_len = PDNA_RV_SIZE;
  d->region_shift = 16u; d->n_regions = NREG;
  d->n_zones = 3u;
  d->z[0].off = 0u;         d->z[0].len = PDNA_RV_HEAD_GUARD;
  d->z[1].off = HOLE_OFF;   d->z[1].len = PDNA_RV_SIZE;         /* 1732, 4-aligned */
  d->z[2].off = TAIL_OFF;   d->z[2].len = PDNA_RV_TAIL_GUARD;
  stamp_grid(d);
}

/* Scan the whole grid and return the state of region `want_i` (or -1). */
static int scan(const PdnaRomVerify* d, PdnaRvGrid* g, int want_i, int* out_state) {
  int i, st;
  if (!pdna_rv_grid_init(d, g)) return 0;
  for (i = 0; i < g->n_regions; i++) {
    st = pdna_rv_region_check(d, img, i, tab, PDNA_RV_GRID_TRIES, g);
    if (out_state && i == want_i) *out_state = st;
  }
  return 1;
}

int main(void) {
  PdnaRomVerify d;
  PdnaRvGrid    g;
  int           st = -1;
  uint32_t      expect_verified;

  pdna_rv_crc32_table(tab);

  /* 0. The layout the Python stamper hard-codes. If MAX_REGIONS/MAX_ZONES ever move, this
   *    is the assertion that says "and tools/stamp_rom_windows.py has not been told". */
  CHECK(sizeof(PdnaRomVerify) == PDNA_RV_SIZE, "descriptor %u bytes, expected %u",
        (unsigned)sizeof(PdnaRomVerify), (unsigned)PDNA_RV_SIZE);
  CHECK(PDNA_RV_ZONES_OFF == 624u && PDNA_RV_EXCL_OFF == 676u &&
        PDNA_RV_RCRC_OFF == 708u && PDNA_RV_SIZE == 1732u,
        "descriptor offsets moved: %u/%u/%u/%u - update tools/stamp_rom_windows.py",
        (unsigned)PDNA_RV_ZONES_OFF, (unsigned)PDNA_RV_EXCL_OFF,
        (unsigned)PDNA_RV_RCRC_OFF, (unsigned)PDNA_RV_SIZE);

  build(&d);

  /* 1. A healthy image: every non-excluded region matches, coverage is exactly the image
   *    minus the zones, and the excluded tail region is SKIP — never OK. */
  CHECK(scan(&d, &g, NREG - 1, &st), "grid_init refused a good descriptor");
  expect_verified = IMG_BYTES - PDNA_RV_HEAD_GUARD - PDNA_RV_SIZE - PDNA_RV_TAIL_GUARD;
  CHECK(g.region_bytes == RB && g.n_regions == (int32_t)NREG,
        "geometry: %lu B x %d", (unsigned long)g.region_bytes, (int)g.n_regions);
  CHECK(g.n_ok == (int32_t)NREG - 1, "good image: %d ok, expected %u",
        (int)g.n_ok, NREG - 1);
  CHECK(g.n_bad == 0 && g.n_unstable == 0, "good image reported %d bad, %d unstable",
        (int)g.n_bad, (int)g.n_unstable);
  CHECK(g.n_skipped == 1 && st == PDNA_RVG_SKIP,
        "the all-zone tail region must be SKIP, got %d skipped / state %d",
        (int)g.n_skipped, st);
  CHECK(g.bytes_verified == expect_verified, "verified %lu B, expected %lu",
        (unsigned long)g.bytes_verified, (unsigned long)expect_verified);
  CHECK(g.bytes_verified + g.bytes_zoned == IMG_BYTES,
        "verified+blind = %lu, image is %lu",
        (unsigned long)(g.bytes_verified + g.bytes_zoned), (unsigned long)IMG_BYTES);
  CHECK(g.first_bad == -1, "first_bad set on a clean image");
  /* The coverage contract: what grid_init derives up front as provable has to be exactly
   * what a completed scan measured. This is the number the verdict is gated on, and it is
   * also the number tools/stamp_rom_windows.py prints as "%d B verifiable". */
  CHECK(g.bytes_verifiable == expect_verified, "verifiable %lu B, expected %lu",
        (unsigned long)g.bytes_verifiable, (unsigned long)expect_verified);
  /* Region 0 holds the head zone and region 3 the descriptor hole: both pass on LESS than
   * a region's worth of bytes, and the screen colours them differently for saying so. */
  CHECK(g.n_partial == 2, "%d partial regions, expected 2 (head zone, descriptor hole)",
        (int)g.n_partial);
  CHECK(pdna_rv_region_real(&d, 1) == RB &&
        pdna_rv_region_real(&d, 0) == RB - PDNA_RV_HEAD_GUARD &&
        pdna_rv_region_real(&d, 3) == RB - PDNA_RV_SIZE &&
        pdna_rv_region_real(&d, NREG - 1) == 0u,
        "region_real: %lu / %lu / %lu / %lu",
        (unsigned long)pdna_rv_region_real(&d, 1),
        (unsigned long)pdna_rv_region_real(&d, 0),
        (unsigned long)pdna_rv_region_real(&d, 3),
        (unsigned long)pdna_rv_region_real(&d, NREG - 1));
  /* The stamper's rule and the console's derivation must agree bit for bit on a healthy
   * image, or every boot would report a damaged descriptor. */
  CHECK(g.excl_mismatch == 0, "%d excl disagreements on a healthy image",
        (int)g.excl_mismatch);
  CHECK(pdna_rv_grid_verdict(&g, 0) == PDNA_RVV_OK &&
        !pdna_rv_grid_is_fault(pdna_rv_grid_verdict(&g, 0)),
        "a clean full scan must be PDNA_RVV_OK, got %d", pdna_rv_grid_verdict(&g, 0));

  /* 2. ONE FLIPPED BIT, mid-image, must turn exactly one region red and name its address.
   *    Region 5, 0x2000 into it, i.e. file offset 0x52000. */
  {
    uint32_t bit_off = 5u * RB + 0x2000u;
    img[bit_off] ^= 0x01u;
    CHECK(scan(&d, &g, 5, &st), "grid_init refused after a bit flip");
    CHECK(g.n_bad == 1, "one flipped bit gave %d bad regions", (int)g.n_bad);
    CHECK(st == PDNA_RVG_BAD, "the flipped region's state is %d, expected BAD", st);
    CHECK(g.first_bad == 5, "first_bad = %d, expected 5", (int)g.first_bad);
    CHECK(g.first_bad_off == 5u * RB, "first_bad_off = 0x%lx, expected 0x%lx",
          (unsigned long)g.first_bad_off, (unsigned long)(5u * RB));
    CHECK(g.first_bad_state == PDNA_RVG_BAD, "a stable difference must classify as BAD");
    CHECK(g.n_ok == (int32_t)NREG - 2, "%d regions still ok, expected %u",
          (int)g.n_ok, NREG - 2);
    img[bit_off] ^= 0x01u;                     /* restore */
    CHECK(scan(&d, &g, -1, 0) && g.n_bad == 0, "restoring the bit did not clear the fault");
  }

  /* 3. A flipped bit INSIDE a zone must NOT fire: those bytes are read as zeroes on both
   *    sides. This is the mechanism that keeps the kernel's ROM-word-0 rewrite, its tail
   *    blob and our own post-link stamping from producing an alarm on every boot. */
  {
    img[0x40] ^= 0xFFu;                        /* head zone */
    img[HOLE_OFF + 8] ^= 0xFFu;                /* descriptor hole */
    img[TAIL_OFF + 0x1234] ^= 0xFFu;           /* tail drop zone */
    CHECK(scan(&d, &g, -1, 0), "grid_init refused with zone bytes changed");
    CHECK(g.n_bad == 0 && g.n_unstable == 0,
          "changing ZONE bytes fired the alarm (%d bad) - the hole rule is broken",
          (int)g.n_bad);
    img[0x40] ^= 0xFFu; img[HOLE_OFF + 8] ^= 0xFFu; img[TAIL_OFF + 0x1234] ^= 0xFFu;
  }

  /* 4. CANONICAL IRQ WORDS, both directions. Put a 0x03007FFC and a 0x03FFFFFC in the
   *    image, stamp it, then do to the image exactly what PatchInternal does in PSRAM. The
   *    verdict must not move. Without this the tool would report ROM IMAGE MODIFIED on
   *    every SD boot of a perfectly good file. */
  {
    uint32_t a = 2u * RB + 0x100u, b = 6u * RB + 0x40u;
    wr32(img + a, PDNA_RV_PW0);
    wr32(img + b, PDNA_RV_PW1);
    stamp_grid(&d);                            /* stamp WITH the pre-patch values */
    CHECK(scan(&d, &g, -1, 0) && g.n_bad == 0, "unpatched image with IRQ words fired");
    wr32(img + a, PDNA_RV_PWC);                /* the kernel's rewrite */
    wr32(img + b, PDNA_RV_PWC);
    CHECK(scan(&d, &g, -1, 0), "grid_init refused a kernel-patched image");
    CHECK(g.n_bad == 0 && g.n_unstable == 0,
          "a kernel-patched IRQ word fired the alarm (%d bad) - canon is broken",
          (int)g.n_bad);
    /* ...but a DIFFERENT value at the same place still must fire: canonicalisation may
     * not become a blanket exemption for those offsets. */
    wr32(img + a, 0x03007FF0u);
    CHECK(scan(&d, &g, 2, &st) && g.n_bad == 1 && st == PDNA_RVG_BAD,
          "canon swallowed a real change at an IRQ-word offset (%d bad, state %d)",
          (int)g.n_bad, st);
    wr32(img + a, PDNA_RV_PWC);
  }

  /* 5. THE DESCRIPTOR IS NOT TRUSTED. Each of these lives in the image under suspicion, so
   *    a flipped bit must yield "no grid" — not a wild read, not an accusation. */
  {
    PdnaRomVerify bad;
    build(&bad);
    bad.version = 1u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "a v1 descriptor must not present a grid");
    build(&bad); bad.stamped = 0u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "an UNSTAMPED descriptor must not present a grid");
    build(&bad); bad.region_shift = 0u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "region_shift 0 means no grid");
    build(&bad); bad.region_shift = 31u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "a wild region_shift must be refused");
    build(&bad); bad.n_regions = NREG + 1;
    CHECK(!pdna_rv_grid_init(&bad, &g), "n_regions must be re-derived, not believed");
    build(&bad); bad.image_bytes = 0u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "image_bytes 0 must be refused");
    build(&bad); bad.image_bytes = 0xFFFFFFFFu;
    CHECK(!pdna_rv_grid_init(&bad, &g), "a 4 GiB image_bytes must be refused");
    build(&bad); bad.n_zones = PDNA_RV_MAX_ZONES + 1u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "too many zones must be refused");
    /* TOO FEW is a fault as well, and it is the one that was missed: the stamper always
     * emits head + descriptor + tail, so anything under three means the word is damaged.
     * With n_zones == 0 the tail drop zone stops being excluded and gets COMPARED, and a
     * byte-perfect image reported "IMAGE INCOMPLETE - 4 bad" plus a remedy, on a healthy
     * cart. A false alarm on a good load is how an alarm gets trained away. */
    build(&bad); bad.n_zones = 0u;
    CHECK(!pdna_rv_grid_init(&bad, &g),
          "n_zones == 0 must be refused - it un-excludes the loader's drop zone");
    build(&bad); bad.n_zones = 1u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "n_zones == 1 must be refused");
    build(&bad); bad.n_zones = 2u;
    CHECK(!pdna_rv_grid_init(&bad, &g), "n_zones == 2 must be refused");
    build(&bad); bad.z[1].off += 2u;            /* unaligned zone */
    CHECK(!pdna_rv_grid_init(&bad, &g),
          "an unaligned zone must be refused (a canon word could straddle it)");
    build(&bad); bad.z[0].off = 0x40000u;       /* now out of order vs z[1] */
    CHECK(!pdna_rv_grid_init(&bad, &g), "an unsorted zone list must be refused");
    build(&bad); bad.z[1].len = 0x41000u;       /* runs 0x1000 into z[2] */
    CHECK(!pdna_rv_grid_init(&bad, &g), "overlapping zones must be refused");
    build(&bad); bad.z[1].len = 0x40000u;       /* ABUTS z[2] exactly - legal */
    CHECK(pdna_rv_grid_init(&bad, &g), "abutting zones are legal (the stamper merges them)");
    build(&bad); bad.z[2].len = PDNA_RV_TAIL_GUARD + 4u;   /* runs past the image */
    CHECK(!pdna_rv_grid_init(&bad, &g), "a zone past image_bytes must be refused");
  }

  /* 6. An out-of-range region index is a no-op, not a read: pdna_rv_region_check is called
   *    from a UI loop and must survive being asked for a region that does not exist. */
  build(&d);
  CHECK(pdna_rv_grid_init(&d, &g), "grid_init refused a rebuilt descriptor");
  CHECK(pdna_rv_region_check(&d, img, -1, tab, 3, &g) == PDNA_RVG_SKIP &&
        pdna_rv_region_check(&d, img, NREG + 5, tab, 3, &g) == PDNA_RVG_SKIP &&
        g.n_done == 0,
        "an out-of-range region index must be a no-op");

  /* 7. THE FLIPPED EXCLUSION BIT. This is the adversarial review's headline finding,
   *    reproduced exactly: an image with a REAL 4 KiB hole in region 6, plus one flipped
   *    bit in the stamped excl bitmap claiming region 6 has nothing to compare. Obeying
   *    that bitmap turned the verdict from "IMAGE INCOMPLETE - 1 bad" into a green
   *    "IMAGE OK - 190 regions match, 2 skipped". The bitmap is fully derivable from the
   *    zone list, so the zone list decides and the bitmap is only compared against. */
  build(&d);
  {
    uint32_t hole = 6u * RB + 0x1000u;
    memset(img + hole, 0, 4096);                 /* the hole a partial SD load leaves */
    d.excl[6u >> 5] |= 1u << 6;                  /* "nothing to compare in region 6"  */

    CHECK(scan(&d, &g, 6, &st), "grid_init refused after an excl bit was flipped");
    CHECK(st == PDNA_RVG_BAD,
          "region 6 has a 4 KiB hole and a set excl bit: state %d, expected BAD", st);
    CHECK(g.n_bad == 1 && g.first_bad == 6,
          "the hole must still be found and named: %d bad, first_bad %d",
          (int)g.n_bad, (int)g.first_bad);
    CHECK(g.n_skipped == 1,
          "a flipped excl bit must not add a skip: %d skipped", (int)g.n_skipped);
    CHECK(g.n_ok == (int32_t)NREG - 2, "%d ok, expected %u", (int)g.n_ok, NREG - 2);
    CHECK(g.excl_mismatch == 1, "the disagreement must be COUNTED, got %d",
          (int)g.excl_mismatch);
    CHECK(pdna_rv_grid_verdict(&g, 0) == PDNA_RVV_INCOMPLETE,
          "verdict %d, expected INCOMPLETE", pdna_rv_grid_verdict(&g, 0));
    CHECK(pdna_rv_grid_is_fault(pdna_rv_grid_verdict(&g, 0)),
          "a hole hidden behind an excl bit must still be a FAULT");

    /* And the same bit flipped on a HEALTHY image is reported rather than swallowed: the
     * descriptor lives inside its own zone, so this disagreement is the only evidence any
     * of these instruments ever gets that the descriptor's own bytes were damaged. */
    build(&d);
    d.excl[2u >> 5] |= 1u << 2;
    CHECK(scan(&d, &g, 2, &st), "grid_init refused a healthy image with a flipped bit");
    CHECK(st == PDNA_RVG_OK && g.n_bad == 0,
          "region 2 is healthy: state %d, %d bad", st, (int)g.n_bad);
    CHECK(g.excl_mismatch == 1 && pdna_rv_grid_verdict(&g, 0) == PDNA_RVV_DESC,
          "a lying excl bit on a healthy image must read as DESCRIPTOR DAMAGED "
          "(%d mismatches, verdict %d)",
          (int)g.excl_mismatch, pdna_rv_grid_verdict(&g, 0));
  }

  /* 8. THE SCAN THAT MEASURED NOTHING. Zones that cover the whole image exclude every
   *    region, so n_bad and n_unstable are both 0 — and the old verdict, (n_bad +
   *    n_unstable) > 0, called that GREEN: "IMAGE OK - 0 regions match, 192 skipped",
   *    under the sentence "Every region matched its build-time CRC32.". */
  build(&d);
  {
    d.n_zones = 3u;
    d.z[0].off = 0u;         d.z[0].len = 0x20000u;
    d.z[1].off = 0x20000u;   d.z[1].len = 0x20000u;
    d.z[2].off = 0x40000u;   d.z[2].len = 0x40000u;   /* ... to EOF */
    d.excl[0] = 0xFFu;                                /* honestly stamped: all skipped */

    CHECK(pdna_rv_grid_init(&d, &g), "an all-zone image is still a valid descriptor");
    CHECK(g.bytes_verifiable == 0u, "verifiable %lu B, expected 0",
          (unsigned long)g.bytes_verifiable);
    CHECK(g.excl_mismatch == 0, "the excl bitmap agrees here (%d)", (int)g.excl_mismatch);
    for (st = 0; st < (int)NREG; st++)
      (void)pdna_rv_region_check(&d, img, st, tab, PDNA_RV_GRID_TRIES, &g);
    CHECK(g.n_skipped == (int32_t)NREG && g.n_ok == 0 && g.bytes_verified == 0u,
          "%d skipped, %d ok, %lu B verified", (int)g.n_skipped, (int)g.n_ok,
          (unsigned long)g.bytes_verified);
    CHECK(g.n_bad == 0 && g.n_unstable == 0, "nothing can fail when nothing is read");
    CHECK(pdna_rv_grid_verdict(&g, 0) == PDNA_RVV_NOTHING,
          "a scan that compared 0 bytes must be NOTHING VERIFIED, got %d",
          pdna_rv_grid_verdict(&g, 0));
    CHECK(pdna_rv_grid_is_fault(pdna_rv_grid_verdict(&g, 0)),
          "NOTHING VERIFIED must paint as a fault, never green");
  }

  /* 9. The verdict rule itself, branch by branch. It is a pure function of the counters
   *    precisely so the thing the user reads can be pinned without a GBA. */
  {
    PdnaRvGrid v;
    memset(&v, 0, sizeof v);
    v.n_regions = 8; v.n_ok = 7; v.bytes_verified = 1000u; v.bytes_verifiable = 1000u;
    CHECK(pdna_rv_grid_verdict(&v, 0) == PDNA_RVV_OK, "full clean scan is OK");
    CHECK(!pdna_rv_grid_is_fault(PDNA_RVV_OK) && !pdna_rv_grid_is_fault(PDNA_RVV_STOPPED),
          "OK and STOPPED are not faults");
    CHECK(pdna_rv_grid_is_fault(PDNA_RVV_INCOMPLETE) &&
          pdna_rv_grid_is_fault(PDNA_RVV_DESC) &&
          pdna_rv_grid_is_fault(PDNA_RVV_NOTHING) &&
          pdna_rv_grid_is_fault(PDNA_RVV_SHORT),
          "every other verdict is a fault");
    CHECK(pdna_rv_grid_verdict(&v, 1) == PDNA_RVV_STOPPED, "aborted clean scan is STOPPED");

    /* An abort does not bury a finding. */
    v.n_bad = 1;
    CHECK(pdna_rv_grid_verdict(&v, 1) == PDNA_RVV_INCOMPLETE,
          "a bad region outranks the abort");
    v.n_bad = 0; v.n_unstable = 1;
    CHECK(pdna_rv_grid_verdict(&v, 0) == PDNA_RVV_INCOMPLETE, "an unstable region fires");
    v.n_unstable = 0; v.excl_mismatch = 3;
    CHECK(pdna_rv_grid_verdict(&v, 0) == PDNA_RVV_DESC, "a lying skip map fires");
    v.excl_mismatch = 0;

    /* Half the image skipped away and the rest clean: still not a pass. */
    v.bytes_verified = 999u;
    CHECK(pdna_rv_grid_verdict(&v, 0) == PDNA_RVV_SHORT,
          "one byte short of the verifiable total is COVERAGE SHORT");
    CHECK(pdna_rv_grid_verdict(&v, 1) == PDNA_RVV_STOPPED,
          "an aborted scan is judged on faults only, not on coverage");
    v.bytes_verified = 1000u; v.n_ok = 0; v.n_recovered = 0;
    CHECK(pdna_rv_grid_verdict(&v, 0) == PDNA_RVV_NOTHING,
          "zero matched regions is NOTHING VERIFIED even at full byte coverage");
    v.n_recovered = 1;
    CHECK(pdna_rv_grid_verdict(&v, 0) == PDNA_RVV_OK,
          "a region that matched only on a retry still counts as measured");
    CHECK(pdna_rv_grid_verdict(0, 0) == PDNA_RVV_NOTHING, "a null grid claims nothing");
  }

  printf(fails ? "%d FAILURES\n" : "all pass\n", fails);
  return fails ? 1 : 0;
}
