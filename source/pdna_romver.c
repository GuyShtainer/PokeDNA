/* pdna_romver.c — CRC32 + the window comparison. Pure C: no tonc, no sys.h, so
 * tests/host_romver_test.c compiles it straight on the Mac. See pdna_romver.h for why
 * this exists, what an OK verdict does and does not prove, and what the EZ-Flash kernel
 * does to the image behind our back. */

#include "pdna_romver.h"

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "pdna_romver.c's word path assumes little-endian (the GBA and the Mac both are)"
#endif

/* 16-entry nibble table: 64 bytes of caller scratch instead of 1 KiB. The reason is a
 * stack-depth constraint, not speed — see the comment on pdna_rv_crc32_table in the
 * header. Generated from the polynomial, so there is nothing on the cart bus that could
 * corrupt the table itself. */
void pdna_rv_crc32_table(uint32_t* t) {
  uint32_t i;
  for (i = 0; i < 16u; i++) {
    uint32_t c = i;
    int k;
    for (k = 0; k < 4; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
}

#define PDNA_RV_STEP(c, byte) do {                 \
    (c) ^= (uint32_t)(uint8_t)(byte);              \
    (c) = ((c) >> 4) ^ t[(c) & 0xFu];              \
    (c) = ((c) >> 4) ^ t[(c) & 0xFu];              \
  } while (0)

/* One 32-bit word, little-endian, four bytes at a time. */
#define PDNA_RV_STEP4(c, v) do {                   \
    uint32_t v_ = (v);                             \
    PDNA_RV_STEP(c, v_); v_ >>= 8;                 \
    PDNA_RV_STEP(c, v_); v_ >>= 8;                 \
    PDNA_RV_STEP(c, v_); v_ >>= 8;                 \
    PDNA_RV_STEP(c, v_);                           \
  } while (0)

/* The canonicalisation test, written so the HOT path never touches a literal pool.
 *
 * This matters more than it looks. The whole-image scan is ~12.5 MB and its cost is
 * dominated by CART FETCHES — instructions and literal pools come off the same bus as the
 * data. MEASURED by the verifier's own calibration slab (mGBA, 2026-08-18, screenshot at
 * docs/analysis-2026-08-18/romfull-verdict.png): 256 KiB costs ~0.49 s at 0x4317 and
 * ~0.85 s at the loader's 0x0000, and the full 12.5 MB scan lands around 27 s. (Those move
 * by ~20% between builds purely from code layout, which is itself the point.) So two extra
 * 32-bit constants compared per word are NOT free: in Thumb each is a literal-pool load
 * from the very ROM being scanned, on the slowest device in the machine.
 *
 * Both canonical words end in 0xFC, and 0xFC is an 8-bit immediate, so `(v & 0xFF) == 0xFC`
 * compiles to uxtb+cmp with no pool access. Only a word that passes it — a handful in a
 * 12.5 MB image — pays for the full comparison. */
#define PDNA_RV_IS_CANON(v) \
    (((v) & 0xFFu) == 0xFCu && ((v) == PDNA_RV_PW0 || (v) == PDNA_RV_PW1))

/* The aligned path reads the cartridge 32 bits at a time. That is not cosmetic: on the
 * GBA bus a byte read costs a full non-sequential access, so a byte loop pays several
 * cycles per byte purely to fetch, where one word read serves four. The stamper
 * guarantees base|off|len are all 4-aligned, so the fast path is the only one that ever
 * runs on the cartridge; the byte path exists for the host test (and for a corrupt
 * descriptor that somehow got past the clamps). */
