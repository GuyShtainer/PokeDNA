/* Host (PC) test for rom_text -- item / move / ability descriptions read from REAL
 * retail ROMs. rom_text does all its I/O through the RomCtx callback, so the code
 * exercised here is byte-for-byte the code that runs on the GBA.
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_romtext_test.c source/rom_text.c \
 *      source/rom_map.c source/data_tables.c source/data_desc.c source/gen3_flags.c \
 *      source/ui_font.c -o /tmp/hrtx && /tmp/hrtx
 *
 * What it proves, against Guy's own five cartridge dumps:
 *   1) the tables open on Emerald + FireRed + LeafGreen (GF header) and on the two
 *      Ruby/Sapphire revisions that have pinned rows, and rom_text FAILS CLOSED on a
 *      Ruby/Sapphire revision with no pin;
 *   2) POTION (item 13), ability 13 and move 1 decode EXACTLY the strings the decomps
 *      say they should -- per game, because the wording differs per game;
 *   3) every single table entry decodes: all 349/375/377 items, all 78 abilities and
 *      all 354 moves, with no read failure and no charset escape;
 *   4) the ROM-decoded Emerald strings match the 741 VERBATIM strings PokeDNA embeds
 *      today (data_tables.c s_itemdesc/s_mvdesc/s_abilitydesc) -- which is the whole
 *      point of the module: if they match, the embedded copies can be deleted;
 *   5) bounds are enforced -- a truncated ROM, a garbage ROM and a ROM whose tables
 *      point off the end all fail closed instead of reading past the image;
 *   6) every decoded string is drawable by PokeDNA's own 5x7 face, and no single WORD
 *      is wider than the narrowest description pane we render into (108 px,
 *      pdna_pick.c:1010). This is host_textfit_test.c's rule applied to strings that
 *      only exist at runtime, so it has to live here rather than there.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_map.h"
#include "rom_text.h"
#include "data_tables.h"
#include "ui_font.h"

static int fails = 0, checks = 0;
static void chk(const char* rom, const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", rom, what); fails++; }
}

/* ---- a RomReadFn over a byte buffer, so a test can mutate the "ROM" ------------- */
typedef struct { const uint8_t* p; uint32_t n; } MemCtx;

/* Every read is counted, because on hardware a read is not free: rc->read is an
 * f_lseek + f_read on the user's .gba on the microSD, and this handle carries no
 * cluster link map (FF_USE_FASTSEEK is 1 since 2026-08-23, but it is opt-in per handle
 * -- see source/fastseek.h -- and only the icon/art ROM handle and icons.bin opt in),
 * so a backward seek walks the FAT chain. The
 * cache_tests() section below asserts on this counter -- see rom_text.c "THE READ
 * CACHE". Every other section ignores it. */
static long g_reads = 0;

static bool mem_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  MemCtx* m = (MemCtx*)ctx;
  g_reads++;
  if (off > m->n || len > m->n - off) return false;
  memcpy(dst, m->p + off, len);
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

/* ---- ui_ptext geometry (mirrors source/ui.c pnext/ui_ptext_w) ------------------- */
static int pwidth_n(const char* s, int n) {
  int w = 0;
  const unsigned char* p = (const unsigned char*)s;
  const unsigned char* e = p + n;
  while (p < e) {
    unsigned c;
    if (*p == 0xC3u && p + 1 < e && p[1] == 0xA9u) { c = 127; p += 2; }
    else if (*p < 0x80u) { c = *p++; }
    else { p++; while (p < e && (*p & 0xC0u) == 0x80u) p++; c = '?'; }
    if (c < 32u || c > 127u) c = '?';
    w += ui_font_w[c - 32];
  }
  return w;
}
#define DESC_PANE_W 108   /* narrowest description pane: pdna_pick.c:1010 */

/* Every byte must be plain ASCII 32..126 or the one UTF-8 pair the font has a glyph
 * for; anything else would draw as '?' on hardware. */
static int drawable(const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  while (*p) {
    if (*p == 0xC3u && p[1] == 0xA9u) { p += 2; continue; }
    if (*p < 32u || *p > 126u) return 0;
    p++;
  }
  return 1;
}

/* Widest single word, in ui_ptext pixels. A word wider than the pane can never be
 * wrapped and would clip silently. */
static int widest_word(const char* s) {
  int best = 0;
  const char* w = s;
  for (const char* p = s;; p++) {
    if (*p == ' ' || *p == 0) {
      int px = pwidth_n(w, (int)(p - w));
      if (px > best) best = px;
      if (!*p) break;
      w = p + 1;
    }
  }
  return best;
}

