#ifndef GB_SIDECAR_H
#define GB_SIDECAR_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"     /* GbEditMon, GB_ATK/DEF/SPE/SPC, gb_get_*                 */

/* The sidecar format and the merge UP — pure C, host-testable.
 * docs/GEN3-TO-GB-SIDECAR-DESIGN.md sections 2-4 are the design; this is the format
 * (section 3, with two corrections applied here — see below) and the merge (section 4).
 *
 * NO FILE I/O LIVES HERE. This module reads and writes an in-memory buffer only; the
 * caller (a later UI slice) is the one that owns `/PokeDNA/sidecar/<key16hex>.pds` and
 * `sf_write_verified`/`sf_read_full`. That keeps this file dual-compilable by
 * tests/host_gen3gb_test.c exactly like every other pure-C core in this tree.
 *
 * FORMAT, two corrections from the design doc applied per the S5-A brief:
 *   (a) the entry's "reserved 6 bytes" line is dropped — the entry is 128 bytes with
 *       6 bytes of trailing pad instead, so the byte count for the fields below and
 *       128 agree without a phantom field;
 *   (b) the header is 18 bytes: magic(4) version(1) count(1) flags(2) key(8) crc16(2).
 * All multi-byte fields are little-endian, matching every other pure-C module here.
 *
 * ---- HEADER, GBSC_HEADER (18) bytes ----------------------------------------------
 *   0   4   magic "PDS1"
 *   4   1   version = 1
 *   5   1   count (entries that follow, 0..GBSC_MAX_ENTRIES)
 *   6   2   flags (reserved 0)
 *   8   8   key (FNV-1a-64, gbsc_key() — also the filename, for self-check)
 *   16  2   header crc16 (CRC-16/CCITT-FALSE over bytes 0..15)
 *
 * ---- ENTRY, GBSC_ENTRY (128) bytes, `count` of them follow the header ------------
 *   +0    1   gen (GB_GEN1 / GB_GEN2)
 *   +1    1   claimed (0/1) -- reserved for the "keep after a successful up-transfer"
 *             policy the design doc leaves as a build-time choice; 0 always today
 *   +2    2   species_written (National Dex, as gb_get_species_dex(written) reads it)
 *   +4    2   otid16
 *   +6    4   dv4 (Atk, Def, Spe, Spc, one byte each -- gb_edit.h's own DV order)
 *   +10   11  otname_written (raw GB bytes, exactly as GbEditMon.otname stores them)
 *   +21   11  nick_written   (raw GB bytes)
 *   +32   4   exp_written
 *   +36   4   rtc_epoch of the transfer (0 if the RTC was absent)
 *   +40   80  ORIGINAL Gen-3 record (the box-shaped 80-byte core)
 *   +120  6   pad (0)
 *   +126  2   entry crc16 (CRC-16/CCITT-FALSE over bytes +0..+125)
 *
 * ---- THE FINGERPRINT ---------------------------------------------------------
 * key = FNV-1a-64 over, IN ORDER: gen (1 byte), otid16 (2 bytes LE), dv4 (4 bytes,
 * Atk/Def/Spe/Spc), otname (11 raw GB bytes) -- 18 bytes total, offset basis
 * 14695981039346656037, prime 1099511628211 (the standard 64-bit FNV-1a constants).
 * Species is DELIBERATELY EXCLUDED (design doc section 2's recommendation) so a
 * Pokemon that evolves on the Game Boy still finds its sidecar; gbsc_find() compares
 * every OTHER immutable field and never touches species at all. */

#define GBSC_MAX_ENTRIES 8
#define GBSC_HEADER       18
#define GBSC_ENTRY        128
#define GBSC_FILE_MAX     (GBSC_HEADER + GBSC_MAX_ENTRIES * GBSC_ENTRY)   /* 1042 */

/* ---- the fingerprint -------------------------------------------------------- */

uint64_t gbsc_key(uint8_t gen, uint16_t otid16, const uint8_t dv4[4],
                  const uint8_t otname[GB_NAME_BYTES]);
