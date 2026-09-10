/*
 * Field-edit screen for PokeDNA (PKHeX-style, in-RAM).
 * UP/DOWN pick a field; LEFT/RIGHT adjust by 1, L/R shoulders by a bigger step;
 * A opens a list picker (species/item/move/nature) or the on-screen keyboard
 * (nickname/OT) or toggles (ability/shiny/gender); B cancels; START -> commit
 * confirm. Personality-derived fields (nature/shiny/gender) re-roll the PID.
 * The actual SD write is done by the caller on the returned record.
 *
 * NOTE ON REACH: pdna_edit() -- the FIELD LIST below -- has no call site today; the
 * six-card summary (pdna_summary.c) is the editor the user actually reaches, and it
 * drives this file's em_field_press / em_field_adjust directly. Both screens are kept
 * incremental for the same reason and by the same rule (see render() below), so the
 * list does not quietly rot back into a full-screen repaint if it is ever re-wired.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "pdna_edit.h"
#include "ui.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_contest.h"  /* gc_ribbon_get/set — F_RIB0..4 rank rows (BACKLOG #60) */
#include "gen3_box.h"      /* pk_resolve */
#include "gen3_places.h"   /* met-location region + per-game scoping */
#include "data_tables.h"
#include "osk.h"
#include "pdna_pick.h"
#include "pdna_layout.h"   /* labels/geometry shared with tests/host_textfit_test.c */
#include "snd.h"

static const char* const FLABEL[F_NUM] = {
  "Species", "Nickname", "Level", "Nature", "Ability", "Shiny", "Gender",
  "Item", "Friendship",
  "IV HP", "IV Atk", "IV Def", "IV Spe", "IV SpA", "IV SpD",
  "EV HP", "EV Atk", "EV Def", "EV Spe", "EV SpA", "EV SpD",
  "Move 1", "Move 2", "Move 3", "Move 4",
  PDNA_EDIT_MAXPP_LBL(1), PDNA_EDIT_MAXPP_LBL(2),
  PDNA_EDIT_MAXPP_LBL(3), PDNA_EDIT_MAXPP_LBL(4),
  "PP 1", "PP 2", "PP 3", "PP 4", "OT Name",
  "Ball", PDNA_EDIT_REGION_LBL, "Met Loc", "Met Lv", "Met Game",
  "Cool", "Beauty", "Cute", "Smart", "Tough", "Sheen",
  PDNA_EDIT_RIB_COOL_LBL, PDNA_EDIT_RIB_BEAUTY_LBL, PDNA_EDIT_RIB_CUTE_LBL,
  PDNA_EDIT_RIB_SMART_LBL, PDNA_EDIT_RIB_TOUGH_LBL,
};

/* Gen-3 origin-game id -> name. */
const char* pk_metgame_name(uint8_t g) {
  switch (g) {
    case 1: return "Sapphire"; case 2: return "Ruby";   case 3: return "Emerald";
    case 4: return "FireRed";  case 5: return "LeafGreen"; case 15: return "Colo/XD";
    default: return "?";
  }
}

#define VIS_ROWS 16

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (k & (KEY_L | KEY_R)) snd_tab();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}
static int  clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static const char* GEN[3] = { "Male", "Female", "-" };

static void refresh(EditMon* e, PkMon* cur) {
  em_preview(e, cur);
  pk_resolve(cur);                 /* fills gender (+ box level/stats) */
}

/* A move's maximum PP is DERIVED, never stored: em_pp_max (gen3_edit.c) applies the
 * game's own CalculatePPWithBonus to the base PP and the slot's 2-bit PP-Up count. */
static uint8_t pp_max(const PkMon* c, int i) {
  return em_pp_max(c->moves[i], c->ppBonuses, i);
}
static uint8_t pp_ups(const PkMon* c, int i) { return (uint8_t)((c->ppBonuses >> (i * 2)) & 3); }

/* format a field's current value into buf */
static const char* const RIB_RANK_NAME[5] = { "None", "Normal", "Super", "Hyper", "Master" };

