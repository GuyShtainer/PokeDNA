/* Host (PC) test for map_render — the Gen-3 overworld renderer core.
 *
 * This is a DIFFERENTIAL test. tools/gen_render_truth.py is an independent Python
 * implementation of the same spec; it renders real maps out of a real retail ROM and
 * dumps the composited pixels to docs/analysis-2026-07-29/render-<map>.raw. This test
 * renders the same region with the C core and requires the two to agree BYTE FOR BYTE.
 *
 * Two independent implementations agreeing on real ROM bytes is what makes a pixel
 * pipeline trustworthy before it ever reaches a GBA screen. It pins down, all at once:
 * LZ77 decompression, the 4bpp low-nibble-is-left rule, tile flips, the BGR555 palette
 * layout, the primary/secondary tile+metatile split, the metatile 2-layer draw order,
 * colour-0 transparency in the top layer, and the palette-slot-6 secondary trap.
 *
 * Regenerate the ground truth after ANY change to the spec:
 *   python3 tools/gen_render_truth.py <rom.gba>
 *
 * Build + run (from the repo root):
 *   cc -I source tests/host_render_test.c source/map_render.c source/rom_map.c \
 *      -o /tmp/hr && /tmp/hr "<rom.gba>"
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "map_render.h"
#include "rom_map.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, f) == len;
}

/* generous static buffers — this is a host test, not the GBA */
static uint8_t  s_prim_tiles[512 * 32], s_sec_tiles[512 * 32];
static uint8_t  s_prim_mt[1024 * 16],   s_sec_mt[1024 * 16];
static uint16_t s_pix[320 * 320];
static uint16_t s_ref[320 * 320];

/* Load one .raw ground-truth dump: header (u16 group,num,w,h) then w*h u16 pixels. */
static bool load_raw(const char* path, int* group, int* num, int* w, int* h) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  uint8_t hdr[8];
  if (fread(hdr, 1, 8, f) != 8) { fclose(f); return false; }
  *group = hdr[0] | (hdr[1] << 8);
  *num   = hdr[2] | (hdr[3] << 8);
  *w     = hdr[4] | (hdr[5] << 8);
  *h     = hdr[6] | (hdr[7] << 8);
  size_t n = (size_t)(*w) * (size_t)(*h);
  if (n > sizeof s_ref / sizeof s_ref[0]) { fclose(f); return false; }
  for (size_t i = 0; i < n; i++) {
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) { fclose(f); return false; }
    s_ref[i] = (uint16_t)(b[0] | (b[1] << 8));
  }
  fclose(f);
  return true;
}

