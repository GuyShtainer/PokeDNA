#ifndef GEN3_SAVE_H
#define GEN3_SAVE_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Gen 3 (Ruby / Sapphire / Emerald) save geometry.
 * Verified against reference/pokeemerald/save.{c,h} and global.h (and pokeruby).
 *
 * The sector framing (size, footer offsets, signature, checksum) is IDENTICAL
 * across Ruby, Sapphire and Emerald. Only the *contents* of SaveBlock1/2 differ.
 * ==========================================================================*/

#define G3_SECTOR_SIZE       0x1000   /* 4096: 3968 data + 116 unused + 12 footer */
#define G3_SECTOR_DATA_SIZE  3968     /* 0xF80 usable data bytes                  */
#define G3_SECTORS_PER_SLOT  14
#define G3_NUM_SLOTS         2
#define G3_SAVE_FILE_SIZE    0x20000  /* 128 KiB full .sav image                  */
#define G3_SLOT_BYTES        (G3_SECTORS_PER_SLOT * G3_SECTOR_SIZE) /* 0xE000     */

/* Footer offsets inside each 4096-byte sector (struct SaveSector). */
#define G3_OFF_ID            0xFF4    /* u16 logical section id                   */
#define G3_OFF_CHECKSUM      0xFF6    /* u16                                      */
#define G3_OFF_SIGNATURE     0xFF8    /* u32, must equal G3_SIGNATURE             */
#define G3_OFF_COUNTER       0xFFC    /* u32 save counter (per slot)              */
#define G3_SIGNATURE         0x08012025u

/* Logical section ids (save.h). */
#define G3_SID_SAVEBLOCK2        0
#define G3_SID_SAVEBLOCK1_START  1
#define G3_SID_SAVEBLOCK1_END    4
#define G3_SID_PKMN_STORAGE_START 5
#define G3_SID_PKMN_STORAGE_END  13

/* Reassembled SaveBlock1 size = 4 full data chunks. */
#define G3_SAVEBLOCK1_BYTES  (4 * G3_SECTOR_DATA_SIZE)  /* 15872 */

/* SaveBlock2 (trainer info) field offsets — identical across RSE. */
#define SB2_OFF_PLAYER_NAME  0x00     /* 7 chars + 0xFF terminator               */
#define SB2_OFF_GENDER       0x08     /* 0 = male, 1 = female                     */
#define SB2_OFF_TRAINER_ID   0x0A     /* TID lo, TID hi, SID lo, SID hi           */
#define SB2_OFF_PLAYTIME_H   0x0E     /* u16                                      */
#define SB2_OFF_PLAYTIME_M   0x10     /* u8                                       */
#define SB2_OFF_PLAYTIME_S   0x11     /* u8                                       */
#define SB2_OFF_ENCRYPT_KEY  0xAC     /* u32, EMERALD ONLY (RS has no key here)   */

/* SecretBase array offset *inside reassembled SaveBlock1*:
 *   Emerald   -> 0x1A9C   (reference/pokeemerald/global.h, struct SecretBase)
 *   Ruby/Sapp -> 0x1A08   (reference/pokeruby/global.h,    struct SecretBaseRecord)
 * Both: 20 records x 160 bytes, both spanning SaveBlock1 sections 2 and 3
 * (which are full 3968-byte sections). */
#define SB1_OFF_SECRET_BASES_EMERALD 0x1A9C
#define SB1_OFF_SECRET_BASES_RS      0x1A08
#define G3_SECRET_BASES_COUNT 20
#define G3_SECRET_BASE_SIZE   160     /* bytes per record (both versions)         */

#define G3_PLAYER_NAME_LEN    7
#define G3_MAX_SPECIES        411     /* Gen 3 internal species ceiling           */

/* Player's LIVE party inside SaveBlock1 (identical offsets for RS and Emerald).
 * The party sits in SaveBlock1 section 1 (the first reassembled chunk). */
#define SB1_OFF_PARTY_COUNT   0x234   /* u8: number of party mons (0..6)          */
#define SB1_OFF_PARTY         0x238   /* PARTY_SIZE x struct Pokemon (100 bytes)  */
#define G3_MON_SIZE           100     /* sizeof(struct Pokemon)                   */
#define G3_PARTY_SIZE         6

typedef enum {
  G3_VER_UNKNOWN = 0,
  G3_VER_RS      = 1,   /* Ruby AND Sapphire (identical save format) */
  G3_VER_EMERALD = 2
} Gen3Version;

