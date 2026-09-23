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
  X(NV_CONTEST,    "Contests")                  \
  /* Present in the emulator build too: when a Pokemon ROM has been fused into this   */ \
  /* image (tools/fuse_rom.py) the map reads it from cartridge space, no SD needed.   */ \
  X(NV_MAP,        "Map")                       \
  X(NV_GB,         "GB import")   /* import from a Game Boy (Gen 1/2) save on the card */ \
  X(NV_XFER,       "Transfers")   /* BACKLOG #150 S150-11: the transfer-ledger reconcile */ \
  X(NV_SETTINGS,   "Settings")                  \
  X(NV_BACK,       "Back")

#define PDNA_NAV_COUNT_ONE(id, label) +1
#define PDNA_NAV_COUNT (0 PDNA_NAV_ITEMS(PDNA_NAV_COUNT_ONE))   /* == NV_COUNT */

/* The nav enum itself, shared: BACKLOG #48 lets a Game Boy session (pdna_gen12.c,
 * gb_session_core) open the SAME menu pdna_main.c's box screen does (through
 * app_nav_menu's availability mask), so both translation units need the identical
 * numeric IDs for NV_SETTINGS/NV_BACK/NV_TRAINER etc. Used to live as a private
 * enum inside pdna_main.c right above nav_menu() -- moved here, off the same
 * X-macro that already drives the labels/count above, so there is still exactly
 * ONE place that lists the items. A plain (non-typedef'd) enum in a header is
 * fine to include from several .c files: it declares constants, not a symbol, so
 * every translation unit just gets its own identical copy. */
#define PDNA_NAV_ENUM_ONE(id, label) id,
enum { PDNA_NAV_ITEMS(PDNA_NAV_ENUM_ONE) NV_COUNT };
#undef PDNA_NAV_ENUM_ONE

/* The column is wide enough for "Flags & counters" (85 px in the proportional face) and
 * two of them plus the gutter still clear the 240 px screen. */
#define PDNA_NAV_COL_W   98
#define PDNA_NAV_BAND_W  (PDNA_NAV_COL_W - 4)   /* selection bar width               */
#define PDNA_NAV_BAND_H  12                     /* bar height: contains the glyph box */
#define PDNA_NAV_LABEL_DX 4                     /* label inset inside the bar         */
/* A label is drawn PDNA_NAV_LABEL_DX into the bar, so this — not the screen — is its
 * budget. A wider label runs out past the highlight and into the second column. */
#define PDNA_NAV_LABEL_W (PDNA_NAV_BAND_W - PDNA_NAV_LABEL_DX)

/* BACKLOG #150 S150-11 decision 13: shrunk 11 -> 10 to seat the 21st row (NV_XFER).
 * PDNA_NAV_ROWS = (21+1)/2 = 11; PDNA_NAV_MH = 20 + 11*10 + 14 = 144 <= UI_FOOTER_Y
 * (150, source/ui_layout.h) -- both pass at 10 and fail at 9:
 *   host_textfit_test.c "nav panel height": PDNA_NAV_MH (144) <= UI_FOOTER_Y (150) -- OK.
 *   the bar-vs-row check: PDNA_NAV_BAND_DY + PDNA_NAV_BAND_H - 1 (-2+12-1=9) <=
 *     PDNA_NAV_ROW_H - 1 (10-1=9) -- OK, exactly, fails at 9-1=8 if ROW_H were 9.
 *   UI_FONT_CELL_H - 1 - PDNA_NAV_ROW_H (8-1-10=-3) <= -3 -- OK, exactly.
 *   last-row check: PDNA_NAV_HEAD + (PDNA_NAV_ROWS-1)*PDNA_NAV_ROW_H + UI_FONT_CELL_H - 1
 *     (20+10*10+7=127) <= PDNA_NAV_MH - PDNA_NAV_FOOT - 1 (144-14-1=129) -- OK. */
#define PDNA_NAV_ROW_H   10
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
/* BACKLOG #150 S150-15, decision 2: 11 glyphs = 88 px = exactly PDNA_MONMENU_ROW_W --
 * Guy's wording "ORIGINAL DATA" (13 glyphs) does not fit (mismatch 4). Grouped with
 * LEGALITY, the other "look at it" row (decision 1). */
#define PDNA_LBL_ORIGINAL    "GB ORIGINAL"
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
#define PDNA_LBL_VIEW        "VIEW"           /* read-only popup only, source not editable */
/* PDNA_LBL_MOVE_TO ("MOVE TO") used to live here -- the GB read-only popup's
 * own bespoke wording for its move-to-another-box row. Removed (UX-parity
 * audit, Guy 2026-09-07): that row now reuses PDNA_LBL_MOVE_TO_BOX, Gen 3's
 * OWN label for the same shape of action (a destination-box picker), rather
 * than a third, GB-only phrasing -- see pdna_main.c's app_mon_menu_readonly
 * for the reasoning. PDNA_LBL_PASTE_GB ("PASTE (GB)") used to live here too --
 * the empty-cell PASTE row's own bespoke wording. Removed (G1 review
 * BLOCKING-1, 2026-09-08): that row now reuses PDNA_LBL_PASTE_HERE, Gen 3's
 * OWN label for the identical empty-cell-paste action, rather than a
 * cosmetic "(GB)" qualifier -- the cross-generation conversion this paste
 * performs is explained in the confirm dialog the user sees before any
 * write, not in the row's own two words. */

/* BACKLOG #200 F3: A on a blocked grid cell (index >= the source's own capacity --
 * F1's dim/X-marked cells, F2's cursor already skips/clamps around them; this is
 * the defensive backstop for the one path F2 does not cover, an A press that
 * lands here anyway) -- never the EMPTY/CREATE/CANCEL menu, which has nothing
 * real to create there. */
#define PDNA_BOX_NO_SLOT     "No such slot in this game."

/* Every label either action popup can show, so the host test measures the strings the
 * menus actually draw. The X() entries are the macros above, not fresh literals. */
#define PDNA_MONMENU_LABELS(X)                                                        \
  X(PDNA_LBL_VIEW_EDIT) X(PDNA_LBL_ITEM) X(PDNA_LBL_LEGALITY) X(PDNA_LBL_ORIGINAL)    \
  X(PDNA_LBL_HATCH)                                                                  \
  X(PDNA_LBL_MOVE) X(PDNA_LBL_MOVE_TO_BOX) X(PDNA_LBL_COPY) X(PDNA_LBL_PASTE)         \
  X(PDNA_LBL_DUPLICATE) X(PDNA_LBL_TO_DAYCARE) X(PDNA_LBL_TO_GAME)                    \
  X(PDNA_LBL_EXPORT_PK) X(PDNA_LBL_TAKE_ITEM) X(PDNA_LBL_GIVE_ITEM)                   \
  X(PDNA_LBL_RELEASE) X(PDNA_LBL_CREATE) X(PDNA_LBL_PASTE_HERE) X(PDNA_LBL_CANCEL)    \
  X(PDNA_LBL_VIEW)

/* Read-only source popup: the header grows by one line per explanatory line above the
 * rows (the source's note, and the per-record "why this one is locked"). */
