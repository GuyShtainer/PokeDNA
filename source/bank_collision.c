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
#include "bank_cell.h"   /* BACKLOG #223: BC_OFF_BANK_SERIAL, bc_is_native -- pure C, same class of module */

bool bank_scan_serial_and_clash(BankBoxGetter get_box, void* ctx,
                                int num_boxes, int slots_per_box,
                                int self_box, int self_slot,
                                const uint8_t held80[80],
                                uint32_t* max_out, int* coll_box, int* coll_slot) {
  if (max_out) *max_out = 0;                                          /* rule 7: default before any early return */
  if (!get_box || !held80 || num_boxes <= 0 || slots_per_box <= 0) return false;   /* rule 7: validate parameters */
  uint32_t max = 0;
  bool collided = false;
  int cb = -1, cs = -1;
  for (int b = 0; b < num_boxes; b++) {                 /* rule 2: bounded by caller-supplied num_boxes (16) */
    const uint8_t* recs = get_box(b, ctx);
    if (!recs) continue;                                 /* unreadable/never-written box: nothing to see here */
    for (int s = 0; s < slots_per_box; s++) {             /* rule 2: bounded by caller-supplied slots_per_box (30) */
      const uint8_t* rec = recs + (uint32_t)s * 80;
      if (bc_is_native(rec)) {                            /* an empty/foreign slot carries no real serial */
        uint32_t serial = (uint32_t)rec[BC_OFF_BANK_SERIAL]
                         | ((uint32_t)rec[BC_OFF_BANK_SERIAL + 1] << 8)
                         | ((uint32_t)rec[BC_OFF_BANK_SERIAL + 2] << 16)
                         | ((uint32_t)rec[BC_OFF_BANK_SERIAL + 3] << 24);
        if (serial > max) max = serial;
      }
      /* the FIRST match only (box order then slot order), same as the old
       * bank_ident32_collision() -- latched once, never overwritten, but the loop
       * itself keeps running so the max-serial answer above still sees every box. */
      if (!collided && !(b == self_box && s == self_slot) && memcmp(rec, held80, 8) == 0) {
        collided = true;
        cb = b; cs = s;
      }
    }
  }
  if (max_out) *max_out = max;
  if (collided) {
    if (coll_box) *coll_box = cb;
    if (coll_slot) *coll_slot = cs;
  }
  return collided;
}

bool bank_ident32_collision(BankBoxGetter get_box, void* ctx,
                             int num_boxes, int slots_per_box,
                             int self_box, int self_slot,
                             const uint8_t ident8[8],
                             int* out_box, int* out_slot) {
  if (!get_box || !ident8 || num_boxes <= 0 || slots_per_box <= 0) return false;   /* rule 7: validate parameters */
  uint8_t held80[80] = {0};
  memcpy(held80, ident8, 8);
  uint32_t max_unused;
  return bank_scan_serial_and_clash(get_box, ctx, num_boxes, slots_per_box,
                                    self_box, self_slot, held80,
                                    &max_unused, out_box, out_slot);
}

uint32_t bank_serial_max(BankBoxGetter get_box, void* ctx, int num_boxes, int slots_per_box) {
  if (!get_box || num_boxes <= 0 || slots_per_box <= 0) return 0;   /* rule 7 */
  uint8_t held80[80] = {0};   /* no self-exclusion, no real cell ever matches an all-zero ident */
  uint32_t max = 0;
  int cb_unused, cs_unused;
  (void)bank_scan_serial_and_clash(get_box, ctx, num_boxes, slots_per_box,
                                   -1, -1, held80, &max, &cb_unused, &cs_unused);
  return max;
}
