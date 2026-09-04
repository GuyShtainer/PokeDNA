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
 *
 * (run_host_tests.py reads that line out of the FIRST 24 lines of this comment — keep
 * it up here, above the long note below, or the whole test is silently skipped.)
 *
 * WHERE THE NUMBERS COME FROM — read this before adding a check.
 * A check that RE-TYPES a geometry constant or an on-screen string as its own literal
 * measures its own copy: change the real one in source/pdna_main.c and the test stays
 * green. That is worse than no test, because it makes the next person confident. So
 * anything asserted on here must come from a header the SHIPPED CODE also reads:
 *   source/ui_layout.h  — screen size, UI_FOOTER_Y, ui_popup_fit() (the real function
 *                         ui.c's ui_popup_vfit wraps)
 *   source/pdna_layout.h— per-screen geometry and the fixed strings the screens draw
 *   source/rmbl.h       — RCUE_COUNT, the rumble page's cue-row count
 * Strings that are built inside a function from data the host cannot see (a species
 * name, a path) stay out; a worst-case shape is fine as long as the FORMAT and the
 * fixed parts come from the header. Where a value genuinely cannot be shared it is
 * called out in a comment as a gap rather than mirrored silently.
 */
#include <stdio.h>
#include <string.h>

#include "ui_font.h"
#include "ui_layout.h"     /* UI_SCR_W, UI_FOOTER_Y, ui_popup_fit — as shipped */
#include "pdna_layout.h"   /* the screens' own geometry + fixed strings           */
#include "pdna_romver.h"   /* the ROM verifier's own clamps: MAX_REGIONS, MAX_IMAGE,
                            * TAIL_GUARD — i.e. the WORST CASE the verdict band's
                            * numbers can reach, taken from the shipped header rather
                            * than guessed at here */
#include "rmbl.h"          /* RCUE_COUNT: how many cue rows the Rumble page draws */
#include "mon_icons.h"     /* MON_ICON_W — pure C (stdint.h only, no tonc), declares
                            * mon_icon_for* but never calls them, so it links clean
                            * without mon_icons.c. The party icon-clearance checks
                            * below need the icon's real pixel width, not a re-typed
                            * literal — the whole point of every other check here. */

#define SCR_W   UI_SCR_W
#define SYS8_W  UI_SYS8_W   /* tonc sys8 advance: fixed 8 px per glyph */

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

/* Mirrors ui.c's padv_tight()/ui_ptext_w_tight() exactly, using the SAME
 * UI_PTEXT_TIGHT_DELTA the shipped renderer reads (ui_font.h) rather than a re-typed
 * constant — the party-name tight face used by party_draw_name_level (source/pdna_main.c). */
static int pwidth_tight(const char* s) {
  int w = 0;
  const unsigned char* p = (const unsigned char*)s;
  while (*p) {
    unsigned c;
    if (*p == 0xC3u && p[1] == 0xA9u) { c = 127; p += 2; }
    else if (*p < 0x80u) { c = *p++; }
    else { p++; while ((*p & 0xC0u) == 0x80u) p++; c = '?'; }
    if (c < 32u || c > 127u) c = '?';
    int a = (int)ui_font_w[c - 32] - UI_PTEXT_TIGHT_DELTA;
    w += a < 1 ? 1 : a;
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
/* ...and the other direction: a value that must REACH a floor (a highlight bar that has
 * to cover the whole glyph box, say). */
static void chkv_min(const char* what, int value, int floor_) {
  checks++;
  int ok = (value >= floor_);
  if (!ok) fails++;
  printf("  %-4s %-46.46s          v=%-4d min=%-4d %s\n",
         ok ? "ok" : "FAIL", "(laid-out value)", value, floor_, what);
}
/* Thousands separators, mirroring pdna_romfull.c's commas(): 33554432 -> "33,554,432".
 * The VALUE comes from PDNA_RV_MAX_IMAGE (the shipped clamp) — only the grouping is
 * duplicated, because commas() is a static in the .c. Stated as a gap rather than hidden:
 * if someone changes the separator style the screen and this test drift apart, but the
 * WIDTH this test measures cannot drift from the real bound. */
static void commas(char* out, unsigned long v) {
  char raw[16];
  int n, i, j = 0, k;
  sprintf(raw, "%lu", v);
  n = (int)strlen(raw);
  for (i = 0; i < n; i++) {
    out[j++] = raw[i];
    k = n - 1 - i;
    if (k > 0 && (k % 3) == 0) out[j++] = ',';
  }
  out[j] = 0;
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
  /* pdna_summary.c footers, drawn at x=4 — the MACROS the screen draws, not copies of
   * them, so adding a word to a hint is what fails here rather than in someone's photo. */
#define SUMFOOT(s) T(s, 4);
  PDNA_SUM_FOOTS(SUMFOOT)
#undef SUMFOOT
  /* confirm_q panel: ui_panel(16, 44, 208, 60) so the ink must stop by x=224, and the
   * lines are drawn at x=28. This is where "A = write  (backup made first)" ran 44 px
   * past the panel and 28 px off the screen before it was shortened. */
#define PANEL_R 224
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("Save changes?") * SYS8_W, "Save changes?");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("Keep this Pokemon?") * SYS8_W, "Keep this Pokemon?");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("A = write (backup first)") * SYS8_W, "A = write (backup first)");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("B = discard") * SYS8_W, "B = discard");
  chk("confirm panel", 28, PANEL_R - 28, (int)strlen("B = discard it") * SYS8_W, "B = discard it");

  printf("\n== summary screen: the IV reroll row (#25) ==\n");
  /* The button and its two history arrows share ONE 138 px card row: the label is centred
   * and the arrows sit hard against each end, so the label must not reach either of them. */
  T(PDNA_SUM_ROLL_LBL, PDNA_SUM_CARD_X + (PDNA_SUM_CARD_W
      - (int)strlen(PDNA_SUM_ROLL_LBL) * SYS8_W) / 2);
  chkv("reroll label clear of both arrows",
       (int)strlen(PDNA_SUM_ROLL_LBL) * SYS8_W, PDNA_SUM_CARD_W - 2 * SYS8_W - 2);
  T(PDNA_SUM_ARROW_L, PDNA_SUM_CARD_X);
  T(PDNA_SUM_ARROW_R, PDNA_SUM_CARD_X + PDNA_SUM_CARD_W - SYS8_W);
  T(PDNA_SUM_ROLLING, PDNA_SUM_CARD_X);
  /* The note row is ui_ptext_fit'd into the card width, so it can only clip itself —
   * measure every string it can hold, including the widest possible counter. */
  PF(PDNA_SUM_NOTE_IDLE,   PDNA_SUM_CARD_X, PDNA_SUM_CARD_W);
  PF(PDNA_SUM_NOTE_VIEW,   PDNA_SUM_CARD_X, PDNA_SUM_CARD_W);
  PF(PDNA_SUM_NOTE_BADEGG, PDNA_SUM_CARD_X, PDNA_SUM_CARD_W);
  { char b[48];
    /* the widest counter the ring can ever print: two 2-digit numbers at capacity, and
     * the longest of the three tails */
    sprintf(b, PDNA_SUM_NOTE_FMT, IVH_CAP, IVH_CAP, PDNA_SUM_TAIL_ORIG);
    PF(b, PDNA_SUM_CARD_X, PDNA_SUM_CARD_W);
    sprintf(b, PDNA_SUM_NOTE_FMT, IVH_CAP, IVH_CAP, PDNA_SUM_TAIL_IVONLY);
    PF(b, PDNA_SUM_CARD_X, PDNA_SUM_CARD_W);
    sprintf(b, PDNA_SUM_NOTE_FMT, IVH_CAP, IVH_CAP, PDNA_SUM_TAIL_PIDIV);
    PF(b, PDNA_SUM_CARD_X, PDNA_SUM_CARD_W); }

  printf("\n== summary screen: the \"PID moves\" confirm panel (#25) ==\n");
  /* Title and hint are sys8; the five variable lines are proportional (ui_ptext_fit),
   * because "Nat ADAMANT>ADAMANT" is 19 glyphs = 152 px at sys8's fixed cell against a
   * 120 px column. All of them are measured at the widest value they can hold. */
#define RC_TX (PDNA_SUM_RC_X + PDNA_SUM_RC_PAD)
  chk("confirm PID panel", RC_TX, PDNA_SUM_RC_TEXT_W,
      (int)strlen(PDNA_SUM_RC_TITLE) * SYS8_W, PDNA_SUM_RC_TITLE);
  chk("confirm PID panel", RC_TX, PDNA_SUM_RC_TEXT_W,
      (int)strlen(PDNA_SUM_RC_HINT) * SYS8_W, PDNA_SUM_RC_HINT);
  PF(PDNA_SUM_RC_TRADE,     RC_TX, PDNA_SUM_RC_TEXT_W);
  PF(PDNA_SUM_RC_SHINY_ON,  RC_TX, PDNA_SUM_RC_TEXT_W);
  PF(PDNA_SUM_RC_SHINY_OFF, RC_TX, PDNA_SUM_RC_TEXT_W);
  { char b[48];
    sprintf(b, PDNA_SUM_RC_NAT_FMT, PDNA_SUM_RC_NAT_LONGEST, PDNA_SUM_RC_NAT_LONGEST);
    PF(b, RC_TX, PDNA_SUM_RC_TEXT_W);
    sprintf(b, PDNA_SUM_RC_SEX_FMT, "M", "F");      PF(b, RC_TX, PDNA_SUM_RC_TEXT_W);
    sprintf(b, PDNA_SUM_RC_ABI_FMT, 2u, 1u);        PF(b, RC_TX, PDNA_SUM_RC_TEXT_W);
    sprintf(b, PDNA_SUM_RC_UNO_FMT, 'W', '?');      PF(b, RC_TX, PDNA_SUM_RC_TEXT_W); }
  /* ...and the three vertical facts the row depends on: the note must finish above the
   * screen's footer rule, the panel must clear the footer, and the button must not sit on
   * top of its own note. */
  chkv("reroll note above the footer rule",
       PDNA_SUM_NOTE_Y + UI_ROW_H, UI_FOOTER_RULE_Y - 1);
  chkv("PID-moves panel above the footer",
       PDNA_SUM_RC_Y + PDNA_SUM_RC_H, UI_FOOTER_Y - 1);
  chkv("reroll button clear of its note row",
       PDNA_SUM_ROLL_Y + UI_ROW_H, PDNA_SUM_NOTE_Y - 1);
  /* the framed button box must not reach into the note row either */
  chkv("reroll box clear of its note row",
       PDNA_SUM_ROLL_Y + PDNA_SUM_ROLL_BOX_DY + PDNA_SUM_ROLL_BOX_H, PDNA_SUM_NOTE_Y - 1);

  printf("\n== day-care yard (#21) ==\n");
  /* The three-row status panel. Geometry from pdna_layout.h (PDNA_DCY_*), which is what
   * source/pdna_main.c draws it with: text starts at PDNA_DCY_TEXT_X and may ink up to
   * the panel's right border column, i.e. PDNA_DCY_TEXT_W pixels. There is no vertical
   * slack at all (see the descender check at the end of this file), so anything added
   * here has to fit the WIDTH exactly. */
#define DC_PANEL_W PDNA_DCY_TEXT_W
  chk("daycare panel", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("They get along very well!"), "They get along very well!");
  chk("daycare panel", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("They'd rather be elsewhere."), "They'd rather be elsewhere.");
  chk("daycare panel", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("(no Egg: incompatible pair)"), "(no Egg: incompatible pair)");
  chk("daycare panel", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("An EGG is ready to collect!"), "An EGG is ready to collect!");
  chk("daycare panel", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("Egg check ~9999 steps (70%)"), "Egg check ~9999 steps (70%)");
  chk("daycare panel", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("One Pokemon is boarding."), "One Pokemon is boarding.");
  chk("daycare panel", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("No Pokemon are boarding."), "No Pokemon are boarding.");
  /* pk_daycare_yard_note's three returns — the lines that say what is actually yours */
  chk("yard note", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("Others are just visiting."), "Others are just visiting.");
  chk("yard note", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("Both Day-Care slots are full."), "Both Day-Care slots are full.");
  chk("yard note", PDNA_DCY_TEXT_X, DC_PANEL_W, pwidth("The Day-Care holds 2 Pokemon."), "The Day-Care holds 2 Pokemon.");
  P(PDNA_DCY_HINT_PAIR, 4);
  P(PDNA_DCY_HINT_PUT, 4);
  /* the Settings screen's own one-liner about them (drawn there, checked here with
   * its neighbours) */
  P(PDNA_SET_NOTE, PDNA_SET_HELP_X);
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
  /* The panel and its insets are PDNA_MONMENU_* (pdna_layout.h) — the same numbers
   * app_mon_menu_readonly draws with. The prose below is pdna_gen12.c's "why this one
   * is locked" set, which is why it is measured here and not with the popup geometry. */
