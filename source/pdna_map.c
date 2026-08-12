/*
 * Overworld map screen — stage 1: the ROM pipeline.
 *
 * This is the on-cartridge half of the map feature. The parser itself (rom_map.c) is
 * pure C and is already validated on the PC against real retail ROMs; what could only
 * ever be proven on hardware is THIS layer: opening the user's 16 MiB ROM off the
 * microSD through FatFs and random-accessing it while the cartridge ROM window is
 * unmapped. So this screen exists first, deliberately, before any renderer: it reports
 * exactly what it managed to read, so a hardware failure is legible instead of a hang.
 *
 * ---- the flashcart facts this code is written around ------------------------
 *  - During ANY SD transfer the game ROM is unmapped. Code and data live across a
 *    transfer must be EWRAM-resident with IRQs handled by the driver, and NOTHING may
 *    render mid-transfer. Hence the strict read-then-draw split: no f_read happens
 *    while anything is being drawn.
 *  - Read_SD_sectors returns RES_OK UNCONDITIONALLY and DMAs even after a timeout
 *    (projects/rom-load-lab, from the EZ-Flash kernel source). FatFs therefore CANNOT
 *    surface a read error: a failed read arrives as "success" holding garbage. That is
 *    why rom_open()'s structural sanity walk is load-bearing rather than defensive, and
 *    why every value shown here is bounds-checked before it is trusted.
 *  - rmbl_pause()/rmbl_resume() bracket the SD work: the rumble motor is a cart-bus
 *    write and the cart window is gone mid-transfer.
 *
 * PokeDNA ships no Nintendo map data; everything drawn from a ROM is read transiently
 * from the user's own file and never written anywhere.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "pdna_map.h"
#include "rom_map.h"
#include "map_render.h"
#include "map_gfx.h"
#include "gen3_warp.h"
#include "gen3_flags.h"
#include "fused_rom.h"
#include "map_oam.h"
#include "map_region.h"
#include "gen3_sbmap.h"
#include "gen3_sbdecor.h"
#include "rom_script.h"
#include "gen3_secretbase.h"
#include "gen3_save.h"
#include "data_tables.h"   /* pk_location_name */
#include "ff.h"
#include "ui.h"
#include "snd.h"
#include "rmbl.h"
#include "sys.h"          /* EWRAM_BSS (after tonc.h so the u8 macro is safe) */
#include "log.h"
#include "pdna_app.h"

#ifndef PATH_MAX
#define PATH_MAX 256
#endif

static void s_vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }
static u16  s_wait(u16 mask) {
  u16 k; do { s_vsync(); k = key_hit(mask); } while (!k);
  if      (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
  else if (k & KEY_A) snd_ok();
  else if (k & KEY_B) snd_back();
  return k;
}

/* s_wait() returns on the very frame key_poll() saw the press EDGE, and nothing polls
 * again before the caller's own key_hit() runs — so that same edge is still latched and
 * gets consumed a SECOND time. Measured consequences: dismissing "CANNOT ENTER" with B
 * also exited the map view; dismissing it with A opened the place-confirm dialog; and one
 * more A would have committed a save write the user never asked for. Any dialog that hands
 * control back to a key-reading loop must flush first. */
static void s_flush_keys(void) {
  do { VBlankIntrWait(); key_poll(); } while (key_curr_state() & KEY_ANY);
  key_poll();                       /* one clean poll so key_hit's "previous" is empty too */
}

/* Body lines must stay <= 25 columns (panel 216 px, text at x=20). */
static void s_msg(const char* title, u16 ink, const char* l1, const char* l2) {
  /* Clear first: this is sometimes raised straight after another modal (the save path's
   * own SAVED / CHECKSUM panels), and drawing a smaller panel over a larger one left both
   * borders and both "Press A" prompts on screen. */
  ui_clear();
  ui_panel(12, 50, 216, l2 ? 60 : 50, UI_PANEL, UI_BORDER);
  ui_text(20, 58, ink, title);
  ui_hline(16, 70, 208, UI_BORDER);
  if (l1) ui_text(20, 76, UI_TEXT, l1);
  if (l2) ui_text(20, 86, UI_TEXT, l2);
  ui_text(20, l2 ? 96 : 86, UI_DIM, "A ok");
  s_wait(KEY_A | KEY_B);
  /* Same rule as map_dialog: the dismissing press is still latched when this returns, and
   * the map loop would consume it again. It did — dismissing the "PLACED" panel silently
   * re-grabbed the character. Flush here so EVERY dialog in this file is safe by default. */
  s_flush_keys();
}

/* ---- the FatFs side of RomReadFn -------------------------------------------
 * One open FIL for the whole session. f_lseek + f_read; the OS-mode discipline is
 * the driver's, but the rumble motor must be quiet across it. */
typedef struct { FIL f; bool open; } RomFile;
/* EWRAM, not IWRAM: a FatFs FIL carries a 512-byte sector buffer (~600 B total) and the
 * IWRAM .bss it would occupy is stack the rest of the app needs. IWRAM is never unmapped,
 * but neither is EWRAM, so this is still safe to touch during an SD transfer. */
static RomFile EWRAM_BSS s_rf;

static bool rom_fatfs_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  RomFile* r = (RomFile*)ctx;
  if (!r || !r->open) return false;
  UINT br = 0;
  /* Seek ONLY when the file is not already there. FF_USE_FASTSEEK is 0, so every f_lseek
   * walks the cluster chain from the start — on a fragmented card that is a long chain,
   * and the LZ77 and blockdata paths read strictly sequentially, so the seek they were
   * paying for was almost always a no-op. Fewer SD transactions is also less exposure to
   * the driver's silent-failure path. */
  if (off != r->f.fptr && f_lseek(&r->f, off) != FR_OK) return false;
  if (f_read(&r->f, dst, (UINT)len, &br) != FR_OK) return false;
  /* Short read is the ONLY failure FatFs can still report to us here — see the
   * header note about Read_SD_sectors always returning RES_OK. */
  return br == len;
}

/* ---- a minimal .gba picker (kept local so the .sav browser stays untouched) -- */
#define PICK_MAX 128
#define PICK_NAME 64
typedef struct { char name[PICK_NAME]; bool dir; } PickEnt;

/* The picker's extension filter. `pick_rom` wants .gba; the GB-import picker wants
 * .sav/.srm (Game Boy battery files), so the wanted extension is a parameter now and
 * the two entry points below just say which they mean. */
static const char* s_pick_ext[3] = { ".gba", 0, 0 };
static bool ext_matches(const char* n) {
  int l = (int)strlen(n);
  for (int e = 0; e < 3 && s_pick_ext[e]; e++) {
    int el = (int)strlen(s_pick_ext[e]);
    if (l <= el) continue;
    const char* t = n + l - el;
    int ok = 1;
    for (int i = 0; i < el && ok; i++) {
      char a = t[i], b = s_pick_ext[e][i];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (a != b) ok = 0;
    }
    if (ok) return true;
  }
  return false;
}


/* Browse for a .gba. `cwd` is updated in place. Returns true with the full path in
 * `out`. Entries live in the caller's buffer so this adds no EWRAM of its own. */
static bool pick_rom(char* cwd, int cwd_cap, char* out, int out_cap, PickEnt* ents) {
  int sel = 0, top = 0, n = 0;
  bool rescan = true;
  for (;;) {
    if (rescan) {
      rescan = false; n = 0; sel = 0; top = 0;
      DIR d; FILINFO fi;
      rmbl_pause();
      if (f_opendir(&d, cwd) == FR_OK) {
        while (n < PICK_MAX && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
          bool isdir = (fi.fattrib & AM_DIR) != 0;
          if (!isdir && !ext_matches(fi.fname)) continue;
          if (fi.fname[0] == '.') continue;
          strncpy(ents[n].name, fi.fname, PICK_NAME - 1);
          ents[n].name[PICK_NAME - 1] = 0;
          ents[n].dir = isdir;
          n++;
        }
        f_closedir(&d);
      }
      rmbl_resume();
    }

    ui_clear();
    ui_text(4, 4, UI_TITLE, "PICK YOUR ROM (.gba)");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char t[40]; ui_truncate(t, cwd, 29);
    ui_text(4, 18, UI_DIM, t);
    const int vis = 11;
    if (sel < top) top = sel; else if (sel >= top + vis) top = sel - vis + 1;
    if (top > n - vis) top = n - vis;
    if (top < 0) top = 0;
    for (int i = 0; i < vis && top + i < n; i++) {
      char row[40];
      siprintf(row, "%s%s", ents[top + i].dir ? "/" : " ", ents[top + i].name);
      char rt[40]; ui_truncate(rt, row, 28);
      ui_text_sel(4, 30 + i * 10, 232, top + i == sel, ents[top + i].dir ? UI_DIRCLR : UI_TEXT, rt);
    }
    if (!n) ui_text(4, 40, UI_DIM, "No .gba here. B goes up.");
    ui_text(4, 152, UI_DIM, "A pick  B up  START cancel");

    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START);
    if (k & KEY_START) return false;
    if (k & KEY_UP)    sel = sel ? sel - 1 : (n ? n - 1 : 0);
    if (k & KEY_DOWN)  sel = n ? (sel + 1) % n : 0;
    if (k & KEY_B) {
      int l = (int)strlen(cwd);
      if (l <= 1) return false;                       /* already at root */
      while (l > 1 && cwd[l - 1] != '/') l--;
      if (l > 1) l--;
      cwd[l ? l : 1] = 0;
      rescan = true;
      continue;
    }
    if ((k & KEY_A) && n) {
      int l = (int)strlen(cwd);
      if (ents[sel].dir) {
        if (l + 1 + (int)strlen(ents[sel].name) < cwd_cap - 1) {
          if (l > 1) { cwd[l++] = '/'; }
          strcpy(cwd + l, ents[sel].name);
          rescan = true;
        }
        continue;
      }
      siprintf(out, "%s%s%s", cwd, (l > 1) ? "/" : "", ents[sel].name);
      (void)out_cap;
      return true;
    }
  }
}

/* ---- STAGE A/B: the scrolling map view -------------------------------------
 * Mode 0 at 1:1 (or an affine BG when zoomed), real metatiles straight out of the user's
 * ROM. The cursor walks the map, crosses connection seams into neighbouring maps, and
 * enters warps. Read-only until the user grabs the character.
 *
 * CONTROLS (Guy's mapping, 2026-07-31 — do not "improve" this):
 *   D-pad  move the cursor; crossing an edge loads the connected map
 *   A      grab the character / drop it here
 *   B      cancel the carry if carrying, else leave the map view
 *   SELECT toggle the camera between the character and the last place you scrolled to
 *   START  enter the door / cave / warp under the cursor
 *   L / R  zoom out / in
 * There is deliberately NO back-stack key: rooms are left the way the game leaves them,
 * by walking onto the exit warp and pressing START.
 *
 * The SD work happens up front with the screen blanked, and again on a map change behind a
 * "Loading" frame; nothing is ever drawn while a transfer is in flight. */

typedef struct {
  const RomCtx* rc;
  uint8_t*      arena;
  PkGame        game;
  uint8_t       group, num;
  RomMapHeader  hdr;
  RomLayout     lay;              /* BY VALUE: re-read on every load                */
  int           cx, cy, zoom, hold;
  bool          hud_dirty, fatal;
  uint8_t       base_id;        /* secretBaseId of the interior we walked into, 0 = unknown */
  uint8_t       decor_sprites;  /* decorations this room has that are OBJ sprites, not tiles */
  /* SELECT camera toggle: where the character is, and the last place we scrolled to. */
  uint8_t       home_group, home_num;
  int           home_x, home_y;
  bool          at_home;
  uint8_t       away_group, away_num;
  int           away_x, away_y;
  bool          have_away;
  /* grab & drop */
  uint8_t*      sb1;
  uint8_t*      sb2;
  bool          carrying;
  bool          placed;                 /* something was committed this session      */
  uint8_t       snap[G3W_SNAPSHOT_SIZE];/* taken BEFORE the first write, for undo     */
  bool          have_snap;
  /* render integrity */
  uint32_t      frame;
  int           repairs, repairs_map, last_repair;
  bool          detect_only;            /* SELECT held on entry: detect + log, no repair */
  bool          said_repaired;
  /* secret bases */
  bool          in_base;                /* the loaded map is a secret-base interior   */
  uint8_t       ret_group, ret_num;     /* the entrance we came in through            */
  int           ret_x, ret_y;
} MapViewState;

