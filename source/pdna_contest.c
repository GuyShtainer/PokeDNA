/*
 * CONTESTS — the Lilycove Art Museum's 5 paintings (pick who's shown) + the Contest
 * Hall's recent winners (view only). RSE only; see gen3_contest.h for the exact
 * SaveBlock1 layout this reads/writes and the museum-record caption-id subtlety.
 *
 * Picking a painting's Pokemon reuses the save's own party/PC data (a small built-in
 * two-level list picker: source, then mon) rather than the full box screen — pdna_box()
 * is a stateful editor screen (drag/drop, wallpapers, deferred PC-dirty moves) with no
 * "pick one and return" mode, and adding one would risk its own, much bigger surface.
 * This picker only ever READS party/PC records to copy identity fields out of them; it
 * never mutates g_pc.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"           /* EWRAM_BSS */
#include "pdna_contest.h"
#include "gen3_contest.h"
#include "gen3_save.h"     /* SB1_OFF_PARTY(_COUNT) */
#include "gen3_mon.h"
#include "gen3_box.h"      /* pk_box_name, G3_TOTAL_BOXES/G3_IN_BOX, pk_resolve */
#include "data_tables.h"   /* pk_species_name */
#include "ui.h"
#include "snd.h"
#include "rmbl.h"
#include "pdna_app.h"
#include "pdna_pick.h"     /* pick_rows, PR_SORTABLE/PR_ROWH9/PR_ROWH26 (BACKLOG #107) */
#include "pdna_layout.h"   /* PDNA_FILT_*, PDNA_PR_ICON_* -- shared with the host textfit test */
#include "mon_icons.h"     /* mon_icon_for, mon_icon_for_form_frame */

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  else if (k & KEY_A)      snd_ok();
  else if (k & KEY_B)      snd_back();
  return k;
}

static const char* const CAT_NAME[GC_CATEGORY_COUNT] = { "Cool", "Beauty", "Cute", "Smart", "Tough" };
/* GcWinner.rank is CONTEST_RANK_* (gen3_contest.h), not the ribbon word's GC_RANK_*:
 * no "None" -- a winner record only exists once something has won -- and index 4 is
 * a real Link Contest win, not an overflow value. */
static const char* const RANK_NAME[5] = { "Normal", "Super", "Hyper", "Master", "Link" };

/* ---- the donor picker: source (party / one of 14 boxes), then a mon in it -------
 *
 * PickRow caches NOTHING decoded, only `slot` (which record to re-decode) -- caching
 * species+nickname per row (14 B) put rows[30] at 420 B, the whole -424 B this slice
 * took EWRAM below the artless bar; there is no arena buffer available here to grow
 * into (app_arena_acquire hands out g_pc, the very buffer this picker walks). Every
 * visible row's content re-decodes its record from sb1/pc via redecode() on each
 * repaint (mon_row for drawing, mon_key for search/sort -- at most 5-15 on-screen
 * rows per pick_rows() page, blocking in s_wait between them -- cheap, not a hot
 * loop; BACKLOG #107 moved this picker onto pdna_pick.c's shared pick_rows() engine,
 * PickRow's 1-byte shape is unchanged). */
typedef struct { uint8_t slot; } PickRow;
_Static_assert(sizeof(PickRow) == 1, "PickRow must stay 1 byte -- see the header comment above");

/* List up to G3_IN_BOX (30) occupied slots of `source` (-1 = party, 0..13 = box) into
 * `rows`; returns how many. Bounded loop — party is <=6, a box is exactly G3_IN_BOX. */
