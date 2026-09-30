/*
 * Battle Frontier win streaks — viewer + editor.
 *
 * Emerald: a scrolling list of every streak lane (7 facilities x their battle
 * modes x Lv50/Open), showing CURRENT / BEST and whether the lane's
 * winStreakActiveFlags bit is set, plus the records-board counters that sit
 * alongside them (Dome championships, Factory rentals, Pike total clears, BP).
 * Ruby/Sapphire: the much smaller Battle Tower record pair.
 *
 * Two behaviours here exist because of the game, not because of taste:
 *   - editing a CURRENT streak always sets that lane's active bit (g3f_set_current),
 *     because every facility's challenge-init zeroes a streak whose bit is clear;
 *   - the presets exist because the Frontier Brain test is `==`, not `>=`, so a
 *     hand-typed 9999 permanently skips the symbol. "Meet <Brain>" writes the exact
 *     value the game wants.
 *
 * All values are plaintext in SaveBlock2 — no XOR, no sub-checksum — so the edit is
 * an in-place poke of g_sb2 plus one app_commit_sb2().
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_frontier.h"
#include "gen3_frontier.h"
#include "gen3_flags.h"   /* frontier symbol flags (SaveBlock1) */
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "rmbl.h"          /* rmbl_fire (haptic cues) */
#include "sys.h"           /* EWRAM_BSS (after tonc.h) */
#include "pdna_app.h"

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A)      snd_ok();
  else if (k & KEY_B)      snd_back();
  return k;
}

/* Small framed message box (A or B dismisses). The panel is 216 px wide with text
 * at x=20, so body lines must stay <= 25 columns at the 8x8 fixed font. */
static void s_msg(const char* title, u16 ink, const char* l1, const char* l2) {
  ui_panel(12, 50, 216, l2 ? 60 : 50, UI_PANEL, UI_BORDER);
  ui_text(20, 58, ink, title);
  ui_hline(16, 70, 208, UI_BORDER);
  if (l1) ui_text(20, 76, UI_TEXT, l1);
  if (l2) ui_text(20, 86, UI_TEXT, l2);
  ui_text(20, l2 ? 96 : 86, UI_DIM, "A ok");
  s_wait(KEY_A | KEY_B);
}

/* Numeric entry via the OSK.
 * Returns true only when the user actually COMMITTED a number. Cancelling must be
 * distinguishable from confirming the current value: the callers set the lane's
 * winStreakActiveFlags bit and raise the record as a side effect of writing, so a
 * cancel that looked like "confirmed the same number" would still mutate the save
 * and mark it dirty. */
static bool num_entry(const char* prompt, int cur, int maxv, int* out_val) {
  char init[12], out[12];
  siprintf(init, "%d", cur);
  if (!osk_search(prompt, init, out, sizeof out)) return false;   /* cancelled */
  if (out[0] < '0' || out[0] > '9') return false;                 /* empty/garbage */
  int v = 0;
  for (const char* p = out; *p >= '0' && *p <= '9'; p++) {
    v = v * 10 + (*p - '0');
    if (v > maxv) { v = maxv; break; }
  }
  *out_val = v;
  return true;
}

/* ---- row model -------------------------------------------------------------
 * The list is flattened once: facility headers, each facility's lanes, then a
 * trailing "counters" section. Rows are generated rather than stored so adding a
 * facility can't desync a parallel array. */
enum { ROW_HDR, ROW_LANE, ROW_COUNTER };
typedef struct {
  uint8_t kind;
  uint8_t fac, mode, lvl;     /* ROW_LANE */
  uint16_t off; uint16_t cap; /* ROW_COUNTER */
  const char* label;
} FrRow;

#define FR_MAX_ROWS 64

/* Counters that share the records boards with the streaks. Index order matches
 * the facilities they belong to so they read in a sensible place. */
