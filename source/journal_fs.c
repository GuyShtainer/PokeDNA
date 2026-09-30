/* SPDX-License-Identifier: GPL-3.0-or-later */
/* journal_fs.c -- the FatFs binding of the journal's fs seam. See journal_fs.h. */
#include "journal_fs.h"

#include <string.h>

#include "ff.h"

#define ZBLK 512u   /* zero-fill block: lives on the stack of jfs_create_zero (never ROM: the
                     * flashcart unmaps ROM during a transfer) */

static int jfs_mkdir(void* ctx, const char* path) {
  FRESULT fr;
  (void)ctx;
  fr = f_mkdir(path);
  return (fr == FR_OK || fr == FR_EXIST) ? 0 : -1;
}

static long jfs_size(void* ctx, const char* path) {
  FILINFO fi;
  FRESULT fr;
  (void)ctx;
  fr = f_stat(path, &fi);
  if (fr == FR_OK) return (long)fi.fsize;
  return (fr == FR_NO_FILE || fr == FR_NO_PATH) ? -1L : -2L;   /* absent vs a card error */
}

static int jfs_read(void* ctx, const char* path, uint32_t off, void* buf, uint32_t n) {
  FIL f;
  UINT br = 0;
  int ok;
  (void)ctx;
  if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return -1;
  ok = f_lseek(&f, off) == FR_OK && f.fptr == off && f_read(&f, buf, n, &br) == FR_OK && br == n;
  return (f_close(&f) == FR_OK && ok) ? 0 : -1;
}

/* IN PLACE: opens the existing file, refuses to extend it (the FAT chain must never grow
 * mid-session), writes, syncs. The directory entry it rewrites is byte-identical when the
 * caller holds the timestamp (jrn_fattime_filter). */
