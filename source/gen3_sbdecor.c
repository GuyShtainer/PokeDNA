/*
 * Secret-base decoration compositing. See gen3_sbdecor.h for the format and the reasoning.
 * Pure C: only <stdint.h>/<string.h> and rom_map.h, so tests/host_* can run it on the PC.
 */
#include "gen3_sbdecor.h"
#include <string.h>

#define SBD_ENTRIES     121      /* gDecorations, in every RSE and FRLG build */
#define SBD_MAX_ID      120      /* DECOR_REGISTEEL_DOLL; ids run 1..120      */
#define SBD_ENTRY_BYTES  32
#define SBD_OFF_PERM     17      /* 0x11 */
#define SBD_OFF_SHAPE    18      /* 0x12 */
#define SBD_OFF_TILES    28      /* 0x1C, u32 ROM pointer to the metatile list */

/* DECORPERM_*. pokeruby calls 4 SOLID_MAT and pokeemerald calls it SPRITE; same value, and
 * Emerald's name is the accurate one — those decorations are object events, not metatiles. */
#define DECORPERM_PASS_FLOOR 1
#define DECORPERM_NA_WALL    3
#define DECORPERM_SPRITE     4

/* Metatile behaviours. RSE and FRLG disagree on 41 behaviour names, but on none that this
 * code touches — and FRLG has no secret bases anyway. */
#define MB_SB_NORTH_WALL  0xB7
#define MB_SB_IMPASSABLE  0xB9
/* MB_HOLDS_SMALL_DECORATION / MB_HOLDS_LARGE_DECORATION. pokeruby calls these
 * MB_SECRET_BASE_LARGE_MAT_EDGE / MB_LARGE_MAT_CENTER — same numbers, same meaning. */
#define MB_HOLDS_SMALL    0xB5
#define MB_HOLDS_LARGE    0xC3

#define CELL_ID_MASK    0x03FFu
#define CELL_COLL_MASK  0x0C00u
#define CELL_ELEV_MASK  0xF000u

/* DECORSHAPE_*, width first. Shapes 2 (3x1) and 6 (1x3) exist but no decoration uses them. */
static const uint8_t k_shape_w[10] = { 1, 2, 3, 4, 2, 1, 1, 2, 3, 3 };
static const uint8_t k_shape_h[10] = { 1, 1, 1, 2, 2, 2, 3, 4, 3, 2 };

/* The only two decorations the game places with MapGridSetMetatileEntryAt, which rewrites
 * the WHOLE cell including elevation. Everything else goes through ...MetatileIdAt and keeps
 * the elevation the template had. Indexed by footprint cell (row*w + col); both are 8 cells.
 * Tables byte-identical in Ruby, Sapphire and Emerald. */
#define DECOR_SLIDE 34
#define DECOR_STAND 38
static const uint8_t k_stand_elev[8] = { 4, 4, 4, 4, 0, 3, 3, 0 };
static const uint8_t k_slide_elev[8] = { 4, 4, 4, 4, 0, 4, 3, 0 };

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- finding gDecorations ----------------------------------------------------
 * This code runs on a GBA reading the ROM off a microSD card, so sweeping 16 MB for a
 * signature is not an option. Resolution order, cheapest first, every candidate validated:
 *
 *   1. sGFRomHeader. Emerald and FRLG carry Game Freak's own index block at the FIXED link
 *      address 0x08000100, and its field at +0x4C is a pointer to gDecorations. That makes
 *      Emerald — the game where secret bases actually matter — revision-proof with one
 *      4-byte read. Ruby and Sapphire predate the block and must stay pinned.
 *   2. The exact (kind, revision) pin.
 *   3. Any pin for the same game, which covers an unknown revision byte.
 *
 * There is deliberately NO scan fallback: a base drawn with the wrong table would be worse
 * than the bare template, and the template is always available. */
static bool ptr_ok(const RomCtx* c, uint32_t p) {
  return p >= 0x08000000u && (p - 0x08000000u) < c->size;
}

