#ifndef ITEM_MAP_G1G2_H
#define ITEM_MAP_G1G2_H
#include <stdint.h>

/* BACKLOG #249 cases B/C/D: Gen 1 has no held-item byte, but it DOES have a bag, so a
 * Gen-2-equivalent item (item_g3_to_g2()'s own output) can still travel there --
 * source/gen3_to_gb.h's g3gb_item_ladder() is the caller.
 *
 * There is no Gen-3 -> Gen-1 item map in this tree, and Gen-1 item ids are NOT Gen-2
 * item ids (they are two independent, hand-typed id spaces -- gb_item_names.c). This
 * file never hand-copies a table from a decomp: item_g2_to_g1() derives its answer by
 * NAME EQUALITY between the two name tables this repo already ships (gb1_item_name()/
 * gb2_item_name(), gb_item_names.h), at call time, so the mapping can never drift from
 * those names the way a checked-in table could.
 *
 * Rules, in order:
 *   - id 0, or an id gb2_item_name() has no entry for (0x00 hole / out of table /
 *     "unused" placeholder -- gb2_item_name returns NULL) -> 0.
 *   - a Gen-1 candidate that gbb_is_g1_key_item() calls a key item is NEVER a match --
 *     a key item is never a valid bag-insert target for this ladder.
 *   - AMBIGUITY refuses rather than guesses, in EITHER direction: if this Gen-2 item's
 *     name matches more than one Gen-1 id, or if the one Gen-1 id it does match is
 *     itself the name of more than one Gen-2 id, the answer is 0. */
uint8_t item_g2_to_g1(uint8_t g2_item);

#endif /* ITEM_MAP_G1G2_H */
