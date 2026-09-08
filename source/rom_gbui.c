/* Gen-1 / Gen-2 trainer-card and bag UI graphics, located BY SHAPE in the
 * user's own Game Boy cartridge -- see rom_gbui.h for the design, the
 * per-signature citations and the fail-closed rules. Pure C: no tonc, no
 * FatFs, no GBA headers.
 *
 * THIS COMMIT: the five Gen-1 locators (G1-F/T/C/B/P), the tile/glyph
 * expanders and the .loc cache. Gen 2 (G2-*) lands in the next commit --
 * until then rom_gbui_open() on a Gold/Crystal dump fails closed (gen = 0,
 * ok = 0), which is correct, not a bug: nothing here claims to locate it yet. */
#include "rom_gbui.h"

#include <string.h>

#define GB_BANK   0x4000u
#define GB_WIN_LO 0x4000u
#define GB_WIN_HI 0x8000u

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t fileoff(uint8_t bank, uint16_t addr) {
  if (addr < GB_WIN_LO) return addr;
  return (uint32_t)bank * GB_BANK + (uint32_t)(addr - GB_WIN_LO);
}

/* ------------------------------------------------------------- scan context */

/* Everything a locator needs during open()/open_loc() -- deliberately NOT
 * part of RomGbUi (which retains no scratch pointer at all, see the header's
 * memory note). Local to this file, lives on locate()'s own stack frame. */
typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t size;
  uint8_t  banks;         /* size / 16 KiB, needed by the card-frame bank sweep */
  uint8_t* scratch;
  uint32_t scratch_len;
} Scan;

static int rd(const Scan* s, uint32_t off, void* dst, uint32_t len) {
  if (!s->read || len == 0) return 0;
  if (off >= s->size || len > s->size - off) return 0;
  return s->read(s->ctx, off, dst, len) ? 1 : 0;
}

/* ------------------------------------------------------------- header/id */

#define GB_LOGO_OFF  0x104u
#define GB_LOGO_LEN  0x30u
#define GB_LOGO_FNV  0x016BAD3Fu   /* measured identically on Red, Yellow, Gold, Crystal */

