/* Host (PC) test for gb_sprite_codec — the Game Boy / GBC Pokemon picture codecs
 * that let a Gen-1 or Gen-2 import be drawn in the art of the game it came from.
 *
 * All of the codec's input goes through a GbReadFn, so the code exercised here is
 * byte-for-byte the code that will run on the GBA against the SD card.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_gbcodec_test.c source/gb_sprite_codec.c \
 *      -o /tmp/hgbc && /tmp/hgbc
 *
 * ROMs: Guy's own cartridge dumps at gba-toolkit/roms/gb (override with $GBROMS).
 * They are never in the repo; if they are absent the ROM sweeps SKIP and the
 * synthetic half still runs.
 *
 * ---- where the expected values come from (this is the whole point) ----------
 * A decompressor that only agrees with itself proves nothing. The CRCs below were
 * produced by an INDEPENDENT Python implementation written from the same pret
 * disassembly, and that implementation was first checked pixel-for-pixel against
 * pret's own PNG artwork for every pic in all four ROMs — 1716 pictures, zero
 * differing pixels. That check is meaningful because all four of Guy's dumps
 * hash-match the decomps' roms.sha1 (Red.gb = pokered.gbc, Yellow.gb =
 * pokeyellow.gbc, Gold.gbc = pokegold.gbc, Crystal.gbc = pokecrystal11.gbc), so
 * the PNGs pret compresses are literally the art inside the cartridge. So the
 * chain is: Game Freak's pixels -> pret's PNG -> Python -> these constants -> C.
 *
 * What it proves:
 *   1) every species in Red, Yellow, Gold and Crystal decodes — 302 pics per Gen-1
 *      ROM (151 species x front+back, Mew included) and 552 per Gen-2 ROM (251
 *      species x front+back plus all 26 Unown letters) — to the exact bytes the
 *      art-verified oracle produces, aggregate CRC32 per ROM plus per-species spot
 *      checks;
 *   2) geometry is always 1..7 tiles, every pixel is 0..3, and NOTHING is written
 *      outside w*h — the whole GbSprite is poisoned before each decode and the
 *      tail is re-checked after;
 *   3) no read ever lands outside [off, off + GB_SPRITE_MAX_INPUT) — the reader
 *      records the highest byte touched;
 *   4) decoding is deterministic (same input, same output, twice);
 *   5) garbage and truncated input are REFUSED, not chased: a stream that is cut
 *      short, one whose zero-run length code overflows the encoding table, and one
 *      whose lz3 back-reference points outside the output all return an error, and
 *      100k pseudo-random streams terminate inside the budget without ever
 *      touching a byte outside the picture;
 *   6) the CRC check has teeth — flipping one pixel of a real decode changes it.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gb_sprite_codec.h"

static int fails = 0, checks = 0;
static void chk(const char *who, const char *what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", who, what); fails++; }
}

/* ------------------------------------------------------------------ CRC32 */
static uint32_t crc_tab[256];
static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_tab[i] = c;
  }
}
/* zlib-compatible running CRC32 (crc_upd(0, ...) == zlib.crc32(...)). */
static uint32_t crc_upd(uint32_t crc, const void *buf, size_t n) {
  const uint8_t *p = (const uint8_t *)buf;
  crc = ~crc;
  while (n--) crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
static uint32_t crc_pic(uint32_t c, const GbSprite *s) {
  uint8_t hdr[2] = { s->wt, s->ht };
  c = crc_upd(c, hdr, 2);
  return crc_upd(c, s->px, (size_t)s->w * s->h);
}

/* ------------------------------------------------------ readers under test */
typedef struct {
  const uint8_t *data;
  uint32_t size;
  uint32_t cut;        /* pretend the file ends here (0 = don't)              */
  uint32_t lo, hi;     /* [lowest, highest+1) byte offset actually touched    */
  uint32_t calls;
} Src;

static bool src_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
  Src *s = (Src *)ctx;
  uint32_t end = s->cut ? s->cut : s->size;
  s->calls++;
  if ((uint64_t)off + len > end) return false;
  if (s->calls == 1 || off < s->lo) s->lo = off;
  if (off + len > s->hi) s->hi = off + len;
  memcpy(dst, s->data + off, len);
  return true;
}
static void src_reset(Src *s) { s->lo = s->hi = 0; s->calls = 0; }

/* ------------------------------------------------- poison / bounds harness */
#define POISON 0xA5
static void poison(GbSprite *s) { memset(s, POISON, sizeof *s); }

