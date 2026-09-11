/* gb_session.c — one editing API over a RESIDENT Game Boy save. See gb_session.h for
 * the design; this file is the two pipelines it promises, kept deliberately apart. */

#include <string.h>

#include "gb_session.h"
#include "gb_edit.h"      /* GB_GEN1 / GB_GEN2, gb_box_valid, gb_list_size */
#include "gen1_write.h"

const char* gbs_status_text(GbsStatus st) {
  switch (st) {
    case GBS_OK:            return "ok";
    case GBS_ERR_ARG:       return "bad argument";
    case GBS_ERR_NOT_GB:    return "not a Game Boy save";
    case GBS_ERR_BOX:       return "no such box";
    case GBS_ERR_UNWRITABLE:return "this box cannot be written";
    case GBS_ERR_STRUCT:    return "the box is not structurally sound";
    case GBS_ERR_ENGINE:    return "the write was refused";
    case GBS_ERR_VERIFY:    return "it did not read back as written";
    case GBS_ERR_FULL:      return "that box is full";
    case GBS_ERR_PARTY_FLOOR: return "the party needs one Pokemon";
    case GBS_ERR_MAIL:      return "a party member is holding Mail";
    case GBS_ERR_NEEDS_BASE:return "Gen 1 needs base stats for that";
    case GBS_ERR_SLOT:      return "no Pokemon in that slot";
    default:                return "?";
  }
}

/* ---- the resident-image callbacks the Gen-2 writer streams through --------
 *
 * These are the whole reason gen2_write.c can be used without a file. Both bound the
 * access with two subtractions rather than `off + len > c->len`, so a range that would
 * wrap 32 bits is refused instead of silently accepted. */

static bool img_rd(void* ctx, uint32_t off, void* buf, uint32_t len) {
  const GbSession* s = (const GbSession*)ctx;
  if (!s || !s->img || !buf) return false;
  if (off > s->len || len > s->len - off) return false;
  memcpy(buf, s->img + off, len);
  return true;
}

static bool img_wr(void* ctx, uint32_t off, const void* buf, uint32_t len) {
  GbSession* s = (GbSession*)ctx;
  if (!s || !s->img || !buf) return false;
  if (off > s->len || len > s->len - off) return false;
  memcpy(s->img + off, buf, len);
  return true;
}

/* ---- open ----------------------------------------------------------------- */

GbsStatus gbs_open(GbSession* s, uint8_t* img, uint32_t len,
                   uint8_t* scratch, uint32_t scratch_len) {
  if (!s || !img || !scratch || scratch_len < GBS_SCRATCH_BYTES) return GBS_ERR_ARG;
  memset(s, 0, sizeof *s);
  s->img = img;
  s->len = len;
  s->scratch = scratch;
  s->scratch_len = scratch_len;

  /* Gen 2 FIRST — see the header. g2w_begin both detects and latches the layout, so a
   * successful begin is the identification AND the writer, in one step. G2_VER_NONE
   * means "accept whatever this file is"; the version is sealed after this call.
   *
   * The writer KEEPS this scratch pointer and streams through it for the rest of the
   * session, which is why it is the caller's and not a local: a stack buffer here would
   * dangle the moment this function returned. */
  if (g2w_begin(&s->g2w, img_rd, img_wr, s, len, scratch, scratch_len,
                G2_VER_NONE) == G2W_OK) {
    s->gen  = GB_GEN2;
    s->open = true;
    return GBS_OK;
  }

  if (gen1_open(img, len, &s->g1) == GEN1_OK) {
    s->gen  = GB_GEN1;
    s->open = true;
    return GBS_OK;
  }
  return GBS_ERR_NOT_GB;
}

int gbs_nboxes(const GbSession* s) {
  if (!s || !s->open) return 0;
  return (s->gen == GB_GEN1) ? GEN1_NUM_BOXES : G2_NUM_BOXES;
}

int gbs_party_box(const GbSession* s) {
  if (!s || !s->open) return -1;
  return (s->gen == GB_GEN1) ? GEN1_PARTY_BOX : G2_BOX_PARTY;
}

