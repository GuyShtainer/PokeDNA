#ifndef GB_CLOCK_H
#define GB_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame, GbField, gbf_off/len/kind                          */
#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish */

/* gb_clock — pure-C Gen-2 "Clock fix" core over the generated field table
 * (BACKLOG #86, docs/GEN12-PARITY-DESIGN.md §1.8).
 *
 * Gen 1 HAS NO RTC AT ALL — the Clock-fix nav row must stay hidden for a Gen-1 session,
 * exactly like the Gen-3 screen this mirrors. `present` is false and every other field
 * is left zeroed; every write op on a Gen-1 session refuses with GBS_ERR_ARG (the same
 * "not applicable to this generation" convention gb_trainer.h's gbt_field_present()
 * already establishes — a caller checks `present`/`gbc_field_present()` BEFORE it ever
 * offers the row, the same way it checks gbt_field_present() for GBF_GENDER).
 *
 * ============================================================================
 * WHAT THE IN-GAME CLOCK ACTUALLY IS (P1a review D1 — this replaces an earlier, WRONG
 * design that treated wStartDay/Hour/Minute/Second as an absolute time you could set).
 *
 * pokecrystal's engine/rtc/timeset.asm InitClock asks the player for a wall-clock hour
 * and minute at new-game time. home/time.asm's FixTime:: then does, every frame:
 *
 *     hSeconds = (hRTCSeconds + wStartSecond) mod 60, carry into minutes
 *     hMinutes = (hRTCMinutes + wStartMinute) mod 60, carry into hours
 *     hHours   = (hRTCHours   + wStartHour)   mod 24, carry into days
 *     wCurDay  =  hRTCDayLo   + wStartDay                          (NOT mod 7 -- see below)
 *
 * i.e. wStartDay/Hour/Minute/Second are OFFSETS ADDED to the cartridge's own hardware
 * RTC counter (hRTCDayLo/Hours/Minutes/Seconds, latched fresh off the MBC3/7 RTC
 * registers by home/time.asm's GetClock), not a stored wall-clock time. "_InitTime"
 * (the code InitClock/InitDayOfWeek farcall to actually commit the player's typed
 * hour/minute) computes the INVERSE: it reads the current hardware RTC and solves for
 * the wStartX values that make FixTime's sum equal what the player typed.
 *
 * We verified this the hard way before finding the algorithm: writing a literal 14:20
 * into wStartHour/wStartMinute made the BOOTED GAME show 23:20, not 14:20, because the
 * cartridge's own hRTCHours/hRTCMinutes were nonzero and FixTime added them in. There
 * is no cartridge RTC on a GBA flashcart for this core to read FROM (the GB cartridge's
 * RTC lives in the GB cart, not the GBA flashcart PokeDNA runs on) — so this core
 * CANNOT compute or write an absolute wall-clock time. It can only do what a player
 * physically can do at retail without knowing the reset password: shift the offset by a
 * known delta, or ask the game to re-run its own new-game clock-set prompt.
 *
 * THE UI THIS CORE SUPPORTS (P1a review D1's own words — do not add "Auto-sync" or
 * "Manual set", both misdescribe what is actually happening):
 *   - "Ask for the time at next load"  -> gbc_request_time_reset(): sets the RTC_RESET
 *     bit of sRTCStatusFlags so the next CONTINUE runs the game's own RestartClock
 *     prompt (engine/menus/intro_menu.asm Continue_CheckRTC_RestartClock tests this
 *     exact bit) -- the SAME prompt engine/rtc/reset_password.asm's password path
 *     triggers, reached here WITHOUT needing the password.
 *   - "Shift the clock by ±H:MM, ±N days" -> gbc_shift(): adds a signed delta to the
 *     wStartDay/Hour/Minute/Second offsets, with the same borrow/carry chain FixTime's
 *     addition uses (seconds carry into minutes into hours into days).
 *   - "Clear the clock-error flag" -> gbc_clear_status_flags(): dismisses the on-screen
 *     clock-error banner ONLY. A dead cartridge battery re-raises RTC_RESET on every
 *     future boot (home/init.asm's StartClock -> FixDays -> RecordRTCStatus re-ORs the
 *     flag in whenever the hardware day-count looks impossible), so this is not a fix
 *     for a dead battery, only a way to stop nagging about one boot's reading.
 *
 * `cur_day` (below) is hRTCDayLo + offset_day PER FixTime'S OWN FORMULA ABOVE — it is a
 * running day COUNT since the cartridge's RTC was last cleared, not a weekday index; a
 * weekday (Sun=0..Sat=6, per pokecrystal's own wTempDayOfWeek/SetDayOfWeek table) is
 * that count mod 7, and this core does not compute or expose it (see the field's own
 * comment).
 *   - wRTC (4 raw bytes) — the last-read cartridge-RTC snapshot; VIEW-ONLY.
 *   - wDST — the daylight-saving bit (Crystal's own daylight state; semantics "UNSURE
 *     until a hardware run" per the design doc). Exposed as a bool (`v != 0`) since the
 *     corpus stores 0x80 for "on", not a bare 1; the shift/clear ops compare the wanted
 *     BOOL against the current bool before writing so a no-op round trip never
 *     normalises that 0x80 down to 0x01.
 *   - game time h/m/s/f + wGameTimeCap bit 0 — VIEW-ONLY here; this is gb_trainer.h's
 *     GBF_GAMETIME_* pair (the play-time clock), already owned by gbt_read/gbt_write —
 *     duplicating a WRITE path for it here would let two cores race the same bytes.
 *   - sRTCStatusFlags (GBF_RTC_STATUS_FLAGS) — the "RTC has been reset / clock is
 *     unreliable" flag. OUTSIDE every checksummed span (design doc §1.8), so it is
 *     written on its own via gbs_write_field/gbs_finish exactly like every other field
 *     here — gbs_write_field's Gen-2 path (g2w_write_range) does not require a byte to
 *     be inside the checksummed span to patch it, only that it not be one of the four
 *     things that function's own header refuses (the RTC tail, the checksums/backup,
 *     a Pokemon list, the current-box number).
 * ============================================================================
 */

