/* Host test for the generated script-placement table (source/statics.c, built by
 * tools/gen_statics.py --from-rom).
 *   cc -std=c11 -I source tests/host_statics_test.c source/statics.c \
 *      source/gen3_legality_hooks.c source/gen3_legality2.c source/learnsets2.c \
 *      source/encounters.c source/evolutions.c source/gen3_pidiv.c source/gen3_mon.c \
 *      source/gen3_edit.c source/gen3_save.c source/gen3_daycare.c source/data_tables.c \
 *      source/rom_map.c -o /tmp/hst
 *   /tmp/hst                       # ROMs from the default corpus directory
 *   /tmp/hst /path/to/rom/dir
 *
 * WHY THIS TABLE EXISTS AT ALL — the bug it closes, stated once so the next reader does
 * not have to reconstruct it. The wild-encounter table describes grass, water, rocks and
 * fishing. It does NOT describe the Kecleon on Routes 119/120, which is an invisible
 * object event you reveal with the Devon Scope and which a map script starts at a level
 * it fixes itself (`setwildbattle SPECIES_KECLEON, 30`). Those same routes ALSO carry an
 * ordinary wild Kecleon row at L25-25. The legality encounter hook compared the first
 * against the second and called five Kecleon that Guy caught in normal play suspect —
 * reasoning from data that does not describe how the Pokemon is obtained. statics.h is
 * the missing half; this test is what keeps it honest.
 *
 * The generator is Python; this test is the INDEPENDENT re-derivation. It derives the
 * event-script region from each ROM again — in C, through rom_map.c's own header walk
 * rather than the generator's — rescans it for the two script opcodes, and demands an
 * exact bijection with what statics.c reports. A generator bug that drops, invents or
 * mislevels a placement cannot survive that; a table that merely "looks plausible" would.
 *
 * What it asserts:
 *   1) every present ROM yields a non-EMPTY table (an empty one is a silent
 *      regeneration failure, not a skip);
 *   2) the shipped table == a fresh C scan of the ROM, exactly, in both directions —
 *      same (species, level, kind) set;
 *   3) the shipped table is sorted by (species, level), which pk_static_list relies on;
 *   4) GROUND TRUTH from the games themselves, including facts a wrong opcode or a
 *      mis-derived region could not fake: KECLEON L30 in R/S/E, HYPNO L30 in FR/LG, the
 *      three Regis at L40, MEWTWO L70, and the version-exclusive Groudon/Kyogre split
 *      (Ruby L45 Groudon and no Kyogre, Sapphire the mirror, Emerald both at L70);
 *   5) THE REGRESSION ITSELF: pk2_line_scripted answers YES for Kecleon in its origin
 *      game, and walks the pre-evolution chain so New Mauville's static VOLTORB also
 *      covers the ELECTRODE its owner is showing you;
 *   6) the API's no-data / out-of-range answers are the conservative ones, and the
 *      encounter hook's arming gate really does require BOTH tables.
 *
 * ROMs are Guy's own cartridge dumps and are never part of this repo; a missing ROM
 * SKIPS its game rather than failing, so the suite still runs without them.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "statics.h"
#include "encounters.h"
#include "gen3_legality_hooks.h"
#include "data_tables.h"
#include "rom_map.h"

static int g_fail = 0, g_check = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); \
  printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define ROM_BYTES  (16u * 1024u * 1024u)
#define ROM_BASE   0x08000000u
#define MAX_SPECIES 411
#define MAX_ITEM    376

#define OP_SETWILDBATTLE 0xB6   /* pokeemerald data/script_cmd_table.inc:200 */
#define OP_GIVEMON       0x79   /* ...:139                                   */

/* Gen-3 INTERNAL species ids (reference/pokeemerald_data/include/constants/species.h) */
#define SP_VOLTORB    100
#define SP_ELECTRODE  101
#define SP_DROWZEE     96
#define SP_HYPNO       97
#define SP_MEWTWO     150
#define SP_SUDOWOODO  185
#define SP_POOCHYENA  286
#define SP_SKITTY     315
#define SP_KECLEON    317
#define SP_BELDUM     398
#define SP_REGIROCK   401
#define SP_REGICE     402
#define SP_REGISTEEL  403
#define SP_KYOGRE     404
#define SP_GROUDON    405

