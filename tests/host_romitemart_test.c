/* Host (PC) test for rom_itemart -- ITEM ICONS and TYPE BADGES read from REAL retail
 * ROMs. rom_itemart does all its I/O through the RomCtx callback, so the code
 * exercised here is byte-for-byte the code that runs on the GBA.
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -O2 -I source tests/host_romitemart_test.c source/rom_itemart.c \
 *      source/rom_map.c source/map_render.c -o /tmp/hria && /tmp/hria
 *
 * What it proves, against Guy's own five cartridge dumps:
 *
 *  1) LOCATION BY SHAPE, not by trust. Every pinned address in rom_itemart.c is
 *     re-derived here by an independent scan of the whole image and must match:
 *       - gItemIconTable  = the unique maximal run of 8-byte { pic, pal } rows whose
 *         pic decompresses to exactly 288 B and pal to exactly 32 B (>= 300 rows).
 *       - gMoveTypes_Gfx  = the unique LZ blob of exactly 0x1700 B followed at the
 *         next 4-aligned address by one of exactly 0x60 B.
 *       - gMenuInfoElements_Gfx (FR/LG) = the literal-pool neighbour of the sole
 *         occurrence of the 96-byte sMenuInfoIcons table, confirmed by the badge
 *         geometry.
 *     Each address is PRINTED so a human can check it against published values, and
 *     the scan must produce EXACTLY ONE candidate or the test fails.
 *
 *  2) Every id in range decodes to the exact expected size -- and to the exact
 *     PIXELS: this file carries its OWN LZ77 decoder and its own tile->RGB15
 *     expander, written from the format rather than from rom_itemart.c, and every
 *     one of the ~378 icons and 23 badges is byte-compared against them. Two
 *     independent implementations agreeing on real cartridge bytes is the same bar
 *     host_render_test.c sets for the map renderer.
 *
 *  3) Out-of-range ids, short buffers, NULL pointers and sheets from the wrong ROM
 *     are refused.
 *
 *  4) A truncated image, an all-zero image, and an image whose pins point past the
 *     end all fail closed instead of reading out of bounds.
 *
 *  5) The EZ-Flash silent-garbage defence really works: a reader that returns
 *     SUCCESS while quietly corrupting a different byte on every call makes
 *     rom_item_icon FAIL with verification on, and (deliberately) succeed with it
 *     off. NOTE the honest limit of the scheme, which this test also demonstrates:
 *     it catches TRANSIENT garbage. A reader that returns the SAME wrong bytes every
 *     time is indistinguishable from a correct read of different data, and no
 *     read-twice scheme can catch that.
 *
 *  6) The geometry claims in rom_itemart.h are measured, not asserted: all 23 RSE
 *     badges have blank pixel rows 0 and 15 (so ROM rows 1..14 reproduce the shipped
 *     32x14 crop), and all 18 FR/LG badges have transparent corners.
 *
 * It also REPORTS how many item ids carry a real icon versus the shared "?"
 * placeholder in each game.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_map.h"
#include "rom_itemart.h"

static int fails = 0, checks = 0;
static void chk(const char* rom, const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", rom, what); fails++; }
}

/* ---- a RomReadFn over a byte buffer, optionally FLAKY ---------------------------
 * `flaky` reproduces the EZ-Flash hazard exactly: Read_SD_sectors returns SUCCESS
 * while the buffer holds garbage. One byte of every read is corrupted with a value
 * that changes on every call, so no two reads of the same window ever agree -- which
 * is what a read-twice-and-compare scheme exists to catch. */
typedef struct {
  const uint8_t* p;
  uint32_t n;
  long calls;
  int  flaky;
} MemCtx;

static bool mem_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  MemCtx* m = (MemCtx*)ctx;
  if (off > m->n || len > m->n - off) return false;
  memcpy(dst, m->p + off, len);
  m->calls++;
  if (m->flaky && len)
    ((uint8_t*)dst)[len - 1] ^= (uint8_t)(m->calls * 37u + 1u);
  return true;
}

static uint8_t* slurp(const char* path, uint32_t* out_n) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* b = (uint8_t*)malloc((size_t)sz);
  if (!b) { fclose(f); return 0; }
  if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return 0; }
  fclose(f);
  *out_n = (uint32_t)sz;
  return b;
}

/* ================================================================================
 * INDEPENDENT reference implementations -- written from the format, not from
 * rom_itemart.c / map_render.c. If these and the module ever disagree on a real
 * cartridge byte, one of them is wrong and the test says so.
 * ============================================================================== */

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* LZ10 over a resident image. Returns the decompressed length, 0 on any malformity,
 * and (optionally) the number of COMPRESSED bytes consumed. */
static uint32_t ref_lz77(const uint8_t* rom, uint32_t n, uint32_t off,
                         uint8_t* out, uint32_t cap, uint32_t* consumed) {
  if (off + 4 > n || rom[off] != 0x10) return 0;
  uint32_t size = (uint32_t)rom[off + 1] | ((uint32_t)rom[off + 2] << 8) |
                  ((uint32_t)rom[off + 3] << 16);
  if (!size || size > cap) return 0;
  uint32_t p = off + 4, w = 0;
  while (w < size) {
    if (p >= n) return 0;
    uint8_t flags = rom[p++];
    for (int bit = 0; bit < 8 && w < size; bit++) {
      if (flags & (0x80 >> bit)) {
        if (p + 1 >= n) return 0;
        uint32_t b1 = rom[p], b2 = rom[p + 1];
        p += 2;
        uint32_t len = (b1 >> 4) + 3, disp = (((b1 & 0x0F) << 8) | b2) + 1;
        if (disp > w) return 0;
        if (len > size - w) len = size - w;
        for (uint32_t k = 0; k < len; k++) { out[w] = out[w - disp]; w++; }
      } else {
        if (p >= n) return 0;
        out[w++] = rom[p++];
      }
    }
  }
  if (consumed) *consumed = p - off;
  return size;
}

