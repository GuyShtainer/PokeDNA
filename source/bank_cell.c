#include "bank_cell.h"
#include <string.h>

/* --- little-endian helpers (hard rule 5: explicit, no bit-cast, no memcpy-onto-u32) */
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* --- FNV-1a-32, the standard constants (art_cache.h's ART_FNV1A_INIT uses the same
 * offset basis; not shared from there because that header pulls in art-cache-specific
 * declarations this pure codec has no business depending on). */
#define BC_FNV_OFFSET 0x811c9dc5u
#define BC_FNV_PRIME  0x01000193u

static uint32_t fnv1a_feed(uint32_t h, const uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= BC_FNV_PRIME;
  }
  return h;
}

uint32_t bc_ident32(const uint8_t rec80[BC_CELL_BYTES]) {
  if (!rec80) return 0;
  uint32_t h = BC_FNV_OFFSET;
  h = fnv1a_feed(h, rec80 + 8, 3u);    /* bytes  8..10 inclusive */
  h = fnv1a_feed(h, rec80 + 12, 57u);  /* bytes 12..68 inclusive */
  h = fnv1a_feed(h, rec80 + 73, 4u);   /* bytes 73..76 inclusive */
  return h;
}

bool bc_is_native(const uint8_t rec80[BC_CELL_BYTES]) {
  if (!rec80) return false;
  if (rec80[0] != BC_MAGIC_0 || rec80[1] != BC_MAGIC_1 ||
      rec80[2] != BC_MAGIC_2 || rec80[3] != BC_MAGIC_3)
    return false;
  uint8_t gen = rec80[BC_OFF_GEN];
  if (gen != GB_GEN1 && gen != GB_GEN2) return false;
  if (rec80[BC_OFF_OLDBUILD] != BC_OLDBUILD_BYTE) return false;
  uint32_t stored = rd32(rec80 + BC_OFF_IDENT32);
  return stored == bc_ident32(rec80);
}

uint8_t bc_kind(const uint8_t rec80[BC_CELL_BYTES]) {
  if (!bc_is_native(rec80)) return 0;
  return rec80[BC_OFF_GEN];
}

int bc_pack(const GbEditMon* mon, uint8_t flags, uint8_t origin_game,
            uint32_t rtc_epoch, uint32_t bank_serial, uint8_t out80[BC_CELL_BYTES]) {
  if (!mon || !out80) return -1;
  if (mon->gen != GB_GEN1 && mon->gen != GB_GEN2) return -1;
  /* gb_commit's own refusal (gb_edit.h): the list terminator is never a real slot's
   * species byte, so a cell can never legitimately hold it either. */
  if (mon->list_species == GB_LIST_TERMINATOR) return -1;

  /* Box-shape record, 33 bytes wide (the larger of the two generations' box sizes;
   * Gen-2 uses only the first 32 and leaves rec33[32] at its memset zero). */
  uint8_t rec33[GEN1_BOX_REC_BYTES];
  memset(rec33, 0, sizeof rec33);
  uint8_t rec_len;

  if (mon->gen == GB_GEN1) {
    rec_len = GEN1_BOX_REC_BYTES;
    memcpy(rec33, mon->rec, GEN1_BOX_REC_BYTES);
    if (mon->is_party) {
      /* party -> box truncation, matching gbs_move()'s own rule (gb_session.c): sync
       * the box-level byte to the live level BEFORE the party-only tail is dropped. */
      rec33[G1R_BOXLEVEL] = mon->rec[G1R_LEVEL];
    }
  } else { /* GB_GEN2 */
    rec_len = (uint8_t)G2_BOX_ENTRY;
    memcpy(rec33, mon->rec, G2_BOX_ENTRY);
    /* Gen-2 party -> box drops bytes 32..47 to zero (g2w_slot_to_box's own rule);
     * rec33[32] never receives anything above, so it is already correct either way. */
  }

  memset(out80, 0, BC_CELL_BYTES);
  out80[0] = BC_MAGIC_0;
  out80[1] = BC_MAGIC_1;
  out80[2] = BC_MAGIC_2;
  out80[3] = BC_MAGIC_3;
  out80[BC_OFF_GEN]          = mon->gen;
  out80[BC_OFF_REC_LEN]      = rec_len;
  out80[BC_OFF_LIST_SPECIES] = mon->list_species;
  out80[BC_OFF_FLAGS]        = flags;
  memcpy(out80 + BC_OFF_REC_LO, rec33, 7);
  out80[BC_OFF_OLDBUILD] = BC_OLDBUILD_BYTE;
  memcpy(out80 + BC_OFF_REC_HI, rec33 + 7, 26);
  memcpy(out80 + BC_OFF_OTNAME, mon->otname, GB_NAME_BYTES);
  memcpy(out80 + BC_OFF_NICK, mon->nick, GB_NAME_BYTES);
  out80[BC_OFF_ORIGIN_GAME] = origin_game;
  wr32(out80 + BC_OFF_RTC_EPOCH, rtc_epoch);
  wr32(out80 + BC_OFF_BANK_SERIAL, bank_serial);
  /* bytes 77..79 stay zero (reserved) from the memset above */

  wr32(out80 + BC_OFF_IDENT32, bc_ident32(out80));
  return 0;
}

bool bc_unpack(const uint8_t rec80[BC_CELL_BYTES], GbEditMon* mon, BcMeta* meta) {
  if (!rec80 || !mon || !meta) return false;
  if (!bc_is_native(rec80)) return false;

  uint8_t gen = rec80[BC_OFF_GEN];
  uint8_t rec33[GEN1_BOX_REC_BYTES];
  memcpy(rec33, rec80 + BC_OFF_REC_LO, 7);
  memcpy(rec33 + 7, rec80 + BC_OFF_REC_HI, 26);

  /* A cell only ever holds the box shape (S150-5 lifts a party mon into a box-shape
   * cell); is_party is always false on the way back out. gb_rec_size(gen, false)
   * picks the right length (33/32) for gb_load_parts to copy. */
  if (!gb_load_parts(mon, gen, false, rec33, rec80 + BC_OFF_OTNAME, rec80 + BC_OFF_NICK,
                     rec80[BC_OFF_LIST_SPECIES]))
    return false;

  meta->gen         = gen;
  meta->flags       = rec80[BC_OFF_FLAGS];
  meta->origin_game = rec80[BC_OFF_ORIGIN_GAME];
  meta->rtc_epoch   = rd32(rec80 + BC_OFF_RTC_EPOCH);
  meta->bank_serial = rd32(rec80 + BC_OFF_BANK_SERIAL);
  return true;
}
