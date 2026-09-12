/*
 * Gen-1/2 "Flags & counters" -- BACKLOG #88. Mirrors pdna_main.c's data_editor_tab()
 * (lines ~4983-5169 as of the b88 brief: two tabs COUNTERS|FLAGS, L/R swaps, 9-px
 * rows, 14 visible; the FLAGS tab is a foldable NamedFlag-shaped list with section
 * HEADER rows (A folds/unfolds, SELECT jumps to the next header), a trailing "Raw
 * flag browser (#N)..." row, and a ONE-TIME "CAUTION / Toggling story flags can /
 * soft-lock the save." before the first toggle each visit) -- same row geometry,
 * same legends, same gesture set, over a live GbSession instead of SaveBlock1.
 *
 * Data (docs/GB-FLAGS-RESEARCH.md, all citations held; generated into
 * source/gb_flags.c by tools/gen_gbfields.py): each FLAGS-tab row carries a
 * GbFlagKind (gb_flags.h) the research doc assigned --
 *   GBFL_KIND_TOGGLE     plain A-to-toggle, behind the one-time CAUTION
 *   GBFL_KIND_BAG_GRANT  HM01-05/Bicycle/GS Ball: these are bag-item grants in the
 *                        real game, not flags worth toggling directly -- shown
 *                        READ-ONLY with "(grant it in the Bag)"
 *   GBFL_KIND_READONLY   Champion/Elite-Four story flags -- display only
 *   GBFL_KIND_WARN       EVENT_RESTORED_POWER_TO_KANTO -- toggle behind an EXTRA
 *                        confirm naming the consequence (mirrors PokeDNA's existing
 *                        id_edit_ok() identity-edit-warning pattern)
 *
 * COUNTERS tab: Money/Coins (via gbt_read/gbt_write, which already knows each
 * game's BCD-vs-binary encoding and cap -- not re-derived here), Rival's name (a
 * GB-text field gbt_write never touches, so written directly through
 * gbs_write_field/gb_name_encode -- the same primitives gbtr_edit_row's own NAME
 * case uses), Safari Zone steps (Gen 1 only, editable ONLY while
 * EVENT_IN_SAFARI_ZONE reads set -- this tool never SETS that flag itself, entering
 * the zone is a map-placement operation, out of scope here), the lucky-number
 * "already shown today" flag (Gen 2 only, a plain on/off byte -- the actual lucky
 * NUMBER is derived state the game computes from the date and is never edited,
 * per the research doc).
 *
 * Commit: every edit lands in the session's own RAM image immediately (gbfl_set /
 * gbs_write_field, same as Gen 3's live SaveBlock1 edits); on B, if anything moved,
 * one confirm ("Save data changes?") then gbs_finish(s) + gb_persist("gbflags") --
 * any failure rolls the WHOLE image back (gb_rollback), never a partial write.
 *
 * Read-only carts (can_edit == false): every row is still browsable; A is refused
 * with snd_deny(), mirroring pdna_gbclock.c's own read-only posture (never a silent
 * dead key -- s_wait's generic snd_ok() on the raw A hit is corrected immediately).
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_gbflags.h"
#include "gb_flags.h"
#include "gb_flags_rw.h"
#include "gb_fields.h"
#include "gb_trainer.h"    /* gbt_game/gbt_read/gbt_write -- money/coins            */
#include "gb_edit.h"       /* GB_GEN1, GB_OT_GLYPHS, GB_TEXT_MAX, gb_name_encode/decode */
#include "flags_fold.h"    /* ff_build_ord/ff_hdr_ord/ff_row_visible/ff_step (BACKLOG #2a) */
#include "pdna_gen12.h"    /* gb_rollback / gb_persist                               */
#include "pdna_trainer.h"  /* trainer_row_paint / trainer_key_legend / num_entry     */
#include "osk.h"           /* osk_input */
#include "ui.h"
#include "snd.h"
#include "pdna_app.h"      /* msg_wait / app_confirm                                */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* ============================================================================
 * ---- COUNTERS tab ----------------------------------------------------------
 * ============================================================================ */

