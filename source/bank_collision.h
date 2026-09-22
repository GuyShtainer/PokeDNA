#ifndef BANK_COLLISION_H
#define BANK_COLLISION_H

#include <stdint.h>
#include <stdbool.h>

/* BACKLOG #168a: pure ident32-collision scan across every Bank box. No tonc/FatFs
 * dependency (only <stdint.h>/<stdbool.h>/<string.h>) so this host-compiles and is
 * exercised directly by tests/host_bank_collision_test.c, on a synthesized in-memory
 * Bank -- the real box files, pdna_bank.c, and the GBA build are never involved in
 * that test.
 *
 * `get_box(b, ctx)` must return box `b`'s raw records (at least
 * slots_per_box * 80 bytes), or NULL if box `b` cannot be read/paged. NULL is treated
 * as "nothing there to collide with", never as an error: an unreadable or
 * never-written box holds no on-card data at all, so it cannot be hiding a duplicate
 * ident32. The GBA build's getter pages real boxes through pdna_bank_peek_box()
 * (pdna_bank.c) -- ONE 2,400-B box buffer at a time, the same shared buffer every
 * other Bank reader uses, never a second one on the stack.
 *
 * Compares only the first 8 bytes (bank_cell.h's magic + ident32) of each 80-byte
 * record, skipping (self_box, self_slot) -- the destination cell this drop is about
 * to overwrite, which is never a "collision" with itself. Returns true and fills
 * out_box/out_slot on the FIRST match found (box order 0..num_boxes-1, then slot
 * order within a box); returns false, leaving out_box/out_slot untouched,
 * otherwise. */
typedef const uint8_t* (*BankBoxGetter)(int box, void* ctx);

bool bank_ident32_collision(BankBoxGetter get_box, void* ctx,
                             int num_boxes, int slots_per_box,
                             int self_box, int self_slot,
                             const uint8_t ident8[8],
                             int* out_box, int* out_slot);

#endif /* BANK_COLLISION_H */
