/* Host test for the generated wild-encounter tables (source/encounters.c, built by
 * tools/gen_encounters.py --from-rom).
 *   cc -std=c11 -I source tests/host_encounters_test.c source/encounters.c \
 *      source/rom_map.c -o /tmp/he
 *   /tmp/he                       # ROMs from the default corpus directory
 *   /tmp/he /path/to/rom/dir
 *
 * The generator is Python; this test is the INDEPENDENT re-derivation. It locates
 * gWildMonHeaders in each of the five retail dumps again — in C, by the same shape rule
 * but through a second implementation — walks every header, and demands an exact
 * bijection with what encounters.c reports. A generator bug that silently drops, merges
 * or shifts a row cannot survive that; a table that merely "looks plausible" would.
 *
 * What it asserts:
 *   1) every ROM's table is locatable and parses: entry count sane, mapGroup/mapNum
 *      inside gMapGroups, every WildPokemonInfo pointer either NULL or in-bounds with
 *      its whole slot array in-bounds, encounterRate 1..100;
 *   2) every slot is sane: species 1..411, levels 1..100, min <= max;
 *   3) the shipped table == the ROM, exactly: same set of (mapsec, species) rows, same
 *      merged level window, same method mask — in both directions;
 *   4) the shipped table is sorted by (mapsec, species), so the binary search is valid;
 *   5) ground truth from the decomps (Route 101 in Emerald, Route 1 in FR/LG);
 *   6) Ruby and Sapphire really DIFFER: the Seedot line is Ruby-only and the Lotad line
 *      is Sapphire-only, which is the whole reason they ship as separate tables;
 *   7) the API's no-data / out-of-range answers are the conservative ones.
 *
 * ROMs are Guy's own cartridge dumps and are never part of this repo; a missing ROM
 * SKIPS its game rather than failing, so the suite still runs without them. Likewise
 * an absent source/encounters.c: the weak fallbacks in encounters.h keep the link
 * working and the test then only exercises the no-data contract.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "encounters.h"
#include "rom_map.h"

static int g_fail = 0, g_check = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); \
  printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define NSPECIES   412              /* 1..411 are real; 412 is SPECIES_EGG */
#define NMAPSEC    256
#define HDR_SIZE   20
#define ROM_BYTES  (16u * 1024u * 1024u)

/* Gen-3 internal species ids (reference/pokeemerald_data/include/constants/species.h).
 * Kanto keeps its national numbering; the Hoenn block is reshuffled. */
#define SP_CATERPIE   10
#define SP_PIDGEY     16
#define SP_RATTATA    19
#define SP_POOCHYENA 286
#define SP_ZIGZAGOON 288
#define SP_WURMPLE   290
#define SP_LOTAD     295
#define SP_LOMBRE    296
#define SP_SEEDOT    298
#define SP_NUZLEAF   299
#define SP_SABLEYE   322
#define SP_LUNATONE  348
#define SP_SOLROCK   349
#define SP_MAWILE    355
#define SP_SEVIPER   379
#define SP_ZANGOOSE  380

/* MAPSEC values (reference/pokeemerald_data/include/constants/region_map_sections.h).
 * Emerald's table carries the Kanto sections too, because it has to display FRLG-origin
 * met locations — which is exactly why one MAPSEC space covers all five games. */
#define MS_ROUTE_101 0x10
#define MS_ROUTE_102 0x11
#define MS_ROUTE_1   0x65

static const uint8_t SLOTS[4] = { 12, 5, 5, 10 };     /* land / water / rock / fishing */

/* ---- ROM access ----------------------------------------------------------- */
typedef struct { const uint8_t* d; uint32_t size; } Img;

static bool img_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  const Img* im = (const Img*)ctx;
  if (off > im->size || len > im->size - off) return false;
  memcpy(dst, im->d + off, len);
  return true;
}