enum { CTR_MONEY = 0, CTR_COINS, CTR_RIVAL, CTR_SAFARI, CTR_LUCKY, CTR_KIND_MAX };

static int ctr_build_rows(GbGame g, int* rows) {
  int n = 0;
  if (gbf_off(g, GBF_MONEY) || gbf_off(g, GBF_MONEY_BIN)) rows[n++] = CTR_MONEY;
  if (gbf_off(g, GBF_COINS) || gbf_off(g, GBF_COINS_BIN)) rows[n++] = CTR_COINS;
  if (gbf_off(g, GBF_RIVAL_NAME)) rows[n++] = CTR_RIVAL;
  if (gbf_off(g, GBF_SAFARI_STEPS)) rows[n++] = CTR_SAFARI;
  if (gbf_off(g, GBF_LUCKY_NUMBER_SHOW_FLAG)) rows[n++] = CTR_LUCKY;
  return n;
}

static bool ctr_read_u8(GbSession* s, GbGame g, GbField f, uint8_t* out) {
  uint32_t off = gbf_off(g, f);
  if (!off) return false;
  return gbs_read_field(s, off, out, 1) == GBS_OK;
}
static GbsStatus ctr_write_u8_unless_same(GbSession* s, GbGame g, GbField f, uint8_t v,
                                          bool* changed) {
  uint32_t off = gbf_off(g, f);
  if (!off) return GBS_ERR_ARG;
  uint8_t cur;
  if (gbs_read_field(s, off, &cur, 1) == GBS_OK && cur == v) return GBS_OK;
  GbsStatus st = gbs_write_field(s, off, &v, 1);
  if (st == GBS_OK) *changed = true;
  return st;
}

static bool ctr_read_rival(GbSession* s, GbGame g, char out[GB_TEXT_MAX]) {
  uint32_t off = gbf_off(g, GBF_RIVAL_NAME);
  uint16_t len = gbf_len(g, GBF_RIVAL_NAME);
  if (!off || !len) { out[0] = 0; return false; }
  uint8_t raw[16];
  if (len > sizeof raw) len = sizeof raw;
  if (gbs_read_field(s, off, raw, len) != GBS_OK) { out[0] = 0; return false; }
  gb_name_decode(s->gen, out, GB_TEXT_MAX, raw, len);
  return true;
}
static GbsStatus ctr_write_rival(GbSession* s, GbGame g, const char* name, bool* changed) {
  uint32_t off = gbf_off(g, GBF_RIVAL_NAME);
  uint16_t len = gbf_len(g, GBF_RIVAL_NAME);
  if (!off || !len || len > 16) return GBS_ERR_ARG;
  uint8_t nb[16];
  gb_name_encode(s->gen, nb, len, GB_OT_GLYPHS, name);
  uint8_t cur[16];
  if (gbs_read_field(s, off, cur, len) == GBS_OK && memcmp(cur, nb, len) == 0) return GBS_OK;
  GbsStatus st = gbs_write_field(s, off, nb, len);
  if (st == GBS_OK) *changed = true;
  return st;
}