/* ---- ROM access ----------------------------------------------------------- */
typedef struct { const uint8_t* d; uint32_t size; } Img;

static bool img_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  const Img* im = (const Img*)ctx;
  if ((uint64_t)off + len > im->size) return false;
  memcpy(dst, im->d + off, len);
  return true;
}

static uint8_t  im_u8 (const Img* im, uint32_t a) { return im->d[a - ROM_BASE]; }
static uint16_t im_u16(const Img* im, uint32_t a) {
  uint32_t o = a - ROM_BASE; return (uint16_t)(im->d[o] | (im->d[o + 1] << 8));
}
static uint32_t im_u32(const Img* im, uint32_t a) {
  uint32_t o = a - ROM_BASE;
  return (uint32_t)im->d[o] | ((uint32_t)im->d[o + 1] << 8) |
         ((uint32_t)im->d[o + 2] << 16) | ((uint32_t)im->d[o + 3] << 24);
}

/* ---- the independent re-derivation ----------------------------------------
 * Same PROPERTY as tools/gen_statics.py, different code path: the generator walks
 * gMapGroups itself from a hardcoded per-version address, while this walks it through
 * rom_map.c, which identifies the build on its own. If both land on the same span and
 * the same commands, neither is fitting the other's mistakes. */
typedef struct { uint16_t species; uint8_t level, kind; } Row;

static int row_cmp(const void* a, const void* b) {
  const Row* x = (const Row*)a; const Row* y = (const Row*)b;
  if (x->species != y->species) return x->species < y->species ? -1 : 1;
  if (x->level   != y->level)   return x->level   < y->level   ? -1 : 1;
  return (int)x->kind - (int)y->kind;
}

/* min/max over every script pointer a map header can reach: its own mapScripts plus the
 * script of every object / coord / background event. rom_map.c exposes the first three
 * directly; coord events are read here from the raw MapEvents fields (16 bytes each,
 * script at +0x08) because it has no accessor for them. */
static bool script_region(const RomCtx* rc, const Img* im, uint32_t* lo, uint32_t* hi,
                          int* nptr) {
  uint32_t mn = 0xFFFFFFFFu, mx = 0;
  int n = 0;
  for (int g = 0; g < rc->group_count; g++) {
    int maps = rom_maps_in_group(rc, g);
    for (int m = 0; m < maps; m++) {
      RomMapHeader h;
      if (!rom_map_header(rc, g, m, &h)) continue;
      uint32_t cand[1];
      cand[0] = h.scripts;
      for (unsigned k = 0; k < 1; k++)
        if (cand[k] && rom_ptr_ok(rc, cand[k])) {
          if (cand[k] < mn) mn = cand[k];
          if (cand[k] > mx) mx = cand[k];
          n++;
        }
      RomMapEvents ev;
      if (!rom_map_events(rc, h.events, &ev)) continue;
      for (int i = 0; i < ev.object_count; i++) {
        RomObjectEvent o;
        if (!rom_object_event(rc, &ev, i, &o)) continue;
        if (o.script && rom_ptr_ok(rc, o.script)) {
          if (o.script < mn) mn = o.script;
          if (o.script > mx) mx = o.script;
          n++;
        }
      }
      for (int i = 0; i < ev.coord_count; i++) {
        uint32_t a = ev.coords + (uint32_t)i * 16u + 8u;
        if (!rom_ptr_ok(rc, a + 3)) continue;
        uint32_t p = im_u32(im, a);
        if (p && rom_ptr_ok(rc, p)) {
          if (p < mn) mn = p;
          if (p > mx) mx = p;
          n++;
        }
      }
      for (int i = 0; i < ev.bg_count; i++) {
        RomBgEvent b;
        if (!rom_bg_event(rc, &ev, i, &b)) continue;
        /* `param` is a script pointer for signs and a plain item id for hidden items;
         * rom_ptr_ok rejects the latter, and a stray extra pointer could only WIDEN the
         * bound, never lose a real command. */
        if (b.param && rom_ptr_ok(rc, b.param)) {
          if (b.param < mn) mn = b.param;
          if (b.param > mx) mx = b.param;
          n++;
        }
      }
    }
  }
  *nptr = n;
  if (n < 500 || mn > mx) return false;
  *lo = mn; *hi = mx;
  return true;
}

