/* The whole-ROM scan window (source/gb_scanwin.h) and the SD-read stop rules
 * (source/gb_scan_guard.h) behind the 2026-09-14 "stuck choosing the .gbc" cart fix,
 * plus the FatFs transaction count that fix exists to cut, measured on the real
 * lib/fatfs over tests/hostfat's RAM disk.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_gbscan_test.c source/rom_gbsprite.c source/rom_gbicon.c source/rom_gbui.c \
 *      source/gb_sprite_codec.c lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hgbscan && /tmp/hgbscan
 *
 * A. gb_scanwin on synthetic buffers, never skips: every read is chunk-aligned, chunk
 *    is a power of two, reads only move forward, every position 0..size-look is
 *    offered EXACTLY once with the right bytes under it (the carry memmove), the
 *    read count is the closed form, init refuses what it must.
 * B. the three locators on Guy's real dumps (SKIPs without them): a 2 KB, an 8 KB and a
 *    64 KB scratch locate byte-identical tables, at the addresses rom_gbsprite.h
 *    documents; the scan's reads are forward and aligned; a read failure unwinds
 *    without another read.
 * C. gb_scan_guard with a fake clock, never skips: ticks, the timeout edge, wrap
 *    safety, first-reason-wins latching, hi as a high-water mark.
 * D. FatFs transactions for Gold.gbc's sprite scan through a shim identical to
 *    gb_art_source.c's (f_lseek + f_read): the number the EZ-Flash actually charges
 *    for. BEFORE this fix, same harness, 2 KB scratch: 8,592 disk_read calls, 12,954
 *    sectors, 2,490 backward seeks. The bounds below are ~10% over what the fixed
 *    scanner measures (545 / 4,132 / 6 at 8 KB), so a regression to the old pattern
 *    fails by an order of magnitude, not by a flaky margin.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb_scanwin.h"
#include "gb_scan_guard.h"
#include "rom_gbsprite.h"
#include "rom_gbicon.h"
#include "rom_gbui.h"
#include "ff.h"
#include "ramdisk.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb/"

static int g_fail = 0, g_check = 0;
static void chk(const char* who, const char* what, int cond) {
  g_check++;
  if (!cond) { printf("  !! FAIL [%s] %s\n", who, what); g_fail++; }
}

/* ------------------------------------------------------------------ part A */

static uint8_t  a_src[70000];
static uint8_t  a_w[16384];
static uint8_t  a_seen[70000];

static int is_pow2(uint32_t v) { return v && (v & (v - 1u)) == 0; }