static void ctr_row_paint(GbSession* s, GbGame g, int kind, int y, bool sel) {
  char val[40];
  const char* lbl = "";
  uint16_t ink = UI_TEXT;
  switch (kind) {
    case CTR_MONEY: {
      GbTrainer t;
      if (gbt_read(s, &t) && t.money_ok) { lbl = "MONEY"; siprintf(val, "$%lu", (unsigned long)t.money); }
      else { lbl = "MONEY"; siprintf(val, "?"); ink = UI_DIM; }
    } break;
    case CTR_COINS: {
      GbTrainer t;
      if (gbt_read(s, &t) && t.coins_ok) { lbl = "COINS"; siprintf(val, "%u", (unsigned)t.coins); }
      else { lbl = "COINS"; siprintf(val, "?"); ink = UI_DIM; }
    } break;
    case CTR_RIVAL: {
      char nm[GB_TEXT_MAX];
      ctr_read_rival(s, g, nm);
      lbl = "RIVAL"; siprintf(val, "%s", nm);
    } break;
    case CTR_SAFARI: {
      uint8_t steps = 0;
      bool ok = ctr_read_u8(s, g, GBF_SAFARI_STEPS, &steps);
      int sz_flag = gbfl_safari_zone_flag(g);
      bool in_zone = sz_flag >= 0 && gbfl_get(s, g, (uint16_t)sz_flag);
      lbl = "SAFARI";
      if (!ok) { siprintf(val, "?"); ink = UI_DIM; }
      else if (!in_zone) { siprintf(val, "%u (not in zone)", (unsigned)steps); ink = UI_DIM; }
      else siprintf(val, "%u steps left", (unsigned)steps);
    } break;
    case CTR_LUCKY: {
      uint8_t v = 0;
      bool ok = ctr_read_u8(s, g, GBF_LUCKY_NUMBER_SHOW_FLAG, &v);
      lbl = "LUCKY#";
      if (!ok) { siprintf(val, "?"); ink = UI_DIM; }
      else siprintf(val, "%s", v ? "shown today" : "not shown yet");
    } break;
    default: val[0] = 0;
  }
  trainer_row_paint(y, sel, lbl, val, ink);
}

/* One field's A-edit. `dirty`/`can_edit` shared with the caller. */
static void ctr_edit_row(GbSession* s, GbGame g, int kind, bool* dirty) {
  switch (kind) {
    case CTR_MONEY: {
      GbTrainer t;
      if (!gbt_read(s, &t) || !t.money_ok) { msg_wait("UNREADABLE", UI_WARN, "This field is not a valid", "number and cannot be edited."); break; }
      t.money = num_entry("MONEY", t.money, 999999u);
      GbsStatus st = gbt_write(s, &t);
      if (st != GBS_OK) msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), 0);
      else *dirty = true;
    } break;
    case CTR_COINS: {
      GbTrainer t;
      if (!gbt_read(s, &t) || !t.coins_ok) { msg_wait("UNREADABLE", UI_WARN, "This field is not a valid", "number and cannot be edited."); break; }
      t.coins = (uint16_t)num_entry("COINS", t.coins, 9999u);
      GbsStatus st = gbt_write(s, &t);
      if (st != GBS_OK) msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), 0);
      else *dirty = true;
    } break;
    case CTR_RIVAL: {
      char cur[GB_TEXT_MAX]; ctr_read_rival(s, g, cur);
      char b[GB_TEXT_MAX];
      if (osk_input("RIVAL NAME", cur, b, sizeof b)) {
        bool changed = false;
        GbsStatus st = ctr_write_rival(s, g, b, &changed);
        if (st != GBS_OK) msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), 0);
        else if (changed) *dirty = true;
      }
    } break;
    case CTR_SAFARI: {
      int sz_flag = gbfl_safari_zone_flag(g);
      bool in_zone = sz_flag >= 0 && gbfl_get(s, g, (uint16_t)sz_flag);
      if (!in_zone) { snd_deny(); msg_wait("SAFARI ZONE", UI_WARN, "Only editable while the save", "is inside the Safari Zone."); break; }
      uint8_t cur = 0; ctr_read_u8(s, g, GBF_SAFARI_STEPS, &cur);
      uint32_t v = num_entry("SAFARI STEPS", cur, 255u);
      bool changed = false;
      GbsStatus st = ctr_write_u8_unless_same(s, g, GBF_SAFARI_STEPS, (uint8_t)v, &changed);
      if (st != GBS_OK) msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), 0);
      else if (changed) *dirty = true;
    } break;
    case CTR_LUCKY: {
      uint8_t cur = 0; ctr_read_u8(s, g, GBF_LUCKY_NUMBER_SHOW_FLAG, &cur);
      bool changed = false;
      GbsStatus st = ctr_write_u8_unless_same(s, g, GBF_LUCKY_NUMBER_SHOW_FLAG, cur ? 0 : 1, &changed);
      if (st != GBS_OK) msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), 0);
      else if (changed) *dirty = true;
    } break;
    default: break;
  }
}

