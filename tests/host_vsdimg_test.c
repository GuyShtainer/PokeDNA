/* Host test for tools/vsd_img.{c,h} -- the image factory, BACKLOG #179 Phase A step
 * A2. Round-trips mkimg -> list -> patch -> list on the REAL lib/fatfs (the exact ff.c
 * the GBA links, over tests/hostfat's ramdisk.c) and mounts the same image a second
 * time through a fresh f_mount to prove FatFs on the "GBA side" reads back exactly
 * what the factory wrote -- there is no second FAT implementation anywhere in this
 * path (docs/briefs/s179-design.md S4.6).
 *
 *   cc -std=c11 -Wall -Wextra -DFF_USE_MKFS=1 -DVSD_IMG_NO_MAIN -Dsiprintf=sprintf \
 *      -I tests/hostfat -I lib/fatfs -I source -I tools \
 *      tests/host_vsdimg_test.c tools/vsd_img.c tools/host_walk.c source/pdna_romver.c \
 *      lib/fatfs/ff.c lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hvsdimg && /tmp/hvsdimg
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vsd_img.h"
#include "ff.h"
#include "diskio.h"
#include "ramdisk.h"

static int fails = 0;
#define CHECK(c, ...) \
  do { \
    if (!(c)) { \
      printf("FAIL: " __VA_ARGS__); \
      printf("\n"); \
      fails++; \
    } \
  } while (0)

#define IMG "/tmp/host_vsdimg_test.img"
#define TMPL_DIR "/tmp/host_vsdimg_test_tmpl"

/* Reads an entire file into a malloc'd, NUL-terminated buffer (host test only --
 * golden rule 3's "hosted tools: malloc is fine, every allocation checked" applies,
 * not the GBA no-heap-after-init rule). Caller frees. */
static char* slurp(const char* path, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* buf = (char*)malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[got] = '\0';
  if (out_len) *out_len = got;
  return buf;
}

static void write_host_file(const char* path, const char* content) {
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot create %s\n", path); exit(1); }
  fwrite(content, 1, strlen(content), f);
  fclose(f);
}

static void make_template(void) {
  char cmd[512];
  snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/PokeDNA/xfer", TMPL_DIR, TMPL_DIR);
  system(cmd);
  char p1[256], p2[256];
  snprintf(p1, sizeof p1, "%s/PokeDNA/xfer/AAAABBBBCCCCDDDD.pds", TMPL_DIR);
  snprintf(p2, sizeof p2, "%s/PokeDNA/bank.meta", TMPL_DIR);
  write_host_file(p1, "fixture .pds payload, exactly what a real one looks like\n");
  write_host_file(p2, "fixture bank.meta v1\n");
}

