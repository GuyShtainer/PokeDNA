/*
 * Trainer card / stats screen for gba-pokeviewer. Shows the trainer identity,
 * money, play time, Pokédex counts, the designated Elite-Four / Hall-of-Fame
 * first-clear time, and the Game Records.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pkview_trainer.h"
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "pkview_app.h"
#include "gen3_flags.h"   /* badge / frontier flag toggles */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* numeric entry via the on-screen keyboard; returns `cur` on cancel. */
static uint32_t num_entry(const char* prompt, uint32_t cur, uint32_t maxv) {
  char init[12], out[12];
  siprintf(init, "%lu", (unsigned long)cur);
  if (!osk_search(prompt, init, out, sizeof(out))) return cur;
  uint32_t v = 0;
  for (const char* p = out; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
  return v > maxv ? maxv : v;
}

/* badges (0..7) then the Emerald Battle-Frontier symbols (8..21). */
static const char* const BADGEFRONT_LBL[22] = {
  "Badge 1", "Badge 2", "Badge 3", "Badge 4", "Badge 5", "Badge 6", "Badge 7", "Badge 8",
  "Tower Silver", "Tower Gold", "Dome Silver", "Dome Gold", "Palace Silver", "Palace Gold",
  "Arena Silver", "Arena Gold", "Factory Silver", "Factory Gold", "Pike Silver", "Pike Gold",
  "Pyramid Silver", "Pyramid Gold",
};
static int badge_or_frontier_flag(PkGame g, int i) {
  return i < 8 ? pk_badge_flag(g, i) : pk_frontier_flag(g, i - 8);   /* -1 if absent */
}

/* On/off toggler for a set of flags (badges / frontier symbols). Returns true if
 * anything changed. Edits SaveBlock1 flags in place; the caller commits SB1. */
static bool flag_set_editor(uint8_t* sb1, PkGame game, const char* title,
                            int (*flagnum)(PkGame, int), const char* const* names, int count) {
  int sel = 0, top = 0; bool changed = false;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, title);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    const int vis = 15;
    if (sel < top) top = sel; if (sel >= top + vis) top = sel - vis + 1;
    for (int i = 0; i < vis && top + i < count; i++) {
      int idx = top + i, y = 16 + i * 9; bool s = (idx == sel);
      int fn = flagnum(game, idx);
      bool on = fn >= 0 && pk_flag_get(sb1, game, fn);
      char row[40]; siprintf(row, "%-15s %s", names[idx], on ? "ON" : "off");
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(8, y, s ? UI_SELTEXT : (on ? UI_OK : UI_DIM), row);
    }
    ui_text(4, 152, UI_DIM, "A toggle  U/D  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return changed;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : count - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % count;
    else if (k & KEY_A) {
      int fn = flagnum(game, sel);
      if (fn >= 0) { pk_flag_set(sb1, game, fn, !pk_flag_get(sb1, game, fn)); changed = true; }
    }
  }
}

enum { TF_NAME, TF_SEX, TF_TID, TF_SID, TF_MONEY, TF_TIME, TF_BADGES, TF_NUM };

