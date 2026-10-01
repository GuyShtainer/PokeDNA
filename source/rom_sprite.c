/* Front/back battle sprites + shiny palettes out of the user's own ROM.
 * See rom_sprite.h for the design and for every measured fact behind it. */
#include "rom_sprite.h"

#include <string.h>
#include "map_render.h"   /* mr_lz77 — the streaming LZ77; do not write another */

/* GFRomHeader (ROM+0x100). rom_mon.c reads +0x38..+0x40 of the same struct and
 * rom_text.c reads +0xC4..+0xC8; these four are the sprite block. */
#define GFH_OFF        0x100
#define GFH_VERSION    0x00   /* u32, 1..8                     */
#define GFH_LANGUAGE   0x04   /* u32, 1..8                     */
#define GFH_GAMENAME   0x08   /* char[32], starts "pokemon "   */
#define GFH_FRONT      0x28
#define GFH_BACK       0x2C
#define GFH_NPAL       0x30
#define GFH_SPAL       0x34
#define GFH_BYTES      0x38

#define SHEET_STRIDE   8      /* {const u32* data; u16 size; u16 tag;}  */
#define PAL_STRIDE     8      /* {const u32* data; u16 tag;} + padding  */
#define PAL_MAX_BYTES  (ROM_SPRITE_PAL_BYTES * ROM_SPRITE_MAX_FRAMES)  /* Castform */

/* The icon tables' species axis, shared by the pic tables (rom_mon.h says the same). */
#define UNOWN_B_ENTRY  413

/*
 * RUBY / SAPPHIRE are served from PINNED addresses (BACKLOG #293), because they
 * predate the GF header. The precedent is rom_text.c's k_pins: a row is keyed by
 * game code + revision, so a revision nobody has a dump of stays unpinned and
 * fails closed. The four tables per game were located by an independent
 * shape-scan (tools/rs_locate.py -- the unique run of 440 eight-byte rows whose
 * tag halfword equals the index at +6 for sheets and at +4 for palettes, tag 500 +
 * index for the shiny palettes, and whose pointers land on LZ10 blobs inside the
 * image) and verified by rendering Bulbasaur out of each one:
 *
 *     AXVE rev2 (Ruby):     front 0x081E836C  back 0x081E980C
 *                           npal  0x081EA5CC  spal 0x081EB38C
 *     AXPE rev1 (Sapphire): front 0x081E82FC  back 0x081E979C
 *                           npal  0x081EA55C  spal 0x081EB31C
 *
 * R/S fronts are single-frame (no anim_front), Castform is still 8192 B / four
 * formes, and Deoxys is 2048 B -- Normal only, so R/S add no forme to the set.
 * A pinned row is re-verified at open (table_shape_ok below): the 440-row
 * tag == index shape must hold in all four tables, or the open fails closed.
 */
typedef struct {
  const char* code;      /* 4-char game code at 0xAC */
  uint8_t     version;   /* revision byte at 0xBC    */
  uint32_t    front, back, npal, spal;   /* ROM addresses */
} RomSpritePin;

static const RomSpritePin k_pins[] = {
  { "AXVE", 2, 0x081E836Cu, 0x081E980Cu, 0x081EA5CCu, 0x081EB38Cu },
  { "AXPE", 1, 0x081E82FCu, 0x081E979Cu, 0x081EA55Cu, 0x081EB31Cu },
};
#define K_NPINS ((int)(sizeof k_pins / sizeof k_pins[0]))

/* Shiny palette tags run 500 + index; every other table's tags run 0 + index. */
#define SPAL_TAG_BASE  500u

static uint32_t rd32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A ROM pointer is sane if it lands inside the image with `need` bytes of room. */
static int ptr_ok(const RomCtx* rc, uint32_t addr, uint32_t need) {
  if (!rc || addr < ROM_BASE) return 0;
  uint32_t off = addr - ROM_BASE;
  return off < rc->size && need <= rc->size - off;
}

