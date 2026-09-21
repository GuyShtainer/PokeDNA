/* gb_art_source.c — see gb_art_source.h. The GB half of pdna_origin_art's
 * PdnaGbArtSource vtable, and rom_gbsprite.c's first real caller. */
#include <string.h>
#include <tonc.h>

#include "ff.h"
#include "log.h"
#include "sys.h"              /* EWRAM_BSS -- #62 D1's in-memory PDNA_DELTA loc caches */
#include "pdna_app.h"
#include "artbuf.h"           /* mon_decomp, MON_DECOMP_BYTES */
#include "pdna_origin_art.h"  /* PdnaGbArtSource, PDNA_GEN1/2, pdna_origin_art_register */
#include "rom_gbsprite.h"
#include "rom_gbicon.h"       /* E5: the 16x16 Gen-2 party/PC menu icon rung          */
#include "gb_art_source.h"
#include "gb_art_io.h"        /* BACKLOG #148: GbArtIo/gb_art_io_init/gb_art_read moved  *
                               * here so source/pdna_gbscreen.c can reuse them            */
#include "fused_gb.h"         /* PDNA_DELTA half: read the fuse_gb.py corpus from cart space */
#include "gb_scan_guard.h"    /* cancel / timeout / read-error latch for the SD read shim  */
#ifndef PDNA_DELTA
#include "perf.h"             /* perf_ticks/perf_ms: the timeout clock (runs with IRQs off) */
#include "rmbl.h"             /* rmbl_pause/resume around every SD read (rmbl.h contract)  */
#endif

/* ---- per-gen state -----------------------------------------------------------------
 * All plain (non-EWRAM) statics -- a handful of bytes each, the same "goes to IWRAM's
 * .bss, negligible against the ~11.5 KB stack" convention pdna_origin_art.c's own
 * s_gb/s_gb_on/memo fields already use. Indexed by `gen` directly (1 or 2); index 0 is
 * wasted on purpose, same call as sprite_era.h's SeRoms -- clearer than gen-1 math at
 * every call site. Needed by both builds (gb_art_have()/gb_art_register() below read
 * and write them either way, even the delta stand-ins). */
static bool s_reg_have[3];      /* last gb_art_register() outcome for gen, sticky      */
static bool s_reg_checked[3];   /* gb_art_register() has run at least once for gen     */
static bool s_fb_have[3];       /* "beside the save" fallback result, THIS open save   */
static bool s_fb_checked[3];    /* fallback has been probed for the CURRENT save       */

/* ---- E3 review item 8: everything below that touches FatFs/SD is real ONLY outside
 * PDNA_DELTA (the emulator build has no SD card at all). Previously each function had
 * its OWN #ifdef PDNA_DELTA/#else, which left several SD-only helpers (gb_art_load_loc,
 * gb_art_save_loc, gb_art_read, gb_art_open_and_identify, gb_art_resolve_path) fully
 * compiled but UNREACHABLE under PDNA_DELTA -- four -Wunused-function warnings, and a
 * real function to keep in sync by hand every time one of the per-function ifdefs
 * changed. ONE block, so "no SD in the emulator build" is a single fact instead of
 * five scattered ones. */
#ifndef PDNA_DELTA

/* ---- the location cache file: /PokeDNA/gbart1.loc / gbart2.loc ---------------------
 * One RomGbSpriteLoc (~260 B) per gen slot, so rom_gbsprite_open_loc() can skip the
 * whole-ROM scan on every fetch after the first. Read fresh from the file at EVERY
 * fetch rather than held resident (two 260 B copies would eat half of the 524 B EWRAM
 * budget for a cache that is only a performance optimisation) -- see gb_art_source.h's
 * memory note. Self-validating: rom_gbsprite_open_loc() rejects a stale/foreign loc via
 * its own id_hash+size check and falls back to a full scan, so a corrupt or wrong-ROM
 * cache file degrades to "slow" rather than "wrong", and this is a plain (unverified)
 * f_write -- like config.cfg's own philosophy, there is no reason for a regenerable,
 * self-checked cache to be the one exception. */
static void gb_art_loc_path(uint8_t gen, char* out, int cap) {
  siprintf(out, "%.*s/gbart%u.loc", cap - 12, PDNA_DIR, (unsigned)gen);
}

static bool gb_art_load_loc(uint8_t gen, RomGbSpriteLoc* out) {
  char path[40];
  gb_art_loc_path(gen, path, (int)sizeof path);
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  FRESULT fr = f_read(&f, out, (UINT)sizeof *out, &br);
  f_close(&f);
  return fr == FR_OK && br == sizeof *out;
}

static void gb_art_save_loc(uint8_t gen, const RomGbSpriteLoc* loc) {
  if (!app_can_edit()) return;                    /* Everdrive/read-only: don't even try */
  char path[40];
  gb_art_loc_path(gen, path, (int)sizeof path);
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
  f_write(&f, loc, (UINT)sizeof *loc, &bw);
  f_close(&f);
  if (bw != sizeof *loc) log_line("gb art: loc cache write short for gen%u", (unsigned)gen);
}

/* ---- E5: the icon location cache: /PokeDNA/gbicon2.loc ----------------------------
 * A SEPARATE file from gbart2.loc above, deliberately -- not "the existing format with
 * a version bump" as a first cut of this comment assumed, because gbart2.loc's raw
 * single-struct layout is already shipped and tested for the PORTRAIT rung and this
 * slice has no reason to put that at risk. A second small file under the SAME PDNA_DIR
 * costs nothing (rule 9's "one folder per tool" is about the FOLDER, and gbart1.loc/
 * gbart2.loc already prove one tool keeping several small cache files there is normal)
 * and trivially gets "an old install rescans once" for free: the file simply does not
 * exist yet, so gb_icon_load_loc() reports no cache and rom_gbicon_open_loc() falls
 * back to its own full scan on first use, exactly like a first-ever gbart2.loc would.
 * Only gen 2 ever has one (Gen 1 has no menu icons at all -- rom_gbicon.h), so there is
 * no gbicon1.loc. Not resident (read fresh at every fetch), same rationale as
 * gbart*.loc's own comment: two 20 B copies would not be worth EWRAM for a cache that
 * is only a performance optimisation. */
static void gb_icon_loc_path(char* out, int cap) {
  siprintf(out, "%.*s/gbicon2.loc", cap - 13, PDNA_DIR);
}

static bool gb_icon_load_loc(RomGbIconLoc* out) {
  char path[40];
  gb_icon_loc_path(path, (int)sizeof path);
  FIL f; UINT br = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  FRESULT fr = f_read(&f, out, (UINT)sizeof *out, &br);
  f_close(&f);
  return fr == FR_OK && br == sizeof *out;
}

