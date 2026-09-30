#ifndef PDNA_GBDAYCARE_H
#define PDNA_GBDAYCARE_H

#include <stdbool.h>
#include "gb_session.h"

/* pdna_gbdaycare — the Gen-1/2 twin of pdna_main.c's pdna_daycare() (BACKLOG #85,
 * BACKLOG #61 parity rule: "mirror Gen 3's Day-Care screen exactly in shape and
 * flow"), over the pure-C core in gb_daycare.h (gbd_read/gbd_deposit/gbd_withdraw/
 * gbd_withdraw_egg — already merged on main).
 *
 * SHAPE, mirrored from pdna_daycare() (BACKLOG #114 ports the yard scene too, now
 * that it lives in source/pdna_yard.{c,h} and not pdna_main.c-private statics):
 *   - a header line + "Boarding N/1" (Gen 1) or "N/2" (Gen 2) count, top right;
 *   - the SAME animated yard scene pdna_daycare() draws (dc_scene/dc_icon_over_bg/
 *     dc_pointer, pdna_yard.h): each OCCUPIED slot's real boarder icon, placed
 *     into the six type-grouped areas (DC_SPOT) via pdna_yard_place(), plus this
 *     visit's gen-correct random visitors (pdna_yard_roll(): 151 species (Kanto
 *     only) on a Gen-1 save, 251 (Kanto+Johto) on Gen 2 — Guy's own wording,
 *     BACKLOG #114 — vs Gen 3's fixed 251); U/D moves the cursor between the
 *     FIXED slots (Gen 1: one, the "Boarder"; Gen 2: two, "Man"/"Lady") — Gen 3
 *     uses L/R because its recs[]/dc[] only ever hold OCCUPIED entries, so it has
 *     no "select an empty slot" concept; this screen does (Put a mon into a named
 *     empty slot), so U/D over fixed indices is the shape adaptation, not a
 *     shortcut. A selected EMPTY slot has no icon to point at — named in the
 *     footer instead (a GB-only case Gen 3 never reaches);
 *   - a third row for the Egg (Gen 2 only, only while `has_egg`) selectable the
 *     same way, still no in-yard representation (an egg is not on the boarder
 *     list gb_daycare.h tracks a species for);
 *   - the SAME three-row status panel geometry pdna_daycare() uses (PDNA_DCY_* in
 *     pdna_layout.h): row 0/1 the game's own compatibility read (gb_daycare.h's
 *     `compatible`/`egg_ready` — a flag, not the on-the-fly gen3 calculation,
 *     since Gen 2 already computed it) or the level-up-only note on Gen 1; row 2
 *     the SAME yard-note row Gen 3's own pk_daycare_yard_note() row is, GB-worded
 *     (gbd_visitors_note()'s three-way wording when app_yard_visitors_ok() is false —
 *     no ROM registered / ROM art off / the Settings switch off — measured, not silently blank).
 *

 * DEPOSIT'S SOURCE: gb_daycare.h's own gbd_deposit() REQUIRES a box-shaped record
 * (`mon->is_party` must be false — "the caller converts party -> box itself first...
 * before calling this", and no such converter is exposed). So — unlike Gen 3's
 * daycare, whose universal mon clipboard (g_clip, pdna_main.c, static) can paste a
 * PARTY-sourced copy — this screen's picker draws from `cur_box`'s STORAGE slots
 * only, never the party. BACKLOG #107 already found there is no reusable "pick one
 * owned mon and return" screen (pdna_pick.c is a SPECIES picker); this file's own
 * `gbdc_pick` is the smallest one that fits gb_daycare's own is_party constraint,
 * mirroring pdna_pick.c's list idiom (a scrollable row list, U/D + A/B) rather than
 * inventing a different shape. `cur_box` is the box the caller was already looking
 * at (Gb12Mount.current_box in pdna_gen12.c) — passed as a plain int so this header
 * stays decoupled from pdna_gen12.h.
 *
 * WITHDRAW'S DESTINATION, retail's own order (BACKLOG #85 D6 review fix): the PARTY
 * if it has room, else the FIRST storage box with a free slot, else refuse — named in
 * the success message ("Sent to the party." / "Sent to Box N."). gbs_insert() (the
 * only public primitive for landing an already-built GbEditMon) REFUSES the party
 * pseudo-box outright (gb_session.h's own doc: "a caller that wants the party target
 * still has to go through gbs_move() with mon inserted into a scratch box first"), so
 * the party leg is composed exactly that way: gbs_insert() into whichever storage box
 * has room, then gbs_move() from there into the party. Every step is a RAM-only
 * commit; the caller rolls the WHOLE image back (gb_rollback(), restores from
 * pristine unconditionally) on any failure before gb_persist() ever runs, so the extra
 * RAM commit this composition needs is exactly as safe to unwind as the single-commit
 * version was. See gbdc_land() in pdna_gbdaycare.c.
 *
 * `s` must be an OPEN session over the SAME resident image gb_persist()/gb_rollback()
 * (pdna_gen12.h) act on — i.e. `&g_ed->s`, exactly like pdna_gbbag()/pdna_gbtrainer().
 * `cur_box` is the box "Put in"/"Take out" read from and land in; `can_edit` gates
 * every write the same way every other Gen-1/2 screen's own `can_edit` does (View
 * always works read-only; Take out/Put in/saving an edit all refuse without it). */
void pdna_gbdaycare(GbSession* s, int cur_box, bool can_edit);

#endif /* PDNA_GBDAYCARE_H */