/* Does `tbl` (a FILE offset) hold 440 rows of `stride` bytes whose u16 tag at
 * `tag_off` equals tag_base + row index and whose pointer (first word) is ROM-sane?
 * Read in 128-byte chunks so the check stays off the big buffers. A pinned address
 * that is wrong, or a revision that merely shares the game code, fails here. */
static int table_shape_once(const RomCtx* rc, uint32_t tbl, uint32_t tag_off,
                            uint32_t tag_base) {
  uint8_t buf[128];
  const uint32_t rows_per = (uint32_t)sizeof buf / SHEET_STRIDE;
  for (uint32_t row = 0; row < ROM_SPRITE_ENTRIES; row += rows_per) {
    uint32_t n = ROM_SPRITE_ENTRIES - row;
    if (n > rows_per) n = rows_per;
    if (!rc->read(rc->ctx, tbl + row * SHEET_STRIDE, buf, n * SHEET_STRIDE)) return 0;
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t* e = buf + i * SHEET_STRIDE;
      uint32_t tag = (uint32_t)e[tag_off] | ((uint32_t)e[tag_off + 1] << 8);
      if (tag != tag_base + row + i) return 0;
      if (!ptr_ok(rc, rd32le(e), 4)) return 0;
    }
  }
  return 1;
}

/* Two attempts: one transient garbled read must not cost the session its art, while a
 * genuinely wrong address fails both times. */
static int table_shape_ok(const RomCtx* rc, uint32_t tbl, uint32_t tag_off,
                          uint32_t tag_base) {
  for (int attempt = 0; attempt < 2; attempt++)
    if (table_shape_once(rc, tbl, tag_off, tag_base)) return 1;
  return 0;
}

/* Ruby/Sapphire: match the pin table by code + revision, then prove the shape. */
static int open_pinned(RomSprite* rs, const RomCtx* rc) {
  if (rc->kind != ROM_RUBY && rc->kind != ROM_SAPPHIRE) return 0;
  for (int i = 0; i < K_NPINS; i++) {
    const RomSpritePin* p = &k_pins[i];
    if (memcmp(p->code, rc->code, 4) != 0 || p->version != rc->version) continue;
    const uint32_t sheet_bytes = (uint32_t)ROM_SPRITE_ENTRIES * SHEET_STRIDE;
    const uint32_t pal_bytes   = (uint32_t)ROM_SPRITE_ENTRIES * PAL_STRIDE;
    if (!ptr_ok(rc, p->front, sheet_bytes) || !ptr_ok(rc, p->back, sheet_bytes) ||
        !ptr_ok(rc, p->npal, pal_bytes)    || !ptr_ok(rc, p->spal, pal_bytes)) return 0;
    uint32_t f = p->front - ROM_BASE, b = p->back - ROM_BASE;
    uint32_t n = p->npal - ROM_BASE,  s = p->spal - ROM_BASE;
    if (!table_shape_ok(rc, f, 6, 0) || !table_shape_ok(rc, b, 6, 0) ||
        !table_shape_ok(rc, n, 4, 0) || !table_shape_ok(rc, s, 4, SPAL_TAG_BASE)) return 0;
    rs->front = f; rs->back = b; rs->npal = n; rs->spal = s;
    rs->ok = 1;
    return 1;
  }
  return 0;   /* an unpinned R/S revision: fail closed */
}

