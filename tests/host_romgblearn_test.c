/* source/rom_gblearn.c + source/rom_gbbase.c's Gen-2 half -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_romgblearn_test.c \
 *      source/rom_gblearn.c source/rom_gbbase.c source/rom_gbsprite.c \
 *      source/gb_sprite_codec.c source/data_tables.c source/gb_edit.c \
 *      source/gb_session.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c -o /tmp/hromlearn && /tmp/hromlearn
 *
 * BACKLOG #50's ROM-derived facts for a legal Gen-1/2 mon-from-scratch: level-up
 * learnsets (this file's main subject) and the Gen-2 half of rom_gbbase.c's base
 * stats (the cross-check source for gb_new_mon's growth-rate agreement gate).
 *
 * Coverage (7 = rom_gblearn_moves_between, BACKLOG #373):
 *   1. every species parses (Red/Yellow 1..151, Gold/Crystal 1..251): a real
 *      rom_gblearn_open() hit, and rom_gblearn_moves_at() never structurally
 *      refuses (>= 0) at level 100 for any of them.
 *   2. rom_gbbase_gen2 cross-checked against pk_base_stats()/pk_species_growth()
 *      (data_tables.c, generated from pokeemerald) for all 251 species in both
 *      Gold and Crystal -- the "IDENTICAL to Gen 3's" claim rom_gbbase.h's
 *      header already made, independently re-verified here by reading the
 *      cartridge live rather than trusting the claim.
 *   3. Bulbasaur@5 = Tackle, Growl -- self-contained from rom_gblearn_moves_at
 *      ALONE on Gen 2 (whose table embeds level-1 starters); Gen 1's OWN
 *      Bulbasaur base-stats row start[] (rom_gbbase_gen1, BACKLOG #50's other
 *      new field) independently reads the SAME two moves.
 *   4. Pikachu@5 (Gen 1) = ThunderShock, Growl -- Gen 1's table alone returns
 *      NOTHING at level 5 (proven, not assumed: its first post-starter move is
 *      well above level 5, so this is a real check that the table stays out of
 *      the way, not a vacuous one), and rom_gbbase_gen1's start[] alone is the
 *      whole answer -- the merge a full gb_new_mon build performs (a later
 *      commit) reduces to "just the starters" at this level for exactly that
 *      reason. Verified against the ROM's own bytes, then eyeballed once
 *      against a public move-list reference (ThunderShock=84, Growl=45 in the
 *      Gen-3+ numbering this tree's move ids already use).
 *   5. Bulbasaur@100, both gens: the LAST four moves, oldest dropped first,
 *      cross-checked against the decomp's own evos_moves/evos_attacks source
 *      (read on the web, reference only -- never used as an input; the table
 *      itself is always located by shape at runtime, see rom_gblearn.h).
 *   6. negative controls: a corrupted pointer inside the (already-located, on a
 *      first clean open) table is refused by a fresh rom_gblearn_open() over
 *      the mutated bytes; Red.gb asked for as Gen 2, and Gold.gbc as Gen 1,
 *      both fail cleanly; NULL/bad-dex/unopened arguments are refused.
 *
 * ROMs are Guy's own dumps: outside the repo, gitignored, never copied in. A
 * missing corpus SKIPs rather than fails.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "rom_gblearn.h"
#include "rom_gbbase.h"
#include "rom_gbsprite.h"
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

/* A GbReadFn over an in-RAM copy of the ROM with ONE byte forced to a fixed
 * value -- the mutation negative control needs to corrupt a specific byte of
 * the table this same file located, without ever touching Guy's file on disk. */
typedef struct { const uint8_t* base; uint32_t size; uint32_t at; uint8_t val; } MutCtx;
static bool mut_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  const MutCtx* m = (const MutCtx*)ctx;
  if (off >= m->size || len > m->size - off) return false;
  memcpy(dst, m->base + off, len);
  uint8_t* d = (uint8_t*)dst;
  if (m->at >= off && m->at < off + len) d[m->at - off] = m->val;
  return true;
}

/* Opens `file` once and locates BOTH tables this batch of work added: the
 * learnset table (RomGbLearn) and, for Gen 2 only, RomGbSprite's own base_data
 * anchor that rom_gbbase_gen2() decodes. Returns false (with a SKIP printed)
 * only when the file is absent; a located-but-failed table is a real FAIL,
 * caught by the CHECKs inside. `*f_out` is the caller's to fclose(). */
