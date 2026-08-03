/* Gen-3 overworld NPC sprite extraction. See npc_gfx.h for the verified format,
 * the per-game divergences and the legal posture. Pure C: <stdint.h>/<string.h> only,
 * every ROM byte fetched through RomCtx.read, every followed pointer gated by
 * rom_ptr_ok(). Nothing here decompresses, allocates, or keeps a big buffer. */

#include <string.h>
#include "npc_gfx.h"

/* ---- little-endian helpers (never cast a ROM byte pointer to a wider type) --- */
static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* struct ObjectEventGraphicsInfo — 0x24 bytes, identical in RSE and FRLG. */
#define OEGI_SIZE          0x24
#define OEGI_PALETTE_TAG   0x02
#define OEGI_SIZE_FIELD    0x06   /* unreliable; see header */
#define OEGI_WIDTH         0x08
#define OEGI_HEIGHT        0x0A
#define OEGI_FLAGS         0x0C   /* paletteSlot:4 shadowSize:2 inanimate:1 noRefl:1 */
#define OEGI_TRACKS        0x0D
#define OEGI_OAM           0x10
#define OEGI_SUBSPRITES    0x14
#define OEGI_ANIMS         0x18
#define OEGI_IMAGES        0x1C
#define OEGI_AFFINE        0x20

/* struct SpriteFrameImage { const void *data; u16 size; } — 8 bytes with padding. */
#define SFI_SIZE           8

/* struct SpritePalette { const u16 *data; u16 tag; } — 8 bytes with padding. */
#define SPAL_SIZE          8
#define OBJ_PAL_TAG_MIN    0x1100u
#define OBJ_PAL_TAG_MAX    0x11FFu
#define OBJ_PAL_TAG_FIRST  0x1103u   /* NPC_1 — the run's fixed opening tag        */
#define OBJ_PAL_TABLE_MAX  256       /* sanity cap on the run length               */

/* Palette-table search window, relative to gObjectEventGraphicsInfoPointers.
 * Measured deltas: Emerald +0x65A8, Ruby rev2 +0x5B24, FireRed rev1 +0x53A8. The
 * backward slack is pure insurance against a build that emitted the two arrays in the
 * other order. */
#define SCAN_BACK   0x2000u
#define SCAN_FWD    0x18000u
#define SCAN_BUF    512          /* stack buffer; hard rule 2 — nothing multi-KB    */
#define SCAN_SIG    64           /* bytes the 8-record signature needs              */

const char* npc_gfx_err_str(NpcGfxErr e) {
  switch (e) {
    case NPC_OK:               return "ok";
    case NPC_ERR_NOT_OPEN:     return "npc_gfx not opened";
    case NPC_ERR_NO_PAL_TABLE: return "object-event palette table not found";
    case NPC_ERR_DYNAMIC_VAR:  return "dynamic id (set from a RAM var at runtime)";
    case NPC_ERR_ID_RANGE:     return "graphics id out of range for this game";
    case NPC_ERR_BAD_PTR:      return "pointer outside the ROM image";
    case NPC_ERR_BAD_DIMS:     return "implausible sprite dimensions";
    case NPC_ERR_BAD_FRAME:    return "frame size disagrees with the dimensions";
    case NPC_ERR_NO_PALETTE:   return "palette tag not in the palette table";
    case NPC_ERR_TOO_BIG:      return "does not fit the caller's buffer";
  }
  return "?";
}

/* NUM_OBJ_EVENT_GFX per game (pret constants, cross-checked against the pointer
 * tables in the retail ROMs). */
static uint16_t gfx_count_for(RomKind k) {
  switch (k) {
    case ROM_EMERALD:                 return 239;
    case ROM_RUBY: case ROM_SAPPHIRE: return 218;
    case ROM_FIRERED: case ROM_LEAFGREEN: return 152;
    default:                          return 0;
  }
}

/* ---- palette-table discovery ------------------------------------------------ */

/* Does a well-formed sObjectEventSpritePalettes run start at `addr`?
 * `b` holds SCAN_SIG bytes read from addr. */
