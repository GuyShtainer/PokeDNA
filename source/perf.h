#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include <stdbool.h>

/*
 * perf.h — the log's telemetry instrument.
 *
 * WHY THIS EXISTS. The SD path cannot be emulated: every claim about how much card
 * traffic a screen costs has, until now, been a calculation on a host RAM disk, and
 * every claim about how long that traffic TAKES has been an estimate with a guessed
 * card latency in it. Guy runs the real thing on a GBA SP / DS Lite + EZ-Flash Omega
 * DE and pastes /PokeDNA/log.txt back. So the log is the only measuring instrument
 * this project has, and this module is what puts numbers in it:
 *
 *   1. counters at the ONE choke point every SD read and write passes through
 *      (lib/fatfs/diskio.c disk_read, lib/fatfs/diskio_write.c disk_write);
 *   2. counters on the artless icon ladder's three rungs (MRU, icons.bin, the
 *      user's ROM), at the CALL SITES in art_fallbacks.c and box_oam.c;
 *   3. a session-long millisecond clock, so a sector count can be converted into
 *      time on the user's actual card instead of on paper;
 *   4. SPANS, which turn 1-3 into one greppable log line per interesting event.
 *
 * COMPILING IT OUT. Everything below is behind PDNA_PERF, default 1. A release build
 * sets -DPDNA_PERF=0 and pays literally nothing: the macros become `((void)0)`, the
 * functions become empty `static inline`s the optimiser deletes, perf.c compiles to an
 * empty translation unit, and no counter storage is allocated. Call sites need no
 * #ifdef of their own -- that is the point of the inline-stub half.
 *
 * MEMORY. All state is EWRAM (.sbss), NOT IWRAM: this build has a documented,
 * reproducible boot crash at roughly 1,232 B of NEW IWRAM .bss (art_icons_cache.c:9,
 * art_fallbacks.c:53, pdna_romcheck.c:47), and telemetry is not worth going anywhere
 * near that cliff. The cost is 329 B of EWRAM -- measured, not estimated: sum the
 * `[bB]` symbols of perf.o in `arm-none-eabi-nm -S PokeDNA-artless.elf` (perf_sd 28,
 * perf_icons 32, s_span 72, s_rep 180, and 17 for the clock/flush/high-water scalars).
 * The Makefile's post-link `EWRAM ok: N bytes free` guard reads 416 with all of it in,
 * and `make artless PDNA_PERF=0` reads 736 -- which is the same 320 back, plus the
 * always-compiled clock's 9. That guard must stay POSITIVE.
 *
 * PURE-C RULE (docs/kb/c-coding-guideline.md Sec 2). This HEADER is stdint-only, so
 * lib/fatfs/diskio.c (which pulls in sys.h's u8/u16/u32 MACROS) and any host-compiled
 * file can include it without a type clash. perf.c itself is NOT pure-C -- it owns
 * hardware timers and writes log lines -- and is deliberately not in any host test's
 * object list, which is also why NEITHER source/rom_mon.c NOR source/art_icons_cache.c
 * is instrumented directly: both ARE in host-test object lists (tests/host_rommon_test.c,
 * tests/host_iconscache_test.c and three others), and instrumenting them would force
 * perf.c -- and through it log.c, ff.h and tonc -- into those links. Their traffic is
 * counted at their call sites instead, which sees exactly the same calls.
 */

#ifndef PDNA_PERF
#define PDNA_PERF 1
#endif

/* NOT behind PDNA_PERF, deliberately. Two SCREENS depend on this clock for a readout
 * the user sees -- the art-extraction screen's elapsed/KB-s figures and the
 * full-image verifier's timings -- so compiling it out would not remove telemetry, it
 * would silently make those screens read 0.0s. It is three tiny functions and 9 bytes
 * of state; the counters and spans below are what -DPDNA_PERF=0 removes.
 */
/* ---- the session clock ----------------------------------------------------------
 * TIMER0 at F/1024 (16,384 Hz) with TIMER1 cascaded = a 32-bit tick counter good for
 * ~72 hours. TIMER2 belongs to the rumble PWM (source/rumble.c) and is NEVER touched
 * here. perf owns TIMER0/TIMER1 for the whole session; the two screens that used to
 * start and stop their own pair (the art-extraction screen in pdna_main.c and the
 * full-image verifier in pdna_romfull.c) now read this one instead, so there is
 * exactly one owner. perf_ticks() is additionally self-healing: if it ever finds the
 * timer disabled or its count moved backwards -- a future double-owner -- it carries
 * the elapsed ticks forward and re-arms, so the clock stays MONOTONIC and a span can
 * never report a negative or wrapped duration. */