int rom_sprite_open(RomSprite* rs, const RomCtx* rc) {
  if (!rs) return 0;
  memset(rs, 0, sizeof *rs);
  rs->verify = 1;                       /* fail safe: verification on until told otherwise */
  rs->rc = rc;
  if (!rc || !rc->read) return 0;

  /* Ruby/Sapphire predate the GF header: they take the pinned path (see the note above)
   * and never reach the header read below. */
  if (rc->kind == ROM_RUBY || rc->kind == ROM_SAPPHIRE) return open_pinned(rs, rc);

  /* The header read was the module's ONE unverified read, and it is the worst place
   * to skip verification: its result is four table ADDRESSES cached for the whole
   * session, so a single garbled read poisons every later fetch with pointers that
   * still pass the bounds check. Read it twice and require agreement, exactly like
   * the payload path. */
  uint8_t h[GFH_BYTES], h2[GFH_BYTES];
  if (!rc->read(rc->ctx, GFH_OFF, h, sizeof h)) return 0;
  if (!rc->read(rc->ctx, GFH_OFF, h2, sizeof h2)) return 0;
  if (memcmp(h, h2, sizeof h) != 0) return 0;
  uint32_t ver = rd32le(h + GFH_VERSION), lang = rd32le(h + GFH_LANGUAGE);
  if (ver < 1 || ver > 8 || lang < 1 || lang > 8) return 0;
  if (memcmp(h + GFH_GAMENAME, "pokemon ", 8) != 0) return 0;

  uint32_t fr = rd32le(h + GFH_FRONT), bk = rd32le(h + GFH_BACK);
  uint32_t np = rd32le(h + GFH_NPAL),  sp = rd32le(h + GFH_SPAL);
  const uint32_t sheet_bytes = (uint32_t)ROM_SPRITE_ENTRIES * SHEET_STRIDE;
  const uint32_t pal_bytes   = (uint32_t)ROM_SPRITE_ENTRIES * PAL_STRIDE;
  if (!ptr_ok(rc, fr, sheet_bytes) || !ptr_ok(rc, bk, sheet_bytes) ||
      !ptr_ok(rc, np, pal_bytes)   || !ptr_ok(rc, sp, pal_bytes)) return 0;

  rs->front = fr - ROM_BASE;
  rs->back  = bk - ROM_BASE;
  rs->npal  = np - ROM_BASE;
  rs->spal  = sp - ROM_BASE;
  rs->ok    = 1;
  return 1;
}

void rom_sprite_set_verify(RomSprite* rs, int on) { if (rs) rs->verify = on ? 1 : 0; }

int rom_sprite_deoxys_forme(const RomSprite* rs) {
  if (!rs || !rs->ok || !rs->rc) return 0;
  switch (rs->rc->kind) {
    case ROM_FIRERED:   return 1;   /* Attack  */
    case ROM_LEAFGREEN: return 2;   /* Defense */
    case ROM_EMERALD:   return 3;   /* Speed   */
    default:            return 0;   /* R/S carry the Normal forme only */
  }
}

/* internal species + Unown letter -> the pic/palette tables' species axis */
static uint16_t table_species(uint16_t species, uint8_t form) {
  if (species == ROM_SPRITE_UNOWN && form >= 1 && form <= 27)
    return (uint16_t)(UNOWN_B_ENTRY + form - 1);
  return species;
}

/* Reject a form this species has no axis for, before any I/O happens. */
static int form_ok(uint16_t species, uint8_t form) {
  if (species == ROM_SPRITE_UNOWN)    return form <= 27;
  if (species == ROM_SPRITE_DEOXYS ||
      species == ROM_SPRITE_CASTFORM) return form <= 3;
  return form == 0;
}

/* Read one small table entry, twice-and-compare when verification is on. The
 * payload check below cannot cover this: a garbled POINTER that still lands on a
 * valid LZ77 blob decodes identically every time, so both decodes would agree on
 * the wrong sprite. Eight bytes, so the second read costs nothing. */
static int read_entry(const RomSprite* rs, uint32_t off, uint8_t* e, uint32_t n) {
  uint8_t again[SHEET_STRIDE];
  if (n > sizeof again) return 0;             /* both table strides are 8; stay honest */
  if (!rs->rc->read(rs->rc->ctx, off, e, n)) return 0;
  if (!rs->verify) return 1;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!rs->rc->read(rs->rc->ctx, off, again, n)) return 0;
    if (memcmp(e, again, n) == 0) return 1;
    memcpy(e, again, n);
  }
  return 0;
}

