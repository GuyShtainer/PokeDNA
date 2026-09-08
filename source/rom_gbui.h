#ifndef ROM_GBUI_INCLUDED
#define ROM_GBUI_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#include "gb_sprite_codec.h"   /* GbReadFn */

/*
 * The Gen-1/Gen-2 trainer-card and bag UI GRAPHICS (font, text-box frame,
 * card-frame block, badges, gym-leader faces, the player's card pic, the
 * pack pocket art) -- located BY SHAPE in the user's own Game Boy cartridge
 * dump, the same posture rom_gbsprite.c / rom_gbicon.c already carry for the
 * Pokemon sprites and menu icons. Design: docs/GB-GAME-SCREENS-DESIGN.md
 * (BACKLOG #66/#67), section 2 for every signature + its verification rule,
 * Appendix A for the offsets these locators land on in Guy's four dumps
 * (recorded for a human to eyeball -- NEVER an input to this code).
 *
 * PokeDNA ships NO Game Freak art. Every graphic here is a transient read of
 * a file the user already owns; nothing is cached to disk except a small
 * table of FILE OFFSETS (RomGbUiLoc).
 *
 * PURE C: no tonc, no FatFs, no GBA headers, no mutable globals. All I/O goes
 * through the caller's GbReadFn, so tests/host_romgbui_test.c runs this exact
 * code on the PC against roms/gb/{Red,Yellow,Gold,Crystal}.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DATA IS, AND HOW THIS MODULE FINDS IT
 * ---------------------------------------------------------------------------
 * Nothing is pinned to a per-game constant unless it is CODE rather than data.
 * Where the graphic itself has no scannable shape (a font is just pixels),
 * the locator anchors on the LOADER ROUTINE's own opcode bytes -- exactly how
 * rom_gbsprite.c finds PokemonPalettes from _GetMonPalettePointer. Signatures
 * are short opcode runs (<= 33 B) or structural fingerprints only, never a
 * glyph/pixel bitmap.
 *
 * GEN 1 (pokered -- Red/Blue/Yellow)
 *   G1-F Font              LoadFontTilePatterns's LCD-off arm (home/load_font.asm),
 *                           128 tiles x 8 B, 1bpp. 1 hit required.
 *   G1-T TextBoxGraphics    same routine's other LCD-off arm, 32 tiles x 16 B, 2bpp.
 *   G1-C Card-frame block   DrawTrainerInfo's three consecutive far-copies
 *                           (engine/menus/start_sub_menus.asm), pinned by the
 *                           source-address relation C-A==144, E-A==512; the
 *                           BANK is then the unique one of banks[0..N) whose
 *                           9 frame tiles are all distinct, whose 22
 *                           leader-name tiles are ALL blank (erased in the
 *                           English releases) and whose 8 badge-number tiles
 *                           are non-degenerate.
 *   G1-B Badges + faces     the 8-byte .FaceBadgeTiles table
 *                           {20,28,30,38,40,48,50,58}; graphics start at
 *                           anchor+8, 64 tiles x 16 B, 2bpp.
 *   G1-P Player pic         DrawTrainerInfo's own opening `11 lo hi 01 01
 *                           bank`; of the ~70 raw hits, only the candidates
 *                           whose target byte is 0x77 (a Gen-1 pic's own
 *                           w<<4|h header) survive -- must collapse to
 *                           exactly one.
 *
 * GEN 2 (pokecrystal/pokegold -- Gold/Silver/Crystal)
 *   G2-R Text-box frames    LoadFrame (engine/gfx/load_font.asm), BYTE-IDENTICAL
 *                           in Gold and Crystal; 9 frames x 6 tiles, 1bpp.
 *   G2-F Font               DERIVED: Font = Frames - 1536 (gfx/font.asm's fixed
 *                           file order: FontExtra, Font, FontBattleExtra, Frames).
 *   G2-E FontExtra          DERIVED: FontExtra = Font - 512.
 *   G2-B/L Badges/Leaders   TrainerCard_Page2_LoadGFX; TWO hits in each ROM
 *                           (page-2 and the unreferenced page-3 INCBIN), and
 *                           the rule is "all hits byte-identical", not
 *                           "exactly one hit" (R4).
 *   G2-P Card pic           Crystal: GetCardPic (Chris + Kris, Kris = Chris +
 *                           0x230 exactly) + TrainerCardGFX. Gold has no
 *                           gender branch: ChrisPicAndTrainerCardGFX (35 + 6
 *                           tiles, no Kris pic at all).
 *   G2-K Pack pocket art    pure SHAPE, no code anchor: four consecutive LE
 *                           pointers in [0x4000,0x8000) satisfying
 *                           p0=base+240, p1=base+720, p2=base, p3=base+480.
 *                           Gold: exactly 1 hit. Crystal: exactly 2 (the 2nd
 *                           is PackFGFXPointers, Kris's pack).
 *
 * R5 (design doc): Crystal's CardStatusGFX offset is only PROVEN for Gold (it
 * is read straight out of that ROM's own loader); the Crystal relationship
 * (leaders - 96) is a derivation from Gold's layout applied to a DIFFERENT
 * loader and is UNPROVEN. This module therefore exposes no `cardstatus`
 * field at all -- U3 must either verify it independently or draw the page-1
 * status strip from FontExtra's own literal tiles instead.
 *
 * ---------------------------------------------------------------------------
 * FAIL CLOSED
 * ---------------------------------------------------------------------------
 * `ok` is 1 only when every graphic THIS GEN REQUIRES was located and passed
 * its structural verification:
 *   Gen 1: font, textbox, cardframe, badges, playerpic.
 *   Gen 2: frames, font, fontextra, badges, leaders, cardpic_m, cardgfx,
 *          pack_m -- and, when the ROM is Crystal-shaped (its GetCardPic
 *          signature hit rather than Gold's), additionally cardpic_f, pack_f.
 * A ROM that matches neither gen's full requirement set gets `gen = 0`,
 * every offset 0, `ok = 0`.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT COSTS
 * ---------------------------------------------------------------------------
 * No statics, no globals: RomGbUi is ~76 B, entirely caller-owned (stack or
 * app_arena_acquire()). open()'s scan window is a caller-owned scratch buffer
 * (>= ROM_GBUI_SCRATCH_MIN, 2048 B -- the longest pattern here is 33 B, same
 * generous margin rom_gbsprite.c keeps) used ONLY during open()/open_loc();
 * it is not retained in the struct and may be released or reused afterward.
 * PDNA_GB_UI_NEED (below) is open()'s own measured stack-frame gate.
 */

