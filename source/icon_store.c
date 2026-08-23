/* icon_store.c — see icon_store.h for the contract and for what this replaces. */
#include "icon_store.h"

#include <string.h>

#include "art_icons_cache.h"  /* icons.bin FORMAT (no handle, no state)             */
#include "fastseek.h"         /* the ONE cltbl owner                                */
#include "ff.h"
#include "log.h"
#include "perf.h"             /* PERF_ICON -- which rung served, how expensively     */
#include "rom_mon.h"
#include "rumble.h"           /* the GPIO freeze every SD/ROM read must be inside    */
#include "sys.h"              /* EWRAM_BSS                                           */

/* ---- sizing ---------------------------------------------------------------------
 *
 * ICON_POOL_BYTES is the ONLY large allocation this design owns, and it was paid for,
 * not found: LOG_CAP 8192 -> 2048 (6,144 B) plus folding pdna_pick.c's g_mv into
 * g_idx (710 B) plus deleting art_fallbacks.c's 1,560 B s_icfr. The Makefile's
 * post-link "EWRAM ok: N bytes free" guard is the arbiter and must stay comfortably
 * positive; if it ever goes under ~512, shrink THIS first (5 rows still covers a
 * party), before reaching for anything else.
 *
 * 6 rows is not arbitrary: it is exactly the party/day-care working set, which is the
 * set behind the bob Guy has twice reported as broken. */
#define ICON_POOL_BYTES 6144u
#define ICON_ROW_BYTES  ART_ICONS_ROW_BYTES        /* 1024 */
#define ICON_POOL_SLOTS (ICON_POOL_BYTES / ICON_ROW_BYTES)

/* THE IRQ-OFF CAP ON ONE TRANSFER. IRQs are off for the whole of _EZFO_readSectors, so
 * the size of one f_read is the size of one blind window. Measured cost model
 * (docs/analysis-2026-08-23-io/measure-costmodel.md): ~7.2 us fixed + ceil(n/4) x
 * (6 us + card latency L) + n x 46 us per sector. An unbroken 21 KiB read is 11 card
 * chunks -- ~3.1 ms at L=100 us and ~13 ms at L=1 ms, i.e. most of a 16.7 ms frame,
 * with snd_vblank() unable to run. 8 KiB is 16 sectors / 4 chunks: ~1.16 ms and
 * ~4.8 ms. Bulk trades stutter FREQUENCY for stutter DEPTH, and this constant is where
 * that trade is budgeted. */
#define ICON_BULK_MAX_BYTES 8192u
#define ICON_BULK_MAX_ROWS  (ICON_BULK_MAX_BYTES / ICON_ROW_BYTES)   /* 8 */

/* 30 PC box slots is the largest set any screen declares; 40 leaves room and still
 * costs only 80 B. A longer declaration is TRUNCATED, not refused -- the extra rows
 * simply go through the single-row path, which is what they did before plans existed. */
#define ICON_PLAN_MAX ICON_STORE_PLAN_MAX
_Static_assert(ICON_PLAN_MAX >= 30, "a plan must hold a whole PC box");

/* The icons.bin link map. 16 fragments (2 + 2*16 = 34 DWORDs, 136 B) is generous for a
 * 451 KB file: at the 32 KB clusters a big card formats with, that is 14 clusters
 * total. Past 16 fragments fastseek_arm fails closed and this handle simply seeks the
 * ordinary way -- and the logged fragment count says so. */
#define ICON_CLMT_FRAGS 16

typedef struct {
  uint16_t row;      /* 0..439 -- THE key. NOT (row, frame): a slot holds BOTH frames */
  uint16_t age;      /* LRU clock, consulted only for unpinned slots                  */
  uint8_t  valid;    /* filled AND length-checked. Fill-then-validate, never before.   */
  uint8_t  pinned;   /* fetched by the live plan and NOT YET HANDED OUT. See below.    */
  uint8_t  spec;     /* filled speculatively and not yet used -> first eviction victim */
  uint8_t  pal;      /* the row's palette bank 0..2, learned when the row was fetched  */
} IconSlot;

