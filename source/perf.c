/* perf.c — see perf.h for what this is and why the log is the only instrument. */

/* Include order matters, exactly as pdna_main.c documents: <tonc.h> FIRST so its
 * u8/u16/u32 typedefs are in place before anything drags in sys.h's same-named
 * MACROS. perf.h itself is stdint-only and safe in either order. */
#include <tonc.h>

/* THE BUILD STAMP. Written by the Makefile into $(BUILD)/pdna_git.h on every build and
 * rewritten only when its CONTENT changes, which is what makes perf.o's dependency on it
 * real -- a -DPDNA_GIT_HASH on CFLAGS was not tracked by make and went stale on any
 * incremental build (the Makefile's PDNA_GIT block has the whole story). It must come
 * BEFORE perf.h, which supplies the "" fallback that makes the log say "git ?".
 * __has_include so a hand-rolled compile of this file outside the Makefile still builds. */
#if defined(__has_include)
#  if __has_include("pdna_git.h")
#    include "pdna_git.h"
#  endif
#endif

#include "perf.h"

#include <stddef.h>  /* offsetof -- span_flip walks PerfSd's uint32 prefix only */
#include <stdio.h>   /* sniprintf -- never sprintf/snprintf (c-coding-guideline Sec 0.13;
                        BACKLOG #84a S4: snprintf pulls newlib's float-capable
                        _svfprintf_r -> _dtoa_r -> _malloc_r) */
#include <string.h>

#include "ff.h"
#include "log.h"
#include "rmbl.h"      /* freeze the cart-bus motor across the throughput sample */
#include "pdna_app.h"  /* app_log_flush -- the Omega-gated, rmbl-paused SD flush  */

/* EWRAM (.sbss), spelled out here rather than pulled from lib/sys.h, so this file
 * never has to include sys.h's u8/u16/u32 macros alongside tonc's typedefs. Same
 * section attribute sys.h's EWRAM_BSS expands to. Every byte of perf's state lives
 * here and NONE in IWRAM -- see perf.h's MEMORY note for the boot-crash cliff that
 * rule exists for. */
#define PERF_EWRAM __attribute__((section(".sbss")))

/* D2 (BACKLOG #84b fifth pass): the build-dir stamp tools/stack_budget.py's --builddir
 * cross-check reads back out of the linked ELF (by symbol, via nm -- see the guard's
 * own main()). Plain .rodata (ROM), NOT EWRAM -- this is a fixed string baked in at
 * link time, never written at runtime, so it costs zero bytes of the 256 KiB EWRAM
 * budget the golden rules are strict about. See perf.h's PDNA_BUILD_DIR comment. */
const char pdna_build_dir[] = PDNA_BUILD_DIR;

/* ---- the session clock (always compiled -- see perf.h) --------------------------- */

/* s_acc carries ticks from any previous EPOCH of the hardware counter. There is only
 * one owner today, so s_acc normally stays 0 for the whole session; it exists so that
 * a future second owner (or a screen that resets TIMER0 behind our back) degrades into
 * "a few lost ticks" instead of "every span from here on reports garbage". */
static uint32_t PERF_EWRAM s_acc;
static uint32_t PERF_EWRAM s_prev;
static uint8_t  PERF_EWRAM s_clk_on;

static void clk_arm(void) {
  REG_TM0CNT = 0; REG_TM1CNT = 0;
  REG_TM0D   = 0; REG_TM1D   = 0;
  REG_TM1CNT = TM_ENABLE | TM_CASCADE;
  REG_TM0CNT = TM_ENABLE | TM_FREQ_1024;      /* TM2 is rumble's; untouched */
}

/* Read hi, lo, hi: if the high half moved between the two reads, the low half belongs
 * to the wrong epoch, so take a fresh pair. One retry is enough -- both halves cannot
 * roll in the microseconds between two loads. (Same read pattern pdna_romfull.c has
 * used since it was written; this is now the single copy of it.) */
static uint32_t clk_raw(void) {
  u16 hi = REG_TM1D, lo = REG_TM0D, hi2 = REG_TM1D;
  if (hi2 != hi) { hi = hi2; lo = REG_TM0D; }
  return ((uint32_t)hi << 16) | lo;
}

void perf_clock_start(void) {
  s_acc = 0; s_prev = 0; s_clk_on = 1;
  clk_arm();
}

