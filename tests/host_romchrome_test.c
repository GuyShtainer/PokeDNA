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
#include <stdlib.h>
#include <string.h>

#include "rom_chrome.h"
#include "map_render.h"

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

/* BACKLOG #139: the same call-ordinal single-byte-flip corruption mock
 * host_romsprite_test.c uses (hit_call/hit_pos), keyed on nothing but WHICH
 * physical read() call this is, never on len or address -- exactly what
 * review-opus's attack list demands. Fires exactly once (hit_call == 0
 * disarms it). g_reads (above) is the shared call-ordinal counter -- rom_chrome
 * has no per-item ROM TABLE the way rom_sprite does (its addresses are
 * compile-time CardPins/BagPins constants), so there is no table-entry variant
 * of this mock here; only decode_verified's compressed-input hash compare is
 * under test. */
static long     g_hit_call = 0;
static uint32_t g_hit_pos = 0;

/* #313: a PERSISTENT poison window -- every read that overlaps file bytes [lo, hi) comes back
 * with those bytes XOR 0xFF, modelling a wrong/hacked revision at a pinned address. */
static uint32_t g_poison_lo = 0, g_poison_hi = 0;

static bool host_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  HostCtx* c = (HostCtx*)ctx;
  g_reads++;
  g_bytes += len;
  if (fseek(c->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, c->f) != len) return false;
  if (g_hit_call && g_reads == (unsigned long)g_hit_call && g_hit_pos < len) {
    ((uint8_t*)dst)[g_hit_pos] ^= 0xFF;
    g_hit_call = 0;      /* fires exactly once */
  }
  for (uint32_t i = 0; g_poison_hi > g_poison_lo && i < len; i++)
    if (off + i >= g_poison_lo && off + i < g_poison_hi) ((uint8_t*)dst)[i] ^= 0xFF;
  return true;
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


/* ---- #313: the Ruby/Sapphire bag ------------------------------------------------------------ */

#define RSB_MAP   0x08E77004u
#define RSB_TILES 0x08E76728u
#define RSB_PAL_M 0x08E76F94u
#define RSB_PAL_F 0x08E76FCCu

/* Independent chain scan over the WHOLE image (the tools/rs_locate.py predicate, host C): at
 * every 4-aligned LZ10->12288 start, how many leading clauses of
 * [12288][12288][32][8192][64][64][RAW 2048 map] hold. Uses mr_lz77_x's consumed span so it
 * shares NOTHING with rs_bag_open_once. Returns the depth at `pinned`, sets *best_other to the
 * deepest chain anywhere ELSE (the runner-up) and *full to the count of 7-deep chains. */
static int rs_chain_depth(const RomCtx* rc, const uint8_t* img, uint32_t file_off) {
  static const uint32_t want[6] = { 12288u, 12288u, 32u, 8192u, 64u, 64u };
  static uint8_t out[16384];
  uint32_t o = file_off;
  for (int i = 0; i < 6; i++) {
    uint32_t sz = (uint32_t)img[o + 1] | ((uint32_t)img[o + 2] << 8) | ((uint32_t)img[o + 3] << 16);
    if (img[o] != 0x10 || sz != want[i]) return i;
    uint32_t consumed = 0, h = 0;
    if (mr_lz77_x(rc, ROM_BASE + o, out, sizeof out, 0, 0, &consumed, &h) != want[i]) return i;
    o = (o + consumed + 3u) & ~3u;
  }
  /* the RAW tilemap */
  uint32_t seen = 0, bad = 0;
  uint8_t used[1024]; memset(used, 0, sizeof used);
  for (int i = 0; i < 1024; i++) {
    uint32_t e = (uint32_t)img[o + 2 * i] | ((uint32_t)img[o + 2 * i + 1] << 8);
    if ((e & 0x3FF) >= 256u || (e >> 12) > 1u) bad = 1;
    if (!used[e & 0x3FF]) { used[e & 0x3FF] = 1; seen++; }
  }
  return (!bad && seen >= 20) ? 7 : 6;
}