static void field_value(int f, const PkMon* c, char* buf) {
  switch (f) {
    case F_SPECIES: siprintf(buf, "%s", pk_species_name(c->species)); break;
    case F_NICK:    siprintf(buf, "%s", c->nickname); break;
    case F_LEVEL:   siprintf(buf, "%u", (unsigned)c->level); break;
    case F_NATURE:  siprintf(buf, "%s", pk_nature_name(c->nature)); break;
    case F_ABILITY: siprintf(buf, "%s", pk_ability_name(pk_species_ability(c->species, c->abilityNum))); break;
    case F_SHINY:   siprintf(buf, "%s", c->isShiny ? "Yes" : "No"); break;
    case F_GENDER:  siprintf(buf, "%s", GEN[c->gender <= 2 ? c->gender : 2]); break;
    case F_ITEM:    siprintf(buf, "%s", c->heldItem ? pk_item_name(c->heldItem) : "-"); break;
    case F_FRIEND:  siprintf(buf, "%u", (unsigned)c->friendship); break;
    case F_IV0: case F_IV1: case F_IV2: case F_IV3: case F_IV4: case F_IV5:
      siprintf(buf, "%u", (unsigned)c->ivs[f - F_IV0]); break;
    case F_EV0: case F_EV1: case F_EV2: case F_EV3: case F_EV4: case F_EV5:
      siprintf(buf, "%u", (unsigned)c->evs[f - F_EV0]); break;
    case F_MV0: case F_MV1: case F_MV2: case F_MV3: {
      uint16_t mv = c->moves[f - F_MV0];
      siprintf(buf, "%s", mv ? pk_move_name(mv) : "-"); break;
    }
    case F_PPU0: case F_PPU1: case F_PPU2: case F_PPU3: {
      int i = f - F_PPU0;
      if (!c->moves[i]) { buf[0] = '-'; buf[1] = 0; break; }
      siprintf(buf, PDNA_EDIT_MAXPP_FMT, (unsigned)pp_max(c, i), (unsigned)pp_ups(c, i));
      break;
    }
    case F_PP0: case F_PP1: case F_PP2: case F_PP3:
      siprintf(buf, "%u/%u", (unsigned)c->pp[f - F_PP0], (unsigned)pp_max(c, f - F_PP0)); break;
    case F_OT:      siprintf(buf, "%s", c->otName); break;
    case F_BALL:    siprintf(buf, "%s", pk_item_name(c->pokeball)); break;
    case F_METREGION: {
      int r = g3_region_of(c->metLocation);
      siprintf(buf, "%s", r < 0 ? "?" : g3_region_name(r));
      break;
    }
    case F_METLOC:  siprintf(buf, "%s", pk_location_name(c->metLocation)); break;
    case F_METLEVEL:siprintf(buf, "%u", (unsigned)c->metLevel); break;
    case F_METGAME: siprintf(buf, "%s", pk_metgame_name(c->metGame)); break;
    case F_CT0: case F_CT1: case F_CT2: case F_CT3: case F_CT4: case F_CT5:
      siprintf(buf, "%u", (unsigned)c->contest[f - F_CT0]); break;
    case F_RIB0: case F_RIB1: case F_RIB2: case F_RIB3: case F_RIB4:
      siprintf(buf, "%s", RIB_RANK_NAME[gc_ribbon_get(c->ribbons, f - F_RIB0)]); break;
    default:        buf[0] = 0;
  }
}

