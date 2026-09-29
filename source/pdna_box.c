/*
 * Game-faithful PC box screen for PokeDNA (mimics the Gen-3 PC).
 * Left "PKMN DATA" panel (front sprite + name/No/Lv/gender/item) + a 6x5 grid of
 * 32x32 box-icon sprites on a colored wallpaper, with a ◄ box-name ► banner.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"            /* EWRAM_BSS (after tonc.h so u8 macro doesn't clash) */
#include "pdna_box.h"
#include "xfer_gate.h"      /* BACKLOG #120 S2: xg_drop_denied gates cross-generation drops */
#include "bank_down_convert.h" /* BACKLOG #174 (S150-8c): bank_down_convert_gen3_party */
_Static_assert(BOXSCOPE_GB == 2, "source/xfer_gate.c's XG_SCOPE_GB hard-codes 2 for "
               "BOXSCOPE_GB -- keep them in step or the cross-generation drop deny "
               "silently stops firing");
_Static_assert(BOXSCOPE_BANK == 1, "source/xfer_gate.c's XG_SCOPE_BANK hard-codes 1 for "
               "BOXSCOPE_BANK -- keep them in step or the native-cell escape gate "
               "silently stops firing (BACKLOG #150 S150-3)");
#include "ui.h"
#include "pdna_layout.h"    /* PDNA_PCP_*: the PC-box party strip's retail-measured geometry */
#include "tab_focus_footer.h" /* BACKLOG #173 F2: pure tab_focus_footer() decision */
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"      /* em_set_item: held-item moves in ITEM cursor mode */
#include "data_tables.h"
#include "mon_front.h"
#include "mon_icons.h"
#include "mon_icons_gate.h"  /* PDNA_MON_ICONS_ART_COMPILED -- see pcp_declare_party */
#include "art_icons_cache.h" /* art_icons_row_for -- species -> icon-store row        */
#include "icon_store.h"      /* the party panel declares its 6 rows before it paints  */
#include "mon_icons_oam.h"   /* artless probe: are the OAM icons compiled in? */
#include "item_icons.h"     /* item_icon_for: held-item markers in ITEM mode */
#include "box_oam.h"        /* hardware-OAM icon/cursor/carry/marker rendering */
#include "pdna_origin_art.h" /* THE BANK IN PARALLEL: each cell in the art of its own era */
#include "gb_art_source.h"   /* BACKLOG #263: GbArtBatch -- one ROM open + table validation per era pass */
#include "sprite_era.h"      /* SePlace -- pdna_origin_art_set_place() on entry (E4) */
#include "pdna_summary.h"
#include "perf.h"        /* screen-enter spans + the box-load/bob rollups (telemetry) */
#include "pdna_app.h"
#include "snd.h"
#include "osk.h"
#include "rumble.h"         /* rumble_io_suspend/resume: mute the motor while the wallpaper blit reads ROM */
#include "log.h"            /* log_line: wallpaper self-verify diagnostics */
#include "artbuf.h"         /* mon_decomp: shared 8 KiB scratch, borrowed by the ROM wallpaper rung */
#include "rom_wallpaper.h"  /* the §12c ROM-live wallpaper rung (a registered/fused ROM) */
#include "gen3_chunk.h"     /* Chunk: Emerald-style rubber-band multi-select geometry */
#include "pdna_progress.h"  /* pdna_progress_frame: batch sprite + N/total bar */
#include "pdna_pk.h"        /* pdna_pk_export_silent: "Export all" from the box title */
#include "bank_cell.h"      /* bc_is_native/bc_unpack/bc_view/bc_ident32 -- native Bank cell render (BACKLOG #150 S150-2) */
#include "pdna_gen12.h"     /* BACKLOG #150 S150-8: BankDownResult, bank_down_convert_gb/gen3 */
#include "gb12_render.h"    /* gb12_render_rec/GB_SHOW_* -- shared display ladder */
#include "pdna_bank.h"      /* pdna_bank_prepare_native -- the UP drop's backup gate (BACKLOG #150 S150-4) */
#include "bank_collision.h" /* BACKLOG #168a: drop_held's UP-branch 16-box ident32 collision scan */
#include "gb_sidecar.h"     /* GbscEntry, gbsc_count/gbsc_get/gbsc_set_claimed -- the RESTORE edge's ledger (BACKLOG #150 S150-8b) */
#include "xfer_io.h"        /* xr_open */
#include "xfer_rec.h"       /* xr_key_g3 */
#include "bank_restore.h"   /* bank_restore_from_entry */
#include "savefile.h"       /* SfStatus, sf_write_verified, sf_status_str */
/* item_map_g2g3.h's item_g2_to_g3 moved into xfer_rec.c's xr_merge_down_sel
 * (BACKLOG #150 S150-9 decision 5 -- the g3_item probe that used to live here is
 * now rep->abroad_item_dropped); no longer needed in this file. */

#define COLS 6
#define ROWS 5
#define CELL_W 24
#define CELL_H 22
#define GRID_X 82
#define GRID_Y 30
#define PANEL_W 76
#define WP_X 78           /* wallpaper / grid region */
#define WP_Y 12
#define WP_W 162
#define WP_H 141          /* 12..153 */

/* SELECT cursor modes: 0 normal hand, 1 MOVE (orange hand, A grabs a mon directly),
 * 2 ITEM (translucent hand, mons show held items, A picks one up / drops / swaps).
 * The hand's normal/orange/translucent looks are produced by box_oam.c (a palette
 * swap + OBJ alpha-blend), not a per-pixel software blit. */
#define CM_NORMAL 0
#define CM_MOVE   1
#define CM_ITEM   2
static int s_cur_mode = CM_NORMAL;
static int s_item_held = 0;           /* item id carried in ITEM mode (0 = none) */
static int s_tab_focus = -1;          /* top-tab cursor: -1 none, 0 PKMN DATA, 1 PARTY SEL, 2 SAVE */
static int s_item_from = -1;          /* slot the carried item was taken from */
static int s_item_from_box = -1;      /* box the carried item came from (for put-back across boxes) */

static PkMon EWRAM_BSS g_box[30];

/* The DAMAGED stand-in (BACKLOG #150 S150-2, decision 8): a native cell whose
 * gb12_render_rec ladder bottomed out at GB_SHOW_NONE (no decodable Pokemon at all --
 * a genuinely corrupt record, not merely a refused transfer) is shown as species 252
 * ("?" in data_tables.c, an unused Gen-3 slot -- non-zero so app_mon_menu's own
 * `occupied` test never calls the cell EMPTY) with the plaintext isBadEgg bit forced
 * on, exactly the predicate `cell_damaged()` below tests for. `species == 252 &&
 * isBadEgg` is produced by NOTHING else in this tree (review finding 6 sweeps the
 * whole Gen-3 corpus for it). Level is the native level clamped to 1..100, or 1. */
static void bank_damaged_standin(const Gb12Mon* in, uint32_t salt, uint8_t out80[80]) {
  uint8_t lv = in->level;
  if (lv < 1) lv = 1;
  if (lv > 100) lv = 100;
  gen3_build_mon(252, lv, salt, (uint32_t)in->ot_id, in->ot_name, 3, out80);
  EditMon e;
  gen3_edit_load(out80, false, &e);
  em_set_nickname(&e, "DAMAGED");
  gen3_edit_commit(&e, out80);
  out80[0x13] |= 0x01;   /* plaintext isBadEgg bit, OUTSIDE the encrypted/checksummed
                          * 48 bytes (gen3_mon.c:79-80) -- set AFTER gen3_edit_commit,
                          * the same byte the tree already writes by hand elsewhere
                          * (pdna_main.c:6533's e.raw[0x13], a different call shape). */
}

/* A native Bank cell ("GBC1", source/bank_cell.h, merged in S150-1) rendered as the
 * Gen-1/2 Pokemon it is, via the SAME display ladder a GB session's own grid uses
 * (source/gb12_render.h) -- so a native cell and the identical mon in a GB session
 * render identically. `__attribute__((noinline))`: this function's GbEditMon +
 * Gb12Mon + 80-byte scratch must never join a caller's own frame -- box_decode_to's
 * sits on the party-strip chain (#define PDNA_PARTY_STRIP_NEED, pdna_box.c) at
 * effectively zero slack, and app_mon_menu's (pdna_main.c, review F1) is under the
 * same root. `hint` may be NULL.
 *
 * Review F1 (BACKLOG #150 S150-2): hoisted from a pdna_box.c-static `box_native_
 * decode` to a shared, non-static function (declared in pdna_app.h) so app_mon_menu
 * (pdna_main.c) can decode a native cell the SAME way the grid does, instead of
 * running pk_decode_mon on bytes it cannot decrypt (a meaningless-key decrypt that
 * read "??? ? ?" and made `occupied` a coin flip -- a G-H2 violation: CREATE/PASTE
 * HERE could be offered over a cell that is never empty). Definition stays here
 * (not moved to a different .c) because bank_damaged_standin/bc_view/gb12_render_rec
 * all already live in this TU; only the declaration moves to a shared header. */
void __attribute__((noinline)) pdna_native_cell_decode(const uint8_t* cell, PkMon* out, uint8_t* hint) {
  GbEditMon e; BcMeta meta;
  if (!bc_unpack(cell, &e, &meta)) return;       /* leave the plain Gen-3 decode alone */
  uint32_t salt = bc_ident32(cell);
  Gb12Mon in; if (!bc_view(&e, &meta, salt, &in)) return;
  Gb12Target tgt; tgt.met_game = 0;               /* 0 -> Emerald, gen12_convert.h:87; display only.
                                                   * The DOWN edge (S150-8) is where a REAL
                                                   * destination game belongs. */
  uint8_t tmp[80]; uint8_t reason = 0;
  int rung = gb12_render_rec(&in, &tgt, salt, tmp, &reason);
  if (rung == GB_SHOW_NONE) bank_damaged_standin(&in, salt, tmp);
  pk_decode_mon(tmp, false, out);
  pk_resolve(out);   /* pk_decode_box_raw's own convention for every other slot
                      * (gen3_box.c:113) -- level/stats/gender are all DERIVED, never
                      * stored in a box record, and are computed here, not by
                      * pk_decode_mon. Skipping this left every native cell showing
                      * Lv0 (found live: this lane's own mGBA shot) even though the
                      * built record's EXP was correct throughout. */
  out->raw = 0;   /* BACKLOG #46: gen3_mon.c:77 parks a pointer to `tmp`, a local about to
                   * go out of scope -- precedent gen3_edit.c:686-693. */
  if (hint) *hint = meta.gen;   /* 1/2 == PDNA_GEN1/PDNA_GEN2, pdna_origin_art.h:107 */
}

/* Decode a box's raw records for DISPLAY. For the bank, a mon the user already carried out to the
 * PC is deleted from the card only at the save prompt — but it must LOOK gone right away (Guy), so
 * blank those slots here. This is display-only: the raw buffer keeps the record (it's the mon's only
 * on-card copy until the PC is written), so box_save can never persist a half-done move.
 *
 * The native pass (BACKLOG #150 S150-2, G-M6) is scope-agnostic ON PURPOSE (no is_bank
 * test): §11.1's residual says an old build can drag a native cell into a Gen-3 PC box,
 * and this is the one interception point that renders it correctly wherever it sits. It
 * runs BEFORE app_bank_hide_pending (G-M6, non-negotiable) -- a native cell queued for a
 * Bank->PC deferred delete must still draw BLANK, exactly like a plain Gen-3 mon does,
 * which only holds if hide_pending sees (and can blank) the slot the native pass just
 * wrote. `hint` may be NULL. */
static void box_decode_to(BoxSource* src, const uint8_t* recs, int box, PkMon out[G3_BOX_SLOTS], uint8_t* hint) {
  pk_decode_box_raw(recs, out);
  if (hint) memset(hint, 0, G3_BOX_SLOTS);
  for (int s = 0; s < G3_BOX_SLOTS; s++)
    if (bc_is_native(recs + (uint32_t)s * 80)) pdna_native_cell_decode(recs + (uint32_t)s * 80, &out[s], hint ? &hint[s] : 0);
  if (src->is_bank) app_bank_hide_pending(box, out);              /* LAST — pdna_main.c:2158 */
}
static void box_decode(BoxSource* src, const uint8_t* recs, int box) {
  uint8_t hint[G3_BOX_SLOTS];
  box_decode_to(src, recs, box, g_box, hint);
  /* THE ERA CACHE IS FILLED FROM WHAT IS ACTUALLY ON SCREEN. This is the single point
   * where the 30 displayed records change, in the PC and in the bank alike, so it is
   * the one place that can promise "the markers describe the mons you are looking at".
   * Filling it from the RAW records instead (which is where it used to be done, in
   * pdna_bank.c) put a Game Boy marker on a bank slot the user had already carried out
   * to the PC: box_decode_to blanks those for display, and the raw buffer still holds
   * them because that record is the mon's only on-card copy until the PC is written.
   * Cost is 30 record decodes + 30 integer comparisons on a user action, never a frame.
   *
   * _hinted (BACKLOG #150 S150-2): a Bank box is MIXED -- a real Gen-3 mon beside a
   * native Gen-1/2 cell beside an old-style GB-import Gen-3 stand-in, all 30 slots at
   * once -- and no single session-wide hint (pdna_origin_box_set_hint) can describe
   * that, so `hint` (filled by box_decode_to's native pass) carries the per-slot era. */
  pdna_origin_box_note_hinted(g_box, hint);
}

/* Occupancy for DROP targeting. A bank slot pending a Bank->PC deletion looks empty (box_decode
 * hides it) but still physically holds the mon's ONLY on-card copy, so it counts as OCCUPIED —
 * nothing may overwrite it until the PC destination is saved (after that the slot frees for real).
 *
 * `recs` (BACKLOG #150 S150-2, G-H2): occupancy must be provable from the RAW bytes, not
 * from g_box[].species alone -- a native cell that bank_damaged_standin() could not even
 * build a stand-in for (GB_SHOW_NONE with an unrepresentable species) would otherwise read
 * as an empty slot and accept a drop that destroys the ONLY on-card copy of that Game Boy
 * mon. Do NOT call src->records(box) in here: on the Bank that is a paging call that can
 * flush a dirty box (banksrc_records, pdna_bank.c) -- every caller already holds the recs
 * for this exact box and must pass it in. */
static void box_occupancy(BoxSource* src, int box, const uint8_t* recs, uint8_t occ[G3_BOX_SLOTS]) {
  for (int s = 0; s < G3_BOX_SLOTS; s++)
    occ[s] = (g_box[s].species || bc_is_native(recs + (uint32_t)s * 80) ||
             (src->is_bank && app_bank_slot_pending(box, s))) ? 1 : 0;
}

/* Mon-in-hand carry (move mode). The carried mon lives in s_held (a copy); its ORIGIN
 * cell keeps the real record (lift-don't-clear) and is only cleared on a successful drop,
 * so an interrupted carry never loses the mon. s_orig_slot<0 means "no origin": either a
 * fresh DUPLICATE (s_held_dup -> cancel discards it) or a real mon displaced by a swap
 * (s_held_dup=false -> cancel must place it in a free slot; can't cross save scopes).
 * Statics persist across pdna_box runs so a carry survives the PC<->Bank screen hand-off;
 * pdna_box_clear_carry() resets it per save. */
static bool s_holding = false;
static uint8_t s_held[80];
static int  s_orig_box = -1, s_orig_slot = -1;
/* BACKLOG #120 S1: was `s_orig_bank` (bool bank/PC). Renamed + widened to the three-way
 * BOXSCOPE_* discriminator — the real Bank and a raw Game Boy save's own box both set
 * `is_bank` true (a LAYOUT flag, see pdna_box.h), so a plain bool can no longer tell
 * "came from the Bank" from "came from a GB save". Every site that used to compare
 * `s_orig_bank` against `src->is_bank` compares `s_orig_scope` against `src->scope` now. */
static uint8_t s_orig_scope = BOXSCOPE_PC;
static bool s_held_dup = false;   /* the held mon is a fresh, discardable duplicate */
static bool s_orig_party = false; /* the held mon was carried OUT of the party (origin = a party slot,
                                   * s_orig_slot = party index): on a within-PC drop, clear_origin()
                                   * removes it from the party (deferred); cancel returns it (untouched). */
/* ---- Multi-select chunk carry (Emerald rubber-band multi-move) --------------------
 * A chunk is a rectangular selection of one box, lifted as a GROUP. The held 80-byte
 * records live in EWRAM (2400 B); s_ch holds the footprint geometry + each mon's source
 * slot. Like the single-mon carry, the SOURCE cells keep their records (lift-don't-clear)
 * until a successful drop, so a cancelled/interrupted carry loses nothing. Statics persist
 * across pdna_box runs so the chunk survives the PC<->Bank hand-off. s_holding (single) and
 * s_ch_hold (chunk) are mutually exclusive — only one carry is ever active. */
static bool s_ch_hold = false;
static uint8_t EWRAM_BSS s_ch_rec[G3_BOX_SLOTS][80];   /* held records (2400 B) */
static Chunk s_ch;                                      /* footprint + per-mon src slots */
static int  s_ch_box  = -1;                             /* source box index */
static uint8_t s_ch_scope = BOXSCOPE_PC;                /* source scope (was `s_ch_bank`, see s_orig_scope) */
/* BACKLOG #120 S1: the xfer peer a Bank visit reached from a Game Boy session installs
 * around itself (§4 gb_bank_visit) — pdna_box_xfer_set()'s stored pointer. Always NULL in
 * S1 (no caller installs a real peer yet); read by the chunk DOWN edge (§2.3) and by
 * pdna_box_carry_is_gb() so a GB-scope carry never crosses into a foreign grid. */
static const BoxXferOps* s_xfer_peer = 0;

void pdna_box_xfer_set(const BoxXferOps* ops) { s_xfer_peer = ops; }

bool pdna_box_carry_is_gb(void) {
  if (s_holding)  return s_orig_scope == BOXSCOPE_GB;
  if (s_ch_hold)  return s_ch_scope == BOXSCOPE_GB;
  return false;
}

/* One-shot "is this carry still inside the scope it started in" test — every site that
 * used to compare `s_orig_bank`/`s_ch_bank` against a source's `is_bank` now compares
 * scopes instead (§3.1: is_bank stays layout-only, scope is the real discriminator). */
static bool same_scope(const BoxSource* src) { return src->scope == s_orig_scope; }

/* BACKLOG #120 S1: every site that used to gate on `src->can_edit()` directly now goes
 * through this — `can_lift` NULL (every source today: PC, Bank, and the Game Boy source
 * in S1) falls back to can_edit() exactly, so this is a no-op wrapper until a later slice
 * gives the Game Boy source a real `can_lift` (a narrower refusal than the save-wide
 * can_edit(), e.g. one unreadable cell). `slot` is -1 for box-level actions (the title-row
 * menu, the box rename) that have no single cell to name. */
static bool src_can_lift(const BoxSource* src, int box, int slot) {
  return src->can_lift ? src->can_lift(box, slot) : src->can_edit();
}
static int  s_ch_tr = 0, s_ch_tc = 0;                   /* current carry anchor (top-left) */
static int  s_ch_fr = 0, s_ch_fc = 0;                   /* grab fist's footprint-relative cell
                                                         * (where A was released, Emerald-style) */
static int  s_ch_lift = 8;                              /* block float height in px: 8 = carrying;
                                                         * the grab/place beats animate it 0..8 */
static BoxOamChunkMon EWRAM_BSS s_ch_cells[G3_BOX_SLOTS]; /* per-mon display info, decoded ONCE at
                                                           * grab so anchor moves don't re-decode */

void pdna_box_clear_carry(void) {
  s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_scope = BOXSCOPE_PC; s_held_dup = false; s_orig_party = false;
  s_ch_hold = false; s_ch_box = -1; s_ch_scope = BOXSCOPE_PC;
}

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

/* ---- procedural Emerald-PC chrome (colors decoded from the real wallpapers) ---- */

/* a tiny 2-tone leaf sprig for the grass wallpaper */
static void draw_leaf(int x, int y, u16 dk, u16 lt) {
  m3_plot(x + 1, y,     dk);
  m3_plot(x + 2, y,     lt);
  m3_plot(x,     y + 1, dk);
  m3_plot(x + 1, y + 1, lt);
  m3_plot(x + 2, y + 1, dk);
  m3_plot(x + 1, y + 2, dk);
}

/* the forest/grass wallpaper: flat green field + an offset scatter of leaf sprigs */
static void draw_grass(int x, int y, int w, int h) {
  const u16 base = RGB15(19, 25, 12), dk = RGB15(13, 19, 6), lt = RGB15(22, 28, 15);
  ui_fill_rect(x, y, w, h, base);
  for (int j = 0; j + 6 < h; j += 12) {
    int off = ((j / 12) & 1) ? 8 : 0;
    for (int i = off; i + 4 < w; i += 16) draw_leaf(x + i + 2, y + j + 3, dk, lt);
  }
}

static const char* const WP_NAME[G3_BOX_WALLPAPER_COUNT] = {
  "Forest", "City", "Desert", "Savanna", "Crag", "Volcano", "Snow", "Cave",
  "Beach", "Seafloor", "River", "Sky", "Polkadot", "Pokecenter", "Machine", "Plain",
};
/* Emerald "Walda"/secret wallpapers (chooser ids 16..31). */
static const char* const WALDA_NAME[G3_WALDA_COUNT] = {
  "Zigzagoon", "Screen", "Horizontal", "Diagonal", "Block", "Ribbon", "Pokecenter2", "Frame",
  "Blank", "Circles", "Azumarill", "Pikachu", "Legendary", "Dusclops", "Ludicolo", "Whiscash",
};
static const char* wp_name(int id) { return id < 16 ? WP_NAME[id] : WALDA_NAME[id - 16]; }

/* Draw box wallpaper `wp` into the region. Falls back to the procedural grass for
 * any wallpaper without a real generated bitmap (see wallpaper_bmp). */
static void draw_wallpaper(int wp, int x, int y, int w, int h);

/* ---- the parallel era view (definitions live below artless_cells) ------------------
 * Bit s set = cell s is wearing Game Boy era ART in the Mode-3 bitmap and its Gen-3 OBJ
 * icon is hidden. A plain 4-byte .bss word, NOT EWRAM: the hardware build has ~1.5 KB
 * of EWRAM headroom and a post-link guard, and this feature adds none of it. */
static uint32_t s_era_drawn = 0;
static void era_cell_mark(int slot);         /* the always-there era pad (free)        */
static void era_cell_icon_back(int slot);    /* drop this cell's art, un-hide its icon */
static void era_hides_apply(void);           /* re-hide era cells after an OAM reload  */

/* light checkerboard behind the front sprite (the PKMN DATA "monitor") */
static void draw_checker(int x, int y, int w, int h, u16 a, u16 b) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      m3_plot(x + i, y + j, (((i >> 3) ^ (j >> 3)) & 1) ? a : b);
}

/* solid 4x7 triangle arrows (banner ends) */
static void tri_left(int x, int y, u16 c) {
  for (int i = 0; i < 4; i++) for (int j = 3 - i; j <= 3 + i; j++) m3_plot(x + i, y + j, c);
}
static void tri_right(int x, int y, u16 c) {
  for (int i = 0; i < 4; i++) for (int j = 3 - i; j <= 3 + i; j++) m3_plot(x + (3 - i), y + j, c);
}

/* tan rounded box-name banner with arrows. `pfx`, if non-NULL, is a dim box-number
 * ordinal ("3:") drawn immediately ahead of `name` in the banner's own shadow ink, so
 * it reads as an ordinal and not part of the name (Guy, BACKLOG #33). The prefix+name
 * pair is measured and centred TOGETHER as one unit, on the same rect midpoint the
 * name alone used to use -- see the centring comment below for why that invariant
 * holds regardless of how long the prefix is. */
static void draw_banner(int x, int y, int w, const char* pfx, const char* name) {
  const u16 fill = RGB15(30, 27, 14), hi = RGB15(31, 31, 24), sh = RGB15(24, 15, 2),
            bd = RGB15(14, 9, 0), ink = RGB15(8, 5, 0);
  ui_fill_rect(x, y, w, 14, fill);
  ui_hline(x, y, w, hi);
  ui_hline(x, y + 13, w, sh);
  m3_frame(x, y, x + w - 1, y + 13, bd);
  tri_left(x + 5, y + 4, ink);
  tri_right(x + w - 9, y + 4, ink);
  /* Proportional, and centred on its combined width. The glove's fingertip is
   * anchored to the banner's rect midpoint (box_oam.c boxoam_cursor's title_row==1,
   * WP_X + WP_W/2 -- a FIXED point, not derived from the text), so `tx + (pw+tw)/2`
   * must land on `x + w/2` for the hand to keep pointing at the text -- which it does
   * for ANY pw/tw split, the same identity that already made the name-only version
   * correct. `sh` (the banner's own bottom-hairline shadow colour) doubles as the
   * prefix's dim ink: darker than `ink` against the `fill` tan, still legible -- the
   * same colour already proven on-screen as the banner's 1px shadow line. */
  int pw = pfx ? ui_ptext_w(pfx) : 0;
  int tw = ui_ptext_w(name);
  int tx = x + (w - (pw + tw)) / 2;
  if (pfx) tx = ui_ptext(tx, y + 3, sh, pfx);   /* returns the next glyph's x -- exact join, no gap math */
  ui_ptext(tx, y + 3, ink, name);
}

/* a top-bar tab (PKMN DATA / PARTY / CLOSE); active reads bright, inactive dim */
static void draw_tab(int x, int w, const char* label, bool active) {
  const u16 fill = active ? RGB15(7, 22, 27) : RGB15(3, 10, 14);
  const u16 ink  = active ? RGB15(29, 31, 31) : RGB15(12, 20, 24);
  ui_fill_rect(x, 0, w, 12, fill);
  ui_hline(x, 0, w, active ? RGB15(14, 28, 31) : RGB15(6, 16, 20));
  m3_frame(x, 0, x + w - 1, 11, RGB15(2, 8, 11));
  int tw = (int)strlen(label) * 8;
  int tx = x + (w - tw) / 2; if (tx < x + 2) tx = x + 2;
  ui_text(tx, 2, ink, label);
}

static const char* gender_str(uint8_t g) { return g == 0 ? " M" : g == 1 ? " F" : ""; }

static void draw_left(const PkMon* p) {
  ui_panel(0, 0, PANEL_W, 160, UI_PANEL, UI_BORDER);
  /* checkered "monitor" backdrop behind the front sprite, like the real PKMN DATA window */
  draw_checker(5, 15, 66, 66, RGB15(23, 23, 25), RGB15(28, 28, 30));
  m3_frame(4, 14, 71, 81, UI_BORDER);
  if (!p || p->species == 0) { ui_text(16, 92, UI_DIM, "(empty)"); return; }

  rumble_io_suspend();   /* the fetch LZ77-decompresses the portrait from ROM */
  /* pdna_origin_art_portrait() is the SAME unified ladder the summary screen's own
   * draw_left uses (compiled art -> the mon's own GB-origin art, if that era's ROM is
   * registered -> the Gen-3 ROM rung / rom_portrait). This panel used to call
   * mon_front_for_form()/mon_front_egg() directly, which stops at compiled art and
   * never reaches the ROM rung -- exactly why an artless build with a registered ROM
   * showed a 32x32 icon here instead of the full portrait (Guy, item 2). Decoded fresh
   * into mon_decomp on every call (pdna_origin_art.c's rom_portrait comment: never
   * memoised, because era_cells()/item icons/type badges share the same buffer), and
   * used immediately below, never stored past this function -- the shared-buffer rule
   * this file's own header (source/pdna_origin_art.c:339-347) documents. */
  PdnaArt art;
  pdna_origin_art_portrait(p, 0, &art, 0);
  rumble_io_resume();
  if (art.px) {
    int ax, ay;
    pdna_origin_art_place(&art, 6, 16, 64, 64, &ax, &ay);   /* 64x64 Gen-3 -> (6,16), as before */
    ui_sprite(ax, ay, art.w, art.h, art.px);
  } else if (art.egg) {
    ui_sprite(22, 32, MON_ICON_W, MON_ICON_H, mon_icon_egg());
  } else {
    ui_sprite(22, 32, MON_ICON_W, MON_ICON_H, mon_icon_for_form(p->species, p->form));
  }

  /* Names are drawn with the PROPORTIONAL face. At 8 px/glyph this panel held nine
   * characters, so `JIGGLYPUFF` printed as `JIGGLYPU~` and every berry collapsed to
   * `CHES~`/`LUM ~` — you could not tell which berry a mon was holding. Retail fits
   * the same strings in a NARROWER panel because its font is variable-width; this is
   * the same fix, and it also makes this panel agree with the party overlay, which
   * had been showing the same two fields at a different width. */
  char buf[40];
  siprintf(buf, "No.%u", (unsigned)pk_national_no(p->species));
  ui_text(4, 86, UI_DIRCLR, buf);
  if (p->isShiny) ui_text(56, 86, UI_WARN, "*");
  const int TW = PANEL_W - 8;                         /* text budget: x=4 .. panel edge */
  ui_ptext_fit(4, 96, TW, UI_TEXT,
               p->nickname[0] ? p->nickname : pk_species_name(p->species));
  siprintf(buf, "Lv%u%s", (unsigned)p->level, gender_str(p->gender));
  ui_text(4, 106, UI_TEXT, buf);
  ui_ptext_fit(4, 116, TW, UI_DIM, pk_species_name(p->species));

  /* The item icon moved UP beside its label so the NAME gets a full-width line of
   * its own: the old layout stopped the name at the icon's left edge (x=50), which
   * is the five columns that produced `LUM ~`. */
  ui_text(4, 128, UI_DIRCLR, "Item");
  if (p->heldItem) {
    const uint16_t* iic = app_item_icon(p->heldItem);   /* + the ROM rung */
    if (iic) ui_sprite(PANEL_W - 28, 124, ITEM_ICON_W, ITEM_ICON_H, iic);
    char it[48];
    pk_item_label(p->heldItem, it, sizeof it);        /* a TM carries its move */
    /* x=2 and the full panel width, not the x=4/TW the rows above use: the longest real
     * item names land within a pixel or two of the panel edge ("CHESTO BERRY" is 69 px,
     * "DEEPSEATOOTH" 72) and losing the last letter to a 4 px indent is the whole defect
     * this is fixing. */
    ui_ptext_fit(2, 150, PANEL_W - 4, UI_TEXT, it);
  } else {
    ui_ptext(4, 150, UI_DIM, "-");
  }
}

/* Box wallpaper: blit the real wallpaper for `wp` (deduped 8x8 RGB15 tiles +
 * a 20x18 tilemap) if present, else fall back to the procedural grass. The strong
 * accessors come from the generated wallpapers.c (git-ignored); these weak
 * fallbacks return NULL so the build works before the wallpapers are generated. */
const uint16_t* wallpaper_tile_data(int wp, int* ntiles);
const uint16_t* wallpaper_tilemap(int wp);
uint32_t wallpaper_map_sum(int wp);    /* build-time FNV-1a goldens (generated wallpapers.c) */
uint32_t wallpaper_tile_sum(int wp);
const uint16_t* wallpaper_walda_sentinels(void);   /* the 2 sentinel RGB15s baked into the Walda tiles */
__attribute__((weak)) const uint16_t* wallpaper_tile_data(int wp, int* n) { (void)wp; if (n) *n = 0; return 0; }
__attribute__((weak)) const uint16_t* wallpaper_tilemap(int wp) { (void)wp; return 0; }
__attribute__((weak)) uint32_t wallpaper_map_sum(int wp) { (void)wp; return 0; }   /* 0 = no golden, skip */
__attribute__((weak)) uint32_t wallpaper_tile_sum(int wp) { (void)wp; return 0; }
__attribute__((weak)) const uint16_t* wallpaper_walda_sentinels(void) { return 0; }

/* pdna_main.c: the save's two Walda wallpaper colors ([0] background, [1] foreground),
 * or the game's contrasting defaults when unreadable (non-Emerald). Declared here like
 * the wallpaper accessors above (pdna_app.h is owned by concurrent work). */
bool app_walda_colors(uint16_t out[2]);

/* pdna_main.c: the currently open ROM's wallpaper reader (fused, or later a
 * registered SD file) -- NULL if none is open this session, or the game/revision
 * isn't one of the three rom_wallpaper.h pins. Declared here like the app_* ROM-art
 * accessors above; defined beside s_iconrom/s_romsprite/s_romitemart so one
 * registration lights this up too. */
const RomWallpaper* app_wallpaper_rom(void);

/* ---- §12a Walda color substitution -------------------------------------------------
 * The Friends/Walda wallpapers (ids 16..31) are pre-rendered with two SENTINEL colors
 * standing in for palette entries 1..2 — the entries Emerald overwrites at load with
 * the SAVE'S chosen Walda colors (pokemon_storage_system.c:5391-5397 LoadWallpaperGfx
 * copies GetWaldaWallpaperColorsPtr() over palette entries [1..2]/[17..18]). The
 * substitution happens AFTER a tile's verified ROM staging and BEFORE its blit, so
 * every golden/checksum keeps operating on the sentinel data unchanged. */
static const uint16_t* s_wp_sent;       /* active sentinels (NULL = standard wp, no sub) */
static uint16_t s_wp_wcol[2];           /* the save's (or default) Walda colors          */
static void wp_walda_setup(int wp) {
  s_wp_sent = (wp >= G3_BOX_WALLPAPER_COUNT) ? wallpaper_walda_sentinels() : 0;
  if (s_wp_sent) app_walda_colors(s_wp_wcol);
}
static void wp_sub_walda(uint16_t* t64) {
  if (!s_wp_sent) return;
  for (int k = 0; k < 64; k++) {
    if      (t64[k] == s_wp_sent[0]) t64[k] = (uint16_t)(s_wp_wcol[0] & 0x7FFF);
    else if (t64[k] == s_wp_sent[1]) t64[k] = (uint16_t)(s_wp_wcol[1] & 0x7FFF);
  }
}

/* §12b state: which wallpaper's ART currently fills the region (-1 = grass fallback or
 * none — wp_restore_rect then repaints the aligned grass patch instead), and which grid
 * cells carry a software-blitted "understudy" icon in the bitmap (see the hooks below). */
static int s_wp_drawn = -1;
static uint8_t s_wp_drawn_kind = 0;    /* 0 none/grass, 1 compiled (cart-baked), 2 ROM-live */
static uint8_t s_under[G3_BOX_SLOTS];

/* Copy n u16s ROM -> RAM, then RE-READ the ROM and byte-compare; retry on mismatch.
 * A transient cart-bus glitch during either pass makes the passes disagree, so a
 * verified copy is trustworthy on any single-glitch theory. Returns the number of
 * retries burned (0 = clean first try), or -1 if still dirty after 4 attempts.
 * IWRAM_CODE: while this runs, the cart bus carries PURE data reads — no ROM opcode
 * fetches interleaved (the one traffic shape the never-garbled paths — DMA icon
 * uploads, BIOS LZ77 — never produce). volatile source: without it the compiler may
 * legally fold "copy then compare-to-source" into always-equal and delete the verify. */
static uint16_t s_wp_map[20 * 18];     /* verified tilemap copy (720 B, IWRAM .bss) */
static uint16_t s_wp_tile[64] __attribute__((aligned(4)));
                                       /* one verified 8x8 tile (128 B) — shared by
                                        * draw_wallpaper and the START+SELECT audit.
                                        * 4-ALIGNED: wp_blit_tile reads it as u32.   */

IWRAM_CODE __attribute__((noinline))
static int wp_copy_verified(uint16_t* dst, const uint16_t* src, int n) {
  volatile const uint16_t* vsrc = src;
  for (int a = 0; a < 4; a++) {
    for (int i = 0; i < n; i++) dst[i] = vsrc[i];
    int ok = 1;
    for (int i = 0; i < n; i++) if (dst[i] != vsrc[i]) { ok = 0; break; }
    if (ok) return a;
  }
  return -1;
}

/* Blit one staged 8x8 tile to (bx,by), `rows` scanlines tall, as 32-bit VRAM stores.
 * The wallpaper grid is word-aligned BY CONSTRUCTION: bx = WP_X + 8*tx with WP_X=78, so
 * the VRAM byte offset 2*bx = 156 + 16*tx is always a multiple of 4, and a Mode-3
 * scanline is 480 B — so a row of 8 pixels is exactly four aligned word stores. That
 * replaces 64 m3_plot calls per tile, each of which carried two loop-bound tests and a
 * per-pixel mask: ~12 instructions per pixel over 23,040 pixels, a third of a whole box
 * repaint.
 * `t64` must ALREADY be Walda-substituted and masked to 15 bits — a word store cannot
 * mask per pixel, so the caller does it once per tile instead of once per pixel. The GBA
 * ignores bit 15 of a Mode-3 halfword, so this is the same picture; the mask is kept only
 * so the bitmap stays byte-identical to what every other path writes.
 * SAFE ONLY BECAUSE CFLAGS carries -fno-strict-aliasing (Makefile:141) — this reads a
 * uint16_t[] through a uint32_t*. If that flag is ever dropped, route this through memcpy
 * or a union. */
static void wp_blit_tile(const uint16_t* t64, int bx, int by, int rows) {
  uint32_t* d = (uint32_t*)(vid_mem + (uint32_t)by * 240 + bx);
  const uint32_t* s = (const uint32_t*)t64;
  for (int j = 0; j < rows; j++) {
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    d += 120;                          /* 480 B = one Mode-3 scanline */
    s += 4;
  }
}

/* ---- §12c the ROM-live wallpaper rung -------------------------------------------
 * When the compiled (cart-baked) wallpaper art is absent (the artless build) but the
 * user has a Pokemon ROM open this session (fused, or a registered SD file --
 * app_wallpaper_rom()), serve the wallpaper straight off THAT cartridge instead of
 * falling to procedural grass. Scope (which games/revisions, standard-only, why
 * palette index 0 is not substituted): rom_wallpaper.h's top-of-file note.
 *
 * VERIFICATION: the SAME retry count (4) and the SAME abandon-to-grass rule as
 * wp_copy_verified above -- deliberately NOT rom_sprite.c's "3 tries, agree twice"
 * convention. A stray SD/cart-bus glitch during either of two independent
 * decompress passes makes them disagree, exactly the "does a second read agree"
 * contract wp_copy_verified applies to a raw pointer, one level up (decompress-of-
 * a-read instead of a read). A tid that lands outside its own tiles blob is treated
 * exactly like an out-of-range map index above: the jumble symptom, not trusted.
 *
 * MEMORY: zero new EWRAM. The map borrows the FIRST 720 B of the shared 8,192 B
 * `mon_decomp` scratch (artbuf.h) as its compare buffer, then releases it; the tile
 * blob (<= ROM_WP_TILES_MAX_BYTES = 4,096 B, comfortably above the measured 3,072 B
 * worst case among the 16 standard wallpapers) uses the front and back HALVES of
 * that same buffer for its own two independent decompresses. Nothing is held onto
 * across calls -- every draw re-stages fresh, the same posture the compiled path's
 * "read is a cart-bus memcpy, cheap" already takes. */
#define WP_ROM_RETRIES 4   /* == wp_copy_verified's own attempt count, on purpose */

static bool wp_rom_stage(const RomWallpaper* rw, int wp, uint32_t* tiles_bytes,
                         uint16_t pal[ROM_WP_PAL_BANKS][16], int* retries_io) {
  artbuf_claim();    /* E3 review BLOCKING 2: about to borrow+overwrite mon_decomp */
  int ok = 0;
  for (int a = 0; a < WP_ROM_RETRIES && !ok; a++) {
    if (!rom_wallpaper_map(rw, wp, s_wp_map)) { (*retries_io)++; continue; }
    uint16_t* cmp = mon_decomp;                      /* 720 B borrowed, released below */
    if (rom_wallpaper_map(rw, wp, cmp) && memcmp(s_wp_map, cmp, ROM_WP_MAP_BYTES) == 0) ok = 1;
    else (*retries_io)++;
  }
  if (!ok) return false;

  ok = 0;
  uint8_t* ta = (uint8_t*)mon_decomp;
  uint8_t* tb = (uint8_t*)mon_decomp + ROM_WP_TILES_MAX_BYTES;
  uint32_t na = 0, nb = 0;
  for (int a = 0; a < WP_ROM_RETRIES && !ok; a++) {
    if (!rom_wallpaper_tiles(rw, wp, ta, ROM_WP_TILES_MAX_BYTES, &na)) { (*retries_io)++; continue; }
    if (rom_wallpaper_tiles(rw, wp, tb, ROM_WP_TILES_MAX_BYTES, &nb) &&
        na == nb && memcmp(ta, tb, na) == 0) ok = 1;
    else (*retries_io)++;
  }
  if (!ok) return false;
  *tiles_bytes = na;

  ok = 0;
  for (int a = 0; a < WP_ROM_RETRIES && !ok; a++) {
    uint16_t pal2[ROM_WP_PAL_BANKS][16];             /* 128 B, a real automatic (rule 2 OK) */
    if (!rom_wallpaper_pal(rw, wp, pal)) { (*retries_io)++; continue; }
    if (rom_wallpaper_pal(rw, wp, pal2) && memcmp(pal, pal2, sizeof pal2) == 0) ok = 1;
    else (*retries_io)++;
  }
  return ok != 0;
}

/* One cell's tile, expanded from the just-verified staging into s_wp_tile, with the
 * same consecutive-same-tile skip and 15-bit mask the compiled path applies. Shared
 * by the full paint and the rect-restore below. Returns false on an out-of-range
 * tid (the caller treats that as dirty, same as the compiled path). */
static bool wp_rom_cell(const uint8_t* tiles, uint32_t tiles_bytes,
                        const uint16_t pal[ROM_WP_PAL_BANKS][16], uint16_t e,
                        int32_t* last_key) {
  /* last_key is int32_t (not uint16_t) so -1 is a genuine "nothing staged yet"
   * sentinel: e is a full 16-bit attribute word (tid 10b + hflip + vflip + bank
   * 4b == 16 bits, so every uint16_t value is reachable) and a narrower sentinel
   * could collide with a real first cell and wrongly skip its expansion, leaving
   * s_wp_tile holding whatever an unrelated earlier caller left in it. */
  if (last_key && (int32_t)e == *last_key) return true;     /* already staged */
  uint16_t tid = (uint16_t)(e & 0x3FFu);
  int hf = (e >> 10) & 1, vf = (e >> 11) & 1, bank = (e >> 12) & 0xF;
  if (!rom_wallpaper_expand_tile(tiles, tiles_bytes, tid, hf, vf,
                                 pal[rom_wallpaper_pal_bank(bank)], s_wp_tile))
    return false;
  if (last_key) *last_key = (int32_t)e;
  uint32_t* wmk = (uint32_t*)s_wp_tile;         /* 15-bit mask ONCE per tile, matching draw_wallpaper */
  for (int k = 0; k < 32; k++) wmk[k] &= 0x7FFF7FFFu;
  return true;
}

static bool draw_wallpaper_rom(int wp, int x, int y, int w, int h) {
  if (wp < 0 || wp >= ROM_WP_COUNT) return false;   /* Walda ids: not served, see rom_wallpaper.h */
  const RomWallpaper* rw = app_wallpaper_rom();
  if (!rw || !rw->ok) return false;

  int retries = 0;
  uint32_t tiles_bytes = 0;
  uint16_t pal[ROM_WP_PAL_BANKS][16];
  rumble_io_suspend();
  bool ok = wp_rom_stage(rw, wp, &tiles_bytes, pal, &retries);
  if (!ok) {
    rumble_io_resume();
    log_line("wp %d (rom): unstable reads after %d re-decodes, grass fallback", wp, retries);
    return false;                                    /* abandon rule: caller falls to grass */
  }

  const uint8_t* tiles = (const uint8_t*)mon_decomp;  /* the verified pass-1 half, still resident */
  bool dirty = false;
  int32_t last_key = -1;
  for (int ty = 0; ty < 18 && !dirty; ty++)
    for (int tx = 0; tx < 20; tx++) {
      uint16_t e = s_wp_map[ty * 20 + tx];
      if (!wp_rom_cell(tiles, tiles_bytes, pal, e, &last_key)) { dirty = true; break; }
      int bx = x + tx * 8, by = y + ty * 8;
      int rows = y + h - by; if (rows > 8) rows = 8;
      int cols = x + w - bx; if (cols > 8) cols = 8;
      if (rows <= 0 || cols <= 0) continue;
      if (cols == 8 && !(bx & 1)) wp_blit_tile(s_wp_tile, bx, by, rows);
      else for (int j = 0; j < rows; j++)
             for (int i = 0; i < cols; i++)
               m3_plot(bx + i, by + j, s_wp_tile[j * 8 + i]);
    }
  rumble_io_resume();
  if (dirty) {
    log_line("wp %d (rom): tid out of range, grass fallback", wp);
    return false;
  }
  if (retries) log_line("wp %d (rom): %d re-decodes", wp, retries);
  s_wp_drawn = wp;
  s_wp_drawn_kind = 2;
  return true;
}

static void draw_wallpaper(int wp, int x, int y, int w, int h) {
  int nt; const uint16_t* tiles = wallpaper_tile_data(wp, &nt);
  const uint16_t* map = wallpaper_tilemap(wp);
  /* any full-region repaint wipes the §12b understudy blits with it */
  for (int s = 0; s < G3_BOX_SLOTS; s++) s_under[s] = 0;
  s_wp_drawn = -1;
  s_wp_drawn_kind = 0;
  if (!tiles || !map) {
    if (draw_wallpaper_rom(wp, x, y, w, h)) return;   /* NEW: a registered/fused ROM */
    draw_grass(x, y, w, h);                           /* procedural fallback: no ROM read */
    return;
  }
  wp_walda_setup(wp);                  /* Walda (16..31): substitute the save's colors at blit */
  /* HW-ROBUST + SELF-VERIFYING render. History: the wallpaper garbled into a "jumble of
   * tiles" on the real EZ-Flash (never in emulators) and survived two fix theories — the
   * rumble-GPIO render guard (cba40e7; Guy later proved zero rumble correlation) and the
   * sequential RAM-staging rewrite (a2eb21e; kept below). The data itself host-renders
   * clean (all 32), so if the jumble persists the corruption happens on this cartridge
   * read path at draw time. So this draw no longer TRUSTS any ROM read: every staged
   * block is re-read and compared (retry on mismatch), every staged map index is
   * bounds-checked against nt (a corrupt index IS the jumble symptom), and a block that
   * stays dirty after 4 attempts abandons the art for the procedural grass instead of
   * baking garbage into VRAM. Every anomaly is counted + logged, so a hardware session
   * yields a diagnosis either way:
   *   "wp N: R rom re-reads"     -> transient cart-bus read corruption, caught + healed
   *   "wp N: map changed ..."    -> same wallpaper staged DIFFERENTLY across draws
   *   "wp N: unstable ..."       -> persistent read corruption; grass fallback shown
   *   none of the above yet still garbled on screen -> the fault is NOT in ROM reads
   *                                 (VRAM write / display path — a different hunt). */
  static uint32_t s_wp_sum[32];        /* per-id checksum of the last staged map */
  static uint8_t  s_wp_seen[32];
  static uint8_t s_wp_tchk[32];        /* tile-array golden verified this session   */
  int retries = 0, dirty = 0, anom = 0;
  uint32_t mgold = wallpaper_map_sum(wp);
  rumble_io_suspend();
  /* stage + verify the tilemap, then validate it TWICE over: every index in range
   * (an out-of-range index IS the jumble) and, when the build carries goldens, the
   * staged map's FNV must equal the build-time value — this catches DETERMINISTIC
   * mis-reads that a copy+re-read can never see (both passes read the same wrong data) */
  for (int a = 0; ; a++) {
    int r = wp_copy_verified(s_wp_map, map, 20 * 18);
    if (r < 0) { dirty = 1; break; }
    retries += r;
    int bad = 0;
    for (int m = 0; m < 20 * 18 && !bad; m++) if (s_wp_map[m] >= (uint16_t)nt) bad = 1;
    uint32_t sum = 2166136261u;
    for (int m = 0; m < 20 * 18; m++) { sum ^= s_wp_map[m]; sum *= 16777619u; }
    if (!bad && mgold && sum != mgold) bad = 2;
    if (!bad) {
      /* cross-draw stability: the same id must stage the same map every time
       * (matters for the no-golden build, where mgold==0) */
      if (wp >= 0 && wp < 32) {
        if (s_wp_seen[wp] && s_wp_sum[wp] != sum) {
          log_line("wp %d: map changed between draws (%lx != %lx)",
                   wp, (unsigned long)s_wp_sum[wp], (unsigned long)sum);
          anom = 1;
        }
        s_wp_seen[wp] = 1; s_wp_sum[wp] = sum;
      }
      break;
    }
    if (a >= 5) {
      if (bad == 2) log_line("wp %d: map sum != golden (%lx != %lx)",
                             wp, (unsigned long)sum, (unsigned long)mgold);
      dirty = 1; break;
    }
    retries++;
  }
  /* once per id per session: stream the WHOLE tile array (verified) and FNV it
   * against the build-time golden — the deterministic-mis-read check for the tiles */
  if (!dirty && wp >= 0 && wp < 32 && !s_wp_tchk[wp]) {
    uint32_t tgold = wallpaper_tile_sum(wp);
    if (tgold) {
      uint32_t tsum = 2166136261u; int bad = 0;
      for (int off = 0; off < nt * 64 && !bad; off += 64) {
        if (wp_copy_verified(s_wp_tile, tiles + off, 64) < 0) bad = 1;
        else for (int k = 0; k < 64; k++) { tsum ^= s_wp_tile[k]; tsum *= 16777619u; }
      }
      if (bad || tsum != tgold) {
        log_line("wp %d: tile sum != golden (%lx != %lx)",
                 wp, (unsigned long)tsum, (unsigned long)tgold);
        dirty = 1;
      } else s_wp_tchk[wp] = 1;
    }
  }
  if (!dirty) {
    int last_idx = -1;                 /* consecutive same-tile cells skip the re-stage */
    for (int ty = 0; ty < 18 && !dirty; ty++)
      for (int tx = 0; tx < 20; tx++) {
        int idx = s_wp_map[ty * 20 + tx];
        if (idx != last_idx) {
          int r = wp_copy_verified(s_wp_tile, tiles + (uint32_t)idx * 64, 64);
          if (r < 0) { dirty = 1; break; }
          retries += r;
          last_idx = idx;
          wp_sub_walda(s_wp_tile);     /* AFTER the verify, BEFORE the blit (§12a) */
          { uint32_t* wmk = (uint32_t*)s_wp_tile;      /* 15-bit mask ONCE per tile,   */
            for (int k = 0; k < 32; k++)               /* so the blit can move words   */
              wmk[k] &= 0x7FFF7FFFu; }
        }
        /* TRAP for a future in-loop painter: the cols clamp below never actually
         * shrinks anything for this call's WP_W=162 -- it only ever caps at 8 --
         * which reads like "this loop paints the full WP_W-wide rect." It does
         * not: the grid is fixed at 20 tiles * 8 px = 160 px wide regardless of w,
         * so this loop only ever writes x=78..237 (WP_X..WP_X+159). Columns
         * 238-239 -- 2 px still inside the declared WP_W=162 -- are UI_BG solely
         * because whatever ran before this call cleared them; this loop never
         * touches them. */
        int bx = x + tx * 8, by = y + ty * 8;
        int rows = y + h - by; if (rows > 8) rows = 8;   /* only the bottom row clips  */
        int cols = x + w - bx; if (cols > 8) cols = 8;   /* caps at 8, doesn't shrink (see above) */
        if (rows <= 0 || cols <= 0) continue;
        if (cols == 8 && !(bx & 1)) wp_blit_tile(s_wp_tile, bx, by, rows);
        else for (int j = 0; j < rows; j++)              /* clipped/odd-x: per pixel   */
               for (int i = 0; i < cols; i++)
                 m3_plot(bx + i, by + j, s_wp_tile[j * 8 + i]);
      }
  }
  rumble_io_resume();
  if (!dirty) { s_wp_drawn = wp; s_wp_drawn_kind = 1; }  /* art on screen: rect restores may re-use it */
  if (dirty) {
    log_line("wp %d: unstable rom reads, grass fallback (%d re-reads)", wp, retries);
    anom = 1;
    draw_grass(x, y, w, h);
  } else if (retries) {
    log_line("wp %d: %d rom re-reads", wp, retries);
    anom = 1;
  }
  /* an anomaly must survive a power-off: flush the RAM log to SD now (capped so a
   * chooser sweep over a bad cart can't thrash the card) */
  static int s_wp_flushes = 0;
  if (anom && s_wp_flushes < 3) { s_wp_flushes++; app_log_flush(); }
}

/* ---- §12b rect-limited wallpaper restore + the "bitmap understudy" ------------------
 * While the floating chunk block covers an OCCUPIED cell, that cell's OBJ is hidden and
 * its icon is software-blitted INTO the Mode-3 bitmap instead — the ghost block
 * (ATTR0_BLEND, 2nd target BG2) then alpha-blends OVER it, Emerald's see-through look
 * (OBJ can't blend over OBJ on this hardware, so the mon must live on the BG layer).
 * On uncover, only the touched cell's wallpaper rect is re-staged and repainted. */

static void plot_clip(int x, int y, u16 c, int x0, int y0, int x1, int y1) {
  if (x >= x0 && x < x1 && y >= y0 && y < y1) m3_plot(x, y, c);
}

/* Rect-only repaint for the §12c ROM-live rung, mirroring wp_restore_rect's
 * compiled-path loop below but re-staging from ROM (cheap: <= 4,096 B LZ77 blob)
 * rather than re-using a cart pointer, since nothing here is held resident between
 * draws. `x0,y0,x1,y1` are ALREADY CLIPPED to the wallpaper region by the caller. A
 * restage that fails to verify leaves the rect exactly as it was on screen --
 * "a tile that stays dirty is skipped" (wp_restore_rect's own rule) applies
 * identically here: a stale patch beats baked garbage. */
static void wp_restore_rect_rom(int x0, int y0, int x1, int y1) {
  if (s_wp_drawn < 0) return;
  const RomWallpaper* rw = app_wallpaper_rom();
  if (!rw || !rw->ok) return;
  int retries = 0;
  uint32_t tiles_bytes = 0;
  uint16_t pal[ROM_WP_PAL_BANKS][16];
  rumble_io_suspend();
  bool ok = wp_rom_stage(rw, s_wp_drawn, &tiles_bytes, pal, &retries);
  if (!ok) { rumble_io_resume(); return; }
  const uint8_t* tiles = (const uint8_t*)mon_decomp;
  int tx0 = (x0 - WP_X) / 8, tx1 = (x1 - 1 - WP_X) / 8;
  int ty0 = (y0 - WP_Y) / 8, ty1 = (y1 - 1 - WP_Y) / 8;
  if (tx1 > 19) tx1 = 19;          /* the map is 20 tiles wide: index 20 would read the NEXT row's first tile */
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) {
      uint16_t e = s_wp_map[ty * 20 + tx];
      if (!wp_rom_cell(tiles, tiles_bytes, pal, e, 0)) continue;  /* skip: stale beats garbage */
      int bx = WP_X + tx * 8, by = WP_Y + ty * 8;
      for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++)
          plot_clip(bx + i, by + j, s_wp_tile[j * 8 + i] & 0x7FFF, x0, y0, x1, y1);
    }
  rumble_io_resume();
  if (retries) log_line("wp %d (rom): %d re-decodes (rect restore)", s_wp_drawn, retries);
}

/* Repaint the wallpaper inside [x0,x0+w)x[y0,y0+h) only. Art path: re-uses the staged
 * s_wp_map (verified + golden-checked by the last full draw of s_wp_drawn) and the
 * same verified per-tile staging + Walda substitution as draw_wallpaper. ROM path
 * (s_wp_drawn_kind == 2): the §12c rung above. Grass path (s_wp_drawn < 0): repaint
 * base + leaves with the FULL-REGION phase so the patch can't seam against the
 * surrounding fallback. A tile that stays dirty is skipped — a stale 8x8 beats
 * baking garbage, and the next full repaint heals it. */
static void wp_restore_rect(int x0, int y0, int w, int h) {
  int x1 = x0 + w, y1 = y0 + h;
  if (x0 < WP_X) x0 = WP_X;
  if (y0 < WP_Y) y0 = WP_Y;
  if (x1 > WP_X + WP_W) x1 = WP_X + WP_W;
  if (y1 > WP_Y + WP_H) y1 = WP_Y + WP_H;
  if (x0 >= x1 || y0 >= y1) return;
  if (s_wp_drawn_kind == 2) { wp_restore_rect_rom(x0, y0, x1, y1); return; }
  int nt = 0;
  const uint16_t* tiles = (s_wp_drawn >= 0) ? wallpaper_tile_data(s_wp_drawn, &nt) : 0;
  if (!tiles) {                                   /* the grass fallback is on screen */
    const u16 base = RGB15(19, 25, 12), dk = RGB15(13, 19, 6), lt = RGB15(22, 28, 15);
    ui_fill_rect(x0, y0, x1 - x0, y1 - y0, base);
    for (int j = 0; j + 6 < WP_H; j += 12) {
      int off = ((j / 12) & 1) ? 8 : 0;
      for (int i = off; i + 4 < WP_W; i += 16) {
        int lx = WP_X + i + 2, ly = WP_Y + j + 3;     /* same placement as draw_grass */
        plot_clip(lx + 1, ly,     dk, x0, y0, x1, y1);
        plot_clip(lx + 2, ly,     lt, x0, y0, x1, y1);
        plot_clip(lx,     ly + 1, dk, x0, y0, x1, y1);
        plot_clip(lx + 1, ly + 1, lt, x0, y0, x1, y1);
        plot_clip(lx + 2, ly + 1, dk, x0, y0, x1, y1);
        plot_clip(lx + 1, ly + 2, dk, x0, y0, x1, y1);
      }
    }
    return;
  }
  wp_walda_setup(s_wp_drawn);
  rumble_io_suspend();
  int tx0 = (x0 - WP_X) / 8, tx1 = (x1 - 1 - WP_X) / 8;
  int ty0 = (y0 - WP_Y) / 8, ty1 = (y1 - 1 - WP_Y) / 8;
  if (tx1 > 19) tx1 = 19;          /* see wp_restore_rect_rom: never read past the 20-tile row */
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) {
      int idx = s_wp_map[ty * 20 + tx];
      if (idx >= nt || wp_copy_verified(s_wp_tile, tiles + (uint32_t)idx * 64, 64) < 0) continue;
      wp_sub_walda(s_wp_tile);
      int bx = WP_X + tx * 8, by = WP_Y + ty * 8;
      for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++)
          plot_clip(bx + i, by + j, s_wp_tile[j * 8 + i] & 0x7FFF, x0, y0, x1, y1);
    }
  rumble_io_resume();
}

/* the chunk block currently floats over its own SOURCE box (set by chunk_oam_sync) */
static bool s_ch_on_src = false;

/* Blit `slot`'s icon into the Mode-3 bitmap at its cell, skipping transparent pixels
 * (the 0x8000 key, like ui_sprite) — but from a VERIFIED staging: this cartridge
 * demonstrably garbles raw CPU ROM reads (the whole wallpaper history above), and a
 * garbled understudy would sit UNDER the ghost block looking exactly like the old
 * jumble. Each 64-pixel chunk re-uses s_wp_tile + wp_copy_verified; a chunk that
 * stays dirty is skipped (a hole beats baked garbage; the next repaint heals it). */
static void under_blit(int slot) {
  const uint16_t* ic = (g_box[slot].isEgg && !g_box[slot].isBadEgg)
                     ? mon_icon_egg()
                     : mon_icon_for_form(g_box[slot].species, g_box[slot].form);
  if (!ic) return;                                /* art-free build: nothing to blit */
  int x0 = GRID_X + (slot % COLS) * CELL_W, y0 = GRID_Y + (slot / COLS) * CELL_H;
  rumble_io_suspend();
  for (int off = 0; off < MON_ICON_W * MON_ICON_H; off += 64) {
    if (wp_copy_verified(s_wp_tile, ic + off, 64) < 0) continue;
    for (int k = 0; k < 64; k++) {
      uint16_t p = s_wp_tile[k];
      if (p & 0x8000)
        m3_plot(x0 + (off + k) % MON_ICON_W, y0 + (off + k) / MON_ICON_W,
                (u16)(p & 0x7FFF));
    }
  }
  rumble_io_resume();
}

/* Strong impls of the box_oam.h cover/uncover hooks (box_oam.c owns the covered-cell
 * bookkeeping and fires these exactly on transitions; the bitmap work needs g_box +
 * the wallpaper staging, both private to this file — hence the hook seam). */
void boxoam_under_show(int slot) {
  if (slot < 0 || slot >= G3_BOX_SLOTS || s_under[slot] || !g_box[slot].species) return;
  if (s_ch_hold && s_ch_on_src)                   /* a lifted SOURCE cell is vacating: it is
                                                   * carried IN the block, nothing lives under */
    for (int i = 0; i < s_ch.n; i++) if (s_ch.src[i] == slot) return;
  s_under[slot] = 1;
  under_blit(slot);
}

void boxoam_under_hide(int slot) {
  if (slot < 0 || slot >= G3_BOX_SLOTS || !s_under[slot]) return;
  s_under[slot] = 0;
  int x = GRID_X + (slot % COLS) * CELL_W, y = GRID_Y + (slot / COLS) * CELL_H;
  wp_restore_rect(x, y, MON_ICON_W, MON_ICON_H);
  /* THE "transparent BOX" ARTIFACT (Guy's HW round): the 32x32 icons live on a 24x22
   * cell pitch, so this 32x32 restore rect overlaps the neighbouring cells — chopping
   * up to 8-px-wide / 10-px-tall wallpaper-coloured RECTANGLES out of the blits of
   * cells still covered by the block (their s_under stays set, so nothing repainted
   * them). Every anchor move carved fresh notches that showed through the translucent
   * block as a moving rectangular hole. Repair: re-blit every still-understudied cell
   * whose icon rect intersects the restored rect (idempotent — repaints exactly what
   * that cell's own blit painted, transparent pixels skipped). */
  for (int s = 0; s < G3_BOX_SLOTS; s++) if (s_under[s]) {
    int sx = GRID_X + (s % COLS) * CELL_W, sy = GRID_Y + (s / COLS) * CELL_H;
    if (sx < x + MON_ICON_W && sx + MON_ICON_W > x &&
        sy < y + MON_ICON_H && sy + MON_ICON_H > y)
      under_blit(s);
  }
  /* Same wound, the era layer. The restore rect also erased the era art / era pads of
   * every cell it overlapped, and this runs on EVERY anchor move of a held chunk — so
   * repainting the PICTURE here would mean an SD decode per D-pad press. Instead the
   * damaged art cells are handed back to their Gen-3 OBJ icon (instant, and the mon is
   * still visible) and only the free marker layer is repainted; the next full repaint,
   * which the drop already does, restores the era art. */
  for (int s = 0; s < G3_BOX_SLOTS; s++) {
    int sx = GRID_X + (s % COLS) * CELL_W, sy = GRID_Y + (s / COLS) * CELL_H;
    if (sx >= x + CELL_W || sx + CELL_W <= x || sy >= y + CELL_H || sy + CELL_H <= y)
      continue;
    era_cell_icon_back(s);
    era_cell_mark(s);
  }
}

/* ---- WP AUDIT (START+SELECT): the on-hardware wallpaper experiment ------------------
 * Fold the audit region — the art rows clear of the banner/footer overdraw — in tile
 * order. src_vram folds the LIVE bitmap; otherwise the verified-staged art (the
 * expected image). IDENTICAL traversal for both, so the sums compare directly.
 * Uses the s_wp_map staged by the caller. Returns 0 only on a staging failure. */
#define AUD_Y0 28    /* below the banner strip (12..27)  */
#define AUD_Y1 151   /* above the footer strip (152..)   */
static uint32_t wp_fold_region(const uint16_t* tiles, int nt, bool src_vram) {
  uint32_t sum = 2166136261u;
  int last_idx = -1;
  for (int ty = 0; ty < 18; ty++)
    for (int tx = 0; tx < 20; tx++) {
      int idx = s_wp_map[ty * 20 + tx];
      if (!src_vram && idx != last_idx) {
        if (idx >= nt || wp_copy_verified(s_wp_tile, tiles + (uint32_t)idx * 64, 64) < 0) return 0;
        last_idx = idx;
        wp_sub_walda(s_wp_tile);       /* the SCREEN carries the substituted Walda colors —
                                        * the expected fold must match (caller ran wp_walda_setup) */
      }
      for (int j = 0; j < 8; j++) {
        int y = WP_Y + ty * 8 + j;
        if (y < AUD_Y0 || y > AUD_Y1) continue;
        for (int i = 0; i < 8; i++) {
          uint16_t v = src_vram ? vid_mem[y * 240 + (WP_X + tx * 8 + i)]
                                : (uint16_t)(s_wp_tile[j * 8 + i] & 0x7FFF);
          sum ^= v; sum *= 16777619u;
        }
      }
    }
  return sum ? sum : 1;
}

/* Run WHILE the garble is visible. Three verdicts, shown on the footer + logged +
 * flushed to /PokeDNA/log.txt:
 *   SHOWN  — does the bitmap ON SCREEN RIGHT NOW match the verified art? BAD here
 *            with an OK REDRAW = something corrupted VRAM after the original draw.
 *   REDRAW — after painting again on the spot. BAD = the write path itself is broken.
 *   OBJ blink — 3 s with all sprites OFF: if the garble vanishes, it lives on the
 *            OBJ layer (garbled sprites over a clean wallpaper look identical). */
static void wp_audit(BoxSource* src, int box) {
  int wp = src->get_wp(box);
  int nt; const uint16_t* tiles = wallpaper_tile_data(wp, &nt);
  const uint16_t* map = wallpaper_tilemap(wp);
  uint32_t esum = 0, va = 0, vb = 0;
  wp_walda_setup(wp);                                    /* folds must use the Walda-substituted look */
  bool staged = tiles && map && wp_copy_verified(s_wp_map, map, 20 * 18) >= 0;
  if (staged) {
    va   = wp_fold_region(tiles, nt, true);              /* as displayed now */
    esum = wp_fold_region(tiles, nt, false);             /* the expected art */
    draw_wallpaper(wp, WP_X, WP_Y, WP_W, WP_H);          /* fresh paint      */
    if (wp_copy_verified(s_wp_map, map, 20 * 18) >= 0)   /* re-stage (draw shares the buffer) */
      vb = wp_fold_region(tiles, nt, true);              /* audited again    */
  }
  bool okA = staged && esum && va == esum, okB = staged && esum && vb == esum;
  log_line("wpaudit wp=%d shown=%s redraw=%s e=%lx a=%lx b=%lx", wp,
           okA ? "OK" : "BAD", okB ? "OK" : "BAD",
           (unsigned long)esum, (unsigned long)va, (unsigned long)vb);
  char l[32];
  siprintf(l, "SHOWN:%s REDRAW:%s", okA ? "OK" : "BAD", okB ? "OK" : "BAD");
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, (okA && okB) ? UI_OK : UI_WARN, l);
  boxoam_commit();
  for (int v = 0; v < 90; v++) s_vsync();                /* time to read the verdict */
  REG_DISPCNT &= ~DCNT_OBJ;                              /* sprites OFF: which layer is it on? */
  for (int v = 0; v < 180; v++) s_vsync();
  REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D;
  log_line("wpaudit objblink done");
  app_log_flush();
}

/* --- Icons, cursor, carry, and item markers are HARDWARE OBJ sprites (box_oam.c).
 * The GPU composites all 30 icons + the overlays above the BG bitmap every frame,
 * so the bob is a free in-vblank OAM nudge (all icons together, no flicker) and the
 * cursor never stalls. Only the wallpaper / banner / panel / tabs / footer below are
 * software Mode-3. Helpers below push OAM state from the box loop's existing render
 * points; boxoam_commit() flushes the OAM shadow in the vblank tick (s_vsync). --- */

/* one-shot flag: re-upload the box's icon tiles into OBJ VRAM on the next paint */
static bool s_oam_reload = true;

/* the cursor look matching the active cursor mode */
static int cursor_look(void) {
  return s_cur_mode == CM_MOVE ? BOXOAM_HAND_MOVE
       : s_cur_mode == CM_ITEM ? BOXOAM_HAND_ITEM
                               : BOXOAM_HAND_NORMAL;
}

/* ---- mon-in-hand carry helpers (data-safety notes on the s_held declaration) ---- */
/* BACKLOG #199 (lane b199): a GB-scope grab no longer pays the Bank's price (the
 * "WHICH GAME IS THIS?" origin prompt and a verified bank.meta serial write, both
 * inside lift_up()) at GRAB time -- it copies the display record exactly like a
 * PC/Bank grab always has (`recs` is the mount's SYNTHESISED Gen-3 grid for a GB
 * source, gb_build_slot's own output, built to decode/render like an ordinary Gen-3
 * record; see oam_sync's own bc_is_native(s_held) branch below for what that buys).
 * The lift through BoxXferOps.lift_up moved to the one drop that actually needs it
 * -- drop_held_up, the GB -> Bank UP drop -- so a within-save move (drag to another
 * box of the SAME save) now asks nothing and writes no serial; only landing in the
 * Bank still does. This function can no longer fail on a valid slot (nothing here
 * touches the card), but keeps its `bool` return and every call site's existing
 * `!start_carry(...)` check as harmless, unchanged defensive code -- the checked
 * failure mode moved to drop_held_up's own `!s_xfer_peer->lift_up(...)` branch. */
static bool start_carry(BoxSource* src, const uint8_t* recs, int box, int slot) {
  memcpy(s_held, recs + (uint32_t)slot * 80, 80);              /* lift-don't-clear: copy, origin stays */
  s_holding = true; s_orig_box = box; s_orig_slot = slot; s_orig_scope = src->scope;
  s_held_dup = false;                                         /* a real mon (origin keeps it) */
  s_orig_party = false;                                       /* a box/bank origin, not the party */
  return true;
}

/* Clear the origin cell after a within-scope drop; handles bank paging and returns the
 * CURRENT box reloaded into recs. Call AFTER placing the held mon at the dest (dest-first
 * ordering -> a mid-op power loss duplicates, never loses). */
static uint8_t* clear_origin(BoxSource* src, int box) {
  if (s_orig_party) {                                         /* carried OUT of the party -> remove it there */
    app_party_remove_at(s_orig_slot);                         /* deferred (staged SB1); fail-toward-dup */
    s_orig_party = false; s_orig_slot = -1; return src->records(box);
  }
  if (s_orig_slot < 0 || !same_scope(src)) { s_orig_slot = -1; return src->records(box); }
  uint8_t* o = src->records(s_orig_box);                     /* bank: flushes the current (dest) box first */
  memset(o + (uint32_t)s_orig_slot * 80, 0, 80);
  src->mark_dirty(); s_orig_slot = -1;
  return src->records(box);                                  /* reload the current box */
}

/* docs/BANK-CROSSGEN-DESIGN.md SS11.3 + SS11.20 item 12(c): the ONE entry a native
 * "GBC1" Bank cell leaves the Bank through. Called from drop_held() the moment a
 * native carry is dropped on a NON-Bank destination, BEFORE xg_native_escape_denied
 * -- that gate still guards every RAW 80-byte write below it, and this call never
 * falls through to one. Either the cell LANDED (destination persisted AND the Bank
 * source consumed) or NOTHING anywhere changed and the hand keeps holding. */
/* BankDownResult (REFUSED / LANDED / CONVERTED) lives in bank_down_convert.h (S150-8). */

/* The XG_DOWN_ARM_EXACT arm: land the cell in a Game Boy save of the SAME generation
 * via the vtable's accept_down hook, then the ONE Bank consume (D7/D12). G-H1's
 * ordering, applied to a GB destination: the consume runs ONLY after accept_down's
 * internal gb_persist() returned true, and it is IMMEDIATE, not queued -- unlike a
 * Gen-3 PC write (app_bank_defer_delete, deferred to the exit save), gb_persist already
 * wrote the card by the time control returns here.
 *
 * D-Q7 PLUMBING FIX: reads `s_xfer_peer` (this file's own module-singleton, above),
 * NOT `src->xfer` -- historically `pdna_gen12_source()` never set its returned
 * BoxSource's `.xfer` field (BACKLOG #171b fixed that at the 1817790 merge: it now
 * assigns `&k_gb_xfer`, the SAME static const object, so either read resolves to it;
 * `k_gb_xfer.gen` is still 0 -- never derive dst_gen from `src->xfer->gen`); the peer
 * that has always been populated is
 * `s_xfer_peer` via `pdna_box_xfer_set(&k_gb_xfer)`, called once per GB session visit
 * from gb_session_core() and read directly by drop_held's own UP-carry sites (:1222/
 * :1238/:1286 above). Using `src->xfer` here would make bank_down_dispatch's EXACT arm
 * ALWAYS see a NULL vtable and refuse every drop silently -- the exact "has no way out"
 * failure this whole slice exists to fix. Confirmed on the emulator (step 7's delta
 * shots): with `src->xfer` the MOVE/DOWN/A gesture never reaches accept_down at all. */
static BankDownResult bank_down_exact(BoxSource* src, int dst_box, const uint8_t cell80[80]) {
  (void)src;
  if (!s_xfer_peer || !s_xfer_peer->accept_down) { snd_deny(); return BANK_DOWN_REFUSED; }
  boxoam_suspend();
  bool ok = s_xfer_peer->accept_down(dst_box, cell80);
  boxoam_resume();
  if (!ok) return BANK_DOWN_REFUSED;          /* the hook already said why */

  uint8_t slots1[1]; uint8_t recs1[1][80];
  slots1[0] = (uint8_t)s_orig_slot; memcpy(recs1[0], cell80, 80);
  /* REVIEW F3: app_bank_clear_slots() is a full SD read+write (box_save(), pdna_bank.c)
   * -- OS-mode rule (hard rule 1), the box OAM animation must be suspended for the
   * whole SD transfer, same as this file's other consume site (:2135's
   * gb_release_up_hook path, which keeps its own boxoam_suspend/resume bracket around
   * the write). This one previously resumed right after accept_down (:1136 above) and
   * never re-suspended before the consume -- hardware-only (mGBA has no OS-mode ROM
   * disappearance to reproduce), but a real EZ-Flash Omega DE could have painted a
   * cursor-bob frame mid-transfer. */
  boxoam_suspend();
  if (!app_bank_clear_slots(s_orig_box, slots1, (const uint8_t (*)[80])recs1, 1)) {
    /* D7: the reconcile (S150-11) cannot help here -- the EXACT arm writes NO
     * /PokeDNA/xfer/ entry (D6) for it to walk. Say so explicitly: the game HAS the
     * mon now, the Bank slot is a DUPLICATE the player can delete themselves, never a
     * loss. Still BANK_DOWN_LANDED -- the operation succeeded from the player's view;
     * only the Bank-side cleanup didn't, and that is reported, not silently retried. */
    snd_error();
    /* REVIEW F4: PDNA_XFER_DOWN_DUP_L1 ("The game save HAS it now.") is the
     * reassuring half of D7's message -- used here as the static first line; the
     * dynamic box/slot naming moves to the second line. */
    char l2[40];
    siprintf(l2, "Bank box %d slot %d", s_orig_box + 1, s_orig_slot + 1);
    msg_wait(PDNA_XFER_DOWN_DUP_TITLE, UI_WARN, PDNA_XFER_DOWN_DUP_L1, l2);
    boxoam_resume();
    log_line("gen12: bank-down consume failed -- Bank box %d slot %d still holds a "
             "duplicate (the game save already has it)", s_orig_box, s_orig_slot);
    return BANK_DOWN_LANDED;
  }
  boxoam_resume();
  snd_save();
  return BANK_DOWN_LANDED;
}

/* BACKLOG #150 S150-7 decision D1/D-Q5 (CANONICAL spelling for the parallel S150-8
 * lane -- see the brief's "Known divergence" section): a switch over xg_bank_down_arm()
 * whose EXACT case calls bank_down_exact(); the GB_BRIDGE and GEN3 cases are each
 * exactly one line so S150-8's merge replaces just those two lines with calls into its
 * own new file. `dst_cell` is unused by the EXACT arm (a Game Boy list always appends
 * at its own next free slot -- see gb_create_hook's own comment) and is carried for
 * S150-8's Gen-3 arm, which needs the exact cell. noinline: must not inline into
 * drop_held, which sits on the box-screen stack chain. */
static BankDownResult __attribute__((noinline))
bank_down_dispatch(BoxSource* src, int dst_box, int dst_cell, const uint8_t cell80[80],
                   const uint8_t dstrec[80], uint8_t out80[80]) {
  switch (xg_bank_down_arm(bc_kind(cell80), src->scope, app_gb_session_gen())) {
    case XG_DOWN_ARM_EXACT:     return bank_down_exact(src, dst_box, cell80);
    case XG_DOWN_ARM_GB_BRIDGE: return bank_down_convert_gb(src, dst_box, dst_cell, cell80);
    case XG_DOWN_ARM_GEN3:      return bank_down_convert_gen3(src, dst_box, dst_cell, cell80, dstrec, out80);
    case XG_DOWN_ARM_NONE:
    default:                    return BANK_DOWN_REFUSED;
  }
}

/* BACKLOG #150 S150-8b, D-Q5: no BoxSource.xfer vtable on the Bank (pdna_box.h:116-117's
 * "NULL on every Gen-3 source, forever" stands) -- the RESTORE hook is called DIRECTLY
 * from drop_held's PC->Bank arm below, gated on this ledger lookup. Both halves are
 * noinline with their OWN GBSC_FILE_MAX (1042 B) frame -- D-Q6 asked for this to avoid
 * that frame on drop_held's own deepest path; measured instead of assumed:
 * `stack_budget.py --root drop_held` reports drop_held's own worst path at 4,560 of
 * 15,032 B (10,472 B of local margin) on this lane's base, nowhere near the program's
 * actual deepest root (11,912 B, a completely different call chain) -- adding 1042 B
 * here cannot move either number. See the delivery report for the full measurement;
 * this is a declared, numbers-backed deviation from D-Q6's literal wording, not an
 * edit to the frozen source/xfer_io.* (which D-Q6's "add an accessor... if none
 * exists" would otherwise have required). */
static int __attribute__((noinline))
pc_bank_restore_up(const uint8_t g3_rec80[80], uint8_t out_cell80[80]) {
  if (!app_can_edit()) return 0;              /* decision 13: nothing to restore, read-only cart */
  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = 0;
  uint64_t key = xr_key_g3(g3_rec80);

  /* BACKLOG #206 review R1: pick loop + RESTORED/PENDING refusals moved to
   * source/xfer_rec.c's xr_restore_pick_basic (see its contract comment) --
   * behaviour byte-identical, same S150-9 decision 8 tiebreak/order.
   * BACKLOG #175c: bounded 2-attempt loop (golden rule 2's provable bound) --
   * attempt 0 may offer SAVE NOW? on a PENDING pick and re-read once (attempt 1)
   * so a successful promotion falls through into the SAME restore flow below
   * instead of a second copy of it; attempt 1 never offers again (decision 9:
   * only one unpromoted transfer can ever exist, so a re-read that is STILL
   * PENDING after a verified save-now is a real refusal, not a retry loop). */
  GbscEntry e;
  XrRestorePick pick = XR_PICK_NONE;
  for (int attempt = 0; attempt < 2; attempt++) {
    SfStatus rst = xr_open(key, buf, sizeof buf, &len, NULL);
    if (rst == SF_ERR_OPEN) return 0;         /* no ledger entry -- an ordinary Gen-3 mon */
    if (rst != SF_OK) {
      log_line("bank: restore lookup: %s", sf_status_str(rst));
      return -1;
    }
    int count = gbsc_count(buf, len);
    if (count < 0) { log_line("bank: restore lookup: ledger file failed to validate"); return -1; }
    pick = xr_restore_pick_basic(buf, len, count, &e);
    /* BACKLOG #175c review D2: only offer SAVE NOW? when THIS session's own
     * unpromoted transfer (g_xd_key/g_xd_idx) is the entry blocking us -- a PENDING
     * entry left by an EARLIER session can never be promoted (app_xfer_promote()
     * only ever acts on the RAM key), so offering here would run a real verified
     * app_commit_pc() and then report PDNA_XFER_NOTSAVED_* regardless. Falling
     * through with pick still XR_PICK_REFUSE_PENDING reaches the honest SAVE FIRST
     * wall below instead. */
    if (pick != XR_PICK_REFUSE_PENDING || attempt == 1 || !app_xfer_pending_is(key)) break;
    log_line("bank: restore: entry still PENDING -- offering SAVE NOW? instead of the flat wall");
    boxoam_suspend();
    char l1[64];
    siprintf(l1, "%s %s", PDNA_XFER_SAVENOW_L1, PDNA_XFER_SAVENOW_L2);
    bool yes = app_confirm(PDNA_XFER_SAVENOW_TITLE, l1);       /* A = save now, B = no */
    bool ok  = yes && app_xfer_save_now();  /* the helper shows its own NOTSAVED on failure */
    boxoam_resume();
    if (!ok) { snd_deny(); return -2; }     /* the helper/confirm already said why */
    /* saved -- loop once more to re-read the now-promoted entry (crosses the SAME
     * xr_open()/xr_restore_pick_basic() this function already used, never a second
     * implementation). */
  }
  if (pick == XR_PICK_NONE) return 0;         /* only Gen-3-home entries (or none) -- already exact */
  if (pick == XR_PICK_REFUSE_RESTORED) {
    log_line("bank: restore: entry already RESTORED -- refusing a second restore");
    boxoam_suspend();
    snd_deny();
    msg_wait(PDNA_XFERDUP_TITLE, UI_WARN, PDNA_XFERDUP_L1, PDNA_XFERDUP_L2);
    boxoam_resume();
    return -2;   /* review D3's convention: a declined/refused restore, not a genuine failure */
  }
  if (pick == XR_PICK_REFUSE_PENDING) {
    /* BACKLOG #175c: still PENDING after a VERIFIED save-now -- decision 9 says
     * this cannot be the same entry (one unpromoted transfer at a time), so this
     * is a genuine second refusal, not the flat wall this lane replaced. */
    log_line("bank: restore: entry still PENDING after save-now -- refusing");
    boxoam_suspend();
    snd_deny();
    msg_wait(PDNA_XFER_SAVEFIRST_TITLE, UI_WARN, PDNA_XFER_SAVEFIRST_L1, PDNA_XFER_SAVEFIRST_L2);
    boxoam_resume();
    return -2;
  }
  if (e.state == XR_STATE_NONE) {
    /* A pre-state-byte entry cannot exist for a NATIVE_HOME kind -- xfer_down_write
     * always stamps PENDING (decision 8's own note). Treat as CLAIMED and restore. */
    log_line("restore: native-home entry with state NONE");
  }

  /* decision 6/7: probe with accept=0 (nothing applied, the report alone) to decide
   * whether the screen needs to show at all -- decision 7's own rule: XR_MERGE_DOWN
   * draws only when at least one row exists (app_xfer_merge_screen returns true with
   * *accept=0 itself when there is nothing to say, so a byte-identical restore stays
   * silent exactly as it did before this commit). */
  GbEditMon probe;
  XrMergeReport rep;
  if (!xr_merge_down_sel(&e, g3_rec80, 0, &probe, &rep)) {
    log_line("bank: restore: xr_merge_down_sel probe failed");
    return -1;
  }

  boxoam_suspend();
  uint8_t accept = 0;
  bool confirmed = app_xfer_merge_screen(&rep, XR_MERGE_DOWN, &accept);
  boxoam_resume();
  if (!confirmed) return -2;   /* review D3: B, user declined -- distinct from a genuine failure */

  uint32_t serial = pdna_bank_next_serial();
  if (serial == 0) {
    log_line("bank: restore: bank_serial allocation failed");
    return -1;
  }
  int rc = bank_restore_from_entry(&e, g3_rec80, accept, serial, out_cell80, NULL);
  if (rc != 1) { log_line("bank: restore: bank_restore_from_entry rc=%d", rc); return -1; }
  return 1;
}

/* Marks the entry LAST, after the Bank write has already landed (D-Q1/D-Q2, decision
 * 3's step 7 / §3.2's commit order). D-Q2: MARK, do not remove -- app_pc_release_slot
 * only marks the PC dirty, so a user who then declines the exit save would keep a
 * record-less Gen-3 duplicate if the entry were gone. Review F2: `gb_sidecar.h` is
 * unfrozen for exactly one line (XR_STATE_RESTORED, S150-8b) -- gbsc_set_claimed()
 * (the first cut of this function) was a byte-for-byte no-op, because
 * xfer_down_write() already sets `claimed = 1` at BIRTH (source/pdna_gen12.c),
 * so S150-11's reconcile had no signal to tell a restored entry apart from an
 * ordinary pending/claimed one. This writes a remove-then-gbsc_add rewrite so the
 * entry's own crc16 stays correct -- BACKLOG #215(c): app_xfer_promote()
 * (source/pdna_main.c) no longer matches this description; it now uses
 * gbsc_set_state(), which flips only the state bits of the entry's flags byte
 * IN PLACE and therefore PRESERVES every other bit, including bank_keep (b6,
 * "KEEP BOTH was chosen for this row"). This site's own gbsc_add() call does
 * NOT carry bank_keep forward (its ef_compose() never sets b6), so marking an
 * entry RESTORED here silently drops a prior KEEP BOTH choice. Known gap, not
 * fixed by this pass (BACKLOG #213/#215 review F5 is comments-only). */
static void __attribute__((noinline))
pc_bank_restore_done(const uint8_t g3_rec80[80]) {
  if (!app_can_edit()) return;                /* decision 13 */
  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = 0;
  char path[GBSC_PATH_MAX];
  uint64_t key = xr_key_g3(g3_rec80);
  SfStatus rst = xr_open(key, buf, sizeof buf, &len, path);
  if (rst != SF_OK) { log_line("bank: restore done: re-open failed (%s)", sf_status_str(rst)); return; }
  int count = gbsc_count(buf, len);
  if (count < 0) { log_line("bank: restore done: ledger file failed to validate"); return; }

  int best = -1;                              /* re-resolve by the SAME rule the lookup used */
  for (int i = 0; i < count; i++) {
    GbscEntry cand;
    if (!gbsc_get(buf, len, i, &cand)) continue;
    if (cand.kind != XR_KIND_NATIVE_HOME) continue;
    if (!bc_is_native(cand.original80)) continue;
    best = i;
  }
  if (best < 0) { log_line("bank: restore done: entry vanished before marking"); return; }

  GbscEntry e;
  if (!gbsc_get(buf, len, best, &e)) { log_line("bank: restore done: gbsc_get failed"); return; }
  /* S150-9 decision 8: mark RESTORED only an entry that is CLAIMED/NONE -- a
   * RESTORED entry re-resolved here (defensive; pc_bank_restore_up's own state
   * branch already refuses this case before the write) is left alone, never
   * re-marked, never a second time. */
  if (e.state == XR_STATE_RESTORED) {
    log_line("bank: restore done: entry already RESTORED -- left alone");
    return;
  }
  e.state = XR_STATE_RESTORED;
  if (gbsc_remove(buf, &len, best) != 0) { log_line("bank: restore done: remove failed"); return; }
  if (gbsc_add(buf, &len, GBSC_FILE_MAX, &e) < 0) { log_line("bank: restore done: re-add failed"); return; }
  /* A failure here is LOGGED and swallowed, never shown and never fatal -- decision 7's
   * own posture: the native cell is already verified on the card, so a surviving,
   * unmarked entry is a residual duplicate risk for a future lane, never a loss. */
  if (sf_write_verified(path, buf, len) != SF_OK) {
    log_line("bank: restore done: rewrite failed for %s", path);
  } else {
    app_xv_cache_invalidate();   /* BACKLOG #213: a real ledger write */
  }
}

/* BACKLOG #168a review D1: pdna_bank_peek_box() re-pages the ONE shared box buffer,
 * which is the SAME buffer `recs` points into -- handing the cached pointer back for
 * self_box made the scan compare the destination box against box (self_box-1)'s bytes,
 * i.e. never scan the destination box at all unless it was box 0. Page every box,
 * self included, through the one reader. The re-page below restores `recs`. */
static const uint8_t* bank_scan_get(int b, void* ctx) {
  (void)ctx;
  return pdna_bank_peek_box(b);
}

/* BACKLOG #170 (from the s150-4-5 review A11): drop_held's UP branch (a Game Boy
 * mon carried into an empty Bank cell), extracted verbatim into its own noinline
 * helper -- same signature as drop_held itself (BoxSource*, box, cur, recs, done),
 * so drop_held's own call site is a plain one-line tail return and every
 * s_orig_box/s_orig_slot/s_orig_scope/s_xfer_peer/s_held file-static this branch
 * reads/writes needs no plumbing. BACKLOG #168a review D4: the measured truth,
 * not "sheds held_copy[80] (80 B)" -- drop_held's own frame went 240 -> 232 B
 * (GCC overlapped the locals it still has), and the UP path itself is +144 B net
 * (4,640 -> 4,784 B guarded, no sibcall: drop_held_up is a real, separate frame on
 * the stack). The deepest chain is unchanged at 13,112 of 15,024 (Settings ->
 * register-ROM browser, 8.3 KB above this path either way) -- the extraction is
 * for readability and pin anchoring (BACKLOG #170's own point), not for a stack
 * saving.
 *
 * ORDER (decision 1, unchanged by this extraction): the Bank write is committed
 * and VERIFIED first; only then does the Game Boy save lose the mon (release_up).
 * A failed Bank write reverts the cell and KEEPS HOLDING (never touches the GB
 * save); a failed release_up leaves a DUPLICATE (the Bank already has it, the GB
 * save still has it too) -- a duplicate is visible and repairable, a loss is not.
 * BACKLOG #150 S150-12 decision 6: `s_xfer_peer->lift_up`, not `->release_up` --
 * this branch also carries a COPY (no release_up at all) all the way to the
 * commit; the release_up call further down is itself gated (else: queue for the
 * PC instead of deleting).
 *
 * tests/host_escape_gate_sites_test.py's structural pins (i)/(n1) and their MUT
 * H/MUT N1 self-mutation demonstrations now anchor on THIS function's body
 * (re-anchored in the same commit that moved the code -- BACKLOG #170's own
 * requirement: a move without re-anchoring must fail loudly, not silently stop
 * checking anything). */
static uint8_t* __attribute__((noinline)) drop_held_up(BoxSource* src, int box, int cur, uint8_t* recs, bool* done) {
  boxoam_suspend();
  /* BACKLOG #199 (lane b199): the Bank's price -- the one-time-per-save origin
   * prompt, the sidecar-ledger refusal, and pdna_bank_next_serial()'s own VERIFIED
   * bank.meta write -- is paid HERE, at the one drop that actually needs it, not at
   * grab time (start_carry, above, now just copies the display record for every
   * scope). `s_xfer_peer->lift_up` is called by the carry's own ORIGIN coordinates
   * (s_orig_box/s_orig_slot), the same coordinate form gb_release_up_hook's own
   * re-verify already uses -- robust to an L/R repage of the GB display between the
   * grab and this drop, unlike the retired rec80 form (which needed
   * g_m->loaded == s_orig_box to resolve at all). `packed` is the real native
   * "GBC1" cell from here on; s_held stays the display copy and is never written to
   * the Bank. A refusal (the origin prompt cancelled, a sidecar entry already
   * exists, or the serial write failed) returns "still holding" -- the same shape
   * as every other refusal in this function, so a carried mon can never be
   * silently stranded nor land in neither save. BACKLOG #199 review D5: lift_up
   * is now a TRI-STATE (XG_LIFT_OK/CANCELLED/FAILED, pdna_box.h) -- a plain B
   * decline or a refusal that already drew its OWN dialog (a stale ledger
   * record, an already-RESTORED entry, the PENDING SAVE FIRST wall) comes back
   * CANCELLED and shows NOTHING further here, matching pc_bank_restore_up's own
   * rc==-2 convention (the Gen-3 twin, just above) and app_confirm's "B = no"
   * house style; only a genuinely UNREPORTED failure (an unreadable ledger, a
   * failed serial write, a pack failure) shows this generic dialog. */
  uint8_t packed[80];
  int lift_rc = s_xfer_peer->lift_up(s_orig_box, s_orig_slot, packed);
  if (lift_rc != XG_LIFT_OK) {
    if (lift_rc == XG_LIFT_FAILED) {
      /* review D6: this was the only refusal in the function with no sound --
       * every sibling below (pdna_bank_prepare_native's own refusal, the
       * collision refusal further down) calls snd_error() before its own
       * msg_wait(). Folded into D5's own tri-state edit: a CANCELLED (silent)
       * return never reaches here, so this stays paired 1:1 with the ONE
       * dialog left standing. */
      snd_error();
      msg_wait(PDNA_XFER_LIFT_REFUSED_TITLE, UI_WARN, PDNA_XFER_LIFT_REFUSED_L1, PDNA_XFER_LIFT_REFUSED_L2);
    }
    boxoam_resume();
    log_line("bank: up box %d slot %d -> bank box %d slot %d: lift %s", s_orig_box, s_orig_slot, box, cur,
              lift_rc == XG_LIFT_CANCELLED ? "cancelled" : "refused");
    app_log_flush();
    return recs;                                          /* still holding */
  }
  if (!pdna_bank_prepare_native()) {
    snd_error();
    msg_wait(PDNA_XFER_PREP_TITLE, UI_WARN, PDNA_XFER_PREP_L1, PDNA_XFER_PREP_L2);
    boxoam_resume();
    log_line("bank: up box %d slot %d -> bank box %d slot %d: backup gate refused", s_orig_box, s_orig_slot, box, cur);
    app_log_flush();
    return recs;                                          /* still holding */
  }
  /* BACKLOG #197: pdna_bank_prepare_native() -> bank_backup_v1() sets g_loaded =
   * -1 on its FIRST-EVER-run success path (pdna_bank.c, "force a fresh page-in"),
   * discarding whatever box g_bankbuf held. `recs` here is a plain pointer into
   * that same buffer, still holding the STALE contents from before prepare ran
   * (or garbage, on the very first entry) -- src->records(box) (box_load) must
   * re-page box's real records before the collision scan below reads them and
   * before the commit below writes into them, or the commit sees g_loaded < 0
   * and box_save() refuses outright ("bank write failed", still holding) even
   * though the write itself was never attempted on real data. On every call
   * AFTER the first (marker already DONE), bank_backup_v1's O(1) early return
   * never touches g_loaded, so this re-page is a same-box no-op paging call --
   * cheap, and required unconditionally since the caller cannot tell which case
   * it is in. */
  recs = src->records(box);
  /* decision 10: the ident32 collision refusal, NOT a re-pack -- the serial is
   * monotonic and persisted before use, so a collision means the meta was lost
   * or rolled back. A plain memcmp on bytes 0..7 (magic + ident32) against each
   * CANDIDATE cell already stored in a Bank box -- not a recomputed bc_ident32()
   * on the candidate (a match on bytes 0..3 alone already implies "GBC1" for
   * THAT side, so no separate bc_is_native() check is needed on it).
   * BACKLOG #246 review F5 fix: `packed` itself is a DIFFERENT matter -- since
   * gb_lift_restore_g3home (source/pdna_gen12.c), it can be a PLAIN Gen-3
   * record (bc_is_native(packed) == false by construction: gen3_edit_commit
   * never writes the GBC1 tag), and for that shape bytes 0..7 are PID + OT ID,
   * not magic + ident32 at all -- comparing them against every stored cell's
   * ident32 span is comparing two unrelated fields, and a false "BANK RECORD
   * CLASH" refusal follows whenever another cell happens to share that PID+OTID.
   * No corruption either way (bc_unpack rejects the false match before commit),
   * but this whole scan -- serial resync, repack, ident32 clash -- is a
   * native-cell-only concept, so it is now gated on bc_is_native(packed) below.
   * BACKLOG #168a (REVIEW F5's own follow-up): scans ALL 16 Bank boxes, not just
   * the destination -- a duplicate serial landing in another box used to be
   * invisible here, and S150-6/S150-7 would mis-target it. One box buffer at a
   * time through pdna_bank_peek_box() (never a second 2,400-B buffer on the
   * stack): bank_scan_get()/bank_ident32_collision() (bank_collision.c, a pure
   * host-tested core; tests/host_bank_collision_test.c).
   * BACKLOG #168a review D2: `trusted` says only that the LAST meta_load() this
   * session saw a clean primary -- it cannot see a bank.meta restored from .bak,
   * an older /PokeDNA/bank copied back, or box files from a second card dropped
   * in beside this card's meta. Pay the full 16-box scan ONCE per session (15
   * extra 2,400-B reads, on a deliberate user action), then trust it. */
  static EWRAM_BSS bool s_up_scan_done;   /* EWRAM: an IWRAM static would cost 8 B of stack budget (re-verify) */
  /* BACKLOG #246 review F5 fix: wraps the WHOLE native-only scan (not folded into
   * the inner guard's own condition) so the #168a review D2 structural test can
   * still find the exact `if (!pdna_bank_serial_trusted() ... s_up_scan_done ...)`
   * line its latch checks pin -- see that test's own comment for why the latch
   * form matters (a bare `if (!pdna_bank_serial_trusted())` silently skips the
   * scan forever on an ordinary card). */
  if (bc_is_native(packed)) {
  if (!pdna_bank_serial_trusted() || !s_up_scan_done) {
    /* BACKLOG #223 review D4/D5, fused into one pass by #206 fixes2 R2
     * (bank_scan_serial_and_clash, bank_collision.c): a rolled-back counter
     * (BACKLOG #219's .bak recovery) can hand serial S to a DIFFERENT mon than the
     * one that originally held it -- no ident32 clash results (the two cells' other
     * 76 bytes differ), so a collided-only gate would never notice and would
     * silently duplicate the serial, #168's own hazard. The high-water mark
     * (stored_max) repairs a rolled-back counter even when the stale serial lands
     * on a mon that does NOT collide -- computed by the SAME scan that answers
     * `collided`, so the resync decision below always sees a mark from BEFORE
     * `packed` was touched. BACKLOG #199: this whole block reads/re-packs `packed`
     * (the just-lifted native cell), never `s_held` (still the display copy, never
     * native-formatted for a GB carry any more -- see start_carry's own comment). */
    uint32_t stored_max = 0;
    int coll_box = -1, coll_slot = -1;
    bool collided = bank_scan_serial_and_clash(bank_scan_get, NULL, PDNA_BANK_BOXES, G3_BOX_SLOTS,
                                               box, cur, packed, &stored_max, &coll_box, &coll_slot);
    GbEditMon rmon; BcMeta rmeta;
    bool repacked = false;
    if (pdna_bank_serial_resync(stored_max) && bc_unpack(packed, &rmon, &rmeta)) {
      uint32_t fresh = pdna_bank_next_serial();
      if (fresh != 0) {
        (void)bc_pack(&rmon, rmeta.flags, rmeta.origin_game, rmeta.rtc_epoch, fresh, packed);
        repacked = true;
      }
    }
    /* re-serialising changes ident32 BY CONSTRUCTION (bc_pack folds bank_serial
     * into the hash) -- the fused pass's `collided` verdict above described the
     * PRE-repack `packed` and no longer applies. Re-probe ONLY when a repack
     * actually happened (rare: only a rolled-back counter reaches here); the
     * ordinary case (no resync, or resync declined/failed) reuses the first
     * pass's verdict untouched, same one-scan cost as before this fix. */
    if (repacked) {
      collided = bank_ident32_collision(bank_scan_get, NULL, PDNA_BANK_BOXES, G3_BOX_SLOTS,
                                        box, cur, packed, &coll_box, &coll_slot);
    }
    if (collided) {
      snd_error();
      /* BACKLOG #219b: was a log line + snd_error() only -- the player saw nothing.
       * msg_wait BEFORE boxoam_resume(), same as the backup-gate refusal above (the
       * comment on the write-failure branch below says this bracket's own reason:
       * sprites must be off while a dialog draws). */
      msg_wait(PDNA_BANK_COLL_TITLE, UI_WARN, PDNA_BANK_COLL_L1, NULL);
      boxoam_resume();
      log_line("bank: up box %d slot %d -> bank box %d slot %d: ident32 collision at box %d slot %d, refusing", s_orig_box, s_orig_slot, box, cur, coll_box, coll_slot);
      app_log_flush();
      return recs;                                        /* still holding */
    }
    if (pdna_bank_serial_trusted()) s_up_scan_done = true;   /* review B1: a scan forced by an untrusted serial never latches -- the next lift re-scans until a session started trusted */
    /* pdna_bank_peek_box() re-pages the ONE shared bank buffer for whichever
     * box it last read (S150-11 decision 19's own contract) -- if the scan
     * touched any OTHER box, `recs` (same pointer value) now aliases THAT
     * box's bytes, not `box`'s. Re-page the destination before writing. */
    recs = src->records(box);
  }
  }   /* BACKLOG #246 review F5 fix: closes the bc_is_native(packed) wrap above */
  memcpy(recs + (uint32_t)cur * 80, packed, 80);
  bool ok = src->commit();                                 /* verified bank box_save */
  if (!ok) {
    memset(recs + (uint32_t)cur * 80, 0, 80);
    /* REVIEW F4: resume here, not right after commit() -- the caller (gb_persist,
     * via release_up below) draws its own "Saving -- do not power off" panels and
     * msg_wait draws PDNA_XFER_KEPT_*; both must render with the box sprites OFF,
     * same as the backup-gate/collision refusals above. */
    boxoam_resume();
    snd_error();
    log_line("bank: up box %d slot %d -> bank box %d slot %d: bank write failed", s_orig_box, s_orig_slot, box, cur);
    app_log_flush();
    return recs;                                          /* still holding */
  }
  int gb_box = s_orig_box, gb_slot = s_orig_slot;
  /* BACKLOG #199: held_copy is the just-committed NATIVE bytes (`packed`), not
   * `s_held` -- release_up's own re-verify (gb_release_up_hook) compares this
   * against the origin card's bytes, and only the native cell IS what the Bank now
   * holds. */
  uint8_t held_copy[80]; memcpy(held_copy, packed, 80);
  s_holding = false; *done = true;                         /* hand empties BEFORE source cleanup (§11.2 step 6) */
  /* BACKLOG #150 S150-12 decision 6: a vtable with no release_up (the read-only
   * mount's k_gb_xfer_ro) never deletes -- the Bank commit above already landed
   * the copy; queue it for the PC offer instead of trying to delete a GB save
   * this session structurally cannot write. */
  if (s_xfer_peer->release_up) {
    if (!s_xfer_peer->release_up(gb_box, gb_slot, held_copy)) {
      snd_error();
      msg_wait(PDNA_XFER_KEPT_TITLE, UI_WARN, PDNA_XFER_KEPT_L1, PDNA_XFER_KEPT_L2);
      log_line("bank: up box %d slot %d -> bank box %d slot %d: release refused, duplicate", gb_box, gb_slot, box, cur);
      app_log_flush();
    } else {
      snd_save();
      log_line("bank: up box %d slot %d -> bank box %d slot %d: ok", gb_box, gb_slot, box, cur);
    }
  } else {
    app_pc_queue_note(box);
    snd_save();
    msg_wait(PDNA_XFER_COPIED_TITLE, UI_TEXT, PDNA_XFER_COPIED_L1, PDNA_XFER_COPIED_L2);
    log_line("bank: copy box %d slot %d -> bank box %d slot %d: ok, queued for the PC", gb_box, gb_slot, box, cur);
  }
  boxoam_resume();                                         /* REVIEW F4: covers gb_persist's own panels + PDNA_XFER_KEPT_* above */
  return recs;
}

/* BACKLOG #246 (#104 Phase 1): the DOWN mirror of drop_held_up above -- a PLAIN
 * Gen-3 Bank cell (never native) carried onto a Game Boy grid and dropped. Own
 * noinline frame for the same reason drop_held_up gets one (BACKLOG #170): gen3_to_gb_fixed
 * + the loss/legal screens are exactly the kind of weight that must not sit on
 * drop_held's own worst-case stack path, which runs on every grid A-press, not
 * just a carry.
 *
 * ORDER, mirroring drop_held_up's own contract (its doc comment, decision 1): the
 * Game Boy save is written and VERIFIED first (gb_bank_down_g3 -> gb_persist,
 * BANK_DOWN_LANDED only after a real card write); only THEN does the Bank lose the
 * cell (app_bank_clear_slots). A refused landing reverts nothing and keeps holding
 * (gb_bank_down_g3 has already said why, on screen); a landed-but-not-consumed
 * failure leaves a DUPLICATE -- the game HAS it, the Bank slot is a repairable
 * leftover, never silently lost -- same shape as every OTHER Bank-down consume in
 * this file (bank_down_exact's own comment, just above, makes the identical
 * argument). `s_held` IS the 80-byte record: handed to gb_bank_down_g3 and to the
 * consume directly, no extra 80-B copy (the escape-gate structural test treats
 * every 80-B memcpy in drop_held as a write that must sit below the gate; this
 * function adds none). */
static uint8_t* __attribute__((noinline))
drop_held_down_g3(BoxSource* src, int box, int cur, uint8_t* recs, bool* done) {
  (void)cur;   /* BACKLOG #246 review D7: never read -- a Game Boy list always appends at
                * its own next free slot (gbs_insert), never at the cursor cell; `cur`
                * stays in the signature only to match drop_held_up's own sibling shape. */
  boxoam_suspend();
  bool landed = gb_bank_down_g3(box, s_held) == BANK_DOWN_LANDED;
  boxoam_resume();
  if (!landed) {
    log_line("gen12: g3-down box %d slot %d -> gb box %d: refused/not landed",
             s_orig_box, s_orig_slot, box);
    return recs;                                            /* still holding -- the arm already said why */
  }

  s_holding = false; *done = true;
  { uint8_t slots1[1]; slots1[0] = (uint8_t)s_orig_slot;
    boxoam_suspend();                                        /* hard rule 1: SD write with OAM off */
    if (!app_bank_clear_slots(s_orig_box, slots1, (const uint8_t (*)[80])s_held, 1)) {
      snd_error();                                           /* D7's own shape: the game HAS it; the Bank keeps a duplicate */
      char l2[40]; siprintf(l2, "Bank box %d slot %d", s_orig_box + 1, s_orig_slot + 1);
      msg_wait(PDNA_XFER_DOWN_DUP_TITLE, UI_WARN, PDNA_XFER_DOWN_DUP_L1, l2);
    }
    boxoam_resume(); }
  s_oam_reload = true;
  recs = src->records(box);   /* the GB list grew -- repaint from the image */
  log_line("gen12: g3-down box %d slot %d -> gb box %d: ok", s_orig_box, s_orig_slot, box);
  return recs;
}

/* Drop the held mon onto cursor cell `cur`. Within the origin's scope: true move (place +
 * clear origin; swap if occupied). Across the PC<->Bank boundary: COPY onto an empty cell
 * only (origin kept) so a mon can't be lost between two save scopes. *done=true when the
 * hand is empty afterwards. Returns the (maybe reloaded) recs. */
static uint8_t* drop_held(BoxSource* src, int box, int cur, uint8_t* recs, bool* done) {
  *done = false;
  if (s_orig_slot >= 0 && same_scope(src) && s_orig_box == box && cur == s_orig_slot) {
    s_holding = false; *done = true; return recs;            /* dropped back on its own cell */
  }
  /* BACKLOG #150 S150-8 decision 11/12 (as adapted for this lane's base -- S150-7's
   * `bank_down_dispatch` does not exist on disk, see the delivery report): a native
   * "GBC1" cell converting DOWN, either into the Gen-3 PC (BANK_DOWN_CONVERTED) or
   * straight into a Game Boy save of the OTHER generation (BANK_DOWN_LANDED). The
   * bridge arm (LANDED) has ALREADY written its own destination -- not a PC record
   * at all, so it returns immediately, same shape as the UP drop's own tail. The
   * Gen-3 arm (CONVERTED) has NOT: `conv` holds the finished record and `converted`
   * makes control FALL THROUGH into the EXISTING "BANK -> PC true MOVE" branch
   * below, which places `conv` (not `s_held`) -- reusing that branch's own already-
   * declared BoxSource.note_add/records call sites rather than adding a new one
   * (tools/stack_edges.txt's GATED-4 gate forbids a new caller; a second reason,
   * independent of gate wiring, this reuse is also required for:
   * tests/host_escape_gate_sites_test.py's structural check that every 80-byte
   * memcpy in this function is preceded, in TEXT ORDER, by the ONE
   * `xg_native_escape_denied(` call below -- reusing the existing memcpy keeps that
   * true without adding a second gate call, which would break the file's own exact
   * count check). `s_held` stays untouched (still the native cell) so
   * `app_bank_defer_delete`'s own identity match (pdna_bank.c's BANK_DEL_IDLEN
   * memcmp against the first 8 bytes handed at drop time) still matches the Bank
   * slot's real native bytes. */
  /* BACKLOG #150 S150-8 review F7: hoisted from further below (its own comment stays
   * there) so the dispatch condition just below can gate the GEN3 arm on `!occupied` -- a
   * refused-for-occupied drop must never even CALL bank_down_dispatch, so the Gen-3
   * arm's own ledger write (xfer_down_write, decision 8) can never run for a
   * destination this call site is about to refuse anyway, which would otherwise
   * orphan an XR_PENDING entry AND leave decision 9's g_xd_key/g_xd_idx pointing at
   * a transfer nothing will ever complete. */
  bool occupied = g_box[cur].species != 0 || bc_is_native(recs + (uint32_t)cur * 80) ||
                 (src->scope == BOXSCOPE_BANK && app_bank_slot_pending(box, cur));   /* the REAL Bank's deferred-delete queue, never a GB box index (S1 review D1) */
  uint8_t conv[80]; bool converted = false;
  /* The arm is pure and cheap; derive it ONCE here. The `occupied` refusal (S150-8
   * review F7: never let the Gen-3 arm write a ledger entry for a drop this site is
   * about to refuse) applies to the GEN3 arm ONLY -- a Game Boy list appends at its
   * own next free slot, so for EXACT/GB_BRIDGE the cell under the cursor is
   * irrelevant and the accept_down hook decides (incl. the party-full offer, which
   * is reachable ONLY through a drop on a fully occupied party -- the union's
   * unscoped `!occupied` made it unreachable; caught by the merged-tree shot lane,
   * 2026-09-16). */
  uint8_t arm = xg_bank_down_arm(bc_kind(s_held), src->scope, app_gb_session_gen());
  if (s_orig_scope == BOXSCOPE_BANK && s_orig_slot >= 0 && !s_held_dup &&
      src->scope != BOXSCOPE_BANK && bc_is_native(s_held) &&
      !(arm == XG_DOWN_ARM_GEN3 && occupied)) {
    BankDownResult bd = bank_down_dispatch(src, box, cur, s_held,
                                           recs + (uint32_t)cur * 80, conv);
    if (bd == BANK_DOWN_LANDED) {
      /* Two arms LAND. EXACT (S150-7): the GB list grew and the Bank consume ALREADY
       * ran inside bank_down_exact -- repaint from the image. GB_BRIDGE (S150-8): the
       * bridge wrote the MOUNTED session's box `box` (gbs_insert appends to the list on
       * screen) and gb_persist already verified the card, so its Bank consume is
       * IMMEDIATE like EXACT's -- a deferred delete would wait for the Gen-3 exit-save
       * flush that a Game Boy session never runs, and every later save open clears the
       * queue (merged-tree review F2); then repaint (F3). */
      if (arm == XG_DOWN_ARM_EXACT) {
        s_holding = false; *done = true; s_oam_reload = true;
        recs = src->records(box);            /* the GB list grew -- repaint from the image */
        return recs;
      }
      s_holding = false; *done = true;
      { uint8_t slots1[1]; slots1[0] = (uint8_t)s_orig_slot;
        /* s_held IS the 80-byte record: hand it to the consume directly (no 80-B copy
         * here -- the escape-gate structural test treats every 80-B memcpy in
         * drop_held as a write that must sit below the gate). */
        boxoam_suspend();                              /* hard rule 1: SD write with OAM off */
        if (!app_bank_clear_slots(s_orig_box, slots1, (const uint8_t (*)[80])s_held, 1)) {
          snd_error();                                 /* D7: the game HAS it; the Bank keeps a duplicate */
          char l2[40]; siprintf(l2, "Bank box %d slot %d", s_orig_box + 1, s_orig_slot + 1);
          msg_wait(PDNA_XFER_DOWN_DUP_TITLE, UI_WARN, PDNA_XFER_DOWN_DUP_L1, l2);
        }
        boxoam_resume(); }
      s_oam_reload = true; recs = src->records(box);   /* the mounted list grew -- repaint */
      return recs;
    }
    if (bd != BANK_DOWN_CONVERTED) return recs;   /* REFUSED: keep holding; the arm already said why */
    converted = true;                             /* fall through to the BANK -> PC branch, with `conv` */
  }
  /* BACKLOG #150 S150-3 decision 3: a native "GBC1" cell may only ever land back in the
   * Bank -- `src->scope` IS the destination scope on every branch below (`recs` always
   * belongs to `src`), including the homeless fall-through further down that runs the
   * "PC -> Bank" code with the PC as the real destination. Dominates the five memcpy
   * sites below that can write s_held into `recs`: the cross-scope DUPLICATE fast path,
   * the BANK -> PC true-MOVE, the PC -> Bank always-RELEASE, the empty-cell place, and
   * the same-scope SWAP place. The hand is NOT emptied (still holding), same shape as
   * the `if (occupied)` refusal below it.
   * BACKLOG #150 S150-8 decision 12: `!converted &&` -- after BANK_DOWN_CONVERTED
   * `s_held` is still native, so this gate would otherwise refuse a transfer its own
   * arm already approved; every OTHER path (that never went through the dispatch
   * above) still refuses exactly as before -- defence in depth, unweakened. */
  if (!converted && xg_native_escape_denied(s_held, src->scope)) {
    boxoam_suspend();
    snd_deny();
    msg_wait(PDNA_XFER_NATIVE_TITLE, UI_WARN, PDNA_XFER_NATIVE_L1, PDNA_XFER_NATIVE_L2);
    boxoam_resume();
    return recs;
  }
  /* `occupied` -- computed above (review F7), before the dispatch block, so this
   * function's own destination-occupancy rule gates BOTH the DOWN dispatch and the
   * ordinary cross/same-scope drop paths below with the identical definition. A bank
   * slot whose mon is moving out to the PC looks empty but still holds that mon's
   * only on-card copy until the PC is saved — treated as OCCUPIED so nothing
   * overwrites it. bc_is_native (BACKLOG #150 S150-2, G-H2): a native cell
   * g_box[cur].species cannot represent (GB_SHOW_NONE, no DAMAGED stand-in built)
   * must not read as empty either. */
  if (!same_scope(src)) {                                    /* cross-scope drop */
    /* BACKLOG #120 S2 / #150 S150-4 decision 9: no lift path exists for a
     * Game-Boy-scope transfer unless a REAL xfer vtable is installed (not the S2
     * marker-only k_gb_xfer_s2, whose members were all NULL) -- deny before the
     * occupied check and before the DUPLICATE fast-path below, so a GB-scope carry
     * can never land there, dup or not. `have_xfer` is what lets decision 9's one
     * allow-rule (BANK<-GB) through once k_gb_xfer (this lane) is session-wide.
     * Bracketed like the PC->Bank commit below it uses for its own
     * boxoam_suspend()/resume(). */
    /* BACKLOG #150 S150-12 decision 6: release_up is no longer required for
     * `have_xfer` -- the read-only mount's k_gb_xfer_ro table has a real lift_up
     * (the COPY-flavoured one) but no release_up at all, and its drop must still be
     * admitted (as a COPY, never a delete) rather than refused by xg_drop_denied. */
    bool have_xfer = s_xfer_peer && s_xfer_peer->lift_up;
    if (xg_drop_denied(src->scope, s_orig_scope, have_xfer)) {
      boxoam_suspend();
      snd_deny();
      msg_wait(PDNA_XFER_NOGEN_TITLE, UI_WARN, PDNA_XFER_NOGEN_L1, PDNA_XFER_NOGEN_L2);
      boxoam_resume();
      return recs;
    }
    /* BACKLOG #246 (#104 Phase 1): the mirror pair -- a PLAIN Gen-3 Bank cell
     * (never native; a native cell reaching this point for a GB destination
     * would already have returned above, through the bc_is_native-gated DOWN-arm
     * dispatch block) carried out of the Bank and dropped onto a Game Boy grid.
     * `have_xfer` (computed just above, `s_xfer_peer && s_xfer_peer->lift_up`) is
     * exactly what unlocked this pair past xg_drop_denied a few lines up --
     * re-checked here by name (`!bc_is_native(s_held)`) as the edge predicate this
     * branch actually means: "a Bank-origin carry that is a plain Gen-3 record,
     * not the native-cell case the dispatch block above already owns."
     * BEFORE the `occupied` check below, not after (found live: Red's real GB
     * BOX1 is 20/20 on this corpus and the drop silently no-op'd, snd_deny()'d
     * by that check with no dialog at all) -- `occupied` reads the DESTINATION
     * cell under the cursor, which means something for a slot-addressable Bank
     * cell (drop_held_up, just below) but NOTHING for a Game Boy list: gbs_insert
     * always appends at the list's own next free slot (gb_bank_down_g3's own
     * capacity check, further down, is the real gate), exactly why the native-cell
     * dispatch block above this whole `if (!same_scope(src))` branch also runs
     * BEFORE `occupied` for its own EXACT/GB_BRIDGE arms. */
    if (src->scope == BOXSCOPE_GB && s_orig_scope == BOXSCOPE_BANK &&
        s_orig_slot >= 0 && s_xfer_peer && s_xfer_peer->lift_up && !bc_is_native(s_held)) {
      return drop_held_down_g3(src, box, cur, recs, done);
    }
    /* BACKLOG #246 review D1 (HIGH -- a false success): the down-arm above requires
     * s_orig_slot >= 0 (it deletes a card slot on landing -- a real ORIGIN drop), so
     * a Bank DUPLICATE carry (s_orig_slot == -1, s_held_dup == true -- the read-only
     * nav-menu's copy, or a COPY lift) falls PAST it. Before this fix it fell past
     * `occupied` too, into the generic DUPLICATE fast path further down, which
     * memcpy's the Gen-3 record straight into the GB page buffer with no
     * gen3_to_gb_fixed, no loss screen, no capacity check and no sidecar write -- a
     * silent, undeclared copy into a Game Boy save (display-only: gbsrc_commit
     * refuses a native-shaped write and a re-page clears it, but the box header still
     * lies 16/20 -> 17/20 with no dialog at all). Refuse every Bank-origin duplicate
     * headed for a GB destination outright -- named by the exact scope this drop is
     * headed for (src->scope == BOXSCOPE_GB), so a PC-destination duplicate is
     * unaffected and still reaches its own existing fast path below. There is no
     * landing path for a Bank duplicate on a Game Boy save yet (#104's later phases,
     * not #246 Phase 1). tests/host_escape_gate_sites_test.py check (bb) pins this
     * refusal strictly between the down-arm above and `if (occupied)` below. */
    if (src->scope == BOXSCOPE_GB && s_orig_scope == BOXSCOPE_BANK && s_held_dup) {
      boxoam_suspend();
      snd_deny();
      /* BACKLOG #246 review F6(a) fix: this is NOT the "not across generations"
       * rule (PDNA_XFER_NOGEN_*) -- the user has just seen that disproved by a
       * successful non-duplicate move a moment earlier. This is COPY-specific:
       * a duplicate carry has no Bank slot of its own to free on landing. */
      msg_wait(PDNA_XFER_COPYNOXFER_TITLE, UI_WARN, PDNA_XFER_COPYNOXFER_L1, PDNA_XFER_COPYNOXFER_L2);
      boxoam_resume();
      return recs;
    }
    if (occupied) { snd_deny(); return recs; }
    /* BACKLOG #150 S150-4 decisions 1/7/9/10 / BACKLOG #170: the UP drop -- a Game
     * Boy mon carried into an empty Bank cell -- extracted into its own noinline
     * helper (drop_held_up, above) so its held_copy[80] and its own frame are shed
     * from drop_held's worst-case stack path on every OTHER drop (drop_held runs on
     * every grid A-press, not just a carry). See drop_held_up's own doc comment for
     * the ORDER contract (decision 1) this call site does not touch. */
    if (src->scope == BOXSCOPE_BANK && s_orig_scope == BOXSCOPE_GB &&
        s_orig_slot >= 0 && s_xfer_peer && s_xfer_peer->lift_up) {
      return drop_held_up(src, box, cur, recs, done);
    }
    if (s_held_dup && s_orig_slot < 0) {                     /* a fresh DUPLICATE: placing it is loss-proof
                                                                 in either direction -> no confirm needed
                                                                 (also fixes the reversed "Copy to Bank?"
                                                                 text when a bank dup lands in the PC). */
      if (src->note_add) src->note_add(s_held);
      memcpy(recs + (uint32_t)cur * 80, s_held, 80); src->mark_dirty();
      s_holding = false; s_held_dup = false; *done = true; return recs;
    }
    if (src->scope == BOXSCOPE_PC && s_orig_scope == BOXSCOPE_BANK && s_orig_slot >= 0) {  /* BANK -> PC: a true MOVE, no prompt */
      /* place in the PC now (deferred); the bank original is deleted at the save phase,
       * AFTER the PC is written, so it can't be lost (worst case a duplicate). The defer
       * queue holds 64: when FULL, refuse the drop (a silent un-queued move would leave a
       * duplicate behind) — save + re-enter to flush the queue. */
      if (app_bank_defer_full()) { snd_deny(); return recs; }
      /* BACKLOG #150 S150-8 decision 11: `converted` -> place `conv` (the arm's own
       * finished Gen-3 record), never `s_held` (still the native cell) -- see this
       * function's own dispatch-block comment for why this reuses the existing
       * call sites instead of adding new ones. */
      const uint8_t* placing = converted ? conv : s_held;
      if (src->note_add) src->note_add(placing);
      memcpy(recs + (uint32_t)cur * 80, placing, 80); src->mark_dirty();
      app_bank_defer_delete(s_orig_box, s_orig_slot, s_held);
      s_holding = false; *done = true; return recs;
    }
    /* PC -> Bank: an always-RELEASE move (no autoduplication — Guy's decision). Write the
     * mon into the (empty) bank cell, VERIFY the bank box on SD, and only then release the PC
     * source. On a write failure, revert the cell and keep holding so nothing is lost (learn:
     * commit the destination before clearing the source -> worst case a duplicate, never a
     * loss; the PC clear is deferred to the one exit save, backed by the immutable backup).
     *
     * BACKLOG #150 S150-8b, D-Q1 (the RESTORE edge, hop 2 of Guy's 2->3->1->2): if
     * `s_held` (the Gen-3 mon in hand) has a native-home transfer-ledger entry --
     * S150-8's DOWN edge wrote one when this exact mon left the Bank as a converted
     * Gen-3 record -- this Gen-3 mon going BACK into the Bank restores that ORIGINAL
     * native cell instead of landing as another Gen-3 cell. rc==-1 refuses the WHOLE
     * drop: nothing written anywhere, the hand keeps holding (an unreadable record or
     * a serial refusal). rc==0 is the ordinary, unchanged path: an ordinary Gen-3 mon,
     * or one whose ledger entry is Gen-3-home, lands exactly as it did before this
     * lane, byte for byte. */
    uint8_t cell80[80];
    /* review D9: defensive -- this "PC -> Bank" comment block is reached by the
     * homeless-carry fall-through too (s_orig_box == -1, decision 2's own footgun
     * note), which is NOT guaranteed to have src->scope == BOXSCOPE_BANK. Never call
     * the restore lookup, and never let a native cell land, on any destination that
     * is not actually the Bank. */
    int rc = (src->scope == BOXSCOPE_BANK) ? pc_bank_restore_up(s_held, cell80) : 0;
    /* review D3: rc == -2 is a plain user decline on the F3 confirm -- app_confirm
     * already drew its own "B = no", so nothing further is shown; still holding,
     * nothing written, same as every other refusal here. */
    if (rc == -2) return recs;
    if (rc < 0) {                                                      /* still holding, nothing written */
      snd_error();
      /* review F5: decision 9's own message -- an unreadable ledger record or a
       * serial refusal, not silence. review D5: box sprites off around the panel,
       * same as the escape-gate refusal above and the UP arm's own backup-gate
       * message. */
      boxoam_suspend();
      msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
      boxoam_resume();
      return recs;
    }
    if (rc == 1 && !pdna_bank_prepare_native()) {
      snd_error();
      boxoam_suspend();
      msg_wait(PDNA_XFER_PREP_TITLE, UI_WARN, PDNA_XFER_PREP_L1, PDNA_XFER_PREP_L2);
      boxoam_resume();
      return recs;                                                     /* still holding */
    }
    memcpy(recs + (uint32_t)cur * 80, rc == 1 ? cell80 : s_held, 80);
    boxoam_suspend();
    bool ok = src->commit();                                 /* verified bank box_save (banksrc_commit) */
    boxoam_resume();
    if (!ok) { memset(recs + (uint32_t)cur * 80, 0, 80); snd_error(); return recs; }   /* keep holding */
    if (s_orig_slot >= 0) app_pc_release_slot(s_orig_box, s_orig_slot, s_held);
    if (rc == 1) pc_bank_restore_done(s_held);      /* entry marked LAST, after the Bank write landed */
    snd_save();
    s_holding = false; *done = true; return recs;
  }
  /* BACKLOG #187/#191a, F2: a within-GB move, via BoxXferOps.move_within -- the
   * deny-beep stub decision 8(c) left here is now wired. Dropping the mon back on
   * its OWN cell is handled by the top-of-function check above and never reaches
   * here; this covers every OTHER cell of a same-scope GB drop.
   *   occupied cell (any box): a GB box cannot swap two mons at once (no addressable
   *     slots, just a packed list) -- refuse with a dialog that says why, not a beep.
   *   empty cell, SAME box: the mon is already in this box's own list; nothing to do
   *     -- put back silently (no beep), matching the "put back on own cell" idiom
   *     just above.
   *   empty cell, ANOTHER box: gbs_move() via move_within -- lands at the
   *     destination's own next free slot (a count-prefixed list has no other
   *     addressing), reusing gb_move_hook's exact refusal table/persist/reload so
   *     the display mount matches the save afterwards. */
  if (src->scope == BOXSCOPE_GB) {
    if (occupied) {
      boxoam_suspend();
      snd_deny();
      msg_wait(PDNA_XFER_GBSWAP_TITLE, UI_WARN, PDNA_XFER_GBSWAP_L1, PDNA_XFER_GBSWAP_L2);
      boxoam_resume();
      return recs;
    }
    if (box == s_orig_box) {
      s_holding = false; *done = true; return recs;         /* already in this list */
    }
    /* src->xfer, NOT s_xfer_peer -- #171b's own rule (see k_gb_xfer's comment in
     * pdna_gen12.c): a SAME-scope carry (this is one -- src->scope == s_orig_scope
     * got it here) reads the vtable off `src` itself, exactly like start_carry()
     * does; s_xfer_peer is the OTHER scope's table, only meaningful for a
     * cross-scope drop (the branch above this whole same-scope block). */
    if (!(src->xfer && src->xfer->move_within)) { snd_deny(); return recs; }
    boxoam_suspend();
    bool ok = src->xfer->move_within(s_orig_box, s_orig_slot, box);
    boxoam_resume();
    if (!ok) return recs;             /* move_within's own hook already reported why */
    s_holding = false; *done = true; s_oam_reload = true;
    recs = src->records(box);         /* repaint from the just-committed image */
    return recs;
  }
  if (!occupied) {                                           /* empty -> place, clear origin */
    if (src->note_add) src->note_add(s_held);
    memcpy(recs + (uint32_t)cur * 80, s_held, 80); src->mark_dirty();
    recs = clear_origin(src, box);
    s_holding = false; *done = true; return recs;
  }
  /* occupied within scope -> SWAP, then KEEP HOLDING the displaced occupant (place it
   * yourself next; we don't auto-throw it into the held mon's old cell). */
  if (s_orig_slot < 0 && s_held_dup) { snd_deny(); return recs; }          /* a fresh dup can't swap */
  /* BACKLOG #150 S150-3 review F2: the escape gate above reads the HELD record; the SWAP
   * below also WRITES the destination. A native cell may be DISPLACED by another
   * native cell, never overwritten by a Gen-3 record -- box_save's invariant would
   * then refuse every later save of this box with no UI at all. */
  if (bc_is_native(recs + (uint32_t)cur * 80) && !bc_is_native(s_held)) { snd_deny(); return recs; }
  if (s_orig_slot >= 0 && s_orig_box != box && src->scope == BOXSCOPE_BANK) { snd_deny(); return recs; }  /* the REAL Bank's cross-box swap is unsafe (S1 review D1 companion); GB swaps arrive with S3's move_within */
  if (src->note_add) src->note_add(s_held);                                /* placed mon enters this scope -> dex */
  uint8_t occ[80]; memcpy(occ, recs + (uint32_t)cur * 80, 80);             /* save the occupant */
  memcpy(recs + (uint32_t)cur * 80, s_held, 80);                           /* place the held mon at the cursor */
  src->mark_dirty();
  recs = clear_origin(src, box);                                           /* free the held mon's old cell */
  memcpy(s_held, occ, 80);                                                  /* now carry the displaced occupant */
  s_orig_slot = -1; s_held_dup = false;            /* RAM-only real mon: place it; never crosses save scopes */
  return recs;                                      /* *done stays false: still holding */
}

/* Push the full sprite state for the current frame: icons (reloaded if needed),
 * cursor / carry, and ITEM markers + carried item. */
/* Mid-grab-dip: s_holding is already true (start_carry ran), but visually the mon has
 * not left its cell yet — the open hand is still descending onto it. oam_sync draws the
 * reaching cursor instead of the carry while this is set, and does NOT lift-hide the
 * origin slot, which is exactly retail's ordering (MonPlaceChange_Grab: the icon stays
 * put until the fist closes). */
static int s_grab_dip = 0;

/* boxoam_cursor's title_row (box_oam.h, 2026-08-23 glove-vs-tab-row fix): fold this
 * file's own on_title/s_tab_focus pair into the single value box_oam.c needs to
 * re-anchor the glove correctly. s_tab_focus wins over on_title when both are set —
 * true simultaneously the whole time the cursor is up on the top tabs, see
 * pdna_box()'s own KEY_UP handling on the title row, which sets s_tab_focus without
 * ever clearing on_title (by design: DOWN/B from the tabs must return to the SAME box-
 * name-row state, not the grid). 0/1 for the "not tab-focused" cases match the old
 * bool exactly (false/true), so this is the only call site that needs to know about
 * s_tab_focus at all. */
static int cursor_title_row(bool on_title) {
  if (s_tab_focus >= 0) return 2 + s_tab_focus;   /* 2/3/4 = PKMN DATA/PARTY/SAVE */
  return on_title ? 1 : 0;
}

/* Compute the label's centre X for a tab (0=PKMN DATA, 1=PARTY, 2=SAVE). draw_tab
 * CENTRES its label in the tab rect (tw = strlen(label)*8, tx = x + (w - tw)/2), so a
 * centred label's centre is the rect midpoint -- independent of the label's length,
 * which is what makes this correct for the Bank's runtime "(BANK)" label too. The
 * strlen dance below still mirrors draw_tab's formula so the two can only drift
 * together, and the result is tx + tw/2 (the CENTRE -- the first cut returned tx, the
 * label's left edge, which parked the glove 16-36 px left of every label and hung it
 * off-screen on PKMN DATA). */
static int tab_label_cx(int tab) {
  const char* labels[3] = { "PKMN DATA", "PARTY", "SAVE" };
  const int tabs_x[3] = { 0, PANEL_W + 1, PANEL_W + 93 };
  const int tabs_w[3] = { PANEL_W + 1, 92, 240 - (PANEL_W + 93) };
  int tw = (int)strlen(labels[tab]) * 8;
  int tx = tabs_x[tab] + (tabs_w[tab] - tw) / 2;
  if (tx < tabs_x[tab] + 2) tx = tabs_x[tab] + 2;
  return tx + tw / 2;
}

/* Compute the label centre X for boxoam_cursor() based on title_row:
 * 0 (grid): unused; 1 (banner): rect midpoint; 2/3/4 (tabs): tab label centre. */
static int cursor_label_cx(int title_row) {
  if (title_row == 1) return WP_X + WP_W / 2;
  if (title_row >= 2) return tab_label_cx(title_row - 2);
  return 0;    /* unused for grid (title_row == 0) */
}

static void oam_sync(int cur, bool on_title, int box, bool is_bank) {
  if (s_oam_reload) {
    /* The 30-slot icon upload -- the whole cost of an L/R box page flip, and the
     * single most expensive thing this screen does. Rolled up (it fires on every box
     * change and after every operation that dirties the grid), emitted when the screen
     * is left. */
    perf_rep_begin(PERF_REP_PAGE, "box.load");
    boxoam_load_box(g_box);
    perf_rep_end(PERF_REP_PAGE);
    s_oam_reload = false; era_hides_apply();
  }
  if (s_holding && s_grab_dip) {
    boxoam_carry_end();
    /* the menu-wipe repaint ran ONE oam_sync in the carry look, which lift-hid the
     * origin cell — undo that: retail keeps the mon in place until the fist closes.
     * NOT for a cell wearing era art: its icon is hidden on purpose and un-hiding it
     * would stack a Gen-3 icon on the Gen-1 sprite for the length of the dip. */
    if (s_orig_slot >= 0 && s_orig_slot < G3_BOX_SLOTS &&
        (s_orig_scope == BOXSCOPE_BANK) == is_bank && s_orig_box == box &&
        !(s_era_drawn & (1u << s_orig_slot)))
      boxoam_show_slot(s_orig_slot);
    boxoam_item_markers(g_box, false);
    boxoam_carry_item(cur, 0, false);
    int tr = cursor_title_row(on_title);
    boxoam_cursor(cur, tr, cursor_look(), cursor_label_cx(tr));   /* the descending open hand */
  } else if (s_holding) {
    int tr = cursor_title_row(on_title);
    /* BACKLOG #150 S150-13 / #164: a carried NATIVE Bank cell is not Gen-3-shaped, so
     * pk_decode_mon() cannot resolve a real species for it (undefined-in-practice on
     * non-Gen-3 bytes -- "fixing" pk_decode_mon to tolerate native bytes is out of
     * scope, it would hide the real bug class #164 is about). badge-only (decision a
     * of the brief): species 0 makes boxoam_carry_held() fall into its own
     * hide(OE_CARRY) arm (the fist rides empty), and the glove shows the era badge
     * instead -- cheaper and safer than laying the badge atop pk_decode_mon()'s
     * undefined output. */
    if (bc_is_native(s_held)) {
      boxoam_carry_held(cur, tr, cursor_label_cx(tr), 0, 0, false);
      uint8_t gen = bc_kind(s_held);
      boxoam_carry_badge(cur, tr, cursor_label_cx(tr), pdna_origin_native_mark(gen),
                          pdna_origin_native_color(gen));
    } else {
      PkMon hm; pk_decode_mon(s_held, false, &hm);
      boxoam_carry_held(cur, tr, cursor_label_cx(tr), hm.species, hm.form, hm.isEgg && !hm.isBadEgg);   /* held mon (or Egg) front-most + orange fist */
      boxoam_carry_badge(cur, tr, cursor_label_cx(tr), 0, 0);   /* no badge on an ordinary Gen-3 carry */
    }
    if (s_orig_slot >= 0 && (s_orig_scope == BOXSCOPE_BANK) == is_bank && s_orig_box == box)
      boxoam_hide_slot(s_orig_slot);                         /* lift-hide the origin cell */
    boxoam_item_markers(g_box, false);
    boxoam_carry_item(cur, 0, false);
  } else if (s_cur_mode == CM_ITEM && s_item_held) {
    boxoam_carry_end();                                      /* ITEM GRAB: carried item rides the cursor */
    boxoam_item_markers(g_box, true);
    int tr = cursor_title_row(on_title);
    boxoam_cursor(cur, tr, BOXOAM_HAND_ITEM, cursor_label_cx(tr));  /* the HAND follows too — it used to stay
                                                              * frozen on the source mon (only the item
                                                              * sprite moved with the cursor) */
    /* Small item (bottom-centre), NOT the full 32x32 that covered the mon — so the source
     * mon you just took from is visibly FADED (it no longer holds an item), instead of
     * looking opaque behind the carried item (Guy). The footer says you're carrying it. */
    boxoam_carry_item(cur, (uint16_t)s_item_held, false);
  } else {
    boxoam_carry_end();
    boxoam_item_markers(g_box, s_cur_mode == CM_ITEM);
    if (s_cur_mode == CM_ITEM && g_box[cur].heldItem)
      boxoam_carry_item(cur, g_box[cur].heldItem, false);    /* HOVER: small item bottom-left */
    else
      boxoam_carry_item(cur, 0, false);
    int tr = cursor_title_row(on_title);
    boxoam_cursor(cur, tr, cursor_look(), cursor_label_cx(tr));  /* cursor hand last; restores region A */
  }
}

static void draw_footer(bool is_bank, bool on_title, bool moving) {
  const char* f;
  if (s_tab_focus >= 0)    f = tab_focus_footer(s_holding, pdna_box_carry_is_gb());
  else if (moving)         f = "A drop  B cancel";
  else if (s_item_held)    f = "A give  B put back";
  /* A now renames directly and SELECT opens the box menu (Guy, BACKLOG #33) -- "L/R
   * box  A name  SEL menu  DN" (27 chars) does not fit the footer's real budget (the
   * fill/text only spans WP_W=162px at 8px/glyph => 20 columns, not the wider figure
   * this line's own history assumed), so "box" and "DN" are dropped: L/R's box-switch
   * meaning is this row's original, most-used function and is kept; DN (title -> grid)
   * is a bare DOWN-press a player finds by exploring, same as the grid footer never
   * spelling out that L/R does nothing there. */
  else if (on_title)       f = "L/R A name SEL menu";
  else if (s_cur_mode == CM_MOVE) f = "MOVE A grab hold=set";
  else if (s_cur_mode == CM_ITEM) f = "ITEM  A take  SEL B";
  else                     f = is_bank ? "A menu  SEL  L/R  B"
                                       : "A menu  UP  SEL  B";
  /* clear the footer strip first (it changes between modes) */
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), f);
}

/* Set a box mon's held item (decrypt -> set -> re-encode + checksum, in place).
 * Returns false, writing nothing, on a native cell.
 * BACKLOG #150 S150-3 review F1: a native GBC1 cell is not a Gen-3 record -- the
 * grid's ITEM mode is a fourteenth escape route the brief's twelve missed. Every
 * native cell decodes as g_box[cur].species != 0 (S150-2's G-H2 stand-in) and
 * heldItem == 0, so GIVE/swap, TAKE and the B put-back's own g_box[]-only tests
 * never exclude it on their own -- this is the single choke point all three
 * callers now gate on. */
static bool box_set_held(uint8_t* recs, int slot, uint16_t item) {
  uint8_t* rec = recs + (uint32_t)slot * 80;
  if (bc_is_native(rec)) { snd_deny(); return false; }
  EditMon e; gen3_edit_load(rec, false, &e);
  em_set_item(&e, item);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, 80);
  return true;
}

/* A safe home slot to deposit the carried item: its source slot if still empty-
 * handed, else the first occupied mon with no item; -1 if nowhere (never lose it).
 * BACKLOG #150 S150-3 review F1: `recs` lets this skip bc_is_native() slots -- a
 * native cell always decodes heldItem == 0 (it has no Gen-3 item field at all), so
 * without this it reads as a perfectly "safe" home for a real item, and
 * box_set_held() would then have to refuse the put-back and strand the item on
 * the cursor mid-carry. */
static int item_home(const uint8_t* recs) {
  if (s_item_from >= 0 && g_box[s_item_from].species && !g_box[s_item_from].heldItem &&
      !bc_is_native(recs + (uint32_t)s_item_from * 80)) return s_item_from;
  for (int s = 0; s < 30; s++)
    if (g_box[s].species && !g_box[s].heldItem && !bc_is_native(recs + (uint32_t)s * 80)) return s;
  return -1;
}

/* Art-free build: the 30 icon sprites don't exist, so an occupied cell would be an
 * empty patch of wallpaper. Draw an ORIGINAL name chip per occupied cell instead
 * (our font, our colours) so the PC stays a usable grid; the icon art or a
 * registered ROM upgrades it back to real sprites. Painted with the wallpaper, so
 * every full-region repaint carries the labels for free. */
static void artless_cells(void) {
  if (boxoam_icons_available()) return;   /* compiled art OR the user's registered ROM */
  /* PARITY-AUDIT-2026-09 #75: this session-wide gate only rules out the Gen-3 icon
   * SOURCES (compiled art / the SD icon cache / a registered Gen-3 ROM) -- it knows
   * nothing about a Game Boy session's own art (gb_art_source.h's PdnaGbArtSource
   * vtable), which era_cells() (called BEFORE this function now, see render_full/
   * move_cursor/chunk_draw) already painted straight to the BG bitmap for every cell
   * it could actually decode a picture for, tracked one bit per grid slot in
   * s_era_drawn. Skip the chip PER CELL for exactly those slots -- a cell whose GB
   * fetch failed (stack gate refusal, an undecodable species) leaves its s_era_drawn
   * bit clear and still gets the chip below, same as before this fix. */
  for (int i = 0; i < 30; i++) {
    if (!g_box[i].species) continue;
    if (s_era_drawn & (1u << i)) continue;  /* era_cells() already drew a real picture here */
    int cx = GRID_X + (i % COLS) * CELL_W, cy = GRID_Y + (i / COLS) * CELL_H;
    if (g_box[i].isEgg && !g_box[i].isBadEgg)
      ui_name_chip(cx, cy + 5, CELL_W - 2, 12, 0x2A7A, 0x0000, "EGG");
    else
      ui_name_chip(cx, cy + 5, CELL_W - 2, 12, UI_PANEL, UI_TEXT,
                   pk_species_name(g_box[i].species));
  }
}

/* BACKLOG #200: how many of the 30 drawn cells this box's SOURCE actually has.
 * NULL src->capacity (every Gen-3 PC/Bank source) means the grid's own 30 --
 * unchanged, same fallback draw_box_banner's own occupancy denominator already
 * uses (:2084 below). The one place every OTHER capacity-aware call in this file
 * (F1's blocked_cells, F2's cursor clamps in pdna_box() itself) goes through,
 * instead of each repeating the ternary inline.
 *
 * `noipa`: a stack_budget.py walker requirement (BACKLOG #200 review). A plain
 * static helper here gets IPA-SRA-cloned by -O2 into a `.isra.0`/specialized
 * form whose `bl src->capacity(box)` site loses the plain `ldr rN,[rY,#64]`-
 * right-before-`bl` shape the walker's struct-field classifier looks for --
 * the call then falls through to the uncounted-argsites bucket and silently
 * pushes count-only callers over the gate's declared budget. `noipa` forces a
 * real, unspecialized function so the field load stays local and resolvable,
 * matching every other src->capacity(box) site's already-working shape (the
 * global `BoxSource.capacity @64 -> gbsrc_capacity` entry in
 * tools/stack_edges.txt, unchanged by this lane). Calling this helper is
 * itself an ordinary DIRECT call (not a struct-field dispatch) from every
 * site that uses it, including inside pdna_box() -- it adds no new indirect
 * call site there at all, so pdna_box()'s own already-declared argsites count
 * is untouched. */
static int __attribute__((noipa)) box_cap(BoxSource* src, int box) {
  return src->capacity ? src->capacity(box) : COLS * ROWS;
}

/* BACKLOG #200 F2: one LEFT/RIGHT press within the current row, re-applying the
 * grid's own existing wrap rule until it lands on a real slot. Blocked cells are
 * a strict INDEX-INCREASING tail (index >= cap): moving further RIGHT from a
 * valid cell only ever walks INTO higher, still-blocked indices until the
 * COLS-1 wrap fires and drops back to this row's own column 0 -- which is
 * always real, because the caller never starts a press already sitting on a
 * blocked cell (F2's whole point) and a row only holds the cursor at all once
 * DOWN has already refused to enter a row with no real cells in it (see the
 * DOWN sites below). Symmetric argument for LEFT's col-0 wrap. Bounded by COLS
 * (golden rule 2): at most one full lap of the row before landing on column 0. */
static int grid_lr_step(int cur, int cap, bool right) {
  for (int i = 0; i < COLS; i++) {
    cur = right ? ((cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1)
                : ((cur % COLS == 0) ? cur + COLS - 1 : cur - 1);
    if (cur < cap) break;
  }
  return cur;
}

/* F1: cells at/after the source's own capacity don't exist in this game (a Game
 * Boy box holds 20, its party 6; the grid always draws 30). box_oam.c's OBJ tile
 * budget is spent in full already -- 30 icons x 16 tiles + the hand + region B is
 * exactly the 512 tiles bitmap-mode OBJ VRAM has (that file's boxoam_set_frame
 * header) -- so there is no spare tile for a new BLOCKED graphic; this paints straight
 * onto the BG bitmap, the same layer artless_cells()/era_cells() paint into, so it
 * rides every full repaint of the wallpaper for free.
 * BACKLOG #262 (Guy: "ugly X's ... something that blends in"): the old hatch + X is
 * gone. A dead cell is now a RECESSED TILE -- the user's own wallpaper darkened in
 * place (top row and left column harder, like a bevel, the rest softer) so the cell
 * grid stays visible but reads as "not there". No glyph and no ROM art, so it is
 * identical in the artless build and on every game; the slot still refuses a drop
 * (that is box_cap's job, not this paint). Drawn for every blocked cell regardless
 * of the artless/real-art build -- a cell with no species never gets an OBJ icon
 * either way, so there is nothing for this to hide behind. */
#define BLOCKED_EDGE_PCT 45   /* top row / left column of a dead cell, % of the wallpaper */
#define BLOCKED_FILL_PCT 70   /* the rest of it */

static u16 blocked_shade(u16 c, int pct) {
  /* exact on 0..31, no divide (the GBA has none): v*45/100 == (v*231)>>9, v*70/100 == (v*45)>>6 */
  _Static_assert(BLOCKED_EDGE_PCT == 45 && BLOCKED_FILL_PCT == 70, "re-derive the multipliers");
  int m  = (pct == BLOCKED_EDGE_PCT) ? 231 : 45;
  int sh = (pct == BLOCKED_EDGE_PCT) ? 9 : 6;
  int r = ((c & 31) * m) >> sh;
  int g = (((c >> 5) & 31) * m) >> sh;
  int b = (((c >> 10) & 31) * m) >> sh;
  return (u16)(r | (g << 5) | (b << 10));
}

static void blocked_cells(BoxSource* src, int box) {
  int cap = box_cap(src, box);
  if (cap >= COLS * ROWS) return;                  /* Gen-3 PC/Bank: every cell real */
  if (cap < 0) cap = 0;
  for (int i = cap; i < COLS * ROWS; i++) {        /* <= COLS*ROWS iterations */
    int cx = GRID_X + (i % COLS) * CELL_W, cy = GRID_Y + (i / COLS) * CELL_H;
    for (int r = 0; r < CELL_H; r++)
      for (int c = 0; c < CELL_W; c++) {
        u16* px = &vid_mem[(cy + r) * 240 + cx + c];
        *px = blocked_shade(*px, (r == 0 || c == 0) ? BLOCKED_EDGE_PCT : BLOCKED_FILL_PCT);
      }
  }
}

/* ---- THE BANK IN PARALLEL --------------------------------------------------------
 *
 * Guy: "if a pokemon is from gen 1, use a gen 1 sprite, if its from gen 2, use its gen 2
 * sprite ... THE BANK SHOULD SHOW ALL IN PARALLEL." The bank is 16 SD-backed boxes where
 * imports and natives sit side by side, so this grid is the screen the request is about;
 * it is drawn by the SAME renderer as the PC (one BoxSource, one loop), and the era view
 * is simply part of what a cell looks like — there is no second grid anywhere.
 *
 * E5b (Guy, 2026-09-06) MADE LAYER 1 OPT-IN. The paragraph above describes the
 * original ask, which layer 1 used to satisfy unconditionally (every GB import always
 * drew its own generation's picture, no Settings touched); Guy then decided that was
 * too much by default -- a Gen-3 save's PC and BANK grids now draw the icon-store's
 * OWN picture for every mon, imports included, unless the (kind, place) cell in
 * Settings > Game ROM > Sprites is explicitly set away from NATIVE (sprite_era.h's
 * se_resolve_cell has the exact rule, including why GEN1 keeps working at BANK but is
 * a no-op at PC). Layer 2 (the mark) is UNCHANGED and is what still makes "the bank
 * shows every mon's provenance in parallel" true by default -- only the PICTURE
 * became a choice.
 *
 * Two layers, independent of each other since E5b (rationale + measured cost in
 * pdna_origin_art.h §3):
 *
 *   1. THE PICTURE, OPT-IN. A cell whose Settings era resolves concretely to GEN1 or
 *      GEN2 (an explicit override -- or, at PC/BANK, never a plain import's own
 *      untouched NATIVE cell any more) is drawn in that era's own sprite/icon, scaled
 *      into the cell and blitted to the BG bitmap, with its Gen-3 OBJ icon hidden so
 *      the two cannot stack. Every other cell -- natives AND untouched imports alike
 *      -- keeps its OBJ icon.
 *   2. THE LABEL, ALWAYS ON PROVENANCE. Every GB-import cell gets a small era pad
 *      ('1'/'2'/'?') at its top-left, REGARDLESS of whether layer 1 drew anything for
 *      it. This layer is free (one cached byte per cell) so it is ALWAYS drawn — no
 *      GB ROM, the artless build, a species the ROM would not serve, layer 1 declining
 *      to draw a picture at all. It is also the layer that makes the answer
 *      unambiguous: a Gen-1 and a Gen-2 sprite of the same species can look nearly
 *      identical, so the picture alone was never a complete answer -- and now that the
 *      picture is opt-in, the label is the ONLY thing a default-cell import wears.
 *
 * DEGRADES IN LAYERS, and the artless build is untouched: a save with no GB imports has
 * no marked cells at all, so nothing here draws a single pixel — pdna_origin_box_mark()
 * is a cache lookup and returns 0 for every cell.
 */

/* The era pad: a 1 px keyline, the era colour, the era glyph. BOTTOM-RIGHT of the cell.
 *
 * It used to sit top-LEFT, justified as "a 32x32 box icon on a 24x22 pitch is reliably
 * transparent there (the mon is centred)". That reasoning was wrong about WHOSE icon is
 * in the way. box_oam.c places every icon AT its own cell's top-left, so a 32x32 icon on
 * a 24x22 pitch OVERHANGS its cell by 8 px right and 10 px down — and the pad is a BG
 * bitmap draw, permanently under the OBJ layer. The cell ABOVE therefore covers
 * cy..cy+10, i.e. the whole pad; 10 of 24 markers were partly hidden.
 *
 * Working the coverage out per neighbour, for cell (r,c) at (cx,cy):
 *     its own icon   x cx..cx+32   y cy..cy+32
 *     cell (r-1,c)   x cx..cx+32   y cy-22..cy+10   <- the one that broke it
 *     cell (r,c-1)   x cx-24..cx+8 y cy..cy+32
 * so the only part of the cell no NEIGHBOUR can reach is x >= cx+16, y >= cy+11 — the
 * bottom-right corner, where just this cell's own icon overlaps, and that is its
 * lower-right quadrant, which a centred mon leaves transparent. */
static void era_cell_mark(int slot) {
  char m = pdna_origin_box_mark(slot);
  if (!m) return;
  int cx = GRID_X + (slot % COLS) * CELL_W + (CELL_W - 8);
  int cy = GRID_Y + (slot / COLS) * CELL_H + (CELL_H - 9);
  char s[2] = { m, 0 };
  ui_fill_rect(cx,     cy,     8, 9, RGB15(0, 0, 0));
  ui_fill_rect(cx + 1, cy + 1, 6, 7, pdna_origin_box_color(slot));
  ui_ptext(cx + 2, cy + 1, UI_TEXT, s);     /* the 5x7 face: rows cy+1..cy+8 */
}

/* Give a cell back to the OBJ layer (its bitmap art is about to be painted over and we
 * are not paying an SD decode to redraw it right now). */
static void era_cell_icon_back(int slot) {
  if (slot < 0 || slot >= G3_BOX_SLOTS || !(s_era_drawn & (1u << slot))) return;
  s_era_drawn &= ~(1u << slot);
  boxoam_show_slot(slot);
}

/* The scale+blit half of one cell, split out (E3 review BLOCKING 1) so its 1,056-B
 * `cell[]` is NEVER on the stack at the same time as pdna_origin_box_art()'s fetch
 * chain (gb_art_fetch's own frame, ~3.5 KB, plus its callees) -- era_cell_draw()
 * below calls this ONLY AFTER that fetch has already returned and popped, so the two
 * frames never coexist. noinline: an inlined copy would put `cell[]` right back into
 * era_cell_draw's own frame, silently undoing the split. */
/* BACKLOG #264: the page colour a Game Boy picture is drawn over. A Gen-1/2 picture is four
 * shades and the LIGHTEST (index 0) is white -- and in the games it is white, not "nothing":
 * the picture is written into the BACKGROUND tilemap (pokered engine/pokemon/status_screen.asm:
 * 169-170, LoadFlippedFrontSpriteByMonIndex at hlcoord 1,0), so shade 0 is drawn opaque with
 * BGP = %11100100 (home/palettes.asm:22, colour 0 = the lightest) on a white page. Yellow's own
 * palettes agree for every species: colour 0 of each PAL_*MON row is white in both its SGB set
 * (pokeyellow data/sgb/sgb_palettes.asm:20.., RGB 31,31,30) and its CGB set (:65.., RGB
 * 31,31,31). Gen 2 is the same fact by construction: pokecrystal's PokemonPalettes hold only
 * the MIDDLE two colours of a species ("not black or white", data/pokemon/palettes.asm:4-6),
 * white being the engine's PALRGB_WHITE = $7FFF (constants/gfx_constants.asm:3) -- so the
 * per-species palettes change colours 1 and 2 and never the page. Our decode keeps index 0
 * transparent (so one blitter can serve three generations), which over the wallpaper read as
 * holes in every white belly and cheek; this puts the page back, one cell at a time. */
#define ERA_PAGE_WHITE RGB15(31, 31, 31)

static void __attribute__((noinline))
era_cell_blit(int slot, const PdnaArt* a, int cx, int cy) {
  u16 cell[CELL_W * CELL_H];             /* 1056 B of STACK. Never a static, never
                                          * EWRAM (hard rule 2) — and never 30 of them. */
  if (pdna_origin_cell_render(a, cell, CELL_W, CELL_H)) {
    ui_fill_rect(cx, cy, CELL_W, CELL_H, ERA_PAGE_WHITE);   /* #264: only behind a cell that HAS a picture */
    ui_sprite(cx, cy, CELL_W, CELL_H, cell);
    s_era_drawn |= 1u << slot;
    boxoam_hide_slot(slot);              /* or the Gen-3 icon sits ON the Gen-1 sprite */
  }
}

/* One cell, both layers. Called only from a full repaint — the picture costs a GB pic
 * decode off the card, so it must never sit on a per-frame path.
 *
 * E5b: pdna_origin_box_gb() answers the ART half only (does this cell want a bitmap
 * era picture) -- a plain GB import with no Settings override no longer implies that
 * (Guy's opt-in decision), so it can no longer gate the PROVENANCE mark too. The mark
 * is checked independently here (pdna_origin_box_mark(), a second cache-only lookup,
 * equally free) so a Crystal import sitting at its default cell still wears its '2'
 * even though it draws no picture over the Gen-3 OBJ icon. */
/* A native cell whose display ladder bottomed out at GB_SHOW_NONE (bank_damaged_standin,
 * above): species 252 + the plaintext isBadEgg bit, produced by NOTHING else in this
 * tree. A damaged cell wears no '1'/'2' era pad (species 252 fails
 * species_could_be_gb) -- the DMG chip below IS its only marker. */
static bool cell_damaged(int s) { return g_box[s].species == 252 && g_box[s].isBadEgg; }

static void era_cell_draw(int slot) {
  int cx = GRID_X + (slot % COLS) * CELL_W, cy = GRID_Y + (slot / COLS) * CELL_H;
  if (cell_damaged(slot)) {
    /* CELL_W - 2 = 22 px wide with 18 px of label room (ui.c's chip_label(t, name,
     * w - 4)) -- "DAMAGED" would be cut mid-word, so the chip says DMG (3 glyphs,
     * exactly like the shipped EGG chip above) and the full word rides in the
     * stand-in's NICKNAME, which the left data panel shows when the cursor is on
     * the cell. Same bookkeeping era_cell_blit does (s_era_drawn + boxoam_hide_slot)
     * so artless_cells() skips this cell and a later repaint hands the OBJ icon
     * back correctly (era_cell_icon_back / era_hides_apply). */
    ui_name_chip(cx, cy + 5, CELL_W - 2, 12, UI_WARN, 0x0000, "DMG");
    s_era_drawn |= 1u << slot;
    boxoam_hide_slot(slot);
    return;
  }
  if (!pdna_origin_box_gb(slot) && !pdna_origin_box_mark(slot)) return; /* nothing at all */

  /* Layer 1. art_wanted() is a cache lookup plus the source's have() probe — no decode,
   * no card access — so a box with no registered era ROM stops here for free. */
  if (pdna_origin_box_art_wanted(slot)) {
    PdnaArt a;
    PERF_ICON(mru_miss);   /* BACKLOG #263: one art FETCH attempt (the rep line's `icons H/M mru`) */
    /* gen == PDNA_GEN3 means the router fell back (the ROM could not serve this
     * species): leave the ordinary Gen-3 OBJ icon alone rather than blitting the same
     * picture twice, once badly. era_cell_blit's own frame (cell[]) is allocated only
     * for THIS call, after pdna_origin_box_art's fetch chain has already unwound. */
    if (pdna_origin_box_art(slot, &g_box[slot], &a) && a.px && a.gen != PDNA_GEN3)
      era_cell_blit(slot, &a, cx, cy);
  }
  era_cell_mark(slot);                   /* layer 2 goes on top of layer 1 */
}

/* Every cell's era, in one pass, painted with the wallpaper so each full repaint carries
 * it for free. */

/* A DAMAGED cell needs no registered era ROM and trips no pdna_origin_box_any_gb()
 * evidence (species 252 is not a GB species at all) -- so era_cells()'s bail below
 * needs its OWN 30-cell scan, or a box whose only native cell is damaged would never
 * reach era_cell_draw() and the DMG chip would never paint. */
static bool any_damaged(void) {
  for (int i = 0; i < G3_BOX_SLOTS; i++) if (cell_damaged(i)) return true;
  return false;
}

static void era_cells(void) {
  /* THE BOX WITH NO IMPORTS DOES NOTHING AT ALL. Nothing to un-draw and nothing to
   * draw is the case for every box of a save with no Game Boy imports -- so say it
   * once instead of thirty times.
   *
   * HONEST MEASUREMENT: this guard bought nothing under mGBA (art-free box flip stayed
   * at a 13-frame median), so the per-cell pass was never the expensive part -- the
   * header's "free" claim about it holds up. It stays because it cannot change a pixel
   * and it does take 60 calls plus the cell geometry off every full repaint, which is
   * worth more on an Omega running from PSRAM than it is on a PC. The box-screen cost
   * that DID move this session is in box_decode, not here: see pdna_origin_box_note.
   *
   * s_era_drawn must be in the condition: if a previous box left art on screen it
   * still has to be handed back even when THIS box has no imports. any_damaged()
   * (BACKLOG #150 S150-2) covers the DMG-only case above. */
  if (s_era_drawn || pdna_origin_box_any_gb() || any_damaged()) {
    /* BACKLOG #263: the era pass's own rollup -- `perf box.era xN: ... sd Nr/Ns, icons
     * H/M mru` is emitted when the box is left (boxoam_exit flushes PERF_REP_MON). N =
     * passes, sd = the card cost of ALL of them (delta: the fused-ROM read count, see
     * gb_art_source.c), M = fetches attempted. PERF_REP_MON, not PAGE: the box owns PAGE
     * for "box.load"; a screen that opens FROM the box (summary) flushes this line first. */
    perf_rep_begin(PERF_REP_MON, "box.era");
    /* BACKLOG #263: ONE ROM open + table validation for the whole pass, not one per cell.
     * The batch (~1.1 KB) lives on this frame -- there is no spare EWRAM -- and begin() does
     * no I/O, so a box whose cells fetch nothing still costs nothing. Every path out of this
     * block reaches gb_art_batch_end() below (there is no early return between them). */
    GbArtBatch bt;
    gb_art_batch_begin(&bt);
    /* Reset first, THEN recompute. s_era_drawn survives across box flips and across whole
     * pdna_box() runs, and a stale set bit means a permanently hidden OBJ icon over a cell
     * that no longer has art to show — an empty cell holding a real Pokemon. Handing every
     * marked cell back to the OAM layer up front makes that unrepresentable. */
    for (int i = 0; i < G3_BOX_SLOTS; i++) era_cell_icon_back(i);
    for (int i = 0; i < G3_BOX_SLOTS; i++) era_cell_draw(i);
    gb_art_batch_end(&bt);
    perf_rep_end(PERF_REP_MON);
  }
  /* OUTSIDE the bail, deliberately. The art source decodes into a buffer it owns and
   * PUBLISHES A POINTER TO — and the obvious home for that buffer is mon_decomp, which
   * this screen's own Gen-3 portrait streams through moments later. Dropping the
   * router's fetch memo is what stops a later HIT handing out pixels that have since
   * been overwritten (pdna_origin_art.h's contract), and a box flip from a box that DID
   * fetch into one that has nothing to fetch is precisely when that would bite. */
  pdna_origin_art_invalidate();
}

/* A boxoam_load_box() re-lays out and re-SHOWS all 30 grid entries, so cells wearing
 * bitmap era art have to be re-hidden or the Gen-3 icon reappears on top of the Gen-1
 * sprite. Re-applied from here rather than remembered inside box_oam.c, so the OAM layer
 * goes on knowing nothing about generations. */
static void era_hides_apply(void) {
  if (!s_era_drawn) return;
  for (int i = 0; i < G3_BOX_SLOTS; i++)
    if (s_era_drawn & (1u << i)) boxoam_hide_slot(i);
}

/* Repaint the box-name banner + occupancy + the on-title selection frame (BG, software). */
static void draw_box_banner(BoxSource* src, int box, bool on_title) {
  char bn[12], bnocc[24], bnum[6];
  src->get_name(box, bn);
  int occ = 0;
  for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
  /* box+1: this file's `box` is the zero-based index everywhere, and this banner is
   * the ONLY box identifier on screen (there is no box-select screen in this app), so
   * the ordinal it shows has to be right. The one slot with no ordinal at all is the
   * GB sources' party pseudo-box at nboxes-1 ("GB PARTY" -- RBY has 12 boxes, so a
   * "13:" prefix there would be an invented number contradicting pdna_gen12.c's own
   * party special-case): last_box_is_party suppresses the prefix, and draw_banner's
   * NULL-pfx path degenerates to the original name-only centring. Computed BEFORE
   * bnocc now (BACKLOG #35 item 3): the same pseudo-box also holds at most a Gen-1/2
   * PARTY (6), not a 30-slot box, and "GB PARTY  6/30" for a genuinely full party
   * read as 24 short instead of full -- the /30 denominator was contradicting the
   * very party_slot special-case that already suppresses the ordinal for this exact
   * slot. */
  bool party_slot = src->last_box_is_party && box == src->nboxes - 1;
  /* BACKLOG #40(a): a GB source's box holds fewer than the 30-cell grid it is drawn
   * into (src->capacity, NULL for every source before this — falls back to the
   * Gen-3 PC/bank's real 30, unchanged for them). */
  int denom = party_slot ? 6 : (src->capacity ? src->capacity(box) : 30);
  siprintf(bnocc, "%s  %d/%d", bn[0] ? bn : "BOX", occ, denom);
  siprintf(bnum, "%d:", box + 1);
  /* BACKLOG #163: while THIS box is the one box_save() most recently refused to write,
   * say so here instead of a name/occupancy line that would read as perfectly fine --
   * box_save() itself runs from callers with no grid on screen (review G4) and its own
   * msg_wait is a one-shot dialog the user can dismiss and forget, so the persistent
   * banner is the thing that keeps saying it every time this box is drawn. Same rect,
   * same draw_banner() call, so it is guaranteed to fit (pinned by
   * tests/host_textfit_test.c) and can never drift out of sync with the normal row. */
  if (src->is_bank && pdna_bank_box_unsaved(box))
    draw_banner(WP_X + 2, 13, WP_W - 4, 0, PDNA_BANK_UNSAVED_BANNER);
  else
    draw_banner(WP_X + 2, 13, WP_W - 4, party_slot ? 0 : bnum, bnocc);
  /* Only when the cursor is ACTUALLY on the box name. `on_title` stays true while the cursor
   * is further up on the top tabs, so keying off it alone drew the identical frame in two
   * different focus states — a pixel diff of the banner band between "on box name" and "on
   * SAVE tab" showed ZERO changed pixels, which is exactly why it read as confusing.
   *
   * And the cue itself is doubled: a 1 px UI_SELTEXT hairline is indistinguishable from the
   * banner's own gold body, so the frame is drawn in the menu's selection colour (which
   * contrasts against gold) and two pixels thick. */
  if (on_title && s_tab_focus < 0) {
    m3_frame(WP_X,     12,     WP_X + WP_W - 1, 27,     UI_SEL);
    m3_frame(WP_X + 1, 13,     WP_X + WP_W - 2, 26,     UI_SEL);
  }
}

/* Full BG repaint (tabs + left panel + wallpaper + banner + footer). The icons,
 * cursor, carry and item markers are SPRITES (oam_sync), composited above this BG
 * by the GPU — so `moving` only affects the footer/sprite state, not the BG. With
 * clear=false it repaints OVER the current screen (no black flash) to wipe a modal. */
static void render_full(BoxSource* src, int box, int cur, bool on_title, bool moving, bool clear) {
  if (clear) ui_clear();
  draw_tab(0, PANEL_W + 1, "PKMN DATA", s_tab_focus < 0 || s_tab_focus == 0);
  draw_tab(PANEL_W + 1, 92, src->is_bank ? "(BANK)" : "PARTY", s_tab_focus == 1);
  draw_tab(PANEL_W + 93, UI_SCR_W - (PANEL_W + 93), "SAVE", s_tab_focus == 2);
  draw_left((on_title && s_tab_focus < 0) ? 0 : &g_box[cur]);   /* see draw_box_banner */

  draw_wallpaper(src->get_wp(box), WP_X, WP_Y, WP_W, WP_H);
  /* PARITY-AUDIT-2026-09 #75: era_cells() BEFORE artless_cells() -- it has to run
   * first so s_era_drawn is populated for THIS box before artless_cells() reads it
   * to decide, per cell, whether the chip is still needed. */
  era_cells();                  /* each cell in the art of the era it came from */
  artless_cells();
  blocked_cells(src, box);      /* BACKLOG #200 F1: mark cells past this source's capacity */
  draw_box_banner(src, box, on_title);
  draw_footer(src->is_bank, on_title, moving);

  oam_sync(cur, on_title, box, src->is_bank);        /* icons + cursor + carry + markers */
}

/* The PLACE beat — retail's mirror of the grab (capture doc §1e): fist + mon descend
 * together onto the cell, the mon detaches exactly at grid rest, the open hand rises.
 * Split in two so the data commit (drop_held, which may also SWAP) sits between the
 * halves; the up-half draws the rising cursor if the hand is empty now, or the risen
 * carry if a swap put another mon in it. */
static void play_place_anim_down(BoxSource* src, int box, int slot) {
  if (!app_anim_enabled(ANIM_BOX)) return;
  for (int d = 0; d <= 8; d++) {
    boxoam_cursor_dy(d);
    oam_sync(slot, false, box, src->is_bank);
    boxoam_commit(); s_vsync();
  }
}
static void play_place_anim_up(BoxSource* src, int box, int slot) {
  if (!app_anim_enabled(ANIM_BOX)) { boxoam_cursor_dy(0); return; }
  if (!s_holding) boxoam_hand_pose(BOXOAM_POSE_REACH);  /* the letting-go open hand */
  for (int d = 8; d >= 0; d--) {
    boxoam_cursor_dy(d);
    oam_sync(slot, false, box, src->is_bank);
    boxoam_commit(); s_vsync();
  }
  boxoam_hand_pose(BOXOAM_POSE_NORMAL);
  boxoam_cursor_dy(0);
}

/* Pick-up grab cue when MOVE is chosen — retail's beat, not a hold. Emerald's
 * MonPlaceChange_Grab is pure pose + motion: the hand dips 8 px at 1 px/frame in the
 * wide-OPEN pose while the mon stays in its cell, the fist closes at the bottom, then
 * hand + mon rise 8 px together. Our carry state (orange fist + riding icon) IS the
 * "closed at the bottom" look, so the rise just animates the carry sprites up to their
 * float height with the same dy driver. Pure OAM, ~18 frames, gated on the Box
 * animation toggle like every other beat. */
static void play_grab_anim(BoxSource* src, int box, int slot) {
  if (!app_anim_enabled(ANIM_BOX)) { (void)src; (void)box; (void)slot; return; }
  /* descend: open hand over the still-in-place mon */
  s_grab_dip = 1;
  boxoam_hand_pose(BOXOAM_POSE_REACH);
  for (int d = 0; d <= 8; d++) {
    boxoam_cursor_dy(d);
    oam_sync(slot, false, box, src->is_bank);
    boxoam_commit(); s_vsync();
  }
  /* the fist closes: switch to the carry look, still at the cell */
  s_grab_dip = 0;
  boxoam_hand_pose(BOXOAM_POSE_NORMAL);
  /* rise: fist + mon come up from the cell to the carry float */
  for (int d = 8; d >= 0; d--) {
    boxoam_cursor_dy(d);
    oam_sync(slot, false, box, src->is_bank);
    boxoam_commit(); s_vsync();
  }
  boxoam_cursor_dy(0);
}

/* ITEM-take grab cue (the mon single-grab's play_grab_anim beat, mirrored for items —
 * taking an item used to snap with no animation at all): the orange grab fist closes
 * over the holder with the taken item popping full-size in front (boxoam_carry_item's
 * full-grab look, the existing fist/hand OAM machinery), holds a beat, then the normal
 * repaint swaps in the small carried-item ride. Pure OAM, keypress-cadence only. */
static void play_item_grab_anim(int cur, uint16_t item) {
  boxoam_carry_item(cur, item, true);              /* fist + full item over the mon */
  for (int v = 0; v < 10; v++) { boxoam_commit(); s_vsync(); }
}

/* Light update on cursor move: with hardware sprites the icons composite themselves,
 * so this only repositions the cursor sprite (in oam_sync), refreshes the left PKMN-
 * DATA panel (the selected mon changed) and the on-title banner/footer/frame. No
 * software icon repaint, no erase — the GPU handles overlap. */
/* Retail slides the cursor cell-to-cell at 4 px/frame (6 frames per cell, 12 on a
 * wrap) instead of snapping (capture doc §1b). One shared slider: offset the hand
 * (and carry pair) from the DESTINATION back toward the old cell and walk it in. */
static void cursor_slide(BoxSource* src, int box, int old_cur, int cur, bool carrying) {
  if (!app_anim_enabled(ANIM_BOX) || old_cur == cur) return;
  int dx = ((old_cur % COLS) - (cur % COLS)) * CELL_W;
  int dy = ((old_cur / COLS) - (cur / COLS)) * CELL_H;
  int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
  steps /= 4; if (steps < 1) steps = 1;
  /* A column/row WRAP is a teleport, not travel: LEFT off column 0 is dx=120 and the
   * PC's DOWN off the bottom row is dy=88, which used to clamp to 12 steps — 183 ms of
   * dead, uninterruptible input on a press made constantly. Give a wrap a 2-frame
   * acknowledgement instead of animating a journey the cursor does not make. */
  if (steps > 12) steps = 2;
  for (int f = steps - 1; f >= 1; f--) {
    boxoam_cursor_dxy(dx * f / steps, dy * f / steps);
    oam_sync(cur, false, box, src->is_bank);
    boxoam_commit(); s_vsync();
    /* Bail the moment the user is already asking for the next cell. Held traversal
     * used to pay the full glide per cell even though key_repeat wants one every 3
     * frames (key_repeat_limits(14,3)), so a held d-pad ran ~2.5x slower than the
     * repeat rate it was configured for — the scroll felt like it was wading. A
     * level test, not key_hit: the repeat edge need not land on this frame. */
    if (key_is_down(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) break;
  }
  boxoam_cursor_dxy(0, 0);
  (void)carrying;
}

static void move_cursor(BoxSource* src, int box, int old_cur, bool old_title,
                        int cur, bool on_title) {
  if (on_title != old_title) {                        /* entering/leaving the title row */
    /* BACKLOG #268: the ONLY thing that changes on the way onto or off the box name is the
     * banner's selection frame -- a 2 px ring in y 12..27 (draw_box_banner), above the grid
     * (GRID_Y = 30). This used to repaint the WHOLE wallpaper and then redraw every cell
     * layer over it: for a Game Boy box that is a fresh art fetch of all 20 cells, so a plain
     * UP onto the name (and DOWN off it) reloaded the box, visibly (Guy: "it reloads the box.
     * It shouldn't as its already loaded"). The Gen-3 grid never showed it because its icons
     * are sprites; the Game Boy art lives in the bitmap, and the wallpaper repaint wiped it.
     *
     * So restore just the band the ring sat in (the same rect-limited restore the chunk
     * carry uses) and redraw the banner and footer over it. Cells, era art, era pads,
     * name chips and the blocked-cell hatch are outside the band and are never touched.
     * The full WP_W: the procedural grass covers all 162 columns (as draw_wallpaper's does),
     * while the tiled wallpaper's map is 20 tiles = 160 px wide, so wp_restore_rect clamps to
     * the map and the two columns past it are left exactly as draw_wallpaper leaves them.
     * Known: the selection frame itself paints x=238, which wp_restore_rect never
     * repaints, so a 1-px sliver can remain -- pre-existing, BACKLOG #274 (not fixed here). */
    wp_restore_rect(WP_X, WP_Y, WP_W, 16);
    draw_box_banner(src, box, on_title);
    draw_footer(src->is_bank, on_title, false);
  }
  draw_left(on_title ? 0 : &g_box[cur]);              /* the selected mon changed */
  if (!on_title && !old_title) cursor_slide(src, box, old_cur, cur, false);
  oam_sync(cur, on_title, box, src->is_bank);         /* reposition cursor sprite */
}

/* Update while CARRYING a mon (move-mode): just reposition the carry sprites and the
 * left panel. The GPU composites; nothing to erase. */
static void carry_move(BoxSource* src, int box, int old_cur, int cur) {
  draw_left(&g_box[cur]);                             /* panel follows the destination cell */
  cursor_slide(src, box, old_cur, cur, true);
  oam_sync(cur, false, box, src->is_bank);           /* moves OE_CARRY + OE_GRAB sprites */
}

/* ======================= Multi-select chunk carry (Emerald multi-move) ======================= */

/* OAM for chunk carry: the box icons (reloaded if needed), then the WHOLE lifted block
 * floating at the anchor (whitened = fits, darkened = blocked) with the fist riding the
 * grab cell, and — while viewing the SOURCE box — the lifted source cells hidden LAST
 * (a block move may restore a just-uncovered source cell; re-hide it). Order matters:
 * item_markers/carry_item hide OAM entries 34..63/CITEM, so they run BEFORE the block. */
static void chunk_oam_sync(int box, bool is_bank, bool fit) {
  if (s_oam_reload) { boxoam_load_box(g_box); s_oam_reload = false; }
  s_ch_on_src = (box == s_ch_box && is_bank == (s_ch_scope == BOXSCOPE_BANK));   /* §12b: source cells vacate, no understudy */
  boxoam_item_markers(g_box, false);
  boxoam_carry_item(g3_slot(s_ch_tr, s_ch_tc), 0, false);
  boxoam_chunk_carry(s_ch_tr, s_ch_tc, s_ch_tr + s_ch_fr, s_ch_tc + s_ch_fc,
                     s_ch_cells, s_ch.n, fit, s_ch_lift);
  if (box == s_ch_box && is_bank == (s_ch_scope == BOXSCOPE_BANK))
    for (int i = 0; i < s_ch.n; i++) boxoam_hide_slot(s_ch.src[i]);   /* lift-hide the sources */
}

/* Footprint fit test vs the CURRENT box (own sources count as vacating on the source box).
 * `recs` (BACKLOG #150 S150-2, G-H2, finding 1): box_occupancy needs the raw records to
 * see a native cell that g_box[].species cannot represent. chunk_fit never calls
 * src->records(box) itself -- that can flush a dirty bank box (pdna_bank.c's
 * banksrc_records) -- every caller below already holds the `recs` for this exact `box`
 * (begin_select's own parameter; the main render loop's recs/SWITCH_BOX pairing, which
 * keeps recs and box in lockstep at every reassignment of either), so this is pure
 * parameter threading, zero new I/O. */
static bool chunk_fit(BoxSource* src, int box, const uint8_t* recs) {
  uint8_t dest[G3_BOX_SLOTS], vac[G3_BOX_SLOTS], tgt[G3_BOX_SLOTS];
  bool same = (box == s_ch_box && src->is_bank == (s_ch_scope == BOXSCOPE_BANK));
  box_occupancy(src, box, recs, dest);           /* moving-out bank mons still count as occupied */
  if (same) { memset(vac, 0, sizeof vac); for (int i = 0; i < s_ch.n; i++) vac[s_ch.src[i]] = 1; }
  return chunk_can_drop(&s_ch, s_ch_tr, s_ch_tc, dest, same ? vac : 0, tgt);
}

/* Light anchor-move update: the block, its fit tint, and the fist are pure OAM — no
 * bitmap touches at all (the old code re-blitted the whole wallpaper per step). */
static void chunk_move(BoxSource* src, int box, const uint8_t* recs) {
  chunk_oam_sync(box, src->is_bank, chunk_fit(src, box, recs));
}

/* Full repaint while carrying a chunk: BG chrome + the floating block (whitened = fits here,
 * darkened = blocked) at the anchor. clear=false repaints OVER the current screen (no black
 * flash) so a box switch doesn't flicker; plain anchor moves use chunk_move (OAM-only). */
static void chunk_draw(BoxSource* src, int box, bool clear, const uint8_t* recs) {
  if (clear) ui_clear();
  draw_tab(0, PANEL_W + 1, "PKMN DATA", true);
  draw_tab(PANEL_W + 1, 92, src->is_bank ? "(BANK)" : "PARTY", false);
  draw_tab(PANEL_W + 93, UI_SCR_W - (PANEL_W + 93), "SAVE", false);
  PkMon rep; pk_decode_mon(s_ch_rec[0], false, &rep); pk_resolve(&rep);
  draw_left(&rep);                                    /* the panel shows what you're carrying */

  draw_wallpaper(src->get_wp(box), WP_X, WP_Y, WP_W, WP_H);
  /* PARITY-AUDIT-2026-09 #75: same reorder as render_full/move_cursor -- era_cells()
   * first so artless_cells()'s per-cell s_era_drawn check is fresh. */
  era_cells();                     /* same pairing as move_cursor: BG repaint owes both */
  artless_cells();
  blocked_cells(src, box);      /* BACKLOG #200 F1: mark cells past this source's capacity
                                  * (a no-op today -- chunk carry refuses BOXSCOPE_GB, see
                                  * begin_select -- but keeps the "every wallpaper repaint
                                  * owes every BG cell layer" invariant true everywhere) */

  /* no footprint frame — the block itself carries the fit cue (whitened/darkened).
   * Budget is 20 columns (WP_W=162px fill, text at WP_X+2 -> 160px/8). The double-
   * spaced original ("x%d  A drop  B cancel") was exactly 20 chars at a single-digit
   * count and 21 at two digits (s_ch.n can reach 30, the whole box) -- it fit by
   * accident up to x9 and clipped from x10 on. Single-spaced, the n=30 worst case is
   * 19 chars ("x30 A drop B cancel"), inside the budget with one column to spare. */
  char f[28]; siprintf(f, "x%d A drop B cancel", s_ch.n);
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), f);

  chunk_oam_sync(box, src->is_bank, chunk_fit(src, box, recs));

  /* §12b: draw_wallpaper above WIPED every understudy blit, but a full repaint with
   * an UNMOVED anchor (refused-drop popups etc.) fires no cover transitions in
   * box_oam — re-blit the covered occupied cells ourselves (under_show no-ops on
   * cells that still carry their blit / are empty / are lifted sources). */
  for (int i = 0; i < s_ch.n; i++)
    boxoam_under_show((s_ch_tr + s_ch_cells[i].rr) * COLS + (s_ch_tc + s_ch_cells[i].cc));
}

/* Footer line for the rubber-band selection (count of occupied cells inside). */
static void draw_select_footer(int cnt) {
  char f[28]; siprintf(f, "SELECT %d  A+DPAD", cnt);   /* short: must end < x=240 */
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), f);
}

/* Emerald-style selection: whiten the OCCUPIED icons inside [a..b] — the highlight
 * IS the icons, so nothing can misalign. No rectangle, no wallpaper repaint. */
static void update_select(int a, int b) {
  int ar = g3_row(a), ac = g3_col(a), br = g3_row(b), bc = g3_col(b);
  int r0 = ar < br ? ar : br, r1 = ar < br ? br : ar;
  int c0 = ac < bc ? ac : bc, c1 = ac < bc ? bc : ac;
  uint8_t sel[G3_BOX_SLOTS]; int cnt = 0;
  for (int s = 0; s < G3_BOX_SLOTS; s++) sel[s] = 0;
  for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++)
    if (g_box[g3_slot(r, c)].species) { sel[g3_slot(r, c)] = 1; cnt++; }
  boxoam_select_mark(sel);
  draw_select_footer(cnt);
}

/* PC -> Bank chunk drop: an always-RELEASE batch move. Copy each held mon into the bank box
 * (per-mon progress sprite), VERIFY the bank box on SD once, and only on success release the
 * PC sources (deferred to the one exit save). On failure, revert the (empty) target cells and
 * keep holding — nothing is lost. `tgt` was verified all-empty by the caller. */
static uint8_t* drop_chunk_pc_to_bank(BoxSource* src, int box, uint8_t* recs, const uint8_t* tgt) {
  (void)box;
  boxoam_suspend();
  for (int i = 0; i < s_ch.n; i++) {
    PkMon m; pk_decode_mon(s_ch_rec[i], false, &m); pk_resolve(&m);
    pdna_progress_frame("SEND TO BANK", &m, i, s_ch.n, "Copying...");
    memcpy(recs + (uint32_t)tgt[i] * 80, s_ch_rec[i], 80);
    for (int v = 0; v < 5; v++) s_vsync();             /* let each sprite show */
  }
  PkMon last; pk_decode_mon(s_ch_rec[s_ch.n - 1], false, &last); pk_resolve(&last);
  pdna_progress_frame("SEND TO BANK", &last, s_ch.n, s_ch.n, "Saving to card...");
  bool ok = src->commit();                             /* verified bank box_save (banksrc_commit) */
  if (ok) {
    for (int i = 0; i < s_ch.n; i++) app_pc_release_slot(s_ch_box, s_ch.src[i], s_ch_rec[i]);
    snd_save();
    pdna_progress_frame("SENT TO BANK", &last, s_ch.n, s_ch.n, "Released from save");
    for (int v = 0; v < 45; v++) s_vsync();             /* brief hold on "done" */
    s_ch_hold = false;
  } else {
    for (int i = 0; i < s_ch.n; i++) memset(recs + (uint32_t)tgt[i] * 80, 0, 80);   /* revert (were empty) */
    snd_error();
    ui_clear();
    ui_panel(20, 60, 200, 44, UI_PANEL, UI_WARN);
    ui_text(30, 70, UI_WARN, "BANK WRITE FAILED");
    ui_text(30, 86, UI_DIM, "Kept in the save. Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);   /* keep holding the chunk */
  }
  boxoam_resume();
  box_decode(src, recs, box); s_oam_reload = true;
  return recs;
}

/* Drop the carried chunk into the current box with its top-left at (s_ch_tr,s_ch_tc). Picks the
 * right semantics by scope. *pfull is always set (a redraw is due). Returns (maybe reloaded) recs. */
/* Bank->bank cross-box MOVE (multi-select into a clean/free region of another bank box). The bank
 * pages ONE box at a time, so: write the chunk into the DEST box (current) and COMMIT it (verified)
 * FIRST, THEN page in the SOURCE box, clear the moved slots and commit it. Dest-before-source: a
 * failure after the dest commit leaves a recoverable duplicate, never a loss. Both are small bank
 * box files -> immediate (no deferral): the mons really leave the source box. `tgt` = verified-empty
 * destination slots. */
static uint8_t* drop_chunk_bank_cross(BoxSource* src, int box, const uint8_t* tgt) {
  uint8_t* recs = src->records(box);                 /* dest box (current, already loaded) */
  boxoam_suspend();
  for (int i = 0; i < s_ch.n; i++) {
    PkMon m; pk_decode_mon(s_ch_rec[i], false, &m); pk_resolve(&m);
    pdna_progress_frame("MOVE IN BANK", &m, i, s_ch.n, "Writing...");
    memcpy(recs + (uint32_t)tgt[i] * 80, s_ch_rec[i], 80);
    for (int v = 0; v < 4; v++) s_vsync();
  }
  PkMon last; pk_decode_mon(s_ch_rec[s_ch.n - 1], false, &last); pk_resolve(&last);
  pdna_progress_frame("MOVE IN BANK", &last, s_ch.n, s_ch.n, "Saving...");
  if (!src->commit()) {                              /* persist the DEST box (verified) */
    for (int i = 0; i < s_ch.n; i++) memset(recs + (uint32_t)tgt[i] * 80, 0, 80);   /* revert (were empty) */
    snd_error();
    ui_clear(); ui_panel(20, 60, 200, 44, UI_PANEL, UI_WARN);
    ui_text(30, 70, UI_WARN, "BANK WRITE FAILED");
    ui_text(30, 86, UI_DIM, "Kept in place. Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    boxoam_resume();
    recs = src->records(box); box_decode(src, recs, box); s_oam_reload = true;
    return recs;                                     /* keep holding */
  }
  /* Dest committed -> clear the moved slots in the SOURCE box. app_bank_clear_slots pages that box
   * in and REFUSES to rewrite it if the page-in read was incomplete or nothing matched: committing a
   * zeroed/partial buffer would wipe the source box's untouched BYSTANDER mons, and bank box files
   * take only a single rolling .bak (box_save's sf_save_rolling, BACKLOG #150 S150-0), not an
   * immutable backup -- there is no earlier generation to fall back past that one file. If it
   * refuses, the move simply degrades to a safe duplicate (the mons live in both boxes) — never a
   * loss. */
  app_bank_clear_slots(s_ch_box, s_ch.src, (const uint8_t (*)[80])s_ch_rec, s_ch.n);
  recs = src->records(box);                          /* page the dest box back for display */
  boxoam_resume();
  snd_save();
  s_ch_hold = false; box_decode(src, recs, box); s_oam_reload = true;
  return recs;
}

/* Does the footprint at the current anchor cover a bank cell that still holds a moving-out mon?
 * Those read as EMPTY (box_decode hides them) but can't be overwritten until the PC is saved — so
 * a refused drop there needs explaining rather than a bare deny beep. */
static bool footprint_hits_pending(BoxSource* src, int box) {
  if (!src->is_bank) return false;
  if (s_ch_tr + s_ch.h > G3_BOX_ROWS || s_ch_tc + s_ch.w > G3_BOX_COLS) return false;
  for (int r = 0; r < s_ch.h; r++)
    for (int c = 0; c < s_ch.w; c++)
      if (app_bank_slot_pending(box, g3_slot(s_ch_tr + r, s_ch_tc + c))) return true;
  return false;
}

/* Drop the carried chunk into the current box with its top-left at (s_ch_tr,s_ch_tc). Picks the
 * right semantics by scope. *pfull is always set (a redraw is due). Returns (maybe reloaded) recs. */
static uint8_t* drop_chunk(BoxSource* src, int box, uint8_t* recs, bool* pfull) {
  /* §2.3 belt-and-braces: no chunk ever crosses a scope pair involving BOXSCOPE_GB (v1).
   * The chunk edges above already keep a GB-origin chunk from reaching a foreign grid and
   * a Bank chunk from reaching the GB grid, so this is unreachable today (S1: `can_lift`
   * is NULL everywhere, so a GB source's can_edit()==false already blocks any chunk grab)
   * — but drop_chunk is the one place that would actually memcpy converted-looking bytes
   * into a foreign records buffer, so it denies on its own, before any memcpy, rather than
   * trusting the edges alone. */
  if (xg_chunk_crossgen_denied(src->scope, s_ch_scope)) { snd_deny(); *pfull = false; return recs; }
  /* BACKLOG #150 S150-3 decision 4: dominates every memcpy in drop_chunk_pc_to_bank,
   * drop_chunk_bank_cross and this function's own same-scope/Bank->PC-deferred writes
   * below -- all four are reached only through this function's own control flow, never
   * called directly. Refuses the WHOLE drop (keeps the chunk in hand) if ANY selected
   * cell is native and the destination is not the Bank -- a chunk never partially lands. */
  for (int i = 0; i < s_ch.n; i++) {
    if (xg_native_escape_denied(s_ch_rec[i], src->scope)) { snd_deny(); *pfull = false; return recs; }
  }
  *pfull = true;

  uint8_t dest[G3_BOX_SLOTS], vac[G3_BOX_SLOTS], tgt[G3_BOX_SLOTS];
  bool same = (box == s_ch_box && src->is_bank == (s_ch_scope == BOXSCOPE_BANK));
  box_occupancy(src, box, recs, dest);            /* moving-out bank mons still count as occupied */
  if (same) { memset(vac, 0, sizeof vac); for (int i = 0; i < s_ch.n; i++) vac[s_ch.src[i]] = 1; }
  if (!chunk_can_drop(&s_ch, s_ch_tr, s_ch_tc, dest, same ? vac : 0, tgt)) {
    snd_deny();
    if (footprint_hits_pending(src, box)) {      /* the one non-obvious block: cells that LOOK empty */
      boxoam_suspend(); ui_clear();
      ui_panel(20, 56, 200, 52, UI_PANEL, UI_WARN);
      ui_text(30, 66, UI_WARN, "CELLS NOT FREE YET");
      ui_text(30, 82, UI_DIM, "They hold mons you moved");
      ui_text(30, 92, UI_DIM, "out. Save first. Press A");
      u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
      boxoam_resume();
    } else *pfull = false;                       /* bare refusal: beep only — Emerald repaints nothing
                                                  * (kills the ui_clear flash on every blocked A) */
    return recs;
  }

  /* Emerald place beat (mirror of the grab): block + fist descend together onto the
   * cells and hold a beat; the repaint after the data move swaps the real icons in at
   * touch-down. (The rare TOO-MANY-MOVES refusal below re-lifts afterwards — fine.) */
  for (int v = 7; v >= 0; v--) { s_ch_lift = v; chunk_oam_sync(box, src->is_bank, true);
                                 boxoam_commit(); s_vsync(); }
  for (int v = 0; v < 4; v++) { boxoam_commit(); s_vsync(); }
  /* ...then the OPEN hand rises off the placed block, mirroring the lift's descent
   * (capture doc §4.6). The block itself stays down — only the hand leaves. */
  if (app_anim_enabled(ANIM_BOX)) {
    boxoam_hand_pose(BOXOAM_POSE_REACH);
    int tr = (s_tab_focus >= 0) ? (2 + s_tab_focus) : 0;
    for (int d = 8; d >= 0; d--) {
      boxoam_cursor_dy(d);
      boxoam_cursor(g3_slot(s_ch_tr + s_ch_fr, s_ch_tc + s_ch_fc), tr, cursor_look(), cursor_label_cx(tr));
      boxoam_commit(); s_vsync();
    }
    boxoam_cursor_dy(0);
    boxoam_hand_pose(BOXOAM_POSE_NORMAL);
  }
  s_ch_lift = 8;                                        /* carry height again for whoever keeps holding */

  /* ---- Bank -> Bank, different box: immediate paging-aware move (commit dest, then clear source) ---- */
  if (src->is_bank && s_ch_scope == BOXSCOPE_BANK && box != s_ch_box)
    return drop_chunk_bank_cross(src, box, tgt);

  /* ---- same scope, same box (bank rearrange) OR PC<->PC (any box): a plain deferred move ---- */
  if (src->is_bank == (s_ch_scope == BOXSCOPE_BANK)) {
    uint8_t* srcp = (box == s_ch_box) ? recs : src->records(s_ch_box);   /* PC: g_pc box (no paging); bank is same-box here */
    for (int i = 0; i < s_ch.n; i++) memset(srcp + (uint32_t)s_ch.src[i] * 80, 0, 80);   /* clear sources first */
    for (int i = 0; i < s_ch.n; i++) {
      memcpy(recs + (uint32_t)tgt[i] * 80, s_ch_rec[i], 80);
      if (src->note_add) src->note_add(s_ch_rec[i]);
    }
    src->mark_dirty();
    snd_ok(); s_ch_hold = false; box_decode(src, recs, box); s_oam_reload = true;
    return recs;
  }

  /* ---- PC -> Bank: always-release batch (verified bank write, then release the PC sources) ---- */
  if (src->is_bank && s_ch_scope != BOXSCOPE_BANK)
    return drop_chunk_pc_to_bank(src, box, recs, tgt);

  /* ---- Bank -> PC: a DEFERRED move that LOOKS immediate. No disk write here — the user has a SAVE
   * tab and the exit "Save changes?" prompt for that (Guy), so the real write batches later. The mons
   * are placed into g_pc and their bank sources queued for deletion; box_decode hides those slots so
   * the bank reads as if they really left, while the record stays in the box file (their only on-card
   * copy) until the PC is committed — then flush_on_exit deletes them, gated on that commit
   * succeeding. Nothing may overwrite a hidden slot meanwhile (box_occupancy keeps it occupied). ---- */
  if (!app_bank_defer_room(s_ch.n)) {
    snd_deny();
    boxoam_suspend(); ui_clear();
    ui_panel(20, 60, 200, 44, UI_PANEL, UI_WARN);
    ui_text(30, 70, UI_WARN, "TOO MANY MOVES");
    ui_text(30, 86, UI_DIM, "Save first, then continue.");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    boxoam_resume();
    return recs;
  }
  for (int i = 0; i < s_ch.n; i++) {
    memcpy(recs + (uint32_t)tgt[i] * 80, s_ch_rec[i], 80);
    if (src->note_add) src->note_add(s_ch_rec[i]);
    app_bank_defer_delete(s_ch_box, s_ch.src[i], s_ch_rec[i]);
  }
  src->mark_dirty();
  snd_ok(); s_ch_hold = false; box_decode(src, recs, box); s_oam_reload = true;
  return recs;
}

/* CM_MOVE + A: rubber-band multi-select. While A is held, the D-pad grows/shrinks a rectangle
 * from the anchor (cur); releasing A lifts every mon inside as a chunk. A plain tap (no drag)
 * over an occupied cell grabs just that mon (the classic single carry). Sets *pfull. */
static uint8_t* begin_select(BoxSource* src, int box, uint8_t* recs, int cur, bool* pfull) {
  /* Retail (§1g of the capture doc): A-DOWN alone shows the OPEN hand over the mon —
   * the multi-select theme appears only when a drag actually starts. Theming on the
   * press was divergence #1: a plain tap flashed the whole re-themed box (with
   * Mode-3 repaint tearing) for ~16 frames before the grab beat. */
  *pfull = false;
  int anchor = cur, corner = cur;
  bool themed = false;
  boxoam_hand_pose(BOXOAM_POSE_REACH);
  int tr = (s_tab_focus >= 0) ? (2 + s_tab_focus) : 0;
  boxoam_cursor(cur, tr, cursor_look(), cursor_label_cx(tr));
  boxoam_commit();
  for (;;) {
    s_vsync();                                          /* polls keys */
    boxoam_commit();
    if (!key_is_down(KEY_A)) break;                     /* A released -> finalize */
    u16 kk = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)
           | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (!kk) continue;
    if (!themed) {                                      /* the drag begins NOW */
      themed = true; *pfull = true;
      boxoam_hand_pose(BOXOAM_POSE_NORMAL);
      update_select(anchor, corner);
      boxoam_select_cursor();
    }
    int r = g3_row(corner), c = g3_col(corner);
    if ((kk & KEY_LEFT)  && c > 0)        c--;
    if ((kk & KEY_RIGHT) && c < COLS - 1) c++;
    if ((kk & KEY_UP)    && r > 0)        r--;
    if ((kk & KEY_DOWN)  && r < ROWS - 1) r++;
    int nc = g3_slot(r, c);
    if (nc != corner) { corner = nc; snd_move(); update_select(anchor, corner);
                        boxoam_select_cursor(); }
  }
  /* --- finalize --- */
  if (themed) boxoam_select_clear();                    /* un-whiten; grab/deny takes over */
  boxoam_hand_pose(BOXOAM_POSE_NORMAL);
  if (anchor == corner) {                               /* no drag: the classic single-mon grab */
    /* BACKLOG #150 S150-4 step 3 / BACKLOG #199: the caller's src_can_lift() gate
     * (the CM_MOVE dispatch above begin_select's own call) is the only refusal a GB
     * grab can hit any more -- start_carry() no longer calls lift_up() (BACKLOG #199
     * moved that to drop_held_up, the Bank-UP drop), so its return is always true for
     * a real cell. The check stays: cheap, and start_carry()'s own contract comment
     * still asks every call site to check it. */
    if (g_box[anchor].species && start_carry(src, recs, box, anchor)) {
      if (themed) render_full(src, box, anchor, false, false, false);   /* only to undo the theme */
      play_grab_anim(src, box, anchor);                 /* hand stays on screen throughout */
      carry_move(src, box, anchor, anchor);
      draw_footer(src->is_bank, false, true);
      *pfull = false;
    } else { snd_deny(); *pfull = true; }
    return recs;
  }
  /* BACKLOG #150 S150-5 decision 8(a): a GB-scope carry stays SINGLE -- a chunk carry
   * going through the same can_lift/lift_up machinery decision 8 just opened up would
   * be a second, much larger surface to re-verify (the re-verify in gb_release_up_hook
   * only ever expects ONE cell's worth of identity to match). Refuse before the chunk
   * is even built (SS11.2 step 2: "a CHUNK carry stays denied in every cross-gen case"). */
  if (src->scope == BOXSCOPE_GB) { snd_deny(); return recs; }
  /* build the chunk from the OCCUPIED cells inside the rectangle */
  uint8_t occ[G3_BOX_SLOTS];
  for (int s = 0; s < G3_BOX_SLOTS; s++) occ[s] = g_box[s].species ? 1 : 0;
  if (chunk_build(&s_ch, occ, anchor, corner) == 0) { snd_deny(); return recs; }
  /* BACKLOG #150 S150-3 decision 5: a native cell never enters a chunk in v1 -- a lift has
   * no destination yet, so this checks bc_is_native() directly against the RAW bytes still
   * in `recs`, not the escape predicate. Denies the whole selection outright (s_ch_hold
   * stays unset, nothing copied into s_ch_rec). */
  for (int i = 0; i < s_ch.n; i++)
    if (bc_is_native(recs + (uint32_t)s_ch.src[i] * 80)) { snd_deny(); return recs; }
  for (int i = 0; i < s_ch.n; i++) memcpy(s_ch_rec[i], recs + (uint32_t)s_ch.src[i] * 80, 80);
  for (int i = 0; i < s_ch.n; i++) {                    /* decode ONCE for the block display */
    PkMon m; pk_decode_mon(s_ch_rec[i], false, &m);
    s_ch_cells[i].rr = s_ch.rr[i];  s_ch_cells[i].cc = s_ch.cc[i];
    s_ch_cells[i].species = m.species;  s_ch_cells[i].form = m.form;
    s_ch_cells[i].egg = (m.isEgg && !m.isBadEgg) ? 1 : 0;
  }
  s_ch_hold = true; s_ch_box = box; s_ch_scope = src->scope;
  s_ch_tr = g3_row(s_ch.src[0]) - s_ch.rr[0];           /* footprint top-left in the source box */
  s_ch_tc = g3_col(s_ch.src[0]) - s_ch.cc[0];
  s_ch_fr = g3_row(corner) - s_ch_tr;                   /* fist rides the cell A was released on */
  s_ch_fc = g3_col(corner) - s_ch_tc;
  s_oam_reload = true;                                  /* Emerald's grab itself is SILENT (the A-press
                                                         * earcon at select START already matched) */
  /* Emerald grab beat (MultiMove_GrabSelection): the fist closes on the block in
   * place, holds a moment, then block + fist rise together 1px/frame to the 8px carry
   * height. After the first draw the anchor is unmoved, so every rise frame is pure OAM. */
  s_ch_lift = 0;
  chunk_draw(src, box, false, recs);
  /* Retail descends the OPEN hand onto the block before the fist closes on it — the
   * same beat as the single-mon grab (capture doc §4.6). chunk_draw has already put
   * the fist sprite up, so the descent runs on the cursor's own dy driver and the
   * fist is re-placed by chunk_move at the end. */
  if (app_anim_enabled(ANIM_BOX)) {
    boxoam_hand_pose(BOXOAM_POSE_REACH);
    int tr = (s_tab_focus >= 0) ? (2 + s_tab_focus) : 0;
    for (int d = 0; d <= 8; d++) {
      boxoam_cursor_dy(d);
      boxoam_cursor(g3_slot(s_ch_tr + s_ch_fr, s_ch_tc + s_ch_fc), tr, cursor_look(), cursor_label_cx(tr));
      boxoam_commit(); s_vsync();
    }
    boxoam_cursor_dy(0);
    boxoam_hand_pose(BOXOAM_POSE_NORMAL);
    chunk_draw(src, box, false, recs);                        /* fist look back, hand hidden */
  }
  for (int v = 0; v < 8; v++) { boxoam_commit(); s_vsync(); }
  for (int v = 1; v <= 8; v++) { s_ch_lift = v; chunk_move(src, box, recs); boxoam_commit(); s_vsync(); }
  *pfull = false;
  return recs;
}

/* §14 wallpaper chooser: FIRST the game's own wallpaper-set menu, THEN the existing
 * live-preview cycle within the chosen set. Groups verified against Emerald's
 * AddWallpaperSetsMenu/AddWallpapersMenu (pokemon_storage_system.c:4327-4370):
 *   SCENERY 1 = forest/city/desert/savanna (0..3), SCENERY 2 = crag/volcano/snow/cave
 *   (4..7), SCENERY 3 = beach/seafloor/river/sky (8..11), ETCETERA = polkadot/
 *   pokecenter/machine/plain ("SIMPLE") (12..15), FRIENDS = the 16 Walda patterns
 *   (chooser ids 16..31, Emerald PC only — wp_count==32). RS/FRLG/bank: 4 groups. */
static const char* const WP_GROUP[5] = { "Scenery 1", "Scenery 2", "Scenery 3", "Etcetera", "Friends" };

/* the little set menu, drawn over whatever is on screen (box_options_menu style).
 * Returns the picked group index or -1 on B. */
static int wallpaper_group_menu(int ngroups, int gsel) {
  for (;;) {
    const int mx = 60, my = 46, mw = 120, mh = 18 + ngroups * 14 + 11;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "WALLPAPER");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < ngroups; i++) {
      int y = my + 18 + i * 14; bool s = (i == gsel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, WP_GROUP[i]);
    }
    ui_text(mx + 6, my + mh - 9, UI_DIM, "A pick B back");
    u16 k; do { s_vsync(); k = key_hit(KEY_UP | KEY_DOWN | KEY_A | KEY_B); } while (!k);
    if (k & KEY_B)         { snd_back(); return -1; }
    else if (k & KEY_UP)   { snd_move(); gsel = (gsel > 0) ? gsel - 1 : ngroups - 1; }
    else if (k & KEY_DOWN) { snd_move(); gsel = (gsel + 1) % ngroups; }
    else if (k & KEY_A)    { snd_ok(); return gsel; }
  }
}

static int wallpaper_pick(BoxSource* src, int cur_wp) {
  int count = src->wp_count > 0 ? src->wp_count : G3_BOX_WALLPAPER_COUNT;
  int ngroups = (count > G3_BOX_WALLPAPER_COUNT) ? 5 : 4;    /* Friends set: Emerald PC only */
  int wp = (cur_wp >= 0 && cur_wp < count) ? cur_wp : 0;
  int gsel = (wp >= G3_BOX_WALLPAPER_COUNT) ? 4 : wp / 4;    /* open on the current wp's set */
  for (;;) {
    int g = wallpaper_group_menu(ngroups, gsel);
    if (g < 0) return -1;
    gsel = g;
    int base = (g < 4) ? g * 4 : G3_BOX_WALLPAPER_COUNT;     /* the set's id range */
    int glen = (g < 4) ? 4 : G3_WALDA_COUNT;
    if (wp < base || wp >= base + glen) wp = base;           /* keep wp when re-entering its set */
    /* NO FULL-SCREEN FLASH ON L/R (the box loop's own need_full/"NO BLACK FLASH"
     * convention, mirrored here). L/R and A/B are the only keys this loop reads --
     * there is no separate cursor -- so every L/R press is a genuine wallpaper
     * change, and draw_wallpaper()'s own full-rect repaint below is correct and
     * unavoidable (it resets s_under[]/s_wp_drawn every single call regardless, see
     * draw_wallpaper()'s §12b comment above -- this loop does not touch that
     * contract at all, it only decides what happens AROUND that call). What used to
     * also run on every press was ui_clear() -- a full 240x160 wipe of the margins
     * OUTSIDE the preview rect too, so those margins flashed to plain UI_BG on
     * every single L/R press (key_hit is rising-edge only, no key_repeat here, so
     * that is a per-tap flash, not a held-shoulder one). wallpaper_group_menu above
     * draws its OWN popup reaching into those same margins WITHOUT calling
     * ui_clear() (box_options_menu style), so re-entering this while loop from the
     * outer for(;;) is a place that can leave its stale pixels behind -- need_full
     * is a fresh local every time this loop is (re)entered, which catches exactly
     * that case (manual invalidation, since there is no ui_clear() to bump
     * ui_clear_gen() on that path).
     *
     * ONE MARGIN IS NOT LEFT ALONE, THOUGH: the caption panel is a fixed 140x13
     * fill (x 50..189), but the caption STRING inside it is not clipped/margined --
     * "%d/%d %s%s" can run to 18+ chars for a Friends-group name ("7/16
     * Pokecenter2 *"), and at x=56 with an 8px font that is ink out to x~200, past
     * the panel's own right edge at x=190. Pre-commit this self-healed every press
     * because ui_clear() repainted x=190..239 to UI_BG before ANY caption drew, so
     * a shorter caption on the next press could never show a longer caption's
     * leftover ink. Now that ui_clear() only runs on need_full, that self-heal is
     * gone for this one strip, so it is repainted by hand below, unconditionally,
     * every iteration -- the same "wipe the whole thing this could ever have
     * touched, then draw" contract row_paint() uses (pdna_edit.c), sized to this
     * caption's own worst-case overflow instead of a whole row. The caption panel
     * and footer hint otherwise stay unconditional, as before: the caption's text
     * always changes with wp, and the footer hint sits on y=152 == WP_Y+WP_H-1, the
     * SAME row draw_wallpaper's own rect reaches down to -- skipping it past
     * iteration 1 would let the wallpaper repaint clip the top scanline off every
     * glyph. */
    bool back = false;
    bool need_full = true;
    while (!back) {
      if (need_full) ui_clear();
      draw_wallpaper(wp, WP_X, WP_Y, WP_W, WP_H);
      /* the box's icons stay composited as OBJ sprites above this preview BG */
      char b[40]; siprintf(b, "%d/%d %s%s", wp - base + 1, glen, wp_name(wp), wp >= 16 ? " *" : "");   /* * = Walda secret */
      ui_panel(50, 0, 140, 13, UI_PANEL, UI_BORDER);
      ui_fill_rect(190, 0, UI_SCR_W - 190, 12, UI_BG);   /* caption overflow strip (panel ends
                                                          * at x=190); see the block above. Height
                                                          * 12, NOT the panel's 13: row 12 is the
                                                          * preview's top tile row (draw_wallpaper
                                                          * ran above us), and caption ink only
                                                          * reaches rows 2..9. */
      ui_text(56, 2, UI_TITLE, b);
      ui_text(2, 152, RGB15(31, 31, 31), "L/R pick  A set  B sets");
      need_full = false;
      u16 k; do { s_vsync(); k = key_hit(KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R | KEY_A | KEY_B); } while (!k);
      if (k & KEY_B) { snd_back(); back = true; }            /* back to the set menu, like the game */
      else if (k & KEY_A) { snd_ok(); return wp; }
      else if (k & (KEY_LEFT | KEY_L))  { snd_move(); wp = (wp > base) ? wp - 1 : base + glen - 1; }
      else if (k & (KEY_RIGHT | KEY_R)) { snd_move(); wp = (wp - base + 1) % glen + base; }
    }
  }
}

/* "Export all to .pk": write every occupied mon in `box` to a PKHeX .pk3 in the bank folder,
 * with a per-mon progress sprite + N/total bar (the slow batch the user asked to see). PC and
 * bank both; Omega-only (SD writes — the caller gates on can_edit). */
static void export_box_all(BoxSource* src, int box) {
  uint8_t* recs = src->records(box);
  PkMon list[G3_BOX_SLOTS];
  box_decode_to(src, recs, box, list, 0);
  /* BACKLOG #150 S150-3 D-Q8 (the thirteenth site): a native cell decodes to a non-zero
   * `species` stand-in (box_decode_to's native pass), so the plain `list[s].species`
   * test below would count it as an exportable Gen-3 mon and pdna_pk_export_silent
   * would write its RAW 80 bytes into a .pk3 verbatim -- not a Gen-3 record at all.
   * Skip and count; do NOT rely on the import side's pk3_validate to catch it later. */
  int total = 0, native_skip = 0;
  for (int s = 0; s < G3_BOX_SLOTS; s++) {
    if (bc_is_native(recs + (uint32_t)s * 80)) { native_skip++; continue; }
    if (list[s].species) total++;
  }
  if (total == 0) {
    snd_deny();
    ui_clear();
    ui_panel(20, 60, 200, 44, UI_PANEL, UI_BORDER);
    ui_text(30, 70, UI_WARN, native_skip ? "NOTHING TO EXPORT" : "BOX IS EMPTY");
    if (native_skip) { char l0[40]; siprintf(l0, "%d native skipped. Press A", native_skip); ui_text(30, 86, UI_DIM, l0); }
    else ui_text(30, 86, UI_DIM, "Nothing to export. Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    return;
  }
  boxoam_suspend();
  int done = 0, failed = 0;
  for (int s = 0; s < G3_BOX_SLOTS; s++) {
    if (bc_is_native(recs + (uint32_t)s * 80)) continue;
    if (!list[s].species) continue;
    pdna_progress_frame("EXPORT TO .pk", &list[s], done, total, "Writing...");
    if (pdna_pk_export_silent(recs + (uint32_t)s * 80, &list[s], 0, 0) != SF_OK) failed++;
    done++;
    for (int v = 0; v < 3; v++) s_vsync();
  }
  if (failed) snd_error(); else snd_save();
  ui_clear();
  ui_panel(20, 54, 200, 58, UI_PANEL, failed ? UI_WARN : UI_OK);
  char l[40]; siprintf(l, "EXPORTED %d / %d", total - failed, total);
  ui_text(30, 64, failed ? UI_WARN : UI_OK, l);
  ui_text(30, 82, UI_DIM, "Saved to /PokeDNA/bank/");
  if (native_skip) {
    char l2[32]; siprintf(l2, "%d native skipped", native_skip);
    ui_text(30, 94, UI_DIM, l2);
    ui_text(30, 108, UI_DIM, "Press A");
  } else {
    ui_text(30, 96, UI_DIM, "Press A");
  }
  u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
  boxoam_resume();
}

/* "Release all": permanently delete every mon in `box` (PC = removed from the save; bank = removed
 * from the bank box). Confirms first (destructive), then clears + commits via the box's verified-
 * write path (an immutable backup is taken before the .sav/box write). Omega-only. */
static void release_box_all(BoxSource* src, int box) {
  uint8_t* recs = src->records(box);
  PkMon list[G3_BOX_SLOTS];
  box_decode_to(src, recs, box, list, 0);
  int total = 0;
  for (int s = 0; s < G3_BOX_SLOTS; s++) if (list[s].species) total++;
  if (total == 0) {
    snd_deny();
    ui_clear();
    ui_panel(20, 60, 200, 44, UI_PANEL, UI_BORDER);
    ui_text(30, 70, UI_WARN, "BOX IS EMPTY");
    ui_text(30, 86, UI_DIM, "Nothing to release. Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    return;
  }
  char q[40]; siprintf(q, "Release all %d Pokemon?", total);
  if (!app_confirm(q, "Deleted permanently!")) { snd_back(); return; }
  static uint8_t EWRAM_BSS bak[G3_BOX_SLOTS][80];     /* pre-clear snapshot: a failed commit must
                                                       * leave RAM matching the card ("Nothing changed.") */
  memcpy(bak, recs, sizeof bak);
  for (int s = 0; s < G3_BOX_SLOTS; s++) if (list[s].species) memset(recs + (uint32_t)s * 80, 0, 80);
  bool ok = src->commit();                            /* PC: verified g_pc write (+ backup); bank: box file */
  if (!ok) memcpy(recs, bak, sizeof bak);             /* revert: EZ writes have no retry — never leave the
                                                       * zeroed box live for a later save to persist */
  if (ok) snd_save(); else snd_error();
  ui_clear();
  ui_panel(20, 54, 200, 56, UI_PANEL, ok ? UI_OK : UI_WARN);
  ui_text(30, 64, ok ? UI_OK : UI_WARN, ok ? "RELEASED" : "RELEASE FAILED");
  if (ok) { char l[40]; siprintf(l, "Freed %d slots.", total); ui_text(30, 82, UI_DIM, l); }
  else ui_text(30, 82, UI_DIM, "Nothing changed.");
  ui_text(30, 96, UI_DIM, "Press A");
  u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
}

/* Overlay menu when the box TITLE is selected: rename / wallpaper / export all / release all. Each
 * action mutates the source and commits via its verified-write path. */
static void box_options_menu(BoxSource* src, int box) {
  static const char* const OPT[5] = { "Rename box", "Wallpaper", "Export all .pk", "Release all", "Cancel" };
  /* #154: the Rename row used to draw+act unconditionally and refuse on A (D2,
   * BACKLOG #93) -- that is the shown-then-refused pattern the repo's own
   * convention is to avoid (the omitted-row convention every other capability
   * gate here follows: can_boxops/can_edit/can_lift all hide, never refuse-after).
   * A source with no box-name table at all (Gen 1: gbbn_supported()==false, so
   * gbsrc_can_rename_impl() returns false) has nothing legitimate to do with this
   * row, so it is left out of the menu entirely. can_rename() takes no box
   * argument -- it gates the whole session, not a specific box (BoxSource.can_rename's
   * own header note) -- so this can be decided once, before the loop, and cannot
   * change while the menu is open. Same condition the old refusal used, so PC/Bank
   * (can_rename == NULL -> src_can_lift fallback) keep showing it, byte-identical. */
  bool show_rename = src->can_rename ? src->can_rename() : src_can_lift(src, box, -1);
  int action[5]; int n = 0;
  if (show_rename) action[n++] = 0;
  action[n++] = 1; action[n++] = 2; action[n++] = 3; action[n++] = 4;
  int sel = 0;
  for (;;) {
    const int mx = 50, my = 42, mw = 140, mh = 18 + n * 14 + 11;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "BOX");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < n; i++) {
      int y = my + 18 + i * 14; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, OPT[action[i]]);
    }
    ui_text(mx + 6, my + mh - 9, UI_DIM, "A pick B back");
    u16 k; do { s_vsync(); k = key_hit(KEY_UP | KEY_DOWN | KEY_A | KEY_B); } while (!k);
    if (k & KEY_B) { snd_back(); return; }
    else if (k & KEY_UP)   { snd_move(); sel = (sel > 0) ? sel - 1 : n - 1; }
    else if (k & KEY_DOWN) { snd_move(); sel = (sel + 1) % n; }
    else if (k & KEY_A) {
      snd_ok();
      int a = action[sel];
      if (a == 0) {                                 /* rename */
        /* Defense-in-depth only now that show_rename gates the row itself above:
         * this branch is unreachable when the check would fail (a == 0 only appears
         * in `action[]` when show_rename was true, and can_rename() is session-wide
         * so it cannot flip mid-menu) -- kept so a future caller of this branch
         * cannot silently skip the gate the way the pre-D2 code did. */
        if (!(src->can_rename ? src->can_rename() : src_can_lift(src, box, -1))) {
          snd_deny();
          msg_wait("NO BOX NAMES", UI_WARN, "This game has no box names.", 0);
          return;
        }
        /* F1b: seed with the RAW stored name, not get_name()'s display string --
         * get_name() prefixes "GB " only for Gen-1's synthesized names; Gen-2 names
         * are echoed as-is (BACKLOG #122). The raw name must never be typed back
         * with a prefix into the save. */
        char cur[12];
        if (src->get_raw_name) src->get_raw_name(box, cur); else src->get_name(box, cur);
        char buf[12];
        if (osk_input("BOX NAME", cur[0] ? cur : "BOX", buf, 9)) {
          src->set_name(box, buf);
          src->commit();
        }
        return;
      } else if (a == 1) {                          /* wallpaper */
        /* D3 (review-opus, BACKLOG #93): gbsrc_set_wp is a documented no-op (GB
         * boxes have no wallpaper byte), so wallpaper_pick's own choice used to
         * vanish silently on a GB box (now reachable via can_boxops). `can_boxops
         * != NULL` alone is the simplest test that is true EXACTLY for the GB
         * source: no BoxSource field for "has wallpaper" exists (and adding one
         * would shift every hand-verified offset in tools/stack_edges.txt for no
         * real gain, same reasoning can_rename's own comment gives) -- can_boxops
         * is set ONLY by pdna_gen12_source() today (grepped), so this reads
         * exactly as "the GB source", not a coincidence. PC/Bank (can_boxops NULL)
         * are unaffected -- byte-identical to before this check existed. */
        if (src->can_boxops) {
          snd_deny();
          msg_wait("NO WALLPAPER", UI_WARN, "Game Boy boxes have no",
                   "wallpaper to change.");
          return;
        }
        int wp = wallpaper_pick(src, src->get_wp(box));
        if (wp >= 0) {
          if (src->is_bank || wp < G3_BOX_WALLPAPER_FRIENDS) {  /* standard wallpaper */
            src->set_wp(box, wp);
            src->commit();
          } else {                                        /* Emerald Walda secret wallpaper (PC) */
            src->set_wp(box, G3_BOX_WALLPAPER_FRIENDS);
            app_set_walda((uint8_t)(wp - G3_BOX_WALLPAPER_FRIENDS));
            if (src->commit()) app_commit_sb1();          /* box byte + the Walda config */
          }
        }
        return;
      } else if (a == 2) {                          /* export all to .pk */
        if (src->export_all) src->export_all(box); else export_box_all(src, box);
        return;
      } else if (a == 3) {                          /* release all (destructive; confirms) */
        if (src->release_all) src->release_all(box); else release_box_all(src, box);
        return;
      } else return;                               /* cancel */
    }
  }
}

/* Idle icon bob: every ANIM_PERIOD frames toggle a 1px unison Y-offset on ALL icon
 * sprites in a single in-vblank OAM write (boxoam_set_bob). Every icon moves together,
 * the GPU composites the rest, ~0 CPU, and the cursor never blocks — so this is free and
 * flicker-free (the hardware-OBJ path, now that the BG2-priority bug is fixed). Gated on
 * app_anim_enabled(); suspended while move-carrying and in ITEM mode (markers stay put). */
#define ANIM_PERIOD 30                    /* vblanks per bob toggle (~0.5s, Gen-3 cadence) */

/* ---------------------------------------------------------------------------------
 * PC-box party PANEL — the retail PARTY POKEMON panel, REBUILT 2026-08-20 against
 * native-E12e-storage-partystrip.top.png, the ONLY unobstructed retail frame
 * (docs/analysis-2026-08-20-pcparty/MEASUREMENTS.md "PANE MEASUREMENTS"). Replaces
 * app_party_overlay's full-screen field-menu list at THIS screen's two PARTY-tab call
 * sites only (app_party_overlay itself is unchanged and still serves the standalone
 * NV_PARTY nav-menu screen — see its own doc comment, pdna_app.h).
 *
 * Retail's shape (see pdna_layout.h's PDNA_PCP_* block for the full derivation): ONE
 * big framed teal-dithered panel holding SIX party tiles — slot 1 (party index 0)
 * alone and offset left, slots 2-6 (party indices 1-5) stacked in a right-hand column
 * at 24 px pitch — with a CANCEL pill snug in the panel's own bottom-right corner. A
 * party never exceeds 6, so these SIX PHYSICAL POSITIONS map 1:1 to party index and
 * never scroll: idx 0 -> the offset tile, idx 1..5 -> column rows 0..4. Every one of
 * the 6 positions is drawn every frame (pcp_draw_slot), occupied or not — an unfilled
 * position (party size < 6 in BROWSE mode, or the PLACE "+ add" target) is just an
 * empty bordered tile, matching the box grid's own empty-cell rule (nothing else
 * drawn; MEASUREMENTS.md's slot-1/column section never captures an empty tile to
 * measure directly, so this is the same judgement call the box grid already makes).
 *
 * The box grid + banner + PKMN DATA panel stay visible and alive behind the panel:
 * this function never calls ui_clear(); it repaints the box BG via
 * render_full(...,clear=false) every iteration (undoing whatever a full-screen
 * sub-view — the summary, legality, etc, reached through app_party_mon_menu — painted
 * over it) and then draws the panel's own bevel/dither chrome + the six tiles + the
 * reused PKMN DATA panel (draw_left, this file) on top, now showing the panel's own
 * focused party mon rather than the box's last cursor cell.
 *
 * Box-grid MON ICONS specifically do NOT go away while the panel is open, except the
 * grid cells the panel's own on-screen rectangle physically sits over — see
 * boxoam_strip_open/slot/slot1/close in box_oam.c, and PDNA_PCP_OCCLUDE_X0/X1
 * (pdna_layout.h) for the panel's full width now driving that occlusion, not just the
 * old narrow column. An earlier version called boxoam_suspend() around the whole
 * popup (same convention every other popup in this file uses), which flips DCNT_OBJ
 * off — and every box icon, the cursor and any carried mon are OBJ, so that blanked
 * the ENTIRE 30-icon grid, not just the panel's own footprint. This is a judgement
 * call either way, not a retail fact — retail hides its own grid ENTIRELY here — but
 * Guy's own request for this feature ("a side menu of the party pops up ontop of the
 * pokemon in the PC in the background") specifically wants the background grid
 * intact, so the fix keeps everything OUTSIDE the panel's own rectangle visible
 * (including a carried mon still riding the cursor, mid-drag into the party) rather
 * than blanking the grid to reuse the old, simpler suspend/resume call. A full-screen
 * sub-view reached from inside the panel (the read-only dialog, the party action
 * menu) still gets its OWN boxoam_suspend()/boxoam_resume() bracket at its own call
 * site below — that pair is a pure DISPCNT toggle and never disturbs the panel's own
 * hidden/reused slots, so nesting it here is safe.
 *
 * CANCEL is a SEVENTH selectable stop (index == rows, past every party/add tile):
 * DOWN from the last tile reaches it, A closes exactly like B. No retail frame shows
 * this panel's own cursor navigating to CANCEL (MEASUREMENTS.md's own gap), so
 * treating it as reachable-by-cursor rather than B-only is a judgement call, made so
 * the button is not just decoration once it exists on screen.
 *
 * CHANGE 2 (2026-08-20, "the glove/cursor [must] go to the pc box while grabbing,
 * just like the MOVE option in the game"): the popup's own cursor now has TWO foci,
 * mirroring retail's "the cursor moves freely between the party panel and the box
 * grid" (MEASUREMENTS.md has no frame of this — no capture shows the panel's cursor
 * off the panel — so the exact mapping is this build's own judgement call, chosen to
 * reuse the geometry that already exists): PANEL (the six tiles + CANCEL, as before)
 * and GRID (the box's own cursor, over the 2 grid columns the panel does NOT cover —
 * columns 4-5 at this build's GRID_X/CELL_W/COLS, see box_oam.c's boxoam_strip_open
 * comment for why only those 2 survive the panel's own occlusion). RIGHT from the
 * party COLUMN (not the offset tile — Guy's own call, "RIGHT from the party column
 * into the grid", picked because the column already sits closer to the grid on
 * screen) crosses into GRID at the near column, same row; LEFT from the grid's near
 * column crosses back to PANEL, same row (col_sel tracks "last column row" across
 * BOTH directions so a RIGHT then LEFT is a no-op on the cursor's position). UP/DOWN/
 * LEFT/RIGHT clamp (not wrap) at the grid's own edges while in GRID focus — there is
 * no top-tabs/Bank-switch/box-switch escape from inside this popup, so clamping is
 * the safe, simple choice (a future version could wire those in; not attempted here).
 * A grid cell can never render inside the panel's own rectangle by construction
 * (columns 4-5 sit entirely to the panel's right, with margin — see box_oam.c).
 *
 * While focus is PANEL, the box's own cursor hand / carried mon stay hidden exactly
 * as before (boxoam_strip_hide_cursor). While focus is GRID, they render normally —
 * oam_sync (already shared with the outer pdna_box loop) draws them at the grid
 * cursor, including a mon riding the glove, for free.
 *
 * GRID-focus A reuses the SAME mutation primitives the outer loop trusts (start_carry
 * / drop_held / app_mon_menu / play_grab_anim / play_place_anim_down/up) rather than
 * re-deriving them, and reads/writes the SAME file-scope s_holding/s_held/s_orig_*
 * this whole file already shares with the outer loop — so does the PANEL's own PLACE
 * branch, below (it used to take a `held`/`orig_*` snapshot as a parameter; now it
 * reads the live globals directly, which is what makes a mon grabbed mid-session from
 * the GRID immediately placeable from the PANEL without reopening anything):
 *   - s_holding true, A on a grid cell: PLACE (drop_held) -- an empty cell or a
 *     within-scope swap. Closes the popup (goto out, result 1) only once the hand is
 *     genuinely empty (*done); a SWAP keeps the popup open, now holding the displaced
 *     occupant, exactly like the outer loop's own swap handling.
 *   - !s_holding, A on an occupied grid cell, s_cur_mode == CM_MOVE (2026-08-23): a
 *     DIRECT grab -- start_carry + the grab beat, no menu -- exactly what this same
 *     cell does one screen over, outside the popup (pdna_box()'s own CM_MOVE A-
 *     handler / begin_select()'s no-drag tap). Before this, GRID focus never read
 *     s_cur_mode at all, so the grid's own fast "MOVE mode" idiom silently reverted
 *     to the slow menu-first grab the instant the cursor crossed into the panel --
 *     the same gesture, same cell, a DIFFERENT grammar depending on which side of
 *     the RIGHT/LEFT crossing you made it from. Guy's own words this fixes: "i cant
 *     intuitively grab pokemon from party to pc and back like in the game."
 *   - !s_holding, A on an occupied grid cell, any OTHER cursor mode: GRAB -- opens
 *     the box's own action menu; MOVE or DUPLICATE starts a carry (start_carry / the
 *     dup-copy path) and keeps the popup OPEN so the freshly-grabbed mon can be
 *     walked back to the panel and placed into a party slot without ever leaving
 *     this screen. B still closes the popup unconditionally (matching how B already
 *     worked for a mon carried in from OUTSIDE this popup) — the carry survives
 *     exactly as it always has, because closing on B never discards s_holding/s_held.
 *
 * Round trips (both proven — docs/analysis-2026-08-20-pcparty's own report has the
 * capture sequence): grab a party mon (top tabs, as before) -> RIGHT into GRID ->
 * place onto a box cell (closes); and grab a box mon from GRID focus (MOVE) -> LEFT
 * into PANEL -> place onto a party tile (closes). RULE 3 (never lose/duplicate a
 * carried mon, never empty the party) holds throughout because every mutation still
 * runs through the SAME primitives the outer loop already relies on for that
 * guarantee — this popup adds new PATHS to them, not new COPIES of their logic.
 *
 * Same PLACE/GRAB contract as app_party_overlay (see pdna_app.h) for the RETURN
 * VALUE: PLACE (a mon was already held on entry) drops/swaps a carried box mon into
 * the party on A, returning 1; GRAB (empty-handed on entry) opens the full action
 * menu on A, returning 2 (+ grab80 / grab_slot filled) iff MOVE TO BOX was chosen.
 * 0 = closed (B, or CANCEL) — possibly still holding something (a mon grabbed via
 * GRID focus, or one that was already held on entry and never got placed); the
 * caller's own s_holding check (unconditional, same as any other carry) picks that
 * up on the very next iteration regardless of which entry point this was. */

/* Physical bbox (inclusive) for party index `idx` (0 = offset slot 1, 1..5 = column
 * rows 0..4, top-to-bottom = party index 1..5). Pure geometry, no drawing. */
static void pcp_pos(int idx, int* x0, int* y0, int* x1, int* y1) {
  if (idx == 0) {
    *x0 = PDNA_PCP_S1_X0; *x1 = PDNA_PCP_S1_X1;
    *y0 = PDNA_PCP_S1_Y0; *y1 = PDNA_PCP_S1_Y1;
  } else {
    int row = idx - 1;
    *x0 = PDNA_PCP_COL_X0; *x1 = PDNA_PCP_COL_X1;
    *y0 = PDNA_PCP_COL_Y0 + row * PDNA_PCP_SLOT_H;
    *y1 = *y0 + PDNA_PCP_SLOT_VISH - 1;
  }
}

/* MUST-FIX 1 (2026-08-20 review): retail's 3-tone bevelled BLUE PLATE — see UI_PCP_TILE_*'s
 * own comment (source/ui.h) for how the colours were re-measured and why the old
 * "no fill, the dither shows through" reading was wrong. Layout + exact colours are a
 * pixel-for-pixel transcription of native-E12e-storage-partystrip.top.png (the mode
 * across all 6 real tiles, at every position, with sprite pixels excluded by
 * majority vote): a light-blue top row + left column, a mid-blue main fill switching
 * to light-blue for the lower two-thirds, a mid-blue bottom row, and a darker-blue
 * right column. Draw order matters at the two seams where a row and a column claim
 * the same pixel (mirrors pcp_draw_panel's own "the band drawn LAST wins the corner"
 * rule, right below this function): the right column is drawn LAST so it wins both of
 * its own two corners (measured: mid-blue at its own top cell, shadow-blue below
 * that, all the way to the bottom row); the left column is drawn after the fill so it
 * wins its own top/mid cells but purposely does NOT extend to the bottom row
 * (measured: that corner belongs to the row's own colour, not the column's).
 * (x0,y0)-(x1,y1) is the tile's OUTER bordered box (pcp_pos's own convention);
 * every party tile this panel draws is a fixed 30x23, so the offsets below are literal
 * pixel counts from y0, not proportional math. */
static void pcp_fill_plate(int x0, int y0, int x1, int y1) {
  (void)y1;                                     /* every tile is 23 px tall (y1 == y0+22);
                                                 * offsets below are measured from y0    */
  m3_rect(x0 + 1, y0 + 2,  x1,     y0 + 12, UI_PCP_TILE_MID);   /* rows y0+2..y0+11  */
  m3_rect(x0 + 1, y0 + 12, x1,     y0 + 21, UI_PCP_TILE_HI);    /* rows y0+12..y0+20 */
  m3_line(x0 + 1, y0 + 1,  x1 - 1, y0 + 1,  UI_PCP_TILE_HI);    /* row y0+1          */
  m3_line(x0 + 1, y0 + 21, x1 - 1, y0 + 21, UI_PCP_TILE_MID);   /* row y0+21         */
  m3_plot(x1 - 1, y0 + 1,  UI_PCP_TILE_MID);                    /* right col, top cell */
  m3_line(x1 - 1, y0 + 2,  x1 - 1, y0 + 21, UI_PCP_TILE_SHADOW);/* right col, rest   */
  m3_line(x0 + 1, y0 + 1,  x0 + 1, y0 + 20, UI_PCP_TILE_HI);    /* left col (not the
                                                                 * bottom row — measured) */
}

/* The panel's six party rows, declared to the icon store before the tile loop draws
 * them. NO BORROW HERE, deliberately and permanently: this popup lives inside the box
 * screen, whose whole reason for existing is to display g_pc -- the very buffer Tier B
 * rents. So the panel gets the plan's transaction saving (one sorted, merged sweep per
 * pool-sized group instead of six cold single-row fetches per repaint) and not its
 * residency; six rows fit the cache rung's pool anyway, and on the ROM rung's four they
 * honestly do not, which is what icon_store_plan_resident() will keep saying.
 *
 * ARTLESS ONLY, for dex_declare_page's reason (pdna_pick.c). */
static void pcp_declare_party(const PkMon* pm, int n) {
#if PDNA_MON_ICONS_ART_COMPILED
  (void)pm; (void)n;
#else
  uint16_t rows[6];
  int k = 0;
  for (int i = 0; i < n && i < 6; i++) {
    bool egg = pm[i].isEgg && !pm[i].isBadEgg;
    if (!egg && !pm[i].species) continue;
    rows[k++] = art_icons_row_for(egg ? 412 : pm[i].species, egg ? 0 : pm[i].form);
  }
  icon_store_plan(rows, k);
#endif
}

static void pcp_draw_slot(int idx, const PkMon* p, bool addslot, bool selected) {
  int x0, y0, x1, y1; pcp_pos(idx, &x0, &y0, &x1, &y1);
  /* m3_frame's (right,bottom) are EXCLUSIVE — see ui_progress's own comment in ui.c
   * for the measured proof — hence the +1s. */
  u16 border = selected ? UI_PCP_CURSOR : UI_PCP_TILE_BORDER;
  m3_frame(x0, y0, x1 + 1, y1 + 1, border);
  pcp_fill_plate(x0, y0, x1, y1);
  /* The borrowed OAM slot (box_oam.c) never DISPLAYS this tile's icon any more (see
   * boxoam_icon_blit_clip's own comment, MUST-FIX 5) — always hide it here so a stale
   * sprite from an earlier iteration/species can never show through the plate. */
  if (idx == 0) boxoam_strip_slot1(0, 0, 0, 0, false);
  else          boxoam_strip_slot(idx - 1, 0, 0, 0, 0, false);
  if (addslot || !p || p->species == 0) return;    /* empty target/unfilled position: the
                                                     * plate alone, nothing else drawn (see
                                                     * this function group's own top-of-
                                                     * block comment). */
  bool egg = p->isEgg && !p->isBadEgg;
  /* CHANGE 1 (2026-08-20, "make sure the pokemon are not trimmed" -- see
   * PDNA_PCP_ICON_DX/DY's own comment, pdna_layout.h, for the measurement this
   * replaces MUST-FIX 5's centring formula with): retail's real, fixed icon anchor,
   * not a centred crop -- confirmed constant across all 6 tiles. The 32x32 canvas
   * still can't fit a 30x23 tile, so something still has to give, but retail's OWN
   * answer is "let it": the top overflow (8 px) is real geometry, and retail lets
   * EVERY tile have the full allowance, interior column rows included -- there is
   * no "collide with the neighbour" special case in retail's own rendering.
   *
   * CHANGE 1 FOLLOW-UP (2026-08-20, same day): the code above used to give only the
   * offset slot and column row 0 (idx 0/1) the full 8 px, and clamp column rows 1-4
   * to whatever gap sat between them and the tile above -- reasoning that every
   * species tested had "zero ink up there anyway" so the clamp was a no-op safety
   * net. That reasoning does not hold in general (a tall enough species' art DOES
   * reach the top of its 32x32 canvas -- Milotic's crest was the case that exposed
   * it), so EVERY tile now gets the identical treatment: full overflow, bounded
   * only by the panel's own top edge, never by a neighbour.
   *
   * Z-ORDER, measured directly off native-E12e-storage-partystrip.top.png
   * (docs/analysis-2026-08-20-pcparty) at the one boundary in that capture where a
   * tile's overflow reaches into the tile above (row 3/row 4, Dragonite/Milotic):
   * the LOWER tile's overflow paints ON TOP of the tile above, not the other way
   * around -- retail draws its strip top-to-bottom and lets each later tile win the
   * shared pixels. This loop already draws idx 0..5 in that same order, plate and
   * icon together per idx, so idx-1's plate+icon are already fully on-screen by the
   * time idx's icon (with its own overflow) is blitted -- the existing draw order
   * reproduces retail's z-order for free; nothing needed restructuring into a
   * separate plates-first pass.
   *
   * CAVEAT (found while re-verifying this against real gameplay, not just the
   * spec that prompted it): for the six-species roster in this project's own test
   * capture (Salamence/Metagross/Gengar/Dragonite/Milotic, against PokeDNA's own
   * compiled data/mon_icons_oam_tiles.bin), NONE of the five column-row icons —
   * Milotic included — carry any opaque pixel in the 8 rows above their own tile
   * (verified by decoding the exact compiled tile data these five species resolve
   * to, and separately confirmed on real hardware-equivalent render output by
   * painting a marker over that exact 32x8 px strip immediately before the icon
   * blit and finding it undisturbed). So on today's evidence this change is a
   * pixel-for-pixel no-op for every tile in the verified capture -- it does not,
   * by itself, restore Milotic's crest (that gap sits lower, INSIDE the tile's own
   * bounds, not in the clamped overflow band; the true anchor DX=-1/DY=-8 was
   * re-confirmed as the best fit by an exhaustive offset search, so it is not a
   * placement bug either -- see this fix's own commit message). It is kept anyway
   * because it is the geometrically correct, retail-matching rule and the one this
   * spec asked for, and because a taller-posed species (not in today's roster)
   * would otherwise still lose its own overflow to the old defensive clamp. Left/
   * right/bottom overflow is unchanged (1 px each, matching retail). Deliberately
   * NOT calling party_draw_name_level or party_draw_hp_fields (pdna_main.c) here;
   * that per-mon detail lives ONLY in the reused PKMN DATA panel (draw_left,
   * below), matching retail's own division of labour. */
  int ix = x0 + PDNA_PCP_ICON_DX;
  int iy = y0 + PDNA_PCP_ICON_DY;
  int cx0 = x0 + PDNA_PCP_ICON_DX, cx1 = x1 + 2;   /* 1px overflow L/R, matching retail */
  int cy1 = y1 + 2;                                /* 1px overflow bottom, matching retail */
  int cy0 = y0 + PDNA_PCP_ICON_DY;                 /* up to 8px overflow above, EVERY tile; */
  if (cy0 < PDNA_PCP_FILL_Y0) cy0 = PDNA_PCP_FILL_Y0;  /* ...bounded only by the panel's own
                                                         * top edge, never by a neighbour. */
  /* PARITY-AUDIT-2026-09 #75: the chip is a PER-CELL fallback, keyed to the actual
   * icon draw's own result (boxoam_icon_blit_clip already returns 0/1 -- no new
   * per-cell state needed), not the session-wide boxoam_icons_available() gate this
   * used to short-circuit on. Same fallback convention as the box grid's own
   * artless_cells(): an original name chip so a party member stays IDENTIFIABLE
   * whenever this particular slot's icon genuinely failed to draw (artless with no
   * icon source at all, an SD hiccup, a species the store can't serve) — never for a
   * slot whose icon DID draw, session-wide art availability aside. */
  if (!boxoam_icon_blit_clip(ix, iy, cx0, cy0, cx1, cy1, p->species, p->form, egg)) {
    int cw = (x1 - x0 + 1) - 2, ch = 12, cy = y0 + 5;
    if (egg) ui_name_chip(x0 + 1, cy, cw, ch, 0x2A7A, 0x0000, "EGG");
    else     ui_name_chip(x0 + 1, cy, cw, ch, UI_PANEL, UI_TEXT, pk_species_name(p->species));
  }
}

/* CANCEL pill: retail's flat/dithered chrome vocabulary (pale-blue body, WHITE glyph
 * + green drop shadow — see UI_PCP_CANCEL_*'s own comment, source/ui.h, for the
 * 2026-08-20 role-swap fix), not faked sprite art. The tight-kerned "CANCEL" measures
 * exactly PDNA_PCP_CANCEL_W_BUDGET px (pinned in tests/host_textfit_test.c) so it
 * fills the 1px-bordered interior with no clamp/ellipsis needed.
 *
 * SHOULD-FIX 4 (2026-08-20 review): retail's body IS its own border (the histogram
 * found no separate 3rd blue shade), so the unselected border is drawn in the SAME
 * body colour — a plain pale-blue pill, only the cursor-highlighted state gets a
 * visually distinct border. Corners are cut 1px (m3_plot, restoring the panel's own
 * teal dither at that exact pixel) — the only "rounding" this codebase's Mode-3
 * primitives allow (there is no arc/rounded-rect primitive to reach for). */
static void pcp_draw_cancel(bool selected) {
  int x0 = PDNA_PCP_CANCEL_X0, y0 = PDNA_PCP_CANCEL_Y0;
  int x1 = PDNA_PCP_CANCEL_X1, y1 = PDNA_PCP_CANCEL_Y1;
  m3_rect(x0, y0, x1 + 1, y1 + 1, UI_PCP_CANCEL_BODY);
  m3_frame(x0, y0, x1 + 1, y1 + 1, selected ? UI_PCP_CURSOR : UI_PCP_CANCEL_BODY);
  m3_plot(x0, y0, (y0 & 1) ? UI_PCP_FILL_B : UI_PCP_FILL_A);
  m3_plot(x1, y0, (y0 & 1) ? UI_PCP_FILL_B : UI_PCP_FILL_A);
  m3_plot(x0, y1, (y1 & 1) ? UI_PCP_FILL_B : UI_PCP_FILL_A);
  m3_plot(x1, y1, (y1 & 1) ? UI_PCP_FILL_B : UI_PCP_FILL_A);
  ui_ptext_fit_shadow_tight(x0 + 1, y0 + 2, PDNA_PCP_CANCEL_W_BUDGET,
                            UI_PCP_CANCEL_GLYPH, UI_PCP_CANCEL_SHADOW, PDNA_LBL_CANCEL);
}

/* The panel's own outer rectangle (PDNA_PCP_OCCLUDE_X0..X1) covers 4 of the box
 * grid's 6 columns at this build's own GRID_X/CELL_W pitch, not a clean multiple of
 * either — so boxoam_strip_open() (which must hide a WHOLE grid icon; an OBJ sprite
 * can't be clipped) ends up hiding a sliver of a column that only PARTIALLY falls
 * under the panel too. Left alone, that reads as the same "content vanished for no
 * reason" defect this whole fix exists to remove. Repainting the hidden columns' TRUE
 * occupants at their normal grid position fixes it: this call runs BEFORE the panel's
 * own chrome draws, so the part that actually falls under the panel (x0..x1) gets
 * painted over a moment later — leaving exactly the kind of partially-visible column
 * MEASUREMENTS.md's retail read shows (2 columns fully clear + a sliver of a 3rd) —
 * and the part outside it stands untouched.
 *
 * boxoam_slot_blit_bitmap (box_oam.c), not the separate mon_icons.h bitmap table: an
 * EARLIER version of this used mon_icon_for_form_frame + ui_sprite here and it looked
 * right but measured wrong — a direct capture diff showed hundreds of small per-pixel
 * colour deltas versus the OAM grid's own rendering, because the two icon tables are
 * independently generated (different palette reduction) and were never guaranteed to
 * agree byte for byte. Sourcing the blit from the SAME tile bytes + live OBJ palette
 * the sprite itself uses removes that whole class of mismatch, frame/pose included. */
static void strip_restore_icons(int x0, int x1) {
  for (int s = 0; s < COLS * ROWS; s++) {
    int cx0 = GRID_X + (s % COLS) * CELL_W, cx1 = cx0 + CELL_W;
    if (cx1 <= x0 || cx0 >= x1) continue;               /* this cell's column misses the panel */
    /* ALSO-WORTH-DOING 9 (2026-08-20 review): the icon this restores is a 32px
     * sprite (MON_ICON_W), wider than the 24px cell pitch — the same bleed
     * MUST-FIX 5's own comment documents for the box grid. When that whole 32px
     * span already lies inside [x0,x1), pcp_draw_panel repaints over every pixel
     * this blit would draw a moment later: skip the 1024-pixel Mode-3 write +
     * verified re-read entirely (measured: 15 of the 20 occluded cells are fully
     * covered — only the column straddling the panel's right edge needs restoring). */
    if (cx0 >= x0 && cx0 + MON_ICON_W <= x1) continue;
    if (s_era_drawn & (1u << s)) continue;               /* its own Gen-1/2 bitmap art already stands */
    boxoam_slot_blit_bitmap(s);                          /* no-op for an empty/art-less slot */
  }
}

/* The panel's own bevel border + interior teal dither (MEASUREMENTS.md "PANEL OUTER
 * BBOX"/"INTERIOR FILL"): outer bbox x=[82,175] full native height y=[0,159], no top
 * border (fill is flush with y=0). A classic top-left-highlight / bottom-right-shadow
 * bevel — left's inner band is a lighter HIGHLIGHT, right/bottom's inner bands are a
 * darker SHADOW, as-measured (not forced to match each other). The bottom band is
 * drawn LAST, across the panel's FULL outer width, so it wins the two bottom corners
 * where it overlaps the left/right bands (unmeasured corner pixels; this is the only
 * self-consistent draw order). m3_rect's (x1,y1) are EXCLUSIVE, hence the +1s (see
 * ui_progress's own comment in ui.c for the measured proof this project goes by). */
static void pcp_draw_panel(void) {
  for (int y = PDNA_PCP_FILL_Y0; y <= PDNA_PCP_FILL_Y1; y++)
    m3_line(PDNA_PCP_FILL_X0, y, PDNA_PCP_FILL_X1, y,
            (y & 1) ? UI_PCP_FILL_B : UI_PCP_FILL_A);
  /* left: outer(1px, WIDENED leftward to PANEL_W) / mid(2px) / inner HIGHLIGHT(3px)
   *
   * ALSO-WORTH-DOING 7 (2026-08-20 review): the left PKMN DATA panel (draw_left)
   * only reaches x=PANEL_W-1, and the box grid's own content starts at GRID_X
   * (==PDNA_PCP_LB_OUTER_X, both 82) — leaving a few columns of stray box wallpaper
   * visible in the seam between them for the panel's full height. The outer band
   * below normally runs PDNA_PCP_LB_OUTER_X..+1; starting it at PANEL_W instead
   * closes that seam for free (same colour, one wider rect) without touching
   * PDNA_PCP_LB_OUTER_X itself, PDNA_PCP_OCCLUDE_X0, or any pinned
   * host_textfit_test.c check that reads them. */
  m3_rect(PANEL_W, 0, PDNA_PCP_LB_OUTER_X + 1, PDNA_PCP_FILL_Y1 + 1, UI_PCP_PANEL_OUTER);
  m3_rect(PDNA_PCP_LB_MID_X0,  0, PDNA_PCP_LB_MID_X1 + 1,  PDNA_PCP_FILL_Y1 + 1, UI_PCP_PANEL_MID);
  m3_rect(PDNA_PCP_LB_HI_X0,   0, PDNA_PCP_LB_HI_X1 + 1,   PDNA_PCP_FILL_Y1 + 1, UI_PCP_PANEL_HILITE);
  /* right: inner SHADOW(3px) / mid(2px) / outer(1px). BACKLOG #243: draw_box_banner's
   * own rect (called from render_full, a moment before this function every frame of
   * party_strip_overlay's loop) occupies y=13..26 across the WHOLE box width
   * (WP_X+2==80 .. +158==238) -- including the box-occupancy readout ("28/30") that
   * sits past this panel's own right edge, close enough to PDNA_PCP_RB_OUTER_X==175
   * that its leading digit's leftmost ink column lands ON that column. Starting the
   * right border's rects at PDNA_PCP_BANNER_Y1+1 instead of 0 leaves the banner row
   * for THIS panel's right edge alone (its own 1px bottom shadow line at y==26 stays
   * the visual boundary there, same as everywhere else the banner sits on top of the
   * box BG) -- the left border and the interior fill are UNCHANGED (still full height,
   * still hide the box NAME under the panel the way "stay alive behind the panel"
   * describes; only the occupancy text peeking out past this panel's right edge was
   * ever meant to read intact). */
  m3_rect(PDNA_PCP_RB_SH_X0,  PDNA_PCP_BANNER_Y1 + 1, PDNA_PCP_RB_SH_X1 + 1,  PDNA_PCP_FILL_Y1 + 1, UI_PCP_PANEL_SHADOW);
  m3_rect(PDNA_PCP_RB_MID_X0, PDNA_PCP_BANNER_Y1 + 1, PDNA_PCP_RB_MID_X1 + 1, PDNA_PCP_FILL_Y1 + 1, UI_PCP_PANEL_MID);
  m3_rect(PDNA_PCP_RB_OUTER_X, PDNA_PCP_BANNER_Y1 + 1, PDNA_PCP_RB_OUTER_X + 1, PDNA_PCP_FILL_Y1 + 1, UI_PCP_PANEL_OUTER);
  /* bottom (drawn last, full width): inner SHADOW(3px) / mid(3px) / outer(2px) */
  m3_rect(PDNA_PCP_PANEL_X0, PDNA_PCP_BB_SH_Y0,    PDNA_PCP_PANEL_X1 + 1, PDNA_PCP_BB_SH_Y1 + 1,    UI_PCP_PANEL_SHADOW);
  m3_rect(PDNA_PCP_PANEL_X0, PDNA_PCP_BB_MID_Y0,   PDNA_PCP_PANEL_X1 + 1, PDNA_PCP_BB_MID_Y1 + 1,   UI_PCP_PANEL_MID);
  m3_rect(PDNA_PCP_PANEL_X0, PDNA_PCP_BB_OUTER_Y0, PDNA_PCP_PANEL_X1 + 1, PDNA_PCP_BB_OUTER_Y1 + 1, UI_PCP_PANEL_OUTER);
}

static int party_strip_overlay(BoxSource* src, int box, int* cur,
                               uint8_t grab80[80], int* grab_slot, bool allow_move_to_box) {
  if (!app_can_edit()) {
    snd_deny();
    boxoam_suspend(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); boxoam_resume();
    return 0;
  }
  /* Same read-only-SOURCE gate as app_party_overlay (see that function's own comment,
   * pdna_main.c) — defence in depth on a path that must never open for a mounted
   * foreign source, even though both call sites below already gate on !src->is_bank. */
  if (app_src_readonly()) { snd_deny(); return 0; }

  int result = 0;                     /* 0 = closed; the goto-cleanup single-exit below
                                       * needs one value all return paths funnel through */
  int sel = 0;                        /* 0..5 = one of the 6 fixed tile positions;
                                       * == rows (below) = CANCEL, the 7th stop      */
  bool sel_seeded = false;            /* MUST-FIX 8: one-time seed, once `rows` is known */
  int col_sel = 1;                    /* MUST-FIX 8: last COLUMN row (never 0/offset or
                                       * CANCEL), so RIGHT from the offset tile returns
                                       * somewhere sane regardless of party size; CHANGE 2
                                       * also updates this on the PANEL<->GRID crossing so
                                       * a RIGHT-then-LEFT round trip is a no-op            */
  enum { PCP_FOCUS_PANEL = 0, PCP_FOCUS_GRID = 1 };   /* CHANGE 2: see this function
                                                       * group's own top comment          */
  int focus = PCP_FOCUS_PANEL;
  int gcur = *cur;                    /* the box grid's own cursor while focus==GRID;
                                       * seeded from the caller's last box cursor, but
                                       * only ever DISPLAYED once RIGHT actually crosses
                                       * into the grid (which overwrites it then)         */
  uint8_t* recs = src->records(box);  /* CHANGE 2: grid-focus grab/place mutate the box
                                       * directly; this popup never switches boxes (no
                                       * L/R here), so this stays valid for the whole
                                       * call, same guarantee the outer loop's own
                                       * `recs` already relies on                        */
  /* Hide the grid icons under the panel's own on-screen rectangle and reserve 6 of
   * them (5 column + 1 offset) for the panel's OWN icons (box_oam.c) — done ONCE, not
   * per iteration, since neither the panel's rectangle nor the box's contents move
   * while this popup is open. From here on every exit path must reach `out` so those
   * slots come back. */
  boxoam_strip_open(PDNA_PCP_OCCLUDE_X0, PDNA_PCP_OCCLUDE_X1);
  for (;;) {
    int n = app_party_n();
    if (n < 1) goto out;                              /* shouldn't happen (party never empties) */
    int addslot = (s_holding && n < 6) ? n : -1;     /* PLACE: cell n is the "+ add" target;
                                                       * s_holding (not a snapshot param any
                                                       * more, CHANGE 2) so a mon grabbed
                                                       * mid-session from the GRID also gets
                                                       * the add-slot target                */
    int rows = (addslot >= 0) ? n + 1 : n;           /* selectable party/add tiles, 1..6   */
    if (sel > rows) sel = rows;                      /* rows itself is the CANCEL stop     */
    /* MUST-FIX 8 (2026-08-20 review): pcp_pos puts idx 0 (the offset tile) BELOW idx 1
     * (the column's own top row) on screen, so seeding the cursor to idx 0 meant the
     * very FIRST DOWN press most players make jumped the highlight up-and-right by
     * 48 px — the geometry is retail-correct, the linear idx==sel mapping wasn't.
     * Seed to the column's own top row instead (when one exists) so DOWN reads
     * naturally from the first press; idx 0 stays reachable via LEFT/RIGHT, below. */
    if (!sel_seeded) { sel_seeded = true; if (rows >= 2) sel = 1; }

    /* PkMon[6] on the stack, same as app_party_overlay's own local (pdna_main.c) — not
     * EWRAM_BSS: the EWRAM guard is down to 748 B free (see this project's own build
     * budget), and this ~700 B fits comfortably inside the ~12 KB IWRAM stack margin
     * (c-coding-guideline.md S1's concern is multi-KB locals, not this). */
    PkMon pm[6]; app_party_read(pm);

    render_full(src, box, gcur, false, false, false);  /* box grid/banner/footer stay alive
                                                         * behind the panel; also undoes
                                                         * whatever a sub-screen opened by
                                                         * app_party_mon_menu/app_mon_menu
                                                         * last iteration may have painted
                                                         * over. Always gcur (CHANGE 2): its
                                                         * own oam_sync positions the box
                                                         * cursor/carry there, which matters
                                                         * once focus==GRID and is harmless
                                                         * (immediately hidden) otherwise. */
    if (focus == PCP_FOCUS_PANEL)
      boxoam_strip_hide_cursor();      /* MUST-FIX 2: render_full's own oam_sync() just
                                       * positioned the box's cursor hand / carried mon
                                       * at gcur, which the panel covers for up to 2/3 of
                                       * the grid — suppress them while focus is on the
                                       * PANEL (restored at `out:`, or the instant focus
                                       * crosses to GRID, CHANGE 2) */
    ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);   /* SHOULD-FIX 6: replace whatever fragment of
                                       * the box's own footer survived render_full's draw
                                       * (see PDNA_LBL_PCP_FOOTER's own comment) */
    ui_ptext(PDNA_PCP_OCCLUDE_X1 + 2, 152, RGB15(31, 31, 31), PDNA_LBL_PCP_FOOTER);
    strip_restore_icons(PDNA_PCP_OCCLUDE_X0, PDNA_PCP_OCCLUDE_X1);  /* the sliver of
                                                         * hidden-column icon outside the
                                                         * panel's own rectangle — before
                                                         * the panel's chrome draws over
                                                         * the rest */
    pcp_draw_panel();                                   /* bevel + teal dither, every frame:
                                                         * render_full repainted the box BG
                                                         * underneath it a moment ago       */
    pcp_declare_party(pm, n);                           /* the 6 rows, before the tiles that
                                                         * draw them -- see its own comment  */
    for (int idx = 0; idx < 6; idx++) {                 /* all 6 physical tiles, every frame
                                                         * (see this group's own top comment) */
      bool isAdd = (idx == addslot);
      const PkMon* p = (!isAdd && idx < n) ? &pm[idx] : 0;
      pcp_draw_slot(idx, p, isAdd, focus == PCP_FOCUS_PANEL && idx == sel && sel < rows);
    }
    pcp_draw_cancel(focus == PCP_FOCUS_PANEL && sel == rows);
    boxoam_commit();                  /* flush the panel's own OBJ icons + the hidden grid slots;
                                       * render_full's own oam_sync() only ever touches the shadow */
    draw_left(focus == PCP_FOCUS_GRID ? &g_box[gcur] : (sel < n ? &pm[sel] : 0));
                                          /* the reused PKMN DATA panel follows whichever
                                          * focus has the cursor (CHANGE 2), not the box's
                                          * cursor cell from BEFORE the popup opened —
                                          * design decision #3 (blank while CANCEL is
                                          * focused: sel==rows >= n, same as "no mon") */

    u16 k; do { s_vsync(); k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B); } while (!k);
    if (k & KEY_B) { snd_back(); goto out; }          /* closes regardless of focus or of
                                                       * whether a mon is held — a carry
                                                       * that came from OUTSIDE this popup
                                                       * (or one grabbed via GRID focus,
                                                       * CHANGE 2) is untouched by B, exactly
                                                       * like the outer loop's own carry-B
                                                       * contract; nothing is ever discarded
                                                       * here (RULE 3) */
    else if (focus == PCP_FOCUS_GRID) {
      /* CHANGE 2: the box's own grid, restricted to the 2 columns the panel does not
       * cover (COLS-2, COLS-1 — see boxoam_strip_open's own comment, box_oam.c, for
       * why only those 2 of 6 columns survive the panel's occlusion at this build's
       * geometry). Clamp (not wrap) at every edge except the near column's LEFT,
       * which is the crossing back to PANEL. */
      if (k & KEY_LEFT) {
        if (gcur % COLS > COLS - 2) { snd_move(); gcur--; }
        else { snd_move(); focus = PCP_FOCUS_PANEL; sel = col_sel; }
      }
      else if (k & KEY_RIGHT) { if (gcur % COLS < COLS - 1) { snd_move(); gcur++; } }
      else if (k & KEY_UP)    { if (gcur >= COLS) { snd_move(); gcur -= COLS; } }
      else if (k & KEY_DOWN)  { if (gcur < COLS * (ROWS - 1)) { snd_move(); gcur += COLS; } }
      else if (k & KEY_A) {
        if (s_holding) {           /* PLACE onto the box cell — the SAME primitive (and
                                    * the SAME empty/swap/cross-scope rules) the outer
                                    * loop's own s_holding+A path already trusts */
          play_place_anim_down(src, box, gcur);
          bool done; recs = drop_held(src, box, gcur, recs, &done);
          box_decode(src, recs, box); s_oam_reload = true;
          play_place_anim_up(src, box, gcur);
          if (done) { result = 1; goto out; }   /* hand now genuinely empty -> close, same
                                                 * as the panel's own successful PLACE     */
          /* else: SWAP -> still holding the displaced occupant; loop continues, exactly
           * like the outer loop's own swap handling (drop_held already reset the
           * origin to -1 for it — a RAM-only carry, never crosses save scopes) */
        } else if (s_cur_mode == CM_MOVE && g_box[gcur].species) {
          /* 2026-08-23 ("grab pokemon from party to pc and back, intuitively"):
           * this cell IS a box grid cell (CHANGE 2 only restricts WHICH columns
           * are reachable, not what a cell IS), so it must honour the SAME
           * s_cur_mode the outer loop's own grid already does — before this fix
           * it never consulted s_cur_mode at all, so setting MOVE mode on the
           * grid (SELECT) and then crossing in here via RIGHT silently reverted
           * to the slow menu-first grab, a real third grammar for one gesture.
           * Mirrors begin_select()'s own no-drag tap (start_carry + the grab
           * beat, no menu) — not its rubber-band drag: this focus only shows 2
           * of 6 columns (boxoam_strip_open's occlusion), so a multi-cell
           * rectangle would reach into cells hidden under the panel's own
           * chrome. The popup stays open, same as the menu's own MOVE branch
           * below, so the grabbed mon can be walked back to the panel.
           * BACKLOG #150 S150-4: return ignored -- this is the party-strip FOCUS
           * popup (boxoam_strip_open), which pdna_box.h's own is_bank comment says a
           * GB source never opens (is_bank suppresses the PARTY tab this popup is
           * reached through) -- PC/Bank-only, so start_carry always returns true here. */
          (void)start_carry(src, recs, box, gcur);
          play_grab_anim(src, box, gcur);
        } else if (g_box[gcur].species || app_src_empty_action_offered()) {   /* GRAB / PASTE (GB):
                                            * the box's own action menu, same call the
                                            * outer loop's NORMAL-mode A already makes.
                                            * S5-B review fix (BLOCKING #1): an empty GB
                                            * cell must still open the menu when PASTE
                                            * (GB) is on offer, or that action is
                                            * unreachable from this focus. */
          int mbox = src->is_bank ? 0 : box;
          boxoam_suspend();                 /* full-screen sub-view — own bracket, see box_oam.h */
          app_mon_menu(recs + (uint32_t)gcur * 80, false, src->is_bank, src->commit,
                       src->menu_block, mbox, gcur, UI_FOOTER_Y);
          boxoam_resume();
          recs = src->records(box); box_decode(src, recs, box); s_oam_reload = true;
          if (app_take_move_request()) {                    /* MOVE -> into the glove; the
                                                             * popup STAYS OPEN (CHANGE 2's
                                                             * whole point) so it can be
                                                             * walked back to the panel
                                                             * BACKLOG #150 S150-4: same
                                                             * party-strip-is-PC/Bank-only
                                                             * reasoning as the tap-grab
                                                             * above -- return ignored. */
            (void)start_carry(src, recs, box, gcur);
            play_grab_anim(src, box, gcur);
          } else if (app_take_dup_request()) {                /* DUPLICATE -> a fresh copy;
                                                                * both flags are consumed
                                                                * here so neither can leak
                                                                * into a LATER, unrelated
                                                                * app_mon_menu call         */
            memcpy(s_held, recs + (uint32_t)gcur * 80, 80);
            s_holding = true; s_orig_box = box; s_orig_slot = -1; s_orig_scope = src->scope;
            s_held_dup = true; s_orig_party = false;
            play_grab_anim(src, box, gcur);
          }
          /* neither -> the menu only viewed/edited/released in place; recs/g_box are
           * already fresh (refreshed above), the next iteration's redraw reflects it */
        }
      }
    }
    /* MUST-FIX 8: LEFT/RIGHT hop between the offset tile (idx 0) and the column (idx
     * 1..rows-1) — chosen over row-aligning LEFT/RIGHT to just idx 3 (the column row
     * that happens to share the offset tile's own y-range) because that would make
     * idx 0 UNREACHABLE by any input whenever the party has fewer than 4 members
     * (idx 3 doesn't exist as a selectable stop until rows>=4); this version works at
     * every party size. UP/DOWN never cross the idx0<->column boundary any more — no
     * keypress can ever move the highlight in a direction the input didn't ask for. */
    else if (k & KEY_LEFT)  { if (sel > 0 && sel < rows) { snd_move(); col_sel = sel; sel = 0; } }
    else if (k & KEY_RIGHT) {
      if (sel == 0 && rows >= 2) { snd_move(); sel = col_sel; }
      else if (sel > 0 && sel < rows) {         /* CHANGE 2: column -> GRID, same row,
                                                 * entering at the near visible column */
        snd_move(); col_sel = sel; focus = PCP_FOCUS_GRID;
        gcur = (sel - 1) * COLS + (COLS - 2);
      }
    }
    else if (k & KEY_UP) {
      if (sel > 1) { snd_move(); sel--; }
      else if (sel == 1 && rows == 1) { snd_move(); sel = 0; }   /* no column at all (n==1,
                                                                   * browse): idx0 and CANCEL
                                                                   * are the only 2 stops     */
    }
    else if (k & KEY_DOWN) {
      if (sel == 0) { if (rows == 1) { snd_move(); sel = rows; } }  /* same no-column case,
                                                                     * the only path to CANCEL */
      else if (sel < rows) { snd_move(); sel++; }
    }
    else if (k & KEY_A) {
      if (sel == rows) { snd_back(); goto out; }        /* CANCEL selected -> same as B */
      if (s_holding) {                                /* PLACE: drop/swap into the party.
                                                        * s_holding/s_held/s_orig_* (not
                                                        * snapshot params any more, CHANGE
                                                        * 2) so a mon grabbed mid-session
                                                        * from the GRID is placeable here
                                                        * too, without ever leaving this
                                                        * popup. */
        bool can_swap_now = (s_orig_scope == BOXSCOPE_PC && s_orig_slot >= 0);
        /* MUST-FIX (found by pixel-diffing the capture, not trusted from the trace):
         * a box-origin carry's ADD clears its origin cell, and SWAP overwrites the
         * target's origin cell, BOTH by writing g_pc directly (party_place_held /
         * clear_origin) -- bytes this popup's OWN g_box[]/OAM cache never sees unless
         * something re-decodes it. The very first version of this fix left that to
         * the CALLER (mirroring how call site B's own rr==1 handling already did a
         * redundant box_decode) -- which is exactly backwards: call site A (the
         * BROWSE entry) never re-decoded at all, so a GRID-grabbed-then-PANEL-placed
         * mon updated the real save data correctly but the box grid kept showing the
         * stale pre-swap icon until the NEXT unrelated redraw. Refresh here,
         * unconditionally, so this function is self-sufficient regardless of which
         * call site is running (matches the same principle the s_holding clear below
         * already follows). */
        /* BACKLOG #174 (S150-8c) D2: a native Bank cell now converts and joins the
         * Gen-3 party, mirroring drop_held's own arm (:1529 ff.) -- the party site's
         * OWN pre-flights ALL run before the conversion arm (G-M2's rule): a non-ADD
         * target (a SWAP is already impossible for a bank origin -- can_swap_now is
         * false and party_place_held forces can_swap=false for a bank origin, but
         * today the user would only learn that AFTER the whole conversion had run;
         * refusing it first makes the only reachable post-ledger failure impossible,
         * D4/D5), a full party (party_place_held's own PARTY FULL check runs AFTER a
         * landing would have been recorded -- hoist the identical condition), and a
         * full Bank defer queue (a full queue would silently drop the delete, D3's
         * queue-full precondition). Each dialog is bracketed with its OWN
         * boxoam_suspend()/boxoam_resume() -- never once around the whole block:
         * gb_down_loss_screen re-enables OBJ on its own return, so an outer bracket
         * would be cancelled by the inner one and the NEXT dialog would draw over
         * live box sprites (BACKLOG #207's exact defect class). */
        bool placed = false;
        int n_now = app_party_n();
        bool native = bc_is_native(s_held);
        bool converted = false; uint8_t conv[80];
        /* BACKLOG #174 review D4: with a FULL party, addslot == -1 so rows == n_now
         * and `sel == n_now` (the CANCEL row) is consumed by the CANCEL branch before
         * this switch is ever reached -- sel here can only be in [0, n_now-1], which
         * is NEVER != n_now, so the old PARTYSWAP-first order made PARTYFULL3
         * unreachable (OQ3 never delivered). Full-party must be decided FIRST.
         * BACKLOG #226: bounded 2-attempt loop (golden rule 2's provable bound), same
         * shape as the restore sites' own #175c fix -- attempt 0 may offer to send a
         * party member to a box first (Gen-1/2 parity, gb_accept_down_party_deposit()
         * in source/pdna_gen12.c) instead of the flat PARTYFULL3 wall; on success it
         * re-targets `sel` at the now-free ADD slot and re-runs the chain once
         * (attempt 1) so the SAME convert+place code below lands the held cell.
         * Attempt 1 never offers again: a party still full after a verified deposit
         * would mean party_release()+app_inject_to_game_deferred() together did not
         * free a slot, which cannot happen (both are plain C mutations, no card I/O
         * in between) -- treat it as a real refusal, not a retry loop. */
        /* BACKLOG #226 review D1: dep_box/dep_slot track where app_party_full_
         * deposit_offer() landed the deposited party member (-1,-1 = "no deposit
         * outstanding"); app_party_deposit_undo() rolls it back on any refusal that
         * survives the deposit. D1(a): app_bank_defer_full()'s "TOO MANY MOVES" and
         * the 16(g) pending-transfer SAVE NOW? confirm -- both normally run INSIDE
         * bank_down_convert_gen3_party() -- are hoisted to run BEFORE the deposit
         * (a decline here touches nothing, like every sibling refusal). Only
         * gb_down_loss_screen/gb_paste_legal_screen, which run inside the convert
         * and need the freed slot's converted record to know what to ask, can still
         * refuse AFTER the deposit -- bank_down_convert_gen3_party()'s own dstrec is
         * always k_empty80 for the party target (bank_down_convert.c), so its
         * internal occupancy test never fires here. */
        int dep_box = -1, dep_slot = -1;
        for (int attempt = 0; attempt < 2; attempt++) {
          if (native && n_now >= 6) {
            if (attempt == 1) {
              /* review D3-R: cannot happen with a deposit outstanding (dep_box < 0
               * here always -- see the loop's own header comment above), but the
               * rollback guard costs nothing and matches every sibling arm below;
               * golden rule 7, never trust a postcondition silently. */
              if (dep_box >= 0) { app_party_deposit_undo(dep_box, dep_slot); dep_box = -1; }
              boxoam_suspend(); snd_deny();
              msg_wait(PDNA_XFER_PARTYFULL3_TITLE, UI_WARN, PDNA_XFER_PARTYFULL3_L1, PDNA_XFER_PARTYFULL3_L2);
              boxoam_resume();
              placed = false;
              break;
            }
            if (s_orig_scope == BOXSCOPE_BANK && s_orig_slot >= 0 && app_bank_defer_full()) {
              boxoam_suspend(); snd_deny();
              msg_wait("TOO MANY MOVES", UI_WARN, "Save first, then continue.", 0);
              boxoam_resume();
              placed = false;
              break;
            }
            if (!xg_cell_is_copy(s_held) && app_xfer_pending()) {
              boxoam_suspend();
              char l1[64];
              siprintf(l1, "%s %s", PDNA_XFER_SAVENOW_L1, PDNA_XFER_SAVENOW_L2);
              bool yes = app_confirm(PDNA_XFER_SAVENOW_TITLE, l1);       /* A = save now, B = no */
              bool ok  = yes && app_xfer_save_now();  /* the helper shows its own NOTSAVED on failure */
              boxoam_resume();
              if (!ok) { snd_deny(); placed = false; break; }
            }
            boxoam_suspend();
            bool deposited = app_party_full_deposit_offer(&dep_box, &dep_slot);  /* review D3: bracket */
            boxoam_resume();
            if (!deposited) {
              /* review D3-R: app_party_full_deposit_offer writes *out_box/*out_slot
               * BEFORE evaluating D5's `return app_party_n() < 6;`, so a false
               * return here can still leave dep_box/dep_slot pointing at a real
               * deposit (the postcondition-fail edge, unreachable in practice --
               * see D5's own comment -- but never trusted silently). */
              if (dep_box >= 0) { app_party_deposit_undo(dep_box, dep_slot); dep_box = -1; }
              placed = false; break;
            }
            n_now = app_party_n(); sel = n_now;   /* re-target the newly-freed ADD slot */
            continue;
          }
          if (native && sel != n_now) {
            boxoam_suspend(); snd_deny();
            msg_wait(PDNA_XFER_PARTYSWAP_TITLE, UI_WARN, PDNA_XFER_PARTYSWAP_L1, PDNA_XFER_PARTYSWAP_L2);
            boxoam_resume();
            if (dep_box >= 0) { app_party_deposit_undo(dep_box, dep_slot); dep_box = -1; }
            placed = false;
            break;
          }
          if (native && s_orig_scope == BOXSCOPE_BANK && s_orig_slot >= 0 && app_bank_defer_full()) {
            boxoam_suspend(); snd_deny();
            msg_wait("TOO MANY MOVES", UI_WARN, "Save first, then continue.", 0);
            boxoam_resume();
            if (dep_box >= 0) { app_party_deposit_undo(dep_box, dep_slot); dep_box = -1; }
            placed = false;
            break;
          }
          if (native && bank_down_convert_gen3_party(src, sel, s_held, conv) != BANK_DOWN_CONVERTED) {
            if (dep_box >= 0) { app_party_deposit_undo(dep_box, dep_slot); dep_box = -1; }
            placed = false;                          /* the arm's own dialog already said why */
            break;
          }
          converted = native;
          /* BACKLOG #174 D4: `placing` mirrors drop_held's own idiom (:1686) -- a
           * converted record is an ordinary Gen-3 record the escape gate would
           * (correctly) never refuse; s_held is still native, so an ungated call
           * would refuse a transfer its own arm just approved (the identical
           * reasoning at drop_held's own escape gate, :1614-1619). */
          const uint8_t* placing = converted ? conv : s_held;
          if (!converted && xg_native_escape_denied(s_held, BOXSCOPE_PC)) { snd_deny(); placed = false; }
          else placed = app_party_place_held(placing, sel, s_orig_box, s_orig_slot,
                                             (s_orig_scope == BOXSCOPE_BANK), can_swap_now,
                                             converted ? s_held : NULL);
          /* BACKLOG #174 D5: after a converted cell's ledger entry is written,
           * party_append can fail ONLY when party_count() >= PARTY_MAX -- already
           * excluded above by n_now >= 6 on this SAME frame -- and box_to_party
           * cannot fail. If app_party_place_held still returns false here, the
           * ledger entry this frame just wrote must be undone (nothing landed
           * anywhere yet -- unlike flush_on_exit's failed-commit arm, where the
           * write may already be on the card, which is why #176 chose _drop there
           * instead). */
          if (converted && !placed) {
            log_line("party: place refused AFTER a converted transfer -- undoing the ledger entry");
            app_xfer_pending_undo();
            if (dep_box >= 0) { app_party_deposit_undo(dep_box, dep_slot); dep_box = -1; }
            snd_error();
            msg_wait(PDNA_XFERREC_TITLE, UI_WARN, PDNA_XFERREC_L1, PDNA_XFERREC_L2);
          }
          break;
        }
        /* BACKLOG #226 review D6: dep_box >= 0 means the offer landed a mon in a PC
         * box (possibly the one on screen) and was NOT rolled back above -- refresh
         * regardless of `placed`/`s_orig_scope` so the grid's occupancy count can't
         * go stale until the user happens to change boxes (box_decode() re-reads the
         * CURRENTLY VIEWED `box`, a harmless redundant redraw when dep_box != box). */
        if ((placed && s_orig_scope == BOXSCOPE_PC && s_orig_slot >= 0) || dep_box >= 0) {
          recs = src->records(box); box_decode(src, recs, box); s_oam_reload = true;
        }
        if (placed) {
          /* party_place_held doesn't know about s_holding -- clear it HERE so this
           * function is self-sufficient regardless of which call site is running (a
           * mon grabbed via GRID focus can close a BROWSE-entered session this way,
           * a caller that only ever expected PLACE-entry's rr==1 didn't used to have
           * to clear this itself) */
          s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_scope = BOXSCOPE_PC;
          s_held_dup = false; s_orig_party = false;
          result = 1; goto out;
        }
      } else if (sel < n) {                          /* BROWSE: the full action menu on this mon */
        bool tobox_hit = false;
        boxoam_suspend();               /* full-screen sub-view — own bracket, see box_oam.h */
        app_party_mon_menu(sel, UI_FOOTER_Y, allow_move_to_box, grab80, &tobox_hit);
        boxoam_resume();
        if (tobox_hit) { if (grab_slot) *grab_slot = sel; result = 2; goto out; }
        /* else: edit/release/etc. ran -> loop re-reads the party + clamps sel, then redraws */
      }
    }
  }
out:
  *cur = gcur;                          /* CHANGE 2: hand the box cursor back to the
                                       * caller wherever GRID focus (if ever entered)
                                       * left it — a no-op if focus never left PANEL */
  oam_sync(*cur, false, box, src->is_bank);   /* MUST-FIX 2: bring the box's own cursor
                                       * hand / carried mon back now that the panel is
                                       * closing — mirrors boxoam_strip_close() right
                                       * below, which hands the borrowed grid slots back */
  boxoam_strip_close();                 /* hand every hidden/reused grid slot back */
  return result;
}

/* Open the PARTY-tab strip exactly as if the user had pressed UP then A on the box's
 * own top tabs -- factored out of that dispatch (below) so pdna_box()'s new
 * app_box_start_take()==3 entry state (pdna_main.c's NV_PARTY routing, Guy's "the
 * party menu on top of the pc pokemon in the background" ask) can fire the SAME
 * popup on entry, before the user has pressed anything. `*need_full` is left the
 * caller's to act on: true means "repaint from scratch" (the popup just closed with
 * nothing else painted since), matching what a manual A-press already left it at;
 * the grab branch instead paints everything itself (render_full + the grab anim +
 * footer, same as the manual path always did) and clears it, so a caller that was
 * sitting on need_full==true from its own fresh entry (this function's only other
 * caller always had it already false, from its own prior paint) does not repeat
 * that paint a second time for no reason. */
/* D1-style split (gbscr_open/gbscr_open_inner precedent, source/pdna_gbscreen.c:463-556,
 * rom_gbui.h:213-224's own note on why the OTHER order is wrong): the stack-room gate has
 * to run BEFORE this function's own frame exists. Gating from inside the frame measures
 * the room LEFT UNDER it, not the room the frame (plus everything it calls) NEEDS -- on a
 * build already tight on stack that under-counts by exactly this frame's own size and
 * refuses when it shouldn't, or (worse here) fails to refuse when it should. The renamed
 * original body is `pcp_open_party_strip_inner()`; this file's `pcp_open_party_strip()`
 * below is the thin wrapper that gates first, then tail-calls into it.
 *
 * PDNA_PARTY_STRIP_NEED is the tail BELOW this wrapper's own (tiny, is_bank-check-only)
 * frame, as measured by tools/stack_budget.py (BACKLOG #102, 2026-09-11), re-derived
 * from BOTH gate ELFs after the two GB-rung art descents (gb_art_fetch/gb_art_fetch_
 * icon.constprop.0) were declared `gated` in tools/stack_edges.txt:
 *
 *   python3 tools/stack_budget.py --elf PokeDNA-artless.elf \
 *       --builddir "$(pwd)/build-artless" --root pcp_open_party_strip_inner --top 3
 *   python3 tools/stack_budget.py --elf PokeDNA.elf \
 *       --builddir "$(pwd)/build" --root pcp_open_party_strip_inner --top 3
 *
 * BACKLOG #84b's fourth pass (2026-09-10) correctly found this call site's deepest
 * chain descending through pdna_origin_art_portrait -> fetch_pic_ex.constprop.0 ->
 * gb_art_pic_cb -> gb_art_fetch -> gb_art_save_loc -> log_line -> vsniprintf -> ...
 * -> newlib's malloc/free chain (9,720-9,744 B depending on exact tree state), once a
 * PdnaGbArtSource dispatch blind spot was declared and the walker could finally see
 * that branch at all. What it did NOT yet account for: gb_art_fetch is only ever
 * entered after pdna_origin_art_portrait's OWN runtime pdna_origin_art_stack_room(
 * PDNA_GB_FETCH_NEED) check has already passed (source/pdna_origin_art.c:530,559) --
 * a SEPARATE, independent gate, freshly re-verified against the LIVE stack pointer at
 * the exact moment gb_art_fetch actually runs. Because that inner gate is what keeps
 * gb_art_fetch's execution safe, PDNA_PARTY_STRIP_NEED does not have to additively
 * reserve room for it on top of its own chain -- doing so was inflating this
 * constant by the whole subtree's real size (~5,750 B) for no safety benefit, exactly
 * the "safe-direction inflation" BACKLOG #102 exists to remove. tools/stack_edges.txt
 * now declares `gated gb_art_fetch need=6144` and `gated gb_art_fetch_icon.constprop.0
 * need=6144` (the literal PDNA_GB_FETCH_NEED/PDNA_GB_ICON_NEED values, source/
 * gb_art_source.h:19,44). D2 (review-opus fix pass, 2026-09-12) corrects an
 * overclaim this comment used to make here: this re-verification is NOT part of
 * any build -- `make`/`make artless` run stack_budget.py with `--root main`
 * (Makefile), where enforce_gates=False and a gated declaration is a deliberate
 * no-op (see deepest_from()'s own BACKLOG #102 docstring for why). Re-run
 * `--root pcp_open_party_strip_inner` BY HAND on both gate ELFs after any change
 * under pdna_origin_art_portrait / gb_art_source.c, and separately re-check
 * `--root fetch_pic_ex.constprop.0` (5,864 B on the artless ELF, 2026-09-12)
 * against PDNA_GB_FETCH_NEED (6,144) -- that whole chain, not gb_art_fetch's own
 * 5,752 B alone, is what the runtime gate must cover (fetch_pic_ex.constprop.0
 * and gb_art_pic_cb's own frames sit BETWEEN the gate check in
 * pdna_origin_art_portrait and gb_art_fetch), with 280 B of margin today. (A
 * `--root fetch_pic_ex.constprop.0` run against a build tree where gb_art_fetch
 * is STILL declared `gated` under-reports this number, since that declaration's
 * own exclusion would apply recursively to the very subtree being re-checked --
 * temporarily comment the `gated gb_art_fetch` line out of tools/stack_edges.txt
 * for this one measurement, exactly how the 5,864 figure above was obtained.)
 *
 * With both descents excluded, the heaviest chain for this root used to be the
 * OLD SD-write branch BACKLOG #84b's own comment named as the runner-up
 * (party_strip_overlay -> app_party_mon_menu -> app_mon_menu -> app_paste_gb_merge
 * -> app_commit_sb1 -> ... -> ed_sd_dma_to_rom, 6,552 B artless / 6,536 B normal,
 * +64 ISR = 6,616). BACKLOG #93 (2026-09-15, review-opus re-derivation) replaced
 * it: app_mon_menu now also reaches app_mon_menu_readonly (the read-only GB source
 * popup, g_src_ro true for the whole party-strip visit), whose new DUPLICATE/TO
 * DAY-CARE/EXPORT rows add a deeper real chain than the old paste/commit one --
 *
 *   party_strip_overlay -> app_party_mon_menu -> app_mon_menu ->
 *   app_mon_menu_readonly -> gb_daycare_hook -> pdna_gbdaycare ->
 *   pdna_gbsummary_inner -> gbedit_press -> ... -> commit_bytes -> f_mkdir
 *
 * MEASURED (2026-09-15, both ELFs freshly built from this exact tree):
 *
 *   python3 tools/stack_budget.py --elf PokeDNA-artless.elf \
 *       --builddir "$(pwd)/build-artless" --root pcp_open_party_strip_inner --top 3
 *   python3 tools/stack_budget.py --elf PokeDNA.elf \
 *       --builddir "$(pwd)/build" --root pcp_open_party_strip_inner --top 3
 *
 * 7,128 B on the artless ELF, 7,112 B on the normal ELF (re-measured after merging into 48b5eaa: b54's app_mon_menu frame grew 888 -> 896) -- the WORSE (artless) of
 * the two is taken, +64 B for the ISR reentry onto the same stack (libtonc's
 * isr_master runs handlers on __sp_usr too) = 7,184. (The old paste/commit chain
 * is still reachable from this root and still real -- gb_daycare_hook's chain is
 * simply deeper now, 7,120 > 6,552, so it is the one that sets the constant.)
 *
 * With #84a's stack room at ~15,080-15,616 B (tools/stack_budget.py's own "STACK ok"
 * line, both non-delta build variants) this gate should NEVER fire on today's tree --
 * margin ~7,888-8,432 B at 7,184, comfortably above the 1,024 B floor the static
 * per-build guard itself requires -- it is a tripwire against a future regression
 * eating most of that margin, not a live constraint. Proved by: (a) shooting the
 * party strip once in mGBA at the re-derived constant, confirming it still opens
 * (BACKLOG #102, tools/dgb_shots.py); (b) a scratch delta-artless build with
 * PDNA_PARTY_STRIP_NEED temporarily raised to 20,000, confirming the refusal message
 * appears instead of a silent overrun, then reverted back to the real derived
 * constant (same pass) -- real-hardware sign-off is a SEPARATE, still-pending gate
 * (hardware-testing-protocol; the emulator cannot prove a stack-overflow refusal is
 * correct on real silicon, only that the code path the refusal message takes is
 * reachable and renders). */
#define PDNA_PARTY_STRIP_NEED 7392   /* RE-CHECKED 2026-09-23 (BACKLOG #227 D2, lane mail-integrity
                                      * fix pass): the mail guard added to app_duplicate/app_release/
                                      * app_to_daycare also inlines into app_mon_menu on this #1 chain,
                                      * but each guard is one extra call plus a branch, not a new local
                                      * buffer -- re-running the exact commands below on fresh artless
                                      * AND normal ELFs after the fix still prints 7,392 artless / 7,384
                                      * normal, unchanged from #229's own derivation, so this constant
                                      * and tools/stack_edges.txt's need=7392 line stand as-is.
                                      *
                                      * re-derived 2026-09-23 (BACKLOG #229, lane mail-integrity):
                                      * app_create_mon (inlined into app_mon_menu, reached through
                                      * app_party_mon_menu on this same #1 chain) gained an is_party
                                      * branch -- a new uint8_t p100[100] plus the box_to_party() call
                                      * that fills it -- growing app_mon_menu's own frame 896 -> 992 B
                                      * (+96 B artless; the normal build's chain moved the same amount,
                                      * 7,224 -> 7,320). New need = 7,328 + 64 ISR = 7,392 (artless
                                      * dominates). RE-MEASURED on this exact tree:
                                      *   python3 tools/stack_budget.py --elf PokeDNA-artless.elf \
                                      *       --builddir "$(pwd)/build-artless" \
                                      *       --root pcp_open_party_strip_inner --top 6
                                      *   python3 tools/stack_budget.py --elf PokeDNA.elf \
                                      *       --builddir "$(pwd)/build" \
                                      *       --root pcp_open_party_strip_inner --top 1
                                      * confirms 7,392 artless / 7,384 normal. #1 chain is UNCHANGED
                                      * in shape (still gb_daycare_hook's, via app_mon_menu_readonly --
                                      * app_create_mon's own is_party branch is not itself reachable
                                      * from this root today, since xg_create_row's `!is_party` gate
                                      * still hides CREATE on the party; only app_mon_menu's compiled
                                      * FRAME SIZE grew, because app_create_mon is unconditionally
                                      * inlined regardless of which branch a given call can reach).
                                      *
                                      * Previously 7,296 = 7,232 + 64 ISR (BACKLOG #226 review D1/D2
                                      * fix pass: party_strip_overlay's own frame 984 -> 1,016 B, +32 B,
                                      * drop_held's D1 rollback locals + D1(a)'s hoisted char l1[64]).
                                      *
                                      * Previously 7,264 artless / 7,256 normal (BACKLOG #150 S150-8c/
                                      * #174 review D2, native-cell party landing).
                                      *
                                      * Older history (was 7,224 = 7,160 + 64 ISR, BACKLOG #150
                                      * S150-3 review D-Q4; was 7,216 = 7,152 + 64 ISR, S150-2
                                      * D-Q1; was 7,192 = 7,128 + 64 ISR, BACKLOG #93; was 6,616 =
                                      * 6,552 + 64 from #102) -- #1 chain is still gb_daycare_hook's
                                      * (DUPLICATE/TO DAY-CARE/EXPORT rows on the read-only GB mon
                                      * menu, reachable through app_mon_menu from this same root),
                                      * not the old app_paste_gb_merge/app_commit_sb1 branch. */

static void __attribute__((noinline)) pcp_open_party_strip_inner(BoxSource* src, int box,
                                                                  int* cur, bool* need_full) {
  uint8_t grab[80]; int gslot = -1;
  /* empty-handed: A opens the action menu (Move to box -> grab). The popup's own
   * GRID focus, if the user crosses into it, moves the box cursor and hands the new
   * position back on close; it can also grab a BOX mon itself (rr==1, placed into
   * the party without ever leaving the popup) as well as the party-mon grab below. */
  int rr = party_strip_overlay(src, box, cur, grab, &gslot, true);
  if (rr == 2 && gslot >= 0) {                  /* grabbed a party mon -> carry it (party origin) */
    memcpy(s_held, grab, 80);
    s_holding = true; s_orig_party = true; s_orig_slot = gslot;
    s_orig_box = -1; s_orig_scope = BOXSCOPE_PC; s_held_dup = false;
    s_tab_focus = -1; s_oam_reload = true;
    render_full(src, box, *cur, false, false, true);                  /* repaint box over the popup */
    play_grab_anim(src, box, *cur); carry_move(src, box, *cur, *cur);
    draw_footer(src->is_bank, false, true);
    *need_full = false;                         /* already fully painted, see header comment */
  } else { s_tab_focus = -1; s_oam_reload = true; *need_full = true; } /* closed -> back to the
                                      * grid (rr==0 not holding, rr==0 still holding a
                                      * GRID-grabbed box mon, or rr==1 already placed --
                                      * all three want the same fresh redraw) */
}

static void __attribute__((noinline)) pcp_open_party_strip(BoxSource* src, int box, int* cur,
                                                            bool* need_full) {
  if (src->is_bank) { snd_deny(); return; }
  if (!pdna_origin_art_stack_room(PDNA_PARTY_STRIP_NEED)) {   /* tripwire, see comment above */
    snd_deny();
    boxoam_suspend();
    msg_wait("NOT ENOUGH STACK", UI_WARN, "Back out one screen, then retry.", 0);
    boxoam_resume();
    *need_full = true;
    return;                                     /* refuse BEFORE party_strip_overlay's frame
                                                  * (and every write beneath it) ever exists */
  }
  pcp_open_party_strip_inner(src, box, cur, need_full);
}

int pdna_box(BoxSource* src) {
  /* E4 (sprite-era): tell the art router which PLACE is on screen, once per visit --
   * this same function draws the in-save PC, the SD-backed Bank, AND a Game Boy
   * save's own box grid, distinguished by the two fields every BoxSource already
   * carries: `capacity` is non-NULL ONLY for a Game Boy source (its own doc comment,
   * pdna_box.h -- the PC/Bank sources leave it NULL), and `is_bank` tells the PC
   * apart from the Bank for everything else. */
  pdna_origin_art_set_place(src->capacity ? SE_PLACE_GBGRID
                            : src->is_bank ? SE_PLACE_BANK : SE_PLACE_PC);
  int nb = src->nboxes; if (nb < 1) nb = 1;
  int box = src->start_box; if (box < 0 || box >= nb) box = 0;
  int cur = 0;
  bool on_title = false;
  bool need_full = true;
  /* The next full paint goes OVER the current screen instead of ui_clear()-ing to
   * black first. Set by SWITCH_BOX; starts false so the FIRST paint still clears
   * (boxoam_enter does not touch the bitmap, so the previous screen would show). */
  bool paint_over = false;
  int anim_ctr = 0, bob = 0;                   /* current unison Y-bob offset (0/1) */
  /* s_holding persists across pdna_box runs so a carried mon survives the PC<->Bank
   * hand-off (the receiving screen just keeps drawing it). It's reset per-save by
   * pdna_box_clear_carry() — do NOT reset it here. */
  s_cur_mode = CM_NORMAL; s_item_held = 0; s_item_from = -1; s_item_from_box = -1;   /* fresh cursor mode each open */
  s_tab_focus = -1;
  /* A Day-Care withdraw-to-PC parked a mon in a free slot and asked us to carry it:
   * open that box and lift the parked mon into the glove so the user places it. */
  int pickup_ps = -1;
  if (!s_holding && !s_ch_hold) { int pb, ps;
    if (app_take_pickup(&pb, &ps) && pb >= 0 && pb < nb && ps >= 0 && ps < 30) { box = pb; cur = ps; pickup_ps = ps; }
  }
  /* Screen-enter span. On a save open, view_save's own span is already covering this
   * (its whole point is "save open + parse + FIRST PAINT"), so do not start a second
   * one -- spans do not nest, and starting one here would force-close that span and
   * tag it !unclosed. Either way the first full paint below closes whichever span is
   * open, which is the moment the box is actually on screen. */
  bool perf_first_paint = true;
  if (!perf_span_active()) perf_span_begin(src->is_bank ? "bank" : "box");
  boxoam_enter();                             /* enable OBJ; upload hand/grab + palettes */
  s_oam_reload = true;                        /* first paint uploads the box's icon tiles */
  uint8_t* recs = src->records(box);          /* current box's 30*80 records */
  box_decode(src, recs, box);
  /* BACKLOG #200 F2: how many of the 30 grid cells `box` actually has (30 for
   * every Gen-3 PC/Bank source; 20 or 6 for a GB source's storage/party pseudo-
   * box). Recomputed by SWITCH_BOX below on every box change; this is the value
   * for the box the function is entering with, right after it settled. */
  int cap = box_cap(src, box);
  /* BACKLOG #150 S150-4: return ignored -- app_take_pickup()'s flag is set ONLY by
   * day-care withdraw (pdna_main.c, "withdraw->PC sets a pickup"), a Gen-3-only flow
   * that never runs inside a GB session's own box grid -- PC-only, so start_carry
   * always returns true here. */
  if (pickup_ps >= 0) (void)start_carry(src, recs, box, pickup_ps);   /* lift the parked mon */
  /* Cursor-arrival hint when crossing the PC<->Bank edge: bottom row (carrying up into the
   * bank) or the top tabs (only when NOT carrying — you can't rest a held mon on a tab).
   * 3 = open straight onto the PARTY strip (pdna_main.c's NV_PARTY routing) -- see
   * pcp_open_party_strip's call below, right before the main loop starts. */
  bool want_party_strip = false;
  { int st = app_box_start_take();
    if (s_ch_hold) {                                    /* carrying a chunk across the PC<->Bank edge */
      if (s_ch_tc > chunk_anchor_cmax(&s_ch)) s_ch_tc = chunk_anchor_cmax(&s_ch);
      if (st == 2)      s_ch_tr = chunk_anchor_rmax(&s_ch);   /* arrived from below -> bottom of grid */
      else if (st == 1) s_ch_tr = 0;                          /* arrived from above -> top of grid */
      else if (s_ch_tr > chunk_anchor_rmax(&s_ch)) s_ch_tr = chunk_anchor_rmax(&s_ch);
    } else if (st == 1 && !s_holding && !src->is_bank) s_tab_focus = 1;
    /* BACKLOG #142: an is_bank grid (the Game Boy session's own box; the real Bank
     * never reaches st==1 -- see app_box_start_set(1)'s two call sites, both "...
     * PC opens/tabs") has no PARTY tab to land on (tab 1 there is the inert "(BANK)"
     * label), and it landed on tab 2 = SAVE instead -- one stray A ended the session.
     * s_tab_focus is already -1 from this function's own reset a few lines up and
     * `cur` is already 0 (its declared initial value, untouched by this branch), so
     * doing nothing here for is_bank leaves the cursor on the grid's top-left CELL,
     * mirroring the real PC grid's own st==1 arrival (which lands in its tabs, not
     * possible here since GB's tab 1 is inert) -- tabs stay reachable via UP, same as
     * every other grid visit. */
    else if (st == 2) { cur = COLS * (ROWS - 1);
                        if (cur >= cap) cur = cap - 1; }  /* BACKLOG #200 F2: the physical
                                                           * bottom row can itself be
                                                           * blocked on a small-capacity
                                                           * source (e.g. the GB party
                                                           * pseudo-box, cap 6) -- land on
                                                           * the last REAL slot instead */
    else if (st == 3 && !s_holding && !src->is_bank) want_party_strip = true;
    /* BACKLOG #188: no directional hint (st == 0) and no day-care pickup already
     * placed the cursor -- resume the cell this same box was left on last time
     * pdna_box() ran (app_box_resume_note() on every return below), bounds-checked
     * against this call's own grid size; app_box_resume_take() itself already
     * returns -1 on a box mismatch or before the first note, in which case `cur`
     * stays at its declared 0 default, untouched. */
    else if (st == 0 && pickup_ps < 0) {
      int rc = app_box_resume_take(box);
      if (rc >= 0 && rc < cap) cur = rc;   /* BACKLOG #200 F2: bounds-checked against
                                            * capacity too -- a box that shrank (or a
                                            * resume cell from a different source
                                            * entirely) must never resume onto a
                                            * blocked cell */
    }
  }
  /* Switch to box `nbx` (wrapping), reload + redraw. Two things this gets right that
   * it used to get wrong, both visible on every single L/R:
   *
   * 1. THE CURSOR STAYS PUT. Retail's PC scrolls the box *underneath* a stationary
   *    hand: every INPUT_SCROLL_LEFT/RIGHT path in pokeemerald's
   *    pokemon_storage_system.c returns before the handler's SetCursorPosition(), and
   *    the scroll state machine only re-reads the panel for the new box. This used to
   *    do `cur = 0`, so every flip teleported the glove to the top-left cell and lost
   *    your place. `cur` is 0..29 by construction everywhere it is assigned; the clamp
   *    is belt-and-braces because the macro has eight call sites.
   *
   * 2. NO BLACK FLASH. `need_full` used to imply render_full(clear=true), i.e. a
   *    full-screen ui_clear() followed by a multi-frame rebuild — measured at ~12
   *    frames with the wallpaper and banner absent for most of them. render_full
   *    repaints tabs + panel + wallpaper + banner + footer, which is the whole screen,
   *    so on a box switch the clear buys nothing and costs a wipe. The menu-return
   *    paths already knew this and passed clear=false; `paint_over` plumbs the same
   *    thing into the need_full path. */
  /* BACKLOG #163: `box` captured BEFORE the switch, so a refused flush (the OLD box's
   * own box_save() failed and the user chose "keep editing" in banksrc_records's retry
   * dialog) can be told apart from a clean flip -- banksrc_records() already refused to
   * page in `nb__` and returned the OLD box's own records unchanged, so committing to
   * `nb__` here would show the new box's name/wallpaper over the old box's still-loaded,
   * still-dirty data (never a data loss, since g_bankbuf never actually paged, but a
   * confusing display mismatch this skip avoids entirely). Never silently flip: stay on
   * the old box, and still repaint so the leftover retry dialog and the persistent
   * BOX NOT SAVED banner (draw_box_banner) get redrawn on top of it. */
  #define SWITCH_BOX(nbx) do { \
                               int nb__ = (nbx); \
                               uint8_t* nr__ = src->records(nb__); \
                               if (!(src->is_bank && pdna_bank_box_unsaved(box))) { \
                                 box = nb__; recs = nr__; \
                                 box_decode(src, recs, box); \
                                 cap = box_cap(src, box); \
                                 if (cur < 0 || cur >= COLS * ROWS) cur = 0; \
                                 /* BACKLOG #200 F2: a box switch that would land the \
                                  * cursor on a blocked cell (a smaller-capacity box, \
                                  * or the GB party pseudo-box) clamps to the last \
                                  * real slot instead of resting past it. */ \
                                 if (cur >= cap) cur = cap - 1; \
                                 bob = 0; anim_ctr = 0; \
                                 if (!src->is_bank) app_note_pc_box(box); \
                                 if (src->note_box) src->note_box(box); \
                               } \
                               s_oam_reload = true; need_full = true; paint_over = true; } while (0)

  for (;;) {
    if (need_full) {
      bool clr = !paint_over;                  /* a box switch repaints over, no wipe */
      if (s_ch_hold) chunk_draw(src, box, clr, recs);
      else           render_full(src, box, cur, on_title, s_holding, clr);
      need_full = false; paint_over = false;
      app_crumb_shown();   /* one-shot per save-open: the box is on screen (no-op after) */
      /* Closes view_save's save-open span (if app_crumb_shown did not already) or this
       * screen's own -- a no-op if neither is open. */
      if (perf_first_paint) { perf_first_paint = false; perf_span_end(); }
    }
    /* want_party_strip (set above from app_box_start_take()==3): the box has now had
     * its normal first paint -- clear=true, since paint_over starts false -- so it is
     * safe for pcp_open_party_strip's own render_full(...,clear=false) passes to draw
     * over it. Consumed once: a later need_full repaint (e.g. once the strip closes)
     * must never re-trigger this. */
    if (want_party_strip) { want_party_strip = false; pcp_open_party_strip(src, box, &cur, &need_full); }
    u16 k, fresh;
    do { s_vsync();
         /* MUST-FIX 1 (2026-08-22 review): drain the PREVIOUS tick's deferred pose-swap
          * half FIRST, before this tick gets a chance to queue a new one. This used to
          * sit at the BOTTOM of the loop, right after boxoam_set_frame() in the SAME
          * iteration — which meant a just-queued swap's odd half ran mere microseconds
          * after its own even half, both inside the one vblank window s_vsync() just
          * returned from: no real two-tick split at all, exactly the "cannot fit the
          * vblank window" failure box_oam.c's own header warns about. REG_VCOUNT probes
          * (artless+fused Emerald, mGBA) on that ordering: VCOUNT 214 after set_frame,
          * 38 (into the NEXT frame's active display) after pose_pump+commit — 11/11
          * ticks overrunning a 68-scanline VBlank by ~156%. Calling pose_pump() HERE
          * instead — right after s_vsync(), before this tick's own set_frame() can run
          * — makes the split real: the half deferred on tick N is drained at the very
          * TOP of tick N+1, its own vblank window, with nothing else queued that tick
          * (ANIM_PERIOD is 30, so ticks N and N+1 never both trigger a NEW swap). A
          * no-op whenever nothing is pending, so unconditional every tick costs nothing
          * on a full-art build (s_pend is never 1 there) or on a tick with no pending
          * half. */
         boxoam_pose_pump();
         /* The HAND bounces on a 30/30 cadence (capture doc §1a) — and so does the
          * grid, by 1 px in unison.
          *
          * 0a8f51f added the hand bounce and dropped the grid's own idle, which left
          * boxoam_set_bob() with ZERO callers: the machinery was all still here and
          * consistent (place_grid_slot adds s_bob to every y it writes; boxoam_enter
          * resets it) but nothing ever drove it, so 30 icons sat frozen while a single
          * 32x32 glove changed pose. Guy read that as the box animation being broken,
          * which it was — the comment above this loop and box_oam.c's own header had
          * been describing a bob that did not run.
          *
          * boxoam_set_bob is the right driver rather than boxoam_set_frame (the real
          * 2-frame pose swap) ONLY on a slot that cannot pose-swap at all — box_oam.c
          * now does BOTH per slot: compiled art and a ROM registered with cheap_reads
          * (a fused cart-space memcpy) get the real swap, every other slot (icons.bin
          * cache, or an SD-registered ROM) keeps the 1 px OAM nudge. The 2026-08-19
          * claim that the swap "cannot fit the vblank window" was about the VERIFIED
          * re-stage path (upload_tiles_verified), not this plain-DMA one — see
          * box_oam.c's boxoam_set_frame header. Paused while carrying / dragging /
          * ITEM mode. */
         if (app_anim_enabled(ANIM_BOX) && !s_holding && !s_ch_hold && s_cur_mode != CM_ITEM) {
           if (++anim_ctr >= ANIM_PERIOD) {
             anim_ctr = 0; bob ^= 1;
             /* The box's own bob tick, rolled up under "bob.box". This loop never goes
              * through pdna_main.c's wait_keys_bob_p helper (whose comment used to claim
              * it covered the box), so it needs its own bracket -- and it is the widest
              * ICON bob that can carry real per-tick SD I/O: up to 30 occupied slots,
              * each one a pose_swap_rom_slot() fetch on the artless build's ROM rung.
              * boxoam_exit() already flushes PERF_REP_BOB on all ~9 of this screen's
              * exit paths, so nothing else is needed to make the line land. */
             perf_rep_begin(PERF_REP_BOB, "bob.box");
             boxoam_hand_pose(bob ? BOXOAM_POSE_BOUNCE : BOXOAM_POSE_NORMAL);
             /* The REAL animation first: a 2-frame pose swap, which is what Gen 3
              * actually does and what "animated" means. It only fails whole-box on a
              * box with NO pose-capable slot at all (every occupied icon is on the
              * icons.bin cache or an SD-registered ROM) — there, and only there, fall
              * back to nudging every sprite 1 px so the grid is not dead. Guy's words
              * for the fallback on its own were "not animated, just jumping up and
              * down" — exactly right, which is why it is now the fallback and not the
              * animation. Any ODD cheap-ROM slots this call queues get drained by
              * boxoam_pose_pump() at the TOP of the *next* iteration (see above), not
              * this one. */
             if (!boxoam_set_frame(bob)) boxoam_set_bob(bob);
             int tr = cursor_title_row(on_title);
             boxoam_cursor(cur, tr, cursor_look(), cursor_label_cx(tr));
             perf_rep_end(PERF_REP_BOB);
           }
         } else if (bob) { bob = 0; boxoam_hand_pose(BOXOAM_POSE_NORMAL);
                           if (!boxoam_set_frame(0)) boxoam_set_bob(0);   /* settle the grid */
                           int tr = cursor_title_row(on_title);
                           /* BACKLOG #150 S150-13, review D1: NOT while s_holding -- this
                            * settle-the-grid beat can fire the instant a carry begins (bob
                            * was mid-animation when MOVE picked the cell up), and
                            * boxoam_cursor() -> load_rega_hand() re-uploads the hand pose
                            * over TID_HAND (region A), clobbering the carry badge's own
                            * tiles there (boxoam_carry_badge reuses that region while it is
                            * otherwise idle -- see its own comment) and re-showing OE_HAND,
                            * which hides OE_GRAB/OE_CARRY outright. The carry render arm
                            * (the `else if (s_holding)` branch above) already draws its own
                            * cursor/fist/badge every frame; this settle beat has nothing to
                            * do while holding. */
                           if (!s_holding)
                             boxoam_cursor(cur, tr, cursor_look(), cursor_label_cx(tr)); }
         boxoam_commit();                       /* flush the OAM shadow in the vblank window */
         fresh = key_hit(KEY_FULL);
         k = fresh | key_repeat(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } while (!k);
    /* fresh-press earcons (held d-pad repeats stay silent) */
    if      (fresh & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
    else if (fresh & (KEY_L | KEY_R | KEY_SELECT))               snd_tab();
    else if (fresh & (KEY_A | KEY_START))                        snd_ok();
    else if (fresh & KEY_B)                                      snd_back();

    int old_cur = cur; bool old_title = on_title;

    /* WP AUDIT hotkey (hold START+SELECT together): the on-hardware discrimination
     * for the garbled-wallpaper hunt — run it WHILE the garble is on screen. */
    if (key_is_down(KEY_START) && key_is_down(KEY_SELECT) && !s_ch_hold && !s_holding) {
      wp_audit(src, box);
      need_full = true;
      continue;
    }

    /* ---- CHUNK CARRY (multi-select group in hand): move the footprint; drop places it ---- */
    if (s_ch_hold) {
      if (k & KEY_B) {                               /* cancel: sources kept in place, nothing lost */
        s_ch_hold = false; s_oam_reload = true; need_full = true;
      }
      else if (k & KEY_A) { recs = drop_chunk(src, box, recs, &need_full); }
      else if ((k & (KEY_L | KEY_R)) && nb > 1) {    /* carry to prev/next box */
        int nbx = (k & KEY_R) ? (box + 1) % nb : (box + nb - 1) % nb; SWITCH_BOX(nbx);
      }
      else if (k & KEY_LEFT)  { if (s_ch_tc > 0) s_ch_tc--; else if (nb > 1) SWITCH_BOX((box + nb - 1) % nb); }
      else if (k & KEY_RIGHT) { if (s_ch_tc < chunk_anchor_cmax(&s_ch)) s_ch_tc++; else if (nb > 1) SWITCH_BOX((box + 1) % nb); }
      else if (k & KEY_UP)    { if (s_ch_tr > 0) s_ch_tr--;
                                /* §2.3: this edge does NOT gain `bank_edge` -- a GB chunk
                                 * (post-can_lift, a future slice) still cannot hop UP into
                                 * the Bank; only the single-carry UP edge is wired for that. */
                                else if (!src->is_bank) { app_box_resume_note(box, cur); boxoam_exit(); return 4; }    /* up past PC top -> Bank */
                                else snd_deny(); }
      else if (k & KEY_DOWN)  { if (s_ch_tr < chunk_anchor_rmax(&s_ch)) s_ch_tr++;
                                /* §2.3: denied whenever an xfer peer is installed (always
                                 * NULL in S1 -- pdna_box_xfer_set() has no caller yet) so a
                                 * Bank chunk can never cross DOWN into the GB grid it opens
                                 * onto once a later slice installs the peer around a Bank
                                 * visit reached from a Game Boy session. */
                                else if (src->is_bank && !s_xfer_peer) { app_box_resume_note(box, cur); boxoam_exit(); return 5; }     /* off Bank bottom -> PC */
                                else snd_deny(); }
      if (!s_ch_hold) boxoam_chunk_end();            /* B-cancel / successful drop: restore the
                                                      * borrowed regions before the full repaint */
      if (s_ch_hold && !need_full) chunk_move(src, box, recs);   /* anchor move: pure OAM, no bitmap */
      continue;                                      /* chunk carry swallows all other keys */
    }

    /* ---- MOVE MODE (mon-in-hand): the carried mon floats; place it anywhere ---- */
    if (s_holding) {
      bool homeless = (s_orig_slot < 0 && !s_held_dup);   /* a real mon displaced by a swap (RAM-only) */
      if (s_tab_focus >= 0) {                        /* carrying with the cursor up on the top tabs */
        if (k & KEY_LEFT)       { s_tab_focus = (s_tab_focus > 0) ? s_tab_focus - 1 : 2; need_full = true; }
        else if (k & KEY_RIGHT) { s_tab_focus = (s_tab_focus + 1) % 3; need_full = true; }
        else if (k & (KEY_B | KEY_DOWN)) { s_tab_focus = -1; need_full = true; }   /* back to the grid, still holding */
        else if (k & KEY_UP) {                       /* up past the PC tabs -> Bank, still holding */
          /* a party-origin carry stays in the PC (its undo = drop it / B returns it to the party) */
          if (!src->is_bank || src->bank_edge) { if (homeless || s_orig_party) snd_deny(); else { s_tab_focus = -1; app_box_resume_note(box, cur); boxoam_exit(); return 4; } }
        }
        else if (k & KEY_A) {
          if (s_tab_focus == 1 && !src->is_bank && !s_orig_party) {  /* PARTY tab: place/swap the held box mon into the party
                                                        * (CHANGE 2: `cur` is now &cur -- the
                                                        * popup's own GRID focus, if the user
                                                        * crosses into it, can move the box
                                                        * cursor and hands the new position
                                                        * back on close) */
            int rr = party_strip_overlay(src, box, &cur, 0, 0, false);
            if (rr == 1) {                            /* placed -> end the carry (party_strip_overlay
                                                        * already cleared s_holding/s_orig_* itself,
                                                        * CHANGE 2 -- this is belt-and-braces) */
              s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_scope = BOXSCOPE_PC; s_held_dup = false; s_orig_party = false;
              s_tab_focus = -1; recs = src->records(box); box_decode(src, recs, box);
            }
            /* rr==0: closed without placing -- either the untouched original carry (B) or
             * a mon grabbed via GRID focus and then B'd out of; s_holding already reflects
             * whichever is true, unconditionally checked next iteration same as any carry */
            s_oam_reload = true; need_full = true;    /* redraw over the popup */
          } else snd_deny();                          /* party-origin mon can't go back; PKMN DATA / SAVE locked while carrying */
        }
        continue;
      }
      if (k & KEY_B) {                               /* cancel */
        if (homeless) {                              /* must place it somewhere -> first free in this box */
          /* BACKLOG #150 S150-3 decision 3: a native homeless carry is unreachable today
           * (DUPLICATE is dominated by drop_held's own gate and a native cell cannot be
           * in the party), but losing it would be a data-loss bug -- refuse, never
           * discard, the same fail-safe as the box-full `fs < 0` case right below. */
          if (xg_native_escape_denied(s_held, src->scope)) { snd_deny(); }
          else {
          int fs = -1; for (int s = 0; s < COLS * ROWS; s++) if (!g_box[s].species) { fs = s; break; }
          if (fs < 0) { snd_deny(); }                /* box full: keep holding */
          else { memcpy(recs + (uint32_t)fs * 80, s_held, 80); src->mark_dirty();
                 snd_back(); s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_scope = BOXSCOPE_PC; s_held_dup = false; s_orig_party = false;
                 box_decode(src, recs, box); s_oam_reload = true; need_full = true; }
          }
        } else {                                     /* origin keeps it (party / box / dup) -> nothing to place */
          snd_back(); s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_scope = BOXSCOPE_PC; s_held_dup = false; s_orig_party = false;
          s_oam_reload = true; need_full = true;
        }
      }
      else if (k & KEY_A) {                          /* drop / swap onto the cursor cell */
        play_place_anim_down(src, box, cur);         /* fist + mon settle onto the cell */
        bool done; recs = drop_held(src, box, cur, recs, &done);
        (void)done;                                  /* a cross-scope copy may have shown a confirm dialog */
        box_decode(src, recs, box); s_oam_reload = true;
        play_place_anim_up(src, box, cur);           /* open hand (or swapped mon) rises */
        need_full = true; paint_over = true;         /* repaint OVER: no black flash */
      }
      else if ((k & (KEY_L | KEY_R)) && nb > 1) {    /* carry to the next/prev box (even a FULL one) */
        int nbx = (k & KEY_R) ? (box + 1) % nb : (box + nb - 1) % nb;
        SWITCH_BOX(nbx);                             /* held mon floats along; no slot needed */
      }
      else if (k & KEY_LEFT)  cur = grid_lr_step(cur, cap, false);   /* BACKLOG #200 F2 */
      else if (k & KEY_RIGHT) cur = grid_lr_step(cur, cap, true);    /* BACKLOG #200 F2 */
      else if (k & KEY_UP)    {
        if (cur >= COLS) cur -= COLS;
        /* BACKLOG #171: a GB source is is_bank+bank_edge, so the carry must be allowed to enter tab focus */
        else if (!src->is_bank || src->bank_edge) { s_tab_focus = 1; need_full = true; }  /* off PC top -> top tabs (PARTY), still holding */
      }
      else if (k & KEY_DOWN)  {
        /* BACKLOG #200 F2: `cur + COLS >= cap` (not the physical bottom row) is
         * "no real cell below" -- once blocked in a column every cell further
         * down it is blocked too (index only grows going down), so there is
         * nothing to skip TO; treat it the same as the physical-bottom case. */
        if (cur + COLS < cap) cur += COLS;
        else if (homeless) snd_deny();                          /* place the swapped mon before leaving */
        else if (src->is_bank && cur + COLS >= COLS * ROWS) { app_box_resume_note(box, cur); boxoam_exit(); return 5; }     /* off the PHYSICAL Bank bottom -> PC, still holding; a capacity edge stays put (b200 review A1) */
      }

      /* cursor move while carrying -> partial redraw (no ui_clear), so it doesn't flicker */
      if (!need_full && cur != old_cur) carry_move(src, box, old_cur, cur);
      continue;                                      /* move-mode swallows all other keys */
    }

    /* ---- ITEM CARRY: holding a held item; place it / swap onto another mon ---- */
    if (s_item_held > 0) {
      if (k & KEY_B) {                               /* put it back (never lose it) */
        if (s_item_from_box >= 0 && s_item_from_box != box) SWITCH_BOX(s_item_from_box);  /* back to its box */
        int home = item_home(recs);
        /* Park the cursor on the mon that just got the item back, so the left panel
         * shows where it went. (SWITCH_BOX now PRESERVES the cursor cell, so without
         * this a cross-box put-back would leave the glove on an unrelated slot.) */
        if (home >= 0) { cur = home;
                         if (box_set_held(recs, home, (uint16_t)s_item_held)) {
                           box_decode(src, recs, box);
                           src->mark_dirty(); s_item_held = 0; s_item_from = -1; s_item_from_box = -1; need_full = true;
                         } }
        else snd_deny();
      }
      else if (k & KEY_A) {                          /* give / swap onto the cursor mon */
        if (g_box[cur].species) {
          uint16_t old = g_box[cur].heldItem;        /* swap: take this mon's old item */
          if (box_set_held(recs, cur, (uint16_t)s_item_held)) {
            box_decode(src, recs, box);
            src->mark_dirty();
            s_item_held = old; s_item_from = old ? cur : -1;   /* keep holding the swapped-out item */
            s_item_from_box = old ? box : -1;
            need_full = true;
          }
        } else snd_deny();
      }
      else if ((k & KEY_L) && nb > 1) { SWITCH_BOX((box + nb - 1) % nb); }   /* flip boxes while carrying */
      else if ((k & KEY_R) && nb > 1) { SWITCH_BOX((box + 1) % nb); }
      else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
      else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
      else if (k & KEY_UP)    { if (cur >= COLS) cur -= COLS; }
      else if (k & KEY_DOWN)  { if (cur < COLS * (ROWS - 1)) cur += COLS; }
      /* carried item + cursor are sprites: just reposition them (no BG repaint) */
      if (!need_full && cur != old_cur) { draw_left(&g_box[cur]); oam_sync(cur, false, box, src->is_bank); }
      continue;                                      /* item-carry swallows all other keys */
    }

    /* ---- TOP-TAB cursor (reached by pressing UP on the box name): pick a tab ---- */
    if (s_tab_focus >= 0) {
      if      (k & KEY_UP) { if (!src->is_bank || src->bank_edge) { s_tab_focus = -1; app_box_resume_note(box, cur); boxoam_exit(); return 4; } }   /* up past the PC tabs -> Bank */
      else if (k & (KEY_B | KEY_DOWN)) { s_tab_focus = -1; need_full = true; }              /* back to box name */
      else if (k & KEY_LEFT)  { s_tab_focus = (s_tab_focus > 0) ? s_tab_focus - 1 : 2; need_full = true; }
      else if (k & KEY_RIGHT) { s_tab_focus = (s_tab_focus + 1) % 3; need_full = true; }
      else if (k & KEY_A) {
        if (s_tab_focus == 0) { s_tab_focus = -1; on_title = false; need_full = true; }      /* PKMN DATA -> grid */
        else if (s_tab_focus == 1) {                                                          /* PARTY -> overlay popup */
          /* 2026-08-23: pcp_open_party_strip already resets s_tab_focus to -1 on every
           * exit path (placed, grabbed, or just closed), same as the PKMN DATA branch
           * above -- but unlike that branch it never touched on_title, which this whole
           * dispatch is nested INSIDE (entered from "if (s_tab_focus >= 0)", itself only
           * reachable while on_title was true). Left uncleared, the very next LEFT/RIGHT/
           * A on what LOOKS like a plain grid cursor was actually still routed through
           * the TITLE ROW's own handler below (box-switch on LEFT/RIGHT, the BOX OPTIONS
           * popup on A instead of a grab/menu) until the player happened to press DOWN
           * once -- silently breaking the exact "grab a mon" gesture this session's other
           * fix (party_strip_overlay's GRID-focus CM_MOVE) exists to make reliable, on
           * literally every use of the PARTY tab (which is how that popup is reached).
           * The PKMN DATA tab already got this right one branch up; PARTY just never
           * matched it. */
          pcp_open_party_strip(src, box, &cur, &need_full);
          on_title = false;
        }
        else { s_tab_focus = -1; app_box_resume_note(box, cur); boxoam_exit(); return 0; }                                   /* SAVE -> exit (save prompt) */
      }
      continue;
    }

    if (k & KEY_B) { if (s_cur_mode != CM_NORMAL && !on_title) { s_cur_mode = CM_NORMAL; need_full = true; } else { app_box_resume_note(box, cur); boxoam_exit(); return 0; } }
    else if ((k & KEY_START) && (!src->is_bank || src->has_start)) { app_box_resume_note(box, cur); boxoam_exit(); return 2; }  /* BACKLOG #48: a GB session's own box sets has_start to opt back in */
    else if (k & KEY_L) { SWITCH_BOX((box + nb - 1) % nb); }
    else if (k & KEY_R) { SWITCH_BOX((box + 1) % nb); }
    else if (k & KEY_SELECT) {
      /* This branch sits ABOVE `on_title` in the else-if chain (it has to: SELECT is
       * also the grid's cursor-mode cycle, tested every iteration regardless of row),
       * so on_title's own KEY_SELECT never gets a chance to run -- SELECT must be
       * handled HERE for the title row, not down there. (Before this change, that
       * meant the title row silently ate SELECT: `!on_title` was false so it fell to
       * `else snd_deny()` and did nothing but beep -- a real, load-bearing dispatch-
       * order fact, not a hypothetical collision, confirmed by reading this chain.) */
      if (on_title) {                                /* TITLE row: SELECT opens the box menu
                                                        * (rename/wallpaper/export/release —
                                                        * box_options_menu, unchanged; rename
                                                        * there is now a second path to the
                                                        * same osk_input as the direct-A one) */
        /* BACKLOG #93: a source whose box menu is unreachable through the ordinary
         * can_lift/can_edit capability (the finding that shapes this step -- GB
         * hardwires can_edit false and can_lift is S3's transfer field, neither means
         * "the box options menu may open") narrows the gate with can_boxops instead;
         * NULL falls back to src_can_lift exactly as before this field existed. */
        if (src->can_boxops ? src->can_boxops(box) : src_can_lift(src, box, -1)) {
                               boxoam_suspend(); box_options_menu(src, box); boxoam_resume();
                               recs = src->records(box); box_decode(src, recs, box);  /* Release all mutates records */
                               s_oam_reload = true; need_full = true; }
        else snd_deny();
      }
      /* BACKLOG #150 S150-5 decision 8(b): a GB grid cycles NORMAL<->MOVE only -- CM_ITEM's
       * take-the-held-item branch mutates the SYNTHESISED Gen-3 grid via box_set_held() +
       * mark_dirty() and can never persist (a UX lie), now that can_lift going true
       * switches on machinery that was unreachable before this lane. */
      /* BACKLOG #187/#192, F1: leaving a non-NORMAL mode is now UNCONDITIONAL (the
       * `s_cur_mode != CM_NORMAL` clause) -- before this fix, moving the cursor onto
       * an empty cell while in MOVE (or ITEM) mode made src_can_lift(box,cur) false
       * (gbs_can_delete refuses slot>=count on an empty GB cell), so the SAME gate
       * that ADMITS a mode also, wrongly, GATED leaving it -- SELECT there just beeped
       * forever (#192: "stuck on orange (grab)"). Entering (NORMAL -> anything) still
       * needs a real capability check, but now box-level (can_enter_move, falling
       * back to the old per-cell src_can_lift when a source leaves it NULL -- every
       * source before this field existed, PC/Bank included, is byte-identical): the
       * per-cell question ("can THIS one mon be lifted") stays exactly where it
       * belongs, the actual lift on A (src_can_lift is still called there,
       * untouched). */
      else if (s_cur_mode != CM_NORMAL ||
               (src->can_enter_move ? src->can_enter_move(box) : src_can_lift(src, box, cur))) {
        s_cur_mode = (s_cur_mode + 1) % (src->scope == BOXSCOPE_GB ? 2 : 3); need_full = true;
      }  /* cycle cursor mode (Omega-only edit modes) */
      else snd_deny();
    }
    else if (on_title) {                           /* TITLE row: limited controls */
      if (k & KEY_DOWN) on_title = false;
      else if (k & KEY_UP) { s_tab_focus = src->is_bank ? 2 : 1; need_full = true; }   /* up into the top tabs */
      /* LEFT/RIGHT on the box name flips boxes, like the real Gen-3 PC. FRESH presses
       * only: retail reads JOY_HELD here but is rate-limited by its 32-frame scroll
       * animation, whereas the Bank pages boxes off the SD card on every switch
       * (banksrc_records saves the dirty box then loads the next), so a held flip
       * would hammer the card. `on_title` stays true by construction — SWITCH_BOX
       * does not touch it — so it is not re-asserted here. */
      else if (fresh & KEY_LEFT)  { SWITCH_BOX((box + nb - 1) % nb); }
      else if (fresh & KEY_RIGHT) { SWITCH_BOX((box + 1) % nb); }
      else if (k & KEY_A) {
        /* A on the banner renames the box directly (Guy, BACKLOG #33): the rest of
         * box_options_menu (wallpaper/export/release) moved to SELECT, below, since
         * rename is the action a player reaches for from the name itself. This is
         * box_options_menu's OWN rename case (sel==0), lifted verbatim rather than
         * shared by a helper -- it is five lines and the menu's copy must stay able to
         * `return` mid-loop on its own, so a shared function would need an out
         * parameter for no real gain. `cur`/`buf` are LOCAL to this block only: this
         * function's outer `cur` is the int grid cursor, so a same-named `char cur[12]`
         * here would shadow it -- named `bxname`/`bxnew` instead to keep that impossible. */
        if (src->can_rename ? src->can_rename() : src_can_lift(src, box, -1)) {
          boxoam_suspend();                                              /* full-screen sub-view — own bracket, see box_oam.h */
          /* F1b: seed with the RAW stored name, not get_name()'s display string --
           * get_name() prefixes "GB " only for Gen-1's synthesized names; Gen-2 names
           * are echoed as-is (BACKLOG #122). The raw name must never be typed back
           * with a prefix into the save. */
          char bxname[12];
          if (src->get_raw_name) src->get_raw_name(box, bxname); else src->get_name(box, bxname);
          char bxnew[12];
          if (osk_input("BOX NAME", bxname[0] ? bxname : "BOX", bxnew, 9)) {
            src->set_name(box, bxnew);
            src->commit();
          }
          boxoam_resume();
          recs = src->records(box); box_decode(src, recs, box);          /* belt-and-braces refresh, same as the menu path */
          s_oam_reload = true; need_full = true;
        }
        /* BACKLOG #244 (b199-fixes2, corrected design): can_rename() above is false
         * for THREE different reasons -- no box-name table (Gen 1), an EverDrive
         * (app_can_edit() false, hard rule 4), or a hack-flagged/bad ROM -- and only
         * the first of those is "this game has no box names". The review's first
         * prescription (`src->can_edit && !src->can_edit()`) does not distinguish
         * them EITHER: for a GB source can_edit is hardwired false unconditionally
         * (gbsrc_can_edit, pdna_gen12.c) and can_rename is non-NULL, so that branch
         * always fired for every GB save and swallowed Gen 1's genuine case (proven
         * live on a Red save: A produced no frame change at all). The predicate that
         * actually says the dialog's sentence is box_names_supported() (NULL means
         * yes -- Gen-3 PC/Bank always have a table): explain ONLY when the table
         * itself is missing; every other refusal (not writable right now) stays the
         * shipped-silent beep, exactly as main's own box_options_menu path does. */
        else if (src->box_names_supported && !src->box_names_supported()) {
          snd_deny();
          boxoam_suspend();                                              /* full-screen dialog — same bracket as the rename branch above */
          msg_wait("NO BOX NAMES", UI_WARN, "This game has no box names.", 0);
          boxoam_resume();
        }
        else {
          snd_deny();                 /* not writable -- stay silent, exactly as main did */
        }
      }
    }
    else if (k & KEY_LEFT)  cur = grid_lr_step(cur, cap, false);   /* BACKLOG #200 F2 */
    else if (k & KEY_RIGHT) cur = grid_lr_step(cur, cap, true);    /* BACKLOG #200 F2 */
    else if (k & KEY_UP)    { if (cur < COLS) on_title = true; else cur -= COLS; }
    /* BACKLOG #200 F2: `cur + COLS >= cap` replaces the physical-bottom-row test
     * `cur >= COLS*(ROWS-1)` on BOTH branches below -- identical to the old test
     * whenever cap==30 (every Gen-3 PC/Bank source, box_cap()'s NULL fallback),
     * and on a smaller-capacity GB source it fires as soon as the NEXT row down
     * in this column would be blocked, since (established above) everything
     * below a blocked cell in the same column is blocked too -- there is
     * nothing further to skip to. The wrap target `cur - COLS*(ROWS-1)` is
     * `cur % COLS` (row 0, same column), always real by the same argument. */
    else if (k & KEY_DOWN)  { if (src->is_bank && cur + COLS >= COLS * ROWS) { app_box_resume_note(box, cur); boxoam_exit(); return 5; }   /* off the PHYSICAL bank bottom -> PC tabs; a capacity edge mid-grid (GB source) wraps below instead (b200 review A1) */
                              else cur = (cur + COLS >= cap) ? cur % COLS : cur + COLS; }
    else if ((k & KEY_A) && s_cur_mode == CM_MOVE) {     /* orange hand: TAP = grab one; HOLD+DPAD = rubber-band multi-select */
      if (!src_can_lift(src, box, cur)) snd_deny();
      else recs = begin_select(src, box, recs, cur, &need_full);
    }
    else if ((k & KEY_A) && s_cur_mode == CM_ITEM) {     /* transparent hand: pick up the held item */
      if (!src_can_lift(src, box, cur)) snd_deny();
      else if (g_box[cur].species && g_box[cur].heldItem) {
        /* BACKLOG #150 S150-3 review F1: structurally unreachable on a native cell
         * already (it always decodes heldItem == 0), but box_set_held is still the
         * gate of record -- read the item BEFORE the write attempt, same ordering
         * as the GIVE/swap site's `old = g_box[cur].heldItem` above. */
        uint16_t item = g_box[cur].heldItem;
        if (box_set_held(recs, cur, 0)) {
          s_item_held = item; s_item_from = cur; s_item_from_box = box;
          box_decode(src, recs, box);
          src->mark_dirty();
          play_item_grab_anim(cur, (uint16_t)s_item_held);   /* fist closes over the mon (grab beat) */
          need_full = true;
        }
      } else snd_deny();                                 /* empty slot or no item */
    }
    else if (k & KEY_A) {
      /* BACKLOG #200 F3: defensive backstop -- F2 already keeps the cursor off
       * every blocked cell, so this should be unreachable, but A on one (index
       * >= cap) must never open the EMPTY/CREATE/CANCEL menu: there is no real
       * slot here to create into. Same one-line-dialog shape the chunk-carry
       * BANK WRITE FAILED popup above uses (ui_panel + a text row + wait for A). */
      if (cur >= cap) {
        snd_deny();
        ui_clear();
        ui_panel(20, 60, 200, 44, UI_PANEL, UI_WARN);
        ui_ptext_fit(26, 70, 188, UI_WARN, PDNA_BOX_NO_SLOT);
        ui_text(30, 86, UI_DIM, "Press A");
        u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
        need_full = true;
        continue;
      }
      /* NORMAL: open the action menu on an occupied slot, or on an empty slot when
       * editable (CREATE a mon, or PASTE if the clipboard holds one) -- or, S5-B review
       * fix (BLOCKING #1), when a read-only GB source is offering PASTE (GB): that
       * source's can_edit() is always false, so without this OR an empty GB cell never
       * opened the menu PASTE (GB) lives in at all. */
      if (g_box[cur].species || src->can_edit() || app_src_empty_action_offered()) {
        uint8_t* rec = recs + (uint32_t)cur * 80;
        int mbox = src->is_bank ? 0 : box;                               /* box index within menu_block */
        boxoam_suspend();                                                /* sprites off while the menu/summary is up */
        app_mon_menu(rec, false, src->is_bank, src->commit, src->menu_block, mbox, cur, UI_FOOTER_Y);
        boxoam_resume();
        recs = src->records(box);                                        /* menu may have edited it */
        box_decode(src, recs, box);                                  /* refresh after possible write */
        s_oam_reload = true;                                             /* contents may have changed */
        if (app_take_move_request()) {                                   /* picked MOVE -> into the glove */
          /* BACKLOG #150 S150-4/5: the SECOND way to reach start_carry with a
           * BOXSCOPE_GB source (the first is the CM_MOVE grab above, begin_select) --
           * this menu's MOVE row is offered on ANY occupied, non-native, non-party
           * cell regardless of scope (pdna_main.c's app_mon_menu row builder never
           * sees `src`), so a Gen-1/2 cell a Gen-1-one-mon-party/Mail-holding-party
           * can_lift refuses must be guarded here too, or a cell this game's own
           * rules forbid lifting at all (party floor, a Mail-holding party) would
           * start a carry anyway -- the same SILENT-but-honest refusal decision 8's
           * grab-site guards use (the menu has already closed here, so the grid is
           * visible underneath, same as the CM_MOVE site's own refusal reads).
           * render_full/play_grab_anim/carry_move/draw_footer must only run when the
           * carry actually started -- moved into the else branch so a refusal leaves
           * nothing held and no grab animation plays. */
          /* src_can_lift() is the grab-time gate (parity with the CM_MOVE site above);
           * start_carry()'s own return is still checked, same defensive shape as
           * begin_select's single-tap grab -- BACKLOG #199: start_carry() no longer
           * calls lift_up() (moved to drop_held_up, the Bank-UP drop), so it cannot
           * fail here any more on a valid cell, but the check costs nothing to keep. */
          if (!src_can_lift(src, box, cur) || !start_carry(src, recs, box, cur)) {
            snd_deny();
          } else {
            render_full(src, box, cur, false, false, false);             /* repaint OVER the menu, no black flash */
            play_grab_anim(src, box, cur);                               /* grab cue (OAM lift) */
            carry_move(src, box, cur, cur);                              /* lift into carry */
            draw_footer(src->is_bank, false, true);                      /* move-mode footer */
          }
        } else if (app_take_dup_request()) {                            /* picked DUPLICATE -> a fresh COPY in the glove */
          memcpy(s_held, recs + (uint32_t)cur * 80, 80);                /* copy floats in-hand; no origin (cancel discards it) */
          s_holding = true; s_orig_box = box; s_orig_slot = -1; s_orig_scope = src->scope; s_held_dup = true; s_orig_party = false;
          render_full(src, box, cur, false, false, false);
          play_grab_anim(src, box, cur);
          carry_move(src, box, cur, cur);
          draw_footer(src->is_bank, false, true);
        } else {
          int pb, ps;
          if (app_take_pickup(&pb, &ps) && pb >= 0 && pb < nb && ps >= 0 && ps < 30) {
            box = pb; recs = src->records(box); box_decode(src, recs, box);   /* TO DAY-CARE->PC: carry the parked mon */
            cap = box_cap(src, box);   /* BACKLOG #200 F2: keep `cap` in lockstep with
                                        * `box` at every reassignment, same as SWITCH_BOX --
                                        * day-care is Gen-3-only so this is always 30, but
                                        * a stale cap here would be a live trap for later */
            /* BACKLOG #150 S150-4: return ignored -- same day-care-is-Gen-3-only
             * reasoning as the pdna_box() entry-point pickup above; PC-only. */
            cur = ps; (void)start_carry(src, recs, box, ps); s_oam_reload = true;
            render_full(src, box, cur, false, false, false);
            play_grab_anim(src, box, cur);
            carry_move(src, box, cur, cur);
            draw_footer(src->is_bank, false, true);
          } else {
            need_full = true;                                            /* menu may have edited -> redraw */
          }
        }
      }
    }

    /* cursor-only change -> light update: reposition the cursor sprite + refresh the
     * left panel (and banner/footer if the title row changed). Icons/markers are
     * sprites the GPU composites, so there is nothing to erase or repaint. */
    if (!need_full && (cur != old_cur || on_title != old_title))
      move_cursor(src, box, old_cur, old_title, cur, on_title);
  }
  #undef SWITCH_BOX
}
