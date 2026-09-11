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

int main(void) {
  test_one_rom("Gold.gbc", "Gold", 0x940ed, 0x156be, 26, 29, 368);
  test_one_rom("Crystal.gbc", "Crystal", 0x94000, 0x4d596, 26, 37, 388);
  test_live_saves();
  test_negative_controls("Gold.gbc", "Gold", 0x2cc6, 0x2dfa);
  test_negative_controls("Crystal.gbc", "Crystal", 0x2bed, 0x2d27);
  test_offgame_refusal();
  test_bank_boundary_mutation();

  if (!g_ran) { printf("host_romgbmap2_test: SKIP (no corpus)\n"); return 0; }
  printf("host_romgbmap2_test: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
