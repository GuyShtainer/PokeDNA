/* Item icons + type badges out of the user's own ROM — see rom_itemart.h for the
 * design, the measurements behind every pin, and the memory rules. */
#include "rom_itemart.h"

#include <string.h>
#include "map_render.h"   /* mr_lz77 — the streaming LZ77; do not write another */

/* ---- per-revision pinned addresses ------------------------------------------
 *
 * Neither table is reachable from the GF header, so every row here was located by
 * an independent SHAPE SCAN of Guy's own dump (the method is reproduced verbatim in
 * tests/host_romitemart_test.c, which re-derives each address and asserts it equals
 * the pin — so these numbers are checked by a test, not trusted):
 *
 *   gItemIconTable  the unique maximal run of 8-byte { pic, pal } rows where pic
 *                   points at an LZ blob of exactly 0x120 B and pal at one of
 *                   exactly 0x20 B, with >= 300 real rows. Exactly one such run
 *                   exists in Emerald/FireRed/LeafGreen (Emerald has one 43-row
 *                   decoy, which the >= 300 rule discards) and NONE in Ruby or
 *                   Sapphire at any threshold, because R/S have no item icons.
 *   gMoveTypes_Gfx  the unique LZ blob whose uncompressed size is exactly 0x1700,
 *                   immediately followed (next 4-aligned address) by an LZ blob of
 *                   exactly 0x60. Exactly one per RSE cart; none in FR/LG.
 *   gMenuInfoElements_Gfx  FR/LG only: found through the ROM's own code — the
 *                   96-byte sMenuInfoIcons table (list_menu.c:52-77) appears once
 *                   per image, and the literal pool entry next to the reference to
 *                   it is the sheet. Confirmed by rendering all 18 badges.
 *
 * MISSING ON PURPOSE: BPRE rev 0, BPGE rev 1, AXVE rev 0/1, AXPE rev 0/2. There is
 * no dump of those here, and rom_map.c's precedent is that an address nobody
 * verified is worse than no feature — so they fail closed. Adding one is mechanical:
 * run the host test's scan against the dump and paste the row.
 *
 * item_count is ITEMS_COUNT for that game (349 pokeruby / 375 pokefirered / 377
 * pokeemerald); the table itself is ITEMS_COUNT + 1 rows, the extra row being the
 * bag's "return to field" arrow. R/S carry an item_count only so the struct reads
 * honestly — `items` is 0 there and nothing is served. */
typedef struct {
  const char* code;        /* 4-char game code at 0xAC     */
  uint8_t     version;     /* revision byte at 0xBC        */
  uint16_t    item_count;  /* ITEMS_COUNT                  */
  uint32_t    items;       /* gItemIconTable, 0 = none     */
  uint8_t     type_style;  /* RomTypeStyle                 */
  uint32_t    type_gfx;
  uint32_t    type_pal;
} RomItemArtPins;

static const RomItemArtPins k_pins[] = {
  /*  code   rev  items  gItemIconTable  style              type gfx      type pal   */
  { "BPEE", 0, 377, 0x08614410, ROM_TYPEART_RSE,  0x08D971B0, 0x08D97B84 },
  { "BPRE", 1, 375, 0x083D4304, ROM_TYPEART_FRLG, 0x08E95DDC, 0x08E95DBC },
  { "BPGE", 0, 375, 0x083D40D0, ROM_TYPEART_FRLG, 0x08E95E5C, 0x08E95E3C },
  /* Ruby/Sapphire: badges yes, item icons never (the games have none at all). The
   * two carts really do hold the sheet at the same address, and the 2,513 compressed
   * bytes there are byte-identical between them — measured, not copy-pasted. */
  { "AXVE", 2, 349, 0,          ROM_TYPEART_RSE,  0x08E71D10, 0x08E726E4 },
  { "AXPE", 1, 349, 0,          ROM_TYPEART_RSE,  0x08E71D10, 0x08E726E4 },
};
#define K_NPINS ((int)(sizeof k_pins / sizeof k_pins[0]))

