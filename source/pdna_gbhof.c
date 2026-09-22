/*
 * Gen 1/2's own Hall of Fame screen -- BACKLOG #89. See pdna_gbhof.h for the full
 * design rationale (no Gen-3 twin, why the "Records" nav row hosts it, the L/R
 * fast-jump / list-detail shape). BACKLOG #202 (F1, this file's bottom half) adds
 * the shared gbscr card shell (list/detail/START menu drawn inside the frame with
 * the ROM's own font); hof_plain_screen()/hof_*_render() below are the ORIGINAL
 * plain-row screen, kept as the honest fallback when the shell refuses to open
 * (no ROM registered, bad ROM, non-English release, no tile-bank memory).
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
#include "pdna_gbscreen.h"  /* gbscr_open/_cell/_text/_flush/_close (F1 card shell)   */
#include "pdna_origin_art.h" /* A1: pdna_origin_art_icon / pdna_origin_cell_render     */
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

/* F2: the card cursor colour -- same value pdna_gbtrainer.c's own GBCARD_CSEL
 * uses for its m3_frame() field-cursor highlight (file-local #define there
 * too, so this is its own copy, not a shared symbol). */
#define GBCARD_CSEL RGB15(26, 4, 3)

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
                            int sel, HofListPaint* pv, bool art_off) {
  int top = (sel / ROWS_PER_PAGE) * ROWS_PER_PAGE;
  bool full = !pv->valid || pv->gen != ui_clear_gen() || top != pv->top;

  if (full) {
    ui_clear();
    /* F1 fallback marker (design sec 3.5's own honest-header rule): the shell
     * refused, so this plain screen -- unlike a real refusal-only screen --
     * must say so, sharing the title row (there is no other free row above
     * the list, ROW_Y0=34 already starts right after the header line below). */
    ui_text(4, 4, UI_TITLE, art_off ? "HALL OF FAME (GB ART: OFF)" : "HALL OF FAME");
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

/* One team's detail visit (view + F2 per-mon edit) -- extracted so BOTH the
 * plain list (hof_plain_screen) and the F1 card list (hof_card_screen further
 * below) drive the SAME code; there is no art-shell twin of the per-mon
 * field editors to keep in sync (matching the trainer card's own "sub-editors
 * are shared with the plain page" shape). */
static void hof_detail_visit(GbSession* s, int team_idx, bool can_edit) {
  int mon_sel = 0;
  for (;;) {
    GbHofTeam t;
    if (!gbh_team(s, team_idx, &t)) break;
    if (mon_sel >= t.n) mon_sel = (t.n > 0) ? t.n - 1 : 0;
    hof_detail_render(&t, s->gen, team_idx + 1, can_edit, can_edit ? mon_sel : -1);
    u16 mask = can_edit ? (KEY_UP | KEY_DOWN | KEY_A | KEY_B) : KEY_B;
    u16 k = s_wait(mask);
    if (k & KEY_B) break;
    else if (k & KEY_UP)   mon_sel = (mon_sel > 0) ? mon_sel - 1 : t.n - 1;
    else if (k & KEY_DOWN) mon_sel = (mon_sel + 1) % t.n;
    else if (k & KEY_A)    hof_edit_mon(s, team_idx, mon_sel, &t.mon[mon_sel]);
    /* the top of the loop re-reads gbh_team(): a committed edit shows up
     * immediately, exactly like every other GB edit screen's own resume. */
  }
}

/* Today's plain row list (unchanged behaviour) -- the F1 fallback, reached
 * when gbscr_open() refuses the card shell (no ROM registered, bad ROM,
 * non-English release, or no tile-bank memory -- pdna_gbtrainer's own list of
 * reasons). `art_off` prints the same honest "GB ART: OFF" header every other
 * GB-art screen's plain fallback shows, so a refusal never looks like the
 * finished design (design sec 3.5 / pdna_gbtrainer_plain's own D7 header
 * contract). */
static void hof_plain_screen(GbSession* s, bool can_edit, bool art_off) {
  int sel = 0;
  bool in_detail = false;
  HofListPaint pv; memset(&pv, 0, sizeof pv);

  for (;;) {
    int present = gbh_team_count_present(s);
    int count = gbh_count(s);
    if (present == 0) sel = 0;
    else if (sel >= present) sel = present - 1;

    if (in_detail) {
      hof_detail_visit(s, sel, can_edit);
      in_detail = false;
      pv.valid = false;
      continue;
    }

    hof_list_render(s, present, count, can_edit, sel, &pv, art_off);
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

/* ---- F1: the card look (BACKLOG #202) --------------------------------------------
 *
 * b194 (f93cf5a) built this shell but CLOSED + REOPENED it on every detail/START
 * visit -- the Fable review of that lane proved the "hang" it was reverted for was
 * really the PDNA_DELTA leg of gbscr_open_inner (source/pdna_gbscreen.c) running
 * its whole-ROM UI locator on EVERY open, ~3,240 frames each time, ~6,450 more per
 * detail/START round trip. This version opens the shell ONCE per pdna_gbhof()
 * visit and keeps it open across LIST <-> DETAIL <-> the START menu <-> EVERY
 * full-screen sub-editor (A2 review fix): the species dex grid, the level
 * stepper, the nickname OSK, app_confirm, msg_wait -- none of pick_species()/
 * hof_level_editor()/osk_input()/app_confirm()/msg_wait() touch
 * gb12_arena_tail() (grep proves only the gbscr-shell family in this file
 * does), so there was nothing to close and reopen around them in the first
 * place; hof_edit_mon()/hof_do_clear()/hof_set_count_editor()/
 * hof_do_add_team()/hof_do_delete_team() are called with `gs` OPEN, exactly
 * the way the trainer card's own g1card_edit_sel() (pdna_gbtrainer.c:693-698)
 * calls its field editor with the shell open, then repaints + marks the
 * canvas dirty on the SAME `gs` -- ONE gbscr_open() per whole visit, proven
 * by direct count (this lane's own report). List/detail/menu are drawn
 * INSIDE the frame with gbscr_text()/gbscr_cell() (never ui_*, which would
 * paint outside the card's border); every full-screen sub-editor still uses
 * ui_* and takes the WHOLE screen -- gbscr_mark_all_dirty() forces a full
 * repaint on return, the #119 partial-repaint contract every screen sharing
 * the Mode-3 framebuffer must honour.
 *
 * F2: no '>' cursor glyph and no '#' row marker -- gb_edit.c's enc_one() maps
 * both to the blank tile (0x7F, "no GB glyph -> a space") on the REAL located
 * font, not just a decorative choice; the row cursor is instead a
 * gbscr_cell_rect() framebuffer highlight (the SAME primitive the trainer card's
 * own field cursor uses) and the row label is "N: ..." (":" IS a real font
 * tile, 0x9C/0x9C on both gens per enc_one()). A4: the shiny mark is " (S)",
 * not "*" (no GB glyph for '*' either -- see hof_card_paint_detail()'s own
 * comment).
 *
 * A1 review fix (the original "NOT wired -- STACK risk" note here was FALSE,
 * per the review's own re-measurement: gb_art_fetch/pdna_origin_art_icon sit
 * behind pdna_origin_art_stack_room(), a GATED subtree the stack walker
 * excludes from this file's own deepest-chain number by design -- rooted at
 * pdna_gbhof alone that chain is ~7,200 B, nowhere near the gate). Gen 2's
 * detail rows now draw each mon's real 16x16 ROM menu icon (hof_card_icon_
 * refresh()/hof_card_icon_blit() below, the SAME pdna_origin_art_icon() chain
 * the dex grid's cell art and the box grid's era art already share, BACKLOG
 * #124) at the row's left. Gen 1 stays text-only, and THAT gap is real: a
 * Gen-1 front-pic decode has no per-species cache at all (unlike Gen 2's
 * icon fetch, memoised one entry deep) -- BACKLOG #196 is where a real
 * per-species Gen-1 cache would need to land first. */
#define HOF_CARD_COLS          18   /* usable columns 1..18 (0/19 are border)  */
#define HOF_CARD_ROW0          4    /* first team/mon/menu row                */
#define HOF_CARD_ROWS_PER_PAGE 8

/* A1: one Gen-2 detail-row menu icon, RGB15 ui_sprite()-ready, and the whole
 * per-visit cache (GBH_NUM_MONS slots) carved from the SAME arena-tail slice
 * the shell's own tile cache uses -- see hof_card_open()'s own comment. */
#define HOF_ICON_W 16
#define HOF_ICON_H 16
#define HOF_ICON_PX (HOF_ICON_W * HOF_ICON_H)
#define HOF_ICON_CACHE_BYTES ((uint32_t)GBH_NUM_MONS * HOF_ICON_PX * 2)

typedef enum { HOF_CARD_LIST = 0, HOF_CARD_DETAIL, HOF_CARD_MENU } HofCardMode;

/* A plain rectangle of TEXTBOX-block tile 0 all the way round -- the SAME
 * border gbscr_run_demo() already draws (pdna_gbscreen.c), reused verbatim. */
static void hof_card_border(GbScreen* gs) {
  for (int x = 0; x < GBSCR_COLS; x++) {
    gbscr_cell(gs, x, 0, GBSCR_SRC_TEXTBOX, 0);
    gbscr_cell(gs, x, GBSCR_ROWS - 1, GBSCR_SRC_TEXTBOX, 0);
  }
  for (int y = 0; y < GBSCR_ROWS; y++) {
    gbscr_cell(gs, 0, y, GBSCR_SRC_TEXTBOX, 0);
    gbscr_cell(gs, GBSCR_COLS - 1, y, GBSCR_SRC_TEXTBOX, 0);
  }
}

/* Blanks every interior cell before a full content repaint -- always painting
 * from a clean interior (rather than tracking which cells a SHORTER new string
 * must blank over a LONGER previous one) is the same "always full-repaint on a
 * mode/page change" posture the plain screens already use (ui_clear() at the
 * top of every hof_*_render), and sidesteps the #119 partial-repaint trap
 * class entirely instead of re-deriving a dirty-cell-diff by hand. */
static void hof_card_clear_interior(GbScreen* gs) {
  for (int y = 1; y < GBSCR_ROWS - 1; y++)
    for (int x = 1; x < GBSCR_COLS - 1; x++)
      gbscr_cell(gs, x, y, GBSCR_SRC_BLANK, 0);
}

/* F2: paints `s` only if it fits the card's own column budget starting at
 * `x` (gbscr_text() itself silently drops anything past GBSCR_COLS-1, but
 * that can truncate mid-word) -- otherwise paints `fallback` (may be NULL,
 * meaning "leave this row blank" rather than show a truncated string). */
static void hof_card_text_fit(GbScreen* gs, uint8_t gen, int x, int y,
                              const char* s, const char* fallback) {
  int budget = HOF_CARD_COLS + 1 - x;
  if (gbscr_text_cols(gen, s) <= budget) gbscr_text(gs, x, y, s);
  else if (fallback)                     gbscr_text(gs, x, y, fallback);
}

/* F2: "N: ..." row label -- no '#', no '>' (drawn instead by the cursor rect
 * in hof_card_screen's own loop). */
static void hof_card_list_row(GbScreen* gs, uint8_t gen, const GbSession* s,
                              int idx, int y) {
  char label[24];
  GbHofTeam t;
  if (gbh_team(s, idx, &t) && t.n > 0) {
    int lo = 100, hi = 0;
    for (int m = 0; m < t.n; m++) {
      if (t.mon[m].level < lo) lo = t.mon[m].level;
      if (t.mon[m].level > hi) hi = t.mon[m].level;
    }
    siprintf(label, "%d: %dmon Lv%d-%d", idx + 1, t.n, lo, hi);
  } else {
    siprintf(label, "%d: --", idx + 1);
  }
  hof_card_text_fit(gs, gen, 1, y, label, 0);
}

static void hof_card_paint_list(GbScreen* gs, uint8_t gen, const GbSession* s,
                                int present, int count, int sel, bool can_edit) {
  hof_card_clear_interior(gs);
  hof_card_border(gs);
  gbscr_text(gs, 2, 1, "HALL OF FAME");
  char hdr[24];
  siprintf(hdr, "%d teams (life %d)", present, count);
  hof_card_text_fit(gs, gen, 1, 2, hdr, "TEAMS");
  if (present == 0) {
    gbscr_text(gs, 1, HOF_CARD_ROW0, "No teams yet.");
  } else {
    int top = (sel / HOF_CARD_ROWS_PER_PAGE) * HOF_CARD_ROWS_PER_PAGE;
    int shown = present - top;
    if (shown > HOF_CARD_ROWS_PER_PAGE) shown = HOF_CARD_ROWS_PER_PAGE;
    for (int i = 0; i < shown; i++)
      hof_card_list_row(gs, gen, s, top + i, HOF_CARD_ROW0 + i);
  }
  if (!can_edit)
    hof_card_text_fit(gs, gen, 1, 13, app_gb_readonly_why(), "READ ONLY");
  hof_card_text_fit(gs, gen, 1, 15, "L/R PAGE", 0);
}

/* Detail: 2 rows/mon (species+level, then the nickname iff it differs from
 * the species name -- same "not always noise" rule hof_detail_render's own
 * comment already established), up to GBH_NUM_MONS mons -- 6*2=12 rows fit
 * comfortably inside rows 4..16 (13 interior rows below the title/wins). OT
 * id is dropped from the card view (there is no more room without a 3rd row
 * per mon); it stays visible on the plain fallback page.
 *
 * A4 review fix: the shiny mark used to be a bare "*", but gb_edit.c's
 * enc_one() has no GB glyph for '*' -- it silently maps to the blank tile
 * and `lost` is discarded here (hof_card_text_fit only measures column
 * COUNT via gbscr_text_cols(), which counts a lost glyph as ONE cell same as
 * a real one), so a shiny Gen-2 HoF mon showed NOTHING extra at all. " (S)"
 * is four real font tiles (space/'('/S/')' -- every one of them is in
 * enc_one()'s single-ASCII-character table, pinned by
 * tests/host_gbhof_glyph_test.c) and reads unambiguously on a screen with no
 * colour to show an actual shiny sparkle.
 *
 * A1 GEN-2 ONLY: each row's left 2 cells (16x16 px at 1:1) show the ROM's
 * OWN menu icon (pdna_origin_art_icon(), the SAME chain the dex grid's cell
 * art and the box grid's era art already share -- BACKLOG #124) -- text
 * starts at x=3 instead of x=1 to leave room, so HOF_CARD_TEXT_X below feeds
 * hof_card_text_fit() a tighter (16-col) budget on Gen 2 than Gen 1's 18.
 * GEN 1 stays text-only: gb_art_fetch's Gen-1 pic decode has no per-species
 * cache at all yet (unlike Gen 2's icon fetch, which IS memoised one entry
 * deep, see fetch_pic_ex()'s own s_memo_* -- still only one entry, which is
 * exactly why this card renders the icon cache ONCE per paint into
 * `icon_cache`, below, rather than re-fetching every frame) -- a 6-row Gen-1
 * page would re-decode 6 DIFFERENT front pics from the ROM on every single
 * paint, no cache to reuse between them. BACKLOG #196 is where a real
 * per-species Gen-1 cache would need to land before this is safe to add. */
#define HOF_CARD_TEXT_X(gen) ((gen) == GB_GEN2 ? 3 : 1)

static void hof_card_paint_detail(GbScreen* gs, uint8_t gen, const GbHofTeam* t,
                                  int team_no) {
  hof_card_clear_interior(gs);
  hof_card_border(gs);
  char title[16];
  siprintf(title, "TEAM %d", team_no);
  gbscr_text(gs, 2, 1, title);
  if (gen == GB_GEN2) {
    char wc[20];
    siprintf(wc, "Wins: %d", (int)t->win_count);
    hof_card_text_fit(gs, gen, 1, 2, wc, 0);
  }
  int tx = HOF_CARD_TEXT_X(gen);
  for (int m = 0; m < t->n && m < GBH_NUM_MONS; m++) {
    int y = HOF_CARD_ROW0 + m * 2;
    const GbHofMon* mn = &t->mon[m];
    const char* nm = (mn->dex >= 1 && mn->dex <= 251) ? pk_species_name(mn->dex) : "?";
    char line[28];
    siprintf(line, "%s Lv%d%s", nm, mn->level, (gen == GB_GEN2 && mn->shiny) ? " (S)" : "");
    hof_card_text_fit(gs, gen, tx, y, line, nm);
    if (mn->nick[0] != '\0' && strcmp(mn->nick, nm) != 0)
      hof_card_text_fit(gs, gen, tx, y + 1, mn->nick, 0);
  }
}

/* A1: fetch + render every mon's 16x16 menu icon into `cache` (GBH_NUM_MONS
 * slots of 16*16 RGB15 = 512 B each, carved from the SAME arena-tail slice
 * hof_card_open() sized -- see that function's own comment) ONCE per detail
 * paint -- Gen 2 only, a no-op on Gen 1 (deliberately, see the paint
 * function's own comment above). A slot whose fetch fails (no ROM/no stack
 * room right now/species out of range) is zeroed -- ui_sprite() treats an
 * all-zero RGB15 buffer as fully transparent, so a failed icon draws
 * nothing rather than garbage. noinline for the same reason gbdex_cell_blit
 * (pdna_gbdex.c) is: pdna_origin_cell_render()'s own scale/blit locals must
 * never coexist on the stack with pdna_origin_art_icon()'s own fetch chain
 * frame (its own GATED pdna_origin_art_stack_room(PDNA_GB_ICON_NEED) check
 * covers that chain independently of this file's un-gated STACK number --
 * confirmed by direct measurement, the review's own re-verify note). */
static void __attribute__((noinline))
hof_card_icon_refresh(uint8_t gen, const GbHofTeam* t, uint8_t* cache) {
  if (gen != GB_GEN2 || !cache) return;
  for (int m = 0; m < GBH_NUM_MONS; m++) {
    uint16_t* slot = (uint16_t*)(cache + (uint32_t)m * HOF_ICON_PX * 2);
    memset(slot, 0, HOF_ICON_PX * 2);
    if (m >= t->n) continue;
    uint16_t dex = t->mon[m].dex;
    if (dex < 1 || dex > 251) continue;
    PdnaArt a;
    if (!pdna_origin_art_icon(dex, &a) || !a.px) continue;
    pdna_origin_cell_render(&a, slot, HOF_ICON_W, HOF_ICON_H);
  }
}

/* A1: blit the ALREADY-RENDERED cache onto the framebuffer, one 16x16 sprite
 * per row, at each row's own pixel origin (gbscr_cell_rect(), the SAME
 * primitive the cursor rect uses -- adapts to gb_scale_mode automatically).
 * Pure RAM->framebuffer copy, no SD/ROM access at all -- safe to call every
 * frame the DETAIL page is on screen (hof_card_screen's own loop does,
 * exactly like the cursor rect it is drawn alongside), which is what keeps
 * the icons visible across a cursor move or a SELECT scale toggle without
 * needing a second content repaint or a dirty-cell-per-icon scheme. */
static void hof_card_icon_blit(uint8_t gen, int n, const uint8_t* cache) {
  if (gen != GB_GEN2 || !cache) return;
  if (n > GBH_NUM_MONS) n = GBH_NUM_MONS;
  for (int m = 0; m < n; m++) {
    int cy = HOF_CARD_ROW0 + m * 2;
    int px0, py0, px1, py1;
    gbscr_cell_rect(1, cy, 2, 2, &px0, &py0, &px1, &py1);
    int w = px1 - px0, h = py1 - py0;
    if (w <= 0 || h <= 0) continue;
    const uint16_t* slot = (const uint16_t*)(cache + (uint32_t)m * HOF_ICON_PX * 2);
    if (w == HOF_ICON_W && h == HOF_ICON_H) {
      ui_sprite(px0, py0, w, h, slot);
    } else {
      /* stretched scale: pdna_origin_cell_render() already fit the source
       * into a 16x16 buffer -- re-render straight to the stretched size
       * would need a second cache, so this rung simply skips the icon at
       * non-1:1 scale rather than draw it warped or mis-sized; the row TEXT
       * still repaints correctly either way (gbscr's own stretch path). */
    }
  }
}

static void hof_card_paint_menu(GbScreen* gs, uint8_t gen) {
  hof_card_clear_interior(gs);
  hof_card_border(gs);
  gbscr_text(gs, 2, 1, "HOF MENU");
  for (int i = 0; i < HOFMENU_N; i++)
    hof_card_text_fit(gs, gen, 1, HOF_CARD_ROW0 + i * 2, kHofMenuLbl[i], 0);
}

static void hof_card_legend_list(GbScreen* gs, bool can_edit) {
  static const char* const kEdit[4] =
    { PDNA_LBL_VIEW, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, PDNA_GBHOF_ACT_MENU };
  static const char* const kView[4] =
    { PDNA_LBL_VIEW, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0 };
  gbscr_set_legend(gs, can_edit ? kEdit : kView);
}

static void hof_card_legend_detail(GbScreen* gs, bool can_edit) {
  static const char* const kEdit[4] =
    { PDNA_GBTR_ACT_EDIT, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0 };
  static const char* const kView[4] =
    { 0, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0 };
  gbscr_set_legend(gs, can_edit ? kEdit : kView);
}

static void hof_card_legend_menu(GbScreen* gs) {
  static const char* const kMenu[4] =
    { PDNA_GBSCR_ACT_OK, PDNA_GBSCR_ACT_BACK, PDNA_GBSCR_ACT_SIZE, 0 };
  gbscr_set_legend(gs, kMenu);
}

/* A1: ONE slice, carved -- the shell's own tile cache AND the Gen-2 icon
 * cache share the SAME gb12_arena_tail() loan (the trainer card's own
 * "shell + player-pic share one slice" pattern, pdna_gbtrainer.c's
 * pdna_gbtrainer_gen1_card()). Returns a pointer to the icon-cache bytes
 * (past the shell's own share) on success, NULL on any refusal (no ROM, bad
 * ROM, non-English release, no tile-bank memory, or not enough arena left)
 * -- Gen 1 still gets a valid (unused) pointer back, since HOF_ICON_CACHE_
 * BYTES is only ever read/written by the Gen-2-gated icon functions above. */
static uint8_t* hof_card_open(uint8_t gen, GbScreen* gs) {
  uint32_t shell_need = gbscr_tail_need(gen, GBSCR_NEED_TEXTBOX, 0);
  uint32_t need = shell_need + HOF_ICON_CACHE_BYTES;
  uint8_t* tail = gb12_arena_tail(need);
  const char* reason = 0;
  if (!gbscr_open(gen, gs, tail, shell_need, GBSCR_NEED_TEXTBOX, 0, &reason)) {
    gb12_arena_tail_release();
    return 0;
  }
  return tail + shell_need;
}

/* All mutable state one visit to the card shell threads through the three
 * per-mode key handlers below -- a struct instead of a growing parameter
 * list per handler (rule 4: keeps hof_card_screen()'s own loop body short
 * enough to read as one page). */
typedef struct {
  HofCardMode mode;
  int sel, detail_team, mon_sel, menu_sel, present, count;
  uint8_t* icon_cache;   /* A1: GBH_NUM_MONS x 16x16 RGB15, carved by hof_card_open() */
} HofCardState;

/* Re-reads present/count, clamps `sel`, and repaints the LIST content --
 * every path that lands back on LIST (B from DETAIL/MENU, or after a
 * sub-editor closes the shell and reopens it) needs exactly this. */
static void hof_card_back_to_list(GbSession* s, GbScreen* gs, bool can_edit, HofCardState* st) {
  st->mode = HOF_CARD_LIST;
  st->present = gbh_team_count_present(s);
  st->count = gbh_count(s);
  if (st->present == 0) st->sel = 0;
  else if (st->sel >= st->present) st->sel = st->present - 1;
  hof_card_legend_list(gs, can_edit);
  hof_card_paint_list(gs, s->gen, s, st->present, st->count, st->sel, can_edit);
}

/* LIST page: UP/DOWN/L-R move the cursor/page; A enters DETAIL; START opens
 * MENU (can_edit only). Always ends by marking the whole canvas dirty (erases
 * the previous frame's cursor rect, #119) and repainting whatever mode it
 * lands in. */
static void hof_card_list_key(GbSession* s, GbScreen* gs, bool can_edit, u16 k,
                              HofCardState* st) {
  if (k & KEY_UP)        st->sel = (st->present > 0) ? ((st->sel > 0) ? st->sel - 1 : st->present - 1) : 0;
  else if (k & KEY_DOWN) st->sel = (st->present > 0) ? ((st->sel + 1) % st->present) : 0;
  else if (k & (KEY_LEFT | KEY_RIGHT)) {
    if (st->present > 0) {
      int dir = (k & KEY_LEFT) ? -1 : 1;
      st->sel += dir * HOF_CARD_ROWS_PER_PAGE;
      if (st->sel < 0) st->sel = 0;
      if (st->sel >= st->present) st->sel = st->present - 1;
    }
  } else if (k & KEY_A) {
    if (st->present == 0) { snd_deny(); gbscr_mark_all_dirty(gs); return; }
    st->detail_team = st->sel; st->mon_sel = 0; st->mode = HOF_CARD_DETAIL;
    hof_card_legend_detail(gs, can_edit);
  } else if (k & KEY_START) {
    if (!can_edit) { snd_deny(); gbscr_mark_all_dirty(gs); return; }
    st->mode = HOF_CARD_MENU; st->menu_sel = 0;
    hof_card_legend_menu(gs);
  }
  gbscr_mark_all_dirty(gs);
  if (st->mode == HOF_CARD_LIST)
    hof_card_paint_list(gs, s->gen, s, st->present, st->count, st->sel, can_edit);
  else if (st->mode == HOF_CARD_DETAIL) {
    GbHofTeam t;
    if (gbh_team(s, st->detail_team, &t)) {
      hof_card_paint_detail(gs, s->gen, &t, st->detail_team + 1);
      hof_card_icon_refresh(s->gen, &t, st->icon_cache);   /* A1: fetch once per paint */
    }
  } else {
    hof_card_paint_menu(gs, s->gen);
  }
}

/* DETAIL page. A1/A2 review fix: the shell stays OPEN across the per-mon field
 * editor -- pick_species()/hof_level_editor()/osk_input()/app_confirm()/
 * msg_wait() never touch gb12_arena_tail() (grep proves only the gbscr-shell
 * family in this file does), so there is nothing to release/reacquire around
 * them; the trainer card's own g1card_edit_sel() (pdna_gbtrainer.c:693-698)
 * calls its field editor with the shell open too, then repaints + marks dirty
 * on the SAME `gs` -- this now matches that posture exactly instead of paying
 * a second gbscr_open() (another ~3,240-frame PDNA_DELTA locator scan) for an
 * editor that never needed the shell closed at all. */
static void hof_card_detail_key(GbSession* s, GbScreen* gs, bool can_edit, u16 k,
                                HofCardState* st) {
  GbHofTeam t;
  if (!gbh_team(s, st->detail_team, &t)) {
    hof_card_back_to_list(s, gs, can_edit, st);
    gbscr_mark_all_dirty(gs);
    return;
  }
  if (k & KEY_B) { hof_card_back_to_list(s, gs, can_edit, st); gbscr_mark_all_dirty(gs); return; }
  if (k & KEY_UP)        st->mon_sel = (st->mon_sel > 0) ? st->mon_sel - 1 : t.n - 1;
  else if (k & KEY_DOWN) st->mon_sel = (st->mon_sel + 1) % t.n;
  else if (k & KEY_A)
    /* Full-screen ui_* menu, shell left open -- it repaints the WHOLE
     * Mode-3 framebuffer itself, so the shell's own tiles are gone the
     * instant this call starts painting; gbscr_mark_all_dirty() below
     * (unconditional, every branch) is what brings them back on the very
     * next gbscr_flush(), the same #119 partial-repaint contract F3 cites. */
    hof_edit_mon(s, st->detail_team, st->mon_sel, &t.mon[st->mon_sel]);
  if (!gbh_team(s, st->detail_team, &t)) {
    hof_card_back_to_list(s, gs, can_edit, st);
  } else {
    hof_card_paint_detail(gs, s->gen, &t, st->detail_team + 1);
    hof_card_icon_refresh(s->gen, &t, st->icon_cache);   /* A1: species/shiny may have changed */
  }
  gbscr_mark_all_dirty(gs);
}

/* START menu (CLEAR ALL / SET COUNT / ADD TEAM / DELETE TEAM), drawn inside
 * the frame. A2: same "shell stays open" fix as hof_card_detail_key() above --
 * none of hof_do_clear/hof_set_count_editor/hof_do_add_team/hof_do_delete_team
 * touch gb12_arena_tail either. */
static void hof_card_menu_key(GbSession* s, GbScreen* gs, bool can_edit, u16 k,
                              HofCardState* st) {
  if (k & KEY_B) { hof_card_back_to_list(s, gs, can_edit, st); gbscr_mark_all_dirty(gs); return; }
  if (k & KEY_UP)        st->menu_sel = (st->menu_sel > 0) ? st->menu_sel - 1 : HOFMENU_N - 1;
  else if (k & KEY_DOWN) st->menu_sel = (st->menu_sel + 1) % HOFMENU_N;
  else if (k & KEY_A) {
    /* Full-screen ui_* confirm/stepper/picker, shell left open (A2) -- same
     * "mark_all_dirty repaints it" contract as the per-mon editor above. */
    if (st->menu_sel == HOFMENU_CLEAR) hof_do_clear(s);
    else if (st->menu_sel == HOFMENU_SETCOUNT) hof_set_count_editor(s, s->gen);
    else if (st->menu_sel == HOFMENU_ADD) hof_do_add_team(s);
    else hof_do_delete_team(s, st->sel);
    hof_card_back_to_list(s, gs, can_edit, st);
  }
  gbscr_mark_all_dirty(gs);
}

/* The cursor's cell span for the current mode -- gbscr_cell_rect() turns
 * this into the framebuffer pixel rect m3_frame() outlines (F2: this IS the
 * row cursor; no '>' glyph is ever drawn). */
static void hof_card_cursor(const HofCardState* st, bool can_edit,
                            int* cx, int* cy, int* cw, int* ch, bool* show) {
  *cx = 1; *cw = HOF_CARD_COLS; *ch = 1; *show = true;
  if (st->mode == HOF_CARD_LIST) {
    if (st->present == 0) { *show = false; return; }
    int top = (st->sel / HOF_CARD_ROWS_PER_PAGE) * HOF_CARD_ROWS_PER_PAGE;
    *cy = HOF_CARD_ROW0 + (st->sel - top);
  } else if (st->mode == HOF_CARD_DETAIL) {
    if (!can_edit) { *show = false; return; }
    *cy = HOF_CARD_ROW0 + st->mon_sel * 2;
  } else {
    *cy = HOF_CARD_ROW0 + st->menu_sel * 2;
  }
}

/* Returns true once the shell has run to completion (B pressed on the LIST
 * page -- A2: the shell now stays open for the WHOLE visit, including every
 * full-screen sub-editor, so there is no "could not reopen" exit any more);
 * false only if the shell never opened at all, meaning the caller must show
 * the plain screen. */
static bool hof_card_screen(GbSession* s, bool can_edit) {
  GbScreen gs;
  uint8_t* icon_cache = hof_card_open(s->gen, &gs);
  if (!icon_cache) return false;

  HofCardState st;
  memset(&st, 0, sizeof st);
  st.mode = HOF_CARD_LIST;
  st.present = gbh_team_count_present(s);
  st.count = gbh_count(s);
  st.icon_cache = icon_cache;
  hof_card_legend_list(&gs, can_edit);
  hof_card_paint_list(&gs, s->gen, s, st.present, st.count, st.sel, can_edit);

  for (;;) {
    gbscr_flush(&gs, 0);

    if (st.mode == HOF_CARD_DETAIL) {
      /* A1: cheap RAM->framebuffer blit, every frame -- see hof_card_icon_
       * blit()'s own comment for why this (not a per-icon dirty scheme) is
       * both correct and safe to call unconditionally here. */
      GbHofTeam t;
      if (gbh_team(s, st.detail_team, &t))
        hof_card_icon_blit(s->gen, t.n, st.icon_cache);
    }

    int cx, cy, cw, ch; bool show_cursor;
    hof_card_cursor(&st, can_edit, &cx, &cy, &cw, &ch, &show_cursor);
    if (show_cursor) {
      int px0, py0, px1, py1;
      gbscr_cell_rect(cx, cy, cw, ch, &px0, &py0, &px1, &py1);
      m3_frame(px0, py0, px1, py1, GBCARD_CSEL);
    }

    u16 mask = KEY_SELECT | KEY_B;
    if (st.mode == HOF_CARD_LIST)        mask |= KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_START;
    else if (st.mode == HOF_CARD_DETAIL) mask |= can_edit ? (KEY_UP | KEY_DOWN | KEY_A) : 0;
    else                                 mask |= KEY_UP | KEY_DOWN | KEY_A;
    u16 k = s_wait(mask);

    if (k & KEY_SELECT) { gbscr_toggle_scale(&gs); continue; }

    if (st.mode == HOF_CARD_LIST) {
      if (k & KEY_B) break;
      hof_card_list_key(s, &gs, can_edit, k, &st);
    } else if (st.mode == HOF_CARD_DETAIL) {
      hof_card_detail_key(s, &gs, can_edit, k, &st);
    } else {
      hof_card_menu_key(s, &gs, can_edit, k, &st);
    }
  }

  gbscr_close(&gs);
  gb12_arena_tail_release();
  return true;
}

/* ---- entry ----------------------------------------------------------------------- */

void pdna_gbhof(GbSession* s, bool can_edit) {
  if (!s || !s->open) {
    msg_wait("HALL OF FAME", UI_WARN, "Could not read this save.", 0);
    return;
  }
  if (hof_card_screen(s, can_edit)) return;
  hof_plain_screen(s, can_edit, true);   /* gbscr refused: honest "GB ART: OFF" header */
}
