#ifndef ROM_HAND_H
#define ROM_HAND_H

#include <stdint.h>
#include "rom_map.h"   /* RomCtx / RomReadFn / ROM_BASE / rom_read_at */

/*
 * The Gen-3 PC-storage POINTER GLOVE ("the hand"), read out of the user's own ROM.
 *
 * Phase 3 of the ROM-gated build (docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 1.3 /
 * 4.5) -- the cheapest of Guy's nine art items: a raw (UNCOMPRESSED) 2,048 B sheet,
 * four 32x32 4bpp poses stacked vertically, no decompression at all. Pure C (no tonc,
 * no FatFs): all I/O goes through the RomCtx's RomReadFn, so tests/host_romhand_test.c
 * runs this exact code against the real cartridge dumps on the PC.
 *
 * ============================================================================
 * WHERE THE DATA LIVES -- measured on Guy's own five dumps, not assumed
 * ============================================================================
 * NOT reachable from GFRomHeader (checked field by field, same as rom_itemart.h's
 * icon/badge tables) -- pinned per revision, exactly like rom_itemart.c's k_pins.
 * DESIGN.md Sec 1.3 located the sheet + Emerald/FireRed palettes by a shape scan and
 * flagged LeafGreen's palette "(not probed; expect the same relative site)". Both
 * gaps are now closed, VERIFIED against the actual dumps in gba-toolkit/roms/, not
 * guessed:
 *
 *   - the 2,048 B sheet is BYTE-IDENTICAL across Emerald, FireRed and LeafGreen (one
 *     PNG, the same glove, shipped three times at three different addresses) --
 *     confirmed with a direct memcmp, not inferred from "same relative site";
 *   - LeafGreen's palette was independently LOCATED by searching its image for the
 *     32 raw bytes FireRed's palette resolves to (identical 16 colours, since it is
 *     the same glove art): exactly one hit, at ROM+0x08E9C478 -- +0x80 from
 *     FireRed's 0x08E9C3F8, a real, found address, not a guessed offset.
 *
 *     Game       sheet (RAW, 2048 B)   palette (RAW, 32 B)
 *     Emerald    0x0857B118            0x085723DC
 *     FireRed    0x083D2C5C            0x08E9C3F8
 *     LeafGreen  0x083D2A28            0x08E9C478
 *
 * Ruby/Sapphire ship a DIFFERENT glove (pokeruby's own hand_cursor.png -- DESIGN.md's
 * own honest gap): searching both dumps for Emerald's exact 2,048 B sheet gets ZERO
 * hits, confirmed, not assumed. Not pinned; rom_hand_open() fails closed there, the
 * same "an address nobody verified is worse than no feature" posture rom_itemart.c
 * and rom_map.c already apply to their own unpinned revisions.
 *
 * ---- pose order -------------------------------------------------------------
 * The sheet is exactly 32 px wide, so its natural row-major tile order IS the GBA 1D
 * OAM order for a 32x32 sprite -- pose f is simply bytes f*512..f*512+511, no
 * rearrangement. The four poses match tools/gen_hand.py's WANT table and hand_oam.h's
 * four committed arrays exactly:
 *
 *     frame 0  open hand / rest     ->  hand_oam_cursor_tiles  (ROM_HAND_FRAME_CURSOR)
 *     frame 1  idle bounce          ->  hand_oam_bounce_tiles  (ROM_HAND_FRAME_BOUNCE)
 *     frame 2  wide reach (grab dip)->  hand_oam_reach_tiles   (ROM_HAND_FRAME_REACH)
 *     frame 3  closed fist / grab   ->  hand_oam_grab_tiles    (ROM_HAND_FRAME_GRAB)
 *
 * ============================================================================
 * THE SILENT-GARBAGE HAZARD
 * ============================================================================
 * EZ-Flash `Read_SD_sectors` can return SUCCESS while holding garbage. Every fetch is
 * read up to three times and accepted only when two consecutive reads agree on a
 * 32-bit FNV-1a hash of the bytes -- byte-for-byte the scheme rom_itemart.c's
 * read_verified() uses. Verification is ON by default; a fused / cartridge-bus source
 * needs none of it: rom_hand_set_verify(rh, 0).
 *
 * ============================================================================
 * MEMORY
 * ============================================================================
 * This file defines ZERO mutable statics (only a `const` pin table in .rodata).
 * Every call a SCREEN makes is caller-buffered: rom_hand_frame() writes into the
 * caller's 512 B (box_oam.c's existing s_stage, the same buffer the icon-streaming
 * rung already stages into) and rom_hand_pal() into the caller's 16 B array
 * (boxoam_enter()'s own 32 B stack local) -- neither has a stack buffer of its own
 * (8 B / 40 B frames, MEASURED with -fstack-usage).
 *
 * rom_hand_open() is the one exception, and it is a ONE-SHOT lifecycle call (once
 * per ROM registration, from app_icon_rom_open() -- never a render loop), not a
 * per-frame path: its content-signature probe stages a full 512 B pose plus a 32 B
 * palette on the stack to validate them before trusting the pins, the SAME shape and
 * SAME justification rom_itemart.h documents for rom_itemart_open() (544 B, "its
 * probe decodes one icon"). MEASURED: validate() is 560 B (thumb/-O2 target build),
 * well inside the ~12 KiB IWRAM stack -- and, like rom_itemart_open(), it is never
 * the thing that overflows it.
 */