uint32_t pdna_rv_crc32_upd(const uint32_t* t, uint32_t c,
                           const void* data, uint32_t len, int canon) {
  const unsigned char* b = (const unsigned char*)data;

  if (((((uintptr_t)b) | (uintptr_t)len) & 3u) == 0u) {
    const uint32_t* w = (const uint32_t*)(const void*)b;
    uint32_t n = len >> 2;
    /* Two loops rather than one with a flag in it: `canon` is loop-invariant and the
     * window path (which runs at every boot) must not pay a branch per word for a feature
     * only the grid uses. */
    if (canon) {
      while (n--) {
        uint32_t v = *w++;
        /* The kernel's IRQ-word collapse, applied identically by the stamper — without it
         * a whole-image CRC would false-alarm on every SD boot that patched a site. */
        if (PDNA_RV_IS_CANON(v)) v = PDNA_RV_PWC;
        PDNA_RV_STEP4(c, v);
      }
    } else {
      while (n--) PDNA_RV_STEP4(c, *w++);
    }
  } else if (canon && (len & 3u) == 0u) {
    /* Whole words, but from an unaligned pointer. This cannot happen on the cartridge
     * (rom base 0x08000000, aligned zones) and it is a MISALIGNED-HOST case only — but
     * silently dropping the canonicalisation here would make the host stamp and the
     * cartridge disagree, i.e. a false "ROM IMAGE MODIFIED" on every SD boot. Assemble
     * the words by hand instead of choosing between wrong answers. */
    uint32_t n = len >> 2;
    while (n--) {
      uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                   ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
      b += 4;
      if (PDNA_RV_IS_CANON(v)) v = PDNA_RV_PWC;
      PDNA_RV_STEP4(c, v);
    }
  } else {
    /* Ragged: no canonicalisation is possible (a canonical word cannot be split) and
     * none is needed. The grid walk only lands here on the last few bytes of an image
     * whose size is not a multiple of 4, and the stamper's byte path agrees. */
    while (len--) PDNA_RV_STEP(c, *b++);
  }
  return c;
}

uint32_t pdna_rv_crc32_zeros(const uint32_t* t, uint32_t c, uint32_t n) {
  while (n--) PDNA_RV_STEP(c, 0u);
  return c;
}

uint32_t pdna_rv_crc32(const uint32_t* t, const void* data, uint32_t len) {
  return pdna_rv_crc32_fin(
      pdna_rv_crc32_upd(t, PDNA_RV_CRC_INIT, data, len, 0));
}

/* The bus-vs-image discrimination, isolated so it can be unit-tested.
 *
 *   any attempt matched          -> MATCH (first try) or RECOVERED (a retry). The house
 *                                   pattern (icopy_verified, box_oam.c:110-121) calls
 *                                   that a success and logs the attempt count, and so
 *                                   do we: one transient bit-flip in a 64 KiB boot scan
 *                                   must never permanently disable Guy's write features.
 *   no match, attempts disagree  -> UNSTABLE. The cart returned different bytes for the
 *                                   same address: that is a BUS fault, and the answer to
 *                                   it is the timing ladder, not amputating write mode.
 *   no match, all attempts equal -> STABLE_BAD. The cartridge is consistently holding
 *                                   bytes that are not the ones we shipped. */
int pdna_rv_classify(uint32_t expect, const uint32_t* got, int n) {
  int i;
  int disagree = 0;
  if (n <= 0) return PDNA_RVC_UNSTABLE;   /* nothing measured: never call it an image fault */
  if (got[0] == expect) return PDNA_RVC_MATCH;
  for (i = 1; i < n; i++) {
    if (got[i] != got[0]) disagree = 1;
    if (got[i] == expect) return PDNA_RVC_RECOVERED;
  }
  return disagree ? PDNA_RVC_UNSTABLE : PDNA_RVC_STABLE_BAD;
}

