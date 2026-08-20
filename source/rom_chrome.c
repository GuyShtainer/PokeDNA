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
} CardPins;

/* Emerald BPEE r0 -- DESIGN.md Sec 1.6. tileset+front+back+bg LZ77; tier/female RAW. */
static const CardPins k_card_emerald = {
  0x08DD1AB8, 1, 5120u,
  0x08DD2010, 1,
  0x08DD21B0, 1, 1200u,
  0x08DD1F78, 1,
  30,
  { 0x08DD1A58, 0x0856F1AC, 0x0856F26C, 0x0856F32C, 0x0856F3EC }, 0,
  0x0856F4AC, 0,
};

/* Ruby AXVE r2 -- everything RAW (Sec 1.6: "Ruby | everything RAW"). */
static const CardPins k_card_ruby = {
  0x08E8B4E0, 0, 5120u,
  0x08E8CAC0, 0,
  0x08E8CFC0, 0, 1280u,
  0x08E8D9C0, 0,
  32,
  { 0x08E8C8E0, 0x08E8C940, 0x08E8C9A0, 0x08E8CA00, 0x08E8CA60 }, 0,
  0x083B5F28, 0,
};

typedef struct {
  uint32_t tileset;  uint8_t tileset_lz;  uint32_t tileset_bytes;   /* 40 tiles    */
  uint32_t tilemap;  uint8_t tilemap_lz;  uint32_t map_bytes;
  uint16_t map_w;
  uint32_t pal;      uint8_t pal_lz;      uint32_t pal_bytes;       /* 6 banks     */
} PokeblockPins;

/* Emerald BPEE r0 -- DESIGN.md Sec 1.7. Everything LZ77. */
static const PokeblockPins k_pb_emerald = {
  0x08D9B2B4, 1, 1280u,
  0x08D9B7C8, 1, 2048u,
  32,
  0x08D9B470, 1, 192u,
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
  uint32_t n = mr_lz77(rc, addr, dst, cap);
  if (n != want) return 0;
  if (!verify) return 1;
  uint32_t prev = hash32(dst, n);
  for (int attempt = 0; attempt < 2; attempt++) {
    n = mr_lz77(rc, addr, dst, cap);
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
  out->as_lzblob.base = 0; out->as_lzblob.pages = 0;
  out->as_lzblob.npages = 0; out->as_lzblob.raw_len = 0;
  out->as_lzblob.romsrc = &out->src;
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
  uint32_t need = pal_off + p->pal_bytes;
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

#endif /* PDNA_ROM_CHROME_NEEDED */
