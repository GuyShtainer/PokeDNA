#ifndef PDNA_LAYOUT_H
#define PDNA_LAYOUT_H

/* Screen layout constants and fixed on-screen strings that a HOST TEST has to see.
 *
 * WHY THIS FILE EXISTS: PokeDNA's fonts clip silently. A string one pixel too wide just
 * loses its tail on hardware ("JIGGLYPU~"), a popup four pixels too tall lands on the
 * screen's own footer row and the two strings blend into garbage ("A pick ABmback UP SEL
 * B"), and a highlight bar two pixels short strikes through the row it is meant to
 * contain. tests/host_textfit_test.c catches all three at build time — but only if it
 * measures the SAME numbers the screens draw with. When the test re-typed them as its
 * own literals it measured its own copy, stayed green when pdna_main.c changed, and gave
 * false confidence. So every number a test asserts on lives HERE, the screen code reads
 * it from here, and the test includes this file.
 *
 * Pure C on purpose (no tonc, no libc): the host test compiles it as-is. Anything that
 * needs a GBA type or a colour belongs in the .c file, not here.
 *
 * Rule of thumb for adding to this file: if changing the value could push ink off a
 * screen, off a panel, or onto another string, it belongs here. Pure cosmetics
 * (colours, gaps with slack on both sides) do not. */

/* ---------------------------------------------------------------------------
 * START menu over the box screen (nav_menu, source/pdna_main.c)
 *
 * FIXED height and drawn exactly ONCE, so it cannot window or scroll: if the entry
 * list outgrows the screen the menu simply hangs off the bottom. That shipped — adding
 * the GB-import entry made 19 entries = 10 rows at a 13 px pitch = a 164 px panel on a
 * 160 px screen, with the hint row landing on the box screen's own footer. pdna_main.c
 * turns that into a BUILD failure (_Static_assert on PDNA_NAV_MH) and the host test
 * checks the same sum plus the label widths.
 *
 * The list is an X-macro so the enum, the drawn labels and the test all come from ONE
 * place: X(enum_id, label). Add an entry here and nowhere else. */
#define PDNA_NAV_ITEMS(X)                       \
  X(NV_PARTY,      "Party")                     \
  X(NV_BANK,       "Bank")                      \
  X(NV_DAYCARE,    "Daycare")                   \
  X(NV_TRAINER,    "Trainer")                   \
  X(NV_CLOCK,      "Clock fix")                 \
  X(NV_MIRAGE,     "Mirage")                    \
  X(NV_DEX,        "Pokedex")                   \
  X(NV_BAG,        "Bag")                       \
  X(NV_DATA,       "Flags & counters")          \
  X(NV_SECRET,     "Bases")                     \
  X(NV_POKEBLOCK,  "Blocks")                    \
  X(NV_EVENTS,     "Tickets")                   \
  X(NV_BATTLEREC,  "Records")                   \
  X(NV_FRONTIER,   "Frontier")                  \
  X(NV_FLY,        "Fly")                       \
  /* Present in the emulator build too: when a Pokemon ROM has been fused into this   */ \
  /* image (tools/fuse_rom.py) the map reads it from cartridge space, no SD needed.   */ \
  X(NV_MAP,        "Map")                       \
  X(NV_GB,         "GB import")   /* import from a Game Boy (Gen 1/2) save on the card */ \
  X(NV_SETTINGS,   "Settings")                  \
  X(NV_BACK,       "Back")

#define PDNA_NAV_COUNT_ONE(id, label) +1
#define PDNA_NAV_COUNT (0 PDNA_NAV_ITEMS(PDNA_NAV_COUNT_ONE))   /* == NV_COUNT */

/* The column is wide enough for "Flags & counters" (85 px in the proportional face) and
 * two of them plus the gutter still clear the 240 px screen. */
#define PDNA_NAV_COL_W   98
#define PDNA_NAV_BAND_W  (PDNA_NAV_COL_W - 4)   /* selection bar width               */
#define PDNA_NAV_BAND_H  12                     /* bar height: contains the glyph box */
#define PDNA_NAV_LABEL_DX 4                     /* label inset inside the bar         */
/* A label is drawn PDNA_NAV_LABEL_DX into the bar, so this — not the screen — is its
 * budget. A wider label runs out past the highlight and into the second column. */
#define PDNA_NAV_LABEL_W (PDNA_NAV_BAND_W - PDNA_NAV_LABEL_DX)

#define PDNA_NAV_ROW_H   11
#define PDNA_NAV_ROWS    ((PDNA_NAV_COUNT + 1) / 2)   /* two columns  */
#define PDNA_NAV_HEAD    20                     /* title + divider above row 0        */
#define PDNA_NAV_FOOT    14                     /* hint line + bottom border          */
/* nav_menu draws row i of a column at my + PDNA_NAV_HEAD + i * PDNA_NAV_ROW_H, and the
 * selection bar PDNA_NAV_BAND_DY above that. Those three sites used to spell the head as
 * a bare 20 while PDNA_NAV_HEAD fed only PDNA_NAV_MH — so shrinking the constant shrank
 * the PANEL and left the rows where they were, silently, with every test still green.
 * They read the constant now, which is also what makes the two checks below real. */
#define PDNA_NAV_BAND_DY (-2)                   /* bar top, relative to the text row   */
#define PDNA_NAV_PAD      6                     /* title / label / hint inset from mx  */
#define PDNA_NAV_TITLE_DY 4                     /* "MENU" baseline below the panel top */
#define PDNA_NAV_DIV_DY  15                     /* the rule under the title            */
#define PDNA_NAV_MH      (PDNA_NAV_HEAD + PDNA_NAV_ROWS * PDNA_NAV_ROW_H + PDNA_NAV_FOOT)
#define PDNA_NAV_MW      (PDNA_NAV_COL_W * 2 + 12)
#define PDNA_NAV_HINT_DY (-10)                  /* hint baseline rel. to panel bottom */
#define PDNA_NAV_HINT    "A pick  B back"

/* ---------------------------------------------------------------------------
 * Per-mon action popup (app_mon_menu / app_mon_menu_readonly, source/pdna_main.c)
 *
 * Drawn over the box or party screen while THAT screen's hints are still on the bottom
 * row, so it is laid out with ui_popup_fit above UI_FOOTER_Y and windowed if the action
 * list ever stops fitting. */
#define PDNA_MONMENU_MAX    18                  /* longest possible action list       */
#define PDNA_MONMENU_ROW_H  13
#define PDNA_MONMENU_HEAD   18                  /* title + divider                    */
#define PDNA_MONMENU_FOOT   11                  /* "A ok B back" + bottom border      */
#define PDNA_MONMENU_X     138
#define PDNA_MONMENU_W     100
#define PDNA_MONMENU_ROW_DX 10                  /* row text inset (sys8)              */
#define PDNA_MONMENU_PAD     6                  /* title / prose / hint inset         */
/* Right edge of the panel's ink: x + w - 2 is the border column, so a row drawn at
 * X + ROW_DX has this much room. */
#define PDNA_MONMENU_ROW_W  (PDNA_MONMENU_W - PDNA_MONMENU_ROW_DX - 2)
/* The read-only popup's two prose lines are ui_ptext_fit'd to this. */
#define PDNA_MONMENU_PROSE_W (PDNA_MONMENU_W - 2 * PDNA_MONMENU_PAD)
#define PDNA_MONMENU_FOOT_TXT "A ok B back"     /* 100 px panel: 12 sys8 columns max  */
/* Baseline of a popup's own hint line, relative to the panel's BOTTOM (my + mh). The
 * hint inks UI_ROW_H rows and ui_panel's bottom rule sits at my + mh - 2, so -9 is the
 * last offset that keeps the text's final row off that rule. Shared by every popup that
 * carries a hint (app_mon_menu, the read-only menu, dex_bulk). */
#define PDNA_POPUP_HINT_DY   (-9)

#define PDNA_LBL_VIEW_EDIT   "VIEW / EDIT"
#define PDNA_LBL_ITEM        "ITEM"
#define PDNA_LBL_LEGALITY    "LEGALITY"
#define PDNA_LBL_HATCH       "HATCH"
#define PDNA_LBL_MOVE        "MOVE"
#define PDNA_LBL_MOVE_TO_BOX "MOVE TO BOX"
#define PDNA_LBL_COPY        "COPY"
#define PDNA_LBL_PASTE       "PASTE"
#define PDNA_LBL_DUPLICATE   "DUPLICATE"
#define PDNA_LBL_TO_DAYCARE  "TO DAY-CARE"
#define PDNA_LBL_TO_GAME     "TO GAME"
#define PDNA_LBL_EXPORT_PK   "EXPORT .pk"
#define PDNA_LBL_TAKE_ITEM   "TAKE ITEM"
#define PDNA_LBL_GIVE_ITEM   "GIVE ITEM"
#define PDNA_LBL_RELEASE     "RELEASE"
#define PDNA_LBL_CREATE      "CREATE"
#define PDNA_LBL_PASTE_HERE  "PASTE HERE"
#define PDNA_LBL_CANCEL      "CANCEL"
#define PDNA_LBL_VIEW        "VIEW"           /* read-only popup only */
#define PDNA_LBL_EDIT_GB     "EDIT"           /* read-only popup, GB session only */
#define PDNA_LBL_MOVE_TO     "MOVE TO"        /* read-only popup, GB session S3 only */
#define PDNA_LBL_PASTE_GB    "PASTE (GB)"     /* read-only popup, empty GB cell, S5-B */

/* Every label either action popup can show, so the host test measures the strings the
 * menus actually draw. The X() entries are the macros above, not fresh literals. */
#define PDNA_MONMENU_LABELS(X)                                                        \
  X(PDNA_LBL_VIEW_EDIT) X(PDNA_LBL_ITEM) X(PDNA_LBL_LEGALITY) X(PDNA_LBL_HATCH)       \
  X(PDNA_LBL_MOVE) X(PDNA_LBL_MOVE_TO_BOX) X(PDNA_LBL_COPY) X(PDNA_LBL_PASTE)         \
  X(PDNA_LBL_DUPLICATE) X(PDNA_LBL_TO_DAYCARE) X(PDNA_LBL_TO_GAME)                    \
  X(PDNA_LBL_EXPORT_PK) X(PDNA_LBL_TAKE_ITEM) X(PDNA_LBL_GIVE_ITEM)                   \
  X(PDNA_LBL_RELEASE) X(PDNA_LBL_CREATE) X(PDNA_LBL_PASTE_HERE) X(PDNA_LBL_CANCEL)    \
  X(PDNA_LBL_VIEW) X(PDNA_LBL_EDIT_GB) X(PDNA_LBL_MOVE_TO) X(PDNA_LBL_PASTE_GB)

/* Read-only source popup: the header grows by one line per explanatory line above the
 * rows (the source's note, and the per-record "why this one is locked"). */
#define PDNA_ROMENU_MAX       7               /* VIEW, EDIT?, MOVE TO?, RELEASE?, LEGALITY, COPY?, CANCEL */
#define PDNA_ROMENU_HDR       15              /* title only                           */
#define PDNA_ROMENU_LINE      10              /* each optional prose line             */
#define PDNA_ROMENU_HEAD_PAD   3              /* divider -> first row                 */

/* ---------------------------------------------------------------------------
 * Summary screen (source/pdna_summary.c). The card column starts at x=98 and the screen
 * is 240 wide. The IVs card's six stat rows end at y=98 and TOTAL is drawn at y=100, so
 * y=108 is the first free scanline and the note row must finish above the footer rule at
 * y=151. */
#define PDNA_SUM_CARD_X       98
#define PDNA_SUM_CARD_W      138
#define PDNA_SUM_ROLL_Y      116
#define PDNA_SUM_ROLL_BOX_DY  (-2)
#define PDNA_SUM_ROLL_BOX_H   12
#define PDNA_SUM_NOTE_Y      130
#define PDNA_SUM_FOOTER_Y    152
/* IVH_CAP LIVES HERE, not in gen3_ivroll.h: tests/host_textfit_test.c includes only
 * ui_font.h / ui_layout.h / pdna_layout.h / rmbl.h, and referencing IVH_CAP from the other
 * header made the whole 36-test suite fail to build. pdna_layout.h is pure C with no tonc
 * and no libc, so the pure-C rule survives. */
