#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbdaycare.h"
#include "gb_daycare.h"
#include "gb_edit.h"
#include "pdna_gen12.h"      /* gb_rollback / gb_persist / gb12_arena_tail(_release) */
#include "pdna_gbsummary.h"  /* the View/Edit screen (BACKLOG #41's own Gen-1/2 card) */
#include "pdna_summary.h"   /* pdna_summary_quiet_save (#234 s4 D2) */
#include "pdna_layout.h"     /* PDNA_DCY_*, PDNA_DCPOP_* -- the SAME geometry pdna_daycare() uses */
#include "data_tables.h"     /* pk_species_name, pk_move_name */
#include "log.h"             /* log_line */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"        /* msg_wait / app_confirm / app_yard_visitors_ok / app_icons_* */
#include "pdna_yard.h"       /* BACKLOG #114: the shared yard scene (dc_scene/dc_pointer/
                              * dc_icon_over_bg/pdna_yard_roll/pdna_yard_place) */
#include "mon_icons.h"       /* mon_icon_for_form_frame / mon_icon_anim_cheap */

/* See pdna_gbdaycare.h for the shape this mirrors and the two documented scope
 * reductions (no yard art; deposit/withdraw touch `cur_box` only, never the party). */

#define GBDC_A4(n)  (((uint32_t)(n) + 3u) & ~3u)

/* wait_keys() is static to pdna_main.c -- every other GB screen (pdna_gbbag.c,
 * pdna_gbpack.c) carries its own copy of this exact idiom rather than exporting it;
 * this file does the same. */
static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* GEN1_BOX_CAPACITY == G2_BOX_CAPACITY == 20 (gen1_save.h / gen2_save.h) -- the widest
 * either generation's storage box ever holds, so the picker's index array is sized to
 * this fixed bound rather than a VLA. */
#define GBDC_BOX_MAX     20
#define GBDC_PICK_VROWS  10

/* ---- the deposit picker: cur_box's occupied, non-Egg slots, box-shaped by
 * construction (a storage box never holds a party-shaped record) -- see the header's
 * own note on why the party is never a source here. Mirrors pdna_pick.c's list idiom
 * (scrollable rows, U/D + A/B), the smallest shape that fits gb_daycare's own
 * is_party constraint (BACKLOG #107: no reusable "pick one owned mon" screen exists
 * yet). `list` is the caller's GBS_LIST_BYTES staging buffer (arena-carved, not a
 * stack local -- see pdna_gbdaycare() below). Returns true with `out` filled on A,
 * false on B or when the box has nothing pickable. */
static bool gbdc_pick(GbSession* s, int cur_box, uint8_t* list, GbEditMon* out, int* picked_slot) {
  GbsStatus lst = gbs_load_list(s, cur_box, list);
  if (lst != GBS_OK) { snd_deny(); msg_wait("PUT IN", UI_WARN, gbs_status_text(lst), 0); return false; }
  int count = gb_list_count(s->gen, list, cur_box);
  if (count > GBDC_BOX_MAX) count = GBDC_BOX_MAX;   /* defensive; never true today */

  int idx[GBDC_BOX_MAX], n = 0;
  for (int i = 0; i < count; i++) {
    GbEditMon e;
    if (!gb_load(&e, s->gen, list, cur_box, i)) continue;
    /* gbd_deposit() itself refuses an Egg (gb_daycare.c's own structural gate) --
     * filtered here too so the picker never offers a row that would just bounce
     * back with a refusal message. */
    if (gb_is_egg(&e)) continue;
    idx[n++] = i;
  }
  if (n == 0) {
    snd_deny();
    msg_wait("PUT IN", UI_DIM, "That box has nothing to put in.", "(no Eggs -- deposit those in-game)");
    return false;
  }

  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + GBDC_PICK_VROWS) top = sel - GBDC_PICK_VROWS + 1;

    ui_clear();
    ui_text(4, 3, UI_TITLE, "PUT IN - PICK A POKEMON");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    int shown = n - top; if (shown > GBDC_PICK_VROWS) shown = GBDC_PICK_VROWS;
    for (int r = 0; r < shown; r++) {
      int i = idx[top + r];
      GbEditMon e;
      const char* nm = "?";
      char nick[GB_TEXT_MAX]; nick[0] = 0;
      uint8_t lvl = 0;
      if (gb_load(&e, s->gen, list, cur_box, i)) {
        gb_get_nickname(&e, nick, sizeof nick);
        uint16_t dex = gb_get_species_dex(&e);
        nm = nick[0] ? nick : ((dex >= 1 && dex <= 251) ? pk_species_name(dex) : "?");
        lvl = gb_get_level(&e);
      }
      int y = 18 + r * 12; bool sh = (top + r == sel);
      if (sh) ui_panel(2, y - 1, UI_SCR_W - 4, 12, UI_SEL, UI_TITLE);
      char row[12]; siprintf(row, "Lv.%-3u", (unsigned)lvl);
      ui_text(6, y, sh ? UI_SELTEXT : UI_DIM, row);
      ui_ptext_fit(62, y, 170, sh ? UI_SELTEXT : UI_TEXT, nm);
    }
    ui_text(4, 152, UI_DIM, "A pick  B cancel");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      if (!gb_load(out, s->gen, list, cur_box, idx[sel])) {
        snd_error(); msg_wait("PUT IN", UI_WARN, "Could not read that slot.", 0);
        return false;
      }
      *picked_slot = idx[sel];
      return true;
    }
  }
}

/* The per-slot action popup -- the Gen-1/2 twin of pdna_main.c's dc_menu(). `occupied`
 * picks the row set (View/Edit + Take out, vs Put in); Cancel is always last. */
