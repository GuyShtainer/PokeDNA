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

/* Ruby/Sapphire predate the GF header, so their three icon tables are PINNED by game
 * code + revision (BACKLOG #293), the rom_text.c precedent. Each row was located by
 * tools/rs_locate.py, an independent by-shape scan of the corpus dump: a run of 440
 * 4-aligned ROM pointers each with room for a 1,024 B icon, followed directly by 440
 * palette-index bytes all < 3, followed by three {ptr, tag, pad} rows whose tags run
 * consecutively (0xDAC0..0xDAC2). An unpinned R/S revision stays fail-closed. */
typedef struct {
  const char* code;      /* 4-char game code at 0xAC */
  uint8_t     version;   /* revision byte at 0xBC    */
  uint32_t    icons, ids, pals;   /* ROM addresses */
} RomMonPin;

static const RomMonPin k_pins[] = {
  { "AXVE", 2, 0x083BBD3Cu, 0x083BC41Cu, 0x083BC5D4u },
  { "AXPE", 1, 0x083BBD98u, 0x083BC478u, 0x083BC630u },
};
#define K_NPINS ((int)(sizeof k_pins / sizeof k_pins[0]))

/* The shape a pinned row must still have at open. One transient garbled read gets a
 * second chance; a wrong address fails both. */
static int pin_shape_once(const RomMon* rm, uint32_t icons, uint32_t ids, uint32_t pals) {
  uint8_t buf[128];
  for (uint32_t o = 0; o < RM_TABLE_ENTRIES * 4u; o += sizeof buf) {
    uint32_t n = RM_TABLE_ENTRIES * 4u - o;
    if (n > sizeof buf) n = sizeof buf;
    if (!rm->rc->read(rm->rc->ctx, icons + o, buf, n)) return 0;
    for (uint32_t i = 0; i < n; i += 4)
      if (!ptr_ok(rm, rd32le(buf + i), ROM_MON_ICON_FRAMES * ROM_MON_ICON_BYTES)) return 0;
  }
  for (uint32_t o = 0; o < RM_TABLE_ENTRIES; o += sizeof buf) {
    uint32_t n = RM_TABLE_ENTRIES - o;
    if (n > sizeof buf) n = sizeof buf;
    if (!rm->rc->read(rm->rc->ctx, ids + o, buf, n)) return 0;
    for (uint32_t i = 0; i < n; i++) if (buf[i] >= ROM_MON_PALS) return 0;
  }
  uint8_t pe[ROM_MON_PALS * 8];
  if (!rm->rc->read(rm->rc->ctx, pals, pe, sizeof pe)) return 0;
  uint32_t tag0 = (uint32_t)pe[4] | ((uint32_t)pe[5] << 8);
  for (int k = 0; k < ROM_MON_PALS; k++) {
    const uint8_t* e = pe + k * 8;
    uint32_t tag = (uint32_t)e[4] | ((uint32_t)e[5] << 8);
    if (tag != tag0 + (uint32_t)k) return 0;
    if (!ptr_ok(rm, rd32le(e), 32)) return 0;
  }
  return 1;
}

static int open_pinned(RomMon* rm, const RomCtx* rc) {
  if (rc->kind != ROM_RUBY && rc->kind != ROM_SAPPHIRE) return 0;
  for (int i = 0; i < K_NPINS; i++) {
    const RomMonPin* p = &k_pins[i];
    if (memcmp(p->code, rc->code, 4) != 0 || p->version != rc->version) continue;
    rm->ok = 1;                                  /* ptr_ok needs rc set; flip back on failure */
    if (!ptr_ok(rm, p->icons, RM_TABLE_ENTRIES * 4) || !ptr_ok(rm, p->ids, RM_TABLE_ENTRIES) ||
        !ptr_ok(rm, p->pals, ROM_MON_PALS * 8)) { rm->ok = 0; return 0; }
    uint32_t ic = p->icons - ROM_BASE, id = p->ids - ROM_BASE, pl = p->pals - ROM_BASE;
    if (!pin_shape_once(rm, ic, id, pl) && !pin_shape_once(rm, ic, id, pl)) { rm->ok = 0; return 0; }
    rm->icons = ic; rm->pal_ids = id; rm->pals = pl;
    return 1;
  }
  return 0;   /* an unpinned R/S revision: fail closed */
}

