/* pdna_gbscreen.c — see pdna_gbscreen.h. The shared GB-screen shell (U2a).
 *
 * FILE LAYOUT / PURE-C BOUNDARY: everything up to the "#ifndef PDNA_DELTA" I/O
 * block below (the LUTs, the dirty-bitmap helpers, gbscr_cell/text/raw,
 * gbscr_mark_all_dirty, gbscr_toggle_scale) touches only stdint/stdbool/string.h
 * and gb_edit.h's gb_char_encode() -- no tonc, no FatFs. This whole file DOES
 * include tonc.h/ff.h at the top (gbscr_open/close/flush need FIL and vid_mem),
 * so tests/host_gbscreen_test.c does NOT compile this .c file directly -- see
 * its own header comment for how it exercises the pure half instead (it #includes
 * this file with PDNA_GBSCREEN_HOST_TEST defined, which stubs the tonc/FatFs
 * block out, exactly the way rom_gbicon.c precedent -- checked, no such
 * precedent exists here; this is this module's own scheme, documented once).
 */
#include <string.h>

#ifndef PDNA_GBSCREEN_HOST_TEST
#include <tonc.h>
#include "ff.h"
#include "log.h"
#include "sys.h"              /* EWRAM_BSS */
#include "snd.h"              /* snd_deny() for the NOT NOW refusal (U2b review D4) */
#include "pdna_app.h"         /* PDNA_DIR, app_can_edit, app_gb_rom_path, ... */
#include "pdna_origin_art.h"  /* PDNA_GEN1/2, pdna_origin_art_stack_room       */
#include "ui.h"                /* ui_fill_rect, ui_ptext, ui_ptext_shadow      */
#include "pdna_layout.h"       /* PDNA_GBSCR_KEY_.. / PDNA_GBSCR_ACT_.. -- D1 fix, measured by host_textfit */
#ifdef PDNA_DELTA
#include "fused_gb.h"
#endif
#else
/* Host-test stand-ins, just enough for the pure half below to compile/link on
 * the PC with no GBA toolchain. */
#define PDNA_GEN1 1
#define PDNA_GEN2 2
typedef unsigned short u16;
#endif

#include "gb_edit.h"           /* gb_char_encode, GB_GEN1/GB_GEN2 == PDNA_GEN1/2 */
#include "rom_gbui.h"
#include "gb_art_source.h"     /* GB_ROM_PATH_MAX, gb_art_have's siblings        */
#include "pdna_gbscreen.h"

/* ---------------------------------------------------------------------------
 * gb_scale_mode -- the ONE new EWRAM byte this whole shell adds.
 * --------------------------------------------------------------------------- */
#ifndef PDNA_GBSCREEN_HOST_TEST
EWRAM_BSS uint8_t gb_scale_mode = 0;
#else
uint8_t gb_scale_mode = 0;
#endif

/* ---------------------------------------------------------------------------
 * The stretch-blit tables (design sec 1.5, this slice's exact spec).
 *
 * D4 fix (U2a review): this file used to ALSO carry a pair of gbscr_x_lut[240]/
 * gbscr_y_lut[160] "destination -> source" tables, tested by
 * tests/host_gbscreen_test.c -- but blit_stretched() below never read them; it
 * always used the inline x formula and the y_dst0/y_dst_count tables. That
 * made the LUT tests pure theater (400 B of dead ROM data, "verified" by a
 * test that could never catch a real bug in the blit). Deleted; the tables
 * actually used by the blit are exported (not `static`) so the host test can
 * check THOSE instead -- see this file's own y_dst0/y_dst_count below and the
 * x formula inline in blit_stretched().
 *
 * y_dst0[144]/y_dst_count[144]: reverse of a "source row -> destination rows"
 * mapping, precomputed so the per-cell blit never divides by 9 -- for source
 * row r (0..143), y_dst0[r] is the FIRST destination row it shows on, and
 * y_dst_count[r] (1 or 2) how many consecutive destination rows (every 9th
 * source row maps to two destination rows instead of one: 16 groups cover
 * source 0..143 -> destination 0..159). x needs no equivalent table -- its
 * period is 2, so blit_stretched()'s own `s>>1`/`s&1` formula is exact and
 * division-free already: for source column s (0..159), group g = s>>1, parity
 * p = s&1, the two destination columns are dx0 = 3*g + (p?1:0) for dxn = p?2:1
 * columns (covers destination 0..239 exactly once, duplicating every 2nd
 * source column, 80 duplicates). */
const uint8_t gbscr_y_dst0[144] = {
  0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,16,17,18,20,21,22,23,24,25,26,27,28,
  30,31,32,33,34,35,36,37,38,40,41,42,43,44,45,46,47,48,50,51,52,53,54,55,
  56,57,58,60,61,62,63,64,65,66,67,68,70,71,72,73,74,75,76,77,78,80,81,82,
  83,84,85,86,87,88,90,91,92,93,94,95,96,97,98,100,101,102,103,104,105,106,
  107,108,110,111,112,113,114,115,116,117,118,120,121,122,123,124,125,126,
  127,128,130,131,132,133,134,135,136,137,138,140,141,142,143,144,145,146,
  147,148,150,151,152,153,154,155,156,157,158
};
const uint8_t gbscr_y_dst_count[144] = {
  1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,
  1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,2
};

/* ---------------------------------------------------------------------------
 * Dirty bitmap.
 * --------------------------------------------------------------------------- */
static void dirty_set(GbScreen* gs, int idx) { gs->dirty[idx >> 3] |= (uint8_t)(1u << (idx & 7)); }
static void dirty_clear(GbScreen* gs, int idx) { gs->dirty[idx >> 3] &= (uint8_t)~(1u << (idx & 7)); }
static bool dirty_test(const GbScreen* gs, int idx) { return ((gs->dirty[idx >> 3] >> (idx & 7)) & 1u) != 0; }

void gbscr_mark_all_dirty(GbScreen* gs) {
  if (!gs) return;
  memset(gs->dirty, 0xFF, sizeof gs->dirty);
}

/* ---------------------------------------------------------------------------
 * Tilemap writers -- pure, no tonc/FatFs.
 * --------------------------------------------------------------------------- */
void gbscr_cell(GbScreen* gs, int x, int y, GbScrSrc src, uint8_t tile) {
  if (!gs || !gs->ok) return;
  if ((unsigned)x >= GBSCR_COLS || (unsigned)y >= GBSCR_ROWS) return;
  int idx = y * GBSCR_COLS + x;
  if (gs->map[idx] == tile && gs->src[idx] == (uint8_t)src) return;
  gs->map[idx] = tile;
  gs->src[idx] = (uint8_t)src;
  dirty_set(gs, idx);
}

/* One GB byte -> one cell. 0x7F (space) is a BLANK cell, never a font tile --
 * the design's own rule (docs/GB-GAME-SCREENS-DESIGN.md sec 3.2). */
static void gbscr_put_byte(GbScreen* gs, int x, int y, uint8_t b) {
  if (b == 0x7Fu) gbscr_cell(gs, x, y, GBSCR_SRC_BLANK, 0);
  else            gbscr_cell(gs, x, y, GBSCR_SRC_FONT, b);
}

void gbscr_text(GbScreen* gs, int x, int y, const char* ascii) {
  if (!gs || !gs->ok || !ascii) return;
  int cx = x;
  while (*ascii && cx < GBSCR_COLS) {
    uint8_t b;
    int used = gb_char_encode(gs->gen, ascii, &b);
    if (used <= 0) break;
    gbscr_put_byte(gs, cx, y, b);
    ascii += used;
    cx++;
  }
}

