#include "savefile.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "log.h"
#include "sys.h"   /* EWRAM_BSS */

/* Shared 4 KiB compare/copy chunk in EWRAM (.bss), never on the IWRAM stack. */
static uint8_t EWRAM_BSS s_cmp[4096];

const char* sf_status_str(SfStatus s) {
  switch (s) {
    case SF_OK:         return "OK";
    case SF_ERR_OPEN:   return "open failed";
    case SF_ERR_READ:   return "read error";
    case SF_ERR_WRITE:  return "write error";
    case SF_ERR_SIZE:   return "bad size";
    case SF_ERR_VERIFY: return "verify mismatch";
    case SF_ERR_BACKUP: return "backup failed";
    case SF_ERR_PARSE:  return "parse/validate failed";
    case SF_ERR_RENAME: return "rename failed";
    case SF_ERR_LAYOUT: return "layout error";
    default:            return "?";
  }
}

SfStatus sf_read_full(const char* path, uint8_t* buf, uint32_t cap,
                      uint32_t* out_size) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return SF_ERR_OPEN;
  UINT br = 0;
  FRESULT fr = f_read(&f, buf, cap, &br);
  f_close(&f);
  if (fr != FR_OK) return SF_ERR_READ;
  if (out_size) *out_size = (uint32_t)br;
  return SF_OK;
}

/* Compare two open files byte-for-byte using the shared compare buffer. */
static SfStatus files_equal(const char* a, const char* b, bool* equal) {
  *equal = false;
  FIL fa, fb;
  if (f_open(&fa, a, FA_READ) != FR_OK) return SF_ERR_OPEN;
  if (f_open(&fb, b, FA_READ) != FR_OK) { f_close(&fa); return SF_ERR_OPEN; }

  static uint8_t EWRAM_BSS bufb[4096];
  SfStatus st = SF_OK;
  bool same = true;
  if (f_size(&fa) != f_size(&fb)) same = false;

  while (same) {
    UINT ra = 0, rb = 0;
    if (f_read(&fa, s_cmp, sizeof(s_cmp), &ra) != FR_OK) { st = SF_ERR_READ; break; }
    if (f_read(&fb, bufb, sizeof(bufb), &rb) != FR_OK)   { st = SF_ERR_READ; break; }
    if (ra != rb) { same = false; break; }
    if (ra == 0) break; /* both EOF */
    if (memcmp(s_cmp, bufb, ra) != 0) { same = false; break; }
  }
  f_close(&fa);
  f_close(&fb);
  if (st != SF_OK) return st;
  *equal = same;
  return SF_OK;
}

/* Copy src -> dst in 4 KiB chunks. */
static SfStatus copy_file(const char* src, const char* dst) {
  FIL fs, fd;
  if (f_open(&fs, src, FA_READ) != FR_OK) return SF_ERR_OPEN;
  if (f_open(&fd, dst, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
    f_close(&fs);
    return SF_ERR_OPEN;
  }
  SfStatus st = SF_OK;
  for (;;) {
    UINT br = 0, bw = 0;
    if (f_read(&fs, s_cmp, sizeof(s_cmp), &br) != FR_OK) { st = SF_ERR_READ; break; }
    if (br == 0) break;
    if (f_write(&fd, s_cmp, br, &bw) != FR_OK || bw != br) { st = SF_ERR_WRITE; break; }
  }
  f_close(&fs);
  if (f_close(&fd) != FR_OK && st == SF_OK) st = SF_ERR_WRITE;
  return st;
}

static bool file_exists(const char* path) {
  FILINFO fno;
  return f_stat(path, &fno) == FR_OK;
}

SfStatus sf_backup(const char* src_path, char* out_bak, unsigned out_bak_cap) {
  char bak[SF_PATH_MAX];
  bool chosen = false;
  for (int n = 0; n <= 20 && !chosen; n++) {
    if (n == 0) siprintf(bak, "%s.bak", src_path);
    else        siprintf(bak, "%s.bak%d", src_path, n);
    if (!file_exists(bak)) chosen = true; /* never overwrite an existing backup */
  }
  if (!chosen) {
    log_line("backup: no free .bak slot (kept existing backups)");
    return SF_ERR_BACKUP;
  }

  SfStatus st = copy_file(src_path, bak);
  if (st != SF_OK) {
    log_line("backup: copy failed (%s)", sf_status_str(st));
    return SF_ERR_BACKUP;
  }
  bool eq = false;
  st = files_equal(src_path, bak, &eq);
  if (st != SF_OK || !eq) {
    log_line("backup: verify failed");
    return SF_ERR_BACKUP;
  }
  if (out_bak && out_bak_cap) {
    strncpy(out_bak, bak, out_bak_cap - 1);
    out_bak[out_bak_cap - 1] = 0;
  }
  log_line("backup OK -> %s", bak);
  return SF_OK;
}

SfStatus sf_write_verified(const char* path, const uint8_t* buf, uint32_t len) {
  char tmp[SF_PATH_MAX];
  siprintf(tmp, "%s.tmp", path);

  /* 1) write temp */
  FIL f;
  if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return SF_ERR_OPEN;
  UINT bw = 0;
  FRESULT fr = f_write(&f, buf, len, &bw);
  FRESULT fc = f_close(&f);
  if (fr != FR_OK || bw != len || fc != FR_OK) {
    f_unlink(tmp);
    return SF_ERR_WRITE;
  }

  /* 2) re-read temp and byte-compare to the intended buffer */
  if (f_open(&f, tmp, FA_READ) != FR_OK) { f_unlink(tmp); return SF_ERR_OPEN; }
  uint32_t off = 0;
  bool ok = true;
  while (off < len) {
    UINT br = 0;
    uint32_t want = len - off;
    if (want > sizeof(s_cmp)) want = sizeof(s_cmp);
    if (f_read(&f, s_cmp, want, &br) != FR_OK || br != want) { ok = false; break; }
    if (memcmp(s_cmp, buf + off, br) != 0) { ok = false; break; }
    off += br;
  }
  f_close(&f);
  if (!ok) { f_unlink(tmp); return SF_ERR_VERIFY; }

  /* 3) swap into place (original only disappears once temp is verified) */
  f_unlink(path); /* ignore error if absent */
  if (f_rename(tmp, path) != FR_OK) return SF_ERR_RENAME;
  return SF_OK;
}
