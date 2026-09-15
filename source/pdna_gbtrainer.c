/*
 * Gen-1/2 trainer card -- BACKLOG #49 P1b (plain row-list fallback) started
 * this; P1c/P1d then art-degraded it onto Emerald's own card_bg()/
 * CARD_LAYOUTS art for UX parity, since neither Game Boy generation had a
 * card of its own on the shared shell yet. U2c (Gen 1, Red) and U3 (Gen 2,
 * Gold/Silver/Crystal) each replaced that borrowed art with the real
 * cartridge's OWN trainer card -- pdna_gbtrainer_gen1_card()/g1card_paint()
 * and pdna_gbtrainer_gen2_card()/g2card_paint_page{1,2}() below -- so the
 * Emerald-art path (gbcard_front_full/gbcard_back_full/pdna_gbtrainer_card/
 * gb_cardfields/CardFields, card_bg.h) is gone from this file entirely; both
 * generations now only fall back to the plain row-list page (still this
 * file's own P1b, unchanged) when their own shell refuses to open.
 *
 * Cursor/edit semantics (unchanged since P1b): edits are staged in a LOCAL
 * GbTrainer (gbt_read at entry, mutated in place by the row editors) and
 * committed in one gbt_write() batch; a no-op commit (nothing touched, t ==
 * t0) returns silently with no popup; a real edit asks "Save trainer
 * changes?" before gbt_write runs, and any failure rolls the whole image
 * back (gb_rollback) rather than leaving a partial edit. The KEY that asks
 * for that commit differs by page: the plain row-list page uses START; both
 * generations' own real cards use B (matching what the real cartridge shows
 * on that key), same "always ask, the caller's memcmp is the no-op check"
 * contract Gen 3's own card_editor uses.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbtrainer.h"
#include "gb_trainer.h"
#include "gbtr_rows.h"     /* pure-C row-visibility model, host-tested separately  */
#include "pdna_trainer.h"  /* num_entry / trainer_row_paint / trainer_flag_row_paint /
                            * trainer_key_legend                                    */
#include "pdna_gen12.h"    /* gb_rollback / gb_persist -- the S2 commit primitives;
                            * gb12_arena_tail/gb12_arena_tail_release (U2c)          */
#include "g2card_cells.h"  /* BACKLOG #96 D11: the Gen-2 card's pure static cell table */
#include "pdna_gbscreen.h" /* U2c/U3: the shared GB-screen shell -- both generations'
                            * own real cards                                        */
#include "pdna_origin_art.h" /* PDNA_GEN1/2 -- gbscr_open()'s own `gen` constant      */
#include "pdna_layout.h"   /* PDNA_GBTRAINER_ID_WARN_* -- measured by host_textfit   */
#include "ui.h"
#include "snd.h"
#include "osk.h"
#include "pdna_app.h"      /* msg_wait */

