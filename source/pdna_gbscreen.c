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
#include "pdna_app.h"         /* PDNA_DIR, app_can_edit, app_gb_rom_path, ... */
#include "pdna_origin_art.h"  /* PDNA_GEN1/2, pdna_origin_art_stack_room       */
#include "ui.h"                /* ui_fill_rect, ui_ptext, ui_ptext_shadow      */
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

void gbscr_raw(GbScreen* gs, int x, int y, const uint8_t* bytes, int n) {
  if (!gs || !gs->ok || !bytes) return;
  for (int i = 0; i < n; i++) {
    int cx = x + i;
    if (cx >= GBSCR_COLS) break;
    gbscr_put_byte(gs, cx, y, bytes[i]);
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

/* ===========================================================================
 * Everything below needs tonc/FatFs: ROM I/O (gbscr_open/close) and the VRAM
 * blit (gbscr_flush). Stubbed out under PDNA_GBSCREEN_HOST_TEST so the pure
 * half above still compiles/links on the PC.
 * =========================================================================== */
#ifndef PDNA_GBSCREEN_HOST_TEST

#define GBSCR_BLANK_COLOR RGB15(31, 31, 31)   /* DMG's own lightest shade (0xF8) */

static const char* const kReasonNoRom   = "no ROM registered";
static const char* const kReasonNoStack = "not enough stack";
static const char* const kReasonOpen    =
  "ROM art unavailable (bad ROM, wrong game, or non-English release)";
static const char* const kReasonBadGen  = "not a Gen-1/Gen-2 request";
static const char* const kReasonNoTail  = "no tile-bank memory";

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

/* U2b item 1: which order the extra (need_mask) blocks are copied into the tail
 * cache, right after FONT -- fixed, so the cache-building loop below and any test
 * that inspects a GbscrCache agree on layout. */
static const GbScrSrc kCacheOptOrder[3] = {
  GBSCR_SRC_TEXTBOX, GBSCR_SRC_CARDFRAME, GBSCR_SRC_BADGES
};

/* Total tail bytes gbscr_open() needs for `need_mask` on generation `gen`:
 * the 2,048-B rom_gbui scan scratch, reused afterward for FONT (always cached)
 * plus every block need_mask names. */
static uint32_t gbscr_tail_need(uint8_t gen, uint16_t need_mask) {
  uint32_t need = ROM_GBUI_SCRATCH_MIN + gbscr_block_bytes(gen, GBSCR_SRC_FONT);
  for (int i = 0; i < 3; i++)
    if (need_mask & (1u << kCacheOptOrder[i])) need += gbscr_block_bytes(gen, kCacheOptOrder[i]);
  return need;
}

/* Bulk-copies one block straight from the (still-open) ROM source into the tail
 * cache -- a raw byte range, no tile decode (rom_gbui_tile()/glyph() do that
 * later, out of RAM, at flush time). `gu` must already be open (its own
 * .read/.ctx are what this reads through). Returns false (and leaves `cache`
 * untouched for this block) on a short/failed read or an unlocated (offset 0 for
 * a src the ROM never actually located -- see FAIL CLOSED in rom_gbui.h: `ok`
 * would already be 0 in that case, so this is a belt-and-braces check). */
static bool gbscr_cache_block(RomGbUi* gu, uint8_t gen, GbScrSrc src,
                              uint8_t* tail, uint32_t* cursor, GbscrCache* cache) {
  uint32_t off = gbscr_block_off(gu, gen, src);
  uint32_t len = gbscr_block_bytes(gen, src);
  if (!off || !len || cache->nblocks >= GBSCR_MAX_BLOCKS) return false;
  if (!gu->read(gu->ctx, off, tail + *cursor, len)) return false;
  cache->blocks[cache->nblocks].rom_off = off;
  cache->blocks[cache->nblocks].ram_off = *cursor;
  cache->blocks[cache->nblocks].len = len;
  cache->nblocks++;
  *cursor += len;
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

  /* U2b item 1: bulk-copy FONT + every need_mask block into `tail`, reusing the
   * SAME bytes the scan scratch above just finished with, then close the FIL --
   * gbscr_flush() never reopens it. gu->read/gu->ctx are still bound to `fil`
   * here (rom_gbui_open_loc() left them that way on success). */
  gs->cache.tail = tail;
  uint32_t cursor = 0;
  bool cok = gbscr_cache_block(&gs->gu, gen, GBSCR_SRC_FONT, tail, &cursor, &gs->cache);
  for (int i = 0; cok && i < 3; i++)
    if (need_mask & (1u << kCacheOptOrder[i]))
      cok = gbscr_cache_block(&gs->gu, gen, kCacheOptOrder[i], tail, &cursor, &gs->cache);
  f_close(&fil);
  if (!cok) { if (reason) *reason = kReasonOpen; return false; }
#else
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(gen, &base, &size)) { if (reason) *reason = kReasonNoRom; return false; }
  FusedGbSlice slice = { base, size };
  /* #62's own posture (fused corpus is immutable for the whole run): no EWRAM
   * loc cache here -- this slice's memory budget forbids any new EWRAM static
   * beyond gb_scale_mode, so a delta-build gbscr_open() always does the full
   * scan. Rare (a screen entry, not a per-frame cost); a future slice may add
   * an EWRAM cache the same way gb_art_source.c's #62 D1 did IF the budget is
   * revisited. */
  int ok = rom_gbui_open(&gs->gu, fused_gb_slice_read, &slice, size, tail, ROM_GBUI_SCRATCH_MIN);
  if (!ok || (uint8_t)gs->gu.gen != gen) { if (reason) *reason = kReasonOpen; return false; }

  gs->cache.tail = tail;
  uint32_t cursor = 0;
  bool cok = gbscr_cache_block(&gs->gu, gen, GBSCR_SRC_FONT, tail, &cursor, &gs->cache);
  for (int i = 0; cok && i < 3; i++)
    if (need_mask & (1u << kCacheOptOrder[i]))
      cok = gbscr_cache_block(&gs->gu, gen, kCacheOptOrder[i], tail, &cursor, &gs->cache);
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
    case GBSCR_SRC_PIC:
      /* U2b: the Gen-1 player pic is a separate compressed codec
       * (gb_sprite_gen1), not a rom_gbui_tile() block. Not implemented here --
       * U2a never requests this src. Falls through to BLANK. */
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

/* Legend rows, 1:1 mode (design sec 1.5: two stacked columns in the 40-px side
 * bars -- kept to ONE column here, the left bar, since the shell's three fixed
 * lines plus one screen-supplied extra fit in 40 px with room to spare and a
 * second column would only matter for a screen with many more keys than any
 * U2..U5 screen actually has). */
static void gbscr_paint_legend_1to1(const char* extra) {
  ui_fill_rect(0, 0, GBSCR_ORIGIN_X, 160, UI_BG);
  ui_fill_rect(GBSCR_ORIGIN_X + GBSCR_COLS * 8, 0,
              240 - (GBSCR_ORIGIN_X + GBSCR_COLS * 8), 160, UI_BG);
  ui_fill_rect(0, 0, 240, GBSCR_ORIGIN_Y, UI_BG);
  ui_fill_rect(0, GBSCR_ORIGIN_Y + GBSCR_ROWS * 8, 240,
              160 - (GBSCR_ORIGIN_Y + GBSCR_ROWS * 8), UI_BG);
  ui_ptext(2, 20, UI_TEXT, "A OK");
  ui_ptext(2, 30, UI_TEXT, "B BACK");
  ui_ptext(2, 40, UI_TEXT, "SEL SIZE");
  if (extra) ui_ptext(2, 52, UI_DIM, extra);
}

/* D3 fix (U2a review): stretched mode used to REPLACE the shell's own keys
 * ("A OK  B BACK  SEL SIZE") with `extra` when a screen supplied one, so a
 * screen's own key line (e.g. gbscr_run_demo's "EXIT") silently hid the
 * shell's SELECT-toggle hint. Always paint the shell's keys at x=2; `extra`
 * (when supplied) goes to its right, its start x computed from the shell
 * keys' own measured width (plus a fixed gap) so the two strings never
 * overlap even if a future screen's extra text runs long. */
static void gbscr_paint_legend_stretched(const char* extra) {
  static const char* const kShellKeys = "A OK  B BACK  SEL SIZE";
  ui_fill_rect(0, 150, 240, 10, RGB15(0, 0, 0));
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
  if (gb_scale_mode == 0) gbscr_paint_legend_1to1(legend_extra);

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

  if (gb_scale_mode != 0) gbscr_paint_legend_stretched(legend_extra);
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