void     perf_clock_start(void);
uint32_t perf_ticks(void);            /* monotonic ticks since perf_clock_start()   */
uint32_t perf_ms(uint32_t ticks);     /* ticks -> milliseconds                      */
uint32_t perf_us(uint32_t ticks);     /* ticks -> microseconds (exact: *15625 >> 8) */


/* Set by the Makefile through the generated $(BUILD)/pdna_git.h, which perf.c includes
 * BEFORE this header (it used to be a -D on CFLAGS, which make could not track -- see the
 * Makefile's PDNA_GIT block). Empty or absent is not an error: a Docker build with no git,
 * or a tarball, still builds and logs "git ?". */
#ifndef PDNA_GIT_HASH
#define PDNA_GIT_HASH ""
#endif

/* D2 (BACKLOG #84b fifth pass): the SAME generated $(BUILD)/pdna_git.h stamps which
 * per-variant build directory (build/build-artless/build-delta/...) this ELF was
 * linked from -- tools/stack_budget.py reads it back out of the built ELF and
 * refuses to certify a stack budget computed against a MISMATCHED --builddir (the
 * .su files from a DIFFERENT variant, silently giving a wrong-but-plausible-looking
 * number -- BACKLOG #84b review, D2). Empty/absent is not an error, same posture as
 * PDNA_GIT_HASH above -- a hand-rolled compile outside the Makefile still builds,
 * just without the cross-check. */
#ifndef PDNA_BUILD_DIR
#define PDNA_BUILD_DIR ""
#endif

#if PDNA_PERF

/* ---- the SD choke-point counters ------------------------------------------------
 *
 * Bumped ONLY by the two macros below, and only from disk_read/disk_write. Plain
 * arithmetic on plain EWRAM words: no function call, no log line, no ROM access, so a
 * bump is safe even though the driver a few instructions later unmaps the cartridge
 * ROM for the length of the transfer (hard rule 1). See the macros' own note.
 *
 * `multi` and `max` are the batching instruments: today almost every icon read is a
 * single 512 B sector, so `rd == rd_sect` and `rd_max == 1`. When the batching work
 * lands, `rd_sect / rd` is the average transfer size and `rd_multi / rd` is the share
 * of reads that carried more than one sector. Those two ratios ARE the proof that the
 * fix landed, on the user's card, in his own log. */
typedef struct {
  uint32_t rd;        /* disk_read calls attempted                                 */
  uint32_t rd_sect;   /* sectors those calls asked for                             */
  uint32_t rd_multi;  /* how many of them asked for MORE THAN ONE sector           */
  uint32_t wr;        /* disk_write calls attempted                                */
  uint32_t wr_sect;   /* sectors those calls asked for                             */
  uint32_t wr_multi;  /* how many of them asked for MORE THAN ONE sector           */
  uint16_t rd_max;    /* largest single read, in sectors                           */
  uint16_t wr_max;    /* largest single write, in sectors                          */
} PerfSd;
extern PerfSd perf_sd;

/* ---- the artless icon ladder's counters -----------------------------------------
 * One field per rung, so a span line says WHICH rung served and how expensively.
 * `null_ans` is the one nobody would think to count and the one that explains a hole
 * on screen: the ladder was asked for an icon and no rung could produce one.
 *
 * mru_hit/mru_miss are the RETENTION ratio, and BOTH icon ladders feed them: the
 * dex/party/day-care ladder's 3-slot (row, frame) MRU (art_fallbacks.c) and the PC
 * box's per-slot pose cache (box_oam.c's s_cache_ok[], filled once per box load and
 * then swapped VRAM<->EWRAM on every bob tick with zero I/O). The two are never on
 * screen at the same time, they mean exactly the same thing -- "this frame came out
 * of RAM instead of off the card" -- and a span line is always scoped to one screen,
 * so one pair of counters reads correctly for either. */
typedef struct {
  uint32_t mru_hit;   /* a RAM-cached frame served with no SD I/O at all           */
  uint32_t mru_miss;  /* ...had to go to a rung below                              */
  uint32_t bin;       /* 512 B frames read out of /PokeDNA/art/icons.bin           */
  uint32_t bin_pal;   /* icons.bin palette FETCHES (box_oam's 3-per-box-enter loop;
                       * only the first per session touches the card -- the metadata
                       * tail is loaded lazily and then held in RAM)                */
  uint32_t rom_loc;   /* rom_mon locate calls (the 2-field table lookup, verified)  */
  uint32_t rom_frm;   /* rom_mon 512 B frame reads (2 per verified fill)           */
  uint32_t rom_pal;   /* rom_mon palette reads ONLY -- never the cache rung's      */
  uint32_t null_ans;  /* the ladder answered "no icon"                             */
} PerfIcons;
extern PerfIcons perf_icons;

