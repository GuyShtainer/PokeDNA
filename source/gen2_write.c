/* Generation-II (Gold/Silver/Crystal) save WRITER — pure C, host-testable.
 * The rationale, the two duplication hazards and the caller contract are all in
 * gen2_write.h; this file is the machinery.
 *
 * Offsets and structure come from pret/pokecrystal (reference only, per
 * docs/kb/licensing.md — nothing here is copied from it):
 *   macros/ram.asm            MACRO box / box_struct / party_struct — the five parallel
 *                             pieces of a list and the 2 padding bytes that make the
 *                             banked stride 0x450 instead of 1102
 *   ram/sram.asm + layout.link  where the Save / Backup Save / Active Box / Boxes
 *                             sections land, which is how the 0x2009 / 0x1209 / 0x2D10
 *                             file offsets in gen2_save.c are arrived at
 *   engine/menus/save.asm     Checksum (16-bit byte sum, stored little-endian),
 *                             SaveChecksum / SaveBackupChecksum, and the SaveBox /
 *                             LoadBox pair that makes the current box exist twice
 *   engine/pokemon/move_mon.asm  RemoveMonFromPartyOrBox — the exact compaction this
 *                             file reproduces
 *   engine/menus/naming_screen.asm  NamingScreen_StoreEntry — 0x50 fills every unused
 *                             position of a name field
 * G/S has no decomp to check against, so its layout rests on gen2_save.c's constants,
 * which Guy's real Gold cartridge save validates end to end (both checksums, all five
 * mirror regions byte-exact).
 */
#include "gen2_write.h"
#include <string.h>

/* GB save fields are big-endian; the two stored checksums are the exception. */
static void     wr16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void     wr24be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

/* g2w_begin's seal. Any later call re-derives it from the fields it depends on, so a
 * caller that assigns to w->sv or w->current_box by hand — the only way a Crystal layout
 * could ever be aimed at a G/S save — fails with G2W_ERR_STATE instead of writing. It
 * also catches the on-cart case of something running over the writer in EWRAM. */
#define G2W_SEAL 0x32577200u
static uint32_t seal_of(const G2Writer* w) {
  return G2W_SEAL ^ ((uint32_t)w->sv.version * 2654435761u)
                  ^ ((uint32_t)(w->current_box + 1) * 40503u);
}
static bool sealed(const G2Writer* w) { return w && w->ready == seal_of(w); }

const char* g2w_status_text(G2WStatus st) {
  switch (st) {
    case G2W_OK:              return "ok";
    case G2W_ERR_ARG:         return "bad argument";
    case G2W_ERR_STATE:       return "writer not initialised (or its layout was tampered with)";
    case G2W_ERR_VERSION:     return "the save is a different Gen-2 version than expected";
    case G2W_ERR_UNSUPPORTED: return "this save cannot be written (unsupported or already damaged)";
    case G2W_ERR_IO:          return "the save file could not be read or written";
    case G2W_ERR_STRUCT:      return "the box would be structurally invalid";
    case G2W_ERR_CONTENT:     return "a slot holds a species or level the games do not have";
    case G2W_ERR_TEXT:        return "that text cannot be stored in the Gen-2 character set";
    case G2W_ERR_FULL:        return "no free slot";
    case G2W_ERR_EMPTY:       return "that slot is empty";
    case G2W_ERR_STATS:       return "a party Pokemon needs its stats computed first";
    case G2W_ERR_PARTY_MAIL:  return "changing the party order would misplace held Mail";
    case G2W_ERR_RANGE:       return "that part of the file is not the caller's to write";
    case G2W_ERR_LAYOUT:      return "internal: the backup mirror map is inconsistent";
    case G2W_ERR_VERIFY:      return "the save did not read back as intended — nothing was kept";
    default:                  return "?";
  }
}

/* ============================================================== text encoding */

/* Inverse of gen2_save.c's g2_glyph. Where the decoder maps two codes onto one character
 * (0x80/0xC0 both used to decode to 'A' before BACKLOG #216b gave 0xC0 its own UTF-8
 * spelling "Ä"; 0xE8/0xF2 still both decode to '.') the encoder picks the letter/plain
 * form, so decode(encode(s)) == s while encode(decode(b)) may differ for the remaining
 * decorative codes (0xF2's decimal point has no spelling of its own here). That
 * asymmetry is exactly why every name setter below re-decodes what it just encoded and
 * refuses on a mismatch. The umlauts/é/× are NOT decorative any more: each UTF-8
 * spelling now has exactly one Gen-2 byte, so they round-trip byte-exact, both ways. */
static const struct { const char* s; uint8_t code; } k_multi[] = {
  /* Two-character contraction glyphs first — the games store these as ONE byte, so
   * matching them greedily is what lets a 10-glyph field hold "FARFETCH'D". */
  { "'d", 0xD0 }, { "'l", 0xD1 }, { "'m", 0xD2 }, { "'r", 0xD3 },
  { "'s", 0xD4 }, { "'t", 0xD5 }, { "'v", 0xD6 },
  /* The gender signs, spelled the way g2_decode_text emits them (UTF-8, 3 bytes). */
  { "\xE2\x99\x82", 0xEF }, { "\xE2\x99\x80", 0xF5 },
  /* BACKLOG #216b: e-acute, the umlauts and the times sign, spelled the way
   * g2_decode_text now emits them (source/gen2_save.c) -- each of these 8 UTF-8
   * spellings has exactly ONE Gen-2 byte that decodes to it, so there is no ambiguity
   * to pick a "plain form" for the way 0x80/0xC0 both decoding to 'A' has. */
  { "\xC3\xA9", 0xEA },                                    /* é */
  { "\xC3\x84", 0xC0 }, { "\xC3\x96", 0xC1 }, { "\xC3\x9C", 0xC2 },
  { "\xC3\xA4", 0xC3 }, { "\xC3\xB6", 0xC4 }, { "\xC3\xBC", 0xC5 },
  { "\xC3\x97", 0xF1 },                                    /* U+00D7 ×, not the letter x */
};

/* One glyph. Returns the input bytes consumed (0 = end of string or unrepresentable). */
static int enc_step(const char* s, uint8_t* out) {
  if (!*s) return 0;
  for (unsigned i = 0; i < sizeof k_multi / sizeof k_multi[0]; i++) {
    size_t n = strlen(k_multi[i].s);
    if (strncmp(s, k_multi[i].s, n) == 0) { *out = k_multi[i].code; return (int)n; }
  }
  char c = *s;
  if (c >= 'A' && c <= 'Z') { *out = (uint8_t)(0x80 + (c - 'A')); return 1; }
  if (c >= 'a' && c <= 'z') { *out = (uint8_t)(0xA0 + (c - 'a')); return 1; }
  if (c >= '0' && c <= '9') { *out = (uint8_t)(0xF6 + (c - '0')); return 1; }
  switch (c) {
    case ' ':  *out = 0x7F; return 1;
    case '(':  *out = 0x9A; return 1;
    case ')':  *out = 0x9B; return 1;
    case ':':  *out = 0x9C; return 1;
    case ';':  *out = 0x9D; return 1;
    case '[':  *out = 0x9E; return 1;
    case ']':  *out = 0x9F; return 1;
    case '\'': *out = 0xE0; return 1;
    case '-':  *out = 0xE3; return 1;
    case '?':  *out = 0xE6; return 1;
    case '!':  *out = 0xE7; return 1;
    case '.':  *out = 0xE8; return 1;
    case '&':  *out = 0xE9; return 1;
    case '$':  *out = 0xF0; return 1;
    case '/':  *out = 0xF3; return 1;
    case ',':  *out = 0xF4; return 1;
    default:   return 0;                 /* no Gen-2 glyph for this character */
  }
}

