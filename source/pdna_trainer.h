#ifndef PDNA_TRAINER_H
#define PDNA_TRAINER_H

#include <stdint.h>
#include "gen3_save.h"
#include "gen3_trainer.h"
#include "rom_chrome.h"     /* RomChrome -- the card's ROM rung, see rom_chrome.h */

/* Register the open ROM's chrome session (or NULL to clear it) -- called once by
 * app_icon_rom_open() beside pdna_origin_art_set_romsprite(), same pattern. Safe
 * to call even in a full-art build (a no-op there: the registered RomChrome is
 * simply never consulted once the strong card_bg() wins the link). */
void pdna_trainer_set_romchrome(const RomChrome* rch);

/* ---- shared card painters (BACKLOG #49 P1b) --------------------------------
 * Exported so a second data model (the Gen-1/2 trainer card, pdna_gbtrainer.c)
 * can draw rows that LOOK AND FLOW like this screen's own plain (art-free)
 * page without duplicating its ui_panel/ui_fill_rect layout. Gen-3's own call
 * sites (tcard_row_paint / flag_row_paint) now call these too -- behaviour is
 * unchanged, only the ui_panel/ui_fill_rect/ui_text lines moved here. */

/* Numeric entry via the on-screen keyboard; returns `cur` on cancel. No clamp
 * message on an out-of-range entry (Gen 3 never had one; P1b keeps it that way). */
uint32_t num_entry(const char* prompt, uint32_t cur, uint32_t maxv);

/* One "label   value" row, red/blue selection panel when `sel`, plain background
 * otherwise -- the exact look of the plain trainer-card page's rows (2,y-1,236,9). */
void trainer_row_paint(int y, bool sel, const char* label, const char* val,
                       uint16_t val_ink);

/* One "label   ON/off" row, same panel/selection look as trainer_row_paint but the
 * badge/flag toggle screen's own column widths (8,y-1,236,9) and ON/off text. */
void trainer_flag_row_paint(const char* label, bool on, int y, bool sel);

/* The bottom key-legend line every plain trainer/flag page prints (row 152, UI_DIM). */
void trainer_key_legend(const char* text);

/* Trainer card / stats screen: name, ID, money, play time, Pokédex, the
 * designated Elite-Four / Hall-of-Fame first-clear time, and game records. On an
 * EZ-Flash Omega the top fields (name/gender/TID/SID/money/play-time) are EDITABLE
 * (U/D to pick a field, A to change, B saves on exit). sb1/sb2 are edited in place
 * and committed via the shared verified-write path. B returns. */
void pdna_trainer(uint8_t* sb1, uint8_t* sb2, const Gen3SaveInfo* info, PkGame game);

/* ---- shared card-FRONT painter (BACKLOG #49 P1c) ---------------------------
 * A game-agnostic field model so pdna_gbtrainer.c's Gen-1/2 card can drive the
 * SAME per-game card_bg()/CARD_LAYOUTS art this file's own card_editor() uses,
 * instead of duplicating the field-position math. Gen 1/2 have no card art of
 * their own (card_bg.h's whole layout table is RS/Emerald/FRLG-shaped), so
 * the GB card always paints on the EMERALD layout/art (card_bg(PK_EMERALD,
 * ...)) -- see pdna_gbtrainer.c for why Emerald specifically (6 back rows
 * fits every GB back-row list; badge_x/24*i fits 8 front badge cells). */
typedef struct {
  const char* name;         /* already-decoded display name (ASCII/UTF-8)    */
  uint16_t    id;            /* IDNo. (5 digits)                             */
  uint32_t    money;
  bool        money_unknown; /* true: draw "?" instead of money (Gen-1 BCD
                              * decode failure); Gen-3 always leaves this
                              * false (CardFields cf = {0} at its one call
                              * site, card_field() above) so this is a
                              * Gen-1/2-only state, P1d */
  uint16_t    play_h;
  uint8_t     play_m;
  uint16_t    badges;         /* bit i = badge i owned; the front row draws
                               * bits 0..7 only (8 slots baked into the art)  */
  bool        has_dex;
  uint16_t    dex_caught;     /* dex counts: number caught/owned              */
  bool        photo;          /* true: the CALLER already drew a real photo
                               * overlay (Gen-3's card_draw_photo) over the
                               * SEX rect and this painter leaves it alone;
                               * false: this painter fills that rect with a
                               * neutral placeholder box itself (Gen-1/2: no
                               * GB trainer-sprite locator yet, BACKLOG note) */
} CardFields;

/* Paint ID/NAME/MONEY/TIME/BADGES(0..7)/DEX(if has_dex)/photo-placeholder(if
 * !photo) at CARD_LAYOUTS[game]'s own coordinates. SEX (the real photo) and
 * STARS (Gen-3's card tier) are NOT part of `cf` -- they have no Gen-1/2
 * analogue -- so a Gen-3 caller still draws those two itself. */
void card_front_fields_paint(PkGame game, const CardFields* cf);

/* One field of the above (CARDF_ID/NAME/MONEY/TIME/BADGES only -- SEX/STARS
 * are a no-op here, draw them yourself). Exported so a cursor-move restore
 * can repaint just the one field that changed. */
void card_field_one(PkGame game, int f, const CardFields* cf);

/* A CARDF_* field's cursor rect (card_bg.h order; CARDF_BADGES + bsel 0..7
 * for a single badge cell). */
void card_field_rect(PkGame game, int f, int bsel, int* x, int* y, int* w, int* h);

/* 2px red selection frame on a field's rect (over the card art). */
void card_field_sel_frame(PkGame game, int f, int bsel);

/* Restore ONE field's rect from the card bg (tier/female pick which frame)
 * and repaint it via card_field_one -- the shared cursor-move idiom both
 * card_editor() and pdna_gbtrainer.c's front card use. */
void card_field_restore(PkGame game, int f, int bsel, int tier, int female,
                        const CardFields* cf);

/* ---- shared card-BACK painter (BACKLOG #49 P1c) ----------------------------
 * Same idea as the front painter above, for the card BACK's row list (HoF/
 * link-battle-shaped rows on Gen 3; COINS/MOM/RIVAL/MOTHER/GENDER for GB) --
 * only the GEOMETRY (CARD_BACK_LAYOUTS[game]) and the plain "label / value"
 * text draw are shared; each caller supplies its own label+value strings
 * (Gen-3's CBK_* stat lookups stay in pdna_trainer.c, unexported). */
void card_back_name_paint(PkGame game, const char* name);
void card_back_rect(PkGame game, int row, int* x, int* y, int* w, int* h);
void card_back_sel_frame(PkGame game, int row);
void card_back_row_paint(PkGame game, int row, const char* label, const char* value);
void card_back_row_restore(int game, int row, int tier, int female,
                           const char* label, const char* value);

#endif /* PDNA_TRAINER_H */
