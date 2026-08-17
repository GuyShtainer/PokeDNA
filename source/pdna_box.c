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
#include "ui.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"      /* em_set_item: held-item moves in ITEM cursor mode */
#include "data_tables.h"
#include "mon_front.h"
#include "mon_icons.h"
#include "mon_icons_oam.h"   /* artless probe: are the OAM icons compiled in? */
#include "item_icons.h"     /* item_icon_for: held-item markers in ITEM mode */
#include "box_oam.h"        /* hardware-OAM icon/cursor/carry/marker rendering */
#include "pdna_origin_art.h" /* THE BANK IN PARALLEL: each cell in the art of its own era */
#include "pdna_summary.h"
#include "pdna_app.h"
#include "snd.h"
#include "osk.h"
#include "rumble.h"         /* rumble_io_suspend/resume: mute the motor while the wallpaper blit reads ROM */
#include "log.h"            /* log_line: wallpaper self-verify diagnostics */
#include "gen3_chunk.h"     /* Chunk: Emerald-style rubber-band multi-select geometry */
#include "pdna_progress.h"  /* pdna_progress_frame: batch sprite + N/total bar */
#include "pdna_pk.h"        /* pdna_pk_export_silent: "Export all" from the box title */

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

/* Decode a box's raw records for DISPLAY. For the bank, a mon the user already carried out to the
 * PC is deleted from the card only at the save prompt — but it must LOOK gone right away (Guy), so
 * blank those slots here. This is display-only: the raw buffer keeps the record (it's the mon's only
 * on-card copy until the PC is written), so box_save can never persist a half-done move. */
static void box_decode_to(BoxSource* src, const uint8_t* recs, int box, PkMon out[G3_BOX_SLOTS]) {
  pk_decode_box_raw(recs, out);
  if (src->is_bank) app_bank_hide_pending(box, out);
}
static void box_decode(BoxSource* src, const uint8_t* recs, int box) {
  box_decode_to(src, recs, box, g_box);
  /* THE ERA CACHE IS FILLED FROM WHAT IS ACTUALLY ON SCREEN. This is the single point
   * where the 30 displayed records change, in the PC and in the bank alike, so it is
   * the one place that can promise "the markers describe the mons you are looking at".
   * Filling it from the RAW records instead (which is where it used to be done, in
   * pdna_bank.c) put a Game Boy marker on a bank slot the user had already carried out
   * to the PC: box_decode_to blanks those for display, and the raw buffer still holds
   * them because that record is the mon's only on-card copy until the PC is written.
   * Cost is 30 record decodes + 30 integer comparisons on a user action, never a frame. */
  pdna_origin_box_note(g_box);
}

/* Occupancy for DROP targeting. A bank slot pending a Bank->PC deletion looks empty (box_decode
 * hides it) but still physically holds the mon's ONLY on-card copy, so it counts as OCCUPIED —
 * nothing may overwrite it until the PC destination is saved (after that the slot frees for real). */
