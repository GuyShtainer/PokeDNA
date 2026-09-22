/*
 * Gen 1/2's own Hall of Fame screen -- BACKLOG #89. See pdna_gbhof.h for the full
 * design rationale (no Gen-3 twin, why the "Records" nav row hosts it, the L/R
 * fast-jump / list-detail shape). This file only ever draws plain rows (no GB-shell
 * art), same posture as pdna_gbclock.c/pdna_battle_record().
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbhof.h"
#include "gb_hof.h"
#include "gb_edit.h"        /* GB_GEN1/GB_GEN2, gb_max_species                       */
#include "pdna_gen12.h"     /* gb_persist / gb_rollback -- the verified-write path    */
#include "pdna_trainer.h"   /* trainer_row_paint / trainer_key_legend                 */
#include "data_tables.h"    /* pk_species_name                                        */
#include "pdna_pick.h"      /* pick_species / pick_species_set_max_dex (F2/F3)        */
#include "osk.h"            /* osk_input (F2/F3 nickname)                             */
#include "pdna_layout.h"    /* PDNA_GBEDIT_KEEP_TITLE (D4 discard-confirm)            */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"       /* msg_wait / app_confirm / app_session_seed              */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

/* Same shape as every sibling GB screen's own s_wait (pdna_gbclock.c etc.): UP/DOWN
 * auto-repeat, L/R/A/B/START do not (matching pdna_battle_record()'s own L/R paging,
 * which is deliberately NOT in wait_keys' repeat mask either). */
static u16 s_wait(u16 mask) {
  u16 k, fresh;
  do {
    s_vsync();
    fresh = key_hit(mask);
    k = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN));
  } while (!k);
  if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
  else if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  return k;
}

#define ROWS_PER_PAGE 8
#define ROW_Y0        34
#define ROW_DY        13

/* ---- LIST -------------------------------------------------------------------- */

static void hof_list_row_paint(const GbSession* s, int idx, int y, bool sel) {
  GbHofTeam t;
  char label[32];
  if (gbh_team(s, idx, &t) && t.n > 0) {
    int lo = 100, hi = 0;
    for (int m = 0; m < t.n; m++) {
      if (t.mon[m].level < lo) lo = t.mon[m].level;
      if (t.mon[m].level > hi) hi = t.mon[m].level;
    }
    siprintf(label, "#%-3d %d mon  Lv%d-%d", idx + 1, t.n, lo, hi);
  } else {
    siprintf(label, "#%-3d --", idx + 1);
  }
  trainer_row_paint(y, sel, label, "", UI_TEXT);
}

typedef struct { int sel; int top; bool valid; uint32_t gen; } HofListPaint;

static void hof_list_render(const GbSession* s, int present, int count, bool can_edit,
                            int sel, HofListPaint* pv) {
  int top = (sel / ROWS_PER_PAGE) * ROWS_PER_PAGE;
  bool full = !pv->valid || pv->gen != ui_clear_gen() || top != pv->top;

  if (full) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "HALL OF FAME");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char hdr[40];
    siprintf(hdr, "%d teams (lifetime count %d)", present, count);
    ui_text(6, 20, UI_DIM, hdr);

    if (present == 0) {
      ui_text(6, 60, UI_DIM, "No teams recorded yet.");
    } else {
      int shown = present - top;
      if (shown > ROWS_PER_PAGE) shown = ROWS_PER_PAGE;
      for (int i = 0; i < shown; i++)
        hof_list_row_paint(s, top + i, ROW_Y0 + i * ROW_DY, (top + i) == sel);
    }
    if (!can_edit) {
      /* D3: rows 0..7 fill Y0..Y0+7*DY = 34..125 (each row's text runs to ~132) --
       * the old y=112/122 pair painted straight over rows 7-8. Sit below the last
       * row and above the hline at 151 instead. Same two-line shape/column
       * pdna_gbclock.c's own read-only branch uses otherwise. */
      ui_text(6, 134, UI_DIM, app_gb_readonly_why());
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    trainer_key_legend(can_edit ? "U/D sel A view L/R pg ST menu"
                                : "U/D sel A view L/R pg B back");
  } else if (sel != pv->sel) {
    hof_list_row_paint(s, pv->sel, ROW_Y0 + (pv->sel - top) * ROW_DY, false);
    hof_list_row_paint(s, sel,     ROW_Y0 + (sel     - top) * ROW_DY, true);
  }
  pv->sel = sel; pv->top = top; pv->gen = ui_clear_gen(); pv->valid = true;
}

