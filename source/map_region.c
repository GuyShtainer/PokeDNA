/*
 * Region-map zoom levels. See map_region.h for the VRAM/palette contract and for why the
 * two levels share one upload.
 */
#include <tonc.h>
#include <string.h>

#include "map_region.h"
#include "log.h"
#include "map_oam.h"

/* RSE: 8bpp affine, 64x64 one-byte entries, NO wrap (the region is a finite picture —
 * wrapping would tile Hoenn across the screen). */
#define RGN_BG2CNT_AFF  (BG_CBB(2) | BG_SBB(26) | BG_AFF_64x64 | BG_PRIO(2))
/* FRLG: a plain 4bpp TEXT background, 30x20 tiles = 240x160 = exactly one screen. */
#define RGN_BG2CNT_TEXT (BG_CBB(2) | BG_SBB(26) | BG_4BPP | BG_REG_32x32 | BG_PRIO(2))

/* Which layout we uploaded, so mr_region_show configures BG2 the same way. */
static bool s_text_bg;
/* Which of the region map's views is loaded. RSE has exactly one; FRLG has four (Kanto +
 * three Sevii groups) and the retail game switches between them with a "SWITCH MAP" button
 * rather than by zooming. The four views SHARE one tileset, so switching re-uploads only
 * the 1,200-byte tilemap — no CBB2 traffic, well under a millisecond. */
static int  s_view;

static bool stage_and_expand(const RgnMap* rg, int what, int view,
                             uint32_t vram_dst, uint8_t* arena, uint32_t arena_len) {
  uint32_t addr = 0, max_bytes = 0;
  if (!rgn_stream(rg, what, view, &addr, &max_bytes)) return false;
  if (!max_bytes || max_bytes > arena_len) return false;

  /* One sequential read of the compressed blob into the arena. A short read at end-of-ROM
   * is fine: the BIOS decoder stops at the size in the LZ77 header. */
  const RomCtx* rom = rg->rom;
  uint32_t off = addr - ROM_BASE;
  uint32_t n = max_bytes;
  if (off >= rom->size) return false;
  if (off + n > rom->size) n = rom->size - off;
  if (!rom->read(rom->ctx, off, arena, n)) return false;

  /* SWI 0x12, not mr_lz77: the destination is VRAM and mr_lz77 writes BYTES, which the
   * hardware would splat into both halves of each halfword. The BIOS VRAM variant writes
   * 16 bits at a time and is the only correct decoder for this destination. */
  LZ77UnCompVram(arena, (void*)vram_dst);
  return true;
}

