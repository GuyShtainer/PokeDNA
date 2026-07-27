#include "ui.h"
#include <string.h>
#include "rumble.h"   /* rumble_io_suspend/resume: mute the cart-bus motor toggle while a blit reads ROM */

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
 * `filled` pixels (clamped to [0,w]), `border` frame. */
void ui_progress(int x, int y, int w, int h, int filled, u16 fill, u16 track, u16 border) {
  if (filled < 0) filled = 0;
  if (filled > w) filled = w;
  m3_rect(x, y, x + w, y + h, track);
  if (filled > 0) m3_rect(x, y, x + filled, y + h, fill);
  m3_frame(x, y, x + w - 1, y + h - 1, border);
}

/* Blit a w×h RGB15 sprite (0 = transparent, 0x8000|RGB15 = opaque). Same pixel
 * format as ui_icon16 and the mon_icons / mon_front generators. */
void ui_sprite(int x, int y, int w, int h, const u16* data) {
  if (!data) return;
  rumble_io_suspend();                       /* data may be in ROM: don't let the motor toggle mid-read */
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      u16 p = data[j * w + i];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
  }
  rumble_io_resume();
}

/* Blit a 32x32 RGB15 sprite shrunk to 16x16 (sample every other pixel) — for
 * compact list rows where the full 32x32 icon won't fit. */
/* Nearest-neighbour blit of a 32x32 (0x8000-keyed) icon at an arbitrary dst size
 * — for grids that want icons bigger than the 16x16 sub but not the full 32. */
void ui_icon_scaled(int x, int y, int dw, int dh, const u16* src32) {
  if (!src32) return;
  rumble_io_suspend();                       /* src32 may be a raw ROM icon pointer */
  for (int j = 0; j < dh; j++) {
    int sj = j * 32 / dh;
    for (int i = 0; i < dw; i++) {
      int si = i * 32 / dw;
      u16 p = src32[sj * 32 + si];
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
  rumble_io_suspend();                       /* src32 may be a raw ROM icon pointer */
  for (int j = 0; j < dh; j++) {
    int sj = j * 32 / dh;
    for (int i = 0; i < dw; i++) {
      int si = i * 32 / dw;
      u16 p = src32[sj * 32 + si];
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
void ui_bg_restore(const u16* bg, int x, int y, int w, int h) {
  if (!bg) return;
  bool dma = ((x & 1) == 0) && ((w & 1) == 0);
  rumble_io_suspend();                        /* bg lives in ROM: no motor toggle mid-read */
  for (int j = 0; j < h; j++) {
    const u16* src = bg + (uint32_t)(y + j) * 240 + x;
    u16* dst = &vid_mem[(y + j) * 240 + x];
    if (dma) dma3_cpy(dst, src, (u32)w * 2);
    else     for (int i = 0; i < w; i++) dst[i] = src[i];
  }
  rumble_io_resume();
}

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
