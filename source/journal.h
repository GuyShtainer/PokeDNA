/* SPDX-License-Identifier: GPL-3.0-or-later */
/* journal.h -- the #234 undo journal, PURE C (slice 1 of docs/briefs/234-UNDO-DESIGN.md).
 *
 * WHAT THIS IS. The on-disk format and the in-RAM engine of the persistent edit history:
 * segment files, records, the append cursor, longest-valid-prefix open, keys + redirects,
 * the hash anchor, crossed floors, and the two-pass undo/redo primitives. NOTHING in the
 * app calls it yet (slice 2 wires the funnel to it, slice 3 is the UI).
 *
 * PURITY (repo rule 5). <stdint.h>/<string.h> only. No tonc, no FatFs, no sys.h: every
 * file operation goes through the JrnFs table below (source/journal_fs.c binds it to the
 * real lib/fatfs) and the image is only ever touched through JrnImage -- so
 * tests/host_journal_test.c and tests/host_journal_cut_test.c dual-compile it on the Mac
 * and can inject a power cut between any two sector writes.
 *
 * THE POWER-CUT DISCIPLINE (D3, v2-verify fixes 6+7) -- every rule below exists so that
 * a cut at ANY sector leaves the journal reading as exactly its before-state or its
 * after-state and never touches a neighbouring file:
 *   - SECTOR 0 IS THE HEADER'S ALONE (records begin at JRN_REC_BASE = 512): a flush never touches it
 *     after activation, so the flush-tears-the-header class does not exist (costs 0.7% of a segment).
 *   - Segments are PRE-ZEROED fixed-size files. Appends overwrite in place and never
 *     grow the FAT chain; the file is never truncated (torn tails are zeroed in place).
 *   - THE RING. The segment set is a FIXED RING of slot files (max_segs + 1) created once at the
 *     FIRST fill (create-or-complete, zero-filled, header-less = free). After that the directory
 *     NEVER changes: activating a segment zero-fills its slot in place and writes the header LAST
 *     (the header carries the LOGICAL index and the ring size); compaction retires the oldest by
 *     ZEROING ITS HEADER in place. A slot is a segment only when its header crc holds, so a torn
 *     retire / activation reads as the before- or the after-state, never garbage. No create, delete
 *     or rename after the first fill (an exFAT directory-entry tear hides every live segment).
 *   - An append flush writes its records BACK TO FRONT: a record becomes visible only
 *     when everything before it is valid (seq continuity + CRC), so the whole batch
 *     commits when its FIRST record's last sector lands -- one atomic point.
 *   - While a flush writes into a segment the FILE'S OWN timestamp is held (fs->stamp
 *     read first, jrn_fattime_filter answers get_fattime with it): f_sync then rewrites
 *     the directory entry byte-for-byte, so there is no directory-entry tear to lose.
 *   - Every flush is followed by a content RE-READ; a mismatch stops recording.
 *
 * ENDIANNESS: every multi-byte field is little-endian, written byte by byte. */
#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>

/* Placement of the engine's two hold statics (the frozen-timestamp hook is read by get_fattime WHILE a card
 * transfer runs, when ROM is unmapped): on the cartridge define this as tonc's EWRAM_BSS (compiler line or
 * before the include); it is empty on the host. */
#ifndef JRN_EWRAM_BSS
#define JRN_EWRAM_BSS
#endif
/* Placement of the CRC32 byte kernel (#302): on the cartridge an IWRAM ARM function; empty on the host. */
#ifndef JRN_IWRAM_CODE
#define JRN_IWRAM_CODE
#endif

/* ---- format constants (the values as landed) ------------------------------------- */
#define JRN_SEG_SIZE   65536u   /* every segment file is exactly this long, pre-zeroed */
#define JRN_SEG_HDR       32u   /* 'PDJS', ver u16, ring u16, index u32, zeros, crc32   */
#define JRN_REC_BASE     512u   /* the FIRST record starts here: sector 0 belongs to the header alone and is
                                 * never rewritten after activation (an append flush can never tear it) */