static uint32_t fnv1a(const uint8_t* p, uint32_t n, uint32_t h) {
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

static int parse_header(const Scan* s, uint8_t* out_banks, uint32_t* out_id_hash) {
  uint8_t h[0x50];   /* 0x100..0x14F */
  if (!rd(s, 0x100, h, sizeof h)) return 0;
  if (fnv1a(h + (GB_LOGO_OFF - 0x100), GB_LOGO_LEN, 0x811C9DC5u) != GB_LOGO_FNV) return 0;

  uint8_t x = 0;
  for (uint32_t i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - h[i - 0x100] - 1u);
  if (x != h[0x14D - 0x100]) return 0;

  uint8_t code = h[0x148 - 0x100];
  if (code > 8) return 0;
  if (s->size != (0x8000u << code)) return 0;
  if (s->size % GB_BANK) return 0;
  uint32_t banks = s->size / GB_BANK;
  if (banks > 255u || banks == 0) return 0;

  *out_banks = (uint8_t)banks;
  *out_id_hash = fnv1a(h, sizeof h, 0x811C9DC5u);
  return 1;
}

/* -------------------------------------------------------------- verifiers */

/* All fixed-size structural checks re-read through a bounded local buffer
 * rather than the scan window, which is only valid during the single
 * scan_multi pass. */

static uint32_t distinct_tiles(const Scan* s, uint32_t off, uint8_t bpp, uint32_t ntiles) {
  uint32_t stride = (bpp == 2) ? 16u : 8u;
  uint8_t tiles[96][16];   /* worst case here is 86 (leaders, next commit); 96 headroom */
  if (ntiles > 96) return 0;
  if (off + ntiles * stride > s->size) return 0;
  for (uint32_t i = 0; i < ntiles; i++)
    if (!rd(s, off + i * stride, tiles[i], stride)) return 0;
  uint32_t distinct = 0;
  for (uint32_t i = 0; i < ntiles; i++) {
    int dup = 0;
    for (uint32_t j = 0; j < i; j++)
      if (memcmp(tiles[i], tiles[j], stride) == 0) { dup = 1; break; }
    if (!dup) distinct++;
  }
  return distinct;
}

static int all_blank(const Scan* s, uint32_t off, uint32_t nbytes) {
  uint8_t buf[64];
  uint32_t done = 0;
  while (done < nbytes) {
    uint32_t chunk = nbytes - done; if (chunk > sizeof buf) chunk = sizeof buf;
    if (!rd(s, off + done, buf, chunk)) return 0;
    for (uint32_t i = 0; i < chunk; i++) if (buf[i]) return 0;
    done += chunk;
  }
  return 1;
}

static int any_nonzero(const Scan* s, uint32_t off, uint32_t nbytes) {
  uint8_t buf[64];
  uint32_t done = 0;
  while (done < nbytes) {
    uint32_t chunk = nbytes - done; if (chunk > sizeof buf) chunk = sizeof buf;
    if (!rd(s, off + done, buf, chunk)) return 0;
    for (uint32_t i = 0; i < chunk; i++) if (buf[i]) return 1;
    done += chunk;
  }
  return 0;
}

/* 1024 B = 128 tiles, 1bpp (Gen 1 Font / Gen 2 Font, same charmap layout). */
static int font_verify(const Scan* s, uint32_t off) {
  if (off + 1024 > s->size) return 0;
  uint32_t solid = 0, blank = 0;
  for (uint32_t t = 0; t < 128; t++) {
    uint8_t p[8];
    if (!rd(s, off + t * 8, p, 8)) return 0;
    int all_ff = 1, all_00 = 1;
    for (int i = 0; i < 8; i++) { if (p[i] != 0xFF) all_ff = 0; if (p[i] != 0x00) all_00 = 0; }
    if (all_ff) solid++;
    if (all_00) blank++;
  }
  return solid == 0 && blank <= 40;   /* measured 26-32 blank across the corpus */
}

/* -------------------------------------------------------- pattern matchers */
/* Each cb reads only the scan window (no ROM re-read); `look` is how many
 * window bytes it inspects. Struct-address relations that need only the
 * window (e.g. G1-C's C-A==144) are checked inline here; everything that
 * needs `banks` or a fresh read happens in the try_*() functions below,
 * against the RAW hit offset. */

typedef int (*ScanCb)(const uint8_t* w);

static int g1_font_cb(const uint8_t* w) {
  return w[0]==0xF0 && w[1]==0x40 && w[2]==0xCB && w[3]==0x7F && w[4]==0x20 &&
         w[6]==0x21 && w[9]==0x11 && w[12]==0x01 && w[13]==0x00 && w[14]==0x04 &&
         w[15]==0x3E && w[17]==0xC3;
}
static int g1_textbox_cb(const uint8_t* w) {
  return w[0]==0xF0 && w[1]==0x40 && w[2]==0xCB && w[3]==0x7F && w[4]==0x20 &&
         w[6]==0x21 && w[9]==0x11 && w[10]==0x00 && w[11]==0x96 &&
         w[12]==0x01 && w[13]==0x00 && w[14]==0x02 && w[15]==0x3E && w[17]==0xC3;
}
static int g1_cardframe_cb(const uint8_t* w) {
  if (w[0]!=0x21 || w[3]!=0x11 || w[4]!=0x70 || w[5]!=0x97 || w[6]!=0x01 || w[7]!=0x80 ||
      w[8]!=0x00 || w[9]!=0xC5 || w[10]!=0xCD) return 0;
  if (w[13]!=0x21 || w[16]!=0x11 || w[17]!=0x00 || w[18]!=0x96 || w[19]!=0x01 ||
      w[20]!=0x70 || w[21]!=0x01 || w[22]!=0xCD) return 0;
  if (w[25]!=0xC1 || w[26]!=0x21 || w[29]!=0x11 || w[30]!=0x80 || w[31]!=0x8D || w[32]!=0xCD) return 0;
  uint16_t a = rd16(w+1), c = rd16(w+14), e = rd16(w+27);
  return (uint16_t)(c - a) == 144 && (uint16_t)(e - a) == 512;
}
static int g1_badges_cb(const uint8_t* w) {
  static const uint8_t sig[8] = {0x20,0x28,0x30,0x38,0x40,0x48,0x50,0x58};
  return memcmp(w, sig, 8) == 0;
}
static int g1_playerpic_cb(const uint8_t* w) {
  return w[0]==0x11 && w[3]==0x01 && w[4]==0x01;
}

enum { J_G1_FONT = 0, J_G1_TEXTBOX, J_G1_CARDFRAME, J_G1_BADGES, J_G1_PLAYERPIC, J_COUNT };

#define SCAN_MAX_HITS 128u   /* the player-pic anchor alone raw-hits ~70-75 */

typedef struct {
  ScanCb   cb;
  uint32_t look;
  uint32_t off[SCAN_MAX_HITS];
  uint32_t n;                 /* > SCAN_MAX_HITS means "too many, fail closed" */
} ScanJob;

static int scan_multi(const Scan* s, ScanJob* jobs, uint32_t njobs) {
  uint8_t* w = s->scratch;
  uint32_t cap = s->scratch_len;
  uint32_t look = 0;
  for (uint32_t j = 0; j < njobs; j++) { jobs[j].n = 0; if (jobs[j].look > look) look = jobs[j].look; }
  if (!w || cap < look + 16u || s->size < look) return 0;
  uint32_t step = cap - look + 1u;
  for (uint32_t base = 0; base + look <= s->size; base += step) {
    uint32_t n = s->size - base; if (n > cap) n = cap;
    if (!rd(s, base, w, n)) return 0;
    uint32_t lim = n - look;
    for (uint32_t i = 0; i <= lim; i++) {
      const uint8_t* p = w + i;
      for (uint32_t j = 0; j < njobs; j++) {
        if (!jobs[j].cb(p)) continue;
        if (jobs[j].n < SCAN_MAX_HITS) jobs[j].off[jobs[j].n] = base + i;
        jobs[j].n++;
      }
    }
  }
  return 1;
}

/* --------------------------------------------------------- try_* (Gen 1) */

static int try_font(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[18];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 7);
  uint8_t bank = buf[16];
  uint32_t off = fileoff(bank, addr);
  if (!font_verify(s, off)) return 0;
  *out = off;
  return 1;
}
static int try_textbox(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[18];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 7);
  uint8_t bank = buf[16];
  uint32_t off = fileoff(bank, addr);
  if (off + 512 > s->size) return 0;
  if (distinct_tiles(s, off, 2, 32) != 32) return 0;
  *out = off;
  return 1;
}
static int try_cardframe(const Scan* s, uint32_t hit, uint32_t* out) {
  uint8_t buf[33];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 1);
  uint32_t good = 0, good_off = 0;
  for (uint32_t bank = 0; bank < (uint32_t)s->banks; bank++) {
    uint32_t off = fileoff((uint8_t)bank, addr);
    if (off + 640 > s->size) continue;
    if (distinct_tiles(s, off, 2, 9) != 9) continue;
    if (!all_blank(s, off + 144, 352)) continue;
    if (!any_nonzero(s, off + 496, 16)) continue;
    if (distinct_tiles(s, off + 512, 2, 8) < 7) continue;
    good++; good_off = off;
    if (good > 1) return 0;
  }
  if (good != 1) return 0;
  *out = good_off;
  return 1;
}
static int try_badges(const Scan* s, uint32_t hit, uint32_t* out) {
  uint32_t off = hit + 8;
  if (off + 1024 > s->size) return 0;
  if (distinct_tiles(s, off, 2, 64) != 64) return 0;
  *out = off;
  return 1;
}
static int try_playerpic(const Scan* s, uint32_t hit, uint32_t* out, uint8_t* out_bank) {
  uint8_t buf[6];
  if (!rd(s, hit, buf, sizeof buf)) return 0;
  uint16_t addr = rd16(buf + 1);
  uint8_t bank = buf[5];
  if (addr < GB_WIN_LO || addr >= GB_WIN_HI) return 0;
  if (bank == 0 || (uint32_t)bank * GB_BANK >= s->size) return 0;
  uint32_t off = fileoff(bank, addr);
  uint8_t first;
  if (!rd(s, off, &first, 1)) return 0;
  if (first != 0x77) return 0;
  *out = off; *out_bank = bank;
  return 1;
}