/* Secret-base entrances on the CURRENT map. MEASURED max on any real map is 13. */
typedef struct { int16_t x, y; uint8_t id; bool occupied; char owner[8]; } SbEnt;
static SbEnt EWRAM_BSS s_ent[SB_MAX_ENTRANCE];
static int          s_ent_n;
static void scan_secret_bases(uint32_t events_addr);   /* map_switch needs it */
static void apply_base_decor(void);                    /* ditto, and map_view */

static MapViewState EWRAM_BSS s_mv;

/* An object event is an item ball iff it carries the game's ball graphics id — the same
 * test map_oam uses to decide what to grey out, kept in step with it deliberately. */
static bool is_item_ball(const RomObjectEvent* o) {
  return o && o->graphics_id == ((s_mv.game == PK_FRLG) ? 92u : 59u);
}

/* Already collected? The ball's own flag being SET is exactly what stops the game
 * respawning it, so that one bit is both the question and the thing START toggles. */
static bool ball_taken(const RomObjectEvent* o) {
  MapViewState* v = &s_mv;
  return o && o->flag_id && v->sb1 && pk_flag_get(v->sb1, v->game, o->flag_id);
}
static void player_visibility(void);   /* defined below; map_switch needs it */

static RgnMap    EWRAM_BSS s_rgn;      /* the region map, loaded lazily on first use */
static bool                s_rgn_ok;   /* false => levels 3/4 are unavailable        */
static bool                s_rgn_tried;
static MapGfx    EWRAM_BSS gfx;
static MapRender EWRAM_BSS mr;

/* The 5 ids where Ruby/Sapphire's MAPSEC table diverges from Emerald's; everywhere else
 * (and all of FRLG's Kanto block) the shipped table is already exact. */
/* The "there is nothing here" mapsec, which is a DIFFERENT NUMBER PER FAMILY: RSE uses 213
 * (0xD5) and FRLG uses 197 (0xC5). PokeDNA has ONE 256-entry name table, so FRLG's 197 was
 * being printed as the Hoenn entry that happens to sit at that index — the region map read
 * "AQUA HIDEOUT" over open sea in Kanto. MEASURED: 197 is the most common id in all four FRLG
 * views (240-316 cells each), 213 in Hoenn. */
#define MAPSEC_NONE_RSE  213
#define MAPSEC_NONE_FRLG 197

static const char* map_name(PkGame game, uint8_t mapsec) {
  if (mapsec == (game == PK_FRLG ? MAPSEC_NONE_FRLG : MAPSEC_NONE_RSE)) return "NONE";
  if (game == PK_RS) {
    switch (mapsec) {
      case 0x33: return "UNDERWATER 125";
      case 0x34: return "UNDERWATER 126";
      case 0x35: return "UNDERWATER 127";
      case 0x3A: return "BATTLE TOWER";
      case 0x45: return "UNDERWATER 128";
    }
  }
  return pk_location_name(mapsec);
}

/* DCNT_BLANK displays WHITE, so a blank on every seam would strobe. It is not needed
 * either: the LCD's display DMA reads VRAM/PALRAM/OAM and never the cart, so the last good
 * frame may legally stay up for the whole transfer. Only the VRAM overwrite is disruptive,
 * and MEASURED 100% of connected pairs share the primary tileset (~72% the secondary too),
 * so most crossings overwrite nothing. BG0 uses CBB3 + SBB31 + palette bank 15, which a map
 * load provably never touches, so this text stays legible throughout. */
static void loading_frame(const char* name) {
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
  tte_erase_screen();
  tte_set_pos(4, 76); tte_write("Loading");
  tte_set_pos(4, 86); tte_write(name ? name : "");
}

/* Keep the sprite layer in step with the background camera. cam_px/cam_py are the WORLD
 * PIXEL coordinates of screen (0,0) at the current zoom — the same quantity map_gfx feeds
 * the scroll registers, so sprites and tiles can never disagree by construction. */
static void sprites_follow(void) {
  MapViewState* v = &s_mv;
  int ppt = (v->zoom >= 2) ? 4 : (v->zoom ? 8 : 16);
  int cam_px = v->cx * ppt + ppt / 2 - 120;
  int cam_py = v->cy * ppt + ppt / 2 - 80;
  moam_set_camera(cam_px, cam_py, ppt);
  if (v->carrying) moam_carry(true, v->cx, v->cy);
}

static void camera_refresh(void) {
  MapViewState* v = &s_mv;
  if (v->zoom == 0) {
    mgfx_camera(&gfx, &mr, v->cx, v->cy);
  } else if (mgfx_zoom_build(&gfx, &mr, v->cx, v->cy, v->zoom)) {
    mgfx_zoom_show(v->zoom, v->cx, v->cy);
  } else {
    /* The mip dictionary overflowed twice; mgfx_zoom_build left BG2 off, so staying zoomed
     * would show an empty screen while the HUD reported a new cell. Fall back honestly. */
    snd_deny();
    v->zoom = 0;
    gfx.ring_valid = false;
    mgfx_camera(&gfx, &mr, v->cx, v->cy);
    mgfx_z0_show();
  }
  sprites_follow();
}

/* Load a different map in place. Returns false WITHOUT disturbing the current map when the
 * destination fails validation; sets v->fatal only when the old map is already gone. */
/* ---- off-map places -------------------------------------------------------
 * Places the region map cannot take you to by pointing at them. Three separate reasons,
 * all measured (docs/MAP-FEATURE-CONTINUATION.md Round-7):
 *
 *  - SKY PILLAR is mapsec 85 with a rect at grid (19,10) — but that cell is OWNED by
 *    mapsec 46, ROUTE 131, so `rgn_find_mapsec` can never return it and the HUD names the
 *    host section instead. Retail does the same on purpose: sRegionMap_SpecialPlaceLocations
 *    remaps SKY_PILLAR to ROUTE_131, so the Pokenav never says "SKY PILLAR" either. No
 *    tower is drawn there — the three cells around it are byte-identical open sea.
 *  - FRLG's islands ARE selectable, just on a view the player has to know to switch to.
 *  - Neither is gated by a story flag in the ROM data we read, so this list shows them
 *    whatever the save has done. That makes PokeDNA MORE revealing than retail, which
 *    hides them behind FLAG_WORLD_MAP_* — fine for a save viewer.
 *
 * `mapsec` = 0xFF when the place has no selectable region cell (Sky Pillar); then `host`
 * names the section whose cell it sits inside so the cursor can still be parked on it. */
typedef struct {
  const char* name;
  const char* note;
  uint8_t     group, num;      /* the map to open in the tile view */
  uint8_t     mapsec;          /* region cell to park the cursor on, or 0xFF */
  uint8_t     host;            /* section that owns the cell when mapsec is 0xFF */
} OffMapPlace;

/* Hoenn (Ruby / Sapphire / Emerald). Sky Pillar's maps are 24.77..24.85; 24.83 is NOT one of
 * them (it is Shoal Cave). Rayquaza stands on the TOP floor, 24.85. */
static const OffMapPlace PLACES_RSE[] = {
  { "SKY PILLAR",         "entrance, from Route 131",  24, 77, 0xFF, 46 },
  { "SKY PILLAR TOP",     "Rayquaza is here",          24, 85, 0xFF, 46 },
  { "SEALED CHAMBER",     "the braille room",          24, 71, 0xFF, 49 },
  { "SEALED CHAMBER 2",   "inner room",                24, 72, 0xFF, 49 },
  { "UNDERWATER SEALED",  "the dive room above it",    24, 70, 0xFF, 49 },
  { "DESERT RUINS",       "Regirock",                  24,  6, 0xFF, 27 },
  { "ISLAND CAVE",        "Regice",                    24, 67, 0xFF, 20 },
  { "ANCIENT TOMB",       "Registeel",                 24, 68, 0xFF, 35 },
};
/* Kanto + Sevii. Both of these have real region cells, just on views the player has to
 * switch to: Navel Rock on SEVII 4-5, Birth Island on SEVII 6-7. */
static const OffMapPlace PLACES_FRLG[] = {
  { "NAVEL ROCK",   "Lugia and Ho-Oh",  2,  0, 174, 174 },
  { "BIRTH ISLAND", "Deoxys",           2, 56, 187, 187 },
};

/* A short list on L from the widest region view. On FRLG that key already switches the
 * region VIEW (the game's own SWITCH MAP button) so the view rows are folded into the same
 * list — picking an island switches the view for you, which beats cycling blind.
 *
 * MODE 0 ONLY, like map_dialog: in Mode 3 the bitmap framebuffer IS the map's tileset char
 * data, so a ui_panel here draws confetti over the tiles AND destroys them. Map layers off,
 * BG0's tte draws, nothing in VRAM is harmed, no reload needed on the way back. */
static int places_menu(const OffMapPlace* pl, int n, int views, const char* viewlbl) {
  enum { VIS = 8, ROW_H = 12, TOP_Y = 34 };      /* 8 rows is what fits above the note line */
  int sel = 0, top = 0, total = n + (views > 1 ? 1 : 0);
  char l[48];
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    tte_erase_screen();
    tte_set_pos(8, 6);  tte_write("PLACES");
    tte_set_pos(8, 18); tte_write("Not reachable by pointing");
    for (int i = top; i < total && i < top + VIS; i++) {
      const char* nm = (i < n) ? pl[i].name : "SWITCH MAP VIEW";
      siprintf(l, "%c %s", (i == sel) ? '>' : ' ', nm);
      tte_set_pos(8, TOP_Y + (i - top) * ROW_H); tte_write(l);
    }
    /* The note belongs to the SELECTED row and lives on its own line: eight entries do not
     * fit two lines each, and a caption that moves with the cursor reads better anyway. */
    const char* nt = (sel < n) ? pl[sel].note : (viewlbl ? viewlbl : "next region view");
    siprintf(l, "  %s", nt);
    tte_set_pos(8, 134); tte_write(l);
    tte_set_pos(8, 146); tte_write("A go   B back");
    u16 k = s_wait(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    s_flush_keys();
    if (k & KEY_B) return -1;
    if (k & KEY_A) return sel;
    if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : total - 1;
    if (k & KEY_DOWN) sel = (sel + 1) % total;
  }
}

static bool map_switch(uint8_t ng, uint8_t nn, int nx, int ny) {
  MapViewState* v = &s_mv;
  RomMapHeader nh; RomLayout nl;

  rmbl_pause();
  bool ok = rom_map_header(v->rc, ng, nn, &nh)
         && rom_layout(v->rc, nh.layout, &nl)
         && mgfx_can_load(v->rc, &nl, APP_ARENA_BYTES);
  rmbl_resume();
  if (!ok) { snd_deny(); v->hud_dirty = true; return false; }

  bool same_ts = (nl.tileset_primary   == v->lay.tileset_primary &&
                  nl.tileset_secondary == v->lay.tileset_secondary);
  if (!same_ts) loading_frame(map_name(v->game, nh.mapsec));

  /* Nothing may run between pause and resume — no vsync, no sound, no rumble: the motor is
   * a cart-bus write and the cart window is gone mid-transfer. log_line is RAM-buffered and
   * therefore safe to call after. */
  rmbl_pause();
  ok = mgfx_load(&gfx, &mr, v->rc, &nl, nh.connections, nh.events, v->arena, APP_ARENA_BYTES);
  rmbl_resume();
  log_line("map: -> %u.%u @(%d,%d) ts%c arena2 %lu/%lu mt-miss %lu cbb2 %lu %s",
           ng, nn, nx, ny, same_ts ? '=' : '!',
           (unsigned long)mgfx_arena_phase2(), (unsigned long)APP_ARENA_BYTES,
           (unsigned long)mgfx_mt_misses(), (unsigned long)mgfx_cbb2_bad(),
           ok ? "OK" : "FAILED");
  if (!ok) { v->fatal = true; return false; }   /* previous map is gone; leave the view */

  v->group = ng; v->num = nn; v->hdr = nh; v->lay = nl;
  /* A NEGATIVE coordinate means "centre me": the region map names a PLACE, not a tile,
   * so zooming into a section should land in the middle of it rather than in its
   * top-left corner (which is usually out-of-bounds scenery). */
  v->cx = (nx < 0) ? (v->lay.width  / 2) : nx;
  v->cy = (ny < 0) ? (v->lay.height / 2) : ny;
  if (v->cx < 0) v->cx = 0;
  if (v->cx >= v->lay.width)  v->cx = v->lay.width  - 1;
  if (v->cy < 0) v->cy = 0;
  if (v->cy >= v->lay.height) v->cy = v->lay.height - 1;
  v->hold = 0;                       /* or a still-held D-pad re-crosses the seam */
  v->repairs_map = 0;                /* the per-map repair budget is per LOAD */
  /* NPCs are per-map and npc_gfx walks the ROM, so this belongs in the same paused window
   * as the load, not in the render loop. */
  rmbl_pause();
  moam_load_map(v->rc, nh.events, v->sb1, v->game);
  scan_secret_bases(nh.events);        /* patches g.blocks, so BEFORE the ring fill */
  apply_base_decor();                  /* ditto — the interior template is only half the room */
  /* Both of those DELIBERATELY rewrite blockdata that mgfx_load already fingerprinted, so the
   * integrity checker would report the map as corrupting itself, repair it (which re-applies
   * the same patches), and eventually give up with "DISPLAY UNSTABLE". Re-baseline here: this
   * is the last point at which the blockdata is intentionally changed. */
  mgfx_fp_capture(&gfx, &mr, v->arena);
  rmbl_resume();
  v->in_base = (ng == SB_INTERIOR_GROUP);
  gfx.ring_valid = false;
  camera_refresh();
  if (v->zoom == 0) mgfx_z0_show();
  player_visibility();
  v->hud_dirty = true;
  return true;
}

