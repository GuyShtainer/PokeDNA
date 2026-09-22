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
 *   6   2   flags -- bit 0 GBSC_FLAG_KEEP_ASKED (S5-C Part B2: "the reconcile screen
 *           already asked about every entry in this file and the user chose KEEP,
 *           so gb_reconcile_on_load() must not ask again"), bits 1..15 reserved 0
 *   8   8   key (FNV-1a-64, gbsc_key() — also the filename, for self-check)
 *   16  2   header crc16 (CRC-16/CCITT-FALSE over bytes 0..15)
 *
 * ---- ENTRY, GBSC_ENTRY (128) bytes, `count` of them follow the header ------------
 *   +0    1   gen (GB_GEN1 / GB_GEN2)
 *   +1    1   FLAGS byte (BACKLOG #150 S150-6, promoted from the old single-purpose
 *             "claimed" byte -- every entry ever written held only 0 or 1 here, so
 *             this is backward-compatible by construction):
 *               b0     claimed (0/1) -- S5-C Part B2: set by gb_reconcile_on_load()
 *                      once the Gen-3 original has been released from this save
 *                      (design doc section 12); a claimed entry is never offered
 *                      again by that screen but still serves gbsc_merge_up() on the
 *                      way UP (gbsc_find(..., include_claimed)) -- the sidecar's
 *                      whole job (restoring what a GB edit can't hold) does not stop
 *                      just because the original was released
 *               b1-b2  state: 0 legacy/none (XR_STATE_NONE), 1 XR_STATE_PENDING,
 *                      2 XR_STATE_CLAIMED
 *               b3     kind: 0 XR_KIND_G3_HOME, 1 XR_KIND_NATIVE_HOME
 *               b4     direction (G-M8): 0 == residence abroad is a GB save
 *                      (XR_DIR_ABROAD_GB), 1 == residence abroad is a Gen-3 save
 *                      (XR_DIR_ABROAD_G3)
 *               b5     has-written-moves (this entry's +121..+125 block is valid)
 *               b6-b7  reserved 0
 *             A pre-#150 entry (flags byte literally 0 or 1) therefore decodes as
 *             kind=G3_HOME / direction=GB / state=NONE / has_written_moves=0 --
 *             exactly today's defaults, no version bump, no format change.
 *   +2    2   species_written (National Dex, as gb_get_species_dex(written) reads it)
 *   +4    2   otid16
 *   +6    4   dv4 (Atk, Def, Spe, Spc, one byte each -- gb_edit.h's own DV order)
 *   +10   11  otname_written (raw GB bytes, exactly as GbEditMon.otname stores them)
 *   +21   11  nick_written   (BACKLOG #150 S150-8 F3: the nickname bytes IN THE
 *             ABROAD FORMAT -- Gen-3 bytes (the converted record's own raw 10-byte
 *             nickname field, its own encoding) for a NATIVE_HOME entry whose
 *             direction is XR_DIR_ABROAD_G3; raw GB bytes, exactly as
 *             GbEditMon.nick stores them, for XR_DIR_ABROAD_GB and every
 *             pre-#150-S150-8 (XR_KIND_G3_HOME) entry. gbsc_entry_from() fills GB
 *             bytes by default from `written`; a NATIVE_HOME/ABROAD_G3 caller
 *             (source/pdna_gen12.c's xfer_down_write) overrides them with the
 *             Gen-3 record's own bytes afterward, because `written` there is the
 *             cell's own unpacked GB record, not what actually landed abroad)
 *   +32   4   exp_written
 *   +36   4   rtc_epoch of the transfer (0 if the RTC was absent)
 *   +40   80  ORIGINAL Gen-3 record (the box-shaped 80-byte core)
 *   +120  1   written_level (BACKLOG #104 R1) -- the level ACTUALLY WRITTEN to the
 *             Game Boy record at transfer time (gb_get_level() of `written`, the
 *             same object species_written/exp_written already come from). 0 is a
 *             SENTINEL, not a real level (no Game Boy Pokemon is ever level 0):
 *             it means "this entry predates R1" -- every entry gbsc_add() wrote
 *             before this field existed has this byte at its old pad value, 0,
 *             because gbsc_add() memset()s the whole entry to 0 before filling it.
 *             gbsc_merge_up()'s merge_species_and_level() reads 0 as "fall back to
 *             comparing against the ORIGINAL's own decoded level" (today's exact,
 *             pre-R1 behaviour), and a nonzero value as "compare against THIS
 *             instead" -- the fix that lets a MAKE-LEGAL level correction (which
 *             changes what is written without changing what the ORIGINAL was) be
 *             told apart from a genuine in-game level-up. See gb_sidecar.c's
 *             merge_species_and_level() for the comparison itself.
 *   +121  4   moves_written[4] -- one byte per move slot, the MOVE ID actually
 *             written to the Game Boy record at transfer time (the same id space
 *             gb_get_move() reads), valid only when the flags byte's b5
 *             (has-written-moves) is set. This is the baseline merge_moves()
 *             compares the CURRENT Game Boy moves against, so a MAKE-LEGAL move
 *             swap applied before the write does not read back as a genuine
 *             in-game move change (G-H9).
 *   +125  1   ppup_written -- four 2-bit fields (one per move slot, LSB-first),
 *             valid only when b5 is set: the PP-Ups actually written per slot.
 *   +121..+125 together replace the old 5-byte pad; both fields are 0 (and b5 is
 *             clear) on any pre-#150 entry, which is exactly the old pad value.
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
/* The on-card path buffer every gbsc_path() caller needs (S5-B review fix #10): the
 * longest real `dir` (PDNA_SIDECAR_DIR = "/PokeDNA/sidecar", 17 bytes) plus '/' plus
 * 16 hex plus ".pds" plus NUL is 39 -- one shared constant so every `char path[...]`
 * in this tree agrees, instead of three separate `char path[48]` locals. */
#define GBSC_PATH_MAX     48

/* Header flags byte, bit 0 (S5-C Part B2 -- see the header layout comment above). */
#define GBSC_FLAG_KEEP_ASKED  0x0001u
/* Header flags byte, bit 1 (BACKLOG #150 S150-6, decision D-Q2): file-level MIRROR of
 * "at least one entry in this file has its own has-written-moves bit set". The
 * per-entry bit (below) is what merge_moves() actually branches on -- this header bit
 * exists only so a future scan does not have to open every entry to know whether the
 * file is worth a closer look. */
#define GBSC_FLAG_HAS_WRITTEN_MOVES  0x0002u

/* Entry flags byte (+1), decoded fields -- see the entry layout comment above. */
#define XR_KIND_G3_HOME       0
#define XR_KIND_NATIVE_HOME   1

#define XR_STATE_NONE         0
#define XR_STATE_PENDING      1
#define XR_STATE_CLAIMED      2
#define XR_STATE_RESTORED     3   /* BACKLOG #150 S150-8b: the native home is back in the Bank; the abroad copy is a duplicate */

#define XR_DIR_ABROAD_GB       0
#define XR_DIR_ABROAD_G3       1

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
 * `out` is NULL. A caller sizes `out` at >= GBSC_PATH_MAX. */
int gbsc_path(char* out, int cap, const char* dir, uint64_t key);

/* ---- one entry, decoded ------------------------------------------------------ */

typedef struct {
  uint8_t  gen;
  uint8_t  claimed;
  uint16_t species_written;
  uint16_t otid16;
  uint8_t  dv4[4];                       /* Atk, Def, Spe, Spc */
  uint8_t  otname_written[GB_NAME_BYTES];
  uint8_t  nick_written[GB_NAME_BYTES];   /* see the entry layout comment above (+21):
                                           * ABROAD format, not always GB bytes */
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
  uint8_t  written_level;                /* BACKLOG #104 R1 -- 0 == pre-R1 entry, see
                                           * the entry layout comment above. Placed
                                           * LAST (not next to exp_written) so it adds
                                           * no internal struct padding a whole-struct
                                           * memcmp (host_gen3gb_test.c's own sidecar-
                                           * roundtrip check) would trip on. */
  /* BACKLOG #150 S150-6 (decision D-Q1) -- placed after written_level, same reasoning:
   * appending keeps every field above at its old offset inside the struct, so a
   * caller that already only assigns named fields (never memcpy's the whole struct
   * in) is unaffected. */
  uint8_t  moves_written[4];             /* valid iff has_written_moves */
  uint8_t  ppup_written;                 /* four 2-bit fields, valid iff has_written_moves */
  uint8_t  kind;                         /* XR_KIND_* */
  uint8_t  state;                        /* XR_STATE_* */
  uint8_t  direction;                    /* XR_DIR_ABROAD_* */
  uint8_t  has_written_moves;            /* 0/1 -- entry flags byte b5 */
} GbscEntry;

/* Fills every field of `e` from the record `gen3_to_gb` just built (`written`) and the
 * Gen-3 bytes it was built from (`original80`, the ORIGINAL 80-byte record, pre-loss).
 * `claimed` starts at 0; `kind`/`state`/`direction` start at XR_KIND_G3_HOME/
 * XR_STATE_NONE/XR_DIR_ABROAD_GB -- a caller that wants something else sets it on
 * the returned `*e` before gbsc_add(). `moves_written`/`ppup_written` are ALWAYS
 * filled from `written`'s own current moves/PP-ups (the same object
 * written_level/species_written/exp_written already come from) and
 * `has_written_moves` is ALWAYS set to 1 (BACKLOG #150 S150-6 review F1, G-H9 for
 * real): the written-moves baseline exists precisely so a MAKE-LEGAL move
 * correction applied before this call is captured, not re-derived later from
 * `original80` (which merge_moves() only still falls back to for a PRE-#150 entry,
 * one this function never produces). */
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
 * above). -1 if none, or if the file does not validate.
 *
 * `include_claimed` (S5-C Part B2, new parameter): false SKIPS entries whose
 * `claimed` byte is set -- gb_reconcile_on_load()'s own walk never re-offers a mon it
 * already released. true also considers claimed entries -- the merge UP
 * (app_paste_gb_lookup, pdna_main.c) must still find one: a released original is
 * exactly why the sidecar exists, not a reason to stop serving it. Every call site
 * that predates this parameter (the up-merge, and every existing test) wants true --
 * nothing could BE claimed before this slice.
 *
 * `want_kind` (BACKLOG #150 S150-6, decision D-Q5/review 8b): XR_KIND_* to require,
 * or < 0 for "any kind" (every call site that predates this parameter). The filter
 * may only REMOVE non-matching candidates from the walk -- it never changes which
 * entry wins among the survivors (app_paste_gb_lookup's own tiebreak order, S150-9's
 * job to rework, must survive byte-for-byte). A caller resolving the merge UP wants
 * XR_KIND_G3_HOME (a native cell, kind XR_KIND_NATIVE_HOME, must never reach
 * gbsc_merge_up -- see gbsc_merge_up's own native refusal below and G-H6). */
int gbsc_find(const uint8_t* buf, uint32_t len, const GbEditMon* now, int start,
             bool include_claimed, int want_kind);

/* First entry (any index) whose original80[0..7] equals `id8` -- DOWN's replace-not-
 * append lookup (S150-6, §11.6). -1 if none, or if the file does not validate. */
int gbsc_find_by_key(const uint8_t* buf, uint32_t len, const uint8_t id8[8]);

/* When the file is already at GBSC_MAX_ENTRIES and entry 0 (the OLDEST -- gbsc_add
 * always appends, gbsc_remove always compacts, so index 0 is always the oldest
 * surviving entry) is not itself XR_STATE_PENDING, evict it (gbsc_remove(buf, len, 0))
 * and return 0. Otherwise -1, refusing nothing else: a file whose oldest entry IS
 * pending is the one case §11.6 reserves the refusal for. */
int gbsc_evict_oldest(uint8_t* buf, uint32_t* len);

/* Set/clear entry `idx`'s `claimed` byte and rewrite its crc16 (the entry's own crc
 * covers bytes +0..+125, `claimed` included -- see the entry layout above). 0 on
 * success, -1 on a bad index or a file that does not validate; `buf` unchanged on
 * failure. Does NOT touch the header (count/flags/header crc16 are unaffected by a
 * claim -- only gbsc_add/gbsc_remove change the entry count). */
int gbsc_set_claimed(uint8_t* buf, uint32_t len, int idx, bool claimed);

/* Header `flags` (bit 0 GBSC_FLAG_KEEP_ASKED, see the header layout above). Rewrites
 * the header crc16 on set. gbsc_flags_get returns 0 for a file that does not
 * validate (the same "absent == no flags" convention gbsc_count's callers already
 * rely on); gbsc_flags_set returns 0 on success, -1 on a bad/NULL buffer. */
uint16_t gbsc_flags_get(const uint8_t* buf, uint32_t len);
int      gbsc_flags_set(uint8_t* buf, uint32_t len, uint16_t flags);

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
  bool level_changed;     /* level differs from written_level (or, for a pre-R1 entry
                            * with written_level == 0, from the ORIGINAL's own level) */
  bool moves_changed;     /* any of the 4 moves or their PP Ups differ                */
  bool renamed;           /* nickname bytes differ and the new text was applied       */
  bool rename_refused;    /* nickname bytes differ but decoded to something the
                            * Gen-3 (ASCII-only) charset cannot spell -- a "{XX}"
                            * escape OR a real non-ASCII glyph such as the gender
                            * signs -- so the SIDECAR's name is kept instead         */
  bool gb_item_ignored;   /* Gen 2 holds a non-zero item and the Gen-3 side keeps the
                            * sidecar's own item. The Gen-2<->Gen-3 name-keyed map now
                            * exists (source/item_map_g2g3.h, BACKLOG #150 S150-8-CORE)
                            * but has no callers yet -- S150-8 is the slice that wires
                            * it into this merge and into the DOWN edge.              */
  uint8_t level_from, level_to;  /* S150-9 decision 5 -- baseline / abroad level, 0/0
                                  * when there is no level row                        */
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

/* BACKLOG #150 S150-9 decision 1: `accept` gates which fields the walk APPLIES to
 * `out80` (the XR_ACCEPT_* bits, source/xfer_rec.h -- not included here, it would be
 * a circular include: xfer_rec.h already includes this header for GbscEntry. Pass the
 * literal bits or xfer_rec.h's macros from a caller that already includes both).
 * `rep` is filled identically no matter what `accept` is (mask-independent); only the
 * apply is gated. `gbsc_merge_up` is the ACCEPT_ALL (0x0F) wrapper above. */
bool gbsc_merge_up_sel(const GbscEntry* e, const GbEditMon* now, uint8_t accept,
                       uint8_t out80[80], GbscMergeReport* rep);

#endif /* GB_SIDECAR_H */
