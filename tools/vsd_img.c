/*
 * vsd_img -- the image factory, BACKLOG #179 Phase A step A2. See vsd_img.h for the
 * three entry points and docs/briefs/s179-design.md S4.6 for the design ("never a
 * second FAT implementation" -- this tool links the project's own lib/fatfs/ff.c over
 * tests/hostfat/ramdisk.c, exactly the recipe tests/host_xferio_test.c and its ten
 * siblings already use).
 *
 * Build (CLI):
 *   cc -std=c11 -Wall -Wextra -DFF_USE_MKFS=1 -Dsiprintf=sprintf \
 *      -I tests/hostfat -I lib/fatfs -I source -I tools \
 *      tools/vsd_img.c tools/host_walk.c source/pdna_romver.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/vsd_img
 *
 * Build (linked by a host test, main() stripped):
 *   same, plus -DVSD_IMG_NO_MAIN, and the test's own .c/.o instead of a `main` call.
 *
 * Usage:
 *   vsd_img mkimg OUT.img SIZE_MB [TEMPLATE_DIR]
 *   vsd_img list IMG
 *   vsd_img patch IMG PATH FILE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ff.h"
#include "diskio.h"
#include "ramdisk.h"
#include "pdna_romver.h"
#include "host_walk.h"
#include "vsd_img.h"

/* One volume mounted at a time -- this tool never needs two. Matches every existing
 * hostfat test's own s_fs/s_work statics (tests/host_savefat_test.c:44-45). */
static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];

/* ============================= raw image <-> RAM disk ========================== */

static int dump_image(const char* out_path, unsigned sectors) {
  FILE* f = fopen(out_path, "wb");
  if (!f) { fprintf(stderr, "vsd_img: cannot create %s\n", out_path); return 1; }
  unsigned char buf[FF_MAX_SS];
  unsigned s;
  int rc = 0;
  for (s = 0; s < sectors; s++) {
    if (disk_read(0, buf, s, 1) != RES_OK) {
      fprintf(stderr, "vsd_img: dump read failed at sector %u\n", s);
      rc = 1; break;
    }
    if (fwrite(buf, 1, sizeof buf, f) != sizeof buf) {
      fprintf(stderr, "vsd_img: short write to %s\n", out_path);
      rc = 1; break;
    }
  }
  fclose(f);
  return rc;
}

/* Loads `in_path` into a freshly rd_init'd RAM disk. Returns the sector count (>=0)
 * on success, -1 on any failure (message already on stderr). */
static long load_image(const char* in_path) {
  FILE* f = fopen(in_path, "rb");
  if (!f) { fprintf(stderr, "vsd_img: cannot open %s\n", in_path); return -1; }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long sz = ftell(f);
  if (sz <= 0 || (sz % FF_MAX_SS) != 0) {
    fprintf(stderr, "vsd_img: %s is not a %d-byte-sector-aligned image (%ld bytes)\n",
            in_path, FF_MAX_SS, sz);
    fclose(f); return -1;
  }
  rewind(f);
  unsigned sectors = (unsigned)(sz / FF_MAX_SS);
  rd_init(sectors);
  unsigned char buf[FF_MAX_SS];
  unsigned s;
  for (s = 0; s < sectors; s++) {
    if (fread(buf, 1, sizeof buf, f) != sizeof buf) {
      fprintf(stderr, "vsd_img: short read from %s\n", in_path);
      fclose(f); return -1;
    }
    if (disk_write(0, buf, s, 1) != RES_OK) {
      fprintf(stderr, "vsd_img: load write failed at sector %u\n", s);
      fclose(f); return -1;
    }
  }
  fclose(f);
  return (long)sectors;
}

/* ============================= mkimg ============================================ */

/* mkdir every path component of `img_dir` ("/PokeDNA/xfer" -> "/PokeDNA" then
 * "/PokeDNA/xfer"). FR_EXIST is not an error -- the same directory can be visited via
 * two different template files. */