static void gb_icon_save_loc(const RomGbIconLoc* loc) {
  if (!app_can_edit()) return;
  char path[40];
  gb_icon_loc_path(path, (int)sizeof path);
  FIL f; UINT bw = 0;
  if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
  f_write(&f, loc, (UINT)sizeof *loc, &bw);
  f_close(&f);
  if (bw != sizeof *loc) log_line("gb art: icon loc cache write short");
}

/* The one seek+read shim every FatFs-backed rom_gb* call in this file goes through
 * -- same shape as pdna_gen12.c's file-local gb_read(), duplicated rather than
 * shared because that one is `static` to a different translation unit and this
 * module deliberately does not reach into pdna_gen12.c's arena-resident session
 * state (see gb_art_source.h: this is a parallel, independent path, not a refactor
 * of tested paste-path code).
 *
 * 2026-09-14 (the "stuck choosing the .gbc" cart bug): the ctx is no longer a bare
 * FIL* but a GbArtIo carrying a gb_scan_guard. Before EVERY read the guard is asked
 * (gb_scan_guard.h): once anything has stopped it -- the user's B, the per-locator
 * time limit, or one failed read -- this returns false immediately, forever, without
 * touching the handle. That is what turns "FatFs latched fp->err and every later
 * f_read fails instantly" from a scanner spinning through thousands of instant
 * failures into a clean unwind on the first one. A read failure records fr, the
 * latched FIL.err and the offset for the caller's message and the log.
 *
 * BACKLOG #148: the GbArtIo type itself, and gb_art_io_init()/gb_art_read() below, now
 * live in gb_art_io.h (no longer `static` here) so source/pdna_gbscreen.c's
 * gbscr_open_inner can drive the same guard over its own whole-tail scan instead of the
 * plain, unguarded gbscr_sd_read() shim it used to open with (now deleted). */

#define GB_ART_TICK_MASK    7u                                        /* poll/draw every 8 reads */
/* BACKLOG #185 F3: a stall clock, not a flat one -- see gb_scan_guard.h/gb_art_source.h
 * for why. perf_ticks() is 16,384 Hz. */
#define GB_ART_STALL_TICKS        (GB_ART_STALL_S        * 16384u)
#define GB_ART_HARD_CEILING_TICKS (GB_ART_HARD_CEILING_S * 16384u)

void gb_art_io_init(GbArtIo* io, FIL* f, uint32_t size, GbArtProgressFn fn, void* fn_ctx,
                    uint8_t locator, bool limited) {
  memset(io, 0, sizeof *io);
  io->f = f; io->fn = fn; io->fn_ctx = fn_ctx; io->locator = locator;
  io->t_start = perf_ticks();
  gb_scan_guard_init(&io->g, size, io->t_start, limited ? GB_ART_STALL_TICKS : 0u,
                     limited ? GB_ART_HARD_CEILING_TICKS : 0u, GB_ART_TICK_MASK);
}

/* Start the next locator on the same handle: a fresh stall/ceiling clock and
 * progress fraction, the read count carried forward. */
static void gb_art_io_next(GbArtIo* io, uint8_t locator) {
  io->reads_done += io->g.reads;
  io->locator = locator;
  uint32_t stall = io->g.stall_limit, ceiling = io->g.hard_limit;
  gb_scan_guard_init(&io->g, io->g.total, perf_ticks(), stall, ceiling, GB_ART_TICK_MASK);
}

bool gb_art_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  GbArtIo* io = (GbArtIo*)ctx;
  if (!io || !io->f || !buf) return false;
  int tick = 0;
  if (!gb_scan_guard_admit(&io->g, off, len, perf_ticks(), &tick)) return false;
  if (tick && io->fn &&
      !io->fn(io->fn_ctx, io->locator, io->g.hi, io->g.total,
              perf_ms(gb_scan_guard_elapsed(&io->g, perf_ticks())))) {
    gb_scan_guard_cancel(&io->g);
    return false;
  }
  UINT br = 0;
  FRESULT fr = FR_OK;
  /* rmbl.h's contract: no cue may toggle the cart bus mid-transfer. Same per-read
   * bracket pdna_main.c's iconrom_fatfs_read uses; a no-op when nothing is armed. */
  rmbl_pause();
  if ((FSIZE_t)off != io->f->fptr) fr = f_lseek(io->f, (FSIZE_t)off);
  if (fr == FR_OK) fr = f_read(io->f, buf, (UINT)len, &br);
  rmbl_resume();
  if (fr != FR_OK || br != len) {
    /* Recorded, not logged, HERE: this shim sits at the bottom of every locator's
     * call chain, and log_line's newlib formatting subtree (~1.4 KB as the stack
     * guard charges it) would be added to every GbReadFn site in the program. The
     * callers log it from their own, shallower frames (gb_art_register via
     * GbArtRegInfo; gb_art_log_stop below for the per-fetch paths). */
    io->fr = (uint8_t)fr; io->err = io->f->err; io->fail_off = off;
    gb_scan_guard_fail(&io->g);
    return false;
  }
  /* BACKLOG #185 F3: this read COMPLETED -- reset the stall clock. Admitting a
   * read is not the same as it succeeding (gb_scan_guard_admit() itself never
   * touches `last`); only a real, successful transfer counts as progress. */
  gb_scan_guard_progress(&io->g, perf_ticks());
  return true;
}

/* The per-fetch paths' one line of evidence when a read stopped them -- at the
 * fetch frame's depth, a sibling of the FatFs tail rather than beneath it. */
static void gb_art_log_stop(const GbArtIo* io, const char* what) {
  if (io->g.stop == GB_SCAN_OK) return;
  log_line("gb art: %s stopped: loc%u stop=%u fr=%u err=%u off=%lu after %lu reads", what,
           (unsigned)io->locator, (unsigned)io->g.stop, (unsigned)io->fr, (unsigned)io->err,
           (unsigned long)io->fail_off, (unsigned long)io->g.reads);
}

/* Map a stopped guard to the registration status, or `fallback` when the locator
 * failed on its own (tables not found) with the guard still green. */
static GbArtRegStatus gb_art_stop_status(const GbScanGuard* g, GbArtRegStatus fallback) {
  switch (g->stop) {
    case GB_SCAN_STOP_CANCEL:   return GB_ART_REG_CANCELLED;
    case GB_SCAN_STOP_TIMEOUT:  return GB_ART_REG_TIMEOUT;
    case GB_SCAN_STOP_READ_ERR: return GB_ART_REG_READ_ERR;
    default:                    return fallback;
  }
}