/* D2 (review): 0x50 is the GB text terminator (PlaceString stops there) --
 * this shell used to blit every byte in `bytes` regardless, so a shorter new
 * name left old tail bytes from a longer previous name on screen ("ASH" +
 * 0x50 + stale "JACK" tail read as "ASH<blank>JAC"). Once the terminator (or
 * any other byte that is not a real font tile, i.e. < 0x80 and not 0x7F) is
 * seen, every remaining byte -- including the terminator itself -- paints
 * BLANK, so the field is fully cleared on repaint instead of only up to the
 * terminator. */
void gbscr_raw(GbScreen* gs, int x, int y, const uint8_t* bytes, int n) {
  if (!gs || !gs->ok || !bytes) return;
  bool blank_tail = false;
  for (int i = 0; i < n; i++) {
    int cx = x + i;
    if (cx >= GBSCR_COLS) break;
    uint8_t b = bytes[i];
    if (!blank_tail && b < 0x80u && b != 0x7Fu) blank_tail = true;
    gbscr_put_byte(gs, cx, y, blank_tail ? 0x7Fu : b);
  }
}

void gbscr_toggle_scale(GbScreen* gs) {
  gb_scale_mode = gb_scale_mode ? 0 : 1;
  if (gs) { gbscr_mark_all_dirty(gs); gs->scale_dirty = true; }
}

/* ---------------------------------------------------------------------------
 * U2b item 1: the RAM tile bank -- pure (no tonc/FatFs), so the memory-backed
 * read fn and the block-size/offset tables are host-testable exactly like the
 * rest of this half of the file.
 * --------------------------------------------------------------------------- */

/* Exact byte length of one located block, by generation -- rom_gbui_tile()'s own
 * tile_count * stride for that block/bpp (docs/GB-GAME-SCREENS-DESIGN.md sec 2.1/
 * 2.2): FONT 128 tiles * 8 B (1bpp) both gens; TEXTBOX is Gen 1's own 32-tile 2bpp
 * block (512 B) or Gen 2's 54-tile 1bpp frame set (432 B, G2-R: "9 frames x 6
 * tiles x 8 B"); CARDFRAME is Gen 1 only (40 tiles, 2bpp, 640 B -- 9+22+1+8 per
 * G1-C); BADGES is Gen 1's 64-tile (1,024 B) or Gen 2's 44-tile (704 B) block,
 * both 2bpp. PIC has no entry here -- it is not a RomGbUi block (see
 * GBSCR_NEED_* in pdna_gbscreen.h). */
uint32_t gbscr_block_bytes(uint8_t gen, GbScrSrc src) {
  switch (src) {
    case GBSCR_SRC_FONT:      return 128u * 8u;
    case GBSCR_SRC_TEXTBOX:   return (gen == PDNA_GEN1) ? 32u * 16u : 54u * 8u;
    case GBSCR_SRC_CARDFRAME: return 40u * 16u;
    case GBSCR_SRC_BADGES:    return (gen == PDNA_GEN1) ? 64u * 16u : 44u * 16u;
    /* U3 (re-anchored, see the STATUSWORD/CARDGFX comment on
     * gbscr_block_off() below): FONTEXTRA 32 tiles 2bpp (512 B, unused by
     * the fixed painter, kept for other callers); LEADERS 86 tiles 2bpp
     * (1,376 B: 80 for the 8 gym-leader faces + 6 for the "BADGES" page-2
     * word, only 5 used); CARDGFX 6 tiles 2bpp (96 B: border fill, notch,
     * divider fill, divider cap, "ID", "No" -- all 6 used); CARDPIC_M/F 35
     * tiles 2bpp (560 B, the 5x7 card photo); STATUSWORD 6 tiles 2bpp
     * (96 B: the 5 "STATUS" glyphs + the play-time colon, immediately
     * before LEADERS). */
    case GBSCR_SRC_FONTEXTRA: return 32u * 16u;
    case GBSCR_SRC_LEADERS:   return 86u * 16u;
    case GBSCR_SRC_CARDGFX:   return 6u * 16u;
    case GBSCR_SRC_CARDPIC_M: return 35u * 16u;
    case GBSCR_SRC_CARDPIC_F: return 35u * 16u;
    case GBSCR_SRC_STATUSWORD: return 6u * 16u;
    default:                  return 0;
  }
}

/* The located ROM/fused-image offset of one block, out of an already-open
 * RomGbUi -- mirrors gbscr_tile_pixels()'s own per-src field choice (Gen 1's
 * TEXTBOX src reads `gu->textbox`, Gen 2's reads `gu->frames` -- see that
 * function's own comment for why there is no single "textbox" field). Returns 0
 * (a real ROM never starts its own image there -- offset 0 is the GB header,
 * never a target of one of THESE locators) for FONT's caller-supplied cases /
 * PIC / an unrecognised src. */
uint32_t gbscr_block_off(const RomGbUi* gu, uint8_t gen, GbScrSrc src) {
  if (!gu) return 0;
  switch (src) {
    case GBSCR_SRC_FONT:      return gu->font;
    case GBSCR_SRC_TEXTBOX:   return (gen == PDNA_GEN1) ? gu->textbox : gu->frames;
    case GBSCR_SRC_CARDFRAME: return gu->cardframe;
    case GBSCR_SRC_BADGES:    return gu->badges;
    case GBSCR_SRC_FONTEXTRA: return gu->fontextra;
    case GBSCR_SRC_LEADERS:   return gu->leaders;
    case GBSCR_SRC_CARDGFX:   return gu->cardgfx;
    case GBSCR_SRC_CARDPIC_M: return gu->cardpic_m;
    case GBSCR_SRC_CARDPIC_F: return gu->cardpic_f;
    case GBSCR_SRC_STATUSWORD: return gu->leaders ? gu->leaders - 96u : 0u;
    default:                  return 0;
  }
}

/* The GbReadFn gbscr_flush() binds every repaint to (U2b item 1): serves
 * rom_gbui_tile()/rom_gbui_glyph()'s reads out of `ctx`'s (a GbscrCache*) `tail`
 * buffer, built ONCE at gbscr_open() time -- no SD card, no FIL, ever, after
 * open() returns. A read whose [off, off+len) is not fully covered by exactly one
 * cached block fails closed (0/false) -- same "the caller's need_mask must cover
 * every src it paints" contract app_arena_acquire() callers already carry for
 * sizing; this is what a host test can drive directly with a synthetic
 * GbscrCache, no ROM file needed. */
bool gbscr_mem_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  const GbscrCache* c = (const GbscrCache*)ctx;
  if (!c || !c->tail || !buf) return false;
  for (int i = 0; i < c->nblocks; i++) {
    const GbscrBlock* b = &c->blocks[i];
    if (off < b->rom_off) continue;
    uint32_t rel = off - b->rom_off;
    if (rel > b->len || len > b->len - rel) continue;   /* overflow-safe bound check */
    memcpy(buf, c->tail + b->ram_off + rel, len);
    return true;
  }
  return false;
}

/* U2b item 1: which order the extra (need_mask) blocks are copied into the tail
 * cache, right after FONT -- fixed, so the cache-building loop and any test that
 * inspects a GbscrCache agree on layout. Pure data: moved above the tonc/FatFs
 * boundary (U2b/U2c review item 0c) so gbscr_cache_plan() below can use it. */
