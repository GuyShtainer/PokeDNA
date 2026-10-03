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
#include "bank_layout.h"  /* BACKLOG #371: layout_exists / meta source decisions (pure, host-tested) */
#include "bank_cell.h"    /* bc_is_native -- box_save's native invariant (BACKLOG #150 S150-3) */
#include "bank_plant.h"   /* PDNA_DELTA-only test plant (BACKLOG #150 S150-2 step 6) */
#include "pdna_layout.h"  /* PDNA_BANKSAVE_* -- box_save's SF_ERR_RENAME switch (S150-0 F4) */
#include "pdna_origin_art.h"  /* the parallel era view: the bank is where all three meet */
#include "log.h"           /* log_line (BACKLOG #150 S150-0's backup/rename triage) */
#include "ui.h"
#include "snd.h"
#include "rmbl.h"          /* rumble must not toggle the cart bus during an SD write */

#define BANK_BOXES   16
_Static_assert(BANK_BOXES == PDNA_BANK_BOXES, "pdna_bank.h's PDNA_BANK_BOXES must track BANK_BOXES");
#define BOX_RECS     G3_IN_BOX            /* 30 */
#define REC_BYTES    80
#define BOX_BYTES    (BOX_RECS * REC_BYTES)   /* 2400 */
#define META_MAGIC   "PKVBNK"
#define META_VERSION 2   /* BACKLOG #150 S150-4 decision 2: bytes 10..13 now hold g_bank_serial */
#define META_HDR     16
#define META_BYTES   (META_HDR + BANK_BOXES * 10)   /* 16 + 160 = 176 */

/* current box, pc-mini-layout: 4-byte header then 30 records (so app_mon_menu's
 * pk_box_slot(block, 0, slot) lands on record `slot`). */
static uint8_t EWRAM_BSS g_bankbuf[0x0004 + BOX_BYTES];
static int  g_loaded = -1;                 /* which box g_bankbuf holds, or -1     */
static bool g_dirty  = false;              /* loaded box has unsaved deferred moves */
/* BACKLOG #163 review F1: g_box_unsaved_box's whole meaning is "the box g_dirty is
 * about" -- it must never survive past a g_dirty clear (box_load re-reading the box
 * from disk, or a discard) or the marker outlives the fact it describes: the NEXT time
 * that box index is loaded (any box, since g_box_unsaved_box is a plain index, not tied
 * to identity) it would wrongly show BOX NOT SAVED on a box nobody has touched, and
 * SWITCH_BOX would refuse to page away from it while banksrc_records's own g_dirty
 * check says there is nothing to flush -- the exact split-brain review F1 found live:
 * a refused save, discarded on exit ("Its edits are lost", g_dirty cleared, marker
 * left standing), then the NEXT Bank visit loads a DIFFERENT box into g_bankbuf while
 * the screen still shows the OLD box's frame. Declared here (above box_load) so
 * box_load's own g_dirty clear can clear it in the same breath -- see box_load below. */
static int8_t g_box_unsaved_box = -1;
bool pdna_bank_box_unsaved(int box) { return box >= 0 && box == g_box_unsaved_box; }
static struct { uint8_t name[9]; uint8_t wp; } g_meta[BANK_BOXES];

static uint8_t* box_recs(void) { return g_bankbuf + 0x0004; }

/* BACKLOG #150 S150-3 decision 6 (D-Q2 accepted): one bit per slot in the loaded box --
 * set at page-in time (box_load's own tail, after sf_read_full) when that slot's raw
 * bytes were bc_is_native(). box_save() consults it (and only it -- the ONE new static
 * this lane may add) to refuse an in-place overwrite of a native cell by a different,
 * non-empty record. Zero elsewhere (RELEASE/clear_origin/consume-to-zero all leave the
 * slot all-zero, which the invariant exempts). 30 bits, one per BOX_RECS slot. */
static uint32_t g_native_snap;

/* BACKLOG #150 S150-4 decision 2: the EXACTLY ONE new static this lane licenses (4
 * bytes). bank.meta bytes 10..13 (LE), persisted BEFORE the cell that consumes it
 * (pdna_bank_next_serial's own contract) -- makes ident32 unique by construction
 * across two packs of the same mon (G-M4). 0 when absent/short/bad-magic, same as
 * every other meta_load() field. */
static uint32_t g_bank_serial;

/* BACKLOG #168: true only immediately after a meta_load() whose PRIMARY bank.meta
 * parsed clean (magic + length ok) -- never after a .bak fallback or a default reset.
 * pdna_bank_next_serial() calls meta_load() fresh right before it derives+persists
 * every serial a native cell's ident32 is ever built from, so at the moment a lift
 * mints a serial this flag says whether that serial came off a card that has NOT
 * been rolled back since. The drop_held() UP collision scan (BACKLOG #168a) treats
 * "trusted" as "no other box can hold this ident32" and skips its 15 extra box
 * reads; the moment ANY later meta_load() in the session falls back to the .bak or
 * defaults (the exact "meta was lost or rolled back" scenario the scan exists for),
 * this drops to false and the full 16-box scan resumes for every drop after it. */
static bool EWRAM_BSS g_serial_trusted;
bool pdna_bank_serial_trusted(void) { return g_serial_trusted; }

/* BACKLOG #219a: true only immediately after a meta_load() that fell back to
 * bank.meta.bak because the PRIMARY was unreadable/corrupt -- i.e. the primary on
 * the card right now is the corrupt file that fallback recovered PAST, not the good
 * bytes RAM holds. sf_save_rolling's own f_stat(primary) cannot tell "present but
 * corrupt" from "present and fine": it sees FR_OK either way and backs the PRIMARY
 * up over the still-good bank.meta.bak before writing -- so the very next meta_save()
 * after a bak recovery clobbered the one good copy with the corrupt one, making
 * recovery single-shot (a second corruption before another clean write would have
 * no good .bak left). Consumed by meta_save() below: one f_unlink(meta_path()) BEFORE
 * the save call turns the corrupt primary into "absent" from sf_save_rolling's own
 * FR_NO_FILE probe (savefile.c), which skips the backup step entirely -- bank.meta.bak
 * is left holding the good recovered bytes while the primary is rewritten fresh from
 * RAM. Consumed (cleared) unconditionally by the next meta_save() call, whether or not
 * that save succeeds -- a failed save leaves the primary absent/partial, which the next
 * meta_load() already detects on its own via the normal FR_OK+magic probe. */
static bool EWRAM_BSS g_meta_from_bak;

/* BACKLOG #379 F4: do g_meta/g_bank_serial hold a real read of the card (or legit defaults)?
 * 0 = unknown (nothing loaded yet this session: the legacy behaviour, every caller as before),
 * 1 = valid (a clean/restored load, or the genuine no-Bank defaults), 2 = UNSAFE (the very first
 * meta_load() hit a card READ ERROR: RAM holds display-only defaults, so meta_save() must refuse
 * -- a defaults-shaped write would roll the good primary into .bak). A READ ERROR after a valid
 * load changes nothing: the names the session holds ARE the card's names and stay. */