#define JRN_REC_HDR       52u   /* fixed record header (below)                          */
#define JRN_REC_MIN       56u   /* header + CRC32 = a span-less marker record           */
#define JRN_REC_MAX      512u   /* no record is ever longer                             */
#define JRN_PEND_CAP     512u   /* the pending (batched) buffer                         */
#define JRN_FLUSH_HEADROOM 384u /* free pending bytes below which a flush is REQUESTED  */
#define JRN_MAX_SEGS      16u   /* retention cap: cfg.max_segs is clamped to this (the ring is max_segs + 1 slots) */
#define JRN_RING_MAX      17u   /* JRN_MAX_SEGS + 1 spare slot: the per-segment index and every walk bound derive from it */
#define JRN_NAME_LEN      24u
#define JRN_NREG_MAX      16u   /* regions in an image (Gen-3: 14 sections)             */
#define JRN_SPAN_HDR       6u   /* region u8, rsv u8, off u16, len u16                  */
#define JRN_MERGE_GAP      4u   /* unchanged bytes tolerated inside one diff span       */
#define JRN_PATH_MAX      72u
#define JRN_PEND_MAXN      9u   /* JRN_PEND_CAP / JRN_REC_MIN, rounded down             */
/* Every walk and scan bound DERIVES from the ring capacity (never a guess): the most records the whole
 * ring can hold is JRN_RING_MAX segments x records of at least JRN_REC_MIN bytes. A parent-chain walk,
 * the open scan and the anchor walk can therefore never lose the tail of a long session (the old fixed
 * 512-hop cap dropped the re-apply offer of any session longer than 512 steps). */
#define JRN_WALK_MAX     (JRN_RING_MAX * ((JRN_SEG_SIZE - JRN_REC_BASE) / JRN_REC_MIN))
/* THE VERSION RULE (slice-1 bounce, ruling 4). A slot header is `free` (bad magic or bad CRC: never
 * written, retired, or torn), `live` (magic + CRC + a version and a shape THIS build knows), or
 * FOREIGN (magic + CRC hold but the version, index, ring size or the region layout is one this build
 * does not know). Any foreign slot makes the WHOLE journal read-only: jrn_open returns JRN_E_VERSION
 * with j->readonly and j->foreign set and no segment mapped; activation, retire, flush and repair
 * all refuse. A reader must never zero, recycle or trim what it does not understand. So: ANY change
 * a current reader would reject or misread (a new record kind or flag, a new header field with
 * meaning, a moved offset, a bigger ring) BUMPS JRN_SEG_VER. */
#define JRN_SEG_VER        1u   /* the version every segment is activated with unless it is chain-capable (below) */
#define JRN_SEG_VER2       2u   /* v2 (z9, D10): the segment MAY hold chained steps (kind 3 parts + a step aux >= 2). A reader
                                 * of THIS build reads v1 and v2; a v1-only (older) build meets v2 as FOREIGN: "no history", read-only,
                                 * nothing zeroed. A journal may mix v1 and v2 segments (an old journal continued). */
#define JRN_CHAIN_MAX      8u   /* records in one chained step (head + <= 7 parts): capacity ~3.6 KiB of span bytes */
#define JRN_PDR_VER        1u   /* the .pdr redirect layout version (byte 3) */

