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
int  rd_lie_writes     = 0;
long rd_lie_after = -1;
long rd_fail_at = -1;
long rd_fail_reads_after = -1;
long rd_fail_read_at = -1;
unsigned long rd_writes = 0, rd_reads = 0, rd_lied = 0, rd_read_fails = 0;
unsigned long rd_read_calls = 0, rd_read_back = 0, rd_read_last = 0;

long rd_cut_sectors = -1;
int  rd_cut_torn = 0;
int  rd_cut_fired = 0;
uint32_t rd_fattime_now = 0;
uint32_t (*rd_fattime_hook)(uint32_t live) = 0;

static unsigned char* s_mem = 0;
static unsigned       s_sectors = 0;
static unsigned char* s_snap = 0;    /* the volume as rd_snapshot() saw it */
static unsigned char* s_dirty = 0;   /* per sector: written since the snapshot */

void rd_init(unsigned sectors) {
  rd_free();
  s_sectors = sectors;
  s_mem = calloc(sectors, FF_MAX_SS);
  rd_protect = 0; rd_fail_write_in = 0; rd_fail_all_writes = 0; rd_lie_writes = 0;
  rd_fail_reads_after = -1; rd_fail_read_at = -1; rd_lie_after = -1; rd_fail_at = -1;
  rd_cut_sectors = -1; rd_cut_torn = 0; rd_cut_fired = 0;
  rd_fattime_now = 0; rd_fattime_hook = 0;
  rd_writes = rd_reads = rd_lied = rd_read_fails = 0;
  rd_read_calls = rd_read_back = rd_read_last = 0;
}

void rd_free(void) {
  free(s_mem); free(s_snap); free(s_dirty);
  s_mem = 0; s_snap = 0; s_dirty = 0; s_sectors = 0;
}

void rd_snapshot(void) {
  free(s_snap); free(s_dirty);
  s_snap = malloc((size_t)s_sectors * FF_MAX_SS);
  s_dirty = calloc(s_sectors, 1);
  if (s_snap && s_mem) memcpy(s_snap, s_mem, (size_t)s_sectors * FF_MAX_SS);
}

void rd_restore(void) {
  unsigned i;
  if (!s_snap || !s_mem) return;
  for (i = 0; i < s_sectors; i++)
    if (s_dirty[i]) {
      memcpy(s_mem + (size_t)i * FF_MAX_SS, s_snap + (size_t)i * FF_MAX_SS, FF_MAX_SS);
      s_dirty[i] = 0;
    }
  rd_cut_sectors = -1; rd_cut_torn = 0; rd_cut_fired = 0;
  rd_writes = rd_lied = 0;
}

unsigned char* rd_sector(unsigned lba) { return (s_mem && lba < s_sectors) ? s_mem + (size_t)lba * FF_MAX_SS : 0; }
unsigned rd_sector_count(void) { return s_sectors; }

unsigned rd_changed(unsigned* out, unsigned max) {
  unsigned i, n = 0;
  if (!s_snap || !s_mem || !s_dirty) return 0;
  for (i = 0; i < s_sectors; i++)
    if (s_dirty[i] && memcmp(s_mem + (size_t)i * FF_MAX_SS, s_snap + (size_t)i * FF_MAX_SS, FF_MAX_SS) != 0) {
      if (n < max) out[n] = i;
      n++;
    }
  return n;
}

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
  if (rd_fail_reads_after >= 0) {
    if (rd_fail_reads_after == 0) { rd_read_fails++; return RES_ERROR; }
    rd_fail_reads_after--;
  }
  /* One honest read error, then healthy again -- the flaky cart contact, as opposed to the
   * card that dies for good. A cleanup path that deletes on failure is only VISIBLE this
   * way: the operation fails, the card recovers, and the delete really lands. */
  if (rd_fail_read_at >= 0) {
    if (rd_fail_read_at < (long)count) { rd_fail_read_at = -1; rd_read_fails++; return RES_ERROR; }
    rd_fail_read_at -= (long)count;
  }
  memcpy(buff, s_mem + (size_t)sector * FF_MAX_SS, (size_t)count * FF_MAX_SS);
  rd_reads += count;
  rd_read_calls++;
  if ((unsigned long)sector < rd_read_last) rd_read_back++;
  rd_read_last = (unsigned long)sector;
  return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;
  if (!s_mem) return RES_NOTRDY;
  if (rd_protect) return RES_WRPRT;
  if (rd_fail_all_writes) return RES_ERROR;
  if (rd_fail_write_in > 0) { rd_fail_write_in--; return RES_ERROR; }
  if (sector + count > s_sectors) return RES_PARERR;
  /* One honest failure, then healthy again -- and after the range check for the same reason
   * the liar below is. This is the knob that catches a cleanup path which DELETES a good
   * copy on failure: under rd_lie_* that cleanup's own f_unlink is swallowed too, so the
   * deletion never takes effect and the bug stays invisible. Here the card recovers, the
   * unlink really lands, and the loss is visible on the next remount. */
  if (rd_fail_at >= 0) {
    if (rd_fail_at < (long)count) { rd_fail_at = -1; return RES_ERROR; }
    rd_fail_at -= (long)count;
  }
  /* The card that says yes and keeps nothing. Deliberately AFTER the range check so a
   * genuine bug still trips PARERR, and deliberately RES_OK so nothing above this line
   * can tell -- that is the whole point. Reads keep returning the real (stale) bytes,
   * which is what a read-back is for. */
  if (rd_lie_after >= 0) {                     /* a card that goes bad part way through */
    if (rd_lie_after == 0) rd_lie_writes = 1;
    else rd_lie_after -= (long)count;
    if (rd_lie_after < 0) rd_lie_after = 0;
  }
  if (rd_lie_writes) { rd_lied += count; return RES_OK; }
  if (rd_cut_fired) return RES_ERROR;          /* the process is dead: nothing more lands */
  if (rd_cut_sectors >= 0) {                   /* the power cut, sector by sector */
    UINT i;
    for (i = 0; i < count; i++) {
      unsigned char* dst = s_mem + (size_t)(sector + i) * FF_MAX_SS;
      if (rd_cut_sectors == 0) {               /* this sector is the one in flight */
        if (rd_cut_torn > 0) {
          memcpy(dst, buff + (size_t)i * FF_MAX_SS,
                 (size_t)(rd_cut_torn < FF_MAX_SS ? rd_cut_torn : FF_MAX_SS));
          if (s_dirty) s_dirty[sector + i] = 1;
        }
        rd_cut_fired = 1;
        return RES_ERROR;
      }
      memcpy(dst, buff + (size_t)i * FF_MAX_SS, FF_MAX_SS);
      if (s_dirty) s_dirty[sector + i] = 1;
      rd_writes++; rd_cut_sectors--;
    }
    return RES_OK;
  }
  memcpy(s_mem + (size_t)sector * FF_MAX_SS, buff, (size_t)count * FF_MAX_SS);
  if (s_dirty) { UINT i; for (i = 0; i < count; i++) s_dirty[sector + i] = 1; }
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
  uint32_t t = rd_fattime_now ? rd_fattime_now :
      (((uint32_t)(2026 - 1980) << 25) | ((uint32_t)8 << 21) | ((uint32_t)18 << 16) |
       ((uint32_t)12 << 11) | ((uint32_t)0 << 5) | 0);
  return (DWORD)(rd_fattime_hook ? rd_fattime_hook(t) : t);
}
