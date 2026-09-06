#ifndef ROM_GBICON_INCLUDED
#define ROM_GBICON_INCLUDED

#include <stdint.h>

/*
 * Gen-2 (Gold/Silver/Crystal) PARTY-MENU ICONS -- the tiny 16x16 bouncing
 * silhouettes the real game draws in its party menu (also the naming screen,
 * the move-list/trade screens, and the Fly map), read live out of the user's
 * own cartridge dump.
 *
 * D5 (E5 fix, adversarial review): NOT the PC box list -- per pokecrystal, Bill's
 * PC draws each mon's FRONT PICTURE (the same battle-style sprite rom_gbsprite.c
 * already serves), never this icon. An earlier version of this comment claimed
 * the PC list used it too; it does not.
 *
 * WHY THIS EXISTS (E5, docs/SPRITE-ERA-DESIGN.md sec 2/4): source/rom_gbsprite.c
 * already gives a Gen-2 import its 56x56 battle-style front sprite for the PC and
 * the summary, but the PARTY MENU draws a much smaller, much simpler icon
 * instead (one of only ~38 generic shapes shared across many species, e.g. every
 * "small round bird" species reuses the same bird icon) with NO per-species
 * colour. Kept a separate module (not folded into rom_gbsprite.c) because the
 * data shape, the addressing scheme (species -> a small icon KIND -> a shared
 * bitmap, not species -> its own bitmap) and the palette rule are all different
 * enough that merging them would have made rom_gbsprite.c's own header harder to
 * read, not easier.
 *
 * PokeDNA ships NO Game Freak art. Everything here is a transient read of a file
 * the user already owns, exactly the posture rom_gbsprite.c/rom_sprite.c take.
 *
 * PURE C: no tonc, no FatFs, no GBA headers. All I/O goes through a caller
 * GbReadFn (the exact typedef gb_sprite_codec.h/rom_gbsprite.h already use -- see
 * that header's own note: "same shape as rom_map.h's RomReadFn so one FatFs
 * adapter can serve both"), so tests/host_romgbicon_test.c runs this exact code
 * on the PC against Guy's real Gold.gbc and Crystal.gbc.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DATA IS, AND HOW THIS MODULE FINDS IT (clean-room: pokecrystal and
 * pokegold were read on the web for the LAYOUT only -- data/pokemon/menu_icons.asm,
 * data/icon_pointers.asm, constants/icon_constants.asm, engine/gfx/mon_icons.asm,
 * gfx/icons/ (one NAME.png per icon) -- and every number below was then
 * INDEPENDENTLY REDISCOVERED
 * by scanning Guy's own dumps; nothing here is copied from, or trusts, decomp
 * source. The addresses this landed on are recorded so a human can sanity-check
 * them, exactly like rom_gbsprite.h's own table -- they are NEVER used as inputs):
 *
 *                    Gold.gbc   Crystal.gbc
 *   MonMenuIcons     0x8E975    0x8EAC4      (251 bytes, one icon KIND per
 *                                             species, national-dex indexed --
 *                                             Gen 2's internal species id IS the
 *                                             national dex number, unlike Gen 1)
 *   IconPointers     0x8EA70    0x8EBBF      (N+1 little-endian bank-relative
 *                                             pointers, N = 38 in BOTH games)
 *   icon bank        0x23       0x23         (= floor(IconPointers' OWN file
 *                                             offset / 0x4000) -- see below)
 *
 * MonMenuIcons (pokecrystal/pokegold data/pokemon/menu_icons.asm): 251 consecutive
 * bytes, each a "kind" 1..N (kind 0, ICON_NULL, and the EGG kind never appear in
 * this species table). Located BY SHAPE: every byte in [1,63] (a generous ceiling;
 * the real ceiling N is discovered per game, see below) PLUS three structural
 * invariants that are true in every English release and are exceedingly unlikely
 * to occur by chance in unrelated ROM bytes:
 *     bytes[0] == bytes[1] == bytes[2]   (Bulbasaur/Ivysaur/Venusaur share a kind)
 *     bytes[3] == bytes[4]               (Charmander/Charmeleon share a kind)
 *     bytes[3] != bytes[5]               (Charizard's kind DIFFERS -- BIGMON)
 *     bytes[6] == bytes[7] == bytes[8]   (Squirtle/Wartortle/Blastoise share one)
 * plus at least 8 distinct values across the 251 bytes (rules out a degenerate
 * all-same-byte run). N is then read off as the WINDOW'S OWN maximum byte value
 * (never a literal "36" or "38" pinned in this file) -- measured 38 on both of
 * Guy's Gen-2 dumps, matching pokegold's and pokecrystal's identical
 * `DEF NUM_ICONS EQU const_value - 1` (39 constants 0..38, ICON_NULL=0 unused
 * here) -- but the code below never assumes that number, it reads it back.
 *
 * D7 (E5 fix): "the window's own maximum byte value" equals NUM_ICONS only
 * because Charizard happens to hold the LAST-assigned kind in both games (its
 * own row above -- BIGMON -- is what proves its kind is unique, not that it is
 * the highest one; the two facts coincide in Gold/Silver/Crystal but nothing
 * here derives one from the other). A hypothetical variant whose top-numbered
 * kind belonged to some OTHER, unused species would make window_max() read a
 * value strictly LESS than that variant's true NUM_ICONS. That is not a
 * mis-location risk: an icon kind window_max() never reports for `n` is simply
 * a kind this module will refuse to serve (kind > gi->n fails rom_gbicon_tiles/
 * rom_gbicon_pal's own range check) -- FAIL CLOSED, never mis-locate. The
 * failure mode is "this one high-numbered species never gets an icon," not
 * "some species gets the WRONG icon."
 *
 * IconPointers (data/icon_pointers.asm): N+1 little-endian 16-bit values in
 * 0x4000..0x7FFF (the GB ROMX window). Located BY SHAPE too, using a fact this
 * project discovered rather than one decomp states outright: entries 1..N are
 * laid down by consecutive `INCBIN "gfx/icons/NAME.2bpp"` directives with NO
 * padding between them, and every icon is exactly 128 bytes (8 tiles: two 16x16
 * animation frames of 4 tiles each) -- so entries 1..N form an EXACT arithmetic
 * run, entry[i+1] == entry[i] + 128, and it must be MAXIMAL (the run does not
 * continue one entry further, which is what pins down N exactly rather than
 * accepting any shorter prefix). Entry 0 (NullIcon) is additionally required to
 * equal entry 1 (PoliwagIcon) -- pokecrystal's own `NullIcon: PoliwagIcon:
 * INCBIN ...` declares them at THE SAME ADDRESS, a zero-length "alias" label, and
 * that equality is itself a strong, ROM-shape-only signature. A hit must be
 * unique in the whole file; combined with the MonMenuIcons hit above (whose own
 * N selects the run length to search for), this locator found EXACTLY ONE
 * (MonMenuIcons, IconPointers) pair in each of Gold.gbc and Crystal.gbc, and
 * NONE in Red.gb or Yellow.gb (whose Gen-1 icon table is a completely different,
 * nybble-packed, ~11-kind shape -- pokered data/pokemon/menu_icons.asm -- so it
 * never satisfies the byte-per-species invariants above).
 *
 * THE ICON BANK. `ld b, BANK(Icons)` (engine/gfx/mon_icons.asm) is emitted right
 * before the graphics are DMA'd in, but IconPointers/MonMenuIcons themselves are
 * addressed with NO bank switch at all in the same routine -- meaning the whole
 * "mon icons" subsystem (code, both tables AND the `Icons:` graphics they point
 * into) lives together in ONE non-fixed ROMX bank, reached via a `callfar` from
 * elsewhere, not in the fixed bank-0 "home" area. That is exactly what this
 * module found: on BOTH of Guy's dumps, `floor(IconPointers' file offset /
 * 0x4000)` -- the bank IconPointers itself sits in -- is bank 0x23, and decoding
 * every one of the N icons through THAT bank produces a real, non-degenerate
 * 16x16x2bpp picture for every one of them (cross-checked, during this module's
 * development only and never shipped or embedded, pixel-for-pixel against
 * pokecrystal's own published gfx/icons/bulbasaur.png -- an exact match). So the
 * icon bank is DERIVED, not pinned: `icon_bank = icon_pointers_offset / 0x4000`,
 * and open() additionally requires every one of the N icons to decode to a
 * non-degenerate (>=2 distinct 2bpp shades) frame 0 through that derived bank as
 * a fail-closed sanity net on top of the structural argument -- a ROM where this
 * derivation does not hold refuses outright rather than guessing.
 *
 * ---------------------------------------------------------------------------
 * THE PALETTE (D2, E5 fix -- an earlier version of this note was wrong twice
 * over). Gen-2 menu icons carry NO per-species (or per-kind) colour table at
 * all -- engine/gfx/mon_icons.asm never references a palette, OBP or BGP
 * anywhere near the icon-loading code, in EITHER pokegold or pokecrystal, unlike
 * the front/back sprites' own PokemonPalettes table (rom_gbsprite.h). But that
 * does NOT mean the icons render in the DMG's monochrome ramp -- they are
 * coloured with the FIXED party-menu OBJ palette (gfx/stats/party_menu_ob.pal,
 * PartyMenuOBPals, byte-identical in both games): idx0 RGB(27,31,27)
 * transparent, idx1 RGB(31,19,10) light orange, idx2 RGB(31,7,4) red, idx3
 * RGB(0,0,0) black. rom_gbicon_pal() therefore always answers that same fixed
 * four-colour palette for every kind, in both games -- there is nothing else to
 * read, but it is a genuine (if narrow) colour palette, not a monochrome ramp,
 * and it is NOT rom_gbsprite.h's DMG grey ramp (G1_GREY0..3) -- that one belongs
 * to a completely different asset (Gen-1 battle sprites), which really is
 * monochrome. Confusing the two would have painted every Gen-2 icon in Gen-1's
 * greys instead of Gen-2's actual orange-and-red look.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT COSTS. RomGbIcon is small (~24 B: one borrowed GbReadFn/ctx pair plus
 * two file offsets and two bytes) and RomGbIconLoc (~20 B) is a plain-old-data
 * cache record, same shape as RomGbSpriteLoc. One frame (rom_gbicon_tiles) is
 * exactly 64 B -- 4 tiles x 16 B, ONE animation frame, never both -- and one
 * expanded RGB15 icon is 256 x 2 B = 512 B, which is what gb_art_source.c's
 * icon() callback decodes into (see its own memory note for exactly where).
 * open()'s scan window requirement mirrors rom_gbsprite.h's (>= 2 KiB) so the two
 * modules can share one caller-owned scratch buffer, never held resident at the
 * same time.
 */

