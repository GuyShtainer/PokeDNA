/* pdna_gbsummary.c — inline VIEW + EDIT of a Game Boy Pokemon over its NATIVE
 * GbEditMon, styled after the retail Gen-1/2 summary screens. See pdna_gbsummary.h
 * for the contract; docs/GEN12-EDIT-DESIGN.md section 3.3 for why this reads/writes
 * the native record and never the lossy Gen-3-converted copy the box grid shows.
 *
 * Field ORDER is taken from (clean-room — text placement only, no ripped art):
 *   pokered/engine/pokemon/status_screen.asm:106-169   (page 1: HP/status/type/OT/lvl)
 *                                             :243-314  (page 2: stats box + moves/PP)
 *   pokecrystal/engine/pokemon/stats_screen.asm:590-627 (pink: status/type/pokerus)
 *                                              :629-660  (pink: EXP/level-up)
 *                                              :745-780  (green: item/move list)
 *                                              :788+     (blue: OT info + stats)
 * This screen regroups those fields into its OWN three cards (INFO / STATS / MOVES)
 * rather than reproducing the retail page split byte-for-byte — see pdna_gbsummary.h.
 *
 * NO NEW STATICS (docs/GEN12-EDIT-DESIGN.md 3.4: "EWRAM free at HEAD: 612 bytes").
 * The field registry and the repaint shadow both live on THIS FUNCTION'S OWN STACK
 * FRAME (locals in pdna_gbsummary(), threaded through the static render helpers by
 * pointer) — never file-scope statics, unlike pdna_summary.c's g_slot/g_edit/g_ivh.
 *
 * Diff-render, review fix: render() takes a `full` flag. A change to the record, an
 * overlay (ui_clear_gen() moved), the card, or edit-mode itself forces `full` — a real
 * ui_clear() plus the VIEW/EDIT chip, the dots, `note` and the footer, none of which
 * change on a bare cursor move. Every OTHER repaint (a d-pad move between fields, or a
 * value edited in place) is partial: only the y=HDR_Y..FOOT_RULE_Y band (the header
 * line, the card title, and the card body) is erased and redrawn — a fill_rect, not a
 * full-screen clear, so the chip/dots/note/footer are left alone and the active
 * display's rows outside that band never repaint on every single keypress. */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "pdna_gbsummary.h"
#include "gb_editor.h"
#include "pdna_gbedit.h"     /* gbedit_confirm/press/adjust_checked/is_dv_field (shared) */
#include "ui.h"
#include "data_tables.h"     /* pk_species_name, pk_species_type1/2 */
#include "type_icons.h"      /* TYPE_ICON_W/H */
#include "pdna_layout.h"
#include "snd.h"

#define NCARDS       3
#define CARD_INFO    0
#define CARD_STATS   1
#define CARD_MOVES   2

#define ROW_Y0   34            /* first row, below the y=19 header rule (pdna_gbedit's
                                 * own convention: title 0..9, header 10..18, rule 19) */
#define ROW_H     9
#define TITLE_Y  22
#define HDR_Y    10
#define RULE_Y   19
#define FOOT_RULE_Y 151
#define FOOT_Y   152

/* Registered editable field slots for the CURRENT card, on the caller's stack. The
 * real worst case across every card is 12 (MOVES: 4 moves x {move, PP, PP Up}); 14
 * leaves headroom without a caller having to reason about the exact count. reg()
 * silently drops anything past this rather than corrupting adjacent memory — safe by
 * construction, since 14 already exceeds every card's real count today. */
#define MAX_SLOT 14
typedef struct { uint8_t field, x, y, w; } GbSlot;

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

/* pdna_summary.c's draw_dots, duplicated (not exported: that file's own is static) —
 * a graphical card indicator draws no new string, so it needs no textfit macro. */
static void draw_dots(int x, int y, int n, int active) {
  for (int i = 0; i < n; i++) {
    int cx = x + i * 7;
    if (i == active) ui_fill_rect(cx, y, 5, 5, UI_TITLE);
    else { ui_fill_rect(cx, y, 5, 5, UI_PANEL); m3_frame(cx, y, cx + 4, y + 4, UI_BORDER); }
  }
}