static bool pal_sig_ok(const RomCtx* rom, const uint8_t* b) {
  for (int m = 0; m < 8; m++) {
    uint32_t p   = rd32(b + m * SPAL_SIZE);
    uint16_t tag = rd16(b + m * SPAL_SIZE + 4);
    uint16_t pad = rd16(b + m * SPAL_SIZE + 6);
    if (pad != 0) return false;
    if (tag != (uint16_t)(OBJ_PAL_TAG_FIRST + m)) return false;
    if (!rom_ptr_ok(rom, p) || (p & 1u)) return false;
    if (!rom_ptr_ok(rom, p + 31)) return false;
  }
  /* The first entry must really look like a 16-colour BGR555 palette: bit 15 of a
   * GBA colour is unused and is 0 in every palette the games ship. */
  uint8_t pal[32];
  uint32_t p0 = rd32(b);
  if (!rom_read_at(rom, p0, pal, 32)) return false;
  for (int i = 0; i < 16; i++)
    if (rd16(pal + i * 2) & 0x8000u) return false;
  return true;
}

/* Count consecutive valid {ptr, tag, 0} records from `addr`. */
static uint16_t pal_run_len(const RomCtx* rom, uint32_t addr) {
  uint8_t buf[SPAL_SIZE * 8];
  uint16_t n = 0;
  while (n < OBJ_PAL_TABLE_MAX) {
    uint32_t a = addr + (uint32_t)n * SPAL_SIZE;
    uint32_t want = SPAL_SIZE * 8;
    if (!rom_read_at(rom, a, buf, want)) {
      want = SPAL_SIZE;                      /* near the end of the image */
      if (!rom_read_at(rom, a, buf, want)) break;
    }
    uint16_t got = 0;
    for (uint32_t m = 0; m * SPAL_SIZE < want; m++) {
      uint32_t p   = rd32(buf + m * SPAL_SIZE);
      uint16_t tag = rd16(buf + m * SPAL_SIZE + 4);
      uint16_t pad = rd16(buf + m * SPAL_SIZE + 6);
      if (pad != 0 || tag < OBJ_PAL_TAG_MIN || tag > OBJ_PAL_TAG_MAX ||
          !rom_ptr_ok(rom, p) || (p & 1u)) { return (uint16_t)(n + got); }
      got++;
    }
    n = (uint16_t)(n + got);
    if (got * SPAL_SIZE < want) break;
  }
  return n;
}

static bool find_pal_table(const RomCtx* rom, uint32_t anchor,
                           uint32_t* out_addr, uint16_t* out_count) {
  uint32_t lo = (anchor > ROM_BASE + SCAN_BACK) ? anchor - SCAN_BACK : ROM_BASE;
  uint32_t hi = anchor + SCAN_FWD;
  uint32_t end_of_rom = ROM_BASE + rom->size;
  if (hi > end_of_rom) hi = end_of_rom;
  if (lo + SCAN_SIG > hi) return false;

  uint8_t buf[SCAN_BUF];
  uint32_t best_addr = 0; uint16_t best_len = 0;
  const uint32_t step = SCAN_BUF - SCAN_SIG;   /* overlap so no candidate is split */

  for (uint32_t a = lo; a + SCAN_SIG <= hi; a += step) {
    uint32_t want = SCAN_BUF;
    if (a + want > hi) want = hi - a;
    if (want < SCAN_SIG) break;
    if (!rom_read_at(rom, a, buf, want)) break;
    for (uint32_t o = 0; o + SCAN_SIG <= want; o += 4) {
      if (!pal_sig_ok(rom, buf + o)) continue;
      uint32_t cand = a + o;
      uint16_t n = pal_run_len(rom, cand);
      if (n > best_len) { best_len = n; best_addr = cand; }
    }
    /* Every retail table is 18 (FRLG) to 35 (Emerald) entries long, and the opening
     * signature is far too specific to hit by chance, so a run this long ends the
     * search instead of reading the rest of the window off the SD card for nothing. */
    if (best_len >= 16) break;
  }
  if (best_len < 8) return false;
  *out_addr = best_addr; *out_count = best_len;
  return true;
}

bool npc_gfx_open(NpcGfx* g, const RomCtx* rom) {
  if (!g) return false;
  memset(g, 0, sizeof *g);
  if (!rom || rom->kind == ROM_NONE) return false;
  if (!rom_ptr_ok(rom, rom->gfx_info_ptrs)) return false;

  g->rom       = rom;
  g->info_ptrs = rom->gfx_info_ptrs;
  g->gfx_count = gfx_count_for(rom->kind);
  if (!g->gfx_count) return false;

  /* The whole pointer table must be inside the image before we trust any of it. */
  if (!rom_ptr_ok(rom, g->info_ptrs + (uint32_t)g->gfx_count * 4u - 1u)) return false;

  if (!find_pal_table(rom, g->info_ptrs, &g->pal_table, &g->pal_count)) return false;

  g->ready = true;
  return true;
}

