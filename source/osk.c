#include "osk.h"

#include <tonc.h>
#include <string.h>

#include "ui.h"

#define OSK_ROWS   8
#define OSK_MAXLEN 16

/* Gen-3-encodable layout (no shift mode; both cases shown). Every glyph here is
 * accepted by gen3_encode_char, so typed text round-trips on save. */
static const char* const KB[OSK_ROWS] = {
  "0123456789",
  "ABCDEFGHIJ",
  "KLMNOPQRST",
  "UVWXYZ",
  "abcdefghij",
  "klmnopqrst",
  "uvwxyz",
  " -.,'!?",
};

static int rowlen(int r) { return (int)strlen(KB[r]); }

static void osk_vsync(void) { VBlankIntrWait(); key_poll(); }

static void osk_field(const char* buf, int len, int cpos) {
  ui_panel(4, 16, 232, 14, UI_PANEL, UI_BORDER);
  const int x0 = 8, y = 19, cols = 28;
  int scroll = (cpos > cols - 1) ? cpos - (cols - 1) : 0;
  for (int i = 0; i < cols; i++) {
    int ci = scroll + i;
    if (ci > len) break;
    int x = x0 + i * 8;
    char ch[2];
    ch[0] = (ci < len) ? buf[ci] : ' ';
    ch[1] = 0;
    if (ci == cpos) {
      m3_rect(x, y, x + 8, y + UI_ROW_H, RGB15(31, 31, 31));
      ui_text(x, y, RGB15(0, 0, 0), ch);
    } else if (ci < len) {
      ui_text(x, y, UI_TEXT, ch);
    }
  }
}

static void osk_render(const char* prompt, const char* buf, int len, int cpos,
                       int cr, int cc, const char* warn) {
  ui_clear();
  char p[40];
  ui_truncate(p, prompt, 29);
  ui_text(2, 0, UI_TITLE, p);

  osk_field(buf, len, cpos);

  for (int r = 0; r < OSK_ROWS; r++) {
    int y = 44 + r * 13;
    int rl = rowlen(r);
    for (int c = 0; c < rl; c++) {
      int x = 8 + c * 17;
      char cell[2] = { KB[r][c], 0 };
      ui_text_sel(x, y, 13, (r == cr && c == cc), UI_TEXT, cell);
    }
  }

  char ftext[40];
  ui_truncate(ftext, warn ? warn : "A ins  B del  L/R move  ST ok", 29);
  ui_text(2, 150, warn ? UI_WARN : UI_DIM, ftext);
}

bool osk_input(const char* prompt, const char* initial, char* out, int cap) {
  char buf[OSK_MAXLEN + 1];
  int len = 0;
  buf[0] = 0;
  if (initial) {
    for (; initial[len] && len < OSK_MAXLEN && len < cap - 1; len++) buf[len] = initial[len];
    buf[len] = 0;
  }

  int cr = 0, cc = 0;
  int cpos = len;
  bool dirty = true;
  const char* warn = NULL;

  while (1) {
    if (cc >= rowlen(cr)) cc = rowlen(cr) - 1;
    if (dirty) { osk_render(prompt, buf, len, cpos, cr, cc, warn); dirty = false; }
    osk_vsync();

    u16 k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R |
                    KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (!k) continue;
    dirty = true;
    warn = NULL;

    if (k & KEY_SELECT) return false;
    else if (k & KEY_START) {
      if (len < 1) { warn = "Name cannot be empty"; }
      else {
        int i = 0;
        for (; i < cap - 1 && buf[i]; i++) out[i] = buf[i];
        out[i] = 0;
        return true;
      }
    }
    else if (k & KEY_A) {
      if (len < OSK_MAXLEN && len < cap - 1) {
        for (int j = len; j > cpos; j--) buf[j] = buf[j - 1];
        buf[cpos] = KB[cr][cc];
        len++; cpos++;
        buf[len] = 0;
      }
    }
    else if (k & KEY_B) {
      if (cpos > 0) {
        for (int j = cpos - 1; j < len; j++) buf[j] = buf[j + 1];
        len--; cpos--;
      }
    }
    else if (k & KEY_L) { if (cpos > 0)   cpos--; }
    else if (k & KEY_R) { if (cpos < len) cpos++; }
    else if (k & KEY_UP)    { cr = (cr == 0) ? OSK_ROWS - 1 : cr - 1; }
    else if (k & KEY_DOWN)  { cr = (cr + 1) % OSK_ROWS; }
    else if (k & KEY_LEFT)  { int rl = rowlen(cr); cc = (cc == 0) ? rl - 1 : cc - 1; }
    else if (k & KEY_RIGHT) { int rl = rowlen(cr); cc = (cc + 1) % rl; }
  }
}
