/*
 * Mode-0 map graphics. See map_gfx.h for the VRAM map and the two rules that shape this
 * file (BG0 tile ids <= 239; the LZ77 staging buffers must not co-exist with the caches).
 */
#include <tonc.h>
#include <string.h>

#include "map_gfx.h"
#include "log.h"
#include "ui.h"

/* ---- VRAM placement (bytes, not "blocks" — they alias) --------------------- */
#define VRAM_MAP_CHAR  0x06000000u      /* CBB0+CBB1: 1024 tiles, ids 0..1023  */
#define CBB_HUD        3                /* HUD font charblock                   */
#define SBB_BG1_TOP    29               /* metatile TOP layer                   */
#define SBB_BG3_BOTTOM 30               /* metatile BOTTOM layer                */
#define SBB_BG0_HUD    31               /* HUD text                            */
#define MGFX_HUD_MAX_TILE 127           /* the aliasing cap — see map_gfx.h     */
#define VRAM_MIP_CHAR  0x06008000u      /* CBB2: 256 x 8x8 8bpp mip tiles       */
#define VRAM_AFF_MAP   0x0600D000u      /* SBB26+27: 64x64 one-byte entries     */
#define MIP_PX         8                /* one metatile becomes one 8x8 tile    */

/* ---- LZ77 staging sizes, from measurement --------------------------------
 * Max COMPRESSED tileset: 8,909 B (Emerald) / 10,452 B (FireRed)  -> 12,288 is ample.
 * Max DECOMPRESSED:      16,384 B (Emerald) / 20,480 B (FireRed, 640 tiles). */
#define STAGE_BYTES   12288u
#define DECOMP_BYTES  20480u

/* ---- metatile tables live in OBJ VRAM -------------------------------------
 * They are read-only byte-addressed lookup tables and are structurally <= 16,384 B in
 * total (1024 metatiles x 16 B; RSE splits 8192+8192, FRLG 10240+6144 — both exactly
 * 16,384). Keeping them out of the 35,712 B EWRAM arena is what makes stitching fit:
 * phase-2 worst case drops 29,280 -> 13,824.
 *
 * OBJ VRAM 0x06010000..0x06013FFF is free for us: the map view is Mode 0 / Mode 1 and
 * never sets DCNT_OBJ, and every PokeDNA sprite lives at tile ids >= 512, i.e. from
 * 0x06014000 up (box_oam.h). In Mode 3 the framebuffer covers 0x06000000..0x06012BFF and
 * ui_init() repaints all of it on exit, so this region is self-healing. */
#define VRAM_MT_BASE   0x06010000u
#define VRAM_MT_LIMIT  0x06014000u      /* NEVER write at or past this: sprite tiles */
#define MT_SCRATCH     10240u           /* largest single table (FRLG primary)      */

static uint32_t s_peak;
uint32_t mgfx_arena_peak(void) { return s_peak; }

/* Metatile lookups that fell outside the cached tables. Must stay 0 on every real map;
 * logged after a load so a hardware run leaves evidence instead of a silent glitch. */
static uint32_t s_mt_misses;
uint32_t mgfx_mt_misses(void) { return s_mt_misses; }

/* Arena bytes still held AFTER a load (caches, strips, warps). mgfx_arena_peak() is
 * dominated by phase 1's transient 32,768 B and would hide this number entirely. */
static uint32_t s_phase2;
uint32_t mgfx_arena_phase2(void) { return s_phase2; }

/* ---- a bump allocator over the borrowed arena -----------------------------
 * One cursor, reset between the load phase and the run phase. Every allocation is
 * checked: the arena aliases the PC-storage buffer, so silently running past the end
 * would corrupt the user's Pokemon rather than glitch a tile. */
typedef struct { uint8_t* base; uint32_t len, used; } Bump;
static Bump s_bump;

static void bump_reset(uint8_t* base, uint32_t len) {
  s_bump.base = base; s_bump.len = len; s_bump.used = 0;
}
/* Defined with the tilemap fill further down; the strip builder below needs them. */
static uint16_t border_at(const MapGfx* g, int mx, int my);
static void     build_flat_tiles(void);   /* Z2's flat fallback palette, defined with the zoom */
static uint16_t cell_at(const MapGfx* g, const MapRender* mr, int mx, int my);

static uint32_t bump_avail(void) { return s_bump.base ? (s_bump.len - s_bump.used) : 0; }
static void*    bump_tail(void)  { return s_bump.base ?  s_bump.base + s_bump.used  : 0; }
static void* bump(uint32_t n) {
  n = (n + 3u) & ~3u;                              /* keep everything word-aligned */
  if (!s_bump.base || n > s_bump.len - s_bump.used) return 0;
  void* p = s_bump.base + s_bump.used;
  s_bump.used += n;
  if (s_bump.used > s_peak) s_peak = s_bump.used;
  return p;
}

/* ---- a memory-backed RomCtx so mr_lz77 never touches the SD mid-decode ----
 * mr_lz77 pulls its input through the RomCtx callback. Letting that callback hit FatFs
 * would mean an SD transfer per literal — and each transfer unmaps the ROM. So the
 * compressed blob is read ONCE into EWRAM and mr_lz77 is pointed at a shim over it,
 * which needs zero changes to map_render.c. */
typedef struct { const uint8_t* buf; uint32_t addr0, len; } MemWin;

static bool mem_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  MemWin* w = (MemWin*)ctx;
  /* rom_read_at() converts a ROM address to a file offset, so `off` arrives as a file
   * offset; the window knows which file offset its byte 0 corresponds to. */
  if (off < w->addr0) return false;
  uint32_t rel = off - w->addr0;
  if (rel > w->len || len > w->len - rel) return false;
  memcpy(dst, w->buf + rel, len);
  return true;
}

/* ---- mode setup ----------------------------------------------------------- */