uint16_t npc_gfx_count(const NpcGfx* g) { return (g && g->ready) ? g->gfx_count : 0; }

/* ---- palette lookup --------------------------------------------------------- */

static bool pal_addr_for_tag(const NpcGfx* g, uint16_t tag, uint32_t* out) {
  uint8_t buf[SPAL_SIZE * 8];
  for (uint16_t i = 0; i < g->pal_count; i += 8) {
    uint16_t n = (uint16_t)(g->pal_count - i);
    if (n > 8) n = 8;
    uint32_t want = (uint32_t)n * SPAL_SIZE;
    if (!rom_read_at(g->rom, g->pal_table + (uint32_t)i * SPAL_SIZE, buf, want))
      return false;
    for (uint16_t m = 0; m < n; m++) {
      if (rd16(buf + m * SPAL_SIZE + 4) == tag) {
        uint32_t p = rd32(buf + m * SPAL_SIZE);
        if (!rom_ptr_ok(g->rom, p) || !rom_ptr_ok(g->rom, p + 31)) return false;
        *out = p;
        return true;
      }
    }
  }
  return false;
}

/* ---- resolution ------------------------------------------------------------- */

static bool fail(NpcGfxInfo* out, NpcGfxErr e) {
  memset(out, 0, sizeof *out);
  out->ok = false; out->err = e;
  return false;
}

/* Which ids the game re-points at runtime even though the static entry is valid. */
static bool id_is_dynamic(RomKind k, uint16_t gid) {
  bool rse = (k == ROM_EMERALD || k == ROM_RUBY || k == ROM_SAPPHIRE);
  if (rse && gid >= 60 && gid <= 62) return true;        /* berry trees            */
  if (k == ROM_EMERALD && gid == 69) return true;        /* Mauville old man/Bard  */
  return false;
}

/* The whole resolution, minus the "is this a VAR id?" gate — shared by the public
 * npc_gfx_info_dir (which refuses VAR ids) and npc_gfx_info_var (which substitutes). */