static void gb_art_fill_info(GbArtRegInfo* info, const GbArtIo* io) {
  if (!info) return;
  info->reads      = io->reads_done + io->g.reads;
  info->elapsed_ms = perf_ms(perf_ticks() - io->t_start);
  info->covered    = io->g.hi;
  info->fail_off   = io->fail_off;
  info->locator    = io->locator;
  info->fr         = io->fr;
  info->err        = io->err;
  info->stop       = io->g.stop;
}

/* ---- open + identify: shared by an explicit Settings registration and the lazy
 * "beside the save" probe. FIL (600 B) + RomGbSprite (~336 B) + the 2,048 B scan
 * window rom_gbsprite_open()/open_loc() both require unconditionally -- see
 * gb_art_source.h's memory note for why this cannot be smaller. Called rarely (once
 * per Settings action, once per boot, once per save-open's first fallback probe), so
 * its ~3 KB own frame is not the hot-path budget gb_art_fetch() has to answer for.
 *
 * Deliberately does NOT call gb_art_save_loc() itself (E3 review, second pass): with
 * ONE function serving both a caller that wants the loc cached and one that does
 * not, gcc could no longer prove the `false` caller (gb_art_have's fallback probe)
 * never reaches the loc-file f_open/FAT-walk subtree, and the STATIC call-graph tool
 * (path-insensitive -- it sees "this function calls gb_art_save_loc", not "only when
 * the argument is true") charged every caller for it, undoing the very fix this
 * split exists to prove. Splitting the WRITE out to the caller's own frame makes it
 * true at the call-graph level, not just at runtime, that gb_art_have() never reaches
 * it. `out_loc` may be NULL (the fallback probe's case). */
/* 2026-09-14: the scan window is mon_decomp (8 KB, EWRAM, word-aligned), not a 2 KB
 * stack array -- four times the window is 4x fewer reads, and the frame drops by
 * 2,048 B at the one call site that used to be this module's deepest. mon_decomp is
 * only ever a scratch here (artbuf_claim() first, so any memo of its contents knows),
 * and rom_gbsprite_open_loc()/rom_gbicon_open_loc() touch it only inside the open
 * call. Loc-FIRST: the cached /PokeDNA/gbart<gen>.loc is loaded into *out_loc and
 * offered to open_loc(), which validates it in a few dozen reads and only falls back
 * to the whole-ROM scan when it does not describe this exact file -- so a boot, or
 * re-registering the same ROM, no longer pays the 2 MB scan the old rom_gbsprite_open()
 * call here always did. */
static GbArtRegStatus __attribute__((noinline))
gb_art_warm_icons(GbArtIo* io, uint32_t sz, RomGbIconLoc* out_iloc, bool* out_have_iloc) {
  gb_art_io_next(io, GB_ART_LOC_ICONS);
  bool have = gb_icon_load_loc(out_iloc);
  RomGbIcon gi;
  int ok = rom_gbicon_open_loc(&gi, gb_art_read, io, sz, (uint8_t*)mon_decomp, MON_DECOMP_BYTES,
                               have ? out_iloc : 0);
  if (!ok) {
    if (io->g.stop != GB_SCAN_OK) return gb_art_stop_status(&io->g, GB_ART_REG_BAD_ROM);
    /* Icons are optional (Gen 1 has none at all): a Gen-2 ROM whose icon tables do
     * not locate still registers for portraits, exactly as before this pre-warm. */
    log_line("gb art: gen2 icon tables not located (portraits still on)");
    return GB_ART_REG_OK;
  }
  rom_gbicon_save_loc(&gi, out_iloc);
  *out_have_iloc = true;
  return GB_ART_REG_OK;
}

static GbArtRegStatus __attribute__((noinline))
gb_art_open_and_identify(uint8_t gen, const char* path, RomGbSpriteLoc* out_loc,
                         RomGbIconLoc* out_iloc, bool* out_have_iloc,
                         GbArtProgressFn fn, void* fn_ctx, GbArtRegInfo* info) {
  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) return GB_ART_REG_CANT_OPEN;

  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  GbArtIo io;
  gb_art_io_init(&io, &fil, sz, fn, fn_ctx, GB_ART_LOC_SPRITES, true);

  bool have_loc = gb_art_load_loc(gen, out_loc);
  artbuf_claim();
  RomGbSprite gs;
  /* BACKLOG #185 F1: `gen` is already known here (the caller is registering
   * THIS generation), so the scan runs only that gen's three jobs instead of
   * all six -- and a ROM whose header says the other generation is refused
   * before a single position is scanned. */
  int ok = rom_gbsprite_open_loc(&gs, gb_art_read, &io, sz, (uint8_t*)mon_decomp, MON_DECOMP_BYTES,
                                 have_loc ? out_loc : 0, gen);
  GbArtRegStatus st = GB_ART_REG_OK;
  if (!ok)                          st = gb_art_stop_status(&io.g, GB_ART_REG_BAD_ROM);
  else if ((uint8_t)gs.gen != gen)  st = GB_ART_REG_WRONG_GEN;
  *out_have_iloc = false;
  if (st == GB_ART_REG_OK) {
    rom_gbsprite_save_loc(&gs, out_loc);
    if (gen == PDNA_GEN2) st = gb_art_warm_icons(&io, sz, out_iloc, out_have_iloc);
  }
  gb_art_fill_info(info, &io);
  f_close(&fil);
  return st;
}

GbArtRegStatus gb_art_register(uint8_t gen, const char* path, GbArtProgressFn progress,
                               void* progress_ctx, GbArtRegInfo* info) {
  if (info) memset(info, 0, sizeof *info);
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return GB_ART_REG_BAD_ROM;
  if (!path || !path[0]) {
    s_reg_have[gen] = false;
    s_reg_checked[gen] = true;
    pdna_origin_art_invalidate();  /* E3 review: unconditional -- clearing a registration
                                    * is itself a reason to drop a stale memo, same as
                                    * setting one */
    return GB_ART_REG_EMPTY;
  }
  RomGbSpriteLoc loc;
  RomGbIconLoc iloc;
  bool have_iloc = false;
  GbArtRegStatus st = gb_art_open_and_identify(gen, path, &loc, &iloc, &have_iloc,
                                               progress, progress_ctx, info);
  s_reg_have[gen] = (st == GB_ART_REG_OK);
  s_reg_checked[gen] = true;
  /* The ONLY writes, and only on success: a cancel, a timeout or a read error leaves
   * the card exactly as it was. Both sources are stack structs (RAM), never ROM. */
  if (s_reg_have[gen]) {
    gb_art_save_loc(gen, &loc);
    if (have_iloc) gb_icon_save_loc(&iloc);
  }
  /* E3 review: unconditional, not just on success -- a FAILED re-registration still
   * means "whatever the memo remembers for this gen may no longer be true" (the old
   * ROM might already be gone/replaced even though the new one didn't validate). */
  pdna_origin_art_invalidate();
  if (st != GB_ART_REG_OK) {
    if (info)
      log_line("gb art: gen%u registration failed (%d) for %s: loc%u stop=%u fr=%u err=%u off=%lu "
               "reads=%lu %lums", (unsigned)gen, (int)st, path, (unsigned)info->locator,
               (unsigned)info->stop, (unsigned)info->fr, (unsigned)info->err,
               (unsigned long)info->fail_off, (unsigned long)info->reads,
               (unsigned long)info->elapsed_ms);
    else
      log_line("gb art: gen%u registration failed (%d) for %s", (unsigned)gen, (int)st, path);
  } else if (info) {
    log_line("gb art: gen%u registered %s: icons %s, %lu reads, %lu ms", (unsigned)gen, path,
             have_iloc ? "cached" : (gen == PDNA_GEN2 ? "NOT located" : "n/a"),
             (unsigned long)info->reads, (unsigned long)info->elapsed_ms);
  }
  return st;
}

