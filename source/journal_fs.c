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
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) return -1L;   /* absent */
  if (fr == FR_INT_ERR) return -3L;                        /* the directory structure is damaged (a torn exFAT entry set): PERSISTENT */
  return -2L;                                              /* a card error (FR_DISK_ERR ...): possibly transient */
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

/* The two callbacks the engine hands the seam (JrnListFn / JrnScanFn) are called through a file-local one-field struct by a
 * noipa wrapper each: a struct-field dispatch the cartridge stack walker can name (tools/stack_edges.txt `JfsListCb.cb @0`,
 * `JfsScanCb.cb @0`), where a bare callback parameter would be an unresolvable register call. */
#if defined(__GNUC__) && !defined(__clang__)
#define JRN_NOIPA __attribute__((noinline, noipa))
#else
#define JRN_NOIPA __attribute__((noinline))
#endif
typedef struct { JrnListFn cb; } JfsListCb;
typedef struct { JrnScanFn cb; } JfsScanCb;
static void JRN_NOIPA jfs_emit_name(const JfsListCb* c, void* arg, const char* name) { c->cb(arg, name); }
static int JRN_NOIPA jfs_emit_scan(const JfsScanCb* c, void* arg, uint32_t off, const uint8_t* b,
                                                          uint32_t n, int last, uint32_t* used) {
  return c->cb(arg, off, b, n, last, used);
}

static int jfs_list(void* ctx, const char* dir, JrnListFn cb, void* arg) {
  JfsListCb lc;
  lc.cb = cb;
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
    if (!(fi.fattrib & AM_DIR)) jfs_emit_name(&lc, arg, fi.fname);
  }
  return (f_closedir(&d) == FR_OK && ok) ? 0 : -1;
}

/* One handle, chunked read (see JrnFs.scan): one aligned sector per f_read (one disk read per sector), the
 * bytes the callback did not consume carried in front of the next sector. blk is 1 KiB on the stack of this
 * call (never ROM: the flashcart unmaps ROM during a transfer). */
static int jfs_scan(void* ctx, const char* path, uint32_t start, uint32_t limit, JrnScanFn cb, void* arg) {
  JfsScanCb sc;
  FIL f;
  UINT br;
  uint8_t blk[2u * ZBLK];
  uint32_t base = start, have = 0, pos = start, n, used, guard;
  int ok = 1, go = 1, last = 0;
  (void)ctx;
  sc.cb = cb;
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
    go = jfs_emit_scan(&sc, arg, base, blk, have + n, last, &used);
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
  jfs_list, jfs_scan, jfs_stamp
};

/* ---- Settings > Clear history (slice 4): the ONE place this module removes files ------------------------------------ */
int jrnfs_clear_key(const char* root, uint64_t key) {
  char dir[JRN_PATH_MAX], p[JRN_PATH_MAX + 12], hex[17];
  uint32_t n = 0, i;
  unsigned s;
  int removed = 0;
  FRESULT fr;
  if (!root) return JRN_E_ARG;
  jrn_key_hex(key, hex);
  while (root[n] && n < JRN_PATH_MAX - 18u) { dir[n] = root[n]; n++; }
  if (root[n]) return JRN_E_ARG;                                  /* the root does not fit: never a truncated path */
  dir[n++] = '/';
  for (i = 0; i < 16u; i++) dir[n++] = hex[i];
  dir[n] = 0;
  for (s = 1; s <= JRN_RING_MAX; s++) {                           /* bounded: the ring has at most JRN_RING_MAX slot files */
    memcpy(p, dir, n);
    p[n] = '/';
    p[n + 1] = (char)('0' + (s / 1000u) % 10u); p[n + 2] = (char)('0' + (s / 100u) % 10u);
    p[n + 3] = (char)('0' + (s / 10u) % 10u);   p[n + 4] = (char)('0' + s % 10u);
    memcpy(p + n + 5, ".pdj", 5);                                 /* incl. the NUL */
    fr = f_unlink(p);
    if (fr == FR_OK) removed++;
    else if (fr != FR_NO_FILE && fr != FR_NO_PATH) return JRN_E_IO;   /* a card error: stop, the journal is whatever it still is */
  }
  (void)f_unlink(dir);                                            /* best effort: a stray file keeps it, and that is harmless */
  return removed;
}