bool gb_session_is_crystal(const GbSession* s) {
  if (!s || !s->open || s->gen != GB_GEN2) return false;
  return s->g2w.sv.version == G2_VER_CRYSTAL;
}

/* ---- may this box be written? --------------------------------------------- */

GbsStatus gbs_box_writable(GbSession* s, int box) {
  if (!s || !s->open) return GBS_ERR_ARG;
  if (!gb_box_valid(s->gen, box)) return GBS_ERR_BOX;

  if (s->gen == GB_GEN1) {
    /* gen1_write_targets already refuses a virgin bank (bit 7 of 0x284C clear) and an
     * uninitialised box count, which is exactly the question being asked — so ask it
     * rather than re-deriving the rule and risking a second, disagreeing copy. */
    uint32_t tgt[GEN1_WRITE_MAX_TARGETS];
    int n = 0;
    Gen1WStatus st = gen1_write_targets(s->img, &s->g1, box, tgt, &n);
    if (st == GEN1W_ERR_VIRGIN || st == GEN1W_ERR_UNINIT) return GBS_ERR_UNWRITABLE;
    if (st != GEN1W_OK || n <= 0) return GBS_ERR_ENGINE;
    return GBS_OK;
  }
  /* Gen 2 has no equivalent trap: every box has one home and g2w_begin already refused
   * anything the reader will not support. */
  return GBS_OK;
}

/* ---- load ----------------------------------------------------------------- */

GbsStatus gbs_load_list(GbSession* s, int box, uint8_t* list) {
  if (!s || !s->open || !list) return GBS_ERR_ARG;
  if (!gb_box_valid(s->gen, box)) return GBS_ERR_BOX;

  if (s->gen == GB_GEN2)
    return (g2w_load_list(&s->g2w, box, list) == G2W_OK) ? GBS_OK : GBS_ERR_ENGINE;

  /* Gen 1: the blob lives at one offset the reader already knows. gen1_list_offset
   * resolves the current-box duality on the READ side (0x30C0 for the open box). */
  uint32_t off = gen1_list_offset(&s->g1, box);
  uint32_t n   = gen1_list_bytes(box);
  if (!n || off > s->len || n > s->len - off) return GBS_ERR_BOX;
  memcpy(list, s->img + off, n);
  return GBS_OK;
}

/* ---- commit --------------------------------------------------------------- */

/* Gen 1, composed from gen1_write.h's published primitives because that module has no
 * "commit this blob" entry point (its gen1_write_apply builds the blob itself from a
 * Gen1Op, which is the wrong shape for a slot gb_edit has already patched).
 *
 * The order below is gen1_write_apply's own, minus the blob construction:
 *   1. structural gate on the candidate, BEFORE a byte moves;
 *   2. resolve every destination (this also re-refuses a virgin bank);
 *   3. snapshot the primary destination so step 6 can put it back;
 *   4. no-op short circuit — an unchanged blob must leave the file untouched, checksums
 *      included, or "open a box and back out" would rewrite the save;
 *   5. write the blob to EVERY destination, then fix the checksums the game maintains;
 *   6. verify from the image, and restore everything on failure. */