int rom_mon_open(RomMon* rm, const RomCtx* rc) {
  memset(rm, 0, sizeof *rm);
  rm->rc = rc;
  if (!rc || !rc->read) return 0;

  /* Ruby/Sapphire predate the GF header: they take the pinned path and never reach the
   * header read below. */
  if (rc->kind == ROM_RUBY || rc->kind == ROM_SAPPHIRE) return open_pinned(rm, rc);
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
 * fast path instead of walking the chain. It adds no FAR seek at all. Measured on the
 * host FatFs harness: both small-field verify re-reads cost ZERO extra disk_read
 * calls, which is the claim this paragraph makes and it holds.
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

/* A Fletcher-style running pair over a blob, in one pass and with no second buffer:
 * the destination itself is both passes' target and only the two sums are compared.
 * That is the same trick box_oam.c's stage_sum and icon_store.c's row_sum use, one
 * accumulator wider -- and the extra accumulator earns its keep here, because a PLAIN
 * byte sum cannot see two bytes swapping places, and in a 1,760 B table of pointers a
 * swapped pair is not a smudge, it is the wrong species' icon at full confidence. */
static uint32_t blob_sum(const uint8_t* b, uint32_t n) {
  uint32_t a = 1, c = 0;
  for (uint32_t i = 0; i < n; i++) { a += b[i]; c += a; }
  return (a & 0xFFFFu) | (c << 16);
}

/* Read `len` bytes at `off` into `dst` twice back to back and accept only when the two
 * passes' sums agree, up to `attempts` times. Returns 1 = trustworthy, 0 = a read
 * failed, -1 = the reads succeeded but never agreed. */
static int read_verified(const RomMon* rm, uint32_t off, uint8_t* dst, uint32_t len,
                         int attempts) {
  for (int a = 0; a < attempts; a++) {
    if (!rm->rc->read(rm->rc->ctx, off, dst, len)) return 0;
    uint32_t s1 = blob_sum(dst, len);
    if (!rm->rc->read(rm->rc->ctx, off, dst, len)) return 0;
    if (blob_sum(dst, len) == s1) return 1;
  }
  return -1;
}

int rom_mon_read_tables(const RomMon* rm, uint32_t off[], uint8_t palid[],
                        uint16_t pals[][16], int* bad_rows) {
  const int ATT = 4;
  int bad = 0;

  if (bad_rows) *bad_rows = 0;
  if (!rm || !rm->ok || !off || !palid || !pals) return 0;

  /* The pointer table, read as RAW BYTES into its own destination and widened in
   * place. Forward order is what makes in-place safe: entry i is written only after
   * bytes [4i, 4i+4) have been consumed, and nothing later has been touched yet. The
   * widening goes through rd32le, never a pointer cast -- ARM7TDMI silently ROTATES an
   * unaligned word load rather than faulting, and 438 of these 440 values sit at
   * offsets a cast would get wrong on some other table. */
  if (read_verified(rm, rm->icons, (uint8_t*)off, RM_TABLE_ENTRIES * 4u, ATT) != 1) return 0;
  {
    const uint8_t* raw = (const uint8_t*)off;
    for (uint32_t i = 0; i < RM_TABLE_ENTRIES; i++) {
      uint32_t pic = rd32le(raw + i * 4u);
      if (ptr_ok(rm, pic, ROM_MON_ICON_FRAMES * ROM_MON_ICON_BYTES)) {
        off[i] = pic - ROM_BASE;
      } else {
        off[i] = ROM_MON_OFF_NONE;                 /* no icon for this row, as before */
        bad++;
      }
    }
  }

  /* The palette-id table. One byte per row, no widening, same verify. */
  if (read_verified(rm, rm->pal_ids, palid, RM_TABLE_ENTRIES, ATT) != 1) return 0;
  for (uint32_t i = 0; i < RM_TABLE_ENTRIES; i++)
    if (palid[i] >= ROM_MON_PALS) { palid[i] = 0xFF; bad++; }

  /* The 3 shared palettes: one verified read of the SpritePalette array, then one
   * verified read of each 32 B colour block it points at. A palette that will not
   * verify fails the WHOLE load rather than being marked bad per-bank: unlike a row,
   * there is no "this one mon has no icon" degradation available -- every row using
   * that bank would draw in the wrong colours, which looks like working art. */
  {
    uint8_t pe[ROM_MON_PALS * 8];
    if (read_verified(rm, rm->pals, pe, sizeof pe, ATT) != 1) return 0;
    for (int i = 0; i < ROM_MON_PALS; i++) {
      uint32_t pd = rd32le(pe + i * 8);
      uint8_t raw[32];
      if (!ptr_ok(rm, pd, 32)) return 0;
      if (read_verified(rm, pd - ROM_BASE, raw, 32, ATT) != 1) return 0;
      for (int c = 0; c < 16; c++)
        pals[i][c] = (uint16_t)(raw[c * 2] | ((uint16_t)raw[c * 2 + 1] << 8));
    }
  }

  if (bad_rows) *bad_rows = bad;
  return 1;
}

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
