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
  const int vis = 12;

  for (;;) {
    if (sel < top) top = sel;
    else if (sel >= top + vis) top = sel - vis + 1;
    if (top > n - vis) top = n - vis;
    if (top < 0) top = 0;

    ui_clear();
    ui_text(4, 4, UI_TITLE, "FLY DESTINATIONS");
    int tot = 0, on = g3fly_count_on(sb1, game, &tot);
    char l[48];
    siprintf(l, "%d/%d", on, tot);
    ui_text(200, 4, on == tot ? UI_OK : UI_DIM, l);
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);

    for (int i = 0; i < vis && top + i < n; i++) {
      const G3FlyDest* d = &t[top + i];
      bool set = g3fly_get(sb1, game, top + i);
      int y = 18 + i * 10; bool s = (top + i == sel);
      const char* tag = kind_tag(d->kind);
      /* 15 + 4 + 3 = 22 columns at x=4 -> 180 px, inside the 240 px screen. */
      siprintf(l, "%-16.15s%-4s%s", d->name, set ? "ON" : "off", tag);
      ui_text_sel(4, y, 232, s, set ? UI_OK : UI_DIM, l);
    }

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

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) break;
    if (k & KEY_UP)    sel = sel ? sel - 1 : n - 1;
    if (k & KEY_DOWN)  sel = (sel + 1) % n;
    if (k & KEY_LEFT)  { sel -= vis; if (sel < 0) sel = 0; }
    if (k & KEY_RIGHT) { sel += vis; if (sel >= n) sel = n - 1; }

    if (k & (KEY_A | KEY_START)) {
      if (!app_can_edit()) { snd_deny(); s_msg("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
    }

    if (k & KEY_START) {
      if (!app_confirm("Mark all towns visited?", "Skips story order.")) continue;
      int ch = g3fly_mark_all(sb1, game);       /* towns + FRLG prereqs; never the facility */
      if (ch) { dirty = true; rmbl_fire(RCUE_EDIT); }
      siprintf(l, "%d newly marked.", ch);
      s_msg("MARKED", UI_OK, l, "Facility row untouched.");
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
      rmbl_fire(RCUE_EDIT);
      /* Mauville's flag doubles as the Cable Club Record Corner gate — benign, but
       * surprising if it just happens. */
      if (!set && g3fly_extra_effect(game, sel))
        s_msg("ALSO UNLOCKED", UI_TITLE, "Mauville also opens the", "Record Corner.");
    }
  }

  if (dirty && app_confirm("Save fly destinations?", "Writes SaveBlock1 now."))
    app_commit_sb1();
}
