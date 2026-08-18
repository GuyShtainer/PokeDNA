/* Host shim for lib/fatfs/ff.h — stdio-backed, exposing exactly what source/log.c
 * uses, so log.c compiles UNCHANGED on the Mac and its append/watermark accounting
 * can be unit-tested off-hardware.
 *
 * Fault injection: hostff_fail_open / hostff_fail_writes / hostff_fail_close.
 * f_close matters most: on real FatFs a sub-512-byte append NEVER fails at
 * f_write (the bytes land in fp->buf and FR_OK comes back without touching the
 * card) — the disk_write happens in f_close -> f_sync. So f_close failure is the
 * only hardware-reachable flush failure, and it is the branch that protects the
 * log from growing a hole.
 */
#ifndef FF_H
#define FF_H

#include <stdio.h>
#include <string.h>

typedef unsigned int UINT;
typedef enum { FR_OK = 0, FR_DISK_ERR = 1, FR_NO_FILE = 4, FR_DENIED = 7 } FRESULT;
typedef struct { FILE* fp; unsigned long sz; } FIL;

#define FA_READ         0x01
#define FA_WRITE        0x02
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_APPEND  0x30

extern int hostff_fail_open;
extern int hostff_fail_writes;
extern int hostff_fail_close;
extern int hostff_opens;                 /* count of successful f_open calls */

static inline FRESULT f_open(FIL* f, const char* p, unsigned char m) {
  (void)m;
  if (hostff_fail_open) return FR_DISK_ERR;
  f->fp = fopen(p, "ab");
  if (!f->fp) return FR_DISK_ERR;
  fseek(f->fp, 0, SEEK_END);
  f->sz = (unsigned long)ftell(f->fp);
  hostff_opens++;
  return FR_OK;
}
static inline FRESULT f_write(FIL* f, const void* b, UINT n, UINT* bw) {
  if (hostff_fail_writes) { *bw = 0; return FR_DISK_ERR; }
  *bw = (UINT)fwrite(b, 1, n, f->fp);
  return (*bw == n) ? FR_OK : FR_DISK_ERR;
}
static inline FRESULT f_close(FIL* f) {
  int r = fclose(f->fp);
  f->fp = 0;
  if (hostff_fail_close) return FR_DISK_ERR;
  return r ? FR_DISK_ERR : FR_OK;
}
static inline unsigned long hostff_size(FIL* f) { return f->sz; }
#define f_size(fp) hostff_size(fp)

static inline FRESULT f_unlink(const char* p) { return remove(p) ? FR_NO_FILE : FR_OK; }
static inline FRESULT f_rename(const char* a, const char* b) {
  return rename(a, b) ? FR_NO_FILE : FR_OK;
}

#endif /* FF_H */
