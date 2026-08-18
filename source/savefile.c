#include "savefile.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "log.h"
#include "sys.h"   /* EWRAM_BSS */

/* Shared 4 KiB compare/copy chunk in EWRAM (.bss), never on the IWRAM stack. */
/* 2 KiB, not 4: EWRAM is genuinely full since the map feature landed, and this only
 * affects copy/compare throughput on a 128 KiB file. Must stay >= 512 and 4-byte aligned
 * so copy_file keeps hitting disk_write's DIRECT path instead of the bounce buffer. */
static uint8_t EWRAM_BSS s_cmp[2048];

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

  static uint8_t EWRAM_BSS bufb[2048];   /* see the note on s_cmp */
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

/* THE STACK RULE FOR THIS FILE. FatFs' FIL carries a 512-byte sector buffer (~560 bytes)
 * and FILINFO an LFN name buffer (~290, FF_LFN_BUF=255), on a 12,624-byte IWRAM stack --
 * d95202f is what one careless frame there costs. So sf_write_verified keeps NOTHING big
 * of its own (just the 272-byte scratch path) and each step -- write, compare, read-back --
 * borrows its buffer in a SIBLING frame that is gone before the next one starts. Measured
 * with -fstack-usage (-O2, thumb): sf_write_verified's own frame fell 904 -> 296, and the
 * deepest nesting on the write path is now 296 + 624 (file_matches) = 920 bytes against
 * the old 904 -- sixteen bytes, for three steps that are each read back. */

/* Write buf(len) to `path`, creating/truncating it. Own frame (see above). */
__attribute__((noinline))
static SfStatus write_all(const char* path, const uint8_t* buf, uint32_t len) {
  FIL f;
  if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return SF_ERR_OPEN;
  UINT bw = 0;
  FRESULT fr = f_write(&f, buf, len, &bw);
  FRESULT fc = f_close(&f);
  if (fr != FR_OK || bw != len || fc != FR_OK) return SF_ERR_WRITE;
  return SF_OK;
}

/* Does the file at `path` hold exactly buf(len), as read back off the CARD?
 *
 * The verified-write's byte-compare, lifted out so the write and the after-the-fact
 * "where did my bytes end up" query cannot drift apart -- and so its FIL lives in its own
 * frame instead of sf_write_verified's. */
__attribute__((noinline))
static bool file_matches(const char* path, const uint8_t* buf, uint32_t len) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  if (f_size(&f) != (FSIZE_t)len) { f_close(&f); return false; }
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
  return ok;
}

/* Size of `path` per the card, or -1 if it cannot be read. Own frame, same reason. */
__attribute__((noinline))
static long file_size_on_card(const char* path) {
  FILINFO fno;
  if (f_stat(path, &fno) != FR_OK) return -1;
  return (long)fno.fsize;
}

/* Did the "<x>.tmp -> <x>" swap actually reach the CARD?
 *
 * f_rename returning FR_OK is not evidence. An EZ-Flash write has no retry and no
 * read-back (flashcartio_write_sector hands back _EZFO_writeSectors' verdict and nothing
 * ever re-reads it), so a card, a dying contact, or a bad SD copy that ACKs a sector it
 * never stores is invisible to every layer above it -- the same lesson the log learned in
 * 94d9f0c. Every step before the rename was already read back byte-for-byte; the rename
 * was the one still taken on trust, and tests/host_savefat_test.c's sweep found the exact
 * two-write-wide window where it costs the user their .sav (k=260..261 of a 270-write
 * commit, reproduced there).
 *
 * Two facts, both required, both pure reads:
 *   - `path` exists at exactly `len` bytes, and
 *   - `tmp` is GONE.
 * The second is what makes this exact rather than a heuristic. A rename is one directory
 * entry being rewritten: if that write landed, the new name appears and the scratch name
 * disappears together; if it was swallowed, the scratch name is still there. Size alone
 * would have passed a swallowed rename that left a STALE file of the same size under the
 * target name -- and a .sav is always the same size as the .sav it replaces, so that is
 * the common case here, not a corner one.
 *
 * f_stat failing for any OTHER reason is NOT a pass. An unverifiable swap is the bug
 * again; the caller is told it failed and nothing is deleted, so the user still holds
 * every copy they held a moment ago.
 *
 * noinline on purpose: FILINFO carries an LFN name buffer (~290 bytes, FF_LFN_BUF=255) and
 * the callers already have FatFs' 512-byte FIL (or two of them, via copy_file) live on the
 * 12,624-byte IWRAM stack. Sibling frames, never nested -- d95202f is what a careless
 * frame on that stack costs. */
