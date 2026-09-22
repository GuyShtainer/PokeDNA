/*-----------------------------------------------------------------------*/
/* Low level, read-only, Flashcart I/O module for FatFs                  */
/*-----------------------------------------------------------------------*/

#include <string.h>
#include "../flashcartio.h"
#include "../sys.h"

#include "ff.h" /* Obtains integer types */

#include "diskio.h" /* Declarations of disk functions */

#ifdef PDNA_DELTA
#include "log.h"    /* BACKLOG #179 A3 review D7's own unaligned-read log line, -Isource */
#endif

/* Telemetry only. perf.h is stdint-only on purpose so it can sit alongside sys.h's
 * u8/u16/u32 MACROS here without a type clash, and PERF_SD_READ/PERF_SD_WRITE expand
 * to plain arithmetic on EWRAM words -- no call, no log line, no ROM access. That
 * matters HERE specifically: a few instructions below, flashcartio_read_sector unmaps
 * the cartridge ROM for the length of the transfer with IRQs off (hard rule 1), so
 * anything that reached into ROM-resident code from this function would be the exact
 * class of bug that rule exists to prevent. Compiles to nothing under -DPDNA_PERF=0.
 * Resolved through the Makefile's -Isource. */
#include "perf.h"

#define ALIGNED __attribute__((aligned(4)))

/* ONE bounce buffer, shared by disk_read and disk_write.
 *
 * They are never in flight together: each fills and drains it inside a single call with
 * IRQs off for the whole flashcart transfer, and neither calls the other. Two separate
 * 2 KiB buffers is what pushed PokeDNA's .sbss 868 bytes PAST THE END OF EWRAM in the
 * 2026-08 map build — and because EWRAM mirrors every 256 KiB, the overhang landed on
 * 0x02000000, which is where the EZ-Flash driver's own EWRAM-resident code lives
 * (_EZFO_startUp, _EZFO_readSectors, and the head of _EZFO_writeSectors). Staging a write
 * therefore overwrote the routine performing it: every SD write failed, which surfaced as
 * "BACKUP FAILED / Save NOT modified". Reads kept working, so the symptom looked like a
 * save bug rather than a memory bug. Keep this ONE buffer. */
u8 EWRAM_BSS fc_bounce[512 * 4] ALIGNED;

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status(BYTE driveId) {
  return driveId == 0 ? 0 : STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* Initialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize(BYTE driveId) {
  return active_flashcart != NO_FLASHCART ? 0 : STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
  /* Counted here, at the ONE choke point every SD read in the app passes through, and
   * counted BEFORE the transfer rather than after: a read that never returns is
   * exactly the one we most want to see in the log, and a failed read still cost the
   * card its round trip. `count` is what FatFs asked for -- note the unaligned branch
   * below splits that into ceil(count/4) driver calls, so on that path the real number
   * of EZ-Flash transactions is higher than the one call recorded here. */
  PERF_SD_READ(count);

  /* WORD alignment, not halfword. The EZ-Flash read path is a DMA32 copy
   * (io_ezfo.c Read_SD_sectors -> sys.h dmaCopy -> DMA_Copy(..., DMA32)) and the GBA DMA
   * unit force-aligns its addresses DOWN to the transfer width. A destination at 2 mod 4
   * therefore receives every whole sector TWO BYTES EARLY -- and Read_SD_sectors still
   * returns RES_OK, so FatFs reports FR_OK over shifted data. The non-DMA fallback in
   * sys.h is a u32 store loop, which is just as wrong on an unaligned pointer.
   *
   * FatFs hands us a 2-mod-4 pointer ROUTINELY: f_read copies the head partial sector
   * through the FIL buffer and then passes `buff + head_len` straight to disk_read, where
   * head_len = 512 - (fptr % 512). So ANY read starting at a 2-mod-4 file offset lands
   * here, even when the caller's buffer is perfectly word-aligned. Masking 0x1 only
   * caught odd addresses, which in practice never occur.
   *
   * Found 2026-07-31: it is why PokeDNA's map viewer rendered Emerald corrupt (RSE
   * metatile attribute arrays are u16, so about half of Emerald's metatile tables start
   * at 2 mod 4) while FireRed/LeafGreen -- whose attributes are u32, keeping every array
   * word-aligned -- were structurally immune. */
  if ((u32)buff & 0x3) {
#ifdef PDNA_DELTA
    /* BACKLOG #179 A3 review D7: tools/vsd.py's own unaligned_count counts the
     * mailbox's `addr` field, which by the time a served transaction reaches vsd_xfer
     * is ALWAYS fc_bounce (4-byte aligned, ALIGNED static below) -- structurally 0 on
     * this vehicle regardless of what the CALLER's original `buff` was. This logs the
     * real unaligned-source fact, once per disk_read CALL (not once per bounce
     * chunk), at the one place that still has the caller's own pointer. */
    log_line("vsd: unaligned read buff=0x%08x sector=%lu count=%u", (unsigned)buff,
             (unsigned long)sector, (unsigned)count);
#endif
    for (UINT i = 0; i < count; i += 4) {
      const u16 blocks = (count - i > 4) ? 4 : (count - i);

      if (!flashcartio_read_sector(sector + i, fc_bounce, blocks))
        return RES_ERROR;

      memcpy(buff + i * 512, fc_bounce, blocks * 512);
    }
    return RES_OK;
  } else {
    return flashcartio_read_sector(sector, buff, count) ? RES_OK : RES_ERROR;
  }
}
