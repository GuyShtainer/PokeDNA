/* pdna_gbedit.c — the Game Boy mon editor SCREEN. See pdna_gbedit.h.
 *
 * Shape and paint discipline are pdna_edit.c's, on purpose: same title/rule/footer
 * geometry, same label/value columns (pdna_layout.h), same diff-render — a shadow of
 * the record as last drawn, rows repainted only where their text changed, a full
 * repaint only on entry or after an overlay bumped ui_clear_gen(). The rows themselves
 * come from gb_editor.c so this file never touches a record byte directly. */

#include <tonc.h>
#include <string.h>

#include "sys.h"
#include "pdna_gbedit.h"
#include "gb_editor.h"
#include "ui.h"
#include "osk.h"
#include "pdna_pick.h"
#include "pdna_layout.h"
#include "pdna_app.h"      /* msg_wait */
#include "snd.h"

#define VIS_ROWS 16
#define ROW_Y0   21
#define HDR_MAX  64

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* What is on screen right now (pdna_edit.c's EditPaint, for a GbEditMon). */
typedef struct {
  GbEditMon mon;
  uint32_t  gen;
  int       top, sel;
  bool      valid;
} Paint;

static void row_paint(const GbEditMon* e, int f, int i, bool sel, bool erase_top) {
  int y = ROW_Y0 + i * 8;
  int y0 = erase_top ? y - 1 : y;
  ui_fill_rect(2, y0, 236, y + UI_ROW_H - y0, UI_BG);
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  ui_text(PDNA_EDIT_LBL_X, y, sel ? UI_SELTEXT : UI_DIM, gbe_label(f));
  char val[GBE_VALUE_MAX], vt[PDNA_EDIT_VAL_COLS * 4 + 1];
  gbe_value(e, f, val, sizeof val);
  ui_truncate(vt, val, PDNA_EDIT_VAL_COLS);
  ui_text(PDNA_EDIT_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, vt);
}

static void render(const GbEditMon* e, const uint8_t* rows, int nrows, int sel, int top,
                   const char* note, Paint* pv) {
  bool full = !pv->valid || pv->gen != ui_clear_gen();
  char hdr[HDR_MAX], old[HDR_MAX];

  if (full) {
    ui_clear();
    ui_text(4, 0, UI_TITLE, "EDIT GB POKEMON");
    if (note) ui_ptext_right(UI_SCR_W - 4, 0, UI_WARN, note);
    ui_hline(0, 19, UI_SCR_W, UI_BORDER);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, PDNA_EDIT_FOOT);
  }

  gbe_header(e, hdr, sizeof hdr);
  gbe_header(&pv->mon, old, sizeof old);
  if (full || strcmp(hdr, old) != 0) {
    ui_fill_rect(0, 10, UI_SCR_W, UI_ROW_H, UI_BG);
    char lt[29 * 4 + 1];
    ui_truncate(lt, hdr, 29);
    ui_text(4, 10, UI_DIRCLR, lt);
  }

  uint32_t dirty = 0;
  for (int i = 0; i < VIS_ROWS && top + i < nrows; i++) {
    int  r = top + i, orow = pv->top + i;
    bool s = (r == sel), os = (orow == pv->sel);
    if (full || orow != r || s != os) { dirty |= 1u << i; continue; }
    char a[GBE_VALUE_MAX], b[GBE_VALUE_MAX];
    gbe_value(e, rows[r], a, sizeof a);
    gbe_value(&pv->mon, rows[r], b, sizeof b);
    if (strcmp(a, b) != 0) dirty |= 1u << i;
  }
  /* The 9 px selection panel reaches one scanline into the row above (pdna_edit.c
   * explains why both rows of each cursor position move as one unit). */
  for (int p = 0; p < 2; p++) {
    int r = p ? pv->sel - pv->top : sel - top;
    if (r <= 0 || r >= VIS_ROWS) continue;
    if (dirty & ((1u << r) | (1u << (r - 1)))) dirty |= (1u << r) | (1u << (r - 1));
  }
  for (int i = 0; i < VIS_ROWS && top + i < nrows; i++) {
    if (!(dirty & (1u << i))) continue;
    bool etop = (i == 0) || (dirty & (1u << (i - 1))) != 0;
    row_paint(e, rows[top + i], i, top + i == sel, etop);
  }

  pv->mon = *e;
  pv->top = top;
  pv->sel = sel;
  pv->gen = ui_clear_gen();
  pv->valid = true;
}