static int scan(const Img* im, uint32_t lo, uint32_t hi, Row* out, int cap) {
  int n = 0;
  for (uint32_t a = lo; a <= hi && a + 15 < ROM_BASE + im->size; a++) {
    uint8_t op = im_u8(im, a);
    if (op != OP_SETWILDBATTLE && op != OP_GIVEMON) continue;
    uint16_t sp = im_u16(im, a + 1), item = im_u16(im, a + 4);
    uint8_t lvl = im_u8(im, a + 3);
    if (sp < 1 || sp > MAX_SPECIES || lvl < 1 || lvl > 100 || item > MAX_ITEM) continue;
    if (op == OP_GIVEMON) {
      bool zeros = true;                       /* the macro's nine trailing zero bytes */
      for (int k = 6; k < 15; k++) if (im_u8(im, a + (uint32_t)k)) { zeros = false; break; }
      if (!zeros) continue;
    }
    if (n < cap) {
      out[n].species = sp;
      out[n].level = lvl;
      out[n].kind = (uint8_t)(op == OP_GIVEMON ? PK_STATIC_GIFT : PK_STATIC_BATTLE);
      n++;
    }
  }
  return n;
}

static uint8_t game_of(RomKind k) {
  switch (k) {
    case ROM_SAPPHIRE:  return 1;
    case ROM_RUBY:      return 2;
    case ROM_EMERALD:   return 3;
    case ROM_FIRERED:   return 4;
    case ROM_LEAFGREEN: return 5;
    default:            return 0;
  }
}

static int g_empty_tables = 0;

static int run_rom(const char* path, const char* name) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  %-10s SKIP (no %s)\n", name, path); return 0; }
  uint8_t* buf = (uint8_t*)malloc(ROM_BYTES);
  if (!buf) { fclose(f); printf("  %-10s SKIP (out of memory)\n", name); return 0; }
  size_t got = fread(buf, 1, ROM_BYTES, f);
  fclose(f);
  if (got != ROM_BYTES) { free(buf); printf("  %-10s SKIP (not a 16 MiB dump)\n", name); return 0; }

  Img im = { buf, ROM_BYTES };
  RomCtx rc;
  if (!rom_open(&rc, img_read, &im, ROM_BYTES)) {
    printf("  !! FAIL: %s: rom_open refused the image\n", name);
    g_fail++; g_check++; free(buf); return 0;
  }
  uint8_t g = game_of(rc.kind);
  CHECK(g != 0, "%s: recognised as a Gen-3 game", name);
  if (!g) { free(buf); return 0; }
  if (pk_static_count(g) == 0) {
    printf("  %-10s SKIP (no table generated for this game)\n", name);
    g_empty_tables++;
    free(buf);
    return 0;
  }

  uint32_t lo = 0, hi = 0;
  int nptr = 0;
  if (!script_region(&rc, &im, &lo, &hi, &nptr)) {
    printf("  !! FAIL: %s: could not derive the event-script region (%d ptrs)\n", name, nptr);
    g_fail++; g_check++; free(buf); return 0;
  }

  static Row found[256];
  int nf = scan(&im, lo, hi, found, (int)(sizeof found / sizeof found[0]));
  qsort(found, (size_t)nf, sizeof found[0], row_cmp);
  /* dedupe to the same (species, level, kind) set the table stores */
  int nu = 0;
  for (int i = 0; i < nf; i++)
    if (!nu || row_cmp(&found[i], &found[nu - 1])) found[nu++] = found[i];

  /* (2a) every row the fresh scan found is in the shipped table */
  int missing = 0;
  for (int i = 0; i < nu; i++) {
    const PkStaticEntry* r = 0;
    int n = pk_static_list(g, found[i].species, &r), hit = 0;
    for (int k = 0; k < n; k++)
      if (r[k].level == found[i].level && r[k].kind == found[i].kind) hit = 1;
    if (!hit) {
      printf("  !! FAIL: %s: ROM has %s L%d kind %d, the table does not\n", name,
             pk_species_name(found[i].species), found[i].level, found[i].kind);
      missing++;
    }
  }
  CHECK(missing == 0, "%s: %d ROM placement(s) missing from the table", name, missing);

  /* (2b) ...and nothing the table claims is absent from the ROM. Both directions,
   * because a generator that emitted an extra row would pass (2a) alone — and an extra
   * row here means a species this checker silently stops judging. */
  int invented = 0, shipped = 0;
  for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++) {
    const PkStaticEntry* r = 0;
    int n = pk_static_list(g, sp, &r);
    for (int k = 0; k < n; k++) {
      shipped++;
      int hit = 0;
      for (int i = 0; i < nu; i++)
        if (found[i].species == sp && found[i].level == r[k].level &&
            found[i].kind == r[k].kind) hit = 1;
      if (!hit) {
        printf("  !! FAIL: %s: table claims %s L%d kind %d, the ROM does not\n", name,
               pk_species_name(sp), r[k].level, r[k].kind);
        invented++;
      }
    }
  }
  CHECK(invented == 0, "%s: %d table row(s) not present in the ROM", name, invented);
  CHECK(shipped == nu && shipped == pk_static_count(g),
        "%s: table has %d rows, the ROM scan found %d, pk_static_count says %d",
        name, shipped, nu, pk_static_count(g));

  /* (3) sortedness — pk_static_list walks the slice assuming it */
  const PkStaticEntry* prev = 0;
  int unsorted = 0;
  for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++) {
    const PkStaticEntry* r = 0;
    int n = pk_static_list(g, sp, &r);
    for (int k = 0; k < n; k++) {
      if (prev && (prev->species > r[k].species ||
                   (prev->species == r[k].species && prev->level > r[k].level))) unsorted++;
      prev = &r[k];
    }
  }
  CHECK(unsorted == 0, "%s: table is sorted by (species, level)", name);

  printf("  %-10s %-9s scripts %08X..%08X (%d ptrs)  %d row(s), scan agrees\n",
         name, rom_kind_name(rc.kind), lo, hi, nptr, shipped);
  free(buf);
  return 1;
}