#include "gb_sprite_codec.h"   /* GbReadFn -- the one I/O typedef every rom_gb*
                                * module shares, so one FatFs adapter serves all */

#define ROM_GBICON_SCRATCH_MIN   2048   /* same window size as rom_gbsprite.h    */
#define ROM_GBICON_SPECIES       251    /* MonMenuIcons length                   */
#define ROM_GBICON_MAX_KINDS     63     /* generous ceiling on N (measured 38)   */
#define ROM_GBICON_TILE_BYTES    16u
#define ROM_GBICON_FRAME_TILES   4u
#define ROM_GBICON_FRAME_BYTES   (ROM_GBICON_FRAME_TILES * ROM_GBICON_TILE_BYTES) /* 64 */
#define ROM_GBICON_W             16
#define ROM_GBICON_H             16
#define ROM_GBICON_PX            (ROM_GBICON_W * ROM_GBICON_H)   /* 256          */

typedef struct RomGbIcon {
  GbReadFn read;
  void*    ctx;
  uint32_t size;

  uint32_t mon_menu_icons;   /* file offset, ROM_GBICON_SPECIES bytes            */
  uint32_t icon_pointers;    /* file offset, (n+1) little-endian u16 entries     */
  uint8_t  n;                /* highest valid kind (1..n); 0 is never valid      */
  uint8_t  icon_bank;        /* derived: icon_pointers / 0x4000                  */
  uint32_t id_hash;          /* FNV-1a of the GB header, same fingerprint as
                               * RomGbSprite's -- lets a cached loc prove it is
                               * still describing THIS exact file               */
  int      ok;
} RomGbIcon;

