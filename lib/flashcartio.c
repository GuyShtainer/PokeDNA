#include "sys.h"

#include "flashcartio.h"

#if FLASHCARTIO_ED_ENABLE != 0
#include "everdrivegbax5/disk.h"
#include "everdrivegbax5/everdrive.h"
#endif

#if FLASHCARTIO_EZFO_ENABLE != 0
#include "ezflashomega/io_ezfo.h"
#endif

ActiveFlashcart active_flashcart = NO_FLASHCART;
volatile bool flashcartio_is_reading = false;
volatile unsigned long flashcartio_read_retries  = 0;
volatile unsigned long flashcartio_read_failures = 0;

/* ---- game-pak bus timing (see flashcartio.h for the why) ----------------- */

#define FCIO_WAITCNT (*(volatile unsigned short*)0x04000204)

/* WS0 3/1 + prefetch + SRAM 8: the timing a commercial GBA cartridge asks for. */
#define FCIO_WAIT_FAST 0x4317u
/* WS0 3/2 + prefetch + SRAM 8: one rung down. Gives back only the sequential-access
 * cycle and keeps the prefetch buffer (most of the win). This is the value upstream's
 * gba-flashcartio README blesses for this cart family -- "3,2 or slower". */
#define FCIO_WAIT_MID  0x4307u

static const unsigned short s_rungs[] = { FCIO_WAIT_FAST, FCIO_WAIT_MID };
#define FCIO_NRUNGS ((int)(sizeof s_rungs / sizeof s_rungs[0]))

/* Whatever the flashcart loader handed us — restored around every SD transfer.
 * Captured before the first change so we give the card back exactly the timing
 * it was working at, rather than a value this file invented. */
static unsigned short s_wait_inherited = 0;
static bool           s_wait_captured  = false;
static int            s_rung           = 0;      /* index into s_rungs           */
static bool           s_locked         = false;  /* a self-test failed: no boost */

void flashcartio_bus_fast(void) {
  if (!s_wait_captured) { s_wait_inherited = FCIO_WAITCNT; s_wait_captured = true; }
  /* Defensive: after a fully-exhausted ladder s_rung == FCIO_NRUNGS, which would
   * index one past s_rungs[]. s_locked is set in that case and short-circuits it,
   * but the clamp costs one compare and removes the class. */
  if (s_rung >= FCIO_NRUNGS) s_rung = FCIO_NRUNGS - 1;
  /* Once a self-test has demoted us, a later call must not quietly re-boost. */
  FCIO_WAITCNT = s_locked ? s_wait_inherited : s_rungs[s_rung];
}

void flashcartio_bus_hold(void) {
  if (!s_wait_captured) { s_wait_inherited = FCIO_WAITCNT; s_wait_captured = true; }
  s_locked = true;
  FCIO_WAITCNT = s_wait_inherited;
}

int flashcartio_bus_rung(void) { return s_locked ? FCIO_NRUNGS : s_rung; }

unsigned short flashcartio_bus_inherited(void) { return s_wait_inherited; }

/* ---- fast-bus self-test (see flashcartio.h for the contract and its limits) -- */

/* Image bounds, from devkitARM's gba_cart.ld. Absolute/section symbols: take their
 * ADDRESS, never their value. Defined in every build at every size, so the probe
 * needs no build-specific addresses and no #ifdef for the artless image. (Checked
 * on this project's link: __rom_end__ - 0x08000000 == the exact byte size of
 * PokeDNA.gba, so the tail window is always real file data.) */
extern char __text_start[];
extern char __rom_end__[];

#define FCIO_PROBE_BYTES 4096u
#define FCIO_PROBE_WORDS (FCIO_PROBE_BYTES / 4u)
#define FCIO_PROBE_WINS  3