/* ---- the embedded tables, normalised for comparison -----------------------------
 * tools/gen_data.py copies the decomp's C source text verbatim, so the embedded rows
 * keep a few things the raw ROM bytes spell differently. Normalising them is the
 * honest way to compare, and each rewrite below is a place where reading the ROM is
 * strictly BETTER than the embedded copy:
 *   "{POKEBLOCK}"  the macro was never expanded, so PokeDNA literally prints the
 *                  braces today; the ROM's 0x55..0x59 ligature is "POKeBLOCK".
 *   curly quotes   the decomp source uses U+201C/U+201D/U+2019, which ui_ptext has no
 *                  glyph for and draws as '?'; the ROM's 0xB1/0xB2/0xB4 are quotes.
 */
static void normalise(const char* src, char* dst, size_t cap) {
  static const struct { const char* from; const char* to; } k_fix[] = {
    { "{POKEBLOCK}", "POK\xC3\xA9" "BLOCK" },
    { "\xE2\x80\x9C", "\"" }, { "\xE2\x80\x9D", "\"" },   /* U+201C U+201D */
    { "\xE2\x80\x98", "'"  }, { "\xE2\x80\x99", "'"  },   /* U+2018 U+2019 */
    { "\xE2\x80\xA6", "..." },                            /* U+2026        */
  };
  size_t w = 0;
  for (const char* p = src; *p && w + 1 < cap;) {
    size_t k = 0;
    for (; k < sizeof k_fix / sizeof k_fix[0]; k++) {
      size_t l = strlen(k_fix[k].from);
      if (strncmp(p, k_fix[k].from, l) == 0) {
        for (const char* q = k_fix[k].to; *q && w + 1 < cap; q++) dst[w++] = *q;
        p += l;
        break;
      }
    }
    if (k == sizeof k_fix / sizeof k_fix[0]) dst[w++] = *p++;
  }
  dst[w] = 0;
}

/* ---- per-ROM run ---------------------------------------------------------------- */
typedef struct {
  const char* name;
  const char* file;
  int         expect_open;     /* 1 = every kind must open                       */
  int         expect_items;    /* item count this ROM must serve                 */
  int         group;
  TextPin     potion;          /* item 13, pinned by length + FNV                 */
  TextPin     pound;           /* move 1                                          */
} RomCase;

/* Expected texts are pinned by (UTF-8 byte length, FNV-1a 32 of the decoded string) --
 * never verbatim, so the test carries no game text (release audit 2026-10-08). The pins
 * are of item 13 / move 1 / ability 13 as the on-cartridge bytes decode (the source's "\n"
 * collapsed to a space); they differ per game, which is why rom_text exposes a group.
 * Regenerate a pin with tools/text_pin.py "<decoded string>". */
typedef struct { int len; uint32_t fnv; } TextPin;
static uint32_t fnv1a(const char* s) {
  uint32_t h = 0x811C9DC5u;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 0x01000193u; }
  return h;
}
static int pin_ok(const char* s, TextPin p) { return (int)strlen(s) == p.len && fnv1a(s) == p.fnv; }
static const TextPin POTION_RSE = { 43, 0x3CDA01EBu };
static const TextPin POTION_FRLG = { 77, 0x0F1BDE2Bu };
static const TextPin POUND_RSE = { 37, 0x9A403A6Du };
static const TextPin POUND_FRLG = { 63, 0x8E9E211Eu };
static const TextPin ABIL13 = { 24, 0x56022BD4u };