/* The signature is entries 0 and 1, and the strong part is an invariant rather than any
 * game's text: gDecorations[0] (DECOR_NONE) was given the SAME name, description pointer and
 * tiles pointer as gDecorations[1] (DECOR_SMALL_DESK), differing only in id and price.
 * MEASURED over all five of Guy's ROMs at every byte offset: exactly one match each, and it
 * rejects all ten other games' pinned addresses and every real inter-revision displacement. */
static bool sbd_plausible(const RomCtx* c, uint32_t addr) {
  uint8_t b[64];
  if (!addr || !ptr_ok(c, addr)) return false;
  if (!rom_read_at(c, addr, b, sizeof b)) return false;

  if (b[0] != 0 || b[32] != 1) return false;
  if (memcmp(b + 1, b + 33, 16) != 0) return false;      /* same 16-byte name        */
  if (memcmp(b + 24, b + 56, 8) != 0) return false;      /* same desc + tiles ptrs   */
  for (int i = 17; i < 24; i++) if (b[i]) return false;  /* entry 0 is all zeroes    */
  if (b[49] || b[50] || b[51] || b[54] || b[55]) return false;
  if (rd16(b + 52) != 3000) return false;                /* SMALL_DESK costs 3000    */
  return ptr_ok(c, rd32(b + 24)) && ptr_ok(c, rd32(b + 28));
}

/* Game Freak's ROM index block, linked at a fixed address in Emerald/FireRed/LeafGreen. */
static uint32_t sbd_from_rom_header(const RomCtx* c) {
  uint8_t h[0x50];
  if (!rom_read_at(c, 0x08000100u, h, sizeof h)) return 0;
  uint32_t version = rd32(h + 0), language = rd32(h + 4);
  if (version < 1 || version > 8 || language < 1 || language > 8) return 0;
  if (memcmp(h + 8, "pokemon ", 8) != 0) return 0;
  return rd32(h + 0x4C);
}

typedef struct { uint8_t kind, version; uint32_t addr; } SbdAnchor;

/* MEASURED against Guy's own dumps for Emerald r0, Ruby r2, Sapphire r1, FireRed r1 and
 * LeafGreen r0 (a 121-entry scan found exactly one candidate in each 16 MB ROM). The other
 * six rows come from pret's per-revision byte-matching .sym files — the same source
 * region_map.c and rom_map.c already trust, and it agreed with all five measurements.
 *
 * FRLG has no secret bases, so its rows can never be reached through sbdecor_apply. They are
 * here so the resolver is uniform and so a future FRLG use has a checked answer.
 *
 * Do NOT try to derive these from region_map.c's anchors: the rev0->rev1 delta matches the
 * region-map delta for Ruby, Sapphire and LeafGreen but is 0x60 rather than 0x70 for
 * FireRed, so the "same delta" shortcut lands 0x10 short there. */
static const SbdAnchor k_anchor[] = {
  { ROM_EMERALD,   0, 0x085A5C08u },
  { ROM_RUBY,      0, 0x083EB6C4u },
  { ROM_RUBY,      1, 0x083EB6E0u },
  { ROM_RUBY,      2, 0x083EB6E0u },
  { ROM_SAPPHIRE,  0, 0x083EB71Cu },
  { ROM_SAPPHIRE,  1, 0x083EB73Cu },
  { ROM_SAPPHIRE,  2, 0x083EB73Cu },
  { ROM_FIRERED,   0, 0x084556F8u },
  { ROM_FIRERED,   1, 0x08455758u },
  { ROM_LEAFGREEN, 0, 0x08455118u },
  { ROM_LEAFGREEN, 1, 0x08455188u },
};

uint32_t sbdecor_table(const RomCtx* c) {
  if (!c) return 0;

  if (c->kind == ROM_EMERALD || c->kind == ROM_FIRERED || c->kind == ROM_LEAFGREEN) {
    uint32_t a = sbd_from_rom_header(c);
    if (sbd_plausible(c, a)) return a;
  }
  uint32_t family = 0;
  for (unsigned i = 0; i < sizeof k_anchor / sizeof k_anchor[0]; i++) {
    if (k_anchor[i].kind != (uint8_t)c->kind) continue;
    if (k_anchor[i].version == c->version && sbd_plausible(c, k_anchor[i].addr))
      return k_anchor[i].addr;
    if (!family) family = k_anchor[i].addr;
  }
  if (sbd_plausible(c, family)) return family;
  return 0;
}

