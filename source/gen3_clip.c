#include "gen3_clip.h"
#include "gen3_edit.h"     /* gen3_edit_load/commit, em_set_party_flag */
#include "gen3_mon.h"      /* pk_decode_mon (validation) */
#include "gen3_box.h"      /* G3_IN_BOX */
#include <string.h>

#define BOX_MON   80
#define PARTY_MON 100
#define PC_OFF_BOXES 0x0004
#define PARTY_MAX 6

/* live-party offsets within SaveBlock1 (FRLG moved the block to the start). */
static uint16_t party_count_off(bool frlg) { return frlg ? 0x0034 : 0x0234; }
static uint16_t party_data_off(bool frlg)  { return frlg ? 0x0038 : 0x0238; }

void clip_copy_from(ClipMon* c, const uint8_t* rec, bool is_party) {
  memset(c, 0, sizeof(*c));
  memcpy(c->rec, rec, is_party ? PARTY_MON : BOX_MON);
  c->is_party = is_party;
  c->occupied = true;
}

bool clip_to_record(const ClipMon* c, bool dst_is_party, uint8_t out[100]) {
  if (!c->occupied) return false;
  if (!dst_is_party) { memcpy(out, c->rec, BOX_MON); return true; }   /* any -> box: first 80 */
  if (c->is_party)   { memcpy(out, c->rec, PARTY_MON); return true; } /* party -> party: exact */
  EditMon e;                                                          /* box -> party: derive stats */
  gen3_edit_load(c->rec, false, &e);
  em_set_party_flag(&e, true);
  gen3_edit_commit(&e, out);
  return true;
}

bool pk3_validate(const uint8_t rec80[80]) {
  PkMon m;
  if (!pk_decode_mon(rec80, false, &m)) return false;   /* empty slot */
  if (m.isBadEgg) return false;                          /* checksum failed */
  return m.species >= 1 && m.species <= 411;
}

/* ---- box slots ---- */
uint8_t* pk_box_slot(uint8_t* pc, int box, int slot) {
  return pc + PC_OFF_BOXES + ((uint32_t)(box * G3_IN_BOX + slot)) * BOX_MON;
}
void clip_write_box_slot(uint8_t* pc, int box, int slot, const uint8_t rec80[80]) {
  memcpy(pk_box_slot(pc, box, slot), rec80, BOX_MON);
}
void clip_clear_box_slot(uint8_t* pc, int box, int slot) {
  memset(pk_box_slot(pc, box, slot), 0, BOX_MON);
}

/* ---- party (count-tracked, gap-free) ---- */
int party_count(const uint8_t* sb1, bool frlg) {
  return sb1[party_count_off(frlg)];
}
uint8_t* pk_party_slot(uint8_t* sb1, bool frlg, int idx) {
  return sb1 + party_data_off(frlg) + (uint32_t)idx * PARTY_MON;
}
bool party_append(uint8_t* sb1, bool frlg, const uint8_t rec100[100]) {
  int n = party_count(sb1, frlg);
  if (n >= PARTY_MAX) return false;
  memcpy(pk_party_slot(sb1, frlg, n), rec100, PARTY_MON);
  sb1[party_count_off(frlg)] = (uint8_t)(n + 1);
  return true;
}
void party_write(uint8_t* sb1, bool frlg, int idx, const uint8_t rec100[100]) {
  int n = party_count(sb1, frlg);
  if (idx < 0 || idx >= n) return;
  memcpy(pk_party_slot(sb1, frlg, idx), rec100, PARTY_MON);
}
void party_release(uint8_t* sb1, bool frlg, int idx) {
  int n = party_count(sb1, frlg);
  if (idx < 0 || idx >= n) return;
  for (int i = idx; i < n - 1; i++)                      /* shift the rest down to stay gap-free */
    memcpy(pk_party_slot(sb1, frlg, i), pk_party_slot(sb1, frlg, i + 1), PARTY_MON);
  memset(pk_party_slot(sb1, frlg, n - 1), 0, PARTY_MON); /* clear the now-vacant last slot */
  sb1[party_count_off(frlg)] = (uint8_t)(n - 1);
}
