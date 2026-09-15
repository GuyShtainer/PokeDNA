#ifndef IO_EZFO_H
#define IO_EZFO_H

#pragma GCC system_header

#include "../sys.h"

bool _EZFO_startUp(void);

/* Outcome of the last _EZFO_startUp(), for boot diagnostics. */
#define EZFO_DET_NONE        0   /* never ran                                           */
#define EZFO_DET_OK          1   /* found the running image (content-verified)          */
#define EZFO_DET_NOT_EZFO    2   /* unmapping the ROM changed nothing: not this cart     */
#define EZFO_DET_NO_PAGE     3   /* the cart answered, but no page shows this image      */
#define EZFO_DET_OK_HDRONLY  4   /* found by the header word only: the image would not   */
                                 /* checksum the same twice at boot (unstable bus)       */
int _EZFO_detect_result(void);
/* The page the running image was found on: 0x200 = PSRAM (SD-loaded), 0..0x1FF = the
 * game NOR (page * 128 KiB from its start), 0xFFFF = not located. */
u16 _EZFO_rompage(void);
/* Pages whose header word matched ours but whose contents did not -- a stale build with
 * the same title still sitting in PSRAM or NOR. `first` receives the first such page
 * (0xFFFF if none). Returns the count. */
unsigned _EZFO_lookalikes(unsigned* first);
bool _EZFO_readSectors(u32 address, u32 count, void* buffer);
bool _EZFO_writeSectors(u32 address, u32 count, const void* buffer);
void _EZFO_reboot(void);   /* SetRompage(BOOTLOADER) + SoftReset -> kernel; no return */

#endif /* IO_EZFO_H */
