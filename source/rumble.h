#ifndef RUMBLE_H
#define RUMBLE_H

#include <stdbool.h>

/*
 * Minimal GBA rumble driver (EZ-Flash Omega DE + generic rumble carts).
 *
 * The motor is an ERM (pager motor) switched by GPIO bit 3 of the gamepak port —
 * the WarioWare Twisted / Drill Dozer "MOT" line, the SAME port as the cartridge
 * RTC (RTC = bits 0-2, rumble = bit 3): data 0x080000C4, dir 0x080000C6. It is
 * TOGGLE-driven, not level-driven (a static high does nothing on the Omega DE), so
 * "strength" is faked with software PWM duty from a TIMER2 IRQ; amplitude and
 * frequency are physically coupled (ERM), so there is no independent frequency.
 *
 * Validated on Guy's GBA SP + EZ-Flash Omega DE (the rumble-lab test bench). Two
 * gotchas that only hardware reveals: (1) the motor is toggle-driven, so never a
 * 100% static duty; (2) the Omega only routes GPIO bit3 to the motor when RTC is
 * ENABLED for the ROM in the EZ-Flash settings. Not emulated.
 *
 * OS-mode: the motor is a cart-bus write, so it must NOT run during an SD transfer
 * (the ROM/cart window is unmapped mid-write) — call rumble_pause() before any
 * FatFs write and rumble_resume() after.  Distilled from projects/rumble-lab; see
 * the learn skill's references/rumble-haptics.md for the full hardware notes.
 */

void rumble_init(void);            /* motor off; detect cart. Call once at boot.   */
bool rumble_omega(void);           /* true if the active cart is an EZ-Flash Omega */

/* TIMER2-IRQ PWM: carrier `freq_hz` (1..RUMBLE_FREQ_MAX), duty 0..255. */
#define RUMBLE_FREQ_MAX 500
void rumble_pwm_start(int freq_hz, int duty_0_255);
void rumble_pwm_set(int freq_hz, int duty_0_255);
void rumble_pwm_stop(void);
bool rumble_pwm_active(void);
void rumble_raw_off(void);         /* force the motor off right now                */

/* SD-transfer safety: freeze the motor + the timer IRQ around FatFs writes. */
void rumble_pause(void);
void rumble_resume(void);

#endif /* RUMBLE_H */
