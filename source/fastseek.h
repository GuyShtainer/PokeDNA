#ifndef FASTSEEK_H
#define FASTSEEK_H

#include "ff.h"

/*
 * fastseek.c — the ONE place FatFs' per-FIL cluster link map (`FIL.cltbl`) is ever
 * assigned in this program.
 *
 * WHAT IT BUYS. With FF_USE_FASTSEEK 1 (lib/fatfs/ffconf.h) a FIL that has opted in
 * resolves every f_lseek through an in-RAM table of {length, start-cluster} fragment
 * pairs (ff.c clmt_clust) instead of walking the FAT one cluster at a time. The
 * measured saving on this project's hot path: a 21-cell Pokedex page off a registered
 * 12.5 MB .gba went 369 -> 126 disk_read sectors on the host FatFs harness, because
 * the icon-table pointers scatter across the whole image and a normal seek RESTARTS
 * from cluster 0 on every backward jump (ff.c f_lseek's `ifptr > 0` test fails, so it
 * takes the restart branch). Forward seeks were already incremental and are unchanged.
 *
 * IT IS OPT-IN, PER HANDLE, AND THAT IS WHY FLIPPING THE GLOBAL SWITCH IS SAFE.
 * f_open sets fp->cltbl = 0 unconditionally on EVERY successful open (ff.c, inside the
 * `res == FR_OK` block, after the FF_FS_READONLY split rejoins, before fp->obj.fs is
 * even set) -- so an uninitialised stack FIL can never carry a garbage table into
 * FatFs, and no memset of a FIL local is needed anywhere. f_read, f_write and f_lseek
 * each test `if (fp->cltbl)` and otherwise run the FASTSEEK-0 code verbatim. A handle
 * that never calls this function therefore behaves bit-identically to before the flip.
 * THE SAVE WRITE PATH IS UNAFFECTED: savefile.c sets no cltbl and never will (see
 * rule 1 below, which is enforced here rather than by convention).
 *
 * The only global cost of the flip is sizeof(FIL) 592 -> 600 (+8, not +4: FSIZE_t is
 * 8 bytes with exFAT on, so _Alignof(FIL) is 8 and the 4-byte pointer pads out) and
 * offsetof(FIL, buf) 80 -> 84 -- still 4-aligned, so lib/fatfs/diskio.c's DMA32 fast
 * path is untouched. That was checked, not assumed; it is the one thing here that
 * would have failed silently.
 *
 * FOUR RULES, each closing a real hazard. Rules 1 and 2 are enforced by the code
 * below; 3 and 4 are the caller's.
 *
 *   1. READ-ONLY HANDLES ONLY -- tested here, on fp->flag, not left to call sites.
 *      With a cltbl set, f_write calls clmt_clust INSTEAD of create_chain: past the
 *      mapped range that is a silent short write, and against a STALE map it writes
 *      into ANOTHER FILE'S CLUSTERS. f_truncate and f_expand never look at cltbl at
 *      all and would silently strand one. All three require FA_WRITE, so the single
 *      test below covers every one of them.
 *
 *   2. NULL cltbl ON ANY FAILURE -- done here. This is the subtle one. On
 *      FR_NOT_ENOUGH_CORE, FatFs leaves cltbl NON-NULL and the table UNTERMINATED.
 *      clmt_clust's only stop condition is `ncl == 0`, so the next f_read walks off
 *      the end of the array and returns a garbage cluster; f_read's `clst < 2` guard
 *      catches only SMALL garbage, and any value >= 2 becomes a real sector address --
 *      it reads the WRONG SECTORS and returns FR_OK. That is the only way this feature
 *      can silently show wrong data, and FatFs does not close it for you.
 *
 *   3. THE TABLE MUST BE EWRAM_BSS, never a stack local. Zero-init turns a missed
 *      rule 2 into a clean FR_INT_ERR instead of an out-of-bounds walk, and it keeps
 *      520-byte arrays off the ~10.8 KB IWRAM stack.
 *
 *   4. ONE TABLE PER FIL, REBUILT ON EVERY REOPEN, NEVER SHARED between two live
 *      handles -- a table is valid only for the chain it was built from. A disk error
 *      mid-build goes through FatFs' ABORT, which latches fp->err permanently: close
 *      and reopen, never retry on the same handle.
 *
 * FREE TELEMETRY. FatFs writes the REQUIRED table size back into tbl[0] before it
 * checks whether yours is big enough, so `fragments = (tbl[0] - 2) / 2` is valid even
 * on FR_NOT_ENOUGH_CORE. That number comes from walking the real FAT chain -- the same
 * data the EZ-Flash kernel's own run-table builder consumes -- so a ROM reporting
 * ~61+ fragments on a card where SD-load fails is direct evidence for the run-table
 * overrun hypothesis in ../rom-load-lab/README.md, from an ordinary PokeDNA run with
 * no separate diagnostic image. It is logged on both outcomes.
 */

/* Arm `fp` for fast seek using `tbl` (which MUST be EWRAM_BSS, zero-initialised, and
 * owned solely by this handle), sized `items` DWORDs -- 2 + 2*fragments are needed, so
 * `items` covers (items - 2) / 2 fragments. `what` names the file in the log line.
 *
 * Returns the fragment count on success and 0 on any refusal or failure, having left
 * fp->cltbl NULL so the handle falls back to ordinary seeks -- which is always correct,
 * just slower. Logs exactly one line either way. */
int fastseek_arm(FIL* fp, DWORD* tbl, DWORD items, const char* what);

/* Bytes of table for F fragments, for sizing a static array with intent. */
#define FASTSEEK_ITEMS_FOR(frags) (2u + 2u * (unsigned)(frags))

#endif /* FASTSEEK_H */
