#ifndef ROM_GBSPRITE_INCLUDED
#define ROM_GBSPRITE_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#include "gb_sprite_codec.h"   /* module A: GbReadFn, GbSprite, gb_sprite_gen1/2 */

/*
 * Gen-1 and Gen-2 Pokemon FRONT/BACK sprites, read live out of the user's own
 * Game Boy cartridge dumps (Red/Blue, Yellow, Gold/Silver, Crystal).
 *
 * WHY: "a Pokemon is drawn in the art of the generation it came from". A Gen-1
 * Mew shows the Red/Blue Mew, a Gen-2 Unown shows the Gold/Crystal one, a native
 * Gen-3 mon shows the Gen-3 sprite -- so in the bank, where mons from all three
 * eras sit side by side, provenance is something you SEE while deciding what to
 * transfer. This module is the Gen-1/2 half of that; source/rom_sprite.* is the
 * Gen-3 half and this file deliberately mirrors its shape (open / fetch into a
 * caller buffer / expand to RGB15) so the UI can treat all three alike.
 *
 * PokeDNA ships NO Game Freak art. Everything here is a transient read of a file
 * the user already owns, exactly the posture rom_mon.c / rom_sprite.c take.
 *
 * PURE C: no tonc, no FatFs, no GBA headers. All I/O goes through the caller's
 * GbReadFn, so tests/host_romgbsprite_test.c runs this exact code on the PC
 * against the real cartridge dumps.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DATA IS, AND HOW THIS MODULE FINDS IT
 * ---------------------------------------------------------------------------
 * Nothing below is pinned to a per-game constant unless it is CODE rather than
 * data. Every table is located BY SHAPE in the ROM the user handed us, the way
 * tools/gen_encounters.py locates gWildMonHeaders, and every hit must be unique.
 * The addresses this found on Guy's four dumps are recorded here so a human can
 * sanity-check them (they are NOT used as inputs):
 *
 *              Red.gb      Yellow.gb   Gold.gbc    Crystal.gbc
 *   BaseStats  0x383DE     0x383DE     0x51B0B     0x51424
 *   PokedexOrder 0x41024   0x410B1     -           -
 *   MewBaseStats 0x0425B   (in table)  -           -
 *   PicPointers -          -           0x48000     0x120000
 *   UnownPtrs   -          -           0x07C000    0x124000
 *   Palettes    -          -           0x0AD3D     0x0A8CE
 *
 * and the PicsBanks maps it derives (stored bank byte -> real bank):
 *   Crystal  0x12..0x23 -> 0x48..0x59      (contiguous: real = stored + 0x36)
 *   Gold     0x12->0x12  0x13->0x1F  0x14->0x20  0x15..0x1E identity
 *            0x1F->0x2E                    (NOT contiguous — see below)
 *
 * GEN 1 (pokered)
 *   * BaseStats is indexed by NATIONAL DEX number, not by the internal index:
 *     home/pokemon.asm GetMonHeader does `predef IndexToPokedex` then
 *     `dec a / ld bc, BASE_DATA_SIZE / AddNTimes`. Entry i therefore begins with
 *     the byte i+1, which is the shape we scan for (28-byte stride, 150 rows).
 *     Layout from constants/pokemon_data_constants.asm:
 *       +0 dex, +1..5 stats, +6..7 types, +8 catch, +9 exp,
 *       +10 PIC SIZE, +11..12 front pointer, +13..14 back pointer, ... (28 B)
 *   * The pic pointers are bank-relative; the BANK comes from a hard ladder on
 *     the INTERNAL index in home/pics.asm UncompressMonSprite:
 *       idx < 0x1F -> 0x09, < 0x4A -> 0x0A, < 0x74 -> 0x0B, < 0x99 -> 0x0C,
 *       else 0x0D;  MEW (idx 0x15) -> BANK(MewPicFront).
 *     That is CODE, so it is pinned -- but it is checked, not trusted, on EVERY
 *     fetch: a Gen-1 pic carries its own dimension byte and it must equal the
 *     base-stats one. A wrong bank hands back a perfectly well-formed picture of
 *     the WRONG POKEMON, and this is what turns that into an error. (Verified:
 *     151/151 fronts and 151/151 backs in both Red and Yellow, and a mutation
 *     that deletes the check is caught by the test's negative control.)
 *   * dex -> internal index comes from the ROM's own 190-byte PokedexOrder table
 *     (data/pokemon/dex_order.asm), located by shape: 151 non-zero entries that
 *     are a bijection onto 1..151, plus 39 MissingNo zeros. Unique in both ROMs.
 *   * MEW IS NOT UNIFORM. In Red/Blue the table stops at 150 rows and Mew's
 *     record sits alone in bank 1 next to its pics (data/pokemon/mew.asm: "a kind
 *     of prank"); in Yellow it is an ordinary 151st row and there is NO Mew bank
 *     special case. This module detects which by looking at row 151 rather than
 *     by sniffing the title, so Blue and localised builds work too. Mew's pic
 *     bank is then the bank its standalone record lives in -- discovered, not
 *     pinned (it lands on bank 1, as the decomp says).
 *   * Gen-1 fronts are 5x5/6x6/7x7 tiles; every Gen-1 BACK is 4x4.
 *
 * GEN 2 (pokecrystal)
 *   * PokemonPicPointers (data/pokemon/pic_pointers.asm) is 251 x 6 bytes:
 *     [bank-PICS_FIX][lo][hi] for the front, then the same for the back
 *     (macros/data.asm dba_pic). Located by shape; entry 200 (Unown) is
 *     FF FF FF FF FF FF because Unown pics live in their own table, so the scan
 *     expects exactly one hole and it must be at index 200.
 *   * The stored bank byte is NOT the bank. engine/gfx/load_pics.asm FixPicBank
 *     indexes a PicsBanks[] table with (stored - stored_of_Pics_1). Crystal's
 *     PicsBanks happens to be contiguous (real = stored + 0x36); GOLD'S IS NOT
 *     (0x13 -> 0x1F and 0x14 -> 0x20 while everything else is identity, and
 *     0x1F -> 0x2E). So this module starts from the one fact that is structural
 *     -- "Pic Pointers" shares its bank with "Pics 1", i.e. PicsBanks[0] is the
 *     bank the pointer table itself is in -- and REPAIRS any stored value whose
 *     default fails, by trying EVERY bank and demanding a single winner that
 *     satisfies a dozen different pictures (see g2_plausible in the .c: "it
 *     decoded" is nowhere near enough, and that is measured). The repaired map is
 *     cached in the struct, so Crystal never sweeps at all and Gold pays for its
 *     three odd values once.
 *   * UnownPicPointers is 26 x 6 at the SAME bank-relative address as
 *     PokemonPicPointers (gfx/pics.asm asserts it), in the bank of "Pics 2" =
 *     bank_map[1]. Verified: all 26 Crystal letters decode to sprites that match
 *     the decomp's own PNGs byte for byte.
 *   * PokemonPalettes is inside engine/gfx/color.asm, so no data shape reaches
 *     it. It is located from the CODE that reads it -- _GetMonPalettePointer is
 *     `ld l,a / ld h,0 / add hl,hl x3 / ld bc,PokemonPalettes / add hl,bc / ret`
 *     = 6F 26 00 29 29 29 01 lo hi 09 C9 -- and the candidate is then required to
 *     pass the table's own shape test (252 entries x 4 GB colours, bit 15 clear,
 *     never pure white or black, entry 0's normal pair equal to its shiny pair).
 *     Exactly one of the two signature hits survives in each ROM.
 *     Entry layout (index = species, entry 0 is a dummy): normal c1, normal c2,
 *     shiny c1, shiny c2. The displayed 4-colour palette is
 *     [PALRGB_WHITE, c1, c2, BLACK] -- engine/gfx/color.asm
 *     LoadPalette_White_Col1_Col2_Black.
 *   * Gen-2 pics are square, size from BaseData +17 (low nibble, 5/6/7); backs
 *     are always 6x6. A Crystal front decompresses to MORE than the static pic
 *     (the animation frames are appended); Gold's is exactly the static pic.
 *     Either way only the first w*h tiles are the sprite.
 *
 * ---------------------------------------------------------------------------
 * FAIL CLOSED
 * ---------------------------------------------------------------------------
 * open() refuses anything that is not a real Game Boy ROM before it looks at a
 * single table: the 48-byte boot logo at 0x104 must hash to the one value every
 * licensed cartridge has, the header checksum at 0x14D must be correct, the size
 * code at 0x148 must agree with the file length, and the file must be a whole
 * number of 16 KiB banks. A Gen-3 .gba, a .sav and a truncated image all fail. So
 * does a ROM whose tables are not found, or found more than once.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT COSTS
 * ---------------------------------------------------------------------------
 * No new statics anywhere. The two buffers are the caller's:
 *   - a GbSprite (3 928 B, module A's) receives the decoded picture;
 *   - an RGB15 destination of w*h pixels (3 136 max, so 6 272 B) receives the
 *     drawable form. mon_decomp (8 192 B) is the obvious home for the second;
 *     the first wants app_arena_acquire(), because it and mon_decomp together are
 *     ~10 KB and the hardware build has ~1.5 KB of EWRAM going spare.
 * The RomGbSprite itself is 336 B (the 190-byte PokedexOrder copy dominates, and
 * it is Gen-1 only), the RomGbSpriteLoc cache 260 B, and rom_gbsprite.o is 6.4 KB
 * of Thumb with ZERO .data and ZERO .bss -- measured, not estimated.
 *
 * open() streams the whole ROM once through the caller's scratch to find the
 * tables -- 1 MB (Gen 1) or 2 MB (Gen 2) of sequential reads, in ONE pass for all
 * six patterns. Do it once: rom_gbsprite_save_loc() hands you everything it found
 * and rom_gbsprite_open_loc() re-validates a cached copy in a handful of reads.
 *
 * Everything past open() is a handful of short reads: module A's codec pulls the
 * compressed stream through its own 64-byte window, so no staging buffer exists
 * on this side at all.
 *
 * GbReadFn is module A's typedef (same shape as rom_map.h's RomReadFn). On
 * hardware it is the callback that touches the SD, so IT is what must obey the
 * OS-mode rule.
 */