/* ---- COMPARE AND REPAINT ---------------------------------------------------------
 *
 * THE BUG (Guy, 2026-08-23): "when editing the numbers it still rerenders the whole
 * screen and not only the IV i edit for example which is very slow, so i cant quickly
 * go from 0 to 30, it takes patience." render() opened with ui_clear() -- a 76,800 B
 * wipe of the entire Mode-3 framebuffer -- and then redrew the title, the EV total, the
 * header line, all 16 visible rows and the footer; the loop in pdna_edit() called it
 * unconditionally at the top of EVERY iteration. LEFT/RIGHT sit in key_repeat_mask
 * (pdna_edit() below), so HOLDING RIGHT to walk an IV from 0 to 30 paid the whole thing
 * thirty times.
 *
 * WHY NOT SIMPLY "REDRAW THE SELECTED ROW". The values on this screen are DERIVED:
 * every press runs refresh() -> em_preview() + pk_resolve(), which recompute the whole
 * PkMon, so one edit cascades. A species change rewrites the Ability and Gender rows AND
 * the header line; any EV change moves the "EV n/510" total in the top-right corner; a
 * Max PP change moves the matching "PP n" row four rows further down. A row-only
 * repaint would leave every one of those stale, which is worse than slow.
 *
 * THE SHADOW IS THE MON, NOT THE STRINGS. Everything this screen prints is a pure
 * function of (field id, PkMon): field_value() and FLABEL[] read nothing else -- no
 * card, no ROM, no SD. So keeping ONE copy of the record that is currently ON SCREEN is
 * enough to recompute any row's OLD text on demand and diff it against the new one.
 * That is exactly as correct as shadowing the sixteen drawn strings, handles every
 * cascade for free (a changed byte anywhere shows up in whichever rows print it), and
 * costs ~112 B (one PkMon) instead of sixteen truncated-string buffers. It is also the
 * idiom pdna_summary.c's draw_left_conditional already uses, for the same reason. A
 * false MISS -- struct padding differing -- just repaints a row that did not need it; a
 * false HIT cannot happen, because what is compared IS what the strings are derived
 * from. It lives on pdna_edit()'s stack (128 B, well inside the ~256 B ceiling
 * docs/kb/c-coding-guideline.md Sec 0.2 sets for a local) rather than in EWRAM, which
 * also means a fresh entry to the screen always paints in full -- which is what we want.
 *
 * WHAT ELSE CAN HAVE PAINTED. Any picker (pdna_pick.c), the on-screen keyboard (osk.c)
 * and confirm() open with ui_clear(); a picker CANCEL returns the mon byte-identical, so
 * the mon shadow alone would think nothing changed and leave the picker's pixels sitting
 * on this screen forever. Invalidation is therefore keyed to ui_clear_gen() (ui.h) --
 * one counter every full-screen overlay in the codebase already bumps by calling
 * ui_clear(), so a picker added later needs no bookkeeping here.
 *
 * COUNTED, per keypress -- framebuffer BYTES written and sys8 GLYPH CELLS drawn (a cell
 * is 8x8 px = 128 B). The old cost was the same every press, whatever the press did:
 *   ui_clear 240x160          76,800 B
 *   two rules, 240 px each         960 B
 *   one selection panel          5,212 B   (236x9 fill + its frame)
 *   219 cells (title 12, EV 10, header 29, 16 rows of label+value ~168)  28,032 B
 *                              -----------
 *                            111,004 B / 219 cells
 * and after, per press:
 *   move the selection one row   21,324 B /  30 cells   ( 5.2x /  7.3x)  3 rows
 *   change a value, no cascade   15,796 B /  20 cells   ( 7.0x / 11.0x)  2 rows
 *   change a value, w/ cascade   18,356 B /  30 cells   ( 6.0x /  7.3x)  2 rows + corner
 *   press that changes nothing        0 B /   0 cells   (LEFT on an IV already at 0)
 *   scroll the window one row    94,684 B / 168 cells   ( 1.2x /  1.3x)  all 16 rows
 * The scroll line is barely cheaper ON PURPOSE: when `top` moves, every row genuinely
 * shows different text, so every row genuinely must be repainted -- all that is saved is
 * the full-screen wipe and the chrome that did not change. The cases the user actually
 * complained about (holding RIGHT on an IV) are 6-7x fewer bytes and 11x fewer glyphs,
 * and the presses that change nothing at all now cost nothing at all. */

/* What is on screen right now. */
typedef struct {
  PkMon    mon;        /* the record whose values are currently drawn        */
  uint32_t gen;        /* ui_clear_gen() as of that paint                    */
  int      top, sel;   /* the window and cursor that were drawn              */
  bool     valid;      /* false = nothing of ours is on screen               */
} EditPaint;

static int ev_total(const PkMon* c) {
  return c->evs[0] + c->evs[1] + c->evs[2] + c->evs[3] + c->evs[4] + c->evs[5];
}

/* Repaint ONE row in place. The wipe first, then the row, is what stops a shorter string
 * leaving the tail of a longer one behind ("picker-cancel left pixels", the ghost-ink bug
 * this codebase has hit before): the full render never had to think about it because
 * ui_clear() had already erased everything.
 *
 * `erase_top` extends the wipe one scanline UP, onto the row above's last scanline. It is
 * not free to do that -- see render()'s dirty-set rule for exactly when it is allowed. */