static bool open_rom(const char* file, uint8_t gen, RomGbLearn* rl, RomGbSprite* gs,
                     FILE** f_out) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return false; }
  uint32_t sz = file_size(path);
  *f_out = f;

  memset(rl, 0, sizeof *rl);
  CHECK(rom_gblearn_open(rl, gen, file_read, f, sz), "%s: rom_gblearn_open", file);

  memset(gs, 0, sizeof *gs);
  GbRomGen want_rom_gen = (gen == GB_GEN1) ? GB_ROM_GEN1 : GB_ROM_GEN2;
  CHECK(rom_gbsprite_open(gs, file_read, f, sz, g_scratch, sizeof g_scratch, GB_ROM_NONE)
        && gs->gen == want_rom_gen, "%s: rom_gbsprite_open", file);

  return rl->ok && gs->ok;
}

/* ============================================================================ */
/* 1/2. every species parses; Gen-2 base stats/growth vs the compiled tables    */
/* ============================================================================ */

static void test_every_species_gen1(const char* file) {
  RomGbLearn rl; RomGbSprite gs; FILE* f;
  printf("\n== %s (Gen 1) ==\n", file);
  if (!open_rom(file, GB_GEN1, &rl, &gs, &f)) return;

  int nrefused = 0, nstart = 0;
  for (uint16_t dex = 1; dex <= 151; dex++) {
    uint8_t out4[4];
    int kept = rom_gblearn_moves_at(&rl, dex, 100, out4);
    if (kept < 0) { nrefused++; continue; }
    for (int i = 0; i < kept; i++)
      CHECK(out4[i] >= 1 && out4[i] <= gb_max_move(GB_GEN1),
            "%s: dex %u learnset move slot %d (%u) in range", file, dex, i, out4[i]);

    RomGb1Species sp;
    CHECK(rom_gbbase_gen1(&gs, file_read, f, dex, &sp), "%s: dex %u rom_gbbase_gen1", file, dex);
    if (sp.start[0]) nstart++;
  }
  CHECK(nrefused == 0, "%s: rom_gblearn_moves_at parses all 151 species", file);
  CHECK(nstart >= 145, "%s: nearly every species has a real starting move (%d/151)", file, nstart);
  fclose(f);
}

static void test_every_species_gen2(const char* file) {
  RomGbLearn rl; RomGbSprite gs; FILE* f;
  printf("\n== %s (Gen 2) ==\n", file);
  if (!open_rom(file, GB_GEN2, &rl, &gs, &f)) return;

  int nrefused = 0, mism_stats = 0, mism_growth = 0;
  for (uint16_t dex = 1; dex <= 251; dex++) {
    uint8_t out4[4];
    int kept = rom_gblearn_moves_at(&rl, dex, 100, out4);
    if (kept < 0) { nrefused++; continue; }
    for (int i = 0; i < kept; i++)
      CHECK(out4[i] >= 1 && out4[i] <= gb_max_move(GB_GEN2),
            "%s: dex %u learnset move slot %d (%u) in range", file, dex, i, out4[i]);

    RomGb2Species sp;
    bool sp_ok = rom_gbbase_gen2(&gs, file_read, f, dex, &sp);
    CHECK(sp_ok, "%s: dex %u rom_gbbase_gen2", file, dex);
    if (!sp_ok) continue;
    uint8_t ref[6];
    pk_base_stats(dex, ref);
    if (memcmp(sp.base, ref, 6) != 0) mism_stats++;
    if (sp.growth != pk_species_growth(dex)) mism_growth++;
  }
  CHECK(nrefused == 0, "%s: rom_gblearn_moves_at parses all 251 species", file);
  CHECK(mism_stats == 0, "%s: rom_gbbase_gen2 stats match pk_base_stats for all 251", file);
  CHECK(mism_growth == 0, "%s: rom_gbbase_gen2 growth matches pk_species_growth for all 251", file);
  fclose(f);
}

/* ============================================================================ */
/* 3/4/5. Bulbasaur and Pikachu, both generations, against the decomp's own     */
/*        evos_moves.asm / evos_attacks.asm (reference only -- read on the web, */
/*        never used as a locator input; cross-checked here against the ROM's   */
/*        own bytes, which is the only thing this module ever actually reads). */
/* ============================================================================ */

