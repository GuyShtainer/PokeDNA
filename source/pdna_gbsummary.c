/* pdna_gbsummary.c — inline VIEW + EDIT of a Game Boy Pokemon over its NATIVE
 * GbEditMon, restyled to the SAME Gen-3 CARD chrome pdna_summary.c uses
 * (BACKLOG #41 slice E1, docs/SPRITE-ERA-DESIGN.md sec 3 — Guy, 2026-09-05:
 * "I prefer the summary edit design to be like we did for gen 3 ... [but with]
 * less editable stats"). See pdna_gbsummary.h for the public contract, which is
 * UNCHANGED from the earlier retail-page version this replaces.
 *
 * SHARED CHROME, exported from pdna_summary.c (pdna_summary.h): the whole-screen
 * gradient (pdna_summary_bg), the card-index dots (pdna_summary_draw_dots), the
 * moving-outline selection (pdna_summary_sel_frame_set/hide/drop, backed by that
 * file's OWN s_self_px buffer — safe to share because the two summary screens
 * are never both on screen at once), and — the piece that makes a Game Boy
 * Pokemon "look like" looking at one — the shared LEFT PANEL painter
 * (pdna_summary_draw_left): portrait via the origin-art router, dex no., name,
 * level + gender, species, type badges, egg/shiny/Pokerus tag.
 *
 * THE LEFT PANEL'S POKEMON IS A THROWAWAY CONVERSION, never the record this
 * screen reads or writes. gen12_convert.h is explicit that there was never an
 * official Gen 1/2 -> Gen 3 transfer; gbsum_convert_left() below builds one
 * anyway, purely so the shared painter has a PkMon to draw, and decodes it back
 * out immediately — it is never committed, never shown to gb_edit_commit, and
 * never touches the box grid's own identity matching. THE CARDS ON THE RIGHT
 * read and write the NATIVE `e` exactly as before; the ORIGIN card says so in
 * plain words (docs/SPRITE-ERA-DESIGN.md sec 3's "honest converted-copy line").
 *
 * LESS EDITABLE, on purpose: four cards (INFO / SKILLS / MOVES / ORIGIN)
 * against Gen-3's eight — no separate IV/EV cards (Game Boy DVs and stat exp
 * share one SKILLS grid instead), no contest stats or contest moves (neither
 * generation has them), no species/type/gender/shiny row on INFO (the shared
 * left panel already shows all four).
 *
 * DIFF-RENDER, matching pdna_summary.c's OWN two-tier idiom rather than the
 * retail-page version's single erase-band:
 *   - a bare field-cursor move (fsel changes, nothing else) repaints NOTHING —
 *     only the moving outline updates (pdna_summary_sel_frame_set's save-under),
 *     the same "just the outline" behaviour draw_left_conditional's header
 *     comment in pdna_summary.c measures at ~48x fewer framebuffer bytes and,
 *     on real hardware, zero extra SD reads;
 *   - the card body (chip/dots/title/rule/content/footer) repaints in full
 *     whenever the record changes, the card flips, edit mode toggles, or an
 *     overlay (the OSK, a picker, the SELECT fallback) painted over the screen
 *     — via pdna_summary_bg(), which by construction never touches the left
 *     panel's own rect (0,11)-(92,150);
 *   - the LEFT PANEL — and the ROM portrait fetch inside it — repaints ONLY
 *     when the underlying record actually changed (or on first paint / after
 *     an overlay), independent of a bare card flip: exactly the second, nested
 *     memo draw_left_conditional keeps in pdna_summary.c, reimplemented here
 *     from data already on hand (the same GbEditMon `shadow` this file already
 *     tracked) rather than a second static.
 * NO NEW STATICS (docs/SPRITE-ERA-DESIGN.md sec 0: "EWRAM 524 B free."): the
 * field registry, the shadow record and the converted PkMon all live on THIS
 * FUNCTION'S OWN STACK FRAME.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "pdna_gbsummary.h"
#include "gb_editor.h"
#include "pdna_gbedit.h"     /* gbedit_confirm/press/adjust_checked/is_dv_field (shared) */
#include "pdna_summary.h"    /* the shared Gen-3 card chrome (BACKLOG #41 slice E1) */
#include "gen12_convert.h"   /* the throwaway left-panel conversion */
#include "gen3_edit.h"       /* EditMon, gen3_edit_load/commit, gen3_build_mon, em_preview */
#include "gen3_mon.h"        /* PkMon */
#include "gen3_box.h"        /* pk_resolve */
#include "ui.h"
#include "data_tables.h"     /* pk_species_name, pk_move_name, pk_species_type1/2 */
#include "type_icons.h"      /* TYPE_ICON_W/H, ui_type_chip */
#include "pdna_layout.h"
#include "snd.h"
#include "sprite_era.h"       /* SE_PLACE_SUMMARY -- pdna_origin_art_set_place() (E4/D2) */
#include "pdna_origin_art.h"  /* pdna_origin_art_get_place/set_place */

#define NCARDS       4
#define CARD_INFO    0
#define CARD_SKILLS  1
#define CARD_MOVES   2
#define CARD_ORIGIN  3

#define ROW_H     9
#define TITLE_Y  14      /* matches pdna_summary.c's own card_info/card_skills title y */

/* Same colour convention as pdna_summary.c's cards ("same title style"). */
#define C_HDR  UI_TITLE
#define C_KEY  UI_DIRCLR
#define C_VAL  UI_TEXT
#define C_HOT  UI_WARN

