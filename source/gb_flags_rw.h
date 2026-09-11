#ifndef GB_FLAGS_RW_H
#define GB_FLAGS_RW_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_session.h"   /* GbSession, GbsStatus                                       */
#include "gb_flags.h"     /* GbGame (via gb_fields.h)                                    */

/* BACKLOG #88: read/write ANY event-flag bit `n` (not just the ~40-entry named
 * shortlist gb_flags.h/.c carries) through a live GbSession -- backs both the FLAGS
 * tab's named rows AND the raw flag browser (mirrors Gen 3's flags_raw_view /
 * pk_flag_get/pk_flag_set, pdna_main.c), and lets a caller gate on an UNNAMED flag
 * (EVENT_IN_SAFARI_ZONE, via gbfl_safari_zone_flag()) without it needing its own
 * named-shortlist row.
 *
 * Bit addressing is IDENTICAL to Gen 3's own event-flags array (gen3_flags.c's
 * pk_flag_get/pk_flag_set): byte = GBF_EVENT_FLAGS_BASE(_G2 on Gen 2) + n/8,
 * bit = n & 7. Pure C (no tonc/GBA headers) -- host-tested directly by
 * tests/host_gbflagsrw_test.c.
 */

/* Read bit `n` of `g`'s event-flags bitfield through session `s`. False for a NULL/
 * unopened session, a game with no event-flags region (should not happen -- every
 * GbGame has one), or n >= the region's own bit count (gbf_len(g, field) * 8). */
bool gbfl_get(const GbSession* s, GbGame g, uint16_t n);

/* Write bit `n` to `v` the same way, through gbs_write_field (inherits that call's
 * own refusal set -- confined to the checksummed span, never a Pokemon list). A
 * no-op (the bit already reads as `v`) writes NOTHING (hard rule 3: a no-op writes
 * nothing, not even a redundant identical byte) and returns GBS_OK. GBS_ERR_ARG for
 * a NULL/unopened session, no event-flags region, or n out of range. */
GbsStatus gbfl_set(GbSession* s, GbGame g, uint16_t n, bool v);

#endif /* GB_FLAGS_RW_H */