static bool resolve(const NpcGfx* g, uint16_t graphics_id, int dir, NpcGfxInfo* out) {
  if (!out) return false;
  if (!g || !g->ready)                    return fail(out, NPC_ERR_NOT_OPEN);
  if (graphics_id >= NPC_GFX_VARS_BASE)   return fail(out, NPC_ERR_DYNAMIC_VAR);
  if (graphics_id >= g->gfx_count)        return fail(out, NPC_ERR_ID_RANGE);
  if (dir < 0 || dir > 3) dir = NPC_DIR_SOUTH;

  const RomCtx* rom = g->rom;
  uint8_t b[OEGI_SIZE];

  uint32_t slot = g->info_ptrs + (uint32_t)graphics_id * 4u;
  if (!rom_read_at(rom, slot, b, 4))      return fail(out, NPC_ERR_BAD_PTR);
  uint32_t info = rd32(b);
  if (!rom_ptr_ok(rom, info) || !rom_ptr_ok(rom, info + OEGI_SIZE - 1))
                                          return fail(out, NPC_ERR_BAD_PTR);
  if (!rom_read_at(rom, info, b, OEGI_SIZE)) return fail(out, NPC_ERR_BAD_PTR);

  int16_t  w16 = (int16_t)rd16(b + OEGI_WIDTH);
  int16_t  h16 = (int16_t)rd16(b + OEGI_HEIGHT);
  if (w16 <= 0 || h16 <= 0 || (w16 & 7) || (h16 & 7) ||
      w16 > NPC_GFX_MAX_W || h16 > NPC_GFX_MAX_H) return fail(out, NPC_ERR_BAD_DIMS);

  memset(out, 0, sizeof *out);
  out->info_addr   = info;
  out->w           = (uint8_t)w16;
  out->h           = (uint8_t)h16;
  out->tiles_x     = (uint8_t)(w16 >> 3);
  out->tiles_y     = (uint8_t)(h16 >> 3);
  out->pal_tag     = rd16(b + OEGI_PALETTE_TAG);
  out->pal_slot    = (uint8_t)(b[OEGI_FLAGS] & 0x0Fu);
  out->shadow_size = (uint8_t)((b[OEGI_FLAGS] >> 4) & 0x03u);
  out->inanimate   = (b[OEGI_FLAGS] & 0x40u) != 0;
  out->tracks      = b[OEGI_TRACKS];
  out->dynamic     = id_is_dynamic(rom->kind, graphics_id);

  uint32_t anims  = rd32(b + OEGI_ANIMS);
  uint32_t images = rd32(b + OEGI_IMAGES);

  /* Frame index. Default to the standard overworld layout (0 south, 1 north,
   * 2 west, 2+hflip east), then let the ROM's own anim table override it — which is
   * what makes inanimate objects and the odd tables come out right. */
  uint16_t frame = (dir == NPC_DIR_EAST) ? 2u : (uint16_t)dir;
  bool     hflip = (dir == NPC_DIR_EAST);
  if (rom_ptr_ok(rom, anims) && rom_ptr_ok(rom, anims + (uint32_t)dir * 4u + 3u)) {
    uint8_t p4[4];
    if (rom_read_at(rom, anims + (uint32_t)dir * 4u, p4, 4)) {
      uint32_t acmd = rd32(p4);
      if (rom_ptr_ok(rom, acmd) && rom_ptr_ok(rom, acmd + 3u) &&
          rom_read_at(rom, acmd, p4, 4)) {
        uint32_t cmd = rd32(p4);
        uint16_t iv  = (uint16_t)(cmd & 0xFFFFu);
        /* union AnimCmd: a FRAME command's low half is the image index; the LOOP/
         * JUMP/END commands are s16 type = -3/-2/-1, i.e. 0xFFFD..0xFFFF. */
        if (iv < 0xFFFDu) { frame = iv; hflip = ((cmd >> 22) & 1u) != 0; }
      }
    }
  }
  /* A real anim table never names a frame past ~40; refuse rather than clamp, so a
   * hacked table can never silently select a different sprite's tiles. */
  if (frame > 255u)                       return fail(out, NPC_ERR_BAD_FRAME);
  out->frame = (uint8_t)frame;
  out->hflip = hflip;

  uint32_t fent = images + (uint32_t)out->frame * SFI_SIZE;
  if (!rom_ptr_ok(rom, images) || !rom_ptr_ok(rom, fent + SFI_SIZE - 1))
                                          return fail(out, NPC_ERR_BAD_PTR);
  uint8_t fb[SFI_SIZE];
  if (!rom_read_at(rom, fent, fb, SFI_SIZE)) return fail(out, NPC_ERR_BAD_PTR);
  uint32_t fdata = rd32(fb);
  uint16_t fsize = rd16(fb + 4);

  uint32_t want = (uint32_t)out->w * (uint32_t)out->h / 2u;
  if (fsize != want)                      return fail(out, NPC_ERR_BAD_FRAME);
  if (!rom_ptr_ok(rom, fdata) || !rom_ptr_ok(rom, fdata + want - 1u))
                                          return fail(out, NPC_ERR_BAD_PTR);
  out->frame_addr  = fdata;
  out->frame_bytes = want;

  if (!pal_addr_for_tag(g, out->pal_tag, &out->pal_addr))
                                          return fail(out, NPC_ERR_NO_PALETTE);

  out->ok  = true;
  out->err = NPC_OK;
  return true;
}

bool npc_gfx_info_dir(const NpcGfx* g, uint16_t graphics_id, int dir, NpcGfxInfo* out) {
  return resolve(g, graphics_id, dir, out);
}

bool npc_gfx_info(const NpcGfx* g, uint16_t graphics_id, NpcGfxInfo* out) {
  return resolve(g, graphics_id, NPC_DIR_SOUTH, out);
}

int npc_gfx_var_index(uint16_t graphics_id) {
  if (graphics_id < NPC_GFX_VARS_BASE || graphics_id > 255) return -1;
  return (int)(graphics_id - NPC_GFX_VARS_BASE);
}

bool npc_gfx_info_var(const NpcGfx* g, uint16_t graphics_id, uint8_t var_value,
                      int dir, NpcGfxInfo* out) {
  uint16_t id = graphics_id;
  if (npc_gfx_var_index(graphics_id) >= 0) id = var_value;
  return resolve(g, id, dir, out);
}