/* Everything open() discovered, for a location-cache file (a NEW, separate
 * per-gen cache -- see gb_art_source.c -- not folded into the existing
 * gbart2.loc: that file's format is shipped and tested for the PORTRAIT rung,
 * and a second, independently-versioned cache file costs nothing extra under
 * PDNA_DIR while carrying zero risk to it). Validate-on-load is automatic:
 * rom_gbicon_open_loc() re-checks id_hash/size and every named offset before
 * trusting the cache, exactly like RomGbSpriteLoc. (D3, E5 fix: this claim used
 * to be false for mon_menu_icons -- the cached path re-validated icon_pointers/
 * icon_bank via the per-kind decode sanity net but never re-read the menu-icons
 * window at all, so a stale/tampered mon_menu_icons offset with an otherwise
 * matching id_hash/size would sail through. Fixed: the cached path now re-runs
 * menu_icons_cb's structural shape check against loc->mon_menu_icons and
 * requires window_max() to still equal loc->n before accepting it.) */
typedef struct RomGbIconLoc {
  uint32_t id_hash;
  uint32_t size;
  uint32_t mon_menu_icons;
  uint32_t icon_pointers;
  uint8_t  n;
  uint8_t  icon_bank;
  uint8_t  pad[2];
} RomGbIconLoc;

