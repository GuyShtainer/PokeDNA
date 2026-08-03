/* Gen-3 region-map reader/renderer. See region_map.h for the data model, the exact
 * relationship between the two zoom levels, and the GBA memory budget.
 *
 * ---- where the addresses come from -----------------------------------------
 * Every anchor below is the `sRegionMapBg_GfxLZ` / `sRegionMapBkgnd_ImageLZ` /
 * `sRegionMap_Gfx` symbol from pret's per-revision byte-matching .sym files
 * (pokeemerald + pokeruby{,_rev1,_rev2} + pokesapphire{,_rev1,_rev2} +
 * pokefirered{,_rev1} + pokeleafgreen{,_rev1}). Everything else is reached by a fixed
 * offset from that anchor, because these symbols sit in one contiguous data blob whose
 * INTERNAL layout is identical across revisions even though the blob itself shifts:
 *
 *   Ruby r0  gfx 083E5DA0  layout +0x10B0  entries +0x1624
 *   Ruby r1  gfx 083E5DBC  layout +0x10B0  entries +0x1624   <- +0x1C vs r0
 *   Sapph r0 gfx 083E5DF8  layout +0x10B0  entries +0x1624
 *   Emerald  gfx 0859F77C  layout +0x11F0  entries +0x1D00
 *   FireRed  gfx 083EF61C  kanto  +0x1280  corners  +0x2844
 *   FR r1    gfx 083EF68C  kanto  +0x1280  corners  +0x2844   <- +0x70 vs r0
 *
 * NOTE the revision deltas are NOT the same as rom_map.c's gMapGroups deltas
 * (Ruby r2 shifts gMapGroups by +0x18 but the region map by +0x1C), so these had to be
 * taken per symbol rather than extrapolated. They were then re-derived independently by
 * signature-scanning the user's own Ruby r2 and FireRed r1 dumps, and matched.
 *
 * ---- the field order that bites --------------------------------------------
 * RSE's struct RegionMapLocation is { u8 x, y, width, height; const u8 *name; }.
 * x/y/w/h FIRST, pointer LAST. Reading it the intuitive way round gives you 0x01010B04
 * as a "pointer" and four bytes of a string as "coordinates".
 */

#include <stdint.h>
#include <string.h>
#include "region_map.h"
#include "map_render.h"     /* mr_lz77 / mr_lz77_size — the ONE LZ77 decoder */

/* ---- little-endian helpers (no unaligned loads, no host-endian assumptions) -- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- per-family geometry ---------------------------------------------------- */

#define RSE_TILES_BYTES    14912u   /* 233 8bpp tiles                              */
#define RSE_TILEMAP_BYTES   4096u   /* 64x64 affine, 1 byte per entry              */
#define FR_TILES_BYTES     10240u   /* 320 4bpp tiles                              */
#define FR_TILEMAP_BYTES    1200u   /* 30x20 text entries, 2 bytes each            */

/* Padded compressed stream lengths, from the decomp symbol sizes. Used only to size
 * the caller's staging buffer for the BIOS path; over-reading past the stream is
 * harmless because the decompressor stops at the declared output size. */
#define RSE_TILES_STREAM   0xD64u
#define RSE_TILEMAP_STREAM 0x34Cu
#define FR_TILES_STREAM    0xD14u
static const uint16_t FR_TILEMAP_STREAM[RGN_MAX_VIEWS] = { 0x260u, 0x110u, 0xE4u, 0x11Cu };

/* Offsets from the tile-stream anchor. */
#define RS_OFF_LAYOUT   0x10B0u
#define RS_OFF_ENTRIES  0x1624u
#define E_OFF_LAYOUT    0x11F0u
#define E_OFF_ENTRIES   0x1D00u
#define RSE_OFF_TILEMAP 0x0D64u
#define RSE_OFF_PAL     0x0040u   /* subtracted */

#define FR_OFF_PAL      0x0340u   /* subtracted */
#define FR_OFF_CORNERS  0x2844u
#define FR_OFF_DIMS     0x2B5Cu
#define FR_OFF_SECTIONS 0x2E74u   /* Kanto; each later view is +0x294               */
#define FR_SECTIONS_STEP 0x294u
static const uint16_t FR_OFF_TILEMAP[RGN_MAX_VIEWS] = { 0x1280u, 0x14E0u, 0x15F0u, 0x16D4u };