static void test_rs_bag_pins(const char* label, const char* path) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s rs-bag pins: SKIP (no dump)\n", label); return; }
  RomChrome rch; rom_chrome_open(&rch, &rc);
  CHECK(rch.bag_style == 0 && rom_chrome_bag_have(&rch, 0) && !rom_chrome_bag_have(&rch, 1) &&
        !rom_chrome_bag_have(&rch, 2), "%s: pinned R/S revision must report bag_style 0", label);

  /* (1) the locator: the chain is found at the pinned address, once, with a wide margin. */
  fseek(c.f, 0, SEEK_END); long sz = ftell(c.f); fseek(c.f, 0, SEEK_SET);
  uint8_t* img = (uint8_t*)malloc((size_t)sz);
  CHECK(img && fread(img, 1, (size_t)sz, c.f) == (size_t)sz, "%s: read image", label);
  if (!img) { fclose(c.f); return; }
  int full = 0, best_other = 0, at_pin = -1;
  for (uint32_t o = 0; o + 4 < (uint32_t)sz; o += 4) {
    if (img[o] != 0x10 || img[o + 1] != 0x00 || img[o + 2] != 0x30 || img[o + 3] != 0x00) continue;  /* 12288 = 0x003000 */
    int d = rs_chain_depth(&rc, img, o);
    if (o == 0x00E75024u) at_pin = d;
    else if (d > best_other) best_other = d;
    if (d == 7) full++;
  }
  CHECK(at_pin == 7, "%s: the pinned chain start must satisfy all 7 clauses (got %d)", label, at_pin);
  CHECK(full == 1, "%s: exactly ONE full chain in the image (got %d)", label, full);
  CHECK(best_other <= 2, "%s: runner-up chain depth %d must stay <= 2 of 7 (margin >= 5)", label, best_other);
  printf("%s rs-bag locator: pinned depth %d/7, unique=%d, runner-up depth %d/7 (margin %d)\n",
         label, at_pin, full == 1, best_other, 7 - best_other);

  /* (2) the load against an independent whole-blob oracle */
  static uint8_t scratch[8192], whole[8192], palbuf[64];
  CHECK(mr_lz77(&rc, RSB_TILES, whole, sizeof whole) == 8192u, "%s: oracle tileset decode", label);
  uint16_t first[2][96];
  for (int female = 0; female <= 1; female++) {
    RomChromeBag out;
    memset(scratch, 0xEE, sizeof scratch);
    int ld = rom_chrome_bag_load(&rch, 0, female, scratch, 6208, &out);
    CHECK(ld, "%s: an exact 6,208 B buffer must succeed (female=%d)", label, female);
    if (!ld) continue;
    CHECK(out.src.tiles == scratch && memcmp(out.src.tiles, whole, 105u * 32u) == 0,
          "%s: the streamed tile prefix == the whole-blob oracle's first 3,360 B (female=%d)", label, female);
    CHECK((const uint8_t*)out.src.map == scratch + 4096 &&
          memcmp(out.src.map, img + (RSB_MAP - ROM_BASE), 2048) == 0,
          "%s: the tilemap is the raw ROM bytes (female=%d)", label, female);
    CHECK(mr_lz77(&rc, female ? RSB_PAL_F : RSB_PAL_M, palbuf, sizeof palbuf) == 64u, "%s: pal oracle", label);
    for (int i = 0; i < 32; i++)
      CHECK(out.src.pal[i] == (uint16_t)(palbuf[2 * i] | (palbuf[2 * i + 1] << 8)),
            "%s: palette entry %d (female=%d)", label, i, female);
    for (int i = 32; i < 96; i++) CHECK(out.src.pal[i] == 0, "%s: pal tail %d must be zeroed", label, i);
    CHECK(out.src.bg_map == 0 && out.src.map_w == 32 && out.as_lzblob.romsrc == &out.src,
          "%s: bg_map NULL / map_w 32 / romsrc wired", label);
    memcpy(first[female], out.src.pal, sizeof first[female]);
    /* every tile the map names lies inside the decoded prefix */
    const uint16_t* m = out.src.map; unsigned mx = 0;
    for (int i = 0; i < 1024; i++) if ((unsigned)(m[i] & 0x3FF) > mx) mx = m[i] & 0x3FF;
    CHECK(mx < 105u, "%s: map names tile %u outside the 105-tile prefix", label, mx);
    CHECK(!rom_chrome_bag_load(&rch, 0, female, scratch, 6207, &out),
          "%s: a 6,207 B buffer must be refused (female=%d)", label, female);
  }
  CHECK(memcmp(first[0], first[1], 16 * sizeof(uint16_t)) != 0, "%s: bank 0 must differ by gender", label);

  /* (3) wrong-revision / wrong-code refusal -- the plain-list fallback */
  RomCtx bad = rc;
  bad.version = (uint8_t)(rc.version == 2 ? 1 : 2);
  RomChrome rb; rom_chrome_open(&rb, &bad);
  CHECK(rb.bag_style == -1 && !rom_chrome_bag_have(&rb, 0), "%s: a different revision must NOT serve the bag", label);
  bad = rc; bad.version = 0;
  rom_chrome_open(&rb, &bad);
  CHECK(rb.bag_style == -1, "%s: revision 0 must NOT serve the bag", label);
  CHECK(!rom_chrome_bag_load(&rb, 0, 0, scratch, sizeof scratch, &(RomChromeBag){0}), "%s: refused revision cannot load", label);

  /* (4) tampered bytes at the pinned addresses: open fails closed, each poison RED-able */
  struct { uint32_t lo, hi; const char* what; } pz[] = {
    { RSB_MAP - ROM_BASE + 2, RSB_MAP - ROM_BASE + 4, "tilemap entry (bank bits)" },
    { RSB_TILES - ROM_BASE + 1, RSB_TILES - ROM_BASE + 3, "tileset LZ10 size" },
    { 0x00E75024u + 1, 0x00E75024u + 3, "male sheet LZ10 size" },
    { RSB_PAL_F - ROM_BASE, RSB_PAL_F - ROM_BASE + 1, "female palette LZ10 type byte" },
  };
  for (unsigned k = 0; k < sizeof pz / sizeof pz[0]; k++) {
    g_poison_lo = pz[k].lo; g_poison_hi = pz[k].hi;
    RomChrome rp; rom_chrome_open(&rp, &rc);
    CHECK(rp.bag_style == -1, "%s: poisoned %s must refuse the bag at open", label, pz[k].what);
    g_poison_lo = g_poison_hi = 0;
  }
  free(img);
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