/* 16 upper-case hex digits + NUL -- the file's own name. */
void gbsc_key_hex(uint64_t key, char out[17]);

/* One place for the on-card path both S5-B callers need (pdna_main.c's PASTE-merge and
 * pdna_gen12.c's PASTE(GB)): "<dir>/<16 hex><.pds>", e.g.
 * "/PokeDNA/sidecar/0019A3F17C0B44E2.pds". `dir` carries NO trailing slash. Still no
 * file I/O -- this only builds a string. Returns the length written (excluding the
 * NUL), or -1 if it would not fit in `cap` bytes (out is then left untouched) or `dir`/
 * `out` is NULL. A caller sizes `out` at >= 48: the longest real `dir`
 * (PDNA_DIR "/sidecar", 17 bytes) plus '/' plus 16 hex plus ".pds" plus NUL is 39. */
int gbsc_path(char* out, int cap, const char* dir, uint64_t key);

/* ---- one entry, decoded ------------------------------------------------------ */

typedef struct {
  uint8_t  gen;
  uint8_t  claimed;
  uint16_t species_written;
  uint16_t otid16;
  uint8_t  dv4[4];                       /* Atk, Def, Spe, Spc */
  uint8_t  otname_written[GB_NAME_BYTES];
  uint8_t  nick_written[GB_NAME_BYTES];
  uint32_t exp_written;
  /* Not a UNIX epoch -- there is no RTC-epoch conversion anywhere in this tree and this
   * module will not invent one (same posture as GbGen1Base). PACKING (S5-B, the only
   * writer is pdna_gen12.c's gb_paste_hook, from a GbaRtcTime -- gba_rtc.h):
   *   bits 31..26  year - 2000  (0..63, i.e. 2000..2063)
   *   bits 25..22  month        (1..12)
   *   bits 21..17  day          (1..31)
   *   bits 16..12  hour         (0..23)
   *   bits 11..6   minute       (0..59)
   *   bits  5..0   second       (0..59)
   * 0 when gba_rtc_get() fails (no RTC exposed) -- also a real, if vanishingly
   * unlikely, encoding (year 2000, month 0, ...), so 0 is "absent" by CONVENTION, not
   * because it cannot otherwise occur; nothing in this module or S5-B decodes this
   * field back into a GbaRtcTime today, so the ambiguity has no consequence yet. */
  uint32_t rtc_epoch;
  uint8_t  original80[80];
} GbscEntry;

/* Fills every field of `e` from the record `gen3_to_gb` just built (`written`) and the
 * Gen-3 bytes it was built from (`original80`, the ORIGINAL 80-byte record, pre-loss).
 * `claimed` starts at 0. */
void gbsc_entry_from(GbscEntry* e, const GbEditMon* written, const uint8_t* original80,
                     uint32_t rtc_epoch);

/* ---- the file, as one in-memory buffer --------------------------------------- */

/* Write an empty, valid file (header only, count 0) into `buf` (>= GBSC_HEADER
 * bytes). Returns the length written (always GBSC_HEADER), or -1 if `buf` is NULL. */
int gbsc_init(uint8_t* buf, uint64_t key);

/* Validate the whole buffer -- magic, version, the header crc16, `len` matching
 * GBSC_HEADER + count*GBSC_ENTRY exactly, count <= GBSC_MAX_ENTRIES, AND every one of
 * those `count` entries' own crc16 (so a single corrupted byte anywhere in the file,
 * header or any entry, is caught here rather than only at the entry it happens to
 * land in). Returns the entry count on success, -1 otherwise. */
int gbsc_count(const uint8_t* buf, uint32_t len);

/* Append `e` as a new entry. `*len` is updated on success. Fails (-1, `buf`/`*len`
 * unchanged) if the file does not already validate, is already at GBSC_MAX_ENTRIES,
 * or the grown file would not fit in `cap` bytes. Returns the new entry's index. */
int gbsc_add(uint8_t* buf, uint32_t* len, uint32_t cap, const GbscEntry* e);