#define IVH_CAP               16
/* BATTLE MOVES rows. The PP cell is 60 px wide at card_x + 78 and now has to carry the
 * PP-Up count as well, so it is drawn PROPORTIONALLY (ui_ptext_fit): "PP35/35 +3" is 10
 * fixed-width columns = 80 px and would have run off the card. Worst realistic case is
 * base 40 with 3 Ups -> "PP64/64 +3"; a corrupt PP byte can print wider and ui_ptext_fit
 * clips it with a visible '~'. */
#define PDNA_SUM_PP_X_DX      78
#define PDNA_SUM_PP_W         60
#define PDNA_SUM_PP_FMT      "PP%u/%u"
#define PDNA_SUM_PP_UPS_FMT  "PP%u/%u +%u"
/* ORIGIN / MET card. It grew a Region row above Loc — region scopes the place list, so
 * it reads above the place it scopes — which pushed the whole tail (OT / TID / SID /
 * Pokerus) down 11 px. That card had no named geometry at all and its rows were bare
 * numbers, so nothing would have noticed the tail walking into the note row; these are
 * the numbers card_origin now draws with and the host test measures. */
#define PDNA_SUM_ORG_Y0        14
#define PDNA_SUM_ORG_HDR_DY    13             /* first row, below the card header      */
#define PDNA_SUM_ORG_ROW_H     11             /* Ball / Met Lv / Region / Loc / Origin */
#define PDNA_SUM_ORG_ROWS       5
#define PDNA_SUM_ORG_GAP       14             /* under Origin, before the OT block     */
#define PDNA_SUM_ORG_TAIL_H     9             /* OT / TID / SID / Pokerus              */
#define PDNA_SUM_ORG_TAIL_ROWS  4
#define PDNA_SUM_ORG_VAL_DX    60             /* value column, from the card's x       */
#define PDNA_SUM_ORG_RIGHT    238             /* right ink margin for a value          */
#define PDNA_SUM_ORG_REGION_LBL "Region"
/* y of the last row's ink, which must stay above the note row. */
#define PDNA_SUM_ORG_LAST_Y  (PDNA_SUM_ORG_Y0 + PDNA_SUM_ORG_HDR_DY + \
                              (PDNA_SUM_ORG_ROWS - 1) * PDNA_SUM_ORG_ROW_H + \
                              PDNA_SUM_ORG_GAP + \
                              (PDNA_SUM_ORG_TAIL_ROWS - 1) * PDNA_SUM_ORG_TAIL_H)
#define PDNA_SUM_ROLL_LBL    "Reroll IVs"
#define PDNA_SUM_ARROW_L     "<"
#define PDNA_SUM_ARROW_R     ">"
#define PDNA_SUM_ROLLING     "Rolling..."
#define PDNA_SUM_NOTE_FMT    "Roll %d/%d  %s"
#define PDNA_SUM_TAIL_ORIG   "the original"
#define PDNA_SUM_TAIL_IVONLY "IVs only"
#define PDNA_SUM_TAIL_PIDIV  "PID + IVs"
#define PDNA_SUM_NOTE_IDLE   "A = roll a new IV spread"
#define PDNA_SUM_NOTE_VIEW   "Edit mode: A rolls IVs"
#define PDNA_SUM_NOTE_BADEGG "Bad egg - cannot roll"
/* Hint rows. sys8 is a FIXED 8 px cell drawn from x=4, so 29 columns is the budget
 * (4 + 29*8 = 236). The reroll line is exactly at it — measure before editing. */
#define PDNA_SUM_FOOT_EDIT        "A list  <>edit  U/D  L/R  B"
#define PDNA_SUM_FOOT_CREATE_EDIT "A list  <>edit  L/R  START"
#define PDNA_SUM_FOOT_CREATE      "A edit  L/R card  START keep"
#define PDNA_SUM_FOOT_VIEW        "A edit  U/D mon  L/R  SEL  B"
#define PDNA_SUM_FOOT_RO          "U/D mon  L/R card  SEL  B"
#define PDNA_SUM_FOOT_REROLL      "A reroll  <>undo/redo  U/D  B"
#define PDNA_SUM_FOOTS(X) \
  X(PDNA_SUM_FOOT_EDIT) X(PDNA_SUM_FOOT_CREATE_EDIT) X(PDNA_SUM_FOOT_CREATE) \
  X(PDNA_SUM_FOOT_VIEW) X(PDNA_SUM_FOOT_RO) X(PDNA_SUM_FOOT_REROLL)
/* Confirm panel. x=96 (NOT 16): at x=16 it covered the 64x64 portrait at x=12..79, while
 * its own comment claimed the user could still see which Pokemon it was about. */
#define PDNA_SUM_RC_X         96
#define PDNA_SUM_RC_Y         34
#define PDNA_SUM_RC_W        136
#define PDNA_SUM_RC_PAD        8
#define PDNA_SUM_RC_LINES      5
#define PDNA_SUM_RC_ROW_H     10
#define PDNA_SUM_RC_HEAD      24
#define PDNA_SUM_RC_FOOT      22
#define PDNA_SUM_RC_H (PDNA_SUM_RC_HEAD + PDNA_SUM_RC_LINES * PDNA_SUM_RC_ROW_H + PDNA_SUM_RC_FOOT)
#define PDNA_SUM_RC_TEXT_W (PDNA_SUM_RC_W - 2 * PDNA_SUM_RC_PAD)
#define PDNA_SUM_RC_TITLE     "PID moves"
#define PDNA_SUM_RC_HINT      "A ok   B cancel"
#define PDNA_SUM_RC_SHINY_ON  "Becomes SHINY"
#define PDNA_SUM_RC_SHINY_OFF "Not SHINY now"
#define PDNA_SUM_RC_NAT_FMT   "Nat %s>%s"
#define PDNA_SUM_RC_SEX_FMT   "Sex %s>%s"
#define PDNA_SUM_RC_ABI_FMT   "Abil %u>%u"
#define PDNA_SUM_RC_UNO_FMT   "Unown %c>%c"
#define PDNA_SUM_RC_NAT_LONGEST "ADAMANT"
/* An NPC-trade Pokemon's PID and IVs are FIXED in the trade template (src/trade.c:4570),
 * so NO reroll outcome keeps its provenance — there is no seed that reproduces a value
 * the game never drew from an RNG. PokeDNA's own checker exempts met 0xFE from the PID/IV
 * search and so will stay quiet, which is precisely why this has to be SAID: the mon
 * leaves the reroll no longer being the Pokemon that trade hands out, and a checker that
 * carries the fixed-PID table will see it. It is the first line on the panel, and it is
 * why a trade mon always gets the panel even when everything else was preserved.
 * 14 glyphs x 8 px = 112, inside PDNA_SUM_RC_TEXT_W = 120. */
#define PDNA_SUM_RC_TRADE     "Trade PID lost"

/* ---------------------------------------------------------------------------
 * Pokedex SELECT-all popup (dex_bulk, source/pdna_pick.c) */
#define PDNA_DEXBULK_MAX     6                /* Catch/See/Wipe/Natl/Undo/Cancel      */
#define PDNA_DEXBULK_ROW_H  14
#define PDNA_DEXBULK_HEAD   18
#define PDNA_DEXBULK_FOOT   11

/* ---------------------------------------------------------------------------
 * Day-care popups (dc_menu / dc_withdraw, source/pdna_main.c) */
#define PDNA_DCPOP_MAX       4                /* View/Take out/Put in/Cancel          */
#define PDNA_DCPOP_ROW_H    14
#define PDNA_DCPOP_HEAD     18
#define PDNA_DCPOP_FOOT      8

/* ---------------------------------------------------------------------------
 * Day-care yard: the three-row status panel (source/pdna_main.c)
 *
 * The panel used to be 124..151, whose bottom border row (150) fell exactly on row 2's
 * DESCENDER row — "Others are just visiting." lost the tails of its 'j' and 'g'. It is
 * 122..151 now with the rows lifted 1 px. ui_ptext inks UI_FONT_CELL_H rows, so the
 * last row must still end above the border. */
#define PDNA_DCY_PANEL_X     2
#define PDNA_DCY_PANEL_Y   122
#define PDNA_DCY_PANEL_W   236
#define PDNA_DCY_PANEL_H    30
#define PDNA_DCY_TEXT_X      6
#define PDNA_DCY_ROW0_Y    124
#define PDNA_DCY_ROW_PITCH   9
/* Ink budget for a status line: up to the panel's right border column. */
#define PDNA_DCY_TEXT_W  (PDNA_DCY_PANEL_X + PDNA_DCY_PANEL_W - 2 - PDNA_DCY_TEXT_X)
#define PDNA_DCY_NAME_W    228                /* the single-boarder ui_ptext_fit clamp */
#define PDNA_DCY_FOOTER_Y  152                /* the yard's own hint row               */
#define PDNA_DCY_HINT_PAIR "A menu  L/R your 2  B back"
#define PDNA_DCY_HINT_PUT  "A put in  B back"

/* ---------------------------------------------------------------------------
 * FILTER / SORT lists (source/pdna_pick.c)
 *
 * The species/dex lists run at a 9 px pitch with a BORDERLESS selection bar. They used
 * to run at 8 px with ui_panel(2, y-1, .., 9): ui_panel's frame puts its bottom line at
 * y+7, INSIDE the 8-row glyph box, so the rule ran through the feet of "Sort: No. (dex)"
 * and its fill ended one pixel above the next row's ascenders. Two invariants:
 *   the bar must CONTAIN the glyph box  (BAR_H - 2 >= UI_FONT_CELL_H - 1), and
 *   it must not touch the next row's ink (BAR_H - 2 <= ROW_H - 2). */
#define PDNA_FILT_Y0        14
#define PDNA_FILT_ROW_H      9
#define PDNA_FILT_VIS       15                /* rows in the window                   */
#define PDNA_FILT_BAR_X      2
#define PDNA_FILT_BAR_W    236
#define PDNA_FILT_BAR_DY   (-1)               /* bar top, relative to the text row     */
#define PDNA_FILT_BAR_H      9
/* Where a row's text starts. filter_menu / dex_menu / item_filter_menu spelled this as a
 * bare 8 at eight call sites while the constant sat here read by nobody — a decoy that
 * looked like coverage. They read it now, so the two width checks below are real. */
#define PDNA_FILT_TEXT_X     8
/* A type row shows a colour chip first and the filter name after it. type_chip() paints
 * PDNA_FILT_CHIP_W px from PDNA_FILT_TEXT_X; the name starts PDNA_FILT_CHIP_DX along, so
 * the gap between them is CHIP_DX - CHIP_W and must not go negative. */
#define PDNA_FILT_CHIP_W    26
#define PDNA_FILT_CHIP_DX   32
/* VERTICAL placement of that chip, and it is deliberately not a second pair of numbers:
 * the chip IS the row's highlight bar, drawn narrow. Every list that paints a type chip
 * (filter_menu, dex_menu, pick_move's mv_row) backs its rows with a rect at
 * y + BAR_DY of height BAR_H, and a row may not paint outside the rect its own
 * background wipe covers — see pdna_pick.c's ROW REPAINT RULE. The chip used to be a
 * literal `9` at `y`, i.e. y..y+8 against a bar of y-1..y+7: its LAST scanline sat in
 * the NEXT row's rect, so the next row's wipe ate it and chips rendered 8 px tall
 * everywhere except the bottom of the window. Tying the two together here means a future
 * edit to the bar moves the chip with it and cannot re-open that gap. */
#define PDNA_FILT_CHIP_DY   PDNA_FILT_BAR_DY
#define PDNA_FILT_CHIP_H    PDNA_FILT_BAR_H
/* The bar's right border column is the ink budget for a row. */
#define PDNA_FILT_ROW_W  (PDNA_FILT_BAR_X + PDNA_FILT_BAR_W - PDNA_FILT_TEXT_X)

/* MOVE PICKER (source/pdna_pick.c pick_move / mv_row): the same PDNA_FILT_Y0/ROW_H/
 * BAR_* geometry as the two filter lists above -- mv_row wipes+highlights the identical
 * rect (BACKLOG #36 item 7 moved it off a coincidentally-equal `14 + i*9` / `(2,y-1,236,9)`
 * literal pair onto these names) -- but a SHORTER window: pick_move draws a stat detail
 * panel below the list (PDNA_MV_DETAIL_Y..+58), so it only fits PDNA_MV_VIS rows where
 * the filter lists fit the taller PDNA_FILT_VIS. Named here (not left as bare 9/92 in
 * pdna_pick.c) so a future PDNA_FILT_ROW_H change is checked against THIS boundary too,
 * not just the filter lists' own footer -- the exact class of gap host_textfit_test's
 * "filter list last row ink" check exists to catch for the other two lists. */