static void check_sane(const char *who, const GbSprite *s, const Src *src,
                       uint32_t off) {
  chk(who, "geometry in 1..7 tiles",
      s->wt >= 1 && s->wt <= 7 && s->ht >= 1 && s->ht <= 7);
  chk(who, "pixel dims match tile dims", s->w == s->wt * 8 && s->h == s->ht * 8);
  size_t used = (size_t)s->w * s->h;
  int bad = 0;
  for (size_t i = 0; i < used; i++) if (s->px[i] > 3) bad++;
  chk(who, "every pixel is an index 0..3", bad == 0);
  int spill = 0;
  for (size_t i = used; i < GB_SPRITE_MAX_PX; i++) if (s->px[i] != POISON) spill++;
  chk(who, "nothing written past w*h", spill == 0);
  chk(who, "no read below the start offset", src->lo >= off);
  chk(who, "no read past the input budget",
      src->hi <= off + GB_SPRITE_MAX_INPUT);
}

/* ------------------------------------------------------------- ROM loading */
static uint8_t *load(const char *path, uint32_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = (uint8_t *)malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  fclose(f);
  *size = (uint32_t)n;
  return b;
}
/* The cartridge title at $134. Refusing to guess which game a file is stops a
 * wrong ROM being silently mis-decoded into a "pass". */
static int title_is(const uint8_t *rom, const char *want) {
  return memcmp(rom + 0x134, want, strlen(want)) == 0;
}

/* =========================================================== Gen-1 sweep ==
 * Enumerating "every species" needs the pic ADDRESSES, and they live in the
 * base-stats table. Nothing here is a remembered constant:
 *   - the table is FOUND by structure — 150 entries of 28 bytes whose first byte
 *     counts 1,2,3,... (data/pokemon/base_stats.asm). That is unique in both ROMs.
 *   - Red keeps Mew out of the table ("a kind of prank", data/pokemon/mew.asm), so
 *     its entry is found by its own signature: dex 151, five base stats of 100,
 *     type1 == type2. Yellow simply has 151 entries.
 *   - the pic BANK is not in the entry (home/pics.asm picks it from the internal
 *     index), so the candidates are the five Pics banks $9..$D plus bank 1 for
 *     Red's Mew (layout.link / BANK(MewPicFront)), and the right one is the one
 *     where the front pic's own header byte equals the entry's sprite-dimension
 *     byte, both pics decode, and the back pic is 4x4 (every Gen-1 back is). That
 *     is unique for all 151 species of both ROMs — the test asserts it.
 */
#define G1E 28

static long g1_find_base(const uint8_t *rom, uint32_t size) {
  long hit = -1;
  for (uint32_t o = 0; o + G1E * 150 <= size; o++) {
    if (rom[o] != 1) continue;
    int ok = 1;
    for (uint32_t i = 1; i < 150 && ok; i++) if (rom[o + i * G1E] != i + 1) ok = 0;
    if (ok) { if (hit >= 0) return -2; hit = (long)o; }
  }
  return hit;
}
static long g1_find_mew(const uint8_t *rom, uint32_t size) {
  long hit = -1;
  for (uint32_t o = 0; o + G1E <= size; o++) {
    if (rom[o] != 151) continue;
    if (rom[o + 1] != 100 || rom[o + 2] != 100 || rom[o + 3] != 100 ||
        rom[o + 4] != 100 || rom[o + 5] != 100 || rom[o + 6] != rom[o + 7]) continue;
    if (hit >= 0) return -2;
    hit = (long)o;
  }
  return hit;
}

