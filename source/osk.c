#include "osk.h"

#include <tonc.h>
#include <string.h>

#include "ui.h"
#include "snd.h"

#define OSK_ROWS   8
/* §15: sized for real FAT long names (the browser's NAME_MAX is 64), NOT the 8-16
 * chars of the Gen-3 fields — seeding a long file name must never truncate-on-open
 * (the field view scrolls; callers with short save-format fields still clamp via
 * their own `cap`). The edit buffer lives on the stack: 64 B is nothing. */
#define OSK_MAXLEN 63

/* QWERTY layout (same as the sd-browser keyboard); both cases shown so there is
 * no shift mode. Bottom row is the Gen-3-encodable punctuation. */
static const char* const KB[OSK_ROWS] = {
  "1234567890",
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm",
  "QWERTYUIOP",
  "ASDFGHJKL",
  "ZXCVBNM",
  " -.,'!?",
};

static int rowlen(int r) { return (int)strlen(KB[r]); }
static void osk_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

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

/* What is on screen. There is no shift/layout mode here -- both cases of every letter
 * are laid out at once (KB[] is a compile-time constant) -- so the prompt and the grid
 * of key cells never change shape for the life of one osk_core() call; only WHICH cell
 * is highlighted, the field's content/caret, and the footer line move from one keypress
 * to the next. Stack-local, not a static, so a fresh call always starts invalid (first
 * pass paints in full) -- same shape as pdna_edit.c's EditPaint / pdna_legality.c's
 * LegPaint, minus the record shadow neither of those two need here: cr/cc IS the whole
 * selection state, and the field/footer are cheap enough to just redraw every press
 * rather than diff. */
typedef struct { uint32_t gen; int cr, cc; bool valid; } OskPaint;

/* One keyboard cell. ui_text_sel() only fills its UI_SEL highlight rect on the SELECTED
 * path (ui.c) -- the unselected path draws the glyph straight over whatever is already
 * there -- so unhighlighting a cell here has to wipe that rect itself or the old
 * highlight fill would survive under the new plain-ink glyph. 13 px is the same width
 * ui_text_sel's own fill uses; the 4 px gap to the next column (17 px pitch) is never
 * touched by either cell, so a wipe here can't bleed into a neighbour. */
static void osk_key_paint(int r, int c, bool sel) {
  int x = 8 + c * 17, y = 44 + r * 13;
  char cell[2] = { KB[r][c], 0 };
  if (!sel) ui_fill_rect(x, y, 13, UI_ROW_H, UI_BG);
  ui_text_sel(x, y, 13, sel, UI_TEXT, cell);
}

/* Wipe-then-draw, same ghost-ink guard as every other row painter in this codebase: a
 * warning line shorter than the default hint (or vice versa) must not leave the old
 * text's tail on screen. */
static void osk_footer_paint(const char* warn) {
  ui_fill_rect(2, 150, 236, UI_ROW_H, UI_BG);
  char ftext[40];
  ui_truncate(ftext, warn ? warn : "A ins  B del  L/R caret  ST ok", 29);
  ui_text(2, 150, warn ? UI_WARN : UI_DIM, ftext);
}

static void osk_render(const char* prompt, const char* buf, int len, int cpos,
                       int cr, int cc, const char* warn, OskPaint* pv) {
  bool full = !pv->valid || pv->gen != ui_clear_gen();

  if (full) {
    ui_clear();
    char p[40];
    ui_truncate(p, prompt, 29);
    ui_text(2, 0, UI_TITLE, p);
    for (int r = 0; r < OSK_ROWS; r++) {
      int rl = rowlen(r);
      for (int c = 0; c < rl; c++) osk_key_paint(r, c, r == cr && c == cc);
    }
  } else if (cr != pv->cr || cc != pv->cc) {
    /* Cursor moved within the same grid: repaint just the two affected cells. A press
     * that moves neither (A/B/L/R/START/SELECT) falls through this too and touches no
     * cell at all. */
    osk_key_paint(pv->cr, pv->cc, false);
    osk_key_paint(cr, cc, true);
  }

  /* The field and footer are redrawn on every call regardless of `full`: both are
   * self-contained (osk_field repaints its own panel; osk_footer_paint wipes its own
   * strip first) and cheap, and almost every handled key changes one or the other
   * (a typed char, a caret move, a warning appearing/clearing) -- diffing buf/cpos/warn
   * to catch the rare true no-op (e.g. A on an already-full buffer) would cost more
   * code than the two rows it would occasionally save. */
  osk_field(buf, len, cpos);
  osk_footer_paint(warn);

  pv->cr    = cr;
  pv->cc    = cc;
  pv->gen   = ui_clear_gen();        /* read AFTER the ui_clear() above, not before */
  pv->valid = true;
}

static bool osk_core(const char* prompt, const char* initial, char* out, int cap, bool allow_empty) {
  char buf[OSK_MAXLEN + 1];
  int len = 0;
  buf[0] = 0;
  if (initial)
    for (; initial[len] && len < OSK_MAXLEN && len < cap - 1; len++) buf[len] = initial[len];
  buf[len] = 0;

  int cr = 0, cc = 0, cpos = len;
  bool dirty = true;
  const char* warn = NULL;
  OskPaint pv;
  memset(&pv, 0, sizeof pv);   /* .valid = false: the first osk_render() paints in full */

  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_B);  /* hold B to clear fast */
  key_repeat_limits(16, 3);

  for (;;) {
    if (cc >= rowlen(cr)) cc = rowlen(cr) - 1;
    if (dirty) { osk_render(prompt, buf, len, cpos, cr, cc, warn, &pv); dirty = false; }
    osk_vsync();

    u16 k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R |
                    KEY_A | KEY_B | KEY_START | KEY_SELECT);
    if (!k) continue;
    dirty = true;
    warn = NULL;
    if (k & (KEY_A | KEY_B | KEY_L | KEY_R | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))
      snd_move();                                /* light typing/caret tick */

    if (k & KEY_SELECT) { snd_back(); key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); return false; }
    else if (k & KEY_START) {
      if (len < 1 && !allow_empty) { snd_deny(); warn = "Name cannot be empty"; }
      else {
        snd_ok();
        int i = 0;
        for (; i < cap - 1 && buf[i]; i++) out[i] = buf[i];
        out[i] = 0;
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
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
      if (cpos > 0) { for (int j = cpos - 1; j < len; j++) buf[j] = buf[j + 1]; len--; cpos--; }
    }
    else if (k & KEY_L) { if (cpos > 0)   cpos--; }
    else if (k & KEY_R) { if (cpos < len) cpos++; }
    else if (k & KEY_UP)    { cr = (cr == 0) ? OSK_ROWS - 1 : cr - 1; }
    else if (k & KEY_DOWN)  { cr = (cr + 1) % OSK_ROWS; }
    else if (k & KEY_LEFT)  { int rl = rowlen(cr); cc = (cc == 0) ? rl - 1 : cc - 1; }
    else if (k & KEY_RIGHT) { int rl = rowlen(cr); cc = (cc + 1) % rl; }
  }
}

bool osk_input(const char* prompt, const char* initial, char* out, int cap) {
  return osk_core(prompt, initial, out, cap, false);
}
bool osk_search(const char* prompt, const char* initial, char* out, int cap) {
  return osk_core(prompt, initial, out, cap, true);
}
