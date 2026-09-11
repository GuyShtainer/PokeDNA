/* Gen-1 overworld map data, located BY SHAPE -- see rom_gbmap.h for the design
 * and the exact anchor bytes. Pure C: no tonc, no FatFs, no GBA headers. */
#include "rom_gbmap.h"

#include <string.h>

#define GB_BANK   0x4000u
#define GB_WIN_LO 0x4000u

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t fileoff(uint32_t bank, uint16_t addr) {
  if (addr < GB_WIN_LO) return addr;          /* home bank, bank byte ignored */
  return bank * GB_BANK + (uint32_t)(addr - GB_WIN_LO);
}

typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t size;
  uint8_t* scratch;
  uint32_t scratch_len;
} Scan;

static int rd(const Scan* s, uint32_t off, void* dst, uint32_t len) {
  if (!s->read || len == 0) return 0;
  if (off >= s->size || len > s->size - off) return 0;
  return s->read(s->ctx, off, dst, len) ? 1 : 0;
}

static int rdg(const RomGbMap1* g, uint32_t off, void* dst, uint32_t len) {
  if (!g->read || len == 0) return 0;
  if (off >= g->size || len > g->size - off) return 0;
  return g->read(g->ctx, off, dst, len) ? 1 : 0;
}

/* --------------------------------------------------------------- scan -----
 * Same windowed-scratch full-ROM scan rom_gbui.c's scan_multi() uses, cut
 * down to the single-job case this module needs 3 separate times (each
 * table's own anchor is scanned independently -- Gen-1 ROMs are 1 MiB,
 * three passes at a few hundred KB/s of SD read is the same order of
 * magnitude rom_gbui.c's own font/textbox/cardframe scan already pays). */
typedef int (*ScanCb)(const uint8_t* w);

/* Returns the number of hits found (capped at `cap`), storing each hit's
 * FILE OFFSET in `out`. `look` is how many bytes of context `cb` needs
 * starting at each candidate position. */
static uint32_t scan_one(const Scan* s, ScanCb cb, uint32_t look,
                          uint32_t* out, uint32_t cap) {
  uint8_t* w = s->scratch;
  uint32_t wcap = s->scratch_len;
  uint32_t n = 0;
  if (!w || wcap < look + 16u || s->size < look) return 0;
  uint32_t step = wcap - look + 1u;
  for (uint32_t base = 0; base + look <= s->size; base += step) {
    uint32_t take = s->size - base; if (take > wcap) take = wcap;
    if (!rd(s, base, w, take)) return n;
    uint32_t lim = take - look;
    for (uint32_t i = 0; i <= lim; i++) {
      if (!cb(w + i)) continue;
      if (n < cap) out[n] = base + i;
      n++;
    }
  }
  return n;
}

/* ---- MapHeaderBanks: E5 C5 4F 06 00 3E bb CD ll hh 21 lo hi 09 ---------- */
static int cb_banks(const uint8_t* w) {
  return w[0]==0xE5 && w[1]==0xC5 && w[2]==0x4F && w[3]==0x06 && w[4]==0x00 &&
         w[5]==0x3E && w[7]==0xCD && w[10]==0x21 && w[13]==0x09;
}

/* ---- MapHeaderPointers: LoadMapHeader's BIT 7,B / RET NZ (CB 78 C0) ----- */
static int cb_hdr(const uint8_t* w) {
  return w[0]==0xCB && w[1]==0x78 && w[2]==0xC0;
}

/* Structural filter for a cb_ts() raw hit -- forward-declared so cb_ts()
 * itself can fold it in (m1 review D7): one-or-more ADD HL,DE (0x19) then
 * LD DE,imm16 (0x11 lo hi) whose operand looks like a WRAM address
 * (0xC000-0xDFFF, wTilesetBank) -- this is what collapses dozens of raw
 * `5F 21` hits down to exactly one real Tilesets reference (verified on
 * both Red and Yellow: 1 candidate survives in each, landing on the
 * decomp's own Tilesets symbol byte-for-byte). `w` has >= 12 bytes (the
 * scan's own `look`). */
static int ts_filter_ok(const uint8_t* w);

/* ---- Tilesets: LD E,A / LD HL,Tilesets (5F 21 lo hi), structurally
 * filtered in-callback (m1 review D7) so scan_one()'s own `cap` counts real
 * candidates, not raw 2-byte-prefix hits -- a raw scan finds dozens of `5F
 * 21` occurrences, so capping the raw hit array at a small `cap` used to
 * risk truncating before the one real candidate was even seen. ---------- */
static int cb_ts(const uint8_t* w) {
  return w[0]==0x5F && w[1]==0x21 && ts_filter_ok(w);
}