typedef enum {
  GB_ROM_NONE = 0,
  GB_ROM_GEN1,          /* Red / Blue / Yellow                                 */
  GB_ROM_GEN2           /* Gold / Silver / Crystal                             */
} GbRomGen;

typedef enum { ROM_GBSPRITE_FRONT = 0, ROM_GBSPRITE_BACK = 1 } RomGbSide;

#ifdef ROM_GBSPRITE_JOB_COUNTERS
/* BACKLOG #185 Step 1 / T2: six per-job scan_multi callback-INVOCATION counters
 * (not hits -- every time a job's gate lets a position through to its cb, win or
 * lose), in this order, for a host benchmark/test to reset and read. Only exists
 * when the host test build defines ROM_GBSPRITE_JOB_COUNTERS on the compile line
 * -- the GBA Makefile never does, so this costs the cart build nothing. */
enum { ROM_GBSPRITE_JOB_G1_BS = 0, ROM_GBSPRITE_JOB_G1_DEX, ROM_GBSPRITE_JOB_G1_MEW,
       ROM_GBSPRITE_JOB_G2_BD, ROM_GBSPRITE_JOB_G2_PP, ROM_GBSPRITE_JOB_G2_PAL };
extern uint32_t g_rgs_cb_calls[6];
#endif

/* The RGB15 destination rom_gbsprite_to_rgb15() needs, at worst (7x7 tiles). */
#define ROM_GBSPRITE_MAX_PIXELS    GB_SPRITE_MAX_PX          /* 56*56 = 3136   */
#define ROM_GBSPRITE_RGB15_BYTES   (GB_SPRITE_MAX_PX * 2u)   /* 6272 B         */
/* open()'s scan window. It must clear the longest pattern (201 pic-pointer
 * entries = 1206 B) with room to spare; 4096 halves the number of reads. */