/* WHAT `pinned` MEANS, exactly, because it is the whole anti-thrash mechanism.
 *
 * A pin is set by a plan sweep on every row it just fetched, and cleared the moment
 * icon_store_row() hands that row out. So a pin marks "the live plan paid for this row
 * and the screen has not drawn it yet" -- which is precisely the window in which
 * evicting it would be self-defeating. A pinned slot is NEVER an eviction victim.
 *
 * That is what turns a pool-sized bulk group into a bulk group: the sweep fetches k
 * rows, all k survive until the paint loop consumes them, and only then do they become
 * ordinary LRU rows that a later sweep may reuse. A plain LRU cannot do this -- on a
 * 21-cell cyclic sweep through a 6-row pool its hit rate is 0 %, every access evicting
 * the entry needed 21 accesses later, which is exactly Guy's "it gets worse the more
 * pokemon are moving".
 *
 * Retention outlives the pin on purpose: after the paint the rows are still VALID, just
 * unpinned, so a screen whose whole set fits (a 6-mon party) re-declares its plan on the
 * next repaint, finds every row resident, and pays nothing. */
_Static_assert(sizeof(IconSlot) == 8, "IconSlot must stay 8 B -- it is budgeted as such");

static uint8_t EWRAM_BSS g_iconpool[ICON_POOL_BYTES] __attribute__((aligned(4)));

/* NAMED, and that is not style. `static struct { ... } EWRAM_BSS s_is;` puts the
 * attribute after an ANONYMOUS struct's closing brace, where GCC binds it to the TYPE
 * and not to the object -- it warns ("'section' attribute does not apply to types") and
 * then silently leaves the object in ordinary .bss, i.e. IWRAM. This block was written
 * as EWRAM_BSS, measured at 0x03000E10, and was 90 B of IWRAM .bss nobody budgeted --
 * on a build with a documented boot crash at roughly 1,232 B of new IWRAM .bss. A named
 * type puts the attribute on the declarator, where it means what it says. */
typedef struct {
  IconSlot slot[ICON_POOL_SLOTS];
  uint16_t plan[ICON_PLAN_MAX];  /* the live declaration, in PAINT order             */
  uint8_t  plan_n;
  uint16_t age_clock;
  uint8_t  rung;                 /* ICON_RUNG_*                                      */
  uint8_t  cap;                  /* usable slots                                     */
  uint8_t  hot;                  /* slot index of the last row handed out, 0xFF none */
  uint8_t  suspended;            /* the extraction latch                             */
  uint8_t  fil_open;
  uint8_t  pal_have;             /* bit i = palette bank i is loaded                 */
  uint32_t epoch;                /* bumped by every reset; a debugging anchor        */
} IconStoreState;

static IconStoreState EWRAM_BSS s_is;

/* The one open handle, held for the whole session. 600 B of EWRAM_BSS -- which is the
 * whole point: the previous design's per-caller FIL was 2,272 B of IWRAM .bss, past the
 * ~1,232 B this build boot-crashes at. EWRAM_BSS (.sbss) is a different linker region
 * entirely and touches none of the ~10.8 KB IWRAM stack. */
static FIL   EWRAM_BSS s_ic_fil;
static DWORD EWRAM_BSS s_ic_clmt[FASTSEEK_ITEMS_FOR(ICON_CLMT_FRAGS)];

/* The path the handle was opened from, kept so store_reopen() can rebuild the handle
 * after FatFs latches a read error on it. 64 B matches the buffer art_icons_cache.c's
 * old path-keyed metadata block used for the same string. EWRAM_BSS, not IWRAM: this
 * module is already spending its IWRAM allowance on the palette tables. */
static char  EWRAM_BSS s_ic_path[64];

/* How many times this session had to rebuild the handle after a card read error. Also
 * the log's rate limiter. Reset with the store, so a re-registration starts clean. */
static uint16_t s_reopens = 0;

/* The ROM rung, when there is no icons.bin. */
static const RomMon* s_rm = 0;

/* ONE palette authority for BOTH rungs -- this is where art_icons_cache.c's 601 B of
 * statics and art_fallbacks.c's 97 B went. IWRAM by default and deliberately so: it
 * REPLACES, byte for byte, more IWRAM than it costs (536 in, 723 out), which is what
 * keeps this whole change on the safe side of the boot-crash threshold. */
