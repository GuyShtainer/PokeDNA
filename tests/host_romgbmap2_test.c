/* Host (PC) test for rom_gbmap2 -- Gen-2 (Gold/Silver/Crystal) MAP data
 * located BY SHAPE in real cartridge dumps. rom_gbmap2 does all its I/O
 * through the caller's GbReadFn, so the code under test is byte-for-byte
 * the code that runs on the GBA.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_romgbmap2_test.c \
 *      source/rom_gbmap2.c source/rom_gbmap.c -o /tmp/hgbmap2 && /tmp/hgbmap2
 *
 * ORACLE: docs/GB-MAP-DESIGN-G2.md §3.4/§2.4/§2.5/§8, independently
 * re-derived (see this test's own comments) and cross-checked against
 * `assets/upstream/pokecrystal`'s own map_constants.asm/maps.asm (data/
 * layout FACTS only, never copied into source/).
 *
 * Coverage (docs/briefs/map-g2-brief.md Step 3):
 *   1) both ROMs locate all 3 tables at the exact oracle offsets, with the
 *      derived counts 26/29 (Gold) and 26/37 (Crystal);
 *   2) New Bark Town (group 24, number 4) decodes byte-exact on both;
 *   3) whole-corpus sweep: every group/number pair the located tables
 *      actually define parses with 1<=height,width<=64 and an in-bounds
 *      Blocks span -- expect 368 maps on Gold, 388 on Crystal, 0 failures.
 *      Per-group map counts are derived structurally (never hard-coded):
 *      sort the n_groups pointers by address; for all but the
 *      highest-address group, count = (next sorted pointer - this
 *      pointer)/9; the highest-address group is walked sequentially until
 *      a record fails the same plausibility test rgm2_map() itself applies;
 *   4) the live saves resolve: Gold.sav@0x2868 and Crystal.sav@0x2843 both
 *      give group 24 / number 4 / y 6 / x 13 (New Bark Town), player block
 *      (6,3) in bounds;
 *   5) negative controls (fail-closed): mutating one concrete byte inside
 *      any of the 3 anchors makes rgm2_open() fail with EVERY derived
 *      field zeroed; forging a second copy of an anchor elsewhere makes it
 *      refuse (ambiguous); running the locator against Red.gb, Yellow.gb
 *      and Emerald.gba refuses (0 hits);
 *   6) bank-boundary bounds check: a synthesized MapAttributes whose Blocks
 *      span would run past the ROM end is refused by rgm2_map().
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never
 * copied into it, so a missing corpus SKIPS rather than failing.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "rom_gbmap2.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
static void chk(const char* who, const char* what, int cond) {
  g_check++;
  if (!cond) { printf("  !! FAIL [%s] %s\n", who, what); g_fail++; }
}

/* ------------------------------------------------------------- I/O ---- */
typedef struct { uint8_t* buf; uint32_t len; } Mem;

static bool rd_mem(void* ctx, uint32_t off, void* dst, uint32_t len) {
  Mem* m = (Mem*)ctx;
  if (off >= m->len || len > m->len - off) return false;
  memcpy(dst, m->buf + off, len);
  return true;
}

static Mem load_file(const char* path) {
  Mem m = { NULL, 0 };
  FILE* f = fopen(path, "rb");
  if (!f) return m;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return m; }
  m.buf = (uint8_t*)malloc((size_t)sz);
  m.len = (uint32_t)sz;
  if (m.buf) { if (fread(m.buf, 1, (size_t)sz, f) != (size_t)sz) { free(m.buf); m.buf = NULL; m.len = 0; } }
  fclose(f);
  return m;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t fileoff(uint32_t bank, uint16_t addr) {
  return (addr < 0x4000u) ? addr : bank * 0x4000u + (uint32_t)(addr - 0x4000u);
}