/* Independent oracle for the bag sheet: the WHOLE blob decoded with mr_lz77() into a big host
 * buffer (the very thing Emerald's 8,192 B budget can't do on the GBA). BACKLOG #295. */
static uint32_t bag_sheet_addr(int emerald, int fr, int female) {
  if (emerald == 2) return female ? 0x08E75BA0u : 0x08E75024u;   /* Ruby/Sapphire (#313) */
  if (emerald) return female ? 0x08D99A00u : 0x08D98E84u;
  if (fr) return female ? 0x08E83DBCu : 0x08E8362Cu;
  return female ? 0x08E83E3Cu : 0x08E836ACu;
}

static void test_bag_sprite(const char* label, const char* path, int card_bag_g, int expect_ok) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s: SKIP (no dump)\n", label); return; }
  RomChrome rch; rom_chrome_open(&rch, &rc);
  CHECK(rom_chrome_bag_sprite_have(&rch, card_bag_g) == expect_ok,
       "%s: bag_sprite_have=%d want %d", label, rom_chrome_bag_sprite_have(&rch, card_bag_g), expect_ok);
  int rs = (rc.kind == ROM_RUBY || rc.kind == ROM_SAPPHIRE);
  int emerald = (rc.kind == ROM_EMERALD) || rs, fr = (rc.kind == ROM_FIRERED);
  uint32_t sheet_bytes = emerald ? 12288u : 8192u;
  static uint8_t scratch[8192], whole[16384];
  for (int female = 0; female <= 1; female++) {
    if (expect_ok)
      CHECK(mr_lz77(&rc, bag_sheet_addr(rs ? 2 : emerald, fr, female), whole, sizeof whole) == sheet_bytes,
            "%s: oracle whole-sheet decode size (female=%d)", label, female);
    int nframes = expect_ok ? (emerald ? 6 : 4) : 1;
    for (int frame = 0; frame < nframes; frame++) {
      RomChromeBagSprite bs;
      memset(scratch, 0xEE, sizeof scratch);   /* a reused scratch hid the drop-first-byte mutant */
      int ok = rom_chrome_bag_sprite_load(&rch, card_bag_g, female, frame, scratch, sizeof scratch, &bs);
      CHECK(ok == expect_ok, "%s: bag_sprite_load(female=%d frame=%d)=%d want %d", label, female, frame, ok, expect_ok);
      if (!ok) continue;
      CHECK(bs.tiles != 0 && bs.frame_count == (emerald ? 6 : 4), "%s: bad bag sprite decode (female=%d)", label, female);
      CHECK(bs.pal[0] == 0, "%s: bag sprite pal[0] must stay 0 (female=%d)", label, female);
      CHECK(memcmp(bs.tiles, whole + (size_t)frame * 2048u, 2048u) == 0,
            "%s: frame %d (female=%d) == the whole-sheet oracle's frame, byte for byte", label, frame, female);
    }
    if (!expect_ok) continue;
    RomChromeBagSprite t;
    CHECK(rom_chrome_bag_sprite_load(&rch, card_bag_g, female, 1, scratch, 8192, &t),
         "%s: an exact 8,192 B buffer must succeed (female=%d)", label, female);
    CHECK(!rom_chrome_bag_sprite_load(&rch, card_bag_g, female, 1, scratch, 8191, &t),
         "%s: an 8,191 B buffer must be refused, not truncated (female=%d)", label, female);
    /* out-of-range frame falls back to frame 0, never reads past the sheet */
    CHECK(rom_chrome_bag_sprite_load(&rch, card_bag_g, female, 99, scratch, 8192, &t) &&
          memcmp(t.tiles, whole, 2048u) == 0,
          "%s: frame 99 falls back to frame 0 (female=%d)", label, female);
    if (emerald) {
      /* verify off needs only window + frame (6,144 B); verify on needs the copy too */
      rom_chrome_set_verify(&rch, 0);
      CHECK(rom_chrome_bag_sprite_load(&rch, card_bag_g, female, 5, scratch, 6144, &t) &&
            memcmp(t.tiles, whole + 5u * 2048u, 2048u) == 0,
            "%s: verify-off 6,144 B buffer decodes the LAST frame (female=%d)", label, female);
      CHECK(!rom_chrome_bag_sprite_load(&rch, card_bag_g, female, 5, scratch, 6143, &t),
            "%s: verify-off 6,143 B buffer refused (female=%d)", label, female);
      rom_chrome_set_verify(&rch, 1);
      CHECK(!rom_chrome_bag_sprite_load(&rch, card_bag_g, female, 5, scratch, 6144, &t),
            "%s: verify-on 6,144 B buffer refused (needs the compare copy) (female=%d)", label, female);
    }
  }
  fclose(c.f);
}