bool mr_region_enter(RgnMap* rg, const RomCtx* rom, uint8_t* arena, uint32_t arena_len) {
  if (!rg || !rom || !arena) return false;
  if (!rgn_open(rg, rom)) { log_line("rgn: open FAILED"); return false; }

  /* Two genuinely different layouts. RSE is an 8bpp AFFINE background with one-byte
   * tilemap entries — which is what lets its two zoom levels be four register writes.
   * FRLG is a plain 4bpp TEXT background with 30x20 TWO-byte entries. Pushing FRLG through
   * the affine path produced convincing-looking noise while reporting success (QA pass 2,
   * defect 3), so the two are kept strictly apart and anything that is neither is refused. */
  if (rg->bpp == 8 && rg->tilemap_entry_bytes == 1) {
    s_text_bg = false;
  } else if (rg->bpp == 4 && rg->tilemap_entry_bytes == 2) {
    s_text_bg = true;
  } else {
    log_line("rgn: unsupported layout (bpp %u, entry %u) - refusing",
             rg->bpp, rg->tilemap_entry_bytes);
    return false;
  }

  s_view = RGN_VIEW_MAIN;
  if (!stage_and_expand(rg, RGN_STREAM_TILES, RGN_VIEW_MAIN,
                        MGFX_VRAM_MIP_CHAR, arena, arena_len)) {
    log_line("rgn: tiles FAILED");
    return false;
  }

  if (!s_text_bg) {
    /* Affine: the 64x64 one-byte map is exactly the 4,096 B slot, so straight to VRAM. */
    if (!stage_and_expand(rg, RGN_STREAM_TILEMAP, s_view,
                          MGFX_VRAM_AFF_MAP, arena, arena_len)) {
      log_line("rgn: tilemap FAILED");
      return false;
    }
  } else {
    /* Text: 30x20 u16 must be re-pitched into a 32-wide screenblock, so it cannot be
     * expanded straight into VRAM. Stage compressed, expand in EWRAM (SWI 0x11 — the WRAM
     * variant, because the destination is not VRAM), then copy row by row as HALFWORDS. */
    uint32_t addr = 0, max_bytes = 0;
    if (!rgn_stream(rg, RGN_STREAM_TILEMAP, s_view, &addr, &max_bytes)) return false;
    uint32_t need = ((uint32_t)rg->tilemap_bytes + 3u) & ~3u;
    if (max_bytes + need + 8u > arena_len) return false;
    const RomCtx* rom2 = rg->rom;
    uint32_t off = addr - ROM_BASE;
    uint32_t n = max_bytes;
    if (off >= rom2->size) return false;
    if (off + n > rom2->size) n = rom2->size - off;
    if (!rom2->read(rom2->ctx, off, arena, n)) return false;

    uint8_t* out = arena + ((max_bytes + 3u) & ~3u);
    LZ77UnCompWram(arena, out);

    const uint16_t* src = (const uint16_t*)(const void*)out;
    uint16_t* dst = (uint16_t*)MGFX_VRAM_AFF_MAP;      /* SBB26, reused as a text map */
    int tw = rg->map_tw, th = rg->map_th;
    if (tw <= 0 || tw > 32 || th <= 0 || th > 32) return false;
    memset32(dst, 0, 1024);                             /* 2,048 B of screen entries  */
    for (int y = 0; y < th; y++)
      for (int x = 0; x < tw; x++)
        dst[y * 32 + x] = src[y * tw + x];              /* halfword stores throughout */
  }

  /* Palette. RSE loads 48 colours at index 112 but its 8bpp tiles index the whole 256, so
   * the full image goes in; FRLG is 4bpp with 80 colours at index 0. Either way bank 15
   * belongs to the HUD's text and must survive. */
  {
    static uint16_t EWRAM_BSS pal[RGN_PAL_ENTRIES];
    if (!rgn_palette(rg, pal)) { log_line("rgn: palette FAILED"); return false; }
    if (s_text_bg) {
      int n = rg->pal_colours;
      if (n > 240) n = 240;                             /* never reach bank 15 */
      for (int i = 0; i < n; i++) pal_bg_mem[rg->pal_base + i] = pal[rg->pal_base + i];
    } else {
      memcpy32(pal_bg_mem, pal, RGN_PAL_ENTRIES / 2);   /* 256 u16 = 128 words */
      pal_bg_mem[0xF0] = 0;                             /* tte's paper: transparent  */
      pal_bg_mem[0xF1] = CLR_WHITE;                     /* tte's ink                 */
    }
  }
  log_line("rgn: ok  %s", rgn_view_name(rg, RGN_VIEW_MAIN));
  return true;
}

bool mr_region_cell(const RgnMap* rg, uint8_t mapsec, int* gx, int* gy, int* out_view) {
  if (!rg || !gx || !gy) return false;
  int x, y, w, h;
  /* rgn_find_mapsec searches the grid planes and is the only thing that works for FRLG's
   * dungeon sections, whose rect table holds a placeholder (0,0,1,1). Fall back to the
   * rect table, which is complete on RSE. */
  int view, layer;
  if (rgn_find_mapsec(rg, mapsec, &view, &layer, gx, gy)) {
    if (out_view) *out_view = view;
    return true;
  }
  if (rgn_mapsec_rect(rg, mapsec, &x, &y, &w, &h) && (w > 0) && (h > 0)) {
    /* (0,0,1,1) is the table's PLACEHOLDER for a section that is not drawn anywhere —
     * secret bases, link rooms, the FRLG dungeon sections. The grid search above is what
     * finds the ones that really are on the picture, so reaching here with the placeholder
     * means "nowhere", and returning it would park the marker in the top-left corner of
     * Hoenn. Emerald's own save (inside a secret base, mapsec 86) did exactly that. */
    if (x == 0 && y == 0 && w == 1 && h == 1) return false;
    *gx = x + w / 2; *gy = y + h / 2;
    if (out_view) *out_view = RGN_VIEW_MAIN;
    return true;
  }
  return false;
}

