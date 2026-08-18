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
typedef enum {
  FR_OK = 0, FR_DISK_ERR = 1, FR_NO_FILE = 4, FR_NO_PATH = 5, FR_DENIED = 7,
  FR_EXIST = 8, FR_WRITE_PROTECTED = 10
} FRESULT;
typedef struct { FILE* fp; unsigned long sz; } FIL;
typedef struct { unsigned long fsize; } FILINFO;

#define FA_READ         0x01
#define FA_WRITE        0x02
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_APPEND  0x30

extern int hostff_fail_open;
extern int hostff_fail_writes;
extern int hostff_fail_close;
extern int hostff_opens;                 /* count of successful f_open calls */
extern int hostff_fail_stat;             /* f_stat returns FR_DISK_ERR: the read-back
                                          * itself broken, which must NOT read as ok  */

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
  /* Real FatFs grows fp->obj.objsize as it writes, and log.c reads f_size() AFTER the
   * write to learn what the file should now hold. A shim that left sz at its open-time
   * value would make that read-back compare against a stale number and pass no matter
   * what the file did -- a vacuous test of the one thing this is here to check. */
  f->sz += *bw;
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

/* The read-back. log.c asks the medium what it actually kept, because on an EZ-Flash a
 * write's return code is not evidence (tests/host_logfat_test.c proves the ACK-and-drop
 * case over real FatFs). Here it is just stat(), plus a knob for "the read-back broke". */
static inline FRESULT f_stat(const char* p, FILINFO* fi) {
  FILE* g;
  if (hostff_fail_stat) return FR_DISK_ERR;
  g = fopen(p, "rb");
  if (!g) return FR_NO_FILE;
  fseek(g, 0, SEEK_END);
  fi->fsize = (unsigned long)ftell(g);
  fclose(g);
  return FR_OK;
}

static inline FRESULT f_unlink(const char* p) { return remove(p) ? FR_NO_FILE : FR_OK; }
/* log.c creates its path's parent folder when f_open says FR_NO_PATH. This shim's
 * f_open is fopen(), which never reports FR_NO_PATH, so the stub is never reached --
 * the REAL behaviour is covered in tests/host_logfat_test.c over real FatFs. */
static inline FRESULT f_mkdir(const char* p) { (void)p; return FR_OK; }
static inline FRESULT f_rename(const char* a, const char* b) {
  return rename(a, b) ? FR_NO_FILE : FR_OK;
}

#endif /* FF_H */
