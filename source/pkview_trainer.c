/*
 * Trainer card / stats screen for gba-pokeviewer. Shows the trainer identity,
 * money, play time, Pokédex counts, the designated Elite-Four / Hall-of-Fame
 * first-clear time, and the Game Records.
 */
#include <tonc.h>
#include <stdio.h>

#include "pkview_trainer.h"
#include "ui.h"
#include "snd.h"

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

void pkview_trainer(const uint8_t* sb1, const uint8_t* sb2, const Gen3SaveInfo* info, PkGame game) {
  ui_clear();
  ui_text(4, 2, UI_TITLE, "TRAINER CARD");
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);

  char line[64];
  int y = 16;
  siprintf(line, "NAME  %s  (%c)", info->trainer_name, info->gender ? 'F' : 'M');
  ui_text(6, y, UI_TEXT, line);
  y += 11;
  siprintf(line, "IDNo  %05u", (unsigned)info->tid_public);
  ui_text(6, y, UI_TEXT, line);
  siprintf(line, "SID %05u", (unsigned)info->tid_secret);
  ui_text(140, y, UI_DIM, line);
  y += 11;
  siprintf(line, "MONEY $%u", (unsigned)pk_money(sb1, sb2, game));
  ui_text(6, y, UI_OK, line);
  y += 11;
  siprintf(line, "TIME  %uh %02um", (unsigned)info->play_h, (unsigned)info->play_m);
  ui_text(6, y, UI_TEXT, line);
  y += 11;
  int seen, caught; bool nat;
  pk_pokedex(sb2, &seen, &caught, &nat);
  siprintf(line, "DEX   seen %d   caught %d%s", seen, caught, nat ? "  (Nat)" : "");
  ui_text(6, y, UI_TEXT, line);
  y += 15;

  /* designated Elite-Four / Hall-of-Fame section */
  ui_hline(4, y, 232, UI_BORDER);
  y += 4;
  ui_text(6, y, UI_TITLE, "ELITE FOUR / HALL OF FAME");
  y += 11;
  uint16_t h; uint8_t m, s;
  if (pk_hof_time(sb1, sb2, game, &h, &m, &s)) {
    siprintf(line, "First cleared at  %uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)s);
    ui_text(10, y, UI_OK, line);
  } else {
    ui_text(10, y, UI_DIM, "Not cleared yet");
  }
  y += 15;

  /* game records */
  ui_hline(4, y, 232, UI_BORDER);
  y += 4;
  ui_text(6, y, UI_TITLE, "GAME RECORDS");
  y += 11;
  siprintf(line, "Steps     %u", (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_STEPS));
  ui_text(10, y, UI_TEXT, line);
  y += 10;
  siprintf(line, "Battles   %u  (wild %u / trn %u)",
           (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TOTAL_BATTLES),
           (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_WILD_BATTLES),
           (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TRAINER_BATTLES));
  ui_text(10, y, UI_TEXT, line);
  y += 10;
  siprintf(line, "Captures  %u    Eggs hatched %u",
           (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_POKEMON_CAPTURES),
           (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_HATCHED_EGGS));
  ui_text(10, y, UI_TEXT, line);

  ui_hline(0, 151, UI_SCR_W, UI_BORDER);
  ui_text(4, 152, UI_DIM, "B back");

  u16 k;
  do { s_vsync(); k = key_hit(KEY_B); } while (!k);   /* B only — START stays the menu key */
  snd_back();
}
