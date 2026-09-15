/*
 * pdna_bank — the external bank as a parallel set of 16 PC-style boxes.
 *
 * Storage on the SD card under /PokeDNA/bank/:
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
#include "pdna_origin_art.h"  /* the parallel era view: the bank is where all three meet */
#include "log.h"           /* log_line (BACKLOG #150 S150-0's backup/rename triage) */
#include "ui.h"
#include "snd.h"
#include "rmbl.h"          /* rumble must not toggle the cart bus during an SD write */

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

/* noinline (BACKLOG #81): meta_load's 176-byte buf[] would otherwise be inlined into
 * every caller and, per -fstack-usage, held for the caller's ENTIRE frame lifetime --
 * including the deep pdna_box()->...->ed_sd_dma_to_rom chain that runs long after this
 * function has already returned. buf[] is dead the instant meta_load() returns, so
 * forcing a real call/return frees those bytes before the deep chain begins. Pure
 * stack-layout change, no behavior change (same code, same order). */
static bool __attribute__((noinline)) meta_load(void) {
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
  rmbl_pause();
  bool ok = sf_write_verified(meta_path(), buf, META_BYTES) == SF_OK;
  rmbl_resume();
  return ok;
}

/* ---- box files ---- */
/* Read a box file -> g_bankbuf (absent/short/failed -> zeroed = an empty box, which is what
 * BROWSING wants). Returns whether the file was read IN FULL: any caller that intends to WRITE the
 * box back must gate on this — committing a zeroed buffer after a failed page-in would silently wipe
 * that box's untouched mons, and bank box files take NO immutable backup. */
static bool box_load(int box) {
  char path[SF_PATH_MAX]; box_path(box, path);
  uint32_t sz = 0;
  memset(g_bankbuf, 0, sizeof g_bankbuf);
  SfStatus st = sf_read_full(path, box_recs(), BOX_BYTES, &sz);
  g_loaded = box;
  g_dirty = false;
  return st == SF_OK && sz >= BOX_BYTES;
}

/* box_save's pre-write backup step (BACKLOG #150 S150-0). Up to this slice, box_save was
 * plain sf_write_verified with NO backup and NO SF_ERR_RENAME triage; UP (the native-cell
 * feature this unblocks) makes the Bank file a mon's ONLY copy, so both are required
 * before any of that can land -- mirrors gb_persist's triage (pdna_gen12.c, the
 * sf_backup_rolling + SF_ERR_RENAME block right after its busy-panel).
 *
 * noinline: keeps bak[SF_PATH_MAX] (272 B) off box_save's frame across whatever deep
 * chain called it, same reasoning as meta_load's noinline above.
 *
 * THE GUARD: a box file exists only after its first save (box_load's own comment, and
 * pdna_bank.c:98-109's zero-on-absent read) -- so on the FIRST save into a never-used
 * box, `path` does not exist yet. Without this guard, sf_backup_rolling -> copy_file ->
 * f_open(FA_READ) on a missing source returns SF_ERR_OPEN -> SF_ERR_BACKUP, and box_save
 * would refuse that first write forever. f_stat absent is the "nothing to back up, carry
 * on" case, not a failure. */
static bool box_save(void) {                    /* write the loaded box's records */
  if (g_loaded < 0) return false;
  char path[SF_PATH_MAX]; box_path(g_loaded, path);
  /* sf_save_rolling (savefile.c) owns the f_stat-absent-is-fine guard, the rolling
   * backup and the verified write -- host-linkable, so host_bankbackup_test.c tests
   * THIS function directly instead of a re-typed copy of its decision table
   * (BACKLOG #150 S150-0 review F3). */
  rmbl_pause();
  SfStatus st = sf_save_rolling(path, box_recs(), BOX_BYTES);
  rmbl_resume();
  if (st == SF_ERR_RENAME) {
    /* The bytes were written AND read back byte-for-byte -- it is the final swap the
     * card did not keep, a different piece of news from "the write failed". Ask the
     * card which file the user is actually holding rather than guessing, same triage
     * as gb_persist (pdna_gen12.c) and app_commit (pdna_main.c). */
    SfWhere w = sf_where_are_the_bytes(path, box_recs(), BOX_BYTES);
    log_line("bank: box save rename unconfirmed, bytes at %d", (int)w);
    if (w != SF_WHERE_TARGET) return false;   /* anything else: a real failure */
    st = SF_OK;                               /* the bytes ARE at path -- this is a success */
  }
  bool ok = st == SF_OK;
  if (ok) g_dirty = false;
  return ok;
}