__attribute__((noinline))
static bool swap_landed(const char* path, const char* tmp, uint32_t len) {
  FILINFO fno;
  if (f_stat(path, &fno) != FR_OK) return false;      /* missing, or cannot vouch */
  if ((uint32_t)fno.fsize != len)  return false;      /* short: the card kept part of it */
  if (f_stat(tmp, &fno) != FR_NO_FILE) return false;  /* scratch still there => no rename */
  return true;
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
    /* All 21 slots taken. This used to FAIL — which blocked EVERY save with
     * "BACKUP FAILED - Save NOT modified" until the user found Clear backups.
     * A full shelf must never stop a save: overwrite the HIGHEST slot instead
     * (the 20 older backups survive) and say so in the log. */
    siprintf(bak, "%s.bak20", src_path);
    log_line("backup: all slots full - replacing %s", bak);
    f_unlink(bak);
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

/* Single ROLLING backup: keep one "<src>.bak" without it piling up. Build the new
 * copy in a scratch "<src>.baktmp", verify it byte-for-byte, and only THEN swap it
 * into place (unlink + rename). This mirrors sf_write_verified's invariant: the
 * previous good "<src>.bak" survives until a verified replacement exists, so a
 * mid-copy SD failure never leaves the user with a truncated/missing backup. */
SfStatus sf_backup_rolling(const char* src_path, char* out_bak, unsigned out_bak_cap) {
  char bak[SF_PATH_MAX], tmp[SF_PATH_MAX];
  siprintf(bak, "%s.bak", src_path);
  siprintf(tmp, "%s.baktmp", src_path);
  long want = file_size_on_card(src_path);       /* for the read-back after the swap */
  SfStatus st = copy_file(src_path, tmp);
  if (st != SF_OK) { f_unlink(tmp); log_line("rolling backup: copy failed (%s)", sf_status_str(st)); return SF_ERR_BACKUP; }
  bool eq = false;
  st = files_equal(src_path, tmp, &eq);
  if (st != SF_OK || !eq) { f_unlink(tmp); log_line("rolling backup: verify failed"); return SF_ERR_BACKUP; }
  f_unlink(bak);                                 /* safe now: a verified replacement exists */
  if (f_rename(tmp, bak) != FR_OK) {
    /* Do NOT delete the tmp here. By this line it is a byte-for-byte VERIFIED copy of the
     * save and the previous good .bak has already been unlinked, so this unlink is the one
     * action in the function that can remove the user's last copy. The two branches above
     * delete their tmp because theirs is known-BAD (the copy or the compare failed); this
     * one is known-GOOD, which is the whole difference. Same rule as the read-back below
     * and as sf_write_verified: a failing write must never be the thing that throws away a
     * good copy.
     *
     * Honesty about the evidence: the single-transient-error sweep in
     * tests/host_savefat_test.c could NOT drive this to a zero-backup end state, because a
     * FatFs rename that returns an error often still lands -- its directory window is
     * dirty and the next operation flushes it. So this is defense in depth, not a
     * reproduced loss; what the test does pin is that the verified copy is still there
     * under one name or the other. Keeping it is never worse: if the rename did land, this
     * unlink was a no-op on an already-gone file anyway. */
    log_line("rolling backup: rename failed; verified copy kept in %s", tmp);
    return SF_ERR_BACKUP;
  }
  /* Same read-back as sf_write_verified, and for the same reason: an unread rename means
   * "rolling backup OK" can be printed over a card with no .bak on it -- and the commit
   * that follows this call would then proceed believing a backup exists. Note the .baktmp
   * is NOT deleted here: if the swap did not land, that file is the backup. */
  if (want < 0 || !swap_landed(bak, tmp, (uint32_t)want)) {
    log_line("rolling backup: card did not keep the swap (%s may hold it)", tmp);
    return SF_ERR_BACKUP;
  }
  if (out_bak && out_bak_cap) { strncpy(out_bak, bak, out_bak_cap - 1); out_bak[out_bak_cap - 1] = 0; }
  log_line("rolling backup OK -> %s", bak);
  return SF_OK;
}

SfStatus sf_copy(const char* src_path, const char* dst_path) {
  SfStatus st = copy_file(src_path, dst_path);
  if (st != SF_OK) { log_line("copy: failed (%s)", sf_status_str(st)); return st; }
  bool eq = false;
  st = files_equal(src_path, dst_path, &eq);
  if (st != SF_OK || !eq) { log_line("copy: verify failed"); return SF_ERR_VERIFY; }
  log_line("copy OK %s -> %s", src_path, dst_path);
  return SF_OK;
}

/* Delete every "<src>.bak" / ".bakN" file (and a leftover ".baktmp"). Returns the
 * count removed. Guy reported this "broken" on hardware, so it is self-diagnosing
 * now: every candidate's FRESULT is logged (a mismatch between what the writer
 * named and what we sweep shows up in /PokeDNA's log instead of as a silent 0),
 * the sweep runs past the writer's 21-slot ceiling in case older builds left more,
 * and it only stops after several consecutive holes (the writer names densely). */
int sf_clear_backups(const char* src_path) {
  char bak[SF_PATH_MAX]; int removed = 0, misses = 0;
  for (int n = 0; n <= 99 && misses < 5; n++) {
    if (n == 0) siprintf(bak, "%s.bak", src_path);
    else        siprintf(bak, "%s.bak%d", src_path, n);
    FRESULT fr = f_unlink(bak);
    if (fr == FR_OK) { removed++; misses = 0; log_line("clear: removed %s", bak); }
    else {
      if (fr != FR_NO_FILE) log_line("clear: %s -> fr=%d", bak, (int)fr);
      misses++;
    }
  }
  siprintf(bak, "%s.baktmp", src_path);            /* a crashed rolling backup's scratch */
  if (f_unlink(bak) == FR_OK) { removed++; log_line("clear: removed %s", bak); }
  log_line("clear: %d removed for %s", removed, src_path);
  return removed;
}

SfStatus sf_write_verified(const char* path, const uint8_t* buf, uint32_t len) {
  char tmp[SF_PATH_MAX];
  siprintf(tmp, "%s.tmp", path);

  /* 1) write temp */
  SfStatus wst = write_all(tmp, buf, len);
  if (wst != SF_OK) { f_unlink(tmp); return wst; }

  /* 2) re-read temp and byte-compare to the intended buffer */
  if (!file_matches(tmp, buf, len)) { f_unlink(tmp); return SF_ERR_VERIFY; }

  /* 3) swap into place (original only disappears once temp is verified) */
  f_unlink(path); /* ignore error if absent */
  if (f_rename(tmp, path) != FR_OK) {
    /* The original may still be there (a swallowed unlink makes f_rename say FR_EXIST) or
     * it may be gone -- either way the verified bytes are in the .tmp, so KEEP IT. */
    log_line("write: rename %s -> %s failed; bytes kept in the .tmp", tmp, path);
    return SF_ERR_RENAME;
  }

  /* 4) ...and read the CARD back, because f_rename returning FR_OK is not evidence that
   * anything landed (see swap_landed). Nothing is deleted on this path: the user is left
   * holding either the new save under its own name, the old one, or the complete verified
   * .tmp -- and now the tool SAYS which, instead of painting "SAVED" over an empty card. */
  if (!swap_landed(path, tmp, len)) {
    log_line("write: card did not keep the rename - %s holds the verified bytes", tmp);
    return SF_ERR_RENAME;
  }
  return SF_OK;
}

/* Ask the card, do not guess. Pure reads; nothing here deletes or writes anything. */
SfWhere sf_where_are_the_bytes(const char* path, const uint8_t* buf, uint32_t len) {
  char tmp[SF_PATH_MAX];
  siprintf(tmp, "%s.tmp", path);
  if (file_matches(tmp, buf, len))
    return file_exists(path) ? SF_WHERE_TMP_AND_OLD : SF_WHERE_TMP_ONLY;
  if (file_matches(path, buf, len)) return SF_WHERE_TARGET;
  return SF_WHERE_NEITHER;
}