static uint8_t  s_palid[ART_ICONS_ROWS];
static uint16_t s_pal[ART_ICONS_PALS][16];

_Static_assert(ART_ICONS_ROWS == 440, "the row axis is rom_mon's RM_TABLE_ENTRIES");
_Static_assert(ART_ICONS_PALS == ROM_MON_PALS, "both rungs share one 3-bank palette set");
_Static_assert(ICON_ROW_BYTES == 2u * ROM_MON_ICON_BYTES, "a row is both bob frames");
_Static_assert(ICON_POOL_SLOTS >= 6, "the pool must hold a full party without evicting");

static uint8_t* slot_bytes(int i) { return g_iconpool + (uint32_t)i * ICON_ROW_BYTES; }

/* ---- rung plumbing -------------------------------------------------------------- */

static void store_clear_slots(void) {
  memset(s_is.slot, 0, sizeof s_is.slot);
  s_is.age_clock = 0;
  s_is.hot = 0xFF;
  s_is.plan_n = 0;   /* a plan naming rows of a retired rung is worse than no plan */
}

static void store_close(void) {
  if (s_is.fil_open) {
    /* Order matters: drop the link map BEFORE the close, so nothing can observe a
     * closed handle still pointing at a table (fastseek.h rule 2's sibling). */
    s_ic_fil.cltbl = 0;
    f_close(&s_ic_fil);
    s_is.fil_open = 0;
  }
}

/* Rebuild the handle after FatFs has latched a read error on it.
 *
 * WHY THIS EXISTS, and why it is not optional. FatFs latches a disk error into
 * fp->err (ff.c:234's ABORT macro) and then REFUSES every later f_read on that handle
 * (ff.c:3932 checks fp->err before doing anything else). Only f_open clears it
 * (ff.c:3864). The code this module replaced opened and closed icons.bin around every
 * single 512 B frame, so it was accidentally immune: each read got a handle with a
 * clean error flag, and one transient glitch cost one icon.
 *
 * Holding the handle for the whole session -- the change that makes this module fast --
 * trades that immunity away. Without this function, ONE transient SD hiccup anywhere in
 * a session would silently kill every Pokemon icon until the next reboot, on a card
 * that is otherwise fine. That is a worse failure than the slowness we came here to fix.
 *
 * Returns false without disturbing the rung if the file cannot be reopened; the next
 * fetch simply tries again. The palette tail is NOT re-read -- it was validated at
 * reset and icons.bin is immutable while we hold it. */
static bool store_reopen(void) {
  store_close();
  if (!s_ic_path[0]) return false;
  if (f_open(&s_ic_fil, s_ic_path, FA_READ) != FR_OK) return false;
  s_is.fil_open = 1;
  fastseek_arm(&s_ic_fil, s_ic_clmt, sizeof s_ic_clmt / sizeof s_ic_clmt[0], "icons.bin");
  return true;
}

void icon_store_suspend(void) {
  store_close();
  store_clear_slots();
  s_is.rung = ICON_RUNG_NONE;
  s_is.cap = 0;
  s_is.pal_have = 0;
  s_is.suspended = 1;
}

void icon_store_reset(const char* icons_path, const struct RomMon* rm) {
  store_close();
  store_clear_slots();
  s_is.suspended = 0;
  s_is.epoch++;
  s_is.rung = ICON_RUNG_NONE;
  s_is.cap = 0;
  s_is.pal_have = 0;
  s_rm = (rm && rm->ok) ? rm : 0;

  /* Rung 1: the extracted cache. Preferred because it needs no ROM registered THIS
   * session -- the card outlives the registration -- and because its rows are laid out
   * in row order, so a Pokedex page is usually one contiguous span. */
  s_reopens = 0;
  s_ic_path[0] = 0;
  if (icons_path && icons_path[0]) {
    strncpy(s_ic_path, icons_path, sizeof s_ic_path - 1);
    s_ic_path[sizeof s_ic_path - 1] = 0;
  }
  if (s_ic_path[0] && f_open(&s_ic_fil, s_ic_path, FA_READ) == FR_OK) {
    s_is.fil_open = 1;
    fastseek_arm(&s_ic_fil, s_ic_clmt, sizeof s_ic_clmt / sizeof s_ic_clmt[0], "icons.bin");
    if (art_icons_meta_read_fp(&s_ic_fil, s_palid, s_pal)) {
      s_is.rung = ICON_RUNG_CACHE;
      s_is.pal_have = (uint8_t)((1u << ART_ICONS_PALS) - 1u);   /* all three, from the tail */
    } else {
      /* A file that is the wrong size or unreadable is not a rung. Fall through to the
       * ROM rather than serving a handle we could not validate. */
      log_line("icons: cache tail unreadable - falling back");
      store_close();
    }
  }
  if (s_is.rung == ICON_RUNG_NONE && s_rm) s_is.rung = ICON_RUNG_ROM;

  s_is.cap = (s_is.rung == ICON_RUNG_NONE) ? 0 : (uint8_t)ICON_POOL_SLOTS;
  log_line("icons: store rung=%s cap=%u", s_is.rung == ICON_RUNG_CACHE ? "cache"
                                        : s_is.rung == ICON_RUNG_ROM   ? "rom" : "none",
           s_is.cap);
}