static int gbdc_menu(bool occupied, bool can_put_here, bool can_take) {
  const char* rows[PDNA_DCPOP_MAX]; int act[PDNA_DCPOP_MAX], nr = 0;
  if (occupied) {
    rows[nr] = "View / Edit"; act[nr++] = 0;
    if (can_take) { rows[nr] = "Take out"; act[nr++] = 1; }
  } else if (can_put_here) {
    rows[nr] = "Put in (from this box)"; act[nr++] = 2;
  }
  rows[nr] = "Cancel"; act[nr++] = -1;
  if (nr == 1) return -1;   /* nothing offered but Cancel -- no point opening the popup */

  int sel = 0;
  for (;;) {
    int my, mh;
    ui_popup_vfit(nr, PDNA_DCPOP_ROW_H, PDNA_DCPOP_HEAD, PDNA_DCPOP_FOOT, &my, &mh);
    const int mx = 30, mw = 190;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "DAY-CARE");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < nr; i++) {
      int y = my + PDNA_DCPOP_HEAD + i * PDNA_DCPOP_ROW_H; bool sh = (i == sel);
      if (sh) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, sh ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : nr - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % nr;
    else if (k & KEY_A)    return act[sel];
  }
}

/* ---- BACKLOG #373: the take-out growth panel (shared with pdna_main.c's dc_withdraw) ---- */

static EWRAM_BSS DcGrow s_dc_grow;
DcGrow* pdna_dc_grow_buf(void) { return &s_dc_grow; }

#define DCG_ROW_H 12

/* One event line: "<new> replaces <old>" or "<new> learned". `buf` is the caller's. */
static void dcg_event_line(char* buf, int cap, const DcLearn* l) {
  if (l->replaced) sniprintf(buf, (size_t)cap, "%s replaces %s", pk_move_name(l->newmove), pk_move_name(l->replaced));
  else             sniprintf(buf, (size_t)cap, "%s learned", pk_move_name(l->newmove));
}

/* "GREW IN DAY-CARE": how far it rose and which attack replaces which, then the three
 * choices (Guy 2026-10-04): leave it inside / take it and learn / take it and keep its
 * old moves. B = leave inside. Rows are drawn into one popup sized by ui_popup_vfit. */
int pdna_dc_grow_panel(const DcGrow* g, bool no_rom) {
  const char* opts[3]; int act[3], nopt = 0;
  opts[nopt] = "Leave inside"; act[nopt++] = PDNA_DCG_LEAVE;
  if (no_rom)               { opts[nopt] = "Take, keep moves"; act[nopt++] = PDNA_DCG_KEEP; }
  else if (g->n_learn == 0) { opts[nopt] = "Take out";         act[nopt++] = PDNA_DCG_KEEP; }
  else { opts[nopt] = "Take, learn moves"; act[nopt++] = PDNA_DCG_LEARN;
         opts[nopt] = "Take, keep moves";  act[nopt++] = PDNA_DCG_KEEP; }

  bool big = g->overflow || g->n_learn > 4;
  int nev = no_rom || g->n_learn == 0 ? 0 : (big ? 2 : g->n_learn);
  int ntext = 1 + (no_rom || g->n_learn == 0 ? 1 : nev + (big ? 4 : 0));   /* + the Lv line below */
  /* big: "...and N more", "Moves now:", 2 name lines (two names each) */
  int sel = 0;
  for (;;) {
    int my, mh;
    ui_popup_vfit(ntext + nopt, DCG_ROW_H, PDNA_DCPOP_HEAD, PDNA_DCPOP_FOOT, &my, &mh);
    const int mx = 8, mw = 224;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "GREW IN DAY-CARE");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    char line[48];
    int y = my + PDNA_DCPOP_HEAD;
    siprintf(line, "Lv %d -> Lv %d  (+%d)", (int)g->lv_before, (int)g->lv_after,
             (int)g->lv_after - (int)g->lv_before);
    ui_ptext_fit(mx + 8, y, mw - 16, UI_TEXT, line); y += DCG_ROW_H;
    if (no_rom) {
      ui_ptext_fit(mx + 8, y, mw - 16, UI_DIM, "Moves need a ROM: " PDNA_ROM_WHERE); y += DCG_ROW_H;
    } else if (g->n_learn == 0) {
      ui_ptext_fit(mx + 8, y, mw - 16, UI_DIM, "No new moves."); y += DCG_ROW_H;
    } else {
      for (int i = 0; i < nev; i++) {
        dcg_event_line(line, sizeof line, &g->learn[i]);
        ui_ptext_fit(mx + 8, y, mw - 16, UI_TEXT, line); y += DCG_ROW_H;
      }
      if (big) {
        siprintf(line, "...and %d more", g->n_learn - nev);
        ui_ptext_fit(mx + 8, y, mw - 16, UI_DIM, line); y += DCG_ROW_H;
        ui_ptext_fit(mx + 8, y, mw - 16, UI_DIM, "Moves now:"); y += DCG_ROW_H;
        for (int r = 0; r < 2; r++) {
          sniprintf(line, sizeof line, "%s, %s", pk_move_name(g->moves_after[r * 2]),
                   pk_move_name(g->moves_after[r * 2 + 1]));
          ui_ptext_fit(mx + 8, y, mw - 16, UI_TEXT, line); y += DCG_ROW_H;
        }
      }
    }
    for (int i = 0; i < nopt; i++) {
      bool sh = (i == sel);
      if (sh) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, sh ? UI_SELTEXT : UI_TEXT, opts[i]);
      y += DCG_ROW_H;
    }
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return PDNA_DCG_LEAVE;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : nopt - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % nopt;
    else if (k & KEY_A)    return act[sel];
  }
}

