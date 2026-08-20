#ifndef ROM_WALLPAPER_H
#define ROM_WALLPAPER_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"   /* RomCtx / RomReadFn / ROM_BASE */

/*
 * BOX WALLPAPERS, read out of the user's own retail ROM.
 *
 * Phase 4 of the ROM-art plan (docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 1.2 /
 * 4.4 / 6). Pure C (no tonc, no FatFs, no GBA headers): all I/O goes through the
 * RomCtx, so tests/host_romwallpaper_test.c runs this exact code against the real
 * cartridge dumps on the PC, exactly like rom_sprite.c / rom_itemart.c / rom_mon.c.
 * Decompression is map_render.c's streaming mr_lz77() -- do not write another.
 *
 * ============================================================================
 * SCOPE, STATED UP FRONT
 * ============================================================================
 *  - Only the 16 STANDARD wallpapers (sWallpapers[0..15], chooser ids 0..15) are
 *    served. The 16 Friends/Walda wallpapers (ids 16..31, Emerald only, need the
 *    save's two Walda colours substituted at blit time -- pdna_box.c's wp_sub_walda)
 *    are NOT served by this module. wp >= ROM_WP_COUNT is refused, same as an
 *    out-of-range id anywhere else in this file.
 *  - Only Emerald (BPEE rev 0), FireRed (BPRE rev 1) and LeafGreen (BPGE rev 0) are
 *    pinned -- the exact three dumps DESIGN.md Sec 1.2 measured. FireRed rev 0 and
 *    LeafGreen rev 1 were never measured and are deliberately absent from k_pins
 *    rather than guessed at: rom_wallpaper_open() fails closed (ok = 0) for them,
 *    same posture rom_sprite.c/rom_mon.c take for Ruby/Sapphire.
 *  - Ruby/Sapphire are not pinned at all. DESIGN.md Sec 1.2 locates their table
 *    addresses (AXVE 0x083BB104 / AXPE 0x083BB160) but they use a DIFFERENT 16-byte
 *    row struct ({tiles; u32 compressedSize; tilemap; palettes}, not this file's
 *    12-byte {tiles; tilemap; palettes}) that this module does not parse. Cost to
 *    close: a second row-reader function; not written here for lack of time in this
 *    pass. Reported, not silently absorbed.
 *
 * ============================================================================
 * WHERE THE DATA LIVES -- DESIGN.md Sec 1.2, byte-matched against the decomp and
 * cross-checked by a shape scan (docs/analysis-2026-08-19-rom-art/probe_*.py)
 * ============================================================================
 *   Emerald  BPEE r0   0x085775B8   sWallpapers, 16 rows x 12 B
 *   FireRed  BPRE r1   0x083D2A80   16 rows x 12 B
 *   LeafGreen BPGE r0  0x083D284C   16 rows x 12 B
 * Row = { const u32* tiles; const u32* tilemap; const u16* palettes }, all three
 * LZ77 (LZ10) except the tilemap, which is ALSO LZ77 but always decompresses to
 * EXACTLY 720 B (20x18 BG map entries) -- the self-check rom_wallpaper_open() runs
 * on row 0 before trusting a pin. Tiles decompress to 608-3,072 B (19-96 tiles of
 * raw 4bpp, DESIGN.md Sec 1.2) -- always comfortably inside ROM_WP_TILES_MAX_BYTES,
 * which is why this rung needs no new EWRAM (see rom_wallpaper.c's header note).
 *
 * ---- the composition question DESIGN.md Sec 1.2/5 raised, MEASURED AND CLOSED
 * ---- except for one specific, quantified piece
 * The repo's compiled wallpapers.c (git-ignored generated art, Emerald-only source
 * assets) is the byte-for-byte retail reference. Rendering this module's output
 * against it (docs/analysis-2026-08-20-artless-phases/probe_wpfix_verify.py,
 * re-runnable) settles the composition rule: a tilemap entry's raw 4-bit bank field
 * is 0, 1 or 2 across every wallpaper on all three pinned games; the row's own
 * palette blob holds exactly ROM_WP_PAL_BANKS (2) banks (64 B, NOT 128 -- see the
 * ROM_WP_PAL_BYTES history below); raw bank 0 or 1 both read the row's FIRST bank,
 * raw bank 2 reads its SECOND (rom_wallpaper_pal_bank() implements exactly this).
 * Measured on Emerald (the only pinned game wallpapers.c was built from -- FireRed/
 * LeafGreen use their OWN, never-byte-matched art per DESIGN.md Sec 1.2's own note,
 * so comparing them against this Emerald reference is not a correctness check):
 * every pixel whose 4bpp nibble is NONZERO (i.e. not palette index 0) reproduces
 * wallpapers.c EXACTLY, 100.00%, across all 16 standard wallpapers.
 *   The ONE genuinely open piece: pixels at palette index 0 (the tile nibble that
 * is 0). Retail's LoadWallpaperGfx composites a wallpaper as TWO layers -- a tiled
 * scenery pattern (bg.png in the decomp) underneath, with the tilemap layer drawn
 * over it where index 0 is transparent and the pattern shows through. This module
 * draws ONE Mode-3 layer with no compositing, so it expands index 0 LITERALLY
 * (whatever RGB15 the row's palette bank stores there), not the true per-position
 * backdrop pattern. Measured: 272 of 35,585 index-0 pixels (0.76%) happen to
 * coincide anyway (an all-fill wallpaper stretch); the remaining ~9.6% of a
 * wallpaper's total pixels are index-0 and differ from retail's tiled backdrop by
 * this design. This includes the box's 12 corner cells (tilemap bank 0 on every
 * wallpaper on every pinned game, always tid 0 -- the tiles blob's all-zero "blank"
 * tile): retail draws these fully transparent over the same tiled backdrop, so ANY
 * literal color choice there is equally approximate; this module reads them through
 * rom_wallpaper_pal_bank(0) = the row's own first bank, the same literal-index-0
 * policy applied everywhere else in this module, rather than inventing a special
 * case or a synthetic fill. Closing this exactly would mean finding and compositing
 * the per-wallpaper backdrop pattern (DESIGN.md speculated about a second blob; a
 * repeated search in this pass still did not find one in the Emerald image) --
 * genuinely open, budgeted separately, and NOT required for this module's own
 * contract (it documents literal, no-substitution expansion as the design, not a
 * bug). Nothing else about this module's composition is open.
 */

