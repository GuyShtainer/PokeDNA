#include "snd.h"
#include <tonc.h>

/* ---- GBA PSG beeps -------------------------------------------------------
 * Channel 1 (square) for tones, channel 4 (noise) for buzzes. Each note uses a
 * decreasing volume envelope so it silences itself; we never have to stop it.
 *
 * Square frequency:  rate = 2048 - 131072/Hz   (written to SOUND1CNT_X bits0-10)
 * SOUND1CNT_H: bits12-15 init volume, bit11 env dir (0=down), bits8-10 env step,
 *              bits6-7 duty (2 = 50%).
 * SOUND1CNT_X: bits0-10 freq rate, bit15 = restart/trigger.                    */

static bool s_on = true;

/* a tiny jingle queue: up to 4 pending (delay_frames, hz, vol, env) notes that
 * snd_vblank() fires in sequence, so "save" can be a two-note rise. */
typedef struct { u8 wait; u16 hz; u8 vol; u8 env; } Note;
static Note s_q[4];
static int  s_qn = 0, s_qi = 0;
static u8   s_delay = 0;

static inline u16 hz_to_rate(u16 hz) {
    u32 d = 131072u / (hz ? hz : 1);
    return (u16)((d >= 2048) ? 0 : (2048 - d)) & 0x07FF;
}

static void sq(u16 hz, u8 vol, u8 env, u8 duty) {
    if (!s_on) return;
    REG_SND1SWEEP = 0;                       /* no sweep (no pitch drift)        */
    REG_SND1CNT   = ((u16)vol << 12) | ((u16)env << 8) | ((u16)duty << 6);
    REG_SND1FREQ  = 0x8000 | hz_to_rate(hz); /* trigger                          */
}

static void noise(u8 vol, u8 env, u16 freqdiv) {
    if (!s_on) return;
    REG_SND4CNT  = ((u16)vol << 12) | ((u16)env << 8);
    REG_SND4FREQ = 0x8000 | (freqdiv & 0x40FF);
}

void snd_init(void) {
    REG_SNDSTAT  = 0x0080;   /* master enable (bit7)                            */
    /* SOUNDCNT_L: lo byte = L/R master volume (7 each); hi byte = per-channel
     * L/R enables (0xFF = all 4 channels to both speakers).                    */
    REG_SNDDMGCNT = 0xFF77;
    REG_SNDDSCNT  = 0x0002;  /* DMG mix volume = 100%                           */
    s_qn = s_qi = s_delay = 0;
}

void snd_set_enabled(bool on) { s_on = on; if (!on) { s_qn = s_qi = 0; } }
bool snd_is_enabled(void)     { return s_on; }

/* ---- effects (kept short + fairly quiet; cursor move is the quietest) ---- */
void snd_move(void) { sq(1760, 5, 1, 2); }              /* high, faint, fast decay */
void snd_ok(void)   { sq(1320, 9, 3, 2); }
void snd_back(void) { sq( 660, 7, 3, 1); }
void snd_tab(void)  { sq( 990, 7, 2, 2); }
void snd_edit(void) { sq(1480, 8, 2, 2); }
void snd_deny(void) { noise(9, 4, 0x0006); }            /* low gritty buzz         */

static void queue(u8 wait, u16 hz, u8 vol, u8 env) {
    if (s_qn >= 4) return;
    s_q[s_qn].wait = wait; s_q[s_qn].hz = hz; s_q[s_qn].vol = vol; s_q[s_qn].env = env; s_qn++;
}

void snd_save(void) {                                   /* rising two-note "ta-da" */
    if (!s_on) return;
    s_qn = s_qi = 0;
    sq(1047, 9, 3, 2);                                  /* C6 now                  */
    queue(7, 1568, 10, 4);                              /* G6 a few frames later   */
    s_delay = 0;
}

void snd_error(void) {                                  /* descending two-note buzz */
    if (!s_on) return;
    s_qn = s_qi = 0;
    sq(392, 9, 4, 1);
    queue(8, 262, 9, 5);
    s_delay = 0;
}

void snd_boot(void) {                                   /* soft rising welcome      */
    if (!s_on) return;
    s_qn = s_qi = 0;
    sq(784, 7, 3, 2);                                   /* G5                       */
    queue(6, 1175, 8, 4);                               /* D6                       */
    s_delay = 0;
}

void snd_vblank(void) {
    if (s_qi >= s_qn) return;
    if (s_delay) { s_delay--; return; }
    sq(s_q[s_qi].hz, s_q[s_qi].vol, s_q[s_qi].env, 2);
    s_delay = s_q[s_qi].wait;
    s_qi++;
    if (s_qi >= s_qn) s_qn = s_qi = 0;
}