static int ts_filter_ok(const uint8_t* w) {
  uint32_t j = 4; int n19 = 0;
  while (j < 8 && w[j] == 0x19) { n19++; j++; }
  if (n19 < 1) return 0;
  if (w[j] != 0x11) return 0;
  uint16_t wram = rd16(w + j + 1);
  return wram >= 0xC000u && wram < 0xE000u;
}

/* Fail-closed contract (matches rom_gbui.h's own: "gen = 0, every offset 0,
 * ok = 0"): every `return false` path below goes through here so a caller
 * can never see a PARTIALLY-located `g` (e.g. banks_off set from an earlier
 * step that succeeded, before a later step failed) and mistake it for a
 * trustworthy result. `read`/`ctx`/`size` are the caller's own inputs, kept
 * as-is; everything DERIVED is zeroed. */
static bool fail_closed(RomGbMap1* g) {
  GbReadFn read = g->read; void* ctx = g->ctx; uint32_t size = g->size;
  memset(g, 0, sizeof *g);
  g->read = read; g->ctx = ctx; g->size = size;
  return false;
}

bool rgm1_open(RomGbMap1* g, GbReadFn read, void* ctx, uint32_t size,
               uint8_t* scratch, uint32_t scratch_len) {
  memset(g, 0, sizeof *g);
  g->read = read; g->ctx = ctx; g->size = size;
  if (!read || size == 0 || !scratch || scratch_len < ROM_GBMAP_SCRATCH_MIN) return false;

  Scan s = { read, ctx, size, scratch, scratch_len };

  /* MapHeaderBanks: exactly 1 hit required (fail closed / ambiguous fallback). */
  uint32_t bhit[2];
  uint32_t bn = scan_one(&s, cb_banks, 14, bhit, 2);
  if (bn != 1) return fail_closed(g);
  {
    uint8_t w[14];
    if (!rd(&s, bhit[0], w, sizeof w)) return fail_closed(g);
    uint8_t  bank = w[6];
    uint16_t addr = rd16(w + 11);
    g->banks_off = fileoff(bank, addr);
  }

  /* MapHeaderPointers: exactly 1 CB 78 C0 hit required; branch on the byte
   * right after it (Red: 0x21 direct; Yellow: 0xCD indirect via
   * GetMapHeaderPointer). */
  uint32_t hhit[2];
  uint32_t hn = scan_one(&s, cb_hdr, 3, hhit, 2);
  if (hn != 1) return fail_closed(g);
  {
    uint8_t w[6];
    if (!rd(&s, hhit[0], w, sizeof w)) return fail_closed(g);
    uint8_t nb = w[3];
    if (nb == 0x21) {
      uint16_t addr = rd16(w + 4);
      if (addr >= GB_WIN_LO) return fail_closed(g);   /* must be home bank */
      g->ptrs_off = addr;
      g->num_maps = 248;   /* Red/Blue-shape (direct): pokered's own NUM_MAPS (D8) */
    } else if (nb == 0xCD) {
      uint16_t target = rd16(w + 4);
      if (target >= GB_WIN_LO) return fail_closed(g); /* call target must be home bank */
      uint8_t fn[48];
      uint32_t fn_n = 48;
      if (target + fn_n > size) fn_n = size - target;
      if (fn_n < 8 || !rd(&s, target, fn, fn_n)) return fail_closed(g);
      int have_bank = 0; uint8_t bank = 0; uint32_t i;
      uint32_t addr_off = 0; int have_addr = 0;
      for (i = 0; i + 2 < fn_n; i++) {
        if (fn[i] == 0x3E && fn[i + 2] == 0xCD) { bank = fn[i + 1]; have_bank = 1; }
        if (have_bank && fn[i] == 0x21 && i + 2 < fn_n) { addr_off = i; have_addr = 1; break; }
      }
      if (!have_bank || !have_addr) return fail_closed(g);
      uint16_t addr = rd16(fn + addr_off + 1);
      g->ptrs_off = fileoff(bank, addr);
      g->num_maps = 249;   /* Yellow-shape (indirect): Yellow's own NUM_MAPS (D8) */
    } else {
      return fail_closed(g);
    }
  }

  /* Tilesets: the structural filter is now IN cb_ts() itself (m1 review D7),
   * so scan_one()'s own hit cap counts real (filtered) candidates instead of
   * raw `5F 21` prefix hits -- exactly 1 is required, same as the other two
   * tables' own anchors. */
  {
    uint32_t thit[2];
    uint32_t tn = scan_one(&s, cb_ts, 12, thit, 2);
    if (tn != 1) return fail_closed(g);
    uint8_t w[12];
    if (!rd(&s, thit[0], w, sizeof w)) return fail_closed(g);
    uint16_t addr = rd16(w + 2);
    uint32_t bank = thit[0] / GB_BANK;    /* same bank as this code, see header note */
    g->tilesets_off = fileoff(bank, addr);
  }

  /* Structural cross-check (design §6): map id 0 must parse to a plausible
   * header through BOTH located tables together, not just each anchor in
   * isolation. Every Gen-1 game defines map id 0. */
  GbMap1Header h0;
  g->ok = 1;   /* rgm1_header() below requires g->ok to run */
  if (!rgm1_header(g, 0, &h0)) return fail_closed(g);
  GbMap1Tileset ts0;
  if (!rgm1_tileset(g, h0.tileset_id, &ts0)) return fail_closed(g);

  return true;
}

