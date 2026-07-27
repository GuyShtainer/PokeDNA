/*
 * Trainer card / stats screen for PokeDNA. With the vendored card art the
 * REAL in-game card of the loaded save's own game (RS / Emerald / FRLG,
 * per-game layout tables in card_bg.h) IS the editor: a red selection frame
 * moves with U/D across the fields drawn on the card front (IDNo, name,
 * stars, money, play time, gender photo, badge row) and A edits the field in
 * place — SEX flips instantly (the card art swap is the feedback) and A on
 * the badge row toggles the badge under the LEFT/RIGHT badge cursor. L or R
 * flips to the real card BACK (HoF debut, link W/L, trades, and the game's
 * own extra rows), all editable even at 0; B on the back returns to the
 * front, B on the front exits with the one save prompt. Art-free builds keep
 * the plain details/edit page (trainer identity, money, play time, Pokédex
 * counts, HoF first-clear time, Game Records, star achievements — the latter
 * toggled honestly via gen3_stars).
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_trainer.h"
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "pdna_app.h"
#include "gen3_flags.h"   /* badge / frontier flag toggles */
#include "gen3_stars.h"   /* star achievements (the card's tier)  */
#include "card_bg.h"      /* real Emerald card front (weak NULLs) */
#include "rumble.h"       /* io-suspend around the big bg DMA     */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
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

/* ---- the real per-game card front (generated art; tools/gen_card_bg.py) ---- */

/* Strong card_bg()/card_badge16()/card_hoenn_dex() come from the GENERATED
 * card_bg.c (git-ignored ripped art); these weak NULLs keep an art-free clone
 * building — the screen then opens straight on the plain page (same pattern
 * as the bag-screen fallback in pdna_bag.c). */
__attribute__((weak)) const uint16_t* card_bg(int game, int tier, int female) { (void)game; (void)tier; (void)female; return 0; }
__attribute__((weak)) const uint16_t* card_bg_back(int game, int tier, int female) { (void)game; (void)tier; (void)female; return 0; }
__attribute__((weak)) const uint16_t* card_badge16(int game, int i) { (void)game; (void)i; return 0; }
__attribute__((weak)) const uint16_t* card_hoenn_dex(void) { return 0; }

#define CINK   RGB15( 9,  9, 10)   /* card text (the games' dark-gray ink) */
#define CFOOT  RGB15(31, 31, 31)   /* footer hint on the card border       */
#define CSEL   RGB15(26,  4,  3)   /* selection frame (pdna_bag's red)     */

/* Copy the 240x160 frame ROM -> VRAM one row at a time, each row read with
 * the wallpaper discipline (pdna_box.c wp_copy_verified): volatile re-read +
 * byte-compare, up to 4 attempts, from IWRAM so the cart bus carries pure
 * data reads. A raw 75 KB dma3_cpy is exactly the bulk-ROM-read shape this
 * cartridge demonstrably glitches (the wallpaper/icon saga) — the on-HW
 * "hot mess" class — and EZ reads have no retry, so verify instead of trust. */
IWRAM_CODE __attribute__((noinline))
static void card_blit(const uint16_t* bg) {
  volatile const uint16_t* vsrc = bg;
  rumble_io_suspend();                       /* bg lives in ROM */
  for (uint32_t row = 0; row < CARD_BG_H; row++) {
    uint16_t* dst = &vid_mem[row * CARD_BG_W];
    const volatile uint16_t* src = vsrc + row * CARD_BG_W;
    for (int a = 0; a < 4; a++) {
      for (int i = 0; i < CARD_BG_W; i++) dst[i] = src[i];
      int ok = 1;
      for (int i = 0; i < CARD_BG_W; i++) if (dst[i] != src[i]) { ok = 0; break; }
      if (ok) break;                         /* two passes agree -> row is good */
    }
  }
  rumble_io_resume();
}

/* The cursor-editable fields ON the card (CARDF_* order) own the per-game
 * screen rects in CARD_LAYOUTS[game].rect: the selection frame is drawn on
 * the rect's edge and a move/redraw restores exactly this rect from the ROM
 * bg. All geometry comes from card_bg.h — nothing game-specific here. */

/* Draw one field's runtime overlay at the game's own position (card_bg.h);
 * STARS and SEX are baked into the frame, so they have nothing to add. RS
 * bakes the NAME/IDNo./MONEY/TIME labels into the art (L->labels == 0), so
 * only the bare values are drawn there. */
