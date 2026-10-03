#include "xfer_reconcile.h"
#include <string.h>
#include "gen3_clip.h"     /* pk_box_slot, party_count, pk_party_slot                    */
#include "gen3_box.h"      /* G3_TOTAL_BOXES, G3_IN_BOX                                    */
#include "gen3_mon.h"      /* PkMon, pk_decode_mon                                        */
#include "data_tables.h"   /* pk_national_no                                              */
#include "bank_cell.h"     /* bc_is_native, bc_unpack, bc_pack, BcMeta                     */
#include "gb_edit.h"       /* GbEditMon, gb_get_dv/gb_get_otid, GB_ATK/DEF/SPE/SPC          */

/* ---- decision 2: xrc_classify ---------------------------------------------------- */

void xrc_classify(const XrcInput* in, XrcResult* out) {
  if (!in || !out) return;   /* review D6(3): the NULL check must precede the memset */
  memset(out, 0, sizeof *out);
  out->kept = in->bank_keep;

  if (in->kind != XR_KIND_NATIVE_HOME) { out->kind = XRC_G3HOME; return; }

  /* "2+ matches by key or identity" (decision 2's catch-all row) beats every other
   * row -- checked first, regardless of state, so no later branch has to re-derive
   * it. bank_matches >= 2 is the SAME clause on the Bank side (decision 3's own
   * refuse-on-2+ posture, mirrored here). */
  if (in->g3_key_matches >= 2 || in->bank_matches >= 2) { out->kind = XRC_AMBIGUOUS; return; }

  /* Day-Care awareness (decision 15) -- a key match that landed in the Day-Care is
   * informational only, never a release target, regardless of state. */
  if (in->g3_key_matches == 1 && in->g3_in_daycare) { out->kind = XRC_DAYCARE; return; }

  /* excl. 2 (decision 6): the scan never opens a GB save, so an ABROAD_GB row is
   * unconditionally "in a Game Boy save", no action, no further lookups meaningful. */
  if (in->direction == XR_DIR_ABROAD_GB) { out->kind = XRC_ABROAD_GB; return; }

  /* #270: the Gen-3 copy is parked in the Bank (PC -> Bank pass-through), not in the
   * save. It is not lost -- the normal "abroad" state, and never destructive (no
   * RESTORE, which would clone it, and no DELETE, which would lose the way home). */
  if (in->bank_g3_matches >= 1 && in->g3_key_matches == 0) { out->kind = XRC_ABROAD_BANK; return; }

  bool g3_seen = (in->g3_key_matches == 1);
  bool bank_seen = (in->bank_matches == 1);

  /* The stale-key identity pass (decision 15) only fires when the key match failed
   * outright (0, not 2+ -- that was handled above) and this is an ABROAD_G3 entry
   * (ABROAD_GB already returned). */
  if (in->g3_key_matches == 0 && in->g3_identity_matches >= 1) {
    if (in->g3_identity_matches >= 2) { out->kind = XRC_AMBIGUOUS; return; }
    out->kind = XRC_STALE_KEY;
    out->actions = XRC_ACT_REKEY;
    return;
  }

  switch (in->state) {
    case XR_STATE_PENDING:
      /* G-F1: a PENDING entry never drives anything destructive. */
      /* #377: the Gen-3 copy is found exactly once AND the save holding it is the verified one on the card
       * (g3_on_card) -> the exit-time promotion that never happened can be done by hand ("Mark finished":
       * PENDING -> CLAIMED, nothing else changes). Never without g3_on_card: a copy that exists only in RAM is
       * exactly what PENDING protects. */
      if (bank_seen && g3_seen)       { out->kind = XRC_PENDING_BOTH; if (in->g3_on_card) out->actions = XRC_ACT_PROMOTE; }
      else if (bank_seen)             { out->kind = XRC_PENDING_ORPHAN; out->actions = XRC_ACT_DELETE; }
      else if (g3_seen)               { out->kind = XRC_PENDING_NOBANK; if (in->g3_on_card) out->actions = XRC_ACT_PROMOTE; }
      else if (in->bank_ident_matches >= 1) { out->kind = XRC_IN_BANK; out->actions = XRC_ACT_DELETE; }   /* #385 */
      else if (in->bank_unread)       { out->kind = XRC_UNREAD; }   /* #386: could be in a box we could not read */
      else                             { out->kind = XRC_PENDING_LOST; out->actions = XRC_ACT_RESTORE | XRC_ACT_DELETE; }
      return;

    case XR_STATE_CLAIMED:
      /* G-H1: a slot queued for a deferred Bank->PC deletion is never a duplicate,
       * checked BEFORE the dup test so a pending deletion always wins. */
      if (bank_seen && in->bank_slot_pending) { out->kind = XRC_DEFERRED; return; }
      if (bank_seen && g3_seen)               { out->kind = XRC_DUP_BANK; out->actions = XRC_ACT_REMOVE; return; }
      if (!bank_seen && g3_seen)              { out->kind = XRC_ABROAD; return; }
      /* #385: the mon is already in the Bank under a NEW serial (an earlier RESTORE whose ledger rewrite never
       * landed): RESTORE would clone it -- only the record can go. */
      if (!bank_seen && !g3_seen && in->bank_ident_matches >= 1) { out->kind = XRC_IN_BANK; out->actions = XRC_ACT_DELETE; return; }
      if (!bank_seen && !g3_seen && in->bank_unread) { out->kind = XRC_UNREAD; return; }   /* #386 */
      if (!bank_seen && !g3_seen)             { out->kind = XRC_LOST; out->actions = XRC_ACT_RESTORE | XRC_ACT_DELETE; return; }
      /* bank_seen && !g3_seen, not slot_pending: no decision-2 row covers a stray
       * Bank cell with no linked Gen-3 copy at all -- REMOVE requires a g3_seen
       * duplicate (decision 8a), so nothing destructive is offered; treat it the
       * same as the normal end state rather than inventing a new row. */
      out->kind = XRC_ABROAD;
      return;

    case XR_STATE_RESTORED:
      if (bank_seen && g3_seen)   { out->kind = XRC_DUP_G3; out->actions = XRC_ACT_RELEASE; return; }
      if (bank_seen && !g3_seen)  { out->kind = XRC_STALE; out->actions = XRC_ACT_DELETE; return; }
      if (!bank_seen && g3_seen)  { out->kind = XRC_RESTORED_MOVED; return; }
      out->kind = XRC_STALE; out->actions = XRC_ACT_DELETE; return;

    default: /* XR_STATE_NONE on a NATIVE_HOME entry should not occur (state is set at
              * write time, xfer_down_write); do not crash, offer only DELETE. */
      out->kind = XRC_STALE; out->actions = XRC_ACT_DELETE; return;
  }
}

