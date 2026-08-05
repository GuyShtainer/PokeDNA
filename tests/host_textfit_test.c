/* Host test: every fixed on-screen string must FIT the 240 px screen.
 *
 * PokeDNA draws names and prose with its own proportional 5x7 face (ui_ptext*), and
 * fixed-width hints with tonc's sys8 (ui_text, 8 px/glyph). Both clip SILENTLY — a
 * string that is one pixel too wide simply loses its tail on hardware, which is how
 * "JIGGLYPU~" and "L/R box  A edit  DN" shipped. Emulator screenshots catch that only
 * if someone happens to open the screen; this catches it at build time.
 *
 * ui_ptext_w (source/ui.c) is exactly sum(ui_font_w[c - 32]) over the string, so the
 * measurement below is the real one, not an approximation. ui_font.c is pure data with
 * no GBA headers, so it dual-compiles on the host.
 *
 *   cc -std=c11 -I source tests/host_textfit_test.c source/ui_font.c -o /tmp/htf && /tmp/htf
 */
#include <stdio.h>
#include <string.h>

#include "ui_font.h"

#define SCR_W   240
#define SYS8_W    8     /* tonc sys8 advance: fixed 8 px per glyph */

static int fails = 0, checks = 0;

/* Mirrors ui_ptext_w()/pnext(): ASCII 32..127 map straight through; a UTF-8 two-byte
 * sequence for e-acute (C3 A9) maps to code 127; anything else becomes '?'. */
static int pwidth(const char* s) {
  int w = 0;
  const unsigned char* p = (const unsigned char*)s;
  while (*p) {
    unsigned c;
    if (*p == 0xC3u && p[1] == 0xA9u) { c = 127; p += 2; }
    else if (*p < 0x80u) { c = *p++; }
    else { p++; while ((*p & 0xC0u) == 0x80u) p++; c = '?'; }
    if (c < 32u || c > 127u) c = '?';
    w += ui_font_w[c - 32];
  }
  return w;
}

static void chk(const char* what, int x, int budget, int w, const char* s) {
  checks++;
  int end = x + w;
  int ok = (w <= budget);
  if (!ok) fails++;
  printf("  %-4s %-46.46s x=%-4d w=%-4d end=%-4d budget=%-4d %s\n",
         ok ? "ok" : "FAIL", s, x, w, end, budget, what);
}

/* proportional string drawn at x, must end before SCR_W */
static void P(const char* s, int x) { chk("ptext", x, SCR_W - x, pwidth(s), s); }
/* proportional string inside an explicit clamp (e.g. msg_wait's ui_ptext_fit) */
static void PF(const char* s, int x, int maxw) { chk("ptext_fit", x, maxw, pwidth(s), s); }
/* fixed-width sys8 string drawn at x */
static void T(const char* s, int x) { chk("text", x, SCR_W - x, (int)strlen(s) * SYS8_W, s); }

int main(void) {
  printf("== Battle Record screen (#20) ==\n");
  /* pdna_main.c — populated screen, the free band under the team columns */
  P("A exports a .rec + .txt to /PokeDNA/battles/.", 4);
  P("On a PC, rec2mp4 replays it into a video.", 4);
  /* pdna_main.c — empty screen */
  P("A .rec is a battle exported here or by a", 4);
  P("friend; rec2mp4 (PC) turns one into video.", 4);
  /* pdna_main.c — post-export toast: msg_wait draws l2 via ui_ptext_fit(28, 92, 184, ..) */
  PF("rec2mp4 (PC) renders it to MP4.", 28, 184);
  /* the existing footers on that screen must still fit */
  T("A export  SEL import  B back", 4);
  T("SEL import  B back", 4);

  printf("\n== PC box footers (#23/#27 neighbourhood) ==\n");
  /* pdna_box.c draw_footer(): drawn at WP_X + 2 = 80 */
  T("L/R tab  A pick  DN", 80);
  T("A drop  B cancel", 80);
  T("A give  B put back", 80);
  T("L/R box  A edit  DN", 80);
  T("MOVE A grab hold=set", 80);
  T("ITEM  A take  SEL B", 80);
  T("A menu  SEL  L/R  B", 80);
  T("A menu  UP  SEL  B", 80);

  printf("\n== summary screen (#25) ==\n");
  /* pdna_summary.c footers, drawn at x=4 */
  T("A list  <>edit  L/R  START", 4);
  T("A edit  L/R card  START keep", 4);
  T("A list  <>edit  U/D  L/R  B", 4);
  T("A edit  U/D mon  L/R  SEL  B", 4);
  T("U/D mon  L/R card  SEL  B", 4);
  /* confirm_q panel: ui_panel(16, 44, 208, 60) so the ink must stop by x=224, and the
   * lines are drawn at x=28. This is where "A = write  (backup made first)" ran 44 px
   * past the panel and 28 px off the screen before it was shortened. */
#define PANEL_R 224
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("Save changes?") * SYS8_W, "Save changes?");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("Keep this Pokemon?") * SYS8_W, "Keep this Pokemon?");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("A = write (backup first)") * SYS8_W, "A = write (backup first)");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("B = discard") * SYS8_W, "B = discard");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("B = discard it") * SYS8_W, "B = discard it");

  printf("\n== day-care yard (#21) ==\n");
  /* The three-row panel is ui_panel(2,124,236,28), text at x=6 -> 230 px of room.
   * Rows sit at y=125/134/143; ui_ptext inks 8 rows, so row 2 ends at y=150 with the
   * panel border at 151 — a descender is legal but there is no slack, so anything
   * added here has to fit the WIDTH exactly. */
#define DC_PANEL_W 230
  chk("daycare panel", 6, DC_PANEL_W, pwidth("They get along very well!"), "They get along very well!");
  chk("daycare panel", 6, DC_PANEL_W, pwidth("They'd rather be elsewhere."), "They'd rather be elsewhere.");
  chk("daycare panel", 6, DC_PANEL_W, pwidth("(no Egg: incompatible pair)"), "(no Egg: incompatible pair)");
  chk("daycare panel", 6, DC_PANEL_W, pwidth("An EGG is ready to collect!"), "An EGG is ready to collect!");
  chk("daycare panel", 6, DC_PANEL_W, pwidth("Egg check ~9999 steps (70%)"), "Egg check ~9999 steps (70%)");
  chk("daycare panel", 6, DC_PANEL_W, pwidth("One Pokemon is boarding."), "One Pokemon is boarding.");
  chk("daycare panel", 6, DC_PANEL_W, pwidth("No Pokemon are boarding."), "No Pokemon are boarding.");
  /* pk_daycare_yard_note's three returns — the lines that say what is actually yours */
  chk("yard note", 6, DC_PANEL_W, pwidth("Others are just visiting."), "Others are just visiting.");
  chk("yard note", 6, DC_PANEL_W, pwidth("Both Day-Care slots are full."), "Both Day-Care slots are full.");
  chk("yard note", 6, DC_PANEL_W, pwidth("The Day-Care holds 2 Pokemon."), "The Day-Care holds 2 Pokemon.");
  P("A menu  L/R your 2  B back", 4);
  P("A put in  B back", 4);
  P("Yard visitors are scenery, not your Pokemon.", 8);
  /* the title-bar slot counter is right-aligned to x=236, so it must not reach the
   * "DAY CARE" title, which ends at 4 + 8*8 = 68 px */
  chk("daycare title bar", 236 - pwidth("Boarding 2/2"), 236 - 68,
      pwidth("Boarding 2/2"), "Boarding 2/2");

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
