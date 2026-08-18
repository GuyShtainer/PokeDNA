#ifndef PDNA_ROMCHECK_H
#define PDNA_ROMCHECK_H

#include <stdbool.h>

/* Sampled high-ROM self-verification: read ~64 KiB of this ROM back over the cartridge
 * bus in 17 windows and compare against the CRC32s stamped into the image at build time
 * by tools/stamp_rom_windows.py. See source/pdna_romver.h for the sampling rationale and
 * for an honest statement of what an OK verdict does and does not prove. */

/* Run the check and LOG the verdict. Call ONCE at boot, after log_init() and BEFORE
 * flashcartio_activate(): it touches only the ROM bus, never the card, and running it
 * before any cart activity is what proves no SD transfer can be in flight (hard rule 1).
 * Its log lines ride out on the existing post-mount flush — it writes nothing itself.
 *
 * Unconditional on purpose: the log line is useful on every cart, including an EverDrive.
 * Only the CONSUMING half below is gated. */
void pdna_romcheck_boot(void);

/* Show the warning screen if the boot check failed, else do nothing. Call after the SD is
 * mounted and the log has been flushed, so the evidence is already on the card before the
 * user is asked to press A.
 *
 * Does nothing on an EverDrive GBA X5: its loader has not been analysed for image
 * patching the way the Omega DE kernel has, so a mismatch there could be the loader
 * rather than the file — and the remedy the panel offers ("delete the /PATCH cache file, re-copy")
 * is Omega-specific and would not help. The verdict is still in the log there. */
void pdna_romcheck_report(void);

/* True only when the image is STABLY not what we shipped (PDNA_RV_BAD_IMAGE) and the cart
 * is one whose loader we understand. Gates every write path: an image we cannot trust must
 * never write a user's save (hard rule 3).
 *
 * Deliberately FALSE for PDNA_RV_BAD_BUS. An unstable cart bus is answered by the timing
 * ladder, not by permanently amputating Guy's write features on a cart that may be fine —
 * and every other ROM-read self-verify in this codebase (icopy_verified, box_oam.c:110)
 * already fails soft on a retry-recovered read for exactly that reason. */
bool pdna_romcheck_bad(void);

#endif /* PDNA_ROMCHECK_H */
