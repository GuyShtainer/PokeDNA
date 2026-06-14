/*
 * pdna_bank — the external bank as a parallel set of 16 PC-style boxes.
 *
 * Storage on the SD card under /pokedna/bank/:
 *   boxNN.box   one file per box = 30 * 80 = 2400 raw box-mon records (NN = 00..15)
 *   bank.meta   16-byte header + 16 * (9-byte ASCII name + 1 wallpaper byte)
 *
 * Only ONE box (2400 B) is held in RAM at a time (EWRAM is tight — the in-save PC
 * already costs 35 KB), so boxes page in/out of the box files. The shared box screen
 * (pdna_box) drives it through a BoxSource; mons move in/out of the save via the
 * universal copy/paste clipboard. Writes are verified (.tmp -> re-read -> rename) and
 * EZ-Flash-Omega-only; read-only carts browse + copy but never write.
 */
#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"            /* EWRAM_BSS (after tonc.h) */
#include "ff.h"
#include "savefile.h"
#include "gen3_box.h"       /* G3_IN_BOX, G3_BOX_WALLPAPER_COUNT */
#include "gen3_clip.h"      /* pk3_validate */
#include "gen3_mon.h"
#include "pdna_box.h"     /* BoxSource */
#include "pdna_app.h"     /* app_can_edit, app_confirm */
#include "pdna_pk.h"      /* PDNA_BANK_DIR */
#include "pdna_bank.h"
#include "ui.h"
#include "snd.h"

#define BANK_BOXES   16
#define BOX_RECS     G3_IN_BOX            /* 30 */
#define REC_BYTES    80
#define BOX_BYTES    (BOX_RECS * REC_BYTES)   /* 2400 */
#define META_MAGIC   "PKVBNK"
#define META_VERSION 1
#define META_HDR     16
#define META_BYTES   (META_HDR + BANK_BOXES * 10)   /* 16 + 160 = 176 */

/* current box, pc-mini-layout: 4-byte header then 30 records (so app_mon_menu's
 * pk_box_slot(block, 0, slot) lands on record `slot`). */
static uint8_t EWRAM_BSS g_bankbuf[0x0004 + BOX_BYTES];
static int  g_loaded = -1;                 /* which box g_bankbuf holds, or -1     */
static bool g_dirty  = false;              /* loaded box has unsaved deferred moves */
static struct { uint8_t name[9]; uint8_t wp; } g_meta[BANK_BOXES];

static uint8_t* box_recs(void) { return g_bankbuf + 0x0004; }

/* ---- paths ---- */
static void box_path(int box, char* out) { siprintf(out, PDNA_BANK_DIR "/box%02d.box", box); }
static const char* meta_path(void) { return PDNA_BANK_DIR "/bank.meta"; }

/* ---- metadata ---- */
static void meta_defaults(void) {
  for (int b = 0; b < BANK_BOXES; b++) {
    siprintf((char*)g_meta[b].name, "BANK %d", b + 1);
    g_meta[b].wp = (uint8_t)(b % G3_BOX_WALLPAPER_COUNT);   /* each box a different look */
  }
}

static bool meta_load(void) {
  uint8_t buf[META_BYTES]; uint32_t sz = 0;
  if (sf_read_full(meta_path(), buf, sizeof buf, &sz) != SF_OK || sz < META_BYTES ||
      memcmp(buf, META_MAGIC, 6) != 0) { meta_defaults(); return false; }
  for (int b = 0; b < BANK_BOXES; b++) {
    const uint8_t* p = buf + META_HDR + b * 10;
    memcpy(g_meta[b].name, p, 9); g_meta[b].name[8] = 0;
    g_meta[b].wp = p[9] % G3_BOX_WALLPAPER_COUNT;
  }
  return true;
}

static bool meta_save(void) {
  uint8_t buf[META_BYTES];
  memset(buf, 0, sizeof buf);
  memcpy(buf, META_MAGIC, 6);
  buf[8] = META_VERSION; buf[9] = BANK_BOXES;
  for (int b = 0; b < BANK_BOXES; b++) {
    uint8_t* p = buf + META_HDR + b * 10;
    memcpy(p, g_meta[b].name, 9);
    p[9] = g_meta[b].wp;
  }
  return sf_write_verified(meta_path(), buf, META_BYTES) == SF_OK;
}

/* ---- box files ---- */
static void box_load(int box) {                 /* read box file -> g_bankbuf (empty if absent) */
  char path[SF_PATH_MAX]; box_path(box, path);
  uint32_t sz = 0;
  memset(g_bankbuf, 0, sizeof g_bankbuf);
  sf_read_full(path, box_recs(), BOX_BYTES, &sz);  /* short/absent -> zeroed (empty box) */
  g_loaded = box;
  g_dirty = false;
}

static bool box_save(void) {                    /* write the loaded box's records */
  if (g_loaded < 0) return false;
  char path[SF_PATH_MAX]; box_path(g_loaded, path);
  bool ok = sf_write_verified(path, box_recs(), BOX_BYTES) == SF_OK;
  if (ok) g_dirty = false;
  return ok;
}

