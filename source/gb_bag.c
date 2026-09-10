#include "gb_bag.h"

#include <string.h>

/* ---- pocket <-> field table ------------------------------------------------- */

typedef struct {
  GbField count_field;
  GbField body_field;
  int     cap;
  bool    has_qty;
} GbBagPocketDesc;

static const GbBagPocketDesc k_pocket[GBB_POCKET_COUNT] = {
  [GBB_POCKET_ITEMS] = { GBF_BAG_COUNT,       GBF_BAG_BODY,       GBB_CAP_ITEMS, true  },
  [GBB_POCKET_KEY]   = { GBF_KEY_ITEMS_COUNT, GBF_KEY_ITEMS_BODY, GBB_CAP_KEY,   false },
  [GBB_POCKET_BALLS] = { GBF_BALLS_COUNT,     GBF_BALLS_BODY,     GBB_CAP_BALLS, true  },
  [GBB_POCKET_TMHM]  = { GBF_FIELD_COUNT,     GBF_TMHM_COUNTS,    0,             false },
  [GBB_POCKET_PC]    = { GBF_PC_COUNT,        GBF_PC_BODY,        GBB_CAP_PC,    true  },
};

static int entry_size(const GbBagPocketDesc* d) { return d->has_qty ? 2 : 1; }

bool gbb_field_present(GbGame game, GbBagPocket pocket) {
  if (pocket < 0 || pocket >= GBB_POCKET_COUNT) return false;
  if (pocket == GBB_POCKET_TMHM) return gbf_off(game, GBF_TMHM_COUNTS) != 0;
  return gbf_off(game, k_pocket[pocket].body_field) != 0;
}

int gbb_pocket_cap(GbGame game, GbBagPocket pocket) {
  if (!gbb_field_present(game, pocket)) return 0;
  if (pocket == GBB_POCKET_TMHM) return 0;   /* not a list */
  return k_pocket[pocket].cap;
}

GbField gbb_body_field(GbBagPocket pocket) {
  if (pocket < 0 || pocket >= GBB_POCKET_COUNT) return GBF_FIELD_COUNT;
  return k_pocket[pocket].body_field;
}

GbField gbb_count_field(GbBagPocket pocket) {
  if (pocket < 0 || pocket >= GBB_POCKET_COUNT) return GBF_FIELD_COUNT;
  return k_pocket[pocket].count_field;
}

uint8_t gbb_max_item_id(GbGame game) {
  return (game == GBF_G_RED || game == GBF_G_YELLOW) ? 0xFAu : 0xBEu;
}

static bool is_valid_id(GbGame game, uint8_t id) {
  if (id == 0x00u || id == 0xFFu) return false;
  return id <= gbb_max_item_id(game);
}

/* See gb_bag.h for the full contract and the "documented fallback, not a
 * located ROM table" note. Sorted so a future ROM-locate swap can keep this
 * shape (linear scan; the list is short enough that a binary search buys
 * nothing and would only add an unproven "must stay sorted" invariant). */
bool gbb_is_g1_key_item(uint8_t id) {
  if (id >= 0xC4u && id <= 0xC8u) return true;   /* HM01..HM05 */
  static const uint8_t kKeyIds[] = {
    5,   /* TOWN MAP     */  6,   /* BICYCLE      */
    31,  /* OLD AMBER    */  41,  /* DOME FOSSIL  */  42, /* HELIX FOSSIL */
    43,  /* SECRET KEY   */  45,  /* BIKE VOUCHER */
    48,  /* CARD KEY     */  63,  /* S.S.TICKET   */  64, /* GOLD TEETH   */
    69,  /* COIN CASE    */  70,  /* OAK'S PARCEL */  71, /* ITEMFINDER   */
    72,  /* SILPH SCOPE  */  73,  /* POKE FLUTE   */  74, /* LIFT KEY     */
    76,  /* OLD ROD      */  77,  /* GOOD ROD     */  78, /* SUPER ROD    */
  };
  for (unsigned i = 0; i < sizeof(kKeyIds); i++)
    if (kKeyIds[i] == id) return true;
  return false;
}

/* ---- read -------------------------------------------------------------------- */

static void read_list(const GbSession* s, GbGame g, GbBagPocket pocket, GbBagList* out) {
  memset(out, 0, sizeof *out);
  const GbBagPocketDesc* d = &k_pocket[pocket];
  uint32_t coff = gbf_off(g, d->count_field);
  uint32_t boff = gbf_off(g, d->body_field);
  uint16_t blen = gbf_len(g, d->body_field);
  if (!coff || !boff || !blen) return;   /* this game lacks the pocket */

  GbSession* ncs = (GbSession*)(const void*)s;
  uint8_t cnt = 0;
  if (gbs_read_field(ncs, coff, &cnt, 1) != GBS_OK) return;

  uint8_t body[GBB_MAX_BODY];
  if (blen > sizeof body) blen = sizeof body;   /* defensive; widest field is 101 B */
  if (gbs_read_field(ncs, boff, body, blen) != GBS_OK) return;

  int esz = entry_size(d);
  int want = cnt;
  if (want > d->cap) want = d->cap;

  int n = 0;
  for (int i = 0; i < want; i++) {
    int base = i * esz;
    if (base + esz > blen) break;               /* would read past the field's bytes */
    uint8_t id = body[base];
    if (id == 0xFFu) break;                      /* terminator hit before `cnt`       */
    out->entries[n].id  = id;
    out->entries[n].qty = d->has_qty ? body[base + 1] : 1;
    n++;
  }
  out->count = (uint8_t)n;
}

