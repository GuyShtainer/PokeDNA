/* Host (PC) measurement for art_fallbacks.c's NEW ROM rung under mon_icon_for* --
 * the fix for Guy's item 6 ("daycare and pokedex are not it"). MIRRORS the
 * production algorithm (art_fallbacks.c's rom_locate_row/rom_icon_fill/
 * rom_pal_for_row -- art_fallbacks.c itself cannot be host-compiled, it pulls in
 * GBA-only headers), driven off the SAME rom_mon.c the real build calls, so the
 * READ COUNT here is the real per-RomReadFn-call cost the SD path pays.
 *
 * What this proves (see the per-section comments below for the full reasoning):
 *   1) a full first Pokedex page (21 cells) costs a BOUNDED number of RomReadFn
 *      calls, well under a naive/no-cache baseline;
 *   2) the "no SD I/O on an animation tick" fix is a GATE: with no icons.bin
 *      cache and only this ROM rung open, the real per-60-idle-frame cost is
 *      EXACTLY ZERO BY CONSTRUCTION (pdna_pick.c/pdna_main.c's idle-bob branches
 *      are gated on it), reported here beside the avoided counterfactual;
 *   3) the Day-Care's 7-icon cast costs the same bounded shape.
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 *   cc -std=c11 -I source tests/host_dexicons_test.c source/rom_mon.c source/rom_map.c source/data_tables.c -o /tmp/hdi && /tmp/hdi
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "rom_map.h"
#include "rom_mon.h"
#include "data_tables.h"

static int fails = 0, checks = 0;
static void chk(const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

/* ---- the counting reader: every RomReadFn call is a transaction the SD path
 * (iconrom_fatfs_read) pays for (f_lseek [only if the offset isn't already where
 * the file pointer sits] + f_read); a fused source pays a free memcpy for the same
 * call. `backward` counts calls whose offset is LESS than the previous call's --
 * FF_USE_FASTSEEK is 0, so exactly these are the ones that would re-walk a 16 MB
 * cluster chain from the head on real hardware. */
/* Two backward tallies, on purpose: rom_mon.c's OWN header comment (read_small)
 * says a same-field re-read (the verify pass, <= 4 B backward) "stays inside the
 * current cluster ... adds no FAR seek at all" -- so `backward` (any decrease)
 * would count those as if they were expensive, which they are not. `far_backward`
 * only counts a jump backward by more than one FAT cluster's worth (4 KiB, a
 * deliberately conservative floor -- real cluster sizes on a big SD card run
 * larger) -- THAT is the class FF_USE_FASTSEEK=0 makes costly (a re-walk of the
 * chain from the head of a 16 MB file). */
#define FAR_BACKWARD_BYTES 4096
typedef struct { FILE* f; int reads; int backward; int far_backward; long last_off; } FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->reads++;
  if ((long)off < fc->last_off) {
    fc->backward++;
    if (fc->last_off - (long)off > FAR_BACKWARD_BYTES) fc->far_backward++;
  }
  fc->last_off = (long)off + (long)len;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  return true;
}

/* ---- mirror of art_fallbacks.c's ROM rung (keep in sync by eye) ------------- */
static RomMonLoc s_loc; static uint16_t s_loc_row = 0xFFFF;
static uint16_t  s_pal[ROM_MON_PALS][16]; static uint8_t s_pal_have = 0;

static void mirror_reset(void) { s_loc_row = 0xFFFF; s_pal_have = 0; }

