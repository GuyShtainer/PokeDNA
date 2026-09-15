/*
 * Gen-3 overworld map reader — parses the user's own Pokemon ROM. See rom_map.h for
 * the design (read callback, so this same code is host-tested against real ROMs) and
 * for the FireRed/LeafGreen structural divergences.
 *
 * Pure C: no tonc, no FatFs, no GBA headers.
 */
#include <string.h>
#include "rom_map.h"

/* ---- little-endian scalar readers ----------------------------------------- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- known versions --------------------------------------------------------
 * gMapGroups / gMapLayouts / gObjectEventGraphicsInfoPointers per retail build,
 * from pret's byte-matching .sym files. FireRed's 0x083526A8 is the same "map bank
 * table" address Advance Map and PGE have used for two decades — independent
 * corroboration of the method.
 *
 * The number of groups is NOT discoverable at runtime (it comes from the size of the
 * gMapGroups symbol), so it is part of this table. The per-group map COUNTS are
 * derived at open time from consecutive pointer differences — see derive_groups. */
typedef struct {
  const char* code;      /* 4-char game code at 0xAC */
  uint8_t     version;   /* revision byte at 0xBC    */
  RomKind     kind;
  uint32_t    map_groups, map_layouts, gfx_info_ptrs;
  uint8_t     groups;
} RomVersion;

static const RomVersion k_versions[] = {
  { "BPEE", 0, ROM_EMERALD,   0x08486578, 0x08481DD4, 0x08505620, 34 },
  { "AXVE", 0, ROM_RUBY,      0x08308588, 0x08304F18, 0x0836DC58, 34 },
  { "AXVE", 1, ROM_RUBY,      0x083085A0, 0x08304F30, 0x0836DC70, 34 },
  { "AXVE", 2, ROM_RUBY,      0x083085A0, 0x08304F30, 0x0836DC70, 34 },
  { "AXPE", 0, ROM_SAPPHIRE,  0x08308518, 0x08304EA8, 0x0836DBE8, 34 },
  { "AXPE", 1, ROM_SAPPHIRE,  0x08308530, 0x08304EC0, 0x0836DC00, 34 },
  { "AXPE", 2, ROM_SAPPHIRE,  0x08308530, 0x08304EC0, 0x0836DC00, 34 },
  { "BPRE", 0, ROM_FIRERED,   0x083526A8, 0x0834EB8C, 0x0839FDB0, 43 },
  { "BPRE", 1, ROM_FIRERED,   0x08352718, 0x0834EBFC, 0x0839FE20, 43 },
  { "BPGE", 0, ROM_LEAFGREEN, 0x08352688, 0x0834EB6C, 0x0839FD90, 43 },
  { "BPGE", 1, ROM_LEAFGREEN, 0x083526F8, 0x0834EBDC, 0x0839FE00, 43 },
};
#define K_NVERSIONS ((int)(sizeof k_versions / sizeof k_versions[0]))

/* ---- ROM-hack detection (BACKLOG #54) --------------------------------------
 * Retail internal title at 0xA0, exactly 12 chars, no padding — indexed by RomKind
 * so it lives beside k_versions and cannot drift from it (decision 2). PER-GAME, not
 * per-revision: FireRed r0 and r1 both ship "POKEMON FIRE", so one row covers both
 * pinned rows in k_versions above. Measured on Guy's own corpus (one dump per game;
 * the revision the title was READ FROM is noted, but the string is the same for
 * every revision of that game):
 *   POKEMON EMER — BPEE rev 0
 *   POKEMON RUBY — AXVE rev 2
 *   POKEMON SAPP — AXPE rev 1
 *   POKEMON FIRE — BPRE rev 1
 *   POKEMON LEAF — BPGE rev 0
 * NO content hash, NO fingerprint window: a whole-ROM hash at boot is too slow, and a
 * fixed-window CRC is wrong on correctness — six of the eleven pinned builds (AXVE
 * r0/r1, AXPE r0/r2, BPRE r0, BPGE r1) are absent from the corpus, so their constants
 * would be guessed, and a guessed constant misdetecting a retail ROM as a hack is the
 * exact failure this table exists to prevent. Content-level identification (a
 * per-hack SD profile) is BACKLOG #54's T2, deliberately out of scope here. */