static void reg(GbSlot* slot, int* n, int field, int x, int y, int w) {
  if (*n < MAX_SLOT) { slot[*n].field = (uint8_t)field; slot[*n].x = (uint8_t)x;
                       slot[*n].y = (uint8_t)y; slot[*n].w = (uint8_t)w; (*n)++; }
}

/* A single GBE_* field, drawn at the shared PDNA_EDIT_LBL_X/VAL_X columns exactly
 * like pdna_gbedit.c's own row_paint — same label/value split, same truncation.
 * `fsel` is the CURRENTLY selected slot index (into the array being built), known
 * up front by the caller; the row draws its own highlight inline rather than via a
 * second pass, which is what lets this file carry no save-under pixel buffer. */
static void field_row(const GbEditMon* e, int field, int y, bool editing, int fsel,
                      GbSlot* slot, int* n) {
  int idx = *n;
  bool sel = editing && idx == fsel;
  char val[GBE_VALUE_MAX], vt[PDNA_EDIT_VAL_COLS * 4 + 1];
  gbe_value(e, field, val, sizeof val);
  ui_truncate(vt, val, PDNA_EDIT_VAL_COLS);
  if (sel) ui_panel(PDNA_EDIT_VAL_X - 2, y - 1, UI_SCR_W - PDNA_EDIT_VAL_X, UI_ROW_H + 1,
                    UI_SEL, UI_TITLE);
  ui_text(PDNA_EDIT_LBL_X, y, sel ? UI_SELTEXT : UI_DIM, gbe_label_of(e, field));
  ui_text(PDNA_EDIT_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, vt);
  reg(slot, n, field, PDNA_EDIT_VAL_X, y, UI_SCR_W - PDNA_EDIT_VAL_X);
}

/* A display-only full-width line (species, type, EXP, ...) — never registered. */
static void disp_row(int y, u16 col, const char* s) {
  char t[36];
  ui_truncate(t, s, 34);
  ui_text(PDNA_EDIT_LBL_X, y, col, t);
}

static void val_row(int y, const char* label, u16 col, const char* val) {
  char vt[PDNA_EDIT_VAL_COLS * 4 + 1];
  ui_truncate(vt, val, PDNA_EDIT_VAL_COLS);
  ui_text(PDNA_EDIT_LBL_X, y, UI_DIM, label);
  ui_text(PDNA_EDIT_VAL_X, y, col, vt);
}

static const char* gender_sym(int g) { return g == 0 ? "M" : g == 1 ? "F" : "-"; }

/* GBE_DVA/DVD/DVS/DVC -> the GB_* stat index gb_get_dv() takes — the same mapping
 * gb_editor.c's own (private) dv_stat() uses, duplicated here in the 4 lines it
 * takes rather than exporting a helper for just this. */
static int dv_stat_of(int f) {
  switch (f) {
    case GBE_DVA: return GB_ATK;
    case GBE_DVD: return GB_DEF;
    case GBE_DVS: return GB_SPE;
    case GBE_DVC: return GB_SPC;
    default:      return GB_HP;
  }
}

static const char* gbsum_g1_status_text(uint8_t st) {
  if (st & 0x08) return PDNA_GBSUM_ST_PSN;
  if (st & 0x10) return PDNA_GBSUM_ST_BRN;
  if (st & 0x20) return PDNA_GBSUM_ST_FRZ;
  if (st & 0x40) return PDNA_GBSUM_ST_PAR;
  if (st & 0x07) return PDNA_GBSUM_ST_SLP;
  return PDNA_GBSUM_ST_OK;
}

/* Gen-1's own type ids are NOT Gen-3's (gb_edit.h's GbGen1Base comment). Mapping onto
 * Gen 3's numbering so a Gen-1 mon draws the SAME ui_type_chip a Gen-2 one does,
 * instead of a raw "T14"-style id:
 *   0x00-0x05 NORMAL..ROCK    -> identity (both tables agree here)
 *   0x06                      -> Gen 1's unused BIRD slot: no Gen-3 equivalent
 *   0x07-0x09 BUG/GHOST/STEEL -> t-1  (Gen 3's BUG=6, GHOST=7, STEEL=8)
 *   0x14-0x1B FIRE..DARK      -> t-0x0A (Gen 3's FIRE=10 .. DARK=17)
 * Returns -1 for 0x06 or any id outside those ranges (a corrupt/hacked byte) — the
 * caller skips drawing a chip rather than showing a wrong one. */