static uint16_t g16(const Img* im, uint32_t a) {
  uint32_t o = a - ROM_BASE;
  return (uint16_t)(im->d[o] | (im->d[o + 1] << 8));
}
static uint32_t g32(const Img* im, uint32_t a) {
  uint32_t o = a - ROM_BASE;
  return (uint32_t)im->d[o] | ((uint32_t)im->d[o + 1] << 8) |
         ((uint32_t)im->d[o + 2] << 16) | ((uint32_t)im->d[o + 3] << 24);
}
static bool in_rom(const Img* im, uint32_t a, uint32_t len) {
  if (a < ROM_BASE) return false;
  uint32_t o = a - ROM_BASE;
  return o <= im->size && len <= im->size - o;
}

/* A WildPokemonInfo { u8 encounterRate; u8 pad[3]; const struct WildPokemon *slots; }
 * whose whole slot array is in bounds and plausible. Deliberately strict: this
 * predicate is the only thing separating the real table from any other run of pointers
 * in 16 MiB. */
static bool info_ok(const Img* im, uint32_t a, int n) {
  if (!in_rom(im, a, 8) || (a & 3)) return false;
  uint8_t rate = im->d[a - ROM_BASE];
  if (rate < 1 || rate > 100) return false;
  if (im->d[a - ROM_BASE + 1] || im->d[a - ROM_BASE + 2] || im->d[a - ROM_BASE + 3]) return false;
  uint32_t slots = g32(im, a + 4);
  if (!in_rom(im, slots, (uint32_t)n * 4) || (slots & 1)) return false;
  for (int i = 0; i < n; i++) {
    uint8_t lo = im->d[slots - ROM_BASE + i * 4], hi = im->d[slots - ROM_BASE + i * 4 + 1];
    uint16_t sp = g16(im, slots + i * 4 + 2);
    if (lo < 1 || lo > 100 || hi < 1 || hi > 100 || lo > hi) return false;
    if (sp < 1 || sp > 411) return false;
  }
  return true;
}

static bool entry_ok(const Img* im, const RomCtx* rc, uint32_t a) {
  if (!in_rom(im, a, HDR_SIZE)) return false;
  uint8_t grp = im->d[a - ROM_BASE], num = im->d[a - ROM_BASE + 1];
  if (g16(im, a + 2) != 0) return false;                 /* the struct's alignment pad */
  int cnt = rom_maps_in_group(rc, grp);
  if (cnt <= 0 || num >= cnt) return false;
  int live = 0;
  for (int i = 0; i < 4; i++) {
    uint32_t p = g32(im, a + 4 + 4 * i);
    if (!p) continue;
    if (!info_ok(im, p, SLOTS[i])) return false;
    live++;
  }
  return live > 0;                       /* four NULLs is not an encounter table */
}

/* Find gWildMonHeaders by shape: the 20-byte terminator (mapGroup 0xFF and nothing
 * else set — the game breaks on `mapGroup == MAP_GROUP(MAP_UNDEFINED)`, pokeemerald
 * src/wild_encounter.c:312), then walk backwards while entries validate. */
static uint32_t find_table(const Img* im, const RomCtx* rc, int* out_n) {
  uint32_t found = 0;
  int found_n = 0, hits = 0;
  for (uint32_t o = 0; o + HDR_SIZE <= im->size; o += 4) {
    if (im->d[o] != 0xFF || im->d[o + 1] != 0xFF) continue;
    bool zero = true;
    for (int i = 2; i < HDR_SIZE; i++) if (im->d[o + i]) { zero = false; break; }
    if (!zero) continue;
    int n = 0;
    int64_t p = (int64_t)o - HDR_SIZE;
    while (p >= 0 && entry_ok(im, rc, ROM_BASE + (uint32_t)p)) { n++; p -= HDR_SIZE; }
    if (n >= 30) { hits++; found = ROM_BASE + (uint32_t)(p + HDR_SIZE); found_n = n; }
  }
  CHECK(hits == 1, "exactly one gWildMonHeaders-shaped table (found %d)", hits);
  *out_n = found_n;
  return hits == 1 ? found : 0;
}