static const char* const k_retail_title[] = {
  [ROM_NONE]      = 0,
  [ROM_EMERALD]   = "POKEMON EMER",
  [ROM_RUBY]      = "POKEMON RUBY",
  [ROM_SAPPHIRE]  = "POKEMON SAPP",
  [ROM_FIRERED]   = "POKEMON FIRE",
  [ROM_LEAFGREEN] = "POKEMON LEAF",
};

/* Decision 1's five-way verdict, straight off the header rom_open() already reads —
 * no extra I/O, no statics, no GBA/tonc/FatFs headers (host-testable in isolation:
 * tests/host_romident_test.c calls this directly with synthetic headers). */
RomIdent rom_identify(const uint8_t hdr[0xC0], uint32_t size, RomKind* base_kind) {
  if (base_kind) *base_kind = ROM_NONE;
  if (hdr[0xB2] != 0x96) return ROM_ID_NOT_GBA;              /* rule (a) */

  char code[5];  memcpy(code, hdr + 0xAC, 4);  code[4]  = 0;
  char title[13]; memcpy(title, hdr + 0xA0, 12); title[12] = 0;
  uint8_t version = hdr[0xBC];

  const RomVersion* v = 0;
  for (int i = 0; i < K_NVERSIONS; i++)
    if (memcmp(k_versions[i].code, code, 4) == 0 && k_versions[i].version == version) {
      v = &k_versions[i]; break;
    }

  if (v) {
    if (base_kind) *base_kind = v->kind;
    bool title_ok = memcmp(title, k_retail_title[v->kind], 12) == 0;
    if (size == 16u * 1024u * 1024u && title_ok) return ROM_ID_RETAIL;   /* rule (b) */
    return ROM_ID_HACK;                                                 /* rule (c) */
  }
  if (memcmp(title, "POKEMON", 7) == 0) return ROM_ID_HACK;             /* rule (d), base unknown */
  return ROM_ID_NOT_POKEMON;                                            /* rule (e) */
}

static const RomFmt k_fmt_rse  = { 24, 0x10, 0x14, 2, 512, 512, 6, 0 };
static const RomFmt k_fmt_frlg = { 28, 0x14, 0x10, 4, 640, 640, 7, 1 };

const char* rom_kind_name(RomKind k) {
  switch (k) {
    case ROM_EMERALD:   return "Emerald";
    case ROM_RUBY:      return "Ruby";
    case ROM_SAPPHIRE:  return "Sapphire";
    case ROM_FIRERED:   return "FireRed";
    case ROM_LEAFGREEN: return "LeafGreen";
    default:            return "unknown";
  }
}

bool rom_ptr_ok(const RomCtx* c, uint32_t addr) {
  if (!c || addr < ROM_BASE) return false;
  uint32_t off = addr - ROM_BASE;
  return off < c->size;
}

bool rom_read_at(const RomCtx* c, uint32_t addr, void* dst, uint32_t len) {
  if (!c || !c->read || !dst || !len) return false;
  if (addr < ROM_BASE) return false;
  uint32_t off = addr - ROM_BASE;
  /* overflow-safe bound: off + len must fit inside the file */
  if (off > c->size || len > c->size - off) return false;
  return c->read(c->ctx, off, dst, len);
}

/* Derive per-group map counts. The generated group arrays sit contiguously and in
 * order immediately BEFORE gMapGroups itself, so each group's length is the gap to
 * the next group's array, and the last one runs up to gMapGroups. Verified on the
 * retail Emerald ROM: yields [57,5,5,...] totalling 518, matching map_groups.json. */
