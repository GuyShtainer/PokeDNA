#ifndef GEN3_CLIP_H
#define GEN3_CLIP_H

#include <stdint.h>
#include <stdbool.h>

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

/* ---- PC box slots (fixed 14x30 array; an all-zero record == empty) ---- */
uint8_t* pk_box_slot(uint8_t* pc, int box, int slot);   /* pc + 0x0004 + (box*30+slot)*80 */
void clip_write_box_slot(uint8_t* pc, int box, int slot, const uint8_t rec80[80]);
void clip_clear_box_slot(uint8_t* pc, int box, int slot);

/* ---- live party (count-tracked, gap-free — order matters!) ---- */
int      party_count(const uint8_t* sb1, bool frlg);
uint8_t* pk_party_slot(uint8_t* sb1, bool frlg, int idx);
bool     party_append (uint8_t* sb1, bool frlg, const uint8_t rec100[100]); /* false if full (6) */
void     party_release(uint8_t* sb1, bool frlg, int idx);  /* shift the rest down + count-- */
void     party_write  (uint8_t* sb1, bool frlg, int idx, const uint8_t rec100[100]);

#endif /* GEN3_CLIP_H */