/* --------------------------------------------------------------- locate() */

typedef struct { uint32_t off[ROM_GBUI_OFF_COUNT]; uint8_t gen, playerpic_bank; } LocResult;

static int locate(const Scan* s, LocResult* r) {
  memset(r, 0, sizeof *r);

  ScanJob jobs[J_COUNT];
  memset(jobs, 0, sizeof jobs);
  jobs[J_G1_FONT     ].cb = g1_font_cb;      jobs[J_G1_FONT     ].look = 18;
  jobs[J_G1_TEXTBOX  ].cb = g1_textbox_cb;   jobs[J_G1_TEXTBOX  ].look = 18;
  jobs[J_G1_CARDFRAME].cb = g1_cardframe_cb; jobs[J_G1_CARDFRAME].look = 33;
  jobs[J_G1_BADGES   ].cb = g1_badges_cb;    jobs[J_G1_BADGES   ].look = 8;
  jobs[J_G1_PLAYERPIC].cb = g1_playerpic_cb; jobs[J_G1_PLAYERPIC].look = 6;
  if (!scan_multi(s, jobs, J_COUNT)) return 0;

  if (jobs[J_G1_FONT].n != 1) return 0;
  uint32_t font;
  if (!try_font(s, jobs[J_G1_FONT].off[0], &font)) return 0;

  if (jobs[J_G1_TEXTBOX].n != 1) return 0;
  uint32_t textbox;
  if (!try_textbox(s, jobs[J_G1_TEXTBOX].off[0], &textbox)) return 0;

  if (jobs[J_G1_CARDFRAME].n != 1) return 0;
  uint32_t cardframe;
  if (!try_cardframe(s, jobs[J_G1_CARDFRAME].off[0], &cardframe)) return 0;

  if (jobs[J_G1_BADGES].n != 1) return 0;
  uint32_t badges;
  if (!try_badges(s, jobs[J_G1_BADGES].off[0], &badges)) return 0;

  if (jobs[J_G1_PLAYERPIC].n == 0 || jobs[J_G1_PLAYERPIC].n > SCAN_MAX_HITS) return 0;
  uint32_t pp = 0, pp_found = 0; uint8_t pp_bank = 0;
  for (uint32_t i = 0; i < jobs[J_G1_PLAYERPIC].n; i++) {
    uint32_t cand; uint8_t bk;
    if (try_playerpic(s, jobs[J_G1_PLAYERPIC].off[i], &cand, &bk)) {
      pp_found++; pp = cand; pp_bank = bk;
    }
  }
  if (pp_found != 1) return 0;

  r->off[ROM_GBUI_OFF_FONT] = font;
  r->off[ROM_GBUI_OFF_TEXTBOX] = textbox;
  r->off[ROM_GBUI_OFF_CARDFRAME] = cardframe;
  r->off[ROM_GBUI_OFF_BADGES] = badges;
  r->off[ROM_GBUI_OFF_PLAYERPIC] = pp;
  r->playerpic_bank = pp_bank;
  r->gen = ROM_GBUI_GEN1;
  return 1;
}