int g2w_text_glyphs(const char* utf8) {
  if (!utf8) return -1;
  int n = 0;
  for (const char* p = utf8; *p; n++) {
    uint8_t b;
    int used = enc_step(p, &b);
    if (used <= 0) return -1;
    p += used;
  }
  return n;
}

int g2w_encode_text(uint8_t* dst, int field_bytes, const char* utf8) {
  if (!dst || !utf8 || field_bytes < 2) return -1;
  int max_glyphs = field_bytes - 1;      /* one byte always left for the terminator */
  int n = 0;
  for (const char* p = utf8; *p; n++) {
    uint8_t b;
    int used = enc_step(p, &b);
    if (used <= 0 || n >= max_glyphs) return -1;
    dst[n] = b;
    p += used;
  }
  /* Fill the WHOLE field, not just one terminator: NamingScreen_StoreEntry writes "@"
   * over every unused position, so this is what the games themselves leave behind, and
   * it guarantees no fragment of the previous name survives past the terminator. */
  for (int i = n; i < field_bytes; i++) dst[i] = 0x50;
  return n;
}

/* Encode, then decode the result back and require it to equal the input: a name that
 * cannot survive the round trip is refused rather than silently stored as something else.
 *
 * Honest note: with the tables as they stand today this can never fire — g2w_encode_text
 * already rejects anything it has no glyph for, and host_gen2write_test.c sweeps all 256
 * codes to prove decode/encode are symmetric. It is here for the edit that adds a glyph
 * to one table and not the other, which is the shape the last two bugs in this area
 * took. Cheap runtime assertion, deliberate. */
static G2WStatus encode_checked(uint8_t* dst, int field_bytes, const char* utf8) {
  uint8_t tmp[16];
  char back[G2_NAME_BYTES];
  if (field_bytes < 2 || field_bytes > (int)sizeof tmp) return G2W_ERR_ARG;
  if (g2w_encode_text(tmp, field_bytes, utf8) < 0) return G2W_ERR_TEXT;
  g2_decode_text(tmp, field_bytes - 1, back, (int)sizeof back);
  if (strcmp(back, utf8) != 0) return G2W_ERR_TEXT;
  memcpy(dst, tmp, (size_t)field_bytes);
  return G2W_OK;
}

/* ============================================================ list geometry */

#define NAME_FIELD 11                    /* NAME_LENGTH / MON_NAME_LENGTH             */
#define BOXNAME_FIELD 9                  /* BOX_NAME_LENGTH                           */

static bool box_ok(int box) {
  return (box >= 0 && box < G2_NUM_BOXES) || box == G2_BOX_PARTY;
}
static int off_species(void) { return 1; }   /* g2_off_species_area() for every list */

/* The one gate every field setter goes through: the list has to be well formed AND the
 * slot has to be occupied before a single byte of a record is addressable. Distinguishing
 * "this list is broken" from "that slot is empty" matters — the first means refuse and
 * shout, the second is just a UI off-by-one. */
static G2WStatus need_slot(uint8_t* list, int box, int slot, uint8_t** rec) {
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  if (slot < 0 || slot >= n) return G2W_ERR_EMPTY;
  *rec = list + g2_off_record(box, slot);
  return G2W_OK;
}

/* ============================================================ structural gate */

static bool name_terminated(const uint8_t* f) {
  for (int i = 0; i < NAME_FIELD; i++) if (f[i] == 0x50) return true;
  return false;
}

/* The PER-SLOT half of the structural gate. The load-bearing thing about it is not what
 * it checks but what it is allowed to look at: the slot's OWN four pieces and nothing
 * else — not its index, not its neighbours, not the count. That is what makes the whole
 * outcome of a list op decidable before the op moves a byte (step (2) of THE PROOF,
 * below), which is how this module gets its all-or-nothing guarantee without a
 * 1102-byte shadow copy the GBA build has no room for.
 *
 * SO: ANY NEW INVARIANT ADDED HERE MUST STAY INDEX-INDEPENDENT. One that relates a slot
 * to its position or to another slot cannot be read off the caller's buffer in advance,
 * and adding one silently turns every list op back into a call that can fail with the
 * box half-reshaped — a Pokemon deleted and its neighbour cloned, from a call that
 * reported failure. If a list-level invariant is ever genuinely needed, the ops need a
 * shadow copy back, and that has to be budgeted for (see gen2_write.h). */
static bool slot_wellformed(const uint8_t* rec, uint8_t listed,
                            const uint8_t* ot, const uint8_t* nick) {
  /* An early terminator inside the occupied range is the nastiest of these: the count
   * byte says N and the list says fewer, and the two halves of the game disagree about
   * how many Pokemon are in the box (RemoveMonFromPartyOrBox walks to the 0xFF, the box
   * menu trusts the count). */
  if (listed == G2_LIST_TERMINATOR) return false;
  /* An occupied slot with no species is a hole in the middle of the list. */
  if (listed == 0 || rec[0x00] == 0) return false;
  /* The record's species byte is the mon's identity; the list byte is the menu's cache
   * of it (0xFD while the mon is an Egg). Letting those drift is how a box ends up
   * showing one Pokemon and containing another. */
  if (listed != G2_LIST_EGG && listed != rec[0x00]) return false;
  /* A name field with no terminator in its 11 bytes makes the game's string routines
   * run into the next Pokemon's name. */
  if (!name_terminated(ot))   return false;
  if (!name_terminated(nick)) return false;
  return true;
}

static bool slot_wellformed_at(const uint8_t* list, int box, int s) {
  return slot_wellformed(list + g2_off_record(box, s), list[off_species() + s],
                         list + g2_off_otname(box, s), list + g2_off_nickname(box, s));
}

G2WStatus g2w_check_list(const uint8_t* list, int box) {
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  /* g2_list_count already enforces the two things a wrong count byte breaks: it is at
   * most the capacity, and the 0xFF terminator sits exactly at [1+count]. */
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;

  for (int s = 0; s < n; s++)
    if (!slot_wellformed_at(list, box, s)) return G2W_ERR_STRUCT;
  return G2W_OK;
}

G2WStatus g2w_check_list_strict(const uint8_t* list, int box) {
  G2WStatus st = g2w_check_list(list, box);
  if (st != G2W_OK) return st;
  int n = g2_list_count(list, box);
  for (int s = 0; s < n; s++) {
    const uint8_t* rec = list + g2_off_record(box, s);
    if (rec[0x00] > G2_SPECIES_MAX) return G2W_ERR_CONTENT;
    if (rec[0x1F] == 0 || rec[0x1F] > 100) return G2W_ERR_CONTENT;   /* stored level */
    /* Last word to the reader itself — if g2_list_mon cannot make sense of the slot,
     * neither will anything downstream of it. */
    G2Mon m;
    if (!g2_list_mon(list, box, s, &m)) return G2W_ERR_CONTENT;
  }
  return G2W_OK;
}

/* ========================================================== slot get/put/move */

G2WStatus g2w_get(const uint8_t* list, int box, int slot, G2Slot* out) {
  if (!out) return G2W_ERR_ARG;
  memset(out, 0, sizeof *out);
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  if (slot < 0 || slot >= n) return G2W_ERR_EMPTY;

  int e = g2_list_entry_size(box);
  memcpy(out->rec,      list + g2_off_record(box, slot),   (size_t)e);
  memcpy(out->otname,   list + g2_off_otname(box, slot),   NAME_FIELD);
  memcpy(out->nickname, list + g2_off_nickname(box, slot), NAME_FIELD);
  out->is_egg   = list[off_species() + slot] == G2_LIST_EGG;
  out->is_party = (box == G2_BOX_PARTY);
  return G2W_OK;
}