static void sweep_gen1(const char *who, const uint8_t *rom, uint32_t size,
                       int want_pics, uint32_t want_crc,
                       uint32_t spot1, uint32_t spot25, uint32_t spot151) {
  static const unsigned banks[] = { 1, 9, 0xA, 0xB, 0xC, 0xD };
  Src src = { rom, size, 0, 0, 0, 0 };
  GbSprite *a = (GbSprite *)malloc(sizeof *a);
  GbSprite *b = (GbSprite *)malloc(sizeof *b);

  long base = g1_find_base(rom, size);
  chk(who, "base-stats table found, and uniquely", base >= 0);
  if (base < 0) { free(a); free(b); return; }

  int entries = (rom[base + 150 * G1E] == 151) ? 151 : 150;
  long mew = -1;
  if (entries == 150) {
    mew = g1_find_mew(rom, size);
    chk(who, "Mew's out-of-table base-stats entry found, and uniquely", mew >= 0);
    if (mew < 0) { free(a); free(b); return; }
  }

  uint32_t crc = 0;
  int n = 0, ambiguous = 0;
  for (int dex = 1; dex <= 151; dex++) {
    long e = (dex <= entries) ? base + (long)(dex - 1) * G1E : mew;
    unsigned dim = rom[e + 0x0A];
    uint32_t fa = (uint32_t)rom[e + 0x0B] | ((uint32_t)rom[e + 0x0C] << 8);
    uint32_t ba = (uint32_t)rom[e + 0x0D] | ((uint32_t)rom[e + 0x0E] << 8);

    int found = 0;
    uint32_t fo = 0, bo = 0;
    for (unsigned i = 0; i < sizeof banks / sizeof *banks; i++) {
      if (fa < 0x4000 || fa >= 0x8000 || ba < 0x4000 || ba >= 0x8000) continue;
      uint32_t f = banks[i] * 0x4000u + (fa - 0x4000u);
      uint32_t k = banks[i] * 0x4000u + (ba - 0x4000u);
      if (f >= size || k >= size) continue;
      if (rom[f] != dim) continue;
      poison(a); src_reset(&src);
      if (gb_sprite_gen1(a, src_read, &src, f) != GB_SPRITE_OK) continue;
      poison(b); src_reset(&src);
      if (gb_sprite_gen1(b, src_read, &src, k) != GB_SPRITE_OK) continue;
      if (b->wt != 4 || b->ht != 4) continue;
      found++; fo = f; bo = k;
    }
    if (found != 1) { ambiguous++; continue; }

    uint32_t offs[2] = { fo, bo };
    for (int w = 0; w < 2; w++) {
      poison(a); src_reset(&src);
      GbSpriteErr r = gb_sprite_gen1(a, src_read, &src, offs[w]);
      chk(who, "front/back pic decodes", r == GB_SPRITE_OK);
      if (r != GB_SPRITE_OK) continue;
      check_sane(who, a, &src, offs[w]);
      /* determinism */
      poison(b); src_reset(&src);
      chk(who, "second decode is identical",
          gb_sprite_gen1(b, src_read, &src, offs[w]) == GB_SPRITE_OK &&
          a->w == b->w && a->h == b->h &&
          memcmp(a->px, b->px, (size_t)a->w * a->h) == 0);
      crc = crc_pic(crc, a);
      n++;
      if (w == 0) {
        uint32_t one = crc_pic(0, a);
        if (dex == 1)   chk(who, "spot check: dex 1 front", one == spot1);
        if (dex == 25)  chk(who, "spot check: dex 25 front", one == spot25);
        if (dex == 151) chk(who, "spot check: dex 151 front", one == spot151);
        if (dex == 1) {
          /* Truncation, on REAL compressed bytes rather than hand-written ones:
           * cut the file short at every byte of this pic and require a refusal
           * that reads nothing past the cut and leaves the bitmap untouched. */
          uint32_t full = a->consumed, ref = 0;
          for (uint32_t cut = 1; cut < full; cut++) {
            Src t = { rom, size, offs[0] + cut, 0, 0, 0 };
            poison(b);
            if (gb_sprite_gen1(b, src_read, &t, offs[0]) != GB_SPRITE_OK) ref++;
            chk(who, "truncated real pic reads nothing past the cut",
                t.hi <= offs[0] + cut);
          }
          chk(who, "every truncation of a real pic is refused", ref == full - 1);
        }
      }
    }
  }
  chk(who, "every species resolved to exactly one pic bank", ambiguous == 0);
  chk(who, "decoded the expected number of pics", n == want_pics);
  if (crc != want_crc)
    printf("   [%s] CRC32 %08X, oracle says %08X (%d pics)\n", who, crc, want_crc, n);
  chk(who, "aggregate CRC32 matches the art-verified oracle", crc == want_crc);
  printf("   %-8s %3d pics, CRC32 %08X  (base-stats @0x%05lX)\n", who, n, crc, base);
  free(a); free(b);
}