static GbsStatus gen1_commit(GbSession* s, int box, const uint8_t* list) {
  uint8_t* scratch = s->scratch;
  uint32_t n = gen1_list_bytes(box);
  if (!n || s->scratch_len < n) return GBS_ERR_ARG;

  if (gen1_blob_check(list, box) != GEN1W_OK) return GBS_ERR_STRUCT;   /* 1 */

  uint32_t tgt[GEN1_WRITE_MAX_TARGETS];                                /* 2 */
  int ntgt = 0;
  Gen1WStatus ts = gen1_write_targets(s->img, &s->g1, box, tgt, &ntgt);
  if (ts == GEN1W_ERR_VIRGIN || ts == GEN1W_ERR_UNINIT) return GBS_ERR_UNWRITABLE;
  if (ts != GEN1W_OK || ntgt <= 0) return GBS_ERR_ENGINE;

  for (int i = 0; i < ntgt; i++)
    if (tgt[i] > s->len || n > s->len - tgt[i]) return GBS_ERR_ENGINE;

  memcpy(scratch, s->img + tgt[0], n);                                 /* 3 */

  /* 4. The no-op test is against the PRIMARY destination ALONE, which is what
   * gen1_write_apply's own step 5 compares ("if the candidate is byte-identical to the
   * original, WRITE NOTHING"). Testing every destination instead looks stricter and is
   * actually wrong: for the OPEN box the game deliberately EMPTIES the banked mirror on
   * a box switch (pokered's ChangeBox), so a save the user only looked at has a primary
   * full of Pokemon and a mirror full of nothing. An all-targets comparison never
   * matches there, so every no-op would "repair" the mirror and renormalise the bank
   * checksums -- rewriting a save nobody edited, which is exactly what the zero-byte
   * guarantee exists to forbid. Guy's Yellow.sav caught this: 47 bytes moved on a pure
   * open-and-back-out, all of them box 11's mirror and its bank sums. */
  if (memcmp(s->img + tgt[0], list, n) == 0) return GBS_OK;

  /* 5. Which bank checksums to refresh is read off the DESTINATIONS, exactly as
   * gen1_write_apply's own banks_touched() does, rather than re-derived from the box
   * number -- the two agree today, and a second copy of a rule is how they stop
   * agreeing. The open box writes to 0x30C0 (in neither bank) AND to its bank slot,
   * so only the destinations know the answer.
   *
   * On SUCCESS the corrected sums stay. That is the engine's documented choice, not an
   * accident: the fourteen bank bytes are written by the game and never read back by
   * it, ten of fourteen are wrong in Guy's real Red.sav, and "the state the game itself
   * would leave" is the safest state to leave. So an edit normalises the sums of the
   * bank it touched, and undoing that edit does NOT un-normalise them. */
  bool touched[2] = { false, false };
  for (int i = 0; i < ntgt; i++) {
    if      (tgt[i] >= GEN1_OFF_BANK3) touched[1] = true;
    else if (tgt[i] >= GEN1_OFF_BANK2) touched[0] = true;
  }
  for (int i = 0; i < ntgt; i++) memcpy(s->img + tgt[i], list, n);
  gen1_write_fix_main_checksum(s->img);
  if (touched[0]) gen1_write_fix_bank_checksums(s->img, 0);
  if (touched[1]) gen1_write_fix_bank_checksums(s->img, 1);

  Gen1Save after;                                                      /* 6 */
  if (gen1_open(s->img, s->len, &after) == GEN1_OK &&
      gen1_write_verify_image_box(s->img, s->len, &after, box, list) == GEN1W_OK) {
    s->g1 = after;                    /* the reader's view now matches the bytes */
    return GBS_OK;
  }
  for (int i = 0; i < ntgt; i++) memcpy(s->img + tgt[i], scratch, n);
  gen1_write_fix_main_checksum(s->img);
  if (touched[0]) gen1_write_fix_bank_checksums(s->img, 0);
  if (touched[1]) gen1_write_fix_bank_checksums(s->img, 1);
  return GBS_ERR_VERIFY;
}

