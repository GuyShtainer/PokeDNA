/*
 * bank_collision — pure ident32-collision scan for the Bank's 16 boxes.
 *
 * Extracted out of drop_held() (pdna_box.c) so BACKLOG #168's 16-box scan is a
 * pure-C core (golden rule: algorithm logic free of tonc/GBA headers) that host
 * tests can exercise on a synthesized Bank directly, without linking pdna_box.c or
 * pdna_bank.c (both tonc-dependent).
 */
#include <string.h>

#include "bank_collision.h"

bool bank_ident32_collision(BankBoxGetter get_box, void* ctx,
                             int num_boxes, int slots_per_box,
                             int self_box, int self_slot,
                             const uint8_t ident8[8],
                             int* out_box, int* out_slot) {
  if (!get_box || !ident8 || num_boxes <= 0 || slots_per_box <= 0) return false;   /* rule 7: validate parameters */
  for (int b = 0; b < num_boxes; b++) {                 /* rule 2: bounded by caller-supplied num_boxes (16) */
    const uint8_t* recs = get_box(b, ctx);
    if (!recs) continue;                                 /* unreadable/never-written box: nothing to collide with */
    for (int s = 0; s < slots_per_box; s++) {             /* rule 2: bounded by caller-supplied slots_per_box (30) */
      if (b == self_box && s == self_slot) continue;      /* the destination cell itself */
      if (memcmp(recs + (uint32_t)s * 80, ident8, 8) == 0) {
        if (out_box) *out_box = b;
        if (out_slot) *out_slot = s;
        return true;
      }
    }
  }
  return false;
}
