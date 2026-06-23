#include "gen3_secretbase.h"
#include "gen3_save.h"     /* gen3_decode_char (pure) */

#include <string.h>

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* within-record field offsets (RS == Emerald) */
#define O_ID      0x00
#define O_FLAGS   0x01
#define O_NAME    0x02
#define O_TID     0x09
#define O_RECV    0x0E
#define O_ENTER   0x10
#define O_DECOR   0x12
#define O_PARTY   0x34
#define O_P_PERS  (O_PARTY + 0x00)   /* u32[6] */
#define O_P_MOVE  (O_PARTY + 0x18)   /* u16[24] */
#define O_P_SPEC  (O_PARTY + 0x48)   /* u16[6]  -> record 0x7C */
#define O_P_ITEM  (O_PARTY + 0x54)   /* u16[6]  -> record 0x88 */
#define O_P_LVL   (O_PARTY + 0x60)   /* u8[6]   -> record 0x94 */
#define O_P_EV    (O_PARTY + 0x66)   /* u8[6]   -> record 0x9A */

static bool rec_empty(const uint8_t* r) {
  return r[O_ID] == 0 && rd16(r + O_P_SPEC) == 0;
}

bool sb_slot_empty(const uint8_t* sb1, uint32_t base_off, int slot) {
  if (base_off == 0 || slot < 0 || slot >= SB_COUNT) return true;
  return rec_empty(sb1 + base_off + (uint32_t)slot * SB_RECORD);
}

static void parse_rec(const uint8_t* r, int slot, SbRecord* o) {
  memset(o, 0, sizeof(*o));
  o->slot = slot;
  o->own  = (slot == 0);
  o->id   = r[O_ID];
  uint8_t f = r[O_FLAGS];
  o->gender         = (f >> 4) & 1;
  o->battledToday   = ((f >> 5) & 1) != 0;
  o->registryStatus = (f >> 6) & 3;
  int k = 0;
  for (; k < 7; k++) { char c = gen3_decode_char(r[O_NAME + k]); if (!c) break; o->trainerName[k] = c; }
  o->trainerName[k] = 0;
  o->trainerId  = rd16(r + O_TID);
  o->numReceived = rd16(r + O_RECV);
  o->numEntered  = r[O_ENTER];
  for (int i = 0; i < SB_DECOR; i++) if (r[O_DECOR + i]) o->decorCount++;
  for (int i = 0; i < SB_PARTY; i++) {
    o->party.species[i]     = rd16(r + O_P_SPEC + i * 2);
    o->party.heldItem[i]    = rd16(r + O_P_ITEM + i * 2);
    o->party.level[i]       = r[O_P_LVL + i];
    o->party.ev[i]          = r[O_P_EV + i];
    o->party.personality[i] = rd32(r + O_P_PERS + i * 4);
    for (int m = 0; m < 4; m++) o->party.moves[i * 4 + m] = rd16(r + O_P_MOVE + (i * 4 + m) * 2);
    if (o->party.species[i]) o->partyCount++;
  }
}

int sb_read_all(const uint8_t* sb1, uint32_t base_off, SbRecord* out) {
  if (base_off == 0) return 0;
  int n = 0;
  for (int slot = 0; slot < SB_COUNT; slot++) {
    const uint8_t* r = sb1 + base_off + (uint32_t)slot * SB_RECORD;
    if (rec_empty(r)) continue;
    parse_rec(r, slot, &out[n++]);
  }
  return n;
}

void sb_clear(uint8_t* sb1, uint32_t base_off, int slot) {
  if (base_off == 0 || slot < 0 || slot >= SB_COUNT) return;
  memset(sb1 + base_off + (uint32_t)slot * SB_RECORD, 0, SB_RECORD);
}

void sb_write_mon(uint8_t* sb1, uint32_t base_off, int slot, int idx, const SbPartyMon* m) {
  if (base_off == 0 || slot < 0 || slot >= SB_COUNT || idx < 0 || idx >= SB_PARTY || !m) return;
  uint8_t* r = sb1 + base_off + (uint32_t)slot * SB_RECORD;
  wr16(r + O_P_SPEC + idx * 2, m->species);
  wr16(r + O_P_ITEM + idx * 2, m->heldItem);
  r[O_P_LVL + idx] = m->level;
  r[O_P_EV  + idx] = m->ev;
  wr32(r + O_P_PERS + idx * 4, m->personality);
  for (int k = 0; k < 4; k++) wr16(r + O_P_MOVE + (idx * 4 + k) * 2, m->moves[k]);
}

int sb_owner_class(const uint8_t* sb1, uint32_t base_off, int slot) {
  if (base_off == 0 || slot < 0 || slot >= SB_COUNT) return -1;
  const uint8_t* r = sb1 + base_off + (uint32_t)slot * SB_RECORD;
  if (rec_empty(r)) return -1;
  int gender = (r[O_FLAGS] >> 4) & 1;
  return gender * 5 + (r[O_TID] % 5);                  /* trainerId[0] is the low byte at O_TID */
}

void sb_set_owner_class(uint8_t* sb1, uint32_t base_off, int slot, int cls) {
  if (base_off == 0 || slot <= 0 || slot >= SB_COUNT || cls < 0 || cls > 9) return;  /* never slot 0 */
  uint8_t* r = sb1 + base_off + (uint32_t)slot * SB_RECORD;
  int gender = cls / 5, col = cls % 5;
  r[O_FLAGS] = (uint8_t)((r[O_FLAGS] & ~0x10) | (gender ? 0x10 : 0));   /* flags bit4 = gender */
  int base5 = r[O_TID] - (r[O_TID] % 5);              /* keep trainerId[0] close, set residue=col */
  if (base5 + col > 255) base5 -= 5;
  r[O_TID] = (uint8_t)(base5 + col);
}