GbsStatus gbs_commit_list(GbSession* s, int box, const uint8_t* list) {
  if (!s || !s->open || !list) return GBS_ERR_ARG;
  if (!gb_box_valid(s->gen, box)) return GBS_ERR_BOX;

  if (s->gen == GB_GEN1) return gen1_commit(s, box, list);

  /* Gen 2 needs none of that composition: g2w_commit_list checks the structure, writes
   * every copy the layout demands (both copies for the current box), mirrors anything
   * inside the checksummed span, refreshes both stored checksums, then re-reads and
   * re-parses the WHOLE file and refuses unless it comes back exactly as intended. Its
   * scratch was set at gbs_open and has been its own ever since. */
  /* NOT mapped to GBS_ERR_MAIL: g2w_commit_list raises G2W_ERR_PARTY_MAIL whenever the
   * party's count+species area differs from what is on disk and party_mail_ack is not
   * set -- which fires just as readily for an S2 in-place species/Egg edit (gb_edit_hook
   * never sets the ack) as for an actual S3 restructure. Reporting that as "a party
   * member is holding Mail" would be a FALSE statement about a save with no Mail
   * anywhere in it -- caught on Crystal.sav during review. gbs_delete/gbs_move (below)
   * are the only S3 callers that legitimately mean this refusal, and they detect it
   * themselves via g2_party_has_mail() BEFORE ever touching party_mail_ack or calling
   * this function, so they never depend on this mapping. Falls through to the generic
   * engine refusal instead, as it did before S3.
   *
   * G2W_ERR_FULL / G2W_ERR_EMPTY are not mapped either: g2w_commit_list never returns
   * them (only its own g2w_check_list, write_patch, refresh_checksums and g2w_verify
   * calls contribute a status, and none of those four can produce FULL/EMPTY -- those
   * come only from the slot-level ops g2w_put/insert/append/delete/get, which this
   * function never calls). Mapping a status the callee cannot produce is dead code that
   * looks load-bearing; removed rather than left as false documentation. */
  G2WStatus st = g2w_commit_list(&s->g2w, box, list);
  if (st == G2W_OK)          return GBS_OK;
  if (st == G2W_ERR_STRUCT || st == G2W_ERR_CONTENT) return GBS_ERR_STRUCT;
  if (st == G2W_ERR_VERIFY)  return GBS_ERR_VERIFY;
  return GBS_ERR_ENGINE;
}

/* ============================================================================
 * S3 — list surgery: gbs_delete / gbs_move (docs/GEN12-EDIT-DESIGN.md section 6)
 * ========================================================================== */

/* Map the two engines' own statuses onto GbsStatus, so gbs_delete/gbs_move never have to
 * re-litigate what a refusal means -- the same discipline gbs_box_writable and
 * gbs_commit_list already use. */
static GbsStatus map_gen1w(Gen1WStatus st) {
  switch (st) {
    case GEN1W_OK:          return GBS_OK;
    case GEN1W_ERR_FULL:    return GBS_ERR_FULL;
    case GEN1W_ERR_EMPTY:   return GBS_ERR_SLOT;
    case GEN1W_ERR_VIRGIN:
    case GEN1W_ERR_UNINIT:  return GBS_ERR_UNWRITABLE;
    default:                return GBS_ERR_STRUCT;   /* ARG/SIZE/SAVE/SPECIES/TEXT/STRUCT */
  }
}

static GbsStatus map_g2w(G2WStatus st) {
  switch (st) {
    case G2W_OK:            return GBS_OK;
    case G2W_ERR_FULL:      return GBS_ERR_FULL;
    case G2W_ERR_EMPTY:     return GBS_ERR_SLOT;
    case G2W_ERR_PARTY_MAIL:return GBS_ERR_MAIL;
    default:                return GBS_ERR_STRUCT;   /* ARG/STRUCT/CONTENT/TEXT/STATS/... */
  }
}

/* Gen-2 party floor: would `exclude` leaving (deleted, or moved away) leave the party
 * with no non-Egg member? DELIBERATELY STRICTER than retail: pokecrystal's own deposit
 * gate (bills_pc.asm:1594-1632 BillsPC_CheckMail_PreventBlackout) is a plain party-count
 * check with no species test at all, so retail itself lets a player carry an Egg as
 * their only party member -- this module keeps one non-Egg member instead, so an edited
 * save is never left with nothing usable in battle. (Retail's Egg rule runs the OTHER
 * way -- bills_pc.asm:1634-1650 BillsPC_IsMonAnEgg refuses to RELEASE an Egg at all;
 * gbs_delete() does not reproduce that refusal, so releasing an Egg through S3 stays
 * allowed.) gb_list_count includes Eggs in its count, so they cannot be counted here. */
