#ifndef ROM_GBMAP2_H
#define ROM_GBMAP2_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_sprite_codec.h"   /* GbReadFn */
#include "rom_gbmap.h"         /* gbmap_block_of() -- reused verbatim, see design §1.8 */

/*
 * Gen-2 (Gold/Silver/Crystal) overworld MAP data, located BY SHAPE in the
 * user's own Game Boy Color cartridge dump -- BACKLOG #91 M1-G2
 * (docs/GB-MAP-DESIGN-G2.md, its "Grilling 2026-09-11" section, and
 * docs/briefs/map-g2-brief.md, which this file implements exactly).
 *
 * Gen 1 and Gen 2 share NO table, NO anchor and NO record shape -- this is a
 * new file, not an extension of rom_gbmap.c. Only the *structure* is
 * mirrored: the Scan/scan_one windowed scanner, fileoff(), rd()/rdg() bounds
 * wrappers, the fail_closed() contract.
 *
 * PokeDNA ships NO Nintendo map data -- every table here is a plain read of
 * the user's own ROM file, located structurally, never a hard-coded address.
 * Three tables, each found by anchoring on the CODE that consumes it:
 *
 *   MapGroupPointers  GetAnyMapPointer's own opcode shape gives the table's
 *                     ADDRESS (it lives in the home bank, so address ==
 *                     file offset); GetAnyMapField's own shape (which calls
 *                     GetAnyMapPointer -- cross-checked by call target) gives
 *                     the table's BANK. Exactly 1 hit required for each, and
 *                     the two must cross-validate.
 *   Tilesets          LoadMapTileset's own opcode shape gives BOTH the
 *                     address and the bank in one anchor (extra structural
 *                     filter: the FA/11 operands must be WRAM). Exactly 1
 *                     hit required.
 *
 * Table lengths are DERIVED, never compiled in: MapGroupPointers is
 * self-delimiting (entry[0] - table_addr gives the group count, and
 * entry[n_groups] fails the in-window test); Tilesets rows are walked until
 * a row's reserved dw (offset 0x0B) is non-zero or its pointers/banks are
 * implausible.
 *
 * Pure C: no tonc, no FatFs, no GBA headers -- all I/O goes through the
 * caller's GbReadFn, so tests/host_romgbmap2_test.c runs this exact code on
 * the PC against roms/gb/{Gold,Crystal}.gbc.
 */

#define ROM_GBMAP2_SCRATCH_MIN 2048u

typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t size;

  uint32_t groups_off;     /* MapGroupPointers, file offset      */
  uint8_t  groups_bank;
  uint16_t n_groups;       /* derived, see rom_gbmap2.c           */

  uint32_t tilesets_off;   /* Tilesets, file offset               */
  uint8_t  n_tilesets;     /* derived                             */

  int ok;
} RomGbMap2;

/* One map, fully resolved (design §2.1/§2.2/§2.3). */
typedef struct {
  uint8_t  group, number;
  uint8_t  tileset_id, environment;
  uint8_t  tod_palette;    /* record byte 0x07 & 0x0F -- MUST be masked;
                             * the high nibble is a phone-service flag */
  uint8_t  border_block;
  uint8_t  height, width;  /* in blocks */
  uint32_t blocks_off;     /* file offset, height*width bytes */
  uint8_t  conn_mask;      /* bit0 E, bit1 W, bit2 S, bit3 N */
  uint8_t  nconn;
  struct { uint8_t group, number, width, len, y, x; } conn[4];   /* N,S,W,E source order */
} GbMap2Map;

typedef struct {
  uint32_t gfx_off;
  uint32_t meta_off;   /* blockset: 16 B/block */
  uint32_t coll_off;   /* collision: 4 B/block, unused by a read-only viewer */
  uint32_t pal_off;    /* PalMap: bare dw, no bank byte (see design §7.2) */
  uint32_t meta_len;   /* blockset size in bytes, derived from Coll-Meta */
} GbMap2Tileset;

/* Locate the 2 tables above (3 code anchors). `scratch`/`scratch_len`
 * (>= ROM_GBMAP2_SCRATCH_MIN) is used only during this call and may be
 * reused for anything else afterward. Returns g->ok; every g-> field is 0
 * on failure (fail-closed, same contract as rgm1_open()). */
bool rgm2_open(RomGbMap2* g, GbReadFn read, void* ctx, uint32_t size,
               uint8_t* scratch, uint32_t scratch_len);

