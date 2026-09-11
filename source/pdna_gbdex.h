#ifndef PDNA_GBDEX_H
#define PDNA_GBDEX_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession */

/* Gen-1/2's Pokedex screen (BACKLOG #87 item 3), over source/gb_dex.h's honest core.
 * UX-PARITY RULE (BACKLOG #61 / #87's own item 1): this REUSES pdna_pick.c's shared
 * `pdna_dex_screen()` UNCHANGED -- the same grid/list/by-type views, the A-cycles-3-
 * states gesture, the START menu + bulk Catch/See/Wipe-ALL/Undo overlay, the legends,
 * the bob loop, the icon-store rental. This file is ONLY the GB-side wiring: the cap
 * (item 1's pdna_dex_set_max), the get/set shims over gb_dex.h, and the commit path.
 * No second dex screen; nothing here re-renders a grid cell or a bulk-overlay row.
 *
 * `s` must already be open (gbs_open) and BE the live resident session g_ed->s wraps
 * (gb_persist() commits g_ed->img, not an arbitrary GbSession) -- the caller
 * (gb_nav_from_start, pdna_gen12.c) only reaches this row when g_ed is non-NULL, same
 * gate NV_TRAINER/NV_BAG/NV_CLOCK/NV_DAYCARE already use.
 *
 * Returns true iff anything was written to the card (a confirmed, dirty session);
 * false on a declined confirm, a no-op session (opened, browsed, nothing changed), or
 * a malformed session/commit failure (gb_persist's own message already told the user
 * in that last case -- this return value is "did the card change", not "did anything
 * go wrong"). */
bool pdna_gbdex(GbSession* s, bool can_edit);

#endif /* PDNA_GBDEX_H */
