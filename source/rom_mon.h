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

/* Read one 512 B icon frame for an internal species id into dst, and that
 * species' palette index (0..2) into *pal_index (may be NULL). `form` handles
 * Unown (species 201, form 0..27); pass 0 otherwise. Species 412 = the Egg.
 * Returns 1 on success, 0 on any bounds/read failure. */
int rom_mon_icon(const RomMon* rm, uint16_t species, uint8_t form, uint8_t frame,
                 uint8_t dst[ROM_MON_ICON_BYTES], uint8_t* pal_index);

/* Read shared icon palette `pal_index` (0..2) as 16 raw RGB15 entries. */
int rom_mon_icon_pal(const RomMon* rm, int pal_index, uint16_t dst[16]);

#endif /* ROM_MON_H */
