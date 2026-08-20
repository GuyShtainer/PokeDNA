#ifndef UI_H
#define UI_H

#include <tonc.h>
#include <stdbool.h>

/* Screen size, the footer row and the popup-fit arithmetic. Split out because that
 * header is pure C: the host text-fit test includes it and therefore measures the SAME
 * constants this code draws with. Do not re-declare them here. */
#include "ui_layout.h"

/* Bitmap (Mode 3) UI helper layer: filled panels, bordered boxes, colored text,
 * and a solid selection-highlight bar. Text still goes through libtonc TTE
 * (tte_write/tte_set_ink/tte_set_pos), but is rendered into the Mode 3 bitmap.
 * Initialise the mode + TTE in main (see ui_init). */

/* Palette (RGB15). Tuned for a dark, readable look. */
#define UI_BG       RGB15( 1,  2,  4)   /* screen background          */
#define UI_PANEL    RGB15( 2,  4,  9)   /* panel fill                 */
#define UI_BORDER   RGB15(10, 13, 20)   /* panel/divider border       */
#define UI_TEXT     RGB15(31, 31, 31)   /* primary text               */
#define UI_DIM      RGB15(17, 18, 21)   /* secondary/dim text         */
#define UI_TITLE    RGB15( 8, 28, 31)   /* headers (cyan)             */
#define UI_SEL      RGB15( 5, 10, 24)   /* selection highlight bar    */
#define UI_SELTEXT  RGB15(31, 31, 18)   /* text on the highlight bar  */
#define UI_OK       RGB15( 8, 28, 10)   /* good/green                 */
#define UI_WARN     RGB15(31, 18,  3)   /* warning/orange             */
#define UI_DIRCLR   RGB15(10, 24, 31)   /* directory rows (cyan)      */
#define UI_SAVECLR  RGB15(31, 31, 31)   /* save rows (white)          */

/* ---- retail party-screen palette (measured off Guy's own Emerald cartridge,
 * docs/analysis-2026-08-19-party/MEASUREMENTS.md — every value below is a
 * PIL.Image.getpixel() readout converted to RGB15, not eyeballed) ---------- */
#define UI_PTY_BG_A     RGB15(25, 26, 15)  /* background stripe, band A (206,214,123) */
#define UI_PTY_BG_B     RGB15(22, 22, 11)  /* background stripe, band B (181,181,90)  */
#define UI_PTY_BG_MARGIN RGB15(17, 19, 5)  /* flat left-margin column   (140,156,41)  */
#define UI_PTY_BOX_FILL_A RGB15(18, 28, 30)  /* slot-1 box interior, band A (approx)  */
#define UI_PTY_BOX_FILL_B RGB15(21, 29, 31)  /* slot-1 box interior, band B (approx)  */
#define UI_PTY_ROW_FILL_A RGB15(11, 21, 27)  /* list-row interior, band A (approx)    */
#define UI_PTY_ROW_FILL_B RGB15(16, 24, 27)  /* list-row interior, band B (approx)    */
#define UI_PTY_BORDER    RGB15( 9,  9, 12)  /* unselected box/row border  (74,74,99)  */
#define UI_PTY_CURSOR    RGB15(31, 14,  6)  /* selected box/row border   (255,115,49) */
#define UI_PTY_HP_OUTLINE RGB15(10, 10, 10) /* HP bar/label outline       (82,82,82)  */
#define UI_PTY_HP_HILITE RGB15(31, 31, 31)  /* HP bar 1px white highlight (255,255,255)*/
#define UI_PTY_HP_FILL   RGB15(14, 31, 21)  /* HP bar main body, GREEN    (115,255,173)*/
#define UI_PTY_HP_SHADE  RGB15(11, 26, 16)  /* HP bar top-row shading, GREEN (90,214,132)*/
/* YELLOW/RED bands: PROVISIONAL, not measured (Guy's save is entirely full-HP — see
 * MEASUREMENTS.md Differences #7). The THRESHOLDS that pick between these three pairs
 * ARE cited (pokeemerald's own GetHPBarLevel, src/battle_interface.c — see
 * party_draw_hp_fields, source/pdna_main.c); its literal RGB values live in a compiled
 * palette buffer copied at runtime (party_menu.c:750: `CpuCopy16(gPlttBufferUnfaded,
 * sPartyMenuInternal->palBuffer, ...)`), not a source-level constant, so they could not
 * be recovered from source alone. These follow the SAME main/shade lightness ratio as
 * the measured GREEN pair rather than an invented one. */
