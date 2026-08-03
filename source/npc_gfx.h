#ifndef NPC_GFX_H
#define NPC_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"

/*
 * Gen-3 overworld NPC sprites, read live out of the USER'S OWN Pokemon ROM.
 *
 * rom_map.c already answers "which NPCs stand on this map, and where"
 * (RomObjectEvent.graphics_id / x / y). This module answers the missing half:
 * graphics_id -> PIXELS. Nothing is redistributed; every byte is fetched
 * transiently through the caller's RomReadFn from the .gba file the user already
 * owns. PokeDNA ships no Nintendo art or tables.
 *
 * Pure C, exactly like rom_map/map_render: no tonc, no GBA headers, no BIOS calls,
 * no malloc, explicit little-endian reads, every followed pointer gated by
 * rom_ptr_ok(). tests/host_npcgfx_test.c compiles and runs this same code on a PC
 * against real retail ROMs.
 *
 * ---- the data path (verified byte-for-byte against Emerald/Ruby/FireRed) ------
 *
 *   RomCtx.gfx_info_ptrs  = gObjectEventGraphicsInfoPointers
 *      -> u32[NUM_OBJ_EVENT_GFX], indexed by graphics_id
 *      -> struct ObjectEventGraphicsInfo, 0x24 bytes, IDENTICAL in RSE and FRLG:
 *
 *           0x00 u16 tileTag                (0xFFFF/TAG_NONE for every overworld obj)
 *           0x02 u16 paletteTag             (0x1100..0x11FF)
 *           0x04 u16 reflectionPaletteTag
 *           0x06 u16 size                   ** DO NOT TRUST - see below **
 *           0x08 s16 width                  pixels (8/16/32/48/64/88/96/128)
 *           0x0A s16 height                 pixels
 *           0x0C u8  paletteSlot:4          which of the 16 OBJ palettes the game uses
 *                    shadowSize:2
 *                    inanimate:1
 *                    disableReflectionPaletteLoad:1
 *           0x0D u8  tracks
 *           0x0E     (2 bytes of struct padding, always 0)
 *           0x10 u32 *oam                   -> struct OamData
 *           0x14 u32 *subspriteTables
 *           0x18 u32 **anims                -> array of ptr to union AnimCmd
 *           0x1C u32 *images                -> struct SpriteFrameImage[]
 *           0x20 u32 **affineAnims
 *
 *      -> images[frame] = struct SpriteFrameImage { const void *data; u16 size; }
 *         8 bytes each (6 used + 2 padding). `data` points at RAW, UNCOMPRESSED 4bpp
 *         char data; `size` is that frame's byte length.
 *
 * `info.size` at 0x06 is NOT the frame size. It disagrees with width*height/2 for
 * Brendan/May/BerryTree in RSE and for Red/Green in FRLG (e.g. 512 declared for a
 * 16x32 = 256-byte frame). images[frame].size is the authority and matches
 * width*height/2 for every id in all three ROMs but one (Emerald/Ruby id 62,
 * BERRY_TREE_LATE_STAGES, whose pic table mixes 16x16 and 16x32 frames). This module
 * requires images[frame].size == width*height/2 and refuses the frame otherwise.
 *
 * ---- NO LZ77 ANYWHERE ON THIS PATH ------------------------------------------
 * Object-event frames are plain 4bpp (that is why the decomp's overworld_frame()
 * macro can compute a frame's address with (w*h*frame*64)/2 arithmetic), and
 * sObjectEventSpritePalettes is `struct SpritePalette` (raw), not
 * CompressedSpritePalette. So this module deliberately calls no decompressor at all
 * - not mr_lz77, not the BIOS. If a future caller sees an LZ77 header here it means
 * the pointer is wrong, not that decompression is needed.
 *
 * ---- palettes ---------------------------------------------------------------
 * paletteTag is looked up in sObjectEventSpritePalettes[] (an array of
 * { const u16 *data; u16 tag; }, 8 bytes each) which gives 16 raw BGR555 colours.
 * That table's address is NOT in RomCtx, so npc_gfx_open() FINDS it: it scans a small
 * window around gObjectEventGraphicsInfoPointers for >= 8 consecutive well-formed
 * records whose first eight tags are exactly 0x1103,04,05,06,07,08,09,0A (NPC 1-4 and
 * their four reflections - the identical opening run in RSE and FRLG) and takes the
 * longest such run. Measured: Emerald 0x0850BBC8 (35 entries), Ruby rev2 0x08373794
 * (27), FireRed rev1 0x083A51C8 (18); all three sit 0x53A8-0x65A8 bytes AFTER the
 * graphics-info pointer table. Deriving it beats hardcoding 11 per-revision
 * constants for versions we cannot all test, and it fails cleanly on a ROM hack.
 *
 * ---- per-game differences that matter ---------------------------------------
 *   - NUM_OBJ_EVENT_GFX: Emerald 239, Ruby/Sapphire 218, FireRed/LeafGreen 152.
 *     Ids at or above that are clamped by the game to a default NPC; we REFUSE them.
 *   - Palette count (distinct paletteTags actually referenced): Emerald 25,
 *     Ruby 17, FireRed 8 — FRLG's four shared NPC palettes carry 108 of its 152
 *     sprites. Per MAP it is far smaller: no retail map needs more than 5 (RSE) or
 *     4 (FRLG) distinct object palettes, so the GBA's 16 OBJ palette slots are not a
 *     constraint. See the budget section of tests/host_npcgfx_test.c.
 *   - Emerald only: graphics_id 69 (OBJ_EVENT_GFX_BARD) is re-pointed at runtime
 *     through gMauvilleOldManGraphicsInfoPointers[GetCurrentMauvilleOldMan()]; the
 *     static table entry is the Bard, which is the default. Flagged as `dynamic`.
 *   - RSE only: ids 60/61/62 are berry trees; the game swaps in the pic table for
 *     whichever berry is planted (the template's trainerRange_berryTreeId picks it).
 *     The static entry is the Pecha tree. Flagged as `dynamic`.
 *   - All games: graphics_id 240..255 are OBJ_EVENT_GFX_VAR_0..F, indirected through
 *     RAM vars at runtime. There is NOTHING in ROM to draw; npc_gfx refuses them with
 *     NPC_ERR_DYNAMIC_VAR so the caller can draw a placeholder instead of garbage.
 *   - FRLG only: an ObjectEventTemplate with kind == 255 (OBJ_KIND_CLONE) mirrors
 *     another object; its own graphics_id still resolves here, but the caller should
 *     prefer the target object's sprite. rom_map exposes that as RomObjectEvent.kind.
 */

