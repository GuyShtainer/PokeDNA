/* disk_* + get_fattime for the host RAM disk. Compiled INSTEAD of lib/fatfs/diskio.c
 * and lib/fatfs/diskio_write.c (which talk to a flashcart), so everything above this
 * line -- ff.c and source/log.c -- is the code the cartridge runs. */
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"
#include "ramdisk.h"

int  rd_protect        = 0;
long rd_fail_write_in  = 0;
int  rd_fail_all_writes = 0;
unsigned long rd_writes = 0, rd_reads = 0;

static unsigned char* s_mem = 0;
static unsigned       s_sectors = 0;

void rd_init(unsigned sectors) {
  rd_free();
  s_sectors = sectors;
  s_mem = calloc(sectors, FF_MAX_SS);
  rd_protect = 0; rd_fail_write_in = 0; rd_fail_all_writes = 0;
  rd_writes = rd_reads = 0;
}

void rd_free(void) { free(s_mem); s_mem = 0; s_sectors = 0; }

DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return s_mem ? 0 : STA_NOINIT; }

DSTATUS disk_status(BYTE pdrv) {
  (void)pdrv;
  if (!s_mem) return STA_NOINIT;
  return (DSTATUS)(rd_protect ? STA_PROTECT : 0);
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;
  if (!s_mem) return RES_NOTRDY;
  if (sector + count > s_sectors) return RES_PARERR;
  memcpy(buff, s_mem + (size_t)sector * FF_MAX_SS, (size_t)count * FF_MAX_SS);
  rd_reads += count;
  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;
  if (!s_mem) return RES_NOTRDY;
  if (rd_protect) return RES_WRPRT;
  if (rd_fail_all_writes) return RES_ERROR;
  if (rd_fail_write_in > 0) { rd_fail_write_in--; return RES_ERROR; }
  if (sector + count > s_sectors) return RES_PARERR;
  memcpy(s_mem + (size_t)sector * FF_MAX_SS, buff, (size_t)count * FF_MAX_SS);
  rd_writes += count;
  return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  (void)pdrv;
  if (!s_mem) return RES_NOTRDY;
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t*)buff = s_sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD*)buff = FF_MAX_SS;  return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD*)buff = 1;         return RES_OK;
    default: return RES_PARERR;
  }
}

/* 2026-08-18 12:00:00, so a rotated file's timestamp is deterministic. */
DWORD get_fattime(void) {
  return ((DWORD)(2026 - 1980) << 25) | ((DWORD)8 << 21) | ((DWORD)18 << 16) |
         ((DWORD)12 << 11) | ((DWORD)0 << 5) | 0;
}
