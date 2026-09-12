/* Trainer card + Pokeblock case backgrounds, read out of the user's own ROM.
 * See rom_chrome.h for the design, the honest scope (Emerald+Ruby card, Emerald-
 * only Pokeblock, NO bag), and why. */
#include "rom_chrome_gate.h"
#include "rom_chrome.h"

/*
 * rom_chrome_open()/rom_chrome_card_have()/rom_chrome_pokeblock_have()/
 * rom_chrome_set_verify() stay OUTSIDE the PDNA_ROM_CHROME_NEEDED gate below and
 * ALWAYS compile: pdna_main.c's app_icon_rom_open() calls rom_chrome_open() on
 * every ROM registration unconditionally, in BOTH builds (rom_chrome.h's own
 * promise: "Safe to call even in a full-art build"). They are a handful of
 * comparisons and field writes each -- negligible, unlike the LZ77 decode +
 * pin-table weight the *_load() functions below pull in, which is exactly what
 * the gate exists to keep out of a full-art ROM. */
void rom_chrome_open(RomChrome* rch, const RomCtx* rc) {
  rch->rc = rc;
  rch->card_style = -1;
  rch->pokeblock_ok = 0;
  rch->bag_style = -1;
  rch->verify = 1;
  if (!rc) return;
  if (rc->kind == ROM_EMERALD) { rch->card_style = 1; rch->pokeblock_ok = 1; rch->bag_style = 1; }
  else if (rc->kind == ROM_RUBY) { rch->card_style = 0; }
  else if (rc->kind == ROM_FIRERED || rc->kind == ROM_LEAFGREEN) { rch->bag_style = 2; }
  /* Sapphire: nothing wired (see rom_chrome.h's scope note). */
}

void rom_chrome_set_verify(RomChrome* rch, int on) { rch->verify = on ? 1 : 0; }

int rom_chrome_card_have(const RomChrome* rch, int g) {
  return rch && rch->rc && rch->card_style == g;
}

int rom_chrome_pokeblock_have(const RomChrome* rch, int g) {
  (void)g;                       /* Emerald is card_style==1 AND the only pokeblock */
  return rch && rch->rc && rch->pokeblock_ok;
}

int rom_chrome_bag_have(const RomChrome* rch, int g) {
  return rch && rch->rc && rch->bag_style == g;
}

#if PDNA_ROM_CHROME_NEEDED   /* the actual decoders: empty otherwise -- see the gate header */

#include "map_render.h"     /* mr_lz77 -- do not write another LZ77 decoder */

/* ---- per-game pins, EACH individually verified in DESIGN.md ------------------ */

typedef struct {
  uint32_t tileset;  uint8_t tileset_lz;  uint32_t tileset_bytes;   /* 128x80 4bpp */
  uint32_t front;    uint8_t front_lz;
  uint32_t back;     uint8_t back_lz;     uint32_t map_bytes;       /* front/back/bg */
  uint32_t bg;       uint8_t bg_lz;       /* background tilemap UNDER front/back,
                                            * one shared address for both faces --
                                            * palette bank 1 exclusively (measured);
                                            * front/back use bank 0 exclusively, so
                                            * compositing with index-0-transparent
                                            * is exactly what retail draws */
  uint16_t map_w;                                                   /* stored stride */
  uint32_t tier[5];  uint8_t tier_lz;                               /* 96 B each   */
  uint32_t female_bg; uint8_t female_bg_lz;                         /* 32 B, bank1 */
  uint32_t star_pal; uint8_t star_pal_lz;                           /* 32 B, FLAT
                                                                      * (not a bank
                                                                      * -- see
                                                                      * rom_chrome.h) */
} CardPins;

/* Emerald BPEE r0 -- DESIGN.md Sec 1.6. tileset+front+back+bg LZ77; tier/female RAW.
 * star_pal located 2026-08-22 (a RAW 32 B blob, verified by decompress-and-
 * byte-compare against tools/gen_card_bg.py's staged assets/card/emerald/star.pal --
 * DESIGN.md Sec 1.6 already named this address as "star.pal" but nothing wired it). */
static const CardPins k_card_emerald = {
  0x08DD1AB8, 1, 5120u,
  0x08DD2010, 1,
  0x08DD21B0, 1, 1200u,
  0x08DD1F78, 1,
  30,
  { 0x08DD1A58, 0x0856F1AC, 0x0856F26C, 0x0856F32C, 0x0856F3EC }, 0,
  0x0856F4AC, 0,
  0x0856F52C, 0,
};

