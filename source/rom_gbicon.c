/* Gen-2 party/PC menu icons out of the user's own Game Boy cartridge — see
 * rom_gbicon.h for the design, the shape invariants and the addresses this
 * lands on. Pure C: no tonc, no FatFs, no GBA headers. */
#include "rom_gbicon.h"
#include "gb_scanwin.h"

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

  /* BACKLOG #201 F2: the same title/version/global_checksum extraction
   * rom_gbsprite.c's own parse_header() does, byte-identical fields (this is
   * the same 0x100..0x14F header) -- the identity a known-ROM table looks up
   * by. Never used to locate anything; locate() below is unchanged, shape-only. */
  memcpy(gi->title, h + (0x134 - 0x100), 15);
  gi->title[15] = 0;
  for (int i = 0; i < 15; i++) {
    uint8_t c = (uint8_t)gi->title[i];
    if (c && (c < 0x20 || c > 0x7E)) { gi->title[i] = 0; break; }
  }
  gi->version         = h[0x14C - 0x100];
  gi->global_checksum = (uint16_t)((h[0x14E - 0x100] << 8) | h[0x14F - 0x100]);

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

/* BACKLOG #201 F1: which inline gate scan_one() runs before calling its (indirect,
 * un-inlinable) callback. Each gate is a STRICT PREFIX of its own callback's own
 * first real check, verified against the two real callbacks below (never a new,
 * independent condition), so it can never reject a position the callback itself
 * would have accepted -- same contract as rom_gbsprite.c's ScanJob gate. */
enum { GB_ICON_GATE_MENU = 0, GB_ICON_GATE_PTR = 1 };

#ifdef ROM_GBICON_JOB_COUNTERS
/* BACKLOG #201 Step 1: per-callback (post-gate) invocation counts, mirroring
 * rom_gbsprite.c's ROM_GBSPRITE_JOB_COUNTERS -- never defined by the GBA build
 * (the Makefile never passes this macro); only a host test binary sees it. */
uint32_t g_rgi_cb_calls[2];
#endif

/*
 * One pass over the whole file, offering a `look`-byte window at every offset to
 * `cb`. Mirrors rom_gbsprite.c's own scan_walk idiom (independent
 * implementation — this module owns no static state to share with it). Returns
 * the number of hits (capped reporting at 2, since every caller here only cares
 * about "exactly one"), and writes the first hit's offset to *out_off.
 *
 * BACKLOG #201 F1: `gate_kind` picks an inline, un-indirected prefilter tested
 * BEFORE `cb` -- see rom_gbsprite.c scan_multi's own ScanJob comment for the
 * measured reason a function-pointer call cannot be inlined across, so a cheap
 * byte compare ahead of it is worth far more here than inside the callback.
 */