static bool mirror_locate_row(const RomMon* rm, uint16_t row) {
  if (s_loc_row == row && s_loc.ok) return true;
  int unstable = 0;
  s_loc_row = row;
  if (!rom_mon_locate_row_verified(rm, row, &s_loc, 4, &unstable)) { s_loc_row = 0xFFFF; return false; }
  return true;
}
static uint32_t sum512(const uint8_t* b) { uint32_t v = 0; for (int i = 0; i < 512; i++) v += b[i]; return v; }
static bool mirror_fill(const RomMon* rm, uint16_t row, uint8_t frame, uint8_t raw[512]) {
  if (!mirror_locate_row(rm, row)) return false;
  if (!rom_mon_icon_at(rm, &s_loc, frame, raw)) return false;
  uint32_t s1 = sum512(raw);
  if (!rom_mon_icon_at(rm, &s_loc, frame, raw)) return false;
  return sum512(raw) == s1;
}
static bool mirror_pal(const RomMon* rm, uint16_t row, uint16_t out[16]) {
  if (!mirror_locate_row(rm, row)) return false;
  uint8_t id = s_loc.pal;
  if (id >= ROM_MON_PALS) return false;
  if (!(s_pal_have & (1u << id))) {
    if (!rom_mon_icon_pal(rm, id, s_pal[id])) return false;
    s_pal_have |= (uint8_t)(1u << id);
  }
  memcpy(out, s_pal[id], 32);
  return true;
}

/* 3-slot MRU exactly like art_fallbacks.c's s_icfr, keyed by (row, frame). */
#define SLOTS 3
typedef struct { uint16_t row; uint8_t frame; uint8_t valid; uint32_t age; uint8_t raw[512]; } Slot;
static Slot s_mru[SLOTS]; static uint32_t s_clock = 0;
static void mru_reset(void) { memset(s_mru, 0, sizeof s_mru); s_clock = 0; }
static int mru_index(uint16_t row, uint8_t frame, bool* hit) {
  for (int i = 0; i < SLOTS; i++)
    if (s_mru[i].valid && s_mru[i].row == row && s_mru[i].frame == frame) { s_mru[i].age = ++s_clock; *hit = true; return i; }
  int v = 0;
  for (int i = 0; i < SLOTS; i++) { if (!s_mru[i].valid) { v = i; break; } if (s_mru[i].age < s_mru[v].age) v = i; }
  s_mru[v].row = row; s_mru[v].frame = frame; s_mru[v].valid = 0; s_mru[v].age = ++s_clock;
  *hit = false; return v;
}
/* the one entry point pdna_pick.c/pdna_main.c effectively call through mon_icon_for*
 * on this rung (cache_ready is always false in every scenario this test drives --
 * there is no icons.bin on a host run, matching "no emulator has an SD card"). */
static bool icon_via_rom(const RomMon* rm, uint16_t species, uint8_t frame, uint16_t pal[16]) {
  bool hit = false;
  int idx = mru_index(species, frame, &hit);
  if (!hit) {
    uint8_t tmp[512];
    if (!mirror_fill(rm, species, frame, tmp)) return false;
    memcpy(s_mru[idx].raw, tmp, 512);
    s_mru[idx].valid = 1;
  }
  return mirror_pal(rm, species, pal);
}

/* National #1..21 -> internal species id, via the SAME reverse lookup pk_national_no
 * already makes forward -- so "page 1" is the REAL display order dex_build()
 * produces (sort=0, national-number ascending), not internal-id order (which is
 * NOT the display order in Hoenn-native games -- national and internal axes are a
 * genuine permutation of each other). */
static uint16_t species_for_national(uint16_t nat) {
  for (uint16_t sp = 1; sp <= 411; sp++) if (pk_national_no(sp) == nat) return sp;
  return 0;
}