static int g1_to_g3_type(uint8_t t) {
  if (t <= 0x05u) return t;
  if (t >= 0x07u && t <= 0x09u) return t - 1;
  if (t >= 0x14u && t <= 0x1Bu) return t - 0x0A;
  return -1;
}

/* ---- Card 0: INFO ------------------------------------------------------------- */

static void card_info_top(const GbEditMon* e, bool editing, int fsel, GbSlot* slot,
                          int* n, int* y_io) {
  char b[48];
  int y = *y_io;
  uint16_t dex = gb_get_species_dex(e);

  siprintf(b, "#%03u %s", (unsigned)dex, dex ? pk_species_name(dex) : "???");
  val_row(y, PDNA_GBSUM_LBL_SPECIES, UI_TEXT, b);
  y += ROW_H;

  field_row(e, GBE_NICK, y, editing, fsel, slot, n); y += ROW_H;
  field_row(e, GBE_LEVEL, y, editing, fsel, slot, n); y += ROW_H;

  /* Both generations now draw real chips (review fix): Gen 1's raw bytes go through
   * g1_to_g3_type() first; a byte that maps to nothing (0x06, or a corrupt id) simply
   * draws no chip rather than a wrong one. */
  uint8_t t1 = 0, t2 = 0;
  bool has1 = false, has2 = false;
  if (e->gen == GB_GEN2) {
    t1 = pk_species_type1(dex); t2 = pk_species_type2(dex);
    has1 = true; has2 = (t2 != t1);
  } else {
    int m1 = g1_to_g3_type(gb_get_gen1_type1(e));
    int m2 = g1_to_g3_type(gb_get_gen1_type2(e));
    has1 = m1 >= 0; if (has1) t1 = (uint8_t)m1;
    has2 = m2 >= 0 && m2 != m1; if (has2) t2 = (uint8_t)m2;
  }
  ui_text(PDNA_EDIT_LBL_X, y, UI_DIM, PDNA_GBSUM_LBL_TYPE);
  if (has1) ui_type_chip(PDNA_EDIT_VAL_X, y - 2, TYPE_ICON_W, TYPE_ICON_H, t1);
  if (has2) ui_type_chip(PDNA_EDIT_VAL_X + TYPE_ICON_W + 4, y - 2, TYPE_ICON_W, TYPE_ICON_H, t2);
  y += TYPE_ICON_H + 2;

  if (e->gen == GB_GEN1) {
    val_row(y, PDNA_GBSUM_LBL_STATUS, gb_get_gen1_status(e) ? UI_WARN : UI_OK,
            gbsum_g1_status_text(gb_get_gen1_status(e)));
    y += ROW_H;
  }

  {
    GbDvEffects fx; gb_dv_effects_of(e, &fx);
    siprintf(b, PDNA_GBSUM_SEXSHINY_FMT, gender_sym(fx.gender),
             fx.shiny ? PDNA_GBSUM_SHINY_YES : PDNA_GBSUM_SHINY_NO);
    disp_row(y, fx.shiny ? UI_OK : UI_TEXT, b);
    y += ROW_H;
  }
  *y_io = y;
}