#define ROM_HAND_TILES        16    /* 32x32 sprite = 16 4bpp tiles          */
#define ROM_HAND_TILE_BYTES   32
#define ROM_HAND_FRAME_BYTES  (ROM_HAND_TILES * ROM_HAND_TILE_BYTES)   /* 512 */
#define ROM_HAND_FRAMES       4

enum {
  ROM_HAND_FRAME_CURSOR = 0,   /* open hand, resting            */
  ROM_HAND_FRAME_BOUNCE = 1,   /* idle-bounce second pose       */
  ROM_HAND_FRAME_REACH  = 2,   /* wide reach (grab dip)         */
  ROM_HAND_FRAME_GRAB   = 3,   /* closed fist / carrying        */
};

typedef struct RomHand {
  const RomCtx* rc;
  uint32_t sheet;    /* ROM address of the 2,048 B raw 4bpp sheet; 0 = none */
  uint32_t pal;      /* ROM address of the 32 B raw palette; 0 = none      */
  int      ok;       /* 1 = pinned revision + content validated            */
  int      verify;   /* 1 = fetch-twice-and-compare (default 1)            */
} RomHand;

/* Identify this ROM's revision against the pin table and validate the sheet's shape
 * (all four 32x32 corners transparent, real art near the centre) and the palette
 * (bit 15 clear on all 16 entries). Returns 1 on success; 0 (fail closed) for an
 * unpinned revision, a relocated ROM hack, a truncated dump, or content that fails
 * the shape/palette check. */
int rom_hand_open(RomHand* rh, const RomCtx* rc);

/* Turn the fetch-twice-and-compare verification off (fused / cartridge-bus source)
 * or back on. Default is ON. */
void rom_hand_set_verify(RomHand* rh, int on);

/* 1 if this ROM can serve the glove at all. */
int rom_hand_have(const RomHand* rh);

/* Read one 512 B pose (raw, no decompression) into dst, verbatim GBA 1D 4bpp tile
 * order -- a direct DMA source once verified, no rearrangement. Returns 1, or 0 (dst
 * undefined) for an unopened/unpinned ROM, an out-of-range frame, or a fetch that
 * failed verification. */
int rom_hand_frame(const RomHand* rh, uint8_t frame, uint8_t dst[ROM_HAND_FRAME_BYTES]);

/* 16 raw RGB15 palette entries, slot 0 transparent (the format pal_obj_mem wants
 * directly -- no expansion needed). Returns 1, or 0 (dst undefined) on failure. */
int rom_hand_pal(const RomHand* rh, uint16_t dst[16]);

#endif /* ROM_HAND_H */