static int build_rows(FrRow* r) {
  int n = 0;
  for (int f = 0; f < G3F_FACILITIES && n < FR_MAX_ROWS - 8; f++) {
    r[n].kind = ROW_HDR; r[n].fac = (uint8_t)f; r[n].label = g3f_facility_name(f); n++;
    for (int m = 0; m < g3f_modes(f); m++)
      for (int l = 0; l < 2; l++) {
        r[n].kind = ROW_LANE; r[n].fac = (uint8_t)f;
        r[n].mode = (uint8_t)m; r[n].lvl = (uint8_t)l; n++;
      }
    if (f == G3F_DOME) {
      for (int m = 0; m < 2; m++)
        for (int l = 0; l < 2; l++) {
          r[n].kind = ROW_COUNTER; r[n].fac = (uint8_t)f;
          r[n].mode = (uint8_t)m; r[n].lvl = (uint8_t)l;
          r[n].off = (uint16_t)(G3F_DOME_CHAMPS_OFF + (m * 2 + l) * 2);
          r[n].cap = G3F_MAX_CHAMPS; r[n].label = "Champs"; n++;
        }
    } else if (f == G3F_FACTORY) {
      for (int m = 0; m < 2; m++)
        for (int l = 0; l < 2; l++) {
          r[n].kind = ROW_COUNTER; r[n].fac = (uint8_t)f;
          r[n].mode = (uint8_t)m; r[n].lvl = (uint8_t)l;
          r[n].off = (uint16_t)(G3F_FACTORY_RENTS_OFF + (m * 2 + l) * 2);
          r[n].cap = G3F_MAX_STREAK; r[n].label = "Rentals"; n++;
        }
    } else if (f == G3F_PIKE) {
      for (int l = 0; l < 2; l++) {
        r[n].kind = ROW_COUNTER; r[n].fac = (uint8_t)f;
        r[n].mode = 0; r[n].lvl = (uint8_t)l;
        r[n].off = (uint16_t)(G3F_PIKE_TOTALS_OFF + l * 2);
        r[n].cap = G3F_MAX_STREAK; r[n].label = "Clears"; n++;
      }
    }
  }
  /* Battle Points, on their own header. */
  r[n].kind = ROW_HDR; r[n].fac = 0xFF; r[n].label = "Battle Points"; n++;
  r[n].kind = ROW_COUNTER; r[n].fac = 0xFF; r[n].mode = 0xFF; r[n].lvl = 0xFF;
  r[n].off = G3F_BATTLE_POINTS_OFF; r[n].cap = G3F_MAX_STREAK; r[n].label = "BP to spend"; n++;
  r[n].kind = ROW_COUNTER; r[n].fac = 0xFF; r[n].mode = 0xFF; r[n].lvl = 0xFF;
  r[n].off = G3F_CARD_BP_OFF; r[n].cap = 0xFFFF; r[n].label = "BP on card"; n++;
  return n;
}

/* Row labels must fit 11 columns: the list row is drawn at x=10 as
 * "%-11s%4d/%4d %c" = 22 columns = 176 px, well inside the 240 px screen. The
 * full mode names ("Link multis") do not fit, so lanes use short forms. */
static const char* short_mode(int fac, int mode) {
  if (g3f_modes(fac) == 1) return "";              /* singles-only facility */
  switch (mode) {
    case G3F_SINGLES: return "Sgl ";
    case G3F_DOUBLES: return "Dbl ";
    case G3F_MULTIS:  return "Mlt ";
    default:          return "Link ";
  }
}

static void lane_label(char* out, int cap, const FrRow* r) {
  const char* lv = (r->lvl == G3F_LVL_OPEN) ? "Open" : "Lv50";
  if (r->kind == ROW_COUNTER && r->fac == 0xFF) { sniprintf(out, cap, "%s", r->label); return; }
  if (r->kind == ROW_COUNTER) {
    /* "Champs", "Rentals", "Clears" + mode initial + level */
    const char* m2 = short_mode(r->fac, r->mode);
    if (m2[0]) sniprintf(out, cap, "%s %c%s", r->label, m2[0], r->lvl ? "Op" : "50");
    else       sniprintf(out, cap, "%s %s", r->label, r->lvl ? "Op" : "50");
    return;
  }
  sniprintf(out, cap, "%s%s", short_mode(r->fac, r->mode), lv);
}

/* ---- per-lane action menu --------------------------------------------------
 * There is ONE "Meet Brain" action, not a Silver/Gold pair: the game chooses the
 * threshold by how many symbols you already own, so offering the other tier would
 * write a number the game never compares against (and jump the tier you actually
 * needed). `brain_lbl` names the tier the save will really summon. */
enum { ACT_CURRENT, ACT_BEST, ACT_MEET_BRAIN, ACT_CLEAR, ACT_ACTIVE, ACT_COUNT };