/* Deferred cross-screen deletions: a mon carried Bank->PC (or Bank->party) is removed from
 * the bank only at the overall save phase (AFTER the PC is written), so the move needs no
 * prompt and can't lose the mon — worst case a duplicate if interrupted between the two
 * writes. We remember the carried mon's 8-byte IDENTITY (personality 0-3 + OT-ID 4-7, both
 * plaintext at the start of a box record — uniquely identifies a mon): the user may re-arrange
 * the bank before saving, so at flush time we delete the slot ONLY if it still holds that exact
 * mon; if a different mon now occupies the slot we skip it (harmless duplicate) rather than
 * deleting a bystander (a loss). (8 bytes, not the whole 80-byte record — the full-record table
 * was 5 KB and pushed EWRAM past 256 KB, corrupting the save image.) */
#define BANK_DEL_MAX  64
#define BANK_DEL_IDLEN 8
typedef struct { uint8_t box, slot; uint8_t id[BANK_DEL_IDLEN]; } BankDel;
static BankDel g_bank_del[BANK_DEL_MAX];   /* 640 B -> plain BSS (keeps EWRAM free for the save image) */
static int g_bank_ndel = 0;

bool pdna_bank_defer_full(void) { return g_bank_ndel >= BANK_DEL_MAX; }   /* callers refuse the move when full */
bool pdna_bank_defer_room(int n) { return n >= 0 && g_bank_ndel + n <= BANK_DEL_MAX; }   /* room for a whole chunk? */
void pdna_bank_defer_pop(int n) { g_bank_ndel -= n; if (g_bank_ndel < 0) g_bank_ndel = 0; }   /* undo the last n queued deletions */
void pdna_bank_defer_delete(int box, int slot, const uint8_t* rec80) {
  if (box < 0 || box >= BANK_BOXES || slot < 0 || slot >= BOX_RECS || g_bank_ndel >= BANK_DEL_MAX) return;
  g_bank_del[g_bank_ndel].box = (uint8_t)box; g_bank_del[g_bank_ndel].slot = (uint8_t)slot;
  if (rec80) memcpy(g_bank_del[g_bank_ndel].id, rec80, BANK_DEL_IDLEN);
  else       memset(g_bank_del[g_bank_ndel].id, 0, BANK_DEL_IDLEN);   /* no record => never matches => never deletes */
  g_bank_ndel++;
}
void pdna_bank_clear_deletions(void) { g_bank_ndel = 0; }

/* Is this bank slot's mon pending a deferred Bank->PC deletion? It still PHYSICALLY occupies the box
 * file (that's the mon's only on-card copy until the PC is saved), even though the display hides it. */
bool pdna_bank_slot_pending(int box, int slot) {
  for (int i = 0; i < g_bank_ndel; i++)
    if (g_bank_del[i].box == box && g_bank_del[i].slot == slot) return true;
  return false;
}

/* Display-only: blank the DECODED entries of slots pending a Bank->PC deletion, so a mon the user
 * already moved out reads as GONE from the bank immediately (the real delete lands at the save
 * prompt). Only touches the caller's PkMon array — never g_bankbuf, which box_save persists. */
void pdna_bank_hide_pending(int box, PkMon g[BOX_RECS]) {
  for (int i = 0; i < g_bank_ndel; i++)
    if (g_bank_del[i].box == box && g_bank_del[i].slot < BOX_RECS)
      g[g_bank_del[i].slot].species = 0;
}

/* Clear identity-matched `slots` in bank `box` and persist it (verified). For the cross-box MOVE —
 * called ONLY after the destination box is already committed. Pages the box in FIRST and REFUSES to
 * write when that read did not fully succeed, or when nothing matched: committing a zeroed/partial
 * buffer would wipe the box's untouched bystander mons, and bank box files take NO immutable backup.
 * Returning false leaves the file untouched => the move degrades to a safe, recoverable DUPLICATE. */
bool pdna_bank_clear_slots(int box, const uint8_t* slots, const uint8_t (*recs80)[80], int n) {
  if (box < 0 || box >= BANK_BOXES || n <= 0) return false;
  if (g_loaded != box) { if (g_dirty) box_save(); if (!box_load(box)) return false; }   /* page-in must be COMPLETE */
  int cleared = 0;
  for (int i = 0; i < n; i++) {
    if (slots[i] >= BOX_RECS) continue;
    uint8_t* p = box_recs() + (uint32_t)slots[i] * REC_BYTES;
    if (memcmp(p, recs80[i], BANK_DEL_IDLEN) != 0) continue;   /* not our mon any more -> leave it alone */
    memset(p, 0, REC_BYTES);
    cleared++;
  }
  if (!cleared) return false;                                  /* nothing matched -> do NOT rewrite the box */
  g_dirty = true;
  return box_save();
}
void pdna_bank_flush_deletions(void) {
  for (int i = 0; i < g_bank_ndel; i++) {
    int box = g_bank_del[i].box, slot = g_bank_del[i].slot;
    if (g_loaded != box) { if (g_dirty) box_save(); box_load(box); }   /* page the box in (reads its file) */
    uint8_t* p = box_recs() + (uint32_t)slot * REC_BYTES;
    if (memcmp(p, g_bank_del[i].id, BANK_DEL_IDLEN) != 0) continue;    /* slot no longer holds OUR mon -> don't delete */
    memset(p, 0, REC_BYTES);
    g_dirty = true; box_save();                                        /* write the box without that mon */
  }
  g_bank_ndel = 0;
}

