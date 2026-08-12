/* Game Boy / GBC Pokemon picture codecs. See gb_sprite_codec.h for the format
 * write-up; this file is the transcription, and every non-obvious step cites the
 * pret disassembly line it came from.
 *
 * Sources of truth (NOT wikis -- a transposed digit off a wiki has already cost
 * this project a day):
 *   Gen 1  pret/pokered   home/uncompress.asm   (the whole decompressor)
 *          pret/pokered   home/pics.asm         (how the 1bpp chunks interleave)
 *          pret/pokered   tools/pkmncompress.c  (pret's own C encoder/decoder --
 *                                                a second reading of the same
 *                                                algorithm, and it agrees)
 *   Gen 2  pret/pokecrystal home/decompress.asm (the lz3 command set)
 *
 * Verified by decoding every pic in Guy's Red.gb, Yellow.gb, Gold.gbc and
 * Crystal.gbc and comparing pixel-for-pixel with the PNGs those decomps build
 * from -- all four dumps hash-match the decomps' roms.sha1, so the PNGs are
 * literally the cartridge art. 1716 pictures, zero differing pixels.
 */

#include <string.h>
#include "gb_sprite_codec.h"

/* Pull the compressed bytes through a small window instead of staging the blob:
 * on hardware every refill is one SD read, and 64 B keeps a 7x7 pic to ~11 of
 * them while costing nothing on the stack. */
#define WINDOW 64

typedef struct {
  GbReadFn rd;
  void    *ctx;
  uint32_t base;        /* file offset of stream byte 0                      */
  uint32_t pos;         /* next stream byte index                            */
  uint32_t cap;         /* hard budget (GB_SPRITE_MAX_INPUT)                 */
  uint32_t wstart;      /* stream index of win[0]                            */
  uint32_t wlen;
  uint8_t  win[WINDOW];
  uint8_t  cur;         /* byte being shifted out, MSB first                 */
  uint8_t  nbits;       /* bits left in cur                                  */
  bool     fail;
} Br;

static void br_init(Br *b, GbReadFn rd, void *ctx, uint32_t off) {
  memset(b, 0, sizeof *b);
  b->rd = rd;
  b->ctx = ctx;
  b->base = off;
  b->cap = GB_SPRITE_MAX_INPUT;
  b->wstart = 0;
  b->wlen = 0;
}

static unsigned br_byte(Br *b) {
  if (b->fail) return 0;
  if (b->pos >= b->cap) { b->fail = true; return 0; }
  if (b->pos < b->wstart || b->pos >= b->wstart + b->wlen) {
    uint32_t want = b->cap - b->pos;
    if (want > WINDOW) want = WINDOW;
    /* A pic can sit near the end of the file, where a full-window read is a short
     * read and the callback rightly refuses it. Back off rather than give up. */
    while (want && !b->rd(b->ctx, b->base + b->pos, b->win, want)) want >>= 1;
    if (!want) { b->fail = true; return 0; }
    b->wstart = b->pos;
    b->wlen = want;
  }
  return b->win[b->pos++ - b->wstart];
}

static unsigned br_bit(Br *b) {
  /* ReadNextInputBit, pokered/home/uncompress.asm:242 -- MSB first. */
  if (b->nbits == 0) {
    b->cur = (uint8_t)br_byte(b);
    b->nbits = 8;
  }
  b->nbits--;
  unsigned v = (b->cur >> 7) & 1u;
  b->cur = (uint8_t)(b->cur << 1);
  return v;
}

/* ------------------------------------------------------------------ Gen 1 */

/* DecodeNybble0Table / DecodeNybble1Table, pokered/home/uncompress.asm:432-449
 * (identical to pkmncompress.c's `codes`). Row index is the LSB of the previously
 * decoded nybble; the tables are the Gray code that turns "toggle on a 1 bit"
 * into a nybble lookup. */
static const uint8_t gb1_decode[2][16] = {
  { 0x0, 0x1, 0x3, 0x2, 0x7, 0x6, 0x4, 0x5, 0xF, 0xE, 0xC, 0xD, 0x8, 0x9, 0xB, 0xA },
  { 0xF, 0xE, 0xC, 0xD, 0x8, 0x9, 0xB, 0xA, 0x0, 0x1, 0x3, 0x2, 0x7, 0x6, 0x4, 0x5 },
};