/* ---- the merged grid: [mapsec][species] -> {min,max,methods,present} ------- */
typedef struct { uint8_t lo, hi, methods, present; } Cell;

static PkEncGame game_of(RomKind k) {
  switch (k) {
    case ROM_SAPPHIRE:  return PK_ENC_SAPPHIRE;
    case ROM_RUBY:      return PK_ENC_RUBY;
    case ROM_EMERALD:   return PK_ENC_EMERALD;
    case ROM_FIRERED:   return PK_ENC_FIRERED;
    case ROM_LEAFGREEN: return PK_ENC_LEAFGREEN;
    default:            return (PkEncGame)0;
  }
}

/* games whose ROM was present but whose generated table was EMPTY — a silent
 * regeneration failure that used to slip through as a harmless SKIP. */
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
  PkEncGame g = game_of(rc.kind);
  CHECK(g != 0, "%s: recognised as a Gen-3 game", name);
  if (!g) { free(buf); return 0; }
  /* This ROM is present now but was not when the tables were generated. Comparing
   * would only report the absence 400 times over; the generator already said so. */
  if (pk_wild_count(g) == 0) {
    printf("  %-10s SKIP (no table generated for this game)\n", name);
    g_empty_tables++;
    free(buf); return 0;
  }

  int n = 0;
  uint32_t tbl = find_table(&im, &rc, &n);
  if (!tbl) { free(buf); return 0; }
  CHECK(n >= 90 && n <= 200, "%s: %d headers is in the retail range 90..200", name, n);

  Cell* grid = (Cell*)calloc((size_t)NMAPSEC * NSPECIES, sizeof(Cell));
  if (!grid) { free(buf); printf("  %-10s SKIP (out of memory)\n", name); return 0; }

  int slots_seen = 0, mapsecs = 0;
  for (int e = 0; e < n; e++) {
    uint32_t h = tbl + (uint32_t)e * HDR_SIZE;
    uint8_t grp = im.d[h - ROM_BASE], num = im.d[h - ROM_BASE + 1];
    RomMapHeader mh;
    if (!rom_map_header(&rc, grp, num, &mh)) {
      CHECK(0, "%s: header %d maps to %d.%d, which is not a map", name, e, grp, num);
      continue;
    }
    for (int i = 0; i < 4; i++) {
      uint32_t p = g32(&im, h + 4 + 4 * i);
      if (!p) continue;
      CHECK(in_rom(&im, p, 8), "%s: entry %d field %d pointer %08X in bounds", name, e, i, p);
      uint32_t sl = g32(&im, p + 4);
      CHECK(in_rom(&im, sl, (uint32_t)SLOTS[i] * 4),
            "%s: entry %d field %d slot array %08X in bounds", name, e, i, sl);
      uint8_t rate = im.d[p - ROM_BASE];
      CHECK(rate >= 1 && rate <= 100, "%s: entry %d field %d rate %d in 1..100",
            name, e, i, rate);
      for (int s = 0; s < SLOTS[i]; s++) {
        uint8_t lo = im.d[sl - ROM_BASE + s * 4], hi = im.d[sl - ROM_BASE + s * 4 + 1];
        uint16_t sp = g16(&im, sl + s * 4 + 2);
        CHECK(sp >= 1 && sp <= 411, "%s: entry %d field %d slot %d species %d in 1..411",
              name, e, i, s, sp);
        CHECK(lo >= 1 && lo <= 100 && hi >= 1 && hi <= 100 && lo <= hi,
              "%s: entry %d field %d slot %d levels %d..%d sane", name, e, i, s, lo, hi);
        if (sp < 1 || sp >= NSPECIES) continue;
        Cell* c = &grid[(size_t)mh.mapsec * NSPECIES + sp];
        if (!c->present) { c->present = 1; c->lo = lo; c->hi = hi; }
        else { if (lo < c->lo) c->lo = lo; if (hi > c->hi) c->hi = hi; }
        c->methods |= (uint8_t)(1u << i);
        slots_seen++;
      }
    }
  }

  /* (3a) every row the ROM has, the shipped table has — with identical merge. */
  int rows = 0;
  for (int ms = 0; ms < NMAPSEC; ms++) {
    bool any = false;
    for (int sp = 0; sp < NSPECIES; sp++) {
      Cell* c = &grid[(size_t)ms * NSPECIES + sp];
      if (!c->present) continue;
      any = true; rows++;
      PkWildEntry got;
      int r = pk_wild_at(g, (uint8_t)ms, (uint16_t)sp, &got);
      if (r != PK_WILD_YES) {
        CHECK(0, "%s: mapsec %02X species %d is in the ROM but not in the table (r=%d)",
              name, ms, sp, r);
        continue;
      }
      CHECK(got.minlvl == c->lo && got.maxlvl == c->hi && got.methods == c->methods,
            "%s: mapsec %02X species %d table {%d..%d,m%X} vs ROM {%d..%d,m%X}",
            name, ms, sp, got.minlvl, got.maxlvl, got.methods, c->lo, c->hi, c->methods);
    }
    if (any) mapsecs++;
  }

  /* (3b) ...and nothing else. Iterating the per-mapsec slices visits every row exactly
   * once, so equal totals plus 3a is a bijection. */
  CHECK(pk_wild_count(g) == rows, "%s: table has %d rows, ROM has %d",
        name, pk_wild_count(g), rows);
  int listed = 0, slices = 0;
  const PkWildEntry* prev_end = 0;
  for (int ms = 0; ms < NMAPSEC; ms++) {
    const PkWildEntry* p = 0;
    int k = pk_wild_mapsec_list(g, (uint8_t)ms, &p);
    if (k == 0) {
      /* an empty slice must mean the ROM has nothing there either */
      for (int sp = 0; sp < NSPECIES; sp++)
        CHECK(!grid[(size_t)ms * NSPECIES + sp].present,
              "%s: mapsec %02X slice is empty but the ROM has species %d", name, ms, sp);
      continue;
    }
    slices++;
    CHECK(p != 0, "%s: mapsec %02X reports %d rows but no pointer", name, ms, k);
    /* (4) sorted, ascending species within the slice and ascending mapsec across them */
    CHECK(prev_end == 0 || p >= prev_end, "%s: mapsec %02X slice is out of order", name, ms);
    prev_end = p + k;
    for (int i = 0; i < k; i++) {
      CHECK(p[i].mapsec == ms, "%s: slice %02X row %d has mapsec %02X", name, ms, i, p[i].mapsec);
      if (i) CHECK(p[i].species > p[i - 1].species,
                   "%s: slice %02X not sorted by species at %d", name, ms, i);
      CHECK(p[i].species >= 1 && p[i].species <= 411,
            "%s: slice %02X row %d species %d in range", name, ms, i, p[i].species);
      CHECK(p[i].minlvl >= 1 && p[i].maxlvl <= 100 && p[i].minlvl <= p[i].maxlvl,
            "%s: slice %02X row %d levels %d..%d sane", name, ms, i, p[i].minlvl, p[i].maxlvl);
      CHECK(p[i].methods != 0 && p[i].methods <= 0x0F,
            "%s: slice %02X row %d method mask %02X", name, ms, i, p[i].methods);
      Cell* c = &grid[(size_t)ms * NSPECIES + p[i].species];
      CHECK(c->present, "%s: table row mapsec %02X species %d is not in the ROM",
            name, ms, p[i].species);
      listed++;
    }
  }
  CHECK(listed == rows, "%s: per-mapsec slices cover %d rows, expected %d", name, listed, rows);
  CHECK(slices == mapsecs, "%s: %d non-empty slices, ROM has %d sections", name, slices, mapsecs);

  /* the flat fast path must agree with the detailed one */
  int any_yes = 0;
  for (int sp = 1; sp <= 411; sp++) {
    bool in_grid = false;
    for (int ms = 0; ms < NMAPSEC && !in_grid; ms++)
      if (grid[(size_t)ms * NSPECIES + sp].present) in_grid = true;
    int r = pk_wild_anywhere(g, (uint16_t)sp);
    CHECK(r == (in_grid ? PK_WILD_YES : PK_WILD_NO),
          "%s: pk_wild_anywhere(%d) = %d, ROM says %d", name, sp, r, (int)in_grid);
    if (in_grid) any_yes++;
    CHECK(((pk_wild_games((uint16_t)sp, 0) >> (g - 1)) & 1) == (in_grid ? 1 : 0),
          "%s: pk_wild_games(%d) bit disagrees", name, sp);
  }

  printf("  %-10s %s rev %u  %3d headers  %4d slots  %4d rows  %3d sections  %3d species  ok\n",
         name, rom_kind_name(rc.kind), rc.version, n, slots_seen, rows, mapsecs, any_yes);
  free(grid);
  free(buf);
  return 1;
}

