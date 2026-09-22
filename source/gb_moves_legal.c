/* gb_moves_legal.c -- BACKLOG #150 S150-10. See the header for the bound, the
 * Struggle guard and the pack-rule justification. */
#include "gb_moves_legal.h"

#include "gen3_mon.h"   /* PkMon, pk_decode_mon -- g3gb_moves_ok_rec only */

static bool gen_ok(uint8_t gen) { return gen == GB_GEN1 || gen == GB_GEN2; }

int g3gb_moves_ok(const uint16_t moves[4], uint8_t gen, uint8_t bad4[4]) {
  if (!moves || !bad4 || !gen_ok(gen)) return -1;
  uint8_t bound = gb_max_move(gen);
  int n = 0;
  for (int i = 0; i < 4; i++) {
    bad4[i] = (moves[i] != 0 && moves[i] > bound) ? 1u : 0u;
    if (bad4[i]) n++;
  }
  return n;
}

int g3gb_moves_ok_rec(const uint8_t rec80[80], uint8_t gen, uint8_t bad4[4]) {
  if (!rec80) return -1;
  PkMon m;
  if (!pk_decode_mon(rec80, false, &m) || m.isEgg || m.isBadEgg) return -1;
  return g3gb_moves_ok(m.moves, gen, bad4);
}

int g3gb_moves_fill(GbEditMon* out, const uint8_t bad4[4], const uint8_t learn4[4],
                     uint8_t fill4[4]) {
  if (!out || !bad4 || !learn4 || !fill4 || !gen_ok(out->gen)) return -1;
  uint8_t bound = gb_max_move(out->gen);
  uint8_t kept[4] = {0};       /* the non-bad slots' own moves, for the dedup check */
  for (int i = 0; i < 4; i++) fill4[i] = 0;
  for (int i = 0; i < 4; i++) if (!bad4[i]) kept[i] = gb_get_move(out, i);

  int filled = 0;
  for (int i = 0; i < 4; i++) {
    if (!bad4[i]) continue;
    uint8_t mv = 0;
    for (int j = 0; j < 4; j++) {
      uint8_t cand = learn4[j];
      if (!cand || cand == 165u || cand > bound) continue;          /* empty / Struggle / out-of-bound */
      bool used = false;
      for (int k = 0; k < 4; k++) if (kept[k] == cand) { used = true; break; }
      for (int k = 0; k < 4; k++) if (fill4[k] == cand) { used = true; break; }
      if (used) continue;
      mv = cand;
      break;
    }
    if (mv) { fill4[i] = mv; filled++; }
  }
  for (int i = 0; i < 4; i++) {
    if (!bad4[i] || !fill4[i]) continue;
    /* Cannot fail (rule 7 -- checked, not silently ignored): fill4[i] only ever holds
     * a `cand` already screened `cand <= bound` above, and i is one of this loop's own
     * fixed 0..3 indices -- gb_set_move's only two refusal conditions. */
    (void)gb_set_move(out, i, fill4[i]);      /* fresh base PP, 0 PP-Ups */
  }
  g3gb_moves_pack(out);
  return filled;
}

bool g3gb_moves_pack(GbEditMon* e) {
  if (!e) return false;
  bool moved = false;
  int dst = 0;
  for (int src = 0; src < 4; src++) {
    uint8_t mv = gb_get_move(e, src);
    if (!mv) continue;
    if (src != dst) {
      uint8_t pp = gb_get_pp(e, src), ups = gb_get_ppup(e, src);
      /* Cannot fail: `mv` was just read back via gb_get_move from a slot that already
       * held it (never invents a move), and dst/src are this loop's own bounded 0..3
       * indices -- gb_set_move's only two refusal conditions. */
      (void)gb_set_move(e, dst, mv);
      gb_set_ppup(e, dst, ups);
      gb_set_pp(e, dst, pp);
      (void)gb_set_move(e, src, 0);            /* 0 is never > any gb_max_move() bound */
      moved = true;
    }
    dst++;
  }
  return moved;
}