/* ---- one-time migration from the old flat /PokeDNA/bank/*.pk3 layout ---- */
static bool has_pk_ext(const char* n) {
  int L = (int)strlen(n);
  if (L >= 4 && n[L-4]=='.' && (n[L-3]=='p'||n[L-3]=='P') && (n[L-2]=='k'||n[L-2]=='K') && n[L-1]=='3') return true;
  if (L >= 3 && n[L-3]=='.' && (n[L-2]=='p'||n[L-2]=='P') && (n[L-1]=='k'||n[L-1]=='K')) return true;
  return false;
}

/* Pack existing .pk3 files into box files in directory order. Non-destructive: the
 * .pk3 files stay; they're simply no longer the store. Omega-only (it writes).
 *
 * noinline (BACKLOG #81): same reasoning as meta_load() above, at far higher stakes --
 * this function's locals (DIR d, FILINFO fno with its LFN name buffers, path[SF_PATH_MAX],
 * rec[REC_BYTES]) are the single largest contributor to pdna_bank_show()'s 784-byte
 * frame, and all of them are dead before pdna_bank_show() ever calls into pdna_box().
 * Inlined, that dead space still sits under the deepest write chain
 * (pdna_box->...->app_paste_gb_merge->...->ed_sd_dma_to_rom) for the rest of the
 * function's lifetime. Forcing a real call/return frees it before that chain runs.
 * Pure stack-layout change: same code, same order, no write-path semantics touched. */
static void __attribute__((noinline)) migrate_flat_pk3(void) {
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

int pdna_bank_show(void) {
  f_mkdir("/PokeDNA");
  f_mkdir(PDNA_BANK_DIR);

  if (app_can_edit() && !layout_exists()) migrate_flat_pk3();   /* first run: import old .pk3 */
  meta_load();
  g_loaded = -1; g_dirty = false;

  BoxSource s; memset(&s, 0, sizeof s);
  s.nboxes     = BANK_BOXES;
  s.start_box  = 0;
  s.is_bank    = true;
  s.scope      = BOXSCOPE_BANK;  /* BACKLOG #120 S1: can_lift/xfer stay NULL (memset above) */
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

  int r = pdna_box(&s);

  /* deferred moves: ask on leaving the bank. HONEST SCOPE: paging to another box
   * auto-saves the box being left (banksrc_records -> box_save; one shared buffer),
   * so this prompt only covers the CURRENT box — say exactly that. */
  if (g_dirty) {
    if (app_can_edit() && app_confirm("Save this bank box?", "Prior boxes auto-saved."))
      box_save();
    else if (g_loaded >= 0)
      box_load(g_loaded);                          /* discard: reload the box from its file */
    g_dirty = false;
  }
  /* THE BANK IN PARALLEL — where this screen's part of Guy's request actually lives.
   *
   * The bank is the one place mons from all three eras genuinely coexist, so it is the
   * screen the "show all in parallel" ask is about; what it does NOT do is render them,
   * because that is the shared box grid's job (pdna_box.c's era_cells) and forking a
   * second grid for the bank would be the wrong answer twice over.
   *
   * Filling the era cache is not this file's job either, and used to be: box_load /
   * box_save / mark_dirty each rebuilt it from the RAW records. That was both redundant
   * — pdna_box.c re-decodes the box right after every one of those — and subtly WRONG:
   * a bank slot the user has already carried out to the PC is displayed empty but still
   * physically holds the mon's only on-card copy until the PC is written, so the raw
   * buffer wears a Game Boy marker on a cell that shows no Pokemon. The cache is now
   * filled from the DECODED box, which is exactly what is on screen.
   *
   * The cache is a singleton, so drop it on the way out: no other screen fills it, and
   * a stale one would describe a box that is no longer displayed. */
  pdna_origin_box_clear();
  return r;        /* 5 = the user dropped off the bottom -> caller reopens the PC on its tabs */
}