#define UI_PTY_HP_FILL_YEL  RGB15(31, 31, 10) /* PROVISIONAL yellow main  (255,255,82) */
#define UI_PTY_HP_SHADE_YEL RGB15(26, 26,  8) /* PROVISIONAL yellow shade (214,214,66) */
#define UI_PTY_HP_FILL_RED  RGB15(31, 10, 10) /* PROVISIONAL red main     (255,82,82)  */
#define UI_PTY_HP_SHADE_RED RGB15(26,  8,  8) /* PROVISIONAL red shade    (214,66,66)  */
#define UI_PTY_HP_TRACK  UI_DIM             /* empty portion — NOT MEASURED (this
                                              * save has no damaged party member) */
#define UI_PTY_HP_LABEL  RGB15(31, 22,  8)  /* "HP" badge text           (255,181,66) */
#define UI_PTY_TEXT_SHADOW RGB15(14, 14, 14) /* hard 1px text drop-shadow (115,115,115)*/
#define UI_PTY_GEND_M_FILL RGB15(20, 24, 31) /* male glyph fill          (165,198,255)*/
#define UI_PTY_GEND_M_LINE RGB15( 8,  8,  8) /* male glyph outline        (66,66,66)  */
#define UI_PTY_GEND_F_FILL RGB15(31, 19, 18) /* female glyph fill        (255,156,148)*/
#define UI_PTY_GEND_F_LINE RGB15(19,  8,  7) /* female glyph outline     (156,66,57)  */
#define UI_PTY_MSG_TEXT    RGB15( 3,  4,  6) /* message-box text, on its own white fill */
#define UI_PTY_CANCEL_FILL RGB15(14, 11, 22) /* CANCEL button fill        (115,90,181) */

/* ---- PC-box party PANEL palette — REBUILT 2026-08-20 against the ONLY unobstructed
 * retail frame (native-E12e-storage-partystrip.top.png, PIL.Image.getpixel() exact
 * reads, this UI is flat/dithered so edges are crisp — see pdna_layout.h's PDNA_PCP_*
 * comment for the geometry these pair with). A DIFFERENT retail screen from the
 * UI_PTY_* set above (that's the field-menu party list, START -> POKeMON). */
#define UI_PCP_PANEL_OUTER   RGB15(10, 12, 14)  /* bevel outer band     #556171 (85,97,113)  */
#define UI_PCP_PANEL_MID     RGB15(17, 21, 22)  /* bevel mid band       #8CA9B4 (140,169,180) */
#define UI_PCP_PANEL_HILITE  RGB15( 7, 13, 12)  /* left inner (highlight) #3B6863 (59,104,99) */
#define UI_PCP_PANEL_SHADOW  RGB15( 3,  9,  8)  /* right/bottom inner (shadow) #1F4842 (31,72,66) */
#define UI_PCP_FILL_A        RGB15(12, 21, 20)  /* teal dither, even scanline #64A8A4 (100,168,164) */
#define UI_PCP_FILL_B        RGB15(10, 17, 17)  /* teal dither, odd scanline  #50888B (80,136,139)  */
#define UI_PCP_TILE_BORDER   RGB15( 8, 10, 11)  /* every tile's border  #455159 (69,81,89)    */
#define UI_PCP_CURSOR        UI_PTY_CURSOR      /* selected-tile border — UNKNOWN in retail
                                                 * (this frame shows every tile with the SAME
                                                 * border colour, no captured frame shows a
                                                 * focused-but-unopened tile — see
                                                 * MEASUREMENTS.md "FOCUS/CURSOR INDICATOR");
                                                 * reuses the app's existing party-cursor
                                                 * recolour convention rather than inventing
                                                 * a new one. */