/* ============================================================================
 * ---- FLAGS tab --------------------------------------------------------------
 * The row table is copied from gbfl_row_at() into a NamedFlag-shaped local array
 * ONCE per screen entry -- GbFlagRow is NOT layout-identical to NamedFlag (it
 * carries an extra `kind` byte), so this is a field-by-field copy, never a
 * reinterpret-cast (see tests/host_gbflagsfold_test.c's own header comment for
 * why). A PARALLEL kind[] array (same index) carries the GbFlagKind flags_fold.h
 * itself does not know about.
 * ============================================================================ */

#define GBFL_ROW_CAP 64   /* >= the largest real table (Crystal: 49 rows, gbfl_row_count) */
/* NF_ORD_MAX's own size-rule note (pdna_main.c) applies here too: GBFL_ROW_CAP must
 * stay >= the largest per-game gbfl_row_count(); a row past the cap silently clamps
 * out (flg_cache's own `if (n > GBFL_ROW_CAP) n = GBFL_ROW_CAP`), never an overflow. */
#define GBFL_TAIL_NEED ((uint32_t)(GBFL_ROW_CAP * sizeof(NamedFlag) \
                        + GBFL_ROW_CAP /* kind[] */ + GBFL_ROW_CAP /* ord[] */))

/* Carved from the GB12 arena tail (gb12_arena_tail), NOT a second module-level
 * static array -- pdna_main.c's own NF_ORD_MAX/s_nf_for comment is explicit that a
 * second static of this shape is the thing to avoid. These are just POINTERS
 * (trivial IWRAM .data, not the ~640 B the arrays themselves would cost); the bytes
 * they point at live in EWRAM, lent for exactly this screen's visit and released on
 * every exit path (flg_tail_acquire/flg_tail_release below). Only reachable when
 * g_ed is non-NULL (pdna_gen12.c's own gate on this screen), which is exactly
 * gb12_arena_tail()'s own precondition. */
static NamedFlag*  s_nf;
static uint8_t*    s_kind;
static uint8_t*    s_ord;
static int         s_nc;
static GbGame       s_nf_for = (GbGame)-1;

/* Acquire the tail slice for this visit; MUST be paired with flg_tail_release() on
 * every exit path. Returns false (nothing carved, s_nf left NULL) if the arena has
 * no room right now -- callers must refuse rather than dereference a NULL s_nf. */
static bool flg_tail_acquire(void) {
  uint8_t* tail = gb12_arena_tail(GBFL_TAIL_NEED);
  if (!tail) { s_nf = 0; s_kind = 0; s_ord = 0; return false; }
  s_nf   = (NamedFlag*)(void*)tail;
  s_kind = tail + GBFL_ROW_CAP * sizeof(NamedFlag);
  s_ord  = s_kind + GBFL_ROW_CAP;
  s_nf_for = (GbGame)-1;   /* force flg_cache to repopulate: fresh/reused bytes */
  return true;
}
static void flg_tail_release(void) {
  gb12_arena_tail_release();
  s_nf = 0; s_kind = 0; s_ord = 0; s_nf_for = (GbGame)-1;
}

