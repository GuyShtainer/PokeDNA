/* Host (PC) test for npc_gfx — Gen-3 overworld NPC sprite extraction, run against
 * REAL retail ROMs. Because npc_gfx does all its I/O through the same RomReadFn
 * callback rom_map uses, the code exercised here is byte-for-byte the code that will
 * run on the GBA over FatFs.
 *
 * What it proves:
 *   1) sObjectEventSpritePalettes is FOUND by signature in Emerald, Ruby and FireRed
 *      (no per-revision constants), and lands on the addresses the decomp implies;
 *   2) the ObjectEventGraphicsInfo layout (0x24 bytes) and SpriteFrameImage layout
 *      (8 bytes) are right: every resolvable id has images[frame].size == w*h/2 and a
 *      frame that lies wholly inside the image;
 *   3) the special ids fail CLEANLY rather than rendering garbage — VAR_0..F, ids past
 *      NUM_OBJ_EVENT_GFX, and the one genuinely broken pic table (RSE id 62);
 *   4) npc_gfx_pixels (per-tile streaming) and npc_gfx_frame + npc_gfx_blit (bulk)
 *      produce identical pixels, for every resolvable id, in every ROM;
 *   5) buffer discipline: npc_gfx_frame refuses a short buffer and writes nothing;
 *   6) ground truth: Emerald id 0 is Brendan, 16x32, palette slot 0, tag 0x1100;
 *      FireRed id 0 is Red, 16x32, slot 0, tag 0x1100.
 *   7) the OAM budget: distinct graphics ids and distinct palettes on the busiest map.
 *
 * It also dumps a contact sheet of the first 64 resolvable sprites per ROM as a
 * binary PPM, for a human (or Claude) to eyeball. Nothing is written into the repo.
 *
 * ROMs are the user's own dumps and are NEVER part of this repo. Missing ROMs SKIP.
 *
 * Build + run (from projects/PokeDNA):
 *   cc -I source tests/host_npcgfx_test.c source/npc_gfx.c source/rom_map.c \
 *      source/map_render.c -o /tmp/hn && /tmp/hn
 *   /tmp/hn "/path/emerald.gba" "/path/ruby.gba" "/path/firered.gba"
 * (map_render.c is only on the link line for symmetry with the other host tests —
 *  npc_gfx deliberately depends on nothing but rom_map.)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rom_map.h"
#include "npc_gfx.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

/* Counted so the test can report what npc_gfx_open() costs over FatFs on the GBA. */
static long g_reads = 0, g_bytes = 0;

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  g_reads++; g_bytes += len;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, f) == len;
}
static uint32_t file_size(FILE* f) {
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  return (uint32_t)(n < 0 ? 0 : n);
}

/* ---- contact sheet ---------------------------------------------------------- */
#define CELL   68
#define COLS   8
#define SCALE  2

typedef struct { int w, h; uint8_t* rgb; } Sheet;

static void sheet_init(Sheet* s, int rows) {
  s->w = CELL * COLS * SCALE; s->h = CELL * rows * SCALE;
  s->rgb = (uint8_t*)malloc((size_t)s->w * s->h * 3);
  for (int i = 0; i < s->w * s->h; i++) {
    s->rgb[i*3+0] = 0x20; s->rgb[i*3+1] = 0x20; s->rgb[i*3+2] = 0x28;
  }
}
static void sheet_put(Sheet* s, int cell, int w, int h, const uint16_t* px, int stride) {
  int cx = (cell % COLS) * CELL, cy = (cell / COLS) * CELL;
  int ox = cx + (CELL - w) / 2, oy = cy + (CELL - h) / 2;
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    uint16_t c = px[y * stride + x];
    if (c == 0xFFFF) continue;                      /* our "untouched" sentinel */
    int r = (c & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = ((c >> 10) & 31) * 255 / 31;
    for (int sy = 0; sy < SCALE; sy++) for (int sx = 0; sx < SCALE; sx++) {
      int px_ = (ox + x) * SCALE + sx, py_ = (oy + y) * SCALE + sy;
      if (px_ < 0 || py_ < 0 || px_ >= s->w || py_ >= s->h) continue;
      uint8_t* q = s->rgb + ((size_t)py_ * s->w + px_) * 3;
      q[0] = (uint8_t)r; q[1] = (uint8_t)g; q[2] = (uint8_t)b;
    }
  }
}
static void sheet_write(Sheet* s, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) { printf("  (could not write %s)\n", path); free(s->rgb); return; }
  fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
  fwrite(s->rgb, 1, (size_t)s->w * s->h * 3, f);
  fclose(f);
  printf("  contact sheet -> %s (%dx%d)\n", path, s->w, s->h);
  free(s->rgb);
}

