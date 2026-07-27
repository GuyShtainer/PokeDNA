#ifndef GEN3_STARS_H
#define GEN3_STARS_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/* Trainer-card star achievements (pure C, host-testable). The card's tier
 * (green/bronze/copper/silver/gold) IS the star count 0..4; each star is one
 * concrete achievement in the save, so the editor toggles the underlying data
 * honestly instead of poking a "stars" byte (there is none — the game recounts).
 *
 * Emerald (CountPlayerTrainerStars, decomp src/trainer_card.c):
 *   0 Hall of Fame entered      gameStats[10] != 0 (+ stats[1] debut time)
 *   1 Hoenn dex complete        caught flags for Hoenn dex 1..200 (- Jirachi/Deoxys)
 *   2 All 5 museum paintings    SB1 contestWinners[8..12].species != 0 (@0x2F90+i*0x20)
 *   3 All 14 frontier symbols   flags 0x8C4..0x8D1
 * Ruby/Sapphire (GetRubyTrainerStars; offsets vs reference/pokeruby/global.h):
 *   0 Hall of Fame entered      gameStats[1]/[10] (SB1 0x1540, plaintext)
 *   1 Hoenn dex complete        same species set as Emerald
 *   2 Battle Tower 50-streak    SB2 u16 @ 0x0572 (bestBattleTowerWinStreak) > 49
 *   3 All 5 museum paintings    SB1 museumPortraits[i].species != 0 (@0x2EFC+i*0x20)
 * FRLG (TrainerCard_GenerateCardForLinkPlayer, vendored pokefirered
 * src/trainer_card.c:858-888; offsets vs its include/global.h SaveBlock2):
 *   0 Hall of Fame entered      gameStats[10] + [1] debut time (SB1 0x1200, key-XOR)
 *   1 Kanto dex complete        caught national 1..150 (HasAllKantoMons, - Mew)
 *   2 National dex complete     + 152..248, 252..384 (HasAllMons, - Mew/Lugia/
 *                               Ho-Oh/Celebi/Jirachi/Deoxys)
 *   3 Minigame records          berryPick.berriesPicked (SB2 u16 @ 0xB14) >= 200
 *                               AND pokeJump.jumpsInRow (SB2 u16 @ 0xB00) >= 200
 *
 * `hoenn200` is the Hoenn-dex->National table from card_hoenn_dex() (generated
 * art data); pass NULL in an art-free build — the RS/Emerald dex achievement
 * then reads as not-done and cannot be toggled, and pk_star_count skips that
 * star. FRLG's dex achievements are plain national ranges (no table needed). */

#define PK_STAR_ACH_MAX 4

int  pk_star_ach_count(PkGame g);                    /* 4 for RS/E/FRLG */
const char* pk_star_ach_name(PkGame g, int i);

/* Is achievement i a dex-completion star? (turning it OFF un-catches species
 * — the caller should confirm with the user first). */
bool pk_star_ach_is_dex(PkGame g, int i);

bool pk_star_ach_done(const uint8_t* sb1, const uint8_t* sb2, PkGame g, int i,
                      const uint16_t* hoenn200);
int  pk_star_count(const uint8_t* sb1, const uint8_t* sb2, PkGame g,
                   const uint16_t* hoenn200);        /* 0..4 */

/* Can achievement i be toggled at all? (false: FRLG rows, dex without table) */
bool pk_star_ach_can_set(PkGame g, int i, const uint16_t* hoenn200);

/* Toggle achievement i on/off in the RAM blocks. Returns a dirty mask of the
 * blocks touched (1 = SB1, 2 = SB2; 0 = unsupported/no-op) — the caller maps
 * it onto its d1/d2 flags and commits through the usual verified-write path.
 * Note: turning the dex star OFF clears the caught flag of all 200 Hoenn
 * species (seen flags are kept) — confirm with the user before calling. */
int  pk_star_ach_set(uint8_t* sb1, uint8_t* sb2, PkGame g, int i, bool on,
                     const uint16_t* hoenn200);

#endif /* GEN3_STARS_H */