/* One 1bpp chunk into `plane` (wt*ht*8 bytes, pre-zeroed).
 *
 * The write order is the fiddly part and is dictated by MoveToNextBufferPosition
 * (uncompress.asm:153): a plane byte covers 8 pixels across, and the stream fills
 * a whole tile COLUMN top to bottom at one 2-bit slot before moving to the next
 * slot -- slots run 3,2,1,0, i.e. bits 7-6 first and bits 1-0 last -- and only
 * then steps to the next tile column. Byte index is col*H + pixel_row. */
static GbSpriteErr gb1_chunk(Br *br, uint8_t *plane, unsigned wt, unsigned H) {
  const uint32_t total = (uint32_t)wt * 4u * H;
  uint32_t n = 0;
  unsigned col = 0, slot = 3, y = 0;

  /* .startDecompression (uncompress.asm:81): the first bit says whether the plane
   * opens with literal 2-bit groups or with a run of zeros. */
  unsigned data_mode = br_bit(br);

  while (n < total) {
    if (data_mode) {
      for (;;) {
        unsigned g = br_bit(br) << 1;
        g |= br_bit(br);
        if (br->fail) return GB_SPRITE_E_READ;
        if (g == 0) break;                 /* 00 switches to a zero run          */
        plane[col * H + y] |= (uint8_t)(g << (slot * 2));
        if (++y == H) { y = 0; if (slot) slot--; else { slot = 3; col++; } }
        if (++n == total) break;
      }
    } else {
      /* .readRLEncodedZeros (uncompress.asm:96): count the leading 1 bits to get
       * the field width, read width+1 more bits, then add 2^(width+1)-1 -- the
       * LengthEncodingOffsetList (:273) that makes every length uniquely coded. */
      unsigned bits = 0;
      while (br_bit(br))
        if (++bits >= 16) return GB_SPRITE_E_DATA;   /* would run off the table  */
      if (br->fail) return GB_SPRITE_E_READ;
      uint32_t v = 0;
      for (unsigned i = 0; i <= bits; i++) v = (v << 1) | br_bit(br);
      if (br->fail) return GB_SPRITE_E_READ;
      uint32_t run = v + ((1u << (bits + 1)) - 1u);
      while (n < total && run) {
        /* the bits are already zero; just walk the cursor */
        if (++y == H) { y = 0; if (slot) slot--; else { slot = 3; col++; } }
        n++;
        run--;
      }
    }
    data_mode ^= 1u;
  }
  return GB_SPRITE_OK;
}

/* SpriteDifferentialDecode (uncompress.asm:305). A 0 bit keeps the running pixel
 * value, a 1 toggles it -- done a nybble at a time through the tables. The state
 * resets at the start of each PIXEL ROW, and a row spans all the tile columns,
 * which is why this walks col-within-row and not the buffer order. */
static void gb1_undiff(uint8_t *plane, unsigned wt, unsigned H) {
  for (unsigned row = 0; row < H; row++) {
    unsigned last = 0;
    for (unsigned col = 0; col < wt; col++) {
      const uint32_t i = col * H + row;
      unsigned hi = gb1_decode[last & 1u][(plane[i] >> 4) & 0xFu];
      unsigned lo = gb1_decode[hi & 1u][plane[i] & 0xFu];
      plane[i] = (uint8_t)((hi << 4) | lo);
      last = lo;
    }
  }
}