static void run_rom(const char* dir, const RomCase* rcase) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, rcase->file);
  uint32_t n = 0;
  uint8_t* rom = slurp(path, &n);
  if (!rom) { printf("SKIP %s (no %s)\n", rcase->name, path); return; }
  const char* name = rcase->name;

  MemCtx mc = { rom, n };
  RomCtx rc;
  if (!rom_open(&rc, mem_read, &mc, n)) {
    chk(name, "rom_open accepts the retail dump", 0);
    free(rom); return;
  }

  RomText rt;
  int ok = rom_text_open(&rt, &rc);
  chk(name, rcase->expect_open ? "rom_text_open succeeds" : "rom_text_open FAILS CLOSED",
      ok == rcase->expect_open);
  if (!ok) {
    /* fail-closed must also mean: nothing readable, no half-open kinds */
    char b[ROM_TEXT_MAX];
    chk(name, "no kind is served after a failed open",
        !rom_text_have(&rt, ROM_TEXT_ITEM) && !rom_text_have(&rt, ROM_TEXT_MOVE) &&
        !rom_text_have(&rt, ROM_TEXT_ABILITY) &&
        !rom_text_get(&rt, ROM_TEXT_ITEM, 13, b, sizeof b));
    printf("  %s: closed (kind=%s rev=%u) -- no pinned row\n",
           name, rom_kind_name(rc.kind), rc.version);
    free(rom); return;
  }

  chk(name, "all three kinds available",
      rom_text_have(&rt, ROM_TEXT_ITEM) && rom_text_have(&rt, ROM_TEXT_MOVE) &&
      rom_text_have(&rt, ROM_TEXT_ABILITY));
  chk(name, "item count matches this game's ITEMS_COUNT",
      rom_text_count(&rt, ROM_TEXT_ITEM) == rcase->expect_items);
  chk(name, "ability count is 78",  rom_text_count(&rt, ROM_TEXT_ABILITY) == 78);
  chk(name, "move ids run 0..354",  rom_text_count(&rt, ROM_TEXT_MOVE) == 355);
  chk(name, "game group is right",  rom_text_group(&rt) == rcase->group);

  /* 2) known ids decode EXACTLY */
  char b[ROM_TEXT_MAX];
  chk(name, "POTION (item 13) decodes exactly",
      rom_text_get(&rt, ROM_TEXT_ITEM, 13, b, sizeof b) && pin_ok(b, rcase->potion));
  if (!pin_ok(b, rcase->potion)) printf("      got %s\n", b);
  chk(name, "move 1 (Pound) decodes exactly",
      rom_text_get(&rt, ROM_TEXT_MOVE, 1, b, sizeof b) && pin_ok(b, rcase->pound));
  if (!pin_ok(b, rcase->pound)) printf("      got %s\n", b);
  chk(name, "ability 13 decodes exactly",
      rom_text_get(&rt, ROM_TEXT_ABILITY, 13, b, sizeof b) && pin_ok(b, ABIL13));
  if (!pin_ok(b, ABIL13)) printf("      got %s\n", b);

  /* 3) EVERY entry decodes, and every one of them is drawable + wrappable */
  int bad = 0, empty = 0, undrawable = 0, toowide = 0, longest = 0, nonempty = 0;
  static const struct { RomTextKind k; uint16_t lo; } k_kinds[] = {
    { ROM_TEXT_ITEM, 0 }, { ROM_TEXT_ABILITY, 0 }, { ROM_TEXT_MOVE, 1 },
  };
  for (unsigned ki = 0; ki < 3; ki++) {
    int cnt = rom_text_count(&rt, k_kinds[ki].k);
    for (int id = k_kinds[ki].lo; id < cnt; id++) {
      if (!rom_text_get(&rt, k_kinds[ki].k, (uint16_t)id, b, sizeof b)) { bad++; continue; }
      int len = (int)strlen(b);
      if (!len) { empty++; continue; }
      nonempty++;
      if (len > longest) longest = len;
      if (!drawable(b)) { if (undrawable++ < 3) printf("      [%s] undrawable k%u id%d: %s\n", name, ki, id, b); }
      if (widest_word(b) > DESC_PANE_W) {
        if (toowide++ < 3) printf("      [%s] word too wide (%d px) k%u id%d: %s\n",
                                  name, widest_word(b), ki, id, b);
      }
    }
  }
  chk(name, "every table entry decodes", bad == 0);
  chk(name, "every decoded string is drawable by ui_ptext", undrawable == 0);
  chk(name, "no word exceeds the 108 px description pane", toowide == 0);
  chk(name, "longest string fits ROM_TEXT_MAX", longest > 0 && longest < ROM_TEXT_MAX);

  /* 5a) a SHORT buffer must fail rather than truncate */
  char small[16];
  chk(name, "a too-small buffer fails instead of truncating",
      !rom_text_get(&rt, ROM_TEXT_ITEM, 13, small, sizeof small) && small[0] == 0);
  chk(name, "cap 0 / NULL dst are refused",
      !rom_text_get(&rt, ROM_TEXT_ITEM, 13, small, 0) &&
      !rom_text_get(&rt, ROM_TEXT_ITEM, 13, 0, sizeof small));

  /* 5b) out-of-range ids */
  chk(name, "item id == count is refused",
      !rom_text_get(&rt, ROM_TEXT_ITEM, (uint16_t)rcase->expect_items, b, sizeof b));
  chk(name, "ability id 78 is refused", !rom_text_get(&rt, ROM_TEXT_ABILITY, 78, b, sizeof b));
  chk(name, "move id 0 (MOVE_NONE) is refused", !rom_text_get(&rt, ROM_TEXT_MOVE, 0, b, sizeof b));
  chk(name, "move id 355 is refused", !rom_text_get(&rt, ROM_TEXT_MOVE, 355, b, sizeof b));
  chk(name, "an invalid kind is refused", !rom_text_get(&rt, (RomTextKind)7, 1, b, sizeof b));

  printf("  %s: ok (kind=%s rev=%u group=%d, %d strings, %d empty, longest %d B)\n",
         name, rom_kind_name(rc.kind), rc.version, rom_text_group(&rt), nonempty, empty, longest);
  free(rom);
}