static void card_field(int f, PkGame game, const uint8_t* sb1, const char* name,
                       uint16_t tid, uint32_t money, uint16_t ph, uint8_t pm) {
  const CardLayout* L = &CARD_LAYOUTS[game];
  char line[32];
  switch (f) {
    case CARDF_ID:                           /* E/FRLG write "IDNo." + id; RS
                                              * bakes the label, digits only  */
      siprintf(line, L->labels ? "IDNo.%05u" : "%05u", (unsigned)tid);
      if (L->id_w)                           /* Emerald: centered in the box  */
        ui_text(L->id_x + (L->id_w - 8 * (int)strlen(line)) / 2, L->id_y, CINK, line);
      else
        ui_text(L->id_x, L->id_y, CINK, line);
      break;
    case CARDF_NAME:
      if (L->labels) siprintf(line, "NAME: %s", name);   /* gText_TrainerCardName */
      else           siprintf(line, "%s", name);
      ui_text(L->name_x, L->name_y, CINK, line);
      break;
    case CARDF_MONEY:
      if (L->labels) ui_text(L->lbl_x, L->money_y, CINK, "MONEY");
      siprintf(line, "$%lu", (unsigned long)money);
      ui_text(L->val_xr - 8 * (int)strlen(line), L->money_y, CINK, line);
      break;
    case CARDF_TIME:
      if (L->labels) ui_text(L->lbl_x, L->time_y, CINK, "TIME");
      siprintf(line, "%u:%02u", (unsigned)ph, (unsigned)pm);
      ui_text(L->val_xr - 8 * (int)strlen(line), L->time_y, CINK, line);
      break;
    case CARDF_BADGES:                       /* badges overlay only when owned
                                              * (the baked empty slots keep the
                                              * games' own 1..8 digit marks) */
      for (int i = 0; i < 8; i++)
        if (pk_flag_get(sb1, game, pk_badge_flag(game, i)))
          ui_sprite(L->badge_x + 24 * i, L->badge_y, 16, 16, card_badge16(game, i));
      break;
  }
}

/* A field's cursor rect. On the badge row the cursor owns ONE 16x16 badge
 * cell (bsel 0..7) — A toggles exactly that badge, so the frame hugs it. */
static void card_rect(PkGame game, int f, int bsel, int* x, int* y, int* w, int* h) {
  const CardLayout* L = &CARD_LAYOUTS[game];
  if (f == CARDF_BADGES) { CARD_BADGE_RECT(L, bsel, *x, *y, *w, *h); return; }
  *x = L->rect[f].x; *y = L->rect[f].y; *w = L->rect[f].w; *h = L->rect[f].h;
}

/* 2 px red selection frame on a field's rect edge (over the card art). */
static void card_sel_frame(PkGame game, int f, int bsel) {
  int x, y, w, h; card_rect(game, f, bsel, &x, &y, &w, &h);
  m3_frame(x, y, x + w, y + h, CSEL);
  m3_frame(x + 1, y + 1, x + w - 1, y + h - 1, CSEL);
}

/* restore a field's rect from the ROM bg and repaint its overlay (no smear).
 * +1 px right/bottom so the selection frame is erased whichever edge
 * convention m3_frame uses (all rects stay on-screen with the margin). */
static void card_restore(const uint16_t* bg, int f, int bsel, PkGame game,
                         const uint8_t* sb1, const char* name, uint16_t tid,
                         uint32_t money, uint16_t ph, uint8_t pm) {
  int x, y, w, h; card_rect(game, f, bsel, &x, &y, &w, &h);
  ui_bg_restore(bg, x, y, w + 1, h + 1);
  card_field(f, game, sb1, name, tid, money, ph, pm);
}

/* ---- the card BACK (L/R flips; geometry + row model in card_bg.h) ---- */

static uint32_t back_stat(const uint8_t* sb1, const uint8_t* sb2, PkGame game, int stat) {
  return pk_game_stat(sb1, sb2, game, stat);
}