void mgfx_enter(void) {
  REG_DISPCNT = DCNT_BLANK;                        /* nothing garbled is ever shown */

  /* Two metatile layers + the HUD. BG1 (top) must draw IN FRONT of BG3 (bottom), so it
   * gets the lower priority number; BG0 (HUD) sits in front of both. */
  REG_BG0CNT = BG_CBB(CBB_HUD)   | BG_SBB(SBB_BG0_HUD)    | BG_4BPP | BG_REG_32x32 | BG_PRIO(0);
  REG_BG1CNT = BG_CBB(0)         | BG_SBB(SBB_BG1_TOP)    | BG_4BPP | BG_REG_32x32 | BG_PRIO(1);
  REG_BG3CNT = BG_CBB(0)         | BG_SBB(SBB_BG3_BOTTOM) | BG_4BPP | BG_REG_32x32 | BG_PRIO(2);

  /* These are WRITE-ONLY registers — they cannot be read back, so give them known
   * values rather than assuming anything about what the previous screen left. */
  REG_BG0HOFS = REG_BG0VOFS = 0;
  REG_BG1HOFS = REG_BG1VOFS = 0;
  REG_BG3HOFS = REG_BG3VOFS = 0;
  REG_MOSAIC = 0;

  /* HUD LEGIBILITY. The HUD is white 4bpp text on a TRANSPARENT paper (tte's SE renderer
   * snaps every glyph to an 8x8 screen entry, so a 1px drop shadow is not expressible and
   * palette index 0 is transparent by hardware rule — neither of the cheap tricks works).
   * Over pale terrain — Route 121's grey ledges, Lavender Town's roofs — white-on-white
   * made the text unreadable, which matters because Guy reads coordinates off phone photos
   * of the screen.
   *
   * So darken the MAP under the two HUD strips and leave the text alone: WIN0 over the top
   * line, WIN1 over the bottom three, brightness-decrease applied to every layer EXCEPT
   * BG0 (the text). Inside/outside both list every BG so this survives the Mode-0 tile
   * view, the Mode-1 affine zoom and the region map without per-mode bookkeeping. */
  REG_WIN0H = (0 << 8) | 240;   REG_WIN0V = (0   << 8) | 14;    /* map name + coords */
  REG_WIN1H = (0 << 8) | 240;   REG_WIN1V = (128 << 8) | 160;   /* the three info rows */
  REG_WININ  = ((WIN_BG0|WIN_BG1|WIN_BG2|WIN_BG3|WIN_OBJ|WIN_BLD))
             | ((WIN_BG0|WIN_BG1|WIN_BG2|WIN_BG3|WIN_OBJ|WIN_BLD) << 8);
  REG_WINOUT = (WIN_BG0|WIN_BG1|WIN_BG2|WIN_BG3|WIN_OBJ);       /* no BLD outside */
  REG_BLDCNT = MGFX_HUD_BLDCNT;
  REG_BLDY   = MGFX_HUD_BLDY;                                   /* out of 16 */

  /* HUD text as tiles. tte_init_se's default se0 of 0xF000 selects palette bank 15 —
   * exactly the range a map load never writes (banks 0-12 are the tilesets), so the HUD
   * colours can never be clobbered by loading a map. */
  tte_init_se(0, BG_CBB(CBB_HUD) | BG_SBB(SBB_BG0_HUD) | BG_4BPP | BG_REG_32x32,
              0xF000, CLR_WHITE, 0, &sys8Font, NULL);
  tte_erase_screen();

  /* memset32, not memset: VRAM turns an 8-bit store into a halfword *0x0101. Zero is the
   * same either way, so this is prophylactic — but every VRAM write in this file should
   * be provably >= 16 bits wide, because the one that was not caused the zoom corruption. */
  memset32(&se_mem[SBB_BG1_TOP],    0, 512);       /* 2048 B = 512 words */
  memset32(&se_mem[SBB_BG3_BOTTOM], 0, 512);

  /* Integrity canary, past the HUD font inside CBB3. WORD stores only — an 8-bit store
   * into VRAM lands in both halves of the halfword, which is the bug that produced the
   * original zoom corruption. */
  {
    volatile uint32_t* c = (volatile uint32_t*)0x0600CC20u;
    c[0] = 0x5047424Du; c[1] = ~0x5047424Du;
    c[2] = 0x5047424Du; c[3] = ~0x5047424Du;
  }
}

void mgfx_show(void) {
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_BG3 | MGFX_HUD_WIN;
}

void mgfx_exit(void) {
  REG_DISPCNT = DCNT_BLANK;
  REG_BG0HOFS = REG_BG0VOFS = 0;
  REG_BG1HOFS = REG_BG1VOFS = 0;
  REG_BG3HOFS = REG_BG3VOFS = 0;
  REG_BG0CNT = REG_BG1CNT = REG_BG2CNT = REG_BG3CNT = 0;
  /* CRITICAL: in Mode 3 the bitmap IS BG2, and BG2's AFFINE registers still apply to it.
   * Leaving the zoom transform loaded came back as a scaled+offset UI (misplaced box
   * sprites, a clipped file browser). These are WRITE-ONLY, so they cannot be "restored" —
   * they must be reset to the identity transform explicitly. */
  REG_BG2PA = 0x100; REG_BG2PB = 0;
  REG_BG2PC = 0;     REG_BG2PD = 0x100;
  REG_BG2X  = 0;     REG_BG2Y  = 0;
  REG_BG3PA = 0x100; REG_BG3PB = 0;
  REG_BG3PC = 0;     REG_BG3PD = 0x100;
  REG_BG3X  = 0;     REG_BG3Y  = 0;
  REG_MOSAIC = 0;
  REG_BLDCNT = 0;
  REG_WININ = REG_WINOUT = 0;
  /* Mode 3 is direct-colour and every PokeDNA screen full-paints on entry, so neither
   * VRAM nor BG PALRAM needs restoring — re-running ui_init() is a complete restore. */
  ui_init();
}

/* ---- palettes -------------------------------------------------------------
 * Mirrors the game's LoadTilesetPalette: primary banks 0-5 -> entries 0..95, and the
 * SECONDARY tileset's OWN banks 6..12 -> entries 96..207 (indexed at the same slot, not
 * slot-6 — the palette trap map_render.c documents). Entry 0 is forced to black: the
 * game discards the primary tileset's own colour 0 and never displays it. */
void mgfx_repaint_palettes(const MapRender* mr) {
  /* The split is PER GAME: NUM_PALS_IN_PRIMARY is 6 on Ruby/Sapphire/Emerald but 7 on
   * FireRed/LeafGreen. Hardcoding 6 gave FRLG bank 6 the SECONDARY tileset's colours,
   * which is wrong for every metatile that uses it. */
  int split = mr->split_pal;
  for (int b = 0; b < split; b++)
    for (int c = 0; c < 16; c++) pal_bg_mem[b * 16 + c] = mr->prim_pal[b][c];
  /* Up to bank 14, not 13. MEASURED: 8 Emerald and 8 Ruby metatiles reference palette
   * slot 14, and two of them (ids 586/587) are used by Littleroot's own blockdata — with
   * banks 13/14 left unwritten those quadrants painted from whatever UI colours happened
   * to be there. Bank 15 is tte's: mgfx_enter passes se0 = 0xF000, which selects bank 15
   * only, so leaving exactly one bank to the HUD keeps the map from ever clobbering it. */
  for (int b = split; b < 15; b++)
    for (int c = 0; c < 16; c++) pal_bg_mem[b * 16 + c] = mr->sec_pal[b][c];
  pal_bg_mem[0] = 0;                               /* RGB_BLACK backdrop */
}

/* ---- tileset -> VRAM ------------------------------------------------------
 * mr_lz77 writes BYTES, and an 8-bit store into BG VRAM does NOT do what C says: the
 * hardware writes the byte into BOTH halves of the addressed halfword (GBATEK, "8bit
 * Writes to Video Memory": [addr AND NOT 1] = data*0x0101). It is a corrupting write, not
 * a dropped one. mr_lz77 also reads back what it wrote to resolve back-references, so
 * decompressing straight into VRAM would compound the damage through the whole stream.
 * Hence: SD -> EWRAM stage -> decompress in EWRAM -> DMA (32-bit) to VRAM.
 *
 * This file previously described the rule as "VRAM silently drops 8-bit writes", and that
 * misunderstanding is exactly what produced the zoom-out corruption below — the affine
 * tilemap's one-byte entries were written with plain byte stores. */
static bool tileset_to_vram(const RomCtx* rom, uint32_t ts_addr, uint32_t vram_dst,
                            uint32_t tile_cap_bytes, uint8_t* stage, uint8_t* decomp) {
  RomTileset ts;
  if (!rom_tileset(rom, ts_addr, &ts) || !ts.tiles) return false;

  if (!ts.compressed) {
    /* Uncompressed sets (Cable Club / Secret Base) have no length field; take the cap. */
    if (!rom_read_at(rom, ts.tiles, decomp, tile_cap_bytes)) return false;
    dma3_cpy((void*)vram_dst, decomp, tile_cap_bytes);
    return true;
  }

  uint32_t want = mr_lz77_size(rom, ts.tiles);
  if (!want || want > DECOMP_BYTES || want > tile_cap_bytes) return false;

  /* One sequential read of the compressed blob. A short read at end-of-ROM is fine —
   * the decoder stops at the declared size and the window bounds-checks every fetch. */
  uint32_t off = ts.tiles - ROM_BASE;
  uint32_t n = STAGE_BYTES;
  if (off + n > rom->size) n = rom->size - off;
  if (!rom->read(rom->ctx, off, stage, n)) return false;

  MemWin w = { stage, off, n };
  RomCtx shim = *rom;                              /* same fmt/addresses, memory-backed I/O */
  shim.read = mem_read;
  shim.ctx  = &w;

  uint32_t got = mr_lz77(&shim, ts.tiles, decomp, DECOMP_BYTES);
  if (got != want) return false;                   /* truncated / corrupt — refuse */
  dma3_cpy((void*)vram_dst, decomp, (got + 3u) & ~3u);
  return true;
}

