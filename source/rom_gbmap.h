#ifndef ROM_GBMAP_H
#define ROM_GBMAP_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_sprite_codec.h"   /* GbReadFn */

/*
 * Gen-1 (Red/Blue/Yellow) overworld MAP data, located BY SHAPE in the user's
 * own Game Boy cartridge dump -- BACKLOG #91 M1 (docs/GB-MAP-DESIGN.md +
 * its "Grilling 2026-09-10" section, whose 3 corrections this module
 * implements, NOT the design doc's original tables):
 *
 *   1. The Gen-1 map header is NOT a fixed 12 bytes ending in the Object
 *      pointer. It is a 10-byte PARTIAL header (tileset id, height, width,
 *      Blocks ptr, TextPointers ptr, Script ptr, connections bitmask) --
 *      the bitmask is the header's own LAST fixed field -- followed by
 *      popcount(bitmask) 11-byte connection records (N, S, W, E order),
 *      followed LAST by the 2-byte Object pointer (`end_map_header`
 *      appends it AFTER every `connection` line in the source .asm).
 *      A parser that reads the Object pointer at a fixed offset 0x0A
 *      misparses every map with >=1 connection (most outdoor maps).
 *   2. (Gen 2 offsets -- not this module's concern; M1 is Gen 1 only.)
 *   3. (Gen 2 connection width -- not this module's concern.)
 *
 * PokeDNA ships NO Nintendo map data -- every table here is a plain read of
 * the user's own ROM file, located structurally, never a hard-coded address
 * (the same posture rom_map.c/rom_gbui.c already carry). Three tables, each
 * found by anchoring on the CODE that consumes it (never a blind data scan):
 *
 *   MapHeaderBanks   SwitchToMapRomBank's own opcode shape:
 *                    `E5 C5 4F 06 00 3E bb CD ll hh 21 lo hi 09`
 *                    (push hl/push bc/ld c,a/ld b,0/ld a,BANK(MapHeaderBanks)/
 *                    call BankswitchHome/ld hl,MapHeaderBanks/add hl,bc) --
 *                    `bb` is the table's own bank, `lo hi` its address; the
 *                    `CD ll hh` (BankswitchHome's call target) is a WILDCARD,
 *                    so this anchor needs no externally-known routine
 *                    address. IDENTICAL shape in Red and Yellow (verified:
 *                    exactly 1 hit in each, landing on the decomp's own
 *                    MapHeaderBanks symbol byte-for-byte).
 *   MapHeaderPointers LoadMapHeader's own `BIT 7,B / RET NZ` (CB 78 C0),
 *                    unique in both ROMs. Red's very next byte is `21 lo hi`
 *                    (LD HL, MapHeaderPointers -- home bank, addr < 0x4000,
 *                    used directly). Yellow's is `CD lo hi` (CALL
 *                    GetMapHeaderPointer, a bank-switched indirection --
 *                    MapHeaderPointers lives in bank 0x3F on Yellow, not
 *                    bank 0): the call target is followed for up to 48
 *                    bytes looking for `LD A,bank / .. / CALL` (`3E bb ..
 *                    CD`) then the first `LD HL,imm16` (`21 lo hi`) after
 *                    it, giving (bank, addr) the same way the direct case
 *                    gives addr alone (bank 0 implied).
 *   Tilesets         LoadTilesetHeader's own `LD E,A / LD HL,Tilesets`
 *                    (5F 21 lo hi), filtered to the ONE candidate (out of
 *                    dozens of raw `5F 21` hits) followed by one-or-more
 *                    `ADD HL,DE` (19) then `LD DE,imm16` (11 lo hi) whose
 *                    operand is a plausible WRAM address (0xC000-0xDFFF,
 *                    wTilesetBank) -- this structural filter is what makes
 *                    the anchor unique, not the raw 2-byte prefix alone.
 *
 * Whichever bank a table's own code anchor is found in tells that code's
 * (and therefore the table's, since nothing bankswitches between the anchor
 * and reading the operand) bank: bank = anchor_file_offset / 0x4000, so the
 * table's own address operand resolves via fileoff(bank, addr) exactly the
 * way rom_gbui.c's own fileoff() does.
 *
 * Once these 3 tables are located, everything else (a map's own header,
 * blockset, tile graphics) is reached by following ALREADY-VALIDATED
 * pointers -- no further code anchors needed, same "no independent locate
 * needed" posture the design's own table lists for the blockset/tile-gfx
 * rows. Gen-1 tile graphics are UNCOMPRESSED (INCBIN of a flat 2bpp blob,
 * gfx/tilesets.asm) -- the design doc's own "VERIFY compression scheme"
 * item is resolved: there is none.
 *
 * Pure C: no tonc, no FatFs, no GBA headers -- all I/O goes through the
 * caller's GbReadFn, so tests/host_romgbmap_test.c runs this exact code on
 * the PC against roms/gb/{Red,Yellow}.gb.
 */

