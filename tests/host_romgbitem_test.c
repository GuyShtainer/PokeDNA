/* Host (PC) test for rom_gbitem -- Gen-2 item descriptions from REAL Gold/Crystal dumps
 * (BACKLOG #340b).
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_romgbitem_test.c source/rom_gbitem.c source/gb_edit.c \
 *      source/gen1_save.c source/gen2_save.c source/data_tables.c -o /tmp/hrgbitem && /tmp/hrgbitem
 *
 * What it proves (ROMs are the user's own dumps, never in the repo; a missing ROM SKIPs):
 *   1) Gold and Crystal open (the two pins) and Red (Gen 1: no table) is REFUSED;
 *   2) all 190 descriptions of each decode, fit ROM_GBITEM_DESC_MAX, contain no "{XX}" escape
 *      and hash EXACTLY to goldens made by an independent Python decoder (own charmap; the line-end
 *      '-' join keeps the hyphen before type/colored/level -- review-zr D1);
 *   3) spot texts: id 1 "[redacted]" (the first entry), the hyphenation
 *      join ("[redacted]") and POKe/&/'s glyphs;
 *   4) refusals: id 0, id 191 (TM/HM block), id 255, a cap too small (never truncates), a
 *      NULL out, an unopened RomGbItem;
 *   5) MUTATIONS through a patching read hook: a version/layout lie (the first table entry
 *      pointing out of the bank, a broken terminator before entry 1, a broken terminator
 *      before the LAST described entry) makes open() refuse; a broken terminator before ONE
 *      entry makes only that id refuse.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_gbitem.h"

static int fails = 0, checks = 0;
static void chk(const char* who, const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", who, what); fails++; }
}

typedef struct { const uint8_t* d; uint32_t n; long poff; uint8_t pval; } Img;
static unsigned g_reads;
static bool rd(void* ctx, uint32_t off, void* dst, uint32_t len) {
  Img* im = (Img*)ctx;
  g_reads++;
  if ((uint64_t)off + len > im->n) return false;
  memcpy(dst, im->d + off, len);
  if (im->poff >= 0 && (uint32_t)im->poff >= off && (uint32_t)im->poff < off + len)
    ((uint8_t*)dst)[(uint32_t)im->poff - off] = im->pval;
  return true;
}

static uint8_t* slurp(const char* path, uint32_t* n) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* b = (uint8_t*)malloc((size_t)sz);
  if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return 0; }
  fclose(f); *n = (uint32_t)sz;
  return b;
}

static uint32_t fnv_add(uint32_t h, const char* s) {
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; }
  return h;
}

typedef struct { const char* file; const char* name; uint32_t bank_off; uint32_t taddr; uint32_t golden; } Game;
/* goldens: independent Python decode of the 190 strings, '\n'-joined, FNV-1a 32 (UTF-8) */
static const Game k_games[] = {
  { "Gold.gbc",    "Gold",    0x6Eu * 0x4000u, 0x4000u, 0x3ED940B6u },
  { "Crystal.gbc", "Crystal", 0x72u * 0x4000u, 0x4987u, 0xD4871EE1u },
};

