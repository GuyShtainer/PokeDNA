/*
 * Hardware sprites for the overworld map screen — see map_oam.h for the contract.
 *
 * NPCs, the player, and the grab/carry/drop effects are GBA OBJ. The map's two metatile
 * BGs (or the affine BG when zoomed) are untouched by this file: everything here is OAM,
 * OBJ tile VRAM and OBJ PALRAM.
 *
 * ---- OBJ TILE BUDGET (the single most important thing in this file) ---------
 * map_gfx.c parks the metatile lookup tables at 0x06010000..0x06013FFF, which IS OBJ tile
 * ids 0..511. A sprite tile written below id 512 silently scrambles the map. Every tile id
 * this file can ever produce is therefore >= 512, and every allocation is bounds-checked
 * against the region below:
 *
 *   512..975   464 tiles  NPC sprite pool — bump-allocated PER GRAPHICS ID (deduped),
 *                         reset on every moam_load_map()
 *   976..991    16 tiles  the player's sprite (up to 32x32) — NEVER reset by a map load,
 *                         because moam_set_player() may run before or after it
 *   992..993     2 tiles  the zoomed-out dots (NPC, player)
 *   994..995     2 tiles  sweat droplet, 2 frames
 *   996..1011   16 tiles  dust puff, 4 expanding-ring frames of 16x16
 *   1012..1023  12 tiles  spare
 *
 * Measured worst real map: 46 objects / 24 distinct graphics ids / 5 distinct palettes.
 * 24 ids at the common 16x16 (4 tiles) or 16x32 (8 tiles) is 96-192 of the 464 available.
 * Running out is still handled: the sprite is skipped and that NPC falls back to a dot.
 *
 * ---- OBJ PALETTE BANKS ------------------------------------------------------
 *   0..13  NPC palettes. Keyed on the game's own paletteSlot (per map_oam.h), with a
 *          collision/overflow fallback to any free bank. Reset on every map load.
 *   14     the player (kept out of the reset so a map load cannot recolour it)
 *   15     ALL the procedural effects (dots, sweat, puff) share one bank
 *
 * ---- OAM ENTRIES (lower index draws IN FRONT for equal priority) -----------
 *   0      sweat droplet          1  player      2..4  puff rings
 *   5..52  the 48 NPC slots
 * All at OBJ priority 1: in front of BG1 (prio 1) and BG3/BG2 (prio 2), behind the HUD
 * on BG0 (prio 0), so map text is never covered.
 *
 * ---- VRAM/OAM/PALRAM WRITE WIDTH -------------------------------------------
 * Video memory turns an 8-bit store into a halfword written to BOTH halves (GBATEK, "8bit
 * Writes to Video Memory"). Nothing in this file does a byte store into VRAM/OAM/PALRAM:
 * tile uploads go through dma3_cpy (32-bit), procedurally generated tiles are built in RAM
 * and written as u32 words, OAM attributes and palette entries are u16 stores.
 *
 * ---- OS-MODE ----------------------------------------------------------------
 * moam_init(), moam_set_player() and moam_load_map() read the user's ROM off the SD card
 * through npc_gfx/rom_map, so they must be called from the same blanked/paused window as
 * mgfx_load(). Everything else (moam_set_camera, moam_tick, moam_carry, moam_drop_puff,
 * moam_player_at, moam_npc_at, moam_shutdown) touches only VRAM/OAM/PALRAM/registers and
 * is safe every frame.
 */
#include <tonc.h>
#include <string.h>

#include "map_oam.h"
#include "hand_oam.h"
#include "map_gfx.h"     /* MGFX_HUD_BLDCNT — the puff borrows REG_BLDCNT */
#include "gen3_flags.h"    /* pk_flag_get — an NPC is present iff its flag is CLEAR */
#include "log.h"

/* ---- OBJ tile ids (all >= 512; see the budget above) ----------------------- */
#define TID_NPC0      512
#define TID_NPC_END   928          /* exclusive — the NPC pool may never reach this.
                                    * 416 tiles; MEASURED worst real map needs 384.    */
#define TID_HAND      928          /* PC hand, TWO 32x32 frames: +0 open, +16 closed   */
#define TID_HAND_GRAB (TID_HAND + 16)
#define TID_PLAYER    976
#define PLAYER_TILES   16          /* 32x32 worth; refuses anything larger          */
/* The player block holds TWO frames when they fit: the south-facing standing pose, and
 * the WEST side pose used for the carry flail. A 16x32 character is 8 tiles, so both fit
 * in the 16. A 32x32 one does not, and then the flail falls back to flipping south. */
#define TID_DOT       992          /* +0 = NPC dot, +1 = player dot                 */
#define TID_SWEAT     994          /* +0 = droplet, +1 = flick                      */
#define TID_PUFF      996          /* 4 frames x 4 tiles (16x16 each)               */
#define TID_GHOST    1012          /* 4 tiles: a COLLECTED item ball, greyed out    */
#define TID_GHOST_DOT 1016         /* its zoomed-out dot                            */
#define PUFF_FRAMES     4

/* ---- OBJ palette banks ------------------------------------------------------ */
#define PB_NPC_MAX     13          /* NPCs get banks 0..12 */
#define PB_HAND        13
#define PB_PLAYER      14
#define PB_FX          15

/* Colour indices inside the shared effects bank (PB_FX). */
#define CI_DOT_EDGE     1
#define CI_DOT_NPC      2
#define CI_DOT_PLR      3
#define CI_SW_EDGE      4
#define CI_SW_BODY      5
#define CI_SW_HI        6
#define CI_PF_HI        7
#define CI_PF_MID       8
#define CI_PF_LO        9
#define CI_GH_LO       10          /* the three greys a collected item ball maps to */
#define CI_GH_MID      11
#define CI_GH_HI       12

/* ---- OAM assignment --------------------------------------------------------- */
#define MOAM_MAX_NPC   48          /* measured worst real map = 46 objects */
#define OE_HAND         0          /* in FRONT of the carried character            */
#define OE_PLAYER       1
#define OE_SWEAT       53          /* beside the head, so behind the body is fine   */
#define OE_PUFF0        2          /* 2,3,4 */
#define OE_NPC0         5          /* 5..52 */

