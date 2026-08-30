/*
 * Fly destinations — mark the towns you've been to so the region map will let you
 * Fly there.
 *
 * These bits are among the very safest in the save (see gen3_fly.h: one bit per
 * town, only two readers in the whole game, no checksum, no derived state), which
 * is exactly why this gets its own screen instead of living under the data editor's
 * FLAGS tab: that tab shows a blanket "toggling story flags can soft-lock the save"
 * caution which is actively misleading here. The rows are ALSO published to the
 * FLAGS tab via gen_data.py's "Fly destinations" category, for people who want them
 * next to everything else.
 *
 * What this screen must say out loud, because otherwise it looks broken: marking a
 * town visited does NOT give you Fly. You still need the badge (Feather in Hoenn,
 * Thunder in Kanto) and a party Pokemon that knows the move.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_fly.h"
#include "gen3_fly.h"
#include "ui.h"
#include "snd.h"
#include "rmbl.h"
#include "pdna_app.h"

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (k & KEY_A)      snd_ok();
  else if (k & KEY_B)      snd_back();
  return k;
}

/* Panel is 216 px with text at x=20 -> body lines must stay <= 25 columns. */
static void s_msg(const char* title, u16 ink, const char* l1, const char* l2) {
  ui_panel(12, 50, 216, l2 ? 60 : 50, UI_PANEL, UI_BORDER);
  ui_text(20, 58, ink, title);
  ui_hline(16, 70, 208, UI_BORDER);
  if (l1) ui_text(20, 76, UI_TEXT, l1);
  if (l2) ui_text(20, 86, UI_TEXT, l2);
  ui_text(20, l2 ? 96 : 86, UI_DIM, "A ok");
  s_wait(KEY_A | KEY_B);
}

/* Short tags — the row is "%-16.15s%-4s%s" at x=4, so the tag gets 3 columns.
 * The width is a TRUNCATING 15 plus a hard space: "%-15s" only PADS, so the 14-character
 * names ran straight into the state column and printed "Littleroot Townoff",
 * "Verdanturf TownON", "Cinnabar IslandON". */
static const char* kind_tag(int kind) {
  switch (kind) {
    case G3FLY_FACILITY: return "fac";
    case G3FLY_CURSOR:   return "cur";
    case G3FLY_PREREQ:   return "req";
    default:             return "";
  }
}

/* What is on screen. `t`/`n` (the destination list) are fixed for the whole screen
 * life -- g3fly_list() is called once, above -- so `top`/`sel` are the whole cursor
 * state. The header's ON-count is NOT shadowed here (no `on` field): it is re-derived
 * fresh every frame from g3fly_count_on(), and the repaint decision below is driven by
 * the explicit `toggled` flag, not a `on != pv.on` diff (see that flag's own comment --
 * a facility/cursor/prereq toggle flips a row without moving the town-only count at
 * all, so a stored `on` couldn't answer "did anything change" by itself anyway; an
 * earlier revision stored one, but nothing ever read it -- BACKLOG #36 item 8, golden
 * rule 6). Same shape as pdna_legality.c's SweepPaint, with the header counter
 * pdna_trainer.c's stars_editor also tracks. Stack-local, not a static: a fresh call
 * always starts invalid (first pass paints in full). */
typedef struct { uint32_t gen; int top, sel; bool valid; } FlyPaint;

/* One row. Self-contained: wipes its own UI_ROW_H-tall strip to UI_BG first (the
 * ghost-ink guard every other row painter in this codebase uses) -- ui_text_sel only
 * fills its own UI_SEL highlight rect on the SELECTED path (ui.c), so the unselected
 * draw needs this or a shorter/changed row could leave the old one's tail on screen.
 * No pairing trap: 8 px content on a 10 px pitch leaves a 2 px gap neither this nor
 * any neighbour ever touches. */
static void fly_row_paint(const G3FlyDest* t, uint8_t* sb1, PkGame game, int idx, int y, bool sel) {
  bool set = g3fly_get(sb1, game, idx);
  const G3FlyDest* d = &t[idx];
  char l[48];
  /* 15 + 4 + 3 = 22 columns at x=4 -> 180 px, inside the 240 px screen. */
  siprintf(l, "%-16.15s%-4s%s", d->name, set ? "ON" : "off", kind_tag(d->kind));
  ui_fill_rect(4, y, 232, UI_ROW_H, UI_BG);
  ui_text_sel(4, y, 232, sel, set ? UI_OK : UI_DIM, l);
}

static void fly_header_paint(int on, int tot) {
  char l[16];
  siprintf(l, "%d/%d", on, tot);
  ui_text(200, 4, on == tot ? UI_OK : UI_DIM, l);
}

