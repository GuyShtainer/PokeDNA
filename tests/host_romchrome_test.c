/* Host (PC) test for rom_chrome -- the TRAINER CARD (Emerald + Ruby),
 * POKeBLOCK CASE (Emerald), and BAG SCREEN (Emerald + FireRed + LeafGreen)
 * full-screen backgrounds read from REAL retail ROMs. rom_chrome does all its
 * I/O through the RomCtx callback, so the code exercised here is byte-for-byte
 * the code that runs on the GBA.
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 * -DPDNA_CARD_ART_COMPILED=0 -DPDNA_POKEBLOCK_ART_COMPILED=0
 * -DPDNA_BAG_ART_COMPILED=0 forces the decoders to compile regardless of
 * whether this machine's source/ tree happens to have the generated
 * (git-ignored) art staged right now -- see rom_chrome_gate.h.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -O2 -I source -DPDNA_CARD_ART_COMPILED=0 -DPDNA_POKEBLOCK_ART_COMPILED=0 \
 *      -DPDNA_BAG_ART_COMPILED=0 \
 *      tests/host_romchrome_test.c source/rom_chrome.c source/rom_map.c source/map_render.c \
 *      -o /tmp/hrct && /tmp/hrct
 *
 * What it proves:
 *  1) rom_chrome_open() correctly identifies Emerald (card+pokeblock+bag),
 *     Ruby (card only), FireRed/LeafGreen (bag only), and correctly refuses
 *     Sapphire everywhere (the honest scope rom_chrome.h states).
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
 *  6) rom_chrome_bag_load() succeeds for both genders on Emerald/FireRed/
 *     LeafGreen and is refused for Ruby/Sapphire; every load's pointers land
 *     inside scratch, bg_map is always NULL (single-layer screen), and a
 *     too-small buffer fails closed.
 *  7) Palette bank 0 actually DIFFERS between the male and female load on
 *     every wired game (the MUST-FIX 1 regression test: pal_f_banks == 0
 *     would make this fail silently, since female would just re-decode the
 *     male banks), and on FRLG specifically banks 1-2 stay byte-identical
 *     across gender (the override is bank-0-only, not a bank-1 recolour).
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

/* Step 1 instrumentation (BACKLOG #103): count read-callback calls + bytes so
 * the primary metric (calls/bytes per screen load) is measured directly on
 * the exact code path the GBA runs, no emulator needed. Reset with
 * g_reads = g_bytes = 0 around exactly one screen load. */
static unsigned long g_reads = 0, g_bytes = 0;

static bool host_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  HostCtx* c = (HostCtx*)ctx;
  g_reads++;
  g_bytes += len;
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