/* THE SEGMENT HEADER (sector 0, 32 bytes at offset 0; the rest of the sector is zero and never written):
 *   [0..3]  'PDJS'                    magic          reader: bad magic = FREE slot
 *   [4..5]  version u16 (JRN_SEG_VER or JRN_SEG_VER2)               reader: unknown = FOREIGN  (the version rule above)
 *   [6..7]  ring u16, 2..JRN_RING_MAX               reader: outside = FOREIGN
 *   [8..11] logical index u32, 1..9999              reader: outside = FOREIGN (the cap is part of the format)
 *   [12]    nreg u8      regions of the image       reader: 0 / > JRN_NREG_MAX = FOREIGN; != cfg.nreg = FOREIGN
 *   [13]    reserved                                writer ZERO, reader IGNORES
 *   [14..15] reg_size u16 bytes per region          reader: 0 = FOREIGN; != cfg.reg_size = FOREIGN
 *   [16..27] reserved                               writer ZERO, reader IGNORES
 *   [28..31] crc32 of [0..27]                       reader: mismatch = FREE slot (a torn write is never foreign)
 * A layout mismatch (a different game layout under the same key) is therefore DETECTABLE -- the journal opens
 * read-only with JRN_E_VERSION -- never a silent JRN_ANCHOR_NEWROOT. "Reader ignores" bytes are covered by the
 * crc, so a writer that starts using them MUST bump JRN_SEG_VER.
 * STEP NAMES are printable ASCII 0x20..0x7E, NUL padded, at most JRN_NAME_LEN bytes (longer is truncated):
 * jrn_step_begin refuses anything else with JRN_E_ARG. The READER accepts any bytes (the crc protects them):
 * a UI must render non-printables as '?'.
 * THE KEY (jrn_key64): FNV-1a 64 over the CANONICAL name bytes -- everything before the first 0xFF Gen-3
 * terminator, so the 0xFF padding of the name field and garbage after the terminator never move the key --
 * then TID, SID (u16 LE each), gender and frlg as BOOLEANS (0 or non-zero, hashed as 0 / 1). frlg = 0 is the
 * Ruby/Sapphire/Emerald layout, 1 the FireRed/LeafGreen layout: the two layouts never share a journal. */

/* Record: [0..3] 'PDJR' | [4..5] len | [6] flags (bit0 crossed, bits4-5 kind) |
 * [7] nspans | [8..11] seq | [12..15] parent | [16..19] aux | [20..23] pre_hash |
 * [24..27] post_hash | [28..51] name[24] | spans... | crc32 of everything before it.
 * kind 0 = step, 1 = cursor marker (parent = the seq the cursor moved to, aux = the
 * redo tip), 2 = discarded marker (parent = the anchor kept, aux = the head thrown away),
 * 3 = CHAIN PART (v2 segments only, D10). Span: {region, rsv, off, len} then before[len] then after[len].
 *
 * CHAINED STEPS (v2). A step whose span diff does not fit one record is a CHAIN of N = 2..JRN_CHAIN_MAX records
 * with consecutive seqs, all inside ONE segment: a kind-0 HEAD (parent = the normal undo parent, aux = N, pre/post =
 * the whole step's hashes, crossed = the step's flag) then N-1 kind-3 PARTS, part k (k = 1..N-1) carrying seq = head.seq + k,
 * parent = head.seq, aux = k, crossed = 0, pre/post = the head's, nspans >= 1. Spans are split across the records in image
 * order (one run may be cut at a record boundary; the pieces are disjoint). A plain step has aux 0 (no v1 writer ever stored
 * another value). Cursor, tip, parent and marker links only ever name a HEAD's seq, so every walk sees a chain as ONE step.
 * COMMIT: the writer lays the parts down first (back to front) and the HEAD LAST, each verified: the head becomes valid -- and
 * with it the whole chain -- only when its last sector lands, so a power cut leaves the chain absent (orphan parts beyond the
 * tail are zeroed by the open-time repair). The reader accepts a chain only when ALL N records are present, consecutive and
 * well-formed; a head without its parts ends the valid prefix AT the head (never a partial step). */
enum { JRN_KIND_STEP = 0, JRN_KIND_CURSOR = 1, JRN_KIND_DISCARD = 2, JRN_KIND_PART = 3 };