static void flg_cache(GbGame g) {
  if (s_nf_for == g) return;
  int n = gbfl_row_count(g);
  if (n > GBFL_ROW_CAP) n = GBFL_ROW_CAP;   /* clamped -- see the size-rule note above */
  for (int i = 0; i < n; i++) {
    GbFlagRow row;
    gbfl_row_at(g, i, &row);
    s_nf[i].num = row.index;
    s_nf[i].name = row.label;
    s_kind[i] = row.kind;
  }
  s_nc = n;
  ff_build_ord(s_nf, s_nc, s_ord, GBFL_ROW_CAP);
  s_nf_for = g;
}

static const char* kind_suffix(uint8_t kind) {
  switch (kind) {
    case GBFL_KIND_BAG_GRANT: return GBFL_SUF_BAG;
    case GBFL_KIND_READONLY:  return GBFL_SUF_STY;
    case GBFL_KIND_WARN:      return GBFL_SUF_WARN;
    default: return "";
  }
}

/* Session-lifetime fold state, mirroring pdna_main.c's own s_flags_folded (a
 * DIFFERENT static of the same name would be fine too -- internal linkage, separate
 * translation unit -- named s_gbfl_folded anyway so grep never confuses the two
 * screens' fold state while reading either file). Every session starts fully
 * collapsed, then persists across screen visits until power-off. */
static uint32_t s_gbfl_folded = 0xFFFFFFFFu;

static void nf_draw_row(GbSession* s, GbGame g, int r, int y, bool sel) {
  char row[64];
  if (r == s_nc) {
    if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    ui_text(8, y, sel ? UI_SELTEXT : UI_DIM, "Raw flag browser (#N)...");
    return;
  }
  if (s_nf[r].num == NAMED_FLAG_HEADER) {
    char fold_glyph = ((s_gbfl_folded >> ff_hdr_ord(s_ord, GBFL_ROW_CAP, r)) & 1u) ? '+' : '-';
    siprintf(row, "%c %s", fold_glyph, s_nf[r].name);
    char ht[40]; ui_truncate(ht, row, 29);
    if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    ui_text(4, y, sel ? UI_SELTEXT : UI_DIRCLR, ht);
    return;
  }
  bool ro = (s_kind[r] == GBFL_KIND_BAG_GRANT || s_kind[r] == GBFL_KIND_READONLY);
  bool on = gbfl_get(s, g, s_nf[r].num);
  siprintf(row, GBFL_ROW_FMT, s_nf[r].name, on ? "ON" : "off", kind_suffix(s_kind[r]));
  char rt[40]; ui_truncate(rt, row, GBFL_ROW_COLS);
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  ui_text(8, y, sel ? UI_SELTEXT : (ro ? UI_DIM : (on ? UI_OK : UI_DIM)), rt);
}

static bool nf_visible(int r) { return ff_row_visible(s_nf, s_nc, s_ord, GBFL_ROW_CAP, s_gbfl_folded, r); }
static int  nf_step(int total, int r, int dir) { return ff_step(s_nf, s_nc, s_ord, GBFL_ROW_CAP, s_gbfl_folded, total, r, dir); }

/* Repaint just row r of the COUNTERS list in place. ctr_row_paint() (via
 * trainer_row_paint) already wipes its own 236x9 band on EVERY call, selected or
 * not, so unlike the flags painter below this needs no separate erase -- calling it
 * again for the same y is exactly what the full redraw loop already does per row. */
static void ctr_row_repaint(GbSession* s, GbGame g, const int* ctr_rows, int ctr_n,
                            int top, int r, int sel) {
  if (r < 0 || r >= ctr_n) return;
  int i = r - top; if (i < 0 || i >= 14) return;
  ctr_row_paint(s, g, ctr_rows[r], 16 + i * 9, r == sel);
}

/* Repaint just row r of the FLAGS list in place -- the pick_species partial-redraw
 * idea (pdna_main.c:4917-4927), so a cursor move no longer flashes the whole list.
 * nf_draw_row() does NOT self-wipe on the unselected path (only the `sel` branch
 * paints a panel), so this erases the 9-px band first: proportional text means a
 * shorter new string would otherwise leave the old one's tail on screen. */