/* The player's OWN overworld sprite id.
 *   RSE  : Brendan 0 / May   89
 *   FRLG : Red     0 / Green  7
 * playerGender is SaveBlock2 byte 8 in every Gen-3 game (0 = male, 1 = female).
 * This used to pass 8 for "female", which is not a player sprite in ANY of these games —
 * it is a generic girl NPC, which is exactly what showed up on screen. The ids are far
 * apart and per-family, so they have to be a table, not an arithmetic guess. */
static uint16_t player_gfx_id(PkGame game, const uint8_t* sb2) {
  bool female = (sb2 && sb2[8] != 0);
  if (game == PK_FRLG) return female ? 7u : 0u;
  return female ? 89u : 0u;
}

/* The character is only ON the currently loaded map when that map IS their map. Their
 * stored position is map-LOCAL and carries no map identity, so without this gate walking
 * across a seam draws them again at the same local cell on every neighbour — Guy saw the
 * character "reappear again and again" while panning left. While CARRYING they ride the
 * cursor and are legitimately on whatever map you are looking at. */
static void player_visibility(void) {
  MapViewState* v = &s_mv;
  moam_player_visible(v->carrying ||
                      (v->group == v->home_group && v->num == v->home_num));
}

/* moam_init() resets the whole sprite layer INCLUDING the player slot, and map_switch only
 * reloads the NPCs — so every moam_init() must be paired with a set_player or the character
 * silently disappears for the rest of the session (QA pass 2, defect 1: the sprite vanished
 * after any region-map visit and grab then drew a dot with no sweat). Doing both here makes
 * the pairing impossible to forget. Both calls read the ROM, so callers must be paused. */
static void sprites_rearm(void) {
  MapViewState* v = &s_mv;
  moam_init(v->rc);
  moam_set_player(v->rc, player_gfx_id(v->game, v->sb2), v->home_x, v->home_y);
}

/* Find the secret-base entrances on the freshly loaded map, mark the occupied ones OPEN in
 * the blockdata, and remember them for the HUD and for START.
 *
 * Opening the tile is exactly what the game does at map load
 * (SetOccupiedSecretBaseEntranceMetatiles) — a base you have registered shows a tunnel
 * mouth in the shrub/tree/cave instead of a plain tile. Patching g.blocks is the same
 * mechanism, so the render is byte-identical to the game's. */
static void scan_secret_bases(uint32_t events_addr) {
  MapViewState* v = &s_mv;
  s_ent_n = 0;
  if (!sbmap_supported((int)v->game)) return;

  RomMapEvents ev;
  if (!rom_map_events(v->rc, events_addr, &ev) || !ev.bg_count) return;

  /* The save's registry: slot 0 is the player's own base, 1..19 are friends'.
   * Read the two fields we need straight out of the 160-byte records rather than calling
   * sb_read_all — that fills a 20-entry SbRecord[] carrying six-Pokemon parties, about
   * 3 KB of EWRAM, to answer "is this door occupied and whose is it". EWRAM has ~5 KB
   * spare in total, so the full struct is not affordable here. Offsets per
   * gen3_secretbase.h: id @0x00, trainerName[7] @0x02 (Gen-3 encoded). */
  const uint8_t* regs = 0;
  uint32_t off = gen3_secret_base_offset(v->game == PK_EMERALD ? G3_VER_EMERALD : G3_VER_RS);
  if (v->sb1) regs = v->sb1 + off;

  for (int i = 0; i < ev.bg_count && s_ent_n < SB_MAX_ENTRANCE; i++) {
    RomBgEvent bg;
    if (!rom_bg_event(v->rc, &ev, i, &bg)) continue;
    if (bg.kind != ROM_BG_SECRET_BASE) continue;
    SbEnt* e = &s_ent[s_ent_n];
    e->x = bg.x; e->y = bg.y; e->id = (uint8_t)bg.param;
    e->occupied = false; e->owner[0] = 0;
    if (regs && e->id) {
      for (int r = 0; r < SB_COUNT; r++) {
        const uint8_t* rec = regs + (uint32_t)r * SB_RECORD;
        if (rec[0x00] != e->id) continue;
        e->occupied = true;
        int oc = 0;
        for (; oc < 7; oc++) {
          char ch = gen3_decode_char(rec[0x02 + oc]);
          if (!ch) break;
          e->owner[oc] = ch;
        }
        e->owner[oc] = 0;
        break;
      }
    }
    /* Open the door for a base that exists. Vacant entrances stay shut, as in the game. */
    if (e->occupied &&
        (unsigned)e->x < (unsigned)v->lay.width && (unsigned)e->y < (unsigned)v->lay.height) {
      uint32_t idx = (uint32_t)e->y * (uint32_t)v->lay.width + (uint32_t)e->x;
      uint16_t cell = gfx.blocks[idx];
      uint16_t open = sbmap_open_metatile(ROM_CELL_METATILE(cell));
      if (open) gfx.blocks[idx] = (uint16_t)((cell & ~0x03FFu) | open);
    }
    s_ent_n++;
  }
  if (s_ent_n) log_line("map: %d secret-base entrance(s)", s_ent_n);
}

static const SbEnt* sb_entrance_at(int x, int y) {
  for (int i = 0; i < s_ent_n; i++) if (s_ent[i].x == x && s_ent[i].y == y) return &s_ent[i];
  return 0;
}

/* The blockdata cell under the cursor. The cursor is always inside the current map (it
 * switches maps the instant it would leave), so this never needs the off-map path. */
static uint16_t cell_here(int x, int y) {
  if ((unsigned)x < (unsigned)s_mv.lay.width && (unsigned)y < (unsigned)s_mv.lay.height)
    return gfx.blocks[(uint32_t)y * (uint32_t)s_mv.lay.width + (uint32_t)x];
  return 0;
}

/* A DYNAMIC warp (mapNum 0x7F) has no destination in the ROM at all — the game picks one
 * at runtime from state a viewer does not have. That is not a rare curiosity: MEASURED
 * 32 Emerald / 30 Ruby / 9 FireRed maps have NO connections and ONLY dynamic warps, and
 * FireRed's own test save starts on one. With no back-stack key by design, such a room
 * would be a dead end you can only leave by quitting the whole screen.
 *
 * The save already knows the answer. `escapeWarp` (SaveBlock1+0x24) is where Dig and an
 * Escape Rope drop you — i.e. the game's own "get me out of here" destination, kept
 * current by the game itself. Offering it turns a dead end into the exit the player would
 * actually have taken. Read-only: we follow it, we never write it. */
static bool escape_warp_of(const uint8_t* sb1, uint8_t* g, uint8_t* n, int* x, int* y) {
  if (!sb1) return false;
  const uint8_t* w = sb1 + G3W_ESCAPE_OFF;
  int8_t  eg = (int8_t)w[0], en = (int8_t)w[1];
  int16_t ex = (int16_t)(w[4] | (w[5] << 8));
  int16_t ey = (int16_t)(w[6] | (w[7] << 8));
  if (eg < 0 || en < 0) return false;           /* never set on a fresh save */
  *g = (uint8_t)eg; *n = (uint8_t)en; *x = ex; *y = ey;
  return true;
}

/* The warp under the cursor, or NULL. First match wins, matching GetWarpEventAtPosition's
 * ascending scan (measured: no real map has two warps on one tile). Deliberately NOT
 * filtered by metatile behaviour — MEASURED 120 Emerald / 118 Ruby / 263 FireRed warps sit
 * on non-trigger tiles because they are arrival-only landing spots, and hiding those would
 * make rooms look like dead ends. */
static const RomWarp* warp_at(int x, int y) {
  for (int i = 0; i < gfx.warp_n; i++)
    if (gfx.warps[i].x == x && gfx.warps[i].y == y) return &gfx.warps[i];
  return 0;
}

/* The map's DIVE or EMERGE connection, if it has one.
 *
 * Diving is not a warp and not a normal connection: it is a whole-map link recorded as a
 * connection with direction 5 (down, to the underwater map) or 6 (up, back to the surface).
 * The destination keeps the SAME coordinates — that is what makes the two maps feel like one
 * place seen from two depths — so unlike a warp there is no landing tile to look up.
 *
 * Offered anywhere on the map rather than only on deep-water tiles. This viewer already lets
 * you stand on unwalkable ground on purpose, and the destination is equally real from any
 * cell; requiring the exact dive-capable behaviour would just make the feature hard to find. */
static bool dive_conn(uint8_t dir, uint8_t* g, uint8_t* n) {
  MapViewState* v = &s_mv;
  if (!v->hdr.connections) return false;
  int cnt = rom_connection_count(v->rc, v->hdr.connections);
  for (int i = 0; i < cnt; i++) {
    RomConnection c;
    if (!rom_connection(v->rc, v->hdr.connections, i, &c)) break;
    if (c.direction != dir) continue;
    *g = c.group; *n = c.num;
    return true;
  }
  return false;
}

/* A dialog that NEVER LEAVES MODE 0.
 *
 * The obvious implementation — mgfx_exit() to Mode 3, draw a ui_panel, mgfx_enter() back —
 * is actively destructive here. In Mode 3 the bitmap framebuffer occupies
 * 0x06000000..0x06012BFF, which IS the map's tileset char data. So a Mode-3 dialog (a) is
 * drawn on top of the tileset rendered as pixels, which QA correctly reported as
 * "confetti", and (b) OVERWRITES the tiles, so returning to the map needs a full SD
 * reload that the old dialog_back() never did.
 *
 * Instead: switch the map layers off and draw with the HUD's own tte context. BG0 lives in
 * CBB3 + SBB31 and uses palette bank 15, none of which a map load ever writes, so the text
 * is guaranteed legible and nothing in VRAM is harmed. Cost: no SD access, no reload. */
static u16 map_dialog(const char* title, const char* l1, const char* l2,
                      const char* prompt, u16 mask) {
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;          /* map hidden; backdrop is black */
  tte_erase_screen();
  tte_set_pos(8, 46);  tte_write(title);
  if (l1) { tte_set_pos(8, 66); tte_write(l1); }
  if (l2) { tte_set_pos(8, 78); tte_write(l2); }
  tte_set_pos(8, 108); tte_write(prompt ? prompt : "A ok");
  u16 k = s_wait(mask);
  s_flush_keys();
  s_mv.hud_dirty = true;                        /* the HUD text was erased */
  return k;
}

/* A paged text page for NPC dialog. map_dialog's two fixed lines cannot hold a real Gen-3
 * message: the decoder emits its own newlines and the longest Emerald line runs past 40
 * characters. Same Mode-0 discipline as map_dialog — the map layers go off, BG0's tte draws,
 * nothing in VRAM is touched, so there is no reload afterwards.
 *
 * Wraps on spaces at COLS characters and pages at ROWS lines. Word-wrapping rather than
 * hard-cutting matters here because the source text is prose the user is meant to read. */