/* ---- connections + strips (stitching) --------------------------------------
 * Called AFTER the blockdata and border are cached, because it falls back to border_at
 * for cells a neighbour does not cover. Everything it allocates is graceful: if the arena
 * is short, `cells` stays NULL and that edge simply shows the border, exactly as before
 * stitching existed. bump() refuses rather than overflows, and the arena IS g_pc. */
static void build_connections(MapGfx* g, const MapRender* mr, const RomCtx* rom,
                              const RomLayout* lay, uint32_t conn_addr) {
  g->conn_n = 0;
  int n = rom_connection_count(rom, conn_addr);
  int W = lay->width, H = lay->height;

  for (int i = 0; i < n && g->conn_n < MGFX_MAX_CONN; i++) {
    RomConnection c;
    if (!rom_connection(rom, conn_addr, i, &c)) continue;
    /* DIVE(5)/EMERGE(6) are the Dive HM's vertical link, not a neighbour. MEASURED: every
     * one has offset 0 and IDENTICAL dimensions, so treating them as a direction paints
     * the Underwater twin exactly on top of the surface map — plausible and completely
     * wrong. The game skips them here too. */
    if (c.direction < ROM_CONN_SOUTH || c.direction > ROM_CONN_EAST) continue;

    RomMapHeader nh; RomLayout nl;
    if (!rom_map_header(rom, c.group, c.num, &nh)) continue;
    if (!rom_layout(rom, nh.layout, &nl)) continue;
    if (nl.width <= 0 || nl.height <= 0) continue;
    if ((uint32_t)nl.width * (uint32_t)nl.height > 10240u) continue;   /* the game's bound */
    /* Retail NEVER differs on the PRIMARY tileset (0 of 398 links across three ROMs). If a
     * hack ever does, refuse rather than render the neighbour through the wrong tileset. */
    if (nl.tileset_primary != lay->tileset_primary) continue;

    MapConn* mc = &g->conn[g->conn_n++];
    memset(mc, 0, sizeof *mc);
    mc->dir = (uint8_t)c.direction; mc->group = c.group; mc->num = c.num;
    mc->off = c.offset; mc->nb_w = nl.width; mc->nb_h = nl.height;

    /* ---- the visible band of this neighbour, clipped to what can ever be on screen ----
     * Try the FAR (1/4-view) depth first and fall back to the near one; a strip that is
     * merely shallower still stitches the seam, whereas skipping it shows a border wall. */
    uint32_t nb_bytes = (uint32_t)nl.width * (uint32_t)nl.height * 2u;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0, h = 0;
    uint32_t need = 0;
    bool fits = false;
    for (int tier = 0; tier < 2 && !fits; tier++) {
      int we = tier ? STRIP_WE : STRIP_WE_FAR;
      int ns = tier ? STRIP_NS : STRIP_NS_FAR;
      if (c.direction == ROM_CONN_SOUTH || c.direction == ROM_CONN_NORTH) {
        x0 = (int)c.offset; x1 = x0 + (int)nl.width;
        if (x0 < -we)     x0 = -we;
        if (x1 >  W + we) x1 =  W + we;
        int d = (nl.height < ns) ? (int)nl.height : ns;
        if (c.direction == ROM_CONN_SOUTH) { y0 = H;  y1 = H + d; }
        else                               { y1 = 0;  y0 = -d;    }
      } else {
        y0 = (int)c.offset; y1 = y0 + (int)nl.height;
        if (y0 < -ns)     y0 = -ns;
        if (y1 >  H + ns) y1 =  H + ns;
        int d = (nl.width < we) ? (int)nl.width : we;
        if (c.direction == ROM_CONN_EAST) { x0 = W;  x1 = W + d; }
        else                              { x1 = 0;  x0 = -d;    }
      }
      if (x1 <= x0 || y1 <= y0) break;
      w = x1 - x0; h = y1 - y0;
      if (w <= 0 || h <= 0 || w > 256 || h > 256) break;    /* offsets are SIGNED */
      need = ((uint32_t)w * (uint32_t)h * 2u + 3u) & ~3u;
      fits = (need + nb_bytes <= bump_avail());
    }
    if (!fits) continue;                                    /* graceful: border instead */

    uint16_t* cells = (uint16_t*)bump(need);
    if (!cells) continue;
    /* Stage the WHOLE neighbour into the arena's untouched tail (after the bump, so it can
     * never alias `cells`): slicing a west/east strip row by row would be up to 112 seeks,
     * and every seek is an SD transfer. Worst neighbour is 13,600 B. */
    uint16_t* stage = (uint16_t*)bump_tail();
    if (!rom_blocks(rom, &nl, 0, stage, (uint32_t)nl.width * (uint32_t)nl.height)) continue;

    /* MEASURED: connected maps ALWAYS share the primary tileset, but the SECONDARY differs
     * on ~28% of links. Substituting HERE, at build time, means cell_at never has to know:
     * a neighbour cell whose id >= split_mt under a different secondary would index the
     * CURRENT map's table and come back as an unrelated metatile with unrelated tiles and
     * an unrelated palette — confetti, i.e. the "random tiles outside the room" bug
     * returning. Border is strictly better for those cells. Measured cost of the
     * substitution over a depth-16 band on every link: 95.3% EM / 95.6% RU / 96.2% FR of
     * cells still render EXACTLY, and most links are 100% exact. */
    bool sec_ok = (nl.tileset_secondary == lay->tileset_secondary);
    int32_t ox, oy;
    if (!rom_conn_origin(c.direction, c.offset, W, H, nl.width, nl.height, &ox, &oy)) continue;

    for (int yy = 0; yy < h; yy++) {
      int my = y0 + yy, nby = my - (int)oy;
      for (int xx = 0; xx < w; xx++) {
        int mx = x0 + xx, nbx = mx - (int)ox;
        uint16_t cell;
        if ((unsigned)nbx < (unsigned)nl.width && (unsigned)nby < (unsigned)nl.height) {
          cell = stage[(uint32_t)nby * (uint32_t)nl.width + (uint32_t)nbx];
          if (!sec_ok && ROM_CELL_METATILE(cell) >= mr->split_mt) cell = border_at(g, mx, my);
        } else {
          cell = border_at(g, mx, my);        /* gaps between same-direction connections */
        }
        cells[(uint32_t)yy * (uint32_t)w + (uint32_t)xx] = cell;
      }
    }
    mc->x0 = (int16_t)x0; mc->y0 = (int16_t)y0;
    mc->w  = (int16_t)w;  mc->h  = (int16_t)h;
    mc->cells = cells;
  }
}

/* ---- warps ---------------------------------------------------------------- */
static void build_warps(MapGfx* g, const RomCtx* rom, uint32_t events_addr) {
  g->warps = 0; g->warp_n = 0;
  RomMapEvents ev;
  if (!rom_map_events(rom, events_addr, &ev) || !ev.warp_count) return;
  int n = ev.warp_count;
  if (n > MGFX_MAX_WARP) n = MGFX_MAX_WARP;      /* measured max on any real map is 38 */
  RomWarp* w = (RomWarp*)bump((uint32_t)n * sizeof(RomWarp));
  if (!w) return;
  int k = 0;
  for (int i = 0; i < n; i++) if (rom_warp(rom, &ev, i, &w[k])) k++;
  g->warps = w; g->warp_n = (uint8_t)k;
}

/* ---- load ----------------------------------------------------------------- */