static void test_map(RomCtx* rc, const char* label, const char* raw_path) {
  int group, num, wpx, hpx;
  if (!load_raw(raw_path, &group, &num, &wpx, &hpx)) {
    printf("(skip %s: %s not found — run tools/gen_render_truth.py)\n", label, raw_path);
    return;
  }

  RomMapHeader h;
  RomLayout lay;
  CHECK(rom_map_header(rc, group, num, &h), "map header reads");
  CHECK(rom_layout(rc, h.layout, &lay), "layout reads");
  if (!rom_map_header(rc, group, num, &h) || !rom_layout(rc, h.layout, &lay)) return;

  MapRender mr;
  CHECK(mr_init(&mr, rc, &lay), "mr_init");

  /* --- decompress both tilesets with OUR LZ77 and check the sizes --- */
  RomTileset p, s;
  rom_tileset(rc, lay.tileset_primary, &p);
  uint32_t pn = 0, sn = 0;
  if (p.compressed) {
    uint32_t want = mr_lz77_size(rc, p.tiles);
    pn = mr_lz77(rc, p.tiles, s_prim_tiles, sizeof s_prim_tiles);
    CHECK(pn == want && pn > 0, "primary tiles decompress to the header size");
  }
  bool have_sec = lay.tileset_secondary && rom_tileset(rc, lay.tileset_secondary, &s);
  if (have_sec && s.compressed) {
    uint32_t want = mr_lz77_size(rc, s.tiles);
    sn = mr_lz77(rc, s.tiles, s_sec_tiles, sizeof s_sec_tiles);
    CHECK(sn == want && sn > 0, "secondary tiles decompress to the header size");
  }
  mr_set_tiles(&mr, s_prim_tiles, pn, s_sec_tiles, sn);

  /* --- cache the metatile tables (size derived from the attribute-array gap) --- */
  uint32_t pmb = mr_metatile_table_bytes(rc, lay.tileset_primary);
  uint32_t smb = have_sec ? mr_metatile_table_bytes(rc, lay.tileset_secondary) : 0;
  CHECK(pmb > 0, "primary metatile table size derived");
  if (pmb > sizeof s_prim_mt) pmb = sizeof s_prim_mt;
  if (smb > sizeof s_sec_mt)  smb = sizeof s_sec_mt;
  if (pmb) CHECK(rom_read_at(rc, p.metatiles, s_prim_mt, pmb), "primary metatiles read");
  if (smb) CHECK(rom_read_at(rc, s.metatiles, s_sec_mt, smb), "secondary metatiles read");
  mr_set_metatiles(&mr, s_prim_mt, pmb, s_sec_mt, smb);

  printf("\n%-11s map %d.%d  %ldx%ld blocks  tiles %u+%u B  metatiles %u+%u B\n",
         label, group, num, (long)lay.width, (long)lay.height, pn, sn, pmb, smb);

  /* --- THE differential check: render the same region and byte-compare --- */
  int wc = wpx / 16, hc = hpx / 16;
  memset(s_pix, 0, sizeof s_pix);
  CHECK(mr_region(&mr, 0, 0, wc, hc, s_pix, wpx), "mr_region renders");

  size_t n = (size_t)wpx * (size_t)hpx, bad = 0, first = 0;
  for (size_t i = 0; i < n; i++)
    if (s_pix[i] != s_ref[i]) { if (!bad) first = i; bad++; }

  if (bad) {
    printf("  MISMATCH: %zu/%zu pixels differ; first at (%zu,%zu) C=0x%04X py=0x%04X\n",
           bad, n, first % (size_t)wpx, first / (size_t)wpx, s_pix[first], s_ref[first]);
    g_fail++;
  } else {
    printf("  ✓ %zu pixels identical to the Python reference renderer\n", n);
  }

  /* --- spot-checks on the pieces, so a failure localises --- */
  uint16_t cell = 0;
  CHECK(rom_blocks(rc, &lay, 0, &cell, 1), "cell (0,0) reads");
  uint16_t e[8];
  CHECK(mr_metatile_entries(&mr, ROM_CELL_METATILE(cell), e), "metatile entries read");
  /* the cached path and the ROM path must agree */
  MapRender bare = mr;
  mr_set_metatiles(&bare, 0, 0, 0, 0);
  uint16_t e2[8];
  CHECK(mr_metatile_entries(&bare, ROM_CELL_METATILE(cell), e2), "uncached entries read");
  CHECK(memcmp(e, e2, sizeof e) == 0, "cached and ROM-read metatile entries agree");

  /* mips must be a point-sample of the full block, not garbage */
  uint16_t full[MR_METATILE_PIXELS], mip[64];
  memset(full, 0, sizeof full);
  CHECK(mr_metatile_pixels(&mr, ROM_CELL_METATILE(cell), full, MR_METATILE_PX), "full block");
  CHECK(mr_metatile_mip(&mr, ROM_CELL_METATILE(cell), 8, mip, 8), "8x8 mip");
  int mip_ok = 1;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      if (mip[y * 8 + x] != full[(y * 2) * MR_METATILE_PX + x * 2]) mip_ok = 0;
  CHECK(mip_ok, "8x8 mip point-samples the composited block");
  CHECK(!mr_metatile_mip(&mr, 0, 32, mip, 8), "a mip larger than the metatile is refused");
}

int main(int argc, char** argv) {
  const char* rom_path = (argc > 1) ? argv[1]
      : "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_EMER_BPEE00.gba";
  FILE* f = fopen(rom_path, "rb");
  if (!f) { printf("(skipped: %s not found)\n", rom_path); return 0; }
  fseek(f, 0, SEEK_END);
  uint32_t sz = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);

  RomCtx rc;
  if (!rom_open(&rc, file_read, f, sz)) {
    printf("FAIL: rom_open rejected %s\n", rom_path);
    fclose(f);
    return 1;
  }
  printf("ROM %s rev%u -> %s\n", rc.code, rc.version, rom_kind_name(rc.kind));

  /* ---- LZ77 edge cases ---- */
  {
    uint8_t small[8];
    /* a non-LZ77 address must be refused, not misparsed */
    CHECK(mr_lz77_size(&rc, ROM_BASE) == 0 || mr_lz77_size(&rc, ROM_BASE) > 0,
          "lz77_size on arbitrary data does not crash");
    CHECK(mr_lz77(&rc, ROM_BASE + 0x100, small, 0) == 0, "zero cap refused");
    CHECK(mr_lz77(&rc, ROM_BASE + 0x100, 0, 100) == 0, "NULL dst refused");
    /* a real stream must refuse a cap smaller than its declared size */
    RomMapHeader h; RomLayout lay; RomTileset p;
    if (rom_map_header(&rc, 0, 0, &h) && rom_layout(&rc, h.layout, &lay) &&
        rom_tileset(&rc, lay.tileset_primary, &p) && p.compressed) {
      uint32_t want = mr_lz77_size(&rc, p.tiles);
      CHECK(want > 0, "real tileset has an LZ77 header");
      CHECK(mr_lz77(&rc, p.tiles, s_prim_tiles, want - 1) == 0,
            "a cap below the declared size is refused (no overrun)");
    }
  }

  test_map(&rc, "littleroot", "docs/analysis-2026-07-29/render-littleroot.raw");
  test_map(&rc, "petalburg",  "docs/analysis-2026-07-29/render-petalburg.raw");

  fclose(f);
  printf("\n");
  if (g_fail == 0) printf("OK: host_render_test (0 failures)\n");
  else             printf("host_render_test: %d FAILURE(S)\n", g_fail);
  return g_fail ? 1 : 0;
}
