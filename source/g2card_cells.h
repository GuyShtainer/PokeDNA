#ifndef G2CARD_CELLS_H
#define G2CARD_CELLS_H

#include <stdint.h>
#include <stdbool.h>
#include "pdna_gbscreen.h"   /* GbScrSrc -- pure header, see its own top-of-file note */

/* BACKLOG #96 D11 -- the Gen-2 trainer card's STATIC graphical cell set,
 * lifted out of g2card_paint_upper/page1/page2 (source/pdna_gbtrainer.c:
 * 811-907) into a pure, host-compilable table the same way gbtr_rows.c
 * lifted row visibility (BACKLOG #49). Only cells that draw a FIXED tile out
 * of a located ROM block (CARDGFX/CARDPIC_M/CARDPIC_F/STATUSWORD/LEADERS/
 * BADGES/FONT) are here -- gbscr_cell() calls with a compile-time-known
 * {x,y,src,index}, enumerated over their loop's own possibility space where
 * the painter used a loop (the pic grid, the diploma grid, the per-badge
 * overlay). NOT here: gbscr_text()/gbscr_raw() cells (NAME, ID number,
 * MONEY, DEX count, PLAY TIME digits, the BADGES caption -- data-dependent
 * tile indices, no fixed fact to pin) and the blinking PLAY TIME colon cell
 * (STATUSWORD-vs-BLANK toggling on g2_frame_ctr -- time-dependent, not a
 * fixed cell). g2card_border()'s own cells (source/pdna_gbtrainer.c:780-806)
 * are also out of scope -- the brief's line range (811-888) is
 * paint_upper/page1/page2 only.
 *
 * tests/host_gbcard_cells_test.c asserts every cell this file can emit
 * resolves inside its located block's byte length (pdna_gbscreen.c's
 * gbscr_block_off()/gbscr_block_bytes()) and pins the facts BACKLOG #96/#125
 * name: on Gold the (18,9) corner cell is pic_src display index 4 (pixel-
 * exact -- Gold has no separate corner block); on Crystal both (18,1) and
 * (18,9) resolve through GBSCR_SRC_CARDCORNER index 0 (BACKLOG #125, fixed);
 * STATUSWORD is located at leaders-96 (gbscr_block_off()'s own derivation);
 * and G2L_BADGES_WORD (pdna_gbtrainer.c:770) is 80.
 */

typedef struct {
  uint8_t  x, y;
  GbScrSrc src;
  uint8_t  index;
} G2CardCell;

/* U3 re-anchor (found by byte-searching the ROM for each tile's own REAL VRAM
 * pattern): CARDGFX's 6 tiles (border/body fill, notch, divider fill, divider
 * cap, "ID", "No"); STATUSWORD's 6 tiles (5 "STATUS" glyphs + the play-time
 * colon, GBSCR_SRC_STATUSWORD anchored at gbscr_block_off()/_bytes()) --
 * moved here from pdna_gbtrainer.c (BACKLOG #96 D11) so g2card_cells.c and
 * the painter share one source of truth. */
enum { G2G_FILL = 0, G2G_NOTCH = 1, G2G_DIVFILL = 2, G2G_DIVCAP = 3, G2G_ID = 4, G2G_NO = 5 };
enum { G2X_STATUS0 = 0, G2X_COLON = 5 };
/* The card lists the 8 leaders in GYM order (row-major k=0..7), but the badge
 * byte's bits are in BADGE order -- confirmed against the real cart
 * (Gold_bit4.sav / Gold_bit5.sav). Index by face k to get the real badge bit. */
extern const uint8_t kG2BadgeBit[8];
/* LEADERS-block-relative index where the "BADGES" word graphic starts (page 2
 * row 8) -- 8 faces * 10 tiles = 80, LEADERS' own declared size is 86. */
enum { G2L_BADGES_WORD = 80 };

/* g2card_build_upper_cells(): CARDGFX "ID"/"No" glyphs, the 5x7 pic grid
 * (35 cells, including the (18,1) corner cell) + the (18,9) corner cell, and
 * the divider fill (12) + cap (1). `female` picks CARDPIC_F vs CARDPIC_M,
 * same parameter g2card_paint_upper() takes. `has_corner` (the caller's
 * RomGbUi.cardcorner != 0 -- true on Crystal, false on Gold) routes BOTH
 * right-corner cells ((18,1) and (18,9)) through GBSCR_SRC_CARDCORNER index 0
 * instead of pic_src index 4 -- BACKLOG #125. Always emits exactly
 * G2CARD_UPPER_CELLS cells (the corner routing overrides in place, it never
 * appends); `out` must hold that many. Returns the count (always
 * G2CARD_UPPER_CELLS) for call-site symmetry with the page builders below. */
#define G2CARD_UPPER_CELLS 51
int g2card_build_upper_cells(bool female, bool has_corner, G2CardCell out[G2CARD_UPPER_CELLS]);

/* g2card_build_page1_cells(): the 5 STATUSWORD "STATUS" tiles + the FONT
 * (r) hint arrow. Always emits exactly G2CARD_PAGE1_CELLS cells. */
#define G2CARD_PAGE1_CELLS 6
int g2card_build_page1_cells(G2CardCell out[G2CARD_PAGE1_CELLS]);

/* g2card_build_page2_cells(): the 5 LEADERS "BADGES" word tiles, the 8-leader
 * diploma grid (10 cells each = 80), and -- for every k in 0..7 where
 * badge_owned[k] is true -- the 2x2 BADGES overlay over that leader's face
 * (4 more cells each). `badge_owned` may be NULL (no overlay cells, same as
 * every badge unowned). Writes at most G2CARD_PAGE2_CELLS_MAX cells; `out`
 * must hold that many. Returns the actual count written (85 + 4*owned_count,
 * so 85..117). */
#define G2CARD_PAGE2_CELLS_MAX 117
int g2card_build_page2_cells(const bool badge_owned[8], G2CardCell out[G2CARD_PAGE2_CELLS_MAX]);

#endif /* G2CARD_CELLS_H */
