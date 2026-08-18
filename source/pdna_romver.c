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

/* The aligned path reads the cartridge 32 bits at a time. That is not cosmetic: on the
 * GBA bus a byte read costs a full non-sequential access, so a byte loop pays several
 * cycles per byte purely to fetch, where one word read serves four. The stamper
 * guarantees base|off|len are all 4-aligned, so the fast path is the only one that ever
 * runs on the cartridge; the byte path exists for the host test (and for a corrupt
 * descriptor that somehow got past the clamps). */
uint32_t pdna_rv_crc32(const uint32_t* t, const void* data, uint32_t len) {
  const unsigned char* b = (const unsigned char*)data;
  uint32_t c = 0xFFFFFFFFu;

  if (((((uintptr_t)b) | (uintptr_t)len) & 3u) == 0u) {
    const uint32_t* w = (const uint32_t*)(const void*)b;
    uint32_t n = len >> 2;
    while (n--) {
      uint32_t v = *w++;
      PDNA_RV_STEP(c, v);       v >>= 8;
      PDNA_RV_STEP(c, v);       v >>= 8;
      PDNA_RV_STEP(c, v);       v >>= 8;
      PDNA_RV_STEP(c, v);
    }
  } else {
    while (len--) PDNA_RV_STEP(c, *b++);
  }
  return c ^ 0xFFFFFFFFu;
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