/* ---- 4) do the ROM strings really replace the 741 embedded ones? ----------------- */
static void compare_with_embedded(const char* dir) {
  char path[512];
  snprintf(path, sizeof path, "%s/Emerald.gba", dir);
  uint32_t n = 0;
  uint8_t* rom = slurp(path, &n);
  if (!rom) { printf("SKIP embedded-string comparison (no %s)\n", path); return; }

  MemCtx mc = { rom, n };
  RomCtx rc; RomText rt;
  if (!rom_open(&rc, mem_read, &mc, n) || !rom_text_open(&rt, &rc)) {
    chk("Emerald", "opens for the embedded-string comparison", 0); free(rom); return;
  }

  char b[ROM_TEXT_MAX], want[ROM_TEXT_MAX * 2];
  int match = 0, diff = 0, missing = 0, shown = 0, replaceable = 0;

  /* PokeDNA's embedded tables are Emerald's text, so Emerald is the ROM they must
   * match. 741 = 309 item + 354 move + 78 ability non-empty rows (docs/BACKLOG.md #19). */
  for (int id = 0; id < 377; id++) {
    const char* s = pk_item_desc((uint16_t)id);
    if (!s || !*s) continue;
    replaceable++;
    normalise(s, want, sizeof want);
    if (!rom_text_get(&rt, ROM_TEXT_ITEM, (uint16_t)id, b, sizeof b)) { missing++; continue; }
    if (strcmp(b, want) == 0) match++;
    else { diff++; if (shown++ < 5) printf("      item %d\n        embedded %s\n        rom      %s\n", id, want, b); }
  }
  for (int id = 1; id < 355; id++) {
    const char* s = pk_move_desc((uint16_t)id);
    if (!s || !*s) continue;
    replaceable++;
    normalise(s, want, sizeof want);
    if (!rom_text_get(&rt, ROM_TEXT_MOVE, (uint16_t)id, b, sizeof b)) { missing++; continue; }
    if (strcmp(b, want) == 0) match++;
    else { diff++; if (shown++ < 5) printf("      move %d\n        embedded %s\n        rom      %s\n", id, want, b); }
  }
  for (int id = 0; id < 78; id++) {
    const char* s = pk_ability_desc((uint16_t)id);
    if (!s || !*s) continue;
    replaceable++;
    normalise(s, want, sizeof want);
    if (!rom_text_get(&rt, ROM_TEXT_ABILITY, (uint16_t)id, b, sizeof b)) { missing++; continue; }
    if (strcmp(b, want) == 0) match++;
    else { diff++; if (shown++ < 5) printf("      ability %d\n        embedded %s\n        rom      %s\n", id, want, b); }
  }

  printf("  embedded-string coverage: %d of %d shipped strings reproduced from the ROM"
         " (%d differ, %d unreadable)\n", match, replaceable, diff, missing);
  chk("Emerald", "every shipped description is reproduced byte-for-byte from the ROM",
      diff == 0 && missing == 0 && match == replaceable);
  free(rom);
}

/* ---- 1b) a revision with no pinned row --------------------------------------------
 * Guy owns Ruby rev 2 and Sapphire rev 1, which are exactly the two R/S revisions
 * rom_text pins, so the "unlisted revision" path cannot be reached with a real dump.
 * Rebuild the RomCtx by hand instead (rom_text only ever looks at kind/code/version/
 * size/read) and check the two halves of the degradation rule:
 *   - Ruby with an unpinned revision byte loses EVERYTHING (no header to fall back on)
 *   - FireRed with an unpinned revision byte keeps items + abilities from the GF
 *     header and loses only the pinned move descriptions.
 * That second case is not hypothetical: BPRE rev 0 and BPGE rev 1 ship with no pin. */