/* ------------------------------------------------------------------- open */

static void fill_from_result(RomGbUi* gu, const LocResult* r) {
  gu->gen = r->gen;
  gu->font        = r->off[ROM_GBUI_OFF_FONT];
  gu->textbox     = r->off[ROM_GBUI_OFF_TEXTBOX];
  gu->cardframe   = r->off[ROM_GBUI_OFF_CARDFRAME];
  gu->badges      = r->off[ROM_GBUI_OFF_BADGES];
  gu->leaders     = r->off[ROM_GBUI_OFF_LEADERS];
  gu->playerpic   = r->off[ROM_GBUI_OFF_PLAYERPIC];
  gu->frames      = r->off[ROM_GBUI_OFF_FRAMES];
  gu->fontextra   = r->off[ROM_GBUI_OFF_FONTEXTRA];
  gu->cardpic_m   = r->off[ROM_GBUI_OFF_CARDPIC_M];
  gu->cardpic_f   = r->off[ROM_GBUI_OFF_CARDPIC_F];
  gu->cardgfx     = r->off[ROM_GBUI_OFF_CARDGFX];
  gu->pack_m      = r->off[ROM_GBUI_OFF_PACK_M];
  gu->pack_f      = r->off[ROM_GBUI_OFF_PACK_F];
  gu->playerpic_bank = r->playerpic_bank;
}

