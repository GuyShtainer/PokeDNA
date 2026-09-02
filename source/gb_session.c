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
  G2WStatus st = g2w_commit_list(&s->g2w, box, list);
  if (st == G2W_OK)          return GBS_OK;
  if (st == G2W_ERR_STRUCT || st == G2W_ERR_CONTENT) return GBS_ERR_STRUCT;
  if (st == G2W_ERR_VERIFY)  return GBS_ERR_VERIFY;
  return GBS_ERR_ENGINE;
}