/* ---- animation timing (frames @ 59.7 Hz) -------------------------------------
 * Every period and divisor here is a power of two on purpose. Thumb-1 has no long
 * multiply, so GCC turns even a divide by a constant into an __aeabi_idiv CALL — and
 * these run every frame. Powers of two compile to shifts, and masking the frame counter
 * (rather than letting it free-run) also removes any chance of a negative operand years
 * from now. */
#define CARRY_LIFT      3          /* px the carried character is held above the tile */
#define SWEAT_PERIOD   64          /* ~1.07 s loop; a multiple of the 32-frame bob     */
#define SWEAT_APPEAR   12          /* droplet becomes visible here                     */
#define SWEAT_FLICK    44          /* ...runs down over 32 frames, then flicks away    */
#define SWEAT_GONE     52          /* ...and is gone until the loop restarts           */
#define PUFF_LEN       32          /* whole dust puff, ~0.54 s                         */
#define PUFF_STAGGER    4          /* frames between the three rings                   */
#define PUFF_RING_LEN  24          /* one ring's life (4+2*4+24 == PUFF_LEN)           */
#define FLAIL_SHIFT     3          /* side pose flips every 8 frames: ~3.7 Hz of panic */

/* One resolved, uploaded sprite. Shared by every NPC using the same graphics id. */
typedef struct {
  uint16_t key;      /* the RESOLVED graphics id (after VAR substitution) */
  uint16_t tid;
  uint8_t  w, h;
  uint8_t  pb;
  uint8_t  shape, size;   /* raw OAM fields from npc_gfx_oam_shape() */
  uint8_t  hflip;
} MoamSpr;

/* ---- state ------------------------------------------------------------------
 * The big arrays are EWRAM_BSS (hard rule 2 — never on the 32 KB IWRAM stack); the
 * handful of scalars stay in IWRAM .bss where they are cheap to touch every frame.
 * EWRAM total: 48*24 + 48 + 32*10 + 20 + 12 + 14 + 14 = 1,580 B. */
static NpcGfx         EWRAM_BSS s_g;
static RomObjectEvent EWRAM_BSS s_tmpl[MOAM_MAX_NPC];   /* placed NPCs, for moam_npc_at */
static uint8_t        EWRAM_BSS s_slot[MOAM_MAX_NPC];   /* sprite-cache index, 0xFF = dot */
static uint8_t        EWRAM_BSS s_gone[MOAM_MAX_NPC];   /* 1 = a collected item ball      */
static uint16_t       s_ball_gfx;                       /* OBJ_EVENT_GFX_ITEM_BALL, per game */
static MoamSpr        EWRAM_BSS s_spr[32];
static MoamSpr        EWRAM_BSS s_plr;
static uint16_t       EWRAM_BSS s_bank_tag[PB_NPC_MAX];
static uint8_t        EWRAM_BSS s_bank_used[PB_NPC_MAX];

static int      s_cam_px, s_cam_py;
static int      s_ppt = 16;        /* px per metatile at the current zoom */
static int      s_npc_n;
static int      s_spr_n;
static uint16_t s_next_tid = TID_NPC0;
static bool     s_ready;           /* npc_gfx_open() succeeded */
static bool     s_active;          /* moam_init() ran: OAM/OBJ VRAM are ours */
static bool     s_plr_ok;          /* the player's real sprite is in VRAM */
static bool     s_plr_west;        /* the WEST side pose is loaded too (carry flail) */
/* "the sprite is UPLOADED", as distinct from s_plr_ok's "map_oam is live and drawing it".
 * moam_shutdown() hides sprites but does NOT wipe VRAM, so the region map can still draw a
 * player icon from these tiles; only moam_init() invalidates them. */
static bool     s_plr_have_gfx;
static uint16_t s_plr_west_tid;
static bool     s_hand_ok;         /* the PC grab hand is in VRAM */
static bool     s_plr_shown;
static int      s_plr_mx, s_plr_my;
static bool     s_carry;
static int      s_carry_t;
static int      s_puff_t = -1;     /* -1 = idle */
static int      s_puff_mx, s_puff_my;
static bool     s_bld_on;          /* we own REG_BLDCNT right now */

/* A ~32-frame "sine-ish" bob, 0..-2 px. Held at the top of the arc a touch longer than
 * the bottom, which reads as effort rather than a bounce. */
static const int8_t s_bob[16] = { 0, 0, -1, -1, -2, -2, -2, -2, -2, -2, -1, -1, 0, 0, 0, 0 };

/* ---- low-level OAM ---------------------------------------------------------- */

static void hide(int oe) { oam_mem[oe].attr0 = ATTR0_HIDE; }

/* Place one regular 4bpp OBJ. sx/sy are the sprite's TOP-LEFT in screen pixels and may be
 * negative; a sprite entirely off screen is HIDDEN, never merely moved (attr0's y is 8 bits
 * and attr1's x is 9, so "just leave it at y=200" wraps it back into view). */
static void place(int oe, int sx, int sy, int w, int h, uint16_t tid,
                  int pb, int shape, int size, bool hflip, bool blend) {
  if (sx <= -w || sx >= 240 || sy <= -h || sy >= 160) { hide(oe); return; }
  oam_mem[oe].attr0 = (uint16_t)((sy & 0x00FF) | ATTR0_REG | ATTR0_4BPP |
                                 ATTR0_SHAPE(shape) | (blend ? ATTR0_BLEND : 0));
  oam_mem[oe].attr1 = (uint16_t)((sx & 0x01FF) | ATTR1_SIZE(size) |
                                 (hflip ? ATTR1_HFLIP : 0));
  oam_mem[oe].attr2 = (uint16_t)(ATTR2_ID(tid) | ATTR2_PRIO(1) | ATTR2_PALBANK(pb));
}

/* ---- VRAM / PALRAM uploads --------------------------------------------------- */

/* 16 BGR555 colours -> one OBJ palette bank. u16 stores. */
static void upload_pal(int bank, const uint16_t pal[16]) {
  COLOR* d = &pal_obj_mem[bank * 16];
  for (int i = 0; i < 16; i++) d[i] = pal[i];
}

