/* SPDX-License-Identifier: GPL-3.0-or-later
 * xfer_heal.c -- see xfer_heal.h (BACKLOG #389). */
#include "xfer_heal.h"
#include "gb_sidecar.h"
#include "savefile.h"
#include "log.h"
#include "ff.h"
#include <string.h>

#define XH_SCAN_MAX  256   /* directory entries looked at per scan (golden rule 2)  */
#define XH_CAND_MAX  16    /* orphan keys collected per scan                         */
#define XH_TMP_LEN   24    /* "0123456789ABCDEF.pds.tmp"                             */

static bool is_orphan_name(const char* n) {
  size_t L = strlen(n);
  if (L != XH_TMP_LEN) return false;
  for (int i = 0; i < 16; i++) {
    char c = n[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
  }
  const char* e = n + 16;
  return e[0] == '.' && (e[1] | 32) == 'p' && (e[2] | 32) == 'd' && (e[3] | 32) == 's' &&
         e[4] == '.' && (e[5] | 32) == 't' && (e[6] | 32) == 'm' && (e[7] | 32) == 'p';
}

static bool key_from_name(const char* n, uint64_t* key) {
  char p[24];
  memcpy(p, n, 20); p[20] = 0;          /* "<16hex>.pds" */
  return gbsc_key_from_path(p, key);
}

XhResult xh_heal_key(const char* dir, uint64_t key, uint8_t* scratch, uint32_t cap, bool can_edit) {
  if (!dir || !scratch || cap < GBSC_FILE_MAX) return XH_FAILED;
  char path[GBSC_PATH_MAX], tmp[GBSC_PATH_MAX + 8];
  if (gbsc_path(path, (int)sizeof path, dir, key) < 0) return XH_FAILED;
  memcpy(tmp, path, strlen(path) + 1);
  strcat(tmp, ".tmp");

  FILINFO fi;
  FRESULT fr = f_stat(path, &fi);
  if (fr == FR_OK) return XH_NONE;                       /* the primary wins; a stale .tmp is not ours to touch */
  if (fr != FR_NO_FILE && fr != FR_NO_PATH) return XH_FAILED;   /* a card fault is not "absent" */
  fr = f_stat(tmp, &fi);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) return XH_NONE;
  if (fr != FR_OK) return XH_FAILED;

  uint32_t len = 0;
  if (sf_read_full(tmp, scratch, cap, &len) != SF_OK) return XH_FAILED;
  if (len != (uint32_t)fi.fsize) return XH_BAD_TMP;      /* longer than the buffer / torn size */
  if (gbsc_count(scratch, len) < 0 || gbsc_file_key(scratch, len) != key) {
    log_line("xfer: %s.tmp is not a valid ledger for its key, left alone", path);
    return XH_BAD_TMP;
  }
  if (!can_edit) {
    log_line("xfer: %s primary missing, verified .tmp found; card read-only, not restored", path);
    return XH_RO_TMP;
  }
  if (f_rename(tmp, path) != FR_OK) {
    log_line("xfer: %s primary missing, restore from .tmp FAILED (rename)", path);
    return XH_FAILED;
  }
  /* f_rename returning FR_OK is not evidence the card kept it: read the primary back. */
  if (sf_where_are_the_bytes(path, scratch, len) != SF_WHERE_TARGET) {
    log_line("xfer: %s restore from .tmp not confirmed on the card", path);
    return XH_FAILED;
  }
  log_line("xfer: %s primary missing, restored from .tmp", path);
  return XH_RESTORED;
}

int __attribute__((noinline)) xh_heal_dir(const char* dir, uint8_t* scratch, uint32_t cap, bool can_edit, int* n_ro) {
  if (n_ro) *n_ro = 0;
  if (!dir || !scratch || cap < GBSC_FILE_MAX) return 0;
  uint64_t cand[XH_CAND_MAX];
  int nc = 0;
  {
    DIR d; FILINFO fi;
    if (f_opendir(&d, dir) != FR_OK) return 0;
    for (int seen = 0; seen < XH_SCAN_MAX && nc < XH_CAND_MAX; seen++) {
      if (f_readdir(&d, &fi) != FR_OK || !fi.fname[0]) break;
      uint64_t k;
      if ((fi.fattrib & AM_DIR) || !is_orphan_name(fi.fname) || !key_from_name(fi.fname, &k)) continue;
      cand[nc++] = k;
    }
    f_closedir(&d);                                      /* healed AFTER the scan: no rename mid-iteration */
  }
  int restored = 0;
  for (int i = 0; i < nc; i++) {
    XhResult r = xh_heal_key(dir, cand[i], scratch, cap, can_edit);
    if (r == XH_RESTORED) restored++;
    else if (r == XH_RO_TMP && n_ro) (*n_ro)++;
  }
  return restored;
}

