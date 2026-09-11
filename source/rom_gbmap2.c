/* Gen-2 overworld map data, located BY SHAPE -- see rom_gbmap2.h for the
 * design and the exact anchor bytes. Pure C: no tonc, no FatFs, no GBA
 * headers. Mirrors rom_gbmap.c's SHAPE only (Scan/scan_one, fileoff(),
 * rd()/rdg(), fail_closed()) -- Gen 1 and Gen 2 share no table. */
#include "rom_gbmap2.h"

#include <string.h>

#define GB_BANK   0x4000u
#define GB_WIN_LO 0x4000u
#define GB_WIN_HI 0x8000u

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

static int rdg(const RomGbMap2* g, uint32_t off, void* dst, uint32_t len) {
  if (!g->read || len == 0) return 0;
  if (off >= g->size || len > g->size - off) return 0;
  return g->read(g->ctx, off, dst, len) ? 1 : 0;
}

/* Same windowed-scratch full-ROM scan rom_gbmap.c's scan_one() uses. */
typedef int (*ScanCb)(const uint8_t* w);

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

/* ---- Anchor 1: GetAnyMapPointer -> MapGroupPointers ADDRESS (offset 6-7).
 * C5 05 48 06 00 21 lo hi 09 09 2A 66 6F C1 0D 06 00 3E 09 CD ?? ?? C9
 * The literal 3E 09 embeds MAP_LENGTH=9, self-validating. 23 bytes. */
static int cb_a1(const uint8_t* w) {
  return w[0]==0xC5 && w[1]==0x05 && w[2]==0x48 && w[3]==0x06 && w[4]==0x00 &&
         w[5]==0x21 && w[8]==0x09 && w[9]==0x09 && w[10]==0x2A && w[11]==0x66 &&
         w[12]==0x6F && w[13]==0xC1 && w[14]==0x0D && w[15]==0x06 && w[16]==0x00 &&
         w[17]==0x3E && w[18]==0x09 && w[19]==0xCD && w[22]==0xC9;
}
#define A1_LOOK 23

/* ---- Anchor 2: GetAnyMapField -> MapGroupPointers BANK (offset 4).
 * F0 ?? F5 3E bb ?? CD ll hh 19 4E 23 46 F1 ?? C9
 * Accepted only when its CD ll hh call target equals anchor 1's own hit
 * (file offset), i.e. GetAnyMapField really does call GetAnyMapPointer. */
static int cb_a2(const uint8_t* w) {
  return w[0]==0xF0 && w[2]==0xF5 && w[3]==0x3E && w[6]==0xCD && w[9]==0x19 &&
         w[10]==0x4E && w[11]==0x23 && w[12]==0x46 && w[13]==0xF1 && w[15]==0xC9;
}
#define A2_LOOK 16

/* ---- Anchor 3: LoadMapTileset -> Tilesets ADDRESS (offset 3-4) and BANK
 * (offset 21), together. E5 C5 21 lo hi 01 0F 00 FA ?? ?? CD ?? ?? 11 ?? ??
 * 01 0F 00 3E bb CD ?? ?? C1 E1 C9. "01 0F 00" (ld bc,TILESET_LENGTH=15)
 * appears twice -- self-validating. Extra filter: the FA operand (offset
 * 9-10, wMapTileset) and the 11 operand (offset 15-16, wTilesetBank) must
 * both be WRAM (0xC000-0xDFFF). */
static int cb_a3(const uint8_t* w) {
  if (!(w[0]==0xE5 && w[1]==0xC5 && w[2]==0x21 && w[5]==0x01 && w[6]==0x0F &&
        w[7]==0x00 && w[8]==0xFA && w[11]==0xCD && w[14]==0x11 &&
        w[17]==0x01 && w[18]==0x0F && w[19]==0x00 && w[20]==0x3E &&
        w[22]==0xCD && w[25]==0xC1 && w[26]==0xE1 && w[27]==0xC9))
    return 0;
  uint16_t fa = rd16(w + 9);
  uint16_t l11 = rd16(w + 15);
  return fa >= 0xC000u && fa < 0xE000u && l11 >= 0xC000u && l11 < 0xE000u;
}
#define A3_LOOK 28

