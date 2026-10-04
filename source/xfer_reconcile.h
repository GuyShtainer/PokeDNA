#ifndef XFER_RECONCILE_H
#define XFER_RECONCILE_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_sidecar.h"   /* GbscEntry, XR_KIND_*, XR_STATE_*, XR_DIR_*                  */

/*
 * BACKLOG #150 S150-11 decision 2/3 -- the pure classification core for the
 * cross-generation transfer-ledger reconcile. PURE C: no tonc, no FatFs, no GBA
 * headers, no statics -- dual-compiles for tests/host_xfer_reconcile_test.c exactly
 * like every other pure-C core in this tree (gb_reconcile.c is the model).
 *
 * A NATIVE_HOME sidecar entry (source/gb_sidecar.h) describes one Bank cell that was
 * converted DOWN into a Gen-3 record (XR_DIR_ABROAD_G3) or lifted UP into a Game Boy
 * save (XR_DIR_ABROAD_GB). Depending on what actually happened since, the entry, its
 * Bank cell and its abroad copy can be in one of the row shapes the decision-2 table
 * in the brief enumerates: still in two places (a power cut before the source was
 * cleared), an orphaned record whose promised destination never landed, a restored
 * duplicate (S150-8b/S150-9 put the native cell BACK in the Bank and marked the entry
 * XR_STATE_RESTORED), or the normal end state. xrc_classify() is a pure table over
 * what the caller observed this session -- it makes no ledger/Bank/save reads itself.
 */

/* ---- row kinds, one per decision-2 table row ------------------------------------ */
typedef enum {
  XRC_PENDING_BOTH = 0,   /* did not finish; both copies here -- informational        */
  XRC_PENDING_ORPHAN,     /* transfer never landed -- DELETE RECORD                   */
  XRC_PENDING_NOBANK,     /* copy landed, record unproven -- drag it back to restore  */
  XRC_PENDING_LOST,       /* only the record is left -- RESTORE or DELETE (loss)      */
  XRC_DUP_BANK,           /* in two places -- REMOVE DUPLICATE (Bank-open star)       */
  XRC_DEFERRED,           /* queued for a Bank->PC deletion -- never a duplicate      */
  XRC_ABROAD,             /* in <game>'s PC, restorable -- the normal end state       */
  XRC_LOST,                /* only the record is left -- RESTORE or DELETE (loss)      */
  XRC_ABROAD_GB,          /* in a Game Boy save -- the scan never opens a GB save     */
  XRC_DUP_G3,             /* restored; the Gen-3 copy is a duplicate -- RELEASE COPY  */
  XRC_STALE,              /* restored/lost; record is stale -- DELETE RECORD          */
  XRC_RESTORED_MOVED,     /* restored copy left the Bank again -- drag it back        */
  XRC_DAYCARE,            /* in the Day-Care -- no action (decision 15)               */
  XRC_STALE_KEY,          /* PID changed; record not linked -- RE-KEY (decision 15)   */
  XRC_AMBIGUOUS,          /* 2+ matches by key or identity -- no action, logged       */
  XRC_G3HOME,              /* Gen-3 original; the shipped load-time screen owns it     */
  XRC_IN_BANK,             /* #385: the Bank already holds this mon (a restored copy under a NEW serial, the
                            * ledger rewrite never landed) -- DELETE RECORD only, never RESTORE (a clone) */
  XRC_ABROAD_BANK,         /* #387: the Gen-3 copy is parked in the Bank (#270 pass-through), not in the PC --
                            * same normal end state and no actions as XRC_ABROAD, but its detail line says WHERE */
  XRC_UNREAD               /* #386: a Bank box could not be read during the scan, so "nothing found" proves nothing --
                            * a row that would be LOST says "unknown -- re-open later" and offers NO action (RESTORE
                            * would clone a mon that is merely sitting in the unread box) */
} XrcRowKind;

/* Allowed-action mask, decision 8's letters. */
#define XRC_ACT_NONE     0x00u
#define XRC_ACT_REMOVE   0x01u   /* REMOVE DUPLICATE  (8a)                            */
#define XRC_ACT_RELEASE  0x02u   /* RELEASE COPY      (8b)                            */
#define XRC_ACT_RESTORE  0x04u   /* RESTORE TO BANK   (8c)                            */
#define XRC_ACT_DELETE   0x08u   /* DELETE RECORD     (8d)                            */
#define XRC_ACT_REKEY    0x10u   /* RE-KEY            (8e)                            */
#define XRC_ACT_PROMOTE  0x20u   /* MARK FINISHED (#377): PENDING -> CLAIMED, the Gen-3 copy is on the card */

