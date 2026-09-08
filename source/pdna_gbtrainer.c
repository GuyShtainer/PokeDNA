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
 * Cursor/edit semantics UNCHANGED from P1b: edits are staged in a LOCAL
 * GbTrainer (gbt_read at entry, mutated in place by the row editors) and
 * committed ONLY on START, in one gbt_write() batch; a no-op START (nothing
 * touched, t == t0) returns silently with no popup; a real edit asks "Save
 * trainer changes?" before gbt_write runs, and any failure rolls the whole
 * image back (gb_rollback) rather than leaving a partial edit. B always
 * discards on the FRONT page (the session's image was never touched unless
 * START already ran and returned) -- "B = cancel" is this screen's own honest
 * adaptation of Gen-3's card key legend, which uses B for "save and exit"
 * there; B on the BACK page instead flips back to the front (same as Gen 3).
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
#include "pdna_gen12.h"    /* gb_rollback / gb_persist -- the S2 commit primitives   */
#include "pdna_layout.h"   /* PDNA_GBTRAINER_ID_WARN_* -- measured by host_textfit   */
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "pdna_app.h"      /* msg_wait */

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
 * rather than PkGame/pk_flag_get -- nothing here touches the session until START.
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

static void gbtr_plain_render(const GbTrainer* t, bool gen1, const int* rows, int nrows,
                              bool can_edit, int sel, GbtrPaint* pv) {
  bool full = !pv->valid || pv->gen != ui_clear_gen();

  if (full) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, gen1 ? "TRAINER CARD (Gen 1)" : "TRAINER CARD (Gen 2)");
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
static bool pdna_gbtrainer_plain(GbTrainer* t, bool gen1, bool can_edit) {
  int rows[GBTR_ROW_MAX];
  int nrows = gbtr_build_rows(t, rows);

  int sel = 0;
  GbtrPaint pv = { 0, 0, false };
  for (;;) {
    gbtr_plain_render(t, gen1, rows, nrows, can_edit, sel, &pv);

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
  /* P1c gap (documented, not fixed here): the shared card painter has no "?"
   * state for MONEY the way this file's own plain page does (t->money_ok ==
   * false, a Gen-1 BCD decode failure) -- it always formats "$%lu". Showing 0
   * here is the honest-enough compromise (the field stays refused for editing
   * either way, gbtr_edit_row's own money_ok check, unchanged from P1b); a
   * real "?" on the card face is a BACKLOG follow-up. */
  cf->money = t->money_ok ? t->money : 0;
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
  ui_text(4, 152, UI_TEXT, can_edit ? "U/D A edit  SELECT Kanto  L/R flip  START save  B cancel"
                                      : "L/R flip  B back");
  if (can_edit) card_field_sel_frame(PK_EMERALD, cardf[sel], 0);
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

/* The card's own editor loop (P1c): SAME shape as pdna_trainer.c's card_editor
 * -- a red selection frame walks the fields ON the card (front) or the row
 * list (back); A edits in place with the same sub-editors gbtr_edit_row above
 * already shares with the plain page. Reads `t`/`t0`/`gen1`/`can_edit` from
 * the caller; returns when the caller should exit (B on the front) or commit
 * (START anywhere) -- `*want_commit` tells the caller which. noinline so this
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
                   KEY_A | KEY_B | KEY_L | KEY_R | KEY_START | KEY_SELECT);
    if (k & (KEY_L | KEY_R)) { back = !back; full = true; continue; }

    if (k & KEY_START) {
      if (!can_edit) continue;
      *want_commit = true;
      return;
    }
    if (k & KEY_B) {
      if (back) { back = false; full = true; continue; }
      return;                                    /* front: discard, caller B-cancels */
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

void pdna_gbtrainer(GbSession* s, bool can_edit) {
  GbTrainer t, t0;
  if (!s || !gbt_read(s, &t)) {
    msg_wait("TRAINER CARD", UI_WARN, "Could not read this save.", 0);
    return;
  }
  memcpy(&t0, &t, sizeof t0);        /* snapshot: proves a no-op START at commit time */
  s_id_warned = false;               /* the identity warning is once per VISIT */
  const bool gen1 = (s->gen == GB_GEN1);

  /* Emerald's card art (or its own weak-NULL/ROM-rung fallback -- see card_bg.h):
   * present -> the card IS the whole screen (P1c); absent (artless build, no
   * matching ROM open) -> P1b's plain row-list page, unchanged. Mirrors
   * pdna_trainer()'s own `card_bg(game, 0, gender).blob != 0` fork exactly. */
  bool want_commit = false;
  if (card_bg(PK_EMERALD, 0, 0).blob != 0) {
    pdna_gbtrainer_card(&t, gen1, can_edit, &want_commit);
  } else {
    want_commit = pdna_gbtrainer_plain(&t, gen1, can_edit);
  }
  if (!want_commit) return;                                 /* B: discard, unchanged from P1b */

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