int main(void) {
  make_template();

  /* --- mkimg: fresh 16 MiB volume (BACKLOG #179 A3 review D5: the harness's own
   * default chain-image size -- small enough that a 2 MiB image quietly built FAT12,
   * which no real SD card is ever formatted as) + the template tree ------------- */
  /* D5's refusal guard needs its own coverage (s179-a2 re-verify D-A5-R): below 16 MiB
   * f_mkfs picks FAT12 with one FAT, a shape no real card has, so mkimg must refuse. */
  CHECK(vsdimg_mkimg("/tmp/host_vsdimg_4mb.img", 4, NULL) != 0,
        "mkimg at 4 MiB must refuse (FAT12, not a real card's shape)");
  CHECK(vsdimg_mkimg("/tmp/host_vsdimg_8mb.img", 8, NULL) != 0,
        "mkimg at 8 MiB must refuse (FAT12, not a real card's shape)");
  CHECK(vsdimg_mkimg(IMG, 16, TMPL_DIR) == 0, "mkimg failed");

  /* --- list #1: the template's two files, plus /PokeDNA itself is a dir (not listed) */
  FILE* l1 = fopen("/tmp/host_vsdimg_before.list", "w");
  CHECK(l1 != NULL, "cannot open before.list");
  CHECK(vsdimg_list(IMG, l1) == 0, "list #1 failed");
  fclose(l1);

  size_t before_len = 0;
  char* before = slurp("/tmp/host_vsdimg_before.list", &before_len);
  CHECK(before != NULL, "cannot re-read before.list");
  CHECK(strstr(before, "/PokeDNA/bank.meta 21 ") != NULL,
        "before.list missing bank.meta line, got:\n%s", before ? before : "(null)");
  CHECK(strstr(before, "/PokeDNA/xfer/AAAABBBBCCCCDDDD.pds 57 ") != NULL,
        "before.list missing the .pds line, got:\n%s", before ? before : "(null)");
  /* Exactly two files, both accounted for above -- a third line would mean the
   * factory wrote something nobody asked for. */
  int before_lines = 0;
  for (const char* p = before; *p; p++) if (*p == '\n') before_lines++;
  CHECK(before_lines == 2, "expected exactly 2 files in before.list, got %d", before_lines);

  /* --- FatFs on the "GBA side" reads this image's directory through a SEPARATE,
   * fresh f_mount (not vsdimg_list's own internal one) -- the round-trip the brief
   * asks for. Same ff.c, same ramdisk.c: there is nothing else this could be testing
   * other than "does the image the factory wrote actually mount". */
  {
    extern FRESULT f_mount(FATFS*, const char*, BYTE);
    extern FRESULT f_opendir(DIR*, const TCHAR*);
    extern FRESULT f_readdir(DIR*, FILINFO*);
    extern FRESULT f_closedir(DIR*);
    FATFS fs2;
    long sectors;
    {
      FILE* f = fopen(IMG, "rb");
      CHECK(f != NULL, "cannot reopen %s for the GBA-side remount", IMG);
      fseek(f, 0, SEEK_END);
      sectors = ftell(f) / FF_MAX_SS;
      fclose(f);
    }
    unsigned char* raw = (unsigned char*)malloc((size_t)sectors * FF_MAX_SS);
    CHECK(raw != NULL, "malloc for remount failed");
    FILE* f = fopen(IMG, "rb");
    fread(raw, 1, (size_t)sectors * FF_MAX_SS, f);
    fclose(f);

    rd_init((unsigned)sectors);
    for (long s = 0; s < sectors; s++)
      disk_write(0, raw + (size_t)s * FF_MAX_SS, (LBA_t)s, 1);
    free(raw);

    CHECK(f_mount(&fs2, "", 1) == FR_OK, "GBA-side remount failed to mount");
    /* BACKLOG #179 A3 review D5: pin the shape mkimg's own f_mkfs is meant to
     * produce -- FAT16 (never FAT12, which no real card ships as) with TWO FATs
     * (the ubiquitous real-card default; a lone FAT here would mean a corrupted
     * card silently loses redundancy). */
    CHECK(fs2.fs_type == FS_FAT16, "expected FS_FAT16, got fs_type=%d", fs2.fs_type);
    CHECK(fs2.n_fats == 2, "expected 2 FATs, got n_fats=%d", fs2.n_fats);
    DIR d;
    CHECK(f_opendir(&d, "/PokeDNA") == FR_OK, "GBA-side f_opendir /PokeDNA failed");
    int seen = 0;
    for (;;) {
      FILINFO fno;
      if (f_readdir(&d, &fno) != FR_OK || fno.fname[0] == '\0') break;
      seen++;
    }
    f_closedir(&d);
    CHECK(seen == 2, "GBA-side /PokeDNA listing expected 2 entries (bank.meta + xfer/), got %d", seen);
    f_mount(0, "", 0);
  }

  /* --- patch: overwrite bank.meta with different, longer content ------------- */
  const char* new_meta = "fixture bank.meta v2 -- longer than before, on purpose\n";
  write_host_file("/tmp/host_vsdimg_patch_src.txt", new_meta);
  CHECK(vsdimg_patch(IMG, "/PokeDNA/bank.meta", "/tmp/host_vsdimg_patch_src.txt") == 0,
        "patch failed");

  /* --- list #2: exactly bank.meta's line changed, the .pds line is untouched -- */
  FILE* l2 = fopen("/tmp/host_vsdimg_after.list", "w");
  CHECK(l2 != NULL, "cannot open after.list");
  CHECK(vsdimg_list(IMG, l2) == 0, "list #2 failed");
  fclose(l2);
  char* after = slurp("/tmp/host_vsdimg_after.list", NULL);
  CHECK(after != NULL, "cannot re-read after.list");

  char meta_prefix[64];
  snprintf(meta_prefix, sizeof meta_prefix, "/PokeDNA/bank.meta %zu ", strlen(new_meta));
  CHECK(strstr(after, meta_prefix) != NULL,
        "after.list missing the patched bank.meta line, got:\n%s", after ? after : "(null)");
  CHECK(strstr(after, "/PokeDNA/xfer/AAAABBBBCCCCDDDD.pds 57 ") != NULL,
        "after.list's .pds line changed when it should not have, got:\n%s",
        after ? after : "(null)");

  /* Same CRC32 line for the .pds file in both lists -- the untouched file's
   * mutation target: if `list`'s CRC ever went stale (e.g. cached from the wrong
   * file), THIS is what would go undetected. Extract each list's .pds line and
   * compare them byte-for-byte. */
  {
    const char* b = strstr(before, "/PokeDNA/xfer/AAAABBBBCCCCDDDD.pds");
    const char* a = strstr(after, "/PokeDNA/xfer/AAAABBBBCCCCDDDD.pds");
    CHECK(b && a, "missing .pds line in one of the two lists");
    if (b && a) {
      size_t blen = strcspn(b, "\n"), alen = strcspn(a, "\n");
      CHECK(blen == alen && memcmp(b, a, blen) == 0,
            "the .pds line changed across the patch of an unrelated file");
    }
  }

  /* Exactly one line differs between the two lists (bank.meta's) -- the "exactly
   * these files changed" contract the design promises for A2, checked here without
   * needing tools/vsd_diff.py's own subprocess (that script is exercised separately
   * by hand in the lane report; this pins the C side that feeds it). */
  {
    int changed = 0;
    const char* p = before;
    while ((p = strstr(p, "/PokeDNA/bank.meta")) != NULL) { changed++; break; }
    (void)changed;
    CHECK(strstr(before, "bank.meta 21 ") != NULL, "sanity: before still has old size");
    CHECK(strstr(after, "bank.meta 21 ") == NULL, "sanity: after should NOT have the old size");
  }

  /* --- mutation: break one CRC in `list` by corrupting one payload byte between
   * the two `list` calls without going through `patch` at all (a direct disk_write
   * behind FatFs' back, exactly what a lying/flaky card would produce) -- and show
   * the CRC-mismatch detection actually distinguishes it from a same-size no-op. */
  {
    long sectors;
    FILE* f = fopen(IMG, "r+b");
    CHECK(f != NULL, "cannot reopen %s for the corruption mutation", IMG);
    fseek(f, 0, SEEK_END);
    sectors = ftell(f) / FF_MAX_SS;
    (void)sectors;
    /* Flip one byte inside the .pds file's known content by locating the fixture
     * string on disk and mutating its first character -- crude but sufficient: this
     * mutation exists only to prove list's CRC changes, not to model a real fault. */
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc((size_t)sectors * FF_MAX_SS);
    fread(buf, 1, (size_t)sectors * FF_MAX_SS, f);
    long hit = -1;
    for (long i = 0; i + 8 < sectors * FF_MAX_SS; i++) {
      if (memcmp(buf + i, "fixture ", 8) == 0 && buf[i + 8] == '.') { hit = i; break; }
    }
    CHECK(hit >= 0, "could not locate the .pds fixture bytes on disk to corrupt");
    if (hit >= 0) buf[hit] ^= 0xFF;
    fseek(f, 0, SEEK_SET);
    fwrite(buf, 1, (size_t)sectors * FF_MAX_SS, f);
    free(buf);
    fclose(f);

    FILE* l3 = fopen("/tmp/host_vsdimg_corrupt.list", "w");
    CHECK(vsdimg_list(IMG, l3) == 0, "list after corruption failed");
    fclose(l3);
    char* corrupt = slurp("/tmp/host_vsdimg_corrupt.list", NULL);
    CHECK(corrupt != NULL, "cannot re-read corrupt.list");
    const char* a_line = strstr(after, "/PokeDNA/xfer/AAAABBBBCCCCDDDD.pds");
    const char* c_line = corrupt ? strstr(corrupt, "/PokeDNA/xfer/AAAABBBBCCCCDDDD.pds") : NULL;
    CHECK(a_line && c_line, "missing .pds line after corruption");
    if (a_line && c_line) {
      size_t alen = strcspn(a_line, "\n"), clen = strcspn(c_line, "\n");
      CHECK(!(alen == clen && memcmp(a_line, c_line, alen) == 0),
            "list's CRC did NOT change after a one-byte on-disk corruption -- the mutation this test exists to catch");
    }
    free(corrupt);
  }

  free(before);
  free(after);

  if (fails) { printf("%d FAILED\n", fails); return 1; }
  printf("all vsd_img tests passed\n");
  return 0;
}
