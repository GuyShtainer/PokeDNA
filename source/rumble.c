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

/* ---- cart-bus bracketing ---------------------------------------------------
 * Cart GPIO lives in the gamepak address window, so a GPIO write is a cart-bus
 * write: it is exposed to whatever ROM timing the app happens to be running at.
 * On the EZ-Flash Omega DE that window is FPGA-emulated on the same bus that
 * serves ROM, and this project has already caught it corrupting in-flight ROM
 * reads at the loader's own 4/2 timing (the garbled-wallpaper bug -- see the
 * render guard below). Once the app boosts the bus to 3/1 + prefetch the margin is
 * strictly tighter, and the render guard cannot cover instruction fetch.
 *
 * So every PHYSICAL GPIO write drops the bus back to the timing the loader handed
 * us for the duration of the store, and restores it. Cost: two extra I/O-register
 * stores per motor edge. The carrier is 120 Hz (rmbl.c R_FREQ) and motor_set runs
 * twice per period, so that is ~240 edges/s -- the 8192 Hz ISR returns without
 * touching the cart on ~97% of its ticks -- i.e. under 0.01% of the CPU. Safe
 * against ISR/main-line interleave: each site saves and restores whatever it
 * found, so a nested save/restore is a no-op.
 *
 * This does NOT claim to make GPIO/ROM interleave safe. It restores it to the
 * timing regime that shipped and was hardware-validated for months. Whether even
 * that is safe on a given unit is what the bus self-test's GPIO pass measures --
 * which is why there are TWO probe entry points below, raw and bracketed. */
static u16 s_bus_safe = 0;               /* the loader's WAITCNT, cached at init */

#define CART_W(reg, val) do {            \
    u16 w_ = REG_WAITCNT;                \
    REG_WAITCNT = s_bus_safe;            \
    (reg) = (val);                       \
    REG_WAITCNT = w_;                    \
  } while (0)

static bool          s_omega  = false;
static volatile bool s_pwm    = false;   /* PWM mode running          */
static volatile bool s_paused = false;   /* true around SD transfers  */
static volatile u8   s_motor  = 0;       /* last commanded motor state */
static volatile int  s_io_depth = 0;     /* >0: cart-bus GPIO writes frozen for a ROM-read blit (render guard) */

/* PWM state, all touched by the ISR. */
static volatile u16 s_period = 256;      /* ticks per carrier period      */
static volatile u16 s_on     = 128;      /* ticks motor is on per period  */
static volatile u16 s_ctr    = 0;

IWRAM_CODE static void motor_set(int on) {
  s_motor = (u8)(on != 0);
  if (s_io_depth) return;                 /* bus frozen for a ROM-read blit: don't toggle the cart */
  CART_W(GPIO_DATA, on ? RUMBLE_BIT : 0);
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
  /* Cache the loader's timing BEFORE the first cart write. If the app never called
   * flashcartio_bus_fast() this reads 0, which is the slowest setting the hardware
   * has -- conservative in the right direction -- and CART_W restores whatever was
   * actually live afterwards, so it is safe either way. */
  s_bus_safe = flashcartio_bus_inherited();
  CART_W(GPIO_CTRL, 1);            /* allow GPIO read/write           */
  CART_W(GPIO_DIR,  RUMBLE_BIT);   /* bit3 output, RTC bits as input  */
  CART_W(GPIO_DATA, 0);            /* motor off                       */
  s_pwm = false; s_paused = false; s_motor = 0;
}

/* ---- bus self-test hooks ---------------------------------------------------
 * TWO forms, because one alone cannot be interpreted. The bracketed form is what
 * actually ships, so it answers "is the mitigation safe here?"; the raw form
 * writes GPIO exactly as the code did before the bracket existed and touches
 * WAITCNT not at all, so it answers the ORIGINAL question -- "does cart GPIO
 * corrupt ROM reads on this unit at the boosted timing?" -- without the confound
 * of the bracket's own WAITCNT thrash. Running only the bracketed one would make
 * a failure unattributable: the bracket writes WAITCNT twice per edge, and writing
 * WAITCNT from a preempting ISR has no retail precedent either.
 *
 * Both deliberately bypass s_io_depth / s_paused (reproducing the hazard is the
 * whole point) and are in IWRAM so the interleave under test is data-vs-GPIO,
 * exactly like the real PWM ISR. Call them from the bus self-test and nowhere
 * else. NOTE: the probe toggles far faster than the ~240 edges/s that ship -- it
 * is a stress test, not a simulation. */
IWRAM_CODE void rumble_bus_probe_raw(int on)       { GPIO_DATA = on ? RUMBLE_BIT : 0; }
IWRAM_CODE void rumble_bus_probe_bracketed(int on) { CART_W(GPIO_DATA, on ? RUMBLE_BIT : 0); }

bool rumble_omega(void)      { return s_omega; }
bool rumble_pwm_active(void) { return s_pwm; }

void rumble_raw_off(void) { s_motor = 0; if (s_io_depth) return; CART_W(GPIO_DATA, 0); }

/* Render guard: while a long software blit reads pixel data from ROM, a rumble GPIO
 * write to the cart bus (0x080000C4) can corrupt those in-flight ROM reads on the
 * EZ-Flash Omega DE (its RTC/GPIO window is emulated on the same gamepak bus that
 * serves ROM) — the cause of the garbled box wallpaper. These freeze the *physical*
 * GPIO writes (motor_set/rumble_raw_off no-op while depth>0) for the duration of the
 * blit; the PWM timer/ISR keep running so the cue's phase is preserved and the haptic
 * resumes seamlessly afterwards. Nesting-counted (blit primitives nest). DISTINCT from
 * rumble_pause() (the SD-write guard) — do not share the s_paused flag. */
void rumble_io_suspend(void) { s_io_depth++; }
void rumble_io_resume(void)  { if (s_io_depth > 0 && --s_io_depth == 0 && !s_pwm) { CART_W(GPIO_DATA, 0); s_motor = 0; } }

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
  CART_W(GPIO_DIR, RUMBLE_BIT);
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
    CART_W(GPIO_DIR, RUMBLE_BIT);
    s_ctr = 0;
    REG_TM2D   = (u16)(0x10000u - (16777216u / TICK_HZ));
    REG_TM2CNT = TM_ENABLE | TM_IRQ;
  }
}
