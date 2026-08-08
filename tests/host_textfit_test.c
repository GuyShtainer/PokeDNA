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

/* ---- ADDED for the legality report screen (source/pdna_legality.c) --------
 * Its help pages are the first fixed text in PokeDNA that is WRAPPED rather than
 * clamped: ui_ptext_wrap guarantees the width but nothing guarantees the page still
 * ends above the footer rule. So mirror ui_ptext_break (source/ui.c:324) to count
 * lines, and check the resulting page HEIGHT. */
static int wrap_lines(const char* s, int maxw) {
  int n = 0;
  while (*s) {
    const char* p = s;
    int w = 0, last_space = -1;
    while (*p) {
      if (*p == ' ') last_space = (int)(p - s);
      unsigned c = (unsigned char)*p;
      if (c < 32u || c > 127u) c = '?';
      int a = ui_font_w[c - 32];
      if (w + a > maxw) break;
      w += a; p++;
    }
    int i = (int)(p - s), skip;
    if (!s[i]) skip = i;                       /* the rest fits */
    else if (last_space > 0) skip = last_space + 1;
    else skip = i ? i : 1;
    s += skip; n++;
  }
  return n;
}
/* Pixels one help_para() consumes: wrapped lines at 9 px pitch, then 5 px of gap. */
static int WRAPH(const char* s) { return wrap_lines(s, 228) * 9 + 5; }
/* Non-string check: a laid-out value must stay within a limit. */
static void chkv(const char* what, int value, int limit) {
  checks++;
  int ok = (value <= limit);
  if (!ok) fails++;
  printf("  %-4s %-46.46s          y=%-4d limit=%-4d %s\n",
         ok ? "ok" : "FAIL", "(laid-out height)", value, limit, what);
}
/* ---- end of the added helpers -------------------------------------------- */

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

  /* ======================================================================== */
  /* ==== BEGIN: legality report screen (source/pdna_legality.c) ============ */
  /* ======================================================================== */
  printf("\n== legality report: banner ==\n");
  /* The banner word is fixed-width at x=4; the mon's NAME is drawn from x=104, so
   * the widest verdict must stop before that or the two collide. */
  chk("banner word", 4, 100, (int)strlen("QUESTIONABLE") * SYS8_W, "QUESTIONABLE");
  chk("banner word", 4, 100, (int)strlen("ILLEGAL") * SYS8_W, "ILLEGAL");
  /* subtitles: ui_ptext_fit(4, 12, 232, ...) */
  PF("Values the games cannot produce.", 4, 232);
  PF("Worth a look, not an accusation.", 4, 232);
  PF("Nothing unusual in what we check.", 4, 232);

  printf("\n== legality report: category headers + rows ==\n");
  /* pk2_cat_name() drawn with ui_text at x=4; the divider rule starts after it. */
  T("STRUCTURE", 4); T("MOVES", 4); T("PID/RNG", 4);
  T("MET", 4); T("EGG", 4); T("FLAGS", 4);
  /* every row and note: ui_ptext_fit(14, y, 218, ...) */
  PF("No impossible or odd values found.", 14, 218);
  PF("More findings than fit; list cut off.", 14, 218);
  PF("Move-source checks: not in this build.", 14, 218);
  PF("Encounter checks: not in this build.", 14, 218);
  PF("Press A for the PID/IV RNG check.", 14, 218);
  /* the PIDIV-fallback rows this screen appends itself (worst case of each shape) */
  PF("RNG: no method makes this PID+IVs", 14, 218);
  PF("RNG: Method 4 (Unown), seed 89ABCDEF", 14, 218);
  /* widest of pk_pidiv_exempt_name()'s eight strings; the "seed" line above is the
   * widest of pk_pidiv_method_name()'s seven */
  PF("RNG check skipped: unknown origin", 14, 218);
  /* the longest strings gen3_legality2.c can hand this screen — a Pk2Row is 39
   * chars and they render HERE, so their fit is this screen's problem */
  PF("Ability slot does not match the PID", 14, 218);
  PF("Name too long for a JP-language mon", 14, 218);
  PF("Pokerus strain 8 cannot be rolled", 14, 218);
  PF("Mew/Deoxys lacks the event flag", 14, 218);

  printf("\n== legality report: footers + busy panels ==\n");
  P("PokeDNA only reads here. Nothing is changed.", 4);
  P("U/D  A RNG  R sweep  START help  B back", 4);
  P("U/D  A RNG  START help  B back", 4);
  /* RNG busy panel: ui_panel(40, 62, 160, 38) -> ink must stop by x=198 */
  chk("rng panel", 48, 198 - 48, (int)strlen("RNG CHECK") * SYS8_W, "RNG CHECK");
  PF("Searching 65,536 seeds...", 48, 150);

  printf("\n== legality report: box sweep ==\n");
  /* sweep progress panel: ui_panel(24, 58, 192, 46) -> ink must stop by x=214 */
  chk("sweep panel", 32, 214 - 32, (int)strlen("BOX SWEEP") * SYS8_W, "BOX SWEEP");
  PF("Fast checks, no RNG search.", 32, 182);
  PF("Checking 30 of 30...", 32, 176);   /* erased/redrawn in a 176 px band */
  /* list header: "BOX SWEEP" (sys8, ends at 76) plus a right-aligned counter */
  chk("sweep counter", 236 - pwidth("30 of 30 flagged"), 236 - 76,
      pwidth("30 of 30 flagged"), "30 of 30 flagged");
  /* one row = marker | slot no. | name (32..132) | verdict (138..216) | count.
   * The verdict column is the tight one: QUESTIONABLE has to fit 78 px. */
  PF("QUESTIONABLE", 138, 78);
  PF("ILLEGAL", 138, 78);
  PF("x30", 234 - 18, 18);
  /* empty-box state, drawn at x=8 */
  P("Nothing questionable in this box.", 8);
  P("All 30 cells passed the fast checks.", 8);
  P("The RNG check is per Pokemon (A).", 8);
  P("Worst first. Only flagged cells listed.", 4);
  P("U/D   A open that Pokemon   B back", 4);
  P("B back", 4);

  printf("\n== legality report: help pages ==\n");
  T("WHAT THE VERDICTS MEAN", 4);
  T("WHAT THIS SCREEN DOES", 4);
  T("QUESTIONABLE", 6);
  P("A more   B back", 4);
  /* The help prose is ui_ptext_wrap'd, so WIDTH is guaranteed — HEIGHT is not.
   * Each page lays paragraphs out top-down and must finish above the footer rule
   * at y=148, so count the wrapped lines the same way ui_ptext_break does. */
  {
    /* page 0: y=18, a 10 px heading before each of three paragraphs */
    int y = 18;
    y += 10; y += WRAPH("Every check this build can run passed. It is not proof of "
                        "anything: PokeDNA cannot see every rule of the games.");
    y += 10; y += WRAPH("A value with no path we know of. Event Pokemon, Colosseum/XD "
                        "Pokemon and in-game trades land here too, so read it as worth "
                        "a look, not as an accusation.");
    y += 10; y += WRAPH("A value the retail games cannot produce at all, such as an EV "
                        "total above 510.");
    chkv("help page 1 height", y, 148);
    /* page 1: y=18, three paragraphs, no headings */
    y = 18;
    y += WRAPH("This screen only READS. PokeDNA never edits, fixes or releases "
               "anything because of these checks.");
    y += WRAPH("A runs the PID/IV RNG check on this Pokemon: it searches 65,536 seeds "
               "for one that makes both its PID and its IVs. Eggs, event and "
               "Colosseum/XD Pokemon are exempt, because no such link exists for them.");
    y += WRAPH("R sweeps all 30 cells of this box with the fast checks only. Run the "
               "RNG check per Pokemon from here.");
    chkv("help page 2 height", y, 148);
  }
  /* ==== END: legality report screen ====================================== */

  /* ======================================================================== */
  /* ==== BEGIN: Game Boy import (source/pdna_gen12.c + the read-only mon  == */
  /* ====        menu in source/pdna_main.c)                              === */
  /* ======================================================================== */
  printf("\n== GB import: info page ==\n");
  /* Header line: ui_ptext_fit(4, 18, 232, ...). Worst case is the longest version
   * name + a 7-glyph player + a 5-digit id. */
  PF("Red/Blue/Yellow  -  MMMMMMM  ID 65535", 4, 232);
  /* The honest line is WRAPPED at 232 px into at most 2 lines starting at y=30; the
   * divider under it is at y=50, so 2 lines at 9 px pitch is the whole budget. */
  chkv("honest line fits 2 wrapped lines",
       wrap_lines("Converted copy - not a native Gen 3 Pokemon.", 232), 2);
  PF("Pokemon in this save: 999", 4, 232);
  PF("Ready to copy: 999", 4, 232);
  PF("Shown but locked: 999", 4, 232);
  PF("Slots we cannot read: 999", 4, 232);
  /* The closing paragraph is wrapped at 232 px from y=105 with a 9 px pitch, and the
   * footer rule is at y=147 -> at most 4 lines. */
  chkv("info paragraph fits above the footer",
       105 + wrap_lines("Copy a Pokemon here, then paste it into your Gen 3 boxes or the Bank. "
                        "This Game Boy save is only ever read.", 232) * 9, 147);
  T("A browse  SEL list  B back", 4);

  printf("\n== GB import: not-transferable list ==\n");
  T("NOT TRANSFERABLE", 4);
  PF("Where they are, and why they stay:", 4, 232);
  PF("Every Pokemon here can be copied.", 4, 232);
  PF("More than fit; the rest are in the boxes.", 4, 232);
  /* Three columns per row: where (x=4, 60 px), species (x=66, 74 px), reason
   * (x=142, 94 px). The widest of each. */
  PF("BOX 14 #20", 4, 60);
  PF("PARTY #6", 4, 60);
  PF("BELLSPROUT", 66, 74);          /* a 10-glyph Gen-1/2 species name */
  PF("NIDORAN?", 66, 74);
  T("B back", 4);

  printf("\n== GB import: read-only mon menu (pdna_main.c) ==\n");
  /* ui_panel(138, .., 100, ..) -> prose is ui_ptext_fit(144, y, 88, ..) and the
   * fixed-width rows are ui_text at x=148 inside a panel whose right edge is 238. */
