#ifndef ROM_MON_H
#define ROM_MON_H

#include <stdint.h>
#include "rom_map.h"   /* RomCtx / RomReadFn / ROM_BASE */

/*
 * Pokemon ICONS read out of the user's own retail ROM at runtime.
 *
 * This is phase 1 of the ROM-gated build (docs/research-rom-gated-build.md): the
 * shipped binary carries no icon art; when the user's ROM is available (fused into
 * the image, or later a registered SD file), the box screen streams the real 32x32
 * icons straight from it. Nothing is copied anywhere — reads are transient, the
 * posture the map viewer has always used.
 *
 * WHERE THE DATA LIVES (verified in the pret decomps + dumped from Guy's carts):
 * Game Freak's own index block `GFRomHeader` sits at ROM+0x100 in Emerald and
 * FireRed/LeafGreen (Ruby/Sapphire predate it — they need pinned addresses and are
 * NOT served yet; rom_mon_open fails closed there):
 *     +0x38  const u8* const* monIcons            (species -> raw 4bpp, 2 frames of
 *                                                  512 B each, tonc tile order)
 *     +0x3C  const u8*        monIconPaletteIds   (species -> palette 0..2)
 *     +0x40  const struct SpritePalette* monIconPalettes  ({const u16* data; u16 tag}
 *                                                  x3, data = 32 B raw, uncompressed)
 * The tables extend past the 411 normal species: 412 = the Egg icon, 413..439 = the
 * Unown letters B..? (letter A is species 201 itself), so every form is reachable.
 *
 * Pure C (no tonc/FatFs): everything goes through the RomCtx's RomReadFn, so the
 * host tests run this against the real cartridge dumps on the PC.
 */

typedef struct RomMon {
  const RomCtx* rc;
  uint32_t icons;         /* FILE offset of gMonIconTable                  */
  uint32_t pal_ids;       /* FILE offset of gMonIconPaletteIndices         */
  uint32_t pals;          /* FILE offset of gMonIconPaletteTable           */
  int      ok;            /* 1 = header parsed and pointers sane           */
} RomMon;

#define ROM_MON_ICON_BYTES   512   /* one 32x32 4bpp frame                 */
#define ROM_MON_ICON_FRAMES  2
#define ROM_MON_PALS         3

/* Parse the GF header + sanity-check the three icon pointers. Returns 1 on
 * success; 0 (fail closed) for Ruby/Sapphire, an unknown ROM, or any pointer
 * that does not stay inside the image. */
int rom_mon_open(RomMon* rm, const RomCtx* rc);

/* Where one species' icon lives: the answer to the two SMALL table lookups (the
 * gMonIconTable pointer and the palette index) that every frame read had to redo.
 * Splitting locate from read matters because a caller that reads the same frame
 * twice to verify it — box_oam does — otherwise paid those lookups twice as well:
 * six RomCtx reads per icon instead of two. On the SD-file source every read is an
 * f_lseek + f_read, and a BACKWARD seek re-walks the cluster chain from the head of
 * a 16 MB file (FF_USE_FASTSEEK is deliberately 0 — it is a global switch on the
 * same FatFs the save write path uses), so the ping-ponging offsets were the cost. */
typedef struct RomMonLoc {
  uint32_t tiles;         /* FILE offset of frame 0 (frame 1 sits +512 after it) */
  uint8_t  pal;           /* palette index 0..2                                  */
  uint8_t  ok;            /* 1 = usable; 0 = never located / lookup failed        */
} RomMonLoc;

/* Resolve (species, form) to its icon offset + palette index. Two small reads.
 * `form` handles Unown (species 201, form 0..27); pass 0 otherwise. Species 412
 * = the Egg. Returns 1 on success, 0 on any bounds/read failure (and *out is
 * left with ok = 0, so a memoised RomMonLoc self-invalidates). */
int rom_mon_locate(const RomMon* rm, uint16_t species, uint8_t form, RomMonLoc* out);

/* Same, but VERIFIED: every table field is read twice back to back and accepted only
 * when both passes agree, retried up to `attempts` times (minimum 2).
 *
 * WHY a caller that memoises the location MUST use this. The two fields locate reads
 * are the icon's ADDRESS and its palette bank. Garble the pointer and ptr_ok still
 * accepts it as long as it lands inside the image with 1 KiB of room, so the frame
 * read succeeds — off the WRONG offset. A caller that verifies the 512 B frame by
 * reading it twice cannot catch that: with the location located once, both of its
 * passes read the same wrong offset, the two frames are identical, the verify says
 * "stable", and the slot shows another species' art with nothing logged. The same
 * goes for the palette byte: a garbled id that still lands in 0..2 picks the wrong
 * OBJ palette bank. Before the memo existed, box_oam re-did these lookups inside
 * every verify pass, and that is the coverage this restores — cheaply, because the
 * second read of a 1..4 byte field is a backward seek that stays inside the current
 * cluster (see read_small in rom_mon.c) and so costs no extra FAR seek.
 *
 * Returns 1 with *out usable; 0 on failure with out->ok = 0 (so a memoised RomMonLoc
 * self-invalidates). `unstable` (may be NULL) is set to 1 only for the case worth
 * logging: the reads themselves succeeded but never agreed. A bounds/read failure
 * leaves it 0, so a corrupt species id in a save cannot spam the log. */
int rom_mon_locate_verified(const RomMon* rm, uint16_t species, uint8_t form,
                            RomMonLoc* out, int attempts, int* unstable);

/* Same as rom_mon_locate_verified, but by the icon table's own row index (0..439:
 * 0..411 the species axis's raw ceiling, 412 the Egg, 413..439 Unown B..'?') instead
 * of a (species, form) pair. This is the axis source/art_icons_extract.c's cache
 * mirrors 1:1 (icons.bin row r == this table's row r) and the axis
 * source/art_icons_cache.c's loader indexes by, so both sides of the cache agree with
 * rom_mon.c's own layout by construction, not by re-deriving table_species(). Total
 * row count is RM_TABLE_ENTRIES (440), exposed as rom_mon_table_rows(). */
int rom_mon_locate_row_verified(const RomMon* rm, uint16_t row, RomMonLoc* out,
                                int attempts, int* unstable);
uint16_t rom_mon_table_rows(void);

/* Read one 512 B frame of an already-located icon. One read, no lookups. */
int rom_mon_icon_at(const RomMon* rm, const RomMonLoc* loc, uint8_t frame,
                    uint8_t dst[ROM_MON_ICON_BYTES]);

/* Read one 512 B icon frame for an internal species id into dst, and that
 * species' palette index (0..2) into *pal_index (may be NULL). `form` handles
 * Unown (species 201, form 0..27); pass 0 otherwise. Species 412 = the Egg.
 * Returns 1 on success, 0 on any bounds/read failure.
 * Convenience wrapper over rom_mon_locate + rom_mon_icon_at — a caller that reads
 * more than one frame or verifies by re-reading should use those directly. */
int rom_mon_icon(const RomMon* rm, uint16_t species, uint8_t form, uint8_t frame,
                 uint8_t dst[ROM_MON_ICON_BYTES], uint8_t* pal_index);

/* Read shared icon palette `pal_index` (0..2) as 16 raw RGB15 entries. */
int rom_mon_icon_pal(const RomMon* rm, int pal_index, uint16_t dst[16]);

#endif /* ROM_MON_H */