/* Registered editable field slots for the CURRENT card, on the caller's stack.
 * Worst case is SKILLS on a Gen-2 record: HP registers only its stat-exp field
 * (its DV is GBE_DVH, derived, shown but never editable) = 1, Atk/Def/Spe each
 * register a DV + a stat-exp field = 6, and Gen 2's SpA/SpD rows each register
 * the SAME underlying Spc DV/stat-exp fields again = 4 more -- 11 total. 14
 * leaves headroom without a caller having to reason about the exact count. */
#define MAX_SLOT 14
typedef struct { uint8_t field, x, y, w; } GbSlot;

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

static void reg(GbSlot* slot, int* n, int field, int x, int y, int w) {
  if (*n < MAX_SLOT) { slot[*n].field = (uint8_t)field; slot[*n].x = (uint8_t)x;
                       slot[*n].y = (uint8_t)y; slot[*n].w = (uint8_t)w; (*n)++; }
}

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
 * Gen 3's numbering so the ORIGIN card draws the SAME ui_type_chip a Gen-2 species'
 * table lookup does, instead of a raw "T14"-style id:
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

/* ---- the left panel: a throwaway Gen-3-style conversion ------------------------
 *
 * See this file's header comment for why building one is safe. On its own stack
 * frame (noinline), the same reasoning as gb_persist/gb_has_sidecar's own split
 * in pdna_gen12.c: a Gb12Mon (~110 B) + an 80-byte record + an EditMon (~165 B)
 * is well past what pdna_gbsummary()'s own <=700 B frame budget should spend on
 * a conversion its caller only needs the RESULT of.
 *
 * Returns false only when the species itself cannot be resolved at all (a Gen-1
 * internal index the National Dex table has no entry for — MissingNo and
 * friends): every OTHER refusal reason (an egg, a held item, a damaged move
 * list, an out-of-range level) still yields a real, correctly-named PkMon,
 * exactly the two-tier fallback pdna_gen12.c's own gb_build_slot() uses for the
 * box grid (a relaxed retry, then a gen3_build_mon placeholder). */
static bool __attribute__((noinline)) gbsum_convert_left(const GbEditMon* e, PkMon* out) {
  Gb12Mon in;
  memset(&in, 0, sizeof in);
  in.gen = e->gen;
  in.species_dex = gb_get_species_dex(e);
  in.exp   = gb_get_exp(e);
  in.level = gb_get_level(e);
  in.dv_atk = gb_get_dv(e, GB_ATK);
  in.dv_def = gb_get_dv(e, GB_DEF);
  in.dv_spd = gb_get_dv(e, GB_SPE);      /* Gb12Mon's own naming: the "Speed" DV */
  in.dv_spc = gb_get_dv(e, GB_SPC);
  for (int i = 0; i < 4; i++) {
    in.moves[i]  = gb_get_move(e, i);
    in.pp_ups[i] = gb_get_ppup(e, i);
  }
  in.ot_id = gb_get_otid(e);
  gb_get_otname(e, in.ot_name, (int)sizeof in.ot_name);
  gb_get_nickname(e, in.nickname, (int)sizeof in.nickname);
  in.held_item  = gb_get_held_item(e);    /* 0 for Gen 1, by gb_edit.c's own contract */
  in.friendship = gb_get_friendship(e);   /* 0 for Gen 1: gen12_convert then keeps
                                           * gen3_build_mon's own default of 70 */
  in.pokerus    = gb_get_pokerus(e);      /* 0 for Gen 1 */
  in.is_egg     = gb_is_egg(e);           /* always false for Gen 1: no eggs exist there */
  /* has_caught_data / ot_gender / slot_salt stay 0 -- none of the three affects
   * anything pdna_summary_draw_left reads (species/level/name/type/gender/shiny
   * all come from the fields above); they only feed the record's own otGender
   * byte and the box grid's identity hash, neither of which this throwaway
   * conversion is ever shown to. */

  Gb12Target tgt; memset(&tgt, 0, sizeof tgt);     /* met_game 0 -> Emerald */
  uint8_t rec[80];

  Gb12Result r = gen12_can_convert(&in);
  if (r == GB12_ERR_EGG || r == GB12_ERR_HELD_ITEM) {
    /* A real, identifiable Pokemon; only the (irrelevant, here) transfer rule is
     * refused -- the same relaxed retry pdna_gen12.c's own gb_presentation()
     * takes for the box grid, so an egg or an item holder still gets a real
     * portrait instead of falling all the way to the placeholder tier. */
    Gb12Mon relaxed = in;
    relaxed.is_egg = false;
    relaxed.held_item = 0;
    if (gen12_can_convert(&relaxed) == GB12_OK) { in = relaxed; r = GB12_OK; }
  }

  if (r == GB12_OK && gen12_convert(&in, &tgt, rec, 0) == GB12_OK) {
    EditMon ed; gen3_edit_load(rec, false, &ed);
    em_preview(&ed, out); pk_resolve(out);
    return true;
  }

  /* Everything else that still names a real species (a damaged move list, an
   * out-of-range level, the unreachable PID-search failure): a stand-in of the
   * right species/level/nickname, gen3_build_mon's own defaults for the rest —
   * not the real mon, which is exactly why it is never written anywhere. Only a
   * genuinely unresolvable species (dex 0) has nothing to placeholder. */
  if (in.species_dex >= 1 && in.species_dex <= 251) {
    uint8_t lv = in.level; if (lv < 1) lv = 1; if (lv > 100) lv = 100;
    char otname[sizeof in.ot_name], nick[sizeof in.nickname];
    int i;
    for (i = 0; i < (int)sizeof otname - 1 && in.ot_name[i]; i++) otname[i] = in.ot_name[i];
    otname[i] = 0;
    for (i = 0; i < (int)sizeof nick - 1 && in.nickname[i]; i++) nick[i] = in.nickname[i];
    nick[i] = 0;
    gen3_build_mon(in.species_dex, lv, 1u, (uint32_t)in.ot_id, otname, 3, rec);
    if (nick[0]) {
      EditMon ed; gen3_edit_load(rec, false, &ed);
      em_set_nickname(&ed, nick);
      gen3_edit_commit(&ed, rec);
    }
    EditMon ed2; gen3_edit_load(rec, false, &ed2);
    em_preview(&ed2, out); pk_resolve(out);
    return true;
  }
  return false;
}