static int lane_menu(bool singles, const char* brain_lbl) {
  const char* L[ACT_COUNT] = {
    "Set current streak", "Set best streak", brain_lbl,
    "Clear this lane", "Toggle 'active' bit",
  };
  const int mx = 30, my = 34, mw = 180, rh = 11, mh = 16 + ACT_COUNT * rh + 10;
  int sel = 0;
  for (;;) {
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "EDIT LANE");
    ui_hline(mx + 2, my + 14, mw - 4, UI_BORDER);
    for (int i = 0; i < ACT_COUNT; i++) {
      int y = my + 18 + i * rh; bool s = (i == sel);
      /* The Brain preset only means anything in singles — the game returns
       * NOT_READY for every other battle mode. Shown greyed rather than hidden,
       * so the rule is visible instead of mysterious. */
      bool dim = (!singles && i == ACT_MEET_BRAIN);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, rh - 1, UI_SEL, UI_TITLE);
      ui_text(mx + 8, y, s ? UI_SELTEXT : (dim ? UI_DIM : UI_TEXT), L[i]);
    }
    ui_text(mx + 6, my + mh - 8, UI_DIM, "A pick  B cancel");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    if (k & KEY_UP)   sel = sel ? sel - 1 : ACT_COUNT - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % ACT_COUNT;
    if (k & KEY_A) {
      if (!singles && sel == ACT_MEET_BRAIN) {
        snd_deny();
        s_msg("SINGLES ONLY", UI_WARN, "Brains appear only in", "Singles battles.");
        continue;
      }
      return sel;
    }
  }
}

/* One selectable tower row (i=0/1 the two records, i=2 total wins).
 * ui_text_sel draws no background when unselected (D5's lesson 3), so this erases
 * first; UI_ROW_H (8px) fits well inside the 12px row pitch, no shared-scanline
 * case. */
static void rst_row_paint(const uint8_t* sb2, int i, bool sel) {
  char l[40];
  int y = (i < 2) ? 62 + i * 12 : 86;
  ui_fill_rect(4, y, 232, UI_ROW_H, UI_BG);
  if (i < 2) siprintf(l, "%-17s%4d", i ? "Record (Open)" : "Record (Lv50)", g3f_rs_record(sb2, i));
  else       siprintf(l, "%-17s%4d", "Total wins", g3f_u16_get(sb2, G3F_RS_TOTAL_WINS_OFF));
  ui_text_sel(4, y, 232, sel, UI_TEXT, l);
}
/* The non-selectable "Best (derived)" row -- g3f_rs_set_record's own comment says it
 * refreshes this cached value as a SIDE EFFECT of editing either record, so it needs
 * its own value-diff even though it is never the cursor target. */
static void rst_best_paint(const uint8_t* sb2) {
  char l[40];
  ui_fill_rect(4, 100, 232, UI_ROW_H, UI_BG);
  siprintf(l, "%-17s%4d", "Best (derived)", g3f_u16_get(sb2, G3F_RS_BEST_STREAK_OFF));
  ui_text(4, 100, UI_DIM, l);
}

/* ---- Ruby/Sapphire Battle Tower --------------------------------------------
 * s_msg() (this file's own, NOT pdna_main.c's msg_wait) draws its panel directly
 * over whatever is already on screen -- it never calls ui_clear(), so it does not
 * bump ui_clear_gen() either. Every call site here that shows it must therefore
 * force the shadow invalid by hand (pv_valid = false) right after, or the panel's
 * leftover pixels would sit there forever with nothing to trigger their removal --
 * the "overlay without ui_clear needs manual invalidation" rule. num_entry() (via
 * osk_search, pdna_main.c-shared osk.c) DOES ui_clear() even on cancel, so gen alone
 * already catches every path through it. */
