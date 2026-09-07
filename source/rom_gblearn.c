/* Gen-1/2 level-up learnsets, read live out of the user's own cartridge dump --
 * see rom_gblearn.h for the design, the citations and the shape this locates. */
#include <string.h>

#include "rom_gblearn.h"

#define GB_BANK    0x4000u
#define GB_WIN_LO  0x4000u
#define GB_WIN_HI  0x8000u

#define GBL_G1_N       190u
#define GBL_G2_N       251u
#define GBL_SCAN_BUF   512u   /* Phase-A chunk size; smaller than rom_gbsprite.c's
                               * 2048 B ROM_GBSPRITE_SCRATCH_MIN on purpose -- this
                               * struct can share a stack frame with a RomGbSprite
                               * (item 3's budget), trading some scan speed for it */
#define GBL_PREFILTER_K  24u  /* Phase-A pointers checked per byte position       */
#define GBL_MAX_DELTA    48u  /* Phase-B/A: max bytes between two consecutive per-
                               * species blobs -- the busiest real blob measured is
                               * ~35 B (14 moves + a 2-method evolution + 2 terms) */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

/* gen 1: methods 1..3, EVOLVE_ITEM (2) is 4 B, else 3.
 * gen 2: methods 1..5, EVOLVE_STAT (5) is 4 B, else 3 -- Gen 2's own EVOLVE_ITEM
 * is 3 B, ONE LESS than Gen 1's; verified against the decomp source, not
 * assumed from Gen 1's shape (see rom_gblearn.h). Returns -1 for a method byte
 * neither generation defines. */
static int evo_entry_len(uint8_t gen, uint8_t method) {
  uint8_t maxm = (gen == GB_GEN1) ? 3u : 5u;
  if (method < 1u || method > maxm) return -1;
  if (gen == GB_GEN1) return (method == 2u) ? 4 : 3;
  return (method == 5u) ? 4 : 3;
}

/* Phase A's per-position check: are the first K pointers starting at `w` all in
 * the ROMX window, strictly increasing, and no more than GBL_MAX_DELTA apart?
 * Cheap and simple, so it can run at every byte offset of the whole ROM; it is
 * also cheap because it is WRONG constantly -- rejecting fast on the very first
 * pointer is the common case, which is the point of checking this before ever
 * reading a real evolution/move byte. `w` must have 2*K bytes available. */
static int prefilter_hit(const uint8_t* w) {
  uint16_t prev = 0;
  for (uint32_t k = 0; k < GBL_PREFILTER_K; k++) {
    uint16_t v = rd16(w + k * 2u);
    if (v < GB_WIN_LO || v >= GB_WIN_HI) return 0;
    if (k > 0) {
      int d = (int)v - (int)prev;
      if (d < 1 || (uint32_t)d > GBL_MAX_DELTA) return 0;
    }
    prev = v;
  }
  return 1;
}

/* One pass over the whole ROM, chunked through a small stack buffer (mirrors
 * rom_gbsprite.c's scan_multi, one job instead of six): every byte offset that
 * could start an n-pointer table gets prefilter_hit(). `on_hit` is called
 * immediately for each survivor rather than collecting them, so this needs no
 * array sized for "however many the ROM happens to contain" -- observed 1..4
 * on Guy's four dumps, but nothing here assumes that stays true. Bounded by
 * `size` (a real file length, so the loop provably terminates). */
typedef void (*GblHitFn)(void* user, uint32_t off);

static void scan_phase_a(GbReadFn read, void* ctx, uint32_t size, uint32_t n,
                         GblHitFn on_hit, void* user) {
  uint8_t buf[GBL_SCAN_BUF];
  uint32_t look = GBL_PREFILTER_K * 2u;
  uint32_t cap = sizeof buf;
  if (size < n * 2u || cap <= look) return;
  uint32_t last_start = size - n * 2u;
  uint32_t step = cap - look + 1u;
  for (uint32_t base = 0; base <= last_start; base += step) {
    uint32_t want = cap;
    if (base + want > size) want = size - base;
    if (want < look) break;                       /* not enough left for one check */
    if (!read(ctx, base, buf, want)) return;
    uint32_t lim = want - look;
    uint32_t cap_by_table = (base <= last_start) ? last_start - base : 0;
    if (lim > cap_by_table) lim = cap_by_table;
    for (uint32_t i = 0; i <= lim; i++) {
      if (prefilter_hit(buf + i)) on_hit(user, base + i);
    }
    if (base + step < base) break;                 /* defensive: no overflow wrap */
  }
}

/* Phase B: the SAME check as prefilter_hit, extended over EVERY one of the n
 * pointers (not just the first K) -- still pure arithmetic on the pointer
 * bytes, no evolution/move decode yet, so still cheap. This is what cuts the
 * "38 in the first K happened to line up" survivors of phase A (measured:
 * 370..1364 of them) down to the 1..4 that reach the expensive phase C. Reads
 * two bytes at a time rather than buffering all n*2 -- n tops out at 502 B for
 * Gen 2, which is fine for a stack buffer but this needs none at all. */
static int full_pointer_shape_ok(GbReadFn read, void* ctx, uint32_t table_off, uint32_t n) {
  uint16_t prev = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t p[2];
    if (!read(ctx, table_off + i * 2u, p, 2)) return 0;
    uint16_t v = rd16(p);
    if (v < GB_WIN_LO || v >= GB_WIN_HI) return 0;
    if (i > 0) {
      int d = (int)v - (int)prev;
      if (d < 1 || (uint32_t)d > GBL_MAX_DELTA) return 0;
    }
    prev = v;
  }
  return 1;
}