static void print4(const char* label, const uint8_t out4[4]) {
  printf("    %s: %u,%u,%u,%u (%s / %s / %s / %s)\n", label,
         out4[0], out4[1], out4[2], out4[3],
         out4[0] ? pk_move_name(out4[0]) : "-", out4[1] ? pk_move_name(out4[1]) : "-",
         out4[2] ? pk_move_name(out4[2]) : "-", out4[3] ? pk_move_name(out4[3]) : "-");
}

static void test_bulbasaur_pikachu_gen1(const char* file) {
  RomGbLearn rl; RomGbSprite gs; FILE* f;
  if (!open_rom(file, GB_GEN1, &rl, &gs, &f)) return;

  /* Bulbasaur (dex 1): the ROM's own base-stats row start[] -- Tackle, Growl --
   * and the SAME two moves the app's own move-name table (data_tables.c) names,
   * matching pokecrystal's independently-fetched BulbasaurEvosAttacks source
   * ("db 1, TACKLE" / "db 4, GROWL") even though this is the GEN-1 row. */
  RomGb1Species bulba;
  CHECK(rom_gbbase_gen1(&gs, file_read, f, 1, &bulba), "%s: rom_gbbase_gen1 Bulbasaur", file);
  uint8_t bulba_start[4] = { bulba.start[0], bulba.start[1], bulba.start[2], bulba.start[3] };
  print4("Bulbasaur start[]", bulba_start);
  CHECK(bulba_start[0] == 33 && bulba_start[1] == 45 && bulba_start[2] == 0 && bulba_start[3] == 0,
        "%s: Bulbasaur's Gen-1 starting moves are Tackle(33), Growl(45)", file);

  /* Gen 1's OWN table contributes nothing at level 5 for Bulbasaur (its first
   * post-starter move, Leech Seed, is level 7 in every one of Guy's dumps) --
   * so "Bulbasaur@5 = Tackle + Growl" for Gen 1 is exactly start[] alone; the
   * FULL merge (source/gb_new_mon.c) folds the two together, and reduces to
   * this at any level below the table's first entry. */
  uint8_t out4[4];
  int kept = rom_gblearn_moves_at(&rl, 1, 5, out4);
  CHECK(kept == 0, "%s: Bulbasaur's Gen-1 table alone has nothing by level 5 (kept=%d)", file, kept);

  /* Bulbasaur@100: the LAST four of the full 7-move Gen-1 table (7 Leech Seed,
   * 13 Vine Whip, 20 Poison Powder, 27 Razor Leaf, 34 Growth, 41 Sleep Powder,
   * 48 Solarbeam -- pokered's own BulbasaurEvosMoves), oldest three dropped. */
  kept = rom_gblearn_moves_at(&rl, 1, 100, out4);
  print4("Bulbasaur@100", out4);
  CHECK(kept == 4 && out4[0] == 75 && out4[1] == 74 && out4[2] == 79 && out4[3] == 76,
        "%s: Bulbasaur@100 = Razor Leaf(75), Growth(74), Sleep Powder(79), Solarbeam(76)", file);

  /* Pikachu (dex 25): ThunderShock + Growl, straight out of start[] -- the same
   * "table contributes nothing yet" situation as Bulbasaur, proven rather than
   * assumed by also checking rom_gblearn_moves_at() returns 0 at level 5. */
  RomGb1Species pika;
  CHECK(rom_gbbase_gen1(&gs, file_read, f, 25, &pika), "%s: rom_gbbase_gen1 Pikachu", file);
  uint8_t pika_start[4] = { pika.start[0], pika.start[1], pika.start[2], pika.start[3] };
  print4("Pikachu start[]", pika_start);
  CHECK(pika_start[0] == 84 && pika_start[1] == 45 && pika_start[2] == 0 && pika_start[3] == 0,
        "%s: Pikachu's Gen-1 starting moves are ThunderShock(84), Growl(45)", file);
  kept = rom_gblearn_moves_at(&rl, 25, 5, out4);
  CHECK(kept == 0, "%s: Pikachu's Gen-1 table alone has nothing by level 5 either (kept=%d)", file, kept);
  printf("  %s: Pikachu@5 (Gen 1) = ThunderShock + Growl -- confirmed via start[] "
         "(table contributes nothing this low)\n", file);

  fclose(f);
}

