#ifndef GB_BOXNAMES_H
#define GB_BOXNAMES_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame                                                     */
#include "gb_session.h"   /* GbSession, GbsStatus                                       */
#include "gen2_save.h"    /* G2_NUM_BOXES, G2_BOXNAME_CHARS, g2_box_name_at             */

/* gb_boxnames — pure-C Gen-2 box-name core (BACKLOG #94, docs/GEN12-PARITY-DESIGN.md
 * §1.0's own region table + gen2_write.h's g2w_set_box_name, which this core is a thin
 * wrapper around rather than a re-derivation).
 *
 * GEN 1 IS NOT APPLICABLE: pokered/pokeyellow have no per-box name table at all (every
 * box banner is always literally "BOX n") — gbbn_read()/gbbn_rename() both refuse a
 * Gen-1 session outright (false / GBS_ERR_ARG) rather than inventing a field. A caller
 * asks gbbn_supported(session) BEFORE it ever offers the rename row, the same shape
 * gbt_field_present()/gbc_field_present() use elsewhere in this family.
 *
 * WHY THIS WRAPS EXISTING PRIMITIVES INSTEAD OF GOING THROUGH gbs_read_field/
 * gbs_write_field THE WAY THE OTHER THREE #85-#95 CORES DO: gen2_save.c already ships a
 * whole-image reader (g2_box_name_at, decoding through g2_offsets().box_names) and
 * gen2_write.c already ships a purpose-built, self-verifying writer
 * (g2w_set_box_name) that validates the glyph count/charset, patches the field,
 * mirrors it, refreshes both checksums AND re-verifies the whole file in one call —
 * exactly gbf_off(GBF_BOXNAMES)'s own offset (0x2727 GS / 0x2703 Crystal, cross-checked
 * against gen2_save.c's o.box_names in the same commit that added the field), just
 * already built and already reviewed. Re-deriving a second encode/write path over the
 * generic field primitive would race the same bytes through two different validators
 * for no benefit; GBF_BOXNAMES itself still exists (for a reader that wants the raw
 * 126-byte blob without decoding, or a future screen doing its own paging) and this
 * core's own host test cross-checks gbf_off(..., GBF_BOXNAMES) against
 * g2_offsets().box_names to keep the two from silently drifting apart.
 */

#define GB_BOXNAME_GLYPHS G2_BOXNAME_CHARS   /* 8 -- BOX_NAME_LENGTH(9) minus the
                                              * terminator, same convention as gb_trainer.h's
                                              * GB_OT_GLYPHS */

/* Is this session's generation one Gen 2 saves ever have box names for at all? False
 * for a Gen-1 session or a malformed one. */
bool gbbn_supported(const GbSession* s);

/* Read box `box`'s (0..G2_NUM_BOXES-1) name into `out` (UTF-8, `cap` >= GB_TEXT_MAX to
 * be safe, same convention as gb_trainer.h's name fields). False on a Gen-1 session, a
 * bad box index, or a malformed session -- `out[0]` is left 0 in every false case. */
bool gbbn_read(const GbSession* s, int box, char* out, int cap);

/* Rename box `box`. Refuses (GBS_ERR_ARG) before a single byte moves when: the session
 * is Gen 1 (gbbn_supported false), `box` is out of range, `utf8` is over
 * GB_BOXNAME_GLYPHS glyphs, or the target generation's charset cannot store it exactly
 * -- g2w_set_box_name's own encode_checked() is the single source of truth for that
 * refusal, not a second hand-rolled check here.
 *
 * UNCHANGED IS UNTOUCHED: compares the DECODED current name against `utf8` first (same
 * discipline gb_trainer.c's set_name_if_changed() documents — a GB-encoded field can
 * carry stray residue past its own terminator that a blind re-encode would overwrite
 * even when nothing the player can see changed) and skips the whole write, including
 * gen2_write.c's own checksum refresh, when they already match. Otherwise delegates to
 * g2w_set_box_name(), which mirrors, refreshes both stored checksums and re-verifies
 * the whole file itself -- no separate gbs_finish() call is needed or made. */
GbsStatus gbbn_rename(GbSession* s, int box, const char* utf8);

#endif /* GB_BOXNAMES_H */