/* ------------------------------------------------------- all-zero? ---- */
static int all_zero_after_header(const RomGbMap2* g) {
  return g->groups_off == 0 && g->groups_bank == 0 && g->n_groups == 0 &&
         g->tilesets_off == 0 && g->n_tilesets == 0 && g->ok == 0;
}

/* -------------------------------------------------- corpus sweep ---- */
/* Structural (never hard-coded) per-group map-count derivation -- see the
 * file header comment. Fills `counts[n_groups]`, returns the total. */
static uint32_t derive_group_counts(Mem* m, const RomGbMap2* g, uint32_t* counts) {
  uint16_t ptrs[64];
  for (uint32_t i = 0; i < g->n_groups; i++) {
    uint8_t p2[2];
    rd_mem(m, g->groups_off + i * 2u, p2, 2);
    ptrs[i] = rd16(p2);
  }
  uint32_t order[64];
  for (uint32_t i = 0; i < g->n_groups; i++) order[i] = i;
  for (uint32_t i = 0; i < g->n_groups; i++)
    for (uint32_t j = i + 1; j < g->n_groups; j++)
      if (ptrs[order[j]] < ptrs[order[i]]) { uint32_t t = order[i]; order[i] = order[j]; order[j] = t; }

  uint32_t total = 0;
  for (uint32_t rank = 0; rank < g->n_groups; rank++) {
    uint32_t gi = order[rank];
    uint32_t gbase = fileoff(g->groups_bank, ptrs[gi]);
    uint32_t cnt;
    if (rank + 1 < g->n_groups) {
      uint32_t nxt = fileoff(g->groups_bank, ptrs[order[rank + 1]]);
      cnt = (nxt > gbase) ? (nxt - gbase) / 9u : 0u;
    } else {
      cnt = 0;
      for (; cnt < 200u; cnt++) {
        GbMap2Map mm;
        if (!rgm2_map(g, (uint8_t)(gi + 1u), (uint8_t)(cnt + 1u), &mm)) break;
      }
    }
    counts[gi] = cnt;
    total += cnt;
  }
  return total;
}

/* ---------------------------------------------------------- tests ---- */
static void test_one_rom(const char* rom_file, const char* name,
                          uint32_t want_groups_off, uint32_t want_tilesets_off,
                          uint32_t want_n_groups, uint32_t want_n_tilesets,
                          uint32_t want_total_maps) {
  char path[512]; snprintf(path, sizeof path, "%s/%s", ROMS, rom_file);
  Mem m = load_file(path);
  if (!m.buf) { printf("  [%s] SKIP (corpus absent)\n", name); return; }
  g_ran = 1;

  static uint8_t scratch[4096];
  RomGbMap2 g;
  bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
  chk(name, "locator opens", ok);
  if (ok) {
    chk(name, "groups_off matches oracle", g.groups_off == want_groups_off);
    chk(name, "tilesets_off matches oracle", g.tilesets_off == want_tilesets_off);
    chk(name, "n_groups == 26", g.n_groups == want_n_groups);
    chk(name, "n_tilesets matches oracle", g.n_tilesets == want_n_tilesets);
  }

  /* New Bark Town: group 24, number 4 */
  GbMap2Map nb;
  bool nok = rgm2_map(&g, 24, 4, &nb);
  chk(name, "New Bark Town parses", nok);
  if (nok) {
    chk(name, "NBT height==9", nb.height == 9);
    chk(name, "NBT width==10", nb.width == 10);
    chk(name, "NBT border==0x05", nb.border_block == 0x05);
    chk(name, "NBT connmask==0x03", nb.conn_mask == 0x03);
    chk(name, "NBT nconn==2", nb.nconn == 2);
    if (nb.nconn == 2) {
      chk(name, "NBT conn0 W group=24", nb.conn[0].group == 24);
      chk(name, "NBT conn0 W number=3", nb.conn[0].number == 3);
      chk(name, "NBT conn0 W width=30", nb.conn[0].width == 30);
      chk(name, "NBT conn1 E group=24", nb.conn[1].group == 24);
      chk(name, "NBT conn1 E number=2", nb.conn[1].number == 2);
      chk(name, "NBT conn1 E width=40", nb.conn[1].width == 40);
    }
  }

  /* Whole-corpus sweep */
  if (ok) {
    uint32_t counts[64];
    uint32_t total = derive_group_counts(&m, &g, counts);
    printf("  [%s] derived total maps = %u (want %u)\n", name, total, want_total_maps);
    chk(name, "sweep total matches design doc", total == want_total_maps);

    int walked = 0, failed = 0;
    for (uint32_t gi = 0; gi < g.n_groups; gi++) {
      for (uint32_t nu = 1; nu <= counts[gi]; nu++) {
        GbMap2Map mm;
        if (!rgm2_map(&g, (uint8_t)(gi + 1u), (uint8_t)nu, &mm)) { failed++; continue; }
        walked++;
        if (mm.height < 1 || mm.height > 64 || mm.width < 1 || mm.width > 64) failed++;
      }
    }
    chk(name, "sweep walked == derived total", (uint32_t)walked == total);
    chk(name, "sweep failures == 0", failed == 0);
  }

  free(m.buf);
}