static bool g2_nonegg_survives(const uint8_t* list, int box, int exclude) {
  int n = gb_list_count(GB_GEN2, list, box);
  if (n < 0) return false;
  for (int i = 0; i < n; i++) {
    if (i == exclude) continue;
    int sp = gb_off_species(GB_GEN2, box, i);
    if (sp >= 0 && list[sp] != G2_LIST_EGG) return true;
  }
  return false;
}

/* See gb_session.h for the citation; declared there (not static) so
 * tests/host_gbsession_test.c checks the SAME predicate this file refuses on, rather
 * than a second hand-copied guess at the id ranges. */
#define G2_MAIL_FLOWER  0x9eu
#define G2_MAIL_LO      0xb5u
#define G2_MAIL_HI      0xbdu
bool gbs_is_mail_item(uint8_t item) {
  return item == G2_MAIL_FLOWER || (item >= G2_MAIL_LO && item <= G2_MAIL_HI);
}

/* Does ANY current member of party box `box` hold Mail? Checked on the list as it stands
 * BEFORE the operation, over every occupied slot -- an append changes the species/count
 * area exactly as a delete does (both trip g2w_commit_list's own party_mail_ack gate), so
 * both directions need the same answer. */
static bool g2_party_has_mail(const uint8_t* list, int box) {
  int n = gb_list_count(GB_GEN2, list, box);
  if (n < 0) return false;
  for (int i = 0; i < n; i++) {
    GbEditMon e;
    if (!gb_load(&e, GB_GEN2, list, box, i)) continue;
    if (gbs_is_mail_item(gb_get_held_item(&e))) return true;
  }
  return false;
}

/* The byte-surgery half of a delete, shared by gbs_delete() and the second half of
 * gbs_move(): apply the engine's own DELETE op to a list already loaded and already past
 * every refusal gbs_delete() would have raised. Does not commit. */
static GbsStatus delete_from_list(GbSession* s, int box, int slot, uint8_t* list) {
  if (s->gen == GB_GEN1) {
    Gen1Op op; memset(&op, 0, sizeof op);
    op.kind = GEN1_OP_DELETE; op.box = box; op.slot = slot; op.mon = NULL;
    return map_gen1w(gen1_blob_apply(list, &op));
  }
  return map_g2w(g2w_delete(list, box, slot));
}

GbsStatus gbs_delete(GbSession* s, int box, int slot, uint8_t* list) {
  if (!s || !s->open || !list) return GBS_ERR_ARG;
  if (!gb_box_valid(s->gen, box)) return GBS_ERR_BOX;

  GbsStatus ld = gbs_load_list(s, box, list);
  if (ld != GBS_OK) return ld;

  int count = gb_list_count(s->gen, list, box);
  if (count < 0) return GBS_ERR_STRUCT;
  if (slot < 0 || slot >= count) return GBS_ERR_SLOT;

  bool need_ack = false;
  if (gb_box_is_party(s->gen, box)) {
    if (s->gen == GB_GEN1) {
      if (count == 1) return GBS_ERR_PARTY_FLOOR;
    } else {
      if (!g2_nonegg_survives(list, box, slot)) return GBS_ERR_PARTY_FLOOR;
      if (g2_party_has_mail(list, box)) return GBS_ERR_MAIL;
      need_ack = true;
    }
  }

  GbsStatus dst = delete_from_list(s, box, slot, list);
  if (dst != GBS_OK) return dst;

  if (need_ack) s->g2w.party_mail_ack = true;
  GbsStatus cst = gbs_commit_list(s, box, list);
  if (need_ack) s->g2w.party_mail_ack = false;   /* never leaks into a later commit */
  return cst;
}

/* ---- the append tail, shared by gbs_move()'s destination half and gbs_insert() -----
 *
 * gbs_move() decides (and may refuse) a box<->party record-kind CONVERSION above this;
 * by the time either of these runs, the record is already shaped for `box` and the only
 * question left is "does the engine's own insert primitive accept it". gbs_insert()
 * skips the conversion entirely (it only ever targets a storage box) and calls straight
 * in. */
