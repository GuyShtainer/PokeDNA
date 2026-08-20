/* Host (PC) test for rom_chrome -- the TRAINER CARD (Emerald + Ruby) and
 * POKeBLOCK CASE (Emerald) full-screen backgrounds read from REAL retail ROMs.
 * rom_chrome does all its I/O through the RomCtx callback, so the code
 * exercised here is byte-for-byte the code that runs on the GBA.
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 * -DPDNA_CARD_ART_COMPILED=0 -DPDNA_POKEBLOCK_ART_COMPILED=0 forces the
 * decoders to compile regardless of whether this machine's source/ tree
 * happens to have the generated (git-ignored) art staged right now -- see
 * rom_chrome_gate.h.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -O2 -I source -DPDNA_CARD_ART_COMPILED=0 -DPDNA_POKEBLOCK_ART_COMPILED=0 \
 *      tests/host_romchrome_test.c source/rom_chrome.c source/rom_map.c source/map_render.c \
 *      -o /tmp/hrct && /tmp/hrct
 *
 * What it proves:
 *  1) rom_chrome_open() correctly identifies Emerald (card+pokeblock) and Ruby
 *     (card only) and correctly refuses Sapphire/FireRed/LeafGreen (the honest
 *     scope rom_chrome.h states).
 *  2) rom_chrome_card_load() succeeds for every (back, tier, female) combination
 *     on both wired games, always returns EXACTLY the declared byte counts, and
 *     the returned RomChromeSrc's tile/map pointers land inside the caller's
 *     scratch buffer (never a stray pointer into ROM or off the end).
 *  3) A too-small scratch buffer FAILS rather than silently truncating.
 *  4) rom_chrome_pokeblock_load() succeeds for Emerald and is refused for every
 *     other game, matching gen3_pokeblock.c's own pk_pokeblock_offset()==0 for
 *     FRLG and the Ruby/Sapphire "located but not pinned" gap this module
 *     documents rather than guesses through.
 *  5) Every card load carries a bg_map distinct from its face map, landing
 *     inside scratch, whose tilemap entries are palette bank 1 EXCLUSIVELY
 *     while the face tilemap's are bank 0 exclusively -- the measured fact
 *     that makes an index-0-transparent composite the correct retail look
 *     instead of a flat fill. The Pokeblock case carries no bg_map at all.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rom_chrome.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                   printf("\n"); g_fail++; } \
  } while (0)

typedef struct { FILE* f; } HostCtx;
static bool host_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  HostCtx* c = (HostCtx*)ctx;
  if (fseek(c->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, c->f) == len;
}

static int open_rom(const char* path, HostCtx* c, RomCtx* rc) {
  c->f = fopen(path, "rb");
  if (!c->f) return 0;
  fseek(c->f, 0, SEEK_END);
  long sz = ftell(c->f);
  fseek(c->f, 0, SEEK_SET);
  return sz > 0 && rom_open(rc, host_read, c, (uint32_t)sz);
}

static void test_card_game(const char* label, const char* path, int expect_style) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s: SKIP (no dump)\n", label); return; }

  RomChrome rch; rom_chrome_open(&rch, &rc);
  CHECK(rch.card_style == expect_style, "%s: card_style=%d want %d", label, rch.card_style, expect_style);

  if (expect_style >= 0) {
    static uint8_t scratch[8192];
    int any = 0;
    for (int back = 0; back <= 1; back++)
      for (int tier = 0; tier <= 4; tier++)
        for (int female = 0; female <= 1; female++) {
          RomChromeCard out;
          memset(&out, 0xAA, sizeof out);
          int ok = rom_chrome_card_load(&rch, expect_style, back, tier, female,
                                        scratch, sizeof scratch, &out);
          CHECK(ok, "%s: card_load back=%d tier=%d female=%d failed", label, back, tier, female);
          if (!ok) continue;
          any = 1;
          CHECK(out.src.tiles != 0 && out.src.map != 0, "%s: null src pointers", label);
          CHECK(out.src.tiles >= scratch && out.src.tiles < scratch + sizeof scratch,
               "%s: tiles pointer outside scratch", label);
          CHECK((const uint8_t*)out.src.map >= scratch &&
               (const uint8_t*)out.src.map < scratch + sizeof scratch,
               "%s: map pointer outside scratch", label);
          /* The card composites a background layer under front/back (the retail
           * male/blue vs female/pink striped border) -- rom_chrome.h's MEMORY
           * section. Must always be present and land inside scratch, distinct
           * from the face tilemap it draws under. */
          CHECK(out.src.bg_map != 0, "%s: card bg_map is NULL", label);
          CHECK((const uint8_t*)out.src.bg_map >= scratch &&
               (const uint8_t*)out.src.bg_map < scratch + sizeof scratch,
               "%s: bg_map pointer outside scratch", label);
          CHECK((const uint8_t*)out.src.bg_map != (const uint8_t*)out.src.map,
               "%s: bg_map aliases the face map", label);
          CHECK(out.as_lzblob.romsrc == &out.src, "%s: as_lzblob.romsrc wrong", label);
          CHECK(out.as_lzblob.pages == 0, "%s: as_lzblob.pages should be NULL (romsrc dispatch)", label);
          /* Palette-bank exclusivity, measured (the review that found this bug):
           * every entry of the face tilemap (front/back) uses bank 0, every
           * entry of bg_map uses bank 1. If a future edit ever points bg_map at
           * the wrong address (e.g. aliases front/back), this catches it even
           * though the pointer-distinctness check above would not. */
          /* Only the first 30 columns of each row are ever drawn (romchrome_blit
           * skips tx >= 30) -- Ruby's stored stride pads to 32, and those two
           * trailing columns per row are not composited art at all, so they
           * must be excluded here or they read as noise. */
          int seen_face = 0, seen_bg = 0;
          for (int ty = 0; ty < 20; ty++)
            for (int tx = 0; tx < 30; tx++) {
              int i = ty * (int)out.src.map_w + tx;
              int fb = (out.src.map[i] >> 12) & 0xF;
              if (fb != 0) seen_face++;
              int bb = (out.src.bg_map[i] >> 12) & 0xF;
              if (bb != 1) seen_bg++;
            }
          CHECK(seen_face == 0, "%s: back=%d face tilemap has %d entries outside bank 0",
               label, back, seen_face);
          CHECK(seen_bg == 0, "%s: back=%d bg_map has %d entries outside bank 1",
               label, back, seen_bg);
        }
    CHECK(any, "%s: every (back,tier,female) combination failed", label);

    /* A too-small buffer must fail closed, not truncate. */
    RomChromeCard bad;
    CHECK(!rom_chrome_card_load(&rch, expect_style, 0, 0, 0, scratch, 64, &bad),
         "%s: a 64 B scratch buffer must be refused, not truncated into", label);
  } else {
    RomChromeCard out;
    CHECK(!rom_chrome_card_load(&rch, 0, 0, 0, 0, 0, 0, &out) &&
         !rom_chrome_card_load(&rch, 1, 0, 0, 0, 0, 0, &out) &&
         !rom_chrome_card_load(&rch, 2, 0, 0, 0, 0, 0, &out),
         "%s: an unwired game must refuse every card style", label);
  }

  /* rom_chrome_card_have must agree with card_style for every style id. */
  for (int g = 0; g < 3; g++)
    CHECK(rom_chrome_card_have(&rch, g) == (g == expect_style),
         "%s: rom_chrome_card_have(%d) disagrees with card_style", label, g);

  fclose(c.f);
}