static void back_row_value(const CardBackRow* r, PkGame game, const uint8_t* sb1,
                           const uint8_t* sb2, char* out) {
  switch (r->kind) {
    case CBK_HOF: {                          /* shown 0:00:00 when never entered */
      uint16_t h = 0; uint8_t m = 0, s = 0;
      pk_hof_time(sb1, sb2, game, &h, &m, &s);
      siprintf(out, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    } break;
    case CBK_WL:
      siprintf(out, "W%lu L%lu",
               (unsigned long)back_stat(sb1, sb2, game, r->stat),
               (unsigned long)back_stat(sb1, sb2, game, r->stat + 1));
      break;
    case CBK_STAT:
      siprintf(out, "%lu", (unsigned long)back_stat(sb1, sb2, game, r->stat));
      break;
    case CBK_TOWER_RS:
      siprintf(out, "W%u S%u", (unsigned)pk_rs_tower(sb2, 0), (unsigned)pk_rs_tower(sb2, 1));
      break;
    case CBK_BP_E:
      siprintf(out, "%u BP", (unsigned)pk_e_card_bp(sb2));
      break;
  }
}

static void back_rect(PkGame game, int row, int* x, int* y, int* w, int* h) {
  const CardBackLayout* B = &CARD_BACK_LAYOUTS[game];
  *x = B->lbl_x - 3; *y = B->rows[row].y - 3;
  *w = B->val_xr - B->lbl_x + 6; *h = 14;
}

static void back_sel_frame(PkGame game, int row) {
  int x, y, w, h; back_rect(game, row, &x, &y, &w, &h);
  m3_frame(x, y, x + w, y + h, CSEL);
  m3_frame(x + 1, y + 1, x + w - 1, y + h - 1, CSEL);
}

static void back_row_draw(int row, PkGame game, const uint8_t* sb1, const uint8_t* sb2) {
  const CardBackLayout* B = &CARD_BACK_LAYOUTS[game];
  char v[24];
  ui_text(B->lbl_x, B->rows[row].y, CINK, B->rows[row].label);
  back_row_value(&B->rows[row], game, sb1, sb2, v);
  ui_text(B->val_xr - 8 * (int)strlen(v), B->rows[row].y, CINK, v);
}

static void back_restore(const uint16_t* bg, int row, PkGame game,
                         const uint8_t* sb1, const uint8_t* sb2) {
  int x, y, w, h; back_rect(game, row, &x, &y, &w, &h);
  ui_bg_restore(bg, x, y, w + 1, h + 1);
  back_row_draw(row, game, sb1, sb2);
}

/* Edit one back row (the front editors' exact prompt style; RAM-only until
 * the caller's single exit commit). Sets *d1 (gameStats, SB1) / *d2 (SB2). */
static void back_row_edit(const CardBackRow* r, PkGame game, uint8_t* sb1,
                          uint8_t* sb2, bool* d1, bool* d2) {
  switch (r->kind) {
    case CBK_HOF: {
      uint16_t h = 0; uint8_t m = 0, s = 0;
      pk_hof_time(sb1, sb2, game, &h, &m, &s);
      h = (uint16_t)num_entry("HOF DEBUT HOURS", h, 999);
      m = (uint8_t)num_entry("HOF DEBUT MINUTES", m, 59);
      s = (uint8_t)num_entry("HOF DEBUT SECONDS", s, 59);
      uint32_t packed = ((uint32_t)h << 16) | ((uint32_t)m << 8) | s;
      pk_set_game_stat(sb1, sb2, game, PK_STAT_FIRST_HOF_PLAY_TIME, packed);
      /* the games gate the row (and the HoF star) on ENTERED_HOF: a nonzero
       * debut time needs at least one entry, 0:00:00 = honestly never entered */
      uint32_t entered = pk_game_stat(sb1, sb2, game, PK_STAT_ENTERED_HOF);
      if (packed && !entered)      pk_set_game_stat(sb1, sb2, game, PK_STAT_ENTERED_HOF, 1);
      else if (!packed && entered) pk_set_game_stat(sb1, sb2, game, PK_STAT_ENTERED_HOF, 0);
      *d1 = true;
    } break;
    case CBK_WL: {
      uint32_t w = num_entry("LINK WINS", back_stat(sb1, sb2, game, r->stat), r->cap);
      uint32_t l = num_entry("LINK LOSSES", back_stat(sb1, sb2, game, r->stat + 1), r->cap);
      pk_set_game_stat(sb1, sb2, game, r->stat, w);
      pk_set_game_stat(sb1, sb2, game, r->stat + 1, l);
      *d1 = true;
    } break;
    case CBK_STAT:
      pk_set_game_stat(sb1, sb2, game, r->stat,
                       num_entry(r->label, back_stat(sb1, sb2, game, r->stat), r->cap));
      *d1 = true;
      break;
    case CBK_TOWER_RS: {
      uint16_t w = (uint16_t)num_entry("TOWER WINS", pk_rs_tower(sb2, 0), r->cap);
      uint16_t s = (uint16_t)num_entry("TOWER STREAK", pk_rs_tower(sb2, 1), r->cap);
      pk_set_rs_tower(sb2, 0, w);
      pk_set_rs_tower(sb2, 1, s);
      *d2 = true;
    } break;
    case CBK_BP_E:
      pk_set_e_card_bp(sb2, (uint16_t)num_entry("BATTLE POINTS", pk_e_card_bp(sb2), r->cap));
      *d2 = true;
      break;
  }
}

/* The STARS row's sub-editor: this game's 4 star achievements (each ONE card
 * star / palette tier), toggled honestly in the underlying save data via
 * gen3_stars. Returns a dirty mask (1 = SB1, 2 = SB2); the caller commits. */
static int stars_editor(uint8_t* sb1, uint8_t* sb2, PkGame game) {
  const uint16_t* dex = card_hoenn_dex();    /* NULL in an art-free build */
  const int n = pk_star_ach_count(game);
  int sel = 0, dirty = 0;
  for (;;) {
    ui_clear();
    char t[32]; siprintf(t, "CARD STARS  %d/4", pk_star_count(sb1, sb2, game, dex));
    ui_text(4, 2, UI_TITLE, t);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < n; i++) {
      int y = 16 + i * 9; bool s = (i == sel);
      bool can = pk_star_ach_can_set(game, i, dex);
      bool on  = pk_star_ach_done(sb1, sb2, game, i, dex);
      char row[40]; siprintf(row, "%-16s %s", pk_star_ach_name(game, i),
                             !can ? "n/a" : on ? "ON" : "off");
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(8, y, s ? UI_SELTEXT : (can && on ? UI_OK : UI_DIM), row);
    }
    ui_text(4, 62, UI_DIM, "Each ON = one star (card color).");
    if (!dex) ui_text(4, 72, UI_DIM, "n/a: needs the generated art data.");
    ui_text(4, 152, UI_DIM, "A toggle  U/D  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return dirty;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      if (!pk_star_ach_can_set(game, sel, dex)) continue;
      bool on = pk_star_ach_done(sb1, sb2, game, sel, dex);
      if (on && pk_star_ach_is_dex(game, sel) &&   /* dex stars: OFF un-catches */
          !app_confirm("Clear dex catches?", "Un-catches these species."))
        continue;
      dirty |= pk_star_ach_set(sb1, sb2, game, sel, !on, dex);
    }
  }
}

