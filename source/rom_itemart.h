#ifndef ROM_ITEMART_H
#define ROM_ITEMART_H

#include <stdint.h>
#include "rom_map.h"   /* RomCtx / RomReadFn / ROM_BASE */

/*
 * ITEM ICONS and TYPE BADGES, read out of the user's own retail ROM.
 *
 * Phase 4 of the ROM-gated build (docs/research-rom-gated-build.md §10): two small
 * art classes that together weigh 331,048 B of .text in the shipped binary
 * (measured 2026-08-09 from build/: item_icons_data.o 313,344 + item_icons.o 1,552 +
 * type_icons.o 16,152) and are 100% Nintendo pixels. Reading them from the player's
 * own cartridge instead deletes both from the distribution surface, and costs 2,619 B
 * of code to do it. Nothing is copied anywhere; the reads are transient, the posture
 * the map viewer has always used.
 *
 * The output format is deliberately the one the screens already draw:
 *     u16 = 0x0000          transparent (palette index 0)
 *         = 0x8000 | rgb15  opaque
 * i.e. exactly what `item_icon_for()` (item_icons.h) and `type_icon_for()`
 * (type_icons.h) return today, so the ROM-backed provider drops into the same call
 * sites and the art-free build (art_fallbacks.c returning NULL) stays the product.
 *
 * Pure C (no tonc, no FatFs, no GBA headers): all I/O goes through the RomCtx's
 * RomReadFn, so tests/host_romitemart_test.c runs this exact code against the real
 * cartridge dumps on the PC. Decompression is map_render.c's streaming mr_lz77(),
 * which pulls the compressed bytes through a 64-byte window -- no compressed blob is
 * ever staged.
 *
 * ============================================================================
 * WHERE THE DATA LIVES -- measured on all five of Guy's dumps, not assumed
 * ============================================================================
 *
 * NEITHER table is in Game Freak's GFRomHeader at ROM+0x100 (checked field by field
 * against pokeemerald src/rom_header_gf.c:18-175 -- the header carries `items`
 * (+0xC8) but nothing about icon GRAPHICS). Both are therefore PINNED per revision,
 * exactly like rom_map.c's k_versions and rom_text.c's k_pins, and every pin below
 * was found by an independent SHAPE SCAN of the whole image -- the same method
 * tools/gen_encounters.py uses for gWildMonHeaders. tests/host_romitemart_test.c
 * re-runs that scan on every dump and asserts it reproduces the pin, and PRINTS the
 * address so a human can check it against published values.
 *
 * ---- ITEM ICONS -------------------------------------------------------------
 * `gItemIconTable[ITEMS_COUNT + 1][2]` (pokeemerald src/data/item_icon_table.h:1,
 * pokefirered src/data/item_icon_table.h:1 where it is `sItemIconTable`): one row
 * per item id, each row { const u32* pic; const u32* pal }, BOTH LZ77-compressed --
 * the pic to 288 B (24x24 4bpp, decomp graphics/items/icons/<name>.png are 24x24 without
 * exception, and item_icon.c allocates exactly 0x120) and the palette to 32 B
 * (16 RGB15 entries; the game calls LoadCompressedSpritePalette on it, which is what
 * gives the palette an LZ header too).
 *
 * The scan looks for a maximal run of 8-byte rows where the first word points at an
 * LZ blob whose header says 0x120 and the second at one that says 0x20 (or the row
 * is a NULL pair), and requires >= 300 real rows. Result on Guy's dumps:
 *
 *     Emerald  BPEE r0   0x08614410   378 rows (ITEMS_COUNT 377 + 1), 378 non-NULL
 *     FireRed  BPRE r1   0x083D4304   376 rows (ITEMS_COUNT 375 + 1), 376 non-NULL
 *     LeafGreen BPGE r0  0x083D40D0   376 rows,                       376 non-NULL
 *     Ruby AXVE r2 / Sapphire AXPE r1 -- NO CANDIDATE AT ANY THRESHOLD
 *
 * The +1 row is the bag's "return to field" arrow (item_icon.c's ITEM_LIST_END
 * case), not an item; ids 0..ITEMS_COUNT-1 are what this module serves.
 *
 * **Ruby/Sapphire have no item icons AT ALL, and that is a fact about the games,
 * not a gap in the scan.** An entire 16 MiB Ruby image contains exactly ONE LZ blob
 * whose uncompressed size is 0x120 (Emerald has 262) and ZERO words pointing at it.
 * Item icon sprites debuted in FR/LG/Emerald: R/S's bag is a text list, and the
 * pokeruby decomp accordingly has no item_icon.c and no gItemIconTable. So R/S FAIL
 * CLOSED for items -- rom_itemart_have_items() returns 0 and the bag keeps its
 * text-only layout there forever, which is the honest answer.
 *
 * ---- TYPE BADGES ------------------------------------------------------------
 * These are stored in two COMPLETELY different ways, and a reader that assumes the
 * Emerald one finds nothing in FR/LG:
 *
 *   RSE (Ruby, Sapphire, Emerald) -- `gMoveTypes_Gfx` + `gMoveTypes_Pal`
 *     (pokeemerald src/graphics.c:1122-1123, pokeruby src/pokemon_summary_screen.c:136).
 *     ONE LZ77 sheet of (NUMBER_OF_MON_TYPES + CONTEST_CATEGORIES_COUNT) * 0x100 =
 *     23 * 256 = 5,888 B (the size pokemon_summary_screen.c:891-895 declares), i.e.
 *     23 badges of 32x16 4bpp in GBA 1D tile order (4 tiles across, 2 down); the
 *     art is graphics/types/<name>.png, each 32x16. The palette blob is a second LZ77
 *     blob of 96 B = THREE 16-colour palettes (graphics_file_rules.mk:136-138
 *     concatenates move_types_{1,2,3}.gbapal), and which palette a badge uses is
 *     `sMoveTypeToOamPaletteNum` (pokemon_summary_screen.c:907-931, values 13/14/15
 *     = OBJ palette slots) -- get it wrong and FIRE renders in WATER's blue.
 *     The palette blob always sits at the next 4-aligned address after the sheet's
 *     compressed bytes, but the compressed length is not recoverable from a header,
 *     so both addresses are pinned.
 *
 *   FRLG -- there is NO gMoveTypes_Gfx. FireRed/LeafGreen draw type badges with
 *     `BlitMenuInfoIcon` (src/list_menu.c:755-758) out of `gMenuInfoElements_Gfx`
 *     (src/graphics.c:1142), an UNCOMPRESSED 128x128 4bpp sheet (0x2000 B) that also
 *     holds the POWER/ACCURACY/PP/EFFECT labels and the dex "caught" ball. A badge
 *     is 32x12 (not 32x16) at a per-type TILE offset from `sMenuInfoIcons`
 *     (list_menu.c:52-77), and all 18 share ONE palette, `gMenuInfoElements2_Pal`
 *     (graphics.c:1141, "pokemon_types.gbapal"), 32 raw bytes immediately before the
 *     sheet. Confirmed by rendering the located sheet: all 18 badges legible and
 *     correctly coloured. FRLG has no contest categories, so it serves 18, not 23.
 *
 *     Pins: FireRed BPRE r1 gfx 0x08E95DDC pal 0x08E95DBC
 *           LeafGreen BPGE r0 gfx 0x08E95E5C pal 0x08E95E3C
 *           Emerald BPEE r0  gfx 0x08D971B0 pal 0x08D97B84
 *           Ruby AXVE r2 / Sapphire AXPE r1 -- gfx 0x08E71D10 pal 0x08E726E4
 *           (the same address in both, and the 2,513 compressed bytes there are
 *           BYTE-IDENTICAL between the two carts -- verified, not a copy-paste slip)
 *
 * Type badges are the one part of this module that is purely COSMETIC: PokeDNA's
 * art-free fallback for them is `ui_type_chip()` (source/ui.c, drawn from original
 * coloured chips), so a ROM that cannot serve badges loses nothing functional. Item
 * icons degrade the same way -- `item_icon_for()` returns NULL and the bag/TM/berry
 * rows stay text-only, which is what the shipped artless build already does.
 *
 * ---- GEOMETRY NOTE for callers replacing the baked blob ----------------------
 * The shipped `type_icons` blob is 32x14, because tools/gen_types.py crops to the
 * tight bounding box shared by all 18 PNGs. MEASURED on the ROM sheet: for all 23
 * RSE badges pixel rows 0 and 15 are entirely transparent, so ROM rows 1..14 are
 * byte-for-byte the framing the baked asset used. FRLG's 32x12 badges have no blank
 * rows and are a different (smaller, flat-shaded) design -- ask
 * rom_type_badge_h() rather than assuming.
 *
 * ============================================================================
 * MEMORY -- no new statics anywhere
 * ============================================================================
 * This file defines ZERO mutable statics (only `const` pin/offset tables in .rodata),
 * which matters: the hardware build has ~1.5 KB of EWRAM left and a post-link guard
 * that rejects it if .sbss crosses the end of EWRAM.
 *
 * Stack, MEASURED with -fstack-usage for the thumb/-O2 target build, deepest chain
 * per entry point (rom_itemart.c frames + decode_verified 40 + mr_lz77 120):
 *   - rom_item_icon()        552 B  (288 B 4bpp stage + 32 B palette stage inside)
 *   - rom_itemart_open()     544 B  (its probe decodes one icon)
 *   - rom_type_sheet_load()  296 B
 *   - rom_type_badge()       344 B  (FR/LG stages 8 tiles = 256 B)
 * All well inside the 32 KiB IWRAM stack, and nothing here is ever the thing that
 * overflows it -- but every PIXEL buffer is CALLER-PROVIDED and must not be a stack
 * local:
 *   - an item icon needs 1,152 B (ROM_ITEM_ICON_PX u16);
 *   - a badge needs up to 1,024 B (ROM_TYPE_BADGE_MAX_PX u16);
 *   - rom_type_sheet_load() on RSE needs a scratch of at least ROM_TYPE_SHEET_BYTES
 *     (5,888 B) that stays alive and unmodified as long as the RomTypeSheet is used;
 *     FR/LG needs none at all (it reads the 256 B a badge occupies straight out of
 *     the ROM when the badge is asked for).
 *
 * RECOMMENDED BUFFER: `mon_decomp` (source/mon_front.c, 8,192 B EWRAM_BSS, already
 * shared with mon_back.c and rom_sprite.c). It holds the 5,888 B badge sheet with
 * 2,304 B to spare, so a screen can keep the sheet at mon_decomp[0..5888) and decode
 * each badge into mon_decomp[6144..7168) with no second allocation. An item icon
 * needs only 1,152 B of it. Never put these on the IWRAM stack.
 *
 * ============================================================================
 * THE SILENT-GARBAGE HAZARD
 * ============================================================================
 * EZ-Flash `Read_SD_sectors` can return SUCCESS while holding garbage, so any read
 * that becomes pixels must be verified. Verification is ON by default and lives
 * where the data lands, not in every consumer: every pixel and palette payload is
 * fetched up to three times and accepted only when two consecutive fetches agree on
 * length and on a 32-bit FNV-1a hash of the bytes (box_oam.c's icopy_verified
 * posture, and byte-for-byte the scheme rom_sprite.c uses). It costs a second pass
 * over the SD. A fused / cartridge-bus source needs none of it:
 * rom_itemart_set_verify(ra, 0).
 */

