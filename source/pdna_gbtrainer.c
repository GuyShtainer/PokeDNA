/*
 * Gen-1/2 trainer card -- BACKLOG #49 P1b. See pdna_gbtrainer.h for the design note.
 *
 * Row order (P1b review D9: this is NOT the Gen-3 card's own order -- Gen 3's TF_*
 * is NAME, SEX, ID, SID, MONEY, TIME, BADGE, STARS, and has no COINS/MOM/DEX/RIVAL/
 * MOTHER at all): NAME, ID, MONEY, COINS, MOM'S MONEY + saving mode (Gen 2), BADGES,
 * PLAY TIME, GENDER (Crystal only, view-only), DEX seen/owned (view-only), RIVAL
 * (view-only), MOTHER (Gen 2, view-only). Editable fields come first, view-only
 * fields last; GENDER sits in the view-only tail rather than up near NAME/ID
 * because, unlike Gen 3's SEX, it is never user-editable here (Crystal reads it from
 * the save but nothing in-game lets the player change it, so this screen doesn't
 * pretend otherwise). The row list is built once per visit from gbt_read()'s own
 * has_mom/has_gender/has_mother flags -- never a fixed enum-sized array the way
 * Gen 3's TF_NUM is, because which rows exist is a per-GAME fact here, not a
 * per-screen constant.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbtrainer.h"
#include "gb_trainer.h"
#include "gbtr_rows.h"     /* pure-C row-visibility model, host-tested separately  */
#include "pdna_trainer.h"  /* num_entry / trainer_row_paint / trainer_flag_row_paint /
                            * trainer_key_legend -- the exported Gen-3 card painters */
#include "pdna_gen12.h"    /* gb_rollback / gb_persist -- the S2 commit primitives   */
#include "pdna_layout.h"   /* PDNA_GBTRAINER_ID_WARN_* -- measured by host_textfit   */
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "pdna_app.h"      /* msg_wait */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN)) snd_move();
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
 * rather than PkGame/pk_flag_get -- nothing here touches the session until START. */
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
 * the real game's own play_time.asm does (gb_trainer.c's own comment). */
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

/* D5: what is on screen. Every editor reachable below (osk_input/num_entry/
 * app_confirm/gbtr_badges_editor/gbtr_time_editor/msg_wait) opens with its own
 * ui_clear() on entry, so ui_clear_gen() alone proves whether an edit ran --
 * same idiom as pdna_trainer.c's own TCardPaint. That leaves exactly one case
 * worth special-casing: a plain UP/DOWN cursor move, which touches nothing
 * outside the two affected rows. Stack-local, not a static: a fresh call
 * always starts invalid (first pass paints in full). */
typedef struct { uint32_t gen; int sel; bool valid; } GbtrPaint;

static void gbtr_render(const GbTrainer* t, bool gen1, const int* rows, int nrows,
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

void pdna_gbtrainer(GbSession* s, bool can_edit) {
  GbTrainer t, t0;
  if (!s || !gbt_read(s, &t)) {
    msg_wait("TRAINER CARD", UI_WARN, "Could not read this save.", 0);
    return;
  }
  memcpy(&t0, &t, sizeof t0);        /* snapshot: proves a no-op START at commit time */
  s_id_warned = false;               /* the identity warning is once per VISIT */
  const bool gen1 = (s->gen == GB_GEN1);

  int rows[GBTR_ROW_MAX];
  int nrows = gbtr_build_rows(&t, rows);

  int sel = 0;
  GbtrPaint pv = { 0, 0, false };
  for (;;) {
    gbtr_render(&t, gen1, rows, nrows, can_edit, sel, &pv);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return;                       /* discard: `t` was never written */

    if (k & KEY_START) {
      if (!can_edit) continue;
      if (!memcmp(&t, &t0, sizeof t)) { snd_back(); return; }   /* nothing to write */
      if (!app_confirm("Save trainer changes?", "Writes the card edits now.")) continue;
      GbsStatus st = gbt_write(s, &t);
      if (st != GBS_OK) {
        /* gbt_write may already have landed SOME of the batch (gb_trainer.h's own
         * contract) -- roll the whole image back rather than leave a partial edit. */
        gb_rollback();
        snd_error();
        msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
        continue;
      }
      /* gb_persist() plays its own snd_save()/snd_error() and, on any failure past
       * this point, has ALREADY called gb_rollback() and told the user why. */
      gb_persist("trainer");
      return;
    }

    if (k & KEY_UP)        sel = (sel > 0) ? sel - 1 : nrows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % nrows;
    else if ((k & KEY_A) && can_edit) {
      switch (rows[sel]) {
        case GBTR_NAME: {
          if (!gbtr_id_edit_ok()) break;
          char b[GB_TEXT_MAX];        /* t.name is char[GB_TEXT_MAX]; UTF-8 decoded,
                                        * a truncated buffer here silently drops glyphs
                                        * (P1b review D1) */
          if (osk_input("TRAINER NAME", t.name, b, sizeof b)) strcpy(t.name, b);
        } break;
        case GBTR_ID:
          if (!gbtr_id_edit_ok()) break;
          t.trainer_id = (uint16_t)num_entry("ID No", t.trainer_id, 65535u);
          break;
        case GBTR_MONEY:
          if (!t.money_ok) {
            msg_wait("UNREADABLE", UI_WARN, "This field is not a valid", "number and cannot be edited.");
            break;
          }
          t.money = num_entry("MONEY", t.money, 999999u);
          break;
        case GBTR_COINS:
          if (!t.coins_ok) {
            msg_wait("UNREADABLE", UI_WARN, "This field is not a valid", "number and cannot be edited.");
            break;
          }
          t.coins = (uint16_t)num_entry("COINS", t.coins, 9999u);
          break;
        case GBTR_MOMMONEY:
          t.moms_money = num_entry("MOM'S MONEY", t.moms_money, 999999u);
          break;
        case GBTR_MOMSAVE:
          t.mom_saving_bits = gbtr_cycle_mom_saving(t.mom_saving_bits);
          break;
        case GBTR_BADGES:
          gbtr_badges_editor(&t, gen1);
          break;
        case GBTR_TIME:
          gbtr_time_editor(&t, gen1);
          break;
        default: break;   /* GENDER / DEX / RIVAL / MOTHER: view-only */
      }
    }
  }
}
