#include "pkview_legality.h"
#include "gen3_legality.h"

#include <tonc.h>
#include <stdio.h>
#include "ui.h"
#include "snd.h"

#define VIS 13

void pkview_legality_show(const PkMon* m) {
  PkLegality L = pk_check_legality(m);
  int illegal = 0;
  for (int i = 0; i < L.n; i++) if (L.issue[i].sev) illegal++;

  int top = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "LEGALITY  (structural)");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);

    if (L.n == 0) {
      ui_text(10, 32, UI_OK, "Legal");
      ui_text(10, 50, UI_DIM, "No impossible/tampered values.");
      ui_text(10, 62, UI_DIM, "Moves learnable. Encounter: later.");
    } else {
      char hdr[40];
      siprintf(hdr, "%d illegal, %d warning(s)", illegal, L.n - illegal);
      ui_text(8, 16, illegal ? UI_WARN : UI_DIRCLR, hdr);
      for (int i = 0; i < VIS && top + i < L.n; i++) {
        int y = 30 + i * 9;
        bool ill = L.issue[top + i].sev != 0;
        ui_text(6, y, ill ? UI_WARN : UI_DIM, ill ? "!" : "-");
        char rt[40]; ui_truncate(rt, L.issue[top + i].text, 27);
        ui_text(16, y, ill ? UI_TEXT : UI_DIM, rt);
      }
    }

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, L.n > VIS ? "U/D scroll  B back" : "B back");

    u16 k; do { VBlankIntrWait(); snd_vblank(); key_poll(); k = key_hit(KEY_B | KEY_UP | KEY_DOWN); } while (!k);
    if (k & KEY_B) { snd_back(); return; }
    else if ((k & KEY_UP) && top > 0) { snd_move(); top--; }
    else if ((k & KEY_DOWN) && top < L.n - VIS) { snd_move(); top++; }
  }
}