static const GbScrSrc kCacheOptOrder[9] = {
  GBSCR_SRC_TEXTBOX, GBSCR_SRC_CARDFRAME, GBSCR_SRC_BADGES,
  /* U3: Gen 2's own card additions. */
  GBSCR_SRC_FONTEXTRA, GBSCR_SRC_LEADERS, GBSCR_SRC_CARDGFX,
  GBSCR_SRC_CARDPIC_M, GBSCR_SRC_CARDPIC_F, GBSCR_SRC_STATUSWORD
};
#define GBSCR_CACHE_OPT_N 9

/* Total tail bytes gbscr_open() needs for `need_mask` on generation `gen`:
 * the 2,048-B rom_gbui scan scratch, reused afterward for FONT (always cached)
 * plus every block need_mask names. Exported (U2c) so a caller sizing its own
 * arena-tail request (e.g. the Gen-1 card, on top of its own player-pic bytes)
 * can call the SAME arithmetic gbscr_open_inner() gates on, rather than
 * re-deriving it and risking the two falling out of sync. */
uint32_t gbscr_tail_need(uint8_t gen, uint16_t need_mask) {
  uint32_t need = ROM_GBUI_SCRATCH_MIN + gbscr_block_bytes(gen, GBSCR_SRC_FONT);
  for (int i = 0; i < GBSCR_CACHE_OPT_N; i++)
    if (need_mask & (1u << kCacheOptOrder[i])) need += gbscr_block_bytes(gen, kCacheOptOrder[i]);
  return need;
}

/* U2b/U2c review item 0c: the PURE half of what used to be gbscr_cache_block()'s
 * loop -- decides WHERE each block (FONT, always, then every need_mask block in
 * kCacheOptOrder) lands in the tail cache (rom_off/ram_off/len), with NO I/O at
 * all (gu->read is never called). This is what tests/host_gbscreen_test.c can
 * drive directly with a synthetic RomGbUi (offsets set, no real ROM file needed)
 * -- the shot harness cannot see a shifted-glyph corruption from a bad ram_off,
 * but a host test that recomputes the layout by hand can.
 *
 * `gu` need only have its located offset fields set (gu->font, gu->textbox/
 * frames, gu->cardframe, gu->badges) -- .ok/.read/.ctx are never touched.
 * Returns false (out->nblocks left at whatever was filled before the failure)
 * if a needed block has no located offset (off==0) or size (len==0), or the
 * plan would overrun GBSCR_MAX_BLOCKS -- the same "fail closed" contract the
 * old gbscr_cache_block() loop had, just without the read. */
bool gbscr_cache_plan(uint8_t gen, uint16_t need_mask, const RomGbUi* gu,
                      uint32_t tail_len, GbscrCache* out) {
  if (!gu || !out) return false;
  memset(out, 0, sizeof *out);
  uint32_t cursor = 0;

  /* FONT is always cached, first. */
  {
    uint32_t off = gbscr_block_off(gu, gen, GBSCR_SRC_FONT);
    uint32_t len = gbscr_block_bytes(gen, GBSCR_SRC_FONT);
    if (!off || !len || out->nblocks >= GBSCR_MAX_BLOCKS) return false;
    out->blocks[out->nblocks].rom_off = off;
    out->blocks[out->nblocks].ram_off = cursor;
    out->blocks[out->nblocks].len = len;
    out->nblocks++;
    cursor += len;
  }

  for (int i = 0; i < GBSCR_CACHE_OPT_N; i++) {
    GbScrSrc src = kCacheOptOrder[i];
    if (!(need_mask & (1u << src))) continue;
    uint32_t off = gbscr_block_off(gu, gen, src);
    uint32_t len = gbscr_block_bytes(gen, src);
    if (!off || !len || out->nblocks >= GBSCR_MAX_BLOCKS) return false;
    out->blocks[out->nblocks].rom_off = off;
    out->blocks[out->nblocks].ram_off = cursor;
    out->blocks[out->nblocks].len = len;
    out->nblocks++;
    cursor += len;
  }

  (void)tail_len;   /* the caller (gbscr_open_inner) already gated on
                      * gbscr_tail_need() before calling this; a plan never
                      * needs more than sum(len) <= tail_need - SCRATCH_MIN,
                      * always true when the caller's own gate passed */
  return true;
}

/* ---------------------------------------------------------------------------
 * U2c: the Gen-1 player pic pack/unpack pair -- pure arithmetic (no tonc/
 * FatFs), moved up from the impure half below (minor, U2c review) so
 * tests/host_gbscreen_test.c can exercise the pack format directly instead
 * of only through the shipped decode path. `px` is gb_sprite_gen1_buf()'s
 * own output shape: index 0..3 per pixel, row-major, stride `w` -- NOT the
 * ROM's own bit-plane tile format. This module's OWN pack/unpack pair (never
 * need to match the ROM's layout, since this code both writes and reads it):
 * 4 pixels/byte, 2 bits each, LSB-first (leftmost column in the low bits), 2
 * bytes per tile ROW (8 px), 16 B/tile -- GBSCR_PIC_PACKED_BYTES for the
 * full 7x7 grid. `tiles_w`/`tiles_h` (1..7) bound which of the 49 tile slots
 * actually get real data; any tile beyond the decoded grid (never happens
 * for a 7x7 Gen-1 pic, but Slowbro-sized smaller pics exist in principle)
 * stays zeroed (index 0, lightest) on pack, and unpack naturally reads back
 * 0 there too. */
void gbscr_pack_pic(const uint8_t* px, int w, int h, int tiles_w, int tiles_h, uint8_t* out) {
  memset(out, 0, GBSCR_PIC_PACKED_BYTES);
  if (tiles_w > 7) tiles_w = 7;
  if (tiles_h > 7) tiles_h = 7;
  for (int ty = 0; ty < tiles_h; ty++) {
    for (int tx = 0; tx < tiles_w; tx++) {
      uint8_t* td = out + (ty * 7 + tx) * 16;
      for (int ry = 0; ry < 8; ry++) {
        int py = ty * 8 + ry;
        uint8_t b0 = 0, b1 = 0;
        if (py < h) {
          for (int cx = 0; cx < 4; cx++) {
            int pxx = tx * 8 + cx;
            uint8_t v = (pxx < w) ? (uint8_t)(px[py * w + pxx] & 3u) : 0u;
            b0 = (uint8_t)(b0 | (v << (cx * 2)));
          }
          for (int cx = 0; cx < 4; cx++) {
            int pxx = tx * 8 + 4 + cx;
            uint8_t v = (pxx < w) ? (uint8_t)(px[py * w + pxx] & 3u) : 0u;
            b1 = (uint8_t)(b1 | (v << (cx * 2)));
          }
        }
        td[ry * 2 + 0] = b0;
        td[ry * 2 + 1] = b1;
      }
    }
  }
}

/* The exact inverse of gbscr_pack_pic(): reads back the 2-bit index at pixel
 * (px_x, px_y) of the tiles_w x tiles_h grid `packed` describes. Out-of-range
 * coordinates (>= tiles_w*8 or >= tiles_h*8) return 0 (same "lightest" value
 * pack's own zero-fill leaves there) rather than reading past the buffer --
 * host_gbscreen_test.c's pack/unpack round trip is the one caller so far. */
uint8_t gbscr_unpack_pic_px(const uint8_t* packed, int tiles_w, int tiles_h, int px_x, int px_y) {
  if (tiles_w > 7) tiles_w = 7;
  if (tiles_h > 7) tiles_h = 7;
  if (px_x < 0 || px_y < 0 || px_x >= tiles_w * 8 || px_y >= tiles_h * 8) return 0;
  int tx = px_x / 8, ty = px_y / 8, cx = px_x % 8, ry = px_y % 8;
  const uint8_t* td = packed + (uint32_t)(ty * 7 + tx) * 16u;
  uint8_t b = td[ry * 2 + (cx >= 4 ? 1 : 0)];
  int shift = (cx % 4) * 2;
  return (uint8_t)((b >> shift) & 3u);
}