#define PDNA_MV_VIS          9                /* rows in pick_move's window            */
#define PDNA_MV_DETAIL_Y    92                /* top of the stat detail panel below it */
/* These lists put their hint TWO rows below UI_FOOTER_Y, not on it (nothing pops up over
 * them, so they can use the whole band). Named so the host test can assert it is still
 * inside the footer band and still on screen. */
#define PDNA_FILT_FOOTER_Y 152
#define PDNA_FILT_FOOT     "A pick  U/D/L/R move  B back"
#define PDNA_IFILT_FOOT    "A pick  U/D move  B back"
/* The one row of these lists that is a FIXED string rather than a species/type/item name
 * out of a table the host cannot see. It is also the widest, which is why it is the one
 * pinned here. */
#define PDNA_FILT_SORT_FMT   "Sort: %s"
#define PDNA_FILT_SORT_DEX   "No. (dex)"
#define PDNA_FILT_SORT_ID    "No. (id)"       /* the item list sorts by item id       */
#define PDNA_FILT_SORT_NAME  "A-Z (name)"
#define PDNA_FILT_SORT_REGION "Region"        /* the met-location list's third order   */
#define PDNA_FILT_SORT_VALUES(X) \
  X(PDNA_FILT_SORT_DEX) X(PDNA_FILT_SORT_ID) X(PDNA_FILT_SORT_NAME) X(PDNA_FILT_SORT_REGION)
#define PDNA_FILT_MARKALL    "Mark all..."    /* dex_menu's bulk-edit row             */

/* The ITEM filter list keeps a BORDERED box, which is why its numbers differ: ui_panel's
 * bottom rule sits at y+h-2, so height 12 from y-2 puts it at y+8, one pixel clear of
 * the 8-row text, and an 11 px pitch keeps the next row's ascenders out of the fill. */
#define PDNA_IFILT_Y0       14
#define PDNA_IFILT_ROW_H    11
#define PDNA_IFILT_BOX_DY  (-2)
#define PDNA_IFILT_BOX_H    12
#define PDNA_IFILT_NCAT      6                /* All, Items, Key items, Balls, TMs, Berries */
#define PDNA_IFILT_ROWS     (2 + PDNA_IFILT_NCAT)   /* Sort + Game + the categories   */

/* ---------------------------------------------------------------------------
 * BALL picker (source/pdna_pick.c)
 *
 * The ball field used to TOGGLE through the twelve balls one press at a time. It is a
 * list now, borrowing the item picker's split-view geometry (24 px icon + name + blurb)
 * so there is one list idiom rather than a new one — and it degrades to name-only in the
 * artless build, where item_icon_for returns NULL for everything. The met-location list
 * below borrows the same idiom for the same reason.
 *
 * Every fixed string below is measured by tests/host_textfit_test.c: this screen family
 * clips silently. */
/* A ball row is a 24 px item icon plus the name and its blurb, so the pitch has to
 * clear the icon (ROW_H - 2 >= ITEM icon height) and the last row has to stop above the
 * note. Four rows of twelve balls is the item picker's split-view density. */
#define PDNA_BALL_Y0        14
#define PDNA_BALL_ROW_H     26
#define PDNA_BALL_VIS        4
#define PDNA_BALL_ICON_X     6
#define PDNA_BALL_TEXT_X    36
#define PDNA_BALL_NAME_DY    4                /* name baseline inside the row          */
#define PDNA_BALL_DESC_DY   14                /* blurb, one proportional line under it  */
#define PDNA_BALL_NOTE_Y   126
#define PDNA_BALL_RULE_Y   147
#define PDNA_BALL_TITLE     "POKE BALL"
#define PDNA_BALL_TITLE_FMT "%s  %d/%d"
#define PDNA_BALL_FOOT      "A pick  U/D move  B cancel"
#define PDNA_BALL_NOTE      "Gen 3 has a 4-bit ball id: these 12"



/* ---------------------------------------------------------------------------
 * MET-LOCATION list + its filter menu, and the REGION chooser (source/pdna_pick.c)
 *
 * The met location used to step ±1 through 256 ids. It is a list over gen3_places.c now:
 * 16 sys8 rows of "%3d %s" at an 8 px pitch, like list_pick's text mode, with the item
 * filter menu's bordered-box geometry behind START. */
#define PDNA_LOC_Y0         14
#define PDNA_LOC_ROW_H       8
#define PDNA_LOC_VIS        16                /* 14 + 15*8 + 7 = 141, clear of the rule */
#define PDNA_LOC_TEXT_X      6
/* The selection panel's rect, relative to the row's text baseline. Named (they were two
 * literals inside ml_row) so the host test can see that this rect is ONE PIXEL TALLER
 * THAN ITS OWN PITCH: rows -1..7 against a next-row top of ROW_H + SEL_DY = 7, i.e. one
 * shared scanline. That is legal only under pdna_pick.c's ROW REPAINT RULE, which makes
 * the selected row paint last on every path so it always wins that scanline. Unlike the
 * IFILT box's shared scanline this one is INSIDE the glyph box (7 < UI_ROW_H) -- see
 * lp_row's comment for the descender consequence and what widening it would cost. */
#define PDNA_LOC_SEL_DY    (-1)
#define PDNA_LOC_SEL_H       9
#define PDNA_LOC_PAGE       10                /* L/R jump, same as list_pick's         */
#define PDNA_LOC_ROW_COLS   28                /* ui_truncate budget for "%3d %s"       */
#define PDNA_LOC_HDR_COLS   29                /* ui_truncate budget for the header     */
#define PDNA_LOC_RULE_Y    148
/* Header: HDR_FMT(region, game, sort, count). The FULL names ("All regions", "FireRed/LG",
 * "A-Z (name)") add up to 41 columns, which ui_truncate cut back to "MET All regions/
 * Emerald No. ~" — legal, visibly clipped, and useless: the count and the sort both went.
 * So the header uses SHORT tags and the filter menu keeps the long names. The tags live
 * here as X-macros so pdna_pick.c and the host test index the same table, and the test
 * formats every combination. Worst case is "MET Sevii/FRLG A-Z 217" = 22 columns. */
#define PDNA_LOC_HDR_FMT    "MET %s/%s %s %d"
#define PDNA_LOC_RGN_TAGS(X)  X("All") X("Hoenn") X("Kanto") X("Sevii") X("Spec")
#define PDNA_LOC_GAME_TAGS(X) X("All") X("RS") X("E") X("FRLG")
#define PDNA_LOC_SORT_TAGS(X) X("No.") X("A-Z") X("Rgn")
#define PDNA_LOC_FOOT       "A pick  SEL find  ST sort  B"
#define PDNA_LOC_EMPTY      "No place matches. ST to widen."
/* The location filter menu reuses the item filter's bordered-box geometry; it has Sort +
 * Game rows plus one row per region and an "All regions" row. */
#define PDNA_LFILT_NRGN      5                /* All, Hoenn, Kanto, Sevii, Special     */
#define PDNA_LFILT_ROWS     (2 + PDNA_LFILT_NRGN)
#define PDNA_LFILT_ALL      "All regions"
#define PDNA_LFILT_GAME_FMT "Game: %s"
#define PDNA_LFILT_TITLE    "MET FILTER / SORT"

/* The REGION row of the edit list opens this one-screen chooser, and picking a region
 * then drops straight into the location list scoped to it. */
#define PDNA_RGN_TITLE      "REGION"
#define PDNA_RGN_FOOT       "A pick  U/D move  B cancel"
#define PDNA_RGN_NOTE       "Region scopes the place list"
#define PDNA_RGN_Y0         20
#define PDNA_RGN_ROW_H      16
#define PDNA_RGN_TEXT_X      8
#define PDNA_RGN_NOTE_Y    140
#define PDNA_RGN_RULE_Y    148
/* "%-12s %3d": the region name padded to the longest ("Sevii Isles"), then how many
 * places it offers THIS origin game — 0 says out loud that a FireRed record has no
 * Hoenn to have been met in. */
#define PDNA_RGN_ROW_FMT    "%-12s %3d"

/* ---------------------------------------------------------------------------
 * EDIT field list (source/pdna_edit.c): label column starts at PDNA_EDIT_LBL_X and the
 * value column at PDNA_EDIT_VAL_X, so a label has (VAL_X - LBL_X) px and a value has
 * (UI_SCR_W - VAL_X). The value is ui_truncate'd to PDNA_EDIT_VAL_COLS, the label is
 * NOT — a label that overruns silently paints into the value column. Both new labels
 * ("Max PP n", "Region") and the max-PP value format are therefore pinned here. */
#define PDNA_EDIT_LBL_X       6
#define PDNA_EDIT_VAL_X     118
#define PDNA_EDIT_VAL_COLS   15               /* (240-118)/8 = 15 */
#define PDNA_EDIT_LBL_W     (PDNA_EDIT_VAL_X - PDNA_EDIT_LBL_X)
#define PDNA_EDIT_MAXPP_LBL(n)  "Max PP " #n
#define PDNA_EDIT_REGION_LBL    "Region"
/* "35  Ups 3": the derived maximum, then the PP-Up count that bought it. */
#define PDNA_EDIT_MAXPP_FMT     "%u  Ups %u"
#define PDNA_EDIT_FOOT          "L/R+- A:pick B:exit ST:save"

/* ---------------------------------------------------------------------------
 * The Game Boy mon editor screen (source/pdna_gbedit.c) and the persist-path popups
 * it and source/pdna_gen12.c's gb_edit_hook/gb_edit_persist show -- pinned here so
 * tests/host_textfit_test.c can measure every one against the real font.
 *
 * confirm()'s dynamic prose block draws, in order: an optional issue sentence
 * (ui_ptext_wrap, <=2 lines), an optional "write anyway?" line (1 line, only when the
 * issue line drew), an optional stale-stats sentence (ui_ptext_wrap, <=2 lines) --
 * worst case 2+1+2 = 5 lines at PDNA_GBEDIT_CONFIRM_LINE_H px each. That pushes the
 * "A = write" / "B = cancel" lines low enough to reach y=131, which collided with the
 * old footer at y=128/138 -- so the footer moved to PDNA_GBEDIT_BAK_Y1/Y2 (134/144;
 * nothing else is drawn below it and UI_SCR_H is 160, so there is room). */
#define PDNA_GBEDIT_TITLE          "EDIT GB POKEMON"
#define PDNA_GBEDIT_CONFIRM_TITLE  "Write to the save?"
#define PDNA_GBEDIT_CONFIRM_W      216            /* x=20 -> ends at 236, screen is 240 */
#define PDNA_GBEDIT_CONFIRM_LINE_H 10              /* = UI_ROW_H (8) + 2                */
#define PDNA_GBEDIT_CONFIRM_MAXLN  2
#define PDNA_GBEDIT_CONFIRM_Y0     56              /* first dynamic line's y            */
#define PDNA_GBEDIT_CONFIRM_GAP    12              /* after "write anyway?" / after stale */
#define PDNA_GBEDIT_AB_DY1          4              /* "A = write" y, relative to the block end */
#define PDNA_GBEDIT_AB_DY2         16              /* "B = cancel" y, relative to the block end */
#define PDNA_GBEDIT_WRITE_ANYWAY   "The game may not accept it. Write anyway?"
#define PDNA_GBEDIT_A_WRITE        "A = write (backs up first)"
#define PDNA_GBEDIT_B_CANCEL       "B = cancel"
#define PDNA_GBEDIT_BAK_L1         "Original backed up to .bak,"
#define PDNA_GBEDIT_BAK_L2         "new save verified on write."
#define PDNA_GBEDIT_BAK_Y1         134
#define PDNA_GBEDIT_BAK_Y2         144

#define PDNA_GBEDIT_BADCHARSET_TITLE "CAN'T STORE THAT"
#define PDNA_GBEDIT_BADCHARSET_L1    "Not in this game's charset:"
#define PDNA_GBEDIT_BADNAME_L1       "The name was refused."
#define PDNA_GBEDIT_MOVE_LATE_TITLE  "NOT IN THIS GAME"
#define PDNA_GBEDIT_MOVE_LATE_L1     "That move is from a later"
#define PDNA_GBEDIT_MOVE_LATE_L2     "generation."
#define PDNA_GBEDIT_MOVE_DUP_TITLE   "ALREADY KNOWN"
#define PDNA_GBEDIT_MOVE_DUP_L1      "This Pokemon has that move"
#define PDNA_GBEDIT_MOVE_DUP_L2      "in another slot."