static void rs_tower(uint8_t* sb2) {
  bool dirty = false;
  int sel = 0;
  int pv_sel = -1, pv_r0 = -1, pv_r1 = -1, pv_tw = -1, pv_best = -1;
  bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    int r0 = g3f_rs_record(sb2, 0), r1 = g3f_rs_record(sb2, 1);
    int tw = g3f_u16_get(sb2, G3F_RS_TOTAL_WINS_OFF);
    int best = g3f_u16_get(sb2, G3F_RS_BEST_STREAK_OFF);
    bool full = !pv_valid || pv_gen != ui_clear_gen();

    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "BATTLE TOWER");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(4, 22, UI_DIM, "Ruby/Sapphire store only the");
      ui_text(4, 32, UI_DIM, "RECORD streaks. The current");
      ui_text(4, 42, UI_DIM, "streak is derived in-game.");
      for (int i = 0; i < 3; i++) rst_row_paint(sb2, i, sel == i);
      rst_best_paint(sb2);
      ui_text(4, 118, UI_DIM, "The game recomputes 'best'");
      ui_text(4, 128, UI_DIM, "from the two records.");
      ui_text(4, 152, UI_DIM, app_can_edit() ? "A edit  B back" : "B back (read-only)");
    } else {
      bool rowchg[3] = { r0 != pv_r0, r1 != pv_r1, tw != pv_tw };
      for (int i = 0; i < 3; i++) {
        bool s = (sel == i), os = (pv_sel == i);
        if (rowchg[i] || s != os) rst_row_paint(sb2, i, s);
      }
      if (best != pv_best) rst_best_paint(sb2);
    }
    pv_sel = sel; pv_r0 = r0; pv_r1 = r1; pv_tw = tw; pv_best = best;
    pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) break;
    if (k & KEY_UP)   sel = sel ? sel - 1 : 2;
    if (k & KEY_DOWN) sel = (sel + 1) % 3;
    if (k & KEY_A) {
      if (!app_can_edit()) { snd_deny(); s_msg("READ-ONLY", UI_WARN, app_readonly_why(), 0); pv_valid = false; continue; }
      int v;
      if (sel < 2) {
        if (!num_entry(sel ? "Record streak (Open)" : "Record streak (Lv50)",
                       g3f_rs_record(sb2, sel), G3F_MAX_STREAK, &v)) continue;
        g3f_rs_set_record(sb2, sel, v);       /* also refreshes the derived 0x572 cache */
      } else {
        if (!num_entry("Total tower wins", g3f_u16_get(sb2, G3F_RS_TOTAL_WINS_OFF),
                       G3F_MAX_STREAK, &v)) continue;
        g3f_u16_set(sb2, G3F_RS_TOTAL_WINS_OFF, v, G3F_MAX_STREAK);
      }
      dirty = true;
      rmbl_fire(RCUE_EDIT);
    }
  }
  if (dirty) (void)app_hold_sb2("Tower records");        /* #234 s2: staged, no prompt */
}

/* How many symbols (Silver/Gold) this facility has already awarded. The game uses
 * exactly this count to pick which Frontier Brain threshold it tests, so the preset
 * is meaningless without it. Symbol flags live in SaveBlock1, not SaveBlock2. */
static int fac_symbols(const uint8_t* sb1, PkGame game, int fac) {
  int n = 0;
  if (!sb1) return 0;
  for (int i = 0; i < 2; i++) {                 /* sym index = fac*2 + (0 silver, 1 gold) */
    int fl = pk_frontier_flag(game, fac * 2 + i);
    if (fl >= 0 && pk_flag_get(sb1, game, fl)) n++;
  }
  return n;
}

/* One streaks-table row (HDR/LANE/COUNTER). Row pitch 10; the HDR panel is 9px tall
 * and ui_text_sel's own highlight is UI_ROW_H (8px) -- both fit inside the pitch with
 * a 1-2px margin, so this erase (10px, touching not overlapping the next row's) is
 * the only wipe needed regardless of kind. Values are read LIVE from sb2 every call
 * (cur/rec/act or the counter), same as render_browser reads live from g_entries --
 * correct because every path that can change them also forces a full repaint (see
 * pdna_frontier's own header comment on this loop). */