bool gb_rom_path_beside(const char* save_path, uint8_t gen, char* out, int cap) {
  (void)gen;
  if (!save_path || !out || cap < 5) return false;
  int len = 0; while (save_path[len]) len++;
  int slash = -1, dot = -1;
  for (int i = 0; i < len; i++) { if (save_path[i] == '/') slash = i; if (save_path[i] == '.') dot = i; }
  int baselen = (dot > slash) ? dot : len;         /* strip the LAST extension only */
  /* E3 review item 7: REFUSE rather than clip -- a truncated base would probe a
   * DIFFERENT (likely nonexistent, or worse, some unrelated file's) path than the
   * one actually beside the save, and a false "found it" there is worse than
   * finding nothing. GB_ROM_PATH_MAX(128) is comfortably larger than any real save
   * path this app itself ever produces, so this is a defensive refusal, not an
   * expected outcome. */
  if (baselen > cap - 5) return false;             /* room for the longest extension + NUL */

  static const char* const kExt[2] = { ".gb", ".gbc" };
  for (int e = 0; e < 2; e++) {
    memcpy(out, save_path, (size_t)baselen);
    int p = baselen;
    for (int i = 0; kExt[e][i] && p < cap - 1; i++) out[p++] = kExt[e][i];
    out[p] = 0;
    FIL f;
    if (f_open(&f, out, FA_READ) == FR_OK) { f_close(&f); return true; }
  }
  return false;
}

bool gb_art_have(uint8_t gen) {
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return false;
  /* BACKLOG #47: the detach switch means NO rom art at all, not just Gen-3's -- checked
   * BEFORE the registration check right below so an explicit Settings registration
   * cannot override it (same posture as g3cross_pic_cb: the switch always wins over
   * "is a ROM configured"). The registration itself is untouched by this -- flipping
   * the switch back on makes this function true again with no re-registration. */
  if (app_rom_art_off()) return false;
  if (s_reg_checked[gen] && s_reg_have[gen]) return true;   /* an explicit registration always wins */
  if (!app_current_save_is_gb()) return false; /* the fallback only applies to a raw GB session       */
  if (!s_fb_checked[gen]) {
    s_fb_checked[gen] = true;
    char path[GB_ROM_PATH_MAX];
    /* EXISTENCE ONLY -- deliberately does NOT call gb_art_open_and_identify() (that
     * needs its own FIL+RomGbSprite+2,048 B scratch, ~2,992 B) to fully validate the
     * sibling file here. `have()`'s own contract is "cheap; used to skip work", and
     * gb_art_fetch() is going to open+identify this exact path for real in a moment
     * anyway (rom_gbsprite_open_loc()/open() inline in its own frame) -- a bad or
     * wrong-generation sibling still degrades correctly there (returns NULL, exactly
     * the router's normal "no art" outcome), so a second, heavier validation here
     * bought nothing but stack: E3 review's call-graph tool found this nested INSIDE
     * gb_art_fetch's own frame (called from pdna_origin_art_have(), itself called
     * from pdna_origin_art_portrait() before fetch_pic()) reaching a deeply nested
     * UI chain (Bank -> box -> party strip -> mon menu -> Daycare -> inspect ->
     * summary -> portrait) hard enough to push main()'s worst path over the 11.5 KB
     * stack even after gb_art_fetch's OWN frame was already fixed. */
    s_fb_have[gen] = gb_rom_path_beside(app_current_save_path(), gen, path, (int)sizeof path);
  }
  return s_fb_have[gen];
}

/* ---- the actual fetch: gb_art_source.h's whole memory-budget discussion ------------
 * E3 review BLOCKING 1: decodes straight into mon_decomp itself, IN PLACE, instead of
 * a separate 3,928 B GbSprite -- rom_gbsprite_pic_buf() writes the indexed pixels at
 * mon_decomp+ROM_GBSPRITE_MAX_PIXELS (the codec's own work[] scratch immediately
 * after that), and rom_gbsprite_to_rgb15_inplace() expands them back into
 * mon_decomp+0 -- see rom_gbsprite.h for why that specific offset is safe. This is
 * now ONE noinline frame (FIL + RomGbSpriteLoc + RomGbSprite + the 2,048 B scan
 * window + the path buffer), not two: there is no more scratch GbSprite to give a
 * split any reason to exist. */
static bool gb_art_resolve_path(uint8_t gen, char* out, int cap) {
  const char* reg = app_gb_rom_path(gen);
  if (reg && reg[0]) {
    int i = 0; for (; reg[i] && i < cap - 1; i++) out[i] = reg[i]; out[i] = 0;
    return true;
  }
  if (app_current_save_is_gb()) return gb_rom_path_beside(app_current_save_path(), gen, out, cap);
  return false;
}