int rom_gbui_open(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                  uint8_t* scratch, uint32_t scratch_len) {
  if (!gu) return 0;
  memset(gu, 0, sizeof *gu);
  gu->read = read; gu->ctx = ctx; gu->size = size;
  if (!read || !scratch || scratch_len < ROM_GBUI_SCRATCH_MIN) return 0;

  Scan s; memset(&s, 0, sizeof s);
  s.read = read; s.ctx = ctx; s.size = size; s.scratch = scratch; s.scratch_len = scratch_len;

  uint8_t banks; uint32_t id_hash;
  if (!parse_header(&s, &banks, &id_hash)) return 0;
  s.banks = banks;
  gu->banks = banks; gu->id_hash = id_hash;

  LocResult r;
  if (!locate(&s, &r)) { gu->gen = ROM_GBUI_NONE; return 0; }
  fill_from_result(gu, &r);
  gu->ok = 1;
  return 1;
}

void rom_gbui_save_loc(const RomGbUi* gu, RomGbUiLoc* out) {
  if (!gu || !out) return;
  memset(out, 0, sizeof *out);
  out->id_hash = gu->id_hash;
  out->size    = gu->size;
  out->gen     = gu->gen;
  out->off[ROM_GBUI_OFF_FONT]      = gu->font;
  out->off[ROM_GBUI_OFF_TEXTBOX]   = gu->textbox;
  out->off[ROM_GBUI_OFF_CARDFRAME] = gu->cardframe;
  out->off[ROM_GBUI_OFF_BADGES]    = gu->badges;
  out->off[ROM_GBUI_OFF_LEADERS]   = gu->leaders;
  out->off[ROM_GBUI_OFF_PLAYERPIC] = gu->playerpic;
  out->off[ROM_GBUI_OFF_FRAMES]    = gu->frames;
  out->off[ROM_GBUI_OFF_FONTEXTRA] = gu->fontextra;
  out->off[ROM_GBUI_OFF_CARDPIC_M] = gu->cardpic_m;
  out->off[ROM_GBUI_OFF_CARDPIC_F] = gu->cardpic_f;
  out->off[ROM_GBUI_OFF_CARDGFX]   = gu->cardgfx;
  out->off[ROM_GBUI_OFF_PACK_M]    = gu->pack_m;
  out->off[ROM_GBUI_OFF_PACK_F]    = gu->pack_f;
}

/* Re-validate a cached loc against THIS ROM without a full rescan. Every
 * field gets the same structural verifier locate() itself used (font,
 * textbox, cardframe, badges); the player pic gets the same one cheap check
 * locate() used (its own 0x77 header byte) rather than a full decode.
 * Anything that fails falls back to rom_gbui_open()'s full scan -- never a
 * partial accept. Gen 2 lands in the next commit. */
static int revalidate_loc(const Scan* s, const RomGbUiLoc* loc) {
  if (loc->gen != ROM_GBUI_GEN1) return 0;
  uint32_t font = loc->off[ROM_GBUI_OFF_FONT];
  uint32_t textbox = loc->off[ROM_GBUI_OFF_TEXTBOX];
  uint32_t cardframe = loc->off[ROM_GBUI_OFF_CARDFRAME];
  uint32_t badges = loc->off[ROM_GBUI_OFF_BADGES];
  uint32_t pp = loc->off[ROM_GBUI_OFF_PLAYERPIC];
  if (!font_verify(s, font)) return 0;
  if (textbox + 512 > s->size || distinct_tiles(s, textbox, 2, 32) != 32) return 0;
  if (cardframe + 640 > s->size) return 0;
  if (distinct_tiles(s, cardframe, 2, 9) != 9) return 0;
  if (!all_blank(s, cardframe + 144, 352)) return 0;
  if (!any_nonzero(s, cardframe + 496, 16)) return 0;
  if (distinct_tiles(s, cardframe + 512, 2, 8) < 7) return 0;
  if (badges + 1024 > s->size || distinct_tiles(s, badges, 2, 64) != 64) return 0;
  uint8_t first;
  if (!rd(s, pp, &first, 1) || first != 0x77) return 0;
  return 1;
}