/* ---- DETAIL -------------------------------------------------------------------- */

/* `mon_sel` (-1 = none, e.g. read-only) highlights one mon row with a leading '>'
 * marker (the same idiom trainer_row_paint's own sel argument implies, but this
 * screen draws its own multi-line rows directly rather than through that helper --
 * BACKLOG #194 F2). */
static void hof_detail_render(const GbHofTeam* t, uint8_t gen, int team_no,
                              bool can_edit, int mon_sel) {
  ui_clear();
  char title[24];
  siprintf(title, "TEAM #%d", team_no);
  ui_text(4, 4, UI_TITLE, title);
  ui_hline(0, 14, UI_SCR_W, UI_BORDER);

  int y0 = 22;
  if (gen == GB_GEN2) {
    char wc[24];
    siprintf(wc, "Wins so far: %d", (int)t->win_count);
    ui_text(6, 20, UI_DIM, wc);
    y0 = 34;
  }

  for (int m = 0; m < GBH_NUM_MONS; m++) {
    int y = y0 + m * 16;
    if (m >= t->n) continue;
    const GbHofMon* mn = &t->mon[m];
    const char* nm = (mn->dex >= 1 && mn->dex <= 251) ? pk_species_name(mn->dex) : "?";
    /* NICK: gb_hof.c decodes mn->nick on both gens but nothing drew it -- show it
     * only when it differs from the species name (a lot of teams never get
     * nicknamed, so "PIKACHU PIKACHU Lv100" would just be noise). Measured to
     * sys8Font's fixed 8 px cell (ui.c) against a 29-char (232 px, x=6..238)
     * budget, dropped to a shortened %.*s rather than overflowing off-screen. R4
     * (b89 re-verify): no quotes around the nickname and no padding on the species
     * -- a bare "SPECIES NICK Lv.N" separated by one space, not "SPECIES \"NICK\"
     * Lv.N" -- buys the nickname 2 extra characters of budget (the two quote
     * glyphs) versus the quoted form; a 10-char species (TYPHLOSION) plus a full
     * 10-char nickname WITHOUT a shiny mark now fits whole (29-10-1-7=11 >= 10),
     * though the worst case (shiny too, suffix=" Lv.100 *") still trims the last
     * character or so (29-10-1-9=9 of 10) -- see this caption's own honesty about
     * that in tools/dgb_shots.py's b89_*_08_nick shot. */
    bool has_nick = mn->nick[0] != '\0' && strcmp(mn->nick, nm) != 0;
    char line[40];
    if (has_nick) {
      char suffix[12];  /* " Lv.100 *" is 9 chars + NUL; [8] overflowed at Lv >= 10 (R2) */
      siprintf(suffix, " Lv.%d%s", mn->level, (gen == GB_GEN2 && mn->shiny) ? " *" : "");
      int budget = 29 - (int)strlen(nm) - 1 /* one separating space, no quotes/padding */
                   - (int)strlen(suffix);
      if (budget < 1) budget = 1;
      siprintf(line, "%s %.*s%s", nm, budget, mn->nick, suffix);
    } else if (gen == GB_GEN2)
      siprintf(line, "%-10s Lv%-3d%s", nm, mn->level, mn->shiny ? " *" : "");
    else
      siprintf(line, "%-10s Lv%-3d", nm, mn->level);
    if (can_edit && m == mon_sel) ui_text(0, y, UI_TITLE, ">");
    ui_text(6, y, UI_TEXT, line);
    if (gen == GB_GEN2) {
      char ot[16];
      siprintf(ot, "OT %u", (unsigned)mn->otid);
      /* sys8Font is a fixed 8x8 face (ui.c) -- strlen*8 is the line's real drawn
       * width. When a nicknamed line runs past the OT column (x=168), drop OT to a
       * second sub-line instead of overlapping it: each row already has a full
       * 16 px of vertical room (ROW_DY) for one 8 px line of text, so y+8 fits
       * without touching any other row's layout. */
      int line_w = (int)strlen(line) * 8;
      if (6 + line_w > 168)
        ui_text(6, y + 8, UI_DIM, ot);
      else
        ui_text(168, y, UI_DIM, ot);
    }
  }
  ui_hline(0, 151, UI_SCR_W, UI_BORDER);
  trainer_key_legend(can_edit ? "U/D sel A edit B back" : "B back");
}

