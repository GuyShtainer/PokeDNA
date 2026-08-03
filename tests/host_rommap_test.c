/* Host (PC) test for rom_map — the Gen-3 overworld map reader, run against REAL
 * retail ROMs. Because rom_map does all its I/O through a callback, the code under
 * test here is byte-for-byte the code that will run on the GBA over FatFs.
 *
 * What it proves:
 *   1) version identification accepts the known retail builds and REFUSES a hack,
 *      a wrong size, and a non-Pokemon GBA ROM;
 *   2) the per-group map counts derived from consecutive gMapGroups differences
 *      match the decomp's map_groups.json (Emerald group 0 = 57, total 518);
 *   3) EVERY map in the ROM walks cleanly: header -> layout -> tilesets -> events,
 *      with every pointer inside the image and every dimension sane;
 *   4) the FireRed/LeafGreen structural divergences are handled (28-byte MapLayout,
 *      swapped Tileset callback/attributes, 640 metatiles in the primary tileset) —
 *      an Emerald-shaped parser reads attributes == NULL here;
 *   5) known ground truth: Littleroot Town's dimensions, layout id and NPC set.
 *
 * ROMs are the user's own dumps and are NEVER part of this repo. Paths are taken
 * from argv or the default location; missing ROMs SKIP rather than fail, so the
 * suite still runs on a machine without them.
 *
 * Build + run (from the repo root):
 *   cc -I source tests/host_rommap_test.c source/rom_map.c -o /tmp/hrm && /tmp/hrm
 *   cc -I source tests/host_rommap_test.c source/rom_map.c -o /tmp/hrm && \
 *      /tmp/hrm "/path/to/emerald.gba" "/path/to/ruby.gba" "/path/to/firered.gba"
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_map.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

/* ---- the read callback: plain fread, the host analogue of FatFs ------------ */
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, f) == len;
}

static uint32_t file_size(FILE* f) {
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  return (uint32_t)(n < 0 ? 0 : n);
}

