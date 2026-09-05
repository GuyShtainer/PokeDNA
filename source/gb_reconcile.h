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

/* ---- the release plan (S5-C review, 2026-09-05): order, dedupe, re-verify ------
 *
 * DATA-LOSS BUG THIS EXISTS TO FIX. gb_reconcile_match() above refuses an entry
 * that matches 2+ slots -- but nothing stopped TWO DIFFERENT ENTRIES from matching
 * the SAME slot (the same Gen-3 mon transferred to a Gen-1 save AND a Gen-2 save,
 * or re-transferred after an earlier merge-up, produces two unclaimed sidecar
 * entries with the same original80). The old pdna_main.c gb_reconcile_release()
 * blindly released every hit in a fixed order: releasing party slot 1 for the
 * FIRST entry shifted every later party slot down by one, so the SECOND entry's
 * OWN recorded "slot 1" now named a DIFFERENT, innocent mon -- releasing it too
 * deleted a Pokemon that was never transferred anywhere. gb_reconcile_plan() is
 * the fix, pulled into pure C so the ordering/dedupe/re-verify decision has a real
 * test (tests/host_gb_reconcile_test.c) instead of living untested in pdna_main.c. */

#define GB_RECON_MAX_HITS 64

typedef struct {
  int8_t  box;          /* -1 == party, else 0..G3_TOTAL_BOXES-1 (from a
                         * gb_reconcile_match() call) -- gb_reconcile_plan() never
                         * changes this field */
  int8_t  slot;
  uint8_t file_idx;      /* caller's own bookkeeping (which .pds/entry this hit came
                         * from) -- gb_reconcile_plan() does not read or write it   */
  uint8_t entry_idx;
  uint8_t id8[8];        /* personality+otId, re-verified against the LIVE record
                         * immediately before gb_reconcile_plan() touches anything */
  bool    released;      /* OUT: true iff this hit's mon is gone from the save by the
                         * time gb_reconcile_plan() returns -- whether because THIS
                         * hit released it, or because an earlier hit (or its own
                         * duplicate status) already accounts for it. The caller
                         * claims exactly the sidecar entries with released == true */
  bool    duplicate;     /* IN/OUT: true iff an EARLIER (lower-index) hit already
                         * targets this exact (box, slot) -- e.g. the same original
                         * transferred to two different Game Boy generations. A
                         * caller MAY pre-mark this (gb_reconcile_walk() does, for an
                         * early skip); gb_reconcile_plan() independently re-derives
                         * it too and will set a FALSE to true if it finds a match
                         * itself, so leaving it false is always safe. It is
                         * MONOTONIC, NOT a fresh recompute: gb_reconcile_plan()
                         * never clears a duplicate flag the caller already set to
                         * true, even if that pre-mark turns out to be wrong (e.g. a
                         * bug in the caller's own dedupe) -- a caller must only ever
                         * pre-mark this true for a GENUINE duplicate */
  bool    done;          /* OUT: this hit has been fully decided (released, refused
                         * for the party floor, found a live mismatch, or a
                         * duplicate) -- gb_reconcile_plan()'s own "already visited"
                         * marker for its bounded highest-slot-first party loop.
                         * Replaces an earlier design that overloaded `box == -2`
                         * for the same purpose (S5-C review: a dedicated field is
                         * clearer than a sentinel value on a field with its own,
                         * unrelated meaning). Always true when this function
                         * returns; a caller has no reason to read it mid-call. */
} GbReconHit;

/* Release every hit that still resolves in `sb1`/`pc`, then return how many ended
 * up with `released == true`.
 *
 * ORDER. PC hits (box >= 0) are released in array order -- fixed addresses,
 * clip_clear_box_slot() never shifts a sibling. Party hits (box == -1) are
 * released HIGHEST SLOT FIRST -- party_release() shifts every later index down by
 * one, so releasing low-to-high would silently release the WRONG (shifted) mon at
 * a later hit's recorded slot.
 *
 * BOUNDS. A PC hit whose box/slot falls outside 0..G3_TOTAL_BOXES-1 /
 * 0..G3_IN_BOX-1 is refused (unreleased, unclaimed, `done`) BEFORE it is ever used
 * to index `pc` -- a hit is caller-supplied data, ultimately built from bytes read
 * off the SD card, so it is validated like any other untrusted boundary crossing.
 * `pc == NULL` (no PC storage this save) marks every box-hit `done` the same way,
 * for the same reason: nothing else in this function will ever visit it.
 *
 * DEDUPE. Any hit sharing an EXACT (box, slot) with an earlier hit is a duplicate:
 * never touched (the earlier hit already released "the" mon), but `released` is
 * still set true (`duplicate` implies "already released by this batch, whichever
 * entry actually did it" -- the mon really is gone from the save, and marking it
 * so lets the caller claim BOTH sidecar entries instead of orphaning one).
 *
 * RE-VERIFY. Immediately before releasing any NON-duplicate hit, the live record at
 * its recorded slot is compared against `id8` (belt, alongside the dedupe above --
 * catches anything the dedupe pass missed, or a party index a PRIOR release in this
 * same call already invalidated). A PC mismatch leaves that hit unreleased (`done`,
 * not claimed -- something unexpected is there, so this pass does not touch it and
 * does not claim an entry for a release that did not happen). A PARTY mismatch,
 * by contrast, marks the hit released anyway: by construction the only way a
 * party slot's content can change between the walk and this call is an EARLIER
 * hit in this SAME pass releasing it (the ordering above accounts for every other
 * cause), so a mismatch there means "already released, by a different entry."
 *
 * FLOOR. A party release is refused (this hit's `released` stays false, `done`
 * true) whenever `party_count(sb1, frlg) <= 1` at the moment it would run --
 * re-checked LIVE before each one, since an earlier release in this same batch can
 * reach the floor.
 *
 * `sb1`/`pc` are mutated IN PLACE via gen3_clip.h's own party_release() /
 * clip_clear_box_slot() (pure C, no persistence -- the caller commits or discards
 * them through whatever layer it uses). `pc` may be NULL (no PC storage this
 * save); `frlg` selects the party layout, same convention as
 * pk_read_party_auto(). `n` is clamped to GB_RECON_MAX_HITS (golden rule 2). */
int gb_reconcile_plan(GbReconHit* hits, int n, uint8_t* sb1, bool frlg, uint8_t* pc);

#endif /* GB_RECONCILE_H */