/* Everything a placement can be refused for, decided while nothing has been written yet.
 * Every list op calls this BEFORE the first byte moves — it is half of what replaces the
 * shadow copy (step (2) of THE PROOF, below).
 *
 * `resident` distinguishes the two kinds of placement, and the difference is the point:
 *
 *   resident == false — the slot comes from OUTSIDE this list (g2w_put / g2w_insert /
 *     g2w_append). It has to be a species the games have. PokeDNA must never be the thing
 *     that puts a glitch Pokemon into a save that did not already have one.
 *
 *   resident == true — the slot is ALREADY IN THIS LIST and is only being relocated
 *     (g2w_move_slot / g2w_swap_slots). No content gate. gen2_write.h's own reasoning for
 *     g2w_check_list_strict applies verbatim: a box that has been through a GB glitch can
 *     hold species 252 and still be perfectly walkable, and refusing to reorder that box
 *     would be exactly the uselessness that reasoning rejects. Moving bytes that are
 *     already in the list cannot make the save worse than it already is. */
static G2WStatus slot_check(const G2Slot* in, int box, bool resident) {
  if (!in) return G2W_ERR_ARG;
  if (in->is_party != (box == G2_BOX_PARTY)) return G2W_ERR_STATS;
  /* A glitch species is CONTENT, not structure — hence the separate status (the old code
   * called this G2W_ERR_STRUCT, which told the UI the box was broken when it was not).
   * Above the structural test so that "252" reads as content rather than as the 0xFF
   * terminator clash a species byte of 255 would also be. */
  if (!resident && in->rec[0x00] > G2_SPECIES_MAX) return G2W_ERR_CONTENT;
  /* THE SAME PREDICATE THE GATE USES, against the very bytes write_slot() is about to
   * store — including the species byte it derives, derived here identically. That
   * identity is not a convenience; it is what makes "the result will pass the gate"
   * knowable in advance, and it is why a rejected slot never reaches the list. */
  if (!slot_wellformed(in->rec, in->is_egg ? G2_LIST_EGG : in->rec[0x00],
                       in->otname, in->nickname)) return G2W_ERR_STRUCT;
  return G2W_OK;
}

/* Lay a slot's four pieces down at `slot`. Does not touch the count.
 *
 * Deliberately void: every caller has already run slot_check() on `in`, and a check
 * inside the byte-mover is a check that fires when it is too late to honour — the shift
 * has happened, the caller's box has been renumbered, and returning a status cannot undo
 * either. The decision lives above; this only moves bytes. */
static void write_slot(uint8_t* list, int box, int slot, const G2Slot* in) {
  int e = g2_list_entry_size(box);
  memcpy(list + g2_off_record(box, slot),   in->rec,      (size_t)e);
  memcpy(list + g2_off_otname(box, slot),   in->otname,   NAME_FIELD);
  memcpy(list + g2_off_nickname(box, slot), in->nickname, NAME_FIELD);
  list[off_species() + slot] = in->is_egg ? G2_LIST_EGG : in->rec[0x00];
}

/* ---------------------------------------------------------- all-or-nothing plumbing */

/* ============================================================== THE PROOF ==========
 *
 * Every op that MOVES slots decides its ENTIRE outcome before it moves a byte, so once
 * the reshape starts there is no failure path left and nothing to roll back. That is the
 * whole mechanism: no shadow copy, no undo journal, no second 1102-byte buffer.
 *
 * The property being bought is worth restating, because it is not the usual one. A
 * half-applied shift does not leave a BROKEN list — it leaves a different, perfectly
 * valid one with a Pokemon deleted and its neighbour cloned. g2w_check_list passes it,
 * g2w_commit_list writes it, and g2w_verify re-reads it, re-parses it and compares both
 * copies and both checksums and finds nothing to object to, because there is nothing
 * wrong with it. It just is not the box the player had. No gate downstream is capable of
 * noticing, so it has to be impossible here.
 *
 * g2w_check_list demands exactly two things of a finished list:
 *
 *   (L) g2_list_count() >= 0 — the count byte is at most the capacity and the 0xFF
 *       terminator sits exactly at [1+count], hence nowhere earlier, which is the same
 *       as saying no occupied slot's species byte is 0xFF;
 *   (S) every occupied slot satisfies slot_wellformed(), which by construction depends
 *       only on that slot's own four pieces.
 *
 * (1) THE RESHAPES MOVE WHOLE SLOTS. shift_down / shift_up slide the record, OT-name and
 *     nickname arrays and the species list by the same one slot over the same range, so
 *     every slot of the RESULT is either an intact slot of the INPUT at a new index, or
 *     the incoming slot laid down by write_slot(). None is ever assembled from two.
 *
 * (2) THEREFORE (S) IS DECIDABLE IN ADVANCE. Because slot_wellformed() cannot see the
 *     index, "every occupied slot of the result is well formed" is the same statement as
 *     "every input slot that SURVIVES is well formed, and so is the incoming one" — and
 *     both are readable off the caller's untouched buffer. survivors_ok() checks the
 *     first; slot_check() checks the second, against the same bytes and the same derived
 *     species byte write_slot() will store.
 *
 *     Survivors: insert / move / swap keep every occupied slot; delete keeps every one
 *     except the slot being removed, which it passes as `skip`. That exception is the
 *     point, not a loophole — it is what still lets a user delete an entry that is itself
 *     malformed, which is the one thing they actually want from a box a GB glitch got to.
 *
 * (3) AND (L) IS ARITHMETIC. The count is bounds-checked up front (G2W_ERR_FULL /
 *     G2W_ERR_EMPTY), and the terminator is only ever slid: shift_down copies the 0xFF at
 *     [1+n] up to [1+n+1], shift_up copies it down to [1+n-1], and a move does one of
 *     each so it ends where it began. g2_list_count() succeeded on the input, so that
 *     0xFF really was at [1+n]. No occupied result slot can carry 0xFF, because (2)
 *     already required slot_wellformed() — which rejects exactly that — of every survivor
 *     and of the incoming slot.
 *
 * WHAT WOULD BREAK IT. Only two things, and both are guarded where they live: a new
 * invariant in slot_wellformed() that looks outside its own slot (see the comment there),
 * or a new reshape that does not move whole slots. Anything else added to an op is just
 * another up-front refusal, which is always safe.
 *
 * Module A is not comparable here even though it looks it: gen1_write_apply builds in
 * scratch->neu and restores scratch->old, because it edits a RESIDENT 32 KiB image with
 * no untouched original to fall back on. Module B never holds the save — the caller's own
 * staged list is the pre-edit copy, and it is not written until every decision is made. */

/* Every occupied slot except `skip` (-1 for none) — step (2) above. */
static G2WStatus survivors_ok(const uint8_t* list, int box, int n, int skip) {
  for (int s = 0; s < n; s++)
    if (s != skip && !slot_wellformed_at(list, box, s)) return G2W_ERR_STRUCT;
  return G2W_OK;
}

/* Open a hole at `at`: all five structures shift DOWN one slot and the count goes up.
 * `n` is the current count and must be < capacity.
 *
 * This runs on the CALLER'S buffer, and by the time it is called every reason the op
 * could have had to refuse has already been ruled out — see THE PROOF. It moves whole
 * slots (record, OT name, nickname and species byte by the same one place over the same
 * range), which is step (1) of that proof and the reason it holds. */