void pdna_fly(uint8_t* sb1, PkGame game) {
  const G3FlyDest* t;
  int n = g3fly_list(game, &t);
  if (!sb1 || n <= 0) {
    s_msg("FLY", UI_DIM, "No fly destinations for", "this game.");
    return;
  }
  rmbl_fire(RCUE_ROOM);

  int sel = 0, top = 0;
  bool dirty = false;
  /* Set by the KEY_A handler below when g3fly_set() actually flipped the row at
   * `sel` this iteration, and consumed (cleared) by the very next iteration's
   * paint block. `on` (g3fly_count_on) only counts G3FLY_TOWN rows (gen3_fly.c)
   * -- a facility/cursor/prereq toggle leaves it unchanged, so it cannot be the
   * "did this row change" signal. `toggled` shadows the mutation itself instead. */
  bool toggled = false;
  const int vis = 12;
  FlyPaint pv;
  memset(&pv, 0, sizeof pv);           /* .valid = false: the first pass paints in full */

  for (;;) {
    if (sel < top) top = sel;
    else if (sel >= top + vis) top = sel - vis + 1;
    if (top > n - vis) top = n - vis;
    if (top < 0) top = 0;

    int tot = 0, on = g3fly_count_on(sb1, game, &tot);
    /* `top` unchanged also proves `sel` (old and new) is still inside the visible
     * window -- see pdna_legality.c's sweep_screen for why that makes the row-pair
     * repaint below safe without a bounds check. */
    bool full = !pv.valid || pv.gen != ui_clear_gen() || top != pv.top;

    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "FLY DESTINATIONS");
      fly_header_paint(on, tot);
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);

      for (int i = 0; i < vis && top + i < n; i++)
        fly_row_paint(t, sb1, game, top + i, 18 + i * 10, top + i == sel);

      ui_hline(0, 140, UI_SCR_W, UI_BORDER);
      /* The badge line is the difference between "this feature is broken" and "I
       * understand what this does". Both variants must stay <= 29 columns or they
       * wrap onto y=152 and collide with the key hints drawn there. */
      if (g3fly_badge_ok(sb1, game))
        ui_text(4, 144, UI_OK, "Badge OK - need a mon w/ Fly");
      else
        ui_text(4, 144, UI_WARN, "No Fly badge yet - can't fly");
      ui_text(4, 152, UI_DIM, app_can_edit() ? "A toggle  START all  B back"
                                             : "read-only (Omega)  B back");
    } else {
      if (sel != pv.sel) {
        fly_row_paint(t, sb1, game, pv.sel, 18 + (pv.sel - top) * 10, false);
        fly_row_paint(t, sb1, game, sel,    18 + (sel    - top) * 10, true);
      } else if (toggled) {
        /* toggled at the cursor, which did not move: the only single-flag mutator
         * below (KEY_A) only ever touches `sel`, so it is the row that flipped.
         * Gated on `toggled`, NOT `on != pv.on` -- a facility/cursor/prereq row
         * flips its ON/off text without moving the town-only `on` counter at all. */
        fly_row_paint(t, sb1, game, sel, 18 + (sel - top) * 10, true);
      }
      if (toggled) {
        /* Header repaint is cheap (one small rect) and this also covers the
         * count-changed case (a TOWN toggle does move `on`). */
        ui_fill_rect(200, 4, UI_SCR_W - 200, UI_ROW_H, UI_BG);
        fly_header_paint(on, tot);
      }
    }

    pv.top = top; pv.sel = sel; pv.gen = ui_clear_gen(); pv.valid = true;
    toggled = false;                     /* consumed for this pass either way */

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) break;
    if (k & KEY_UP)    sel = sel ? sel - 1 : n - 1;
    if (k & KEY_DOWN)  sel = (sel + 1) % n;
    if (k & KEY_LEFT)  { sel -= vis; if (sel < 0) sel = 0; }
    if (k & KEY_RIGHT) { sel += vis; if (sel >= n) sel = n - 1; }

    if (k & (KEY_A | KEY_START)) {
      if (!app_can_edit()) {
        snd_deny();
        s_msg("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0);
        pv.valid = false;             /* s_msg painted over us without ui_clear() */
        continue;
      }
    }

    if (k & KEY_START) {
      if (!app_confirm("Mark all towns visited?", "Skips story order.")) continue;
      int ch = g3fly_mark_all(sb1, game);       /* towns + FRLG prereqs; never the facility */
      if (ch) { dirty = true; rmbl_fire(RCUE_EDIT); }
      char l[48];
      siprintf(l, "%d newly marked.", ch);
      s_msg("MARKED", UI_OK, l, "Facility row untouched.");
      pv.valid = false;               /* s_msg painted over us without ui_clear() */
      continue;
    }

    if (k & KEY_A) {
      bool set = g3fly_get(sb1, game, sel);
      const G3FlyDest* d = &t[sel];
      /* The facility rows skip a lot more of the story than a town does, so they
       * ask before turning ON (turning OFF is always harmless). */
      if (!set && d->kind == G3FLY_FACILITY &&
          !app_confirm("Unlock this facility?", "Bigger skip than a town."))
        continue;
      g3fly_set(sb1, game, sel, !set);
      dirty = true;
      toggled = true;
      rmbl_fire(RCUE_EDIT);
      /* Mauville's flag doubles as the Cable Club Record Corner gate — benign, but
       * surprising if it just happens. */
      if (!set && g3fly_extra_effect(game, sel)) {
        s_msg("ALSO UNLOCKED", UI_TITLE, "Mauville also opens the", "Record Corner.");
        pv.valid = false;             /* s_msg painted over us without ui_clear() */
      }
    }
  }

  if (dirty && app_confirm("Save fly destinations?", "Writes SaveBlock1 now."))
    app_commit_sb1();
}
