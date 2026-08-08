#ifndef ROM_SPRITE_INCLUDED          /* not ROM_SPRITE_H — that is the pixel height */
#define ROM_SPRITE_INCLUDED

#include <stdint.h>
#include "rom_map.h"   /* RomCtx / RomReadFn / ROM_BASE */

/*
 * Front / back battle SPRITES and their normal + shiny palettes, read out of the
 * user's own retail ROM.
 *
 * Phase 3 of the ROM-gated build (docs/research-rom-gated-build.md §10) and the
 * biggest single win on the list: mon_front_data + mon_front_shiny_data +
 * mon_back_data are ~3.5 MB of the shipped binary, all of it Nintendo pixels.
 * Reading them from the player's own cartridge instead deletes that from the
 * distribution surface entirely. Nothing is copied anywhere -- the reads are
 * transient, the posture the map viewer has always used.
 *
 * Pure C (no tonc, no FatFs, no GBA headers): all I/O goes through the RomCtx's
 * RomReadFn, so tests/host_romsprite_test.c runs this exact code against the real
 * cartridge dumps on the PC. It uses map_render.c's streaming mr_lz77(), which
 * pulls the compressed bytes through a 64-byte window -- the compressed blob is
 * never staged anywhere.
 *
 * ---- WHERE THE DATA LIVES (measured on all five of Guy's dumps) --------------
 *
 * Game Freak's own index block `GFRomHeader` at ROM+0x100 -- the same block
 * rom_mon.c and rom_text.c already parse -- carries four pointers:
 *     +0x28  const struct CompressedSpriteSheet*   monFrontPics
 *     +0x2C  const struct CompressedSpriteSheet*   monBackPics
 *     +0x30  const struct CompressedSpritePalette* monNormalPalettes
 *     +0x34  const struct CompressedSpritePalette* monShinyPalettes
 *
 *     struct CompressedSpriteSheet   { const u32* data; u16 size; u16 tag; }  8 B
 *     struct CompressedSpritePalette { const u32* data; u16 tag; }            8 B (padded)
 *
 * Ruby/Sapphire predate the header (ARM code sits at 0x100) and FAIL CLOSED here,
 * exactly like rom_mon.c. Their tables were located anyway and are recorded in
 * rom_sprite.c's comment, ready to be pinned the day someone wants them.
 *
 * MEASURED FACTS, none of which are safe to assume:
 *
 *  1. All four tables have exactly 440 entries, tag == index, on E/FR/LG (and on
 *     R/S). Same species axis as the icon tables: 0..411 normal (411 = Chimecho,
 *     the internal ceiling), 412 = the Egg, 413..439 = Unown letters B..'?'.
 *     The SHINY palette table is the exception: its tag is index + 500
 *     (SPECIES_SHINY_TAG), which is why a tag == index check finds three tables
 *     in an Emerald ROM and not four.
 *
 *  2. **The `size` field is a LIE for our purposes.** It reads 2048 in all 440
 *     rows of all six sheet tables -- it is the game's VRAM allocation
 *     (MON_PIC_SIZE), not the length of the data. The real geometry is the LZ77
 *     header's own size, which is 2048, 4096 or 8192 depending on the row. This
 *     module ignores `size` and trusts only the decompressed length.
 *
 *  3. What those extra frames MEAN differs, and getting it wrong shows the wrong
 *     Pokemon:
 *       - Emerald FRONT pics are 4096 B for 439 of 440 rows: 64x128 = TWO
 *         ANIMATION frames (pret's `anim_front.png`). Emerald backs, and both
 *         FRLG tables, are a single 2048 B frame.
 *       - CASTFORM (internal 385) is 8192 B in every table of every game: FOUR
 *         FORMES (Normal / Sunny / Rainy / Snowy), and its palette blob is 128 B
 *         = four palettes, one per forme. Frame index == palette index; pairing
 *         forme 2 with palette 0 renders a blue Castform in white.
 *       - DEOXYS (internal 410) is 4096 B: frame 0 is the NORMAL forme in every
 *         cart, frame 1 is THAT CART'S OWN forme -- Attack in FireRed, Defense in
 *         LeafGreen, Speed in Emerald (verified by rendering all three). See
 *         rom_sprite_deoxys_forme().
 *     Everything else is a plain 64x64.
 *
 *  4. Palettes decompress to 32 B = 16 RGB15 entries (128 B / 4 palettes for
 *     Castform), bit 15 clear in every entry of every palette of all five carts.
 *     Shiny and normal are byte-identical for exactly 26 rows: the 25 unused
 *     internal ids 252..276 and the Egg. Every real species has a distinct shiny.
 *
 * ---- WHAT THIS COSTS IN EWRAM ------------------------------------------------
 * Nothing new. The output is retail's own 4bpp + a 16-colour palette, which is
 * HALF the size of PokeDNA's baked RGB15: the caller hands in the existing
 * `mon_decomp` (source/mon_front.c, 8192 B, shared with mon_back.c) and this
 * module writes at most 8192 B into it -- Castform's four formes, the worst case.
 * A plain 64x64 frame needs 2048 B where the baked path needed all 8192.
 * rom_sprite_to_rgb15() then converts IN PLACE inside that same buffer into
 * exactly what ui_sprite() draws, with no second buffer (see its comment).
 *
 * ---- THE SILENT-GARBAGE HAZARD ----------------------------------------------
 * EZ-Flash `Read_SD_sectors` can return success holding garbage, so a read that
 * becomes pixels must be verified. Verification is ON by default: every pixel and
 * palette payload is decompressed up to three times and accepted only when two
 * consecutive decodes agree on length and on a 32-bit hash of the bytes -- the
 * read-twice-and-compare posture of box_oam.c's icopy_verified, applied where the
 * data lands rather than at every consumer. It costs a second pass over the SD.
 * A fused/cartridge-bus source needs none of it: call rom_sprite_set_verify(rs, 0).
 */

