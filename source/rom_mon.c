/* Pokemon icons out of the user's own ROM — see rom_mon.h for the design. */
#include "rom_mon.h"

#include <string.h>

/* GFRomHeader field offsets (derived from pokeemerald src/rom_header_gf.c and
 * independently confirmed by gen3_sbdecor.c's +0x4C decorations read, which this
 * layout predicts exactly). The header sits at ROM+0x100. */
#define GFH_OFF          0x100
#define GFH_VERSION      0x00   /* u32, 1..8                       */
#define GFH_LANGUAGE     0x04   /* u32, 1..8                       */
#define GFH_GAMENAME     0x08   /* char[32], starts "pokemon "     */
#define GFH_MON_ICONS    0x38   /* const u8* const* gMonIconTable  */
#define GFH_MON_ICON_IDS 0x3C   /* const u8* gMonIconPaletteIndices */
#define GFH_MON_ICON_PAL 0x40   /* const struct SpritePalette*     */

/* The icon tables' species axis: 0..411 normal (411 = Chimecho, the internal
 * ceiling), 412 = Egg, 413..439 = Unown letters B..'?' (A is species 201). */
#define RM_SPECIES_EGG      412
#define RM_SPECIES_UNOWN_B  413
#define RM_TABLE_ENTRIES    440

static uint32_t rd32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A ROM pointer is sane if it lands inside the image with `need` bytes of room. */
static int ptr_ok(const RomMon* rm, uint32_t addr, uint32_t need) {
  if (addr < ROM_BASE) return 0;
  uint32_t off = addr - ROM_BASE;
  return off < rm->rc->size && need <= rm->rc->size - off;
}

int rom_mon_open(RomMon* rm, const RomCtx* rc) {
  memset(rm, 0, sizeof *rm);
  rm->rc = rc;
  if (!rc || !rc->read) return 0;

  /* Ruby/Sapphire predate the GF header — fail closed until they get pinned rows. */
  uint8_t h[0x44];
  if (!rc->read(rc->ctx, GFH_OFF, h, sizeof h)) return 0;
  uint32_t ver = rd32le(h + GFH_VERSION), lang = rd32le(h + GFH_LANGUAGE);
  if (ver < 1 || ver > 8 || lang < 1 || lang > 8) return 0;
  if (memcmp(h + GFH_GAMENAME, "pokemon ", 8) != 0) return 0;

  uint32_t icons = rd32le(h + GFH_MON_ICONS);
  uint32_t ids   = rd32le(h + GFH_MON_ICON_IDS);
  uint32_t pals  = rd32le(h + GFH_MON_ICON_PAL);
  rm->ok = 1;                                    /* ptr_ok needs rc set; flip back on failure */
  if (!ptr_ok(rm, icons, RM_TABLE_ENTRIES * 4) ||
      !ptr_ok(rm, ids,   RM_TABLE_ENTRIES) ||
      !ptr_ok(rm, pals,  ROM_MON_PALS * 8)) { rm->ok = 0; return 0; }
  rm->icons   = icons - ROM_BASE;
  rm->pal_ids = ids   - ROM_BASE;
  rm->pals    = pals  - ROM_BASE;
  return 1;
}

/* internal species + Unown form -> the icon table's species axis */
static uint16_t table_species(uint16_t species, uint8_t form) {
  if (species == 201 && form >= 1 && form <= 27) return (uint16_t)(RM_SPECIES_UNOWN_B + form - 1);
  return species;
}

int rom_mon_icon(const RomMon* rm, uint16_t species, uint8_t form, uint8_t frame,
                 uint8_t dst[ROM_MON_ICON_BYTES], uint8_t* pal_index) {
  if (!rm || !rm->ok || frame >= ROM_MON_ICON_FRAMES) return 0;
  if (species == 201 && form > 27) return 0;     /* 28 letters total: A + B..'?' */
  uint16_t ts = table_species(species, form);
  if (ts >= RM_TABLE_ENTRIES) return 0;

  uint8_t pe[4];
  if (!rm->rc->read(rm->rc->ctx, rm->icons + (uint32_t)ts * 4, pe, 4)) return 0;
  uint32_t pic = rd32le(pe);
  if (!ptr_ok(rm, pic, ROM_MON_ICON_FRAMES * ROM_MON_ICON_BYTES)) return 0;
  if (!rm->rc->read(rm->rc->ctx, (pic - ROM_BASE) + (uint32_t)frame * ROM_MON_ICON_BYTES,
                    dst, ROM_MON_ICON_BYTES)) return 0;

  if (pal_index) {
    uint8_t id = 0;
    if (!rm->rc->read(rm->rc->ctx, rm->pal_ids + ts, &id, 1)) return 0;
    if (id >= ROM_MON_PALS) return 0;
    *pal_index = id;
  }
  return 1;
}

int rom_mon_icon_pal(const RomMon* rm, int pal_index, uint16_t dst[16]) {
  if (!rm || !rm->ok || pal_index < 0 || pal_index >= ROM_MON_PALS) return 0;
  uint8_t pe[8];                                  /* struct SpritePalette {data; tag} */
  if (!rm->rc->read(rm->rc->ctx, rm->pals + (uint32_t)pal_index * 8, pe, 8)) return 0;
  uint32_t pd = rd32le(pe);
  if (!ptr_ok(rm, pd, 32)) return 0;
  uint8_t raw[32];
  if (!rm->rc->read(rm->rc->ctx, pd - ROM_BASE, raw, 32)) return 0;
  for (int i = 0; i < 16; i++) dst[i] = (uint16_t)(raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
  return 1;
}
