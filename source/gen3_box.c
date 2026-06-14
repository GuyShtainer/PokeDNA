#include "gen3_box.h"
#include "gen3_save.h"      /* gen3_find_section, gen3_decode_char, sizes */
#include "data_tables.h"
#include <string.h>

#define PC_OFF_BOXES     0x0004
#define PC_OFF_NAMES     0x8344
#define PC_OFF_WALLPAPER 0x83C2
#define BOX_MON_SIZE     80

uint32_t gen3_read_pc_storage(const uint8_t* save, int slot, uint8_t* dst) {
  uint32_t written = 0;
  for (int id = G3_SID_PKMN_STORAGE_START; id <= G3_SID_PKMN_STORAGE_END; id++) {
    int s = gen3_find_section(save, slot, id);
    if (s < 0) return 0;
    const uint8_t* sec = save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)s * G3_SECTOR_SIZE;
    memcpy(dst + written, sec, G3_SECTOR_DATA_SIZE);
    written += G3_SECTOR_DATA_SIZE;
  }
  return written; /* 35712 */
}

uint8_t pk_current_box(const uint8_t* pc) { return pc[0]; }

void pk_box_name(const uint8_t* pc, int box, char out[12]) {
  const uint8_t* src = pc + PC_OFF_NAMES + (uint32_t)box * 9;
  int k = 0;
  for (; k < 8; k++) { char c = gen3_decode_char(src[k]); if (!c) break; out[k] = c; }
  out[k] = 0;
}

uint8_t pk_box_wallpaper(const uint8_t* pc, int box) { return pc[PC_OFF_WALLPAPER + box]; }

uint8_t gen3_encode_char(char c);   /* from gen3_edit.c (Gen-3 string encoder) */

void pk_set_box_name(uint8_t* pc, int box, const char* s) {
  uint8_t* dst = pc + PC_OFF_NAMES + (uint32_t)box * 9;
  int i = 0;
  for (; i < 8 && s[i]; i++) dst[i] = gen3_encode_char(s[i]);
  for (; i < 9; i++) dst[i] = 0xFF;     /* 0xFF = EOS + pad (box-name field is 9 bytes) */
}

void pk_set_box_wallpaper(uint8_t* pc, int box, uint8_t wp) {
  if (wp > G3_BOX_WALLPAPER_FRIENDS) wp = G3_BOX_WALLPAPER_FRIENDS;
  pc[PC_OFF_WALLPAPER + box] = wp;
}

/* Emerald "Walda"/secret wallpaper config lives in SaveBlock1's WaldaPhrase
 * struct (EMERALD ONLY): colors[2] @ +0, text[16] @ +4, iconId @ +20,
 * patternId @ +21, patternUnlocked @ +22. A box whose wallpaper byte is
 * G3_BOX_WALLPAPER_FRIENDS (16) shows sWaldaWallpapers[patternId]. Caller must
 * gate on Emerald — the offset differs on R/S/FR/LG. */
#define WALDA_OFF 0x3D70
uint8_t pk_walda_pattern(const uint8_t* sb1) { return sb1[WALDA_OFF + 21] & 0x0F; }
void pk_set_walda_pattern(uint8_t* sb1, uint8_t pattern) {
  if (pattern > 15) pattern = 15;
  sb1[WALDA_OFF + 21] = pattern;
  sb1[WALDA_OFF + 22] = 1;                  /* patternUnlocked */
}

void pk_resolve(PkMon* m) {
  if (m->species == 0) return;
  m->gender = pk_gender_from(m->personality, pk_species_gender_ratio(m->species));
  if (m->isParty) return;                 /* party carries plaintext level + stats */
  uint8_t growth = pk_species_growth(m->species);
  m->level = pk_level_from_exp(growth, m->experience);
  uint8_t base[6];
  pk_base_stats(m->species, base);
  int nb = pk_nature_boost(m->nature), nh = pk_nature_hinder(m->nature);
  m->stats[PK_HP] = pk_calc_hp(base[PK_HP], m->ivs[PK_HP], m->evs[PK_HP], m->level);
  for (int s = PK_ATK; s <= PK_SPD; s++) {
    int mod = (s == nb) ? 1 : (s == nh) ? -1 : 0;
    m->stats[s] = pk_calc_stat(base[s], m->ivs[s], m->evs[s], m->level, mod);
  }
}

int pk_decode_box_raw(const uint8_t* recs, PkMon out[30]) {
  int n = 0;
  for (int slot = 0; slot < G3_IN_BOX; slot++) {
    const uint8_t* mon = recs + (uint32_t)slot * BOX_MON_SIZE;
    if (pk_decode_mon(mon, false, &out[slot])) {  /* false => 80-byte box record */
      pk_resolve(&out[slot]);
      n++;
    } else {
      out[slot].species = 0;                       /* empty cell */
    }
  }
  return n;
}

int pk_read_box(const uint8_t* pc, int box, PkMon out[30]) {
  return pk_decode_box_raw(pc + PC_OFF_BOXES + (uint32_t)box * G3_IN_BOX * BOX_MON_SIZE, out);
}