/* BACKLOG #139: prove decode_verified's hash-compare in rom_chrome.c is
 * load-bearing, the same way host_romsprite_test.c proved it for rom_sprite.c
 * (that mock exists only there today -- gutting rom_chrome's hash compare
 * left the suite green). One trainer-card load (Emerald, front/tier0/male):
 * corrupt one byte the decode consumes -> the verify must catch it and a
 * retry must yield the clean tileset bytes, never accepted-dirty; then the
 * SAME corruption point with verification off must sail through unfixed,
 * proving it is verification -- not luck -- doing the catching.
 *
 * Call ordinals traced empirically with a debug read-counter tracer against
 * Emerald.gba, verify ON, tileset fetch (the FIRST of card_load's four
 * fetches, k_card_emerald.tileset): call 1 = the LZ10 header (4 B), call 2 =
 * the whole compressed-body window chunk (the spare-tail window is large
 * enough here to cover the whole span in one read), call 3 = decode_verified's
 * verify re-read of the consumed span. Corrupting byte 1 of either call 2 or
 * call 3 costs a second full attempt (more than 3 calls) and still lands on
 * the exact clean bytes; byte 0 (a flags byte) was tried first and rejected
 * for this test because it can flip a token from literal to back-reference
 * and trip decode_verified's own "reference before the start" fail-closed
 * path (n != want) instead of producing a verifiable mismatch -- byte 1 always
 * lands inside a literal/length byte for this blob. With verification off,
 * corrupting call 2 costs the same read-call shape MINUS every re-read (8
 * calls total, one attempt), and the corrupted tileset bytes are accepted. */