static void unpinned_revision_tests(const char* dir) {
  char path[512];
  uint32_t n = 0;
  RomCtx rc; RomText rt; char b[ROM_TEXT_MAX];

  snprintf(path, sizeof path, "%s/Ruby.gba", dir);
  uint8_t* rom = slurp(path, &n);
  if (rom) {
    MemCtx mc = { rom, n };
    if (rom_open(&rc, mem_read, &mc, n)) {
      chk("Ruby", "the real rev 2 opens", rom_text_open(&rt, &rc));
      rc.version = 0;                                  /* AXVE rev 0: no pinned row */
      chk("Ruby rev0", "an unpinned R/S revision fails closed", !rom_text_open(&rt, &rc));
      chk("Ruby rev0", "and serves nothing",
          !rom_text_have(&rt, ROM_TEXT_ITEM) && !rom_text_have(&rt, ROM_TEXT_MOVE) &&
          !rom_text_have(&rt, ROM_TEXT_ABILITY) &&
          !rom_text_get(&rt, ROM_TEXT_MOVE, 1, b, sizeof b));
    }
    free(rom);
  } else printf("SKIP unpinned-revision test (no %s)\n", path);

  snprintf(path, sizeof path, "%s/FireRed.gba", dir);
  rom = slurp(path, &n);
  if (rom) {
    MemCtx mc = { rom, n };
    if (rom_open(&rc, mem_read, &mc, n)) {
      rc.version = 0;                                  /* BPRE rev 0: no pinned row */
      chk("FireRed rev0", "an unpinned E/FRLG revision still opens", rom_text_open(&rt, &rc));
      chk("FireRed rev0", "items survive (GF header)",     rom_text_have(&rt, ROM_TEXT_ITEM));
      chk("FireRed rev0", "abilities survive (GF header)", rom_text_have(&rt, ROM_TEXT_ABILITY));
      chk("FireRed rev0", "moves are OFF (header field is NULL, no pin)",
          !rom_text_have(&rt, ROM_TEXT_MOVE));
      chk("FireRed rev0", "and a move read fails", !rom_text_get(&rt, ROM_TEXT_MOVE, 1, b, sizeof b));
      chk("FireRed rev0", "while POTION still decodes",
          rom_text_get(&rt, ROM_TEXT_ITEM, 13, b, sizeof b) && pin_ok(b, POTION_FRLG));
    }
    free(rom);
  }
}