typedef struct RomSprite {
  const RomCtx* rc;
  uint32_t front;      /* FILE offset of gMonFrontPicTable          */
  uint32_t back;       /* FILE offset of gMonBackPicTable           */
  uint32_t npal;       /* FILE offset of gMonPaletteTable           */
  uint32_t spal;       /* FILE offset of gMonShinyPaletteTable      */
  int      ok;         /* 1 = header parsed and all four are sane   */
  int      verify;     /* 1 = decode-twice-and-compare (default 1)  */
} RomSprite;

#define ROM_SPRITE_W            64
#define ROM_SPRITE_H            64
#define ROM_SPRITE_FRAME_BYTES  2048   /* one 64x64 4bpp frame                     */
#define ROM_SPRITE_MAX_FRAMES   4      /* Castform's four formes                   */
#define ROM_SPRITE_BUF_BYTES    8192   /* worst-case pic buffer == sizeof mon_decomp */
#define ROM_SPRITE_PIXELS       4096   /* 64*64, after rom_sprite_to_rgb15         */
#define ROM_SPRITE_PAL_BYTES    32     /* 16 RGB15 entries                         */

/* Internal species ids with a forme axis in the sprite tables (stable in all five
 * games; the same ids data_tables.c calls CASTFORM and DEOXYS). */
#define ROM_SPRITE_CASTFORM     385
#define ROM_SPRITE_DEOXYS       410
#define ROM_SPRITE_EGG          412
#define ROM_SPRITE_UNOWN        201
#define ROM_SPRITE_ENTRIES      440

typedef enum { ROM_SPRITE_FRONT = 0, ROM_SPRITE_BACK = 1 } RomSpriteSide;

/* What the frames in a blob are. Report it; do not guess from the count. */
typedef enum {
  ROM_SPRITE_SINGLE = 0,   /* one 64x64 frame                                    */
  ROM_SPRITE_ANIM,         /* two ANIMATION poses (Emerald fronts)               */
  ROM_SPRITE_FORME         /* frames are alternate FORMES (Castform 4, Deoxys 2) */
} RomSpriteFrameKind;

typedef struct RomSpritePic {
  uint32_t bytes;       /* decompressed bytes written to dst (2048/4096/8192)     */
  uint8_t  frames;      /* bytes / 2048                                           */
  uint8_t  frame;       /* the frame index that answers the request               */
  uint8_t  kind;        /* RomSpriteFrameKind                                     */
  uint8_t  form_exact;  /* 1 = `form` is really in this ROM; 0 = a stand-in       */
} RomSpritePic;

/* Parse the GF header and sanity-check all four table pointers. Returns 1 on
 * success; 0 (fail closed) for Ruby/Sapphire, an unknown/hacked ROM, or any
 * pointer that does not leave room for 440 entries inside the image. */
