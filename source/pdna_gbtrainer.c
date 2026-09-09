/*
 * Gen-1/2 trainer card -- BACKLOG #49 P1b (plain row-list fallback) + P1c
 * (UX-parity: the SAME Gen-3 card look, source/pdna_trainer.c's card_editor()
 * over Emerald's card_bg()/CARD_LAYOUTS art -- P1c's brief, "make the Gen-1/2
 * trainer card LOOK like the Gen-3 card").
 *
 * NO CARD ART OF ITS OWN: Gen 1/2 have no card front table in the ROM by
 * shape the way Emerald's card_bg.h does, so this screen always paints on
 * CARD_LAYOUTS[PK_EMERALD] / CARD_BACK_LAYOUTS[PK_EMERALD] -- the SAME
 * background pixels the Emerald save's own trainer card uses (card_bg(),
 * pdna_trainer.h's shared painters). Why Emerald and not RS/FRLG: Emerald's
 * back page has exactly 6 row slots, which is exactly what GB's fullest back
 * list (Crystal: COINS/MOM$/MOM SAVE/RIVAL/MOTHER/GENDER) needs (see
 * gbtr_rows.c's own comment); Emerald's front badge row is also the one with
 * baked room for a full 8 cells at a fixed pitch (badge_x + 24*i) that this
 * screen's 8-badge front row reuses unmodified.
 *
 * Front card (art-degraded to the plain row-list page below when card_bg()
 * returns no art -- an artless build with no Gen-3 ROM open, mirroring
 * pdna_trainer()'s own card_bg(game,...).blob != 0 fork exactly): NAME,
 * ID No., MONEY, PLAY TIME, BADGES (Johto 8 / Gen-1's 8, as icons on the
 * card's own badge row) are the CARDF_* cursor; U/D moves it, A edits in
 * place with the SAME sub-editors P1b already had (num_entry / osk_input /
 * gbtr_badges_editor / gbtr_time_editor); the photo/SEX slot shows a neutral
 * placeholder box (card_front_fields_paint's own `photo=false` case -- no GB
 * trainer-sprite locator exists yet, BACKLOG note). L/R flips to the BACK
 * page (card_back_name_paint + gbtr_build_back_rows' own per-game row list:
 * COINS always; MOM'S MONEY + MOM SAVE MODE if Gen 2; RIVAL always; MOTHER if
 * Gen 2; and EITHER Kanto badges (Gold/Silver) OR GENDER (Crystal only, view-
 * only) -- gbtr_rows.c's own comment explains why not both fit).
 *
 * Cursor/edit semantics: edits are staged in a LOCAL GbTrainer (gbt_read at
 * entry, mutated in place by the row editors) and committed in one
 * gbt_write() batch; a no-op commit (nothing touched, t == t0) returns
 * silently with no popup; a real edit asks "Save trainer changes?" before
 * gbt_write runs, and any failure rolls the whole image back (gb_rollback)
 * rather than leaving a partial edit -- unchanged since P1b. The KEY that
 * asks for that commit differs by page: the plain row-list fallback (no card
 * art) still uses START, unchanged from P1b/P1c. The card page (P1c's
 * Gen-3-look front/back) was changed in P1d to match pdna_trainer.c's own
 * card_editor() EXACTLY -- there is no START on the card at all; B on the
 * FRONT page always asks for a commit (the caller's memcmp is the no-op
 * check), same as Gen 3's card; B on the BACK page flips back to the front.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbtrainer.h"
#include "gb_trainer.h"
#include "gbtr_rows.h"     /* pure-C row-visibility model, host-tested separately  */
#include "pdna_trainer.h"  /* num_entry / trainer_row_paint / trainer_flag_row_paint /
                            * trainer_key_legend / card_* shared card painters (P1c) */
#include "card_bg.h"       /* CARD_LAYOUTS / CARD_BACK_LAYOUTS / card_bg() -- always
                            * PK_EMERALD's, see the file comment above for why       */
#include "pdna_gen12.h"    /* gb_rollback / gb_persist -- the S2 commit primitives;
                            * gb12_arena_tail/gb12_arena_tail_release (U2c)          */
#include "pdna_gbscreen.h" /* U2c: the shared GB-screen shell -- Red's own card      */
#include "pdna_origin_art.h" /* PDNA_GEN1 -- gbscr_open()'s own `gen` constant        */
#include "pdna_layout.h"   /* PDNA_GBTRAINER_ID_WARN_* -- measured by host_textfit   */
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "pdna_app.h"      /* msg_wait */

/* P1d: same red as pdna_trainer.c's own CSEL (its file-local #define, not
 * exported) -- kept in sync by eye since this file's own selection frame for
 * CARDF_NAME (gbcard_front_full below) has to draw itself, not go through
 * the shared card_field_sel_frame(). */
#define GBCARD_CSEL RGB15(26, 4, 3)

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

static bool gbtr_badge_get(const GbTrainer* t, bool gen1, int i) {
  if (gen1) return ((t->badges >> i) & 1u) != 0;
  return i < 8 ? ((t->badges_johto >> i) & 1u) != 0
               : ((t->badges_kanto >> (i - 8)) & 1u) != 0;
}

static void gbtr_badge_toggle(GbTrainer* t, bool gen1, int i) {
  if (gen1) { t->badges = (uint8_t)(t->badges ^ (1u << i)); return; }
  if (i < 8) t->badges_johto = (uint8_t)(t->badges_johto ^ (1u << i));
  else       t->badges_kanto = (uint8_t)(t->badges_kanto ^ (1u << (i - 8)));
}

/* One-hot cycle off -> some -> half -> all -> off. The real games do not promise
 * these three bits are mutually exclusive (gb_trainer.h's own comment), but a
 * one-hot cycle is the only sane UI over three independent toggles the player
 * never sees as anything but "how much is Mom saving". mom_active is left alone
 * here (view-only reflection of whatever gbt_read found) -- gbt_write's own
 * read-modify-write preserves it exactly, so never touching it in the UI never
 * risks writing a value the user did not choose. */
static uint8_t gbtr_cycle_mom_saving(uint8_t bits) {
  switch (bits & 0x07u) {
    case 0x00u: return 0x01u;
    case 0x01u: return 0x02u;
    case 0x02u: return 0x04u;
    default:    return 0x00u;
  }
}

