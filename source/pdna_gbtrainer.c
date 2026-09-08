/*
 * Gen-1/2 trainer card -- BACKLOG #49 P1b. See pdna_gbtrainer.h for the design note.
 *
 * Rows, in Gen-3 card order where the field exists: NAME, ID, MONEY, COINS,
 * MOM'S MONEY + saving mode (Gen 2), BADGES, PLAY TIME, GENDER (Crystal, view-only),
 * DEX seen/owned (view-only), RIVAL (view-only), MOTHER (Gen 2, view-only). The row
 * list is built once per visit from gbt_read()'s own has_mom/has_gender/has_mother
 * flags -- never a fixed enum-sized array the way Gen 3's TF_NUM is, because which
 * rows exist is a per-GAME fact here, not a per-screen constant.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbtrainer.h"
#include "gb_trainer.h"
#include "pdna_trainer.h"  /* num_entry / trainer_row_paint / trainer_flag_row_paint /
                            * trainer_key_legend -- the exported Gen-3 card painters */
#include "pdna_gen12.h"    /* gb_rollback / gb_persist -- the S2 commit primitives   */
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

enum {
  GBTR_NAME, GBTR_ID, GBTR_MONEY, GBTR_COINS, GBTR_MOMMONEY, GBTR_MOMSAVE,
  GBTR_BADGES, GBTR_TIME, GBTR_GENDER, GBTR_DEX, GBTR_RIVAL, GBTR_MOTHER,
  GBTR_ROW_MAX
};

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
      lbl = "MOM SAVE";
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
  trainer_row_paint(y, sel, lbl, val, ink);
}

void pdna_gbtrainer(GbSession* s, bool can_edit) {
  GbTrainer t;
  if (!s || !gbt_read(s, &t)) {
    msg_wait("TRAINER CARD", UI_WARN, "Could not read this save.", 0);
    return;
  }
  const bool gen1 = (s->gen == GB_GEN1);

  int rows[GBTR_ROW_MAX]; int nrows = 0;
  rows[nrows++] = GBTR_NAME;
  rows[nrows++] = GBTR_ID;
  rows[nrows++] = GBTR_MONEY;
  rows[nrows++] = GBTR_COINS;
  if (t.has_mom)    { rows[nrows++] = GBTR_MOMMONEY; rows[nrows++] = GBTR_MOMSAVE; }
  rows[nrows++] = GBTR_BADGES;
  rows[nrows++] = GBTR_TIME;
  if (t.has_gender) rows[nrows++] = GBTR_GENDER;
  rows[nrows++] = GBTR_DEX;
  rows[nrows++] = GBTR_RIVAL;
  if (t.has_mother) rows[nrows++] = GBTR_MOTHER;

  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, gen1 ? "TRAINER CARD (Red/Blue/Yellow)"
                                 : "TRAINER CARD (Gold/Silver/Crystal)");
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < nrows; i++)
      gbtr_row_paint(&t, rows[i], gen1, 14 + i * 9, can_edit && i == sel);
    trainer_key_legend(can_edit ? "U/D field  A edit  START save  B back" : "B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return;                       /* discard: `t` was never written */

    if (k & KEY_START) {
      if (!can_edit) continue;
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
          char b[8];
          if (osk_input("TRAINER NAME", t.name, b, sizeof b)) strcpy(t.name, b);
        } break;
        case GBTR_ID:
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