/* ---- per-version anchors ---------------------------------------------------- */

typedef struct { RomKind kind; uint8_t ver; uint32_t gfx; } RgnAnchor;

static const RgnAnchor ANCHORS[] = {
  { ROM_EMERALD,   0, 0x0859F77Cu },   /* VERIFIED against the user's BPEE r0 dump  */
  { ROM_RUBY,      0, 0x083E5DA0u },
  { ROM_RUBY,      1, 0x083E5DBCu },
  { ROM_RUBY,      2, 0x083E5DBCu },   /* VERIFIED against the user's AXVE r2 dump  */
  { ROM_SAPPHIRE,  0, 0x083E5DF8u },
  { ROM_SAPPHIRE,  1, 0x083E5E18u },
  { ROM_SAPPHIRE,  2, 0x083E5E18u },
  { ROM_FIRERED,   0, 0x083EF61Cu },
  { ROM_FIRERED,   1, 0x083EF68Cu },   /* VERIFIED against the user's BPRE r1 dump  */
  { ROM_LEAFGREEN, 0, 0x083EF458u },
  { ROM_LEAFGREEN, 1, 0x083EF4C8u },
};
#define ANCHOR_COUNT ((int)(sizeof ANCHORS / sizeof ANCHORS[0]))

/* ---- geometry fill ---------------------------------------------------------- */

static bool fill_family(RgnMap* r, RomKind kind) {
  memset(r, 0, sizeof *r);
  switch (kind) {
    case ROM_RUBY: case ROM_SAPPHIRE: r->family = RGN_FAM_RS;   break;
    case ROM_EMERALD:                 r->family = RGN_FAM_E;    break;
    case ROM_FIRERED: case ROM_LEAFGREEN: r->family = RGN_FAM_FRLG; break;
    default: return false;
  }
  if (r->family == RGN_FAM_FRLG) {
    r->view_count = 4;  r->bpp = 4;  r->tilemap_entry_bytes = 2;  r->layer_count = 2;
    r->grid_w = 22; r->grid_h = 15;  r->grid_x0 = 32; r->grid_y0 = 32;
    r->map_tw = 30; r->map_th = 20;
    r->tiles_bytes = FR_TILES_BYTES;  r->tilemap_bytes = FR_TILEMAP_BYTES;
    r->pal_colours = 80;  r->pal_base = 0;
    r->mapsec_first = 88; r->mapsec_count = 109; r->mapsec_none = 197;
  } else {
    r->view_count = 1;  r->bpp = 8;  r->tilemap_entry_bytes = 1;  r->layer_count = 1;
    r->grid_w = 28; r->grid_h = 15;  r->grid_x0 = 8;  r->grid_y0 = 16;
    r->map_tw = 64; r->map_th = 64;
    r->tiles_bytes = RSE_TILES_BYTES; r->tilemap_bytes = RSE_TILEMAP_BYTES;
    r->pal_colours = 48;  r->pal_base = 112;
    r->mapsec_first = 0;
    r->mapsec_count = (r->family == RGN_FAM_E) ? 213 : 88;
    r->mapsec_none  = r->mapsec_count;   /* MAPSEC_NONE is one past the last entry */
  }
  return true;
}

/* Derive every address from the tile-stream anchor. */
static void derive(RgnMap* r, uint32_t gfx) {
  r->tiles_addr = gfx;
  if (r->family == RGN_FAM_FRLG) {
    r->pal_addr = gfx - FR_OFF_PAL;
    for (int v = 0; v < 4; v++) {
      r->tilemap_addr[v] = gfx + FR_OFF_TILEMAP[v];
      r->grid_addr[v]    = gfx + FR_OFF_SECTIONS + (uint32_t)v * FR_SECTIONS_STEP;
    }
    r->corners_addr = gfx + FR_OFF_CORNERS;
    r->dims_addr    = gfx + FR_OFF_DIMS;
    r->entries_addr = 0;
  } else {
    r->pal_addr        = gfx - RSE_OFF_PAL;
    r->tilemap_addr[0] = gfx + RSE_OFF_TILEMAP;
    r->grid_addr[0]    = gfx + ((r->family == RGN_FAM_E) ? E_OFF_LAYOUT  : RS_OFF_LAYOUT);
    r->entries_addr    = gfx + ((r->family == RGN_FAM_E) ? E_OFF_ENTRIES : RS_OFF_ENTRIES);
    r->corners_addr = r->dims_addr = 0;
  }
}