#define UI_PCP_CANCEL_BORDER RGB15(21, 26, 30)  /* CANCEL pill border   #AED1F3 (174,209,243) */
#define UI_PCP_CANCEL_BODY   RGB15(31, 31, 31)  /* CANCEL pill body (white highlight) #FEFBFF */
#define UI_PCP_CANCEL_GLYPH  RGB15(16, 23, 13)  /* CANCEL glyph, green  #83B86D (131,184,109) */

/* Switch to Mode 3 and init bitmap TTE with the fixed 8x8 system font. */
void ui_init(void);

/* Clear the whole screen to UI_BG. */
void ui_clear(void);

/* Filled rectangle with a 1px border. (x,y) top-left, w/h in pixels. */
void ui_panel(int x, int y, int w, int h, u16 fill, u16 border);

/* A horizontal divider line at pixel row y, from x..x+w. */
/* A panel whose fill is blended with what is already on screen: `num`/8 toward `fill`
 * (8 = opaque, 4 = half). Mode 3 has no hardware blend for a software-drawn panel, so this
 * mixes per pixel. */
void ui_panel_alpha(int x, int y, int w, int h, u16 fill, u16 border, int num);

/* Vertical layout for a list popup, so it can never collide with the footer.
 *
 * `head` is the chrome above the first row (title + divider) and `foot` the chrome
 * below the last one (the popup's own hint line + bottom border) — the same numbers
 * the caller already draws with. Returns how many of `nrows` rows FIT above
 * UI_FOOTER_Y: WINDOW the list at that count (the caller already knows how to scroll)
 * instead of letting the panel run off the bottom of the screen. out_y and out_h receive
 * the panel's top and height, centred in the space above the footer.
 *
 * A popup whose row count is a compile-time constant should instead assert its height
 * at build time (see nav_menu) — a fixed menu that cannot scroll must not silently
 * grow past the screen when someone adds an entry. */
int ui_popup_vfit(int nrows, int row_h, int head, int foot, int* out_y, int* out_h);
/* Same contract, laid out against a caller-supplied footer row instead of the global
 * UI_FOOTER_Y — see ui_layout.h's ui_popup_fit_at. */
int ui_popup_vfit_at(int nrows, int row_h, int head, int foot, int footer_y, int* out_y, int* out_h);

void ui_hline(int x, int y, int w, u16 color);

/* Draw text at pixel (x,y) in colour `ink`. */
void ui_text(int x, int y, u16 ink, const char* s);

/* Draw a list row of width `w` px at (x,y). When `selected`, paints a UI_SEL
 * bar behind it and uses UI_SELTEXT; otherwise draws the text in `ink`. */
void ui_text_sel(int x, int y, int w, bool selected, u16 ink, const char* s);

/* Blit a 16x16 icon at (x,y). Each pixel is a u16: 0 = transparent, otherwise
 * 0x8000 | RGB15 (the format emitted by tools/gen_icons.py). NULL is a no-op. */
void ui_icon16(int x, int y, const u16* icon);

/* Copy `in` into `out` clamped to `max_cols` display columns, UTF-8-safe
 * (never splits a codepoint); appends '~' as the last column if truncated.
 * `out` must hold at least max_cols*4 + 1 bytes to be safe. */
void ui_truncate(char* out, const char* in, int max_cols);