/* Decompress `addr` into dst, verifying by repetition when rs->verify is set.
 * Returns the decompressed length, or 0. On 0 the buffer holds garbage.
 *
 * BACKLOG #103 steps 3+4: the decompressed length is not known ahead of time
 * here (unlike rom_chrome/rom_itemart's fixed `want`), so the window is sized
 * off mr_lz77_size()'s own peek: dst + size, cap - size, used only when that
 * tail is comfortably large (>=256 B) -- e.g. a 1-frame 2,048 B portrait in
 * the shared 8,192 B buffer gets a 6,144 B window; a 4-frame 8,192 B sheet
 * has no spare tail and falls back to the default 64 B window. Correctness:
 * mr_lz77_x only writes dst[0,size) and only reads dst[0,out) with out<size
 * for back-references, so dst[size,cap) is provably dead at decode time.
 * Verify is by re-reading the exact CONSUMED compressed span and hashing it
 * (mr_hash_span, the one shared helper) against the decode's own FNV-1a of
 * its input -- never by re-running the CPU-heavy decode a second time. On a
 * mismatch the decode itself is redone (up to 2 retries, matching the old
 * 3-attempt budget), so the pass that is checked is always the pass whose
 * output is kept. */
static uint32_t decode_verified(const RomSprite* rs, uint32_t addr,
                                uint8_t* dst, uint32_t cap) {
  uint32_t size = mr_lz77_size(rs->rc, addr);
  uint8_t* win = 0; uint32_t win_bytes = 0;
  if (size && size <= cap && cap - size >= 256u) { win = dst + size; win_bytes = cap - size; }

  for (int attempt = 0; attempt < 3; attempt++) {
    uint32_t consumed = 0, in_hash = 0;
    uint32_t n = mr_lz77_x(rs->rc, addr, dst, cap, win, win_bytes, &consumed, &in_hash);
    if (!n) {
      /* mr_lz77_x returns 0 both for a transient bad read of its OWN header
       * (type byte != 0x10, or a malformed NEXT/disp mid-stream) and for a
       * genuinely oversize/corrupt blob -- the two are indistinguishable at
       * this level. Retrying with the fixed stack window (never derived from
       * `size`, never aliasing dst) costs nothing but a re-read, so spend the
       * budget: the loop-exit `return 0` below is the terminal path. */
      win = 0; win_bytes = 0; continue;
    }
    /* `win` was sized off a SEPARATE mr_lz77_size() peek of the same 4 header
     * bytes. If that peek disagreed with the header this decode actually read,
     * dst + size lands INSIDE dst[0,n): the window aliases live output and
     * mr_hash_span()'s re-read below would overwrite it. A successful decode
     * always returns exactly its header's size, so n != size means the two
     * header reads disagreed -- a transient bad header read, not proof the
     * blob itself is bad. Fall back to mr_lz77_x's own fixed 64 B stack
     * window (win = 0, win_bytes = 0) for the remaining attempts: that window
     * is never derived from `size` and never overlaps `dst`, so aliasing is
     * impossible regardless of what the header reads next -- BACKLOG #103's
     * review found this `return 0` was spending the whole 3-attempt retry
     * budget on one bad peek instead of falling back, turning a transient
     * miss into a missing sprite. */
    if (win_bytes && n != size) { win = 0; win_bytes = 0; continue; }
    if (!rs->verify) return n;
    uint32_t reread_hash = 0;
    if (mr_hash_span(rs->rc, addr, consumed, win, win_bytes, &reread_hash) && reread_hash == in_hash)
      return n;
  }
  return 0;                                     /* three tries, never twice the same */
}