/* ---- F2: edit one mon of a team (species/level/nickname) ----------------------- */

enum { HOFEDIT_SPECIES = 0, HOFEDIT_LEVEL, HOFEDIT_NICK, HOFEDIT_DONE, HOFEDIT_N };
static const char* const kHofEditLbl[HOFEDIT_N] = { "SPECIES", "LEVEL", "NICKNAME", "DONE" };

/* Same shape as hof_set_count_editor's own stepper (pdna_gbhof.c above): U/D +-1,
 * L/R +-10 (brief's own "L/R x10" ask), A confirms THIS field, B cancels it (the
 * level stays whatever it was before this call). s_wait's own repeat mask covers
 * only KEY_UP|KEY_DOWN (this file's s_wait, not the global key_repeat_mask), so
 * L/R here deliberately do not auto-repeat -- matching every other L/R page/step
 * key in this screen.
 *
 * D4 (b194 review): returns bool, same contract as hof_species_editor()/
 * hof_nick_editor() -- true only on A (the level actually changed), false on B
 * (cancelled, *level untouched). Previously void, so hof_edit_mon_menu()'s own
 * caller unconditionally set `dirty = true` even when the user cancelled out of
 * the stepper with no change at all -- DONE would then confirm+backup+write for
 * an edit that never happened. */
static bool hof_level_editor(uint8_t* level) {
  int v = *level;
  bool valid = false; int pv = -1; uint32_t g = 0;
  for (;;) {
    bool full = !valid || g != ui_clear_gen();
    char b[24];
    siprintf(b, "Level: %d", v);
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "LEVEL");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(10, 60, UI_TEXT, b);
      ui_text(4, 152, UI_DIM, "U/D +-1  L/R +-10  A set  B cancel");
    } else if (v != pv) {
      ui_fill_rect(8, 52, 200, 16, UI_BG);
      ui_text(10, 60, UI_TEXT, b);
    }
    pv = v; valid = true; g = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return false;                  /* cancel: *level untouched */
    if (k & KEY_A) { *level = (uint8_t)v; return true; }
    if (k & (KEY_UP | KEY_DOWN)) v += (k & KEY_UP) ? 1 : -1;
    else if (k & (KEY_LEFT | KEY_RIGHT)) v += (k & KEY_RIGHT) ? 10 : -10;
    if (v < 1) v = 1;
    if (v > 100) v = 100;
  }
}

/* Species: the SAME icon-grid picker + generation ceiling CREATE uses (pdna_gen12.c
 * gb_create_hook, BACKLOG #50 UX-parity) -- pick_species_set_max_dex() is a
 * file-static that must be cleared right after, or it leaks into the next,
 * unrelated pick_species() caller (pdna_pick.h's own contract). Cancel (0xFFFF or
 * 0) leaves *dex untouched and returns false. */
static bool hof_species_editor(uint8_t gen, uint16_t* dex) {
  pick_species_set_max_dex(gb_max_species(gen));
  uint16_t d = pick_species(*dex);
  pick_species_set_max_dex(0);
  if (d == 0xFFFFu || d == 0) return false;
  *dex = d;
  return true;
}

/* Nickname: the same osk_input() the party editor's GBE_K_TEXT branch uses
 * (pdna_gbedit.c). osk_input rejects an empty result on its own (osk.h's own
 * contract), so this only needs to copy a non-empty result back. */
static bool hof_nick_editor(char* nick, int cap) {
  char out[GBH_NICK_CAP];
  if (!osk_input("NICKNAME", nick, out, sizeof out)) return false;
  int n = cap - 1;
  if (n > (int)sizeof out - 1) n = (int)sizeof out - 1;
  int i = 0;
  for (; i < n && out[i]; i++) nick[i] = out[i];
  nick[i] = 0;
  return true;
}