/* ---- proportional text (source/ui_font.c, generated by tools/gen_font.py) ----
 *
 * The default face is tonc's sys8: a fixed 8 px cell. A 76 px panel therefore holds
 * nine characters, which is why `JIGGLYPUFF` and `CHESTO BERRY` used to print as
 * `JIGGLYPU~` and `CHES~`. Retail Emerald prints both in that space because its font
 * is variable-width; `ui_ptext` is our equivalent (a 5x7 face of our own, ~6 px per
 * capital), for the places where a real name has to fit: item and species names, list
 * rows, description panes.
 *
 * Line pitch stays UI_ROW_H, so a proportional row drops into an 8 px row grid
 * unchanged. Draws transparently — only ink pixels are written. */

/* Width of `s` in pixels if drawn with ui_ptext. */
int ui_ptext_w(const char* s);

/* Draw `s` at (x,y) in `ink`; returns the x the next glyph would start at. */
int ui_ptext(int x, int y, u16 ink, const char* s);

/* Draw `s` clamped to `maxw` pixels, ending in '~' only if it genuinely does not
 * fit. Returns the width actually drawn. */
int ui_ptext_fit(int x, int y, int maxw, u16 ink, const char* s);

/* Right-align `s` so it ENDS at x=`right`. */
int ui_ptext_right(int right, int y, u16 ink, const char* s);

/* Same as ui_ptext/ui_ptext_fit, but with a HARD 1px drop shadow first — the shadow
 * glyph drawn at (x+1,y+1) in `shadow`, then the ink glyph at (x,y). A genuine
 * offset duplicate, not blended, matching retail's own text rendering (see
 * UI_PTY_TEXT_SHADOW). Returns the same width ui_ptext/ui_ptext_fit would. */
int ui_ptext_shadow(int x, int y, u16 ink, u16 shadow, const char* s);
int ui_ptext_fit_shadow(int x, int y, int maxw, u16 ink, u16 shadow, const char* s);

/* ---- TIGHT-SPACING variant: same face, glyph advance shrunk by UI_PTEXT_TIGHT_DELTA
 * (ui_font.h) px per glyph. A SEPARATE path from the four functions above, which keep
 * their exact original per-glyph advance for every existing caller — this is additive,
 * not a change to the shared one. Only the party screen's name text (row + slot-1 box,
 * source/pdna_main.c) uses it: PokeDNA's own proportional font runs ~23% wider per
 * glyph than retail's, so a full-length 10-char species name or Gen-3 nickname clips in
 * the column retail's narrower font fits, unless the column is either widened (not
 * allowed — it is measured off retail) or the text is kerned tighter (this). See
 * ui.c's ui_ptext_fit_shadow_tight / padv_tight for the exact arithmetic. */
int ui_ptext_w_tight(const char* s);
int ui_ptext_tight(int x, int y, u16 ink, const char* s);
int ui_ptext_fit_tight(int x, int y, int maxw, u16 ink, const char* s);
int ui_ptext_fit_shadow_tight(int x, int y, int maxw, u16 ink, u16 shadow, const char* s);

/* Fill the whole screen with a 1px horizontally-banded stripe (alternating `a`/`b`
 * every scanline) from x=`marginW`..239, and a FLAT `margin` column at x=0..marginW-1.
 * marginW=0 skips the flat column (pure stripe, edge to edge). */
void ui_stripe_bg(int marginW, u16 margin, u16 a, u16 b);

/* Same banded-stripe fill, but confined to one w×h rect (a panel's own interior) —
 * for the retail party box/rows, whose fill is the same per-scanline dither as the
 * background rather than one flat colour. Border drawn separately (ui_panel/ui_frame
 * pattern): this fills THEN the caller frames it, same order as ui_panel. */
void ui_panel_striped(int x, int y, int w, int h, u16 a, u16 b, u16 border);

/* A small (~9x9) coloured gender glyph — a circle with a diagonal tick (male) or a
 * circle with a cross (female) below it — NOT a text letter. `fill`/`line` are the
 * body colour and its dark outline. Top-left at (x,y). */