GbSpriteErr gb_sprite_gen1(GbSprite *out, GbReadFn rd, void *ctx, uint32_t off) {
  if (!out || !rd) return GB_SPRITE_E_ARGS;

  Br br;
  br_init(&br, rd, ctx, off);

  unsigned hdr = br_byte(&br);
  if (br.fail) return GB_SPRITE_E_READ;
  unsigned wt = (hdr >> 4) & 0xFu, ht = hdr & 0xFu;
  /* The format allows a nybble up to 15 but the game's buffers are 7x7 and no
   * retail pic exceeds it, so anything bigger is a bad address, not a big pic. */
  if (wt < 1 || wt > GB_SPRITE_MAX_TILES || ht < 1 || ht > GB_SPRITE_MAX_TILES)
    return GB_SPRITE_E_HEADER;

  const unsigned W = wt * 8, H = ht * 8;
  const uint32_t plane_sz = (uint32_t)wt * ht * 8u;
  uint8_t *p0 = out->work;                 /* sSpriteBuffer1 -- the LOW bitplane  */
  uint8_t *p1 = out->work + plane_sz;      /* sSpriteBuffer2 -- the HIGH bitplane */
  memset(out->work, 0, plane_sz * 2u);     /* _UncompressSpriteData clears both   */

  /* The next bit picks which buffer takes the FIRST chunk (BIT_USE_SPRITE_BUFFER_2,
   * uncompress.asm:54). */
  unsigned order = br_bit(&br);
  uint8_t *first = order ? p1 : p0;
  uint8_t *second = order ? p0 : p1;

  GbSpriteErr e = gb1_chunk(&br, first, wt, H);
  if (e) return e;

  /* The unpacking mode is read between the chunks, at the top of the LAST one
   * (uncompress.asm:74): 0 -> mode 0, 10 -> mode 1, 11 -> mode 2. */
  unsigned mode = 0;
  if (br_bit(&br)) mode = 1 + br_bit(&br);
  if (br.fail) return GB_SPRITE_E_READ;

  e = gb1_chunk(&br, second, wt, H);
  if (e) return e;
  if (br.fail) return GB_SPRITE_E_READ;

  /* UnpackSprite (:292). The first chunk is always differentially decoded; the
   * second is too unless mode 1 said it is a raw XOR mask; modes 1 and 2 then XOR
   * the first into the second. */
  gb1_undiff(first, wt, H);
  if (mode != 1) gb1_undiff(second, wt, H);
  if (mode != 0)
    for (uint32_t i = 0; i < plane_sz; i++) second[i] ^= first[i];

  /* InterlaceMergeSpriteBuffers (pokered/home/pics.asm) writes buffer1 to the EVEN
   * byte of each 2bpp row pair, and the even byte of a Game Boy tile row is the
   * LOW bitplane -- so p0 is bit 0 and p1 is bit 1. */
  for (unsigned y = 0; y < H; y++) {
    uint8_t *dst = out->px + (uint32_t)y * W;
    for (unsigned x = 0; x < W; x++) {
      const uint32_t i = (uint32_t)(x >> 3) * H + y;
      const unsigned b = 7u - (x & 7u);
      dst[x] = (uint8_t)((((p1[i] >> b) & 1u) << 1) | ((p0[i] >> b) & 1u));
    }
  }

  out->wt = (uint8_t)wt;  out->ht = (uint8_t)ht;
  out->w = (uint8_t)W;    out->h = (uint8_t)H;
  out->consumed = br.pos;
  return GB_SPRITE_OK;
}

/* ------------------------------------------------------------------ Gen 2 */

/* Command ids, pokecrystal/home/decompress.asm:33-52. Bit 7 of the id marks the
 * three "reuse the output" commands. */
#define LZ_END       0xFF
#define LZ_LITERAL   0x00
#define LZ_ITERATE   0x20
#define LZ_ALTERNATE 0x40
#define LZ_ZERO      0x60
#define LZ_REPEAT    0x80
#define LZ_FLIP      0xA0
#define LZ_REVERSE   0xC0
#define LZ_LONG      0xE0

static uint8_t bitrev(uint8_t v) {
  /* .floop in decompress.asm:282 -- rotate right into b, eight times. */
  uint8_t r = 0;
  for (unsigned i = 0; i < 8; i++) {
    r = (uint8_t)(((unsigned)r << 1) | (v & 1u));
    v = (uint8_t)(v >> 1);
  }
  return r;
}