/* Badge toggle screen: same look as pdna_trainer.c's flag_set_editor (a scrolling
 * list of trainer_flag_row_paint rows), over this LOCAL GbTrainer's own badge bytes
 * rather than PkGame/pk_flag_get -- nothing here touches the session until the
 * caller's own commit (plain page: START; card front: B, P1d).
 * Reached from BOTH the plain page's BADGES row (A, unchanged from P1b) AND the
 * card front's BADGES field (SELECT, Gen 2 only -- the card's own badge row shows
 * only the 8 Johto icons; this is where Kanto's 8 stay individually toggleable,
 * mirroring pdna_trainer.c's own Emerald-frontier-behind-SELECT precedent). */
__attribute__((noinline))
static void gbtr_badges_editor(GbTrainer* t, bool gen1) {
  static const char* const G1_LBL[8] = {
    "Badge 1", "Badge 2", "Badge 3", "Badge 4", "Badge 5", "Badge 6", "Badge 7", "Badge 8",
  };
  static const char* const G2_LBL[16] = {
    "Johto 1", "Johto 2", "Johto 3", "Johto 4", "Johto 5", "Johto 6", "Johto 7", "Johto 8",
    "Kanto 1", "Kanto 2", "Kanto 3", "Kanto 4", "Kanto 5", "Kanto 6", "Kanto 7", "Kanto 8",
  };
  const char* const* lbl = gen1 ? G1_LBL : G2_LBL;
  const int n = gen1 ? 8 : 16;
  const int vis = 12;
  int sel = 0, top = 0;

  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;

    ui_clear();
    ui_text(4, 2, UI_TITLE, "BADGES");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < vis && top + i < n; i++) {
      int idx = top + i;
      trainer_flag_row_paint(lbl[idx], gbtr_badge_get(t, gen1, idx), 16 + i * 9, idx == sel);
    }
    trainer_key_legend("A toggle  U/D  B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A)    gbtr_badge_toggle(t, gen1, sel);
  }
}

/* Play-time sub-editor: hours/minutes/seconds via num_entry, same idiom as Gen 3's
 * TF_TIME case (pdna_trainer.c). `maxed` is not directly editable -- gbt_write
 * derives it itself from an hours write that actually overflows, exactly the way
 * the real game's own play_time.asm does (gb_trainer.c's own comment). P1c: the
 * card's own TIME field has no room for a "(max)" suffix (the shared painter's
 * format is a fixed "%u:%02u", same as Gen 3's own card) -- `maxed` stays fully
 * functional (the hours cap here is unchanged) but is not flagged on the card
 * face itself; BACKLOG note for a future exact-parity indicator. */
__attribute__((noinline))
static void gbtr_time_editor(GbTrainer* t, bool gen1) {
  uint32_t maxh = gen1 ? 255u : 999u;
  t->playtime.hours   = (uint16_t)num_entry("PLAY HOURS", t->playtime.hours, maxh);
  t->playtime.minutes = (uint8_t)num_entry("PLAY MINUTES", t->playtime.minutes, 59u);
  t->playtime.seconds = (uint8_t)num_entry("PLAY SECONDS", t->playtime.seconds, 59u);
}

/* Gen 1/2 disobedience/EXP-boost checks compare the mon's own OT trainer ID against
 * the PLAYER's live trainer ID (pokered battle/core.asm:3839-3856 disobedience;
 * pokecrystal core.asm:7084-7096 EXP boost) -- exactly the "is this still my mon"
 * test Gen 3's record mixing runs (pdna_trainer.c's id_edit_ok comment). Editing NAME
 * or ID here changes what that test compares against, so every mon already caught
 * under the old identity becomes foreign the moment this card's edit lands. Warn
 * once per visit, before the first such edit -- same idiom as pdna_trainer.c's
 * id_edit_ok/s_id_warned. */
static bool s_id_warned;

static bool gbtr_id_edit_ok(void) {
  if (s_id_warned) return true;
  s_id_warned = true;
  return app_confirm(PDNA_GBTRAINER_ID_WARN_TITLE, PDNA_GBTRAINER_ID_WARN_L1);
}

/* ============================================================================
 * ---- the plain row-list page (P1b, unchanged) -- the artless-build fallback
 * when card_bg(PK_EMERALD, ...).blob == 0 (no ROM open that can serve Emerald's
 * card chrome). See gbtr_build_rows() (gbtr_rows.c) for the flat row list this
 * draws -- the ORIGINAL P1b order, kept exactly as it shipped.
 * ============================================================================ */

static void gbtr_row_paint(const GbTrainer* t, int row, bool gen1, int y, bool sel) {
  const char* lbl; char val[40]; uint16_t ink = UI_TEXT;
  switch (row) {
    case GBTR_NAME:  lbl = "NAME";  siprintf(val, "%s", t->name); break;
    case GBTR_ID:    lbl = "ID No"; siprintf(val, "%05u", (unsigned)t->trainer_id); break;
    case GBTR_MONEY:
      lbl = "MONEY";
      if (t->money_ok) { siprintf(val, "$%lu", (unsigned long)t->money); ink = UI_OK; }
      else             { siprintf(val, "?"); ink = UI_DIM; }
      break;
    case GBTR_COINS:
      lbl = "COINS";
      if (t->coins_ok) siprintf(val, "%u", (unsigned)t->coins);
      else            { siprintf(val, "?"); ink = UI_DIM; }
      break;
    case GBTR_MOMMONEY:
      lbl = "MOM $"; siprintf(val, "$%lu", (unsigned long)t->moms_money);
      break;
    case GBTR_MOMSAVE: {
      lbl = "SAVE";       /* "MOM SAVE" (8 chars) broke the "%-6s %s" label column (D7) */
      const char* m = (t->mom_saving_bits & 0x04u) ? "all"
                    : (t->mom_saving_bits & 0x02u) ? "half"
                    : (t->mom_saving_bits & 0x01u) ? "some" : "off";
      siprintf(val, "%s%s", m, t->mom_active ? "" : " (inactive)");
    } break;
    case GBTR_BADGES: {
      lbl = "BADGE";
      int nb = 0, total = gen1 ? 8 : 16;
      if (gen1) { for (int i = 0; i < 8; i++) if ((t->badges >> i) & 1u) nb++; }
      else {
        for (int i = 0; i < 8; i++) if ((t->badges_johto >> i) & 1u) nb++;
        for (int i = 0; i < 8; i++) if ((t->badges_kanto >> i) & 1u) nb++;
      }
      siprintf(val, "%d/%d (A)", nb, total);
    } break;
    case GBTR_TIME:
      lbl = "TIME";
      siprintf(val, "%uh %02um %02us%s", (unsigned)t->playtime.hours,
              (unsigned)t->playtime.minutes, (unsigned)t->playtime.seconds,
              t->playtime.maxed ? " (max)" : "");
      break;
    case GBTR_GENDER:
      lbl = "GENDER"; ink = UI_DIM; siprintf(val, "%s", t->gender ? "Female" : "Male");
      break;
    case GBTR_DEX:
      lbl = "DEX"; ink = UI_DIM;
      siprintf(val, "seen %u owned %u", (unsigned)t->dex_seen, (unsigned)t->dex_owned);
      break;
    case GBTR_RIVAL:
      lbl = "RIVAL"; ink = UI_DIM; siprintf(val, "%s", t->rival_name);
      break;
    case GBTR_MOTHER:
      lbl = "MOTHER"; ink = UI_DIM; siprintf(val, "%s", t->mothers_name);
      break;
    default: lbl = ""; val[0] = 0;
  }
  /* D10: NAME/RIVAL/MOTHER can carry a UTF-8 glyph (e-acute, or a Male/Female sign
   * on a rival/mom name) that trainer_row_paint's sys8 face (ui_text, fixed-width
   * ASCII cells) cannot draw -- each byte of a multi-byte sequence would print as
   * its own wrong glyph. Route just the value through ui_ptext instead: it already
   * decodes the e-acute sequence and degrades any other non-ASCII run to a single
   * '?' (source/ui.c's pnext), never garbage tiles. */
  if (row == GBTR_NAME || row == GBTR_RIVAL || row == GBTR_MOTHER) {
    if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    else     ui_fill_rect(2, y - 1, 236, 9, UI_BG);
    char lbl6[8]; siprintf(lbl6, "%-6s", lbl);
    ui_text(6, y, sel ? UI_SELTEXT : UI_TEXT, lbl6);
    ui_ptext(6 + 7 * UI_SYS8_W, y, sel ? UI_SELTEXT : ink, val);
    return;
  }
  trainer_row_paint(y, sel, lbl, val, ink);
}