/* Stream a raw 4bpp frame out of the ROM into OBJ tile `tid`.
 *
 * npc_gfx_frame() would do this in one shot but wants the WHOLE frame (up to 4,096 B) in
 * RAM, and there is no EWRAM left for that. The frame is plain, uncompressed, already in
 * OBJ 1D-mapping order, and npc_gfx_frame is itself just a rom_read_at at frame_addr, so
 * it can be chunked. 256 B of word-aligned stack (not multi-KB — hard rule 2 is about
 * buffers that could swallow the 32 KB IWRAM stack, and this is 0.8% of it). */
static bool upload_frame(const NpcGfxInfo* gi, uint16_t tid) {
  uint32_t stage[64];                                   /* 256 B, 4-byte aligned by type */
  uint8_t* dst = (uint8_t*)tile_mem_obj[0] + (uint32_t)tid * 32u;
  uint32_t bytes = gi->frame_bytes, off = 0;
  if (!bytes || (bytes & 31u)) return false;            /* must be whole 4bpp tiles */
  while (off < bytes) {
    uint32_t n = bytes - off;
    if (n > sizeof stage) n = sizeof stage;
    if (!rom_read_at(s_g.rom, gi->frame_addr + off, stage, n)) return false;
    dma3_cpy(dst + off, stage, n);                      /* 32-bit writes into VRAM */
    off += n;
  }
  return true;
}

/* ---- procedural effect tiles -------------------------------------------------
 * Everything below is generated in code. NOTHING here is taken from the ROM: the sweat
 * droplet and the dust puff are ours, so they carry no Nintendo art. */

static const char* const ART_DOT[8] = {
  "........",
  "..1111..",
  ".122221.",
  ".122221.",
  ".122221.",
  ".122221.",
  "..1111..",
  "........",
};

/* A teardrop: narrow tip, round belly, white highlight up the left side. */
static const char* const ART_SWEAT[8] = {
  "........",
  "...4....",
  "..454...",
  "..4654..",
  ".465554.",
  ".455554.",
  "..4444..",
  "........",
};

/* The same drop mid-flick: a short streak leaving up and to the right. */
static const char* const ART_FLICK[8] = {
  "........",
  "......5.",
  ".....46.",
  "....45..",
  "...45...",
  "..4.....",
  "........",
  "........",
};

static void art_px(const char* const* rows, int w, int h, uint8_t* px) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      char c = rows[y][x];
      px[y * w + x] = (c >= '0' && c <= '9') ? (uint8_t)(c - '0') : 0;
    }
}

/* Pack a w*h paletted image into consecutive 4bpp tiles, in the order 1D OBJ mapping
 * wants (tiles row-major), writing OBJ VRAM as 32-bit words only. */
static void emit_tiles(uint16_t tid, const uint8_t* px, int w, int h) {
  volatile uint32_t* dst =
      (volatile uint32_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)tid * 32u);
  for (int ty = 0; ty < (h >> 3); ty++)
    for (int tx = 0; tx < (w >> 3); tx++)
      for (int y = 0; y < 8; y++) {
        const uint8_t* row = px + (ty * 8 + y) * w + tx * 8;
        uint32_t bits = 0;   /* not `word` — tonc typedefs that */
        for (int x = 0; x < 8; x++) bits |= (uint32_t)(row[x] & 15u) << (x * 4);
        *dst++ = bits;
      }
}

static void gen_dot(uint16_t tid, uint8_t fill) {
  uint8_t px[64];
  art_px(ART_DOT, 8, 8, px);
  for (int i = 0; i < 64; i++) if (px[i] == 2) px[i] = fill;
  emit_tiles(tid, px, 8, 8);
}

/* Four concentric-ring frames of 16x16. Radii are in HALF pixels so the rings can grow in
 * sub-pixel steps and still close cleanly inside the 16 px box (an outer radius past 15
 * half-px would be clipped flat at the four axes and read as a broken ring, not a puff). */
static void gen_puff(void) {
  /* Frame 0's inner radius is 0 on purpose: the puff opens on a solid little burst at the
   * point of impact and only then becomes a ring, which reads as dust being kicked up
   * rather than as a bubble appearing from nowhere. */
  static const uint8_t R_IN[PUFF_FRAMES]  = { 0, 5,  9, 12 };
  static const uint8_t R_OUT[PUFF_FRAMES] = { 5, 9, 13, 15 };
  uint8_t px[256];
  for (int f = 0; f < PUFF_FRAMES; f++) {
    int i2 = R_IN[f] * R_IN[f];
    int o2 = R_OUT[f] * R_OUT[f];
    int m  = (R_IN[f] + R_OUT[f]) / 2, m2 = m * m;
    uint8_t hi = (f < 2) ? CI_PF_HI  : CI_PF_MID;
    uint8_t lo = (f < 2) ? CI_PF_MID : CI_PF_LO;
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++) {
        int dx = 2 * x + 1 - 16, dy = 2 * y + 1 - 16;
        int d2 = dx * dx + dy * dy;
        uint8_t c = 0;
        if (d2 > i2 && d2 <= o2) c = (d2 <= m2) ? hi : lo;
        px[y * 16 + x] = c;
      }
    emit_tiles((uint16_t)(TID_PUFF + f * 4), px, 16, 16);
  }
}

static void gen_effects(void) {
  uint8_t px[64];
  uint16_t pal[16];

  gen_dot(TID_DOT,     CI_DOT_NPC);
  gen_dot(TID_DOT + 1, CI_DOT_PLR);
  art_px(ART_SWEAT, 8, 8, px); emit_tiles(TID_SWEAT,     px, 8, 8);
  art_px(ART_FLICK, 8, 8, px); emit_tiles(TID_SWEAT + 1, px, 8, 8);
  gen_puff();

  memset(pal, 0, sizeof pal);
  pal[CI_DOT_EDGE] = RGB15( 2,  3,  8);    /* dark navy outline, reads on any tileset */
  pal[CI_DOT_NPC]  = RGB15(31, 21,  4);    /* warm gold                              */
  pal[CI_DOT_PLR]  = RGB15(14, 31, 31);    /* bright cyan — the player stands out     */
  pal[CI_SW_EDGE]  = RGB15( 6, 14, 28);
  pal[CI_SW_BODY]  = RGB15(16, 26, 31);
  pal[CI_SW_HI]    = RGB15(31, 31, 31);
  pal[CI_PF_HI]    = RGB15(31, 31, 31);
  pal[CI_PF_MID]   = RGB15(26, 26, 28);
  pal[CI_PF_LO]    = RGB15(19, 19, 22);
  /* A COLLECTED item ball is drawn from the ROM's own ball sprite with every colour
   * collapsed onto this three-step grey ramp — desaturated but still unmistakably a
   * Poke Ball, which a plain dot would not be. Kept slightly blue-cold so it reads as
   * "spent" next to the warm gold NPC dot. */
  pal[CI_GH_LO]    = RGB15( 7,  8, 10);
  pal[CI_GH_MID]   = RGB15(14, 15, 17);
  pal[CI_GH_HI]    = RGB15(22, 23, 25);
  upload_pal(PB_FX, pal);
  gen_dot(TID_GHOST_DOT, CI_GH_MID);
}

