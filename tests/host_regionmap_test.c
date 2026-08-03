/* Host (PC) test for region_map — the Gen-3 REGION MAP reader, run against REAL retail
 * ROMs. region_map does all its I/O through the same RomReadFn callback rom_map uses,
 * so the code exercised here is byte-for-byte the code that runs on the GBA over FatFs.
 *
 * What it proves:
 *   1) rgn_open finds and STRUCTURALLY VALIDATES the region-map data in Emerald,
 *      Ruby and FireRed — LZ77 headers declaring the exact expected sizes, a mapsec
 *      grid in range, and a decodable rectangle table;
 *   2) the per-family geometry is right (8bpp/64x64 affine on RSE, 4bpp/30x20 text on
 *      FRLG) and the four FRLG views all decompress;
 *   3) ground truth from pret's data: Littleroot at grid (4,11), Fallarbor at (3,0),
 *      Ever Grande as a 1x2 rect, Pallet at (4,11), Indigo Plateau at grid (2,3),
 *      Mt. Moon only on FRLG's LAYER_DUNGEON plane, One Island only on the Sevii 1-3
 *      view — i.e. the grid, the rect table and the view split all agree;
 *   4) the RSE name pointer really lands on "LITTLEROOT TOWN" in the Gen-3 charset —
 *      which is what catches the {x,y,w,h,name} vs {name,x,y,w,h} field-order trap;
 *   5) the whole-region and 2x-zoom renders produce real pixels, dumped as PNGs for a
 *      HUMAN to look at. Parsing "succeeding" proves nothing about a map renderer;
 *      the acceptance criterion is that the PNG looks like Hoenn/Kanto.
 *
 * ROMs are the user's own dumps and are NEVER part of this repo. Missing ROMs SKIP.
 *
 * Build + run (from the repo root) — note map_render.c is needed for mr_lz77:
 *   cc -I source tests/host_regionmap_test.c source/region_map.c source/rom_map.c \
 *      source/map_render.c -o /tmp/hrm && /tmp/hrm
 *   ... or pass emerald, ruby, firered paths as argv[1..3], and a PNG output
 *   directory as argv[4] (default /tmp).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_map.h"
#include "region_map.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)
#define CHECK_EQ(got, want, msg) do { long g_=(long)(got), w_=(long)(want); \
  if (g_ != w_) { printf("FAIL: %s (got %ld, want %ld)\n", msg, g_, w_); g_fail++; } } while (0)

static const char* g_outdir = "/tmp";

/* ---- the read callback: plain fread, the host analogue of FatFs ------------- */
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, f) == len;
}
static uint32_t file_size(FILE* f) {
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  return (uint32_t)(n < 0 ? 0 : n);
}