bool rgm1_header(const RomGbMap1* g, uint8_t map_id, GbMap1Header* out) {
  memset(out, 0, sizeof *out);
  if (!g->ok) return false;
  if (map_id >= g->num_maps) return false;   /* past the real table (m1 review D8) --
                                               * reading beyond it hits unrelated bytes
                                               * that can coincidentally still look like
                                               * a plausible small header */

  uint8_t bank;
  if (!rdg(g, g->banks_off + map_id, &bank, 1)) return false;

  uint8_t ptr2[2];
  if (!rdg(g, g->ptrs_off + (uint32_t)map_id * 2u, ptr2, 2)) return false;
  uint16_t ptr = rd16(ptr2);
  uint32_t hdr_off = fileoff(bank, ptr);

  /* 10-byte partial header (corrected shape -- see rom_gbmap.h's top comment). */
  uint8_t h[10];
  if (!rdg(g, hdr_off, h, sizeof h)) return false;
  uint8_t tileset_id = h[0], height = h[1], width = h[2];
  uint8_t conn_mask = h[9];
  if (height == 0 || height > 128 || width == 0 || width > 128) return false;   /* Route 17/23 are 10x72 (m1 review D2) */

  uint8_t nconn = 0;
  for (int b = 0; b < 4; b++) if (conn_mask & (1u << b)) nconn++;
  if (nconn > ROM_GBMAP1_MAX_CONN) return false;   /* cannot happen (4 bits), belt */

  uint32_t p = hdr_off + 10u;
  for (uint8_t i = 0; i < nconn; i++) {
    uint8_t rec[11];
    if (!rdg(g, p, rec, sizeof rec)) return false;
    out->conn[i].map_id = rec[0];
    out->conn[i].width  = rec[6];
    out->conn[i].y      = rec[7];
    out->conn[i].x      = rec[8];
    p += 11u;
  }
  uint8_t obj2[2];
  if (!rdg(g, p, obj2, 2)) return false;

  uint16_t blocks_ptr = rd16(h + 3);
  uint32_t blocks_off = fileoff(bank, blocks_ptr);
  uint32_t blocks_need = (uint32_t)height * (uint32_t)width;
  if (blocks_off >= g->size || blocks_need > g->size - blocks_off) return false;

  out->map_id = map_id;
  out->bank = bank;
  out->hdr_off = hdr_off;
  out->tileset_id = tileset_id;
  out->height = height;
  out->width = width;
  out->blocks_off = blocks_off;
  out->conn_mask = conn_mask;
  out->nconn = nconn;
  out->obj_off = p;
  return true;
}

bool rgm1_tileset(const RomGbMap1* g, uint8_t tileset_id, GbMap1Tileset* out) {
  memset(out, 0, sizeof *out);
  if (!g->ok) return false;
  uint8_t row[12];
  uint32_t off = g->tilesets_off + (uint32_t)tileset_id * 12u;
  if (!rdg(g, off, row, sizeof row)) return false;
  uint8_t  gfx_bank = row[0];
  uint16_t block_ptr = rd16(row + 1);
  uint16_t gfx_ptr = rd16(row + 3);
  uint32_t block_off = fileoff(gfx_bank, block_ptr);
  uint32_t gfx_off = fileoff(gfx_bank, gfx_ptr);
  if (block_off >= g->size || gfx_off >= g->size) return false;
  out->gfx_bank = gfx_bank;
  out->block_off = block_off;
  out->gfx_off = gfx_off;
  return true;
}

bool rgm1_block(const RomGbMap1* g, const GbMap1Tileset* ts, uint8_t block_id,
                uint8_t out16[16]) {
  if (!g->ok) return false;
  return rdg(g, ts->block_off + (uint32_t)block_id * 16u, out16, 16);
}

bool rgm1_tile2bpp(const RomGbMap1* g, const GbMap1Tileset* ts, uint8_t tile_id,
                    uint8_t out16[16]) {
  if (!g->ok) return false;
  return rdg(g, ts->gfx_off + (uint32_t)tile_id * 16u, out16, 16);
}
