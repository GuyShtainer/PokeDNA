#include "gen3_chunk.h"
#include <string.h>

int chunk_build(Chunk* c, const uint8_t occ[G3_BOX_SLOTS], int a, int b) {
  memset(c, 0, sizeof *c);
  if (a < 0) a = 0; else if (a >= G3_BOX_SLOTS) a = G3_BOX_SLOTS - 1;
  if (b < 0) b = 0; else if (b >= G3_BOX_SLOTS) b = G3_BOX_SLOTS - 1;
  int ar = g3_row(a), ac = g3_col(a), brr = g3_row(b), bc = g3_col(b);
  int r0 = ar < brr ? ar : brr, r1 = ar < brr ? brr : ar;
  int c0 = ac < bc  ? ac : bc,  c1 = ac < bc  ? bc  : ac;
  c->h = r1 - r0 + 1;
  c->w = c1 - c0 + 1;
  int n = 0;
  for (int r = r0; r <= r1; r++)
    for (int col = c0; col <= c1; col++) {
      int s = g3_slot(r, col);
      if (occ[s]) {
        c->rr[n]  = (uint8_t)(r - r0);
        c->cc[n]  = (uint8_t)(col - c0);
        c->src[n] = (uint8_t)s;
        n++;
      }
    }
  c->n = n;
  return n;
}

bool chunk_can_drop(const Chunk* c, int tr, int tc, const uint8_t dest[G3_BOX_SLOTS],
                    const uint8_t* vac, uint8_t tgt[G3_BOX_SLOTS]) {
  if (c->n <= 0) return false;
  if (tr < 0 || tc < 0) return false;
  if (tr + c->h > G3_BOX_ROWS || tc + c->w > G3_BOX_COLS) return false;   /* off-grid footprint */
  for (int i = 0; i < c->n; i++) {
    int ts = g3_slot(tr + c->rr[i], tc + c->cc[i]);
    if (dest[ts] && !(vac && vac[ts])) return false;                      /* target cell blocked */
    tgt[i] = (uint8_t)ts;
  }
  return true;
}