#define RO_PROSE_X (PDNA_MONMENU_X + PDNA_MONMENU_PAD)
#define RO_PROSE_W PDNA_MONMENU_PROSE_W
#define RO_ROW_X   (PDNA_MONMENU_X + PDNA_MONMENU_ROW_DX)
  PF("Converted copy", RO_PROSE_X, RO_PROSE_W);
  PF("Egg: can't move", RO_PROSE_X, RO_PROSE_W);
  PF("Holding an item", RO_PROSE_X, RO_PROSE_W);
  PF("Glitch Pokemon", RO_PROSE_X, RO_PROSE_W);
  PF("Bad move data", RO_PROSE_X, RO_PROSE_W);
  PF("Bad level data", RO_PROSE_X, RO_PROSE_W);
  PF("No ID could fit", RO_PROSE_X, RO_PROSE_W);
  PF("Can't convert", RO_PROSE_X, RO_PROSE_W);
  chk("ro menu row", RO_ROW_X, PDNA_MONMENU_ROW_W,
      (int)strlen(PDNA_LBL_LEGALITY) * SYS8_W, PDNA_LBL_LEGALITY);
  chk("ro menu row", RO_ROW_X, PDNA_MONMENU_ROW_W,
      (int)strlen(PDNA_LBL_CANCEL) * SYS8_W, PDNA_LBL_CANCEL);
  chk("ro menu foot", RO_PROSE_X, PDNA_MONMENU_W - 2 * PDNA_MONMENU_PAD,
      (int)strlen(PDNA_MONMENU_FOOT_TXT) * SYS8_W, PDNA_MONMENU_FOOT_TXT);

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

  /* ==== ADDED for the GB mon editor screen (source/pdna_gbedit.c) + its persist-path
   * popups (source/pdna_gen12.c gb_edit_hook/gb_edit_persist) — every fixed string
   * both draw now lives in pdna_layout.h as a PDNA_GBEDIT_* macro so this test reads
   * the SAME literal the screen does, not a re-typed copy (this file's own rule). */
  printf("\n== GB mon editor ==\n");
  T(PDNA_GBEDIT_TITLE, 4);
  T(PDNA_GBEDIT_CONFIRM_TITLE, 20);
  PF(PDNA_GBEDIT_WRITE_ANYWAY, 20, PDNA_GBEDIT_CONFIRM_W);
  T(PDNA_GBEDIT_A_WRITE, 20);
  T(PDNA_GBEDIT_B_CANCEL, 20);
  T(PDNA_GBEDIT_BAK_L1, 20);
  T(PDNA_GBEDIT_BAK_L2, 20);
  {
    /* confirm()'s dynamic prose block draws, worst case: an issue sentence wrapped to
     * PDNA_GBEDIT_CONFIRM_MAXLN lines, the "write anyway?" line + its gap, then a
     * stale-stats sentence also wrapped to PDNA_GBEDIT_CONFIRM_MAXLN lines — the exact
     * arithmetic confirm() itself does. Assert "B = cancel", the LOWER of the two
     * fixed lines below that block, still ends above the backup footer (moved to
     * PDNA_GBEDIT_BAK_Y1 for exactly this reason — see that macro's own comment). */
    int y = PDNA_GBEDIT_CONFIRM_Y0;
    y += PDNA_GBEDIT_CONFIRM_MAXLN * PDNA_GBEDIT_CONFIRM_LINE_H;
    y += PDNA_GBEDIT_CONFIRM_GAP;
    y += PDNA_GBEDIT_CONFIRM_MAXLN * PDNA_GBEDIT_CONFIRM_LINE_H;
    chkv("gbedit confirm: worst-case B=cancel line ends above the backup footer",
         y + PDNA_GBEDIT_AB_DY2 + UI_ROW_H - 1, PDNA_GBEDIT_BAK_Y1 - 1);
  }
  chkv("gbedit confirm: second backup footer line stays on-screen",
       PDNA_GBEDIT_BAK_Y2 + UI_ROW_H - 1, UI_SCR_H - 1);

  /* press()'s msg_wait popups — all through the (28, .., 184) clamp msg_wait itself
   * draws with (source/pdna_main.c msg_wait). */
  PF(PDNA_GBEDIT_BADCHARSET_TITLE, 28, 184);
  PF(PDNA_GBEDIT_BADCHARSET_L1,    28, 184);
  PF(PDNA_GBEDIT_BADNAME_L1,       28, 184);
  PF(PDNA_GBEDIT_MOVE_LATE_TITLE,  28, 184);
  PF(PDNA_GBEDIT_MOVE_LATE_L1,     28, 184);
  PF(PDNA_GBEDIT_MOVE_LATE_L2,     28, 184);
  PF(PDNA_GBEDIT_MOVE_DUP_TITLE,   28, 184);
  PF(PDNA_GBEDIT_MOVE_DUP_L1,      28, 184);
  PF(PDNA_GBEDIT_MOVE_DUP_L2,      28, 184);

  /* gb_edit_persist's SF_ERR_RENAME switch + gb_edit_hook's SF_ERR_UNWRITABLE hint —
   * same (28, .., 184) msg_wait clamp. PDNA_GBEDIT_UNCONFIRMED_L2 is deliberately
   * SHORTER than pdna_main.c's own wording for the same case ("Could not re-check the
   * card. Verify it." measures 191px, over budget there too — this file must not
   * copy that clip; see the macro's own comment in pdna_layout.h). */
  PF(PDNA_GBEDIT_UNCONFIRMED_TITLE, 28, 184);
  PF(PDNA_GBEDIT_UNCONFIRMED_L2,    28, 184);
  PF(PDNA_GBEDIT_TMPONLY_TITLE,     28, 184);
  PF(PDNA_GBEDIT_TMPONLY_L2,        28, 184);
  PF(PDNA_GBEDIT_TMPANDOLD_TITLE,   28, 184);
  PF(PDNA_GBEDIT_TMPANDOLD_L2,      28, 184);
  PF(PDNA_GBEDIT_SAVELOST_TITLE,    28, 184);
  PF(PDNA_GBEDIT_SAVELOST_L1,       28, 184);
  PF(PDNA_GBEDIT_SAVELOST_BAK,      28, 184);
  PF(PDNA_GBEDIT_SAVELOST_NOBAK,    28, 184);
  PF(PDNA_GBEDIT_UNWRITABLE_HINT,   28, 184);

  /* gb_edit_hook / gb_edit_persist's own gate + verdict popups — everything else the
   * S2 edit path draws. s_busy's own line ("Saving - do not power off") and the
   * caller-supplied line draw via ui_text at x=28 (T(), fixed sys8); every msg_wait
   * title/line is PF() at the same (28, .., 184) clamp as the rest of this screen. */
  T(PDNA_GBEDIT_BUSY_SAVING,  28);
  T(PDNA_GBEDIT_BUSY_BACKUP,  28);
  T(PDNA_GBEDIT_BUSY_WRITING, 28);
  PF(PDNA_GBEDIT_READONLY_TITLE,    28, 184);
  PF(PDNA_GBEDIT_NEEDS_OMEGA,       28, 184);
  PF(PDNA_GBEDIT_BOXWR_TITLE,       28, 184);
  PF(PDNA_GBEDIT_BOXRD_TITLE,       28, 184);
  PF(PDNA_GBEDIT_EMPTYSLOT_TITLE,   28, 184);
  PF(PDNA_GBEDIT_EMPTYSLOT_L1,      28, 184);
  PF(PDNA_GBEDIT_REFUSED_TITLE,     28, 184);
  PF(PDNA_GBEDIT_NOVERIFY_L1,       28, 184);
  PF(PDNA_GBEDIT_NOTHING_L2,        28, 184);
  PF(PDNA_GBEDIT_UNCHANGED_L2,      28, 184);
  PF(PDNA_GBEDIT_BACKUPFAIL_TITLE,  28, 184);
  PF(PDNA_GBEDIT_WRITEFAIL_TITLE,   28, 184);
  PF(PDNA_GBEDIT_DISCARDED_L2,      28, 184);

  /* S3: MOVE TO / RELEASE (source/pdna_gen12.c). RELEASE's title goes through the
   * shared app_confirm(), which draws at the same (28, .., 184) clamp as msg_wait; the
   * move-refusal hints are msg_wait's own second line, same clamp. */
  PF(PDNA_GBEDIT_RELEASE_TITLE,     28, 184);
  PF(PDNA_GBEDIT_MOVE_NEEDSBASE_L2, 28, 184);
  PF(PDNA_GBEDIT_MOVE_FLOOR_L2,     28, 184);
  PF(PDNA_GBEDIT_MOVE_MAIL_L2,      28, 184);
  PF(PDNA_GBEDIT_MOVE_FULL_L2,      28, 184);

  /* gb_pick_box's full-screen list: fixed sys8 title/footer at x=4, same as
   * gb_report_page/gb_info_page's own "NOT TRANSFERABLE" / "A browse..." checks above. */
  T(PDNA_GBEDIT_PICKBOX_TITLE, 4);
  T(PDNA_GBEDIT_PICKBOX_FOOT,  4);
  /* Its last visible row's ink must clear the y=147 footer rule. */
  chk("pickbox last row", 0,
      147 - 1,
      PDNA_GBEDIT_PICKBOX_Y0 + (PDNA_GBEDIT_PICKBOX_ROWS - 1) * PDNA_GBEDIT_PICKBOX_ROW_H + UI_ROW_H - 1,
      "pickbox rows clear the footer rule");
  /* ==== END: GB mon editor ================================================= */

  /* ==== ADDED for the native-generation art router (source/pdna_origin_art.c) ====
   * Guy: "if a pokemon is from gen 1, use a gen 1 sprite ... The bank should show all
   * in parallel." These are the fixed strings that feature puts on screen.
   *
   * (a) The provenance stamp on the summary portrait is
   *         ui_name_chip(13, 15, 30, 11, era colour, white, pdna_origin_tag(o))
   *     and ui_name_chip (source/ui.c:374-379) draws its label with ui_ptext at x+2
   *     after clipping it to w - 4 == 26 px. A tag wider than that loses its tail
   *     SILENTLY, and "GB1?" losing its '?' would turn "we are not sure which Game Boy
   *     generation this is" into a confident claim -- the exact lie this feature must
   *     not tell. So the budget here is 26, not the screen.
   *
   * (b) pdna_origin_text() is the one-line honest description, sized for a full-width
   *     row of the ORIGIN / MET card (ui_text/ui_ptext at x = 98, ink must stop by
   *     238 -- source/pdna_summary.c:315-338). */
  printf("\n== native-generation art: provenance strings ==\n");
#define CHIP_W 26                       /* ui_name_chip(.., w=30, ..) -> w - 4 */
  PF("GB1",  15, CHIP_W);
  PF("GB2",  15, CHIP_W);
  PF("GB1?", 15, CHIP_W);