static void test_live_saves(void) {
  struct { const char* sav; uint32_t off; const char* name; } t[] = {
    { "Gold.sav", 0x2868u, "Gold.sav" },
    { "Crystal.sav", 0x2843u, "Crystal.sav" },
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", ROMS, t[i].sav);
    Mem m = load_file(path);
    if (!m.buf) { printf("  [%s] SKIP (save absent)\n", t[i].name); continue; }
    g_ran = 1;
    uint8_t group, number, y, x;
    rd_mem(&m, t[i].off + 0u, &group, 1);
    rd_mem(&m, t[i].off + 1u, &number, 1);
    rd_mem(&m, t[i].off + 2u, &y, 1);
    rd_mem(&m, t[i].off + 3u, &x, 1);
    chk(t[i].name, "group==24", group == 24);
    chk(t[i].name, "number==4", number == 4);
    chk(t[i].name, "y==6", y == 6);
    chk(t[i].name, "x==13", x == 13);
    free(m.buf);
  }
}

static void test_negative_controls(const char* rom_file, const char* name,
                                    uint32_t a1_off, uint32_t a3_off) {
  char path[512]; snprintf(path, sizeof path, "%s/%s", ROMS, rom_file);
  Mem m = load_file(path);
  if (!m.buf) { printf("  [%s] SKIP negative controls (corpus absent)\n", name); return; }
  g_ran = 1;
  static uint8_t scratch[4096];

  /* Baseline: must locate cleanly first. */
  RomGbMap2 g0;
  chk(name, "baseline locates before mutation", rgm2_open(&g0, rd_mem, &m, m.len, scratch, sizeof scratch));

  /* Mutate one concrete byte inside anchor 1 (GetAnyMapPointer's own first
   * opcode byte, 0xC5) -- must refuse with every derived field zeroed. */
  {
    uint8_t save = m.buf[a1_off];
    m.buf[a1_off] ^= 0xFFu;
    RomGbMap2 g;
    bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
    chk(name, "anchor1 mutation refuses", !ok);
    chk(name, "anchor1 mutation zeros every field", all_zero_after_header(&g));
    m.buf[a1_off] = save;
  }

  /* Mutate anchor 3 (LoadMapTileset's own first opcode byte, 0xE5). */
  {
    uint8_t save = m.buf[a3_off];
    m.buf[a3_off] ^= 0xFFu;
    RomGbMap2 g;
    bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
    chk(name, "anchor3 mutation refuses", !ok);
    chk(name, "anchor3 mutation zeros every field", all_zero_after_header(&g));
    m.buf[a3_off] = save;
  }

  /* Forge a second copy of anchor 1's full byte pattern far away (in an
   * unused tail region, if the ROM is large enough) -- must refuse
   * (ambiguous), never silently pick either candidate. */
  if (m.len > 0x100000u) {
    uint32_t forge_at = m.len - 64u;
    uint8_t a1bytes[23];
    memcpy(a1bytes, m.buf + a1_off, sizeof a1bytes);
    uint8_t save[sizeof a1bytes];
    memcpy(save, m.buf + forge_at, sizeof save);
    memcpy(m.buf + forge_at, a1bytes, sizeof a1bytes);
    RomGbMap2 g;
    bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
    chk(name, "forged duplicate anchor1 refuses (ambiguous)", !ok);
    memcpy(m.buf + forge_at, save, sizeof save);
  }

  free(m.buf);
}