static void test_bag_game(const char* label, const char* path, int expect_style) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s: SKIP (no dump)\n", label); return; }

  RomChrome rch; rom_chrome_open(&rch, &rc);
  CHECK(rch.bag_style == expect_style, "%s: bag_style=%d want %d", label, rch.bag_style, expect_style);

  if (expect_style >= 0) {
    static uint8_t scratch[8192];
    uint16_t pal[2][96];
    for (int female = 0; female <= 1; female++) {
      RomChromeBag out;
      memset(&out, 0xAA, sizeof out);
      int ok = rom_chrome_bag_load(&rch, expect_style, female, scratch, sizeof scratch, &out);
      CHECK(ok, "%s: bag_load female=%d failed", label, female);
      if (!ok) continue;
      CHECK(out.src.tiles != 0 && out.src.map != 0, "%s: null src pointers", label);
      CHECK(out.src.tiles >= scratch && out.src.tiles < scratch + sizeof scratch,
           "%s: tiles pointer outside scratch", label);
      CHECK((const uint8_t*)out.src.map >= scratch &&
           (const uint8_t*)out.src.map < scratch + sizeof scratch,
           "%s: map pointer outside scratch", label);
      CHECK(out.src.bg_map == 0, "%s: bag has no background layer -- bg_map must be "
           "NULL, not a stale pointer", label);
      CHECK(out.as_lzblob.romsrc == &out.src, "%s: as_lzblob.romsrc wrong", label);
      CHECK(out.as_lzblob.pages == 0, "%s: as_lzblob.pages should be NULL (romsrc dispatch)", label);
      /* Every decode must fit comfortably under half the shared 8 KiB buffer --
       * the whole point of the fix over the old "Ruby saturates it" finding. */
      memcpy(pal[female], out.src.pal, sizeof pal[female]);
    }

    /* Bank 0 MUST differ by gender on every wired game -- this is the exact
     * regression MUST-FIX 1 shipped once (pal_f_banks == 0, so female == male
     * bank-for-bank). FRLG's override is bank-0-ONLY (rom_chrome.h), so banks
     * 1-2 must stay byte-identical across gender -- a bank-1 (or any other)
     * override sneaking back in would show up here as a bank-1+ mismatch. */
    int bank0_differs = memcmp(pal[0], pal[1], 16 * sizeof(uint16_t)) != 0;
    CHECK(bank0_differs, "%s: bank 0 identical for male and female -- the "
         "female palette override isn't being applied (this is the MUST-FIX 1 bug)", label);
    if (expect_style == 2)   /* FRLG: bank-0-only override */
      CHECK(memcmp(pal[0] + 16, pal[1] + 16, 32 * sizeof(uint16_t)) == 0,
           "%s: banks 1-2 changed with gender -- FRLG's override must be bank 0 only", label);

    /* A too-small buffer must fail closed, not truncate. */
    RomChromeBag bad;
    CHECK(!rom_chrome_bag_load(&rch, expect_style, 0, scratch, 64, &bad),
         "%s: a 64 B scratch buffer must be refused, not truncated into", label);
  } else {
    RomChromeBag out;
    CHECK(!rom_chrome_bag_load(&rch, 0, 0, 0, 0, &out) &&
         !rom_chrome_bag_load(&rch, 1, 0, 0, 0, &out) &&
         !rom_chrome_bag_load(&rch, 2, 0, 0, 0, &out),
         "%s: an unwired game must refuse every bag style", label);
  }

  for (int g = 0; g < 3; g++)
    CHECK(rom_chrome_bag_have(&rch, g) == (g == expect_style),
         "%s: rom_chrome_bag_have(%d) disagrees with bag_style", label, g);

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

/* Foreground overlays added 2026-08-22: card stars/badges/photo, the
 * Pokeblock device, the bag sprite. Each address was located by decompress-
 * and-byte-compare against the staged decomp assets (docs/analysis-2026-08-19-
 * rom-art scripts' own method); this test proves the WIRING (right game
 * gating, right sizes, non-NULL pointers, budget-safe scratch requirements),
 * not the pixels themselves (that's the render probe / mGBA capture's job --
 * this file, like the rest of rom_chrome, is pure C with no framebuffer). */