/* Land `mon` (already withdrawn, box-shaped) the same way retail's own counter does:
 * the PARTY if it has room, else the FIRST storage box with a free slot, else refuse.
 * gbs_insert() itself refuses the party pseudo-box outright (gb_session.h), so the
 * party leg is composed as gbs_insert into a scratch storage box, then gbs_move() from
 * that box into the party -- gbs_move's own Mail-shift/party-floor rules apply exactly
 * as they would for a manual box->party move. Every step here only touches RAM; the
 * caller (gbdc_take) is the one that either calls gb_persist() once everything below
 * returns GBS_OK, or gb_rollback()s the WHOLE image on any failure -- so a party leg
 * that needs its own extra RAM commit (insert, then move) is exactly as safe to unwind
 * as gbd_withdraw()+gbs_insert() already was: gb_rollback() restores from pristine
 * unconditionally, regardless of how many intermediate gbs_* calls ran.
 * `list`/`list2` are the caller's two GBS_LIST_BYTES staging buffers (gbs_move needs
 * two, src+dst, loaded independently). On success, `*landed_box` is the box index the
 * mon ended up in, or -1 for the party (so the caller can name the landing in its own
 * message); on failure, `*out_status` explains why and nothing has changed. */
static bool gbdc_land(GbSession* s, const GbEditMon* mon, uint8_t* list, uint8_t* list2,
                      int* landed_box, GbsStatus* out_status, Gb12BaseSt* base_st) {
  int nb = gbs_nboxes(s);
  int party = gbs_party_box(s);

  GbsStatus pld = gbs_load_list(s, party, list);
  int pcount = (pld == GBS_OK) ? gb_list_count(s->gen, list, party) : -1;
  bool party_room = (pld == GBS_OK && pcount >= 0 && pcount < gb_list_capacity(s->gen, party));

  if (party_room) {
    /* Find a storage box with a free slot to stage the insert -- try `cur_box`-agnostic,
     * first box first, since any box works equally well as a scratch landing spot. */
    if (s->gen == GB_GEN1) {
      /* #367: gbs_move box->party always refuses on Gen 1 (no base stats), so the party
       * leg goes through gbs_insert_party() with the ROM's BaseStats row, the same
       * stat-recomputing path the Bank TO-GAME landing uses. No ROM row = the old,
       * honest GBS_ERR_NEEDS_BASE refusal (nothing changed). */
      GbGen1Base g1base;
      *base_st = gb12_gen1_base_for(mon, &g1base);
      if (*base_st != GB12_BASE_OK) { *out_status = GBS_ERR_NEEDS_BASE; return false; }
      int pslot = -1;
      GbsStatus pst = gbs_insert_party_daycare(s, mon, &g1base, &pslot, list);
      if (pst == GBS_OK) { *landed_box = -1; return true; }
      *out_status = pst;
      return false;
    }
    for (int b = 0; b < nb; b++) {
      GbsStatus bld = gbs_load_list(s, b, list);
      if (bld != GBS_OK) continue;
      int bc = gb_list_count(s->gen, list, b);
      if (bc < 0 || bc >= gb_list_capacity(s->gen, b)) continue;

      int slot_out = -1;
      GbsStatus ist = gbs_insert(s, b, mon, &slot_out, list);
      if (ist != GBS_OK) continue;   /* unwritable/full after all -- try the next box */

      int to_slot = -1;
      GbsStatus mst = gbs_move(s, b, slot_out, party, &to_slot, list, list2);
      if (mst == GBS_OK) { *landed_box = -1; return true; }
      *out_status = mst;
      return false;   /* the insert already landed in RAM -- caller must gb_rollback() */
    }
    /* No box had room to stage the insert (every box full while the party is not) --
     * fall through to the "first free box" leg, which will also find nothing and
     * report GBS_ERR_FULL, the honest answer either way. */
  }

  for (int b = 0; b < nb; b++) {
    GbsStatus bld = gbs_load_list(s, b, list);
    if (bld != GBS_OK) continue;
    int bc = gb_list_count(s->gen, list, b);
    if (bc < 0 || bc >= gb_list_capacity(s->gen, b)) continue;

    int slot_out = -1;
    GbsStatus ist = gbs_insert(s, b, mon, &slot_out, list);
    if (ist == GBS_OK) { *landed_box = b; return true; }
    *out_status = ist;
    return false;
  }

  *out_status = GBS_ERR_FULL;
  return false;
}

/* Take `slot` out, landing it retail's own way: party-first, else the first box with
 * room, else refuse -- named in the success message so the player knows where it went.
 * gbd_withdraw() commits to RAM first; any landing failure below rolls the WHOLE image
 * back from pristine, so the withdraw and the landing are never left half-done on the
 * card, only (briefly) in RAM. */
/* BACKLOG #373: what the boarded mon grew by, from the record the screen already holds.
 * Fills the shared DcGrow staging struct and returns true when it crossed at least one
 * level (then *no_rom says whether the learnset could not be read). Both generations keep
 * the grown EXP and re-derive the level from it in gbdc_grow_apply (orchestrator ruling D4:
 * no floor, unlike pokecrystal's RetrieveBreedmon). Nothing is written here. */
static bool gbdc_grow_preview(const GbSession* s, const GbEditMon* mon, DcGrow* g, bool* no_rom) {
  uint16_t dex = gb_get_species_dex(mon);
  if (!dex) return false;
  uint8_t lv0 = gb_get_level(mon);
  uint8_t lv1 = gb_level_from_exp(dex, gb_get_exp(mon));
  if (lv1 <= lv0) return false;
  memset(g, 0, sizeof *g);
  g->lv_before = lv0; g->lv_after = lv1;
  uint8_t cur4[4], out4[4];
  for (int k = 0; k < 4; k++) { cur4[k] = gb_get_move(mon, k); out4[k] = cur4[k]; }
  int n_ev = 0; bool ovf = false;
  DcLearn ev[DC_LEARN_MAX];
  int kept = gb_daycare_learn(s->gen, dex, lv0, lv1, cur4, out4, ev, &n_ev, &ovf);
  *no_rom = kept < 0;
  if (kept >= 0) {
    g->n_learn = n_ev; g->overflow = ovf;
    for (int i = 0; i < DC_LEARN_MAX; i++) g->learn[i] = ev[i];
  }
  for (int k = 0; k < 4; k++) g->moves_after[k] = out4[k];
  return true;
}

