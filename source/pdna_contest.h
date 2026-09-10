#ifndef PDNA_CONTEST_H
#define PDNA_CONTEST_H

#include <stdint.h>
#include "gen3_trainer.h"   /* PkGame */

/* CONTESTS screen (BACKLOG #60): the 5 Lilycove Art Museum paintings (pick the
 * Pokemon shown, from the party or a PC box) + the Contest Hall's recent winners
 * (view only — Guy's ask was "choose the Pokemon on the painting", not rewrite
 * history). RSE only; FRLG has no Contests (refuses with its own message, same
 * pattern as pdna_battle_record/pdna_mirage/pdna_clock).
 *
 * `sb1` is g_sb1 (edited in place, committed via app_commit_sb1()); `pc` is g_pc
 * (read-only here — the picker browses it, never writes it). B returns. */
void pdna_contest(uint8_t* sb1, uint8_t* pc, PkGame game);

#endif /* PDNA_CONTEST_H */
