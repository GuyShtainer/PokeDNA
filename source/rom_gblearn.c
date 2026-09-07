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
#define GBL_FPSO_BLK     64u  /* G1 review MEDIUM-2: full_pointer_shape_ok's own
                               * read-block size (32 pointers/read instead of 1) --
                               * see that function's own comment */

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
 * 370..1364 of them) down to the 1..4 that reach the expensive phase C.
 *
 * G1 review MEDIUM-2 (2026-09-08): used to read two bytes at a time, one
 * read() call per pointer -- cheap arithmetic, but each call is a real f_read
 * (a sector-sized fetch on a real cartridge) regardless of how few bytes it
 * asks for, and this runs over EVERY (offset, n) pair phase A's own cheap
 * prefilter lets through (hundreds on a real ROM). Measured contribution to
 * gb_create_hook's own full scan: a large share of the ~118,000 (Gold.gbc) /
 * ~185,000 (Crystal.gbc) read() calls CREATE used to make. Reads
 * GBL_FPSO_BLK (64) bytes -- 32 pointers -- per call instead, the same
 * "buffer through a small stack chunk" idiom scan_phase_a already uses; the
 * increasing/bounded-delta/in-window checks themselves are byte-for-byte
 * unchanged, just fed from `blk` instead of a fresh 2-byte read each time. */
static int full_pointer_shape_ok(GbReadFn read, void* ctx, uint32_t table_off, uint32_t n) {
  uint16_t prev = 0;
  uint8_t blk[GBL_FPSO_BLK];
  for (uint32_t i = 0; i < n; ) {
    uint32_t want = n - i;
    if (want * 2u > GBL_FPSO_BLK) want = GBL_FPSO_BLK / 2u;
    if (!read(ctx, table_off + i * 2u, blk, want * 2u)) return 0;
    for (uint32_t j = 0; j < want; j++, i++) {
      uint16_t v = rd16(blk + j * 2u);
      if (v < GB_WIN_LO || v >= GB_WIN_HI) return 0;
      if (i > 0) {
        int d = (int)v - (int)prev;
        if (d < 1 || (uint32_t)d > GBL_MAX_DELTA) return 0;
      }
      prev = v;
    }
  }
  return 1;
}

/* Walk the evolution list at `off`, validating every method byte, and return
 * the offset just past its terminating 0 -- where the move list starts --  or
 * 0 on any structural violation. Doubles as rom_gblearn_min_level()'s own
 * predecessor search: when `want_idx` is non-zero, the FIRST entry (if any)
 * whose OWN target byte equals it also reports whether its method carries a
 * real level requirement and, if so, what it is (`*found`/`*is_level`/
 * `*level`; any of the three may be NULL to not ask). This is deliberately
 * ONE "skip an evolution entry" loop shared by both callers rather than two
 * parallel copies that could quietly drift apart (this tree's own history:
 * fccf29d, 16078af) -- walk_entry() below calls this with `want_idx = 0` and
 * every out-param NULL, which short-circuits the match-check entirely (one
 * extra NULL test per entry, no extra reads) and is byte-for-byte the
 * original skip-only loop's behaviour. */
static uint32_t evo_skip(GbReadFn read, void* ctx, uint32_t bank_hi, uint32_t off,
                         uint8_t gen, uint32_t want_idx, bool* found,
                         bool* is_level, uint8_t* level) {
  uint32_t cur = off;
  if (found) *found = false;
  for (int guard = 0; guard < 16; guard++) {
    uint8_t method;
    if (cur >= bank_hi || !read(ctx, cur, &method, 1)) return 0;
    uint32_t entry_off = cur;
    cur++;
    if (method == 0) return cur;
    int len = evo_entry_len(gen, method);
    if (len < 0) return 0;
    if (found && !*found && want_idx) {
      uint8_t last;
      if (entry_off + (uint32_t)len - 1u >= bank_hi ||
          !read(ctx, entry_off + (uint32_t)len - 1u, &last, 1)) return 0;
      if ((uint32_t)last == want_idx) {
        bool lvl_method = (method == 1u) || (gen == GB_GEN2 && method == 5u);
        if (is_level) *is_level = lvl_method;
        if (lvl_method) {
          uint8_t b1;
          if (entry_off + 1u >= bank_hi || !read(ctx, entry_off + 1u, &b1, 1)) return 0;
          if (level) *level = b1;
        } else if (level) *level = 0;
        *found = true;
      }
    }
    cur = entry_off + (uint32_t)len;
    if (guard == 15) return 0;             /* no real species has 16 evolution steps */
  }
  return 0;
}