/* =========================================================== Gen-2 sweep ==
 * Gen 2 has a real pointer table, so the sweep follows it exactly as the game
 * does (engine/gfx/load_pics.asm GetFrontpicPointer).
 *   - PokemonPicPointers is 251 entries of { bank, addr16 } x { front, back } and
 *     starts at $4000 of its bank ("org $4000" in layout.link). It is found by
 *     structure: every address either sits in $4000..$7FFF or is the $FFFF
 *     sentinel (Unown's own row, which points at its own table instead). Unique.
 *   - the geometry is the base-stats PicSize byte at +0x11 of the 32-byte entry;
 *     that table is found by the same 1,2,3,... first-byte structure. Backs are
 *     always 6x6.
 *   - the stored bank is BANK(pic) - PICS_FIX and has to be put back:
 *     Crystal adds $36 flat (engine/gfx/load_pics.asm:250, its .PicsBanks table is
 *     the identity); Gold is the identity apart from three rows spelled out in its
 *     own FixPicBank comment ($13 -> Pics 12, $14 -> Pics 13, $1f -> Pics 14).
 *     Those, and the Unown table's bank, are the only remembered numbers in this
 *     file — and a single wrong one moves the aggregate CRC.
 */
#define G2E 32

static long g2_find_base(const uint8_t *rom, uint32_t size) {
  long hit = -1;
  for (uint32_t o = 0; o + G2E * 251 <= size; o++) {
    if (rom[o] != 1) continue;
    int ok = 1;
    for (uint32_t i = 1; i < 251 && ok; i++) if (rom[o + i * G2E] != i + 1) ok = 0;
    if (ok) { if (hit >= 0) return -2; hit = (long)o; }
  }
  return hit;
}
static long g2_find_ptr_bank(const uint8_t *rom, uint32_t size) {
  long hit = -1;
  for (uint32_t bk = 0; (bk + 1) * 0x4000u <= size; bk++) {
    uint32_t t = bk * 0x4000u;
    if (t + 251 * 6 > size) break;
    int good = 0, bad = 0;
    for (uint32_t i = 0; i < 251; i++) {
      for (uint32_t w = 0; w < 2; w++) {
        uint32_t a = (uint32_t)rom[t + i * 6 + w * 3 + 1] |
                     ((uint32_t)rom[t + i * 6 + w * 3 + 2] << 8);
        if (a >= 0x4000 && a < 0x8000) good++;
        else if (a != 0xFFFF) bad++;
      }
    }
    if (!bad && good >= 500) { if (hit >= 0) return -2; hit = (long)bk; }
  }
  return hit;
}

typedef unsigned (*BankFix)(unsigned stored);
static unsigned fix_gold(unsigned sb) {
  if (sb == 0x13) return 0x1F;
  if (sb == 0x14) return 0x20;
  if (sb == 0x1F) return 0x2E;
  return sb;
}
static unsigned fix_crystal(unsigned sb) { return sb + 0x36; }

static void sweep_gen2(const char *who, const uint8_t *rom, uint32_t size,
                       BankFix fix, unsigned unown_bank, int want_pics,
                       uint32_t want_crc, uint32_t spot1, uint32_t spot25,
                       uint32_t spot251) {
  Src src = { rom, size, 0, 0, 0, 0 };
  GbSprite *a = (GbSprite *)malloc(sizeof *a);
  GbSprite *b = (GbSprite *)malloc(sizeof *b);

  long base = g2_find_base(rom, size);
  long pbk = g2_find_ptr_bank(rom, size);
  chk(who, "base-stats table found, and uniquely", base >= 0);
  chk(who, "PokemonPicPointers bank found, and uniquely", pbk >= 0);
  if (base < 0 || pbk < 0) { free(a); free(b); return; }

  uint32_t crc = 0;
  int n = 0;
  for (int tab = 0; tab < 2; tab++) {
    uint32_t t = (tab ? unown_bank : (uint32_t)pbk) * 0x4000u;
    int count = tab ? 26 : 251;
    for (int i = 0; i < count; i++) {
      /* every Unown letter shares species 201's PicSize */
      unsigned pic = rom[base + (long)(tab ? 200 : i) * G2E + 0x11];
      for (int w = 0; w < 2; w++) {
        uint32_t e = t + (uint32_t)i * 6u + (uint32_t)w * 3u;
        unsigned sb = rom[e];
        uint32_t addr = (uint32_t)rom[e + 1] | ((uint32_t)rom[e + 2] << 8);
        if (addr < 0x4000 || addr >= 0x8000) continue;   /* $FFFF sentinel row */
        unsigned wt = w ? 6u : (pic & 0xFu);
        uint32_t off = fix(sb) * 0x4000u + (addr - 0x4000u);
        if (off >= size) { chk(who, "pic offset inside the ROM", 0); continue; }

        poison(a); src_reset(&src);
        GbSpriteErr r = gb_sprite_gen2(a, src_read, &src, off, wt, wt);
        chk(who, "pic decodes", r == GB_SPRITE_OK);
        if (r != GB_SPRITE_OK) continue;
        check_sane(who, a, &src, off);
        poison(b); src_reset(&src);
        chk(who, "second decode is identical",
            gb_sprite_gen2(b, src_read, &src, off, wt, wt) == GB_SPRITE_OK &&
            memcmp(a->px, b->px, (size_t)a->w * a->h) == 0);
        crc = crc_pic(crc, a);
        n++;
        if (!tab && w == 0) {
          uint32_t one = crc_pic(0, a);
          if (i == 0)   chk(who, "spot check: species 1 front", one == spot1);
          if (i == 24)  chk(who, "spot check: species 25 front", one == spot25);
          if (i == 250) chk(who, "spot check: species 251 front", one == spot251);
        }
      }
    }
  }
  chk(who, "decoded the expected number of pics", n == want_pics);
  if (crc != want_crc)
    printf("   [%s] CRC32 %08X, oracle says %08X (%d pics)\n", who, crc, want_crc, n);
  chk(who, "aggregate CRC32 matches the art-verified oracle", crc == want_crc);
  printf("   %-8s %3d pics, CRC32 %08X  (base-stats @0x%05lX, pointers bank 0x%02lX)\n",
         who, n, crc, base, pbk);
  free(a); free(b);
}

