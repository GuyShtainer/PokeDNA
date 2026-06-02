#ifndef PKVIEW_SUMMARY_H
#define PKVIEW_SUMMARY_H

#include "gen3_mon.h"

/* Paged, game-faithful Pokémon summary for party[idx]:
 *   L/R (or LEFT/RIGHT) = change card, UP/DOWN = previous/next mon, B = back.
 * Six cards: INFO, SKILLS, IVs, EVs, BATTLE MOVES, CONTEST MOVES.
 * Loops until B; returns the index last viewed. */
int pkview_summary(const PkMon* party, int count, int idx);

#endif /* PKVIEW_SUMMARY_H */
