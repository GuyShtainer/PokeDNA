#ifndef GEN3_RECORD_H
#define GEN3_RECORD_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Emerald "Battle Record" (Frontier Pass) — save flash sector 31, byte offset
 * 0x1F000 of a 128 KiB .sav. Single copy, never slot-rotated: the game overwrites
 * it each time a new battle is recorded. Pure C (no tonc) so tests/host_record_test.c
 * dual-compiles this on the PC. Clean-room from the pret decomp (reference only);
 * layout notes in docs/analysis-2026-07-17/record-spec.md.
 *
 * On-disk framing (all little-endian):
 *   0x1F000  u32   sentinel 0x0000B39D (special-sector marker — NOT the normal
 *                  rotating-sector footer; sector 31 has no id/signature/counter)
 *   0x1F004  3968  RecordedBattleSave: playerParty[6] + opponentParty[6] (the
 *                  standard 100-byte encrypted party mons), 4 players' names /
 *                  genders / TIDs / languages, rngSeed, battleFlags, opponent ids,
 *                  facility + level mode, per-battler 664-byte input lanes, and a
 *                  trailing u32 checksum = byte-sum of the struct's first 3964 B.
 * Emerald-only: RS predate the Frontier; FRLG use sector 31 for Trainer Tower.
 */

#define G3_REC_SECTOR_OFF   0x1F000u
#define G3_REC_STRUCT_OFF   (G3_REC_SECTOR_OFF + 4u)
#define G3_REC_STRUCT_SIZE  3968u
#define G3_REC_MON_SIZE     100u     /* struct Pokemon: 80-byte box core + battle stats */
#define G3_REC_LANE_SIZE    664u     /* recorded input stream, one lane per battler     */

typedef struct {
  bool     present;         /* sentinel 0xB39D found (a record was ever saved)     */
  bool     checksum_ok;     /* trailing u32 byte-sum matches                       */
  bool     flags_ok;        /* battleFlags nonzero and no impossible bits          */
  uint32_t rng_seed;        /* RNG state at battle start (replay determinism)      */
  uint32_t battle_flags;
  uint16_t opponent_a, opponent_b, partner_id;
  uint16_t multiplayer_id;  /* which players[] index is the recording player       */
  uint8_t  lvl_mode;        /* 0 = Level 50, 1 = Open Level                        */
  uint8_t  facility;        /* 0..6 Tower/Dome/Palace/Arena/Factory/Pike/Pyramid   */
  char     names[4][8];     /* decoded trainer names (local battles fill [0] only) */
  uint8_t  genders[4];      /* 0 male / 1 female                                   */
  int      lane_len[4];     /* input bytes per battler (prefix before first 0xFF)  */
} G3RecordInfo;

/* Scan sector 31 of `save` (size = actual loaded byte count). Returns true iff a
 * record is PRESENT (sentinel found); fills *out either way. A 64 KiB dump (no
 * sector 31) or an erased sector returns false. Trust the teams/fields only when
 * present && checksum_ok. */
bool g3_record_scan(const uint8_t* save, uint32_t size, G3RecordInfo* out);

/* Pointer to the 6-slot party inside the record (side 0 = player, 1 = opponent).
 * Each mon is G3_REC_MON_SIZE bytes; its first 80 bytes are the standard encrypted
 * box-mon record (pk_decode_mon applies unchanged); byte 84 is the plaintext level. */
const uint8_t* g3_record_party(const uint8_t* save, int side);

const char* g3_record_facility_name(int facility);   /* "Battle Tower" .. "?" */
const char* g3_record_facility_short(int facility);  /* "Tower" .. "Pyramid" / "?" */

/* ---- Current frontier win streak (SaveBlock2, Emerald only) -----------------
 * The CURRENT win streak of the challenge a record belongs to lives ONLY in
 * SaveBlock2's BattleFrontier block — sector 31 never stores it — so it must be
 * read at export time or it is lost. `sb2` = the 3968-byte data region of save
 * section 0 (the game's SaveBlock2 image; Emerald layout — the caller gates on
 * the game version). Selects the lane by the record's facility byte + lvlMode
 * (+ battle mode recovered from battleFlags), mirroring the game's
 * GetCurrentFacilityWinStreak (pokeemerald src/frontier_util.c:1804-1830).
 * Returns the streak as stored (0..G3_REC_MAX_STREAK), or -1 when it cannot be
 * trusted: sb2/ri NULL, non-frontier facility byte, lvlMode > 1, a battle mode
 * the facility has no streak lane for, or a value above MAX_STREAK. */
#define G3_REC_MAX_STREAK 9999   /* MAX_STREAK, pokeemerald include/constants/battle_frontier.h:49 */
int g3_record_win_streak(const uint8_t* sb2, const G3RecordInfo* ri);

/* One streak lane: facility 0..6, mode 0..3 (singles/doubles/multis/link-multis
 * where the facility has them), lvl 0..1 (Lv50/Open). -1 = no such lane or
 * implausible value. Same table g3_record_win_streak indexes. */
int g3_facility_streak(const uint8_t* sb2, int facility, int mode, int lvl);

/* Build the human-readable export sidecar text (player + record summary + both
 * teams + every facility's CURRENT/BEST streak, one line per level mode) into
 * out (cap bytes, NUL-terminated). Everything platform-specific comes in as
 * arguments; pure C so the host tests exercise it. `stamp` = RTC string or NULL.
 * Returns the length written (always < cap; the builder truncates safely).
 * The streak block is Emerald-only in truth, not just in the header: it needs
 * `game == PK_EMERALD` AND a non-NULL `sb2` (Emerald's SaveBlock2 Frontier
 * layout), else it prints "not available" rather than misreading a save that
 * has no such struct — RS's Battle Tower current streak is DERIVED, not
 * stored (gen3_frontier.h), and RS never reaches this export anyway (RS
 * predates the Frontier: no sector 31 recording exists to export). */
/* `sb1`/`game` may be NULL/anything when unavailable — the SAVE STATE block is then
 * omitted rather than guessed. See docs/REC-SIDECAR.md in the rec2mp4 project for the
 * consumer's contract. */
int g3_record_sidecar(char* out, int cap, const G3RecordInfo* ri,
                      const uint8_t* save, const uint8_t* sb2, const uint8_t* sb1,
                      int game, uint16_t tid_public, const char* stamp);

#endif /* GEN3_RECORD_H */
