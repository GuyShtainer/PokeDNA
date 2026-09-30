/* Shared harness for the journal host tests (host_journal_test.c, host_journal_cut_test.c):
 * a RAM-disk card in FAT16 / FAT32 / exFAT, a 14 x 3,968 B in-RAM image behind a JrnImage,
 * a co-resident 128 KiB .sav, and helpers that stage a step the way the slice-2 funnel will
 * (diff old/new region blocks). Included, never linked: every function is static. */
#ifndef JRN_HARNESS_H
#define JRN_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "journal.h"
#include "journal_fs.h"
#include "ramdisk.h"

#define NREG 14
#define RSZ  3968u
#define ROOT "/PokeDNA/journal"
#define SAVP "/PokeDNA/emerald.sav"
#define SAVN 131072u

static int fails = 0;
static unsigned long checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static FATFS s_fs;
static BYTE  s_work[FF_MAX_SS * 8];
static uint8_t g_img[NREG][RSZ];
static uint8_t s_sav[SAVN];

static int img_get(void* c, uint8_t r, uint16_t off, uint8_t* d, uint16_t n) {
  (void)c; if (r >= NREG || off + n > RSZ) return -1; memcpy(d, g_img[r] + off, n); return 0;
}
static int img_set(void* c, uint8_t r, uint16_t off, const uint8_t* s, uint16_t n) {
  (void)c; if (r >= NREG || off + n > RSZ) return -1; memcpy(g_img[r] + off, s, n); return 0;
}
static const JrnImage IMG = { 0, img_get, img_set };

static void img_fill(unsigned seed) {
  unsigned r, i;
  for (r = 0; r < NREG; r++)
    for (i = 0; i < RSZ; i++) g_img[r][i] = (uint8_t)(i * 31u + r * 17u + seed * 7u + (i >> 8));
}

static void card_remount(void) {
  f_mount(0, "", 0);
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "remount failed");
}

/* fmt: FM_FAT (FAT16 here), FM_FAT32, FM_EXFAT. */
static void card_fresh(BYTE fmt) {
  unsigned sectors = fmt == FM_FAT32 ? 70000u : 24000u;
  MKFS_PARM opt = { fmt | FM_SFD, 1, 1, 0, 512 };
  FIL f; UINT bw = 0; unsigned i;
  f_mount(0, "", 0);
  rd_init(sectors);
  CHECK(f_mkfs("", &opt, s_work, sizeof s_work) == FR_OK, "f_mkfs %d", (int)fmt);
  CHECK(f_mount(&s_fs, "", 1) == FR_OK, "f_mount");
  CHECK(f_mkdir("/PokeDNA") == FR_OK, "mkdir /PokeDNA");
  for (i = 0; i < SAVN; i++) s_sav[i] = (uint8_t)(i * 13u + (i >> 9) + 5u);
  CHECK(f_open(&f, SAVP, FA_WRITE | FA_CREATE_NEW) == FR_OK, "create sav");
  CHECK(f_write(&f, s_sav, SAVN, &bw) == FR_OK && bw == SAVN, "write sav");
  CHECK(f_close(&f) == FR_OK, "close sav");
}

static int sav_intact(void) {
  static uint8_t rb[SAVN];
  FIL f; UINT br = 0;
  if (f_open(&f, SAVP, FA_READ) != FR_OK) return 0;
  if (f_read(&f, rb, SAVN, &br) != FR_OK || br != SAVN) { f_close(&f); return 0; }
  f_close(&f);
  return memcmp(rb, s_sav, SAVN) == 0;
}

static JrnCfg cfg_for(uint64_t key, uint8_t max_segs, uint8_t readonly) {
  JrnCfg c;
  memset(&c, 0, sizeof c);
  c.fs = &jrn_fatfs; c.root = ROOT; c.key = key; c.nreg = NREG; c.reg_size = RSZ;
  c.max_segs = max_segs; c.readonly = readonly;
  return c;
}

static int jopen(Jrn* j, uint64_t key) { JrnCfg c = cfg_for(key, 0, 0); return jrn_open(j, &c, &IMG); }
static int jopen_ro(Jrn* j, uint64_t key) { JrnCfg c = cfg_for(key, 0, 1); return jrn_open(j, &c, &IMG); }

/* Stage one step the way the funnel will: change `n` bytes of one region to `val`, hand the
 * engine the old and new blocks. Returns the engine's verdict of _end (JRN_OK / JRN_NOOP). */
