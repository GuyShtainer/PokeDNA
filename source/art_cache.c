/* art_cache.c — see art_cache.h. Pure C, no tonc/FatFs: dual-compiles on the host
 * (tests/host_artcache_test.c) and on the GBA build unchanged. */
#include "art_cache.h"

#include <string.h>

/* Magic is initialised element-by-element, never as a string literal — the same
 * reasoning fused_rom.h's PdnaFuseRec.magic documents: a string literal lands in the
 * constant pool where it can be merged/duplicated, and this bit pattern should have
 * exactly one meaning in the image (it is what tells a parser "this is a PokeDNA
 * art.idx" at all). It does not need to appear exactly once the way the fuse magic
 * does (nothing patches art.idx post-link), but writing it the same way costs
 * nothing and keeps the convention uniform across the codebase. */
static const char k_magic[8] = { 'P', 'D', 'N', 'A', 'A', 'R', 'T', '1' };

static const char* const k_kind_names[ART_KIND_COUNT] = {
  "/PokeDNA/art/icons.bin",     "/PokeDNA/art/wallpaper.bin",
  "/PokeDNA/art/glove.bin",     "/PokeDNA/art/card.bin",
  "/PokeDNA/art/bag.bin",       "/PokeDNA/art/pokeblock.bin",
  "/PokeDNA/art/items.bin",     "/PokeDNA/art/types.bin",
};

const char* art_kind_filename(ArtKind k) {
  if ((unsigned)k >= ART_KIND_COUNT) return 0;
  return k_kind_names[k];
}

/* ---- little-endian packers, matching rom_mon.c's rd32le style ------------------ */
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

uint32_t art_fnv1a(uint32_t seed, const void* data, uint32_t len) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t h = seed;
  for (uint32_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 0x01000193u; /* FNV prime */
  }
  return h;
}

#define ART_ROM_FNV_WINDOWS 16u
#define ART_ROM_FNV_WINDOW_BYTES 4096u

bool art_rom_fnv(RomReadFn read, void* ctx, uint32_t rom_size, uint32_t* out) {
  if (!read || !out || rom_size < ART_ROM_FNV_WINDOW_BYTES) return false;
  uint32_t h = ART_FNV1A_INIT;
  uint8_t buf[256]; /* read each 4 KiB window in small pieces — no big stack local */
  /* Evenly spaced anchors across the image, the last window pulled back so it never
   * runs past EOF. Deterministic and independent of the caller's I/O chunking. */
  for (uint32_t w = 0; w < ART_ROM_FNV_WINDOWS; w++) {
    uint32_t anchor = (uint32_t)(((uint64_t)rom_size * w) / ART_ROM_FNV_WINDOWS);
    if (anchor + ART_ROM_FNV_WINDOW_BYTES > rom_size)
      anchor = rom_size - ART_ROM_FNV_WINDOW_BYTES;
    anchor &= ~3u; /* 4-aligned, matches every other ROM read in this codebase */
    uint32_t done = 0;
    while (done < ART_ROM_FNV_WINDOW_BYTES) {
      uint32_t n = ART_ROM_FNV_WINDOW_BYTES - done;
      if (n > sizeof buf) n = sizeof buf;
      if (!read(ctx, anchor + done, buf, n)) return false;
      h = art_fnv1a(h, buf, n);
      done += n;
    }
  }
  *out = h;
  return true;
}

void art_idx_head_write(const ArtIdxHead* h, uint8_t out[ART_IDX_HEAD_BYTES]) {
  memcpy(out, k_magic, 8);
  wr16(out + 8, h->format);
  wr16(out + 10, h->builder);
  memcpy(out + 12, h->rom_code, 4);
  out[16] = h->rom_rev;
  out[17] = h->rom_kind;
  out[18] = h->kinds;
  out[19] = 0; /* pad */
  wr32(out + 20, h->rom_bytes);
  wr32(out + 24, h->rom_fnv);
  wr32(out + 28, 0); /* reserved — see the header comment on the 28-vs-32 note */
}