static void map_text_page(const char* title, const char* body) {
  enum { COLS = 28, ROWS = 9 };
  char line[COLS + 1];
  const char* p = body;

  for (;;) {
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    tte_erase_screen();
    tte_set_pos(4, 8); tte_write(title);

    int row = 0;
    const char* page_end = p;
    while (row < ROWS && *page_end) {
      /* One display line: up to COLS chars, broken at the last space that fits, or at an
       * embedded newline, whichever comes first. */
      const char* q = page_end;
      int take = 0, brk = -1;
      while (take < COLS && q[take] && q[take] != '\n') {
        if (q[take] == ' ') brk = take;
        take++;
      }
      if (q[take] == '\n') {
        /* keep `take` — the newline itself is consumed below */
      } else if (q[take] && brk > 0) {
        take = brk;                      /* back up to the space so a word is not split */
      }
      int i = 0;
      for (; i < take; i++) line[i] = q[i];
      line[i] = 0;
      tte_set_pos(4, 26 + row * 12); tte_write(line);
      page_end += take;
      while (*page_end == ' ') page_end++;
      if (*page_end == '\n') page_end++;
      row++;
    }

    bool more = (*page_end != 0);
    tte_set_pos(4, 146); tte_write(more ? "A more   B close" : "A/B close");
    u16 k = s_wait(KEY_A | KEY_B);
    s_flush_keys();
    if (!more || (k & KEY_B)) break;
    p = page_end;
  }
  s_mv.hud_dirty = true;
}

/* Put the map back after a dialog. No reload needed — nothing touched VRAM. */
static void dialog_done(void) {
  MapViewState* v = &s_mv;
  if (v->zoom == 0) mgfx_z0_show(); else mgfx_zoom_show(v->zoom, v->cx, v->cy);
  v->hud_dirty = true;
}

/* Ask, then write. Returns true if the placement was committed.
 *
 * WHAT THIS WRITES, and why it is the safe mechanism: g3warp_apply touches exactly 8 bytes
 * of SaveBlock1 (continueGameWarp) plus one bit in SaveBlock2 — NOT `pos`/`location`. On
 * Continue the game boots the map as it was saved (self-consistent, because nothing about
 * it changed) and then runs its own warp code, which rewrites location/pos/mapLayoutId/
 * weather/music/flash and respawns the destination's object events. That is literally the
 * path retail Emerald uses to pull the player out of a link room. Patching `pos` directly
 * instead would require keeping five interdependent fields consistent and would let
 * LoadSavedMapView blit past the end of its own buffer on a smaller destination map. */
static bool do_drop(void) {
  MapViewState* v = &s_mv;
  char l1[40], l2[40];

  if (!app_can_edit()) {
    map_dialog("READ-ONLY", "Needs EZ-Flash Omega.", "Cannot place here.", "A ok",
               KEY_A | KEY_B);
    dialog_done();
    return false;
  }

  G3MapBounds b = { v->group, v->num, (uint16_t)v->lay.width, (uint16_t)v->lay.height, true };
  G3Warp w; memset(&w, 0, sizeof w);
  w.group = v->group; w.num = v->num;
  w.x = (int16_t)v->cx; w.y = (int16_t)v->cy;

  int chk = g3warp_check(&b, &w);
  if (chk == G3W_BAD_MAP || chk == G3W_OUT_OF_BOUNDS) {
    map_dialog("CANNOT PLACE", "That spot is outside", "the map.", "A ok", KEY_A | KEY_B);
    dialog_done();
    return false;
  }

  /* Guy asked for unwalkable spots to be allowed, "but be warned" — so warn, do not block.
   * Collision != 0 means the game will not let you walk OFF this tile normally; you can
   * still Fly/Dig/Teleport out, which is why this is a warning and not a refusal. */
  uint16_t cell = cell_here(v->cx, v->cy);
  bool blocked = (ROM_CELL_COLLISION(cell) != 0);
  bool risky   = g3warp_risky_map(v->game, v->group, v->num) || (chk == G3W_RISKY_MAP);

  const char* nm = map_name(v->game, v->hdr.mapsec);
  siprintf(l1, "%.16s (%d,%d)", nm ? nm : "?", v->cx, v->cy);
  /* Only ever a WARNING line here — never repeat the key prompt, which the dialog draws
   * itself. Guy asked to be allowed onto unwalkable tiles "but be warned", so this is the
   * warning, not a refusal. */
  if      (blocked && risky) siprintf(l2, "BLOCKED tile + odd map!");
  else if (blocked)          siprintf(l2, "Tile is NOT walkable!");
  else if (risky)            siprintf(l2, "Odd map - may not load.");
  else                       l2[0] = 0;

  u16 k = map_dialog("PLACE CHARACTER HERE?", l1, l2[0] ? l2 : 0,
                     "A place   B cancel", KEY_A | KEY_B);
  if (!(k & KEY_A)) { dialog_done(); return false; }

  /* One snapshot per visit, taken before the FIRST write, so undo always returns to where
   * the character actually was rather than to the previous experiment. */
  if (!v->have_snap) { g3warp_snapshot(v->sb1, v->sb2, v->snap); v->have_snap = true; }

  /* LEAVE MODE 0 BEFORE COMMITTING. app_commit_sb12() draws its own progress and result
   * panels through the shared Mode-3 UI (busy_panel / grow_in / msg_wait -> ui_clear ->
   * m3_fill), and in Mode 3 that framebuffer IS the tileset char VRAM plus the HUD font
   * charblock. Running it while still in Mode 0 destroyed CBB0/CBB1/CBB3 and turned the
   * whole screen into opaque noise — including the "do not power off" and "SAVED" panels,
   * so a FAILED flash write would have been invisible too (QA pass 2, defect 2).
   * Mode 3 is where those panels belong; the map is rebuilt afterwards. */
  mgfx_exit();
  bool ok = g3warp_apply(v->sb1, v->sb2, v->game, &w) && app_commit_sb12();
  log_line("map: place %u.%u (%d,%d) blocked=%d risky=%d -> %s",
           v->group, v->num, v->cx, v->cy, (int)blocked, (int)risky, ok ? "OK" : "FAILED");
  if (ok) {
    v->placed = true;
    v->home_group = v->group; v->home_num = v->num;
    v->home_x = v->cx; v->home_y = v->cy;
    /* Say it, because the map view still shows `pos`, not the pending warp — otherwise a
     * successful placement reads as if it did nothing. */
    s_msg("PLACED", UI_OK, "Load your save to appear", "there. Map shows old spot.");
  } else {
    s_msg("WRITE FAILED", UI_WARN, "Nothing was changed.", 0);
  }

  /* Mode 3 has eaten the tilesets and the arena is untouched but the screen is not, so the
   * map is rebuilt from scratch: re-enter Mode 0, re-arm the sprites, reload the map. */
  mgfx_enter();
  rmbl_pause(); sprites_rearm(); rmbl_resume();
  if (!map_switch(v->group, v->num, v->cx, v->cy)) v->fatal = true;
  return ok;
}

/* Paint the owner's decorations onto a secret-base interior.
 *
 * A group-25 map in the ROM is a bare TEMPLATE — floor, walls, the PC. Everything that makes
 * it somebody's base lives in that owner's 160-byte save record, and the game composites it
 * into the block layout at load time. Rendering the template alone shows a room the player
 * has never seen, which is what Guy reported.
 *
 * Runs inside map_switch's paused window, right after scan_secret_bases and BEFORE the ring
 * fill, because it patches gfx.blocks[] and the tilemap is built from those.
 *
 * WHICH record: if the player walked in through a door, the BG event gave us the exact
 * secretBaseId, so the answer is exact. Otherwise (the save itself starts inside a base —
 * Guy's Emerald save does) it has to be inferred, and it can be genuinely ambiguous: several
 * secretBaseIds share one interior map. Guessing wrong would draw a stranger's furniture, so
 * an ambiguous match draws NOTHING and says so in the log. */
static void apply_base_decor(void) {
  MapViewState* v = &s_mv;
  if (v->group != SB_INTERIOR_GROUP || !v->sb1) return;

  uint32_t off = gen3_secret_base_offset(v->game == PK_EMERALD ? G3_VER_EMERALD : G3_VER_RS);
  if (!off) return;                                   /* FRLG has no secret bases at all */
  const uint8_t* regs = v->sb1 + off;
  const uint8_t* rec = 0;
  const char* how = "";
  int slot = -1;

  if (v->base_id) {
    /* Walked in through a door: the BG event handed us the exact secretBaseId. */
    for (int r = 0; r < SB_COUNT; r++)
      if (regs[(uint32_t)r * SB_RECORD] == v->base_id) {
        rec = regs + (uint32_t)r * SB_RECORD; slot = r; how = "door"; break;
      }
  } else {
    /* The save itself starts inside a base — Guy's Emerald save does. Ask the game: it
     * keeps the record INDEX in VAR_CURRENT_SECRET_BASE, and that is the same value
     * InitSecretBaseAppearance uses, so agreeing with it is exact rather than inferred. */
    int idx = sbmap_cur_base_index(v->sb1, v->game);
    if (idx >= 0) {
      const uint8_t* q = regs + (uint32_t)idx * SB_RECORD;
      if (q[0] && sbmap_interior(q[0]) == (int)v->num) { rec = q; slot = idx; how = "var"; }
    }
    if (!rec) {
      /* The var was stale or disagreed. Fall back to matching the interior — but only when
       * the answer is unique. secretBaseId/10 collapses 240 ids onto 24 rooms, so with a
       * full registry roughly half the records are ambiguous, and drawing a stranger's
       * furniture is worse than drawing none. */
      int hits = 0;
      for (int r = 0; r < SB_COUNT; r++) {
        const uint8_t* q = regs + (uint32_t)r * SB_RECORD;
        if (!q[0] || sbmap_interior(q[0]) != (int)v->num) continue;
        hits++; rec = q; slot = r;
      }
      if (hits != 1) {
        log_line("sbdecor: 25.%u ambiguous (%d records) - bare template", v->num, hits);
        return;
      }
      how = "match";
    }
  }
  if (!rec) { log_line("sbdecor: no record for base %u", v->base_id); return; }

  SbDecorStat st;
  bool ok = sbdecor_apply(v->rc, &v->lay, rec, slot, gfx.blocks, &st);

  /* The dolls. The ROM's 14 placeholders are junk wherever they sit, so they go regardless
   * of whether the real ones can be resolved — showing nothing is honest, showing the
   * template's is not. Then re-add the owner's, which sbdecor_sprites gates on the holder
   * metatile the pass above just painted. */
  int nsp = 0;
  if (ok) {
    SbDecorSprite sp[SBD_MAX_SPRITES];
    nsp = sbdecor_sprites(v->rc, &v->lay, rec, gfx.blocks, sp);
    moam_drop_gfx_range(240, 253);
    for (int i = 0; i < nsp; i++) moam_add_sprite(sp[i].gfx, sp[i].x, sp[i].y);
  }
  log_line("sbdecor: base %u slot %d (%s) on 25.%u -> %s placed %u sprite %u/%u bad %u",
           rec[0], slot, how, v->num, ok ? "ok" : "NO TABLE", st.placed, nsp, st.sprites, st.bad);
  v->decor_sprites = (uint8_t)nsp;
}

/* Put a collected item ball back on the ground.
 *
 * The whole edit is ONE BIT: the ball's own flag. The game spawns an item-ball object iff
 * its flag is clear and runs `finditem` when you press A on it, so clearing the flag hands
 * the item back exactly the way the game itself would — no bag write, no quantity to get
 * wrong, no way to exceed a stack limit. It also means the ball is re-collectable rather
 * than granted, which is what Guy asked for ("reactivate them").
 *
 * Only flags[] is touched, so this commits SB1 alone. Same Mode-0 rule as do_drop: leave
 * Mode 0 before app_commit_* draws its Mode-3 panels over the tileset VRAM. */
static bool restore_item_ball(const RomObjectEvent* o) {
  MapViewState* v = &s_mv;
  char l1[40];

  if (!app_can_edit()) {
    map_dialog("READ-ONLY", "Needs EZ-Flash Omega.", "Cannot restore items.", "A ok",
               KEY_A | KEY_B);
    dialog_done();
    return false;
  }
  if (!o->flag_id || !v->sb1) { snd_deny(); return false; }

  /* SD I/O: the motor is a cart-bus write and this read walks the ROM off the card.
   * These two call sites were the only ROM reads on this screen outside the bracket. */
  rmbl_pause();
  uint16_t it = rom_script_item_ball(v->rc, o->script);
  rmbl_resume();
  const char* nm = it ? pk_item_name(it) : 0;
  siprintf(l1, "%.20s", nm ? nm : "This item");
  u16 k = map_dialog("PUT IT BACK?", l1, "It can be picked up again.",
                     "A restore   B cancel", KEY_A | KEY_B);
  if (!(k & KEY_A)) { dialog_done(); return false; }

  mgfx_exit();
  pk_flag_set(v->sb1, v->game, o->flag_id, false);
  bool ok = app_commit_sb1();
  log_line("map: restore ball flag %u item %u on %u.%u (%d,%d) -> %s",
           o->flag_id, it, v->group, v->num, o->x, o->y, ok ? "OK" : "FAILED");
  if (ok) s_msg("RESTORED", UI_OK, nm ? nm : "The item", "is back on the ground.");
  else {
    /* The write failed, so the save on the card is unchanged — but the in-RAM copy is not.
     * Put the bit back or the greyed ball would silently disagree with the card. */
    pk_flag_set(v->sb1, v->game, o->flag_id, true);
    s_msg("WRITE FAILED", UI_WARN, "Nothing was changed.", 0);
  }

  mgfx_enter();
  rmbl_pause(); sprites_rearm(); rmbl_resume();
  if (!map_switch(v->group, v->num, v->cx, v->cy)) v->fatal = true;
  return ok;
}