static void card_info_bottom(const GbEditMon* e, bool editing, int fsel, GbSlot* slot,
                             int* n, int y) {
  char b[48];
  field_row(e, GBE_OT, y, editing, fsel, slot, n); y += ROW_H;
  field_row(e, GBE_OTID, y, editing, fsel, slot, n); y += ROW_H;

  if (e->gen == GB_GEN2) {
    field_row(e, GBE_ITEM, y, editing, fsel, slot, n); y += ROW_H;
    field_row(e, GBE_FRIEND, y, editing, fsel, slot, n); y += ROW_H;

    /* Pokerus (pokecrystal stats_screen.asm:590-604): high nibble strain, low nibble
     * days left (0 with a strain set = immune, past infection); byte 0 = never had it.
     * Display-only — this tree carries no gb_editor.h row/setter for it. */
    uint8_t raw = gb_get_pokerus(e);
    if (raw) {
      unsigned strain = raw >> 4, days = raw & 0x0F;
      if (days) siprintf(b, "S%u  %ud left", strain, days); else siprintf(b, "S%u  immune", strain);
      val_row(y, PDNA_GBSUM_LBL_PKRS, UI_OK, b);
      y += ROW_H;
    }
  }

  {
    uint16_t dex = gb_get_species_dex(e);
    uint32_t exp = gb_get_exp(e);
    uint8_t lvl = gb_get_level(e);
    if (lvl >= 100) siprintf(b, PDNA_GBSUM_EXP_MAX_FMT, (unsigned long)exp);
    else {
      uint32_t nxt = gb_exp_for_level(dex, (uint8_t)(lvl + 1));
      siprintf(b, PDNA_GBSUM_EXP_FMT, (unsigned long)exp, (unsigned long)(nxt > exp ? nxt - exp : 0));
    }
    disp_row(y, UI_TEXT, b);   /* "EXP " is baked into the _FMT macro itself (the whole
                               * line needs ~160 px; PDNA_EDIT_VAL_X's own 122 px value
                               * column does not have it) — full-width, no split label */
  }
}

static void card_info(const GbEditMon* e, bool editing, int fsel, GbSlot* slot, int* n) {
  int y = ROW_Y0;
  card_info_top(e, editing, fsel, slot, n, &y);
  card_info_bottom(e, editing, fsel, slot, n, y);
}

/* ---- Card 1: STATS -------------------------------------------------------------- */

/* One stat's row: label / computed value (or "-" for a box record) / DV (editable,
 * or GBE_DVH dim for HP) / stat exp (editable). `dv_field` may be GBE_DVH (shown,
 * never registered — gb_editor.h: "DERIVED, shown, not editable"). */
static void stat_row(const GbEditMon* e, const char* label, int stat_i, int dv_field,
                     int se_field, int y, bool editing, int fsel, GbSlot* slot, int* n) {
  char b[24];
  ui_text(PDNA_GBSUM_STAT_LBL_X, y, UI_DIM, label);

  if (e->is_party) {
    if (stat_i == GB_HP) siprintf(b, PDNA_GBSUM_STAT_CURMAX_FMT,
                                  (unsigned)gb_get_current_hp(e), (unsigned)gb_get_stat(e, GB_HP));
    else                 siprintf(b, "%u", (unsigned)gb_get_stat(e, stat_i));
  } else {
    siprintf(b, "%s", PDNA_GBSUM_STAT_DASH);
  }
  ui_text(PDNA_GBSUM_STAT_VAL_X, y, UI_TEXT, b);

  if (dv_field == GBE_DVH) {
    siprintf(b, PDNA_GBSUM_STAT_DV_FMT, (unsigned)gb_get_dv(e, GB_HP));
    ui_text(PDNA_GBSUM_STAT_DV_X, y, UI_DIM, b);
  } else {
    int idx = *n;
    bool sel = editing && idx == fsel;
    siprintf(b, PDNA_GBSUM_STAT_DV_FMT, (unsigned)gb_get_dv(e, dv_stat_of(dv_field)));
    if (sel) ui_panel(PDNA_GBSUM_STAT_DV_X - 2, y - 1, 40, UI_ROW_H + 1, UI_SEL, UI_TITLE);
    ui_text(PDNA_GBSUM_STAT_DV_X, y, sel ? UI_SELTEXT : UI_TEXT, b);
    reg(slot, n, dv_field, PDNA_GBSUM_STAT_DV_X, y, 36);
  }

  {
    int idx = *n;
    bool sel = editing && idx == fsel;
    siprintf(b, PDNA_GBSUM_STAT_SE_FMT, (unsigned)gb_get_statexp(e, se_field));
    if (sel) ui_panel(PDNA_GBSUM_STAT_SE_X - 2, y - 1, UI_SCR_W - PDNA_GBSUM_STAT_SE_X,
                      UI_ROW_H + 1, UI_SEL, UI_TITLE);
    ui_text(PDNA_GBSUM_STAT_SE_X, y, sel ? UI_SELTEXT : UI_TEXT, b);
    reg(slot, n, GBE_SE0 + se_field, PDNA_GBSUM_STAT_SE_X, y, UI_SCR_W - PDNA_GBSUM_STAT_SE_X);
  }
}