/* Write the grown level/EXP (always) and, when with_moves, the learned moves, into the
 * freshly withdrawn `mon`. New moves get base PP; shifted moves keep their original PP and PP Ups. */
static void gbdc_grow_apply(GbEditMon* mon, uint8_t gen, const DcGrow* g, bool with_moves) {
  (void)gb_set_exp(mon, gb_get_exp(mon));   /* level re-derived, EXP kept — a mid-level EXP is a normal, legal record */
  if (!with_moves) return;
  uint8_t om[4], opp[4], oup[4];
  for (int k = 0; k < 4; k++) { om[k] = gb_get_move(mon, k); opp[k] = gb_get_pp(mon, k); oup[k] = gb_get_ppup(mon, k); }
  for (int k = 0; k < 4; k++) {
    uint8_t mv = (uint8_t)g->moves_after[k];
    if (om[k] == mv) continue;
    (void)gb_set_move(mon, k, mv);                    /* a NEW move: base PP, no PP Ups */
    for (int j = 0; j < 4; j++)                       /* a SHIFTED move keeps its PP + PP Ups (pokered */
      if (mv && om[j] == mv) {                        /* WriteMonMoves shifts the PP bytes with the moves) */
        (void)gb_set_ppup(mon, k, oup[j]);
        (void)gb_set_pp(mon, k, opp[j]);
        break;
      }
  }
}

static void gbdc_take(GbSession* s, const GbDaycare* dc, int slot, uint8_t* list, uint8_t* list2) {
  DcGrow* grow = pdna_dc_grow_buf();
  bool grew = false, grow_norom = false, with_moves = false;
  if (slot >= 0 && slot < 2 && dc->slot[slot].occupied) {
    grew = gbdc_grow_preview(s, &dc->slot[slot].mon, grow, &grow_norom);
    if (grew) {
      int pick = pdna_dc_grow_panel(grow, grow_norom);
      if (pick == PDNA_DCG_LEAVE) return;                 /* nothing written */
      with_moves = (pick == PDNA_DCG_LEARN);
    }
  }
  GbEditMon mon;
  GbsStatus wst = gbd_withdraw(s, slot, &mon);
  if (wst != GBS_OK) { snd_error(); msg_wait("TAKE OUT", UI_WARN, gbs_status_text(wst), 0); return; }
  if (grew) {
    gbdc_grow_apply(&mon, s->gen, grow, with_moves);
    log_line("daycare: take %s Lv%d->%d +%d moves(%s)", pk_species_name(gb_get_species_dex(&mon)),
             (int)grow->lv_before, (int)grow->lv_after, with_moves ? grow->n_learn : 0,
             grow_norom ? "no rom" : (with_moves ? "learn" : "keep"));
  }

  int landed_box = -2;
  GbsStatus lst = GBS_OK;
  Gb12BaseSt base_st = GB12_BASE_OK;
  if (!gbdc_land(s, &mon, list, list2, &landed_box, &lst, &base_st)) {
    gb_rollback();
    snd_deny();
    /* D6: the no-ROM refusal speaks like the Bank twin (pdna_gen12.c) -- name the missing
     * ROM / the bad ROM instead of the generic "needs base stats" status text. */
    if (lst == GBS_ERR_NEEDS_BASE && base_st == GB12_BASE_NO_ROM) {
      gb_gen12_norom_msg(GB_GEN1);
    } else if (lst == GBS_ERR_NEEDS_BASE && base_st == GB12_BASE_BAD) {
      msg_wait(PDNA_GBEDIT_MOVE_REFUSED_TITLE, UI_WARN, PDNA_GBEDIT_MOVE_NEEDSBASE_L2, "Nothing was changed.");
    } else {
      msg_wait("TAKE OUT", UI_WARN, gbs_status_text(lst), "Nothing was changed.");
    }
    return;
  }
  if (!gb_hold_commit("daycare-take")) return;   /* gb_persist already reported any refusal */
  snd_ok();
  char line[32];
  if (landed_box < 0) siprintf(line, "Sent to the party.");
  else                siprintf(line, "Sent to Box %d.", landed_box + 1);
  msg_wait("TAKEN OUT", UI_OK, line, gb_hold_live() ? "Save on exit to keep it." : "Saved.");
}

/* Take the Egg out (Gen 2 only), landing it like gbdc_take() (party first, then a box with room).
 * gbd_withdraw_egg()'s own `out` is already Egg-shaped (list_species == G2_LIST_EGG,
 * gb_daycare.h's own header note), so gbs_insert() lands a real Egg, not the species
 * underneath it. */