/* ---- results ---------------------------------------------------------------------- */
enum {
  JRN_OK = 0,
  JRN_NOOP = 1,          /* step_end with no change; redo/undo with nothing to do (see fn) */
  JRN_E_ARG = -1,        /* bad argument / bad state for the call                          */
  JRN_E_IO = -2,         /* the fs seam failed                                             */
  JRN_E_VERIFY = -3,     /* the re-read did not match what was written: recording STOPS    */
  JRN_E_FULL = -4,       /* pending buffer or segment space exhausted (nothing was lost)   */
  JRN_E_TOOBIG = -5,     /* one record cannot fit the buffer/segment at all; for a chain: more than JRN_CHAIN_MAX records */
  JRN_E_RDONLY = -6,     /* opened read-only (Everdrive posture)                           */
  JRN_E_STOPPED = -7,    /* recording was stopped by an earlier verify failure             */
  JRN_E_NOSEG = -8,      /* no tail segment exists: run jrn_prepare at a safe moment       */
  JRN_E_CROSSED = -9,    /* the step is a crossed floor: undo/redo refuses                 */
  JRN_E_FLOOR = -10,     /* the parent record was compacted away (orphan floor)            */
  JRN_E_DIVERGED = -11,  /* the image bytes at the spans are not what the cursor rule wants */
  JRN_E_NOTHING = -12,   /* nothing to undo (cursor at the root) */
  JRN_E_LOOP = -13,      /* redirect chain too long / cyclic, or a chain walk ran away     */
  JRN_E_EXISTS = -14,    /* a redirect for that key already exists and points elsewhere    */
  JRN_E_STATE = -15,     /* an internal invariant failed (a bug, never expected)           */
  JRN_E_VERSION = -16,   /* a FOREIGN journal (see JRN_SEG_VER below): jrn_open returns it with j
                          * readonly and EMPTY; nothing is read past it, nothing is ever written  */
  JRN_E_TORN = -17       /* a chained undo/redo failed part way AND its rollback failed: the image holds a PARTIAL
                          * step. The cursor did not move; the caller re-derives its copies and floors the history */
};

/* ---- the fs seam (source/journal_fs.c binds it to FatFs; the host sweep injects cuts
 * underneath it, at the disk_write layer) ------------------------------------------ */
typedef void (*JrnListFn)(void* arg, const char* name);
typedef int  (*JrnScanFn)(void* arg, uint32_t off, const uint8_t* buf, uint32_t n, int last, uint32_t* used);