bool mgfx_can_load(const RomCtx* rom, const RomLayout* lay, uint32_t arena_len) {
  if (!rom || !lay) return false;
  if (lay->width <= 0 || lay->height <= 0) return false;
  if ((uint32_t)lay->width * (uint32_t)lay->height > 10240u) return false;
  if (!rom_ptr_ok(rom, lay->blocks) || !rom_ptr_ok(rom, lay->tileset_primary)) return false;
  RomTileset p;
  if (!rom_tileset(rom, lay->tileset_primary, &p) || !p.tiles) return false;
  uint32_t pcap = (uint32_t)rom->fmt.metatiles_primary * 16u;
  if (mr_metatile_table_bytes(rom, lay->tileset_primary) > pcap) return false;
  if (lay->tileset_secondary) {
    RomTileset s;
    if (!rom_tileset(rom, lay->tileset_secondary, &s) || !s.tiles) return false;
    if (mr_metatile_table_bytes(rom, lay->tileset_secondary) > (1024u * 16u - pcap)) return false;
  }
  /* slot_of + blockdata are the only MANDATORY phase-2 allocations; strips and warps are
   * all graceful. 512 B of slack keeps a marginal map from loading with zero headroom. */
  return 1024u + (uint32_t)lay->width * (uint32_t)lay->height * 2u + 512u <= arena_len;
}

bool mgfx_load(MapGfx* g, MapRender* mr, const RomCtx* rom, const RomLayout* lay,
               uint32_t conn_addr, uint32_t events_addr,
               uint8_t* arena, uint32_t arena_len) {
  if (!g || !mr || !rom || !lay || !arena) return false;
  memset(g, 0, sizeof *g);
  s_peak = 0; s_phase2 = 0;

  if (!mr_init(mr, rom, lay)) return false;

  /* ---- phase 1: decompression (transient buffers) ---- */
  bump_reset(arena, arena_len);
  uint8_t* stage  = (uint8_t*)bump(STAGE_BYTES);
  uint8_t* decomp = (uint8_t*)bump(DECOMP_BYTES);
  if (!stage || !decomp) return false;

  bool ok = tileset_to_vram(rom, lay->tileset_primary, VRAM_MAP_CHAR,
                            (uint32_t)rom->fmt.tiles_primary * 32u, stage, decomp);
  if (ok && lay->tileset_secondary) {
    /* Secondary char data sits immediately after the primary's slot so tile ids
     * 512..1023 (640..1023 on FRLG) address it with no offset arithmetic. */
    uint32_t sec_dst = VRAM_MAP_CHAR + (uint32_t)rom->fmt.tiles_primary * 32u;
    ok = tileset_to_vram(rom, lay->tileset_secondary, sec_dst,
                         32768u - (uint32_t)rom->fmt.tiles_primary * 32u, stage, decomp);
  }
  if (!ok) return false;

  mgfx_repaint_palettes(mr);

  /* ---- phase 2: RELEASE the staging buffers, then cache what we keep ----
   * Both sets together are 62,736 B against a 35,712 B arena; they must never be
   * co-resident. Everything above this line is dead now. */
  bump_reset(arena, arena_len);

  uint32_t pmb = mr_metatile_table_bytes(rom, lay->tileset_primary);
  uint32_t smb = lay->tileset_secondary ? mr_metatile_table_bytes(rom, lay->tileset_secondary) : 0;
  RomTileset p, s;
  if (!rom_tileset(rom, lay->tileset_primary, &p)) return false;
  bool have_sec = lay->tileset_secondary && rom_tileset(rom, lay->tileset_secondary, &s);

  /* MANDATORY CLAMP before anything is written. mr_metatile_table_bytes only caps at
   * 1024*16 PER TABLE, so a malformed or hacked ROM could report 16,384 for both and
   * smear 16 KB past 0x06013FFF into the box screen's sprite tiles, which mgfx_exit does
   * not restore. Refuse the map instead. */
  uint32_t pcap = (uint32_t)mr->split_mt * 16u;
  uint32_t scap = (uint32_t)(1024u - mr->split_mt) * 16u;
  if (pmb > pcap || smb > scap) return false;

  uint32_t prim_dst = VRAM_MT_BASE;
  uint32_t sec_dst  = VRAM_MT_BASE + pcap;

  /* Staged through the arena, then DMA32'd out — two reasons, both load-bearing:
   *  1. f_read writes BYTES, and an 8-bit store into VRAM lands in BOTH halves of the
   *     halfword. Reading straight into VRAM would shred the table.
   *  2. Gen-3 metatile arrays are `const u16*` and on RSE a large share of them start at
   *     a ROM address that is 2 mod 4. Reading from `addr - (addr & 3)` and dropping the
   *     pad keeps every f_read word-aligned at BOTH ends, so this cannot depend on the
   *     diskio bounce being right. (It is now — but defence in depth is cheap here.) */
  uint8_t* mts = (uint8_t*)bump(MT_SCRATCH + 4u);
  if (!mts) return false;
  if (pmb) {
    uint32_t pad = p.metatiles & 3u;
    if (pmb + pad > MT_SCRATCH + 4u) return false;
    if (!rom_read_at(rom, p.metatiles - pad, mts, pmb + pad)) return false;
    if (pad) memmove(mts, mts + pad, pmb);
    dma3_cpy((void*)prim_dst, mts, (pmb + 3u) & ~3u);
  }
  if (smb && have_sec) {
    uint32_t pad = s.metatiles & 3u;
    if (smb + pad > MT_SCRATCH + 4u) return false;
    if (!rom_read_at(rom, s.metatiles - pad, mts, smb + pad)) return false;
    if (pad) memmove(mts, mts + pad, smb);
    dma3_cpy((void*)sec_dst, mts, (smb + 3u) & ~3u);
  }
  bump_reset(arena, arena_len);                   /* the scratch is dead */

  /* 8-bit VRAM READS are legal (only writes misbehave), and mr_metatile_entries reads the
   * table byte-wise through rd16 — the mip builder already depends on exactly that for the
   * tile data. So the tables can simply live in VRAM and be read in place. */
  g->mt_prim = (uint8_t*)prim_dst; g->mt_prim_len = pmb;
  g->mt_sec  = (uint8_t*)sec_dst;  g->mt_sec_len  = smb;
  mr_set_metatiles(mr, g->mt_prim, g->mt_prim_len, g->mt_sec, g->mt_sec_len);
  /* Point the core's tile pointers at VRAM. VRAM 8-bit READS are perfectly legal (only
   * WRITES are dropped), so the mip builder can composite straight out of the tile data we
   * just uploaded — no second copy in EWRAM, which there is no room for. */
  {
    uint32_t pb = (uint32_t)rom->fmt.tiles_primary * 32u;
    mr_set_tiles(mr, (const uint8_t*)VRAM_MAP_CHAR, pb,
                     (const uint8_t*)(VRAM_MAP_CHAR + pb), 32768u - pb);
  }

  /* metatile id -> affine slot for the zoom level. Persistent across window rebuilds so
   * a mip is built once per metatile ever seen, not once per cursor step. */
  g->slot_of = (uint8_t*)bump(1024);
  if (!g->slot_of) return false;
  memset(g->slot_of, 0xFF, 1024);
  g->slots_used = 0;
  /* The affine tilemap survives a map change and still holds the PREVIOUS map's slot
   * bytes outside the 34x22 window. Invisible today only because the visible 31x21 sits
   * inside that window — one off-by-one from being a stale strip on screen. Word-wise:
   * an 8-bit VRAM store would land in both halves of the halfword. */
  memset32((void*)VRAM_AFF_MAP, 0, 1024);
  /* Slot 0 = the all-transparent tile. Written with memset32 and NO staging buffer.
   *
   * This used to build the 64 bytes in a local and dma3_cpy it out, and GCC DELETED the fill:
   * tonc's dma3_cpy is INLINE and does nothing with the source but store its ADDRESS into a
   * volatile register, so alias analysis saw a local written and never read. Verified in the
   * shipped binary — the DMA ran with the source pointing at uninitialised stack. memset32 is a
   * real out-of-line call, so there is nothing for dead-store elimination to remove. */
  memset32((void*)0x06008000u, 0, 16);             /* 16 words = 64 B = one 8bpp tile */
  build_flat_tiles();               /* the Z2 approximation's fallback palette */

  /* Whole-map blockdata: the biggest map is 6,400 cells = 12,800 B, so it always fits and
   * scrolling then needs no SD access at all. */
  uint32_t cells = (uint32_t)lay->width * (uint32_t)lay->height;
  g->blocks = (uint16_t*)bump(cells * 2u);
  if (!g->blocks) return false;
  if (!rom_blocks(rom, lay, 0, g->blocks, cells)) return false;
  g->blocks_cells = cells;

  /* The border: what the game shows outside the map bounds. Normally 2x2 metatiles
   * (8 bytes); FRLG stores explicit dimensions. Tiny, so it lives in the struct. */
  g->bw = lay->border_w ? lay->border_w : 2;
  g->bh = lay->border_h ? lay->border_h : 2;
  if ((uint32_t)g->bw * g->bh > 16) { g->bw = 2; g->bh = 2; }
  if (!lay->border || !rom_read_at(rom, lay->border, g->border,
                                   (uint32_t)g->bw * g->bh * 2u)) {
    for (int i = 0; i < 16; i++) g->border[i] = 0;
    g->bw = g->bh = 1;
  }

  /* Order matters: both of these fall back to border_at, so the border must already be
   * cached, and both allocate from what is left of the arena. */
  build_connections(g, mr, rom, lay, conn_addr);
  build_warps(g, rom, events_addr);

  s_phase2 = s_bump.used;
  /* Fingerprint what this load produced, here rather than at the call site: first load,
   * seam crossing, warp, region round trip and repair all funnel through mgfx_load, so it
   * cannot be forgotten. */
  mgfx_fp_capture(g, mr, arena);
  g->ring_valid = false;
  return true;
}

