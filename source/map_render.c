/*
 * Gen-3 overworld renderer core. See map_render.h for the design and for why the
 * on-GBA path is Mode 0 (measured: a map's tiles cannot fit the EWRAM arena).
 *
 * Pure C — host-tested against an independent Python reference renderer run over a
 * real retail ROM (tools/gen_render_truth.py -> docs/analysis-2026-07-29/render-*.raw).
 */
#include <string.h>
#include "map_render.h"

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t mr_lz77_size(const RomCtx* rom, uint32_t addr) {
  uint8_t h[4];
  if (!rom_read_at(rom, addr, h, 4)) return 0;
  if (h[0] != 0x10) return 0;                     /* LZ10 only */
  return (uint32_t)h[1] | ((uint32_t)h[2] << 8) | ((uint32_t)h[3] << 16);
}

/* Streaming LZ77 (LZ10). Pulls the compressed bytes through a window so a 16 MiB
 * ROM on the SD is never fully resident, and hard-caps every write to dst. See
 * map_render.h for `win`/`win_bytes`: 0,0 gets the original fixed-64-B stack
 * window (that is exactly what mr_lz77() below passes). */
uint32_t mr_lz77_w(const RomCtx* rom, uint32_t addr, uint8_t* dst, uint32_t dst_cap,
                   uint8_t* win, uint32_t win_bytes) {
  uint32_t size = mr_lz77_size(rom, addr);
  if (!size || !dst || size > dst_cap) return 0;
  if (win_bytes && !win) return 0;

  uint8_t stack_buf[64];
  uint8_t* buf = win_bytes ? win : stack_buf;
  uint32_t buf_cap = win_bytes ? win_bytes : (uint32_t)sizeof stack_buf;
  uint32_t buf_at = 0, buf_len = 0;               /* buf covers [buf_at, buf_at+buf_len) */
  uint32_t src = addr + 4;
  uint32_t out = 0;
  /* LZ10 all-literal upper bound on the compressed span: 4 header bytes + `size`
   * literal bytes + one flag byte per (up to) 8 literals. An over-read past this
   * can never be needed to decode a well-formed stream. */
  uint32_t span_end = addr + 4 + size + (size + 7u) / 8u;
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
  return out;
}

uint32_t mr_lz77(const RomCtx* rom, uint32_t addr, uint8_t* dst, uint32_t dst_cap) {
  return mr_lz77_w(rom, addr, dst, dst_cap, 0, 0);
}

uint32_t mr_metatile_table_bytes(const RomCtx* rom, uint32_t tileset_addr) {
  RomTileset ts;
  if (!rom_tileset(rom, tileset_addr, &ts)) return 0;
  /* The attribute array immediately follows the metatile array in ROM, so the gap is
   * the metatile table's size (verified: 0x83980F0 - 0x83960F0 = 8192 = 512 * 16). */
  if (!ts.metatiles || !ts.attributes || ts.attributes <= ts.metatiles) return 0;
  uint32_t n = ts.attributes - ts.metatiles;
  return (n > 1024u * 16u) ? 0 : n;               /* implausible -> unknown */
}

bool mr_init(MapRender* mr, const RomCtx* rom, const RomLayout* lay) {
  if (!mr || !rom || !lay) return false;
  memset(mr, 0, sizeof *mr);
  mr->rom = rom;
  mr->lay = *lay;
  mr->split_mt   = rom->fmt.metatiles_primary;    /* 512 RSE / 640 FRLG */
  mr->split_tile = rom->fmt.tiles_primary;
  mr->split_pal  = rom->fmt.pals_primary;         /* 6 RSE / 7 FRLG     */

  RomTileset p, s;
  if (!rom_tileset(rom, lay->tileset_primary, &p)) return false;
  mr->prim_mt_addr = p.metatiles;
  if (p.palettes && !rom_read_at(rom, p.palettes, mr->prim_pal, sizeof mr->prim_pal))
    return false;

  if (lay->tileset_secondary && rom_tileset(rom, lay->tileset_secondary, &s)) {
    mr->sec_mt_addr = s.metatiles;
    if (s.palettes) rom_read_at(rom, s.palettes, mr->sec_pal, sizeof mr->sec_pal);
  }
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  /* Palettes were read as raw bytes; swap on a big-endian host so the host test is
   * meaningful there too. Every real target is little-endian like the ROM. */
  for (int i = 0; i < 16; i++)
    for (int j = 0; j < 16; j++) {
      uint8_t* a = (uint8_t*)&mr->prim_pal[i][j]; mr->prim_pal[i][j] = (uint16_t)(a[0] | (a[1] << 8));
      uint8_t* b = (uint8_t*)&mr->sec_pal[i][j];  mr->sec_pal[i][j]  = (uint16_t)(b[0] | (b[1] << 8));
    }
#endif
  return true;
}

void mr_set_tiles(MapRender* mr, const uint8_t* prim, uint32_t prim_len,
                                 const uint8_t* sec,  uint32_t sec_len) {
  if (!mr) return;
  mr->prim_tiles = prim; mr->prim_tiles_len = prim_len;
  mr->sec_tiles  = sec;  mr->sec_tiles_len  = sec_len;
}