typedef struct JrnFs {
  void* ctx;
  int      (*mkdir)(void* ctx, const char* path);                    /* 0 ok or exists   */
  /* file size; -1 = ABSENT (no such file / path), -2 = a card I/O error (possibly transient), -3 = the directory
   * STRUCTURE is damaged (FatFs FR_INT_ERR: a torn exFAT entry set -- persistent, not retryable). Never conflate
   * them: a redirect lookup treats -3 as `no redirect`, everything else treats -2 and -3 as an error. */
  long     (*size)(void* ctx, const char* path);
  int      (*read)(void* ctx, const char* path, uint32_t off, void* buf, uint32_t n);
  /* IN PLACE, never grows the file, synced before returning. 0 ok. */
  int      (*write)(void* ctx, const char* path, uint32_t off, const void* buf, uint32_t n);
  /* STAGED FIRST FILL. Create-OR-COMPLETE a `size`-byte file: open (creating if absent), extend it to
   * `size` (the FAT chain is allocated) WITHOUT writing the body, overwrite the first `headn` bytes
   * (<= 512) with ZEROS, sync. The body's content is UNDEFINED (whatever the card held): a caller must
   * zero it (`zero`) before it may be read as a segment. A file longer than `size` fails. */
  int      (*alloc)(void* ctx, const char* path, uint32_t size, uint32_t headn);
  /* ONE handle, IN PLACE, never grows the file: for each 512-byte chunk of [off, off+n) (aligned to
   * `off`; the last may be shorter) that holds a non-zero byte, overwrite it with zeros; sync once. 0 ok. */
  int      (*zero)(void* ctx, const char* path, uint32_t off, uint32_t n);
  /* Create-OR-COMPLETE: open (creating if absent), zero-fill from the file's current end to `size`, then
   * write head[0..headn) at offset 0 LAST, sync. A file longer than `size` fails. (Small files: .pdr.) */
  int      (*create_zero)(void* ctx, const char* path, uint32_t size,
                          const void* head, uint32_t headn);
  /* NO unlink / rename / delete op exists in this seam, on purpose: a journal can never remove a file. */
  /* file names of `dir` (not sub-directories) through cb. 0 = listed, 1 = the directory does NOT exist (a
   * normal answer for a journal that was never started), < 0 = a card error (never conflate the two: a
   * transient read error must not look like an empty journal). */
  int      (*list)(void* ctx, const char* dir, JrnListFn cb, void* arg);
  /* ONE handle, chunked READ (the open/locate/anchor scans). `start` is a multiple of 512. The seam reads
   * the file one aligned <= 512-byte chunk at a time (one disk read per sector) into a buffer that already
   * holds the bytes cb did not consume last time, and calls cb(arg, off, buf, n, last, &used): buf[0..n)
   * is the file at [off, off+n) (n <= 1023), `last` = 1 when this window reaches `limit`. cb consumes as
   * many WHOLE records as buf holds and sets *used to the bytes it consumed; it returns 1 to go on (what
   * it did not consume, always < 512 bytes, is carried into the next window) or 0 to stop. When `last` is
   * set the scan ends after this call whatever cb returns. Returns 0 when the scan ended, -1 on a card
   * error. */
  int      (*scan)(void* ctx, const char* path, uint32_t start, uint32_t limit, JrnScanFn cb, void* arg);
  uint32_t (*stamp)(void* ctx, const char* path);                    /* file's FAT stamp */
} JrnFs;

/* ---- the image accessor: the engine never owns the image ---------------------------- */
typedef struct JrnImage {
  void* ctx;
  int (*get)(void* ctx, uint8_t region, uint16_t off, uint8_t* dst, uint16_t n);       /* 0 ok */
  int (*set)(void* ctx, uint8_t region, uint16_t off, const uint8_t* src, uint16_t n); /* 0 ok */
} JrnImage;

/* ---- the engine ---------------------------------------------------------------------- */
typedef struct JrnCfg {
  const JrnFs* fs;
  const char*  root;      /* e.g. "/PokeDNA/journal" (must outlive the Jrn) */
  uint64_t     key;       /* save_key64 -- resolved through .pdr redirects by jrn_open */
  uint8_t      nreg;      /* regions in the image (<= JRN_NREG_MAX)                     */
  uint16_t     reg_size;  /* bytes per region                                            */
  uint8_t      max_segs;  /* compaction cap (segment count), 0 = 16                      */
  uint8_t      readonly;  /* Everdrive posture: never mkdir/create/write/zero            */
} JrnCfg;

enum { JRN_ANCHOR_EMPTY = 0,   /* the journal has no records */
       JRN_ANCHOR_MATCH = 1,   /* the image sits on the last cursor path */
       JRN_ANCHOR_BRANCH = 2,  /* matched an older record on another branch */
       JRN_ANCHOR_NEWROOT = 3  /* unrecognized image: a new root */ };