/* #389 review: delete a ledger -- its stale .tmp FIRST, so a later heal can never resurrect it. */
FRESULT xh_unlink_ledger(const char* path) {
  char tmp[GBSC_PATH_MAX + 8];
  size_t n = strlen(path);
  if (n + 5 > sizeof tmp) return FR_INVALID_NAME;
  memcpy(tmp, path, n);
  memcpy(tmp + n, ".tmp", 5);
  FRESULT tr = f_unlink(tmp);
  if (tr != FR_OK && tr != FR_NO_FILE) return tr;
  return f_unlink(path);
}

/* #394: a BAD <path>.tmp is moved to <path>.tmp.bad (an older .tmp.bad is replaced) before the fresh ledger is
 * started -- the same set-aside the primary-corruption path does -- so a salvageable torn write is kept, not
 * silently overwritten. false = the rename failed (the bytes are still at .tmp): the caller refuses. */
static bool xh_set_aside_tmp(const char* path) {
  char tmp[GBSC_PATH_MAX + 8], bad[GBSC_PATH_MAX + 12];
  size_t n = strlen(path);
  if (n + 9 > sizeof bad) return false;
  memcpy(tmp, path, n); memcpy(tmp + n, ".tmp", 5);
  memcpy(bad, path, n); memcpy(bad + n, ".tmp.bad", 9);
  (void)f_unlink(bad);                                   /* an older set-aside: replaced (FR_NO_FILE is the normal case) */
  FRESULT rr = f_rename(tmp, bad);
  log_line("xfer_heal: %s bad .tmp set aside as .tmp.bad (%s)", path, rr == FR_OK ? "OK" : "FAILED");
  return rr == FR_OK;
}

/* #389 review D4: the writers' "ledger read said absent" branch. XH_NONE -> a fresh ledger in buf; XH_RESTORED
 * -> the healed ledger read back into buf; XH_BAD_TMP -> a fresh ledger too (a bad .tmp holds no verified bytes,
 * so the caller's next verified write replaces it; refusing would lock the mon's transfers forever after a
 * torn FIRST write); anything else (read-only .tmp, card fault, a restored file that does not parse) -> refuse. */
bool xh_absent_resolve(const char* dir, const char* path, uint64_t key, uint8_t* buf, uint32_t cap,
                       bool can_edit, uint32_t* len) {
  if (!dir || !path || !buf || !len || cap < GBSC_FILE_MAX) return false;
  XhResult hr = xh_heal_key(dir, key, buf, cap, can_edit);
  /* SF_ERR_OPEN is ANY failed f_open, not "absent": a present primary the caller could not open is refused,
   * never replaced by a fresh ledger (re-verify-za2b F1; the loss predates za2). */
  if (hr == XH_NONE) { FRESULT ps = f_stat(path, 0); if (ps != FR_NO_FILE && ps != FR_NO_PATH) return false; }
  /* An unparseable .tmp is torn scratch from a pull mid-write (savefile only keeps a byte-compared .tmp past
   * the write itself); the caller's next verified write replaces it. */
  if (hr == XH_BAD_TMP && !xh_set_aside_tmp(path)) return false;   /* #394: keep the bad bytes, refuse if they cannot be moved */
  if (hr == XH_NONE || hr == XH_BAD_TMP) { *len = (uint32_t)gbsc_init(buf, key); return true; }
  if (hr == XH_RESTORED) {
    log_line("xfer_heal: %s healed from its .tmp mid-session", path);
    if (sf_read_full(path, buf, cap, len) == SF_OK && gbsc_count(buf, *len) >= 0 &&
        gbsc_file_key(buf, *len) == key) return true;
  }
  return false;
}