/* gb_edit_persist's SF_ERR_RENAME switch (source/pdna_gen12.c): same shape as the
 * Gen-3 path (pdna_main.c app_commit), but the SF_WHERE_TARGET line is SHORTER —
 * pdna_main.c's own "Could not re-check the card. Verify it." measures 191px against
 * msg_wait's 184px proportional clamp (already over budget there, unfixed; this file
 * must not copy that clip). The other three lines measure under 184px unchanged. */
#define PDNA_GBEDIT_UNCONFIRMED_TITLE "UNCONFIRMED"
#define PDNA_GBEDIT_UNCONFIRMED_L2    "Could not re-check the card."
#define PDNA_GBEDIT_TMPONLY_TITLE     "SAVE NOT IN PLACE"
#define PDNA_GBEDIT_TMPONLY_L2        "Card dropped it. Rename .tmp on a PC."
#define PDNA_GBEDIT_TMPANDOLD_TITLE   "NOT SAVED"
#define PDNA_GBEDIT_TMPANDOLD_L2      "Old save intact. Try saving again."
#define PDNA_GBEDIT_SAVELOST_TITLE    "SAVE LOST"
#define PDNA_GBEDIT_SAVELOST_L1       "Card kept neither copy."
#define PDNA_GBEDIT_SAVELOST_BAK      "Restore the .bak on a PC."
#define PDNA_GBEDIT_SAVELOST_NOBAK    "No backup was made!"

/* gbs_box_writable's SF_ERR_UNWRITABLE hint (source/pdna_gen12.c gb_edit_hook step 2):
 * "Switch boxes in-game once, then retry." measures 195px, over the 184px clamp;
 * "Change box in-game once, then retry." measures 188px, STILL over; this one (161px)
 * fits. */
#define PDNA_GBEDIT_UNWRITABLE_HINT   "Change box in-game, then retry."

/* gb_edit_hook / gb_edit_persist's own gate + verdict popups (source/pdna_gen12.c) —
 * everything else the S2 edit path draws besides the confirm screen (above) and the
 * SF_ERR_RENAME switch (above). msg_wait lines measure against its 184px proportional
 * clamp; s_busy draws with ui_text at x=28 (8px/glyph fixed), budget 240-28=212px. */
#define PDNA_GBEDIT_BUSY_SAVING    "Saving - do not power off"     /* s_busy's own line */
#define PDNA_GBEDIT_BUSY_BACKUP    "Backing up original..."
#define PDNA_GBEDIT_BUSY_WRITING   "Writing + verifying..."

#define PDNA_GBEDIT_READONLY_TITLE "READ-ONLY"
#define PDNA_GBEDIT_NEEDS_OMEGA    "Needs EZ-Flash Omega."

#define PDNA_GBEDIT_BOXWR_TITLE    "CAN'T EDIT THIS BOX"
#define PDNA_GBEDIT_BOXRD_TITLE    "CAN'T READ THIS BOX"
#define PDNA_GBEDIT_EMPTYSLOT_TITLE "EMPTY SLOT"
#define PDNA_GBEDIT_EMPTYSLOT_L1   "The GB list has no Pokemon here."
#define PDNA_GBEDIT_REFUSED_TITLE  "EDIT REFUSED"
#define PDNA_GBEDIT_NOVERIFY_L1    "The record did not verify."
#define PDNA_GBEDIT_NOTHING_L2     "Nothing was written."
#define PDNA_GBEDIT_UNCHANGED_L2   "Save unchanged."
#define PDNA_GBEDIT_BACKUPFAIL_TITLE "BACKUP FAILED"
#define PDNA_GBEDIT_WRITEFAIL_TITLE  "WRITE FAILED"
#define PDNA_GBEDIT_DISCARDED_L2   "Save NOT modified; edit discarded."

/* S3: MOVE TO / RELEASE (source/pdna_gen12.c gb_move_hook/gb_release_hook/gb_pick_box).
 * RELEASE's title goes through the shared app_confirm() (pdna_main.c), which draws its
 * title with the SAME x=28/184px proportional clamp msg_wait uses -- measured in the
 * same PF(...,28,184) block as the other GBEDIT titles below. */
#define PDNA_GBEDIT_RELEASE_TITLE     "Release this Pokemon?"
/* gb_release_confirm's fallback when the slot's own GB nickname fails to load (should
 * not happen once gb_locate has already validated box/slot, but a confirm dialog must
 * never show garbage). Exactly 12 characters -- the same ui_truncate() cap the real
 * nickname goes through -- so it never itself needs the cut. */
#define PDNA_GBEDIT_RELEASE_FALLBACK  "this Pokemon"

/* gb_move_hook's per-status hints, shown as msg_wait's second line alongside
 * gbs_status_text(st) as the first -- 184px proportional, same clamp. */
#define PDNA_GBEDIT_MOVE_NEEDSBASE_L2 "Gen 1: withdraw it in-game instead."
#define PDNA_GBEDIT_MOVE_FLOOR_L2     "The party must keep one Pokemon."
#define PDNA_GBEDIT_MOVE_MAIL_L2      "Take the Mail off it in-game first."
/* NOT "That box is full." -- gbs_status_text(GBS_ERR_FULL) already says exactly that as
 * msg_wait's L1, so an L2 repeating it would tell the player nothing new. */
#define PDNA_GBEDIT_MOVE_FULL_L2      "Free a slot there first."
/* BACKLOG #40(d): gbs_status_text(GBS_ERR_FULL) says "that box is full" -- correct for
 * a storage box, but read backwards for the party pseudo-box (a "box" a player has
 * never once called one). gb_move_hook substitutes this as msg_wait's L1 specifically
 * when the destination gb_box_is_party(). */
#define PDNA_GBEDIT_MOVE_PARTYFULL_L1 "The party is full."

/* gb_pick_box's full-screen destination list -- same fixed sys8 layout as
 * gb_report_page/gb_info_page (title at x=4,y=3; footer at x=4,y=150; both 8px/glyph). */
#define PDNA_GBEDIT_PICKBOX_TITLE  "MOVE TO"
#define PDNA_GBEDIT_PICKBOX_FOOT   "A pick  B cancel"
#define PDNA_GBEDIT_PICKBOX_Y0     20    /* first row's y, below the y=13 title rule */
#define PDNA_GBEDIT_PICKBOX_ROW_H  10
#define PDNA_GBEDIT_PICKBOX_ROWS   12    /* visible rows; last row's ink ends at y=137,
                                           * clear of the y=147 footer rule           */
/* Shown (via the shared msg_wait, 184px proportional clamp) instead of the picker when
 * every box but the source is a virgin/uninitialised Gen-1 bank -- there is nowhere to
 * pick, so no destination list is drawn at all. */
#define PDNA_GBEDIT_PICKBOX_NONE_TITLE "NO DESTINATION"
#define PDNA_GBEDIT_PICKBOX_NONE_L1    "No other box can be written to."

/* ---------------------------------------------------------------------------
 * BACKLOG #41 (Guy, 2026-09-05): source/pdna_gbsummary.c, the Gen-1/2 twin of
 * pdna_summary.c's pdna_inspect() — "the edit page for gen 2 and 1 should feel the
 * same as gen 3 ... we edit within the summary page". Three cards styled after the
 * retail Gen-1/2 summary screens (pokered/engine/pokemon/status_screen.asm,
 * pokecrystal/engine/pokemon/stats_screen.asm — field ORDER only, clean-room, no
 * ripped art). Fixed sys8 throughout (ui_text, 8 px/glyph — the same convention
 * pdna_gbedit.c's own flat list uses), so every literal here is checked by counting
 * columns rather than measuring proportional pixels; per-record VALUES (a species
 * name, gbe_value's own formatted numbers) are ui_truncate'd to a column width and
 * stay out of host_textfit_test.c by that file's own header rule ("built from data
 * the host cannot see"). PDNA_EDIT_LBL_X/VAL_X (6/118, above) are reused as-is for
 * every single-field row so a label already proven to fit there does not need its
 * own new check. */
#define PDNA_GBSUM_VIEW_CHIP   "VIEW"
#define PDNA_GBSUM_EDIT_CHIP   "EDIT"
#define PDNA_GBSUM_CARD_INFO   "INFO"
#define PDNA_GBSUM_CARD_STATS  "STATS"
#define PDNA_GBSUM_CARD_MOVES  "MOVES"
#define PDNA_GBSUM_FOOT_VIEW    "A edit  U/D mon  L/R card  B"
#define PDNA_GBSUM_FOOT_VIEW_RO "U/D mon  L/R card  B back"
#define PDNA_GBSUM_FOOT_EDIT    "<>edit A:ok L/R SEL:list B"

/* Card 0 (INFO) row labels not already covered by gb_editor.c's own LABEL[] table
 * (gbe_label_of) — species/type/status/sex/shiny/pokerus/EXP are display-only, so
 * they get their own short labels rather than borrowing an editable field's. All at
 * PDNA_EDIT_LBL_X (6); the longest, "PKRS", is 4 columns (32 px), well inside
 * PDNA_EDIT_LBL_W (112 px) — see gbe_label_of's own "Friendship" (10 cols/80 px),
 * already proven to fit the same column by pdna_gbedit's own screen. */
#define PDNA_GBSUM_LBL_SPECIES "Species"
#define PDNA_GBSUM_LBL_TYPE    "Type"
#define PDNA_GBSUM_LBL_G1TYPE  "G1 Type"     /* Gen 1: the STORED type bytes, not a name table */
#define PDNA_GBSUM_LBL_STATUS  "Status"      /* Gen 1 only (Gen 2's status_screen row has no
                                              * Gen-2 equivalent this tree can read — see
                                              * gb_edit.h, no Gen-2 status getter exists) */
#define PDNA_GBSUM_LBL_PKRS    "PKRS"        /* Gen 2 only */
/* "Sex M  Shiny Yes" — one combined display row (BACKLOG #41's row budget: Card 0
 * has up to 11 rows on Gen 2 and the y=34..150 band is 116 px / 9 px-per-row = ~12,
 * so two single-purpose rows here would leave no margin for Pokerus). Worst case
 * "Sex M  Shiny Yes" is 17 cols (136 px) at x=6, comfortably inside 240. */
#define PDNA_GBSUM_SEXSHINY_FMT "Sex %s  Shiny %s"
#define PDNA_GBSUM_SHINY_YES    "Yes"
#define PDNA_GBSUM_SHINY_NO     "No"
/* "EXP 1640000  +999999" (worst case) is 21 cols (168 px) at x=6. */
#define PDNA_GBSUM_EXP_FMT      "EXP %lu  +%lu"
#define PDNA_GBSUM_EXP_MAX_FMT  "EXP %lu  MAX"
/* Gen-1 status byte (pokered/constants/battle_constants.asm:62-67): bits 0-2 sleep-
 * turns-left, bit 3 poison, bit 4 burn, bit 5 freeze, bit 6 paralysis. Only ONE ever
 * applies to a legally-obtained Gen-1 mon at a time (a hacked record could set more
 * than one bit; the first match wins, same "report the first problem" convention as
 * gb_issue_text()). */
#define PDNA_GBSUM_ST_OK   "OK"
#define PDNA_GBSUM_ST_SLP  "Asleep"
#define PDNA_GBSUM_ST_PSN  "Poisoned"
#define PDNA_GBSUM_ST_BRN  "Burned"
#define PDNA_GBSUM_ST_FRZ  "Frozen"
#define PDNA_GBSUM_ST_PAR  "Paralyzed"

/* Card 1 (STATS): a 4-column grid — label / computed stat / DV / stat exp — none of
 * gbe_label_of's own labels (they are one word each, "DV Atk" etc.) fit this tight a
 * layout, so the stat row gets its own 3-letter names. DV_X - VAL_X leaves 64 px for
 * the widest VAL text, "999/999" (the HP row's cur/max), at 56 px; SE_X - DV_X
 * leaves 44 px for "DV 15" (40 px, the widest DV cell — the HP row's own DV, GBE_DVH,
 * is read-only and drawn dim in the SAME column). Checked directly against these
 * macros, not retyped, in host_textfit_test.c. */