/* ---- 5) bounds: a truncated / garbage / poisoned image must fail closed ---------- */
static void bounds_tests(const char* dir) {
  char path[512];
  snprintf(path, sizeof path, "%s/Emerald.gba", dir);
  uint32_t n = 0;
  uint8_t* rom = slurp(path, &n);
  if (!rom) { printf("SKIP bounds tests (no %s)\n", path); return; }

  RomCtx rc; RomText rt; char b[ROM_TEXT_MAX];

  /* (a) all zeroes -- rom_open itself must refuse it, and rom_text must refuse the
   *     half-built context that leaves behind. */
  uint8_t* zero = (uint8_t*)calloc(1, n);
  MemCtx zc = { zero, n };
  chk("bounds", "rom_open refuses an all-zero image", !rom_open(&rc, mem_read, &zc, n));
  chk("bounds", "rom_text_open refuses it too", !rom_text_open(&rt, &rc));
  chk("bounds", "and serves nothing", !rom_text_get(&rt, ROM_TEXT_ITEM, 13, b, sizeof b));
  free(zero);

  /* (b) truncated dump: rom_open's 16 MiB rule catches it before we ever get here. */
  MemCtx tc = { rom, n / 2 };
  chk("bounds", "rom_open refuses a half-length dump", !rom_open(&rc, mem_read, &tc, n / 2));

  /* (c) the interesting one: a VALID Emerald whose GF header item/ability pointers
   *     have been rewritten to point one byte past the end of the image, and whose
   *     move-description pin is left intact. Items + abilities must switch off, moves
   *     must keep working, and the open must still succeed. */
  uint8_t* poisoned = (uint8_t*)malloc(n);
  memcpy(poisoned, rom, n);
  uint32_t past = 0x08000000u + n;            /* first address outside the image */
  for (int k = 0; k < 4; k++) {
    poisoned[0x100 + 0xC4 + k] = (uint8_t)(past >> (8 * k));
    poisoned[0x100 + 0xC8 + k] = (uint8_t)(past >> (8 * k));
  }
  MemCtx pc = { poisoned, n };
  chk("bounds", "the poisoned image still opens as Emerald", rom_open(&rc, mem_read, &pc, n));
  chk("bounds", "rom_text_open still succeeds (moves survive)", rom_text_open(&rt, &rc));
  chk("bounds", "out-of-image item table is switched off",    !rom_text_have(&rt, ROM_TEXT_ITEM));
  chk("bounds", "out-of-image ability table is switched off", !rom_text_have(&rt, ROM_TEXT_ABILITY));
  chk("bounds", "the pinned move table still serves",          rom_text_have(&rt, ROM_TEXT_MOVE));
  chk("bounds", "and a disabled kind reads nothing",
      !rom_text_get(&rt, ROM_TEXT_ITEM, 13, b, sizeof b) && b[0] == 0);
  chk("bounds", "moves still decode", rom_text_get(&rt, ROM_TEXT_MOVE, 1, b, sizeof b) &&
      pin_ok(b, POUND_RSE));

  /* (d) garbage text: repoint the ability table at a run of executable code. Every
   *     pointer there is nonsense, so the probe must reject the whole kind. */
  memcpy(poisoned, rom, n);
  uint32_t code_addr = 0x08000200u;           /* ARM code, definitely not text */
  for (int k = 0; k < 4; k++)
    poisoned[0x100 + 0xC4 + k] = (uint8_t)(code_addr >> (8 * k));
  chk("bounds", "still opens with a garbage ability pointer", rom_open(&rc, mem_read, &pc, n));
  rom_text_open(&rt, &rc);
  chk("bounds", "garbage ability table is rejected by the probe",
      !rom_text_have(&rt, ROM_TEXT_ABILITY));
  chk("bounds", "items are unaffected", rom_text_have(&rt, ROM_TEXT_ITEM) &&
      rom_text_get(&rt, ROM_TEXT_ITEM, 13, b, sizeof b) && pin_ok(b, POTION_RSE));

  /* (e) an unterminated string: blot out every 0xFF for a long stretch after the
   *     Master Ball description so the decoder runs off the end of its window. */
  memcpy(poisoned, rom, n);
  {
    RomCtx rc2; RomText rt2;
    rom_open(&rc2, mem_read, &pc, n);
    rom_text_open(&rt2, &rc2);
    /* find where item 1's description lives, then erase its terminators */
    uint32_t items = (uint32_t)poisoned[0x100 + 0xC8] | ((uint32_t)poisoned[0x100 + 0xC9] << 8) |
                     ((uint32_t)poisoned[0x100 + 0xCA] << 16) | ((uint32_t)poisoned[0x100 + 0xCB] << 24);
    uint32_t e = items - 0x08000000u + 1 * 44 + 0x14;
    uint32_t d = (uint32_t)poisoned[e] | ((uint32_t)poisoned[e + 1] << 8) |
                 ((uint32_t)poisoned[e + 2] << 16) | ((uint32_t)poisoned[e + 3] << 24);
    uint32_t off = d - 0x08000000u;
    for (uint32_t i = 0; i < 400 && off + i < n; i++)
      if (poisoned[off + i] == 0xFF) poisoned[off + i] = 0xD5;   /* 'a' -- valid text, no EOS */
    rom_open(&rc2, mem_read, &pc, n);
    chk("bounds", "an unterminated description kills only that kind",
        !rom_text_open(&rt2, &rc2) || !rom_text_have(&rt2, ROM_TEXT_ITEM));
  }

  /* (f) ONE byte outside the charset inside the probe string. This is the fail-closed
   *     rule the whole module rests on: a plausible-looking but wrong table address
   *     produces exactly this, and printing '?' for the byte instead of refusing the
   *     string is how a viewer starts confidently showing garbage. 0x30 is a
   *     Japanese-only codepoint with no international glyph. */
  memcpy(poisoned, rom, n);
  {
    uint32_t items = (uint32_t)poisoned[0x100 + 0xC8] | ((uint32_t)poisoned[0x100 + 0xC9] << 8) |
                     ((uint32_t)poisoned[0x100 + 0xCA] << 16) | ((uint32_t)poisoned[0x100 + 0xCB] << 24);
    uint32_t e = items - 0x08000000u + 1 * 44 + 0x14;
    uint32_t d = (uint32_t)poisoned[e] | ((uint32_t)poisoned[e + 1] << 8) |
                 ((uint32_t)poisoned[e + 2] << 16) | ((uint32_t)poisoned[e + 3] << 24);
    poisoned[d - 0x08000000u + 3] = 0x30;
    chk("bounds", "a poisoned-charset image still opens", rom_open(&rc, mem_read, &pc, n));
    rom_text_open(&rt, &rc);
    chk("bounds", "a byte outside the charset disables the kind",
        !rom_text_have(&rt, ROM_TEXT_ITEM));
    chk("bounds", "abilities are unaffected by it", rom_text_have(&rt, ROM_TEXT_ABILITY));
  }

  /* (g) a table whose PROBE entry is perfectly valid but whose EXTENT runs off the end
   *     of the image. Every individual read is already bounds-checked, so without the
   *     whole-table check this opens happily and then fails on entry 2 of 78 -- which
   *     is a kind that "works" until the user scrolls. Build the trap by hand: two
   *     good pointers in the last 8 bytes of the ROM, declared as a 78-entry table. */
  memcpy(poisoned, rom, n);
  {
    uint32_t items = (uint32_t)poisoned[0x100 + 0xC8] | ((uint32_t)poisoned[0x100 + 0xC9] << 8) |
                     ((uint32_t)poisoned[0x100 + 0xCA] << 16) | ((uint32_t)poisoned[0x100 + 0xCB] << 24);
    uint32_t e = items - 0x08000000u + 1 * 44 + 0x14;   /* Master Ball's description */
    uint32_t good = (uint32_t)poisoned[e] | ((uint32_t)poisoned[e + 1] << 8) |
                    ((uint32_t)poisoned[e + 2] << 16) | ((uint32_t)poisoned[e + 3] << 24);
    for (int k = 0; k < 4; k++) {
      poisoned[n - 8 + k] = (uint8_t)(good >> (8 * k));   /* fake entry 0 */
      poisoned[n - 4 + k] = (uint8_t)(good >> (8 * k));   /* fake entry 1 = the probe */
    }
    uint32_t fake_tab = 0x08000000u + n - 8;
    for (int k = 0; k < 4; k++) poisoned[0x100 + 0xC4 + k] = (uint8_t)(fake_tab >> (8 * k));
    chk("bounds", "the extent-trap image still opens", rom_open(&rc, mem_read, &pc, n));
    rom_text_open(&rt, &rc);
    chk("bounds", "a table that does not FIT is rejected even when its probe reads",
        !rom_text_have(&rt, ROM_TEXT_ABILITY));
    chk("bounds", "items keep working next to it", rom_text_have(&rt, ROM_TEXT_ITEM));
  }

  free(poisoned);
  free(rom);
}