/* The ONLY way these counters are touched. Arithmetic only -- deliberately not a
 * function call: disk_read runs on the edge of an OS-mode transfer (IRQs off, ROM
 * about to be unmapped by _EZFO_readSectors), and a call into a ROM-resident helper
 * there is exactly the mistake hard rule 1 exists to prevent. The operands are EWRAM
 * .sbss words, which stay addressable throughout. `n` is evaluated once. */
#define PERF_SD_READ(n)                                                       \
  do {                                                                        \
    unsigned pf_n_ = (unsigned)(n);                                           \
    perf_sd.rd++;                                                             \
    perf_sd.rd_sect += pf_n_;                                                 \
    if (pf_n_ > 1u) perf_sd.rd_multi++;                                       \
    if (pf_n_ > perf_sd.rd_max) perf_sd.rd_max = (uint16_t)pf_n_;             \
  } while (0)
#define PERF_SD_WRITE(n)                                                      \
  do {                                                                        \
    unsigned pf_n_ = (unsigned)(n);                                           \
    perf_sd.wr++;                                                             \
    perf_sd.wr_sect += pf_n_;                                                 \
    if (pf_n_ > 1u) perf_sd.wr_multi++;                                       \
    if (pf_n_ > perf_sd.wr_max) perf_sd.wr_max = (uint16_t)pf_n_;             \
  } while (0)

/* PERF_ICON(mru_hit) etc. -- one field name, no strings, no branches. */
#define PERF_ICON(field) (perf_icons.field++)

/* ---- spans: one log line per interesting event ----------------------------------
 *
 * perf_span_begin("dex") ... perf_span_end() emits ONE line with everything that
 * happened in between:
 *
 *   perf dex: 412 ms, sd 105r/105s/2m 0w/0s, icons 3/21 mru, bin 21/3, rom 0/0/0, null 0
 *
 * `sd 105r/105s/2m` is calls / sectors / calls-that-carried-more-than-one-sector, and
 * `bin 21/3` is icons.bin frames / icons.bin palettes. Stable shape, under ~110 chars,
 * greppable and diffable across runs.
 *
 * SPANS DO NOT NEST, and misuse cannot be silently wrong: a perf_span_begin() while
 * another span is open CLOSES the open one first and tags its line ` !unclosed`, then
 * starts the new one. You always get the data and the marker names the bug. An
 * unmatched perf_span_end() is a no-op.
 *
 * NO SPAN MAY CONTAIN A CALL THAT BLOCKS ON USER INPUT. A span is a measurement of
 * what the MACHINE did; the moment a confirm dialog or a file picker sits inside one,
 * its milliseconds are the user's thinking time and its sector count is however much
 * of the card he chose to browse -- and the same binary then reports a different
 * number for the same work on every run. view_save()'s "boot" span is the case that
 * proved it: the artless build's one-shot "ADD YOUR GAME ROM?" offer sits right in the
 * middle of it. Where such a call cannot be moved out, bracket it with the pause/resume
 * pair below, which subtracts the blocked interval's ms AND its counter deltas from the
 * open span. Nested pauses are not supported (a second pause is a no-op); a span that
 * ends while paused resumes itself first, so the numbers are always well-formed. */
void perf_span_begin(const char* name);
void perf_span_end(void);
void perf_span_pause(void);
void perf_span_resume(void);
/* True while a span is open. Lets a screen that can be entered EITHER standing alone
 * OR inside a bigger span (the PC box, which is both its own screen and the last step
 * of a save open) avoid starting a second one. */
bool perf_span_active(void);

/* ---- rollups: for events that fire over and over --------------------------------
 *
 * A bob flip every 8 frames or a page repaint per D-pad press must NOT get a line
 * each: log.c has a 192 KiB per-RUN byte budget (LOG_RUN_MAX) and a log that floods
 * is a log that says nothing. Wrap each occurrence in perf_rep_begin/perf_rep_end and
 * call perf_rep_flush() when the screen is left; one line reports the lot:
 *
 *   perf bob.dex x37: tot 640 ms, worst 31 ms, sd 3885r/3885s, icons 12/37 mru, null 0
 *
 * The icon fields are not decoration: a bob rollup is the ONLY telemetry the bobs
 * produce, and without them "the retention cache is holding every row" and "the ladder
 * is returning nothing at all, so the grid is blank" emit byte-identical lines -- both
 * read `sd 0r/0s`. `null` is what separates a working screen from an empty one, and
 * `mru` is the only place mandate #3 (keep in RAM what is still needed) can be proven,
 * because the hit rate lives in the REPEATING event, not in the one-shot enter span.
 *
 * THREE slots, because a screen can have three distinct repeating events live at once:
 * a page step, an idle bob, and -- for the summary -- a per-record open that the user
 * walks through by holding the D-pad. Screens are modal, so the same slots are reused
 * by each screen in turn: the name travels with the slot, and a perf_rep_begin with a
 * DIFFERENT name auto-flushes what the slot was holding first. That is what makes a
 * forgotten perf_rep_flush() a late line rather than two screens' numbers silently
 * added together. A flush with nothing accumulated prints nothing. */
