#ifndef MAP_GFX_H
#define MAP_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"
#include "map_render.h"

/*
 * Mode-0 map graphics — the GBA-only half of the overworld viewer.
 * (The pure-C half is map_render.c, which is host-tested against a Python reference.)
 *
 * Full derivation in docs/analysis-2026-07-29/MODE0-IMPLEMENTATION-PLAN.md. The parts
 * that constrain this file:
 *
 * WHY MODE 0. MEASURED across every Emerald/FireRed map: a map's decompressed tiles +
 * metatile tables reach 49-51 KB, and the borrowed EWRAM arena is only 35,712 B. Caching
 * tiles in EWRAM is impossible. In Mode 0 the 32 KB of tiles live in VRAM and scrolling
 * is free hardware work instead of ~38,400 pixel writes per step.
 *
 * BG VRAM (64,544 of 65,536 B used; charblocks and screenblocks share one address space,
 * so this is proven in BYTES):
 *   0x06000000  16,384  CBB0  primary tileset char, 4bpp, tile ids 0..511
 *   0x06004000  16,384  CBB1  secondary tileset char, ids 512..1023
 *   0x06008000  16,384  CBB2  reserved for the Z1/Z2 zoom mips (stage B)
 *   0x0600C000   3,104  CBB3  HUD font + fill tile
 *   0x0600D000   4,096  SBB26+27  reserved: affine tilemap for Z1/Z2 (stage B)
 *   0x0600E000   2,048  SBB28  reserved: BG2, middle metatile layer (3-layer mode)
 *   0x0600E800   2,048  SBB29  BG1 tilemap — metatile TOP layer
 *   0x0600F000   2,048  SBB30  BG3 tilemap — metatile BOTTOM layer
 *   0x0600F800   2,048  SBB31  BG0 tilemap — HUD text
 *
 * HARD RULE from the aliasing: BG0's char base is CBB3 (0x0600C000) and the affine
 * tilemap + screenblocks live inside CBB3, so **BG0 tile ids must stay <= 127** or a HUD
 * glyph renders a tilemap as pixels. Tile 128 IS 0x0600D000, the affine map — the cap was
 * previously documented as 239, which is wrong by 2x. sys8Font uses ids 0..95, so there
 * is no live collision today; the rule as written was simply false.
 *
 * TWO LAYERS, NOT THREE, in this stage. A metatile has 2 layers (8 tilemap entries); the
 * game spreads them over 3 BGs and uses the layer TYPE to pick which two, purely so
 * sprites can interleave at different depths. For a viewer the result is PIXEL-IDENTICAL
 * (MEASURED: the bottom half is opaque in all but 6 of 981 COVERED metatiles), so this
 * stage uses BG3=bottom and BG1=top and skips the attribute cache entirely. SBB28 stays
 * reserved so the 3-BG version can be added later for sprite depth.
 */

/* The BG is 32x32 tiles = 256x256 px = 16x16 metatiles, and it WRAPS. Because the 256-px
 * wrap period is exactly the 16-metatile ring, the screen-entry index is a plain mask —
 * no ring bookkeeping is needed at all. */
#define MGFX_RING 16
#define MGFX_SE(mx, my) ((((my) & 15) << 6) | (((mx) & 15) << 1))

/* ---- stitching ------------------------------------------------------------
 * A connected neighbour, plus a cached STRIP of its blockdata so the seam can be drawn
 * with the neighbour's real blocks instead of the border pattern.
 *
 * Depths come from the registers, not from taste. At Z0 the camera shows cols [cur-7,cur+7]
 * and rows [cur-5,cur+5]; at Z1 [cur-15,cur+15] and [cur-10,cur+10]. The cursor is confined
 * to the map (it switches maps the instant it leaves), so the deepest OFF-MAP cell that can
 * ever be visible is 15 columns / 10 rows. One cell of margin gives 16 / 11. Sizing for the
 * wider Z1 FILL window instead would cost 6,432 B and land Emerald Route 127 on exactly
 * 35,712 of a 35,712 B arena — no. */