static bool derive_groups(RomCtx* c) {
  uint8_t buf[ROM_MAX_GROUPS * 4];
  uint32_t n = c->group_count;
  if (n == 0 || n > ROM_MAX_GROUPS) return false;
  if (!rom_read_at(c, c->map_groups, buf, n * 4)) return false;

  for (uint32_t i = 0; i < n; i++) {
    c->group_ptr[i] = rd32(buf + i * 4);
    if (!rom_ptr_ok(c, c->group_ptr[i])) return false;
  }
  /* The arrays must be laid out in increasing address order for the difference
   * trick to be valid; a hacked/relocated table fails here rather than producing
   * a plausible-looking wrong answer. */
  uint32_t total = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t end = (i + 1 < n) ? c->group_ptr[i + 1] : c->map_groups;
    if (end <= c->group_ptr[i]) return false;
    uint32_t bytes = end - c->group_ptr[i];
    if (bytes & 3u) return false;                 /* must be a whole number of pointers */
    uint32_t cnt = bytes >> 2;
    if (cnt == 0 || cnt > 200) return false;      /* Emerald's biggest group is 108 */
    c->group_size[i] = (uint16_t)cnt;
    total += cnt;
  }
  if (total == 0 || total > 4096) return false;
  c->total_maps = (uint16_t)total;
  return true;
}

/* Structural sanity walk. This is a SECONDARY gate: it rejects hacks that relocated
 * the tables (Glazed) and truncated dumps, but a content-only hack that leaves
 * gMapGroups structurally intact will pass. That is why the version table above is
 * keyed on the game code + revision, and why a caller should surface exactly which
 * ROM was detected rather than claiming it "verified" it. */
static bool sanity_walk(RomCtx* c) {
  int checked = 0;
  for (int g = 0; g < c->group_count && checked < 8; g++) {
    int cnt = rom_maps_in_group(c, g);
    if (cnt <= 0) return false;
    for (int m = 0; m < cnt && checked < 8; m += (cnt > 4 ? cnt / 4 + 1 : 1)) {
      RomMapHeader h;
      if (!rom_map_header(c, g, m, &h)) return false;
      RomLayout l;
      if (!rom_layout(c, h.layout, &l)) return false;
      /* The game's own map buffer is 10,240 u16 (sBackupMapData); anything bigger
       * is not a real map and would smear that buffer if it were ever loaded. */
      if (l.width <= 0 || l.height <= 0 || l.width > 1000 || l.height > 1000) return false;
      if ((uint32_t)l.width * (uint32_t)l.height > 10240u) return false;
      if (!rom_ptr_ok(c, l.blocks) || !rom_ptr_ok(c, l.tileset_primary)) return false;
      checked++;
    }
  }
  return checked > 0;
}

bool rom_open(RomCtx* c, RomReadFn read, void* ctx, uint32_t size) {
  if (!c || !read) return false;
  memset(c, 0, sizeof *c);
  c->read = read; c->ctx = ctx; c->size = size; c->kind = ROM_NONE;

  /* A retail Gen-3 cart is 16 MiB. Refuse anything else outright: a 32 MiB image is
   * a hack, and a short one is a truncated dump. */
  if (size != 16u * 1024u * 1024u) return false;

  uint8_t hdr[0xC0];
  if (!c->read(c->ctx, 0, hdr, sizeof hdr)) return false;
  /* Nintendo logo checksum byte + the fixed 0x96 at 0xB2 — a cheap "is this even a
   * GBA ROM" gate before we trust anything else in the header. */
  if (hdr[0xB2] != 0x96) return false;

  memcpy(c->title, hdr + 0xA0, 12); c->title[12] = 0;
  memcpy(c->code,  hdr + 0xAC, 4);  c->code[4]  = 0;
  c->version = hdr[0xBC];

  const RomVersion* v = 0;
  for (int i = 0; i < K_NVERSIONS; i++)
    if (memcmp(k_versions[i].code, c->code, 4) == 0 && k_versions[i].version == c->version) {
      v = &k_versions[i]; break;
    }
  if (!v) return false;

  c->kind          = v->kind;
  c->map_groups    = v->map_groups;
  c->map_layouts   = v->map_layouts;
  c->gfx_info_ptrs = v->gfx_info_ptrs;
  c->group_count   = v->groups;
  c->fmt = (v->kind == ROM_FIRERED || v->kind == ROM_LEAFGREEN) ? k_fmt_frlg : k_fmt_rse;

  if (!derive_groups(c) || !sanity_walk(c)) { c->kind = ROM_NONE; return false; }
  return true;
}

int rom_maps_in_group(const RomCtx* c, int group) {
  if (!c || group < 0 || group >= c->group_count) return -1;
  return c->group_size[group];
}