/* ---- tilemap fill --------------------------------------------------------- */

/* Outside the map, return the BORDER metatile the game would show there — not entry 0,
 * which is tileset tile 0 and therefore an arbitrary real tile (the "random tile outside
 * the room"). Modulo is written to work for negative coordinates. */
static uint16_t border_at(const MapGfx* g, int mx, int my) {
  int bw = g->bw ? g->bw : 1, bh = g->bh ? g->bh : 1;
  /* Mask when the dims are powers of two (always 2x2 on RSE, almost always on FRLG):
   * '%' on ARM7TDMI is an __aeabi_idivmod CALL, and this runs 748x per Z1 rebuild. */
  int bxi = (bw & (bw - 1)) ? (((mx % bw) + bw) % bw) : (mx & (bw - 1));
  int byi = (bh & (bh - 1)) ? (((my % bh) + bh) % bh) : (my & (bh - 1));
  return g->border[byi * bw + bxi];
}

/* THE single rendering hook for what is at a map cell. put_metatile (Z0) and
 * mgfx_zoom_build (Z1) both go through it, so stitching lands on both zoom levels at once. */
static uint16_t cell_at(const MapGfx* g, const MapRender* mr, int mx, int my) {
  if ((unsigned)mx < (unsigned)mr->lay.width && (unsigned)my < (unsigned)mr->lay.height)
    return g->blocks[(uint32_t)my * (uint32_t)mr->lay.width + (uint32_t)mx];
  /* Off the map: a connected neighbour's real blocks if we cached a strip there.
   * LAST connection wins where two overlap — the game's fills run in list order and each
   * blits over the previous one (InitBackupMapLayoutConnections). No retail rect actually
   * overlaps at these depths; this just makes ROM hacks behave predictably. */
  for (int i = (int)g->conn_n - 1; i >= 0; i--) {
    const MapConn* c = &g->conn[i];
    if (!c->cells) continue;
    int rx = mx - c->x0, ry = my - c->y0;
    if ((unsigned)rx < (unsigned)c->w && (unsigned)ry < (unsigned)c->h)
      return c->cells[(uint32_t)ry * (uint32_t)c->w + (uint32_t)rx];
  }
  return border_at(g, mx, my);
}

/* Write one metatile's 8 entries into the two layer tilemaps. */
static void put_metatile(const MapGfx* g, const MapRender* mr, int mx, int my) {
  uint16_t* bot = se_mem[SBB_BG3_BOTTOM];
  uint16_t* top = se_mem[SBB_BG1_TOP];
  uint32_t se = MGFX_SE(mx, my);
  uint16_t cell = cell_at(g, mr, mx, my);         /* border metatile when off-map */
  uint16_t e[8];
  if (!mr_metatile_entries(mr, ROM_CELL_METATILE(cell), e)) {
    /* Do NOT return silently: that leaves the PREVIOUS camera's entries in this slot, so
     * a miss shows as a stale fragment sliding around the map rather than as a hole. */
    bot[se] = bot[se + 1] = bot[se + 32] = bot[se + 33] = 0;
    top[se] = top[se + 1] = top[se + 32] = top[se + 33] = 0;
    s_mt_misses++;
    return;
  }
  /* entries 0-3 = bottom layer, 4-7 = top layer; within a layer TL, TR, BL, BR */
  bot[se]      = e[0]; bot[se + 1]  = e[1];
  bot[se + 32] = e[2]; bot[se + 33] = e[3];
  top[se]      = e[4]; top[se + 1]  = e[5];
  top[se + 32] = e[6]; top[se + 33] = e[7];
}

static void fill_ring(MapGfx* g, const MapRender* mr, int base_mx, int base_my) {
  for (int y = 0; y < MGFX_RING; y++)
    for (int x = 0; x < MGFX_RING; x++)
      put_metatile(g, mr, base_mx + x, base_my + y);
  g->base_mx = base_mx; g->base_my = base_my; g->ring_valid = true;
}

void mgfx_camera(MapGfx* g, const MapRender* mr, int cur_mx, int cur_my) {
  if (!g || !mr) return;

  /* Put the cursor's centre at screen centre (120, 80). */
  int sx = cur_mx * 16 + 8 - 120;
  int sy = cur_my * 16 + 8 - 80;

  /* Leftmost/topmost metatile the screen can touch. floor division, not truncation —
   * negative scroll happens at the map's top-left corner. */
  int want_mx = (sx >= 0) ? (sx / 16) : -(((-sx) + 15) / 16);
  int want_my = (sy >= 0) ? (sy / 16) : -(((-sy) + 15) / 16);

  if (!g->ring_valid) {
    fill_ring(g, mr, want_mx, want_my);
  } else {
    int dx = want_mx - g->base_mx, dy = want_my - g->base_my;
    if (dx <= -MGFX_RING || dx >= MGFX_RING || dy <= -MGFX_RING || dy >= MGFX_RING) {
      fill_ring(g, mr, want_mx, want_my);          /* jumped: cheaper to redo everything */
    } else {
      /* Refill only the newly exposed columns, then rows. The ring wraps mod 16, so the
       * slot being rewritten is the one that just scrolled off the far side — provably
       * off-screen for a full frame (the BG shows 240 of its 256 px). */
      while (g->base_mx != want_mx) {
        int step = (dx > 0) ? 1 : -1;
        int col = (step > 0) ? g->base_mx + MGFX_RING : g->base_mx - 1;
        for (int y = 0; y < MGFX_RING; y++) put_metatile(g, mr, col, g->base_my + y);
        g->base_mx += step;
      }
      while (g->base_my != want_my) {
        int step = (dy > 0) ? 1 : -1;
        int row = (step > 0) ? g->base_my + MGFX_RING : g->base_my - 1;
        for (int x = 0; x < MGFX_RING; x++) put_metatile(g, mr, g->base_mx + x, row);
        g->base_my += step;
      }
    }
  }

  /* Both layers scroll together. & 255 because the BG is 256 px and wraps. */
  REG_BG1HOFS = REG_BG3HOFS = (uint16_t)(sx & 255);
  REG_BG1VOFS = REG_BG3VOFS = (uint16_t)(sy & 255);
}