typedef enum {
  ROM_GBUI_NONE = 0,
  ROM_GBUI_GEN1 = 1,          /* Red / Blue / Yellow                         */
  ROM_GBUI_GEN2 = 2           /* Gold / Silver / Crystal                     */
} RomGbUiGen;

/* open()'s scan window. The longest pattern (the card-frame/card-pic
 * signatures) is 33 B; 2048 halves the number of whole-ROM read passes the
 * same way ROM_GBSPRITE_SCRATCH_MIN does. */
#define ROM_GBUI_SCRATCH_MIN  2048u

/* MEASURED (arm-none-eabi-gcc -mcpu=arm7tdmi -mtune=arm7tdmi -O2
 * -mthumb-interwork -marm -mlong-calls -fstack-usage -c source/rom_gbui.c,
 * 2026-09-09, commit range 3b9882f..a4ce595 + this fix): the deepest chain is
 * rom_gbui_open() -> locate() -> distinct_tiles() -> rd(), 112 + 1000 + 1576
 * + 16 = 2,704 B own-frames-summed. No gb_sprite_gen1_buf call is on this
 * path -- the Gen-1 player pic's 0x77 header byte is checked with a
 * single-byte read; decoding is the CALLER's job (the host test does it,
 * a future screen will), never open()'s. rom_gbui_open_loc()'s own
 * fall-to-a-full-scan path (open_loc -> open -> locate -> distinct_tiles ->
 * rd) adds its own 120-B frame on top: 2,824 B -- still rounds to the same
 * 256-B boundary as the direct-open figure once the shell's frame is added,
 * so either entry point is covered.
 *
 * FIRST MEASUREMENT WAS 5,120 B WORSE: locate()'s own frame was 5,376 B, not
 * the 1,000 B above, because ScanJob's hit array (SCAN_MAX_HITS=128, sized
 * for the ONE signature that needs it -- G1-P's player-pic anchor, ~70-75
 * raw hits before the 0x77 filter) was applied uniformly to all 10 job
 * slots: 10 * 128 * 4 B = 5,120 B, when the other 9 signatures are unique or
 * near-unique in a real ROM and never need more than a handful of slots.
 * FIXED (same commit as this measurement) by giving each job a
 * caller-sized pointer+cap instead of a fixed inline array: 9 jobs at
 * SCAN_SMALL_CAP (8 slots, 288 B total) + one at SCAN_PLAYERPIC_CAP (96
 * slots, 384 B) = 672 B, with IDENTICAL located-offset output on all four of
 * Guy's ROMs (re-verified: tests/host_romgbui_test.c, 86/86 checks).
 *
 * PDNA_GB_UI_NEED = 2,704 (this module's own chain) + 3,568 (the shell's
 * planned frame, Sec 3.4: FIL 600 + scan window 2048 + tilemap 360 + src 360
 * + one expanded tile 128 + RomGbUi 72) = 6,272, rounded UP to the next
 * 256-B boundary = 6,400. Matches PDNA_GB_FETCH_NEED/PDNA_GB_ICON_NEED's
 * order of magnitude (both 6,144) with the small excess this module's own
 * bank-sweep (try_cardframe's up-to-256-bank loop, each iteration re-running
 * distinct_tiles) costs over those two simpler chains. */