void mr_region_show(const RgnMap* rg, int level, uint8_t mapsec) {
  if (s_text_bg) {
    /* FRLG: a text BG cannot be scaled by the hardware, and the game itself has no zoomed
     * region map — so both levels show the same 240x160 picture. Honest rather than fake. */
    REG_BG2CNT  = RGN_BG2CNT_TEXT;
    REG_BG2HOFS = 0; REG_BG2VOFS = 0;
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG2 | MGFX_HUD_WIN;
    return;
  }
  REG_BG2CNT = RGN_BG2CNT_AFF;

  if (level >= 4) {
    /* Whole region, 1:1 with the picture. */
    REG_BG2PA = 0x100; REG_BG2PB = 0;
    REG_BG2PC = 0;     REG_BG2PD = 0x100;
    REG_BG2X  = 0;     REG_BG2Y  = 0;
  } else {
    /* The game's zoomed mode: 2x magnification centred on the player's section. PA/PD of
     * 0x80 means half a texel per screen pixel. */
    int gx = 0, gy = 0, sx = 0, sy = 0;
    if (!mr_region_cell(rg, mapsec, &gx, &gy, 0)) { gx = 0; gy = 0; }
    rgn_zoom_origin(rg, gx, gy, &sx, &sy);
    REG_BG2PA = 0x80; REG_BG2PB = 0;
    REG_BG2PC = 0;    REG_BG2PD = 0x80;
    REG_BG2X  = sx << 8;
    REG_BG2Y  = sy << 8;
  }
  REG_DISPCNT = DCNT_MODE1 | DCNT_BG0 | DCNT_BG2 | MGFX_HUD_WIN;
}

void mr_region_exit(MapGfx* g, const MapRender* mr) {
  /* CBB2 and the affine tilemap now hold region-map data, so every mip slot the tile view
   * believes it owns is stale. Invalidating the dictionary is mandatory: keeping it would
   * draw the region map's pixels as metatiles. */
  if (g) {
    if (g->slot_of) memset(g->slot_of, 0xFF, 1024);
    g->slots_used = 0;
    g->aff_valid = false;
    memset32((void*)MGFX_VRAM_AFF_MAP, 0, 1024);
  }
  if (mr) mgfx_repaint_palettes(mr);
}

/* ---- the cursor ------------------------------------------------------------
 * See map_region.h. Sprite tiles live high (>= 992) and OAM entry 0 is ours: map_oam is
 * shut down for the whole time the region view is up, and moam_init() regenerates its own
 * effect tiles on the way back, so nothing here has to be restored.
 *
 * The art is a square OUTLINE generated in code. Nothing is taken from the ROM. */
#define RGN_TID_CUR16   992        /* 16x16, 8 tiles  -> full view   */
#define RGN_TID_CUR32  1000        /* 32x32, 16 tiles -> zoomed view */
#define RGN_PB_CUR       15        /* OBJ palette bank we own here   */
#define RGN_OE_CUR        0
#define RGN_BLINK        20        /* the game's anim1: 20 frames per phase */
#define RGN_GLIDE         4        /* full view: 4 frames x 2 px == one 8 px cell */
#define RGN_SCROLL        8        /* zoomed: 8 frames x 1 px */