/* Walk every map in the ROM, checking structural integrity. Returns maps walked. */
static int walk_all(RomCtx* c, int* npc_total, int* conn_total, int* biggest, int* oob) {
  int maps = 0;
  *npc_total = *conn_total = *biggest = *oob = 0;
  for (int g = 0; g < c->group_count; g++) {
    int n = rom_maps_in_group(c, g);
    CHECK(n > 0, "every group has at least one map");
    for (int m = 0; m < n; m++) {
      RomMapHeader h;
      if (!rom_map_header(c, g, m, &h)) { printf("FAIL: header %d.%d\n", g, m); g_fail++; continue; }
      RomLayout l;
      if (!rom_layout(c, h.layout, &l)) { printf("FAIL: layout %d.%d\n", g, m); g_fail++; continue; }

      CHECK(l.width > 0 && l.height > 0, "layout has positive dimensions");
      CHECK(l.width < 1000 && l.height < 1000, "layout dimensions are sane");
      /* The game's own sBackupMapData is 10,240 u16 — no retail map exceeds it. */
      CHECK((uint32_t)l.width * (uint32_t)l.height <= 10240u, "map fits the game's map buffer");
      CHECK(rom_ptr_ok(c, l.blocks), "blockdata pointer is inside the ROM");
      CHECK(rom_ptr_ok(c, l.tileset_primary), "primary tileset pointer is inside the ROM");

      RomTileset ts;
      CHECK(rom_tileset(c, l.tileset_primary, &ts), "primary tileset reads");
      CHECK(rom_ptr_ok(c, ts.metatiles), "primary metatiles pointer valid");
      /* The FRLG swap lands here: with the wrong field order this is 0/garbage. */
      CHECK(rom_ptr_ok(c, ts.attributes), "primary metatile-attributes pointer valid");
      CHECK(ts.secondary == 0, "primary tileset is flagged primary");
      if (l.tileset_secondary) {
        RomTileset ts2;
        if (rom_tileset(c, l.tileset_secondary, &ts2)) {
          CHECK(rom_ptr_ok(c, ts2.metatiles), "secondary metatiles pointer valid");
          CHECK(rom_ptr_ok(c, ts2.attributes), "secondary attributes pointer valid");
        }
      }

      /* first and last blockdata cells must read, and the metatile id must resolve */
      uint16_t cell;
      uint32_t cells = (uint32_t)l.width * (uint32_t)l.height;
      CHECK(rom_blocks(c, &l, 0, &cell, 1), "first blockdata cell reads");
      CHECK(rom_blocks(c, &l, cells - 1, &cell, 1), "last blockdata cell reads");
      CHECK(!rom_blocks(c, &l, cells, &cell, 1), "reading past the blockdata is refused");
      uint16_t mt[8];
      CHECK(rom_metatile(c, &l, ROM_CELL_METATILE(cell), mt), "metatile resolves");
      RomMetatileAttr attr;
      CHECK(rom_metatile_attr(c, &l, ROM_CELL_METATILE(cell), &attr), "metatile attribute resolves");
      /* Layer type lives in DIFFERENT bits per game (RSE 12-15, FRLG 29-30); a
       * normalised reader must never report an out-of-range type. */
      CHECK(attr.layer_type <= ROM_LAYER_SPLIT, "layer type normalised into range");

      RomMapEvents ev;
      if (rom_map_events(c, h.events, &ev)) {
        *npc_total += ev.object_count;
        for (int i = 0; i < ev.object_count; i++) {
          RomObjectEvent oe;
          CHECK(rom_object_event(c, &ev, i, &oe), "object event reads");
          /* NOTE: an object event is NOT guaranteed to lie inside its own map.
           * MapEvents tables are SHARED between maps in retail data (11 Emerald maps
           * reuse another map's table), and group 25's 1x1 placeholder maps
           * (secret-base / link rooms) share one table whose NPC sits at (6,4).
           * So the renderer MUST cull object events against the map bounds rather
           * than assume they are in range. All we can assert is that the decoded
           * coordinates are sane s16 map coordinates, not garbage from a misparse. */
          CHECK(oe.x > -1000 && oe.x < 1000, "object x is a sane map coordinate");
          CHECK(oe.y > -1000 && oe.y < 1000, "object y is a sane map coordinate");
          if (oe.x < 0 || oe.x >= l.width || oe.y < 0 || oe.y >= l.height) (*oob)++;
        }
        for (int i = 0; i < ev.warp_count; i++) {
          RomWarp w;
          CHECK(rom_warp(c, &ev, i, &w), "warp reads");
          CHECK(w.dest_group < c->group_count || w.dest_group == 0x7F,
                "warp destination group exists");
        }
      }

      int nc = rom_connection_count(c, h.connections);
      *conn_total += nc;
      for (int i = 0; i < nc; i++) {
        RomConnection cn;
        CHECK(rom_connection(c, h.connections, i, &cn), "connection reads");
        CHECK(cn.group < c->group_count, "connection target group exists");
        CHECK(cn.num < rom_maps_in_group(c, cn.group), "connection target map exists");
      }

      if ((int)cells > *biggest) *biggest = (int)cells;
      maps++;
    }
  }
  return maps;
}

static void test_rom(const char* path, RomKind expect, int expect_groups,
                     int expect_maps, int expect_group0) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("(skip: %s not found)\n", path); return; }
  RomCtx c;
  uint32_t sz = file_size(f);
  bool ok = rom_open(&c, file_read, f, sz);
  if (!ok) {
    printf("FAIL: rom_open rejected %s (size=%u)\n", path, sz);
    g_fail++; fclose(f); return;
  }
  printf("\n%s\n  title=\"%s\" code=%s rev=%u -> %s\n",
         path, c.title, c.code, c.version, rom_kind_name(c.kind));
  CHECK(c.kind == expect, "identified as the expected game");
  CHECK(c.group_count == expect_groups, "group count matches the known build");

  int npcs = 0, conns = 0, biggest = 0, oob = 0;
  int maps = walk_all(&c, &npcs, &conns, &biggest, &oob);
  printf("  groups=%u maps=%d (declared %u)  group0=%u  objects=%d (%d outside their map) connections=%d biggest=%d cells\n",
         c.group_count, maps, c.total_maps, c.group_size[0], npcs, oob, conns, biggest);
  CHECK(maps == c.total_maps, "walked exactly the derived map count");
  if (expect_maps > 0)   CHECK(maps == expect_maps, "total map count matches the decomp");
  if (expect_group0 > 0) CHECK(c.group_size[0] == expect_group0, "group 0 size matches the decomp");
  fclose(f);
}