#define PDNA_GB_UI_NEED 6400

typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t size;               /* file size in bytes                        */
  uint8_t  gen;                /* RomGbUiGen                                */
  uint8_t  banks;               /* size / 16 KiB                            */
  uint32_t id_hash;             /* FNV-1a of 0x100..0x14F: cache key         */

  /* Gen 1 */
  uint32_t font;                /* G1-F / G2-F: 128 tiles, 1bpp              */
  uint32_t textbox;             /* G1-T: 32 tiles, 2bpp                      */
  uint32_t cardframe;           /* G1-C: 640 B (9+22+1+8 tiles), 2bpp        */
  uint32_t badges;               /* G1-B (64t) / G2-B (44t), 2bpp            */
  uint32_t leaders;              /* G2-L: leader faces, 2bpp (Gen 2 only)    */
  uint32_t playerpic;            /* G1-P: file offset of the Gen-1 pic blob  */

  /* Gen 2 */
  uint32_t frames;               /* G2-R: 9 x 6 tiles, 1bpp                  */
  uint32_t fontextra;            /* G2-E: 32 tiles, 2bpp                     */
  uint32_t cardpic_m;            /* G2-P: Chris, 35 tiles, 2bpp, col-major   */
  uint32_t cardpic_f;            /* G2-P: Kris (Crystal only), else 0        */
  uint32_t cardgfx;              /* G2-P: TrainerCardGFX, 6 tiles, 2bpp      */
  uint32_t pack_m;                /* G2-K: PackGFX, 60 tiles, 2bpp            */
  uint32_t pack_f;                /* G2-K: PackFGFX (Crystal only), else 0    */

  uint8_t  playerpic_bank;       /* the bank playerpic's blob lives in       */
  uint8_t  cardpic_colmajor;     /* 1 = Crystal-shaped (rgbgfx --columns), 0 = Gold (row-major) */
  int      ok;
} RomGbUi;

/* Everything open() discovered, in a form a caller can write to the SD and
 * hand back next time to skip the whole-ROM scan. Validate-on-load is
 * automatic: open_loc() re-checks id_hash, size, and re-runs each field's own
 * structural verifier before trusting it.
 * Field order matches RomGbUi's: font, textbox, cardframe, badges, leaders,
 * playerpic, frames, fontextra, cardpic_m, cardpic_f, cardgfx, pack_m, pack_f. */