/* What this NPC says, read live out of the ROM's own script bytecode.
 *
 * rom_script.h is explicit that this is a WALKER, not an interpreter: it cannot know which
 * branch the player's flags would take, so it reports how sure it is. Surface that honestly
 * — "SAYS" only when no conditional was passed, "MAY SAY" otherwise — because a confident
 * wrong line is worse than an admitted guess. */
static void show_npc_dialog(const RomObjectEvent* o) {
  MapViewState* v = &s_mv;
  static char EWRAM_BSS text[512];
  RomDialogRef ref;

  if (!o->script) {
    map_dialog("NO SCRIPT", "This one has nothing", "to say.", "A ok", KEY_A | KEY_B);
    dialog_done();
    return;
  }
  rmbl_pause();
  bool got = rom_script_find_dialog(v->rc, o->script, &ref);
  int n = got ? rom_text_decode(v->rc, ref.text_addr, text, sizeof text, 0) : -1;
  rmbl_resume();

  if (n <= 0) {
    map_dialog("NO DIALOG FOUND", "Its script does not lead", "to a plain text box.",
               "A ok", KEY_A | KEY_B);
    dialog_done();
    return;
  }
  map_text_page(ref.certainty == ROM_DLG_CERTAIN ? "SAYS" : "MAY SAY", text);
  dialog_done();
}