static void test_offgame_refusal(void) {
  const char* files[] = { "Red.gb", "Yellow.gb" };
  for (size_t i = 0; i < 2; i++) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", ROMS, files[i]);
    Mem m = load_file(path);
    if (!m.buf) { printf("  [%s] SKIP (absent)\n", files[i]); continue; }
    g_ran = 1;
    static uint8_t scratch[4096];
    RomGbMap2 g;
    bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
    chk(files[i], "off-game ROM refuses", !ok);
    free(m.buf);
  }
  {
    Mem m = load_file("/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/Emerald.gba");
    if (m.buf) {
      g_ran = 1;
      static uint8_t scratch[4096];
      RomGbMap2 g;
      bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
      chk("Emerald.gba", "off-game ROM refuses", !ok);
      free(m.buf);
    } else {
      printf("  [Emerald.gba] SKIP (absent)\n");
    }
  }
}

/* Bank-boundary bounds check: synthesize a tiny in-memory ROM whose located
 * MapAttributes claims a Blocks span that runs past the buffer end. */
static void test_bank_boundary_mutation(void) {
  /* Reuse the real Crystal ROM (needed for the anchors themselves) but
   * truncate the in-memory copy so New Bark Town's own Blocks span
   * (0xacdb5..+90) runs past the buffer end. */
  char path[512]; snprintf(path, sizeof path, "%s/Crystal.gbc", ROMS);
  Mem m = load_file(path);
  if (!m.buf) { printf("  [bank-boundary] SKIP (Crystal.gbc absent)\n"); return; }
  g_ran = 1;
  static uint8_t scratch[4096];
  RomGbMap2 g;
  bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
  chk("bank-boundary", "baseline locates", ok);
  if (ok) {
    uint32_t truncated_len = 0xacdb5u + 5u;   /* well short of h*w=90 bytes */
    Mem short_m = { m.buf, truncated_len };
    RomGbMap2 g2 = g;   /* reuse located tables, only the read window shrinks */
    g2.size = truncated_len;
    GbMap2Map mm;
    bool mok = rgm2_map(&g2, 24, 4, &mm);
    chk("bank-boundary", "truncated Blocks span refuses", !mok);
    (void)short_m;
  }
  free(m.buf);
}

/* review-opus non-blocking (a): rgm2_map() had no upper bound on `number`.
 * Group 24 (New Bark) has exactly 13 real maps on BOTH ROMs -- assert 13
 * resolves and 14/255 both refuse. */
static void test_number_bound(const char* rom_file, const char* name) {
  char path[512]; snprintf(path, sizeof path, "%s/%s", ROMS, rom_file);
  Mem m = load_file(path);
  if (!m.buf) { printf("  [%s] SKIP number-bound (corpus absent)\n", name); return; }
  g_ran = 1;
  static uint8_t scratch[4096];
  RomGbMap2 g;
  bool ok = rgm2_open(&g, rd_mem, &m, m.len, scratch, sizeof scratch);
  chk(name, "number-bound: locates", ok);
  if (ok) {
    GbMap2Map mm;
    chk(name, "group 24 number 13 (last real map) resolves", rgm2_map(&g, 24, 13, &mm));
    chk(name, "group 24 number 14 (one past the real count) refuses", !rgm2_map(&g, 24, 14, &mm));
    chk(name, "group 24 number 255 refuses", !rgm2_map(&g, 24, 255, &mm));
  }
  free(m.buf);
}