/* Decode ONE evos+moves/evos+attacks blob at `off` (already resolved into a
 * flat file offset, inside `bank`'s window). Skips evolutions via evo_skip();
 * walks the (level, move) pairs keeping a 4-slot FIFO of
 * moves with level <= `level_cap` (oldest dropped first -- see rom_gblearn.h).
 *
 * `seed4` (may be NULL), if given, PRE-LOADS the FIFO before the walk starts,
 * as if those (up to 4, 0 = unused, left-packed) moves were already known --
 * rom_gblearn_moves_at_seeded()'s whole reason to exist: Gen 1's base-stats
 * starters are effectively "known before level 1", so a move the table
 * re-teaches later must dedupe against them the SAME way it dedupes against
 * an earlier table entry (below), not just against other table entries.
 * Without this, gb_new_mon_g1_moves() could handed back the SAME move twice
 * (once as a starter, once from the table) for any species whose table
 * relists a move it already starts with -- measured: Nidoqueen, Nidoking and
 * Kabutops all do, in Guy's own Red.gb.
 *
 * `*total_moves` (may be NULL) gets the count of ALL legal move pairs seen,
 * ignoring `level_cap` -- gbl_verify()'s "does this look like a real learnset"
 * bar, and nothing else. Returns the number of moves kept in `out4` (0..4), or
 * -1 on any structural violation: an unrecognised method, an out-of-range level
 * or move id, or running off the end of the 16 KiB bank. Both guard counts
 * (16 evolutions, 32 move pairs) are generous multiples of the busiest real row
 * this module has decoded (2 evolutions, 14 moves) -- real data will never
 * approach them, so hitting one means the data is not what it claims to be. */
static int walk_entry(GbReadFn read, void* ctx, uint32_t bank_hi, uint32_t off,
                      uint8_t gen, uint8_t level_cap, const uint8_t seed4[4],
                      uint8_t out4[4], int* total_moves) {
  uint32_t cur = evo_skip(read, ctx, bank_hi, off, gen, 0, NULL, NULL, NULL);
  if (!cur) return -1;

  uint8_t kept_moves[4] = { 0, 0, 0, 0 };
  int maxmove = (int)gb_max_move(gen);
  int kept = 0, seen = 0;
  if (seed4) while (kept < 4 && seed4[kept]) { kept_moves[kept] = seed4[kept]; kept++; }
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
      /* A handful of real species (Metapod/Kakuna's Harden, Smeargle's Sketch)
       * genuinely relist the SAME move at a later level -- verified against
       * Gold.gbc/Crystal.gbc, not a parsing artifact. The real games skip
       * re-teaching a move already known rather than create a duplicate slot
       * (well-documented Gen-1/2 engine behaviour), so a move already present
       * among the CURRENTLY KEPT four is dropped here too. This is deliberately
       * NOT a check against the whole history: a move bumped out of the four
       * earlier is, exactly as in the games, treated as forgotten and can be
       * relearned for real if the table lists it again later. */
      bool already_known = false;
      for (int k = 0; k < kept; k++) if (kept_moves[k] == mv) { already_known = true; break; }
      if (!already_known) {
        if (kept < 4) {
          kept_moves[kept++] = mv;
        } else {
          kept_moves[0] = kept_moves[1]; kept_moves[1] = kept_moves[2];
          kept_moves[2] = kept_moves[3]; kept_moves[3] = mv;
        }
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
    if (walk_entry(read, ctx, bank_hi, off, gen, 255u, NULL, NULL, &seen) < 0) return -1;
    if (seen > 0) with_moves++;
  }
  if (!distinct) return -1;
  return with_moves;
}