static void nf_row_repaint(GbSession* s, GbGame g, int top, int r, int sel) {
  if (r < top || !nf_visible(r)) return;
  int drawn = 0;
  for (int i = top; i < r; i++) if (nf_visible(i)) drawn++;
  if (drawn >= 14) return;                              /* below the window */
  int y = 26 + drawn * 9;
  ui_fill_rect(0, y - 1, UI_SCR_W, 9, UI_BG);
  nf_draw_row(s, g, r, y, r == sel);
}

/* Raw flag browser: mirrors pdna_main.c's flags_raw_view() over the WHOLE
 * bitfield (2560 Gen 1 / 2048 Gen 2 bits) -- same one-time CAUTION, shared with
 * the named list above (`warned` is the SAME bool both paths take by reference). */
static void raw_flag_browser(GbSession* s, GbGame g, bool* dirty, bool* warned, bool can_edit) {
  uint16_t region_bits = gbf_len(g, s->gen == GB_GEN1 ? GBF_EVENT_FLAGS_BASE : GBF_EVENT_FLAGS_BASE_G2);
  int N = (int)region_bits * 8;
  int flagn = 0;
  for (;;) {
    if (flagn >= N) flagn = N - 1; if (flagn < 0) flagn = 0;
    ui_clear();
    ui_text(4, 2, UI_TITLE, "RAW FLAGS");
    ui_text(6, 14, UI_WARN, "Raw flags can break a save!");
    ui_hline(0, 24, UI_SCR_W, UI_BORDER);
    for (int i = -6; i <= 6; i++) {
      int fn = flagn + i;
      bool valid = (fn >= 0 && fn < N);
      char row[24]; uint16_t ink = UI_DIM;
      if (valid) {
        bool on = gbfl_get(s, g, (uint16_t)fn);
        siprintf(row, "#%-5d %s", fn, on ? "ON" : "off");
        ink = (i == 0) ? UI_SELTEXT : (on ? UI_OK : UI_DIM);
      } else row[0] = 0;
      int y = 78 + i * 9;
      if (i == 0) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(8, y, ink, row);
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, "A toggle  U/D  SEL jump#  B back");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   { if (flagn > 0) flagn--; }
    else if (k & KEY_DOWN) flagn++;
    else if (k & KEY_SELECT) flagn = (int)num_entry("FLAG #", (uint32_t)flagn, (uint32_t)(N - 1));
    else if (k & KEY_A) {
      if (!can_edit) { snd_deny(); continue; }
      if (!*warned) { msg_wait("CAUTION", UI_WARN, "Toggling story flags can", "soft-lock the save."); *warned = true; }
      GbsStatus st = gbfl_set(s, g, (uint16_t)flagn, !gbfl_get(s, g, (uint16_t)flagn));
      if (st == GBS_OK) *dirty = true;
    }
  }
}

/* ============================================================================
 * ---- the shell: two tabs, L/R swaps (mirrors data_editor_tab exactly) -------
 * ============================================================================ */