bool npc_gfx_palette(const NpcGfx* g, const NpcGfxInfo* in, uint16_t pal[16]) {
  if (!g || !g->ready || !in || !in->ok || !pal) return false;
  uint8_t b[32];
  if (!rom_read_at(g->rom, in->pal_addr, b, 32)) return false;
  for (int i = 0; i < 16; i++) pal[i] = rd16(b + i * 2);
  return true;
}

bool npc_gfx_frame(const NpcGfx* g, const NpcGfxInfo* in, uint8_t* dst4bpp, uint32_t cap) {
  if (!g || !g->ready || !in || !in->ok || !dst4bpp) return false;
  if (in->frame_bytes > cap) return false;
  return rom_read_at(g->rom, in->frame_addr, dst4bpp, in->frame_bytes);
}

/* ---- compositing ------------------------------------------------------------ */

bool npc_gfx_blit(const NpcGfxInfo* in, const uint8_t* frame4bpp, uint32_t frame_len,
                  const uint16_t pal[16], uint16_t* dst, int stride) {
  if (!in || !in->ok || !frame4bpp || !pal || !dst) return false;
  if (frame_len < in->frame_bytes) return false;
  if (stride < (int)in->w) return false;      /* a row must fit the caller's buffer */

  for (int ty = 0; ty < in->tiles_y; ty++) {
    for (int tx = 0; tx < in->tiles_x; tx++) {
      const uint8_t* t = frame4bpp + ((ty * in->tiles_x) + tx) * 32;
      for (int yy = 0; yy < 8; yy++) {
        int y = ty * 8 + yy;
        for (int xx = 0; xx < 8; xx++) {
          uint8_t byte = t[yy * 4 + (xx >> 1)];
          uint8_t ci   = (xx & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
          if (!ci) continue;                       /* index 0 is transparent */
          int x = tx * 8 + xx;
          if (in->hflip) x = in->w - 1 - x;
          dst[y * stride + x] = pal[ci];
        }
      }
    }
  }
  return true;
}

bool npc_gfx_pixels(const NpcGfx* g, const NpcGfxInfo* in, uint16_t* dst, int stride) {
  if (!g || !g->ready || !in || !in->ok || !dst) return false;
  if (stride < (int)in->w) return false;
  uint16_t pal[16];
  if (!npc_gfx_palette(g, in, pal)) return false;

  uint8_t t[32];                                   /* one 8x8 tile — 32 bytes */
  for (int ty = 0; ty < in->tiles_y; ty++) {
    for (int tx = 0; tx < in->tiles_x; tx++) {
      uint32_t off = (uint32_t)((ty * in->tiles_x) + tx) * 32u;
      if (!rom_read_at(g->rom, in->frame_addr + off, t, 32)) return false;
      for (int yy = 0; yy < 8; yy++) {
        int y = ty * 8 + yy;
        for (int xx = 0; xx < 8; xx++) {
          uint8_t byte = t[yy * 4 + (xx >> 1)];
          uint8_t ci   = (xx & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0Fu);
          if (!ci) continue;
          int x = tx * 8 + xx;
          if (in->hflip) x = in->w - 1 - x;
          dst[y * stride + x] = pal[ci];
        }
      }
    }
  }
  return true;
}

/* ---- OAM geometry ----------------------------------------------------------- */

bool npc_gfx_oam_shape(uint8_t w, uint8_t h, uint8_t* shape, uint8_t* size) {
  uint8_t sh, sz;
  if      (w ==  8 && h ==  8) { sh = 0; sz = 0; }
  else if (w == 16 && h == 16) { sh = 0; sz = 1; }
  else if (w == 32 && h == 32) { sh = 0; sz = 2; }
  else if (w == 64 && h == 64) { sh = 0; sz = 3; }
  else if (w == 16 && h ==  8) { sh = 1; sz = 0; }
  else if (w == 32 && h ==  8) { sh = 1; sz = 1; }
  else if (w == 32 && h == 16) { sh = 1; sz = 2; }
  else if (w == 64 && h == 32) { sh = 1; sz = 3; }
  else if (w ==  8 && h == 16) { sh = 2; sz = 0; }
  else if (w ==  8 && h == 32) { sh = 2; sz = 1; }
  else if (w == 16 && h == 32) { sh = 2; sz = 2; }
  else if (w == 32 && h == 64) { sh = 2; sz = 3; }
  else return false;
  if (shape) *shape = sh;
  if (size)  *size  = sz;
  return true;
}
