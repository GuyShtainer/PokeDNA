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

#endif /* ROM_GBMAP2_H */