static void frs_row_paint(const uint8_t* sb2, const FrRow* rows, int idx, int i, bool sel) {
  const FrRow* r = &rows[idx];
  int y = 18 + i * 10;
  ui_fill_rect(0, y - 1, UI_SCR_W, 9, UI_BG);   /* 9, NOT the 10-px pitch: row ink spans
                                                 * y-1..y+7 only (panel y-1..y+7, text
                                                 * y..y+7); a 10-tall wipe on visible row
                                                 * 12 (y=138) reaches y+8=146 and erases
                                                 * the footer rule drawn only on full */
  if (r->kind == ROW_HDR) {
    if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    ui_text(4, y, sel ? UI_SELTEXT : UI_DIRCLR, r->label);
    return;
  }
  char l[48], nm[28];
  lane_label(nm, sizeof nm, r);
  if (r->kind == ROW_LANE) {
    int cur = g3f_streak_get(sb2, r->fac, r->mode, r->lvl, G3F_CURRENT);
    int rec = g3f_streak_get(sb2, r->fac, r->mode, r->lvl, G3F_RECORD);
    bool act = g3f_active_get(sb2, r->fac, r->mode, r->lvl);
    /* '*' = the lane's winStreakActiveFlags bit is set, i.e. the game will KEEP
     * this streak. Without it the number is cosmetic and dies on the next visit. */
    /* 22 cols at x=10 -> 186 px. Indent comes from x, not padding spaces, so a
     * long label can never push the row past the screen edge. */
    siprintf(l, "%-11s%4d/%4d %c", nm, cur, rec, act ? '*' : ' ');
    ui_text_sel(10, y, 226, sel, (cur > 0 && !act) ? UI_WARN : UI_TEXT, l);
  } else {
    siprintf(l, "%-11s%4d", nm, g3f_u16_get(sb2, r->off));
    ui_text_sel(10, y, 226, sel, UI_DIM, l);
  }
}

/* ---- the Emerald screen -----------------------------------------------------
 *
 * VERIFIED (batch-D8 note 3): every path that writes a lane/counter value routes
 * through one of two gates, both of which invalidate the shadow --
 *   - ROW_COUNTER and every ACT_CURRENT/ACT_BEST edit go through num_entry() ->
 *     osk_search (pdna_main.c-shared osk.c), which ui_clear()s even on cancel, so
 *     gen alone catches it;
 *   - every ACT_* case (including ACT_MEET_BRAIN, ACT_CLEAR, ACT_ACTIVE, which do
 *     NOT go through num_entry) is only reachable after lane_menu() has already run,
 *     and lane_menu -- like this file's own s_msg() -- draws its panel directly over
 *     the list WITHOUT ui_clear(), so it does NOT bump gen. Both of THOSE are
 *     therefore forced invalid by hand (pv_valid = false) the moment they return,
 *     unconditionally -- covering every downstream ACT_* case and the READ-ONLY
 *     denial in one place each, rather than one flag per case.
 * With both gates covered, `top` stays OUT of `full` and gets the same per-row
 * index-identity diff as pdna_main.c's render_browser/pdna_secretbase (D5's F1 fix):
 * LEFT/RIGHT's +-6 jump and UP/DOWN wrap both move the window without an auto-repeat
 * concern (s_wait(), this file's own helper, does not key_repeat -- unlike
 * pdna_main.c's wait_keys()), but the window can still legitimately move, and a
 * fixed-width value-diff on top of a wrong row position would be exactly D5's F1 bug
 * again. */