typedef struct Jrn {
  const JrnFs* fs;
  const char*  root;
  uint64_t     key;            /* the DIRECTORY key (after redirects)          */
  uint32_t     next_seq;       /* seq of the next record sealed                */
  uint32_t     cursor;         /* seq the image sits on (0 = before any step)  */
  uint32_t     tip;            /* redo target (== cursor when nothing to redo) */
  uint32_t     tail_off;       /* append offset inside tail_seg                */
  uint32_t     crc[JRN_NREG_MAX];   /* CRC32 of each region as the engine tracks it */
  uint32_t     bcrc[JRN_NREG_MAX];  /* tentative crcs while a step is being built   */
  uint16_t     seg_first, seg_last, tail_seg;  /* LOGICAL segment indices, 0 = none  */
  uint16_t     ring;           /* slot files in the ring (from the header), 0 = no journal yet */
  uint16_t     reg_size;
  uint32_t     seg_seq[JRN_RING_MAX];   /* the per-segment first-seq INDEX, by ring slot (slot-1): seq of the first
                                         * record the slot's segment holds, 0 = none yet. Built by open, kept by
                                         * flush/activate/retire; locate/undo/redo start their scan in the right segment. */
  uint16_t     pend_len;       /* bytes of sealed pending records              */
  uint16_t     bld_len;        /* bytes of the record under construction (0 = none) */
  uint8_t      nreg, max_segs, pend_n, nspans;
  uint8_t      readonly, stopped, flush_wanted, anchor, offer, foreign;   /* foreign: JRN_SEG_VER rule above */
  uint8_t      pend[JRN_PEND_CAP];
} Jrn;

/* Parsed record header (also what find/scan hand back). */
typedef struct JrnRec {
  uint8_t  kind, crossed, nspans;
  uint16_t len;
  uint32_t seq, parent, aux, pre, post;
  char     name[JRN_NAME_LEN + 1];
} JrnRec;

/* ---- pure helpers ---------------------------------------------------------------------- */
uint32_t jrn_crc32_update(uint32_t crc, const void* data, uint32_t n); /* chainable, zlib-compatible */
/* FNV-1a 64 over the CANONICAL name + TID + SID + gender + FRLG layout bit (see THE KEY above). Plain
 * arguments only. */
uint64_t jrn_key64(const uint8_t* name, uint8_t name_len, uint16_t tid, uint16_t sid,
                   uint8_t gender, uint8_t frlg);
void     jrn_key_hex(uint64_t key, char out[17]);        /* 16 lowercase hex digits + NUL */

/* The frozen-timestamp hook. GBA get_fattime (slice 2) and the host RAM disk both return
 * jrn_fattime_filter(live): the held stamp while a journal flush is writing, else live. */
uint32_t jrn_fattime_filter(uint32_t live);
int      jrn_stamp_held(void);

/* ---- keys and redirects ---------------------------------------------------------------- */
/* Write <root>/r/<newkey>.pdr -> `target`: 16 bytes, 'P' 'D' 'R', version u8 (JRN_PDR_VER), target u64, crc32 of
 * the first 12. Created in place under its final name in the SUBDIRECTORY <root>/r/ (never beside the key
 * directories: a torn exFAT create there re-parents nothing else) and NEVER unlinked or replaced (an invalid
 * leftover of exactly the wrong size fails with JRN_E_IO; one of the right size is overwritten in place).
 * A redirect with a valid crc and an unknown version is FOREIGN: resolving through it fails with
 * JRN_E_VERSION (the same rule as the segment header). Flattens: `target` is
 * first resolved through existing redirects. 0 ok; JRN_E_EXISTS if newkey already redirects
 * elsewhere; JRN_OK (no-op) when it already points at the target or when newkey == target. */
int jrn_redirect_write(const JrnFs* fs, const char* root, uint64_t newkey, uint64_t target);
/* Follow .pdr redirects from `key` (at most 8 hops). JRN_E_LOOP on a cycle/overlong chain. */
int jrn_key_resolve(const JrnFs* fs, const char* root, uint64_t key, uint64_t* out);

/* ---- open / safe-moment operations ------------------------------------------------------- */
/* Read the journal: resolve the key, validate the longest valid prefix, ZERO the torn tail
 * IN PLACE (unless readonly), derive the per-region CRCs from `img`, build the per-segment first-seq
 * index and anchor the cursor by post_hash. Never creates a segment (see jrn_prepare).
 * Returns JRN_OK; JRN_E_VERSION when the journal (or a redirect to it) is FOREIGN -- j is then read-only,
 * foreign and EMPTY, nothing was written; or JRN_E_IO on ANY card error: a read error is never a shorter
 * ring, an empty journal, a moved tail or another key -- j must not be used after an error. */
