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

/* The icons.bin link map. 16 fragments (2 + 2*16 = 34 DWORDs, 136 B) is generous for a
 * 451 KB file: at the 32 KB clusters a big card formats with, that is 14 clusters
 * total. Past 16 fragments fastseek_arm fails closed and this handle simply seeks the
 * ordinary way -- and the logged fragment count says so. */
#define ICON_CLMT_FRAGS 16

typedef struct {
  uint16_t row;      /* 0..439 -- THE key. NOT (row, frame): a slot holds BOTH frames */
  uint16_t age;      /* LRU clock, consulted only for unpinned slots                  */
  uint8_t  valid;    /* filled AND length-checked. Fill-then-validate, never before.   */
  uint8_t  pinned;   /* reserved for the plan set (wired in a later step)              */
  uint8_t  spec;     /* filled speculatively and not yet used -> first eviction victim */
  uint8_t  pal;      /* the row's palette bank 0..2, learned when the row was fetched  */
} IconSlot;
_Static_assert(sizeof(IconSlot) == 8, "IconSlot must stay 8 B -- it is budgeted as such");

static uint8_t EWRAM_BSS g_iconpool[ICON_POOL_BYTES] __attribute__((aligned(4)));

static struct {
  IconSlot slot[ICON_POOL_SLOTS];
  uint16_t age_clock;
  uint8_t  rung;                 /* ICON_RUNG_*                                      */
  uint8_t  cap;                  /* usable slots                                     */
  uint8_t  hot;                  /* slot index of the last row handed out, 0xFF none */
  uint8_t  suspended;            /* the extraction latch                             */
  uint8_t  fil_open;
  uint8_t  pal_have;             /* bit i = palette bank i is loaded                 */
  uint32_t epoch;                /* bumped by every reset; a debugging anchor        */
} EWRAM_BSS s_is;

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

/* Fill one slot. Brackets the GPIO freeze HERE rather than at the call sites, so no
 * caller can forget it -- box_oam.c's two rungs each carried their own and
 * art_fallbacks.c's carried none at all, which was a real gap: a motor toggle during
 * an in-flight ROM read is the garbled-wallpaper bug. */
static bool slot_fill(int idx, uint16_t row) {
  uint8_t* dst = slot_bytes(idx);
  bool ok = false;
  uint8_t pal = 0xFF;

  /* Fill-then-validate: claim the slot invalid FIRST, so a failed read leaves it
   * exactly as a genuine miss would rather than serving half a row as a real one. */
  s_is.slot[idx].valid = 0;

  rumble_io_suspend();
  if (s_is.rung == ICON_RUNG_CACHE) {
    ok = cache_fill_rows(row, 1, dst);
    if (ok) { PERF_ICON(bin); pal = s_palid[row]; }
  } else if (s_is.rung == ICON_RUNG_ROM) {
    ok = rom_fill_row(row, dst, &pal);
  }
  rumble_io_resume();

  if (!ok) return false;
  s_is.slot[idx].row   = row;
  s_is.slot[idx].pal   = pal;
  s_is.slot[idx].spec  = 0;
  s_is.slot[idx].age   = ++s_is.age_clock;
  s_is.slot[idx].valid = 1;
  return true;
}

/* ---- the public read ------------------------------------------------------------- */

const uint8_t* icon_store_row(uint16_t row) {
  if (s_is.rung == ICON_RUNG_NONE || row >= ART_ICONS_ROWS) return 0;

  int i = slot_find(row);
  if (i >= 0) {
    PERF_ICON(mru_hit);
    s_is.slot[i].age = ++s_is.age_clock;
    s_is.slot[i].spec = 0;                 /* a speculative row that got used is real */
    s_is.hot = (uint8_t)i;
    return slot_bytes(i);
  }

  PERF_ICON(mru_miss);
  int v = slot_victim();
  if (v < 0) return 0;                     /* every slot pinned AND hot: nothing to give */
  if (!slot_fill(v, row)) return 0;
  s_is.hot = (uint8_t)v;
  return slot_bytes(v);
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