static const uint16_t* __attribute__((noinline))
gb_art_fetch(uint8_t gen, uint16_t dex, uint8_t form, uint8_t back, uint8_t shiny,
            uint8_t* out_w, uint8_t* out_h) {
  /* have() is re-checked in gb_art_pic_cb() (24 B own frame) now, not here (E3
   * re-verification "free partial") -- pdna_origin_art_portrait() already checked it
   * before ever reaching this call, so this was a redundant re-check paid for INSIDE
   * gb_art_fetch's own ~3.7 KB frame; the graph tool cannot tell "redundant" from
   * "load-bearing" and charged the whole gb_art_have() subtree against this frame's
   * total either way. Measured: 5,664 -> 5,552 B. */
  char path[GB_ROM_PATH_MAX];
  if (!gb_art_resolve_path(gen, path, (int)sizeof path)) return 0;

  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) return 0;
  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  RomGbSpriteLoc loc;
  bool have_loc = gb_art_load_loc(gen, &loc);

  /* Silent, unlimited guard (no UI can be shown from a box repaint) -- it still latches
   * the first failed read so a bad card unwinds instead of spinning. The scan window
   * is mon_decomp (claimed right here, BEFORE its first write -- see the E3 BLOCKING 2
   * note below, which this claim now also covers), so the loc-miss rescan is 4x fewer
   * reads than the 2 KB stack window it replaced, and this frame is 2 KB lighter. */
  GbArtIo io;
  gb_art_io_init(&io, &fil, sz, 0, 0, GB_ART_LOC_SPRITES, false);
  artbuf_claim();
  RomGbSprite gs;
  /* BACKLOG #185 F1: same gen-restricted scan as the registration path above --
   * this is the per-fetch cache-miss fallback (box-full-of-strangers, a stale
   * .loc), so restricting it to `gen`'s three jobs matters on exactly the box
   * repaint the module header's own HARDWARE-ONLY PERFORMANCE NOTE flags. */
  int ok = rom_gbsprite_open_loc(&gs, gb_art_read, &io, sz, (uint8_t*)mon_decomp, MON_DECOMP_BYTES,
                                 have_loc ? &loc : 0, gen);
  /* Re-save the loc whenever it does NOT already match this exact ROM -- not just
   * when the file was missing. E3 review item 4: a STALE loc (the file at `path`
   * was SWAPPED for a different ROM since it was cached) still makes have_loc true
   * (a loc file was read) even though rom_gbsprite_open_loc() silently fell back to
   * a full scan internally because its id_hash/size no longer matched -- the
   * original `!have_loc` check never caught that case, so a swapped ROM paid the
   * full-ROM scan cost on EVERY fetch, forever, with no way to self-heal. */
  if (ok && (!have_loc || loc.id_hash != gs.id_hash || loc.size != sz)) {
    RomGbSpriteLoc fresh;
    rom_gbsprite_save_loc(&gs, &fresh);
    gb_art_save_loc(gen, &fresh);
  }
  if (!ok || (uint8_t)gs.gen != gen) { gb_art_log_stop(&io, "fetch"); f_close(&fil); return 0; }

  RomGbSide side = back ? ROM_GBSPRITE_BACK : ROM_GBSPRITE_FRONT;
  RomGbPic info;
  const uint16_t* px = 0;
  uint8_t* mdbuf = (uint8_t*)mon_decomp;
  /* E3 review BLOCKING 2: claim BEFORE the very first write (rom_gbsprite_pic_buf
   * writes the indexed px/work region even on a later failure) -- see artbuf.h. The
   * claim now sits above open_loc() (the scan window is mon_decomp too), which is
   * earlier still; it is also what lets pdna_origin_art.c's fetch_pic() memo()
   * capture a POST-fetch epoch that already accounts for this call's own writes. */
  /* px at [ROM_GBSPRITE_MAX_PIXELS, +3,136), work right after at
   * [ROM_GBSPRITE_RGB15_BYTES, +784) -- ROM_GBSPRITE_RGB15_BYTES (6,272) IS
   * ROM_GBSPRITE_MAX_PIXELS*2, so this is "px, then work" back to back, both past
   * the RGB15 region rom_gbsprite_to_rgb15_inplace() writes below. The
   * _Static_assert pins the arithmetic so a future constant change fails the build
   * instead of silently corrupting mon_decomp's neighbour in EWRAM. */
  _Static_assert(ROM_GBSPRITE_RGB15_BYTES + GB_SPRITE_WORK <= MON_DECOMP_BYTES,
                 "GB art in-place layout no longer fits mon_decomp");
  if (rom_gbsprite_pic_buf(&gs, side, dex, form, mdbuf + ROM_GBSPRITE_MAX_PIXELS,
                           mdbuf + ROM_GBSPRITE_RGB15_BYTES, &info)) {
    uint16_t pal[4];
    if (rom_gbsprite_pal(&gs, dex, shiny ? 1 : 0, pal) &&
        rom_gbsprite_to_rgb15_inplace(mdbuf, ROM_GBSPRITE_MAX_PIXELS, info.w, info.h, pal)) {
      *out_w = info.w; *out_h = info.h;
      px = mon_decomp;
    }
  }
  f_close(&fil);
  return px;
}

/* ---- E5: the 16x16 Gen-2 menu-icon fetch -------------------------------------------
 * Deliberately its OWN noinline frame, not folded into gb_art_fetch() above: there is
 * no in-place-aliasing trick to reason about here (rom_gbicon_tiles() writes a plain
 * 64 B tile buffer on THIS frame's own stack, nowhere near mon_decomp, and
 * rom_gbicon_to_rgb15() then expands it straight into mon_decomp with no overlap at
 * all) and RomGbIcon (~24 B) + RomGbIconLoc (~20 B) are both smaller than their sprite
 * counterparts, so sharing gb_art_fetch's frame would only inflate ITS measured cost
 * for a portrait-only caller that never takes this path. MEASURED (arm-none-eabi-gcc
 * -O2 -fstack-usage): see PDNA_GB_ICON_NEED's own comment (gb_art_source.h) for the
 * number and how it compares to the portrait rung's PDNA_GB_FETCH_NEED. */