/* No species is resolvable at all (a Gen-1 internal index the National Dex table
 * has no entry for): draw the same panel frame pdna_summary_draw_left would, with
 * a plain "?" in place of a portrait it has no PkMon to fetch one for. */
static void gbsum_draw_left_unknown(void) {
  ui_panel(0, 11, 92, 139, RGB15(4, 7, 16), UI_BORDER);
  m3_frame(11, 13, 80, 78, UI_BORDER);
  ui_text(42, 42, C_HOT, "?");
  ui_ptext_fit(6, 84, 86, UI_DIM, "Unknown species");
}

/* ---- Card 0: INFO --------------------------------------------------------------
 *
 * Species/type/shiny/egg/Pokerus-tag are NOT repeated here: the shared left panel
 * already shows all of them for the CONVERTED mon, which resolves to the same
 * species/level/name/gender this card edits. Nickname and level keep their own
 * rows because this is where the edit CONTROL lives -- the left panel only
 * displays what they resolve to, it is not itself editable.
 *
 * GENDER (BACKLOG #51) is the one exception: the left panel shows the sign but
 * offers no way to change it, so a directly-editable Gender row lives here,
 * gated by gbe_has_gender_row() exactly like gbe_fields() gates it for the flat
 * editor -- Gen 2 only, and only for a species with a real (non-fixed,
 * non-genderless) gender ratio. LEFT/RIGHT/A all flip it (gb_editor.c's
 * gbe_flip_gender): there is no "up"/"down" for a two-state field. */

static void field_row(const GbEditMon* e, int field, const char* label, int y,
                      GbSlot* slot, int* n) {
  const int x = PDNA_SUM_CARD_X, vx = x + PDNA_GBSUM_VAL_DX;
  const int vw = PDNA_SUM_CARD_W - PDNA_GBSUM_VAL_DX;
  char val[GBE_VALUE_MAX], vt[PDNA_SUM_CARD_W / 8 + 1];
  gbe_value(e, field, val, sizeof val);
  ui_truncate(vt, val, vw / 8);
  ui_text(x, y, C_KEY, label);
  if (field == GBE_ITEM) ui_ptext_fit(vx, y, vw, C_VAL, val);   /* #340a (review-zq F1): 68 Gen-2 names exceed 10 fixed cols; proportional max 72 px <= vw 86 */
  else                   ui_text(vx, y, C_VAL, vt);
  reg(slot, n, field, vx, y, vw);
}

static void card_info(const GbEditMon* e, GbSlot* slot, int* n) {
  const int x = PDNA_SUM_CARD_X, vx = x + PDNA_GBSUM_VAL_DX;
  int y = TITLE_Y; char b[48];
  ui_text(x, y, C_HDR, PDNA_GBSUM_CARD_INFO); y += 12;

  field_row(e, GBE_NICK, PDNA_GBSUM_LBL_NAME, y, slot, n); y += ROW_H;
  field_row(e, GBE_LEVEL, PDNA_GBSUM_LBL_LV, y, slot, n); y += ROW_H;
  if (gbe_has_gender_row(e)) {
    field_row(e, GBE_GENDER, PDNA_GBSUM_LBL_GENDER, y, slot, n); y += ROW_H;
  }
  field_row(e, GBE_OT, PDNA_GBSUM_LBL_OT, y, slot, n); y += ROW_H;

  siprintf(b, PDNA_GBSUM_ID_FMT, (unsigned)gb_get_otid(e));
  ui_text(x, y, C_KEY, PDNA_GBSUM_LBL_ID); ui_text(vx, y, C_VAL, b);
  reg(slot, n, GBE_OTID, vx, y, PDNA_SUM_CARD_W - PDNA_GBSUM_VAL_DX); y += ROW_H;

  if (e->gen == GB_GEN2) {
    field_row(e, GBE_ITEM, PDNA_GBSUM_LBL_ITEM, y, slot, n); y += ROW_H;
    /* An Egg stores its hatch counter in this SAME byte (gb_editor.c's own
     * gbe_label_of: "Egg cycles" for GBE_FRIEND on an egg) — the old flat-list
     * field_row honoured that; this card's own copy did not, and showed a
     * hatching egg's countdown as "Friend". */
    field_row(e, GBE_FRIEND, gb_is_egg(e) ? PDNA_GBSUM_LBL_EGGC : PDNA_GBSUM_LBL_FRIEND,
              y, slot, n); y += ROW_H;

    /* Pokerus (pokecrystal stats_screen.asm:590-604): high nibble strain, low
     * nibble days left (0 with a strain set = immune, past infection); byte 0 =
     * never had it. Display-only — this tree carries no gb_editor.h row/setter
     * for it. */
    uint8_t raw = gb_get_pokerus(e);
    if (raw) {
      unsigned strain = raw >> 4, days = raw & 0x0F;
      if (days) siprintf(b, PDNA_GBSUM_PKRS_DAYS_FMT, strain, days);
      else      siprintf(b, PDNA_GBSUM_PKRS_IMMUNE_FMT, strain);
      ui_text(x, y, C_KEY, PDNA_GBSUM_LBL_PKRS);
      ui_ptext_fit(vx, y, PDNA_SUM_CARD_W - PDNA_GBSUM_VAL_DX, UI_OK, b);
      y += ROW_H;
    }
  } else {
    ui_text(x, y, C_KEY, PDNA_GBSUM_LBL_STATUS);
    ui_text(vx, y, gb_get_gen1_status(e) ? UI_WARN : UI_OK, gbsum_g1_status_text(gb_get_gen1_status(e)));
    y += ROW_H;
  }

  y += 2;
  uint16_t dex = gb_get_species_dex(e);
  uint32_t exp = gb_get_exp(e);
  uint8_t lvl = gb_get_level(e);
  if (lvl >= 100) {
    siprintf(b, PDNA_GBSUM_EXP_MAX_FMT, (unsigned long)exp);
  } else {
    uint32_t nxt = gb_exp_for_level(dex, (uint8_t)(lvl + 1));
    siprintf(b, PDNA_GBSUM_EXP_FMT, (unsigned long)exp, (unsigned long)(nxt > exp ? nxt - exp : 0));
  }
  ui_ptext_fit(x, y, PDNA_SUM_CARD_W, C_VAL, b);
}