/* Build the greyed copy of the item-ball frame at TID_GHOST. Reads the SAME ROM tiles the
 * live ball uses and rewrites every 4bpp nibble to one of the three PB_FX greys, chosen by
 * the luma of the colour that index holds in the ball's own palette. Costs 4 OBJ tiles and
 * no palette bank — stealing one of the 13 NPC banks would have recoloured a real NPC on
 * crowded maps, and pal_slot is a direct index so the collision would not be detectable. */
static bool s_ghost_ok;

static bool gen_ghost(const NpcGfxInfo* gi) {
  uint16_t pal[16];
  uint8_t  map[16];
  if (!gi->ok || gi->w != 16 || gi->h != 16) return false;
  if (gi->frame_bytes != 128) return false;                 /* 4 tiles of 4bpp */
  if (!npc_gfx_palette(&s_g, gi, pal)) return false;

  for (int i = 0; i < 16; i++) {
    if (i == 0) { map[i] = 0; continue; }                   /* index 0 stays transparent */
    int r = pal[i] & 31, g = (pal[i] >> 5) & 31, b = (pal[i] >> 10) & 31;
    int y = (r * 77 + g * 151 + b * 28) >> 8;               /* 0..31 */
    map[i] = (y < 11) ? CI_GH_LO : (y < 21) ? CI_GH_MID : CI_GH_HI;
  }

  uint8_t src[128];
  if (!rom_read_at(s_g.rom, gi->frame_addr, src, sizeof src)) return false;

  /* Remap STRAIGHT INTO VRAM with volatile word stores, exactly like emit_tiles.
   *
   * The obvious version — build the tiles in a local buffer, then dma3_cpy it out — is
   * silently WRONG. tonc's dma3_cpy is INLINE and does nothing with the source but store
   * its ADDRESS into a volatile register, so as far as GCC's alias analysis is concerned
   * the local is written and never read: it is free to delete the whole remap loop, and at
   * -O2 it does. The symptom was a ball drawn from whatever junk the previous screen had
   * left at these tiles, and it came and went with unrelated edits. (upload_frame gets away
   * with the same shape only because rom_read_at is a real call that fills its buffer.)
   *
   * Word stores, never bytes: an 8-bit store into VRAM lands in both halves of the
   * halfword — the same rule that governs every other VRAM write in this file. */
  volatile uint32_t* dst =
      (volatile uint32_t*)((uint8_t*)tile_mem_obj[0] + (uint32_t)TID_GHOST * 32u);
  for (int w = 0; w < 32; w++) {
    const uint8_t* b = src + w * 4;
    uint32_t word = 0;
    for (int k = 0; k < 4; k++)
      word |= (uint32_t)(map[b[k] & 15u] | (map[(b[k] >> 4) & 15u] << 4)) << (k * 8);
    dst[w] = word;
  }
  return true;
}

/* ---- palette bank allocation -------------------------------------------------
 * map_oam.h: palettes are keyed on the game's own paletteSlot, which measurement showed
 * is a safe direct index. Honoured — but not trusted blindly: if two different paletteTags
 * ever claim one slot (or a slot lands on the player/effects banks), the loser is moved to
 * a free bank instead of silently recolouring another NPC. */
static bool load_pal(int b, const NpcGfxInfo* gi) {
  uint16_t pal[16];
  if (!npc_gfx_palette(&s_g, gi, pal)) return false;
  upload_pal(b, pal);
  s_bank_tag[b] = gi->pal_tag;
  s_bank_used[b] = 1;
  return true;
}

static int pal_claim(const NpcGfxInfo* gi) {
  int slot = gi->pal_slot;
  if (slot >= 0 && slot < PB_NPC_MAX) {
    if (s_bank_used[slot] && s_bank_tag[slot] == gi->pal_tag) return slot;
    if (!s_bank_used[slot]) return load_pal(slot, gi) ? slot : -1;
  }
  for (int b = 0; b < PB_NPC_MAX; b++)
    if (s_bank_used[b] && s_bank_tag[b] == gi->pal_tag) return b;
  for (int b = 0; b < PB_NPC_MAX; b++)
    if (!s_bank_used[b]) return load_pal(b, gi) ? b : -1;
  return -1;
}

/* ---- sprite cache ------------------------------------------------------------
 * Tiles are uploaded ONCE PER GRAPHICS ID. Any failure (unknown id, a size that genuinely
 * needs the game's subsprite tables, a full tile pool or a full palette table) returns -1
 * and the caller falls back to a dot — never a hole, never a wrong sprite. */
static int spr_get(uint16_t key, const NpcGfxInfo* gi) {
  for (int i = 0; i < s_spr_n; i++) if (s_spr[i].key == key) return i;
  if (!gi->ok || s_spr_n >= (int)(sizeof s_spr / sizeof s_spr[0])) return -1;

  uint8_t sh, sz;
  if (!npc_gfx_oam_shape(gi->w, gi->h, &sh, &sz)) return -1;   /* needs subsprites */
  int tiles = (gi->w >> 3) * (gi->h >> 3);
  if ((int)s_next_tid + tiles > TID_NPC_END) return -1;        /* pool exhausted */

  int pb = pal_claim(gi);
  if (pb < 0) return -1;
  if (!upload_frame(gi, s_next_tid)) return -1;

  MoamSpr* s = &s_spr[s_spr_n];
  s->key   = key;
  s->tid   = s_next_tid;
  s->w     = gi->w;
  s->h     = gi->h;
  s->pb    = (uint8_t)pb;
  s->shape = sh;
  s->size  = sz;
  s->hflip = gi->hflip ? 1 : 0;
  s_next_tid = (uint16_t)(s_next_tid + tiles);
  return s_spr_n++;
}