/* 4bpp GBA tiles (1D order, `tw` tiles per row) -> RGB15, 0 transparent. */
static void ref_expand(const uint8_t* tiles, int tw, int w, int h,
                       const uint16_t pal[16], uint16_t* dst) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int t = (y / 8) * tw + (x / 8);
      const uint8_t* tp = tiles + t * 32 + (y % 8) * 4 + (x % 8) / 2;
      int idx = (x & 1) ? (*tp >> 4) : (*tp & 0x0F);
      dst[y * w + x] = idx ? (uint16_t)(0x8000u | (pal[idx] & 0x7FFFu)) : 0u;
    }
}

static void ref_pal(const uint8_t* raw, uint16_t out[16]) {
  for (int i = 0; i < 16; i++) out[i] = (uint16_t)(raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
}

/* ================================================================================
 * THE SHAPE SCANS -- how the pins were found in the first place
 * ============================================================================== */

#define SCAN_MIN_ROWS 300

/* Per-word classification: 1 = points at an LZ blob of 0x120 (an item pic),
 * 2 = at one of 0x20 (an item palette), 4 = the word is zero. */
static uint8_t* classify(const uint8_t* rom, uint32_t n) {
  uint32_t nw = n / 4;
  uint8_t* c = (uint8_t*)calloc(nw, 1);
  if (!c) return 0;
  for (uint32_t i = 0; i < nw; i++) {
    uint32_t w = rd32(rom + i * 4);
    if (!w) { c[i] = 4; continue; }
    if (w < ROM_BASE) continue;
    uint32_t off = w - ROM_BASE;
    if (off >= n || (off & 3) || off + 4 > n || rom[off] != 0x10) continue;
    uint32_t sz = (uint32_t)rom[off + 1] | ((uint32_t)rom[off + 2] << 8) |
                  ((uint32_t)rom[off + 3] << 16);
    if (sz == 288) c[i] = 1;
    else if (sz == 32) c[i] = 2;
  }
  return c;
}

/* Maximal runs of { pic, pal } rows (NULL rows allowed inside a run) with at least
 * SCAN_MIN_ROWS real rows. Returns the number of candidates; fills the first. */
static int scan_item_table(const uint8_t* rom, uint32_t n,
                           uint32_t* out_addr, int* out_rows, int* out_icons) {
  uint8_t* c = classify(rom, n);
  if (!c) return -1;
  uint32_t nw = n / 4;
  int found = 0;
  for (int par = 0; par < 2; par++) {
    uint32_t i = (uint32_t)par;
    while (i + 1 < nw) {
      int good = ((c[i] == 1 && c[i + 1] == 2) || (c[i] == 4 && c[i + 1] == 4));
      if (!good) { i += 2; continue; }
      uint32_t start = i;
      int rows = 0, icons = 0;
      while (i + 1 < nw && ((c[i] == 1 && c[i + 1] == 2) || (c[i] == 4 && c[i + 1] == 4))) {
        rows++;
        if (c[i] == 1) icons++;
        i += 2;
      }
      if (icons >= SCAN_MIN_ROWS) {
        if (!found) { *out_addr = ROM_BASE + start * 4; *out_rows = rows; *out_icons = icons; }
        found++;
      }
    }
  }
  free(c);
  return found;
}

/* The RSE badge sheet: an LZ blob of exactly 0x1700 whose next 4-aligned neighbour is
 * an LZ blob of exactly 0x60. */
static int scan_type_sheet_rse(const uint8_t* rom, uint32_t n,
                               uint32_t* out_gfx, uint32_t* out_pal) {
  static uint8_t sheet[ROM_TYPE_SHEET_BYTES];
  uint8_t pal[96];
  int found = 0;
  for (uint32_t off = 0; off + 4 <= n; off += 4) {
    if (rom[off] != 0x10) continue;
    uint32_t sz = (uint32_t)rom[off + 1] | ((uint32_t)rom[off + 2] << 8) |
                  ((uint32_t)rom[off + 3] << 16);
    if (sz != ROM_TYPE_SHEET_BYTES) continue;
    uint32_t used = 0;
    if (ref_lz77(rom, n, off, sheet, sizeof sheet, &used) != ROM_TYPE_SHEET_BYTES) continue;
    uint32_t po = (off + used + 3u) & ~3u;
    if (ref_lz77(rom, n, po, pal, sizeof pal, 0) != 96) continue;
    if (!found) { *out_gfx = ROM_BASE + off; *out_pal = ROM_BASE + po; }
    found++;
  }
  return found;
}

/* FR/LG: sMenuInfoIcons (src/list_menu.c:52-77) is 24 rows of { u8 w, u8 h, u16 off }
 * and appears exactly once per image. The literal pool entry immediately after the
 * one reference to it is gMenuInfoElements_Gfx. */
static int scan_type_sheet_frlg(const uint8_t* rom, uint32_t n, uint32_t* out_gfx) {
  static const uint8_t sig[96] = {
    0x0C,0x0C,0x00,0x00, 0x20,0x0C,0x20,0x00, 0x20,0x0C,0x64,0x00, 0x20,0x0C,0x60,0x00,
    0x20,0x0C,0x80,0x00, 0x20,0x0C,0x48,0x00, 0x20,0x0C,0x44,0x00, 0x20,0x0C,0x6C,0x00,
    0x20,0x0C,0x68,0x00, 0x20,0x0C,0x88,0x00, 0x20,0x0C,0xA4,0x00, 0x20,0x0C,0x24,0x00,
    0x20,0x0C,0x28,0x00, 0x20,0x0C,0x2C,0x00, 0x20,0x0C,0x40,0x00, 0x20,0x0C,0x84,0x00,
    0x20,0x0C,0x4C,0x00, 0x20,0x0C,0xA0,0x00, 0x20,0x0C,0x8C,0x00, 0x28,0x0C,0xA8,0x00,
    0x28,0x0C,0xC0,0x00, 0x28,0x0C,0xC8,0x00, 0x28,0x0C,0xE0,0x00, 0x28,0x0C,0xE8,0x00,
  };
  uint32_t tab = 0;
  int hits = 0;
  for (uint32_t off = 0; off + sizeof sig <= n; off += 4)
    if (memcmp(rom + off, sig, sizeof sig) == 0) { if (!hits) tab = ROM_BASE + off; hits++; }
  if (hits != 1) return hits;
  int found = 0;
  for (uint32_t i = 0; i + 8 <= n; i += 4)
    if (rd32(rom + i) == tab) {
      uint32_t nxt = rd32(rom + i + 4);
      if (nxt >= ROM_BASE && nxt - ROM_BASE + ROM_TYPE_FRLG_SHEET_BYTES <= n) {
        if (!found) *out_gfx = nxt;
        found++;
      }
    }
  return found;
}

/* ================================================================================
 * per-ROM run
 * ============================================================================== */

typedef struct {
  const char* name;
  const char* file;
  int         want_items;      /* 1 = this game has item icons          */
  int         want_item_count; /* ITEMS_COUNT                           */
  int         want_style;      /* RomTypeStyle                          */
} RomCase;

static uint8_t  g_sheet[ROM_TYPE_SHEET_BYTES];   /* stands in for mon_decomp        */
static uint8_t  g_sheet_wide[8192];    /* BACKLOG #145: stands in for the REAL
                                         * mon_decomp cap (MON_DECOMP_BYTES, artbuf.h)
                                         * pdna_main.c's app_type_badge() now passes,
                                         * instead of ROM_TYPE_SHEET_BYTES == want */
static uint16_t g_icon[ROM_ITEM_ICON_PX];
static uint16_t g_ref[ROM_ITEM_ICON_PX];
static uint16_t g_badge[ROM_TYPE_BADGE_MAX_PX];
static uint16_t g_bref[ROM_TYPE_BADGE_MAX_PX];

static void run_rom(const char* dir, const RomCase* rcase) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, rcase->file);
  uint32_t n = 0;
  uint8_t* rom = slurp(path, &n);
  if (!rom) { printf("SKIP %s (no %s)\n", rcase->name, path); return; }
  const char* name = rcase->name;

  MemCtx mc = { rom, n, 0, 0 };
  RomCtx rc;
  if (!rom_open(&rc, mem_read, &mc, n)) {
    chk(name, "rom_open accepts the retail dump", 0);
    free(rom); return;
  }

  RomItemArt ra;
  int ok = rom_itemart_open(&ra, &rc);
  chk(name, "rom_itemart_open succeeds", ok == 1);
  if (!ok) { free(rom); return; }

  printf("  %s (%s rev%u):\n", name, rom_kind_name(rc.kind), rc.version);

  /* ---- 1) the pins, re-derived by shape ------------------------------------ */
  chk(name, "item icons available exactly where the game has them",
      rom_itemart_have_items(&ra) == rcase->want_items);

  uint32_t saddr = 0; int srows = 0, sicons = 0;
  int ncand = scan_item_table(rom, n, &saddr, &srows, &sicons);
  if (rcase->want_items) {
    chk(name, "the item-icon shape scan finds EXACTLY ONE candidate", ncand == 1);
    chk(name, "the scan reproduces the pinned gItemIconTable", ncand == 1 && saddr == ra.items);
    chk(name, "the located table is ITEMS_COUNT+1 rows long",
        srows == rcase->want_item_count + 1);
    printf("      gItemIconTable  = 0x%08X   (%d rows, %d non-NULL)  [scan; pin = 0x%08X]\n",
           saddr, srows, sicons, ra.items);
    chk(name, "item count is this game's ITEMS_COUNT",
        rom_itemart_item_count(&ra) == rcase->want_item_count);
  } else {
    chk(name, "no item-icon table exists in this game at all", ncand == 0);
    chk(name, "and the module serves no item icons", ra.items == 0);
    chk(name, "and rom_item_icon refuses every id",
        !rom_item_icon(&ra, 0, g_icon, ROM_ITEM_ICON_PX) &&
        !rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX));
    printf("      gItemIconTable  = none (this game has no item icons)\n");
  }

  chk(name, "type-badge style is right", ra.type_style == rcase->want_style);
  chk(name, "type badges available", rom_itemart_have_types(&ra) == 1);
  if (rcase->want_style == ROM_TYPEART_RSE) {
    uint32_t g = 0, p = 0;
    int nt = scan_type_sheet_rse(rom, n, &g, &p);
    chk(name, "the type-sheet scan finds EXACTLY ONE candidate", nt == 1);
    chk(name, "the scan reproduces the pinned gMoveTypes_Gfx", nt == 1 && g == ra.type_gfx);
    chk(name, "the scan reproduces the pinned gMoveTypes_Pal", nt == 1 && p == ra.type_pal);
    printf("      gMoveTypes_Gfx  = 0x%08X   gMoveTypes_Pal = 0x%08X  [scan; pins 0x%08X / 0x%08X]\n",
           g, p, ra.type_gfx, ra.type_pal);
    chk(name, "RSE serves 23 badges (18 types + 5 contest)",
        rom_type_badge_count(&ra) == ROM_TYPE_BADGES_RSE);
    chk(name, "RSE badges are 32x16", rom_type_badge_h(&ra) == ROM_TYPE_BADGE_H_RSE);
    chk(name, "RSE needs a 5,888 B scratch",
        rom_type_scratch_bytes(&ra) == ROM_TYPE_SHEET_BYTES);
  } else {
    uint32_t g = 0;
    int nt = scan_type_sheet_frlg(rom, n, &g);
    chk(name, "the FR/LG menu-info scan finds EXACTLY ONE candidate", nt == 1);
    chk(name, "the scan reproduces the pinned gMenuInfoElements_Gfx", nt == 1 && g == ra.type_gfx);
    printf("      gMenuInfoElements_Gfx = 0x%08X  pal = 0x%08X  [scan; pins 0x%08X / 0x%08X]\n",
           g, g - 32, ra.type_gfx, ra.type_pal);
    chk(name, "FR/LG serves 18 badges (no contest categories)",
        rom_type_badge_count(&ra) == ROM_TYPE_TYPES);
    chk(name, "FR/LG badges are 32x12", rom_type_badge_h(&ra) == ROM_TYPE_BADGE_H_FRLG);
    chk(name, "FR/LG needs no scratch", rom_type_scratch_bytes(&ra) == 0);
  }

  /* ---- 2) every item icon, byte-compared against the reference decoder ------ */
  if (rcase->want_items) {
    int cnt = rom_itemart_item_count(&ra);
    int bad = 0, mism = 0, placeholder = 0, real = 0, badpx = 0;
    uint32_t qm = rd32(rom + (ra.items - ROM_BASE));      /* row 0's pic = the "?" */
    for (int id = 0; id < cnt; id++) {
      if (!rom_item_icon(&ra, (uint16_t)id, g_icon, ROM_ITEM_ICON_PX)) { bad++; continue; }
      /* independent decode of the same row */
      uint32_t row = ra.items - ROM_BASE + (uint32_t)id * 8;
      uint32_t pic = rd32(rom + row), pal = rd32(rom + row + 4);
      if (pic == qm) placeholder++; else real++;
      uint8_t px[ROM_ITEM_ICON_BYTES], pr[ROM_ITEM_PAL_BYTES];
      uint16_t p16[16];
      if (ref_lz77(rom, n, pic - ROM_BASE, px, sizeof px, 0) != ROM_ITEM_ICON_BYTES ||
          ref_lz77(rom, n, pal - ROM_BASE, pr, sizeof pr, 0) != ROM_ITEM_PAL_BYTES) { mism++; continue; }
      ref_pal(pr, p16);
      ref_expand(px, 3, ROM_ITEM_ICON_W, ROM_ITEM_ICON_H, p16, g_ref);
      if (memcmp(g_icon, g_ref, sizeof g_ref) != 0) {
        if (mism++ < 3) printf("      [%s] item %d: module != reference\n", name, id);
      }
      for (int k = 0; k < ROM_ITEM_ICON_PX; k++)
        if (g_icon[k] && !(g_icon[k] & 0x8000u)) { badpx++; break; }
    }
    chk(name, "every item id in range decodes", bad == 0);
    chk(name, "every icon matches the independent reference decoder", mism == 0);
    chk(name, "every opaque pixel carries bit 15", badpx == 0);
    printf("      item icons: %d ids decode; %d carry a real icon, %d the shared \"?\" placeholder\n",
           cnt - bad, real, placeholder);
  }

  /* ---- 2b) every type badge, byte-compared ---------------------------------- */
  RomTypeSheet ts;
  int loaded = rom_type_sheet_load(&ra, &ts, g_sheet, sizeof g_sheet);
  chk(name, "the badge sheet loads", loaded == 1);
  if (loaded) {
    int cnt = rom_type_badge_count(&ra), h = rom_type_badge_h(&ra);
    chk(name, "the sheet reports the same count/height as the ROM",
        ts.count == cnt && ts.h == h);
    int bad = 0, mism = 0, blankrow = 0, corners = 0;
    /* the reference needs its own copy of the palette mapping / offsets */
    static const uint8_t rse_pal[ROM_TYPE_BADGES_RSE] =
      { 0,0,1,1,0,0,2,1,0,2,0,1,2,0,1,1,2,0, 0,1,1,2,0 };
    static const uint8_t frlg_off[ROM_TYPE_TYPES] =
      { 0x20,0x64,0x60,0x80,0x48,0x44,0x6C,0x68,0x88,0xA4,
        0x24,0x28,0x2C,0x40,0x84,0x4C,0xA0,0x8C };
    uint8_t sheet[ROM_TYPE_SHEET_BYTES], praw[96], blk[8 * 32];
    uint16_t p16[3][16];
    if (ts.style == ROM_TYPEART_RSE) {
      ref_lz77(rom, n, ra.type_gfx - ROM_BASE, sheet, sizeof sheet, 0);
      ref_lz77(rom, n, ra.type_pal - ROM_BASE, praw, sizeof praw, 0);
      for (int i = 0; i < 3; i++) ref_pal(praw + i * 32, p16[i]);
    } else {
      ref_pal(rom + (ra.type_pal - ROM_BASE), p16[0]);
    }
    for (int b = 0; b < cnt; b++) {
      if (!rom_type_badge(&ra, &ts, (uint8_t)b, g_badge, ROM_TYPE_BADGE_MAX_PX)) { bad++; continue; }
      if (ts.style == ROM_TYPEART_RSE) {
        ref_expand(sheet + b * 256, 4, ROM_TYPE_BADGE_W, ROM_TYPE_BADGE_H_RSE,
                   p16[rse_pal[b]], g_bref);
        /* the 32x14 crop claim: pixel rows 0 and 15 must be entirely transparent */
        for (int x = 0; x < ROM_TYPE_BADGE_W; x++)
          if (g_badge[x] || g_badge[15 * ROM_TYPE_BADGE_W + x]) { blankrow++; break; }
      } else {
        /* stage the badge out of the raw 128x128 sheet the way the game does */
        const uint8_t* sh = rom + (ra.type_gfx - ROM_BASE);
        for (int r = 0; r < 2; r++)
          memcpy(blk + r * 4 * 32, sh + ((uint32_t)frlg_off[b] + (uint32_t)r * 16) * 32, 4 * 32);
        ref_expand(blk, 4, ROM_TYPE_BADGE_W, ROM_TYPE_BADGE_H_FRLG, p16[0], g_bref);
        int w = ROM_TYPE_BADGE_W;
        if (g_badge[0] || g_badge[w - 1] || g_badge[(h - 1) * w] || g_badge[(h - 1) * w + w - 1] ||
            !g_badge[(h / 2) * w + w / 2]) corners++;
      }
      if (memcmp(g_badge, g_bref, (size_t)ROM_TYPE_BADGE_W * h * 2) != 0) {
        if (mism++ < 3) printf("      [%s] badge %d: module != reference\n", name, b);
      }
    }
    chk(name, "every badge decodes", bad == 0);
    chk(name, "every badge matches the independent reference decoder", mism == 0);
    if (ts.style == ROM_TYPEART_RSE)
      chk(name, "all 23 RSE badges have blank rows 0 and 15 (the 32x14 crop)", blankrow == 0);
    else
      chk(name, "all 18 FR/LG badges have transparent corners + an opaque centre", corners == 0);
    printf("      type badges: %d decoded (%d types%s), %dx%d\n", cnt, ROM_TYPE_TYPES,
           ts.style == ROM_TYPEART_RSE ? " + 5 contest categories" : "",
           ROM_TYPE_BADGE_W, h);
  }

  /* ---- 3) refusals ---------------------------------------------------------- */
  if (rcase->want_items) {
    int cnt = rom_itemart_item_count(&ra);
    chk(name, "item id == ITEMS_COUNT is refused (that row is the bag's arrow)",
        !rom_item_icon(&ra, (uint16_t)cnt, g_icon, ROM_ITEM_ICON_PX));
    chk(name, "item id 0xFFFF is refused",
        !rom_item_icon(&ra, 0xFFFF, g_icon, ROM_ITEM_ICON_PX));
    chk(name, "a short buffer is refused, not partially filled",
        !rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX - 1));
    chk(name, "a NULL destination is refused", !rom_item_icon(&ra, 13, 0, ROM_ITEM_ICON_PX));
  }
  if (loaded) {
    chk(name, "badge id == count is refused",
        !rom_type_badge(&ra, &ts, (uint8_t)rom_type_badge_count(&ra), g_badge, ROM_TYPE_BADGE_MAX_PX));
    chk(name, "badge id 255 is refused",
        !rom_type_badge(&ra, &ts, 255, g_badge, ROM_TYPE_BADGE_MAX_PX));
    chk(name, "a short badge buffer is refused",
        !rom_type_badge(&ra, &ts, 0, g_badge, 1));
    RomTypeSheet wrong = ts;
    wrong.style = (uint8_t)(ts.style == ROM_TYPEART_RSE ? ROM_TYPEART_FRLG : ROM_TYPEART_RSE);
    chk(name, "a sheet loaded from a DIFFERENT ROM style is refused",
        !rom_type_badge(&ra, &wrong, 0, g_badge, ROM_TYPE_BADGE_MAX_PX));
    RomTypeSheet zero; memset(&zero, 0, sizeof zero);
    chk(name, "an unloaded sheet is refused",
        !rom_type_badge(&ra, &zero, 0, g_badge, ROM_TYPE_BADGE_MAX_PX));
    /* the stale-sheet footgun: Ruby, Sapphire and Emerald all report ROM_TYPEART_RSE
     * but ship different badge art, so a sheet from another RomItemArt must be
     * refused rather than quietly drawing the wrong game's badges. */
    RomItemArt other = ra;
    chk(name, "a sheet whose OWNER is a different RomItemArt is refused",
        !rom_type_badge(&other, &ts, 0, g_badge, ROM_TYPE_BADGE_MAX_PX));
    chk(name, "and the sheet still works with its real owner",
        rom_type_badge(&ra, &ts, 0, g_badge, ROM_TYPE_BADGE_MAX_PX));
  }
  if (rcase->want_style == ROM_TYPEART_RSE) {
    RomTypeSheet t2;
    chk(name, "a scratch one byte too small is refused",
        !rom_type_sheet_load(&ra, &t2, g_sheet, ROM_TYPE_SHEET_BYTES - 1) && !t2.ok);
    chk(name, "a NULL scratch is refused on RSE",
        !rom_type_sheet_load(&ra, &t2, 0, ROM_TYPE_SHEET_BYTES));
  } else {
    RomTypeSheet t2;
    chk(name, "FR/LG loads with NO scratch at all",
        rom_type_sheet_load(&ra, &t2, 0, 0) && t2.ok);
  }

  free(rom);
}

