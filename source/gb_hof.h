#ifndef GB_HOF_H
#define GB_HOF_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish,
                          * gbs_write_outside_sum                                    */

/* gb_hof — pure-C Hall-of-Fame core (BACKLOG #89, docs/kb/pokemon/).
 *
 * THERE IS NO GEN-3 TWIN: PokeDNA never reads the Gen-3 HoF sectors at all (the
 * "Records" nav row on Gen 3 is the Frontier battle record, pdna_main.c). This is a
 * new shape borrowed for Gen 1/2 only.
 *
 * FACTS, all re-derived from the pinned .sym files AND (Gen 1) the decomp's own
 * save/league_pc/hall_of_fame source, not taken on the brief's word alone:
 *
 *   Gen 1 (Red/Blue/Yellow identical layout): sHallOfFame is 50 slots x HOF_TEAM (96 B
 *   = PARTY_LENGTH(6) x HOF_MON(16 B): species, level, nickname 11 B, 3 pad),
 *   file 0x0598. wNumHoFTeams (file 0x284E) is the 1-based LIFETIME total, saturating
 *   at 255 (AnimateHallOfFame: "inc a / jr z, .skipInc / inc [hl]" — an increment that
 *   would wrap to 0 is skipped). SaveHallOfFameTeams (engine/menus/save.asm) stores
 *   teams OLDEST-FIRST by slot: while count <= 50, the new team lands at slot
 *   (count-1) — so the newest team is always the HIGHEST occupied slot; once count > 50
 *   every team shifts down one slot (discarding slot 0, the oldest) and the new team
 *   always lands at slot 49. So "newest" is slot min(count,50)-1 on Gen 1, confirmed
 *   against the decomp, not assumed. bills_pc.asm/main_menu.asm gate the PC's
 *   "HALL OF FAME" option purely on wNumHoFTeams != 0 -- NEVER on a byte scan of the
 *   blob -- so gbh_clear() only strictly needs to zero the count to hide the option;
 *   this core also zero-fills the blob for defense-in-depth and Gen-2 parity.
 *
 *   THE GEN-1 "EMPTY SLOT" MARKER: AnimateHallOfFame writes species=$FF as the
 *   end-of-party marker for the CURRENTLY-RECORDED team only (LeaguePCShowTeam's own
 *   read loop stops at species==$FF); nothing in the decomp ever fills sHallOfFame
 *   with $FF at new-game time — the region is simply never touched until the first
 *   win, so a genuinely fresh cartridge's "empty" bytes are whatever the SRAM chip
 *   powered up holding (commonly $00, not guaranteed $FF). gbh_team_count_present()
 *   therefore treats EITHER $FF or $00 as a terminator/empty marker, and gbh_clear()
 *   zero-fills (not $FF-fills) -- the count reaching 0 is what actually hides the PC
 *   option, exactly like retail.
 *
 *   Gen 2: sHallOfFame is 30 slots x 98 B (win-count byte + 6 x HOF_MON(16 B: species,
 *   OT id 2 BE, DVs 2, level, nickname 10 B no terminator) + 1 trailing byte), file
 *   0x321A (GS) / 0x32C0 (Crystal). AddHallOfFameEntry inserts the newest team at
 *   index 0 and shifts the rest down (eggs skipped) -- so Gen 2 is ALREADY
 *   newest-first in storage order, unlike Gen 1. wHallOfFameCount (file 0x24EB GS /
 *   0x24EC Crystal) is capped at 200, sits INSIDE the checksummed+mirrored span. The
 *   backup-checksum run starts at sHallOfFameEnd = file 0x3D96 (GS) -- a clear must
 *   stop at 0x3D95 inclusive; the blob's own 2940 B width (30*98) already lands
 *   exactly there, so a straight team-by-team zero-fill never overruns it.
 */

#define GBH_NUM_MONS    6    /* PARTY_LENGTH, both gens                                */
#define GBH_G1_CAPACITY 50   /* HOF_TEAM_CAPACITY, Gen 1                               */
#define GBH_G2_CAPACITY 30   /* Gen 2's own fixed team-slot count                      */
#define GBH_NICK_CAP    16   /* decoded UTF-8 cap for a nickname (raw is 10 or 11 B)   */