void mr_set_metatiles(MapRender* mr, const uint8_t* prim, uint32_t prim_len,
                                     const uint8_t* sec,  uint32_t sec_len) {
  if (!mr) return;
  mr->prim_mt = prim; mr->prim_mt_len = prim_len;
  mr->sec_mt  = sec;  mr->sec_mt_len  = sec_len;
}

bool mr_metatile_entries(const MapRender* mr, uint16_t mid, uint16_t out[8]) {
  if (!mr || !out) return false;
  const uint8_t* cache; uint32_t cache_len, addr, idx;
  if (mid < mr->split_mt) {
    cache = mr->prim_mt; cache_len = mr->prim_mt_len; addr = mr->prim_mt_addr; idx = mid;
  } else {
    cache = mr->sec_mt;  cache_len = mr->sec_mt_len;  addr = mr->sec_mt_addr;
    idx = (uint32_t)(mid - mr->split_mt);
  }
  uint32_t off = idx * 16u;
  if (cache && off + 16u <= cache_len) {
    for (int i = 0; i < 8; i++) out[i] = rd16(cache + off + i * 2);
    return true;
  }
  /* A cache was supplied but the id fell past its end. On the GBA the ROM fallback below
   * would issue an SD read from INSIDE the render loop with the display on — a hard-rule-1
   * violation (the cart window vanishes mid-transfer). Refuse instead; the caller draws
   * nothing and counts the miss. MEASURED: 0 maps in Emerald/Ruby/FireRed reach here, and
   * mgfx_load always caches both tables, so this is unreachable by construction rather
   * than by luck. Host tests pass NULL caches and still get the ROM path below. */
  if (cache) return false;
  if (!addr) return false;
  uint8_t b[16];
  if (!rom_read_at(mr->rom, addr + off, b, 16)) return false;
  for (int i = 0; i < 8; i++) out[i] = rd16(b + i * 2);
  return true;
}

/* 32 bytes of 4bpp char data for a tile id, or NULL if out of range. */
static const uint8_t* tile_bytes(const MapRender* mr, uint16_t tid) {
  const uint8_t* src; uint32_t len, idx;
  if (tid < mr->split_tile) { src = mr->prim_tiles; len = mr->prim_tiles_len; idx = tid; }
  else { src = mr->sec_tiles; len = mr->sec_tiles_len; idx = (uint32_t)(tid - mr->split_tile); }
  if (!src) return 0;
  uint32_t off = idx * 32u;
  return (off + 32u <= len) ? src + off : 0;
}

/* THE PALETTE TRAP: slots below the split come from the primary tileset; slots at or
 * above it are read from the SECONDARY tileset's own 16-palette array AT THE SAME
 * INDEX — not at (index - split). Getting this wrong yields obviously wrong colours. */
static const uint16_t* pal_of(const MapRender* mr, uint8_t slot) {
  return (slot < mr->split_pal) ? mr->prim_pal[slot & 15] : mr->sec_pal[slot & 15];
}

/* Draw one 8x8 tile from a tilemap entry. `transparent` skips colour index 0. */
static void draw_tile(const MapRender* mr, uint16_t entry, uint16_t* dst, int stride,
                      bool transparent) {
  uint16_t tid = (uint16_t)(entry & 0x03FF);
  int hf = (entry >> 10) & 1, vf = (entry >> 11) & 1;
  const uint16_t* pal = pal_of(mr, (uint8_t)((entry >> 12) & 0x0F));
  const uint8_t* tb = tile_bytes(mr, tid);
  if (!tb) return;                                /* missing tile: leave as-is */
  for (int ty = 0; ty < 8; ty++) {
    const uint8_t* row = tb + (vf ? (7 - ty) : ty) * 4;
    uint16_t* o = dst + ty * stride;
    for (int tx = 0; tx < 8; tx++) {
      int sx = hf ? (7 - tx) : tx;
      /* 4bpp: two pixels per byte, LOW nibble is the LEFT pixel */
      uint8_t ci = (sx & 1) ? (uint8_t)(row[sx >> 1] >> 4) : (uint8_t)(row[sx >> 1] & 0x0F);
      if (transparent && ci == 0) continue;
      o[tx] = pal[ci];
    }
  }
}

