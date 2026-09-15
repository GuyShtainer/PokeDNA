#ifndef XFER_IO_H
#define XFER_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_sidecar.h"   /* GBSC_PATH_MAX                                          */
#include "savefile.h"     /* SfStatus                                               */

/* BACKLOG #150 S150-6, decision 9/D-Q4: the FatFs half of the transfer ledger. May
 * include ff.h/savefile.h/log.h; never tonc.h/sys.h (source/xfer_rec.h is the pure
 * half). */

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