/* DMA scratch: VRAM between the end of the Mode-3 framebuffer (0x06012C00) and the
 * first bitmap-mode OBJ tile (0x06014000). Free without spending one byte of the
 * ~1.5 KiB of EWRAM this project has left. Mode 4/5 page 1 DOES cover it, so the
 * DMA pass forces DCNT_BLANK for its duration. Boot-only: in a TILE mode this is
 * OBJ tile storage and no blank can make that safe. */
#define FCIO_PROBE_DST ((volatile u32*)0x06013000)
#define FCIO_DISPCNT   (*(volatile unsigned short*)0x04000000)
#define FCIO_DCNT_BLANK 0x0080u

/* Run the probe out of IWRAM so a bad instruction fetch at the rung under test can
 * never take down the very code that exists to detect it. The price is that this
 * pass cannot test instruction fetch at all -- that is what
 * flashcartio_bus_probe_fetch() is for. Flip to 0 if the IWRAM boot cliff ever
 * bites; the test still works, it just becomes able to crash. */
#ifndef FCIO_PROBE_IN_IWRAM
#define FCIO_PROBE_IN_IWRAM 1
#endif
#if FCIO_PROBE_IN_IWRAM
#define FCIO_IW __attribute__((section(".iwram"), long_call, noinline))
#else
#define FCIO_IW __attribute__((noinline))
#endif

/* Fault injector for regression-testing the FAILURE paths in an emulator, where the
 * bus never actually lies. An untested error branch first executes on the user's
 * hardware, inside the build that is already hanging -- so every branch below has a
 * switch that makes it fire on demand:
 *   1 = CPU pass fails at every rung   -> ladder exhausts, demote + latch
 *   2 = DMA pass fails at every rung   -> ditto, with the DMA verdict
 *   3 = GPIO pass fails                -> rumble verdict, ladder untouched
 *   4 = ROM-fetch pass fails           -> fetch verdict, ladder untouched
 *   5 = CPU pass fails at rung 0 ONLY  -> the ladder stops at rung 1 (0x4307)
 *   6 = the two reference passes differ-> FCIO_BUS_UNSTABLE_SLOW
 * Never define it in a shipped build. */
#ifndef FCIO_PROBE_FORCE_FAIL
#define FCIO_PROBE_FORCE_FAIL 0
#endif
#define FCIO_FAULT(n, x) (((n) == FCIO_PROBE_FORCE_FAIL) ? ((x) ^ 1u) : (x))
#define FCIO_FAULT_RUNG0 (FCIO_PROBE_FORCE_FAIL == 5)

/* Order-sensitive rolling checksum. Volatile source: every word must be a real cart
 * read, not something the optimiser hoisted or reused.
 * `ag`, when non-NULL, is toggled every 8 words so the caller can interleave a
 * cart-bus write with the read burst -- the documented FPGA hazard. */
FCIO_IW static u32 fcio_sum_ag(const volatile u32* p, u32 words, FlashcartioAgitator ag) {
  u32 s = 0x9E3779B9u;
  u32 i = 0;
  while (words--) {
    u32 v = *p++;
    s = ((s << 1) | (s >> 31)) ^ v;
    if (ag && ((++i & 7u) == 0u)) ag((int)((i >> 3) & 1u));
  }
  if (ag) ag(0);                       /* never leave the line asserted */
  return s;
}

/* Same window, fetched by DMA3 straight out of ROM -- the box-idle animation's
 * access pattern (a 15 KiB dma3_cpy from high ROM every tick). Checksummed out of
 * VRAM, which cannot itself lie. */
FCIO_IW static u32 fcio_sum_dma(const volatile u32* p, u32 words) {
  DMA_Copy(3, p, FCIO_PROBE_DST, DMA32 | words);
  while (REG_DMA3CNT & DMA_ENABLE) { }          /* immediate DMA halts the CPU, but
                                                 * poll anyway: it costs nothing and
                                                 * documents the dependency */
  return fcio_sum_ag(FCIO_PROBE_DST, words, 0);
}