static void row_paint(const PkMon* c, int f, int i, bool sel, bool erase_top) {
  int y = 21 + i * 8;
  int y0 = erase_top ? y - 1 : y;
  ui_fill_rect(2, y0, 236, y + UI_ROW_H - y0, UI_BG);
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  ui_text(PDNA_EDIT_LBL_X, y, sel ? UI_SELTEXT : UI_DIM, FLABEL[f]);
  char val[72], vt[72];
  field_value(f, c, val);
  ui_truncate(vt, val, PDNA_EDIT_VAL_COLS);
  ui_text(PDNA_EDIT_VAL_X, y, sel ? UI_SELTEXT : UI_TEXT, vt);
}

static void render(const PkMon* c, int sel, int top, EditPaint* pv) {
  char line[64];
  int  evtot = ev_total(c);
  bool full  = !pv->valid || pv->gen != ui_clear_gen();

  if (full) {                       /* first paint of this screen, or an overlay wiped it */
    ui_clear();
    ui_text(4, 0, UI_TITLE, "EDIT POKEMON");
    ui_hline(0, 19, UI_SCR_W, UI_BORDER);
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, PDNA_EDIT_FOOT);
  }

  /* EV total (top-right). Both the number and its colour are pure functions of the six
   * EV bytes, so their sum is the whole comparison. */
  if (full || ev_total(&pv->mon) != evtot) {
    ui_fill_rect(160, 0, UI_SCR_W - 160, UI_ROW_H, UI_BG);
    siprintf(line, "EV %d/510", evtot);
    ui_text(160, 0, evtot > 510 ? UI_WARN : UI_DIM, line);  /* x=160: 10 cols fits "EV 510/510" */
  }

  /* Header line. Its five inputs are listed here rather than diffed as a string, so no
   * second 64 B buffer is needed to hold the old one. */
  if (full || c->species != pv->mon.species || c->level   != pv->mon.level ||
      c->nature  != pv->mon.nature  || c->isShiny != pv->mon.isShiny ||
      c->gender  != pv->mon.gender) {
    ui_fill_rect(0, 10, UI_SCR_W, UI_ROW_H, UI_BG);
    siprintf(line, "%s  Lv%u  %s%s%s", pk_species_name(c->species), (unsigned)c->level,
             pk_nature_name(c->nature), c->isShiny ? "  SHINY" : "",
             c->gender == 1 ? "  F" : c->gender == 0 ? "  M" : "");
    /* lt[] was 40 B: 29 display columns can be 29 multi-byte codepoints (NIDORAN(f),
     * POKe BALL), which ui_truncate copies through verbatim. 128 is the size Sec 1 of the
     * C guideline tells screen-string buffers to be. */
    char lt[128];
    ui_truncate(lt, line, 29);
    ui_text(4, 10, UI_DIRCLR, lt);
  }

  /* ---- which rows differ from what is drawn ---- */
  uint32_t dirty = 0;
  for (int i = 0; i < VIS_ROWS && top + i < F_NUM; i++) {
    int  f = top + i, of = pv->top + i;              /* field now / field last time */
    bool s = (f == sel), os = (of == pv->sel);
    if (full || of != f || s != os) { dirty |= 1u << i; continue; }
    char a[72], b[72];
    field_value(f, c, a);
    field_value(f, &pv->mon, b);                     /* the same row, off the shadow mon */
    if (strcmp(a, b) != 0) dirty |= 1u << i;         /* comparing UNtruncated is safe: it
                                                      * can only over-report, never under */
  }

  /* THE SELECTION PANEL IS 9 px TALL ON AN 8 px ROW PITCH. ui_panel(2, y-1, 236, 9)
   * deliberately reaches one scanline INTO the row above -- that is how the full repaint
   * has always drawn it (top row down, each panel covering the previous row's descender
   * scanline). So the selected row and the row directly above it are ONE UNIT here:
   * repainting the selected row alone would paint its panel over a descender nothing puts
   * back, and repainting the row above alone would wipe the panel's top rule. Marking
   * both, for the new cursor row AND the old one, costs at most one extra row each and
   * makes the wipe below provably self-contained: a row's erase can only ever touch
   * scanlines belonging to rows that are themselves in this set. */
  for (int p = 0; p < 2; p++) {
    int r = p ? pv->sel - pv->top : sel - top;
    if (r <= 0 || r >= VIS_ROWS) continue;           /* r == 0 has no row above it */
    if (dirty & ((1u << r) | (1u << (r - 1)))) dirty |= (1u << r) | (1u << (r - 1));
  }

  for (int i = 0; i < VIS_ROWS && top + i < F_NUM; i++) {
    if (!(dirty & (1u << i))) continue;
    bool etop = (i == 0) || (dirty & (1u << (i - 1))) != 0;   /* row 0's y-1 is blank chrome */
    row_paint(c, top + i, i, top + i == sel, etop);
  }

  pv->mon = *c;
  pv->top = top;
  pv->sel = sel;
  pv->gen = ui_clear_gen();          /* read AFTER the ui_clear() above, not before */
  pv->valid = true;
}

