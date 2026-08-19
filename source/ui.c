#include "ui.h"
#include "data_tables.h"   /* pk_type_name for the artless type chip */
#include <string.h>
#include "rumble.h"   /* rumble_io_suspend/resume: mute the cart-bus motor toggle while a blit reads ROM */
#include "ui_font.h"  /* our proportional 5x7 face (generated) — see ui_ptext below */

void ui_init(void) {
  REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;
  tte_init_bmp(DCNT_MODE3, &sys8Font, NULL);  /* fixed 8x8 font on the M3 bitmap */
  tte_set_paper(UI_BG);
}

void ui_clear(void) {
  m3_fill(UI_BG);
}

void ui_panel(int x, int y, int w, int h, u16 fill, u16 border) {
  m3_rect(x, y, x + w, y + h, fill);
  m3_frame(x, y, x + w - 1, y + h - 1, border);
}

/* Same as ui_panel, but the fill is BLENDED with whatever is already on screen instead of
 * replacing it. Mode 3 is a direct-colour framebuffer with no hardware BG blending available
 * to a software-drawn panel, so the mix is done per pixel: each BGR555 channel is averaged
 * with weight `num`/8 toward `fill`. num=8 is opaque, num=4 is half.
 *
 * Cost is one read-modify-write per pixel — about 26k pixels for a full menu panel, well
 * under a frame, and it only runs on a redraw rather than every frame. */
void ui_panel_alpha(int x, int y, int w, int h, u16 fill, u16 border, int num) {
  if (num < 0) num = 0;
  if (num > 8) num = 8;
  int fr = fill & 31, fg = (fill >> 5) & 31, fb = (fill >> 10) & 31;
  for (int py = y; py < y + h; py++) {
    if ((unsigned)py >= 160u) continue;
    u16* row = (u16*)MEM_VRAM + py * 240;
    for (int px = x; px < x + w; px++) {
      if ((unsigned)px >= 240u) continue;
      u16 c = row[px];
      int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
      r += ((fr - r) * num) >> 3;
      g += ((fg - g) * num) >> 3;
      b += ((fb - b) * num) >> 3;
      row[px] = (u16)(r | (g << 5) | (b << 10));
    }
  }
  m3_frame(x, y, x + w - 1, y + h - 1, border);
}

/* See ui.h for the contract. The arithmetic itself lives in ui_layout.h (pure C) so
 * tests/host_textfit_test.c can run THIS code instead of a mirror of it — a mirrored
 * copy of the formula in the test would keep passing after this one changed. */
int ui_popup_vfit(int nrows, int row_h, int head, int foot, int* out_y, int* out_h) {
  return ui_popup_fit(nrows, row_h, head, foot, out_y, out_h);
}

/* Same, laid out against a caller-supplied footer row instead of the global
 * UI_FOOTER_Y — see ui_layout.h's ui_popup_fit_at for why (app_mon_menu drawn over the
 * party overlay, whose own message box starts well above UI_FOOTER_Y). */
int ui_popup_vfit_at(int nrows, int row_h, int head, int foot, int footer_y, int* out_y, int* out_h) {
  return ui_popup_fit_at(nrows, row_h, head, foot, footer_y, out_y, out_h);
}

void ui_hline(int x, int y, int w, u16 color) {
  m3_line(x, y, x + w - 1, y, color);
}

void ui_text(int x, int y, u16 ink, const char* s) {
  tte_set_ink(ink);
  tte_set_pos(x, y);
  tte_write(s);
}

void ui_text_sel(int x, int y, int w, bool selected, u16 ink, const char* s) {
  if (selected) m3_rect(x, y, x + w, y + UI_ROW_H, UI_SEL);
  tte_set_ink(selected ? UI_SELTEXT : ink);
  tte_set_pos(x + 1, y);
  tte_write(s);
}