static void box_occupancy(BoxSource* src, int box, uint8_t occ[G3_BOX_SLOTS]) {
  for (int s = 0; s < G3_BOX_SLOTS; s++)
    occ[s] = (g_box[s].species || (src->is_bank && app_bank_slot_pending(box, s))) ? 1 : 0;
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
static bool s_orig_bank = false;
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
static bool s_ch_bank = false;                          /* source scope (bank vs PC) */
static int  s_ch_tr = 0, s_ch_tc = 0;                   /* current carry anchor (top-left) */
static int  s_ch_fr = 0, s_ch_fc = 0;                   /* grab fist's footprint-relative cell
                                                         * (where A was released, Emerald-style) */
static int  s_ch_lift = 8;                              /* block float height in px: 8 = carrying;
                                                         * the grab/place beats animate it 0..8 */
static BoxOamChunkMon EWRAM_BSS s_ch_cells[G3_BOX_SLOTS]; /* per-mon display info, decoded ONCE at
                                                           * grab so anchor moves don't re-decode */

void pdna_box_clear_carry(void) {
  s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_bank = false; s_held_dup = false; s_orig_party = false;
  s_ch_hold = false; s_ch_box = -1; s_ch_bank = false;
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

/* tan rounded box-name banner with arrows */
static void draw_banner(int x, int y, int w, const char* name) {
  const u16 fill = RGB15(30, 27, 14), hi = RGB15(31, 31, 24), sh = RGB15(24, 15, 2),
            bd = RGB15(14, 9, 0), ink = RGB15(8, 5, 0);
  ui_fill_rect(x, y, w, 14, fill);
  ui_hline(x, y, w, hi);
  ui_hline(x, y + 13, w, sh);
  m3_frame(x, y, x + w - 1, y + 13, bd);
  tri_left(x + 5, y + 4, ink);
  tri_right(x + w - 9, y + 4, ink);
  /* Proportional, and centred on its MEASURED width: the cursor hand parks in the left
   * gutter of this banner when the box name is selected, so the text has to stay clear of
   * it even for an 8-character box name plus "  30/30". */
  int tw = ui_ptext_w(name);
  int tx = x + (w - tw) / 2; if (tx < x + 24) tx = x + 24;
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
  if (p->isEgg && !p->isBadEgg) {                        /* an Egg reads as the Egg sprite */
    const uint16_t* eg = mon_front_egg();
    rumble_io_resume();
    if (eg) ui_sprite(6, 16, MON_FRONT_W, MON_FRONT_H, eg);
    else    ui_sprite(22, 32, MON_ICON_W, MON_ICON_H, mon_icon_egg());
  } else {
    const uint16_t* spr = mon_front_for_form(p->species, p->isShiny, p->form);
    rumble_io_resume();
    if (spr) ui_sprite(6, 16, MON_FRONT_W, MON_FRONT_H, spr);
    else     ui_sprite(22, 32, MON_ICON_W, MON_ICON_H, mon_icon_for_form(p->species, p->form));
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
    const uint16_t* iic = item_icon_for(p->heldItem);
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
static uint16_t s_wp_tile[64];         /* one verified 8x8 tile (128 B) — shared by
                                        * draw_wallpaper and the START+SELECT audit */

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

static void draw_wallpaper(int wp, int x, int y, int w, int h) {
  int nt; const uint16_t* tiles = wallpaper_tile_data(wp, &nt);
  const uint16_t* map = wallpaper_tilemap(wp);
  /* any full-region repaint wipes the §12b understudy blits with it */
  for (int s = 0; s < G3_BOX_SLOTS; s++) s_under[s] = 0;
  s_wp_drawn = -1;
  if (!tiles || !map) { draw_grass(x, y, w, h); return; }   /* procedural fallback: no ROM read */
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
        }
        int bx = x + tx * 8, by = y + ty * 8;
        for (int j = 0; j < 8 && by + j < y + h; j++)
          for (int i = 0; i < 8 && bx + i < x + w; i++)
            m3_plot(bx + i, by + j, s_wp_tile[j * 8 + i] & 0x7FFF);  /* VRAM write, source is RAM */
      }
  }
  rumble_io_resume();
  if (!dirty) s_wp_drawn = wp;         /* the art is on screen: rect restores may re-use it */
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

/* Repaint the wallpaper inside [x0,x0+w)x[y0,y0+h) only. Art path: re-uses the staged
 * s_wp_map (verified + golden-checked by the last full draw of s_wp_drawn) and the
 * same verified per-tile staging + Walda substitution as draw_wallpaper. Grass path
 * (s_wp_drawn < 0): repaint base + leaves with the FULL-REGION phase so the patch
 * can't seam against the surrounding fallback. A tile that stays dirty is skipped —
 * a stale 8x8 beats baking garbage, and the next full repaint heals it. */
static void wp_restore_rect(int x0, int y0, int w, int h) {
  int x1 = x0 + w, y1 = y0 + h;
  if (x0 < WP_X) x0 = WP_X;
  if (y0 < WP_Y) y0 = WP_Y;
  if (x1 > WP_X + WP_W) x1 = WP_X + WP_W;
  if (y1 > WP_Y + WP_H) y1 = WP_Y + WP_H;
  if (x0 >= x1 || y0 >= y1) return;
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
static void start_carry(BoxSource* src, const uint8_t* recs, int box, int slot) {
  memcpy(s_held, recs + (uint32_t)slot * 80, 80);            /* lift-don't-clear: copy, origin stays */
  s_holding = true; s_orig_box = box; s_orig_slot = slot; s_orig_bank = src->is_bank;
  s_held_dup = false;                                         /* a real mon (origin keeps it) */
  s_orig_party = false;                                       /* a box/bank origin, not the party */
}

/* Clear the origin cell after a within-scope drop; handles bank paging and returns the
 * CURRENT box reloaded into recs. Call AFTER placing the held mon at the dest (dest-first
 * ordering -> a mid-op power loss duplicates, never loses). */
static uint8_t* clear_origin(BoxSource* src, int box) {
  if (s_orig_party) {                                         /* carried OUT of the party -> remove it there */
    app_party_remove_at(s_orig_slot);                         /* deferred (staged SB1); fail-toward-dup */
    s_orig_party = false; s_orig_slot = -1; return src->records(box);
  }
  if (s_orig_slot < 0 || s_orig_bank != src->is_bank) { s_orig_slot = -1; return src->records(box); }
  uint8_t* o = src->records(s_orig_box);                     /* bank: flushes the current (dest) box first */
  memset(o + (uint32_t)s_orig_slot * 80, 0, 80);
  src->mark_dirty(); s_orig_slot = -1;
  return src->records(box);                                  /* reload the current box */
}

/* Drop the held mon onto cursor cell `cur`. Within the origin's scope: true move (place +
 * clear origin; swap if occupied). Across the PC<->Bank boundary: COPY onto an empty cell
 * only (origin kept) so a mon can't be lost between two save scopes. *done=true when the
 * hand is empty afterwards. Returns the (maybe reloaded) recs. */
static uint8_t* drop_held(BoxSource* src, int box, int cur, uint8_t* recs, bool* done) {
  *done = false;
  if (s_orig_slot >= 0 && s_orig_bank == src->is_bank && s_orig_box == box && cur == s_orig_slot) {
    s_holding = false; *done = true; return recs;            /* dropped back on its own cell */
  }
  /* A bank slot whose mon is moving out to the PC looks empty but still holds that mon's only
   * on-card copy until the PC is saved — treat it as OCCUPIED so nothing overwrites it. */
  bool occupied = g_box[cur].species != 0 || (src->is_bank && app_bank_slot_pending(box, cur));
  if (s_orig_bank != src->is_bank) {                         /* cross-scope drop */
    if (occupied) { snd_deny(); return recs; }
    if (s_held_dup && s_orig_slot < 0) {                     /* a fresh DUPLICATE: placing it is loss-proof
                                                                 in either direction -> no confirm needed
                                                                 (also fixes the reversed "Copy to Bank?"
                                                                 text when a bank dup lands in the PC). */
      if (src->note_add) src->note_add(s_held);
      memcpy(recs + (uint32_t)cur * 80, s_held, 80); src->mark_dirty();
      s_holding = false; s_held_dup = false; *done = true; return recs;
    }
    if (!src->is_bank && s_orig_bank && s_orig_slot >= 0) {  /* BANK -> PC: a true MOVE, no prompt */
      /* place in the PC now (deferred); the bank original is deleted at the save phase,
       * AFTER the PC is written, so it can't be lost (worst case a duplicate). The defer
       * queue holds 64: when FULL, refuse the drop (a silent un-queued move would leave a
       * duplicate behind) — save + re-enter to flush the queue. */
      if (app_bank_defer_full()) { snd_deny(); return recs; }
      if (src->note_add) src->note_add(s_held);
      memcpy(recs + (uint32_t)cur * 80, s_held, 80); src->mark_dirty();
      app_bank_defer_delete(s_orig_box, s_orig_slot, s_held);
      s_holding = false; *done = true; return recs;
    }
    /* PC -> Bank: an always-RELEASE move (no autoduplication — Guy's decision). Write the
     * mon into the (empty) bank cell, VERIFY the bank box on SD, and only then release the PC
     * source. On a write failure, revert the cell and keep holding so nothing is lost (learn:
     * commit the destination before clearing the source -> worst case a duplicate, never a
     * loss; the PC clear is deferred to the one exit save, backed by the immutable backup). */
    memcpy(recs + (uint32_t)cur * 80, s_held, 80);
    boxoam_suspend();
    bool ok = src->commit();                                 /* verified bank box_save (banksrc_commit) */
    boxoam_resume();
    if (!ok) { memset(recs + (uint32_t)cur * 80, 0, 80); snd_error(); return recs; }   /* keep holding */
    if (s_orig_slot >= 0) app_pc_release_slot(s_orig_box, s_orig_slot, s_held);
    snd_save();
    s_holding = false; *done = true; return recs;
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
  if (s_orig_slot >= 0 && s_orig_box != box && src->is_bank) { snd_deny(); return recs; }  /* bank cross-box swap unsafe */
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

static void oam_sync(int cur, bool on_title, int box, bool is_bank) {
  if (s_oam_reload) { boxoam_load_box(g_box); s_oam_reload = false; era_hides_apply(); }
  if (s_holding && s_grab_dip) {
    boxoam_carry_end();
    /* the menu-wipe repaint ran ONE oam_sync in the carry look, which lift-hid the
     * origin cell — undo that: retail keeps the mon in place until the fist closes.
     * NOT for a cell wearing era art: its icon is hidden on purpose and un-hiding it
     * would stack a Gen-3 icon on the Gen-1 sprite for the length of the dip. */
    if (s_orig_slot >= 0 && s_orig_slot < G3_BOX_SLOTS &&
        s_orig_bank == is_bank && s_orig_box == box &&
        !(s_era_drawn & (1u << s_orig_slot)))
      boxoam_show_slot(s_orig_slot);
    boxoam_item_markers(g_box, false);
    boxoam_carry_item(cur, 0, false);
    boxoam_cursor(cur, false, cursor_look());        /* the descending open hand */
  } else if (s_holding) {
    PkMon hm; pk_decode_mon(s_held, false, &hm);
    boxoam_carry_held(cur, hm.species, hm.form, hm.isEgg && !hm.isBadEgg);   /* held mon (or Egg) front-most + orange fist */
    if (s_orig_slot >= 0 && s_orig_bank == is_bank && s_orig_box == box)
      boxoam_hide_slot(s_orig_slot);                         /* lift-hide the origin cell */
    boxoam_item_markers(g_box, false);
    boxoam_carry_item(cur, 0, false);
  } else if (s_cur_mode == CM_ITEM && s_item_held) {
    boxoam_carry_end();                                      /* ITEM GRAB: carried item rides the cursor */
    boxoam_item_markers(g_box, true);
    boxoam_cursor(cur, on_title, BOXOAM_HAND_ITEM);          /* the HAND follows too — it used to stay
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
    boxoam_cursor(cur, on_title, cursor_look());             /* cursor hand last; restores region A */
  }
}

static void draw_footer(bool is_bank, bool on_title, bool moving) {
  const char* f;
  if (s_tab_focus >= 0)    f = "L/R tab  A pick  DN";
  else if (moving)         f = "A drop  B cancel";
  else if (s_item_held)    f = "A give  B put back";
  else if (on_title)       f = "L/R box  A edit  DN";
  else if (s_cur_mode == CM_MOVE) f = "MOVE A grab hold=set";
  else if (s_cur_mode == CM_ITEM) f = "ITEM  A take  SEL B";
  else                     f = is_bank ? "A menu  SEL  L/R  B"
                                       : "A menu  UP  SEL  B";
  /* clear the footer strip first (it changes between modes) */
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), f);
}

/* Set a box mon's held item (decrypt -> set -> re-encode + checksum, in place). */
static void box_set_held(uint8_t* recs, int slot, uint16_t item) {
  uint8_t* rec = recs + (uint32_t)slot * 80;
  EditMon e; gen3_edit_load(rec, false, &e);
  em_set_item(&e, item);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, 80);
}

/* A safe home slot to deposit the carried item: its source slot if still empty-
 * handed, else the first occupied mon with no item; -1 if nowhere (never lose it). */
static int item_home(void) {
  if (s_item_from >= 0 && g_box[s_item_from].species && !g_box[s_item_from].heldItem) return s_item_from;
  for (int s = 0; s < 30; s++) if (g_box[s].species && !g_box[s].heldItem) return s;
  return -1;
}

/* Art-free build: the 30 icon sprites don't exist, so an occupied cell would be an
 * empty patch of wallpaper. Draw an ORIGINAL name chip per occupied cell instead
 * (our font, our colours) so the PC stays a usable grid; the icon art or a
 * registered ROM upgrades it back to real sprites. Painted with the wallpaper, so
 * every full-region repaint carries the labels for free. */
static void artless_cells(void) {
  if (boxoam_icons_available()) return;   /* compiled art OR the user's registered ROM */
  for (int i = 0; i < 30; i++) {
    if (!g_box[i].species) continue;
    int cx = GRID_X + (i % COLS) * CELL_W, cy = GRID_Y + (i / COLS) * CELL_H;
    if (g_box[i].isEgg && !g_box[i].isBadEgg)
      ui_name_chip(cx, cy + 5, CELL_W - 2, 12, 0x2A7A, 0x0000, "EGG");
    else
      ui_name_chip(cx, cy + 5, CELL_W - 2, 12, UI_PANEL, UI_TEXT,
                   pk_species_name(g_box[i].species));
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
 * Two layers, both simultaneous across all 30 cells (rationale + measured cost in
 * pdna_origin_art.h §3):
 *
 *   1. THE PICTURE. A GB import whose era has a registered ROM is drawn in that era's
 *      own sprite, scaled into the cell and blitted to the BG bitmap, with its Gen-3 OBJ
 *      icon hidden so the two cannot stack. Natives keep their OBJ icon. All three eras
 *      are therefore on screen at once, each in its own art.
 *   2. THE LABEL. Every GB cell gets a small era pad ('1'/'2'/'?') at its top-left. This
 *      layer is free (one cached byte per cell) so it is ALWAYS drawn — no GB ROM, the
 *      artless build, a species the ROM would not serve. It is also the layer that makes
 *      the answer unambiguous: a Gen-1 and a Gen-2 sprite of the same species can look
 *      nearly identical, so the picture alone was never a complete answer.
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

/* One cell, both layers. Called only from a full repaint — the picture costs a GB pic
 * decode off the card, so it must never sit on a per-frame path. */
static void era_cell_draw(int slot) {
  if (!pdna_origin_box_gb(slot)) return;              /* native / empty: nothing to do */
  int cx = GRID_X + (slot % COLS) * CELL_W, cy = GRID_Y + (slot / COLS) * CELL_H;

  /* Layer 1. art_wanted() is a cache lookup plus the source's have() probe — no decode,
   * no card access — so a box with no registered era ROM stops here for free. */
  if (pdna_origin_box_art_wanted(slot)) {
    PdnaArt a;
    /* gen == PDNA_GEN3 means the router fell back (the ROM could not serve this
     * species): leave the ordinary Gen-3 OBJ icon alone rather than blitting the same
     * picture twice, once badly. */
    if (pdna_origin_box_art(slot, &g_box[slot], &a) && a.px && a.gen != PDNA_GEN3) {
      u16 cell[CELL_W * CELL_H];         /* 1056 B of STACK. Never a static, never
                                          * EWRAM (hard rule 2) — and never 30 of them. */
      if (pdna_origin_cell_render(&a, cell, CELL_W, CELL_H)) {
        ui_sprite(cx, cy, CELL_W, CELL_H, cell);
        s_era_drawn |= 1u << slot;
        boxoam_hide_slot(slot);          /* or the Gen-3 icon sits ON the Gen-1 sprite */
      }
    }
  }
  era_cell_mark(slot);                   /* layer 2 goes on top of layer 1 */
}

/* Every cell's era, in one pass, painted with the wallpaper so each full repaint carries
 * it for free. */
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
   * still has to be handed back even when THIS box has no imports. */
  if (s_era_drawn || pdna_origin_box_any_gb()) {
    /* Reset first, THEN recompute. s_era_drawn survives across box flips and across whole
     * pdna_box() runs, and a stale set bit means a permanently hidden OBJ icon over a cell
     * that no longer has art to show — an empty cell holding a real Pokemon. Handing every
     * marked cell back to the OAM layer up front makes that unrepresentable. */
    for (int i = 0; i < G3_BOX_SLOTS; i++) era_cell_icon_back(i);
    for (int i = 0; i < G3_BOX_SLOTS; i++) era_cell_draw(i);
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
  char bn[12], bnocc[24];
  src->get_name(box, bn);
  int occ = 0;
  for (int s = 0; s < 30; s++) if (g_box[s].species) occ++;
  siprintf(bnocc, "%s  %d/30", bn[0] ? bn : "BOX", occ);
  draw_banner(WP_X + 2, 13, WP_W - 4, bnocc);
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
  artless_cells();
  era_cells();                  /* each cell in the art of the era it came from */
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
    draw_wallpaper(src->get_wp(box), WP_X, WP_Y, WP_W, WP_H);
    artless_cells();                                  /* clear stale title frame */
    /* The wallpaper repaint above wipes the BG, and the era layer LIVES in the BG — so it
     * has to be redrawn here too. Leaving it out cost every era marker on the first press
     * of UP, permanently: moving onto the box title is how you change boxes, i.e. the
     * core interaction of the screen this feature exists for, and nothing else repaints
     * the layer. The rule is simply that artless_cells() and era_cells() are the two BG
     * cell layers and every site that repaints the wallpaper owes both. */
    era_cells();
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
  s_ch_on_src = (box == s_ch_box && is_bank == s_ch_bank);   /* §12b: source cells vacate, no understudy */
  boxoam_item_markers(g_box, false);
  boxoam_carry_item(g3_slot(s_ch_tr, s_ch_tc), 0, false);
  boxoam_chunk_carry(s_ch_tr, s_ch_tc, s_ch_tr + s_ch_fr, s_ch_tc + s_ch_fc,
                     s_ch_cells, s_ch.n, fit, s_ch_lift);
  if (box == s_ch_box && is_bank == s_ch_bank)
    for (int i = 0; i < s_ch.n; i++) boxoam_hide_slot(s_ch.src[i]);   /* lift-hide the sources */
}

/* Footprint fit test vs the CURRENT box (own sources count as vacating on the source box). */
static bool chunk_fit(BoxSource* src, int box) {
  uint8_t dest[G3_BOX_SLOTS], vac[G3_BOX_SLOTS], tgt[G3_BOX_SLOTS];
  bool same = (box == s_ch_box && src->is_bank == s_ch_bank);
  box_occupancy(src, box, dest);                /* moving-out bank mons still count as occupied */
  if (same) { memset(vac, 0, sizeof vac); for (int i = 0; i < s_ch.n; i++) vac[s_ch.src[i]] = 1; }
  return chunk_can_drop(&s_ch, s_ch_tr, s_ch_tc, dest, same ? vac : 0, tgt);
}

/* Light anchor-move update: the block, its fit tint, and the fist are pure OAM — no
 * bitmap touches at all (the old code re-blitted the whole wallpaper per step). */
static void chunk_move(BoxSource* src, int box) {
  chunk_oam_sync(box, src->is_bank, chunk_fit(src, box));
}

/* Full repaint while carrying a chunk: BG chrome + the floating block (whitened = fits here,
 * darkened = blocked) at the anchor. clear=false repaints OVER the current screen (no black
 * flash) so a box switch doesn't flicker; plain anchor moves use chunk_move (OAM-only). */
static void chunk_draw(BoxSource* src, int box, bool clear) {
  if (clear) ui_clear();
  draw_tab(0, PANEL_W + 1, "PKMN DATA", true);
  draw_tab(PANEL_W + 1, 92, src->is_bank ? "(BANK)" : "PARTY", false);
  draw_tab(PANEL_W + 93, UI_SCR_W - (PANEL_W + 93), "SAVE", false);
  PkMon rep; pk_decode_mon(s_ch_rec[0], false, &rep); pk_resolve(&rep);
  draw_left(&rep);                                    /* the panel shows what you're carrying */

  draw_wallpaper(src->get_wp(box), WP_X, WP_Y, WP_W, WP_H);
  artless_cells();
  era_cells();                     /* same pairing as move_cursor: BG repaint owes both */
  draw_box_banner(src, box, false);

  /* no footprint frame — the block itself carries the fit cue (whitened/darkened) */
  char f[28]; siprintf(f, "x%d  A drop  B cancel", s_ch.n);
  ui_fill_rect(WP_X, 152, WP_W, 8, UI_BG);
  ui_text(WP_X + 2, 152, RGB15(31, 31, 31), f);

  chunk_oam_sync(box, src->is_bank, chunk_fit(src, box));

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
   * take no backup. If it refuses, the move simply degrades to a safe duplicate (the mons live in
   * both boxes) — never a loss. */
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
  *pfull = true;

  uint8_t dest[G3_BOX_SLOTS], vac[G3_BOX_SLOTS], tgt[G3_BOX_SLOTS];
  bool same = (box == s_ch_box && src->is_bank == s_ch_bank);
  box_occupancy(src, box, dest);                 /* moving-out bank mons still count as occupied */
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
    for (int d = 8; d >= 0; d--) {
      boxoam_cursor_dy(d);
      boxoam_cursor(g3_slot(s_ch_tr + s_ch_fr, s_ch_tc + s_ch_fc), false, cursor_look());
      boxoam_commit(); s_vsync();
    }
    boxoam_cursor_dy(0);
    boxoam_hand_pose(BOXOAM_POSE_NORMAL);
  }
  s_ch_lift = 8;                                        /* carry height again for whoever keeps holding */

  /* ---- Bank -> Bank, different box: immediate paging-aware move (commit dest, then clear source) ---- */
  if (src->is_bank && s_ch_bank && box != s_ch_box)
    return drop_chunk_bank_cross(src, box, tgt);

  /* ---- same scope, same box (bank rearrange) OR PC<->PC (any box): a plain deferred move ---- */
  if (src->is_bank == s_ch_bank) {
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
  if (src->is_bank && !s_ch_bank)
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
  boxoam_cursor(cur, false, cursor_look());
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
    if (g_box[anchor].species) {
      start_carry(src, recs, box, anchor);
      if (themed) render_full(src, box, anchor, false, false, false);   /* only to undo the theme */
      play_grab_anim(src, box, anchor);                 /* hand stays on screen throughout */
      carry_move(src, box, anchor, anchor);
      draw_footer(src->is_bank, false, true);
      *pfull = false;
    } else snd_deny();
    return recs;
  }
  /* build the chunk from the OCCUPIED cells inside the rectangle */
  uint8_t occ[G3_BOX_SLOTS];
  for (int s = 0; s < G3_BOX_SLOTS; s++) occ[s] = g_box[s].species ? 1 : 0;
  if (chunk_build(&s_ch, occ, anchor, corner) == 0) { snd_deny(); return recs; }
  for (int i = 0; i < s_ch.n; i++) memcpy(s_ch_rec[i], recs + (uint32_t)s_ch.src[i] * 80, 80);
  for (int i = 0; i < s_ch.n; i++) {                    /* decode ONCE for the block display */
    PkMon m; pk_decode_mon(s_ch_rec[i], false, &m);
    s_ch_cells[i].rr = s_ch.rr[i];  s_ch_cells[i].cc = s_ch.cc[i];
    s_ch_cells[i].species = m.species;  s_ch_cells[i].form = m.form;
    s_ch_cells[i].egg = (m.isEgg && !m.isBadEgg) ? 1 : 0;
  }
  s_ch_hold = true; s_ch_box = box; s_ch_bank = src->is_bank;
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
  chunk_draw(src, box, false);
  /* Retail descends the OPEN hand onto the block before the fist closes on it — the
   * same beat as the single-mon grab (capture doc §4.6). chunk_draw has already put
   * the fist sprite up, so the descent runs on the cursor's own dy driver and the
   * fist is re-placed by chunk_move at the end. */
  if (app_anim_enabled(ANIM_BOX)) {
    boxoam_hand_pose(BOXOAM_POSE_REACH);
    for (int d = 0; d <= 8; d++) {
      boxoam_cursor_dy(d);
      boxoam_cursor(g3_slot(s_ch_tr + s_ch_fr, s_ch_tc + s_ch_fc), false, cursor_look());
      boxoam_commit(); s_vsync();
    }
    boxoam_cursor_dy(0);
    boxoam_hand_pose(BOXOAM_POSE_NORMAL);
    chunk_draw(src, box, false);                        /* fist look back, hand hidden */
  }
  for (int v = 0; v < 8; v++) { boxoam_commit(); s_vsync(); }
  for (int v = 1; v <= 8; v++) { s_ch_lift = v; chunk_move(src, box); boxoam_commit(); s_vsync(); }
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
    bool back = false;
    while (!back) {
      ui_clear();
      draw_wallpaper(wp, WP_X, WP_Y, WP_W, WP_H);
      /* the box's icons stay composited as OBJ sprites above this preview BG */
      char b[40]; siprintf(b, "%d/%d %s%s", wp - base + 1, glen, wp_name(wp), wp >= 16 ? " *" : "");   /* * = Walda secret */
      ui_panel(50, 0, 140, 13, UI_PANEL, UI_BORDER);
      ui_text(56, 2, UI_TITLE, b);
      ui_text(2, 152, RGB15(31, 31, 31), "L/R pick  A set  B sets");
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
  box_decode_to(src, recs, box, list);
  int total = 0;
  for (int s = 0; s < G3_BOX_SLOTS; s++) if (list[s].species) total++;
  if (total == 0) {
    snd_deny();
    ui_clear();
    ui_panel(20, 60, 200, 44, UI_PANEL, UI_BORDER);
    ui_text(30, 70, UI_WARN, "BOX IS EMPTY");
    ui_text(30, 86, UI_DIM, "Nothing to export. Press A");
    u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
    return;
  }
  boxoam_suspend();
  int done = 0, failed = 0;
  for (int s = 0; s < G3_BOX_SLOTS; s++) {
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
  ui_text(30, 96, UI_DIM, "Press A");
  u16 kk; do { s_vsync(); kk = key_hit(KEY_A); } while (!kk);
  boxoam_resume();
}

/* "Release all": permanently delete every mon in `box` (PC = removed from the save; bank = removed
 * from the bank box). Confirms first (destructive), then clears + commits via the box's verified-
 * write path (an immutable backup is taken before the .sav/box write). Omega-only. */
static void release_box_all(BoxSource* src, int box) {
  uint8_t* recs = src->records(box);
  PkMon list[G3_BOX_SLOTS];
  box_decode_to(src, recs, box, list);
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
  int sel = 0;
  for (;;) {
    const int mx = 50, my = 42, mw = 140, mh = 18 + 5 * 14 + 11;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "BOX");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < 5; i++) {
      int y = my + 18 + i * 14; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, OPT[i]);
    }
    ui_text(mx + 6, my + mh - 9, UI_DIM, "A pick B back");
    u16 k; do { s_vsync(); k = key_hit(KEY_UP | KEY_DOWN | KEY_A | KEY_B); } while (!k);
    if (k & KEY_B) { snd_back(); return; }
    else if (k & KEY_UP)   { snd_move(); sel = (sel > 0) ? sel - 1 : 4; }
    else if (k & KEY_DOWN) { snd_move(); sel = (sel + 1) % 5; }
    else if (k & KEY_A) {
      snd_ok();
      if (sel == 0) {                              /* rename */
        char cur[12]; src->get_name(box, cur);
        char buf[12];
        if (osk_input("BOX NAME", cur[0] ? cur : "BOX", buf, 9)) {
          src->set_name(box, buf);
          src->commit();
        }
        return;
      } else if (sel == 1) {                       /* wallpaper */
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
      } else if (sel == 2) {                       /* export all to .pk */
        export_box_all(src, box);
        return;
      } else if (sel == 3) {                       /* release all (destructive; confirms) */
        release_box_all(src, box);
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

int pdna_box(BoxSource* src) {
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
  boxoam_enter();                             /* enable OBJ; upload hand/grab + palettes */
  s_oam_reload = true;                        /* first paint uploads the box's icon tiles */
  uint8_t* recs = src->records(box);          /* current box's 30*80 records */
  box_decode(src, recs, box);
  if (pickup_ps >= 0) start_carry(src, recs, box, pickup_ps);   /* lift the parked mon */
  /* Cursor-arrival hint when crossing the PC<->Bank edge: bottom row (carrying up into the
   * bank) or the top tabs (only when NOT carrying — you can't rest a held mon on a tab). */
  { int st = app_box_start_take();
    if (s_ch_hold) {                                    /* carrying a chunk across the PC<->Bank edge */
      if (s_ch_tc > chunk_anchor_cmax(&s_ch)) s_ch_tc = chunk_anchor_cmax(&s_ch);
      if (st == 2)      s_ch_tr = chunk_anchor_rmax(&s_ch);   /* arrived from below -> bottom of grid */
      else if (st == 1) s_ch_tr = 0;                          /* arrived from above -> top of grid */
      else if (s_ch_tr > chunk_anchor_rmax(&s_ch)) s_ch_tr = chunk_anchor_rmax(&s_ch);
    } else if (st == 1 && !s_holding) s_tab_focus = src->is_bank ? 2 : 1;
    else if (st == 2) cur = COLS * (ROWS - 1);
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
  #define SWITCH_BOX(nbx) do { box = (nbx); recs = src->records(box); \
                               box_decode(src, recs, box); \
                               if (cur < 0 || cur >= COLS * ROWS) cur = 0; \
                               bob = 0; anim_ctr = 0; \
                               if (!src->is_bank) app_note_pc_box(box); \
                               s_oam_reload = true; need_full = true; paint_over = true; } while (0)

  for (;;) {
    if (need_full) {
      bool clr = !paint_over;                  /* a box switch repaints over, no wipe */
      if (s_ch_hold) chunk_draw(src, box, clr);
      else           render_full(src, box, cur, on_title, s_holding, clr);
      need_full = false; paint_over = false;
    }
    u16 k, fresh;
    do { s_vsync();
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
          * 2-frame pose swap): the pose swap re-DMAs ~15 KiB, which box_oam.c:199 says
          * cannot fit the vblank window, and it self-disables on streamed icons
          * (box_oam.c:433) — i.e. on every artless box. The 1 px OAM nudge is a shadow
          * write with no VRAM, ROM or SD traffic, so it is the one icon animation that
          * works in BOTH builds. Paused while carrying / dragging / ITEM mode. */
         if (app_anim_enabled(ANIM_BOX) && !s_holding && !s_ch_hold && s_cur_mode != CM_ITEM) {
           if (++anim_ctr >= ANIM_PERIOD) {
             anim_ctr = 0; bob ^= 1;
             boxoam_hand_pose(bob ? BOXOAM_POSE_BOUNCE : BOXOAM_POSE_NORMAL);
             /* The REAL animation first: a 2-frame pose swap, which is what Gen 3
              * actually does and what "animated" means. It only fails on ROM-streamed
              * icons (no frame-1 source in RAM, and no 15 KiB anywhere to cache one —
              * see boxoam_set_frame). There, and only there, fall back to nudging the
              * sprites 1 px so the grid is not dead. Guy's words for the fallback on
              * its own were "not animated, just jumping up and down" — exactly right,
              * which is why it is now the fallback and not the animation. */
             if (!boxoam_set_frame(bob)) boxoam_set_bob(bob);
             boxoam_cursor(cur, on_title, cursor_look());
           }
         } else if (bob) { bob = 0; boxoam_hand_pose(BOXOAM_POSE_NORMAL);
                           if (!boxoam_set_frame(0)) boxoam_set_bob(0);   /* settle the grid */
                           boxoam_cursor(cur, on_title, cursor_look()); }
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
                                else if (!src->is_bank) { boxoam_exit(); return 4; }    /* up past PC top -> Bank */
                                else snd_deny(); }
      else if (k & KEY_DOWN)  { if (s_ch_tr < chunk_anchor_rmax(&s_ch)) s_ch_tr++;
                                else if (src->is_bank) { boxoam_exit(); return 5; }     /* off Bank bottom -> PC */
                                else snd_deny(); }
      if (!s_ch_hold) boxoam_chunk_end();            /* B-cancel / successful drop: restore the
                                                      * borrowed regions before the full repaint */
      if (s_ch_hold && !need_full) chunk_move(src, box);   /* anchor move: pure OAM, no bitmap */
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
          if (!src->is_bank) { if (homeless || s_orig_party) snd_deny(); else { s_tab_focus = -1; boxoam_exit(); return 4; } }
        }
        else if (k & KEY_A) {
          if (s_tab_focus == 1 && !src->is_bank && !s_orig_party) {  /* PARTY tab: place/swap the held box mon into the party */
            bool can_swap = (!s_orig_bank && s_orig_slot >= 0);
            boxoam_suspend();
            int rr = app_party_overlay(s_held, s_orig_box, s_orig_slot, s_orig_bank, can_swap, 0, 0, false);
            boxoam_resume();
            if (rr == 1) {                            /* placed -> end the carry */
              s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_bank = false; s_held_dup = false; s_orig_party = false;
              s_tab_focus = -1; recs = src->records(box); box_decode(src, recs, box);
            }
            s_oam_reload = true; need_full = true;    /* redraw over the popup */
          } else snd_deny();                          /* party-origin mon can't go back; PKMN DATA / SAVE locked while carrying */
        }
        continue;
      }
      if (k & KEY_B) {                               /* cancel */
        if (homeless) {                              /* must place it somewhere -> first free in this box */
          int fs = -1; for (int s = 0; s < COLS * ROWS; s++) if (!g_box[s].species) { fs = s; break; }
          if (fs < 0) { snd_deny(); }                /* box full: keep holding */
          else { memcpy(recs + (uint32_t)fs * 80, s_held, 80); src->mark_dirty();
                 snd_back(); s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_bank = false; s_held_dup = false; s_orig_party = false;
                 box_decode(src, recs, box); s_oam_reload = true; need_full = true; }
        } else {                                     /* origin keeps it (party / box / dup) -> nothing to place */
          snd_back(); s_holding = false; s_orig_slot = -1; s_orig_box = -1; s_orig_bank = false; s_held_dup = false; s_orig_party = false;
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
      else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
      else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
      else if (k & KEY_UP)    {
        if (cur >= COLS) cur -= COLS;
        else if (!src->is_bank) { s_tab_focus = 1; need_full = true; }  /* off PC top -> top tabs (PARTY), still holding */
      }
      else if (k & KEY_DOWN)  {
        if (cur < COLS * (ROWS - 1)) cur += COLS;
        else if (homeless) snd_deny();                          /* place the swapped mon before leaving */
        else if (src->is_bank) { boxoam_exit(); return 5; }     /* off Bank bottom -> PC, still holding */
      }

      /* cursor move while carrying -> partial redraw (no ui_clear), so it doesn't flicker */
      if (!need_full && cur != old_cur) carry_move(src, box, old_cur, cur);
      continue;                                      /* move-mode swallows all other keys */
    }

    /* ---- ITEM CARRY: holding a held item; place it / swap onto another mon ---- */
    if (s_item_held > 0) {
      if (k & KEY_B) {                               /* put it back (never lose it) */
        if (s_item_from_box >= 0 && s_item_from_box != box) SWITCH_BOX(s_item_from_box);  /* back to its box */
        int home = item_home();
        /* Park the cursor on the mon that just got the item back, so the left panel
         * shows where it went. (SWITCH_BOX now PRESERVES the cursor cell, so without
         * this a cross-box put-back would leave the glove on an unrelated slot.) */
        if (home >= 0) { cur = home;
                         box_set_held(recs, home, (uint16_t)s_item_held); box_decode(src, recs, box);
                         src->mark_dirty(); s_item_held = 0; s_item_from = -1; s_item_from_box = -1; need_full = true; }
        else snd_deny();
      }
      else if (k & KEY_A) {                          /* give / swap onto the cursor mon */
        if (g_box[cur].species) {
          uint16_t old = g_box[cur].heldItem;        /* swap: take this mon's old item */
          box_set_held(recs, cur, (uint16_t)s_item_held);
          box_decode(src, recs, box);
          src->mark_dirty();
          s_item_held = old; s_item_from = old ? cur : -1;   /* keep holding the swapped-out item */
          s_item_from_box = old ? box : -1;
          need_full = true;
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
      if      (k & KEY_UP) { if (!src->is_bank) { s_tab_focus = -1; boxoam_exit(); return 4; } }   /* up past the PC tabs -> Bank */
      else if (k & (KEY_B | KEY_DOWN)) { s_tab_focus = -1; need_full = true; }              /* back to box name */
      else if (k & KEY_LEFT)  { s_tab_focus = (s_tab_focus > 0) ? s_tab_focus - 1 : 2; need_full = true; }
      else if (k & KEY_RIGHT) { s_tab_focus = (s_tab_focus + 1) % 3; need_full = true; }
      else if (k & KEY_A) {
        if (s_tab_focus == 0) { s_tab_focus = -1; on_title = false; need_full = true; }      /* PKMN DATA -> grid */
        else if (s_tab_focus == 1) {                                                          /* PARTY -> overlay popup */
          if (src->is_bank) snd_deny();
          else {
            uint8_t grab[80]; int gslot = -1;
            boxoam_suspend();
            int rr = app_party_overlay(0, 0, 0, false, false, grab, &gslot, true);   /* empty-handed: A opens the action menu (Move to box -> grab) */
            boxoam_resume();
            if (rr == 2 && gslot >= 0) {                  /* grabbed a party mon -> carry it (party origin) */
              memcpy(s_held, grab, 80);
              s_holding = true; s_orig_party = true; s_orig_slot = gslot;
              s_orig_box = -1; s_orig_bank = false; s_held_dup = false;
              s_tab_focus = -1; s_oam_reload = true;
              render_full(src, box, cur, false, false, true);                  /* repaint box over the popup */
              play_grab_anim(src, box, cur); carry_move(src, box, cur, cur);
              draw_footer(src->is_bank, false, true);
            } else { s_tab_focus = -1; s_oam_reload = true; need_full = true; } /* closed -> back to the grid */
          }
        }
        else { s_tab_focus = -1; boxoam_exit(); return 0; }                                   /* SAVE -> exit (save prompt) */
      }
      continue;
    }

    if (k & KEY_B) { if (s_cur_mode != CM_NORMAL && !on_title) { s_cur_mode = CM_NORMAL; need_full = true; } else { boxoam_exit(); return 0; } }
    else if ((k & KEY_START) && !src->is_bank) { boxoam_exit(); return 2; }
    else if (k & KEY_L) { SWITCH_BOX((box + nb - 1) % nb); }
    else if (k & KEY_R) { SWITCH_BOX((box + 1) % nb); }
    else if (k & KEY_SELECT) {                       /* cycle cursor mode (Omega-only edit modes) */
      if (!on_title && src->can_edit()) { s_cur_mode = (s_cur_mode + 1) % 3; need_full = true; }
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
        if (src->can_edit()) { boxoam_suspend(); box_options_menu(src, box); boxoam_resume();
                               recs = src->records(box); box_decode(src, recs, box);  /* Release all mutates records */
                               s_oam_reload = true; need_full = true; }
        else { snd_deny(); }
      }
    }
    else if (k & KEY_LEFT)  cur = (cur % COLS == 0) ? cur + COLS - 1 : cur - 1;
    else if (k & KEY_RIGHT) cur = (cur % COLS == COLS - 1) ? cur - COLS + 1 : cur + 1;
    else if (k & KEY_UP)    { if (cur < COLS) on_title = true; else cur -= COLS; }
    else if (k & KEY_DOWN)  { if (src->is_bank && cur >= COLS * (ROWS - 1)) { boxoam_exit(); return 5; }   /* off the bank bottom -> PC tabs */
                              else cur = (cur >= COLS * (ROWS - 1)) ? cur - COLS * (ROWS - 1) : cur + COLS; }
    else if ((k & KEY_A) && s_cur_mode == CM_MOVE) {     /* orange hand: TAP = grab one; HOLD+DPAD = rubber-band multi-select */
      if (!src->can_edit()) snd_deny();
      else recs = begin_select(src, box, recs, cur, &need_full);
    }
    else if ((k & KEY_A) && s_cur_mode == CM_ITEM) {     /* transparent hand: pick up the held item */
      if (!src->can_edit()) snd_deny();
      else if (g_box[cur].species && g_box[cur].heldItem) {
        s_item_held = g_box[cur].heldItem; s_item_from = cur; s_item_from_box = box;
        box_set_held(recs, cur, 0);
        box_decode(src, recs, box);
        src->mark_dirty();
        play_item_grab_anim(cur, (uint16_t)s_item_held);   /* fist closes over the mon (grab beat) */
        need_full = true;
      } else snd_deny();                                 /* empty slot or no item */
    }
    else if (k & KEY_A) {
      /* NORMAL: open the action menu on an occupied slot, or on an empty slot when
       * editable (CREATE a mon, or PASTE if the clipboard holds one). */
      if (g_box[cur].species || src->can_edit()) {
        uint8_t* rec = recs + (uint32_t)cur * 80;
        int mbox = src->is_bank ? 0 : box;                               /* box index within menu_block */
        boxoam_suspend();                                                /* sprites off while the menu/summary is up */
        app_mon_menu(rec, false, src->is_bank, src->commit, src->menu_block, mbox, cur);
        boxoam_resume();
        recs = src->records(box);                                        /* menu may have edited it */
        box_decode(src, recs, box);                                  /* refresh after possible write */
        s_oam_reload = true;                                             /* contents may have changed */
        if (app_take_move_request()) {                                   /* picked MOVE -> into the glove */
          start_carry(src, recs, box, cur);
          render_full(src, box, cur, false, false, false);               /* repaint OVER the menu, no black flash */
          play_grab_anim(src, box, cur);                                 /* grab cue (OAM lift) */
          carry_move(src, box, cur, cur);                                /* lift into carry */
          draw_footer(src->is_bank, false, true);                        /* move-mode footer */
        } else if (app_take_dup_request()) {                            /* picked DUPLICATE -> a fresh COPY in the glove */
          memcpy(s_held, recs + (uint32_t)cur * 80, 80);                /* copy floats in-hand; no origin (cancel discards it) */
          s_holding = true; s_orig_box = box; s_orig_slot = -1; s_orig_bank = src->is_bank; s_held_dup = true; s_orig_party = false;
          render_full(src, box, cur, false, false, false);
          play_grab_anim(src, box, cur);
          carry_move(src, box, cur, cur);
          draw_footer(src->is_bank, false, true);
        } else {
          int pb, ps;
          if (app_take_pickup(&pb, &ps) && pb >= 0 && pb < nb && ps >= 0 && ps < 30) {
            box = pb; recs = src->records(box); box_decode(src, recs, box);   /* TO DAY-CARE->PC: carry the parked mon */
            cur = ps; start_carry(src, recs, box, ps); s_oam_reload = true;
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