/* re-roll PID for a (nature, shiny, gender) combo, relaxing gender then shiny. */
static void reroll_to(EditMon* e, const PkMon* c, int nat, int shiny, int gender) {
  uint8_t ratio = pk_species_gender_ratio(c->species);
  if (em_reroll(e, nat, shiny, gender, ratio)) return;
  if (em_reroll(e, nat, shiny, -1, ratio)) return;
  em_reroll(e, nat, -1, -1, ratio);
}

void em_field_adjust(int f, int dir, bool big, EditMon* e, const PkMon* c) {
  int s = big ? 10 : 1;
  switch (f) {
    case F_SPECIES: em_set_species(e, clampi(c->species + dir * (big ? 10 : 1), 1, 411)); break;
    case F_LEVEL:   em_set_level(e, clampi(c->level + dir * s, 1, 100)); break;
    case F_FRIEND:  em_set_friendship(e, clampi(c->friendship + dir * s, 0, 255)); break;
    case F_ITEM:    em_set_item(e, clampi(c->heldItem + dir * (big ? 10 : 1), 0, 65535)); break;
    case F_ABILITY: em_set_ability(e, c->abilityNum ^ 1); break;
    case F_NATURE:  reroll_to(e, c, (c->nature + (dir > 0 ? 1 : 24)) % 25,
                              c->isShiny ? 1 : 0, c->gender < 2 ? c->gender : -1); break;
    case F_SHINY:   reroll_to(e, c, c->nature, c->isShiny ? 0 : 1, c->gender < 2 ? c->gender : -1); break;
    case F_GENDER: { uint8_t r = pk_species_gender_ratio(c->species);
                     if (r >= 1 && r <= 253) reroll_to(e, c, c->nature, c->isShiny ? 1 : 0, c->gender ^ 1); break; }
    case F_IV0: case F_IV1: case F_IV2: case F_IV3: case F_IV4: case F_IV5:
      em_set_iv(e, f - F_IV0, (uint8_t)(big ? (dir > 0 ? 31 : 0) : clampi(c->ivs[f - F_IV0] + dir, 0, 31))); break;
    case F_EV0: case F_EV1: case F_EV2: case F_EV3: case F_EV4: case F_EV5:
      em_set_ev(e, f - F_EV0, (uint8_t)clampi(c->evs[f - F_EV0] + dir * s, 0, 255)); break;
    case F_MV0: case F_MV1: case F_MV2: case F_MV3:
      em_set_move(e, f - F_MV0, clampi(c->moves[f - F_MV0] + dir * (big ? 10 : 1), 0, 65535)); break;
    /* Max PP: the ONLY knob is the slot's PP-Up count, 0..3. LEFT/RIGHT steps it, the
     * shoulders jump to the ends (the IV row's idiom). em_set_ppups carries current PP
     * along, exactly as using the item does. */
    case F_PPU0: case F_PPU1: case F_PPU2: case F_PPU3: {
      int i = f - F_PPU0;
      int v = big ? (dir > 0 ? 3 : 0) : clampi(pp_ups(c, i) + dir, 0, 3);
      em_set_ppups(e, i, (uint8_t)v);
      break;
    }
    case F_PP0: case F_PP1: case F_PP2: case F_PP3:
      em_set_pp(e, f - F_PP0, (uint8_t)clampi(c->pp[f - F_PP0] + dir * s, 0, pp_max(c, f - F_PP0))); break;
    case F_BALL:     { int v = c->pokeball + dir; if (v < 1) v = 12; if (v > 12) v = 1; em_set_ball(e, (uint8_t)v); break; }
    /* Region: step to the next region that HAS places for this origin game, and re-home
     * the met location to its first one — region and place are one field at two zooms. */
    case F_METREGION: {
      int gf = g3_game_filter_for(c->metGame);
      int r = g3_region_of(c->metLocation);
      if (r < 0) r = 0;
      for (int t = 0; t < G3_RGN_COUNT; t++) {
        r = (r + (dir > 0 ? 1 : G3_RGN_COUNT - 1)) % G3_RGN_COUNT;
        uint16_t first = g3_place_first(r, gf, 0xFFFF);
        if (first != 0xFFFF) { em_set_metloc(e, (uint8_t)first); break; }
      }
      break;
    }
    /* Step through VALID ids only: 214..252 is a hole no Gen-3 game emits, and the big
     * step stays inside the current region so it cannot silently cross into another. */
    case F_METLOC: {
      int gf = g3_game_filter_for(c->metGame);
      uint16_t v = c->metLocation;
      int reps = big ? 10 : 1;
      int rgn = big ? g3_region_of(c->metLocation) : -1;
      for (int t = 0; t < reps; t++) v = g3_place_step(v, dir, rgn, gf);
      em_set_metloc(e, (uint8_t)v);
      break;
    }
    case F_METLEVEL: em_set_metlevel(e, (uint8_t)clampi(c->metLevel + dir * s, 0, 100)); break;
    case F_METGAME:  { int v = c->metGame + dir; if (v < 0) v = 15; if (v > 15) v = 0; em_set_metgame(e, (uint8_t)v); break; }
    case F_CT0: case F_CT1: case F_CT2: case F_CT3: case F_CT4: case F_CT5:
      em_set_contest(e, f - F_CT0, (uint8_t)clampi(c->contest[f - F_CT0] + dir * s, 0, 255)); break;
    case F_RIB0: case F_RIB1: case F_RIB2: case F_RIB3: case F_RIB4: {
      int cat = f - F_RIB0;
      em_set_ribbon_rank(e, cat, (uint8_t)clampi(gc_ribbon_get(c->ribbons, cat) + dir, 0, 4));
      break;
    }
    default: break;   /* names: use A */
  }
}

