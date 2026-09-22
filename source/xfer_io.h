#ifndef XFER_IO_H
#define XFER_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_sidecar.h"   /* GBSC_PATH_MAX                                          */
#include "savefile.h"     /* SfStatus                                               */

/* BACKLOG #150 S150-6, decision 9/D-Q4: the FatFs half of the transfer ledger. May
 * include ff.h/savefile.h/log.h; never tonc.h/sys.h (source/xfer_rec.h is the pure
 * half). */

/* BACKLOG #213: a tiny fixed-size ring of "this key resolved to NEITHER folder"
 * results -- the GB ORIGINAL row's own menu-open cost (three f_stat on a miss,
 * source/pdna_main.c's app_mon_menu -> xv_has_original -> xr_path_for_key) becomes
 * 0 f_stat on a repeat A over the same ordinary mon in one visit. Storage is
 * caller-owned (xfer_io.c/h never include tonc.h/sys.h -- this file's own note
 * above -- so the EWRAM_BSS array itself lives in the caller, pdna_main.c); these
 * three functions are pure logic over that storage, with no FatFs/EWRAM
 * involvement at all, so they host-test directly (tests/host_xferio_test.c).
 * Correctness invariant: the CALLER must call xr_miss_cache_reset() after every
 * ledger write -- these functions cannot enforce that themselves, they only give
 * the caller a cheap, correct place to remember and check misses. */
#define XR_MISS_RING_N 8
typedef struct {
  uint64_t keys[XR_MISS_RING_N];
  uint8_t  n;      /* valid ring entries, 0..XR_MISS_RING_N */
  uint8_t  next;   /* next ring slot to overwrite            */
} XrMissCache;

void xr_miss_cache_reset(XrMissCache* c);
bool xr_miss_cache_has(const XrMissCache* c, uint64_t key);
void xr_miss_cache_remember(XrMissCache* c, uint64_t key);

/* review F3: true once "/PokeDNA/xfer/MIGRATED" exists -- the migration has
 * actually completed, so /PokeDNA/xfer is now THE ledger and every sidecar source
 * is an inert backup. xr_path_for_key/xr_path_for_name's sidecar fallback (and
 * gb_reconcile_on_load's pass 2, source/pdna_main.c) are both gated on this being
 * false. */
bool xr_migrated(void);

/* Decision 4/D-Q7 -- ONE path per key, for reads AND in-place writes of an EXISTING
 * key: the /PokeDNA/xfer path, unless that file is absent AND the migration has
 * NOT completed (xr_migrated() false) AND the /PokeDNA/sidecar one exists, in
 * which case the sidecar path (review F3: post-migration the sidecar fallback is
 * gated off -- see xr_migrated()'s own comment). Writes out[GBSC_PATH_MAX] either
 * way. Returns true iff a file exists at the path it wrote; when neither exists
 * (or migration already ran) it still writes the xfer path (where a brand-new
 * file belongs, decision 4/D-Q7) and returns false. False (with `out` untouched)
 * only on a bad argument or a path that would not fit GBSC_PATH_MAX. */
bool xr_path_for_key(char out[GBSC_PATH_MAX], uint64_t key);

/* BACKLOG #213: same contract as xr_path_for_key, plus one caller-supplied hint --
 * when `xfer_dir_absent` is true the caller already knows /PokeDNA/xfer itself does
 * not exist (its own f_stat, xr_dir_exists() below), so BOTH the xfer-file f_stat
 * AND the MIGRATED-marker f_stat are skipped (a file and a marker cannot exist
 * inside a directory that does not exist) -- only the sidecar fallback's own f_stat
 * still runs. `false` reproduces xr_path_for_key exactly; xr_path_for_key is this
 * function with the hint hardwired false, so every existing caller is unaffected. */
bool xr_path_for_key_hint(char out[GBSC_PATH_MAX], uint64_t key, bool xfer_dir_absent);

/* BACKLOG #213: one f_stat(PDNA_XFER_DIR), no cached state (xfer_io.h/.c never
 * include tonc.h/sys.h -- see the note at the top of this file -- so an EWRAM_BSS
 * per-session latch built on top of this lives in the caller, e.g. pdna_main.c). */
bool xr_dir_exists(void);

/* Same rule, applied to an on-card FILENAME (e.g. "0019A3F17C0B44E2.pds") instead of
 * a key -- gb_recon_path()'s replacement, so a reconcile hit's later claim write
 * lands in the same file it was read from. */
bool xr_path_for_name(char out[GBSC_PATH_MAX], const char* name);

/* Resolve `key`'s path (xr_path_for_key) and sf_read_full() it into buf(cap).
 * *len is set on SF_OK. `path_out` (>= GBSC_PATH_MAX, or NULL) receives the
 * resolved path either way, so a caller that wants to write back in place has it
 * without re-resolving. SF_ERR_OPEN when neither folder holds a file for this key. */
SfStatus xr_open(uint64_t key, uint8_t* buf, uint32_t cap, uint32_t* len, char* path_out);

/* Decision 5 -- ONE-TIME migration: copies every "/PokeDNA/sidecar/<16hex>.pds" whose
 * "/PokeDNA/xfer" twin is absent or byte-different into xfer under the SAME filename
 * (sf_read_full -> sf_write_verified, decision 5's own reasoning for why not sf_copy).
 * Sources are never renamed or deleted. Writes "/PokeDNA/xfer/MIGRATED" (the copied
 * count, ASCII) LAST, only after every file that needed copying copied cleanly --
 * its presence makes the next call an O(1) f_stat. `scratch`/`cap`: a caller-supplied
 * buffer, cap >= GBSC_FILE_MAX, for the read/write cycle (this function never holds
 * its own static buffer -- NO new statics).
 *
 * Returns the number of files copied, 0 if there was nothing to do (marker already
 * present, or /PokeDNA/sidecar absent -- a fresh marker with count 0 is written in
 * that second case), or -1 on a refusal (app_can_edit() false) or a copy failure
 * (no marker is written on failure, so the next call retries from scratch -- a
 * partially migrated card is always safe because decision 4/D-Q7's fallback still
 * finds every un-migrated record in sidecar). */
int xr_migrate_once(uint8_t* scratch, uint32_t cap);

#endif