/* ---- a dependency-free PNG writer (stored deflate, so no zlib link) --------- */
static uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t crc) {
  static uint32_t tab[256]; static int init = 0;
  if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
      tab[i] = c; } init = 1; }
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
static void put_be32(FILE* f, uint32_t v, uint32_t* crc) {
  uint8_t b[4] = { (uint8_t)(v>>24), (uint8_t)(v>>16), (uint8_t)(v>>8), (uint8_t)v };
  fwrite(b, 1, 4, f); if (crc) *crc = crc32_of(b, 4, *crc);
}
static void chunk(FILE* f, const char* tag, const uint8_t* data, uint32_t n) {
  put_be32(f, n, NULL);
  uint32_t crc = 0;
  crc = crc32_of((const uint8_t*)tag, 4, crc);
  fwrite(tag, 1, 4, f);
  if (n) { crc = crc32_of(data, n, crc); fwrite(data, 1, n, f); }
  put_be32(f, crc, NULL);
}
/* BGR555 -> PNG. Returns 0 on success. */
static int write_png(const char* path, int w, int h, const uint16_t* px, int stride) {
  FILE* f = fopen(path, "wb");
  if (!f) return -1;
  static const uint8_t sig[8] = { 0x89,'P','N','G',13,10,26,10 };
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13] = { (uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w,
                       (uint8_t)(h>>24),(uint8_t)(h>>16),(uint8_t)(h>>8),(uint8_t)h,
                       8, 2, 0, 0, 0 };
  chunk(f, "IHDR", ihdr, 13);

  size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
  uint8_t* raw = (uint8_t*)malloc(raw_n);
  size_t o = 0;
  for (int y = 0; y < h; y++) {
    raw[o++] = 0;
    for (int x = 0; x < w; x++) {
      uint16_t c = px[(size_t)y * stride + x];
      raw[o++] = (uint8_t)(((c        & 31) * 255) / 31);
      raw[o++] = (uint8_t)((((c >> 5) & 31) * 255) / 31);
      raw[o++] = (uint8_t)((((c >>10) & 31) * 255) / 31);
    }
  }
  /* zlib stream with stored deflate blocks */
  size_t blocks = (raw_n + 65534) / 65535;
  size_t z_n = 2 + blocks * 5 + raw_n + 4;
  uint8_t* z = (uint8_t*)malloc(z_n);
  size_t zo = 0;
  z[zo++] = 0x78; z[zo++] = 0x01;
  size_t left = raw_n, at = 0;
  while (left) {
    uint16_t n = (uint16_t)(left > 65535 ? 65535 : left);
    z[zo++] = (uint8_t)((left == n) ? 1 : 0);
    z[zo++] = (uint8_t)n; z[zo++] = (uint8_t)(n >> 8);
    z[zo++] = (uint8_t)~n; z[zo++] = (uint8_t)(~n >> 8);
    memcpy(z + zo, raw + at, n); zo += n; at += n; left -= n;
  }
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  uint32_t adler = (b << 16) | a;
  z[zo++] = (uint8_t)(adler>>24); z[zo++] = (uint8_t)(adler>>16);
  z[zo++] = (uint8_t)(adler>>8);  z[zo++] = (uint8_t)adler;
  chunk(f, "IDAT", z, (uint32_t)zo);
  chunk(f, "IEND", NULL, 0);
  fclose(f); free(raw); free(z);
  return 0;
}

/* ---- Gen-3 charset, just enough to read a place name -----------------------
 * Test-local on purpose: region_map.c must not depend on gen3_save.c. Region-map
 * names use letters, digits, space, '.', '-', apostrophes and 0x1B ('e' with an
 * accent, as in POKeMON LEAGUE), plus one 0xFD placeholder (STRING_VAR + arg) in the
 * empty MAPSEC_NONE slot. */
static void decode_name(const uint8_t* raw, char* out, int cap) {
  int i = 0, o = 0;
  for (; o < cap - 1 && raw[i] != 0xFF; i++) {
    uint8_t c = raw[i];
    if (c == 0xFD) { i++; continue; }              /* placeholder + its argument */
    if (c == 0x00) out[o++] = ' ';
    else if (c >= 0xBB && c <= 0xD4) out[o++] = (char)('A' + (c - 0xBB));
    else if (c >= 0xD5 && c <= 0xEE) out[o++] = (char)('a' + (c - 0xD5));
    else if (c >= 0xA1 && c <= 0xAA) out[o++] = (char)('0' + (c - 0xA1));
    else if (c == 0x1B) out[o++] = 'e';            /* accented e */
    else if (c == 0xAB) out[o++] = '!';
    else if (c == 0xAC) out[o++] = '?';
    else if (c == 0xAD || c == 0xB0) out[o++] = '.';
    else if (c == 0xAE) out[o++] = '-';
    else if (c >= 0xB1 && c <= 0xB4) out[o++] = '\'';
    else if (c == 0xB8) out[o++] = ',';
    else if (c == 0xBA) out[o++] = '/';
    else out[o++] = '?';
  }
  out[o] = 0;
}

/* Draw a 1px magenta box around a grid rect, in RENDER space, so the PNG shows whether
 * the rect table and the image actually line up. */