static int list_source(const uint8_t* sb1, const uint8_t* pc, int source, PickRow rows[30]) {
  int n = 0;
  if (source < 0) {
    int cnt = sb1[SB1_OFF_PARTY_COUNT];
    if (cnt > G3_PARTY_SIZE) cnt = G3_PARTY_SIZE;
    for (int i = 0; i < cnt; i++) {
      PkMon m;
      if (pk_decode_mon(sb1 + SB1_OFF_PARTY + (uint32_t)i * 100, true, &m) && m.species)
        rows[n++].slot = (uint8_t)i;
    }
  } else {
    const uint8_t* recs = pc + 0x0004 + (uint32_t)source * G3_IN_BOX * 80;
    for (int i = 0; i < G3_IN_BOX; i++) {
      PkMon m;
      if (pk_decode_mon(recs + (uint32_t)i * 80, false, &m) && m.species)
        rows[n++].slot = (uint8_t)i;
    }
  }
  return n;
}

/* Re-decode the record a PickRow points at — called for every visible row's label,
 * and once more for the chosen row on A. pk_resolve() fills level (box records store
 * only experience; party carries plaintext level and pk_resolve() no-ops on it) and
 * gender -- BACKLOG #107's mon_row wants level to show "Lv.NN". */
static bool redecode(const uint8_t* sb1, const uint8_t* pc, int source, const PickRow* row,
                     PkMon* out) {
  bool ok;
  if (source < 0) ok = pk_decode_mon(sb1 + SB1_OFF_PARTY + (uint32_t)row->slot * 100, true, out);
  else { const uint8_t* recs = pc + 0x0004 + (uint32_t)source * G3_IN_BOX * 80;
         ok = pk_decode_mon(recs + (uint32_t)row->slot * 80, false, out); }
  if (ok) pk_resolve(out);
  return ok;
}

/* page_hall (view-only Contest Hall list, below) keeps its own plain scrolling loop --
 * it is a read-only list of AT MOST GC_HALL_MAX records (well under a screenful), not
 * a picker, so it has no use for pick_rows' search/sort/paging chrome. PICK_VIS caps
 * its on-screen rows. */
#define PICK_VIS 12

/* ---- the donor picker's two levels, now on pick_rows (BACKLOG #107) -------------
 * SourceCtx/MonCtx carry state through pick_rows' opaque `ctx` -- no file-statics,
 * same convention pdna_pick.c's own list_pick wrapper uses. The icon column (mon_row,
 * below) reads mon_icon_for_form_frame() directly -- Tier A only, NEVER
 * icon_store_borrow(): `pc` here is g_pc itself (pdna_main.c's NV_CONTEST case passes
 * g_pc straight through, not an app_arena_acquire() copy -- see this file's own top-of-
 * file comment: "never mutates g_pc", and pdna_pick.c's icon_store.h documents that
 * Tier B's borrow literally reuses g_pc's memory for icon rows), so borrowing while
 * this screen's redecode() calls are still reading `pc` would overwrite the very
 * records being displayed. */
typedef struct { const uint8_t* pc; } SourceCtx;

static void source_row(int i, int y, bool sel, void* vctx) {
  SourceCtx* c = (SourceCtx*)vctx;
  char b[16];
  if (i == 0) siprintf(b, "Party");
  else { char nm[12]; pk_box_name(c->pc, i - 1, nm); siprintf(b, "%.*s", (int)sizeof(nm) - 1, nm[0] ? nm : "Box"); }
  if (sel) ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_SEL);
  else     ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_BG);
  ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, b);
}

typedef struct {
  const uint8_t* sb1;
  const uint8_t* pc;
  int source;             /* -1 = party, else box index */
  const PickRow* rows;
  bool icon_ok;            /* mon_icon_for(1) != 0 -- real art linked, not the artless build */
  char keybuf[24];         /* mon_key()'s returned string lives HERE, not a file static */
} MonCtx;

/* Species + nickname, for search (nickname or species name) and, doubling as the sort
 * key (pick_rows' own contract), A-Z order. */
static const char* mon_key(int i, void* vctx) {
  MonCtx* c = (MonCtx*)vctx;
  PkMon m;
  if (!redecode(c->sb1, c->pc, c->source, &c->rows[i], &m)) { c->keybuf[0] = 0; return c->keybuf; }
  siprintf(c->keybuf, "%s %s", pk_species_name(m.species), m.nickname);
  return c->keybuf;
}

