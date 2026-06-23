#ifndef RMBL_H
#define RMBL_H

#include <stdbool.h>

/*
 * PokeDNA haptic-cue layer over the rumble driver (rumble.h).
 *
 * Five independent UI cues, each with its own on/off toggle in Settings (persisted
 * in /PokeDNA/config.cfg) — exactly like the per-place animation toggles. The motor
 * is toggle-driven, so a cue's "strength" is a PWM duty (weak/medium/strong); the
 * error cue is a strong double "heartbeat". One cue plays at a time; a new fire
 * replaces the current one.
 *
 * Omega-only in practice and needs RTC enabled for the ROM in the EZ-Flash settings;
 * not emulated, so the cue feel must be tuned/validated on real hardware. Cues are
 * gated off during SD writes (rmbl_pause/rmbl_resume) so the motor never toggles the
 * cart bus mid-transfer.
 */

/* The cues, in Settings-menu order. Keep in sync with the tables in rmbl.c. */
enum { RCUE_SCROLL,   /* weak   — cursor move / list scroll                     */
       RCUE_ROOM,     /* medium — entering PC / Party / Bank / Secret Base      */
       RCUE_EDIT,     /* medium — a stat/value was changed                      */
       RCUE_SAVE,     /* strong — a verified save completed                     */
       RCUE_ERROR,    /* strong double "heartbeat" — blocked / failed action    */
       RCUE_COUNT };

void rmbl_init(void);            /* rumble_init + clear cue state. Call once at boot.   */
void rmbl_vblank(void);          /* advance the active cue one frame (tick every vsync) */
void rmbl_fire(int cue);         /* play `cue` if its toggle is on (no-op otherwise)    */

/* SD-transfer safety: stop any cue + freeze the motor around FatFs writes. */
void rmbl_pause(void);
void rmbl_resume(void);

/* Per-cue enable mask (Settings UI + config persistence). */
bool        rmbl_cue_enabled(int cue);
void        rmbl_cue_set(int cue, bool on);
unsigned    rmbl_get_mask(void);
void        rmbl_set_mask(unsigned mask);
const char* rmbl_cue_name(int cue);

/* Global strength + duration (1..5), applied to every cue. Persisted in config.cfg. */
int  rmbl_get_strength(void);
void rmbl_set_strength(int level);
int  rmbl_get_duration(void);
void rmbl_set_duration(int level);
void rmbl_demo(void);            /* play a sample cue ignoring the per-cue mask (Settings preview) */

#endif /* RMBL_H */