/* ===========================================================================
 * Everything below needs tonc/FatFs: ROM I/O (gbscr_open/close) and the VRAM
 * blit (gbscr_flush). Stubbed out under PDNA_GBSCREEN_HOST_TEST so the pure
 * half above still compiles/links on the PC.
 * =========================================================================== */
#ifndef PDNA_GBSCREEN_HOST_TEST

#define GBSCR_BLANK_COLOR RGB15(31, 31, 31)   /* DMG's own lightest shade (0xF8) */

static const char* const kReasonNoRom   = PDNA_GBSCR_REASON_NO_ROM;
static const char* const kReasonNoStack = PDNA_GBSCR_REASON_NO_STACK;
/* D7 (review) + BACKLOG #79: shortened to <=24 chars so it fits the plain
 * page's single title line (gbtr_plain_render's `header`) without clipping;
 * the longer detail (bad ROM / wrong game / non-English release) lives only
 * in the triple-logger call sites below, not on screen. D2 fix (U2c 2nd
 * re-verify): now lives on its OWN second line (`header2`), not appended to
 * the title -- see PDNA_GBTR_FALLBACK_TITLE/GBTR_HEADER2_MAXW (pdna_layout.h). */
static const char* const kReasonOpen    = PDNA_GBSCR_REASON_OPEN;
static const char* const kReasonBadGen  = PDNA_GBSCR_REASON_BAD_GEN;
static const char* const kReasonNoTail  = PDNA_GBSCR_REASON_NO_TAIL;
#ifdef PDNA_DELTA
/* BACKLOG #98 D2: distinct fallback-page reasons for the two fused_gb_rom()
 * failure modes fused_gb_lookup_failed_reason() can now report that
 * kReasonNoRom used to swallow indistinguishably from "nothing fused at all". */
static const char* const kReasonAmbiguousRom = PDNA_GBSCR_REASON_AMBIGUOUS_ROM;
static const char* const kReasonOrphanedRom  = PDNA_GBSCR_REASON_ORPHANED_ROM;
#endif

#ifndef PDNA_DELTA
/* ---- SD build: FIL-backed I/O, the /PokeDNA/gbui<gen>.loc cache -----------
 * Same shape as gb_art_source.c's own gb_art_read/gb_art_loc_path/load_loc/
 * save_loc/resolve_path -- duplicated rather than shared (that module's own
 * comment explains why: those helpers are `static` to a different translation
 * unit and this is a parallel, independent path). */
static bool gbscr_sd_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  FIL* f = (FIL*)ctx;
  UINT br = 0;
  if (!f || !buf) return false;
  if (f_lseek(f, (FSIZE_t)off) != FR_OK) return false;
  if (f_read(f, buf, (UINT)len, &br) != FR_OK) return false;
  return br == len;
}

static void gbscr_loc_path(uint8_t gen, char* out, int cap) {
  siprintf(out, "%.*s/gbui%u.loc", cap - 11, PDNA_DIR, (unsigned)gen);
}

static bool gbscr_load_loc(uint8_t gen, RomGbUiLoc* out) {
  char path[40];
  gbscr_loc_path(gen, path, (int)sizeof path);
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  FRESULT fr = f_read(&f, out, (UINT)sizeof *out, &br);
  f_close(&f);
  return fr == FR_OK && br == sizeof *out;
}

static void gbscr_save_loc(uint8_t gen, const RomGbUiLoc* loc) {
  if (!app_can_edit()) return;                 /* Everdrive/read-only: don't even try */
  char path[40];
  gbscr_loc_path(gen, path, (int)sizeof path);
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
  f_write(&f, loc, (UINT)sizeof *loc, &bw);
  f_close(&f);
  if (bw != sizeof *loc) log_line("gbscreen: loc cache write short for gen%u", (unsigned)gen);
}

static bool gbscr_resolve_path(uint8_t gen, char* out, int cap) {
  const char* reg = app_gb_rom_path(gen);
  if (reg && reg[0]) {
    int i = 0; for (; reg[i] && i < cap - 1; i++) out[i] = reg[i]; out[i] = 0;
    return true;
  }
  if (app_current_save_is_gb()) return gb_rom_path_beside(app_current_save_path(), gen, out, cap);
  return false;
}
#endif /* !PDNA_DELTA */

/* U2b/U2c review item 0c: the plan (WHERE each block goes) now lives in the
 * pure gbscr_cache_plan() above the tonc/FatFs boundary; this loop performs
 * ONLY the I/O (the actual `gu->read()` into `tail`) against a plan already
 * computed. Returns false (and leaves `cache` at whatever the plan filled,
 * matching the old gbscr_cache_block() loop's own partial-fill behaviour) on
 * a short/failed read for any planned block. */
static bool gbscr_cache_fill(RomGbUi* gu, const GbscrCache* plan, uint8_t* tail,
                             GbscrCache* cache) {
  cache->tail = tail;
  cache->nblocks = plan->nblocks;
  for (int i = 0; i < plan->nblocks; i++) {
    cache->blocks[i] = plan->blocks[i];
    if (!gu->read(gu->ctx, plan->blocks[i].rom_off, tail + plan->blocks[i].ram_off,
                  plan->blocks[i].len))
      return false;
  }
  return true;
}

/* D1 fix (U2a review): the stack-room gate must run BEFORE this function's own
 * frame exists, not from inside it -- gating from inside measures the room LEFT
 * UNDER the frame, not the room the frame itself needs, which on the artless/SD
 * build's Settings path always under-counts by exactly this frame's own size and
 * so ALWAYS refuses on real hardware. `gbscr_open()` below is the thin gate;
 * this is the renamed original body, unchanged in that respect.
 *
 * U2b item 1: the 2,048-B rom_gbui scan scratch is no longer a local array here
 * (that was this frame's single biggest cost) -- it is the FIRST 2,048 B of the
 * caller-owned `tail` buffer, reused for the RAM tile cache once the scan is
 * done with it (rom_gbui_open_loc() returns before this function touches `tail`
 * again). `rom_path` also drops off this frame (GbScreen no longer stores a
 * path at all -- the FIL/fused slice is never reopened after this call, see
 * gbscr_flush()'s own note), leaving FIL + RomGbUiLoc + a couple of locals. */
