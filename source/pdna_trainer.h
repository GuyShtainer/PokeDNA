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

/* Trainer card / stats screen: name, ID, money, play time, Pokédex, the
 * designated Elite-Four / Hall-of-Fame first-clear time, and game records. On an
 * EZ-Flash Omega the top fields (name/gender/TID/SID/money/play-time) are EDITABLE
 * (U/D to pick a field, A to change, B saves on exit). sb1/sb2 are edited in place
 * and committed via the shared verified-write path. B returns. */
void pdna_trainer(uint8_t* sb1, uint8_t* sb2, const Gen3SaveInfo* info, PkGame game);

#endif /* PDNA_TRAINER_H */