/* One mon's edit menu: SPECIES/LEVEL/NICKNAME/DONE, each field staged directly into
 * `staged` (the caller's own copy of the mon, never the live session). Returns true
 * iff DONE was chosen -- exactly like pdna_gbtrainer's own "B always discards, the
 * ONE call site above decides whether to commit" contract; the caller still runs
 * its own confirm + gbh_set_mon + gb_persist, this function only stages fields. */
static bool hof_edit_mon_menu(uint8_t gen, GbHofMon* staged) {
  int sel = 0;
  bool dirty = false;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "EDIT MON");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char l1[32];
    const char* nm = (staged->dex >= 1 && staged->dex <= 251) ? pk_species_name(staged->dex) : "?";
    siprintf(l1, "%s Lv%d", nm, staged->level);
    ui_text(6, 20, UI_DIM, l1);
    for (int i = 0; i < HOFEDIT_N; i++)
      trainer_row_paint(40 + i * 16, i == sel, kHofEditLbl[i], "", UI_TEXT);
    ui_text(4, 152, UI_DIM, "U/D select  A choose  B cancel");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) {
      /* D4 (b194 review): B with nothing staged discards silently (there is
       * nothing to lose) -- same as before. B with `dirty` true now confirms
       * first, the editor's own KEEP idiom (PDNA_GBEDIT_KEEP_TITLE, matching
       * gbedit_confirm_keep()'s own title elsewhere) -- declining (B on the
       * confirm) returns to the menu with every staged field intact instead
       * of silently throwing a real edit away. */
      if (dirty && !app_confirm(PDNA_GBHOF_DISCARD_TITLE,
                                "Your changes to this mon will be lost."))
        continue;
      return false;
    }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : HOFEDIT_N - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % HOFEDIT_N;
    else if (k & KEY_A) {
      if (sel == HOFEDIT_SPECIES) { if (hof_species_editor(gen, &staged->dex)) dirty = true; }
      else if (sel == HOFEDIT_LEVEL) { if (hof_level_editor(&staged->level)) dirty = true; }
      else if (sel == HOFEDIT_NICK) { if (hof_nick_editor(staged->nick, GBH_NICK_CAP)) dirty = true; }
      else return dirty;   /* DONE: commit only if SOMETHING actually changed */
    }
  }
}

/* The ONE call site that stages -> confirms -> writes -> persists an edited mon
 * (mirrors hof_do_clear/hof_set_count_editor's own shape, and pdna_gbtrainer's
 * "commit lives at one call site" rule). `team_idx` is the UI (newest-first) team
 * index this detail page is showing; `mon_idx` the selected mon row. */