/* ---- Z1: the affine zoom level -------------------------------------------- */

/* The window that must be valid in the affine map.
 *
 * VISIBLE at Z1 (BG2X = cur_mx*8 + 4 - 120, BG2Y = cur_my*8 + 4 - 80, PA = PD = 0x100):
 *     columns [cur_mx-15, cur_mx+15]   rows [cur_my-10, cur_my+10]     (31 x 21)
 *
 * The window is 34x22 because THE ORIGIN MUST BE EVEN. An 8-bit store into BG VRAM writes
 * the byte into BOTH halves of the addressed halfword (GBATEK, "8bit Writes to Video
 * Memory": [addr AND NOT 1] = data*0x0101), so the affine map's one-byte entries have to
 * go in as ALIGNED HALFWORD PAIRS. Rounding bx down to even would drop the rightmost
 * visible column on every odd cur_mx (a 4-px stale strip at x=236..239), so the width
 * grows 32 -> 34. Rows need no alignment: a row base is a multiple of 64 and the pairs
 * are horizontal, so a pair never straddles the 64-metatile ring wrap.
 *
 * THIS WAS THE ZOOM-OUT CORRUPTION. Writing `aff[ai] = slot` byte-wise made every write
 * clobber its pair partner, so each even column displayed its right neighbour's mip —
 * a horizontal shear across roughly half the screen's columns, in every game. Simulated
 * against all three real ROMs: 33.1% of pixels wrong before, 0 wrong after.
 *
 * MEASURED distinct metatiles in the window (every map x every cursor cell, off-map cells
 * resolved to the border metatile exactly as cell_at does):
 *     31x21 exact visible            206 EM / 199 FR / 188 RU
 *     32x22 unaligned bx (old)       221 EM / 210 FR / 197 RU
 *     34x22 even-aligned (THIS)      227 EM / 217 FR / 204 RU
 *     34x24 even-aligned             239 EM / 231 FR / 211 RU   <- do NOT grow further
 * against 255 usable slots (slot 0 is the blank tile). A refill is 374 halfword writes;
 * what is expensive is building mips, and the dictionary is PERSISTENT so that happens
 * once per newly-seen metatile, not once per step. */
#define ZW 34
#define ZH 22

/* Z2 (1/4 scale, 4 px per metatile) shows 60x40 metatiles at once. The affine map is 64x64
 * and wraps, so 64x44 is the largest window that cannot alias itself, and it covers the
 * visible 61x41 with margin. 64 is even, so the halfword-pair rule still holds. */
#define ZW2 64
#define ZH2 44

/* THE APPROXIMATION, and why Z2 needs one.
 *
 * An affine BG's tilemap entries are ONE byte, so it can address at most 256 distinct
 * tiles. MEASURED distinct metatiles in a Z2 window: up to 428 (Emerald) / 303 (FireRed)
 * against 255 usable slots. Z2 therefore CANNOT be exact — this is a property of the
 * hardware, not of the implementation.
 *
 * So the dictionary is split. Slots 1..MGFX_EXACT_MAX hold real per-metatile mips, handed
 * out first-come in map order. Everything that no longer fits falls back to a FLAT tile of
 * the metatile's centre-pixel colour, one per palette bank, in the slots above that. The
 * result reads as a correct low-resolution impression of the map — coastlines, buildings
 * and routes stay legible — with the busiest maps losing fine detail to flat blocks.
 * Being honest about that in the HUD ("1/4~") matters more than pretending it is exact. */
/* Banks 0..12 ONLY. Banks 13/14 hold whatever the secondary tileset happened to define
 * there and bank 15 is the HUD's, so a flat tile keyed on those paints a UI colour into
 * the middle of the map — it showed up as a magenta streak across the first Z2 render. */
#define MGFX_FLAT_BANKS  13
#define MGFX_EXACT_MAX   (MGFX_AFF_TILES - 1 - MGFX_FLAT_BANKS)   /* = 242 exact slots  */
#define MGFX_FLAT_BASE   (MGFX_EXACT_MAX + 1)                     /* 243..255           */

/* Build the 16 flat tiles once per load: slot MGFX_FLAT_BASE+b is filled with palette byte
 * (b<<4)|8, i.e. the middle colour of bank b. Written by DMA32, never byte stores. */
/* Same dead-store hazard as the blank tile above, and it bit here too: the `tile[i] = w` loop
 * was compiled away entirely and all 13 banks were filled from one uninitialised stack slot.
 * memset32 writes the constant directly to VRAM with no buffer to eliminate. */
static void build_flat_tiles(void) {
  for (int b = 0; b < MGFX_FLAT_BANKS; b++) {
    uint8_t px = (uint8_t)((b << 4) | 8);
    memset32((void*)(VRAM_MIP_CHAR + (uint32_t)(MGFX_FLAT_BASE + b) * 64u),
             (uint32_t)px * 0x01010101u, 16);
  }
}

/* Build the exact mip for one metatile into `slot`. Split out of zslot so the optimiser can
 * use the same code path — the two must never diverge. */
static void build_exact(const MapRender* mr, uint16_t mid, uint8_t slot) {
  uint8_t mip[MIP_PX * MIP_PX] ALIGN4;            /* it is a DMA32 source */
  memset(mip, 0, sizeof mip);
  mr_metatile_mip_idx(mr, mid, MIP_PX, mip, MIP_PX);
  dma3_cpy((void*)(VRAM_MIP_CHAR + (uint32_t)slot * 64u), mip, sizeof mip);
}

/* Assign (and build, on first sight) the mip slot for one map cell.
 *
 * `approx` is set at Z2, where the window genuinely cannot fit: instead of failing, a
 * metatile that misses the exact budget is given the flat tile for its dominant palette
 * bank. At Z1 approx is false and a full dictionary is a real overflow, handled by the
 * caller's clear-and-retry. */
static uint8_t zslot(MapGfx* g, const MapRender* mr, int mx, int my, bool approx,
                     bool* overflow) {
  uint16_t mid = ROM_CELL_METATILE(cell_at(g, mr, mx, my));   /* border when off-map */
  uint8_t slot = g->slot_of[mid];
  if (slot != 0xFF) return slot;
  int cap = approx ? MGFX_EXACT_MAX : (MGFX_AFF_TILES - 1);
  if (g->slots_used >= cap) {
    if (!approx) { *overflow = true; return 0; }
    /* Out of exact slots: point-sample the metatile's centre and use that bank's flat
     * tile. Recorded in slot_of so the (expensive) sample happens once per metatile. */
    uint8_t px[MIP_PX * MIP_PX] ALIGN4;
    memset(px, 0, sizeof px);
    mr_metatile_mip_idx(mr, mid, MIP_PX, px, MIP_PX);
    uint8_t mid_px = px[(MIP_PX / 2) * MIP_PX + (MIP_PX / 2)];
    int bank = (mid_px >> 4) & 15;
    if (bank >= MGFX_FLAT_BANKS) bank = MGFX_FLAT_BANKS - 1;   /* never a UI colour */
    slot = (uint8_t)(MGFX_FLAT_BASE + bank);
    g->slot_of[mid] = slot;
    return slot;
  }
  slot = (uint8_t)(++g->slots_used);              /* slot 0 stays the blank tile */
  g->slot_of[mid] = slot;
  /* 8bpp PALETTE INDICES: an affine BG is 8bpp and its tiles hold indices into the one
   * merged 256-entry palette, which is an identity mapping. */
  build_exact(mr, mid, slot);
  return slot;
}