static int stage(Jrn* j, const char* name, int crossed, uint8_t reg, uint16_t off, uint16_t n, uint8_t val) {
  static uint8_t old_b[RSZ];
  int rc;
  memcpy(old_b, g_img[reg], RSZ);
  memset(g_img[reg] + off, val, n);
  rc = jrn_step_begin(j, name, crossed);
  if (!rc) rc = jrn_step_region(j, reg, old_b, g_img[reg]);
  if (!rc) rc = jrn_step_end(j);
  if (rc) { jrn_step_abort(j); memcpy(g_img[reg], old_b, RSZ); }   /* the funnel would not copy in */
  return rc;
}

/* The same bytes the reference says the hash of the current image should be. */
static uint32_t ref_hash(void) {
  uint8_t b[4 * NREG];
  unsigned r;
  for (r = 0; r < NREG; r++) {
    uint32_t c = jrn_crc32_update(0, g_img[r], RSZ);
    b[4 * r] = (uint8_t)c; b[4 * r + 1] = (uint8_t)(c >> 8); b[4 * r + 2] = (uint8_t)(c >> 16); b[4 * r + 3] = (uint8_t)(c >> 24);
  }
  return jrn_crc32_update(0, b, sizeof b);
}

/* A value that differs from what `n` bytes at reg/off hold now (so the step is never a no-op). */
static uint8_t fresh_val(uint8_t reg, uint16_t off) { return (uint8_t)(g_img[reg][off] + 1u); }

/* Write raw bytes into a segment file in place (test-side tampering: tails, flips). */
static int raw_write(uint64_t key, unsigned seg, uint32_t off, const void* buf, uint32_t n) {
  char p[96], hex[17];
  jrn_key_hex(key, hex);
  snprintf(p, sizeof p, ROOT "/%s/%04u.pdj", hex, seg);
  return jrn_fatfs.write(0, p, off, buf, n);
}
static int raw_read(uint64_t key, unsigned seg, uint32_t off, void* buf, uint32_t n) {
  char p[96], hex[17];
  jrn_key_hex(key, hex);
  snprintf(p, sizeof p, ROOT "/%s/%04u.pdj", hex, seg);
  return jrn_fatfs.read(0, p, off, buf, n);
}

/* A record's stable fingerprint: everything durable a reader can see of the journal. */
static uint32_t jr_fingerprint(uint64_t key, char* dbg, size_t dbgn) {
  Jrn j;
  uint8_t b[JRN_SEG_SIZE > 4096 ? 4096 : JRN_SEG_SIZE];
  uint8_t hdr[32];
  uint32_t h = 0, n;
  char p[96], hex[17];
  FIL f; UINT br = 0;
  if (jopen_ro(&j, key) != JRN_OK) { if (dbg) snprintf(dbg, dbgn, "open-failed"); return 0xDEAD0001u; }
  hdr[0] = (uint8_t)j.seg_first; hdr[1] = (uint8_t)j.seg_last; hdr[2] = (uint8_t)j.tail_seg;
  hdr[3] = (uint8_t)(j.tail_off); hdr[4] = (uint8_t)(j.tail_off >> 8); hdr[5] = (uint8_t)(j.tail_off >> 16);
  hdr[6] = (uint8_t)j.next_seq; hdr[7] = (uint8_t)(j.next_seq >> 8);
  h = jrn_crc32_update(h, hdr, 8);
  if (j.tail_seg) {                       /* the valid prefix of the tail segment, byte for byte */
    jrn_key_hex(j.key, hex);
    snprintf(p, sizeof p, ROOT "/%s/%04u.pdj", hex, j.tail_seg);
    if (f_open(&f, p, FA_READ) == FR_OK) {
      for (n = 0; n < j.tail_off;) {
        uint32_t m = j.tail_off - n < sizeof b ? j.tail_off - n : (uint32_t)sizeof b;
        f_lseek(&f, n); br = 0;
        if (f_read(&f, b, m, &br) != FR_OK || br != m) break;
        h = jrn_crc32_update(h, b, m); n += m;
      }
      f_close(&f);
    }
  }
  if (dbg) snprintf(dbg, dbgn, "segs %u..%u tail %u@%u next_seq %u", j.seg_first, j.seg_last, j.tail_seg, (unsigned)j.tail_off, (unsigned)j.next_seq);
  return h;
}

#endif /* JRN_HARNESS_H */