int      icon_store_rung(void)     { return s_is.rung; }
uint16_t icon_store_capacity(void) { return s_is.cap; }

/* ---- slot lookup and eviction ---------------------------------------------------- */

static int slot_find(uint16_t row) {
  for (int i = 0; i < s_is.cap; i++)
    if (s_is.slot[i].valid && s_is.slot[i].row == row) return i;
  return -1;
}

/* Victim order: a free slot, then a speculatively-filled slot nothing has used yet,
 * then the oldest unpinned slot. The middle rung is what guarantees read-ahead can
 * never cost a re-fetch of something a screen actually asked for. The HOT slot (the
 * last row handed out) is never a victim -- that is what makes the pointer contract
 * hold for the length of one blit. */
static int slot_victim(void) {
  int v = -1;
  for (int i = 0; i < s_is.cap; i++)
    if (!s_is.slot[i].valid && i != s_is.hot) return i;
  for (int i = 0; i < s_is.cap; i++)
    if (s_is.slot[i].spec && !s_is.slot[i].pinned && i != s_is.hot) return i;
  for (int i = 0; i < s_is.cap; i++) {
    if (s_is.slot[i].pinned || i == s_is.hot) continue;
    if (v < 0 || s_is.slot[i].age < s_is.slot[v].age) v = i;
  }
  return v;
}

/* ---- the two rungs' fills -------------------------------------------------------- */

/* A running byte-sum over a row, the trick box_oam.c's stage_sum already uses:
 * comparing two passes' sums verifies a read without a second 1024 B buffer, so the
 * caller's own destination serves as both passes' target. */
static uint32_t row_sum(const uint8_t* b) {
  uint32_t v = 0;
  for (unsigned i = 0; i < ICON_ROW_BYTES; i++) v += b[i];
  return v;
}

/* ROM rung: locate the row (verified -- the pointer and the palette bank ARE "which
 * species is this", and a garbled pointer that still lands inside the image would
 * confidently paint the wrong Pokemon), then read both frames twice and require the
 * two passes to agree. The EZ read path can return success holding garbage; nothing
 * about that changed, so neither does the verify. */
static bool rom_fill_row(uint16_t row, uint8_t* dst, uint8_t* pal_out) {
  if (!s_rm) return false;
  RomMonLoc loc;
  int unstable = 0;
  PERF_ICON(rom_loc);
  if (!rom_mon_locate_row_verified(s_rm, row, &loc, 4, &unstable)) {
    if (unstable) log_line("icons: rom locate unstable row=%u", row);
    return false;
  }
  /* Both frames in ONE read: rom_mon_open already range-checked 2 x 512 B at this
   * pointer, so a 1024 B read at loc.tiles is exactly as bounded as two 512 B reads
   * were -- for half the transactions. */
  for (int a = 0; a < 4; a++) {
    PERF_ICON(rom_frm);
    if (!s_rm->rc->read(s_rm->rc->ctx, loc.tiles, dst, ICON_ROW_BYTES)) return false;
    uint32_t s1 = row_sum(dst);
    PERF_ICON(rom_frm);
    if (!s_rm->rc->read(s_rm->rc->ctx, loc.tiles, dst, ICON_ROW_BYTES)) return false;
    if (row_sum(dst) == s1) { *pal_out = loc.pal; return true; }
  }
  log_line("icons: rom read unstable row=%u", row);
  return false;
}