static void map_view(const RomCtx* rc, uint8_t* arena, PkGame game,
                     uint8_t group, uint8_t num, const G3Warp* start,
                     uint8_t* sb1, uint8_t* sb2) {
  MapViewState* v = &s_mv;
  memset(v, 0, sizeof *v);
  v->rc = rc; v->arena = arena; v->game = game; v->sb1 = sb1; v->sb2 = sb2;
  v->group = group; v->num = num;

  mgfx_enter();                                    /* screen blanked from here */
  rmbl_pause();
  bool ok = rom_map_header(rc, group, num, &v->hdr)
         && rom_layout(rc, v->hdr.layout, &v->lay)
         && mgfx_can_load(rc, &v->lay, APP_ARENA_BYTES)
         && mgfx_load(&gfx, &mr, rc, &v->lay, v->hdr.connections, v->hdr.events,
                      arena, APP_ARENA_BYTES);
  rmbl_resume();

  log_line("map: load %s  peak %lu  phase2 %lu / %lu  %ldx%ld  conn %u warp %u  mt-miss %lu  cbb2 %lu",
           ok ? "OK" : "FAILED", (unsigned long)mgfx_arena_peak(),
           (unsigned long)mgfx_arena_phase2(), (unsigned long)APP_ARENA_BYTES,
           (long)v->lay.width, (long)v->lay.height, gfx.conn_n, gfx.warp_n,
           (unsigned long)mgfx_mt_misses(), (unsigned long)mgfx_cbb2_bad());

  if (!ok) {
    mgfx_exit();
    s_msg("MAP LOAD FAILED", UI_WARN, "Could not read tilesets.", "Re-copy the ROM?");
    return;
  }

  v->cx = start->x; v->cy = start->y;
  if (v->cx < 0) v->cx = 0;
  if (v->cx >= v->lay.width)  v->cx = v->lay.width - 1;
  if (v->cy < 0) v->cy = 0;
  if (v->cy >= v->lay.height) v->cy = v->lay.height - 1;
  v->home_group = group; v->home_num = num;
  v->home_x = v->cx; v->home_y = v->cy; v->at_home = true;

  /* Sprite layer: still blanked, still paused — both of these read the ROM. The player's
   * own overworld sprite is graphics id 0 (Brendan) or 8 (May); playerGender is SaveBlock2
   * byte 8 in every Gen-3 game. */
  rmbl_pause();
  bool sprites_ok = moam_init(rc);
  if (sprites_ok) {
    moam_load_map(rc, v->hdr.events, sb1, game);
    moam_set_player(rc, player_gfx_id(game, sb2), v->cx, v->cy);
    scan_secret_bases(v->hdr.events);
  }
  /* The save can START inside a secret base — Guy's Emerald save does — so the initial load
   * needs the decoration pass just as much as map_switch does. Outside the `sprites_ok`
   * guard on purpose: decorations are block data, not sprites. */
  apply_base_decor();
  mgfx_fp_capture(&gfx, &mr, v->arena);   /* see map_switch: the patches are intentional */
  rmbl_resume();
  log_line("map: sprites %s", sprites_ok ? "ok" : "unavailable");

  mgfx_camera(&gfx, &mr, v->cx, v->cy);
  player_visibility();
  sprites_follow();
  mgfx_show();

  const u16 DPAD = KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN;
  /* Hold SELECT while the view opens to DETECT ONLY: log and mark, never repair. For
   * root-cause work you want the corrupt frame left on screen to photograph — an
   * auto-repair that silently hides the fault is worse than the fault. */
  v->detect_only = (key_curr_state() & KEY_SELECT) != 0;
  if (v->detect_only) log_line("map: integrity DETECT-ONLY (SELECT held)");
  v->hud_dirty = true;
  for (;;) {
    s_vsync();
    v->frame++;

    /* ---- render integrity ------------------------------------------------
     * One slice per frame plus the canary: ~0.5% of a frame. Read VRAM in the slot right
     * after VBlankIntrWait. Anything that diverges here was written by something that had
     * no business writing it — the map's own per-step output (screenblocks, the affine
     * map, the mips) is deliberately NOT fingerprinted. */
    /* TILE VIEW ONLY. The region view legitimately owns CBB2, the affine tilemap, the BG
     * palette AND the scratch buffer (hard rule 0: mr_region_enter stages its compressed
     * stream through the arena), so every one of those fingerprints diverges by design
     * while it is up. Checking there reported a real invariant violation that happens to
     * be intentional — which is exactly the false positive it produced on first run. The
     * reload on the way back down recaptures. */
    if (v->zoom <= MGFX_ZOOM_TILE_MAX) {
      uint32_t bad = mgfx_fp_check(&gfx, &mr, v->arena, (int)(v->frame & (MGFX_FP_SLICES - 1)));
      if (bad) {
        bool may = !v->detect_only
                && (int)(v->frame - (uint32_t)v->last_repair) > 30
                && v->repairs_map < 4 && v->repairs < 12;
        log_line("map: CORRUPT %s bits=%02lX at %u.%u zoom %d  repair %d/%d %s",
                 mgfx_fp_name(bad), (unsigned long)bad, v->group, v->num, v->zoom,
                 v->repairs_map, v->repairs, may ? "yes" : "NO");
        /* Straight to the card. Until now NOTHING in this file flushed, so every map
         * diagnostic died at power-off — which is how the corruption stayed a photograph
         * instead of a log line. The user sees a glitch and pulls the plug; that is the
         * one moment the evidence has to already be on the SD. */
        app_log_flush();
        v->repairs++; v->repairs_map++; v->last_repair = (int)v->frame;
        v->hud_dirty = true;
        if (may) {
          /* T2: the existing map_switch IS the repair. It already encodes the whole
           * OS-mode contract, so do not write a second reload path. */
          bool was_carrying = v->carrying;
          loading_frame(map_name(v->game, v->hdr.mapsec));   /* forced: tilesets are suspect */
          rmbl_pause(); sprites_rearm(); rmbl_resume();
          if (!map_switch(v->group, v->num, v->cx, v->cy)) { v->fatal = true; break; }
          v->carrying = was_carrying;
          moam_carry(was_carrying, v->cx, v->cy);
          player_visibility();
          if (!v->said_repaired) {
            v->said_repaired = true;
            map_dialog("DISPLAY REPAIRED", "Map data went bad.",
                       "Reloaded from your ROM.", "A ok", KEY_A | KEY_B);
            dialog_done();
          }
        } else if (v->repairs >= 12 || v->repairs_map >= 4) {
          if (!v->said_repaired) {
            v->said_repaired = true;
            map_dialog("DISPLAY UNSTABLE", "Map data keeps going bad.",
                       "B leaves the map.", "A ok", KEY_A | KEY_B);
            dialog_done();
          }
        }
        continue;                       /* skip this frame's input; state just changed */
      }
    }

    u16 down = key_curr_state();
    int dx = 0, dy = 0;
    /* Above the tile levels the cursor is meaningless — the region map shows whole towns,
     * not tiles — so movement, grabbing and warping are all inert there. */
    const bool in_region = (v->zoom > MGFX_ZOOM_TILE_MAX);

    /* One press = one metatile, holding glides. key_hit alone makes a 100-tile route
     * unusable; auto-repeat alone makes single steps impossible. */
    bool fresh = (key_hit(DPAD) != 0);
    if (fresh) v->hold = 0;
    bool step = fresh;
    if (!fresh && (down & DPAD)) { if (++v->hold > 10) { v->hold = 9; step = true; } }

    /* SHARPEN THE ZOOMED-OUT VIEW WHEN PANNING STOPS.
     *
     * At 1/4 the affine layer can only hold 242 exact metatiles and the window can need 428,
     * so the rest are flat colour. Which 242 is chosen matters enormously, and the lazy
     * first-come dictionary picks badly once you have panned around (see mgfx_zoom_optimise).
     * Re-ranking costs up to 242 metatile renders, so it must not run per step — but the
     * moment the D-pad is released the frame is static and nobody notices the work.
     *
     * Gated on >1% filler and on the window not already being optimised, so a view that is
     * genuinely irreducible (every tile unique) settles after ONE pass instead of thrashing. */
    if (!in_region && v->zoom == 2 && !(down & DPAD) && !v->carrying &&
        mgfx_zoom_wants_opt(&gfx, v->cx, v->cy)) {
      int was = mgfx_zoom_filler_pct(&gfx);
      rmbl_pause();
      mgfx_zoom_optimise(&gfx, &mr, v->cx, v->cy);
      rmbl_resume();
      /* optimise only rewrites the DICTIONARY and invalidates the tilemap; the rebuild is
       * what re-resolves every cell and recounts the filler. Show alone would leave the old
       * tilemap on screen pointing at slots that now mean something else. */
      mgfx_zoom_build(&gfx, &mr, v->cx, v->cy, v->zoom);
      mgfx_zoom_show(v->zoom, v->cx, v->cy);
      log_line("z2opt: (%d,%d) filler %d%% -> %d%% (%d exact)",
               v->cx, v->cy, was, mgfx_zoom_filler_pct(&gfx), gfx.slots_used);
      v->hud_dirty = true;
    }
    if (step) {
      /* ONE STEP = ONE METATILE was why panning felt sluggish when zoomed out: the cursor
       * always moved 1 map cell, but at 1/2 that is half the screen distance and at 1/4 a
       * quarter of it, so crossing a route took 2x / 4x as many presses. Scaling the step
       * with the zoom keeps the SCREEN speed constant — which is what "flying over the
       * map" should feel like — and costs nothing: the window rebuild is per step, not
       * per cell, so bigger steps are strictly cheaper per screen travelled. */
      /* A SINGLE TAP ALWAYS MOVES EXACTLY ONE TILE. Scaling every step by the zoom made
       * the cursor land only on multiples of 4 at 1/4 zoom, so three tiles in four became
       * UNREACHABLE — which is why the Safari Zone passages had "nowhere to press START":
       * the warp tiles were simply being skipped over. Only auto-repeat scales. */
      int sstep = 1;
      if (!fresh) {
        sstep = 1 << v->zoom;                   /* 1 at 1:1, 2 at 1/2, 4 at 1/4 */
        if (v->zoom > MGFX_ZOOM_TILE_MAX) sstep = 1;
        if (v->hold > 60) sstep *= 2;           /* ~1 s of holding, then twice as fast */
      }
      if (down & KEY_LEFT)  dx = -sstep;
      if (down & KEY_RIGHT) dx =  sstep;
      if (down & KEY_UP)    dy = -sstep;
      if (down & KEY_DOWN)  dy =  sstep;
    }

    if ((dx || dy) && in_region) {
      /* On the region map the D-pad moves the GAME'S OWN cursor between sections, one
       * cell at a time. mr_region_tick does the gliding (full view) or the background
       * scroll (zoomed view), matching what the game does in each mode. */
      int sx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
      int sy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
      if (mr_region_cursor_move(&s_rgn, sx, sy)) { if (fresh) snd_move(); v->hud_dirty = true; }
      else if (fresh) snd_deny();
    } else if (dx || dy) {
      int nx = v->cx + dx, ny = v->cy + dy;
      int dir = 0;
      /* Cross at most ONE axis per step: the cursor can leave on both at a corner, and the
       * game resolves a diagonal as EAST > WEST > SOUTH > NORTH (GetMapBorderIdAt). */
      if      (nx >= v->lay.width)  dir = ROM_CONN_EAST;
      else if (nx < 0)              dir = ROM_CONN_WEST;
      else if (ny >= v->lay.height) dir = ROM_CONN_SOUTH;
      else if (ny < 0)              dir = ROM_CONN_NORTH;

      if (!dir) {
        v->cx = nx; v->cy = ny;
        camera_refresh();
      } else {
        int along;
        if (dir == ROM_CONN_EAST || dir == ROM_CONN_WEST) {
          if (ny < 0) ny = 0; else if (ny >= v->lay.height) ny = v->lay.height - 1;
          along = ny;
        } else {
          if (nx < 0) nx = 0; else if (nx >= v->lay.width) nx = v->lay.width - 1;
          along = nx;
        }
        /* LAST match wins, matching the game's fill order. A map really can have more than
         * one connection in one direction, with GAPS between them showing the border. */
        const MapConn* c = 0;
        for (int i = (int)gfx.conn_n - 1; i >= 0 && !c; i--) {
          const MapConn* k = &gfx.conn[i];
          if (k->dir != dir) continue;
          if (rom_conn_contains(dir, k->off, k->nb_w, k->nb_h, along)) c = k;
        }
        if (!c) {
          snd_deny();                              /* edge of the world: clamp */
        } else {
          int bx2, by2;
          switch (dir) {
            case ROM_CONN_SOUTH: bx2 = nx - (int)c->off;  by2 = ny - v->lay.height; break;
            case ROM_CONN_NORTH: bx2 = nx - (int)c->off;  by2 = ny + (int)c->nb_h;  break;
            case ROM_CONN_WEST:  bx2 = nx + (int)c->nb_w; by2 = ny - (int)c->off;   break;
            default:             bx2 = nx - v->lay.width; by2 = ny - (int)c->off;   break;
          }
          map_switch(c->group, c->num, bx2, by2);
          if (v->fatal) break;
        }
      }
      if (fresh) snd_move();                       /* silent while gliding, or it buzzes */
      v->hud_dirty = true;
    }

    /* L zooms OUT, R zooms IN (Guy's preference — the reverse felt backwards). */
    int zdir = (key_hit(KEY_L) ? 1 : 0) - (key_hit(KEY_R) ? 1 : 0);

    /* L on the region map = the game's own "SWITCH MAP" button. On FRLG the region map has
     * four views (Kanto + three Sevii groups) that the retail game switches between rather
     * than zooming; L is already a DEAD key there because the zoom clamps at the top of the
     * ladder, so it costs nothing to give it this job. Handled before the zoom block so it
     * consumes the press. */
    if (zdir > 0 && in_region && s_rgn_ok && v->zoom >= MGFX_ZOOM_MAX) {
      /* L at the widest view = PLACES. On RS/Emerald this key was DEAD (one Hoenn view, and
       * the zoom is already clamped) — pressing it did nothing at all, which is exactly what
       * you would do looking for the Sky Pillar. */
      const bool frlg = (v->game == PK_FRLG);
      const OffMapPlace* pl = frlg ? PLACES_FRLG : PLACES_RSE;
      const int npl = frlg ? (int)(sizeof PLACES_FRLG / sizeof PLACES_FRLG[0])
                           : (int)(sizeof PLACES_RSE  / sizeof PLACES_RSE[0]);
      int views = mr_region_view_count(&s_rgn);
      int pick = places_menu(pl, npl, views, mr_region_view_label(&s_rgn));
      if (pick < 0) {                              /* cancelled: repaint the region view */
        mr_region_show(&s_rgn, v->zoom, v->hdr.mapsec);
      } else if (pick >= npl) {                    /* the folded "switch view" row */
        rmbl_pause();
        bool sw = mr_region_next_view(&s_rgn, v->arena, APP_ARENA_BYTES);
        rmbl_resume();
        mr_region_show(&s_rgn, v->zoom, v->hdr.mapsec);
        if (sw) snd_tab(); else snd_deny();
      } else {
        /* Leaving the region view DESTROYS the arena (hard rule 0), so the jump has to go
         * through the same reload path a zoom-down uses — map_switch is that path. A
         * negative coordinate means "centre me on the map".
         *
         * ORDER MATTERS, and getting it wrong cost two visible bugs. map_switch ends with
         * camera_refresh() + the Z0 show, both keyed off v->zoom, so the zoom has to be its
         * FINAL value BEFORE the switch: setting it afterwards built the view for one zoom
         * level and then displayed another, leaving a stale Z0 ring on screen — the wrong
         * tiles, which only a later zoom round trip rebuilt. And the region view runs
         * moam_shutdown(), so without sprites_rearm() the OAM stays dead: no cursor glove,
         * no NPCs, and a screen where nothing appears to respond. */
        mr_region_exit(&gfx, &mr);
        v->zoom = 0;                               /* the list names a place to LOOK at */
        rmbl_pause(); sprites_rearm(); rmbl_resume();
        if (map_switch(pl[pick].group, pl[pick].num, -1, -1)) snd_ok();
        else { v->zoom = MGFX_ZOOM_MAX; snd_deny(); }
      }
      v->hud_dirty = true;
      zdir = 0;
    }

    if (zdir) {
      int nz = v->zoom + zdir;
      if (nz < 0) nz = 0;
      if (nz > MGFX_ZOOM_MAX) nz = MGFX_ZOOM_MAX;
      if (nz != v->zoom) {
        int was = v->zoom;
        bool was_rgn = (was  > MGFX_ZOOM_TILE_MAX);
        bool now_rgn = (nz   > MGFX_ZOOM_TILE_MAX);

        if (now_rgn && !was_rgn) {
          /* Crossing INTO the region view. This must happen every time, not once: going
           * back down rebuilds the mip dictionary over CBB2 and the affine tilemap, so the
           * region data is gone by the next visit.
           *
           * ⚠️ IT ALSO DESTROYS THE ARENA. The compressed stream is staged there, and the
           * arena is where the map's blockdata, connection strips and warp list live — so
           * the map is NOT valid after this and has to be reloaded on the way back. This
           * was a real bug: VRAM hashes showed the tilesets intact but the Z0 tilemaps
           * rebuilt from overwritten blockdata, i.e. a scrambled map. */
          loading_frame("Region map");
          rmbl_pause();
          s_rgn_ok = mr_region_enter(&s_rgn, v->rc, v->arena, APP_ARENA_BYTES);
          rmbl_resume();
          s_rgn_tried = true;
          if (!s_rgn_ok) mr_region_exit(&gfx, &mr);
          gfx.ring_valid = false;
        }

        /* FRLG has ONE region view, not two. Landing on level 3 there would cost a
         * keypress that changes nothing, so step straight through it in whichever
         * direction we were travelling. Both 2 and 4 are inside the existing clamps. */
        if (now_rgn && s_rgn_ok && mr_region_no_zoom() && nz == 3) {
          nz = (zdir > 0) ? 4 : 2;
          now_rgn = (nz > MGFX_ZOOM_TILE_MAX);
        }

        if (now_rgn && !s_rgn_ok) {
          /* No usable region map in this ROM: refuse the level rather than show garbage.
           * The failed attempt still ate the arena, so rebuild the tile view properly. */
          v->zoom = was;
          snd_deny();
          rmbl_pause(); sprites_rearm(); rmbl_resume();
          if (!map_switch(v->group, v->num, v->cx, v->cy)) v->fatal = true;
        } else {
          v->zoom = nz;
          if (now_rgn) {
            if (!was_rgn) {
              moam_shutdown();                      /* no NPCs on a region map */
              mr_region_cursor_reset(&s_rgn, v->hdr.mapsec);
            }
            mr_region_show(&s_rgn, nz, v->hdr.mapsec);
            v->hud_dirty = true;
          } else if (was_rgn) {
            /* Coming back down. mr_region_exit restores the palettes and invalidates the
             * mip dictionary; the RELOAD is what restores the arena the region upload
             * consumed.
             *
             * ZOOM IN WHERE YOU ARE POINTING. Landing back on the map you left ignores the
             * region cursor entirely, which is the opposite of what a zoom means. The
             * region map only knows SECTIONS, but every map header carries its mapsec, so
             * the section under the cursor resolves to a concrete map. */
            uint8_t tg = v->group, tn = v->num;
            uint8_t ms = mr_region_cursor_mapsec(&s_rgn);
            mr_region_exit(&gfx, &mr);
            if (ms != 0xFF && ms != v->hdr.mapsec) {
              loading_frame(map_name(v->game, ms));
              uint8_t fg, fn;
              rmbl_pause();
              bool found = rom_find_map_by_mapsec(v->rc, ms, &fg, &fn);
              rmbl_resume();
              if (found) { tg = fg; tn = fn; }
              else snd_deny();       /* section with no map of its own: stay put */
            }
            rmbl_pause(); sprites_rearm(); rmbl_resume();
            /* Centre of the destination, since the region cursor names a place, not a tile. */
            if (!map_switch(tg, tn, (tg == v->group && tn == v->num) ? v->cx : -1,
                                    (tg == v->group && tn == v->num) ? v->cy : -1))
              v->fatal = true;
          } else {
            if (nz == 0) { gfx.ring_valid = false; mgfx_camera(&gfx, &mr, v->cx, v->cy); mgfx_z0_show(); }
            else if (mgfx_zoom_build(&gfx, &mr, v->cx, v->cy, nz)) mgfx_zoom_show(nz, v->cx, v->cy);
            else { v->zoom = was; snd_deny(); }
            v->hud_dirty = true;
          }
        }
        snd_tab();
        if (v->fatal) break;
      }
    }


    /* START: enter the warp under the cursor. */
    if (key_hit(KEY_START) && !in_region) {
      const SbEnt* se = sb_entrance_at(v->cx, v->cy);
      const RomWarp* w = warp_at(v->cx, v->cy);
      const RomObjectEvent* soe = moam_npc_at(v->cx, v->cy);
      uint8_t dg = 0, dn = 0;
      if (soe && is_item_ball(soe)) {
        if (!ball_taken(soe)) { snd_deny(); }
        else if (!restore_item_ball(soe)) { /* it says why itself */ }
        if (v->fatal) break;
      } else if (soe && !w && !se) {
        show_npc_dialog(soe);
        if (v->fatal) break;
      } else if (se) {
        /* A secret-base door. The interior is an ordinary map in group 25, so entering is
         * just a map_switch — but the way BACK is not: the interior's only warp is
         * MAP_DYNAMIC, which resolves to nothing useful. Remember the entrance instead. */
        int room = sbmap_interior(se->id);
        if (!se->occupied) {
          snd_deny();
          map_dialog("NO BASE HERE", "Nobody has made a base", "at this spot.", "A ok",
                     KEY_A | KEY_B);
          dialog_done();
        } else if (room < 0) {
          snd_deny();
        } else {
          snd_ok();
          uint8_t rg = v->group, rn = v->num; int rx = v->cx, ry = v->cy;
          uint8_t bid = se->id;
          v->base_id = bid;                 /* read by apply_base_decor during the switch */
          /* Land where the GAME lands you. SetSecretBaseWarpDestination uses warp id 0 for
           * all 24 groups, and every interior has exactly one warp. Centring instead put
           * the view up to seven rows away from the door — 25.6 is 7x16 — which is most of
           * why the rooms "did not look placed right". The coordinate cannot be derived
           * from the dimensions: it is usually (w/2, h-2) but six of the 24 break that. */
          int ex = -1, ey = -1;
          RomMapHeader ih; RomMapEvents iev; RomWarp iw;
          rmbl_pause();
          if (rom_map_header(v->rc, SB_INTERIOR_GROUP, (uint8_t)room, &ih) &&
              rom_map_events(v->rc, ih.events, &iev) && iev.warp_count > 0 &&
              rom_warp(v->rc, &iev, 0, &iw)) { ex = iw.x; ey = iw.y; }
          rmbl_resume();
          if (map_switch(SB_INTERIOR_GROUP, (uint8_t)room, ex, ey)) {
            v->ret_group = rg; v->ret_num = rn; v->ret_x = rx; v->ret_y = ry;
          } else {
            v->base_id = 0;
          }
          if (v->fatal) break;
        }
      } else if (!w && dive_conn(ROM_CONN_DIVE, &dg, &dn)) {
        snd_ok();
        v->base_id = 0;
        map_switch(dg, dn, v->cx, v->cy);          /* dive keeps the coordinates */
        if (v->fatal) break;
      } else if (!w && dive_conn(ROM_CONN_EMERGE, &dg, &dn)) {
        snd_ok();
        v->base_id = 0;
        map_switch(dg, dn, v->cx, v->cy);
        if (v->fatal) break;
      } else if (!w) {
        snd_deny();
      } else {
        uint8_t dg, dn; int32_t dxw, dyw;
        rmbl_pause();
        int r = rom_warp_dest(v->rc, w, &dg, &dn, &dxw, &dyw);
        rmbl_resume();
        if (r == ROM_WARP_OK) {
          snd_ok();
          map_switch(dg, dn, (int)dxw, (int)dyw);
          if (v->fatal) break;
        } else {
          /* Say WHY. A silent refusal on a door the user can see is indistinguishable
           * from a bug. */
          snd_deny();
          const char* why = (r == ROM_WARP_DYNAMIC) ? "The game picks this exit"
                          : (r == ROM_WARP_DUMMY)   ? "This warp goes nowhere."
                          : (r == ROM_WARP_BAD_MAP) ? "Destination map missing."
                                                    : "Could not read it.";
          const char* l2  = (r == ROM_WARP_DYNAMIC) ? "at runtime - no fixed one."
                                                    : 0;
          uint8_t eg, en; int ex, ey;
          if (r == ROM_WARP_DYNAMIC && escape_warp_of(v->sb1, &eg, &en, &ex, &ey)) {
            /* Offer the save's own escape point rather than leaving the user stuck. */
            u16 kk = map_dialog("NO FIXED EXIT", "The game picks this one.",
                                "Use your escape point?", "A go   B stay",
                                KEY_A | KEY_B);
            if (kk & KEY_A) {
              /* dialog_done() unconditionally: map_switch leaves the display alone when it
               * REFUSES a destination, and map_dialog left only BG0 enabled — so without
               * this a bad escape point left the map invisible (QA pass 2, defect 4). */
              bool moved = map_switch(eg, en, ex, ey);
              if (v->fatal) break;
              if (!moved) dialog_done();
            } else {
              dialog_done();
            }
          } else {
            map_dialog("CANNOT ENTER", why, l2, "A ok", KEY_A | KEY_B);
            dialog_done();
          }
        }
      }
    }

    /* SELECT: jump between the character's tile and wherever we last scrolled to. */
    if (key_hit(KEY_SELECT)) {
      if (v->at_home) {
        if (v->have_away) {
          uint8_t g2 = v->away_group, n2 = v->away_num; int x2 = v->away_x, y2 = v->away_y;
          v->away_group = v->group; v->away_num = v->num; v->away_x = v->cx; v->away_y = v->cy;
          if (g2 == v->group && n2 == v->num) { v->cx = x2; v->cy = y2; camera_refresh(); }
          else map_switch(g2, n2, x2, y2);
          v->at_home = false; snd_tab(); v->hud_dirty = true;
        } else snd_deny();
      } else {
        v->away_group = v->group; v->away_num = v->num; v->away_x = v->cx; v->away_y = v->cy;
        v->have_away = true;
        if (v->home_group == v->group && v->home_num == v->num) {
          v->cx = v->home_x; v->cy = v->home_y; camera_refresh();
        } else map_switch(v->home_group, v->home_num, v->home_x, v->home_y);
        v->at_home = true; snd_tab(); v->hud_dirty = true;
      }
      if (v->fatal) break;
    }

    /* Any move away from the character's own tile means the camera is no longer "home". */
    if (v->at_home && (dx || dy) &&
        !(v->group == v->home_group && v->num == v->home_num &&
          v->cx == v->home_x && v->cy == v->home_y)) {
      v->at_home = false;
    }

    /* A: grab the character off its own tile, or drop it where the cursor is. */
    if (key_hit(KEY_A) && !in_region) {
      if (v->carrying) {
        if (do_drop()) {
          v->carrying = false;
          moam_carry(false, v->cx, v->cy);
          moam_player_at(v->cx, v->cy);
          player_visibility();
          moam_drop_puff(v->cx, v->cy);
          rmbl_fire(RCUE_EDIT);
        }
        v->hud_dirty = true;
      } else if (v->group == v->home_group && v->num == v->home_num &&
                 v->cx == v->home_x && v->cy == v->home_y) {
        v->carrying = true;
        moam_carry(true, v->cx, v->cy);
        player_visibility();
        snd_ok();
        rmbl_fire(RCUE_SCROLL);
        v->hud_dirty = true;
      } else {
        /* Deny with a reason rather than silently: from across a route it is not obvious
         * that A is context-sensitive. SELECT jumps back to the character. */
        snd_deny();
        v->hud_dirty = true;
      }
    }

    if (v->hud_dirty) {
      uint16_t cell = cell_here(v->cx, v->cy);
      char l[48];
      const char* nm = map_name(v->game, v->hdr.mapsec);
      /* "1/4~" carries the tilde on purpose: Z2 cannot be exact (the affine layer caps at
       * 256 distinct tiles and the window needs up to 428), so the label says so. */
      static const char* const ZL[5] = { "1:1", "1/2", "1/4~", "RGN", "RGN+" };
      char zbuf[16];
      const char* zl = ZL[(v->zoom >= 0 && v->zoom <= 4) ? v->zoom : 0];
      /* A permanent marker, so a photo of the screen carries the evidence. */
      /* At 1/4 the label carries how much of the frame is approximated, so a screenshot is
       * self-describing: "1/4~ 0%" is as good as this level gets. */
      if (v->zoom == 2 && !v->repairs) {
        siprintf(zbuf, "%s %d%%", zl, mgfx_zoom_filler_pct(&gfx)); zl = zbuf;
      }
      if (v->repairs) { siprintf(zbuf, "%s !%d", zl, v->repairs); zl = zbuf; }
      /* The top line is 30 characters wide and tte wraps at the edge, so the name field has
       * to give ground when the zoom label grows: "1/4~ 12%" is four characters longer than
       * "1/4~". At 1/4 the map name matters least anyway — you can see the whole region. */
      if (v->zoom == 2) siprintf(l, "%-11.11s %3d,%-3d %s", nm ? nm : "", v->cx, v->cy, zl);
      else              siprintf(l, "%-15s %3d,%-3d %s", nm ? nm : "", v->cx, v->cy, zl);
      tte_erase_screen();
      tte_set_pos(4, 4); tte_write(l);
      /* The cursor's own row: an NPC if one is standing here, otherwise the raw cell.
       * Showing WHICH npc (graphics id, movement type, trainer, and the flag that controls
       * it) is what makes the overlay a tool rather than decoration — the flag in
       * particular explains why an NPC you expect is missing. */
      /* Recomputed, NOT the cached `in_region`: that is sampled at the top of the loop, so
       * on the very iteration a zoom change happens it still holds the OLD level and the
       * HUD would paint tile data over a region map until something else dirtied it. */
      if (v->zoom > MGFX_ZOOM_TILE_MAX) {
        /* On the region map the cursor and the tile data mean nothing; what is useful is
         * which section you are looking at and how to get back. */
        /* MAPSEC_NONE is a real id meaning "no section here" (open sea, mostly). Printing
         * the table's raw "NONE" leaks an internal constant at the user; say what it means. */
        uint8_t ms = mr_region_cursor_mapsec(&s_rgn);
        const char* sn = (ms != 0xFF) ? map_name(v->game, ms) : 0;
        bool none = (!sn || !sn[0] || !strcmp(sn, "NONE") || !strcmp(sn, "FARAWAY PLACE"));
        siprintf(l, "%s", none ? "- off map -" : sn);
        tte_set_pos(4, 132); tte_write(l);
        tte_set_pos(4, 141);
        tte_write(mr_region_view_count(&s_rgn) > 1 ? mr_region_view_label(&s_rgn)
                 : mr_region_no_zoom() ? "region map"
                 : (v->zoom >= 4 ? "whole region" : "zoomed"));
        tte_set_pos(4, 150);
        tte_write(mr_region_view_count(&s_rgn) > 1 ? "L switch map   R in   B back"
                                                   : "D-pad move  R in  B back");
        v->hud_dirty = false;
        /* Fall through rather than `continue`: the B check lives below, and skipping it
         * swallowed a B pressed on the exact frame a zoom change dirtied the HUD. */
        goto hud_done;
      }
      const SbEnt* se = sb_entrance_at(v->cx, v->cy);
      if (se) {
        siprintf(l, "SECRET BASE  %s", se->occupied ? se->owner : "(vacant)");
        tte_set_pos(4, 132); tte_write(l);
        tte_set_pos(4, 141); tte_write(se->occupied ? "START enter" : "no base here");
        tte_set_pos(4, 150); tte_write(v->carrying ? "A drop  B cancel carry"
                                                   : "A grab  L/R zoom  SEL  B");
        v->hud_dirty = false;
        goto hud_done;
      }
      const RomObjectEvent* oe = moam_npc_at(v->cx, v->cy);
      if (oe && is_item_ball(oe)) {
        /* Which item, straight from the ball's own script. rom_script_item_ball returns 0
         * for the handful of balls whose script is not the plain finditem shape (hidden
         * items behind a `special`, mostly) — say so rather than printing "item 0". */
        rmbl_pause();                       /* per-frame HUD block: this is the exact frame a
                                             * cursor step can arm the 8192 Hz PWM ISR */
        uint16_t it = rom_script_item_ball(v->rc, oe->script);
        rmbl_resume();
        const char* nm = it ? pk_item_name(it) : 0;
        bool taken = ball_taken(oe);
        siprintf(l, "%-12s %s", nm ? nm : "ITEM?", taken ? "TAKEN" : "here");
        tte_set_pos(4, 132); tte_write(l);
        tte_set_pos(4, 141);
        tte_write(!taken            ? "already on the ground"
                : !oe->flag_id      ? "no flag - cannot restore"
                :                     "START put it back");
      } else if (oe) {
        siprintf(l, "NPC g%-3u mv%-2u %s", oe->graphics_id, oe->movement_type,
                 oe->trainer_type ? "TRAINER" : "");
        tte_set_pos(4, 132); tte_write(l);
        siprintf(l, "flag %u  elev %u  %s", oe->flag_id, oe->elevation,
                 warp_at(v->cx, v->cy) ? "START enter" : "START talk");
        tte_set_pos(4, 141); tte_write(l);
      } else {
        uint8_t hg, hn;
        const char* act = warp_at(v->cx, v->cy)              ? "START enter"
                        : dive_conn(ROM_CONN_DIVE, &hg, &hn) ? "START dive"
                        : dive_conn(ROM_CONN_EMERGE, &hg, &hn) ? "START surface"
                        : "";
        siprintf(l, "t%-4u c%u e%-2u %s", ROM_CELL_METATILE(cell),
                 ROM_CELL_COLLISION(cell), ROM_CELL_ELEVATION(cell), act);
        tte_set_pos(4, 141); tte_write(l);
      }
      tte_set_pos(4, 150); tte_write(v->carrying ? "A drop  B cancel carry"
                                                 : "A grab  L/R zoom  SEL jump  B");
      v->hud_dirty = false;
    }
hud_done:

    if (key_hit(KEY_B)) {
      if (!v->carrying && v->in_base) {
        /* Back out of a secret base to the door we came in through. There is deliberately
         * no general map stack, and the interior has no usable exit warp, so this is a
         * single remembered slot — the same shape as the SELECT home/away pair. */
        snd_back();
        map_switch(v->ret_group, v->ret_num, v->ret_x, v->ret_y);
        if (v->fatal) break;
        continue;
      }
      if (v->carrying) {                 /* cancel the carry; the character stays put */
        v->carrying = false;
        moam_carry(false, v->home_x, v->home_y);
        player_visibility();
        snd_back();
        v->hud_dirty = true;
      } else break;
    }

    if (v->zoom > MGFX_ZOOM_TILE_MAX) {
      mr_region_tick(&s_rgn, v->zoom);
    } else {
      sprites_follow();
      moam_tick();
    }
  }

  moam_shutdown();
  mgfx_exit();
  snd_back();
}