/* ---- validation ------------------------------------------------------------- */

/* Read the LAYER_MAP plane of view 0's grid in chunks and check it is a plausible
 * mapsec grid: every value inside the game's id range, and a healthy number of cells
 * actually occupied (the real grids have 150+ non-empty cells out of 330-420). */
static bool grid_ok(const RgnMap* r) {
  uint8_t buf[64];
  uint32_t total = (uint32_t)r->grid_w * r->grid_h, done = 0, used = 0;
  while (done < total) {
    uint32_t n = total - done; if (n > sizeof buf) n = sizeof buf;
    if (!rom_read_at(r->rom, r->grid_addr[0] + done, buf, n)) return false;
    for (uint32_t i = 0; i < n; i++) {
      uint16_t v = buf[i];
      if (v < r->mapsec_first && v != r->mapsec_none) return false;
      if (v > r->mapsec_none) return false;
      if (v != r->mapsec_none) used++;
    }
    done += n;
  }
  return used >= 60 && used <= total;
}

static bool rects_ok(const RgnMap* r) {
  if (r->family == RGN_FAM_FRLG) {
    uint8_t c[32], d[32];   /* 8 entries x u16[2] */
    if (!rom_read_at(r->rom, r->corners_addr, c, sizeof c)) return false;
    if (!rom_read_at(r->rom, r->dims_addr,    d, sizeof d)) return false;
    for (int i = 0; i < 8; i++) {
      uint16_t x = rd16(c + i * 4), y = rd16(c + i * 4 + 2);
      uint16_t w = rd16(d + i * 4), h = rd16(d + i * 4 + 2);
      if (x >= r->grid_w || y >= r->grid_h) return false;
      if (w == 0 || h == 0 || w > r->grid_w || h > r->grid_h) return false;
    }
    return true;
  }
  uint8_t e[64];            /* 8 entries x 8 bytes */
  if (!rom_read_at(r->rom, r->entries_addr, e, sizeof e)) return false;
  for (int i = 0; i < 8; i++) {
    const uint8_t* p = e + i * 8;
    if (p[0] >= 32 || p[1] >= 24) return false;              /* x, y            */
    if (p[2] == 0 || p[3] == 0 || p[2] > 16 || p[3] > 16) return false; /* w, h  */
    if (!rom_ptr_ok(r->rom, rd32(p + 4))) return false;      /* name pointer    */
  }
  return true;
}

static bool validate(RgnMap* r, uint32_t gfx) {
  if (!rom_ptr_ok(r->rom, gfx)) return false;
  derive(r, gfx);
  if (mr_lz77_size(r->rom, r->tiles_addr) != r->tiles_bytes) return false;
  for (int v = 0; v < r->view_count; v++)
    if (mr_lz77_size(r->rom, r->tilemap_addr[v]) != r->tilemap_bytes) return false;
  if (!rom_ptr_ok(r->rom, r->grid_addr[r->view_count - 1])) return false;
  if (!grid_ok(r)) return false;
  if (!rects_ok(r)) return false;
  return true;
}

/* Bounded signature scan for the tile stream: the exact 4-byte LZ77 header
 * (0x10 | size<<8), 4-aligned, within +/-128 KB of the table hint. Covers any revision
 * shift by three orders of magnitude (the largest real one is 0x70) at ~512 reads.
 * Every candidate still has to pass the full structural validation above. */
