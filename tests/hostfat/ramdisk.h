/* A RAM disk for the real lib/fatfs, with the failure knobs a flashcart actually has.
 *
 * Every knob mirrors a condition Guy's card can be in, and each one is a way the log
 * can go silent on hardware:
 *   rd_protect            -> a write-protected volume: f_open(FA_WRITE) is refused
 *   rd_fail_write_in      -> the Nth disk_write fails (EZ-Flash writes have no retry)
 *   rd_fail_all_writes    -> every disk_write fails (this is an EverDrive, by design)
 *   rd_lie_writes         -> every disk_write REPORTS SUCCESS and keeps nothing
 *   a small volume         -> a FULL card: f_write returns FR_DENIED with bw < want
 *
 * rd_lie_writes is the nastiest of them and the reason this file exists. An EZ-Flash
 * write has NO RETRY and NO READ-BACK: flashcartio_write_sector returns whatever
 * _EZFO_writeSectors said, and Write_SD_sectors returning 0 is the only thing that can
 * make it false. A card (or a cart contact) that acknowledges a sector it never stored
 * is therefore INVISIBLE to every layer above it -- FatFs returns FR_OK, f_close
 * returns FR_OK, and the app's counters say everything committed. That is precisely
 * the failure mode docs/kb/safety-pipeline.md's verified-write pattern exists for, and
 * the only way to catch it is to READ BACK. This knob makes that testable on the host.
 */
#ifndef HOSTFAT_RAMDISK_H
#define HOSTFAT_RAMDISK_H

void rd_init(unsigned sectors);   /* allocate + zero a volume, clear all knobs */
void rd_free(void);

extern int  rd_protect;           /* !=0: disk_status reports STA_PROTECT      */
extern long rd_fail_write_in;     /* >0: fail that many writes from now, then heal */
extern int  rd_fail_all_writes;   /* !=0: every disk_write returns RES_ERROR   */
extern int  rd_lie_writes;        /* !=0: every disk_write returns RES_OK and discards */
extern long rd_fail_reads_after;  /* >=0: let that many reads through, then fail every
                                   * read (-1 = disabled). Lets a test sweep a read
                                   * error across every step of a flush.          */
extern unsigned long rd_lied;     /* sectors swallowed by rd_lie_writes         */
extern unsigned long rd_read_fails; /* reads refused by rd_fail_reads_after      */
extern unsigned long rd_writes;   /* successful sector writes since rd_init    */
extern unsigned long rd_reads;

#endif /* HOSTFAT_RAMDISK_H */
