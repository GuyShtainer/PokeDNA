/* BACKLOG #103 -- map_render.c's LZ10 core (mr_lz77_w / mr_lz77_x / mr_hash_span): the chunk-hashed,
 * fast-path decoder against the ORIGINAL byte-at-a-time decoder, kept below verbatim as the reference.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_lz77_diff_test.c source/map_render.c source/rom_map.c \
 *      -o /tmp/hlz77d && /tmp/hlz77d
 *
 * What it proves, per case (seeded random valid streams from a small LZ10 encoder, random
 * garbage streams behind a valid header, truncated streams, an injected read failure, and window
 * sizes 0 (the 64 B stack window) / 5 / 17 / 24 / 37 / 64 / 100 / 256 / 2048): the same return value, the same consumed
 * span, the same FNV hash, the same bytes in the WHOLE destination (output + the window tail the
 * chunks were read into), and the same SEQUENCE of (offset, length) window reads. Equal read
 * sequences are what make the speed-up safe on the SD path: the new decoder asks the card for
 * exactly what the old one did, in the same order.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "map_render.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- a synthetic ROM image + a read callback that logs every call -------------------- */
#define IMG_MAX 70000u
static uint8_t g_img[IMG_MAX];
static uint32_t g_rd_n, g_rd_off[4096], g_rd_len[4096];
static int g_fail_at = -1;                       /* the Nth read call fails (-1 = never) */
static bool rd(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  if (g_rd_n < 4096) { g_rd_off[g_rd_n] = off; g_rd_len[g_rd_n] = len; }
  if ((int)g_rd_n == g_fail_at) { g_rd_n++; return false; }
  g_rd_n++;
  memcpy(dst, g_img + off, len);
  return true;
}
static RomCtx make_ctx(uint32_t size) {
  RomCtx c; memset(&c, 0, sizeof c);
  c.read = rd; c.size = size;
  return c;
}

/* ---- the reference: the pre-#103 decoder, verbatim ----------------------------------- */
#define MR_FNV_OFFSET 2166136261u
#define MR_FNV_PRIME  16777619u
static uint32_t ref_lz77_run(const RomCtx* rom, uint32_t addr, uint8_t* dst, uint32_t dst_cap,
                         uint8_t* win, uint32_t win_bytes,
                         uint32_t* out_consumed, uint32_t* out_hash) {
  uint8_t h[4];
  if (!rom_read_at(rom, addr, h, 4)) return 0;
  if (h[0] != 0x10) return 0;                     /* LZ10 only */
  uint32_t size = (uint32_t)h[1] | ((uint32_t)h[2] << 8) | ((uint32_t)h[3] << 16);
  if (!size || !dst || size > dst_cap) return 0;
  if (win_bytes && !win) return 0;

  int track = (out_consumed || out_hash) ? 1 : 0;
  uint32_t hash = MR_FNV_OFFSET;
  uint32_t consumed = 4;
  if (track) for (int i = 0; i < 4; i++) { hash ^= h[i]; hash *= MR_FNV_PRIME; }

  uint8_t stack_buf[64];
  uint8_t* buf = win_bytes ? win : stack_buf;
  uint32_t buf_cap = win_bytes ? win_bytes : (uint32_t)sizeof stack_buf;
  uint32_t buf_at = 0, buf_len = 0;               /* buf covers [buf_at, buf_at+buf_len) */
  uint32_t src = addr + 4;
  uint32_t out = 0;
  /* LZ10 all-literal upper bound on the compressed span: 4 header bytes + `size`
   * literal bytes + one flag byte per (up to) 8 literals, PLUS one more byte
   * (BACKLOG #103 F4, review-opus LOW): a stream whose FINAL token is a
   * back-reference clamped by `len > size - out` (this decoder's own clamp,
   * a few lines below) still spends its full 2-byte back-reference token in
   * the compressed stream even though it produces fewer than `len` output
   * bytes -- the plain `size / 8` flag-byte count alone under-covers that
   * token's own bytes by one in the worst case. An over-read past this bound
   * can never be needed to decode a well-formed stream. */
  uint32_t span_end = addr + 4 + size + 1u + (size + 7u) / 8u;
  uint32_t img_end = ROM_BASE + rom->size;

  /* one byte of compressed input, buffered */
  #define NEXT(v) do {                                                        \
      if (buf_at >= buf_len) {                                                \
        uint32_t remain_span = (src < span_end) ? (span_end - src) : 0u;      \
        uint32_t remain_img  = (src < img_end)  ? (img_end - src)  : 0u;      \
        uint32_t want = buf_cap;                                              \
        if (remain_span < want) want = remain_span;                           \
        if (remain_img  < want) want = remain_img;                            \
        if (!want) return 0;                                                  \
        if (!rom_read_at(rom, src, buf, want)) return 0;                      \
        buf_len = want; src += want; buf_at = 0;                              \
      }                                                                       \
      (v) = buf[buf_at++];                                                    \
      if (track) { hash ^= (v); hash *= MR_FNV_PRIME; consumed++; }           \
    } while (0)

  while (out < size) {
    uint8_t flags;
    NEXT(flags);
    for (int bit = 0; bit < 8 && out < size; bit++) {
      if (flags & (0x80 >> bit)) {
        uint8_t b1, b2;
        NEXT(b1); NEXT(b2);
        uint32_t len  = (uint32_t)(b1 >> 4) + 3;
        uint32_t disp = ((uint32_t)(b1 & 0x0F) << 8 | b2) + 1;
        if (disp > out) return 0;                 /* reference before the start */
        if (len > size - out) len = size - out;
        uint32_t from = out - disp;
        /* byte-wise on purpose: overlapping runs are legal and common */
        for (uint32_t k = 0; k < len; k++) dst[out + k] = dst[from + k];
        out += len;
      } else {
        uint8_t lit;
        NEXT(lit);
        dst[out++] = lit;
      }
    }
  }
  #undef NEXT
  if (out_consumed) *out_consumed = consumed;
  if (out_hash) *out_hash = hash;
  return out;
}

