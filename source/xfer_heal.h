/* SPDX-License-Identifier: GPL-3.0-or-later
 * xfer_heal.h -- BACKLOG #389: heal a transfer-ledger file (<key>.pds) whose verified-write swap
 * was interrupted. sf_write_verified unlinks the primary and then renames <key>.pds.tmp over it;
 * a card pull (or a swallowed rename) in between leaves ONLY the .tmp, which every directory scan
 * (the scans match "*.pds") never saw -- the record and the Game Boy original it holds vanished
 * from every screen. The pure FatFs half (no tonc / sys.h), host-tested over a RAM disk by
 * tests/host_xfer_heal_test.c, same family as bank_layout.{c,h} (#371/#378). */
#ifndef XFER_HEAL_H
#define XFER_HEAL_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  XH_NONE = 0,    /* nothing to do: the primary exists (a stale .tmp is never touched) or no .tmp  */
  XH_RESTORED,    /* primary absent + verified .tmp -> renamed to <key>.pds, read back             */
  XH_RO_TMP,      /* a usable orphan .tmp exists but the card is read-only: nothing written        */
  XH_BAD_TMP,     /* primary absent, .tmp present but not a valid ledger for this key: left alone   */
  XH_FAILED       /* a card fault (stat/read/rename): nothing decided, nothing destroyed            */
} XhResult;

/* Heal ONE key in `dir` (no trailing slash). `scratch`/`cap` (cap >= GBSC_FILE_MAX) holds the .tmp bytes.
 * Trust rule: the .tmp is used only when <key>.pds is ABSENT (FR_NO_FILE/FR_NO_PATH) AND the .tmp passes
 * gbsc_count (magic, version, header crc16, every entry crc16, exact length) AND its embedded key equals
 * `key` AND its file size equals the bytes read. The heal is a RENAME (never a rewrite: the .tmp is the
 * only copy of the newest bytes), then the card is read back. Call with can_edit == app_can_edit(). */
XhResult xh_heal_key(const char* dir, uint64_t key, uint8_t* scratch, uint32_t cap, bool can_edit);

/* Scan `dir` for "<16 hex>.pds.tmp" names (bounded: 256 entries looked at, 16 candidates healed per call;
 * the rest heal on the next call) and xh_heal_key() each. Returns the number of keys RESTORED. A missing
 * dir is 0. `*n_ro` (may be NULL) receives how many usable orphans were left because can_edit is false. */
int xh_heal_dir(const char* dir, uint8_t* scratch, uint32_t cap, bool can_edit, int* n_ro);
#endif