static void run_game(const char* dir, const Game* g) {
  char p[600]; sprintf(p, "%s/gb/%s", dir, g->file);
  uint32_t n = 0; uint8_t* rom = slurp(p, &n);
  if (!rom) { printf("SKIP %s (no %s)\n", g->name, p); return; }
  Img im = { rom, n, -1, 0 };
  RomGbItem gi;
  chk(g->name, "opens", rom_gbitem_open(&gi, rd, &im, n) && gi.ok);
  if (!gi.ok) { free(rom); return; }
  chk(g->name, "pin found at the expected bank/address", gi.bank_off == g->bank_off && gi.taddr == g->taddr);

  uint32_t h = 0x811C9DC5u;
  int bad = 0, esc = 0, maxlen = 0;
  char d[ROM_GBITEM_DESC_MAX];
  for (unsigned id = 1; id <= ROM_GBITEM_MAX_ID; id++) {
    int len = rom_gbitem_desc(&gi, (uint8_t)id, d, (int)sizeof d);
    if (len <= 0 || (int)strlen(d) != len) { bad++; continue; }
    if (strchr(d, '{')) esc++;
    if (len > maxlen) maxlen = len;
    h = fnv_add(h, d); h = fnv_add(h, "\n");
  }
  chk(g->name, "all 190 descriptions decode", bad == 0);
  chk(g->name, "no {XX} escape survives", esc == 0);
  chk(g->name, "hash == independent Python golden", h == g->golden);
  printf("%s: opened bank_off=0x%X taddr=0x%X, 190 descriptions, longest %d B, hash %08X\n",
         g->name, (unsigned)gi.bank_off, (unsigned)gi.taddr, maxlen, (unsigned)h);

  chk(g->name, "id 1 text", rom_gbitem_desc(&gi, 1, d, (int)sizeof d) > 0 &&
                           strcmp(d, "[redacted]") == 0);
  chk(g->name, "hyphen join + no leading/trailing space (id 4)", rom_gbitem_desc(&gi, 4, d, (int)sizeof d) > 0 &&
                           strcmp(d, "[redacted]") == 0);
  chk(g->name, "id 3 apostrophe-s", rom_gbitem_desc(&gi, 3, d, (int)sizeof d) > 0 &&
                           strcmp(d, "[redacted]") == 0);
  /* review-zr D1: a line-end '-' before type/colored/level is a REAL compound hyphen (kept) */
  chk(g->name, "id 60 compound 'silver-colored' keeps its hyphen", rom_gbitem_desc(&gi, 60, d, (int)sizeof d) > 0 &&
                           strcmp(d, "[redacted]") == 0);
  chk(g->name, "id 76 compound 'ground-type' keeps its hyphen", rom_gbitem_desc(&gi, 76, d, (int)sizeof d) > 0 &&
                           strcmp(d, "[redacted]") == 0);
  chk(g->name, "id 159 compound 'lower-level' keeps its hyphen", rom_gbitem_desc(&gi, 159, d, (int)sizeof d) > 0 &&
                           strcmp(d, "[redacted]") == 0);
  chk(g->name, "id 5 POKe glyph", rom_gbitem_desc(&gi, 5, d, (int)sizeof d) > 0 &&
                           strcmp(d, "[redacted]") == 0);

  /* refusals */
  chk(g->name, "id 0 refused", rom_gbitem_desc(&gi, 0, d, (int)sizeof d) == 0 && d[0] == 0);
  chk(g->name, "id 191 (TM01) refused", rom_gbitem_desc(&gi, 191, d, (int)sizeof d) == 0);
  chk(g->name, "id 255 refused", rom_gbitem_desc(&gi, 255, d, (int)sizeof d) == 0);
  chk(g->name, "cap too small refused, not truncated", rom_gbitem_desc(&gi, 1, d, 10) == 0 && d[0] == 0);
  chk(g->name, "cap 1 refused", rom_gbitem_desc(&gi, 1, d, 1) == 0);
  chk(g->name, "NULL out refused", rom_gbitem_desc(&gi, 1, 0, 8) == 0);
  { RomGbItem z; memset(&z, 0, sizeof z);
    chk(g->name, "unopened RomGbItem refused", rom_gbitem_desc(&z, 1, d, (int)sizeof d) == 0); }

  /* mutations through the patch hook */
  uint32_t tbl = g->bank_off + g->taddr - 0x4000u;
  RomGbItem m;
  im.poff = (long)(tbl + 1); im.pval = 0xFF;              /* entry 0 high byte -> 0xFFxx: out of the bank */
  chk(g->name, "MUT first entry out of the bank: open refuses", rom_gbitem_open(&m, rd, &im, n) == 0 && m.ok == 0);
  /* the byte before entry 1's text is entry 0's terminator */
  uint16_t e1 = (uint16_t)(rom[tbl + 2] | (rom[tbl + 3] << 8));
  im.poff = (long)(g->bank_off + e1 - 0x4000u - 1u); im.pval = 0x7F;
  chk(g->name, "MUT broken terminator before entry 1: open refuses", rom_gbitem_open(&m, rd, &im, n) == 0);
  uint16_t eL = (uint16_t)(rom[tbl + 2u * (ROM_GBITEM_MAX_ID)] | (rom[tbl + 2u * ROM_GBITEM_MAX_ID + 1] << 8));
  im.poff = (long)(g->bank_off + eL - 0x4000u - 1u); im.pval = 0x7F;
  chk(g->name, "MUT broken terminator before the last entry: open refuses", rom_gbitem_open(&m, rd, &im, n) == 0);
  /* one MID-table terminator (id 60's end): open still passes, only that id refuses */
  uint16_t e60 = (uint16_t)(rom[tbl + 2u * 60] | (rom[tbl + 2u * 60 + 1] << 8));
  im.poff = (long)(g->bank_off + e60 - 0x4000u - 1u); im.pval = 0x7F;
  chk(g->name, "MUT mid-table terminator: open still passes", rom_gbitem_open(&m, rd, &im, n) == 1);
  chk(g->name, "MUT mid-table terminator: that id refuses",
      rom_gbitem_desc(&m, 60, d, (int)sizeof d) == 0 &&
      rom_gbitem_desc(&m, 59, d, (int)sizeof d) > 0);
  /* a control byte inside a text refuses the whole description */
  uint16_t e9 = (uint16_t)(rom[tbl + 2u * 9] | (rom[tbl + 2u * 9 + 1] << 8));
  im.poff = (long)(g->bank_off + e9 - 0x4000u); im.pval = 0x57;     /* "done" control inside id 10 */
  chk(g->name, "MUT control byte inside a text: that id refuses",
      rom_gbitem_desc(&gi, 10, d, (int)sizeof d) == 0);
  im.poff = -1;
  /* #403b: the cached-pin re-open reads far less than the full probe and serves the same text */
  {
    RomGbItem pn; char a[ROM_GBITEM_DESC_MAX], b[ROM_GBITEM_DESC_MAX];
    unsigned r0 = g_reads; RomGbItem full; int fo = rom_gbitem_open(&full, rd, &im, n); unsigned r_full = g_reads - r0;
    r0 = g_reads; int po = rom_gbitem_open_pin(&pn, rd, &im, n, full.pin); unsigned r_pin = g_reads - r0;
    chk(g->name, "pinned re-open succeeds on the remembered pin", fo && po && pn.ok && pn.pin == full.pin);
    chk(g->name, "pinned re-open reads at most 6 times and fewer than half the full probe", r_pin <= 6 && r_pin * 2 < r_full);
    chk(g->name, "pinned re-open serves the same text",
        rom_gbitem_desc(&full, 7, a, (int)sizeof a) > 0 && rom_gbitem_desc(&pn, 7, b, (int)sizeof b) > 0 && strcmp(a, b) == 0);
    RomGbItem w;
    chk(g->name, "pinned re-open on a pin index out of range refuses", rom_gbitem_open_pin(&w, rd, &im, n, 200) == 0 && w.ok == 0);
    chk(g->name, "pinned re-open on the OTHER game's pin refuses", rom_gbitem_open_pin(&w, rd, &im, n, (uint8_t)(full.pin ^ 1u)) == 0);
    im.poff = (long)(tbl + 1); im.pval = 0xFF;
    chk(g->name, "MUT pinned re-open: first entry out of the bank refuses", rom_gbitem_open_pin(&w, rd, &im, n, full.pin) == 0);
    im.poff = (long)(g->bank_off + eL - 0x4000u - 1u); im.pval = 0x7F;
    chk(g->name, "MUT pinned re-open: broken last terminator refuses", rom_gbitem_open_pin(&w, rd, &im, n, full.pin) == 0);
    im.poff = -1;
  }
  /* image too small to hold the pinned bank */
  chk(g->name, "image shorter than the bank: refused", rom_gbitem_open(&m, rd, &im, g->bank_off + 0x2000u) == 0);
  free(rom);
}

int main(void) {
  const char* dir = getenv("ROMS");
  if (!dir) dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  for (unsigned i = 0; i < sizeof k_games / sizeof k_games[0]; i++) run_game(dir, &k_games[i]);
  /* Red (Gen 1): no description table -- must refuse */
  {
    char p[600]; sprintf(p, "%s/gb/Red.gb", dir);
    uint32_t n = 0; uint8_t* rom = slurp(p, &n);
    if (!rom) printf("SKIP Red (no %s)\n", p);
    else { Img im = { rom, n, -1, 0 }; RomGbItem gi;
      chk("Red", "Gen 1 ROM refused (no item descriptions)", rom_gbitem_open(&gi, rd, &im, n) == 0 && gi.ok == 0);
      free(rom); }
  }
  printf("%d checks, %d fails\n", checks, fails);
  return fails ? 1 : 0;
}