bool mr_metatile_pixels(const MapRender* mr, uint16_t mid, uint16_t* dst, int stride) {
  uint16_t e[8];
  if (!mr || !dst || !mr_metatile_entries(mr, mid, e)) return false;

  /* Colour index 0 is transparent on EVERY background layer, including the bottom one —
   * what shows through is the backdrop, which the game sets to black in
   * LoadTilesetPalette (it overwrites palette entry 0 with RGB_BLACK and never displays
   * the tileset's own colour 0; measured 0x28A3 for gTileset_General, never visible).
   *
   * This function used to paint the tileset's colour 0 for the bottom layer. That is
   * wrong on hardware, and it bit only 0.6% of metatiles (6 of 981 COVERED ones have a
   * transparent bottom-layer pixel), which is exactly why the differential test against
   * the Python reference did not catch it: both implementations shared the same wrong
   * rule. A differential test between two of MY implementations cannot find a spec
   * error — only comparing against the real game's behaviour can. */
  for (int y = 0; y < MR_METATILE_PX; y++)
    for (int x = 0; x < MR_METATILE_PX; x++)
      dst[y * stride + x] = MR_BACKDROP;

  for (int layer = 0; layer < 2; layer++) {
    for (int q = 0; q < 4; q++) {                 /* TL, TR, BL, BR */
      uint16_t* o = dst + (q >> 1) * 8 * stride + (q & 1) * 8;
      draw_tile(mr, e[layer * 4 + q], o, stride, true /* 0 = transparent */);
    }
  }
  return true;
}

bool mr_region(const MapRender* mr, int x0, int y0, int w, int h,
               uint16_t* dst, int stride) {
  if (!mr || !dst || w <= 0 || h <= 0) return false;
  for (int by = 0; by < h; by++) {
    int my = y0 + by;
    if (my < 0 || my >= mr->lay.height) continue;
    for (int bx = 0; bx < w; bx++) {
      int mx = x0 + bx;
      if (mx < 0 || mx >= mr->lay.width) continue;
      uint16_t cell;
      if (!rom_blocks(mr->rom, &mr->lay,
                      (uint32_t)my * (uint32_t)mr->lay.width + (uint32_t)mx, &cell, 1))
        continue;
      mr_metatile_pixels(mr, ROM_CELL_METATILE(cell),
                         dst + (by * 16) * stride + bx * 16, stride);
    }
  }
  return true;
}

bool mr_metatile_mip(const MapRender* mr, uint16_t mid, int px, uint16_t* dst, int stride) {
  if (!mr || !dst || px <= 0 || px > MR_METATILE_PX) return false;
  uint16_t full[MR_METATILE_PIXELS];
  memset(full, 0, sizeof full);
  if (!mr_metatile_pixels(mr, mid, full, MR_METATILE_PX)) return false;
  /* Point-sample. The 13 tileset palettes hold ~208 distinct colours in total, so a
   * reduced image needs no requantisation — averaging would only invent colours. */
  int step = MR_METATILE_PX / px;
  for (int y = 0; y < px; y++)
    for (int x = 0; x < px; x++)
      dst[y * stride + x] = full[(y * step) * MR_METATILE_PX + x * step];
  return true;
}

/* ---- 8bpp index compositing (affine zoom BG) ------------------------------- */

/* Same geometry as draw_tile, but emits palette BYTES for the merged 256-entry BG
 * palette instead of colours: byte = (slot << 4) | colour_index. */
static void draw_tile_idx(const MapRender* mr, uint16_t entry, uint8_t* dst, int stride) {
  uint16_t tid = (uint16_t)(entry & 0x03FF);
  int hf = (entry >> 10) & 1, vf = (entry >> 11) & 1;
  uint8_t slot = (uint8_t)((entry >> 12) & 0x0F);
  const uint8_t* tb = tile_bytes(mr, tid);
  if (!tb) return;
  for (int ty = 0; ty < 8; ty++) {
    const uint8_t* row = tb + (vf ? (7 - ty) : ty) * 4;
    uint8_t* o = dst + ty * stride;
    for (int tx = 0; tx < 8; tx++) {
      int sx = hf ? (7 - tx) : tx;
      uint8_t ci = (sx & 1) ? (uint8_t)(row[sx >> 1] >> 4) : (uint8_t)(row[sx >> 1] & 0x0F);
      if (ci == 0) continue;                       /* transparent on every layer */
      o[tx] = (uint8_t)((slot << 4) | ci);
    }
  }
}

bool mr_metatile_idx8(const MapRender* mr, uint16_t mid, uint8_t* dst, int stride) {
  uint16_t e[8];
  if (!mr || !dst || !mr_metatile_entries(mr, mid, e)) return false;
  for (int y = 0; y < MR_METATILE_PX; y++)
    for (int x = 0; x < MR_METATILE_PX; x++)
      dst[y * stride + x] = 0;                     /* index 0 = backdrop */
  for (int layer = 0; layer < 2; layer++)
    for (int q = 0; q < 4; q++)
      draw_tile_idx(mr, e[layer * 4 + q],
                    dst + (q >> 1) * 8 * stride + (q & 1) * 8, stride);
  return true;
}

bool mr_metatile_mip_idx(const MapRender* mr, uint16_t mid, int px, uint8_t* dst, int stride) {
  if (!mr || !dst || px <= 0 || px > MR_METATILE_PX || (MR_METATILE_PX % px)) return false;
  uint8_t full[MR_METATILE_PIXELS];
  if (!mr_metatile_idx8(mr, mid, full, MR_METATILE_PX)) return false;
  int step = MR_METATILE_PX / px;
  for (int y = 0; y < px; y++)
    for (int x = 0; x < px; x++)
      dst[y * stride + x] = full[(y * step) * MR_METATILE_PX + x * step];
  return true;
}