void pdna_rv_check(const PdnaRomVerify* d, const unsigned char* rom,
                   uint32_t rom_base, uint32_t* tab, PdnaRvResult* out) {
  uint32_t n, img;
  int i, rec_stable = 0;

  if (!out) return;
  out->verdict     = PDNA_RV_UNSTAMPED;
  out->n_checked   = 0;
  out->n_bad       = 0;
  out->n_unstable  = 0;
  out->n_recovered = 0;
  out->n_skipped   = 0;
  out->first_bad   = -1;
  out->bad_addr = out->bad_flags = 0u;
  out->bad_expect = out->bad_got = out->bad_got2 = 0u;
  out->bad_tries  = 0;
  out->bad_tag[0] = out->bad_tag[1] = out->bad_tag[2] = out->bad_tag[3] = ' ';

  if (!d || !rom || !tab) return;
  if (d->magic0 != PDNA_RV_MAGIC0 || d->magic1 != PDNA_RV_MAGIC1) return;
  if (d->version != PDNA_RV_VERSION) return;
  if (d->stamped != PDNA_RV_STAMPED) return;    /* the build tool never ran */

  /* SANITY THE DESCRIPTOR BEFORE OBEYING IT. Everything below bounds reads against
   * image_bytes, so image_bytes itself has to be believable first. A magic that survived
   * with a wild size is more likely a corrupt image than a tooling bug — but we did not
   * MEASURE anything, so this reports "no opinion" loudly rather than latching the tool
   * read-only on a premise we could not test. */
  img = d->image_bytes;
  if (img == 0u || img > PDNA_RV_MAX_IMAGE) {
    out->verdict = PDNA_RV_BAD_DESC;
    return;
  }

  n = d->n_windows;
  if (n > PDNA_RV_MAX_WINDOWS) n = PDNA_RV_MAX_WINDOWS;

  pdna_rv_crc32_table(tab);

  for (i = 0; i < (int)n; i++) {
    const PdnaRvWindow* w = &d->w[i];
    /* MASK BIT 0. Anchoring a window on a function is the only way to sample .text by
     * name, and the ARM ABI makes an R_ARM_ABS32 against a THUMB function symbol carry
     * the interworking bit: measured on devkitARM 15.2.0, `nm` says 0800026c and the
     * relocated word is 0800026d. Data and linker symbols are unaffected. Masking is
     * safe for the absence test (0 & ~1 == 0) and the stamper masks identically. */
    uint32_t base = w->base_addr & ~1u, off = w->off, len = w->len;
    uint32_t addr, foff, got[PDNA_RV_TRIES];
    int tries, cls;

    if (base == 0u || len == 0u) continue;   /* anchor absent in this build, or unused */

    /* --- clamps. Not one of these may be dropped: the descriptor is IN the ROM we are
     * accusing, so a flipped bit here is exactly the input we must survive. --- */
    if (len > PDNA_RV_MAX_WINLEN) { out->n_skipped++; continue; }
    addr = base + off;
    if (addr < base)              { out->n_skipped++; continue; }  /* off overflowed */
    if (addr < rom_base)          { out->n_skipped++; continue; }
    foff = addr - rom_base;
    if (foff >= img || len > img - foff) { out->n_skipped++; continue; }

    got[0] = pdna_rv_crc32(tab, rom + foff, len);
    tries  = 1;
    if (got[0] != w->crc) {
      /* Second, third and fourth opinions from the same bytes. This is the house
       * 4-attempt pattern, and it is also the ONLY way to tell "the cart is lying" from
       * "the image is wrong" — which are different diagnoses with different remedies. */
      while (tries < PDNA_RV_TRIES) {
        got[tries] = pdna_rv_crc32(tab, rom + foff, len);
        tries++;
        if (got[tries - 1] == w->crc) break;
      }
    }
    out->n_checked++;

    cls = pdna_rv_classify(w->crc, got, tries);
    if (cls == PDNA_RVC_MATCH) continue;

    if      (cls == PDNA_RVC_RECOVERED)  out->n_recovered++;
    else if (cls == PDNA_RVC_UNSTABLE)   out->n_unstable++;
    else                                 out->n_bad++;

    /* Keep the details of the first non-clean window, but let the FIRST stable mismatch
     * displace a merely-flaky one: a stable mismatch is the finding with an actionable
     * remedy, so it is the one that belongs in the log's detail line. */
    if (out->first_bad < 0 || (cls == PDNA_RVC_STABLE_BAD && !rec_stable)) {
      rec_stable      = (cls == PDNA_RVC_STABLE_BAD);
      out->first_bad  = i;
      out->bad_addr   = addr;
      out->bad_flags  = w->flags;
      out->bad_expect = w->crc;
      out->bad_got    = got[0];
      out->bad_got2   = got[tries - 1];
      out->bad_tries  = tries;
      out->bad_tag[0] = w->tag[0]; out->bad_tag[1] = w->tag[1];
      out->bad_tag[2] = w->tag[2]; out->bad_tag[3] = w->tag[3];
    }
  }

  if (out->n_checked == 0)      out->verdict = PDNA_RV_NOWINDOWS;
  else if (out->n_bad > 0)      out->verdict = PDNA_RV_BAD_IMAGE;
  else if (out->n_unstable > 0) out->verdict = PDNA_RV_BAD_BUS;
  else                          out->verdict = PDNA_RV_OK;
}