#define SCAN_HALF_WINDOW 0x20000u
static bool scan_for_gfx(RgnMap* r, uint32_t hint) {
  uint8_t want[4];
  want[0] = 0x10;
  want[1] = (uint8_t)(r->tiles_bytes & 0xFF);
  want[2] = (uint8_t)((r->tiles_bytes >> 8) & 0xFF);
  want[3] = (uint8_t)((r->tiles_bytes >> 16) & 0xFF);

  uint32_t lo = (hint > ROM_BASE + SCAN_HALF_WINDOW) ? hint - SCAN_HALF_WINDOW : ROM_BASE;
  uint32_t hi = hint + SCAN_HALF_WINDOW;
  uint32_t end = ROM_BASE + r->rom->size;
  if (hi > end) hi = end;
  lo &= ~3u;

  uint8_t buf[512];
  for (uint32_t a = lo; a + sizeof buf <= hi; a += sizeof buf) {
    if (!rom_read_at(r->rom, a, buf, sizeof buf)) return false;
    for (uint32_t i = 0; i + 4 <= sizeof buf; i += 4) {
      if (buf[i] == want[0] && buf[i+1] == want[1] &&
          buf[i+2] == want[2] && buf[i+3] == want[3]) {
        if (validate(r, a + i)) return true;
      }
    }
  }
  return false;
}

bool rgn_open(RgnMap* r, const RomCtx* rom) {
  if (!r || !rom || rom->kind == ROM_NONE || !rom->read) { if (r) memset(r, 0, sizeof *r); return false; }
  if (!fill_family(r, rom->kind)) { memset(r, 0, sizeof *r); return false; }
  r->rom = rom;

  uint32_t hint = 0;
  /* exact (kind, revision) match first */
  for (int i = 0; i < ANCHOR_COUNT; i++) {
    if (ANCHORS[i].kind == rom->kind && ANCHORS[i].ver == rom->version) {
      hint = ANCHORS[i].gfx;
      if (validate(r, hint)) return true;
      break;
    }
  }
  /* any anchor of the same game, then any anchor of the same family */
  for (int i = 0; i < ANCHOR_COUNT; i++) {
    if (ANCHORS[i].kind != rom->kind) continue;
    if (!hint) hint = ANCHORS[i].gfx;
    if (validate(r, ANCHORS[i].gfx)) return true;
  }
  if (!hint) {
    for (int i = 0; i < ANCHOR_COUNT; i++) {
      RgnMap probe; if (!fill_family(&probe, ANCHORS[i].kind)) continue;
      if (probe.family != r->family) continue;
      hint = ANCHORS[i].gfx; break;
    }
  }
  if (hint && scan_for_gfx(r, hint)) return true;

  memset(r, 0, sizeof *r);
  return false;
}

const char* rgn_view_name(const RgnMap* r, int view) {
  if (!r || view < 0 || view >= r->view_count) return "";
  if (r->family != RGN_FAM_FRLG) return "HOENN";
  switch (view) {
    case RGN_VIEW_MAIN:     return "KANTO";
    case RGN_VIEW_SEVII123: return "SEVII 1-3";
    case RGN_VIEW_SEVII45:  return "SEVII 4-5";
    default:                return "SEVII 6-7";
  }
}

/* ---- getting the pixels in -------------------------------------------------- */

bool rgn_stream(const RgnMap* r, int what, int view,
                uint32_t* out_addr, uint32_t* out_max_bytes) {
  if (!r || !r->rom || !out_addr || !out_max_bytes) return false;
  uint32_t addr, len;
  if (what == RGN_STREAM_TILES) {
    addr = r->tiles_addr;
    len  = (r->family == RGN_FAM_FRLG) ? FR_TILES_STREAM : RSE_TILES_STREAM;
  } else if (what == RGN_STREAM_TILEMAP) {
    if (view < 0 || view >= r->view_count) return false;
    addr = r->tilemap_addr[view];
    len  = (r->family == RGN_FAM_FRLG) ? FR_TILEMAP_STREAM[view] : RSE_TILEMAP_STREAM;
  } else return false;

  len += 16;                                  /* slack */
  uint32_t end = ROM_BASE + r->rom->size;
  if (addr >= end) return false;
  if (addr + len > end) len = end - addr;
  *out_addr = addr; *out_max_bytes = len;
  return true;
}

uint32_t rgn_tiles(const RgnMap* r, uint8_t* dst, uint32_t cap) {
  if (!r || !r->rom || !dst || cap < r->tiles_bytes) return 0;
  return mr_lz77(r->rom, r->tiles_addr, dst, cap);
}

uint32_t rgn_tilemap(const RgnMap* r, int view, uint8_t* dst, uint32_t cap) {
  if (!r || !r->rom || !dst || view < 0 || view >= r->view_count) return 0;
  if (cap < r->tilemap_bytes) return 0;
  return mr_lz77(r->rom, r->tilemap_addr[view], dst, cap);
}

