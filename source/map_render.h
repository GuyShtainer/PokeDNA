#ifndef MAP_RENDER_H
#define MAP_RENDER_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"

/*
 * Gen-3 overworld renderer core — metatiles to pixels, and metatiles to Mode-0
 * tilemap entries. Pure C (no tonc, no GBA headers, no BIOS): the exact same code is
 * exercised by tests/host_render_test.c on a PC, byte-compared against an independent
 * Python reference renderer (tools/gen_render_truth.py) run over a real retail ROM.
 * Two independent implementations agreeing on real ROM bytes is the cheapest way to be
 * sure about a pixel pipeline before it reaches a GBA screen.
 *
 * ---- why the on-GBA path is Mode 0, and why that decides this API ------------
 * MEASURED across every map in Emerald and FireRed: the worst map needs
 * 49-51 KB of decompressed tiles + metatile tables, and up to 13.6 KB of blockdata.
 * The map screen's borrowed EWRAM arena is 35,712 bytes, so **caching a map's tiles in
 * EWRAM is impossible**. In Mode 0 the 32 KB of tile data lives in VRAM instead,
 * leaving EWRAM to hold only the metatile tables (<=16.4 KB) plus the blockdata
 * (<=13.6 KB) ~= 30 KB, which fits — and BG scrolling becomes free hardware work
 * rather than 38,400 pixel writes per step.
 *
 * So this core does three separable jobs:
 *   1. LZ77-decompress tile data to a caller buffer (which on the GBA is VRAM).
 *   2. Turn a metatile id into its 8 Mode-0 tilemap entries (the 1:1 view).
 *   3. Composite a metatile into RGB555 pixels — used for the zoomed-out mip levels
 *      (where there is no hardware equivalent) and for host verification.
 */

/* Colour index 0 is transparent on EVERY layer, bottom included — what shows through is
 * the backdrop (MR_BACKDROP). */
#define MR_METATILE_PX 16              /* a metatile is 16x16 pixels */
#define MR_METATILE_PIXELS (MR_METATILE_PX * MR_METATILE_PX)

/* What shows through where every layer is transparent. The game overwrites BG palette
 * entry 0 with RGB_BLACK in LoadTilesetPalette and never displays the tileset's own
 * colour 0, so black is what the hardware actually shows. */
#define MR_BACKDROP 0x0000

typedef struct {
  const RomCtx* rom;
  RomLayout     lay;

  /* Decompressed 4bpp char data. On the GBA these point into VRAM; on the host into
   * malloc'd/static buffers. Lengths are what actually decompressed, which is often
   * far less than the 512-tile maximum (Littleroot's secondary set is 159 tiles). */
  const uint8_t* prim_tiles; uint32_t prim_tiles_len;
  const uint8_t* sec_tiles;  uint32_t sec_tiles_len;

  /* 16 palettes x 16 BGR555 colours per tileset, copied out of ROM. */
  uint16_t prim_pal[16][16];
  uint16_t sec_pal[16][16];

  /* Metatile tables. Kept as ROM addresses plus a caller-provided RAM cache, because
   * reading 16 bytes per metatile straight off the SD per draw would be far too slow. */
  uint32_t prim_mt_addr, sec_mt_addr;
  const uint8_t* prim_mt; uint32_t prim_mt_len;   /* cached copies (may be NULL) */
  const uint8_t* sec_mt;  uint32_t sec_mt_len;

  /* Per-game split points (Emerald/Ruby 512/6, FireRed/LeafGreen 640/7). */
  uint16_t split_mt, split_tile;
  uint8_t  split_pal;
} MapRender;

/* GBA BIOS-compatible LZ77 (LZ10) decoder, portable and bounds-checked.
 * Reads the 4-byte header at ROM address `addr` (type byte must be 0x10), then
 * decompresses into dst, never writing more than dst_cap. Returns the number of bytes
 * produced, or 0 on a malformed stream / overrun. Deliberately our own rather than
 * SWI 0x11/0x12 so this core stays host-testable — the GBA may use the BIOS call
 * separately when the destination is VRAM. */
uint32_t mr_lz77(const RomCtx* rom, uint32_t addr, uint8_t* dst, uint32_t dst_cap);

/* Same decoder, but pulls its compressed INPUT through a caller-supplied window
 * (`win`/`win_bytes`) instead of a fixed 64 B stack buffer -- for a caller that owns
 * spare space at the tail of `dst` past what this decode needs (BACKLOG #103), so a
 * multi-KB blob costs O(1) read-callback calls instead of O(span/64). Pass
 * win = 0, win_bytes = 0 for the original fixed-64-B-window behaviour; mr_lz77()
 * is exactly that call. The window is filled at most `min(win_bytes, remaining
 * compressed span, remaining ROM bytes)` at a time, so it can never read past the
 * LZ10 all-literal upper bound on the compressed span nor past end of ROM (this
 * also fixes a latent bug in the old fixed-64-B fill: an unconditional 64 B read
 * fails `rom_read_at`'s bounds check for any blob within 63 B of EOF, even though
 * the actual compressed span needs fewer bytes). Returns 0 (decode fails) if the
 * computed fill would be zero-length. */
uint32_t mr_lz77_w(const RomCtx* rom, uint32_t addr, uint8_t* dst, uint32_t dst_cap,
                   uint8_t* win, uint32_t win_bytes);

/* BACKLOG #295: decode only the output bytes [start, start+len) of an LZ10 blob into dst,
 * keeping the last 4,096 B in `ring` (ring_bytes: a power of two >= 4096) instead of the whole
 * output -- for a blob bigger than any buffer we own. Stops as soon as the range is full.
 * Returns len, or 0 on any bad argument / malformed stream / range outside the declared size.
 * Every byte returned is byte-identical to mr_lz77()'s output at the same offsets. */