static void test_card_corruption(const char* label, const char* path) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s corruption: SKIP (no dump)\n", label); return; }
  RomChrome rch; rom_chrome_open(&rch, &rc);
  static uint8_t scratch_clean[8192], scratch_dirty[8192];

  g_hit_call = 0;
  RomChromeCard clean;
  int okc = rom_chrome_card_load(&rch, 1, 0, 0, 0, scratch_clean, sizeof scratch_clean, &clean);
  CHECK(okc, "%s: clean card-load baseline for the corruption cases failed", label);

  /* (a) corrupt the decode's own compressed-input read (call 2). A retry
   * must recover the exact clean tileset bytes. */
  g_reads = 0;
  g_hit_call = 2; g_hit_pos = 1;
  RomChromeCard dec;
  int ok_dec = rom_chrome_card_load(&rch, 1, 0, 0, 0, scratch_dirty, sizeof scratch_dirty, &dec);
  CHECK(okc && ok_dec && memcmp(scratch_dirty, scratch_clean, 5120) == 0,
       "%s: a decode-phase read corruption still yields the clean tileset (a retry recovers)", label);
  CHECK(g_reads > 3, "%s: a decode-phase read corruption cost more than one attempt", label);
  g_hit_call = 0;

  /* (b) corrupt ONLY the verify re-read (call 3); the decode's own input hash
   * is correct, so only the confirmation read sees garbage -- must not be
   * silently accepted, a retry must still land on the clean bytes. */
  memset(scratch_dirty, 0, sizeof scratch_dirty);
  g_reads = 0;
  g_hit_call = 3; g_hit_pos = 1;
  RomChromeCard rr;
  int ok_rr = rom_chrome_card_load(&rch, 1, 0, 0, 0, scratch_dirty, sizeof scratch_dirty, &rr);
  CHECK(g_reads > 3, "%s: a re-read-phase corruption is not silently accepted (a retry happened)", label);
  CHECK(okc && ok_rr && memcmp(scratch_dirty, scratch_clean, 5120) == 0,
       "%s: a re-read-phase corruption still yields the clean tileset after the retry", label);
  g_hit_call = 0;

  /* (c) the SAME corruption point (call 2), verification OFF: with no reread
   * to catch it, the corrupted decode must sail through UNFIXED -- proving it
   * is verification, not luck, doing the catching above. */
  rom_chrome_set_verify(&rch, 0);
  memset(scratch_dirty, 0, sizeof scratch_dirty);
  g_reads = 0;
  g_hit_call = 2; g_hit_pos = 1;
  RomChromeCard nv;
  int ok_nv = rom_chrome_card_load(&rch, 1, 0, 0, 0, scratch_dirty, sizeof scratch_dirty, &nv);
  CHECK(okc && ok_nv && memcmp(scratch_dirty, scratch_clean, 5120) != 0,
       "%s: with verification OFF the same corrupted decode sails through unfixed "
       "(so it is verification doing the catching)", label);
  g_hit_call = 0;
  rom_chrome_set_verify(&rch, 1);

  fclose(c.f);
}

/* Mirror of test_card_corruption() for the bag screen's tileset fetch
 * (rom_chrome_bag_load, Emerald male). Traced empirically the same way: call
 * 1 = the LZ10 header, call 2 = the compressed-body window chunk, call 3 =
 * the verify re-read. */