/* ---- layout ------------------------------------------------------------------ */

/* Screen position of metatile (mx,my)'s top-left corner at the current camera/zoom. */
static inline int tile_left(int mx) { return mx * s_ppt - s_cam_px; }
static inline int tile_top(int my)  { return my * s_ppt - s_cam_py; }

static void place_dot(int oe, int left, int top, uint16_t tid) {
  place(oe, left + (s_ppt - 8) / 2, top + (s_ppt - 8) / 2, 8, 8, tid, PB_FX, 0, 0,
        false, false);
}

/* Characters stand ON their tile: horizontally centred, bottom edge flush with the tile's
 * bottom, so a 16x32 NPC correctly sticks up into the tile above. */
static void place_char(int oe, const MoamSpr* s, int left, int top, int dy) {
  place(oe, left + s_ppt / 2 - s->w / 2, top + s_ppt - s->h + dy,
        s->w, s->h, s->tid, s->pb, s->shape, s->size, s->hflip != 0, false);
}

static void place_npc(int i) {
  int oe = OE_NPC0 + i;
  int left = tile_left(s_tmpl[i].x), top = tile_top(s_tmpl[i].y);
  uint8_t ci = s_slot[i];
  /* Below 1:1 a 16 px sprite covers two whole metatiles and a busy town turns to mush, so
   * every NPC becomes one small dot (map_oam.h). Same path serves as the 1:1 fallback for
   * anything we could not resolve or fit. */
  if (s_gone[i]) {
    /* A collected ball keeps a ball's footprint, so place it like a 16x16 character rather
     * than like a dot — otherwise it jumps half a tile when the flag is cleared. */
    if (s_ppt < 16 || !s_ghost_ok) place_dot(oe, left, top, TID_GHOST_DOT);
    else place(oe, left + (s_ppt - 16) / 2, top + s_ppt - 16, 16, 16,
               TID_GHOST, PB_FX, 0 /*square*/, 1 /*16x16*/, false, false);
    return;
  }
  if (s_ppt < 16 || ci == 0xFF) place_dot(oe, left, top, TID_DOT);
  else                          place_char(oe, &s_spr[ci], left, top, 0);
}

/* The sweat droplet: appears by the head, slides down a few pixels, flicks away, repeat.
 * px/py are the carried character's sprite top-left. */
static void place_sweat(int px, int py, int w, int h) {
  if (!s_carry || s_ppt < 16) { hide(OE_SWEAT); return; }
  int t = s_carry_t & (SWEAT_PERIOD - 1), fr, dx, dy;
  if (t < SWEAT_APPEAR || t >= SWEAT_GONE) { hide(OE_SWEAT); return; }
  if (t < SWEAT_FLICK) {                      /* welling up and running down */
    fr = 0; dx = 0;
    dy = ((t - SWEAT_APPEAR) * 5) >> 5;       /* 0..4 px over 32 frames */
  } else {                                    /* flicked off, up and to the right */
    int u = t - SWEAT_FLICK;                  /* 0..7 */
    fr = 1; dx = u; dy = 4 - (u >> 1);
  }
  /* Anchor to the HEAD, not to the sprite's top edge. Gen-3 overworld characters are
   * 16x32, so py is a full body-height above the face and `py + 1` floated the droplet
   * clear above the character, reading as an unrelated object rather than as their sweat.
   * h/4 is the forehead for a 16x32 sprite and still sensible for a 16x16 one.
   * w-4 straddles the right edge so the drop sits AT the side of the head. */
  place(OE_SWEAT, px + w - 4 + dx, py + (h >> 2) + dy, 8, 8,
        (uint16_t)(TID_SWEAT + fr), PB_FX, 0, 0, false, false);
}

/* The PC grab hand, closed around the character it is carrying. Drawn at OAM index 0 so
 * it composites IN FRONT of everything else on the map, and offset so the fist sits over
 * the character's shoulders rather than floating above their head. */
static void place_hand(void) {
  if (!s_hand_ok || s_ppt < 16) { hide(OE_HAND); return; }

  int hx, hy;
  if (s_carry && s_plr_ok && s_plr_shown) {
    /* Gripping the character: anchor to THEM so the fist tracks the bob.
     * The FIST sits left-of-centre and high inside the 32x32 cell, so centring the CELL
     * would leave the grip up and to their left. Both offsets are MEASURED against real
     * frames, not derived — do not "simplify" them back to a centred cell. */
    int cx = 120 - s_plr.w / 2;
    int cy =  80 - s_ppt / 2 + s_ppt - s_plr.h
             + (-CARRY_LIFT + s_bob[(s_carry_t >> 1) & 15]);
    hx = cx + s_plr.w / 2 - 10;
    hy = cy + 2;
  } else {
    /* Empty hand = the CURSOR, pointing at the tile the camera is centred on — which is
     * exactly the tile START acts on. Without it "what am I about to enter?" is only
     * implied by the middle of the screen.
     *
     * The art's FINGERTIP is at roughly (8,30) inside the 32x32 cell, so the cell is
     * placed UP and slightly LEFT of the tile: that puts the fingertip on the tile while
     * the palm floats above it. Centring the cell instead (the obvious thing) buries the
     * tile — and the character standing on it — under a big white hand. */
    hx = 120 - 8;
    hy = (80 - s_ppt / 2) - 20;
  }
  place(OE_HAND, hx, hy, 32, 32,
        (uint16_t)(s_carry ? TID_HAND_GRAB : TID_HAND),
        PB_HAND, ATTR0_SQUARE >> 14, 2 /* 32x32 */, false, false);
}