static void part_a_one(uint32_t size, uint32_t cap, uint32_t look) {
  char who[64];
  snprintf(who, sizeof who, "A size=%u cap=%u look=%u", size, cap, look);
  GbScanWin sw;
  uint32_t ovl_min = ((look - 1u) + 511u) / 512u * 512u;
  int ok = gb_scanwin_init(&sw, size, cap, look);
  if (!ok) {
    chk(who, "init refused only when cap < ovl_min+512 or size < look",
        cap < ovl_min + 512u || size < look);
    return;
  }
  chk(who, "init accepted only when it fits", cap >= ovl_min + 512u && size >= look);
  chk(who, "chunk is a power of two >= 512", is_pow2(sw.chunk) && sw.chunk >= 512u);
  chk(who, "carry is a multiple of chunk", sw.ovl % sw.chunk == 0);
  chk(who, "carry covers look-1", sw.ovl >= look - 1u);
  chk(who, "carry + chunk fits the scratch", sw.ovl + sw.chunk <= cap);
  /* the largest such chunk: doubling it must not fit */
  {
    uint32_t c2 = sw.chunk * 2u;
    uint32_t ovl2 = (ovl_min + c2 - 1u) / c2 * c2;
    chk(who, "chunk is the largest power of two that fits", !(c2 <= cap - ovl_min && ovl2 + c2 <= cap));
  }

  memset(a_seen, 0, size);
  uint32_t reads = 0, last_off = 0, lasts = 0;
  while (gb_scanwin_plan(&sw, a_w)) {
    reads++;
    chk(who, "read offset is a multiple of chunk", sw.rd_off % sw.chunk == 0);
    chk(who, "read lands chunk-aligned in the scratch", sw.rd_dst % sw.chunk == 0);
    chk(who, "read stays inside the file", sw.rd_off + sw.rd_len <= size && sw.rd_len > 0);
    chk(who, "read stays inside the scratch", sw.rd_dst + sw.rd_len <= cap);
    chk(who, "reads only move forward", reads == 1 || sw.rd_off > last_off);
    int last_read = (sw.rd_off + sw.rd_len == size);
    chk(who, "every read but the last is exactly a chunk (first: carry+chunk)",
        last_read || sw.rd_len == (reads == 1 ? sw.ovl + sw.chunk : sw.chunk));
    last_off = sw.rd_off;
    memcpy(a_w + sw.rd_dst, a_src + sw.rd_off, sw.rd_len);
    uint32_t cnt = gb_scanwin_filled(&sw);
    if (gb_scanwin_is_last(&sw)) lasts++;
    chk(who, "is_last only on the read that reaches the end", gb_scanwin_is_last(&sw) == last_read);
    for (uint32_t i = sw.first; i < sw.first + cnt; i++) {
      uint32_t pos = sw.base + i;
      if (pos + look > size) { chk(who, "position within the file", 0); break; }
      if (memcmp(a_w + i, a_src + pos, look) != 0) { chk(who, "window bytes match the file", 0); break; }
      if (pos < sizeof a_seen) a_seen[pos]++;
    }
  }
  chk(who, "is_last fired exactly once", lasts == 1);
  int once = 1;
  for (uint32_t p = 0; p + look <= size; p++) if (a_seen[p] != 1) { once = 0; break; }
  chk(who, "every position 0..size-look offered exactly once", once);
  uint32_t first_len = sw.ovl + sw.chunk;
  uint32_t want = (size <= first_len) ? 1u : 1u + (size - first_len + sw.chunk - 1u) / sw.chunk;
  chk(who, "read count is the closed form", reads == want);
}

static void part_a(void) {
  uint32_t x = 0x12345678u;
  for (size_t i = 0; i < sizeof a_src; i++) { x = x * 1664525u + 1013904223u; a_src[i] = (uint8_t)(x >> 24); }
  static const uint32_t looks[] = { 1, 11, 33, 251, 1206, 1537 };
  static const uint32_t caps[]  = { 512, 1024, 2048, 2560, 4096, 6000, 8192, 9000, 16384 };
  static const uint32_t sizes[] = { 1, 2, 511, 512, 513, 1206, 1207, 4095, 4096, 4097, 8192, 8193,
                                    12345, 16384, 20000, 65536, 65537, 70000 };
  for (size_t l = 0; l < sizeof looks / sizeof looks[0]; l++)
    for (size_t c = 0; c < sizeof caps / sizeof caps[0]; c++)
      for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
        part_a_one(sizes[s], caps[c], looks[l]);

  /* the two production configurations, by name */
  GbScanWin sw;
  chk("A", "rom_gbsprite through mon_decomp: 4 KB chunks, 4 KB carry",
      gb_scanwin_init(&sw, 2u << 20, 8192, 1206) && sw.chunk == 4096 && sw.ovl == 4096);
  chk("A", "rom_gbsprite through its 2 KB minimum: 512 B chunks, 1536 B carry",
      gb_scanwin_init(&sw, 2u << 20, 2048, 1206) && sw.chunk == 512 && sw.ovl == 1536);
  chk("A", "a 2 MB ROM through mon_decomp is 511 reads",
      gb_scanwin_init(&sw, 2u << 20, 8192, 1206) &&
      1u + ((2u << 20) - 8192u + 4095u) / 4096u == 511u);
  chk("A", "NULL window refused", !gb_scanwin_init(0, 100, 8192, 10));
  chk("A", "zero look refused", !gb_scanwin_init(&sw, 100, 8192, 0));
}