/* the one exit-save prompt; commits through the usual verified paths. */
static void trainer_commit(bool d1, bool d2) {
  if ((d1 || d2) && app_confirm("Save trainer changes?", "Writes the card edits now.")) {
    if (d1 && d2)  app_commit_sb12();        /* both blocks: ONE backup + write */
    else if (d2)   app_commit_sb2();         /* trainer block (section 0)     */
    else           app_commit_sb1();         /* money (sections 1..4)         */
  }
}

/* The card editor (any game with art): the selection frame walks the fields
 * ON the card and A edits in place with the plain page's exact editors and
 * validation; every full-screen editor forces a re-blit on return (gender/
 * star edits swap the frame itself). SEX and single badges toggle INSTANTLY
 * (the art swap / badge appearing is the feedback). L/R flips to the card
 * BACK (per-game stat rows, editable even at 0); B there returns to the
 * front, B on the front returns to the caller, which owns the one
 * trainer_commit prompt. */
static void card_editor(uint8_t* sb1, uint8_t* sb2, PkGame game, bool edit,
                        char* name, uint8_t* gender, uint16_t* tid, uint16_t* sid,
                        uint32_t* money, uint16_t* ph, uint8_t* pm,
                        bool* d1, bool* d2) {
  const CardLayout* L = &CARD_LAYOUTS[game];
  const int nback = CARD_BACK_LAYOUTS[game].nrows;
  int sel = CARDF_NAME, bsel = 0, brow = 0;
  bool back = false, full = true;
  const uint16_t* bg = 0;
  for (;;) {
    if (full) {                              /* (re)blit + all overlays */
      int tier = pk_star_count(sb1, sb2, game, card_hoenn_dex());
      if (!back) {
        bg = card_bg(game, tier, *gender);
        card_blit(bg);
        for (int f = 0; f < CARDF_NUM; f++)
          card_field(f, game, sb1, name, *tid, *money, *ph, *pm);
        if (pk_flag_get(sb1, game, L->dex_flag)) {   /* dex row: display-only */
          char line[16]; int seen, caught; bool nat; pk_pokedex(sb2, &seen, &caught, &nat);
          if (L->labels) ui_text(L->lbl_x, L->dex_y, CINK, "POKeDEX");
          siprintf(line, "%d", caught);
          ui_text(L->val_xr - 8 * (int)strlen(line), L->dex_y, CINK, line);
        }
        ui_text(4, 152, CFOOT, edit ? "U/D A edit  L/R flip  B save"
                                    : "L/R flip  B back");
        if (edit) card_sel_frame(game, sel, bsel);
      } else {                               /* the card back */
        const CardBackLayout* B = &CARD_BACK_LAYOUTS[game];
        bg = card_bg_back(game, tier, *gender);
        card_blit(bg);
        char line[32];
        /* RS/E append "'s TRAINER CARD" (gText_Var1sTrainerCard); FRLG's own
         * back prints ONLY the name after its baked "TRAINER:" (pokefirered
         * trainer_card.c:1254-1261 BufferNameForCardBack: the suffix is
         * RSE-cards-only). */
        siprintf(line, game == PK_FRLG ? "%s" : "%s's TRAINER CARD", name);
        ui_text(B->name_right ? B->name_x - 8 * (int)strlen(line) : B->name_x,
                B->name_y, CINK, line);
        for (int r = 0; r < nback; r++)
          back_row_draw(r, game, sb1, sb2);
        ui_text(4, 152, CFOOT, edit ? "U/D A edit  B front" : "B front");
        if (edit) back_sel_frame(game, brow);
      }
      full = false;
    }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT |
                   KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT);
    if (k & (KEY_L | KEY_R)) { back = !back; full = true; continue; }
    if (k & KEY_B) {
      if (back) { back = false; full = true; continue; }
      return;                                /* front: exit to the save prompt */
    }
    if (!edit) continue;                     /* read-only: flip/view only */

    if (back) {                              /* ---- back page input ---- */
      if (k & (KEY_UP | KEY_DOWN)) {
        back_restore(bg, brow, game, sb1, sb2);
        brow = (k & KEY_UP) ? (brow > 0 ? brow - 1 : nback - 1) : (brow + 1) % nback;
        back_sel_frame(game, brow);
      } else if (k & KEY_A) {
        back_row_edit(&CARD_BACK_LAYOUTS[game].rows[brow], game, sb1, sb2, d1, d2);
        full = true;                         /* num_entry used the whole screen */
      }
      continue;
    }

    if (k & (KEY_UP | KEY_DOWN)) {           /* move the frame: restore + repaint */
      card_restore(bg, sel, bsel, game, sb1, name, *tid, *money, *ph, *pm);
      sel = (k & KEY_UP) ? (sel > 0 ? sel - 1 : CARDF_NUM - 1) : (sel + 1) % CARDF_NUM;
      card_sel_frame(game, sel, bsel);
    } else if ((k & (KEY_LEFT | KEY_RIGHT)) && sel == CARDF_BADGES) {
      card_restore(bg, sel, bsel, game, sb1, name, *tid, *money, *ph, *pm);
      bsel = (bsel + ((k & KEY_RIGHT) ? 1 : 7)) & 7;   /* badge cursor 0..7 */
      card_sel_frame(game, sel, bsel);
    } else if ((k & KEY_SELECT) && sel == CARDF_BADGES && game == PK_EMERALD) {
      /* the Frontier symbols aren't drawn on the card: keep the full list
       * reachable on Emerald behind SELECT (badges themselves = instant A) */
      if (flag_set_editor(sb1, game, "BADGES + FRONTIER",
                          badge_or_frontier_flag, BADGEFRONT_LBL, 22))
        *d1 = true;
      full = true;
    } else if (k & KEY_A) {                  /* edit in place (plain page's editors) */
      switch (sel) {
        case CARDF_NAME: { char b[8]; if (osk_input("TRAINER NAME", name, b, 8)) {
                          strcpy(name, b); pk_set_trainer_name(sb2, name); *d2 = true; } } break;
        case CARDF_ID: *tid = (uint16_t)num_entry("ID No", *tid, 65535);
                       *sid = (uint16_t)num_entry("SID", *sid, 65535);   /* SID rides the ID field */
                       pk_set_trainer_id(sb2, *tid, *sid); *d2 = true; break;
        case CARDF_MONEY: *money = num_entry("MONEY", *money, 999999);
                       pk_set_money(sb1, sb2, game, *money); *d1 = true; break;
        case CARDF_TIME: *ph = (uint16_t)num_entry("PLAY HOURS", *ph, 999);
                       *pm = (uint8_t)num_entry("PLAY MINUTES", *pm, 59);
                       pk_set_playtime(sb2, *ph, *pm, 0); *d2 = true; break;
        case CARDF_SEX:  /* instant flip — the card art + photo swap IS the
                          * feedback (and it stays RAM-only until B-save) */
                       *gender ^= 1; pk_set_gender(sb2, *gender); *d2 = true;
                       break;
        case CARDF_BADGES: {                 /* instant toggle of the badge under
                                              * the cursor; repaint its cell only */
          int fn = pk_badge_flag(game, bsel);
          if (fn >= 0) {
            pk_flag_set(sb1, game, fn, !pk_flag_get(sb1, game, fn)); *d1 = true;
            card_restore(bg, sel, bsel, game, sb1, name, *tid, *money, *ph, *pm);
            card_sel_frame(game, sel, bsel);
          }
        } break;
        case CARDF_STARS: if (pk_star_ach_count(game)) {
                         int m2 = stars_editor(sb1, sb2, game);
                         if (m2 & 1) *d1 = true;
                         if (m2 & 2) *d2 = true;
                       } break;
      }
      if (sel != CARDF_BADGES) full = true;  /* those editors used the whole
                                              * screen (SEX: art swap redraw) */
    }
  }
}

