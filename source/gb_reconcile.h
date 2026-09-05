#ifndef GB_RECONCILE_H
#define GB_RECONCILE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * S5-C Part B2: "the move, as reconcile-on-load" (docs/GEN3-TO-GB-SIDECAR-DESIGN.md
 * section 12). No session ever holds both a Gen-3 save and a Game Boy save at once, so
 * a PASTE (GB) transfer down cannot remove the Gen-3 original THEN AND THERE -- the
 * removal happens the NEXT time the Gen-3 save loads, once pdna_main.c's
 * gb_reconcile_on_load() has read the sidecar files and found which Pokemon in THIS
 * save were the ones transferred. Finding them is the one piece of that job worth
 * pulling out of pdna_main.c and testing on the real corpus: does a sidecar entry's
 * 80-byte original80 identify EXACTLY ONE mon in this save, and if a card error or a
 * cloned Gen-3 mon makes that ambiguous, does this module refuse to guess.
 *
 * PURE C: no tonc, no FatFs, no GBA headers, no statics -- gen3_clip.h's own
 * pk_party_slot()/pk_box_slot() are pure C too, so this dual-compiles exactly like
 * every other pure-C core in this tree (tests/host_gb_reconcile_test.c runs it on the
 * PC against Guy's real .sav corpus).
 *
 * THE IDENTITY. A Gen-3 record's first 8 bytes are personality (4, plaintext) then
 * OT id (4, plaintext) -- never touched by the substruct encryption -- and an EMPTY
 * slot's canonical representation is all-zero bytes (gen3_clip.h: "PC box slots (fixed
 * 14x30 array; an all-zero record == empty)"). So `id8` doubles as both the
 * fingerprint AND the occupancy test: an all-zero `id8` can never legitimately belong
 * to a real transferred mon (gbsc_entry_from() only ever captures a REAL record's own
 * first 8 bytes), so it is refused outright rather than being allowed to "find" every
 * empty slot in the save.
 */

/* Count how many party/PC slots in this save hold a record whose first 8 bytes
 * (personality + OT id, both plaintext) equal `id8`.
 *
 * `sb1` is the reassembled SaveBlock1 (pdna_main.c's g_sb1); `frlg` selects the party
 * layout pk_read_party_auto() would (gen3_mon.h). `pc` is the reassembled PC storage
 * (gen3_box.h's G3_PC_BYTES layout, pdna_main.c's g_pc) or NULL when this save has no
 * PC (g_have_pc false) -- a party-only save is walked over just the party.
 *
 * On exactly one match, `*where_box`/`*where_slot` (either may be NULL) are filled:
 * a PARTY match sets `*where_box = -1` and `*where_slot` to the party index (0..5); a
 * PC match sets `*where_box` to the box (0..G3_TOTAL_BOXES-1) and `*where_slot` to the
 * slot (0..G3_IN_BOX-1). On 0 or 2+ matches they are left at -1/-1 -- 2+ is a cloned
 * Gen-3 mon (two records that happen to share PID+otId), and this module refuses to
 * guess which one the sidecar entry actually belongs to; the caller's job is to skip
 * that entry, not to pick one.
 *
 * Returns the match count (0, 1, or more). An all-zero `id8`, or a NULL `sb1`/`id8`,
 * always returns 0 without looking at either buffer. */
int gb_reconcile_match(const uint8_t* sb1, bool frlg, const uint8_t* pc,
                       const uint8_t id8[8], int* where_box, int* where_slot);

#endif /* GB_RECONCILE_H */