/* ---- the read cache ---------------------------------------------------------------
 *
 * These are PERFORMANCE assertions, and they are here because nothing else can make
 * them: PDNA_TARGET=delta compiles the whole SD path out, so no emulator run can see
 * what a description costs on hardware. Measured before the cache existed: 2 reads per
 * description, EVERY time, including asking for the same id again -- and the summary
 * (card_info -> app_ability_desc) plus the item/move pickers re-ask on every keypress,
 * with key-repeat on.
 *
 * They also pin the two staleness rules, which is the part that could silently corrupt
 * what the user reads: a reopen and a source switch must both throw the cache away. */
static void cache_tests(const char* dir) {
  char path[512];
  snprintf(path, sizeof path, "%s/Emerald.gba", dir);
  uint32_t n = 0;
  uint8_t* img = slurp(path, &n);
  if (!img) { printf("  SKIP cache: no Emerald.gba\n"); return; }

  MemCtx mc = { img, n };
  RomCtx rc; RomText rt; char b[ROM_TEXT_MAX], b2[ROM_TEXT_MAX];
  if (!rom_open(&rc, mem_read, &mc, n) || !rom_text_open(&rt, &rc)) {
    chk("cache", "Emerald opens", 0); free(img); return;
  }

  long r0 = g_reads;
  chk("cache", "a cold ability read succeeds", rom_text_get(&rt, ROM_TEXT_ABILITY, 13, b, sizeof b));
  long cold = g_reads - r0;
  chk("cache", "a cold description costs at most 2 reads", cold <= 2);

  /* The summary re-renders card 0 on every L/R, so this is the shape of the hot path. */
  r0 = g_reads;
  for (int i = 0; i < 8; i++) rom_text_get(&rt, ROM_TEXT_ABILITY, 13, b2, sizeof b2);
  chk("cache", "re-asking for the same id costs ZERO reads", g_reads - r0 == 0);
  chk("cache", "the memoised string is the one that was decoded", strcmp(b, b2) == 0);

  /* A list scroll walks consecutive ids; the pointer-table window must absorb the
   * pointer read, so a row costs strictly less than the two it used to. */
  r0 = g_reads;
  for (int id = 1; id <= 40; id++) rom_text_get(&rt, ROM_TEXT_MOVE, (uint16_t)id, b2, sizeof b2);
  chk("cache", "scrolling 40 move rows costs under 1.5 reads a row", g_reads - r0 < 60);

  r0 = g_reads;
  for (int id = 1; id <= 40; id++) rom_text_get(&rt, ROM_TEXT_ITEM, (uint16_t)id, b2, sizeof b2);
  chk("cache", "scrolling 40 item rows costs under 1.5 reads a row", g_reads - r0 < 60);

  /* A truncating `cap` must not poison a later caller with room for the whole string. */
  char small[8];
  rom_text_get(&rt, ROM_TEXT_ABILITY, 14, small, sizeof small);
  chk("cache", "a full read after a truncated one is still complete",
      rom_text_get(&rt, ROM_TEXT_ABILITY, 14, b2, sizeof b2) && strlen(b2) > sizeof small);

  /* STALENESS 1 -- a different source, opened while this one is still alive. Repoint
   * ability 13's pointer-table entry at ability 1's string (the table address is the GF
   * header's abilityDescriptions field, ROM+0x100+0xC4), so the copy must read back
   * ability 1's text for id 13. If either cache survived the switch it reads back 13's. */
  uint8_t* copy = (uint8_t*)malloc(n);
  memcpy(copy, img, n);
  uint32_t tbl = (uint32_t)copy[0x1C4] | ((uint32_t)copy[0x1C5] << 8) |
                 ((uint32_t)copy[0x1C6] << 16) | ((uint32_t)copy[0x1C7] << 24);
  uint32_t toff = tbl - 0x08000000u;
  memcpy(copy + toff + 13 * 4, copy + toff + 1 * 4, 4);

  MemCtx mc2 = { copy, n };
  RomCtx rc2; RomText rt2; char b3[ROM_TEXT_MAX], b4[ROM_TEXT_MAX];
  if (rom_open(&rc2, mem_read, &mc2, n) && rom_text_open(&rt2, &rc2)) {
    rom_text_get(&rt2, ROM_TEXT_ABILITY, 1, b3, sizeof b3);
    rom_text_get(&rt2, ROM_TEXT_ABILITY, 13, b4, sizeof b4);
    chk("cache", "a reopened source is not served the old source's string",
        strcmp(b4, b3) == 0 && strcmp(b4, b) != 0);
    /* STALENESS 2 -- going BACK to the first source, with no reopen at all. */
    rom_text_get(&rt, ROM_TEXT_ABILITY, 13, b4, sizeof b4);
    chk("cache", "switching back to the first source re-reads it", strcmp(b4, b) == 0);
  } else {
    chk("cache", "the repointed copy still opens", 0);
  }
  free(copy);
  free(img);
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  /* run_host_tests.py hands every argv[1]-reading test the .sav corpus -- this test
   * wants the ROM DIRECTORY, so only accept an argument that is one. */
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }

  static const RomCase k_cases[] = {
    { "Emerald",   "Emerald.gba",   1, 377, ROM_TEXT_GROUP_E,    POTION_RSE,  POUND_RSE  },
    { "FireRed",   "FireRed.gba",   1, 375, ROM_TEXT_GROUP_FRLG, POTION_FRLG, POUND_FRLG },
    { "LeafGreen", "LeafGreen.gba", 1, 375, ROM_TEXT_GROUP_FRLG, POTION_FRLG, POUND_FRLG },
    /* Ruby rev2 and Sapphire rev1 are the two R/S revisions with pinned rows (they are
     * the dumps that exist here). Any other R/S revision has no pin and must close. */
    { "Ruby",      "Ruby.gba",      1, 349, ROM_TEXT_GROUP_RS,   POTION_RSE,  POUND_RSE  },
    { "Sapphire",  "Sapphire.gba",  1, 349, ROM_TEXT_GROUP_RS,   POTION_RSE,  POUND_RSE  },
  };
  for (unsigned i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) run_rom(dir, &k_cases[i]);

  unpinned_revision_tests(dir);
  compare_with_embedded(dir);
  bounds_tests(dir);
  cache_tests(dir);

  printf("rom_text test: %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