static void gbdc_take_egg(GbSession* s, uint8_t* list, uint8_t* list2) {
  if (!app_confirm("TAKE THE EGG?", "Party first, else a box with room.")) return;
  GbEditMon egg;
  GbsStatus wst = gbd_withdraw_egg(s, &egg);
  if (wst != GBS_OK) { snd_error(); msg_wait("EGG", UI_WARN, gbs_status_text(wst), 0); return; }

  /* Retail hands a collected Egg to the PARTY (daycare.asm .GiveEgg: wPartyCount vs
   * PARTY_LENGTH first) -- the same party-first landing gbdc_take() uses (re-verify N2). */
  int landed_box = -2;
  GbsStatus ist = GBS_OK;
  Gb12BaseSt egg_base_st = GB12_BASE_OK;   /* Gen 2 only: never set */
  if (!gbdc_land(s, &egg, list, list2, &landed_box, &ist, &egg_base_st)) {
    gb_rollback();
    snd_deny();
    msg_wait("EGG", UI_WARN, gbs_status_text(ist), "Nothing was changed.");
    return;
  }
  if (!gb_hold_commit("daycare-egg")) return;
  snd_ok();
  char line[32];
  if (landed_box < 0) siprintf(line, "Sent to the party.");
  else                siprintf(line, "Sent to Box %d.", landed_box + 1);
  msg_wait("EGG TAKEN", UI_OK, line, gb_hold_live() ? "Save on exit to keep it." : "Saved.");
}

/* Put a box-picked mon into `slot` -- a MOVE, not a paste (review D1): the picked
 * source slot is deleted from `cur_box` once the deposit itself has landed, the same
 * two-commit shape gbdc_take() already uses (commit #1, then a second commit that
 * either lands too or rolls the WHOLE image back). gbd_deposit() itself refuses an
 * occupied slot, an Egg, or a party-shaped record -- all three are already impossible
 * to reach here (the caller only offers this action on an empty slot; gbdc_pick
 * already filters Eggs and only ever reads box-shaped storage), so any refusal here is
 * a genuine, user-visible surprise worth reporting rather than a dead branch. */
static void gbdc_deposit(GbSession* s, int slot, int cur_box, uint8_t* list) {
  GbEditMon mon;
  int picked_slot = -1;
  if (!gbdc_pick(s, cur_box, list, &mon, &picked_slot)) return;
  if (!app_confirm("Send to Day-Care?", "Moves this Pokemon there.")) return;

  GbsStatus st = gbd_deposit(s, slot, &mon);
  if (st != GBS_OK) { snd_deny(); msg_wait("PUT IN", UI_WARN, gbs_status_text(st), 0); return; }

  /* The deposit already landed in RAM (gbd_deposit calls gbs_finish() itself); now
   * remove the source from `cur_box` before anything is written to the card. gbs_delete
   * reloads `cur_box`'s list itself, so the stale copy gbdc_pick left in `list` is not
   * reused here. */
  GbsStatus dst = gbs_delete(s, cur_box, picked_slot, list);
  if (dst != GBS_OK) {
    gb_rollback();
    snd_deny();
    msg_wait("PUT IN", UI_WARN, gbs_status_text(dst), "Nothing was changed.");
    return;
  }
  if (!gb_hold_commit("daycare-put")) return;   /* gb_persist already reported any refusal */
  snd_ok();
  msg_wait("LEFT AT DAY CARE", UI_OK, gb_hold_live() ? "Moved from the box." : "Moved from the box. Saved.", 0);
}

/* View/Edit `start_slot`, Gen-3-parity shape (pdna_daycare()'s own card-editor loop):
 * pdna_gbsummary() over a THROWAWAY copy; U/D (when there IS a second slot) scrolls to
 * it, exactly like pdna_inspect()'s own `nav` contract. A PURE VIEW (never saved)
 * makes ZERO writes to the session -- no withdraw, no redeposit -- so the slot's own
 * compatibility bit (Gen 2 slot 0 only) is never disturbed by opening this screen.
 * Only a CONFIRMED edit pays for the withdraw+redeposit round-trip gbd_deposit()
 * needs (there is no lower-level "rewrite just the record" entry point in
 * gb_daycare.h) -- and that round-trip clears the slot's own compatibility bit
 * (gbd_withdraw()'s own documented side effect on slot 0), which is the right call:
 * an edited Pokemon's compatibility has to be recomputed by the game itself anyway,
 * exactly the same "let retail recompute it" posture gbd_deposit()'s own header
 * comment already takes for every fresh deposit. */
static void gbdc_view_edit(GbSession* s, GbDaycare* dc, int start_slot, bool can_edit) {
  int nrows = dc->gen1 ? 1 : 2;
  int slot = start_slot;
  GbEditMon edited = dc->slot[slot].mon;
  gb_set_caught_available(&edited, gb_session_is_crystal(s));   /* #356: gb_load_parts leaves has_caught false; the session knows Crystal */
  int nav;
  do {
    char note[40];
    siprintf(note, "Day-Care %s", dc->gen1 ? "boarder" : (slot == 0 ? "Man's Pokemon" : "Lady's Pokemon"));
    bool saved = false;
    pdna_summary_quiet_save(true);            /* #234 s4: a resident-session edit is held (one step; the exit confirm writes) */
    nav = pdna_gbsummary(&edited, can_edit, false, note, false, false, &saved, 0);
    pdna_summary_quiet_save(false);
    if (saved) {
      GbEditMon original;
      GbsStatus wst = gbd_withdraw(s, slot, &original);
      if (wst != GBS_OK) {
        snd_error(); msg_wait("DAY CARE", UI_WARN, gbs_status_text(wst), 0);
      } else {
        GbsStatus dst = gbd_deposit(s, slot, &edited);
        if (dst != GBS_OK) {
          gb_rollback();
          snd_error();
          msg_wait("DAY CARE", UI_WARN, gbs_status_text(dst), "Nothing was changed.");
        } else if (gb_hold_commit("daycare-edit")) {
          snd_ok();
          dc->slot[slot].mon = edited;
          dc->slot[slot].occupied = true;
        }
      }
    }
    if (nrows > 1 && nav != 0) { slot ^= 1; edited = dc->slot[slot].mon; gb_set_caught_available(&edited, gb_session_is_crystal(s)); }
  } while (nav != 0 && nrows > 1);
}

