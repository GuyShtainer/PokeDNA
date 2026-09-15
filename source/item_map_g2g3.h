/* GENERATED FILE -- DO NOT EDIT BY HAND.
 * Produced by tools/gen_item_map.py from source/data_tables.c (Gen-3 s_item[])
 * and source/gb_item_names.c (Gen-2 kGen2ItemName[]). Re-run the generator to
 * refresh it; `python3 tools/gen_item_map.py --check` verifies it is current.
 * BACKLOG #150 S150-8-CORE -- see docs/BANK-CROSSGEN-DESIGN.md S10 Q8 / S11.20
 * item 10(a) for the design rationale. */
#ifndef ITEM_MAP_G2G3_H
#define ITEM_MAP_G2G3_H

#include <stdint.h>

#define ITEM_MAP_G2_MAX    0xBEu   /* mirrors GEN2_ITEM_MAX, gb_item_names.c:42 */
#define ITEM_MAP_G3_COUNT  377u    /* mirrors s_item[377], data_tables.c:204    */
#define ITEM_MAP_PAIRS     107u

/* 0 = "no counterpart" (and 0 in -> 0 out: id 0 is "no item" in both generations).
 * Gen 1 has no held items -- never pass a Gen-1 catch-rate byte here
 * (gen12_convert.h:163-165). Out-of-range ids return 0, never read past the table.
 *
 * The fallback contract for a Gen-2 item with NO Gen-3 counterpart (decision 7 /
 * D-Q6, implemented in S150-8, not here):
 *   - A NATIVE-home entry (gb_sidecar.h XR_KIND_NATIVE_HOME, gb_sidecar.h:121) keeps
 *     the item byte INSIDE the cell's own +40 ORIGINAL field (gb_sidecar.h:64) --
 *     that field IS the GBC1 cell for this entry kind. Read it back with
 *     bc_unpack() (bank_cell.h:172) followed by gb_get_held_item() (gb_edit.h:299);
 *     BC_FLAG_HOLDS_ITEM (bank_cell.h:98) is the cheap "does it even hold one" marker.
 *   - A Gen-3-HOME entry keeps its Gen-3 item id inside original80, restored by the
 *     per-field merge (S150-9).
 *   - No byte is added to the 128-byte sidecar entry for this; the loss screen names
 *     the item at draw time from gb2_item_name() (gb_item_names.h:33).
 *
 * Three of the mapped Gen-3 ids are cart-restricted (decision 8): BICYCLE->360,
 * CARD KEY->355 (FRLG+E only, mask 0x06), BASEMENT KEY->271 (RS+E only, mask 0x03).
 * This map is cart-agnostic; the caller consults pk_item_games() (data_tables.h:46)
 * before writing a mapped id into a specific cart's save -- that check is S150-8's,
 * not this table's. */
uint16_t item_g2_to_g3(uint8_t  g2_item);
uint8_t  item_g3_to_g2(uint16_t g3_item);

#endif /* ITEM_MAP_G2G3_H */