static void hof_edit_mon(GbSession* s, int team_idx, int mon_idx, const GbHofMon* orig) {
  GbHofMon staged = *orig;
  if (!hof_edit_mon_menu(s->gen, &staged)) return;   /* B or DONE-with-no-change */

  /* Gen 2: a species change re-rolls DVs (and therefore shininess) exactly like
   * CREATE's own contract -- gb_hof.h's own doc comment on gbh_set_mon. OT id is
   * never rerolled (still the save's own trainer, untouched either way). */
  if (s->gen == GB_GEN2 && staged.dex != orig->dex) {
    uint8_t dv[4];
    gbh_roll_dv(app_session_seed() ^ (uint32_t)(team_idx * 97 + mon_idx), dv);
    memcpy(staged.dv, dv, sizeof dv);
  }

  char l1[40];
  siprintf(l1, "Save changes to slot %d?", mon_idx + 1);
  if (!app_confirm("EDIT ENTRY", l1)) return;

  GbsStatus st = gbh_set_mon(s, team_idx, mon_idx, &staged);
  if (st != GBS_OK) {
    gb_rollback();
    msg_wait("REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
    return;
  }
  gb_persist("hof edit");
}

/* ---- START menu: CLEAR ALL / SET COUNT ------------------------------------------ */

/* CLEAR ALL: gbh_clear's own contract (gb_hof.h) is "on ANY non-GBS_OK, some teams may
 * already be zeroed in the image" -- gb_rollback() restores the WHOLE image from the
 * pristine copy every GB edit screen already keeps, discarding every chunk this call
 * wrote, good or bad. Same shape gbclock_do_reset/do_clear use for their own refusal. */
static void hof_do_clear(GbSession* s) {
  /* D4: an already-empty HoF has nothing to clear -- skip the confirm dialog, the
   * SD write, and the backup slot it would burn, same as the SET COUNT editor's own
   * no-op-skips-the-write rule just above. b89 re-verify R1: ask gbh_slots_in_blob()
   * (the raw, unclamped scan), not gbh_team_count_present() -- on Gen 2 the clamped
   * view can read 0 while real teams still sit in the blob (LoadHOFTeam bails on
   * each record's own win-count byte, not the count field), which would silently
   * strand them uncleared. */
  if (gbh_count(s) == 0 && gbh_slots_in_blob(s) == 0) { snd_deny(); return; }
  if (!app_confirm("CLEAR ALL",
                   "The PC's HALL OF FAME option disappears until you win again."))
    return;
  GbsStatus st = gbh_clear(s);
  if (st != GBS_OK) {
    gb_rollback();
    msg_wait("REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
    return;
  }
  gb_persist("hof clear");
}

/* SET COUNT: a plain stepper, same shape as pdna_gbclock.c's shift editor but a
 * single non-negative, clamped value (Gen 1 <= teams present, up to GBH_G1_CAPACITY;
 * Gen 2 <=200 -- gbh_set_count's own clamp, D1; this editor mirrors it so the
 * displayed cap never lies about what a press past it will actually do). A no-op
 * (value unchanged) skips app_confirm AND the
 * write entirely, hard rule 3. */
__attribute__((noinline))
static void hof_set_count_editor(GbSession* s, uint8_t gen) {
  /* Mirrors gbh_set_count's own clamp exactly (D1, R1): Gen 1's real League PC
   * decodes every slot up to the stored count with no independent bounds check, so
   * the displayed cap must never promise more than the teams actually present --
   * and that ceiling has to be gbh_slots_in_blob() (the raw, unclamped scan), never
   * gbh_team_count_present() (which clamps to the CURRENT count on Gen 1, D2): using
   * the clamped view here would make the displayed cap ratchet down with every
   * commit and never recover. */
  int raw = gbh_slots_in_blob(s);
  int cap = (gen == GB_GEN2) ? 200 : ((raw < GBH_G1_CAPACITY) ? raw : 255);
  int start = gbh_count(s);
  int v = start;
  bool valid = false; int pv = -1; uint32_t g = 0;
  key_repeat_mask(KEY_UP | KEY_DOWN);
  for (;;) {
    bool full = !valid || g != ui_clear_gen();
    char b[28];
    siprintf(b, "Lifetime wins: %d / %d", v, cap);
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "SET COUNT");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(10, 60, UI_TEXT, b);
      ui_text(4, 152, UI_DIM, "U/D value  A set  B cancel");
    } else if (v != pv) {
      ui_fill_rect(8, 52, 200, 16, UI_BG);
      ui_text(10, 60, UI_TEXT, b);
    }
    pv = v; valid = true; g = ui_clear_gen();

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) break;
    else if (k & (KEY_UP | KEY_DOWN)) {
      int dir = (k & KEY_UP) ? 1 : -1;
      v += dir;
      if (v < 0) v = 0;
      if (v > cap) v = cap;
    } else if (k & KEY_A) {
      if (v == start) { snd_back(); break; }   /* no-op: nothing to confirm or write */
      char l1[40];
      siprintf(l1, "Set lifetime count to %d?", v);
      if (app_confirm("SET COUNT", l1)) {
        GbsStatus st = gbh_set_count(s, v);
        if (st != GBS_OK) {
          gb_rollback();
          msg_wait("REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
        } else {
          gb_persist("hof setcount");
        }
      }
      break;
    }
  }
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);   /* restore the global set */
}

/* ---- F3: ADD TEAM (build 1-6 mons with the same pickers) + DELETE TEAM --------- */

static void hof_add_team_row_paint(const GbHofTeam* t) {
  for (int i = 0; i < t->n; i++) {
    char l[32];
    const char* nm = (t->mon[i].dex >= 1 && t->mon[i].dex <= 251)
                     ? pk_species_name(t->mon[i].dex) : "?";
    siprintf(l, "%d. %s Lv%d", i + 1, nm, t->mon[i].level);
    ui_text(6, 20 + i * 10, UI_TEXT, l);
  }
}