/* ---- (4) ground truth ------------------------------------------------------ */
static void want(uint8_t g, const char* gn, uint16_t sp, uint8_t lvl, uint8_t kind) {
  if (pk_static_count(g) == 0) return;              /* that ROM was absent: nothing to say */
  const PkStaticEntry* r = 0;
  int n = pk_static_list(g, sp, &r), hit = 0;
  for (int k = 0; k < n; k++) if (r[k].level == lvl && r[k].kind == kind) hit = 1;
  CHECK(hit, "%s: %s should be placed at L%d (%s)", gn, pk_species_name(sp), lvl,
        kind == PK_STATIC_GIFT ? "givemon" : "setwildbattle");
}
static void want_absent(uint8_t g, const char* gn, uint16_t sp) {
  if (pk_static_count(g) == 0) return;
  CHECK(pk_static_any(g, sp) == PK_STATIC_NO, "%s: %s must NOT be script-placed",
        gn, pk_species_name(sp));
}

static void ground_truth(void) {
  /* THE BUG. Five of these, caught with the Devon Scope in normal play, were the false
   * positives that started all this. */
  want(1, "Sapphire", SP_KECLEON, 30, PK_STATIC_BATTLE);
  want(2, "Ruby",     SP_KECLEON, 30, PK_STATIC_BATTLE);
  want(3, "Emerald",  SP_KECLEON, 30, PK_STATIC_BATTLE);
  /* ...and the Kanto one: the Hypno that has Lostelle in Berry Forest, met at L30 in a
   * section whose wild rows for that line are L34-40. Same shape, different game. */
  want(4, "FireRed",   SP_HYPNO, 30, PK_STATIC_BATTLE);
  want(5, "LeafGreen", SP_HYPNO, 30, PK_STATIC_BATTLE);

  /* Facts a wrong opcode or a mis-derived script region could not fake — the version
   * exclusives. Ruby's Cave of Origin holds Groudon at L45 and no Kyogre; Sapphire is
   * the mirror; Emerald moved both to the roaming Terra/Marine Cave at L70. */
  want(2, "Ruby",     SP_GROUDON, 45, PK_STATIC_BATTLE);
  want_absent(2, "Ruby", SP_KYOGRE);
  want(1, "Sapphire", SP_KYOGRE, 45, PK_STATIC_BATTLE);
  want_absent(1, "Sapphire", SP_GROUDON);
  want(3, "Emerald",  SP_GROUDON, 70, PK_STATIC_BATTLE);
  want(3, "Emerald",  SP_KYOGRE,  70, PK_STATIC_BATTLE);
  want(3, "Emerald",  SP_SUDOWOODO, 40, PK_STATIC_BATTLE);   /* Emerald-only, Battle Frontier */
  want_absent(2, "Ruby", SP_SUDOWOODO);

  for (int g = 1; g <= 3; g++) {
    const char* gn = g == 1 ? "Sapphire" : g == 2 ? "Ruby" : "Emerald";
    want(g, gn, SP_REGIROCK,  40, PK_STATIC_BATTLE);
    want(g, gn, SP_REGICE,    40, PK_STATIC_BATTLE);
    want(g, gn, SP_REGISTEEL, 40, PK_STATIC_BATTLE);
    want(g, gn, SP_VOLTORB,   25, PK_STATIC_BATTLE);   /* New Mauville */
    want(g, gn, SP_BELDUM,     5, PK_STATIC_GIFT);     /* Steven's house */
  }
  want(4, "FireRed",   SP_MEWTWO, 70, PK_STATIC_BATTLE);
  want(5, "LeafGreen", SP_MEWTWO, 70, PK_STATIC_BATTLE);

  /* The control. An ordinary route Pokemon must NOT be in here, or the table would be
   * suppressing the checker everywhere instead of only where it must. */
  for (int g = 1; g <= 5; g++) {
    want_absent((uint8_t)g, "all", SP_POOCHYENA);
    want_absent((uint8_t)g, "all", SP_SKITTY);
  }
}