/* ================= the full-image region grid ==================================
 *
 * The windows above sample 0.53% of the image and can only ever be EVIDENCE. This is
 * the proof: every byte from 0x08000000 to the end of the stamped image, in fixed
 * regions, each against its own stamped CRC32, so the answer carries an ADDRESS.
 *
 * Everything here is a pure function of (descriptor, rom bytes) and is driven one
 * region at a time by source/pdna_romfull.c, which paints a cell and polls the keys
 * between regions. Nothing here allocates, blocks, or knows what a screen is.
 *
 * THE DESCRIPTOR IS NOT TRUSTED. It lives in the very image under suspicion, so
 * pdna_rv_grid_init re-derives n_regions from image_bytes and refuses the whole grid on
 * any disagreement, and the zone list is checked for sortedness, alignment and overlap
 * before the walk relies on all three. A flipped bit must produce "no opinion", never a
 * wild read or a false accusation.
 */

int pdna_rv_grid_init(const PdnaRomVerify* d, PdnaRvGrid* g) {
  uint32_t rb, img, want, k, prev_end = 0u;

  if (!g) return 0;
  g->region_bytes = 0u;
  g->n_regions = g->n_done = 0;
  g->n_ok = g->n_recovered = g->n_unstable = g->n_bad = g->n_skipped = 0;
  g->first_bad = -1;
  g->first_bad_state = PDNA_RVG_PENDING;
  g->first_bad_off = g->first_bad_exp = g->first_bad_got = 0u;
  g->bytes_verified = g->bytes_zoned = 0u;

  if (!d) return 0;
  if (d->magic0 != PDNA_RV_MAGIC0 || d->magic1 != PDNA_RV_MAGIC1) return 0;
  if (d->version != PDNA_RV_VERSION) return 0;
  if (d->stamped != PDNA_RV_STAMPED) return 0;

  img = d->image_bytes;
  if (img == 0u || img > PDNA_RV_MAX_IMAGE) return 0;
  if (d->region_shift < PDNA_RV_MIN_RSHIFT || d->region_shift > PDNA_RV_MAX_RSHIFT)
    return 0;                                  /* 0 == this image carries no grid */

  rb   = 1u << d->region_shift;
  want = (img + rb - 1u) / rb;                 /* re-derived, never believed */
  if (want == 0u || want > PDNA_RV_MAX_REGIONS) return 0;
  if (d->n_regions != want) return 0;

  if (d->n_zones > PDNA_RV_MAX_ZONES) return 0;
  for (k = 0; k < d->n_zones; k++) {
    uint32_t z0 = d->z[k].off, zl = d->z[k].len, z1;
    if (zl == 0u) return 0;                    /* the stamper never emits an empty zone */
    if ((z0 | zl) & 3u) return 0;              /* alignment is what keeps canon words whole */
    z1 = z0 + zl;
    if (z1 < z0 || z1 > img) return 0;
    if (k > 0 && z0 < prev_end) return 0;      /* sorted AND non-overlapping */
    prev_end = z1;
  }

  g->region_bytes = rb;
  g->n_regions    = (int32_t)want;
  return 1;
}

int pdna_rv_region_excluded(const PdnaRomVerify* d, int32_t i) {
  if (!d || i < 0 || (uint32_t)i >= PDNA_RV_MAX_REGIONS) return 1;
  return (int)((d->excl[(uint32_t)i >> 5] >> ((uint32_t)i & 31u)) & 1u);
}