static void place_player(void) {
  place_hand();      /* the cursor hand is independent of the player sprite */
  if (!s_active || !s_plr_shown) { hide(OE_PLAYER); hide(OE_SWEAT); place_hand(); return; }

  int left, top, dy = 0;
  if (s_carry) {
    /* While carried the character rides the cursor. mgfx_camera() and mgfx_zoom_show()
     * both put the cursor's centre at screen (120,80) unconditionally — there is no edge
     * clamping — so the cursor tile's top-left is exactly this, at any zoom. */
    left = 120 - s_ppt / 2;
    top  =  80 - s_ppt / 2;
    if (s_ppt >= 16) dy = -CARRY_LIFT + s_bob[(s_carry_t >> 1) & 15];
  } else {
    left = tile_left(s_plr_mx);
    top  = tile_top(s_plr_my);
  }

  if (s_ppt < 16 || !s_plr_ok) {
    place_dot(OE_PLAYER, left, top, TID_DOT + 1);
    hide(OE_SWEAT);
    return;
  }
  if (s_carry && s_plr_west) {
    MoamSpr sp = s_plr;                       /* same geometry, side-facing frame */
    sp.tid   = s_plr_west_tid;
    sp.hflip = (uint8_t)((s_carry_t >> FLAIL_SHIFT) & 1);   /* west <-> east */
    place_char(OE_PLAYER, &sp, left, top, dy);
  } else {
    place_char(OE_PLAYER, &s_plr, left, top, dy);
  }
  {
    int cx = left + s_ppt / 2 - s_plr.w / 2;
    int cy = top  + s_ppt - s_plr.h + dy;
    place_sweat(cx, cy, s_plr.w, s_plr.h);
  }
}

/* Three pale rings expanding out of the landing tile and fading to nothing.
 *
 * The fade is a real alpha ramp: only the three ring OBJs carry ATTR0_BLEND, so naming OBJ
 * as the blend source touches nothing else on screen. REG_BLDCNT is claimed for the 32
 * frames the puff lasts and put back to 0 afterwards (map_gfx zeroes it on enter/exit, so
 * 0 is the state the rest of the map view expects). */
static void place_puff(void) {
  if (s_puff_t < 0 || s_ppt < 16) {
    for (int i = 0; i < 3; i++) hide(OE_PUFF0 + i);
    if (s_bld_on) { REG_BLDCNT = MGFX_HUD_BLDCNT; REG_BLDY = MGFX_HUD_BLDY;
                  REG_BLDALPHA = 0; s_bld_on = false; }
    return;
  }
  int t = s_puff_t;                        /* 0..PUFF_LEN-1 */
  int eva = 16 - (t >> 1);                 /* 16 -> 1 across the 32 frames */
  REG_BLDALPHA = (uint16_t)(eva | ((16 - eva) << 8));
  if (!s_bld_on) {
    REG_BLDCNT = (uint16_t)(BLD_OBJ | BLD_STD |
                            BLD_BOT(BLD_BG0 | BLD_BG1 | BLD_BG2 | BLD_BG3 | BLD_BACKDROP));
    s_bld_on = true;
  }

  int cx = tile_left(s_puff_mx) + s_ppt / 2;
  int cy = tile_top(s_puff_my)  + s_ppt - 4;     /* at the feet, not the middle */
  /* Which ring frame each of a ring's 24 frames of life shows — a table rather than u/6,
   * which would be a libcall in Thumb and is harder to read than the shape it encodes. */
  static const uint8_t RING_F[PUFF_RING_LEN] = {
    0,0,0,0,0,0, 1,1,1,1,1,1, 2,2,2,2,2,2, 3,3,3,3,3,3
  };
  for (int i = 0; i < 3; i++) {
    int u = t - i * PUFF_STAGGER;
    if (u < 0 || u >= PUFF_RING_LEN) { hide(OE_PUFF0 + i); continue; }
    place(OE_PUFF0 + i, cx - 8, cy - 8, 16, 16,
          (uint16_t)(TID_PUFF + RING_F[u] * 4), PB_FX, 0, 1, false, true);
  }
}

/* mgfx_show(), mgfx_z0_show(), mgfx_zoom_show() and the "Loading" frame all ASSIGN
 * REG_DISPCNT wholesale, which drops the OBJ bits. Re-assert them from the per-frame
 * entry points rather than hoping the caller ordered things right. */
static void obj_on(void) { REG_DISPCNT |= DCNT_OBJ | DCNT_OBJ_1D; }

/* ---- API --------------------------------------------------------------------- */

bool moam_init(const RomCtx* rom) {
  oam_init(oam_mem, 128);                 /* all 128 entries hidden */
  memset(s_bank_tag,  0, sizeof s_bank_tag);
  memset(s_bank_used, 0, sizeof s_bank_used);
  memset(&s_plr, 0, sizeof s_plr);
  s_npc_n = 0;
  s_spr_n = 0;
  s_next_tid = TID_NPC0;
  s_cam_px = s_cam_py = 0;
  s_ppt = 16;
  s_plr_ok = s_plr_shown = s_carry = false;
  s_plr_west = false;
  s_plr_have_gfx = false;   /* VRAM is about to be reused */
  s_plr_mx = s_plr_my = 0;
  s_carry_t = 0;
  s_puff_t = -1;
  s_puff_mx = s_puff_my = 0;
  if (s_bld_on) { REG_BLDCNT = MGFX_HUD_BLDCNT; REG_BLDY = MGFX_HUD_BLDY;
                  REG_BLDALPHA = 0; s_bld_on = false; }

  gen_effects();                          /* pure VRAM/PALRAM — no SD access */

  /* The PC grab hand — the SAME art the box screen uses to pick a Pokemon up, so grabbing
   * your character reads as the gesture the tool already taught you. It ships in the ROM
   * image (hand_oam.c, generated from Guy's own reconstruction), so this is a plain
   * memcpy: no SD access and nothing to fail at runtime. */
  memcpy32((void*)(0x06010000u + (uint32_t)TID_HAND * 32u),
           hand_oam_cursor_tiles, sizeof(hand_oam_cursor_tiles) / 4);
  memcpy32((void*)(0x06010000u + (uint32_t)TID_HAND_GRAB * 32u),
           hand_oam_grab_tiles, sizeof(hand_oam_grab_tiles) / 4);
  upload_pal(PB_HAND, hand_oam_pal);
  s_hand_ok = true;

  s_active = true;

  s_ready = rom && npc_gfx_open(&s_g, rom);   /* ~200 x 512 B of SD reads, once */
  obj_on();
  if (!s_ready) log_line("map_oam: npc_gfx_open failed - dots only");
  return s_ready;
}

