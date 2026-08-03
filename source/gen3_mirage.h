#ifndef GEN3_MIRAGE_H
#define GEN3_MIRAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/*
 * MIRAGE ISLAND (Route 130, RSE only).
 *
 * ---- the mechanic ------------------------------------------------------------
 * There is NO "island is present" flag anywhere in the save. Presence is recomputed live every
 * time you step onto Route 130 and every time you talk to the watcher in Pacifidlog, by
 * IsMirageIslandPresent():
 *
 *     u16 rnd = VAR_MIRAGE_RND_H;
 *     for (i = 0; i < 6; i++)
 *         if (party[i].species && (party[i].personality & 0xFFFF) == rnd) return TRUE;
 *
 * So the island appears iff ANY of the six party slots — eggs included, they have a real
 * species and a real personality — has a personality whose low half equals one saved u16.
 *
 * ---- why this is not simply "write the u16" ----------------------------------
 * VAR_MIRAGE_RND is rolled forward once per in-game day by an LCG:
 *
 *     rnd = 1103515245 * rnd + 12345          (mod 2^32, VAR_MIRAGE_RND_H is the TOP half)
 *
 * and the catch-up for every day owed runs in DoTimeBasedEvents(), which is called BEFORE the
 * map's own transition script on every path INCLUDING continuing a saved game. A save that has
 * not been played for N days therefore rewrites this value the instant it is loaded — before
 * Route 130 or the watcher ever asks. Writing the target directly is silently undone, and the
 * failure is indistinguishable from a wrong offset. (MEASURED: Guy's Emerald owes 5 days, his
 * Ruby 68.)
 *
 * The fix is to write the LCG PRE-IMAGE: the value which, after the game applies the N steps it
 * already owes, BECOMES the target. The game's own catch-up does the work, every daily event
 * still processes normally, and nothing else in the save is touched.
 *
 * ---- blast radius ------------------------------------------------------------
 * VAR_MIRAGE_RND_H/L have exactly three consumers in the whole game: Route 130's layout swap
 * (the island itself), the Pacifidlog watcher's dialog, and the choice of ambient cry on
 * Route 130. Nothing else reads them. No Pokemon is touched, no trainer identity is touched.
 *
 * ---- how long it lasts -------------------------------------------------------
 * Until the next in-game day rolls over. MEASURED by brute force over all 65,536 low halves:
 * at most TWO consecutive days are reachable for any real party, and three is impossible — the
 * LCG has no fixed point, because 1103515245 - 1 is even while 12345 is odd.
 *
 * Pure C (no tonc, no GBA headers) so tests/host_* runs it on the PC.
 */

#define MIRAGE_PARTY_SLOTS 6

/* Is Mirage Island visible right now, i.e. does the CURRENT saved value already match a party
 * Pokemon? Note this ignores days owed — it answers "what would the watcher say if the game
 * were already caught up". */
bool mirage_present(const uint8_t* sb1, PkGame game);

/* The saved value's two halves, for display. Returns false on a game with no Mirage Island. */
bool mirage_get(const uint8_t* sb1, PkGame game, uint16_t* hi, uint16_t* lo);

/* The personality low halves of the party, i.e. every value that WOULD make the island appear.
 * Writes up to MIRAGE_PARTY_SLOTS entries and returns how many; slot i of the output
 * corresponds to `out_slot[i]` in the party. Empty slots are skipped. */
int mirage_party_keys(const uint8_t* sb1, uint16_t* out_key, uint8_t* out_slot);

/* Days the save owes: how many times the game will roll the LCG the moment it is loaded.
 * `cur_day` is TODAY in the game's own 1-based day count (gen3_rtc_days(...) + 1, adjusted by
 * the save's localTimeOffset — see gen3_clock.*). Returns 0 when the clock was never set, when
 * nothing is owed, or on a game with no Mirage Island. Never negative: a VAR_DAYS in the future
 * means the game will not roll at all. */
int mirage_days_owed(const uint8_t* sb1, PkGame game, int cur_day);

/* Solve for the value to WRITE so that, after `steps` of the game's own catch-up, the top half
 * equals `target_hi`. `steps` may be 0 (write the target directly). Always succeeds: the LCG is
 * a bijection on 32 bits, so every target has exactly one pre-image per step count. */
void mirage_solve(uint16_t target_hi, uint16_t target_lo, int steps,
                  uint16_t* out_hi, uint16_t* out_lo);

/* Apply the game's forward step `steps` times — the inverse of mirage_solve, for verification.
 * Host tests use it to prove solve() round-trips; the UI uses it to show what tomorrow brings. */
void mirage_advance(uint16_t hi, uint16_t lo, int steps, uint16_t* out_hi, uint16_t* out_lo);

/* Write the two u16s into SaveBlock1. Caller commits (app_commit_sb1). No-op and false on a
 * game with no Mirage Island. */
bool mirage_set(uint8_t* sb1, PkGame game, uint16_t hi, uint16_t lo);

#endif /* GEN3_MIRAGE_H */