/* ---- (5) the regression, through the hook's own helper --------------------- */
static void regression(void) {
  if (!pk_static_have_data()) return;

  /* The exact question pk2_hook_encounter now asks before it is allowed to speak. */
  CHECK(pk2_line_scripted(3, SP_KECLEON) == PK_STATIC_YES,
        "Emerald KECLEON is script-placed, so the encounter hook must stay silent");
  CHECK(pk2_line_scripted(2, SP_KECLEON) == PK_STATIC_YES,
        "Ruby KECLEON likewise");
  CHECK(pk2_line_scripted(4, SP_HYPNO) == PK_STATIC_YES,
        "FireRed HYPNO likewise");

  /* THE CHAIN. New Mauville places a VOLTORB at L25; what its owner shows you may be an
   * ELECTRODE, whose met data still belongs to the Voltorb that was caught. A
   * species-only lookup would judge that Electrode by the wild table again. */
  CHECK(pk_static_any(3, SP_ELECTRODE) == PK_STATIC_YES &&
        pk2_line_scripted(3, SP_ELECTRODE) == PK_STATIC_YES,
        "Emerald ELECTRODE is covered");
  CHECK(pk2_line_scripted(4, SP_HYPNO) == PK_STATIC_YES &&
        pk_static_any(4, SP_DROWZEE) == PK_STATIC_NO,
        "FireRed HYPNO is placed directly, DROWZEE is not — the chain walks upward only");

  /* ...and the control: an ordinary species must still be judged. If this ever answers
   * YES the suppression has gone global and the whole check is dead. */
  CHECK(pk2_line_scripted(3, SP_POOCHYENA) == PK_STATIC_NO,
        "an ordinary route Pokemon is still judged");
  CHECK(pk2_line_scripted(3, SP_SKITTY) == PK_STATIC_NO,
        "an ordinary route Pokemon is still judged");

  /* How many species the table actually silences, printed rather than asserted — it is
   * the number to watch if a future generator change makes the scan sloppy. */
  for (int g = 1; g <= 5; g++) {
    if (!pk_static_count((uint8_t)g)) continue;
    int n = 0;
    for (uint16_t sp = 1; sp <= MAX_SPECIES; sp++)
      if (pk2_line_scripted((uint8_t)g, sp) == PK_STATIC_YES) n++;
    printf("  game %d: %d row(s) silence %d of %d species (chain included)\n",
           g, pk_static_count((uint8_t)g), n, MAX_SPECIES);
  }
}

