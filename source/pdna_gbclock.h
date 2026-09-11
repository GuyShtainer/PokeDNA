#ifndef PDNA_GBCLOCK_H
#define PDNA_GBCLOCK_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession                                            */

/* Gen-2's own "Clock fix" screen (BACKLOG #86/#108), over source/gb_clock.h's honest
 * core. UX-PARITY RULE (BACKLOG #61): this reuses the shape and flow of the Gen-3
 * "Clock fix" screen (pdna_clock(), source/pdna_main.c) -- a plain rows page (no art),
 * a view-only readout at the top, row actions each going through app_confirm() before
 * anything is written, and the SAME verified-write commit path (gb_persist(), through
 * gb_clock.h's gbc_* ops which already batch gbs_write_field/gbs_finish -- see below).
 *
 * It does NOT reuse pdna_clock()'s own rows: the Gen-3 screen promises an absolute
 * cart-clock sync/manual-set, which gb_clock.h's own header explains is impossible
 * here (there is no cartridge RTC on the GBA flashcart to read the Game Boy cart's
 * hardware RTC from -- see gb_clock.h's header note in full). This screen instead
 * offers the THREE things a player can actually do without the reset password,
 * verbatim from gb_clock.h's own header:
 *
 *   Row 1  "Ask for the time at next load"  -> gbc_request_time_reset()
 *   Row 2  "Shift the clock"                -> gbc_shift() (signed +-days/hours/min)
 *   Row 3  "Clear the clock-error flag"     -> gbc_clear_status_flags()
 *
 * Never an absolute-time promise, never "Auto-sync"/"Manual set" (those names
 * describe the Gen-3 screen's mechanism, not this one's).
 *
 * `s` must already be open (gbs_open) and BE the live resident session g_ed->s wraps
 * (gb_persist() commits g_ed->img, not an arbitrary GbSession) -- the caller
 * (gb_nav_from_start, pdna_gen12.c) only reaches this row when g_ed is non-NULL, same
 * gate NV_TRAINER/NV_BAG/NV_PACK already use. `can_edit`: true for the editable
 * session; false is not expected here today (nav_avail only offers this row when a
 * write path exists), but the screen still degrades to a read-only view rather than
 * assert, for the same defensive reason every other GB screen does.
 *
 * Gen 1 has no clock at all (gb_clock.h: `present` is false) -- nav_avail.c's own
 * GB_TABLE keeps the Gen-1 Clock row NAV_NOT_IN_GAME, so this screen is not expected
 * to be reached for a Gen-1 session either; it still shows the honest "no clock"
 * fallback rather than assert, matching pdna_clock()'s own FRLG fallback shape. */
void pdna_gbclock(GbSession* s, bool can_edit);

#endif /* PDNA_GBCLOCK_H */
