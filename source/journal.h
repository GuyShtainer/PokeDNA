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
 *   - Segments are PRE-ZEROED fixed-size files. Appends overwrite in place and never
 *     grow the FAT chain; the file is never truncated (torn tails are zeroed in place).
 *   - A segment is built as NNNN.tmp then RENAMED to NNNN.pdj, so a visible .pdj is
 *     always complete. A cut mid-fill leaves a .tmp the next safe moment deletes.
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

/* ---- format constants (the values as landed) ------------------------------------- */
#define JRN_SEG_SIZE   65536u   /* every segment file is exactly this long, pre-zeroed */
#define JRN_SEG_HDR       32u   /* 'PDJS', ver u16, rsv u16, index u32, zeros, crc32   */
#define JRN_REC_HDR       52u   /* fixed record header (below)                          */
#define JRN_REC_MIN       56u   /* header + CRC32 = a span-less marker record           */
#define JRN_REC_MAX      512u   /* no record is ever longer                             */
#define JRN_PEND_CAP     512u   /* the pending (batched) buffer                         */
#define JRN_FLUSH_HEADROOM 384u /* free pending bytes below which a flush is REQUESTED  */
#define JRN_NAME_LEN      24u
#define JRN_NREG_MAX      16u   /* regions in an image (Gen-3: 14 sections)             */
#define JRN_SPAN_HDR       6u   /* region u8, rsv u8, off u16, len u16                  */
#define JRN_MERGE_GAP      4u   /* unchanged bytes tolerated inside one diff span       */
#define JRN_PATH_MAX      72u
#define JRN_PEND_MAXN      9u   /* JRN_PEND_CAP / JRN_REC_MIN, rounded down             */
#define JRN_WALK_MAX     512u   /* hard bound on any parent-chain walk                  */

/* Record: [0..3] 'PDJR' | [4..5] len | [6] flags (bit0 crossed, bits4-5 kind) |
 * [7] nspans | [8..11] seq | [12..15] parent | [16..19] aux | [20..23] pre_hash |
 * [24..27] post_hash | [28..51] name[24] | spans... | crc32 of everything before it.
 * kind 0 = step, 1 = cursor marker (parent = the seq the cursor moved to, aux = the
 * redo tip), 2 = discarded marker (parent = the anchor kept, aux = the head thrown away).
 * Span: {region, rsv, off, len} then before[len] then after[len]. */
enum { JRN_KIND_STEP = 0, JRN_KIND_CURSOR = 1, JRN_KIND_DISCARD = 2 };

/* ---- results ---------------------------------------------------------------------- */
enum {
  JRN_OK = 0,
  JRN_NOOP = 1,          /* step_end with no change; redo/undo with nothing to do (see fn) */
  JRN_E_ARG = -1,        /* bad argument / bad state for the call                          */
  JRN_E_IO = -2,         /* the fs seam failed                                             */
  JRN_E_VERIFY = -3,     /* the re-read did not match what was written: recording STOPS    */
  JRN_E_FULL = -4,       /* pending buffer or segment space exhausted (nothing was lost)   */
  JRN_E_TOOBIG = -5,     /* one record cannot fit the buffer/segment at all                */
  JRN_E_RDONLY = -6,     /* opened read-only (Everdrive posture)                           */
  JRN_E_STOPPED = -7,    /* recording was stopped by an earlier verify failure             */
  JRN_E_NOSEG = -8,      /* no tail segment exists: run jrn_prepare at a safe moment       */
  JRN_E_CROSSED = -9,    /* the step is a crossed floor: undo/redo refuses                 */
  JRN_E_FLOOR = -10,     /* the parent record was compacted away (orphan floor)            */
  JRN_E_DIVERGED = -11,  /* the image bytes at the spans are not what the cursor rule wants */
  JRN_E_NOTHING = -12,   /* nothing to undo (cursor at the root) */
  JRN_E_LOOP = -13,      /* redirect chain too long / cyclic, or a chain walk ran away     */
  JRN_E_EXISTS = -14,    /* a redirect for that key already exists and points elsewhere    */
  JRN_E_STATE = -15      /* an internal invariant failed (a bug, never expected)           */
};

