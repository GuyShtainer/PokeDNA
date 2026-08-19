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

void art_session_invalidate(void) {
  memset(s_verdict, 0, sizeof s_verdict);
}

/* Read /PokeDNA/art/art.idx (at most 160 B for format v1: 32 + 8*16) and validate its
 * own shape. Returns the parsed head/rows on ART_IDX_OK; false status otherwise
 * (art.idx absent, truncated, wrong magic/format — every case DESIGN.md Sec 2.2
 * calls out is refused here, before a single kind is even considered). */
static bool read_idx(ArtIdxHead* head, ArtIdxKindRow rows[ART_KIND_COUNT], int* nrows) {
  FIL f;
  if (f_open(&f, ART_IDX_PATH, FA_READ) != FR_OK) return false; /* absent: no cache at all */
  uint8_t buf[ART_IDX_HEAD_BYTES + ART_KIND_COUNT * ART_IDX_KIND_BYTES];
  UINT br = 0;
  FRESULT fr = f_read(&f, buf, sizeof buf, &br);
  f_close(&f);
  if (fr != FR_OK) return false;
  ArtIdxParseStatus st = art_idx_parse(buf, br, head, rows, nrows);
  if (st != ART_IDX_OK) {
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

static bool verify_kind_file(ArtKind k, const ArtIdxKindRow* row) {
  const char* path = art_kind_filename(k);
  if (!path) return false;
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    log_line("art cache: %s missing (art.idx promised it)", path);
    return false;
  }
  bool size_ok = (uint32_t)f_size(&f) == row->bytes;
  uint32_t fnv = 0;
  bool fnv_ok = size_ok &&
                art_fnv_of_stream(fil_read, &f, row->bytes, (uint8_t*)mon_decomp,
                                  8192u, &fnv) &&
                fnv == row->fnv;
  f_close(&f);
  if (!size_ok) log_line("art cache: %s size mismatch (art.idx says %lu)", path,
                         (unsigned long)row->bytes);
  else if (!fnv_ok) log_line("art cache: %s failed its stored FNV check", path);
  return fnv_ok;
}

bool art_session_kind_ready(ArtKind k, const RomCtx* rc) {
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
    if (!row) break; /* this kind's bit was never set -- nothing was ever extracted */

    if (rc && rc->kind != ROM_NONE) {
      uint32_t rom_fnv;
      if (!art_rom_fnv(rc->read, rc->ctx, rc->size, &rom_fnv)) {
        log_line("art cache: could not hash the open rom to cross-check art.idx");
        break;
      }
      if (!art_idx_matches_rom(&head, rc, rom_fnv)) {
        log_line("art cache: art.idx does not match the currently open rom");
        break;
      }
    }
    /* No rc: no ROM is open this session, so there is nothing to cross-check
     * against — the cache is trusted on ITS OWN validity alone (DESIGN.md Sec 4.1:
     * "the card outlives the registration"). */

    ok = verify_kind_file(k, row);
  } while (0);
  rmbl_resume();

  s_verdict[k] = ok ? 1 : -1;
  log_line("art cache: kind %d %s", (int)k, ok ? "ready" : "not ready");
  return ok;
}

bool art_session_icons_ready(const RomCtx* rc) {
  return art_session_kind_ready(ART_KIND_ICONS, rc);
}

bool art_session_icons_ready_memoized(void) {
  return s_verdict[ART_KIND_ICONS] > 0;
}

const char* art_session_icons_path(void) { return art_kind_filename(ART_KIND_ICONS); }