static void shift_down(uint8_t* b, int box, int at, int n) {
  int cap = g2_list_capacity(box), e = g2_list_entry_size(box);
  /* The species list moves its terminator along with everything else: [1+n] is 0xFF now
   * and has to become [1+n+1]. Slide from the back so the copies do not overlap wrongly. */
  for (int i = n; i >= at; i--) b[off_species() + i + 1] = b[off_species() + i];
  /* The three record-sized arrays shift down by one for the whole REST OF THE ARRAY, not
   * just up to the count — that is what RemoveMonFromPartyOrBox's mirror image does, and
   * it keeps the junk in unused slots looking like the game left it. */
  size_t tail = (size_t)(cap - 1 - at);
  if (tail) {
    memmove(b + g2_off_record(box, at + 1),   b + g2_off_record(box, at),   tail * (size_t)e);
    memmove(b + g2_off_otname(box, at + 1),   b + g2_off_otname(box, at),   tail * NAME_FIELD);
    memmove(b + g2_off_nickname(box, at + 1), b + g2_off_nickname(box, at), tail * NAME_FIELD);
  }
  b[0] = (uint8_t)(n + 1);
}

/* Close the gap at `slot`. This is RemoveMonFromPartyOrBox, byte for byte:
 *  - count -= 1
 *  - the species list is copied down until (and including) the 0xFF, which lands the
 *    terminator at [1+n-1] and leaves the old one at [1+n] as a second 0xFF
 *  - the record / OT / nickname arrays are memmoved down over their whole remaining
 *    length, so the vacated LAST slot keeps a stale duplicate — the game does not scrub
 *    it either, and scrubbing would be a change we were not asked to make.
 * Same contract as shift_down: the caller's buffer, after every refusal is spent. */
static void shift_up(uint8_t* b, int box, int slot, int n) {
  int cap = g2_list_capacity(box), e = g2_list_entry_size(box);
  b[0] = (uint8_t)(n - 1);
  for (int i = slot; i < n; i++) b[off_species() + i] = b[off_species() + i + 1];

  size_t tail = (size_t)(cap - 1 - slot);
  if (tail) {
    memmove(b + g2_off_record(box, slot),   b + g2_off_record(box, slot + 1),   tail * (size_t)e);
    memmove(b + g2_off_otname(box, slot),   b + g2_off_otname(box, slot + 1),   tail * NAME_FIELD);
    memmove(b + g2_off_nickname(box, slot), b + g2_off_nickname(box, slot + 1), tail * NAME_FIELD);
  } else {
    /* Deleting the very last SLOT INDEX of a full list: the game skips the shifts and
     * instead stamps 0xFF over the first byte of that slot's OT name. Nothing reads it
     * (the slot is past the count either way) — reproduced so a PokeDNA-written save is
     * indistinguishable from a game-written one here too. */
    b[g2_off_otname(box, slot)] = 0xFF;
  }
}

/* ------------------------------------------------------------------ the slot ops */

G2WStatus g2w_put(uint8_t* list, int box, int slot, const G2Slot* in) {
  if (!list || !in || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  if (slot < 0 || slot >= n) return G2W_ERR_EMPTY;
  /* No survivors_ok(): a put replaces ONE slot's four pieces and leaves the count, the
   * terminator and every other slot exactly where they were, so it cannot turn a well
   * formed list ill formed except through the incoming slot — and it cannot repair one
   * either. A list that was already broken elsewhere stays exactly as broken, which is
   * what lets a UI rename or re-species one entry of a box a glitch got to. */
  G2WStatus st = slot_check(in, box, false);
  if (st != G2W_OK) return st;
  write_slot(list, box, slot, in);
  return G2W_OK;
}

G2WStatus g2w_insert(uint8_t* list, int box, int at, const G2Slot* in) {
  if (!list || !in || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  if (n >= g2_list_capacity(box)) return G2W_ERR_FULL;
  if (at < 0 || at > n) return G2W_ERR_ARG;
  /* THE PROOF, steps (2) and (3): the incoming slot, then every slot that survives — all
   * of them, since an insert removes nothing. In the original bug the check on `in` lived
   * inside the byte-mover, which ran AFTER the shift on the caller's own buffer, so the
   * "failure" handed back a well-formed list with one Pokemon gone and its neighbour
   * duplicated. */
  G2WStatus st = slot_check(in, box, false);
  if (st != G2W_OK) return st;
  if ((st = survivors_ok(list, box, n, -1)) != G2W_OK) return st;

  shift_down(list, box, at, n);
  write_slot(list, box, at, in);
  return G2W_OK;
}

G2WStatus g2w_append(uint8_t* list, int box, const G2Slot* in, int* slot_out) {
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  /* Atomic because g2w_insert is; *slot_out is only written on success, so a failed
   * append does not leave the caller pointing at a slot that was never created. */
  G2WStatus st = g2w_insert(list, box, n, in);
  if (st == G2W_OK && slot_out) *slot_out = n;
  return st;
}

G2WStatus g2w_delete(uint8_t* list, int box, int slot) {
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  if (slot < 0 || slot >= n) return G2W_ERR_EMPTY;
  /* No slot_check, and `slot` excluded from survivors_ok: deleting is the one thing that
   * must work on an entry that is itself unusable. Everything that stays, though, has to
   * be sound, because after the shift there is no way left to refuse. */
  G2WStatus st = survivors_ok(list, box, n, slot);
  if (st != G2W_OK) return st;

  shift_up(list, box, slot, n);
  return G2W_OK;
}

G2WStatus g2w_move_slot(uint8_t* list, int box, int from, int to) {
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  if (from < 0 || from >= n || to < 0 || to >= n) return G2W_ERR_ARG;
  /* A move removes nothing, so every occupied slot survives it — and this runs BEFORE the
   * from == to short circuit on purpose. "Returned G2W_OK" is meant to be usable by a
   * caller as "this list is well formed", and a no-op that skipped the check would be the
   * single hole in that: it would report success over a box the very next commit refuses. */
  G2WStatus st = survivors_ok(list, box, n, -1);
  if (st != G2W_OK) return st;
  if (from == to) return G2W_OK;

  G2Slot s;
  if ((st = g2w_get(list, box, from, &s)) != G2W_OK) return st;
  /* Resident — see slot_check(). Redundant now that survivors_ok has passed (slot `from`
   * is one of the survivors, and g2w_get sets is_party itself), but it is the statement
   * that the thing being PLACED is judged by the same predicate as any other placement,
   * and it costs four comparisons. The old code got here via delete-then-insert on the
   * caller's own buffer, so a mon the insert would not take had ALREADY been deleted by
   * the time anyone found out: the Pokemon was gone and a neighbour cloned in its place. */
  if ((st = slot_check(&s, box, true)) != G2W_OK) return st;

  /* Still delete-then-insert, so a reorder cannot grow its own way of getting the
   * terminator wrong — and by here neither half has anything left to object to. */
  shift_up(list, box, from, n);
  shift_down(list, box, to, n - 1);
  write_slot(list, box, to, &s);
  return G2W_OK;
}

G2WStatus g2w_swap_slots(uint8_t* list, int box, int a, int b) {
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  int n = g2_list_count(list, box);
  if (n < 0) return G2W_ERR_STRUCT;
  if (a < 0 || a >= n || b < 0 || b >= n) return G2W_ERR_ARG;
  /* Before the a == b short circuit, for the reason given in g2w_move_slot. */
  G2WStatus st = survivors_ok(list, box, n, -1);
  if (st != G2W_OK) return st;
  if (a == b) return G2W_OK;

  G2Slot sa, sb;
  if ((st = g2w_get(list, box, a, &sa)) != G2W_OK) return st;
  if ((st = g2w_get(list, box, b, &sb)) != G2W_OK) return st;
  /* BOTH before either is written. The old code did put(a,sb) and then discovered on
   * put(b,sa) that it could not finish, leaving slot a holding a second copy of slot b's
   * Pokemon and slot a's original nowhere. */
  if ((st = slot_check(&sa, box, true)) != G2W_OK) return st;
  if ((st = slot_check(&sb, box, true)) != G2W_OK) return st;

  write_slot(list, box, a, &sb);
  write_slot(list, box, b, &sa);
  return G2W_OK;
}

void g2w_slot_to_box(G2Slot* s) {
  if (!s) return;
  memset(s->rec + G2_BOX_ENTRY, 0, sizeof s->rec - G2_BOX_ENTRY);
  s->is_party = false;
}

void g2w_slot_to_party(G2Slot* s, const uint16_t stats[6], uint16_t cur_hp, uint8_t status) {
  if (!s || !stats) return;
  s->rec[0x20] = status;
  s->rec[0x21] = 0;                       /* the games' unused byte */
  wr16be(s->rec + 0x22, cur_hp);
  for (int i = 0; i < 6; i++) wr16be(s->rec + 0x24 + i * 2, stats[i]);
  s->is_party = true;
}

/* ============================================================= field setters */

G2WStatus g2w_set_nickname(uint8_t* list, int box, int slot, const char* utf8) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (!utf8) return G2W_ERR_ARG;
  return encode_checked(list + g2_off_nickname(box, slot), NAME_FIELD, utf8);
}

G2WStatus g2w_set_otname(uint8_t* list, int box, int slot, const char* utf8) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (!utf8) return G2W_ERR_ARG;
  return encode_checked(list + g2_off_otname(box, slot), NAME_FIELD, utf8);
}

