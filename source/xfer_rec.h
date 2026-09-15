#ifndef XFER_REC_H
#define XFER_REC_H

#include <stdint.h>

/* BACKLOG #150 S150-6, decision 9/D-Q4: the PURE half of the transfer ledger --
 * no FatFs, no tonc/sys.h, so tests/host_xferrt_test.c (S150-9's merge core,
 * xr_merge_down) and every future test that wants xr_key_g3 alone can link this
 * file without dragging lib/fatfs/ff.c in. source/xfer_io.c is the FatFs layer
 * (path resolution, the one reader, the one-time migration).
 *
 * This slice adds ONLY xr_key_g3 here -- §11.12 puts xr_merge_down/xr_open in the
 * same file too, but S150-8b/S150-9 own those; splitting keeps this file's own
 * scope exactly what this slice needs. */

/* FNV-1a-64 over rec80[0..7] -- the SAME offset basis/prime gbsc_key() uses
 * (source/gb_sidecar.c/.h), applied to a DIFFERENT 8 bytes (the Gen-3 record's own
 * PID+OTID span, bytes 0..7 of the 80-byte box-shape record) so a reroll (which
 * rewrites the PID, source/gen3_edit.c's em_set_pid/em_reroll) changes this key --
 * exactly the property the reroll re-key guard (decision 8) needs. */
uint64_t xr_key_g3(const uint8_t rec80[80]);

#endif