typedef struct {
  uint32_t id_hash, size;
  uint8_t  gen, pad[3];
  uint32_t off[13];             /* 60 B */
  uint32_t check;                /* FNV-1a over off[]: catches a corrupted
                                   * cached offset that would otherwise still
                                   * pass its own structural re-verification
                                   * (e.g. font+16 still looks like a font) */
} RomGbUiLoc;

enum {
  ROM_GBUI_OFF_FONT = 0, ROM_GBUI_OFF_TEXTBOX, ROM_GBUI_OFF_CARDFRAME,
  ROM_GBUI_OFF_BADGES, ROM_GBUI_OFF_LEADERS, ROM_GBUI_OFF_PLAYERPIC,
  ROM_GBUI_OFF_FRAMES, ROM_GBUI_OFF_FONTEXTRA, ROM_GBUI_OFF_CARDPIC_M,
  ROM_GBUI_OFF_CARDPIC_F, ROM_GBUI_OFF_CARDGFX, ROM_GBUI_OFF_PACK_M,
  ROM_GBUI_OFF_PACK_F, ROM_GBUI_OFF_COUNT
};

/* Identify the ROM and locate its UI tables. `scratch` is the caller's scan
 * window (>= ROM_GBUI_SCRATCH_MIN; bigger scans faster) and is used ONLY
 * inside this call -- it may be released or reused immediately afterward.
 * Returns 1, or 0 (fail closed) for a non-GB image, a truncated one, or a ROM
 * whose required tables (see the "FAIL CLOSED" note above) are missing,
 * ambiguous, or fail their structural verification. */
int rom_gbui_open(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                  uint8_t* scratch, uint32_t scratch_len);

/* Same, but start from a cached RomGbUiLoc. The header is still parsed and
 * the cache is REJECTED (falling back to a full scan) unless its id_hash and
 * size still match and every offset it names still passes that field's own
 * structural re-verification. */
int rom_gbui_open_loc(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                      uint8_t* scratch, uint32_t scratch_len,
                      const RomGbUiLoc* loc);

/* Snapshot what open() found. Safe to call only when gu->ok. */
void rom_gbui_save_loc(const RomGbUi* gu, RomGbUiLoc* out);

/* Expand one 8x8 tile at ROM `off + index*stride` (stride = 16 for bpp==2,
 * 8 for bpp==1) into `out[64]`, row-major, RGB15, the DMG 4-shade ramp
 * {0xF8,0xA8,0x58,0x10} (8-bit grey, expanded to RGB15 by >>3 per channel).
 *
 * grid_w/grid_h = the block's tile grid; 0,0 = raw storage index, no bound.
 * colmajor: `index` is a ROW-MAJOR DISPLAY index (row*grid_w+col) and the
 * storage index is col*grid_h+row (rgbgfx --columns); requires the grid.
 * Only Crystal's card pics (chris_card.2bpp/kris_card.2bpp, 35 tiles = 5x7)
 * are stored column-major -- Gold's ChrisPicAndTrainerCardGFX pic shares the
 * same 5x7 shape but is plain ROW-major (see RomGbUi.cardpic_colmajor,
 * docs/GB-GAME-SCREENS-DESIGN.md 2.2 G2-P). Every other block this module
 * locates (font, textbox, frames, badges, leaders, pack) is row-major.
 * Returns 1, or 0 for a NULL/unopened `gu`, a bad bpp, an out-of-grid
 * `index`, colmajor requested without a grid, or a read past the ROM. */
int rom_gbui_tile(RomGbUi* gu, uint32_t off, uint32_t index, uint8_t bpp,
                  uint32_t grid_w, uint32_t grid_h, int colmajor, uint16_t out[64]);

/* One glyph of the located font: `ch` is the GAME's own charmap code (A=0x80,
 * a=0xA0, 0=0xF6 -- constants/charmap.asm), tile = ch - 0x80. Returns 1, or 0
 * if `gu` has no font (Gen 1 & Gen 2 both do when `ok`) or `ch` is outside
 * 0x80..0xFF. */
int rom_gbui_glyph(RomGbUi* gu, uint8_t ch, uint16_t out[64]);

#endif /* ROM_GBUI_INCLUDED */