/* Build the boarder name line: "<NICK> (<SPECIES>) Lv.N is boarding." or shortened variant.
 * Used by both Gen-1 and Gen-2 n==1 branches for consistency (BACKLOG #121). */
static void gbdc_boarder_name(char out[48], const GbEditMon* mon, const GbDaycareSlot* slot) {
  const char* nick = slot->nick[0] ? slot->nick : pk_species_name(gb_get_species_dex(mon));
  const char* species = pk_species_name(gb_get_species_dex(mon));
  uint8_t level = gb_get_level(mon);

  /* #400: no "NAME (NAME)" repeat when the nickname is just the species (case-blind) */
  bool same = true;
  for (int i = 0; i < 12 && same; i++) {
    char a = nick[i], b = species[i];
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    if (a != b) same = false;
    if (a == 0 || b == 0) break;
  }
  siprintf(out, "%s (%s) Lv.%u is boarding.", nick, species, (unsigned)level);
  if (same || ui_ptext_w(out) > PDNA_DCY_NAME_W) {
    siprintf(out, "%s Lv.%u is boarding.", nick, (unsigned)level);
  }
}

/* The status/compatibility panel -- SAME geometry pdna_daycare() uses (PDNA_DCY_*,
 * pdna_layout.h), Gen 1/2's own flag-based read swapped in for Gen 3's on-the-fly
 * pk_daycare_compat() calculation (gb_daycare.h's `compatible`/`egg_ready` are what
 * the game itself already computed, not this tree's guess). */
static void gbdc_panel(const GbDaycare* dc, int n, bool visitors_ok, int n_visitors) {
  const int dcy0 = PDNA_DCY_ROW0_Y, dcyp = PDNA_DCY_ROW_PITCH, dcx = PDNA_DCY_TEXT_X;
  ui_panel(PDNA_DCY_PANEL_X, PDNA_DCY_PANEL_Y, PDNA_DCY_PANEL_W, PDNA_DCY_PANEL_H, UI_PANEL, UI_BORDER);
  if (dc->gen1) {
    if (n) {
      char l[48];
      gbdc_boarder_name(l, &dc->slot[0].mon, &dc->slot[0]);
      ui_ptext(dcx, dcy0, UI_DIM, l);
    } else {
      ui_ptext(dcx, dcy0, UI_DIM, "No Pokemon are boarding.");
    }
    ui_ptext(dcx, dcy0 + dcyp, UI_DIM, "Gen 1 Day Care has no breeding.");
  } else if (n == 2) {
    ui_ptext(dcx, dcy0, dc->compatible ? UI_OK : UI_DIM,
             dc->compatible ? "They get along very well!" : "They'd rather be elsewhere.");
    if (dc->has_egg) {
      ui_ptext(dcx, dcy0 + dcyp, UI_OK, "An EGG is ready to collect!");
    } else if (!dc->compatible) {
      ui_ptext(dcx, dcy0 + dcyp, UI_DIM, "(no Egg: incompatible pair)");   /* the game only counts down while compatible */
    } else {
      char l[48]; siprintf(l, "Steps to next egg check: %u", (unsigned)dc->steps_to_egg);
      ui_ptext(dcx, dcy0 + dcyp, UI_DIM, l);
    }
  } else if (n == 1) {
    char l[48];
    const GbDaycareSlot* bs = dc->slot[0].occupied ? &dc->slot[0] : &dc->slot[1];   /* #400: only the Lady's slot may be used */
    gbdc_boarder_name(l, &bs->mon, bs);
    ui_ptext(dcx, dcy0, UI_DIM, l);
    if (dc->has_egg) ui_ptext(dcx, dcy0 + dcyp, UI_OK, "An EGG is ready to collect!");
  } else {
    ui_ptext(dcx, dcy0, dc->has_egg ? UI_OK : UI_DIM,
             dc->has_egg ? "An EGG is ready to collect!" : "No Pokemon are boarding.");
  }
  /* Third row -- unconditional, same shape as Gen 3's own pk_daycare_yard_note()
   * row (pdna_main.c's pdna_daycare()): says whether the mons walking the yard
   * are real boarders or invented scenery, or WHY there are none (BACKLOG #114
   * step 3's own measured wording -- a GB-only setup with no registered Gen-3
   * ROM has no icon art to draw visitors with, same gate as Gen 3's own
   * app_yard_visitors_ok(), so it says so instead of silently showing none). The wording
   * names the failing predicate: gb_daycare.h's gbd_visitors_note() (BACKLOG #298). */
  if (!visitors_ok) {
    /* #298: name WHICH of the three predicates failed, not one register-a-ROM line for all */
    const char* why = gbd_visitors_note(app_any_rom_registered(), app_rom_art_off(),
                                        app_yard_visitors_setting());
    ui_ptext(dcx, dcy0 + 2 * dcyp, UI_DIM, why ? why : "No visitors this time.");
  }
  else if (n_visitors > 0) ui_ptext(dcx, dcy0 + 2 * dcyp, UI_DIM, "Others are just visiting.");
  else ui_ptext(dcx, dcy0 + 2 * dcyp, UI_DIM, "No extra visitors this time.");
}

/* Where a real boarder icon was placed by pdna_yard_place(): up to 2 entries,
 * `phys[i]` names WHICH physical slot (0 = Man/Gen-1 Boarder, 1 = Lady) `x[i]`/
 * `y[i]`/`sp[i]` belong to -- the same shape dc_rescan's own dcx/dcy/phys triple
 * has for Gen 3, needed here because a GB slot can be EMPTY (no icon at all,
 * unlike Gen 3 where dc[]/dcx[]/dcy[] only ever hold occupied entries). */