#define ORIGIN_ROW_W (238 - 98)
  PF("Gen 3 native",             98, ORIGIN_ROW_W);
  PF("Gen 1 import (converted)", 98, ORIGIN_ROW_W);
  PF("Gen 2 import (converted)", 98, ORIGIN_ROW_W);
  PF("GB import: Gen 1 or 2",    98, ORIGIN_ROW_W);
  /* ==== END: native-generation art ======================================== */

  /* ======================================================================== */
  /* ==== BEGIN: popups vs the footer row, + the 2026-08 text-fit sweep ===== */
  /* ====        (source/ui_layout.h, source/pdna_layout.h,                  */
  /* ====         source/pdna_main.c, source/pdna_pick.c)                 === */
  /* ========================================================================
   * A popup is drawn OVER a screen that has already printed its own control
   * hints on the bottom row, and it carries a hint line of its own. Anything it
   * paints from y=150 down therefore lands on the screen's footer: the nav menu
   * (19 entries -> a 164 px panel on a 160 px screen) put "A pick B back" on top
   * of the box screen's "A menu UP SEL B" and, because that panel is translucent,
   * the two showed THROUGH each other as "A pick ABmback UP SEL B". The per-mon
   * menu and the party popup sheared the top rows off the same footer.
   *
   * ui_layout.h names that row (UI_FOOTER_Y) and ui_popup_fit() lays a popup out
   * above it, windowing the list rather than overflowing.
   *
   * EVERY constant and string below is #included from the header the SHIPPED CODE
   * reads — ui_popup_fit() here IS the function ui.c's ui_popup_vfit calls, and the
   * strings are the ones pdna_main.c passes to ui_text. An earlier version of this
   * block re-typed all of them (#define NAV_ROW_H 11, "Yard visitors:  ...", the
   * day-care 122/30/142) and therefore could not fail when a screen changed: it was
   * measuring its own copy. If you change a number in pdna_layout.h and nothing here
   * goes red, the check for it is missing, not satisfied. */
  printf("\n== popups must end above the footer row ==\n");
  /* Worst case over every row count each call site can produce, run through the REAL
   * layout function rather than a mirror of its arithmetic. */
  { int worst = 0, y, h;
    for (int n = 1; n <= PDNA_MONMENU_MAX; n++) {                 /* app_mon_menu */
      ui_popup_fit(n, PDNA_MONMENU_ROW_H, PDNA_MONMENU_HEAD, PDNA_MONMENU_FOOT, &y, &h);
      if (y + h > worst) worst = y + h; }
    chkv("app_mon_menu panel bottom (every action count)", worst, UI_FOOTER_Y); }
  { /* SHOULD-FIX 6 (2026-08-19): app_mon_menu now takes a caller-supplied footer_y —
     * the party overlay passes PDNA_PTY_FOOTER_Y (133), well above the global
     * UI_FOOTER_Y (150) the check right above this one already covers for box/bank/
     * party_list. Every party mon has 9+ actions (occupied: VIEW/EDIT, ITEM, LEGALITY,
     * MOVE TO BOX, COPY, DUPLICATE, TO DAY-CARE, EXPORT, RELEASE, CANCEL = 10), which
     * is exactly the row count that used to seat a 146px panel at y=2..148 — squarely
     * on top of the message box that starts at y=133. This is the check that would
     * have caught it: laid out against ui_popup_fit's OLD global-only contract (before
     * ui_popup_fit_at existed) there was no way to even express "against 133", so the
     * gap was structural, not just an untested case. */
    int worst = 0, y, h;
    for (int n = 1; n <= PDNA_MONMENU_MAX; n++) {
      ui_popup_fit_at(n, PDNA_MONMENU_ROW_H, PDNA_MONMENU_HEAD, PDNA_MONMENU_FOOT,
                      PDNA_PTY_FOOTER_Y, &y, &h);
      if (y + h > worst) worst = y + h; }
    chkv("app_mon_menu panel bottom, called from the party overlay (every action count)",
         worst, PDNA_PTY_FOOTER_Y); }
  { /* app_mon_menu_readonly: header grows a line per prose line (source note / why
     * this record is locked), so try every combination. */
    int worst = 0, y, h;
    for (int extra = 0; extra <= 2; extra++)
      for (int n = 2; n <= PDNA_ROMENU_MAX; n++) {
        ui_popup_fit(n, PDNA_MONMENU_ROW_H,
                     PDNA_ROMENU_HDR + extra * PDNA_ROMENU_LINE + PDNA_ROMENU_HEAD_PAD,
                     PDNA_MONMENU_FOOT, &y, &h);
        if (y + h > worst) worst = y + h; }
    chkv("app_mon_menu_readonly panel bottom", worst, UI_FOOTER_Y); }
  { /* dex_bulk (pdna_pick.c). Six options used to give a fixed my=42 panel ending at
     * y=154, on top of the dex screen's own footer. */
    int worst = 0, y, h;
    for (int n = 4; n <= PDNA_DEXBULK_MAX; n++) {
      ui_popup_fit(n, PDNA_DEXBULK_ROW_H, PDNA_DEXBULK_HEAD, PDNA_DEXBULK_FOOT, &y, &h);
      if (y + h > worst) worst = y + h; }
    chkv("dex_bulk panel bottom", worst, UI_FOOTER_Y); }
  { /* dc_menu / dc_withdraw (no hint line of their own) */
    int worst = 0, y, h;
    for (int n = 2; n <= PDNA_DCPOP_MAX; n++) {
      ui_popup_fit(n, PDNA_DCPOP_ROW_H, PDNA_DCPOP_HEAD, PDNA_DCPOP_FOOT, &y, &h);
      if (y + h > worst) worst = y + h; }
    chkv("day-care popups panel bottom", worst, UI_FOOTER_Y); }
  /* A popup's own hint sits at my + mh + PDNA_POPUP_HINT_DY and inks UI_ROW_H rows;
   * ui_panel's bottom rule is at my + mh - 2, so the hint must finish above it. */
  chkv("popup hint clear of the panel's bottom rule",
       PDNA_POPUP_HINT_DY + UI_ROW_H - 1, -2);
  { /* ...and clear of the LAST list row's ink, which ends at head + (vis-1)*row_h + 7
     * while the hint starts at head + vis*row_h + foot + HINT_DY. */
    int y, h;
    int vis = ui_popup_fit(PDNA_MONMENU_MAX, PDNA_MONMENU_ROW_H, PDNA_MONMENU_HEAD,
                           PDNA_MONMENU_FOOT, &y, &h);
    chkv("mon-menu hint clear of the last row's ink",
         PDNA_MONMENU_HEAD + (vis - 1) * PDNA_MONMENU_ROW_H + UI_ROW_H - 1,
         h + PDNA_POPUP_HINT_DY - 1);
    vis = ui_popup_fit(PDNA_DEXBULK_MAX, PDNA_DEXBULK_ROW_H, PDNA_DEXBULK_HEAD,
                       PDNA_DEXBULK_FOOT, &y, &h);
    chkv("dex_bulk hint clear of the last row's ink",
         PDNA_DEXBULK_HEAD + (vis - 1) * PDNA_DEXBULK_ROW_H + UI_ROW_H - 1,
         h + PDNA_POPUP_HINT_DY - 1); }

  printf("\n== the footer row itself ==\n");
  /* UI_FOOTER_Y is the y the screens PASS to ui_text — pdna_main.c's box footer, the
   * party footer, "B=back", the build stamp — not merely the limit the popup arithmetic
   * derives from. That matters for what CAN be tested: while the footers were literal
   * 150s, every "does the popup clear the footer?" check compared a value ui_popup_fit
   * had derived FROM UI_FOOTER_Y against UI_FOOTER_Y itself and so could never fail,
   * while the real footers were free to drift away from it. With one symbol the drift is
   * impossible, and what is left to test is that the symbol is a legal place to draw:
   * sys8 inks UI_ROW_H rows from it, and the last of them must be on the screen. */
  chkv("footer text's last ink row is on screen",
       UI_FOOTER_Y + UI_ROW_H - 1, UI_SCR_H - 1);
  /* Screens whose hint row sits LOWER carry their own constant. Each must be at or below
   * UI_FOOTER_Y — a footer ABOVE the row popups are laid out to clear is exactly how
   * "A pick ABmback UP SEL B" happened — and each must still fit on the screen. */
  chkv_min("settings/rumble footer is inside the footer band",
           PDNA_SET_FOOTER_Y, UI_FOOTER_Y);
  chkv("settings/rumble footer last ink row is on screen",
       PDNA_SET_FOOTER_Y + UI_ROW_H - 1, UI_SCR_H - 1);
  chkv_min("day-care yard footer is inside the footer band",
           PDNA_DCY_FOOTER_Y, UI_FOOTER_Y);
  chkv("day-care yard footer last ink row is on screen",
       PDNA_DCY_FOOTER_Y + UI_FONT_CELL_H - 1, UI_SCR_H - 1);
  chkv_min("filter-list footer is inside the footer band",
           PDNA_FILT_FOOTER_Y, UI_FOOTER_Y);
  chkv("filter-list footer last ink row is on screen",
       PDNA_FILT_FOOTER_Y + UI_ROW_H - 1, UI_SCR_H - 1);

  printf("\n== ui_popup_fit: the layout function's own contract ==\n");
  /* The per-screen sweeps above only ever reach ONE branch of ui_popup_fit. At every
   * geometry PokeDNA ships, (UI_FOOTER_Y - head - foot) / row_h lands in 7..9, so the
   * "vis > nrows" clamp does the work and the "vis < 1" clamp is unreachable — asserting
   * that clamp from those call sites was theatre: deleting `if (vis < 1) vis = 1;` from
   * ui_layout.h left every one of them green. So drive the function directly over
   * geometries a caller can reach by accident (a header that eats the screen, a pitch
   * taller than the panel, row_h == 0 hitting the divide-by-zero guard).
   *
   * Contract at ANY input: 1 <= vis <= nrows, and y >= 0. A caller draws `vis` of its
   * `n` labels, so vis > n walks off the end of the label array and vis < 1 gives a menu
   * with nothing in it — neither shows up in the panel's height. The panel BOTTOM is NOT
   * part of the contract at absurd head/foot: ui_popup_fit deliberately prefers one
   * usable row to a zero-height panel, so that half is asserted only where a head + one
   * row + foot demonstrably fits above the footer. */
  { int over = 0, under = 0, high = 0, bottom_ok = 1;
    static const int RH[]   = { 0, 1, 8, 13, 14, 60, 149, 150, 151, 400 };
    static const int HEAD[] = { 0, 15, 18, 100, 140, 149, 150, 200 };
    static const int FOOT[] = { 0, 8, 11, 14, 60, 149, 150 };
    for (unsigned a = 0; a < sizeof RH / sizeof RH[0]; a++)
      for (unsigned b = 0; b < sizeof HEAD / sizeof HEAD[0]; b++)
        for (unsigned c = 0; c < sizeof FOOT / sizeof FOOT[0]; c++)
          for (int n = 1; n <= 20; n++) {
            int y, h, vis = ui_popup_fit(n, RH[a], HEAD[b], FOOT[c], &y, &h);
            if (vis > n) over  = 1;
            if (vis < 1) under = 1;
            if (y < 0)   high  = 1;
            if (HEAD[b] + RH[a] + FOOT[c] <= UI_FOOTER_Y && y + h > UI_FOOTER_Y)
              bottom_ok = 0;
          }
    chkv("ui_popup_fit never returns more rows than it was given (0=ok)", over, 0);
    chkv("ui_popup_fit always leaves at least one row (0=ok)", under, 0);
    chkv("ui_popup_fit never places a panel off the top of the screen (0=ok)", high, 0);
    chkv_min("ui_popup_fit keeps the panel above the footer whenever a row fits (1=ok)",
             bottom_ok, 1); }

  printf("\n== nav menu (START over the box) ==\n");
  /* FIXED height and drawn exactly once, so it cannot window: pdna_main.c guards it
   * with a _Static_assert on the same PDNA_NAV_MH, and this re-checks it on the host
   * (a build failure is the real gate; this one names the culprit in one line). */
  chkv("nav panel height", PDNA_NAV_MH, UI_FOOTER_Y);
  { const int my = (UI_FOOTER_Y - PDNA_NAV_MH) / 2;
    chkv("nav panel bottom", my + PDNA_NAV_MH, UI_FOOTER_Y);
    /* its own hint line sits at my + mh + HINT_DY and inks UI_ROW_H rows */
    chkv("nav hint last ink row",
         my + PDNA_NAV_MH + PDNA_NAV_HINT_DY + UI_ROW_H - 1, UI_FOOTER_Y - 1); }
  /* INSIDE the panel. nav_menu now places its rows at my + PDNA_NAV_HEAD + i * ROW_H and
   * its selection bar PDNA_NAV_BAND_DY above that — until 2026-08 those three sites said
   * "my + 20" and PDNA_NAV_HEAD fed only PDNA_NAV_MH, so shrinking the constant moved the
   * PANEL and left the rows behind, silently. These are the checks that constant now
   * answers to; every one of them is a collision the fixed-height menu cannot survive,
   * because it has no scroll and is composited exactly once. */
  chkv("nav title clear of the divider under it",
       PDNA_NAV_TITLE_DY + UI_ROW_H - 1, PDNA_NAV_DIV_DY - 1);
  chkv("nav divider clear of the first row's selection bar",
       PDNA_NAV_DIV_DY, PDNA_NAV_HEAD + PDNA_NAV_BAND_DY - 1);
  chkv_min("nav selection bar covers the label's glyph box",
           PDNA_NAV_BAND_DY + PDNA_NAV_BAND_H - 1, UI_FONT_CELL_H - 1);
  chkv("nav selection bar top is not below the label", PDNA_NAV_BAND_DY, 0);
  chkv("nav selection bar clear of the next row's ink",
       PDNA_NAV_BAND_DY + PDNA_NAV_BAND_H - 1, PDNA_NAV_ROW_H - 1);
  chkv("nav selection bar clear of the previous row's ink",
       UI_FONT_CELL_H - 1 - PDNA_NAV_ROW_H, PDNA_NAV_BAND_DY - 1);
  /* the hint line has to clear the LAST row of the taller column, not just the panel */
  chkv("nav hint clear of the last row's ink",
       PDNA_NAV_HEAD + (PDNA_NAV_ROWS - 1) * PDNA_NAV_ROW_H + UI_FONT_CELL_H - 1,
       PDNA_NAV_MH + PDNA_NAV_HINT_DY - 1);
  /* and the SECOND column's selection bar must stay inside the panel's right border */
  chkv("nav second column inside the panel",
       PDNA_NAV_PAD - PDNA_NAV_LABEL_DX + PDNA_NAV_COL_W + PDNA_NAV_BAND_W,
       PDNA_NAV_MW - 2);
  /* Labels are proportional and live inside the selection band, inset by LABEL_DX from
   * its left edge — so PDNA_NAV_LABEL_W, not the screen. The list is the SAME X-macro
   * pdna_main.c builds both the enum and the drawn label array from. */
