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

/* BACKLOG #223: the same shared getter, one more pure walk -- the HIGHEST
 * bank_serial (BC_OFF_BANK_SERIAL, bank_cell.h) stored in any native cell across
 * every box. After a bank.meta .bak rollback (BACKLOG #219) the recovered serial
 * counter can sit below serials already written into the boxes; every UP landing
 * then computes a serial the collision scan above immediately refuses, burning one
 * serial per retry forever. Callers resync by raising the RAM counter to
 * bank_serial_max(...) + 1 before allocating the next serial. Returns 0 when no box
 * holds a native cell (a fresh Bank, or every get_box(b, ctx) returned NULL) --
 * 0 is never a valid allocated bank_serial (pdna_bank_next_serial() never returns
 * it), so "0 = nothing stored" cannot be confused with a genuine high-water mark. */
uint32_t bank_serial_max(BankBoxGetter get_box, void* ctx, int num_boxes, int slots_per_box);

/* BACKLOG #206 fixes2 R2: bank_ident32_collision() and bank_serial_max() used to be
 * two independent full 16-box walks through the SAME getter (drop_held_up called
 * both, back to back, every session's first UP landing) -- one pass answers both
 * questions instead. Compares held80's own first 8 bytes (magic+ident32) against
 * every OTHER cell's, same (self_box, self_slot) exclusion and "first match in box
 * order then slot order" semantics as bank_ident32_collision(); tracks the highest
 * bc_is_native() bank_serial across every cell scanned, same semantics as
 * bank_serial_max() (0 = no native cell anywhere). *max_out is always written (0 on
 * an empty/all-foreign bank or on invalid parameters); coll_box/coll_slot are
 * written ONLY when the return value is true, same "untouched on no collision"
 * contract as bank_ident32_collision(). The scan never stops early on the first
 * collision (unlike the old bank_ident32_collision(), which could stop scanning
 * further boxes) because the max-serial answer needs every box regardless -- the
 * reported coll_box/coll_slot are still the FIRST match, latched once and never
 * overwritten by a later one.
 *
 * bank_ident32_collision()/bank_serial_max() above are now thin wrappers over this
 * (kept because tests/host_bank_collision_test.c's 42 cases drive their own
 * narrower signatures directly -- an 8-byte ident and a bare max-only query,
 * neither of which decomposes cleanly from a single held80[80]+two-out-param call
 * without a full test rewrite). */
bool bank_scan_serial_and_clash(BankBoxGetter get_box, void* ctx,
                                int num_boxes, int slots_per_box,
                                int self_box, int self_slot,
                                const uint8_t held80[80],
                                uint32_t* max_out, int* coll_box, int* coll_slot);

#endif /* BANK_COLLISION_H */