/* ---- Card 1: SKILLS --------------------------------------------------------------
 *
 * Two rows per stat, not the old flat-list's single 4-column row (label / value /
 * DV / stat-exp spanning the whole 240 px screen) — that grid does not fit this
 * card's 138 px. Row 1: "Atk 999/999" (party) or "Atk -" (box). Row 2, indented,
 * its own two registered fields: "DV 15" then "SE 65535". */

static void stat_row(const GbEditMon* e, const char* label, int stat_i, int dv_field,
                     int se_field, int y, GbSlot* slot, int* n) {
  const int x = PDNA_SUM_CARD_X;
  char b[16];
  ui_text(x, y, C_KEY, label);
  if (e->is_party) {
    if (stat_i == GB_HP) siprintf(b, PDNA_GBSUM_STAT_CURMAX_FMT,
                                  (unsigned)gb_get_current_hp(e), (unsigned)gb_get_stat(e, GB_HP));
    else                 siprintf(b, "%u", (unsigned)gb_get_stat(e, stat_i));
  } else {
    strcpy(b, PDNA_GBSUM_STAT_DASH);
  }
  ui_text(x + PDNA_GBSUM_STAT_VAL_DX, y,
          (stat_i == GB_HP && e->is_party && gb_get_current_hp(e) == 0) ? UI_WARN : C_VAL, b);
  /* BACKLOG #231: a PARTY mon's "cur/max" is its CURRENT HP slot -- the one reviving edit
   * (LEFT/RIGHT +-1, A full<->0). A box record has neither the byte's max nor a row. */
  if (stat_i == GB_HP && e->is_party)
    reg(slot, n, GBE_CURHP, x + PDNA_GBSUM_STAT_VAL_DX, y, PDNA_SUM_CARD_W - PDNA_GBSUM_STAT_VAL_DX);

  int y2 = y + ROW_H;
  if (dv_field == GBE_DVH) {
    siprintf(b, PDNA_GBSUM_STAT_DV_FMT, (unsigned)gb_get_dv(e, GB_HP));
    ui_text(x + PDNA_GBSUM_STAT_DV_DX, y2, UI_DIM, b);   /* derived, shown, not registered */
  } else {
    siprintf(b, PDNA_GBSUM_STAT_DV_FMT, (unsigned)gb_get_dv(e, dv_stat_of(dv_field)));
    ui_text(x + PDNA_GBSUM_STAT_DV_DX, y2, C_VAL, b);
    reg(slot, n, dv_field, x + PDNA_GBSUM_STAT_DV_DX, y2,
        PDNA_GBSUM_STAT_SE_DX - PDNA_GBSUM_STAT_DV_DX);
  }

  siprintf(b, PDNA_GBSUM_STAT_SE_FMT, (unsigned)gb_get_statexp(e, se_field));
  ui_text(x + PDNA_GBSUM_STAT_SE_DX, y2, C_VAL, b);
  reg(slot, n, GBE_SE0 + se_field, x + PDNA_GBSUM_STAT_SE_DX, y2,
      PDNA_SUM_CARD_W - PDNA_GBSUM_STAT_SE_DX);
}