/* Cache rung: one seek + one read, no per-read verify -- the whole file's FNV was
 * already checked once this session by art_session, so a single read is trusted the
 * same way a compiled .rodata array is. */
static bool cache_fill_rows(uint16_t first, uint16_t n, uint8_t* dst) {
  if (s_is.suspended) return false;   /* the extraction latch outranks everything here */
  if (s_is.fil_open && art_icons_read_rows_fp(&s_ic_fil, first, n, dst)) return true;

  /* Either the read failed, or a previous failure already left us with no handle.
   * Both recover the same way: build a fresh handle and try exactly once more.
   *
   * Exactly once. A card that is genuinely gone must fail fast and let the caller fall
   * back to the ROM rung or to a text layout, not spin reopening a file that is not
   * there. And note the asymmetry that makes the closed-handle case matter: a failed
   * reopen leaves fil_open at 0, so without the retry ALSO covering that state, one
   * transient glitch would strand the rung closed for the rest of the session -- the
   * exact bug the old open-per-frame code could not have.
   *
   * Logged, because a reopen means the CARD glitched and that is worth seeing in a
   * hardware log -- but capped, because a dying card would otherwise turn every icon
   * into a log line and burn log.c's per-run byte budget on one message. */
  if (s_reopens < 3) log_line("icons: reopening icons.bin after a failed read (row %u)",
                              (unsigned)first);
  else if (s_reopens == 3) log_line("icons: further icons.bin reopens not logged");
  if (s_reopens < 0xFFFF) s_reopens++;

  if (!store_reopen()) return false;
  return art_icons_read_rows_fp(&s_ic_fil, first, n, dst);
}

/* ONE TRANSFER: `k` rows whose SOURCE BYTES ARE STRICTLY CONSECUTIVE, into the `k`
 * CONSECUTIVE slots starting at `s0`. Both halves of that sentence are load-bearing --
 * consecutive source bytes are what make it one f_read, consecutive slots are what make
 * it one destination. `pin` marks the run as owed to the live plan.
 *
 * Brackets the GPIO freeze HERE rather than at the call sites, so no caller can forget
 * it -- box_oam.c's two rungs each carried their own and art_fallbacks.c's carried none
 * at all, which was a real gap: a motor toggle during an in-flight ROM read is the
 * garbled-wallpaper bug.
 *
 * FILL-THEN-VALIDATE, ACROSS THE WHOLE RUN. Every slot is claimed invalid first and
 * validated only after the read reports full length. A SHORT read invalidates the
 * ENTIRE run, not the tail: a short read does not tell us where the bytes stopped, so
 * no slot in it can be trusted -- and a half-filled slot served as a real one is the
 * wrong Pokemon on screen with nothing logged. */
static bool run_fill(int s0, const uint16_t* rows, int k, bool pin) {
  uint8_t pal[ICON_BULK_MAX_ROWS];
  bool ok = false;

  if (k <= 0 || k > (int)ICON_BULK_MAX_ROWS) return false;
  for (int i = 0; i < k; i++) s_is.slot[s0 + i].valid = 0;

  rumble_io_suspend();
  if (s_is.rung == ICON_RUNG_CACHE) {
    ok = cache_fill_rows(rows[0], (uint16_t)k, slot_bytes(s0));
    if (ok) for (int i = 0; i < k; i++) { PERF_ICON(bin); pal[i] = s_palid[rows[i]]; }
  } else if (s_is.rung == ICON_RUNG_ROM) {
    ok = true;
    for (int i = 0; i < k && ok; i++)
      ok = rom_fill_row(rows[i], slot_bytes(s0 + i), &pal[i]);
  }
  rumble_io_resume();

  if (!ok) return false;
  for (int i = 0; i < k; i++) {
    s_is.slot[s0 + i].row    = rows[i];
    s_is.slot[s0 + i].pal    = pal[i];
    s_is.slot[s0 + i].spec   = 0;
    s_is.slot[s0 + i].pinned = pin ? 1 : 0;
    s_is.slot[s0 + i].age    = ++s_is.age_clock;
    s_is.slot[s0 + i].valid  = 1;
  }
  return true;
}

