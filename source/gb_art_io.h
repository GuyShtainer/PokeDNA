#ifndef GB_ART_IO_H
#define GB_ART_IO_H

/* gb_art_io — the guarded ROM read shim BACKLOG #148 lifts out of gb_art_source.c so a
 * second caller (source/pdna_gbscreen.c's gbscr_open_inner) can drive the SAME cancel/
 * timeout/read-error-latch machinery over its own scan, instead of the plain, unguarded
 * gbscr_sd_read() shim it used to open with. gb_art_source.c is still the only place
 * that INITIALISES/READS through a GbArtIo for its own two locators (GB_ART_LOC_SPRITES/
 * ICONS); this header exists so gb_art_read()/gb_art_io_init() and the GbArtIo layout
 * itself are declared ONCE, not copied.
 *
 * #include "gb_art_source.h" for GbArtProgressFn and the pre-existing GB_ART_LOC_SPRITES/
 * GB_ART_LOC_ICONS/GB_ART_SCAN_LIMIT_S — this header does not redefine any of those, it
 * only ADDS GB_ART_LOC_UI (the gbscreen tail-window scan) alongside them. gb_art_source.h
 * itself is unchanged (it has no FatFs dependency and stays that way — see BACKLOG #148's
 * brief: source/pdna_gbscreen.c includes THIS header only inside its own
 * `#ifndef PDNA_GBSCREEN_HOST_TEST` FatFs block, never unconditionally, so the pure host
 * test half of that file — which includes gb_art_source.h unconditionally today — never
 * has to see ff.h). */
#include "gb_art_source.h"

#include "ff.h"
#include "gb_scan_guard.h"

/* BACKLOG #148: gbscr_open_inner's own whole-tail scan. Named distinctly from
 * GB_ART_LOC_SPRITES/ICONS (gb_art_source.c's registration locators) so a progress
 * screen or a log line can always tell which module's scan is running. */
#define GB_ART_LOC_UI 3

/* 2026-09-14 (the "stuck choosing the .gbc" cart bug): the ctx is no longer a bare
 * FIL* but a GbArtIo carrying a gb_scan_guard. Before EVERY read the guard is asked
 * (gb_scan_guard.h): once anything has stopped it -- the user's B, the per-locator
 * time limit, or one failed read -- this returns false immediately, forever, without
 * touching the handle. That is what turns "FatFs latched fp->err and every later
 * f_read fails instantly" from a scanner spinning through thousands of instant
 * failures into a clean unwind on the first one. A read failure records fr, the
 * latched FIL.err and the offset for the caller's message and the log. */
typedef struct GbArtIo {
  GbArtProgressFn fn;         /* NULL = silent (boot, per-fetch). FIRST on purpose: this
                               * is the one indirect call tools/stack_budget.py must
                               * resolve (tools/stack_edges.txt `GbArtIo.fn @0`), and at
                               * offset 0 its check never depends on sizing the nested
                               * guard below.                                            */
  void*           fn_ctx;
  FIL*            f;
  GbScanGuard     g;
  uint32_t        t_start;    /* perf_ticks() at open, for GbArtRegInfo.elapsed_ms       */
  uint32_t        reads_done; /* reads admitted by EARLIER guards (locator 1 -> 2)       */
  uint32_t        fail_off;
  uint8_t         locator;    /* GB_ART_LOC_* -- what the progress screen names          */
  uint8_t         fr;         /* FRESULT of the failing call, 0 = none                   */
  uint8_t         err;        /* FIL.err after it                                        */
} GbArtIo;

/* Ctx for gb_reg_progress (source/pdna_main.c) — pdna_main.c's Settings/boot
 * registration and pdna_gbscreen.c's gbscr_open_inner both build one of these and pass
 * gb_reg_progress as the GbArtProgressFn, so the same progress screen (title, phase
 * name, KB counter, bar, elapsed clock, B-cancel) covers both scans instead of a second
 * copy. `restoring` is always 0 from gbscr_open_inner (that screen only ever says
 * "CHECKING", never "RESTORING" -- pdna_main.c's own restore path is a distinct,
 * explicit user action pdna_gbscreen.c never performs). */
typedef struct GbRegUi { uint8_t gen; uint8_t restoring; } GbRegUi;

/* Defined in source/pdna_main.c (BACKLOG #148: no longer `static` so
 * gbscr_open_inner can reuse it). See GbArtProgressFn's own doc comment
 * (source/gb_art_source.h) for the calling contract. */
bool gb_reg_progress(void* vctx, uint8_t locator, uint32_t done, uint32_t total,
                     uint32_t elapsed_ms);

/* Start (or restart, for a second locator on the same handle) the guard: `size` is the
 * ROM's total byte length (the progress bar's `total`), `fn`/`fn_ctx` the caller's
 * progress callback (NULL = silent), `locator` a GB_ART_LOC_* id, `limited` whether the
 * GB_ART_SCAN_LIMIT_S wall-clock timeout applies (every real caller passes true; only
 * the host-side reuse of this shim would ever want false). */
void gb_art_io_init(GbArtIo* io, FIL* f, uint32_t size, GbArtProgressFn fn, void* fn_ctx,
                    uint8_t locator, bool limited);

/* The GbReadFn every rom_gb*_open(_loc)() call in this codebase now binds to for its
 * SD-backed leg: seeks (only if the FatFs cursor is not already there), reads, and
 * consults/updates `io`'s guard on every call -- cancel, timeout and a failed read all
 * latch and refuse every read after the first, without touching the card again. */
bool gb_art_read(void* ctx, uint32_t off, void* buf, uint32_t len);

#endif /* GB_ART_IO_H */