/* Biggest object-event sprite in any retail Gen-3 ROM: FireRed's SS Anne, 128x64,
 * 4bpp -> 4096 bytes. Emerald and Ruby top out at 96 wide (the SS Tidal, 96x40) and
 * 64 tall (the legendaries, 64x64 = 2048 bytes). Anything outside these bounds is
 * rejected as NPC_ERR_BAD_DIMS rather than trusted. */
#define NPC_GFX_MAX_W      128
#define NPC_GFX_MAX_H      64
#define NPC_GFX_MAX_BYTES  ((NPC_GFX_MAX_W * NPC_GFX_MAX_H) / 2)   /* 4096 */

/* The four cardinal "standing" animations, in the game's own ANIM_STD_FACE_* order
 * (identical in RSE and FRLG). EAST is normally the WEST frame h-flipped, which is
 * why NpcGfxInfo carries an hflip bit rather than a separate frame. */
#define NPC_DIR_SOUTH  0
#define NPC_DIR_NORTH  1
#define NPC_DIR_WEST   2
#define NPC_DIR_EAST   3

typedef enum {
  NPC_OK = 0,
  NPC_ERR_NOT_OPEN,      /* npc_gfx_open() never succeeded on this context        */
  NPC_ERR_NO_PAL_TABLE,  /* sObjectEventSpritePalettes not found (hacked ROM?)    */
  NPC_ERR_DYNAMIC_VAR,   /* OBJ_EVENT_GFX_VAR_0..F - resolved from a RAM var      */
  NPC_ERR_ID_RANGE,      /* graphics_id >= NUM_OBJ_EVENT_GFX for this game        */
  NPC_ERR_BAD_PTR,       /* a followed pointer left the ROM image                 */
  NPC_ERR_BAD_DIMS,      /* width/height not a sane multiple of 8                 */
  NPC_ERR_BAD_FRAME,     /* images[frame].size != width*height/2                  */
  NPC_ERR_NO_PALETTE,    /* paletteTag absent from sObjectEventSpritePalettes     */
  NPC_ERR_TOO_BIG,       /* would not fit the caller's buffer                     */
} NpcGfxErr;

