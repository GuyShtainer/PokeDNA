#include "xfer_io.h"
#include "pdna_app.h"      /* PDNA_XFER_DIR, PDNA_SIDECAR_DIR, app_can_edit          */
#include "log.h"
#include "ff.h"
#include <string.h>
#ifdef PDNA_DELTA
#include "bank_plant.h"    /* BACKLOG #150 S150-9 decision 12: bank_plant_xfer_open -- the
                            * delta vehicle has no readable FAT, so a planted in-RAM
                            * ledger entry is the only way the merge screen is
                            * reachable on the emulator. Zero effect on either gate
                            * build (PDNA_DELTA is never defined there). */
#endif

/* ---- BACKLOG #213: the negative-key ring (pure logic, caller-owned storage) --- */

void xr_miss_cache_reset(XrMissCache* c) {
  if (!c) return;
  c->n = 0;
  c->next = 0;
}

bool xr_miss_cache_has(const XrMissCache* c, uint64_t key) {
  if (!c) return false;
  for (int i = 0; i < c->n; i++) if (c->keys[i] == key) return true;
  return false;
}

void xr_miss_cache_remember(XrMissCache* c, uint64_t key) {
  if (!c) return;
  if (xr_miss_cache_has(c, key)) return;   /* golden rule 2: the ring never grows past N */
  c->keys[c->next] = key;
  c->next = (uint8_t)((c->next + 1) % XR_MISS_RING_N);
  if (c->n < XR_MISS_RING_N) c->n++;
}

/* ---- path resolution (decision 4/D-Q7) --------------------------------------- */

/* out[GBSC_PATH_MAX] <- "<dir>/<name>", the same shape gbsc_path() builds from a
 * key, but from an already-formed filename instead. Returns the length written, or
 * -1 if it would not fit (out left untouched) or an argument is NULL. */
static int join_path(char* out, int cap, const char* dir, const char* name) {
  if (!out || !dir || !name || cap <= 0) return -1;
  int dl = 0; while (dir[dl]) dl++;
  int nl = 0; while (name[nl]) nl++;
  int need = dl + 1 + nl + 1;
  if (need > cap) return -1;
  int p = 0;
  for (int i = 0; i < dl; i++) out[p++] = dir[i];
  out[p++] = '/';
  for (int i = 0; i < nl; i++) out[p++] = name[i];
  out[p] = 0;
  return p;
}

/* BACKLOG #150 S150-6 review F3: the sidecar fallback is gated on the MIGRATED
 * marker. Once a migration has actually completed, /PokeDNA/xfer IS the ledger and
 * every sidecar source is an inert backup -- preferring a sidecar file over an
 * ABSENT xfer file at that point would resurrect a record a claim/KEEP/re-key
 * write already updated only in xfer (the sidecar copy is frozen at whatever it
 * held at migration time). The fallback exists ONLY for the case decision 4/D-Q7
 * was written for: migration never ran at all (read-only cart, EverDrive, or a
 * failed pass), so xfer might not even hold a copy of a record that is still
 * genuinely live in sidecar. */
static bool marker_present(void) {
  char marker[GBSC_PATH_MAX] = {0};
  if (join_path(marker, GBSC_PATH_MAX, PDNA_XFER_DIR, "MIGRATED") < 0) return false;
  FILINFO fi;
  return f_stat(marker, &fi) == FR_OK;
}

bool xr_migrated(void) { return marker_present(); }

/* BACKLOG #213: one f_stat, no cached state (see xfer_io.h's own comment). */
bool xr_dir_exists(void) {
  FILINFO fi;
  return f_stat(PDNA_XFER_DIR, &fi) == FR_OK;
}

/* #415: THE orphan-.tmp predicate on the first GBSC_HEADER bytes (xr_tmp_present) AND on the whole file (xr_read_orphan_tmp).
 * Both call gbsc_header_ok + the key compare through here, so presence and acceptance share one header test; acceptance
 * adds gbsc_count's entry crc16s on top (the probe cannot afford a 1 KB buffer on its frame). */