static bool slot_fill(int idx, uint16_t row) { return run_fill(idx, &row, 1, false); }

/* ---- the plan and its sorted, merged sweep --------------------------------------- */

/* Where row `row`'s bytes live in the rung that is serving, or ICON_OFF_UNKNOWN.
 *
 * icons.bin is dense and ordered by construction, so this is arithmetic and every row
 * offset is a multiple of 512 -- which is why an icons.bin bulk read goes straight into
 * the pool instead of through lib/fatfs/diskio.c's 4-sector-capped bounce buffer.
 *
 * The ROM rung has no arithmetic answer: its icon blobs are scattered over ~1.4 MB with
 * strides of 2.5-3 KB, so an offset there is a TABLE LOOKUP that does not exist yet.
 * ICON_OFF_UNKNOWN disables both the sort and the merge for that sweep, which is the
 * honest answer: without a real offset we cannot prove two rows are adjacent, and
 * guessing means reading 1024 B out of the middle of the wrong Pokemon. */
#define ICON_OFF_UNKNOWN 0xFFFFFFFFu
static uint32_t src_off(uint16_t row) {
  if (s_is.rung == ICON_RUNG_CACHE) return (uint32_t)row * ICON_ROW_BYTES;
  return ICON_OFF_UNKNOWN;
}

static int plan_index(uint16_t row) {
  for (int i = 0; i < s_is.plan_n; i++) if (s_is.plan[i] == row) return i;
  return -1;
}

/* Eviction preference as ONE sortable key, so the whole victim policy is a single
 * insertion sort instead of three passes that then have to be re-merged. Smaller wins:
 * a free slot, then a speculatively-filled slot nothing has used yet, then the oldest.
 * The speculative rung is what will guarantee read-ahead can never cost a re-fetch of
 * something a screen actually asked for. `age` is uint16, so it fits below the class. */
static uint32_t victim_key(int i) {
  uint32_t cls = !s_is.slot[i].valid ? 0u : (s_is.slot[i].spec ? 1u : 2u);
  return (cls << 16) | (uint32_t)s_is.slot[i].age;
}

/* Fill the pool with the next GROUP of planned rows, starting the walk at plan index
 * `start` (the row whose miss triggered this, or 0 from icon_store_plan()).
 *
 * The six steps of the sweep, in order:
 *   1. drop every pin -- see the note below;
 *   2. walk the plan FORWARD from `start` (never wrapping), collecting rows that are
 *      NOT resident, until the pool is full. Resident rows are skipped, not re-fetched;
 *   3. sort the collected rows ASCENDING BY SOURCE BYTE OFFSET;
 *   4. pick victim slots by policy (free, then speculative, then LRU) and then put the
 *      chosen slot indices in ASCENDING order, so a merged run lands on consecutive
 *      slots;
 *   5. merge entries whose offsets are strictly adjacent AND whose slots are strictly
 *      adjacent into one run, breaking at ICON_BULK_MAX_ROWS;
 *   6. one f_lseek + one f_read per run, and validate only on full length.
 *
 * ASCENDING OFFSET ORDER IS THE LOAD-BEARING DETAIL and it is not about merging. FatFs
 * restarts a cluster walk from the start of the chain only on a BACKWARD seek
 * (ff.c:4527); a forward seek resumes incrementally (ff.c:4521). So a sweep that only
 * ever moves forward is cheap even on a rung that never merges a single pair, and even
 * if the cluster link map failed to build. The sort is an insertion sort over at most
 * `cap` keys -- a few thousand cycles, once per group, against a saved transaction that
 * costs 24 halfword cart writes plus a card-latency poll.
 *
 * WHY STEP 1 DROPS EVERY PIN. A pin means "the plan fetched this and the screen has not
 * drawn it yet". A sweep only ever runs at the start of a paint (icon_store_plan) or
 * because the paint has walked PAST the current group and missed -- in both cases the
 * previous group has served its purpose. Dropping the pins here is also what guarantees
 * this function can never come up empty-handed: after step 1 every slot is a candidate,
 * so a plan sweep always has room for at least one row and a planned miss can never
 * return NULL for want of a slot. `hot` goes with them, on the same authority the
 * pointer contract already gives: any row pointer a caller still holds died the moment
 * this icon_store_* call began. Freeing `hot` is what lets a run land on slot 0. */
