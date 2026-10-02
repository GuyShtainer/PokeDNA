/* SPDX-License-Identifier: GPL-3.0-or-later
 * bank_layout.c -- see bank_layout.h (BACKLOG #371). Pure C: FatFs + savefile only. */
#include "bank_layout.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "savefile.h"

/* Path buffers are 48 B: "/PokeDNA/bank/bank.meta.bak" is 27, a box path 22. */
#define BML_PATH 48
#define BML_DIR_MAX 20   /* + "/bank.meta.bak" (14) + NUL fits BML_PATH */

static bool present(const char* path) {
  return f_stat(path, 0) == FR_OK;
}

static bool any_box(const char* dir, int nboxes) {
  char p[BML_PATH];
  for (int b = 0; b < nboxes && b < 99; b++) {
    siprintf(p, "%s/box%02d.box", dir, b);
    if (present(p)) return true;
  }
  return false;
}

bool __attribute__((noinline)) bml_layout_exists(const char* dir, int nboxes) {
  char p[BML_PATH];
  if (!dir || nboxes < 0 || strlen(dir) > BML_DIR_MAX) return false;
  siprintf(p, "%s/bank.meta", dir);
  if (present(p)) return true;
  siprintf(p, "%s/bank.meta.bak", dir);
  if (present(p)) return true;
  return any_box(dir, nboxes);
}

static bool read_ok(const char* path, uint8_t* buf, uint32_t cap, uint32_t need, const char* magic) {
  uint32_t sz = 0;
  return sf_read_full(path, buf, cap, &sz) == SF_OK && sz >= need && memcmp(buf, magic, 6) == 0;
}

BmlSource __attribute__((noinline)) bml_meta_read(const char* dir, int nboxes, uint8_t* buf,
                                                   uint32_t cap, uint32_t need, const char* magic) {
  char p[BML_PATH];
  if (!dir || !buf || !magic || cap < need || strlen(dir) > BML_DIR_MAX) return BML_NONE_EMPTY;
  siprintf(p, "%s/bank.meta", dir);
  if (read_ok(p, buf, cap, need, magic)) return BML_PRIMARY;
  FRESULT pr = f_stat(p, 0);
  bool absent = pr == FR_NO_FILE || pr == FR_NO_PATH;
  siprintf(p, "%s/bank.meta.bak", dir);
  if (read_ok(p, buf, cap, need, magic)) return absent ? BML_BAK_PRIMARY_ABSENT : BML_BAK_PRIMARY_BAD;
  return any_box(dir, nboxes) ? BML_NONE_BOXES : BML_NONE_EMPTY;
}

/* ---- BACKLOG #378: box files ---- */
typedef enum { BX_OK, BX_ABSENT, BX_SHORT, BX_ERR } BxProbe;

static BxProbe box_probe(const char* path, uint8_t* buf, uint32_t need, uint32_t* sz) {
  FRESULT fr = f_stat(path, 0);
  if (fr == FR_NO_FILE || fr == FR_NO_PATH) return BX_ABSENT;
  if (fr != FR_OK) return BX_ERR;                 /* a card fault is not "no such file" */
  *sz = 0;
  if (sf_read_full(path, buf, need, sz) != SF_OK) return BX_ERR;
  return *sz >= need ? BX_OK : BX_SHORT;
}

BmlBoxSrc __attribute__((noinline)) bml_box_read(const char* path, uint8_t* buf, uint32_t need,
                                                  uint32_t* out_sz) {
  char p[BML_PATH];
  uint32_t sz = 0;
  if (!path || !buf || !out_sz || need == 0 || strlen(path) > BML_PATH - 6) return BML_BOX_READ_ERROR;
  BxProbe r = box_probe(path, buf, need, &sz);
  *out_sz = sz;
  if (r == BX_OK) return BML_BOX_PRIMARY;
  if (r == BX_ERR) return BML_BOX_READ_ERROR;
  if (r == BX_SHORT) return BML_BOX_BAD;
  siprintf(p, "%s.tmp", path);
  r = box_probe(p, buf, need, &sz);
  *out_sz = sz;
  if (r == BX_OK) return BML_BOX_TMP;
  if (r == BX_ERR) return BML_BOX_READ_ERROR;
  siprintf(p, "%s.bak", path);
  r = box_probe(p, buf, need, &sz);
  *out_sz = sz;
  if (r == BX_OK) return BML_BOX_BAK;
  if (r == BX_ERR) return BML_BOX_READ_ERROR;
  memset(buf, 0, need);                            /* a short/partial scratch must not leak */
  *out_sz = 0;
  return BML_BOX_NONE;
}

/* Does `path` exist at exactly `len` bytes? (own frame: FILINFO carries an LFN buffer) */
static bool __attribute__((noinline)) size_is(const char* path, uint32_t len) {
  FILINFO fno;
  return f_stat(path, &fno) == FR_OK && (uint32_t)fno.fsize == len;
}

bool __attribute__((noinline)) bml_box_heal(const char* path, BmlBoxSrc src, const uint8_t* bytes,
                                             uint32_t len) {
  char t[BML_PATH];
  if (!path || !bytes || len == 0 || strlen(path) > BML_PATH - 6) return false;
  if (src == BML_BOX_BAK) return sf_write_verified(path, bytes, len) == SF_OK && size_is(path, len);
  if (src != BML_BOX_TMP) return false;
  siprintf(t, "%s.tmp", path);
  if (f_rename(t, path) != FR_OK) return false;    /* the verified .tmp stays where it is */
  return size_is(path, len) && f_stat(t, 0) == FR_NO_FILE;   /* swap read back, tmp gone */
}