bool gbb_read(const GbSession* s, GbBag* out) {
  if (!s || !out || !s->open) return false;
  memset(out, 0, sizeof *out);

  /* Yellow deliberately maps to GBF_G_RED here (as gbt_game in gb_trainer.c does):
   * its wMainDataStart shifts by one byte and so does the bag, so every bag offset
   * is identical in the save (review 2026-09-09 re-derived 0x25C9/0x25CA on both). */
  GbGame g = (s->gen == GB_GEN1) ? GBF_G_RED
                                  : ((s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL
                                                                            : GBF_G_GS);

  for (int p = 0; p < GBB_POCKET_COUNT; p++) {
    if (p == GBB_POCKET_TMHM) continue;
    if (!gbb_field_present(g, (GbBagPocket)p)) continue;
    read_list(s, g, (GbBagPocket)p, &out->pockets[p]);
  }

  uint32_t toff = gbf_off(g, GBF_TMHM_COUNTS);
  uint16_t tlen = gbf_len(g, GBF_TMHM_COUNTS);
  if (toff && tlen == GBB_TMHM_COUNT) {
    GbSession* ncs = (GbSession*)(const void*)s;
    gbs_read_field(ncs, toff, out->tmhm_counts, GBB_TMHM_COUNT);
  }

  return true;
}

/* ---- write --------------------------------------------------------------------
 *
 * Pocket-granularity diff: build the CANDIDATE count byte + body bytes from `in`,
 * starting from a COPY of the CURRENT on-disk bytes for that pocket (so anything
 * past the new terminator is left exactly as it was found, never zero-filled --
 * the same residue-preserving discipline gb_trainer.c's set_name_if_changed uses
 * for the player-name field's post-terminator tail), then only call
 * gbs_write_field for whichever of {count, body} actually differs. */

static GbsStatus write_list(GbSession* s, GbGame g, GbBagPocket pocket,
                            const GbBagList* in, bool* changed) {
  const GbBagPocketDesc* d = &k_pocket[pocket];
  uint32_t coff = gbf_off(g, d->count_field);
  uint32_t boff = gbf_off(g, d->body_field);
  uint16_t blen = gbf_len(g, d->body_field);
  if (!coff || !boff || !blen) return GBS_OK;   /* this game lacks the pocket -- no-op */
  if (blen > GBB_MAX_BODY) return GBS_ERR_ARG;  /* defensive; widest field is 101 B    */

  uint8_t cur_cnt = 0;
  uint8_t cur_body[GBB_MAX_BODY];
  if (gbs_read_field(s, coff, &cur_cnt, 1) != GBS_OK) return GBS_ERR_ARG;
  if (gbs_read_field(s, boff, cur_body, blen) != GBS_OK) return GBS_ERR_ARG;

  int esz = entry_size(d);
  int n = in->count;
  if (n > d->cap) n = d->cap;

  uint8_t new_body[GBB_MAX_BODY];
  memcpy(new_body, cur_body, blen);   /* start from current -- preserves tail residue */
  int pos = 0;
  for (int i = 0; i < n; i++) {
    if (pos + esz > blen) break;
    new_body[pos] = in->entries[i].id;
    if (d->has_qty) new_body[pos + 1] = in->entries[i].qty;
    pos += esz;
  }
  if (pos < blen) new_body[pos] = 0xFFu;   /* terminator; bytes past it untouched */

  uint8_t new_cnt = (uint8_t)n;
  bool cnt_changed  = (new_cnt != cur_cnt);
  bool body_changed = (memcmp(new_body, cur_body, blen) != 0);
  if (!cnt_changed && !body_changed) return GBS_OK;

  if (cnt_changed) {
    GbsStatus st = gbs_write_field(s, coff, &new_cnt, 1);
    if (st != GBS_OK) return st;
  }
  if (body_changed) {
    GbsStatus st = gbs_write_field(s, boff, new_body, blen);
    if (st != GBS_OK) return st;
  }
  *changed = true;
  return GBS_OK;
}

GbsStatus gbb_write(GbSession* s, const GbBag* in) {
  if (!s || !in || !s->open) return GBS_ERR_ARG;

  /* Yellow deliberately maps to GBF_G_RED here (as gbt_game in gb_trainer.c does):
   * its wMainDataStart shifts by one byte and so does the bag, so every bag offset
   * is identical in the save (review 2026-09-09 re-derived 0x25C9/0x25CA on both). */
  GbGame g = (s->gen == GB_GEN1) ? GBF_G_RED
                                  : ((s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL
                                                                            : GBF_G_GS);
  bool changed = false;

  for (int p = 0; p < GBB_POCKET_COUNT; p++) {
    if (p == GBB_POCKET_TMHM) continue;
    if (!gbb_field_present(g, (GbBagPocket)p)) continue;
    GbsStatus st = write_list(s, g, (GbBagPocket)p, &in->pockets[p], &changed);
    if (st != GBS_OK) return st;
  }

  uint32_t toff = gbf_off(g, GBF_TMHM_COUNTS);
  uint16_t tlen = gbf_len(g, GBF_TMHM_COUNTS);
  if (toff && tlen == GBB_TMHM_COUNT) {
    uint8_t cur[GBB_TMHM_COUNT];
    if (gbs_read_field(s, toff, cur, GBB_TMHM_COUNT) != GBS_OK) return GBS_ERR_ARG;
    if (memcmp(cur, in->tmhm_counts, GBB_TMHM_COUNT) != 0) {
      GbsStatus st = gbs_write_field(s, toff, in->tmhm_counts, GBB_TMHM_COUNT);
      if (st != GBS_OK) return st;
      changed = true;
    }
  }

  return changed ? gbs_finish(s) : GBS_OK;
}

/* ---- in-memory model edits --------------------------------------------------- */

GbBagOpStatus gbb_insert(GbGame game, GbBag* bag, GbBagPocket pocket,
                          uint8_t id, uint8_t qty) {
  if (!bag || pocket < 0 || pocket >= GBB_POCKET_COUNT || pocket == GBB_POCKET_TMHM)
    return GBB_ERR_ARG;
  if (!gbb_field_present(game, pocket)) return GBB_ERR_NOT_PRESENT;
  if (!is_valid_id(game, id)) return GBB_ERR_BADID;

  const GbBagPocketDesc* d = &k_pocket[pocket];
  if (!d->has_qty) {
    if (qty != 1u) return GBB_ERR_QTY;
  } else {
    if (qty < 1u || qty > GBB_QTY_CAP) return GBB_ERR_QTY;
  }

  GbBagList* list = &bag->pockets[pocket];
  for (int i = 0; i < list->count; i++) {
    if (list->entries[i].id != id) continue;
    if (!d->has_qty) return GBB_ERR_ARG;   /* duplicate key item: refuse, don't merge */
    uint32_t sum = (uint32_t)list->entries[i].qty + qty;
    if (sum > GBB_QTY_CAP) {
      list->entries[i].qty = GBB_QTY_CAP;   /* saturate, but tell the caller */
      return GBB_ERR_QTY;
    }
    list->entries[i].qty = (uint8_t)sum;
    return GBB_OK;
  }

  if (list->count >= d->cap) return GBB_ERR_FULL;
  list->entries[list->count].id  = id;
  list->entries[list->count].qty = qty;
  list->count++;
  return GBB_OK;
}

GbBagOpStatus gbb_remove(GbGame game, GbBag* bag, GbBagPocket pocket, int slot) {
  if (!bag || pocket < 0 || pocket >= GBB_POCKET_COUNT || pocket == GBB_POCKET_TMHM)
    return GBB_ERR_ARG;
  if (!gbb_field_present(game, pocket)) return GBB_ERR_NOT_PRESENT;

  GbBagList* list = &bag->pockets[pocket];
  if (slot < 0 || slot >= list->count) return GBB_ERR_ARG;

  for (int i = slot; i + 1 < list->count; i++) list->entries[i] = list->entries[i + 1];
  list->count--;
  list->entries[list->count].id  = 0;
  list->entries[list->count].qty = 0;
  return GBB_OK;
}

GbBagOpStatus gbb_set_qty(GbGame game, GbBag* bag, GbBagPocket pocket,
                           int slot, uint8_t qty) {
  if (!bag || pocket < 0 || pocket >= GBB_POCKET_COUNT || pocket == GBB_POCKET_TMHM)
    return GBB_ERR_ARG;
  if (!gbb_field_present(game, pocket)) return GBB_ERR_NOT_PRESENT;
  if (!k_pocket[pocket].has_qty) return GBB_ERR_ARG;   /* Key items carry no quantity */
  if (qty < 1u || qty > GBB_QTY_CAP) return GBB_ERR_QTY;

  GbBagList* list = &bag->pockets[pocket];
  if (slot < 0 || slot >= list->count) return GBB_ERR_ARG;
  list->entries[slot].qty = qty;
  return GBB_OK;
}

bool gbb_tmhm_get(const GbBag* bag, int tmhm_index, uint8_t* count_out) {
  if (!bag || !count_out || tmhm_index < 0 || tmhm_index >= GBB_TMHM_COUNT) return false;
  *count_out = bag->tmhm_counts[tmhm_index];
  return true;
}

GbBagOpStatus gbb_tmhm_set(GbGame game, GbBag* bag, int tmhm_index, uint8_t count) {
  if (!bag || tmhm_index < 0 || tmhm_index >= GBB_TMHM_COUNT) return GBB_ERR_ARG;
  if (!gbb_field_present(game, GBB_POCKET_TMHM)) return GBB_ERR_NOT_PRESENT;
  if (count > GBB_TMHM_CAP) return GBB_ERR_QTY;
  bag->tmhm_counts[tmhm_index] = count;
  return GBB_OK;
}
