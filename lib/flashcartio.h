#ifndef FLASHCARTIO_H
#define FLASHCARTIO_H

#include <stdbool.h>
#include "fatfs/ff.h"

typedef enum { NO_FLASHCART, EVERDRIVE_GBA_X5, EZ_FLASH_OMEGA } ActiveFlashcart;

extern ActiveFlashcart active_flashcart;
extern volatile bool flashcartio_is_reading;

/* Chunk-level read retries since boot, and reads that exhausted their retries. The driver
 * cannot log from inside a transfer (log.c lives in the ROM this cart has unmapped), so it
 * counts instead and the caller reports afterwards: a save-open that took a long time with
 * retries>0 is a cart/bus problem, one with retries==0 is not. */
extern volatile unsigned long flashcartio_read_retries;
extern volatile unsigned long flashcartio_read_failures;

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

/* ---- fast-bus self-test ---------------------------------------------------
 *
 * flashcartio_bus_fast() is a bet: that this cart, in this console, answers ROM
 * reads correctly at 3/1 + prefetch. That bet has no retail precedent for a
 * flashcart's FPGA-fronted PSRAM, no emulator can test it, and when it loses the
 * symptom is a hang somewhere far from the cause.
 *
 * These functions settle it by measurement, at boot. They checksum three 4 KiB
 * windows of this very image at the loader's own timing -- the only timing this
 * project has hardware proof for -- and then again at the boosted timing.
 *
 * WHAT EACH ONE CAN AND CANNOT SEE. Read this before quoting a verdict:
 *
 *   flashcartio_bus_validate()      CPU reads + a DMA3 burst straight out of ROM,
 *                                   with the probe loop itself running from IWRAM.
 *                                   It therefore tests ROM *data* reads ONLY. The
 *                                   probe code is in IWRAM on purpose (a bad
 *                                   instruction fetch must not take down the code
 *                                   that exists to detect one), and the price of
 *                                   that is that GamePak *prefetch* -- WAITCNT bit
 *                                   14, which only engages for opcode fetches out
 *                                   of the gamepak -- is untestable here BY
 *                                   CONSTRUCTION. This is the ONLY function that
 *                                   moves the rung ladder or latches a demotion.
 *
 *   flashcartio_bus_probe_fetch()   The missing half: the same checksum, but from
 *                                   a copy of the loop deliberately left in .text,
 *                                   so the instruction fetches AND the data reads
 *                                   both run at the rung under test. Run it LAST,
 *                                   after validate() has proven data integrity,
 *                                   and flush your log FIRST -- if the fetch path
 *                                   is what is broken, this is where it hangs, and
 *                                   a hang here is a diagnosis rather than a
 *                                   mystery. Verdict only; it never demotes.
 *
 *   flashcartio_bus_probe_gpio(ag)  The same read burst with `ag` toggling the
 *                                   cart's GPIO between reads -- the FPGA/ROM
 *                                   interleave that has already been observed to
 *                                   corrupt ROM reads on the EZ-Flash Omega DE.
 *                                   Verdict only; it NEVER moves the ladder,
 *                                   because "rumble is unsafe here" must not cost
 *                                   the whole session its frame rate. NOTE the
 *                                   agitation is far denser than any real workload
 *                                   -- it is a stress test, not a simulation.
 *
 * A pass says the cart returned CONSISTENT bytes. It does NOT say the bytes are
 * the ones the linker produced -- nothing here reads a reference from outside the
 * possibly-bad image. That question belongs to the ROM self-check (pdna_romver).
 *
 * BOOT ONLY: the DMA pass copies into VRAM 0x06013000, which is scratch in a
 * bitmap mode (past the Mode-3 framebuffer, below bitmap OBJ tiles) but is OBJ
 * tile storage in tile modes. The probe forces DCNT_BLANK for the duration and
 * restores DISPCNT, so it is safe in Mode 4/5 page 1 too -- but never call it once
 * a tile-mode BG is live.
 */
typedef void (*FlashcartioAgitator)(int on);

#define FCIO_BUS_OK              0   /* boosted timing reads the image correctly   */
#define FCIO_BUS_SKIPPED        (-1) /* never boosted, already demoted, or no ref  */
#define FCIO_BUS_BAD_READ        1   /* CPU reads differ from the reference        */
#define FCIO_BUS_BAD_DMA         2   /* DMA3-from-ROM differs from the reference   */
#define FCIO_BUS_BAD_GPIO        3   /* differs only while cart GPIO is written    */
#define FCIO_BUS_UNSTABLE_SLOW   4   /* two reads at the LOADER's timing disagree:
                                      * not a boost problem -- bad image or cart   */
#define FCIO_BUS_BAD_FETCH       5   /* the ROM-resident (code+data) pass differs  */

int flashcartio_bus_validate(void);
int flashcartio_bus_probe_gpio(FlashcartioAgitator agitate);
int flashcartio_bus_probe_fetch(void);

/* Which rung is live: 0 = 3/1+prefetch, 1 = 3/2+prefetch, 2 = the loader's timing
 * (demoted and latched). Log it; it is the number that explains a slow session. */
int flashcartio_bus_rung(void);

/* Capture the loader's timing but never boost, and latch that. For a user override
 * ("run this session at the loader's timing") -- keeps the transfer bracketing
 * bookkeeping consistent, which simply not calling flashcartio_bus_fast() does not. */
void flashcartio_bus_hold(void);

#endif  // FLASHCARTIO_H