/* Decode ONE evos+moves/evos+attacks blob at `off` (already resolved into a
 * flat file offset, inside `bank`'s window). Skips evolutions after validating
 * each method byte; walks the (level, move) pairs keeping a 4-slot FIFO of
 * moves with level <= `level_cap` (oldest dropped first -- see rom_gblearn.h).
 * `*total_moves` (may be NULL) gets the count of ALL legal move pairs seen,
 * ignoring `level_cap` -- gbl_verify()'s "does this look like a real learnset"
 * bar, and nothing else. Returns the number of moves kept in `out4` (0..4), or
 * -1 on any structural violation: an unrecognised method, an out-of-range level
 * or move id, or running off the end of the 16 KiB bank. Both guard counts
 * (16 evolutions, 32 move pairs) are generous multiples of the busiest real row
 * this module has decoded (2 evolutions, 14 moves) -- real data will never
 * approach them, so hitting one means the data is not what it claims to be. */
static int walk_entry(GbReadFn read, void* ctx, uint32_t bank_hi, uint32_t off,
                      uint8_t gen, uint8_t level_cap, uint8_t out4[4], int* total_moves) {
  uint32_t cur = off;
  for (int guard = 0; guard < 16; guard++) {
    uint8_t method;
    if (cur >= bank_hi || !read(ctx, cur, &method, 1)) return -1;
    cur++;
    if (method == 0) break;
    int len = evo_entry_len(gen, method);
    if (len < 0) return -1;
    cur += (uint32_t)(len - 1);            /* the method byte itself already consumed */
    if (guard == 15) return -1;             /* no real species has 16 evolution steps */
  }

  uint8_t kept_moves[4] = { 0, 0, 0, 0 };
  int maxmove = (int)gb_max_move(gen);
  int kept = 0, seen = 0;
  for (int guard = 0; guard < 32; guard++) {
    uint8_t lvl;
    if (cur >= bank_hi || !read(ctx, cur, &lvl, 1)) return -1;
    cur++;
    if (lvl == 0) break;
    if (lvl > 100u) return -1;
    uint8_t mv;
    if (cur >= bank_hi || !read(ctx, cur, &mv, 1)) return -1;
    cur++;
    if (mv < 1u || (int)mv > maxmove) return -1;
    seen++;
    if (lvl <= level_cap) {
      if (kept < 4) {
        kept_moves[kept++] = mv;
      } else {
        kept_moves[0] = kept_moves[1]; kept_moves[1] = kept_moves[2];
        kept_moves[2] = kept_moves[3]; kept_moves[3] = mv;
      }
    }
    if (guard == 31) return -1;             /* no real species has 32 level-up moves */
  }
  if (out4) memcpy(out4, kept_moves, sizeof kept_moves);
  if (total_moves) *total_moves = seen;
  return kept;
}

/* Phase C: decode all n entries of a candidate (table_off, bank). Returns the
 * number carrying at least one real move (the "is this a genuine learnset, not
 * a coincidence over blank ROM space" bar -- see rom_gblearn.h), or -1 on the
 * first structural violation OR if every entry pointed at the exact same
 * address (the degenerate all-zero-bank false positive this locator's first
 * draft found in Red.gb, where "0 evolutions, 0 moves" trivially validates). */