/* Shared selection-frame red, both generations' own real cards (g1card_paint/
 * g2card_paint_page{1,2}). */
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
 * ---- the plain row-list page (P1b, unchanged) -- the fallback for BOTH
 * generations' own real cards when their shell refuses to open (no ROM
 * registered, not enough stack, a non-English release, or no tile-bank
 * memory), and the field-complete screen START reaches from either card.
 * See gbtr_build_rows() (gbtr_rows.c) for the flat row list this draws --
 * the ORIGINAL P1b order, kept exactly as it shipped.
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
 * shell refused (e.g. "GB ART: OFF") -- never silently degraded, per the design's
 * own rule. NULL keeps the ordinary title, unchanged from P1b/P1c (Gen 2's own
 * art-unavailable fallback, and any other caller).
 *
 * `header2` (may be NULL): D2 fix (U2c 2nd re-verify) -- the REASON for a
 * `header` fallback used to be appended to `header` itself ("GB ART: OFF --
 * <reason>") and painted as ONE fixed-font title line, which ran off the right
 * edge of the 240-px screen for any reason text past ~29 chars (the fixed font
 * is 8 px/glyph; the re-verify's forced-fallback shot showed the tail of
 * "forced (PDNA_U2C_FORCE_FALLBACK)" cut at the screen edge). `header2`, when
 * supplied, paints on its OWN line below `header` in the proportional font
 * (ui_ptext -- see GBTR_HEADER2_MAXW below for the fits-on-screen contract),
 * and every row + the top hline shift down by one line to make room. */
static void gbtr_plain_render(const GbTrainer* t, bool gen1, const int* rows, int nrows,
                              bool can_edit, int sel, GbtrPaint* pv, const char* header,
                              const char* header2) {
  bool full = !pv->valid || pv->gen != ui_clear_gen();
  int hline_y = header2 ? 20 : 11;
  int row_y0  = header2 ? 23 : 14;

  if (full) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, header ? header : (gen1 ? "TRAINER CARD (Gen 1)" : "TRAINER CARD (Gen 2)"));
    if (header2) ui_ptext_fit(4, 11, GBTR_HEADER2_MAXW, UI_DIM, header2);
    ui_hline(0, hline_y, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < nrows; i++)
      gbtr_row_paint(t, rows[i], gen1, row_y0 + i * 9, i == sel);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, can_edit ? "A edit  START save  B cancel" : app_gb_readonly_footer());
  } else if (sel != pv->sel) {
    gbtr_row_paint(t, rows[pv->sel], gen1, row_y0 + pv->sel * 9, false);
    gbtr_row_paint(t, rows[sel],     gen1, row_y0 + sel     * 9, true);
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
 * call site in pdna_gbtrainer() below (matches both real cards' own B ->
 * want_commit contract) -- this used to ask+write here directly (P1b), but
 * every caller needs the SAME commit path, not a separate one per page. */
__attribute__((noinline))
static bool pdna_gbtrainer_plain(GbTrainer* t, bool gen1, bool can_edit, const char* header,
                                 const char* header2) {
  int rows[GBTR_ROW_MAX];
  int nrows = gbtr_build_rows(t, rows);

  int sel = 0;
  GbtrPaint pv = { 0, 0, false };
  for (;;) {
    gbtr_plain_render(t, gen1, rows, nrows, can_edit, sel, &pv, header, header2);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return false;                  /* discard: `t` was never written */
    if (k & KEY_START) {
      if (!can_edit) {
        snd_deny();
        msg_wait("READ-ONLY", UI_WARN, app_gb_readonly_why(), NULL);
        continue;
      }
      if (can_edit) return true;
      continue;
    }

    if (k & KEY_UP)        sel = (sel > 0) ? sel - 1 : nrows - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % nrows;
    else if (k & KEY_A) {
      if (!can_edit) {
        snd_deny();
        msg_wait("READ-ONLY", UI_WARN, app_gb_readonly_why(), NULL);
        continue;
      }
      gbtr_edit_row(t, gen1, rows[sel]);
    }
  }
}


/* ============================================================================
 * ---- U2c: Red's OWN trainer card, over the shared GB-screen shell -- Gen 1
 * ONLY (docs/GB-GAME-SCREENS-DESIGN.md sec 1.1). Retired the Emerald-art card
 * (gbcard_front_full/gbcard_back_full/pdna_gbtrainer_card/gb_cardfields) for
 * Gen-1 saves; U3 retired it for Gen 2 too (its own real card, below), so
 * that whole Emerald-art path is now dead and deleted, not just unreached.
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
   * For UNOWNED badges: draw leader-name tiles at (c+1, r) and (c+2, r) using
   * CARDFRAME block indices 9+2k and 10+2k, matching the real game's behavior.
   * For OWNED badges: leader-name cells stay BLANK. Every cell starts BLANK
   * per gbscr_open()'s own zeroing (English release: erased in the ROM). */
  for (int k = 0; k < 8; k++) {
    int row = k / 4, col = k % 4;
    int c = 2 + col * 4, r = 11 + row * 3;
    gbscr_cell(gs, c, r, GBSCR_SRC_CARDFRAME, (uint8_t)(G1F_BADGENUM0 + k));
    bool owned = gbtr_badge_get(t, true, k);
    if (!owned) {
      gbscr_cell(gs, c + 1, r, GBSCR_SRC_CARDFRAME, (uint8_t)(9 + 2 * k));
      gbscr_cell(gs, c + 2, r, GBSCR_SRC_CARDFRAME, (uint8_t)(10 + 2 * k));
    }
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
      GBSCR_NEED_CARDFRAME | GBSCR_NEED_BADGES | GBSCR_NEED_TEXTBOX, 0);
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
                       GBSCR_NEED_CARDFRAME | GBSCR_NEED_BADGES | GBSCR_NEED_TEXTBOX, 0, &reason);
#ifdef PDNA_U2C_FORCE_FALLBACK
  /* D10 (review): the fallback branch below (no ROM registered / bad ROM /
   * non-English release / no tile-bank memory) is this function's only
   * exit path with no shot -- every real corpus ROM this tree ships opens
   * fine, so the shot harness can never reach it by driving a real save.
   * A build-time flag forces the refusal so the honest-header fallback
   * (D7's header contract) gets ONE real shot instead of staying untested. */
  if (ok) { gbscr_close(&gs); ok = false; reason = PDNA_GBSCR_REASON_FORCED_TEST; }