/* Fail-closed contract (matches rom_gbmap.c's own): every `return false`
 * path zeroes every DERIVED field; read/ctx/size (the caller's own inputs)
 * are preserved. */
static bool fail_closed(RomGbMap2* g) {
  GbReadFn read = g->read; void* ctx = g->ctx; uint32_t size = g->size;
  memset(g, 0, sizeof *g);
  g->read = read; g->ctx = ctx; g->size = size;
  return false;
}

/* Tileset row (15 B) plausibility -- design §3.4: the three dba pointers
 * (GFX, Meta, Coll) must be in the banked window (0x4000-0x7FFF), the three
 * bank bytes must be within the ROM's own bank count, and the reserved
 * dw at row offset 0x0B must be zero (the table's own length terminator). */
static int tileset_row_ok(const uint8_t row[15], uint32_t n_banks) {
  uint8_t gfx_bank = row[0], meta_bank = row[3], coll_bank = row[6];
  uint16_t gfx_a = rd16(row + 1), meta_a = rd16(row + 4), coll_a = rd16(row + 7);
  uint16_t reserved = rd16(row + 11);
  if (!(gfx_a >= GB_WIN_LO && gfx_a < GB_WIN_HI)) return 0;
  if (!(meta_a >= GB_WIN_LO && meta_a < GB_WIN_HI)) return 0;
  if (!(coll_a >= GB_WIN_LO && coll_a < GB_WIN_HI)) return 0;
  if (gfx_bank >= n_banks || meta_bank >= n_banks || coll_bank >= n_banks) return 0;
  if (reserved != 0) return 0;
  return 1;
}

