#ifndef GEN3_SECRETBASE_H
#define GEN3_SECRETBASE_H

#include <stdint.h>
#include <stdbool.h>

/* Gen-3 Secret Bases (Ruby/Sapphire/Emerald only; FireRed/LeafGreen have none).
 * SaveBlock1 holds an array of 20 fixed 160-byte records: slot 0 is the player's
 * own base, slots 1..19 are friends' bases registered via record-mixing. The
 * within-record field offsets are identical across RS and Emerald (Emerald adds a
 * `language` byte where RS keeps padding, but every following field stays put):
 *
 *   0x00 secretBaseId (u8)
 *   0x01 flags: toRegister:4, gender:1, battledOwnerToday:1, registryStatus:2
 *   0x02 trainerName[7]   (Gen-3 encoded)
 *   0x09 trainerId[4]
 *   0x0E numSecretBasesReceived (u16)
 *   0x10 numTimesEntered (u8)
 *   0x12 decorations[16]   0x22 decorationPositions[16]
 *   0x34 party: personality[6] u32, moves[24] u16, species[6] u16,
 *               heldItems[6] u16, levels[6] u8, EVs[6] u8   (total 0x6C)
 *
 * Pure C (no tonc) so it dual-compiles in tests/host_*. */

#define SB_COUNT      20
#define SB_RECORD     160
#define SB_PARTY      6
#define SB_DECOR      16

typedef struct {
  uint16_t species[SB_PARTY];        /* internal Gen-3 species id (0 = empty)  */
  uint8_t  level[SB_PARTY];
  uint16_t heldItem[SB_PARTY];
  uint16_t moves[SB_PARTY * 4];
  uint32_t personality[SB_PARTY];
  uint8_t  ev[SB_PARTY];
} SbParty;

typedef struct {
  int      slot;                     /* array index 0..19 (0 = own base)        */
  bool     own;                      /* slot == 0                               */
  uint8_t  id;                       /* secretBaseId                            */
  uint8_t  gender;                   /* 0 male, 1 female                        */
  uint8_t  registryStatus;
  bool     battledToday;
  char     trainerName[8];           /* decoded ASCII (<=7 + NUL)               */
  uint16_t trainerId;                /* low 16 bits of trainerId[]              */
  uint16_t numReceived;
  uint8_t  numEntered;
  int      decorCount;               /* non-zero decoration slots               */
  int      partyCount;               /* non-empty party species                 */
  SbParty  party;
} SbRecord;

/* Parse every NON-EMPTY record at sb1+base_off into out[] (out sized >= SB_COUNT),
 * preserving the array slot index. Returns the count parsed. base_off comes from
 * gen3_secret_base_offset(); pass 0 (FR/LG) to get 0 back. */
int sb_read_all(const uint8_t* sb1, uint32_t base_off, SbRecord* out);

/* True if the 160-byte record at sb1+base_off+slot*160 is empty (no base). */
bool sb_slot_empty(const uint8_t* sb1, uint32_t base_off, int slot);

/* Zero the 160-byte record for `slot` (clears that base). The caller commits SB1. */
void sb_clear(uint8_t* sb1, uint32_t base_off, int slot);

/* One secret-base party mon — the REDUCED format the record actually stores. A secret
 * base does NOT keep IVs, nickname, OT, friendship, met data, or per-stat EVs: the game
 * rebuilds the enemy mon from exactly these fields (nature/gender/ability all derive from
 * `personality`; the single `ev` byte is applied to every stat). Edit only what's here. */
typedef struct {
  uint16_t species;
  uint8_t  level;
  uint16_t heldItem;
  uint16_t moves[4];
  uint32_t personality;
  uint8_t  ev;
} SbPartyMon;

/* Write one party mon (idx 0..5) back into the 160-byte record for `slot`. The caller
 * commits SaveBlock1 (sections 1..4) via the verified-write path. */
void sb_write_mon(uint8_t* sb1, uint32_t base_off, int slot, int idx, const SbPartyMon* m);

/* Secret-base OWNER overworld sprite + battle trainer CLASS are DERIVED, not stored:
 *   class index 0..9 = gender*5 + (trainerId[0] % 5)   (5 male presets, then 5 female)
 * sb_owner_class returns that index for a record (-1 if the slot is empty/invalid).
 * sb_set_owner_class writes the gender bit (flags @0x01 bit4) and trainerId[0] (@0x09)
 * so the derived class becomes `cls` (0..9), changing trainerId[0] as little as possible.
 * NEVER call on slot 0 (your own base is regenerated from the trainer card). Caller
 * commits SB1. There are only these 10 presets — not a free choice of any NPC. */
int  sb_owner_class(const uint8_t* sb1, uint32_t base_off, int slot);
void sb_set_owner_class(uint8_t* sb1, uint32_t base_off, int slot, int cls);

#endif /* GEN3_SECRETBASE_H */