void em_field_press(int f, EditMon* e, const PkMon* c) {
  char buf[20];
  switch (f) {
    case F_SPECIES: { uint16_t id = pick_species(c->species);
                      if (id != 0xFFFF) {
                        em_set_species(e, id);
                        if (id == 201) {                       /* Unown -> also pick the letter */
                          int form = pick_unown_form(pk_unown_form(e->personality));
                          if (form >= 0) em_set_unown_form(e, form);
                        }
                      }
                      break; }
    case F_ITEM:    { uint16_t id = pick_item(c->heldItem);   if (id != 0xFFFF) em_set_item(e, id); break; }
    case F_NATURE:  { uint8_t nt = pick_nature(c->nature); reroll_to(e, c, nt, c->isShiny ? 1 : 0, c->gender < 2 ? c->gender : -1); break; }
    case F_MV0: case F_MV1: case F_MV2: case F_MV3:
      { uint16_t id = pick_move(c->moves[f - F_MV0]); if (id != 0xFFFF) em_set_move(e, f - F_MV0, id); break; }
    case F_PPU0: case F_PPU1: case F_PPU2: case F_PPU3: {                    /* A = 0 <-> 3 Ups */
      int i = f - F_PPU0;
      em_set_ppups(e, i, (uint8_t)(pp_ups(c, i) == 3 ? 0 : 3));
      break;
    }
    case F_PP0: case F_PP1: case F_PP2: case F_PP3:                          /* A = restore to max PP */
      em_set_pp(e, f - F_PP0, pp_max(c, f - F_PP0)); break;
    case F_NICK: if (osk_input("NICKNAME", c->nickname, buf, 11)) em_set_nickname(e, buf); break;
    case F_OT:   if (osk_input("OT NAME", c->otName, buf, 8))    em_set_otname(e, buf); break;
    case F_ABILITY: em_set_ability(e, pick_ability(c->species, c->abilityNum)); break;
    case F_SHINY:   reroll_to(e, c, c->nature, c->isShiny ? 0 : 1, c->gender < 2 ? c->gender : -1); break;
    case F_GENDER: { uint8_t r = pk_species_gender_ratio(c->species);
                     if (r >= 1 && r <= 253) reroll_to(e, c, c->nature, c->isShiny ? 1 : 0, c->gender ^ 1); break; }
    case F_FRIEND:  em_set_friendship(e, c->friendship == 255 ? 0 : 255); break;   /* quick toggle */
    case F_IV0: case F_IV1: case F_IV2: case F_IV3: case F_IV4: case F_IV5:
      em_set_iv(e, f - F_IV0, c->ivs[f - F_IV0] == 31 ? 0 : 31); break;             /* 0 <-> max */
    case F_EV0: case F_EV1: case F_EV2: case F_EV3: case F_EV4: case F_EV5: {
      uint8_t v = c->evs[f - F_EV0];                                                /* cycle 0/4/252/255 */
      em_set_ev(e, f - F_EV0, v < 4 ? 4 : v < 252 ? 252 : v < 255 ? 255 : 0);
      break;
    }
    case F_BALL:     em_set_ball(e, pick_ball(c->pokeball)); break;                  /* the 12-ball list */
    case F_METGAME:  { int v = c->metGame + 1;  if (v > 15) v = 0; em_set_metgame(e, (uint8_t)v); break; }/* next game */
    /* Region first, then the place list already scoped to it (Guy: "same for region"). */
    case F_METREGION: {
      int r = pick_region(g3_region_of(c->metLocation), c->metGame);
      if (r < 0) break;
      uint16_t id = pick_metloc(c->metLocation, c->metGame, r);
      if (id == 0xFFFF) {                       /* backed out of the place list */
        if (g3_region_of(c->metLocation) == r) break;   /* already there: change nothing */
        id = g3_place_first(r, g3_game_filter_for(c->metGame), c->metLocation);
      }
      em_set_metloc(e, (uint8_t)id);
      break;
    }
    case F_METLOC: { uint16_t id = pick_metloc(c->metLocation, c->metGame, -1);
                     if (id != 0xFFFF) em_set_metloc(e, (uint8_t)id); break; }
    case F_METLEVEL: em_set_metlevel(e, (uint8_t)(c->metLevel >= 100 ? 1 : 100)); break;                  /* 1 <-> 100 */
    case F_CT0: case F_CT1: case F_CT2: case F_CT3: case F_CT4: case F_CT5: {    /* cycle 0/128/255 */
      uint8_t v = c->contest[f - F_CT0];
      em_set_contest(e, f - F_CT0, v < 128 ? 128 : v < 255 ? 255 : 0);
      break;
    }
    case F_RIB0: case F_RIB1: case F_RIB2: case F_RIB3: case F_RIB4: {   /* 0 <-> Master */
      int cat = f - F_RIB0;
      em_set_ribbon_rank(e, cat, gc_ribbon_get(c->ribbons, cat) ? 0 : (uint8_t)GC_RANK_MASTER);
      break;
    }
    default: break;
  }
}

