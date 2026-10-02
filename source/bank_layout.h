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

/* ---- BACKLOG #378: a Bank BOX file has the same interrupted-swap window as bank.meta ---- */
typedef enum {
  BML_BOX_PRIMARY = 0,  /* boxNN.box read in full                                          */
  BML_BOX_TMP,          /* primary ABSENT, boxNN.box.tmp (a verified write's scratch) usable */
  BML_BOX_BAK,          /* primary ABSENT, no usable .tmp, boxNN.box.bak usable              */
  BML_BOX_NONE,         /* primary absent and neither copy usable: a genuinely new box      */
  BML_BOX_BAD,          /* primary PRESENT but short (buf holds what was read): as before   */
  BML_BOX_READ_ERROR    /* a stat/open/read ERROR on a present file or an unexpected stat
                         * result: NOT "absent" -- never fall back past it, never heal      */
} BmlBoxSrc;

/* Classify box file `path` ("<dir>/boxNN.box") and load its bytes into buf[need] (cap == need).
 * A copy is usable when it is at least `need` bytes (box files carry no magic, so length is the
 * only check box_load ever applied). The fallbacks (".tmp" then ".bak") are consulted ONLY when
 * the primary is absent (FR_NO_FILE / FR_NO_PATH): savefile.c keeps a ".tmp" only after the
 * byte-compare passed (sf_write_verified unlinks it on a verify fail) and unlinks the primary
 * only after that, so a usable ".tmp" is the newest verified copy and ".bak" the one before.
 * Never writes the card. On NONE buf is zeroed; `*out_sz` is the bytes read from the chosen file. */
BmlBoxSrc bml_box_read(const char* path, uint8_t* buf, uint32_t need, uint32_t* out_sz);

/* Rewrite the absent primary from a TMP / BAK recovery. TMP: rename the verified ".tmp" into
 * place (no data rewrite, the only copy of the newest bytes is never truncated) and read the
 * swap back. BAK: sf_write_verified the recovered bytes (the ".bak" itself is never touched: no
 * backup roll happens because the primary is absent). True only when the primary now exists at
 * `len` bytes. Call only when app_can_edit(). */
bool bml_box_heal(const char* path, BmlBoxSrc src, const uint8_t* bytes, uint32_t len);
#endif