static int gbl_verify(GbReadFn read, void* ctx, uint32_t size, uint32_t table_off,
                      uint8_t bank, uint8_t gen, uint32_t n) {
  uint32_t bank_lo = (uint32_t)bank * GB_BANK, bank_hi = bank_lo + GB_BANK;
  uint32_t first_target = 0;
  int distinct = 0, with_moves = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t p[2];
    if (!read(ctx, table_off + i * 2u, p, 2)) return -1;
    uint16_t addr = rd16(p);
    if (addr < GB_WIN_LO || addr >= GB_WIN_HI) return -1;
    uint32_t off = bank_lo + (addr - GB_WIN_LO);
    if (off >= size) return -1;
    if (i == 0) first_target = off;
    else if (off != first_target) distinct = 1;
    int seen = 0;
    if (walk_entry(read, ctx, bank_hi, off, gen, 255u, NULL, &seen) < 0) return -1;
    if (seen > 0) with_moves++;
  }
  if (!distinct) return -1;
  return with_moves;
}

typedef struct {
  GbReadFn read; void* ctx; uint32_t size; uint8_t gen; uint32_t n;
  uint32_t found; uint32_t good_off; uint8_t good_bank;
} GblScanState;

static void gbl_on_hit(void* user, uint32_t off) {
  GblScanState* st = (GblScanState*)user;
  if (!full_pointer_shape_ok(st->read, st->ctx, off, st->n)) return;

  /* The primary guess -- the table's own bank -- is what every real ROM this
   * module has been checked against actually uses (rom_gblearn.h's sanity
   * addresses); only fall back to a full bank sweep if that guess does not
   * clear the "real learnset" bar. */
  uint8_t primary = (uint8_t)(off / GB_BANK);
  int wm = gbl_verify(st->read, st->ctx, st->size, off, primary, st->gen, st->n);
  uint8_t bank = primary;
  if (wm < (int)(st->n / 2u)) {
    uint32_t banks = st->size / GB_BANK;
    int best = wm; uint8_t best_bank = primary;
    for (uint32_t b = 1; b < banks; b++) {
      int wm2 = gbl_verify(st->read, st->ctx, st->size, off, (uint8_t)b, st->gen, st->n);
      if (wm2 > best) { best = wm2; best_bank = (uint8_t)b; }
    }
    wm = best; bank = best_bank;
  }
  if (wm >= (int)(st->n / 2u)) {
    st->found++;
    st->good_off = off;
    st->good_bank = bank;
  }
}

int rom_gblearn_open(RomGbLearn* rl, uint8_t gen, GbReadFn read, void* ctx, uint32_t size) {
  if (!rl) return 0;
  memset(rl, 0, sizeof *rl);
  if (!read) return 0;
  if (gen != GB_GEN1 && gen != GB_GEN2) return 0;
  if (size == 0 || size % GB_BANK != 0) return 0;
  uint32_t banks = size / GB_BANK;
  if (banks < 2u || banks > 255u) return 0;      /* fixed bank 0 + >=1 switchable */

  GblScanState st;
  memset(&st, 0, sizeof st);
  st.read = read; st.ctx = ctx; st.size = size; st.gen = gen;
  st.n = (gen == GB_GEN1) ? GBL_G1_N : GBL_G2_N;

  scan_phase_a(read, ctx, size, st.n, gbl_on_hit, &st);
  if (st.found != 1u) return 0;                  /* zero or ambiguous: refuse, cleanly */

  rl->gen = gen; rl->read = read; rl->ctx = ctx; rl->size = size;
  rl->banks = (uint8_t)banks; rl->table_off = st.good_off; rl->data_bank = st.good_bank;
  rl->ok = true;
  return 1;
}

int rom_gblearn_moves_at(RomGbLearn* rl, uint16_t dex, uint8_t level, uint8_t out4[4]) {
  if (!rl || !out4 || !rl->ok) return -1;
  uint32_t n = (rl->gen == GB_GEN1) ? GBL_G1_N : GBL_G2_N;
  uint32_t idx = (rl->gen == GB_GEN1) ? gb_index_from_dex(GB_GEN1, dex) : dex;
  if (idx < 1u || idx > n) return -1;

  uint8_t p[2];
  if (!rl->read(rl->ctx, rl->table_off + (idx - 1u) * 2u, p, 2)) return -1;
  uint16_t addr = rd16(p);
  if (addr < GB_WIN_LO || addr >= GB_WIN_HI) return -1;
  uint32_t bank_lo = (uint32_t)rl->data_bank * GB_BANK;
  uint32_t off = bank_lo + (addr - GB_WIN_LO);
  if (off >= rl->size) return -1;

  int kept = walk_entry(rl->read, rl->ctx, bank_lo + GB_BANK, off, rl->gen, level, out4, NULL);
  return kept;
}