/* ---- 4) truncated / zeroed / poisoned images ------------------------------------ */
static void bounds_tests(const char* dir) {
  char path[512];
  snprintf(path, sizeof path, "%s/Emerald.gba", dir);
  uint32_t n = 0;
  uint8_t* rom = slurp(path, &n);
  if (!rom) { printf("SKIP bounds tests (no %s)\n", path); return; }

  RomCtx rc; RomItemArt ra; MemCtx mc = { rom, n, 0, 0 };

  /* (a) all zeroes: rom_open refuses it and rom_itemart refuses the husk it leaves. */
  uint8_t* zero = (uint8_t*)calloc(1, n);
  MemCtx zc = { zero, n, 0, 0 };
  chk("bounds", "rom_open refuses an all-zero image", !rom_open(&rc, mem_read, &zc, n));
  chk("bounds", "rom_itemart_open refuses it too", !rom_itemart_open(&ra, &rc));
  chk("bounds", "and serves nothing",
      !rom_itemart_have_items(&ra) && !rom_itemart_have_types(&ra) &&
      !rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX));
  free(zero);

  /* (b) a half-length dump never gets past rom_open's own size rule. */
  MemCtx tc = { rom, n / 2, 0, 0 };
  chk("bounds", "rom_open refuses a half-length dump", !rom_open(&rc, mem_read, &tc, n / 2));

  /* (c) the interesting truncation: a VALID Emerald whose RomCtx claims the image is
   *     half as long. Both pins now sit past the end, and both classes must switch
   *     off rather than read out of bounds. */
  chk("bounds", "the full image opens", rom_open(&rc, mem_read, &mc, n));
  rc.size = n / 2;
  chk("bounds", "a truncated image is rejected outright", !rom_itemart_open(&ra, &rc));
  chk("bounds", "no item icons after truncation", !rom_itemart_have_items(&ra));
  chk("bounds", "no type badges after truncation", !rom_itemart_have_types(&ra));
  chk("bounds", "and nothing can be read",
      !rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX));

  /* (d) an unpinned revision fails closed even though the ROM is genuine. Guy owns
   *     BPEE rev 0, which IS pinned, so bend the revision byte -- BPEE rev 1 has no
   *     row and there is nothing in the GF header to fall back on. */
  chk("bounds", "the real revision opens", rom_open(&rc, mem_read, &mc, n) &&
      rom_itemart_open(&ra, &rc));
  rc.version = 1;
  chk("bounds", "an unpinned revision fails closed", !rom_itemart_open(&ra, &rc));
  chk("bounds", "and serves neither class",
      !rom_itemart_have_items(&ra) && !rom_itemart_have_types(&ra));

  /* (e) a poisoned copy: keep the item table, break the badge sheet's LZ header, and
   *     separately break the item table's probe row. Each class must die alone. */
  uint8_t* poison = (uint8_t*)malloc(n);
  MemCtx pc = { poison, n, 0, 0 };

  memcpy(poison, rom, n);
  poison[0x08D971B0 - ROM_BASE] = 0x11;                 /* not an LZ10 header */
  chk("bounds", "the poisoned image still opens as Emerald", rom_open(&rc, mem_read, &pc, n));
  chk("bounds", "rom_itemart_open still succeeds (items survive)", rom_itemart_open(&ra, &rc));
  chk("bounds", "a broken badge sheet switches OFF just the badges",
      !rom_itemart_have_types(&ra) && rom_itemart_have_items(&ra));
  chk("bounds", "and item icons still decode",
      rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX));
  RomTypeSheet ts;
  chk("bounds", "loading a switched-off sheet fails",
      !rom_type_sheet_load(&ra, &ts, g_sheet, sizeof g_sheet));

  memcpy(poison, rom, n);
  {   /* point the Master Ball row's pic one byte past the end of the image */
    uint32_t past = ROM_BASE + n;
    uint32_t row = 0x08614410 - ROM_BASE + 1 * 8;
    for (int k = 0; k < 4; k++) poison[row + k] = (uint8_t)(past >> (8 * k));
    chk("bounds", "still opens with a poisoned item row", rom_open(&rc, mem_read, &pc, n));
    chk("bounds", "rom_itemart_open still succeeds (badges survive)", rom_itemart_open(&ra, &rc));
    chk("bounds", "an out-of-image item pointer switches OFF just the item icons",
        !rom_itemart_have_items(&ra) && rom_itemart_have_types(&ra));
    chk("bounds", "and a disabled class reads nothing",
        !rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX));
    chk("bounds", "while badges still load",
        rom_type_sheet_load(&ra, &ts, g_sheet, sizeof g_sheet) &&
        rom_type_badge(&ra, &ts, 0, g_badge, ROM_TYPE_BADGE_MAX_PX));
  }

  memcpy(poison, rom, n);
  {   /* the extent trap: probe rows 0/1 stay valid but the table's LAST row is cut off
       * by shrinking the image to end just after row 2. rom_open's size rule would
       * reject that, so shrink only the RomCtx the module bounds-checks against. */
    chk("bounds", "extent-trap image opens", rom_open(&rc, mem_read, &pc, n));
    rc.size = (0x08614410 - ROM_BASE) + 3 * 8;
    chk("bounds", "a table whose EXTENT does not fit is rejected even though row 1 reads",
        !rom_itemart_open(&ra, &rc) || !rom_itemart_have_items(&ra));
  }

  /* (f) THE SHORT-BLOB TRAP, and the reason rom_item_icon demands an EXACT length.
   *     An LZ blob whose header claims FEWER bytes than an icon decodes happily and
   *     fills only part of the destination -- the rest of the buffer is whatever the
   *     previous icon left there. That is a stale-pixels bug that looks like a
   *     rendering glitch, so the decode must reject the length, not just the failure.
   *     Poison a row that is NOT one of the five rows open() probes, so the module
   *     still opens and only this one id is affected. */
  memcpy(poison, rom, n);
  {
    uint32_t tab = 0x08614410 - ROM_BASE;
    uint32_t probe_pics[5];
    const int probe_rows[5] = { 0, 1, 377 / 2, 377 - 1, 377 };
    for (int k = 0; k < 5; k++) probe_pics[k] = rd32(rom + tab + (uint32_t)probe_rows[k] * 8);
    int victim = -1;
    for (int id = 2; id < 377 && victim < 0; id++) {
      uint32_t pic = rd32(rom + tab + (uint32_t)id * 8);
      int shared = 0;
      for (int k = 0; k < 5; k++) if (probe_pics[k] == pic) shared = 1;
      /* also make sure no OTHER id we still expect to work shares this pic */
      if (!shared) victim = id;
    }
    chk("bounds", "found an item id whose icon no probe row shares", victim > 0);
    if (victim > 0) {
      uint32_t pic = rd32(rom + tab + (uint32_t)victim * 8) - ROM_BASE;
      poison[pic + 1] = 0x00; poison[pic + 2] = 0x01;      /* claim 0x100, not 0x120 */
      chk("bounds", "the short-blob image still opens", rom_open(&rc, mem_read, &pc, n) &&
          rom_itemart_open(&ra, &rc) && rom_itemart_have_items(&ra));
      chk("bounds", "an icon whose blob is SHORTER than 288 B is refused, not partly drawn",
          !rom_item_icon(&ra, (uint16_t)victim, g_icon, ROM_ITEM_ICON_PX));
      chk("bounds", "and every other id still decodes",
          rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX));
      /* the same rule for the palette: 16 colours or nothing */
      memcpy(poison, rom, n);
      uint32_t pal = rd32(rom + tab + (uint32_t)victim * 8 + 4) - ROM_BASE;
      poison[pal + 1] = 0x10;                              /* claim 0x10, not 0x20   */
      chk("bounds", "still opens with a short palette", rom_open(&rc, mem_read, &pc, n) &&
          rom_itemart_open(&ra, &rc));
      chk("bounds", "an icon whose PALETTE is not 16 colours is refused",
          !rom_item_icon(&ra, (uint16_t)victim, g_icon, ROM_ITEM_ICON_PX));
    }
  }

  /* (g) the same exact-length rule for the RSE badge sheet: a header that claims a
   *     different size must switch the badges off at open(), not decode a short sheet
   *     and hand out badges made of stale scratch. */
  memcpy(poison, rom, n);
  {
    uint32_t g = 0x08D971B0 - ROM_BASE;
    poison[g + 1] = 0x00; poison[g + 2] = 0x16;            /* claim 0x1600, not 0x1700 */
    chk("bounds", "the wrong-size sheet image still opens", rom_open(&rc, mem_read, &pc, n));
    chk("bounds", "rom_itemart_open still succeeds (items survive)", rom_itemart_open(&ra, &rc));
    chk("bounds", "a badge sheet of the WRONG declared size switches the badges off",
        !rom_itemart_have_types(&ra) && rom_itemart_have_items(&ra));
  }

  free(poison);
  free(rom);
}