static bool confirm(void) {
  ui_clear();
  ui_text(20, 50, UI_TITLE, "Commit changes to the save?");
  ui_text(20, 66, UI_TEXT, "A = write (backs up first)");
  ui_text(20, 78, UI_WARN, "B = cancel");
  ui_text(20, 100, UI_DIM, "Original backed up to .bak,");
  ui_text(20, 110, UI_DIM, "new save verified on write.");
  u16 k = s_wait(KEY_A | KEY_B);
  return (k & KEY_A) != 0;
}

bool pdna_edit(const uint8_t* rec, bool is_party, uint8_t* out_rec) {
  EditMon e;
  gen3_edit_load(rec, is_party, &e);
  PkMon cur;
  refresh(&e, &cur);

  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
  int sel = 0, top = 0;
  bool committed = false;
  EditPaint pv;
  memset(&pv, 0, sizeof pv);         /* .valid = false: the first render() paints in full */
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;
    render(&cur, sel, top, &pv);

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) break;
    else if (k & KEY_START) { if (confirm()) { gen3_edit_commit(&e, out_rec); committed = true; break; } }
    else if (k & KEY_UP)   sel = (sel == 0) ? F_NUM - 1 : sel - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % F_NUM;
    else if (k & KEY_A)    { em_field_press(sel, &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_LEFT)  { em_field_adjust(sel, -1, false, &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_RIGHT) { em_field_adjust(sel, +1, false, &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_L)     { em_field_adjust(sel, -1, true,  &e, &cur); refresh(&e, &cur); }
    else if (k & KEY_R)     { em_field_adjust(sel, +1, true,  &e, &cur); refresh(&e, &cur); }
  }
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);    /* restore the global repeat set */
  return committed;
}