bool xr_orphan_hdr_ok(const uint8_t* hdr, uint32_t fsize, uint64_t key) {
  return gbsc_header_ok(hdr, fsize) && gbsc_header_key(hdr) == key;
}

/* #392(a)/#415: is there a "<path>.tmp" a reader would accept? Opens it, reads the header, checks magic/version/count/
 * exact size/header crc/key. Torn, foreign, zero-length or unreadable -> false (the same "absent" xr_open reports).
 * (own noinline frame: the 48 B path copy stays off xr_path_for_key_hint's) */
static __attribute__((noinline)) bool xr_tmp_present(const char* path, uint64_t key) {
  char t[GBSC_PATH_MAX + 8];
  uint8_t hdr[GBSC_HEADER] = {0};
  FILINFO fi;
  uint32_t got = 0;
  size_t n = strlen(path);
  if (n + 5 > sizeof t) return false;
  memcpy(t, path, n); memcpy(t + n, ".tmp", 5);
  if (f_stat(t, &fi) != FR_OK) return false;
  if (sf_read_full(t, hdr, GBSC_HEADER, &got) != SF_OK || got != GBSC_HEADER) return false;
  return xr_orphan_hdr_ok(hdr, (uint32_t)fi.fsize, key);
}

bool xr_path_for_key(char out[GBSC_PATH_MAX], uint64_t key) {
  return xr_path_for_key_hint(out, key, false);
}

bool xr_path_for_key_hint(char out[GBSC_PATH_MAX], uint64_t key, bool xfer_dir_absent) {
  if (!out) return false;
  char xpath[GBSC_PATH_MAX] = {0};
  if (gbsc_path(xpath, GBSC_PATH_MAX, PDNA_XFER_DIR, key) < 0) return false;

#ifdef PDNA_DELTA
  /* BACKLOG #209: mirrors xr_open()'s own bank_plant_xfer_open shim below, but for
   * the EXISTENCE question -- gb_has_sidecar() (source/pdna_gen12.c) calls
   * xr_path_for_key() DIRECTLY, never through xr_open(), so that shim (which only
   * xr_open() itself consults) never gets a chance to run for site 2 (the Red-grid
   * restore lift, gb_lift_restore -- retired by #280): the f_stat below always misses on this vehicle
   * (no FAT at all) and the restore is never even called. `out` is filled with
   * `xpath`, the exact bytes a real xfer-dir hit would have produced -- nothing on
   * this vehicle ever reads bytes off the path string itself; xr_open()'s own shim
   * serves the CONTENT when the caller opens it for real. */
  if (bank_plant_xfer_has(key)) { memcpy(out, xpath, GBSC_PATH_MAX); return true; }
#endif

  FILINFO fi;
  bool migrated = false;
  if (!xfer_dir_absent) {
    if (f_stat(xpath, &fi) == FR_OK) {
      memcpy(out, xpath, GBSC_PATH_MAX);
      return true;
    }
    migrated = marker_present();
  }
  /* xfer_dir_absent == true: xpath and the MIGRATED marker are both children of
   * PDNA_XFER_DIR, so a caller who already knows that directory does not exist
   * (xr_dir_exists()) has proven both f_stat calls above would miss without
   * asking the card again -- migrated stays false, the exact value the calls
   * would have produced. */
  if (!migrated) {
    char spath[GBSC_PATH_MAX] = {0};
    if (gbsc_path(spath, GBSC_PATH_MAX, PDNA_SIDECAR_DIR, key) >= 0 &&
        f_stat(spath, &fi) == FR_OK) {
      memcpy(out, spath, GBSC_PATH_MAX);
      return true;
    }
  } else {
    /* review R2 (LOW, accepted cost of decision (a)): a sidecar record arriving
     * AFTER the marker exists is unreachable by this reader and never gets picked
     * up by a later xr_migrate_once() either (that early-returns on the marker
     * too). One extra f_stat, only on this miss, so at least the reason is
     * visible in the log instead of the record silently never resolving. */
    char spath[GBSC_PATH_MAX] = {0};
    if (gbsc_path(spath, GBSC_PATH_MAX, PDNA_SIDECAR_DIR, key) >= 0 &&
        f_stat(spath, &fi) == FR_OK) {
      log_line("xfer: %s exists but migration already ran -- delete %s/MIGRATED to re-import",
               spath, PDNA_XFER_DIR);
    }
  }

  /* #392(a): read-only card + an orphan .tmp in xfer: the record exists as far as a reader can tell (xr_open reads
   * the .tmp in RAM). `out` stays the PRIMARY path -- nothing here ever points a writer at the .tmp. */
  if (!xfer_dir_absent && !app_can_edit() && xr_tmp_present(xpath, key)) { memcpy(out, xpath, GBSC_PATH_MAX); return true; }
  /* neither exists (or migration already ran) -- a brand-new file belongs under
   * xfer (decision 4/D-Q7). */
  memcpy(out, xpath, GBSC_PATH_MAX);
  return false;
}