/* Which of gMoveTypes_Pal's three 16-colour palettes each RSE badge uses.
 * sMoveTypeToOamPaletteNum (pokeemerald src/pokemon_summary_screen.c:907-931) stores
 * OBJ palette slots 13/14/15 and the blob is loaded at slot 13, so this is that
 * table minus 13. It is NOT derivable from the type id — FIRE and WATER are
 * adjacent ids on different palettes. */
static const uint8_t k_rse_pal[ROM_TYPE_BADGES_RSE] = {
  0, 0, 1, 1, 0, 0, 2, 1, 0, 2, 0, 1, 2, 0, 1, 1, 2, 0,   /* the 18 types      */
  0, 1, 1, 2, 0,                                          /* contest categories */
};

/* FR/LG: the TILE offset of each type's badge inside the 128x128 menu-info sheet,
 * indexed by Gen-3 internal type id. Transcribed from sMenuInfoIcons
 * (pokefirered src/list_menu.c:53-71), whose rows are [TYPE_x + 1] = {32, 12, off}. */
static const uint8_t k_frlg_off[ROM_TYPE_TYPES] = {
  0x20, 0x64, 0x60, 0x80, 0x48, 0x44, 0x6C, 0x68, 0x88,   /* NORMAL..STEEL      */
  0xA4,                                                   /* 9 = ??? / MYSTERY  */
  0x24, 0x28, 0x2C, 0x40, 0x84, 0x4C, 0xA0, 0x8C,         /* FIRE..DARK         */
};
#define FRLG_SHEET_TILES_PER_ROW  16     /* 128 px / 8                            */
#define BADGE_TILES_W             4      /* 32 px / 8                             */
#define ITEM_TILES_W              3      /* 24 px / 8                             */
#define TILE_BYTES                32     /* one 8x8 4bpp tile                     */
#define ROW_STRIDE                8      /* { const u32* pic; const u32* pal }     */

static uint32_t rd32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A ROM pointer is sane if it lands inside the image with `need` bytes of room. */
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

/* Read n raw bytes, verifying by repetition when ra->verify is set. Returns 1, or 0
 * with dst's contents undefined. */
static int read_verified(const RomItemArt* ra, uint32_t addr, uint8_t* dst, uint32_t n) {
  if (!rom_read_at(ra->rc, addr, dst, n)) return 0;
  if (!ra->verify) return 1;
  uint32_t prev = hash32(dst, n);
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!rom_read_at(ra->rc, addr, dst, n)) return 0;
    uint32_t h = hash32(dst, n);
    if (h == prev) return 1;                    /* two reads agreed */
    prev = h;
  }
  return 0;                                     /* three tries, never twice the same */
}

/* Decompress addr into dst and require exactly `want` bytes, verifying by repetition
 * when ra->verify is set. Returns 1, or 0 with dst's contents undefined. */
static int decode_verified(const RomItemArt* ra, uint32_t addr, uint8_t* dst,
                           uint32_t cap, uint32_t want) {
  if (want > cap) return 0;
  uint32_t n = mr_lz77(ra->rc, addr, dst, cap);
  if (n != want) return 0;
  if (!ra->verify) return 1;
  uint32_t prev = hash32(dst, n);
  for (int attempt = 0; attempt < 2; attempt++) {
    n = mr_lz77(ra->rc, addr, dst, cap);
    if (n != want) return 0;
    uint32_t h = hash32(dst, n);
    if (h == prev) return 1;
    prev = h;
  }
  return 0;
}

/* The LZ10 header's advertised uncompressed size, or 0 if there is no header there.
 * Used for the cheap open-time probes; the real decode re-reads and re-checks. */