/* START: say what will happen, then ask. A hacked record the player already owns is
 * theirs to keep (gb_edit.h, gb_commit_checked), so a structural issue is shown and
 * may be overridden — but never silently. */
static bool confirm(const GbEditMon* e) {
  GbIssues iss;
  bool clean = gb_check(e, &iss);
  const char* stale = gbe_stale_note(e);
  const char* issue = clean ? 0 : gb_issue_text(&iss);
  /* stats_stale is one of gb_check's issues and has its own sentence above */
  if (!clean && iss.stats_stale) {
    GbIssues rest = iss; rest.stats_stale = false;
    issue = gb_issue_text(&rest);
  }

  ui_clear();
  ui_text(20, 40, UI_TITLE, "Write this Pokemon to the save?");
  int y = 56;
  if (issue) { ui_ptext_fit(20, y, 200, UI_WARN, issue); y += 10;
               ui_ptext_fit(20, y, 200, UI_WARN, "The game may not accept it. Write anyway?"); y += 12; }
  if (stale) { ui_ptext_fit(20, y, 200, UI_TEXT, stale); y += 12; }
  ui_text(20, y + 4,  UI_TEXT, "A = write (backs up first)");
  ui_text(20, y + 16, UI_WARN, "B = cancel");
  ui_text(20, 128, UI_DIM, "Original backed up to .bak,");
  ui_text(20, 138, UI_DIM, "new save verified on write.");
  u16 k = s_wait(KEY_A | KEY_B);
  return (k & KEY_A) != 0;
}

static void press(GbEditMon* e, int f) {
  int kind = gbe_kind(f);
  if (kind == GBE_K_TEXT) {
    char cur[GB_TEXT_MAX], out[GB_TEXT_MAX], bad[GB_GLYPH_MAX];
    gbe_value(e, f, cur, sizeof cur);
    if (!osk_input(gbe_label(f), cur, out, sizeof out)) return;
    if (gbe_set_text(e, f, out, bad)) { snd_edit(); return; }
    snd_deny();
    if (bad[0]) msg_wait("CAN'T STORE THAT", UI_WARN, "Not in this game's charset:", bad);
    else        msg_wait("CAN'T STORE THAT", UI_WARN, "The name was refused.", 0);
    return;
  }
  if (kind == GBE_K_MOVE) {
    uint16_t id = pick_move(gb_get_move(e, f - GBE_MV0));
    if (id == 0xFFFF) return;
    if (gbe_set_move(e, f, id)) { snd_edit(); return; }
    snd_deny();
    if (id > gb_max_move(e->gen)) msg_wait("NOT IN THIS GAME", UI_WARN, "That move is from a later", "generation.");
    else                          msg_wait("ALREADY KNOWN", UI_WARN, "This Pokemon has that move", "in another slot.");
    return;
  }
  if (kind == GBE_K_NUM) { if (gbe_press(e, f)) snd_edit(); else snd_deny(); return; }
  snd_deny();
}

bool pdna_gbedit(GbEditMon* e, const char* note) {
  if (!e) return false;
  uint8_t rows[GBE_NUM];
  int nrows = gbe_fields(e, rows);
  if (nrows <= 0) return false;

  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  int sel = 0, top = 0;
  bool committed = false;
  Paint pv;
  memset(&pv, 0, sizeof pv);
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;
    render(e, rows, nrows, sel, top, note, &pv);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) break;
    else if (k & KEY_START) {
      if (confirm(e)) { gbe_settle_stats(e); committed = true; break; }
    }
    else if (k & KEY_UP)    sel = (sel == 0) ? nrows - 1 : sel - 1;
    else if (k & KEY_DOWN)  sel = (sel + 1) % nrows;
    else if (k & KEY_A)     press(e, rows[sel]);
    else if (k & KEY_LEFT)  gbe_adjust(e, rows[sel], -1, false);
    else if (k & KEY_RIGHT) gbe_adjust(e, rows[sel], +1, false);
    else if (k & KEY_L)     gbe_adjust(e, rows[sel], -1, true);
    else if (k & KEY_R)     gbe_adjust(e, rows[sel], +1, true);
  }
  key_repeat_mask(KEY_UP | KEY_DOWN);
  return committed;
}