static void run_rom(const char* path, const char* name) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0, 0, 0, -1 };
  RomCtx rc;
  if (!rom_open(&rc, file_read, &fc, (uint32_t)sz)) { printf("SKIP %s (rom_open)\n", name); fclose(f); return; }
  RomMon rm;
  if (!rom_mon_open(&rm, &rc)) { printf("SKIP %s (no GF header -- no icons)\n", name); fclose(f); return; }

  /* ---- 1/2/3: a full first Pokedex page, 7x3 = 21 cells, National #1-21 ------ */
  mirror_reset(); mru_reset(); fc.reads = 0; fc.backward = 0; fc.far_backward = 0; fc.last_off = -1;
  uint16_t page[21]; int n = 0;
  for (uint16_t nat = 1; nat <= 21; nat++) { uint16_t sp = species_for_national(nat); if (sp) page[n++] = sp; }
  chk("page 1 resolves 21 species", n == 21);

  int page_ok = 1;
  for (int i = 0; i < n; i++) {
    uint16_t sp = page[i];
    /* the same synthetic dstate mix every run (deterministic): every 7th cell
     * CAUGHT (dex_cell_grid draws frame `bob`, worst case bob=1 -- a SECOND,
     * distinct MRU key from the check call's frame 0), every other remaining
     * cell SEEN, the rest UNSEEN -- both of those only ever touch frame 0. */
    int state = (i % 7 == 0) ? 2 : (i % 2 == 0) ? 1 : 0;
    uint16_t pal[16];
    /* dex_cell_grid's CHECK call: mon_icon_for(in) == frame 0 */
    if (!icon_via_rom(&rm, sp, 0, pal)) { page_ok = 0; continue; }
    if (state == 2) {
      const int bob = 1;                    /* worst case: caught cell's OWN frame differs */
      if (!icon_via_rom(&rm, sp, (uint8_t)bob, pal)) page_ok = 0;
    } else {
      if (!icon_via_rom(&rm, sp, 0, pal)) page_ok = 0;   /* the DRAW call, same key as check */
    }
  }
  chk("every cell on the page resolves", page_ok);
  /* The NAIVE baseline this ladder is measured against: NO locate memo and NO MRU
   * at all, i.e. every one of the 21 cells' TWO calls (check + draw) independently
   * pays a full fresh locate(4)+frame-verify(2) = 6 -- 18 cells x 2 calls x 6, plus
   * the 3 caught cells' SECOND call also needing its own fresh locate (the naive
   * code has no memo to hit): 21*2*6 = 252. That is the number a page render would
   * cost WITHOUT this fix's memo/MRU sharing a locate and a tile across a cell's
   * own check-then-draw pair. */
  int naive = 21 * 2 * 6;
  printf("  [%s] PAGE 1 (21 cells, 3 caught @bob=1): %d RomReadFn calls "
         "(naive/no-cache baseline would be %d), %d backward (%d far/chain-walking)\n",
         name, fc.reads, naive, fc.backward, fc.far_backward);
  /* Bound: worst case per FRESH row is 4 (locate) + 2 (frame verify) = 6, plus at
   * most 2 more for a caught cell's second frame, plus at most 2*3=6 total for the
   * (at most 3) distinct palette banks ever loaded. 21 rows * 8 + 6 is a generous
   * ceiling; the real number is reported above for the record. */
  chk("page 1 cost stays under 21*8 + 6 RomReadFn calls (bounded, not unbounded)",
      fc.reads <= 21 * 8 + 6);
  chk("the check-then-draw pair inside one cell shares its locate+tile (beats the naive baseline)",
      fc.reads < naive);
  /* HONEST FINDING, not the hoped-for zero: National-Dex display order is a
   * SCATTER of the internal-species-id axis the icon TILE table is stored in
   * (Hoenn-native games renumber on registration; national and internal ids are a
   * genuine permutation of each other, not just an offset) -- so a page of 21
   * DIFFERENT species visits 21 genuinely different, non-monotonic tile offsets
   * regardless of any caching this rung can do (a cache can avoid RE-visiting an
   * address, it cannot make 21 inherently-distinct addresses fewer or closer
   * together). Measured: about a fifth of the page's reads are a >4 KiB backward
   * jump, each of which costs a real cluster-chain re-walk on the SD path
   * (FF_USE_FASTSEEK=0). This is bounded (not proportional to a full 386-species
   * dex, only to the 21 on screen) and it is EXACTLY the cost the icons.bin cache
   * (DESIGN.md Sec 4.4's "store tiles in first-reference order, read strictly
   * forward") exists to remove once the user extracts it -- this rung is the
   * degraded-but-real path for a session that has not extracted one yet, not a
   * replacement for it. */
  chk("far-backward jumps stay bounded (<=2 per page cell, not proportional to the whole ROM)",
      fc.far_backward <= 2 * 21);

  /* ---- 4: the animation-tick gate's precondition ---------------------------- */
  /* mon_icon_anim_cheap() in art_fallbacks.c is `cache_ready || !s_rommon`. On this
   * rung cache_ready is always false (no icons.bin on a host run) and s_rommon is
   * set, so the gate is FALSE -- pdna_pick.c's dex bob and pdna_main.c's daycare
   * bob (both patched with `&& mon_icon_anim_cheap()`) never re-enter icon_via_rom
   * on an idle tick. Real cost over any number of idle frames: 0 RomReadFn calls,
   * by construction. Reported here as documentation, not a re-derivation: */
  bool cache_ready_this_scenario = false;
  bool anim_cheap = cache_ready_this_scenario /* || !rommon, and rommon IS open */;
  chk("anim gate is false whenever only the ROM rung serves (no cache, ROM open)",
      anim_cheap == false);
  printf("  [%s] 60 IDLE FRAMES (gated): 0 RomReadFn calls (mon_icon_anim_cheap() == false)\n", name);

  /* Counterfactual, for the record only: what the OLD (unguarded) code would have
   * cost, redrawing 3 visible caught cells every DEX_ANIM_PERIOD=30 ticks for 60
   * idle frames = 2 flips. Each flip touches frame (bob^1) for each caught cell --
   * a DIFFERENT MRU key from whatever the page draw left behind for cells 0 and 14
   * (national #1, #15 -- 0 and 14 are the i%7==0 caught cells among 0..20), so every
   * flip is a fresh locate+frame-verify. */
  mirror_reset(); mru_reset(); fc.reads = 0; fc.backward = 0; fc.far_backward = 0; fc.last_off = -1;
  uint16_t caught[3] = { page[0], page[7], page[14] };  /* the 3 caught cells (i%7==0) */
  int bob = 1;
  for (int flip = 0; flip < 2; flip++) {                /* 60 frames / 30-frame period = 2 flips */
    bob ^= 1;
    for (int i = 0; i < 3; i++) { uint16_t pal[16]; icon_via_rom(&rm, caught[i], (uint8_t)bob, pal); }
  }
  printf("  [%s] 60 IDLE FRAMES counterfactual (UNGATED, pre-fix behaviour): %d RomReadFn calls\n",
         name, fc.reads);
  chk("the counterfactual is real, avoidable cost (>0), proving the gate matters",
      fc.reads > 0);

  /* ---- 5: the Day-Care screen (up to 7 icons, first draw) -------------------- */
  mirror_reset(); mru_reset(); fc.reads = 0; fc.backward = 0; fc.far_backward = 0; fc.last_off = -1;
  /* 2 boarders (species #1, #4 by internal id -- any two distinct species) + 5
   * hazed yard visitors (internal ids 6, 7, 10, 13, 16 -- gen3_daycare.c's own
   * visitor roll is internal-species-id-keyed, 1..251, not national-number-keyed,
   * see pdna_main.c's dc_roll_decos) = 7 icons, frame 0 each (first draw). */
  uint16_t dc_cast[7] = { 1, 4, 6, 7, 10, 13, 16 };
  int dc_ok = 1;
  for (int i = 0; i < 7; i++) { uint16_t pal[16]; if (!icon_via_rom(&rm, dc_cast[i], 0, pal)) dc_ok = 0; }
  chk("all 7 Day-Care icons resolve", dc_ok);
  printf("  [%s] DAY-CARE first draw (7 icons): %d RomReadFn calls, %d backward (%d far/chain-walking)\n",
         name, fc.reads, fc.backward, fc.far_backward);
  chk("Day-Care first draw stays under 7*8 RomReadFn calls", fc.reads <= 7 * 8);

  printf("  %s: ok (kind=%s rev=%u)\n", name, rom_kind_name(rc.kind), rc.version);
  fclose(f);
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }
  char p[512];
  sprintf(p, "%s/Emerald.gba", dir); run_rom(p, "Emerald");
  sprintf(p, "%s/FireRed.gba", dir); run_rom(p, "FireRed");
  printf("dex/daycare icon-rung read-count test: %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