typedef struct { uint32_t gen; int sel; bool valid; } GbtrPaint;

/* `header` (may be NULL): U2c's own honest-fallback line (design sec 3.5), shown
 * INSTEAD of the ordinary title when this page is reached because the GB-screen
 * shell refused (e.g. "GB ART: OFF -- no ROM registered") -- never silently
 * degraded, per the design's own rule. NULL keeps the ordinary title, unchanged
 * from P1b/P1c (Gen 2's own art-unavailable fallback, and any other caller). */
static void gbtr_plain_render(const GbTrainer* t, bool gen1, const int* rows, int nrows,
                              bool can_edit, int sel, GbtrPaint* pv, const char* header) {
  bool full = !pv->valid || pv->gen != ui_clear_gen();

  if (full) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, header ? header : (gen1 ? "TRAINER CARD (Gen 1)" : "TRAINER CARD (Gen 2)"));
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < nrows; i++)
      gbtr_row_paint(t, rows[i], gen1, 14 + i * 9, can_edit && i == sel);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    trainer_key_legend(can_edit ? "A edit  START save  B cancel" : "B back");
  } else if (sel != pv->sel) {
    gbtr_row_paint(t, rows[pv->sel], gen1, 14 + pv->sel * 9, false);
    gbtr_row_paint(t, rows[sel],     gen1, 14 + sel     * 9, can_edit);
  }

  pv->sel = sel; pv->gen = ui_clear_gen(); pv->valid = true;
}

/* One field's A-edit: shared by the plain page AND the card front (P1c) -- both
 * mutate the same LOCAL `t`/`t0` via the same sub-editors, so this is the one
 * place that logic lives. Returns nothing; every case is self-contained (some
 * open a whole-screen sub-editor and the caller is expected to force a full
 * repaint afterward, same as before). */
static void gbtr_edit_row(GbTrainer* t, bool gen1, int kind) {
  switch (kind) {
    case GBTR_NAME: {
      if (!gbtr_id_edit_ok()) break;
      char b[GB_TEXT_MAX];        /* t.name is char[GB_TEXT_MAX]; UTF-8 decoded,
                                    * a truncated buffer here silently drops glyphs
                                    * (P1b review D1) */
      if (osk_input("TRAINER NAME", t->name, b, sizeof b)) strcpy(t->name, b);
    } break;
    case GBTR_ID:
      if (!gbtr_id_edit_ok()) break;
      t->trainer_id = (uint16_t)num_entry("ID No", t->trainer_id, 65535u);
      break;
    case GBTR_MONEY:
      if (!t->money_ok) {
        msg_wait("UNREADABLE", UI_WARN, "This field is not a valid", "number and cannot be edited.");
        break;
      }
      t->money = num_entry("MONEY", t->money, 999999u);
      break;
    case GBTR_COINS:
      if (!t->coins_ok) {
        msg_wait("UNREADABLE", UI_WARN, "This field is not a valid", "number and cannot be edited.");
        break;
      }
      t->coins = (uint16_t)num_entry("COINS", t->coins, 9999u);
      break;
    case GBTR_MOMMONEY:
      t->moms_money = num_entry("MOM'S MONEY", t->moms_money, 999999u);
      break;
    case GBTR_MOMSAVE:
      t->mom_saving_bits = gbtr_cycle_mom_saving(t->mom_saving_bits);
      break;
    case GBTR_BADGES:
      gbtr_badges_editor(t, gen1);
      break;
    case GBTR_TIME:
      gbtr_time_editor(t, gen1);
      break;
    default: break;   /* GENDER / DEX / RIVAL / MOTHER / KANTOBADGES(view via
                       * the badge sub-editor only): view-only */
  }
}

/* Returns true only when START asked for a commit -- B always returns false
 * (P1b's own "B always discards" rule), REGARDLESS of whether `t` is dirty, so
 * the caller never mistakes a discarded edit for a commit request. The confirm
 * dialog + the memcmp no-op check + the actual gbt_write all moved to the ONE
 * call site in pdna_gbtrainer() below (matches pdna_gbtrainer_card()'s own
 * *want_commit contract) -- this used to ask+write here directly (P1b), but
 * P1c needs the SAME commit path for both faces of the card, not two. */
