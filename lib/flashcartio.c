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

/* ---- game-pak bus timing (see flashcartio.h for the why) ----------------- */

#define FCIO_WAITCNT (*(volatile unsigned short*)0x04000204)

/* WS0 3/1 + prefetch + SRAM 8: the timing a commercial GBA cartridge asks for. */
#define FCIO_WAIT_FAST 0x4317u

/* Whatever the flashcart loader handed us — restored around every SD transfer.
 * Captured before the first change so we give the card back exactly the timing
 * it was working at, rather than a value this file invented. */
static unsigned short s_wait_inherited = 0;
static bool           s_wait_captured  = false;

void flashcartio_bus_fast(void) {
  if (!s_wait_captured) { s_wait_inherited = FCIO_WAITCNT; s_wait_captured = true; }
  FCIO_WAITCNT = FCIO_WAIT_FAST;
}

unsigned short flashcartio_bus_inherited(void) { return s_wait_inherited; }

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