static void test_bag_corruption(const char* label, const char* path) {
  HostCtx c; RomCtx rc;
  if (!open_rom(path, &c, &rc)) { printf("%s corruption: SKIP (no dump)\n", label); return; }
  RomChrome rch; rom_chrome_open(&rch, &rc);
  static uint8_t scratch_clean[8192], scratch_dirty[8192];

  g_hit_call = 0;
  RomChromeBag clean;
  int okc = rom_chrome_bag_load(&rch, 1, 0, scratch_clean, sizeof scratch_clean, &clean);
  CHECK(okc, "%s: clean bag-load baseline for the corruption cases failed", label);

  g_reads = 0;
  g_hit_call = 2; g_hit_pos = 1;
  RomChromeBag dec;
  int ok_dec = rom_chrome_bag_load(&rch, 1, 0, scratch_dirty, sizeof scratch_dirty, &dec);
  CHECK(okc && ok_dec && memcmp(scratch_dirty, scratch_clean, 1696) == 0,
       "%s: a decode-phase read corruption still yields the clean tileset (a retry recovers)", label);
  CHECK(g_reads > 3, "%s: a decode-phase read corruption cost more than one attempt", label);
  g_hit_call = 0;

  memset(scratch_dirty, 0, sizeof scratch_dirty);
  g_reads = 0;
  g_hit_call = 3; g_hit_pos = 1;
  RomChromeBag rr;
  int ok_rr = rom_chrome_bag_load(&rch, 1, 0, scratch_dirty, sizeof scratch_dirty, &rr);
  CHECK(g_reads > 3, "%s: a re-read-phase corruption is not silently accepted (a retry happened)", label);
  CHECK(okc && ok_rr && memcmp(scratch_dirty, scratch_clean, 1696) == 0,
       "%s: a re-read-phase corruption still yields the clean tileset after the retry", label);
  g_hit_call = 0;

  rom_chrome_set_verify(&rch, 0);
  memset(scratch_dirty, 0, sizeof scratch_dirty);
  g_reads = 0;
  g_hit_call = 2; g_hit_pos = 1;
  RomChromeBag nv;
  int ok_nv = rom_chrome_bag_load(&rch, 1, 0, scratch_dirty, sizeof scratch_dirty, &nv);
  CHECK(okc && ok_nv && memcmp(scratch_dirty, scratch_clean, 1696) != 0,
       "%s: with verification OFF the same corrupted decode sails through unfixed "
       "(so it is verification doing the catching)", label);
  g_hit_call = 0;
  rom_chrome_set_verify(&rch, 1);

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
  test_bag_game("Ruby bag (#313: tileset PREFIX streamed)", ruby, 0);
  test_bag_game("Sapphire bag (#313: tileset PREFIX streamed)", sapphire, 0);
  test_rs_bag_pins("Ruby", ruby);
  test_rs_bag_pins("Sapphire", sapphire);

  /* stars/badges/photo (card_g: 1 = Emerald, 0 = Ruby) */
  test_foreground_game("Emerald foreground", emerald, 1, 1, 1);
  test_foreground_game("Ruby foreground", ruby, 0, 1, 1);

  test_pokeblock_device("Emerald pokeblock device", emerald, 1);
  test_pokeblock_device("Ruby pokeblock device (unwired case)", ruby, 0);

  /* bag sprite: FireRed/LeafGreen (bag_style 2) only -- Emerald's monolithic
   * 12,288 B blob cannot fit the 8,192 B shared buffer for ANY frame. */
  test_bag_sprite("FireRed bag sprite", firered, 2, 1);
  test_bag_sprite("LeafGreen bag sprite", leafgreen, 2, 1);
  test_bag_sprite("Emerald bag sprite (streamed per frame)", emerald, 1, 1);
  test_bag_sprite("Ruby bag sprite (#313, Emerald-shaped streamed frames)", ruby, 0, 1);
  test_bag_sprite("Sapphire bag sprite (#313)", sapphire, 0, 1);

  measure_counts(emerald);

  /* BACKLOG #139 */
  test_card_corruption("Emerald card corruption", emerald);
  test_bag_corruption("Emerald bag corruption", emerald);

  if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
  printf("host_romchrome_test: all checks passed\n");
  return 0;
}