static GbsStatus append_gen1(uint8_t* list, int box, const Gen1EditMon* e, int* slot_out) {
  Gen1Op ins; memset(&ins, 0, sizeof ins);
  ins.kind = GEN1_OP_INSERT; ins.box = box; ins.slot = 0; ins.mon = e;
  GbsStatus st = map_gen1w(gen1_blob_apply(list, &ins));
  if (st == GBS_OK) *slot_out = ins.slot;
  return st;
}

static GbsStatus append_gen2(uint8_t* list, int box, const G2Slot* slotv, int* slot_out) {
  return map_g2w(g2w_append(list, box, slotv, slot_out));
}

GbsStatus gbs_move(GbSession* s, int from_box, int from_slot, int to_box, int* to_slot,
                   uint8_t* src_list, uint8_t* dst_list) {
  if (!s || !s->open || !src_list || !dst_list || !to_slot) return GBS_ERR_ARG;
  if (from_box == to_box) return GBS_ERR_ARG;
  if (!gb_box_valid(s->gen, from_box) || !gb_box_valid(s->gen, to_box)) return GBS_ERR_BOX;

  GbsStatus ld = gbs_load_list(s, from_box, src_list);
  if (ld != GBS_OK) return ld;
  GbsStatus ld2 = gbs_load_list(s, to_box, dst_list);
  if (ld2 != GBS_OK) return ld2;

  int scount = gb_list_count(s->gen, src_list, from_box);
  if (scount < 0) return GBS_ERR_STRUCT;
  if (from_slot < 0 || from_slot >= scount) return GBS_ERR_SLOT;

  int dcount = gb_list_count(s->gen, dst_list, to_box);
  if (dcount < 0) return GBS_ERR_STRUCT;
  if (dcount >= gb_list_capacity(s->gen, to_box)) return GBS_ERR_FULL;

  const bool src_party = gb_box_is_party(s->gen, from_box);
  const bool dst_party = gb_box_is_party(s->gen, to_box);
  bool need_ack_src = false, need_ack_dst = false;

  if (src_party) {
    if (s->gen == GB_GEN1) {
      if (scount == 1) return GBS_ERR_PARTY_FLOOR;
    } else {
      if (!g2_nonegg_survives(src_list, from_box, from_slot)) return GBS_ERR_PARTY_FLOOR;
      if (g2_party_has_mail(src_list, from_box)) return GBS_ERR_MAIL;
      need_ack_src = true;
    }
  }
  if (dst_party && s->gen == GB_GEN2) {
    /* An append into the party changes its species/count area exactly as a delete does
     * (g2w_commit_list's own on-disk comparison cannot tell the difference), so the same
     * Mail refusal applies here, checked against the party as it stands before arrival. */
    if (g2_party_has_mail(dst_list, to_box)) return GBS_ERR_MAIL;
    need_ack_dst = true;
  }

  /* ---- record-kind conversion, decided (and refused) before any byte moves --------- */
  if (s->gen == GB_GEN1) {
    Gen1EditMon e;
    if (!gen1_edit_load(src_list, from_box, from_slot, &e)) return GBS_ERR_SLOT;
    if (src_party && !dst_party) {
      /* party -> box: drop the extra 11 bytes and sync the box-level field to the live
       * level, exactly what the game's own deposit does (gen1_write.h g1e_set_level's
       * own comment, and docs/GEN12-EDIT-DESIGN.md section 6). */
      e.is_party = false;
      e.rec[G1R_BOXLEVEL] = e.rec[G1R_LEVEL];
    } else if (!src_party && dst_party) {
      /* box -> party: refused. The party record needs computed stats and this tree
       * carries no Gen-1 base-stat table (gb_edit.h's GbGen1Base) -- nothing moved. */
      return GBS_ERR_NEEDS_BASE;
    }
    int slot_out = 0;
    GbsStatus mst = append_gen1(dst_list, to_box, &e, &slot_out);
    if (mst != GBS_OK) return mst;
    *to_slot = slot_out;
  } else {
    G2Slot slotv;
    GbsStatus gst = map_g2w(g2w_get(src_list, from_box, from_slot, &slotv));
    if (gst != GBS_OK) return gst;

    if (src_party && !dst_party) {
      g2w_slot_to_box(&slotv);
    } else if (!src_party && dst_party) {
      /* box -> party: build a party-kind GbEditMon from the box record (bytes 32..47 --
       * status/unused/curHP/stats -- zeroed, so gb_recalc_stats's own "carry HP across
       * the old/new maximum" rule sees old_max == 0 and lands the mon at full HP, status
       * healthy), recompute its stats (Gen 2's base-stat table is built in), and commit
       * the whole 48-byte record straight back out. Chosen over hand-extracting six
       * stats into g2w_slot_to_party: gb_commit_parts already knows the party record
       * layout, so there is nothing left for this file to get wrong by transcribing it a
       * second time. */
      GbEditMon e;
      uint8_t rec48[GB_MAX_REC];
      memcpy(rec48, slotv.rec, 32);
      memset(rec48 + 32, 0, GB_MAX_REC - 32);
      uint8_t list_sp = slotv.is_egg ? (uint8_t)G2_LIST_EGG : slotv.rec[0];
      if (!gb_load_parts(&e, GB_GEN2, true, rec48, slotv.otname, slotv.nickname, list_sp))
        return GBS_ERR_ENGINE;
      if (!gb_recalc_stats(&e)) return GBS_ERR_ENGINE;
      if (!gb_commit_parts(&e, slotv.rec, slotv.otname, slotv.nickname, &list_sp))
        return GBS_ERR_ENGINE;
      slotv.is_egg   = (list_sp == G2_LIST_EGG);
      slotv.is_party = true;
    }

    int slot_out = 0;
    GbsStatus ast = append_gen2(dst_list, to_box, &slotv, &slot_out);
    if (ast != GBS_OK) return ast;
    *to_slot = slot_out;
  }

  /* ---- destination first, one card write, then the source ------------------------- */
  if (need_ack_dst) s->g2w.party_mail_ack = true;
  GbsStatus cdst = gbs_commit_list(s, to_box, dst_list);
  if (need_ack_dst) s->g2w.party_mail_ack = false;
  if (cdst != GBS_OK) return cdst;             /* nothing else has changed yet */

  GbsStatus ddel = delete_from_list(s, from_box, from_slot, src_list);
  if (ddel != GBS_OK) return ddel;             /* destination already committed -- see the
                                                 * header's atomicity contract: the caller
                                                 * must roll the whole image back now */

  if (need_ack_src) s->g2w.party_mail_ack = true;
  GbsStatus csrc = gbs_commit_list(s, from_box, src_list);
  if (need_ack_src) s->g2w.party_mail_ack = false;
  return csrc;
}