#define ROM_GBMAP_SCRATCH_MIN 2048u   /* same generous margin rom_gbui.c keeps */

/* wXCoord/wYCoord -> block coordinate: one coord unit is HALF a block
 * (pokered home/overworld.asm:567 `srl c`; engine/overworld/tilesets.asm:49
 * `wYBlockCoord = wYCoord & 1`) -- ONE halving, not two (m1 review D1: a
 * second /2 put the player marker on the Pokemon Center counter instead of
 * the floor on every map). Exposed as a pure function so
 * tests/host_romgbmap_test.c can assert the conversion directly instead of
 * re-deriving pdna_gbmap.c's own inline arithmetic. */
static inline int gbmap_block_of(uint8_t coord) { return (int)(coord >> 1); }

typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t size;

  uint32_t banks_off;      /* MapHeaderBanks: 1 byte per map id (bank)        */
  uint32_t ptrs_off;       /* MapHeaderPointers: 1 dw per map id (address)    */
  uint32_t tilesets_off;   /* Tilesets: 12 B per tileset id                   */

  int ok;                  /* 1 iff all 3 tables above located + validated    */
} RomGbMap1;

/* One map's header, already resolved to file offsets -- §1.1/§1.2 of the
 * design (corrected shape, see this header's own top comment). */
#define ROM_GBMAP1_MAX_CONN 4   /* N, S, W, E -- at most one of each */

typedef struct {
  uint8_t  map_id;
  uint8_t  bank;            /* MapHeaderBanks[map_id]                        */
  uint32_t hdr_off;         /* file offset of the 10-byte partial header     */
  uint8_t  tileset_id;
  uint8_t  height, width;   /* in BLOCKS (4x4-tile units)                    */
  uint32_t blocks_off;      /* file offset of the Blocks array, height*width */
  uint8_t  conn_mask;       /* bit0 E, bit1 W, bit2 S, bit3 N (map_data_constants.asm);
                             * the RECORDS are still written N,S,W,E (source order) */
  uint8_t  nconn;           /* popcount(conn_mask), <= ROM_GBMAP1_MAX_CONN   */
  struct {
    uint8_t  map_id;
    uint8_t  width;         /* the CONNECTED map's own width, in blocks     */
    uint8_t  y, x;          /* seam tile offsets                            */
  } conn[ROM_GBMAP1_MAX_CONN];
  uint32_t obj_off;         /* file offset of the 2-byte Object pointer      */
} GbMap1Header;

/* One tileset entry, already resolved -- §1.3. */
typedef struct {
  uint8_t  gfx_bank;
  uint32_t block_off;   /* blockset: 16 B per block id (4x4 tile ids)        */
  uint32_t gfx_off;     /* tile graphics: 16 B per tile id, 2bpp planar, raw */
} GbMap1Tileset;

/* Locate the 3 tables above. `scratch`/`scratch_len` (>= ROM_GBMAP_SCRATCH_MIN)
 * is used only during this call (the full-ROM anchor scan) and may be reused
 * for anything else afterward. Returns g->ok; every g-> field is 0 on failure. */
bool rgm1_open(RomGbMap1* g, GbReadFn read, void* ctx, uint32_t size,
               uint8_t* scratch, uint32_t scratch_len);

/* Resolve `map_id`'s own header (§1.1/§1.2, corrected layout). false on any
 * bounds/structural failure (fails closed -- never returns a partially-filled
 * `out`). `g` must have g->ok. */
bool rgm1_header(const RomGbMap1* g, uint8_t map_id, GbMap1Header* out);

/* Resolve `tileset_id`'s own tileset header (§1.3). false on any bounds
 * failure. `g` must have g->ok. */
bool rgm1_tileset(const RomGbMap1* g, uint8_t tileset_id, GbMap1Tileset* out);

/* One block's 16 raw tile ids (4x4), block `block_id` of `ts`'s own blockset.
 * `out[16]` in row-major order (row0: 0..3, row1: 4..7, ...). */
bool rgm1_block(const RomGbMap1* g, const GbMap1Tileset* ts, uint8_t block_id,
                uint8_t out16[16]);

/* One tile's raw 2bpp planar bytes (16 B: 8 rows x 2 bytes/row, low+high
 * bitplane, MSB-first per row -- the standard GB tile format), tile `tile_id`
 * of `ts`'s own tile graphics. */
bool rgm1_tile2bpp(const RomGbMap1* g, const GbMap1Tileset* ts, uint8_t tile_id,
                    uint8_t out16[16]);

#endif /* ROM_GBMAP_H */