#define ROM_GBSPRITE_SCRATCH_MIN   2048
#define ROM_GBSPRITE_BANKMAP       32      /* distinct stored bank bytes       */
#define ROM_GBSPRITE_UNOWN_DEX     201
#define ROM_GBSPRITE_UNOWN_FORMS   26      /* A..Z                             */

/* BACKLOG #205: which path an open served from -- read by the delta bench's cold-locate log
 * line, so a frame count is never ambiguous. NONE = open failed / not yet opened. */
typedef enum {
  ROM_GBSPRITE_SRC_NONE = 0,
  ROM_GBSPRITE_SRC_CACHE,     /* the caller's RomGbSpriteLoc (.loc file / fused / session) */
  ROM_GBSPRITE_SRC_TABLE,     /* the compiled-in known-ROM table (rom_gbsprite_known.h)     */
  ROM_GBSPRITE_SRC_SCAN       /* a real whole-ROM scan                                      */
} RomGbSpriteSrc;

typedef struct RomGbSprite {
  GbReadFn read;
  void*    ctx;
  uint32_t size;              /* file size in bytes                            */
  uint8_t* scratch;           /* caller-owned scan window, open() only         */
  uint32_t scratch_len;       /* >= ROM_GBSPRITE_SCRATCH_MIN                   */

  GbRomGen gen;
  char     title[16];         /* 0x134..0x142, NUL-terminated                  */
  uint8_t  cgb;               /* 0x143                                         */
  uint8_t  cart_type;         /* 0x147                                         */
  uint8_t  rom_size_code;     /* 0x148                                         */
  uint8_t  version;           /* 0x14C                                         */
  uint16_t global_checksum;   /* 0x14E/0x14F, big-endian in the header         */
  uint32_t id_hash;           /* FNV-1a of 0x100..0x14F: cache key             */
  uint8_t  banks;             /* size / 16 KiB                                 */
  uint8_t  src;               /* RomGbSpriteSrc: WHICH path served the last open (BACKLOG #205) */

  /* Gen 1 */
  uint32_t base_stats;        /* file offset of BaseStats                      */
  uint32_t mew_stats;         /* file offset of MewBaseStats, 0 if in-table    */
  uint8_t  mew_bank;
  uint8_t  dex_order[190];    /* PokedexOrder: internal index i+1 -> dex       */

  /* Gen 2 */
  uint32_t base_data;         /* file offset of BaseData                       */
  uint32_t pic_ptrs;          /* file offset of PokemonPicPointers             */
  uint32_t unown_ptrs;        /* file offset of UnownPicPointers               */
  uint32_t palettes;          /* file offset of PokemonPalettes                */
  uint8_t  stored_lo;         /* smallest stored bank byte = index base        */
  uint8_t  bank_map[ROM_GBSPRITE_BANKMAP];  /* stored-lo -> real bank, 0 = TBD */
  uint32_t bank_ok;           /* bit i set once bank_map[i] survived 3 pics    */

  int      ok;
} RomGbSprite;