/* Measured on Guy's four dumps: the phase-A+B prefilter (full-N pointer shape,
 * see full_pointer_shape_ok) leaves exactly 1..4 survivors. 8 is double that
 * for headroom; MORE than 8 is refused as ambiguous rather than silently
 * checking only the first 8 -- the same "unique hit or refuse" doctrine as
 * everywhere else in this module, just applied to the collection step too. */
#define GBL_MAX_HITS 8u

typedef struct { uint32_t off[GBL_MAX_HITS]; uint32_t n; } GblHits;

typedef struct { GbReadFn read; void* ctx; uint32_t n; GblHits* hits; } GblCollectCtx;

/* Phase A's own callback: runs phase B (full_pointer_shape_ok) INLINE -- cheap,
 * pure arithmetic over 2*n bytes, no big locals of its own -- and only STORES a
 * survivor of THAT. This is load-bearing, not an optimisation: phase A alone
 * (prefilter_hit, the first GBL_PREFILTER_K pointers only) lets through
 * hundreds of coincidental hits over a multi-MB ROM (measured: 370..1364 on
 * Guy's four dumps); collecting THOSE into GBL_MAX_HITS(8) would overflow the
 * "too many, refuse" bound on every real ROM before phase B ever narrowed
 * anything down to the 1..4 it actually leaves. What phase B's own cheap check
 * does NOT do -- pull in the expensive per-candidate decode (gbl_verify ->
 * walk_entry, which is what actually reads and parses evolution/move bytes) --
 * is deferred to AFTER scan_phase_a has returned and its 568 B frame is gone,
 * the same "cheap scan now, expensive verify later, never both nested" shape
 * rom_gbsprite.c's own locate()/scan_multi() pair uses. */
static void gbl_collect_hit(void* user, uint32_t off) {
  GblCollectCtx* cc = (GblCollectCtx*)user;
  if (!full_pointer_shape_ok(cc->read, cc->ctx, off, cc->n)) return;
  GblHits* h = cc->hits;
  if (h->n < GBL_MAX_HITS) h->off[h->n] = off;
  h->n++;
}

/* Phase C for ONE already phase-B-shape-checked candidate (gbl_collect_hit
 * already ran full_pointer_shape_ok before this was ever stored, so it is not
 * repeated here): the primary bank guess -- the table's own bank, what every
 * real ROM this module has been checked against actually uses (rom_gblearn.h's
 * sanity addresses) -- then a full bank sweep only if that guess does not
 * clear the "real learnset" bar. Returns 1 and fills `*out_bank` on a hit
 * clearing the bar, 0 otherwise. */
static int gbl_verify_candidate(GbReadFn read, void* ctx, uint32_t size, uint32_t off,
                                uint8_t gen, uint32_t n, uint8_t* out_bank) {
  uint8_t primary = (uint8_t)(off / GB_BANK);
  int wm = gbl_verify(read, ctx, size, off, primary, gen, n);
  uint8_t bank = primary;
  if (wm < (int)(n / 2u)) {
    uint32_t banks = size / GB_BANK;
    int best = wm; uint8_t best_bank = primary;
    for (uint32_t b = 1; b < banks; b++) {
      int wm2 = gbl_verify(read, ctx, size, off, (uint8_t)b, gen, n);
      if (wm2 > best) { best = wm2; best_bank = (uint8_t)b; }
    }
    wm = best; bank = best_bank;
  }
  if (wm < (int)(n / 2u)) return 0;
  *out_bank = bank;
  return 1;
}

