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
 * The two stretch LUTs (design sec 1.5 / this slice's brief, exact spec).
 * x_lut[240]: for every group of 2 source columns (2k, 2k+1), 2k maps to ONE
 * destination column and 2k+1 (the "every 2nd source column") maps to TWO --
 * 80 groups cover source 0..159 -> destination 0..239, 80 duplicated values.
 * y_lut[160]: for every group of 9 source rows, the last row of the group
 * (the "every 9th source row") maps to TWO destination rows instead of one --
 * 16 groups cover source 0..143 -> destination 0..159, 16 duplicated values.
 * Both are literal const tables (generated once, by hand, from the formula
 * above -- see this slice's own notes) -- no division anywhere in this file
 * touches them; the per-cell blit below uses its OWN small integer formulas
 * (x: shift/mask, period 2; y: the precomputed y_dst0/y_dst_count tables) so
 * that no runtime division ever falls inside the pixel loop either.
 * --------------------------------------------------------------------------- */
const uint8_t gbscr_x_lut[240] = {
  0,1,1,2,3,3,4,5,5,6,7,7,8,9,9,10,11,11,12,13,13,14,15,15,16,17,17,18,19,19,
  20,21,21,22,23,23,24,25,25,26,27,27,28,29,29,30,31,31,32,33,33,34,35,35,
  36,37,37,38,39,39,40,41,41,42,43,43,44,45,45,46,47,47,48,49,49,50,51,51,
  52,53,53,54,55,55,56,57,57,58,59,59,60,61,61,62,63,63,64,65,65,66,67,67,
  68,69,69,70,71,71,72,73,73,74,75,75,76,77,77,78,79,79,80,81,81,82,83,83,
  84,85,85,86,87,87,88,89,89,90,91,91,92,93,93,94,95,95,96,97,97,98,99,99,
  100,101,101,102,103,103,104,105,105,106,107,107,108,109,109,110,111,111,
  112,113,113,114,115,115,116,117,117,118,119,119,120,121,121,122,123,123,
  124,125,125,126,127,127,128,129,129,130,131,131,132,133,133,134,135,135,
  136,137,137,138,139,139,140,141,141,142,143,143,144,145,145,146,147,147,
  148,149,149,150,151,151,152,153,153,154,155,155,156,157,157,158,159,159
};

const uint8_t gbscr_y_lut[160] = {
  0,1,2,3,4,5,6,7,8,8,9,10,11,12,13,14,15,16,17,17,18,19,20,21,22,23,24,25,
  26,26,27,28,29,30,31,32,33,34,35,35,36,37,38,39,40,41,42,43,44,44,45,46,
  47,48,49,50,51,52,53,53,54,55,56,57,58,59,60,61,62,62,63,64,65,66,67,68,
  69,70,71,71,72,73,74,75,76,77,78,79,80,80,81,82,83,84,85,86,87,88,89,89,
  90,91,92,93,94,95,96,97,98,98,99,100,101,102,103,104,105,106,107,107,108,
  109,110,111,112,113,114,115,116,116,117,118,119,120,121,122,123,124,125,
  125,126,127,128,129,130,131,132,133,134,134,135,136,137,138,139,140,141,
  142,143,143
};

/* Reverse of gbscr_y_lut above, precomputed so the per-cell blit never divides
 * by 9: for source row r (0..143), y_dst0[r] is the FIRST destination row it
 * shows on, and y_dst_count[r] (1 or 2) how many consecutive destination rows.
 * x needs no equivalent table -- its period is 2, so `s>>1`/`s&1` are exact and
 * division-free already. */
static const uint8_t y_dst0[144] = {
  0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,16,17,18,20,21,22,23,24,25,26,27,28,
  30,31,32,33,34,35,36,37,38,40,41,42,43,44,45,46,47,48,50,51,52,53,54,55,
  56,57,58,60,61,62,63,64,65,66,67,68,70,71,72,73,74,75,76,77,78,80,81,82,
  83,84,85,86,87,88,90,91,92,93,94,95,96,97,98,100,101,102,103,104,105,106,
  107,108,110,111,112,113,114,115,116,117,118,120,121,122,123,124,125,126,
  127,128,130,131,132,133,134,135,136,137,138,140,141,142,143,144,145,146,
  147,148,150,151,152,153,154,155,156,157,158
};
static const uint8_t y_dst_count[144] = {
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
  if (gs) gbscr_mark_all_dirty(gs);
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

bool __attribute__((noinline)) gbscr_open(uint8_t gen, GbScreen* gs, const char** reason) {
  if (reason) *reason = 0;
  if (!gs) return false;
  memset(gs, 0, sizeof *gs);
  gs->gen = gen;
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) { if (reason) *reason = kReasonBadGen; return false; }
  if (!pdna_origin_art_stack_room(PDNA_GB_UI_NEED)) { if (reason) *reason = kReasonNoStack; return false; }

#ifndef PDNA_DELTA
  char path[GB_ROM_PATH_MAX];
  if (!gbscr_resolve_path(gen, path, (int)sizeof path)) { if (reason) *reason = kReasonNoRom; return false; }

  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) { if (reason) *reason = kReasonOpen; return false; }
  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  uint8_t scratch[ROM_GBUI_SCRATCH_MIN];
  RomGbUiLoc loc;
  bool have_loc = gbscr_load_loc(gen, &loc);
  int ok = rom_gbui_open_loc(&gs->gu, gbscr_sd_read, &fil, sz, scratch, (uint32_t)sizeof scratch,
                             have_loc ? &loc : 0);
  f_close(&fil);
  if (!ok || (uint8_t)gs->gu.gen != gen) { if (reason) *reason = kReasonOpen; return false; }
  if (!have_loc || loc.id_hash != gs->gu.id_hash || loc.size != sz) {
    RomGbUiLoc fresh;
    rom_gbui_save_loc(&gs->gu, &fresh);
    gbscr_save_loc(gen, &fresh);
  }
  strncpy(gs->rom_path, path, sizeof gs->rom_path - 1);
  gs->rom_path[sizeof gs->rom_path - 1] = 0;
#else
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(gen, &base, &size)) { if (reason) *reason = kReasonNoRom; return false; }
  FusedGbSlice slice = { base, size };
  uint8_t scratch[ROM_GBUI_SCRATCH_MIN];
  /* #62's own posture (fused corpus is immutable for the whole run): no EWRAM
   * loc cache here -- this slice's memory budget forbids any new EWRAM static
   * beyond gb_scale_mode, so a delta-build gbscr_open() always does the full
   * scan. Rare (a screen entry, not a per-frame cost); a future slice may add
   * an EWRAM cache the same way gb_art_source.c's #62 D1 did IF the budget is
   * revisited. */
  int ok = rom_gbui_open(&gs->gu, fused_gb_slice_read, &slice, size, scratch, (uint32_t)sizeof scratch);
  if (!ok || (uint8_t)gs->gu.gen != gen) { if (reason) *reason = kReasonOpen; return false; }
  gs->rom_base = base; gs->rom_size = size;
#endif

  gs->ok = true;
  gbscr_mark_all_dirty(gs);
  return true;
}