static GbSpriteErr lz3(Br *br, uint8_t *out, uint32_t limit, uint32_t *outlen) {
  uint32_t len = 0;

  while (len < limit) {
    unsigned c = br_byte(br);
    if (br->fail) return GB_SPRITE_E_READ;
    if (c == LZ_END) break;

    unsigned cmd = c & 0xE0u;
    uint32_t n;
    if (cmd == LZ_LONG) {
      /* 111xxxyy yyyyyyyy: the opcode moves to bits 4-2 and the length to 10 bits
       * (decompress.asm:78-102). */
      cmd = (unsigned)((c << 3) & 0xE0u);
      n = (uint32_t)((c & 0x03u) << 8);
      n |= (uint32_t)br_byte(br);
      n += 1u;
    } else {
      n = (uint32_t)(c & 0x1Fu) + 1u;
    }
    if (br->fail) return GB_SPRITE_E_READ;

    if (cmd & 0x80u) {
      /* .rewrite (:206). A negative (bit 7 set) offset is 7 bits back from HERE;
       * a positive one is 15 bits forward from the START of the output. */
      unsigned b = br_byte(br);
      if (br->fail) return GB_SPRITE_E_READ;
      int64_t src;
      if (b & 0x80u) {
        src = (int64_t)len - (int64_t)(b & 0x7Fu) - 1;
      } else {
        src = (int64_t)((b << 8) | br_byte(br));
        if (br->fail) return GB_SPRITE_E_READ;
      }
      /* Overlapping copies are legal and common (that is how a run is coded), so
       * the check has to be per byte: for REPEAT/FLIP src and len advance
       * together and it can only fire on entry, but REVERSE walks src backwards
       * and can fall off the front of the output part way through. */
      for (uint32_t i = 0; i < n && len < limit; i++) {
        if (src < 0 || (uint32_t)src >= len) return GB_SPRITE_E_DATA;
        uint8_t v = out[src];
        out[len++] = (cmd == LZ_FLIP) ? bitrev(v) : v;
        src += (cmd == LZ_REVERSE) ? -1 : 1;
      }
    } else if (cmd == LZ_LITERAL) {
      for (uint32_t i = 0; i < n && len < limit; i++) {
        unsigned v = br_byte(br);
        if (br->fail) return GB_SPRITE_E_READ;
        out[len++] = (uint8_t)v;
      }
    } else if (cmd == LZ_ITERATE) {
      unsigned v = br_byte(br);
      if (br->fail) return GB_SPRITE_E_READ;
      for (uint32_t i = 0; i < n && len < limit; i++) out[len++] = (uint8_t)v;
    } else if (cmd == LZ_ALTERNATE) {
      unsigned a = br_byte(br), v = br_byte(br);
      if (br->fail) return GB_SPRITE_E_READ;
      for (uint32_t i = 0; i < n && len < limit; i++)
        out[len++] = (uint8_t)((i & 1u) ? v : a);
    } else { /* LZ_ZERO */
      for (uint32_t i = 0; i < n && len < limit; i++) out[len++] = 0;
    }
  }
  *outlen = len;
  return GB_SPRITE_OK;
}

GbSpriteErr gb_sprite_gen2(GbSprite *out, GbReadFn rd, void *ctx, uint32_t off,
                           unsigned wt, unsigned ht) {
  if (!out || !rd) return GB_SPRITE_E_ARGS;
  if (wt < 1 || wt > GB_SPRITE_MAX_TILES || ht < 1 || ht > GB_SPRITE_MAX_TILES)
    return GB_SPRITE_E_ARGS;

  Br br;
  br_init(&br, rd, ctx, off);

  const uint32_t need = (uint32_t)wt * ht * 16u;
  uint32_t got = 0;
  GbSpriteErr e = lz3(&br, out->work, need, &got);
  if (e) return e;
  if (got < need) return GB_SPRITE_E_DATA;      /* stream ended mid-picture */

  /* The tiles are COLUMN-major -- pokecrystal builds backs with rgbgfx --columns
   * and transposes each animation frame (pokemon_animation_graphics.c
   * transpose_tiles), so tile (col,row) is at index col*ht + row. Within a tile a
   * row is two bytes, low bitplane first. */
  const unsigned W = wt * 8, H = ht * 8;
  for (unsigned y = 0; y < H; y++) {
    uint8_t *dst = out->px + (uint32_t)y * W;
    for (unsigned x = 0; x < W; x++) {
      const uint32_t t = (uint32_t)(x >> 3) * ht + (y >> 3);
      const uint32_t o = t * 16u + (uint32_t)(y & 7u) * 2u;
      const unsigned b = 7u - (x & 7u);
      dst[x] = (uint8_t)((((out->work[o + 1] >> b) & 1u) << 1) |
                         ((out->work[o] >> b) & 1u));
    }
  }

  out->wt = (uint8_t)wt;  out->ht = (uint8_t)ht;
  out->w = (uint8_t)W;    out->h = (uint8_t)H;
  out->consumed = br.pos;
  return GB_SPRITE_OK;
}

const char *gb_sprite_err(GbSpriteErr e) {
  switch (e) {
    case GB_SPRITE_OK:       return "ok";
    case GB_SPRITE_E_ARGS:   return "bad arguments";
    case GB_SPRITE_E_READ:   return "read failed or input budget exhausted";
    case GB_SPRITE_E_HEADER: return "not a pic header";
    case GB_SPRITE_E_DATA:   return "malformed compressed data";
  }
  return "?";
}