/* ---- Z2 dictionary optimiser -------------------------------------------------
 * At Z2 the window holds 64x44 = 2,816 cells and can need up to 428 DISTINCT metatiles, but
 * an affine BG has 1-byte tilemap entries, so only 242 of them can be exact. The rest get a
 * flat colour.
 *
 * WHICH 242 is the whole question, and first-come-first-served — what zslot does on its own —
 * is the worst possible answer for two compounding reasons:
 *   1. `slot_of` is PERSISTENT and only grows, so after panning it is full of metatiles that
 *      have since scrolled off screen. New ones arriving at the edge get filler even though
 *      the budget is being spent on tiles nobody can see. This is exactly why leaving the map
 *      and re-entering "fixes" it — a reload clears the dictionary.
 *   2. Even with a FRESH dictionary, first-come means scan order, so the top of the screen is
 *      exact and the bottom is a solid block of filler.
 *
 * So rank by how much SCREEN AREA each metatile actually covers and give the 242 exact slots
 * to the biggest contributors. In terrain like the desert a handful of sand and dune tiles
 * cover most of the frame, so this converts a large filler region into a few scattered
 * pixels.
 *
 * RANKED ON RAW CELL COUNT, DELIBERATELY UNWEIGHTED. Weighting the middle of the window 2:1
 * — so that a wrong tile under the cursor costs more than one at the edge — was tried and
 * MEASURED WORSE on real maps: Emerald 26.14 went from 15% filler to 22%. The reason is
 * structural, not a tuning problem. Taking the 242 highest RAW counts is, by construction,
 * the maximum screen area 242 slots can possibly cover, so it can never lose to the lazy
 * first-come dictionary. Any weighting optimises a different quantity than the one the user
 * sees, and buys centre accuracy with strictly more total filler. If centre priority is ever
 * wanted it has to be a tie-break WITHIN equal counts, never a multiplier on them.
 *
 * The histogram lives IN `slot_of` — 1,024 counts written over the 1,024 slot bytes and then
 * converted back in place, ascending, reading each entry exactly once before overwriting it.
 * That keeps the whole thing at ZERO extra EWRAM, which matters with ~4.4 KB free.
 *
 * Below-threshold metatiles are left 0xFF rather than assigned a flat tile here: the build
 * walk resolves them lazily through zslot's existing approx path, which costs exactly what it
 * costs today instead of point-sampling all 428 up front. */
void mgfx_zoom_optimise(MapGfx* g, const MapRender* mr, int cur_mx, int cur_my) {
  if (!g || !mr || !g->slot_of) return;

  int bx = (cur_mx - ZW2 / 2) & ~1;
  int by = cur_my - ZH2 / 2;

  /* 1. Weighted histogram, saturating. 254 is a cap, not a count — ranking only needs the
   *    order, and no real window is decided by the difference between 254 and 255. */
  memset(g->slot_of, 0, 1024);
  for (int y = 0; y < ZH2; y++)
    for (int x = 0; x < ZW2; x++) {
      uint16_t mid = ROM_CELL_METATILE(cell_at(g, mr, bx + x, by + y));
      unsigned c = (unsigned)g->slot_of[mid] + 1u;
      g->slot_of[mid] = (uint8_t)(c > 254u ? 254u : c);
    }

  /* 2. The cut. A count-of-counts gives the threshold in one pass over 256 bins instead of
   *    sorting 1,024 entries: walk down from the most common until taking the next band
   *    would exceed the budget. */
  uint16_t cc[256];
  memset(cc, 0, sizeof cc);
  for (int mid = 0; mid < 1024; mid++) cc[g->slot_of[mid]]++;
  int thresh = 1, taken = 0;
  for (int v = 254; v >= 1; v--) {
    taken += (int)cc[v];
    thresh = v;
    if (taken >= MGFX_EXACT_MAX) break;
  }
  /* thresh INCLUDES the band that straddles the budget. Excluding it — stopping at the last
   * band that fits wholly — left up to 39 of the 242 slots unspent on a real window, because
   * counts tie heavily in the tail. The assignment loop's `slots_used < MGFX_EXACT_MAX`
   * guard trims the straddling band, so the budget is always spent exactly. */

  /* The band AT the threshold usually straddles the budget, and it can be enormous: on
   * Emerald 26.14 the window has 434 distinct metatiles averaging 6 cells each, so counting
   * down from 254 does not reach 242 tiles until thresh == 1 and EVERY tile qualifies.
   * Taking `c >= thresh` in ascending id order then means a metatile covering 500 cells
   * loses its slot to one covering a single cell purely because its id is higher — which
   * MEASURED 15% -> 27% filler, worse than the lazy dictionary it replaced.
   *
   * So split the band: everything strictly above the threshold is in (that set is smaller
   * than the budget by construction), and the equal-count band fills the remainder up to a
   * quota. Both numbers come out of cc[] before slot_of is touched, so this still needs no
   * second buffer. Within the band the choice is arbitrary because every member covers the
   * same area — that is the right place for a centre-distance tie-break if one is ever
   * wanted, and the wrong place is a weight on the counts themselves. */
  int above = 0;
  for (int v = thresh + 1; v <= 254; v++) above += (int)cc[v];
  int quota = MGFX_EXACT_MAX - above;              /* >= 1: `above` < budget by construction */

  /* 3. Convert the histogram into slot assignments, in place. */
  g->slots_used = 0;
  for (int mid = 0; mid < 1024; mid++) {
    uint8_t c = g->slot_of[mid];
    bool win = (c > thresh) || (c == thresh && quota > 0);
    if (c == thresh && win) quota--;
    if (c && win && g->slots_used < MGFX_EXACT_MAX) {
      uint8_t slot = (uint8_t)(++g->slots_used);   /* slot 0 stays the blank tile */
      g->slot_of[mid] = slot;
      build_exact(mr, (uint16_t)mid, slot);
    } else {
      g->slot_of[mid] = 0xFF;                      /* zslot will flat-fill it lazily */
    }
  }

  g->opt_mx = bx; g->opt_my = by; g->opt_valid = true;
  g->aff_valid = false;                            /* the tilemap must be rewritten */
}

bool mgfx_zoom_wants_opt(const MapGfx* g, int cur_mx, int cur_my) {
  if (!g || !g->window_cells) return false;
  int bx = (cur_mx - ZW2 / 2) & ~1;
  int by = cur_my - ZH2 / 2;
  if (g->opt_valid && bx == g->opt_mx && by == g->opt_my) return false;  /* already best-effort */
  /* Strictly more than 1% of the window, per Guy's threshold. 2,816 cells -> 28. */
  return (uint32_t)g->filler_cells * 100u > (uint32_t)g->window_cells;
}

int mgfx_zoom_filler_pct(const MapGfx* g) {
  if (!g || !g->window_cells) return 0;
  return (int)(((uint32_t)g->filler_cells * 100u + g->window_cells / 2u) / g->window_cells);
}