int rom_gbui_open_loc(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                      uint8_t* scratch, uint32_t scratch_len,
                      const RomGbUiLoc* loc) {
  if (!gu) return 0;
  memset(gu, 0, sizeof *gu);
  gu->read = read; gu->ctx = ctx; gu->size = size;
  if (!read || !scratch || scratch_len < ROM_GBUI_SCRATCH_MIN) return 0;

  Scan s; memset(&s, 0, sizeof s);
  s.read = read; s.ctx = ctx; s.size = size; s.scratch = scratch; s.scratch_len = scratch_len;

  uint8_t banks; uint32_t id_hash;
  if (!parse_header(&s, &banks, &id_hash)) return 0;
  s.banks = banks;

  if (loc && loc->id_hash == id_hash && loc->size == size && revalidate_loc(&s, loc)) {
    gu->banks = banks; gu->id_hash = id_hash; gu->gen = loc->gen;
    gu->font        = loc->off[ROM_GBUI_OFF_FONT];
    gu->textbox     = loc->off[ROM_GBUI_OFF_TEXTBOX];
    gu->cardframe   = loc->off[ROM_GBUI_OFF_CARDFRAME];
    gu->badges      = loc->off[ROM_GBUI_OFF_BADGES];
    gu->playerpic   = loc->off[ROM_GBUI_OFF_PLAYERPIC];
    gu->playerpic_bank = (uint8_t)(gu->playerpic / GB_BANK);
    gu->ok = 1;
    return 1;
  }

  return rom_gbui_open(gu, read, ctx, size, scratch, scratch_len);
}

/* ------------------------------------------------------------ tile/glyph */

#define GB8_TO_RGB15(v) ((uint16_t)((((v) >> 3) & 0x1Fu) | \
                          ((((v) >> 3) & 0x1Fu) << 5) | ((((v) >> 3) & 0x1Fu) << 10)))

static const uint16_t DMG_SHADE[4] = {
  GB8_TO_RGB15(0xF8), GB8_TO_RGB15(0xA8), GB8_TO_RGB15(0x58), GB8_TO_RGB15(0x10)
};

int rom_gbui_tile(RomGbUi* gu, uint32_t off, uint32_t index, uint8_t bpp,
                  int colmajor_w_tiles, uint16_t out[64]) {
  if (!gu || !gu->ok || !out) return 0;
  if (bpp != 1 && bpp != 2) return 0;
  if (colmajor_w_tiles < 0) return 0;   /* validated only, see rom_gbui.h    */
  uint32_t stride = (bpp == 2) ? 16u : 8u;
  uint32_t toff = off + index * stride;
  if (toff < off) return 0;             /* overflow guard                   */
  if (toff >= gu->size || stride > gu->size - toff) return 0;

  uint8_t buf[16];
  if (!gu->read(gu->ctx, toff, buf, stride)) return 0;

  for (uint32_t y = 0; y < 8; y++) {
    uint8_t lo = (bpp == 2) ? buf[y * 2] : buf[y];
    uint8_t hi = (bpp == 2) ? buf[y * 2 + 1] : 0;
    for (uint32_t x = 0; x < 8; x++) {
      uint32_t b = 7u - x;
      uint8_t idx = (uint8_t)(((lo >> b) & 1u) | (((hi >> b) & 1u) << 1));
      out[y * 8 + x] = DMG_SHADE[idx];
    }
  }
  return 1;
}

int rom_gbui_glyph(RomGbUi* gu, uint8_t ch, uint16_t out[64]) {
  if (!gu || !gu->ok || !gu->font) return 0;
  if (ch < 0x80u) return 0;
  uint32_t tile = (uint32_t)(ch - 0x80u);
  return rom_gbui_tile(gu, gu->font, tile, 1, 0, out);
}