typedef struct {
  uint16_t sp[2];
  int      x[2], y[2], phys[2];
  int      n;
} GbdcBoard;

/* BACKLOG #114: the yard scene (dc_scene/dc_icon_over_bg/dc_pointer, pdna_yard.h)
 * replaces the old two/three text rows -- same shape pdna_daycare() uses on
 * Gen 3 (source/pdna_main.c), same PDNA_DCY_* geometry underneath the panel.
 * A SELECTED slot that is EMPTY has no icon to point at (a GB-only concept:
 * Gen 3's own dc[]/recs[] never hold an empty entry to begin with, so it never
 * needs this case) -- named in the footer instead of a yard pointer. */
static void gbdc_paint(const GbDaycare* dc, int sel, const GbdcBoard* board,
                       bool visitors_ok, int frame) {
  ui_clear();
  ui_fill_rect(0, 0, UI_SCR_W, 11, UI_BG);
  ui_text(4, 2, UI_TITLE, "DAY CARE");
  int n = (dc->slot[0].occupied ? 1 : 0) + (!dc->gen1 && dc->slot[1].occupied ? 1 : 0);
  char sl[16]; siprintf(sl, "Boarding %d/%d", n, dc->gen1 ? 1 : 2);
  ui_ptext_right(236, 2, UI_DIM, sl);
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);

  dc_scene();   /* the yard background, screen y 12..PDNA_DCY_PANEL_Y (122) */

  /* visitors first + hazed, so a piece of scenery never paints over a real
   * boarder -- same order pdna_daycare() uses. */
  for (int i = 0; i < s_ndeco; i++)
    dc_icon_over_bg(s_deco_x[i], s_deco_y[i],
                    mon_icon_for_form_frame(s_deco_sp[i], 0, (uint8_t)(frame & 1)), 6, false);
  for (int i = 0; i < board->n; i++)
    dc_icon_over_bg(board->x[i], board->y[i],
                    mon_icon_for_form_frame(board->sp[i], 0, (uint8_t)(frame & 1)), 8, false);

  bool sel_has_icon = false;
  for (int i = 0; i < board->n; i++) {
    if (board->phys[i] == sel) {
      int py = board->y[i] - 7; if (py < 12) py = 12;
      dc_pointer(board->x[i] + 16, py);
      sel_has_icon = true;
      break;
    }
  }

  gbdc_panel(dc, n, visitors_ok, s_ndeco);
  ui_fill_rect(0, PDNA_DCY_FOOTER_Y, UI_SCR_W, 8, UI_BG);
  int nslots = dc->gen1 ? 1 : 2;
  if (!sel_has_icon && sel < nslots) {
    /* selected slot is empty -- no icon to point at; name it instead */
    char line[40];
    siprintf(line, "%s (empty)  A menu  B back", dc->gen1 ? "Boarder" : (sel == 0 ? "Man" : "Lady"));
    ui_ptext(4, PDNA_DCY_FOOTER_Y, UI_DIM, line);
  } else if (!dc->gen1 && dc->has_egg && sel == 2) {
    ui_ptext(4, PDNA_DCY_FOOTER_Y, UI_OK, "Egg ready  A take  B back");
  } else {
    ui_ptext(4, PDNA_DCY_FOOTER_Y, UI_DIM, "A menu  U/D move  B back");
  }
}

/* Idle-bob tick: re-composite every icon (visitors + boarders) + the pointer at
 * the NEW frame, CPU-copy transport (tick=true) -- fd205bb's fix, never DMA on a
 * per-vblank tick. Same shape as pdna_daycare()'s own inline bob block. */
static void gbdc_bob(const GbdcBoard* board, int sel, int frame) {
  for (int i = 0; i < s_ndeco; i++)
    dc_icon_over_bg(s_deco_x[i], s_deco_y[i],
                    mon_icon_for_form_frame(s_deco_sp[i], 0, (uint8_t)(frame & 1)), 6, true);
  for (int i = 0; i < board->n; i++)
    dc_icon_over_bg(board->x[i], board->y[i],
                    mon_icon_for_form_frame(board->sp[i], 0, (uint8_t)(frame & 1)), 8, true);
  for (int i = 0; i < board->n; i++) {
    if (board->phys[i] == sel) {
      int py = board->y[i] - 7; if (py < 12) py = 12;
      dc_pointer(board->x[i] + 16, py);
      break;
    }
  }
}

/* Roll (once per screen entry) + place (every repaint, since a put/take/edit can
 * change occupancy) the yard's boarders + visitors. `rolled`/`visit_rng` are the
 * caller's own locals, persisted across loop iterations within ONE call to
 * pdna_gbdaycare() -- this screen has no long-lived visit state of its own
 * (unlike Gen 3's s_dc_visit_rng), it re-derives everything fresh on entry, same
 * as every other value this screen reads on each gbd_read(). */
static void gbdc_roll_and_place(const GbDaycare* dc, bool* rolled, uint32_t* visit_rng,
                                bool visitors_ok, GbdcBoard* board) {
  if (!*rolled) {
    *rolled = true;
    uint16_t max_dex = dc->gen1 ? 151 : 251;   /* BACKLOG #114: Gen 1 = Kanto only, Gen 2 = both */
    if (visitors_ok) pdna_yard_roll(max_dex); else dc_visitors_off();
    *visit_rng = app_session_seed();
  }
  board->n = 0;
  int nslots = dc->gen1 ? 1 : 2;
  uint16_t board_sp[2]; int board_phys[2];
  for (int i = 0; i < nslots; i++) {
    if (!dc->slot[i].occupied) continue;
    uint16_t dex = gb_get_species_dex(&dc->slot[i].mon);
    board_sp[board->n] = (dex >= 1 && dex <= 251) ? dex : 1;   /* clamp: a bad dex never indexes an icon out of range */
    board_phys[board->n] = i;
    board->n++;
  }
  int bx[2], by[2];
  pdna_yard_place(*visit_rng, board_sp, board->n, bx, by);
  for (int i = 0; i < board->n; i++) {
    board->sp[i] = board_sp[i]; board->phys[i] = board_phys[i];
    board->x[i] = bx[i]; board->y[i] = by[i];
  }
}