/* Everything open() discovered, in a form a caller can write to the SD and hand
 * back next time to skip the whole-ROM scan. Validate-on-load is automatic. */
typedef struct RomGbSpriteLoc {
  uint32_t id_hash;
  uint32_t size;
  uint8_t  gen;
  uint8_t  mew_bank;
  uint8_t  stored_lo;
  uint8_t  pad;
  uint32_t base_stats, mew_stats, base_data, pic_ptrs, unown_ptrs, palettes;
  uint8_t  dex_order[190];
  uint8_t  bank_map[ROM_GBSPRITE_BANKMAP];
} RomGbSpriteLoc;

typedef struct RomGbPic {
  uint8_t  gen;          /* 1 or 2                                             */
  uint8_t  wt, ht;       /* tiles                                              */
  uint16_t w, h;         /* pixels = wt*8, ht*8                                */
  uint32_t pixels;       /* w * h -- the length of the GbSprite's px[]         */
  uint32_t consumed;     /* compressed bytes the codec read                    */
} RomGbPic;

/* Identify the ROM and locate its tables. `scratch` is the caller's scan window
 * (>= ROM_GBSPRITE_SCRATCH_MIN; bigger scans faster) and is used ONLY inside the
 * open calls -- it may be released afterwards, and it may be the same buffer a
 * fetch later writes to. Returns 1, or 0 (fail closed) for a non-GB image, a
 * truncated one, or a ROM whose tables are missing or ambiguous.
 *
 * `gen_hint` (BACKLOG #185 F1): GB_ROM_NONE (0) runs all six scan jobs and
 * identifies whichever generation matches -- the original, still-default
 * behaviour every gen-less caller (pdna_gen12.c's session opens, the PDNA_DELTA
 * fetch path, every generic test/tool) keeps using unchanged. GB_ROM_GEN1 or
 * GB_ROM_GEN2 restricts the scan to that generation's three jobs ONLY (half the
 * per-position work, the whole point of #185) and first cross-checks the
 * already-parsed header (size + cgb flag) against the hint -- a ROM whose header
 * says the OTHER generation is refused (fail closed, same as any other locate
 * failure) before a single scan byte is offered to a callback, so a Gen-2 ROM
 * registered as Gen 1 cannot silently "succeed" by being fed only Gen-1 jobs. */