/* Builds ONE new mon via the same three pickers hof_edit_mon_menu uses (species,
 * level, nickname), defaulting level 5 and nickname = the species name (CREATE's
 * own default, pdna_gen12.c gb_create_hook -- uppercase already, gb_new_mon.c's own
 * uppercase_ascii comment). Cancelling the SPECIES step (the only one that can
 * genuinely mean "changed my mind") adds nothing; cancelling level/nickname simply
 * keeps their defaults. Gen 2 DVs are rolled fresh here -- there is no "existing"
 * mon to preserve them from, unlike an edit. Returns false (nothing added) only on
 * a species cancel. */
static bool hof_build_one_mon(uint8_t gen, int slot_for_seed, GbHofMon* out) {
  memset(out, 0, sizeof *out);
  out->dex = 1;
  if (!hof_species_editor(gen, &out->dex)) return false;
  out->level = 5;
  hof_level_editor(&out->level);
  const char* nm = pk_species_name(out->dex);
  int i = 0;
  for (; i < GBH_NICK_CAP - 1 && nm && nm[i]; i++) out->nick[i] = nm[i];
  out->nick[i] = 0;
  (void)hof_nick_editor(out->nick, GBH_NICK_CAP);   /* optional rename */
  if (gen == GB_GEN2) {
    uint8_t dv[4];
    gbh_roll_dv(app_session_seed() ^ (uint32_t)(slot_for_seed * 131 + 7), dv);
    memcpy(out->dv, dv, sizeof dv);
  }
  out->present = true;
  return true;
}

static void hof_do_add_team(GbSession* s) {
  GbHofTeam t; memset(&t, 0, sizeof t);
  int sel = 0;
  for (;;) {
    const char* items[2]; int nitems = 0, add_idx = -1, done_idx = -1;
    if (t.n < GBH_NUM_MONS) { add_idx = nitems; items[nitems++] = "+ ADD MON"; }
    if (t.n >= 1)           { done_idx = nitems; items[nitems++] = "DONE"; }
    if (sel >= nitems) sel = nitems - 1;
    if (sel < 0) sel = 0;

    ui_clear();
    ui_text(4, 4, UI_TITLE, "ADD TEAM");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char hdr[24];
    siprintf(hdr, "%d/%d mons", t.n, GBH_NUM_MONS);
    ui_text(6, 18, UI_DIM, hdr);
    hof_add_team_row_paint(&t);
    ui_hline(0, 96, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < nitems; i++)
      trainer_row_paint(100 + i * 16, i == sel, items[i], "", UI_TEXT);
    ui_text(4, 152, UI_DIM, "U/D select  A choose  B cancel");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;   /* discard: nothing written, no matter how many staged */
    else if (k & KEY_UP)   sel = (nitems > 0) ? ((sel > 0) ? sel - 1 : nitems - 1) : 0;
    else if (k & KEY_DOWN) sel = (nitems > 0) ? (sel + 1) % nitems : 0;
    else if (k & KEY_A) {
      if (sel == add_idx) {
        GbHofMon m;
        if (hof_build_one_mon(s->gen, t.n, &m)) t.mon[t.n++] = m;
      } else if (sel == done_idx) {
        char l1[32];
        siprintf(l1, "Add this %d-mon team?", t.n);
        if (app_confirm("ADD TEAM", l1)) {
          GbsStatus st = gbh_append_team(s, &t);
          if (st != GBS_OK) {
            gb_rollback();
            msg_wait("REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
          } else {
            gb_persist("hof add");
          }
        }
        return;
      }
    }
  }
}

/* Inverse of add: delete the team currently highlighted on the LIST screen (the
 * `team_idx` the caller passed in when START was pressed) -- cheap and symmetric
 * (brief's own wording), no separate picker needed since the list cursor already
 * names the team. */
static void hof_do_delete_team(GbSession* s, int team_idx) {
  int present = gbh_team_count_present(s);
  if (team_idx < 0 || team_idx >= present) { snd_deny(); return; }
  char l1[32];
  siprintf(l1, "Delete team #%d?", team_idx + 1);
  if (!app_confirm("DELETE TEAM", l1)) return;
  GbsStatus st = gbh_delete_team(s, team_idx);
  if (st != GBS_OK) {
    gb_rollback();
    msg_wait("REFUSED", UI_WARN, gbs_status_text(st), "Nothing was changed.");
    return;
  }
  gb_persist("hof delete");
}

enum { HOFMENU_CLEAR = 0, HOFMENU_SETCOUNT, HOFMENU_ADD, HOFMENU_DELETE, HOFMENU_N };
static const char* const kHofMenuLbl[HOFMENU_N] =
  { "CLEAR ALL", "SET COUNT", "ADD TEAM", "DELETE TEAM" };

/* `team_sel` is the list screen's own current cursor position -- DELETE TEAM acts
 * on it directly (F3's own design: no second picker). */
static void hof_start_menu(GbSession* s, uint8_t gen, int team_sel) {
  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "HALL OF FAME MENU");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < HOFMENU_N; i++)
      trainer_row_paint(40 + i * 16, i == sel, kHofMenuLbl[i], "", UI_TEXT);
    ui_text(4, 152, UI_DIM, "U/D select  A choose  B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : HOFMENU_N - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % HOFMENU_N;
    else if (k & KEY_A) {
      if (sel == HOFMENU_CLEAR) hof_do_clear(s);
      else if (sel == HOFMENU_SETCOUNT) hof_set_count_editor(s, gen);
      else if (sel == HOFMENU_ADD) hof_do_add_team(s);
      else hof_do_delete_team(s, team_sel);
      return;   /* back to the list either way; it re-reads count/present fresh */
    }
  }
}