static bool __attribute__((noinline)) gbscr_open_inner(uint8_t gen, GbScreen* gs,
                                                        uint8_t* tail, uint32_t tail_len,
                                                        uint16_t need_mask, const char** reason) {
  memset(gs, 0, sizeof *gs);
  gs->gen = gen;

  if (!tail || tail_len < gbscr_tail_need(gen, need_mask)) {
    if (reason) *reason = kReasonNoTail;
    return false;
  }

#ifndef PDNA_DELTA
  char path[GB_ROM_PATH_MAX];
  if (!gbscr_resolve_path(gen, path, (int)sizeof path)) { if (reason) *reason = kReasonNoRom; return false; }

  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) { if (reason) *reason = kReasonOpen; return false; }
  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  RomGbUiLoc loc;
  bool have_loc = gbscr_load_loc(gen, &loc);
  int ok = rom_gbui_open_loc(&gs->gu, gbscr_sd_read, &fil, sz, tail, ROM_GBUI_SCRATCH_MIN,
                             have_loc ? &loc : 0);
  if (!ok || (uint8_t)gs->gu.gen != gen) { f_close(&fil); if (reason) *reason = kReasonOpen; return false; }
  /* D5 fix (U2a review): the old condition here (`!have_loc || id_hash/size
   * mismatch`) never healed a REJECTED loc -- rom_gbui_open_loc() also falls
   * back to a full scan on a `check`/gen/revalidate failure inside a
   * bit-rotted loc that still matches id_hash/size, so that file would fail
   * the SAME way on every future open forever. Always derive `fresh` from
   * this open's own successful result and rewrite unless it is BYTE-IDENTICAL
   * to what was loaded -- a genuine cache hit never touches the card. */
  RomGbUiLoc fresh;
  rom_gbui_save_loc(&gs->gu, &fresh);
  if (!have_loc || memcmp(&fresh, &loc, sizeof fresh) != 0) gbscr_save_loc(gen, &fresh);

  /* U2b item 1 / U2c review 0c: plan (pure) then fill (I/O) FONT + every
   * need_mask block into `tail`, reusing the SAME bytes the scan scratch above
   * just finished with, then close the FIL -- gbscr_flush() never reopens it.
   * gu->read/gu->ctx are still bound to `fil` here (rom_gbui_open_loc() left
   * them that way on success). */
  GbscrCache plan;
  bool cok = gbscr_cache_plan(gen, need_mask, &gs->gu, tail_len, &plan) &&
             gbscr_cache_fill(&gs->gu, &plan, tail, &gs->cache);
  f_close(&fil);
  if (!cok) { if (reason) *reason = kReasonOpen; return false; }
#else
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(gen, &base, &size)) {
    /* BACKLOG #98 D2: say WHY, not just THAT -- an orphaned active save or a
     * genuinely ambiguous fused directory are both actionable ("open the picker
     * and choose a save" / "this save was never fused with a ROM"), unlike the
     * generic kReasonNoRom ("no ROM registered" -- implies nothing is fused at
     * all, which is misleading when something IS fused, just not resolvably). */
    if (reason) {
      switch (fused_gb_lookup_failed_reason()) {
        case FUSED_GB_FAIL_AMBIGUOUS:    *reason = kReasonAmbiguousRom; break;
        case FUSED_GB_FAIL_ORPHANED:     *reason = kReasonOrphanedRom;  break;
        case FUSED_GB_FAIL_GEN_MISMATCH: *reason = kReasonOrphanedRom;  break;
        default:                         *reason = kReasonNoRom;       break;
      }
    }
    return false;
  }
  FusedGbSlice slice = { base, size };
  /* #62's own posture (fused corpus is immutable for the whole run): no EWRAM
   * loc cache here -- this slice's memory budget forbids any new EWRAM static
   * beyond gb_scale_mode, so a delta-build gbscr_open() always does the full
   * scan. Rare (a screen entry, not a per-frame cost); a future slice may add
   * an EWRAM cache the same way gb_art_source.c's #62 D1 did IF the budget is
   * revisited. */
  int ok = rom_gbui_open(&gs->gu, fused_gb_slice_read, &slice, size, tail, ROM_GBUI_SCRATCH_MIN);
  if (!ok || (uint8_t)gs->gu.gen != gen) { if (reason) *reason = kReasonOpen; return false; }

  GbscrCache plan;
  bool cok = gbscr_cache_plan(gen, need_mask, &gs->gu, tail_len, &plan) &&
             gbscr_cache_fill(&gs->gu, &plan, tail, &gs->cache);
  if (!cok) { if (reason) *reason = kReasonOpen; return false; }
#endif

  gs->ok = true;
  gbscr_mark_all_dirty(gs);
  return true;
}

/* The thin gate (D1): validates `gen` and checks the stack-room budget BEFORE
 * gbscr_open_inner()'s own frame is ever allocated, then tail-calls into it.
 * Both refusal branches leave `gs` zeroed with `gs->gen` set, same observable
 * state gbscr_open_inner() used to leave on the same refusals. */
bool __attribute__((noinline)) gbscr_open(uint8_t gen, GbScreen* gs, uint8_t* tail,
                                          uint32_t tail_len, uint16_t need_mask,
                                          const char** reason) {
  if (reason) *reason = 0;
  if (!gs) return false;
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) {
    memset(gs, 0, sizeof *gs);
    gs->gen = gen;
    if (reason) *reason = kReasonBadGen;
    return false;
  }
  if (!pdna_origin_art_stack_room(PDNA_GB_UI_NEED)) {
    memset(gs, 0, sizeof *gs);
    gs->gen = gen;
    if (reason) *reason = kReasonNoStack;
    return false;
  }
  return gbscr_open_inner(gen, gs, tail, tail_len, need_mask, reason);
}

/* U2b item 3: config.cfg NOW, iff gb_scale_mode changed during this `gs`'s
 * visit -- see the header's own doc comment for the full contract. */
void gbscr_persist_mode(GbScreen* gs) {
  if (!gs || !gs->scale_dirty) return;
  gs->scale_dirty = false;
  app_cfg_save();
}

void gbscr_close(GbScreen* gs) {
  if (!gs) return;
  gbscr_persist_mode(gs);
  gs->ok = false;   /* no persistent handle to release -- see the header's own note */
}

/* ---------------------------------------------------------------------------
 * U2c: the Gen-1 player pic -- rendering half. gbscr_pack_pic()/
 * gbscr_unpack_pic_px() (the actual pack/unpack pair) moved to the pure
 * section above this file's tonc/FatFs boundary (minor, U2c review) so
 * tests/host_gbscreen_test.c can round-trip them directly; only the RGB15
 * shade lookup GBSCR_SRC_PIC's renderer needs stays here. Same DMG ramp
 * rom_gbui.c's own (static, not exported) DMG_SHADE uses -- duplicated here
 * rather than exported across a module boundary for four uint16_t.
 * --------------------------------------------------------------------------- */
#define GBSCR_PIC_GB8_TO_RGB15(v) ((uint16_t)((((v) >> 3) & 0x1Fu) | \
                                    ((((v) >> 3) & 0x1Fu) << 5) | ((((v) >> 3) & 0x1Fu) << 10)))
static const uint16_t kGbscrPicShade[4] = {
  GBSCR_PIC_GB8_TO_RGB15(0xF8), GBSCR_PIC_GB8_TO_RGB15(0xA8),
  GBSCR_PIC_GB8_TO_RGB15(0x58), GBSCR_PIC_GB8_TO_RGB15(0x10)
};

bool gbscr_decode_pic_gen1(GbScreen* gs, uint8_t* buf, uint32_t buf_len) {
  if (!gs || !gs->ok || gs->gen != PDNA_GEN1 || !gs->gu.playerpic) return false;
  if (!buf || buf_len < GBSCR_PIC_TAIL_BYTES) return false;

  uint8_t* pic_out = buf;
  uint8_t* px = buf + GBSCR_PIC_PACKED_BYTES;
  uint8_t* work = px + GB_SPRITE_MAX_PX;
  GbSpriteInfo info;
  GbSpriteErr err;

#ifndef PDNA_DELTA
  char path[GB_ROM_PATH_MAX];
  if (!gbscr_resolve_path(gs->gen, path, (int)sizeof path)) return false;
  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) return false;
  err = gb_sprite_gen1_buf(px, work, gbscr_sd_read, &fil, gs->gu.playerpic, &info);
  f_close(&fil);
