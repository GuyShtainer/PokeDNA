/* The Gen-3 PC-storage pointer glove out of the user's own ROM — see rom_hand.h for
 * the design, the pin table's provenance, and the memory rules. */
#include "hand_gate.h"

#if !PDNA_HAND_ART_COMPILED   /* the whole reader: empty otherwise -- see the gate header.
                               * box_oam.c/pdna_main.c never call into this file at all
                               * when the real poses are linked in, so there is no
                               * "always needed, tiny" portion left ungated the way
                               * rom_chrome.c keeps rom_chrome_open() outside its own
                               * gate -- every call site here is ALREADY inside the
                               * matching #if in its own file. */

#include "rom_hand.h"
#include <string.h>

/* ---- per-revision pinned addresses -------------------------------------------
 *
 * Every row here was VERIFIED against Guy's own dump (gba-toolkit/roms/), not
 * transcribed from DESIGN.md's draft: the sheet bytes were memcmp'd identical across
 * all three games, and LeafGreen's palette (DESIGN.md flagged it "not probed") was
 * independently located by searching its image for the 32 raw bytes FireRed's
 * palette resolves to — exactly one hit, at the address below. See rom_hand.h's
 * header comment for the full method.
 *
 * MISSING ON PURPOSE: Ruby AXVE / Sapphire AXPE. R/S ship a DIFFERENT glove
 * (pokeruby's own hand_cursor.png); searching both dumps for this exact sheet gets
 * zero hits — confirmed, not assumed. rom_map.c's precedent stands: an address
 * nobody verified is worse than no feature, so R/S fail closed here rather than
 * guess at a different asset's location. Adding pokeruby's glove later is a fresh
 * shape scan, not a copy of this table. */
typedef struct {
  const char* code;    /* 4-char game code at 0xAC */
  uint8_t     version; /* revision byte at 0xBC    */
  uint32_t    sheet;
  uint32_t    pal;
} RomHandPin;

static const RomHandPin k_pins[] = {
  /*  code   rev  sheet        palette    */
  { "BPEE", 0, 0x0857B118, 0x085724D4 },   /* Emerald   */
  { "BPRE", 1, 0x083D2C5C, 0x083CE860 },   /* FireRed   */
  { "BPGE", 0, 0x083D2A28, 0x083CE62C },   /* LeafGreen */
};
#define K_NPINS ((int)(sizeof k_pins / sizeof k_pins[0]))

/* A ROM pointer is sane if it lands inside the image with `need` bytes of room. */
static int ptr_ok(const RomCtx* rc, uint32_t addr, uint32_t need) {
  if (!rc || addr < ROM_BASE) return 0;
  uint32_t off = addr - ROM_BASE;
  return off < rc->size && need <= rc->size - off;
}

static uint32_t hash32(const uint8_t* p, uint32_t n) {
  uint32_t h = 2166136261u;
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

/* Read n raw bytes, verifying by repetition when rh->verify is set — the same
 * fetch-twice-and-compare posture rom_itemart.c's read_verified() uses. Returns 1,
 * or 0 with dst's contents undefined. */
static int read_verified(const RomHand* rh, uint32_t addr, uint8_t* dst, uint32_t n) {
  if (!rom_read_at(rh->rc, addr, dst, n)) return 0;
  if (!rh->verify) return 1;
  uint32_t prev = hash32(dst, n);
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!rom_read_at(rh->rc, addr, dst, n)) return 0;
    uint32_t h = hash32(dst, n);
    if (h == prev) return 1;                    /* two reads agreed */
    prev = h;
  }
  return 0;                                     /* three tries, never twice the same */
}

/* One 4bpp pixel out of a 512 B 32x32 1D-tile-order frame (4x4 tiles, row-major). */
static uint8_t frame_px(const uint8_t* frame512, int x, int y) {
  int tx = x >> 3, ty = y >> 3;
  const uint8_t* t = frame512 + (ty * 4 + tx) * 32 + (y & 7) * 4;
  uint8_t b = t[(x & 7) >> 1];
  return (uint8_t)((x & 1) ? (b >> 4) : (b & 0x0F));
}

/* open()'s content signature: frame 0 is a 32x32 sprite that does NOT fill its cell
 * (all four corners transparent, real art near the centre) — cheap, content-based
 * confirmation this is really the glove and not a garbled/relocated read, the same
 * posture rom_itemart.c's FRLG badge corner check uses. Palette must look like a GBA
 * palette (bit 15 clear on every entry). */
static int validate(const RomHand* rh) {
  uint8_t f0[ROM_HAND_FRAME_BYTES];
  if (!read_verified(rh, rh->sheet, f0, sizeof f0)) return 0;
  if (frame_px(f0, 0, 0) || frame_px(f0, 31, 0) ||
      frame_px(f0, 0, 31) || frame_px(f0, 31, 31)) return 0;
  if (!frame_px(f0, 16, 16)) return 0;
  uint8_t praw[32];
  if (!read_verified(rh, rh->pal, praw, sizeof praw)) return 0;
  for (int i = 0; i < 16; i++) if (praw[i * 2 + 1] & 0x80) return 0;
  return 1;
}

int rom_hand_open(RomHand* rh, const RomCtx* rc) {
  if (!rh) return 0;
  memset(rh, 0, sizeof *rh);
  rh->verify = 1;                       /* fail safe: verification on until told otherwise */
  rh->rc = rc;
  if (!rc || !rc->read) return 0;

  const RomHandPin* pin = 0;
  for (int i = 0; i < K_NPINS; i++)
    if (memcmp(k_pins[i].code, rc->code, 4) == 0 && k_pins[i].version == rc->version) {
      pin = &k_pins[i]; break;
    }
  if (!pin) return 0;                   /* unpinned revision (incl. Ruby/Sapphire) */

  if (!ptr_ok(rc, pin->sheet, ROM_HAND_FRAMES * ROM_HAND_FRAME_BYTES) ||
      !ptr_ok(rc, pin->pal, 32)) return 0;

  rh->sheet = pin->sheet;
  rh->pal   = pin->pal;
  rh->ok    = validate(rh);
  if (!rh->ok) { rh->sheet = 0; rh->pal = 0; }
  return rh->ok;
}

void rom_hand_set_verify(RomHand* rh, int on) { if (rh) rh->verify = on ? 1 : 0; }

int rom_hand_have(const RomHand* rh) { return (rh && rh->ok) ? 1 : 0; }

int rom_hand_frame(const RomHand* rh, uint8_t frame, uint8_t dst[ROM_HAND_FRAME_BYTES]) {
  if (!rom_hand_have(rh) || !dst || frame >= ROM_HAND_FRAMES) return 0;
  return read_verified(rh, rh->sheet + (uint32_t)frame * ROM_HAND_FRAME_BYTES,
                       dst, ROM_HAND_FRAME_BYTES);
}

int rom_hand_pal(const RomHand* rh, uint16_t dst[16]) {
  if (!rom_hand_have(rh) || !dst) return 0;
  uint8_t raw[32];
  if (!read_verified(rh, rh->pal, raw, sizeof raw)) return 0;
  for (int i = 0; i < 16; i++)
    dst[i] = (uint16_t)(raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
  return 1;
}

#endif /* !PDNA_HAND_ART_COMPILED */