int  jrn_open(Jrn* j, const JrnCfg* cfg, const JrnImage* img);
/* Safe moment (load / after a verified exit save): make the dir, the tail segment and the
 * NEXT segment exist. jrn_prepare_first makes only the dir, the whole ring of slot files (first
 * fill) and the first segment (a fresh key); jrn_prepare = prepare_first + the spare (an in-place
 * activation of the next slot, retiring the oldest first when the ring is full). */
int  jrn_prepare_first(Jrn* j);
int  jrn_prepare(Jrn* j);
/* Safe moment: RETIRE (zero the header in place, no delete) the oldest segments while more than
 * min(max_segs, ring-1) are live (never the tail). jrn_prepare also compacts when the ring has no free slot. */
int  jrn_compact(Jrn* j);

/* ---- recording ----------------------------------------------------------------------------- */
/* Build one step in the pending buffer: begin, one _region per changed region (the caller
 * hands the OLD and NEW full region blocks; the engine stores diff-only spans and checks
 * `old` against the crc it tracks), then end. JRN_NOOP from _end when nothing changed.
 * Overflow is LOUD: _region/_end return JRN_E_FULL and set flush_wanted; the half-built
 * step is discarded (jrn_step_abort is implied) and the caller flushes and re-stages. */
int  jrn_step_begin(Jrn* j, const char* name, int crossed);
int  jrn_step_region(Jrn* j, uint8_t region, const uint8_t* old_blk, const uint8_t* new_blk);
int  jrn_step_end(Jrn* j);
void jrn_step_abort(Jrn* j);
/* CHAINED STEP (z9, D10): record ONE step whose span diff may exceed a record, as a chain of 2..JRN_CHAIN_MAX records written
 * straight to the tail segment (not through the pending buffer: the diff is split into <=512-byte records at flush-free time).
 * `blk[r]` = the OLD and NEW full block of region r (old_blk NULL = the region is not part of the step), r < nblk <= nreg.
 * Preconditions: no step under construction and NOTHING PENDING (else JRN_E_FULL + flush_wanted: flush, then call again).
 * Returns JRN_OK (the step is on disk, verified; cursor/tip are on its HEAD), JRN_NOOP (nothing changed), JRN_E_TOOBIG (more than
 * JRN_CHAIN_MAX records: nothing was written), JRN_E_DIVERGED (an old block does not hash to the tracked crc), JRN_E_FULL (no room /
 * nothing-pending violated), JRN_E_IO / JRN_E_VERIFY (a card error; only a VERIFY mismatch stops recording -- a read error during the
 * post-write verify reports JRN_E_IO without stopping, exactly like jrn_flush), or the step_begin refusals.
 * A chain of >= 2 records needs a v2 segment: the tail if it is v2 and has room, else the EXISTING spare, re-stamped v2 on demand (a retire +
 * a header write: no body zero-fill, never a segment that holds records; a v1 header is never rewritten in place -- a tear would free the slot).
 * It never CREATES a spare (a zero-filled activation is a safe-moment job, jrn_prepare, which inherits the last segment's version): no spare ->
 * JRN_E_FULL and nothing written. The HEAD is written LAST: a cut leaves no chain, and an incomplete chain is never read as a step. */
typedef struct JrnBlk { const uint8_t* old_blk; const uint8_t* new_blk; } JrnBlk;
int  jrn_chain_record(Jrn* j, const char* name, int crossed, const JrnBlk* blk, uint8_t nblk);
int  jrn_flush_wanted(const Jrn* j);      /* pending buffer nearly full: flush at the next idle frame */
/* Write the pending records (back to front), then re-read and compare. */
int  jrn_flush(Jrn* j);
/* Markers (pending, flushed at the next rest point). */
int  jrn_mark_discarded(Jrn* j);          /* the user declined the re-apply: cursor stays */