void pdna_gbdaycare(GbSession* s, int cur_box, bool can_edit) {
  if (!s || !s->open) { snd_deny(); msg_wait("DAY CARE", UI_WARN, "No save is open.", 0); return; }

  /* GbDaycare (616 B) and TWO GBS_LIST_BYTES (1152 B) staging buffers all live in the
   * GB12 arena tail -- never a static (this screen may be entered on an emulator/
   * hardware run where the same struct would otherwise sit in EWRAM for the rest of
   * the app's life) and never a stack local (this call chain is already several frames
   * deep off gb_nav_from_start). One slice, one call, carved into three sub-regions --
   * same idiom pdna_gbbag.c's own `bag`/`t0` pair uses. The second list buffer is
   * D6's own need: gbdc_take()'s party-landing leg composes gbs_insert() + gbs_move(),
   * and gbs_move() takes two INDEPENDENT staging buffers (src_list/dst_list,
   * gb_session.h), never the same one twice. */
  uint32_t need = GBDC_A4(sizeof(GbDaycare)) + GBS_LIST_BYTES + GBS_LIST_BYTES;
  uint8_t* tail = gb12_arena_tail(need);
  if (!tail) { snd_deny(); msg_wait("DAY CARE", UI_WARN, "Not enough memory right now.", 0); return; }
  GbDaycare* dc = (GbDaycare*)tail;
  uint8_t* list  = tail + GBDC_A4(sizeof(GbDaycare));
  uint8_t* list2 = list + GBS_LIST_BYTES;

  bool visitors_ok = app_yard_visitors_ok();   /* checked once per visit, same as pdna_daycare() */
  bool rolled = false;
  uint32_t visit_rng = 1;
  GbdcBoard board; memset(&board, 0, sizeof board);
  int frame = 0, ctr = 0;

  int sel = 0;
  bool redraw = true;
  for (;;) {
    if (!gbd_read(s, dc)) {
      snd_error(); msg_wait("DAY CARE", UI_WARN, "Could not read this save.", 0);
      break;
    }
    int nrows = dc->gen1 ? 1 : (dc->has_egg ? 3 : 2);
    if (sel >= nrows) sel = nrows - 1;

    /* Roll (once) + place (every iteration -- a put/take/edit can change which
     * physical slots are occupied) the yard's boarders + visitors. */
    gbdc_roll_and_place(dc, &rolled, &visit_rng, visitors_ok, &board);

    if (redraw) { redraw = false; gbdc_paint(dc, sel, &board, visitors_ok, frame); }

    /* Icon-rent bracket (BACKLOG #114 step 3, same shape as pdna_daycare()'s own
     * comment): declared + rented before the idle-bob wait begins, given back the
     * INSTANT a key is detected, before dispatching an action that might reach a
     * persist (gbdc_take/gbdc_deposit/gbdc_view_edit/gbdc_take_egg all can). */
    {
      uint16_t irows[7]; int nr = 0;
      for (int i = 0; i < s_ndeco && nr < 7; i++) irows[nr++] = app_icon_row_of(s_deco_sp[i], 0, false);
      for (int i = 0; i < board.n && nr < 7; i++)  irows[nr++] = app_icon_row_of(board.sp[i], 0, false);
      app_icons_hold(irows, nr);
    }

    u16 k, fresh;
    do {
      s_vsync();
      bool anim_any = (board.n > 0) || (s_ndeco > 0);
      if (app_anim_enabled(ANIM_DAYCARE) && anim_any && mon_icon_anim_cheap() && ++ctr >= 30) {
        ctr = 0; frame ^= 1;
        gbdc_bob(&board, sel, frame);
      }
      fresh = key_hit(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
      k = fresh;
    } while (!k);
    app_icons_drop();   /* idle loop over -- see the comment above app_icons_hold() */

    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & KEY_A) snd_ok();
    else if (fresh & KEY_B) snd_back();

    if (k & KEY_B) break;
    else if (k & KEY_UP)   { sel = (sel > 0) ? sel - 1 : nrows - 1; redraw = true; }
    else if (k & KEY_DOWN) { sel = (sel + 1) % nrows; redraw = true; }
    else if (k & KEY_A) {
      if (!dc->gen1 && dc->has_egg && sel == 2) {
        if (!can_edit) { snd_deny(); msg_wait("EGG", UI_WARN, app_gb_readonly_why(), 0); }
        else gbdc_take_egg(s, list, list2);
      } else {
        int slot = sel;   /* 0 or 1 */
        bool occ = dc->slot[slot].occupied;
        bool can_put_here = can_edit && !occ;
        bool can_take_here = can_edit && occ;
        if (!occ && !can_put_here) {
          /* Read-only cart, empty slot: nothing this menu could offer at all --
           * say so directly instead of opening a popup with only Cancel in it. */
          snd_deny();
          msg_wait("DAY CARE", UI_WARN, app_gb_readonly_why(), 0);
        } else {
          int act = gbdc_menu(occ, can_put_here, can_take_here);
          if (act == 0)      gbdc_view_edit(s, dc, slot, can_edit);
          else if (act == 1) gbdc_take(s, dc, slot, list, list2);
          else if (act == 2) gbdc_deposit(s, slot, cur_box, list);
        }
      }
      redraw = true;
    }
  }
  gb12_arena_tail_release();
}
