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
#include "gen3_box.h"      /* pk_box_name, G3_TOTAL_BOXES/G3_IN_BOX */
#include "data_tables.h"   /* pk_species_name */
#include "ui.h"
#include "snd.h"
#include "rmbl.h"
#include "pdna_app.h"

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
 * row's label re-decodes its record from sb1/pc via redecode() on each keypress
 * (<=12 on-screen rows redrawn per frame, pick_list blocks in s_wait between them --
 * cheap, not a hot loop). */
typedef struct { uint8_t slot; } PickRow;

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
 * and once more for the chosen row on A. */
static bool redecode(const uint8_t* sb1, const uint8_t* pc, int source, const PickRow* row,
                     PkMon* out) {
  if (source < 0) return pk_decode_mon(sb1 + SB1_OFF_PARTY + (uint32_t)row->slot * 100, true, out);
  const uint8_t* recs = pc + 0x0004 + (uint32_t)source * G3_IN_BOX * 80;
  return pk_decode_mon(recs + (uint32_t)row->slot * 80, false, out);
}

/* A short scrolling text list: `n` rows, `label(i)` fills up to 27 chars. Returns the
 * chosen index, or -1 on B. VIS caps the on-screen rows; a bounded loop (n <= 30 here,
 * the widest caller is a full PC box). */
#define PICK_VIS 12
static int pick_list(const char* title, int n, void (*label)(int i, char* out, int cap),
                     void* ctx) {
  (void)ctx;
  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + PICK_VIS) top = sel - PICK_VIS + 1;
    ui_clear();
    ui_text(4, 2, UI_TITLE, title);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);
    if (n == 0) ui_text(8, 24, UI_DIM, "(nothing here)");
    for (int i = 0; i < PICK_VIS && top + i < n; i++) {
      char b[32];
      label(top + i, b, sizeof b);
      ui_text_sel(4, 14 + i * UI_ROW_H, 232, top + i == sel, UI_TEXT, b);
    }
    ui_hline(0, UI_SCR_H - 10, UI_SCR_W, UI_BORDER);
    ui_text(4, UI_SCR_H - 8, UI_DIM, "A pick  B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    if (k & KEY_A) return n ? sel : -1;
    if (k & KEY_UP)   sel = (sel == 0) ? (n ? n - 1 : 0) : sel - 1;
    if (k & KEY_DOWN) sel = (n == 0) ? 0 : (sel + 1) % n;
  }
}

static const uint8_t* g_pick_pc;
static void source_label(int i, char* out, int cap) {
  if (i == 0) { siprintf(out, "Party"); return; }
  char nm[12];
  pk_box_name(g_pick_pc, i - 1, nm);
  siprintf(out, "%.*s", cap - 1, nm[0] ? nm : "Box");
}

static PickRow*      g_pick_rows;
static const uint8_t* g_pick_sb1;
static int            g_pick_source;
static void mon_label(int i, char* out, int cap) {
  PkMon m;
  if (!redecode(g_pick_sb1, g_pick_pc, g_pick_source, &g_pick_rows[i], &m)) {
    siprintf(out, "?"); return;
  }
  siprintf(out, "%-11s%.*s", pk_species_name(m.species), cap - 12,
          m.nickname[0] ? m.nickname : "");
}

/* Returns true and fills *out on a real pick; false on B at either level. */
static bool pick_donor(const uint8_t* sb1, const uint8_t* pc, PkMon* out) {
  for (;;) {
    g_pick_pc = pc;
    int src = pick_list("PICK A POKEMON — SOURCE", 1 + G3_TOTAL_BOXES, source_label, 0);
    if (src < 0) return false;
    int source = src - 1;                        /* -1 = party */
    static EWRAM_BSS PickRow rows[30];            /* 30 B: EWRAM, not the IWRAM stack */
    int n = list_source(sb1, pc, source, rows);
    g_pick_rows = rows; g_pick_sb1 = sb1; g_pick_source = source;
    int m = pick_list(source < 0 ? "PICK A POKEMON — PARTY" : "PICK A POKEMON — BOX",
                      n, mon_label, 0);
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
      if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
      PkMon donor;
      if (!pick_donor(sb1, pc, &donor)) continue;
      char l1[48]; siprintf(l1, "Show %s as %s winner?", pk_species_name(donor.species), CAT_NAME[sel]);
      if (!app_confirm("SET PAINTING", l1)) continue;
      uint32_t off = gc_museum_offset(game, sel);
      uint8_t before[GC_RECORD_BYTES]; memcpy(before, sb1 + off, GC_RECORD_BYTES);
      write_museum(sb1, game, sel, &donor);
      if (memcmp(before, sb1 + off, GC_RECORD_BYTES) != 0) {
        rmbl_fire(RCUE_SAVE);
        app_commit_sb1();
      }
    }
  }
}
