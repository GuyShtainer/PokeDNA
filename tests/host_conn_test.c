/*
 * Host test for the map-connection geometry and warp-destination resolution added for the
 * stitching / enter-exit feature.
 *
 * These two pieces of arithmetic decide where the cursor lands when it walks off a map edge
 * and when it steps through a door. Both are pure C over RomCtx precisely so they can be
 * exercised here against REAL retail ROMs rather than only on a GBA — hard rule 5.
 *
 * The strong assertions are the RECIPROCAL ones: if map A says "my east neighbour is B at
 * offset k", then B must say "my west neighbour is A at offset -k", and a coordinate pushed
 * across the seam and back must land exactly where it started. A sign error in
 * rom_conn_origin survives a one-way test whenever the two maps happen to be the same size,
 * which on the Hoenn water routes is most of the time — so the round trip is the test that
 * actually has teeth.
 *
 * Build + run (from the repo root):
 *   cc -I source tests/host_conn_test.c source/rom_map.c -o /tmp/hc && /tmp/hc "<rom.gba>"
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rom_map.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static FILE* g_fp;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  if (fseek(g_fp, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, g_fp) == len;
}

static int opposite(int dir) {
  switch (dir) {
    case ROM_CONN_SOUTH: return ROM_CONN_NORTH;
    case ROM_CONN_NORTH: return ROM_CONN_SOUTH;
    case ROM_CONN_WEST:  return ROM_CONN_EAST;
    case ROM_CONN_EAST:  return ROM_CONN_WEST;
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: %s <rom.gba>\n", argv[0]); return 2; }
  g_fp = fopen(argv[1], "rb");
  if (!g_fp) { printf("cannot open %s\n", argv[1]); return 2; }
  fseek(g_fp, 0, SEEK_END);
  uint32_t size = (uint32_t)ftell(g_fp);

  RomCtx rc;
  if (!rom_open(&rc, file_read, 0, size)) { printf("not a known ROM\n"); return 2; }
  printf("%s  %s rev%u  %u groups, %u maps\n", argv[1], rom_kind_name(rc.kind), rc.version,
         rc.group_count, rc.total_maps);

  /* ---- pure arithmetic, independent of any ROM -------------------------------- */
  {
    int32_t ox, oy;
    CHECK(rom_conn_origin(ROM_CONN_EAST, 5, 40, 30, 20, 25, &ox, &oy) && ox == 40 && oy == 5,
          "EAST origin should be (W, off) = (40,5), got (%d,%d)", (int)ox, (int)oy);
    CHECK(rom_conn_origin(ROM_CONN_WEST, 5, 40, 30, 20, 25, &ox, &oy) && ox == -20 && oy == 5,
          "WEST origin should be (-Wn, off) = (-20,5), got (%d,%d)", (int)ox, (int)oy);
    CHECK(rom_conn_origin(ROM_CONN_SOUTH, -3, 40, 30, 20, 25, &ox, &oy) && ox == -3 && oy == 30,
          "SOUTH origin should be (off, H) = (-3,30), got (%d,%d)", (int)ox, (int)oy);
    CHECK(rom_conn_origin(ROM_CONN_NORTH, -3, 40, 30, 20, 25, &ox, &oy) && ox == -3 && oy == -25,
          "NORTH origin should be (off, -Hn) = (-3,-25), got (%d,%d)", (int)ox, (int)oy);
    /* Dive/emerge are not spatial and must be refused, or the Underwater twin gets painted
     * straight on top of the surface map. */
    CHECK(!rom_conn_origin(ROM_CONN_DIVE, 0, 40, 30, 40, 30, &ox, &oy), "DIVE must be refused");
    CHECK(!rom_conn_origin(ROM_CONN_EMERGE, 0, 40, 30, 40, 30, &ox, &oy), "EMERGE must be refused");
    /* containment is half-open: [off, off+span) */
    CHECK(rom_conn_contains(ROM_CONN_EAST, 10, 20, 25, 10), "along==off is inside");
    CHECK(!rom_conn_contains(ROM_CONN_EAST, 10, 20, 25, 35), "along==off+span is outside");
    CHECK(!rom_conn_contains(ROM_CONN_EAST, 10, 20, 25, 9), "along<off is outside");
  }

  /* ---- every cardinal connection in this ROM ---------------------------------- */
  int conns = 0, recip = 0, one_way = 0, dive = 0;
  for (int g = 0; g < rc.group_count; g++) {
    int n = rom_maps_in_group(&rc, g);
    for (int m = 0; m < n; m++) {
      RomMapHeader h; RomLayout l;
      if (!rom_map_header(&rc, g, m, &h)) continue;
      if (!rom_layout(&rc, h.layout, &l)) continue;
      int cn = rom_connection_count(&rc, h.connections);
      for (int i = 0; i < cn; i++) {
        RomConnection c;
        if (!rom_connection(&rc, h.connections, i, &c)) continue;
        if (c.direction < ROM_CONN_SOUTH || c.direction > ROM_CONN_EAST) { dive++; continue; }
        RomMapHeader nh; RomLayout nl;
        if (!rom_map_header(&rc, c.group, c.num, &nh)) continue;
        if (!rom_layout(&rc, nh.layout, &nl)) continue;
        if (nl.width <= 0 || nl.height <= 0) continue;
        conns++;

        int32_t ox, oy;
        CHECK(rom_conn_origin((int)c.direction, c.offset, l.width, l.height,
                              nl.width, nl.height, &ox, &oy),
              "%d.%d dir%u origin failed", g, m, (unsigned)c.direction);

        /* The origin must place the neighbour ADJACENT, never overlapping the current map
         * and never leaving a gap: the seam is exactly at the shared edge. */
        switch (c.direction) {
          case ROM_CONN_EAST:  CHECK(ox == l.width,  "%d.%d EAST seam at %d not %d", g, m, (int)ox, (int)l.width); break;
          case ROM_CONN_WEST:  CHECK(ox + nl.width == 0, "%d.%d WEST seam ends at %d not 0", g, m, (int)(ox + nl.width)); break;
          case ROM_CONN_SOUTH: CHECK(oy == l.height, "%d.%d SOUTH seam at %d not %d", g, m, (int)oy, (int)l.height); break;
          case ROM_CONN_NORTH: CHECK(oy + nl.height == 0, "%d.%d NORTH seam ends at %d not 0", g, m, (int)(oy + nl.height)); break;
        }

        /* Round trip: take a cell just past the seam, convert to the neighbour's frame,
         * and convert back. Must be the identity. */
        int32_t probe_x = (c.direction == ROM_CONN_EAST) ? l.width
                        : (c.direction == ROM_CONN_WEST) ? -1 : (int32_t)c.offset;
        int32_t probe_y = (c.direction == ROM_CONN_SOUTH) ? l.height
                        : (c.direction == ROM_CONN_NORTH) ? -1 : (int32_t)c.offset;
        int32_t nbx = probe_x - ox, nby = probe_y - oy;
        CHECK(nbx + ox == probe_x && nby + oy == probe_y,
              "%d.%d round trip broke", g, m);

        /* Reciprocity: does the neighbour point back at us with the opposite direction and
         * the negated offset? Most retail links do; some genuinely do not, and those are
         * exactly why a viewer must never chain connections into a global atlas. */
        int back = rom_connection_count(&rc, nh.connections), found = 0;
        for (int j = 0; j < back; j++) {
          RomConnection d;
          if (!rom_connection(&rc, nh.connections, j, &d)) continue;
          if ((int)d.direction != opposite((int)c.direction)) continue;
          if (d.group != g || d.num != m) continue;
          found = 1;
          if (d.offset == -c.offset) recip++;
          break;
        }
        if (!found) one_way++;
      }
    }
  }
  printf("  cardinal connections %d, reciprocal-with-negated-offset %d, one-way %d, dive/emerge %d\n",
         conns, recip, one_way, dive);
  CHECK(conns > 0, "the ROM should have some connections");

  /* ---- every warp in this ROM -------------------------------------------------- */
  int warps = 0, ok = 0, dyn = 0, dummy = 0, bad = 0, unread = 0, oob_src = 0;
  for (int g = 0; g < rc.group_count; g++) {
    int n = rom_maps_in_group(&rc, g);
    for (int m = 0; m < n; m++) {
      RomMapHeader h; RomLayout l; RomMapEvents ev;
      if (!rom_map_header(&rc, g, m, &h)) continue;
      if (!rom_layout(&rc, h.layout, &l)) continue;
      if (!rom_map_events(&rc, h.events, &ev)) continue;
      for (int i = 0; i < ev.warp_count; i++) {
        RomWarp w;
        if (!rom_warp(&rc, &ev, i, &w)) continue;
        warps++;
        if (w.x < 0 || w.y < 0 || w.x >= l.width || w.y >= l.height) oob_src++;
        uint8_t dg, dn; int32_t dx, dy;
        int r = rom_warp_dest(&rc, &w, &dg, &dn, &dx, &dy);
        switch (r) {
          case ROM_WARP_OK: {
            ok++;
            /* The whole point of the clamp: a resolved destination must ALWAYS be a legal
             * cell on the destination map, because the viewer indexes blockdata with it. */
            RomMapHeader dh; RomLayout dl;
            if (rom_map_header(&rc, dg, dn, &dh) && rom_layout(&rc, dh.layout, &dl)) {
              CHECK(dx >= 0 && dy >= 0 && dx < dl.width && dy < dl.height,
                    "%d.%d warp %d -> %u.%u (%d,%d) outside %dx%d",
                    g, m, i, dg, dn, (int)dx, (int)dy, (int)dl.width, (int)dl.height);
            } else {
              CHECK(0, "%d.%d warp %d resolved to unreadable map %u.%u", g, m, i, dg, dn);
            }
            break;
          }
          case ROM_WARP_DYNAMIC:    dyn++;    break;
          case ROM_WARP_DUMMY:      dummy++;  break;
          case ROM_WARP_BAD_MAP:    bad++;    break;
          default:                  unread++; break;
        }
      }
    }
  }
  printf("  warps %d: ok %d, dynamic %d, dummy %d, bad-map %d, unreadable %d"
         " (source tile off-map: %d)\n", warps, ok, dyn, dummy, bad, unread, oob_src);
  CHECK(warps > 0, "the ROM should have some warps");
  CHECK(ok > warps / 2, "most warps should resolve; got %d of %d", ok, warps);

  printf(fails ? "\nhost_conn_test: %d FAILURE(S) of %d checks\n"
               : "\nOK: host_conn_test (%d failures, %d checks)\n", fails, checks);
  fclose(g_fp);
  return fails ? 1 : 0;
}