/* Ruby AXVE r2 -- everything RAW (Sec 1.6: "Ruby | everything RAW"). star_pal
 * located the same way as Emerald's, verified against assets/card/rs/star.pal. */
static const CardPins k_card_ruby = {
  0x08E8B4E0, 0, 5120u,
  0x08E8CAC0, 0,
  0x08E8CFC0, 0, 1280u,
  0x08E8D9C0, 0,
  32,
  { 0x08E8C8E0, 0x08E8C940, 0x08E8C9A0, 0x08E8CA00, 0x08E8CA60 }, 0,
  0x083B5F28, 0,
  0x083B5F68, 0,
};

/* ---- badges (8 gym-badge 16x16 icons) ---------------------------------------
 * Located 2026-08-22 by the same decompress-and-byte-compare method as the
 * card faces themselves, against tools/gen_card_bg.py's staged badges.png
 * (rom_chrome.h's badge_ids() docs the tile-id conventions each game uses). */
typedef struct {
  uint32_t tiles;   uint8_t tiles_lz;  uint32_t tiles_bytes;   /* 128x16 = 32 tiles */
  uint32_t pal;     uint8_t pal_lz;                            /* 32 B, flat        */
} CardBadgePins;

/* Emerald: LZ10 tileset immediately after star.pal's neighbourhood; its own
 * flat palette sits earlier in the SAME 0x20-spaced table as female_bg.pal/
 * star.pal (two byte-identical 32 B candidates were found 0x20 B apart at
 * 0x0856F4EC/0x0856F50C -- picked the lower/first address; either decodes to
 * the SAME bytes, so the rendered pixels are correct regardless of which one
 * is "the" canonical reference). */
static const CardBadgePins k_badge_emerald = { 0x0856F5CC, 1, 1024u, 0x0856F4EC, 0 };
/* Ruby: RAW tileset (matches the rest of Ruby's card assets being RAW), own
 * flat palette also RAW, single unambiguous hit. */
static const CardBadgePins k_badge_ruby    = { 0x083B5AD4, 0, 1024u, 0x083B5F48, 0 };

/* Ruby's badges_map.bin: DESIGN.md Sec 1.6, already located and address-verified
 * (byte size 64 = 8 badges x 4 absolute u16 tile ids). Absolute ids are VRAM tile
 * numbers based at 164 (tools/gen_card_bg.py's own comment) -- subtract 164 to
 * land inside k_badge_ruby's own 32-tile sheet. */
#define RUBY_BADGE_MAP_ADDR 0x083B5FA8u
#define RUBY_BADGE_MAP_BASE 164

/* ---- trainer photo (the gendered 64x64 portrait) ----------------------------
 * Located 2026-08-22, same method, against assets/card/{emerald,rs}/{brendan,may}.png.
 * DESIGN.md's card table never listed this asset at all -- a genuine gap, not a
 * wrong number; tools/gen_card_bg.py's docstring already named the expected
 * shape (pic_xy from CARD_LAYOUTS, embedded per-picture palette for RS/Emerald)
 * without ever pinning ROM addresses. */
typedef struct {
  uint32_t male;    uint8_t male_lz;    uint32_t male_pal;   uint8_t male_pal_lz;
  uint32_t female;  uint8_t female_lz;  uint32_t female_pal; uint8_t female_pal_lz;
} CardPhotoPins;

static const CardPhotoPins k_photo_emerald = {
  0x08D6170C, 1, 0x08D61A30, 1,
  0x08D61A58, 1, 0x08D61D58, 1,
};
static const CardPhotoPins k_photo_ruby = {
  0x08E492B8, 1, 0x08E5A028, 1,
  0x08E495CC, 1, 0x08E5A050, 1,
};

typedef struct {
  uint32_t tileset;  uint8_t tileset_lz;  uint32_t tileset_bytes;   /* 40 tiles    */
  uint32_t tilemap;  uint8_t tilemap_lz;  uint32_t map_bytes;
  uint16_t map_w;
  uint32_t pal;      uint8_t pal_lz;      uint32_t pal_bytes;       /* 6 banks     */
  uint32_t device;    uint8_t device_lz;    uint32_t device_bytes;  /* 64x64, 8x8-tile */
  uint32_t device_pal; uint8_t device_pal_lz;                       /* 32 B, flat  */
} PokeblockPins;