__attribute__((noinline))
static bool pdna_gbtrainer_plain(GbTrainer* t, bool gen1, bool can_edit, const char* header) {
  int rows[GBTR_ROW_MAX];
  int nrows = gbtr_build_rows(t, rows);

  int sel = 0;
  GbtrPaint pv = { 0, 0, false };
  for (;;) {
    gbtr_plain_render(t, gen1, rows, nrows, can_edit, sel, &pv, header);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return false;                  /* discard: `t` was never written */
    if (k & KEY_START) { if (can_edit) return true; continue; }

    if (k & KEY_UP)        sel = (sel > 0) ? sel - 1 : nrows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % nrows;
    else if ((k & KEY_A) && can_edit) gbtr_edit_row(t, gen1, rows[sel]);
  }
}

/* ============================================================================
 * ---- the card FRONT + BACK (P1c) -- CARD_LAYOUTS[PK_EMERALD] / CARD_BACK_
 * LAYOUTS[PK_EMERALD], via pdna_trainer.h's shared painters. See this file's
 * own top comment for the row lists.
 * ============================================================================ */

/* CARDF_* cell for a front-list GBTR_* kind -- the front list is always
 * {NAME, ID, MONEY, TIME, BADGES} (gbtr_build_front_rows), but this keeps the
 * on-card CURSOR order tied to CARD_LAYOUTS' own field geometry rather than a
 * second hardcoded array. */
static int cardf_for_front(int kind) {
  switch (kind) {
    case GBTR_ID:    return CARDF_ID;
    case GBTR_MONEY: return CARDF_MONEY;
    case GBTR_TIME:  return CARDF_TIME;
    case GBTR_BADGES:return CARDF_BADGES;
    default:         return CARDF_NAME;
  }
}

static void gb_cardfields(const GbTrainer* t, bool gen1, CardFields* cf) {
  memset(cf, 0, sizeof *cf);
  cf->name = t->name;
  cf->id = t->trainer_id;
  /* P1d: closes the P1c gap noted here -- CardFields now has its own
   * money_unknown flag, so the card face shows "?" the same way the plain
   * page does on a Gen-1 BCD decode failure. Editing stays refused either
   * way (gbtr_edit_row's own money_ok check, unchanged from P1b). */
  cf->money = t->money_ok ? t->money : 0;
  cf->money_unknown = !t->money_ok;
  cf->play_h = t->playtime.hours;
  cf->play_m = t->playtime.minutes;
  for (int i = 0; i < 8; i++) if (gbtr_badge_get(t, gen1, i)) cf->badges |= (uint16_t)(1u << i);
  cf->has_dex = true;
  cf->dex_caught = t->dex_owned;
  cf->photo = false;              /* neutral placeholder: no GB trainer-sprite locator */
}

/* Full front repaint: card blit + every field + the DEX row + photo placeholder
 * (card_front_fields_paint), then the footer + selection frame. Mirrors
 * pdna_trainer.c's card_editor()'s own `full && !back` branch. */
static void gbcard_front_full(const GbTrainer* t, bool gen1, int female,
                              const int* cardf, int nfront, int sel, bool can_edit) {
  BgFrame bg = card_bg(PK_EMERALD, 0, female);
  bg_restore(bg, 0, 0, CARD_BG_W, CARD_BG_H);
  CardFields cf; gb_cardfields(t, gen1, &cf);
  card_front_fields_paint(PK_EMERALD, &cf);
  /* P1d: matches pdna_trainer.c's card_editor() footer VERBATIM (no START key
   * on this card at all -- Gen 3's own card_editor has none either; B on the
   * front now exits to the caller's one save prompt, same as Gen 3). */
  ui_text(4, 152, UI_TEXT, can_edit ? "U/D A edit  L/R flip  B save"
                                      : "L/R flip  B back");
  if (can_edit) {
    if (cardf[sel] == CARDF_NAME) {
      /* P1d: CARD_LAYOUTS' baked NAME rect (card_bg.h:74, Emerald 110px =
       * "NAME: " + 7 chars) is sized for a Gen-3 name, but a GB name that
       * decodes a <PK>/<MN> digraph is 8 chars (Gold.sav "MattiaPK") -- the
       * text still renders in full (card_front_fields_paint's own siprintf
       * has no truncation) but the shared card_field_sel_frame() would stop
       * the red frame short of it. Recompute the frame's own width from the
       * ACTUAL name length here, with the same two m3_frame calls
       * card_sel_frame() uses in pdna_trainer.c -- never through the shared
       * Gen-3 path, which always trusts the baked rect. */
      int x, y, w, h;
      card_field_rect(PK_EMERALD, CARDF_NAME, 0, &x, &y, &w, &h);
      int need = 8 * (6 + (int)strlen(cf.name));
      if (need > w) w = need;
      m3_frame(x, y, x + w, y + h, GBCARD_CSEL);
      m3_frame(x + 1, y + 1, x + w - 1, y + h - 1, GBCARD_CSEL);
    } else {
      card_field_sel_frame(PK_EMERALD, cardf[sel], 0);
    }
  }
}

static void gbcard_back_full(const GbTrainer* t, bool gen1, int female,
                             const int* back_rows, int nback, int row, bool can_edit) {
  BgFrame bg = card_bg_back(PK_EMERALD, 0, female);
  bg_restore(bg, 0, 0, CARD_BG_W, CARD_BG_H);
  card_back_name_paint(PK_EMERALD, t->name);
  char val[24];
  for (int r = 0; r < nback; r++) {
    const char* lbl;
    switch (back_rows[r]) {
      case GBTR_COINS:
        lbl = "COINS";
        if (t->coins_ok) siprintf(val, "%u", (unsigned)t->coins); else siprintf(val, "?");
        break;
      case GBTR_MOMMONEY: lbl = "MOM'S MONEY"; siprintf(val, "$%lu", (unsigned long)t->moms_money); break;
      case GBTR_MOMSAVE: {
        lbl = "MOM SAVE MODE";
        const char* m = (t->mom_saving_bits & 0x04u) ? "all"
                      : (t->mom_saving_bits & 0x02u) ? "half"
                      : (t->mom_saving_bits & 0x01u) ? "some" : "off";
        siprintf(val, "%s%s", m, t->mom_active ? "" : " (inactive)");
      } break;
      case GBTR_KANTOBADGES: {
        lbl = "KANTO BADGES";
        int nb = 0; for (int i = 0; i < 8; i++) if ((t->badges_kanto >> i) & 1u) nb++;
        siprintf(val, "%d/8 (SELECT)", nb);
      } break;
      case GBTR_RIVAL: lbl = "RIVAL"; siprintf(val, "%s", t->rival_name); break;
      case GBTR_MOTHER: lbl = "MOTHER"; siprintf(val, "%s", t->mothers_name); break;
      case GBTR_GENDER: lbl = "GENDER"; siprintf(val, "%s", t->gender ? "Female" : "Male"); break;
      default: lbl = ""; val[0] = 0;
    }
    card_back_row_paint(PK_EMERALD, r, lbl, val);
  }
  ui_text(4, 152, UI_TEXT, can_edit ? "U/D A edit  B front" : "B front");
  if (can_edit) card_back_sel_frame(PK_EMERALD, row);
}