uint32_t perf_ticks(void) {
  if (!s_clk_on) return 0;
  /* Self-heal. Either symptom means somebody else drove TIMER0/TIMER1: the enable bit
   * is gone (a tmr_stop somewhere), or the count went BACKWARDS (a tmr_start reset it
   * to 0). Both are handled the same way -- bank what we had, keep counting -- so the
   * result stays monotonic and a span can never report a negative duration. */
  if (!(REG_TM0CNT & TM_ENABLE)) {
    s_acc += s_prev; s_prev = 0;
    clk_arm();
  }
  uint32_t raw = clk_raw();
  if (raw < s_prev) s_acc += s_prev;
  s_prev = raw;
  return s_acc + raw;
}

uint32_t perf_ms(uint32_t ticks) {
  return (uint32_t)(((uint64_t)ticks * 1000ull) >> 14);          /* /16384, no divide */
}

uint32_t perf_us(uint32_t ticks) {
  return (uint32_t)(((uint64_t)ticks * 15625ull) >> 8);          /* 1e6/16384 exactly */
}

#if PDNA_PERF

PerfSd    PERF_EWRAM perf_sd;
PerfIcons PERF_EWRAM perf_icons;

/* ---- flush discipline ------------------------------------------------------------
 *
 * A hang must not eat the evidence, but log.c's own comments price a flush honestly:
 * it is an f_open (a directory walk) + an append + an f_close, and on every 16th
 * commit a read-back f_stat on top. Flushing on EVERY span would put a directory scan
 * behind every screen the user opens, which is the same "SD I/O on a UI event" trap
 * this whole effort exists to remove.
 *
 * The balance chosen: span AND rollup lines flush at most once every PERF_FLUSH_GAP_MS.
 * So the most a freeze can cost is the lines of the last 1.5 seconds, which is at most a
 * handful still sitting in log.c's 8 KiB ring. Only the BOOT FACTS and the throughput
 * sample flush unconditionally, because a run that dies before the first screen must
 * still leave its build id and card geometry behind.
 *
 * Rollups used to be in that unconditional set, on the reasoning that they "fire once
 * per screen EXIT and are therefore rare by construction". That was false for the screen
 * that matters most -- see perf_rep_flush. */
#define PERF_FLUSH_GAP_MS 1500u
static uint32_t PERF_EWRAM s_last_flush;   /* tick stamp, 0 = never flushed yet */

static void flush_now(void) {
  uint32_t t = perf_ticks();
  s_last_flush = t ? t : 1u;               /* 0 is the "never" sentinel */
  app_log_flush();
}

static void flush_rate_limited(void) {
  uint32_t t = perf_ticks();
  if (s_last_flush && perf_ms(t - s_last_flush) < PERF_FLUSH_GAP_MS) return;
  s_last_flush = t ? t : 1u;
  app_log_flush();
}

/* ---- spans ---------------------------------------------------------------------- */

typedef struct {
  const char* name;
  uint32_t    t0;
  PerfSd      sd0;
  PerfIcons   ic0;
  uint8_t     active;
  uint8_t     unclosed;   /* this span was force-closed by a nested begin */
  uint8_t     paused;     /* the baselines below currently hold DELTAS, not baselines */
} PerfSpan;
static PerfSpan PERF_EWRAM s_span;

void perf_span_begin(const char* name) {
  if (s_span.active) {
    /* Do not nest. Close the open one and TAG it, so the log names the bug instead of
     * quietly attributing one screen's I/O to another (see perf.h). */
    s_span.unclosed = 1;
    perf_span_end();
  }
  s_span.name     = name ? name : "?";
  s_span.t0       = perf_ticks();
  s_span.sd0      = perf_sd;
  s_span.ic0      = perf_icons;
  s_span.active   = 1;
  s_span.unclosed = 0;
  s_span.paused   = 0;
}

bool perf_span_active(void) { return s_span.active != 0; }

/* PAUSE AND RESUME ARE THE SAME OPERATION, applied to every stored field:
 *
 *     x = current - x
 *
 * Each field holds either a BASELINE (the counter's value when the span opened) or the
 * ELAPSED delta so far, and that one line is the involution between the two. Pausing
 * turns baselines into deltas; resuming turns those deltas back into baselines shifted
 * forward by exactly the paused interval, so the blocked stretch is subtracted from the
 * span's ms AND from every one of its counters. No snapshot storage at all, which is
 * the point: this build links with 416 bytes of EWRAM to spare (the Makefile's post-link
 * guard), and a second PerfSd+PerfIcons pair would be 60 of them for a two-call feature. */
