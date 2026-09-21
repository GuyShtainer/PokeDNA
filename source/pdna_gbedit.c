/* pdna_gbedit.c — the Game Boy mon editor SCREEN. See pdna_gbedit.h.
 *
 * BACKLOG #41: reached only via SELECT from source/pdna_gbsummary.c now (the box
 * menu's own EDIT/VIEW rows open that screen instead) — the reviewed fallback until
 * pdna_gbsummary.c gets its own hardware-testing-protocol pass. See pdna_gbedit.h.
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
#include "gb_session.h"    /* gbs_is_mail_item (BACKLOG #95 review C4) */
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
  ui_text(PDNA_EDIT_LBL_X, y, sel ? UI_SELTEXT : UI_DIM, gbe_label_of(e, f));
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
    ui_text(4, 0, UI_TITLE, PDNA_GBEDIT_TITLE);
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

/* Shared body of gbedit_confirm()/gbedit_confirm_keep() (G1 review LOW-6,
 * 2026-09-08): say what will happen, then ask. A hacked record the player
 * already owns is theirs to keep (gb_edit.h, gb_commit_checked), so a
 * structural issue is shown and may be overridden — but never silently.
 * `title`/`a_verb`/`b_verb` are the only difference between the two public
 * names: gbedit_confirm_keep() reads Gen 3's own create-flow wording
 * (pdna_summary.c's confirm_keep -- "Keep this Pokemon?"/"A = write (backup
 * first)"/"B = discard it") with gb_check's legality lines still shown
 * underneath, exactly like every other write through this screen; the
 * ORIGINAL gbedit_confirm() keeps its own "Write to the save?" wording
 * unchanged for every existing caller (a plain edit is not a create). */
static bool gbedit_confirm_ex(const GbEditMon* e, const char* title,
                              const char* a_verb, const char* b_verb) {
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
  /* Which mon this arms a write for -- the screen used to ask "Write to the save?"
   * without ever naming it. Same header render() draws at y=10 (species/level/DVs,
   * gbe_header), truncated the same way; built from `e` so it is dynamic text and
   * cannot be a pdna_layout.h macro. Drawn at y=26, ABOVE the title (y=40): there is
   * nothing else in that band and it reads name-then-question rather than squeezing
   * a line into the y=40..56 gap, which is too tight for one. */
  char hdr[HDR_MAX], lt[29 * 4 + 1];
  gbe_header(e, hdr, sizeof hdr);
  ui_truncate(lt, hdr, 29);
  ui_text(4, 26, UI_DIRCLR, lt);          /* x=4 like render(): 29 cols end at 236, not 252 */
  ui_text(20, 40, UI_TITLE, title);
  int y = PDNA_GBEDIT_CONFIRM_Y0;
  if (issue) {
    y += ui_ptext_wrap(20, y, PDNA_GBEDIT_CONFIRM_W, PDNA_GBEDIT_CONFIRM_LINE_H,
                        PDNA_GBEDIT_CONFIRM_MAXLN, UI_WARN, issue)
         * PDNA_GBEDIT_CONFIRM_LINE_H;
    ui_ptext_fit(20, y, PDNA_GBEDIT_CONFIRM_W, UI_WARN, PDNA_GBEDIT_WRITE_ANYWAY);
    y += PDNA_GBEDIT_CONFIRM_GAP;
  }
  if (stale) {
    y += ui_ptext_wrap(20, y, PDNA_GBEDIT_CONFIRM_W, PDNA_GBEDIT_CONFIRM_LINE_H,
                        PDNA_GBEDIT_CONFIRM_MAXLN, UI_TEXT, stale)
         * PDNA_GBEDIT_CONFIRM_LINE_H;
  }
  /* #62 review D5: under PDNA_DELTA, gb_persist() always REFUSES the card write (no
   * SD in the emulator build) and keeps the edit in-session instead -- `a_verb` and
   * the BAK_L1/L2 pair below both promise a backup + card write that will not happen,
   * so this build says the true thing instead of the shared SD-half wording. */
#ifdef PDNA_DELTA
  (void)a_verb;
  ui_text(20, y + PDNA_GBEDIT_AB_DY1, UI_TEXT, "A = write (session only)");
#else
  ui_text(20, y + PDNA_GBEDIT_AB_DY1, UI_TEXT, a_verb);
#endif
  ui_text(20, y + PDNA_GBEDIT_AB_DY2, UI_WARN, b_verb);
#ifndef PDNA_DELTA
  ui_text(20, PDNA_GBEDIT_BAK_Y1, UI_DIM, PDNA_GBEDIT_BAK_L1);
  ui_text(20, PDNA_GBEDIT_BAK_Y2, UI_DIM, PDNA_GBEDIT_BAK_L2);
#else
  ui_text(20, PDNA_GBEDIT_BAK_Y1, UI_DIM, "Kept for this session only,");
  ui_text(20, PDNA_GBEDIT_BAK_Y2, UI_DIM, "not written to a card.");
#endif
  u16 k = s_wait(KEY_A | KEY_B);
  return (k & KEY_A) != 0;
}

