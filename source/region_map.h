#ifndef REGION_MAP_H
#define REGION_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"

/*
 * Gen-3 REGION MAP (the Pokenav / Town Map screen), read live out of the USER'S OWN
 * Pokemon ROM. Same posture as rom_map.c: PokeDNA ships no Nintendo data, nothing is
 * extracted into the repo, everything here is a transient read of a file the user
 * already owns. Pure C — no tonc, no GBA headers, no BIOS — so tests/host_regionmap_test.c
 * runs the identical code on a PC against real retail dumps.
 *
 * =============================================================================
 * WHAT THE TWO ZOOM LEVELS ACTUALLY ARE  (read this before wiring the UI)
 * =============================================================================
 * On Ruby/Sapphire/Emerald the region map is ONE image and the game's "zoomed" mode
 * is nothing but an affine-BG scale of it. From pokeemerald src/region_map.c:
 *
 *     full view : CalcZoomScrollParams(0, 0, 0, 0, 0x100, 0x100, 0)
 *     zoomed    : CalcZoomScrollParams(scrollX, scrollY, 0x38, 0x48, 0x80, 0x80, 0)
 *
 * BG2PA/BG2PD are 8.8 source-texels-per-screen-pixel: 0x100 = 1:1, 0x80 = 2x
 * magnification. Nothing else changes — same tileset, same tilemap, same palette.
 * The scroll registers reduce to (RegionMap_SetBG2XAndBG2Y):
 *     BG2X = (scrollX << 8) + 0x1C00      BG2Y = (scrollY << 8) + 0x2400
 * so the top-left source texel of the zoomed view is (scrollX + 0x1C, scrollY + 0x24),
 * and the game seeds scrollX = cursorX*8 - 0x34, scrollY = cursorY*8 - 0x44.
 * rgn_zoom_origin() below reproduces exactly that.  In zoomed mode SCREEN (56,72) is
 * the centre of the selected grid cell — measured, not guessed (see the header comment
 * in region_map.c).
 *
 * So: BOTH levels the user asked for come from ONE tileset + ONE tilemap + ONE palette.
 * The GBA side should upload them ONCE and switch levels by writing BG2PA/PD
 * (0x100 <-> 0x80) plus BG2X/BG2Y. No re-decompression, no second data set.
 *
 * FireRed/LeafGreen is a DIFFERENT screen and has NO zoom at all. Instead it has FOUR
 * views over one shared tileset (Kanto, Sevii 1-3, Sevii 4-5, Sevii 6-7) selected with
 * the "switch map" menu. This module exposes all four (rgn_view_count / RgnView) and
 * refuses view indices a game does not have. A 2x level on FRLG is available
 * (rgn_render_at with scale 2) but it is OUR addition, not a mode retail FRLG has.
 *
 * =============================================================================
 * PER-GAME FACTS
 * =============================================================================
 *                       RSE (BPEE/AXVE/AXPE)        FRLG (BPRE/BPGE)
 *   colour depth        8bpp                        4bpp
 *   tile data           14,912 B  (233 tiles)       10,240 B  (320 tiles)
 *   tilemap             4,096 B, 64x64, 1 B/entry   1,200 B, 30x20, 2 B/entry
 *                       (affine BG, screensize 2)   (text BG, entry = tile|flip|pal)
 *   palette             48 colours at PALRAM 112    80 colours at PALRAM 0 (banks 0-4)
 *   mapsec grid         28 x 15                     22 x 15
 *   grid cell (0,0) at  image pixel (8,16)          image pixel (32,32)
 *   views               1                           4
 *   mapsec ids          0 .. 212 (E) / 0 .. 87 (RS) 88 .. 196   (NONE = 197)
 *   rect table          gRegionMapEntries           sMapSectionTopLeftCorners
 *                       {u8 x,y,w,h; const u8*name} + sMapSectionDimensions (u16[2] each)
 *
 * Note the RSE struct field order: x,y,width,height come FIRST and the name pointer
 * LAST. Reading it as {name, x, y, w, h} — the intuitive order — yields pure garbage.
 *
 * =============================================================================
 * MEMORY THE GBA SIDE NEEDS
 * =============================================================================
 * Worst case is RSE. Two ways to get the data in:
 *
 *  A) HARDWARE PATH (recommended, and what the 20 KB arena affords).
 *     Give the tiles to a VRAM charblock and the tilemap to a VRAM screenblock; only
 *     the compressed stream ever touches EWRAM:
 *         staging buffer      3,444 B   (rgn_stream() bound for RSE tiles; the tilemap
 *                                        stream is 860 B — reuse the same buffer)
 *         palette image         512 B   (uint16_t[256], or 96 B straight to PALRAM)
 *         RgnMap                ~96 B
 *         ------------------------------------------------------------------
 *         EWRAM TOTAL        ~4.1 KB    VRAM 14,912 B chars + 4,096 B screen
 *     Read the compressed stream into the staging buffer with the RomReadFn, then use
 *     the BIOS SWI 0x12 (LZ77UnCompVram) to expand it into VRAM. You MUST use the VRAM
 *     variant: VRAM ignores 8-bit writes, and mr_lz77() writes bytes, so decompressing
 *     straight into VRAM with mr_lz77 silently corrupts every other byte.
 *
 *     BG wiring for RSE (what the game itself does, InitRegionMapData + LoadRegionMapGfx):
 *       - BG2, ROT/SCALE (Mode 1 or 2), 8bpp, WRAPAROUND on, SCREENSIZE 2 (512x512),
 *         i.e. the 4,096-byte tilemap is 64x64 ONE-BYTE entries — NOT u16 text entries,
 *         and it needs 4 KB (two screenblocks) of contiguous VRAM.
 *       - Palette: copy r->pal_colours colours to PALRAM index r->pal_base (112). The
 *         tile bytes are direct 256-colour indices; measured range is 113..140, so
 *         nothing outside the loaded window is ever referenced.
 *       - Whole region: BG2PA = BG2PD = 0x100, BG2PB = BG2PC = 0, BG2X = BG2Y = 0.
 *       - Zoomed:       BG2PA = BG2PD = 0x080, BG2PB = BG2PC = 0,
 *                       BG2X = (scrollX << 8) + 0x1C00, BG2Y = (scrollY << 8) + 0x2400
 *                       where scroll = the rgn_zoom_origin() result minus 0x1C / 0x24.
 *     FRLG is a plain 4bpp TEXT BG instead: 30x20 u16 entries that must be copied
 *     ROW BY ROW into a 32-wide screenblock (30 entries then skip 2), palette banks 0-4.
 *
 *  B) SOFTWARE PATH (host tests, or a Mode-3/Mode-4 fallback): rgn_tiles() +
 *     rgn_tilemap() decompress into caller EWRAM with mr_lz77.
 *         tiles 14,912 + tilemap 4,096 + palette 512  =  19,520 B
 *     That fits a 20 KB arena with 960 B to spare and nothing else in it. If the arena
 *     is shared, use path A.
 *     FRLG software path is 10,240 + 1,200 + 512 = 11,952 B.
 *
 * There is NO full-frame RGB buffer anywhere: rgn_render_at() writes into whatever
 * rectangle the caller provides (one scanline, a strip, or Mode 3 VRAM directly with
 * stride 240) — a 240x160x2 = 76,800 B bitmap does not exist in EWRAM and must not.
 *
 * =============================================================================
 * WHAT IS NOT HERE
 * =============================================================================
 *  - The frame/border art, the cursor sprite, the player icon, the fly-destination
 *    icons, FRLG's map-edge sprites, map previews and the "switch map" menu.
 *  - FRLG's LAYER_DUNGEON overlay is read (rgn_section_at with RGN_LAYER_DUNGEON) but
 *    the dungeon icons are not.
 *  - Emerald's dynamic mapsecs: Terra/Marine Cave and the Aqua Hideout move or rename
 *    based on save state (CorrectSpecialMapSecId_Internal). This module reports the
 *    static ROM data only.
 */