/* NOTE for anyone comparing this against the decomp: on RSE the palette SYMBOL is only
 * 0x40 bytes (32 colours) but LoadRegionMapGfx asks for 3 * PLTT_SIZE_4BPP = 0x60, so
 * the retail game over-reads 32 bytes of the tile stream that follows it into PALRAM
 * entries 144..159. We copy the same 48 colours so PALRAM matches the real screen byte
 * for byte. The garbage is never visible: the genuine 32 colours cover indices 112..143
 * and no tile byte exceeds 140 (measured over all 14,912 of them). */
bool rgn_palette(const RgnMap* r, uint16_t* dst) {
  if (!r || !r->rom || !dst) return false;
  for (int i = 0; i < RGN_PAL_ENTRIES; i++) dst[i] = 0;
  uint8_t buf[64];
  uint32_t total = (uint32_t)r->pal_colours * 2, done = 0;
  while (done < total) {
    uint32_t n = total - done; if (n > sizeof buf) n = sizeof buf;
    if (!rom_read_at(r->rom, r->pal_addr + done, buf, n)) return false;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
      uint32_t idx = r->pal_base + (done + i) / 2;
      if (idx < RGN_PAL_ENTRIES) dst[idx] = rd16(buf + i);
    }
    done += n;
  }
  return true;
}

void rgn_attach(RgnMap* r, const uint8_t* tiles, const uint8_t* tilemap,
                const uint16_t* pal, int view) {
  if (!r) return;
  r->tiles = tiles; r->tilemap = tilemap; r->pal = pal;
  r->attached_view = (uint8_t)((view < 0 || view >= r->view_count) ? 0 : view);
}

/* ---- rendering -------------------------------------------------------------- */

/* One source pixel -> BGR555. Split out so the two colour depths stay readable; the
 * bounds test is the caller's. */
static uint16_t src_pixel(const RgnMap* r, int tx, int ty) {
  int tcol = tx >> 3, trow = ty >> 3;
  if (r->bpp == 8) {
    /* affine BG: one BYTE per tilemap entry, and the tile byte is a direct index into
     * the 256-colour BG palette (which is why pal_base 112 matters). */
    uint32_t tile = r->tilemap[(uint32_t)trow * r->map_tw + tcol];
    uint32_t off  = tile * 64u + (uint32_t)(ty & 7) * 8u + (uint32_t)(tx & 7);
    if (off >= r->tiles_bytes) return RGN_BACKDROP;
    return r->pal[r->tiles[off]];
  }
  /* text BG: u16 entry = tile(0-9) | hflip(10) | vflip(11) | palette bank(12-15) */
  const uint8_t* e = r->tilemap + ((uint32_t)trow * r->map_tw + tcol) * 2u;
  uint16_t ent = rd16(e);
  uint32_t tile = ent & 0x3FFu;
  int ix = tx & 7, iy = ty & 7;
  if (ent & 0x0400u) ix = 7 - ix;
  if (ent & 0x0800u) iy = 7 - iy;
  uint32_t off = tile * 32u + (uint32_t)iy * 4u + (uint32_t)(ix >> 1);
  if (off >= r->tiles_bytes) return RGN_BACKDROP;
  uint8_t byte = r->tiles[off];
  uint8_t ci = (ix & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
  uint32_t pi = (uint32_t)(ent >> 12) * 16u + ci;
  return (pi < RGN_PAL_ENTRIES) ? r->pal[pi] : RGN_BACKDROP;
}

bool rgn_render_at(const RgnMap* r, int src_x, int src_y, int scale,
                   int w, int h, uint16_t* dst, int stride) {
  if (!r || !r->tiles || !r->tilemap || !r->pal || !dst) return false;
  if (scale != 1 && scale != 2) return false;
  if (w <= 0 || h <= 0 || stride < w) return false;

  const int img_w = (int)r->map_tw * 8, img_h = (int)r->map_th * 8;
  const int sh = (scale == 2) ? 1 : 0;

  for (int py = 0; py < h; py++) {
    int ty = src_y + (py >> sh);
    uint16_t* row = dst + (size_t)py * (size_t)stride;
    if (ty < 0 || ty >= img_h) {
      for (int px = 0; px < w; px++) row[px] = RGN_BACKDROP;
      continue;
    }
    for (int px = 0; px < w; px++) {
      int tx = src_x + (px >> sh);
      row[px] = (tx < 0 || tx >= img_w) ? RGN_BACKDROP : src_pixel(r, tx, ty);
    }
  }
  return true;
}

bool rgn_render(const RgnMap* r, uint16_t* dst, int stride) {
  return rgn_render_at(r, 0, 0, 1, 240, 160, dst, stride);
}

/* ---- the zoom relationship -------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void rgn_zoom_origin(const RgnMap* r, int gx, int gy, int* src_x, int* src_y) {
  int x = 0, y = 0;
  if (r) {
    if (r->family == RGN_FAM_FRLG) {
      /* not a retail mode: centre the cell in a 120x80 window over the 240x160 image */
      x = clampi(r->grid_x0 + gx * 8 + 4 - 60, 0, 240 - 120);
      y = clampi(r->grid_y0 + gy * 8 + 4 - 40, 0, 160 -  80);
    } else {
      /* retail: scrollX = cursorX*8 - 0x34 with cursorX = gx + MAPCURSOR_X_MIN(1),
       * then the affine origin adds 0x1C; likewise 0x44/0x24 with Y_MIN 2.
       * Clamps are the game's own ProcessRegionMapInput_Zoomed bounds. */
      x = clampi(gx * 8 - 16, -16, 200);
      y = clampi(gy * 8 - 16, -16,  96);
    }
  }
  if (src_x) *src_x = x;
  if (src_y) *src_y = y;
}

