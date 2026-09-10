#ifndef PDNA_GBBOXNAMES_H
#define PDNA_GBBOXNAMES_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession */

/* Gen-2 box-name screen (BACKLOG #94) -- Gen 2 only (gb_boxnames.h: Gen 1 has no
 * per-box name table at all). A dedicated list/rename screen rather than folding
 * into source/pdna_box.c's shared box-grid banner: that banner's rename lives
 * behind BoxSource.can_edit(), which ALSO gates the grid's move/paste/release/
 * wallpaper actions for every source (source/pdna_box.h) -- turning it on for the
 * Game-Boy-mounted BoxSource (source/pdna_gen12.c's pdna_gen12_source(), which is
 * READ-ONLY BY DESIGN: gbsrc_can_edit()/gbsrc_commit() are hard-coded false so a
 * Pokemon can never appear to move into/out of a raw Game Boy save through the
 * shared grid) would unlock all of those too, not just the rename row. This screen
 * reuses the same TEXT EDITOR (osk_input, "BOX NAME" prompt, 9-byte cap) Gen 3's
 * own box-banner rename uses (source/pdna_box.c's box_options_menu sel==0 /
 * on_title KEY_A) -- same widget, same prompt string, scoped to rename only.
 *
 * `can_edit`: true for the editable session (gb_nav_from_start passes &g_ed->s,
 * true, same convention as pdna_gbtrainer.h/pdna_gbfly.h), false for a view-only
 * visit -- U/D still scrolls so every name can be read, A does nothing. Commits via
 * gb_persist() (pdna_gen12.h) on B, only when at least one box was actually
 * renamed (gbbn_rename's own "unchanged is untouched" guard already skips a no-op
 * write; this screen tracks `dirty` across the whole visit so N renames cost one
 * write, not N). */
void pdna_gb_boxnames(GbSession* s, bool can_edit);

#endif /* PDNA_GBBOXNAMES_H */