/* ---- ground truth --------------------------------------------------------- */
static void expect(PkEncGame g, uint8_t ms, uint16_t sp, int lo, int hi, unsigned meth,
                   const char* what) {
  PkWildEntry e;
  int r = pk_wild_at(g, ms, sp, &e);
  if (r != PK_WILD_YES) { CHECK(0, "%s: not present (r=%d)", what, r); return; }
  CHECK(e.minlvl == lo && e.maxlvl == hi && e.methods == meth,
        "%s: got %d..%d m%X, want %d..%d m%X", what, e.minlvl, e.maxlvl, e.methods,
        lo, hi, meth);
}

static void ground_truth(void) {
  /* Emerald Route 101 — three land-only species, all level 2-3. From
   * pokeemerald src/data/wild_encounters.json, entry gRoute101. */
  if (pk_wild_count(PK_ENC_EMERALD)) {
    expect(PK_ENC_EMERALD, MS_ROUTE_101, SP_POOCHYENA, 2, 3, PK_ENC_LAND, "E Route101 Poochyena");
    expect(PK_ENC_EMERALD, MS_ROUTE_101, SP_ZIGZAGOON, 2, 3, PK_ENC_LAND, "E Route101 Zigzagoon");
    expect(PK_ENC_EMERALD, MS_ROUTE_101, SP_WURMPLE,   2, 3, PK_ENC_LAND, "E Route101 Wurmple");
    const PkWildEntry* p = 0;
    CHECK(pk_wild_mapsec_list(PK_ENC_EMERALD, MS_ROUTE_101, &p) == 3,
          "E Route101 holds exactly 3 species");
    /* Route 101 has no water: Poochyena's Sapphire/Ruby neighbour Lotad is not there */
    CHECK(pk_wild_at(PK_ENC_EMERALD, MS_ROUTE_101, SP_LOTAD, 0) == PK_WILD_NO,
          "E Route101 has no Lotad");
  }

  /* FR/LG Route 1 — Pidgey 2-5, Rattata 2-4 (pokefirered wild_encounters.json,
   * sRoute1_FireRed / sRoute1_LeafGreen; the twelve slots merge to those windows). */
  for (int i = 0; i < 2; i++) {
    PkEncGame g = i ? PK_ENC_LEAFGREEN : PK_ENC_FIRERED;
    const char* nm = i ? "LG" : "FR";
    if (!pk_wild_count(g)) continue;
    char buf[64];
    snprintf(buf, sizeof buf, "%s Route1 Pidgey", nm);
    expect(g, MS_ROUTE_1, SP_PIDGEY, 2, 5, PK_ENC_LAND, buf);
    snprintf(buf, sizeof buf, "%s Route1 Rattata", nm);
    expect(g, MS_ROUTE_1, SP_RATTATA, 2, 4, PK_ENC_LAND, buf);
    CHECK(pk_wild_at(g, MS_ROUTE_1, SP_CATERPIE, 0) == PK_WILD_NO,
          "%s Route1 has no Caterpie", nm);
  }

  /* Ruby vs Sapphire MUST differ — this is the entire justification for shipping two
   * tables instead of one merged "RS" (OVERNIGHT-DECISIONS.md item 5). Route 102 is
   * where the split first shows: Seedot in Ruby, Lotad in Sapphire. */
  if (pk_wild_count(PK_ENC_RUBY) && pk_wild_count(PK_ENC_SAPPHIRE)) {
    expect(PK_ENC_RUBY, MS_ROUTE_102, SP_SEEDOT, 3, 4, PK_ENC_LAND, "R Route102 Seedot");
    expect(PK_ENC_SAPPHIRE, MS_ROUTE_102, SP_LOTAD, 3, 4, PK_ENC_LAND, "S Route102 Lotad");
    CHECK(pk_wild_at(PK_ENC_RUBY, MS_ROUTE_102, SP_LOTAD, 0) == PK_WILD_NO,
          "R Route102 has no Lotad");
    CHECK(pk_wild_at(PK_ENC_SAPPHIRE, MS_ROUTE_102, SP_SEEDOT, 0) == PK_WILD_NO,
          "S Route102 has no Seedot");
    /* ...and game-wide, not just on one route. */
    static const uint16_t ruby_only[] = { SP_SEEDOT, SP_NUZLEAF, SP_MAWILE, SP_SOLROCK, SP_ZANGOOSE };
    static const uint16_t sapp_only[] = { SP_LOTAD, SP_LOMBRE, SP_SABLEYE, SP_LUNATONE, SP_SEVIPER };
    for (unsigned i = 0; i < sizeof ruby_only / sizeof ruby_only[0]; i++) {
      CHECK(pk_wild_anywhere(PK_ENC_RUBY, ruby_only[i]) == PK_WILD_YES,
            "species %d is wild in Ruby", ruby_only[i]);
      CHECK(pk_wild_anywhere(PK_ENC_SAPPHIRE, ruby_only[i]) == PK_WILD_NO,
            "species %d is NOT wild in Sapphire", ruby_only[i]);
      CHECK(pk_wild_anywhere(PK_ENC_SAPPHIRE, sapp_only[i]) == PK_WILD_YES,
            "species %d is wild in Sapphire", sapp_only[i]);
      CHECK(pk_wild_anywhere(PK_ENC_RUBY, sapp_only[i]) == PK_WILD_NO,
            "species %d is NOT wild in Ruby", sapp_only[i]);
    }
    CHECK(pk_wild_games(SP_SEEDOT, 0) & (1u << (PK_ENC_RUBY - 1)), "Seedot's games include Ruby");
    CHECK(!(pk_wild_games(SP_SEEDOT, 0) & (1u << (PK_ENC_SAPPHIRE - 1))),
          "Seedot's games exclude Sapphire");
  }

  /* Legendaries are never wild-slot encounters in any game — a cheap sanity net on the
   * "is it wild-obtainable at all" fast path. */
  if (pk_wild_have_data())
    CHECK(pk_wild_games(150 /* Mewtwo */, 0) == 0, "Mewtwo is in no wild table");
    { /* the tri-state must survive the bitmask API: a game with no table reports
       * UNKNOWN, never a clear "not obtainable" bit (that is how a missing table
       * would manufacture a legality verdict). */
      uint8_t unk = 0xFF;
      (void)pk_wild_games(SP_SEEDOT, &unk);
      uint8_t want = 0;
      for (int g = PK_ENC_SAPPHIRE; g <= PK_ENC_LEAFGREEN; g++)
        if (pk_wild_anywhere((PkEncGame)g, SP_SEEDOT) == PK_WILD_NO_DATA) want |= (uint8_t)(1u << (g - 1));
      CHECK(unk == want, "pk_wild_games unknown mask = the no-table games (got %02X want %02X)", unk, want);
    }
}