int rom_sprite_pic(const RomSprite* rs, RomSpriteSide side, uint16_t species,
                   uint8_t form, uint8_t* dst, uint32_t dst_cap, RomSpritePic* out) {
  if (out) memset(out, 0, sizeof *out);
  if (!rs || !rs->ok || !dst || !out) return 0;
  if (side != ROM_SPRITE_FRONT && side != ROM_SPRITE_BACK) return 0;
  if (!form_ok(species, form)) return 0;
  uint16_t ts = table_species(species, form);
  if (ts >= ROM_SPRITE_ENTRIES) return 0;

  /* CompressedSpriteSheet: only `data` is trustworthy — `size` reads 2048 in every
   * row of every table and is the game's VRAM allocation, not the payload length. */
  uint8_t e[SHEET_STRIDE];
  uint32_t tbl = (side == ROM_SPRITE_FRONT) ? rs->front : rs->back;
  if (!read_entry(rs, tbl + (uint32_t)ts * SHEET_STRIDE, e, SHEET_STRIDE)) return 0;
  uint32_t pic = rd32le(e);
  if (!ptr_ok(rs->rc, pic, 4)) return 0;

  uint32_t cap = (dst_cap < ROM_SPRITE_BUF_BYTES) ? dst_cap : ROM_SPRITE_BUF_BYTES;
  uint32_t n = decode_verified(rs, pic, dst, cap);
  /* A whole number of 64x64 frames, at most four. Anything else is a hacked table
   * or a bad read wearing a valid LZ77 header. */
  if (!n || (n & (ROM_SPRITE_FRAME_BYTES - 1)) != 0) return 0;
  uint32_t frames = n / ROM_SPRITE_FRAME_BYTES;
  if (frames < 1 || frames > ROM_SPRITE_MAX_FRAMES) return 0;

  out->bytes  = n;
  out->frames = (uint8_t)frames;
  out->frame  = 0;
  out->form_exact = 1;

  if (species == ROM_SPRITE_CASTFORM) {
    /* Four formes, frame index == forme index == palette index. */
    out->kind = ROM_SPRITE_FORME;
    if (form < frames) out->frame = form;
    else               out->form_exact = 0;     /* truncated/odd build: show Normal */
  } else if (species == ROM_SPRITE_DEOXYS) {
    /* frame 0 = Normal in every cart; frame 1 = this cart's own forme. */
    out->kind = ROM_SPRITE_FORME;
    int own = rom_sprite_deoxys_forme(rs);
    if (form == 0)                          out->frame = 0;
    else if (form == own && frames >= 2)    out->frame = 1;
    else                                    out->form_exact = 0;   /* Normal stand-in */
  } else {
    out->kind = (frames >= 2) ? ROM_SPRITE_ANIM : ROM_SPRITE_SINGLE;
  }
  return 1;
}

int rom_sprite_pal(const RomSprite* rs, uint16_t species, uint8_t form, int shiny,
                   uint16_t dst[16]) {
  if (!rs || !rs->ok || !dst) return 0;
  if (!form_ok(species, form)) return 0;
  uint16_t ts = table_species(species, form);
  if (ts >= ROM_SPRITE_ENTRIES) return 0;

  uint8_t e[PAL_STRIDE];                      /* {const u32* data; u16 tag;} */
  uint32_t tbl = shiny ? rs->spal : rs->npal;
  if (!read_entry(rs, tbl + (uint32_t)ts * PAL_STRIDE, e, PAL_STRIDE)) return 0;
  uint32_t pd = rd32le(e);
  if (!ptr_ok(rs->rc, pd, 4)) return 0;

  uint8_t raw[PAL_MAX_BYTES];                 /* 128 B on the stack, never in EWRAM */
  uint32_t n = decode_verified(rs, pd, raw, sizeof raw);
  if (n != ROM_SPRITE_PAL_BYTES && n != PAL_MAX_BYTES) return 0;

  /* Castform's blob holds four palettes, one per forme; everything else has one. */
  uint32_t which = (species == ROM_SPRITE_CASTFORM && n == PAL_MAX_BYTES) ? form : 0;
  if ((which + 1) * ROM_SPRITE_PAL_BYTES > n) return 0;
  const uint8_t* p = raw + which * ROM_SPRITE_PAL_BYTES;
  for (int i = 0; i < 16; i++)
    dst[i] = (uint16_t)(p[i * 2] | ((uint16_t)p[i * 2 + 1] << 8));
  return 1;
}