static uint32_t lz_size_at(const RomItemArt* ra, uint32_t addr) {
  uint8_t h[4];
  if (!ptr_ok(ra->rc, addr, 4)) return 0;
  if (!rom_read_at(ra->rc, addr, h, 4)) return 0;
  if (h[0] != 0x10) return 0;
  return (uint32_t)h[1] | ((uint32_t)h[2] << 8) | ((uint32_t)h[3] << 16);
}

/* 16 raw little-endian RGB15 entries -> a palette. */
static void pal_from_raw(const uint8_t* raw, uint16_t out[16]) {
  for (int i = 0; i < 16; i++)
    out[i] = (uint16_t)(raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
}

/*
 * 4bpp GBA tiles -> RGB15 pixels, the format ui_sprite() draws: index 0 becomes
 * 0x0000 (transparent) and every other index 0x8000 | rgb15.
 *
 * `tiles` is a 1D run of 8x8 tiles laid out left-to-right then top-to-bottom,
 * `tiles_w` tiles per row — the GBA's "1D object mapping" order, which is how both
 * an item icon (3x3) and an RSE badge (4x2) are stored. A FRLG badge is staged into
 * the same shape first, so this one routine covers all three.
 */
static void tiles_to_rgb15(const uint8_t* tiles, int tiles_w, int w, int h,
                           const uint16_t pal[16], uint16_t* dst) {
  for (int y = 0; y < h; y++) {
    const uint8_t* trow = tiles + (y >> 3) * tiles_w * TILE_BYTES + (y & 7) * 4;
    uint16_t* out = dst + (uint32_t)y * w;
    for (int x = 0; x < w; x++) {
      const uint8_t* t = trow + (x >> 3) * TILE_BYTES;
      uint8_t b = t[(x & 7) >> 1];
      uint8_t idx = (uint8_t)((x & 1) ? (b >> 4) : (b & 0x0F));
      out[x] = idx ? (uint16_t)(0x8000u | (pal[idx] & 0x7FFFu)) : 0u;
    }
  }
}

/*
 * FR/LG: copy the 8 tiles a 32x12 badge occupies out of the raw 128x128 sheet into a
 * 4x2 1D block. The badge starts at tile `off`, so it spans tiles off..off+3 on one
 * sheet row and off+16..off+19 on the next — two contiguous 128 B runs, which is
 * also why only 256 B ever leave the ROM for a badge.
 */
static int frlg_stage_badge(const RomItemArt* ra, uint8_t off, uint8_t dst[8 * TILE_BYTES]) {
  for (int row = 0; row < 2; row++) {
    uint32_t tile = (uint32_t)off + (uint32_t)row * FRLG_SHEET_TILES_PER_ROW;
    uint32_t addr = ra->type_gfx + tile * TILE_BYTES;
    if (!ptr_ok(ra->rc, addr, BADGE_TILES_W * TILE_BYTES)) return 0;
    if (!read_verified(ra, addr, dst + row * BADGE_TILES_W * TILE_BYTES,
                       BADGE_TILES_W * TILE_BYTES)) return 0;
  }
  return 1;
}

/* One 4bpp pixel out of a staged 4x2 tile block. */
static uint8_t block_px(const uint8_t* tiles, int tiles_w, int x, int y) {
  const uint8_t* t = tiles + ((y >> 3) * tiles_w + (x >> 3)) * TILE_BYTES + (y & 7) * 4;
  uint8_t b = t[(x & 7) >> 1];
  return (uint8_t)((x & 1) ? (b >> 4) : (b & 0x0F));
}

/* ---- open ------------------------------------------------------------------ */

/* Probe one gItemIconTable row. `full` also decompresses both blobs; otherwise only
 * the LZ headers are read, which keeps open() to a handful of 4-byte reads. */
static int probe_item_row(const RomItemArt* ra, uint16_t row, int full) {
  uint8_t e[ROW_STRIDE];
  uint32_t at = ra->items + (uint32_t)row * ROW_STRIDE;
  if (!ptr_ok(ra->rc, at, ROW_STRIDE)) return 0;
  if (!rom_read_at(ra->rc, at, e, ROW_STRIDE)) return 0;
  uint32_t pic = rd32le(e), pal = rd32le(e + 4);
  if (!pic || !pal) return 0;                 /* a probe row must be a real pair */
  if (lz_size_at(ra, pic) != ROM_ITEM_ICON_BYTES) return 0;
  if (lz_size_at(ra, pal) != ROM_ITEM_PAL_BYTES) return 0;
  if (!full) return 1;
  uint8_t px[ROM_ITEM_ICON_BYTES], pr[ROM_ITEM_PAL_BYTES];
  if (!decode_verified(ra, pic, px, sizeof px, ROM_ITEM_ICON_BYTES)) return 0;
  if (!decode_verified(ra, pal, pr, sizeof pr, ROM_ITEM_PAL_BYTES)) return 0;
  return 1;
}

static void validate_items(RomItemArt* ra) {
  if (!ra->items || !ra->item_count) { ra->items = 0; ra->item_count = 0; return; }
  /* The whole ITEMS_COUNT+1 extent must fit — a table that is right at row 1 but runs
   * off the end of the image is a kind that "works" until the user scrolls. This is a
   * cheap early-out that costs no reads; probing the LAST row below enforces the same
   * extent again, so removing it would not create a hole (a mutation test confirms
   * that), it would just make an absurd table do I/O before being rejected. */
  uint32_t bytes = ((uint32_t)ra->item_count + 1u) * ROW_STRIDE;
  int ok = ptr_ok(ra->rc, ra->items, bytes) &&
           probe_item_row(ra, 1, 1) &&                      /* Master Ball, full    */
           probe_item_row(ra, 0, 0) &&                      /* the "?" placeholder  */
           probe_item_row(ra, (uint16_t)(ra->item_count / 2), 0) &&
           probe_item_row(ra, (uint16_t)(ra->item_count - 1), 0) &&
           probe_item_row(ra, ra->item_count, 0);           /* the arrow row        */
  if (!ok) { ra->items = 0; ra->item_count = 0; }
}

static void validate_types(RomItemArt* ra) {
  if (ra->type_style == ROM_TYPEART_RSE) {
    /* The two LZ headers ARE the signature: exactly 0x1700 followed by exactly 0x60
     * happens once per RSE image and nowhere in FR/LG. The full decode + verify
     * happens in rom_type_sheet_load. */
    if (lz_size_at(ra, ra->type_gfx) == ROM_TYPE_SHEET_BYTES &&
        lz_size_at(ra, ra->type_pal) == ROM_TYPE_PALS_RSE * 32) return;
  } else if (ra->type_style == ROM_TYPEART_FRLG) {
    /* A raw sheet has no header to check, so probe its CONTENT. Every one of the 18
     * badges is a rounded plate: its four corners are transparent and its centre is
     * not. Checking that on the NORMAL badge costs 256 B of reads and rejects a
     * wrong address (or a garbled read) with probability ~1 - 1.4e-5 for random
     * bytes. The palette must also look like a GBA palette (bit 15 clear). */
    uint8_t praw[32];
    if (ptr_ok(ra->rc, ra->type_gfx, ROM_TYPE_FRLG_SHEET_BYTES) &&
        ptr_ok(ra->rc, ra->type_pal, 32) &&
        read_verified(ra, ra->type_pal, praw, sizeof praw)) {
      int pal_ok = 1;
      for (int i = 0; i < 16; i++) if (praw[i * 2 + 1] & 0x80) pal_ok = 0;
      uint8_t blk[8 * TILE_BYTES];
      if (pal_ok && frlg_stage_badge(ra, k_frlg_off[0], blk)) {
        int h = ROM_TYPE_BADGE_H_FRLG;
        if (block_px(blk, BADGE_TILES_W, 0, 0) == 0 &&
            block_px(blk, BADGE_TILES_W, ROM_TYPE_BADGE_W - 1, 0) == 0 &&
            block_px(blk, BADGE_TILES_W, 0, h - 1) == 0 &&
            block_px(blk, BADGE_TILES_W, ROM_TYPE_BADGE_W - 1, h - 1) == 0 &&
            block_px(blk, BADGE_TILES_W, ROM_TYPE_BADGE_W / 2, h / 2) != 0) return;
      }
    }
  }
  ra->type_style = ROM_TYPEART_NONE;
  ra->type_gfx = ra->type_pal = 0;
}

int rom_itemart_open(RomItemArt* ra, const RomCtx* rc) {
  if (!ra) return 0;
  memset(ra, 0, sizeof *ra);
  ra->verify = 1;                       /* fail safe: verification on until told otherwise */
  ra->rc = rc;
  if (!rc || !rc->read) return 0;

  const RomItemArtPins* pin = 0;
  for (int i = 0; i < K_NPINS; i++)
    if (memcmp(k_pins[i].code, rc->code, 4) == 0 && k_pins[i].version == rc->version) {
      pin = &k_pins[i]; break;
    }
  if (!pin) return 0;                   /* unpinned revision -> fail closed, both classes */

  ra->items      = pin->items;
  ra->item_count = pin->items ? pin->item_count : 0;
  ra->type_style = pin->type_style;
  ra->type_gfx   = pin->type_gfx;
  ra->type_pal   = pin->type_pal;

  validate_items(ra);
  validate_types(ra);

  ra->ok = (ra->items || ra->type_style != ROM_TYPEART_NONE) ? 1 : 0;
  return ra->ok;
}

void rom_itemart_set_verify(RomItemArt* ra, int on) { if (ra) ra->verify = on ? 1 : 0; }

/* ---- item icons ------------------------------------------------------------ */

int rom_itemart_have_items(const RomItemArt* ra) {
  return (ra && ra->ok && ra->items && ra->item_count) ? 1 : 0;
}

int rom_itemart_item_count(const RomItemArt* ra) {
  return rom_itemart_have_items(ra) ? (int)ra->item_count : 0;
}

int rom_item_icon(const RomItemArt* ra, uint16_t item_id, uint16_t* dst, uint32_t cap_px) {
  if (!rom_itemart_have_items(ra) || !dst || cap_px < ROM_ITEM_ICON_PX) return 0;
  if (item_id >= ra->item_count) return 0;

  uint8_t e[ROW_STRIDE];
  uint32_t at = ra->items + (uint32_t)item_id * ROW_STRIDE;
  if (!ptr_ok(ra->rc, at, ROW_STRIDE)) return 0;
  if (!read_verified(ra, at, e, ROW_STRIDE)) return 0;
  uint32_t pic = rd32le(e), pal = rd32le(e + 4);
  if (!pic || !pal) return 0;                       /* a NULL row = no icon        */

  uint8_t px[ROM_ITEM_ICON_BYTES];
  uint8_t praw[ROM_ITEM_PAL_BYTES];
  if (!decode_verified(ra, pic, px, sizeof px, ROM_ITEM_ICON_BYTES)) return 0;
  if (!decode_verified(ra, pal, praw, sizeof praw, ROM_ITEM_PAL_BYTES)) return 0;

  uint16_t p[16];
  pal_from_raw(praw, p);
  tiles_to_rgb15(px, ITEM_TILES_W, ROM_ITEM_ICON_W, ROM_ITEM_ICON_H, p, dst);
  return 1;
}

/* ---- type badges ----------------------------------------------------------- */

int rom_itemart_have_types(const RomItemArt* ra) {
  return (ra && ra->ok && ra->type_style != ROM_TYPEART_NONE) ? 1 : 0;
}

int rom_type_badge_count(const RomItemArt* ra) {
  if (!rom_itemart_have_types(ra)) return 0;
  return (ra->type_style == ROM_TYPEART_RSE) ? ROM_TYPE_BADGES_RSE : ROM_TYPE_TYPES;
}

int rom_type_badge_h(const RomItemArt* ra) {
  if (!rom_itemart_have_types(ra)) return 0;
  return (ra->type_style == ROM_TYPEART_RSE) ? ROM_TYPE_BADGE_H_RSE : ROM_TYPE_BADGE_H_FRLG;
}

int rom_type_scratch_bytes(const RomItemArt* ra) {
  if (!rom_itemart_have_types(ra)) return 0;
  return (ra->type_style == ROM_TYPEART_RSE) ? ROM_TYPE_SHEET_BYTES : 0;
}

int rom_type_sheet_load(const RomItemArt* ra, RomTypeSheet* ts,
                        uint8_t* scratch, uint32_t scratch_cap) {
  if (!ts) return 0;
  memset(ts, 0, sizeof *ts);
  if (!rom_itemart_have_types(ra)) return 0;

  if (ra->type_style == ROM_TYPEART_RSE) {
    if (!scratch || scratch_cap < ROM_TYPE_SHEET_BYTES) return 0;
    if (!decode_verified(ra, ra->type_gfx, scratch, scratch_cap, ROM_TYPE_SHEET_BYTES)) return 0;
    uint8_t praw[ROM_TYPE_PALS_RSE * 32];
    if (!decode_verified(ra, ra->type_pal, praw, sizeof praw, sizeof praw)) return 0;
    for (int i = 0; i < ROM_TYPE_PALS_RSE; i++) pal_from_raw(praw + i * 32, ts->pal[i]);
    ts->tiles = scratch;
    ts->style = ROM_TYPEART_RSE;
    ts->count = ROM_TYPE_BADGES_RSE;
    ts->h     = ROM_TYPE_BADGE_H_RSE;
  } else {
    /* FR/LG's sheet is uncompressed, so nothing is staged: a badge's 256 B come
     * straight out of the ROM when it is asked for. Only the shared palette is
     * loaded here. `scratch` is ignored on purpose. */
    (void)scratch; (void)scratch_cap;
    uint8_t praw[32];
    if (!read_verified(ra, ra->type_pal, praw, sizeof praw)) return 0;
    pal_from_raw(praw, ts->pal[0]);
    ts->tiles = 0;
    ts->style = ROM_TYPEART_FRLG;
    ts->count = ROM_TYPE_TYPES;
    ts->h     = ROM_TYPE_BADGE_H_FRLG;
  }
  ts->owner = ra;
  ts->ok = 1;
  return 1;
}

int rom_type_badge(const RomItemArt* ra, const RomTypeSheet* ts, uint8_t badge,
                   uint16_t* dst, uint32_t cap_px) {
  if (!rom_itemart_have_types(ra) || !ts || !ts->ok || !dst) return 0;
  /* A sheet may only be used with the RomItemArt it came from: the three RSE games
   * share a style but not their badge art, so a leftover sheet would draw the wrong
   * game's badges rather than fail. */
  if (ts->owner != ra || ts->style != ra->type_style) return 0;
  if (badge >= ts->count) return 0;
  uint32_t need = (uint32_t)ROM_TYPE_BADGE_W * ts->h;
  if (cap_px < need) return 0;

  if (ts->style == ROM_TYPEART_RSE) {
    if (!ts->tiles) return 0;
    const uint8_t* src = ts->tiles + (uint32_t)badge * 256u;
    tiles_to_rgb15(src, BADGE_TILES_W, ROM_TYPE_BADGE_W, ROM_TYPE_BADGE_H_RSE,
                   ts->pal[k_rse_pal[badge]], dst);
    return 1;
  }

  uint8_t blk[8 * TILE_BYTES];
  if (!frlg_stage_badge(ra, k_frlg_off[badge], blk)) return 0;
  tiles_to_rgb15(blk, BADGE_TILES_W, ROM_TYPE_BADGE_W, ROM_TYPE_BADGE_H_FRLG,
                 ts->pal[0], dst);
  return 1;
}