#define NAV_LABEL_ONE(id, label) label,
  { static const char* const NAVL[] = { PDNA_NAV_ITEMS(NAV_LABEL_ONE) };
    for (int i = 0; i < PDNA_NAV_COUNT; i++)
      chk("nav label", 0, PDNA_NAV_LABEL_W, pwidth(NAVL[i]), NAVL[i]); }
  /* the menu's own hint, drawn at mx + PDNA_NAV_PAD inside a PDNA_NAV_MW-wide panel
   * whose right border column is mx + MW - 2 */
  chk("nav hint", PDNA_NAV_PAD, PDNA_NAV_MW - PDNA_NAV_PAD - 2,
      (int)strlen(PDNA_NAV_HINT) * SYS8_W, PDNA_NAV_HINT);

  printf("\n== per-mon action menu rows ==\n");
  /* sys8 rows at PDNA_MONMENU_X + ROW_DX inside a PDNA_MONMENU_W-wide panel. Both
   * action popups draw from this one list of labels. */
#define MON_LABEL_ONE(s) s,
  { static const char* const ML[] = { PDNA_MONMENU_LABELS(MON_LABEL_ONE) };
    for (unsigned i = 0; i < sizeof ML / sizeof ML[0]; i++)
      chk("mon menu row", PDNA_MONMENU_X + PDNA_MONMENU_ROW_DX, PDNA_MONMENU_ROW_W,
          (int)strlen(ML[i]) * SYS8_W, ML[i]); }
  chk("mon menu hint", PDNA_MONMENU_X + PDNA_MONMENU_PAD,
      PDNA_MONMENU_W - 2 * PDNA_MONMENU_PAD,
      (int)strlen(PDNA_MONMENU_FOOT_TXT) * SYS8_W, PDNA_MONMENU_FOOT_TXT);
  /* (the read-only popup's two prose lines are ui_ptext_fit'd to PDNA_MONMENU_PROSE_W;
   * the strings are pdna_gen12.c's and are measured in the GB-import block above,
   * against that same shared constant) */

  printf("\n== FILTER / SORT row pitch (source/pdna_pick.c) ==\n");
  /* The two dex/species lists: PDNA_FILT_ROW_H pitch with a BORDERLESS bar drawn by
   * filt_bar() as ui_fill_rect(BAR_X, y + BAR_DY, BAR_W, BAR_H). Two invariants, both
   * expressed in the shared constants:
   *   (1) the bar must CONTAIN the 8-row glyph box, or its edge strikes through the
   *       text — that is what ui_panel's frame at y+7 used to do to "Sort: No. (dex)";
   *   (2) it must stop before the next row's ink, or "Status: All" looks clipped by
   *       the box above it. */
  chkv_min("filter bar covers the glyph box",
           PDNA_FILT_BAR_DY + PDNA_FILT_BAR_H - 1, UI_ROW_H - 1);
  chkv("filter bar top is not below the glyph box", PDNA_FILT_BAR_DY, 0);
  chkv("filter bar clear of the next row's ink",
       PDNA_FILT_BAR_DY + PDNA_FILT_BAR_H - 1, PDNA_FILT_ROW_H - 2);
  chkv("filter list last row ink",
       PDNA_FILT_Y0 + (PDNA_FILT_VIS - 1) * PDNA_FILT_ROW_H + UI_ROW_H - 1,
       UI_FOOTER_Y - 1);
  /* ROW TEXT. All three lists (filter_menu, dex_menu, item_filter_menu) draw their rows
   * at PDNA_FILT_TEXT_X inside the PDNA_FILT_BAR_W-wide highlight, so the ink budget is
   * the bar's right border column — PDNA_FILT_ROW_W. Until 2026-08 those eight call
   * sites spelled the x as a literal 8 and PDNA_FILT_TEXT_X was read by NOBODY: a
   * constant sitting in a shared header guarding nothing, which reads as coverage. The
   * species / type / item names come out of tables in pdna_pick.c the host cannot see,
   * so what is pinned here is the Sort row — the widest FIXED string on the list, and
   * the same format + values the screens siprintf — plus dex_menu's bulk-edit row. */
  chkv_min("filter row text starts inside the highlight bar",
           PDNA_FILT_TEXT_X, PDNA_FILT_BAR_X + 1);
  { char row[48];
#define FILT_SORT_ONE(s) s,
    static const char* const SORTV[] = { PDNA_FILT_SORT_VALUES(FILT_SORT_ONE) };
    for (unsigned i = 0; i < sizeof SORTV / sizeof SORTV[0]; i++) {
      snprintf(row, sizeof row, PDNA_FILT_SORT_FMT, SORTV[i]);
      chk("filter sort row", PDNA_FILT_TEXT_X, PDNA_FILT_ROW_W,
          (int)strlen(row) * SYS8_W, row); } }
  chk("filter row", PDNA_FILT_TEXT_X, PDNA_FILT_ROW_W,
      (int)strlen(PDNA_FILT_MARKALL) * SYS8_W, PDNA_FILT_MARKALL);
  /* A type row paints type_chip() at PDNA_FILT_TEXT_X and the filter name a fixed
   * PDNA_FILT_CHIP_DX along, so the chip has to stop before the name starts. */
  chkv("type chip stops before the filter name", PDNA_FILT_CHIP_W, PDNA_FILT_CHIP_DX);
  /* ...and the VERTICAL half, which was missing and is what let a real defect ship: only
   * the chip's WIDTH was ever pinned, so a chip drawn 9 px from `y` (rows -1..7 of the
   * NEXT row's slot) against a bar of -1..7 read as covered. Under partial repaint every
   * row wipes its own rect, so the row below deleted the chip's last scanline and chips
   * came out 8 px tall everywhere but the bottom of the window. The chip must live
   * entirely inside the rect its own row's background wipe covers -- invariant (1) of
   * pdna_pick.c's ROW REPAINT RULE. */
  chkv_min("type chip top is not above the row bar", PDNA_FILT_CHIP_DY, PDNA_FILT_BAR_DY);
  chkv("type chip bottom is not below the row bar",
       PDNA_FILT_CHIP_DY + PDNA_FILT_CHIP_H, PDNA_FILT_BAR_DY + PDNA_FILT_BAR_H);
  /* The FILT_* bar against the next row's BAR (not its ink, which the check above
   * covers): 9 on a 9 px pitch, so the rects are disjoint and this geometry needs no
   * paint-order arbitration at all. It is the only list geometry in pdna_pick.c that
   * can say that. */
  chkv("filter bar clear of the next row's BAR",
       PDNA_FILT_BAR_DY + PDNA_FILT_BAR_H - 1, PDNA_FILT_ROW_H + PDNA_FILT_BAR_DY - 1);
  /* both lists' footers, drawn at x=4 on PDNA_FILT_FOOTER_Y */
  T(PDNA_FILT_FOOT, 4);
  T(PDNA_IFILT_FOOT, 4);

  printf("\n== move picker window (source/pdna_pick.c pick_move/mv_row) ==\n");
  /* mv_row (BACKLOG #36 item 7) now paints on the SAME PDNA_FILT_Y0/ROW_H/BAR_* geometry
   * as fm_row/dxm_row above, so the bar-covers-glyph-box / chip-vs-bar checks already
   * above (they are generic over PDNA_FILT_BAR_ and PDNA_FILT_CHIP_, not tied to a
   * specific caller) now cover mv_row too -- that IS the assertion tying mv_row's pitch
   * to the chip geometry: mv_row referencing the shared names by construction (not a
   * coincidentally-equal literal) is what makes them apply, so no separate numeric check
   * was added here.
   *
   * A DIFFERENT boundary was investigated and is RESIDUAL, stated rather than hidden
   * (pdna_pick.c's ROW REPAINT RULE names the same class): PDNA_MV_VIS rows at
   * PDNA_FILT_ROW_H pitch put row 8's ink at y=86..93 (PDNA_FILT_Y0 + 8*9 .. +8-1),
   * 2 px INTO PDNA_MV_DETAIL_Y=92's panel rect. This is not a bug -- pick_move's own
   * `full`/`mid != prev_mid` gating means the detail panel redraws in the SAME pass as
   * every row-8 draw (any `top` change forces `full`; any `sel` change changes `mid`,
   * since the list holds no duplicate move ids), so the panel's opaque fill always paints
   * over those 2 scanlines after mv_row does. A chkv here would need a hand-tuned "+2 px
   * is fine, more is not" fudge factor to pass without asserting the real invariant (paint
   * order, which a static geometry check cannot see) -- worse than not adding one. */

  /* GAP, stated rather than faked: that the bar is ui_fill_rect and NOT ui_panel is a
   * property of filt_bar()'s body, and no header can carry it. The two invariants above
   * are what a bordered bar would violate (a ui_panel of height 9 puts its rule at
   * y+7, failing check (2) by one pixel), so a regression to ui_panel with the same
   * height is caught indirectly, but a bordered bar of some other height is not. */

  /* The ITEM filter list keeps a BORDERED box, so the geometry is different: ui_panel's
   * bottom rule sits at y + BOX_DY + BOX_H - 2 and its fill ends one row lower. */
  chkv("item filter rule clear of the glyph box",
       UI_ROW_H - 1, PDNA_IFILT_BOX_DY + PDNA_IFILT_BOX_H - 2 - 1);
  chkv("item filter fill clear of the next row's ink",
       PDNA_IFILT_BOX_DY + PDNA_IFILT_BOX_H - 1, PDNA_IFILT_ROW_H - 1);
  /* The check above compares the box against the next row's INK and passes with room to
   * spare, which is why it never noticed that the box overlaps the next row's BOX. It
   * does, by exactly one scanline (rows -2..9 against a next-row top of ROW_H + BOX_DY =
   * 9), and under partial repaint that scanline belongs to whichever of the two rows
   * painted second -- pdna_pick.c's ROW REPAINT RULE makes it always the selected one.
   * Two things have to stay true for that rule to be enough, and neither was pinned:
   *   (a) the overlap is at most ONE scanline, so painting the selected row last is a
   *       complete fix rather than a partial one;
   *   (b) the shared scanline is at or below the glyph box, so arbitrating it costs no
   *       TEXT -- this is the property the met/nature list's 9-on-8 panel does NOT have
   *       (see the GAP note in the met-location section below). */
  chkv("item filter box overlaps the next row's BOX by at most one scanline",
       PDNA_IFILT_BOX_DY + PDNA_IFILT_BOX_H - 1, PDNA_IFILT_ROW_H + PDNA_IFILT_BOX_DY);
  chkv_min("item filter shared scanline is clear of the glyph box",
           PDNA_IFILT_ROW_H + PDNA_IFILT_BOX_DY, UI_ROW_H);
  chkv("item filter last row ink",
       PDNA_IFILT_Y0 + (PDNA_IFILT_ROWS - 1) * PDNA_IFILT_ROW_H + UI_ROW_H - 1,
       UI_FOOTER_Y - 1);

  printf("\n== rebalanced sys8 lines (settings + rumble) ==\n");
  /* TTE does NOT clip at the right margin, it WRAPS: "Yard visitors:  Needs your ROM"
   * is 30 columns at x=10, and the two that did not fit reappeared as "OM" at x=0 on
   * top of the row below. So these are checked with two pixels of slack — a line that
   * lands exactly on column 239 is one glyph from corrupting its neighbour. (sys8 ink
   * stops at column 6 of its 8 px cell, so a string that ends exactly at 240 is not
   * itself truncated — see the note at the end.)
   *
   * The rows are siprintf'd, so the test formats the SAME format string with the SAME
   * value strings the screen can put in it, and tries every one. */
