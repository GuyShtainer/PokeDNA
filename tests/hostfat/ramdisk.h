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
extern long rd_lie_after;         /* >=0: let that many writes land, then start lying
                                   * (-1 = disabled). A card that goes bad PART WAY
                                   * through an operation -- which is how a verified
                                   * write can pass its own read-back and still have
                                   * its final rename swallowed.                  */
extern long rd_fail_at;           /* >=0: let that many writes land, fail exactly ONE, then
                                   * be healthy again (-1 = disabled). The honest transient
                                   * error -- a bad block, a directory with no room -- as
                                   * opposed to the rd_lie_* liars. It is the only way to
                                   * observe a cleanup path that DELETES on failure, because
                                   * a permanently lying card swallows the cleanup's own
                                   * f_unlink too and so hides the very loss it causes. */
extern long rd_fail_reads_after;  /* >=0: let that many reads through, then fail every
                                   * read (-1 = disabled). Lets a test sweep a read
                                   * error across every step of a flush.          */
extern long rd_fail_read_at;      /* >=0: let that many reads through, fail exactly ONE,
                                   * then be healthy again (-1 = disabled). The read-side
                                   * twin of rd_fail_at, and needed for the same reason: it
                                   * is the only knob that can make an f_open fail while
                                   * the very next f_unlink SUCCEEDS, which is what it
                                   * takes to see a cleanup path delete a file this call
                                   * never created. rd_fail_reads_after cannot -- once
                                   * reads stay dead the cleanup's own unlink dies with
                                   * them and the file survives by accident.        */
extern unsigned long rd_lied;     /* sectors swallowed by rd_lie_writes         */
extern unsigned long rd_read_fails; /* reads refused by rd_fail_reads_after      */
extern unsigned long rd_writes;   /* successful sector writes since rd_init    */
extern unsigned long rd_reads;    /* SECTORS delivered by disk_read            */
/* disk_read CALLS -- the transfer count, which is NOT rd_reads and is the number the
 * hardware actually charges for. Per lib/ezflashomega/io_ezfo.c, one call costs 24
 * fixed halfword cart writes with IRQs off plus one card-latency poll per <=4-sector
 * chunk, all of it size-INDEPENDENT; only the sectors scale (~46 us each). So 4 calls
 * carrying 42 sectors and 21 calls carrying 42 sectors move the same bytes at very
 * different prices, and rd_reads alone cannot tell them apart. */
extern unsigned long rd_read_calls;
/* How many disk_read calls started at a LOWER sector than the previous call did, and
 * the previous call's start. A backward seek is the one case FatFs restarts a cluster
 * walk from the head of the chain (ff.c:4527) -- a forward one resumes incrementally
 * (ff.c:4521) -- so "did this sweep only ever move forward" is a real, cheap, and
 * otherwise invisible property to assert. Both are plain counters a test zeroes before
 * the region it means to measure. */
extern unsigned long rd_read_back;
extern unsigned long rd_read_last;

/* ---- the POWER CUT (y19-s1) -- unlike the honest-error / liar knobs above, a cut is not an
 * error return: the process is simply gone. The first N sectors of a run land intact, the
 * next one lands TORN (its first rd_cut_torn bytes new, the rest still the old bytes -- a
 * sector programmed part way), nothing after it lands, and every later disk_write reports
 * RES_ERROR so the operation under test unwinds fast (the test never trusts what the
 * dead process would have done next; it remounts the RAM disk and reads what is on it).
 *
 *   rd_cut_sectors >= 0 : that many MORE sectors land, then power is cut (-1 = disabled)
 *   rd_cut_torn         : bytes (0..511) of the FIRST sector past the cut that still land
 *   rd_cut_fired        : set when the cut has happened
 * rd_snapshot()/rd_restore() make thousands of cut points affordable: restore rewinds only
 * the sectors written since the snapshot. rd_fattime_now (0 = the fixed default stamp) is
 * the live RTC; rd_fattime_hook, when set, is get_fattime's filter -- the seam the
 * journal's frozen-timestamp hook plugs into on the host exactly as slice 2 plugs it into
 * diskio_write.c's get_fattime on the cartridge. */
#include <stdint.h>
extern long rd_cut_sectors;
extern int  rd_cut_torn;
extern int  rd_cut_fired;
extern uint32_t rd_fattime_now;
extern uint32_t (*rd_fattime_hook)(uint32_t live);
void rd_snapshot(void);           /* remember the volume as it is now */
void rd_restore(void);            /* rewind to the snapshot; clears the cut + write counters */
unsigned char* rd_sector(unsigned lba);   /* peek at a sector (test-side inspection) */
unsigned rd_sector_count(void);
unsigned rd_changed(unsigned* out, unsigned max);   /* sectors whose bytes differ from the snapshot */

#endif /* HOSTFAT_RAMDISK_H */