#define PDNA_ROMENU_MAX       10              /* VIEW/EDIT, ITEM?, LEGALITY, MOVE TO?, COPY?, DUPLICATE?,
                                                * TO DAY-CARE?, EXPORT?, RELEASE?, CANCEL -- one row dropped
                                                * 2026-09-05 (BACKLOG #41 follow-up): VIEW and EDIT merged
                                                * into one row, matching the Gen-3 menu's own
                                                * PDNA_LBL_VIEW_EDIT row. ITEM added 2026-09-10 (BACKLOG
                                                * #92, Gen 2 only). DUPLICATE/TO DAY-CARE/EXPORT added
                                                * 2026-09-15 (BACKLOG #93). */
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

/* PICK_ROWS 24x24-icon row preset (BACKLOG #107, pick_rows' PR_ROWH26 option, source/
 * pdna_pick.c): 26 px pitch, panel height 25 -- no shared-scanline overlap with the
 * next row (25 < 26, unlike the 9-on-8 FILT geometry above), so invariant (2) of the
 * ROW REPAINT RULE does not apply here. Named for pdna_contest.c's donor picker (the
 * mon list's icon column, mon_row) -- the first REAL caller of pick_rows' icon-row
 * preset (list_pick's own icon path had zero live callers and was removed rather than
 * kept as an unresolvable stack-guard blind spot; see pdna_pick.c's lp_row comment). */
#define PDNA_PR_ICON_ROWH     26
#define PDNA_PR_ICON_PANEL    (PDNA_PR_ICON_ROWH - 1)   /* 25 */
#define PDNA_PR_ICON_X         4
#define PDNA_PR_ICON_W        24                         /* == ITEM_ICON_W/H, item_icons.h --
                                                            * ui_icon_scaled() downscales the
                                                            * native 32x32 mon icon to fit    */
#define PDNA_PR_ICON_TEXT_X   32
#define PDNA_PR_ICON_TEXT_DY   8
#define PDNA_PR_ICON_MAXCOLS  24                         /* ui_truncate() column budget --
                                                            * 32 + 24*8 = 224 <= UI_SCR_W(240) */

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
/* Contest ribbon rank rows (BACKLOG #60) — the longest labels the field list has: */
#define PDNA_EDIT_RIB_COOL_LBL   "Cool Ribbon"
#define PDNA_EDIT_RIB_BEAUTY_LBL "Beauty Ribbon"
#define PDNA_EDIT_RIB_CUTE_LBL   "Cute Ribbon"
#define PDNA_EDIT_RIB_SMART_LBL  "Smart Ribbon"
#define PDNA_EDIT_RIB_TOUGH_LBL  "Tough Ribbon"
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
/* G1 review LOW-6 (2026-09-08): gbedit_confirm_keep()'s own title/verbs --
 * Gen 3's OWN create-flow wording, pdna_summary.c's confirm_keep(), verbatim
 * (that function's own three literals are not shared macros either; these
 * are an independent byte-for-byte copy, not a cross-file #include). Same
 * panel positions as PDNA_GBEDIT_CONFIRM_TITLE/A_WRITE/B_CANCEL. */
#define PDNA_GBEDIT_KEEP_TITLE     "Keep this Pokemon?"
#define PDNA_GBHOF_DISCARD_TITLE   "Discard changes?"       /* HoF EDIT MON: A = discard, B = stay (b194 review) */
#define PDNA_GBEDIT_KEEP_A         "A = write (backup first)"
#define PDNA_GBEDIT_KEEP_B         "B = discard it"

#define PDNA_GBEDIT_BADCHARSET_TITLE "CAN'T STORE THAT"
#define PDNA_GBEDIT_BADCHARSET_L1    "Not in this game's charset:"
#define PDNA_GBEDIT_BADNAME_L1       "The name was refused."
#define PDNA_GBEDIT_MOVE_LATE_TITLE  "NOT IN THIS GAME"
#define PDNA_GBEDIT_MOVE_LATE_L1     "That move is from a later"
#define PDNA_GBEDIT_MOVE_LATE_L2     "generation."
#define PDNA_GBEDIT_MOVE_DUP_TITLE   "ALREADY KNOWN"
#define PDNA_GBEDIT_MOVE_DUP_L1      "This Pokemon has that move"
#define PDNA_GBEDIT_MOVE_DUP_L2      "in another slot."
/* G1 review LOW-1 (2026-09-08): gbedit_adjust_refused()'s GBE_GENDER message --
 * a shiny of a heavily-skewed gender ratio can have no Atk DV that both flips
 * gender AND keeps the sparkle (gbe_flip_gender's own "best < 0" refusal,
 * gb_editor.c). Worded for either direction (not "always male"/"always
 * female" specifically), since either can be the one that is unreachable. */
#define PDNA_GBEDIT_GENDER_LOCKED_TITLE "CAN'T FLIP GENDER"
#define PDNA_GBEDIT_GENDER_LOCKED_L1    "A shiny here can't flip gender."

/* BACKLOG #95 review C2: gbe_flip_shiny's ON path (gb_editor.c) can be forced to move
 * gender when NO Atk DV both turns shiny on and keeps today's gender (some gender
 * ratios have no shiny combination for one of the two sexes). Reported through
 * msg_wait the moment it happens (gbedit_press/gbedit_adjust_checked, pdna_gbedit.c) --
 * NOT stacked into the write confirm screen's own issue/stale prose block, which the
 * textfit worst-case arithmetic above already leaves only 2 px of slack in (any third
 * wrapped block overflows into PDNA_GBEDIT_BAK_Y1). msg_wait's l1/l2 are each a single
 * ui_ptext_fit CLAMP, not a wrap, so the sentence is pre-split across two lines rather
 * than handed to msg_wait whole (which would silently truncate it, the exact failure
 * textfit exists to catch). Two pairs, not one template, because there is no on-device
 * sprintf for "was %s, now %s" and either direction can be the one that is unreachable
 * (same reasoning as GENDER_LOCKED_L1 above). */
/* gbmon C12: with the gender-ratio table this tree actually ships (Gen-3's five ratio
 * bytes read through pk_species_gender_ratio -> g2_gender_from_dv), the shiny Atk-DV
 * search in gb_editor.c's gbe_flip_shiny only ever picks from the 8 candidates with
 * bit 1 set {2,3,6,7,10,11,14,15} -- the minimum is 2, so a species whose FEMALE
 * threshold is dv<=1 (ratio 31, "7:1 male") has NO female candidate and is always
 * forced to male; every other real ratio (63/127/191/225) keeps a male candidate at
 * dv 14/15 even at its most female-skewed, so "forced to female" (the FEMALE_L1/L2
 * pair below) cannot fire against today's data -- confirmed by exhaustive check over
 * all five ratio bytes. Kept anyway, defensively, per the ORIGINAL author's own
 * comment above ("either direction can be the one that is unreachable"): a species
 * table for a different generation/ratio scheme could reintroduce the reverse case,
 * and there is no second call site to keep in sync if that ever happens -- the two
 * pairs are cheap (four short strings) next to the cost of a silently wrong message
 * if the unreachable direction ever becomes reachable again. */
#define PDNA_GBEDIT_SHINY_FORCED_TITLE   "GENDER FORCED"
#define PDNA_GBEDIT_SHINY_FORCED_MALE_L1   "No shiny female exists for"
#define PDNA_GBEDIT_SHINY_FORCED_MALE_L2   "this species; it is now male."
#define PDNA_GBEDIT_SHINY_FORCED_FEMALE_L1 "No shiny male exists for"   /* unreachable today, kept defensively -- see comment above */
#define PDNA_GBEDIT_SHINY_FORCED_FEMALE_L2 "this species; it is now female."

/* BACKLOG #95 review C4: this tree tracks no mailbox, so a Mail item set through the
 * ITEM row/picker (gb_editor.c's GBE_ITEM, pdna_gen12.c's gb_item_hook) leaves nothing
 * gbs_is_mail_item()'s own callers (gbs_delete/gbs_move, gb_session.h) can find --
 * they refuse the WHOLE PARTY rather than risk shifting SRAM bank 0's mail array
 * blind. Shown via app_confirm() before the item write commits; app_confirm draws its
 * own "A = yes / B = no" footer, same as every other use (pdna_layout.h's own header
 * note on app_confirm callers). */
#define PDNA_GBEDIT_MAIL_TITLE "SET THIS MAIL ITEM?"
#define PDNA_GBEDIT_MAIL_L1    "No mailbox: locks Move/Release."

/* BACKLOG #95 review C5: an Egg cannot hold an item (pack.asm
 * AnEggCantHoldAnItemText) -- refused both directions: a non-zero item on an Egg
 * (gb_set_held_item, gb_item_hook, pdna_gbedit.c's GBE_K_ITEM branch) and turning EGG
 * on while an item is already held (gb_set_egg, gb_editor.c's GBE_EGG row). */
#define PDNA_GBEDIT_EGG_ITEM_TITLE "EGG CAN'T HOLD ITEMS"
#define PDNA_GBEDIT_EGG_ITEM_L1    "Remove the held item first."

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

/* box_save's SF_ERR_RENAME switch (source/pdna_bank.c, BACKLOG #150 S150-0 review
 * F4/XFER-C1): same shape as app_commit/gb_persist's own triage above, but box_save
 * names the file only in the TMP_ONLY case (boxNN.box.tmp, siprintf'd from the box's
 * own path at the call site) -- the other three cases don't send anyone hunting a
 * filename, so they stay static. msg_wait's (28, .., 184) proportional clamp, same as
 * every macro above. */
#define PDNA_BANKSAVE_TMPONLY_TITLE     "BOX NOT IN PLACE"
#define PDNA_BANKSAVE_TMPONLY_L2        "Card dropped it. Rename .tmp on a PC."
#define PDNA_BANKSAVE_TMPANDOLD_TITLE   "BOX NOT SAVED"
#define PDNA_BANKSAVE_TMPANDOLD_L1      "Old box intact."
#define PDNA_BANKSAVE_TMPANDOLD_L2      "Try saving again."
#define PDNA_BANKSAVE_LOST_TITLE        "BOX LOST"
#define PDNA_BANKSAVE_LOST_L1           "Card kept neither copy."
#define PDNA_BANKSAVE_LOST_L2           "Restore the .bak on a PC."
#define PDNA_BANKSAVE_UNCONFIRMED_TITLE "UNCONFIRMED"
#define PDNA_BANKSAVE_UNCONFIRMED_L1    "Box looks correct."
#define PDNA_BANKSAVE_UNCONFIRMED_L2    "Could not re-check the card."

/* BACKLOG #163: the Bank screen's persistent per-box banner (pdna_bank_box_unsaved) --
 * drawn every frame the loaded box is the one box_save() most recently refused to write,
 * so leaving it on screen is never mistaken for a save that landed. Deliberately the same
 * text as PDNA_BANKSAVE_TMPANDOLD_TITLE (it is describing the same fact), kept as its own
 * define since the banner and that one dialog can change independently. */
#define PDNA_BANK_UNSAVED_BANNER        "BOX NOT SAVED"

/* review F5 (hygiene): the flush-retry loop and the exit prompt's own follow-up
 * messages (pdna_bank.c: box_save_or_keep_dirty, pdna_bank_show's exit block) were
 * inline literals -- named here alongside the banner they share a title with, so
 * tests/host_textfit_test.c can pin them the same way as every other on-screen string
 * in this file. */
#define PDNA_BANK_RETRY_L1              "Retry the save?"
#define PDNA_BANK_LOST_L1               "Could not save this box."
#define PDNA_BANK_LOST_L2               "Its edits are lost."
#define PDNA_BANK_DISCARD_L1            "Edits discarded."
#define PDNA_BANK_DISCARD_L2            "This box's edits are lost."

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
/* G1 review MEDIUM-2 (2026-09-08): CREATE's own ROM scan (gb_create_locate_rom +
 * gb_create_learn, together up to ~185,000 read() calls on Crystal.gbc, measured)
 * freezes the screen long enough to look hung -- but s_busy()'s own title
 * ("Saving - do not power off") would be a LIE here: nothing is written, and
 * powering off mid-read risks nothing but re-doing the read. A separate title/
 * line pair, same panel shape as s_busy(), honest text instead. */
#define PDNA_GBCREATE_BUSY_TITLE   "Reading your ROM..."
#define PDNA_GBCREATE_BUSY_LINE   "This can take a moment."

#define PDNA_GBEDIT_READONLY_TITLE "READ-ONLY"

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

/* BACKLOG #93: DUPLICATE on the read-only mon menu (gb_dup_hook, pdna_gen12.c).
 * The once-per-visit sidecar warning (decision 2, the s_id_warned/gbtr_id_edit_ok
 * idiom) and the success message naming the landing slot (decision 1). */
#define PDNA_GBEDIT_DUP_SIDECAR_TITLE "Came from Gen 3."
#define PDNA_GBEDIT_DUP_SIDECAR_L1    "Only one copy can go back."
#define PDNA_GBEDIT_DUP_TITLE         "DUPLICATED"
/* D5 (review-opus, BACKLOG #93): DUPLICATE on a party mon used to hit gbs_insert's
 * own GBS_ERR_ARG (a party-shaped record handed to a box-only destination,
 * gb_session.h's own gbs_insert() header) and show its raw status text, "bad
 * argument" -- accurate but not a sentence a player asked for. Reuses
 * PDNA_GBEDIT_DAYCARE_PARTY_TITLE ("CAN'T"), the same twin refusal TO DAY-CARE
 * already gives the party pseudo-box, for one title both share. */
#define PDNA_GBEDIT_DUP_PARTY_L1      "Can't duplicate a party mon."

/* BACKLOG #93: TO DAY-CARE on the read-only mon menu (gb_daycare_hook, pdna_gen12.c).
 * The full/confirm/success strings are Gen 3's own app_to_daycare/gbdc_deposit literals,
 * reused verbatim (decision-mandated); these two are new, GB-specific structural
 * refusals (step 3: "with a message, never a bare buzz"). */
#define PDNA_GBEDIT_DAYCARE_PARTY_TITLE "CAN'T"
#define PDNA_GBEDIT_DAYCARE_PARTY_L1    "The party can't go to Day-Care that way."
#define PDNA_GBEDIT_DAYCARE_EGG_TITLE   "EGG"
#define PDNA_GBEDIT_DAYCARE_EGG_L1      "An Egg can't be left at the Day-Care."

/* BACKLOG review (post-#40(c)): gb_move_hook's own refusals reused
 * PDNA_GBEDIT_REFUSED_TITLE ("EDIT REFUSED"), same mismatch #40(c) fixed for PASTE — a
 * MOVE TO was never an edit either. Same (28, 184) proportional clamp. */
#define PDNA_GBEDIT_MOVE_REFUSED_TITLE "MOVE REFUSED"

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
/* BACKLOG #187/#193, F3: DUPLICATE reuses gb_pick_box() (same layout/gating) for its
 * own full-source-box destination picker -- a distinct title so the screen never
 * reads "MOVE TO" while the mon is actually being copied, not relocated. */
#define PDNA_GBEDIT_PICKBOX_DUP_TITLE "DUPLICATE TO"
#define PDNA_GBEDIT_PICKBOX_CREATE_TITLE "CREATE IN"
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
 * BACKLOG #41 slice E1 (Guy, 2026-09-05 -- docs/SPRITE-ERA-DESIGN.md sec 3): the
 * Game Boy summary restyled to the SAME Gen-3 CARD chrome as pdna_summary.c —
 * "I prefer the summary edit design to be like we did for gen 3 ... [with] less
 * editable stats". Replaces the earlier retail-page macro set below (three cards,
 * fixed sys8 across the FULL 240 px screen): this version shares
 * PDNA_SUM_CARD_X/W (98/138, above) with the Gen-3 summary, so every card body
 * lives in the SAME 138 px column pdna_summary.c's own card_info/card_skills do,
 * NOT the full screen width the old retail-page layout used. Selection is drawn
 * by the shared MOVING OUTLINE (pdna_summary_sel_frame_*) rather than an inline
 * per-row highlight box, so — like pdna_summary.c's own card_info/card_skills —
 * these rows carry no "selected" colour swap of their own.
 *
 * Fixed sys8 (ui_text, 8 px/glyph) for short label/value columns, exactly
 * pdna_summary.c's own card_info/card_skills convention; PROPORTIONAL
 * (ui_ptext_fit) only for sentence-shaped strings that would not fit a fixed
 * column at this width (an item name, the EXP line, the box-record note, the
 * ORIGIN card's two prose lines). Every literal here is checked at its real x
 * inside the 138 px card by tests/host_textfit_test.c. */
#define PDNA_GBSUM_VIEW_CHIP   "VIEW"
#define PDNA_GBSUM_EDIT_CHIP   "EDIT"
/* CREATE mode's chip (BACKLOG #50 UX-parity, Guy 2026-09-07): the SAME text
 * and colour as pdna_summary.c's own render_card() draws for a Gen-3 create
 * ("NEW", UI_OK fill + UI_PANEL text, x=12 -- "NEW" centers narrower than
 * "EDIT"/"VIEW" in that same 50 px chip, which is why its x differs from
 * theirs too, matching the Gen-3 chip's own x=12 exactly). */
#define PDNA_GBSUM_NEW_CHIP    "NEW"
#define PDNA_GBSUM_CARD_INFO   "INFO"
#define PDNA_GBSUM_CARD_SKILLS "SKILLS"
#define PDNA_GBSUM_CARD_MOVES  "MOVES"
#define PDNA_GBSUM_CARD_ORIGIN "ORIGIN"
/* U/D = move the field cursor, <>edit = LEFT/RIGHT adjusts it, A = press (osk/
 * picker/jump-to-extreme), L/R = flip card, B = back to VIEW (NOT leave the
 * screen — the screen's own B-in-VIEW does that, same two-B contract as before).
 * SELECT (drop to the flat field-list editor, pdna_gbedit.c) stays documented in
 * pdna_gbsummary.h rather than the footer, same reason as the old macro set. */
#define PDNA_GBSUM_FOOT_EDIT    "A ok <>edit U/D L/R card B"
/* CREATE mode's two footers (BACKLOG #50 UX-parity): no U/D-scroll-to-another-
 * mon exists (this is the only mon in the visit, same as Gen 3's own create),
 * and START keeps it from EITHER sub-mode -- mirrors pdna_summary.c's own
 * PDNA_SUM_FOOT_CREATE_EDIT/PDNA_SUM_FOOT_CREATE text closely (adapted to this
 * screen's own "L/R card" wording, not Gen 3's card-dot-only convention). */
#define PDNA_GBSUM_FOOT_CREATE_EDIT "A ok <>edit L/R card START"
#define PDNA_GBSUM_FOOT_CREATE      "A edit  L/R card  START keep"
#define PDNA_GBSUM_FOOT_VIEW    "A edit  U/D mon  L/R  SEL  B"
#define PDNA_GBSUM_FOOT_VIEW_RO "U/D mon  L/R card  SEL  B"

/* Card 0 INFO: label at PDNA_SUM_CARD_X, value at +VAL_DX — the SAME split
 * IDEA pdna_summary.c's own card_info uses for its 138 px INFO_W, but +52, not
 * pdna_summary.c's own +48: this card's own labels ("Friend", "Status", and
 * the Egg relabel "Hatch") are 6 columns (48 px) wide, i.e. EXACTLY 48 px at
 * PDNA_SUM_CARD_X — flush against a +48 value column with no gap at all.
 * tests/host_textfit_test.c's own card-0-label check (budgeted to this macro,
 * not the screen edge) is what caught it; +52 buys a 4 px gap instead. Everything
 * that already lives on the shared left panel (species, level, gender, type,
 * shiny/egg/Pokerus tag — pdna_summary_draw_left) is NOT repeated here; nickname
 * and level keep their rows because THIS is where the edit control lives, the
 * left panel only displays what they resolve to. Gender is the one exception
 * (BACKLOG #51): the left panel shows the sign but has no control for it (its
 * mon is a throwaway conversion, gbsum_convert_left), so a Gender row lives
 * here too, right where the edit control belongs — the SAME "displayed
 * elsewhere, edited here" split the rest of this comment describes. */
#define PDNA_GBSUM_VAL_DX       52
#define PDNA_GBSUM_LBL_NAME    "Name"
#define PDNA_GBSUM_LBL_OT      "OT"
#define PDNA_GBSUM_LBL_ID      "ID"
#define PDNA_GBSUM_LBL_LV      "Lv"
#define PDNA_GBSUM_LBL_GENDER  "Gender"      /* BACKLOG #51, Gen 2 gender-having species
                                              * only (gbe_has_gender_row) — 6 cols, exactly
                                              * the 48 px label budget above */
#define PDNA_GBSUM_LBL_ITEM    "Item"
#define PDNA_GBSUM_LBL_FRIEND  "Friend"
#define PDNA_GBSUM_LBL_EGGC    "Hatch"       /* the SAME GBE_FRIEND byte, on an Egg
                                              * (gb_editor.c's gbe_label_of: "Egg
                                              * cycles" there) — 5 cols, inside the
                                              * 52 px label column */
#define PDNA_GBSUM_LBL_STATUS  "Status"      /* Gen 1 only — see gb_edit.h, no Gen-2
                                              * status getter exists */
#define PDNA_GBSUM_LBL_PKRS    "PKRS"        /* Gen 2 only */
/* OT/Item/Level/Nickname values go through the shared field_row() -> gbe_value()
 * path (gb_editor.c's own formatting: plain decimal, "None" for no held item),
 * the same as every other editable GBE_* row in this tree. ID is drawn
 * zero-padded instead (matching pdna_summary.c's own "TID %05u" on its ORIGIN
 * card) — a 16-bit trainer id is always exactly 5 digits at this width, so
 * there is no separate worst case to check. */
#define PDNA_GBSUM_ID_FMT      "%05u"
/* "S15  15d left" (pokecrystal stats_screen.asm:590-604's strain/days split —
 * both nibbles, so both can be two digits) is 13 cols / 104 px, wider than the
 * value column has (PDNA_SUM_CARD_W - VAL_DX = 86 px) — drawn PROPORTIONALLY
 * (ui_ptext_fit), like the EXP line below, so it can only clip itself. */
#define PDNA_GBSUM_PKRS_DAYS_FMT   "S%u  %ud left"
#define PDNA_GBSUM_PKRS_IMMUNE_FMT "S%u  immune"
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
/* "EXP 1640000  +999999" (worst case) proportionally fit to the 138 px card. */
#define PDNA_GBSUM_EXP_FMT      "EXP %lu  +%lu"
#define PDNA_GBSUM_EXP_MAX_FMT  "EXP %lu  MAX"

/* Card 1 SKILLS: two rows per stat, not the old flat list's one-row 4-column grid
 * (label/value/DV/stat-exp spanning the whole 240 px screen) — that grid does not
 * fit 138 px. Row 1 (main): "Atk 999/999" (party) or "Atk -" (box). Row 2
 * (indented, its own two registered fields): "DV 15" then "SE 65535". */
#define PDNA_GBSUM_STAT_HP     "HP"
#define PDNA_GBSUM_STAT_ATK    "Atk"
#define PDNA_GBSUM_STAT_DEF    "Def"
#define PDNA_GBSUM_STAT_SPE    "Spe"
#define PDNA_GBSUM_STAT_SPA    "SpA"
#define PDNA_GBSUM_STAT_SPD    "SpD"
#define PDNA_GBSUM_STAT_SPC    "Spc"          /* Gen 1: Special, not split */
#define PDNA_GBSUM_STAT_CURMAX_FMT "%u/%u"
#define PDNA_GBSUM_STAT_DASH       "-"
#define PDNA_GBSUM_STAT_DV_FMT     "DV %u"
#define PDNA_GBSUM_STAT_SE_FMT     "SE %u"
/* Row-1 label/value split, from PDNA_SUM_CARD_X: the label ("HP".."SpD", <=3
 * cols/24 px) then the computed value ("999/999" worst case, 7 cols/56 px, so
 * VAL_DX needs at least 24 -- 26 leaves a 2 px gap). */
#define PDNA_GBSUM_STAT_VAL_DX    26
/* Row-2 offsets from PDNA_SUM_CARD_X. DV at +8 ("DV 15", 5 cols/40 px, ends at
 * 48); SE at +56 ("SE 65535", 8 cols/64 px, ends at 120) — both inside the
 * 138 px card with margin either side. */
#define PDNA_GBSUM_STAT_DV_DX      8
#define PDNA_GBSUM_STAT_SE_DX     56
/* Shown once, under the last stat row, for a BOX record only (no live HP/stats
 * exist to show — gb_get_stat/gb_get_current_hp are party-only by contract). */
#define PDNA_GBSUM_BOX_STAT_NOTE  "Computed on withdrawal"

/* Card 2 MOVES reuses pdna_summary.c's own card_moves geometry and format macros
 * VERBATIM (PDNA_SUM_PP_X_DX/PP_W/PP_FMT/PP_UPS_FMT, above) — same 74 px name
 * column, same "PP%u/%u[+%u]" cell registered under GBE_PPU0+i only (pressing it
 * cycles PP Ups, exactly like Gen 3's F_PPU0+i; current PP is not independently
 * editable from this card there either). The one difference: a Game Boy record
 * carries no move TYPE this tree can read (Gen 1's ids differ from Gen 3's own
 * numbering and gb_editor.c has no accessor for it), so there is no type-badge
 * line under it. */

/* Card 3 ORIGIN — new in E1 (the old 3-card design had no such card). The honest
 * "this is a throwaway Gen 3 PREVIEW, not a real transfer" story
 * (gen12_convert.h: "THERE WAS NEVER AN OFFICIAL GEN 1/2 -> GEN 3 TRANSFER"),
 * the record's OWN generation (the `note` string every caller already passes —
 * "Gen 1 record" / "Gen 2 record"), the Gen-1 type-byte chip (Gen 2's own record
 * carries no type field; its type is read straight off the species table, which
 * the shared left panel already shows), and the Gen-3-to-Game-Boy sidecar link
 * status (docs/GEN3-TO-GB-SIDECAR-DESIGN.md). */
#define PDNA_GBSUM_ORIGIN_ART_L1      "Art: Gen 3 preview only"
#define PDNA_GBSUM_ORIGIN_ART_L2      "Edits change the GB save"
#define PDNA_GBSUM_ORIGIN_TYPE_LBL    "Type"
#define PDNA_GBSUM_ORIGIN_SIDECAR_YES "Sidecar: Yes"
#define PDNA_GBSUM_ORIGIN_SIDECAR_NO  "Sidecar: No"

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

/* The merge confirm screen -- BACKLOG #150 S150-9 decision 6: app_sidecar_confirm
 * (which owned this comment) is DELETED this slice, replaced by the shared
 * app_xfer_merge_screen (pdna_main.c); the panel geometry below is that screen's
 * chrome now. PDNA_SIDECAR_L_EVOLVED and PDNA_SIDECAR_A_PASTE were
 * app_sidecar_confirm's own (never shared with app_xferrestore_confirm) and are
 * DELETED with it, decision 13's "delete the dead ones... no orphan strings" --
 * PDNA_XFERMERGE_ROW_SPECIES/KEEP/TAKE below replace the first, the new screen's
 * hint strings replace the second. Every OTHER string here (L_LEVEL/L_MOVES/
 * L_RENAMED/L_RENAME_REFUSED/L_ITEM_IGNORED/L_EVS/B_CANCEL/CONFIRM_TITLE) stays --
 * app_xferrestore_confirm still uses them until this screen replaces ITS call site
 * too (a later commit), and PDNA_SIDECAR_L_EVS/CONFIRM_TITLE are reused directly by
 * the new screen (decision 7/13). */
#define PDNA_SIDECAR_CONFIRM_TITLE   "RESTORED FROM THE SIDECAR"
#define PDNA_SIDECAR_L_LEVEL         "Its level changed."
#define PDNA_SIDECAR_L_MOVES         "Its moves changed."
#define PDNA_SIDECAR_L_RENAMED       "It was renamed."
#define PDNA_SIDECAR_L_RENAME_REFUSED "Rename refused, kept the old name."
#define PDNA_SIDECAR_L_ITEM_IGNORED  "GB item ignored; kept the original."
#define PDNA_SIDECAR_L_EVS           "EVs restored from the sidecar."
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

/* BACKLOG #120 S2: a cross-generation drop denied in drop_held (pdna_box.c) -- either
 * side of the transfer is BOXSCOPE_GB and there is no lift path yet (S3/S4 add one). */
#define PDNA_XFER_NOGEN_TITLE "NOT ACROSS GENERATIONS"
#define PDNA_XFER_NOGEN_L1    "This Pokemon cannot move"
#define PDNA_XFER_NOGEN_L2    "between these two saves yet."

/* BACKLOG #150 S150-3 decision 3/10: a native "GBC1" Bank cell dropped anywhere other
 * than back into the Bank (drop_held's dominating xg_native_escape_denied() call). The
 * hand is NOT emptied -- msg_wait, then the caller keeps holding, like PDNA_XFER_NOGEN. */
#define PDNA_XFER_NATIVE_TITLE "STAYS IN THE BANK"
#define PDNA_XFER_NATIVE_L1    "This Game Boy Pokemon can"
#define PDNA_XFER_NATIVE_L2    "only move inside the Bank."

/* BACKLOG #199 (lane b199): the GB -> Bank UP drop now calls lift_up() at DROP time
 * (drop_held_up, pdna_box.c), not at grab time -- a refusal there (the one-time
 * origin prompt cancelled, a sidecar ledger entry already exists for this mon, or
 * pdna_bank_next_serial()'s own SD write failed) happens over the BANK grid, mid-
 * carry, so it must say so rather than silently doing nothing: the hand keeps
 * holding, same "still holding" shape as every other refusal in drop_held_up. */
#define PDNA_XFER_LIFT_REFUSED_TITLE "NOT MOVED TO THE BANK"
#define PDNA_XFER_LIFT_REFUSED_L1    "This Pokemon could not be"
#define PDNA_XFER_LIFT_REFUSED_L2    "packed for the Bank."

/* BACKLOG #150 S150-9 decision 6/13: app_xferrestore_confirm (S150-8b review F3/D6)
 * is DELETED this commit -- its only call site (pc_bank_restore_up) now shows the
 * shared app_xfer_merge_screen. PDNA_XFERRESTORE_L_LOSS/L_EVOLVED/A_OK were that
 * screen's own (never reused) and are deleted with it. PDNA_XFERRESTORE_TITLE
 * STAYS: it is reused verbatim as PDNA_XFERMERGE_TITLE_DOWN (decision 13). */
#define PDNA_XFERRESTORE_TITLE  "BACK TO ITS ORIGINAL"

/* BACKLOG #150 S150-9 decision 6/13: the shared per-field MERGE screen
 * (app_xfer_merge_screen, pdna_main.c) -- replaces app_sidecar_confirm (this commit)
 * and app_xferrestore_confirm's call site in pc_bank_restore_up (a later commit).
 * Same panel chrome as the two screens it replaces (PDNA_SIDECAR_PANEL_x, TEXT_x,
 * LINE_x, EVS_GAP above). PDNA_SIDECAR_L_EVS is reused verbatim as the UP direction's
 * always-shown read-only row (decision 7); PDNA_SIDECAR_B_CANCEL is reused for the
 * one-row-only degenerate footer case is not needed here -- the new hint strings
 * below always carry both keys on one line. */
#define PDNA_XFERMERGE_TITLE_UP      PDNA_SIDECAR_CONFIRM_TITLE   /* reuse, decision 13 */
#define PDNA_XFERMERGE_TITLE_DOWN    PDNA_XFERRESTORE_TITLE       /* reuse, decision 13 */
#define PDNA_XFERMERGE_ROW_SPECIES   "Evolved abroad"
#define PDNA_XFERMERGE_ROW_LEVEL_FMT "Level %u > %u"
#define PDNA_XFERMERGE_ROW_MOVES     "Moves changed"
#define PDNA_XFERMERGE_ROW_NICK      "Nickname changed"
#define PDNA_XFERMERGE_KEEP          "KEEP"
#define PDNA_XFERMERGE_TAKE          "TAKE"
#define PDNA_XFERMERGE_RO_EVOLVED    "Evolved abroad; kept as it left."
#define PDNA_XFERMERGE_RO_ITEM_DROP  "Held item stays behind."
#define PDNA_XFERMERGE_RO_ITEM_BACK  "Its GB item comes back with it."
#define PDNA_XFERMERGE_RO_RENAME     "New name can't be spelled; kept."
#define PDNA_XFERMERGE_RO_MOVE       "A move can't exist here; kept."
#define PDNA_XFERMERGE_HINT_TOGGLE   "A flip  START apply  B cancel"
#define PDNA_XFERMERGE_HINT_RO       "A ok  B cancel"   /* review D2: A commits with zero toggle rows */

/* BACKLOG #150 S150-9 decision 8: the ledger's RESTORED/PENDING state refusals at a
 * restore lookup (site 1, pc_bank_restore_up; site 2, the GB lift, a later commit). */
#define PDNA_XFERDUP_TITLE   "ALREADY RESTORED"
#define PDNA_XFERDUP_L1      "The Bank has its original."
#define PDNA_XFERDUP_L2      "Release this copy instead."

/* review F5: the rc == -1 "unreadable record" refusal gets its own message instead of
 * a bare snd_error() -- decision 9's own strings. */
#define PDNA_XFERREC_TITLE "TRANSFER RECORD UNREADABLE"
#define PDNA_XFERREC_L1    "Nothing was moved."
#define PDNA_XFERREC_L2    "Try again, or check the card."
/* BACKLOG #187/#191a, F2: dropping a held GB mon onto an OCCUPIED cell -- a Game Boy
 * box is a packed, count-prefixed list (gb_session.c), not an addressable grid, so
 * there is no swap primitive to land two mons at once the way the Gen-3 PC/Bank grid
 * does. Not a silent beep (decision F2): the drop is refused with a dialog that says
 * why, same posture PDNA_XFER_NOGEN/PDNA_XFER_NATIVE already use for their own
 * cross-scope refusals. */
#define PDNA_XFER_GBSWAP_TITLE "CAN'T SWAP HERE"
#define PDNA_XFER_GBSWAP_L1    "A Game Boy box can't swap"
#define PDNA_XFER_GBSWAP_L2    "two Pokemon at once."

/* BACKLOG #150 S150-8 decision 13 / D-Q2/D-Q3: the DOWN-converting edge's own
 * strings -- one unpromoted transfer at a time (decision 9), the ledger-full
 * evict-then-refuse case (decision 8), the Gen-1<->Gen-2 time-capsule refusal
 * (decision 14), and the flush-failure report (decision 10). Same <=28-column
 * discipline as every other fixed title in this file (tests/host_textfit_test.c's
 * own PF(text, 28, 184) rows). The old "PC BOX FIRST" refusal macros were here --
 * DEAD, deleted by BACKLOG #174 (S150-8c): the party is now a real native landing,
 * not a refusal. */

#define PDNA_XFER_SAVEFIRST_TITLE "SAVE FIRST"
#define PDNA_XFER_SAVEFIRST_L1    "One transfer is waiting for"
#define PDNA_XFER_SAVEFIRST_L2    "the game save. START > SAVE."

/* BACKLOG #175 (S150-8d) D16: the mid-session SAVE NOW? confirm that replaces the
 * plain SAVE FIRST wall at the ONE site that used to show it (gb_bank_down_gen3's
 * pre-flight 16(g)) -- PDNA_XFER_SAVEFIRST_* above stays live for the two
 * restore-side refusals (D18, decision 18), untouched by this lane. L1 reuses
 * PDNA_XFER_SAVEFIRST_L1 verbatim (siprintf'd together with L2 below) so the two
 * screens can never drift apart in wording. */
#define PDNA_XFER_SAVENOW_TITLE "SAVE NOW?"
#define PDNA_XFER_SAVENOW_L1    PDNA_XFER_SAVEFIRST_L1
#define PDNA_XFER_SAVENOW_L2    "the game save. Save it now?"

/* BACKLOG #174 (S150-8c) D7: the party arm's own always-drawn loss row -- the ONE
 * thing the destination changes (a fully-healed party mon vs. a box cell). */
#define PDNA_XFER_PARTYLAND_L1 "Joins your party, fully healed."

/* BACKLOG #174 (S150-8c) D2/D8: the party site's own pre-flights, ALL before the
 * conversion arm -- a non-ADD target (SWAP is impossible for a bank origin, D2's
 * reason: `can_swap_now` is already false and party_place_held forces can_swap=
 * false for a bank origin, so refusing it BEFORE the (irreversible-ish) ledger
 * write is strictly better than the post-conversion CAN'T SWAP the user would
 * otherwise see), and a full party (hoisted above party_place_held's own PARTY
 * FULL check, which runs AFTER a landing would have been recorded). */
#define PDNA_XFER_PARTYSWAP_TITLE  "CAN'T SWAP HERE"
#define PDNA_XFER_PARTYSWAP_L1     "A Bank Pokemon can only join"
#define PDNA_XFER_PARTYSWAP_L2     "an empty party slot."
#define PDNA_XFER_PARTYFULL3_TITLE "PARTY IS FULL"
#define PDNA_XFER_PARTYFULL3_L1    "Send a party Pokemon to a box,"
#define PDNA_XFER_PARTYFULL3_L2    "then try again."

#define PDNA_XFER_TOOMANY_TITLE "TOO MANY TRANSFERS"
#define PDNA_XFER_TOOMANY_L1    "Too many transfer records"
#define PDNA_XFER_TOOMANY_L2    "for this Pokemon."

/* BACKLOG #150 S150-11 decision 18 -- the Bank-open reconcile prompt's own strings
 * (a %d-bearing suffix, same siprintf("%d %s") shape as PDNA_SIDECAR_RECON_TITLE_
 * SUFFIX above). Worst case measured in tests/host_textfit_test.c: "64 POKEMON IN
 * TWO PLACES". */
#define PDNA_XRC_DUP_TITLE_SUFFIX "POKEMON IN TWO PLACES"
#define PDNA_XRC_DUP_L1           "Remove the duplicates?"

/* BACKLOG #150 S150-11 decision 11 (#176) -- a failed PC commit leaves the Bank
 * cell untouched; this tells the player their Pokemon is still safe. */
#define PDNA_XFER_NOTSAVED_TITLE "TRANSFER NOT SAVED"
#define PDNA_XFER_NOTSAVED_L1    "The save was not confirmed."
#define PDNA_XFER_NOTSAVED_L2    "Your Pokemon is still in the Bank."

/* BACKLOG #150 S150-11 decision 18/13/14 -- the TRANSFERS screen (start-menu row,
 * list, per-row action popup, APPLY/discard confirms, the loss confirm). Every
 * title/line below is a plain literal through msg_wait/app_confirm's shared (28,
 * .., 184) clamp -- tests/host_textfit_test.c's PF(text, 28, 184) rows, beside the
 * PDNA_XFER_NOTSAVED_* rows above, pin every one. */
#define PDNA_XRC_TITLE      "TRANSFER RECORDS"
#define PDNA_XRC_EMPTY_L1   "No transfer records."
#define PDNA_XRC_EMPTY_L2   "Records appear after a"
#define PDNA_XRC_EMPTY_L3   "Bank transfer."
#define PDNA_XRC_MORE       "More records not shown."
#define PDNA_XRC_FOOT       "A act  SEL info  START apply"

/* Per-row action labels (decision 8), listed on the action popup. */
#define PDNA_XRC_ACT_REMOVE  "Remove duplicate"
#define PDNA_XRC_ACT_RELEASE "Release Gen-3 copy"
#define PDNA_XRC_ACT_RESTORE "Restore to Bank"
#define PDNA_XRC_ACT_DELETE  "Delete record"
#define PDNA_XRC_ACT_REKEY   "Re-link record"
#define PDNA_XRC_ACT_CANCEL  "Cancel"

/* The loss confirm (decision 2/8d) -- DELETE behind a second confirm naming the
 * loss, for any row whose original80 is the last surviving copy. */
#define PDNA_XRC_LOSS_TITLE  "ORIGINAL BYTES LOST"
#define PDNA_XRC_LOSS_L1     "This record is the last copy."

/* RESTORE TO BANK's own failure line (decision 8c/19). */
#define PDNA_XRC_NOBANK_L1   "Nothing was written to the Bank."
#define PDNA_XRC_NOROOM_L1   "No room in the Bank."

/* APPLY / discard confirms (decision 14), both %d-bearing (siprintf worst case
 * "64 changes, 64 deletes" / "64 choices" -- tests/host_textfit_test.c). */
#define PDNA_XRC_APPLY_TITLE   "APPLY CHANGES?"
#define PDNA_XRC_DISCARD_TITLE "DISCARD CHOICES?"

/* Re-key duplicate follow-up (RE-KEY, decision 8e) -- reuses the reroll guard's own
 * wording family (PDNA_XFER_REKEY_*) but scoped to this screen's own action so a
 * caption never has to guess which flow produced it. */
#define PDNA_XRC_REKEY_DUP_TITLE "ALREADY LINKED"
#define PDNA_XRC_REKEY_DUP_L1    "A record already exists there."

/* The 12 detail phrases of decision 2's table (SELECT's three-line detail view),
 * indexed by XrcRowKind in pdna_main.c's xrc_detail_line(). Several row kinds
 * share one phrase (RESTORED-stale and PENDING/CLAIMED-lost both read "only the
 * record is left", both STALE branches read "restored; record is stale") --
 * exactly the table's own repeats, not a new shortcut. */
#define PDNA_XRC_D_PENDING_BOTH    "Did not finish; both copies"
#define PDNA_XRC_D_PENDING_ORPHAN  "Transfer never landed."
#define PDNA_XRC_D_PENDING_NOBANK  "Copy landed, record unproven."
#define PDNA_XRC_D_LOST            "Only the record is left."
#define PDNA_XRC_D_DUP_BANK        "In two places."
#define PDNA_XRC_D_DEFERRED        "Queued to leave the Bank."
#define PDNA_XRC_D_ABROAD          "In the Gen-3 PC, restorable."
#define PDNA_XRC_D_ABROAD_GB       "In a Game Boy save."
#define PDNA_XRC_D_DUP_G3          "Restored; Gen-3 copy is a dup."
#define PDNA_XRC_D_STALE           "Restored; record is stale."
#define PDNA_XRC_D_RESTORED_MOVED  "Restored copy left the Bank."
#define PDNA_XRC_D_DAYCARE         "In the Day-Care."
#define PDNA_XRC_D_STALE_KEY       "PID changed; not linked."
#define PDNA_XRC_D_AMBIGUOUS       "Ambiguous match; skipped."
#define PDNA_XRC_D_G3HOME          "Gen-3 original; see load screen."

#define PDNA_XFER_TC_TITLE        "NO GEN 1 FORM"
/* BACKLOG #150 S150-8: shortened from the brief's original "%s did not exist in
 * Gen 1." / "%s cannot be known in Gen 1." -- host_textfit_test.c found the real
 * worst-case move name in what was then xr_time_capsule_block's own move-bound
 * range (166..251, e.g. "EXTREMESPEED") overflows the 184 px budget with the
 * longer wording (200 px measured); the species side is tight but passes.
 * Shortened uniformly rather than leaving the species/move rows differently
 * worded.
 *
 * BACKLOG #212 review D6: xr_time_capsule_block's own move-bound refusal
 * (`tc == 2`) can no longer fire -- bdc_convert_gb_core (source/bank_down_convert.c)
 * now passes NULL moves into it (species-floor only) and clips the move bound
 * itself, per slot, via g3gb_moves_ok()/bad4/nbad instead (pdna_gen12.c's
 * gb_bank_down_bridge, right after the loss screen). PDNA_XFER_TC_MOVE_FMT has
 * had no production reader since that change -- tests/host_textfit_test.c's own
 * worst-case-width pin (kept, since the format string itself is unchanged and
 * still ships in the binary) is its only remaining caller. */
#define PDNA_XFER_TC_SPECIES_FMT  "%s: no Gen 1 form."
#define PDNA_XFER_TC_MOVE_FMT     "%s: not in Gen 1."   /* no production reader since #212 (D6) */

#define PDNA_XFER_FLUSHFAIL_TITLE "BANK NOT FULLY UPDATED"
#define PDNA_XFER_FLUSHFAIL_L1    "%d Pokemon are still in the"
#define PDNA_XFER_FLUSHFAIL_L2    "Bank. They are not lost."

/* BACKLOG #150 S150-4 decision 11: the UP drop's own strings. PREP = the backup gate
 * refused (nothing moved); KEPT = the Bank write landed but release_up refused (a
 * duplicate, not a loss); REC = decision 5's ledger refusal (the mon already has a
 * restorable Gen-3 original -- point at COPY/PASTE); ORIGIN = decision 4's one-time
 * per-save prompt (Q7) plus its six game-name rows. */
#define PDNA_XFER_PREP_TITLE "COULD NOT PREPARE"
#define PDNA_XFER_PREP_L1    "The Bank backup failed."
#define PDNA_XFER_PREP_L2    "Nothing was moved."

#define PDNA_XFER_KEPT_TITLE "MOVED TO THE BANK"
#define PDNA_XFER_KEPT_L1    "It is still in the Game Boy"
#define PDNA_XFER_KEPT_L2    "save too - remove it there."

/* BACKLOG #219b: the ident32 collision refusal (drop_held_up, pdna_box.c decision 10)
 * used to be a log line + snd_error() only -- the player saw nothing on screen and a
 * refused UP move just silently stayed held. l2 is deliberately NULL at the call site
 * (msg_wait's own if (l2) guard) -- one body line reads better than a padded second. */
#define PDNA_BANK_COLL_TITLE "BANK RECORD CLASH"
#define PDNA_BANK_COLL_L1    "Nothing was moved. See log.txt."

#define PDNA_XFER_REC_TITLE  "IT CAME FROM GEN 3"
#define PDNA_XFER_REC_L1     "Use COPY here, then PASTE in"
#define PDNA_XFER_REC_L2     "the Gen 3 save to bring it back."

/* BACKLOG #150 S150-12 decision 16: the read-only mount's COPY lift (the Game Boy
 * save keeps the mon; only the Bank gets a copy) and the once-per-exit "waiting for
 * the PC" offer. PCQ_L1_FMT is measured at its worst-case COMPOSED string (255
 * Pokemon, the counter's saturation ceiling), never at the bare format -- the b178
 * pattern (tests/host_textfit_test.c). COPY_NOBACK is the honest DOWN loss-screen
 * footer for a copy cell: there is no ledger entry, so there is nothing to keep. */
#define PDNA_XFER_COPIED_TITLE "COPIED TO THE BANK"
#define PDNA_XFER_COPIED_L1    "Your Game Boy save keeps it"
#define PDNA_XFER_COPIED_L2    "too. It waits for the PC."

#define PDNA_XFER_PCQ_TITLE    "WAITING FOR THE PC"
#define PDNA_XFER_PCQ_L1_FMT   "%d Pokemon. Put them in now?"

#define PDNA_XFER_COPY_NOBACK_L1 "No transfer record: this copy"
#define PDNA_XFER_COPY_NOBACK_L2 "cannot be sent back."

/* BACKLOG #150 S150-12 decision 10 (folds BACKLOG #180): the bridge DOWN loss
 * screen's own "the Bank slot stays" line was a stale claim for the bridge arm
 * (the Bank slot IS emptied once the mon lands) -- LOSS_FOOT_BRIDGE replaces it
 * with this honest wording; LOSS_FOOT_PASTE keeps the original three unchanged. */
#define PDNA_XFER_BRIDGE_STAYS "The Bank slot is emptied when it lands."

/* BACKLOG #150 S150-7: the DOWN edge -- a native Bank cell back into a Game Boy save. */

#define PDNA_XFER_DOWN_CONFIRM_TITLE "MOVE TO THE GAME?"

#define PDNA_XFER_DOWN_DUP_TITLE "SAVED - BANK COPY LEFT"
#define PDNA_XFER_DOWN_DUP_L1    "The game save HAS it now."
#define PDNA_XFER_DOWN_DUP_L2    "Delete the Bank copy yourself."

#define PDNA_XFER_DOWN_NOROOM_L1 "Every box is full."

#define PDNA_XFER_PARTYFULL_TITLE "PARTY IS FULL"
#define PDNA_XFER_PARTYFULL_L1    "Send a party Pokemon to a"
#define PDNA_XFER_PARTYFULL_L2    "box first?"

/* BACKLOG #226 review D4: app_party_full_deposit_offer()'s own mail refusal -- retail's
 * MENU_STORE -> ItemIsMail -> "PLEASE REMOVE MAIL" (pokeemerald
 * src/pokemon_storage_system.c:2646-2651), applied to the offer's picker the same way. */
#define PDNA_XFER_PARTYFULL_MAIL_TITLE "CAN'T DEPOSIT"
#define PDNA_XFER_PARTYFULL_MAIL_L1    "PLEASE REMOVE MAIL"
#define PDNA_XFER_PARTYFULL_MAIL_L2    "from that Pokemon first."

/* D-Q3: per-generation confirm-footer wording -- Gen 2's box->party landing also
 * resets HP/status to full-healthy (gb_session.c's own box->party conversion), Gen 1's
 * does not (D4: HP/status are the record's own bytes, preserved). */
#define PDNA_XFER_DOWN_PARTYFOOT_G1 "Recomputes stats."
#define PDNA_XFER_DOWN_PARTYFOOT_G2 "New stats, full HP, healthy."
#define PDNA_XFER_DOWN_CL1_SZ 128     /* gb_accept_down_hook's confirm string buffer size */
#define PDNA_XFER_DOWN_PAD_PX 180     /* ui_ptext_w padding target to force line wrap */

#define PDNA_GBEDIT_PICKPARTY_TITLE "SEND WHICH PARTY MON?"
#define PDNA_GBEDIT_PICKPARTY_FOOT  "U/D pick  A ok  B cancel"

#define PDNA_XFER_ORIGIN_TITLE "WHICH GAME IS THIS?"
#define PDNA_XFER_ORIGIN_FOOT  "U/D pick  A ok  B cancel"
#define PDNA_XFER_GAME_RED     "RED"
#define PDNA_XFER_GAME_BLUE    "BLUE"
#define PDNA_XFER_GAME_YELLOW  "YELLOW"
#define PDNA_XFER_GAME_GOLD    "GOLD"
#define PDNA_XFER_GAME_SILVER  "SILVER"
#define PDNA_XFER_GAME_CRYSTAL "CRYSTAL"

/* BACKLOG #150 S150-15, decision 8: the ORIGIN card's note line for a converted mon's
 * GB ORIGINAL view. Two-digit year -- the ORIGIN card draws this fixed sys8,
 * untruncated, in PDNA_SUM_CARD_W (138 px); worst cases "CRYSTAL 26-09-16" (16
 * glyphs = 128 px) and "CRYSTAL (no date)" (17 = 136 px) both fit, a four-digit year
 * would not (host_textfit_test.c proves both worst cases). */
#define PDNA_XFER_ORIG_NOTE_FMT   "%s %02u-%02u-%02u"
#define PDNA_XFER_ORIG_NODATE_FMT "%s (no date)"
#define PDNA_XFER_ORIG_GEN_FMT    "GEN %u"          /* origin_game unknown -- xv_origin_name() NULL */

/* decision 11: the "no original" plaque -- a row that was visible (a ledger file
 * exists, decision 4's caveat) but the walk found no NATIVE_HOME entry. PF(text, 28,
 * 184) discipline, same as every other msg_wait string in this file. */
#define PDNA_XFER_ORIG_NONE_TITLE "NO ORIGINAL DATA"
#define PDNA_XFER_ORIG_NONE_L1    "The transfer record holds no"
#define PDNA_XFER_ORIG_NONE_L2    "Game Boy original."

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
/* BACKLOG #50 (CREATE): pdna_gen12.c's gb_create_hook and its refusal panels.
 * Short by the same convention as the sidecar titles just above (msg_wait's
 * own ui_ptext_fit clips a long one safely regardless, but these are sized
 * to not need it). The species/level picker's OWN strings that used to live
 * here (PDNA_GBCREATE_SPECIES_TITLE/FOOT, PDNA_GBCREATE_LEVEL_TITLE/FOOT)
 * are gone along with the two bespoke pickers they belonged to
 * (gb_create_pick_species/gb_create_pick_level, BACKLOG #50 UX-parity,
 * Guy 2026-09-07): the create flow now opens pdna_pick.c's own pick_species()
 * (restricted, see pick_species_set_max_dex()) and computes the level via
 * rom_gblearn_min_level() instead of asking for either. */
#define PDNA_GBCREATE_TITLE          "CAN'T CREATE"
#define PDNA_GBCREATE_NOROM_L1       "Needs your Gen 1/2 ROM"
#define PDNA_GBCREATE_NOROM_L2       "(Settings > Game ROM)."
#define PDNA_GBCREATE_FULL_TITLE     "BOX FULL"
#define PDNA_GBCREATE_FULL_L1        "No empty slot here."
/* BACKLOG #187, F4: l1 is overwritten at the call site with the actual box name +
 * count ("BOX1 is full (20/20)."), siprintf'd into a stack buffer -- this L1 stays
 * only as the (now-unused after F4) generic fallback string, kept for
 * PDNA_GBCREATE_FULL_TITLE's own sizing convention comment above. l2 offers the
 * picker that now actually opens right after this message is dismissed. */
#define PDNA_GBCREATE_FULL_PICKHINT_L2 "Pick another box."
/* BACKLOG #187, F4: split out of the old BOX_FULL fold -- an unreadable list
 * (gb_list_count() returning <0, GBS_ERR_STRUCT's own condition) is a corrupt/
 * malformed box, not a full one; conflating the two hid the 2026-09-07 index-bug
 * class this backlog's own root-cause hunt re-derived from scratch (Step 1: every
 * refusal Guy could still hit on his own Yellow.sav turned out to be a genuinely
 * full box, not a misrouted index -- CREATE already resolves g_m->ui_box/
 * current_box correctly; this split is the other, always-latent half of that
 * same fold, caught while re-deriving the message rather than reproduced). */
#define PDNA_GBCREATE_BADLIST_TITLE  "CAN'T READ BOX"
#define PDNA_GBCREATE_BADLIST_L1     "This box's data looks corrupt."
/* Review fix 3 (LOW), BACKLOG #187: CREATE via F4's picker lands in a box the grid
 * is not currently showing (gb_create_hook reassigns `box = dst` before the
 * species/level/insert pipeline runs) -- without this, the ONLY feedback after a
 * successful create is the grid simply not gaining a new mon where the player is
 * still looking, easy to misread as a silent failure. Shown only when box != the
 * box the player actually opened CREATE from (the common case, staying in the same
 * box, already shows it landing right there). */
#define PDNA_GBCREATE_REDIRECTED_TITLE "CREATED"
#define PDNA_GBCREATE_REDIRECTED_FMT   "Created in %s, slot %d."
#define PDNA_GBCREATE_BUILDFAIL_L1   "Could not build a legal record."
/* pdna_pick.c's pick_item(), restricted mode (UX-parity audit, Guy 2026-09-07:
 * the GB editor's item row now opens the SAME picker the Gen-3 flow uses,
 * pick_item_set_gen1_2_max(), instead of stepping a raw byte). Gen-1/2 items
 * have no name source yet -- see that function's own header comment for why
 * "#n" and this placeholder, not a real Gen-3 name/description, are correct
 * here rather than merely a gap. */
#define PDNA_ITEM_NO_DESC_YET  "No description yet (ROM names coming)"
#define PDNA_ITEM_GB_FOOT      "A pick  SEL find  B cancel"
/* BACKLOG #195: the NAMED restricted picker (g_item_gen set -- real Gen-1/2
 * item names + a gbb_pocket_of() category filter) adds ST for that filter,
 * same length (28 cols) as the unrestricted picker's own footer above it. */
#define PDNA_ITEM_GB2_FOOT     "A pick  ST filt  SEL find  B"
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
/* BACKLOG #177: gb_paste_loss_screen's own text picker (pdna_gen12.c loss_name_text())
 * shows ONE of these two in place of the generic line above when only the OT name or
 * only the nickname is the one that lost a glyph -- row-count-neutral (same one
 * conditional row, still 10 total, the screen's 2 px of slack survives). review F2:
 * the bridge is bidirectional (gb_bank_down_bridge's dst_gen can be either Gen-1 or
 * Gen-2 -- the mounted session is whichever generation is NOT the source cell's own),
 * so naming "Gen 1" specifically was wrong on a 1->2 transfer; generation-neutral
 * wording reads correctly either way. Both measured at 4/UI_SCR_W-8 like every other
 * row here (158 px / 162 px, host_textfit_test.c). */
#define PDNA_SIDECAR_LOSS_OTNAME     "OT name: a glyph didn't transfer"
#define PDNA_SIDECAR_LOSS_NICKNAME   "Nickname: a glyph didn't transfer"
#define PDNA_SIDECAR_LOSS_KEPT_L1    "Kept in /PokeDNA/xfer;"   /* BACKLOG #150 S150-6: was /PokeDNA/sidecar */
#define PDNA_SIDECAR_LOSS_KEPT_L2    "restored when it comes back."
#define PDNA_SIDECAR_LOSS_STAYS      "The copy in your Gen-3 save stays."
#define PDNA_SIDECAR_LOSS_A_TRANSFER "A = transfer"
#define PDNA_SIDECAR_LOSS_B_CANCEL   "B = cancel"

/* BACKLOG #104 R1: the KEEP AS IS / MAKE LEGAL choice, gb_paste_legal_screen
 * (pdna_gen12.c) -- a SEPARATE, additive screen shown only when
 * gen3_to_gb_evo_needs_fix() finds a correction to offer (most transfers never see
 * it). Not folded into gb_paste_loss_screen's own rows: that screen's worst-case
 * height (all 10 loss flags + the fixed lines) already lands exactly on the last
 * pixel the display has (host_textfit_test.c's own "loss screen worst-case height"
 * check) -- there is no room left to add a row there. This screen gets its own full
 * ui_clear() budget instead, reusing the SAME primitives (ui_text/ui_ptext_fit/
 * s_wait) and the SAME "A = .../B = cancel" hint convention gb_paste_loss_screen
 * already ships -- not a new screen kind, one more instance of the same shape. The
 * third choice reuses this codebase's own established third-action key (KEY_SELECT,
 * e.g. pdna_gen12.c:798) rather than "X", which does not exist on a GBA pad. */
#define PDNA_SIDECAR_LEGAL_TITLE      "SEND TO GAME BOY"
/* D3 (UX parity with gb_paste_loss_screen, its Gen-3 twin -- that screen names every
 * loss and reassures the copy comes back unchanged; this screen owed the same two
 * things: WHY it is asking, and the same promise). Two WHY wordings, not one:
 * gen3_to_gb_evo_needs_fix() always corrects to pk_evo_floor()'s answer, and that
 * floor is sometimes the true evolution level (pk_evo_floor == pk_evo_min_level) and
 * sometimes the lower wild-caught floor for one of the 23 species evolutions.h lists
 * that are catchable below their own evolution level (Sootopolis' Super Rod Gyarados
 * at L5). "Evolves at L20" would be a false claim for a species whose real evolution
 * level is higher than the floor being offered -- gb_paste_legal_screen picks between
 * the two at runtime by comparing the two floors, never says "evolves at" unless that
 * is literally true. WHY_FMT's wording mirrors the checker's own phrasing (the
 * SUSPECT text in gen3_legality_hooks.c pk2_hook_evolution, "Evolves at L36, this one
 * is L5" -- dictionary discipline, one phrase for one fact everywhere it appears). */
#define PDNA_SIDECAR_LEGAL_WHY_FMT       "%s evolves at L%u; this one is L%u."
#define PDNA_SIDECAR_LEGAL_WHY_FLOOR_FMT "%s legal from L%u; this one is L%u."
#define PDNA_SIDECAR_LEGAL_BACK       "Either way it comes back unchanged."
#define PDNA_SIDECAR_LEGAL_KEEP_ROW   "A = KEEP AS IS"
#define PDNA_SIDECAR_LEGAL_FIX_FMT    "SELECT = MAKE LEGAL (%u -> %u)"

/* BACKLOG #150 S150-10 decision 7: gb_paste_legal_screen_ex's extra rows -- shown when
 * one or more move slots are out of range for the destination generation (G-H8). One
 * swap row per bad slot ("ROCK TOMB -> WHIRLPOOL", or "-> (no move)" when nothing
 * eligible was found); KEEP AS IS is greyed the moment any slot is bad (decision 8: A
 * is not even in the wait mask then, so no row needs "why" beyond this text itself). */
#define PDNA_XFER_SWAP_FMT           "%s -> %s"
#define PDNA_XFER_SWAP_NONE          "(no move)"
#define PDNA_SIDECAR_LEGAL_KEEP_OFF  "KEEP AS IS: not possible here"
#define PDNA_SIDECAR_LEGAL_FIX_MOVES "SELECT = MAKE LEGAL (swap moves)"
/* BACKLOG #150 S150-10 decision 10: gb_gen12_norom_msg's Gen-2 title -- Gen 1's own
 * PDNA_SIDECAR_GEN1_TITLE/_L1 are reused for both generations otherwise (L1's wording
 * -- "beside the .sav to transfer." -- never named an extension, so it needs no
 * Gen-2 twin). */
#define PDNA_SIDECAR_GEN2_TITLE      "NO GEN-2 ROM"
/* BACKLOG #212 review D9: the bridge's OWN zero-move refusal (gb_bank_down_bridge)
 * runs strictly after the base-stats gate already proved a <base>.gb/.gbc ROM sits
 * beside the save (GB1BASE_NO_ROM is refused earlier, at the base-stats lookup) --
 * "NO GEN-1 ROM" would be a false claim there. This pair says the true reason: the
 * fill found no eligible move in the destination generation's learnset at all. */
#define PDNA_SIDECAR_NOMOVES_TITLE1  "NO MOVES FOR GEN 1"
#define PDNA_SIDECAR_NOMOVES_TITLE2  "NO MOVES FOR GEN 2"
#define PDNA_SIDECAR_NOMOVES_L1      "Learnset not found."
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

/* BACKLOG #150 S150-6, decision 8/G-H5: the reroll re-key guard, shown by
 * app_xfer_pid_guard() (source/pdna_main.c) at the editor's commit chokepoint
 * (app_box_browse/party_browse) when a saved edit changed a mon's PID or OT id AND
 * a transfer-ledger record already exists under the OLD key. app_confirm's own
 * title+L1 shape is the model (source/pdna_app.h:380). */
#define PDNA_XFER_REKEY_TITLE        "SIDECAR WARNING"
#define PDNA_XFER_REKEY_L1           "Breaks the link to its GB original."
/* D-Q3: shown (msg_wait, a plain notice, not a confirm) only when the re-key runs
 * AFTER a verified commit and fails -- the save itself already landed; the OLD
 * record file is kept and logged, never lost. */
#define PDNA_XFER_REKEY_FAILED_TITLE "LINK NOT UPDATED"
#define PDNA_XFER_REKEY_FAILED_L1    "Saved. The old sidecar link was kept."
/* A file ALREADY exists at the new key -- two records sharing one PID+OTID would be
 * a DUPLICATE, not a merge (decision 8); refused before anything is written. */
#define PDNA_XFER_REKEY_DUP_TITLE    "CANNOT RE-LINK"
#define PDNA_XFER_REKEY_DUP_L1       "Sidecar already exists for that PID."

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
 * Settings > Game ROM > Sprites grid (source/pdna_main.c, slice E4)
 *
 * A 5 (kind, sprite_era.h's SeSaveKind) x 5 (place, SePlace) grid of the user's
 * chosen SeEra per cell. Reached from rom_row_menu, not its own PDNA_SET_ROWS row
 * (that list is already full at 8 -- same "no new slot" pattern E3's two GB-ROM
 * rows and item 7's ROM-art toggle used).
 *
 * Kind labels (se_kind_name) are sys8 in a narrow left gutter; era CELL text
 * (se_era_name) is PROPORTIONAL (ui_ptext), not sys8 -- "Native" is 48 px at sys8's
 * fixed 8 px/glyph, wider than one 40 px column, but only 31 px proportional (N6+a6+
 * t5+i2+v6+e6, ui_font_w), comfortably inside it (host-checked against ui_font_w
 * directly, tests/host_textfit_test.c -- not a re-typed guess). Column HEADERS are
 * this file's own short abbreviations (PDNA_SETSPR_PLACE_HDR*), not se_place_name()'s
 * full labels -- "SUMMARY" alone is 7 sys8 columns (56 px), wider than one column. */
#define PDNA_SETSPR_TITLE      "SPRITES"
#define PDNA_SETSPR_LABEL_X     2
#define PDNA_SETSPR_LABEL_W    34   /* se_kind_name()'s longest is 4 chars = 32 px sys8 */
#define PDNA_SETSPR_COL0_X     38
#define PDNA_SETSPR_COL_PITCH  40   /* 5 columns, 38..238 px -- inside the 240 px screen */
#define PDNA_SETSPR_HDR_Y      18
#define PDNA_SETSPR_ROW0_Y     28
#define PDNA_SETSPR_ROW_PITCH  16
#define PDNA_SETSPR_HELP_Y1   124
/* E5b (Guy, 2026-09-06): NATIVE means different things at different places, and this
 * is the one line of screen budget to say so -- at the box GRID (PC/BANK), it is now
 * the icon STORE's own picture for every mon (imports included, opt-in required for a
 * bitmap era cell); everywhere else (PARTY/SUMMARY/a GB save's own GBGRID) it is still
 * "the record's own era", unchanged. 29 sys8 chars is the hard budget (SCR_W - PDNA_
 * SET_HELP_X, host_textfit_test.c's "Settings > Sprites grid" section) -- too tight to
 * spell out both halves, so this line names the GRID (the place this whole screen's
 * per-mon overlay actually draws on) and leaves party/summary to the unsurprising
 * default they always had. */
#define PDNA_SETSPR_HELP1     "NATIVE: PC/BANK = game icons"
/* D8 (E4 review): a second, honest line -- the box GRID's per-mon overlay can only
 * ever swap in a Game Boy look, never a cross-game Gen-3 one, at BANK/GBGRID (PC is
 * different: it gets its whole icon SET from se_store_era() instead, unaffected).
 * se_era_next() already refuses to OFFER a Gen-3 destination at those two places
 * (sprite_era.c); this line is why. Same Y1/Y2 two-help-line layout as the Rumble
 * page's PDNA_RMB_HELP1/2 (pdna_layout.h), not colliding with PDNA_SET_FOOTER_Y
 * (152) -- see tests/host_textfit_test.c's "Settings > Sprites grid" section. */
#define PDNA_SETSPR_HELP_Y2   133
#define PDNA_SETSPR_HELP2     "BANK/GBG skip Gen-3 targets."
#define PDNA_SETSPR_FOOT      "L/R col U/D row A set B back"
/* Column legend, index-matched to sprite_era.h's SePlace order (PC/PARTY/SUMMARY/
 * BANK/GBGRID) -- deliberately NOT se_place_name(), see the note above. */
#define PDNA_SETSPR_PLACE_HDR0 "PC"
#define PDNA_SETSPR_PLACE_HDR1 "PTY"
#define PDNA_SETSPR_PLACE_HDR2 "SUM"
#define PDNA_SETSPR_PLACE_HDR3 "BANK"
#define PDNA_SETSPR_PLACE_HDR4 "GBG"
#define PDNA_SETSPR_PLACE_HDRS(X) X(PDNA_SETSPR_PLACE_HDR0) X(PDNA_SETSPR_PLACE_HDR1) \
  X(PDNA_SETSPR_PLACE_HDR2) X(PDNA_SETSPR_PLACE_HDR3) X(PDNA_SETSPR_PLACE_HDR4)
/* The dead-cell marker (se_cell_applies() == false) -- one glyph, trivially fits. */
#define PDNA_SETSPR_DEAD       "-"

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
#define PDNA_PCP_BANNER_Y1  26           /* BACKLOG #243: last row of the box banner draw_box_banner()
                                          * paints every frame (draw_banner(WP_X+2, 13, ..., w, 14) ->
                                          * rows 13..26 inclusive) -- pcp_draw_panel()'s RIGHT border
                                          * bands stop just past this row so they never clip the box-
                                          * occupancy readout that sits in that same row, past this
                                          * panel's own right edge (PDNA_PCP_PANEL_X1). */

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

/* BACKLOG #173: carry-aware tab-focus footer when a GB-source cell is held
 * (s_holding && src->is_bank): A does nothing on the PARTY tab, so the footer
 * mirrors the Gen-3 carry footer's pattern. */
#define PDNA_TAB_FOCUS_CARRY_FOOTER  "L/R tab  UP bank  DN"

/* ---------------------------------------------------------------------------
 * Description placeholder (BACKLOG #19 / docs/AUDIT-2026-09-05-backlog-3-19.md §B,
 * desc_gate.h / data_desc_shim.c): what pk_item_desc/pk_move_desc/pk_ability_desc
 * return in an artless build (PDNA_DESC_TEXT_COMPILED=0) when NO ROM is registered
 * this session, so pdna_main.c's desc_or_fallback() degrades to something honest
 * instead of an empty string. Every caller already renders through ui_ptext_wrap,
 * ui_ptext_fit or text_wrap (pdna_bag.c's draw_desc, pdna_pick.c's item/ability
 * panels, pdna_summary.c's ability card) -- three of those four either wrap or
 * paginate, so an over-long placeholder there just adds a line.
 *
 * The FOURTH -- pdna_pick.c:1478-1481, the non-split item-picker's one-line-above-
 * the-footer view -- is the real tightest budget (review finding, 2026-09-05,
 * corrects this file's earlier claim that the ball row was tightest): it draws
 * "label  desc" through ONE ui_ptext_fit(4, 139, UI_SCR_W-8=232, ...) SHARED between
 * pk_item_label()'s output and the description, with 2 literal space glyphs between
 * them. Measured against the real item table, the widest label is a TM ("No35
 * FLAMETHROWER", 99 px), leaving only 232 - 99 - 2*3 = 127 px for the description
 * half of the line -- PDNA_PICK_DESC_LINE_BUDGET_PX below. Real descriptions already
 * exceed that routinely and clip with a visible '~' (accepted, pre-existing
 * behaviour this gate must not make WORSE); the placeholder must actually FIT it,
 * since a fixed string clipping is a new, avoidable regression, not an existing one.
 * tests/host_textfit_test.c measures it against this budget, not the ball row. */
#define PDNA_PICK_DESC_LINE_BUDGET_PX 127
#define PDNA_DESC_PLACEHOLDER "(needs ROM)"

/* pdna_gbtrainer.c's gbtr_id_edit_ok() -- the Gen-1/2 mirror of pdna_trainer.c's
 * id_edit_ok/s_id_warned, gating GBTR_NAME/GBTR_ID (P1b review D6). Same app_confirm
 * (28, .., 184) proportional clamp as the gbedit popups above. */
#define PDNA_GBTRAINER_ID_WARN_TITLE "Changes your TRAINER identity"
#define PDNA_GBTRAINER_ID_WARN_L1    "Your own Pokemon become 'traded'."

/* D1 fix (U2c 2nd re-verify): the GB-screen shell's 1:1-mode legend used to
 * join a key name and an action word into ONE string per side-bar row ("A
 * EDIT", "START MORE") -- past the 36-px side bar for anything longer than
 * "B BACK", it silently truncated ("SEL S~", "START~"). The fix splits the
 * two into separate columns (pdna_gbscreen.c's LEFT/RIGHT side bars); these
 * macros are the ONE copy of every key name and action word that appears in
 * either column, shared by pdna_gbscreen.c (kGbscrLegendKeys/
 * kGbscrBaseActions), pdna_gbtrainer.c (kLegendEdit/kLegendView), and
 * tests/host_textfit_test.c's width check -- so the check binds to the exact
 * strings painted on screen, never a re-typed copy that could drift.
 * Budgets: GBSCR_LEGEND_MAXW (left bar) and GBSCR_RIGHT_BAR_MAXW (right bar),
 * both 36 px today (pdna_gbscreen.c). */
#define PDNA_GBSCR_KEY_A     "A"
#define PDNA_GBSCR_KEY_B     "B"
#define PDNA_GBSCR_KEY_SEL   "SEL"
#define PDNA_GBSCR_KEY_START "START"

#define PDNA_GBSCR_ACT_OK    "OK"
#define PDNA_GBSCR_ACT_BACK  "BACK"
#define PDNA_GBSCR_ACT_SIZE  "SIZE"

/* M1 (BACKLOG #91) Gen-1 Map screen: L/R are the shell's own SIZE toggle
 * (same as SELECT) on this screen -- the D-pad alone pans -- so there is no
 * legend slot for it (gbscr_set_legend's fixed A/B/SEL/START rows, m1 review
 * D6). This one-line hint is drawn INTO the map's own body (gbscr_text,
 * pdna_gbmap.c's gbmap_paint) instead, at GBSCR_ROWS-1 -- must fit
 * GBSCR_COLS (20) columns at 1:1 AND stretched (the same cell-coordinate
 * text, just a different pixel scale -- see tests/host_textfit_test.c). */
#define PDNA_GBMAP_HINT      "D-PAD PAN"

/* M1-G2 (BACKLOG #91) Gen-2 Map screen: the one genuinely new silent-
 * corruption risk in this slice (design doc §10 risk 1) -- a Gold save
 * with a registered CRYSTAL ROM (or vice-versa) would otherwise locate
 * that ROM's tables successfully and draw a PLAUSIBLE BUT WRONG map. The
 * screen reads the cartridge header title at 0x134 and refuses when it
 * disagrees with gb_session_is_crystal(). */
#define PDNA_GBMAP2_WRONG_GAME "That ROM is not this save's game."

#define PDNA_GBTR_ACT_EDIT   "EDIT"
#define PDNA_GBTR_ACT_SAVE   "SAVE"
#define PDNA_GBTR_ACT_MORE   "MORE"

/* BACKLOG #202 F1: the HoF card's own START legend word -- the trainer card's
 * own PDNA_GBTR_ACT_MORE means "the rest of THIS card's fields, plain page";
 * the HoF card's START instead opens the CLEAR/SET-COUNT/ADD/DELETE menu, a
 * different action that deserves its own word rather than borrowing MORE. */
#define PDNA_GBHOF_ACT_MENU  "MENU"

/* D2 fix (U2c 2nd re-verify): the GB-screen shell's open-refusal reasons
 * (pdna_gbscreen.c's kReasonNoRom/kReasonNoStack/kReasonOpen/kReasonBadGen/
 * kReasonNoTail) used to be concatenated onto the plain trainer page's title
 * ("GB ART: OFF -- <reason>") and painted as ONE fixed-font (8 px/glyph)
 * line, which ran the forced-fallback test string off the 240-px screen
 * ("forced (PDNA_U2C_FORCE_FALLBACK)" alone is 264 px in that font). The
 * title now stays the short, always-fits PDNA_GBTR_FALLBACK_TITLE; every
 * reason macro below is its OWN second line in the plain proportional face
 * (ui_ptext), and must fit GBTR_HEADER2_MAXW there -- checked by
 * tests/host_textfit_test.c against the real macros, not a re-typed copy. */
#define PDNA_GBTR_FALLBACK_TITLE  "GB ART: OFF"
#define GBTR_HEADER2_MAXW 232   /* screen width 240, x=4, 4px right margin */

#define PDNA_GBSCR_REASON_NO_ROM      "no ROM registered"
#define PDNA_GBSCR_REASON_NO_STACK    "not enough stack"
#define PDNA_GBSCR_REASON_OPEN        "ROM art unavailable"
#define PDNA_GBSCR_REASON_OPEN_DETAIL "(bad ROM or non-English)"
#define PDNA_GBSCR_REASON_BAD_GEN     "not a Gen-1/Gen-2 request"
#define PDNA_GBSCR_REASON_NO_TAIL     "no tile-bank memory"
/* BACKLOG #98 D2 (review-sonnet ab81b56): the delta-gb fused-ROM lookup
 * (source/fused_gb.c's fused_gb_lookup_failed_reason()) can now fail for two
 * reasons more specific than a plain "no ROM registered" -- an active save that
 * genuinely has no fused ROM of its own (ORPHANED) vs. one where the fused
 * directory itself is ambiguous (two ROMs of the same generation, ambiguous, no
 * active-save pairing to break the tie). pdna_gbscreen.c's gbscr_open_inner()
 * (delta build only) maps FusedGbFailReason to one of these instead of the
 * generic kReasonNoRom, so the fallback page says WHY, not just THAT. Measured
 * against GBTR_HEADER2_MAXW in tests/host_textfit_test.c, same as every other
 * reason string here. */
#define PDNA_GBSCR_REASON_AMBIGUOUS_ROM "two ROMs of this game fused; pick a save"
#define PDNA_GBSCR_REASON_ORPHANED_ROM  "this save's ROM is not fused"
#define PDNA_GBSCR_REASON_UNAVAILABLE "unavailable"
/* BACKLOG #148: gbscr_open_inner()'s whole-tail scan now runs through the SAME
 * cancel/timeout/read-error guard (gb_scan_guard.h) gb_art_source.c's registration
 * scan already has, via gb_art_io.h's GbArtIo/gb_art_read. A stopped guard maps to
 * one of these instead of the generic kReasonOpen -- same width budget
 * (GBTR_HEADER2_MAXW), same kReason* convention as every reason above. */
#define PDNA_GBSCR_REASON_CANCELLED   "Cancelled"
#define PDNA_GBSCR_REASON_TIMED_OUT   "Timed out"
#define PDNA_GBSCR_REASON_READ_ERR    "SD read error"
/* Only reachable under -DPDNA_U2C_FORCE_FALLBACK (a build-time test flag,
 * pdna_gbtrainer.c) -- included here anyway so the same width test covers
 * the one shot harness the review actually re-shoots. */
#define PDNA_GBSCR_REASON_FORCED_TEST "forced (test)"

/* pdna_gbclock.c -- BACKLOG #86/#108's Gen-2 "Clock fix" screen, over source/
 * gb_clock.h's honest core (see that header for why this is NOT an absolute-time
 * sync/set the way pdna_clock()'s Gen-3 screen is). Every app_confirm title/l1 here
 * goes through the SAME (28, 184) proportional clamp as every other GB-screen confirm
 * (PDNA_GBTRAINER_ID_WARN_* above) -- checked in tests/host_textfit_test.c. The three
 * row labels are drawn with ui_text (tonc sys8, 8 px/glyph) at x=6 on the 240 px
 * screen, same budget pdna_mirage()'s own party rows use (234 px = 29 cols; all three
 * measure under that, see the .c file's own row list). */
#define PDNA_GBCLOCK_TITLE "GEN 2 CLOCK"

#define PDNA_GBCLOCK_ROW_RESET "Ask for the time at next load"
#define PDNA_GBCLOCK_ROW_SHIFT "Shift the clock"
#define PDNA_GBCLOCK_ROW_CLEAR "Clear the clock-error flag"
#define PDNA_GBCLOCK_SHIFT_KEYS "U/D change  L/R fld  A set  B"   /* 29 ch = 232 px @ x=4 (the 31-ch original clipped at 252) */

#define PDNA_GBCLOCK_CONFIRM_RESET_TITLE "Ask for the time at next load?"
#define PDNA_GBCLOCK_CONFIRM_RESET_L1 \
  "Asks for the time on the next CONTINUE, like a dead battery would."

#define PDNA_GBCLOCK_CONFIRM_CLEAR_TITLE "Clear the clock-error flag?"
#define PDNA_GBCLOCK_CONFIRM_CLEAR_L1 \
  "Dismisses the banner; a dead battery raises it again next boot."

#define PDNA_GBCLOCK_CONFIRM_SHIFT_TITLE "Shift the clock by this much?"

/* Gen 1 / a GS-without-the-field fallback (nav_avail already keeps this row out of
 * reach in both cases, but the screen stays honest if it is ever reached anyway --
 * same posture as pdna_clock()'s own FRLG fallback, pdna_main.c). */
#define PDNA_GBCLOCK_NOCLOCK_L1 "This save has no clock to fix."

/* ---- boot: flashcart detection rows + the header-only dialog (pdna_main.c) ---------
 * detect_line() paints one sys8 row per detection attempt at PDNA_DETECT_X, from
 * PDNA_DETECT_Y0, UI_ROW_H apart (8 rows end exactly at UI_FOOTER_Y). The parts below
 * are what it prints; host_textfit_test.c assembles the worst reachable row from THEM
 * (attempt 8, the longest verdict, NOR page 0x1ff, 255 look-alikes first at 0x1ff) and
 * measures it, so a longer verdict string fails at build time instead of wrapping onto
 * the next attempt's row on the cart. A page form is only ever printed with an EZ-Flash
 * "found" verdict (EZ_OK / EZ_HDRONLY); every other verdict uses the plain form. The
 * worst reachable row is "8: EZ hdr! NOR#1ff la=255@1ff" = 29 glyphs (the test found
 * "PSRAM(SD)" pushed the header-only fallback's row to 31: hence the bare "PSRAM"). */
#define PDNA_DETECT_X          6
#define PDNA_DETECT_Y0         86
#define PDNA_DET_ED_OK         "ED ok"
#define PDNA_DET_ED_SDFAIL     "ED sd fail"
#define PDNA_DET_EZ_OK         "EZ ok"
#define PDNA_DET_EZ_HDRONLY    "EZ hdr!"
#define PDNA_DET_NOCART        "no cart"
#define PDNA_DET_EZ_NOPAGE     "EZ no page"
#define PDNA_DET_UNKNOWN       "?"
#define PDNA_DET_FMT_PSRAM     "%d: %s PSRAM"        /* SD-loaded; "(SD)" cost 4 glyphs */
#define PDNA_DET_FMT_NOR       "%d: %s NOR#%x"
#define PDNA_DET_FMT_PLAIN     "%d: %s"
#define PDNA_DET_FMT_LA        " la=%u@%x"
/* msg_wait (28/184 clamp) shown after detection when the page was matched by the header
 * word only -- the fingerprint could not be trusted on this bus (hw1 review #2). */
#define PDNA_DET_HDRONLY_TITLE "IMAGE NOT VERIFIED"
#define PDNA_DET_HDRONLY_L1    "Cart page matched by header only."
#define PDNA_DET_HDRONLY_L2    "Re-copy this build if it misbehaves."

/* ---- BACKLOG #54 T1: the ROM-hack banner ------------------------------------
 * Shown by app_register_rom()'s verdict-specific refusal and once per save open
 * in view_save() (right after app_icon_rom_open() runs this session's rom_open()/
 * rom_identify() classification for g_game's slot) whenever that slot is flagged
 * ROM_ID_HACK (source/rom_map.h). msg_wait's usual (28, 184) clamp, PF-checked in
 * tests/host_textfit_test.c beside the rest of that file's msg_wait strings.
 * Fixed strings, no siprintf (BACKLOG #34). */
#define PDNA_ROMHACK_TITLE "ROM HACK"
#define PDNA_ROMHACK_L1    "Read-only until verified."
#define PDNA_ROMHACK_L2    "Edits could corrupt this save."
/* app_src_readonly_set()'s why-note for the mon-menu refusal -- drawn at
 * PDNA_MONMENU_PROSE_W (a narrower budget than msg_wait's 184), hence the
 * shorter wording. PF-checked in tests/host_textfit_test.c beside the other
 * "why locked" prose the read-only mon menu draws. */
#define PDNA_ROMHACK_NOTE  "ROM hack: locked"
/* Review fix F1: the flat "READ-ONLY / Needs EZ-Flash Omega." refusal at ~12
 * app_can_edit()-gated sites in pdna_main.c is a real lie to an Omega owner whose
 * cart is writable but whose ROM is hack-flagged -- app_readonly_why()
 * (pdna_main.c) picks between this and the Omega message based on
 * app_rom_is_hack(g_game). PF-checked at the same (28, 184) msg_wait clamp. */
#define PDNA_ROMHACK_WHY "ROM hack: writes locked."
/* Review fix F2: the honest reason for the GB-only sites when the session is a
 * streamed/view-only one (pdna_gen12_resident() == false) -- neither the cart nor
 * a hack ROM caused the refusal. */
#define PDNA_GB_VIEWONLY_WHY  "View-only session."
#define PDNA_GB_VIEWONLY_FOOT "view-only  B back"
/* BACKLOG #166 review F1: AppSrcOps.lift_why's grey line draws at
 * PDNA_MONMENU_PROSE_W (88 px, the read-only mon menu's own prose budget --
 * pdna_main.c's app_mon_menu_readonly, RO_PROSE_W in tests/host_textfit_test.c),
 * NOT msg_wait's 184 px clamp gb_move_hook's own late refusal dialog uses (that
 * text stays unchanged: this is a SECOND, narrower home for the same idea, shown
 * before the row is even offered, not a replacement for the dialog after). Every
 * string here is a short, coarse bucket (gb_lift_why_bs maps the GbsStatus that
 * refused the lift onto one of these, source/pdna_gen12.c), not the verbatim
 * gbs_status_text() wording, which does not fit: "the party needs one Pokemon"
 * alone is 176 px, over TWICE this budget. PF-checked (max width in px) in
 * tests/host_textfit_test.c beside the read-only menu's other "why" prose. */
#define PDNA_GB_LIFT_WHY_FLOOR "Can't lift: last mon"   /* 87px: GBS_ERR_PARTY_FLOOR */
#define PDNA_GB_LIFT_WHY_MAIL  "Party holds Mail"       /* 77px: GBS_ERR_MAIL */
#define PDNA_GB_LIFT_WHY_BOX   "Box not writable"       /* 80px: GBS_ERR_UNWRITABLE/BOX/STRUCT */
#define PDNA_GB_LIFT_WHY_OTHER "Lift refused"           /* 62px: every other refusal status */
#define PDNA_GB_LIFT_WHY_VIEW  "View-only"              /* 46px: no open edit session (!g_ed) */
#define PDNA_GB_LIFT_WHY_OMEGA "Needs Omega"             /* 63px: !app_can_edit(), not a hack ROM */
/* Review fix F3: rule 1d (rom_identify()) also catches genuine non-US retail carts
 * (AXVD/BPEJ/BPES/... -- a real (code, version) pair simply absent from k_versions,
 * which only pins the 11 US builds) as ROM_ID_HACK with kind == ROM_NONE. Calling a
 * real retail cartridge from another region a "ROM HACK" is a false accusation;
 * app_register_rom() (pdna_main.c) now shows this honest "can't say what this is"
 * banner instead whenever kind == ROM_NONE, reserving PDNA_ROMHACK_* for a pinned
 * US (code, version) whose title/size genuinely diverges (kind != ROM_NONE). */
#define PDNA_ROMOTHER_TITLE "UNSUPPORTED ROM"
#define PDNA_ROMOTHER_L1    "Not a retail US R/S/E/FR/LG."
#define PDNA_ROMOTHER_L2    "A hack, or another region."

#endif /* PDNA_LAYOUT_H */
