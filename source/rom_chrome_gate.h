#ifndef ROM_CHROME_GATE_H
#define ROM_CHROME_GATE_H

/*
 * Compile-time switch: is the GENERATED, git-ignored art for a chrome screen present
 * in THIS build tree right now? tools/gen_card_bg.py / tools/gen_pokeblock_bg.py write
 * a small companion .s file next to the .c they generate (card_bg_data.s /
 * pokeblock_bg_data.s); their presence is the same signal card_bg.c's own comment
 * documents ("git-ignored ripped art"), and testing for it with __has_include is the
 * SAME technique pdna_main.c already uses for daycare_bg_data.h
 * (`#if defined(__has_include) && __has_include("daycare_bg_data.h")`).
 *
 * WHY THIS MATTERS (ROM SIZE): the Makefile globs every source/ .c file unconditionally and links
 * every resulting .o directly (no -ffunction-sections/--gc-sections), so any code this
 * repo compiles ends up in EVERY build's .gba, called or not. rom_chrome.c's decoders
 * and lzblob.c's ROM-tile compositor are real weight (LZ77 decode + a tile blitter) that
 * the full-art build must NEVER pay for, since it already has the real pre-composited
 * art and would never call them. Wrapping that code in
 * `#if !PDNA_CARD_ART_COMPILED` (etc.) makes it compile to an EMPTY translation unit
 * the moment the matching generated file exists on disk — zero bytes added to the
 * full-art ROM, not just zero bytes CALLED. */

/* A `-D` on the compile line (tests/host_romchrome_test.c's own cc line uses
 * this) wins over the __has_include probe -- the decoders must be host-testable
 * regardless of whether THIS machine's source/ tree happens to have the
 * generated art files staged locally right now. */
#ifndef PDNA_CARD_ART_COMPILED
#if defined(__has_include) && __has_include("card_bg_data.s")
#define PDNA_CARD_ART_COMPILED 1
#else
#define PDNA_CARD_ART_COMPILED 0
#endif
#endif

#ifndef PDNA_POKEBLOCK_ART_COMPILED
#if defined(__has_include) && __has_include("pokeblock_bg_data.s")
#define PDNA_POKEBLOCK_ART_COMPILED 1
#else
#define PDNA_POKEBLOCK_ART_COMPILED 0
#endif
#endif

/* Bag chrome IS implemented from ROM for Emerald/FireRed/LeafGreen (see
 * rom_chrome.h's header comment) — the tileset that DESIGN.md called "not
 * located" (Emerald) or missing entirely (FireRed/LeafGreen) turned out to be
 * a few KB, not 8 KiB; only Ruby's is genuinely too big (8,192 B alone) and
 * stays unsupported. Same __has_include gate as card/pokeblock, so a full-art
 * build with bag_bg_data.s staged pays zero bytes for the decoder. */
#ifndef PDNA_BAG_ART_COMPILED
#if defined(__has_include) && __has_include("bag_bg_data.s")
#define PDNA_BAG_ART_COMPILED 1
#else
#define PDNA_BAG_ART_COMPILED 0
#endif
#endif

/* Any ROM-chrome code needed at all in this build? (card OR pokeblock OR bag missing art) */
#define PDNA_ROM_CHROME_NEEDED \
  (!PDNA_CARD_ART_COMPILED || !PDNA_POKEBLOCK_ART_COMPILED || !PDNA_BAG_ART_COMPILED)

#endif /* ROM_CHROME_GATE_H */