static void span_flip(void) {
  s_span.t0 = perf_ticks() - s_span.t0;
  uint32_t* d = (uint32_t*)&s_span.sd0;
  const uint32_t* c = (const uint32_t*)&perf_sd;
  /* The two uint16 tails (rd_max/wr_max) are HIGH-WATER MARKS, not counters: there is
   * no meaningful "delta" for a maximum, and the span line does not print them. Walk
   * the uint32 prefix only, and let the compiler's own sizeof do the counting so a
   * field added to PerfSd cannot silently fall out of this loop. */
  for (unsigned i = 0; i < offsetof(PerfSd, rd_max) / sizeof(uint32_t); i++)
    d[i] = c[i] - d[i];
  d = (uint32_t*)&s_span.ic0;
  c = (const uint32_t*)&perf_icons;
  for (unsigned i = 0; i < sizeof(PerfIcons) / sizeof(uint32_t); i++)
    d[i] = c[i] - d[i];
}

void perf_span_pause(void) {
  if (!s_span.active || s_span.paused) return;   /* nested pause: a no-op, by contract */
  s_span.paused = 1;
  span_flip();
}

void perf_span_resume(void) {
  if (!s_span.active || !s_span.paused) return;  /* unmatched resume: a no-op */
  s_span.paused = 0;
  span_flip();
}

void perf_span_end(void) {
  if (!s_span.active) return;                    /* unmatched end: a no-op, by contract */
  perf_span_resume();                            /* ending while paused: close the gap first */
  s_span.active = 0;
  uint32_t ms = perf_ms(perf_ticks() - s_span.t0);
  log_line("perf %s: %lu ms, sd %lur/%lus/%lum %luw/%lus, icons %lu/%lu mru, "
           "bin %lu/%lu, rom %lu/%lu/%lu, null %lu%s",
           s_span.name, (unsigned long)ms,
           (unsigned long)(perf_sd.rd       - s_span.sd0.rd),
           (unsigned long)(perf_sd.rd_sect  - s_span.sd0.rd_sect),
           (unsigned long)(perf_sd.rd_multi - s_span.sd0.rd_multi),
           (unsigned long)(perf_sd.wr       - s_span.sd0.wr),
           (unsigned long)(perf_sd.wr_sect  - s_span.sd0.wr_sect),
           (unsigned long)(perf_icons.mru_hit  - s_span.ic0.mru_hit),
           (unsigned long)(perf_icons.mru_miss - s_span.ic0.mru_miss),
           (unsigned long)(perf_icons.bin      - s_span.ic0.bin),
           (unsigned long)(perf_icons.bin_pal  - s_span.ic0.bin_pal),
           (unsigned long)(perf_icons.rom_loc  - s_span.ic0.rom_loc),
           (unsigned long)(perf_icons.rom_frm  - s_span.ic0.rom_frm),
           (unsigned long)(perf_icons.rom_pal  - s_span.ic0.rom_pal),
           (unsigned long)(perf_icons.null_ans - s_span.ic0.null_ans),
           s_span.unclosed ? " !unclosed" : "");
  s_span.unclosed = 0;
  perf_sd_totals(false);      /* prints only if a batching high-water mark moved */
  flush_rate_limited();
}

/* ---- rollups --------------------------------------------------------------------- */

typedef struct {
  const char* name;
  uint32_t    t0, rd0, sect0;                  /* snapshot taken at perf_rep_begin */
  uint32_t    hit0, miss0, null0;              /* ...and the icon ladder's, likewise */
  uint32_t    total_ms, sum_rd, sum_sect;
  uint32_t    sum_hit, sum_miss, sum_null;
  uint16_t    count, worst_ms;
  uint8_t     running;
} PerfRep;
#define PERF_REP_SLOTS 3
static PerfRep PERF_EWRAM s_rep[PERF_REP_SLOTS];

void perf_rep_begin(int slot, const char* name) {
  if ((unsigned)slot >= PERF_REP_SLOTS) return;
  PerfRep* r = &s_rep[slot];
  if (!name) name = "?";
  /* A slot holding another screen's occurrences must not have this screen's added to
   * it. Emitting the old one here means a forgotten perf_rep_flush() costs a LATE line,
   * never a wrong one. Names are string literals, so this strcmp runs on a handful of
   * bytes once per occurrence. */
  if (r->count && (!r->name || strcmp(r->name, name) != 0)) perf_rep_flush(slot);
  r->name  = name;
  r->t0    = perf_ticks();
  r->rd0   = perf_sd.rd;
  r->sect0 = perf_sd.rd_sect;
  r->hit0  = perf_icons.mru_hit;
  r->miss0 = perf_icons.mru_miss;
  r->null0 = perf_icons.null_ans;
  r->running = 1;
}