/* ---- decision 3(i): the Gen-3 key match ------------------------------------------ */

static bool rec8_nonzero(const uint8_t* rec) {
  for (int i = 0; i < 8; i++) if (rec[i]) return true;
  return false;
}

/* FNV-1a-64 over the first 8 bytes -- the same constants xr_key_g3()/gbsc_key() use.
 * Duplicated rather than linking xfer_rec.c's xr_key_g3() so this module's own link
 * line (gen3_clip/gen3_mon/data_tables/bank_cell/gb_edit) stays independent of
 * xfer_rec.c's own dependency set (gen3_box, gen3_save, item_map_g2g3). */
static uint64_t rec8_key(const uint8_t* rec) {
  uint64_t h = 14695981039346656037ULL;
  for (int i = 0; i < 8; i++) { h ^= (uint64_t)rec[i]; h *= 1099511628211ULL; }
  return h;
}

int xrc_g3_match_key(const uint8_t* sb1, bool frlg, const uint8_t* pc,
                     uint32_t dc_base, uint32_t dc_stride,
                     uint64_t key, int* where_box, int* where_slot) {
  int count = 0;
  int wb = -1, ws = -1;
  if (sb1) {
    int n = party_count(sb1, frlg);
    for (int i = 0; i < n; i++) {
      const uint8_t* rec = (const uint8_t*)pk_party_slot((uint8_t*)sb1, frlg, i);
      if (!rec || !rec8_nonzero(rec)) continue;
      if (rec8_key(rec) == key) { count++; if (count == 1) { wb = -1; ws = i; } }
      if (count >= 2) break;
    }
  }
  if (count < 2 && pc) {
    for (int box = 0; box < G3_TOTAL_BOXES && count < 2; box++) {
      for (int slot = 0; slot < G3_IN_BOX; slot++) {
        const uint8_t* rec = (const uint8_t*)pk_box_slot((uint8_t*)pc, box, slot);
        if (!rec || !rec8_nonzero(rec)) continue;
        if (rec8_key(rec) == key) { count++; if (count == 1) { wb = box; ws = slot; } }
        if (count >= 2) break;
      }
    }
  }
  if (count < 2 && sb1 && dc_stride) {
    for (int i = 0; i < 2 && count < 2; i++) {
      const uint8_t* rec = sb1 + dc_base + (uint32_t)i * dc_stride;
      if (!rec8_nonzero(rec)) continue;
      if (rec8_key(rec) == key) { count++; if (count == 1) { wb = -2; ws = i; } }
    }
  }
  if (where_box) *where_box = (count == 1) ? wb : -1;
  if (where_slot) *where_slot = (count == 1) ? ws : -1;
  return count > 2 ? 2 : count;
}

/* ---- decision 15: the identity pass ---------------------------------------------- */

