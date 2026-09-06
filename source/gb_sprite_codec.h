#ifndef GB_SPRITE_CODEC_H
#define GB_SPRITE_CODEC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Game Boy / Game Boy Color Pokemon picture codecs -- the two compressions that
 * stand between a Gen-1 or Gen-2 cartridge and the sprite it draws.
 *
 * WHY THIS EXISTS: a Pokemon is shown in the art of the generation it CAME FROM.
 * A Gen-1 Mew must render as the Red/Blue Mew, a Gen-2 Unown as the Gold/Crystal
 * one. That is provenance you can see while deciding what to transfer, and it can
 * only come from the user's own Red.gb / Yellow.gb / Gold.gbc / Crystal.gbc at
 * runtime. PokeDNA ships no Nintendo art; this file reads the cartridge.
 *
 * ---- the two formats, and why they are NOT LZ77 -----------------------------
 *
 * GEN 1 ("pic compression", pokered/home/uncompress.asm). Nintendo's own
 * bit-plane coder. A 2bpp picture is split into two 1bpp planes; each plane is
 * emitted as a stream of 2-bit groups that alternates between literal groups and
 * run-length-encoded zero runs, and each plane is then reconstructed with a
 * differential (Gray-code) pass. A 1-or-2 bit MODE selects how the second plane
 * relates to the first: mode 0 = both planes decoded independently, mode 1 = the
 * second plane is the XOR of the first with the (un-differenced) second, mode 2 =
 * both are differenced and then XORed. The compressor tries all three plus both
 * plane orders and keeps the smallest, so every mode occurs in a real ROM.
 * The first byte is the geometry: high nybble width in tiles, low nybble height.
 *
 * GEN 2 ("lz3", pokecrystal/home/decompress.asm). A completely different, general
 * purpose byte-oriented LZ: a command byte carries a 3-bit opcode and a 5-bit
 * length (literal / iterate / alternate / zero / repeat / bit-flipped repeat /
 * reversed repeat), with a long form that widens the length to 10 bits. Repeats
 * take either a 15-bit offset from the START of the output or a 7-bit offset back
 * from the current position. The stream ends at a 0xFF command byte. It carries no
 * geometry at all -- the caller must know it, from the base-stats PicSize byte
 * (fronts, always square) or from the fixed 6x6 of every Gen-2 back pic.
 *
 * ---- what "the first frame" means on Gen 2 ----------------------------------
 * A Gen-2 FRONT pic decompresses to an ANIMATION tile bank, not a picture: pret's
 * pokemon_animation_graphics copies frame 0 whole and then appends only the tiles
 * later frames need (pokecrystal/tools/pokemon_animation_graphics.c "Copy the
 * first frame directly"). The static picture is exactly the first wt*ht tiles, so
 * this decoder stops as soon as it has them. That is safe: lz3 output is written
 * strictly forwards and every back-reference points at bytes already written, so a
 * prefix decode is bit-identical to a full one. It is also the reason gb_sprite's
 * `consumed` is NOT the compressed blob's length on Gen 2 (it is on Gen 1).
 *
 * ---- colour is the caller's business ----------------------------------------
 * Output is a plain index 0..3 per pixel, 0 = lightest. Gen 1 has no per-species
 * palette at all (the DMG grey ramp, or a Super Game Boy colour set); Gen 2 stores
 * two colours per species in a separate palette table and the real ramp is
 * { white, colour1, colour2, black }. Nothing here bakes a colour in.
 *
 * ---- host-testability + the OS-mode rule ------------------------------------
 * Pure C: no tonc, no FatFs, no GBA headers, no mutable globals, no allocation.
 * All input arrives through a caller-supplied GbReadFn, so tests/host_gbcodec_test.c
 * runs this exact code against Guy's real cartridge dumps on the PC.
 *
 * On hardware that callback is the one that touches the SD card, so IT is what has
 * to obey the OS-mode rule (docs/kb/, CLAUDE.md rule 1): during an EZ-Flash SD
 * transfer the game ROM is unmapped, so the callback must be EWRAM_CODE with IRQs
 * off. The decoder between callbacks is ordinary ROM-resident code and needs no
 * special placement. Input is pulled through a 64-byte window, so a 7x7 pic costs
 * about a dozen short reads rather than one big staging buffer.
 *
 * ---- memory ------------------------------------------------------------------
 * ONE caller-owned GbSprite (3 928 B) holds the output, the scratch and nothing
 * else; this file declares no statics beyond two 16-byte const tables. The
 * hardware build has ~1.5 KB of free EWRAM, so the caller must hand in existing
 * scratch -- mon_decomp (8192 B, source/mon_front.c) fits it with room to spare,
 * or app_arena_acquire(). Stack use is ~120 B.
 */

/* Both formats top out at 7x7 tiles (56x56 px): Gen 1's sprite buffers are
 * PIC_WIDTH*PIC_HEIGHT*TILE_1BPP_SIZE (pokered/constants/gfx_constants.asm), and
 * Gen 2's front pics are 5x5, 6x6 or 7x7 with every back pic 6x6. */
#define GB_SPRITE_MAX_TILES 7
#define GB_SPRITE_MAX_W     (GB_SPRITE_MAX_TILES * 8)
#define GB_SPRITE_MAX_H     (GB_SPRITE_MAX_TILES * 8)
#define GB_SPRITE_MAX_PX    (GB_SPRITE_MAX_W * GB_SPRITE_MAX_H)   /* 3136 */

/* Scratch: two 1bpp planes on Gen 1, one 2bpp frame on Gen 2 -- same size. */
#define GB_SPRITE_WORK      (GB_SPRITE_MAX_TILES * GB_SPRITE_MAX_TILES * 16) /* 784 */

/* Hard ceiling on input bytes read for one picture. The largest real pic measured
 * over all four of Guy's cartridges is 637 B (Gen 1, Slowbro's front in Red) and
 * 702 B (Gen 2, to the end of Crystal's biggest static frame), so this is ~3x
 * headroom -- and it is what stops a corrupt stream reading the whole card. */
#define GB_SPRITE_MAX_INPUT 2048

/* Read `len` bytes at FILE offset `off`. Returns false on any short/failed read.
 * Same shape as rom_map.h's RomReadFn so one FatFs adapter can serve both. */
typedef bool (*GbReadFn)(void *ctx, uint32_t off, void *dst, uint32_t len);

typedef enum {
  GB_SPRITE_OK = 0,
  GB_SPRITE_E_ARGS,    /* NULL argument, or geometry outside 1..7 tiles         */
  GB_SPRITE_E_READ,    /* the callback failed, or the input budget ran out      */
  GB_SPRITE_E_HEADER,  /* Gen 1: the geometry byte is not a 1..7 by 1..7 pic    */
  GB_SPRITE_E_DATA     /* malformed stream (impossible run length, bad offset)  */
} GbSpriteErr;

typedef struct {
  uint8_t  wt, ht;             /* size in 8x8 tiles, 1..7                        */
  uint8_t  w, h;               /* size in pixels = wt*8, ht*8                    */
  uint32_t consumed;           /* input bytes read (Gen 1: the blob's exact size) */
  /* Indexed bitmap, PACKED row-major with stride `w` (not GB_SPRITE_MAX_W).
   * px[y*w + x] is 0..3, 0 = lightest. Bytes past w*h are never written. */
  uint8_t  px[GB_SPRITE_MAX_PX];
  uint8_t  work[GB_SPRITE_WORK];   /* private scratch; contents undefined         */
} GbSprite;

/* Just the metadata half of GbSprite, for the _buf entry points below -- a caller
 * supplying its own px/work buffers (gb_art_source.c: both live inside the shared
 * mon_decomp, not a 3,928 B GbSprite on the stack -- see rom_gbsprite.h's "does NOT
 * work in place" note, now qualified by rom_gbsprite_to_rgb15_inplace()) still needs
 * somewhere to receive wt/ht/w/h/consumed. */
typedef struct {
  uint8_t  wt, ht, w, h;
  uint32_t consumed;
} GbSpriteInfo;

/* Gen 1 (Red/Blue/Yellow). The geometry comes from the stream's own first byte,
 * so `off` is all you need -- it is the address the base-stats entry points at
 * (offset 0x0B front / 0x0D back, in the bank home/pics.asm picks by index). */
GbSpriteErr gb_sprite_gen1(GbSprite *out, GbReadFn rd, void *ctx, uint32_t off);

/* Same decode, into caller-owned `px` (>= GB_SPRITE_MAX_PX bytes) and `work`
 * (>= GB_SPRITE_WORK bytes) instead of a GbSprite's embedded arrays -- the two
 * buffers need not be adjacent, need not come from a GbSprite at all, and may
 * overlap a THIRD buffer the caller plans to fill afterward (mon_decomp: this is
 * what makes rom_gbsprite_to_rgb15_inplace() possible). Same errors/semantics as
 * gb_sprite_gen1(), which is now a thin wrapper over this. */
GbSpriteErr gb_sprite_gen1_buf(uint8_t *px, uint8_t *work, GbReadFn rd, void *ctx,
                               uint32_t off, GbSpriteInfo *info);

/* Gen 2 (Gold/Silver/Crystal). `wt`/`ht` must come from the ROM: a front pic is
 * square with the side in either nybble of the base-stats PicSize byte (+0x11 of
 * the 32-byte entry), a back pic is always 6x6. Decodes the first wt*ht tiles --
 * for a front pic that is animation frame 0, i.e. the static picture. */
GbSpriteErr gb_sprite_gen2(GbSprite *out, GbReadFn rd, void *ctx, uint32_t off,
                           unsigned wt, unsigned ht);

/* Same decode as gb_sprite_gen2(), into caller-owned px/work -- see
 * gb_sprite_gen1_buf()'s comment, identical rationale. gb_sprite_gen2() is now a
 * thin wrapper over this. */
GbSpriteErr gb_sprite_gen2_buf(uint8_t *px, uint8_t *work, GbReadFn rd, void *ctx,
                               uint32_t off, unsigned wt, unsigned ht,
                               GbSpriteInfo *info);

const char *gb_sprite_err(GbSpriteErr e);

#endif /* GB_SPRITE_CODEC_H */