#define MARGIN_R (SCR_W - 2)
  { char row[64];
#define SET_MODE_ONE(s) s,
    static const char* const MODES[] = { PDNA_SET_BACKUP_MODES(SET_MODE_ONE) };
    for (unsigned i = 0; i < sizeof MODES / sizeof MODES[0]; i++) {
      snprintf(row, sizeof row, PDNA_SET_BACKUP_FMT, MODES[i]);
      chk("settings row", PDNA_SET_ROW_X, MARGIN_R - PDNA_SET_ROW_X,
          (int)strlen(row) * SYS8_W, row); }
#define SET_YARD_ONE(s) s,
    static const char* const YARD[] = { PDNA_SET_YARD_VALUES(SET_YARD_ONE) };
    for (unsigned i = 0; i < sizeof YARD / sizeof YARD[0]; i++) {
      snprintf(row, sizeof row, PDNA_SET_YARD_FMT, YARD[i]);
      chk("settings row", PDNA_SET_ROW_X, MARGIN_R - PDNA_SET_ROW_X,
          (int)strlen(row) * SYS8_W, row); }
#define SET_ART_ONE(s) s,
    static const char* const ART[] = { PDNA_SET_ART_VALUES(SET_ART_ONE) };
    for (unsigned i = 0; i < sizeof ART / sizeof ART[0]; i++) {
      snprintf(row, sizeof row, PDNA_SET_ART_FMT, ART[i]);
      chk("settings row", PDNA_SET_ROW_X, MARGIN_R - PDNA_SET_ROW_X,
          (int)strlen(row) * SYS8_W, row); }
#define SET_ROM_ONE(s) s,
    static const char* const ROMV[] = { PDNA_SET_ROM_VALUES(SET_ROM_ONE) };
    for (unsigned i = 0; i < sizeof ROMV / sizeof ROMV[0]; i++) {
      snprintf(row, sizeof row, PDNA_SET_ROM_FMT, ROMV[i]);
      chk("settings row", PDNA_SET_ROW_X, MARGIN_R - PDNA_SET_ROW_X,
          (int)strlen(row) * SYS8_W, row); }
    chk("settings row", PDNA_SET_ROW_X, MARGIN_R - PDNA_SET_ROW_X,
        (int)strlen(PDNA_SET_ART_GO) * SYS8_W, PDNA_SET_ART_GO);
    snprintf(row, sizeof row, PDNA_SET_ART_CACHED_FMT, (unsigned long)PDNA_SET_ART_CACHED_MAXKB);
    chk("settings row", PDNA_SET_ROW_X, MARGIN_R - PDNA_SET_ROW_X,
        (int)strlen(row) * SYS8_W, row); }
  chk("settings row", PDNA_SET_ROW_X, MARGIN_R - PDNA_SET_ROW_X,
      (int)strlen(PDNA_SET_ROW_CLEAR) * SYS8_W, PDNA_SET_ROW_CLEAR);
  chk("settings help", PDNA_SET_HELP_X, MARGIN_R - PDNA_SET_HELP_X,
      (int)strlen(PDNA_SET_HELP1) * SYS8_W, PDNA_SET_HELP1);
  chk("settings help", PDNA_SET_HELP_X, MARGIN_R - PDNA_SET_HELP_X,
      (int)strlen(PDNA_SET_HELP2) * SYS8_W, PDNA_SET_HELP2);
  T(PDNA_SET_FOOT, PDNA_SET_FOOT_X);
  /* and the rows must not run into the help text or the footer */
  chkv("settings last row ink",
       PDNA_SET_ROW0_Y + (PDNA_SET_ROWS - 1) * PDNA_SET_ROW_PITCH + UI_ROW_H - 1,
       PDNA_SET_HELP_Y1 - 1);
  /* The SELECTION HIGHLIGHT PANEL is a separate rectangle from the text ink checked
   * just above -- ui_panel(2, y - PDNA_SET_ROW_PANEL_YOFF, 236, h, ...) expands (per
   * ui.c's ui_panel: fills y..y+h-1) to a taller band than the glyph row it frames.
   * The last row used the SAME height as every other row until this fix, whose panel
   * bottomed out at row 125 -- two rows INTO the help text at PDNA_SET_HELP_Y1 (124).
   * This is the check that would have caught it (a `PDNA_SET_ROW_PANEL_H` for the
   * last row's height, or a shrunk PDNA_SET_ROW_LASTPANEL_H that isn't short enough,
   * regresses this the same way). */
  chkv("settings last row panel clear of help",
       PDNA_SET_ROW0_Y + (PDNA_SET_ROWS - 1) * PDNA_SET_ROW_PITCH - PDNA_SET_ROW_PANEL_YOFF
         + PDNA_SET_ROW_LASTPANEL_H - 1,
       PDNA_SET_HELP_Y1 - 1);
  chkv("settings note last ink",
       PDNA_SET_NOTE_Y + UI_FONT_CELL_H - 1, PDNA_SET_FOOTER_Y - 1);
  /* Rumble page: the help sentence and the control hints used to share y=150 as
   * "GAME RTC on. <>adj A toggle B" — one full row, which is why the hint had to drop
   * its "back". Three separate rows now. */
  chk("rumble help", PDNA_RMB_HELP_X, MARGIN_R - PDNA_RMB_HELP_X,
      (int)strlen(PDNA_RMB_HELP1) * SYS8_W, PDNA_RMB_HELP1);
  chk("rumble help", PDNA_RMB_HELP_X, MARGIN_R - PDNA_RMB_HELP_X,
      (int)strlen(PDNA_RMB_HELP2) * SYS8_W, PDNA_RMB_HELP2);
  T(PDNA_RMB_FOOT, PDNA_SET_FOOT_X);
  /* and they must not collide vertically. The cue-row count is RCUE_COUNT (rmbl.h) —
   * adding a sixth haptic cue is exactly the change that would push the list into the
   * help text, so it is read from the enum rather than typed here. */
  chkv("rumble help row 2 last ink",
       PDNA_RMB_HELP_Y2 + UI_ROW_H - 1, PDNA_SET_FOOTER_Y - 1);
  chkv("rumble cue rows last ink",
       PDNA_RMB_ROW0_Y + (PDNA_RMB_STEPPERS + RCUE_COUNT - 1) * PDNA_RMB_ROW_PITCH
         + UI_ROW_H - 1,
       PDNA_RMB_HELP_Y1 - 1);

  printf("\n== day-care panel: descenders clear of the bottom border ==\n");
  /* ui_panel(x, y, w, h) fills y .. y+h-1 and frames it with m3_frame(.., y+h-1), whose
   * bottom rule lands on row y+h-2. Three ui_ptext rows at ROW0 + i*PITCH ink
   * UI_FONT_CELL_H rows each, so the last one must finish ABOVE that rule — this is the
   * check that catches the "Others are just visiting." descender shear. */
  chkv("day-care row 3 last ink row",
       PDNA_DCY_ROW0_Y + 2 * PDNA_DCY_ROW_PITCH + UI_FONT_CELL_H - 1,
       PDNA_DCY_PANEL_Y + PDNA_DCY_PANEL_H - 2 - 1);
  chkv("day-care panel bottom vs footer",
       PDNA_DCY_PANEL_Y + PDNA_DCY_PANEL_H - 1, PDNA_DCY_FOOTER_Y - 1);
  /* the single-boarder name is ui_ptext_fit'd, so it can only clip itself — but the
   * clamp still has to fit the panel */
  chkv("day-care name clamp inside the panel", PDNA_DCY_NAME_W, PDNA_DCY_TEXT_W);
  /* NOT a defect, verified on hardware-accurate pixels rather than by eye: sys8
   * glyph ink never passes column 6 of its 8 px cell (checked against libtonc's
   * sys8Glyphs), so "MOVE A grab hold=set" at x=80 — 20 columns ending exactly at
   * 240 — draws in full, its final 't' inked at x=234..238. It is pinned above
   * with the other box footers and left alone deliberately. */
  /* ==== END: popups vs the footer row ===================================== */

  /* ======================================================================== */
  /* ==== BEGIN: ROM IMAGE CHECK, the verdict band (source/pdna_romfull.c) == */
  /* ========================================================================
   * This screen's output IS a phone photo — it is what gets sent when a 12.5 MB image
   * hangs — so a clipped line here is not cosmetic, it is a lost diagnosis. Six of its
   * strings were over the 232 px field at REALISTIC values and one was caught rendering
   * as "...B checked (99.4%) b~" in mGBA, which turns "99.4% of PokeDNA's own image" into
   * "99.4% of the cartridge" — an overstatement of more than half.
   *
   * WORST CASE, NOT THE LUCKY ONE. Every substitution below is the widest value the
   * shipped code can put in that field, and each bound comes from pdna_romver.h rather
   * than from a number typed here:
   *   counts / region indices  PDNA_RV_MAX_REGIONS      (3 digits)
   *   byte counts, KiB, offsets, MB   PDNA_RV_MAX_IMAGE (10-char comma'd, 32768 KiB,
   *                                   a 7-hex-digit offset, "33.55" MB)
   *   percentage               100.0
   * Two fields have no structural clamp and are bounded by argument instead, called out
   * where they are used: the elapsed times (a scan that takes 999.9 s, a 256 KiB CRC that
   * takes 9999 ms — both an order of magnitude past anything measured) and the bus rung
   * (FCIO_NRUNGS is a single digit). */
  printf("\n== ROM IMAGE CHECK: the verdict band ==\n");