/* ---- one-time migration from the old flat /pokedna/bank/*.pk3 layout ---- */
static bool has_pk_ext(const char* n) {
  int L = (int)strlen(n);
  if (L >= 4 && n[L-4]=='.' && (n[L-3]=='p'||n[L-3]=='P') && (n[L-2]=='k'||n[L-2]=='K') && n[L-1]=='3') return true;
  if (L >= 3 && n[L-3]=='.' && (n[L-2]=='p'||n[L-2]=='P') && (n[L-1]=='k'||n[L-1]=='K')) return true;
  return false;
}

/* Pack existing .pk3 files into box files in directory order. Non-destructive: the
 * .pk3 files stay; they're simply no longer the store. Omega-only (it writes). */
static void migrate_flat_pk3(void) {
  meta_defaults();
  int box = 0, slot = 0, packed = 0;
  bool box_open = false;
  memset(g_bankbuf, 0, sizeof g_bankbuf);
  g_loaded = 0;

  DIR d; FILINFO fno;
  if (f_opendir(&d, PDNA_BANK_DIR) == FR_OK) {
    while (box < BANK_BOXES && f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
      if ((fno.fattrib & AM_DIR) || !has_pk_ext(fno.fname)) continue;
      char path[SF_PATH_MAX]; siprintf(path, PDNA_BANK_DIR "/%s", fno.fname);
      uint8_t rec[REC_BYTES]; uint32_t sz = 0;
      if (sf_read_full(path, rec, REC_BYTES, &sz) != SF_OK || sz < REC_BYTES || !pk3_validate(rec)) continue;
      memcpy(box_recs() + slot * REC_BYTES, rec, REC_BYTES);
      box_open = true; packed++;
      if (++slot >= BOX_RECS) {                  /* box full -> flush, next box */
        g_loaded = box; box_save();
        box++; slot = 0; box_open = false;
        memset(g_bankbuf, 0, sizeof g_bankbuf);
      }
    }
    f_closedir(&d);
  }
  if (box_open && box < BANK_BOXES) { g_loaded = box; box_save(); }   /* flush the partial box */
  meta_save();
  (void)packed;
  g_loaded = -1;                                 /* force a fresh load on first records() */
}

/* True once the new layout has been initialised (bank.meta written) — so an empty
 * bank is migrated/initialised exactly once, not on every open. */
static bool layout_exists(void) {
  FILINFO fno;
  return f_stat(meta_path(), &fno) == FR_OK;
}

/* ---- BoxSource hooks (singleton state) ---- */
static uint8_t* banksrc_records(int box) {
  if (box != g_loaded) {
    if (g_dirty) box_save();                     /* flush deferred moves before paging out */
    box_load(box);
  }
  return box_recs();
}
static void banksrc_get_name(int box, char out[12]) {
  strncpy(out, (const char*)g_meta[box].name, 11); out[11] = 0;
}
static void banksrc_set_name(int box, const char* s) {
  int i = 0; for (; i < 8 && s[i]; i++) g_meta[box].name[i] = (uint8_t)s[i];
  g_meta[box].name[i] = 0;
}
static int  banksrc_get_wp(int box) { return g_meta[box].wp; }
static void banksrc_set_wp(int box, int wp) {
  if (wp < 0) wp = 0; if (wp >= G3_BOX_WALLPAPER_COUNT) wp = G3_BOX_WALLPAPER_COUNT - 1;
  g_meta[box].wp = (uint8_t)wp;
}
static bool banksrc_can_edit(void) { return app_can_edit(); }
static bool banksrc_commit(void) {               /* immediate edits: persist box + meta */
  bool ok = box_save();
  meta_save();
  return ok;
}
static void banksrc_mark_dirty(void) { g_dirty = true; }   /* moves: deferred to box-switch/exit */

bool pdna_bank_show(void) {
  f_mkdir("/pokedna");
  f_mkdir(PDNA_BANK_DIR);

  if (app_can_edit() && !layout_exists()) migrate_flat_pk3();   /* first run: import old .pk3 */
  meta_load();
  g_loaded = -1; g_dirty = false;

  BoxSource s; memset(&s, 0, sizeof s);
  s.nboxes     = BANK_BOXES;
  s.start_box  = 0;
  s.is_bank    = true;
  s.wp_count   = G3_BOX_WALLPAPER_COUNT;          /* bank has no Walda */
  s.records    = banksrc_records;
  s.menu_block = g_bankbuf;                        /* records at +0x0004; menu box index = 0 */
  s.get_name   = banksrc_get_name;
  s.set_name   = banksrc_set_name;
  s.get_wp     = banksrc_get_wp;
  s.set_wp     = banksrc_set_wp;
  s.can_edit   = banksrc_can_edit;
  s.commit     = banksrc_commit;
  s.mark_dirty = banksrc_mark_dirty;

  pdna_box(&s);

  /* deferred moves: ask once on leaving the bank (mirrors the PC's save-on-exit). */
  if (g_dirty) {
    if (app_can_edit() && app_confirm("Save bank changes?", "Save the Pokemon you moved?"))
      box_save();
    else if (g_loaded >= 0)
      box_load(g_loaded);                          /* discard: reload the box from its file */
    g_dirty = false;
  }
  return true;
}