static uint8_t EWRAM_BSS g_meta_state;

/* Zy D1: the page-in of g_loaded hit a card READ ERROR, so g_bankbuf is a zeroed buffer, not that box.
 * box_save() refuses while non-zero (committing it would wipe the box's untouched mons).
 * BACKLOG #382(b): 1 = READ ERROR (zeroed buffer), 2 = the #378 heal FAILED (the buffer holds the recovered
 * copy, shown browse-only; writing it would roll the .bak and could unlink the .tmp, the only good copy). */
#define BOX_UNREAD_ERR   1
#define BOX_UNREAD_HEAL  2
#define BOX_UNREAD_KIND  0x03u   /* the kind bits */
#define BOX_UNREAD_NOTED 0x80u   /* the on-screen notice was already shown for this page-in (box_load clears it) */
static uint8_t EWRAM_BSS g_box_unread;

/* ---- paths ---- */
static void box_path(int box, char* out) { siprintf(out, PDNA_BANK_DIR "/box%02d.box", box); }
static const char* meta_path(void) { return PDNA_BANK_DIR "/bank.meta"; }

/* ---- metadata ---- */
static void meta_defaults(void) {
  for (int b = 0; b < BANK_BOXES; b++) {
    siprintf((char*)g_meta[b].name, "BANK %d", b + 1);
    g_meta[b].wp = (uint8_t)(b % G3_BOX_WALLPAPER_COUNT);   /* each box a different look */
  }
  g_bank_serial = 0;   /* decision 2: zero it too, same as every other meta_defaults() field */
  g_serial_trusted = false;   /* BACKLOG #168: a defaulted serial can never be trusted */
  g_meta_from_bak = false;   /* BACKLOG #219a: a fresh default has no corrupt primary to heal */
  g_meta_state = 1;          /* legit defaults (no Bank yet); meta_load's READ_ERROR path downgrades it */
}

/* noinline (BACKLOG #81): meta_load's 176-byte buf[] would otherwise be inlined into
 * every caller and, per -fstack-usage, held for the caller's ENTIRE frame lifetime --
 * including the deep pdna_box()->...->ed_sd_dma_to_rom chain that runs long after this
 * function has already returned. buf[] is dead the instant meta_load() returns, so
 * forcing a real call/return frees those bytes before the deep chain begins. Pure
 * stack-layout change, no behavior change (same code, same order). */
static bool __attribute__((noinline)) meta_load(void) {
  uint8_t buf[META_BYTES];
  /* BACKLOG #371: the source decision (primary / .bak / nothing) lives in bank_layout.c so a
   * host test drives the REAL logic. An ABSENT primary with a good .bak takes the same restore
   * path as a corrupt primary (the interrupted-write window unlink -> rename leaves exactly
   * that). BACKLOG #379 F3: when NO .bak parses either, the verified bank.meta.tmp (else .baktmp:
   * magic + length checked, kept by savefile.c only after a passing byte-compare) is used as a last
   * resort and treated as a .bak restore. A card READ ERROR is not "missing" (F4, below). */
  BmlSource src = bml_meta_read(PDNA_BANK_DIR, BANK_BOXES, buf, sizeof buf, META_BYTES, META_MAGIC);
  bool from_tmp = src == BML_LAST_RESORT_ABSENT || src == BML_LAST_RESORT_BAD;   /* Zy D3: bytes came from bank.meta.tmp */
  bool from_bak = src == BML_BAK_PRIMARY_ABSENT || src == BML_BAK_PRIMARY_BAD ||
                  from_tmp || src == BML_LAST_RESORT_BAKTMP_ABSENT || src == BML_LAST_RESORT_BAKTMP_BAD;
  bool ok = src == BML_PRIMARY || from_bak;
  if (src == BML_READ_ERROR) {
    /* BACKLOG #379 F4: a card fault is NOT "missing". Keep the names RAM already holds (they are the
     * card's), do not meta_defaults() them, return false. Only a session that never loaded anything
     * gets display-only defaults, and then g_meta_state=2 makes meta_save() refuse. */
    log_line("bank: meta read error - keeping the names in RAM, nothing is written");
    app_log_flush();
    if (g_meta_state == 0) { meta_defaults(); g_meta_state = 2; }
    return false;
  }
  if (src == BML_BAK_PRIMARY_ABSENT) {
    log_line("bank: meta primary missing, restored from bank.meta.bak");
    app_log_flush();
  } else if (src == BML_BAK_PRIMARY_BAD) {
    log_line("bank: meta primary unreadable/corrupt, restored from bank.meta.bak");
    app_log_flush();
  } else if (from_bak && src != BML_BAK_PRIMARY_ABSENT && src != BML_BAK_PRIMARY_BAD) {
    log_line("bank: meta primary %s, no usable .bak - restored from the verified bank.meta.%s",
             (src == BML_LAST_RESORT_ABSENT || src == BML_LAST_RESORT_BAKTMP_ABSENT) ? "missing" : "unreadable/corrupt",
             from_tmp ? "tmp" : "baktmp");
    app_log_flush();
  } else if (src == BML_NONE_BOXES) {
    /* Neither copy is usable but box files exist: an existing Bank. Names stay at their
     * defaults in RAM, the 16-box scan + serial resync run as before; nothing is written here. */
    log_line("bank: meta missing (no primary, no .bak) but box files exist - default names in RAM");
    app_log_flush();
  }
  if (!ok) { meta_defaults(); return false; }
  for (int b = 0; b < BANK_BOXES; b++) {
    const uint8_t* p = buf + META_HDR + b * 10;
    memcpy(g_meta[b].name, p, 9); g_meta[b].name[8] = 0;
    g_meta[b].wp = p[9] % G3_BOX_WALLPAPER_COUNT;
  }
  /* decision 2: bytes 10..13, LE -- 0 when absent/short/bad-magic (the guard above
   * already returned false in every one of those cases; a v1 file that passed the
   * guard simply has zero bytes there, which decodes to 0 -- a clean v1 upgrade). No
   * tri-state on byte 8 (the version): meta_load never inspected it before this lane
   * and G-H3 says the version byte is not the backup gate, so a version branch here
   * would be dead code. */
  g_bank_serial = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8) |
                  ((uint32_t)buf[12] << 16) | ((uint32_t)buf[13] << 24);
  /* BACKLOG #168: only a clean PRIMARY parse is "trusted" -- a .bak fallback is
   * exactly the "meta was lost or rolled back" case the drop_held() 16-box scan
   * exists to catch, so it must NOT short-circuit that scan. */
  g_serial_trusted = !from_bak;
  /* BACKLOG #219a: mark that the CARD's primary is currently the corrupt file this
   * load fell back past -- meta_save()'s next call must heal it before it can safely
   * take its usual rolling backup. */
  g_meta_from_bak = from_bak;
  g_meta_state = 1;
  if (from_tmp && app_can_edit()) {
    /* Zy D3: meta_save()'s verified write would TRUNCATE bank.meta.tmp (its own scratch) -- the only copy of
     * these names. Heal by RENAME, before any write can happen. A failed heal makes meta_save refuse
     * (state 2: it would hit that truncation), a later meta_load retries. */
    rmbl_pause();
    bool healed = bml_meta_heal_tmp(PDNA_BANK_DIR, META_BYTES);
    rmbl_resume();
    if (healed) {
      g_meta_from_bak = false;        /* primary is now the good file and no .bak was displaced */
      log_line("bank: meta healed by renaming bank.meta.tmp into place");
    } else {
      g_meta_state = 2;
      log_line("bank: meta heal from .tmp failed - names kept in RAM, meta writes refused");
    }
    app_log_flush();
  }
  return true;
}