/* ---- the screen ------------------------------------------------------------ */

/* Which game a detected ROM belongs to, so a Ruby ROM can't be used to draw an
 * Emerald save's maps (their map data genuinely differs — Guy's requirement). */
static PkGame rom_game(RomKind k) {
  switch (k) {
    case ROM_EMERALD:                    return PK_EMERALD;
    case ROM_RUBY:  case ROM_SAPPHIRE:   return PK_RS;
    case ROM_FIRERED: case ROM_LEAFGREEN:return PK_FRLG;
    default:                             return PK_EMERALD;
  }
}

void pdna_map(uint8_t* sb1, uint8_t* sb2, PkGame game) {
  (void)sb2;
  if (!sb1) return;

  /* The arena is borrowed from g_pc, which holds unsaved box moves when the PC is
   * dirty. Refuse rather than destroy them — and say why. */
  uint8_t* arena = app_arena_acquire(APP_ARENA_BYTES);
  if (!arena) {
    s_msg("MAP", UI_WARN, "Save your PC box moves", "first, then reopen.");
    return;
  }

  static char EWRAM_BSS cwd[PATH_MAX];
  static char EWRAM_BSS path[PATH_MAX];
  RomCtx rc;
  bool have = false;

  /* The picker's entry list lives INSIDE the borrowed arena — no new EWRAM. */
  PickEnt* ents = (PickEnt*)arena;
  const uint32_t ents_bytes = (uint32_t)sizeof(PickEnt) * PICK_MAX;

  /* ---- the emulator path: a ROM fused into our own cartridge -----------------
   * When tools/fuse_rom.py has appended the user's Pokemon ROM to this image there is no
   * file to pick and no SD to read: rom_open() is pointed at a memcpy out of cartridge
   * space instead. Everything downstream is identical because both readers are RomReadFn.
   * Unfused images fall straight through to the FatFs path below. */
  bool fused = false;
  {
    uint32_t fsz = 0;
    if (fused_rom_present(&fsz)) {
      if (rom_open(&rc, fused_rom_read, 0, fsz)) {
        if (rom_game(rc.kind) == game) {
          fused = true; have = true;
          log_line("map: fused ROM %s rev%u, %lu MB", rom_kind_name(rc.kind), rc.version,
                   (unsigned long)(fsz >> 20));
        } else {
          /* A fused image carries exactly one game. Say so plainly rather than letting
           * the user hunt for a file picker that cannot help them here. */
          char l1[40];
          siprintf(l1, "Fused ROM is %s.", rom_kind_name(rc.kind));
          s_msg("WRONG GAME", UI_WARN, l1, "Re-fuse with this game.");
          app_arena_release();
          return;
        }
      }
    }
  }

  for (;;) {
    if (!have) {
      const char* remembered = app_rom_path(game);
      if (remembered && remembered[0]) {
        strncpy(path, remembered, PATH_MAX - 1); path[PATH_MAX - 1] = 0;
      } else {
        /* First time for this game: ask. Start where the user last browsed saves. */
        strcpy(cwd, "/");
        ui_clear();
        s_msg("MAP NEEDS YOUR ROM", UI_TITLE, "Pick the .gba you play.", "Nothing is copied.");
        if (!pick_rom(cwd, PATH_MAX, path, PATH_MAX, ents)) { app_arena_release(); return; }
      }

      /* ---- open + identify. No drawing happens between here and rmbl_resume. ---- */
      rmbl_pause();
      memset(&s_rf, 0, sizeof s_rf);
      FRESULT fr = f_open(&s_rf.f, path, FA_READ);
      uint32_t sz = 0;
      bool opened = (fr == FR_OK);
      if (opened) { s_rf.open = true; sz = (uint32_t)f_size(&s_rf.f); }
      bool ok = opened && rom_open(&rc, rom_fatfs_read, &s_rf, sz);
      rmbl_resume();

      if (!ok) {
        if (s_rf.open) { rmbl_pause(); f_close(&s_rf.f); rmbl_resume(); s_rf.open = false; }
        char l1[40];
        if (!opened) siprintf(l1, "Can't open that file.");
        else         siprintf(l1, "%u MB - not a known ROM", (unsigned)(sz >> 20));
        s_msg("UNSUPPORTED ROM", UI_WARN, l1, "Pick another (A).");
        app_rom_path_set(game, "");           /* forget it so we ask again */
        strcpy(cwd, "/");
        if (!pick_rom(cwd, PATH_MAX, path, PATH_MAX, ents)) { app_arena_release(); return; }
        continue;
      }

      /* A Ruby ROM cannot describe an Emerald save's maps. Refuse the mismatch
       * rather than render the wrong world. */
      if (rom_game(rc.kind) != game) {
        char l1[40];
        siprintf(l1, "That ROM is %s.", rom_kind_name(rc.kind));
        s_msg("WRONG GAME", UI_WARN, l1, "Need this save's game.");
        rmbl_pause(); f_close(&s_rf.f); rmbl_resume(); s_rf.open = false;
        strcpy(cwd, "/");
        if (!pick_rom(cwd, PATH_MAX, path, PATH_MAX, ents)) { app_arena_release(); return; }
        continue;
      }

      app_rom_path_set(game, path);           /* remember per game */
      have = true;
      rmbl_fire(RCUE_ROOM);
    }

    /* ---- read everything we want to show, THEN draw (never interleaved) ---- */
    G3Warp cur;
    g3warp_get_current(sb1, &cur);

    RomMapHeader h; RomLayout lay; RomMapEvents ev;
    bool hdr_ok, lay_ok = false, ev_ok = false;
    int npc_shown = 0, npc_total = 0, warps = 0, conns = 0;

    rmbl_pause();
    hdr_ok = rom_map_header(&rc, cur.group, cur.num, &h);
    if (hdr_ok) {
      lay_ok = rom_layout(&rc, h.layout, &lay);
      ev_ok  = rom_map_events(&rc, h.events, &ev);
      if (ev_ok) {
        npc_total = ev.object_count;
        warps     = ev.warp_count;
        for (int i = 0; i < ev.object_count; i++) {
          RomObjectEvent oe;
          if (!rom_object_event(&rc, &ev, i, &oe)) continue;
          /* An NPC spawns iff its flag is CLEAR; flagId 0 means always present.
           * This is the cross-reference that makes the map match THIS save. */
          if (oe.flag_id == 0 || !pk_flag_get(sb1, game, oe.flag_id)) npc_shown++;
        }
      }
      conns = rom_connection_count(&rc, h.connections);
    }
    rmbl_resume();

    ui_clear();
    ui_text(4, 4, UI_TITLE, "MAP (from your ROM)");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);

    char l[48];
    siprintf(l, "ROM: %s rev%u", rom_kind_name(rc.kind), rc.version);
    ui_text(4, 20, UI_OK, l);
    siprintf(l, "%u groups, %u maps", rc.group_count, rc.total_maps);
    ui_text(4, 30, UI_DIM, l);

    ui_hline(0, 42, UI_SCR_W, UI_BORDER);
    ui_text(4, 46, UI_TITLE, "YOU ARE HERE");
    siprintf(l, "map %u.%u  at (%d,%d)", cur.group, cur.num, cur.x, cur.y);
    ui_text(4, 58, UI_TEXT, l);

    if (hdr_ok && lay_ok) {
      siprintf(l, "size %ldx%ld blocks", (long)lay.width, (long)lay.height);
      ui_text(4, 70, UI_TEXT, l);
      siprintf(l, "mapsec %u  type %u", h.mapsec, h.map_type);
      ui_text(4, 80, UI_DIM, l);
      siprintf(l, "NPCs here: %d of %d", npc_shown, npc_total);
      ui_text(4, 92, UI_TEXT, l);
      siprintf(l, "warps %d  exits %d", warps, conns);
      ui_text(4, 102, UI_DIM, l);
      /* Reading the player's own metatile is the end-to-end proof: save -> ROM
       * header -> layout -> blockdata -> tileset resolution, all off the SD. */
      uint16_t cell = 0;
      bool cell_ok = false;
      if (cur.x >= 0 && cur.y >= 0 && cur.x < lay.width && cur.y < lay.height) {
        rmbl_pause();
        cell_ok = rom_blocks(&rc, &lay, (uint32_t)cur.y * (uint32_t)lay.width + (uint32_t)cur.x, &cell, 1);
        rmbl_resume();
      }
      if (cell_ok) {
        siprintf(l, "tile %u  coll %u  elev %u", ROM_CELL_METATILE(cell),
                 ROM_CELL_COLLISION(cell), ROM_CELL_ELEVATION(cell));
        ui_text(4, 116, UI_OK, l);
      } else {
        ui_text(4, 116, UI_WARN, "Could not read your tile");
      }
    } else {
      ui_text(4, 70, UI_WARN, "Could not read this map.");
      ui_text(4, 80, UI_DIM, "Wrong ROM for this save?");
    }

    ui_hline(0, 140, UI_SCR_W, UI_BORDER);
    ui_text(4, 144, UI_DIM, "A view map  SEL change ROM");
    ui_text(4, 152, UI_DIM, "B back");

    u16 k = s_wait(KEY_A | KEY_B | KEY_SELECT);
    if ((k & KEY_A) && hdr_ok && lay_ok) {
      map_view(&rc, arena, game, cur.group, cur.num, &cur, sb1, sb2);
      app_log_flush();                             /* one write per visit, off the render path */
      continue;                                    /* map_view restored the Mode-3 UI */
    }
    if (k & KEY_SELECT) {
      /* A fused image has exactly one ROM baked in and no filesystem to pick from. */
      if (fused) { snd_deny(); continue; }
      rmbl_pause(); f_close(&s_rf.f); rmbl_resume(); s_rf.open = false;
      app_rom_path_set(game, "");
      have = false;
      strcpy(cwd, "/");
      continue;
    }
    if (k & KEY_B) break;
  }

  if (s_rf.open) { rmbl_pause(); f_close(&s_rf.f); rmbl_resume(); s_rf.open = false; }
  app_arena_release();
  (void)ents_bytes;
}