static void plan_sweep(int start) {
  uint16_t want[ICON_POOL_SLOTS];
  uint8_t  dst[ICON_POOL_SLOTS];
  int nw = 0, cap = s_is.cap;

  if (s_is.plan_n <= 0 || cap <= 0) return;
  if (start < 0 || start >= s_is.plan_n) start = 0;

  for (int i = 0; i < cap; i++) s_is.slot[i].pinned = 0;
  s_is.hot = 0xFF;

  /* FORWARD ONLY, never wrapping. A plan is declared in PAINT order, so the rows after
   * `start` are the ones about to be drawn and the rows before it are the ones just
   * drawn. Wrapping would re-fetch the head of the page at the tail of the paint --
   * measured at 6 wasted sectors on a 21-cell contiguous page, for rows already on
   * screen. A caller that then asks for one of them gets its own sweep. */
  for (int k = start; k < s_is.plan_n && nw < cap; k++) {
    uint16_t r = s_is.plan[k];
    if (slot_find(r) >= 0) continue;                 /* already here: no I/O, no slot */
    want[nw++] = r;
  }
  if (!nw) return;

  /* 3. ascending by source offset. An UNKNOWN offset sorts stable, so a rung with no
   *    offset table keeps the plan's own order and simply never merges. */
  for (int i = 1; i < nw; i++) {
    uint16_t v = want[i];
    uint32_t vo = src_off(v);
    int j = i - 1;
    while (j >= 0 && src_off(want[j]) > vo) { want[j + 1] = want[j]; j--; }
    want[j + 1] = v;
  }

  /* 4. victims by policy, then re-ordered ascending by slot index. Both orders matter
   *    and they are different orders: the policy decides WHICH rows die, the ascending
   *    index decides whether the survivors can be filled in one transfer. */
  {
    uint8_t cand[ICON_POOL_SLOTS];
    int nc = cap;
    for (int i = 0; i < cap; i++) cand[i] = (uint8_t)i;
    for (int i = 1; i < nc; i++) {
      uint8_t v = cand[i];
      uint32_t vk = victim_key(v);
      int j = i - 1;
      while (j >= 0 && victim_key(cand[j]) > vk) { cand[j + 1] = cand[j]; j--; }
      cand[j + 1] = v;
    }
    if (nw > nc) nw = nc;                            /* cannot happen: nc == cap       */
    for (int i = 0; i < nw; i++) dst[i] = cand[i];
    for (int i = 1; i < nw; i++) {                   /* ascending slot index           */
      uint8_t v = dst[i]; int j = i - 1;
      while (j >= 0 && dst[j] > v) { dst[j + 1] = dst[j]; j--; }
      dst[j + 1] = v;
    }
  }

  /* 5 + 6. maximal runs, one transfer each. */
  for (int i = 0; i < nw; ) {
    int k = 1;
    uint32_t o = src_off(want[i]);
    while (i + k < nw && k < (int)ICON_BULK_MAX_ROWS &&
           o != ICON_OFF_UNKNOWN &&
           src_off(want[i + k]) == o + (uint32_t)k * ICON_ROW_BYTES &&
           dst[i + k] == dst[i] + k) k++;
    run_fill(dst[i], want + i, k, true);             /* a failed run stays invalid     */
    i += k;
  }
}

int icon_store_plan(const uint16_t* rows, int n) {
  s_is.plan_n = 0;
  if (s_is.rung == ICON_RUNG_NONE || !rows || n <= 0) return 0;

  for (int i = 0; i < n && s_is.plan_n < (int)ICON_PLAN_MAX; i++) {
    uint16_t r = rows[i];
    if (r >= ART_ICONS_ROWS) continue;               /* bound EVERY index: the pool's
                                                      * EWRAM neighbours are live save
                                                      * data                          */
    if (plan_index(r) >= 0) continue;                /* a duplicate must not eat a slot */
    s_is.plan[s_is.plan_n++] = r;
  }
  plan_sweep(0);

  int res = 0;
  for (int i = 0; i < s_is.plan_n; i++) if (slot_find(s_is.plan[i]) >= 0) res++;
  return res;
}

