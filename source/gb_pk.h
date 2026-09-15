#ifndef GB_PK_H
#define GB_PK_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"   /* GbEditMon, GB_GEN1/GB_GEN2, GB_NAME_BYTES, gb_rec_size, gb_is_egg */

/* gb_pk — the .pk1/.pk2 file shape (BACKLOG #93, decision 4). Pure C (no tonc, no
 * FatFs) so tests/host_gbpk_test.c dual-compiles on the host, same discipline as every
 * other core in this tree.
 *
 * LAYOUT (re-derived from the save's OWN box geometry, gb_edit.h's "list geometry"
 * comment — count | species list | records | OT names | nicknames — but a .pk1/.pk2
 * file carries only ONE slot's own three pieces, concatenated in that order, box-
 * shaped regardless of whether the source slot is a box or party record):
 *   [0 .. rec_len)                      the BOX-shaped record (gb_rec_size(gen, false):
 *                                        33 bytes Gen 1, 32 bytes Gen 2)
 *   [rec_len .. rec_len+11)              OT name, raw GB bytes, 0x50-terminated
 *   [rec_len+11 .. rec_len+22)           nickname, raw GB bytes, 0x50-terminated
 * Total: 55 bytes (Gen 1), 54 bytes (Gen 2). A party record's extra current-stat bytes
 * (rec_len 44/48) are NOT carried — only the leading box-shaped prefix, the same
 * "a snapshot, not the live battle stats" posture Gen 3's own .pk3 EXPORT already takes
 * for a party mon (pdna_pk.c). PKHeX's PK1/PK2 shapes are REFERENCE ONLY and were not
 * consulted for this file (docs/kb/licensing.md hard rule 6) — this layout comes
 * entirely from gen1_save.h / gen2_save.h / gb_edit.h already in this tree. */

#define GB_PK1_BYTES  55   /* 33 (GEN1_BOX_REC_BYTES) + 11 + 11 */
#define GB_PK2_BYTES  54   /* 32 (G2_BOX_ENTRY)       + 11 + 11 */

/* Packs `e` into `out` (caller-owned, at least `cap` bytes). Returns the number of
 * bytes written (GB_PK1_BYTES or GB_PK2_BYTES) on success, or a negative value if:
 *   -1  `e` or `out` is NULL, or `e->gen` is neither GB_GEN1 nor GB_GEN2
 *   -2  `cap` is too small for this generation's fixed size
 *   -3  `e` is an Egg (decision 5: egg-ness lives in the list byte, outside the
 *       record, so the file format cannot carry it — gb_is_egg(e))
 * Never mutates `e`. The box-shaped prefix is copied from `e->rec` regardless of
 * `e->is_party` — see the file header's note on party truncation. */
int gb_pk_pack(const GbEditMon* e, uint8_t* out, int cap);

/* Reverses gb_pk_pack(): fills `out` (memset to 0 first, matching gb_load's own
 * contract) from `in`/`len`, as a BOX-shaped (is_party = false), non-Egg record.
 * `list_species` is NOT part of the file format (decision 4) — set to the record's own
 * species byte (rec[0], the same byte both generations store their species at) so a
 * caller that only cares about the record/name bytes gets a self-consistent, non-Egg
 * GbEditMon back; a caller that needs the ORIGINAL list byte has no way to recover it
 * from this file alone, by design (decision 5's whole point). Returns false on a NULL
 * pointer, an unsupported `gen`, or a `len` that does not match this generation's
 * fixed size exactly (GB_PK1_BYTES / GB_PK2_BYTES — no partial/truncated files). */
bool gb_pk_unpack(const uint8_t* in, int len, uint8_t gen, GbEditMon* out);

/* ".pk1" / ".pk2" / NULL for an unsupported gen -- gb_export_hook's own filename
 * builder (decision 6) appends this verbatim; never dereferenced past its first 4
 * bytes (".pkN\0"), so a caller may treat the return as a plain C string. */
const char* gb_pk_ext(uint8_t gen);

#endif