#define PDNA_GBSUM_STAT_LBL_X   6
#define PDNA_GBSUM_STAT_VAL_X  34
#define PDNA_GBSUM_STAT_DV_X   98
#define PDNA_GBSUM_STAT_SE_X  142
#define PDNA_GBSUM_HDR_DV      "DV"
#define PDNA_GBSUM_HDR_SE      "SE"
#define PDNA_GBSUM_STAT_HP     "HP"
#define PDNA_GBSUM_STAT_ATK    "ATK"
#define PDNA_GBSUM_STAT_DEF    "DEF"
#define PDNA_GBSUM_STAT_SPE    "SPE"
#define PDNA_GBSUM_STAT_SPA    "SpA"
#define PDNA_GBSUM_STAT_SPD    "SpD"
#define PDNA_GBSUM_STAT_SPC    "SPC"          /* Gen 1: Special, not split */
/* "999/999" (party) or "-" (box). "DV 15" / "SE 65535" (worst case, 8 cols/64 px —
 * inside SE_X's own 240 - 142 = 98 px budget). */
#define PDNA_GBSUM_STAT_CURMAX_FMT "%u/%u"
#define PDNA_GBSUM_STAT_DASH       "-"
#define PDNA_GBSUM_STAT_DV_FMT     "DV %u"
#define PDNA_GBSUM_STAT_SE_FMT     "SE %u"
/* Shown once, under the last stat row, for a BOX record only (no live HP/stats exist
 * to show — gb_get_stat/gb_get_current_hp are party-only by contract). 23 cols
 * (184 px) at x=6. */
#define PDNA_GBSUM_BOX_STAT_NOTE  "Computed on withdrawal"

/* Card 2 (MOVES): two lines per move — the name at PDNA_EDIT_LBL_X/VAL_X (like every
 * other GBE_K_MOVE row in this tree), then an indented PP/PP-Ups line with its own
 * two sub-fields. "Move 4" is 6 cols (48 px), inside PDNA_EDIT_LBL_W. */
#define PDNA_GBSUM_LBL_MOVE_FMT "Move %u"
#define PDNA_GBSUM_PPROW_X      18             /* indented under the move name        */
#define PDNA_GBSUM_PP_LBL       "PP"
#define PDNA_GBSUM_PP_VAL_X     40
#define PDNA_GBSUM_UPS_LBL      "Ups"
#define PDNA_GBSUM_UPS_LBL_X    96
#define PDNA_GBSUM_UPS_VAL_X   128

/* ---------------------------------------------------------------------------
 * S5-B: the Gen-3 <-> Game Boy sidecar UI (docs/GEN3-TO-GB-SIDECAR-DESIGN.md sec. 10)
 * source/pdna_main.c's PASTE-with-merge and source/pdna_gen12.c's PASTE(GB) + the
 * DV-orphan warning. Body text goes through the shared msg_wait 184px proportional
 * clamp exactly like PDNA_GBEDIT_* above, EXCEPT the merge confirm screen's own
 * per-flag lines, which reuse that same (28, 184) convention directly. */
/* S5-B review fix (BLOCKING #2): PDNA_SIDECAR_BUSY_* is GONE -- app_paste_gb_merge()
 * no longer borrows app_box_swap (box_oam.c holds that cache for the WHOLE box-screen
 * visit, not a tick, so the borrow could never succeed from the box/bank grid at all);
 * it now uses its own local stack buffer. See app_paste_gb_merge's own comment. */
#define PDNA_SIDECAR_READFAIL_TITLE  "SIDECAR READ FAILED"
#define PDNA_SIDECAR_NONE_TITLE      "NO SIDECAR"
#define PDNA_SIDECAR_NONE_L1         "No sidecar: converted copy."
#define PDNA_SIDECAR_MERGEFAIL_TITLE "MERGE FAILED"
#define PDNA_SIDECAR_MERGEFAIL_L1    "The sidecar could not be merged."
#define PDNA_SIDECAR_NOTUPDATED_TITLE "SIDECAR NOT UPDATED"
#define PDNA_SIDECAR_NOTUPDATED_L1   "The Pokemon was pasted;"
#define PDNA_SIDECAR_NOTUPDATED_L2   "the sidecar entry remains."
/* S5-B review fix #3 (BLOCKING): a sidecar file that exists but fails its own CRC is
 * NOT the same as "no sidecar" -- silently falling back to the lossy converted copy
 * would hide real, avoidable data loss. Two distinct bodies share the one title:
 * pdna_gen12.c's gb_paste_write() (the file is renamed aside, not destroyed) and
 * pdna_main.c's app_paste_gb_merge() (an app_confirm: paste the converted copy anyway,
 * or cancel). */
#define PDNA_SIDECAR_CORRUPT_TITLE     "SIDECAR CORRUPT"
#define PDNA_SIDECAR_CORRUPT_KEPT_L1   "Kept as .bad; a new file is started."
/* S5-B re-verification NEW-3 (should): reworded as a QUESTION -- this is
 * app_confirm()'s own l1, and app_confirm() always draws "A = yes" / "B = no" below
 * whatever text it is given, so a flat statement here read like the choice had already
 * been made instead of being asked. */
#define PDNA_SIDECAR_CORRUPT_MERGE_L1  "Paste the converted copy anyway?"

/* The merge confirm screen (app_sidecar_confirm, pdna_main.c): one panel, up to six
 * conditional GbscMergeReport lines, then the always-shown EVs line, then A/B. */
#define PDNA_SIDECAR_CONFIRM_TITLE   "RESTORED FROM THE SIDECAR"
#define PDNA_SIDECAR_L_EVOLVED       "It evolved on the Game Boy."
#define PDNA_SIDECAR_L_LEVEL         "Its level changed."
#define PDNA_SIDECAR_L_MOVES         "Its moves changed."
#define PDNA_SIDECAR_L_RENAMED       "It was renamed."
#define PDNA_SIDECAR_L_RENAME_REFUSED "Rename refused, kept the old name."
#define PDNA_SIDECAR_L_ITEM_IGNORED  "GB item ignored; kept the original."
#define PDNA_SIDECAR_L_EVS           "EVs restored from the sidecar."
#define PDNA_SIDECAR_A_PASTE         "A = paste"
#define PDNA_SIDECAR_B_CANCEL        "B = cancel"
#define PDNA_SIDECAR_PANEL_X         16
#define PDNA_SIDECAR_PANEL_Y         14
#define PDNA_SIDECAR_PANEL_W         208
#define PDNA_SIDECAR_PANEL_H         144
#define PDNA_SIDECAR_TEXT_X          28
#define PDNA_SIDECAR_TEXT_MAXW       184
#define PDNA_SIDECAR_LINE_H          10
#define PDNA_SIDECAR_LINE_Y0         44   /* first conditional-flag line's y            */
#define PDNA_SIDECAR_EVS_GAP         4    /* extra gap before the always-shown EVs line */
#define PDNA_SIDECAR_AB_GAP          10   /* extra gap before the A/B footer            */

/* ---- S5-B Part D: PASTE (GB) on an empty Game Boy cell (pdna_gen12.c) ------------- */
/* S5-C Part B1: Gen 1 is no longer refused outright -- a base-stat table now exists
 * (read live off the user's own ROM beside the .sav). GEN1_TITLE/L1 are repurposed
 * for the one refusal that remains: no <base>.gb/.gbc sits next to the save. L1 is
 * the STATIC tail of a two-line message; the dynamic "Put NAME.gb here" line is
 * built with siprintf (gb_gen1_norom_msg) and is safe by construction (msg_wait runs
 * it through ui_ptext_fit, same as every other dynamic message in this tree). */
#define PDNA_SIDECAR_GEN1_TITLE      "NO GEN-1 ROM"
#define PDNA_SIDECAR_GEN1_L1         "beside the .sav to transfer."
#define PDNA_SIDECAR_GEN1_BADROM_L1  "Bad or unreadable ROM."
#define PDNA_SIDECAR_XFER_TITLE      "CAN'T TRANSFER"
/* S5-B re-verification NEW-1: gbs_insert() only ever lands a BOX-kind record into a
 * storage box (gb_session.h's own contract) -- the party pseudo-box is refused before
 * gb_paste_write() ever touches the card. */
#define PDNA_SIDECAR_PARTY_L1        "Storage boxes only, not the party."
#define PDNA_SIDECAR_MKDIR_TITLE     "SIDECAR FOLDER"
#define PDNA_SIDECAR_FULL_TITLE      "SIDECAR FULL"
#define PDNA_SIDECAR_FULL_L1         "Too many clones of this Pokemon."
#define PDNA_SIDECAR_NOTWRITTEN_TITLE "SIDECAR NOT WRITTEN"
#define PDNA_SIDECAR_NOTWRITTEN_L2   "Nothing transferred."
/* BACKLOG #40(c): gb_paste_write()/gb_paste_hook's own STRUCTURAL refusals (the
 * conversion already succeeded and the loss screen was already accepted -- gbs_insert()
 * itself then said no: the box filled up between the check and the write, or the list
 * came back malformed) used to reuse PDNA_GBEDIT_REFUSED_TITLE ("EDIT REFUSED"), which
 * reads as though an EDIT was refused when this is a PASTE (a Gen-3 -> GB transfer).
 * Distinct title, same (28, 184) proportional clamp as every other GBEDIT/SIDECAR
 * title in this file. NOT used for the earlier PDNA_SIDECAR_XFER_TITLE refusals above
 * (species/move/Egg/no-ROM) -- those already name the problem specifically; this one
 * is for the generic "it didn't land" cases only. */
#define PDNA_SIDECAR_XFER_REFUSED_TITLE "TRANSFER REFUSED"

/* The transfer-down loss screen (pdna_gen12.c gb_paste_loss_screen): one short line per
 * Gen3ToGbLoss flag set, from a const {flag, text} table -- texts measured here.
 * S5-B review fix #4: two of Gen3ToGbLoss's 17 flags (pokerus_dropped,
 * friendship_dropped) had no row at all. The screen's own fit has exactly 2 px of
 * slack (host_textfit_test.c's own worst-case check), so the fix is ROW-COUNT-NEUTRAL:
 * ITEM and SECRETID merge into one row (both are "this one plain fact about the mon
 * does not survive"), freeing exactly the one row POKERUS needs -- still 10 conditional
 * rows total, not 11. */
#define PDNA_SIDECAR_LOSS_TITLE      "WHAT WON'T TRANSFER"
#define PDNA_SIDECAR_LOSS_NATURE     "Nature and ability"
#define PDNA_SIDECAR_LOSS_RIBBONS    "Ribbons and contest stats"
#define PDNA_SIDECAR_LOSS_METDATA    "Met place / level / ball"
#define PDNA_SIDECAR_LOSS_IVS        "IVs halved to DVs"
#define PDNA_SIDECAR_LOSS_EVS        "EVs rescaled"
#define PDNA_SIDECAR_LOSS_ITEMSECRET "Held item and Secret ID"
#define PDNA_SIDECAR_LOSS_POKERUS    "Pokerus and friendship"
#define PDNA_SIDECAR_LOSS_SHINY      "Shiny not preserved"
#define PDNA_SIDECAR_LOSS_GENDER     "Gender not preserved"
#define PDNA_SIDECAR_LOSS_NAME       "Nickname/OT changed"
#define PDNA_SIDECAR_LOSS_KEPT_L1    "Kept in /PokeDNA/sidecar;"
#define PDNA_SIDECAR_LOSS_KEPT_L2    "restored when it comes back."
#define PDNA_SIDECAR_LOSS_STAYS      "The copy in your Gen-3 save stays."
#define PDNA_SIDECAR_LOSS_A_TRANSFER "A = transfer"
#define PDNA_SIDECAR_LOSS_B_CANCEL   "B = cancel"
/* Full-screen list (gb_pick_box's own geometry: title y=3, rule y=13), not a scrolling
 * picker -- rows are drawn only for flags actually set, so the common case is much
 * shorter than the worst case the host test pins: 10 conditional Gen3ToGbLoss lines +
 * KEPT_L1/L2 + STAYS + A + B = 14 lines at ROW_H, plus two ROW_H/2 gaps, must still
 * clear the screen (UI_SCR_H = 160) -- see gb_paste_loss_screen's own layout. */
#define PDNA_SIDECAR_LOSS_ROW_Y0     16
#define PDNA_SIDECAR_LOSS_ROW_H      9

/* pdna_gbedit.c's DV-orphan warning (Part E): shown once per editor visit, on the first
 * adjust/press of a DV row, when the mon's current key already has a sidecar file. */
#define PDNA_SIDECAR_DV_TITLE        "SIDECAR WARNING"
#define PDNA_SIDECAR_DV_L1           "Changing DVs orphans the sidecar."
#define PDNA_SIDECAR_DV_L2           "A = continue"

/* The GB info page's extra line (Part E): "N here came from Gen 3". Built dynamically
 * (the count is data), but the trailing text is fixed and measured here at its widest
 * plausible count prefix ("30 " -- a box tops out at G3_IN_BOX/gb_list_capacity, well
 * under 100). */