void ui_icon16(int x, int y, const u16* icon) {
  if (!icon) return;
  for (int j = 0; j < 16; j++) {
    for (int i = 0; i < 16; i++) {
      u16 p = icon[j * 16 + i];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
  }
}

void ui_fill_rect(int x, int y, int w, int h, u16 color) {
  m3_rect(x, y, x + w, y + h, color);
}

/* Outlined progress/stat bar: `track` background, `fill` for the first
 * `filled` pixels (clamped to [0,w]), `border` frame.
 *
 * The border call USED to read `m3_frame(x, y, x + w - 1, y + h - 1, border)`, copying
 * the `w-1`/`h-1` idiom every ui_panel* function in this file uses. That idiom is wrong
 * for libtonc's actual m3_frame: measured directly off a live mGBA capture of the party
 * HP bar (bw=50, h=7 — docs/analysis-2026-08-19-party/, sampling columns x=183/190/
 * 231/232 and row y=16 pixel-by-pixel), m3_frame(left,top,right,bottom) draws its right
 * column at right-1 and its bottom row at bottom-1 — i.e. it treats (right,bottom) as
 * EXCLUSIVE, the SAME convention m3_rect already uses, not "inclusive, minus one" as the
 * `w-1`/`h-1` call sites assume. With the old call the frame's right/bottom edges landed
 * ONE PIXEL INSIDE m3_rect's true fill edge, leaving a `fill`-coloured sliver past the
 * border on every progress bar's right and bottom side — invisible on the app's other
 * six ui_progress callers (fill and border are both muted greys/blues there) but glaring
 * on the party HP bar's bright green fill against a dark outline, which is how this got
 * caught. Passing `x + w, y + h` (matching m3_rect's own parameters exactly, since frame
 * and rect share this convention) draws the border flush with the fill on all four sides.
 *
 * SCOPE: only THIS function's own frame call changed. ui_panel/ui_panel_alpha/
 * ui_panel_striped (and the two bare m3_frame calls further down this file) keep their
 * existing `w-1`/`h-1` idiom — they are the app's whole panel-drawing vocabulary, used
 * on dozens of screens, and re-deriving every one of those coordinates is a different,
 * much larger change than the one the party HP bar needed. ui_progress has its own
 * private frame call shared by exactly 7 sites (party + 6 pre-existing bars), none of
 * which relied on the old (wrong) inset — this fixes all 7 consistently. */
void ui_progress(int x, int y, int w, int h, int filled, u16 fill, u16 track, u16 border) {
  if (filled < 0) filled = 0;
  if (filled > w) filled = w;
  m3_rect(x, y, x + w, y + h, track);
  if (filled > 0) m3_rect(x, y, x + filled, y + h, fill);
  m3_frame(x, y, x + w, y + h, border);
}

/* Blit a w×h RGB15 sprite (0 = transparent, 0x8000|RGB15 = opaque). Same pixel
 * format as ui_icon16 and the mon_icons / mon_front generators. */
void ui_sprite(int x, int y, int w, int h, const u16* data) {
  if (!data) return;
  rumble_io_suspend();                       /* data may be in ROM: don't let the motor toggle mid-read */
  /* Row pointers instead of m3_plot: this is the 64x64 mon portrait, so the old form
   * recomputed the destination address 4,096 times per box flip with an occupied cell
   * under the cursor. It cannot be word-blitted — the 0x8000 key is per pixel — so
   * hoisting the row base is the available win. Same pixels, same key. */
  for (int j = 0; j < h; j++) {
    const u16* s = data + (unsigned)j * (unsigned)w;
    u16*       d = &vid_mem[(unsigned)(y + j) * 240u + (unsigned)x];
    for (int i = 0; i < w; i++) { u16 p = s[i]; if (p & 0x8000) d[i] = (u16)(p & 0x7FFF); }
  }
  rumble_io_resume();
}

/* Blit a 32x32 RGB15 sprite shrunk to 16x16 (sample every other pixel) — for
 * compact list rows where the full 32x32 icon won't fit. */
/* Nearest-neighbour blit of a 32x32 (0x8000-keyed) icon at an arbitrary dst size
 * — for grids that want icons bigger than the 16x16 sub but not the full 32. */
/* `si` depends only on i and dw, so it was being recomputed dh times over for every
 * column: dh*dw software divisions per icon (1,056 for the 32x32 the Pokedex grid
 * actually asks for, ~22,000 for a full page of 21 cells) on a CPU with no divide
 * instruction. Build the column table once. Same expression, same pixels. */
#define UI_ICON_SCALE_MAX 64
static int ui_scale_cols(int dw, unsigned char* si) {
  if (dw > UI_ICON_SCALE_MAX) return 0;      /* caller falls back to the divide */
  for (int i = 0; i < dw; i++) si[i] = (unsigned char)(i * 32 / dw);
  return 1;
}

void ui_icon_scaled(int x, int y, int dw, int dh, const u16* src32) {
  if (!src32) return;
  unsigned char sic[UI_ICON_SCALE_MAX];
  int tbl = ui_scale_cols(dw, sic);
  rumble_io_suspend();                       /* src32 may be a raw ROM icon pointer */
  for (int j = 0; j < dh; j++) {
    const u16* srow = src32 + (j * 32 / dh) * 32;
    for (int i = 0; i < dw; i++) {
      u16 p = srow[tbl ? sic[i] : (i * 32 / dw)];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
  }
  rumble_io_resume();
}

/* Cheap greyscale of a 15-bit BGR555 colour: luma = (2R+5G+B)/8, written R=G=B. */
static u16 ui_grey15(u16 c) {
  u32 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
  u32 y = (r * 2 + g * 5 + b) >> 3;          /* 0..31 */
  if (y > 31) y = 31;
  return (u16)(y | (y << 5) | (y << 10));
}

void ui_icon_scaled_grey(int x, int y, int dw, int dh, const u16* src32) {
  if (!src32) return;
  unsigned char sic[UI_ICON_SCALE_MAX];      /* see ui_icon_scaled */
  int tbl = ui_scale_cols(dw, sic);
  rumble_io_suspend();                       /* src32 may be a raw ROM icon pointer */
  for (int j = 0; j < dh; j++) {
    const u16* srow = src32 + (j * 32 / dh) * 32;
    for (int i = 0; i < dw; i++) {
      u16 p = srow[tbl ? sic[i] : (i * 32 / dw)];
      if (p & 0x8000) m3_plot(x + i, y + j, ui_grey15((u16)(p & 0x7FFF)));
    }
  }
  rumble_io_resume();
}

/* 11×11 Poké Ball (region codes: 0 transparent, 1 black outline/band, 2 red top,
 * 3 white bottom, 4 white centre button). A proper ball — red dome, black equator
 * band, white bottom, small centre button. One shared procedural glyph. */
void ui_pokeball(int x, int y) {
  static const char B[11][12] = {
    "...11111...",
    "..1222221..",
    ".122222221.",
    "12222222221",
    "12221112221",
    "11111411111",
    "13331113331",
    "13333333331",
    ".133333331.",
    "..1333331..",
    "...11111...",
  };
  const u16 OUT = RGB15(1, 1, 2), RED = RGB15(28, 4, 4),
            WHT = RGB15(30, 30, 31), BTN = RGB15(31, 31, 31);
  for (int j = 0; j < 11; j++)
    for (int i = 0; i < 11; i++) {
      char c = B[j][i];
      u16 col; switch (c) { case '1': col = OUT; break; case '2': col = RED; break;
                            case '3': col = WHT; break; case '4': col = BTN; break;
                            default: continue; }
      m3_plot(x + i, y + j, col);
    }
}

static u16 __attribute__((aligned(4))) s_ovl_line[240];   /* compose buffer; aligned for 32-bit DMA */
void ui_blit_over(int x, int y, int w, int h, const u16* data, u16 bg) {
  if (!data || w > 240) return;
  /* 32-bit DMA needs a word-aligned destination (even x AND even w); otherwise fall
   * back to a u16 CPU copy, which is valid at any halfword address. Either way each
   * row is composed (sprite over bg) and written in ONE pass — no separate erase, so
   * an animating sprite never blinks even at an odd x (e.g. the Pokedex grid, cw=33). */
  bool dma = ((x & 1) == 0) && ((w & 1) == 0);
  rumble_io_suspend();                        /* data may be in ROM (compose reads it per pixel) */
  for (int j = 0; j < h; j++) {
    const u16* srow = data + (uint32_t)j * w;
    for (int i = 0; i < w; i++) { u16 p = srow[i]; s_ovl_line[i] = (p & 0x8000) ? (u16)(p & 0x7FFF) : bg; }
    u16* dst = &vid_mem[(y + j) * 240 + x];
    if (dma) dma3_cpy(dst, s_ovl_line, (u32)w * 2);
    else     for (int i = 0; i < w; i++) dst[i] = s_ovl_line[i];
  }
  rumble_io_resume();
}

/* Restore a rect from a full-screen ROM background (same 240-px stride as VRAM).
 * Word-aligned rows go through 32-bit DMA; odd x/w falls back to a u16 CPU copy. */

void ui_truncate(char* out, const char* in, int max_cols) {
  if (max_cols < 1) { out[0] = 0; return; }
  int cols = 0, i = 0, o = 0, last_start = 0;
  while (in[i] && cols < max_cols) {
    unsigned char c = (unsigned char)in[i];
    if ((c & 0xC0) != 0x80) { last_start = o; cols++; }  /* UTF-8 lead = new col */
    out[o++] = in[i++];
  }
  if (in[i]) {                 /* more remained -> turn the last column into '~' */
    o = last_start;
    out[o++] = '~';
  }
  out[o] = 0;
}

/* ---------------------------------------------------------------------------
 * Proportional text.
 *
 * TTE is not used here. tonc can carry a `widths` table on a TFont, but going
 * through TTE means saving/restoring font+ink+cursor state around every call, and
 * the glyph procs are a black box for clipping. A 1bpp blit into the Mode 3
 * framebuffer is twenty lines, clips exactly, and lets a caller MEASURE first —
 * which is the whole point: the bag and the box panel need to ask "does the real
 * name fit?" before deciding to shorten it.
 *
 * Font data: source/ui_font.c (ours, generated). Bit 0 of a row byte is the
 * leftmost pixel; rows 0..6 are the body, row 7 carries descenders.
 * ------------------------------------------------------------------------- */

#define PGLYPH(c) (&ui_font_bits[((unsigned)(c) - 32) * 8])
#define PADV(c)   (ui_font_w[(unsigned)(c) - 32])

/* Map any byte onto a drawable glyph. Gen-3 name tables are plain ASCII once
 * decoded, but a corrupt save can hand us anything, and drawing a random 8 KB
 * past the table would be a lot worse than printing '?'. */
static inline unsigned pchar(unsigned char c) {
  return (c >= 32 && c <= 127) ? c : (unsigned)'?';
}

/* Next glyph, advancing `*ps` past the bytes it consumed. The only non-ASCII the data
 * tables contain is 'e'-acute, because the games spell it "POKeMON" with an accent — that
 * arrives as the two-byte UTF-8 sequence C3 A9 and gets the glyph parked at code 127.
 * Any other non-ASCII collapses to '?' with its continuation bytes skipped, so a corrupt
 * string can never desynchronise the walk. */
static unsigned pnext(const char** ps) {
  const unsigned char* p = (const unsigned char*)*ps;
  unsigned c = *p++;
  if (c == 0xC3u && *p == 0xA9u) { c = 127u; p++; }
  else if (c >= 0x80u) {
    while ((*p & 0xC0u) == 0x80u) p++;
    c = (unsigned)'?';
  }
  *ps = (const char*)p;
  return c;
}

int ui_ptext_w(const char* s) {
  int w = 0;
  while (*s) w += PADV(pnext(&s));
  return w;
}

int ui_ptext(int x, int y, u16 ink, const char* s) {
  while (*s) {
    unsigned c = pnext(&s);
    const unsigned char* gl = PGLYPH(c);
    for (int r = 0; r < 8; r++) {
      int py = y + r;
      if ((unsigned)py >= (unsigned)UI_SCR_H) continue;
      unsigned bits = gl[r];
      u16* row = &vid_mem[py * UI_SCR_W];
      while (bits) {
        int col = 0;
        while (!((bits >> col) & 1u)) col++;      /* lowest set bit = leftmost ink */
        bits &= ~(1u << col);
        int px = x + col;
        if ((unsigned)px < (unsigned)UI_SCR_W) row[px] = ink;
      }
    }
    x += PADV(c);
  }
  return x;
}

int ui_ptext_fit(int x, int y, int maxw, u16 ink, const char* s) {
  int full = ui_ptext_w(s);
  if (full <= maxw) return ui_ptext(x, y, ink, s) - x;

  /* Does not fit: keep as many glyphs as leave room for the '~' marker. Byte offsets are
   * taken BEFORE each glyph so a two-byte sequence is never cut in half. */
  int tw = PADV((unsigned)'~');
  const char* p = s;
  int w = 0;
  while (*p) {
    const char* q = p;
    int a = PADV(pnext(&p));
    if (w + a + tw > maxw) { p = q; break; }
    w += a;
  }
  int n = (int)(p - s);
  char buf[64];
  if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
  for (int i = 0; i < n; i++) buf[i] = s[i];
  buf[n] = '~';
  buf[n + 1] = 0;
  return ui_ptext(x, y, ink, buf) - x;
}

int ui_ptext_right(int right, int y, u16 ink, const char* s) {
  return ui_ptext(right - ui_ptext_w(s), y, ink, s);
}

int ui_pchar_w(char c) { return PADV(pchar((unsigned char)c)); }

/* One wrap step: how many bytes of `s` fit in `maxw`, and how many to skip after.
 * Returns the byte count to DRAW; *skip receives the count to advance past
 * (the same plus a consumed space). */
int ui_ptext_break(const char* s, int maxw, int* skip) {
  const char* p = s;
  int w = 0, last_space = -1;
  while (*p) {
    if (*p == ' ') last_space = (int)(p - s);
    const char* q = p;
    int a = PADV(pnext(&p));
    if (w + a > maxw) { p = q; break; }
    w += a;
  }
  int i = (int)(p - s);
  if (!s[i]) { *skip = i; return i; }             /* the rest fits */
  if (last_space > 0) { *skip = last_space + 1; return last_space; }
  *skip = i ? i : 1;                              /* one word wider than the line */
  return *skip;
}

int ui_ptext_wrap(int x, int y, int maxw, int line_h, int max_lines, u16 ink, const char* s) {
  int lines = 0;
  char buf[64];
  while (*s && (max_lines <= 0 || lines < max_lines)) {
    int skip, n = ui_ptext_break(s, maxw, &skip);
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    for (int i = 0; i < n; i++) buf[i] = s[i];
    buf[n] = 0;
    ui_ptext(x, y + lines * line_h, ink, buf);
    s += skip;
    lines++;
  }
  return lines;
}

int ui_ptext_wrap_lines(int maxw, const char* s) {
  int lines = 0;
  while (*s) { int skip; ui_ptext_break(s, maxw, &skip); s += skip; lines++; }
  return lines;
}

/* ---- hard 1px drop-shadow text (see ui.h) --------------------------------
 * Shadow FIRST, ink SECOND: the shadow glyph is the same shape offset by
 * (1,1), so drawing it first and the real glyph on top leaves exactly the
 * shadow's non-overlapping pixels visible — a genuine hard offset, not a
 * blend. Retail draws this on every name/level/HP-number glyph on the party
 * screen (UI_PTY_TEXT_SHADOW, RGB(115,115,115) measured off the cartridge). */
int ui_ptext_shadow(int x, int y, u16 ink, u16 shadow, const char* s) {
  ui_ptext(x + 1, y + 1, shadow, s);
  return ui_ptext(x, y, ink, s);
}

int ui_ptext_fit_shadow(int x, int y, int maxw, u16 ink, u16 shadow, const char* s) {
  ui_ptext_fit(x + 1, y + 1, maxw, shadow, s);
  return ui_ptext_fit(x, y, maxw, ink, s);
}

/* ---- TIGHT-SPACING variant (party-name text only) ------------------------
 *
 * A whole separate path, not a parameter on the functions above: every existing
 * ui_ptext* caller (there are dozens) must keep drawing byte-identical output, so the
 * shared PADV()-based functions above are untouched and this is additive.
 *
 * padv_tight() below is the only new arithmetic; ui_ptext_w_tight/ui_ptext_tight/
 * ui_ptext_fit_tight are the exact same bodies as ui_ptext_w/ui_ptext/ui_ptext_fit with
 * PADV replaced by padv_tight — see UI_PTEXT_TIGHT_DELTA's own comment (ui_font.h) for
 * why 2 px, not 1, is the smallest integer delta that clears the party name column. */
static inline int padv_tight(unsigned c) {
  int a = (int)PADV(c) - UI_PTEXT_TIGHT_DELTA;
  return a < 1 ? 1 : a;             /* never zero/negative: a stuck cursor would hang
                                      * ui_ptext_fit_tight's "does it fit" walk. */
}

int ui_ptext_w_tight(const char* s) {
  int w = 0;
  while (*s) w += padv_tight(pnext(&s));
  return w;
}

int ui_ptext_tight(int x, int y, u16 ink, const char* s) {
  while (*s) {
    unsigned c = pnext(&s);
    const unsigned char* gl = PGLYPH(c);
    for (int r = 0; r < 8; r++) {
      int py = y + r;
      if ((unsigned)py >= (unsigned)UI_SCR_H) continue;
      unsigned bits = gl[r];
      u16* row = &vid_mem[py * UI_SCR_W];
      while (bits) {
        int col = 0;
        while (!((bits >> col) & 1u)) col++;
        bits &= ~(1u << col);
        int px = x + col;
        if ((unsigned)px < (unsigned)UI_SCR_W) row[px] = ink;
      }
    }
    x += padv_tight(c);
  }
  return x;
}

int ui_ptext_fit_tight(int x, int y, int maxw, u16 ink, const char* s) {
  int full = ui_ptext_w_tight(s);
  if (full <= maxw) return ui_ptext_tight(x, y, ink, s) - x;

  int tw = padv_tight((unsigned)'~');
  const char* p = s;
  int w = 0;
  while (*p) {
    const char* q = p;
    int a = padv_tight(pnext(&p));
    if (w + a + tw > maxw) { p = q; break; }
    w += a;
  }
  int n = (int)(p - s);
  char buf[64];
  if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
  for (int i = 0; i < n; i++) buf[i] = s[i];
  buf[n] = '~';
  buf[n + 1] = 0;
  return ui_ptext_tight(x, y, ink, buf) - x;
}

int ui_ptext_fit_shadow_tight(int x, int y, int maxw, u16 ink, u16 shadow, const char* s) {
  ui_ptext_fit_tight(x + 1, y + 1, maxw, shadow, s);
  return ui_ptext_fit_tight(x, y, maxw, ink, s);
}

/* ---- retail-style banded-stripe fill (see ui.h) --------------------------
 * A flat colour swapped for a 1px horizontal hatch, alternating every
 * scanline — matches the retail party screen's background exactly (measured
 * at two adjacent scanlines, docs/analysis-2026-08-19-party/MEASUREMENTS.md).
 * m3_line is already the primitive ui_hline wraps, so this costs the same
 * one-row-at-a-time draw ui_clear's m3_fill pays, just alternating colour. */
void ui_stripe_bg(int marginW, u16 margin, u16 a, u16 b) {
  if (marginW > 0) m3_rect(0, 0, marginW, UI_SCR_H, margin);
  for (int y = 0; y < UI_SCR_H; y++)
    m3_line(marginW, y, UI_SCR_W - 1, y, (y & 1) ? b : a);
}

void ui_panel_striped(int x, int y, int w, int h, u16 a, u16 b, u16 border) {
  for (int r = 0; r < h; r++) {
    int py = y + r;
    if ((unsigned)py >= (unsigned)UI_SCR_H) continue;
    m3_line(x, py, x + w - 1, py, (r & 1) ? b : a);
  }
  m3_frame(x, y, x + w - 1, y + h - 1, border);
}

/* ---- coloured gender glyphs (see ui.h) ------------------------------------
 * A small ring (outline `line`, interior `fill`) with a diagonal tick for
 * male or a cross for female below it — retail draws a genuine graphical
 * glyph here, not a text letter (MEASUREMENTS.md "Gender indicator"). Hand-
 * plotted rather than a bitmap table: the shape is nine rows at most and a
 * table would cost more to read than these plot calls. */
static void gender_ring(int x, int y, u16 fill, u16 line) {
  m3_plot(x + 1, y,     line); m3_plot(x + 2, y,     line); m3_plot(x + 3, y,     line);
  m3_plot(x,     y + 1, line); m3_plot(x + 4, y + 1, line);
  m3_plot(x,     y + 2, line); m3_plot(x + 4, y + 2, line);
  m3_plot(x,     y + 3, line); m3_plot(x + 4, y + 3, line);
  m3_plot(x + 1, y + 4, line); m3_plot(x + 2, y + 4, line); m3_plot(x + 3, y + 4, line);
  for (int r = 1; r <= 3; r++)
    for (int c = 1; c <= 3; c++) m3_plot(x + c, y + r, fill);
}

void ui_gender_glyph_m(int x, int y, u16 fill, u16 line) {
  gender_ring(x, y + 3, fill, line);                 /* ring sits low, arrow points up-right */
  m3_plot(x + 4, y + 2, fill); m3_plot(x + 5, y + 1, fill);
  m3_plot(x + 5, y,     line); m3_plot(x + 4, y,     line); m3_plot(x + 6, y + 1, line);
}

void ui_gender_glyph_f(int x, int y, u16 fill, u16 line) {
  gender_ring(x, y, fill, line);                     /* ring sits high, stem+cross below */
  m3_plot(x + 2, y + 5, fill); m3_plot(x + 2, y + 6, fill);
  m3_plot(x + 1, y + 6, fill); m3_plot(x + 3, y + 6, fill);
}

/* ---- artless fallbacks (see ui.h) ---------------------------------------- */
/* Clip a label to `maxw` px WITHOUT ui_ptext_fit's '~' marker: on a 22 px chip the
 * marker would eat a third of the label ("MA~"); a clean prefix ("MAG") reads better. */
static void chip_label(char t[12], const char* name, int maxw) {
  int n = 0;
  while (name[n] && n < 11) {
    t[n] = name[n]; t[n + 1] = 0;
    if (ui_ptext_w(t) > maxw) { t[n] = 0; break; }
    n++;
  }
}

void ui_name_chip(int x, int y, int w, int h, u16 bg, u16 ink, const char* name) {
  ui_fill_rect(x, y, w, h, bg);
  m3_frame(x, y, x + w - 1, y + h - 1, UI_BORDER);
  char t[12]; t[0] = 0; chip_label(t, name, w - 4);
  ui_ptext(x + 2, y + (h - 8) / 2 + 1, ink, t);
}

/* Original per-type colours (Gen-3 internal type ids 0..17; 9 is the unused slot).
 * Our own picks — the CHIP is the fallback for the ripped badge art. */
static const u16 k_type_col[18] = {   /* literal RGB15 (RGB15() is not a constant expr here) */
  0x4EB5, /* Normal   */ 0x1D38, /* Fighting */
  0x7271, /* Flying   */ 0x5134, /* Poison   */
  0x2A78, /* Ground   */ 0x3676, /* Rock     */
  0x22D4, /* Bug      */ 0x494E, /* Ghost    */
  0x5652, /* Steel    */ 0x39CE, /* ???      */
  0x199E, /* Fire     */ 0x75C8, /* Water    */
  0x2ACA, /* Grass    */ 0x1B5E, /* Electric */
  0x417E, /* Psychic  */ 0x732D, /* Ice      */
  0x714D, /* Dragon   */ 0x210A, /* Dark     */
};

void ui_type_chip(int x, int y, int w, int h, uint8_t type_id) {
  if (type_id >= 18) return;
  ui_fill_rect(x, y, w, h, k_type_col[type_id]);
  m3_frame(x, y, x + w - 1, y + h - 1, UI_BORDER);
  char t[12]; t[0] = 0; chip_label(t, pk_type_name(type_id), w - 4);
  int tw = ui_ptext_w(t);
  ui_ptext(x + (w - tw) / 2, y + (h - 8) / 2 + 1, 0x7FFF, t);
}
