/* Host shim for source/sys.h, so source/log.c dual-compiles on the Mac.
 * log.c wants exactly one thing from sys.h: the EWRAM_BSS attribute. */
#ifndef SYS_H
#define SYS_H
#define EWRAM_BSS
#endif /* SYS_H */