/* Decode entry `idx` (0 .. gbsc_count()-1) into `out`. False on a bad index or a file
 * that does not validate. */
bool gbsc_get(const uint8_t* buf, uint32_t len, int idx, GbscEntry* out);

/* First entry at index >= `start` whose gen, otid16, dv4 and otname_written bytes ALL
 * match `now`'s (species is deliberately NOT compared -- see the fingerprint note
 * above). -1 if none, or if the file does not validate. */
int gbsc_find(const uint8_t* buf, uint32_t len, const GbEditMon* now, int start);

/* Remove entry `idx`, compacting the entries after it down by one slot and rewriting
 * the header (new count, new header crc16). The surviving entries' own bytes --
 * pad and crc16 included -- are relocated verbatim, so their individual crc16 stays
 * valid without being recomputed. Returns 0 on success, -1 on a bad index or a file
 * that does not validate; `buf`/`*len` are unchanged on failure. */
int gbsc_remove(uint8_t* buf, uint32_t* len, int idx);

/* ---- the merge UP (docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 4) -------------- */

/* What gbsc_merge_up actually changed, for the confirm screen. All-false is the
 * common case: the Game Boy record is exactly what gen3_to_gb produced, and the merge
 * reproduces the original 80 bytes byte for byte. */
typedef struct {
  bool evolved;          /* species differs from species_written                     */
  bool level_changed;     /* EXP differs from exp_written                             */
  bool moves_changed;     /* any of the 4 moves or their PP Ups differ                */
  bool renamed;           /* nickname bytes differ and the new text was applied       */
  bool rename_refused;    /* nickname bytes differ but decoded to something the
                            * Gen-3 (ASCII-only) charset cannot spell -- a "{XX}"
                            * escape OR a real non-ASCII glyph such as the gender
                            * signs -- so the SIDECAR's name is kept instead         */
  bool gb_item_ignored;   /* Gen 2 holds a non-zero item -- no Gen-2->Gen-3 item map
                            * exists in this tree, so the sidecar's Gen-3 item is kept */
} GbscMergeReport;

/* NOTE ON DVs, DELIBERATELY ABSENT FROM GbscMergeReport (design decision,
 * 2026-09-04): dv4 is part of the sidecar's own fingerprint (gbsc_key/gbsc_find), so
 * editing a DV on the Game Boy record changes what gbsc_find() looks for -- the
 * fingerprint TWO Pokemon of the same trainer with the same name would otherwise
 * share is exactly what dv4 exists to break, so it cannot be dropped from the key
 * without reintroducing that collision. The consequence is real and intentional: A
 * DV EDIT ON THE GAME BOY ORPHANS THAT POKEMON'S SIDECAR. gbsc_find() will no longer
 * locate it (by any OTHER field matching), and gbsc_merge_up() below does not try to
 * reconcile a DV mismatch it is handed directly -- it keeps the sidecar's original,
 * unedited IVs, because in the supported find-then-merge flow the mismatch cannot
 * arise at all. The UI slice that edits Game Boy DVs (S5-B) must warn before letting
 * a DV edit happen on a mon that still has a sidecar entry. */

/* Rebuild the Gen-3 record: start from `e->original80` (so PID, nature, ability,
 * shininess, gender, met data, ball, ribbons, contest, markings, secret ID, EVs and
 * IVs all come back exactly), then fold in what changed on the Game Boy between the
 * conversion and now -- species (evolution), level (gated on the level itself, not
 * EXP: see gb_sidecar.c's merge_species_and_level for why gating on EXP silently
 * erased within-level progress), moves/PP-Ups, and a charset-safe nickname. `rep`
 * may be NULL. False only for a NULL/mismatched-generation argument, in which case
 * `out80` is untouched; otherwise the merge always succeeds and `out80` holds the
 * merged 80-byte record. */
bool gbsc_merge_up(const GbscEntry* e, const GbEditMon* now, uint8_t out80[80],
                   GbscMergeReport* rep);

#endif /* GB_SIDECAR_H */