void perf_rep_end(int slot) {
  if ((unsigned)slot >= PERF_REP_SLOTS) return;
  PerfRep* r = &s_rep[slot];
  if (!r->running) return;                        /* unmatched end: a no-op */
  r->running = 0;
  uint32_t ms = perf_ms(perf_ticks() - r->t0);
  /* Saturate rather than wrap: a count or a worst-case that silently rolls over is a
   * measurement that lies, and 65,535 occurrences is already "this ran forever". */
  if (r->count < 0xFFFFu) r->count++;
  if (ms > 0xFFFFu) ms = 0xFFFFu;
  if ((uint16_t)ms > r->worst_ms) r->worst_ms = (uint16_t)ms;
  r->total_ms += ms;
  r->sum_rd   += perf_sd.rd      - r->rd0;
  r->sum_sect += perf_sd.rd_sect - r->sect0;
  r->sum_hit  += perf_icons.mru_hit  - r->hit0;
  r->sum_miss += perf_icons.mru_miss - r->miss0;
  r->sum_null += perf_icons.null_ans - r->null0;
}

void perf_rep_flush(int slot) {
  if ((unsigned)slot >= PERF_REP_SLOTS) return;
  PerfRep* r = &s_rep[slot];
  if (r->count) {
    log_line("perf %s x%u: tot %lu ms, worst %u ms, sd %lur/%lus, icons %lu/%lu mru, null %lu",
             r->name, (unsigned)r->count, (unsigned long)r->total_ms,
             (unsigned)r->worst_ms, (unsigned long)r->sum_rd,
             (unsigned long)r->sum_sect, (unsigned long)r->sum_hit,
             (unsigned long)r->sum_miss, (unsigned long)r->sum_null);
    perf_sd_totals(false);
    /* RATE-LIMITED, not unconditional. "Once per screen EXIT and therefore rare" was
     * wrong about the screen that matters most: boxoam_exit() calls this on all ~9 of
     * pdna_box's exit paths, so START (the box's own menu key), B, and every PC<->Bank
     * crossing used to put an f_open + append + f_close on /PokeDNA/log.txt behind the
     * most-used navigation keys in the app -- on a change whose sibling goal is to make
     * this build FASTER, and with those writes counted into perf_sd.wr, so the
     * instrument inflated its own next span. Nothing is lost by waiting: the line is
     * already in log.c's 8 KiB ring, and the very next screen's span flushes it. The
     * only case that loses a rollup is a power-off inside the 1.5 s window. */
    flush_rate_limited();
  }
  memset(r, 0, sizeof *r);
}

/* ---- the batching high-water marks ------------------------------------------------ */

/* Last values printed, so the unforced call can stay silent until something moves.
 * Both marks are monotonic and bounded by the cluster size, so this converges. */
static uint16_t PERF_EWRAM s_rdmax_said, s_wrmax_said;

void perf_sd_totals(bool force) {
  if (!force && perf_sd.rd_max <= s_rdmax_said && perf_sd.wr_max <= s_wrmax_said) return;
  s_rdmax_said = perf_sd.rd_max;
  s_wrmax_said = perf_sd.wr_max;
  log_line("sd totals: %lur/%lus/%lum max %u sect, %luw/%lus/%lum max %u sect",
           (unsigned long)perf_sd.rd, (unsigned long)perf_sd.rd_sect,
           (unsigned long)perf_sd.rd_multi, (unsigned)perf_sd.rd_max,
           (unsigned long)perf_sd.wr, (unsigned long)perf_sd.wr_sect,
           (unsigned long)perf_sd.wr_multi, (unsigned)perf_sd.wr_max);
}

/* ---- boot facts ------------------------------------------------------------------ */

/* Both are ABSOLUTE symbols the devkitARM linker script emits (gba_cart.ld: __eheap_end
 * = ORIGIN(ewram) + LENGTH(ewram), __eheap_start = ABSOLUTE(.) right after .sbss).
 * Reading them at RUNTIME is strictly better than passing the number in with -D: the
 * Makefile's guard can only compute it POST-link, so a -D would have to describe the
 * PREVIOUS build. This is the same figure the guard prints as "EWRAM ok: N bytes free",
 * derived from the same two symbols, and it is exact for the binary that is running. */
