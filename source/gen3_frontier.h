#ifndef GEN3_FRONTIER_H
#define GEN3_FRONTIER_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/*
 * Emerald Battle Frontier win streaks (and the Ruby/Sapphire Battle Tower's much
 * smaller equivalent) — the single source of truth for the streak offset tables.
 *
 * Everything here lives PLAINTEXT in SaveBlock2: no XOR against the 0xAC security
 * key, no per-block checksum. (Emerald key-XORs exactly five things and none of
 * them are in the frontier block — see docs/analysis-2026-07-29/feature1-*.md §1.4.)
 * So a caller edits the shared g_sb2 buffer in place and commits with app_commit_sb2().
 *
 * Layout verified against reference/pokeemerald/global.h (struct BattleFrontier at
 * SaveBlock2 + 0x64C) recompiled under the retail ARM APCS rule
 * -mstructure-size-boundary=32, which is why BattleDomeTrainer is 4 bytes; a table
 * derived by hand-summing "natural" struct sizes lands 0x20 low from domeMonIds on.
 * The hex offset comments in that header are already SaveBlock2-absolute.
 *
 * Pure C (only <stdint.h>/<stdbool.h>/<string.h> + PkGame) so tests/host_frontier_test.c
 * dual-compiles it on the PC.
 */

/* Facility ids — same order as VAR_FRONTIER_FACILITY and gen3_record.c's tables. */
#define G3F_TOWER    0
#define G3F_DOME     1
#define G3F_PALACE   2
#define G3F_ARENA    3
#define G3F_FACTORY  4
#define G3F_PIKE     5
#define G3F_PYRAMID  6
#define G3F_FACILITIES 7

/* Battle modes. Arena / Pike / Pyramid are singles-only (1 lane per level mode). */
#define G3F_SINGLES     0
#define G3F_DOUBLES     1
#define G3F_MULTIS      2
#define G3F_LINK_MULTIS 3

/* Level modes (the INNER, fastest-varying array index — see g3f_lane_off). */
#define G3F_LVL_50   0
#define G3F_LVL_OPEN 1

/* Which array a lane value comes from. */
#define G3F_CURRENT 0   /* the streak a challenge is building right now */
#define G3F_RECORD  1   /* the best-ever streak shown on the records board */

#define G3F_MAX_STREAK    9999   /* MAX_STREAK, constants/battle_frontier.h:49 */
#define G3F_MAX_PYRAMID    999   /* Pyramid's own updater caps at 999          */
#define G3F_MAX_CHAMPS     999   /* domeTotalChampionships cap                 */

/* Number of battle modes facility f actually has (4,2,2,1,2,1,1). 0 if f invalid. */
int g3f_modes(int facility);

const char* g3f_facility_name(int facility);   /* "Battle Tower" .. NULL if invalid */
const char* g3f_mode_name(int facility, int mode);   /* "Singles"/"Doubles"/... */

/* ---- streak lanes ---------------------------------------------------------
 * A lane is (facility, mode, lvl). Both the CURRENT and RECORD arrays are u16 and
 * are indexed [battleMode][lvlMode] with lvlMode INNER, i.e. byte offset
 * base + (mode*2 + lvl)*2. Getting that order backwards silently swaps Lv50 with
 * Open on every multi-mode facility. */
int  g3f_lane_off(int facility, int mode, int lvl, int kind);   /* -1 if invalid */

/* Raw value as stored. Returns -1 on bad args (NOT on an implausible value — a
 * viewer must be able to show a corrupt number rather than hide it). */
int  g3f_streak_get(const uint8_t* sb2, int facility, int mode, int lvl, int kind);

/* Clamped write of one lane. Clamps to [0, g3f_streak_cap(facility)]. Returns the
 * value actually written, or -1 on bad args. Does NOT touch the active bitmask —
 * callers should prefer g3f_set_current(), which keeps them in lockstep. */
int  g3f_streak_set(uint8_t* sb2, int facility, int mode, int lvl, int kind, int value);

int  g3f_streak_cap(int facility);   /* 999 for Pyramid, else 9999 */

/* ---- winStreakActiveFlags (SB2 0xCDC, u32) — THE lockstep field -------------
 * Every facility's challenge-init does
 *     if (!(winStreakActiveFlags & bit_for_this_lane)) currentStreak = 0;
 * so a CURRENT streak written without its bit is silently wiped the moment the
 * player next walks into that facility. The bit assignment is NOT sequential by
 * facility: bits 0-13 cover the singles lanes plus Arena/Pike/Pyramid, bits 14-25
 * cover the doubles/multis lanes. Never derive it — use these. */
#define G3F_ACTIVE_OFF 0xCDC
int  g3f_active_bit(int facility, int mode, int lvl);        /* 0..25, -1 invalid */
bool g3f_active_get(const uint8_t* sb2, int facility, int mode, int lvl);
void g3f_active_set(uint8_t* sb2, int facility, int mode, int lvl, bool on);

/* Set a CURRENT streak the safe way: clamp, write, then set the lane's active bit
 * when v > 0 (clear it when v == 0), and optionally raise RECORD to match when the
 * record is lower. This is the function a UI should call. Returns the value
 * written, or -1 on bad args. */
int  g3f_set_current(uint8_t* sb2, int facility, int mode, int lvl, int value, bool raise_record);

