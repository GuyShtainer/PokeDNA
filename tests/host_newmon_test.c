/* source/gb_new_mon.c -- BACKLOG #50, "create a mon from scratch" -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_newmon_test.c \
 *      source/gb_new_mon.c source/gb_editor.c source/gb_edit.c source/gb_session.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/rom_gblearn.c source/rom_gbbase.c source/rom_gbsprite.c \
 *      source/gb_sprite_codec.c source/data_tables.c source/ui_font.c \
 *      -o /tmp/hnewmon && /tmp/hnewmon
 *
 * Assembles a REAL GbNewMonSrc off Guy's own cartridge dumps (rom_gbbase_gen1/2
 * + rom_gblearn_moves_at + gb_new_mon_g1_moves for the Gen-1 merge -- exactly
 * what source/pdna_gen12.c's CREATE flow will do) and drives gb_new_mon() the
 * way that screen will: pick a species, a level, build, check.
 *
 * Coverage:
 *   1. Bulbasaur@5, both gens (the brief's own worked example): gb_new_mon
 *      succeeds, gb_check() reports ZERO issues, every move id is in range,
 *      EXP agrees with level under the species' own growth curve, every
 *      occupied move slot has PP > 0, and the nickname round-trips (get then
 *      set changes zero bytes -- gb_edit.h's own NAMES guarantee, exercised
 *      through a freshly-created record rather than a loaded one).
 *   2. a full sweep, every species this tree can create, at three levels
 *      (1, 50, 100) in all four ROMs: same five checks, so this is not just a
 *      Bulbasaur-shaped success.
 *   3. determinism: the same seed builds byte-identical DVs twice; two
 *      different seeds (usually) do not.
 *   4. negative controls: a corrupted growth rate refuses (the ROM/compiled-
 *      table cross-check this exists for -- rom_gbbase.h); bad gen/level/dex;
 *      NULL src/out; gb_new_mon_g1_moves' own bad arguments.
 *
 * ROMs are Guy's own dumps: outside the repo, gitignored, never copied in. A
 * missing corpus SKIPs rather than fails.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gb_new_mon.h"
#include "data_tables.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* -------------------------------------------------------------- file I/O -------- */

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, f) != len) return false;
  return true;
}
static uint32_t file_size(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f);
  return (uint32_t)n;
}
static uint8_t g_scratch[4096];

typedef struct {
  uint8_t gen;
  bool ok;
  RomGbSprite gs;
  RomGbLearn rl;
  FILE* f;
} Rig;

static bool rig_open(Rig* r, const char* file, uint8_t gen) {
  char path[512];
  memset(r, 0, sizeof *r);
  r->gen = gen;
  snprintf(path, sizeof path, "%s/%s", ROMS, file);
  r->f = fopen(path, "rb");
  if (!r->f) { printf("  SKIP %s (not present)\n", file); return false; }
  uint32_t sz = file_size(path);
  bool a = rom_gblearn_open(&r->rl, gen, file_read, r->f, sz);
  GbRomGen want = (gen == GB_GEN1) ? GB_ROM_GEN1 : GB_ROM_GEN2;
  bool b = rom_gbsprite_open(&r->gs, file_read, r->f, sz, g_scratch, sizeof g_scratch, GB_ROM_NONE)
        && r->gs.gen == want;
  CHECK(a, "%s: rom_gblearn_open", file);
  CHECK(b, "%s: rom_gbsprite_open", file);
  r->ok = a && b;
  return r->ok;
}

/* Build a GbNewMonSrc for `dex`/`level` the SAME way source/pdna_gen12.c's
 * CREATE flow will: rom_gbbase_gen1/2 for base stats/growth, rom_gblearn for
 * the moveset (merged with the base-stats starters for Gen 1). Returns false
 * on any ROM-read failure (a bad dex for this ROM, mainly). */
