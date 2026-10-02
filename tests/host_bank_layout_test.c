/* Host test for source/bank_layout.c (BACKLOG #371): whether a Bank already exists on the
 * card (bml_layout_exists) and where its bank.meta bytes come from (bml_meta_read). The REAL
 * functions over the REAL lib/fatfs on a RAM disk.
 *
 *   cc -std=c11 -DFF_USE_MKFS=1 -Dsiprintf=sprintf -Dsniprintf=snprintf -Dvsniprintf=vsnprintf -I tests/hostfat -I lib/fatfs -I source \
 *      tests/host_bank_layout_test.c source/bank_layout.c source/savefile.c source/log.c lib/fatfs/ff.c \
 *      lib/fatfs/ffunicode.c tests/hostfat/ramdisk.c -o /tmp/hbl
 *   /tmp/hbl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "bank_layout.h"
#include "ramdisk.h"

#define DIR_ "/PokeDNA/bank"
#define META DIR_ "/bank.meta"
#define BAK  META ".bak"
#define TMP  META ".tmp"
#define NEED 176u
#define MAGIC "PKVBNK"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 2];

static void fresh_card(void) {
  MKFS_PARM opt = { FM_FAT | FM_SFD, 1, 1, 0, 0 };
  f_mount(0, "", 0);
  rd_init(4096);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs failed");
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount failed");
  CHECK(f_mkdir("/PokeDNA") == FR_OK, "mkdir PokeDNA");
  CHECK(f_mkdir(DIR_) == FR_OK, "mkdir bank");
}

static void put(const char* path, const void* data, unsigned n) {
  FIL f; UINT bw = 0;
  CHECK(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK, "open %s", path);
  CHECK(f_write(&f, data, n, &bw) == FR_OK && bw == n, "write %s", path);
  f_close(&f);
}

static void meta_bytes(uint8_t* b, uint8_t tag) {
  memset(b, 0, NEED); memcpy(b, MAGIC, 6); b[8] = 2; b[9] = 16; b[16] = tag;
}

static BmlSource rd(uint8_t* out) { return bml_meta_read(DIR_, 16, out, NEED, NEED, MAGIC); }

int main(void) {
  uint8_t good[NEED], old[NEED], out[NEED], junk[NEED];
  meta_bytes(good, 0x11); meta_bytes(old, 0x22);
  memset(junk, 0xA5, sizeof junk);

  /* nothing at all: a card with no Bank -> "first run" (migrate may run) */
  fresh_card();
  CHECK(!bml_layout_exists(DIR_, 16), "empty card reported as an existing Bank");
  CHECK(rd(out) == BML_NONE_EMPTY, "empty card: expected NONE_EMPTY");

  /* a missing bank dir entirely is still "no Bank", not a crash */
  CHECK(!bml_layout_exists("/nonexistent", 16), "missing dir reported as a Bank");

  /* primary only */
  fresh_card(); put(META, good, NEED);
  CHECK(bml_layout_exists(DIR_, 16), "primary only: not an existing Bank");
  CHECK(rd(out) == BML_PRIMARY && memcmp(out, good, NEED) == 0, "primary only: wrong source/bytes");

  /* THE #371 CASE: primary absent, good .bak -> existing Bank, restore from .bak */
  fresh_card(); put(BAK, old, NEED);
  CHECK(bml_layout_exists(DIR_, 16), "#371: missing primary + good .bak treated as first run");
  CHECK(rd(out) == BML_BAK_PRIMARY_ABSENT, "#371: expected BAK_PRIMARY_ABSENT");
  CHECK(memcmp(out, old, NEED) == 0, "#371: restored bytes are not the .bak bytes");

  /* corrupt primary + good .bak -> BAK_PRIMARY_BAD (the pre-existing #219a path) */
  fresh_card(); put(META, junk, NEED); put(BAK, old, NEED);
  CHECK(rd(out) == BML_BAK_PRIMARY_BAD && memcmp(out, old, NEED) == 0, "corrupt primary: wrong source");
  /* a short primary is corrupt too */
  fresh_card(); put(META, good, 100); put(BAK, old, NEED);
  CHECK(rd(out) == BML_BAK_PRIMARY_BAD, "short primary: wrong source");

  /* both good: primary wins */
  fresh_card(); put(META, good, NEED); put(BAK, old, NEED);
  CHECK(rd(out) == BML_PRIMARY && memcmp(out, good, NEED) == 0, "both good: primary must win");

  /* neither meta file, but a box file: an existing Bank, defaults in RAM, NOT first run */
  fresh_card(); put(DIR_ "/box07.box", junk, 64);
  CHECK(bml_layout_exists(DIR_, 16), "#371: boxes without any meta treated as first run");
  CHECK(rd(out) == BML_NONE_BOXES, "#371: expected NONE_BOXES");
  /* box15 is the last scanned; box16 is out of range */
  fresh_card(); put(DIR_ "/box15.box", junk, 64);
  CHECK(bml_layout_exists(DIR_, 16), "box15 not scanned");
  fresh_card(); put(DIR_ "/box16.box", junk, 64);
  CHECK(!bml_layout_exists(DIR_, 16), "box16 (out of range) counted");

  /* a leftover .tmp is never trusted as data and never counts as a Bank */
  fresh_card(); put(TMP, good, NEED);
  CHECK(!bml_layout_exists(DIR_, 16), "lone bank.meta.tmp counted as a Bank");
  CHECK(rd(out) == BML_NONE_EMPTY, "lone bank.meta.tmp read as meta data");
  fresh_card(); put(TMP, good, NEED); put(BAK, old, NEED);
  CHECK(rd(out) == BML_BAK_PRIMARY_ABSENT && memcmp(out, old, NEED) == 0, ".tmp preferred over .bak");

  /* a box .bak / other stray files alone do not count */
  fresh_card(); put(DIR_ "/box00.box.bak", junk, 64);
  CHECK(!bml_layout_exists(DIR_, 16), "box00.box.bak alone counted as a Bank");

  /* argument validation */
  CHECK(!bml_layout_exists(NULL, 16), "NULL dir");
  CHECK(bml_meta_read(DIR_, 16, out, 10, NEED, MAGIC) == BML_NONE_EMPTY, "cap < need accepted");

  if (fails) { printf("host_bank_layout_test: %d FAILED\n", fails); return 1; }
  printf("host_bank_layout_test: all passed\n");
  return 0;
}