/*
 * 4bpp (8x8 tiles, GBA 1D order) -> 4096 RGB15 pixels, in place, in the same
 * 8192-byte buffer, with no second buffer.
 *
 * Two moves make that safe:
 *   1. the wanted frame is de-tiled into the buffer's LAST 2048 bytes, from a
 *      copy parked at the head — source [0,2048) and destination [6144,8192)
 *      never overlap;
 *   2. the expansion then runs FORWARD, reading linear byte 6144+i and writing
 *      the 4 bytes at 4i. A write at [4i,4i+3] can only reach source byte 6144+j
 *      for j >= 4i-6144, and 4i-6144+3 > i needs i > 2047 — impossible. The one
 *      touching case is i = 2047, where the write covers the byte being read, so
 *      the read is taken into a local BEFORE the write. Do not "optimise" that
 *      local away.
 */
int rom_sprite_to_rgb15(void* buf, uint32_t cap, uint8_t frame, const uint16_t pal[16]) {
  /* This expands 4bpp -> RGB15 IN PLACE and therefore always writes a full
   * 64x64x2 = 8192 bytes, staging the de-tiled frame at buf[6144..8191]. The
   * signature used to take no capacity, while rom_sprite_pic's own contract
   * permits a 2048-byte dst for a known single-frame row — so a caller who
   * followed BOTH documents got a 6 KB overrun. The capacity is a parameter now
   * and a short buffer is refused, not truncated. */
  if (!buf || !pal || frame >= ROM_SPRITE_MAX_FRAMES) return 0;
  if (cap < ROM_SPRITE_BUF_BYTES) return 0;
  uint8_t*  b = (uint8_t*)buf;
  uint16_t* out = (uint16_t*)buf;

  /* 1. the chosen frame to the head (memmove: frame 0 is a no-op, frame n overlaps) */
  memmove(b, b + (uint32_t)frame * ROM_SPRITE_FRAME_BYTES, ROM_SPRITE_FRAME_BYTES);

  /* 2. de-tile into the last quarter. A tile row is 4 bytes = 8 pixels and lands
   *    contiguously in the linear image, so this is 512 four-byte moves. */
  uint8_t* lin = b + (ROM_SPRITE_BUF_BYTES - ROM_SPRITE_FRAME_BYTES);   /* +6144 */
  for (uint32_t t = 0; t < 64; t++) {
    uint32_t tx = (t & 7u) * 4u;              /* byte column of the tile   */
    uint32_t ty = (t >> 3) * 8u;              /* pixel row of the tile     */
    const uint8_t* src = b + t * 32u;
    for (uint32_t row = 0; row < 8; row++) {
      uint8_t* d = lin + (ty + row) * 32u + tx;
      d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
      src += 4;
    }
  }

  /* 3. expand forward; index 0 is transparent, everything else gets the 0x8000
   *    opacity bit ui_sprite() keys on. */
  for (uint32_t i = 0; i < ROM_SPRITE_FRAME_BYTES; i++) {
    uint8_t v = lin[i];                        /* read BEFORE the write (see above) */
    uint8_t lo = (uint8_t)(v & 0x0Fu), hi = (uint8_t)(v >> 4);
    out[i * 2u]      = lo ? (uint16_t)(0x8000u | (pal[lo] & 0x7FFFu)) : 0u;
    out[i * 2u + 1u] = hi ? (uint16_t)(0x8000u | (pal[hi] & 0x7FFFu)) : 0u;
  }
  return 1;
}