/* One resolved sprite. Everything needed to place it in OAM or blit it by hand. */
typedef struct {
  bool     ok;            /* false => err says why; every other field is 0        */
  NpcGfxErr err;

  uint8_t  w, h;          /* pixels                                              */
  uint8_t  tiles_x, tiles_y;
  uint8_t  pal_slot;      /* graphicsInfo.paletteSlot, 0..15                      */
  uint16_t pal_tag;       /* 0x1100..0x11FF - the identity to dedupe palettes by  */
  uint8_t  shadow_size;   /* 0..3                                                 */
  uint8_t  tracks;
  bool     inanimate;
  bool     dynamic;       /* game may substitute another sprite (berry tree, bard) */

  uint8_t  frame;         /* image index actually used                            */
  bool     hflip;         /* the chosen frame must be drawn h-flipped (east)      */

  uint32_t info_addr;     /* ROM addr of the ObjectEventGraphicsInfo              */
  uint32_t frame_addr;    /* ROM addr of the raw 4bpp frame                       */
  uint32_t frame_bytes;   /* == w*h/2                                             */
  uint32_t pal_addr;      /* ROM addr of the 16 BGR555 colours                    */
} NpcGfxInfo;

/* Opened once per ROM; caches the two table addresses so no call re-scans the file. */
typedef struct {
  const RomCtx* rom;
  uint32_t      info_ptrs;   /* == rom->gfx_info_ptrs                             */
  uint32_t      pal_table;   /* sObjectEventSpritePalettes                        */
  uint16_t      pal_count;   /* entries in that table                             */
  uint16_t      gfx_count;   /* NUM_OBJ_EVENT_GFX for this game                   */
  bool          ready;
} NpcGfx;

/* ---- API -------------------------------------------------------------------- */

/* Locate the palette table (a bounded ~100 KB scan around gObjectEventGraphicsInfo-
 * Pointers, done ONCE) and record the per-game id count. Returns false on a ROM whose
 * tables are not where a retail build puts them; the caller must report that rather
 * than draw anything. Costs ~200 reads of 512 bytes - do it at map-screen entry, never
 * inside a draw loop. On the GBA that means it must obey the OS-mode rule like any
 * other rom_map call. */
bool npc_gfx_open(NpcGfx* g, const RomCtx* rom);

/* graphics_id -> the standing, south-facing frame. This is the one the caller wants
 * for a static map view. Always fills *out (with ok=false and an err on failure), so
 * the caller can render a placeholder and show the reason. */
bool npc_gfx_info(const NpcGfx* g, uint16_t graphics_id, NpcGfxInfo* out);

/* The 16 dynamic ids are NOT a dead end when you also hold the player's save.
 * OBJ_EVENT_GFX_VAR_n resolves at runtime to VarGet(VAR_OBJ_GFX_ID_0 + n), and those
 * vars live in SaveBlock1's var array — which PokeDNA already loads. Pull
 * vars[NPC_GFX_VAR_ID_0 - 0x4000 + npc_gfx_var_index(id)] out of the save and pass its
 * low byte here. MEASURED: that recovers 578 of Emerald's 2941 placed objects, 631 of
 * Ruby's 2268 and 44 of FireRed's 1648 — a fifth of RSE's NPCs are VAR-driven, so a
 * map view that ignores them has visible holes. `var_value` is used verbatim as a
 * graphics id (and is itself refused if it is >= NUM_OBJ_EVENT_GFX or itself a VAR id).
 * For a non-VAR graphics_id this behaves exactly like npc_gfx_info_dir. */