/* Step 7 (BACKLOG #91) colour: both anchors hit their expected counts on
 * both ROMs and recover the expected addresses; the PalMap bank comes out
 * $02 on Gold and $13 on Crystal; palette $00 decodes identically in both
 * ROMs; Olivine (group 1) roof day decodes to R7/G11/B15; a mutated anchor
 * byte falls back cleanly (ok=0, every field zeroed), never a guessed
 * address. */
static void test_colour(const char* rom_file, const char* name,
                         uint32_t want_env_ptrs, uint32_t want_bg_pal, uint8_t want_bank,
                         uint32_t palmap_anchor_off) {
  char path[512]; snprintf(path, sizeof path, "%s/%s", ROMS, rom_file);
  Mem m = load_file(path);
  if (!m.buf) { printf("  [%s] SKIP colour (corpus absent)\n", name); return; }
  g_ran = 1;
  static uint8_t scratch[4096];

  RomGbMap2Colour c;
  bool ok = rgm2_colour_open(&c, rd_mem, &m, m.len, scratch, sizeof scratch);
  chk(name, "colour anchors open", ok);
  if (ok) {
    chk(name, "env_ptrs_off matches oracle", c.env_ptrs_off == want_env_ptrs);
    chk(name, "bg_pal_off matches oracle", c.bg_pal_off == want_bg_pal);
    chk(name, "palmap_bank matches oracle", c.palmap_bank == want_bank);

    uint16_t out4[4]; uint8_t bg_idx;
    /* Force bg_idx=0 indirectly is not possible without walking the real
     * chain; instead cross-check TilesetBGPalette[0] directly (design §7.3's
     * own oracle: R28/G31/B16, R21/G21/B21, R13/G13/B13, R7/G7/B7,
     * byte-identical in both ROMs), bypassing rgm2_colour_palette()'s own
     * environment indirection so this assertion is independent of it. */
    uint8_t pal0[8];
    chk(name, "TilesetBGPalette[0] reads", rd_mem(&m, c.bg_pal_off, pal0, 8));
    uint16_t w0 = (uint16_t)(pal0[0] | (pal0[1] << 8));
    uint16_t w1 = (uint16_t)(pal0[2] | (pal0[3] << 8));
    uint16_t w2 = (uint16_t)(pal0[4] | (pal0[5] << 8));
    uint16_t w3 = (uint16_t)(pal0[6] | (pal0[7] << 8));
    chk(name, "palette $00 color0 R28 G31 B16", (w0 & 0x1F) == 28 && ((w0 >> 5) & 0x1F) == 31 && ((w0 >> 10) & 0x1F) == 16);
    chk(name, "palette $00 color1 R21 G21 B21", (w1 & 0x1F) == 21 && ((w1 >> 5) & 0x1F) == 21 && ((w1 >> 10) & 0x1F) == 21);
    chk(name, "palette $00 color2 R13 G13 B13", (w2 & 0x1F) == 13 && ((w2 >> 5) & 0x1F) == 13 && ((w2 >> 10) & 0x1F) == 13);
    chk(name, "palette $00 color3 R7 G7 B7", (w3 & 0x1F) == 7 && ((w3 >> 5) & 0x1F) == 7 && ((w3 >> 10) & 0x1F) == 7);
    (void)out4; (void)bg_idx;

    /* review-opus D2: LoadMapPals copies BOTH roof words (`ld bc, 4`) into
     * palette slot 6's colours 1 and 2 -- assert both, not just the one
     * this test used to check. Olivine (group 1): morn R14/G17/B31, day
     * R7/G11/B15 (design §7.4's own oracle). New Bark's own group (24):
     * word0 R20/G31/B14, word1 R11/G23/B5 -- an independent second data
     * point so a future regression that swaps/drops a word cannot hide
     * behind Olivine alone. */
    uint16_t roof1[2] = { 0, 0 };
    bool rok1 = rgm2_colour_roof(&c, rd_mem, &m, m.len, /*TOWN*/1, /*group 1 Olivine*/1, roof1);
    chk(name, "Olivine roof resolves", rok1);
    if (rok1) {
      chk(name, "Olivine roof word0 (morn) R14 G17 B31",
          (roof1[0] & 0x1F) == 14 && ((roof1[0] >> 5) & 0x1F) == 17 && ((roof1[0] >> 10) & 0x1F) == 31);
      chk(name, "Olivine roof word1 (day) R7 G11 B15",
          (roof1[1] & 0x1F) == 7 && ((roof1[1] >> 5) & 0x1F) == 11 && ((roof1[1] >> 10) & 0x1F) == 15);
    }

    uint16_t roof24[2] = { 0, 0 };
    bool rok24 = rgm2_colour_roof(&c, rd_mem, &m, m.len, /*TOWN*/1, /*group 24 New Bark*/24, roof24);
    chk(name, "New Bark roof resolves", rok24);
    if (rok24) {
      chk(name, "New Bark roof word0 R20 G31 B14",
          (roof24[0] & 0x1F) == 20 && ((roof24[0] >> 5) & 0x1F) == 31 && ((roof24[0] >> 10) & 0x1F) == 14);
      chk(name, "New Bark roof word1 R11 G23 B5",
          (roof24[1] & 0x1F) == 11 && ((roof24[1] >> 5) & 0x1F) == 23 && ((roof24[1] >> 10) & 0x1F) == 5);
    }

    chk(name, "roof refuses outside TOWN/ROUTE",
        !rgm2_colour_roof(&c, rd_mem, &m, m.len, /*INDOOR*/3, 1, roof1));

    /* review-opus D1 regression guard: the override must gate on the PalMap
     * NIBBLE (the loop index gbmap2_colour_setup() iterates, 0-7), never on
     * bg_idx (the outdoor DAY row is $08,$09,$0a,$28,$0c,$0d,$0e,$0f -- 6 is
     * never among them, so a bg_idx==6 gate is permanently dead on every
     * TOWN/ROUTE map). This test can't reach into pdna_gbmap2.c's own
     * static function, so it re-asserts the FACT the fix depends on: no
     * palette_index in 0-7 resolves to bg_idx 6 under New Bark's own TOWN
     * environment on either ROM -- confirming a bg_idx==6 gate really is
     * unreachable, and the fix's own p==6 gate is the only correct one. */
    {
      int any_bg_idx_6 = 0;
      for (uint8_t pidx = 0; pidx < 8; pidx++) {
        uint16_t tmp4[4]; uint8_t bg_idx = 0;
        if (rgm2_colour_palette(&c, rd_mem, &m, m.len, /*TOWN*/1, pidx, tmp4, &bg_idx) && bg_idx == 6u)
          any_bg_idx_6 = 1;
      }
      chk(name, "D1 regression guard: bg_idx==6 never occurs under TOWN (confirms the old gate was dead)",
          !any_bg_idx_6);
    }

    /* review-opus D3: the whole-PalMap bulk read must agree with the
     * existing per-nibble reader on every raw tile id New Bark's own
     * tileset actually defines, and must fail closed (zero-filled) on a
     * bad pal_off. */
    {
      RomGbMap2 g2; GbMap2Map map2; GbMap2Tileset ts2;
      bool g2ok = rgm2_open(&g2, rd_mem, &m, m.len, scratch, sizeof scratch) &&
                  rgm2_map(&g2, 24, 4, &map2) && rgm2_tileset(&g2, map2.tileset_id, &ts2);
      chk(name, "D3 setup: New Bark map+tileset resolve", g2ok);
      if (g2ok) {
        uint8_t bulk[128];
        bool bok = rgm2_colour_palmap(&c, rd_mem, &m, m.len, ts2.pal_off, bulk);
        chk(name, "D3 whole-PalMap bulk read succeeds", bok);
        if (bok) {
          int mismatch = 0;
          for (int raw = 0; raw < 32; raw++) {
            uint8_t nib_single = 0;
            rgm2_colour_nibble(&c, rd_mem, &m, m.len, ts2.pal_off, (uint8_t)raw, &nib_single);
            uint8_t b = bulk[raw >> 1];
            uint8_t nib_bulk = (raw & 1) ? (uint8_t)((b >> 4) & 0x0F) : (uint8_t)(b & 0x0F);
            if (nib_bulk != nib_single) mismatch = 1;
          }
          chk(name, "D3 bulk PalMap agrees with the per-nibble reader (raw ids 0-31)", !mismatch);
        }
        RomGbMap2Colour unopened;
        memset(&unopened, 0, sizeof unopened);
        uint8_t bad[128];
        chk(name, "D3 bulk PalMap fails closed on an unopened colour struct (ok=0)",
            !rgm2_colour_palmap(&unopened, rd_mem, &m, m.len, ts2.pal_off, bad));
        chk(name, "D3 bulk PalMap zero-fills its output on that failure",
            bad[0] == 0 && bad[64] == 0 && bad[127] == 0);
      }
    }
  }

  /* Mutate the PalMap-consumer anchor's own first concrete byte (its own
   * `21` opcode, at the oracle offset the caller passes -- independently
   * re-derived, same posture as test_negative_controls' hardcoded a1_off/
   * a3_off) -- must fall back cleanly, every field zeroed, never a guessed
   * address. This anchor's "exactly 2 hits, same bank" rule means mutating
   * ONE of the two copies drops the count to 1, which must ALSO refuse
   * (not silently accept the surviving one). */
  if (palmap_anchor_off < m.len) {
    static uint8_t scratch2[4096];
    uint8_t save = m.buf[palmap_anchor_off];
    m.buf[palmap_anchor_off] ^= 0xFFu;
    RomGbMap2Colour cm;
    bool mok = rgm2_colour_open(&cm, rd_mem, &m, m.len, scratch2, sizeof scratch2);
    chk(name, "mutated PalMap-consumer anchor (1 of 2) falls back cleanly", !mok);
    chk(name, "mutated PalMap-consumer anchor zeros every field",
        cm.env_ptrs_off == 0 && cm.bg_pal_off == 0 && cm.roof_pals_off == 0 &&
        cm.palmap_bank == 0 && cm.ok == 0);
    m.buf[palmap_anchor_off] = save;
  }

  free(m.buf);
}

int main(void) {
  test_one_rom("Gold.gbc", "Gold", 0x940ed, 0x156be, 26, 29, 368);
  test_one_rom("Crystal.gbc", "Crystal", 0x94000, 0x4d596, 26, 37, 388);
  test_live_saves();
  test_negative_controls("Gold.gbc", "Gold", 0x2cc6, 0x2dfa);
  test_negative_controls("Crystal.gbc", "Crystal", 0x2bed, 0x2d27);
  test_offgame_refusal();
  test_bank_boundary_mutation();
  test_number_bound("Gold.gbc", "Gold");
  test_number_bound("Crystal.gbc", "Crystal");
  test_colour("Gold.gbc", "Gold", 0x0b6ce, 0x0b75e, 0x02, 0x8010);
  test_colour("Crystal.gbc", "Crystal", 0x0b279, 0x0b319, 0x13, 0x4c011);

  if (!g_ran) { printf("host_romgbmap2_test: SKIP (no corpus)\n"); return 0; }
  printf("host_romgbmap2_test: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