/* ---- identity -------------------------------------------------------------- */

typedef enum {
  RGN_FAM_NONE = 0,
  RGN_FAM_RS,          /* Ruby / Sapphire        */
  RGN_FAM_E,           /* Emerald                */
  RGN_FAM_FRLG,        /* FireRed / LeafGreen    */
} RgnFamily;

/* View index. RSE has exactly one (RGN_VIEW_MAIN). FRLG has four. */
typedef enum {
  RGN_VIEW_MAIN     = 0,   /* Hoenn on RSE, Kanto on FRLG */
  RGN_VIEW_SEVII123 = 1,   /* FRLG only */
  RGN_VIEW_SEVII45  = 2,   /* FRLG only */
  RGN_VIEW_SEVII67  = 3,   /* FRLG only */
} RgnView;
#define RGN_MAX_VIEWS 4

/* FRLG stores two mapsec planes per view; RSE has only the map plane. */
#define RGN_LAYER_MAP     0
#define RGN_LAYER_DUNGEON 1   /* FRLG only; RSE returns MAPSEC_NONE */

/* Buffer sizes the caller must be able to satisfy (RSE is the worst case). */
#define RGN_MAX_TILES_BYTES   14912
#define RGN_MAX_TILEMAP_BYTES  4096
#define RGN_PAL_ENTRIES         256   /* uint16_t[256], a full BG palette image */