bool rgm2_open(RomGbMap2* g, GbReadFn read, void* ctx, uint32_t size,
               uint8_t* scratch, uint32_t scratch_len) {
  memset(g, 0, sizeof *g);
  g->read = read; g->ctx = ctx; g->size = size;
  if (!read || size == 0 || !scratch || scratch_len < ROM_GBMAP2_SCRATCH_MIN) return false;

  Scan s = { read, ctx, size, scratch, scratch_len };

  /* Anchor 1: MapGroupPointers address, exactly 1 hit required. */
  uint32_t h1[2];
  uint32_t n1 = scan_one(&s, cb_a1, A1_LOOK, h1, 2);
  if (n1 != 1) return fail_closed(g);
  uint8_t w1[A1_LOOK];
  if (!rd(&s, h1[0], w1, sizeof w1)) return fail_closed(g);
  uint16_t groups_addr = rd16(w1 + 6);

  /* Anchor 2: MapGroupPointers bank, exactly 1 CROSS-VALIDATED hit required. */
  uint32_t h2raw[8];
  uint32_t n2raw = scan_one(&s, cb_a2, A2_LOOK, h2raw, 8);
  uint32_t h2good[2]; uint32_t n2 = 0;
  for (uint32_t i = 0; i < n2raw && i < 8; i++) {
    uint8_t w2[A2_LOOK];
    if (!rd(&s, h2raw[i], w2, sizeof w2)) continue;
    uint16_t call_target = rd16(w2 + 7);
    if (call_target == h1[0]) { if (n2 < 2) h2good[n2] = h2raw[i]; n2++; }
  }
  if (n2 != 1) return fail_closed(g);
  uint8_t w2[A2_LOOK];
  if (!rd(&s, h2good[0], w2, sizeof w2)) return fail_closed(g);
  uint8_t groups_bank = w2[4];
  uint32_t groups_off = fileoff(groups_bank, groups_addr);

  /* Anchor 3: Tilesets address + bank together, exactly 1 hit required. */
  uint32_t h3[2];
  uint32_t n3 = scan_one(&s, cb_a3, A3_LOOK, h3, 2);
  if (n3 != 1) return fail_closed(g);
  uint8_t w3[A3_LOOK];
  if (!rd(&s, h3[0], w3, sizeof w3)) return fail_closed(g);
  uint16_t tilesets_addr = rd16(w3 + 3);
  uint8_t  tilesets_bank = w3[21];
  uint32_t tilesets_off = fileoff(tilesets_bank, tilesets_addr);

  /* Group table length -- derived, never compiled in (design §3.4/§1(2)):
   * N = (entry[0] - table_addr) / 2, cross-checked by requiring all N
   * entries in $4000-$7FFF and entry [N] to fail that test. */
  if (groups_off >= size || size - groups_off < 2) return fail_closed(g);
  uint16_t entry0;
  { uint8_t tmp[2]; if (!rd(&s, groups_off, tmp, 2)) return fail_closed(g); entry0 = rd16(tmp); }
  if (entry0 < groups_addr) return fail_closed(g);
  uint32_t n_groups = (uint32_t)(entry0 - groups_addr) / 2u;
  if (n_groups < 1 || n_groups > 64) return fail_closed(g);
  {
    int all_in = 1;
    for (uint32_t i = 0; i < n_groups; i++) {
      uint8_t tmp[2];
      if (!rd(&s, groups_off + i * 2u, tmp, 2)) return fail_closed(g);
      uint16_t e = rd16(tmp);
      if (!(e >= GB_WIN_LO && e < GB_WIN_HI)) { all_in = 0; break; }
    }
    if (!all_in) return fail_closed(g);
    uint8_t tmp[2];
    if (!rd(&s, groups_off + n_groups * 2u, tmp, 2)) return fail_closed(g);
    uint16_t e_extra = rd16(tmp);
    if (e_extra >= GB_WIN_LO && e_extra < GB_WIN_HI) return fail_closed(g);  /* must FAIL the in-window test */
  }

  /* Tileset row count -- derived by walking rows until one is implausible. */
  uint32_t n_banks = size / GB_BANK;
  uint32_t n_tilesets = 0;
  for (;;) {
    uint32_t base = tilesets_off + n_tilesets * 15u;
    uint8_t row[15];
    if (!rd(&s, base, row, sizeof row)) break;
    if (!tileset_row_ok(row, n_banks)) break;
    n_tilesets++;
    if (n_tilesets > 255) return fail_closed(g);  /* runaway, never happens on real data */
  }
  if (n_tilesets < 1) return fail_closed(g);

  g->groups_off = groups_off;
  g->groups_bank = groups_bank;
  g->n_groups = (uint16_t)n_groups;
  g->tilesets_off = tilesets_off;
  g->n_tilesets = (uint8_t)n_tilesets;
  g->ok = 1;

  /* Structural cross-check (design's own "validate group 1/map 1" rule):
   * resolve group 1 / map 1 through the full two-hop chain and its
   * tileset; fail closed if either fails. */
  GbMap2Map m0;
  if (!rgm2_map(g, 1, 1, &m0)) return fail_closed(g);
  GbMap2Tileset ts0;
  if (!rgm2_tileset(g, m0.tileset_id, &ts0)) return fail_closed(g);

  return true;
}

