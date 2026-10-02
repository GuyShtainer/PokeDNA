/* SPDX-License-Identifier: GPL-3.0-or-later
 * bank_layout.h -- BACKLOG #371: the pure (FatFs + savefile only, no tonc) decisions that say
 * whether a Bank already exists on the card and where its bank.meta bytes come from.
 * pdna_bank.c cannot link on the host; this can, so tests/host_bank_layout_test.c exercises
 * the REAL decisions over a RAM disk. */
#ifndef BANK_LAYOUT_H
#define BANK_LAYOUT_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BML_PRIMARY = 0,         /* primary parsed clean                                         */
  BML_BAK_PRIMARY_ABSENT,  /* primary missing, bank.meta.bak parsed -> restore from it     */
  BML_BAK_PRIMARY_BAD,     /* primary present but unreadable/corrupt, .bak parsed          */
  BML_NONE_BOXES,          /* no usable meta, but box files exist: an existing Bank        */
  BML_NONE_EMPTY           /* no usable meta and no box file: a card with no Bank at all   */
} BmlSource;

/* True when a Bank is already on the card: bank.meta OR bank.meta.bak OR any boxNN.box.
 * A leftover bank.meta.tmp alone does NOT count (never trusted as data). `dir` is the
 * Bank folder without a trailing slash; `nboxes` <= 99. */
bool bml_layout_exists(const char* dir, int nboxes);

/* Read bank.meta (else bank.meta.bak) into buf[cap]; a file is usable when it is at least
 * `need` bytes and starts with the 6-byte `magic`. Never writes the card. When nothing is
 * usable returns BML_NONE_BOXES / BML_NONE_EMPTY (buf is then unspecified). */
BmlSource bml_meta_read(const char* dir, int nboxes, uint8_t* buf, uint32_t cap,
                        uint32_t need, const char* magic);
#endif