static int jfs_write(void* ctx, const char* path, uint32_t off, const void* buf, uint32_t n) {
  FIL f;
  UINT bw = 0;
  int ok;
  (void)ctx;
  if (f_open(&f, path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return -1;
  ok = (uint32_t)f_size(&f) >= off + n && f_lseek(&f, off) == FR_OK && f.fptr == off &&
       f_write(&f, buf, n, &bw) == FR_OK && bw == n && f_sync(&f) == FR_OK;
  return (f_close(&f) == FR_OK && ok) ? 0 : -1;
}

/* Staged first fill (see JrnFs.alloc): extend to `size` by seeking past the end (FatFs allocates the chain,
 * writes no data), zero the header sector, sync. */
static int jfs_alloc(void* ctx, const char* path, uint32_t size, uint32_t headn) {
  FIL f;
  UINT bw = 0;
  uint8_t blk[ZBLK];
  uint32_t done;
  int ok = 1;
  (void)ctx;
  if (headn > ZBLK || headn > size) return -1;
  if (f_open(&f, path, FA_WRITE | FA_OPEN_ALWAYS) != FR_OK) return -1;
  done = (uint32_t)f_size(&f);
  if (done > size) { f_close(&f); return -1; }
  if (done < size) ok = f_lseek(&f, size) == FR_OK && f.fptr == size;   /* disk full: fptr stops short */
  if (ok && headn) {
    memset(blk, 0, sizeof blk);
    ok = f_lseek(&f, 0) == FR_OK && f_write(&f, blk, headn, &bw) == FR_OK && bw == headn;
  }
  ok = ok && f_size(&f) == size && f_sync(&f) == FR_OK;
  return (f_close(&f) == FR_OK && ok) ? 0 : -1;
}

/* One handle: read each 512-byte chunk, overwrite the dirty ones with zeros in place. */
static int jfs_zero(void* ctx, const char* path, uint32_t off, uint32_t n) {
  FIL f;
  UINT br, bw;
  uint8_t blk[ZBLK], z[ZBLK];
  uint32_t pos, m, i;
  int ok = 1;
  (void)ctx;
  memset(z, 0, sizeof z);
  if (f_open(&f, path, FA_READ | FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return -1;
  ok = (uint32_t)f_size(&f) >= off + n && f_lseek(&f, off) == FR_OK && f.fptr == off;
  for (pos = off; ok && pos < off + n; pos += m) {
    m = off + n - pos < ZBLK ? off + n - pos : ZBLK;
    br = 0;
    ok = f_read(&f, blk, m, &br) == FR_OK && br == m;
    for (i = 0; ok && i < m && !blk[i]; i++) {}
    if (!ok || i == m) continue;                                     /* clean chunk: nothing to write */
    bw = 0;
    ok = f_lseek(&f, pos) == FR_OK && f_write(&f, z, m, &bw) == FR_OK && bw == m;
  }
  ok = ok && f_sync(&f) == FR_OK;
  return (f_close(&f) == FR_OK && ok) ? 0 : -1;
}

/* Create-or-complete (see JrnFs.create_zero): a leftover from a cut is finished, not recreated,
 * so the growth path never deletes or renames. The header goes in LAST: until it does (and the
 * final sync sets the size) the file is not a valid segment/redirect. */
static int jfs_create_zero(void* ctx, const char* path, uint32_t size, const void* head, uint32_t headn) {
  FIL f;
  UINT bw;
  uint8_t blk[ZBLK];
  uint32_t done, n;
  int ok = 1;
  (void)ctx;
  if (headn > size || headn > ZBLK) return -1;
  if (f_open(&f, path, FA_WRITE | FA_OPEN_ALWAYS) != FR_OK) return -1;
  done = (uint32_t)f_size(&f);
  if (done > size || f_lseek(&f, done) != FR_OK) { f_close(&f); return -1; }
  memset(blk, 0, sizeof blk);
  while (ok && done < size) {
    n = size - done < ZBLK ? size - done : ZBLK;
    bw = 0;
    ok = f_write(&f, blk, n, &bw) == FR_OK && bw == n;
    done += n;
  }
  if (ok && headn) {
    bw = 0;
    ok = f_lseek(&f, 0) == FR_OK && f_write(&f, head, headn, &bw) == FR_OK && bw == headn;
  }
  ok = ok && f_sync(&f) == FR_OK;
  return (f_close(&f) == FR_OK && ok) ? 0 : -1;
}

static int jfs_unlink(void* ctx, const char* path) {
  FRESULT fr;
  (void)ctx;
  fr = f_unlink(path);
  return (fr == FR_OK || fr == FR_NO_FILE || fr == FR_NO_PATH) ? 0 : -1;
}

static int jfs_list(void* ctx, const char* dir, JrnListFn cb, void* arg) {
  DIR d;
  FILINFO fi;
  uint32_t guard;
  FRESULT fr;
  int ok = 1;
  (void)ctx;
  fr = f_opendir(&d, dir);
  if (fr == FR_NO_PATH || fr == FR_NO_FILE) return 1;      /* absent: a journal never started */
  if (fr != FR_OK) return -1;
  for (guard = 0; guard < 20000u; guard++) {
    if (f_readdir(&d, &fi) != FR_OK) { ok = 0; break; }    /* a read error is an ERROR, not the end of the list */
    if (!fi.fname[0]) break;
    if (!(fi.fattrib & AM_DIR)) cb(arg, fi.fname);
  }
  return (f_closedir(&d) == FR_OK && ok) ? 0 : -1;
}

/* One handle, chunked read (see JrnFs.scan): one aligned sector per f_read (one disk read per sector), the
 * bytes the callback did not consume carried in front of the next sector. blk is 1 KiB on the stack of this
 * call (never ROM: the flashcart unmaps ROM during a transfer). */
static int jfs_scan(void* ctx, const char* path, uint32_t start, uint32_t limit, JrnScanFn cb, void* arg) {
  FIL f;
  UINT br;
  uint8_t blk[2u * ZBLK];
  uint32_t base = start, have = 0, pos = start, n, used, guard;
  int ok = 1, go = 1, last = 0;
  (void)ctx;
  if (!cb || start > limit || start % ZBLK) return -1;
  if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return -1;
  ok = (uint32_t)f_size(&f) >= limit && f_lseek(&f, start) == FR_OK && (uint32_t)f.fptr == start;
  for (guard = 0; ok && go && !last && guard < 0x10000u; guard++) {
    if (pos >= limit) break;
    n = limit - pos < ZBLK ? limit - pos : ZBLK;
    br = 0;
    ok = f_read(&f, blk + have, n, &br) == FR_OK && br == n;
    if (!ok) break;
    pos += n;
    last = pos >= limit;
    used = 0;
    go = cb(arg, base, blk, have + n, last, &used);
    if (!go) break;                                                                 /* cb stopped the scan */
    if (used > have + n || (!last && have + n - used >= ZBLK)) { ok = 0; break; }   /* a callback that stalls is a bug */
    memmove(blk, blk + used, have + n - used);
    have = have + n - used;
    base += used;
  }
  return (f_close(&f) == FR_OK && ok) ? 0 : -1;
}

static uint32_t jfs_stamp(void* ctx, const char* path) {
  FILINFO fi;
  (void)ctx;
  if (f_stat(path, &fi) != FR_OK) return 0;
  return ((uint32_t)fi.fdate << 16) | fi.ftime;
}

const JrnFs jrn_fatfs = {
  0, jfs_mkdir, jfs_size, jfs_read, jfs_write, jfs_alloc, jfs_zero, jfs_create_zero,
  jfs_unlink, jfs_list, jfs_scan, jfs_stamp
};
