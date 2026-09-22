#ifndef VSD_IMG_H
#define VSD_IMG_H
#include <stdio.h>
/*
 * vsd_img -- the image factory, BACKLOG #179 Phase A step A2. Builds/inspects/patches
 * a FAT16 volume file through the project's OWN lib/fatfs (the exact ff.c the GBA
 * links, over tests/hostfat's ramdisk.c), never a second FAT implementation. See
 * tools/vsd_img.c's header comment for the build recipe and docs/briefs/s179-design.md
 * S4.6.
 *
 * Every function returns 0 on success, nonzero on failure (a message already went to
 * stderr). Declared here (rather than static in vsd_img.c) so a host test can link
 * this file with -DVSD_IMG_NO_MAIN and call them directly, without picking up main().
 */

/* f_mkfs a fresh FAT16 volume of `size_mb` MiB, f_mkdir "/PokeDNA", and if
 * `template_dir` is non-NULL, recursively copy its tree in (host path -> image path,
 * 1:1, '/' separators, rooted at the image's "/"). Dumps the resulting volume to
 * `out_path` as a flat 512-byte-sector raw image (no partition table -- disk_ioctl's
 * GET_SECTOR_SIZE is 512, matching FF_MAX_SS). */
int vsdimg_mkimg(const char* out_path, unsigned size_mb, const char* template_dir);

/* Mount `img_path`, walk every regular file under "/", print one "path size crc32"
 * line per file to `out`, sorted by path so two lists of the same tree always compare
 * byte-identical regardless of on-disk directory order. CRC32 matches zlib.crc32 (the
 * project's own pdna_rv_crc32, source/pdna_romver.c -- never a second CRC32). */
int vsdimg_list(const char* img_path, FILE* out);

/* Mount `img_path`, f_open(target_path, FA_WRITE|FA_CREATE_ALWAYS) (creates the file
 * if absent, truncates+rewrites if present -- this is the "seed a real .pds into a
 * fixture image" path A5(b) needs, not just an in-place same-length patch), write
 * `src_file`'s host bytes in full, f_close, dump the volume back over `img_path`. */
int vsdimg_patch(const char* img_path, const char* target_path, const char* src_file);

#endif /* VSD_IMG_H */