/* ------------------------------------------------------------------ part B */

typedef struct {
  FILE*    f;
  uint32_t size;
  uint32_t calls, big_calls, back_big;
  uint32_t last_big_end;
  int      fail_at;          /* >=0: return false from this call on, forever */
  int      calls_after_fail;
} Rd;

static bool rd_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  Rd* r = (Rd*)ctx;
  if (r->fail_at >= 0 && (int)r->calls >= r->fail_at) { r->calls_after_fail++; r->calls++; return false; }
  r->calls++;
  if (len >= 4096) {           /* a scan chunk (the verifies are all far smaller) */
    if (off % 4096u) r->back_big = 0xFFFFFFFFu;                /* misaligned: poison */
    else if (off < r->last_big_end) r->back_big++;
    r->last_big_end = off + len;
    r->big_calls++;
  }
  if (fseek(r->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, r->f) == len;
}

static int rd_open(Rd* r, const char* name) {
  char p[256]; snprintf(p, sizeof p, "%s%s", ROMS, name);
  memset(r, 0, sizeof *r); r->fail_at = -1;
  r->f = fopen(p, "rb"); if (!r->f) return 0;
  fseek(r->f, 0, SEEK_END); r->size = (uint32_t)ftell(r->f); fseek(r->f, 0, SEEK_SET);
  return 1;
}

static uint8_t b_scratch[65536] __attribute__((aligned(8)));

typedef struct { uint32_t base_stats, mew_stats, base_data, pic_ptrs, palettes; } Want;

static void part_b_rom(const char* name, uint8_t gen, const Want* want, int has_icons) {
  Rd r;
  if (!rd_open(&r, name)) { printf("  %s: SKIP (dump not present)\n", name); return; }
  char who[48]; snprintf(who, sizeof who, "B %s", name);
  static const uint32_t caps[] = { 2048, 8192, 65536 };
  RomGbSpriteLoc sl[3]; RomGbIconLoc il[3]; RomGbUiLoc ul[3];
  int iok[3] = { 0, 0, 0 };
  for (int c = 0; c < 3; c++) {
    RomGbSprite gs;
    r.calls = r.big_calls = r.back_big = r.last_big_end = 0;
    int ok = rom_gbsprite_open(&gs, rd_read, &r, r.size, b_scratch, caps[c]);
    chk(who, "sprite locator opens", ok);
    if (ok) {
      chk(who, "generation as expected", (uint8_t)gs.gen == gen);
      rom_gbsprite_save_loc(&gs, &sl[c]);
      if (caps[c] >= 8192) {
        chk(who, "scan chunks are 4 KB-aligned and forward-only", r.back_big == 0);
        chk(who, "scan chunk count <= 1 + size/4096", r.big_calls <= 1u + r.size / 4096u);
      }
    }
    RomGbIcon gi;
    r.calls = r.big_calls = r.back_big = r.last_big_end = 0;
    iok[c] = rom_gbicon_open(&gi, rd_read, &r, r.size, b_scratch, caps[c]);
    chk(who, "icon locator answers as expected", iok[c] == has_icons);
    if (iok[c]) rom_gbicon_save_loc(&gi, &il[c]);
    RomGbUi gu;
    r.calls = r.big_calls = r.back_big = r.last_big_end = 0;
    int uok = rom_gbui_open(&gu, rd_read, &r, r.size, b_scratch, caps[c]);
    chk(who, "ui locator opens", uok);
    if (uok) {
      rom_gbui_save_loc(&gu, &ul[c]);
      if (caps[c] >= 8192) chk(who, "ui scan chunks are aligned and forward-only", r.back_big == 0);
    }
  }
  chk(who, "sprite loc identical across 2K/8K/64K scratch",
      memcmp(&sl[0], &sl[1], sizeof sl[0]) == 0 && memcmp(&sl[1], &sl[2], sizeof sl[0]) == 0);
  chk(who, "ui loc identical across scratch sizes",
      memcmp(&ul[0], &ul[1], sizeof ul[0]) == 0 && memcmp(&ul[1], &ul[2], sizeof ul[0]) == 0);
  if (has_icons)
    chk(who, "icon loc identical across scratch sizes",
        memcmp(&il[0], &il[1], sizeof il[0]) == 0 && memcmp(&il[1], &il[2], sizeof il[0]) == 0);
  if (want) {
    chk(who, "BaseStats/BaseData at the documented address",
        gen == 1 ? sl[0].base_stats == want->base_stats : sl[0].base_data == want->base_data);
    if (gen == 1) chk(who, "Mew record where documented", sl[0].mew_stats == want->mew_stats);
    if (gen == 2) {
      chk(who, "PicPointers where documented", sl[0].pic_ptrs == want->pic_ptrs);
      chk(who, "Palettes where documented", sl[0].palettes == want->palettes);
    }
  }
  /* a read failure unwinds: no read is attempted after the failing one */
  for (int at = 0; at < 40; at += 13) {
    RomGbSprite gs;
    r.calls = 0; r.calls_after_fail = 0; r.fail_at = at;
    int ok = rom_gbsprite_open(&gs, rd_read, &r, r.size, b_scratch, 8192);
    chk(who, "open fails when a read fails", !ok);
    chk(who, "no read follows the failing one (scanner unwinds)", r.calls_after_fail == 1);
  }
  r.fail_at = -1;
  fclose(r.f);
}