static void card_stats(const GbEditMon* e, bool editing, int fsel, GbSlot* slot, int* n) {
  int y = ROW_Y0;
  /* Column mini-headers share the card title's own baseline (TITLE_Y) — they sit far
   * enough right (x=98/142) of the title text ("STATS", 5 cols/40 px) to never collide. */
  ui_text(PDNA_GBSUM_STAT_DV_X, TITLE_Y, UI_DIM, PDNA_GBSUM_HDR_DV);
  ui_text(PDNA_GBSUM_STAT_SE_X, TITLE_Y, UI_DIM, PDNA_GBSUM_HDR_SE);

  stat_row(e, PDNA_GBSUM_STAT_HP,  GB_HP,  GBE_DVH, 0, y, editing, fsel, slot, n); y += ROW_H;
  stat_row(e, PDNA_GBSUM_STAT_ATK, GB_ATK, GBE_DVA, 1, y, editing, fsel, slot, n); y += ROW_H;
  stat_row(e, PDNA_GBSUM_STAT_DEF, GB_DEF, GBE_DVD, 2, y, editing, fsel, slot, n); y += ROW_H;
  stat_row(e, PDNA_GBSUM_STAT_SPE, GB_SPE, GBE_DVS, 3, y, editing, fsel, slot, n); y += ROW_H;
  if (e->gen == GB_GEN2) {
    /* Gen 2 splits Special into SpA/SpD in the COMPUTED party stat block only — both
     * read the same stored Spc DV and Spc stat exp (gb_edit.h's own header comment).
     * gb_get_stat's party-stat array therefore has indices 4 (SpA) and 5 (SpD); the
     * DV/stat-exp column intentionally registers the SAME field (GBE_DVC/GBE_SE4)
     * twice, once per row — editing either row edits the one stored value both use. */
    stat_row(e, PDNA_GBSUM_STAT_SPA, 4, GBE_DVC, 4, y, editing, fsel, slot, n); y += ROW_H;
    stat_row(e, PDNA_GBSUM_STAT_SPD, 5, GBE_DVC, 4, y, editing, fsel, slot, n); y += ROW_H;
  } else {
    stat_row(e, PDNA_GBSUM_STAT_SPC, 4, GBE_DVC, 4, y, editing, fsel, slot, n); y += ROW_H;
  }

  if (!e->is_party) disp_row(y + 2, UI_DIM, PDNA_GBSUM_BOX_STAT_NOTE);
}

/* ---- Card 2: MOVES --------------------------------------------------------------- */

static void move_row(const GbEditMon* e, int i, int y, bool editing, int fsel,
                     GbSlot* slot, int* n) {
  char lbl[8];
  siprintf(lbl, PDNA_GBSUM_LBL_MOVE_FMT, (unsigned)(i + 1));
  int idx = *n;
  bool sel = editing && idx == fsel;
  char val[GBE_VALUE_MAX], vt[PDNA_EDIT_VAL_COLS * 4 + 1];
  gbe_value(e, GBE_MV0 + i, val, sizeof val);
  ui_truncate(vt, val, PDNA_EDIT_VAL_COLS);
  if (sel) ui_panel(PDNA_EDIT_VAL_X - 2, y - 1, UI_SCR_W - PDNA_EDIT_VAL_X, UI_ROW_H + 1,
                    UI_SEL, UI_TITLE);
  ui_text(PDNA_EDIT_LBL_X, y, UI_DIM, lbl);
  ui_text(PDNA_EDIT_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, vt);
  reg(slot, n, GBE_MV0 + i, PDNA_EDIT_VAL_X, y, UI_SCR_W - PDNA_EDIT_VAL_X);
}