/* ---- API edge cases ------------------------------------------------------- */
static void api_edges(void) {
  PkWildEntry e;
  CHECK(pk_wild_at((PkEncGame)0, MS_ROUTE_101, SP_POOCHYENA, &e) == PK_WILD_NO_DATA,
        "game 0 -> NO_DATA, never a verdict");
  CHECK(pk_wild_at((PkEncGame)15, MS_ROUTE_101, SP_POOCHYENA, &e) == PK_WILD_NO_DATA,
        "origin 15 (Colosseum/XD) -> NO_DATA");
  CHECK(pk_wild_anywhere((PkEncGame)0, SP_POOCHYENA) == PK_WILD_NO_DATA,
        "anywhere(game 0) -> NO_DATA");
  CHECK(pk_wild_count((PkEncGame)0) == 0 && pk_wild_count((PkEncGame)15) == 0,
        "count of an unknown game is 0");
  const PkWildEntry* p = (const PkWildEntry*)1;
  CHECK(pk_wild_mapsec_list((PkEncGame)0, MS_ROUTE_101, &p) == 0,
        "mapsec_list of an unknown game is empty");
  if (pk_wild_count(PK_ENC_EMERALD)) {
    CHECK(pk_wild_anywhere(PK_ENC_EMERALD, 0) == PK_WILD_NO, "species 0 -> NO");
    CHECK(pk_wild_anywhere(PK_ENC_EMERALD, 9999) == PK_WILD_NO, "species 9999 -> NO");
    CHECK(pk_wild_at(PK_ENC_EMERALD, 0xFF, SP_POOCHYENA, &e) == PK_WILD_NO,
          "met location 0xFF has no wild data");
    CHECK(pk_wild_mapsec_list(PK_ENC_EMERALD, 0xFF, &p) == 0, "mapsec FF is an empty slice");
  }
}


