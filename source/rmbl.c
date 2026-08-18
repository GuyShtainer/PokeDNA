#include "rmbl.h"
#include "rumble.h"

/*
 * A cue is a short list of {duty, frames} steps (duty 0 = a silent gap); the list
 * ends with a frames==0 sentinel. Strength comes from the PWM duty; the carrier is
 * fixed. rmbl_vblank() advances one step per frame, mirroring the rumble-lab haptic
 * pattern player but with graded duty instead of full on/off.
 *
 * The duties below are a starting point — the dead-zone/saturation band of an ERM is
 * unit-dependent, so tune D_WEAK/D_MED/D_STR by feel on hardware.
 */
typedef struct { unsigned char duty; unsigned char frames; } RStep;

#define R_FREQ  120           /* carrier Hz (rumble-lab's validated sustained-on freq) */
#define D_WEAK  110
#define D_MED   175
#define D_STR   240

static const RStep SEQ_SCROLL[] = { {D_WEAK,  3}, {0, 0} };               /* quick weak tick   */
static const RStep SEQ_ROOM  [] = { {D_MED,   8}, {0, 0} };               /* medium thunk      */
static const RStep SEQ_EDIT  [] = { {D_MED,   5}, {0, 0} };               /* short medium      */
static const RStep SEQ_SAVE  [] = { {D_STR,  16}, {0, 0} };               /* strong confirm    */
static const RStep SEQ_ERROR [] = { {D_STR,   4}, {0, 5}, {D_STR, 4}, {0, 0} }; /* ba-bump heartbeat */

static const RStep* const SEQ[RCUE_COUNT] = {
  SEQ_SCROLL, SEQ_ROOM, SEQ_EDIT, SEQ_SAVE, SEQ_ERROR
};
static const char* const RCUE_NAME[RCUE_COUNT] = {
  "Scroll", "Room change", "Stat edit", "Save done", "Error"
};

#define MASK_ALL ((1u << RCUE_COUNT) - 1u)

/* Global strength + duration scaling (1..5), so cues are tunable on hardware where
 * an ERM's dead-zone/saturation band is unit-specific. Strength scales the PWM duty
 * (toward the motor's max); duration scales every step's frame count. Defaults are
 * punchy (the motor can otherwise read as "nothing happened" on a weak unit). */
static const int STR_PCT[5] = {  40,  70, 100, 140, 190 };   /* of base duty */
static const int DUR_PCT[5] = {  50,  75, 100, 150, 220 };   /* of base frames */

static unsigned     s_mask    = MASK_ALL;   /* all cues on by default      */
static int          s_str     = 5;          /* 1..5 strength (duty scale)   */
static int          s_dur     = 4;          /* 1..5 duration (frame scale)  */
static bool         s_paused  = false;
static const RStep* s_seq     = 0;          /* active pattern, or NULL      */
static int          s_idx     = 0;
static int          s_left    = 0;          /* frames left on current step  */
static bool         s_running = false;      /* PWM timer currently armed    */

static int scale_duty(int duty)   { if (!duty) return 0; int d = duty * STR_PCT[s_str - 1] / 100; return d > 255 ? 255 : (d < 1 ? 1 : d); }
static int scale_frames(int fr)   { int f = fr * DUR_PCT[s_dur - 1] / 100; return f < 1 ? 1 : f; }

static void cue_end(void) {
  if (s_running) { rumble_pwm_stop(); s_running = false; }
  s_seq = 0; s_idx = 0; s_left = 0;
}

void rmbl_init(void) {
  rumble_init();
  s_seq = 0; s_idx = 0; s_left = 0; s_running = false; s_paused = false;
}

/* Arm a pattern (used by rmbl_fire after the toggle check, and by rmbl_demo which
 * bypasses the per-cue mask so Settings previews always play). */
/* Hardware lockout, set only by a MEASURED failure of the bus self-test's GPIO
 * pass: cart-GPIO writes corrupt ROM reads on this unit even in their bracketed
 * (shipping) form. One-way for the session, never persisted -- the next boot
 * re-measures, because a marginal bus is thermal/voltage dependent and a sticky
 * flag in config.cfg would silently rob a good unit forever. */
static bool s_hw_lock = false;

void rmbl_lockout(void) { s_hw_lock = true; cue_end(); rumble_raw_off(); }
bool rmbl_locked(void)  { return s_hw_lock; }

static void fire_seq(const RStep* seq) {
  if (s_hw_lock || s_paused || !seq) return;
  s_seq = seq; s_idx = 0; s_left = 0;             /* replace any current cue */
  if (!s_running) { rumble_pwm_start(R_FREQ, 0); s_running = true; }
}

void rmbl_fire(int cue) {
  if (cue < 0 || cue >= RCUE_COUNT) return;
  if (!((s_mask >> cue) & 1u)) return;            /* this cue is toggled off */
  fire_seq(SEQ[cue]);
}

void rmbl_demo(void) { fire_seq(SEQ_SAVE); }      /* Settings strength/duration preview */

void rmbl_vblank(void) {
  if (!s_seq) return;
  if (s_left == 0) {                              /* time to (re)issue a step */
    RStep st = s_seq[s_idx];
    if (st.frames == 0) { cue_end(); return; }    /* end of pattern */
    rumble_pwm_set(R_FREQ, scale_duty(st.duty));  /* duty 0 = a silent gap   */
    s_left = scale_frames(st.frames); s_idx++;
  }
  s_left--;
}

void rmbl_pause(void)  { cue_end(); s_paused = true; rumble_pause(); }
void rmbl_resume(void) { s_paused = false; rumble_resume(); }

bool rmbl_cue_enabled(int cue) {
  return cue >= 0 && cue < RCUE_COUNT && ((s_mask >> cue) & 1u);
}

void rmbl_cue_set(int cue, bool on) {
  if (cue < 0 || cue >= RCUE_COUNT) return;
  if (on) s_mask |= (1u << cue);
  else  { s_mask &= ~(1u << cue); cue_end(); }    /* kill it now if it's playing */
}

unsigned    rmbl_get_mask(void)          { return s_mask & MASK_ALL; }
void        rmbl_set_mask(unsigned mask) { s_mask = mask & MASK_ALL; }
const char* rmbl_cue_name(int cue)       { return (cue >= 0 && cue < RCUE_COUNT) ? RCUE_NAME[cue] : ""; }

int  rmbl_get_strength(void)      { return s_str; }
void rmbl_set_strength(int level) { s_str = level < 1 ? 1 : (level > 5 ? 5 : level); }
int  rmbl_get_duration(void)      { return s_dur; }
void rmbl_set_duration(int level) { s_dur = level < 1 ? 1 : (level > 5 ? 5 : level); }