/* Identify a Gen-2 Game Boy ROM and locate its menu-icon tables. `scratch` is the
 * caller's scan window (>= ROM_GBICON_SCRATCH_MIN; the SAME buffer
 * rom_gbsprite_open() uses is fine -- both only touch it during their own open
 * call). Returns 1, or 0 (fail closed) for: a non-GB image, a truncated one, a
 * Gen-1 ROM (no matching icon-table shape exists there), or any ROM whose tables
 * are missing, ambiguous, or fail the derived-bank sanity check. */
int rom_gbicon_open(RomGbIcon* gi, GbReadFn read, void* ctx, uint32_t size,
                    uint8_t* scratch, uint32_t scratch_len);

/* Same, but start from a cached RomGbIconLoc. Rejected (falling back to a full
 * scan) unless its id_hash, size and every offset it names still check out. */
int rom_gbicon_open_loc(RomGbIcon* gi, GbReadFn read, void* ctx, uint32_t size,
                        uint8_t* scratch, uint32_t scratch_len,
                        const RomGbIconLoc* loc);

/* Snapshot what open() found. Safe to call only when gi->ok. */
void rom_gbicon_save_loc(const RomGbIcon* gi, RomGbIconLoc* out);

/* This species' icon KIND, 1..gi->n, or 0 (never a valid kind) for `dex` outside
 * 1..251, an unopened/failed `gi`, or a corrupt table entry. `dex` is the
 * NATIONAL dex number -- Gen 2's own internal species id already equals it for
 * every one of the 251 species this module ever sees (no PokedexOrder-style
 * remap exists in Gen 2, unlike Gen 1; see rom_gbsprite.h). */
int rom_gbicon_kind(const RomGbIcon* gi, uint16_t dex);

/* Read one 16x16 animation frame's raw 2bpp tile bytes for icon `kind` (1..n)
 * into `out[64]` -- 4 tiles x 16 B, tile order top-left/top-right/bottom-left/
 * bottom-right (rgbgfx's default row-major 2x2 layout, confirmed against the
 * real pixel art during this module's development). `frame` is 0 or 1 (Gen-2
 * menu icons animate between two frames; E5 only ever asks for frame 0, the
 * static "menu" look, but both exist and are equally cheap to read). Returns 1,
 * or 0 for a bad kind/frame or a read that left the ROM. */
int rom_gbicon_tiles(RomGbIcon* gi, int kind, int frame, uint8_t out[ROM_GBICON_FRAME_BYTES]);

/* The fixed four-colour party-menu OBJ palette every kind uses (see the palette
 * note above -- it is NOT the DMG monochrome ramp).
 * `kind` and `gi` are accepted (and validated) for API symmetry with
 * rom_gbsprite_pal() and so a future per-kind table, if one is ever found, has
 * somewhere to plug in without an ABI change -- today every valid call answers
 * the same four RGB15 values. Returns 1, or 0 for an invalid kind/gi. */
int rom_gbicon_pal(const RomGbIcon* gi, int kind, uint16_t out[4]);

/* Expand one already-read frame (rom_gbicon_tiles' `tiles[64]`) into `dst[256]`
 * RGB15 pixels, row-major 16x16, colour 0 transparent (bit 15 clear) and every
 * other colour 0x8000|RGB15 opaque -- exactly ui_sprite()'s format, so a menu
 * icon and a 56x56 portrait sit in the same drawing path. Returns 1, or 0 for a
 * NULL argument or a tile byte that decodes to an out-of-range palette index
 * (never possible for a real 2bpp tile, but checked rather than trusted). */
int rom_gbicon_to_rgb15(const uint8_t tiles[ROM_GBICON_FRAME_BYTES],
                        const uint16_t pal[4], uint16_t* dst /* ROM_GBICON_PX */);

#endif /* ROM_GBICON_INCLUDED */