int rom_sprite_open(RomSprite* rs, const RomCtx* rc);

/* Turn the decode-twice-and-compare verification off (fused/cartridge-bus source)
 * or back on. Default is ON. */
void rom_sprite_set_verify(RomSprite* rs, int on);

/*
 * Decompress one Pokemon's picture into `dst` as retail 4bpp (8x8 tiles, GBA 1D
 * order, low nibble = left pixel), and describe what arrived in *out.
 *
 * `dst_cap` must be at least ROM_SPRITE_BUF_BYTES for a species that may carry
 * formes; 2048 is enough only if you already know the row is single-frame, and a
 * short buffer FAILS rather than truncating.
 *
 * `species` is the ordinary internal Gen-3 id, 0..439 (412 = Egg; 413..439 are the
 * Unown letters and are also reachable the sane way, below). `form` is PokeDNA's
 * own form axis, matching mon_front_for_form():
 *     Unown    (201)  form 0..27  = letters A..'?'  -> its own table row
 *     Deoxys   (410)  form 0..3   = Normal/Attack/Defense/Speed
 *     Castform (385)  form 0..3   = Normal/Sunny/Rainy/Snowy
 *     everything else: `form` must be 0.
 *
 * out->frame is the frame the CALLER should display -- already resolved for the
 * forme species, so a caller that only ever shows out->frame is always right.
 * out->form_exact is 0 when this cart cannot show the requested forme (asking a
 * FireRed for Speed Deoxys): the Normal forme is served instead, and an honest UI
 * should say so rather than pretend. Nothing else ever sets form_exact to 0.
 *
 * Returns 1 on success; 0 -- with *out zeroed and dst's contents UNDEFINED -- for
 * a bad id, an out-of-range form, a pointer outside the image, a decompressed
 * length that is not a whole number of 2048 B frames, a buffer too small, or a
 * read that failed verification.
 */
int rom_sprite_pic(const RomSprite* rs, RomSpriteSide side, uint16_t species,
                   uint8_t form, uint8_t* dst, uint32_t dst_cap, RomSpritePic* out);

/* The 16-entry RGB15 palette for that species/form, normal or shiny. Castform's
 * palette is picked per forme (its blob holds four); every other species has one
 * palette shared by all its frames. Returns 1, or 0 on any failure. */
int rom_sprite_pal(const RomSprite* rs, uint16_t species, uint8_t form, int shiny,
                   uint16_t dst[16]);

/* Which Deoxys forme this cart's frame 1 holds: 1 Attack (FireRed), 2 Defense
 * (LeafGreen), 3 Speed (Emerald), 0 = Normal only. Every cart's frame 0 is Normal,
 * so ONE ROM gives two formes and three ROMs complete the set. */
int rom_sprite_deoxys_forme(const RomSprite* rs);

/*
 * Convert one frame of a decoded pic into what ui_sprite() draws -- 4096 RGB15
 * pixels, 0 = transparent, 0x8000|RGB15 = opaque -- IN PLACE, inside the same
 * ROM_SPRITE_BUF_BYTES buffer, with no second buffer and no new EWRAM.
 *
 * `buf` must be the ROM_SPRITE_BUF_BYTES (8192 B) buffer rom_sprite_pic() filled
 * -- i.e. mon_decomp -- 4-byte aligned, whatever the decoded length was. `frame`
 * is normally the RomSpritePic's `frame`. After it returns, (const uint16_t*)buf
 * is exactly the pointer mon_front_for_form() used to return.
 *
 * It works by staging the de-tiled frame in the buffer's last quarter and then
 * expanding forward over the whole buffer; the write pointer provably never
 * overtakes the read pointer. Everything outside the chosen frame is destroyed.
 */
/* Expand the decoded 4bpp picture in `buf` to the RGB15+opacity format ui_sprite()
 * draws, IN PLACE. Always writes ROM_SPRITE_BUF_BYTES, so `cap` must be at least
 * that — a shorter buffer is REFUSED (returns 0), never truncated. Returns 1 on
 * success. */
int rom_sprite_to_rgb15(void* buf, uint32_t cap, uint8_t frame, const uint16_t pal[16]);

#endif /* ROM_SPRITE_INCLUDED */