/* The card's own editor loop (P1c/P1d): SAME shape as pdna_trainer.c's
 * card_editor -- a red selection frame walks the fields ON the card (front)
 * or the row list (back); A edits in place with the same sub-editors
 * gbtr_edit_row above already shares with the plain page. Reads `t`/`t0`/
 * `gen1`/`can_edit` from the caller. There is NO START key on this card, same
 * as Gen 3's own card_editor: B on the front is the one exit, and it always
 * asks the caller for a commit (`*want_commit = true`) -- the no-op case is
 * a silent memcmp no-op at the caller, not a discard here. noinline so this
 * function's own (larger) locals never land in pdna_gbtrainer()'s frame. */
__attribute__((noinline))
static void pdna_gbtrainer_card(GbTrainer* t, bool gen1, bool can_edit,
                                bool* want_commit) {
  *want_commit = false;
  int female = t->has_gender ? t->gender : 0;

  int front_gbtr[GBTR_ROW_MAX], front_cardf[GBTR_ROW_MAX];
  int nfront = gbtr_build_front_rows(t, front_gbtr);
  for (int i = 0; i < nfront; i++) front_cardf[i] = cardf_for_front(front_gbtr[i]);

  int back_rows[GBTR_ROW_MAX];
  int nback = gbtr_build_back_rows(t, back_rows);

  int sel = 0, bsel = 0, brow = 0;
  bool back = false, full = true;

  for (;;) {
    if (full) {
      if (!back) gbcard_front_full(t, gen1, female, front_cardf, nfront, sel, can_edit);
      else       gbcard_back_full(t, gen1, female, back_rows, nback, brow, can_edit);
      full = false;
    }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT |
                   KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT);
    if (k & (KEY_L | KEY_R)) { back = !back; full = true; continue; }

    if (k & KEY_B) {
      if (back) { back = false; full = true; continue; }
      *want_commit = true;                        /* front: same as Gen 3's card_editor --
                                                    * B exits to the caller's save prompt */
      return;
    }
    if (!can_edit) continue;                      /* read-only: flip/view only */

    if (back) {                                  /* ---- back page input ---- */
      if (k & (KEY_UP | KEY_DOWN)) {
        brow = (k & KEY_UP) ? (brow > 0 ? brow - 1 : nback - 1) : (brow + 1) % nback;
        full = true;                              /* re-derive the row list is cheap;
                                                    * a single-row restore needs the
                                                    * SAME label/value strings this
                                                    * frame already built once, so a
                                                    * full repaint is the simplest
                                                    * correct choice here (back page
                                                    * moves are rare next to front-row
                                                    * cursor moves) */
      } else if (k & KEY_A) {
        gbtr_edit_row(t, gen1, back_rows[brow]);
        full = true;
      }
      continue;
    }

    if (k & (KEY_UP | KEY_DOWN)) {                /* ---- front page input ---- */
      sel = (k & KEY_UP) ? (sel > 0 ? sel - 1 : nfront - 1) : (sel + 1) % nfront;
      full = true;                                /* same reasoning as the back page:
                                                    * a field-restore needs a fresh
                                                    * CardFields anyway, so this stays
                                                    * a full repaint rather than a
                                                    * second bespoke restore path */
    } else if ((k & (KEY_LEFT | KEY_RIGHT)) && front_cardf[sel] == CARDF_BADGES) {
      bsel = (bsel + ((k & KEY_RIGHT) ? 1 : 7)) & 7;
      full = true;
    } else if ((k & KEY_SELECT) && front_cardf[sel] == CARDF_BADGES && !gen1) {
      /* Kanto's 8 badges aren't drawn on the card (only 8 slots exist) -- the
       * full 16-row toggle screen (unchanged from P1b) stays reachable here,
       * mirroring pdna_trainer.c's own Emerald-frontier-behind-SELECT case. */
      gbtr_badges_editor(t, gen1);
      full = true;
    } else if (k & KEY_A) {
      if (front_cardf[sel] == CARDF_BADGES) {
        gbtr_badge_toggle(t, gen1, bsel);          /* instant, same as Gen 3's card */
        full = true;
      } else {
        gbtr_edit_row(t, gen1, front_gbtr[sel]);
        full = true;
      }
    }
  }
}

/* ============================================================================
 * ---- U2c: Red's OWN trainer card, over the shared GB-screen shell -- Gen 1
 * ONLY (docs/GB-GAME-SCREENS-DESIGN.md sec 1.1). Retires the Emerald-art card
 * (gbcard_front_full/gbcard_back_full/pdna_gbtrainer_card/gb_cardfields, all
 * above, UNCHANGED) for Gen-1 saves -- Gen 2 still reaches them, unaffected.
 * ============================================================================ */

/* CARDFRAME block tile indices (docs/GB-GAME-SCREENS-DESIGN.md sec 2.1 G1-C:
 * "9 + 22 + 1 + 8 tiles" contiguous, in the gfx/trainer_card.asm file order --
 * trainer_info(9), blank_leader_names(22), circle_tile(1), badge_numbers(8)).
 * The design table names 7 of the 9 frame roles by their VRAM id ($77-based --
 * DrawTrainerInfo loads tile 0 of this block to vChars2 $77, so ROM index =
 * VRAM id - $77): $78 right(1), $79 UL(2), $7a top(3), $7b UR(4), $7c left(5),
 * $7d BL(6), $7e BR(7). It does not separately name a "bottom edge" tile --
 * index 0 ($77 itself, the block's own first tile) is the only one of the 9
 * left over once the 7 named roles are assigned indices 1-7, so this is used
 * as the bottom edge here (index 8, $7f, stays unused -- 9 tiles reserved, 8
 * roles filled). Flagged for the item-2 shot comparison against the real ROM,
 * not proven by a decomp opcode the way the block's own OWN offset is. */