static bool identity_matches_rec(const uint8_t* rec, bool is_party,
                                 uint16_t species_written, uint16_t otid16,
                                 const uint8_t nick10[10]) {
  PkMon m;
  if (!pk_decode_mon(rec, is_party, &m)) return false;
  if (pk_national_no(m.species) != species_written) return false;
  if ((uint16_t)(m.otId & 0xFFFFu) != otid16) return false;
  if (memcmp(rec + 0x08, nick10, 10) != 0) return false;
  return true;
}

int xrc_g3_match_identity(const uint8_t* sb1, bool frlg, const uint8_t* pc,
                          uint32_t dc_base, uint32_t dc_stride,
                          uint16_t species_written, uint16_t otid16,
                          const uint8_t nick10[10], int* where_box, int* where_slot) {
  int count = 0;
  int wb = -1, ws = -1;
  if (sb1) {
    int n = party_count(sb1, frlg);
    for (int i = 0; i < n; i++) {
      const uint8_t* rec = (const uint8_t*)pk_party_slot((uint8_t*)sb1, frlg, i);
      if (!rec || !rec8_nonzero(rec)) continue;
      if (identity_matches_rec(rec, true, species_written, otid16, nick10)) {
        count++; if (count == 1) { wb = -1; ws = i; }
      }
      if (count >= 2) break;
    }
  }
  if (count < 2 && pc) {
    for (int box = 0; box < G3_TOTAL_BOXES && count < 2; box++) {
      for (int slot = 0; slot < G3_IN_BOX; slot++) {
        const uint8_t* rec = (const uint8_t*)pk_box_slot((uint8_t*)pc, box, slot);
        if (!rec || !rec8_nonzero(rec)) continue;
        if (identity_matches_rec(rec, false, species_written, otid16, nick10)) {
          count++; if (count == 1) { wb = box; ws = slot; }
        }
        if (count >= 2) break;
      }
    }
  }
  if (count < 2 && sb1 && dc_stride) {
    for (int i = 0; i < 2 && count < 2; i++) {
      const uint8_t* rec = sb1 + dc_base + (uint32_t)i * dc_stride;
      if (!rec8_nonzero(rec)) continue;
      if (identity_matches_rec(rec, false, species_written, otid16, nick10)) {
        count++; if (count == 1) { wb = -2; ws = i; }
      }
    }
  }
  if (where_box) *where_box = (count == 1) ? wb : -1;
  if (where_slot) *where_slot = (count == 1) ? ws : -1;
  return count > 2 ? 2 : count;
}

/* ---- decision 3(ii)/(iii): the Bank match ----------------------------------------- */

int xrc_bank_match(const uint8_t box2400[2400], const GbscEntry* e, bool by_identity,
                   int* slot) {
  if (!box2400 || !e) { if (slot) *slot = -1; return 0; }
  int count = 0, ws = -1;
  for (int s = 0; s < 30 && count < 2; s++) {
    const uint8_t* cell = box2400 + (uint32_t)s * 80;
    if (!bc_is_native(cell)) continue;
    bool hit = false;
    if (!by_identity) {
      hit = memcmp(cell, e->original80, 8) == 0;
    } else {
      GbEditMon mon; BcMeta meta;
      if (bc_unpack(cell, &mon, &meta)) {
        uint8_t dv4[4] = { gb_get_dv(&mon, GB_ATK), gb_get_dv(&mon, GB_DEF),
                          gb_get_dv(&mon, GB_SPE), gb_get_dv(&mon, GB_SPC) };
        hit = mon.gen == e->gen && gb_get_otid(&mon) == e->otid16 &&
              memcmp(dv4, e->dv4, 4) == 0 &&
              memcmp(mon.otname, e->otname_written, GB_NAME_BYTES) == 0;
      }
    }
    if (hit) { count++; if (count == 1) ws = s; }
  }
  if (slot) *slot = (count == 1) ? ws : -1;
  return count > 2 ? 2 : count;
}

int xrc_bank_reserial_match(const uint8_t box2400[2400], const uint8_t orig8[8], const uint8_t orig_serial[4]) {
  if (!box2400 || !orig8 || !orig_serial) return 0;
  uint32_t want = (uint32_t)orig8[4] | ((uint32_t)orig8[5] << 8) | ((uint32_t)orig8[6] << 16) | ((uint32_t)orig8[7] << 24);
  int count = 0;
  for (int s = 0; s < 30 && count < 2; s++) {
    const uint8_t* cell = box2400 + (uint32_t)s * 80;
    if (!bc_is_native(cell)) continue;
    uint8_t t[80]; memcpy(t, cell, 80); memcpy(t + BC_OFF_BANK_SERIAL, orig_serial, 4);
    if (bc_ident32(t) == want) count++;
  }
  return count;
}