G2WStatus g2w_set_species(uint8_t* list, int box, int slot, uint8_t species) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (species == 0 || species > G2_SPECIES_MAX) return G2W_ERR_ARG;
  bool egg = list[off_species() + slot] == G2_LIST_EGG;
  r[0x00] = species;
  /* An Egg keeps 0xFD in the list; everything else must track the record byte, or the
   * box menu and the box contents disagree. */
  list[off_species() + slot] = egg ? G2_LIST_EGG : species;
  return G2W_OK;
}

G2WStatus g2w_set_egg(uint8_t* list, int box, int slot, bool egg) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  list[off_species() + slot] = egg ? G2_LIST_EGG : r[0x00];
  return G2W_OK;
}

G2WStatus g2w_set_held_item(uint8_t* list, int box, int slot, uint8_t item) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  r[0x01] = item;
  return G2W_OK;
}

G2WStatus g2w_set_move(uint8_t* list, int box, int slot, int i, uint8_t move,
                       uint8_t pp, uint8_t pp_ups) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  /* Every range this call can refuse for, checked BEFORE the move id is stored — pp and
   * pp_ups included, even though g2w_set_pp checks them again a line later. Delegating
   * the check along with the work meant an out-of-range pp returned G2W_ERR_ARG with the
   * move already changed and the PP byte still describing the OLD move: a Pokemon with
   * the wrong move at the wrong PP, from a call that said it had failed. Every other
   * setter here validates first, and now so does this one. */
  if (i < 0 || i > 3 || pp > 63 || pp_ups > 3) return G2W_ERR_ARG;
  r[0x02 + i] = move;
  return g2w_set_pp(list, box, slot, i, pp, pp_ups);
}

G2WStatus g2w_set_pp(uint8_t* list, int box, int slot, int i, uint8_t pp, uint8_t pp_ups) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (i < 0 || i > 3 || pp > 63 || pp_ups > 3) return G2W_ERR_ARG;
  r[0x17 + i] = (uint8_t)((pp_ups << 6) | pp);   /* bits 6-7 PP Ups, 0-5 current PP */
  return G2W_OK;
}

G2WStatus g2w_set_otid(uint8_t* list, int box, int slot, uint16_t otid) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  wr16be(r + 0x06, otid);
  return G2W_OK;
}

G2WStatus g2w_set_exp(uint8_t* list, int box, int slot, uint32_t exp) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (exp > 0xFFFFFFu) return G2W_ERR_ARG;      /* 24-bit field */
  wr24be(r + 0x08, exp);
  return G2W_OK;
}

G2WStatus g2w_set_friendship(uint8_t* list, int box, int slot, uint8_t f) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  r[0x1B] = f;
  return G2W_OK;
}

G2WStatus g2w_set_pokerus(uint8_t* list, int box, int slot, uint8_t p) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  r[0x1C] = p;
  return G2W_OK;
}

G2WStatus g2w_set_caught(uint8_t* list, int box, int slot, uint8_t time,
                         uint8_t level, uint8_t loc, uint8_t ot_gender) {
  uint8_t* r;
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (time > 3 || level > 63 || loc > 0x7F || ot_gender > 1) return G2W_ERR_ARG;
  r[0x1D] = (uint8_t)((time << 6) | level);
  r[0x1E] = (uint8_t)((ot_gender << 7) | loc);
  return G2W_OK;
}

static void fx_clear(G2WEffects* fx) { if (fx) memset(fx, 0, sizeof *fx); }

G2WStatus g2w_set_level(uint8_t* list, int box, int slot, uint8_t level, G2WEffects* fx) {
  uint8_t* r;
  fx_clear(fx);
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (level == 0 || level > 100) return G2W_ERR_ARG;
  r[0x1F] = level;
  if (fx) {
    /* Gen 2 stores BOTH the level and the EXP, and re-derives the level from the EXP the
     * next time the mon gains any. Moving one without the other makes the Pokemon snap
     * back. This file owns no growth table on purpose, so it says so instead of guessing. */
    fx->exp_stale   = true;
    fx->stats_stale = (box == G2_BOX_PARTY);
  }
  return G2W_OK;
}

G2WStatus g2w_set_statexp(uint8_t* list, int box, int slot, int i, uint16_t v, G2WEffects* fx) {
  uint8_t* r;
  fx_clear(fx);
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (i < 0 || i > 4) return G2W_ERR_ARG;
  wr16be(r + 0x0B + i * 2, v);
  if (fx) fx->stats_stale = (box == G2_BOX_PARTY);
  return G2W_OK;
}

G2WStatus g2w_set_dvs(uint8_t* list, int box, int slot, const uint8_t dv[4],
                      uint8_t gender_ratio, G2WEffects* fx) {
  uint8_t* r;
  fx_clear(fx);
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (!dv) return G2W_ERR_ARG;
  for (int i = 0; i < 4; i++) if (dv[i] > 15) return G2W_ERR_ARG;

  uint8_t old[4] = { (uint8_t)(r[0x15] >> 4), (uint8_t)(r[0x15] & 0x0F),
                     (uint8_t)(r[0x16] >> 4), (uint8_t)(r[0x16] & 0x0F) };
  r[0x15] = (uint8_t)((dv[0] << 4) | dv[1]);   /* Attack, Defense */
  r[0x16] = (uint8_t)((dv[2] << 4) | dv[3]);   /* Speed,  Special */

  if (fx) {
    /* Gender, shininess, the Unown letter and the HP DV are all COMPUTED from these four
     * nibbles — there is no other copy of any of them in the save. So a DV edit is four
     * edits, and the caller has to be told which ones actually moved. */
    fx->gender_before = (uint8_t)g2_gender_from_dv(old[0], gender_ratio);
    fx->gender_after  = (uint8_t)g2_gender_from_dv(dv[0],  gender_ratio);
    fx->shiny_before  = g2_dv_shiny(old);
    fx->shiny_after   = g2_dv_shiny(dv);
    fx->unown_before  = (uint8_t)g2_unown_letter(old);
    fx->unown_after   = (uint8_t)g2_unown_letter(dv);
    fx->hp_dv_before  = g2_hp_dv(old);
    fx->hp_dv_after   = g2_hp_dv(dv);
    fx->is_unown      = (r[0x00] == 201);
    fx->gender_changed = fx->gender_before != fx->gender_after;
    fx->shiny_changed  = fx->shiny_before  != fx->shiny_after;
    fx->unown_changed  = fx->unown_before  != fx->unown_after;
    fx->hp_dv_changed  = fx->hp_dv_before  != fx->hp_dv_after;
    fx->stats_stale    = (box == G2_BOX_PARTY);
  }
  return G2W_OK;
}