/* ==================================================== synthetic / negative */

/* A hand-built, legal Gen-1 stream. PokeDNA ships no Nintendo art, so the
 * truncation and mutation checks are fed bits written HERE, not lifted out of a
 * cartridge: a 2x2 pic whose two planes are each 128 literal 2-bit groups.
 *
 * Layout (pokered/home/uncompress.asm): geometry byte, one bit choosing which
 * buffer takes the first chunk, the chunk, the 1-2 mode bits, the second chunk.
 * Opening a chunk with a 1 bit means "literal groups follow"; a group of 00 would
 * end the run, so every group here is non-zero. */
typedef struct { uint8_t *p; uint32_t n; unsigned bit; } Bw;
static void bw_bit(Bw *w, unsigned v) {
  if (w->bit == 0) { w->p[w->n] = 0; w->bit = 8; }
  w->bit--;
  if (v) w->p[w->n] |= (uint8_t)(1u << w->bit);
  if (w->bit == 0) w->n++;
}
static uint32_t make_gen1_stream(uint8_t *dst, unsigned wt, unsigned ht,
                                 uint32_t seed) {
  Bw w = { dst, 0, 0 };
  dst[w.n++] = (uint8_t)((wt << 4) | ht);
  bw_bit(&w, 0);                                  /* first chunk -> buffer 1    */
  for (int chunk = 0; chunk < 2; chunk++) {
    if (chunk == 1) bw_bit(&w, 0);                /* unpacking mode 0           */
    bw_bit(&w, 1);                                /* literal groups follow      */
    for (uint32_t i = 0; i < wt * 4u * ht * 8u; i++) {
      seed = seed * 1103515245u + 12345u;
      unsigned g = 1u + ((seed >> 16) % 3u);      /* 1..3: never the 00 escape  */
      bw_bit(&w, (g >> 1) & 1u);
      bw_bit(&w, g & 1u);
    }
  }
  return w.bit ? w.n + 1 : w.n;
}

static uint32_t rnd(uint32_t *s) {   /* xorshift32, so the fuzz is reproducible */
  uint32_t x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *s = x;
}