static void part_b(void) {
  static const Want red     = { 0x383DE, 0x0425B, 0, 0, 0 };
  static const Want yellow  = { 0x383DE, 0,       0, 0, 0 };
  static const Want gold    = { 0, 0, 0x51B0B, 0x48000,  0x0AD3D };
  static const Want crystal = { 0, 0, 0x51424, 0x120000, 0x0A8CE };
  part_b_rom("Red.gb",      1, &red,     0);
  part_b_rom("Yellow.gb",   1, &yellow,  0);
  part_b_rom("Gold.gbc",    2, &gold,    1);
  part_b_rom("Crystal.gbc", 2, &crystal, 1);
}

/* ------------------------------------------------------------------ part C */

static void part_c(void) {
  GbScanGuard g; int tick;
  gb_scan_guard_init(&g, 1000, 100, 50, 7);
  chk("C", "fresh guard is green", g.stop == GB_SCAN_OK && g.reads == 0 && g.hi == 0);
  chk("C", "first read admitted with a tick", gb_scan_guard_admit(&g, 0, 10, 100, &tick) && tick == 1);
  chk("C", "hi tracks off+len", g.hi == 10 && g.reads == 1);
  chk("C", "second read: no tick", gb_scan_guard_admit(&g, 500, 10, 101, &tick) && tick == 0);
  chk("C", "hi is a high-water mark, not a sum", g.hi == 510);
  chk("C", "a backward read does not lower hi", gb_scan_guard_admit(&g, 20, 5, 102, &tick) && g.hi == 510);
  for (int i = 3; i < 8; i++) chk("C", "reads 4..8: no tick", gb_scan_guard_admit(&g, 0, 1, 103, &tick) && tick == 0);
  chk("C", "9th read (reads==8) ticks", gb_scan_guard_admit(&g, 0, 1, 104, &tick) && tick == 1);
  chk("C", "elapsed == limit is still allowed", gb_scan_guard_admit(&g, 0, 1, 150, &tick));
  chk("C", "elapsed > limit refuses with TIMEOUT",
      !gb_scan_guard_admit(&g, 0, 1, 151, &tick) && g.stop == GB_SCAN_STOP_TIMEOUT && tick == 0);
  uint32_t reads_then = g.reads;
  chk("C", "latched: an earlier clock does not revive it",
      !gb_scan_guard_admit(&g, 0, 1, 100, &tick) && g.reads == reads_then);
  gb_scan_guard_fail(&g);
  chk("C", "first reason wins: fail() after TIMEOUT keeps TIMEOUT", g.stop == GB_SCAN_STOP_TIMEOUT);
  gb_scan_guard_cancel(&g);
  chk("C", "first reason wins: cancel() after TIMEOUT keeps TIMEOUT", g.stop == GB_SCAN_STOP_TIMEOUT);

  gb_scan_guard_init(&g, 1000, 0, 50, 7);
  gb_scan_guard_cancel(&g);
  chk("C", "cancel latches", !gb_scan_guard_admit(&g, 0, 1, 0, &tick) && g.stop == GB_SCAN_STOP_CANCEL);
  gb_scan_guard_fail(&g);
  chk("C", "fail() after cancel keeps CANCEL", g.stop == GB_SCAN_STOP_CANCEL);

  gb_scan_guard_init(&g, 1000, 0, 50, 7);
  chk("C", "one admitted read", gb_scan_guard_admit(&g, 0, 1, 0, &tick));
  gb_scan_guard_fail(&g);
  chk("C", "fail latches READ_ERR", !gb_scan_guard_admit(&g, 0, 1, 0, &tick) && g.stop == GB_SCAN_STOP_READ_ERR);
  chk("C", "reads counts only admitted reads", g.reads == 1);
  gb_scan_guard_cancel(&g);
  chk("C", "cancel() after fail keeps READ_ERR", g.stop == GB_SCAN_STOP_READ_ERR);

  gb_scan_guard_init(&g, 1000, 0xFFFFFF00u, 0, 7);
  chk("C", "limit 0 never times out, even across a clock wrap", gb_scan_guard_admit(&g, 0, 1, 0x100, &tick));
  gb_scan_guard_init(&g, 1000, 0xFFFFFFF0u, 50, 7);
  chk("C", "wrap-safe elapsed: 32 units across the wrap is under 50", gb_scan_guard_admit(&g, 0, 1, 0x10, &tick));
  chk("C", "wrap-safe elapsed: 80 units across the wrap times out",
      !gb_scan_guard_admit(&g, 0, 1, 0x40, &tick) && g.stop == GB_SCAN_STOP_TIMEOUT);

  gb_scan_guard_init(&g, 0xFFFFFFFFu, 0, 0, 0);
  chk("C", "off+len overflow saturates hi", gb_scan_guard_admit(&g, 0xFFFFFFF0u, 0x20, 0, &tick) && g.hi == 0xFFFFFFFFu);
  chk("C", "tick_mask 0 ticks every read", gb_scan_guard_admit(&g, 0, 1, 0, &tick) && tick == 1);
  chk("C", "NULL tick pointer is fine", gb_scan_guard_admit(&g, 0, 1, 0, 0));
  chk("C", "the cart's limit is 60 s of 16,384 Hz ticks", 60u * 16384u == 983040u);
}

