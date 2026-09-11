#ifndef PDNA_GBBAG_H
#define PDNA_GBBAG_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession                                            */

/* Gen-1's OWN Item bag / PC item store, on the shared GB-screen shell (U4,
 * BACKLOG #67, docs/GB-GAME-SCREENS-DESIGN.md sec 1.3, docs/briefs/U4-gen1-bag-brief.md).
 *
 * Ground truth (mGBA `tools/gb_roundtrip.py`'s own `_drive()` driving Red.gb +
 * Red.sav and Yellow.gb + Yellow.sav to START > ITEM, dumping the window
 * tilemap at 0x9C00 -- LCDC=0xE3: bit6=1 selects window map 0x9C00, bit4
 * CLEAR selects SIGNED $8800 tile addressing (U4 fix-pass review, D6: the
 * previous wording here had this bit backwards -- "bit4=1 selects UNSIGNED
 * $8000" -- 0xE3's bit4 is 0, and the real cartridge's own tile ids for this
 * screen, e.g. TEXTBOX-relative 25-31, only decode correctly under SIGNED
 * $8800 addressing), WX=7/WY=0 = full-screen window):
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
 *    fills it, matching "key items print no quantity" (implemented via
 *    gbb_is_g1_key_item(), gb_bag.h -- see that function's own comment for
 *    the "documented fallback, not a located ROM table" note).
 *  - The list is `count` real entries PLUS a trailing, cursor-selectable
 *    CANCEL row (index == count) -- confirmed by scrolling a 19-item Items
 *    pocket all the way down: the 20th "row" reads CANCEL and A on it does
 *    the same "leave" action as B. Scrolling CLAMPS at both ends (no wrap:
 *    UP at the first row stays put, DOWN at CANCEL stays put) and PINS the
 *    cursor at the 3rd visible row (screen row 8) once scrolled two rows
 *    past the top -- verified with a 52-press real-cartridge trace (26x
 *    DOWN then 26x UP) whose per-press cursor ROW sequence matches this
 *    shell's own gbbag_clamp_scroll() exactly (pdna_gbbag.c). A down-scroll
 *    marker (font tile 0xEE) BLINKS at screen cell (18,11) exactly while
 *    more of the list sits below the 4 visible rows -- it IS a real,
 *    visible tile on the window layer, contrary to an earlier draft of
 *    this comment that guessed it must be an invisible-to-this-oracle OAM
 *    sprite; that guess was wrong, not the tool.
 *  - Below the list, a SEPARATE nested text box at (10,12)-(19,15) holds
 *    "EXIT" (not "CANCEL" -- the design doc's own guess) at row 14, cols
 *    12-15; its top edge (row 12, cols 10-19) is literally the SAME cells as
 *    the main box's own bottom divider row, which spans the full (4,12)-
 *    (19,12) width using the divider-corner tiles 29/30 at its own ends.
 *    This box is the LEFTOVER real-cartridge START MENU widget still
 *    resident under the list (the real game's own Items screen sits on top
 *    of an already-drawn START menu) -- it is NOT this item screen's own
 *    cancel mechanism (that is the list's own trailing CANCEL row above);
 *    this shell keeps drawing it purely for visual parity with the real
 *    capture, not because it does anything here. It is not independently
 *    cursor-selectable on this shell either way.
 *  - The PC item store re-uses the IDENTICAL box/list routine over a
 *    different pocket (design doc's own citation: "ITEMLISTMENU again, over
 *    wNumBoxItems") -- not independently pixel-dumped in this slice (reaching
 *    a Pokemon Center PC needs real overworld navigation, out of this
 *    slice's time box); the geometry above is reused unchanged, gated only
 *    on which GbBagPocket backs the list.
 *  - SWAP (the game's SELECT-to-swap feature) is NOT bound to SELECT here --
 *    SELECT is the shell's own 1:1<->stretched toggle (design sec 1.5,
 *    required on every GB screen) -- it lives on the START menu instead,
 *    alongside ADD ITEM / REMOVE / TOSS. It is pick-source-then-destination,
 *    matching the real game: START > SWAP arms the row the cursor was on,
 *    the list is shown again, A on a different real row swaps the two and
 *    disarms, B disarms without leaving the screen. No real-cartridge
 *    capture exists for the armed source row's own marker glyph in this
 *    slice -- this shell re-uses the cursor tile (0xED) there, a documented
 *    deviation pending a real capture, not a verified fact.
 *  - Item names: SHIPPED (BACKLOG gbnames brief, supersedes BACKLOG #111's
 *    "locator slice" note above) -- gb_item_label() (source/gb_item_names.c)
 *    looks a real name up in an EMBEDDED identifier table (GREEN per
 *    docs/kb/licensing.md: id->name is a fact list, not decoded ROM prose;
 *    no ROM read, no locator) and falls back to "ITEM-n" ('-' because '#'
 *    has no Gen-1 glyph) only for a documented hole (an id with no real
 *    Gen-1 item, e.g. the unused SURFBOARD/ITEM_2C ids). TM/HM labels
 *    (HM01-HM05, TM01-TM50) are synthesized from the id, not tabled. The
 *    dump's own on-screen names ("TM05", "BICYCLE", ...) that this comment
 *    quoted as cell-layout ground truth are now the SAME strings the table
 *    ships.
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
