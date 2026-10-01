/* BACKLOG #103 -- rom_type_badge_memo(): the RSE type-sheet memo kept in the tail of the shared
 * 8 KiB art buffer (pdna_main.c's app_type_badge -> rom_itemart.c). Pins the epoch rule against
 * the REAL decoder on Guy's own ROM dumps (missing ROMs SKIP):
 *   - a second badge with no write in between is a HIT (zero ROM reads) and is pixel-exact;
 *   - a write by another decoder that claimed the buffer (epoch bump) while leaving the memo's
 *     own bytes intact (an item icon: 1,152 B at offset 0) forces a MISS -- the stale sheet is
 *     never drawn;
 *   - every call claims the buffer, hit or miss (other memos of the buffer must die);
 *   - a failed load leaves no live memo; a memo from a different RomItemArt is never used;
 *   - re-opening the SAME RomItemArt on another ROM is only safe with an epoch bump (Ruby vs
 *     Emerald badge art differ), which is why app_icon_rom_open() claims.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_typememo_test.c source/rom_itemart.c source/rom_map.c \
 *      source/map_render.c -o /tmp/htmemo && /tmp/htmemo
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "rom_map.h"
#include "rom_itemart.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const uint8_t* p; uint32_t n; long reads; int fail; } Img;
static bool rd(void* ctx, uint32_t off, void* dst, uint32_t len) {
  Img* m = (Img*)ctx;
  m->reads++;
  if (m->fail || off > m->n || len > m->n - off) return false;
  memcpy(dst, m->p + off, len);
  return true;
}
static uint8_t* slurp(const char* path, uint32_t* n) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* b = (uint8_t*)malloc((size_t)sz);
  if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = 0; }
  fclose(f);
  *n = (uint32_t)sz;
  return b;
}

static _Alignas(4) uint8_t g_buf[8192];
static uint16_t g_ref[2][ROM_TYPE_TYPES][ROM_TYPE_BADGE_MAX_PX];

/* every badge decoded the plain way (own scratch, no memo) -- the pixels the memo must match */
static int refs(const RomItemArt* ra, uint16_t out[ROM_TYPE_TYPES][ROM_TYPE_BADGE_MAX_PX]) {
  static _Alignas(4) uint8_t scratch[8192];
  RomTypeSheet ts;
  if (!rom_type_sheet_load(ra, &ts, scratch, sizeof scratch)) return 0;
  for (uint8_t t = 0; t < ROM_TYPE_TYPES; t++)
    if (!rom_type_badge(ra, &ts, t, out[t], ROM_TYPE_BADGE_MAX_PX)) return 0;
  return 1;
}
static int same(const uint16_t* px, const uint16_t* ref) {
  return px && memcmp(px, ref, (size_t)ROM_TYPE_BADGE_W * ROM_TYPE_BADGE_H_RSE * 2u) == 0;
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }
  char pe[512], pr[512];
  snprintf(pe, sizeof pe, "%s/Emerald.gba", dir);
  snprintf(pr, sizeof pr, "%s/Ruby.gba", dir);
  Img ie = { 0 }, ir = { 0 };
  ie.p = slurp(pe, &ie.n); ir.p = slurp(pr, &ir.n);
  if (!ie.p || !ir.p) { printf("SKIP (no %s or %s)\n", pe, pr); return 0; }
  RomCtx ce, cr;
  CHK(rom_open(&ce, rd, &ie, ie.n) && rom_open(&cr, rd, &ir, ir.n), "rom_open");
  RomItemArt ra, ra2, rref;
  CHK(rom_itemart_open(&rref, &ce) && refs(&rref, g_ref[0]), "Emerald reference badges");
  CHK(rom_itemart_open(&rref, &cr) && refs(&rref, g_ref[1]), "Ruby reference badges");
  CHK(rom_itemart_open(&ra, &ce) && rom_itemart_open(&ra2, &ce), "open Emerald twice");
  uint32_t epoch = 0;
  long r0;
  const uint16_t* px;

  /* 1. cold: a miss that reads the ROM; 2. warm: a hit with ZERO reads; both pixel-exact */
  r0 = ie.reads; px = rom_type_badge_memo(&ra, g_buf, sizeof g_buf, &epoch, 0);
  CHK(same(px, g_ref[0][0]) && ie.reads > r0, "cold call: badge 0 exact, sheet read (%ld reads)", ie.reads - r0);
  CHK(ie.reads - r0 <= 24, "the miss decodes through the buffer-tail LZ77 window (BACKLOG #145): %ld reads, a 64 B window costs ~90", ie.reads - r0);
  CHK(epoch == 1u, "the cold call claimed the buffer once (epoch %u)", epoch);
  r0 = ie.reads; px = rom_type_badge_memo(&ra, g_buf, sizeof g_buf, &epoch, 13);
  CHK(same(px, g_ref[0][13]) && ie.reads == r0, "warm call: badge 13 exact with 0 reads (%ld)", ie.reads - r0);
  CHK(epoch == 2u, "a HIT still claims the buffer (epoch %u): it rewrote the badge pixels", epoch);

  /* 3. another decoder claims and writes an item icon's 1,152 B over the sheet head; the memo
   *    bytes at the tail are untouched, so only the epoch can say the sheet is gone */
  epoch++; memset(g_buf, 0x5A, 1152);
  r0 = ie.reads; px = rom_type_badge_memo(&ra, g_buf, sizeof g_buf, &epoch, 2);
  CHK(same(px, g_ref[0][2]) && ie.reads > r0, "after a claimed foreign write: re-decoded, badge 2 exact (%ld reads)", ie.reads - r0);
  r0 = ie.reads; px = rom_type_badge_memo(&ra, g_buf, sizeof g_buf, &epoch, 4);
  CHK(same(px, g_ref[0][4]) && ie.reads == r0, "and the reload is memoised again (%ld reads)", ie.reads - r0);

  /* 4. a failed load (unreadable card) leaves nothing live: the next call reloads */
  epoch++; memset(g_buf, 0x5A, 1152);
  ie.fail = 1; px = rom_type_badge_memo(&ra, g_buf, sizeof g_buf, &epoch, 5); ie.fail = 0;
  CHK(px == 0, "a failed sheet load returns NULL");
  r0 = ie.reads; px = rom_type_badge_memo(&ra, g_buf, sizeof g_buf, &epoch, 5);
  CHK(same(px, g_ref[0][5]) && ie.reads > r0, "after a failed load the next call reloads (%ld reads)", ie.reads - r0);

  /* 5. a different RomItemArt never reuses this memo, even with no write in between */
  r0 = ie.reads; px = rom_type_badge_memo(&ra2, g_buf, sizeof g_buf, &epoch, 6);
  CHK(same(px, g_ref[0][6]) && ie.reads > r0, "another RomItemArt misses (%ld reads)", ie.reads - r0);

  /* 6. the SAME struct re-opened on Ruby: with the epoch bump app_icon_rom_open() does, the
   *    badges are Ruby's own (and Ruby's art really differs somewhere, or this proves nothing) */
  int differ = 0;
  for (int t = 0; t < ROM_TYPE_TYPES; t++) differ |= !same(g_ref[0][t], g_ref[1][t]);
  CHK(differ, "Emerald and Ruby badge art differ in at least one badge");
  CHK(rom_itemart_open(&ra2, &cr), "re-open ra2 on Ruby");
  epoch++;
  int all = 1;
  for (uint8_t t = 0; t < ROM_TYPE_TYPES; t++) all &= same(rom_type_badge_memo(&ra2, g_buf, sizeof g_buf, &epoch, t), g_ref[1][t]);
  CHK(all, "after a re-open + claim every badge is Ruby's");

  /* 7. argument refusals */
  CHK(!rom_type_badge_memo(&ra, g_buf + 2, sizeof g_buf - 2, &epoch, 0), "an unaligned buffer is refused");
  CHK(!rom_type_badge_memo(&ra, g_buf, ROM_TYPE_MEMO_END - 1u, &epoch, 0), "a buffer short of the memo is refused");
  CHK(!rom_type_badge_memo(&ra, g_buf, sizeof g_buf, 0, 0), "a NULL epoch is refused");

  printf("type memo: %d checks, %d fail%s\n", checks, fails, fails == 1 ? "" : "s");
  free((void*)ie.p); free((void*)ir.p);
  return fails ? 1 : 0;
}