int main(int argc, char** argv) {
  /* run_host_tests.py hands every argv[1]-reading test the .sav corpus; this one wants
   * the ROM DIRECTORY, so only accept an argument that is not a save. */
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) {
    size_t l = strlen(argv[1]);
    if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1];
  }
  printf("== wild encounters (tables from tools/gen_encounters.py --from-rom) ==\n");
  if (!pk_wild_have_data()) {
    printf("  source/encounters.c holds no rows — run tools/gen_encounters.py --from-rom\n");
    api_edges();
    printf("encounters test: %d checks, %d failure(s)\n", g_check, g_fail);
    return g_fail ? 1 : 0;
  }

  static const char* names[] = { "Emerald", "Ruby", "Sapphire", "FireRed", "LeafGreen" };
  int ran = 0;
  char path[512];
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
    snprintf(path, sizeof path, "%s/%s.gba", dir, names[i]);
    ran += run_rom(path, names[i]);
  }
  if (!ran) printf("  (no ROMs under %s — the table was checked against nothing)\n", dir);

  ground_truth();
  api_edges();
  /* A present ROM whose table is EMPTY is a silent regeneration failure, not a skip:
   * without this the suite stayed green while the shipped data vanished. (Missing ROMs
   * are a legitimate skip — they are Guy's own dumps and are not in the repo.) */
  if (g_empty_tables) {
    printf("!! FAIL: %d game(s) had a ROM but an EMPTY table — regenerate with\n"
           "         python3 tools/gen_encounters.py --from-rom\n", g_empty_tables);
    g_fail += g_empty_tables;
  }
  printf("encounters test: %d checks, %d failure(s)%s\n", g_check, g_fail,
         ran == 5 ? "" : "  [some ROMs skipped]");
  return g_fail ? 1 : 0;
}