enum {
  G1F_BOTTOM = 0, G1F_RIGHT = 1, G1F_UL = 2, G1F_TOP = 3,
  G1F_UR = 4, G1F_LEFT = 5, G1F_BL = 6, G1F_BR = 7,
  /* D4 (review): the real ROM's TrainerInfo_DrawVerticalLine draws the
   * screen-background dither pattern for the two side rules between the
   * upper and lower panel ($d7 = the CARDFRAME block's own index 8 by the
   * same "block's first tile past the 7 named roles" reading G1F_BOTTOM
   * uses -- confirmed against the real tilemap capture, celldiff.py) --
   * NOT a TEXTBOX tile, and not one row tall: it is drawn per source row
   * for the full 8-row gap (screen rows 10..17). */
  G1F_BG = 8,
  G1F_CIRCLE = 31,            /* the block's 32nd tile: 9 frame + 22 blank names */
  G1F_BADGENUM0 = 32          /* badge k's number tile = G1F_BADGENUM0 + k       */
};

/* TEXTBOX block (G1-T, 32 tiles): the design names tile 13 as the ':' colon
 * ("TextBoxGraphics + 13 tiles"). G1T_VRULE (index 14, "one past the colon")
 * was this screen's own guess at the side-rule tile before the real tilemap
 * capture -- wrong (see G1F_BG above) and now unused; deleted. */
enum { G1T_COLON = 13 };

/* Cursor slots, in UP/DOWN cycle order: NAME, MONEY, TIME, then the 8 badges
 * row-major (0-3 top row, 4-7 bottom row) -- matches the card's own on-screen
 * order top to bottom. */
enum { G1C_NAME = 0, G1C_MONEY, G1C_TIME, G1C_BADGE0, G1C_SEL_MAX = G1C_BADGE0 + 8 };

/* Draws one bordered panel (design sec 1.1's own two TrainerInfo_DrawTextBox
 * calls): corners + edges from CARDFRAME; the interior is left BLANK (the
 * design's own rule -- the game's interior fill tile, vChars1 $57, is outside
 * this block and this shell has no located source for it, so a flat colour
 * stands in, same posture as every other "no ROM source for this exact
 * pixel" case in the tree). `x1`/`y1` are inclusive (last border column/row,
 * NOT one-past, matching the design table's own "(0,0)-(19,7)" notation). */
static void g1card_panel(GbScreen* gs, int x0, int y0, int x1, int y1) {
  gbscr_cell(gs, x0, y0, GBSCR_SRC_CARDFRAME, G1F_UL);
  gbscr_cell(gs, x1, y0, GBSCR_SRC_CARDFRAME, G1F_UR);
  gbscr_cell(gs, x0, y1, GBSCR_SRC_CARDFRAME, G1F_BL);
  gbscr_cell(gs, x1, y1, GBSCR_SRC_CARDFRAME, G1F_BR);
  for (int x = x0 + 1; x < x1; x++) {
    gbscr_cell(gs, x, y0, GBSCR_SRC_CARDFRAME, G1F_TOP);
    gbscr_cell(gs, x, y1, GBSCR_SRC_CARDFRAME, G1F_BOTTOM);
  }
  for (int y = y0 + 1; y < y1; y++) {
    gbscr_cell(gs, x0, y, GBSCR_SRC_CARDFRAME, G1F_LEFT);
    gbscr_cell(gs, x1, y, GBSCR_SRC_CARDFRAME, G1F_RIGHT);
  }
}

/* One full-card repaint -- gbscr_cell()/gbscr_text() are all idempotent (a
 * cell that already holds this exact tile is never marked dirty), so calling
 * this every time ANYTHING changes (a field edit, or just forcing a redraw to
 * erase the previous cursor frame) is cheap: gbscr_flush() only re-blits what
 * actually changed, and gbscr_mark_all_dirty() (called by the cursor-move path
 * below, not here) forces a full re-blit when only the cursor moved. */