typedef struct {
  bool     present;              /* false on Gen 1 -- nothing else here is meaningful */

  /* wStartDay/Hour/Minute/Second: OFFSETS added to the hardware RTC by FixTime, NOT an
   * absolute time (see the header note above). */
  uint8_t  rtc_offset_day;
  uint8_t  rtc_offset_hour;
  uint8_t  rtc_offset_minute;
  uint8_t  rtc_offset_second;

  uint8_t  rtc_snapshot[4];      /* VIEW-ONLY, see the header note above              */

  bool     dst;                  /* wDST != 0                                        */

  uint16_t gametime_hours;       /* VIEW-ONLY -- owned by gb_trainer's GbPlayTime     */
  uint8_t  gametime_minutes;
  uint8_t  gametime_seconds;
  uint8_t  gametime_frames;
  bool     gametime_capped;      /* wGameTimeCap bit 0, VIEW-ONLY                    */

  /* wCurDay, raw: hRTCDayLo + rtc_offset_day (FixTime's own formula), a running DAY
   * COUNT since the cartridge RTC was last cleared -- NOT a weekday. weekday = this
   * value mod 7 (Sun=0..Sat=6), which this core does not compute. VIEW-ONLY. */
  uint8_t  day_count;

  bool     status_flags_ok;      /* false: GBF_RTC_STATUS_FLAGS unreadable/absent     */
  uint8_t  status_flags;         /* sRTCStatusFlags, raw                              */
} GbClock;

/* Which of the four field-table games this session is. Gen 1 always answers
 * GBF_G_RED (Yellow == Red/Blue layout) but gbc_field_present(GBF_G_RED, ...) is false
 * for every clock field, so every read/write op refuses on it regardless. */
GbGame gbc_game(const GbSession* s);

/* Mirrors gbf_off(game, field) != 0 -- a screen hides a row this game does not have
 * without repeating the field table's own rule. */
bool gbc_field_present(GbGame game, GbField field);

/* Fill `out` from the session's resident image. False only on a malformed session
 * (never opened). A Gen-1 session fills out->present = false and returns true — this
 * is a successful read that says "there is no clock", not a failure; the caller asks
 * `out->present` before showing the row, the same shape gbt_read uses for has_mom/
 * has_gender. */
bool gbc_read(const GbSession* s, GbClock* out);

/* Shift the wStartDay/Hour/Minute/Second OFFSETS by a signed delta, using the same
 * borrow/carry chain FixTime's own addition uses: d_seconds carries into d_minutes,
 * d_minutes into d_hours, d_hours into d_days (each modulo 60/60/24 respectively); the
 * day offset itself wraps modulo 256 (its own byte width — FixDays keeps the hardware
 * day counter itself bounded separately, this core does not touch that half). Every
 * delta may be zero, positive or negative; e.g. gbc_shift(s, 0, 2, 0, 0) moves the
 * displayed clock forward two hours. Writes ONLY the bytes that actually change (never
 * a blind four-byte store) and calls gbs_finish() once at the end, only when something
 * moved. Refuses with GBS_ERR_ARG on a Gen-1 session or a NULL session, before a single
 * byte moves. */
GbsStatus gbc_shift(GbSession* s, int32_t d_days, int32_t d_hours, int32_t d_minutes,
                    int32_t d_seconds);

/* "Ask for the time at next load": sets sRTCStatusFlags = 0x80 (RTC_RESET), the exact
 * bit engine/menus/intro_menu.asm's Continue_CheckRTC_RestartClock tests to decide
 * whether to run the game's own new-game-style clock-set prompt on the next CONTINUE --
 * the password-protected clock reset (engine/rtc/reset_password.asm), reached here
 * without the password. A no-op (returns GBS_OK, writes nothing) if the flag already
 * reads 0x80. Refuses with GBS_ERR_ARG on a Gen-1 session or a NULL session. */
GbsStatus gbc_request_time_reset(GbSession* s);

/* Dismisses the clock-error banner ONLY -- zeroes GBF_RTC_STATUS_FLAGS when it is
 * currently nonzero (read-then-write, never a blind store, so an already-0 session is
 * left untouched). This does NOT fix a dead cartridge battery: home/init.asm's
 * StartClock -> FixDays -> RecordRTCStatus re-raises RTC_RESET at every future boot
 * whenever the hardware day count looks impossible, so a dead-battery cart will show
 * the error again next time it boots regardless of what this call does. Refuses with
 * GBS_ERR_ARG on a Gen-1 session or a NULL session. */
GbsStatus gbc_clear_status_flags(GbSession* s);

#endif /* GB_CLOCK_H */