bool rom_map_header(const RomCtx* c, int group, int num, RomMapHeader* out) {
  int cnt = rom_maps_in_group(c, group);
  if (!out || cnt < 0 || num < 0 || num >= cnt) return false;
  uint8_t p[4];
  if (!rom_read_at(c, c->group_ptr[group] + (uint32_t)num * 4, p, 4)) return false;
  uint32_t hdr = rd32(p);
  if (!rom_ptr_ok(c, hdr)) return false;

  uint8_t b[28];
  if (!rom_read_at(c, hdr, b, sizeof b)) return false;
  memset(out, 0, sizeof *out);
  out->layout      = rd32(b + 0x00);
  out->events      = rd32(b + 0x04);
  out->scripts     = rd32(b + 0x08);
  out->connections = rd32(b + 0x0C);
  out->music       = rd16(b + 0x10);
  out->layout_id   = rd16(b + 0x12);
  out->mapsec      = b[0x14];
  out->cave        = b[0x15];
  out->weather     = b[0x16];
  out->map_type    = b[0x17];
  if (c->fmt.frlg_header) {
    /* FRLG: no filler; bikingAllowed@0x18, flags@0x19, floorNum@0x1A. */
    out->flags     = b[0x19];
    out->floor_num = b[0x1A];
  } else {
    out->flags     = b[0x1A];
  }
  out->battle_type = b[0x1B];
  return rom_ptr_ok(c, out->layout);
}

bool rom_layout(const RomCtx* c, uint32_t layout_addr, RomLayout* out) {
  if (!c || !out || !rom_ptr_ok(c, layout_addr)) return false;
  uint8_t b[28];
  uint32_t sz = c->fmt.layout_size;
  if (!rom_read_at(c, layout_addr, b, sz)) return false;
  memset(out, 0, sizeof *out);
  out->width             = (int32_t)rd32(b + 0x00);
  out->height            = (int32_t)rd32(b + 0x04);
  out->border            = rd32(b + 0x08);
  out->blocks            = rd32(b + 0x0C);
  out->tileset_primary   = rd32(b + 0x10);
  out->tileset_secondary = rd32(b + 0x14);
  if (c->fmt.layout_size >= 28) { out->border_w = b[0x18]; out->border_h = b[0x19]; }
  else                          { out->border_w = 2; out->border_h = 2; }  /* RSE is fixed 2x2 */
  if (out->border_w == 0 || out->border_h == 0) { out->border_w = 2; out->border_h = 2; }
  return true;
}

bool rom_tileset(const RomCtx* c, uint32_t ts_addr, RomTileset* out) {
  if (!c || !out || !rom_ptr_ok(c, ts_addr)) return false;
  uint8_t b[24];
  if (!rom_read_at(c, ts_addr, b, sizeof b)) return false;
  memset(out, 0, sizeof *out);
  out->compressed = b[0x00];
  out->secondary  = b[0x01];
  out->tiles      = rd32(b + 0x04);
  out->palettes   = rd32(b + 0x08);
  out->metatiles  = rd32(b + 0x0C);
  /* metatileAttributes and callback are SWAPPED on FRLG. */
  out->attributes = rd32(b + c->fmt.ts_attr_off);
  return true;
}

bool rom_map_events(const RomCtx* c, uint32_t events_addr, RomMapEvents* out) {
  if (!c || !out) return false;
  memset(out, 0, sizeof *out);
  if (!events_addr || !rom_ptr_ok(c, events_addr)) return false;   /* many maps have none */
  uint8_t b[20];
  if (!rom_read_at(c, events_addr, b, sizeof b)) return false;
  out->object_count = b[0];
  out->warp_count   = b[1];
  out->coord_count  = b[2];
  out->bg_count     = b[3];
  out->objects = rd32(b + 4);
  out->warps   = rd32(b + 8);
  out->coords  = rd32(b + 12);
  out->bgs     = rd32(b + 16);
  return true;
}

