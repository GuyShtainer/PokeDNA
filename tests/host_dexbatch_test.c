/* Host test for BACKLOG #404 -- the Gen-2 Pokedex grid's first paint opens the ROM once PER
 * ICON on the SD path; pdna_pick.c's dex_grid_cells() now brackets the page pass in
 * gb_art_batch_begin/_end exactly like pdna_box.c's era pass. This test runs the REAL
 * source/gb_art_source.c (SD half) + pdna_origin_art.c + rom_gbicon.c against Guy's own
 * Crystal.gbc through a counting FatFs shim (tests/hostshim/ff.h), fetches the same 21
 * icons one-shot and then inside a batch, and compares:
 *   - ROM f_open count   (on a cart: one root-dir walk + ROM re-validate per open)
 *   - f_read count       (the read callback gb_art_read() makes, one per ROM transfer)
 *   - the pixels         (the batch must return byte-identical icons)
 * pdna_pick.c itself is tonc-bound and not host-compilable; that the dex pass is bracketed
 * is pinned textually by tests/host_zd_sites_test.py, and by the mGBA frame/log in
 * docs/emu-evidence/zd/.
 *
 *   cc -std=c11 -O1 -w -I tests/hostshim -I source -I lib tests/host_dexbatch_test.c \
 *      source/gb_art_source.c source/pdna_origin_art.c source/gen12_convert.c source/gen3_mon.c \
 *      source/gen3_edit.c source/gen3_daycare.c source/gen3_save.c source/gen3_box.c \
 *      source/gen1_save.c source/gen2_save.c source/data_tables.c source/rom_sprite.c \
 *      source/map_render.c source/rom_map.c source/artbuf.c source/sprite_era.c \
 *      source/rom_gbsprite.c source/rom_gbicon.c source/rom_gbitem.c source/gb_sprite_codec.c \
 *      -o /tmp/hdb && /tmp/hdb
 *
 * The ROM is Guy's own dump (outside the repo): a missing corpus SKIPS (exit 0). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "ff.h"
#include "gb_art_source.h"
#include "pdna_origin_art.h"
#include "pdna_app.h"

#define ROM_FILE "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb/Crystal.gbc"
#define ROM_PATH "/Crystal.gbc"
#define ROM1_FILE "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb/Red.gb"
#define ROM1_PATH "/Red.gb"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); g_fail++; } } while (0)

/* ---- counting FatFs shim: the ROM from the corpus, every other path from RAM ---------- */
static unsigned long c_rom_open, c_rom_read, c_rom_seek, c_other_open;
static struct { char path[64]; unsigned char d[4096]; uint32_t n; int used; } s_mem[8];

static int mem_find(const char* p, int create) {
  for (int i = 0; i < 8; i++) if (s_mem[i].used && !strcmp(s_mem[i].path, p)) return i;
  if (!create) return -1;
  for (int i = 0; i < 8; i++) if (!s_mem[i].used) {
    s_mem[i].used = 1; s_mem[i].n = 0; snprintf(s_mem[i].path, sizeof s_mem[i].path, "%s", p); return i;
  }
  return -1;
}
FRESULT f_open(FIL* fp, const char* path, uint8_t mode) {
  memset(fp, 0, sizeof *fp); fp->mem = -1;
  if (!strcmp(path, ROM_PATH) || !strcmp(path, ROM1_PATH)) {
    fp->fp = fopen(!strcmp(path, ROM_PATH) ? ROM_FILE : ROM1_FILE, "rb");
    if (!fp->fp) return FR_NO_FILE;
    fseek(fp->fp, 0, SEEK_END); fp->fsize = (FSIZE_t)ftell(fp->fp); fseek(fp->fp, 0, SEEK_SET);
    c_rom_open++;
    return FR_OK;
  }
  c_other_open++;
  int i = mem_find(path, (mode & FA_WRITE) ? 1 : 0);
  if (i < 0) return FR_NO_FILE;
  if (mode & FA_CREATE_ALWAYS) s_mem[i].n = 0;
  fp->mem = i; fp->fsize = s_mem[i].n;
  return FR_OK;
}
FRESULT f_close(FIL* fp) { if (fp->fp) { fclose(fp->fp); fp->fp = 0; } return FR_OK; }
FRESULT f_lseek(FIL* fp, FSIZE_t ofs) {
  if (fp->fp) { c_rom_seek++; fseek(fp->fp, (long)ofs, SEEK_SET); }
  fp->fptr = ofs; return FR_OK;
}
FRESULT f_read(FIL* fp, void* buf, UINT btr, UINT* br) {
  if (fp->fp) {
    c_rom_read++;
    *br = (UINT)fread(buf, 1, btr, fp->fp); fp->fptr += *br; return FR_OK;
  }
  if (fp->mem < 0) { *br = 0; return FR_DISK_ERR; }
  uint32_t n = s_mem[fp->mem].n, avail = fp->fptr < n ? n - fp->fptr : 0;
  *br = btr < avail ? btr : avail;
  memcpy(buf, s_mem[fp->mem].d + fp->fptr, *br); fp->fptr += *br; return FR_OK;
}
FRESULT f_write(FIL* fp, const void* buf, UINT btw, UINT* bw) {
  if (fp->mem < 0 || fp->fptr + btw > sizeof s_mem[0].d) return FR_DENIED;
  memcpy(s_mem[fp->mem].d + fp->fptr, buf, btw); fp->fptr += btw;
  if (fp->fptr > s_mem[fp->mem].n) s_mem[fp->mem].n = fp->fptr;
  *bw = btw; return FR_OK;
}

