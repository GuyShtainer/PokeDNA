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

#endif /* PDNA_TRAINER_H */