static void test_bulbasaur_gen2(const char* file) {
  RomGbLearn rl; RomGbSprite gs; FILE* f;
  if (!open_rom(file, GB_GEN2, &rl, &gs, &f)) return;

  /* Gen 2's table embeds the level-1 starters itself, so "Bulbasaur@5" is
   * self-contained here -- no base-stats row involved at all. */
  uint8_t out4[4];
  int kept = rom_gblearn_moves_at(&rl, 1, 5, out4);
  print4("Bulbasaur@5", out4);
  CHECK(kept == 2 && out4[0] == 33 && out4[1] == 45,
        "%s: Bulbasaur@5 (Gen 2) = Tackle(33), Growl(45)", file);

  /* Bulbasaur@100: pokecrystal's own BulbasaurEvosAttacks (fetched, reference
   * only) lists 11 level-up moves ending 25 Sweet Scent, 32 Growth,
   * 39 Synthesis, 46 Solarbeam -- the last four, oldest seven dropped. */
  kept = rom_gblearn_moves_at(&rl, 1, 100, out4);
  print4("Bulbasaur@100", out4);
  CHECK(kept == 4 && out4[0] == 230 && out4[1] == 74 && out4[2] == 235 && out4[3] == 76,
        "%s: Bulbasaur@100 (Gen 2) = Sweet Scent(230), Growth(74), Synthesis(235), "
        "Solarbeam(76)", file);
  fclose(f);
}

/* ============================================================================ */
/* 6. negative controls                                                         */
/* ============================================================================ */

/* ============================================================================ */
/* 7. rom_gblearn_min_level -- BACKLOG #50's create-flow level floor            */
/* ============================================================================ */

/* Seven real evolution chains, cross-checked against pokered's own
 * data/pokemon/evos_moves.asm and pokecrystal's data/pokemon/evos_attacks.asm
 * (fetched, reference-only -- rom_gblearn.h's own header comment on
 * rom_gblearn_min_level cites the exact lines and the propagation rule this
 * exercises). Crobat is Gen-2 only: dex 169 does not exist in Gen 1's 1..151
 * range, so it is checked only when `gen == GB_GEN2`. */
static void test_min_level(const char* file, uint8_t gen) {
  RomGbLearn rl; RomGbSprite gs; FILE* f;
  if (!open_rom(file, gen, &rl, &gs, &f)) return;

  static const struct { uint16_t dex; uint8_t want; const char* name; } cases[] = {
    { 1,   5, "Bulbasaur (base form)" },
    { 2,  16, "Ivysaur (Bulbasaur->Ivysaur LEVEL 16)" },
    { 3,  32, "Venusaur (Ivysaur->Venusaur LEVEL 32)" },
    { 6,  36, "Charizard (Charmander base -> Charmeleon LEVEL 16 -> Charizard LEVEL 36)" },
    { 26,  5, "Raichu (Pikachu's own floor propagates through the Thunder Stone ITEM evo)" },
    { 76, 25, "Golem (Geodude->Graveler LEVEL 25, Graveler->Golem TRADE propagates)" },
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    uint8_t got = rom_gblearn_min_level(&rl, cases[i].dex);
    CHECK(got == cases[i].want, "%s: min_level(dex %u) = %u, want %u -- %s",
          file, (unsigned)cases[i].dex, (unsigned)got, (unsigned)cases[i].want, cases[i].name);
  }
  if (gen == GB_GEN2) {
    uint8_t got = rom_gblearn_min_level(&rl, 169);
    CHECK(got == 22, "%s: min_level(dex 169, Crobat) = %u, want 22 "
          "(Zubat->Golbat LEVEL 22, Golbat->Crobat HAPPINESS propagates)", file, (unsigned)got);
  }
  fclose(f);
}


/* ============================================================================ */
/* 7. rom_gblearn_moves_between (BACKLOG #373, Day-Care take-out growth)         */
/* ============================================================================ */

/* Apply the recorded events to cur4 with an INDEPENDENT four-slot FIFO (free slot, else oldest
 * out). If rom_gblearn_moves_between's out4 and its event list ever disagree, this is where. */