static const uint16_t* __attribute__((noinline))
gb_art_fetch_icon(uint8_t gen, uint16_t dex, uint8_t* out_w, uint8_t* out_h) {
  if (gen != PDNA_GEN2) return 0;         /* Gen 1 has no menu icons -- rom_gbicon.h */

  char path[GB_ROM_PATH_MAX];
  if (!gb_art_resolve_path(gen, path, (int)sizeof path)) return 0;

  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) return 0;
  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  RomGbIconLoc loc;
  bool have_loc = gb_icon_load_loc(&loc);

  /* Same silent guard + mon_decomp window as gb_art_fetch() above (claim first: the
   * window is written before the decode below ever is). */
  GbArtIo io;
  gb_art_io_init(&io, &fil, sz, 0, 0, GB_ART_LOC_ICONS, false);
  artbuf_claim();
  RomGbIcon gi;
  int ok = rom_gbicon_open_loc(&gi, gb_art_read, &io, sz, (uint8_t*)mon_decomp, MON_DECOMP_BYTES,
                               have_loc ? &loc : 0);
  /* Same "re-save whenever it does not already match" rule as gb_art_fetch()'s own
   * loc handling above -- a swapped ROM behind the same path/size must not pay a full
   * rescan on every fetch forever. */
  if (ok && (!have_loc || loc.id_hash != gi.id_hash || loc.size != sz)) {
    RomGbIconLoc fresh;
    rom_gbicon_save_loc(&gi, &fresh);
    gb_icon_save_loc(&fresh);
  }
  if (!ok) { gb_art_log_stop(&io, "icon fetch"); f_close(&fil); return 0; }

  /* D6 (E5 fix): dex 0 is the EGG sentinel (pdna_origin_art.c's pdna_origin_box_art
   * comment) -- never a real national dex (1..251), so this can never accidentally
   * fire for a real species. Ask for the fixed ICON_EGG kind instead of a per-
   * species lookup: an egg has no species to look up by, and the retail party menu
   * draws the SAME egg picture no matter what is inside. */
  int kind = (dex == 0) ? rom_gbicon_kind_egg(&gi) : rom_gbicon_kind(&gi, dex);
  const uint16_t* px = 0;
  if (kind) {
    uint8_t tile[ROM_GBICON_FRAME_BYTES];      /* 64 B, this frame's OWN stack --
                                                * never inside mon_decomp           */
    uint16_t pal[4];
    /* claimed above, before open_loc() -- the same "before the first write" rule
     * (artbuf.h): a fetch that fails partway must still have already said "this
     * content is not what it was". */
    if (rom_gbicon_tiles(&gi, kind, 0, tile) && rom_gbicon_pal(&gi, kind, pal) &&
        rom_gbicon_to_rgb15(tile, pal, mon_decomp)) {
      *out_w = ROM_GBICON_W; *out_h = ROM_GBICON_H;
      px = mon_decomp;
    }
  }
  f_close(&fil);
  return px;
}

#else /* PDNA_DELTA: no SD card at all -- but BACKLOG #62's delta-gb build fuses a
       * whole Game Boy corpus (ROMs + saves) into cartridge space (tools/fuse_gb.py,
       * source/fused_gb.h). This half reads THAT instead of a FIL: fused_gb_rom()
       * hands back a {base,size} slice of cart address space, and
       * fused_gb_slice_read() is a GbReadFn over it (same signature the SD half's
       * gb_art_read() implements over a FIL*), so rom_gbsprite_open_loc()/
       * rom_gbicon_open_loc() and every decode step after them run completely
       * unmodified.
       *
       * #62 review D1: there is no SD to write a loc-cache FILE to, but there is
       * nothing stopping an in-memory cache -- the fused corpus is immutable for the
       * whole run (it is baked into the ROM image itself), so a loc discovered on the
       * first fetch of a generation is valid for every later fetch of that same
       * generation this session, with no staleness question the SD half's id_hash/size
       * re-check exists for (that re-check still runs here too; it is cheap and it is
       * what makes an entry NOT present this session -- e.g. gen unfused -- fail closed
       * instead of serving a stale hit). Indexed by gen (1 or 2); index 0 unused, same
       * "gen as array index" convention as s_reg_have/s_fb_have above. EWRAM_BSS per
       * #62 D1's memory rule (the delta build's tight budget is IWRAM/stack headroom,
       * not EWRAM: 1,748 B free here easily covers 2 * ~292 B sprite locs + one ~20 B
       * icon loc). Measured before/after in the commit message. */
static EWRAM_BSS RomGbSpriteLoc s_dsprite_loc[3];
static EWRAM_BSS bool           s_dsprite_loc_ok[3];
static EWRAM_BSS RomGbIconLoc   s_dicon_loc;          /* gen 2 only -- see rom_gbicon.h */
static EWRAM_BSS bool           s_dicon_loc_ok;

GbArtRegStatus gb_art_register(uint8_t gen, const char* path, GbArtProgressFn progress,
                               void* progress_ctx, GbArtRegInfo* info) {
  (void)path; (void)progress; (void)progress_ctx;
  if (info) memset(info, 0, sizeof *info);
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return GB_ART_REG_BAD_ROM;
  /* No file browser / SD under PDNA_DELTA to register a path FROM -- the only
   * source of a generation's ROM here is whatever fuse_gb.py fused in. Report
   * against that so app_gb_rom_registered() (and Settings' "fused" row) have
   * something true to say even though this entry point itself is unreachable
   * (nothing calls it -- there is no Settings > Game ROM file action here). */
  const uint8_t* base; uint32_t size;
  bool have = fused_gb_rom(gen, &base, &size);
  s_reg_have[gen] = have;
  s_reg_checked[gen] = true;
  pdna_origin_art_invalidate();
  return have ? GB_ART_REG_OK : GB_ART_REG_CANT_OPEN;
}

bool gb_art_have(uint8_t gen) {
  if (gen != PDNA_GEN1 && gen != PDNA_GEN2) return false;
  if (app_rom_art_off()) return false;           /* BACKLOG #47: the detach switch still wins */
  const uint8_t* base; uint32_t size;
  return fused_gb_rom(gen, &base, &size);
}

bool gb_rom_path_beside(const char* save_path, uint8_t gen, char* out, int cap) {
  (void)save_path; (void)gen; (void)out; (void)cap;
  return false;                    /* no SD, no "beside the save" concept under PDNA_DELTA */
}

/* BACKLOG #68b: seeds an EWRAM loc cache slot from the fused corpus's own
 * precomputed locator record (tools/fuse_gb.py + tools/gbloc_driver.c ran the
 * SAME rom_gb*_open_loc()/save_loc() this file calls, once, on the PC, at fuse
 * time), instead of paying the whole-ROM scan again on this session's first
 * fetch of the generation. Purely advisory: `dst` is copied UNVALIDATED, and the
 * caller's own rom_gb*_open_loc() still re-checks id_hash/size and every field's
 * own structural signature before trusting it, exactly as it already does for a
 * same-session cache hit -- a stale, hand-edited, or wrong-ROM fused record just
 * falls back to the ordinary full scan (rom_gb*_open_loc()'s existing fallback
 * path), never a wrong picture. The `claimed_rom_size`/`rec_len` checks here are a
 * cheap pre-filter only (avoids handing a wrong-sized blob to memcpy); they are
 * NOT the security boundary -- that is entirely open_loc()'s job. Returns true iff
 * a record was copied into `dst`. */
static bool gb_art_loc_seed(uint8_t kind, uint8_t gen, uint32_t rom_size,
                            void* dst, uint32_t dst_size) {
  const uint8_t* rec; uint32_t rec_len, claimed_rom_size;
  /* id_hash intentionally not requested (NULL): it is redundant with the real
   * validation open_loc() performs against the ROM it actually opened -- rom_size
   * is enough of a pre-filter here to avoid handing a wrong-sized blob to memcpy. */
  if (!fused_gb_loc(kind, gen, &rec, &rec_len, NULL, &claimed_rom_size)) return false;
  if (claimed_rom_size != rom_size || rec_len != dst_size) return false;
  memcpy(dst, rec, dst_size);
  return true;
}