bool xr_path_for_name(char out[GBSC_PATH_MAX], const char* name) {
  if (!out || !name) return false;
  char xpath[GBSC_PATH_MAX] = {0};
  if (join_path(xpath, GBSC_PATH_MAX, PDNA_XFER_DIR, name) < 0) return false;

  FILINFO fi;
  if (f_stat(xpath, &fi) == FR_OK) {
    memcpy(out, xpath, GBSC_PATH_MAX);
    return true;
  }

  bool migrated = marker_present();
  if (!migrated) {
    char spath[GBSC_PATH_MAX] = {0};
    if (join_path(spath, GBSC_PATH_MAX, PDNA_SIDECAR_DIR, name) >= 0 &&
        f_stat(spath, &fi) == FR_OK) {
      memcpy(out, spath, GBSC_PATH_MAX);
      return true;
    }
  } else {
    /* review R2 -- same shape as xr_path_for_key's own miss-only diagnostic. */
    char spath[GBSC_PATH_MAX] = {0};
    if (join_path(spath, GBSC_PATH_MAX, PDNA_SIDECAR_DIR, name) >= 0 &&
        f_stat(spath, &fi) == FR_OK) {
      log_line("xfer: %s exists but migration already ran -- delete %s/MIGRATED to re-import",
               spath, PDNA_XFER_DIR);
    }
  }

  memcpy(out, xpath, GBSC_PATH_MAX);
  return false;
}

/* BACKLOG #392(a): a READ-ONLY card (Everdrive) cannot run the boot heal (xh_heal_dir only counts there), so an
 * orphan <path>.tmp -- the verified bytes of an interrupted write whose primary is gone -- would stay invisible
 * until the next Omega boot. Read it as the primary IN RAM, never writing: same acceptance test as xh_heal_key
 * (whole file read, parses, key matches). Anything else reads as the absence it was before. */
static __attribute__((noinline)) SfStatus xr_read_orphan_tmp(const char* path, uint64_t key, uint8_t* buf,
                                                            uint32_t cap, uint32_t* sz) {
  char tmp[GBSC_PATH_MAX + 8];
  FILINFO fi;
  size_t n = strlen(path);
  if (n + 5 > sizeof tmp) return SF_ERR_OPEN;
  memcpy(tmp, path, n); memcpy(tmp + n, ".tmp", 5);
  if (f_stat(tmp, &fi) != FR_OK) return SF_ERR_OPEN;
  SfStatus st = sf_read_full(tmp, buf, cap, sz);
  if (st != SF_OK) return st;
  if (*sz != (uint32_t)fi.fsize || gbsc_count(buf, *sz) < 0 || !xr_orphan_hdr_ok(buf, *sz, key)) return SF_ERR_OPEN;
  return SF_OK;
}

/* ---- the one reader ----------------------------------------------------------- */