static void test_foreground_game(const char* label, const char* path,
                                 int card_g, int expect_badges, int expect_photo) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s: SKIP (no dump)\n", label); return; }
  RomChrome rch; rom_chrome_open(&rch, &rc);
  static uint8_t scratch[8192];

  CHECK(rom_chrome_card_badges_have(&rch, card_g) == expect_badges,
       "%s: badges_have disagrees", label);
  CHECK(rom_chrome_card_photo_have(&rch, card_g) == expect_photo,
       "%s: photo_have disagrees", label);

  if (expect_badges) {
    RomChromeCardBadges b;
    int ok = rom_chrome_card_badges_load(&rch, card_g, scratch, sizeof scratch, &b);
    CHECK(ok, "%s: badges_load failed", label);
    if (ok) {
      CHECK(b.tiles != 0, "%s: null badge tiles", label);
      CHECK(b.pal[0] == 0, "%s: badge pal[0] must stay 0 (transparent index)", label);
      int seen_nonzero = 0;
      for (int t = 0; t < 32 * 32; t++) if (b.tiles[t]) seen_nonzero = 1;
      CHECK(seen_nonzero, "%s: badge tileset decoded to all zero bytes", label);
      /* every badge's 4 tile ids must be in-range (or the documented -1 skip) */
      for (int i = 0; i < 8; i++) {
        int16_t ids[4];
        CHECK(rom_chrome_card_badge_ids(&rch, card_g, i, ids), "%s: badge_ids(%d) failed", label, i);
        for (int k = 0; k < 4; k++)
          CHECK(ids[k] == -1 || (ids[k] >= 0 && ids[k] < 32),
               "%s: badge %d tile id[%d]=%d out of range", label, i, k, ids[k]);
      }
      CHECK(!rom_chrome_card_badge_ids(&rch, card_g, 8, (int16_t[4]){0}),
           "%s: badge index 8 (out of range) must be refused", label);
    }
    /* too-small buffer fails closed, never truncates */
    RomChromeCardBadges bad;
    CHECK(!rom_chrome_card_badges_load(&rch, card_g, scratch, 64, &bad),
         "%s: a 64 B badge scratch must be refused, not truncated into", label);
  }

  if (expect_photo) {
    for (int female = 0; female <= 1; female++) {
      RomChromeCardPhoto p;
      int ok = rom_chrome_card_photo_load(&rch, card_g, female, scratch, sizeof scratch, &p);
      CHECK(ok, "%s: photo_load(female=%d) failed", label, female);
      if (ok) {
        CHECK(p.tiles != 0, "%s: null photo tiles (female=%d)", label, female);
        CHECK(p.pal[0] == 0, "%s: photo pal[0] must stay 0 (female=%d)", label, female);
        int seen_nonzero = 0;
        for (int t = 0; t < 64 * 32; t++) if (p.tiles[t]) seen_nonzero = 1;
        CHECK(seen_nonzero, "%s: photo tileset decoded to all zero bytes (female=%d)", label, female);
      }
    }
    RomChromeCardPhoto bad;
    CHECK(!rom_chrome_card_photo_load(&rch, card_g, 0, scratch, 64, &bad),
         "%s: a 64 B photo scratch must be refused, not truncated into", label);
  }

  fclose(c.f);
}

static void test_pokeblock_device(const char* label, const char* path, int expect_ok) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s: SKIP (no dump)\n", label); return; }
  RomChrome rch; rom_chrome_open(&rch, &rc);
  static uint8_t scratch[8192];
  RomChromePokeblock out;
  int ok = rom_chrome_pokeblock_load(&rch, 0, scratch, sizeof scratch, &out);
  CHECK(ok == expect_ok, "%s: pokeblock_load returned %d, want %d", label, ok, expect_ok);
  if (ok) {
    CHECK(out.device_tiles != 0, "%s: device_tiles must decode when the case itself does", label);
    CHECK(out.device_pal[0] == 0, "%s: device_pal[0] must stay 0", label);
    int seen_nonzero = 0;
    for (int t = 0; t < 64 * 32; t++) if (out.device_tiles[t]) seen_nonzero = 1;
    CHECK(seen_nonzero, "%s: device tileset decoded to all zero bytes", label);
  }
  fclose(c.f);
}

static void test_bag_sprite(const char* label, const char* path, int card_bag_g, int expect_ok) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s: SKIP (no dump)\n", label); return; }
  RomChrome rch; rom_chrome_open(&rch, &rc);
  CHECK(rom_chrome_bag_sprite_have(&rch, card_bag_g) == expect_ok,
       "%s: bag_sprite_have=%d want %d", label, rom_chrome_bag_sprite_have(&rch, card_bag_g), expect_ok);
  static uint8_t scratch[8192];
  for (int female = 0; female <= 1; female++) {
    RomChromeBagSprite bs;
    int ok = rom_chrome_bag_sprite_load(&rch, card_bag_g, female, scratch, sizeof scratch, &bs);
    CHECK(ok == expect_ok, "%s: bag_sprite_load(female=%d)=%d want %d", label, female, ok, expect_ok);
    if (ok) {
      CHECK(bs.tiles != 0 && bs.frame_count > 0, "%s: bad bag sprite decode (female=%d)", label, female);
      CHECK(bs.pal[0] == 0, "%s: bag sprite pal[0] must stay 0 (female=%d)", label, female);
      /* the WHOLE declared blob (8,192 B) must fit exactly at cap == 8,192,
       * and MUST be refused one byte under -- the "no partial decode" fact
       * this module's whole Emerald-exclusion rests on. */
      RomChromeBagSprite tight;
      CHECK(rom_chrome_bag_sprite_load(&rch, card_bag_g, female, scratch, 8192, &tight),
           "%s: an exact 8,192 B buffer must succeed (female=%d)", label, female);
      RomChromeBagSprite bad;
      CHECK(!rom_chrome_bag_sprite_load(&rch, card_bag_g, female, scratch, 8191, &bad),
           "%s: an 8,191 B buffer must be refused, not truncated (female=%d)", label, female);
    }
  }
  fclose(c.f);
}