/* ---- item icons ------------------------------------------------------------ */
#define ROM_ITEM_ICON_W        24
#define ROM_ITEM_ICON_H        24
#define ROM_ITEM_ICON_PX       (ROM_ITEM_ICON_W * ROM_ITEM_ICON_H)   /* 576 u16   */
#define ROM_ITEM_ICON_BYTES    288    /* 24x24 4bpp, the decompressed pic         */
#define ROM_ITEM_PAL_BYTES     32     /* 16 RGB15 entries, also LZ-compressed     */

/* ---- type badges ----------------------------------------------------------- */
#define ROM_TYPE_BADGE_W       32
#define ROM_TYPE_BADGE_H_RSE   16
#define ROM_TYPE_BADGE_H_FRLG  12
#define ROM_TYPE_BADGE_MAX_PX  (ROM_TYPE_BADGE_W * ROM_TYPE_BADGE_H_RSE)  /* 512  */
#define ROM_TYPE_TYPES         18     /* Gen-3 internal type ids 0..17 (9 = ???)  */
#define ROM_TYPE_CONTEST       5      /* Cool/Beauty/Cute/Smart/Tough -- RSE only */
#define ROM_TYPE_BADGES_RSE    (ROM_TYPE_TYPES + ROM_TYPE_CONTEST)        /* 23   */
#define ROM_TYPE_SHEET_BYTES   (ROM_TYPE_BADGES_RSE * 256)                /* 5888 */
#define ROM_TYPE_PALS_RSE      3
#define ROM_TYPE_FRLG_SHEET_BYTES 0x2000   /* the raw 128x128 4bpp menu-info sheet */