/* Public wrapper for Settings > Game ROM: same picker, arena-backed buffers. */
bool app_pick_rom(char* out, int out_cap) {
  uint8_t* mem = app_arena_acquire(PICK_MAX * (uint32_t)sizeof(PickEnt));
  if (!mem) return false;                    /* PC dirty or arena held: caller explains */
  char cwd[PATH_MAX];
  strcpy(cwd, "/");
  s_pick_ext[0] = ".gba"; s_pick_ext[1] = 0;
  bool ok = pick_rom(cwd, sizeof cwd, out, out_cap, (PickEnt*)mem);
  app_arena_release();
  return ok;
}

/* Same browser, Game Boy battery files: a Gen-1/2 save is 32 KiB (+ an optional RTC
 * tail) and carries no signature in its NAME, so the extension filter is deliberately
 * loose and pdna_gen12_mount does the real identification. */
bool app_pick_gb_save(char* out, int out_cap) {
  uint8_t* mem = app_arena_acquire(PICK_MAX * (uint32_t)sizeof(PickEnt));
  if (!mem) return false;
  char cwd[PATH_MAX];
  strcpy(cwd, "/");
  s_pick_ext[0] = ".sav"; s_pick_ext[1] = ".srm"; s_pick_ext[2] = 0;
  bool ok = pick_rom(cwd, sizeof cwd, out, out_cap, (PickEnt*)mem);
  s_pick_ext[0] = ".gba"; s_pick_ext[1] = 0; s_pick_ext[2] = 0;   /* restore the default */
  app_arena_release();
  return ok;
}

/* Same browser, Game Boy CARTRIDGE dumps — the source of Gen-1/2 sprite art, so that a
 * Pokemon imported from Red or Gold can be drawn in the art it actually came from. As
 * everywhere else in PokeDNA, no game art ships with the tool: it is read at runtime from
 * the user's own cartridge, and the caller identifies the ROM by its header before
 * trusting a byte of it. The extension only narrows the list; it proves nothing. */
bool app_pick_gb_rom(char* out, int out_cap) {
  uint8_t* mem = app_arena_acquire(PICK_MAX * (uint32_t)sizeof(PickEnt));
  if (!mem) return false;
  char cwd[PATH_MAX];
  strcpy(cwd, "/");
  s_pick_ext[0] = ".gb"; s_pick_ext[1] = ".gbc"; s_pick_ext[2] = 0;
  bool ok = pick_rom(cwd, sizeof cwd, out, out_cap, (PickEnt*)mem);
  s_pick_ext[0] = ".gba"; s_pick_ext[1] = 0; s_pick_ext[2] = 0;   /* restore the default */
  app_arena_release();
  return ok;
}