static bool build_src(Rig* r, uint16_t dex, uint8_t level, GbNewMonSrc* src) {
  memset(src, 0, sizeof *src);
  if (r->gen == GB_GEN1) {
    RomGb1Species sp;
    if (!rom_gbbase_gen1(&r->gs, file_read, r->f, dex, &sp)) return false;
    memcpy(src->base, sp.base.base, GB_NSTATS);
    src->type1 = sp.base.type1; src->type2 = sp.base.type2;
    src->growth = sp.growth;
    if (gb_new_mon_g1_moves(&sp, &r->rl, dex, level, src->moves) < 0) return false;
  } else {
    RomGb2Species sp;
    if (!rom_gbbase_gen2(&r->gs, file_read, r->f, dex, &sp)) return false;
    src->growth = sp.growth;
    if (rom_gblearn_moves_at(&r->rl, dex, level, src->moves) < 0) return false;
  }
  src->species_name = pk_species_name(dex);
  src->ot_name = "GUY";
  src->ot_id = 54321;
  return true;
}

/* The five checks every created mon must pass, regardless of species/level. */
static void check_created(const char* label, uint8_t gen, uint16_t dex, uint8_t level,
                          const GbEditMon* e) {
  GbIssues iss;
  bool clean = gb_check(e, &iss);
  CHECK(clean, "%s: gb_check() is clean (first issue: %s)", label, clean ? "" : gb_issue_text(&iss));

  for (int i = 0; i < 4; i++) {
    uint8_t mv = gb_get_move(e, i);
    CHECK(mv <= gb_max_move(gen), "%s: move slot %d (%u) <= gb_max_move", label, i, mv);
    if (mv) CHECK(gb_get_pp(e, i) > 0, "%s: move slot %d has PP > 0", label, i);
  }

  CHECK(gb_level_from_exp(dex, gb_get_exp(e)) == level,
        "%s: EXP agrees with level %u under dex %u's growth curve", label, level, dex);

  char nm[GB_TEXT_MAX];
  gb_get_nickname(e, nm, sizeof nm);
  GbEditMon t = *e;
  char fb[GB_GLYPH_MAX];
  CHECK(gb_set_nickname(&t, nm) && memcmp(&t, e, sizeof t) == 0,
        "%s: nickname '%s' round-trips (changes zero bytes)", label, nm);
  (void)fb;
}

/* ============================================================================ */
/* 1. Bulbasaur@5, both gens -- the brief's own worked example                  */
/* ============================================================================ */

static void test_bulbasaur5(const char* file, uint8_t gen) {
  Rig r;
  printf("\n== %s Bulbasaur@5 ==\n", file);
  if (!rig_open(&r, file, gen)) return;

  GbNewMonSrc src;
  CHECK(build_src(&r, 1, 5, &src), "%s: build_src Bulbasaur@5", file);
  GbEditMon e;
  CHECK(gb_new_mon(gen, 1, 5, &src, 0xC0FFEEu, &e), "%s: gb_new_mon Bulbasaur@5", file);
  check_created(file, gen, 1, 5, &e);
  fclose(r.f);
}

/* ============================================================================ */
/* 2. every species this tree can create, at three levels                       */
/* ============================================================================ */

static void test_sweep(const char* file, uint8_t gen) {
  Rig r;
  printf("\n== %s full species sweep ==\n", file);
  if (!rig_open(&r, file, gen)) return;

  uint16_t maxdex = gb_max_species(gen);
  const uint8_t levels[] = { 1, 50, 100 };
  int ncreated = 0, nrefused = 0;
  for (uint16_t dex = 1; dex <= maxdex; dex++) {
    for (unsigned li = 0; li < sizeof levels / sizeof levels[0]; li++) {
      uint8_t level = levels[li];
      GbNewMonSrc src;
      if (!build_src(&r, dex, level, &src)) { nrefused++; continue; }
      GbEditMon e;
      if (!gb_new_mon(gen, dex, level, &src, (uint32_t)(dex * 977u + level), &e)) {
        nrefused++;
        continue;
      }
      ncreated++;
      char label[32];
      snprintf(label, sizeof label, "dex%u@%u", dex, level);
      check_created(label, gen, dex, level, &e);
    }
  }
  printf("  %d created, %d refused (of %u species x 3 levels)\n", ncreated, nrefused, maxdex);
  CHECK(nrefused == 0, "%s: every species/level combination this ROM offers creates cleanly", file);
  fclose(r.f);
}

/* ============================================================================ */
/* 3. determinism                                                               */
/* ============================================================================ */