bool rom_object_event(const RomCtx* c, const RomMapEvents* ev, int i, RomObjectEvent* out) {
  if (!c || !ev || !out || i < 0 || i >= ev->object_count) return false;
  uint8_t b[24];
  if (!rom_read_at(c, ev->objects + (uint32_t)i * 24u, b, sizeof b)) return false;
  memset(out, 0, sizeof *out);
  out->local_id      = b[0x00];
  out->graphics_id   = b[0x01];
  out->kind          = b[0x02];
  out->x             = (int16_t)rd16(b + 0x04);
  out->y             = (int16_t)rd16(b + 0x06);
  out->elevation     = b[0x08];
  out->movement_type = b[0x09];
  out->range_x       = (uint8_t)(b[0x0A] & 0x0F);
  out->range_y       = (uint8_t)(b[0x0A] >> 4);
  out->trainer_type  = rd16(b + 0x0C);
  out->sight         = rd16(b + 0x0E);
  out->script        = rd32(b + 0x10);
  out->flag_id       = rd16(b + 0x14);
  return true;
}

bool rom_warp(const RomCtx* c, const RomMapEvents* ev, int i, RomWarp* out) {
  if (!c || !ev || !out || i < 0 || i >= ev->warp_count) return false;
  uint8_t b[8];
  if (!rom_read_at(c, ev->warps + (uint32_t)i * 8u, b, sizeof b)) return false;
  out->warp_id      = (uint8_t)i;
  out->x            = (int16_t)rd16(b + 0);
  out->y            = (int16_t)rd16(b + 2);
  out->elevation    = b[4];
  out->dest_warp_id = b[5];
  out->dest_map     = b[6];
  out->dest_group   = b[7];
  return true;
}

int rom_connection_count(const RomCtx* c, uint32_t connections_addr) {
  if (!c || !connections_addr || !rom_ptr_ok(c, connections_addr)) return 0;
  uint8_t b[8];
  if (!rom_read_at(c, connections_addr, b, sizeof b)) return 0;
  uint32_t n = rd32(b);
  uint32_t list = rd32(b + 4);
  if (n == 0 || n > 16 || !rom_ptr_ok(c, list)) return 0;
  return (int)n;
}

bool rom_connection(const RomCtx* c, uint32_t connections_addr, int i, RomConnection* out) {
  int n = rom_connection_count(c, connections_addr);
  if (!out || i < 0 || i >= n) return false;
  uint8_t hdr[8];
  if (!rom_read_at(c, connections_addr, hdr, sizeof hdr)) return false;
  uint32_t list = rd32(hdr + 4);
  uint8_t b[12];
  if (!rom_read_at(c, list + (uint32_t)i * 12u, b, sizeof b)) return false;
  out->direction = rd32(b + 0);
  out->offset    = (int32_t)rd32(b + 4);
  out->group     = b[8];
  out->num       = b[9];
  return out->direction >= ROM_CONN_SOUTH && out->direction <= ROM_CONN_EMERGE;
}

bool rom_blocks(const RomCtx* c, const RomLayout* l, uint32_t first, uint16_t* dst, uint32_t n) {
  if (!c || !l || !dst || !n) return false;
  uint32_t cells = (uint32_t)l->width * (uint32_t)l->height;
  if (first >= cells || n > cells - first) return false;
  if (!rom_read_at(c, l->blocks + first * 2u, dst, n * 2u)) return false;
#if !defined(__ARMEL__) && !defined(__LITTLE_ENDIAN__) && !defined(_WIN32) && !defined(__x86_64__) && !defined(__aarch64__)
  /* Every target we build for is little-endian, same as the ROM; this is only here
   * so a big-endian host test would still be correct rather than silently wrong. */
  for (uint32_t i = 0; i < n; i++) {
    uint8_t* p = (uint8_t*)&dst[i];
    dst[i] = (uint16_t)(p[0] | (p[1] << 8));
  }
#endif
  return true;
}

/* Resolve which tileset a metatile id belongs to and where its 8 entries live.
 * ids below NUM_METATILES_IN_PRIMARY come from the primary tileset; the rest are
 * offset into the secondary. That boundary is 512 on RSE and 640 on FRLG. */
static bool metatile_src(const RomCtx* c, const RomLayout* l, uint16_t id,
                         uint32_t* base_metatiles, uint32_t* base_attrs, uint32_t* index) {
  RomTileset ts;
  uint16_t prim = c->fmt.metatiles_primary;
  if (id < prim) {
    if (!rom_tileset(c, l->tileset_primary, &ts)) return false;
    *index = id;
  } else {
    if (!rom_tileset(c, l->tileset_secondary, &ts)) return false;
    *index = (uint32_t)(id - prim);
  }
  *base_metatiles = ts.metatiles;
  *base_attrs     = ts.attributes;
  return true;
}