#define PDNA_SIDECAR_INFO_SUFFIX     " here came from Gen 3"

/* app_copy()'s toast, shown only when a GB source is active (copy_native registered)
 * -- an ordinary same-generation Gen-3 copy keeps its original "(kept until
 * overwritten)" line. S5-B re-verification NEW-3: retired PDNA_SIDECAR_COPY_NATIVE/
 * CONVERTED, which claimed "lossless" for every GB mon including Gen-1 mons and
 * never-transferred-down Gen-2 mons -- neither of which a later PASTE can actually
 * merge losslessly, since that needs a sidecar entry that does not exist for them.
 * These two are gated on has_sidecar (the real answer) instead of from_gb (only "was
 * the native record captured", which is not the same question). */
#define PDNA_SIDECAR_COPY_HAS        "Sidecar found: pastes lossless"
#define PDNA_SIDECAR_COPY_NONE       "No sidecar: paste will convert it"

/* S5-C Part B2: gb_reconcile_on_load()'s one confirm screen (app_confirm(), so the
 * footer is app_confirm's own standard "A = yes / B = no" -- "yes" means release).
 * The title is built dynamically ("N " + this suffix, siprintf into a 40 B buffer);
 * measured here at a worst-case count prefix the same way PDNA_SIDECAR_INFO_SUFFIX
 * is (a save's party+PC together top out at 6 + G3_TOTAL_BOXES*G3_IN_BOX == 426, so
 * "426 " is the true worst case, wider than "30 "/"999 " would suggest).
 *
 * S5-C review #4: RECON_L1 now says permanent deletion explicitly, matching the
 * single-mon confirm's own wording (pdna_main.c app_release(): "Release this
 * Pokemon?" / "Deleted permanently.") -- silence on that point read as gentler than
 * the action actually is. Two lines via app_confirm's own ui_ptext_wrap(...,2,...). */
#define PDNA_SIDECAR_RECON_TITLE_SUFFIX "POKEMON TRANSFERRED"
#define PDNA_SIDECAR_RECON_L1           "Release the originals? Deleted permanently."
#define PDNA_SIDECAR_RECON_NOTUPD_TITLE "SIDECAR NOT UPDATED"
#define PDNA_SIDECAR_RECON_NOTUPD_L1    "Release saved; a sidecar wasn't."
/* S5-C review #4: shown after A only when fewer than N were actually released (the
 * party floor, or a duplicate-entry dedupe) -- the user was promised N, so a silent
 * shortfall would read as the feature simply not working. K/N are data (siprintf). */
#define PDNA_SIDECAR_RECON_PARTIAL_TITLE "PARTIAL RELEASE"
#define PDNA_SIDECAR_RECON_PARTIAL_L2    "rest skipped (party floor/dupes)."
/* S5-C 2nd review #2: shown instead of (not alongside) the PARTIAL_RELEASE message
 * above when a release was made in RAM but app_commit_pc()/app_commit_sb1() then
 * FAILED to persist it -- the underlying reason was already shown by that commit's
 * OWN failure screen (app_save_finalize's "BACKUP FAILED"/"WRITE FAILED"/etc., which
 * names the real SfStatus); this one just says, specifically for this feature, that
 * the release itself did not reach the card and nothing was claimed. */
#define PDNA_SIDECAR_RECON_NOSAVE_TITLE  "RELEASE NOT SAVED"
#define PDNA_SIDECAR_RECON_NOSAVE_L1     "Not saved; nothing was claimed."

/* S5-C review #5: view_save()'s load_phase_n() label for gb_reconcile_on_load() --
 * it can read/write several SD files before the box screen ever paints, and
 * without its own label the screen would still show the PREVIOUS phase's text for
 * however long that takes, which reads as a hang on the wrong step. Measured below
 * against the same "step/PDNA_LOAD_STEPS " prefix load_phase_n() itself builds
 * (pdna_main.c); PDNA_LOAD_STEPS is pdna_main.c-internal, so the prefix is a
 * literal here, commented so a step-count change is not missed. */
#define PDNA_LOAD_PHASE_SIDECARS "Checking sidecars..."

/* ---------------------------------------------------------------------------
 * Settings + Rumble pages (source/pdna_main.c)
 *
 * These rows are sys8 (a FIXED 8 px cell), and libtonc's TTE does not clip at the right
 * margin — it WRAPS. "Yard visitors:  Needs your ROM" was 30 columns at x=10, and the
 * two that did not fit reappeared as "OM" at x=0 on top of the row below. Every string
 * here is therefore length-critical; the host test measures them. */
#define PDNA_SET_ROW_X        10
#define PDNA_SET_ROW0_Y       24
/* 13, not 14: an 8th row (Extract art) needed room. At 13px the 8th row's ink still
 * clears PDNA_SET_HELP_Y1 by 1px (host-checked) — one row cannot afford more than
 * this without either shrinking the pitch further or moving the help block. */
#define PDNA_SET_ROW_PITCH    13
#define PDNA_SET_ROWS          8   /* Backups, Animations, Yard, Game ROM, Extract art, Rumble, Clear, Close */
/* The selection highlight panel: ui_panel(2, y - PDNA_SET_ROW_PANEL_YOFF, 236,
 * <height>, ...). Normal rows use PDNA_SET_ROW_PANEL_H == PDNA_SET_ROW_PITCH, so
 * consecutive rows' panels exactly TOUCH with no gap (row i's panel bottom is
 * y_i - YOFF + H - 1 == y_i + 10; row i+1's panel top is y_(i+1) - YOFF == y_i + 11
 * -- contiguous). The LAST row has no row below it to touch, only the help text at
 * PDNA_SET_HELP_Y1 -- a full PDNA_SET_ROW_PANEL_H-tall panel there reaches
 * ROW0+(ROWS-1)*PITCH - YOFF + H - 1 = 24+91-2+12 = 125, two rows INTO the help
 * text's first ink row (124). PDNA_SET_ROW_LASTPANEL_H is 3 px shorter so the last
 * row's panel bottoms out at 122, one pixel clear of 123 -- host-checked in
 * tests/host_textfit_test.c ("settings last row panel clear of help"). */
#define PDNA_SET_ROW_PANEL_YOFF   2
#define PDNA_SET_ROW_PANEL_H      PDNA_SET_ROW_PITCH
#define PDNA_SET_ROW_LASTPANEL_H  10
#define PDNA_SET_FOOT_X        4
#define PDNA_SET_HELP_X        8
#define PDNA_SET_HELP_Y1     124
#define PDNA_SET_HELP_Y2     133
#define PDNA_SET_NOTE_Y      142
#define PDNA_SET_FOOTER_Y    152

#define PDNA_SET_BACKUP_FMT  "Backups:  %s"
#define PDNA_SET_BACKUP_MODES(X) \
  X("New each time") X("Single (rolling)") X("Skip (none)")
#define PDNA_SET_YARD_FMT    "Yard visitors:  %s"
/* "Needs your ROM" made this row 30 columns wide — two past the 28 the screen holds.
 * "Set Game ROM" is 28 columns AND points at the row that fixes it. */
#define PDNA_SET_YARD_NEEDROM "Set Game ROM"
#define PDNA_SET_YARD_ON      "On"
#define PDNA_SET_YARD_OFF     "Off"
/* every value the %s can take, for the test to try all of them */
#define PDNA_SET_YARD_VALUES(X) \
  X(PDNA_SET_YARD_NEEDROM) X(PDNA_SET_YARD_ON) X(PDNA_SET_YARD_OFF)

/* Extract-art row (Phase 2, docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 3.1) — one
 * row, three states, same "%s" pattern as Yard visitors above; the fourth (cached,
 * with a byte count) is its own format since the value is numeric, not enumerable. */
#define PDNA_SET_ART_FMT      "Extract art:  %s"
#define PDNA_SET_ART_NEEDROM  "Set Game ROM"
#define PDNA_SET_ART_NOOMEGA  "Omega only"
#define PDNA_SET_ART_VALUES(X) X(PDNA_SET_ART_NEEDROM) X(PDNA_SET_ART_NOOMEGA)
#define PDNA_SET_ART_GO       "Extract art from ROM  >"
#define PDNA_SET_ART_CACHED_FMT "Extract art:  %lu KB cached"
/* worst-case KB the host test tries against PDNA_SET_ART_CACHED_FMT's width budget;
 * icons.bin alone is ~441 KB and every phase combined is designed to stay under 1 MB,
 * so 3 digits is the real ceiling this format has to survive */
#define PDNA_SET_ART_CACHED_MAXKB 999u
/* Game ROM row (item 7's detach switch shows up here, not as its own row — see
 * pdna_main.c's rom_row_menu). Worst case is the longest rom_kind_name() ("LeafGreen",
 * 9 chars) or the detached suffix, whichever is longer; both are well inside budget. */
#define PDNA_SET_ROM_FMT      "Game ROM:  %s"
#define PDNA_SET_ROM_NOTSET   "not set"
#define PDNA_SET_ROM_ARTOFF   "set, art OFF"
#define PDNA_SET_ROM_LONGEST_KIND "LeafGreen"
#define PDNA_SET_ROM_VALUES(X) \
  X(PDNA_SET_ROM_NOTSET) X(PDNA_SET_ROM_ARTOFF) X(PDNA_SET_ROM_LONGEST_KIND)
#define PDNA_SET_ROW_CLEAR   "Clear backups (this save)"
#define PDNA_SET_HELP1       "Animations + Rumble have"
#define PDNA_SET_HELP2       "per-item on/off submenus."
#define PDNA_SET_NOTE        "Yard visitors are scenery, not your Pokemon."
#define PDNA_SET_FOOT        "A change/do  U/D move  B back"

/* Rumble page: the help sentence and the control hints used to share the y=150 row as
 * "GAME RTC on. <>adj A toggle B" — one full row, which is why the hint had to drop its
 * "back". Three separate rows now, and the cue rows must end above the first of them. */
#define PDNA_RMB_ROW0_Y       24
#define PDNA_RMB_ROW_PITCH    14
#define PDNA_RMB_HELP_X        8
#define PDNA_RMB_HELP_Y1     132
#define PDNA_RMB_HELP_Y2     141
#define PDNA_RMB_HELP1       "Needs an EZ-Flash Omega"
#define PDNA_RMB_HELP2       "with GAME RTC on."
#define PDNA_RMB_FOOT        "<> adjust  A toggle  B back"

/* Two rows above the cue list: Strength and Duration. */
#define PDNA_RMB_STEPPERS      2

/* ---------------------------------------------------------------------------
 * ROM IMAGE CHECK — the verdict band (source/pdna_romfull.c)
 *
 * This screen's DELIVERABLE IS A PHONE PHOTO: it is what Guy sends when a 12.5 MB image
 * hangs, and it has to be actionable without the console, the log or the person who wrote
 * it. Every line of it is drawn with ui_ptext_fit(x, y, PDNA_RVF_TEXT_W, ...), and
 * ui_ptext_fit does not wrap or shrink — it CUTS, leaving a single '~'. The tail it cuts
 * is where these particular sentences keep their meaning:
 *
 *   "...(6.29 MB) unstable"   the word that says BUS fault, not IMAGE fault — i.e. whether
 *                             the remedy is a re-copy or a timing change.
 *   "...(99.4%) base only"    the qualifier saying the percentage is against PokeDNA's own
 *                             image and NOT the fused game ROM. Without it the line
 *                             overstates the scan by more than half. It rendered as
 *                             "...B checked (99.4%) b~" on hardware.
 *   "...the bus lied"         why a green grid still needed retries.
 *
 * So every string here is measured at the WIDEST values the code can put in it — 3-digit
 * counts (PDNA_RV_MAX_REGIONS is 256), a 7-hex-digit offset and a 2-digit MB figure
 * (PDNA_RV_MAX_IMAGE is 32 MiB), 10-character comma'd byte counts, "100.0%" — by
 * tests/host_textfit_test.c, which formats THESE macros. A line that only fits at the
 * lucky values is a line that clips on the day it matters. */
#define PDNA_RVF_TEXT_X        6
#define PDNA_RVF_TEXT_W      232   /* x=6 + 232 = 238: two px inside the 240 px screen */
#define PDNA_RVF_HDR_Y1       14   /* coverage / geometry                              */
#define PDNA_RVF_HDR_Y2       22   /* timing / legend                                  */
#define PDNA_RVF_GRID_BOT    124   /* the grid may not draw at or below this row        */
/* Four rows at an 8 px pitch, the last ending on scanline 158. The remedy sentence is the
 * one line a photograph has to carry to be actionable; at three rows it ran off the edge. */