static void mark(const RgnMap* r, uint16_t* fb, int stride, int fw, int fh,
                 int gx, int gy, int gw, int gh, int src_x, int src_y, int scale,
                 uint16_t colour) {
  int px, py; rgn_grid_to_px(r, gx, gy, &px, &py);
  int x0 = (px - src_x) * scale,          y0 = (py - src_y) * scale;
  int x1 = x0 + gw * 8 * scale - 1,       y1 = y0 + gh * 8 * scale - 1;
  for (int x = x0; x <= x1; x++) {
    if (x < 0 || x >= fw) continue;
    if (y0 >= 0 && y0 < fh) fb[(size_t)y0 * stride + x] = colour;
    if (y1 >= 0 && y1 < fh) fb[(size_t)y1 * stride + x] = colour;
  }
  for (int y = y0; y <= y1; y++) {
    if (y < 0 || y >= fh) continue;
    if (x0 >= 0 && x0 < fw) fb[(size_t)y * stride + x0] = colour;
    if (x1 >= 0 && x1 < fw) fb[(size_t)y * stride + x1] = colour;
  }
}

/* ---------------------------------------------------------------------------- */

typedef struct { uint8_t mapsec; int x, y, w, h; const char* what; } RectTruth;

static void test_rom(const char* path, const char* tag, RomKind expect_kind,
                     RgnFamily expect_family, int expect_views, int expect_mapsecs,
                     const RectTruth* truth, int truth_n) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("(skip: %s not found)\n", path); return; }

  RomCtx rom;
  if (!rom_open(&rom, file_read, f, file_size(f))) {
    printf("FAIL: rom_open rejected %s\n", path); g_fail++; fclose(f); return;
  }
  printf("\n=== %s  [%s rev %u]  %s\n", tag, rom.code, rom.version, rom_kind_name(rom.kind));
  CHECK(rom.kind == expect_kind, "ROM identified as expected");

  RgnMap r;
  if (!rgn_open(&r, &rom)) {
    printf("FAIL: rgn_open could not locate/validate the region map in %s\n", path);
    g_fail++; fclose(f); return;
  }
  printf("  tiles @%08X (%u B, %ubpp)  tilemap @%08X (%u B, %ux%u tiles, %u B/entry)\n",
         r.tiles_addr, r.tiles_bytes, r.bpp, r.tilemap_addr[0], r.tilemap_bytes,
         r.map_tw, r.map_th, r.tilemap_entry_bytes);
  printf("  palette @%08X (%u colours at PALRAM %u)  grid %ux%u at px (%u,%u)  views=%u layers=%u\n",
         r.pal_addr, r.pal_colours, r.pal_base, r.grid_w, r.grid_h,
         r.grid_x0, r.grid_y0, r.view_count, r.layer_count);
  printf("  mapsec ids %u..%u  (NONE = %u)   rect table @%08X%s\n",
         r.mapsec_first, r.mapsec_first + r.mapsec_count - 1, r.mapsec_none,
         r.entries_addr ? r.entries_addr : r.corners_addr,
         r.entries_addr ? " (gRegionMapEntries)" : " (corners) + dims");

  CHECK(r.family == expect_family, "region-map family");
  CHECK_EQ(r.view_count, expect_views, "view count");
  CHECK_EQ(r.mapsec_count, expect_mapsecs, "mapsec count");

  /* ---- decompress everything ---- */
  static uint8_t tiles[RGN_MAX_TILES_BYTES];
  static uint8_t tmap[RGN_MAX_TILEMAP_BYTES];
  static uint16_t pal[RGN_PAL_ENTRIES];
  uint32_t nt = rgn_tiles(&r, tiles, sizeof tiles);
  CHECK_EQ(nt, r.tiles_bytes, "tile data decompresses to the declared size");
  CHECK(rgn_palette(&r, pal), "palette reads");

  for (int v = 0; v < r.view_count; v++) {
    uint32_t nm = rgn_tilemap(&r, v, tmap, sizeof tmap);
    if (nm != r.tilemap_bytes) {
      printf("FAIL: view %d (%s) tilemap decompressed to %u, want %u\n",
             v, rgn_view_name(&r, v), nm, r.tilemap_bytes);
      g_fail++;
    }
  }
  CHECK(rgn_tilemap(&r, r.view_count, tmap, sizeof tmap) == 0,
        "a view index past the end is refused, not rendered as garbage");
  CHECK(rgn_tiles(&r, tiles, r.tiles_bytes - 1) == 0, "an undersized buffer is refused");

  /* the BIOS-path stream bounds must at least cover the real stream */
  uint32_t sa, sn;
  CHECK(rgn_stream(&r, RGN_STREAM_TILES, 0, &sa, &sn), "tile stream bounds");
  CHECK(sa == r.tiles_addr && sn > 1024 && sn < 8192, "tile stream bound is sane");

  /* ---- ground truth: rectangles ---- */
  for (int i = 0; i < truth_n; i++) {
    int x, y, w, h;
    if (!rgn_mapsec_rect(&r, truth[i].mapsec, &x, &y, &w, &h)) {
      printf("FAIL: no rect for mapsec %u (%s)\n", truth[i].mapsec, truth[i].what);
      g_fail++; continue;
    }
    if (x != truth[i].x || y != truth[i].y || w != truth[i].w || h != truth[i].h) {
      printf("FAIL: %s rect = (%d,%d %dx%d), want (%d,%d %dx%d)\n",
             truth[i].what, x, y, w, h, truth[i].x, truth[i].y, truth[i].w, truth[i].h);
      g_fail++;
    } else {
      printf("  rect %-22s mapsec %3u -> (%2d,%2d) %dx%d\n",
             truth[i].what, truth[i].mapsec, x, y, w, h);
    }
  }

  /* ---- the grid and the rect table must agree ---- */
  {
    static uint8_t grid[2 * 32 * 16];
    CHECK(rgn_grid(&r, 0, grid, sizeof grid), "grid reads in one go");
    int agree = 0, checked = 0;
    for (int i = 0; i < truth_n; i++) {
      int x, y, w, h;
      if (!rgn_mapsec_rect(&r, truth[i].mapsec, &x, &y, &w, &h)) continue;
      uint8_t at = 0;
      if (!rgn_section_at(&r, 0, x, y, &at)) continue;
      checked++;
      if (at == truth[i].mapsec) agree++;
    }
    printf("  %d/%d truth rects land on their own mapsec in the grid\n", agree, checked);
    CHECK(agree >= checked - 2, "rect table and grid agree (a couple of dungeon/overlay "
                                "ids legitimately do not)");
  }

  /* out-of-range access must fail cleanly rather than read garbage */
  {
    uint8_t s = 0;
    CHECK(!rgn_section_at(&r, 0, -1, 0, &s), "negative grid x refused");
    CHECK(!rgn_section_at(&r, 0, r.grid_w, 0, &s), "grid x past the edge refused");
    CHECK(!rgn_section_at(&r, 0, 0, r.grid_h, &s), "grid y past the edge refused");
    CHECK(s == (uint8_t)r.mapsec_none, "a refused lookup yields MAPSEC_NONE");
  }

  /* ---- names (RSE only) ---- */
  if (r.family != RGN_FAM_FRLG) {
    uint8_t raw[RGN_NAME_MAX]; char name[RGN_NAME_MAX];
    CHECK(rgn_mapsec_name_raw(&r, 0, raw, sizeof raw), "mapsec 0 name reads");
    decode_name(raw, name, sizeof name);
    printf("  mapsec 0 name = \"%s\"", name);
    CHECK(strcmp(name, "LITTLEROOT TOWN") == 0,
          "the name pointer really is the LAST field of RegionMapLocation");
    /* every name must decode cleanly — this is what catches Ruby's embedded FC 00 */
    int bad = 0;
    for (int m = 0; m < (int)r.mapsec_count; m++) {
      if (!rgn_mapsec_name_raw(&r, (uint8_t)m, raw, sizeof raw)) { bad++; continue; }
      decode_name(raw, name, sizeof name);
      for (const char* q = name; *q; q++) if (*q == '?') { bad++; break; }
      if (m == 16) printf("   [16] = \"%s\"", name);
      if (m == 55) printf("   [55] = \"%s\"", name);
    }
    printf("\n");
    CHECK_EQ(bad, 0, "every region-map name decodes without an unknown byte");
  } else {
    uint8_t raw[24];
    CHECK(!rgn_mapsec_name_raw(&r, 88, raw, sizeof raw),
          "FRLG names are honestly refused rather than faked");
  }

  /* ---- render every view, both levels, and dump PNGs for a human ---- */
  static uint16_t fb[240 * 160];
  char p[512];
  for (int v = 0; v < r.view_count; v++) {
    CHECK(rgn_tilemap(&r, v, tmap, sizeof tmap) == r.tilemap_bytes, "tilemap for view");
    rgn_attach(&r, tiles, tmap, pal, v);

    CHECK(rgn_render(&r, fb, 240), "whole-region render");
    /* not-all-one-colour is the cheapest proof we drew something */
    int distinct = 0; uint16_t seen[16];
    for (int i = 0; i < 240 * 160 && distinct < 16; i++) {
      int k = 0; for (; k < distinct; k++) if (seen[k] == fb[i]) break;
      if (k == distinct) seen[distinct++] = fb[i];
    }
    CHECK(distinct >= 8, "the render has real image content, not one flat colour");

    /* Mark the truth rects so the PNG proves the coordinate mapping too — but only on
     * the view that actually contains the mapsec, or FRLG's Sevii screens get Kanto's
     * boxes stamped on empty ocean. */
    for (int i = 0; i < truth_n; i++) {
      int x, y, w, h, onview = RGN_VIEW_MAIN;
      if (!rgn_mapsec_rect(&r, truth[i].mapsec, &x, &y, &w, &h)) continue;
      if (rgn_view_for_mapsec(&r, truth[i].mapsec, &onview) && onview != v) continue;
      mark(&r, fb, 240, 240, 160, x, y, w, h, 0, 0, 1, 0x7C1F);   /* magenta */
    }
    snprintf(p, sizeof p, "%s/rgn_%s_%s_full.png", g_outdir, tag, rgn_view_name(&r, v));
    for (char* q = p; *q; q++) if (*q == ' ') *q = '_';
    CHECK(write_png(p, 240, 160, fb, 240) == 0, "PNG written");
    printf("  wrote %s\n", p);

    if (v == 0) {
      /* the zoomed level, seeded exactly the way the game seeds it */
      int gx = (r.family == RGN_FAM_FRLG) ? 4 : 4;
      int gy = (r.family == RGN_FAM_FRLG) ? 11 : 11;   /* Pallet / Littleroot */
      int sx, sy;
      rgn_zoom_origin(&r, gx, gy, &sx, &sy);
      CHECK(rgn_render_at(&r, sx, sy, 2, 240, 160, fb, 240), "2x zoom render");
      mark(&r, fb, 240, 240, 160, gx, gy, 1, 1, sx, sy, 2, 0x7C1F);
      snprintf(p, sizeof p, "%s/rgn_%s_zoom.png", g_outdir, tag);
      CHECK(write_png(p, 240, 160, fb, 240) == 0, "PNG written");
      printf("  wrote %s   (zoom origin %d,%d — cell (%d,%d) centred at screen 56,72)\n",
             p, sx, sy, gx, gy);

      CHECK(!rgn_render_at(&r, 0, 0, 3, 240, 160, fb, 240), "scale 3 refused");
      CHECK(!rgn_render_at(&r, 0, 0, 1, 240, 160, fb, 100), "stride < width refused");
    }
  }
  rgn_attach(&r, NULL, NULL, NULL, 0);
  CHECK(!rgn_render(&r, fb, 240), "rendering without attached buffers is refused");

  fclose(f);
}