enum { TF_NAME, TF_SEX, TF_TID, TF_SID, TF_MONEY, TF_TIME, TF_BADGES, TF_STARS, TF_NUM };

void pdna_trainer(uint8_t* sb1, uint8_t* sb2, const Gen3SaveInfo* info, PkGame game) {
  const bool edit = app_can_edit();
  /* Seed EVERY identity field from the LIVE SaveBlock2 buffer, never from the
   * `info` snapshot: Gen3SaveInfo is parsed ONCE when the save is opened and
   * never re-parsed after a commit, while sb2 is re-read from the committed
   * image each nav-loop pass. Seeding from the stale snapshot was the "SEX
   * won't save" bug — the flip DID commit (d2 -> app_commit_sb2), but every
   * re-entry re-showed the at-load sex, and A then wrote stale^1 (usually the
   * value already on disk = a no-op), so the card could never visibly change.
   * TID/SID and PLAYTIME were worse: pk_set_trainer_id / pk_set_playtime
   * write BOTH halves, so editing one after a saved edit of the other
   * silently reverted it to the stale co-seed. Offsets: gen3_save.h SB2_OFF_*
   * (same decode as gen3_parse). */
  char name[8]; int ni;
  for (ni = 0; ni < G3_PLAYER_NAME_LEN; ni++) {
    char ch = gen3_decode_char(sb2[SB2_OFF_PLAYER_NAME + ni]);
    if (ch == 0) break;
    name[ni] = ch;
  }
  name[ni] = 0;
  uint8_t  gender = sb2[SB2_OFF_GENDER] ? 1 : 0;
  uint16_t tid = (uint16_t)(sb2[SB2_OFF_TRAINER_ID]     | (sb2[SB2_OFF_TRAINER_ID + 1] << 8));
  uint16_t sid = (uint16_t)(sb2[SB2_OFF_TRAINER_ID + 2] | (sb2[SB2_OFF_TRAINER_ID + 3] << 8));
  uint32_t money = pk_money(sb1, sb2, game);
  uint16_t ph = (uint16_t)(sb2[SB2_OFF_PLAYTIME_H] | (sb2[SB2_OFF_PLAYTIME_H + 1] << 8));
  uint8_t  pm = sb2[SB2_OFF_PLAYTIME_M];
  (void)info;                        /* kept for the call signature only */
  int  sel = 0;
  bool d1 = false, d2 = false;       /* dirty: sb1 (money) / sb2 (identity) */

  /* Any game with vendored art: the real card front IS the whole screen — the
   * cursor edits on the card itself (no plain-page fallback). Art-free builds
   * keep the plain details/edit page below, as before. */
  if (card_bg(game, 0, gender) != 0) {
    card_editor(sb1, sb2, game, edit, name, &gender, &tid, &sid,
                &money, &ph, &pm, &d1, &d2);
    trainer_commit(d1, d2);
    return;
  }

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
    lbl[TF_BADGES] = "BADGE"; siprintf(val[TF_BADGES], "%d/8%s (A)", nb,
                                       game == PK_EMERALD ? " +front" : "");   /* row budget 29 cols */
    lbl[TF_STARS] = "STARS";           /* card tier (all three games, gen3_stars) */
    if (pk_star_ach_count(game))
      siprintf(val[TF_STARS], "%d/4 (A)", pk_star_count(sb1, sb2, game, card_hoenn_dex()));
    else siprintf(val[TF_STARS], "n/a");
    for (int i = 0; i < TF_NUM; i++) {
      int y = 14 + i * 9; bool s = edit && (i == sel);
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      siprintf(line, "%-6s %s", lbl[i], val[i]);
      ui_text(6, y, s ? UI_SELTEXT : (i == TF_MONEY ? UI_OK : UI_TEXT), line);
    }

    int y = 14 + TF_NUM * 9 + 1;       /* one row denser than before: STARS fits */
    int seen, caught; bool nat; pk_pokedex(sb2, &seen, &caught, &nat);
    siprintf(line, "DEX  seen %d  caught %d%s", seen, caught, nat ? " Nat" : "");
    { char lt[40]; ui_truncate(lt, line, 29); ui_text(6, y, UI_TEXT, lt); } y += 9;

    ui_hline(4, y, 232, UI_BORDER); y += 3;
    ui_text(6, y, UI_TITLE, "ELITE FOUR / HALL OF FAME"); y += 9;
    uint16_t h; uint8_t m, s2;
    if (pk_hof_time(sb1, sb2, game, &h, &m, &s2)) {
      siprintf(line, "First cleared  %uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)s2);
      ui_text(10, y, UI_OK, line);
    } else ui_text(10, y, UI_DIM, "Not cleared yet");
    y += 9;

    ui_hline(4, y, 232, UI_BORDER); y += 3;
    ui_text(6, y, UI_TITLE, "GAME RECORDS"); y += 9;
    siprintf(line, "Steps %u", (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_STEPS));
    ui_text(10, y, UI_TEXT, line); y += 9;
    siprintf(line, "Battles %u (w%u/t%u)",
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TOTAL_BATTLES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_WILD_BATTLES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_TRAINER_BATTLES));
    { char lt[40]; ui_truncate(lt, line, 28); ui_text(10, y, UI_TEXT, lt); } y += 9;
    siprintf(line, "Captures %u  Eggs %u",
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_POKEMON_CAPTURES),
             (unsigned)pk_game_stat(sb1, sb2, game, PK_STAT_HATCHED_EGGS));
    { char lt[40]; ui_truncate(lt, line, 28); ui_text(10, y, UI_TEXT, lt); }

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, edit ? "U/D field  A edit  B save" : "B back");

    if (!edit) { do { s_vsync(); } while (!key_hit(KEY_B)); snd_back(); return; }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) {
      trainer_commit(d1, d2);
      return;
    } else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : TF_NUM - 1;
    else if (k & KEY_DOWN)   sel = (sel + 1) % TF_NUM;
    else if (k & KEY_A) {
      switch (sel) {
        case TF_NAME: { char b[8]; if (osk_input("TRAINER NAME", name, b, 8)) {
                          strcpy(name, b); pk_set_trainer_name(sb2, name); d2 = true; } } break;
        case TF_SEX:   /* confirmed toggle: a single stray A here used to flip the
                        * live save's gender silently (male bag/card ever after). */
                       if (app_confirm("Change trainer SEX?",
                                       gender ? "Female -> Male" : "Male -> Female")) {
                         gender ^= 1; pk_set_gender(sb2, gender); d2 = true;
                       } break;
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
        case TF_STARS: if (pk_star_ach_count(game)) {
                         int m2 = stars_editor(sb1, sb2, game);
                         if (m2 & 1) d1 = true;
                         if (m2 & 2) d2 = true;
                       } break;
      }
    }
  }
}