#define PDNA_RVF_ROW1_Y      128
#define PDNA_RVF_ROW2_Y      136
#define PDNA_RVF_ROW3_Y      144
#define PDNA_RVF_ROW4_Y      152

/* --- before the scan: geometry + the colour key --------------------------- */
#define PDNA_RVF_GEOM_FMT    "%s B  %d x %lu KiB  wait %04x rung %d"
#define PDNA_RVF_LEGEND      "green=ok  teal=part  red=BAD  yellow=bus  grey=skip"
#define PDNA_RVF_LEGEND_ALT  "grn=ok teal=part red=BAD ylw=bus gry=skip"

/* --- the header, rewritten when the scan ends ------------------------------
 * "checked" used to sit between the byte count and the percentage. It cost 44 px and the
 * fused build needs those px for PDNA_RVF_COVER_BASE, which is the half of the line that
 * can be WRONG rather than merely terse. */
#define PDNA_RVF_COVER_FMT   "%s of %s B (%lu.%01lu%%)%s"
#define PDNA_RVF_COVER_BASE  " base only"   /* fused image: which denominator this is */
#define PDNA_RVF_SCAN_FMT    "scan %lu.%01lus  %luK: now %lums, loader %lums"
#define PDNA_RVF_SCAN2_FMT   "scan %lu.%01lu s at waitcnt %04x"

/* --- row 1: one line per verdict, each saying what was MEASURED ------------ */
/* "IMAGE FAULT", not "IMAGE INCOMPLETE": at 3-digit counts that headline made the line
 * 256 px and cut the skip count off. Keeping the word IMAGE is what separates this verdict
 * (the bytes are wrong) from STOPPED / COVERAGE SHORT (the scan was short), so the
 * adjective went instead of the noun. */
#define PDNA_RVF_V_FAULT     "IMAGE FAULT - %d bad, %d unstable, %d skip"
#define PDNA_RVF_V_DESC      "DESCRIPTOR DAMAGED - %d skip bit(s) differ"
#define PDNA_RVF_V_STOPPED   "STOPPED at region %d of %d"
#define PDNA_RVF_V_NOTHING   "NOTHING VERIFIED - 0 bytes in %d regions"
#define PDNA_RVF_V_SHORT     "COVERAGE SHORT - %lu/%lu KiB compared"
#define PDNA_RVF_V_OK_PART   "IMAGE OK - %d match (%d part), %d skipped"
#define PDNA_RVF_V_OK        "IMAGE OK - %d regions match, %d skipped"

/* --- row 2: the evidence behind the verdict -------------------------------- */
/* The trailing %s is the whole diagnosis: STABLE means the image on the card differs
 * (re-copy it), UNSTABLE means the same bytes read differently twice (the bus lied, so
 * the timing is what to change). It is last because that is where the interesting value
 * goes on a line whose earlier fields are addresses. */
#define PDNA_RVF_BAD1_FMT    "bad #1: reg %d @ 0x%06lx %lu.%02luMB %s"
#define PDNA_RVF_BAD1_STABLE   "stable"
#define PDNA_RVF_BAD1_UNSTABLE "unstable"
#define PDNA_RVF_DESC_NOTE   "Its bytes are suspect - no CRC covers them."
#define PDNA_RVF_NOTHING_NOTE "This scan proved nothing - 0 bytes read."
#define PDNA_RVF_SHORT_NOTE  "Less compared than this image can prove."
#define PDNA_RVF_RETRY_FMT   "%d region(s) needed a retry - the bus lied"
#define PDNA_RVF_CLEAN_NOTE  "Every compared region matched its stamp."

/* --- row 3: the remedy, or the way out ------------------------------------- */
#define PDNA_RVF_FIX_OMEGA   "Del /PATCH/*.pat, re-copy the .gba, or NOR"
#define PDNA_RVF_FIX_OTHER   "Re-copy the .gba to the card, then re-check"
#define PDNA_RVF_BACK        "B  back"

/* --- row 4: the blind range, on EVERY verdict including the green ones ------ */
#define PDNA_RVF_BLIND_TAIL  "blind: 0x%06lx..EOF (%luK) = crt0 load images"
#define PDNA_RVF_BLIND_MORE  "blind: 0x%06lx..EOF + %luK of other zones"
#define PDNA_RVF_BLIND_ZONES "blind: %lu KiB of zones never compared"

/* ---------------------------------------------------------------------------
 * Party overlay (app_party_overlay, source/pdna_main.c) — retail-layout pass,
 * 2026-08-19. Coordinates are lifted VERBATIM from the measured retail capture
 * (docs/analysis-2026-08-19-party/MEASUREMENTS.md): a fixed slot-1 box, always
 * party position 0 regardless of cursor, plus five 142x24 list rows for
 * positions 1-5. Both the message box and the CANCEL button sit below the last
 * row, in the same 130..160 band retail uses for its own textbox + button.
 *
 * The box (51 px tall) stacks its four fields in one column, one line each
 * (name / level+gender / HP label+bar / HP numbers); a row (only 24 px tall,
 * but 142 wide) instead pairs them two-per-line (name+HP bar on top,
 * level+gender+HP numbers below) — this is NOT the same layout scaled down,
 * it is a different arrangement, per the measured y-ranges in MEASUREMENTS.md. */
#define PDNA_PTY_BOX_X     17
#define PDNA_PTY_BOX_Y     25
#define PDNA_PTY_BOX_W     71
#define PDNA_PTY_BOX_H     51
#define PDNA_PTY_ROW_X     95
#define PDNA_PTY_ROW_W     142
#define PDNA_PTY_ROW_H     24
#define PDNA_PTY_ROW_Y0    10               /* row for party slot 1 (index 1) */
/* Icon anchor bleeds left of the box/row's own edge — measured, not a bug:
 * retail's icon deliberately overlaps the background margin. */
#define PDNA_PTY_BOX_ICON_DX (-12)
#define PDNA_PTY_BOX_ICON_DY  3
#define PDNA_PTY_ROW_ICON_DX (-10)
#define PDNA_PTY_ROW_ICON_DY (-1)

/* ---- row fields, relative to the row's own (x,y) --------------------------
 * top line: name (left) + "HP" label + bar (right); bottom line: level +
 * gender glyph (left) + HP numbers (right, under the bar). */
/* MEASURED (adversarial re-review of a fresh capture, 2026-08-19): retail's name
 * glyphs begin at absolute x=118, one pixel clear of the row icon's own bounding
 * rect (PDNA_PTY_ROW_ICON_DX bleeds it to abs x=85..116). The row's own x is
 * PDNA_PTY_ROW_X (95), so DX = 118 - 95 = 23 -- NOT the x=98 (DX=3) an earlier,
 * wrong measurement in this same pass used, which put 18px of every row's name
 * on top of its icon.
 *
 * PDNA_PTY_NAME_W (corrected 2026-08-20; re-measured pixel-by-pixel off
 * retail-party-idle-f00.png, not eyeballed): the 2026-08-19 pass set this to 48 purely
 * because it kept the field's RIGHT edge where an earlier, narrower layout had left it
 * -- it never re-measured how much room is actually there. Direct pixel search of the
 * retail capture (scanning for the "HP" glyph's own ink -- fill RGB(255,181,66) AND its
 * RGB(82,82,82) outline halo, which is 2px further left than the fill) finds the HP
 * label's own leftmost ink column at absolute x=169, i.e. PDNA_PTY_HP_LBL_DX (74)
 * exactly, confirming that constant is already right. That puts the true available span
 * at HP_LBL_DX(74) - NAME_DX(23) = 51px, and requiring >=2px of clear gap before the
 * label's own ink (never let the name's last glyph touch the "H") caps the name field at
 * 51-2 = 49px -- ONE more pixel than the previous (wrong) budget of 48, not the ~64px an
 * earlier estimate in this same investigation assumed (that number measured to the HP
 * BAR's start, x=183, not the label's own left edge).
 *
 * 49px is deliberately NOT "wide enough for a 10-glyph name at the font's default
 * advance": that would need 60px (BELLSPROUT / any all-wide-letter 10-char Gen-3
 * nickname; see tests/host_textfit_test.c), and even PokeDNA's shortest REAL 9-letter
 * species names in this save (SALAMENCE/METAGROSS, 54px at the default advance) do not
 * fit either -- retail's own "SALAMENCE" fits this exact 51px span only because
 * retail's font is narrower per glyph than PokeDNA's (see the commit that first moved
 * this column, 467f731). There is no more geometric room between the fixed name origin
 * and the retail-measured HP label to find without moving one of those two anchors, and
 * this file's own rule is that neither moves without a fresh measurement. So the field
 * is widened to its true safe maximum and left there -- the DEFAULT face still cannot be
 * patched into fitting a 9-glyph name here with kerning alone; see the field-history note
 * below.
 *
 * 2026-08-21 correction (party_draw_name_level, source/pdna_main.c): the row NOW draws
 * through the TIGHT face (ui_ptext_fit_shadow_tight, delta=1 -- same helper the box below
 * already used), same as the box. Through 2026-08-20 the row deliberately stayed on the
 * DEFAULT face and let a too-long name clip via ui_ptext_fit's own trailing '~'
 * ("SALAMEN~"), on the theory that a clipped name is safer than a kerned one -- true of
 * delta=2 (see UI_PTEXT_TIGHT_DELTA's comment in ui_font.h: it made adjacent glyphs' ink
 * genuinely OVERLAP, not just sit close -- docs/analysis-2026-08-19-party/
 * verify-2026-08-20-zoom.png) but NOT of delta=1, which the same investigation proved
 * touching-at-worst across all 96 glyphs, never overlapping. At delta=1 every real
 * 9-glyph species name in this save (SALAMENCE/METAGROSS/DRAGONITE, 45px tight) fits this
 * 49px column with several pixels of real slack instead of losing its tail -- a complete
 * name reads better than a clipped one when the tight face is provably safe, so both
 * branches take it now. A genuine 10-glyph name (BELLSPROUT, or this save's own
 * "28/01/2026" nickname) still does not fit even at delta=1 (50px tight against this 49px
 * budget) and clips by ~1px -- an accepted, documented residual (tests/
 * host_textfit_test.c's row-name block), not chased with kerning past delta=1 (capped,
 * see ui_font.h) or a narrower column. Closing that last pixel needs a genuinely narrower
 * party typeface, a separate job. */
#define PDNA_PTY_NAME_DX     23
#define PDNA_PTY_NAME_DY      3
#define PDNA_PTY_NAME_W      49              /* true safe max: HP_LBL_DX(74) - 2px gap */
#define PDNA_PTY_LVL_DY      14
#define PDNA_PTY_GEND_DX     62
#define PDNA_PTY_GEND_DY     14
#define PDNA_PTY_HP_LBL_DX   74
#define PDNA_PTY_HP_LBL_DY    4
#define PDNA_PTY_HP_BAR_DX   88
#define PDNA_PTY_HP_BAR_DY    6
#define PDNA_PTY_HP_BAR_W    50
#define PDNA_PTY_HP_BAR_H     7
#define PDNA_PTY_HP_NUM_DX   97
#define PDNA_PTY_HP_NUM_DY   14
#define PDNA_PTY_HP_NUM_W    43              /* row right edge minus the inset */

/* ---- box fields, relative to the box's own (x,y) — four stacked lines ----- */
/* BOX_NAME_DX was 15 (icon-rect overlap 5px, vs retail's measured 3px — MEASUREMENTS.md's
 * box section: icon x~5..35, name x~32..83) through the 2026-08-19 mechanical-fix pass,
 * which left it there: closing the extra 2px would have shrunk the field below
 * "TYRANITAR"'s own 52px width at the DEFAULT font advance, where it fit with 0px of
 * slack -- too tight to trust (see host_textfit_test.c's own ">=1px slack" rule). Moved
 * to 17 that same pass (matching retail's exact 3px overlap) on the strength of
 * tight-spacing, but that pass's tight delta (2) turned out to make ink genuinely
 * OVERLAP, not just sit close (see UI_PTEXT_TIGHT_DELTA's comment in ui_font.h) --
 * "TYRANITAR" rendered as illegible mush, not a tight 34px string.
 *
 * RE-CHECKED 2026-08-20 with the fix (UI_PTEXT_TIGHT_DELTA capped at 1): "TYRANITAR" at
 * delta=1 renders 43px, leaving 9px of real slack inside this 52px budget -- comfortably
 * clear, and every glyph pair in the font is proven touching-at-worst (never
 * overlapping) at delta=1 (see ui_font.h). DX=17 is kept: re-measuring the box's own
 * right border in the retail capture (retail-party-idle-f00.png) puts it at abs x~84-85
 * on this row, and PokeDNA's own box frame (PDNA_PTY_BOX_X + PDNA_PTY_BOX_W - 1 = 87)
 * sits right where this field's current right edge already lands (17+17+52=86) -- there
 * is no room to extend the column rightward without crowding PokeDNA's own border, so
 * the tight face is the correct tool here. (2026-08-21: the row column above now takes
 * the same tight face too, once delta=1 was proven never to overlap -- see PDNA_PTY_NAME_W's
 * own comment above for that history; the two columns are no longer on different faces.) */