SfStatus xr_open(uint64_t key, uint8_t* buf, uint32_t cap, uint32_t* len, char* path_out) {
  char path[GBSC_PATH_MAX] = {0};
  bool exists = xr_path_for_key(path, key);
  if (path_out) memcpy(path_out, path, GBSC_PATH_MAX);
  /* BACKLOG #150 S150-9 review D4 (LOW), reordered by the #175c/#226 review's
   * structural finding: try the REAL file first (unconditionally -- `exists` is
   * folder resolution, not file presence; a real read costs one f_open miss when
   * absent). Only on SF_ERR_OPEN does PDNA_DELTA fall back to the planted shim.
   * Before this, the shim ran BEFORE any real read, so a genuine save-now that
   * landed the real promoted entry on an attached VSD card was invisible forever
   * -- xr_open kept re-serving the stale planted PENDING bytes and the restore's
   * "save-now succeeds -> the restore proceeds" happy path could never execute
   * at either site. Deferring to a real file the moment one exists needs no
   * vsd_attached() check of its own: with no card attached the real read still
   * just returns SF_ERR_OPEN, falling through to the shim exactly as before. */
  uint32_t sz = 0;
  SfStatus st = exists ? sf_read_full(path, buf, cap, &sz) : SF_ERR_OPEN;
  if (st == SF_ERR_OPEN && !app_can_edit()) st = xr_read_orphan_tmp(path, key, buf, cap, &sz);   /* #392a */
#ifdef PDNA_DELTA
  if (st == SF_ERR_OPEN && bank_plant_xfer_open(key, buf, cap, len)) return SF_OK;
#endif
  if (st == SF_OK && len) *len = sz;
  return st;
}

/* ---- the one-time migration (decision 5) --------------------------------------- */

/* "<16 uppercase hex>.pds" -- exactly GBSC_PATH_MAX's own construction (gbsc_path),
 * reused here as an acceptance filter (mirrors the suffix test at
 * source/pdna_main.c's gb_reconcile_walk). 20 chars + NUL, same bound as
 * GB_RECON_NAME_MAX. */
#define XR_MIGRATE_NAME_LEN 20