static int  s_cgx, s_cgy;          /* cursor cell */
static int  s_glide;               /* frames left in the current move */
static int  s_gdx, s_gdy;          /* direction of that move */
static int  s_blink;
static int  s_scx, s_scy;          /* zoomed-view scroll origin, in image px */
static int  s_pgx, s_pgy;          /* the PLAYER's cell — the game shows them here too */
static bool s_phave;
static int  s_pview;               /* WHICH view that cell is in (Kanto vs Sevii, ...) */
static bool s_face_ok;             /* the cut-out face at RGN_TID_FACE is valid          */
#define RGN_OE_PLR  1
/* Rows 8..23 of the 16x32 overworld frame are cap, face and neck — the right crop. The only
 * thing wrong with it was two pixels: the very corners of the bottom row, where the tops of
 * the shoulders poke out either side of the neck. Guy asked for exactly those two gone, not
 * for the row. */
#define RGN_FACE_SHIFT 8
#define RGN_TID_PLR 1016      /* 8x8 player marker, 1 tile */
#define RGN_TID_FACE 1020     /* 16x16 player FACE, 4 tiles — built from the overworld frame */

/* One 4bpp tile row-pair helper: set pixel (x,y) of a w x h sprite in 1D-mapped tiles. */
static void cur_px(uint8_t* buf, int w, int x, int y, uint8_t ci) {
  int tx = x >> 3, ty = y >> 3, tiles_w = w >> 3;
  uint8_t* t = buf + ((ty * tiles_w + tx) * 32);
  uint8_t* b = t + (y & 7) * 4 + ((x & 7) >> 1);
  if (x & 1) *b = (uint8_t)((*b & 0x0F) | (ci << 4));
  else       *b = (uint8_t)((*b & 0xF0) | ci);
}

/* Scroll (in image px) that puts grid cell (gx,gy) at the SCREEN CENTRE in the 2x view.
 * The game parks its zoomed cursor at (56,72) — off centre, because its region map sits in
 * a framed window. Ours is full-screen, so centring is what reads correctly. Clamped so
 * the view never runs off the edge of the picture. */
static void centre_scroll(const RgnMap* rg, int gx, int gy, int* sx, int* sy) {
  int px, py;
  rgn_grid_to_px(rg, gx, gy, &px, &py);
  int x = px - 60, y = py - 40;              /* 240/2/2 and 160/2/2 at 2x magnification */
  int maxx = (int)rg->map_tw * 8 - 120;
  int maxy = (int)rg->map_th * 8 - 80;
  if (maxx < 0) maxx = 0;
  if (maxy < 0) maxy = 0;
  if (x < 0) x = 0; if (x > maxx) x = maxx;
  if (y < 0) y = 0; if (y > maxy) y = maxy;
  *sx = x; *sy = y;
}

/* A hollow square with corner ticks — legible over any terrain at either scale. */
static void gen_cursor(int side, uint32_t vram_tid) {
  static uint8_t EWRAM_BSS buf[16 * 32];        /* 32x32 4bpp = 512 B, the larger case */
  int bytes = (side >> 3) * (side >> 3) * 32;
  memset(buf, 0, (uint32_t)bytes);
  for (int i = 0; i < side; i++) {
    cur_px(buf, side, i, 0, 1);          cur_px(buf, side, i, side - 1, 1);
    cur_px(buf, side, 0, i, 1);          cur_px(buf, side, side - 1, i, 1);
  }
  for (int i = 1; i < side - 1; i++) {   /* inner dark edge, so it reads on light terrain */
    cur_px(buf, side, i, 1, 2);          cur_px(buf, side, i, side - 2, 2);
    cur_px(buf, side, 1, i, 2);          cur_px(buf, side, side - 2, i, 2);
  }
  memcpy32((void*)(0x06010000u + vram_tid * 32u), buf, (uint32_t)bytes / 4);
}

/* An 8x8 "you are here" pin: a solid diamond with a light rim. Deliberately NOT the
 * overworld sprite — that is 16x16 of head-and-shoulders sitting on an 8 px cell, so it
 * covers four cells and disappears into the artwork. A small high-contrast shape at
 * exactly one cell reads instantly, and because it never blinks it can't be confused with
 * the cursor. */