void pkview_trainer(uint8_t* sb1, uint8_t* sb2, const Gen3SaveInfo* info, PkGame game) {
  const bool edit = app_can_edit();
  char     name[8];  strncpy(name, info->trainer_name, 7); name[7] = 0;
  uint8_t  gender = info->gender;
  uint16_t tid = info->tid_public, sid = info->tid_secret;
  uint32_t money = pk_money(sb1, sb2, game);
  uint16_t ph = info->play_h; uint8_t pm = info->play_m;
  int  sel = 0;
  bool d1 = false, d2 = false;       /* dirty: sb1 (money) / sb2 (identity) */

  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "TRAINER CARD");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);

    char line[64];
    const char* lbl[TF_NUM]; char val[TF_NUM][24];
    lbl[TF_NAME]  = "NAME";  siprintf(val[TF_NAME],  "%s", name);
    lbl[TF_SEX]   = "SEX";   siprintf(val[TF_SEX],   "%s", gender ? "Female" : "Male");
    lbl[TF_TID]   = "ID No"; siprintf(val[TF_TID],   "%05u", (unsigned)tid);
    lbl[TF_SID]   = "SID";   siprintf(val[TF_SID],   "%05u", (unsigned)sid);
    lbl[TF_MONEY] = "MONEY"; siprintf(val[TF_MONEY], "$%lu", (unsigned long)money);
    lbl[TF_TIME]  = "TIME";  siprintf(val[TF_TIME],  "%uh %02um", (unsigned)ph, (unsigned)pm);
    int nb = 0; for (int i = 0; i < 8; i++) if (pk_flag_get(sb1, game, pk_badge_flag(game, i))) nb++;
    lbl[TF_BADGES] = "BADGE"; siprintf(val[TF_BADGES], "%d/8%s  (A edit)", nb,
                                       game == PK_EMERALD ? " +frontier" : "");
    for (int i = 0; i < TF_NUM; i++) {
      int y = 14 + i * 9; bool s = edit && (i == sel);
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      siprintf(line, "%-6s %s", lbl[i], val[i]);
      ui_text(6, y, s ? UI_SELTEXT : (i == TF_MONEY ? UI_OK : UI_TEXT), line);
    }

    int y = 14 + TF_NUM * 9 + 3;
    int seen, caught; bool nat; pk_pokedex(sb2, &seen, &caught, &nat);
    siprintf(line, "DEX   seen %d  caught %d%s", seen, caught, nat ? " (Nat)" : "");
    ui_text(6, y, UI_TEXT, line); y += 12;

    ui_hline(4, y, 232, UI_BORDER); y += 3;
    ui_text(6, y, UI_TITLE, "ELITE FOUR / HALL OF FAME"); y += 10;
    uint16_t h; uint8_t m, s2;
    if (pk_hof_time(sb1, sb2, game, &h, &m, &s2)) {
      siprintf(line, "First cleared  %uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)s2);
      ui_text(10, y, UI_OK, line);
    } else ui_text(10, y, UI_DIM, "Not cleared yet");
    y += 12;

    ui_hline(4, y, 232, UI_BORDER); y += 3;
    ui_text(6, y, UI_TITLE, "GAME RECORDS"); y += 10;
    siprintf(line, "Steps %u", (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_STEPS));
    ui_text(10, y, UI_TEXT, line); y += 9;
    siprintf(line, "Battles %u (w%u/t%u)",
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TOTAL_BATTLES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_WILD_BATTLES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TRAINER_BATTLES));
    ui_text(10, y, UI_TEXT, line); y += 9;
    siprintf(line, "Captures %u  Eggs %u",
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_POKEMON_CAPTURES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_HATCHED_EGGS));
    ui_text(10, y, UI_TEXT, line);

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, edit ? "U/D field  A edit  B save+exit" : "B back");

    if (!edit) { do { s_vsync(); } while (!key_hit(KEY_B)); snd_back(); return; }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) {
      if ((d1 || d2) && app_confirm("Save trainer changes?", "Identity / money are written.")) {
        if (d2) app_commit_sb2();              /* trainer block (section 0)     */
        if (d1) app_commit_sb1();              /* money (sections 1..4)         */
      }
      return;
    } else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : TF_NUM - 1;
    else if (k & KEY_DOWN)   sel = (sel + 1) % TF_NUM;
    else if (k & KEY_A) {
      switch (sel) {
        case TF_NAME: { char b[8]; if (osk_input("TRAINER NAME", name, b, 8)) {
                          strcpy(name, b); pk_set_trainer_name(sb2, name); d2 = true; } } break;
        case TF_SEX:   gender ^= 1; pk_set_gender(sb2, gender); d2 = true; break;
        case TF_TID:   tid = (uint16_t)num_entry("ID No", tid, 65535);
                       pk_set_trainer_id(sb2, tid, sid); d2 = true; break;
        case TF_SID:   sid = (uint16_t)num_entry("SID", sid, 65535);
                       pk_set_trainer_id(sb2, tid, sid); d2 = true; break;
        case TF_MONEY: money = num_entry("MONEY", money, 999999);
                       pk_set_money(sb1, sb2, game, money); d1 = true; break;
        case TF_TIME:  ph = (uint16_t)num_entry("PLAY HOURS", ph, 999);
                       pm = (uint8_t)num_entry("PLAY MINUTES", pm, 59);
                       pk_set_playtime(sb2, ph, pm, 0); d2 = true; break;
        case TF_BADGES: if (flag_set_editor(sb1, game,
                              game == PK_EMERALD ? "BADGES + FRONTIER" : "BADGES",
                              badge_or_frontier_flag, BADGEFRONT_LBL,
                              game == PK_EMERALD ? 22 : 8)) d1 = true; break;   /* SB1 flags */
      }
    }
  }
}