#define RO_PROSE_W 88
  PF("Converted copy", 144, RO_PROSE_W);
  PF("Egg: can't move", 144, RO_PROSE_W);
  PF("Holding an item", 144, RO_PROSE_W);
  PF("Glitch Pokemon", 144, RO_PROSE_W);
  PF("Bad move data", 144, RO_PROSE_W);
  PF("Bad level data", 144, RO_PROSE_W);
  PF("No ID could fit", 144, RO_PROSE_W);
  PF("Can't convert", 144, RO_PROSE_W);
  chk("ro menu row", 148, 238 - 148, (int)strlen("LEGALITY") * SYS8_W, "LEGALITY");
  chk("ro menu row", 148, 238 - 148, (int)strlen("CANCEL") * SYS8_W, "CANCEL");
  chk("ro menu foot", 144, 238 - 144, (int)strlen("A ok B back") * SYS8_W, "A ok B back");

  printf("\n== GB import: message panels ==\n");
  /* s_msg: ui_text title at x=20, body via ui_ptext_fit(20, .., 200, ..); the panel
   * is ui_panel(12, 50, 216, ..) so ink must stop by x=228. */
  chk("gb msg title", 20, 228 - 20, (int)strlen("NOT A GB SAVE") * SYS8_W, "NOT A GB SAVE");
  chk("gb msg title", 20, 228 - 20, (int)strlen("CANNOT OPEN") * SYS8_W, "CANNOT OPEN");
  chk("gb msg title", 20, 228 - 20, (int)strlen("NOT NOW") * SYS8_W, "NOT NOW");
  PF("Save the Pokemon you moved,", 20, 200);
  PF("then open the GB save.", 20, 200);
  PF("The file could not be read.", 20, 200);
  /* every refusal reason pdna_gen12_mount can hand s_msg */
  PF("Not a Game Boy save file.", 20, 200);
  PF("Wrong size for a GB save.", 20, 200);
  PF("Japanese saves not supported yet.", 20, 200);
  PF("GB save checksum failed (corrupt?).", 20, 200);
  PF("Could not read the GB header.", 20, 200);
  PF("Unrecognised file.", 20, 200);
  /* ==== END: Game Boy import ============================================== */

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
