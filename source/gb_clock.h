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
 * is left zeroed; gbc_write() on a Gen-1 session refuses with GBS_ERR_ARG (the same
 * "not applicable to this generation" convention gb_trainer.h's gbt_field_present()
 * already establishes — a caller checks `present`/`gbc_field_present()` BEFORE it ever
 * offers the row, the same way it checks gbt_field_present() for GBF_GENDER).
 *
 * WHAT THIS TRACKS (design doc §1.8):
 *   - wStartDay/Hour/Minute/Second — the RTC snapshot the game took when it was last
 *     started (the "epoch" the in-game clock counts forward from).
 *   - wRTC (4 raw bytes) — the last-read cartridge-RTC snapshot; VIEW-ONLY here (there
 *     is no cartridge RTC on a GBA flashcart to read a fresh one from — see gbc_write's
 *     own comment for what "sync" means in this design instead).
 *   - wDST — the daylight-saving bit (Crystal's own daylight state; the design doc
 *     marks its exact semantics "UNSURE until a hardware run"). Exposed as a bool
 *     (`v != 0`) since the corpus stores 0x80 for "on", not a bare 1; gbc_write()
 *     compares the wanted BOOL against the current bool before writing, so a no-op
 *     round trip never normalises that 0x80 down to 0x01 (see gb_clock.c).
 *   - game time h/m/s/f + wGameTimeCap bit 0 — VIEW-ONLY here; this is gb_trainer.h's
 *     GBF_GAMETIME_* pair (the play-time clock), already owned by gbt_read/gbt_write —
 *     duplicating a WRITE path for it here would let two cores race the same bytes.
 *   - wCurDay — the raw day-of-week byte. EXPOSED RAW, not decoded into a weekday name:
 *     the corpus shows this byte does NOT read 0..6 (Gold.sav: 0x6B, matching wRTC's own
 *     first byte) — whatever day-of-week table the game derives it through is a UI
 *     concern this data core does not own.
 *   - sRTCStatusFlags (GBF_RTC_STATUS_FLAGS) — the "RTC has been reset / clock is
 *     unreliable" flag. OUTSIDE every checksummed span (design doc §1.8), so it is
 *     written on its own via gbs_write_field/gbs_finish exactly like every other field
 *     here — gbs_write_field's Gen-2 path (g2w_write_range) does not require a byte to
 *     be inside the checksummed span to patch it, only that it not be one of the four
 *     things that function's own header refuses (the RTC tail, the checksums/backup,
 *     a Pokemon list, the current-box number). Both corpus saves read this byte 0x00 —
 *     UNSURE until a hardware run can set it nonzero and observe the effect (same
 *     caveat the design doc itself states).
 */

typedef struct {
  bool     present;              /* false on Gen 1 -- nothing else here is meaningful */

  uint8_t  start_day;
  uint8_t  start_hour;
  uint8_t  start_minute;
  uint8_t  start_second;

  uint8_t  rtc_snapshot[4];      /* VIEW-ONLY, see the header note above              */

  bool     dst;                  /* wDST != 0                                        */

  uint16_t gametime_hours;       /* VIEW-ONLY -- owned by gb_trainer's GbPlayTime     */
  uint8_t  gametime_minutes;
  uint8_t  gametime_seconds;
  uint8_t  gametime_frames;
  bool     gametime_capped;      /* wGameTimeCap bit 0, VIEW-ONLY                    */

  uint8_t  cur_day;              /* raw wCurDay byte, VIEW-ONLY (see header note)     */

  bool     status_flags_ok;      /* false: GBF_RTC_STATUS_FLAGS unreadable/absent     */
  uint8_t  status_flags;         /* sRTCStatusFlags, raw                              */
} GbClock;

/* Which of the four field-table games this session is. Gen 1 always answers
 * GBF_G_RED (Yellow == Red/Blue layout) but gbc_field_present(GBF_G_RED, ...) is false
 * for every clock field, so gbc_read/gbc_write both refuse on it regardless. */
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

/* Write ONLY the editable subset (start_day/hour/minute/second, dst, status_flags) and
 * ONLY the bytes that actually changed (P1a's own set_u_unless_same discipline, gb_
 * trainer.c) — an unrelated edit must never move sRTCStatusFlags back to whatever
 * gbc_read happened to fill it with, and a pure read -> write round trip must not move
 * a single byte.
 *
 * "sync to the GBA cart's RTC" (design doc §1.8's other half of the Gen-3 Clock-fix
 * shape) is NOT this function's job: there is no cartridge RTC to read FROM here (the
 * GB cart's own RTC lives in the GB cartridge, not the GBA flashcart this tool runs
 * on) — the caller (a screen, not part of this slice) is the one that decides what
 * start_day..start_second to pass, whether that is a value the player typed or a value
 * read from PokeDNA's own gba_rtc_get_status() (source/gba_rtc.h) as the closest
 * available stand-in. This core only ever writes bytes it is handed.
 *
 * clear_status_flags=true additionally zeroes GBF_RTC_STATUS_FLAGS (the "RTC has been
 * reset" flag) UNCONDITIONALLY when the field is already nonzero -- read-then-write,
 * never a blind store, so a session where it already reads 0 is left untouched.
 *
 * Refuses with GBS_ERR_ARG on a Gen-1 session (gbc_field_present(g, GBF_RTC_START_DAY)
 * false) or a NULL session/argument, before a single byte moves. Calls gbs_finish()
 * once at the end of the batch, and only when something actually changed. */
GbsStatus gbc_write(GbSession* s, const GbClock* in, bool clear_status_flags);

#endif /* GB_CLOCK_H */