#endif
  if (!ok) {
    gb12_arena_tail_release();
    /* D2 fix (U2c 2nd re-verify): the reason used to be concatenated onto the
     * "GB ART: OFF -- " title and painted as ONE fixed-font line, which ran
     * off the 240-px screen for any reason past ~29 chars. The title now
     * stays the short, always-fits "GB ART: OFF"; the reason is its OWN
     * second line in gbtr_plain_render's proportional font -- see
     * GBTR_HEADER2_MAXW (this file, below) for the compile-time-checked
     * width contract every kReason* string in pdna_gbscreen.c must meet. */
    return pdna_gbtrainer_plain(t, true, can_edit, PDNA_GBTR_FALLBACK_TITLE,
                                reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE);
  }

  /* Best-effort: a failed pic decode leaves GBSCR_SRC_PIC cells painting the
   * same flat BLANK every other unavailable src uses -- not a hard refusal of
   * the whole card (design: the photo is one field among several). */
  /* Minor (U2c review): `need - shell_need` is the ACTUAL size of the tail
   * past the shell's own share -- always equal to GBSCR_PIC_TAIL_BYTES today
   * (need's own formula above), but passing the constant made
   * gbscr_decode_pic_gen1()'s own `buf_len < GBSCR_PIC_TAIL_BYTES` check a
   * tautology that could never catch a future mismatch between `need`'s
   * formula and this call. Passing the derived size makes the check real. */
  gbscr_decode_pic_gen1(&gs, tail + shell_need, need - shell_need);

  /* D9 (review): the shell's own base legend ("A OK  B BACK  SEL SIZE")
   * contradicts this screen's real keys -- there is no A-OK-only meaning
   * here, and B SAVES on an editable visit, not "back". Replace it entirely
   * (gbscr_set_legend()) instead of only appending to it: can_edit shows the
   * real A/B meanings; read-only drops the A line (there is nothing to
   * edit). Both list D8's new START key.
   *
   * D1 fix (U2c 2nd re-verify): each slot is now the ACTION WORD ALONE --
   * gbscr_set_legend()'s fixed row order is A/B/SEL/START (kGbscrLegendKeys,
   * pdna_gbscreen.c) and the shell paints the key name itself in the left
   * bar, this array's word in the right bar. The joined "A EDIT"/"START
   * MORE" strings used to overflow the 36-px side bar (57 px for "START
   * MORE") and truncate to "START~"; splitting the columns means the widest
   * word here (MORE/SIZE, 22-24 px) never gets near the 36-px budget. View
   * mode's row 0 (A) stays 0 -- there is nothing to edit read-only. */
  static const char* const kLegendEdit[4] = {
    PDNA_GBTR_ACT_EDIT, PDNA_GBTR_ACT_SAVE, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  static const char* const kLegendView[4] = {
    0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
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
      return pdna_gbtrainer_plain(t, true, can_edit, 0, 0);
    }
    if (k & KEY_B) { want_commit = true; break; }

    /* Minor (U2c review): this loop always draws the sel-rect cursor frame
     * (above, every iteration) regardless of can_edit, but a read-only visit
     * used to gate EVERY key past this point behind `if (!can_edit) continue`
     * -- UP/DOWN/LEFT/RIGHT included -- so the cursor was drawn but could
     * never move: a static highlight frozen on NAME for the whole visit,
     * which looks broken rather than read-only. Only KEY_A (the actual edit
     * action) is gated on can_edit now; browsing the card with the cursor
     * works in both modes. */
    int old_sel = sel;
    if (k & KEY_UP)         sel = (sel > 0) ? sel - 1 : G1C_SEL_MAX - 1;
    else if (k & KEY_DOWN)  sel = (sel + 1) % G1C_SEL_MAX;
    else if ((k & (KEY_LEFT | KEY_RIGHT)) && sel >= G1C_BADGE0) {
      int kk = sel - G1C_BADGE0;
      kk = (kk + ((k & KEY_RIGHT) ? 1 : 7)) & 7;
      sel = G1C_BADGE0 + kk;
    } else if (can_edit && (k & KEY_A)) {
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

/* ---------------------------------------------------------------------------
 * U3: Gold/Silver/Crystal's OWN trainer card (BACKLOG #66), on the SAME
 * shared shell g1card_paint() uses above -- retires the Emerald-art card for
 * Gen 2 (pdna_gbtrainer_card/gbcard_front_full/gbcard_back_full/card_bg
 * below, dead now that BOTH generations have their own real card).
 *
 * Ground truth: tools/gb_roundtrip.py's own `_drive()` driving Gold.gbc +
 * Gold.sav and Crystal.gbc + Crystal.sav to the trainer card (START, 4x DOWN
 * to the player's own name row, A), dumping the BG tilemap (LCDC bit 3
 * selects 0x9800/0x9C00 -- Gen 2 draws this card on the BG layer, not the
 * window Red used) for both pages -- 4 dumps, MISMATCHED CELLS: 0 of 360 on
 * every one against the model below (no female/Kris save in this corpus'
 * Crystal.sav to dump a fifth/sixth oracle -- the cardpic_f/GBSCR_SRC_CARDPIC_F
 * path is the SAME mechanism already proven for cardpic_m/CARDPIC_M, just an
 * unverified offset choice, not a new code path).
 *
 * VRAM tile-id chain both pages share (adjacent located blocks land in
 * contiguous VRAM char ids after the always-loaded pic): 0x00-0x22 (35) =
 * CARDPIC display index (row-major on Gold, rom_gbui_tile's own colmajor
 * reorder handles Crystal's column-major storage); 0x23-0x26 (4) = FRAMES-
 * relative 0=border fill, 1=the one asymmetric notch tile (col1, one row
 * above the STATUS/BADGES strip AND one row above the bottom border -- the
 * real game's own choice, not a corner/edge tile set the way Gen 1's
 * CARDFRAME has one), 2=divider fill (x12), 3=divider cap, 11=the play-time
 * colon (0x2E, captured by sampling 70 frames -- toggles OFF/ON, this shell's
 * own repaint just needs the ON tile); 0x27-0x28 (2) = FONTEXTRA-relative
 * "ID"/"No"; page 1 only: 0x29-0x2D (5 of CARDGFX's 6) = the "STATUS" word
 * graphic; page 2 only: 0x29-0x78 (80 of LEADERS' 86) = the 8 gym-leader
 * faces (10 tiles each: 4 across the top row then 3+3 below, column stride
 * 4), 0x79-0x7D (5 more of LEADERS) = the "BADGES" word graphic -- LEADERS'
 * declared 86-tile size is exactly 80 faces + 6 word tiles, confirming this
 * is ONE block, not two. Screen (18,1)/(18,9) are the card's right-corner
 * chamfer, written by the game's own TrainerCard_InitBorder; on Crystal its
 * pixels come from CardRightCornerGFX, not from the pic -- this shell now
 * routes both cells through GBSCR_SRC_CARDCORNER (RomGbUi.cardcorner =
 * badges + 88 tiles) when the located ROM is Crystal-shaped (BACKLOG #125,
 * fixed; see pdna_gbtrainer_gen2_card()'s own has_corner comment below).
 *
 * The real game draws the OWNED-badge overlay as an animated OAM sprite over
 * the gym leader's face, never as a BG tile -- the BG-tilemap oracle above
 * cannot see it at all (every dump's LEADERS range is the unbroken 0-79
 * sequence regardless of which badges this save owns). This shell has no
 * sprites, so g2card_paint() draws a STATIC 2x2 BADGES-block overlay over an
 * owned face's centre instead -- an accepted deviation from the real
 * (animated, sprite-layered) game, not something the tilemap oracle could
 * ever confirm either way; called out again in the U3 report/HW-QUEUE row.
 */

/* Cursor slots -- page 1: NAME, ID, MONEY, TIME (UP/DOWN cycles); page 2: the
 * 8 Johto badges (UP/DOWN cycles 0-7, row-major like the on-screen 4x2 grid).
 * LEFT/RIGHT always flips the page (the real game's own binding) and resets
 * the cursor to slot 0 of whichever page it lands on -- Gen 1's own card
 * instead uses LEFT/RIGHT to step within its one-page badge row, which is
 * not available here since the real Gen-2 card spends that key on paging. */
enum { G2C_NAME = 0, G2C_ID, G2C_MONEY, G2C_TIME, G2C_P1_MAX };
enum { G2C_BADGE0 = 0, G2C_P2_MAX = 8 };

/* U3 re-anchor (found by byte-searching the ROM for each tile's own REAL
 * VRAM pattern, captured live, not guessed -- the celldiff model only
 * checks tile-ID CLASSIFICATION consistency, not whether the chosen block's
 * bytes are the real content, so two earlier hypotheses both painted wrong
 * content and were only caught by looking at the actual mGBA shot). Real
 * tiles $23-$28 (border/body checkerboard fill, the border notch, the
 * divider fill, the divider cap, "ID", "No") all come from CARDGFX, the
 * card's own small 6-tile block, indices 0..5. Real tiles $29-$2E (the 5
 * "STATUS"-word tiles, then the play-time colon) are a 6-tile run
 * immediately before LEADERS -- GBSCR_SRC_STATUSWORD is anchored there
 * (gbscr_block_off()/gbscr_block_bytes()), indices 0..5.
 *
 * BACKLOG #96 D11: the G2G_, G2X_, kG2BadgeBit and G2L_BADGES_WORD constants
 * moved to g2card_cells.h (shared with the new pure host-tested cell table
 * below -- one source of truth instead of two copies drifting apart).
 * G2G_FILL and G2G_NOTCH are still used here by g2card_border(), which
 * g2card_cells.h does not own (out of D11's scope, see that header's own
 * note). */

/* One frame counter, this screen's own -- toggles the play-time colon every
 * 32 VBlanks (the real game's own period, captured: 70 sampled frames showed
 * exactly 2 distinct tile ids, ~half on / half off). Not persisted; a fresh
 * visit always starts on the OFF phase, matching every captured dump (which
 * all happened to land on OFF -- this shell's own choice of start phase is
 * not itself something the oracle can confirm, only that both phases exist). */
static uint16_t g2_frame_ctr;

static void g2card_border(GbScreen* gs) {
  for (int x = 0; x < 20; x++) {
    gbscr_cell(gs, x, 0, GBSCR_SRC_CARDGFX, G2G_FILL);
    gbscr_cell(gs, x, 17, GBSCR_SRC_CARDGFX, G2G_FILL);
  }
  for (int y = 0; y < 18; y++) {
    gbscr_cell(gs, 0, y, GBSCR_SRC_CARDGFX, G2G_FILL);
    gbscr_cell(gs, 19, y, GBSCR_SRC_CARDGFX, G2G_FILL);
  }
  gbscr_cell(gs, 1, 7, GBSCR_SRC_CARDGFX, G2G_NOTCH);
  /* row 8 (the STATUS/BADGES strip) is border-fill EXCEPT the word graphic --
   * g2card_paint() overwrites cols 2-6 with STATUSWORD/LEADERS right after this. */
  for (int x = 1; x < 19; x++) gbscr_cell(gs, x, 8, GBSCR_SRC_CARDGFX, G2G_FILL);
  /* U3 fix (D1): the body (rows 9-16, cols 1-18) is NOT re-painted by either
   * page's cell set in full -- page 1 leaves the badge grid's cells blank
   * and page 2 leaves POKeDEX/PLAY TIME's cells blank, so without an
   * explicit clear here a page flip leaks the other page's content into
   * those cells (55 cells, confirmed via leak.py against the real capture).
   * Both g2card_paint_page1() and g2card_paint_page2() call this border
   * function before painting their own body, so blanking here runs on
   * every flip regardless of which page is being entered. Runs BEFORE the
   * bottom notch below: the notch at row 16 sits inside this blanked range
   * and must be drawn after the sweep, not before it. */
  for (int y = 9; y <= 16; y++)
    for (int x = 1; x <= 18; x++)
      gbscr_cell(gs, x, y, GBSCR_SRC_BLANK, 0);
  gbscr_cell(gs, 1, 16, GBSCR_SRC_CARDGFX, G2G_NOTCH);
}

/* The upper half (NAME/ID/MONEY/pic/divider) is IDENTICAL on both pages --
 * confirmed byte-for-byte across all four dumps -- so both callers share it. */
static void g2card_paint_upper(GbScreen* gs, const GbTrainer* t, bool female, bool has_corner) {
  gbscr_text(gs, 2, 2, "NAME/");
  gbscr_raw(gs, 7, 2, t->name_raw, GB_OT_GLYPHS);

  char buf[16];
  siprintf(buf, "%05u", (unsigned)t->trainer_id);
  gbscr_text(gs, 5, 4, buf);

  gbscr_text(gs, 2, 6, "MONEY");
  /* D6: the real card space-pads and RIGHT-aligns the value with the
   * currency sign immediately before the first digit (verified against the
   * real cart at 999999 / 1234 / 90 -- Gold_e1.sav/Gold_e2.sav), not a
   * fixed-width field with the currency sign always at col 7. */
  int mlen;
  if (t->money_ok) mlen = siprintf(buf, "%lu", (unsigned long)t->money);
  else             { siprintf(buf, "?"); mlen = 1; }
  for (int cx = 7; cx <= 12 - mlen; cx++) gbscr_cell(gs, cx, 6, GBSCR_SRC_BLANK, 0);
  gbscr_cell(gs, 13 - mlen, 6, GBSCR_SRC_FONT, 0xF0);     /* the currency sign */
  gbscr_text(gs, 14 - mlen, 6, buf);

  /* BACKLOG #96 D11: the CARDGFX ID/No glyphs, the 5x7 pic grid + the
   * (18,9) corner cell, and the divider fill+cap all come from the pure,
   * host-tested cell table (g2card_cells.c) instead of being inlined here --
   * host_gbcard_cells_test.c bounds-checks every one of these 51 cells
   * against the located block it reads from. */
  G2CardCell cells[G2CARD_UPPER_CELLS];
  int ncells = g2card_build_upper_cells(female, has_corner, cells);
  for (int i = 0; i < ncells; i++)
    gbscr_cell(gs, cells[i].x, cells[i].y, cells[i].src, cells[i].index);
}

static void g2card_paint_page1(GbScreen* gs, const GbTrainer* t, bool female, bool has_corner) {
  g2card_border(gs);
  g2card_paint_upper(gs, t, female, has_corner);

  /* BACKLOG #96 D11: the 5 STATUSWORD tiles + the FONT hint arrow, from the
   * same pure cell table (the blinking colon cell below stays inline --
   * it is time-dependent, not a fixed cell the table can express). */
  {
    G2CardCell cells[G2CARD_PAGE1_CELLS];
    int ncells = g2card_build_page1_cells(cells);
    for (int i = 0; i < ncells; i++)
      gbscr_cell(gs, cells[i].x, cells[i].y, cells[i].src, cells[i].index);
  }

  char buf[16];
  /* BACKLOG #96 D10: the real card clears rows 9-10 (the POKeDEX label +
   * count) when STATUSFLAGS_POKEDEX_F is off (GbTrainer.has_pokedex) --
   * g2card_border()'s row 9-16 blank sweep above already ran first, so
   * skipping both gbscr_text() calls here leaves the row genuinely blank,
   * not stale content from a previous page/save. */
  if (t->has_pokedex) {
    gbscr_text(gs, 2, 10, "POK\xC3\xA9""DEX");
    siprintf(buf, "%3u", (unsigned)t->dex_owned);
    gbscr_text(gs, 15, 10, buf);
  }

  gbscr_text(gs, 2, 12, "PLAY TIME");
  /* D7: hours are RIGHT-aligned in a 4-wide field (cols 11-14), the colon
   * is at a FIXED column (15), and minutes are a fixed 2-digit field
   * (cols 16-17) -- verified against the real cart (Gold_e1.sav hours=7,
   * Gold_e2.sav hours=123). The body-wide blank in g2card_border() already
   * clears row 12 before this runs, so the unused cells left of a short
   * hours value need no separate blanking. */
  int hlen = siprintf(buf, "%u", (unsigned)t->playtime.hours);
  if (hlen > 4) hlen = 4;   /* clamp to the field width */
  gbscr_text(gs, 15 - hlen, 12, buf);
  bool colon_on = ((g2_frame_ctr >> 5) & 1u) != 0;
  if (colon_on) gbscr_cell(gs, 15, 12, GBSCR_SRC_STATUSWORD, G2X_COLON);
  else          gbscr_cell(gs, 15, 12, GBSCR_SRC_BLANK, 0);
  siprintf(buf, "%02u", (unsigned)t->playtime.minutes);
  gbscr_text(gs, 16, 12, buf);

  gbscr_text(gs, 12, 15, "BADGES");
  /* the (r) hint arrow at (18,15) is in the page-1 cell table above */
}

static void g2card_paint_page2(GbScreen* gs, const GbTrainer* t, bool female, bool has_corner) {
  g2card_border(gs);
  /* the upper half is repainted here too -- U2b's dirty-cell idempotence
   * means this costs nothing extra to blit, and it is what lets L/R flip
   * pages on the SAME gbscr_open() without a second decode of the pic.
   * `female` must be the SAME value page 1 was painted with -- it was
   * hardcoded false here (U3 bug: Kris on Crystal saw Chris's photo on
   * page 2). No oracle exists for Kris (Gold has no gender branch, and no
   * captured Crystal save uses her), so this is fixed by reading the call
   * site rather than a pixel compare: pdna_gbtrainer_gen2_card()'s own
   * `female` local (post fail-safe retry) is threaded through instead of a
   * literal false. Same for `has_corner` (BACKLOG #125) -- the caller's post-
   * retry local, not re-derived here. */
  g2card_paint_upper(gs, t, female, has_corner);

  /* BACKLOG #96 D11: the LEADERS "BADGES" word, the 8-leader diploma grid,
   * and the per-badge 2x2 overlay (U3 accepted deviation -- a STATIC overlay
   * over the face's centre when owned; the real game animates this as an OAM
   * sprite instead) all come from the pure cell table now. */
  bool badge_owned[8];
  for (int k = 0; k < 8; k++) badge_owned[k] = gbtr_badge_get(t, false, kG2BadgeBit[k]);

  G2CardCell cells[G2CARD_PAGE2_CELLS_MAX];
  int ncells = g2card_build_page2_cells(badge_owned, cells);
  for (int i = 0; i < ncells; i++)
    gbscr_cell(gs, cells[i].x, cells[i].y, cells[i].src, cells[i].index);
}

static void g2card_sel_rect(int page, int sel, int* x, int* y, int* w, int* h) {
  if (page == 0) {
    switch (sel) {
      case G2C_NAME:  *x = 7;  *y = 2;  *w = GB_OT_GLYPHS; *h = 1; break;
      case G2C_ID:    *x = 5;  *y = 4;  *w = 5;             *h = 1; break;
      case G2C_MONEY: *x = 8;  *y = 6;  *w = 6;             *h = 1; break;
      default:        *x = 11; *y = 12; *w = 7;             *h = 1; break;   /* TIME */
    }
  } else {
    int row = sel / 4, col = sel % 4;
    *x = 2 + col * 4; *y = 10 + row * 3; *w = 4; *h = 3;
  }
}

static void g2card_edit_sel(GbTrainer* t, int page, int sel) {
  if (page == 0) {
    switch (sel) {
      case G2C_NAME:  gbtr_edit_row(t, false, GBTR_NAME);  break;
      case G2C_ID:    gbtr_edit_row(t, false, GBTR_ID);    break;
      case G2C_MONEY: gbtr_edit_row(t, false, GBTR_MONEY); break;
      default:        gbtr_edit_row(t, false, GBTR_TIME);  break;
    }
  } else {
    gbtr_badge_toggle(t, false, kG2BadgeBit[sel]);
  }
}

/* Gold/Silver/Crystal's own trainer card -- the Gen-2 sibling of
 * pdna_gbtrainer_gen1_card() above; same contract (returns true iff B asked
 * for a commit, falls back to the plain row-list page with an honest header
 * on any shell refusal). `female` (Chris vs Kris) comes from the save's own
 * gender field (has_gender && gender) -- Gold has no gender branch (its
 * cardpic_f is always 0, so `female` is forced false regardless of what the
 * caller passes, same fail-safe cardpic_colmajor already gives rom_gbui.c). */
__attribute__((noinline))
static bool pdna_gbtrainer_gen2_card(GbTrainer* t, bool can_edit, bool female) {
  /* BACKLOG #128: ONE open instead of two (was: open for the requested
   * gender's pic, fail-safe-retry as Chris if that pic's block failed to
   * locate; then a SEPARATE retry adding CARDCORNER once gu.cardcorner was
   * known) -- that cost a second f_open/f_close of the ROM, a second .loc
   * cache-file read, ~10-15 anchor-revalidation reads, and a full refill of
   * every cache block, every single Gen-2 card visit.
   *
   * CARDPIC_M is in need_mask (required, ALWAYS) so a female visit whose
   * cardpic_f block cannot be located still has Chris's own pic cached to
   * fall back to -- the exact outcome the old two-open fail-safe produced,
   * without a second open. CARDPIC_F (only requested when female) and
   * CARDCORNER (always requested -- 0 on Gold, so it is simply absent there)
   * are opt_mask: gbscr_cache_plan() skips either one silently when its own
   * located offset is 0, rather than failing the whole open the way the SAME
   * bit in need_mask would (BACKLOG #128, pdna_gbscreen.c). */
  uint32_t need_mask = GBSCR_NEED_CARDGFX | GBSCR_NEED_STATUSWORD |
                       GBSCR_NEED_LEADERS | GBSCR_NEED_BADGES | GBSCR_NEED_CARDPIC_M;
  uint32_t opt_mask = GBSCR_OPT_CARDCORNER | (female ? GBSCR_OPT_CARDPIC_F : 0u);
  uint32_t shell_need = gbscr_tail_need(PDNA_GEN2, need_mask, opt_mask);
  uint8_t* tail = gb12_arena_tail(shell_need);

  GbScreen gs;
  const char* reason = 0;
  bool ok = gbscr_open(PDNA_GEN2, &gs, tail, shell_need, need_mask, opt_mask, &reason);
  /* Same fail-safe outcome as the old two-open code: a save claims female on
   * a ROM whose cardpic_f block did not get cached (should not happen once
   * cardpic_f is non-zero -- rom_gbui.c's own cross-check ties it to the
   * SAME anchor as cardpic_m -- but CARDPIC_F is opt_mask now, so `ok` can
   * still be true even when it is absent) -- paint Chris instead, no retry,
   * no second open; CARDPIC_M's block is already cached either way. */
  female = female && ok && gbscr_has_block(&gs, GBSCR_SRC_CARDPIC_F);
  bool has_corner = ok && gbscr_has_block(&gs, GBSCR_SRC_CARDCORNER);
  if (!ok) {
    gb12_arena_tail_release();
    return pdna_gbtrainer_plain(t, false, can_edit, PDNA_GBTR_FALLBACK_TITLE,
                                reason ? reason : PDNA_GBSCR_REASON_UNAVAILABLE);
  }

  static const char* const kLegendEdit[4] = {
    PDNA_GBTR_ACT_EDIT, PDNA_GBTR_ACT_SAVE, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  static const char* const kLegendView[4] = {
    0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, PDNA_GBTR_ACT_MORE
  };
  gbscr_set_legend(&gs, can_edit ? kLegendEdit : kLegendView);

  int page = 0, sel = 0;
  bool want_commit = false;
  g2_frame_ctr = 0;
  g2card_paint_page1(&gs, t, female, has_corner);

  for (;;) {
    gbscr_flush(&gs, 0);

    int cx, cy, cw, ch;
    g2card_sel_rect(page, sel, &cx, &cy, &cw, &ch);
    int px0, py0, px1, py1;
    gbscr_cell_rect(cx, cy, cw, ch, &px0, &py0, &px1, &py1);
    m3_frame(px0, py0, px1, py1, GBCARD_CSEL);

    u16 k = 0;
    for (;;) {
      s_vsync();
      g2_frame_ctr++;
      if (page == 0 && (g2_frame_ctr & 31u) == 0) {
        bool colon_on = ((g2_frame_ctr >> 5) & 1u) != 0;
        /* D7: the colon is a FIXED column (15), not hours-length-dependent. */
        if (colon_on) gbscr_cell(&gs, 15, 12, GBSCR_SRC_STATUSWORD, G2X_COLON);
        else          gbscr_cell(&gs, 15, 12, GBSCR_SRC_BLANK, 0);
        gbscr_flush(&gs, 0);
        /* D8: gbscr_flush() repaints every dirty tile cell, which erases the
         * m3_frame() cursor rect drawn below the main loop's own flush (that
         * rect is not part of the tile buffer) -- redraw it every blink so
         * the cursor survives instead of vanishing on the OFF-phase tick. */
        m3_frame(px0, py0, px1, py1, GBCARD_CSEL);
      }
      k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_SELECT | KEY_START);
      if (k) break;
    }
    if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
    else if (k & KEY_A) snd_ok();
    else if (k & KEY_B) snd_back();

    if (k & KEY_SELECT) { gbscr_toggle_scale(&gs); continue; }
    if (k & KEY_START) {
      gbscr_close(&gs);
      gb12_arena_tail_release();
      return pdna_gbtrainer_plain(t, false, can_edit, 0, 0);
    }
    if (k & KEY_B) { want_commit = true; break; }

    if (k & (KEY_LEFT | KEY_RIGHT)) {
      page ^= 1;
      sel = 0;
      if (page == 0) g2card_paint_page1(&gs, t, female, has_corner);
      else           g2card_paint_page2(&gs, t, female, has_corner);
      gbscr_mark_all_dirty(&gs);
      continue;
    }

    int old_sel = sel;
    int selmax = (page == 0) ? G2C_P1_MAX : G2C_P2_MAX;
    if (k & KEY_UP)        sel = (sel > 0) ? sel - 1 : selmax - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % selmax;
    else if (can_edit && (k & KEY_A)) {
      g2card_edit_sel(t, page, sel);
      if (page == 0) g2card_paint_page1(&gs, t, female, has_corner);
      else           g2card_paint_page2(&gs, t, female, has_corner);
      gbscr_mark_all_dirty(&gs);
    }
    if (sel != old_sel) gbscr_mark_all_dirty(&gs);
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

  /* U3: BOTH generations now try their OWN real card first (the GB-screen
   * shell over the user's own ROM), falling back to the plain row-list page
   * (with an honest header naming the refusal) only when the shell itself
   * refuses -- design sec 3.5. The Emerald-art card (pdna_gbtrainer_card,
   * card_bg-gated) is retired for Gen 2 along with it -- see that function's
   * own comment. Crystal's `female` (Chris vs Kris) comes straight from the
   * save's own gender field; Gold has no gender branch, so `t.gender` is
   * whatever gbt_read() left it at (false unless has_gender, which Gold never
   * sets) and pdna_gbtrainer_gen2_card()'s own cardpic_f-missing fail-safe
   * covers the rest. */
  bool want_commit = false;
  if (gen1) {
    want_commit = pdna_gbtrainer_gen1_card(&t, can_edit);
  } else {
    want_commit = pdna_gbtrainer_gen2_card(&t, can_edit, t.has_gender && t.gender != 0);
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
