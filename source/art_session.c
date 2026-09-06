/* art_session.c — see art_session.h. */
#include "art_session.h"

#include <string.h>

#include "ff.h"
#include "artbuf.h"  /* mon_decomp — the shared 8 KiB scratch, reused for FNV streaming */
#include "rmbl.h"    /* rmbl_pause/resume — freeze the motor across every SD transfer   */
#include "log.h"

/* One memo slot per kind: 0 = not yet checked this epoch, 1 = checked, ready to
 * serve, -1 = checked, refused. IWRAM .bss, ART_KIND_COUNT (8) bytes — well inside
 * the "no new IWRAM buffer over ~256 B" guidance (DESIGN.md Sec 4.2's own ceiling
 * for its OWN suggested additions; this is 8 B). */
static int8_t s_verdict[ART_KIND_COUNT];

/* WHY the last kind_ready() answered the way it did, as a short phrase for the log.
 * Every failure below already wrote a distinct log line EXCEPT the two most common
 * ones -- art.idx simply absent (a card that never ran "Extract art") and this kind's
 * bit never set -- which returned silently, so a log said only "kind 0 not ready" and
 * left the reader guessing between "no cache" and "broken cache". A pointer to a
 * string literal: no storage beyond the pointer, and pure C (this file is compiled by
 * tests/host_artsession_test.c too). */
static const char* s_why = "not checked";

const char* art_session_why(void) { return s_why; }

void art_session_invalidate(void) {
  memset(s_verdict, 0, sizeof s_verdict);
  s_why = "not checked";       /* a stale reason outliving its verdict would mislead */
}

/* Read /PokeDNA/art/art.idx (at most 160 B for format v1: 32 + 8*16) and validate its
 * own shape. Returns the parsed head/rows on ART_IDX_OK; false status otherwise
 * (art.idx absent, truncated, wrong magic/format — every case DESIGN.md Sec 2.2
 * calls out is refused here, before a single kind is even considered). */
static bool read_idx(ArtIdxHead* head, ArtIdxKindRow rows[ART_KIND_COUNT], int* nrows) {
  FIL f;
  if (f_open(&f, ART_IDX_PATH, FA_READ) != FR_OK) {
    s_why = "art.idx absent (never extracted)";
    return false;                                               /* absent: no cache at all */
  }
  uint8_t buf[ART_IDX_HEAD_BYTES + ART_KIND_COUNT * ART_IDX_KIND_BYTES];
  UINT br = 0;
  FRESULT fr = f_read(&f, buf, sizeof buf, &br);
  f_close(&f);
  if (fr != FR_OK) { s_why = "art.idx unreadable"; return false; }
  ArtIdxParseStatus st = art_idx_parse(buf, br, head, rows, nrows);
  if (st != ART_IDX_OK) {
    s_why = "art.idx rejected";
    log_line("art cache: art.idx rejected (status %d)", (int)st);
    return false;
  }
  return true;
}

/* FatFs-backed ArtReadFn for art_fnv_of_stream: reads a plain file by absolute
 * offset, matching the (ctx, off, dst, len)->bool shape art_cache.h's ArtReadFn
 * declares. `ctx` is the already-open FIL*. */
static bool fil_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FIL* f = (FIL*)ctx;
  if (f->fptr != off && f_lseek(f, off) != FR_OK) return false;
  UINT br = 0;
  return f_read(f, dst, len, &br) == FR_OK && br == len;
}

/* deep=true re-hashes the WHOLE kind file (up to 451 KB for icons.bin) against
 * art.idx's stored FNV -- the only way to catch bit-level corruption of an
 * otherwise-correctly-sized file. deep=false trusts the file's SIZE alone -- still a
 * real check: it catches a truncated/interrupted/replaced file, exactly the failure
 * mode art.idx's own invariant exists to make impossible (art_cache.h's header
 * comment). TRADE-OFF ACCEPTED for the shallow path: silent same-size corruption (a
 * flipped bit from bit rot, a card re-written by a PC with a byte-identical-length
 * but different-content file) is not caught until the next deep check. Judged
 * acceptable for the path that runs on EVERY boot, given the bytes were already
 * verified byte-exact at write time (sf_write_verified_stream's own re-derive-and-
 * compare pass) and a deep check still runs once, automatically, right after every
 * fresh extraction (art_extract_screen's app_icon_cache_resolve call). */