/* Review fix 4 (BACKLOG #191a), hard rule 9: /PokeDNA/bank is otherwise created
 * ONLY by pdna_bank_show() (below) -- on a card whose Bank screen was never opened
 * yet, pdna_bank_next_serial()'s meta_save() (the ONLY other path that reaches this
 * write) failed at the FatFs layer with no directory to write into, refusing the
 * FIRST-EVER grab with a silent beep (Guy's own #191a report, traced by the review
 * to here). Same idempotent, return-ignored idiom pdna_bank_show() already uses --
 * f_mkdir on an existing directory returns FR_EXIST, which this (like that call)
 * does not distinguish from success; either way the directory exists after this
 * line. */
static bool meta_save(void) {
  if (g_meta_state == 2) {   /* BACKLOG #379 F4: RAM holds display-only defaults after a card read error */
    log_line("bank: meta NOT saved - never read from the card (read error), refusing a defaults write");
    app_log_flush();
    return false;
  }
  f_mkdir("/PokeDNA");
  f_mkdir(PDNA_BANK_DIR);

  uint8_t buf[META_BYTES];
  memset(buf, 0, sizeof buf);
  memcpy(buf, META_MAGIC, 6);
  buf[8] = META_VERSION; buf[9] = BANK_BOXES;
  buf[10] = (uint8_t)(g_bank_serial & 0xFF);
  buf[11] = (uint8_t)((g_bank_serial >> 8) & 0xFF);
  buf[12] = (uint8_t)((g_bank_serial >> 16) & 0xFF);
  buf[13] = (uint8_t)((g_bank_serial >> 24) & 0xFF);
  for (int b = 0; b < BANK_BOXES; b++) {
    uint8_t* p = buf + META_HDR + b * 10;
    memcpy(p, g_meta[b].name, 9);
    p[9] = g_meta[b].wp;
  }
  /* BACKLOG #168b: was sf_write_verified (no backup at all) -- box_save() already
   * takes one rolling .bak per box file (sf_save_rolling_ok); bank.meta (box names,
   * wallpapers, and the serial pdna_bank_next_serial's whole G-M4 uniqueness
   * guarantee rests on) had none, so a bad write left it unrecoverable. Same rolling
   * discipline as the boxes now: one "bank.meta.bak", built+verified before it
   * replaces the previous one. */
  /* BACKLOG #219a: heal a corrupt primary BEFORE sf_save_rolling gets to it. Its own
   * f_stat(meta_path()) probe (savefile.c) cannot distinguish "present and corrupt"
   * from "present and fine" -- FR_OK either way -- so left alone it would back the
   * corrupt primary up OVER the still-good bank.meta.bak this session's meta_load()
   * just recovered from, making a second recovery impossible. f_unlink() first turns
   * the corrupt file into FR_NO_FILE, sf_save_rolling's own "nothing to back up" case
   * (savefile.c), which skips the backup step and writes straight from RAM -- .bak
   * keeps the good recovered bytes. One-shot: consumed here whether or not this save
   * itself succeeds. Return value ignored on purpose (same idiom as f_mkdir above):
   * an unlink failure just leaves sf_save_rolling's normal FR_OK path in force, which
   * is this function's pre-fix (still safe, if single-shot) behavior. */
  if (g_meta_from_bak) {
    FRESULT ur = f_unlink(meta_path());
    if (ur != FR_OK && ur != FR_NO_FILE && ur != FR_NO_PATH) {
      /* BACKLOG #219a review F1: the corrupt primary is STILL on the card (an AM_RDO
       * object -> FR_DENIED, ff.c:5011; a write-protected volume; a disk error).
       * Proceeding would let sf_save_rolling's FR_OK path copy THAT file over the good
       * bank.meta.bak this session recovered from -- measured on real FatFs: the .bak
       * ends up holding the corrupt bytes, the exact single-shot loss this fix exists
       * to stop. Refuse, and KEEP the flag so a later meta_save() can still heal. */
      log_line("bank: meta heal unlink failed (fr=%d) - refusing to write unbacked", (int)ur);
      app_log_flush();
      return false;
    }
    g_meta_from_bak = false;
  }
  rmbl_pause();
  bool ok = sf_save_rolling(meta_path(), buf, META_BYTES, NULL) == SF_OK;
  rmbl_resume();
  return ok;
}

/* BACKLOG #150 S150-4 decision 2: allocate the next serial and PERSIST it before
 * returning (SS11.1 rule (i)) -- so a lift that then fails to write its cell never
 * leaves a serial "spent" only in RAM (which a later lift could reuse, breaking
 * G-M4's "two lifts differ in bytes 0..7" guarantee). Returns 0 (a caller-visible
 * "refuse the lift") on a meta write failure; 0 is otherwise never allocated because
 * this always increments FIRST. */
uint32_t pdna_bank_next_serial(void) {
  /* REVIEW F1: g_meta/g_bank_serial are populated ONLY by pdna_bank_show(); a GB-grid
   * lift runs first */
  meta_load();
  uint32_t next = g_bank_serial + 1;
  uint32_t prev = g_bank_serial;
  g_bank_serial = next;
  if (!meta_save()) { g_bank_serial = prev; return 0; }
  return next;
}

/* BACKLOG #223: see pdna_bank.h's own doc comment. g_bank_serial is populated by
 * meta_load() the same way pdna_bank_next_serial() relies on -- callers on the UP
 * path (drop_held_up) always run through the collision scan (which itself pages
 * every box, so g_meta/g_bank_serial are current by the time this runs) first. */
bool pdna_bank_serial_resync(uint32_t stored_max) {
  if (g_bank_serial > stored_max) return false;   /* already strictly ahead -- nothing to do */
  uint32_t prev = g_bank_serial;
  uint32_t next = stored_max + 1;
  g_bank_serial = next;
  if (!meta_save()) { g_bank_serial = prev; return false; }
  log_line("bank: serial resynced %u -> %u", (unsigned)prev, (unsigned)next);
  return true;
}

/* ---- box files ---- */
/* Read a box file -> g_bankbuf (absent/short/failed -> zeroed = an empty box, which is what
 * BROWSING wants). Returns whether the file was read IN FULL: any caller that intends to WRITE the
 * box back must gate on this — committing a zeroed buffer after a failed page-in would silently wipe
 * that box's untouched mons, and bank box files take only a single rolling .bak (box_save's
 * sf_save_rolling, BACKLOG #150 S150-0), not an immutable backup — there is no earlier generation
 * to fall back past that one file. */