uint32_t mr_lz77_range(const RomCtx* rom, uint32_t addr, uint32_t start, uint32_t len,
                       uint8_t* dst, uint8_t* ring, uint32_t ring_bytes);

/* Same decoder + window as mr_lz77_w(), but ALSO reports the exact compressed
 * span this decode consumed (`*consumed`, header included) and an FNV-1a hash
 * of exactly those consumed bytes (`*in_hash`) -- BACKLOG #103 step 4's cheap
 * verify: a caller re-reads exactly `*consumed` raw bytes with mr_hash_span()
 * below and compares hashes, instead of running the CPU-heavy decode a second
 * time to "verify" it. `consumed`/`in_hash` may each be NULL if not wanted. */
uint32_t mr_lz77_x(const RomCtx* rom, uint32_t addr, uint8_t* dst, uint32_t dst_cap,
                   uint8_t* win, uint32_t win_bytes,
                   uint32_t* consumed, uint32_t* in_hash);

/* FNV-1a hash of `len` raw ROM bytes starting at `addr`, read through the same
 * window (`win`/`win_bytes`, or 0,0 for the default 64 B stack window) a prior
 * mr_lz77_x() call used -- the ONE shared re-read+hash helper every verify
 * path folds through, so there is exactly one hashing implementation to trust.
 * Returns false (leaving *out_hash unset) if any underlying read fails. */
bool mr_hash_span(const RomCtx* rom, uint32_t addr, uint32_t len,
                  uint8_t* win, uint32_t win_bytes, uint32_t* out_hash);

/* Decompressed size from an LZ77 header without decompressing. 0 if not LZ77. */
uint32_t mr_lz77_size(const RomCtx* rom, uint32_t addr);

/* Prepare a renderer for one map layout. Copies both tilesets' palettes, records the
 * metatile table addresses, and fills in the per-game split points. Does NOT fetch
 * tile data or metatile tables — the caller owns those buffers and passes them in via
 * mr_set_tiles / mr_set_metatiles, because on the GBA the tiles go to VRAM. */
bool mr_init(MapRender* mr, const RomCtx* rom, const RomLayout* lay);

void mr_set_tiles(MapRender* mr, const uint8_t* prim, uint32_t prim_len,
                                 const uint8_t* sec,  uint32_t sec_len);
void mr_set_metatiles(MapRender* mr, const uint8_t* prim, uint32_t prim_len,
                                     const uint8_t* sec,  uint32_t sec_len);

/* Byte size of each tileset's metatile table, derived as (attributes - metatiles)
 * since the attribute array follows the metatile array contiguously in ROM.
 * 0 if it cannot be determined. */
uint32_t mr_metatile_table_bytes(const RomCtx* rom, uint32_t tileset_addr);

/* The 8 raw tilemap entries for a metatile id (bottom layer TL,TR,BL,BR then top
 * layer TL,TR,BL,BR). Uses the cached tables when present, else reads ROM. */
bool mr_metatile_entries(const MapRender* mr, uint16_t mid, uint16_t out[8]);

/* Composite one metatile into a 16x16 block of BGR555 pixels.
 * `dst` is indexed dst[y * stride + x]. The block is pre-filled with MR_BACKDROP and
 * BOTH layers treat colour index 0 as transparent, which is what the hardware does.
 * This is the function the host test byte-compares. */
bool mr_metatile_pixels(const MapRender* mr, uint16_t mid, uint16_t* dst, int stride);

/* Composite a w x h rectangle of MAP CELLS (metatiles) starting at cell (x0,y0) into
 * dst, which must be at least (w*16) x (h*16). `stride` is in pixels. Cells outside
 * the map are skipped (left untouched) rather than clamped. */
bool mr_region(const MapRender* mr, int x0, int y0, int w, int h,
               uint16_t* dst, int stride);

/* ---- zoom mips -------------------------------------------------------------
 * There is no hardware downscale for text BGs, so the zoomed-out levels are built by
 * reducing each metatile ONCE into a small tile and letting the same tilemap hardware
 * scroll it. Z1 = 8x8 px per metatile (half scale), Z2 = 4x4 (quarter).
 * Point-sampling, not averaging: the 13 tileset palettes total 208 distinct colours,
 * so a reduced image needs no requantisation. */
bool mr_metatile_mip(const MapRender* mr, uint16_t mid, int px, uint16_t* dst, int stride);

/* ---- 8bpp INDEX compositing (for the affine zoom BG) -----------------------
 * An affine background is 8bpp and its tilemap entries are ONE byte, so its tiles hold
 * palette indices into the single 256-entry BG palette — not colours. Our merged palette
 * is a pure identity mapping, `byte = (tileset_palette_slot << 4) | colour_index`, because
 * PALRAM is literally the tilesets' 16x16 palette arrays copied verbatim. So compositing
 * to indices needs no search and no quantisation.
 *
 * Index 0 is the transparent/backdrop entry, matching MR_BACKDROP. */
bool mr_metatile_idx8(const MapRender* mr, uint16_t mid, uint8_t* dst, int stride);

/* Point-sampled reduction of the above to px*px bytes (px must divide 16).
 * This is what builds the zoom-mip tiles that get DMA'd into the affine charblock. */
bool mr_metatile_mip_idx(const MapRender* mr, uint16_t mid, int px, uint8_t* dst, int stride);

#endif /* MAP_RENDER_H */