static bool verify_kind_file(ArtKind k, const ArtIdxKindRow* row, bool deep) {
  const char* path = art_kind_filename(k);
  if (!path) return false;
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    s_why = "the kind file is missing";
    log_line("art cache: %s missing (art.idx promised it)", path);
    return false;
  }
  bool size_ok = (uint32_t)f_size(&f) == row->bytes;
  bool ok = size_ok;
  if (size_ok && deep) {
    uint32_t fnv = 0;
    artbuf_claim();   /* E3 review BLOCKING 2: the stream below fills mon_decomp per chunk */
    ok = art_fnv_of_stream(fil_read, &f, row->bytes, (uint8_t*)mon_decomp, 8192u, &fnv) &&
         fnv == row->fnv;
    if (!ok) { s_why = "the kind file failed its FNV check";
               log_line("art cache: %s failed its stored FNV check", path); }
  }
  f_close(&f);
  if (!size_ok) { s_why = "the kind file is the wrong size";
                  log_line("art cache: %s size mismatch (art.idx says %lu)", path,
                           (unsigned long)row->bytes); }
  return ok;
}

static bool kind_ready(ArtKind k, const RomCtx* rc, bool deep) {
  if ((unsigned)k >= ART_KIND_COUNT) return false;
  if (s_verdict[k] != 0) return s_verdict[k] > 0;

  bool ok = false;
  rmbl_pause(); /* the whole check is an SD-transfer session — freeze the motor once,
                   not per f_read (matches app_icon_rom_open's own bracketing) */
  do {
    ArtIdxHead head;
    ArtIdxKindRow rows[ART_KIND_COUNT];
    int n = 0;
    if (!read_idx(&head, rows, &n)) break;

    const ArtIdxKindRow* row = art_idx_find(rows, n, k);
    if (!row) { s_why = "this kind was never extracted"; break; }

    if (rc && rc->kind != ROM_NONE) {
      uint32_t rom_fnv;
      if (!art_rom_fnv(rc->read, rc->ctx, rc->size, &rom_fnv)) {
        s_why = "could not hash the open rom";
        log_line("art cache: could not hash the open rom to cross-check art.idx");
        break;
      }
      if (!art_idx_matches_rom(&head, rc, rom_fnv)) {
        s_why = "art.idx is for a DIFFERENT rom";
        log_line("art cache: art.idx does not match the currently open rom");
        break;
      }
    }
    /* No rc: no ROM is open this session, so there is nothing to cross-check
     * against — the cache is trusted on ITS OWN validity alone (DESIGN.md Sec 4.1:
     * "the card outlives the registration"). */

    ok = verify_kind_file(k, row, deep);
  } while (0);
  rmbl_resume();

  if (ok) s_why = deep ? "verified (deep)" : "verified (size only)";

  s_verdict[k] = ok ? 1 : -1;
  log_line("art cache: kind %d %s (%s check)", (int)k, ok ? "ready" : "not ready",
           deep ? "deep" : "shallow");
  return ok;
}

bool art_session_kind_ready(ArtKind k, const RomCtx* rc) { return kind_ready(k, rc, true); }
bool art_session_kind_ready_shallow(ArtKind k, const RomCtx* rc) {
  return kind_ready(k, rc, false);
}

bool art_session_icons_ready(const RomCtx* rc) {
  return kind_ready(ART_KIND_ICONS, rc, true);
}
bool art_session_icons_ready_shallow(const RomCtx* rc) {
  return kind_ready(ART_KIND_ICONS, rc, false);
}

bool art_session_icons_ready_memoized(void) {
  return s_verdict[ART_KIND_ICONS] > 0;
}

const char* art_session_icons_path(void) { return art_kind_filename(ART_KIND_ICONS); }