/* What the caller observed THIS SESSION for one entry, before any classification --
 * every field is a plain fact the caller already had to compute (xrc_g3_match_key(),
 * xrc_bank_match(), app_bank_slot_pending()), never re-derived here. Match counts are
 * clamped at 2 by the matchers below ("2 means 2 or more"). */
typedef struct {
  uint8_t kind;                /* XR_KIND_*                                          */
  uint8_t state;                /* XR_STATE_*                                         */
  uint8_t direction;            /* XR_DIR_* -- meaningful only for kind==NATIVE_HOME  */
  int     g3_key_matches;       /* xrc_g3_match_key() count, 0/1/2(+)                 */
  bool    g3_in_daycare;        /* the unique key match landed in a Day-Care slot     */
  int     g3_identity_matches;  /* xrc_g3_match_identity() count, consulted only when
                                 * g3_key_matches == 0 and direction == ABROAD_G3      */
  int     bank_matches;         /* xrc_bank_match() count, 0/1/2(+)                    */
  bool    bank_slot_pending;    /* app_bank_slot_pending() at the matched bank slot    */
  bool    bank_keep;            /* the entry's bank_keep bit (decision 7)              */
  int     bank_g3_matches;      /* xrc_bank_g3_match(): Gen-3 copies PARKED in the Bank
                                 * (#270 pass-through), 0/1/2(+); 0 when never scanned  */
  bool    g3_on_card;           /* #377: nothing is staged in the open save (it equals the last verified
                                 * write), so a Gen-3 key match is ON the card -- the only case in which a
                                 * PENDING row may be marked finished                                    */
  bool    bank_unread;          /* #386: the Bank scan skipped a box it could not read (read error / failed heal) */
  int     bank_ident_matches;   /* #385: native Bank cells that are a RE-SERIAL of the entry's
                                 * original80 (xrc_bank_reserial_match): every hashed byte equal
                                 * except bank_serial. 0/1/2(+); 0 when never scanned            */
} XrcInput;

typedef struct {
  XrcRowKind kind;
  uint8_t    actions;   /* XRC_ACT_* mask, only ever set for a NATIVE_HOME row         */
  bool       kept;      /* decision 2's "flagged kept" row -- dimmed, still listed on
                         * the full screen, SKIPPED by the Bank-open prompt             */
} XrcResult;

/* Decision 2's whole table, ONE call per row. Never reads any buffer -- `in` is a
 * complete, caller-supplied snapshot. */
void xrc_classify(const XrcInput* in, XrcResult* out);

/* ---- locating the copies (decision 3) -------------------------------------------- */

/* Where a key/identity match landed. box == -1 => party (slot = party index);
 * box == -2 => Day-Care (slot = 0 or 1, the physical slot dc_layout() names); box >= 0
 * => PC (slot = 0..G3_IN_BOX-1). Left at -1/-1 on 0 or 2+ matches. */

/* The GEN-3 copy of a NATIVE_HOME/ABROAD_G3 entry, located by `key` == xr_key_g3(rec)
 * over every non-empty party/PC/Day-Care record (decision 3(i)). `sb1` is the
 * reassembled SaveBlock1, `frlg` the party-layout selector (pk_read_party_auto's own
 * convention), `pc` the reassembled PC storage or NULL (no PC this save). `dc_base`/
 * `dc_stride` come from the caller's dc_layout() -- this module never knows `g_game`.
 * An all-zero record (first 8 bytes) is refused outright, same convention as
 * gb_reconcile_match(). Returns the match count, clamped to 2 ("2 or more"). */
int xrc_g3_match_key(const uint8_t* sb1, bool frlg, const uint8_t* pc,
                     uint32_t dc_base, uint32_t dc_stride,
                     uint64_t key, int* where_box, int* where_slot);

/* The identity pass (decision 15): pk_decode_mon() on every non-empty party/PC/
 * Day-Care record, matching pk_national_no(species) == species_written, otId & 0xFFFF
 * == otid16, and the 10 raw nickname bytes at record offset 0x08 == nick10[0..9].
 * Same where-encoding, same clamp-at-2. Species is NOT part of the key match above
 * (gbsc's own fingerprint excludes it, a mon may have evolved), but IS part of the
 * identity pass, which exists to re-find a mon whose PID (and so its key) changed. */
int xrc_g3_match_identity(const uint8_t* sb1, bool frlg, const uint8_t* pc,
                          uint32_t dc_base, uint32_t dc_stride,
                          uint16_t species_written, uint16_t otid16,
                          const uint8_t nick10[10], int* where_box, int* where_slot);