/* ---- the firmware symbols gb_art_source.c / pdna_origin_art.c reach for ---------------- */
char __iheap_start[4];
void log_line(const char* f, ...) { (void)f; }
uint32_t perf_ticks(void) { return 0; }
uint32_t perf_ms(uint32_t t) { return t; }
void rmbl_pause(void) {}
void rmbl_resume(void) {}
bool app_rom_art_off(void) { return false; }
bool app_current_save_is_gb(void) { return true; }
const char* app_current_save_path(void) { return "/Crystal.sav"; }
const char* app_gb_rom_path(uint8_t gen) { return gen == 2 ? ROM_PATH : gen == 1 ? ROM1_PATH : ""; }
bool app_can_edit(void) { return false; }
const uint16_t* mon_front_for_form(uint16_t s, bool sh, uint8_t f) { (void)s; (void)sh; (void)f; return 0; }
const uint16_t* mon_back_for_form(uint16_t s, bool sh, uint8_t f) { (void)s; (void)sh; (void)f; return 0; }
const uint16_t* mon_front_egg(void) { return 0; }
int gb_char_decode(uint8_t gen, uint8_t c, char out[GB_GLYPH_MAX]) { (void)gen; out[0] = (char)c; out[1] = 0; return 1; }

static int room_ok(int need) { (void)need; return 1; }   /* the real hook reads the ARM sp */

static uint32_t fnv(const void* p, size_t n) {
  const uint8_t* b = p; uint32_t h = 2166136261u;
  while (n--) { h ^= *b++; h *= 16777619u; }
  return h;
}

#define NICON 21
static uint32_t g_h_one[NICON], g_h_bat[NICON];

static int fetch21(uint32_t* hs) {
  int ok = 0;
  for (int d = 1; d <= NICON; d++) {
    PdnaArt a;
    if (pdna_origin_art_icon((uint16_t)d, &a) && a.px && a.w == 16 && a.h == 16) {
      hs[d - 1] = fnv(a.px, 16 * 16 * 2); ok++;
    } else hs[d - 1] = 0;
  }
  return ok;
}