bool icon_store_plan_resident(void) {
  if (s_is.rung == ICON_RUNG_NONE) return true;      /* nothing to read either way    */
  if (s_is.plan_n <= 0) return false;
  for (int i = 0; i < s_is.plan_n; i++) if (slot_find(s_is.plan[i]) < 0) return false;
  return true;
}

/* ---- the public read ------------------------------------------------------------- */

const uint8_t* icon_store_row(uint16_t row) {
  if (s_is.rung == ICON_RUNG_NONE || row >= ART_ICONS_ROWS) return 0;

  int i = slot_find(row);
  if (i < 0) {
    PERF_ICON(mru_miss);
    int pi = plan_index(row);
    if (pi >= 0) {
      /* A PLANNED miss. The screen has walked past the group the last sweep fetched, so
       * fetch the next one -- in bulk, sorted, merged -- rather than this single row.
       * That is what turns a 21-cell page against a 6-row pool from 21 transfers into
       * ceil(21/6) sweeps of merged runs. */
      plan_sweep(pi);
      i = slot_find(row);
      if (i < 0) return 0;                   /* the sweep could not produce it          */
    } else {
      int v = slot_victim();
      if (v < 0) return 0;                   /* every slot pinned AND hot: nothing to give */
      if (!slot_fill(v, row)) return 0;
      i = v;
    }
  } else {
    PERF_ICON(mru_hit);
  }

  s_is.slot[i].age    = ++s_is.age_clock;
  s_is.slot[i].spec   = 0;                   /* a speculative row that got used is real */
  s_is.slot[i].pinned = 0;                   /* handed out: the plan's debt is paid     */
  s_is.hot = (uint8_t)i;
  return slot_bytes(i);
}

/* ---- palettes -------------------------------------------------------------------- */

/* Load ROM palette bank `id` if this session has not already. At most 3 fills ever. */
static bool rom_pal_bank(uint8_t id) {
  if (id >= ART_ICONS_PALS) return false;
  if (s_is.pal_have & (1u << id)) return true;
  if (!s_rm) return false;
  bool ok;
  PERF_ICON(rom_pal);
  rumble_io_suspend();
  ok = rom_mon_icon_pal(s_rm, id, s_pal[id]) != 0;
  rumble_io_resume();
  if (ok) s_is.pal_have |= (uint8_t)(1u << id);
  return ok;
}

uint8_t icon_store_pal_id(uint16_t row) {
  if (s_is.rung == ICON_RUNG_NONE || row >= ART_ICONS_ROWS) return 0xFF;
  if (s_is.rung == ICON_RUNG_CACHE) {
    uint8_t id = s_palid[row];
    return id < ART_ICONS_PALS ? id : 0xFF;
  }
  /* ROM rung: the bank came back with the row, so a caller that just fetched the row
   * pays nothing. A caller that did not gets one locate -- rare by construction, since
   * every consumer asks for the tiles first. */
  int i = slot_find(row);
  if (i >= 0 && s_is.slot[i].pal < ART_ICONS_PALS) return s_is.slot[i].pal;
  if (!s_rm) return 0xFF;
  RomMonLoc loc;
  PERF_ICON(rom_loc);
  if (!rom_mon_locate_row_verified(s_rm, row, &loc, 4, 0)) return 0xFF;
  return loc.pal < ART_ICONS_PALS ? loc.pal : 0xFF;
}

bool icon_store_pal_at(int i, uint16_t out[16]) {
  if (s_is.rung == ICON_RUNG_NONE || i < 0 || i >= (int)ART_ICONS_PALS || !out) return false;
  if (!(s_is.pal_have & (1u << i)) && !rom_pal_bank((uint8_t)i)) return false;
  memcpy(out, s_pal[i], 32);
  return true;
}

bool icon_store_pal(uint16_t row, uint16_t out[16]) {
  uint8_t id = icon_store_pal_id(row);
  if (id >= ART_ICONS_PALS) return false;
  return icon_store_pal_at((int)id, out);
}