void rgn_grid_to_px(const RgnMap* r, int gx, int gy, int* px, int* py) {
  if (px) *px = r ? r->grid_x0 + gx * 8 : 0;
  if (py) *py = r ? r->grid_y0 + gy * 8 : 0;
}

/* ---- sections --------------------------------------------------------------- */

uint32_t rgn_grid_bytes(const RgnMap* r) {
  if (!r) return 0;
  return (uint32_t)r->layer_count * r->grid_h * r->grid_w;
}

bool rgn_grid(const RgnMap* r, int view, uint8_t* dst, uint32_t cap) {
  if (!r || !r->rom || !dst || view < 0 || view >= r->view_count) return false;
  uint32_t n = rgn_grid_bytes(r);
  if (cap < n) return false;
  return rom_read_at(r->rom, r->grid_addr[view], dst, n);
}

bool rgn_section_at_layer(const RgnMap* r, int view, int layer, int gx, int gy,
                          uint8_t* out_mapsec) {
  if (out_mapsec) *out_mapsec = r ? (uint8_t)r->mapsec_none : 0;
  if (!r || !r->rom) return false;
  if (view < 0 || view >= r->view_count) return false;
  if (layer < 0 || layer >= r->layer_count) return false;
  if (gx < 0 || gy < 0 || gx >= r->grid_w || gy >= r->grid_h) return false;
  uint32_t off = ((uint32_t)layer * r->grid_h + (uint32_t)gy) * r->grid_w + (uint32_t)gx;
  uint8_t v;
  if (!rom_read_at(r->rom, r->grid_addr[view] + off, &v, 1)) return false;
  if (out_mapsec) *out_mapsec = v;
  return true;
}

bool rgn_section_at(const RgnMap* r, int view, int gx, int gy, uint8_t* out_mapsec) {
  return rgn_section_at_layer(r, view, RGN_LAYER_MAP, gx, gy, out_mapsec);
}