int main(void) {
  FILE* t = fopen(ROM_FILE, "rb");
  FILE* t1 = fopen(ROM1_FILE, "rb");
  if (t1) fclose(t1);
  if (!t || !t1) { printf("SKIP (host_dexbatch_test: Crystal.gbc not in the corpus, NOTHING RAN)\n"); return 0; }
  fclose(t);

  gb_art_boot_register(0, 0);   /* registers the vtable + scans ROM_PATH once (writes the .loc into RAM) */
  pdna_origin_art_set_stack_room_hook(room_ok);
  CHECK(gb_art_have(2), "Crystal registered");

  /* warm: one one-shot fetch so every cache a real second visit has (the .loc files) exists */
  { uint32_t w[NICON]; (void)fetch21(w); }

  /* A. one-shot: the pre-#404 dex path (no batch) */
  c_rom_open = c_rom_read = c_rom_seek = 0;
  int n1 = fetch21(g_h_one);
  unsigned long open1 = c_rom_open, read1 = c_rom_read;
  printf("  one-shot : %d icons, ROM f_open %lu, f_read %lu\n", n1, open1, read1);

  /* B. batched: the dex_grid_cells() shape */
  c_rom_open = c_rom_read = c_rom_seek = 0;
  GbArtBatch bt;
  gb_art_batch_begin(&bt);
  int n2 = fetch21(g_h_bat);
  gb_art_batch_end(&bt);
  unsigned long open2 = c_rom_open, read2 = c_rom_read;
  printf("  batched  : %d icons, ROM f_open %lu, f_read %lu\n", n2, open2, read2);

  CHECK(n1 == NICON && n2 == NICON, "all 21 Crystal icons fetched in both modes (%d / %d)", n1, n2);
  CHECK(open1 == NICON, "one-shot opens the ROM once per icon (%lu)", open1);
  CHECK(open2 == 1, "batched opens the ROM exactly once for the 21-icon page (%lu)", open2);
  CHECK(read2 < read1, "batched makes fewer ROM reads than one-shot (%lu < %lu)", read2, read1);
  CHECK(!memcmp(g_h_one, g_h_bat, sizeof g_h_one), "batched icons are pixel-identical to one-shot");

  /* C. after end() the router is unrouted: a fetch is one-shot again (one open per icon) */
  c_rom_open = 0;
  PdnaArt a; (void)pdna_origin_art_icon(1, &a); (void)pdna_origin_art_icon(2, &a);
  CHECK(c_rom_open == 2, "after gb_art_batch_end a fetch is one-shot again (%lu opens for 2)", c_rom_open);

  /* D. a pass that fetches nothing costs no I/O (begin() does none) */
  c_rom_open = c_rom_read = 0;
  gb_art_batch_begin(&bt); gb_art_batch_end(&bt);
  CHECK(c_rom_open == 0 && c_rom_read == 0, "empty batch pass = 0 opens, 0 reads");

  /* E. the Gen-1 dex path (gbdex_fetch_gen1 -> pdna_origin_art_portrait_by_dex_gen) rides the
   * same batch: Red.gb, 21 front sprites, one open instead of 21 */
  {
    uint32_t h1[NICON], h2[NICON]; int m1 = 0, m2 = 0;
    c_rom_open = c_rom_read = 0;
    for (int d = 1; d <= NICON; d++) { PdnaArt g; memset(&g, 0, sizeof g);
      if (pdna_origin_art_portrait_by_dex_gen(PDNA_GEN1, (uint16_t)d, &g) && g.px) { h1[d-1] = fnv(g.px, (size_t)g.w * g.h * 2); m1++; } else h1[d-1] = 0; }
    unsigned long o1 = c_rom_open, r1 = c_rom_read;
    c_rom_open = c_rom_read = 0;
    gb_art_batch_begin(&bt);
    for (int d = 1; d <= NICON; d++) { PdnaArt g; memset(&g, 0, sizeof g);
      if (pdna_origin_art_portrait_by_dex_gen(PDNA_GEN1, (uint16_t)d, &g) && g.px) { h2[d-1] = fnv(g.px, (size_t)g.w * g.h * 2); m2++; } else h2[d-1] = 0; }
    gb_art_batch_end(&bt);
    printf("  gen1 one-shot: %d sprites, ROM f_open %lu, f_read %lu; batched: %d sprites, f_open %lu, f_read %lu\n",
           m1, o1, r1, m2, c_rom_open, c_rom_read);
    CHECK(m1 == NICON && m2 == NICON, "all 21 Red sprites fetched (%d / %d)", m1, m2);
    CHECK(o1 == NICON && c_rom_open == 1, "gen1: 21 opens one-shot -> 1 batched (%lu -> %lu)", o1, c_rom_open);
    CHECK(!memcmp(h1, h2, sizeof h1), "gen1 batched sprites are pixel-identical");
  }

  printf("host_dexbatch_test: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