/* The BANK copy of one entry, over a single already-paged-in 2400-byte box
 * (G3_IN_BOX * 80). `by_identity` false: bytes 0..7 of a native cell equal
 * `e->original80[0..7]` (a PENDING/CLAIMED entry's own Bank-source identity,
 * decision 3(ii)). `by_identity` true: bc_unpack() then gen/otid16/dv4/otname_written
 * compared field by field (a RESTORED entry's cell has a NEW ident32, decision 3(iii)/
 * finding (b) -- the four fields gbsc_find() itself compares). Returns the match
 * count, clamped to 2; `*slot` filled only on exactly 1. */
int xrc_bank_match(const uint8_t box2400[2400], const GbscEntry* e, bool by_identity,
                   int* slot);

/* #385: native cells of one box that are a RE-SERIAL of the entry's original: the cell with its bank_serial
 * (bytes 73..76) swapped for `orig_serial` hashes (bc_ident32) to the original's ident32 `orig8[4..7]`. Only
 * xrc_rebuild_cell's output (new serial + ident32, nothing else) qualifies -- a different mon that merely shares
 * gen/OT id/DVs/OT name does not. Count clamped to 2. */
int xrc_bank_reserial_match(const uint8_t box2400[2400], const uint8_t orig8[8], const uint8_t orig_serial[4]);

/* #270 pass-through: the Gen-3 copy of an ABROAD_G3 entry may sit in the Bank (a PC ->
 * Bank drop lands as-is). Counts NON-native cells of one 2400-byte box whose first 8
 * bytes hash to `key` (xr_key_g3()'s FNV-1a-64). All-zero first 8 bytes never match.
 * Returns the count, clamped to 2. */
int xrc_bank_g3_match(const uint8_t box2400[2400], uint64_t key);

/* ---- RESTORE TO BANK (decision 8c) ----------------------------------------------- */

/* Rebuild a native cell from `e->original80` with a FRESH `serial` -- no merge (there
 * is no Gen-3 copy to fold in for a *_LOST row; the S150-9 restore that DOES merge
 * requires one and is a different code path entirely). bc_unpack(e->original80) then
 * bc_pack() with the unpacked meta's flags/origin_game/rtc_epoch and the new serial.
 * Returns 0 on success (out80 written), <0 when e->original80 is not itself a native
 * cell or bc_pack() refuses. */
int xrc_rebuild_cell(const GbscEntry* e, uint32_t serial, uint8_t out80[80]);

/* ---- apply ordering (decision 9) -------------------------------------------------- */

/* Sort `idx[0..n-1]` (a file's entry indices about to be mutated) into DESCENDING
 * order in place -- gbsc_remove()'s compaction shifts every later index down by one,
 * so mutating highest-index-first is the only order under which an earlier removal
 * cannot invalidate a later one's index. Bounded insertion sort (n is always small --
 * at most GBSC_MAX_ENTRIES, 8). */
void xrc_apply_order(uint8_t* idx, int n);

/* ---- row text (decision 14) ------------------------------------------------------- */

/* "<SPECIES> from <GAME>  <status>", never exceeding 39 bytes + NUL. `species`/`game`
 * are the caller's own decoded strings (the Gen-3 name table / BC_ORIGIN_* name) --
 * this module only formats and truncates. `kind`'s short status word is an internal
 * table (<= 14 columns), independent of the longer PDNA_XRC_* popup/detail strings a
 * later slice adds to pdna_layout.h -- the row line and the detail view are different
 * text. Returns the length written (excluding NUL). */
int xrc_row_text(XrcRowKind kind, const char* species, const char* game, char out[40]);
/* #414: same row, status slot "draft" (a read-only card's orphan <key>.pds.tmp). */
int xrc_row_text_draft(const char* species, const char* game, char out[40]);
/* #414: true for exactly "<16 hex>.pds.tmp" (extension case-blind); *key (may be NULL) = the 16 hex digits. */
bool xrc_draft_name(const char* fname, uint64_t* key);

/* #419: the TRANSFERS walk's per-name decision, I/O-free. is_dir: the entry is a directory. allow_draft: read-only card AND
 * pass 1 (the xfer dir). DRAFT = "<16 hex>.pds.tmp" with allow_draft: fills *name_key (may be NULL) and prim[21] =
 * "<16hex>.pds" (the primary the caller must f_stat; the primary wins). PDS = a 20-char ".pds" name (literal bound 21,
 * extension case-blind). Anything else SKIP. */