static void pp_row(const GbEditMon* e, int i, int y, bool editing, int fsel,
                   GbSlot* slot, int* n) {
  char val[GBE_VALUE_MAX], vt[8 * 4 + 1], vt2[14 * 4 + 1];

  int idx = *n; bool sel = editing && idx == fsel;
  gbe_value(e, GBE_PP0 + i, val, sizeof val);
  ui_truncate(vt, val, (PDNA_GBSUM_UPS_LBL_X - PDNA_GBSUM_PP_VAL_X) / 8);
  if (sel) ui_panel(PDNA_GBSUM_PP_VAL_X - 2, y - 1, PDNA_GBSUM_UPS_LBL_X - PDNA_GBSUM_PP_VAL_X,
                    UI_ROW_H + 1, UI_SEL, UI_TITLE);
  ui_text(PDNA_GBSUM_PPROW_X, y, UI_DIM, PDNA_GBSUM_PP_LBL);
  ui_text(PDNA_GBSUM_PP_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, vt);
  reg(slot, n, GBE_PP0 + i, PDNA_GBSUM_PP_VAL_X, y, PDNA_GBSUM_UPS_LBL_X - PDNA_GBSUM_PP_VAL_X);

  idx = *n; sel = editing && idx == fsel;
  gbe_value(e, GBE_PPU0 + i, val, sizeof val);
  ui_truncate(vt2, val, (UI_SCR_W - PDNA_GBSUM_UPS_VAL_X) / 8);
  if (sel) ui_panel(PDNA_GBSUM_UPS_VAL_X - 2, y - 1, UI_SCR_W - PDNA_GBSUM_UPS_VAL_X,
                    UI_ROW_H + 1, UI_SEL, UI_TITLE);
  ui_text(PDNA_GBSUM_UPS_LBL_X, y, UI_DIM, PDNA_GBSUM_UPS_LBL);
  ui_text(PDNA_GBSUM_UPS_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, vt2);
  reg(slot, n, GBE_PPU0 + i, PDNA_GBSUM_UPS_VAL_X, y, UI_SCR_W - PDNA_GBSUM_UPS_VAL_X);
}

static void card_moves(const GbEditMon* e, bool editing, int fsel, GbSlot* slot, int* n) {
  int y = ROW_Y0;
  for (int i = 0; i < 4; i++) {
    move_row(e, i, y, editing, fsel, slot, n); y += ROW_H;
    pp_row(e, i, y, editing, fsel, slot, n);   y += ROW_H + 1;
  }
}

/* ---- shell: title/header/rule/footer + the one card ------------------------- */

static void render(const GbEditMon* e, int card, bool editing, bool can_edit,
                   const char* note, int fsel, GbSlot* slot, int* n, bool full) {
  char hdr[40], lt[29 * 4 + 1];

  if (full) {
    ui_clear();
    if (editing) { ui_fill_rect(0, 0, 50, 9, UI_WARN); ui_text(8, 1, UI_PANEL, PDNA_GBSUM_EDIT_CHIP); }
    else         { ui_text(4, 1, UI_DIM, PDNA_GBSUM_VIEW_CHIP); }
    /* The dots sit right after the widest of the two VIEW/EDIT chips (the EDIT fill is
     * 50 px wide) rather than at the top-right corner: `note` ("Gen 1/2 record") is
     * ALSO drawn there, right-aligned, and a screenshot caught the two overlapping —
     * the note is on every single visit, not just the sidecar-warning case. */
    draw_dots(56, 2, NCARDS, card);
    if (note) ui_ptext_right(UI_SCR_W - 4, 0, UI_WARN, note);

    ui_hline(0, FOOT_RULE_Y, UI_SCR_W, UI_BORDER);
    const char* foot = editing ? PDNA_GBSUM_FOOT_EDIT
                     : can_edit ? PDNA_GBSUM_FOOT_VIEW : PDNA_GBSUM_FOOT_VIEW_RO;
    ui_text(4, FOOT_Y, UI_DIM, foot);
  } else {
    /* Partial repaint: only the header line + card title + card body change on a bare
     * cursor move or an in-place edit. Erasing just this band (not a full ui_clear())
     * keeps the chip/dots/note/footer from flashing on every keypress. */
    ui_fill_rect(0, HDR_Y, UI_SCR_W, FOOT_RULE_Y - HDR_Y, UI_BG);
  }

  /* Always inside the erased band above (full or partial), so always redrawn. */
  gbe_header(e, hdr, sizeof hdr);
  ui_truncate(lt, hdr, 29);
  ui_text(4, HDR_Y, UI_DIRCLR, lt);
  ui_hline(0, RULE_Y, UI_SCR_W, UI_BORDER);

  const char* title = card == CARD_INFO ? PDNA_GBSUM_CARD_INFO
                     : card == CARD_STATS ? PDNA_GBSUM_CARD_STATS : PDNA_GBSUM_CARD_MOVES;
  ui_text(4, TITLE_Y, UI_TITLE, title);

  *n = 0;
  switch (card) {
    case CARD_INFO:  card_info(e, editing, fsel, slot, n); break;
    case CARD_STATS: card_stats(e, editing, fsel, slot, n); break;
    case CARD_MOVES: card_moves(e, editing, fsel, slot, n); break;
  }
}

