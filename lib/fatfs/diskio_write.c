/*-----------------------------------------------------------------------*/
/* Write path for the Flashcart FatFs disk module.                       */
/*                                                                       */
/* afska's diskio.c ships read-only (disk_status/initialize/read). This  */
/* file adds the three things FatFs needs once FF_FS_READONLY == 0:      */
/* disk_write, disk_ioctl (CTRL_SYNC), and get_fattime. Kept separate so */
/* the vendored diskio.c is untouched.                                   */
/*-----------------------------------------------------------------------*/

#include <string.h>

#include "../flashcartio.h"
#include "../flashcartio_write.h"
#include "../sys.h"

#include "ff.h"     /* integer types (WORD/DWORD/LBA_t) */
#include "diskio.h" /* DRESULT, command codes */
#include "gba_rtc.h" /* cartridge RTC (resolved via -Isource) */
#include "perf.h"    /* PERF_SD_WRITE -- see the note in diskio.c's disk_read */
#ifdef PDNA_DELTA
#include "log.h"     /* BACKLOG #179 A3 review D7's own unaligned-write log line, -Isource */
#endif

#define ALIGNED __attribute__((aligned(4)))

/* Shared with disk_read — see the long note on fc_bounce in diskio.c. Do NOT reintroduce
 * a second 2 KiB buffer here; that is what overflowed EWRAM onto the flashcart driver. */
extern u8 fc_bounce[512 * 4];

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  (void)pdrv;

  /* Same contract as disk_read's counter: arithmetic on EWRAM words, before the
   * transfer, at the one choke point. */
  PERF_SD_WRITE(count);

#ifdef PDNA_DELTA
  /* BACKLOG #179 A3 review D8: tools/vsd.py's S7.4 ROM-source check reads the
   * MAILBOX's `addr` field -- which, for a 2-mod-4 `buff`, is always fc_bounce
   * (EWRAM) by the time it gets there (D7's own bounce), never the caller's real ROM
   * pointer. Checked here too, on the pre-bounce `buff` itself, so the "f_write from
   * ROM writes the BOOTLOADER" bug class rom-load-lab found is caught for an
   * unaligned source too, not only an aligned one. */
  if ((u32)buff >= 0x08000000u && (u32)buff <= 0x0DFFFFFFu) {
    log_line("vsd: REFUSED disk_write from ROM buff=0x%08x sector=%lu", (unsigned)buff,
             (unsigned long)sector);
    return RES_ERROR;
  }
#endif

  /* WORD alignment, not halfword -- see the long note in disk_read(). The EZ-Flash write
   * path is the same DMA32 copy in the opposite direction, so a 2-mod-4 SOURCE reads two
   * bytes early and writes shifted data to the card. On the write side that is a
   * data-loss bug, not just a display one. */
  if ((u32)buff & 0x3) {
#ifdef PDNA_DELTA
    /* BACKLOG #179 A3 review D7: same fix as diskio.c's disk_read -- vsd.py's own
     * unaligned_count is structurally 0 (the mailbox only ever sees fc_bounce, always
     * 4-aligned), so log the real unaligned SOURCE once per disk_write CALL, before
     * it is bounced away, at the one place that still has the caller's own buff. */
    log_line("vsd: unaligned write buff=0x%08x sector=%lu count=%u", (unsigned)buff,
             (unsigned long)sector, (unsigned)count);
#endif
    /* Unaligned source: stage through the aligned buffer, 4 sectors at a time. */
    for (UINT i = 0; i < count; i += 4) {
      const u16 blocks = (count - i > 4) ? 4 : (u16)(count - i);
      memcpy(fc_bounce, buff + i * 512, (size_t)blocks * 512);
      if (!flashcartio_write_sector((u32)sector + i, fc_bounce, blocks))
        return RES_ERROR;
    }
    return RES_OK;
  } else {
    return flashcartio_write_sector((u32)sector, (const u8*)buff, (u16)count)
               ? RES_OK
               : RES_ERROR;
  }
}

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  (void)pdrv;

  switch (cmd) {
    case CTRL_SYNC:
      /* Writes are synchronous (Write_SD_sectors blocks), nothing to flush. */
      return RES_OK;

    case GET_SECTOR_SIZE:
      *(WORD*)buff = 512;
      return RES_OK;

    case GET_BLOCK_SIZE:
      *(DWORD*)buff = 1;
      return RES_OK;

    /* GET_SECTOR_COUNT is only needed by f_mkfs, which we never call. */
    default:
      return RES_PARERR;
  }
}

/*-----------------------------------------------------------------------*/
/* Timestamp for created/modified files                                  */
/*-----------------------------------------------------------------------*/
/* Read the flashcart's RTC and pack it into the FatFs DOS datetime. Per the   */
/* "RTC only, no fallback" choice, if the cart doesn't expose its RTC we return */
/* 0 (an unset timestamp) rather than a fabricated date.                        */

DWORD get_fattime(void) {
  GbaRtcTime t;
  if (gba_rtc_get(&t)) {
    return ((DWORD)(t.year - 1980) << 25)
         | ((DWORD)t.month  << 21)
         | ((DWORD)t.day    << 16)
         | ((DWORD)t.hour   << 11)
         | ((DWORD)t.minute << 5)
         | ((DWORD)(t.second / 2));
  }
  return 0;
}