#else
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(gs->gen, &base, &size)) return false;
  FusedGbSlice slice = { base, size };
  err = gb_sprite_gen1_buf(px, work, fused_gb_slice_read, &slice, gs->gu.playerpic, &info);
#endif
  if (err != GB_SPRITE_OK) return false;
  if (info.wt < 1 || info.wt > 7 || info.ht < 1 || info.ht > 7) return false;

  gbscr_pack_pic(px, info.w, info.h, info.wt, info.ht, pic_out);
  gs->cache.pic = pic_out;
  return true;
}

/* One cell's 8x8 RGB15 pixels, looked up through `local` (a RomGbUi copy whose
 * .ctx/.read the caller has just rebound to gs->cache, the RAM tile bank --
 * U2b item 1, never a live FIL/FusedGbSlice any more). BLANK
 * never touches the ROM at all. */
static bool gbscr_tile_pixels(const GbScreen* gs, RomGbUi* local, int idx, uint16_t out[64]) {
  GbScrSrc s = (GbScrSrc)gs->src[idx];
  uint8_t v = gs->map[idx];
  switch (s) {
    case GBSCR_SRC_FONT:
      return rom_gbui_glyph(local, v, out) != 0;
    case GBSCR_SRC_TEXTBOX:
      /* Gen 1: RomGbUi.textbox (G1-T, 2bpp). Gen 2 has no separate "textbox"
       * field -- its own text-box frames are RomGbUi.frames (G2-R, 1bpp). Raw
       * linear index (grid 0,0): neither block's exact 2D shape is load-
       * bearing for this shell -- U2b's card uses CARDFRAME's own documented
       * relative offsets instead. */
      if (gs->gen == PDNA_GEN1) return rom_gbui_tile(local, local->textbox, v, 2, 0, 0, 0, out) != 0;
      return rom_gbui_tile(local, local->frames, v, 1, 0, 0, 0, out) != 0;
    case GBSCR_SRC_CARDFRAME:
      return rom_gbui_tile(local, local->cardframe, v, 2, 0, 0, 0, out) != 0;
    case GBSCR_SRC_BADGES:
      return rom_gbui_tile(local, local->badges, v, 2, 0, 0, 0, out) != 0;
    /* U3: Gen 2's own card blocks -- plain rom_gbui_tile() reads, like BADGES
     * above (no special codec). CARDPIC_M/F pass the 5x7 grid + the located
     * cardpic_colmajor flag so rom_gbui_tile() applies Crystal's own column-
     * major reorder (Gold's cardpic_m is row-major, grid_w/grid_h make that a
     * no-op transform either way -- rom_gbui_tile only reorders when
     * `colmajor` is true). */
    case GBSCR_SRC_FONTEXTRA:
      return rom_gbui_tile(local, local->fontextra, v, 2, 0, 0, 0, out) != 0;
    case GBSCR_SRC_LEADERS:
      return rom_gbui_tile(local, local->leaders, v, 2, 0, 0, 0, out) != 0;
    case GBSCR_SRC_CARDGFX:
      return rom_gbui_tile(local, local->cardgfx, v, 2, 0, 0, 0, out) != 0;
    case GBSCR_SRC_CARDPIC_M:
      return rom_gbui_tile(local, local->cardpic_m, v, 2, 5, 7, local->cardpic_colmajor, out) != 0;
    case GBSCR_SRC_CARDPIC_F:
      return rom_gbui_tile(local, local->cardpic_f, v, 2, 5, 7, local->cardpic_colmajor, out) != 0;
    case GBSCR_SRC_STATUSWORD: {
      uint32_t off = local->leaders ? local->leaders - 96u : 0u;
      return rom_gbui_tile(local, off, v, 2, 0, 0, 0, out) != 0;
    }
    case GBSCR_SRC_PIC:
      /* U2c: the Gen-1 player pic -- a separate compressed codec
       * (gb_sprite_gen1), decoded once by gbscr_decode_pic_gen1() into
       * gs->cache.pic (OUR OWN packed format, not a rom_gbui_tile() block).
       * `v` is the tile index (0..48, row-major over the 7x7 grid) gbscr_cell
       * was given. No cache.pic (never decoded, or Gen 2) or an out-of-range
       * tile falls through to the same flat BLANK every other unavailable
       * src uses. */
      if (gs->cache.pic && v < GBSCR_PIC_TILES) {
        const uint8_t* td = gs->cache.pic + (uint32_t)v * 16u;
        for (int ry = 0; ry < 8; ry++) {
          uint8_t b0 = td[ry * 2 + 0], b1 = td[ry * 2 + 1];
          for (int cx = 0; cx < 4; cx++) out[ry * 8 + cx] = kGbscrPicShade[(b0 >> (cx * 2)) & 3u];
          for (int cx = 0; cx < 4; cx++) out[ry * 8 + 4 + cx] = kGbscrPicShade[(b1 >> (cx * 2)) & 3u];
        }
        return true;
      }
      for (int i = 0; i < 64; i++) out[i] = GBSCR_BLANK_COLOR;
      return true;
    case GBSCR_SRC_BLANK:
    default:
      for (int i = 0; i < 64; i++) out[i] = GBSCR_BLANK_COLOR;
      return true;
  }
}

static void blit_1to1(int cx, int cy, const uint16_t tile[64]) {
  int ox = GBSCR_ORIGIN_X + cx * 8, oy = GBSCR_ORIGIN_Y + cy * 8;
  for (int r = 0; r < 8; r++) {
    u16* row = &vid_mem[(unsigned)(oy + r) * 240u + (unsigned)ox];
    for (int c = 0; c < 8; c++) row[c] = tile[r * 8 + c];
  }
}

/* Stretched blit: x uses the period-2 shift/mask formula directly (exact,
 * division-free); y uses the precomputed gbscr_y_dst0/gbscr_y_dst_count tables
 * above (also division-free) -- no runtime division anywhere in this loop. */
static void blit_stretched(int cx, int cy, const uint16_t tile[64]) {
  int sx0 = cx * 8;
  for (int ly = 0; ly < 8; ly++) {
    int sy = cy * 8 + ly;
    int dy0 = gbscr_y_dst0[sy], dyn = gbscr_y_dst_count[sy];
    const uint16_t* trow = &tile[ly * 8];
    for (int dyi = 0; dyi < dyn; dyi++) {
      u16* row = &vid_mem[(unsigned)(dy0 + dyi) * 240u];
      for (int lx = 0; lx < 8; lx++) {
        int s = sx0 + lx;
        int g = s >> 1, p = s & 1;
        int dx0 = 3 * g + (p ? 1 : 0), dxn = p ? 2 : 1;
        for (int dxi = 0; dxi < dxn; dxi++) row[dx0 + dxi] = trow[lx];
      }
    }
  }
}

/* D9 (U2c review): a screen-owned override for the shell's base legend --
 * see gbscr_set_legend()'s own header doc comment for the contract. */
void gbscr_set_legend(GbScreen* gs, const char* const lines[4]) {
  if (!gs) return;
  for (int i = 0; i < 4; i++) gs->legend[i] = lines ? lines[i] : 0;
}

static bool gbscr_legend_overridden(const GbScreen* gs) {
  return gs && (gs->legend[0] || gs->legend[1] || gs->legend[2] || gs->legend[3]);
}