/* ---- compositing ------------------------------------------------------------- */

/* Secondary-tileset metatile ids inside a secret base. Identical in Ruby, Sapphire and
 * Emerald (pokeemerald METATILE_SecretBase_PC / _RegisterPC; pokeruby's literals 0x220/0x221). */
#define MT_SB_PC          0x220
#define MT_SB_REGISTER_PC 0x221

bool sbdecor_apply(const RomCtx* c, const RomLayout* lay, const uint8_t* rec, int slot,
                   uint16_t* cells, SbDecorStat* stat) {
  SbDecorStat st;
  memset(&st, 0, sizeof st);
  if (stat) *stat = st;
  if (!c || !lay || !rec || !cells) return false;

  uint32_t table = sbdecor_table(c);
  if (!table) return false;

  /* 512 on RSE. Decoration tile lists index the SECONDARY tileset, so the absolute metatile
   * id is primary_count + rel. Taken from the format rather than written as 512 because the
   * constant is a property of the ROM, not of this algorithm. */
  uint16_t mtp = c->fmt.metatiles_primary;

  for (int i = 0; i < SBD_SLOTS; i++) {
    uint8_t id  = rec[0x12 + i];
    uint8_t pos = rec[0x22 + i];
    if (!id || id > SBD_MAX_ID) continue;            /* 0 is "empty slot", not an error */

    uint8_t ent[SBD_ENTRY_BYTES];
    if (!rom_read_at(c, table + (uint32_t)id * SBD_ENTRY_BYTES, ent, sizeof ent)) {
      st.bad++;
      continue;
    }
    uint8_t perm = ent[SBD_OFF_PERM], shape = ent[SBD_OFF_SHAPE];

    /* SPRITE decorations are dolls and cushions, spawned as object events. Skipping them is
     * LOAD-BEARING, not an optimisation: for those entries `tiles` is not a metatile array
     * at all, it is a single u16 object-event graphics id, and `shape` is meaningless (nine
     * of them claim 1x2 with two bytes of data). Reading a footprint out of one yields
     * garbage. 45 of the 120 decorations are SPRITE. */
    if (perm == DECORPERM_SPRITE) { st.sprites++; continue; }
    if (shape >= 10)              { st.bad++;     continue; }

    uint32_t tiles = rd32(ent + SBD_OFF_TILES);
    int w = k_shape_w[shape], h = k_shape_h[shape];
    int mx = pos >> 4, my = pos & 0x0F;
    const uint8_t* elev_tab = (id == DECOR_STAND) ? k_stand_elev
                            : (id == DECOR_SLIDE) ? k_slide_elev : 0;

    /* The stored position is the decoration's BOTTOM-left cell; it grows upward. */
    for (int row = 0; row < h; row++) {
      int y = my - h + 1 + row;
      for (int col = 0; col < w; col++) {
        int x = mx + col;
        if (x < 0 || y < 0 || x >= lay->width || y >= lay->height) { st.bad++; continue; }

        int fi = row * w + col;
        uint8_t tb[2];
        if (!rom_read_at(c, tiles + 2u * (uint32_t)fi, tb, 2)) { st.bad++; continue; }
        uint16_t rel = rd16(tb);

        uint32_t idx = (uint32_t)y * (uint32_t)lay->width + (uint32_t)x;

        /* The wall variant. `+1` is the twin of the decoration metatile drawn against the
         * base's back wall, and it is selected by what is ALREADY on the cell. The
         * NA_WALL exclusion is load-bearing: wall posters occupy consecutive ids, so a
         * stray +1 there would draw the NEXT POSTER rather than a variant of this one. */
        int overlaps = 0;
        if (perm != DECORPERM_NA_WALL) {
          RomMetatileAttr a;
          if (rom_metatile_attr(c, lay, ROM_CELL_METATILE(cells[idx]), &a) &&
              a.behavior == MB_SB_NORTH_WALL)
            overlaps = 1;
        }

        /* Collision, from the attributes of the decoration's OWN metatile (no +1). The game
         * recomputes this rather than inheriting it, and it matters here: PokeDNA reports
         * "coll" in the HUD and warns before dropping the player on a blocked tile, and
         * keeping the template's stale collision was wrong on 70.7% of decorated cells —
         * always in the direction of calling a blocked tile walkable. */
        uint16_t impass = 0;
        {
          RomMetatileAttr da;
          if (rom_metatile_attr(c, lay, (uint16_t)(mtp + rel), &da)) {
            if (da.behavior == MB_SB_IMPASSABLE ||
                (perm != DECORPERM_PASS_FLOOR && da.layer_type != 0))
              impass = CELL_COLL_MASK;
          }
        }

        uint16_t val = (uint16_t)(((rel + (mtp | overlaps)) & CELL_ID_MASK) | impass);
        if (elev_tab)
          cells[idx] = (uint16_t)(val | ((uint16_t)elev_tab[fi & 7] << 12));
        else
          cells[idx] = (uint16_t)((cells[idx] & CELL_ELEV_MASK) | val);
      }
    }
    st.placed++;
  }

  /* InitSecretBaseAppearance does one more thing after the decoration loop: in ANOTHER
   * player's base it swaps the PC for the REGISTER PC — a different graphic, and impassable.
   * Slot 0 is always the player's own base, so this fires for every record-mixed one; in
   * Guy's saves that is four of the six real bases, each a visible one-tile difference.
   * MEASURED: every one of the 24 interiors contains exactly one PC cell. */
  if (slot > 0) {
    uint32_t n = (uint32_t)lay->width * (uint32_t)lay->height;
    for (uint32_t i = 0; i < n; i++) {
      if (ROM_CELL_METATILE(cells[i]) != MT_SB_PC) continue;
      cells[i] = (uint16_t)((cells[i] & CELL_ELEV_MASK) | MT_SB_REGISTER_PC | CELL_COLL_MASK);
      break;
    }
  }

  if (stat) *stat = st;
  return true;
}

