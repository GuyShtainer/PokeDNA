/* SPDX-License-Identifier: GPL-3.0-or-later */
/* journal_fs.h -- binds the journal's fs seam (journal.h JrnFs) to the real FatFs.
 * Nothing calls it yet (slice 2). The host power-cut sweep compiles it over the RAM disk. */
#ifndef JOURNAL_FS_H
#define JOURNAL_FS_H

#include "journal.h"

/* The FatFs-backed table. Every call opens, works and closes: no handle outlives it (a
 * power cut or a card swap can never strand one), and FIL lives on the stack of the call. */
extern const JrnFs jrn_fatfs;

/* Settings > Clear history (slice 4): remove <root>/<key>/NNNN.pdj for every ring slot (1..JRN_RING_MAX) and then the
 * directory itself. NOT part of the JrnFs seam on purpose: a journal can never remove a file, only an explicit user action
 * (a destructive confirm, Omega-only) may. Returns the slot files removed (>= 0; an absent directory is 0) or JRN_E_IO on
 * a card error other than "absent". A directory that cannot be removed (a stray file) is not an error: with no slot file the
 * journal reads as never started. */
int jrnfs_clear_key(const char* root, uint64_t key);

#endif /* JOURNAL_FS_H */
