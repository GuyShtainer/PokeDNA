#ifndef FLASHCARTIO_H
#define FLASHCARTIO_H

#include <stdbool.h>
#include "fatfs/ff.h"

typedef enum { NO_FLASHCART, EVERDRIVE_GBA_X5, EZ_FLASH_OMEGA } ActiveFlashcart;

extern ActiveFlashcart active_flashcart;
extern volatile bool flashcartio_is_reading;

bool flashcartio_activate(void);
bool flashcartio_read_sector(unsigned int sector,
                             unsigned char* destination,
                             unsigned short count);

/* Reboot back into the flashcart loader/kernel menu. Never returns. Refuses
 * while an SD transfer is in flight; keeps IRQs off through the reset. */
void flashcartio_reboot(void);

/* ---- game-pak bus timing (REG_WAITCNT) -----------------------------------
 *
 * Both flashcart loaders hand off with REG_WAITCNT = 0, which is the SLOWEST
 * setting the hardware has: ROM waitstates 4/2 and the instruction prefetch
 * buffer OFF. Nothing in libtonc's crt0 changes it (libgba's does, which is why
 * this is easy to miss), so a tonc tool runs its entire ROM-resident code at
 * that rate. Measured on Guy's EZ-Flash Omega DE: `waitcnt=0000` in
 * /PokeDNA/log.txt, and the same value in mGBA.
 *
 * flashcartio_bus_fast() switches to 3/1 with prefetch enabled — the value
 * commercial GBA carts use, which is the timing every flashcart is built to
 * serve. Call it once at startup.
 *
 * The SD transfer paths do NOT run at that timing: flashcartio_read_sector()
 * and the write path drop back to the inherited conservative value for the
 * duration of each transfer and restore afterwards. The cart's SD interface is
 * an FPGA answering reads in ROM address space, and its timing margin is not
 * something this project has characterised — so the fast setting is used for
 * the 99.9% of the time we are executing code, never while clocking the card.
 *
 * SRAM waitstates (bits 0-1) are left at the conservative 8 cycles, which is
 * what 0x4317 encodes and what flashsave.c's bus_setup() also asks for.
 *
 * Cart GPIO (0x080000C4-C8: the RTC bit-bang in gba_rtc.c and the rumble motor)
 * is deliberately NOT bracketed. Retail Ruby/Sapphire/Emerald drive the very same
 * S-3511A over the very same GPIO while running at 3/1 with prefetch on, so that
 * timing is proven by the games themselves. The card's SD interface is the only
 * part of the bus this project has no retail precedent for.
 */
void flashcartio_bus_fast(void);

/* Bracket one card transaction at the inherited (conservative) timing. Both
 * return/expect the WAITCNT value to restore. No-ops until flashcartio_bus_fast()
 * has been called, so a tool that never opts in is bit-for-bit unaffected. */
unsigned short flashcartio_bus_transfer_enter(void);
void           flashcartio_bus_transfer_leave(unsigned short prev);

/* The WAITCNT the loader handed us, captured by flashcartio_bus_fast(). Worth
 * logging: it is the one number that says which loader/cart timing we started
 * from, and it is how the 4/2-no-prefetch handoff was found in the first place. */
unsigned short flashcartio_bus_inherited(void);

#endif  // FLASHCARTIO_H