static const uint16_t* gb_art_fetch(uint8_t gen, uint16_t dex, uint8_t form, uint8_t back,
                                    uint8_t shiny, uint8_t* out_w, uint8_t* out_h) {
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(gen, &base, &size)) return 0;
  FusedGbSlice slice = { base, size };

  /* #62 D1 (extended by #68b): the in-memory loc cache -- see s_dsprite_loc's own
   * comment above. A same-session hit (s_dsprite_loc_ok[gen]) is tried first; on a
   * cold cache, gb_art_loc_seed() tries the FUSED record next (BACKLOG #68b -- this
   * is what removes the cold-start whole-ROM scan under PDNA_DELTA); either way
   * rom_gbsprite_open_loc() re-validates before trusting it and falls back to a full
   * scan on any mismatch. */
  RomGbSprite gs;
  uint8_t scratch[ROM_GBSPRITE_SCRATCH_MIN];
  bool have_loc = s_dsprite_loc_ok[gen];
  if (!have_loc)
    have_loc = gb_art_loc_seed(FUSED_GB_LOC_SPRITE, gen, size, &s_dsprite_loc[gen],
                               (uint32_t)sizeof s_dsprite_loc[gen]);
  /* BACKLOG #185 F1: deliberately GB_ROM_NONE here, not `gen` -- this is the
   * PDNA_DELTA fused-corpus path (no SD, tools/dgb_shots.py's own cold-scan
   * measurements), kept running the original six-job scan unchanged rather than
   * gaining a second behaviour to verify on a build with no hardware to test it
   * against. */
  int ok = rom_gbsprite_open_loc(&gs, fused_gb_slice_read, &slice, size, scratch,
                                 (uint32_t)sizeof scratch, have_loc ? &s_dsprite_loc[gen] : 0,
                                 GB_ROM_NONE);
  if (ok) {
    /* Always (re)snapshot what open_loc() actually validated on success -- cheap (a
     * 260 B struct copy) and simpler than tracking "did this particular open come
     * from the session cache, the fused seed, or a fresh full scan": all three leave
     * gs holding a fully validated loc, and this makes s_dsprite_loc_ok[gen] true
     * the FIRST time any of them succeeds, including the fused-seed path above
     * (which must not be reported as "cached" until open_loc() has actually
     * re-validated it). */
    rom_gbsprite_save_loc(&gs, &s_dsprite_loc[gen]);
    s_dsprite_loc_ok[gen] = true;
  }
  if (!ok || (uint8_t)gs.gen != gen) return 0;

  RomGbSide side = back ? ROM_GBSPRITE_BACK : ROM_GBSPRITE_FRONT;
  RomGbPic info;
  const uint16_t* px = 0;
  uint8_t* mdbuf = (uint8_t*)mon_decomp;
  /* Same in-place layout + claim-before-write rule as the SD half's gb_art_fetch() --
   * see its own comment above (the #ifndef PDNA_DELTA half of this file). */
  artbuf_claim();
  _Static_assert(ROM_GBSPRITE_RGB15_BYTES + GB_SPRITE_WORK <= MON_DECOMP_BYTES,
                 "GB art in-place layout no longer fits mon_decomp");
  if (rom_gbsprite_pic_buf(&gs, side, dex, form, mdbuf + ROM_GBSPRITE_MAX_PIXELS,
                           mdbuf + ROM_GBSPRITE_RGB15_BYTES, &info)) {
    uint16_t pal[4];
    if (rom_gbsprite_pal(&gs, dex, shiny ? 1 : 0, pal) &&
        rom_gbsprite_to_rgb15_inplace(mdbuf, ROM_GBSPRITE_MAX_PIXELS, info.w, info.h, pal)) {
      *out_w = info.w; *out_h = info.h;
      px = mon_decomp;
    }
  }
  return px;
}

static const uint16_t* gb_art_fetch_icon(uint8_t gen, uint16_t dex, uint8_t* out_w, uint8_t* out_h) {
  if (gen != PDNA_GEN2) return 0;                /* Gen 1 has no menu icons -- rom_gbicon.h */
  const uint8_t* base; uint32_t size;
  if (!fused_gb_rom(gen, &base, &size)) return 0;
  FusedGbSlice slice = { base, size };

  /* #62 D1 (extended by #68b): same in-memory loc cache pattern as gb_art_fetch()
   * above, gen 2 only -- session cache first, then the fused seed, then a full scan;
   * see gb_art_fetch()'s own comment for why the resave is unconditional on success. */
  RomGbIcon gi;
  uint8_t scratch[ROM_GBICON_SCRATCH_MIN];
  bool have_loc = s_dicon_loc_ok;
  if (!have_loc)
    have_loc = gb_art_loc_seed(FUSED_GB_LOC_ICON, gen, size, &s_dicon_loc,
                               (uint32_t)sizeof s_dicon_loc);
  int ok = rom_gbicon_open_loc(&gi, fused_gb_slice_read, &slice, size, scratch,
                               (uint32_t)sizeof scratch, have_loc ? &s_dicon_loc : 0);
  if (ok) {
    rom_gbicon_save_loc(&gi, &s_dicon_loc);
    s_dicon_loc_ok = true;
  }
  if (!ok) return 0;

  int kind = (dex == 0) ? rom_gbicon_kind_egg(&gi) : rom_gbicon_kind(&gi, dex);
  const uint16_t* px = 0;
  if (kind) {
    uint8_t tile[ROM_GBICON_FRAME_BYTES];
    uint16_t pal[4];
    artbuf_claim();
    if (rom_gbicon_tiles(&gi, kind, 0, tile) && rom_gbicon_pal(&gi, kind, pal) &&
        rom_gbicon_to_rgb15(tile, pal, mon_decomp)) {
      *out_w = ROM_GBICON_W; *out_h = ROM_GBICON_H;
      px = mon_decomp;
    }
  }
  return px;
}

#endif /* PDNA_DELTA */

/* ---- the PdnaGbArtSource vtable -- compiled in BOTH builds -------------------------- */
static const uint16_t* gb_art_pic_cb(void* ctx, uint8_t gen, uint16_t dex, uint8_t form,
                                     uint8_t back, uint8_t shiny, uint8_t* out_w, uint8_t* out_h) {
  (void)ctx;
  /* BACKLOG #47: cheap first-line refuse, same posture as g3cross_pic_cb's own
   * first line -- redundant with gb_art_have()'s own check below (belt-and-braces,
   * not load-bearing on its own), but every OTHER rung that honours the switch
   * checks it as its very first statement, and a reader diffing this callback
   * against that one should not have to trust a call three lines down instead. */
  if (app_rom_art_off()) return 0;
  /* have() re-checked HERE (24 B own frame), not inside gb_art_fetch (E3
   * re-verification "free partial") -- see gb_art_fetch's own comment. */
  if (!gb_art_have(gen)) return 0;
  return gb_art_fetch(gen, dex, form, back, shiny, out_w, out_h);
}
static int gb_art_have_cb(void* ctx, uint8_t gen) { (void)ctx; return gb_art_have(gen) ? 1 : 0; }

