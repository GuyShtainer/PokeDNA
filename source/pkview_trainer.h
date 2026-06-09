#ifndef PKVIEW_TRAINER_H
#define PKVIEW_TRAINER_H

#include <stdint.h>
#include "gen3_save.h"
#include "gen3_trainer.h"

/* Trainer card / stats screen: name, ID, money, play time, Pokédex, the
 * designated Elite-Four / Hall-of-Fame first-clear time, and game records. On an
 * EZ-Flash Omega the top fields (name/gender/TID/SID/money/play-time) are EDITABLE
 * (U/D to pick a field, A to change, B saves on exit). sb1/sb2 are edited in place
 * and committed via the shared verified-write path. B returns. */
void pkview_trainer(uint8_t* sb1, uint8_t* sb2, const Gen3SaveInfo* info, PkGame game);

#endif /* PKVIEW_TRAINER_H */