/* ------------------------------------------------------------------ part D */

static FATFS d_fs;
static BYTE  d_work[FF_MAX_SS * 2];
typedef struct { FIL* f; unsigned long calls; } Shim;

/* byte-for-byte the seek+read gb_art_source.c's shim performs */
static bool shim_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  Shim* s = (Shim*)ctx; UINT br = 0; s->calls++;
  if ((FSIZE_t)off != s->f->fptr && f_lseek(s->f, (FSIZE_t)off) != FR_OK) return false;
  if (f_read(s->f, buf, (UINT)len, &br) != FR_OK) return false;
  return br == len;
}

static void part_d(void) {
  char p[256]; snprintf(p, sizeof p, "%s%s", ROMS, "Gold.gbc");
  FILE* f = fopen(p, "rb");
  if (!f) { printf("  D: SKIP (Gold.gbc not present)\n"); return; }
  fseek(f, 0, SEEK_END); uint32_t n = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char* b = malloc(n);
  if (fread(b, 1, n, f) != n) { fclose(f); free(b); chk("D", "read the dump", 0); return; }
  fclose(f);

  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 4096 };   /* 4 KB clusters: the worst case */
  f_mount(0, "", 0);
  rd_init(16384);
  chk("D", "mkfs", f_mkfs("", &opt, d_work, sizeof d_work) == FR_OK);
  chk("D", "mount", f_mount(&d_fs, "", 1) == FR_OK);
  FIL w; UINT bw;
  chk("D", "create", f_open(&w, "/rom.bin", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
  for (uint32_t o = 0; o < n; o += 65536) {
    UINT want = (n - o > 65536) ? 65536 : (UINT)(n - o);
    if (f_write(&w, b + o, want, &bw) != FR_OK || bw != want) { chk("D", "write", 0); break; }
  }
  f_close(&w);

  static const struct { unsigned cap; unsigned long calls, sectors, back; } bound[] = {
    { 8192, 600, 4600, 10 },     /* measured 545 / 4132 / 6  (BEFORE: 8592 / 12954 / 2490) */
    { 2048, 4600, 4600, 10 },    /* measured 4126 / 4132 / 6 (BEFORE: 8592 / 12954 / 2490) */
  };
  RomGbSpriteLoc loc; int have_loc = 0;
  for (int i = 0; i < 2; i++) {
    FIL fil; Shim s = { &fil, 0 };
    chk("D", "open for read", f_open(&fil, "/rom.bin", FA_READ) == FR_OK);
    rd_read_calls = rd_reads = rd_read_back = 0;
    RomGbSprite gs;
    int ok = rom_gbsprite_open(&gs, shim_read, &s, n, b_scratch, bound[i].cap);
    char who[48]; snprintf(who, sizeof who, "D scratch=%u", bound[i].cap);
    chk(who, "locates Gold", ok);
    printf("  %s: %lu f_reads -> %lu disk_read calls, %lu sectors, %lu backward seeks\n",
           who, s.calls, rd_read_calls, rd_reads, rd_read_back);
    chk(who, "disk_read transactions within bound", rd_read_calls <= bound[i].calls);
    chk(who, "sectors within bound (~1x the file)", rd_reads <= bound[i].sectors);
    chk(who, "backward seeks within bound", rd_read_back <= bound[i].back);
    if (ok && !have_loc) { rom_gbsprite_save_loc(&gs, &loc); have_loc = 1; }
    f_close(&fil);
  }
  /* the loc-hit path every box-cell fetch takes: a few dozen transactions, not a scan */
  if (have_loc) {
    FIL fil; Shim s = { &fil, 0 };
    f_open(&fil, "/rom.bin", FA_READ);
    rd_read_calls = rd_reads = rd_read_back = 0;
    RomGbSprite gs;
    int ok = rom_gbsprite_open_loc(&gs, shim_read, &s, n, b_scratch, 8192, &loc);
    printf("  D loc-hit: %lu f_reads -> %lu disk_read calls\n", s.calls, rd_read_calls);
    chk("D loc-hit", "validates from the cache", ok);
    chk("D loc-hit", "under 40 transactions (measured 26)", rd_read_calls <= 40);
    f_close(&fil);
  }
  f_mount(0, "", 0);
  rd_free();
  free(b);
}

int main(void) {
  part_a();
  part_b();
  part_c();
  part_d();
  printf("%d checks, %d failures\n", g_check, g_fail);
  if (g_fail) { printf("FAIL\n"); return 1; }
  printf("ALL PASS\n");
  return 0;
}