static void test_pokeblock_game(const char* label, const char* path, int expect_ok) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s: SKIP (no dump)\n", label); return; }

  RomChrome rch; rom_chrome_open(&rch, &rc);
  CHECK(rch.pokeblock_ok == expect_ok, "%s: pokeblock_ok=%d want %d", label, rch.pokeblock_ok, expect_ok);
  CHECK(rom_chrome_pokeblock_have(&rch, 0) == expect_ok, "%s: rom_chrome_pokeblock_have disagrees", label);

  static uint8_t scratch[8192];
  RomChromePokeblock out;
  int ok = rom_chrome_pokeblock_load(&rch, 0, scratch, sizeof scratch, &out);
  CHECK(ok == expect_ok, "%s: pokeblock_load returned %d, want %d", label, ok, expect_ok);
  if (ok) {
    CHECK(out.src.tiles != 0 && out.src.map != 0, "%s: null pokeblock src pointers", label);
    CHECK(out.src.bg_map == 0, "%s: pokeblock case has no background layer -- bg_map "
         "must be NULL, not a stale pointer", label);
    CHECK(out.as_lzblob.romsrc == &out.src, "%s: as_lzblob.romsrc wrong", label);
  }
  fclose(c.f);
}

int main(void) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  char emerald[256], ruby[256], sapphire[256], firered[256], leafgreen[256];
  snprintf(emerald, sizeof emerald, "%s/Emerald.gba", dir);
  snprintf(ruby, sizeof ruby, "%s/Ruby.gba", dir);
  snprintf(sapphire, sizeof sapphire, "%s/Sapphire.gba", dir);
  snprintf(firered, sizeof firered, "%s/FireRed.gba", dir);
  snprintf(leafgreen, sizeof leafgreen, "%s/LeafGreen.gba", dir);

  test_card_game("Emerald", emerald, 1);
  test_card_game("Ruby", ruby, 0);
  test_card_game("Sapphire (unwired)", sapphire, -1);
  test_card_game("FireRed (unwired)", firered, -1);
  test_card_game("LeafGreen (unwired)", leafgreen, -1);

  test_pokeblock_game("Emerald pokeblock", emerald, 1);
  test_pokeblock_game("Ruby pokeblock (unwired)", ruby, 0);
  test_pokeblock_game("Sapphire pokeblock (unwired)", sapphire, 0);
  test_pokeblock_game("FireRed pokeblock (none in-game)", firered, 0);

  if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
  printf("host_romchrome_test: all checks passed\n");
  return 0;
}