static void synthetic(void) {
  const char *who = "synthetic";
  GbSprite *s = (GbSprite *)malloc(sizeof *s);
  uint8_t buf[GB_SPRITE_MAX_INPUT];
  Src src = { buf, sizeof buf, 0, 0, 0, 0 };

  /* --- argument checking --------------------------------------------------- */
  chk(who, "NULL out is refused",
      gb_sprite_gen1(NULL, src_read, &src, 0) == GB_SPRITE_E_ARGS);
  chk(who, "NULL reader is refused",
      gb_sprite_gen1(s, NULL, &src, 0) == GB_SPRITE_E_ARGS);
  chk(who, "gen2 geometry 0 is refused",
      gb_sprite_gen2(s, src_read, &src, 0, 0, 6) == GB_SPRITE_E_ARGS);
  chk(who, "gen2 geometry 8 is refused",
      gb_sprite_gen2(s, src_read, &src, 0, 8, 6) == GB_SPRITE_E_ARGS);

  /* --- Gen-1 header rejection ---------------------------------------------- */
  memset(buf, 0, sizeof buf);
  poison(s); src_reset(&src);
  chk(who, "header 0x00 (0x0 tiles) is refused",
      gb_sprite_gen1(s, src_read, &src, 0) == GB_SPRITE_E_HEADER);
  memset(buf, 0xFF, sizeof buf);
  poison(s); src_reset(&src);
  chk(who, "an erased/0xFF blob is refused (0xF = 15 tiles)",
      gb_sprite_gen1(s, src_read, &src, 0) == GB_SPRITE_E_HEADER);
  buf[0] = 0x87;                                  /* 8x7 — width out of range   */
  poison(s); src_reset(&src);
  chk(who, "header with a 8-tile side is refused",
      gb_sprite_gen1(s, src_read, &src, 0) == GB_SPRITE_E_HEADER);

  /* --- the zero-run length code cannot run off its table --------------------
   * LengthEncodingOffsetList has 16 entries (uncompress.asm:273), so a run whose
   * unary length prefix is 16 ones is not representable; the real game would
   * index past the table. Build exactly that: valid 7x7 geometry, then the buffer
   * select bit (0), then a 0 to open in zero-run mode, then nothing but 1 bits. */
  buf[0] = 0x77;
  buf[1] = 0x3F;
  memset(buf + 2, 0xFF, sizeof buf - 2);
  poison(s); src_reset(&src);
  GbSpriteErr r = gb_sprite_gen1(s, src_read, &src, 0);
  chk(who, "a 16-deep run-length code is refused, not chased",
      r == GB_SPRITE_E_DATA);
  chk(who, "and it stopped inside the input budget",
      src.hi <= GB_SPRITE_MAX_INPUT);
  chk(who, "...having written no pixels at all", s->px[0] == POISON);

  /* The boundary has to be exactly 16, not "some big number that terminates".
   * A 15-deep prefix is the last legal code (offset 2^16-1) and must still work;
   * a well-formed 16-deep one has no table row and must be refused even though
   * nothing else about the stream is wrong. Both are complete 1x1 pics. */
  for (int depth = 15; depth <= 16; depth++) {
    Bw w = { buf, 0, 0 };
    buf[w.n++] = 0x11;                       /* 1x1 tile: 32 groups per chunk   */
    bw_bit(&w, 0);                           /* first chunk -> buffer 1         */
    for (int chunk = 0; chunk < 2; chunk++) {
      if (chunk == 1) bw_bit(&w, 0);         /* unpacking mode 0                */
      bw_bit(&w, 0);                         /* open in zero-run mode           */
      for (int i = 0; i < depth; i++) bw_bit(&w, 1);
      bw_bit(&w, 0);
      for (int i = 0; i <= depth; i++) bw_bit(&w, 0);  /* value 0 -> a long run */
    }
    Src bs = { buf, w.bit ? w.n + 1 : w.n, 0, 0, 0, 0 };
    poison(s);
    GbSpriteErr d = gb_sprite_gen1(s, src_read, &bs, 0);
    if (depth == 15)
      chk(who, "a 15-deep run-length code is still accepted",
          d == GB_SPRITE_OK && s->wt == 1 && s->ht == 1);
    else
      chk(who, "a well-formed 16-deep run-length code is refused",
          d == GB_SPRITE_E_DATA);
  }

  /* --- a legal hand-built stream, and every truncation of it ---------------- */
  static uint8_t made[GB_SPRITE_MAX_INPUT];
  uint32_t mlen = make_gen1_stream(made, 2, 2, 0xC0FFEEu);
  Src ms = { made, mlen, 0, 0, 0, 0 };
  poison(s); src_reset(&ms);
  chk(who, "the hand-built 2x2 stream decodes",
      gb_sprite_gen1(s, src_read, &ms, 0) == GB_SPRITE_OK &&
      s->wt == 2 && s->ht == 2 && s->w == 16 && s->h == 16);
  chk(who, "...consuming exactly the bytes it was written into", s->consumed == mlen);
  check_sane(who, s, &ms, 0);
  uint32_t whole = crc_pic(0, s);

  int refused = 0;
  for (uint32_t cut = 1; cut < mlen; cut++) {
    Src t = { made, mlen, cut, 0, 0, 0 };
    poison(s);
    GbSpriteErr e = gb_sprite_gen1(s, src_read, &t, 0);
    if (e != GB_SPRITE_OK) {
      refused++;
      for (size_t i = 0; i < GB_SPRITE_MAX_PX; i++)
        if (s->px[i] != POISON) { chk(who, "a refused truncation wrote pixels", 0); break; }
    } else {
      check_sane(who, s, &t, 0);
    }
    chk(who, "a truncated pic never reads past the cut", t.hi <= cut);
  }
  chk(who, "every truncation of a legal stream is refused", refused == (int)mlen - 1);

  /* --- the CRC check has teeth --------------------------------------------- */
  poison(s); src_reset(&ms);
  gb_sprite_gen1(s, src_read, &ms, 0);
  s->px[(size_t)s->w * s->h / 2] ^= 1;
  chk(who, "one flipped pixel changes the CRC", crc_pic(0, s) != whole);

  /* --- Gen-2 rejection ----------------------------------------------------- */
  memset(buf, 0xFF, sizeof buf);               /* immediate LZ_END = no output  */
  poison(s); src_reset(&src);
  chk(who, "an lz3 stream that ends before the picture is refused",
      gb_sprite_gen2(s, src_read, &src, 0, 6, 6) == GB_SPRITE_E_DATA);

  /* A REPEAT from output byte 0 with nothing written yet. The rest of the stream
   * is deliberately VALID and long enough to fill a 6x6 picture on its own, so a
   * decoder that let the bad reference through would return a clean success --
   * only a decoder that checks the reference can fail here. */
  buf[0] = 0x80; buf[1] = 0x00; buf[2] = 0x00;
  for (unsigned i = 0; i < 6 * 6 * 16 / 32 + 1; i++) buf[3 + i] = 0x7F;  /* ZERO x32 */
  buf[3 + 6 * 6 * 16 / 32 + 1] = 0xFF;
  poison(s); src_reset(&src);
  chk(who, "an lz3 back-reference outside the output is refused",
      gb_sprite_gen2(s, src_read, &src, 0, 6, 6) == GB_SPRITE_E_DATA);

  buf[0] = 0x1F; memset(buf + 1, 0x11, 31);     /* 32 literal bytes, then EOF   */
  Src shortsrc = { buf, 33, 0, 0, 0, 0 };
  poison(s); src_reset(&shortsrc);
  chk(who, "an lz3 stream that runs out of input is refused",
      gb_sprite_gen2(s, src_read, &shortsrc, 0, 6, 6) != GB_SPRITE_OK);

  /* --- the input budget really is what stops a hostile stream ---------------
   * lz3's worst ratio is a LZ_LONG repeat: two command bytes plus a two-byte
   * forward offset to move ONE byte. 784 output bytes then need 3134 input, so a
   * blob built that way must die on GB_SPRITE_MAX_INPUT even though the "file"
   * still has bytes left. Raise the budget and this decode succeeds instead. */
  {
    static uint8_t big[4096];
    big[0] = 0x00; big[1] = 0x5A;               /* one literal byte to copy from */
    for (uint32_t i = 2; i + 3 < sizeof big; i += 4) {
      big[i] = 0xF0;                            /* LZ_LONG -> LZ_REPEAT          */
      big[i + 1] = 0x00;                        /* length 1                      */
      big[i + 2] = 0x00; big[i + 3] = 0x00;     /* 15-bit forward offset 0       */
    }
    Src hostile = { big, sizeof big, 0, 0, 0, 0 };
    poison(s); src_reset(&hostile);
    chk(who, "a hostile lz3 stream dies on the input budget",
        gb_sprite_gen2(s, src_read, &hostile, 0, 7, 7) == GB_SPRITE_E_READ);
    chk(who, "...having read no more than the budget",
        hostile.hi <= GB_SPRITE_MAX_INPUT && hostile.hi > GB_SPRITE_MAX_INPUT / 2);
  }

  /* --- fuzz: garbage must terminate inside the budget, never spill ---------- */
  uint32_t seed = 0x13579BDFu;
  int g1ok = 0, g2ok = 0;
  for (int it = 0; it < 100000; it++) {
    for (unsigned i = 0; i < 256; i += 4) {
      uint32_t v = rnd(&seed);
      buf[i] = (uint8_t)v; buf[i + 1] = (uint8_t)(v >> 8);
      buf[i + 2] = (uint8_t)(v >> 16); buf[i + 3] = (uint8_t)(v >> 24);
    }
    Src f = { buf, 256, 0, 0, 0, 0 };
    poison(s);
    if (gb_sprite_gen1(s, src_read, &f, 0) == GB_SPRITE_OK) {
      g1ok++;
      check_sane(who, s, &f, 0);
    } else {
      for (size_t i = 0; i < GB_SPRITE_MAX_PX; i++)
        if (s->px[i] != POISON) { chk(who, "a rejected gen1 decode wrote pixels", 0); break; }
    }
    src_reset(&f);
    poison(s);
    if (gb_sprite_gen2(s, src_read, &f, 0, 5, 5) == GB_SPRITE_OK) {
      g2ok++;
      check_sane(who, s, &f, 0);
    }
    chk(who, "fuzz never reads past the budget", f.hi <= GB_SPRITE_MAX_INPUT);
  }
  printf("   fuzz     100000 random streams: %d decoded as gen1, %d as gen2, "
         "no spill, no over-read\n", g1ok, g2ok);
  free(s);
}