extern unsigned char __eheap_start[];
extern unsigned char __eheap_end[];

uint32_t perf_ewram_free(void) {
  uint32_t s = (uint32_t)__eheap_start, e = (uint32_t)__eheap_end;
  return (e > s) ? (e - s) : 0u;
}

/* Which binary produced this log, and what budget it was built with. Without the git
 * hash a log cannot be tied to a tree, and "is this still the old build?" has already
 * cost this project a debugging session more than once. */
void perf_boot_line(void) {
  const char* git = PDNA_GIT_HASH;
  if (!git[0]) git = "?";
  log_line("perf: git %s, %s%s, ewram free %lu B",
           git,
#if PDNA_ARTLESS
           "artless",
#else
           "full-art",
#endif
#if defined(PDNA_DELTA)
           "/delta",
#elif defined(PDNA_STREAM_SPRITES)
           "/sd",
#else
           "",
#endif
           (unsigned long)perf_ewram_free());
}

/* The card's geometry AS FATFS SEES IT. Cluster size is the single most useful number
 * here: FatFs clips every multi-sector disk_read at the cluster boundary (ff.c f_read:
 * `if (csect + cc > fs->csize) cc = fs->csize - csect;`), so it is the hard ceiling on
 * how much any one batched read can ever carry. Fields: FATFS.fs_type / .csize /
 * .n_fatent / .free_clst (lib/fatfs/ff.h:127-147). Sector size is not a field in this
 * build -- FF_MAX_SS == FF_MIN_SS == 512 (lib/fatfs/ffconf.h:187) compiles .ssize out
 * entirely -- so 512 is stated as the constant it is. */
void perf_fs_facts(const void* fatfs) {
  const FATFS* fs = (const FATFS*)fatfs;
  if (!fs) return;
  const char* t = fs->fs_type == FS_FAT12 ? "FAT12"
                : fs->fs_type == FS_FAT16 ? "FAT16"
                : fs->fs_type == FS_FAT32 ? "FAT32"
                : fs->fs_type == FS_EXFAT ? "exFAT" : "?";
  /* free_clst stays 0xFFFFFFFF until something calls f_getfree(), and nothing here
   * does -- so say "?" rather than printing either a fantastically large number or a
   * plausible-looking 0 that would read as "the card is full". */
  unsigned long freec = (unsigned long)fs->free_clst;
  char fb[16];
  if (freec == 0xFFFFFFFFul) { fb[0] = '?'; fb[1] = 0; }
  else sniprintf(fb, sizeof fb, "%lu", freec);
  log_line("fs: %s, cluster %u sect (%lu KiB), 512 B/sect, %lu clusters, free %s",
           t, (unsigned)fs->csize,
           (unsigned long)(((uint32_t)fs->csize * 512u) >> 10),
           (unsigned long)(fs->n_fatent > 2 ? fs->n_fatent - 2 : 0), fb);
}

/* ---- the throughput sample -------------------------------------------------------
 *
 * The most valuable line in the log: it converts every sector count in every later
 * span into milliseconds, on the user's OWN card, instead of on paper.
 *
 * SIZE CHOICE. 8 reads of 512 B plus one of 16 KiB = 20 KiB total. That is under half
 * a percent of the 4.6 MB this tool already reads at boot before a save is even picked,
 * and measured against the ~1.5 s the mount + config path costs it is not perceptible.
 * Deliberately TWO sizes: the whole batching design rests on the claim that a 32-sector
 * read costs far less than 32 one-sector reads, and until this line existed that claim
 * was arithmetic over a datasheet. The 512 B samples are spread 8 KiB apart and the
 * 16 KiB sample sits past all of them, so no sample can be served out of a sector or
 * cluster another sample already warmed.
 *
 * The `calls` figures come from PERF_SD_READ and are the proof of the mechanism, not
 * just the timing: the 16 KiB read should show ONE disk_read call carrying 32 sectors.
 * If it shows more, the read was not 512-aligned or crossed a cluster boundary, and
 * the KB/s number must be read in that light.
 *
 * Own frame, noinline: FatFs' FIL is 592 B and this stack is ~12 KiB (guideline Sec 1).
 * Same discipline art_icons_cache.c's load_meta and savefile.c's FIL locals already
 * document -- one shallow frame, gone before the caller's own frame grows. */
