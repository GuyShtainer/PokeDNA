/*
 * Rumble driver — see rumble.h for the hardware notes. Trimmed from the rumble-lab
 * test bench to the one path that drives a native-homebrew motor: GPIO bit 3 (the
 * EZ-Flash kernel's own 0x09E20000/0x08001000 sequence is its GBC-emulator rumble
 * and does NOT move the motor from a native GBA ROM — confirmed on hardware).
 *
 * tonc-only includes (no sys.h: its u8/u16 macros collide with libtonc typedefs).
 * The PWM ISR lives in IWRAM for speed (it runs TICK_HZ times a second).
 */
#include <tonc.h>
#include "rumble.h"
#include "flashcartio.h"   /* active_flashcart, EZ_FLASH_OMEGA */

#define GPIO_DATA  (*(vu16*)0x080000C4)
#define GPIO_DIR   (*(vu16*)0x080000C6)
#define GPIO_CTRL  (*(vu16*)0x080000C8)
#define RUMBLE_BIT 0x0008u           /* GPIO bit 3 ("MOT") */

#define TICK_HZ 8192                 /* PWM update IRQ rate */

static bool          s_omega  = false;
static volatile bool s_pwm    = false;   /* PWM mode running          */
static volatile bool s_paused = false;   /* true around SD transfers  */
static volatile u8   s_motor  = 0;       /* last commanded motor state */

/* PWM state, all touched by the ISR. */
static volatile u16 s_period = 256;      /* ticks per carrier period      */
static volatile u16 s_on     = 128;      /* ticks motor is on per period  */
static volatile u16 s_ctr    = 0;

IWRAM_CODE static void motor_set(int on) {
  GPIO_DATA = on ? RUMBLE_BIT : 0;
  s_motor = (u8)(on != 0);
}

IWRAM_CODE static void pwm_isr(void) {
  if (s_paused) return;
  u16 c = (u16)(s_ctr + 1);
  if (c >= s_period) { c = 0; motor_set(s_on != 0); }   /* period start */
  else if (c == s_on) { motor_set(0); }                 /* duty edge    */
  s_ctr = c;
}

void rumble_init(void) {
  s_omega = (active_flashcart == EZ_FLASH_OMEGA);
  GPIO_CTRL = 1;            /* allow GPIO read/write           */
  GPIO_DIR  = RUMBLE_BIT;   /* bit3 output, RTC bits as input  */
  GPIO_DATA = 0;           /* motor off                       */
  s_pwm = false; s_paused = false; s_motor = 0;
}

bool rumble_omega(void)      { return s_omega; }
bool rumble_pwm_active(void) { return s_pwm; }

void rumble_raw_off(void) { GPIO_DATA = 0; s_motor = 0; }

void rumble_pwm_set(int freq_hz, int duty) {
  if (freq_hz < 1) freq_hz = 1;
  if (freq_hz > RUMBLE_FREQ_MAX) freq_hz = RUMBLE_FREQ_MAX;
  if (duty < 0) duty = 0;
  if (duty > 255) duty = 255;
  u32 period = (u32)TICK_HZ / (u32)freq_hz;
  if (period < 2) period = 2;
  if (period > 0xFFFF) period = 0xFFFF;
  u32 on = period * (u32)duty / 255u;
  /* HW reality (Omega DE): the motor rumbles on a TOGGLED line, not a static level
   * — so never let duty be a full 100% hold. Keep at least one tick off per period
   * so every period produces a refresh edge. */
  if (on >= period) on = period - 1;
  /* update the ISR's shared state atomically */
  u16 ime = REG_IME; REG_IME = 0;
  s_period = (u16)period;
  s_on     = (u16)on;
  if (s_ctr >= s_period) s_ctr = 0;
  REG_IME = ime;
}

void rumble_pwm_start(int freq_hz, int duty) {
  rumble_pwm_set(freq_hz, duty);
  GPIO_DIR = RUMBLE_BIT;
  s_ctr = 0; s_pwm = true;
  irq_add(II_TIMER2, pwm_isr);
  REG_TM2D   = (u16)(0x10000u - (16777216u / TICK_HZ));   /* F/1, TICK_HZ overflow */
  REG_TM2CNT = TM_ENABLE | TM_IRQ;
}

void rumble_pwm_stop(void) {
  REG_TM2CNT = 0;
  irq_disable(II_TIMER2);
  s_pwm = false;
  rumble_raw_off();
}

void rumble_pause(void) {
  s_paused = true;
  if (s_pwm) REG_TM2CNT = 0;        /* freeze the timer; cart writes must stop */
  rumble_raw_off();
}

void rumble_resume(void) {
  s_paused = false;
  if (s_pwm) {
    GPIO_DIR = RUMBLE_BIT;
    s_ctr = 0;
    REG_TM2D   = (u16)(0x10000u - (16777216u / TICK_HZ));
    REG_TM2CNT = TM_ENABLE | TM_IRQ;
  }
}