static bool box_load(int box) {
  char path[SF_PATH_MAX]; box_path(box, path);
  uint32_t sz = 0;
  memset(g_bankbuf, 0, sizeof g_bankbuf);
  /* BACKLOG #378: the box file has the same interrupted-swap window as bank.meta (#371): the verified
   * write's unlink -> rename leaves the primary ABSENT while the verified .tmp holds the newest bytes
   * and the .bak the previous ones. The classifier (bank_layout.c, host-tested) picks primary, else
   * .tmp, else .bak -- only when the primary is truly absent; a card read ERROR is not "absent". */
  BmlBoxSrc bsrc = bml_box_read(path, box_recs(), BOX_BYTES, &sz);
  bool recovered = bsrc == BML_BOX_TMP || bsrc == BML_BOX_BAK;
  bool heal_failed = false;
  if (recovered) {
    log_line("bank: box %02d primary missing, restored from %s", box, bsrc == BML_BOX_TMP ? "tmp" : "bak");
    app_log_flush();
    /* Heal BEFORE anything can write: the primary is back (rename of the .tmp, or a verified write from
     * the .bak -- never sf_save_rolling, so no backup roll can bury the good .bak). Read-only (Everdrive):
     * recovered in RAM only, never written. A failed heal returns false below and sets g_box_unread
     * (BOX_UNREAD_HEAL, #382b) so box_save refuses too -- every writer, not only the box_load callers. */
    bool healed = true;
    if (app_can_edit()) {                       /* Zy D7: rmbl.h requires the pause around every SD write */
      rmbl_pause();
      healed = bml_box_heal(path, bsrc, box_recs(), BOX_BYTES);
      rmbl_resume();
    }
    if (!healed) {
      heal_failed = true;
      log_line("bank: box %02d heal failed - recovered copy kept on the card, browse only (writes refused)", box);
      app_log_flush();
    }
  } else if (bsrc == BML_BOX_READ_ERROR) {
    log_line("bank: box %02d read error - not treated as empty, writes refused", box);
    app_log_flush();
  }
  bool got = bsrc == BML_BOX_PRIMARY || bsrc == BML_BOX_BAD || recovered;   /* BAD = present but short, as before: no plant */
#ifdef PDNA_DELTA
  /* BACKLOG #150 S150-2 step 6: a PDNA_DELTA-only test plant -- boxes 0 and 1 only, and
   * only when the real file could not be read (a virgin/absent box, the common delta-
   * test-vehicle case). ZERO effect on the shipped build (PDNA_DELTA is never defined
   * there): see bank_plant.h. */
  if (!got && box == 0) bank_plant_box0(box_recs());
  if (!got && box == 1) bank_plant_box_full(box_recs());
  /* BACKLOG #246 (#104 Phase 1): a THIRD, dedicated box -- never box 0/1, so every
   * existing shot chain keyed on those two boxes' own byte-for-byte content stays
   * untouched. This is the one plant this whole file never had before: a PLAIN
   * Gen-3 cell (see bank_plant_g3_box's own comment), the source this lane's new
   * arm needs to demonstrate carrying a Gen-3 Bank cell onto a Game Boy grid. */
  if (!got && box == 2) bank_plant_g3_box(box_recs());
  if (!got && box == 3) bank_plant_y9_box(box_recs());   /* #280: the target-drop restore chains' cells */
#endif
  /* BACKLOG #150 S150-3 decision 6: recomputed at every page-in, AFTER the PDNA_DELTA
   * plant above so a planted native cell is captured too -- this is the ONE choke
   * point every caller that can later reach box_save() pages through (banksrc_records,
   * pdna_bank_clear_slots, pdna_bank_flush_deletions all call box_load() when the
   * wanted box isn't already resident). */
  g_native_snap = 0;
  for (int s = 0; s < BOX_RECS; s++)
    if (bc_is_native(box_recs() + (uint32_t)s * REC_BYTES)) g_native_snap |= (1u << s);
  /* BACKLOG #382(b): a FAILED heal is write-protected too (it logged "browse only" but the box was still
   * written). Trade-off, accepted: that box's edits do not save this session -- re-open the Bank to retry. */
  g_box_unread = (bsrc == BML_BOX_READ_ERROR) ? BOX_UNREAD_ERR : heal_failed ? BOX_UNREAD_HEAL : 0;
  g_loaded = box;
  g_dirty = false;
  g_box_unsaved_box = -1;   /* review F1: a fresh page-in re-reads the CARD's own copy --
                             * whatever RAM-only edits g_box_unsaved_box was about are gone. */
  return got && !heal_failed && sz >= BOX_BYTES;
}

/* BACKLOG #382(a): the on-screen notice for a box that is not safe to write. Shown once per page-in (banksrc_records),
 * sprites off like every dialog this file raises. */
static void box_unread_notice(void) {
  u16 dc = REG_DISPCNT;
  REG_DISPCNT &= ~DCNT_OBJ;
  if ((g_box_unread & BOX_UNREAD_KIND) == BOX_UNREAD_HEAL)
    msg_wait(PDNA_BANK_HEALFAIL_TITLE, UI_WARN, PDNA_BANK_HEALFAIL_L1, PDNA_BANK_HEALFAIL_L2);
  else
    msg_wait(PDNA_BANK_UNREAD_TITLE, UI_WARN, PDNA_BANK_UNREAD_L1, PDNA_BANK_UNREAD_L2);
  REG_DISPCNT = dc;
}

/* BACKLOG #382(a): "Retry the save?" on a box whose page-in hit a READ ERROR must re-read it -- nothing else ever
 * does, so the retry could never succeed. Only when the buffer is still the zeroed phantom (no edit to merge: a
 * failed drop reverts its cells before it keeps holding); a heal-failed box (BOX_UNREAD_HEAL) is re-opened, not
 * re-read here. true = the card answered this time and the buffer now IS the box: there is nothing left to write. */
static bool box_unread_reread(void) {
  if ((g_box_unread & BOX_UNREAD_KIND) != BOX_UNREAD_ERR || g_loaded < 0) return false;
  const uint8_t* p = box_recs();
  for (int i = 0; i < BOX_BYTES; i++) if (p[i]) return false;     /* an edit sits in the phantom: cannot merge, keep refusing */
  int box = g_loaded;
  (void)box_load(box);                                            /* resets g_box_unread / g_dirty / the unsaved marker */
  log_line("bank: box %02d re-read on retry -> %s", box, g_box_unread ? "still unread" : "ok");
  app_log_flush();
  return g_box_unread == 0;
}