static void g1card_paint(GbScreen* gs, const GbTrainer* t) {
  g1card_panel(gs, 0, 0, 19, 7);                        /* upper panel */

  gbscr_text(gs, 2, 2, "NAME/");
  gbscr_raw(gs, 7, 2, t->name_raw, GB_OT_GLYPHS);

  /* D5 (review): the real PrintBCDNumber call uses LEADING_ZEROES|LEFT_ALIGN
   * with MONEY_SIGN, which places the sign right before the first
   * significant digit and prints NO leading zeros (money 0 -> "$0", not
   * "$000000") -- "$%06lu" was this screen's own wrong guess. Cursor rect
   * stays the fixed 7-wide field (sel_rect, unchanged); blank the residue
   * cells past the shorter string so a later edit that SHRINKS the digit
   * count doesn't leave stale digits from a longer previous value (same
   * defect class as D2's name-field terminator fix, D6's time fix below). */
  gbscr_text(gs, 2, 4, "MONEY/");
  char buf[16];
  if (t->money_ok) siprintf(buf, "$%lu", (unsigned long)t->money);
  else             siprintf(buf, "?");
  gbscr_text(gs, 8, 4, buf);
  for (int cx = 8 + (int)strlen(buf); cx < 15; cx++) gbscr_cell(gs, cx, 4, GBSCR_SRC_BLANK, 0);

  /* D6 (review): the real screen left-aligns hours and writes the colon
   * wherever it lands (no fixed col-12 colon, no zero/space-padded hours) --
   * this screen's own "%3u" + fixed-column colon was wrong. Blank the
   * residue out to column 14 (the field's own max width, sel_rect w=6 from
   * x=9) so a later edit that shrinks the hour count doesn't leave a stale
   * colon or stale minute digits behind. */
  gbscr_text(gs, 2, 6, "TIME/");
  int hw = siprintf(buf, "%u", (unsigned)t->playtime.hours);
  gbscr_text(gs, 9, 6, buf);
  gbscr_cell(gs, 9 + hw, 6, GBSCR_SRC_TEXTBOX, G1T_COLON);
  siprintf(buf, "%02u", (unsigned)t->playtime.minutes);
  gbscr_text(gs, 10 + hw, 6, buf);
  for (int cx = 12 + hw; cx <= 14; cx++) gbscr_cell(gs, cx, 6, GBSCR_SRC_BLANK, 0);

  /* D1 (review): the real game draws the pic BEFORE the text box, so the
   * lower-right corner of the upper panel's own border overwrites pic
   * column 5 (screen col 20, off-map -- N/A here) and pic row 7 (screen row
   * 7, the panel's bottom border) -- net visible area is 4 tile COLUMNS x 6
   * ROWS (tx 0..3, ty 0..5), not the previously-drawn 5x7. Tile index is
   * still row-major over the FULL 7x7 decode (ty*7+tx), matching
   * gbscr_decode_pic_gen1()'s own packing order -- only the painted subrange
   * shrank. */
  for (int ty = 0; ty < 6; ty++)
    for (int tx = 0; tx < 4; tx++)
      gbscr_cell(gs, 15 + tx, 1 + ty, GBSCR_SRC_PIC, (uint8_t)(ty * 7 + tx));

  gbscr_cell(gs, 6, 9, GBSCR_SRC_CARDFRAME, G1F_CIRCLE);
  gbscr_text(gs, 7, 9, "BADGES");
  /* D3 (review): the right BADGES circle is at column 13, not 14 (confirmed
   * against the real tilemap capture). */
  gbscr_cell(gs, 13, 9, GBSCR_SRC_CARDFRAME, G1F_CIRCLE);

  /* D4 (review): the two side rules between the panels are the
   * screen-background dither (CARDFRAME index G1F_BG), drawn per row for the
   * full 8-row gap (screen rows 10..17) at columns 0 and 19 -- not a single
   * TEXTBOX tile on row 10. */
  for (int ry = 10; ry <= 17; ry++) {
    gbscr_cell(gs, 0, ry, GBSCR_SRC_CARDFRAME, G1F_BG);
    gbscr_cell(gs, 19, ry, GBSCR_SRC_CARDFRAME, G1F_BG);
  }

  g1card_panel(gs, 1, 10, 18, 17);                      /* lower panel */

  /* Badges: row 1 at y=11, row 2 at y=14 (design sec 1.1), column stride 4.
   * Leader-name tiles (c+1..c+2, same row as the number) stay BLANK --
   * gbscr_cell() is simply never called there, and every cell starts BLANK
   * per gbscr_open()'s own zeroing (English release: erased in the ROM). */
  for (int k = 0; k < 8; k++) {
    int row = k / 4, col = k % 4;
    int c = 2 + col * 4, r = 11 + row * 3;
    gbscr_cell(gs, c, r, GBSCR_SRC_CARDFRAME, (uint8_t)(G1F_BADGENUM0 + k));
    bool owned = gbtr_badge_get(t, true, k);
    int base = 8 * k + (owned ? 4 : 0);
    for (int dy = 0; dy < 2; dy++)
      for (int dx = 0; dx < 2; dx++)
        gbscr_cell(gs, c + 1 + dx, r + 1 + dy, GBSCR_SRC_BADGES, (uint8_t)(base + dy * 2 + dx));
  }
}

/* Cursor cell-group for one G1C_* slot -- (x,y,w,h) in tile units, matching
 * exactly the cells g1card_paint() painted for that field, so the highlight
 * always outlines real content. */
static void g1card_sel_rect(int sel, int* x, int* y, int* w, int* h) {
  switch (sel) {
    case G1C_NAME:  *x = 7;  *y = 2; *w = GB_OT_GLYPHS; *h = 1; break;
    case G1C_MONEY: *x = 8;  *y = 4; *w = 7;             *h = 1; break;   /* "$"+6 digits */
    case G1C_TIME:  *x = 9;  *y = 6; *w = 6;             *h = 1; break;   /* "HHH:MM"     */
    default: {
      int k = sel - G1C_BADGE0;
      int row = k / 4, col = k % 4;
      *x = 2 + col * 4; *y = 11 + row * 3; *w = 3; *h = 3;   /* number + 2x2 pic */
    } break;
  }
}

/* The card's own editor dispatch: NAME/MONEY/TIME reuse the SAME whole-screen
 * sub-editors gbtr_edit_row() already shares with the plain page and Gen 2's
 * card; badges toggle INSTANTLY (no pop-up), same as Gen 2's own card front
 * badge row -- Gen 1 shows all 8 badges on the card itself, unlike Gen 2's
 * Kanto half, so there is no "more badges than fit" case needing the
 * full-screen gbtr_badges_editor() here. */
static void g1card_edit_sel(GbTrainer* t, int sel) {
  switch (sel) {
    case G1C_NAME:  gbtr_edit_row(t, true, GBTR_NAME);  break;
    case G1C_MONEY: gbtr_edit_row(t, true, GBTR_MONEY); break;
    case G1C_TIME:  gbtr_edit_row(t, true, GBTR_TIME);  break;
    default:        gbtr_badge_toggle(t, true, sel - G1C_BADGE0); break;
  }
}

/* Red's own trainer card, drawn on the shared shell. Returns true (want a
 * commit prompt) exactly when B was pressed -- same "always ask, the caller's
 * memcmp is the no-op check" contract pdna_gbtrainer_card()'s own front page
 * uses, so a read-only visit (can_edit false, t untouched) silently no-ops at
 * the caller rather than ever prompting. Falls back to the plain row-list
 * page (with an HONEST header naming the refusal, design sec 3.5) when the
 * shell cannot open -- no ROM registered, not enough stack, a non-English
 * release, or no tile-bank memory. */