static int mkdir_p_image(const char* img_dir) {
  char buf[FF_LFN_BUF + 1];
  size_t n = strlen(img_dir);
  if (n == 0 || n >= sizeof buf) return 1;
  strcpy(buf, img_dir);
  size_t i;
  for (i = 1; i < n; i++) {
    if (buf[i] != '/') continue;
    buf[i] = '\0';
    FRESULT fr = f_mkdir(buf);
    if (fr != FR_OK && fr != FR_EXIST) {
      fprintf(stderr, "vsd_img: f_mkdir %s failed (fr=%d)\n", buf, fr);
      return 1;
    }
    buf[i] = '/';
  }
  FRESULT fr = f_mkdir(buf);
  if (fr != FR_OK && fr != FR_EXIST) {
    fprintf(stderr, "vsd_img: f_mkdir %s failed (fr=%d)\n", buf, fr);
    return 1;
  }
  return 0;
}

static int copy_file_into_image(const char* host_path, const char* img_path) {
  FILE* hf = fopen(host_path, "rb");
  if (!hf) { fprintf(stderr, "vsd_img: cannot open %s\n", host_path); return 1; }
  FIL fp;
  FRESULT fr = f_open(&fp, img_path, FA_WRITE | FA_CREATE_ALWAYS);
  if (fr != FR_OK) {
    fprintf(stderr, "vsd_img: f_open %s failed (fr=%d)\n", img_path, fr);
    fclose(hf); return 1;
  }
  unsigned char buf[4096];
  size_t n;
  int rc = 0;
  while ((n = fread(buf, 1, sizeof buf, hf)) > 0) {
    UINT bw = 0;
    fr = f_write(&fp, buf, (UINT)n, &bw);
    if (fr != FR_OK || bw != (UINT)n) {
      fprintf(stderr, "vsd_img: f_write %s failed (fr=%d, bw=%u/%zu)\n",
              img_path, fr, (unsigned)bw, n);
      rc = 1; break;
    }
  }
  f_close(&fp);
  fclose(hf);
  return rc;
}

typedef struct { const char* template_root; int rc; } CopyTreeCtx;

static int copy_tree_cb(const char* relpath, int is_dir, void* ud) {
  CopyTreeCtx* ctx = (CopyTreeCtx*)ud;
  char img_path[FF_LFN_BUF + 2];
  int n = snprintf(img_path, sizeof img_path, "/%s", relpath);
  if (n < 0 || (size_t)n >= sizeof img_path) { ctx->rc = 1; return 1; }

  if (is_dir) {
    if (mkdir_p_image(img_path)) { ctx->rc = 1; return 1; }
    return 0;
  }
  char host_path[1024];
  n = snprintf(host_path, sizeof host_path, "%s/%s", ctx->template_root, relpath);
  if (n < 0 || (size_t)n >= sizeof host_path) { ctx->rc = 1; return 1; }
  if (copy_file_into_image(host_path, img_path)) { ctx->rc = 1; return 1; }
  return 0;
}