void ui_gender_glyph_m(int x, int y, u16 fill, u16 line);
void ui_gender_glyph_f(int x, int y, u16 fill, u16 line);

/* Word-wrap `s` into `maxw`-pixel lines at `line_h` pitch, at most `max_lines`
 * (0 = unlimited). Returns the number of lines drawn. Breaks on spaces; a single
 * word longer than the line is hard-split rather than dropped. */
int ui_ptext_wrap(int x, int y, int maxw, int line_h, int max_lines, u16 ink, const char* s);

/* Lines ui_ptext_wrap would need for `s` — measure before laying out a pane. */
int ui_ptext_wrap_lines(int maxw, const char* s);

/* Advance of one character, for callers doing their own layout. */
int ui_pchar_w(char c);

/* One wrap step: bytes of `s` that fit in `maxw` px (returned), and the bytes to
 * advance past (*skip — the same plus a consumed space). Lets a pager lay out
 * page-by-page with a different width on some rows. */
int ui_ptext_break(const char* s, int maxw, int* skip);

/* Solid filled rectangle (no border) — box wallpaper, stat cells. */
void ui_fill_rect(int x, int y, int w, int h, u16 color);

/* Outlined bar filled to `filled` px over a `track` background (EXP/IV/EV bars). */
void ui_progress(int x, int y, int w, int h, int filled, u16 fill, u16 track, u16 border);

/* Blit a w×h RGB15 sprite (0 transparent, 0x8000|RGB15 opaque) — e.g. 64×64 front sprite. */
void ui_sprite(int x, int y, int w, int h, const u16* data);

/* ---- artless fallbacks (original art only) --------------------------------
 * When the generated icon/badge art is absent (the zero-art build, or later the
 * no-ROM state of the ROM-gated build), screens draw these ORIGINAL chips instead
 * of leaving holes: a name chip where a mon icon would sit, and a coloured type
 * chip where a type badge would. Pure font + rects — nothing game-derived. */
void ui_name_chip(int x, int y, int w, int h, u16 bg, u16 ink, const char* name);
void ui_type_chip(int x, int y, int w, int h, uint8_t type_id);

/* Nearest-neighbour blit of a 32×32 (0x8000-keyed) icon at an arbitrary dst size. */
void ui_icon_scaled(int x, int y, int dw, int dh, const u16* src32);

/* Like ui_icon_scaled, but each opaque pixel is rendered in greyscale (luma) —
 * the Pokedex "not seen yet" state. No second asset: the grey is derived at blit
 * time from the colour icon (cheap shift-only luma). */
void ui_icon_scaled_grey(int x, int y, int dw, int dh, const u16* src32);

/* Tiny ~9×9 Poké Ball glyph (the Pokedex "caught" marker) with top-left at (x,y).
 * Procedural — no asset, one shared draw reused on every caught cell. */
void ui_pokeball(int x, int y);

/* Flicker-free sprite blit: composes each row (sprite over a SOLID `bg` colour) in
 * a buffer and DMA-copies it to VRAM in one pass — no separate erase step, so an
 * animating/moving sprite never blinks on the single-buffered Mode-3 display. Call
 * it right after VBlankIntrWait for small regions so the write lands in vblank.
 * Requires x and w to be EVEN (word-aligned DMA). NULL data is a no-op. */
void ui_blit_over(int x, int y, int w, int h, const u16* data, u16 bg);

/* Restoring a rect of a full-screen background bitmap used to live here as
 * ui_bg_restore(const u16*). The backgrounds now ship LZ77-paged (they were
 * 5.7 MB of a 12.5 MB ROM, which is why the image kept failing to load off
 * SD), so there is no pointer to index: use bg_restore(BgFrame, x, y, w, h)
 * from lzblob.h, which unpacks only the pages the rect overlaps. */

#endif /* UI_H */