G2WStatus g2w_set_dv(uint8_t* list, int box, int slot, int which, uint8_t v,
                     uint8_t gender_ratio, G2WEffects* fx) {
  uint8_t* r;
  fx_clear(fx);
  G2WStatus st = need_slot(list, box, slot, &r);
  if (st != G2W_OK) return st;
  if (which < 0 || which > 3) return G2W_ERR_ARG;
  uint8_t dv[4] = { (uint8_t)(r[0x15] >> 4), (uint8_t)(r[0x15] & 0x0F),
                    (uint8_t)(r[0x16] >> 4), (uint8_t)(r[0x16] & 0x0F) };
  dv[which] = v;
  return g2w_set_dvs(list, box, slot, dv, gender_ratio, fx);
}

G2WStatus g2w_set_party_stats(uint8_t* list, int slot, const uint16_t stats[6],
                              uint16_t cur_hp, uint8_t status) {
  uint8_t* r;
  G2WStatus st = need_slot(list, G2_BOX_PARTY, slot, &r);
  if (st != G2W_OK) return st;
  if (!stats) return G2W_ERR_ARG;
  r[0x20] = status;
  wr16be(r + 0x22, cur_hp);
  for (int i = 0; i < 6; i++) wr16be(r + 0x24 + i * 2, stats[i]);
  return G2W_OK;
}

/* ================================================== mirror + checksum plumbing */

bool g2w_mirror_of(G2Version ver, uint32_t off, uint32_t* mirror_off) {
  const G2MirrorRegion* mr;
  int n = g2_mirror_map(ver, &mr);
  for (int i = 0; i < n; i++)
    if (off >= mr[i].from && off <= mr[i].to) {
      if (mirror_off) *mirror_off = mr[i].dest + (off - mr[i].from);
      return true;
    }
  return false;
}

static bool overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
  return a0 <= b1 && b0 <= a1;            /* both inclusive */
}

/* The mirror map is the single source of truth for BOTH the backup copy and the two
 * checksum spans (the sources sum to checksum 1, the destinations to checksum 2), so a
 * bad map is a bad everything. Checked once, in g2w_begin, before a byte is written. */
static G2WStatus layout_ok(G2Version ver) {
  const G2MirrorRegion* mr;
  int n = g2_mirror_map(ver, &mr);
  if (n <= 0) return G2W_ERR_LAYOUT;
  uint32_t c1 = g2_checksum_primary_off(ver), c2 = g2_checksum_backup_off(ver);
  if (!c1 || !c2) return G2W_ERR_LAYOUT;

  for (int i = 0; i < n; i++) {
    uint32_t len = mr[i].to - mr[i].from + 1;
    if (mr[i].from > mr[i].to) return G2W_ERR_LAYOUT;
    if (mr[i].to >= G2_SAVE_SIZE) return G2W_ERR_LAYOUT;
    if (mr[i].dest + len - 1 >= G2_SAVE_SIZE) return G2W_ERR_LAYOUT;
    /* A region that mirrors onto itself, or onto another region's source, would make the
     * backup copy eat the primary. */
    if (overlap(mr[i].dest, mr[i].dest + len - 1, mr[i].from, mr[i].to)) return G2W_ERR_LAYOUT;
    /* The stored checksums must not sit inside anything that gets summed. */
    if (overlap(c1, c1 + 1, mr[i].from, mr[i].to)) return G2W_ERR_LAYOUT;
    if (overlap(c1, c1 + 1, mr[i].dest, mr[i].dest + len - 1)) return G2W_ERR_LAYOUT;
    if (overlap(c2, c2 + 1, mr[i].from, mr[i].to)) return G2W_ERR_LAYOUT;
    if (overlap(c2, c2 + 1, mr[i].dest, mr[i].dest + len - 1)) return G2W_ERR_LAYOUT;

    for (int j = i + 1; j < n; j++) {
      uint32_t lj = mr[j].to - mr[j].from + 1;
      if (overlap(mr[i].from, mr[i].to, mr[j].from, mr[j].to)) return G2W_ERR_LAYOUT;
      if (overlap(mr[i].dest, mr[i].dest + len - 1, mr[j].dest, mr[j].dest + lj - 1))
        return G2W_ERR_LAYOUT;
      /* Both directions: region i's backup must not land on region j's primary AND
       * region j's must not land on region i's. Checking only one way is how you end up
       * with a map that passes its own audit and still eats the save. */
      if (overlap(mr[i].dest, mr[i].dest + len - 1, mr[j].from, mr[j].to))
        return G2W_ERR_LAYOUT;
      if (overlap(mr[j].dest, mr[j].dest + lj - 1, mr[i].from, mr[i].to))
        return G2W_ERR_LAYOUT;
    }
  }
  return G2W_OK;
}

/* ================================================================= file access */

static bool rd_at(const G2Writer* w, uint32_t off, void* buf, uint32_t len) {
  return w->rd && off + len <= G2_SAVE_SIZE && w->rd(w->ctx, off, buf, len);
}
static bool wr_at(const G2Writer* w, uint32_t off, const void* buf, uint32_t len) {
  return w->wr && off + len <= G2_SAVE_SIZE && w->wr(w->ctx, off, buf, len);
}

/* Read `len` bytes back through the caller's own read path and compare them to what we
 * meant to put there. This is the byte-compare half of the house verified-write pattern,
 * done at range granularity so a partial SD write cannot pass. */
static G2WStatus readback_cmp(G2Writer* w, uint32_t off, const uint8_t* want, uint32_t len) {
  while (len) {
    uint32_t n = len < w->scratch_len ? len : w->scratch_len;
    if (!rd_at(w, off, w->scratch, n)) return G2W_ERR_IO;
    if (memcmp(w->scratch, want, n) != 0) return G2W_ERR_VERIFY;
    off += n; want += n; len -= n;
  }
  return G2W_OK;
}

/* Compare two regions of the FILE against each other, in scratch-sized halves. */
static G2WStatus cmp_regions(G2Writer* w, uint32_t a, uint32_t b, uint32_t len) {
  uint32_t half = w->scratch_len / 2;
  if (half < 16) return G2W_ERR_ARG;
  while (len) {
    uint32_t n = len < half ? len : half;
    if (!rd_at(w, a, w->scratch, n))        return G2W_ERR_IO;
    if (!rd_at(w, b, w->scratch + half, n)) return G2W_ERR_IO;
    if (memcmp(w->scratch, w->scratch + half, n) != 0) return G2W_ERR_VERIFY;
    a += n; b += n; len -= n;
  }
  return G2W_OK;
}

G2WStatus g2w_check_mirror(G2Writer* w, uint32_t off, uint32_t len) {
  if (!sealed(w)) return G2W_ERR_STATE;
  const G2MirrorRegion* mr;
  int n = g2_mirror_map(w->sv.version, &mr);
  for (int i = 0; i < n && len; i++) {
    uint32_t a = off > mr[i].from ? off : mr[i].from;
    uint32_t b = (off + len - 1) < mr[i].to ? (off + len - 1) : mr[i].to;
    if (a > b) continue;
    G2WStatus st = cmp_regions(w, a, mr[i].dest + (a - mr[i].from), b - a + 1);
    if (st != G2W_OK) return st;
  }
  return G2W_OK;
}