bool rom_metatile(const RomCtx* c, const RomLayout* l, uint16_t metatile_id, uint16_t out[8]) {
  uint32_t mt, at, idx;
  if (!c || !l || !out) return false;
  if (!metatile_src(c, l, metatile_id, &mt, &at, &idx)) return false;
  if (!rom_ptr_ok(c, mt)) return false;
  uint8_t b[16];
  if (!rom_read_at(c, mt + idx * 16u, b, sizeof b)) return false;
  for (int i = 0; i < 8; i++) out[i] = rd16(b + i * 2);
  return true;
}

bool rom_metatile_attr(const RomCtx* c, const RomLayout* l, uint16_t metatile_id,
                       RomMetatileAttr* out) {
  uint32_t mt, at, idx;
  if (!c || !l || !out) return false;
  if (!metatile_src(c, l, metatile_id, &mt, &at, &idx)) return false;
  if (!rom_ptr_ok(c, at)) return false;
  uint8_t b[4] = { 0, 0, 0, 0 };
  uint32_t w = c->fmt.attr_bytes;               /* 2 on RSE, 4 on FRLG */
  if (!rom_read_at(c, at + idx * w, b, w)) return false;

  /* FRLG did NOT merely widen the field — it MOVED the layer type. Measured over every
   * attribute in both ROMs: Emerald's layer bits are 12-15 with 29-30 always zero
   * (9689 NORMAL / 8538 COVERED / 31 SPLIT), FireRed's are 29-30 with 12-15 always
   * zero (4664 / 5488 / 3). Truncating FRLG's u32 to a u16 — which this function used
   * to do — reports NORMAL for every FireRed metatile. */
  if (w == 4) {
    out->raw        = rd32(b);
    out->behavior   = (uint16_t)(out->raw & 0x01FFu);          /* bits 0-8  */
    out->layer_type = (uint8_t)((out->raw >> 29) & 0x3u);      /* bits 29-30 */
  } else {
    out->raw        = rd16(b);
    out->behavior   = (uint16_t)(out->raw & 0x00FFu);          /* bits 0-7   */
    out->layer_type = (uint8_t)((out->raw >> 12) & 0xFu);      /* bits 12-15 */
  }
  return true;
}

/* ---- connection geometry ---------------------------------------------------
 * Derived from pret/pokeemerald src/fieldmap.c. The four fill functions place the
 * neighbour's blockdata into the backup grid at these origins; expressing it as a closed
 * form lets both the renderer and the crossing logic share one definition. */
bool rom_conn_origin(int dir, int32_t off, int32_t w, int32_t h,
                     int32_t nb_w, int32_t nb_h, int32_t* ox, int32_t* oy) {
  if (!ox || !oy) return false;
  switch (dir) {
    case ROM_CONN_SOUTH: *ox = off;    *oy = h;      return true;
    case ROM_CONN_NORTH: *ox = off;    *oy = -nb_h;  return true;
    case ROM_CONN_WEST:  *ox = -nb_w;  *oy = off;    return true;
    case ROM_CONN_EAST:  *ox = w;      *oy = off;    return true;
    default: return false;             /* DIVE/EMERGE are not spatial neighbours */
  }
}

bool rom_conn_contains(int dir, int32_t off, int32_t nb_w, int32_t nb_h, int32_t along) {
  int32_t span;
  switch (dir) {
    case ROM_CONN_SOUTH: case ROM_CONN_NORTH: span = nb_w; break;
    case ROM_CONN_WEST:  case ROM_CONN_EAST:  span = nb_h; break;
    default: return false;
  }
  return along >= off && along < off + span;
}