typedef enum { XRC_ADMIT_SKIP = 0, XRC_ADMIT_PDS = 1, XRC_ADMIT_DRAFT = 2 } XrcAdmit;
XrcAdmit xrc_walk_admit(const char* fname, bool is_dir, bool allow_draft, uint64_t* name_key, char prim[21]);
/* #415 acceptance for a DRAFT after the full-file read: the record's header (magic/version/count/exact size/crc) must
 * parse AND its header key must equal the NAME's key -- the same two-call predicate as xr_orphan_hdr_ok (xfer_io.c:82),
 * restated here because xfer_io.c is FatFs-bound. */
bool xrc_draft_accept(const uint8_t* buf, uint32_t len, uint64_t name_key);

/* BACKLOG #215(b): a .pds ledger file's identity is its NAME, not any one entry
 * inside it. Two stale-key rows can name the SAME file_idx (two entries inside one
 * physical file, both stale against their own current Gen-3 record); the file only
 * has one name to move to, so only the FIRST row to actually rename it may count --
 * a second row for the same file_idx, run afterward in the same apply pass, would
 * find the old path already gone (the first row's own rename) and, without this
 * guard, wrongly read that as ITS OWN success. `file_rekeyed` is a caller-owned
 * bool array of length `n` (>= every file_idx this pass can see), all-false at the
 * start of the pass. xrc_rekey_should_attempt() says whether a REKEY row should even
 * try; the caller marks a row's own attempt as having genuinely landed with
 * xrc_rekey_mark_done() -- and ONLY on that path, never on a refusal/failure, so a
 * later row for the same file still gets its own real attempt next time (or next
 * visit). `file_idx >= n` never blocks (defensive: the caller's own bound is
 * trusted, this never denies past its knowledge). */
bool xrc_rekey_should_attempt(const bool* file_rekeyed, int n, uint8_t file_idx);
void xrc_rekey_mark_done(bool* file_rekeyed, int n, uint8_t file_idx);

/* ---- the caller's row bookkeeping (decision 5) ----------------------------------- */

/* One row's-worth of bookkeeping for the Bank-open walk / TRANSFERS screen, carried
 * in the caller's GbReconBuf (pdna_main.c) alongside the shipped GbReconHit array --
 * NOT an extension of GbReconHit itself (decision 5: a parallel array). Beyond the
 * classification result (row_kind/actions/kept) this also snapshots exactly the
 * identity fields xrc_bank_match() needs (gen/otid16/dv4/otname/orig8), captured
 * ONCE while the entry's file is already open -- the alternative (re-opening a
 * file per box during the two-phase Bank scan) would turn "page each box once"
 * into "page each box once PER CANDIDATE", which is the stall §11.8/XFER-C14 the
 * cap exists to prevent. */
typedef struct {
  uint8_t file_idx;
  uint8_t entry_idx;
  uint8_t kind, state, direction;      /* XR_KIND_*, XR_STATE_*, XR_DIR_*           */
  int8_t  g3_key_matches;              /* -1 not yet run (ABROAD_GB entries)         */
  int8_t  g3_identity_matches;
  bool    g3_in_daycare;
  int8_t  bank_matches;                /* -1 unresolved until phase 2                */
  int8_t  bank_g3_matches;             /* Gen-3 copies parked in the Bank (phase 2)  */
  int8_t  bank_ident_matches;          /* #385: re-serial Bank matches (D1)      */
  uint64_t file_key;                   /* the ledger file's key (ABROAD_G3 only)     */
  bool    bank_slot_pending;
  bool    bank_keep;
  int8_t  bank_box, bank_slot, g3_box, g3_slot;
  uint8_t row_kind;                    /* XrcRowKind, filled by xrc_classify         */
  uint8_t actions;                     /* the allowed-action mask                    */
  uint8_t action;                      /* the ONE action chosen this visit, 0 = none */
  /* identity snapshot for xrc_bank_match(), captured at the walk: */
  uint8_t gen;
  uint16_t otid16;
  uint8_t dv4[4];
  uint8_t otname[GB_NAME_BYTES];
  uint8_t orig8[8];                    /* original80[0..7]                          */
  uint8_t orig_serial[4];              /* #385: original80[73..76]                  */
} XrcHit;

/* BACKLOG #392(b): the one-pass clone guard for step (1c). Two LOST rows can carry the SAME original (a re-key
 * left a stale twin); RESTOREing both in one pass would write two Bank cells. True when row `i` (a RESTORE) has the
 * same original80[0..7] as an EARLIER RESTORE row j < i whose done[j] is set (restored, or itself skipped as this
 * very duplicate) -- the caller then drops the entry instead of writing a second cell. Rows whose first RESTORE
 * did not land are NOT done, so a failed first restore does not swallow the second. */
bool xrc_restore_is_dup(const XrcHit* hits, const bool* done, int i);

#endif /* XFER_RECONCILE_H */
