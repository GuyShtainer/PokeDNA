#include "ui.h"
#include <string.h>

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
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      u16 p = data[j * w + i];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
  }
}

/* Blit a 32x32 RGB15 sprite shrunk to 16x16 (sample every other pixel) — for
 * compact list rows where the full 32x32 icon won't fit. */
void ui_icon_sub(int x, int y, const u16* src32) {
  if (!src32) return;
  for (int j = 0; j < 16; j++) {
    for (int i = 0; i < 16; i++) {
      u16 p = src32[(j * 2) * 32 + i * 2];
      if (p & 0x8000) m3_plot(x + i, y + j, (u16)(p & 0x7FFF));
    }
  }
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