int rom_gbsprite_open(RomGbSprite* gs, GbReadFn read, void* ctx, uint32_t size,
                      uint8_t* scratch, uint32_t scratch_len, uint8_t gen_hint);

/* Same, but start from a cached RomGbSpriteLoc. The header is still parsed and
 * the cache is REJECTED (falling back to a full scan) unless its id_hash, size
 * and every offset it names still check out -- and, with a non-NONE `gen_hint`,
 * unless the cache's own recorded gen matches the hint too (a foreign-gen cache
 * cannot leak through as a silent success any more than a foreign-gen scan can). */
int rom_gbsprite_open_loc(RomGbSprite* gs, GbReadFn read, void* ctx, uint32_t size,
                          uint8_t* scratch, uint32_t scratch_len,
                          const RomGbSpriteLoc* loc, uint8_t gen_hint);

/* Snapshot what open() found. Safe to call only when gs->ok. */
void rom_gbsprite_save_loc(const RomGbSprite* gs, RomGbSpriteLoc* out);

/*
 * Decompress one Pokemon's picture into the caller's GbSprite -- an indexed
 * bitmap, px[y*w + x] = 0..3 with 0 lightest, packed row-major with stride w --
 * and describe it in *info.
 *
 * `dex` is the NATIONAL dex number: 1..151 for a Gen-1 ROM, 1..251 for a Gen-2
 * one. `form` is the Unown letter 0..25 (A..Z) when dex == 201 on a Gen-2 ROM,
 * and MUST be 0 otherwise -- Gen 1 has no forms and a Gen-2 non-Unown species has
 * exactly one sprite.
 *
 * Returns 1; or 0 -- with *info zeroed and the GbSprite UNDEFINED -- for a bad
 * dex/form, a pointer that leaves the image, a picture whose dimensions disagree
 * with the base stats (i.e. the bank is wrong), a stream that runs past the end of
 * its own 16 KiB bank, or anything the codec rejects.
 */
int rom_gbsprite_pic(RomGbSprite* gs, RomGbSide side, uint16_t dex, uint8_t form,
                     GbSprite* out, RomGbPic* info);

