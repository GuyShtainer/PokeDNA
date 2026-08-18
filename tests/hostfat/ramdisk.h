/* A RAM disk for the real lib/fatfs, with the failure knobs a flashcart actually has.
 *
 * Every knob mirrors a condition Guy's card can be in, and each one is a way the log
 * can go silent on hardware:
 *   rd_protect            -> a write-protected volume: f_open(FA_WRITE) is refused
 *   rd_fail_write_in      -> the Nth disk_write fails (EZ-Flash writes have no retry)
 *   rd_fail_all_writes    -> every disk_write fails (this is an EverDrive, by design)
 *   a small volume         -> a FULL card: f_write returns FR_DENIED with bw < want
 */
#ifndef HOSTFAT_RAMDISK_H
#define HOSTFAT_RAMDISK_H

void rd_init(unsigned sectors);   /* allocate + zero a volume, clear all knobs */
void rd_free(void);

extern int  rd_protect;           /* !=0: disk_status reports STA_PROTECT      */
extern long rd_fail_write_in;     /* >0: fail that many writes from now, then heal */
extern int  rd_fail_all_writes;   /* !=0: every disk_write returns RES_ERROR   */
extern unsigned long rd_writes;   /* successful sector writes since rd_init    */
extern unsigned long rd_reads;

#endif /* HOSTFAT_RAMDISK_H */