/* D9 (U2c review) footnote: the 40-px side bar's own claimed "room to spare"
 * was never actually true past ~36 px of text -- a bare ui_ptext() draws past
 * GBSCR_ORIGIN_X (40) and the canvas's own first tile column then paints
 * straight over the tail, splitting a glyph in half (proven: the shell's OWN
 * pre-existing "SEL SIZE" line, 43 px wide, already lost the last stroke of
 * its 'E' before this fix -- /tmp/crop_selsize.png). ui_ptext_fit() (already
 * used elsewhere in this tree) is the fix: past `GBSCR_LEGEND_MAXW` it
 * truncates on a whole-glyph boundary and appends '~' instead of slicing a
 * glyph -- applied here to every 1:1 legend line, base AND override, so
 * "nothing clipped" is actually true rather than merely usually not
 * noticed.
 *
 * D1 fix (U2c 2nd re-verify): ui_ptext_fit() only stops a glyph from being
 * SLICED -- it still truncates ("SEL S~", "START~") because a joined "KEY
 * ACTION" string never fit 36 px to begin with (measured: A EDIT 31, B SAVE
 * 33, SEL SIZE 43, START MORE 57 px). The real fix is two columns, one per
 * side bar the shell already clears and never painted (the right bar, cleared
 * above since D9 but empty): the LEFT bar takes the KEY name alone (a table
 * the shell owns, below -- max 30 px, "START"), the RIGHT bar takes the
 * ACTION word alone (max 24 px, "MORE"/"SIZE") at the mirrored x
 * (GBSCR_RIGHT_BAR_X). Neither column ever needs ui_ptext_fit's tilde: every
 * legal key name and action word this tree ships is asserted to fit at
 * BUILD time (tests/host_gblegend_test.c), so a live truncation here would
 * mean that test is missing a case, not that the tilde path is "the fix". */
#define GBSCR_LEGEND_MAXW (GBSCR_ORIGIN_X - 4)
#define GBSCR_RIGHT_BAR_X (GBSCR_ORIGIN_X + GBSCR_COLS * 8 + 2)
#define GBSCR_RIGHT_BAR_MAXW (240 - GBSCR_RIGHT_BAR_X - 2)

/* Shell-owned key-name column, fixed per row (row0=A, row1=B, row2=SEL,
 * row3=START) for BOTH the base legend and any gbscr_set_legend() override --
 * a screen never supplies a key name, only the action word for the row(s) it
 * uses (gs->legend[i] == 0 skips that row's key AND action, e.g. view-mode's
 * unused A row). */
static const char* const kGbscrLegendKeys[4] = {
  PDNA_GBSCR_KEY_A, PDNA_GBSCR_KEY_B, PDNA_GBSCR_KEY_SEL, PDNA_GBSCR_KEY_START
};
/* Base (no override) action words for the shell's own 3 fixed rows -- was
 * "A OK" / "B BACK" / "SEL SIZE" as single joined strings; same 3 keys, now
 * split across the two bars like every override. */
static const char* const kGbscrBaseActions[3] = {
  PDNA_GBSCR_ACT_OK, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE
};

/* Legend rows, 1:1 mode (design sec 1.5: two stacked columns in the 40-px side
 * bars -- LEFT bar = key name, RIGHT bar = action word, D1 fix above). */
static void gbscr_paint_legend_1to1(const GbScreen* gs, const char* extra) {
  ui_fill_rect(0, 0, GBSCR_ORIGIN_X, 160, UI_BG);
  ui_fill_rect(GBSCR_ORIGIN_X + GBSCR_COLS * 8, 0,
              240 - (GBSCR_ORIGIN_X + GBSCR_COLS * 8), 160, UI_BG);
  ui_fill_rect(0, 0, 240, GBSCR_ORIGIN_Y, UI_BG);
  ui_fill_rect(0, GBSCR_ORIGIN_Y + GBSCR_ROWS * 8, 240,
              160 - (GBSCR_ORIGIN_Y + GBSCR_ROWS * 8), UI_BG);
  /* D9: an override REPLACES all 4 rows (no base "A OK/B BACK/SEL SIZE",
   * no separate DIM `extra` slot -- every line is a real key hint here). */
  if (gbscr_legend_overridden(gs)) {
    static const int ys[4] = { 20, 30, 40, 52 };
    for (int i = 0; i < 4; i++) {
      if (!gs->legend[i]) continue;
      ui_ptext(2, ys[i], UI_TEXT, kGbscrLegendKeys[i]);
      ui_ptext(GBSCR_RIGHT_BAR_X, ys[i], UI_TEXT, gs->legend[i]);
    }
    return;
  }
  static const int base_ys[3] = { 20, 30, 40 };
  for (int i = 0; i < 3; i++) {
    ui_ptext(2, base_ys[i], UI_TEXT, kGbscrLegendKeys[i]);
    ui_ptext(GBSCR_RIGHT_BAR_X, base_ys[i], UI_TEXT, kGbscrBaseActions[i]);
  }
  if (extra) ui_ptext_fit(2, 52, GBSCR_LEGEND_MAXW, UI_DIM, extra);
}

/* D3 fix (U2a review): stretched mode used to REPLACE the shell's own keys
 * ("A OK  B BACK  SEL SIZE") with `extra` when a screen supplied one, so a
 * screen's own key line (e.g. gbscr_run_demo's "EXIT") silently hid the
 * shell's SELECT-toggle hint. Always paint the shell's keys at x=2; `extra`
 * (when supplied) goes to its right, its start x computed from the shell
 * keys' own measured width (plus a fixed gap) so the two strings never
 * overlap even if a future screen's extra text runs long.
 *
 * D9 (U2c review): UNLESS the screen called gbscr_set_legend(), in which case
 * its up-to-4 lines are joined with "  " into ONE string and painted alone
 * (no base keys, no separate `extra`) -- the caller is responsible for
 * keeping the joined width on screen (ui_ptext_w()), same as any other
 * stretched-mode legend text.
 *
 * D1 fix (U2c 2nd re-verify): gs->legend[i] is now the action word ALONE (the
 * D1 side-bar split at 1:1 needs that), so the stretched bottom bar -- which
 * has no side-bar columns to split across -- re-attaches each slot's key name
 * (kGbscrLegendKeys[i]) here before joining, so it still reads "A EDIT  B
 * SAVE  SEL SIZE  START MORE" instead of silently losing the keys. */
static void gbscr_paint_legend_stretched(const GbScreen* gs, const char* extra) {
  ui_fill_rect(0, 150, 240, 10, RGB15(0, 0, 0));
  if (gbscr_legend_overridden(gs)) {
    /* Bounded join: sniprintf against the REMAINING capacity at each step,
     * never a raw strcat (this file's own golden-rules posture) -- same
     * pattern pdna_main.c's log-line builder uses. */
    char joined[64];
    int n = 0;
    joined[0] = 0;
    for (int i = 0; i < 4; i++) {
      if (!gs->legend[i]) continue;
      int w = sniprintf(joined + n, sizeof(joined) - (size_t)n,
                        "%s%s %s", (n > 0) ? "  " : "", kGbscrLegendKeys[i], gs->legend[i]);
      if (w > 0) n += w;
      if ((size_t)n >= sizeof joined) break;
    }
    ui_ptext_shadow(2, 151, UI_TEXT, RGB15(0, 0, 0), joined);
    return;
  }
  static const char* const kShellKeys = "A OK  B BACK  SEL SIZE";
  ui_ptext_shadow(2, 151, UI_TEXT, RGB15(0, 0, 0), kShellKeys);
  if (extra) {
    int extra_x = 2 + ui_ptext_w(kShellKeys) + 10;
    ui_ptext_shadow(extra_x, 151, UI_TEXT, RGB15(0, 0, 0), extra);
  }
}