/* ---- undo / redo / re-apply ------------------------------------------------------------------ */
/* All two-pass against `img`: pass 1 verifies EVERY span's current bytes (undo: == after,
 * redo: == before), pass 2 applies; nothing is written when pass 1 fails. Cursor markers
 * are recorded for disk steps; a still-pending step is simply popped (no SD I/O). */
int  jrn_undo(Jrn* j, const JrnImage* img, JrnRec* undone);   /* JRN_OK, or a JRN_E_* */
int  jrn_redo(Jrn* j, const JrnImage* img, JrnRec* redone);   /* JRN_OK / JRN_NOOP (nothing) / JRN_E_* */
/* How many steps the redo / re-apply chain from the cursor to the tip holds (*total), how many
 * of them come BEFORE the first crossed record (*avail: redo and re-apply STOP there), and,
 * when non-NULL, that first crossed record (*stop). Returns 1 when a crossed record cuts the
 * chain short, 0 when it is clean, a JRN_E_* on failure. */
int  jrn_redo_info(Jrn* j, uint32_t* avail, uint32_t* total, JrnRec* stop);
int  jrn_recompute(Jrn* j, const JrnImage* img);              /* re-derive crc[] from the image */

/* ---- accessors ---------------------------------------------------------------------------------- */
uint32_t jrn_hash(const Jrn* j);          /* CRC32 over the tracked region CRC32s (pre/post_hash) */
uint32_t jrn_cursor(const Jrn* j);
uint32_t jrn_tip(const Jrn* j);
/* 1 when the journal holds recorded steps the loaded image does not (the load-time re-apply
 * offer, D1): the last cursor position is past the anchor. An undone-and-saved image is NOT an
 * offer -- its cursor marker says the cursor sits where the image does. Cleared by a decline
 * (jrn_mark_discarded), an undo, a new step, or by redoing up to the target. */
int      jrn_offer(const Jrn* j);
int      jrn_pending(const Jrn* j);       /* sealed records not yet on disk */
/* Look a STEP record up by seq (pending first, then the segments). 0 found, else JRN_E_FLOOR. */
int      jrn_find(Jrn* j, uint32_t seq, JrnRec* out);

/* ---- the CHILDREN WALK (#304): READ-ONLY and ADDITIVE -- the format stores parent pointers only, so "the steps whose parent is X" is a
 * SCAN: ONE linear pass over the segments from the one that holds X (seg_for_seq) through the tail, then the pending buffer. No stored
 * byte, header field or SEG_VER changes (v1 and v2 segments read alike). Only STEP records count (kind 0): a chain HEAD is ONE step, its
 * kind-3 parts and the cursor / discarded markers are never children. ONE record header in RAM at a time; nothing is held across a read.
 * The cost is the pass itself, independent of how many parents are asked about: jrn_kid_counts answers EVERY parent of a window in one pass. */
typedef struct JrnKid { uint32_t seq; uint8_t crossed; char name[JRN_NAME_LEN + 1]; } JrnKid;
/* count[i] = how many steps have parent[i], NOT counting the step skip[i] (the branch child the caller already knows: a step is never its own
 * sibling; skip[i] = 0 counts them all). parent[] must be STRICTLY DESCENDING (0 allowed as the last entry = the root: steps with no parent).
 * n <= 255. Returns JRN_OK or JRN_E_ARG / JRN_E_IO (a card error: count[] is then meaningless). */
int      jrn_kid_counts(Jrn* j, const uint32_t* parent, const uint32_t* skip, uint16_t* count, uint8_t n);
/* The children of ONE parent in seq order, minus `skip`: the ones numbered first .. first+cap-1 go to out[0..*got). *got < cap means the
 * list ended. Same pass, same rules; cap >= 1. */
int      jrn_kid_list(Jrn* j, uint32_t parent, uint32_t skip, uint16_t first, JrnKid* out, uint8_t cap, uint8_t* got);

#endif /* JOURNAL_H */
