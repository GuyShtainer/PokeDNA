#ifndef GEN3_CLIP_H
#define GEN3_CLIP_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"       /* GbEditMon -- S5-B: the clipboard also carries a Game Boy
                             * source's NATIVE record, not just its lossy Gen-3 copy */

/* One-slot Pokémon clipboard + raw slot operations (pure C, host-testable).
 * The shared foundation for copy/paste/duplicate/release, held-item move, .pk
 * import/export and the external bank. This module ONLY mutates in-RAM save
 * blocks; the caller persists via app_commit_block (the one verified-write path).
 *
 * The per-mon record (80-byte box / 100-byte party) is byte-identical across all
 * five Gen-3 games, so a copied/banked mon injects into ANY game unchanged. */

typedef struct {
  uint8_t rec[100];   /* raw record (only the first 80 bytes matter for a box mon) */
  bool    is_party;   /* true => a 100-byte party source (carries plaintext stats) */
  bool    occupied;
  /* S5-B (docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 10): when a copy came off a Game
   * Boy source THROUGH ITS OWN AppSrcOps.copy_native (pdna_gen12.c's
   * gb_copy_native_hook), `gb` holds the untouched native record and `from_gb` is
   * true, so a later PASTE in a Gen-3 session can look up
   * `/PokeDNA/sidecar/<gbsc_key(...)>.pds` and restore the ORIGINAL Gen-3 record
   * instead of re-degrading `rec` (which clip_copy_from also filled, lossily, from
   * the same slot -- see pdna_main.c app_copy). clip_copy_from() below always sets
   * from_gb = false; only app_copy's own extra step after calling it may set it true. */
  GbEditMon gb;
  bool      from_gb;
} ClipMon;

/* Snapshot a slot's record into the clipboard. */
void clip_copy_from(ClipMon* c, const uint8_t* rec, bool is_party);

/* Produce record bytes targeted at a destination KIND. To box(80): the first 80
 * bytes verbatim (preserves everything). To party(100): from a party source = an
 * exact copy; from a box source = derive the 20 plaintext bytes (level + stats)
 * via the lossless edit core. Returns false if the clipboard is empty. */
bool clip_to_record(const ClipMon* c, bool dst_is_party, uint8_t out[100]);

/* Validate an 80-byte .pk3 box record: re-checksums + species in 1..411, and
 * rejects empty/bad-egg (corrupt) records. */
bool pk3_validate(const uint8_t rec80[80]);

/* Held-item Mail range (ORANGE MAIL..RETRO MAIL, source/data_tables.c) -- moving a party
 * mon holding one of these into an 80-byte box record drops the plaintext mail byte with
 * nothing left pointing at the mail slot it still owns in gSaveBlock1's mail array. Pure
 * predicate on the HELD ITEM id (never the raw 0x55 mail-index byte -- PokeDNA itself
 * wrote 0x00 there before #225 and 0x00 is a valid mail index). tests/host_clip_test.c
 * boundary-checks 0, 120, 121, 132, 133, 0xFFFF so this predicate's own logic -- not just
 * that some call to it exists -- is what the host suite proves (BACKLOG #227 D1). */
#define G3_ITEM_MAIL_LO 121u
#define G3_ITEM_MAIL_HI 132u
bool g3_item_is_mail(uint16_t item);

/* ---- PC box slots (fixed 14x30 array; an all-zero record == empty) ---- */
uint8_t* pk_box_slot(uint8_t* pc, int box, int slot);   /* pc + 0x0004 + (box*30+slot)*80 */
void clip_clear_box_slot(uint8_t* pc, int box, int slot);

/* ---- live party (count-tracked, gap-free — order matters!) ---- */
int      party_count(const uint8_t* sb1, bool frlg);
uint8_t* pk_party_slot(uint8_t* sb1, bool frlg, int idx);
bool     party_append (uint8_t* sb1, bool frlg, const uint8_t rec100[100]); /* false if full (6) */
void     party_release(uint8_t* sb1, bool frlg, int idx);  /* shift the rest down + count-- */

#endif /* GEN3_CLIP_H */