__attribute__((noinline))
static bool pdna_gbtrainer_gen1_card(GbTrainer* t, bool can_edit) {
  uint32_t shell_need = gbscr_tail_need(PDNA_GEN1,
      GBSCR_NEED_CARDFRAME | GBSCR_NEED_BADGES | GBSCR_NEED_TEXTBOX);
  /* U2b review 0b: ONE slice -- the shell's own tile cache AND this screen's
   * player-pic decode share it; the pic's bytes are carved out past what the
   * shell itself uses (gbscr_tail_need()'s own result), never a second
   * gb12_arena_tail() call. GBSCR_PIC_TAIL_BYTES (4,704 B: 784 B kept +
   * 3,920 B transient codec scratch) is bigger than the brief's own "+784"
   * shorthand -- that number covers only the KEPT packed pic, not
   * gb_sprite_gen1_buf()'s own px[3,136]+work[784] decode scratch, which has
   * nowhere else to live (far too big for the stack budget this screen's own
   * frame is gated against) and is not reused for anything after this call. */
  uint32_t need = shell_need + GBSCR_PIC_TAIL_BYTES;
  uint8_t* tail = gb12_arena_tail(need);

  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN1, &gs, tail, shell_need,
                       GBSCR_NEED_CARDFRAME | GBSCR_NEED_BADGES | GBSCR_NEED_TEXTBOX, &reason);
  if (!ok) {
    gb12_arena_tail_release();
    /* D7 (review): kReasonOpen's longest text used to be 65 chars -- into this
     * "GB ART: OFF -- " (15) + reason buffer that would overflow a 40-byte
     * hdr by 41 bytes on any open refusal. hdr is now sized for the longest
     * reason string in this translation unit + the fixed prefix, and
     * sniprintf (not siprintf) bounds the write regardless. */
    char hdr[96];
    sniprintf(hdr, sizeof hdr, "GB ART: OFF -- %s", reason ? reason : "unavailable");
    return pdna_gbtrainer_plain(t, true, can_edit, hdr);
  }

  /* Best-effort: a failed pic decode leaves GBSCR_SRC_PIC cells painting the
   * same flat BLANK every other unavailable src uses -- not a hard refusal of
   * the whole card (design: the photo is one field among several). */
  gbscr_decode_pic_gen1(&gs, tail + shell_need, GBSCR_PIC_TAIL_BYTES);

  /* D9 (review): the shell's own base legend ("A OK  B BACK  SEL SIZE")
   * contradicts this screen's real keys -- there is no A-OK-only meaning
   * here, and B SAVES on an editable visit, not "back". Replace it entirely
   * (gbscr_set_legend()) instead of only appending to it: can_edit shows the
   * real A/B meanings; read-only drops the A line (there is nothing to
   * edit). Both list D8's new START key. */
  static const char* const kLegendEdit[4] = { "A EDIT", "B SAVE", "SEL SIZE", "START MORE" };
  static const char* const kLegendView[4] = { "B BACK", "SEL SIZE", "START MORE", 0 };
  gbscr_set_legend(&gs, can_edit ? kLegendEdit : kLegendView);

  int sel = 0;
  bool want_commit = false;
  g1card_paint(&gs, t);
  for (;;) {
    gbscr_flush(&gs, 0);   /* legend_extra is ignored once gbscr_set_legend() ran */

    int cx, cy, cw, ch;
    g1card_sel_rect(sel, &cx, &cy, &cw, &ch);
    int px0, py0, px1, py1;
    gbscr_cell_rect(cx, cy, cw, ch, &px0, &py0, &px1, &py1);
    m3_frame(px0, py0, px1, py1, GBCARD_CSEL);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_SELECT | KEY_START);
    if (k & KEY_SELECT) { gbscr_toggle_scale(&gs); continue; }   /* shell owns this */
    /* D8 (review): START reaches the full Gen-1 row list (ID No./COINS/the
     * back page -- gbtr_build_rows()'s complete field set) that this card
     * front does not show; the plain page is the one that already has all
     * of it. Regardless of can_edit -- a read-only visit can still WANT to
     * see the fields this card leaves off. */
    if (k & KEY_START) {
      gbscr_close(&gs);
      gb12_arena_tail_release();
      return pdna_gbtrainer_plain(t, true, can_edit, 0);
    }
    if (k & KEY_B) { want_commit = true; break; }
    if (!can_edit) continue;

    int old_sel = sel;
    if (k & KEY_UP)         sel = (sel > 0) ? sel - 1 : G1C_SEL_MAX - 1;
    else if (k & KEY_DOWN)  sel = (sel + 1) % G1C_SEL_MAX;
    else if ((k & (KEY_LEFT | KEY_RIGHT)) && sel >= G1C_BADGE0) {
      int kk = sel - G1C_BADGE0;
      kk = (kk + ((k & KEY_RIGHT) ? 1 : 7)) & 7;
      sel = G1C_BADGE0 + kk;
    } else if (k & KEY_A) {
      g1card_edit_sel(t, sel);
      g1card_paint(&gs, t);
      gbscr_mark_all_dirty(&gs);       /* the field itself may not have moved a
                                        * cell but the cursor frame drawn last
                                        * loop must be erased under real content */
    }
    if (sel != old_sel) gbscr_mark_all_dirty(&gs);   /* erase the old cursor frame */
  }

  gbscr_close(&gs);
  gb12_arena_tail_release();
  return want_commit;
}

void pdna_gbtrainer(GbSession* s, bool can_edit) {
  GbTrainer t, t0;
  if (!s || !gbt_read(s, &t)) {
    msg_wait("TRAINER CARD", UI_WARN, "Could not read this save.", 0);
    return;
  }
  memcpy(&t0, &t, sizeof t0);        /* snapshot: proves a no-op commit (memcmp below) */
  s_id_warned = false;               /* the identity warning is once per VISIT */
  const bool gen1 = (s->gen == GB_GEN1);

  /* U2c: Gen 1 always tries Red's own card first (the GB-screen shell over the
   * user's own ROM), falling back to the plain row-list page (with an honest
   * header naming the refusal) only when the shell itself refuses -- design
   * sec 3.5. Gen 2 is UNCHANGED: Emerald's card art (or its own weak-NULL/
   * ROM-rung fallback -- see card_bg.h) present -> the card IS the whole
   * screen (P1c); absent -> P1b's plain row-list page. Mirrors pdna_trainer()'s
   * own `card_bg(game, 0, gender).blob != 0` fork exactly. */
  bool want_commit = false;
  if (gen1) {
    want_commit = pdna_gbtrainer_gen1_card(&t, can_edit);
  } else if (card_bg(PK_EMERALD, 0, 0).blob != 0) {
    pdna_gbtrainer_card(&t, gen1, can_edit, &want_commit);
  } else {
    want_commit = pdna_gbtrainer_plain(&t, gen1, can_edit, 0);
  }
  if (!want_commit) return;                    /* plain page's B: discard (unchanged from
                                                  * P1b); the card path (P1d) always asks
                                                  * here -- the memcmp below is its no-op
                                                  * check, same as Gen 3's card_editor */

  if (!memcmp(&t, &t0, sizeof t)) { snd_back(); return; }   /* nothing to write */
  if (!app_confirm("Save trainer changes?", "Writes the card edits now.")) return;
  GbsStatus st = gbt_write(s, &t);
  if (st != GBS_OK) {
    /* gbt_write may already have landed SOME of the batch (gb_trainer.h's own
     * contract) -- roll the whole image back rather than leave a partial edit. */
    gb_rollback();
    snd_error();
    msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
    return;
  }
  /* gb_persist() plays its own snd_save()/snd_error() and, on any failure past
   * this point, has ALREADY called gb_rollback() and told the user why. */
  gb_persist("trainer");
}