void pdna_frontier(uint8_t* sb1, uint8_t* sb2, PkGame game) {
  if (!sb2 || !g3f_supported(game)) {
    s_msg("FRONTIER", UI_DIM, "This game has no Battle", "Frontier records.");
    return;
  }
  if (game == PK_RS) { rmbl_fire(RCUE_ROOM); rs_tower(sb2); return; }

  static FrRow EWRAM_BSS rows[FR_MAX_ROWS];   /* 768 B — keep it off the IWRAM stack */
  int nrows = build_rows(rows);
  int sel = 1, top = 0;                     /* start on the first lane, not the header */
  bool dirty = false, warned_busy = false;
  int pv_top = -1, pv_sel = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  rmbl_fire(RCUE_ROOM);

  for (;;) {
    const int vis = 13;
    if (sel < top) top = sel;
    else if (sel >= top + vis) top = sel - vis + 1;
    if (top > nrows - vis) top = nrows - vis;
    if (top < 0) top = 0;

    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "FRONTIER STREAKS");
      /* 12 cols = 96 px; at x=142 it ends on 238, inside the 240 px screen. */
      if (g3f_challenge_active(sb2))
        ui_text(142, 4, UI_WARN, "IN CHALLENGE");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_hline(0, 146, UI_SCR_W, UI_BORDER);
      /* 27 cols max at x=4 -> 220 px. */
      ui_text(4, 150, UI_DIM, app_can_edit() ? "A edit  cur/best  *=kept" : "read-only  cur/best *=kept");
    }
    uint32_t dirtyrow = 0;
    for (int i = 0; i < vis && top + i < nrows; i++) {
      int f = top + i, of = pv_top + i;
      bool s = (f == sel), os = (of == pv_sel);
      if (full || of != f || s != os) dirtyrow |= 1u << i;
    }
    for (int i = 0; i < vis && top + i < nrows; i++)
      if (dirtyrow & (1u << i)) frs_row_paint(sb2, rows, top + i, i, top + i == sel);
    pv_top = top; pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) break;
    if (k & KEY_UP)    sel = sel ? sel - 1 : nrows - 1;
    if (k & KEY_DOWN)  sel = (sel + 1) % nrows;
    if (k & KEY_LEFT)  { sel -= 6; if (sel < 0) sel = 0; }
    if (k & KEY_RIGHT) { sel += 6; if (sel >= nrows) sel = nrows - 1; }
    if (!(k & KEY_A)) continue;

    const FrRow* r = &rows[sel];
    if (r->kind == ROW_HDR) continue;
    if (!app_can_edit()) { snd_deny(); s_msg("READ-ONLY", UI_WARN, app_readonly_why(), 0); pv_valid = false; continue; }

    /* Editing mid-challenge collides with the resume flow, which also owns the
     * party stashed for the run. Warn once per visit, then let the user decide. */
    if (g3f_challenge_active(sb2) && !warned_busy) {
      if (!app_confirm("A challenge is in progress!", "Editing now can break it."))
        continue;
      warned_busy = true;
    }

    char nm[28];
    if (r->kind == ROW_COUNTER) {
      int v;
      lane_label(nm, sizeof nm, r);
      if (!num_entry(nm, g3f_u16_get(sb2, r->off), r->cap, &v)) continue;   /* cancelled */
      g3f_u16_set(sb2, r->off, v, r->cap);
      dirty = true; rmbl_fire(RCUE_EDIT);
      continue;
    }

    bool singles = (r->mode == G3F_SINGLES);
    /* The tier the game will ACTUALLY test for is fixed by how many symbols this
     * facility has already awarded (SaveBlock1 flags), not by what we'd like. */
    int symbols = fac_symbols(sb1, game, r->fac);
    int tier = g3f_brain_tier(r->fac, symbols);
    char brain_lbl[24];
    siprintf(brain_lbl, "Meet Brain (%s)", tier == 1 ? "Gold" : "Silver");

    int act = lane_menu(singles, brain_lbl);
    pv_valid = false;    /* lane_menu drew its own panel over the list without
                          * ui_clear() -- see this function's header comment */
    if (act < 0) continue;
    int cap = g3f_streak_cap(r->fac);
    int v;
    switch (act) {
      case ACT_CURRENT:
        lane_label(nm, sizeof nm, r);
        if (!num_entry(nm, g3f_streak_get(sb2, r->fac, r->mode, r->lvl, G3F_CURRENT), cap, &v))
          continue;                                    /* cancel writes nothing */
        /* raise_record defaults ON: a current streak above the record is a state the
         * game itself never produces. */
        g3f_set_current(sb2, r->fac, r->mode, r->lvl, v, true);
        dirty = true;
        break;
      case ACT_BEST:
        if (!num_entry("Best streak", g3f_streak_get(sb2, r->fac, r->mode, r->lvl, G3F_RECORD), cap, &v))
          continue;
        g3f_streak_set(sb2, r->fac, r->mode, r->lvl, G3F_RECORD, v);
        dirty = true;
        break;
      case ACT_MEET_BRAIN: {
        int t = g3f_brain_target(r->fac, symbols);
        g3f_set_current(sb2, r->fac, r->mode, r->lvl, t, true);
        char l[40]; siprintf(l, "Set to %d for %s.", t, tier == 1 ? "Gold" : "Silver");
        s_msg("BRAIN NEXT BATTLE", UI_OK, l, "Win once here to meet them.");
        dirty = true;
      } break;
      case ACT_CLEAR:
        g3f_set_current(sb2, r->fac, r->mode, r->lvl, 0, false);   /* also clears the active bit */
        dirty = true;
        break;
      case ACT_ACTIVE: {
        bool on = g3f_active_get(sb2, r->fac, r->mode, r->lvl);
        g3f_active_set(sb2, r->fac, r->mode, r->lvl, !on);
        dirty = true;
      } break;
      default: break;
    }
    if (dirty) rmbl_fire(RCUE_EDIT);
  }

  if (dirty) (void)app_hold_sb2("Frontier streaks");     /* #234 s2: staged, no prompt */
}