/* ---- the fs seam (source/journal_fs.c binds it to FatFs; the host sweep injects cuts
 * underneath it, at the disk_write layer) ------------------------------------------ */
typedef void (*JrnListFn)(void* arg, const char* name);

typedef struct JrnFs {
  void* ctx;
  int      (*mkdir)(void* ctx, const char* path);                    /* 0 ok or exists   */
  long     (*size)(void* ctx, const char* path);                     /* -1 absent        */
  int      (*read)(void* ctx, const char* path, uint32_t off, void* buf, uint32_t n);
  /* IN PLACE, never grows the file, synced before returning. 0 ok. */
  int      (*write)(void* ctx, const char* path, uint32_t off, const void* buf, uint32_t n);
  /* NEW file only (fails when it exists): head[0..headn), zeros to `size`, synced. */
  int      (*create_zero)(void* ctx, const char* path, uint32_t size,
                          const void* head, uint32_t headn);
  int      (*rename)(void* ctx, const char* from, const char* to);
  int      (*unlink)(void* ctx, const char* path);                   /* absent = 0       */
  int      (*list)(void* ctx, const char* dir, JrnListFn cb, void* arg);
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
  uint16_t     seg_first, seg_last, tail_seg;  /* 0 = no segment                   */
  uint16_t     reg_size;
  uint16_t     pend_len;       /* bytes of sealed pending records              */
  uint16_t     bld_len;        /* bytes of the record under construction (0 = none) */
  uint8_t      nreg, max_segs, pend_n, nspans;
  uint8_t      readonly, stopped, flush_wanted, anchor;
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
/* FNV-1a 64 over name + TID + SID + gender + FRLG layout bit. Plain arguments only. */
uint64_t jrn_key64(const uint8_t* name, uint8_t name_len, uint16_t tid, uint16_t sid,
                   uint8_t gender, uint8_t frlg);
void     jrn_key_hex(uint64_t key, char out[17]);        /* 16 lowercase hex digits + NUL */

/* The frozen-timestamp hook. GBA get_fattime (slice 2) and the host RAM disk both return
 * jrn_fattime_filter(live): the held stamp while a journal flush is writing, else live. */
uint32_t jrn_fattime_filter(uint32_t live);
int      jrn_stamp_held(void);

/* ---- keys and redirects ---------------------------------------------------------------- */
/* Write <root>/<newkey>.pdr -> `target` (atomic: .tmp then rename). Flattens: `target` is
 * first resolved through existing redirects. 0 ok; JRN_E_EXISTS if newkey already redirects
 * elsewhere; JRN_OK (no-op) when it already points at the target or when newkey == target. */
int jrn_redirect_write(const JrnFs* fs, const char* root, uint64_t newkey, uint64_t target);
/* Follow .pdr redirects from `key` (at most 8 hops). JRN_E_LOOP on a cycle/overlong chain. */
int jrn_key_resolve(const JrnFs* fs, const char* root, uint64_t key, uint64_t* out);

/* ---- open / safe-moment operations ------------------------------------------------------- */
/* Read the journal: resolve the key, validate the longest valid prefix, ZERO the torn tail
 * IN PLACE (unless readonly), derive the per-region CRCs from `img`, and anchor the cursor
 * by post_hash. Never creates a segment (see jrn_prepare). */
int  jrn_open(Jrn* j, const JrnCfg* cfg, const JrnImage* img);
/* Safe moment (load / after a verified exit save): make the dir, the tail segment and the
 * NEXT segment exist; delete stale .tmp files. */
int  jrn_prepare(Jrn* j);
/* Safe moment: delete the oldest segments while more than max_segs exist (never the tail). */
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
int      jrn_pending(const Jrn* j);       /* sealed records not yet on disk */
/* Look a STEP record up by seq (pending first, then the segments). 0 found, else JRN_E_FLOOR. */
int      jrn_find(Jrn* j, uint32_t seq, JrnRec* out);

#endif /* JOURNAL_H */