/* ============================================================================
 * Generic field read/write (BACKLOG #49 P0, docs/GEN12-PARITY-DESIGN.md §4.0)
 * ========================================================================== */

GbsStatus gbs_read_field(GbSession* s, uint32_t off, void* buf, uint32_t n) {
  if (!s || !s->open || !buf || !n) return GBS_ERR_ARG;
  if (off > s->len || n > s->len - off) return GBS_ERR_ARG;
  memcpy(buf, s->img + off, n);
  return GBS_OK;
}

GbsStatus gbs_write_field(GbSession* s, uint32_t off, const void* buf, uint32_t n) {
  if (!s || !s->open || !buf || !n) return GBS_ERR_ARG;
  if (s->gen == GB_GEN1)
    /* _ex, not the 64-B gen1_write_range wrapper (P0 review D6): GBF_EVENT_FLAGS_BASE
     * alone is 320 B, past the wrapper's cap. The session's own scratch is the
     * rollback buffer -- REUSED, not newly allocated: gen1_commit (above) already
     * borrows this same s->scratch as its box-list snapshot, and a session never runs
     * a box commit and a field write at the same instant, so the two uses cannot
     * collide. s->scratch_len is >= GBS_SCRATCH_BYTES (1152, gbs_open's own gate),
     * comfortably past every field this design defines. */
    return map_gen1w(gen1_write_range_ex(s->img, s->len, &s->g1, off,
                                        (const uint8_t*)buf, n,
                                        s->scratch, s->scratch_len));
  return map_g2w(g2w_write_range(&s->g2w, off, (const uint8_t*)buf, n));
}