/* Colour written where the source is outside the map image. */
#define RGN_BACKDROP 0x0000

typedef struct {
  const RomCtx* rom;
  RgnFamily family;

  uint8_t  view_count;          /* 1 (RSE) or 4 (FRLG)                              */
  uint8_t  bpp;                 /* 8 (RSE) or 4 (FRLG)                              */
  uint8_t  tilemap_entry_bytes; /* 1 (RSE affine) or 2 (FRLG text)                   */
  uint8_t  layer_count;         /* 1 (RSE) or 2 (FRLG)                              */

  uint8_t  grid_w, grid_h;      /* mapsec grid: 28x15 (RSE) / 22x15 (FRLG)          */
  uint8_t  grid_x0, grid_y0;    /* image pixel of grid cell (0,0): (8,16) / (32,32) */
  uint8_t  map_tw, map_th;      /* tilemap size in tiles: 64x64 (RSE) / 30x20 (FRLG)*/

  uint16_t tiles_bytes;         /* 14912 / 10240                                    */
  uint16_t tilemap_bytes;       /* 4096  / 1200                                     */
  uint16_t pal_colours;         /* 48 / 80                                          */
  uint16_t pal_base;            /* PALRAM index the palette loads at: 112 / 0       */

  uint16_t mapsec_first;        /* 0 (RSE) / 88 (FRLG)                              */
  uint16_t mapsec_count;        /* 213 (E) / 88 (RS) / 109 (FRLG)                   */
  uint16_t mapsec_none;         /* the "no section here" id: 213/88/197             */

  /* ROM addresses, all validated by rgn_open(). */
  uint32_t tiles_addr;                    /* LZ77 char data                         */
  uint32_t pal_addr;                      /* raw BGR555                             */
  uint32_t tilemap_addr[RGN_MAX_VIEWS];   /* LZ77 tilemap, one per view             */
  uint32_t grid_addr[RGN_MAX_VIEWS];      /* u8[layer][grid_h][grid_w] of mapsec ids*/
  uint32_t entries_addr;                  /* RSE only: gRegionMapEntries            */
  uint32_t corners_addr, dims_addr;       /* FRLG only: u16[..][2] each             */

  /* Attached decompressed data (rgn_attach). NULL until then; only the render and
   * lookup helpers that need pixels care. */
  const uint8_t*  tiles;
  const uint8_t*  tilemap;      /* the view given to rgn_attach                     */
  const uint16_t* pal;          /* RGN_PAL_ENTRIES colours                          */
  uint8_t         attached_view;
} RgnMap;

/* ---- open ------------------------------------------------------------------ */

/* Locate and VALIDATE the region-map data in `rom` (which must already be open).
 *
 * Addresses come from pret's per-revision .sym files (pokeemerald / pokeruby +
 * pokeruby_rev1/rev2 + pokesapphire{,_rev1,_rev2} / pokefirered{,_rev1} +
 * pokeleafgreen{,_rev1}), and every one of them is then structurally verified against
 * the file: the LZ77 headers must declare the exact expected decompressed sizes, the
 * mapsec grid must be in range and non-empty, and the rectangle table must decode.
 * If the constants miss (an untested revision), rgn_open falls back to a bounded
 * signature scan (+/-256 KB around the hint, ~4 KB of reads at 4 KB granularity) for
 * the tile stream and re-derives everything from the family's fixed relative offsets.
 *
 * Returns false and zeroes *r for a ROM whose region map cannot be found and proved.
 * A false return must be surfaced, never rendered around. */
bool rgn_open(RgnMap* r, const RomCtx* rom);

