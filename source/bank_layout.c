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