/* Species + level, with a 24x24 icon column when real art is linked (mon_icon_for(1)
 * != 0); the artless build falls back to the same plain FILT row geometry source_row
 * above uses -- no icon slot to fill, so no chip either, just the text (pick_species'
 * own `lst` fallback, pdna_pick.c:430, is the same idiom: no icon capability -> plain
 * list, not a placeholder graphic). */
static void mon_row(int i, int y, bool sel, void* vctx) {
  MonCtx* c = (MonCtx*)vctx;
  PkMon m;
  bool ok = redecode(c->sb1, c->pc, c->source, &c->rows[i], &m);
  char row[40], rt[40];
  if (!ok) siprintf(row, "?");
  else if (m.nickname[0] && strcmp(m.nickname, pk_species_name(m.species)) != 0)
    siprintf(row, "%s Lv.%u (%s)", pk_species_name(m.species), (unsigned)m.level, m.nickname);
  else
    siprintf(row, "%s Lv.%u", pk_species_name(m.species), (unsigned)m.level);

  if (c->icon_ok) {
    if (sel) ui_panel(PDNA_FILT_BAR_X, y - 1, PDNA_FILT_BAR_W, PDNA_PR_ICON_PANEL, UI_SEL, UI_TITLE);
    else     ui_fill_rect(PDNA_FILT_BAR_X, y - 1, PDNA_FILT_BAR_W, PDNA_PR_ICON_PANEL, UI_BG);
    if (ok) {
      /* #344 audit: SAFE (plan-less by design) -- the nav switch retires the box plan AND its pins (icon_store_borrow(false)) before this screen opens, so every row here is an ordinary off-plan fill. */
      const uint16_t* ic = mon_icon_for_form_frame(m.species, m.form, 0);
      if (ic) ui_icon_scaled(PDNA_PR_ICON_X, y, PDNA_PR_ICON_W, PDNA_PR_ICON_W, ic);
    }
    ui_truncate(rt, row, PDNA_PR_ICON_MAXCOLS);
    ui_text(PDNA_PR_ICON_TEXT_X, y + PDNA_PR_ICON_TEXT_DY, sel ? UI_SELTEXT : UI_TEXT, rt);
  } else {
    if (sel) ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_SEL);
    else     ui_fill_rect(PDNA_FILT_BAR_X, y + PDNA_FILT_BAR_DY, PDNA_FILT_BAR_W, PDNA_FILT_BAR_H, UI_BG);
    ui_truncate(rt, row, 28);
    ui_text(PDNA_FILT_TEXT_X, y, sel ? UI_SELTEXT : UI_TEXT, rt);
  }
}

/* Returns true and fills *out on a real pick; false on B at either level. */
static bool pick_donor(const uint8_t* sb1, const uint8_t* pc, PkMon* out) {
  for (;;) {
    SourceCtx sc = { pc };
    int src = pick_rows("PICK A POKEMON - SOURCE", 1 + G3_TOTAL_BOXES, 0,
                        source_row, &sc, 0, PR_ROWH9);
    if (src < 0) return false;
    int source = src - 1;                        /* -1 = party */
    static EWRAM_BSS PickRow rows[30];            /* 30 B: EWRAM, not the IWRAM stack */
    int n = list_source(sb1, pc, source, rows);
    MonCtx mc = { sb1, pc, source, rows, mon_icon_for(1) != 0, "" };
    int opts = PR_SORTABLE | (mc.icon_ok ? PR_ROWH26 : PR_ROWH9);
    int m = pick_rows(source < 0 ? "PICK A POKEMON - PARTY" : "PICK A POKEMON - BOX",
                      n, 0, mon_row, &mc, mon_key, opts);
    if (m < 0) continue;                          /* B here: back to source list */
    if (!redecode(sb1, pc, source, &rows[m], out)) continue;   /* should not happen: it was just listed */
    return true;
  }
}

/* ---- confirm + write ------------------------------------------------------------- */