G2WStatus g2w_calc_checksums(G2Writer* w, uint16_t* primary, uint16_t* backup) {
  if (!sealed(w)) return G2W_ERR_STATE;
  const G2MirrorRegion* mr;
  int n = g2_mirror_map(w->sv.version, &mr);
  uint32_t sp = 0, sb = 0;
  for (int i = 0; i < n; i++) {
    uint32_t len = mr[i].to - mr[i].from + 1;
    /* Summed spans are DERIVED from the mirror map — the sources are checksum 1's range
     * and the destinations are checksum 2's — so there is exactly one place either can
     * be wrong, and Guy's real saves have already proved that one place. */
    for (uint32_t side = 0; side < 2; side++) {
      uint32_t base = side ? mr[i].dest : mr[i].from, left = len;
      uint32_t acc = 0;
      while (left) {
        uint32_t c = left < w->scratch_len ? left : w->scratch_len;
        if (!rd_at(w, base, w->scratch, c)) return G2W_ERR_IO;
        for (uint32_t k = 0; k < c; k++) acc += w->scratch[k];
        base += c; left -= c;
      }
      if (side) sb += acc; else sp += acc;
    }
  }
  if (primary) *primary = (uint16_t)sp;
  if (backup)  *backup  = (uint16_t)sb;
  return G2W_OK;
}

/* Store both 16-bit sums little-endian (pokecrystal engine/menus/save.asm SaveChecksum
 * writes e then d), then read the four bytes back. */
static G2WStatus refresh_checksums(G2Writer* w) {
  uint16_t p = 0, b = 0;
  G2WStatus st = g2w_calc_checksums(w, &p, &b);
  if (st != G2W_OK) return st;
  uint8_t buf[2];
  uint32_t op = g2_checksum_primary_off(w->sv.version);
  uint32_t ob = g2_checksum_backup_off(w->sv.version);
  buf[0] = (uint8_t)p; buf[1] = (uint8_t)(p >> 8);
  if (!wr_at(w, op, buf, 2)) return G2W_ERR_IO;
  if ((st = readback_cmp(w, op, buf, 2)) != G2W_OK) return st;
  buf[0] = (uint8_t)b; buf[1] = (uint8_t)(b >> 8);
  if (!wr_at(w, ob, buf, 2)) return G2W_ERR_IO;
  return readback_cmp(w, ob, buf, 2);
}

/* ==================================================================== the writer */

G2WStatus g2w_begin(G2Writer* w, G2ReadFn rd, G2WriteFn wr, void* ctx, uint32_t file_len,
                    uint8_t* scratch, uint32_t scratch_len, G2Version expect) {
  if (!w) return G2W_ERR_ARG;
  memset(w, 0, sizeof *w);
  if (!rd || !wr || !scratch || scratch_len < 64) return G2W_ERR_ARG;
  if (file_len < G2_SAVE_SIZE) return G2W_ERR_ARG;
  /* Anything past 32 KiB is the MBC3 RTC footer an emulator appends (44 or 48 bytes;
   * the Virtual Console files carry 16). It is not save data and it is not ours: no
   * write path here can address it, so it survives byte for byte. */
  if (file_len - G2_SAVE_SIZE > G2_MAX_RTC_TAIL) return G2W_ERR_ARG;

  w->rd = rd; w->wr = wr; w->ctx = ctx;
  w->scratch = scratch; w->scratch_len = scratch_len; w->file_len = file_len;

  if (!g2_detect_ranged(rd, ctx, file_len, scratch, scratch_len, &w->sv)) {
    /* Detection swallows a failed read as "short file", so separate the two: at this
     * point the file is long enough, so a short feed means the callback failed. */
    G2WStatus st = w->sv.short_file ? G2W_ERR_IO : G2W_ERR_UNSUPPORTED;
    memset(w, 0, sizeof *w);
    return st;
  }
  if (expect != G2_VER_NONE && w->sv.version != expect) {
    memset(w, 0, sizeof *w);
    return G2W_ERR_VERSION;
  }
  G2WStatus st = layout_ok(w->sv.version);
  if (st != G2W_OK) { memset(w, 0, sizeof *w); return st; }

  /* Which box is open decides where its live copy lives, and the write path needs that
   * before it can keep the two copies in step. */
  G2Offsets o;
  if (!g2_offsets(w->sv.version, &o)) { memset(w, 0, sizeof *w); return G2W_ERR_UNSUPPORTED; }
  uint8_t cur = 0;
  if (!rd(ctx, o.current_box_no, &cur, 1)) { memset(w, 0, sizeof *w); return G2W_ERR_IO; }
  w->current_box = cur & 0x0F;
  if (w->current_box >= G2_NUM_BOXES) {
    /* The game would clamp this to box 0 and quietly move the player's Pokemon around.
     * Refusing is the honest answer for a file we did not create. */
    memset(w, 0, sizeof *w);
    return G2W_ERR_UNSUPPORTED;
  }
  w->ready = seal_of(w);
  return G2W_OK;
}

/* Where box `box` physically lives. Returns how many copies there are (1 or 2) and fills
 * dst[]: dst[0] is always the copy the GAME loads from, dst[1] the live one when the box
 * happens to be the open one. */
static int list_dests(const G2Writer* w, int box, uint32_t dst[2]) {
  if (box == G2_BOX_PARTY) { dst[0] = g2_list_offset(&w->sv, box, -1); return dst[0] ? 1 : 0; }
  if (box < 0 || box >= G2_NUM_BOXES) return 0;
  dst[0] = g2_list_offset(&w->sv, box, -1);          /* the banked copy in SRAM bank 2/3 */
  if (box != w->current_box) return dst[0] ? 1 : 0;
  dst[1] = g2_list_offset(&w->sv, box, box);         /* the live copy in main data       */
  return (dst[0] && dst[1]) ? 2 : 0;
}

G2WStatus g2w_load_list(G2Writer* w, int box, uint8_t* list) {
  if (!sealed(w)) return G2W_ERR_STATE;
  if (!list || !box_ok(box)) return G2W_ERR_ARG;
  /* The live copy for the open box, matching what the reader parses and what the UI
   * therefore showed the user. g2w_current_box_agrees() is how a caller finds out
   * whether that choice is throwing anything away. */
  uint32_t off = g2_list_offset(&w->sv, box, w->current_box);
  if (!off) return G2W_ERR_ARG;
  if (!rd_at(w, off, list, (uint32_t)g2_list_size(box))) return G2W_ERR_IO;
  return G2W_OK;
}

G2WStatus g2w_current_box_agrees(G2Writer* w, bool* agree) {
  if (!sealed(w)) return G2W_ERR_STATE;
  uint32_t d[2];
  if (list_dests(w, w->current_box, d) != 2) return G2W_ERR_ARG;
  G2WStatus st = cmp_regions(w, d[0], d[1], (uint32_t)G2_BOX_LIST_SIZE);
  if (st == G2W_ERR_VERIFY) { if (agree) *agree = false; return G2W_OK; }
  if (st != G2W_OK) return st;
  if (agree) *agree = true;
  return G2W_OK;
}

/* The one place bytes actually reach the file: write, mirror anything inside the
 * checksummed span in the same breath, then read all of it back and compare. Used by the
 * public patch entry point AND by g2w_commit_list, so a list and a header field get
 * exactly the same treatment. The two things it will not write are the ones it owns:
 * the stored checksums, and any byte of the backup copy (which is reached only as the
 * mirror of a primary write, never addressed directly). */