static void replay_events(const uint8_t cur4[4], const DcLearn* ev, int n, uint8_t out[4]) {
  memcpy(out, cur4, 4);
  for (int i = 0; i < n; i++) {
    int slot = -1;
    for (int k = 0; k < 4; k++) if (out[k] == 0) { slot = k; break; }
    if (slot < 0) { out[0] = out[1]; out[1] = out[2]; out[2] = out[3]; slot = 3; }
    out[slot] = (uint8_t)ev[i].newmove;
  }
}

static void test_between(const char* file, uint8_t gen) {
  RomGbLearn rl; RomGbSprite gs; FILE* f;
  printf("\n== moves_between %s ==\n", file);
  if (!open_rom(file, gen, &rl, &gs, &f)) return;
  uint16_t maxdex = (gen == GB_GEN1) ? 151 : 251;
  const uint8_t junk[4] = { 15, 19, 57, 70 };   /* Cut, Fly, Surf, Strength: no species' learnset */

  /* (a) floor 0 + a seed reproduces rom_gblearn_moves_at_seeded byte-for-byte, 20 pairs */
  uint32_t rng = 12345u;
  for (int t = 0; t < 20; t++) {
    rng = rng * 1103515245u + 12345u;
    uint16_t dex = (uint16_t)(1 + (rng >> 8) % maxdex);
    rng = rng * 1103515245u + 12345u;
    uint8_t lvl = (uint8_t)(1 + (rng >> 8) % 100);
    uint8_t seed[4] = { 0, 0, 0, 0 }, a4[4], b4[4];
    rom_gblearn_moves_at(&rl, dex, (uint8_t)(1 + (rng >> 16) % 100), seed);   /* a realistic current set */
    int ka = rom_gblearn_moves_at_seeded(&rl, dex, lvl, seed, a4);
    DcLearn ev[DC_LEARN_MAX]; int n = 0; bool ovf = false;
    int kb = rom_gblearn_moves_between(&rl, dex, 0, lvl, seed, b4, ev, &n, &ovf);
    CHECK(ka == kb && memcmp(a4, b4, 4) == 0,
          "%s: floor 0 == _seeded (dex %u lvl %u): kept %d/%d", file, dex, lvl, ka, kb);
  }

  /* (b) per species: events replay to out4; overflow flag == (n > 8); the first
   *     shift drops the OLDEST move */
  int nsp_overflow = 0;
  for (uint16_t dex = 1; dex <= maxdex; dex++) {
    uint8_t out4[4], rep[4]; DcLearn ev[DC_LEARN_MAX]; int n = 0; bool ovf = false;
    int k = rom_gblearn_moves_between(&rl, dex, 0, 100, junk, out4, ev, &n, &ovf);
    CHECK(k >= 0, "%s: dex %u between(0,100) refused", file, dex);
    if (k < 0) continue;
    CHECK(ovf == (n > DC_LEARN_MAX), "%s: dex %u overflow flag %d vs n %d", file, dex, ovf, n);
    nsp_overflow += ovf;
    int shown = n < DC_LEARN_MAX ? n : DC_LEARN_MAX;
    replay_events(junk, ev, shown, rep);
    if (!ovf) CHECK(memcmp(rep, out4, 4) == 0, "%s: dex %u events do not replay to out4", file, dex);
    for (int i = 0; i < shown; i++) {
      CHECK(ev[i].at_level >= 1 && ev[i].at_level <= 100, "%s: dex %u ev level", file, dex);
      /* NOT asserted ascending: a few real tables are not level-sorted (Yellow dex 57, dex 89) and the
       * games walk them in TABLE order, which is what the events record. */
    }
    if (n >= 1) CHECK(ev[0].replaced == junk[0], "%s: dex %u first shift drops the OLDEST (%u)", file, dex, ev[0].replaced);
  }
  /* Red's tables are short enough that no species reaches 9 events from L0 (measured: 0); Yellow
   * (2 species) and Gen 2 have some, so the overflow latch is exercised for real there. */
  if (strcmp(file, "Red.gb") != 0)
    CHECK(nsp_overflow > 0, "%s: at least one species overflows 8 events from L0 to L100 (got %d)", file, nsp_overflow);

  /* (c) the floor rule on Bulbasaur (dex 1): find a level L where a move is learned, then
   *     (L-1, L] teaches it but (L, L] does not -- an entry AT lv_prev is NOT taught */
  uint8_t L = 0, prev4[4] = { 0, 0, 0, 0 }, at4[4] = { 0, 0, 0, 0 };
  for (uint8_t l = 8; l <= 60 && !L; l++) {           /* >= 8 skips Gen 2's level-1 starters */
    rom_gblearn_moves_at(&rl, 1, (uint8_t)(l - 1), prev4);
    rom_gblearn_moves_at(&rl, 1, l, at4);
    if (memcmp(prev4, at4, 4)) L = l;
  }
  CHECK(L != 0, "%s: Bulbasaur has a learn level >= 8", file);
  if (L) {
    DcLearn ev[DC_LEARN_MAX]; int n = 0; bool ovf = false; uint8_t o4[4];
    CHECK(rom_gblearn_moves_between(&rl, 1, (uint8_t)(L - 1), L, prev4, o4, ev, &n, &ovf) >= 0 && n >= 1
          && ev[0].at_level == L, "%s: (%u,%u] teaches the L%u move", file, L - 1, L, L);
    n = 0;
    CHECK(rom_gblearn_moves_between(&rl, 1, L, L, prev4, o4, ev, &n, &ovf) >= 0 && n == 0
          && memcmp(o4, prev4, 4) == 0, "%s: (%u,%u] teaches nothing", file, L, L);
    /* (d) already-known is skipped: carry the learned move, nothing is taught */
    n = 0;
    CHECK(rom_gblearn_moves_between(&rl, 1, (uint8_t)(L - 1), L, at4, o4, ev, &n, &ovf) >= 0 && n == 0
          && memcmp(o4, at4, 4) == 0, "%s: a move already known is skipped", file);
  }
  fclose(f);
}