static void write_museum(uint8_t* sb1, PkGame g, int cat, const PkMon* donor) {
  gc_museum_set(sb1, g, cat, donor->species, donor->personality, donor->otId,
               donor->nickname, donor->otName);
}

/* ---- the two-page screen: MUSEUM (editable) / HALL (view only) ------------------ */

static void page_museum(const uint8_t* sb1, PkGame g, int sel) {
  ui_text(4, 2, UI_TITLE, "CONTESTS - MUSEUM");
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);
  for (int i = 0; i < GC_CATEGORY_COUNT; i++) {
    GcWinner w;
    gc_museum_get(sb1, g, i, &w);
    char b[40];
    if (w.species) siprintf(b, "%-8s%-11s%s", CAT_NAME[i], pk_species_name(w.species),
                            RANK_NAME[w.rank > 4 ? 4 : w.rank]);
    else            siprintf(b, "%-8s(empty)", CAT_NAME[i]);
    ui_text_sel(4, 16 + i * (UI_ROW_H + 4), 232, i == sel, UI_TEXT, b);
  }
  ui_hline(0, UI_SCR_H - 10, UI_SCR_W, UI_BORDER);
  ui_text(4, UI_SCR_H - 8, UI_DIM, "A pick  L/R page  B back");
}

static void page_hall(const uint8_t* sb1, PkGame g) {
  ui_text(4, 2, UI_TITLE, "CONTESTS - HALL");
  ui_hline(0, 11, UI_SCR_W, UI_BORDER);
  int n = gc_hall_count(g);
  for (int i = 0; i < n && i < PICK_VIS; i++) {
    GcWinner w;
    gc_hall_get(sb1, g, i, &w);
    char b[40];
    if (w.species) {
      const char* cn = (w.category < GC_CATEGORY_COUNT) ? CAT_NAME[w.category] : "?";
      siprintf(b, "%-7s%-11s%s", cn, pk_species_name(w.species),
              RANK_NAME[w.rank > 4 ? 4 : w.rank]);
    } else siprintf(b, "(empty)");
    ui_text(4, 14 + i * UI_ROW_H, UI_TEXT, b);
  }
  ui_hline(0, UI_SCR_H - 10, UI_SCR_W, UI_BORDER);
  ui_text(4, UI_SCR_H - 8, UI_DIM, "L/R page  B back");
}

void pdna_contest(uint8_t* sb1, uint8_t* pc, PkGame game) {
  if (!gc_supported(game)) {
    msg_wait("CONTESTS", UI_DIM, "FireRed and LeafGreen", "have no Contests.");
    return;
  }
  int page = 0, sel = 0;
  for (;;) {
    ui_clear();
    if (page == 0) page_museum(sb1, game, sel); else page_hall(sb1, game);
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B);
    if (k & KEY_B) break;
    else if (k & (KEY_L | KEY_R)) page ^= 1;
    else if (page == 0 && (k & KEY_UP))   sel = (sel == 0) ? GC_CATEGORY_COUNT - 1 : sel - 1;
    else if (page == 0 && (k & KEY_DOWN)) sel = (sel + 1) % GC_CATEGORY_COUNT;
    else if (page == 0 && (k & KEY_A)) {
      if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); continue; }
      PkMon donor;
      if (!pick_donor(sb1, pc, &donor)) continue;
      char l1[48]; siprintf(l1, "Show %s as %s winner?", pk_species_name(donor.species), CAT_NAME[sel]);
      if (!app_confirm("SET PAINTING", l1)) continue;
      uint32_t off = gc_museum_offset(game, sel);
      uint8_t before[GC_RECORD_BYTES]; memcpy(before, sb1 + off, GC_RECORD_BYTES);
      write_museum(sb1, game, sel, &donor);
      if (memcmp(before, sb1 + off, GC_RECORD_BYTES) != 0) {
        rmbl_fire(RCUE_SAVE);
        (void)app_hold_sb1("Contest painting");     /* #234 s2: staged (the SET PAINTING confirm above stays) */
      }
    }
  }
}