/* ---- entry ----------------------------------------------------------------------- */

void pdna_gbhof(GbSession* s, bool can_edit) {
  if (!s || !s->open) {
    msg_wait("HALL OF FAME", UI_WARN, "Could not read this save.", 0);
    return;
  }

  int sel = 0;
  bool in_detail = false;
  HofListPaint pv; memset(&pv, 0, sizeof pv);

  for (;;) {
    int present = gbh_team_count_present(s);
    int count = gbh_count(s);
    if (present == 0) sel = 0;
    else if (sel >= present) sel = present - 1;

    if (in_detail) {
      int mon_sel = 0;
      for (;;) {
        GbHofTeam t;
        if (!gbh_team(s, sel, &t)) break;
        if (mon_sel >= t.n) mon_sel = (t.n > 0) ? t.n - 1 : 0;
        hof_detail_render(&t, s->gen, sel + 1, can_edit, can_edit ? mon_sel : -1);
        u16 mask = can_edit ? (KEY_UP | KEY_DOWN | KEY_A | KEY_B) : KEY_B;
        u16 k = s_wait(mask);
        if (k & KEY_B) break;
        else if (k & KEY_UP)   mon_sel = (mon_sel > 0) ? mon_sel - 1 : t.n - 1;
        else if (k & KEY_DOWN) mon_sel = (mon_sel + 1) % t.n;
        else if (k & KEY_A)    hof_edit_mon(s, sel, mon_sel, &t.mon[mon_sel]);
        /* the top of the loop re-reads gbh_team(): a committed edit shows up
         * immediately, exactly like every other GB edit screen's own resume. */
      }
      in_detail = false;
      pv.valid = false;
      continue;
    }

    hof_list_render(s, present, count, can_edit, sel, &pv);
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (present > 0) ? ((sel > 0) ? sel - 1 : present - 1) : 0;
    else if (k & KEY_DOWN) sel = (present > 0) ? ((sel + 1) % present) : 0;
    else if (k & (KEY_LEFT | KEY_RIGHT)) {
      if (present > 0) {
        int dir = (k & KEY_LEFT) ? -1 : 1;
        sel += dir * ROWS_PER_PAGE;
        if (sel < 0) sel = 0;
        if (sel >= present) sel = present - 1;
      }
      pv.valid = false;
    } else if (k & KEY_A) {
      if (present > 0) { in_detail = true; pv.valid = false; }
      else { snd_deny(); }
    } else if (k & KEY_START) {
      if (!can_edit) { snd_deny(); continue; }
      hof_start_menu(s, s->gen, sel);
      pv.valid = false;
    }
  }
}