uint32_t pdna_rv_region_crc(const PdnaRomVerify* d, const unsigned char* rom,
                            int32_t i, const uint32_t* tab, uint32_t* out_bytes) {
  uint32_t rb, r0, r1, p, crc = PDNA_RV_CRC_INIT, real = 0u, k;

  if (out_bytes) *out_bytes = 0u;
  if (!d || !rom || !tab || i < 0) return 0u;
  rb = 1u << d->region_shift;
  r0 = (uint32_t)i * rb;
  if (r0 >= d->image_bytes) return 0u;
  r1 = r0 + rb;
  if (r1 > d->image_bytes) r1 = d->image_bytes;

  p = r0;
  for (k = 0; k < d->n_zones && k < PDNA_RV_MAX_ZONES; k++) {
    uint32_t z0 = d->z[k].off, z1 = z0 + d->z[k].len, e;
    if (z1 <= p)  continue;                    /* zone is behind us  */
    if (z0 >= r1) break;                       /* zones are sorted   */
    if (z0 > p) {                              /* real bytes before the zone */
      crc = pdna_rv_crc32_upd(tab, crc, rom + p, z0 - p, 1);
      real += z0 - p;
      p = z0;
    }
    e = (z1 < r1) ? z1 : r1;                   /* ...then the zone itself, as zeroes */
    if (e > p) { crc = pdna_rv_crc32_zeros(tab, crc, e - p); p = e; }
  }
  if (p < r1) {
    crc = pdna_rv_crc32_upd(tab, crc, rom + p, r1 - p, 1);
    real += r1 - p;
  }

  if (out_bytes) *out_bytes = real;
  return pdna_rv_crc32_fin(crc);
}

int pdna_rv_region_check(const PdnaRomVerify* d, const unsigned char* rom,
                         int32_t i, const uint32_t* tab, int tries, PdnaRvGrid* g) {
  uint32_t got[PDNA_RV_GRID_TRIES], nbytes = 0u, span, r0;
  int n = 1, cls, state;

  if (!d || !rom || !tab || !g) return PDNA_RVG_SKIP;
  if (i < 0 || i >= g->n_regions) return PDNA_RVG_SKIP;
  if (tries < 1) tries = 1;
  if (tries > PDNA_RV_GRID_TRIES) tries = PDNA_RV_GRID_TRIES;

  r0   = (uint32_t)i * g->region_bytes;
  span = (d->image_bytes - r0 < g->region_bytes) ? (d->image_bytes - r0)
                                                 : g->region_bytes;
  g->n_done++;

  if (pdna_rv_region_excluded(d, i)) {
    /* Nothing here is verifiable — it is all loader drop zone / descriptor. Counted as
     * blind, drawn as SKIP. Never drawn green: a cell that always passes because it
     * compares nothing is worse than an honest gap. */
    g->n_skipped++;
    g->bytes_zoned += span;
    return PDNA_RVG_SKIP;
  }

  got[0] = pdna_rv_region_crc(d, rom, i, tab, &nbytes);
  g->bytes_verified += nbytes;
  g->bytes_zoned    += span - nbytes;
  if (got[0] != d->rcrc[i]) {
    while (n < tries) {
      got[n] = pdna_rv_region_crc(d, rom, i, tab, 0);
      n++;
      if (got[n - 1] == d->rcrc[i]) break;
    }
  }

  cls = pdna_rv_classify(d->rcrc[i], got, n);
  switch (cls) {
    case PDNA_RVC_MATCH:     g->n_ok++;        state = PDNA_RVG_OK;       break;
    case PDNA_RVC_RECOVERED: g->n_recovered++; state = PDNA_RVG_RECOVER;  break;
    case PDNA_RVC_UNSTABLE:  g->n_unstable++;  state = PDNA_RVG_UNSTABLE; break;
    default:                 g->n_bad++;       state = PDNA_RVG_BAD;      break;
  }

  /* Keep the FIRST offending region, but let a stable mismatch displace a merely-flaky
   * one — same rule as the windows: the stable finding is the one with a remedy. */
  if (state == PDNA_RVG_BAD || state == PDNA_RVG_UNSTABLE) {
    if (g->first_bad < 0 ||
        (state == PDNA_RVG_BAD && g->first_bad_state != PDNA_RVG_BAD)) {
      g->first_bad       = i;
      g->first_bad_state = state;
      g->first_bad_off   = r0;
      g->first_bad_exp   = d->rcrc[i];
      g->first_bad_got   = got[0];
    }
  }
  return state;
}