static void card_skills(const GbEditMon* e, GbSlot* slot, int* n) {
  const int x = PDNA_SUM_CARD_X;
  int y = TITLE_Y;
  ui_text(x, y, C_HDR, PDNA_GBSUM_CARD_SKILLS); y += 12;

  stat_row(e, PDNA_GBSUM_STAT_HP,  GB_HP,  GBE_DVH, 0, y, slot, n); y += ROW_H * 2;
  stat_row(e, PDNA_GBSUM_STAT_ATK, GB_ATK, GBE_DVA, 1, y, slot, n); y += ROW_H * 2;
  stat_row(e, PDNA_GBSUM_STAT_DEF, GB_DEF, GBE_DVD, 2, y, slot, n); y += ROW_H * 2;
  stat_row(e, PDNA_GBSUM_STAT_SPE, GB_SPE, GBE_DVS, 3, y, slot, n); y += ROW_H * 2;
  if (e->gen == GB_GEN2) {
    /* Gen 2 splits Special into SpA/SpD in the COMPUTED party stat block only —
     * both read the same stored Spc DV and Spc stat exp (gb_edit.h's own header
     * comment). Editing either row edits the one stored value both use. */
    stat_row(e, PDNA_GBSUM_STAT_SPA, 4, GBE_DVC, 4, y, slot, n); y += ROW_H * 2;
    stat_row(e, PDNA_GBSUM_STAT_SPD, 5, GBE_DVC, 4, y, slot, n); y += ROW_H * 2;
  } else {
    stat_row(e, PDNA_GBSUM_STAT_SPC, 4, GBE_DVC, 4, y, slot, n); y += ROW_H * 2;
  }

  if (!e->is_party) ui_ptext_fit(x, y + 2, PDNA_SUM_CARD_W, UI_DIM, PDNA_GBSUM_BOX_STAT_NOTE);
}

/* ---- Card 2: MOVES ---------------------------------------------------------------
 *
 * The SAME row shape as pdna_summary.c's own card_moves (PDNA_SUM_PP_X_DX/PP_W,
 * PDNA_SUM_PP_FMT/PP_UPS_FMT, reused verbatim): a name column, then a "PP cur/max
 * [+ups]" cell registered under GBE_PPU0+i only — pressing/adjusting it cycles PP
 * Ups, exactly like Gen 3's F_PPU0+i; current PP is not independently editable
 * from this card there either. The one difference: a Game Boy record carries no
 * move TYPE this tree can read, so there is no type-badge line under it. */

static void move_row(const GbEditMon* e, int i, int y, GbSlot* slot, int* n) {
  const int x = PDNA_SUM_CARD_X;
  const int name_w = PDNA_SUM_PP_X_DX - 4;
  uint8_t mv = gb_get_move(e, i);
  reg(slot, n, GBE_MV0 + i, x, y, name_w);
  if (!mv) { ui_text(x, y, UI_DIM, "-"); return; }
  ui_ptext_fit(x, y, name_w, C_VAL, pk_move_name(mv));

  uint8_t ups = gb_get_ppup(e, i);
  uint8_t maxpp = gb_max_pp(e->gen, mv, ups);
  char b[24];
  if (ups) siprintf(b, PDNA_SUM_PP_UPS_FMT, (unsigned)gb_get_pp(e, i), (unsigned)maxpp, (unsigned)ups);
  else     siprintf(b, PDNA_SUM_PP_FMT,     (unsigned)gb_get_pp(e, i), (unsigned)maxpp);
  reg(slot, n, GBE_PPU0 + i, x + PDNA_SUM_PP_X_DX, y, PDNA_SUM_PP_W);
  ui_ptext_fit(x + PDNA_SUM_PP_X_DX, y, PDNA_SUM_PP_W, UI_DIM, b);
}

static void card_moves(const GbEditMon* e, GbSlot* slot, int* n) {
  const int x = PDNA_SUM_CARD_X;
  int y = TITLE_Y;
  ui_text(x, y, C_HDR, PDNA_GBSUM_CARD_MOVES); y += 12;
  for (int i = 0; i < 4; i++) { move_row(e, i, y, slot, n); y += 22; }
}

/* ---- Card 3: ORIGIN --------------------------------------------------------------
 *
 * New in E1 (the earlier 3-card design had no such card). Nothing here is
 * editable. The honest "this is a throwaway preview, not a real transfer" story
 * (gen12_convert.h), the record's own generation (the same `note` string every
 * caller already passed to the old top-of-screen corner label — moved here,
 * where the box grid never collides with the card dots the way it did at the
 * old fixed top-right position), the Gen-1 type-byte chip (Gen 2's own record
 * carries no type field — its type comes straight off the species table, which
 * the shared left panel already shows), and the sidecar link status
 * (docs/GEN3-TO-GB-SIDECAR-DESIGN.md). */
static void card_origin(const GbEditMon* e, const char* note, bool has_sidecar) {
  const int x = PDNA_SUM_CARD_X;
  int y = TITLE_Y;
  ui_text(x, y, C_HDR, PDNA_GBSUM_CARD_ORIGIN); y += 12;

  if (note) { ui_text(x, y, C_HOT, note); y += ROW_H + 1; }

  ui_ptext_fit(x, y, PDNA_SUM_CARD_W, UI_DIM, PDNA_GBSUM_ORIGIN_ART_L1); y += ROW_H;
  ui_ptext_fit(x, y, PDNA_SUM_CARD_W, UI_DIM, PDNA_GBSUM_ORIGIN_ART_L2); y += ROW_H + 3;

  if (e->gen == GB_GEN1) {
    int m1 = g1_to_g3_type(gb_get_gen1_type1(e));
    int m2 = g1_to_g3_type(gb_get_gen1_type2(e));
    if (m1 >= 0) {
      ui_text(x, y, C_KEY, PDNA_GBSUM_ORIGIN_TYPE_LBL);
      ui_type_chip(x + PDNA_GBSUM_VAL_DX, y - 2, TYPE_ICON_W, TYPE_ICON_H, (uint8_t)m1);
      if (m2 >= 0 && m2 != m1)
        ui_type_chip(x + PDNA_GBSUM_VAL_DX + TYPE_ICON_W + 4, y - 2, TYPE_ICON_W, TYPE_ICON_H, (uint8_t)m2);
      y += TYPE_ICON_H + 3;
    }
  }

  ui_text(x, y, has_sidecar ? UI_OK : UI_DIM,
          has_sidecar ? PDNA_GBSUM_ORIGIN_SIDECAR_YES : PDNA_GBSUM_ORIGIN_SIDECAR_NO);
}