/* Resolve group/number's Map + MapAttributes + connections. false on any
 * bounds/structural failure (fails closed). */
bool rgm2_map(const RomGbMap2* g, uint8_t group, uint8_t number, GbMap2Map* out);

/* Resolve tileset_id's tileset row. false on any bounds failure. */
bool rgm2_tileset(const RomGbMap2* g, uint8_t tileset_id, GbMap2Tileset* out);

/*
 * ---------------------------------------------------------------- colour --
 * M1-G2 colour (BACKLOG #91, design doc §7): DAY palettes + roofs, located
 * by shape same as the map tables above. Two anchors:
 *   PalMap consumer   gives the PalMap's own hosting BANK (per-game,
 *                      Gold $02 / Crystal $13, stated nowhere in ROM data --
 *                      must be discovered, never hard-coded). Expect
 *                      EXACTLY 2 hits (two genuine call sites), both
 *                      resolving to the SAME bank -- the "exactly 1 hit"
 *                      rule the map anchors use does NOT apply here.
 *   env/palette idiom  a proximity-gated PAIR of idioms (neither alone is
 *                      unique) recovers EnvironmentColorsPointers (LD HL)
 *                      and TilesetBGPalette (LD DE) together. Exactly 1
 *                      combined hit. RoofPals sits ~60-70 bytes further
 *                      inside the SAME located routine -- no separate
 *                      search, just the second occurrence of the same
 *                      chained x8 idiom within the routine's own window.
 *
 * Fallback discipline: either anchor returning anything other than its
 * expected hit count means "this ROM does not match my model" -- the
 * caller's own job is to fall back to the fixed grey ramp for the WHOLE
 * screen, never to guess an address (rgm2_colour_open() simply returns
 * false, ok=0, every field zeroed, same fail-closed contract as rgm2_open).
 */
typedef struct {
  uint32_t env_ptrs_off;    /* EnvironmentColorsPointers, file offset       */
  uint32_t bg_pal_off;      /* TilesetBGPalette, file offset                */
  uint32_t roof_pals_off;   /* RoofPals, file offset                        */
  uint8_t  palmap_bank;     /* PalMap's own hosting bank (per-game)         */
  int      ok;
} RomGbMap2Colour;

bool rgm2_colour_open(RomGbMap2Colour* c, GbReadFn read, void* ctx, uint32_t size,
                       uint8_t* scratch, uint32_t scratch_len);

/* PalMap's own nibble for a RAW tile id (one nibble per raw id, 2 ids/byte,
 * low nibble for even ids -- design §7.2). `pal_off` is the tileset's own
 * GbMap2Tileset.pal_off (a bare dw; dereferenced in `c`'s own palmap_bank).
 * `c` must have c->ok. */
bool rgm2_colour_nibble(const RomGbMap2Colour* c, GbReadFn read, void* ctx, uint32_t size,
                         uint32_t pal_off, uint8_t raw_tile_id, uint8_t* out_nibble);

/* Resolve `palette_index` (0-7, a PalMap nibble already masked with & 0x07)
 * under `environment` (a Map record's own byte 0x02) into 4 RGB15 colours,
 * DAY only (design §7.1/§7.5/§7.8) -- already GBA-native, no conversion
 * (§7.3). Also returns the resolved bg_idx (the TilesetBGPalette row
 * index) so the caller can test it against PAL_BG_ROOF (6) for the roof
 * override. `c` must have c->ok. */
bool rgm2_colour_palette(const RomGbMap2Colour* c, GbReadFn read, void* ctx, uint32_t size,
                          uint8_t environment, uint8_t palette_index,
                          uint16_t out4[4], uint8_t* out_bg_idx);

/* Roof colours (design §7.4): only when environment is TOWN(1)/ROUTE(2);
 * `group` is the map's own 1-based group id, indexed DIRECTLY (no
 * decrement). `LoadMapPals` (engine/gfx/color.asm) copies TWO RGB15 words
 * (`ld bc, 4`) into palette slot PAL_BG_ROOF (=6)'s own colours 1 and 2 --
 * out_day[0] is the roof entry's FIRST word (morn), out_day[1] the SECOND
 * (day); the caller applies out_day[1] (design posture is DAY-only), never
 * out_day[0]. Both zeroed on failure. `c` must have c->ok. */
bool rgm2_colour_roof(const RomGbMap2Colour* c, GbReadFn read, void* ctx, uint32_t size,
                       uint8_t environment, uint8_t group, uint16_t out_day[2]);

#endif /* ROM_GBMAP2_H */