static void gen_player_pin(void) {
  static const char* const ART[8] = {
    "...11...",
    "..1221..",
    ".122221.",
    "12222221",
    "12222221",
    ".122221.",
    "..1221..",
    "...11...",
  };
  uint8_t t[32];
  memset(t, 0, sizeof t);
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      char c = ART[y][x];
      if (c == '.') continue;
      cur_px(t, 8, x, y, (uint8_t)(c == '1' ? 3 : 4));
    }
  memcpy32((void*)(0x06010000u + (uint32_t)RGN_TID_PLR * 32u), t, sizeof t / 4);
}

/* The player's face for the region map, cut out of the overworld sprite that map_oam already
 * loaded into OBJ VRAM.
 *
 * Why this is a COPY and not just "point OAM at tid+2": the crop itself IS tid+2 (rows 8..23),
 * but pixels have to be erased from it, and OAM cannot mask pixels.
 *
 * Source and destination are both OBJ VRAM. Reads are halfword-safe; writes are whole words,
 * because an 8-bit store into VRAM lands in both halves of the halfword. */
static bool gen_player_face(void) {
  uint16_t ptid; uint8_t ppb, ph;
  if (!moam_player_sprite(&ptid, &ppb, &ph)) return false;
  if (ph < 32) return false;                     /* a 16x16 character has no shoulders to cut */

  const volatile uint8_t* src =
      (const volatile uint8_t*)(0x06010000u + (uint32_t)ptid * 32u);

  /* Unpack the 16x16 crop one nibble per byte first. The trim below needs to see the whole
   * bottom ROW at once, and a 4bpp byte holds two pixels, so trimming in packed form would
   * mean reasoning about nibble halves at every step. 256 B of stack, the same order as
   * map_oam's upload staging buffer. */
  uint8_t px[256];
  for (int y = 0; y < 16; y++) {
    int sy = y + RGN_FACE_SHIFT;
    for (int x = 0; x < 16; x++) {
      uint32_t sb = (uint32_t)(((sy >> 3) * 2 + (x >> 3)) * 32 + (sy & 7) * 4 + ((x & 7) >> 1));
      uint8_t b = src[sb];
      px[y * 16 + x] = (x & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 15);
    }
  }

  /* The shoulders. Rows 8..23 of the overworld frame are the right crop, but its last row
   * still carries the tops of the shoulders either side of the neck — for Brendan, a skin
   * pixel plus its outline at each end. Guy asked for two off each side.
   *
   * Found by scanning inward from each end rather than hard-coded: the four playable
   * characters do not put their shoulders in the same columns, and a fixed x would clip the
   * neck on one of them and miss the shoulder on another. */
  {
    uint8_t* row = px + 15 * 16;
    int cut = 0;
    for (int x = 0; x < 16 && cut < 2; x++)  if (row[x]) { row[x] = 0; cut++; }
    cut = 0;
    for (int x = 15; x >= 0 && cut < 2; x--) if (row[x]) { row[x] = 0; cut++; }
  }

  /* Repack into the four OBJ tiles. Word stores only: an 8-bit store into VRAM lands in
   * both halves of the halfword. */
  volatile uint32_t* dst = (volatile uint32_t*)(0x06010000u + (uint32_t)RGN_TID_FACE * 32u);
  for (int t = 0; t < 4; t++) {
    int ox = (t & 1) * 8, oy = (t >> 1) * 8;
    for (int row = 0; row < 8; row++) {
      uint32_t word = 0;
      for (int b = 0; b < 4; b++) {
        int x = ox + b * 2, y = oy + row;
        word |= (uint32_t)(px[y * 16 + x] | (px[y * 16 + x + 1] << 4)) << (b * 8);
      }
      dst[t * 8 + row] = word;
    }
  }
  return true;
}