/* box_save's pre-write backup + write + SF_ERR_RENAME triage (BACKLOG #150 S150-0). Up
 * to this slice, box_save was plain sf_write_verified with NO backup and NO
 * SF_ERR_RENAME triage; UP (the native-cell feature this unblocks) makes the Bank file
 * a mon's ONLY copy, so both are required before any of that can land -- mirrors
 * gb_persist's triage (pdna_gen12.c, the sf_backup_rolling + SF_ERR_RENAME block right
 * after its busy-panel).
 *
 * The mechanical work (the f_stat-absent-is-fine guard -- a box file exists only after
 * its first save, box_load's own comment above, so f_stat absent on the FIRST save into
 * a never-used box is "nothing to back up, carry on", not a failure -- plus the rolling
 * backup and the verified write) lives in savefile.c's sf_save_rolling, not here: that
 * makes it host-linkable, so host_bankbackup_test.c tests the real function directly
 * instead of a re-typed copy of its decision table (review F3, which replaced an
 * earlier noinline box_backup() helper that lived right here). box_save keeps only the
 * SF_ERR_RENAME/SF_WHERE_TARGET triage and its UI (review F4), since that decision is
 * this caller's to make, not sf_save_rolling's (savefile.h says so). */
/* BACKLOG #150 S150-3 decision 6: "native ⇒ still native OR all-zero" -- NOT "still
 * native". A same-scope Bank move zeroes the origin (clear_origin), RELEASE zeroes a
 * cell (clip_clear_box_slot), and a consume-to-zero transition is legitimate; the
 * data-loss transition this refuses is an in-place overwrite by a DIFFERENT, non-empty
 * record. Kept out of box_save's own frame (noinline, same reasoning as meta_load /
 * migrate_flat_pk3 above -- this file's deepest write chain runs long after this
 * function returns). Log + refuse only, no UI: box_save runs from callers with no grid
 * on screen (its own review-G4 comment above says so). */
static bool __attribute__((noinline)) native_invariant_ok(void) {
  for (int s = 0; s < BOX_RECS; s++) {
    if (!(g_native_snap & (1u << s))) continue;
    const uint8_t* p = box_recs() + (uint32_t)s * REC_BYTES;
    if (bc_is_native(p)) continue;
    bool allzero = true;
    for (int i = 0; i < REC_BYTES; i++) if (p[i]) { allzero = false; break; }
    if (allzero) continue;
    log_line("bank: box %d slot %d: native cell overwritten, refusing", g_loaded, s);
    app_log_flush();
    return false;
  }
  return true;
}

/* The bytes were written AND read back byte-for-byte -- it is the final swap the card
 * did not keep, a different piece of news from "the write failed". Ask the card which
 * file the user is actually holding rather than guessing, same triage as gb_persist
 * (pdna_gen12.c) and app_commit (pdna_main.c) -- and, per §11.13/XFER-C1 (review F4),
 * SHOW it instead of only logging it silently. Split out of box_save() (rule 4: one
 * printed page) -- this is the whole SF_ERR_RENAME branch, box_save() keeps only the
 * dispatch to it. */
static void box_save_rename_triage(const char* path, SfWhere w, bool ok, bool backed_up) {
  log_line("bank: box save rename unconfirmed, bytes at %d", (int)w);
  app_log_flush();
  /* review G4: unlike the box screen's own boxoam_suspend/resume bracket, box_save
   * has none of its own -- SWITCH_BOX's chain reaches here through banksrc_records
   * with the box-cell sprites still enabled, but box_save ALSO runs from callers with
   * no grid on screen at all (a blind boxoam_resume() here would be wrong in that
   * case). msg_wait's blocking wait-for-A loop must not run with arbitrary leftover
   * OBJ content still visible underneath it, so disable OBJ for exactly this UI block
   * and restore whatever the caller had on every exit path. */
  u16 dc = REG_DISPCNT;
  REG_DISPCNT &= ~DCNT_OBJ;
  if (!ok) {
    snd_error();
    char l1[64];
    switch (w) {
      case SF_WHERE_TMP_ONLY: {           /* the loud one: no .box on the card */
        const char* nm = strrchr(path, '/');
        nm = nm ? nm + 1 : path;
        siprintf(l1, "Box is in %.28s.tmp", nm);
        msg_wait(PDNA_BANKSAVE_TMPONLY_TITLE, UI_WARN, l1, PDNA_BANKSAVE_TMPONLY_L2);
        break;
      }
      case SF_WHERE_TMP_AND_OLD:          /* old box intact; edit not applied */
        msg_wait(PDNA_BANKSAVE_TMPANDOLD_TITLE, UI_WARN,
                  PDNA_BANKSAVE_TMPANDOLD_L1, PDNA_BANKSAVE_TMPANDOLD_L2);
        break;
      default:                            /* neither name matches */
        /* review G2: a virgin box (this write's own backup step never ran, since
         * there was nothing to back up) never made a .bak -- "restore the .bak"
         * would send the user hunting a file that does not exist. */
        msg_wait(PDNA_BANKSAVE_LOST_TITLE, UI_WARN, PDNA_BANKSAVE_LOST_L1,
                  backed_up ? PDNA_BANKSAVE_LOST_L2 : PDNA_GBEDIT_SAVELOST_NOBAK);
        break;
    }
  } else {
    msg_wait(PDNA_BANKSAVE_UNCONFIRMED_TITLE, UI_WARN,
              PDNA_BANKSAVE_UNCONFIRMED_L1, PDNA_BANKSAVE_UNCONFIRMED_L2);
  }
  REG_DISPCNT = dc;
}

static bool box_save(void) {                    /* write the loaded box's records */
  if (g_loaded < 0) return false;
  if (g_box_unread) {
    log_line("bank: box %02d was not read (card error) or its heal failed - refusing to write over it", g_loaded);
    app_log_flush();
    return false;
  }
  /* review F1: a refusal here still LEAVES the box dirty (it always did -- the invariant
   * refusal never touched g_dirty at all before this fix, which is exactly how it could
   * drift out of sync with g_box_unsaved_box: the marker said "unsaved", g_dirty could
   * independently already be false from an unrelated path, and the two facts came
   * apart). Setting both together here is the whole point of F1's invariant: marker set
   * implies g_dirty true, always. */
  if (!native_invariant_ok()) { g_box_unsaved_box = g_loaded; g_dirty = true; return false; }
  char path[SF_PATH_MAX]; box_path(g_loaded, path);
  /* Mirrors sf_save_rolling's own probe (savefile.c): a box file that already exists is
   * exactly the case where sf_save_rolling takes its backup -- so "did the target exist
   * before this call" IS "did this call back it up", without sf_save_rolling_ok needing
   * to hand that bit back out (BACKLOG #158 keeps its signature to path/buf/len/out_where).
   * review F2: f_stat(path, 0) like sf_save_rolling's own probe (savefile.c) -- no
   * FILINFO on this frame (its LFN buffers cost ~280 B) and no second directory read. */
  bool backed_up = f_stat(path, 0) == FR_OK;
  /* Sentinel outside the enum's range: sf_save_rolling_ok only writes *out_where when
   * sf_save_rolling actually returned SF_ERR_RENAME (savefile.h says so), so this value
   * surviving the call means "no ambiguity" -- either a plain success or a hard failure,
   * neither of which this function shows a dialog for (same as before this refactor). */
  SfWhere w = (SfWhere)-1;
  rmbl_pause();
  bool ok = sf_save_rolling_ok(path, box_recs(), BOX_BYTES, &w);
  rmbl_resume();
  if (w != (SfWhere)-1) box_save_rename_triage(path, w, ok, backed_up);
  else if (!ok) { log_line("bank: box save failed"); app_log_flush(); }
  /* review F1's invariant: g_box_unsaved_box set <=> g_dirty true, for the SAME box. */
  g_box_unsaved_box = ok ? -1 : g_loaded;
  g_dirty = !ok;
  return ok;
}