typedef struct {
  bool     valid;
  int      slot;            /* 0 or 1: the current (newer) save slot      */
  uint32_t counter[2];      /* save counters of both slots                */
  bool     slot_valid[2];   /* whether each slot had a valid signature    */
  int      sections_found;  /* distinct valid section ids in current slot */
  bool     sb1_ok;          /* SaveBlock1 (ids 1..4) all present          */

  /* Decoded trainer info from SaveBlock2. */
  char     trainer_name[G3_PLAYER_NAME_LEN + 1];
  uint8_t  gender;
  uint16_t tid_public;
  uint16_t tid_secret;
  uint16_t play_h;
  uint8_t  play_m;
  uint8_t  play_s;

  /* Version detection (a guess; the UI lets the user confirm before writing). */
  Gen3Version version_guess;
  int      bases_em;        /* sane non-empty bases read at the Emerald offset */
  int      bases_rs;        /* sane non-empty bases read at the RS offset      */
} Gen3SaveInfo;

/* One decrypted, kept party member, distilled to what SecretBaseParty stores. */
typedef struct {
  uint16_t species;
  uint16_t heldItem;
  uint16_t moves[4];
  uint8_t  level;
  uint8_t  avgEV;       /* average of the 6 EVs (matches GetAverageEVs)        */
  uint32_t personality;
} Gen3PartyMon;

typedef struct {
  int          count;          /* number of KEPT mons (0..6)                    */
  Gen3PartyMon mon[G3_PARTY_SIZE];
} Gen3LiveParty;

/* One party member as the UI shows it: decoded nickname + a couple of plaintext
 * stats, plus where it sat in the raw party so a selection maps back to a slot. */
typedef struct {
  char     nickname[11];   /* 10 chars + NUL, via gen3_decode_char             */
  uint16_t species;
  uint8_t  level;
  uint16_t maxHP;
  uint16_t attack;
  uint16_t speed;
  uint8_t  original_slot;  /* 0..5 raw party index (eggs/empties skipped)      */
} Gen3DisplayMon;

typedef struct {
  int            count;
  Gen3DisplayMon mon[G3_PARTY_SIZE];
} Gen3DisplayParty;

/* SaveBlock2 RTC bookkeeping. Offsets are identical for Ruby/Sapphire and Emerald
 * (verified byte-exact vs the pret decomps in reference/), and the fields are stored
 * PLAINTEXT — they are NOT obfuscated by Emerald's security key, so write plain
 * little-endian integers, no XOR. struct Time = { s16 days @+0x00; s8 hours @+0x02;
 * s8 minutes @+0x03; s8 seconds @+0x04 } but occupies an 8-byte slot in SaveBlock2
 * (bytes +0x05..+0x07 are padding — leave them untouched). FRLG has these fields but
 * no time-based events, so the fix is RSE-only. */
#define SB2_OFF_LOCAL_TIME_OFFSET 0x98   /* localTimeOffset:   days s16 @+0, h/m/s s8 @+2/+3/+4 */
#define SB2_OFF_LAST_BERRY_UPDATE 0xA0   /* lastBerryTreeUpdate: same layout                    */

/* ---- read-side ---------------------------------------------------------- */

bool     gen3_parse(const uint8_t* save, uint32_t size, Gen3SaveInfo* out);



/* Display read for the UI: same keep-rules as gen3_read_live_party (no omit),
 * additionally decoding each kept mon's nickname + plaintext stats + the raw
 * party slot it came from. Read-only. Returns the number of kept mons. */
int      gen3_read_live_party_display(const uint8_t* sb1, Gen3DisplayParty* out);


/* Days since 2000-01-01 for a Gregorian date (matches the RTC epoch). Pure. */
int      gen3_rtc_days(int year, int month, int day);

/* Best-effort "a new in-game day has passed since this save" using SaveBlock2's
 * stored time bookkeeping and the current RTC day count (from gen3_rtc_days).
 * `sb2` points at the start of SaveBlock2 data. Heuristic — see notes. */
bool     gen3_day_passed(const uint8_t* sb2, int rtc_now_days);

/* ---- RTC clock check & fix (RSE only; pure C, host-testable) -------------
 * The game never stores an absolute clock — it derives in-game local time as
 * (liveCartRTC - localTimeOffset) and tracks event progress in lastBerryTreeUpdate.
 * A flashcart whose RTC was reset makes the derived time run in the PAST, tripping
 * the game's "time went backwards" guards so berries / Shoal tides / Lottery /
 * Mirage Island freeze. We fix it by rewriting those two SaveBlock2 fields. */