bool rgm2_map(const RomGbMap2* g, uint8_t group, uint8_t number, GbMap2Map* out) {
  memset(out, 0, sizeof *out);
  if (!g->ok) return false;
  if (group < 1 || group > g->n_groups) return false;

  uint8_t ptr2[2];
  if (!rdg(g, g->groups_off + (uint32_t)(group - 1) * 2u, ptr2, 2)) return false;
  uint16_t group_ptr = rd16(ptr2);
  /* The group's own map array lives in the SAME bank as MapGroupPointers
   * (GetAnyMapPointer derefs it with no intervening bankswitch). */
  uint32_t group_base = fileoff(g->groups_bank, group_ptr);

  if (number < 1) return false;
  uint32_t map_off = group_base + (uint32_t)(number - 1) * 9u;
  uint8_t rec[9];
  if (!rdg(g, map_off, rec, sizeof rec)) return false;

  uint8_t attr_bank = rec[0];
  uint8_t tileset_id = rec[1];
  uint8_t environment = rec[2];
  uint16_t attr_ptr = rd16(rec + 3);
  uint8_t tod_pal = rec[7] & 0x0Fu;   /* dn-packed nibble pair -- MUST mask */

  uint32_t attr_off = fileoff(attr_bank, attr_ptr);
  uint8_t attr[12];
  if (!rdg(g, attr_off, attr, sizeof attr)) return false;

  uint8_t border = attr[0], height = attr[1], width = attr[2];
  if (height < 1 || height > 64 || width < 1 || width > 64) return false;
  uint8_t blocks_bank = attr[3];
  uint16_t blocks_ptr = rd16(attr + 4);
  uint8_t conn_mask = attr[11];

  uint32_t blocks_off = fileoff(blocks_bank, blocks_ptr);
  uint32_t blocks_need = (uint32_t)height * (uint32_t)width;
  if (blocks_off >= g->size || blocks_need > g->size - blocks_off) return false;

  uint8_t nconn = 0;
  for (int b = 0; b < 4; b++) if (conn_mask & (1u << b)) nconn++;
  if (nconn > 4) return false;   /* cannot happen (4 bits), belt */

  /* Connection records: 12 B each, N,S,W,E SOURCE order (design §2.3),
   * bitmask bit order is E/W/S/N (design §1(4)) -- these two orders differ. */
  static const uint8_t src_order_bits[4] = { 3u, 2u, 1u, 0u };  /* N,S,W,E -> bit index */
  uint32_t p = attr_off + 12u;
  uint8_t idx = 0;
  for (int k = 0; k < 4; k++) {
    if (!(conn_mask & (1u << src_order_bits[k]))) continue;
    uint8_t rec12[12];
    if (!rdg(g, p, rec12, sizeof rec12)) return false;
    out->conn[idx].group  = rec12[0];
    out->conn[idx].number = rec12[1];
    out->conn[idx].len    = rec12[6];
    out->conn[idx].width  = rec12[7];
    out->conn[idx].y      = rec12[8];
    out->conn[idx].x      = rec12[9];
    idx++;
    p += 12u;
  }

  out->group = group;
  out->number = number;
  out->tileset_id = tileset_id;
  out->environment = environment;
  out->tod_palette = tod_pal;
  out->border_block = border;
  out->height = height;
  out->width = width;
  out->blocks_off = blocks_off;
  out->conn_mask = conn_mask;
  out->nconn = nconn;
  return true;
}

bool rgm2_tileset(const RomGbMap2* g, uint8_t tileset_id, GbMap2Tileset* out) {
  memset(out, 0, sizeof *out);
  if (!g->ok) return false;
  if (tileset_id >= g->n_tilesets) return false;

  uint8_t row[15];
  uint32_t off = g->tilesets_off + (uint32_t)tileset_id * 15u;
  if (!rdg(g, off, row, sizeof row)) return false;

  uint8_t  gfx_bank = row[0], meta_bank = row[3], coll_bank = row[6];
  uint16_t gfx_a = rd16(row + 1), meta_a = rd16(row + 4), coll_a = rd16(row + 7);
  uint16_t pal_a = rd16(row + 13);   /* bare dw, no bank byte (design §7.2) */

  uint32_t gfx_off = fileoff(gfx_bank, gfx_a);
  uint32_t meta_off = fileoff(meta_bank, meta_a);
  uint32_t coll_off = fileoff(coll_bank, coll_a);
  if (gfx_off >= g->size || meta_off >= g->size || coll_off >= g->size) return false;

  /* meta_len is derived from Coll-Meta (design §4: Coll-Meta is $400 or
   * $800 for every same-bank row, an independent confirmation of the 16
   * B/block stride) -- only meaningful when Meta and Coll share a bank. */
  uint32_t meta_len = 0;
  if (meta_bank == coll_bank && coll_off > meta_off) meta_len = coll_off - meta_off;
  if (meta_len == 0 || meta_len > 2048u) return false;

  out->gfx_off = gfx_off;
  out->meta_off = meta_off;
  out->coll_off = coll_off;
  out->meta_len = meta_len;
  /* pal_off is resolved by the caller's own bank (discovered separately by
   * the colour anchor in step 7) -- store the raw address's file offset
   * assuming home bank as a harmless default; the colour path never trusts
   * this field without its own anchor-derived bank. */
  out->pal_off = pal_a;
  return true;
}