static void cursor_gfx_init(void) {
  gen_cursor(16, RGN_TID_CUR16);
  gen_cursor(32, RGN_TID_CUR32);
  gen_player_pin();
  s_face_ok = gen_player_face();
  COLOR* p = &pal_obj_mem[RGN_PB_CUR * 16];
  p[0] = 0;
  p[1] = RGB15(31, 31, 31);          /* bright outer edge */
  p[2] = RGB15(4, 6, 12);            /* dark inner edge   */
  /* MAGENTA on purpose. The Hoenn/Kanto region artwork is built from greens, oranges,
   * blues, white and RED — a red pin is invisible among the map's own route markers,
   * which is exactly what happened on the first attempt. Magenta appears nowhere in the
   * source art, so the pin can never be mistaken for part of the map. */
  p[3] = RGB15(31, 31, 31);          /* player pin rim  */
  p[4] = RGB15(31, 0, 24);           /* player pin body */
}

void mr_region_cursor_reset(const RgnMap* rg, uint8_t mapsec) {
  /* Not every mapsec HAS a region-map cell: secret bases, link rooms and a few specials
   * are nowhere on the picture. Parking the pin at (0,0) then claims the player is in the
   * top-left corner of Hoenn, which is a confident lie — Emerald's save (a secret base,
   * mapsec 86) showed exactly that. Hide it instead. */
  int gx = 0, gy = 0;
  s_pview = RGN_VIEW_MAIN;
  s_phave = mr_region_cell(rg, mapsec, &gx, &gy, &s_pview);
  if (!s_phave) { gx = 0; gy = 0; }
  s_cgx = gx; s_cgy = gy;
  s_pgx = gx; s_pgy = gy;                 /* reset is called with the PLAYER's section */
  s_glide = 0; s_gdx = s_gdy = 0; s_blink = 0;
  centre_scroll(rg, s_cgx, s_cgy, &s_scx, &s_scy);
  cursor_gfx_init();
}

bool mr_region_cursor_move(const RgnMap* rg, int dx, int dy) {
  if (!rg || s_glide) return false;               /* one cell at a time, like the game */
  int nx = s_cgx + dx, ny = s_cgy + dy;
  if (nx < 0 || ny < 0 || nx >= rg->grid_w || ny >= rg->grid_h) return false;
  s_cgx = nx; s_cgy = ny;
  s_gdx = dx; s_gdy = dy;
  s_glide = RGN_SCROLL;                            /* 8 frames; the full view uses 4 of them */
  return true;
}

uint8_t mr_region_cursor_mapsec(const RgnMap* rg) {
  uint8_t ms = 0;
  if (rg && rgn_section_at(rg, s_view, s_cgx, s_cgy, &ms)) return ms;
  return 0xFF;
}

bool mr_region_no_zoom(void) { return s_text_bg; }
int  mr_region_view_count(const RgnMap* rg) { return rg ? rg->view_count : 1; }
const char* mr_region_view_label(const RgnMap* rg) {
  return rg ? rgn_view_name(rg, s_view) : "";
}

/* Switch to the next view. Only the TILEMAP changes — all four views share one tileset, so
 * CBB2 is untouched and this is far cheaper than the enter path. Does SD reads, so the
 * caller must pause the motor and not be rendering. */