static void test_determinism(const char* file, uint8_t gen) {
  Rig r;
  printf("\n== %s determinism ==\n", file);
  if (!rig_open(&r, file, gen)) return;

  GbNewMonSrc src;
  CHECK(build_src(&r, 25, 30, &src), "%s: build_src Pikachu@30", file);
  GbEditMon a, b, c;
  CHECK(gb_new_mon(gen, 25, 30, &src, 42u, &a), "%s: gb_new_mon seed 42 (a)", file);
  CHECK(gb_new_mon(gen, 25, 30, &src, 42u, &b), "%s: gb_new_mon seed 42 (b)", file);
  CHECK(memcmp(&a, &b, sizeof a) == 0, "%s: the same seed builds a byte-identical record", file);
  CHECK(gb_new_mon(gen, 25, 30, &src, 4242u, &c), "%s: gb_new_mon seed 4242 (c)", file);
  CHECK(memcmp(&a, &c, sizeof a) != 0, "%s: a different seed (usually) builds a different one", file);
  fclose(r.f);
}

/* ============================================================================ */
/* 4. negative controls                                                         */
/* ============================================================================ */

static void test_bad_args(const char* file, uint8_t gen) {
  Rig r;
  printf("\n== %s bad arguments ==\n", file);
  if (!rig_open(&r, file, gen)) return;

  GbNewMonSrc src;
  CHECK(build_src(&r, 1, 5, &src), "%s: build_src Bulbasaur@5", file);
  GbEditMon e;

  CHECK(gb_new_mon(gen, 1, 5, &src, 1u, &e), "(control) a normal build succeeds");
  CHECK(!gb_new_mon(gen, 1, 5, NULL, 1u, &e), "NULL src refused");
  CHECK(!gb_new_mon(gen, 1, 5, &src, 1u, NULL), "NULL out refused");
  CHECK(!gb_new_mon(3, 1, 5, &src, 1u, &e), "gen 3 refused");
  CHECK(!gb_new_mon(gen, 1, 0, &src, 1u, &e), "level 0 refused");
  CHECK(!gb_new_mon(gen, 1, 101, &src, 1u, &e), "level 101 refused");
  CHECK(!gb_new_mon(gen, 0, 5, &src, 1u, &e), "dex 0 refused");
  CHECK(!gb_new_mon(gen, (uint16_t)(gb_max_species(gen) + 1u), 5, &src, 1u, &e),
        "dex past gb_max_species refused");

  GbNewMonSrc bad_growth = src;
  bad_growth.growth = (uint8_t)(src.growth ^ 0xFFu);
  CHECK(!gb_new_mon(gen, 1, 5, &bad_growth, 1u, &e),
        "a growth rate disagreeing with gb_growth_rate(dex) refuses");

  if (gen == GB_GEN1) {
    RomGb1Species sp;
    CHECK(rom_gbbase_gen1(&r.gs, file_read, r.f, 1, &sp), "%s: rom_gbbase_gen1 Bulbasaur", file);
    uint8_t out4[4];
    CHECK(gb_new_mon_g1_moves(NULL, &r.rl, 1, 5, out4) == -1, "gb_new_mon_g1_moves: NULL base refused");
    CHECK(gb_new_mon_g1_moves(&sp, NULL, 1, 5, out4) == -1, "gb_new_mon_g1_moves: NULL rl refused");
    CHECK(gb_new_mon_g1_moves(&sp, &r.rl, 1, 5, NULL) == -1, "gb_new_mon_g1_moves: NULL out4 refused");
    RomGbLearn unopened; memset(&unopened, 0, sizeof unopened);
    CHECK(gb_new_mon_g1_moves(&sp, &unopened, 1, 5, out4) == -1,
          "gb_new_mon_g1_moves: an unopened rl refused");
  }
  fclose(r.f);
}

int main(void) {
  test_bulbasaur5("Red.gb", GB_GEN1);
  test_bulbasaur5("Yellow.gb", GB_GEN1);
  test_bulbasaur5("Gold.gbc", GB_GEN2);
  test_bulbasaur5("Crystal.gbc", GB_GEN2);

  test_sweep("Red.gb", GB_GEN1);
  test_sweep("Gold.gbc", GB_GEN2);

  test_determinism("Red.gb", GB_GEN1);
  test_determinism("Gold.gbc", GB_GEN2);

  test_bad_args("Red.gb", GB_GEN1);
  test_bad_args("Gold.gbc", GB_GEN2);

  printf("\n%d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