typedef struct {
  bool     present;   /* this mon slot is occupied (species terminator not yet hit)   */
  uint16_t dex;        /* national dex number; 0 if the raw species byte does not map  */
  uint8_t  level;
  char     nick[GBH_NICK_CAP];
  uint16_t otid;       /* Gen 2 only; 0 on Gen 1                                       */
  uint8_t  dv[4];       /* Gen 2 only, order Atk/Def/Spd/Spc, matching G2Mon; all-0 Gen1*/
  bool     shiny;       /* Gen 2 only, derived via g2_dv_shiny(); always false on Gen 1 */
} GbHofMon;

typedef struct {
  int      n;                    /* mons present, 0..GBH_NUM_MONS                     */
  GbHofMon mon[GBH_NUM_MONS];
  uint8_t  win_count;             /* Gen 2 only; 0 on Gen 1 (no per-team counter)       */
} GbHofTeam;

/* The lifetime win counter (both gens; GBF_HOF_COUNT). 0 on a closed/malformed
 * session. Gen 1 saturates at 255; Gen 2 is capped at 200 by gbh_set_count() but a
 * READ simply returns whatever the save holds (a foreign tool could in principle have
 * written something else there — this never lies about what is on the card). */
int gbh_count(const GbSession* s);

/* How many of the capacity's team slots hold real data, via an INDEPENDENT SCAN of
 * the blob itself (not derived from gbh_count) -- the same "two independent
 * derivations, cross-checked" posture the rest of this family uses. Storage order is
 * scanned from slot 0 upward and stops at the first ABSENT slot (a team's first mon's
 * species byte is 0 or, Gen 1 only, 0xFF) -- both generations write contiguously, so
 * this is exact, not a heuristic, UNLESS something outside PokeDNA left a hole, which
 * this will then under-report (a defensive miss, never an over-report). Capped at
 * GBH_G1_CAPACITY / GBH_G2_CAPACITY. 0 on a closed/malformed session. */
int gbh_team_count_present(const GbSession* s);

/* Decode team `i` (0 = NEWEST, matching the screen's list order on both gens even
 * though their on-card storage order differs -- see the big comment above) into
 * `out`. `i` must be < gbh_team_count_present(s). False on a bad index or a
 * malformed/closed session; `out` is zeroed first either way. */
bool gbh_team(const GbSession* s, int i, GbHofTeam* out);

/* Erase every recorded team AND the lifetime counter (the "CLEAR ALL" screen action).
 * Gen 1: zero-fills the whole blob team-by-team through gbs_write_outside_sum (the
 * allowlist -- the blob sits below GEN1_SUM_FIRST, gbs_write_field cannot reach it),
 * then zeroes the count through the normal gbs_write_field path (inside the checksum
 * window). Gen 2: zero-fills team-by-team through gbs_write_field (already capped
 * only by s->len, no allowlist needed), then zeroes the count, then ONE gbs_finish()
 * closes the whole batch (mirrors + both checksums), per gbs_write_field's own
 * "call gbs_finish once after the LAST write in a batch" contract.
 *
 * ATOMICITY ACROSS TEAMS is NOT this function's job: on ANY non-GBS_OK return, some
 * teams may already be zeroed in `s->img` while others are not. The caller MUST
 * treat that exactly like every other gb_session edit (gb_session.h:152's own
 * convention) — restore the WHOLE image from its own pristine copy via gb_rollback().
 * This is deliberate, not an oversight: gen1_write_outside_sum's own contract
 * (gen1_write.h) explains why a NEW multi-chunk rollback buffer was not built for
 * this when the caller already keeps a strictly stronger one (a full pristine image). */
GbsStatus gbh_clear(GbSession* s);

/* Set the lifetime counter directly (the "SET COUNT" screen action). Clamped to the
 * field's real range: Gen 1 <= 255 (its natural u8 ceiling — AnimateHallOfFame's own
 * saturate-at-255 rule), Gen 2 <= 200 (the game's own cap). A no-op (n already equals
 * the stored value) writes nothing, same convention as every gbs_write_field caller.
 * `n` < 0 is clamped to 0. GBS_ERR_ARG on a closed/malformed session. */
GbsStatus gbh_set_count(GbSession* s, int n);

#endif /* GB_HOF_H */