/* ---- 5) the EZ-Flash silent-garbage defence --------------------------------------
 *
 * The point of this section is to isolate VERIFICATION from every other safety net.
 * An LZ77 payload is self-describing, so corrupting it usually trips a structural
 * check (a bad header, a back-reference before the start, a wrong length) and a test
 * that only looked at item icons could pass without verification doing any work.
 *
 * FR/LG's type badge has no structure at all: `frlg_stage_badge` is a plain bounded
 * memcpy of 2 x 128 B out of an uncompressed sheet. Corrupt a byte there and NOTHING
 * except read-twice-and-compare can notice. So the badge path is the control
 * experiment, and the item-icon path is checked afterwards for completeness.
 */
static void verify_tests(const char* dir) {
  char path[512];
  uint32_t n = 0;
  RomCtx rc; RomItemArt ra; RomTypeSheet ts;

  /* ---- the control experiment: FR/LG raw badge reads ---------------------- */
  snprintf(path, sizeof path, "%s/FireRed.gba", dir);
  uint8_t* rom = slurp(path, &n);
  if (rom) {
    MemCtx mc = { rom, n, 0, 0 };
    if (rom_open(&rc, mem_read, &mc, n) && rom_itemart_open(&ra, &rc) &&
        rom_type_sheet_load(&ra, &ts, 0, 0)) {
      chk("verify", "verification is ON by default", ra.verify == 1);

      mc.calls = 0;
      chk("verify", "a clean badge read succeeds",
          rom_type_badge(&ra, &ts, 0, g_bref, ROM_TYPE_BADGE_MAX_PX));
      long verified_calls = mc.calls;

      /* Every read from here returns SUCCESS with one byte quietly wrong, and a
       * different byte each time. The badge path has no structural check at all. */
      mc.flaky = 1; mc.calls = 0;
      chk("verify", "a corrupting reader makes rom_type_badge FAIL (only verify can see this)",
          !rom_type_badge(&ra, &ts, 0, g_badge, ROM_TYPE_BADGE_MAX_PX));

      /* ...and with verification off the very same corruption sails straight
       * through, which is exactly why it is on by default. */
      rom_itemart_set_verify(&ra, 0);
      chk("verify", "rom_itemart_set_verify(0) turns it off", ra.verify == 0);
      mc.calls = 0;
      int got = rom_type_badge(&ra, &ts, 0, g_badge, ROM_TYPE_BADGE_MAX_PX);
      long unverified_calls = mc.calls;
      chk("verify", "with verification OFF the corrupt read is ACCEPTED (the documented cost)",
          got == 1);
      chk("verify", "and the accepted pixels really are wrong",
          got == 1 && memcmp(g_badge, g_bref,
                             (size_t)ROM_TYPE_BADGE_W * ROM_TYPE_BADGE_H_FRLG * 2) != 0);

      /* clean again: the verified path must still agree with the clean baseline */
      rom_itemart_set_verify(&ra, 1);
      mc.flaky = 0;
      chk("verify", "a clean verified read matches the baseline byte for byte",
          rom_type_badge(&ra, &ts, 0, g_badge, ROM_TYPE_BADGE_MAX_PX) &&
          memcmp(g_badge, g_bref,
                 (size_t)ROM_TYPE_BADGE_W * ROM_TYPE_BADGE_H_FRLG * 2) == 0);
      printf("  verify: one FR/LG badge costs %ld reads unverified / %ld verified (%.1fx)\n",
             unverified_calls, verified_calls,
             unverified_calls ? (double)verified_calls / (double)unverified_calls : 0.0);
    } else chk("verify", "FireRed opens for the verification control", 0);
    free(rom);
  } else printf("SKIP verify control (no %s)\n", path);

  /* ---- and the LZ path fails closed too ----------------------------------- */
  snprintf(path, sizeof path, "%s/Emerald.gba", dir);
  rom = slurp(path, &n);
  if (!rom) { printf("SKIP verify tests (no %s)\n", path); return; }
  MemCtx mc = { rom, n, 0, 0 };
  if (rom_open(&rc, mem_read, &mc, n) && rom_itemart_open(&ra, &rc)) {
    mc.calls = 0;
    chk("verify", "a clean item icon succeeds",
        rom_item_icon(&ra, 13, g_ref, ROM_ITEM_ICON_PX));
    long verified_calls = mc.calls;
    rom_itemart_set_verify(&ra, 0);
    mc.calls = 0;
    rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX);
    long plain_calls = mc.calls;
    chk("verify", "verification does not change the pixels of a clean read",
        memcmp(g_icon, g_ref, sizeof g_ref) == 0);
    rom_itemart_set_verify(&ra, 1);

    mc.flaky = 1;
    chk("verify", "a corrupting reader makes rom_item_icon FAIL",
        !rom_item_icon(&ra, 13, g_icon, ROM_ITEM_ICON_PX));
    chk("verify", "a corrupting reader makes the RSE badge sheet FAIL",
        !rom_type_sheet_load(&ra, &ts, g_sheet, sizeof g_sheet));
    mc.flaky = 0;
    printf("  verify: one Emerald item icon costs %ld reads unverified / %ld verified (%.1fx)\n",
           plain_calls, verified_calls,
           plain_calls ? (double)verified_calls / (double)plain_calls : 0.0);

    /* BACKLOG #145: the RSE type sheet's own read-callback count, cap == want
     * (the dead-window shape rom_itemart.c:233/234/341/342's icon/palette
     * callers use on purpose, and what pdna_main.c's app_type_badge() USED to
     * pass) versus cap == MON_DECOMP_BYTES (8,192, what it passes now) --
     * proves the BACKLOG #103 caller-tail LZ77 window actually activates once
     * the caller supplies a real spare tail, and pins the AFTER count so a
     * regression that quietly shrinks the window back to 0 fails this test. */
    {
      RomTypeSheet tsw;
      mc.calls = 0;
      chk("perf#145", "type sheet loads with cap == want (window off)",
          rom_type_sheet_load(&ra, &ts, g_sheet, sizeof g_sheet));
      long narrow_calls = mc.calls;

      mc.calls = 0;
      chk("perf#145", "type sheet loads with cap == MON_DECOMP_BYTES (window on)",
          rom_type_sheet_load(&ra, &tsw, g_sheet_wide, sizeof g_sheet_wide));
      long wide_calls = mc.calls;

      chk("perf#145", "same pixels either way",
          memcmp(ts.pal, tsw.pal, sizeof ts.pal) == 0 &&
          memcmp(g_sheet, g_sheet_wide, sizeof g_sheet) == 0);
      /* BACKLOG #103's own measurement pinned Emerald's type sheet at ~86 reads
       * narrow / ~5 wide; a generous [1,10] band survives an unrelated FatFs
       * chunk-size change without masking a real regression back toward 86. */
      chk("perf#145", "widening the cap collapses the read count (narrow > 4x wide)",
          narrow_calls > wide_calls * 4);
      chk("perf#145", "the AFTER count stays in the pinned [1,10] band",
          wide_calls >= 1 && wide_calls <= 10);
      printf("  perf#145: Emerald type sheet costs %ld reads cap==want / %ld reads cap==MON_DECOMP_BYTES\n",
             narrow_calls, wide_calls);
    }
  } else chk("verify", "Emerald opens for the verification test", 0);
  free(rom);
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  /* run_host_tests.py hands every argv[1]-reading test the .sav corpus -- this test
   * wants the ROM DIRECTORY, so only accept an argument that is one. */
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }

  static const RomCase k_cases[] = {
    { "Emerald",   "Emerald.gba",   1, 377, ROM_TYPEART_RSE  },
    { "FireRed",   "FireRed.gba",   1, 375, ROM_TYPEART_FRLG },
    { "LeafGreen", "LeafGreen.gba", 1, 375, ROM_TYPEART_FRLG },
    /* Ruby/Sapphire: type badges yes, item icons NEVER -- the games have none. */
    { "Ruby",      "Ruby.gba",      0, 0,   ROM_TYPEART_RSE  },
    { "Sapphire",  "Sapphire.gba",  0, 0,   ROM_TYPEART_RSE  },
  };
  for (unsigned i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) run_rom(dir, &k_cases[i]);

  bounds_tests(dir);
  verify_tests(dir);

  printf("rom_itemart test: %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