/* E5: same have()-first-then-fetch shape as gb_art_pic_cb above, restricted to
 * gen 2 (Gen 1 has no menu icons; gb_art_fetch_icon also refuses this, but
 * checking here too skips even the resolve-path/f_open attempt for gen 1). */
static const uint16_t* gb_art_icon_cb(void* ctx, uint8_t gen, uint16_t dex,
                                      uint8_t* out_w, uint8_t* out_h) {
  (void)ctx;
  if (app_rom_art_off()) return 0;   /* BACKLOG #47: see gb_art_pic_cb's own comment */
  if (gen != PDNA_GEN2 || !gb_art_have(gen)) return 0;
  return gb_art_fetch_icon(gen, dex, out_w, out_h);
}

/* E3 review re-verification: the real stack-headroom check. __iheap_start is the
 * SAME linker symbol gba_cart.ld places right after every static IWRAM .text/.data/
 * .bss section -- the low-water mark the user stack (which starts at __sp_usr, just
 * under IWRAM's top, and grows DOWN) can never cross without colliding with code/
 * data. `sp - __iheap_start` is therefore exactly the number of bytes of stack still
 * free at the moment this runs, matching this project's OWN post-link EWRAM-overflow
 * guard's use of the equivalent EWRAM symbol (__eheap_start vs the EWRAM top,
 * Makefile) -- same idiom, other end of memory. Valid in any GBA-target build
 * (delta included; harmless there since gb_art_have() is already false with no SD),
 * so this is not wrapped in #ifndef PDNA_DELTA.
 * NOT NEWLIB'S HEAP (D9, BACKLOG #84b fifth pass, correcting the line below): newlib's
 * heap grows UP from __eheap_start, a SEPARATE symbol in EWRAM -- __iheap_start (this
 * function's own symbol) is IWRAM's __iwram_overlay_end, the tail of the .iwram.c
 * fast-path code (the flashcart SD I/O routines). What an overrun actually corrupts is
 * that fast-path code, not a heap. ASSUMES NOTHING HAS MALLOC'D EWRAM'S HEAP EITHER,
 * for the separate reason below (the subtraction here is still exact regardless -- it
 * measures real, always-present IWRAM code, not heap growth -- this note is about a
 * DIFFERENT assumption the surrounding budget arithmetic relies on). CORRECTED
 * 2026-09-10 (BACKLOG #84a S4): this
 * comment used to claim "0 malloc / ff_memalloc call sites" -- false at the time, because
 * four sites (commit_bytes/log_health_str/perf_fs_facts's snprintf, log_line's vsnprintf)
 * pulled newlib's FLOAT-capable _svfprintf_r (816 B) -> _dtoa_r -> _Balloc -> _malloc_r,
 * plus the mprec assert leg (_svfprintf_r/_dtoa_r/_malloc_r were all present in the linked
 * ELF). Switching those four to sniprintf/vsniprintf removed _svfprintf_r and _dtoa_r
 * entirely (0 hits in `nm`) -- the float/mprec path and its ~11 KB worst-case chain are
 * gone. _malloc_r is still LINKED but is NOT reachable (review #84a, 2026-09-10, reverse
 * call graph over the whole ELF): _svfiprintf_r's two _malloc_r sites are guarded by __SMBF
 * (asprintf-grown buffers) and by the %ls/%lc wide-char conversion, and __ssputs_r's by
 * __SOPT|__SMBF -- none of which an sniprintf/vsniprintf stack FILE ever sets; __sfp is
 * called by nobody. The heap is therefore genuinely empty and this subtraction exact. The
 * ONE thing that would break it: a %ls/%lc (or a %f/%g/%e, which relinks the float chain)
 * in any log_line/sniprintf format string -- grep for those before trusting this. A
 * future malloc, ff_memalloc, or a %f-capable printf reintroducing the float chain would
 * still silently eat the ~550 B of slack between the 6,144-B need and the measured
 * 5,592-B fetch subtree. Re-measure with the call-graph tool if either ever appears.
 *
 * D4 (E4 review): `need` is now the CALLER's own measured requirement, not a fixed
 * 6,144 B baked in here -- pdna_origin_art.c's portrait router passes
 * PDNA_GB_FETCH_NEED for its GB-rung call sites and the smaller PDNA_G3X_FETCH_NEED
 * for the cross-game Gen-3 rung, so each rung gates on its own subtree. This function
 * itself only ever reads the CPU's SP; it has no opinion on what any rung needs. */
extern char __iheap_start[];
static int gb_art_stack_room(int need) {
  register char* sp __asm__("sp");
  return (sp - __iheap_start) > need;
}

void gb_art_boot_register(GbArtProgressFn progress, void* progress_ctx) {
  static const PdnaGbArtSource src = { gb_art_pic_cb, gb_art_have_cb, gb_art_icon_cb, 0 };
  pdna_origin_art_register(&src);
  pdna_origin_art_set_stack_room_hook(gb_art_stack_room);
#ifndef PDNA_DELTA
  for (uint8_t gen = PDNA_GEN1; gen <= PDNA_GEN2; gen++) {
    const char* p = app_gb_rom_path(gen);
    if (p && p[0]) {
      GbArtRegInfo info;
      GbArtRegStatus st = gb_art_register(gen, p, progress, progress_ctx, &info);
      if (st != GB_ART_REG_OK)
        log_line("gb art: gen%u boot re-registration failed (%d)", (unsigned)gen, (int)st);
    }
  }
#else
  (void)progress; (void)progress_ctx;
#endif
}

/* Called once, from view_save() (pdna_main.c), the moment a NEW save is opened -- the
 * "beside the save" fallback is scoped to THAT save, so a stale hit/miss from the
 * PREVIOUS save must not leak into this one. Registered ROMs (s_reg_*) are untouched:
 * they don't depend on which save is open. Compiled in both builds: harmless
 * bookkeeping under PDNA_DELTA (gb_art_have() never reads s_fb_* there), and
 * view_save() calls it unconditionally either way. */
void gb_art_session_reset(void) {
  s_fb_have[PDNA_GEN1] = s_fb_have[PDNA_GEN2] = false;
  s_fb_checked[PDNA_GEN1] = s_fb_checked[PDNA_GEN2] = false;
}