/* How this ROM stores its type badges. */
typedef enum {
  ROM_TYPEART_NONE = 0,
  ROM_TYPEART_RSE,       /* gMoveTypes_Gfx: one LZ sheet, 23 badges of 32x16, 3 pals */
  ROM_TYPEART_FRLG       /* gMenuInfoElements_Gfx: raw 128x128 sheet, 18 of 32x12    */
} RomTypeStyle;

typedef struct RomItemArt {
  const RomCtx* rc;
  uint32_t items;         /* ROM ADDRESS of gItemIconTable; 0 = this ROM has none  */
  uint16_t item_count;    /* ITEMS_COUNT -- ids 0..item_count-1 are served         */
  uint32_t type_gfx;      /* ROM ADDRESS of the badge sheet; 0 = none              */
  uint32_t type_pal;      /* ROM ADDRESS of the badge palette(s); 0 = none         */
  uint8_t  type_style;    /* RomTypeStyle                                          */
  int      ok;            /* 1 = at least one of the two classes opened            */
  int      verify;        /* 1 = fetch-twice-and-compare (default 1)               */
} RomItemArt;

/* A loaded badge sheet. Cheap to keep on a screen's stack (about 128 B); for RSE it
 * borrows the caller's scratch, so that scratch must outlive it.
 *
 * `owner` is the RomItemArt it was loaded from, and rom_type_badge() refuses a sheet
 * whose owner is not the RomItemArt it is handed. That is what stops the ugly bug:
 * Ruby, Sapphire and Emerald all share the RSE style but ship slightly different
 * badge art, so a sheet left over from a previous ROM would quietly draw the wrong
 * game's badges. Note the consequence -- COPYING a RomItemArt by value invalidates
 * every sheet loaded from the original; reload rather than copy. */