static uint32_t scan_one(RomGbIcon* gi, int (*cb)(const uint8_t*), uint32_t look,
                         uint8_t* scratch, uint32_t scratch_len, uint32_t* out_off,
                         int gate_kind) {
  /* Forward-only, sector-aligned reads (gb_scanwin.h) -- the same window rule as
   * rom_gbsprite.c's scan_multi, for the same measured reason. */
  GbScanWin sw;
  if (!scratch || !gb_scanwin_init(&sw, gi->size, scratch_len, look)) return 0;
  uint32_t hits = 0;
  while (gb_scanwin_plan(&sw, scratch)) {
    if (!rd(gi, sw.rd_off, scratch + sw.rd_dst, sw.rd_len)) return 0;
    uint32_t cnt = gb_scanwin_filled(&sw);
    for (uint32_t i = sw.first; i < sw.first + cnt; i++) {
      const uint8_t* p = scratch + i;
      if (gate_kind == GB_ICON_GATE_MENU) {
        /* menu_icons_cb's own w[0]==w[1]==w[2] triad (Bulbasaur/Ivysaur/Venusaur)
         * plus the per-byte [1,63] range every one of its 251 bytes must pass --
         * cheapest to check on w[0] alone here, before the full 251-byte body. */
        if (!(p[0] == p[1] && p[1] == p[2] && p[0] >= 1 && p[0] <= ROM_GBICON_MAX_KINDS))
          continue;
      } else {
        /* icon_ptrs_cb's own first check: entry[0] == entry[1]. */
        if (rd16(p) != rd16(p + 2)) continue;
      }
#ifdef ROM_GBICON_JOB_COUNTERS
      g_rgi_cb_calls[gate_kind]++;
#endif
      if (!cb(p)) continue;
      hits++;
      if (hits == 1) *out_off = sw.base + i;
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

static int locate(RomGbIcon* gi, uint8_t* scratch, uint32_t scratch_len,
                  RomGbIconPassFn pass2_cb, void* pass2_ctx) {
  uint32_t menu_off = 0;
  if (scan_one(gi, menu_icons_cb, ROM_GBICON_SPECIES, scratch, scratch_len, &menu_off,
              GB_ICON_GATE_MENU) != 1)
    return 0;

  uint8_t window[ROM_GBICON_SPECIES];
  if (!rd(gi, menu_off, window, sizeof window)) return 0;
  uint8_t n = window_max(window, sizeof window);
  if (n < 8 || n > ROM_GBICON_MAX_KINDS) return 0;

  /* BACKLOG #201 F3: pass 1 (MonMenuIcons) is done -- one callback, before pass 2
   * (IconPointers) starts its OWN whole-ROM scan, so the caller can reset its own
   * progress bookkeeping and report pass 2's own done/total instead of inheriting
   * pass 1's already-maxed high-water mark. */
  if (pass2_cb) pass2_cb(pass2_ctx);

  /* icon_ptrs_cb needs (n+2) entries of look-ahead to prove maximality. */
  s_scan_n = n;
  uint32_t ptr_off = 0;
  uint32_t look = ((uint32_t)n + 2u) * 2u;
  uint32_t hits = scan_one(gi, icon_ptrs_cb, look, scratch, scratch_len, &ptr_off,
                           GB_ICON_GATE_PTR);
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

/* BACKLOG #201 F2 (factored out of rom_gbicon_open_loc()'s own body, D3's fix
 * unchanged in meaning): re-validate a CANDIDATE RomGbIconLoc -- from the .loc
 * cache file (open_loc()'s caller) or the known-ROM fast-path table below
 * (open()) -- with a HANDFUL of reads: id_hash/size, then the SAME shape check
 * locate() itself runs (menu_icons_cb's ten structural invariants plus
 * window_max() matching the candidate's own n exactly), then the per-kind
 * decode sanity net. Returns 1 (gi fully populated, gi->ok=1) or 0 (gi is
 * UNCHANGED beyond whatever a rejected candidate's own verify reads already
 * touched -- the caller must fall through to a full scan). Same contract as
 * rom_gbsprite.c's own try_loc(): a wrong, stale or foreign candidate degrades
 * to "scan for real", never a wrong picture. */
static int try_loc(RomGbIcon* gi, const RomGbIconLoc* loc) {
  if (!loc) return 0;
  if (!(loc->id_hash == gi->id_hash && loc->size == gi->size &&
        loc->n >= 8 && loc->n <= ROM_GBICON_MAX_KINDS && loc->icon_bank != 0))
    return 0;

  uint8_t menu_window[ROM_GBICON_SPECIES];
  if (!rd(gi, loc->mon_menu_icons, menu_window, sizeof menu_window)) return 0;
  if (!menu_icons_cb(menu_window)) return 0;
  if (window_max(menu_window, sizeof menu_window) != loc->n) return 0;

  for (uint32_t k = 1; k <= loc->n; k++) {
    uint8_t raw[2];
    if (!rd(gi, loc->icon_pointers + k * 2u, raw, 2)) return 0;
    uint16_t v = rd16(raw);
    if (v < GB_WIN_LO || v >= GB_WIN_HI) return 0;
    uint32_t foff = (uint32_t)loc->icon_bank * GB_BANK + (uint32_t)(v - GB_WIN_LO);
    uint8_t tile[ROM_GBICON_FRAME_BYTES];
    if (!rd(gi, foff, tile, sizeof tile)) return 0;
    uint8_t px[ROM_GBICON_PX];
    tiles_to_px(tile, px);
    uint8_t first = px[0]; int mixed = 0;
    for (int i = 1; i < ROM_GBICON_PX; i++) if (px[i] != first) { mixed = 1; break; }
    if (!mixed) return 0;
  }

  gi->mon_menu_icons = loc->mon_menu_icons;
  gi->icon_pointers  = loc->icon_pointers;
  gi->n              = loc->n;
  gi->icon_bank      = loc->icon_bank;
  gi->ok = 1;
  return 1;
}

/* BACKLOG #201 F2: a static const table of (title, version, global_checksum) ->
 * the located offsets, populated ONLY from what THIS scanner finds on Guy's own
 * two Gen-2 corpus ROMs (tests/host_gbicon_test.c's part_f2_known_table(),
 * BACKLOG #201, re-derives every entry from the live scanner on every run and
 * asserts byte-equality, so the table can never drift from it -- see that
 * function for the generator this table was pasted from). No Silver entry (no
 * corpus dump exists): it, and any hack/unknown revision, fall straight through
 * to the full scan. A HIT here still runs the exact same try_loc() gate a .loc
 * cache hit gets -- this table is a candidate, never a trusted source. */
typedef struct {
  char          title[16];
  uint8_t       version;
  uint16_t      global_checksum;
  RomGbIconLoc  loc;
} RomGbIconKnown;

#include "rom_gbicon_known.h"

static const RomGbIconLoc* known_icon_lookup(const char* title, uint8_t version,
                                             uint16_t global_checksum) {
  for (uint32_t i = 0; i < sizeof k_known_gbicon / sizeof k_known_gbicon[0]; i++) {
    const RomGbIconKnown* k = &k_known_gbicon[i];
    if (k->version == version && k->global_checksum == global_checksum &&
        memcmp(k->title, title, sizeof k->title) == 0)
      return &k->loc;
  }
  return 0;
}

int rom_gbicon_open(RomGbIcon* gi, GbReadFn read, void* ctx, uint32_t size,
                    uint8_t* scratch, uint32_t scratch_len,
                    RomGbIconPassFn pass2_cb, void* pass2_ctx) {
  if (!gi) return 0;
  memset(gi, 0, sizeof *gi);
  gi->read = read; gi->ctx = ctx; gi->size = size;
  if (!read || !scratch || scratch_len < ROM_GBICON_SCRATCH_MIN) return 0;
  if (!parse_header(gi)) return 0;
  /* BACKLOG #201 F2: a known ROM's own table entry, verified before use, skips
   * the whole-ROM scan entirely -- checked before locate() so a hit costs only
   * the handful of try_loc() reads, not one scan byte. */
  if (try_loc(gi, known_icon_lookup(gi->title, gi->version, gi->global_checksum)))
    return 1;
  if (!locate(gi, scratch, scratch_len, pass2_cb, pass2_ctx)) return 0;
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
                        const RomGbIconLoc* loc,
                        RomGbIconPassFn pass2_cb, void* pass2_ctx) {
  if (!gi) return 0;
  memset(gi, 0, sizeof *gi);
  gi->read = read; gi->ctx = ctx; gi->size = size;
  if (!read) return 0;
  if (!parse_header(gi)) return 0;

  /* D3 (E5 fix, adversarial review) unchanged in meaning, now shared with F2's
   * known-ROM table via try_loc() above: re-checks loc->mon_menu_icons too, not
   * just loc->icon_pointers -- a stale/tampered mon_menu_icons offset (same
   * id_hash/size) must not sail through just because icon_pointers/icon_bank
   * still check out, or every species would silently get the wrong icon KIND. */
  if (try_loc(gi, loc)) return 1;

  if (!scratch || scratch_len < ROM_GBICON_SCRATCH_MIN) return 0;
  if (!locate(gi, scratch, scratch_len, pass2_cb, pass2_ctx)) return 0;
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

/* D6 (E5 fix): ICON_EGG -- pokecrystal/pokegold's fixed egg-icon kind, 28. Present in
 * the ROM (a real, decodable 16x16 picture at kind 28) but used by no species in
 * MonMenuIcons -- it is drawn only when the game code explicitly asks for kind 28
 * itself (an egg party-menu/box slot), never reached through rom_gbicon_kind()'s
 * per-species table lookup. Same fail-closed posture as every other accessor here:
 * 28 > n on a variant with fewer kinds (there is no such variant among Guy's own
 * dumps, both measuring n=38, but nothing here assumes that) refuses rather than
 * serving a kind this ROM's own table never proved decodes to anything. */
int rom_gbicon_kind_egg(const RomGbIcon* gi) {
  if (!gi || !gi->ok) return 0;
  return (ROM_GBICON_KIND_EGG <= gi->n) ? ROM_GBICON_KIND_EGG : 0;
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