static bool is_hex_pds_name(const char* name, int len) {
  if (len != XR_MIGRATE_NAME_LEN) return false;
  for (int i = 0; i < 16; i++) {
    char c = name[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  const char* e = name + 16;
  return e[0] == '.' && (e[1] | 32) == 'p' && (e[2] | 32) == 'd' && (e[3] | 32) == 's';
}

/* uint32_t -> decimal ASCII, no libc formatter (golden rule: no unbounded loops,
 * fixed-size local buffers only). Returns the digit count written to `out`
 * (>= 10 bytes, NOT NUL-terminated -- the caller controls termination). */
static int u32_to_dec(uint32_t v, char* out) {
  char tmp[10];
  int n = 0;
  do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && n < 10);
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  return n;
}

/* FNV-1a-64 over a whole file's bytes -- used ONLY to decide whether the xfer twin
 * already matches the sidecar source, so the migration loop never needs to hold two
 * GBSC_FILE_MAX buffers at once (the caller-supplied `scratch` is the ONLY large
 * buffer this function touches -- no new statics, no second on-stack copy). A hash
 * collision could in principle hide a real difference, but the only consequence
 * (decision 4/D-Q7's own fallback logic) is that a stale xfer copy is skipped for
 * one more run; the sidecar source is never touched either way (G-M1), so this
 * never loses data -- it can only delay a re-copy that the next boot's migration
 * (or a fresh corruption) would still catch. */
static uint64_t file_hash(const uint8_t* buf, uint32_t len) {
  uint64_t h = 14695981039346656037ULL;
  for (uint32_t i = 0; i < len; i++) { h ^= (uint64_t)buf[i]; h *= 1099511628211ULL; }
  return h;
}

static bool write_marker(uint32_t copied) {
  char path[GBSC_PATH_MAX] = {0};
  if (join_path(path, GBSC_PATH_MAX, PDNA_XFER_DIR, "MIGRATED") < 0) return false;
  char body[16] = {0};
  int n = u32_to_dec(copied, body);
  bool ok = sf_write_verified(path, (const uint8_t*)body, (uint32_t)n) == SF_OK;
  /* BACKLOG #213: the marker's own existence flips xr_path_for_key's `migrated`
   * answer for every key -- and xr_migrate_once() may also have copied sidecar
   * files into xfer just above, moving keys that used to resolve to sidecar. Both
   * invalidate app_xv_cache_invalidate()'s caller-side latch/ring (pdna_main.c). */
  if (ok) app_xv_cache_invalidate();
  return ok;
}

int __attribute__((noinline)) xr_migrate_once(uint8_t* scratch, uint32_t cap) {
  if (!app_can_edit()) return -1;

  char marker[GBSC_PATH_MAX] = {0};
  if (join_path(marker, GBSC_PATH_MAX, PDNA_XFER_DIR, "MIGRATED") < 0) return -1;
  FILINFO fi;
  if (f_stat(marker, &fi) == FR_OK) return 0;   /* already migrated -- O(1) */

  FRESULT mkr = f_mkdir(PDNA_XFER_DIR);
  if (mkr != FR_OK && mkr != FR_EXIST) {
    log_line("xfer: migrate: mkdir %s failed (%d)", PDNA_XFER_DIR, (int)mkr);
    return -1;
  }

  DIR dir;
  if (f_opendir(&dir, PDNA_SIDECAR_DIR) != FR_OK) {
    /* no sidecar folder at all -- nothing to migrate, mark done so the next call
     * is O(1) too. */
    write_marker(0);
    return 0;
  }

  uint32_t copied = 0;
  FILINFO e;
  while (f_readdir(&dir, &e) == FR_OK && e.fname[0]) {
    if (e.fattrib & AM_DIR) continue;
    int L = 0; while (e.fname[L] && L < 64) L++;
    if (!is_hex_pds_name(e.fname, L)) continue;

    char src[GBSC_PATH_MAX] = {0}, dst[GBSC_PATH_MAX] = {0};
    if (join_path(src, GBSC_PATH_MAX, PDNA_SIDECAR_DIR, e.fname) < 0) continue;
    if (join_path(dst, GBSC_PATH_MAX, PDNA_XFER_DIR, e.fname) < 0) continue;

    /* skip when the xfer twin already exists and matches (decision 5): read the
     * DESTINATION first and hash it (sequential use of the one `scratch` buffer --
     * see file_hash()'s own comment for why a hash, not a second buffer). */
    bool skip = false;
    FILINFO dfi;
    if (f_stat(dst, &dfi) == FR_OK) {
      uint32_t tlen = 0;
      SfStatus ts = sf_read_full(dst, scratch, cap, &tlen);
      if (ts == SF_OK && tlen == (uint32_t)dfi.fsize) {
        uint64_t dhash = file_hash(scratch, tlen);
        uint32_t slen2 = 0;
        SfStatus rs2 = sf_read_full(src, scratch, cap, &slen2);
        if (rs2 == SF_OK && slen2 == tlen && file_hash(scratch, slen2) == dhash) skip = true;
      }
    }
    if (skip) continue;

    uint32_t slen = 0;
    SfStatus rs = sf_read_full(src, scratch, cap, &slen);
    if (rs != SF_OK) {
      log_line("xfer: migrate: read %s failed (%s)", src, sf_status_str(rs));
      f_closedir(&dir);
      return -1;                        /* no marker -- the next run retries        */
    }

    SfStatus ws = sf_write_verified(dst, scratch, slen);
    if (ws != SF_OK) {
      log_line("xfer: migrate: write %s failed (%s)", dst, sf_status_str(ws));
      f_closedir(&dir);
      return -1;                        /* no marker -- the next run retries        */
    }
    copied++;
  }
  f_closedir(&dir);

  write_marker(copied);
  return (int)copied;
}