/* Inverse of gen3_rtc_days: a days-since-2000-01-01 count -> calendar date. */
void gen3_days_to_date(int days, int* year, int* month, int* day);

/* A read-only diagnosis of the save's clock vs the live cart RTC day count. */
typedef struct {
  int has_rtc;            /* 1 if rtc_now_days came from a real reading           */
  int off_days, off_h, off_m, off_s;   /* localTimeOffset as stored (signed)      */
  int berry_days;         /* lastBerryTreeUpdate.days as stored (game convention)  */
  int local_days;         /* derived in-game day (epoch-0) = rtc_now - off_days    */
  int ly, lm, ld;         /* derived in-game calendar date                        */
  int delta;              /* elapsed days since last berry update (<0 == frozen)   */
  int verdict;            /* 0 ok, 1 large-jump, 2 backwards/frozen, 3 corrupt, 4 no-RTC */
} Gen3ClockInfo;
void gen3_clock_read(const uint8_t* sb2, int rtc_now_days, int rtc_now_sec, int has_rtc, Gen3ClockInfo* out);

/* Fix writers — edit g_sb2 in RAM (plaintext, little-endian); the caller commits via
 * the verified SaveBlock2 write. Both re-anchor lastBerryTreeUpdate so events resume
 * with no negative/giant elapsed jump.
 *   autosync: make the in-game clock equal the live cart RTC right now (offset -> 0).
 *   manual:   make the game believe `desired` is "now" (offset = liveRTC - desired). */
/* Both return false (and write nothing) if the resulting day counts would not fit
 * the game's s16 Time.days — e.g. a battery-dead cart reporting an implausible year. */
bool gen3_clock_autosync(uint8_t* sb2, int y, int mo, int d, int h, int mi, int s);
bool gen3_clock_manual(uint8_t* sb2,
                       int ly, int lmo, int ld, int lh, int lmi, int ls,   /* live cart RTC */
                       int dy, int dmo, int dd, int dh, int dmi, int ds);  /* desired local */

char     gen3_decode_char(uint8_t c);
uint16_t gen3_checksum(const void* data, uint16_t size);

/* Sector index 0..13 in `slot` whose footer id == section_id (or -1). */
int gen3_find_section(const uint8_t* save, int slot, int section_id);

/* Reassemble SaveBlock1 (ids 1..4 of `slot`) contiguously into `dst`
 * (>= G3_SAVEBLOCK1_BYTES). Returns bytes written or 0 on failure. */
uint32_t gen3_read_saveblock1(const uint8_t* save, int slot, uint8_t* dst);

/* SaveBlock1 byte offset of the secret-base array for a version (0 if unknown). */
uint32_t gen3_secret_base_offset(Gen3Version version);

/* Count non-empty, sane secret-base records in a reassembled SaveBlock1 at the
 * given version's offset. Used for version detection and display. */
int gen3_count_secret_bases(const uint8_t* sb1, Gen3Version version);

/* ---- validation --------------------------------------------------------- */

/* Verify the stored checksum of one section of `slot`, given the data length
 * that section checksums over. Returns true if it matches. */
bool gen3_section_checksum_ok(const uint8_t* save, int slot, int section_id,
                              uint16_t data_size);

/* Verify checksums of every FULL (3968-byte) section in `slot`
 * (SaveBlock1 ids 1..3 and PkmnStorage ids 5..12). Partial sections (0,4,13)
 * are checked for a valid signature only (their checksummed size is
 * version-specific). Returns true if all checks pass. Reports the first
 * failing section id via *fail_id (or -1). */
bool gen3_verify_full_checksums(const uint8_t* save, int slot, int* fail_id);

/* ---- write-side helpers (bit-exact) ------------------------------------- */

/* Which SaveBlock1 section ids the secret-base array touches for `version`. */
void gen3_sb1_touch_sections(Gen3Version version, int* first_id, int* last_id);

/* Overwrite, in `save`, the 3968-byte data region of section `section_id` of
 * `slot` with `data`, then recompute and store that sector's checksum
 * (over 3968 bytes). Footer id/signature/counter and the unused padding are
 * preserved. `section_id` MUST be a full section. Returns the absolute byte
 * offset of the sector, or 0xFFFFFFFF on failure. */
uint32_t gen3_write_full_section(uint8_t* save, int slot, int section_id,
                                 const uint8_t* data);

#endif /* GEN3_SAVE_H */