/* ---------------------------------------------------------------- driver -- */
int main(void) {
  crc_init();
  chk("crc32", "matches zlib on a known vector",
      crc_upd(0, "123456789", 9) == 0xCBF43926u);

  synthetic();

  const char *dir = getenv("GBROMS");
  if (!dir) dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb";

  struct { const char *file, *title, *who; } gen1[] = {
    { "Red.gb",    "POKEMON RED",    "Red"    },
    { "Yellow.gb", "POKEMON YELLOW", "Yellow" },
  };
  /* oracle values, in the same order as gen1[] */
  const int   g1n[]  = { 302, 302 };
  const uint32_t g1c[] = { 0x6D8FFFEDu, 0xBBA4BB04u };
  const uint32_t g1s1[]   = { 0xE811278Cu, 0x8C3A4858u };
  const uint32_t g1s25[]  = { 0x7CFDC787u, 0x1BD37873u };
  const uint32_t g1s151[] = { 0xE092FEFAu, 0x3190D577u };

  int skipped = 0;
  for (unsigned i = 0; i < 2; i++) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, gen1[i].file);
    uint32_t size = 0;
    uint8_t *rom = load(path, &size);
    if (!rom) { printf("   %-8s SKIP (no %s)\n", gen1[i].who, path); skipped++; continue; }
    if (!title_is(rom, gen1[i].title)) {
      printf("   %-8s SKIP (header is not %s)\n", gen1[i].who, gen1[i].title);
      skipped++; free(rom); continue;
    }
    sweep_gen1(gen1[i].who, rom, size, g1n[i], g1c[i], g1s1[i], g1s25[i], g1s151[i]);
    free(rom);
  }

  struct { const char *file, *title, *who; BankFix fix; unsigned ubank; } gen2[] = {
    /* Unown table banks: pokegold layout.link ROMX $1f, pokecrystal ROMX $49 */
    { "Gold.gbc",    "POKEMON_GLD", "Gold",    fix_gold,    0x1F },
    { "Crystal.gbc", "PM_CRYSTAL",  "Crystal", fix_crystal, 0x49 },
  };
  const int      g2n[]  = { 552, 552 };
  const uint32_t g2c[]  = { 0x0CA38733u, 0x70FEC540u };
  const uint32_t g2s1[]   = { 0x5D948D23u, 0xA63DBCABu };
  const uint32_t g2s25[]  = { 0x9303C2E5u, 0x49A774B1u };
  const uint32_t g2s251[] = { 0x07F871EBu, 0x8C02D346u };

  for (unsigned i = 0; i < 2; i++) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, gen2[i].file);
    uint32_t size = 0;
    uint8_t *rom = load(path, &size);
    if (!rom) { printf("   %-8s SKIP (no %s)\n", gen2[i].who, path); skipped++; continue; }
    if (!title_is(rom, gen2[i].title)) {
      printf("   %-8s SKIP (header is not %s)\n", gen2[i].who, gen2[i].title);
      skipped++; free(rom); continue;
    }
    sweep_gen2(gen2[i].who, rom, size, gen2[i].fix, gen2[i].ubank,
               g2n[i], g2c[i], g2s1[i], g2s25[i], g2s251[i]);
    free(rom);
  }

  printf("\nhost_gbcodec_test: %d checks, %d failed, %d ROM(s) skipped\n",
         checks, fails, skipped);
  return fails ? 1 : 0;
}