#define NPC_GFX_VAR_ID_0 0x4010    /* VAR_OBJ_GFX_ID_0 — same in RSE and FRLG */
bool npc_gfx_info_var(const NpcGfx* g, uint16_t graphics_id, uint8_t var_value,
                      int dir, NpcGfxInfo* out);
/* 0..15 for a VAR id, -1 otherwise. */
int  npc_gfx_var_index(uint16_t graphics_id);

/* Same, but for an arbitrary facing. `dir` is NPC_DIR_*. The frame index is READ FROM
 * THE ROM (anims[dir][0].imageValue) rather than assumed, so inanimate objects - whose
 * four anims all point at "stay still" - correctly come back as frame 0, and
 * odd tables (berry trees, the truck) do not produce a wrong frame. Sets out->hflip
 * when the anim command asks for one (the usual east-is-flipped-west case). */
bool npc_gfx_info_dir(const NpcGfx* g, uint16_t graphics_id, int dir, NpcGfxInfo* out);

/* The 16 BGR555 colours for a resolved sprite. Colour index 0 is the transparent one
 * and is returned as stored (the game never displays it). */
bool npc_gfx_palette(const NpcGfx* g, const NpcGfxInfo* in, uint16_t pal[16]);

/* Copy the raw 4bpp frame into dst. Layout is exactly what GBA OBJ 1D mapping wants:
 * tiles row-major, 32 bytes each, so `dst` can be DMA'd straight into a sprite tile
 * block. Writes in->frame_bytes bytes and NEVER more than cap; returns false (and
 * writes nothing) if cap is too small. */
bool npc_gfx_frame(const NpcGfx* g, const NpcGfxInfo* in, uint8_t* dst4bpp, uint32_t cap);

/* Pure memory->memory: composite a frame fetched by npc_gfx_frame() into BGR555.
 * No I/O, no allocation, nothing bigger than a few registers - safe to call while
 * the SD card is mid-transfer is NOT the point; the point is it never touches ROM.
 * Colour index 0 is TRANSPARENT and those pixels are LEFT UNTOUCHED, so this draws
 * over whatever the map renderer already put in dst. dst is indexed dst[y*stride+x].
 * Honours in->hflip. `frame_len` guards against a short buffer. */
bool npc_gfx_blit(const NpcGfxInfo* in, const uint8_t* frame4bpp, uint32_t frame_len,
                  const uint16_t pal[16], uint16_t* dst, int stride);

/* Convenience: fetch + blit in one call, streaming ONE 8x8 tile (32 bytes) at a time
 * so it needs no large buffer anywhere (hard rule 2 - nothing multi-KB on the IWRAM
 * stack). Slower than frame()+blit() because it issues one read per tile; use it for
 * host tests and one-off draws, not for a scrolling map. */
bool npc_gfx_pixels(const NpcGfx* g, const NpcGfxInfo* in, uint16_t* dst, int stride);

/* OAM geometry for a sprite size, as the raw hardware field values:
 *   *shape 0 = square, 1 = wide, 2 = tall;  *size 0..3.
 * Returns false when w x h is not a legal single-OBJ size - Emerald's 48x48, 96x40
 * and 88x32 objects (3 of 239) and Ruby's 48x48/96x40/88x32 genuinely need the game's
 * subsprite tables, i.e. several OBJs. FireRed's 128x64 SS Anne likewise. Everything
 * else (16x16, 16x32, 32x16, 32x32, 64x64) is a single OBJ. */
bool npc_gfx_oam_shape(uint8_t w, uint8_t h, uint8_t* shape, uint8_t* size);

/* How many graphics ids this game defines (NUM_OBJ_EVENT_GFX). 0 if not open. */
uint16_t npc_gfx_count(const NpcGfx* g);

/* First dynamic id: ids >= this are OBJ_EVENT_GFX_VAR_0..F. Always 240. */
#define NPC_GFX_VARS_BASE 240

const char* npc_gfx_err_str(NpcGfxErr e);

#endif /* NPC_GFX_H */