/* ---- input dispatch, split out of pdna_gbsummary() to keep it under ~60 lines --- */

typedef struct {
  GbEditMon* e;
  bool can_edit, has_sidecar, editing, dirty, dv_warned;
  const char* note;
  int card, fsel;
  GbSlot* slot; int nslot;
  bool* saved; int* card_io;
} GbSumCtx;

/* SELECT: drop to the reviewed flat field-list editor (pdna_gbedit.c) — kept as the
 * fallback until this screen gets its own hardware pass (design doc, S2b note +
 * BACKLOG #41). Not gated on `editing`: opening it also lets a VIEW-mode visit jump
 * straight to editing without a separate A-press first.
 *
 * pdna_gbedit.h's own contract: a TRUE return means the user already saw ITS confirm
 * panel and chose to write — asking this screen's OWN confirm again on the way out
 * would be a second "write?" for the same edit, so a commit there ends the visit
 * immediately instead (returns true: the caller `return 0`s). A FALSE return means
 * "discard `e` and reload from the list" (pdna_gbedit.h) — this screen has no list to
 * reload from, so a snapshot taken just before the excursion stands in: any mutation
 * pdna_gbedit made to `e` before the user backed out with B is undone, leaving this
 * screen's own `dirty` exactly as it was walking in. */
static bool gbsum_select_fallback(GbSumCtx* c, bool* shadow_valid) {
  if (!c->can_edit) { snd_deny(); return false; }
  GbEditMon snap = *c->e;
  if (pdna_gbedit(c->e, c->note, c->has_sidecar)) {
    key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (c->card_io) *c->card_io = c->card;
    if (c->saved) *c->saved = true;
    return true;
  }
  *c->e = snap;
  *shadow_valid = false;             /* the sub-screen cleared/repainted the LCD */
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  return false;
}

/* EDIT-mode key dispatch. `dirty` is NOT set here any more (review fix): it is derived
 * in pdna_gbsummary()'s own loop from a memcmp against the pre-edit shadow, so a
 * cancelled osk/picker or a clamped no-op LEFT/RIGHT can never mark the record dirty
 * when not one byte actually changed. */
static void gbsum_edit_keys(GbSumCtx* c, u16 k) {
  if (k & KEY_B) { c->editing = false; c->fsel = 0; return; }
  if (k & (KEY_L | KEY_R)) {
    c->card = (c->card + (k & KEY_R ? 1 : NCARDS - 1)) % NCARDS; c->fsel = 0; return;
  }
  if (c->nslot && (k & KEY_A)) {
    gbedit_press(c->e, c->slot[c->fsel].field, c->has_sidecar, &c->dv_warned); return;
  }
  if (c->nslot && (k & KEY_LEFT)) {
    gbedit_adjust_checked(c->e, c->slot[c->fsel].field, -1, false, c->has_sidecar, &c->dv_warned);
    return;
  }
  if (c->nslot && (k & KEY_RIGHT)) {
    gbedit_adjust_checked(c->e, c->slot[c->fsel].field, +1, false, c->has_sidecar, &c->dv_warned);
    return;
  }
  if (k & KEY_UP)   { if (c->nslot) c->fsel = (c->fsel > 0) ? c->fsel - 1 : c->nslot - 1; return; }
  if (k & KEY_DOWN) { if (c->nslot) c->fsel = (c->fsel + 1) % c->nslot; }
}

/* VIEW-mode key dispatch. Returns true when the caller should `return *out` from
 * pdna_gbsummary() right now. */