#define ROM_WP_COUNT           16   /* standard wallpapers only; see SCOPE above  */
#define ROM_WP_MAP_ENTRIES     360  /* 20 x 18 BG map cells                       */
#define ROM_WP_MAP_BYTES       (ROM_WP_MAP_ENTRIES * 2u)   /* 720                 */
#define ROM_WP_TILES_MAX_BYTES 4096 /* >= measured worst case (3,072 B); refuses  *
                                      * rather than truncates a longer decode      */
#define ROM_WP_PAL_BANKS       2   /* MEASURED: the row's palette blob is exactly *
                                     * 64 B (2 banks) -- on Emerald wp 0 it is    *
                                     * immediately followed by the tiles blob's   *
                                     * LZ77 header (see rom_wallpaper.c). The old *
                                     * 4-bank/128 B guess read past it into the   *
                                     * next blob's compressed stream bytes.       */
#define ROM_WP_PAL_BYTES       (ROM_WP_PAL_BANKS * 32u)    /* 64                  */

typedef struct RomWallpaper {
  const RomCtx* rc;
  uint32_t table;   /* FILE offset of the 16-row standard wallpaper table */
  int      ok;      /* 1 = a pin matched AND row 0's tilemap self-checked to 720 B */
} RomWallpaper;

/* Look up this ROM's pin and self-check row 0 (tilemap decompresses to exactly
 * ROM_WP_MAP_BYTES). Returns 1 on success; 0 (fail closed) for any game/revision
 * not in k_pins, or a pin whose self-check disagrees (a stale/wrong address). */