bool mgfx_zoom_build(MapGfx* g, const MapRender* mr, int cur_mx, int cur_my, int level) {
  if (!g || !mr || !g->slot_of) return false;

  bool approx = (level >= 2);
  int zw = approx ? ZW2 : ZW, zh = approx ? ZH2 : ZH;
  /* Anchor forced EVEN so every write is an aligned halfword pair. (mx & 63) preserves
   * parity, and a row base is a multiple of 64, so a pair never straddles the ring wrap. */
  int bx = (cur_mx - zw / 2) & ~1;
  int by = cur_my - zh / 2;

  /* With an even bx, half of all 1-metatile steps leave the window unchanged: those
   * become a pure BG2X write in mgfx_zoom_show. aff_valid is cleared by mgfx_load, so a
   * map change always forces a rebuild. */
  if (g->aff_valid && bx == g->aff_base_mx && by == g->aff_base_my) return true;

  uint16_t* aff = (uint16_t*)VRAM_AFF_MAP;        /* 32 halfwords per 64-entry row */
  u16  saved_dispcnt = 0;
  bool hidden = false;

  for (int attempt = 0; attempt < 2; attempt++) {
    bool overflow = false;
    uint32_t filler = 0, total = 0;
    for (int y = 0; y < zh && !overflow; y++) {
      int my = by + y;
      uint32_t row = (uint32_t)(my & (MGFX_AFF_RING - 1)) * (MGFX_AFF_RING / 2);
      for (int x = 0; x < zw; x += 2) {
        int mx = bx + x;                          /* even, so (mx, mx+1) is a pair */
        uint8_t a = zslot(g, mr, mx,     my, approx, &overflow);
        uint8_t b = zslot(g, mr, mx + 1, my, approx, &overflow);
        if (overflow) break;
        total += 2;
        if (a >= MGFX_FLAT_BASE) filler++;
        if (b >= MGFX_FLAT_BASE) filler++;
        aff[row + (uint32_t)((mx & (MGFX_AFF_RING - 1)) >> 1)] =
            (uint16_t)((uint16_t)a | ((uint16_t)b << 8));
      }
    }
    if (!overflow) {
      g->aff_base_mx = bx; g->aff_base_my = by; g->aff_valid = true;
      g->filler_cells = (uint16_t)filler;
      g->window_cells = (uint16_t)total;
      if (hidden) REG_DISPCNT = saved_dispcnt;
      return true;
    }
    /* NOT "cannot happen": slot_of is persistent, so panning across ~255 distinct
     * metatiles fills it as a matter of course. Rebuilding up to 227 mips takes several
     * frames, during which the LIVE tilemap still holds pre-reset slot numbers while CBB2
     * is being repurposed — so drop BG2 (the HUD stays up) until the refill is done. */
    if (!hidden) {
      saved_dispcnt = REG_DISPCNT;
      REG_DISPCNT = (u16)(saved_dispcnt & ~DCNT_BG2);
      hidden = true;
    }
    memset(g->slot_of, 0xFF, 1024);
    g->slots_used = 0;
    memset32((void*)VRAM_AFF_MAP, 0, 1024);       /* 4,096 B of stale slot bytes, written
                                                   * word-wise: no 8-bit VRAM store */
    g->aff_valid = false;
    g->opt_valid = false;                          /* the dictionary it optimised is gone */
  }
  if (hidden) REG_DISPCNT = saved_dispcnt;
  return false;
}

void mgfx_zoom_show(int level, int cur_mx, int cur_my) {
  /* Mode 1: BG0 stays a text BG for the HUD, BG2 becomes affine. */
  REG_BG2CNT = BG_CBB(2) | BG_SBB(26) | BG_AFF_64x64 | BG_WRAP | BG_PRIO(2);

  /* Identity scale at Z1: one mip pixel per screen pixel, so a metatile is 8x8 px and the
   * screen shows 30x20 metatiles. (Z2 would be PA/PD = 0x200; see the header for why it
   * needs an approximation first.)
   * The level < 1 guard is not decoration: `0x100 << -1` is UB, and on ARM7TDMI it shifts
   * by 255 and yields scale = 0, which stretches one texel over the whole screen. A trap
   * for whoever wires Z2 and calls this with level 0 by accident. */
  int scale = (level < 1) ? 0x100 : (0x100 << (level - 1));
  REG_BG2PA = (uint16_t)scale; REG_BG2PB = 0;
  REG_BG2PC = 0;               REG_BG2PD = (uint16_t)scale;

  /* Texture pixel that should land at screen (0,0), in 24.8 fixed point. A metatile is
   * MIP_PX texture pixels wide, and the affine map wraps every 64 metatiles. */
  int tex_x = cur_mx * MIP_PX + MIP_PX / 2 - (120 * scale >> 8);
  int tex_y = cur_my * MIP_PX + MIP_PX / 2 - ( 80 * scale >> 8);
  REG_BG2X = tex_x << 8;
  REG_BG2Y = tex_y << 8;

  REG_DISPCNT = DCNT_MODE1 | DCNT_BG0 | DCNT_BG2 | MGFX_HUD_WIN;
}

void mgfx_z0_show(void) {
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_BG3 | MGFX_HUD_WIN;
}

/* ---- render integrity ------------------------------------------------------
 * See map_gfx.h for why this exists. Stride sampling, not a full checksum: hashing all
 * 48 KB of tables+char costs 83,558 cycles = 99.7% of a VBlank, which is unusable. A
 * 128-byte stride catches any contiguous overwrite >= 128 B with certainty and a smaller
 * one with p = size/128. Every corruption class seen on this project has been bulk. */
#define FP_SAMPLES 128
#define CANARY_ADDR 0x0600CC20u          /* just past the HUD font inside CBB3 */
#define CANARY_W0   0x5047424Du          /* "MBGP" */

static uint32_t s_fp[MGFX_FP_SLICES];
static bool     s_fp_valid;

static uint32_t fp_stride(uint32_t base, uint32_t len) {
  if (!len) return 1u;                              /* 1, not 0: 0 means "no capture" */
  uint32_t step = len / FP_SAMPLES; if (step < 4u) step = 4u; step &= ~3u;
  uint32_t h = 0x811C9DC5u;
  for (uint32_t o = 0; o + 4u <= len; o += step)
    h = (h ^ *(const volatile uint32_t*)(base + o)) * 16777619u;
  return h | 1u;
}

static uint32_t fp_slice(const MapGfx* g, const MapRender* mr, const uint8_t* arena, int slice) {
  switch (slice) {
    case 0:  /* metatile tables: both halves of the OBJ-VRAM block we own */
      return fp_stride(VRAM_MT_BASE, (uint32_t)mr->split_mt * 16u
                                   + (uint32_t)(1024u - mr->split_mt) * 16u);
    case 1:  return fp_stride(VRAM_MAP_CHAR, 32768u);
    case 2: {
      /* Beyond slot_of only: slot_of itself is rebuilt constantly. s_phase2 is what the
       * load actually retained, so this also guards the strips and the warp list. */
      uint32_t held = s_phase2;
      if (held <= 1024u || !arena) return 1u;
      return fp_stride((uint32_t)(arena + 1024), held - 1024u);
    }
    case 3:  return fp_stride((uint32_t)pal_bg_mem, 15u * 16u * 2u);  /* banks 0..14 */
    default: return 1u;
  }
  (void)g;
}

void mgfx_fp_capture(const MapGfx* g, const MapRender* mr, const uint8_t* arena) {
  if (!g || !mr) { s_fp_valid = false; return; }
  for (int i = 0; i < MGFX_FP_SLICES; i++) s_fp[i] = fp_slice(g, mr, arena, i);
  s_fp_valid = true;
}

uint32_t mgfx_fp_check(const MapGfx* g, const MapRender* mr, const uint8_t* arena, int slice) {
  uint32_t bad = 0;
  /* The canary is checked EVERY call: 44 cycles, and it covers the nastiest class. The
   * Mode-3 framebuffer spans 0x06000000..0x06012BFF — CBB0..CBB3, every screenblock, and
   * the first 11,264 bytes of the metatile tables (ids 0..703 of 1024). Any unguarded
   * Mode-3 excursion silently rewrites 69% of the tables with UI pixels. That has already
   * caused two shipped defects. */
  const volatile uint32_t* c = (const volatile uint32_t*)CANARY_ADDR;
  if (c[0] != CANARY_W0 || c[1] != ~CANARY_W0 || c[2] != CANARY_W0 || c[3] != ~CANARY_W0)
    bad |= MGFX_FP_CANARY;
  if (s_fp_valid) {
    int i = slice & (MGFX_FP_SLICES - 1);
    if (s_fp[i] && fp_slice(g, mr, arena, i) != s_fp[i])
      bad |= (uint32_t)(MGFX_FP_MT << i);
  }
  return bad;
}

const char* mgfx_fp_name(uint32_t bits) {
  if (bits & MGFX_FP_CANARY) return "canary";
  if (bits & MGFX_FP_MT)     return "metatiles";
  if (bits & MGFX_FP_CHAR)   return "tiles";
  if (bits & MGFX_FP_ARENA)  return "arena";
  if (bits & MGFX_FP_PAL)    return "palette";
  return "?";
}