bool mr_region_next_view(const RgnMap* rg, uint8_t* arena, uint32_t arena_len) {
  if (!rg || rg->view_count <= 1) return false;
  int prev = s_view;
  s_view = (s_view + 1) % rg->view_count;
  bool ok;
  if (!s_text_bg) {
    ok = stage_and_expand(rg, RGN_STREAM_TILEMAP, s_view, MGFX_VRAM_AFF_MAP, arena, arena_len);
  } else {
    uint32_t addr = 0, max_bytes = 0;
    ok = rgn_stream(rg, RGN_STREAM_TILEMAP, s_view, &addr, &max_bytes);
    if (ok) {
      uint32_t need = ((uint32_t)rg->tilemap_bytes + 3u) & ~3u;
      const RomCtx* rom2 = rg->rom;
      uint32_t off = addr - ROM_BASE, n = max_bytes;
      if (max_bytes + need + 8u > arena_len || off >= rom2->size) ok = false;
      else {
        if (off + n > rom2->size) n = rom2->size - off;
        ok = rom2->read(rom2->ctx, off, arena, n);
        if (ok) {
          uint8_t* outb = arena + ((max_bytes + 3u) & ~3u);
          LZ77UnCompWram(arena, outb);
          const uint16_t* src = (const uint16_t*)(const void*)outb;
          uint16_t* dst = (uint16_t*)MGFX_VRAM_AFF_MAP;
          int tw = rg->map_tw, th = rg->map_th;
          if (tw <= 0 || tw > 32 || th <= 0 || th > 32) ok = false;
          else {
            memset32(dst, 0, 1024);
            for (int y = 0; y < th; y++)
              for (int x = 0; x < tw; x++) dst[y * 32 + x] = src[y * tw + x];
          }
        }
      }
    }
  }
  if (!ok) { s_view = prev; log_line("rgn: view switch FAILED"); return false; }
  /* Park the cursor on a cell that exists in the NEW view — its own grid is a different
   * shape, and a stale cell would name a section from the previous map. */
  if (s_cgx >= rg->grid_w) s_cgx = rg->grid_w - 1;
  if (s_cgy >= rg->grid_h) s_cgy = rg->grid_h - 1;
  s_glide = 0;
  log_line("rgn: view -> %s", rgn_view_name(rg, s_view));
  return true;
}