const char* rgn_view_name(const RgnMap* r, int view);   /* "HOENN"/"KANTO"/"SEVII 1-3"... */

/* ---- getting the pixels in -------------------------------------------------- */

/* HARDWARE PATH. Returns the ROM address and an upper bound on the byte length of the
 * LZ77 stream, so the caller can slurp it into a small EWRAM staging buffer and hand
 * it to BIOS SWI 0x12 (LZ77UnCompVram) with a VRAM destination.
 * `what` is RGN_STREAM_TILES or RGN_STREAM_TILEMAP; `view` is ignored for tiles.
 * The bound is the padded stream length from the decomp's symbol sizes plus 16 bytes
 * of slack, clamped to the end of the file — over-reading past the stream is harmless
 * because the decompressor stops at the declared output size. */
#define RGN_STREAM_TILES   0
#define RGN_STREAM_TILEMAP 1
bool rgn_stream(const RgnMap* r, int what, int view,
                uint32_t* out_addr, uint32_t* out_max_bytes);

/* SOFTWARE PATH. Decompress with mr_lz77 (map_render.h) into a caller buffer.
 * Return the number of bytes produced, or 0 on failure / insufficient capacity.
 * `cap` must be >= r->tiles_bytes / r->tilemap_bytes respectively. */
uint32_t rgn_tiles(const RgnMap* r, uint8_t* dst, uint32_t cap);
uint32_t rgn_tilemap(const RgnMap* r, int view, uint8_t* dst, uint32_t cap);

/* Build the 256-entry BG palette image the tiles index into: zeroes everything, then
 * copies r->pal_colours colours from ROM to index r->pal_base. `dst` is
 * uint16_t[RGN_PAL_ENTRIES]. On the GBA you can instead copy r->pal_colours*2 bytes
 * straight to PALRAM + r->pal_base*2. */
bool rgn_palette(const RgnMap* r, uint16_t* dst);

/* Point the renderer at buffers the caller owns. `tilemap` must be the decompressed
 * tilemap for `view`. Any of them may be NULL to detach. */
void rgn_attach(RgnMap* r, const uint8_t* tiles, const uint8_t* tilemap,
                const uint16_t* pal, int view);

/* ---- rendering (BGR555) ----------------------------------------------------- */

/* The map image is r->map_tw*8 x r->map_th*8 pixels. RSE's is 512x512 with everything
 * drawn in the top-left ~240x160; FRLG's is exactly 240x160.
 *
 * rgn_render_at draws a w x h rectangle of OUTPUT pixels taking source pixel
 * (src_x + px/scale, src_y + py/scale). scale must be 1 or 2 (2 = the game's zoomed
 * mode on RSE). Source pixels outside the image become RGN_BACKDROP.
 * `stride` is in uint16_t units. Requires rgn_attach. */
bool rgn_render_at(const RgnMap* r, int src_x, int src_y, int scale,
                   int w, int h, uint16_t* dst, int stride);

/* Convenience: the whole-region level exactly as the game shows it — source (0,0),
 * scale 1, 240x160. `dst` must hold 160 rows of `stride` uint16_t. */
bool rgn_render(const RgnMap* r, uint16_t* dst, int stride);

/* ---- the zoom relationship -------------------------------------------------- */

/* Source origin of the ZOOMED (2x) view centred on grid cell (gx,gy), reproducing the
 * retail scroll seed and clamp:
 *     RSE  : src = (gx*8 - 16, gy*8 - 16), clamped to [-16,200] x [-16,96]
 *            (from scrollX in (-0x2c,0xac), scrollY in (-0x34,0x3c), plus 0x1C/0x24)
 *     FRLG : retail has no zoom; we centre the cell in a 120x80 window clamped to the
 *            240x160 image. Flagged in the header so nobody mistakes it for retail.
 * At scale 2, screen pixel (56,72) is the centre of cell (gx,gy) on RSE. */
void rgn_zoom_origin(const RgnMap* r, int gx, int gy, int* src_x, int* src_y);

/* Top-left image pixel of grid cell (gx,gy) — for drawing a marker on the player's
 * MAPSEC. Cells are 8x8 image pixels at every zoom level; multiply by `scale` and
 * subtract the render's src to get screen coordinates. */
void rgn_grid_to_px(const RgnMap* r, int gx, int gy, int* px, int* py);