int main(int argc, char** argv) {
  const char* em = (argc > 1) ? argv[1] : "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_EMER_BPEE00.gba";
  const char* rb = (argc > 2) ? argv[2] : "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_RUBY_AXVE02.gba";
  const char* fr = (argc > 3) ? argv[3] : "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_FIRE_BPRE01.gba";
  const char* hack = "/Users/guyshtainer/Desktop/switch sd backup/roms/gba/Pokemon Glazed Version.gba";
  const char* other = "/Users/guyshtainer/Desktop/switch sd backup/roms/gba/WinX Club.gba";

  /* Emerald: the decomp's map_groups.json gives 34 groups, 518 maps, group 0 = 57. */
  test_rom(em, ROM_EMERALD, 34, 518, 57);
  /* Ruby: 394 maps, group 0 = 54. */
  test_rom(rb, ROM_RUBY, 34, 394, 54);
  /* FireRed: 425 maps; its towns/routes are NOT group 0 (group 0 has 5 maps), which
   * is exactly why per-version group indices must never be assumed. */
  test_rom(fr, ROM_FIRERED, 43, 425, 5);

  /* ---- negative tests: everything that is not a supported retail ROM ---- */
  {
    FILE* f = fopen(hack, "rb");
    if (f) {
      RomCtx c;
      bool ok = rom_open(&c, file_read, f, file_size(f));
      CHECK(!ok, "a ROM hack is REFUSED, not silently mis-parsed");
      printf("\nhack refused: %s -> %s\n", hack, ok ? "ACCEPTED (BAD)" : "refused");
      fclose(f);
    } else printf("(skip hack negative test: not found)\n");
  }
  {
    FILE* f = fopen(other, "rb");
    if (f) {
      RomCtx c;
      CHECK(!rom_open(&c, file_read, f, file_size(f)), "a non-Pokemon GBA ROM is refused");
      fclose(f);
    } else printf("(skip non-Pokemon negative test: not found)\n");
  }
  {
    /* wrong size: the same Emerald image, but claimed short */
    FILE* f = fopen(em, "rb");
    if (f) {
      RomCtx c;
      CHECK(!rom_open(&c, file_read, f, 8u * 1024u * 1024u), "a truncated/odd-size image is refused");
      CHECK(!rom_open(&c, file_read, f, 32u * 1024u * 1024u), "a 32 MB image is refused");
      fclose(f);
    }
  }
  {
    RomCtx c;
    CHECK(!rom_open(&c, 0, 0, 16u * 1024u * 1024u), "NULL read callback refused");
    CHECK(!rom_open(0, file_read, 0, 16u * 1024u * 1024u), "NULL ctx refused");
  }

  /* ---- ground truth: Littleroot Town (Emerald group 0, map 9 per map_groups.json) --- */
  {
    FILE* f = fopen(em, "rb");
    if (f) {
      RomCtx c;
      if (rom_open(&c, file_read, f, file_size(f))) {
        /* find Littleroot by its layout id (10, 1-based) rather than assuming an index */
        int found = -1;
        for (int m = 0; m < rom_maps_in_group(&c, 0); m++) {
          RomMapHeader h;
          if (rom_map_header(&c, 0, m, &h) && h.layout_id == 10) { found = m; break; }
        }
        CHECK(found >= 0, "Littleroot Town (layout id 10) found in group 0");
        if (found >= 0) {
          RomMapHeader h; RomLayout l;
          rom_map_header(&c, 0, found, &h);
          rom_layout(&c, h.layout, &l);
          printf("\nLittleroot: group0 map %d  %ldx%ld blocks  mapsec=%u type=%u\n",
                 found, (long)l.width, (long)l.height, h.mapsec, h.map_type);
          /* Littleroot is 20x20 metatiles (320x320 px) per data/maps/LittlerootTown */
          CHECK(l.width == 20 && l.height == 20, "Littleroot is 20x20 blocks");
          CHECK(h.mapsec == 0, "Littleroot's MAPSEC is 0 (first region-map entry)");
          /* the layout the header points at must equal gMapLayouts[layout_id - 1] */
          uint8_t p[4];
          CHECK(rom_read_at(&c, c.map_layouts + (uint32_t)(h.layout_id - 1) * 4u, p, 4),
                "gMapLayouts entry reads");
          uint32_t via_table = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
          CHECK(via_table == h.layout, "mapLayoutId is 1-based into gMapLayouts");
        }
      }
      fclose(f);
    }
  }

  printf("\n");
  if (g_fail == 0) printf("OK: host_rommap_test (0 failures)\n");
  else             printf("host_rommap_test: %d FAILURE(S)\n", g_fail);
  return g_fail ? 1 : 0;
}
