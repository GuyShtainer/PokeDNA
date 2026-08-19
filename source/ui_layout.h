#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

/* Screen geometry and popup layout arithmetic — the parts of ui.h that are pure
 * numbers rather than drawing.
 *
 * WHY A SEPARATE HEADER: ui.h includes <tonc.h>, so nothing that is not a GBA
 * translation unit can read it — including tests/host_textfit_test.c, which exists to
 * prove that popups and fixed strings stay inside the screen. A test that RE-TYPES
 * these numbers measures its own copy and stays green when the real one changes, which
 * is exactly the regression it was written to catch. So the numbers live here, ui.h
 * includes this file (every existing user of UI_SCR_W / UI_FOOTER_Y is unchanged), and
 * the host test includes it too. One definition, both readers.
 *
 * Pure C: no tonc, no libc, no types — safe on the host. */

#define UI_SCR_W   240
#define UI_SCR_H   160
#define UI_ROW_H     8          /* sys8 font line height (px)                   */
#define UI_SYS8_W    8          /* sys8 advance: a FIXED 8 px cell per glyph    */

/* Topmost pixel row the UNDERLYING screen's own footer occupies, AND the y the screens
 * actually pass to ui_text for that footer. A popup drawn over such a screen is a
 * separate panel carrying its OWN hint line; anything it paints at or below this row
 * lands on the screen's footer: two strings on one line. ui_panel covers it (the
 * footer's top rows get sheared off); ui_panel_alpha BLENDS, so the two strings show
 * through each other and read as garbage. Popups end above this row — see ui_popup_fit.
 *
 * IT IS THE DRAW COORDINATE, NOT JUST A LIMIT. The screens used to call
 * ui_text(2, 150, ...) with a hard 150 while the popup arithmetic used this constant.
 * That made every "does the popup clear the footer?" check self-referential — both sides
 * derived from the same symbol, so it could never fail — while the thing it was meant to
 * guard, the gap between the popup and the REAL footer, was free to drift the moment
 * anyone edited the literal. There is one number now: change it and the footers move
 * with the popups. Screens whose hint row sits lower carry their own constant
 * (PDNA_SET_FOOTER_Y, PDNA_DCY_FOOTER_Y, PDNA_FILT_FOOTER_Y), each asserted to be at or
 * below this row so a popup laid out against THIS row can never overlap THEIR footer. */
#define UI_FOOTER_Y  150
/* The hairline rule a full screen draws immediately above its footer. Derived, so it
 * travels with the footer rather than being re-typed as 147 at each call site. */
#define UI_FOOTER_RULE_Y (UI_FOOTER_Y - 3)

/* THE popup layout rule, as a pure function so both the shipped ui_popup_vfit() and the
 * host test run the SAME arithmetic. See ui.h for the caller-facing contract.
 *
 * The clamps are deliberate: a caller that asks for more rows than the screen can hold
 * gets a WINDOW, never a panel hanging off the bottom, and one that asks for an absurd
 * head/foot still gets a single usable row rather than a zero-height panel. */
/* Same arithmetic, but against a CALLER-SUPPLIED footer row rather than the global
 * UI_FOOTER_Y. app_mon_menu is drawn over more than one screen, and the party overlay's
 * own message box starts well above UI_FOOTER_Y (PDNA_PTY_FOOTER_Y, pdna_layout.h) — a
 * popup laid out against the wrong one lands on top of it. ui_popup_fit (below) is the
 * UI_FOOTER_Y-default wrapper every other existing caller keeps using unchanged. */
static inline int ui_popup_fit_at(int nrows, int row_h, int head, int foot, int footer_y,
                                  int* out_y, int* out_h) {
  int vis = (row_h > 0) ? (footer_y - head - foot) / row_h : 0;
  if (vis > nrows) vis = nrows;
  if (vis < 1)     vis = 1;
  int h = head + vis * row_h + foot;
  int y = (footer_y - h) / 2;
  if (y < 0) y = 0;
  if (out_y) *out_y = y;
  if (out_h) *out_h = h;
  return vis;
}

static inline int ui_popup_fit(int nrows, int row_h, int head, int foot,
                               int* out_y, int* out_h) {
  return ui_popup_fit_at(nrows, row_h, head, foot, UI_FOOTER_Y, out_y, out_h);
}

#endif /* UI_LAYOUT_H */