void pdna_gbflags(GbSession* s, bool can_edit) {
  if (!s || !s->open) { msg_wait("FLAGS", UI_WARN, "Could not read this save.", 0); return; }
  if (!flg_tail_acquire()) {
    /* g_ed is guaranteed non-NULL here (the only caller, gb_nav_from_start, gates on
     * it) -- a refusal means the arena tail is genuinely full, not a missing
     * precondition. Honest refusal, no dead end, same posture as gbscr_open()'s own
     * `reason` fallback path on the art-shell screens. */
    msg_wait("FLAGS", UI_WARN, "Not enough memory right now.", "Try again after a fresh boot.");
    return;
  }
  GbGame g = gbt_game(s);
  s_gbfl_folded = 0xFFFFFFFFu;   /* fresh visit: fully collapsed, same as pdna_main.c's own screen */

  int ctr_rows[CTR_KIND_MAX]; int ctr_n = ctr_build_rows(g, ctr_rows);
  int tab = 0;                  /* 0 = COUNTERS, 1 = FLAGS */
  int sel = 0, top = 0;
  bool dirty = false, flag_warned = false;

  /* Cursor-move partial-redraw shadow, same idiom as pdna_main.c's data_editor_tab()
   * (c_top/c_sel/c_valid/c_gen and f_top/f_sel/f_valid/f_fold there). Two tabs are
   * BOTH reachable in this one call (L/R toggles between them), so each tab keeps its
   * own shadow and the L/R handler below invalidates both on a switch -- a stale
   * match on the just-switched-to tab's shadow would skip repainting the tab strip. */
  int c_top = -1, c_sel = -1; bool c_valid = false; uint32_t c_gen = 0;   /* tab 0 */
  int f_top = -1, f_sel = -1; bool f_valid = false; uint32_t f_fold = 0; /* tab 1 */

  flg_cache(g);

  for (;;) {
    int total = (tab == 0) ? ctr_n : (s_nc + 1);
    bool part;
    if (tab == 1) {
      if (sel >= total) sel = total - 1; if (sel < 0) sel = 0;
      while (sel > 0 && !nf_visible(sel)) sel--;
      if (sel < top) top = sel;
      else { int cnt = 0; for (int r = top; r <= sel; r++) if (nf_visible(r)) cnt++;
             while (cnt > 14) { int nt = nf_step(total, top, +1); if (nt == top) break; top = nt; cnt--; } }
      if (!nf_visible(top)) top = nf_step(total, top, +1);
      part = f_valid && top == f_top && s_gbfl_folded == f_fold;
    } else {
      if (sel >= ctr_n) sel = ctr_n > 0 ? ctr_n - 1 : 0;
      if (sel < top) top = sel; if (sel >= top + 14) top = sel - 13;
      part = c_valid && top == c_top && c_gen == ui_clear_gen();
    }

    if (!part) {
      ui_clear();
      static const char* const TAB[2] = { "COUNTERS", "FLAGS" };
      for (int t = 0; t < 2; t++) {
        int x = 4 + t * 80; bool ts = (t == tab);
        if (ts) ui_panel(x, 0, 76, 12, UI_SEL, UI_TITLE);
        ui_text(x + 6, 2, ts ? UI_SELTEXT : UI_DIM, TAB[t]);
      }
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    }

    if (tab == 0) {
      if (!part) {
        for (int i = 0; i < 14 && top + i < ctr_n; i++)
          ctr_row_paint(s, g, ctr_rows[top + i], 16 + i * 9, top + i == sel);
        ui_text(4, 152, UI_DIM, "A edit  U/D  L/R tab  B done");
      } else if (sel != c_sel) {                  /* cursor-only change: swap the highlight */
        ctr_row_repaint(s, g, ctr_rows, ctr_n, top, c_sel, sel);
        ctr_row_repaint(s, g, ctr_rows, ctr_n, top, sel, sel);
      }
      c_top = top; c_sel = sel; c_valid = true; c_gen = ui_clear_gen();
    } else {
      if (part) {                                 /* cursor-only change: repaint two rows,
                                                     * always the current one too (a toggle
                                                     * writes straight to the save with no
                                                     * overlay, so ON/off can change with
                                                     * `sel` unmoved) */
        if (f_sel != sel) nf_row_repaint(s, g, top, f_sel, sel);
        nf_row_repaint(s, g, top, sel, sel);
      } else {
        ui_text(6, 15, UI_DIRCLR, "Named flags");
        for (int drawn = 0, r = top; drawn < 14 && r < total; r++) {
          if (!nf_visible(r)) continue;
          nf_draw_row(s, g, r, 26 + drawn * 9, r == sel); drawn++;
        }
        ui_text(4, 152, UI_DIM, "A toggle/fold  SEL jump  L/R");
      }
      f_valid = true; f_top = top; f_fold = s_gbfl_folded; f_sel = sel;
    }

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) break;
    else if (k & (KEY_L | KEY_R)) { tab = tab ? 0 : 1; sel = 0; top = 0; c_valid = false; f_valid = false; }
    else if (tab == 1) {
      if (k & KEY_UP)        sel = nf_step(total, sel, -1);
      else if (k & KEY_DOWN) sel = nf_step(total, sel, +1);
      else if (k & KEY_SELECT) {
        snd_tab();
        int r2 = sel;
        for (int step = 0; step < total; step++) {
          r2 = (r2 + 1) % total;
          if (r2 == s_nc || (r2 < s_nc && s_nf[r2].num == NAMED_FLAG_HEADER)) { sel = r2; break; }
        }
        top = 0;
      } else if (k & KEY_A) {
        /* Every branch below that pops a full-screen panel (msg_wait/app_confirm/
         * raw_flag_browser) sets f_valid = false: those panels ui_clear() over the
         * whole list, so the partial path above must not trust a stale top/fold
         * match on the next iteration and repaint only 1-2 rows -- the #119 class
         * of ghosting the review is briefed to attack. The fold/unfold branch needs
         * no explicit invalidation: it mutates s_gbfl_folded, which `part`'s own
         * fold-equality check already catches. */
        if (sel == s_nc) { raw_flag_browser(s, g, &dirty, &flag_warned, can_edit); f_valid = false; }
        else if (s_nf[sel].num == NAMED_FLAG_HEADER) {
          snd_tab();
          s_gbfl_folded ^= 1u << ff_hdr_ord(s_ord, GBFL_ROW_CAP, sel);
        } else if (!can_edit) {
          snd_deny();
        } else {
          uint8_t kind = s_kind[sel];
          if (kind == GBFL_KIND_BAG_GRANT) {
            msg_wait("BAG ITEM", UI_OK, "Grant this from the Bag", "screen, not here."); f_valid = false;
          } else if (kind == GBFL_KIND_READONLY) {
            msg_wait("STORY FLAG", UI_DIM, "This is a display-only", "progress flag."); f_valid = false;
          } else {
            if (!flag_warned) { msg_wait("CAUTION", UI_WARN, "Toggling story flags can", "soft-lock the save."); flag_warned = true; f_valid = false; }
            bool proceed = true;
            if (kind == GBFL_KIND_WARN) {
              bool now_on = gbfl_get(s, g, s_nf[sel].num);
              proceed = now_on ? app_confirm("Remove Kanto power?", "Kanto becomes unreachable.")
                                : app_confirm("Restore power to Kanto?", "Lets Kanto be reached early.");
              f_valid = false;
            }
            if (proceed) {
              GbsStatus st = gbfl_set(s, g, s_nf[sel].num, !gbfl_get(s, g, s_nf[sel].num));
              if (st == GBS_OK) dirty = true;
              else { msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(st), 0); f_valid = false; }
            }
          }
        }
      }
    } else if (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN)   { if (sel + 1 < ctr_n) sel++; }
    else if (k & KEY_A) {
      if (!can_edit) { snd_deny(); continue; }
      ctr_edit_row(s, g, ctr_rows[sel], &dirty);
    }
  }

  /* Every path from here on releases the tail slice exactly once (gb12_arena_tail's
   * own "lent one slice at a time" contract) before returning -- no naked `return`
   * past this point. */
  if (!dirty) { flg_tail_release(); return; }
  if (!app_confirm("Save data changes?", "Edits write immediately.")) { flg_tail_release(); return; }
  GbsStatus fst = gbs_finish(s);
  if (fst != GBS_OK) {
    gb_rollback();
    snd_error();
    msg_wait("EDIT REFUSED", UI_WARN, gbs_status_text(fst), "Nothing was changed.");
    flg_tail_release();
    return;
  }
  gb_persist("gbflags");
  flg_tail_release();
}