/* ---- challenge state -------------------------------------------------------
 * Editing while a challenge is in progress collides with the resume flow, which
 * also owns selectedPartyMons (0xCAA) and the party stashed by SaveSelectedParty —
 * the classic way people lose Pokemon. Callers must refuse or hard-warn.
 * Do NOT "fix" a busy state by zeroing challengeStatus. */
#define G3F_CHALLENGE_STATUS_OFF 0xCA8   /* u8: 0 none, 1 saving, 2 paused, 3 won, 4 lost */
#define G3F_BITFIELD_OFF         0xCA9   /* u8: bits0-1 lvlMode, bit2 challengePaused    */
#define G3F_CUR_BATTLE_NUM_OFF   0xCB2   /* u16 battle/room/floor # inside the challenge */
bool g3f_challenge_active(const uint8_t* sb2);
int  g3f_challenge_status(const uint8_t* sb2);
int  g3f_lvl_mode(const uint8_t* sb2);          /* frontier.lvlMode, 0..3 as stored */
int  g3f_cur_battle_num(const uint8_t* sb2);

/* ---- supporting counters ---------------------------------------------------
 * Offsets that are NOT streaks but sit on the same records boards. */
#define G3F_DOME_CHAMPS_OFF   0xD1C   /* u16[2][2] lifetime tournaments won  */
#define G3F_FACTORY_RENTS_OFF 0xDF2   /* u16[2][2] — pret's comment says 0xDF6 and is WRONG */
#define G3F_FACTORY_REC_RENTS_OFF 0xDFA
#define G3F_PIKE_TOTALS_OFF   0xE0C   /* u16[2] lifetime times cleared       */
#define G3F_BATTLE_POINTS_OFF 0xEB8   /* u16 spendable BP                    */
#define G3F_CARD_BP_OFF       0xEBA   /* u16 BP shown on the trainer card    */
int  g3f_u16_get(const uint8_t* sb2, int off);
void g3f_u16_set(uint8_t* sb2, int off, int value, int cap);

/* SaveBlock2 is a PARTIAL section (0xF2C of 3968 bytes). PokeDNA's whole-section
 * checksum only works because the tail is zero, so nothing may be written past
 * 0xF2B. Every writer here is bounds-checked against this. */
#define G3F_SB2_LIMIT 0xF2C

/* ---- Frontier Brain / symbol thresholds ------------------------------------
 * GetFrontierBrainStatus compares (streak + modifier) to the threshold with `==`,
 * NEVER `>=`, and returns NOT_READY unless the mode is SINGLES. So overshooting a
 * threshold PERMANENTLY skips that symbol for the lane, and a "set 9999" button is
 * actively hostile to a player who wants the symbols.
 *
 * CRUCIALLY, the game picks WHICH threshold to test by how many symbols the player
 * already owns, not by which one you'd like:
 *     symbolsCount = GetPlayerSymbolCountForFacility(facility);   // 0..2
 *     if (winStreak == sFrontierBrainStreakAppearances[facility][symbolsCount])
 * So asking for Gold while holding no symbols writes a number the game never tests
 * — the Brain silently never appears, and you've jumped clean over the Silver
 * threshold. Callers must pass the ACTUAL symbol count (from the SaveBlock1 flags
 * via pk_frontier_flag) and let this pick the tier.
 * Table: sFrontierBrainStreakAppearances, src/frontier_util.c:86-95. */
int  g3f_brain_target(int facility, int symbols_owned);   /* stored value, -1 invalid */
/* Which tier that target summons: 0 = Silver, 1 = Gold. -1 if invalid. */
int  g3f_brain_tier(int facility, int symbols_owned);

/* ---- Ruby / Sapphire Battle Tower (struct BattleTowerData @ SaveBlock2 0xA8) --
 * Completely different and much smaller. The CURRENT streak is DERIVED, not stored:
 *   ((curStreakChallengesNum[lvl] - 1) * 7 - 1) + curChallengeBattleNum
 * capped at 9999 (pokeruby battle_tower.c:1398-1407), so there is no "current
 * streak" field an editor can meaningfully set. `currentWinStreaks` @0x574 is a
 * stale cache the game recomputes.
 *
 * IMPORTANT: bestBattleTowerWinStreak @0x572 is RECOMPUTED by the game as
 * max(recordWinStreaks[0], recordWinStreaks[1]) inside SaveCurrentWinStreak — so
 * writing 0x572 alone is undone by the next tower battle. Write the record pair. */
#define G3F_RS_RECORD_STREAKS_OFF 0x560   /* u16[2] recordWinStreaks (Lv50, Open) */
#define G3F_RS_TOTAL_WINS_OFF     0x570   /* u16 totalBattleTowerWins             */
#define G3F_RS_BEST_STREAK_OFF    0x572   /* u16 derived cache — see above        */
int  g3f_rs_record(const uint8_t* sb2, int lvl);          /* -1 on bad lvl */
/* Writes recordWinStreaks[lvl] AND refreshes the derived 0x572 cache so the value
 * survives the game's own recomputation. Returns the value written, -1 on bad args. */
int  g3f_rs_set_record(uint8_t* sb2, int lvl, int value);

/* True iff `game` has an editable frontier/tower streak block at all.
 * FireRed/LeafGreen have NOTHING: their SB2 0xB0 block is dead "leftover from R/S"
 * data and the Trainer Tower stores TIMES, not streaks. */
bool g3f_supported(PkGame game);

#endif /* GEN3_FRONTIER_H */