#define RVF_X PDNA_RVF_TEXT_X
#define RVF_W PDNA_RVF_TEXT_W
  { char b[96], nA[16], nB[16];
    const unsigned long MAXREG = PDNA_RV_MAX_REGIONS;
    const unsigned long MAXIMG = PDNA_RV_MAX_IMAGE;
    /* the last offset a region can START at: the smallest region the stamper may pick */
    const unsigned long OFF    = PDNA_RV_MAX_IMAGE - (1ul << PDNA_RV_MIN_RSHIFT);
    commas(nA, MAXIMG);
    commas(nB, MAXIMG);

    /* --- before the scan --------------------------------------------------- */
    /* Two shapes reach the same digit count: 256 regions of 128 KiB, and the coarsest
     * grid the stamper may emit (PDNA_RV_MAX_RSHIFT) over the largest image. */
    sprintf(b, PDNA_RVF_GEOM_FMT, nA, (int)MAXREG,
            (unsigned long)(MAXIMG / MAXREG) >> 10, 0xffffu, 9);
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_GEOM_FMT, nA, 2, (1ul << PDNA_RV_MAX_RSHIFT) >> 10, 0xffffu, 9);
    PF(b, RVF_X, RVF_W);
    /* The long legend is chosen at RUNTIME by ui_ptext_w, so IT is allowed not to fit —
     * the SHORT one is the fallback and nothing measures it at runtime, so it is the one
     * that must fit here. (A fallback that also clips leaves no legend at all.) */
    PF(PDNA_RVF_LEGEND_ALT, RVF_X, RVF_W);

    /* --- the header, rewritten when the scan ends --------------------------- */
    /* THE ONE CONFIRMED BY A RENDER. The " base only" qualifier is fused onto this line
     * on purpose: separated from the number it qualifies, the number is simply wrong. */
    sprintf(b, PDNA_RVF_COVER_FMT, nA, nB, 100ul, 0ul, PDNA_RVF_COVER_BASE);
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_COVER_FMT, nA, nB, 100ul, 0ul, "");
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_SCAN_FMT, 999ul, 9ul, 256ul, 9999ul, 9999ul);
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_SCAN2_FMT, 999ul, 9ul, 0xffffu);
    PF(b, RVF_X, RVF_W);

    /* --- row 1: every verdict, at every count maxed ------------------------- */
    sprintf(b, PDNA_RVF_V_FAULT, (int)MAXREG, (int)MAXREG, (int)MAXREG);
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_V_DESC, (int)MAXREG);           PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_V_STOPPED, (int)MAXREG, (int)MAXREG);  PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_V_NOTHING, (int)MAXREG);        PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_V_SHORT, MAXIMG >> 10, MAXIMG >> 10);  PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_V_OK_PART, (int)MAXREG, (int)MAXREG, (int)MAXREG);
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_V_OK, (int)MAXREG, (int)MAXREG); PF(b, RVF_X, RVF_W);

    /* --- row 2: the evidence ----------------------------------------------- */
    /* BOTH endings, because the ending is the diagnosis: "unstable" is the longer one and
     * it is the one that says BUS fault (re-time) rather than IMAGE fault (re-copy). */
    sprintf(b, PDNA_RVF_BAD1_FMT, (int)MAXREG, OFF, OFF / 1000000ul,
            (OFF / 10000ul) % 100ul, PDNA_RVF_BAD1_UNSTABLE);
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_BAD1_FMT, (int)MAXREG, OFF, OFF / 1000000ul,
            (OFF / 10000ul) % 100ul, PDNA_RVF_BAD1_STABLE);
    PF(b, RVF_X, RVF_W);
    PF(PDNA_RVF_DESC_NOTE,    RVF_X, RVF_W);
    PF(PDNA_RVF_NOTHING_NOTE, RVF_X, RVF_W);
    PF(PDNA_RVF_SHORT_NOTE,   RVF_X, RVF_W);
    PF(PDNA_RVF_CLEAN_NOTE,   RVF_X, RVF_W);
    /* clipped at EVERY count, not just the big ones — so check the small one too */
    sprintf(b, PDNA_RVF_RETRY_FMT, (int)MAXREG);        PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_RETRY_FMT, 1);                  PF(b, RVF_X, RVF_W);

    /* --- row 3: the remedy -------------------------------------------------- */
    PF(PDNA_RVF_FIX_OMEGA, RVF_X, RVF_W);
    PF(PDNA_RVF_FIX_OTHER, RVF_X, RVF_W);
    PF(PDNA_RVF_BACK,      RVF_X, RVF_W);

    /* --- row 4: the blind range, on every verdict ---------------------------- */
    /* This branch only fires when the zone IS the tail guard, so its KiB field is
     * PDNA_RV_TAIL_GUARD and nothing wider. */
    sprintf(b, PDNA_RVF_BLIND_TAIL, OFF, (unsigned long)(PDNA_RV_TAIL_GUARD >> 10));
    PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_BLIND_MORE, OFF, MAXIMG >> 10); PF(b, RVF_X, RVF_W);
    sprintf(b, PDNA_RVF_BLIND_ZONES, MAXIMG >> 10);     PF(b, RVF_X, RVF_W); }

  /* ...and the band's geometry, so the field the strings are measured against is the one
   * the screen actually draws with. */
  chkv("verdict band ink stays on the screen",
       PDNA_RVF_TEXT_X + PDNA_RVF_TEXT_W, UI_SCR_W);
  chkv_min("verdict band starts below the grid's bottom rule",
           PDNA_RVF_ROW1_Y, PDNA_RVF_GRID_BOT + 2);
  chkv("verdict row 1 clear of row 2", PDNA_RVF_ROW1_Y + UI_FONT_CELL_H - 1,
       PDNA_RVF_ROW2_Y - 1);
  chkv("verdict row 2 clear of row 3", PDNA_RVF_ROW2_Y + UI_FONT_CELL_H - 1,
       PDNA_RVF_ROW3_Y - 1);
  chkv("verdict row 3 clear of row 4", PDNA_RVF_ROW3_Y + UI_FONT_CELL_H - 1,
       PDNA_RVF_ROW4_Y - 1);
  chkv("verdict row 4 last ink row is on screen",
       PDNA_RVF_ROW4_Y + UI_FONT_CELL_H - 1, UI_SCR_H - 1);
  chkv("header row 1 clear of header row 2",
       PDNA_RVF_HDR_Y1 + UI_FONT_CELL_H - 1, PDNA_RVF_HDR_Y2 - 1);
  /* ==== END: ROM IMAGE CHECK ============================================== */

  /* ==== BEGIN: BALL picker =====================================================
   * The ball field used to be a 12-press toggle; it is a list of 24 px item icons now.
   * The names inside a row are table data the host cannot see and are drawn through
   * ui_ptext_fit, which clips VISIBLY with a '~' — the chrome around them is what
   * silently overruns, so the chrome is what is checked. */
  printf("\n== ball picker ==\n");
  {
    /* --- ball picker ------------------------------------------------------- */
    { char b[48];
      snprintf(b, sizeof b, PDNA_BALL_TITLE_FMT, PDNA_BALL_TITLE, 12, 12);
      T(b, 4); }                                        /* header, sys8 at x=4 */
    T(PDNA_BALL_FOOT, 4);                               /* footer, sys8 at x=4 */
    P(PDNA_BALL_NOTE, 4);                               /* note, proportional  */
    /* the row has to clear a 24 px item icon, and the last row has to stop above the
     * note — the note used to sit at 140 and cut straight through row 6's blurb. */
    chkv_min("ball row pitch clears the 24 px item icon", PDNA_BALL_ROW_H - 2, 24);
    chkv_min("ball blurb sits below the ball name",
             PDNA_BALL_DESC_DY, PDNA_BALL_NAME_DY + UI_FONT_CELL_H);
    chkv("ball blurb stays inside its row",
         PDNA_BALL_DESC_DY + UI_FONT_CELL_H - 1, PDNA_BALL_ROW_H - 2);
    chkv("last ball row stops above the note",
         PDNA_BALL_Y0 + (PDNA_BALL_VIS - 1) * PDNA_BALL_ROW_H + PDNA_BALL_ROW_H - 2,
         PDNA_BALL_NOTE_Y - 1);
    chkv("ball note stops above its rule",
         PDNA_BALL_NOTE_Y + UI_FONT_CELL_H - 1, PDNA_BALL_RULE_Y - 1);
    chkv("ball rule stops above the footer", PDNA_BALL_RULE_Y, PDNA_FILT_FOOTER_Y - 1);
    chkv_min("ball name column starts clear of the icon",
             PDNA_BALL_TEXT_X, PDNA_BALL_ICON_X + 24);

  }
  /* ==== END: BALL picker ======================================================= */

  /* ==== BEGIN: MET-LOCATION list + REGION chooser ==============================
   * Both are lists over gen3_places.c now. Place names are table data the host cannot
   * see and are ui_truncate'd (which clips with a visible '~'); what is measured here is
   * the chrome, the column budgets those truncations are handed, and the row geometry. */
  printf("\n== met-location + region pickers ==\n");
  {
    /* --- met-location list -------------------------------------------------- */
    T(PDNA_LOC_FOOT, 4);
    P(PDNA_LOC_EMPTY, PDNA_LOC_TEXT_X);
    /* A row is ui_truncate'd to PDNA_LOC_ROW_COLS and the header to PDNA_LOC_HDR_COLS;
     * ui_truncate only guarantees the COLUMN count, so the columns must fit the screen. */
    chkv("location row's column budget fits the screen",
         PDNA_LOC_TEXT_X + PDNA_LOC_ROW_COLS * SYS8_W, SCR_W);
    chkv("location header's column budget fits the screen",
         4 + PDNA_LOC_HDR_COLS * SYS8_W, SCR_W);
    /* The header used to spell the region, game and sort out in full — 41 columns, which
     * ui_truncate cut to "MET All regions/Emerald No. ~", losing the count AND the sort.
     * It uses short tags now, and EVERY combination is formatted and measured here, so a
     * new region or a longer tag fails the build rather than eating the count. */
    { char b[64];
#define LOC_TAG_ONE(s) s,
      static const char* const RT[] = { PDNA_LOC_RGN_TAGS(LOC_TAG_ONE) };
      static const char* const GT[] = { PDNA_LOC_GAME_TAGS(LOC_TAG_ONE) };
      static const char* const ST[] = { PDNA_LOC_SORT_TAGS(LOC_TAG_ONE) };
      for (unsigned r = 0; r < sizeof RT / sizeof RT[0]; r++)
        for (unsigned g = 0; g < sizeof GT / sizeof GT[0]; g++)
          for (unsigned t = 0; t < sizeof ST / sizeof ST[0]; t++) {
            snprintf(b, sizeof b, PDNA_LOC_HDR_FMT, RT[r], GT[g], ST[t], 217);
            chk("met header", 4, PDNA_LOC_HDR_COLS * SYS8_W, (int)strlen(b) * SYS8_W, b);
          } }
    chkv("last location row stops above the rule",
         PDNA_LOC_Y0 + (PDNA_LOC_VIS - 1) * PDNA_LOC_ROW_H + UI_ROW_H - 1,
         PDNA_LOC_RULE_Y - 1);
    chkv("location rule stops above the footer", PDNA_LOC_RULE_Y, PDNA_FILT_FOOTER_Y - 1);
    chkv_min("location row pitch holds a glyph box", PDNA_LOC_ROW_H, UI_ROW_H);
    /* The selection panel, against the next row's PANEL. Same shape as the item filter's
     * box -- one scanline of overlap, arbitrated by pdna_pick.c's ROW REPAINT RULE
     * (selected row painted last on every path, so the panel is 9 rows tall whether you
     * opened the screen onto this row or walked the cursor to it). */
    chkv("location panel overlaps the next row's PANEL by at most one scanline",
         PDNA_LOC_SEL_DY + PDNA_LOC_SEL_H - 1, PDNA_LOC_ROW_H + PDNA_LOC_SEL_DY);
    /* GAP, stated rather than faked: the item filter's twin of this check
     * (chkv_min(shared scanline, UI_ROW_H)) CANNOT be asserted here. ROW_H + SEL_DY = 7
     * and the glyph box is 8 rows, so the shared scanline IS glyph row 7 -- which sys8
     * inks for , ; g j p q y. An unselected row's wipe therefore clips the descenders of
     * the row above it. Harmless for what these screens can display (s_location[] and
     * s_nature[] are upper-case) and cheap to keep true, but list_pick is generic on
     * name_fn: giving it a mixed-case list means widening the pitch to 9 the way the
     * FILT_* lists did (costing a visible row) or dropping the border for a borderless
     * bar. Asserting the invariant here would fail today and asserting the weaker one
     * would read as coverage it is not, so it is written down instead. */

    /* --- the met filter menu (item-filter geometry, one more row) ----------- */
    T(PDNA_LFILT_TITLE, 4);
    T(PDNA_LFILT_ALL, PDNA_FILT_TEXT_X);
    { char b[48];
      /* every value the Game row can hold, formatted the way the screen formats it */
      static const char* const GAMES[] = { "All", "Ruby/Sapph", "Emerald", "FireRed/LG" };
      for (unsigned i = 0; i < sizeof GAMES / sizeof GAMES[0]; i++) {
        snprintf(b, sizeof b, PDNA_LFILT_GAME_FMT, GAMES[i]);
        chk("met filter game row", PDNA_FILT_TEXT_X, PDNA_FILT_ROW_W,
            (int)strlen(b) * SYS8_W, b);
      }
      /* a region row with the "currently selected" marker the screen appends */
      static const char* const RGNS[] = { "Hoenn", "Kanto", "Sevii Isles", "Special" };
      for (unsigned i = 0; i < sizeof RGNS / sizeof RGNS[0]; i++) {
        snprintf(b, sizeof b, "%s  <", RGNS[i]);
        chk("met filter region row", PDNA_FILT_TEXT_X, PDNA_FILT_ROW_W,
            (int)strlen(b) * SYS8_W, b);
      } }
    chkv("met filter's extra row still lands above the footer",
         PDNA_IFILT_Y0 + (PDNA_LFILT_ROWS - 1) * PDNA_IFILT_ROW_H + UI_ROW_H - 1,
         UI_FOOTER_Y - 1);

    /* --- region chooser ----------------------------------------------------- */
    T(PDNA_RGN_TITLE, 4);
    T(PDNA_RGN_FOOT, 4);
    P(PDNA_RGN_NOTE, 4);
    { char b[48];
      static const char* const RGNS[] = { "Hoenn", "Kanto", "Sevii Isles", "Special" };
      for (unsigned i = 0; i < sizeof RGNS / sizeof RGNS[0]; i++) {
        snprintf(b, sizeof b, PDNA_RGN_ROW_FMT, RGNS[i], 104);
        T(b, PDNA_RGN_TEXT_X);
      } }
    chkv("last region row stops above the note",
         PDNA_RGN_Y0 + (4 - 1) * PDNA_RGN_ROW_H + UI_ROW_H - 1, PDNA_RGN_NOTE_Y - 1);
    chkv("region note stops above its rule",
         PDNA_RGN_NOTE_Y + UI_FONT_CELL_H - 1, PDNA_RGN_RULE_Y);
    chkv("region rule stops above the footer", PDNA_RGN_RULE_Y, PDNA_FILT_FOOTER_Y - 1);

  }
  /* ==== END: MET-LOCATION list + REGION chooser ================================ */

  /* ==== BEGIN: the edit field list's new MAX PP rows ==========================
   * A move's maximum PP is derived from its PP Ups, so the field list grew a "Max PP n"
   * row per move and the summary's PP cell grew the Up count. Both are fixed strings the
   * host can measure; the geometry they live in comes from pdna_layout.h. */
  printf("\n== max-PP rows ==\n");
  {
    T(PDNA_EDIT_FOOT, 4);
    /* Labels are NOT truncated: an over-long one paints into the value column. */
    { char b[32];
      for (int n = 1; n <= 4; n++) {
        /* the same macro the FLABEL table uses, so this cannot drift from it */
        const char* lbl = (n == 1) ? PDNA_EDIT_MAXPP_LBL(1) : (n == 2) ? PDNA_EDIT_MAXPP_LBL(2)
                        : (n == 3) ? PDNA_EDIT_MAXPP_LBL(3) : PDNA_EDIT_MAXPP_LBL(4);
        chk("edit label", PDNA_EDIT_LBL_X, PDNA_EDIT_LBL_W, (int)strlen(lbl) * SYS8_W, lbl);
      }
      chk("edit label", PDNA_EDIT_LBL_X, PDNA_EDIT_LBL_W,
          (int)strlen(PDNA_EDIT_REGION_LBL) * SYS8_W, PDNA_EDIT_REGION_LBL);
      /* the max-PP value at its widest reachable state: 40 base + 3 Ups = 64 */
      snprintf(b, sizeof b, PDNA_EDIT_MAXPP_FMT, 64u, 3u);
      chk("edit max-PP value", PDNA_EDIT_VAL_X, PDNA_EDIT_VAL_COLS * SYS8_W,
          (int)strlen(b) * SYS8_W, b);
      /* the Region row's value is a region name, and the longest is "Sevii Isles" */
      chk("edit region value", PDNA_EDIT_VAL_X, PDNA_EDIT_VAL_COLS * SYS8_W,
          (int)strlen("Sevii Isles") * SYS8_W, "Sevii Isles");
    }
    chkv("edit value column's budget fits the screen",
         PDNA_EDIT_VAL_X + PDNA_EDIT_VAL_COLS * SYS8_W, SCR_W);

    /* --- the summary's PP cell, which now carries the PP-Up count ------------ */
    { char b[32];
      snprintf(b, sizeof b, PDNA_SUM_PP_UPS_FMT, 64u, 64u, 3u);
      PF(b, PDNA_SUM_CARD_X + PDNA_SUM_PP_X_DX, PDNA_SUM_PP_W);
      snprintf(b, sizeof b, PDNA_SUM_PP_FMT, 64u, 64u);
      PF(b, PDNA_SUM_CARD_X + PDNA_SUM_PP_X_DX, PDNA_SUM_PP_W); }
    chkv("summary PP cell ends inside the card",
         PDNA_SUM_PP_X_DX + PDNA_SUM_PP_W, PDNA_SUM_CARD_W);

    /* --- the ORIGIN / MET card's new Region row ------------------------------
     * The summary card IS the editor (pdna_edit's field list is not wired up), so this
     * row is the only place a Region is reachable. Adding it pushed the OT/TID/SID tail
     * down 11 px into territory nothing was watching. */
    chk("origin card key", PDNA_SUM_CARD_X, PDNA_SUM_ORG_VAL_DX,
        (int)strlen(PDNA_SUM_ORG_REGION_LBL) * SYS8_W, PDNA_SUM_ORG_REGION_LBL);
    PF("Sevii Isles", PDNA_SUM_CARD_X + PDNA_SUM_ORG_VAL_DX,
       PDNA_SUM_ORG_RIGHT - (PDNA_SUM_CARD_X + PDNA_SUM_ORG_VAL_DX));
    chkv("origin card's value column ends inside the right margin",
         PDNA_SUM_ORG_RIGHT, SCR_W);
    chkv("origin card's last row stays above the note row",
         PDNA_SUM_ORG_LAST_Y + UI_ROW_H - 1, PDNA_SUM_NOTE_Y - 1);
  }
  /* ==== END: the edit field list's new MAX PP rows ============================= */

  printf("\n== party screen: retail layout (app_party_overlay, #2026-08-19) ==\n");
  /* One big slot-1 box (name/level/gender/HP stacked one-per-line) plus five 142x24
   * list rows (name+HP bar on top, level+gender+HP numbers below) — see the block
   * comment above PDNA_PTY_BOX_X in pdna_layout.h for why the box is NOT the row
   * layout scaled down. Every check below reads the SAME constants party_draw_slot_fg
   * draws with (source/pdna_main.c). */
  {
    /* Every field this screen draws goes through ui_ptext_shadow/ui_ptext_fit_shadow
     * (source/ui.c) — the PROPORTIONAL 5x7 face, not sys8 — so every check below is
     * PF()/pwidth(), never T(). */
    PF(PDNA_PTY_MSG_CHOOSE, PDNA_PTY_MSG_X + PDNA_PTY_MSG_PAD, PDNA_PTY_MSG_W_BUDGET);
    PF(PDNA_PTY_MSG_PLACE,  PDNA_PTY_MSG_X + PDNA_PTY_MSG_PAD, PDNA_PTY_MSG_W_BUDGET);
    chkv("message box ends above the screen bottom",
         PDNA_PTY_MSG_Y + PDNA_PTY_MSG_H, UI_SCR_H);
    /* CANCEL button: reuses the shared PDNA_LBL_CANCEL, but against THIS screen's own
     * geometry — a new check even though the string itself is tested elsewhere. */
    PF(PDNA_LBL_CANCEL, PDNA_PTY_CANCEL_X + PDNA_PTY_MSG_PAD, PDNA_PTY_CANCEL_W_BUDGET);
    chkv("CANCEL button ends above the screen bottom",
         PDNA_PTY_CANCEL_Y + PDNA_PTY_CANCEL_H, UI_SCR_H);
    chkv("CANCEL button stays right of the message box",
         PDNA_PTY_MSG_X + PDNA_PTY_MSG_W, PDNA_PTY_CANCEL_X);
    chkv("CANCEL button ends on-screen",
         PDNA_PTY_CANCEL_X + PDNA_PTY_CANCEL_W, SCR_W);

    /* Row template (slots 2-6): name is fit-clamped to PDNA_PTY_NAME_W; level, the "HP"
     * label and the HP numbers are drawn UNCLAMPED (ui_ptext_shadow, not _fit_), so each
     * is measured at its worst case against the gap it actually has to clear.
     *
     * SALAMENCE/METAGROSS/etc used to clip here via ui_ptext_fit's own '~' marker
     * (2026-08-19 mechanical-fix pass), and that pass's "fix" (UI_PTEXT_TIGHT_DELTA=2,
     * tight kerning for the row name) was WORSE than the clip it replaced: it made
     * adjacent glyphs' ink genuinely overlap, turning "SALAMENCE"/"METAGROSS"/
     * "TYRANITAR" into unreadable smears — see
     * docs/analysis-2026-08-19-party/verify-2026-08-20-zoom.png, and
     * UI_PTEXT_TIGHT_DELTA's own comment in ui_font.h for exactly why delta=2 overlaps
     * and delta=1 does not (checked against all 96 glyphs' ink bitmaps, not just the
     * capitals). #2026-08-20 reverted party_draw_name_level's row branch to the DEFAULT
     * proportional face (no kerning at all) and widened PDNA_PTY_NAME_W to its true
     * measured maximum (49px) instead — safe, but it meant every real 9-glyph species
     * name (52-54px default) still clipped to e.g. "SALAMEN~", because 49px was never
     * wide enough for the default face's advance and there is no more room to widen it
     * (see PDNA_PTY_NAME_W's own comment in pdna_layout.h for why 51px is the true
     * geometric ceiling between the fixed name origin and the retail-measured HP label).
     *
     * #2026-08-21: now that delta=1 is proven to never overlap (the guard right below),
     * the row joins the box on the TIGHT face (ui_ptext_fit_shadow_tight,
     * source/pdna_main.c) instead of chasing a wider column that does not exist. A
     * complete name beats a clipped one, and every real 9-glyph species name fits at
     * delta=1 with real slack to spare — the checks below prove that with >=1px slack
     * required, then separately prove the residual: a genuine 10-glyph name still does
     * not fit even at delta=1 and is expected, by design, to clip by roughly a pixel —
     * that gap needs a narrower party typeface (a separate job), not more kerning
     * (delta stays capped at 1) or a moved column. */
    chkv("UI_PTEXT_TIGHT_DELTA never exceeds 1 (glyphs may touch, ink must never overlap)",
         UI_PTEXT_TIGHT_DELTA, 1);
#define PWT(s) pwidth_tight(s)
    /* The mathematical worst case for a 9-glyph name: nine of the font's widest letter
     * (6px default / 5px tight — every uppercase letter except 'I', which is narrower).
     * Synthetic on purpose, not a lucky real name — proves the >=1px-slack claim below
     * holds even at the font's own ceiling, not just for names that happen to contain a
     * narrow glyph. Non-vacuity: shrinking PDNA_PTY_NAME_W by 5px (to 44) turns this red
     * (45px tight name, 43px slack-adjusted limit) — checked by hand for this pass. */
    chkv("row name (synthetic 9-char all-widest-glyph, tight) clears its column with slack",
         PWT("AAAAAAAAA"), PDNA_PTY_NAME_W - 1);
    /* The real 9-glyph species names that motivated this whole investigation — including
     * the three that actually sit in this save's own party rows (SALAMENCE/METAGROSS/
     * DRAGONITE; see docs/analysis-2026-08-19-party/MEASUREMENTS.md). Every one now FITS
     * with real slack at delta=1 (43-45px tight against the 49px budget), rendering the
     * full name instead of "SALAMEN~"/"METAGRO~"/"DRAGONI~". */
    chkv("row name (SALAMENCE, tight) clears its column with slack",
         PWT("SALAMENCE"), PDNA_PTY_NAME_W - 1);
    chkv("row name (METAGROSS, tight) clears its column with slack",
         PWT("METAGROSS"), PDNA_PTY_NAME_W - 1);
    chkv("row name (NIDOQUEEN, tight) clears its column with slack",
         PWT("NIDOQUEEN"), PDNA_PTY_NAME_W - 1);
    chkv("row name (DRAGONITE, tight) clears its column with slack",
         PWT("DRAGONITE"), PDNA_PTY_NAME_W - 1);
    /* KNOWN LIMITATION, recorded on purpose rather than silently fixed or forgotten: a
     * genuine 10-glyph name does not fit this column even at delta=1. BELLSPROUT (no
     * narrow glyphs, the same 50px tight as the font's own worst-case 10-char string)
     * clips by 1px — chkv_min (must EXCEED, not fit) so that if PDNA_PTY_NAME_W or the
     * tight delta ever changes enough to fit it for real, this assertion goes red and
     * forces this comment to be revisited rather than quietly going stale. Retail fits a
     * true 10-character name here only because its own font is narrower per glyph than
     * PokeDNA's; closing this last ~1px needs a narrower party typeface, a separate job
     * from this pass — NOT a higher tight delta (capped at 1, see ui_font.h) or a moved
     * column (PDNA_PTY_NAME_DX/W are unchanged by this pass). */
    chkv_min("row name (BELLSPROUT, tight) exceeds its column and clips safely (no overlap)",
             PWT("BELLSPROUT"), PDNA_PTY_NAME_W + 1);
    chkv_min("row name (synthetic worst-case 10-char nickname, tight) exceeds its column",
             PWT("WWWWWWWWWW"), PDNA_PTY_NAME_W + 1);
    /* Not every 10-glyph string clips, though — worth recording honestly rather than
     * letting the BELLSPROUT case above read as "all 10-char names clip". WEEPINBELL
     * (also a real Gen-3 species name) contains a narrow 'I' that saves 2px versus an
     * all-wide-letter name of the same length, landing it at exactly 48px tight — 1px of
     * real slack inside the 49px budget, so it renders in full. */
    chkv("row name (WEEPINBELL, tight) — a 10-char name that happens to fit anyway",
         PWT("WEEPINBELL"), PDNA_PTY_NAME_W - 1);
#undef PWT
    { char b[16];
      sprintf(b, PDNA_PTY_LVL_FMT, 100u);                 /* "Lv100": worst-case level */
      PF(b, PDNA_PTY_ROW_X + PDNA_PTY_NAME_DX, PDNA_PTY_GEND_DX - PDNA_PTY_NAME_DX);
      sprintf(b, PDNA_PTY_HP_NUM_FMT, 714u, 714u);        /* worst REAL Gen-3 HP (Blissey,
                                                           * Lv100, max IV/EV) — "999/999"
                                                           * cannot occur but is the same
                                                           * digit count, so this is not a
                                                           * narrower case, just the honest one */
      PF(b, PDNA_PTY_ROW_X + PDNA_PTY_HP_NUM_DX, PDNA_PTY_HP_NUM_W); }
    PF("HP", PDNA_PTY_ROW_X + PDNA_PTY_HP_LBL_DX, PDNA_PTY_HP_BAR_DX - PDNA_PTY_HP_LBL_DX);
    chkv("row HP bar ends inside the row",
         PDNA_PTY_HP_BAR_DX + PDNA_PTY_HP_BAR_W, PDNA_PTY_ROW_W);
    chkv("row HP numbers end inside the row",
         PDNA_PTY_HP_NUM_DX + PDNA_PTY_HP_NUM_W, PDNA_PTY_ROW_W);
    /* >=2px of clear gap before the "HP" label's own ink (not just <=0, i.e. not just
     * "does not touch") — the margin PDNA_PTY_NAME_W's own comment in pdna_layout.h
     * derives its 49px value from. Mutation-checked by hand: bumping PDNA_PTY_NAME_W to
     * 50 (1px gap) turns this red. */
    chkv("row name column clears the HP label by >=2px",
         PDNA_PTY_NAME_DX + PDNA_PTY_NAME_W + 2, PDNA_PTY_HP_LBL_DX);
    chkv("row bottom stays on-screen (slot 6, the last row)",
         PDNA_PTY_ROW_Y0 + 5 * PDNA_PTY_ROW_H, PDNA_PTY_MSG_Y);

    /* TEST GAP CLOSED (2026-08-19 mechanical-fix pass): NOTHING here used to check a
     * text column against the ICON rect it sits beside — grep for MON_ICON_W or
     * *_ICON_DX over this file returned nothing before this block. That is exactly the
     * gap that let MUST-FIX 1 ship: PDNA_PTY_NAME_DX=3 put the row name column 18px on
     * top of its own icon, and nothing here would have gone red.
     *
     * VERIFIED this actually catches it: with PDNA_PTY_NAME_DX temporarily set back to
     * its old (wrong) value of 3, PDNA_PTY_ROW_ICON_DX + MON_ICON_W (22) is no longer
     * <= PDNA_PTY_NAME_DX (3) — the very check below — and turns red, mutated and
     * re-run by hand for this pass. Restored to 23 (the fix) before committing. */
    chkv("row name column clears the icon (MUST-FIX 1's own regression test)",
         PDNA_PTY_ROW_ICON_DX + MON_ICON_W, PDNA_PTY_NAME_DX);
    /* Box: NOT a zero-tolerance clearance like the row above. Retail's OWN box has this
     * same kind of small RECT overlap by MEASURED design (MEASUREMENTS.md's box
     * section: icon x≈5..35, name x≈32..83 — a 3px overlap that was never flagged as
     * wrong, unlike the row's name origin) — a real Gen-3 icon's opaque silhouette does
     * not fill its whole 32x32 bounding rect, so a few pixels of RECT overlap is not
     * necessarily a visible collision (confirmed directly: Tyranitar's own icon has no
     * opaque pixel in the overlapping columns at y=33, docs/analysis-2026-08-19-party/
     * new-party-idle-f00.png).
     *
     * #2026-08-20: CLOSED to retail's exact 3px. The 2026-08-19 pass left this at 5px
     * (2 more than retail) because tightening PDNA_PTY_BOX_NAME_DX to zero-overlap would
     * have shrunk the field below "TYRANITAR"'s own 52px width at the DEFAULT font
     * advance, where it fit with 0px of slack. Tight-spacing (see PWT() above) renders
     * the same string at 34px, so PDNA_PTY_BOX_NAME_DX moved 15->17 (pdna_layout.h) —
     * matching retail's 3px exactly — with 17px of slack to spare, not 0. Asserted exact,
     * not merely bounded, now that it is the measured value rather than a compromise. */
    chkv("box name column's icon-rect overlap matches retail's measured 3px (<=)",
         (PDNA_PTY_BOX_ICON_DX + MON_ICON_W) - PDNA_PTY_BOX_NAME_DX, 3);
    chkv_min("box name column's icon-rect overlap matches retail's measured 3px (>=)",
             (PDNA_PTY_BOX_ICON_DX + MON_ICON_W) - PDNA_PTY_BOX_NAME_DX, 3);
    /* "+ Add here" (the PLACE-mode empty-slot affordance, party_draw_slot_fg) is drawn
     * with plain ui_ptext — UNCLAMPED, unlike every other string on this screen — and
     * nothing measured it. It only ever lands on a ROW (a party carry can only ever
     * leave the box, slot 0, occupied — see app_party_overlay's addslot comment), and
     * an add-slot row draws NOTHING else (no icon, no HP fields), so the tighter
     * PDNA_PTY_NAME_W budget the occupied-row template above uses does not apply to it
     * — its real ceiling is the row's own right edge. */
    PF("+ Add here", PDNA_PTY_ROW_X + PDNA_PTY_NAME_DX, PDNA_PTY_ROW_W - PDNA_PTY_NAME_DX);

    /* Slot-1 box: same fields, but ONE per line (four stacked rows) — the box is only
     * 71 px wide, so its own HP bar is narrower than a row's (PDNA_PTY_BOX_HP_BAR_W).
     *
     * The box name is the ONE place left that draws through ui_ptext_fit_shadow_tight
     * (party_draw_name_level, source/pdna_main.c — the row branch reverted to the
     * default face above; only the box branch still uses tight). So it is measured with
     * pwidth_tight/PWT here, not the default-face PF/pwidth, using
     * UI_PTEXT_TIGHT_DELTA=1 (capped — see ui_font.h): "TYRANITAR" renders 43px at
     * delta=1 (was wrongly reported as 34px when this comment was written against the
     * illegible delta=2 build; 43 is the real, checked number), 9px of genuine slack
     * inside the 52px budget. Every glyph pair in the font is proven to touch-at-worst,
     * never overlap, at delta=1 (see ui_font.h's own 96-glyph check). */
#define PWT(s) pwidth_tight(s)
    chk("ptext_fit_tight", PDNA_PTY_BOX_X + PDNA_PTY_BOX_NAME_DX, PDNA_PTY_BOX_NAME_W,
        PWT("TYRANITAR"), "TYRANITAR");
    /* BELLSPROUT/the synthetic 10-char case below are the tightest fits in the box (2px
     * slack at delta=1) — mutation-checked by hand: setting UI_PTEXT_TIGHT_DELTA back to
     * 2 turns every chkv in this box block red (each PWT() value shrinks further below
     * budget, which chkv's <= comparison still passes... except that delta=2 is exactly
     * the illegible-overlap bug this file's own UI_PTEXT_TIGHT_DELTA<=1 check (above,
     * in the row block) catches first — that is the actual regression guard, not these
     * per-name slack numbers, which stay numerically "green" either way since a smaller
     * delta only ever shrinks PWT()'s output. */
    chkv("box name (BELLSPROUT, tight) clears its column with slack",
         PWT("BELLSPROUT"), PDNA_PTY_BOX_NAME_W - 1);
    chkv("box name (NIDOQUEEN, tight) clears its column with slack",
         PWT("NIDOQUEEN"), PDNA_PTY_BOX_NAME_W - 1);
    chkv("box name (SALAMENCE, tight) clears its column with slack",
         PWT("SALAMENCE"), PDNA_PTY_BOX_NAME_W - 1);
    chkv("box name (METAGROSS, tight) clears its column with slack",
         PWT("METAGROSS"), PDNA_PTY_BOX_NAME_W - 1);
    chkv("box name (synthetic worst-case 10-char nickname, tight) clears its column",
         PWT("WWWWWWWWWW"), PDNA_PTY_BOX_NAME_W - 1);
    { char b[16];
      sprintf(b, PDNA_PTY_LVL_FMT, 100u);
      PF(b, PDNA_PTY_BOX_X + PDNA_PTY_BOX_NAME_DX, PDNA_PTY_BOX_GEND_DX - PDNA_PTY_BOX_NAME_DX);
      sprintf(b, PDNA_PTY_HP_NUM_FMT, 714u, 714u);
      PF(b, PDNA_PTY_BOX_X + PDNA_PTY_BOX_HP_NUM_DX, PDNA_PTY_BOX_HP_NUM_W); }
    PF("HP", PDNA_PTY_BOX_X + PDNA_PTY_BOX_HP_LBL_DX,
       PDNA_PTY_BOX_HP_BAR_DX - PDNA_PTY_BOX_HP_LBL_DX);
    chkv("box HP bar ends inside the box",
         PDNA_PTY_BOX_HP_BAR_DX + PDNA_PTY_BOX_HP_BAR_W, PDNA_PTY_BOX_W);
    chkv("box HP numbers end inside the box",
         PDNA_PTY_BOX_HP_NUM_DX + PDNA_PTY_BOX_HP_NUM_W, PDNA_PTY_BOX_W);
    chkv("box gender glyph stays inside the box (9 px glyph)",
         PDNA_PTY_BOX_GEND_DX + 9, PDNA_PTY_BOX_W);
    chkv("box's last text line stays inside the box (7 px glyph row)",
         PDNA_PTY_BOX_HP_NUM_DY + 7, PDNA_PTY_BOX_H);
#undef PWT
  }

  printf("\n== PC-box party PANEL: retail-measured rebuild (#2026-08-20) ==\n");
  /* The only fixed string this panel draws is CANCEL (party_strip_overlay's
   * pcp_draw_cancel, source/pdna_box.c) -- reuses the shared PDNA_LBL_CANCEL, but at a
   * NEW geometry (the pill snug in the panel's own bottom-right corner), so per this
   * file's own convention this is a new check even though the string itself is tested
   * elsewhere (the standalone party-overlay's own CANCEL, above). Drawn with
   * ui_ptext_tight (delta=1) directly against the pill's 1px-bordered interior width,
   * with zero extra padding -- CANCEL must fit EXACTLY, not just under a budget with
   * slack, or the pill was sized wrong. */
  {
#define PWT(s) pwidth_tight(s)
    int interior = (PDNA_PCP_CANCEL_X1 - PDNA_PCP_CANCEL_X0 + 1) - 2;   /* 1px border/side */
    chkv("PDNA_PCP_CANCEL_W_BUDGET matches the pill's own 1px-bordered interior",
         PDNA_PCP_CANCEL_W_BUDGET, interior);
    chkv_min("PDNA_PCP_CANCEL_W_BUDGET matches the pill's own 1px-bordered interior",
             PDNA_PCP_CANCEL_W_BUDGET, interior);
    chkv("CANCEL (tight) fills the pill's interior exactly, no overflow",
         PWT(PDNA_LBL_CANCEL), PDNA_PCP_CANCEL_W_BUDGET);
#undef PWT
  }
  /* Geometry sanity: every element this rebuild introduced stays on the 240x160 native
   * screen and inside the panel's own outer bevel -- a wrong sign on any offset added
   * during the rebuild would silently draw off-screen or outside the panel, which no
   * build-time check previously existed to catch (this whole panel was un-pinned). */
  chkv("panel's own outer bbox stays on-screen (x)",
       PDNA_PCP_PANEL_X1 + 1, SCR_W);
  chkv("panel's own outer bbox stays on-screen (y)",
       PDNA_PCP_PANEL_Y1 + 1, UI_SCR_H);
  chkv("slot 1 (offset tile) stays inside the panel's left inner fill",
       PDNA_PCP_FILL_X0, PDNA_PCP_S1_X0);
  chkv("column's right edge stays inside the panel's right inner fill",
       PDNA_PCP_COL_X1, PDNA_PCP_FILL_X1);
  chkv("slot 1 is vertically centred on the column (matches tile #3, the middle one)",
       PDNA_PCP_S1_Y0, PDNA_PCP_COL_Y0 + 2 * PDNA_PCP_SLOT_H);
  chkv_min("slot 1 is vertically centred on the column (matches tile #3, the middle one)",
           PDNA_PCP_S1_Y0, PDNA_PCP_COL_Y0 + 2 * PDNA_PCP_SLOT_H);
  chkv("the 5th (last) column tile stays inside the panel's fill",
       PDNA_PCP_COL_Y0 + 4 * PDNA_PCP_SLOT_H + PDNA_PCP_SLOT_VISH - 1, PDNA_PCP_FILL_Y1);
  chkv("CANCEL pill stays inside the panel's outer bbox (x)",
       PDNA_PCP_CANCEL_X1, PDNA_PCP_PANEL_X1);
  chkv("CANCEL pill stays inside the panel's outer bbox (y)",
       PDNA_PCP_CANCEL_Y1, PDNA_PCP_PANEL_Y1);
  chkv("CANCEL pill sits below the column's last tile (no overlap)",
       PDNA_PCP_COL_Y0 + 4 * PDNA_PCP_SLOT_H + PDNA_PCP_SLOT_VISH - 1, PDNA_PCP_CANCEL_Y0);

  /* SHOULD-FIX 6 (2026-08-20 review): party_strip_overlay's own footer replaces the
   * box's own (partially-overpainted, garbled) footer string with this one, drawn at
   * PDNA_PCP_OCCLUDE_X1 + 2 — the ONLY screen space free of both the panel's own
   * bevel (which covers the footer row for PDNA_PCP_OCCLUDE_X0..X1) and the left PKMN
   * DATA panel. Budget is the real free width to the screen's right edge, not a
   * guess. */
  PF(PDNA_LBL_PCP_FOOTER, PDNA_PCP_OCCLUDE_X1 + 2, SCR_W - (PDNA_PCP_OCCLUDE_X1 + 2));

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
