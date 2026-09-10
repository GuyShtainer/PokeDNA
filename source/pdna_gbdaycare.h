#ifndef PDNA_GBDAYCARE_H
#define PDNA_GBDAYCARE_H

#include <stdbool.h>
#include "gb_session.h"

/* pdna_gbdaycare — the Gen-1/2 twin of pdna_main.c's pdna_daycare() (BACKLOG #85,
 * BACKLOG #61 parity rule: "mirror Gen 3's Day-Care screen exactly in shape and
 * flow"), over the pure-C core in gb_daycare.h (gbd_read/gbd_deposit/gbd_withdraw/
 * gbd_withdraw_egg — already merged on main).
 *
 * SHAPE, mirrored from pdna_daycare():
 *   - a header line + "Boarding N/1" (Gen 1) or "N/2" (Gen 2) count, top right;
 *   - one row per slot (Gen 1: one, the "Boarder"; Gen 2: two, "Man"/"Lady"), U/D
 *     moves a cursor between them (Gen 3 uses L/R over two icons; this screen has no
 *     icons, so U/D over two text rows is the shape adaptation, not a shortcut);
 *   - a third row for the Egg (Gen 2 only, only while `has_egg`) below the two slots;
 *   - the SAME three-row status panel geometry pdna_daycare() uses (PDNA_DCY_* in
 *     pdna_layout.h), showing the game's own compatibility read (gb_daycare.h's
 *     `compatible`/`egg_ready` — a flag, not the on-the-fly gen3 calculation, since
 *     Gen 2 already computed it) or the level-up-only note on Gen 1.
 *
 * DELIBERATELY NOT PORTED, and why: pdna_daycare()'s animated yard scene
 * (dc_scene/dc_icon_over_bg/dc_house/dc_fence, PDNA_NO_DAYCARE_BG) is private to
 * pdna_main.c (static, no exported entry point) and is Gen-3 icon-cache
 * infrastructure this slice's file list (source/pdna_gbdaycare.{c,h} only) cannot
 * reach without editing pdna_main.c, which is out of scope for this UI slice. The
 * PANEL/menu shape is mirrored exactly; the bobbing-icon yard art is not wired up —
 * flagged, not silently skipped.
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
 * WITHDRAW'S DESTINATION, same reasoning in reverse: gbs_insert() (the only public
 * primitive for landing an already-built GbEditMon) REFUSES the party pseudo-box
 * outright (gb_session.h's own doc: "a caller that wants the party target still has
 * to go through gbs_move() with mon inserted into a scratch box first"). Composing
 * that (gbs_insert then gbs_move, with its own Mail-shift/party-floor rules and a
 * second RAM commit to roll back on failure) is real extra risk for a UI slice, so
 * "Take out" lands the mon back in `cur_box` only — the same box the picker draws
 * from — never the party. This is a narrower menu than pdna_daycare()'s own
 * "To Party / To PC" (it offers only the PC-shaped half), flagged as a scope
 * reduction rather than silently built to look complete.
 *
 * `s` must be an OPEN session over the SAME resident image gb_persist()/gb_rollback()
 * (pdna_gen12.h) act on — i.e. `&g_ed->s`, exactly like pdna_gbbag()/pdna_gbtrainer().
 * `cur_box` is the box "Put in"/"Take out" read from and land in; `can_edit` gates
 * every write the same way every other Gen-1/2 screen's own `can_edit` does (View
 * always works read-only; Take out/Put in/saving an edit all refuse without it). */
void pdna_gbdaycare(GbSession* s, int cur_box, bool can_edit);

#endif /* PDNA_GBDAYCARE_H */
