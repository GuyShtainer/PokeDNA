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

/* Read a SMALL table field (<= 4 B) at `off`.
 *
 * attempts == 1: one plain read — the legacy contract.
 * attempts >  1: read the field TWICE, back to back, and accept only when the two
 *   passes agree; retry up to `attempts` times. These few bytes are the icon's
 *   ADDRESS and its palette bank, so a transient garble here is not a garbled
 *   picture, it is the WRONG picture — and a caller that verifies the 512 B frame by
 *   reading it twice cannot see that, because both of its passes read the same wrong
 *   offset and agree. Verifying the lookup is the only place that error is visible.
 *
 * Reading the same few bytes twice back to back is also the cheapest verify this
 * source can do: the second read seeks BACKWARD by at most 4 bytes, which stays
 * inside the current cluster, so FatFs f_lseek takes its same-or-following-cluster
 * fast path (ff.c:4523) instead of re-walking the chain from the head of a 16 MB file
 * (FF_USE_FASTSEEK is 0). It adds no FAR seek at all.
 *
 * Returns 1 = value in dst is trustworthy, 0 = a read failed, -1 = reads succeeded
 * but the passes never agreed. */
static int read_small(const RomMon* rm, uint32_t off, uint8_t* dst, uint32_t len,
                      int attempts) {
  if (attempts < 1) attempts = 1;
  if (attempts == 1) return rm->rc->read(rm->rc->ctx, off, dst, len) ? 1 : 0;
  uint8_t b[4];
  if (len > sizeof b) return 0;
  for (int a = 0; a < attempts; a++) {
    if (!rm->rc->read(rm->rc->ctx, off, dst, len)) return 0;
    if (!rm->rc->read(rm->rc->ctx, off, b, len)) return 0;
    if (memcmp(dst, b, len) == 0) return 1;
  }
  return -1;
}

/* The guts of locate_ex, taking the icon TABLE INDEX (ts, 0..439) directly instead of
 * a (species, form) pair. rom_mon_locate() and friends map (species, form) down to
 * this via table_species(); the ROM-art extractor (source/art_icons_extract.c) walks
 * the table's own axis in order (row 0..439 == exactly the icons.bin cache's own
 * layout) and has no (species, form) pair to give it for row 412 (Egg) or the raw
 * Unown rows -- it wants the table index directly, which is what this exposes. */
static int locate_row_ex(const RomMon* rm, uint16_t ts, RomMonLoc* out, int attempts,
                         int* unstable) {
  if (unstable) *unstable = 0;
  if (!out) return 0;
  out->tiles = 0; out->pal = 0; out->ok = 0;     /* fail closed: a memo self-invalidates */
  if (!rm || !rm->ok) return 0;
  if (ts >= RM_TABLE_ENTRIES) return 0;

  uint8_t pe[4];
  int r = read_small(rm, rm->icons + (uint32_t)ts * 4, pe, 4, attempts);
  if (r <= 0) { if (r < 0 && unstable) *unstable = 1; return 0; }
  uint32_t pic = rd32le(pe);
  if (!ptr_ok(rm, pic, ROM_MON_ICON_FRAMES * ROM_MON_ICON_BYTES)) return 0;

  uint8_t id = 0;
  r = read_small(rm, rm->pal_ids + ts, &id, 1, attempts);
  if (r <= 0) { if (r < 0 && unstable) *unstable = 1; return 0; }
  if (id >= ROM_MON_PALS) return 0;

  out->tiles = pic - ROM_BASE;
  out->pal   = id;
  out->ok    = 1;
  return 1;
}

static int locate_ex(const RomMon* rm, uint16_t species, uint8_t form, RomMonLoc* out,
                     int attempts, int* unstable) {
  if (out) { out->tiles = 0; out->pal = 0; out->ok = 0; }
  if (!rm || !rm->ok) return 0;
  if (species == 201 && form > 27) return 0;     /* 28 letters total: A + B..'?' */
  uint16_t ts = table_species(species, form);
  return locate_row_ex(rm, ts, out, attempts, unstable);
}

int rom_mon_locate(const RomMon* rm, uint16_t species, uint8_t form, RomMonLoc* out) {
  return locate_ex(rm, species, form, out, 1, 0);
}

int rom_mon_locate_verified(const RomMon* rm, uint16_t species, uint8_t form,
                            RomMonLoc* out, int attempts, int* unstable) {
  return locate_ex(rm, species, form, out, attempts < 2 ? 2 : attempts, unstable);
}

int rom_mon_locate_row_verified(const RomMon* rm, uint16_t row, RomMonLoc* out,
                                int attempts, int* unstable) {
  return locate_row_ex(rm, row, out, attempts < 2 ? 2 : attempts, unstable);
}

uint16_t rom_mon_table_rows(void) { return RM_TABLE_ENTRIES; }

int rom_mon_icon_at(const RomMon* rm, const RomMonLoc* loc, uint8_t frame,
                    uint8_t dst[ROM_MON_ICON_BYTES]) {
  if (!rm || !rm->ok || !loc || !loc->ok) return 0;
  if (frame >= ROM_MON_ICON_FRAMES) return 0;
  return rm->rc->read(rm->rc->ctx, loc->tiles + (uint32_t)frame * ROM_MON_ICON_BYTES,
                      dst, ROM_MON_ICON_BYTES) ? 1 : 0;
}

int rom_mon_icon(const RomMon* rm, uint16_t species, uint8_t form, uint8_t frame,
                 uint8_t dst[ROM_MON_ICON_BYTES], uint8_t* pal_index) {
  if (frame >= ROM_MON_ICON_FRAMES) return 0;
  RomMonLoc loc;
  if (!rom_mon_locate(rm, species, form, &loc)) return 0;
  if (!rom_mon_icon_at(rm, &loc, frame, dst)) return 0;
  if (pal_index) *pal_index = loc.pal;
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