int rom_gblearn_open(RomGbLearn* rl, uint8_t gen, GbReadFn read, void* ctx, uint32_t size) {
  if (!rl) return 0;
  memset(rl, 0, sizeof *rl);
  if (!read) return 0;
  if (gen != GB_GEN1 && gen != GB_GEN2) return 0;
  if (size == 0 || size % GB_BANK != 0) return 0;
  uint32_t banks = size / GB_BANK;
  if (banks < 2u || banks > 255u) return 0;      /* fixed bank 0 + >=1 switchable */

  uint32_t n = (gen == GB_GEN1) ? GBL_G1_N : GBL_G2_N;

  GblHits hits;
  memset(&hits, 0, sizeof hits);
  GblCollectCtx cc; cc.read = read; cc.ctx = ctx; cc.n = n; cc.hits = &hits;
  scan_phase_a(read, ctx, size, n, gbl_collect_hit, &cc);
  if (hits.n == 0u || hits.n > GBL_MAX_HITS) return 0;   /* none, or too many to trust */

  uint32_t found = 0, good_off = 0; uint8_t good_bank = 0;
  for (uint32_t h = 0; h < hits.n; h++) {
    uint8_t bank;
    if (gbl_verify_candidate(read, ctx, size, hits.off[h], gen, n, &bank)) {
      found++; good_off = hits.off[h]; good_bank = bank;
    }
  }
  if (found != 1u) return 0;                     /* zero or ambiguous: refuse, cleanly */

  rl->gen = gen; rl->read = read; rl->ctx = ctx; rl->size = size;
  rl->banks = (uint8_t)banks; rl->table_off = good_off; rl->data_bank = good_bank;
  rl->ok = true;
  return 1;
}

int rom_gblearn_moves_at_seeded(RomGbLearn* rl, uint16_t dex, uint8_t level,
                                const uint8_t seed4[4], uint8_t out4[4]) {
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

  return walk_entry(rl->read, rl->ctx, bank_lo + GB_BANK, off, rl->gen, level, seed4, out4, NULL);
}

int rom_gblearn_moves_at(RomGbLearn* rl, uint16_t dex, uint8_t level, uint8_t out4[4]) {
  return rom_gblearn_moves_at_seeded(rl, dex, level, NULL, out4);
}

/* Guard against a cyclic/corrupt predecessor chain (A evolves into B evolves
 * into A). No real Gen-1/2 species is more than two evolution steps from its
 * own base form -- this is double that, for headroom, the same margin every
 * other guard count in this file uses. */
#define GBL_MINLV_MAX_HOPS 8

uint8_t rom_gblearn_min_level(RomGbLearn* rl, uint16_t dex) {
  const uint8_t base = 5u;                     /* G3_BUILD_BASE_LVL's own value, mirrored */
  if (!rl || !rl->ok) return base;
  uint32_t n = (rl->gen == GB_GEN1) ? GBL_G1_N : GBL_G2_N;
  uint32_t cur_idx = (rl->gen == GB_GEN1) ? gb_index_from_dex(GB_GEN1, dex) : dex;
  if (cur_idx < 1u || cur_idx > n) return base;

  uint32_t bank_lo = (uint32_t)rl->data_bank * GB_BANK, bank_hi = bank_lo + GB_BANK;
  uint8_t floor = base;
  for (int hop = 0; hop < GBL_MINLV_MAX_HOPS; hop++) {
    uint32_t pred_idx = 0; bool pred_is_level = false; uint8_t pred_level = 0;
    for (uint32_t s = 1; s <= n; s++) {
      uint8_t p[2];
      if (!rl->read(rl->ctx, rl->table_off + (s - 1u) * 2u, p, 2)) break;   /* read hiccup: stop, keep floor so far */
      uint16_t addr = rd16(p);
      if (addr < GB_WIN_LO || addr >= GB_WIN_HI) continue;
      uint32_t off = bank_lo + (addr - GB_WIN_LO);
      if (off >= rl->size) continue;
      bool found = false, lvlflag = false; uint8_t lvl = 0;
      evo_skip(rl->read, rl->ctx, bank_hi, off, rl->gen, cur_idx, &found, &lvlflag, &lvl);
      if (found) { pred_idx = s; pred_is_level = lvlflag; pred_level = lvl; break; }
    }
    if (!pred_idx) break;                      /* cur_idx is a base form: stop, keep floor */
    if (pred_is_level && pred_level > floor) floor = pred_level;
    cur_idx = pred_idx;
  }
  return floor;
}
