#ifndef PKVIEW_TRAINER_H
#define PKVIEW_TRAINER_H

#include <stdint.h>
#include "gen3_save.h"
#include "gen3_trainer.h"

/* Trainer card / stats screen: name, ID, money, play time, Pokédex, the
 * designated Elite-Four / Hall-of-Fame first-clear time, and game records.
 * B (or START) returns. */
void pkview_trainer(const uint8_t* sb1, const uint8_t* sb2, const Gen3SaveInfo* info, PkGame game);

#endif /* PKVIEW_TRAINER_H */