void art_idx_head_read(const uint8_t buf[ART_IDX_HEAD_BYTES], ArtIdxHead* out) {
  memset(out, 0, sizeof *out);
  out->format = rd16(buf + 8);
  out->builder = rd16(buf + 10);
  memcpy(out->rom_code, buf + 12, 4);
  out->rom_rev = buf[16];
  out->rom_kind = buf[17];
  out->kinds = buf[18];
  out->rom_bytes = rd32(buf + 20);
  out->rom_fnv = rd32(buf + 24);
}

void art_idx_kind_write(const ArtIdxKindRow* r, uint8_t out[ART_IDX_KIND_BYTES]) {
  out[0] = r->kind; out[1] = 0; out[2] = 0; out[3] = 0;
  wr32(out + 4, r->bytes);
  wr32(out + 8, r->fnv);
  wr32(out + 12, r->entries);
}

void art_idx_kind_read(const uint8_t buf[ART_IDX_KIND_BYTES], ArtIdxKindRow* out) {
  out->kind = buf[0];
  out->bytes = rd32(buf + 4);
  out->fnv = rd32(buf + 8);
  out->entries = rd32(buf + 12);
}

ArtIdxParseStatus art_idx_parse(const uint8_t* buf, uint32_t len, ArtIdxHead* out_head,
                                 ArtIdxKindRow out_rows[ART_KIND_COUNT], int* out_nrows) {
  if (out_nrows) *out_nrows = 0;
  if (!buf || len == 0) return ART_IDX_ABSENT;
  if (len < ART_IDX_HEAD_BYTES) return ART_IDX_TRUNCATED;
  if (memcmp(buf, k_magic, 8) != 0) return ART_IDX_BAD_MAGIC;

  ArtIdxHead h;
  art_idx_head_read(buf, &h);
  if (h.format != ART_IDX_FORMAT_V1) return ART_IDX_BAD_FORMAT;

  int n = 0;
  for (int k = 0; k < ART_KIND_COUNT; k++) if (h.kinds & (1u << k)) n++;
  uint32_t need = ART_IDX_HEAD_BYTES + (uint32_t)n * ART_IDX_KIND_BYTES;
  if (len < need) return ART_IDX_TRUNCATED; /* THE invariant: a cut-off write refuses */

  if (out_head) *out_head = h;
  if (out_rows) {
    int ri = 0;
    for (int k = 0; k < ART_KIND_COUNT; k++) {
      if (!(h.kinds & (1u << k))) continue;
      art_idx_kind_read(buf + ART_IDX_HEAD_BYTES + (uint32_t)ri * ART_IDX_KIND_BYTES,
                         &out_rows[ri]);
      ri++;
    }
  }
  if (out_nrows) *out_nrows = n;
  return ART_IDX_OK;
}

bool art_idx_matches_rom(const ArtIdxHead* h, const RomCtx* rc, uint32_t rom_fnv) {
  if (!h || !rc) return false;
  if (memcmp(h->rom_code, rc->code, 4) != 0) return false;
  if (h->rom_rev != rc->version) return false;
  if (h->rom_kind != (uint8_t)rc->kind) return false;
  if (h->rom_bytes != rc->size) return false;
  if (h->rom_fnv != rom_fnv) return false;
  return true;
}

const ArtIdxKindRow* art_idx_find(const ArtIdxKindRow rows[ART_KIND_COUNT], int nrows,
                                   ArtKind k) {
  if (!rows) return 0;
  for (int i = 0; i < nrows; i++) if (rows[i].kind == (uint8_t)k) return &rows[i];
  return 0;
}

bool art_fnv_of_stream(ArtReadFn read, void* ctx, uint32_t len, uint8_t* scratch,
                        uint32_t chunk, uint32_t* out) {
  if (!read || !scratch || !chunk || !out) return false;
  uint32_t h = ART_FNV1A_INIT, off = 0;
  while (off < len) {
    uint32_t n = len - off; if (n > chunk) n = chunk;
    if (!read(ctx, off, scratch, n)) return false;
    h = art_fnv1a(h, scratch, n);
    off += n;
  }
  *out = h;
  return true;
}