#define PERF_REP_PAGE 0    /* the per-page / per-repaint event                      */
#define PERF_REP_BOB  1    /* the per-animation-tick event                          */
#define PERF_REP_MON  2    /* the per-record event (a summary card opened)          */
void perf_rep_begin(int slot, const char* name);
void perf_rep_end(int slot);
void perf_rep_flush(int slot);

/* ---- one-time boot facts --------------------------------------------------------
 * Cheap, and each answers a question we currently have to guess at. */
void     perf_boot_line(void);              /* build id + git hash + free EWRAM      */

/* The BATCHING high-water marks, absolute rather than per-span: how many sectors the
 * biggest single read and the biggest single write actually carried. rd_max is the only
 * thing that can show whether FatFs clipped a prefetch at the cluster boundary the `fs:`
 * line reports (ff.c f_read: `if (csect + cc > fs->csize) cc = fs->csize - csect;`), and
 * it is uninferable from the rd/rd_sect pair -- 61 reads carrying 124 sectors averages
 * 2.03, which reads like "batching landed" but is equally one 64-sector prefetch plus 60
 * untouched singles. Self-rate-limiting: with force=false this prints ONLY when a
 * high-water mark actually moved, and since both marks are monotonic and bounded by the
 * cluster size, that converges to silence within a screen or two of the first big
 * transfer. force=true prints unconditionally (perf_sd_sample uses it, so that every log
 * carries at least one totals line as a baseline). */
void     perf_sd_totals(bool force);
void     perf_fs_facts(const void* fatfs);  /* const FATFS*; ff.h stays out of here  */
uint32_t perf_ewram_free(void);             /* bytes below 0x02040000, from the ELF  */

/* Time a real sequential read on the user's own card, at TWO sizes, and log KB/s for
 * each. This single line converts every sector count in every later span into
 * milliseconds -- and the gap between the two sizes measures the fixed-vs-marginal
 * transaction cost directly, which until now was arithmetic on a datasheet.
 * `paths` is a NULL-terminated candidate list; the first one that opens and is big
 * enough wins. `scratch` must be at least PERF_SD_SAMPLE_BIG + 4 bytes -- the +4 is
 * headroom to round the destination up to a word, which the measurement depends on
 * (see perf.c). It is written to, never read. */
#define PERF_SD_SAMPLE_BIG 16384u
void perf_sd_sample(const char* const* paths, void* scratch, uint32_t scratch_bytes);

#else /* !PDNA_PERF -- every call site compiles away to nothing */

#define PERF_SD_READ(n)  ((void)0)
#define PERF_SD_WRITE(n) ((void)0)
#define PERF_ICON(field) ((void)0)
#define PERF_REP_PAGE 0
#define PERF_REP_BOB  1
#define PERF_REP_MON  2
#define PERF_SD_SAMPLE_BIG 0u

/* NOTE: perf_clock_start / perf_ticks / perf_ms / perf_us are NOT stubbed here --
 * they are declared above, outside this switch, and always compiled (perf.c). */
static inline void     perf_span_begin(const char* n) { (void)n; }
static inline void     perf_span_end(void) {}
static inline void     perf_span_pause(void) {}
static inline void     perf_span_resume(void) {}
static inline bool     perf_span_active(void) { return false; }
static inline void     perf_rep_begin(int s, const char* n) { (void)s; (void)n; }
static inline void     perf_rep_end(int s) { (void)s; }
static inline void     perf_rep_flush(int s) { (void)s; }
static inline void     perf_boot_line(void) {}
static inline void     perf_sd_totals(bool f) { (void)f; }
static inline void     perf_fs_facts(const void* f) { (void)f; }
static inline uint32_t perf_ewram_free(void) { return 0; }
static inline void     perf_sd_sample(const char* const* p, void* s, uint32_t b) {
  (void)p; (void)s; (void)b;
}

#endif /* PDNA_PERF */

#endif /* PERF_H */
