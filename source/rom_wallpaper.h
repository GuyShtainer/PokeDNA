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
 * ---- the composition question DESIGN.md Sec 1.2/5 left OPEN, and what this module
 * ---- does about it
 * The repo's compiled wallpapers.c (git-ignored generated art) was independently
 * built by tools/gen_wallpaper.py from decomp PNG source assets, and DESIGN.md's
 * own decisive experiment (probe_wpexact.py, re-run in this pass -- see the phase
 * report) could NOT reproduce it byte-for-byte out of the ROM under any of the four
 * plausible palette-substitution rules it tried, nor under a literal "no
 * substitution" rule tried in this pass either. The missing background-pattern blob
 * DESIGN.md speculated about was searched for again in this pass and still was not
 * found anywhere in the Emerald image. This module therefore takes the ROM's own
 * gWallpaperTiles/gWallpaperTilemap as ALREADY the complete, final picture -- which
 * is how LoadWallpaperGfx actually uses them at retail (one decompress straight to
 * BG VRAM, no runtime compositing) -- and expands every tile INCLUDING palette
 * index 0 literally, through whichever of the row's palette banks the tilemap entry
 * names, with no synthetic substitution. This is a real, load-bearing design
 * decision, not a shortcut: it means a ROM-sourced wallpaper may not be pixel-
 * identical to the currently-shipped baked wallpapers.c art (that mismatch is
 * DESIGN.md's own still-open research question, budgeted separately at "one
 * afternoon" and NOT closed by this pass), but it is exactly what the cartridge
 * itself would show on its own PC screen.
 */

#define ROM_WP_COUNT           16   /* standard wallpapers only; see SCOPE above  */
#define ROM_WP_MAP_ENTRIES     360  /* 20 x 18 BG map cells                       */
#define ROM_WP_MAP_BYTES       (ROM_WP_MAP_ENTRIES * 2u)   /* 720                 */
#define ROM_WP_TILES_MAX_BYTES 4096 /* >= measured worst case (3,072 B); refuses  *
                                      * rather than truncates a longer decode      */
#define ROM_WP_PAL_BANKS       4
#define ROM_WP_PAL_BYTES       (ROM_WP_PAL_BANKS * 32u)    /* 128                 */

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
