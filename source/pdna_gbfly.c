/*
 * Gen-1/2 Fly destinations -- see pdna_gbfly.h for the full design note (BACKLOG
 * #90). Shape copied from source/pdna_fly.c (the Gen-3 screen this mirrors): a
 * scrolling list, A toggles, B asks to save.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbfly.h"
#include "gb_fly.h"
#include "pdna_gen12.h"    /* gb_persist                                            */
#include "ui.h"
#include "snd.h"
#include "rmbl.h"
#include "pdna_app.h"      /* app_can_edit / app_confirm                            */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (k & KEY_A)      snd_ok();
  else if (k & KEY_B)      snd_back();
  return k;
}

static void s_msg(const char* title, u16 ink, const char* l1, const char* l2) {
  ui_panel(12, 50, 216, l2 ? 60 : 50, UI_PANEL, UI_BORDER);
  ui_text(20, 58, ink, title);
  ui_hline(16, 70, 208, UI_BORDER);
  if (l1) ui_text(20, 76, UI_TEXT, l1);
  if (l2) ui_text(20, 86, UI_TEXT, l2);
  ui_text(20, l2 ? 96 : 86, UI_DIM, "A ok");
  s_wait(KEY_A | KEY_B);
}

/* ---- destination names (reference-cited; see pdna_gbfly.h's header note) ---- */

static const char* const kGen1Names[11] = {
  "Pallet Town", "Viridian City", "Pewter City", "Cerulean City", "Lavender Town",
  "Vermilion City", "Celadon City", "Fuchsia City", "Cinnabar Island",
  "Indigo Plateau", "Saffron City",
};

/* index order == SPAWN_* enum order (map_data_constants.asm); "spn" marks the four
 * spawn-only entries the real Fly menu (flypoints.asm) never offers as a destination. */
static const char* const kGen2Names[28] = {
  "Spawn: Home", "Spawn: Debug", "Pallet Town", "Viridian City", "Pewter City",
  "Cerulean City", "Rock Tunnel", "Vermilion City", "Lavender Town", "Saffron City",
  "Celadon City", "Fuchsia City", "Cinnabar Island", "Indigo Plateau",
  "New Bark Town", "Cherrygrove City", "Violet City", "Union Cave", "Azalea Town",
  "Cianwood City", "Goldenrod City", "Olivine City", "Ecruteak City", "Mahogany Town",
  "Lake of Rage", "Blackthorn City", "Silver Cave", "Fast Ship",
};

static bool gen2_is_flypoint(int idx) {
  return idx != 0 && idx != 1 && idx != 17 && idx != 27;   /* HOME/DEBUG/UNION_CAVE/FAST_SHIP */
}

static const char* fly_name(bool gen1, int idx) {
  if (gen1) return (idx >= 0 && idx < 11) ? kGen1Names[idx] : "?";
  return (idx >= 0 && idx < 28) ? kGen2Names[idx] : "?";
}

static const char* fly_tag(bool gen1, int idx) {
  if (gen1) return "";
  return gen2_is_flypoint(idx) ? "" : "spn";
}

/* Same paint-diff shape as pdna_fly.c's FlyPaint -- stack-local, not a static (a
 * fresh call always starts invalid). */
typedef struct { uint32_t gen; int top, sel; bool valid; } GbFlyPaint;

static void gbfly_row_paint(const GbSession* s, bool gen1, int idx, int y, bool sel) {
  bool set = gbfy_get(s, idx);
  char l[48];
  /* 17 + 4 + 3 = 24 columns at x=4 -> 192 px, inside the 240 px screen. The precision
   * TRUNCATES: Gen 2's longest name is "Cherrygrove City" (16), so .15 cut it to
   * "Cherrygrove Cit" (b90 review D2). pdna_fly.c's row carries the same widening. */
  siprintf(l, "%-18.17s%-4s%s", fly_name(gen1, idx), set ? "ON" : "off", fly_tag(gen1, idx));
  ui_fill_rect(4, y, 232, UI_ROW_H, UI_BG);
  ui_text_sel(4, y, 232, sel, set ? UI_OK : UI_DIM, l);
}

static void gbfly_header_paint(int on, int tot) {
  char l[16];
  siprintf(l, "%d/%d", on, tot);
  ui_text(200, 4, on == tot ? UI_OK : UI_DIM, l);
}

void pdna_gb_fly(GbSession* s, bool can_edit) {
  if (!s) return;
  GbGame g = gbfy_game(s);
  int n = gbfy_count(g);
  if (n <= 0) {
    s_msg("FLY", UI_DIM, "No fly destinations for", "this save.");
    return;
  }
  bool gen1 = (s->gen == GB_GEN1);
  rmbl_fire(RCUE_ROOM);

  int sel = 0, top = 0;
  bool dirty = false;
  bool toggled = false;
  const int vis = 12;
  GbFlyPaint pv;
  memset(&pv, 0, sizeof pv);

  for (;;) {
    if (sel < top) top = sel;
    else if (sel >= top + vis) top = sel - vis + 1;
    if (top > n - vis) top = n - vis;
    if (top < 0) top = 0;

    int on = 0;
    for (int i = 0; i < n; i++) if (gbfy_get(s, i)) on++;

    bool full = !pv.valid || pv.gen != ui_clear_gen() || top != pv.top;

    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "FLY DESTINATIONS");
      gbfly_header_paint(on, n);
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);

      for (int i = 0; i < vis && top + i < n; i++)
        gbfly_row_paint(s, gen1, top + i, 18 + i * 10, top + i == sel);

      ui_hline(0, 140, UI_SCR_W, UI_BORDER);
      if (!gen1)
        ui_text(4, 144, UI_DIM, "spn = no effect in game");
      ui_text(4, 152, UI_DIM, can_edit ? "A toggle  B save+back" : "read-only (Omega)  B back");
    } else {
      if (sel != pv.sel) {
        gbfly_row_paint(s, gen1, pv.sel, 18 + (pv.sel - top) * 10, false);
        gbfly_row_paint(s, gen1, sel,    18 + (sel    - top) * 10, true);
      } else if (toggled) {
        gbfly_row_paint(s, gen1, sel, 18 + (sel - top) * 10, true);
      }
      if (toggled) {
        ui_fill_rect(200, 4, UI_SCR_W - 200, UI_ROW_H, UI_BG);
        gbfly_header_paint(on, n);
      }
    }

    pv.top = top; pv.sel = sel; pv.gen = ui_clear_gen(); pv.valid = true;
    toggled = false;

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) break;
    if (k & KEY_UP)    sel = sel ? sel - 1 : n - 1;
    if (k & KEY_DOWN)  sel = (sel + 1) % n;
    if (k & KEY_LEFT)  { sel -= vis; if (sel < 0) sel = 0; }
    if (k & KEY_RIGHT) { sel += vis; if (sel >= n) sel = n - 1; }

    if (k & KEY_A) {
      if (!can_edit) {
        snd_deny();
        s_msg("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0);
        pv.valid = false;
        continue;
      }
      bool set = gbfy_get(s, sel);
      GbsStatus st = gbfy_set(s, sel, !set);
      if (st == GBS_OK) {
        dirty = true;
        toggled = true;
        rmbl_fire(RCUE_EDIT);
      } else {
        snd_error();
      }
    }
  }

  if (dirty && app_confirm("Save fly destinations?", "Writes the save file now."))
    gb_persist("fly");
}