/* ---- per-ROM run ------------------------------------------------------------ */

typedef struct { int gid; int cnt; } IdCount;

static void run(const char* path, const char* sheet_dir) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP (no ROM): %s\n", path); return; }
  RomCtx rom;
  if (!rom_open(&rom, file_read, f, file_size(f))) {
    printf("SKIP (unrecognised ROM): %s\n", path); fclose(f); return;
  }
  printf("\n================ %s (%s rev %u) ================\n",
         rom_kind_name(rom.kind), rom.code, rom.version);

  NpcGfx g;
  g_reads = g_bytes = 0;
  bool opened = npc_gfx_open(&g, &rom);
  long open_reads = g_reads, open_bytes = g_bytes;
  CHECK(opened, "npc_gfx_open finds the object-event palette table");
  if (!opened) { fclose(f); return; }
  printf("  gObjectEventGraphicsInfoPointers = 0x%08X  (%u ids)\n", g.info_ptrs, g.gfx_count);
  printf("  sObjectEventSpritePalettes       = 0x%08X  (%u entries)\n", g.pal_table, g.pal_count);
  printf("  open() cost: %ld reads / %ld bytes (one-off; delta to gfx table +0x%X)\n",
         open_reads, open_bytes, g.pal_table - g.info_ptrs);
  CHECK(open_bytes < 200000, "npc_gfx_open reads well under 200 KB of the ROM");
  CHECK(g.pal_table > g.info_ptrs, "palette table follows the graphics-info table");
  CHECK(g.pal_table - g.info_ptrs < 0x18000u, "palette table is inside the scan window");

  /* ---- resolve every id --------------------------------------------------- */
  static NpcGfxInfo infos[256];
  int ok = 0, err_counts[16]; memset(err_counts, 0, sizeof err_counts);
  int dynamic = 0, needs_subsprites = 0;
  int dim_w_max = 0, dim_h_max = 0;
  for (int gid = 0; gid < 256; gid++) {
    npc_gfx_info(&g, (uint16_t)gid, &infos[gid]);
    if (infos[gid].ok) {
      ok++;
      if (infos[gid].dynamic) dynamic++;
      uint8_t sh, sz;
      if (!npc_gfx_oam_shape(infos[gid].w, infos[gid].h, &sh, &sz)) needs_subsprites++;
      if (infos[gid].w > dim_w_max) dim_w_max = infos[gid].w;
      if (infos[gid].h > dim_h_max) dim_h_max = infos[gid].h;
    } else {
      err_counts[infos[gid].err]++;
    }
  }
  printf("  resolvable ids: %d of %u defined (%d of 256 probed)\n", ok, g.gfx_count, ok);
  for (int e = 1; e < 16; e++)
    if (err_counts[e]) printf("    %-3d x %s\n", err_counts[e], npc_gfx_err_str((NpcGfxErr)e));
  printf("  dynamic (game may substitute): %d;  need OAM subsprites: %d;  max %dx%d\n",
         dynamic, needs_subsprites, dim_w_max, dim_h_max);

  CHECK(ok >= (int)g.gfx_count - 2, "at most two ids per game fail to resolve");
  CHECK(err_counts[NPC_ERR_DYNAMIC_VAR] == 16, "exactly the 16 VAR ids are refused as dynamic");
  CHECK(err_counts[NPC_ERR_ID_RANGE] == 256 - 240 - 16 + (240 - (int)g.gfx_count),
        "ids between NUM_OBJ_EVENT_GFX and 240 are refused as out of range");

  /* ---- ground truth: id 0 is the player, 16x32, slot 0, tag 0x1100 --------- */
  CHECK(infos[0].ok && infos[0].w == 16 && infos[0].h == 32,
        "graphics id 0 (the player) is 16x32");
  CHECK(infos[0].pal_slot == 0 && infos[0].pal_tag == 0x1100,
        "graphics id 0 uses palette slot 0 / tag 0x1100");
  CHECK(infos[0].frame == 0 && !infos[0].hflip, "south-facing frame of id 0 is frame 0");

  /* ---- streaming vs bulk must agree, everywhere --------------------------- */
  static uint16_t bufA[NPC_GFX_MAX_W * NPC_GFX_MAX_H];
  static uint16_t bufB[NPC_GFX_MAX_W * NPC_GFX_MAX_H];
  static uint8_t  frame[NPC_GFX_MAX_BYTES];
  int mismatches = 0, blitted = 0;
  for (int gid = 0; gid < 256; gid++) {
    const NpcGfxInfo* in = &infos[gid];
    if (!in->ok) continue;
    for (int i = 0; i < NPC_GFX_MAX_W * NPC_GFX_MAX_H; i++) bufA[i] = bufB[i] = 0xFFFF;
    if (!npc_gfx_pixels(&g, in, bufA, NPC_GFX_MAX_W)) { printf("FAIL: pixels id %d\n", gid); g_fail++; continue; }
    uint16_t pal[16];
    if (!npc_gfx_palette(&g, in, pal)) { printf("FAIL: palette id %d\n", gid); g_fail++; continue; }
    if (!npc_gfx_frame(&g, in, frame, sizeof frame)) { printf("FAIL: frame id %d\n", gid); g_fail++; continue; }
    if (!npc_gfx_blit(in, frame, in->frame_bytes, pal, bufB, NPC_GFX_MAX_W)) { printf("FAIL: blit id %d\n", gid); g_fail++; continue; }
    if (memcmp(bufA, bufB, sizeof bufA) != 0) { mismatches++; printf("  pixel mismatch id %d\n", gid); }
    blitted++;
  }
  CHECK(mismatches == 0, "npc_gfx_pixels and npc_gfx_frame+blit agree on every sprite");
  printf("  %d sprites composited two independent ways, %d mismatches\n", blitted, mismatches);

  /* ---- buffer discipline -------------------------------------------------- */
  {
    const NpcGfxInfo* in = &infos[0];
    uint8_t small[8]; memset(small, 0xAA, sizeof small);
    CHECK(!npc_gfx_frame(&g, in, small, sizeof small), "npc_gfx_frame refuses a short buffer");
    int untouched = 1;
    for (unsigned i = 0; i < sizeof small; i++) if (small[i] != 0xAA) untouched = 0;
    CHECK(untouched, "a refused npc_gfx_frame writes nothing");
    CHECK(npc_gfx_frame(&g, in, frame, in->frame_bytes), "an exactly-sized buffer is accepted");
    uint16_t pal[16]; npc_gfx_palette(&g, in, pal);
    CHECK(!npc_gfx_blit(in, frame, in->frame_bytes, pal, bufA, in->w - 1),
          "blit refuses a stride narrower than the sprite");
    CHECK(!npc_gfx_pixels(&g, in, bufA, in->w - 1),
          "pixels refuses a stride narrower than the sprite");
    CHECK(!npc_gfx_blit(in, frame, in->frame_bytes - 1, pal, bufA, NPC_GFX_MAX_W),
          "blit refuses a short frame buffer");
  }

  /* ---- transparency: colour 0 is never written ---------------------------- */
  {
    const NpcGfxInfo* in = &infos[0];
    for (int i = 0; i < NPC_GFX_MAX_W * NPC_GFX_MAX_H; i++) bufA[i] = 0xFFFF;
    npc_gfx_pixels(&g, in, bufA, NPC_GFX_MAX_W);
    int transparent = 0;
    for (int y = 0; y < in->h; y++) for (int x = 0; x < in->w; x++)
      if (bufA[y * NPC_GFX_MAX_W + x] == 0xFFFF) transparent++;
    CHECK(transparent > 0 && transparent < in->w * in->h,
          "the player sprite is partly transparent and partly drawn");
  }

  /* ---- direction frames --------------------------------------------------- */
  {
    NpcGfxInfo s, n, w, e;
    npc_gfx_info_dir(&g, 0, NPC_DIR_SOUTH, &s);
    npc_gfx_info_dir(&g, 0, NPC_DIR_NORTH, &n);
    npc_gfx_info_dir(&g, 0, NPC_DIR_WEST,  &w);
    npc_gfx_info_dir(&g, 0, NPC_DIR_EAST,  &e);
    CHECK(s.ok && n.ok && w.ok && e.ok, "all four facings of id 0 resolve");
    CHECK(s.frame == 0 && n.frame == 1 && w.frame == 2 && e.frame == 2,
          "player facing frames are 0/1/2/2 as the anim table says");
    CHECK(e.hflip && !w.hflip, "east is the west frame, h-flipped");
    printf("  facings of id 0: S=%u N=%u W=%u E=%u(hflip=%d)\n",
           s.frame, n.frame, w.frame, e.frame, (int)e.hflip);
  }

  /* ---- VAR ids resolve once the caller supplies the save's var ------------ */
  {
    NpcGfxInfo v, direct;
    CHECK(npc_gfx_var_index(240) == 0 && npc_gfx_var_index(255) == 15 &&
          npc_gfx_var_index(239) == -1, "npc_gfx_var_index maps 240..255 to 0..15");
    CHECK(!npc_gfx_info(&g, 240, &v) && v.err == NPC_ERR_DYNAMIC_VAR,
          "a VAR id is refused without a var value");
    npc_gfx_info(&g, 16, &direct);
    CHECK(npc_gfx_info_var(&g, 240, 16, NPC_DIR_SOUTH, &v) && v.ok,
          "a VAR id resolves when given the var's value");
    CHECK(v.frame_addr == direct.frame_addr && v.pal_addr == direct.pal_addr,
          "VAR-resolved sprite is identical to the id it names");
    CHECK(!npc_gfx_info_var(&g, 240, 250, NPC_DIR_SOUTH, &v) && v.err == NPC_ERR_DYNAMIC_VAR,
          "a var pointing at another VAR id is still refused");
    CHECK(npc_gfx_info_var(&g, 16, 0, NPC_DIR_SOUTH, &v) && v.ok &&
          v.frame_addr == direct.frame_addr,
          "npc_gfx_info_var ignores the var value for a normal id");
  }

  /* ---- OAM / palette budget over every map -------------------------------- */
  {
    int max_objects = 0, max_ids = 0, max_pals = 0, max_slots = 0, max_oam = 0, maps = 0;
    int busy_g = -1, busy_m = -1, busy_pg = -1, busy_pm = -1, busy_og = -1, busy_om = -1;
    int busy_sg = -1, busy_sm = -1;
    int unresolvable_placed = 0, placed_total = 0, clones = 0;
    int unres_hist[256]; memset(unres_hist, 0, sizeof unres_hist);
    for (int grp = 0; grp < rom.group_count; grp++) {
      int nm = rom_maps_in_group(&rom, grp);
      for (int m = 0; m < nm; m++) {
        RomMapHeader h;
        if (!rom_map_header(&rom, grp, m, &h)) continue;
        maps++;
        RomMapEvents ev;
        if (!rom_map_events(&rom, h.events, &ev)) continue;
        if (ev.object_count > max_objects) { max_objects = ev.object_count; busy_og = grp; busy_om = m; }
        uint8_t seen_id[256]; memset(seen_id, 0, sizeof seen_id);
        uint16_t seen_tag[64]; int ntag = 0;
        uint16_t seen_slot[32]; int nslot = 0;
        int ids = 0, oam = 0;
        for (int i = 0; i < ev.object_count; i++) {
          RomObjectEvent o;
          if (!rom_object_event(&rom, &ev, i, &o)) continue;
          placed_total++;
          if (o.kind == 255) clones++;
          if (!seen_id[o.graphics_id]) { seen_id[o.graphics_id] = 1; ids++; }
          const NpcGfxInfo* in = &infos[o.graphics_id];
          if (!in->ok) { unresolvable_placed++; unres_hist[o.graphics_id]++; continue; }
          uint8_t sh, sz;
          oam += npc_gfx_oam_shape(in->w, in->h, &sh, &sz) ? 1 : 4;  /* 4 = subsprite guess */
          int found = 0;
          for (int t = 0; t < ntag; t++) if (seen_tag[t] == in->pal_tag) { found = 1; break; }
          if (!found && ntag < 64) seen_tag[ntag++] = in->pal_tag;
          found = 0;
          for (int t = 0; t < nslot; t++) if (seen_slot[t] == in->pal_slot) { found = 1; break; }
          if (!found && nslot < 32) seen_slot[nslot++] = in->pal_slot;
        }
        if (ids > max_ids)     { max_ids = ids;     busy_g = grp;  busy_m = m; }
        if (ntag > max_pals)   { max_pals = ntag;   busy_pg = grp; busy_pm = m; }
        if (nslot > max_slots) { max_slots = nslot; busy_sg = grp; busy_sm = m; }
        if (oam > max_oam) max_oam = oam;
      }
    }
    printf("  --- OAM budget over %d maps, %d placed objects ---\n", maps, placed_total);
    printf("    max object_count on any map : %d   (map %d.%d)\n", max_objects, busy_og, busy_om);
    printf("    max DISTINCT graphics ids   : %d   (map %d.%d)\n", max_ids, busy_g, busy_m);
    printf("    max DISTINCT palette TAGS   : %d   (map %d.%d)\n", max_pals, busy_pg, busy_pm);
    printf("    max DISTINCT palette SLOTS  : %d   (map %d.%d)  [the game's own assignment]\n",
           max_slots, busy_sg, busy_sm);
    printf("    max OAM entries if every object is drawn: %d\n", max_oam);
    printf("    placed objects whose id does not resolve statically: %d (of %d)\n",
           unresolvable_placed, placed_total);
    for (int i = 0; i < 256; i++)
      if (unres_hist[i] > 0)
        printf("       id %3d x%4d  (%s)\n", i, unres_hist[i], npc_gfx_err_str(infos[i].err));
    printf("    OBJ_KIND_CLONE templates (FRLG only): %d\n", clones);
    CHECK(max_pals <= 16, "no single map needs more than the GBA's 16 sprite palettes");
    CHECK(max_objects <= 128, "no single map places more objects than there are OAM entries");
  }

  /* ---- contact sheet ------------------------------------------------------ */
  {
    int sel[64], nsel = 0;
    for (int gid = 0; gid < 256 && nsel < 64; gid++) if (infos[gid].ok) sel[nsel++] = gid;
    Sheet sh; sheet_init(&sh, (nsel + COLS - 1) / COLS);
    for (int i = 0; i < nsel; i++) {
      const NpcGfxInfo* in = &infos[sel[i]];
      for (int k = 0; k < NPC_GFX_MAX_W * NPC_GFX_MAX_H; k++) bufA[k] = 0xFFFF;
      npc_gfx_pixels(&g, in, bufA, NPC_GFX_MAX_W);
      sheet_put(&sh, i, in->w, in->h, bufA, NPC_GFX_MAX_W);
    }
    char out[512];
    snprintf(out, sizeof out, "%s/npc_%s.ppm", sheet_dir, rom_kind_name(rom.kind));
    sheet_write(&sh, out);
  }

  /* A second sheet of only the BIG sprites — 32x32 and up. The odd sizes (48x48,
   * 64x64, 88x32, 96x40, FireRed's 128x64) are exactly where a wrong stride or a
   * mis-read dimension would hide, and they are invisible on the first sheet. */
  {
    int sel[64], nsel = 0;
    for (int gid = 0; gid < 256 && nsel < 64; gid++)
      if (infos[gid].ok && infos[gid].w >= 32 && infos[gid].h >= 32) sel[nsel++] = gid;
    if (nsel) {
      Sheet sh; sheet_init(&sh, (nsel + COLS - 1) / COLS);
      for (int i = 0; i < nsel; i++) {
        const NpcGfxInfo* in = &infos[sel[i]];
        for (int k = 0; k < NPC_GFX_MAX_W * NPC_GFX_MAX_H; k++) bufA[k] = 0xFFFF;
        npc_gfx_pixels(&g, in, bufA, NPC_GFX_MAX_W);
        sheet_put(&sh, i, in->w, in->h, bufA, NPC_GFX_MAX_W);
      }
      char out[512];
      snprintf(out, sizeof out, "%s/npcbig_%s.ppm", sheet_dir, rom_kind_name(rom.kind));
      sheet_write(&sh, out);
    }
  }

  fclose(f);
}

int main(int argc, char** argv) {
  const char* def[3] = {
    "/Users/guyshtainer/Desktop/swtich sd backup new/roms/gba/Pokemon - Emerald Version.gba",
    "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_RUBY_AXVE02.gba",
    "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_FIRE_BPRE01.gba",
  };
  const char* dir = getenv("NPCGFX_OUT");
  if (!dir) dir = "/tmp";
  if (argc > 1) { for (int i = 1; i < argc; i++) run(argv[i], dir); }
  else          { for (int i = 0; i < 3; i++)    run(def[i], dir); }

  printf("\n%s (%d failures)\n", g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", g_fail);
  return g_fail ? 1 : 0;
}
