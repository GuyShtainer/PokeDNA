#ifndef PDNA_ROMFULL_H
#define PDNA_ROMFULL_H

/*
 * pdna_romfull.h — the FULL-IMAGE verifier screen: CRC32 every byte of this ROM over the
 * cartridge bus and paint one cell per region, so a partial SD load stops being a theory.
 *
 * WHY THIS EXISTS, IN ONE PARAGRAPH
 * ---------------------------------
 * The boot self-check samples 17 x 4 KiB = 66 KiB of a 12.5 MB image: 0.527%. It has said
 * "rom self-check: OK (17 windows)" on a DS Lite that then froze, and that OK proves
 * nothing about the other 99.47%. Meanwhile the 530 KB artless build opens the same save
 * on the same card fine, and the Omega DE kernel has a documented mechanism for loading a
 * large fragmented file incompletely (projects/rom-load-lab: Check_game_RTS_FAT emits two
 * words per FAT discontinuity into a 1 KiB buffer whose control words live at offset
 * 0x1F0, with no bounds check). This screen is how that theory gets proved or killed on
 * Guy's hardware, with no PC, no card writes, and a verdict a phone photo can capture.
 *
 * WHAT IT COSTS AND WHAT IT TOUCHES
 * ---------------------------------
 * Reads ROM only — no SD, no SRAM, no card writes, nothing persisted. It freezes the
 * rumble motor for the duration (a cart-GPIO write can corrupt an in-flight ROM read on
 * this cart — source/rumble.c:59-66), borrows TIMER0/TIMER1 to measure itself, and
 * restores both. Zero new EWRAM and zero new .bss in either RAM: the per-region state is
 * a stack local, which is also why it must stay a modal screen.
 *
 * TWO WAYS IN, and the first one is the important one
 * ---------------------------------------------------
 *   HOLD R + SELECT AT BOOT — runs before flashcart detection and before any save is
 *     opened, so it works on the exact failure it exists to diagnose: a tool that hangs
 *     on save-open, or one that never reaches a menu at all. Does NOT collide with the
 *     L+SELECT slow-bus override (pdna_main.c:302); holding all three gives you both.
 *   FILE MENU (START in the browser) -> "Verify ROM image..." — the same screen once the
 *     tool is up, on whatever bus timing the session settled at.
 */

/* Run the verifier modally. Returns when the user leaves it.
 *
 * Safe to call before flashcartio_activate() and before rmbl_init(): it gates its own
 * motor freeze on rumble_omega(), so nothing writes cart GPIO on a cart that has not
 * been detected yet. Logs a 2-4 line summary through log_line() but never flushes — the
 * caller owns the card. */
void pdna_romfull_screen(void);

#endif /* PDNA_ROMFULL_H */