bool gbedit_confirm(const GbEditMon* e) {
  return gbedit_confirm_ex(e, PDNA_GBEDIT_CONFIRM_TITLE, PDNA_GBEDIT_A_WRITE, PDNA_GBEDIT_B_CANCEL);
}

bool gbedit_confirm_keep(const GbEditMon* e) {
  return gbedit_confirm_ex(e, PDNA_GBEDIT_KEEP_TITLE, PDNA_GBEDIT_KEEP_A, PDNA_GBEDIT_KEEP_B);
}

/* S5-B Part E: dv4 is part of the sidecar's own fingerprint (gb_sidecar.h gbsc_key),
 * so editing any of the four stored DVs here moves `e` to a key gbsc_find() will never
 * associate with its current sidecar entry again -- an edit that silently orphans it.
 * GBE_DVH (the derived HP DV) is READ-ONLY (gb_editor.h: "shown, not editable") and is
 * deliberately excluded -- it cannot itself be the edit that orphans anything.
 *
 * GBE_GENDER IS included: flipping it moves the Attack DV exactly like editing GBE_DVA
 * directly would (gb_editor.c's gbe_flip_gender calls the same gb_set_dv), so it is the
 * same key-orphaning edit wearing a friendlier control and must warn identically. */
bool gbedit_is_dv_field(int f) {
  return f == GBE_DVA || f == GBE_DVD || f == GBE_DVS || f == GBE_DVC || f == GBE_GENDER;
}

/* Shown once per editor visit (has_sidecar's own `*warned` latch), on the FIRST
 * adjust/press of a DV row, then never again in this visit -- the sidecar is already
 * orphaned after that first edit, so repeating the warning would say nothing new. */
void gbedit_dv_orphan_warn(bool has_sidecar, bool* warned) {
  if (!has_sidecar || *warned) return;
  *warned = true;
  msg_wait(PDNA_SIDECAR_DV_TITLE, UI_WARN, PDNA_SIDECAR_DV_L1, PDNA_SIDECAR_DV_L2);
}

/* BACKLOG #95 review C2: shown the moment gbe_flip_shiny() had to move gender to turn
 * shininess ON -- see pdna_layout.h's PDNA_GBEDIT_SHINY_FORCED_* comment for why this
 * is an immediate popup rather than a line in the write confirm screen (no room left
 * there). Checked unconditionally after every adjust/press below: cheap (one int
 * read), and gbe_shiny_forced_gender() returns >= 0 only right after a GBE_SHINY (the
 * dispatchers clear the flag on entry -- gbmon re-verify C9), i.e. right after a GBE_SHINY
 * toggle actually forced one, never for any other row. */
static void gbedit_shiny_forced_note(const GbEditMon* e) {
  int g = gbe_shiny_forced_gender(e);
  if (g < 0) return;
  /* gbmon C12: g == 1 (forced to female) does not fire against this tree's own
   * gender-ratio data (pdna_layout.h's PDNA_GBEDIT_SHINY_FORCED_FEMALE_* comment has
   * the exhaustive check across all five ratio bytes) -- kept defensively rather than
   * dropped, same reasoning as keeping both string pairs. */
  if (g == 1) msg_wait(PDNA_GBEDIT_SHINY_FORCED_TITLE, UI_WARN,
                        PDNA_GBEDIT_SHINY_FORCED_FEMALE_L1, PDNA_GBEDIT_SHINY_FORCED_FEMALE_L2);
  else        msg_wait(PDNA_GBEDIT_SHINY_FORCED_TITLE, UI_WARN,
                        PDNA_GBEDIT_SHINY_FORCED_MALE_L1, PDNA_GBEDIT_SHINY_FORCED_MALE_L2);
}