/* BACKLOG #163: box_save()'s own refusal (native-invariant OR the backup/rename triage)
 * used to be discarded silently by both dirty-flush callers below -- the UI kept going as
 * if the flush had landed, the box stayed unsaveable, and later edits in it were dropped
 * on the NEXT flip with only a log line. Retries box_save() with a blocking "try again?"
 * choice (A retry / B keep editing) between attempts; box_save() itself already showed
 * the specific reason on a failed rename (msg_wait, OBJ-bracketed) -- this is the one
 * question it cannot ask on the caller's behalf (review G4: it runs with no grid on
 * screen). false => the caller must NOT treat the box as flushed: g_dirty (or the
 * deletion queue) is left exactly as box_save() last left it, and the box must not be
 * paged away from (a silent flip would drop the very edits this exists to protect). A
 * hard cap (rule 2: every loop needs a provable bound) that a real user session will
 * never hit -- retries are gated on the user's own A/B choice, not a busy-loop.
 * Moved above pdna_bank_clear_slots (BACKLOG #181) so its own page-out flush can call
 * this directly instead of the bare, unchecked `if (g_dirty) box_save();` it used to
 * have -- pdna_bank_clear_slots is the THIRD site this discipline reaches (after
 * banksrc_records / pdna_bank_flush_deletions, both below, fixed by BACKLOG #163). */
