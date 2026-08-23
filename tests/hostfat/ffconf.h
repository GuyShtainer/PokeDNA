/* Host FatFs configuration: the REAL lib/fatfs/ffconf.h, with f_mkfs enabled.
 *
 * The point of tests/hostfat is to run the SHIPPED lib/fatfs/ff.c on the Mac over a
 * RAM disk, so the log's file handling is tested against real FAT semantics instead of
 * against stdio (which differs where it matters: rename() silently REPLACES an existing
 * target, f_rename returns FR_EXIST; fopen("ab") creates missing directories never,
 * FatFs returns FR_NO_PATH; a full volume is unreachable with stdio at all).
 *
 * Only one option changes, and only in the direction of "the test can build a volume":
 * FF_USE_MKFS, which the GBA build has no use for. Everything else -- FAT/exFAT, LFN,
 * sector size, FF_USE_FASTSEEK -- is byte-for-byte what the cartridge runs, because a
 * test on a different configuration proves nothing about the cartridge. (FF_USE_FASTSEEK
 * went to 1 on 2026-08-23; it is opt-in per handle, and tests/host_fastseek_test.c is
 * what holds that claim to account.)
 */
#include "../../lib/fatfs/ffconf.h"

#undef FF_USE_MKFS
#define FF_USE_MKFS 1