void gbscr_close(GbScreen* gs) {
  if (!gs) return;
  gs->ok = false;   /* no persistent handle to release -- see the header's own note */
}

/* One cell's 8x8 RGB15 pixels, looked up through `local` (a RomGbUi copy whose
 * .ctx/.read the caller has just rebound to a live FIL/FusedGbSlice). BLANK
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
 * division-free); y uses the precomputed y_dst0/y_dst_count tables above (also
 * division-free) -- no runtime division anywhere in this loop. */
static void blit_stretched(int cx, int cy, const uint16_t tile[64]) {
  int sx0 = cx * 8;
  for (int ly = 0; ly < 8; ly++) {
    int sy = cy * 8 + ly;
    int dy0 = y_dst0[sy], dyn = y_dst_count[sy];
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

static void gbscr_paint_legend_stretched(const char* extra) {
  ui_fill_rect(0, 150, 240, 10, RGB15(0, 0, 0));
  ui_ptext_shadow(2, 151, UI_TEXT, RGB15(0, 0, 0),
                  extra ? extra : "A OK  B BACK  SEL SIZE");
}

void gbscr_flush(GbScreen* gs, const char* legend_extra) {
  if (!gs || !gs->ok) return;

  if (gb_scale_mode == 0) gbscr_paint_legend_1to1(legend_extra);
  else                    gbscr_paint_legend_stretched(legend_extra);

#ifndef PDNA_DELTA
  FIL fil; bool fil_open = false;
#else
  FusedGbSlice slice = { gs->rom_base, gs->rom_size };
#endif

  for (int cy = 0; cy < GBSCR_ROWS; cy++) {
    for (int cx = 0; cx < GBSCR_COLS; cx++) {
      int idx = cy * GBSCR_COLS + cx;
      if (!dirty_test(gs, idx)) continue;

      bool need_rom = (GbScrSrc)gs->src[idx] != GBSCR_SRC_BLANK;
      RomGbUi local = gs->gu;
#ifndef PDNA_DELTA
      if (need_rom) {
        if (!fil_open) {
          memset(&fil, 0, sizeof fil);
          fil_open = (f_open(&fil, gs->rom_path, FA_READ) == FR_OK);
        }
        if (!fil_open) { dirty_clear(gs, idx); continue; }
        local.read = gbscr_sd_read; local.ctx = &fil;
      }
#else
      if (need_rom) { local.read = fused_gb_slice_read; local.ctx = &slice; }
#endif
      uint16_t tile[64];
      bool ok = gbscr_tile_pixels(gs, &local, idx, tile);
      if (!ok) { dirty_clear(gs, idx); continue; }

      if (gb_scale_mode == 0) blit_1to1(cx, cy, tile);
      else                    blit_stretched(cx, cy, tile);
      dirty_clear(gs, idx);
    }
  }

#ifndef PDNA_DELTA
  if (fil_open) f_close(&fil);
#endif
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
  if (!gbscr_open(gen, &gs, &reason)) {
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
    gbscr_flush(&gs, "START DONE");
    u32 k;
    do { VBlankIntrWait(); key_poll(); k = key_hit(KEY_A | KEY_B | KEY_START | KEY_SELECT); } while (!k);
    if (k & KEY_SELECT) gbscr_toggle_scale(&gs);
    else break;
  }
  gbscr_close(&gs);
}

#endif /* !PDNA_GBSCREEN_HOST_TEST */