/* d-pad/L/R adjust, DV-warning-checked -- the shared tail of pdna_gbedit()'s four
 * KEY_LEFT/RIGHT/L/R branches, none of which differ except direction and step size. */
bool gbedit_adjust_checked(GbEditMon* e, int f, int dir, bool big,
                           bool has_sidecar, bool* dv_warned) {
  if (gbedit_is_dv_field(f)) gbedit_dv_orphan_warn(has_sidecar, dv_warned);
  bool changed = gbe_adjust(e, f, dir, big);
  if (changed) gbedit_shiny_forced_note(e);
  return changed;
}

/* G1 review LOW-1: see pdna_gbedit.h's own comment. */
void gbedit_adjust_refused(int f) {
  snd_deny();
  if (f == GBE_GENDER)
    msg_wait(PDNA_GBEDIT_GENDER_LOCKED_TITLE, UI_WARN, PDNA_GBEDIT_GENDER_LOCKED_L1, 0);
  /* BACKLOG #95 review C5: gb_set_egg(e, true) now refuses while the record holds a
   * non-zero item (gb_edit.c) -- an Egg cannot hold one. Both GBE_K_NUM paths land
   * here on a false return: gbe_press (A) and gbe_adjust (LEFT/RIGHT/L/R, all of
   * which just flip EGG the same dir-independent way GENDER/SHINY do).
   *
   * NOT extended to GBE_ITEM's own LEFT/RIGHT numeric step: gbe_adjust(GBE_ITEM)
   * also returns false on the ORDINARY "already at 0/255" clamp no-op (clampi),
   * which this function cannot tell apart from an Egg refusal without `e` (its
   * signature is `(int f)` only, shared with pdna_gbsummary.c's own two call sites
   * this pass does not touch) -- a message here would misfire on every plain clamp.
   * The A-press item PICKER already gets the real message, with `e` in scope, at
   * the two sites that made this call (gb_item_hook, this file's GBE_K_ITEM branch
   * above); this is the same "silent deny beep, no prose" shape every other numeric
   * clamp in this row already has. */
  else if (f == GBE_EGG)
    msg_wait(PDNA_GBEDIT_EGG_ITEM_TITLE, UI_WARN, PDNA_GBEDIT_EGG_ITEM_L1, 0);
}