int main(int argc, char** argv) {
  const char* em = (argc > 1) ? argv[1]
      : "/Users/guyshtainer/Desktop/swtich sd backup new/roms/gba/Pokemon - Emerald Version.gba";
  const char* rb = (argc > 2) ? argv[2] : "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_RUBY_AXVE02.gba";
  const char* fr = (argc > 3) ? argv[3] : "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_FIRE_BPRE01.gba";
  if (argc > 4) g_outdir = argv[4];

  /* Ground truth is pret's src/data/region_map/region_map_sections.json. */
  static const RectTruth hoenn[] = {
    {  0,  4, 11, 1, 1, "LITTLEROOT TOWN"   },
    {  2,  2, 14, 1, 1, "DEWFORD TOWN"      },
    {  4,  3,  0, 1, 1, "FALLARBOR TOWN"    },
    { 14, 21,  7, 1, 1, "SOOTOPOLIS CITY"   },
    { 15, 27,  8, 1, 2, "EVER GRANDE CITY"  },   /* the only tall rect */
    { 16,  4, 10, 1, 1, "ROUTE 101"         },
    { 55,  1, 13, 1, 1, "GRANITE CAVE"      },   /* a cave: on the rect table, not the grid */
  };
  /* Ruby has the same first 88 mapsecs, minus Emerald-only additions. */
  static const RectTruth hoenn_rs[] = {
    {  0,  4, 11, 1, 1, "LITTLEROOT TOWN"   },
    {  2,  2, 14, 1, 1, "DEWFORD TOWN"      },
    {  4,  3,  0, 1, 1, "FALLARBOR TOWN"    },
    { 14, 21,  7, 1, 1, "SOOTOPOLIS CITY"   },
    { 15, 27,  8, 1, 2, "EVER GRANDE CITY"  },
    { 16,  4, 10, 1, 1, "ROUTE 101"         },
  };
  static const RectTruth kanto[] = {
    {  88,  4, 11, 1, 1, "PALLET TOWN"      },
    {  90,  4,  4, 1, 1, "PEWTER CITY"      },
    {  91, 14,  3, 1, 1, "CERULEAN CITY"    },
    {  92, 18,  6, 1, 1, "LAVENDER TOWN"    },
    {  94, 11,  6, 1, 1, "CELADON CITY"     },
    {  97,  2,  3, 1, 1, "INDIGO PLATEAU"   },
  };

  test_rom(em, "emerald", ROM_EMERALD, RGN_FAM_E,    1, 213, hoenn,    (int)(sizeof hoenn/sizeof hoenn[0]));
  test_rom(rb, "ruby",    ROM_RUBY,    RGN_FAM_RS,   1,  88, hoenn_rs, (int)(sizeof hoenn_rs/sizeof hoenn_rs[0]));
  test_rom(fr, "firered", ROM_FIRERED, RGN_FAM_FRLG, 4, 109, kanto,    (int)(sizeof kanto/sizeof kanto[0]));

  /* ---- FRLG specifics: dungeon plane and the Sevii view split ---- */
  {
    FILE* f = fopen(fr, "rb");
    if (f) {
      RomCtx rom; RgnMap r;
      if (rom_open(&rom, file_read, f, file_size(f)) && rgn_open(&r, &rom)) {
        printf("\n=== firered: layers and views\n");
        uint8_t s = 0;
        CHECK(rgn_section_at(&r, RGN_VIEW_MAIN, 2, 3, &s), "kanto grid reads");
        CHECK_EQ(s, 97, "grid (2,3) on the map plane is INDIGO PLATEAU");
        CHECK(rgn_section_at(&r, RGN_VIEW_MAIN, 14, 3, &s), "kanto grid reads");
        CHECK_EQ(s, 91, "grid (14,3) on the map plane is CERULEAN CITY");
        CHECK(rgn_section_at_layer(&r, RGN_VIEW_MAIN, RGN_LAYER_DUNGEON, 9, 3, &s),
              "kanto dungeon plane reads");
        CHECK_EQ(s, 127, "grid (9,3) on the DUNGEON plane is MT MOON");

        /* Mt. Moon's rect-table entry is a (0,0) placeholder; only the grid knows. */
        int x, y, w, h;
        bool got = rgn_mapsec_rect(&r, 127, &x, &y, &w, &h);
        printf("  MT MOON rect table -> %s(%d,%d)\n", got ? "" : "refused ", got?x:0, got?y:0);
        int view, layer, gx, gy;
        CHECK(rgn_find_mapsec(&r, 127, &view, &layer, &gx, &gy), "MT MOON found on a grid");
        CHECK_EQ(view, RGN_VIEW_MAIN, "MT MOON is on the Kanto view");
        CHECK_EQ(layer, RGN_LAYER_DUNGEON, "MT MOON is a dungeon-plane mapsec");
        CHECK(gx == 9 && gy == 3, "MT MOON grid cell");
        printf("  MT MOON via grid -> view %d layer %d cell (%d,%d)\n", view, layer, gx, gy);

        CHECK(rgn_view_for_mapsec(&r, 143, &view), "ONE ISLAND is on some view");
        CHECK_EQ(view, RGN_VIEW_SEVII123, "ONE ISLAND is on the Sevii 1-3 view");
        CHECK(rgn_view_for_mapsec(&r, 146, &view), "FOUR ISLAND is on some view");
        CHECK_EQ(view, RGN_VIEW_SEVII45, "FOUR ISLAND is on the Sevii 4-5 view");
        CHECK(rgn_view_for_mapsec(&r, 149, &view), "SIX ISLAND is on some view");
        CHECK_EQ(view, RGN_VIEW_SEVII67, "SIX ISLAND is on the Sevii 6-7 view");
        CHECK(rgn_view_for_mapsec(&r, 88, &view), "PALLET TOWN is on some view");
        CHECK_EQ(view, RGN_VIEW_MAIN, "PALLET TOWN is on the Kanto view");

        /* MAPSEC_SPECIAL_AREA is a 0x0 label, not a place */
        CHECK(!rgn_mapsec_rect(&r, 196, 0, 0, 0, 0), "a 0x0 rect is refused");
        CHECK(!rgn_mapsec_rect(&r, 87, 0, 0, 0, 0), "a mapsec below KANTO_MAPSEC_START is refused");
        CHECK(!rgn_mapsec_rect(&r, 250, 0, 0, 0, 0), "a mapsec past the end is refused");
      }
      fclose(f);
    }
  }

  /* ---- negatives: nothing but a supported retail ROM may open ---- */
  {
    RgnMap r; RomCtx rom;
    memset(&rom, 0, sizeof rom);
    CHECK(!rgn_open(&r, &rom), "a ROM_NONE RomCtx is refused");
    CHECK(!rgn_open(&r, NULL), "a NULL RomCtx is refused");
    CHECK(!rgn_open(NULL, &rom), "a NULL RgnMap is refused");
  }
  {
    /* A ROM that opens as Pokemon but whose region-map blob has been moved must not
     * silently render another game's bytes: simulate by validating against a ROM whose
     * kind is right but whose data is a different game's. */
    FILE* f = fopen(fr, "rb");
    if (f) {
      RomCtx rom;
      if (rom_open(&rom, file_read, f, file_size(f))) {
        rom.kind = ROM_EMERALD;         /* lie about the game */
        RgnMap r;
        CHECK(!rgn_open(&r, &rom),
              "Emerald's addresses applied to a FireRed image are REJECTED, not rendered");
      }
      fclose(f);
    }
  }

  printf("\n");
  if (g_fail == 0) printf("OK: host_regionmap_test (0 failures)\n");
  else             printf("host_regionmap_test: %d FAILURE(S)\n", g_fail);
  return g_fail ? 1 : 0;
}