int rom_wallpaper_open(RomWallpaper* rw, const RomCtx* rc);

/* Decompress wallpaper `wp`'s (0..ROM_WP_COUNT-1) 20x18 tilemap into dst, RAW GBA BG
 * map entries (tid = e & 0x3FF, hflip = (e>>10)&1, vflip = (e>>11)&1,
 * bank = (e>>12)&0xF) -- NOT pdna_box.c's plain-dedup-index format, which this
 * module never produces (see rom_wallpaper.c's note on why no dedup happens here).
 * dst must hold ROM_WP_MAP_ENTRIES uint16_t (720 B); the caller reads it back with
 * its own array indexing, never a cast -- writes here are plain byte stores, safe
 * into any 2-aligned destination (map_render.c's mr_lz77 already writes this way).
 * Returns 1 on success, 0 on a bad wp/rw or a decompress that did not land on
 * exactly ROM_WP_MAP_BYTES. */
int rom_wallpaper_map(const RomWallpaper* rw, int wp, uint16_t dst[ROM_WP_MAP_ENTRIES]);

/* Decompress wallpaper `wp`'s raw 4bpp tile array into dst (dst_cap must be >=
 * ROM_WP_TILES_MAX_BYTES -- a shorter buffer, or a row whose declared size exceeds
 * it, is REFUSED rather than truncated). *out_bytes is the exact decompressed
 * length (a multiple of 32; tile count = *out_bytes / 32). Returns 1 on success. */
int rom_wallpaper_tiles(const RomWallpaper* rw, int wp, uint8_t* dst, uint32_t dst_cap,
                        uint32_t* out_bytes);

/* wallpaper `wp`'s palette banks, up to ROM_WP_PAL_BANKS x 16 RGB15 entries, raw
 * (bit 15 clear, no substitution). A bank a tilemap entry never references may read
 * as zeroes if the row's palette blob is shorter than ROM_WP_PAL_BYTES -- callers
 * must not trust a bank past what the row's own tilemap uses. Returns 1 on success. */
int rom_wallpaper_pal(const RomWallpaper* rw, int wp, uint16_t dst[ROM_WP_PAL_BANKS][16]);

/* Map a tilemap entry's raw 4-bit bank field (e>>12 & 0xF) to an index into the
 * ROM_WP_PAL_BANKS-bank array rom_wallpaper_pal() fills -- the SINGLE place this
 * mapping is implemented, so pdna_box.c's draw path and the host test can never
 * drift apart. Measured (this header's top-of-file note, probe_wpfix_verify.py):
 * raw bank 0 or 1 both read the row's bank 0; raw bank 2 reads bank 1. Anything
 * else (never observed on a pinned game) clamps to the last real bank rather than
 * reading out of the dst[ROM_WP_PAL_BANKS][16] array. */
int rom_wallpaper_pal_bank(int bank);

/* Expand ONE 8x8 4bpp tile (tid's 32 raw bytes inside `tiles`/`tiles_bytes`, as
 * rom_wallpaper_tiles produced) to 64 RGB15 pixels (bit 15 clear -- the caller masks
 * before a Mode-3 store, matching pdna_box.c's existing convention), applying
 * hflip/vflip and looking up `pal` LITERALLY (index 0 included -- see this header's
 * top-of-file note on the composition question). Returns 0 (out unchanged) if tid's
 * bytes fall outside tiles_bytes -- a corrupt/out-of-range tid is a caller bug or a
 * read failure, and must never read past the buffer. */
int rom_wallpaper_expand_tile(const uint8_t* tiles, uint32_t tiles_bytes, uint16_t tid,
                              int hflip, int vflip, const uint16_t pal[16],
                              uint16_t out[64]);

#endif /* ROM_WALLPAPER_H */