typedef struct RomTypeSheet {
  const struct RomItemArt* owner;   /* the RomItemArt this was loaded from          */
  const uint8_t* tiles;             /* RSE: the decoded 5,888 B sheet. FRLG: NULL  */
  uint16_t pal[ROM_TYPE_PALS_RSE][16];
  uint8_t  style;                   /* RomTypeStyle                                */
  uint8_t  count;                   /* badges served: 23 RSE / 18 FRLG             */
  uint8_t  h;                       /* badge height: 16 RSE / 12 FRLG              */
  int      ok;
} RomTypeSheet;

/*
 * Identify both tables for this ROM and validate each one independently. Returns 1
 * if EITHER class is available (ask per class with rom_itemart_have_*), 0 if neither
 * is -- which is what an unpinned revision, a relocated ROM hack or a truncated dump
 * gets. A class that fails validation is switched off on its own; the other still
 * works (Ruby/Sapphire routinely land here: badges yes, item icons no).
 *
 * There is deliberately no runtime shape scan: finding these tables means walking a
 * 16 MiB image, which is seconds of SD traffic on hardware. An unknown revision fails
 * closed and the fix is mechanical -- run the host test's scan against the dump and
 * paste the row into k_pins.
 *
 * COST ON HARDWARE: validation is not free -- it fully decodes one item icon and
 * probes four more rows, and on FR/LG it reads one badge to check its shape. That is
 * ~16 verified reads, i.e. one open() is about the cost of two icons. Open once per
 * ROM session (the app_icon_rom_open() lifecycle), not per screen, and never from a
 * render loop: like every other ROM read, this must happen with the screen blanked
 * and IRQs handled per the OS-mode rule.
 */
int rom_itemart_open(RomItemArt* ra, const RomCtx* rc);