void mr_region_tick(const RgnMap* rg, int level) {
  if (!rg) return;
  /* FRLG's region map is a plain 4bpp TEXT background, and the retail game has NO zoomed
   * region map at all (pokefirered has no ProcessRegionMapInput_Zoomed / UpdateRegionMapZoom;
   * it has a Switch Map menu over four views instead). Level 3 must therefore behave exactly
   * like level 4. Without this the zoomed branch below writes REG_BG2X/Y — affine-only
   * registers that fail SILENTLY on a text BG — and then applies *2 screen scaling over an
   * un-doubled picture, putting the cursor on the wrong cell at double speed. That was the
   * reported "doesn't visually zoom but the cursor acts like it does". */
  if (s_text_bg) level = 4;
  if (++s_blink >= RGN_BLINK * 2) s_blink = 0;
  bool on = (s_blink < RGN_BLINK);

  int px, py;
  rgn_grid_to_px(rg, s_cgx, s_cgy, &px, &py);

  if (level >= 4) {
    /* FULL view: the background is fixed and the SPRITE glides, 2 px per frame over the
     * last 4 frames of the move — exactly SpriteCB_CursorMapFull. */
    int back = 0;
    if (s_glide > 0) {
      int f = s_glide > RGN_GLIDE ? RGN_GLIDE : s_glide;   /* frames still to travel */
      back = f * 2;
      s_glide--;
    }
    int sx = px - s_gdx * back, sy = py - s_gdy * back;
    if (on) obj_set_attr(&oam_mem[RGN_OE_CUR],
                         ATTR0_Y(sy - 4) | ATTR0_4BPP | ATTR0_SQUARE,
                         ATTR1_X(sx - 4) | ATTR1_SIZE_16x16,
                         ATTR2_ID(RGN_TID_CUR16) | ATTR2_PALBANK(RGN_PB_CUR) | ATTR2_PRIO(1));
    else obj_hide(&oam_mem[RGN_OE_CUR]);
  } else {
    /* ZOOMED view: the cursor stays put and the BACKGROUND scrolls, 1 px per frame for 8
     * frames per cell — SpriteCB_CursorMapZoomed is empty for exactly this reason. */
    int tx, ty;
    centre_scroll(rg, s_cgx, s_cgy, &tx, &ty);
    if (s_glide > 0) {
      int step = (tx - s_scx); s_scx += (step > 0) ? 1 : (step < 0 ? -1 : 0);
      step     = (ty - s_scy); s_scy += (step > 0) ? 1 : (step < 0 ? -1 : 0);
      if (s_scx == tx && s_scy == ty) s_glide = 0; else s_glide--;
      if (s_glide == 0) { s_scx = tx; s_scy = ty; }
    } else { s_scx = tx; s_scy = ty; }
    REG_BG2X = s_scx << 8;
    REG_BG2Y = s_scy << 8;
    /* Draw the cursor where the cell ACTUALLY is on screen. That is the middle whenever
     * the scroll could centre it, and correctly off-centre near the edges of the picture
     * where the view has hit its clamp — otherwise the square would lie about which cell
     * it is on. */
    int cpx, cpy;
    rgn_grid_to_px(rg, s_cgx, s_cgy, &cpx, &cpy);
    int csx = (cpx - s_scx) * 2, csy = (cpy - s_scy) * 2;
    if (on) obj_set_attr(&oam_mem[RGN_OE_CUR],
                         ATTR0_Y(csy - 12) | ATTR0_4BPP | ATTR0_SQUARE,
                         ATTR1_X(csx - 12) | ATTR1_SIZE_32x32,
                         ATTR2_ID(RGN_TID_CUR32) | ATTR2_PALBANK(RGN_PB_CUR) | ATTR2_PRIO(1));
    else obj_hide(&oam_mem[RGN_OE_CUR]);
  }
  /* The player's own icon, exactly as the game marks where you are. map_oam's player
   * tiles are still in VRAM (moam_shutdown hides sprites, it does not wipe tiles), so a
   * 16x16 OBJ at that tile id draws the head-and-shoulders half of the overworld sprite
   * with no extra ROM access. */
  /* ...and only on the view that actually contains them. FRLG's L key switches Kanto <->
   * the Sevii Islands, and the grid coordinates do not carry over: drawing the marker
   * regardless put Red's face on Sevii while he was standing in Lavender Town. */
  if (s_phave && s_view == s_pview) {
    int ppx, ppy;
    rgn_grid_to_px(rg, s_pgx, s_pgy, &ppx, &ppy);
    int sx, sy;
    if (level >= 4) { sx = ppx; sy = ppy; }
    else            { sx = (ppx - s_scx) * 2; sy = (ppy - s_scy) * 2; }

    /* Guy: "switching the player face on the map with a dot is not a good solution — check
     * how they place the whole player face and do the same." So draw the real character.
     *
     * The overworld sprite map_oam already resolved is STILL IN VRAM at this point: the
     * region view hides OBJs, it never wipes tiles, and the ranges do not overlap (player
     * 976..991, region cursor 1000..1016). Which 16x16 slice is the FACE matters: in a
     * 16x32 overworld frame the top eight rows are empty and the cap only starts around
     * row 10, so tiles 0..3 render as a bare hat brim. Rows 8..23 — tiles 2..5, contiguous
     * under 1D mapping, hence tid+2 — are cap, face and shoulders. That costs no new ROM
     * anchor, which
     * matters: sRegionMapPlayerIcon_* would need a hand-verified address for all eleven
     * game/revision pairs, and a wrong one draws garbage rather than nothing.
     *
     * The procedural magenta pin stays as the fallback for when the sprite is unavailable
     * (an unknown graphics id, or the tile pool full), because "no marker at all" would be
     * a regression from what is on the card today. */
    uint16_t ptid; uint8_t ppb, ph;
    bool face = moam_player_sprite(&ptid, &ppb, &ph);
    if (face) ptid = s_face_ok ? RGN_TID_FACE : (uint16_t)(ptid + 2);
    int half = face ? 8 : 4;                       /* centre the 16x16 or the 8x8 */
    if (sx > -8 && sx < 240 && sy > -8 && sy < 160)
      obj_set_attr(&oam_mem[RGN_OE_PLR],
                   ATTR0_Y(sy - half) | ATTR0_4BPP | ATTR0_SQUARE,
                   ATTR1_X(sx - half) | (face ? ATTR1_SIZE_16x16 : ATTR1_SIZE_8x8),
                   ATTR2_ID(face ? ptid : RGN_TID_PLR) |
                   ATTR2_PALBANK(face ? ppb : RGN_PB_CUR) | ATTR2_PRIO(1));
    else obj_hide(&oam_mem[RGN_OE_PLR]);
  } else {
    obj_hide(&oam_mem[RGN_OE_PLR]);
  }

  REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D;
}
