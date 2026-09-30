/* SPDX-License-Identifier: GPL-3.0-or-later */
/* journal_fs.h -- binds the journal's fs seam (journal.h JrnFs) to the real FatFs.
 * Nothing calls it yet (slice 2). The host power-cut sweep compiles it over the RAM disk. */
#ifndef JOURNAL_FS_H
#define JOURNAL_FS_H

#include "journal.h"

/* The FatFs-backed table. Every call opens, works and closes: no handle outlives it (a
 * power cut or a card swap can never strand one), and FIL lives on the stack of the call. */
extern const JrnFs jrn_fatfs;

#endif /* JOURNAL_FS_H */