int vsdimg_mkimg(const char* out_path, unsigned size_mb, const char* template_dir) {
  if (size_mb == 0 || size_mb > 512) {
    fprintf(stderr, "vsd_img: mkimg size_mb out of range (%u)\n", size_mb);
    return 1;
  }
  unsigned sectors = size_mb * (1024u * 1024u / FF_MAX_SS);
  f_mount(0, "", 0);
  rd_init(sectors);

  /* FM_FAT forces FAT12/16 (never FAT32/exFAT) -- FF_FS_EXFAT is on in ffconf.h, but
   * the design (S4.6) says FAT16, and #179's delta build links a FAT12/16-only path
   * today (no exFAT symbols in the shipped ELF). FM_SFD = no partition table, matching
   * load_image's flat-sector-0-is-the-volume-boot-record assumption above.
   *
   * BACKLOG #179 A3 review D5: n_fat=1 (the third field) at a small size_mb quietly
   * built FAT12, not FAT16 -- a real SD card is never FAT12 (every one Guy owns ships
   * FAT16 or FAT32), so a small fixture image was testing a filesystem variant the
   * delta build has never once run against. n_fat=2 (two FATs, the ubiquitous real-
   * card default) and an explicit post-mount FS_FAT16 check below now make "this
   * mounted, so it must be a real card's shape" load-bearing instead of assumed. */
  MKFS_PARM opt = { FM_FAT | FM_SFD, 2, 1, 0, 0 };
  FRESULT fr = f_mkfs("", &opt, s_work, sizeof s_work);
  if (fr != FR_OK) { fprintf(stderr, "vsd_img: f_mkfs failed (fr=%d)\n", fr); return 1; }
  fr = f_mount(&s_fs, "", 1);
  if (fr != FR_OK) { fprintf(stderr, "vsd_img: f_mount failed (fr=%d)\n", fr); return 1; }
  if (s_fs.fs_type != FS_FAT16) {
    fprintf(stderr, "vsd_img: mkimg built fs_type=%d, not FAT16 -- use >= 16 MiB "
                     "(a real card is never FAT12)\n", s_fs.fs_type);
    f_mount(0, "", 0);
    return 1;
  }

  fr = f_mkdir("/PokeDNA");
  if (fr != FR_OK && fr != FR_EXIST) {
    fprintf(stderr, "vsd_img: f_mkdir /PokeDNA failed (fr=%d)\n", fr);
    return 1;
  }

  if (template_dir) {
    CopyTreeCtx ctx = { template_dir, 0 };
    if (host_walk_tree(template_dir, copy_tree_cb, &ctx) != 0 || ctx.rc) {
      fprintf(stderr, "vsd_img: copying template %s failed\n", template_dir);
      return 1;
    }
  }

  f_mount(0, "", 0); /* flush + unmount before the raw dump */
  return dump_image(out_path, sectors);
}

/* ============================= list ============================================= */

typedef struct { char path[FF_LFN_BUF + 2]; FSIZE_t size; uint32_t crc; } ListEntry;

typedef struct {
  ListEntry* entries;
  size_t count, cap;
  int rc;
} ListCtx;

static int list_push(ListCtx* lc, const char* path, FSIZE_t size, uint32_t crc) {
  if (lc->count == lc->cap) {
    size_t newcap = lc->cap ? lc->cap * 2 : 64;
    ListEntry* p = (ListEntry*)realloc(lc->entries, newcap * sizeof *p);
    if (!p) return 1;
    lc->entries = p;
    lc->cap = newcap;
  }
  ListEntry* e = &lc->entries[lc->count++];
  strncpy(e->path, path, sizeof e->path - 1);
  e->path[sizeof e->path - 1] = '\0';
  e->size = size;
  e->crc = crc;
  return 0;
}

static int crc_file(const char* img_path, FSIZE_t size, uint32_t* out_crc) {
  FIL fp;
  FRESULT fr = f_open(&fp, img_path, FA_READ);
  if (fr != FR_OK) return 1;
  uint32_t tab[16];
  pdna_rv_crc32_table(tab);
  uint32_t crc = PDNA_RV_CRC_INIT;
  unsigned char buf[4096];
  FSIZE_t left = size;
  int rc = 0;
  while (left > 0) {
    UINT want = (left > sizeof buf) ? (UINT)sizeof buf : (UINT)left;
    UINT br = 0;
    fr = f_read(&fp, buf, want, &br);
    if (fr != FR_OK || br != want) { rc = 1; break; }
    crc = pdna_rv_crc32_upd(tab, crc, buf, br, 0);
    left -= br;
  }
  f_close(&fp);
  if (rc) return rc;
  *out_crc = pdna_rv_crc32_fin(crc);
  return 0;
}

/* Recursive walk of the IMAGE's own directory tree (f_opendir/f_readdir -- FatFs'
 * DIR, unrelated to tools/host_walk.c's use of POSIX DIR; see host_walk.h for why
 * those two live in separate translation units). Bounded depth (16) matches the FAT
 * short-path practical ceiling and gives loop 2 (golden rule 2) a provable bound. */