GbsStatus gbs_finish(GbSession* s) {
  if (!s || !s->open) return GBS_ERR_ARG;
  /* Gen 1 has no backup mirror and no deferred checksum: gen1_write_range_ex already
   * fixed the main checksum and re-verified the image on every gbs_write_field call, so
   * there is nothing left to close out. */
  if (s->gen == GB_GEN1) return GBS_OK;
  return map_g2w(g2w_finish(&s->g2w));
}

/* ============================================================================
 * S5-B — insert an already-built BOX record (gen3_to_gb's output), no conversion
 * ========================================================================== */

GbsStatus gbs_insert(GbSession* s, int box, const GbEditMon* mon, int* slot_out,
                     uint8_t* list) {
  if (!s || !s->open || !mon || !slot_out || !list) return GBS_ERR_ARG;
  if (!gb_box_valid(s->gen, box)) return GBS_ERR_BOX;

  GbsStatus wr = gbs_box_writable(s, box);
  if (wr != GBS_OK) return wr;

  /* PARTY IS REFUSED HERE, ON PURPOSE: landing a converted mon in the party belongs to
   * gbs_move() (species-limit / live-stat / Mail rules), not this function -- see
   * gb_session.h. `mon->gen`/`mon->is_party` disagreeing with the destination is folded
   * into the same refusal rather than given its own status, matching gen3_to_gb's own
   * "refuse rather than guess" rule. */
  if (gb_box_is_party(s->gen, box)) return GBS_ERR_ARG;
  if (mon->gen != s->gen || mon->is_party) return GBS_ERR_ARG;

  GbsStatus ld = gbs_load_list(s, box, list);
  if (ld != GBS_OK) return ld;

  int count = gb_list_count(s->gen, list, box);
  if (count < 0) return GBS_ERR_STRUCT;
  if (count >= gb_list_capacity(s->gen, box)) return GBS_ERR_FULL;

  int slot = 0;
  if (s->gen == GB_GEN1) {
    /* Built straight from the GbEditMon's own bytes -- no gen1_edit_load, there is no
     * existing slot to load from. gen3_to_gb() already shaped `mon->rec` as a Gen-1 BOX
     * record (rec_len 33) at the same offsets Gen1EditMon.rec uses (gb_edit.h: "no
     * encryption... the fields are just bytes at fixed offsets", the same layout
     * pokered/macros/ram.asm's box_struct describes), so the bytes carry over as-is. */
    Gen1EditMon e;
    memset(&e, 0, sizeof e);
    e.is_party = false;
    memcpy(e.rec, mon->rec, mon->rec_len);
    memcpy(e.ot,   mon->otname, GB_NAME_BYTES);
    memcpy(e.nick, mon->nick,   GB_NAME_BYTES);
    GbsStatus ist = append_gen1(list, box, &e, &slot);
    if (ist != GBS_OK) return ist;
  } else {
    G2Slot slotv;
    memset(&slotv, 0, sizeof slotv);
    uint8_t list_sp = 0;
    if (!gb_commit_parts(mon, slotv.rec, slotv.otname, slotv.nickname, &list_sp))
      return GBS_ERR_ENGINE;
    slotv.is_egg   = (list_sp == G2_LIST_EGG);
    slotv.is_party = false;
    GbsStatus ist = append_gen2(list, box, &slotv, &slot);
    if (ist != GBS_OK) return ist;
  }

  GbsStatus cst = gbs_commit_list(s, box, list);
  if (cst != GBS_OK) return cst;
  *slot_out = slot;
  return GBS_OK;
}