static bool box_save_or_keep_dirty(void) {
  for (int tries = 0; tries < 100; tries++) {
    if (box_save()) return true;
    if (!app_can_edit()) return false;              /* read-only cart: nothing to retry */
    u16 dc = REG_DISPCNT;
    REG_DISPCNT &= ~DCNT_OBJ;                        /* same OBJ bracket box_save's own msg_wait uses */
    bool retry = app_confirm(PDNA_BANK_UNSAVED_BANNER, PDNA_BANK_RETRY_L1);
    REG_DISPCNT = dc;
    if (!retry) return false;                        /* B: keep editing -- box stays dirty */
    if (g_box_unread && box_unread_reread()) return true;   /* #382(a): the box was never read -- re-read it */
  }
  return false;
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
 * buffer would wipe the box's untouched bystander mons, and bank box files take only a single
 * rolling .bak (box_save's sf_save_rolling, BACKLOG #150 S150-0), not an immutable backup — there
 * is no earlier generation to fall back past that one file.
 * Returning false leaves the file untouched => the move degrades to a safe, recoverable DUPLICATE. */
bool pdna_bank_clear_slots(int box, const uint8_t* slots, const uint8_t (*recs80)[80], int n) {
  if (box < 0 || box >= BANK_BOXES || n <= 0) return false;
  if (g_loaded != box) {
    /* BACKLOG #181: same discipline banksrc_records / pdna_bank_flush_deletions already
     * got from BACKLOG #163 -- the box being LEFT must actually be saved before this
     * pages away from it. The old `if (g_dirty) box_save();` fired the flush and moved
     * on regardless of its verdict, so a failed flush's edits were gone the instant
     * box_load(box) below overwrote the one shared buffer, with only a log line to show
     * for it. box_save_or_keep_dirty() already leaves g_dirty (and the marker) set
     * exactly as box_save()'s own verdict would on failure -- refusing here just means
     * this function's own false (every caller already treats it as a safe D7 duplicate,
     * pdna_box.c:1153/1268/2216) instead of silently discarding the OLD box's edits. */
    if (g_dirty && !box_save_or_keep_dirty()) return false;
    if (!box_load(box)) return false;                           /* page-in must be COMPLETE */
  }
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

/* BACKLOG #150 S150-8 decision 10/G-M3: KEEP failures queued instead of silently
 * discarding them (the old unconditional `g_bank_ndel = 0` below lost a deletion
 * forever on a failed box_save()). Compacts the queue in place, returns the number
 * of deletions still queued (0 = every one flushed) -- a kept entry is harmless
 * (worst case a recoverable duplicate on the next visit), never a loss. */
int pdna_bank_flush_deletions(void) {
  int kept = 0;
  for (int i = 0; i < g_bank_ndel; i++) {
    int box = g_bank_del[i].box, slot = g_bank_del[i].slot;
    if (g_loaded != box) {
      /* BACKLOG #163: the box being LEFT must actually be saved before we page away from
       * it -- ignoring this flush's own false silently dropped whatever it was carrying. */
      if (g_dirty && !box_save_or_keep_dirty()) {
        log_line("bank: flush box %d slot %d deferred, box %d still unsaved", box, slot, g_loaded);
        if (kept != i) g_bank_del[kept] = g_bank_del[i];
        kept++;
        continue;                                    /* never page away from an unsaved box */
      }
      (void)box_load(box);   /* page the box in (reads its file) */
    }
    if (g_box_unread) {                              /* Zy D1: the page-in hit a card error -> the buffer is not that box */
      if (kept != i) g_bank_del[kept] = g_bank_del[i];
      kept++;
      continue;                                      /* keep the deletion queued; worst case a duplicate */
    }
    uint8_t* p = box_recs() + (uint32_t)slot * REC_BYTES;
    if (memcmp(p, g_bank_del[i].id, BANK_DEL_IDLEN) != 0) continue;    /* slot no longer holds OUR mon -> don't delete */
    memset(p, 0, REC_BYTES);
    g_dirty = true;
    if (!box_save()) {
      log_line("bank: flush box %d slot %d failed", box, slot);
      if (kept != i) g_bank_del[kept] = g_bank_del[i];
      kept++;
    }
  }
  g_bank_ndel = kept;
  return kept;
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
  /* BACKLOG #150 S150-3 decision 6: this function sets g_loaded directly (never calls
   * box_load()), so it must clear the snapshot itself -- an .pk3 import always packs
   * plain Gen-3 records, never native cells, but a STALE snapshot left over from
   * whatever box_load() last ran would false-refuse this function's own box_save()
   * calls below. */
  g_native_snap = 0;
  g_box_unread = false;
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
        g_native_snap = 0;                       /* same reasoning as the memset above */
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
  /* BACKLOG #371: bank.meta OR bank.meta.bak OR any boxNN.box -- an existing Bank is never "first run". */
  return bml_layout_exists(PDNA_BANK_DIR, BANK_BOXES);
}

/* BACKLOG #150 S150-4 decision 3: the one-shot immutable pre-#150 snapshot hard rule 3
 * wants before a mon's ONLY copy starts living in a box file. `/PokeDNA/bank/backup-v1/
 * DONE` is a DONE marker, never a count (G-H3/G-L1) -- every attempt REWRITES every
 * non-empty box rather than validating an existing copy, because sf_write_verified
 * already byte-compares the file it just wrote against the source buffer as part of
 * its own contract (savefile.h), so the rewrite IS the per-file validation. At most
 * 16 x 2400 B, written once ever (present marker -> O(1) f_stat and return).
 *
 * Modelled on migrate_flat_pk3 above: noinline (same 784-byte-frame reasoning), same
 * "own the box buffer, force a fresh page-in when done" idiom (g_loaded = -1).
 *
 * Stray-file deletion is DELIBERATELY NOT implemented (declared deviation from
 * SS11.1's prose): nothing here counts files, so a stray backup-v1/*.box left over
 * from an interrupted run cannot fool anything -- the marker is the only thing this
 * function or box_save() ever trusts, and the marker is written LAST. */
/* BACKLOG #219c: bank.meta (box names, wallpapers, the serial) shipped with NO copy in
 * backup-v1 -- only box*.box was ever snapshotted, so a bad meta write left the whole
 * ident32-uniqueness guarantee (G-M4) unrecoverable even though the boxes themselves
 * were safe. Backed up the same way (sf_write_verified, byte-compared as part of its
 * own contract -- same "the rewrite IS the validation" reasoning above), keyed by the
 * backup copy's OWN file existence rather than a second marker: a card whose boxes'
 * DONE marker already exists (an older card upgraded to this build) must NOT redo the
 * 16-box loop below, so `boxes_done` short-circuits it, but the meta gap on that same
 * card is healed by this direct probe regardless. This is the one deliberate exception
 * to "the marker is the only thing this function trusts" (declared deviation above):
 * bank.meta's own presence at the backup path IS its marker, because sf_write_verified
 * never leaves a partial file there (tmp -> byte-compare -> rename) -- there is no
 * "half-written" state to distrust, the same fact the box loop below already leans on
 * per file. */
static bool __attribute__((noinline)) bank_backup_v1(void) {
  char marker[SF_PATH_MAX];
  siprintf(marker, PDNA_BANK_DIR "/backup-v1/DONE");
  FILINFO fno;
  bool boxes_done = f_stat(marker, &fno) == FR_OK;

  char meta_bak[SF_PATH_MAX];
  siprintf(meta_bak, PDNA_BANK_DIR "/backup-v1/bank.meta");
  FILINFO mfno;
  bool meta_done = f_stat(meta_bak, &mfno) == FR_OK;

  if (boxes_done && meta_done) return true;             /* fully done -> O(1) */
  if (!app_can_edit()) return false;

  FRESULT mkr = f_mkdir(PDNA_BANK_DIR "/backup-v1");
  if (mkr != FR_OK && mkr != FR_EXIST) {
    log_line("bank: backup-v1 mkdir failed (%d)", (int)mkr);
    app_log_flush();
    return false;
  }

  if (!meta_done) {
    uint8_t metabuf[META_BYTES]; uint32_t msz = 0;
    if (sf_read_full(meta_path(), metabuf, sizeof metabuf, &msz) == SF_OK && msz >= META_BYTES && memcmp(metabuf, META_MAGIC, 6) == 0) {
      if (sf_write_verified(meta_bak, metabuf, META_BYTES) != SF_OK) {
        log_line("bank: backup-v1 meta write failed");
        app_log_flush();
        return false;
      }
      meta_done = true;
    }
    /* else: primary bank.meta doesn't exist yet (no Bank write has happened this
     * session) -- nothing to snapshot, not a failure; the NEXT call (once a serial
     * mint or a box open has written the primary) backs it up then. */
  }
  if (boxes_done) {
    if (!meta_done) { log_line("bank: backup-v1 meta snapshot skipped (bank.meta unreadable/absent)"); app_log_flush(); }
    return true;   /* review F3: the boxes ARE covered by the older backup-v1, and bank.meta
                    * carries its own rolling .bak (#168b) -- a missing meta snapshot is a
                    * best-effort gap, not grounds to refuse the write. A FAILED verified
                    * write above already returned false. */
  }

  if (g_dirty && !box_save_or_keep_dirty()) return false;   /* BACKLOG #181: never page away from an unsaved box */

  int count = 0;
  for (int b = 0; b < BANK_BOXES; b++) {
    char src[SF_PATH_MAX]; box_path(b, src);
    FILINFO sfno;
    /* Zy D5: an ABSENT primary may still be healable (box_load restores it from the verified .tmp/.bak, and
     * app_can_edit() is true here) -- heal first so that box lands in the backup instead of being skipped. */
    if (f_stat(src, 0) == FR_NO_FILE) (void)box_load(b);
    if (f_stat(src, &sfno) != FR_OK || (uint32_t)sfno.fsize != (uint32_t)BOX_BYTES) continue;   /* absent or wrong size -> skip */
    if (!box_load(b)) { log_line("bank: backup-v1 box %d page-in incomplete, refusing", b); app_log_flush(); return false; }
    char dst[SF_PATH_MAX];
    siprintf(dst, PDNA_BANK_DIR "/backup-v1/box%02d.box", b);
    if (sf_write_verified(dst, box_recs(), BOX_BYTES) != SF_OK) {
      log_line("bank: backup-v1 box %d write failed", b);
      app_log_flush();
      return false;                                     /* NO marker on any failure */
    }
    count++;
  }

  char mbuf[16];
  siprintf(mbuf, "%d %d\n", count, BOX_BYTES);
  if (sf_write_verified(marker, (const uint8_t*)mbuf, (uint32_t)strlen(mbuf)) != SF_OK) {
    log_line("bank: backup-v1 marker write failed");
    app_log_flush();
    return false;                                       /* a card that can't take 16 B can't take the 2400-B box write either */
  }
  g_loaded = -1;                                         /* force a fresh page-in, migrate_flat_pk3's own idiom */
  return true;
}

/* Public entry: called from drop_held BEFORE the cell is written (decision 3). */
bool pdna_bank_prepare_native(void) { return bank_backup_v1(); }

/* BACKLOG #150 S150-11 decision 19 -- the reconcile needs to READ every box without
 * the box screen open. Exactly banksrc_records()'s own shape (flush the CURRENTLY
 * loaded box first, via box_save_or_keep_dirty() -- b163's verdict path, so a refused
 * save is never silently paged over -- then page in `box`). NULL on a page-in failure
 * or an out-of-range box; the caller must not assume the returned pointer's contents
 * are complete without checking box_load()'s own return, which this wrapper folds
 * into "NULL means don't trust it" since a reconcile walk only ever wants to READ. */
const uint8_t* pdna_bank_peek_box(int box) {
  if (box < 0 || box >= BANK_BOXES) return NULL;
  if (g_dirty && !box_save_or_keep_dirty()) return NULL;
  if (!box_load(box)) return NULL;
  return box_recs();
}

/* BACKLOG #150 S150-11 decision 8c/19 -- RESTORE TO BANK's write path: a genuinely
 * NEW Bank write with no merge (only ever called for a *_LOST row, where there is no
 * Gen-3 copy to fold in). Omega-gated FIRST. Refuses unless the target slot is
 * entirely zero -- a native cell is NEVER all-zero (bc_is_native's own false-positive
 * analysis) and native_invariant_ok() already treats "still native OR all-zero" as
 * the only legitimate transitions, so writing into anything else would silently
 * clobber whatever the user has there. On a failed box_save() the buffer's copy of
 * the slot is re-zeroed (so a retry sees the same "empty" state this call started
 * from) and the entry stays intact -- the caller shows PDNA_XRC_NOBANK_L1. */
bool pdna_bank_put_cell(int box, int slot, const uint8_t cell80[80]) {
  if (!app_can_edit()) return false;
  if (box < 0 || box >= BANK_BOXES || slot < 0 || slot >= BOX_RECS || !cell80) return false;
  if (g_dirty && !box_save_or_keep_dirty()) return false;
  if (!box_load(box)) return false;
  uint8_t* p = box_recs() + (uint32_t)slot * REC_BYTES;
  for (int i = 0; i < REC_BYTES; i++) if (p[i]) return false;   /* slot not all-zero -- refuse */
  memcpy(p, cell80, REC_BYTES);
  g_native_snap |= (1u << slot);
  g_dirty = true;
  if (!box_save_or_keep_dirty()) {
    memset(p, 0, REC_BYTES);
    g_native_snap &= ~(1u << slot);
    return false;
  }
  return true;
}

/* ---- BoxSource hooks (singleton state) ---- */
static uint8_t* banksrc_records(int box) {
  if (box != g_loaded) {
    /* BACKLOG #163: ignoring this flush's own false used to page ahead anyway -- the box
     * being left silently kept its edits only in RAM, gone the instant box_load(box)
     * below overwrites the one shared buffer. Refuse to page: never silently flip. */
    if (g_dirty && !box_save_or_keep_dirty()) return box_recs();
    box_load(box);
  }
  /* BACKLOG #382(a): never show a phantom-empty / unwritable box silently -- once per page-in, whoever paged it
   * (the reconcile / backup helpers page boxes too, so the first look at it here is the one that tells the user). */
  if (g_box_unread && !(g_box_unread & BOX_UNREAD_NOTED)) { box_unread_notice(); g_box_unread |= BOX_UNREAD_NOTED; }
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
  /* Zy D4: a refused/failed meta write used to be silent (state 2 after a card read error refuses outright,
   * and the box write is refused too then -- nothing at all appeared on screen). */
  if (!meta_save())
    msg_wait(PDNA_BANK_META_TITLE, UI_WARN, PDNA_BANK_META_L1, PDNA_BANK_META_L2);
  return ok;
}
static void banksrc_mark_dirty(void) { g_dirty = true; }   /* moves: deferred to box-switch/exit */

/* BACKLOG #150 S150-12 decision 8: which box the Bank opens on -- consumed ONCE by
 * pdna_bank_show() below, then reset, so it never sticks past the visit it was set
 * for. 1 B plain .bss. Out-of-range values (a defensive clamp, never expected in
 * practice: the only writer is gb_ro_exit_offer with a box index the drop itself
 * just wrote into) fall back to box 0 rather than an out-of-bounds start_box. */
static uint8_t s_start_box;

void pdna_bank_start_box_set(int box) {
  s_start_box = (box >= 0 && box < BANK_BOXES) ? (uint8_t)box : 0;
}

int pdna_bank_show(void) {
  f_mkdir("/PokeDNA");
  f_mkdir(PDNA_BANK_DIR);

  if (app_can_edit() && !layout_exists()) migrate_flat_pk3();   /* first run: import old .pk3 */
  /* BACKLOG #371: a restore from .bak rewrites the primary NOW (verified-write), before any other
   * meta_save() can roll .bak forward; meta_save()'s from-bak unlink step keeps .bak intact. A failed
   * heal leaves the primary absent and .bak untouched, so the next open simply retries. */
  if (meta_load() && g_meta_from_bak && app_can_edit() && !meta_save()) {
    log_line("bank: meta heal from bank.meta.bak failed - .bak kept, will retry next open");
    app_log_flush();
  }
  /* review F1: a fresh session starts with no box resident and nothing pending -- the
   * unsaved marker from any PRIOR session must not survive to describe a box this one
   * has not touched yet. */
  g_loaded = -1; g_dirty = false; g_box_unsaved_box = -1;

  app_xfer_reconcile_bank_open();   /* BACKLOG #150 S150-11 decision 4/§11.8 */

  BoxSource s; memset(&s, 0, sizeof s);
  s.nboxes     = BANK_BOXES;
  /* BACKLOG #150 S150-12 decision 8: the box the last COPY landed in, if the read-only
   * mount's exit offer just opened the Bank for its A-branch; box 0 otherwise (the
   * shipped default). Consumed once -- a later ordinary Bank visit does not inherit it. */
  s.start_box  = (s_start_box < BANK_BOXES) ? s_start_box : 0;
  s_start_box  = 0;
  s.is_bank    = true;
  s.scope      = BOXSCOPE_BANK;  /* BACKLOG #120 S1: can_lift/xfer stay NULL (memset above) */
  /* BACKLOG #244: box_names_supported stays NULL (memset above) -- the Bank always
   * has a box-name table; the shortcut/menu's writability gate (can_edit) still
   * refuses on an EverDrive, silently, exactly as before. */
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
   * so this prompt only covers the CURRENT box — say exactly that.
   *
   * BACKLOG #163: on A this used to call box_save() and clear g_dirty UNCONDITIONALLY --
   * a false was reported to the log only, while the screen carried on as if the save had
   * landed. There is no "keep editing" to fall back to at this exit point (the box screen
   * has already returned), so box_save_or_keep_dirty()'s own retry loop is this box's LAST
   * chance; if the user gives up on it (or the save never succeeds), that really does mean
   * the edits are gone the moment g_bankbuf is next reused -- say so plainly, both here
   * and on a deliberate B-discard, instead of a bare silent reload. */
  if (g_dirty) {
    if (app_can_edit() && app_confirm("Save this bank box?", "Prior boxes auto-saved.")) {
      if (!box_save_or_keep_dirty())
        msg_wait(PDNA_BANK_UNSAVED_BANNER, UI_WARN, PDNA_BANK_LOST_L1,
                  PDNA_BANK_LOST_L2);
    } else {
      if (g_loaded >= 0) box_load(g_loaded);        /* discard: reload the box from its file */
      msg_wait(PDNA_BANK_UNSAVED_BANNER, UI_WARN, PDNA_BANK_DISCARD_L1, PDNA_BANK_DISCARD_L2);
    }
    /* review F1: whatever box_save_or_keep_dirty()'s last attempt left in
     * g_box_unsaved_box, this is the box's LAST chance (the screen has already
     * returned) -- clear both together, or a failed-then-abandoned save here leaves
     * the marker standing to wrongly flag a DIFFERENT box the next time this index is
     * loaded (box_load's own g_box_unsaved_box clear only fires on the NEXT box_load,
     * not retroactively for the one already resident). The on-card file is untouched
     * (never a corruption, only the lost edits already said above) either way. */
    g_dirty = false;
    g_box_unsaved_box = -1;
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