/* ---- shell: chip/dots/rule + the shared left panel + the one card --------------- */

static void render(const GbEditMon* e, const PkMon* left, bool left_ok, bool draw_left,
                   int card, bool editing, bool can_edit, bool create, const char* note,
                   bool has_sidecar, GbSlot* slot, int* n) {
  pdna_summary_bg();                          /* never touches (0,11)-(92,150) */
  /* CREATE's own chip (BACKLOG #50 UX-parity) only shows outside active
   * editing -- same rule pdna_summary.c's own `g_create = create && !editing`
   * uses -- achieved here by simply checking `editing` FIRST, so the EDIT
   * chip always wins whenever both are true, with no extra combined flag. */
  if (editing)      { ui_fill_rect(0, 0, 50, 9, UI_WARN); ui_text(8, 1, UI_PANEL, PDNA_GBSUM_EDIT_CHIP); }
  else if (create)  { ui_fill_rect(0, 0, 50, 9, UI_OK);   ui_text(12, 1, UI_PANEL, PDNA_GBSUM_NEW_CHIP); }
  else              { ui_text(4, 2, UI_DIM, PDNA_GBSUM_VIEW_CHIP); }
  pdna_summary_draw_dots(150, 2, NCARDS, card);
  ui_hline(0, 10, UI_SCR_W, UI_BORDER);

  if (draw_left) {
    /* e->gen (GB_GEN1/GB_GEN2, 1/2) is the CERTAIN source generation -- this
     * mon was loaded off that mount, not inferred -- so the chip reads GB1/GB2
     * instead of pdna_origin_of's own best guess. See pdna_summary.c's header
     * comment on pdna_summary_draw_left_hint (E1-b, 2026-09-06 review). */
    if (left_ok) pdna_summary_draw_left_hint(left, false, e->gen);
    else         gbsum_draw_left_unknown();
  }

  ui_hline(PDNA_SUM_CARD_X, 24, 100, UI_TITLE);
  *n = 0;
  switch (card) {
    case CARD_INFO:   card_info(e, slot, n); break;
    case CARD_SKILLS: card_skills(e, slot, n); break;
    case CARD_MOVES:  card_moves(e, slot, n); break;
    case CARD_ORIGIN: card_origin(e, note, has_sidecar); break;
  }

  ui_hline(0, 151, UI_SCR_W, UI_BORDER);
  const char* foot = editing ? (create ? PDNA_GBSUM_FOOT_CREATE_EDIT : PDNA_GBSUM_FOOT_EDIT)
                   : create  ? PDNA_GBSUM_FOOT_CREATE
                   : can_edit ? PDNA_GBSUM_FOOT_VIEW : PDNA_GBSUM_FOOT_VIEW_RO;
  ui_text(4, PDNA_SUM_FOOTER_Y, UI_DIM, foot);
}

/* ---- input dispatch, split out of pdna_gbsummary() so that function stays a
 * setup + a two-call loop body (the repaint gate, then gbsum_input() below)
 * instead of one long inline block. ------------------------------------- */

typedef struct {
  GbEditMon* e;
  bool can_edit, has_sidecar, editing, dirty, dv_warned, create;
  const char* note;
  int card, fsel;
  GbSlot* slot; int nslot;
  bool* saved; int* card_io;
} GbSumCtx;

/* CREATE's keep-or-discard confirm (BACKLOG #50 UX-parity, Guy 2026-09-07):
 * reached from START (either sub-mode) or B (browse sub-mode only) -- the
 * two paths pdna_summary.c's own create flow offers, "the same keep-or-
 * discard confirm" Guy's parity ask names. Uses gbedit_confirm_keep() (G1
 * review LOW-6, 2026-09-08): Gen 3's OWN create-flow wording ("Keep this
 * Pokemon?" / "A = write (backup first)" / "B = discard it") with gb_check's
 * structural-issue lines still shown underneath -- the SAME legality net
 * every other GB commit in this screen already goes through, just with the
 * title/verbs a create should actually say instead of the plain-edit
 * screen's "Write to the save?" (an earlier version of this comment argued
 * for reusing gbedit_confirm() as-is; the review named this specific gap).
 * On accept: settles derived stats and reports kept (matching
 * gbsum_view_keys' own dirty-exit path). On decline: reports not kept,
 * leaving `*saved` at whatever it already was (false, from
 * pdna_gbsummary_inner's own setup) -- the caller decides what "not kept"
 * means (stay open, for START; leave anyway, for B). */
static bool gbsum_create_keep(GbSumCtx* c) {
  if (!gbedit_confirm_keep(c->e)) return false;
  gbe_settle_stats(c->e);
  if (c->saved) *c->saved = true;
  return true;
}

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

/* EDIT-mode key dispatch. `dirty` is derived in pdna_gbsummary()'s own loop from a
 * memcmp against the pre-edit shadow, so a cancelled osk/picker or a clamped no-op
 * LEFT/RIGHT can never mark the record dirty when not one byte actually changed. */