int xrc_bank_g3_match(const uint8_t box2400[2400], uint64_t key) {
  if (!box2400) return 0;
  int count = 0;
  for (int s = 0; s < 30 && count < 2; s++) {
    const uint8_t* cell = box2400 + (uint32_t)s * 80;
    if (bc_is_native(cell)) continue;
    bool zero = true;
    for (int i = 0; i < 8; i++) if (cell[i]) { zero = false; break; }
    if (zero) continue;
    if (rec8_key(cell) == key) count++;
  }
  return count;
}

/* ---- decision 8c: RESTORE TO BANK's cell rebuild --------------------------------- */

int xrc_rebuild_cell(const GbscEntry* e, uint32_t serial, uint8_t out80[80]) {
  if (!e || !out80) return -1;
  GbEditMon mon; BcMeta meta;
  if (!bc_unpack(e->original80, &mon, &meta)) return -1;
  return bc_pack(&mon, meta.flags, meta.origin_game, meta.rtc_epoch, serial, out80);
}

/* ---- decision 9: apply ordering ---------------------------------------------------- */

/* #392(b): see xfer_reconcile.h. All-zero orig8 never matches (an uncaptured snapshot is not an identity). */
bool xrc_restore_is_dup(const XrcHit* hits, const bool* done, int i) {
  if (!hits || !done || i <= 0) return false;
  static const uint8_t zero8[8] = {0};
  if (memcmp(hits[i].orig8, zero8, 8) == 0) return false;
  for (int j = 0; j < i; j++)
    if (hits[j].action == XRC_ACT_RESTORE && done[j] && memcmp(hits[j].orig8, hits[i].orig8, 8) == 0) return true;
  return false;
}

void xrc_apply_order(uint8_t* idx, int n) {
  if (!idx || n <= 1) return;
  /* bounded insertion sort, descending; n <= GBSC_MAX_ENTRIES (8), golden rule 2. */
  for (int i = 1; i < n; i++) {
    uint8_t v = idx[i];
    int j = i - 1;
    while (j >= 0 && idx[j] < v) { idx[j + 1] = idx[j]; j--; }
    idx[j + 1] = v;
  }
}

/* ---- decision 14: row text --------------------------------------------------------- */

static const char* xrc_status_word(XrcRowKind kind) {
  switch (kind) {
    case XRC_PENDING_BOTH:    return "unfinished";
    case XRC_PENDING_ORPHAN:  return "never landed";
    case XRC_PENDING_NOBANK:  return "unproven";
    case XRC_PENDING_LOST:    return "record only";
    case XRC_DUP_BANK:        return "in two places";
    case XRC_DEFERRED:        return "moving out";
    case XRC_ABROAD:          return "restorable";
    case XRC_ABROAD_BANK:     return "restorable";
    case XRC_UNREAD:          return "unknown";
    case XRC_LOST:             return "record only";
    case XRC_ABROAD_GB:       return "in a GB save";
    case XRC_DUP_G3:          return "duplicate";
    case XRC_STALE:           return "stale";
    case XRC_RESTORED_MOVED:  return "moved again";
    case XRC_DAYCARE:         return "in Day-Care";
    case XRC_STALE_KEY:       return "not linked";
    case XRC_AMBIGUOUS:       return "ambiguous";
    case XRC_G3HOME:          return "original";
    case XRC_IN_BANK:         return "in the Bank";
    default:                   return "";
  }
}

bool xrc_rekey_should_attempt(const bool* file_rekeyed, int n, uint8_t file_idx) {
  if (!file_rekeyed || file_idx >= n) return true;
  return !file_rekeyed[file_idx];
}

void xrc_rekey_mark_done(bool* file_rekeyed, int n, uint8_t file_idx) {
  if (!file_rekeyed || file_idx >= n) return;
  file_rekeyed[file_idx] = true;
}

int xrc_row_text(XrcRowKind kind, const char* species, const char* game, char out[40]) {
  if (!out) return 0;
  char sp[11]; char gm[8];
  int si = 0; if (species) for (; si < 10 && species[si]; si++) sp[si] = species[si];
  sp[si] = 0;
  int gi = 0; if (game) for (; gi < 7 && game[gi]; gi++) gm[gi] = game[gi];
  gm[gi] = 0;
  const char* status = xrc_status_word(kind);

  int p = 0;
  #define PUT(c) do { if (p < 39) out[p++] = (char)(c); } while (0)
  for (int i = 0; sp[i]; i++) PUT(sp[i]);
  PUT(' '); PUT('f'); PUT('r'); PUT('o'); PUT('m'); PUT(' ');
  for (int i = 0; gm[i]; i++) PUT(gm[i]);
  PUT(' '); PUT(' ');
  for (int i = 0; status[i]; i++) PUT(status[i]);
  #undef PUT
  out[p] = 0;
  return p;
}