/* Turn the fetch-twice-and-compare verification off (fused / cartridge-bus source)
 * or back on. Default is ON. */
void rom_itemart_set_verify(RomItemArt* ra, int on);

/* ---- item icons ------------------------------------------------------------ */

/* 1 if this ROM can answer for item icons at all (0 for Ruby/Sapphire, always). */
int rom_itemart_have_items(const RomItemArt* ra);

/* One past the largest item id served: 377 (Emerald) / 375 (FR/LG). 0 when off. */
int rom_itemart_item_count(const RomItemArt* ra);

/*
 * Decode one item's 24x24 icon into `dst` as ROM_ITEM_ICON_PX RGB15 pixels,
 * row-major, 0 = transparent / 0x8000|rgb = opaque -- the exact buffer
 * `item_icon_for()` hands back today.
 *
 * `cap_px` is the capacity of dst in u16 units; anything short of
 * ROM_ITEM_ICON_PX FAILS rather than writing a partial icon.
 *
 * Returns 1, or 0 -- with dst's contents UNDEFINED -- for an id this ROM does not
 * serve, a NULL row, a pointer outside the image, a blob that does not decompress to
 * exactly 288 B / 32 B, or a fetch that failed verification. Callers fall back to
 * their text layout on 0.
 *
 * Note that a 1 does not mean "a real picture": the games map every unused id to the
 * shared "?" placeholder icon (68 of Emerald's 378 rows, 68 of FR/LG's 376). That is
 * the cartridge's own answer, and this module reports it faithfully rather than
 * inventing a rule for which ids are "real".
 */
int rom_item_icon(const RomItemArt* ra, uint16_t item_id, uint16_t* dst, uint32_t cap_px);

/* ---- type badges ----------------------------------------------------------- */

/* 1 if this ROM can answer for type badges. */
int rom_itemart_have_types(const RomItemArt* ra);

/* Badges served: 23 on RSE (18 types + 5 contest categories), 18 on FRLG, 0 off. */
int rom_type_badge_count(const RomItemArt* ra);

/* Badge height in pixels: 16 on RSE, 12 on FRLG, 0 off. Width is always 32. */
int rom_type_badge_h(const RomItemArt* ra);

/* Bytes of scratch rom_type_sheet_load() needs: ROM_TYPE_SHEET_BYTES on RSE,
 * 0 on FRLG (and 0 when types are off). Ask; do not assume. */
int rom_type_scratch_bytes(const RomItemArt* ra);

/*
 * Load the badge sheet once per screen visit. On RSE this decompresses the whole
 * 5,888 B sheet into `scratch` (which must stay alive and unmodified for as long as
 * *ts is used) and decodes the three palettes; on FRLG it only reads the single
 * 32 B palette and `scratch` may be NULL. Verified per the rules above.
 *
 * Returns 1, or 0 with *ts zeroed (scratch too small, an unreadable blob, a
 * decompressed length that is not exactly the expected one, or a failed verify).
 */
int rom_type_sheet_load(const RomItemArt* ra, RomTypeSheet* ts,
                        uint8_t* scratch, uint32_t scratch_cap);

/*
 * Decode one badge into `dst` as 32 x rom_type_badge_h() RGB15 pixels, row-major,
 * same 0 / 0x8000|rgb convention.
 *
 * `badge` is the Gen-3 internal TYPE id for 0..17 (9 = the unused ??? type, which
 * really is in both sheets); 18..22 are the contest categories and exist on RSE
 * only. The right palette is selected for you (RSE has three and the mapping is not
 * guessable from the type id).
 *
 * `cap_px` is dst's capacity in u16 units; short buffers FAIL. Returns 1, or 0 with
 * dst UNDEFINED.
 *
 * COST: on RSE this touches NO ROM at all -- the sheet is already in the caller's
 * scratch, so a summary screen can redraw badges for free. On FR/LG each call reads
 * the 256 B the badge occupies (x2 when verifying), so cache the ones on screen
 * rather than calling per frame.
 */
int rom_type_badge(const RomItemArt* ra, const RomTypeSheet* ts, uint8_t badge,
                   uint16_t* dst, uint32_t cap_px);

#endif /* ROM_ITEMART_H */