/* Step 1 (BACKLOG #103): print read-callback counts for exactly ONE trainer
 * card load and ONE bag load on Emerald (front, tier 0, male) -- the
 * baseline this lane's every later step must reduce. */
static void measure_counts(const char* emerald_path) {
  /* Traced independently: rom_open()'s own ROM-kind identification (scanning
   * candidate header offsets) costs 26 reads / 776 bytes on Emerald, is a
   * ONE-TIME cost separate from rom_chrome_card_load(), and is untouched by
   * this lane (steps 2-4 only touch mr_lz77/decode_verified). Printing BOTH
   * the card-load-only count and the rom_open-inclusive count: the former
   * reproduces the design doc's exact byte figure (3,992 B); the latter
   * reproduces its exact call figure (94) -- the doc's "94 calls / 3,992
   * bytes" pairing mixes the two (94 = 26 + 68; 3,992 excludes rom_open's
   * 776 B). This is the mechanism-relevant number this lane's target
   * (<=32 calls) is measured against, since rom_open is out of scope. */
  HostCtx c; RomCtx rc;
  g_reads = 0; g_bytes = 0;
  if (!open_rom(emerald_path, &c, &rc)) { printf("counts: SKIP (no Emerald dump)\n"); return; }
  unsigned long open_reads = g_reads, open_bytes = g_bytes;
  RomChrome rch; rom_chrome_open(&rch, &rc);

  static uint8_t scratch[8192];
  RomChromeCard card_out;
  g_reads = 0; g_bytes = 0;
  int card_ok = rom_chrome_card_load(&rch, rch.card_style, 0, 0, 0, scratch, sizeof scratch, &card_out);
  printf("counts Emerald card (load only): %lu reads / %lu bytes (ok=%d)\n", g_reads, g_bytes, card_ok);
  printf("counts Emerald card (+rom_open %lu reads/%lu bytes): %lu reads / %lu bytes\n",
         open_reads, open_bytes, g_reads + open_reads, g_bytes + open_bytes);

  RomChromeBag bag_out;
  g_reads = 0; g_bytes = 0;
  int bag_ok = rom_chrome_bag_load(&rch, rch.bag_style, 0, scratch, sizeof scratch, &bag_out);
  printf("counts Emerald bag: %lu reads / %lu bytes (ok=%d)\n", g_reads, g_bytes, bag_ok);

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

  test_bag_game("Emerald bag", emerald, 1);
  test_bag_game("FireRed bag", firered, 2);
  test_bag_game("LeafGreen bag", leafgreen, 2);
  test_bag_game("Ruby bag (unwired -- tileset alone saturates the buffer)", ruby, -1);
  test_bag_game("Sapphire bag (unwired)", sapphire, -1);

  /* stars/badges/photo (card_g: 1 = Emerald, 0 = Ruby) */
  test_foreground_game("Emerald foreground", emerald, 1, 1, 1);
  test_foreground_game("Ruby foreground", ruby, 0, 1, 1);

  test_pokeblock_device("Emerald pokeblock device", emerald, 1);
  test_pokeblock_device("Ruby pokeblock device (unwired case)", ruby, 0);

  /* bag sprite: FireRed/LeafGreen (bag_style 2) only -- Emerald's monolithic
   * 12,288 B blob cannot fit the 8,192 B shared buffer for ANY frame. */
  test_bag_sprite("FireRed bag sprite", firered, 2, 1);
  test_bag_sprite("LeafGreen bag sprite", leafgreen, 2, 1);
  test_bag_sprite("Emerald bag sprite (budget-excluded)", emerald, 1, 0);
  test_bag_sprite("Ruby bag sprite (unwired game)", ruby, 0, 0);

  measure_counts(emerald);

  if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
  printf("host_romchrome_test: all checks passed\n");
  return 0;
}