/* ---- (6) the API edges ----------------------------------------------------- */
static void api_edges(void) {
  /* Out-of-range games answer NO_DATA, never NO: "I have no table" must not be readable
   * as "this species has no placement", which is what would let a corrupt origin byte
   * manufacture a verdict. */
  CHECK(pk_static_any(0, SP_KECLEON) == PK_STATIC_NO_DATA, "game 0 -> NO_DATA");
  CHECK(pk_static_any(6, SP_KECLEON) == PK_STATIC_NO_DATA, "game 6 -> NO_DATA");
  CHECK(pk_static_any(15, SP_KECLEON) == PK_STATIC_NO_DATA, "Colosseum origin -> NO_DATA");
  CHECK(pk2_line_scripted(0, SP_KECLEON) == PK_STATIC_NO_DATA, "line helper propagates NO_DATA");
  CHECK(pk_static_count(0) == 0 && pk_static_count(9) == 0, "no rows for an unknown game");
  CHECK(pk_static_list(0, SP_KECLEON, 0) == 0, "no slice for an unknown game");

  if (pk_static_count(3)) {
    /* Out-of-range species must be safe, not a read past the bitmap. */
    CHECK(pk_static_any(3, 0) == PK_STATIC_NO, "species 0 -> NO");
    CHECK(pk_static_any(3, 5000) == PK_STATIC_NO, "an absurd species id -> NO");
    CHECK(pk_static_at_level(3, SP_KECLEON, 30) == PK_STATIC_YES, "KECLEON L30 by level");
    CHECK(pk_static_at_level(3, SP_KECLEON, 25) == PK_STATIC_NO,
          "L25 is the WILD row, not a placement");
  }

  /* The arming gate. The encounter hook must need BOTH halves: with only the wild table
   * it cannot tell a Devon-Scope Kecleon from a forged one, and the whole point of this
   * change is that it stops guessing in that state. */
  CHECK(pk2_encounter_data_ok() == (pk_wild_have_data() && pk_static_have_data()),
        "the encounter hook arms on encounters.c AND statics.c");
}

/* The argument is a DIRECTORY of ROMs — but tests/run_host_tests.py hands every test
 * that mentions argv[1] the corpus .sav files instead, and a test whose strongest
 * assertion silently skips because of that is worse than no test. So: if the argument
 * names a readable FILE, use the directory it sits in. The saves and the ROMs live in
 * the same corpus folder, which is exactly why this works. */
static void rom_dir_from(const char* arg, char* out, size_t cap) {
  FILE* f = fopen(arg, "rb");
  const char* slash = strrchr(arg, '/');
  if (f && slash) {
    fclose(f);
    size_t n = (size_t)(slash - arg);
    if (n >= cap) n = cap - 1;
    memcpy(out, arg, n);
    out[n] = 0;
    return;
  }
  if (f) fclose(f);
  snprintf(out, cap, "%s", arg);
}

int main(int argc, char** argv) {
  char dirbuf[512];
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) { rom_dir_from(argv[1], dirbuf, sizeof dirbuf); dir = dirbuf; }
  printf("== script-placement table ==\n");
  printf("  statics.c: %s (S %d / R %d / E %d / FR %d / LG %d rows)\n",
         pk_static_have_data() ? "generated" : "ABSENT",
         pk_static_count(1), pk_static_count(2), pk_static_count(3),
         pk_static_count(4), pk_static_count(5));

  static const char* names[] = { "Emerald", "Ruby", "Sapphire", "FireRed", "LeafGreen" };
  int ran = 0;
  char path[512];
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
    snprintf(path, sizeof path, "%s/%s.gba", dir, names[i]);
    ran += run_rom(path, names[i]);
  }
  if (!ran) printf("  (no ROMs under %s — the table was checked against nothing)\n", dir);

  printf("\n-- ground truth --\n");
  ground_truth();
  printf("\n-- the regression this table closes --\n");
  regression();
  printf("\n-- API edges --\n");
  api_edges();

  if (g_empty_tables) {
    printf("!! FAIL: %d game(s) had a ROM but an EMPTY table — regenerate with\n"
           "         python3 tools/gen_statics.py --from-rom\n", g_empty_tables);
    g_fail += g_empty_tables;
  }
  printf("\nstatics test: %d checks, %d failure(s)%s\n", g_check, g_fail,
         ran == 5 ? "" : "  [some ROMs skipped]");
  return g_fail ? 1 : 0;
}
