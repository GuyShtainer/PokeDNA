#ifndef PDNA_GBBAG_H
#define PDNA_GBBAG_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession                                            */

/* Gen-1's OWN Item bag / PC item store, on the shared GB-screen shell (U4,
 * BACKLOG #67, docs/GB-GAME-SCREENS-DESIGN.md sec 1.3, docs/briefs/U4-gen1-bag-brief.md).
 *
 * Ground truth (mGBA `tools/gb_roundtrip.py`'s own `_drive()` driving Red.gb +
 * Red.sav and Yellow.gb + Yellow.sav to START > ITEM, dumping the window
 * tilemap at 0x9C00 -- LCDC=0xE3: bit6=1 selects window map 0x9C00, bit4=1
 * selects UNSIGNED $8000 tile addressing, WX=7/WY=0 = full-screen window):
 *
 *  - The list box is (4,2)-(19,12) inclusive (LIST_MENU_BOX), on the WINDOW
 *    layer -- NOT the BG map the design doc's own §1.3 table guessed; matches
 *    what U2c's own trainer-card finding already established for this same
 *    ROM ("Red draws menus on the window at 0x9C00").
 *  - The frame is TEXTBOX-block-RELATIVE tiles 25-31 (VRAM ids 0x79-0x7F;
 *    TEXTBOX's own base is VRAM $60, since FONT occupies $80-$FF and
 *    TEXTBOX's 32 tiles sit directly below it) -- NOT tile 0, confirmed
 *    byte-for-byte identical in Red AND Yellow:
 *      25 (0x79) top-left corner       28 (0x7c) vertical side (L and R alike)
 *      26 (0x7a) horizontal fill       29 (0x7d) divider/bottom-left corner
 *      27 (0x7b) top-right corner      30 (0x7e) divider/bottom-right corner
 *                                      31 (0x7f) blank interior fill
 *  - EXACTLY 4 item rows are visible at once (name row then qty row: y=4/5,
 *    6/7, 8/9, 10/11) -- NOT 3 (the design doc's own "wMaxMenuItem=2 => 3
 *    rows" guess is WRONG; a 19-entry Items pocket and a 4-entry one both
 *    showed 4 full rows). Cursor `▶` (charmap 0xED, drawn as a FONT cell) at
 *    column 5; item name at column 6 (up to 13 cols wide, e.g. "ESCAPE ROPE");
 *    `×` (charmap 0xF1, gb_char_encode's own U+00D7 mapping) then a
 *    right-aligned 2-digit quantity at column 14, one row BELOW the name
 *    (SCREEN_WIDTH+8 from the name's own start, exactly matching the design
 *    doc's citation) -- confirmed on Yellow that a key item (BICYCLE, SUPER
 *    ROD) leaves that row BLANK, a regular item (ESCAPE ROPE, MASTER BALL)
 *    fills it, matching "key items print no quantity".
 *  - Scrolling: pressing DOWN past the 4th visible row shifts the whole
 *    window down by one row per press (verified across 6 DOWN presses on a
 *    19-item Items pocket) with NO tile-visible scroll marker at (5,3) or
 *    anywhere else this shell's tilemap oracle can see -- the design doc's
 *    own "(5,3) scroll-up marker" either does not exist as a BG/window tile
 *    or (more likely, given U3's precedent: the Gen-2 badge overlay is a
 *    sprite the same oracle cannot see either) is an OAM sprite. This shell
 *    has no sprites (same posture as every other GB screen here), so no
 *    scroll marker is drawn; an accepted deviation, not a bug.
 *  - Below the list, a SEPARATE nested text box at (10,12)-(19,15) holds
 *    "EXIT" (not "CANCEL" -- the design doc's own guess) at row 14, cols
 *    12-15; its top edge (row 12, cols 10-19) is literally the SAME cells as
 *    the main box's own bottom divider row, which spans the full (4,12)-
 *    (19,12) width using the divider-corner tiles 29/30 at its own ends.
 *    This screen does not make "EXIT" independently cursor-selectable (no
 *    capture exercised moving the cursor onto it, and it is drawn as a
 *    visually separate box, not a 5th list row) -- B does the equivalent
 *    "leave" action instead, the same contract every other GB screen's own
 *    B key already carries here.
 *  - The PC item store re-uses the IDENTICAL box/list routine over a
 *    different pocket (design doc's own citation: "ITEMLISTMENU again, over
 *    wNumBoxItems") -- not independently pixel-dumped in this slice (reaching
 *    a Pokemon Center PC needs real overworld navigation, out of this
 *    slice's time box); the geometry above is reused unchanged, gated only
 *    on which GbBagPocket backs the list.
 *  - SWAP (the game's SELECT-to-swap feature) is NOT bound to SELECT here --
 *    SELECT is the shell's own 1:1<->stretched toggle (design sec 1.5,
 *    required on every GB screen) -- it lives on the START menu instead,
 *    alongside ADD ITEM / REMOVE / TOSS.
 *  - Item names: NOT located by this slice (time-boxed, brief's own
 *    permission) -- every item prints "ITEM #n" (n = the raw id byte) rather
 *    than a real name; ships no names table, no decoded pixel/text data. The
 *    dump's own on-screen names ("TM10", "BICYCLE", ...) are quoted in this
 *    comment purely as ground truth for the CELL LAYOUT, never as shipped data.
 *
 * `s` must already be open (gbs_open, Gen-1 only -- gbb_field_present() gates
 * every pocket this game lacks, but the whole screen itself is Gen-1 ONLY;
 * callers must not reach this for a Gen-2 session). `can_edit`: true for the
 * editable session, false for a view-only visit (cursor still moves; A/START
 * do nothing). Falls back to a plain PokeDNA rows page (BACKLOG #67's own
 * "no dead end" rule) when the GB-screen shell itself refuses -- no ROM
 * registered, not enough stack, a non-English release, or no tile-bank
 * memory -- with the refusal reason on line 2, same posture as the trainer
 * card's own fallback (pdna_gbtrainer.c). */
void pdna_gbbag(GbSession* s, bool can_edit);

#endif /* PDNA_GBBAG_H */
