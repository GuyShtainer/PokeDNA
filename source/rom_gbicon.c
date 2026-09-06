/* Gen-2 party/PC menu icons out of the user's own Game Boy cartridge — see
 * rom_gbicon.h for the design, the shape invariants and the addresses this
 * lands on. Pure C: no tonc, no FatFs, no GBA headers. */
#include "rom_gbicon.h"

#include <string.h>

#define GB_BANK    0x4000u
#define GB_WIN_LO  0x4000u
#define GB_WIN_HI  0x8000u

/* The same 48-byte boot-logo fingerprint rom_gbsprite.c's parse_header() already
 * established (measured identically on Red, Yellow, Gold and Crystal) — reused
 * literally, not re-derived, since it is OUR OWN prior measurement of a fact
 * every licensed cartridge shares, not decomp data. */
#define GB_LOGO_OFF  0x104u
#define GB_LOGO_LEN  0x30u
#define GB_LOGO_FNV  0x016BAD3Fu

static uint32_t fnv1a(const uint8_t* p, uint32_t n, uint32_t h) {
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static int rd(const RomGbIcon* gi, uint32_t off, void* dst, uint32_t len) {
  if (!gi->read || len == 0) return 0;
  if (off >= gi->size || len > gi->size - off) return 0;
  return gi->read(gi->ctx, off, dst, len) ? 1 : 0;
}

/* ------------------------------------------------------------ header / identity */

static int parse_header(RomGbIcon* gi) {
  uint8_t h[0x50];
  if (!rd(gi, 0x100, h, sizeof h)) return 0;
  if (fnv1a(h + (GB_LOGO_OFF - 0x100), GB_LOGO_LEN, 0x811C9DC5u) != GB_LOGO_FNV) return 0;

  uint8_t x = 0;
  for (uint32_t i = 0x134; i <= 0x14C; i++) x = (uint8_t)(x - h[i - 0x100] - 1u);
  if (x != h[0x14D - 0x100]) return 0;

  uint8_t code = h[0x148 - 0x100];
  if (code > 8) return 0;
  if (gi->size != (0x8000u << code)) return 0;
  if (gi->size % GB_BANK) return 0;

  gi->id_hash = fnv1a(h, sizeof h, 0x811C9DC5u);
  return 1;
}

/* ---------------------------------------------------------------- pixel decode */

/* One raw 64-byte 2bpp frame -> 256 packed 0..3 indices, row-major 16x16. Tile
 * order is rgbgfx's default 2-wide x 2-tall layout (top-left, top-right,
 * bottom-left, bottom-right) — confirmed, during this module's development
 * only, pixel-for-pixel against pokecrystal's own published bulbasaur.png; never
 * embedded or shipped. Shared by the open()-time sanity check and by the public
 * rom_gbicon_to_rgb15(). */
static void tiles_to_px(const uint8_t t[ROM_GBICON_FRAME_BYTES], uint8_t px[ROM_GBICON_PX]) {
  static const int tx[4] = { 0, 8, 0, 8 };
  static const int ty[4] = { 0, 0, 8, 8 };
  for (int tile = 0; tile < 4; tile++) {
    const uint8_t* tb = t + (unsigned)tile * ROM_GBICON_TILE_BYTES;
    for (int r = 0; r < 8; r++) {
      uint8_t lo = tb[r * 2], hi = tb[r * 2 + 1];
      for (int c = 0; c < 8; c++) {
        int bit = 7 - c;
        uint8_t v = (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
        px[(ty[tile] + r) * ROM_GBICON_W + (tx[tile] + c)] = v;
      }
    }
  }
}

/* ------------------------------------------------------------------- scanning */

/*
 * One pass over the whole file, offering a `look`-byte window at every offset to
 * `cb`. Mirrors rom_gbsprite.c's own scan_walk idiom (independent
 * implementation — this module owns no static state to share with it). Returns
 * the number of hits (capped reporting at 2, since every caller here only cares
 * about "exactly one"), and writes the first hit's offset to *out_off.
 */
static uint32_t scan_one(RomGbIcon* gi, int (*cb)(const uint8_t*), uint32_t look,
                         uint8_t* scratch, uint32_t scratch_len, uint32_t* out_off) {
  uint32_t cap = scratch_len;
  if (!scratch || cap < look + 16u || gi->size < look) return 0;
  uint32_t step = cap - look + 1u;
  uint32_t hits = 0;
  for (uint32_t base = 0; base + look <= gi->size; base += step) {
    uint32_t n = gi->size - base; if (n > cap) n = cap;
    if (!rd(gi, base, scratch, n)) return 0;
    uint32_t lim = n - look;
    for (uint32_t i = 0; i <= lim; i++) {
      if (!cb(scratch + i)) continue;
      hits++;
      if (hits == 1) *out_off = base + i;
      if (hits > 1) return hits;      /* ambiguous — no need to keep counting  */
    }
  }
  return hits;
}

/* MonMenuIcons: 251 bytes, every one in [1,63], plus ten structural invariants
 * spanning the first 51 species (three "whole evolution line shares one icon"
 * triads, four sharing pairs, one "the next evolution differs" break, and a
 * minimum-distinct-values floor) and a minimum-distinct-values floor. Any FOUR
 * of these already collapse a 2 MB ROM to a handful of coincidental hits (see
 * rom_gbicon.h); using all ten measured a UNIQUE hit in both of Guy's Gen-2
 * dumps and NONE in either Gen-1 one, with no need to cross-check against the
 * pointer table at all -- important for hardware cost: this locator is exactly
 * ONE streamed pass over the ROM, not one pass per shape candidate. */
static int menu_icons_cb(const uint8_t* w) {
  uint8_t seen[64]; memset(seen, 0, sizeof seen);
  uint32_t distinct = 0;
  for (uint32_t i = 0; i < ROM_GBICON_SPECIES; i++) {
    uint8_t b = w[i];
    if (b == 0 || b > ROM_GBICON_MAX_KINDS) return 0;
    if (!seen[b]) { seen[b] = 1; distinct++; }
  }
  if (distinct < 8) return 0;
  if (!(w[0] == w[1] && w[1] == w[2])) return 0;      /* Bulbasaur/Ivysaur/Venusaur */
  if (w[3] != w[4]) return 0;                         /* Charmander/Charmeleon      */
  if (w[3] == w[5]) return 0;                         /* Charizard's kind differs   */
  if (!(w[6] == w[7] && w[7] == w[8])) return 0;       /* Squirtle/Wartortle/Blastoise */
  if (w[9] != w[10]) return 0;                        /* Caterpie/Metapod           */
  if (w[10] == w[11]) return 0;                       /* Butterfree's kind differs  */
  if (!(w[15] == w[16] && w[16] == w[17])) return 0;   /* Pidgey/Pidgeotto/Pidgeot   */
  if (w[22] != w[23]) return 0;                       /* Ekans/Arbok                */
  if (w[26] != w[27]) return 0;                       /* Sandshrew/Sandslash        */
  if (!(w[42] == w[43] && w[43] == w[44])) return 0;   /* Oddish/Gloom/Vileplume     */
  if (w[49] != w[50]) return 0;                       /* Diglett/Dugtrio            */
  return 1;
}

static uint8_t window_max(const uint8_t* w, uint32_t n) {
  uint8_t mx = 0;
  for (uint32_t i = 0; i < n; i++) if (w[i] > mx) mx = w[i];
  return mx;
}

/* IconPointers: N+1 little-endian u16 values, entry[0]==entry[1] and entry[i+1]
 * == entry[i]+128 for i=1..N-1 (successive 128-byte INCBIN'd icons, no padding),
 * REQUIRED MAXIMAL — the run must not extend one entry further, which is what
 * pins the match to exactly N rather than accepting a shorter prefix of a longer
 * coincidental run. `n_kinds` is a file-local (not header-exposed) closure value
 * — C has no closures, so it is threaded through a tiny fixed struct the
 * callback reads via a module-global set immediately before the scan (single-
 * threaded, scan-only, cleared right after; see rom_gbicon_open()). */
static uint8_t s_scan_n;   /* NOLINT: see comment above — open()'s own call frame owns this */

static int icon_ptrs_cb(const uint8_t* w) {
  uint32_t n = s_scan_n;
  uint16_t v0 = rd16(w), v1 = rd16(w + 2);
  if (v0 != v1) return 0;
  if (v0 < GB_WIN_LO || v0 >= GB_WIN_HI) return 0;
  uint16_t prev = v1;
  for (uint32_t i = 1; i < n; i++) {
    uint16_t cur = rd16(w + (i + 1) * 2);
    if ((uint16_t)(prev + 128u) != cur) return 0;
    prev = cur;
  }
  /* maximality: the (n+1)-th entry beyond the required run must NOT continue it */
  uint16_t ext = rd16(w + (n + 1) * 2);
  if ((uint16_t)(prev + 128u) == ext) return 0;
  return 1;
}

/* ------------------------------------------------------------------- open() */

static int locate(RomGbIcon* gi, uint8_t* scratch, uint32_t scratch_len) {
  uint32_t menu_off = 0;
  if (scan_one(gi, menu_icons_cb, ROM_GBICON_SPECIES, scratch, scratch_len, &menu_off) != 1)
    return 0;

  uint8_t window[ROM_GBICON_SPECIES];
  if (!rd(gi, menu_off, window, sizeof window)) return 0;
  uint8_t n = window_max(window, sizeof window);
  if (n < 8 || n > ROM_GBICON_MAX_KINDS) return 0;

  /* icon_ptrs_cb needs (n+2) entries of look-ahead to prove maximality. */
  s_scan_n = n;
  uint32_t ptr_off = 0;
  uint32_t look = ((uint32_t)n + 2u) * 2u;
  uint32_t hits = scan_one(gi, icon_ptrs_cb, look, scratch, scratch_len, &ptr_off);
  s_scan_n = 0;
  if (hits != 1) return 0;

  uint8_t icon_bank = (uint8_t)(ptr_off / GB_BANK);
  if (icon_bank == 0) return 0;             /* bank 0 never holds swappable data */

  /* Fail-closed sanity net (WEAK on its own, see rom_gbicon.h — the real proof
   * is the structural derivation above): every one of the n kinds must decode,
   * through the derived bank, to a non-degenerate frame 0. */
  for (uint32_t k = 1; k <= n; k++) {
    uint8_t raw[2];
    if (!rd(gi, ptr_off + k * 2u, raw, 2)) return 0;
    uint16_t v = rd16(raw);
    if (v < GB_WIN_LO || v >= GB_WIN_HI) return 0;
    uint32_t foff = (uint32_t)icon_bank * GB_BANK + (uint32_t)(v - GB_WIN_LO);
    uint8_t tile[ROM_GBICON_FRAME_BYTES];
    if (!rd(gi, foff, tile, sizeof tile)) return 0;
    uint8_t px[ROM_GBICON_PX];
    tiles_to_px(tile, px);
    uint8_t first = px[0]; int mixed = 0;
    for (int i = 1; i < ROM_GBICON_PX; i++) if (px[i] != first) { mixed = 1; break; }
    if (!mixed) return 0;
  }

  gi->mon_menu_icons = menu_off;
  gi->icon_pointers = ptr_off;
  gi->n = n;
  gi->icon_bank = icon_bank;
  return 1;
}

int rom_gbicon_open(RomGbIcon* gi, GbReadFn read, void* ctx, uint32_t size,
                    uint8_t* scratch, uint32_t scratch_len) {
  if (!gi) return 0;
  memset(gi, 0, sizeof *gi);
  gi->read = read; gi->ctx = ctx; gi->size = size;
  if (!read || !scratch || scratch_len < ROM_GBICON_SCRATCH_MIN) return 0;
  if (!parse_header(gi)) return 0;
  if (!locate(gi, scratch, scratch_len)) return 0;
  gi->ok = 1;
  return 1;
}

void rom_gbicon_save_loc(const RomGbIcon* gi, RomGbIconLoc* out) {
  if (!gi || !out) return;
  memset(out, 0, sizeof *out);
  out->id_hash = gi->id_hash;
  out->size = gi->size;
  out->mon_menu_icons = gi->mon_menu_icons;
  out->icon_pointers = gi->icon_pointers;
  out->n = gi->n;
  out->icon_bank = gi->icon_bank;
}

/* Re-validate a cached loc with a HANDFUL of reads: the header (id_hash + size),
 * then the SAME per-kind sanity net open()'s full scan already runs — cheap (n
 * <= 63 reads of 64 B) and it is what catches a `.gbc` that was swapped out from
 * under an unchanged file size/header (astronomically unlikely, but the full
 * scan's own sanity net cost this little, so there is no reason to skip it on
 * the cached path and trust the cache blindly). Falls back to a full
 * rom_gbicon_open() scan whenever the cache does not check out. */
int rom_gbicon_open_loc(RomGbIcon* gi, GbReadFn read, void* ctx, uint32_t size,
                        uint8_t* scratch, uint32_t scratch_len,
                        const RomGbIconLoc* loc) {
  if (!gi) return 0;
  memset(gi, 0, sizeof *gi);
  gi->read = read; gi->ctx = ctx; gi->size = size;
  if (!read) return 0;
  if (!parse_header(gi)) return 0;

  if (loc && loc->id_hash == gi->id_hash && loc->size == size &&
      loc->n >= 8 && loc->n <= ROM_GBICON_MAX_KINDS && loc->icon_bank != 0) {
    int ok = 1;
    for (uint32_t k = 1; k <= loc->n && ok; k++) {
      uint8_t raw[2];
      if (!rd(gi, loc->icon_pointers + k * 2u, raw, 2)) { ok = 0; break; }
      uint16_t v = rd16(raw);
      if (v < GB_WIN_LO || v >= GB_WIN_HI) { ok = 0; break; }
      uint32_t foff = (uint32_t)loc->icon_bank * GB_BANK + (uint32_t)(v - GB_WIN_LO);
      uint8_t tile[ROM_GBICON_FRAME_BYTES];
      if (!rd(gi, foff, tile, sizeof tile)) { ok = 0; break; }
      uint8_t px[ROM_GBICON_PX];
      tiles_to_px(tile, px);
      uint8_t first = px[0]; int mixed = 0;
      for (int i = 1; i < ROM_GBICON_PX; i++) if (px[i] != first) { mixed = 1; break; }
      if (!mixed) { ok = 0; break; }
    }
    if (ok) {
      gi->mon_menu_icons = loc->mon_menu_icons;
      gi->icon_pointers = loc->icon_pointers;
      gi->n = loc->n;
      gi->icon_bank = loc->icon_bank;
      gi->ok = 1;
      return 1;
    }
  }
  if (!scratch || scratch_len < ROM_GBICON_SCRATCH_MIN) return 0;
  if (!locate(gi, scratch, scratch_len)) return 0;
  gi->ok = 1;
  return 1;
}

/* -------------------------------------------------------------- accessors */

int rom_gbicon_kind(const RomGbIcon* gi, uint16_t dex) {
  if (!gi || !gi->ok || dex < 1 || dex > ROM_GBICON_SPECIES) return 0;
  uint8_t b;
  if (!rd(gi, gi->mon_menu_icons + (uint32_t)(dex - 1), &b, 1)) return 0;
  if (b == 0 || b > gi->n) return 0;
  return b;
}

int rom_gbicon_tiles(RomGbIcon* gi, int kind, int frame, uint8_t out[ROM_GBICON_FRAME_BYTES]) {
  if (!gi || !gi->ok || !out) return 0;
  if (kind < 1 || (uint32_t)kind > gi->n) return 0;
  if (frame != 0 && frame != 1) return 0;
  uint8_t raw[2];
  if (!rd(gi, gi->icon_pointers + (uint32_t)kind * 2u, raw, 2)) return 0;
  uint16_t v = rd16(raw);
  if (v < GB_WIN_LO || v >= GB_WIN_HI) return 0;
  uint32_t foff = (uint32_t)gi->icon_bank * GB_BANK + (uint32_t)(v - GB_WIN_LO)
                + (uint32_t)frame * ROM_GBICON_FRAME_BYTES;
  return rd(gi, foff, out, ROM_GBICON_FRAME_BYTES);
}

/* D2 (E5 fix): there is no per-icon palette table -- Gen 2 colours every menu icon
 * with the FIXED party-menu OBJ palette 0 (PartyMenuOBPals, gfx/stats/party_menu_ob.pal
 * in both pokegold and pokecrystal, byte-identical): idx0 RGB(27,31,27) transparent,
 * idx1 RGB(31,19,10) light orange, idx2 RGB(31,7,4) red, idx3 RGB(0,0,0) black. These
 * are GBC 5-bit-per-channel values (0..31, the palette-RAM format), packed the same
 * way rom_gbicon_to_rgb15's caller expects: r | g<<5 | b<<10. This is NOT the DMG
 * monochrome ramp -- Gen 2's party-menu icons are colour on GBC hardware (and on the
 * SGB); the DMG grey ramp (rom_gbsprite.h's G1_GREY0..3) belongs to Gen 1's back/front
 * sprites, a completely different asset with its own (monochrome, DMG-native)
 * palette. See rom_gbicon.h's own palette note. */
static const uint16_t GB_ICON_PAL[4] = { 0x6FFBu, 0x2A7Fu, 0x10FFu, 0x0000u };

int rom_gbicon_pal(const RomGbIcon* gi, int kind, uint16_t out[4]) {
  if (!gi || !gi->ok || !out) return 0;
  if (kind < 1 || (uint32_t)kind > gi->n) return 0;
  memcpy(out, GB_ICON_PAL, sizeof GB_ICON_PAL);
  return 1;
}

int rom_gbicon_to_rgb15(const uint8_t tiles[ROM_GBICON_FRAME_BYTES],
                        const uint16_t pal[4], uint16_t* dst) {
  if (!tiles || !pal || !dst) return 0;
  uint8_t px[ROM_GBICON_PX];
  tiles_to_px(tiles, px);
  for (int i = 0; i < ROM_GBICON_PX; i++) {
    uint8_t v = px[i];
    if (v > 3) return 0;
    dst[i] = v ? (uint16_t)(0x8000u | (pal[v] & 0x7FFFu)) : 0u;
  }
  return 1;
}