void gbedit_press(GbEditMon* e, int f, bool has_sidecar, bool* dv_warned) {
  if (gbedit_is_dv_field(f)) gbedit_dv_orphan_warn(has_sidecar, dv_warned);
  int kind = gbe_kind(f);
  if (kind == GBE_K_TEXT) {
    char cur[GB_TEXT_MAX], out[GB_TEXT_MAX], bad[GB_GLYPH_MAX];
    gbe_value(e, f, cur, sizeof cur);
    if (!osk_input(gbe_label_of(e, f), cur, out, sizeof out)) return;
    if (gbe_set_text(e, f, out, bad)) { snd_edit(); return; }
    snd_deny();
    if (bad[0]) msg_wait(PDNA_GBEDIT_BADCHARSET_TITLE, UI_WARN, PDNA_GBEDIT_BADCHARSET_L1, bad);
    else        msg_wait(PDNA_GBEDIT_BADCHARSET_TITLE, UI_WARN, PDNA_GBEDIT_BADNAME_L1, 0);
    return;
  }
  if (kind == GBE_K_MOVE) {
    /* BACKLOG #189: the picker itself only offers moves this mon's own generation
     * can learn (gb_max_move(e->gen): 165 Gen 1, 251 Gen 2) -- the LATE refusal
     * below stays as a defence (gbe_set_move's own over-range check, pinned by
     * tests/host_gbeditor_test.c) but is unreachable from this picker now. */
    pick_move_set_gen_max(gb_max_move(e->gen));
    uint16_t id = pick_move(gb_get_move(e, f - GBE_MV0));
    pick_move_set_gen_max(0);
    if (id == 0xFFFF) return;
    if (gbe_set_move(e, f, id)) { snd_edit(); return; }
    snd_deny();
    if (id > gb_max_move(e->gen)) msg_wait(PDNA_GBEDIT_MOVE_LATE_TITLE, UI_WARN, PDNA_GBEDIT_MOVE_LATE_L1, PDNA_GBEDIT_MOVE_LATE_L2);
    else                          msg_wait(PDNA_GBEDIT_MOVE_DUP_TITLE, UI_WARN, PDNA_GBEDIT_MOVE_DUP_L1, PDNA_GBEDIT_MOVE_DUP_L2);
    return;
  }
  /* UX-parity audit (Guy 2026-09-07): the held-item field used to LEFT/RIGHT-
   * step a raw byte with no picker at all -- A now opens the SAME pick_item()
   * screen the Gen-3 flow uses (app_quick_item, pdna_main.c), restricted to
   * ids 0..255 shown as "#n" (pick_item_set_gen1_2_max()'s own header comment
   * has the full "why not real names yet" reasoning; #0 is NO_ITEM and removes
   * the item, same as every other slot's item picker). gb_get_held_item/
   * gb_set_held_item are gb_edit.h calls, reachable here because gb_editor.h
   * includes that header itself -- no new wrapper needed, unlike gbe_set_move
   * (which validates against gb_max_move/move_taken; held_item needs neither).
   * BACKLOG #95 review C4/C5: two refusals ADDED here since that comment was
   * written -- an Egg cannot hold an item at all (pack.asm
   * AnEggCantHoldAnItemText; gb_set_held_item's own refusal would silently
   * no-op without a message, so this checks first to say why), and Mail needs
   * an explicit confirm (this tree tracks no mailbox, so gbs_delete/gbs_move
   * would refuse the whole party the moment this mon carries one, gb_session.h
   * gbs_is_mail_item). */
  if (kind == GBE_K_ITEM) {
    pick_item_set_gen1_2_max(255);
    uint16_t id = pick_item(gb_get_held_item(e));
    pick_item_set_gen1_2_max(0);
    if (id == 0xFFFF) return;                              /* cancel */
    if (id != 0 && gb_is_egg(e)) {
      snd_deny();
      msg_wait(PDNA_GBEDIT_EGG_ITEM_TITLE, UI_WARN, PDNA_GBEDIT_EGG_ITEM_L1, 0);
      return;
    }
    if (id != 0 && gbs_is_mail_item((uint8_t)id)
        && !app_confirm(PDNA_GBEDIT_MAIL_TITLE, PDNA_GBEDIT_MAIL_L1))
      return;                                                /* declined */
    if (gb_set_held_item(e, (uint8_t)id)) snd_edit(); else snd_deny();
    return;
  }
  if (kind == GBE_K_NUM) {
    if (gbe_press(e, f)) { snd_edit(); gbedit_shiny_forced_note(e); }
    else gbedit_adjust_refused(f);   /* the same prose LEFT/RIGHT already gets (gbmon re-verify C10) */
    return;
  }
  snd_deny();
}

bool pdna_gbedit(GbEditMon* e, const char* note, bool has_sidecar) {
  if (!e) return false;
  uint8_t rows[GBE_NUM];
  int nrows = gbe_fields(e, rows);
  if (nrows <= 0) return false;

  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  int sel = 0, top = 0;
  bool committed = false;
  bool dv_warned = false;
  Paint pv;
  memset(&pv, 0, sizeof pv);
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;
    render(e, rows, nrows, sel, top, note, &pv);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) break;
    else if (k & KEY_START) {
      if (gbedit_confirm(e)) { gbe_settle_stats(e); committed = true; break; }
    }
    else if (k & KEY_UP)    sel = (sel == 0) ? nrows - 1 : sel - 1;
    else if (k & KEY_DOWN)  sel = (sel + 1) % nrows;
    else if (k & KEY_A)     gbedit_press(e, rows[sel], has_sidecar, &dv_warned);
    else if (k & KEY_LEFT)  { if (!gbedit_adjust_checked(e, rows[sel], -1, false, has_sidecar, &dv_warned)) gbedit_adjust_refused(rows[sel]); }
    else if (k & KEY_RIGHT) { if (!gbedit_adjust_checked(e, rows[sel], +1, false, has_sidecar, &dv_warned)) gbedit_adjust_refused(rows[sel]); }
    else if (k & KEY_L)     { if (!gbedit_adjust_checked(e, rows[sel], -1, true,  has_sidecar, &dv_warned)) gbedit_adjust_refused(rows[sel]); }
    else if (k & KEY_R)     { if (!gbedit_adjust_checked(e, rows[sel], +1, true,  has_sidecar, &dv_warned)) gbedit_adjust_refused(rows[sel]); }
  }
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);   /* restore the global repeat set */
  return committed;
}