void moam_shutdown(void) {
  oam_init(oam_mem, 128);
  REG_DISPCNT &= (uint16_t)~(DCNT_OBJ | DCNT_OBJ_1D);
  /* Bank 15 is OURS (PB_FX) but it is also box_oam's PB_CITEM, and boxoam_enter() only
   * re-uploads banks 0..14 — so leaving our effect colours behind would repaint the box
   * screen's carried-item icon in sweat-and-dust tones. Zero it; the box rebuilds it. */
  memset32(&pal_obj_mem[15 * 16], 0, 8);        /* 16 entries = 32 B = 8 words */
  if (s_bld_on) { REG_BLDCNT = 0; REG_BLDALPHA = 0; s_bld_on = false; }
  s_active = false;
  s_ready  = false;
  s_npc_n  = 0;
  s_carry  = false;
  s_puff_t = -1;
  s_plr_ok = s_plr_shown = false;
}

/* SaveBlock1's vars[] base. Sits immediately after flags[], whose per-game bases
 * gen3_flags.c already pins: E 0x1270+300, RS 0x1220+288, FRLG 0x0EE0+288. */
static uint32_t vars_off(PkGame g) {
  return (g == PK_FRLG) ? 0x1000u : (g == PK_EMERALD) ? 0x139Cu : 0x1340u;
}

static uint8_t var_gfx_value(const uint8_t* sb1, PkGame g, int vi) {
  if (!sb1 || vi < 0) return 0;
  /* VarGet(VAR_OBJ_GFX_ID_0 + vi); VAR_OBJ_GFX_ID_0 is 0x4010 in all three games and the
   * var array is indexed from 0x4000. Only the low byte is a graphics id. */
  uint32_t off = vars_off(g) + (uint32_t)((NPC_GFX_VAR_ID_0 - 0x4000) + vi) * 2u;
  return sb1[off];
}

int moam_load_map(const RomCtx* rom, uint32_t events_addr,
                  const uint8_t* sb1, PkGame game) {
  for (int i = 0; i < MOAM_MAX_NPC; i++) hide(OE_NPC0 + i);
  s_npc_n = 0;
  s_spr_n = 0;
  s_next_tid = TID_NPC0;
  memset(s_bank_tag,  0, sizeof s_bank_tag);
  memset(s_bank_used, 0, sizeof s_bank_used);   /* banks 14/15 are never in this table */

  /* OBJ_EVENT_GFX_ITEM_BALL: 59 in RSE, 92 in FRLG (MEASURED against all three decomps).
   * Set per LOAD, not per init, so switching game inside one session cannot leave the
   * old id behind and mis-classify every ball on the new map. */
  s_ball_gfx = (game == PK_FRLG) ? 92u : 59u;
  s_ghost_ok = false;
  memset(s_gone, 0, sizeof s_gone);

  if (!s_active || !rom || !events_addr) return 0;

  RomMapEvents ev;
  if (!rom_map_events(rom, events_addr, &ev)) return 0;

  int skipped_flag = 0, dots = 0;
  for (int i = 0; i < (int)ev.object_count && s_npc_n < MOAM_MAX_NPC; i++) {
    RomObjectEvent o;
    if (!rom_object_event(rom, &ev, i, &o)) continue;

    /* An NPC is present iff its flag is CLEAR; flag_id 0 means always present. This is
     * what makes the overlay show the map as THIS save would actually see it.
     *
     * ITEM BALLS ARE THE EXCEPTION. A collected ball is "absent" for the same reason, but
     * Guy wants to see WHERE he has already been and to be able to put one back, so it is
     * kept in the list and drawn greyed instead of vanishing. Every other flagged-off
     * object really is gone and stays hidden — an NPC who has moved on is not a ghost. */
    bool gone = (o.flag_id && sb1 && pk_flag_get(sb1, game, o.flag_id));
    bool ball = (o.graphics_id == s_ball_gfx);
    if (gone && !ball) { skipped_flag++; continue; }

    int idx = s_npc_n;
    s_tmpl[idx] = o;
    s_slot[idx] = 0xFF;
    s_gone[idx] = (gone && ball) ? 1 : 0;
    if (s_gone[idx]) {
      /* Build the greyed tiles once, from the first collected ball we meet. */
      if (!s_ghost_ok && s_ready) {
        NpcGfxInfo gi;
        if (npc_gfx_info(&s_g, o.graphics_id, &gi)) s_ghost_ok = gen_ghost(&gi);
      }
      s_npc_n++;
      continue;                                   /* no sprite-cache slot, no palette bank */
    }

    int vi = npc_gfx_var_index(o.graphics_id);
    /* A VAR id with no save to resolve it against would otherwise read as var value 0,
     * i.e. a Brendan clone for every one of them. A dot is the honest answer. */
    if (s_ready && !(vi >= 0 && !sb1)) {
      uint8_t vv = var_gfx_value(sb1, game, vi);
      uint16_t key = (vi >= 0) ? (uint16_t)vv : o.graphics_id;
      NpcGfxInfo gi;
      npc_gfx_info_var(&s_g, o.graphics_id, vv, NPC_DIR_SOUTH, &gi);   /* always fills gi */
      int ci = spr_get(key, &gi);
      if (ci >= 0) s_slot[idx] = (uint8_t)ci;
      else dots++;
    } else {
      dots++;
    }
    s_npc_n++;
  }

  int ghosts = 0;
  for (int i = 0; i < s_npc_n; i++) ghosts += s_gone[i];
  log_line("map_oam: %d npc (%d flagged off, %d dots, %d spent balls%s) %d spr %d tiles",
           s_npc_n, skipped_flag, dots, ghosts, (ghosts && !s_ghost_ok) ? " NO GFX" : "",
           s_spr_n, (int)(s_next_tid - TID_NPC0));

  /* Lay them out against the camera we already have, but deliberately do NOT switch OBJ
   * on here: a map load runs behind the "Loading" frame, and the caller re-aims the camera
   * immediately afterwards. Sprites appear at the first moam_set_camera/moam_tick, by
   * which time their positions are right. */
  for (int i = 0; i < s_npc_n; i++) place_npc(i);
  place_player();
  place_puff();
  return s_npc_n;
}

void moam_set_camera(int cam_px, int cam_py, int px_per_tile) {
  if (!s_active) return;
  s_cam_px = cam_px;
  s_cam_py = cam_py;
  s_ppt = (px_per_tile > 0) ? px_per_tile : 16;
  obj_on();
  for (int i = 0; i < s_npc_n; i++) place_npc(i);
  place_player();
  place_puff();
}