/* ---- sections --------------------------------------------------------------- */

/* The whole mapsec grid for `view`, layer-major: u8[layer_count][grid_h][grid_w].
 * 420 B on RSE, 660 B on FRLG. Fetch this ONCE when the screen opens — rgn_section_at
 * otherwise costs one SD read per call. */
uint32_t rgn_grid_bytes(const RgnMap* r);
bool     rgn_grid(const RgnMap* r, int view, uint8_t* dst, uint32_t cap);

/* MAPSEC under grid cell (gx,gy) of `view`, LAYER_MAP plane.
 * Out-of-range cells yield r->mapsec_none and return false. */
bool rgn_section_at(const RgnMap* r, int view, int gx, int gy, uint8_t* out_mapsec);

/* Same, choosing the plane. RGN_LAYER_DUNGEON is FRLG-only; on RSE it yields
 * mapsec_none and returns false. */
bool rgn_section_at_layer(const RgnMap* r, int view, int layer, int gx, int gy,
                          uint8_t* out_mapsec);

/* The rectangle a MAPSEC occupies, in GRID cells: RSE reads gRegionMapEntries,
 * FRLG reads sMapSectionTopLeftCorners + sMapSectionDimensions.
 * Returns false for an id outside the game's range or with a zero-sized rect
 * (FRLG's MAPSEC_SPECIAL_AREA is 0x0 — it is the Celadon Dept. Store label, not a
 * place on the map). Any of x/y/w/h may be NULL.
 *
 * PLACING THE PLAYER MARKER — the two families differ and it matters:
 *   RSE  : this is the whole answer. Caves and other indoor mapsecs carry a real
 *          position here (Granite Cave = 1,13) and the game itself uses exactly this
 *          table to seed the cursor.
 *   FRLG : correct for surface mapsecs, but every LAYER_DUNGEON id (Mt. Moon, Viridian
 *          Forest, ...) stores a PLACEHOLDER (0,0,1,1). Their real cell only exists in
 *          the dungeon plane of the grid — use rgn_find_mapsec() and fall back to this
 *          only when that misses. */
bool rgn_mapsec_rect(const RgnMap* r, uint8_t mapsec, int* x, int* y, int* w, int* h);

/* Locate a MAPSEC on the grids: which view, which plane, and the first (top-left-most)
 * cell holding it. This is the robust "where do I draw the marker" call — it is the
 * only thing that positions FRLG's dungeon mapsecs correctly. Returns false if the id
 * is on no grid (RSE caves, FRLG indoor-only ids); fall back to rgn_mapsec_rect then.
 * Any out-param may be NULL. */
bool rgn_find_mapsec(const RgnMap* r, uint8_t mapsec,
                     int* view, int* layer, int* gx, int* gy);

/* Which view shows this MAPSEC (FRLG: Kanto vs the three Sevii screens). Scans both
 * planes of each view's grid. Returns false if it appears on none — a legitimate
 * outcome for indoor-only mapsecs, whose rect is still valid via rgn_mapsec_rect.
 * Always yields RGN_VIEW_MAIN on RSE. */
bool rgn_view_for_mapsec(const RgnMap* r, uint8_t mapsec, int* out_view);

/* RSE only: the section's name in the Gen-3 charset, terminated by 0xFF — feed it to
 * gen3_decode_char (gen3_save.h) one byte at a time. `cap` includes the terminator;
 * 20 bytes is enough (longest measured raw name is 18).
 *
 * Ruby/Sapphire embed the {NAME_END} control code (0xFC 0x00) in the MIDDLE of 16 of
 * their 88 names — "LITTLEROOT" FC 00 <space> "TOWN" — where Emerald has just the
 * space. This strips 0xFC pairs so all three games decode to the same clean string;
 * without that, RS names render as "LITTLEROOT?  TOWN". FC 00 is the only control code
 * any region-map name uses (measured over all 213 Emerald and 88 Ruby entries).
 *
 * FRLG keeps its names in a separate pointer table (sMapNames) that this module does
 * not locate, so it returns false rather than fake one — PokeDNA already has FRLG
 * mapsec names elsewhere. */
#define RGN_NAME_MAX 20
bool rgn_mapsec_name_raw(const RgnMap* r, uint8_t mapsec, uint8_t* dst, int cap);

#endif /* REGION_MAP_H */