int sbdecor_sprites(const RomCtx* c, const RomLayout* lay, const uint8_t* rec,
                    const uint16_t* cells, SbDecorSprite* out) {
  int n = 0;
  if (!c || !lay || !rec || !cells || !out) return 0;
  uint32_t table = sbdecor_table(c);
  if (!table) return 0;

  for (int i = 0; i < SBD_SLOTS && n < SBD_MAX_SPRITES; i++) {
    uint8_t id  = rec[0x12 + i];
    uint8_t pos = rec[0x22 + i];
    if (!id || id > SBD_MAX_ID) continue;

    uint8_t ent[SBD_ENTRY_BYTES];
    if (!rom_read_at(c, table + (uint32_t)id * SBD_ENTRY_BYTES, ent, sizeof ent)) continue;
    if (ent[SBD_OFF_PERM] != DECORPERM_SPRITE) continue;

    int x = pos >> 4, y = pos & 0x0F;
    if (x < 0 || y < 0 || x >= lay->width || y >= lay->height) continue;

    /* The game's own gate. The cell has to be something that HOLDS a decoration, and that
     * behaviour is only there because a metatile decoration put it there — so a doll whose
     * desk was never placed is not drawn, exactly as in the game. */
    RomMetatileAttr a;
    uint16_t cell = cells[(uint32_t)y * (uint32_t)lay->width + (uint32_t)x];
    if (!rom_metatile_attr(c, lay, ROM_CELL_METATILE(cell), &a)) continue;
    if (a.behavior != MB_HOLDS_SMALL && a.behavior != MB_HOLDS_LARGE) continue;

    /* For a SPRITE decoration `tiles` does not point at metatiles at all: tiles[0] IS the
     * object-event graphics id the game feeds to VAR_OBJ_GFX_ID_n. */
    uint8_t gb[2];
    if (!rom_read_at(c, rd32(ent + SBD_OFF_TILES), gb, 2)) continue;

    out[n].gfx = rd16(gb);
    out[n].x   = (int16_t)x;
    out[n].y   = (int16_t)y;
    n++;
  }
  return n;
}
