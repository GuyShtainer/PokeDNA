#ifndef PDNA_GBPACK_H
#define PDNA_GBPACK_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession                                            */

/* Gen-2's OWN Pack (item bag) / PC item store, on the shared GB-screen shell (U5,
 * BACKLOG #67, docs/GB-GAME-SCREENS-DESIGN.md sec 1.4, docs/briefs/U5-gen2-pack-brief.md).
 * Sibling of pdna_gbbag.c (Gen 1's own ITEM screen, U4) -- same shell, same overall
 * shape (a scrolling list + a trailing CANCEL row + a START menu for ADD/REMOVE/SWAP
 * + the memcmp-no-op/confirm/write/rollback commit contract), different real-cartridge
 * geometry (verified separately, see below) and pocket set (four real pockets plus
 * the PC store, not one).
 *
 * Ground truth (mGBA tools/gb_roundtrip.py's own GbDriver driving Gold.gbc + Gold.sav
 * and Crystal.gbc + Crystal.sav to START > PACK, dumping the BG tilemap at 0x9800 --
 * LCDC=0xE3: bit3=0 selects BG map 0x9800, bit4=0 selects SIGNED $8800 tile addressing
 * (same polarity as Gen 1's own bag); WY=144 means the WINDOW layer is OFF for this
 * screen -- Gen 2's Pack draws entirely on the BG layer, unlike Gen 1's window-layer
 * bag), for all four pockets (ITEMS/BALLS/KEY items/TM-HM, cycled with LEFT/RIGHT) on
 * Gold, cross-checked structurally identical on Crystal:
 *
 *  - Row 0 (the top strip) is a STATIC 20-tile run (packmenu-block-relative ids
 *    0x28-0x3B) that NEVER CHANGES, confirmed identical across all four pockets in
 *    every capture -- a real-cartridge quirk (Gold/Silver/Crystal's own Pack header
 *    graphic does not redraw when the pocket changes, only the small nameplate label
 *    at row 8 does), not a bug in this shell reproducing it verbatim.
 *  - Cols 0-4, rows 1-11: the pocket-picture COLUMN. Rows 1-2 and 6, 10-11 are a flat
 *    filler tile (packmenu-relative 0x24); rows 3-5 are the pocket's own picture (15
 *    tiles, PackGFX/RomGbUi.pack_m, ROM order KEY/ITEMS/TM-HM/BALLS per the design
 *    doc's own citation -- NOT the UI order ITEMS->BALLS->KEY->TM/HM); rows 7 and 9
 *    are a small nameplate box's own top/bottom border (packmenu-relative
 *    0x00,0x04,0x04,0x04,0x01 and 0x02,0x05,0x05,0x05,0x03, both pocket-invariant);
 *    row 8 is the ONLY per-pocket cell run in this whole column -- a pre-rendered
 *    label graphic (not FONT text; verified by tile-id shape, not character glyphs):
 *    ITEMS 0x06-0x0A, BALLS 0x15-0x19, KEY items 0x0B-0x0F, TM/HM 0x10-0x14.
 *  - Cols 5-19, rows 0-11: NO drawn border/frame at all (confirmed against the real
 *    screenshot -- items float directly on the flat background colour; only the
 *    description box below has a visible frame) -- col 7 is the cursor column (FONT
 *    0xED on the selected row, blank otherwise), col 8 is where item-name text
 *    starts. Item rows are PAIRED exactly like Gen 1's bag: a name row then a
 *    quantity row directly below it -- name_row(slot) = 2 + 2*slot, qty_row(slot) =
 *    name_row(slot) + 1, for slot 0..4 (FIVE visible rows, not Gen 1's four).
 *  - Quantity: '×' (FONT 0xF1) at col 17, then a right-aligned 1-2 digit count at
 *    cols 18-19 (col 18 blank for a single-digit count) -- present on Items/Balls/
 *    TM-HM rows, ABSENT (both cols blank) on every Key-items row (that pocket has no
 *    per-entry quantity at all, matching gb_bag.h's own GBB_POCKET_KEY contract) and
 *    on the trailing CANCEL row.
 *  - TM/HM ONLY: cols 5-6 on the name row hold the TM/HM's own two-digit number
 *    (FONT digit tiles, right-aligned, e.g. "01" for TM01) BEFORE the cursor column --
 *    confirmed live (Gold's TM/HM pocket opened directly on "01 DYNAMICPUNCH").
 *  - The list is `count` real entries PLUS a trailing, cursor-selectable CANCEL row
 *    (index == count) -- confirmed live: Gold's BALLS pocket (4 real balls) showed
 *    CANCEL as its own 5th row with its quantity cells BLANK, matching Gen 1's own
 *    "count+1, CANCEL has no qty" shape; this CORRECTS an earlier draft of the design
 *    doc's own guess ("the Gen-2 pack has no CANCEL row") -- the real cartridge DOES
 *    show one, this shell reproduces it the same way pdna_gbbag.c's own g1bag does.
 *  - Scrolling was NOT independently re-derived this slice (every pocket captured fit
 *    within the 5 visible rows without scrolling) -- gbbag_clamp_scroll()'s own
 *    clamp-and-pin-at-slot-2 behaviour (verified for Gen 1's own 4-visible-row list)
 *    is reused here at 5 visible rows, a documented ASSUMPTION pending a real
 *    capture of a pocket with 6+ entries, not an independently confirmed fact.
 *  - The description box (rows 12-17) uses the SAME textbox-relative tile ids as
 *    Gen 1's own EXIT box (25-31 relative to the shell's TEXTBOX/frames block base --
 *    confirmed identical absolute VRAM ids 0x79-0x7E in this capture) -- but its own
 *    TEXT (the item's description, two lines) is NOT located this slice (the
 *    description-pointer table locator was time-boxed, per the brief's own
 *    permission) -- the box is drawn EMPTY (its frame only), a documented, honest
 *    deviation from the real screen's own two lines of prose, not a crash/dead end.
 *  - Item names: NOT located this slice either (same time-box posture as Gen 1's own
 *    ItemNames table, U4) -- every item prints "ITEM-n" (n = the raw id byte),
 *    exactly Gen 1's own posture; the real names quoted above ("FIRE STONE",
 *    "MASTER BALL", ...) are cited purely as ground truth for the CELL LAYOUT, never
 *    shipped as data.
 *  - Per-item pocket membership (which pocket a given item id legally belongs to) was
 *    NOT located this slice (time-boxed, per the brief's own fallback permission) --
 *    ADD ITEM works ONLY from the Items pocket (the brief's own sanctioned "fall back
 *    to Items only" rule); START > ADD ITEM on Balls or Key items refuses outright
 *    ("WRONG POCKET") rather than silently accepting an id this core cannot verify
 *    belongs there. TM/HM has no ADD ITEM at all -- its own separate count-array
 *    editor (A on a row) is the only way to change it, see below.
 *  - Kris's own PackFGFX (pack_f) is not painted this slice -- pdna_gbscreen.h's own
 *    GBSCR_SRC_PACK_M always uses pack_m (Chris's pack), even for a Crystal save with
 *    a female trainer -- a documented, cosmetic-only simplification.
 *
 * `s` must already be open (gbs_open, Gen-2 only -- gbb_field_present() gates every
 * pocket this game lacks, matching Red's own screen's posture, though every Gen-2
 * game has every pocket this screen shows). `can_edit`: true for the editable
 * session, false for a view-only visit. Falls back to a plain PokeDNA rows page
 * (BACKLOG #67's own "no dead end" rule) when the GB-screen shell itself refuses --
 * same posture as pdna_gbbag()'s own fallback. */
void pdna_gbpack(GbSession* s, bool can_edit);

#endif /* PDNA_GBPACK_H */