/* Two depth tiers, because the two zoom levels see very different distances:
 *   Z1 (1/2)  visible cols [cur-15,cur+15], rows [cur-10,cur+10]  -> 16 / 11 with margin
 *   Z2 (1/4)  visible cols [cur-30,cur+30], rows [cur-20,cur+20]  -> 31 / 21 with margin
 * Sizing only for Z1 (what this used to do) means that at 1/4 you are looking straight
 * past the end of every strip into border — the map stops being stitched exactly when you
 * zoom out far enough to care.
 *
 * The Z2 tier does NOT always fit: a wide map with four connections wants ~24 KB against
 * ~21 KB free. So build_connections tries Z2 depth first and falls back to Z1 depth per
 * connection, and to no strip at all (border) if even that will not fit. Degrading one
 * edge is much better than refusing the map. */
#define STRIP_WE_FAR 31
#define STRIP_NS_FAR 21
#define STRIP_WE     16
#define STRIP_NS     11
#define MGFX_MAX_CONN 6      /* MEASURED max CARDINAL connections on one map = 4         */
#define MGFX_MAX_WARP 40     /* MEASURED max warps on one map = 38                       */

typedef struct {
  uint8_t   dir;             /* ROM_CONN_SOUTH..EAST only — 5/6 are not spatial          */
  uint8_t   group, num;
  int32_t   off;             /* SIGNED: FRLG really contains -120                        */
  int32_t   nb_w, nb_h;      /* neighbour dims, so a crossing needs no SD read           */
  int16_t   x0, y0, w, h;    /* strip rect, in CURRENT-map local coords                  */
  uint16_t* cells;           /* w*h row-major; NULL => that edge shows the border        */
} MapConn;

/* Arena slices + camera state for one loaded map. */
typedef struct {
  uint8_t*  mt_prim;  uint32_t mt_prim_len;   /* cached metatile tables */
  uint8_t*  mt_sec;   uint32_t mt_sec_len;
  uint16_t* blocks;   uint32_t blocks_cells;  /* cached blockdata for the whole map */
  int  base_mx, base_my;                      /* top-left metatile currently in the ring */
  bool ring_valid;

  /* The map's BORDER metatiles (MapLayout.border), shown OUTSIDE the map bounds — which
   * is what the game itself does. Writing tilemap entry 0 there instead shows tileset
   * TILE 0, which is a real, arbitrary tile: that was the "random tile outside the room". */
  uint16_t border[16];
  uint8_t  bw, bh;

  /* zoom (Z1) state */
  uint8_t* slot_of;        /* metatile id -> affine tile slot, 0xFF = unassigned (1024 B) */
  int      slots_used;
  /* Z2 dictionary quality. `filler_cells` of `window_cells` in the last Z2 build resolved to
   * a flat tile instead of an exact mip; opt_* records the window the dictionary was last
   * optimised for, so the optimiser runs once per window and not once per frame. */
  uint16_t filler_cells, window_cells;
  int      opt_mx, opt_my;
  bool     opt_valid;
  int      aff_base_mx, aff_base_my;
  bool     aff_valid;

  /* stitching + warps, both cached in the arena at load time */
  MapConn  conn[MGFX_MAX_CONN];
  uint8_t  conn_n;
  RomWarp* warps;
  uint8_t  warp_n;
} MapGfx;

/* Switch to Mode 0 and set up the BGs + HUD text. Screen is force-blanked until
 * mgfx_show() so nothing garbled is ever visible. */
void mgfx_enter(void);
/* The two HUD-shading windows (see mgfx_enter). Every map-screen DISPCNT write must OR
 * this in, or the strips stop being darkened and the text goes unreadable again. */
#define MGFX_HUD_WIN (DCNT_WIN0 | DCNT_WIN1)

/* The blend that darkens the map inside those windows. map_oam's dust puff needs REG_BLDCNT
 * for its own alpha fade, and BLDCNT holds ONE mode — so the puff borrows the register and
 * must put THESE values back when it is done, not zero. Zeroing turned the HUD unreadable
 * again for the rest of the session after the first drop. */
#define MGFX_HUD_BLDCNT (BLD_BUILD(BLD_BG1|BLD_BG2|BLD_BG3|BLD_OBJ|BLD_BACKDROP, 0, 3))
#define MGFX_HUD_BLDY   9

void mgfx_show(void);
/* Restore the Mode-3 bitmap UI. Registers that are WRITE-ONLY (BGxHOFS/VOFS, the affine
 * regs, MOSAIC, BLDY) are set to known values rather than "restored", because they cannot
 * be read back. */
void mgfx_exit(void);