/* THE ROM-RESIDENT TWIN. Deliberately NOT in IWRAM: this one's instruction fetches
 * come off the cart at the rung under test, which is the only way to exercise the
 * GamePak prefetch buffer (WAITCNT bit 14 engages for opcode fetches only). Unrolled
 * by 8 so the loop body is far larger than the prefetch buffer and every iteration
 * really does fetch. Same checksum as fcio_sum_ag, so their results are comparable.
 * If the fetch path is what is broken on a given unit, THIS is where it crashes --
 * which is exactly why it runs last, after the IWRAM passes, and after the caller
 * has flushed a log line saying it is about to. */
__attribute__((noinline)) static u32 fcio_sum_romcode(const volatile u32* p, u32 words) {
  u32 s = 0x9E3779B9u;
  u32 n = words >> 3;
  u32 r = words & 7u;
  while (n--) {
    s = ((s << 1) | (s >> 31)) ^ *p++;
    s = ((s << 1) | (s >> 31)) ^ *p++;
    s = ((s << 1) | (s >> 31)) ^ *p++;
    s = ((s << 1) | (s >> 31)) ^ *p++;
    s = ((s << 1) | (s >> 31)) ^ *p++;
    s = ((s << 1) | (s >> 31)) ^ *p++;
    s = ((s << 1) | (s >> 31)) ^ *p++;
    s = ((s << 1) | (s >> 31)) ^ *p++;
  }
  while (r--) s = ((s << 1) | (s >> 31)) ^ *p++;
  return s;
}

static void fcio_windows(const volatile u32** w) {
  unsigned long lo   = (unsigned long)__text_start;
  unsigned long hi   = (unsigned long)__rom_end__;
  unsigned long span = hi - lo;
  if (span < 8u * FCIO_PROBE_BYTES) {           /* pathologically small image */
    w[0] = (const volatile u32*)lo;
    w[1] = (const volatile u32*)lo;
    w[2] = (const volatile u32*)lo;
    return;
  }
  w[0] = (const volatile u32*)((lo + (span >> 2)) & ~3ul);      /* 1/4 in  */
  w[1] = (const volatile u32*)((lo + (span >> 1)) & ~3ul);      /* 1/2 in  */
  w[2] = (const volatile u32*)((hi - FCIO_PROBE_BYTES) & ~3ul); /* the tail */
}

/* The reference checksums, taken at the loader's own timing. Kept so the later
 * pass/fail probes (GPIO, ROM-fetch) can be compared against the SAME baseline
 * without re-measuring, and so they can refuse to run if it was never taken. */
static u32  s_ref[FCIO_PROBE_WINS];
static bool s_ref_ok = false;

