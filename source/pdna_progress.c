#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_progress.h"
#include "ui.h"
#include "mon_front.h"
#include "mon_icons.h"
#include "data_tables.h"
#include "rumble.h"        /* rumble_io_suspend/resume around the ROM sprite fetch */

/* Centre the 64x64 front sprite in the upper half; bar + text below. */
#define SPR_X ((UI_SCR_W - MON_FRONT_W) / 2)   /* 88 */
#define SPR_Y 26

void pdna_progress_frame(const char* title, const PkMon* m, int done, int total, const char* note) {
  if (total < 1) total = 1;
  if (done < 0) done = 0; else if (done > total) done = total;

  ui_clear();
  ui_text(8, 6, UI_TITLE, title);
  ui_hline(0, 16, UI_SCR_W, UI_BORDER);

  /* the mon's portrait (Egg / icon fallback). The fetch LZ77-decompresses from ROM. */
  const uint16_t* spr = 0;
  bool egg = m && m->isEgg && !m->isBadEgg;
  rumble_io_suspend();
  if (egg)               spr = mon_front_egg();
  else if (m && m->species) spr = mon_front_for_form(m->species, m->isShiny, m->form);
  rumble_io_resume();
  if (spr) ui_sprite(SPR_X, SPR_Y, MON_FRONT_W, MON_FRONT_H, spr);
  else if (m && m->species)
    ui_sprite(SPR_X + (MON_FRONT_W - MON_ICON_W) / 2, SPR_Y + (MON_FRONT_H - MON_ICON_H) / 2,
              MON_ICON_W, MON_ICON_H, egg ? mon_icon_egg() : mon_icon_for_form(m->species, m->form));

  /* name + shiny/egg tag, centred under the sprite */
  char nm[24];
  if (m && m->species) {
    ui_truncate(nm, m->nickname[0] ? m->nickname : pk_species_name(m->species), 14);
    if (m->isShiny) { int L = (int)strlen(nm); if (L < 20) { nm[L] = ' '; nm[L+1] = '*'; nm[L+2] = 0; } }
  } else { strcpy(nm, "-"); }
  int nw = (int)strlen(nm) * 8;
  ui_text((UI_SCR_W - nw) / 2, SPR_Y + MON_FRONT_H + 4, UI_TEXT, nm);

  /* count + bar */
  char cnt[24]; siprintf(cnt, "%d / %d", done, total);
  int cw = (int)strlen(cnt) * 8;
  ui_text((UI_SCR_W - cw) / 2, 118, UI_DIM, cnt);

  const int bx = 30, bw = UI_SCR_W - 60, bh = 10, by = 130;
  int filled = (int)(((long)bw * done) / total);
  ui_progress(bx, by, bw, bh, filled, UI_OK, UI_PANEL, UI_BORDER);

  if (note) {
    int nw2 = (int)strlen(note) * 8;
    ui_text((UI_SCR_W - nw2) / 2, 146, UI_DIM, note);
  }
}
