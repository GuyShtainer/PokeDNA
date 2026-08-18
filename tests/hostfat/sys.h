/* Host shim for source/sys.h so source/log.c dual-compiles on the Mac against the
 * REAL lib/fatfs. log.c wants exactly one thing from sys.h: the EWRAM_BSS attribute.
 *
 * Deliberately separate from tests/hostff/sys.h: that directory also holds a FAKE
 * ff.h, and having it on the include path would shadow the real one. */
#ifndef SYS_H
#define SYS_H
#define EWRAM_BSS
#endif /* SYS_H */