#define PDNA_PTY_BOX_NAME_DX 17
#define PDNA_PTY_BOX_NAME_DY  8
#define PDNA_PTY_BOX_NAME_W  52
#define PDNA_PTY_BOX_LVL_DY  18
#define PDNA_PTY_BOX_GEND_DX 61
#define PDNA_PTY_BOX_GEND_DY 18
#define PDNA_PTY_BOX_HP_LBL_DX 15
#define PDNA_PTY_BOX_HP_LBL_DY 28
#define PDNA_PTY_BOX_HP_BAR_DX 29
#define PDNA_PTY_BOX_HP_BAR_DY 28
#define PDNA_PTY_BOX_HP_BAR_W  36            /* the box is too narrow for the row's 50 px bar */
#define PDNA_PTY_BOX_HP_NUM_DX 15
#define PDNA_PTY_BOX_HP_NUM_DY 38
#define PDNA_PTY_BOX_HP_NUM_W  55

#define PDNA_PTY_LVL_FMT     "Lv%u"          /* worst case "Lv100", 5 sys8 cols */
#define PDNA_PTY_HP_NUM_FMT  "%u/%u"         /* worst case "999/999", 7 sys8 cols */

/* Bottom message box + CANCEL button, y=133..160 — the same band retail's own
 * textbox occupies below its last list row (y=130). */
#define PDNA_PTY_MSG_X      2
#define PDNA_PTY_MSG_Y      133
/* This screen's OWN footer boundary, same pattern ui_layout.h documents for
 * PDNA_SET_FOOTER_Y / PDNA_DCY_FOOTER_Y / PDNA_FILT_FOOTER_Y: app_mon_menu is drawn
 * OVER this screen while ITS message box (PDNA_PTY_MSG_Y, above) is still on screen,
 * not the box/bank screens' UI_FOOTER_Y (150) — the per-mon popup used to be laid out
 * against the global UI_FOOTER_Y regardless of caller, so any party mon with 9+ action
 * rows got a 146px panel spanning y=2..148, landing squarely on the message box that
 * starts at 133. app_mon_menu now takes an explicit footer_y; the party overlay passes
 * this constant, box/bank/party_list keep passing UI_FOOTER_Y (unchanged behaviour). */
#define PDNA_PTY_FOOTER_Y   PDNA_PTY_MSG_Y
#define PDNA_PTY_MSG_W      170
#define PDNA_PTY_MSG_H      27
#define PDNA_PTY_MSG_PAD     6
#define PDNA_PTY_CANCEL_X   176
#define PDNA_PTY_CANCEL_Y   134
#define PDNA_PTY_CANCEL_W    62
#define PDNA_PTY_CANCEL_H    25
#define PDNA_PTY_MSG_CHOOSE "Choose a POKéMON."
#define PDNA_PTY_MSG_PLACE  "Place it in the party."
#define PDNA_PTY_MSG_W_BUDGET (PDNA_PTY_MSG_W - 2 * PDNA_PTY_MSG_PAD)
#define PDNA_PTY_CANCEL_W_BUDGET (PDNA_PTY_CANCEL_W - 2 * PDNA_PTY_MSG_PAD)

/* ---------------------------------------------------------------------------
 * PC-box party PANEL (party_strip_overlay, source/pdna_box.c) — REBUILT
 * 2026-08-20 against native-E12e-storage-partystrip.top.png, the ONLY
 * unobstructed retail frame (docs/analysis-2026-08-20-pcparty/MEASUREMENTS.md,
 * "PANE MEASUREMENTS" pass). The panel's own party-section conclusions in
 * that file's *first* pass ("5 slots at 24 px pitch", "the panel fully
 * replaces the box pane") were derived from frames where retail's own action
 * menu covered the pane and are WRONG — do not resurrect them.
 *
 * Guy's own words: "when choosing to move the pokemon within the PC, a side
 * menu of the party pops up ontop of the pokemon in the PC in the background
 * ... i like how i can grab a pokemon from party to box like so directly" —
 * and, on the first build of this: "It also shows 5 pokemon and not 6."
 *
 * Retail's shape: ONE big framed teal-dithered panel, x=[82,175] (94 px),
 * full screen height y=[0,159] (flush to y=0, no top border). It holds SIX
 * party tiles in retail's classic party layout: slot 1 sits ALONE, offset
 * left and vertically centred on the column (not top/bottom aligned); slots
 * 2-6 stack in a column on the panel's right side at 24 px pitch. A CANCEL
 * pill sits bottom-right, snug in the panel's own corner. The box grid stays
 * visible to the right of the panel (2 of its 6 columns fully, a sliver of a
 * 3rd) — see box_oam.c's boxoam_strip_open/strip_slot1 for how the tile
 * arithmetic keeps that true for THIS build's own (different) grid pitch.
 *
 * PokeDNA keeps the box grid + banner + PKMN DATA panel alive behind the
 * panel (Guy's own request, see above) rather than retail's own full pane
 * wipe — that divergence is deliberate, not a gap. */

/* ---- outer panel: bevel border bands + interior teal dither ---- */
#define PDNA_PCP_PANEL_X0   82           /* outer bbox left   (S: PANEL OUTER BBOX)  */
#define PDNA_PCP_PANEL_X1  175           /* outer bbox right, inclusive              */
#define PDNA_PCP_PANEL_Y0    0           /* flush to the very top -- no top border   */
#define PDNA_PCP_PANEL_Y1  159           /* full native height                       */

#define PDNA_PCP_LB_OUTER_X   82         /* left border, outer 1px band  #556171     */
#define PDNA_PCP_LB_MID_X0    83         /* left border, mid 2px band    #8CA9B4     */
#define PDNA_PCP_LB_MID_X1    84
#define PDNA_PCP_LB_HI_X0     85         /* left border, inner 3px HIGHLIGHT #3B6863 */
#define PDNA_PCP_LB_HI_X1     87

#define PDNA_PCP_RB_SH_X0    170         /* right border, inner 3px SHADOW #1F4842   */
#define PDNA_PCP_RB_SH_X1    172
#define PDNA_PCP_RB_MID_X0   173         /* right border, mid 2px band  #8CA9B4      */
#define PDNA_PCP_RB_MID_X1   174
#define PDNA_PCP_RB_OUTER_X  175         /* right border, outer 1px band #556171     */

#define PDNA_PCP_BB_SH_Y0    152         /* bottom border, inner 3px SHADOW #1F4842  */
#define PDNA_PCP_BB_SH_Y1    154
#define PDNA_PCP_BB_MID_Y0   155         /* bottom border, mid 3px band  #8CA9B4     */
#define PDNA_PCP_BB_MID_Y1   157
#define PDNA_PCP_BB_OUTER_Y0 158         /* bottom border, outer 2px band #556171    */
#define PDNA_PCP_BB_OUTER_Y1 159

#define PDNA_PCP_FILL_X0      88         /* interior teal-dither fill                */
#define PDNA_PCP_FILL_X1     169
#define PDNA_PCP_FILL_Y0       0
#define PDNA_PCP_FILL_Y1     151

/* ---- the party column (slots 2-6): x=137-166, 24 px pitch, 23 px tall tile,
 * 1 px gap of plain dither between tiles (rows 31/55/79/103) ---- */
#define PDNA_PCP_COL_X0      137
#define PDNA_PCP_COL_X1      166
#define PDNA_PCP_COL_Y0        8          /* top-of-first-tile              */
#define PDNA_PCP_SLOT_H       24          /* top-to-top pitch, confirmed exact */
#define PDNA_PCP_SLOT_VISH    23          /* visual tile height (< pitch: the gap) */
#define PDNA_PCP_VIS            5          /* slots 2-6: exactly 5, fixed (no scroll --
                                            * party caps at 6, and 1 (slot 1) + 5
                                            * (column) covers every legal party size) */

/* ---- slot 1 (offset, alone): x=89-118, SAME width as a column tile (30 px),
 * y=56-78 -- vertically centred on the column (matches column tile #3, the
 * middle one), NOT grid-aligned with the column horizontally. ---- */
#define PDNA_PCP_S1_X0         89
#define PDNA_PCP_S1_X1        118
#define PDNA_PCP_S1_Y0         56
#define PDNA_PCP_S1_Y1         78

/* ---- CANCEL pill: snug in the panel's own bottom-right corner, centred
 * under the column (not the whole panel, not slot 1). ---- */
#define PDNA_PCP_CANCEL_X0    136
#define PDNA_PCP_CANCEL_X1    167
#define PDNA_PCP_CANCEL_Y0    138
#define PDNA_PCP_CANCEL_Y1    149
#define PDNA_PCP_CANCEL_W_BUDGET (PDNA_PCP_CANCEL_X1 - PDNA_PCP_CANCEL_X0 + 1 - 2)  /* 1px
                                   border each side, zero extra pad -- CANCEL at TIGHT
                                   kerning measures exactly 30px, the same as this budget;
                                   see tests/host_textfit_test.c's pcp-cancel check. */

/* CHANGE 1 (2026-08-20, "make sure the pokemon are not trimmed"): retail's REAL icon
 * anchor within a party tile, measured by cross-correlating the actual 32x32 ROM icon
 * against native-E12e-storage-partystrip.top.png (docs/analysis-2026-08-20-pcparty's
 * own report has the full methodology) for SIX different species on this exact panel
 * (Salamence/Metagross/Tyranitar/Gengar matched pixel-exact, mean abs RGB error <22
 * per opaque pixel; Dragonite/Milotic matched shape-exact with a palette mismatch
 * attributable to the measurement script's own simplified colour decode, not the
 * position). Every one of the 6 tiles (the offset slot AND all 5 column rows) gave the
 * IDENTICAL relative offset -- this is retail's fixed placement rule, not a per-tile
 * fit: the icon's top-left sits at (tile_x0 + PDNA_PCP_ICON_DX, tile_y0 + PDNA_PCP_ICON_DY),
 * NOT a centred crop. The previous code centred with (tileH-32)/2 using C integer
 * division on a negative numerator ((23-32)/2 truncates toward zero to -4), which
 * biased the crop upward and ate more of the bottom than retail does -- retail's real
 * vertical offset is -8, not -4, because Gen-3 icon art keeps the creature weighted
 * toward the BOTTOM of its 32x32 canvas (built-in headroom at the top, for exactly
 * this kind of tile packing) rather than centring it. */
#define PDNA_PCP_ICON_DX      (-1)
#define PDNA_PCP_ICON_DY      (-8)

/* Occlusion range handed to boxoam_strip_open()/strip_restore_icons(): the box grid
 * cells the PANEL's own on-screen rectangle physically covers -- the outer bevel's
 * own extent, not just the column, now that the panel is a single big frame. */
#define PDNA_PCP_OCCLUDE_X0  PDNA_PCP_PANEL_X0
#define PDNA_PCP_OCCLUDE_X1  (PDNA_PCP_PANEL_X1 + 1)

/* SHOULD-FIX 6 (2026-08-20 review): the box's own footer ("L/R tab  A pick  DN",
 * draw_footer) is drawn UNDER this popup, at the s_tab_focus>=0 string -- exactly the
 * state this popup opens in -- and gets partially overpainted by the panel's own
 * bevel and the left PKMN DATA panel, leaving stray fragments on screen. Only
 * UP/DOWN/A/B (and now LEFT/RIGHT, MUST-FIX 8) are read here; party_strip_overlay
 * clears the row and draws this instead, sized to the ACTUAL free width to the right
 * of the panel (screen edge minus PDNA_PCP_OCCLUDE_X1, 62 px -- see
 * tests/host_textfit_test.c's own check). */
#define PDNA_LBL_PCP_FOOTER  "B back"

#endif /* PDNA_LAYOUT_H */