/* Load one map: decompress both tilesets into VRAM, build BG PALRAM, and cache the
 * metatile tables + blockdata in the arena.
 *
 * SEQUENCING IS LOAD-BEARING. The naive allocation is 62,736 B against a 35,712 B arena.
 * It fits only because the LZ77 staging buffers are TRANSIENT: this function uses
 * 12,288 + 20,480 = 32,768 B for decompression, then RELEASES them before allocating the
 * metatile/blockdata caches (29,968 B worst case). The arena aliases the PC-storage
 * buffer, so an overrun corrupts the user's Pokemon — never hold both sets at once.
 *
 * Does its own SD reads, so it must be called with the screen blanked (mgfx_enter) or
 * showing a static "Loading" frame — nothing may render mid-transfer. */
bool mgfx_load(MapGfx* g, MapRender* mr, const RomCtx* rom, const RomLayout* lay,
               uint32_t conn_addr, uint32_t events_addr,
               uint8_t* arena, uint32_t arena_len);

/* Everything mgfx_load needs, checked WITHOUT touching *g.
 *
 * mgfx_load memsets *g and resets the arena BEFORE eight of its `return false` paths, so a
 * failed reload leaves g->blocks NULL and the next HUD paint dereferences it. Call this
 * first and a bad neighbour simply refuses, leaving the current map on screen untouched.
 * Does SD reads, so call it inside the same rmbl_pause window as the load. */
bool mgfx_can_load(const RomCtx* rom, const RomLayout* lay, uint32_t arena_len);

/* Arena bytes still HELD after a load (caches + strips + warps), as opposed to
 * mgfx_arena_peak() which is dominated by phase 1's transient 32 KB. */
uint32_t mgfx_arena_phase2(void);

/* Point the camera at a metatile (it ends up at screen centre), refilling only the newly
 * exposed column/row. A 1-metatile step is 16 metatile lookups + 128 halfword writes,
 * about 1.6% of one VBlank. */
void mgfx_camera(MapGfx* g, const MapRender* mr, int cur_mx, int cur_my);

/* ---- zoom ------------------------------------------------------------------
 * Z0 is the Mode-0 tiled view above. Z1 zooms out to 8x8 px per metatile using an
 * AFFINE background whose tiles are per-metatile "mips".
 *
 * Why affine and not a software bitmap render: a bitmap mode's framebuffer starts at
 * 0x06000000 — exactly where the tileset char data lives — so switching to Mode 3/4
 * would destroy the tiles and force an SD re-decompress on every zoom-in. The affine BG
 * keeps the tiles resident, so a zoom change costs ZERO SD access.
 *
 * THE HARD CAP: an affine BG's tilemap entries are ONE byte, so it can address at most
 * 256 distinct tiles, and it is 8bpp only. MEASURED across every map in all three ROMs,
 * for every cursor position, with off-map cells resolved to the border metatile:
 *   Z1 window (34x22 metatiles): max 227 EM / 217 FR / 204 RU  -> fits, 255 usable
 *   Z2 window (60x40 metatiles): max 428 / 303 distinct        -> DOES NOT FIT
 * So Z1 is exact everywhere and Z2 needs a deliberate approximation (frequency-ranked
 * slots with an average-colour fallback for the overflow); Z2 is not implemented yet.
 *
 * ONE-BYTE ENTRIES ARE A TRAP. BG VRAM turns an 8-bit store into a halfword *0x0101, so
 * the tilemap must be written as ALIGNED HALFWORD PAIRS — which is why the window origin
 * is forced even and the width is 34, not 32. See the long note in map_gfx.c. */
/* The zoom ladder. 0-2 are the tile view; 3-4 are the game's own REGION MAP, which is one
 * data set shown at two scales (see map_region.h). */
#define MGFX_ZOOM_TILE_MAX 2
#define MGFX_ZOOM_MAX 4                 /* 0 = 1:1, 1 = 1/2, 2 = 1/4~, 3 = region, 4 = region wide */

/* The two VRAM blocks the affine zoom owns. map_region.c reuses BOTH for the region map
 * (14,912 B of chars fits CBB2's 16,384; the region tilemap is exactly 4,096 B), which is
 * why leaving the region view must invalidate the mip dictionary. */