/* ---- a tiny LZ10 encoder: random literal/reference decisions, always-valid output ---- */
static uint32_t seed = 0xC0FFEE11u;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

static uint32_t encode(uint8_t* out, const uint8_t* in, uint32_t n, uint32_t refpct) {
  uint32_t o = 4, i = 0;
  out[0] = 0x10; out[1] = (uint8_t)n; out[2] = (uint8_t)(n >> 8); out[3] = (uint8_t)(n >> 16);
  while (i < n) {
    uint32_t fpos = o++; uint8_t flags = 0;
    for (int bit = 0; bit < 8 && i < n; bit++) {
      uint32_t best = 0, bd = 0;
      if (i > 0 && (rnd() % 100u) < refpct) {
        uint32_t maxd = i < 4096 ? i : 4096;
        uint32_t d = 1 + rnd() % maxd;
        uint32_t l = 0;
        while (l < 18 && i + l < n && in[i + l - d] == in[i + l]) l++;
        if (l >= 3) { best = l; bd = d; }
      }
      if (best) {
        flags |= (uint8_t)(0x80 >> bit);
        out[o++] = (uint8_t)(((best - 3) << 4) | ((bd - 1) >> 8));
        out[o++] = (uint8_t)((bd - 1) & 0xFF);
        i += best;
      } else { out[o++] = in[i++]; }
    }
    out[fpos] = flags;
  }
  return o;
}

static int run_case(uint32_t addr_off, uint32_t imgsize, uint32_t size, uint32_t win_bytes, int track,
                    int fail_at, int ref_first) {
  RomCtx rc = make_ctx(imgsize);
  static uint8_t d1[8192 + 4096], d2[8192 + 4096];
  uint32_t dcap = size + win_bytes;
  uint8_t* w1 = win_bytes ? d1 + size : 0; uint8_t* w2 = win_bytes ? d2 + size : 0;
  uint32_t c1 = 0, h1 = 0, c2 = 0, h2 = 0, r1, r2;
  memset(d1, 0xA5, sizeof d1); memset(d2, 0xA5, sizeof d2);
  (void)ref_first;
  g_rd_n = 0; g_fail_at = fail_at;
  r1 = ref_lz77_run(&rc, ROM_BASE + addr_off, d1, dcap, w1, win_bytes, track ? &c1 : 0, track ? &h1 : 0);
  uint32_t n1 = g_rd_n; static uint32_t o1[4096], l1[4096];
  memcpy(o1, g_rd_off, sizeof o1); memcpy(l1, g_rd_len, sizeof l1);
  g_rd_n = 0; g_fail_at = fail_at;
  r2 = track ? mr_lz77_x(&rc, ROM_BASE + addr_off, d2, dcap, w2, win_bytes, &c2, &h2)
             : mr_lz77_w(&rc, ROM_BASE + addr_off, d2, dcap, w2, win_bytes);
  uint32_t n2 = g_rd_n;
  int bad = 0;
  if (r1 != r2) bad = 1;
  if (track && r1 && (c1 != c2 || h1 != h2)) bad = 2;     /* mr_lz77_x zeroes both on failure */
  if (memcmp(d1, d2, sizeof d1) != 0) bad = 3;
  if (n1 != n2 || memcmp(o1, g_rd_off, (n1 < 4096 ? n1 : 4096) * 4) != 0 ||
      memcmp(l1, g_rd_len, (n1 < 4096 ? n1 : 4096) * 4) != 0) bad = 4;
  return bad;
}