/* Emerald BPEE r0 -- DESIGN.md Sec 1.7. Everything LZ77. device.png/its own
 * palette located 2026-08-22 by decompress-and-byte-compare against
 * tools/gen_pokeblock_bg.py's staged assets/pokeblock/emerald/device.png --
 * DESIGN.md already located device.png itself (matches its address exactly)
 * but never located its palette, which turns out to be its OWN dedicated 32 B
 * blob, not any of menu.pal's 6 banks (checked all six -- none match). */
static const PokeblockPins k_pb_emerald = {
  0x08D9B2B4, 1, 1280u,
  0x08D9B7C8, 1, 2048u,
  32,
  0x08D9B470, 1, 192u,
  0x08D9B4E0, 1, 2048u,
  0x08D9B7A0, 1,
};

/* ---- bounds / read helpers, the same posture rom_itemart.c uses -------------- */

static int ptr_ok(const RomCtx* rc, uint32_t addr, uint32_t need) {
  if (!rc || addr < ROM_BASE) return 0;
  uint32_t off = addr - ROM_BASE;
  return off < rc->size && need <= rc->size - off;
}

static uint32_t hash32(const uint8_t* p, uint32_t n) {
  uint32_t h = 2166136261u;
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

/* Raw n-byte read, verified by repetition when `verify`. */
static int read_verified(const RomCtx* rc, int verify, uint32_t addr, uint8_t* dst, uint32_t n) {
  if (!ptr_ok(rc, addr, n) || !rom_read_at(rc, addr, dst, n)) return 0;
  if (!verify) return 1;
  uint32_t prev = hash32(dst, n);
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!rom_read_at(rc, addr, dst, n)) return 0;
    uint32_t h = hash32(dst, n);
    if (h == prev) return 1;
    prev = h;
  }
  return 0;
}

/* LZ77-decompress addr into dst, requiring exactly `want` bytes out, verified by
 * repetition when `verify`. */
static int decode_verified(const RomCtx* rc, int verify, uint32_t addr, uint8_t* dst,
                           uint32_t cap, uint32_t want) {
  if (want > cap) return 0;
  /* Use the caller's own unused tail dst[want,cap) as the LZ77 input window when
   * it is comfortably large (BACKLOG #103 step 3), instead of the decoder's
   * default 64 B stack window. Correctness: mr_lz77_w only ever WRITES
   * dst[0,size) (size == `want`, checked above) and only ever READS dst[0,out)
   * for back-references with out < size -- see map_render.c's mr_lz77_w body --
   * so dst[want,cap) is provably untouched by the decode this window feeds,
   * and safe to borrow as scratch input space. Every rom_chrome caller of
   * fetch() below fills its blobs at strictly increasing scratch offsets
   * (rom_chrome_card_load: tileset then map then bg then pal, :238-241;
   * rom_chrome_pokeblock_load: tiles then pal, :370-371; rom_chrome_bag_load:
   * tileset then tilemap then pal_m [then pal_f], :487-488), so nothing live
   * occupies that tail region yet at decode time either. */
  uint8_t* win = 0; uint32_t win_bytes = 0;
  if (cap - want >= 256u) { win = dst + want; win_bytes = cap - want; }

  uint32_t n = mr_lz77_w(rc, addr, dst, cap, win, win_bytes);
  if (n != want) return 0;
  if (!verify) return 1;
  uint32_t prev = hash32(dst, n);
  for (int attempt = 0; attempt < 2; attempt++) {
    n = mr_lz77_w(rc, addr, dst, cap, win, win_bytes);
    if (n != want) return 0;
    uint32_t h = hash32(dst, n);
    if (h == prev) return 1;
    prev = h;
  }
  return 0;
}

static int fetch(const RomCtx* rc, int verify, uint32_t addr, int is_lz,
                 uint8_t* dst, uint32_t cap, uint32_t want) {
  if (want > cap) return 0;         /* short buffer FAILS, never a silent partial read */
  return is_lz ? decode_verified(rc, verify, addr, dst, cap, want)
               : read_verified(rc, verify, addr, dst, want);
}