void moam_drop_gfx_range(uint16_t lo, uint16_t hi) {
  int w = 0;
  for (int i = 0; i < s_npc_n; i++) {
    if (s_tmpl[i].graphics_id >= lo && s_tmpl[i].graphics_id <= hi) continue;
    if (w != i) { s_tmpl[w] = s_tmpl[i]; s_slot[w] = s_slot[i]; s_gone[w] = s_gone[i]; }
    w++;
  }
  for (int i = w; i < s_npc_n; i++) hide(OE_NPC0 + i);
  s_npc_n = w;
  /* Deliberately does NOT free the dropped sprites' tiles or palette banks. The pool is
   * allocated bump-style and a map only ever loads once, so reclaiming would mean rewriting
   * the allocator for no gain — the placeholders are at most 14 of 48 slots. */
  for (int i = 0; i < s_npc_n; i++) place_npc(i);
}

bool moam_add_sprite(uint16_t gfx_id, int mx, int my) {
  if (!s_active || s_npc_n >= MOAM_MAX_NPC) return false;
  int idx = s_npc_n;
  memset(&s_tmpl[idx], 0, sizeof s_tmpl[idx]);
  s_tmpl[idx].graphics_id = (uint8_t)gfx_id;
  s_tmpl[idx].x = (int16_t)mx;
  s_tmpl[idx].y = (int16_t)my;
  s_slot[idx] = 0xFF;
  s_gone[idx] = 0;

  if (s_ready) {
    NpcGfxInfo gi;
    npc_gfx_info_var(&s_g, gfx_id, 0, NPC_DIR_SOUTH, &gi);
    int ci = spr_get(gfx_id, &gi);
    if (ci >= 0) s_slot[idx] = (uint8_t)ci;
  }
  s_npc_n++;
  place_npc(idx);
  return true;
}

const RomObjectEvent* moam_npc_at(int mx, int my) {
  for (int i = 0; i < s_npc_n; i++)
    if (s_tmpl[i].x == mx && s_tmpl[i].y == my) return &s_tmpl[i];
  return 0;
}

bool moam_set_player(const RomCtx* rom, uint16_t gfx_id, int mx, int my) {
  if (!s_active) return false;
  s_plr_mx = mx;
  s_plr_my = my;
  s_plr_shown = true;
  s_plr_ok = false;

  if (!rom || !s_ready) { place_player(); return false; }

  NpcGfxInfo gi;
  uint8_t sh, sz;
  uint16_t pal[16];
  if (!npc_gfx_info_dir(&s_g, gfx_id, NPC_DIR_SOUTH, &gi) || !gi.ok ||
      !npc_gfx_oam_shape(gi.w, gi.h, &sh, &sz) ||
      (gi.w >> 3) * (gi.h >> 3) > PLAYER_TILES ||
      !npc_gfx_palette(&s_g, &gi, pal) ||
      !upload_frame(&gi, TID_PLAYER)) {
    log_line("map_oam: player gfx %u unusable - dot", (unsigned)gfx_id);
    place_player();
    return false;                        /* the player still shows, as a bright dot */
  }
  upload_pal(PB_PLAYER, pal);

  /* The carry flail needs a pose whose ARMS visibly move. The ROM has no "panicking"
   * frame, but the WEST side pose differs from the south one in exactly the right way:
   * alternating it with its own h-flip (which is how the game itself draws EAST) swings
   * the character left and right, arms and all. Real ROM art, one extra frame, one OAM
   * bit per frame to animate. If the sprite is too big for two frames to fit the player
   * block, we simply skip it and the carry falls back to the standing pose. */
  {
    int tiles = (gi.w >> 3) * (gi.h >> 3);
    NpcGfxInfo gw;
    s_plr_west = false;
    if (tiles * 2 <= PLAYER_TILES &&
        npc_gfx_info_dir(&s_g, gfx_id, NPC_DIR_WEST, &gw) && gw.ok &&
        gw.w == gi.w && gw.h == gi.h &&
        upload_frame(&gw, TID_PLAYER + tiles)) {
      s_plr_west_tid = (uint16_t)(TID_PLAYER + tiles);
      s_plr_west = true;
    }
  }
  s_plr.key   = gfx_id;
  s_plr.tid   = TID_PLAYER;
  s_plr.w     = gi.w;
  s_plr.h     = gi.h;
  s_plr.pb    = PB_PLAYER;
  s_plr.shape = sh;
  s_plr.size  = sz;
  s_plr.hflip = gi.hflip ? 1 : 0;
  s_plr_ok = true;
  s_plr_have_gfx = true;   /* survives moam_shutdown: the tiles stay in VRAM */
  place_player();
  return true;
}

bool moam_player_sprite(uint16_t* tid, uint8_t* pal_bank, uint8_t* h) {
  if (!s_plr_have_gfx) return false;
  if (tid)      *tid = s_plr.tid;
  if (pal_bank) *pal_bank = s_plr.pb;
  if (h)        *h   = s_plr.h;
  return true;
}

void moam_player_visible(bool on) {
  if (!s_active) return;
  s_plr_shown = on;
  place_player();
}

void moam_player_at(int mx, int my) {
  s_plr_mx = mx;
  s_plr_my = my;
  s_plr_shown = true;
  place_player();
}

void moam_carry(bool carrying, int mx, int my) {
  if (!s_active) return;
  if (carrying != s_carry) s_carry_t = 0;    /* restart the bob/sweat cycle cleanly */
  s_carry = carrying;
  s_plr_mx = mx;
  s_plr_my = my;
  s_plr_shown = true;
  place_player();
}

void moam_drop_puff(int mx, int my) {
  if (!s_active) return;
  s_puff_mx = mx;
  s_puff_my = my;
  s_puff_t = 0;
  place_puff();
}

void moam_tick(void) {
  if (!s_active) return;
  if (s_carry) s_carry_t = (s_carry_t + 1) & (SWEAT_PERIOD - 1);   /* never goes negative */
  if (s_puff_t >= 0 && ++s_puff_t >= PUFF_LEN) s_puff_t = -1;
  obj_on();
  place_player();
  place_puff();
}