static void test_cross_gen_refusal(void) {
  printf("\n== cross-generation refusal ==\n");
  char path[512]; uint32_t sz; FILE* f; RomGbLearn rl;

  snprintf(path, sizeof path, "%s/Red.gb", ROMS);
  f = fopen(path, "rb");
  if (f) {
    sz = file_size(path);
    memset(&rl, 0, sizeof rl);
    CHECK(!rom_gblearn_open(&rl, GB_GEN2, file_read, f, sz) && !rl.ok,
          "Red.gb asked for as Gen 2 fails cleanly");
    fclose(f);
  } else printf("  SKIP Red.gb (not present)\n");

  snprintf(path, sizeof path, "%s/Gold.gbc", ROMS);
  f = fopen(path, "rb");
  if (f) {
    sz = file_size(path);
    memset(&rl, 0, sizeof rl);
    CHECK(!rom_gblearn_open(&rl, GB_GEN1, file_read, f, sz) && !rl.ok,
          "Gold.gbc asked for as Gen 1 fails cleanly");
    fclose(f);
  } else printf("  SKIP Gold.gbc (not present)\n");
}

static void test_mutation_negative_control(void) {
  printf("\n== mutation negative control (Red.gb) ==\n");
  char path[512];
  snprintf(path, sizeof path, "%s/Red.gb", ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP Red.gb (not present)\n"); return; }
  uint32_t sz = file_size(path);

  RomGbLearn rl;
  bool opened = rom_gblearn_open(&rl, GB_GEN1, file_read, f, sz);
  CHECK(opened, "Red.gb opens cleanly before the mutation");
  if (!opened) { fclose(f); return; }
  uint32_t table_off = rl.table_off;

  uint8_t* ram = (uint8_t*)malloc(sz);
  CHECK(ram != NULL, "allocate a RAM copy of Red.gb for the mutation");
  if (ram) {
    CHECK(file_read(f, 0, ram, sz), "read the whole ROM into RAM");
    /* Flip the low byte of a pointer well inside the table (entry 50 of 190,
     * comfortably past both ends) -- this either sends the "pointer" out of
     * the ROMX window, breaks the strictly-increasing/delta shape check
     * against its neighbours, or lands it on bytes that do not parse as a
     * legal evolution+moves blob. Any of the three is a real refusal. */
    MutCtx mc; mc.base = ram; mc.size = sz; mc.at = table_off + 50u * 2u;
    uint8_t original; file_read(f, mc.at, &original, 1);
    mc.val = (uint8_t)(original ^ 0xFFu);

    RomGbLearn rl2;
    memset(&rl2, 0, sizeof rl2);
    CHECK(!rom_gblearn_open(&rl2, GB_GEN1, mut_read, &mc, sz) && !rl2.ok,
          "a corrupted pointer makes the whole table fail to relocate");
    free(ram);
  }
  fclose(f);
}

static void test_bad_args(void) {
  printf("\n== bad arguments ==\n");
  char path[512];
  snprintf(path, sizeof path, "%s/Red.gb", ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP Red.gb (not present)\n"); return; }
  uint32_t sz = file_size(path);

  RomGbLearn rl;
  CHECK(!rom_gblearn_open(NULL, GB_GEN1, file_read, f, sz), "NULL rl refused");
  memset(&rl, 0, sizeof rl);
  CHECK(!rom_gblearn_open(&rl, 3, file_read, f, sz), "gen 3 refused");
  CHECK(!rom_gblearn_open(&rl, GB_GEN1, NULL, f, sz), "NULL read refused");
  CHECK(!rom_gblearn_open(&rl, GB_GEN1, file_read, f, 0), "size 0 refused");
  CHECK(!rom_gblearn_open(&rl, GB_GEN1, file_read, f, sz + 1), "a non-bank-aligned size refused");

  CHECK(rom_gblearn_open(&rl, GB_GEN1, file_read, f, sz), "(control) Red.gb opens");
  uint8_t out4[4];
  CHECK(rom_gblearn_moves_at(NULL, 1, 5, out4) == -1, "NULL rl refused");
  CHECK(rom_gblearn_moves_at(&rl, 1, 5, NULL) == -1, "NULL out4 refused");
  CHECK(rom_gblearn_moves_at(&rl, 0, 5, out4) == -1, "dex 0 refused");
  CHECK(rom_gblearn_moves_at(&rl, 999, 5, out4) == -1, "dex 999 refused");
  RomGbLearn unopened; memset(&unopened, 0, sizeof unopened);
  CHECK(rom_gblearn_moves_at(&unopened, 1, 5, out4) == -1, "an unopened rl refused");

  /* min_level never refuses (gb_new_mon always needs SOME legal level) -- its
   * "bad argument" behaviour is failing open to base level 5, not a sentinel. */
  CHECK(rom_gblearn_min_level(NULL, 1) == 5, "min_level: NULL rl fails open to 5");
  CHECK(rom_gblearn_min_level(&unopened, 1) == 5, "min_level: an unopened rl fails open to 5");
  CHECK(rom_gblearn_min_level(&rl, 0) == 5, "min_level: dex 0 fails open to 5");
  CHECK(rom_gblearn_min_level(&rl, 999) == 5, "min_level: dex 999 fails open to 5");

  RomGbSprite gs; memset(&gs, 0, sizeof gs);
  RomGb2Species sp;
  CHECK(!rom_gbbase_gen2(NULL, file_read, f, 1, &sp), "rom_gbbase_gen2: NULL gs refused");
  CHECK(!rom_gbbase_gen2(&gs, file_read, f, 1, &sp), "rom_gbbase_gen2: an unopened gs refused");
  CHECK(!rom_gbbase_gen2(&gs, file_read, f, 1, NULL), "rom_gbbase_gen2: NULL out refused");

  fclose(f);
}

int main(void) {
  test_every_species_gen1("Red.gb");
  test_every_species_gen1("Yellow.gb");
  test_every_species_gen2("Gold.gbc");
  test_every_species_gen2("Crystal.gbc");

  printf("\n== Bulbasaur / Pikachu ==\n");
  test_bulbasaur_pikachu_gen1("Red.gb");
  test_bulbasaur_pikachu_gen1("Yellow.gb");
  test_bulbasaur_gen2("Gold.gbc");
  test_bulbasaur_gen2("Crystal.gbc");

  printf("\n== rom_gblearn_min_level ==\n");
  test_min_level("Red.gb", GB_GEN1);
  test_min_level("Yellow.gb", GB_GEN1);
  test_min_level("Gold.gbc", GB_GEN2);
  test_min_level("Crystal.gbc", GB_GEN2);

  test_between("Red.gb", GB_GEN1);
  test_between("Yellow.gb", GB_GEN1);
  test_between("Gold.gbc", GB_GEN2);
  test_between("Crystal.gbc", GB_GEN2);

  test_cross_gen_refusal();
  test_mutation_negative_control();
  test_bad_args();

  printf("\n%d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