static int list_walk(const char* img_dir, ListCtx* lc, int depth) {
  if (depth > 16) { fprintf(stderr, "vsd_img: directory nesting too deep at %s\n", img_dir); return 1; }
  DIR d;
  FRESULT fr = f_opendir(&d, img_dir);
  if (fr != FR_OK) { fprintf(stderr, "vsd_img: f_opendir %s failed (fr=%d)\n", img_dir, fr); return 1; }
  int rc = 0;
  for (;;) {
    FILINFO fno;
    fr = f_readdir(&d, &fno);
    if (fr != FR_OK) { rc = 1; break; }
    if (fno.fname[0] == '\0') break; /* end of directory */

    char child[FF_LFN_BUF + 2];
    int n = snprintf(child, sizeof child, "%s%s%s", img_dir,
                      (img_dir[strlen(img_dir) - 1] == '/') ? "" : "/", fno.fname);
    if (n < 0 || (size_t)n >= sizeof child) { rc = 1; break; }

    if (fno.fattrib & AM_DIR) {
      if (list_walk(child, lc, depth + 1)) { rc = 1; break; }
    } else {
      uint32_t crc = 0;
      if (crc_file(child, fno.fsize, &crc)) { rc = 1; break; }
      if (list_push(lc, child, fno.fsize, crc)) { rc = 1; break; }
    }
  }
  f_closedir(&d);
  return rc;
}

static int list_cmp(const void* a, const void* b) {
  return strcmp(((const ListEntry*)a)->path, ((const ListEntry*)b)->path);
}

int vsdimg_list(const char* img_path, FILE* out) {
  if (load_image(img_path) < 0) return 1;
  FRESULT fr = f_mount(&s_fs, "", 1);
  if (fr != FR_OK) { fprintf(stderr, "vsd_img: f_mount %s failed (fr=%d)\n", img_path, fr); return 1; }

  ListCtx lc = { 0, 0, 0, 0 };
  int rc = list_walk("/", &lc, 0);
  f_mount(0, "", 0);
  if (rc) { free(lc.entries); return 1; }

  qsort(lc.entries, lc.count, sizeof *lc.entries, list_cmp);
  size_t i;
  for (i = 0; i < lc.count; i++) {
    ListEntry* e = &lc.entries[i];
    fprintf(out, "%s %lu %08x\n", e->path, (unsigned long)e->size, e->crc);
  }
  free(lc.entries);
  return 0;
}

/* ============================= patch ============================================ */

int vsdimg_patch(const char* img_path, const char* target_path, const char* src_file) {
  long sectors = load_image(img_path);
  if (sectors < 0) return 1;
  FRESULT fr = f_mount(&s_fs, "", 1);
  if (fr != FR_OK) { fprintf(stderr, "vsd_img: f_mount %s failed (fr=%d)\n", img_path, fr); return 1; }

  int rc = copy_file_into_image(src_file, target_path);

  f_mount(0, "", 0);
  if (rc) return 1;
  return dump_image(img_path, (unsigned)sectors);
}

/* ============================= CLI =============================================== */

#ifndef VSD_IMG_NO_MAIN
static void usage(const char* argv0) {
  fprintf(stderr,
    "usage:\n"
    "  %s mkimg OUT.img SIZE_MB [TEMPLATE_DIR]\n"
    "  %s list IMG\n"
    "  %s patch IMG PATH FILE\n",
    argv0, argv0, argv0);
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(argv[0]); return 2; }
  if (strcmp(argv[1], "mkimg") == 0) {
    if (argc < 4 || argc > 5) { usage(argv[0]); return 2; }
    unsigned size_mb = (unsigned)strtoul(argv[3], NULL, 10);
    const char* tmpl = (argc == 5) ? argv[4] : NULL;
    return vsdimg_mkimg(argv[2], size_mb, tmpl) ? 1 : 0;
  }
  if (strcmp(argv[1], "list") == 0) {
    if (argc != 3) { usage(argv[0]); return 2; }
    return vsdimg_list(argv[2], stdout) ? 1 : 0;
  }
  if (strcmp(argv[1], "patch") == 0) {
    if (argc != 5) { usage(argv[0]); return 2; }
    return vsdimg_patch(argv[2], argv[3], argv[4]) ? 1 : 0;
  }
  usage(argv[0]);
  return 2;
}
#endif /* VSD_IMG_NO_MAIN */