int flashcartio_bus_validate(void) {
  const volatile u32* w[FCIO_PROBE_WINS];
  u32 chk[FCIO_PROBE_WINS];
  int i, last = FCIO_BUS_BAD_READ;
  unsigned short save, disp;

  if (!s_wait_captured) return FCIO_BUS_SKIPPED;  /* never boosted: nothing to check */
  if (s_locked)         return FCIO_BUS_SKIPPED;  /* already demoted, and latched    */

  save = FCIO_WAITCNT;
  fcio_windows(w);

  /* --- reference, twice, at the loader's own timing -------------------------
   * If the SLOW passes already disagree, the boost is not the problem: the image
   * or the cart is. Say so instead of demoting and pretending it is fixed. */
  FCIO_WAITCNT = s_wait_inherited;
  for (i = 0; i < FCIO_PROBE_WINS; i++) s_ref[i] = fcio_sum_ag(w[i], FCIO_PROBE_WORDS, 0);
  for (i = 0; i < FCIO_PROBE_WINS; i++) chk[i]   = fcio_sum_ag(w[i], FCIO_PROBE_WORDS, 0);
  FCIO_WAITCNT = save;
#if FCIO_PROBE_FORCE_FAIL == 6
  chk[0] ^= 1u;                        /* injector: make the two slow passes differ */
#endif
  for (i = 0; i < FCIO_PROBE_WINS; i++)
    if (s_ref[i] != chk[i]) { FCIO_WAITCNT = s_wait_inherited; s_locked = true;
                              return FCIO_BUS_UNSTABLE_SLOW; }
  s_ref_ok = true;

  /* --- the ladder. ONLY this loop may move s_rung / set s_locked. ----------- */
  disp = FCIO_DISPCNT;
  for (; s_rung < FCIO_NRUNGS; s_rung++) {
    int bad = 0;
    FCIO_WAITCNT = s_rungs[s_rung];

    for (i = 0; i < FCIO_PROBE_WINS && !bad; i++)              /* plain CPU reads */
      if (FCIO_FAULT(1, fcio_sum_ag(w[i], FCIO_PROBE_WORDS, 0)) != s_ref[i] ||
          (FCIO_FAULT_RUNG0 && s_rung == 0))
        bad = FCIO_BUS_BAD_READ;

    FCIO_DISPCNT = (unsigned short)(disp | FCIO_DCNT_BLANK);   /* VRAM scratch is
                                                                * page 1 in modes 4/5 */
    for (i = 0; i < FCIO_PROBE_WINS && !bad; i++)              /* DMA3 out of ROM */
      if (FCIO_FAULT(2, fcio_sum_dma(w[i], FCIO_PROBE_WORDS)) != s_ref[i])
        bad = FCIO_BUS_BAD_DMA;
    FCIO_DISPCNT = disp;

    if (!bad) return FCIO_BUS_OK;                              /* this rung is honest */
    last = bad;
  }

  /* Every rung lied. Give the bus back to the loader's timing, for good. */
  FCIO_WAITCNT = s_wait_inherited;
  s_locked = true;
  return last;
}

/* Pass/fail at whatever rung validate() settled on. Deliberately incapable of
 * moving the ladder: "cart GPIO is unsafe here" is a reason to stop writing cart
 * GPIO, not a reason to hand the whole session's frame rate back to the loader. */
int flashcartio_bus_probe_gpio(FlashcartioAgitator agitate) {
  const volatile u32* w[FCIO_PROBE_WINS];
  int i;
  if (!s_wait_captured || s_locked || !s_ref_ok || !agitate) return FCIO_BUS_SKIPPED;
  fcio_windows(w);
  for (i = 0; i < FCIO_PROBE_WINS; i++)
    if (FCIO_FAULT(3, fcio_sum_ag(w[i], FCIO_PROBE_WORDS, agitate)) != s_ref[i])
      return FCIO_BUS_BAD_GPIO;
  return FCIO_BUS_OK;
}

/* The instruction-fetch half of the question. Same windows, same reference, but the
 * loop is ROM-resident so opcode fetches run at the rung under test too. Verdict
 * only -- by the time this runs the ladder has settled and the tool is committed. */
int flashcartio_bus_probe_fetch(void) {
  const volatile u32* w[FCIO_PROBE_WINS];
  int i;
  if (!s_wait_captured || s_locked || !s_ref_ok) return FCIO_BUS_SKIPPED;
  fcio_windows(w);
  for (i = 0; i < FCIO_PROBE_WINS; i++)
    if (FCIO_FAULT(4, fcio_sum_romcode(w[i], FCIO_PROBE_WORDS)) != s_ref[i])
      return FCIO_BUS_BAD_FETCH;
  return FCIO_BUS_OK;
}

/* Enter/leave a card transaction. No-ops until flashcartio_bus_fast() has run,
 * so a caller that never opts in behaves exactly as before. Non-static because
 * the write path lives in flashcartio_write.c and owes the same discipline. */
unsigned short flashcartio_bus_transfer_enter(void) {
  if (!s_wait_captured) return 0;
  unsigned short cur = FCIO_WAITCNT;
  FCIO_WAITCNT = s_wait_inherited;
  return cur;
}
void flashcartio_bus_transfer_leave(unsigned short prev) {
  if (s_wait_captured) FCIO_WAITCNT = prev;
}
#define bus_transfer_enter flashcartio_bus_transfer_enter
#define bus_transfer_leave flashcartio_bus_transfer_leave

