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
 * Diff-render is therefore coarser than pdna_summary's: a change to the record, the
 * card, edit-mode, or the selected field repaints the WHOLE card body (the y=19..151
 * band) rather than pdna_summary's per-pixel outline; this screen has no portrait
 * animation to protect from flicker, so the coarser grain costs nothing visible. */
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

/* Registered editable field slots for the CURRENT card, on the caller's stack. */
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

/* ---- Card 0: INFO ---------------------------------------------------------- */

static void val_row(int y, const char* label, u16 col, const char* val) {
  char vt[PDNA_EDIT_VAL_COLS * 4 + 1];
  ui_truncate(vt, val, PDNA_EDIT_VAL_COLS);
  ui_text(PDNA_EDIT_LBL_X, y, UI_DIM, label);
  ui_text(PDNA_EDIT_VAL_X, y, col, vt);
}

static void card_info(const GbEditMon* e, bool editing, int fsel, GbSlot* slot, int* n) {
  char b[48];
  int y = ROW_Y0;
  uint16_t dex = gb_get_species_dex(e);

  siprintf(b, "#%03u %s", (unsigned)dex, dex ? pk_species_name(dex) : "???");
  val_row(y, PDNA_GBSUM_LBL_SPECIES, UI_TEXT, b);
  y += ROW_H;

  field_row(e, GBE_NICK, y, editing, fsel, slot, n); y += ROW_H;
  field_row(e, GBE_LEVEL, y, editing, fsel, slot, n); y += ROW_H;

  if (e->gen == GB_GEN2) {
    uint8_t t1 = pk_species_type1(dex), t2 = pk_species_type2(dex);
    ui_text(PDNA_EDIT_LBL_X, y, UI_DIM, PDNA_GBSUM_LBL_TYPE);
    ui_type_chip(PDNA_EDIT_VAL_X, y - 2, TYPE_ICON_W, TYPE_ICON_H, t1);
    if (t2 != t1) ui_type_chip(PDNA_EDIT_VAL_X + TYPE_ICON_W + 4, y - 2, TYPE_ICON_W, TYPE_ICON_H, t2);
    y += TYPE_ICON_H + 2;
  } else {
    uint8_t t1 = gb_get_gen1_type1(e), t2 = gb_get_gen1_type2(e);
    if (t2 == t1) siprintf(b, "T%02X", t1); else siprintf(b, "T%02X/T%02X", t1, t2);
    val_row(y, PDNA_GBSUM_LBL_G1TYPE, UI_TEXT, b);
    y += ROW_H;

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

  field_row(e, GBE_OT, y, editing, fsel, slot, n); y += ROW_H;
  field_row(e, GBE_OTID, y, editing, fsel, slot, n); y += ROW_H;

  if (e->gen == GB_GEN2) {
    field_row(e, GBE_ITEM, y, editing, fsel, slot, n); y += ROW_H;
    field_row(e, GBE_FRIEND, y, editing, fsel, slot, n); y += ROW_H;

    /* Pokerus (pokecrystal stats_screen.asm:590-604): high nibble strain, low nibble
     * days left (0 with a strain set = immune, past infection); byte 0 = never had it.
     * Display-only — this tree carries no gb_editor.h row/setter for it. */
    uint8_t raw = e->rec[0x1C];   /* pokecrystal ram.asm box_struct PokerusStatus;
                                   * verified against the shipping decoder,
                                   * gen2_save.c:490 "out->pokerus = rec[0x1C]". */
    if (raw) {
      unsigned strain = raw >> 4, days = raw & 0x0F;
      if (days) siprintf(b, "S%u  %ud left", strain, days); else siprintf(b, "S%u  immune", strain);
      val_row(y, PDNA_GBSUM_LBL_PKRS, UI_OK, b);
      y += ROW_H;
    }
  }

  {
    uint32_t exp = gb_get_exp(e);
    uint8_t lvl = gb_get_level(e);
    if (lvl >= 100) siprintf(b, PDNA_GBSUM_EXP_MAX_FMT, (unsigned long)exp);
    else {
      uint32_t nxt = gb_exp_for_level(dex, (uint8_t)(lvl + 1));
      siprintf(b, PDNA_GBSUM_EXP_FMT, (unsigned long)exp, (unsigned long)(nxt > exp ? nxt - exp : 0));
    }
    disp_row(y, UI_TEXT, b);   /* "EXP " is baked into the _FMT macro itself (below the
                               * whole line needs the ~160 px PDNA_EDIT_VAL_X's own 122 px
                               * value column does not have) — full-width, no split label */
  }
}

/* ---- Card 1: STATS ---------------------------------------------------------- */

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

/* ---- Card 2: MOVES ----------------------------------------------------------- */

static void card_moves(const GbEditMon* e, bool editing, int fsel, GbSlot* slot, int* n) {
  int y = ROW_Y0;
  char lbl[8];
  for (int i = 0; i < 4; i++) {
    siprintf(lbl, PDNA_GBSUM_LBL_MOVE_FMT, (unsigned)(i + 1));
    {
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
    y += ROW_H;

    {
      char b[GBE_VALUE_MAX];
      int idx = *n; bool sel = editing && idx == fsel;
      gbe_value(e, GBE_PP0 + i, b, sizeof b);
      if (sel) ui_panel(PDNA_GBSUM_PP_VAL_X - 2, y - 1, PDNA_GBSUM_UPS_LBL_X - PDNA_GBSUM_PP_VAL_X,
                        UI_ROW_H + 1, UI_SEL, UI_TITLE);
      ui_text(PDNA_GBSUM_PPROW_X, y, UI_DIM, PDNA_GBSUM_PP_LBL);
      ui_text(PDNA_GBSUM_PP_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, b);
      reg(slot, n, GBE_PP0 + i, PDNA_GBSUM_PP_VAL_X, y, PDNA_GBSUM_UPS_LBL_X - PDNA_GBSUM_PP_VAL_X);
    }
    {
      char b[GBE_VALUE_MAX];
      int idx = *n; bool sel = editing && idx == fsel;
      gbe_value(e, GBE_PPU0 + i, b, sizeof b);
      if (sel) ui_panel(PDNA_GBSUM_UPS_VAL_X - 2, y - 1, UI_SCR_W - PDNA_GBSUM_UPS_VAL_X,
                        UI_ROW_H + 1, UI_SEL, UI_TITLE);
      ui_text(PDNA_GBSUM_UPS_LBL_X, y, UI_DIM, PDNA_GBSUM_UPS_LBL);
      ui_text(PDNA_GBSUM_UPS_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, b);
      reg(slot, n, GBE_PPU0 + i, PDNA_GBSUM_UPS_VAL_X, y, UI_SCR_W - PDNA_GBSUM_UPS_VAL_X);
    }
    y += ROW_H + 1;
  }
}

/* ---- shell: title/header/rule/footer + the one card ------------------------- */

static void render(const GbEditMon* e, int card, bool editing, const char* note,
                   int fsel, GbSlot* slot, int* n) {
  char hdr[40], lt[29 * 4 + 1];

  ui_clear();
  if (editing) { ui_fill_rect(0, 0, 50, 9, UI_WARN); ui_text(8, 1, UI_PANEL, PDNA_GBSUM_EDIT_CHIP); }
  else         { ui_text(4, 1, UI_DIM, PDNA_GBSUM_VIEW_CHIP); }
  /* The dots sit right after the widest of the two VIEW/EDIT chips (the EDIT fill is
   * 50 px wide) rather than at the top-right corner: `note` ("Gen 1/2 record") is
   * ALSO drawn there, right-aligned, and a screenshot caught the two overlapping —
   * the note is on every single visit, not just the sidecar-warning case. */
  draw_dots(56, 2, NCARDS, card);
  if (note) ui_ptext_right(UI_SCR_W - 4, 0, UI_WARN, note);

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

  ui_hline(0, FOOT_RULE_Y, UI_SCR_W, UI_BORDER);
  const char* foot = editing ? PDNA_GBSUM_FOOT_EDIT
                   : PDNA_GBSUM_FOOT_VIEW; /* can_edit is folded into `editing`'s
                                            * reachability by the caller: VIEW-only
                                            * carts never see the EDIT chip either */
  ui_text(4, FOOT_Y, UI_DIM, foot);
}

int pdna_gbsummary(GbEditMon* e, bool can_edit, bool start_editing, const char* note,
                    bool has_sidecar, bool* saved, int* card_io) {
  if (saved) *saved = false;
  if (!e) return 0;

  int card = (card_io && *card_io >= 0 && *card_io < NCARDS) ? *card_io : 0;
  bool editing = can_edit && start_editing;
  int fsel = 0;
  bool dirty = false;
  bool dv_warned = false;

  GbEditMon shadow; memset(&shadow, 0, sizeof shadow);
  bool shadow_valid = false;
  uint32_t shadow_gen = 0;
  int shadow_card = -1, shadow_fsel = -2;
  bool shadow_edit = false;

  GbSlot slot[MAX_SLOT];
  int nslot = 0;

  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);

  for (;;) {
    int want_fsel = editing ? fsel : -1;
    bool need = !shadow_valid || shadow_gen != ui_clear_gen() || shadow_card != card ||
                shadow_edit != editing || shadow_fsel != want_fsel ||
                memcmp(&shadow, e, sizeof(GbEditMon)) != 0;
    if (need) {
      render(e, card, editing, note, fsel, slot, &nslot);
      if (editing && nslot && fsel >= nslot) fsel = nslot - 1;
      shadow = *e; shadow_gen = ui_clear_gen(); shadow_card = card;
      shadow_edit = editing; shadow_fsel = editing ? fsel : -1;
      shadow_valid = true;
    }

    u16 k, fresh;
    do { s_vsync(); fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT)) snd_tab();
    else if (fresh & KEY_A) snd_ok();
    else if (fresh & KEY_B) snd_back();
    else if (fresh & (KEY_LEFT | KEY_RIGHT)) { if (editing) snd_edit(); else snd_tab(); }

    if (fresh & KEY_SELECT) {
      /* Drop to the reviewed flat field-list editor (pdna_gbedit.c) — kept as the
       * fallback until this screen gets its own hardware pass (design doc, S2b note
       * + BACKLOG #41(d)). Not gated on `editing`: opening it also lets a VIEW-mode
       * visit jump straight to editing without a separate A-press first.
       *
       * pdna_gbedit.h's own contract: a TRUE return means the user already saw ITS
       * confirm panel and chose to write — asking this screen's OWN confirm again
       * on the way out would be a second "write?" for the same edit, so a commit
       * there ends the visit immediately instead. A FALSE return means "discard `e`
       * and reload from the list" (pdna_gbedit.h) — this screen has no list to
       * reload from, so a snapshot taken just before the excursion stands in: any
       * mutation pdna_gbedit made to `e` before the user backed out with B is
       * undone, leaving this screen's own `dirty` exactly as it was walking in. */
      if (!can_edit) { snd_deny(); continue; }
      GbEditMon snap = *e;
      if (pdna_gbedit(e, note, has_sidecar)) {
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        if (card_io) *card_io = card;
        if (saved) *saved = true;
        return 0;
      }
      *e = snap;
      shadow_valid = false;             /* the sub-screen cleared/repainted the LCD */
      key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
      continue;
    }

    if (editing) {
      if (k & KEY_B) { editing = false; fsel = 0; }
      else if (k & (KEY_L | KEY_R)) { card = (card + (k & KEY_R ? 1 : NCARDS - 1)) % NCARDS; fsel = 0; }
      else if (nslot && (k & KEY_A)) {
        gbedit_press(e, slot[fsel].field, has_sidecar, &dv_warned);
        dirty = true;
      }
      else if (nslot && (k & KEY_LEFT)) {
        gbedit_adjust_checked(e, slot[fsel].field, -1, false, has_sidecar, &dv_warned);
        dirty = true;
      }
      else if (nslot && (k & KEY_RIGHT)) {
        gbedit_adjust_checked(e, slot[fsel].field, +1, false, has_sidecar, &dv_warned);
        dirty = true;
      }
      else if (k & KEY_UP)   { if (nslot) fsel = (fsel > 0) ? fsel - 1 : nslot - 1; }
      else if (k & KEY_DOWN) { if (nslot) fsel = (fsel + 1) % nslot; }
    } else {
      if (k & KEY_A) { if (can_edit) { editing = true; fsel = 0; } }
      else if ((k & (KEY_L | KEY_R)) || (fresh & (KEY_LEFT | KEY_RIGHT))) {
        int fwd = (k & KEY_R) || (fresh & KEY_RIGHT);
        card = (card + (fwd ? 1 : NCARDS - 1)) % NCARDS;
      }
      else if (k & (KEY_UP | KEY_DOWN | KEY_B)) {
        if (dirty) {
          if (gbedit_confirm(e)) { gbe_settle_stats(e); if (saved) *saved = true; }
          else if (saved) *saved = false;
        }
        key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
        if (card_io) *card_io = card;
        if (k & KEY_B) return 0;
        return (k & KEY_DOWN) ? +1 : -1;
      }
    }
  }
}