static G2WStatus write_patch(G2Writer* w, uint32_t off, const uint8_t* buf, uint32_t len) {
  if (!buf || !len) return G2W_ERR_ARG;
  if (off + len > G2_SAVE_SIZE || off + len < off) return G2W_ERR_RANGE;

  const G2MirrorRegion* mr;
  int nr = g2_mirror_map(w->sv.version, &mr);
  uint32_t last = off + len - 1;

  uint32_t c1 = g2_checksum_primary_off(w->sv.version);
  uint32_t c2 = g2_checksum_backup_off(w->sv.version);
  if (overlap(off, last, c1, c1 + 1) || overlap(off, last, c2, c2 + 1)) return G2W_ERR_RANGE;
  for (int i = 0; i < nr; i++) {
    uint32_t dl = mr[i].to - mr[i].from;
    if (overlap(off, last, mr[i].dest, mr[i].dest + dl)) return G2W_ERR_RANGE;
  }

  if (!wr_at(w, off, buf, len)) return G2W_ERR_IO;

  /* Anything inside the checksummed span goes to its mirror immediately — that is the
   * whole reason a Gen-2 write is harder than a Gen-3 one. Bytes outside the span (the
   * boxes, the live current-box copy) have no mirror and need none. */
  for (int i = 0; i < nr; i++) {
    uint32_t a = off > mr[i].from ? off : mr[i].from;
    uint32_t b = last < mr[i].to ? last : mr[i].to;
    if (a > b) continue;
    const uint8_t* src = buf + (a - off);
    uint32_t dest = mr[i].dest + (a - mr[i].from), n = b - a + 1;
    if (!wr_at(w, dest, src, n)) return G2W_ERR_IO;
    G2WStatus st = readback_cmp(w, dest, src, n);
    if (st != G2W_OK) return st;
  }
  return readback_cmp(w, off, buf, len);
}

G2WStatus g2w_write_range(G2Writer* w, uint32_t off, const uint8_t* buf, uint32_t len) {
  if (!sealed(w)) return G2W_ERR_STATE;
  if (!buf || !len) return G2W_ERR_ARG;
  if (off + len > G2_SAVE_SIZE || off + len < off) return G2W_ERR_RANGE;
  uint32_t last = off + len - 1;

  /* This is the generic HEADER patch — money, badges, the dex flags, a name. It must not
   * reach a Pokemon list, because that would be a way to change Pokemon without passing
   * the structural gate, which is the one thing this module exists to make impossible.
   * Pokemon go through g2w_commit_list. */
  G2Offsets o;
  if (!g2_offsets(w->sv.version, &o)) return G2W_ERR_UNSUPPORTED;
  if (overlap(off, last, o.party_list, o.party_list + G2_PARTY_LIST_SIZE - 1))
    return G2W_ERR_RANGE;
  if (overlap(off, last, o.current_box_list, o.current_box_list + G2_BOX_LIST_SIZE - 1))
    return G2W_ERR_RANGE;
  for (int b = 0; b < G2_NUM_BOXES; b++) {
    uint32_t bo = g2_list_offset(&w->sv, b, -1);
    if (bo && overlap(off, last, bo, bo + G2_BOX_LIST_SIZE - 1)) return G2W_ERR_RANGE;
  }
  /* Nor the byte that says which box is open. Changing it alone silently throws a box
   * away: at the next boot the game copies sBox{new} over the live copy, so the Pokemon
   * sitting in the live copy — the ones the UI was just showing — are gone (pokecrystal
   * engine/menus/save.asm, TryLoadSaveFile -> LoadBox). Moving the open box means moving
   * the live copy with it, which is an operation, not a byte poke. */
  if (o.current_box_no && overlap(off, last, o.current_box_no, o.current_box_no))
    return G2W_ERR_RANGE;

  return write_patch(w, off, buf, len);
}

G2WStatus g2w_verify(G2Writer* w, int box, const uint8_t* expect) {
  if (!sealed(w)) return G2W_ERR_STATE;

  /* Hand the finished file back to the READER and see whether it still likes it. Same
   * code path the UI used to open the save, so "it parses" means the same thing here as
   * it did there. Both copies must validate: primary_ok because that is what the game
   * checks first, backup_ok because a stale mirror is a save that resurrects old data
   * the day the primary gets torn. */
  G2Save sv2;
  if (!g2_detect_ranged(w->rd, w->ctx, w->file_len, w->scratch, w->scratch_len, &sv2))
    return G2W_ERR_VERIFY;
  if (sv2.version != w->sv.version || !sv2.primary_ok || !sv2.backup_ok)
    return G2W_ERR_VERIFY;

  if (!expect || !box_ok(box)) return G2W_OK;

  /* Every copy of the list must be byte-identical to what was asked for — including the
   * banked one, which is the copy the game actually loads. */
  uint32_t dst[2];
  int nd = list_dests(w, box, dst);
  if (nd < 1) return G2W_ERR_ARG;
  uint32_t size = (uint32_t)g2_list_size(box);
  for (int i = 0; i < nd; i++) {
    G2WStatus st = readback_cmp(w, dst[i], expect, size);
    if (st != G2W_OK) return st;
    st = g2w_check_mirror(w, dst[i], size);      /* no-op outside the checksummed span */
    if (st != G2W_OK) return st;
  }

  /* Re-parse. `expect` has just been proved byte-identical to every copy on disk, so
   * parsing it is parsing the file — without needing 1102 bytes of scratch to stage a
   * second copy on a machine that has 1.5 KB of EWRAM to spare. */
  if (g2w_check_list(expect, box) != G2W_OK) return G2W_ERR_VERIFY;
  return G2W_OK;
}

G2WStatus g2w_finish(G2Writer* w) {
  if (!sealed(w)) return G2W_ERR_STATE;
  G2WStatus st = refresh_checksums(w);
  if (st != G2W_OK) return st;
  return g2w_verify(w, -1, 0);
}

G2WStatus g2w_commit_list(G2Writer* w, int box, const uint8_t* list) {
  if (!sealed(w)) return G2W_ERR_STATE;
  if (!list || !box_ok(box)) return G2W_ERR_ARG;

  G2WStatus st = g2w_check_list(list, box);
  if (st != G2W_OK) return st;

  uint32_t dst[2];
  int nd = list_dests(w, box, dst);
  if (nd < 1) return G2W_ERR_ARG;
  uint32_t size = (uint32_t)g2_list_size(box);

  if (box == G2_BOX_PARTY && !w->party_mail_ack) {
    /* Held Mail lives in a parallel six-entry array in SRAM bank 0 that is indexed by
     * PARTY SLOT, and the games shift it in step with the party (pokecrystal
     * RemoveMonFromPartyOrBox, ".Shift our mail messages up"). We do not know where that
     * array is in G/S, so a party whose slot layout changed is refused instead of
     * silently detaching someone's Mail. Comparing the count+species area catches every
     * insert, delete and reorder except swapping two slots holding the same species —
     * a caller that reorders must set party_mail_ack regardless, which the header says. */
    uint8_t on_disk[1 + G2_BOX_CAPACITY + 1];
    uint32_t n = (uint32_t)(1 + g2_list_capacity(box) + 1);
    if (!rd_at(w, dst[0], on_disk, n)) return G2W_ERR_IO;
    if (memcmp(on_disk, list, n) != 0) return G2W_ERR_PARTY_MAIL;
  }

  for (int i = 0; i < nd; i++) {
    st = write_patch(w, dst[i], list, size);
    if (st != G2W_OK) return st;
  }
  st = refresh_checksums(w);
  if (st != G2W_OK) return st;
  return g2w_verify(w, box, list);
}

G2WStatus g2w_set_box_name(G2Writer* w, int box, const char* utf8) {
  if (!sealed(w)) return G2W_ERR_STATE;
  if (box < 0 || box >= G2_NUM_BOXES || !utf8) return G2W_ERR_ARG;
  G2Offsets o;
  if (!g2_offsets(w->sv.version, &o) || !o.box_names) return G2W_ERR_UNSUPPORTED;

  uint8_t field[BOXNAME_FIELD];
  G2WStatus st = encode_checked(field, BOXNAME_FIELD, utf8);
  if (st != G2W_OK) return st;
  st = write_patch(w, o.box_names + (uint32_t)box * BOXNAME_FIELD, field, BOXNAME_FIELD);
  if (st != G2W_OK) return st;
  st = refresh_checksums(w);
  if (st != G2W_OK) return st;
  return g2w_verify(w, -1, 0);
}