/* 16 raw little-endian RGB15 entries -> a palette bank. */
static void pal_bank_from_raw(const uint8_t* raw, uint16_t out[16]) {
  for (int i = 0; i < 16; i++)
    out[i] = (uint16_t)(raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
}

/* ---- card ------------------------------------------------------------------ */

int rom_chrome_card_load(const RomChrome* rch, int g, int back, int tier, int female,
                         uint8_t* scratch, uint32_t cap, RomChromeCard* out) {
  if (!rom_chrome_card_have(rch, g) || !scratch || !out) return 0;
  const CardPins* p = (g == 1) ? &k_card_emerald : &k_card_ruby;
  if (tier < 0) tier = 0;
  if (tier > 4) tier = 4;

  uint32_t tileset_off = 0;
  uint32_t map_off = (tileset_off + p->tileset_bytes + 1u) & ~1u;   /* 2-aligned */
  uint32_t bg_off = map_off + p->map_bytes;                         /* same size as
                                                                      * front/back */
  uint32_t pal_off = bg_off + p->map_bytes;                         /* 3 banks = 96 B */
  uint32_t need = pal_off + 96u;
  if (need > cap) return 0;

  if (!fetch(rch->rc, rch->verify, p->tileset, p->tileset_lz,
            scratch + tileset_off, cap - tileset_off, p->tileset_bytes)) return 0;
  uint32_t face_addr = back ? p->back : p->front;
  int face_lz = back ? p->back_lz : p->front_lz;
  if (!fetch(rch->rc, rch->verify, face_addr, face_lz,
            scratch + map_off, cap - map_off, p->map_bytes)) return 0;
  /* Background layer, shared by front and back -- composited underneath in
   * romchrome_blit() wherever the face's own tilemap has index 0. Same tile
   * grid size as front/back (DESIGN.md: "bg.bin/front.bin/back.bin ... 1,200 B
   * each" for Emerald; Ruby's three faces are likewise all 1,280 B). */
  if (!fetch(rch->rc, rch->verify, p->bg, p->bg_lz,
            scratch + bg_off, cap - bg_off, p->map_bytes)) return 0;
  /* Tier palette: 96 B = 3 banks (0..2). Female overwrites bank 1 with female_bg. */
  if (!fetch(rch->rc, rch->verify, p->tier[tier], p->tier_lz,
            scratch + pal_off, cap - pal_off, 96u)) return 0;

  out->src.tiles = scratch + tileset_off;
  out->src.map = (const uint16_t*)(const void*)(scratch + map_off);
  out->src.bg_map = (const uint16_t*)(const void*)(scratch + bg_off);
  out->src.map_w = p->map_w;
  {
    const uint8_t* raw = scratch + pal_off;
    pal_bank_from_raw(raw + 0, &out->src.pal[0]);
    pal_bank_from_raw(raw + 32, &out->src.pal[16]);
    pal_bank_from_raw(raw + 64, &out->src.pal[32]);
    for (int b = 3; b < 6; b++) for (int i = 0; i < 16; i++) out->src.pal[b * 16 + i] = 0;
  }
  if (female) {
    uint8_t fb[32];
    if (fetch(rch->rc, rch->verify, p->female_bg, p->female_bg_lz, fb, sizeof fb, 32u))
      pal_bank_from_raw(fb, &out->src.pal[16]);
    /* a failed female-palette fetch just keeps the tier's own bank 1 -- the
     * male border colour on a female card, never a corrupt or unreadable one.
     * Bank 1 is not cosmetic trim: bg.bin's tilemap (out->src.bg_map above)
     * uses palette bank 1 EXCLUSIVELY (measured), so this swap is the whole
     * male/female border-colour distinction retail draws. */
  }
  {
    uint8_t sp[32];
    if (fetch(rch->rc, rch->verify, p->star_pal, p->star_pal_lz, sp, sizeof sp, 32u))
      pal_bank_from_raw(sp, out->star_pal);
    else
      for (int i = 0; i < 16; i++) out->star_pal[i] = 0;   /* fail-safe: no stars drawn */
  }
  out->as_lzblob.base = 0; out->as_lzblob.pages = 0;
  out->as_lzblob.npages = 0; out->as_lzblob.raw_len = 0;
  out->as_lzblob.romsrc = &out->src;
  return 1;
}

/* ---- card badges -------------------------------------------------------- */

int rom_chrome_card_badges_have(const RomChrome* rch, int g) {
  return rom_chrome_card_have(rch, g);   /* same two games as the card faces */
}

int rom_chrome_card_badges_load(const RomChrome* rch, int g,
                                uint8_t* scratch, uint32_t cap, RomChromeCardBadges* out) {
  if (!rom_chrome_card_badges_have(rch, g) || !scratch || !out) return 0;
  const CardBadgePins* p = (g == 1) ? &k_badge_emerald : &k_badge_ruby;
  uint32_t pal_off = (p->tiles_bytes + 1u) & ~1u;
  uint32_t need = pal_off + 32u;
  if (need > cap) return 0;
  if (!fetch(rch->rc, rch->verify, p->tiles, p->tiles_lz, scratch, cap, p->tiles_bytes))
    return 0;
  uint8_t pb[32];
  if (!fetch(rch->rc, rch->verify, p->pal, p->pal_lz, pb, sizeof pb, 32u)) return 0;
  out->tiles = scratch;
  pal_bank_from_raw(pb, out->pal);
  out->pal[0] = 0;   /* index 0 is transparent, never drawn -- keep it inert */
  return 1;
}

int rom_chrome_card_badge_ids(const RomChrome* rch, int g, int i, int16_t ids[4]) {
  if (!rom_chrome_card_badges_have(rch, g) || i < 0 || i > 7 || !ids) return 0;
  if (g == 1) {                              /* Emerald: plain 2x2 strip, 16 tiles/row */
    ids[0] = (int16_t)(2 * i);      ids[1] = (int16_t)(2 * i + 1);
    ids[2] = (int16_t)(16 + 2 * i); ids[3] = (int16_t)(17 + 2 * i);
    return 1;
  }
  /* Ruby: badges_map.bin, 8 rows x 4 absolute VRAM tile ids, based at 164. */
  uint8_t raw[64];
  if (!fetch(rch->rc, rch->verify, RUBY_BADGE_MAP_ADDR, 0, raw, sizeof raw, 64u)) return 0;
  for (int k = 0; k < 4; k++) {
    uint32_t o = (uint32_t)(i * 4 + k) * 2u;
    uint16_t e = (uint16_t)(raw[o] | ((uint16_t)raw[o + 1] << 8));
    int32_t tid = (int32_t)(e & 0x3FFu) - RUBY_BADGE_MAP_BASE;
    ids[k] = (tid >= 0 && tid < 32) ? (int16_t)tid : (int16_t)-1;
  }
  return 1;
}

/* ---- card trainer photo -------------------------------------------------- */

int rom_chrome_card_photo_have(const RomChrome* rch, int g) {
  return rom_chrome_card_have(rch, g);   /* same two games as the card faces */
}

int rom_chrome_card_photo_load(const RomChrome* rch, int g, int female,
                               uint8_t* scratch, uint32_t cap, RomChromeCardPhoto* out) {
  if (!rom_chrome_card_photo_have(rch, g) || !scratch || !out) return 0;
  const CardPhotoPins* p = (g == 1) ? &k_photo_emerald : &k_photo_ruby;
  uint32_t addr = female ? p->female : p->male, pal = female ? p->female_pal : p->male_pal;
  uint8_t addr_lz = female ? p->female_lz : p->male_lz;
  uint8_t pal_lz = female ? p->female_pal_lz : p->male_pal_lz;
  uint32_t pal_off = (2048u + 1u) & ~1u;
  uint32_t need = pal_off + 32u;
  if (need > cap) return 0;
  if (!fetch(rch->rc, rch->verify, addr, addr_lz, scratch, cap, 2048u)) return 0;
  uint8_t pb[32];
  if (!fetch(rch->rc, rch->verify, pal, pal_lz, pb, sizeof pb, 32u)) return 0;
  out->tiles = scratch;
  pal_bank_from_raw(pb, out->pal);
  out->pal[0] = 0;
  return 1;
}

/* ---- pokeblock -------------------------------------------------------------- */

int rom_chrome_pokeblock_load(const RomChrome* rch, int g,
                              uint8_t* scratch, uint32_t cap, RomChromePokeblock* out) {
  if (!rom_chrome_pokeblock_have(rch, g) || !scratch || !out) return 0;
  const PokeblockPins* p = &k_pb_emerald;

  uint32_t tileset_off = 0;
  uint32_t map_off = (tileset_off + p->tileset_bytes + 1u) & ~1u;
  uint32_t pal_off = map_off + p->map_bytes;
  uint32_t dev_off = pal_off + p->pal_bytes;
  uint32_t devpal_off = dev_off + p->device_bytes;
  uint32_t need = devpal_off + 32u;
  if (need > cap) return 0;

  if (!fetch(rch->rc, rch->verify, p->tileset, p->tileset_lz,
            scratch + tileset_off, cap - tileset_off, p->tileset_bytes)) return 0;
  if (!fetch(rch->rc, rch->verify, p->tilemap, p->tilemap_lz,
            scratch + map_off, cap - map_off, p->map_bytes)) return 0;
  if (!fetch(rch->rc, rch->verify, p->pal, p->pal_lz,
            scratch + pal_off, cap - pal_off, p->pal_bytes)) return 0;

  out->src.tiles = scratch + tileset_off;
  out->src.map = (const uint16_t*)(const void*)(scratch + map_off);
  out->src.bg_map = 0;   /* no background layer for the Pokeblock case */
  out->src.map_w = p->map_w;
  {
    const uint8_t* raw = scratch + pal_off;
    int banks = (int)(p->pal_bytes / 32u); if (banks > 6) banks = 6;
    for (int b = 0; b < banks; b++) pal_bank_from_raw(raw + b * 32, &out->src.pal[b * 16]);
    for (int b = banks; b < 6; b++) for (int i = 0; i < 16; i++) out->src.pal[b * 16 + i] = 0;
  }
  /* device.png + its own palette -- a failed fetch degrades to "no device
   * drawn" (device_tiles stays 0), never a corrupt or unreadable screen; the
   * case chrome itself (already fetched above) is unaffected. */
  if (fetch(rch->rc, rch->verify, p->device, p->device_lz,
           scratch + dev_off, cap - dev_off, p->device_bytes)) {
    uint8_t dp[32];
    if (fetch(rch->rc, rch->verify, p->device_pal, p->device_pal_lz, dp, sizeof dp, 32u)) {
      out->device_tiles = scratch + dev_off;
      pal_bank_from_raw(dp, out->device_pal);
      out->device_pal[0] = 0;
    } else out->device_tiles = 0;
  } else out->device_tiles = 0;
  (void)devpal_off;
  out->as_lzblob.base = 0; out->as_lzblob.pages = 0;
  out->as_lzblob.npages = 0; out->as_lzblob.raw_len = 0;
  out->as_lzblob.romsrc = &out->src;
  return 1;
}

/* ---- bag ---------------------------------------------------------------- */

typedef struct {
  uint32_t tileset;   uint8_t tileset_lz;  uint32_t tileset_bytes;
  uint32_t tilemap;   uint8_t tilemap_lz;  uint32_t map_bytes;
  uint16_t map_w;
  uint32_t pal_m;      uint8_t pal_m_lz;    uint8_t pal_m_banks;  /* male/base palette */
  uint32_t pal_f;      uint8_t pal_f_lz;    uint8_t pal_f_banks;  /* female palette --
                                              * Emerald: a WHOLE separate blob (pal_f_at
                                              * 0, pal_f_banks == pal_m_banks, full
                                              * replace); FRLG: a partial override
                                              * (pal_f_at 0, pal_f_banks 1 -- bank 0
                                              * only, matching tools/gen_bag_bg.py's
                                              * own `# bank 0 only`) */
  uint8_t  pal_f_at;
} BagPins;

/* Emerald BPEE r0. Tileset is the LZ10 blob immediately before the known
 * tilemap pointer (menu.bin, referenced from 0x081AB134); male/female
 * palettes are two COMPLETE independent 32-colour blobs, not a bank override
 * -- rom_chrome.h's header comment records the code-reference evidence. */
static const BagPins k_bag_emerald = {
  0x08D9A620, 1, 1696u,
  0x08D9A88C, 1, 2048u,
  32,
  0x08D9A588, 1, 2,
  0x08D9A5D4, 1, 2, 0,
};

/* FireRed BPRE r1. A 3-bank palette; the female recolour is a BANK-0 override
 * only (tools/gen_bag_bg.py:118, `female = load_jasc_pal(bg_female.pal) +
 * male[16:]  # bank 0 only`, citing pokefirered src/item_menu.c:574), NOT the
 * bank-1 override an earlier pass tried. MEASURED against Guy's own female
 * FireRed.sav: the pal_f blob at 0x08E83604 (LZ10, 16 RGB15 entries) is
 * BYTE-IDENTICAL to assets/bag/frlg/bg_female.pal, and applying it to bank 0
 * (pal_f_at 0) reproduces the compiled full-art capture of that exact save
 * to the GBA's own RGB15 rounding -- see rom_chrome.h's header comment for
 * the pixel-diff numbers and how the earlier bank-1 attempt was misjudged. */
static const BagPins k_bag_firered = {
  0x08E830CC, 1, 1760u,
  0x08E832C0, 1, 2048u,
  32,
  0x08E835B4, 1, 3,
  0x08E83604, 1, 1, 0,
};

/* LeafGreen BPGE r0 -- the FireRed set, shifted; found independently by the
 * same code-reference scan, not assumed from FireRed's addresses. Same
 * bank-0-only female override, same measured-byte-identical posture as
 * FireRed above. */
static const BagPins k_bag_leafgreen = {
  0x08E8314C, 1, 1760u,
  0x08E83340, 1, 2048u,
  32,
  0x08E83634, 1, 3,
  0x08E83684, 1, 1, 0,
};

static const BagPins* bag_pins_for(RomKind kind) {
  switch (kind) {
    case ROM_EMERALD:   return &k_bag_emerald;
    case ROM_FIRERED:   return &k_bag_firered;
    case ROM_LEAFGREEN: return &k_bag_leafgreen;
    default:             return 0;
  }
}

int rom_chrome_bag_load(const RomChrome* rch, int g, int female,
                        uint8_t* scratch, uint32_t cap, RomChromeBag* out) {
  if (!rom_chrome_bag_have(rch, g) || !scratch || !out) return 0;
  const BagPins* p = bag_pins_for(rch->rc->kind);
  if (!p) return 0;   /* rom_chrome_open() promised bag_style only for pinned kinds */

  uint32_t tileset_off = 0;
  uint32_t map_off = (tileset_off + p->tileset_bytes + 1u) & ~1u;   /* 2-aligned */
  uint32_t pal_off = map_off + p->map_bytes;
  uint32_t pal_bytes = (uint32_t)p->pal_m_banks * 32u;
  uint32_t need = pal_off + pal_bytes;
  if (need > cap) return 0;

  if (!fetch(rch->rc, rch->verify, p->tileset, p->tileset_lz,
            scratch + tileset_off, cap - tileset_off, p->tileset_bytes)) return 0;
  if (!fetch(rch->rc, rch->verify, p->tilemap, p->tilemap_lz,
            scratch + map_off, cap - map_off, p->map_bytes)) return 0;
  if (!fetch(rch->rc, rch->verify, p->pal_m, p->pal_m_lz,
            scratch + pal_off, cap - pal_off, pal_bytes)) return 0;

  out->src.tiles = scratch + tileset_off;
  out->src.map = (const uint16_t*)(const void*)(scratch + map_off);
  out->src.bg_map = 0;   /* single-layer screen -- no background layer under the bag */
  out->src.map_w = p->map_w;
  {
    const uint8_t* raw = scratch + pal_off;
    int banks = (int)p->pal_m_banks; if (banks > 6) banks = 6;
    for (int b = 0; b < banks; b++) pal_bank_from_raw(raw + b * 32, &out->src.pal[b * 16]);
    for (int b = banks; b < 6; b++) for (int i = 0; i < 16; i++) out->src.pal[b * 16 + i] = 0;
  }
  if (female && p->pal_f_banks) {
    uint8_t fb[3 * 32];                              /* worst case 3 banks, 96 B -- stack,
                                                        * not scratch: never overlaps the
                                                        * caller's buffer */
    uint32_t fbytes = (uint32_t)p->pal_f_banks * 32u;
    if (fbytes <= sizeof fb &&
        fetch(rch->rc, rch->verify, p->pal_f, p->pal_f_lz, fb, sizeof fb, fbytes)) {
      for (int b = 0; b < p->pal_f_banks && (p->pal_f_at + b) < 6; b++)
        pal_bank_from_raw(fb + b * 32, &out->src.pal[(p->pal_f_at + b) * 16]);
    }
    /* a failed female fetch just keeps the male/base banks -- the male colours
     * on a female bag, never a corrupt or unreadable one (same posture as the
     * card's female_bg fetch above). */
  }
  out->as_lzblob.base = 0; out->as_lzblob.pages = 0;
  out->as_lzblob.npages = 0; out->as_lzblob.raw_len = 0;
  out->as_lzblob.romsrc = &out->src;
  return 1;
}

/* ---- bag sprite (the drawn bag itself) ----------------------------------- *
 * See rom_chrome.h's header comment for why this is FireRed/LeafGreen only:
 * both games' sheet is ONE monolithic LZ10 blob (declared size 8,192 B,
 * confirmed by reading the header at each pinned address), which just fits
 * the shared 8,192 B buffer alone; Emerald's is 12,288 B, bigger than the
 * WHOLE buffer, for every frame including the closed one. */
typedef struct {
  uint32_t male;    uint8_t male_lz;
  uint32_t female;  uint8_t female_lz;
  uint32_t pal;     uint8_t pal_lz;      /* the sheet's OWN palette -- a
                                           * DIFFERENT 32 B blob from the
                                           * screen's own menu/bg palette
                                           * (BagPins.pal_m/pal_f above) */
  uint8_t  frame_count;
} BagSpritePins;

#define BAG_SPRITE_BYTES 8192u   /* both games: 4 frames x 2,048 B (64x64 4bpp) */

/* FireRed BPRE r1. sheet_pal located 2026-08-22: BYTE-IDENTICAL to
 * DESIGN.md's already-located "bag.pal" (0x08E84560) -- that address was
 * dismissed in an earlier pass as "item-icon plumbing, not bag chrome"
 * (rom_chrome.h's header comment on the SCREEN's own palette). That dismissal
 * was right for the screen but wrong as a general conclusion: bag.pal is
 * genuinely unrelated to the screen's tilemap, but it IS the SPRITE sheet's
 * own palette (tools/gen_bag_bg.py's sheet_pal="bag.pal" -- exactly this
 * asset, just never wired because nothing decoded the sprite itself before
 * now). Worth recording so the next person doesn't re-dismiss it. */
static const BagSpritePins k_bagspr_firered = {
  0x08E8362C, 1, 0x08E83DBC, 1, 0x08E84560, 1, 4,
};
/* LeafGreen BPGE r0 -- the FireRed set, shifted; sheet + palette each found
 * independently by the same byte-compare, not assumed from FireRed. */
static const BagSpritePins k_bagspr_leafgreen = {
  0x08E836AC, 1, 0x08E83E3C, 1, 0x08E845E0, 1, 4,
};

int rom_chrome_bag_sprite_have(const RomChrome* rch, int g) {
  if (!rom_chrome_bag_have(rch, g)) return 0;
  return rch->rc->kind == ROM_FIRERED || rch->rc->kind == ROM_LEAFGREEN;
}

int rom_chrome_bag_sprite_load(const RomChrome* rch, int g, int female,
                               uint8_t* scratch, uint32_t cap, RomChromeBagSprite* out) {
  if (!rom_chrome_bag_sprite_have(rch, g) || !scratch || !out) return 0;
  const BagSpritePins* p = (rch->rc->kind == ROM_FIRERED) ? &k_bagspr_firered
                                                          : &k_bagspr_leafgreen;
  /* The 8,192 B sheet ALONE exactly fills the shared buffer -- its palette
   * (32 B) is fetched to a STACK buffer below, never appended to `scratch`
   * (there is no room left: 8,192 + 32 > 8,192). Same shape as the card's
   * star_pal / photo's own palette / pokeblock's device_pal fetches above. */
  if (BAG_SPRITE_BYTES > cap) return 0;
  uint32_t addr = female ? p->female : p->male;
  uint8_t addr_lz = female ? p->female_lz : p->male_lz;
  if (!fetch(rch->rc, rch->verify, addr, addr_lz, scratch, cap, BAG_SPRITE_BYTES)) return 0;
  uint8_t pb[32];
  if (!fetch(rch->rc, rch->verify, p->pal, p->pal_lz, pb, sizeof pb, 32u)) return 0;
  out->tiles = scratch;
  pal_bank_from_raw(pb, out->pal);
  out->pal[0] = 0;
  out->frame_count = p->frame_count;
  return 1;
}

#endif /* PDNA_ROM_CHROME_NEEDED */