/* Same decode, into caller-owned `px` (>= GB_SPRITE_MAX_PX bytes) and `work`
 * (>= GB_SPRITE_WORK bytes) instead of a GbSprite -- rom_gbsprite_pic() above is now
 * a thin wrapper over this (out->px, out->work). Exists so a caller can place px/work
 * inside a buffer it plans to reuse for something else afterward (gb_art_source.c:
 * both live inside mon_decomp, freeing it from ALSO needing a 3,928 B GbSprite on the
 * stack -- see the "does NOT work in place" note below, now qualified). `px` and
 * `work` need not be adjacent and may alias a THIRD region the caller will overwrite
 * next (rom_gbsprite_to_rgb15_inplace() is what makes that safe). Same
 * errors/semantics as rom_gbsprite_pic(). */
int rom_gbsprite_pic_buf(RomGbSprite* gs, RomGbSide side, uint16_t dex, uint8_t form,
                         uint8_t* px, uint8_t* work, RomGbPic* info);

/*
 * The 4-entry RGB15 palette for that species. Gen 2 reads the cart's own colours
 * (normal or shiny); Gen 1 has none, so it answers the four DMG greys -- which is
 * the point, not a limitation: a Red/Blue mon should LOOK like a Red/Blue mon.
 * Entry 0 is the background and rom_gbsprite_to_rgb15() renders it transparent.
 * `shiny` is ignored on Gen 1. Returns 1, or 0 on any failure.
 */
int rom_gbsprite_pal(const RomGbSprite* gs, uint16_t dex, int shiny, uint16_t dst[4]);

/*
 * Turn a decoded picture into what ui_sprite() draws: w*h RGB15 pixels,
 * row-major, 0 = transparent and 0x8000|RGB15 = opaque -- byte-for-byte the
 * format rom_sprite_to_rgb15() produces for Gen 3, so the UI needs no new
 * blitter and a Gen-1, Gen-2 and Gen-3 sprite can sit in the same bank row.
 *
 * `dst` needs s->w * s->h entries (3136 at worst, i.e. 6 272 B: mon_decomp
 * holds it). This does NOT work in place if `dst` is `s->px` reinterpreted --
 * the general two-buffer case here is unrelated to a SPECIFIC shared-buffer layout
 * that IS safe; see rom_gbsprite_to_rgb15_inplace() immediately below for that one.
 * A short `dst_pixels` is REFUSED, never truncated. Returns 1, or 0.
 */
int rom_gbsprite_to_rgb15(const GbSprite* s, const uint16_t pal[4],
                          uint16_t* dst, uint32_t dst_pixels);

/*
 * The SAME conversion, but IN PLACE within one buffer: RGB15 output at
 * buf[0 .. 2*w*h), read from indexed pixels at buf[px_off .. px_off + w*h). Safe
 * IFF px_off >= GB_SPRITE_MAX_PX (3,136) -- the loop runs i = 0 .. w*h-1 ascending
 * ("front to back" through the destination, reading from "the back" of the shared
 * buffer). Let n = w*h (n <= 3,136 always). For i < n-1, writing dst[i] (bytes
 * [2i, 2i+1]) must not touch the NEXT pixel still unread, px[i+1] (byte
 * px_off+i+1) -- the closest not-yet-read byte, since ascending order has already
 * consumed px[0..i]. That needs 2i+1 < px_off+i+1, i.e. i < px_off; since i <= n-2
 * <= 3,134 and px_off >= 3,136, that holds with margin to spare. The single
 * coincidence is i == n-1 at the worst-case size (px_off == GB_SPRITE_MAX_PX,
 * n == 3,136 exactly): buf[px_off+i] and buf[2i+1] are the SAME byte, which is
 * why the loop body reads px[i] into a local BEFORE writing dst[i] -- ordinary
 * "read source, then write destination" is already enough; no special-casing that
 * index is needed, only NOT reordering the two inside one iteration.
 * `px_off` < GB_SPRITE_MAX_PX is refused (returns 0) -- the margin argument above
 * requires it. Returns 1, or 0 on bad geometry or a bad pixel value (index > 3).
 */
int rom_gbsprite_to_rgb15_inplace(uint8_t* buf, uint32_t px_off, uint8_t w, uint8_t h,
                                  const uint16_t pal[4]);

#endif /* ROM_GBSPRITE_INCLUDED */