static bool gbsum_view_keys(GbSumCtx* c, u16 k, u16 fresh, int* out) {
  if (k & KEY_A) {
    if (c->can_edit) { c->editing = true; c->fsel = 0; }
    else snd_deny();          /* review fix: A used to do nothing at all here */
    return false;
  }
  if ((k & (KEY_L | KEY_R)) || (fresh & (KEY_LEFT | KEY_RIGHT))) {
    int fwd = (k & KEY_R) || (fresh & KEY_RIGHT);
    c->card = (c->card + (fwd ? 1 : NCARDS - 1)) % NCARDS;
    return false;
  }
  if (k & (KEY_UP | KEY_DOWN | KEY_B)) {
    if (c->dirty) {
      if (gbedit_confirm(c->e)) { gbe_settle_stats(c->e); if (c->saved) *c->saved = true; }
      else if (c->saved) *c->saved = false;
    }
    key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (c->card_io) *c->card_io = c->card;
    *out = (k & KEY_B) ? 0 : ((k & KEY_DOWN) ? +1 : -1);
    return true;
  }
  return false;
}

static void gbsum_click(u16 fresh, bool editing) {
  if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
  else if (fresh & (KEY_L | KEY_R | KEY_SELECT)) snd_tab();
  else if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  else if (fresh & (KEY_LEFT | KEY_RIGHT)) { if (editing) snd_edit(); else snd_tab(); }
}

int pdna_gbsummary(GbEditMon* e, bool can_edit, bool start_editing, const char* note,
                    bool has_sidecar, bool* saved, int* card_io) {
  if (saved) *saved = false;
  if (!e) return 0;

  GbSumCtx c;
  c.e = e; c.can_edit = can_edit; c.has_sidecar = has_sidecar; c.note = note;
  c.card = (card_io && *card_io >= 0 && *card_io < NCARDS) ? *card_io : 0;
  c.editing = can_edit && start_editing;
  c.fsel = 0; c.dirty = false; c.dv_warned = false;
  c.saved = saved; c.card_io = card_io;

  GbEditMon shadow; memset(&shadow, 0, sizeof shadow);
  bool shadow_valid = false;
  uint32_t shadow_gen = 0;
  int shadow_card = -1, shadow_fsel = -2;
  bool shadow_edit = false;

  GbSlot slot[MAX_SLOT];
  c.slot = slot; c.nslot = 0;

  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);

  for (;;) {
    int want_fsel = c.editing ? c.fsel : -1;
    bool full = !shadow_valid || shadow_gen != ui_clear_gen() ||
                shadow_card != c.card || shadow_edit != c.editing;
    bool need = full || shadow_fsel != want_fsel || memcmp(&shadow, c.e, sizeof(GbEditMon)) != 0;
    if (need) {
      if (shadow_valid && memcmp(&shadow, c.e, sizeof shadow) != 0) c.dirty = true;
      /* Clamp BEFORE the paint (review): the slot count of the card being redrawn is the
       * one the previous render counted, so on the same card the frame is drawn at a valid
       * field. A card change resets fsel to 0 anyway; the post-render clamp stays as the
       * true bound once nslot is fresh. Unreachable today (U/D is mod nslot) -- defence. */
      if (c.editing && shadow_valid && shadow_card == c.card && c.nslot && c.fsel >= c.nslot)
        c.fsel = c.nslot - 1;
      render(c.e, c.card, c.editing, c.can_edit, c.note, c.fsel, c.slot, &c.nslot, full);
      if (c.editing && c.nslot && c.fsel >= c.nslot) c.fsel = c.nslot - 1;
      shadow = *c.e; shadow_gen = ui_clear_gen(); shadow_card = c.card;
      shadow_edit = c.editing; shadow_fsel = c.editing ? c.fsel : -1;
      shadow_valid = true;
    }

    u16 k, fresh;
    do { s_vsync(); fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    gbsum_click(fresh, c.editing);

    if (fresh & KEY_SELECT) {
      if (gbsum_select_fallback(&c, &shadow_valid)) return 0;
      continue;
    }

    if (c.editing) {
      gbsum_edit_keys(&c, k);
    } else {
      int out;
      if (gbsum_view_keys(&c, k, fresh, &out)) return out;
    }
  }
}