/* Cart detection + SD init is itself a card transaction — several of them — so it
 * owes the same conservative bus timing every other transfer gets. Wrapped rather
 * than bracketed inline because it has six return points. */
static bool flashcartio_activate_inner(void);

bool flashcartio_activate(void) {
  unsigned short wprev = bus_transfer_enter();
  bool ok = flashcartio_activate_inner();
  bus_transfer_leave(wprev);
  return ok;
}

static bool flashcartio_activate_inner(void) {
#if FLASHCARTIO_ED_ENABLE != 0

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
  u16 ime = REG_IME;
  REG_IME = 0;
#endif

  // Everdrive GBA X5
  if (ed_init_sd_only()) {
    ed_init();
    ed_set_save_type(FLASHCARTIO_ED_SAVE_TYPE);
    bool success = diskInit() == 0;
    ed_lock_regs();
    if (!success) {
#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      REG_IME = ime;
#endif

      return false;
    }

    active_flashcart = EVERDRIVE_GBA_X5;

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
    REG_IME = ime;
#endif

    return true;
  }
#endif

#if FLASHCARTIO_EZFO_ENABLE != 0
  // EZ Flash Omega
  if (_EZFO_startUp()) {
    active_flashcart = EZ_FLASH_OMEGA;
    return true;
  }
#endif

  return false;
}

bool flashcartio_read_sector(u32 sector, u8* destination, u16 count) {
  switch (active_flashcart) {
#if FLASHCARTIO_ED_ENABLE != 0
    case EVERDRIVE_GBA_X5: {
#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      u16 ime = REG_IME;
      REG_IME = 0;
#endif

      flashcartio_is_reading = true;
      unsigned short wprev = bus_transfer_enter();
      ed_unlock_regs();
      bool success = diskRead(sector, destination, count) == 0;
      ed_lock_regs();
      bus_transfer_leave(wprev);
      flashcartio_is_reading = false;

#if FLASHCARTIO_ED_DISABLE_IRQ != 0
      REG_IME = ime;
#endif

      return success;
    }
#endif
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA: {
      flashcartio_is_reading = true;
      unsigned short wprev = bus_transfer_enter();
      bool success = _EZFO_readSectors(sector, count, destination);
      bus_transfer_leave(wprev);
      flashcartio_is_reading = false;
      return success;
    }
#endif
    default:
      return false;
  }
}

// Reboot back into the flashcart's loader/kernel menu. Never returns. Safe on
// both carts (read-only). Refuses mid-transfer; keeps IRQs off through the reset.
void flashcartio_reboot(void) {
  if (flashcartio_is_reading) return;  // never reset mid-transfer (rule #1)
  REG_IME = 0;                         // we are not coming back; keep IRQs off
  // Give the loader back the bus timing it handed us. SoftReset does not clear
  // WAITCNT, so a boosted value would otherwise outlive this ROM and become the
  // kernel's problem.
  if (s_wait_captured) FCIO_WAITCNT = s_wait_inherited;
  switch (active_flashcart) {
#if FLASHCARTIO_EZFO_ENABLE != 0
    case EZ_FLASH_OMEGA:
      _EZFO_reboot();                  // SetRompage(BOOTLOADER) + SoftReset -> kernel
      return;
#endif
#if FLASHCARTIO_ED_ENABLE != 0
    case EVERDRIVE_GBA_X5:
      ed_unlock_regs();                // locked REG_CFG write is a no-op, so unlock first
      ed_reboot(0);                    // quick_boot=0 -> HardReset toward EverDrive OS
      return;
#endif
    default:
      break;
  }
  asm volatile("swi 0x00" ::: "memory");  // no/unknown cart: plain BIOS SoftReset
  for (;;) {}                             // unreachable
}