/* N = 16, not 8, purely for RESOLUTION: one clock tick is 61 us, so a single 512 B
 * read (order 100-200 us) would be a 2-3 tick measurement with a +/-30% quantisation
 * error. Timing 16 of them and dividing brings that under +/-5% while still costing
 * only 8 KiB of card traffic. STRIDE keeps every sample in a different 8 KiB window so
 * none of them can be served out of a sector or cluster a previous one warmed, and the
 * 16 KiB sample sits past all of them for the same reason. */
#define PERF_SD_SAMPLE_SMALL   512u
#define PERF_SD_SAMPLE_N       16u
#define PERF_SD_SAMPLE_STRIDE  8192u
#define PERF_SD_SAMPLE_BIGOFF  (PERF_SD_SAMPLE_N * PERF_SD_SAMPLE_STRIDE)  /* 128 KiB */

static uint32_t kbs(uint32_t bytes, uint32_t us) {
  if (!us) return 0;
  return (uint32_t)(((uint64_t)bytes * 1000000ull) / us / 1024ull);
}

__attribute__((noinline))
static bool sample_one(const char* path, uint8_t* buf) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  if ((uint32_t)f_size(&f) < PERF_SD_SAMPLE_BIGOFF + PERF_SD_SAMPLE_BIG) {
    f_close(&f);
    return false;                       /* too small to sample honestly: try the next */
  }

  uint32_t t0, small_t, big_t, small_c, big_c;
  UINT br = 0;
  bool ok = true;

  /* One rmbl_pause for the WHOLE sample, matching art_session.c's kind_ready(): the
   * motor is a cart-bus device and every read below runs on that bus. */
  rmbl_pause();

  small_c = perf_sd.rd;
  t0 = perf_ticks();
  for (unsigned i = 0; i < PERF_SD_SAMPLE_N && ok; i++) {
    ok = f_lseek(&f, (FSIZE_t)i * PERF_SD_SAMPLE_STRIDE) == FR_OK &&
         f_read(&f, buf, PERF_SD_SAMPLE_SMALL, &br) == FR_OK && br == PERF_SD_SAMPLE_SMALL;
  }
  small_t = perf_ticks() - t0;
  small_c = perf_sd.rd - small_c;

  big_c = perf_sd.rd;
  t0 = perf_ticks();
  if (ok)
    ok = f_lseek(&f, PERF_SD_SAMPLE_BIGOFF) == FR_OK &&
         f_read(&f, buf, PERF_SD_SAMPLE_BIG, &br) == FR_OK && br == PERF_SD_SAMPLE_BIG;
  big_t = perf_ticks() - t0;
  big_c = perf_sd.rd - big_c;

  rmbl_resume();
  f_close(&f);

  if (!ok) { log_line("sd sample: read failed on %s", path); return true; }

  uint32_t small_us = perf_us(small_t), big_us = perf_us(big_t);
  log_line("sd: 512B x%u %luus/%luc, 16KiB %luus/%luc -> %lu/%lu KB/s",
           PERF_SD_SAMPLE_N,
           (unsigned long)(small_us / PERF_SD_SAMPLE_N), (unsigned long)small_c,
           (unsigned long)big_us, (unsigned long)big_c,
           (unsigned long)kbs(PERF_SD_SAMPLE_N * PERF_SD_SAMPLE_SMALL, small_us),
           (unsigned long)kbs(PERF_SD_SAMPLE_BIG, big_us));
  log_line("sd: sampled %s", path);
  return true;
}

void perf_sd_sample(const char* const* paths, void* scratch, uint32_t scratch_bytes) {
  if (!paths || !scratch || scratch_bytes < PERF_SD_SAMPLE_BIG + 4u) return;
  /* Round the destination UP to a word. Not cosmetic: lib/fatfs/diskio.c's disk_read
   * routes a 2-mod-4 destination through fc_bounce in 4-SECTOR chunks, so an unaligned
   * 16 KiB read would become eight driver round-trips wearing the label of one -- and
   * the fixed-vs-marginal number this whole line exists to measure would be wrong in
   * exactly the direction that flatters batching. */
  uint8_t* buf = (uint8_t*)(((uintptr_t)scratch + 3u) & ~(uintptr_t)3u);
  for (int i = 0; paths[i]; i++) {
    if (!paths[i][0]) continue;
    if (sample_one(paths[i], buf)) { perf_sd_totals(true); flush_now(); return; }
  }
  log_line("sd: no file big enough to sample (no rom registered, no icons.bin)");
  perf_sd_totals(true);      /* a baseline totals line in EVERY log, sample or not */
  flush_now();
}

#endif /* PDNA_PERF */