/* ---- warp destinations ----------------------------------------------------- */
int rom_warp_dest(const RomCtx* c, const RomWarp* w,
                  uint8_t* og, uint8_t* on, int32_t* ox, int32_t* oy) {
  if (!c || !w || !og || !on || !ox || !oy) return ROM_WARP_UNREADABLE;

  /* MAP_DYNAMIC is tested on mapNum ALONE in the game (field_control_avatar.c never looks
   * at the group), and the destination then comes from runtime state we do not have.
   * Requiring group == 0x7F too would follow a hacked 0x12/0x7F warp into
   * gMapGroups[0x12][0x7F], which is an unbounded dereference. */
  if (w->dest_map == 0x7F) return ROM_WARP_DYNAMIC;
  if (w->dest_group == 0xFF && w->dest_map == 0xFF) return ROM_WARP_DUMMY;
  if (w->dest_group >= c->group_count) return ROM_WARP_BAD_MAP;
  int n = rom_maps_in_group(c, w->dest_group);
  if (n <= 0 || w->dest_map >= n) return ROM_WARP_BAD_MAP;

  RomMapHeader h; RomLayout l;
  if (!rom_map_header(c, w->dest_group, w->dest_map, &h)) return ROM_WARP_UNREADABLE;
  if (!rom_layout(c, h.layout, &l) || l.width <= 0 || l.height <= 0) return ROM_WARP_UNREADABLE;

  /* An out-of-range destination warp id is NOT an error: the game's guard is
   * `warpId >= 0 && warpId < warpCount` on a SIGNED s8, so 0xFF (-1), 0x7E, 0x7F and
   * anything past the count all fall through to the map CENTRE. Refusing instead of
   * centring would diverge from what the game actually does. */
  int32_t x = l.width / 2, y = l.height / 2;
  RomMapEvents ev; RomWarp d;
  if (rom_map_events(c, h.events, &ev) && w->dest_warp_id < ev.warp_count
      && rom_warp(c, &ev, w->dest_warp_id, &d)) { x = d.x; y = d.y; }

  /* RETAIL WARPS REALLY DO LAND OFF THE MAP — Emerald/Ruby SlateportCity warp 9 sits at
   * x=40 on a 40-wide layout and both Harbor warps point at it. Clamp, always. */
  if (x < 0) x = 0;
  if (x >= l.width)  x = l.width  - 1;
  if (y < 0) y = 0;
  if (y >= l.height) y = l.height - 1;

  *og = w->dest_group; *on = w->dest_map; *ox = x; *oy = y;
  return ROM_WARP_OK;
}

/* Map types that count as "outdoors" for the section lookup (include/constants/map_types.h):
 * 1 TOWN, 2 CITY, 3 ROUTE, 6 OCEAN_ROUTE. 4 UNDERGROUND / 5 UNDERWATER / 8 INDOOR /
 * 9 SECRET_BASE are all places you would not expect to land in from a region map. */
static bool map_type_outdoor(uint8_t t) {
  return t == 1 || t == 2 || t == 3 || t == 6;
}

bool rom_find_map_by_mapsec(const RomCtx* c, uint8_t mapsec, uint8_t* og, uint8_t* on) {
  if (!c || !og || !on) return false;
  bool have_any = false;
  uint8_t ag = 0, an = 0;
  for (int g = 0; g < c->group_count; g++) {
    int n = rom_maps_in_group(c, g);
    for (int m = 0; m < n; m++) {
      RomMapHeader h;
      if (!rom_map_header(c, g, m, &h)) continue;
      if (h.mapsec != mapsec) continue;
      if (map_type_outdoor(h.map_type)) { *og = (uint8_t)g; *on = (uint8_t)m; return true; }
      if (!have_any) { ag = (uint8_t)g; an = (uint8_t)m; have_any = true; }
    }
  }
  if (have_any) { *og = ag; *on = an; return true; }
  return false;
}

bool rom_bg_event(const RomCtx* c, const RomMapEvents* ev, int i, RomBgEvent* out) {
  if (!c || !ev || !out || i < 0 || i >= ev->bg_count || !rom_ptr_ok(c, ev->bgs)) return false;
  uint8_t b[12];
  if (!rom_read_at(c, ev->bgs + (uint32_t)i * 12u, b, sizeof b)) return false;
  out->x         = (int16_t)rd16(b + 0);
  out->y         = (int16_t)rd16(b + 2);
  out->elevation = b[4];
  out->kind      = b[5];
  out->param     = rd32(b + 8);          /* bytes 6-7 are padding */
  return true;
}
