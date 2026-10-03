/* host shim of FatFs for tests/host_dexbatch_test.c: just the f_* surface gb_art_source.c uses.
 * FIL is padded to the real size (FF_MAX_SS 512 window + header) so GbArtBatch's _Static_assert
 * on sizeof(GbArtBatchImpl) is checked against a realistic FIL. The implementation (in the
 * test) counts opens/reads/seeks and serves the ROM from the corpus, other paths from RAM. */
#ifndef HOSTSHIM_FF_H
#define HOSTSHIM_FF_H
#include <stdint.h>
#include <stdio.h>
typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH, FR_DENIED } FRESULT;
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08
typedef struct {
  FILE* fp; int mem; FSIZE_t fptr; FSIZE_t fsize; uint8_t err; uint8_t flag;
  unsigned char buf[512];
} FIL;
#define f_size(fp) ((fp)->fsize)
FRESULT f_open(FIL* fp, const char* path, uint8_t mode);
FRESULT f_close(FIL* fp);
FRESULT f_read(FIL* fp, void* buf, UINT btr, UINT* br);
FRESULT f_write(FIL* fp, const void* buf, UINT btw, UINT* bw);
FRESULT f_lseek(FIL* fp, FSIZE_t ofs);
#endif