#define MGFX_VRAM_MIP_CHAR 0x06008000u
#define MGFX_VRAM_AFF_MAP  0x0600D000u
#define MGFX_AFF_TILES 256              /* the 1-byte-entry cap */
#define MGFX_AFF_RING  64               /* affine map is 64x64 metatiles, and wraps */

/* Re-choose which metatiles get the 242 exact Z2 slots, ranking by how much of the window
 * each one actually covers (centre-weighted) instead of by which was seen first. Costs up to
 * 242 metatile renders, so the caller decides WHEN — see mgfx_zoom_wants_opt(). */
void mgfx_zoom_optimise(MapGfx* g, const MapRender* mr, int cur_mx, int cur_my);

/* True when the last Z2 build left more than 1% of the window on flat tiles AND the
 * dictionary has not already been optimised for this window. Guy set the 1% bar: below that
 * the filler is scattered pixels, above it you see blocks of flat colour. */
bool mgfx_zoom_wants_opt(const MapGfx* g, int cur_mx, int cur_my);

/* Percent of the last Z2 window that came out as filler, 0..100 — for the HUD. */
int  mgfx_zoom_filler_pct(const MapGfx* g);

/* Build the mip dictionary + affine tilemap for the window around (cur_mx, cur_my), then
 * show it. Returns false if the window somehow needs more than MGFX_AFF_TILES distinct
 * metatiles (cannot happen at Z1 per the measurement, but it is checked, not assumed). */
bool mgfx_zoom_build(MapGfx* g, const MapRender* mr, int cur_mx, int cur_my, int level);
void mgfx_zoom_show(int level, int cur_mx, int cur_my);
void mgfx_z0_show(void);                /* back to the Mode-0 1:1 view */

/* Re-upload the map's BG palettes. The region-map view overwrites all 256 entries, so
 * coming back to the tile view needs this; the tilesets themselves live in CBB0/CBB1 and
 * are untouched, so no SD reload is required. */
void mgfx_repaint_palettes(const MapRender* mr);

/* Peak arena bytes this module used for the last load — for the hard-rule-2 assertion. */
uint32_t mgfx_arena_peak(void);

/* Metatile lookups that fell outside the cached tables since boot. Expected: always 0.
 * NOTE this is a bounds check on an ID, not on table CONTENTS — MEASURED over 1,337 maps
 * and 802,299 blockdata cells, the corruption actually observed on this hardware (the
 * FatFs 2-byte shift) would have raised it 0 times, because a shifted cell is still a
 * valid id. Perfect specificity, near-zero sensitivity: keep it, trigger on > 0, but it
 * cannot be the safety net. That is what the fingerprints below are for. */
uint32_t mgfx_mt_misses(void);

/* ---- render integrity -------------------------------------------------------
 * Cheap tamper detection over the state a map load produces and NOTHING should touch
 * afterwards. Deliberately excludes SBB29/30, the affine tilemap, the CBB2 mips, slot_of
 * and OBJ tiles >= 512 — all legitimately rewritten every camera step, and all free to
 * rebuild anyway.
 *
 * Why this earns its keep even after every known bug is fixed: Read_SD_sectors returns
 * RES_OK UNCONDITIONALLY and DMAs even after a timeout, so FatFs CANNOT surface a read
 * error — a failed read arrives as success holding garbage, and a map load streams ~50 KB
 * through it. No code fix can remove that class. This is the only thing that can tell
 * "PokeDNA has a bug" apart from "the microSD returned garbage and lied", which are
 * otherwise indistinguishable from a photo of the screen. */
#define MGFX_FP_CANARY 0x01   /* magic words past the HUD font in CBB3            */
#define MGFX_FP_MT     0x02   /* the metatile tables in OBJ VRAM                  */
#define MGFX_FP_CHAR   0x04   /* the tileset char data, CBB0+CBB1                 */
#define MGFX_FP_ARENA  0x08   /* the borrowed arena beyond slot_of (it IS g_pc)   */
#define MGFX_FP_PAL    0x10   /* BG palette banks 0..14                           */
#define MGFX_FP_SLICES 4      /* check one slice per frame; canary checked always */

void        mgfx_fp_capture(const MapGfx* g, const MapRender* mr, const uint8_t* arena);
uint32_t    mgfx_fp_check(const MapGfx* g, const MapRender* mr, const uint8_t* arena, int slice);
const char* mgfx_fp_name(uint32_t bits);

#endif /* MAP_GFX_H */
