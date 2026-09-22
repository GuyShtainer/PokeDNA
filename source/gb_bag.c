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

/* Review D2: pokegold's OWN data/items/attributes.asm (independently checked
 * against pokecrystal's copy, both files parsed the same way gb_bag.h's own
 * derivation comment describes) differs from pokecrystal's at exactly four
 * ids -- 0x46/0x73/0x74/0x81 (CLEAR_BELL/GS_BALL/BLUE_CARD/EGG_TICKET on
 * Crystal) are pocket ITEM (an unused ITEM_46/73/74/81 placeholder, the same
 * shape as every other never-distributed id) on Gold/Silver, not KEY_ITEM.
 * gb_item_names.c's own comment already knew these four NAMES are Crystal-
 * only ("G/S just lack 4 Crystal-only items... shipped anyway since the
 * byte never occurs in a real G/S bag") -- what it did NOT carry through is
 * that a G/S save therefore has NO valid item at these four ids at all, not
 * merely an unnamed one: is_valid_id() must refuse them on GBF_G_GS so
 * gbb_insert() and the picker's own admission test agree the id does not
 * exist there, the same as any other out-of-range id. */
static bool is_valid_id(GbGame game, uint8_t id) {
  if (id == 0x00u || id == 0xFFu) return false;
  if (game == GBF_G_GS && (id == 0x46u || id == 0x73u || id == 0x74u || id == 0x81u)) return false;
  return id <= gbb_max_item_id(game);
}

/* See gb_bag.h for the full contract and the "documented fallback, not a
 * located ROM table" note. KNOWN INCOMPLETE: the unused id block 81..195 was
 * never swept on the cart (U4 review N2, 2026-09-10); only a ROM-located
 * key-item bit table can settle it -- BACKLOG. Sorted so a future ROM-locate swap can keep this
 * shape (linear scan; the list is short enough that a binary search buys
 * nothing and would only add an unproven "must stay sorted" invariant). */
bool gbb_is_g1_key_item(uint8_t id) {
  if (id >= 0xC4u && id <= 0xC8u) return true;   /* HM01..HM05 */
  if (id >= 21u && id <= 28u) return true;       /* the 8 badges: all KEY, verified on the cart (U4 review N2) */
  static const uint8_t kKeyIds[] = {
    5,   /* TOWN MAP     */  6,   /* BICYCLE      */  7,  /* ????? (unused, KEY on the cart) */
    8,   /* SAFARI BALL  */  9,   /* POKeDEX      */
    31,  /* OLD AMBER    */  41,  /* DOME FOSSIL  */  42, /* HELIX FOSSIL */
    43,  /* SECRET KEY   */  44,  /* ????? (unused, KEY on the cart) */  45,  /* BIKE VOUCHER */
    48,  /* CARD KEY     */  63,  /* S.S.TICKET   */  64, /* GOLD TEETH   */
    69,  /* COIN CASE    */  70,  /* OAK'S PARCEL */  71, /* ITEMFINDER   */
    72,  /* SILPH SCOPE  */  73,  /* POKE FLUTE   */  74, /* LIFT KEY     */
    76,  /* OLD ROD      */  77,  /* GOOD ROD     */  78, /* SUPER ROD    */
  };
  for (unsigned i = 0; i < sizeof kKeyIds / sizeof kKeyIds[0]; i++)
    if (kKeyIds[i] == id) return true;
  return false;
}

bool gbb_g1_key_item_compose(bool have_table, const uint8_t bits[15], uint8_t id) {
  if (id >= 0xC4u) return id <= 0xC8u;   /* HM01..HM05 key; TM01+ not (the game's own path) */
  if (have_table) {
    if (id == 0u || id > 120u) return false;   /* the table's own code-bound length */
    unsigned i = (unsigned)id - 1u;
    return (bits[i >> 3] >> (i & 7u)) & 1u;
  }
  return gbb_is_g1_key_item(id);
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

/* ---- BACKLOG #195: per-item pocket membership --------------------------- */

/* See gb_bag.h for the full derivation comment (attributes.asm's own pocket
 * column). Sorted-enough-to-not-matter linear scans, same posture
 * gbb_is_g1_key_item() already uses for its own short id list. */
static bool is_g2_ball(uint8_t id) {
  static const uint8_t kBalls[] = {
    0x01, 0x02, 0x04, 0x05, 0x9D, 0x9F, 0xA0, 0xA1, 0xA4, 0xA5, 0xA6, 0xB1,
  };
  for (unsigned i = 0; i < sizeof kBalls / sizeof kBalls[0]; i++)
    if (kBalls[i] == id) return true;
  return false;
}

static bool is_g2_key(uint8_t id) {
  static const uint8_t kKey[] = {
    0x07, 0x36, 0x37, 0x3A, 0x3B, 0x3D, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x73, 0x74, 0x7F, 0x80, 0x81, 0x82, 0x85, 0x86, 0xAF, 0xB2,
  };
  for (unsigned i = 0; i < sizeof kKey / sizeof kKey[0]; i++)
    if (kKey[i] == id) return true;
  return false;
}

int gbb_tmhm_index_of(GbGame game, uint8_t id) {
  if (game != GBF_G_GS && game != GBF_G_CRYSTAL) return -1;
  if (id >= 0xBFu && id <= 0xC2u) return (int)(id - 0xBFu);         /* TM01-04 -> 0-3   */
  if (id >= 0xC4u && id <= 0xDBu) return (int)(id - 0xC4u) + 4;     /* TM05-28 -> 4-27  */
  if (id >= 0xDDu && id <= 0xF2u) return (int)(id - 0xDDu) + 28;    /* TM29-50 -> 28-49 */
  if (id >= 0xF3u && id <= 0xF9u) return (int)(id - 0xF3u) + 50;    /* HM01-07 -> 50-56 */
  return -1;   /* includes the 0xC3/0xDC holes -- real but unused ids */
}

GbBagPocket gbb_pocket_of(GbGame game, uint8_t id) {
  if (id == 0x00u || id == 0xFFu) return GBB_POCKET_COUNT;
  bool gen2 = (game == GBF_G_GS || game == GBF_G_CRYSTAL);
  if (gen2) {
    if (gbb_tmhm_index_of(game, id) >= 0) return GBB_POCKET_TMHM;
    /* Review D2: pokegold's attributes.asm, not pokecrystal's -- ITEM_46/73/
     * 74/81 are unused ITEM-pocket placeholders on Gold/Silver (is_valid_id()'s
     * own header comment has the full derivation); only Crystal actually has
     * CLEAR_BELL/GS_BALL/BLUE_CARD/EGG_TICKET as Key items at these ids. */
    if (game == GBF_G_GS && (id == 0x46u || id == 0x73u || id == 0x74u || id == 0x81u))
      return GBB_POCKET_COUNT;
    if (id > gbb_max_item_id(game)) return GBB_POCKET_COUNT;   /* holes + past HM07 */
    if (is_g2_ball(id)) return GBB_POCKET_BALLS;
    if (is_g2_key(id))  return GBB_POCKET_KEY;
    return GBB_POCKET_ITEMS;
  }
  /* Gen 1: ITEMS vs TM/HM only -- see gb_bag.h's header comment. */
  if (id > gbb_max_item_id(game)) return GBB_POCKET_COUNT;
  if (id >= 0xC4u && id <= 0xFAu) return GBB_POCKET_TMHM;
  return GBB_POCKET_ITEMS;
}