void gbscr_flush(GbScreen* gs, const char* legend_extra) {
  if (!gs || !gs->ok) return;

  /* 1:1 mode's legend lives entirely in the side/top/bottom bars OUTSIDE the
   * canvas (x<40, x>=200, y<8, y>=152) -- no cell ever paints there, so order
   * doesn't matter for it. Stretched mode's legend is a bottom SCRIM overlay
   * INSIDE the canvas area (the canvas fills the whole screen there), so it
   * MUST be painted AFTER the cell loop below or a full repaint (every SELECT
   * toggle) blits straight over it and the scrim/legend silently vanishes --
   * caught by looking at the U2a shots, not by inspection. */
  if (gb_scale_mode == 0) gbscr_paint_legend_1to1(gs, legend_extra);

  /* U2b item 1: no FIL, no FusedGbSlice, no SD/cart-space read of any kind here
   * any more -- every dirty cell is served from the RAM tile cache gbscr_open()
   * built. `local` is a per-call copy of gs->gu with .read/.ctx rebound to that
   * cache (gs->gu's own .read/.ctx are stale between calls, same as before). */
  RomGbUi local = gs->gu;
  local.read = gbscr_mem_read;
  local.ctx = &gs->cache;

  for (int cy = 0; cy < GBSCR_ROWS; cy++) {
    for (int cx = 0; cx < GBSCR_COLS; cx++) {
      int idx = cy * GBSCR_COLS + cx;
      if (!dirty_test(gs, idx)) continue;

      uint16_t tile[64];
      bool ok = gbscr_tile_pixels(gs, &local, idx, tile);
      if (!ok) { dirty_clear(gs, idx); continue; }

      if (gb_scale_mode == 0) blit_1to1(cx, cy, tile);
      else                    blit_stretched(cx, cy, tile);
      dirty_clear(gs, idx);
    }
  }

  if (gb_scale_mode != 0) gbscr_paint_legend_stretched(gs, legend_extra);
}

/* U2c: pixel bounds of a `w`x`h` group of cells starting at (cx,cy), in the
 * CURRENT gb_scale_mode -- for a screen's own cursor frame, drawn on the
 * framebuffer directly AFTER gbscr_flush() (never a tile, per design sec
 * "Cursor" -- a coloured 1-px frame, not part of the tilemap). Uses the SAME
 * LUTs/formula blit_stretched() itself reads, so the cursor always outlines
 * exactly the cells it claims to, at either scale -- a screen never has to
 * know the stretch math to draw a correct highlight. `x1`/`y1` are ONE PAST
 * the last covered pixel (m3_frame's own convention). */
void gbscr_cell_rect(int cx, int cy, int w, int h, int* x0, int* y0, int* x1, int* y1) {
  if (gb_scale_mode == 0) {
    if (x0) *x0 = GBSCR_ORIGIN_X + cx * 8;
    if (y0) *y0 = GBSCR_ORIGIN_Y + cy * 8;
    if (x1) *x1 = GBSCR_ORIGIN_X + (cx + w) * 8;
    if (y1) *y1 = GBSCR_ORIGIN_Y + (cy + h) * 8;
    return;
  }
  int sx0 = cx * 8, sx1 = (cx + w) * 8 - 1;
  int sy0 = cy * 8, sy1 = (cy + h) * 8 - 1;
  int g0 = sx0 >> 1, p0 = sx0 & 1;
  int dx0 = 3 * g0 + (p0 ? 1 : 0);
  int g1 = sx1 >> 1, p1 = sx1 & 1;
  int dx1 = 3 * g1 + (p1 ? 1 : 0) + (p1 ? 2 : 1);   /* one past the last dest col */
  int dy0 = gbscr_y_dst0[sy0];
  int dy1 = gbscr_y_dst0[sy1] + gbscr_y_dst_count[sy1];
  if (x0) *x0 = dx0;
  if (y0) *y0 = dy0;
  if (x1) *x1 = dx1;
  if (y1) *y1 = dy1;
}

/* ---------------------------------------------------------------------------
 * U2a's own demo: font sheet + a text-box border + one text line, so the shell
 * can be shot standalone before U2b's real card exists. GbScreen is a plain
 * LOCAL here (not a static, not EWRAM) -- this function's own frame IS the
 * "one noinline frame" the header/design describe; -fstack-usage measures it.
 * --------------------------------------------------------------------------- */
void __attribute__((noinline)) gbscr_run_demo(uint8_t gen) {
  GbScreen gs;
  const char* reason = 0;

  /* U2b item 1: this demo is reached from Settings (no GB session, no g_ed), so
   * it takes its OWN tail buffer via app_arena_acquire() rather than
   * gb12_arena_tail() (which only ever returns non-NULL inside a resident-image
   * GB session) -- released before returning either way. TEXTBOX is the only
   * extra block this demo's own border needs; FONT is always cached. */
  uint32_t need = gbscr_tail_need(gen, GBSCR_NEED_TEXTBOX);
  uint8_t* tail = app_arena_acquire(need);
  if (!tail) {
    snd_deny();   /* the same refusal shape as pdna_gen12.c's arena-busy panel */
    msg_wait("NOT NOW", UI_WARN, "Save the Pokemon you moved,", "then open the GB screen.");
    return;
  }
  if (!gbscr_open(gen, &gs, tail, need, GBSCR_NEED_TEXTBOX, &reason)) {
    app_arena_release();
    msg_wait("GB SCREEN SHELL", UI_WARN, reason ? reason : "unavailable", 0);
    return;
  }

  /* Border: a plain rectangle of TEXTBOX-block tile 0 all the way round --
   * this demo does not claim to be a real dialogue-box frame (that is U2b's
   * job, with the card-frame block's own documented relative offsets); it
   * only has to prove the shell fetches and blits a NON-font ROM block
   * correctly, in both scale modes. */
  for (int x = 0; x < GBSCR_COLS; x++) {
    gbscr_cell(&gs, x, 0, GBSCR_SRC_TEXTBOX, 0);
    gbscr_cell(&gs, x, GBSCR_ROWS - 1, GBSCR_SRC_TEXTBOX, 0);
  }
  for (int y = 0; y < GBSCR_ROWS; y++) {
    gbscr_cell(&gs, 0, y, GBSCR_SRC_TEXTBOX, 0);
    gbscr_cell(&gs, GBSCR_COLS - 1, y, GBSCR_SRC_TEXTBOX, 0);
  }

  /* The located font's whole 128-glyph sheet (charmap 0x80..0xFF), 16 cols x
   * 8 rows, starting just inside the border. */
  for (int i = 0; i < 128; i++) {
    int fx = 2 + (i % 16), fy = 2 + (i / 16);
    gbscr_cell(&gs, fx, fy, GBSCR_SRC_FONT, (uint8_t)(0x80 + i));
  }

  gbscr_text(&gs, 2, 11, "GB SCREEN SHELL");

  for (;;) {
    /* Short on purpose: the 40-px side bar fits "A OK"/"B BACK"/"SEL SIZE" but
     * not a longer fourth line -- A/B/START all just exit this demo (there is
     * nothing to keep/discard, unlike a real screen's commit flow), so "EXIT"
     * is both accurate and narrow enough not to overflow into the canvas. */
    gbscr_flush(&gs, "EXIT");
    u32 k;
    do { VBlankIntrWait(); key_poll(); k = key_hit(KEY_A | KEY_B | KEY_START | KEY_SELECT); } while (!k);
    if (k & KEY_SELECT) gbscr_toggle_scale(&gs);
    else break;
  }
  gbscr_close(&gs);
  app_arena_release();
}

#endif /* !PDNA_GBSCREEN_HOST_TEST */