int main(void) {
  static const uint32_t wins[] = { 0, 5, 17, 24, 37, 64, 100, 256, 2048 };
  static uint8_t plain[8192];
  int cases = 0, bad[5] = { 0 }, nonzero_ret = 0, fail_cases = 0;
  for (int iter = 0; iter < 8000; iter++) {
    uint32_t n = 1 + rnd() % 6000u;
    if (iter % 11 == 0) n = 1 + rnd() % 40u;                        /* tiny outputs */
    const int comp = (iter % 3 == 0);                               /* near-constant data: almost every token a reference */
    for (uint32_t i = 0; i < n; i++)
      plain[i] = comp ? (uint8_t)(i % 97u == 0 ? rnd() : 0) : (uint8_t)((rnd() % 3u) ? (rnd() % 7u) : rnd());
    uint32_t addr_off = rnd() % 64u;
    memset(g_img, 0x77, sizeof g_img);
    uint32_t elen = encode(g_img + addr_off, plain, n, comp ? 100u : 20u + rnd() % 70u);
    uint32_t imgsize = addr_off + elen + (rnd() % 8u);
    int variant = iter % 5;
    if (variant == 1) {                                             /* garbage behind a valid header */
      for (uint32_t i = 4; i < elen; i++) g_img[addr_off + i] = (uint8_t)rnd();
    } else if (variant == 2) {                                      /* truncated image */
      imgsize = addr_off + 4 + (elen > 8 ? rnd() % (elen - 4) : 0);
    } else if (variant == 3) {                                      /* one flipped bit */
      g_img[addr_off + 4 + rnd() % (elen - 4)] ^= (uint8_t)(1u << (rnd() % 8u));
    }
    uint32_t hdr_n = n;
    if (variant == 4 && n > 8) {                                    /* declared size < encoded size: the final token clamps */
      hdr_n = n / 2 + rnd() % (n / 2);
      g_img[addr_off + 1] = (uint8_t)hdr_n; g_img[addr_off + 2] = (uint8_t)(hdr_n >> 8); g_img[addr_off + 3] = (uint8_t)(hdr_n >> 16);
    }
    n = hdr_n;
    int fail_at = (iter % 7 == 0) ? (int)(rnd() % 6u) : -1;         /* an injected read failure */
    uint32_t win = wins[rnd() % 9u];
    if (n + win > 8192u + 4096u) win = 0;
    int track = (int)(rnd() & 1u);
    int b = run_case(addr_off, imgsize, n, win, track, fail_at, 0);
    cases++;
    if (fail_at >= 0) fail_cases++;
    if (b) bad[b]++;
    else {
      RomCtx rc = make_ctx(imgsize); static uint8_t dd[8192 + 4096]; g_rd_n = 0; g_fail_at = -1;
      if (mr_lz77_w(&rc, ROM_BASE + addr_off, dd, n + win, win ? dd + n : 0, win) == n) nonzero_ret++;
    }
  }
  CHK(bad[1] == 0, "return value differs from the reference in %d of %d cases", bad[1], cases);
  CHK(bad[2] == 0, "consumed span / hash differs in %d of %d cases", bad[2], cases);
  CHK(bad[3] == 0, "destination bytes differ in %d of %d cases", bad[3], cases);
  CHK(bad[4] == 0, "window read sequence differs in %d of %d cases", bad[4], cases);
  CHK(nonzero_ret > 200, "only %d valid-stream cases decoded fully: the generator is not exercising the success path", nonzero_ret);
  CHK(fail_cases > 100, "only %d injected-read-failure cases", fail_cases);
  printf("%d cases (%d decoded fully, %d with an injected read failure), %d checks, %d fail%s\n",
         cases, nonzero_ret, fail_cases, checks, fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