bool rgn_mapsec_rect(const RgnMap* r, uint8_t mapsec, int* x, int* y, int* w, int* h) {
  if (!r || !r->rom) return false;
  int rx, ry, rw, rh;
  if (r->family == RGN_FAM_FRLG) {
    if (mapsec < r->mapsec_first) return false;
    uint32_t i = (uint32_t)mapsec - r->mapsec_first;
    if (i >= r->mapsec_count) return false;
    uint8_t c[4], d[4];
    if (!rom_read_at(r->rom, r->corners_addr + i * 4u, c, 4)) return false;
    if (!rom_read_at(r->rom, r->dims_addr    + i * 4u, d, 4)) return false;
    rx = rd16(c); ry = rd16(c + 2); rw = rd16(d); rh = rd16(d + 2);
  } else {
    if (mapsec >= r->mapsec_count) return false;
    uint8_t e[8];
    if (!rom_read_at(r->rom, r->entries_addr + (uint32_t)mapsec * 8u, e, 8)) return false;
    rx = e[0]; ry = e[1]; rw = e[2]; rh = e[3];
  }
  /* A zero-sized rect means "this id has no place on the map" (FRLG's
   * MAPSEC_SPECIAL_AREA is the Celadon Dept. Store label). Refuse it rather than let
   * the UI draw a degenerate marker. */
  if (rw == 0 || rh == 0) return false;
  if (rx >= r->grid_w || ry >= r->grid_h) return false;
  if (x) *x = rx;
  if (y) *y = ry;
  if (w) *w = rw;
  if (h) *h = rh;
  return true;
}

bool rgn_find_mapsec(const RgnMap* r, uint8_t mapsec,
                     int* view, int* layer, int* gx, int* gy) {
  if (view) *view = RGN_VIEW_MAIN;
  if (layer) *layer = RGN_LAYER_MAP;
  if (gx) *gx = -1;
  if (gy) *gy = -1;
  if (!r || !r->rom) return false;
  if (mapsec == (uint8_t)r->mapsec_none) return false;

  const uint32_t plane = (uint32_t)r->grid_w * r->grid_h;
  for (int v = 0; v < r->view_count; v++) {
    uint8_t buf[64];
    uint32_t total = rgn_grid_bytes(r), done = 0;   /* every plane of this view */
    while (done < total) {
      uint32_t n = total - done; if (n > sizeof buf) n = sizeof buf;
      if (!rom_read_at(r->rom, r->grid_addr[v] + done, buf, n)) return false;
      for (uint32_t i = 0; i < n; i++) {
        if (buf[i] != mapsec) continue;
        uint32_t off = done + i;
        if (view)  *view  = v;
        if (layer) *layer = (int)(off / plane);
        if (gx)    *gx    = (int)((off % plane) % r->grid_w);
        if (gy)    *gy    = (int)((off % plane) / r->grid_w);
        return true;
      }
      done += n;
    }
  }
  return false;
}

bool rgn_view_for_mapsec(const RgnMap* r, uint8_t mapsec, int* out_view) {
  return rgn_find_mapsec(r, mapsec, out_view, 0, 0, 0);
}

bool rgn_mapsec_name_raw(const RgnMap* r, uint8_t mapsec, uint8_t* dst, int cap) {
  if (!r || !r->rom || !dst || cap <= 0) return false;
  if (r->family == RGN_FAM_FRLG) return false;      /* names live elsewhere on FRLG */
  if (mapsec >= r->mapsec_count) return false;
  uint8_t e[8];
  if (!rom_read_at(r->rom, r->entries_addr + (uint32_t)mapsec * 8u, e, 8)) return false;
  uint32_t name = rd32(e + 4);
  if (!rom_ptr_ok(r->rom, name)) return false;

  /* Longest raw name measured across every entry in Emerald and Ruby is 18 bytes; 48
   * covers it with the 0xFC pairs still in. One read, then filter. */
  uint8_t buf[48];
  uint32_t n = sizeof buf;
  uint32_t end = ROM_BASE + r->rom->size;
  if (name + n > end) n = end - name;
  if (n == 0 || !rom_read_at(r->rom, name, buf, n)) return false;

  int o = 0;
  for (uint32_t i = 0; i < n && o < cap - 1; i++) {
    uint8_t c = buf[i];
    if (c == 0xFF) break;
    /* EXT_CTRL_CODE_BEGIN. Ruby/Sapphire put {NAME_END} (FC 00) in the middle of 16 of
     * their 88 names — "LITTLEROOT" FC 00 <space> "TOWN" — where Emerald just has the
     * space. Drop the 2-byte sequence and the name reads correctly on all three.
     * MEASURED: FC 00 is the only control code any region-map name uses. */
    if (c == 0xFC) { i++; continue; }
    dst[o++] = c;
  }
  dst[o] = 0xFF;
  return true;
}