static void gbsum_edit_keys(GbSumCtx* c, u16 k) {
  if (k & KEY_B) { c->editing = false; c->fsel = 0; return; }
  if (k & (KEY_L | KEY_R)) {
    c->card = (c->card + (k & KEY_R ? 1 : NCARDS - 1)) % NCARDS; c->fsel = 0; return;
  }
  if (c->nslot && (k & KEY_A)) {
    gbedit_press(c->e, c->slot[c->fsel].field, c->has_sidecar, &c->dv_warned); return;
  }
  if (c->nslot && (k & KEY_LEFT)) {
    if (!gbedit_adjust_checked(c->e, c->slot[c->fsel].field, -1, false, c->has_sidecar, &c->dv_warned))
      gbedit_adjust_refused(c->slot[c->fsel].field);
    return;
  }
  if (c->nslot && (k & KEY_RIGHT)) {
    if (!gbedit_adjust_checked(c->e, c->slot[c->fsel].field, +1, false, c->has_sidecar, &c->dv_warned))
      gbedit_adjust_refused(c->slot[c->fsel].field);
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
    else snd_deny();
    return false;
  }
  if ((k & (KEY_L | KEY_R)) || (fresh & (KEY_LEFT | KEY_RIGHT))) {
    int fwd = (k & KEY_R) || (fresh & KEY_RIGHT);
    c->card = (c->card + (fwd ? 1 : NCARDS - 1)) % NCARDS;
    return false;
  }
  /* CREATE browse sub-mode (BACKLOG #50 UX-parity): no other mon exists to
   * scroll to (this IS the only mon in the visit), so U/D do nothing here --
   * B is the other way out, asking the same keep/discard question START
   * does and leaving EITHER way (kept or discarded), mirroring
   * pdna_summary.c's own `else if (create) { if (k & KEY_B) {...} }` branch,
   * which the SAME way pre-empts its generic dirty-exit UP/DOWN/B handling. */
  if (c->create) {
    if (k & KEY_B) {
      gbsum_create_keep(c);                    /* result already folded into *saved */
      key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
      if (c->card_io) *c->card_io = c->card;
      *out = 0;
      return true;
    }
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

/* `nslot` lets a card with nothing to edit (ORIGIN today) say so: in EDIT mode
 * with no registered field, A/U/D/LEFT/RIGHT are exactly the keys
 * gbsum_edit_keys() no-ops on (KEY_B and KEY_L/KEY_R still work regardless --
 * B always leaves, L/R always flips cards), so playing their NORMAL earcon
 * (snd_ok for A, snd_move for U/D, snd_edit for LEFT/RIGHT) would tell the
 * player something happened when nothing did. `create` adds START to the
 * "ok" click (BACKLOG #50 UX-parity: pdna_summary.c's own click dispatch
 * already plays snd_ok() for `KEY_A | KEY_START` together) -- ONLY when
 * `create`, since START has no meaning at all in a plain edit/view visit. */
static void gbsum_click(u16 fresh, bool editing, bool create, int nslot) {
  if (editing && !nslot && (fresh & (KEY_A | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))) {
    snd_deny();
    return;
  }
  if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
  else if (fresh & (KEY_L | KEY_R | KEY_SELECT)) snd_tab();
  else if (fresh & (KEY_A | (create ? KEY_START : 0))) snd_ok();
  else if (fresh & KEY_B) snd_back();
  else if (fresh & (KEY_LEFT | KEY_RIGHT)) { if (editing) snd_edit(); else snd_tab(); }
}

/* Wait for one key, dispatch it, and report whether pdna_gbsummary() should
 * `return *out` right now (a SELECT commit, or a VIEW-mode leave/nav step) --
 * everything past "the card and the outline are on screen" in that function. */
static bool gbsum_input(GbSumCtx* c, bool* shadow_valid, int* out) {
  u16 k, fresh;
  do { s_vsync(); fresh = key_hit(KEY_FULL);
       k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
  gbsum_click(fresh, c->editing, c->create, c->nslot);

  if (fresh & KEY_SELECT) {
    if (gbsum_select_fallback(c, shadow_valid)) { *out = 0; return true; }
    return false;
  }

  /* CREATE: START keeps the record from EITHER sub-mode -- checked before the
   * editing/view split so it works whether or not a field is currently
   * focused, mirroring pdna_summary.c's own create flow (BACKLOG #50
   * UX-parity). See gbsum_create_keep()'s own comment for why the dialog
   * itself reuses gbedit_confirm() rather than porting Gen 3's confirm_keep()
   * text. */
  if (c->create && (fresh & KEY_START)) {
    if (gbsum_create_keep(c)) { *out = 0; return true; }
    return false;                                /* declined -> keep editing/browsing */
  }

  if (c->editing) { gbsum_edit_keys(c, k); return false; }
  return gbsum_view_keys(c, k, fresh, out);
}

static int pdna_gbsummary_inner(GbEditMon* e, bool can_edit, bool start_editing,
                                 const char* note, bool has_sidecar, bool create,
                                 bool* saved, int* card_io) {
  if (saved) *saved = false;
  if (!e) return 0;

  GbSumCtx c;
  c.e = e; c.can_edit = can_edit; c.has_sidecar = has_sidecar; c.note = note;
  c.card = (card_io && *card_io >= 0 && *card_io < NCARDS) ? *card_io : 0;
  c.editing = can_edit && start_editing;
  c.create = create;
  c.fsel = 0; c.dirty = false; c.dv_warned = false;
  c.saved = saved; c.card_io = card_io;

  GbEditMon shadow; memset(&shadow, 0, sizeof shadow);
  bool shadow_valid = false;
  uint32_t shadow_gen = 0;
  int shadow_card = -1;
  bool shadow_edit = false;

  PkMon left_mon; bool left_ok = false;

  GbSlot slot[MAX_SLOT];
  c.slot = slot; c.nslot = 0;

  /* Whatever a PREVIOUS screen (the Gen-3 summary, or an earlier visit here) left
   * mid-outline belongs to a DIFFERENT layout — restoring its save-under pixels
   * at that stale (x,y) here would corrupt whatever this screen just drew there.
   * Drop it rather than try to restore it, exactly like pdna_summary.c's own
   * summary_run() does on its own entry. */
  pdna_summary_sel_frame_drop();
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);

  for (;;) {
    bool full = !shadow_valid || shadow_gen != ui_clear_gen() ||
                shadow_card != c.card || shadow_edit != c.editing;
    bool record_changed = shadow_valid && memcmp(&shadow, c.e, sizeof(GbEditMon)) != 0;
    bool need = full || record_changed;

    if (need) {
      /* The expensive half (a ROM portrait fetch inside pdna_summary_draw_left,
       * plus the type badges it draws) only when the CONVERTED PkMon actually
       * differs from what is already on screen — not merely whenever the
       * native record does. gen12_convert() never reads stat exp or current PP
       * at all (Gb12Mon carries neither field), so a LEFT/RIGHT tick on an SE
       * cell (SKILLS) or a current-PP cell (MOVES) changes `e` -- and used to
       * re-run the whole expensive draw for a value the left panel cannot even
       * show. Re-converting here is cheap (pure computation, no I/O); it is the
       * DRAW below that costs an SD read + an LZ77 decode, so that is what gets
       * gated on the real diff. pdna_summary_bg() never paints the left panel's
       * own rect, so skipping the redraw here is correct: whatever it already
       * shows is still exactly right. */
      PkMon conv;
      bool conv_ok = gbsum_convert_left(c.e, &conv);
      /* BACKLOG #190: `full` (not just !shadow_valid) must force a left-panel redraw
       * too -- msg_wait()'s refusal dialog (pdna_main.c, (16,48)-(224,118)) overlaps
       * pdna_summary_bg()'s own excluded left-panel rect ((0,11)-(92,150), see its
       * header comment), and a REFUSED edit changes zero bytes of `c.e`, so the old
       * `!shadow_valid || conv_ok != left_ok || memcmp(...)` never noticed the panel
       * needed erasing even though `full` (this loop's own repaint gate, keyed to
       * ui_clear_gen()) was already true -- the #119 trap class (a local partial-
       * repaint shadow not keyed to the same counter the rest of the screen uses),
       * already fixed this same way in pdna_gbflags.c/pdna_fly.c/pdna_gbfly.c.
       * `full` is a strict superset of the old `!shadow_valid` term. */
      bool reconv = full || conv_ok != left_ok ||
                    (conv_ok && memcmp(&conv, &left_mon, sizeof conv) != 0);
      if (reconv) { left_mon = conv; left_ok = conv_ok; }

      render(c.e, &left_mon, left_ok, reconv, c.card, c.editing, c.can_edit, c.create,
             c.note, c.has_sidecar, c.slot, &c.nslot);
      pdna_summary_sel_frame_drop();   /* the card body just painted over any outline */
      if (c.editing && c.nslot && c.fsel >= c.nslot) c.fsel = c.nslot - 1;

      if (record_changed) c.dirty = true;
      shadow = *c.e; shadow_gen = ui_clear_gen(); shadow_card = c.card;
      shadow_edit = c.editing; shadow_valid = true;
    }

    if (c.editing && c.nslot) {
      if (c.fsel >= c.nslot) c.fsel = c.nslot - 1;
      pdna_summary_sel_frame_set(c.slot[c.fsel].x, c.slot[c.fsel].y, c.slot[c.fsel].w);
    } else {
      pdna_summary_sel_frame_hide();
    }

    int out;
    if (gbsum_input(&c, &shadow_valid, &out)) return out;
  }
}

/* D2 (E4): pdna_gbsummary_inner() never set the art-router PLACE at all -- the left
 * panel's portrait fetch (gbsum_convert_left -> pdna_summary_draw_left ->
 * pdna_origin_art_portrait) resolved against whatever place a PREVIOUS screen last
 * set (the box/party grid's PC/BANK/GBGRID/PARTY), never SE_PLACE_SUMMARY. Same
 * save/restore idiom as pdna_summary.c's summary_run() wrapping summary_run_inner():
 * this screen's big portrait is always the SUMMARY cell while it is up, and the
 * caller's own place is restored on the way out so whatever draws next (this screen
 * is routinely opened from inside another screen's own loop) is unaffected. */
int pdna_gbsummary(GbEditMon* e, bool can_edit, bool start_editing, const char* note,
                    bool has_sidecar, bool create, bool* saved, int* card_io) {
  int prev = pdna_origin_art_get_place();
  pdna_origin_art_set_place(SE_PLACE_SUMMARY);
  int r = pdna_gbsummary_inner(e, can_edit, start_editing, note, has_sidecar, create,
                               saved, card_io);
  pdna_origin_art_set_place(prev);
  return r;
}
