#ifndef GEN2_WRITE_H
#define GEN2_WRITE_H

#include <stdint.h>
#include <stdbool.h>
#include "gen2_save.h"

/* Generation-II (Gold/Silver/Crystal) save WRITER — pure C, host-testable.
 *
 * gen2_save.c reads; this file is the only thing in PokeDNA allowed to change a Gen-2
 * save's bytes. It is pure C for the same reason the reader is: <stdint.h>/<string.h>
 * and nothing else, so tests/host_gen2write_test.c compiles and runs it on the PC.
 * No statics, no allocation — every buffer bigger than a stack frame is the caller's,
 * because the GBA build has ~1.5 KB of free EWRAM and cannot hold a 32 KiB save.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE IS PARANOID
 * ---------------------------------------------------------------------------
 * A Gen-2 box is not an array of records. It is FIVE things that have to agree:
 *
 *     +0            count byte
 *     +1            capacity+1 species bytes, 0xFF after the last real one
 *     +2+C          C records   (32 bytes boxed / 48 in the party)
 *     +2+C+C*E      C OT names,   11 bytes each
 *     +2+C+C*E+11C  C nicknames,  11 bytes each
 *
 * (pret/pokecrystal macros/ram.asm, MACRO box — the trailing `ds 2 ; padding` is why
 * the stride between banked boxes is 0x450 while the list itself is 1102 bytes.)
 *
 * Editing a nickname touches a different region than the record. Deleting one means
 * compacting four arrays and moving the terminator. A count byte that is one too large
 * does not corrupt one Pokemon: the game walks off the end of the record array and
 * reads the OT-name array as Pokemon data. So slots are moved as a whole G2Slot, never
 * as four independent memcpys at a call site, and every list is re-checked before it is
 * allowed anywhere near the file.
 *
 * On top of that, two duplication hazards that silently eat edits:
 *
 *  1. THE CURRENT BOX EXISTS TWICE. The open box has a live copy in main data
 *     (0x2D6C in G/S, 0x2D10 in Crystal) and a banked copy at its normal slot in SRAM
 *     bank 2/3. pokecrystal engine/menus/save.asm: _SaveGameData calls SaveBox (live ->
 *     banked) before checksumming, and TryLoadSaveFile calls LoadBox (banked -> live) on
 *     load. So in a saved FILE both copies are current and identical (confirmed on Guy's
 *     real Gold and Crystal saves), and — this is the part that bites — the copy the game
 *     reads at boot is the BANKED one. Writing only the live copy loses the edit at the
 *     next load. g2w_commit_list() writes BOTH and verifies BOTH.
 *
 *  2. THE PLAYER BLOCK EXISTS TWICE. Everything in the checksummed span is mirrored to
 *     a backup copy — one contiguous region in Crystal, five scattered ones in G/S — and
 *     the game boots from the backup if the primary checksum fails. Every byte this file
 *     writes inside that span is written to its mirror in the same call, and both stored
 *     checksums are recomputed. The mirror map is g2_mirror_map()'s, not a second
 *     transcription: region 2's destination is 0x3D69, and the 0x3D96 in the wikis is a
 *     transposed digit that agreed with our own fixture and was only caught by Guy's real
 *     Gold cartridge save.
 *
 * ---------------------------------------------------------------------------
 * THE SAFETY PROPERTY
 * ---------------------------------------------------------------------------
 * Nothing is ever "written and hoped for". After laying bytes down, every commit
 *   - re-reads them and byte-compares against what was asked for,
 *   - re-runs the READER over the result (g2_detect_ranged + g2_list_count +
 *     g2_list_mon) and requires the same version, primary_ok AND backup_ok, and the
 *     list decoding back exactly as intended,
 *   - compares the primary bytes against their mirror directly, not via the checksum.
 * If any of that fails the call returns G2W_ERR_VERIFY. Refusing is always allowed;
 * silently writing something we did not prove is not.
 *
 * CALLER CONTRACT — this is the half of the safety pipeline gen2_write does NOT do:
 * point `wr` at a WORKING COPY, never at the user's save. The house pattern
 * (docs/kb/safety-pipeline.md, source/savefile.c) is: immutable backup -> copy the save
 * to a .tmp -> run this file against the .tmp -> only on G2W_OK rename it over the
 * original. gen2_write cannot roll back, so "refuse the rename" IS the rollback.
 * `rd` and `wr` must address the SAME file.
 *
 * WESTERN G/S AND CRYSTAL ONLY, and only if the reader already accepts the save.
 * Japanese layouts are detected and refused, exactly as in the reader.
 */

/* ------------------------------------------------------------------ status */

typedef enum {
  G2W_OK = 0,
  G2W_ERR_ARG,          /* NULL or out-of-range argument                            */
  G2W_ERR_STATE,        /* writer not initialised by g2w_begin, or since tampered    */
  G2W_ERR_VERSION,      /* the save is not the version the caller said it was        */
  G2W_ERR_UNSUPPORTED,  /* Japanese / unparseable / primary checksum already bad     */
  G2W_ERR_IO,           /* the caller's read or write callback failed                */
  G2W_ERR_STRUCT,       /* the list is not structurally sound — never gets written   */
  G2W_ERR_CONTENT,      /* structure fine, but a slot holds a glitch species/level   */
  G2W_ERR_TEXT,         /* the text does not survive encode->decode (or will not fit)*/
  G2W_ERR_FULL,         /* no free slot in the list                                  */
  G2W_ERR_EMPTY,        /* the slot is past the count                                */
  G2W_ERR_STATS,        /* a party record needs stats the caller has to compute      */
  G2W_ERR_PARTY_MAIL,   /* party restructure without acknowledging the mail array     */
  G2W_ERR_RANGE,        /* the write would land on bytes this file owns              */
  G2W_ERR_LAYOUT,       /* the mirror map is self-inconsistent (a code bug)          */
  G2W_ERR_VERIFY        /* the written bytes did not read back as intended           */
} G2WStatus;

const char* g2w_status_text(G2WStatus st);

/* ------------------------------------------------- consequences of an edit */

/* Gen 2 stores four DVs and DERIVES gender, shininess, the Unown letter and the HP DV
 * from them — there is no gender byte, no shiny flag and no form byte anywhere in the
 * save. So "set the Attack DV" is never just that, and a UI that does not say so is
 * lying to the player. Every setter that can move a derived property fills one of these
 * and the caller is expected to show it. */
typedef struct {
  bool    gender_changed, shiny_changed, unown_changed, hp_dv_changed;
  uint8_t gender_before, gender_after;   /* 0 male, 1 female, 2 genderless           */
  bool    shiny_before,  shiny_after;
  uint8_t unown_before,  unown_after;    /* 0..25 = A..Z                             */
  uint8_t hp_dv_before,  hp_dv_after;    /* changes the mon's max HP                 */
  bool    is_unown;                      /* the letter only means anything then      */
  bool    exp_stale;                     /* level moved; EXP now disagrees with it   */
  bool    stats_stale;                   /* stored party stats no longer match       */
} G2WEffects;

/* ------------------------------------------------------ one Pokemon, whole */

/* A slot's FOUR pieces as one movable object. Existing only so that nothing in PokeDNA
 * ever copies a record without its OT name, its nickname and its egg marker. */
typedef struct {
  uint8_t rec[G2_PARTY_ENTRY];      /* 48; a boxed mon only uses the first 32        */
  uint8_t otname[11], nickname[11]; /* raw Gen-2 charset, 0x50-terminated            */
  bool    is_egg;                   /* the species-list marker, not a record field   */
  bool    is_party;                 /* which record size rec[] holds                 */
} G2Slot;

/* ---------------------------------------------------------------- the text */

/* Gen-2 charset encoder — the inverse of g2_decode_text. Writes `field_bytes` bytes at
 * `dst`: the glyphs, then 0x50 padding to the end of the field. Returns the number of
 * GLYPHS written, or -1 if `utf8` contains something the charset cannot represent or
 * needs more than `field_bytes - 1` glyphs.
 *
 * Padding the whole field (rather than writing one terminator and leaving the tail) is
 * what the naming screen does for the two kinds of name a player can actually type:
 * NamingScreen_StoreEntry replaces every unused position with "@" (0x50)
 * (pokecrystal engine/menus/naming_screen.asm), and Guy's real saves show nicknames
 * fully 0x50-padded. It also means renaming never leaves a fragment of the old name in
 * the buffer. Consequence to know: setting a name to the value it already decodes to can
 * still change bytes past the terminator, so a name write is not guaranteed byte-identical
 * the way every other no-op edit here is.
 *
 * "'d", "'l", "'m", "'r", "'s", "'t" and "'v" are matched greedily as the single
 * contraction glyphs the games use; the male/female signs are accepted as the same UTF-8
 * spelling g2_decode_text emits. */
int g2w_encode_text(uint8_t* dst, int field_bytes, const char* utf8);

/* Glyphs `utf8` would need, or -1 if it cannot be represented. Lets a UI check a name
 * against G2_NAME_CHARS / G2_BOXNAME_CHARS before offering to save it. */
int g2w_text_glyphs(const char* utf8);

/* ------------------------------------------------ list surgery (pure, no I/O) */
/* All of these operate on ONE staged list blob of g2_list_size(box) bytes — the same
 * buffer g2_list_* reads. They never touch a file. `box` is 0..13 or G2_BOX_PARTY.
 *
 * ---------------------------------------------------------------------------
 * ALL-OR-NOTHING, AND WHY IT HAS TO BE
 * ---------------------------------------------------------------------------
 * Every operation here either returns G2W_OK having done the whole thing, or returns a
 * failure having left `list` BYTE-IDENTICAL to what the caller passed in. There is no
 * middle state, ever.
 *
 * That is not general tidiness, it is the one hazard this module cannot verify its way
 * out of. An operation that shifts the five parallel structures and then fails partway
 * does not leave a BROKEN list — it leaves a different, perfectly VALID list with one
 * Pokemon deleted and its neighbour cloned. g2w_check_list passes it. g2w_commit_list
 * writes it. g2w_verify re-reads it, re-parses it, compares both copies and both
 * checksums, and finds nothing wrong, because nothing IS wrong with it: it is simply not
 * the box the player had. Every gate downstream is constitutionally incapable of noticing,
 * so the property has to hold up here or not at all.
 *
 * HOW, AND WHY THERE IS NO SECOND BUFFER
 * --------------------------------------
 * By deciding everything up front. Each op rules out every reason it could refuse — the
 * incoming slot, every slot that will survive the reshape, the count and the capacity —
 * while the caller's list is still untouched, and only then shifts. Once bytes move there
 * is no failure path left, so there is nothing to roll back and no shadow copy to roll
 * back from. gen2_write.c's THE PROOF is the argument in full; the load-bearing step is
 * that the structural gate's per-slot test depends only on that slot's own four pieces,
 * so "the result will pass" is a question about the INPUT bytes.
 *
 * The RAM this costs a caller is therefore zero beyond the one staged list of
 * g2_list_size(box) bytes it already has to have: 1102 for a box, 428 for the party. An
 * earlier draft did take a second 1102-byte scratch to build the candidate in; on a build
 * with ~1.5 KB of free EWRAM, 2204 bytes per edit is not a thing that can be wired in.
 * The only other cost is stack, and it is a frame, not a buffer: measured on devkitARM
 * -O2 thumb, 24-40 bytes for delete/insert/append, 104 for a move (one G2Slot) and 176
 * for a swap (two), so under 200 bytes of the 32 KiB IWRAM stack at the deepest.
 * (If a future invariant ever genuinely needs the whole finished list to judge, the
 * shadow has to come back AND find 1102 bytes — realistically from app_arena_acquire(),
 * never from the IWRAM stack, which is 32 KiB and holds the stack.) */

/* THE STRUCTURAL GATE — the invariants that decide whether the games can walk the list
 * at all, and the only thing a write is ever refused for:
 *   count <= capacity; the 0xFF terminator exactly at [1+count] and nowhere before it;
 *   every occupied slot has a non-zero species; the record's species byte agrees with
 *   the species-list byte (or the list byte is 0xFD for an Egg); both name fields carry
 *   a 0x50 terminator inside their 11 bytes.
 * Break any of these and the damage is not to one Pokemon — the game reads the OT-name
 * array as Pokemon data, or two halves of the game disagree about how many are in the box. */
G2WStatus g2w_check_list(const uint8_t* list, int box);

/* The above PLUS content plausibility: species 1..251, level 1..100, every slot decoding
 * through g2_list_mon. Returns G2W_ERR_CONTENT for a violation.
 *
 * Deliberately NOT what a commit is gated on. A save that has been through a GB glitch
 * can hold a slot with species 252 while the list itself is perfectly walkable, and
 * refusing that would mean PokeDNA could not rename a Pokemon in that box — nor delete
 * the glitch entry, which is the one thing the user actually wants. So this is what a UI
 * warns with, and g2w_check_list is what a write is refused for. */
G2WStatus g2w_check_list_strict(const uint8_t* list, int box);

/* Read/replace a whole slot. g2w_put overwrites an occupied slot; g2w_insert shifts the
 * slots at and after `at` down and takes one more; g2w_append adds at the end and
 * reports where it landed. All four parallel arrays and the species list move together.
 *
 * A slot arriving from OUTSIDE the list (put/insert/append) must name a species the games
 * have, 1..251: PokeDNA will not be the thing that puts a glitch Pokemon into a save that
 * did not have one (G2W_ERR_CONTENT if it tries). Both name fields must also carry a 0x50
 * inside their 11 bytes, or the list would fail its own structural gate a moment later
 * (G2W_ERR_STRUCT). Both are decided before anything is written. */
G2WStatus g2w_get(const uint8_t* list, int box, int slot, G2Slot* out);

/* Replaces one slot's four pieces and moves NOTHING else — the count and the terminator
 * stay exactly where they are, and neither does any other slot. So it is the one op with
 * nothing to prove: if the list was well formed before, the only thing that can make it
 * ill formed after is the incoming slot, which is fully checked before a byte is written.
 * It equally cannot REPAIR a list that was already broken elsewhere — that stays exactly
 * as broken, which is what lets a UI rename one entry of a box a glitch got to. Every
 * g2w_set_* setter is atomic for the same reason. */
G2WStatus g2w_put(uint8_t* list, int box, int slot, const G2Slot* in);

/* These reshape, so they additionally require every slot that will SURVIVE the reshape to
 * be structurally sound (G2W_ERR_STRUCT if not) — checked before the first byte moves,
 * which is what makes them all-or-nothing without a shadow copy. */
G2WStatus g2w_insert(uint8_t* list, int box, int at, const G2Slot* in);
G2WStatus g2w_append(uint8_t* list, int box, const G2Slot* in, int* slot_out);

/* Remove a slot and close the gap in all four arrays plus the species list. Deliberately
 * has NO content gate: a box that has been through a GB glitch can hold species 252 and
 * still be perfectly walkable, and deleting that entry is the one thing the user actually
 * wants (same reasoning as g2w_check_list_strict, below). For the same reason the slot
 * being DELETED is exempt from the survivor check above — an entry too broken to keep is
 * exactly the one that has to be removable. */
G2WStatus g2w_delete(uint8_t* list, int box, int slot);

/* Reorder within a list (party order, or tidying a box). Also no content gate: these move
 * bytes that are ALREADY in this list, which cannot make the save worse than it is, so a
 * glitch Pokemon can be dragged around the box like any other. It is only INTRODUCING one
 * that is refused.
 *
 * from == to (a == b) is a no-op, but NOT a free pass: it still runs the survivor check,
 * so G2W_OK from any op here can be read as "this list is well formed" without an
 * exception a caller has to remember. Dragging a Pokemon back where it started in a box
 * that is structurally broken reports G2W_ERR_STRUCT rather than a cheerful nothing. */
G2WStatus g2w_move_slot(uint8_t* list, int box, int from, int to);
G2WStatus g2w_swap_slots(uint8_t* list, int box, int a, int b);

/* A boxed record has no stats; a party record does, and they cannot be derived here
 * (this file owns no base-stat table on purpose). These two are the explicit,
 * caller-supplied conversion — g2w_put refuses a slot whose kind does not match the
 * destination list rather than inventing stats. */
void g2w_slot_to_box(G2Slot* s);
void g2w_slot_to_party(G2Slot* s, const uint16_t stats[6], uint16_t cur_hp, uint8_t status);

/* ------------------------------------------------------------ field setters */
/* Each takes the staged list; `slot` must be occupied. */

G2WStatus g2w_set_nickname(uint8_t* list, int box, int slot, const char* utf8);
G2WStatus g2w_set_otname  (uint8_t* list, int box, int slot, const char* utf8);

/* Writes the record's species byte AND the species-list byte (which is 0xFD while the
 * mon is an Egg). Nothing else keeps those two in step. */
G2WStatus g2w_set_species(uint8_t* list, int box, int slot, uint8_t species);
G2WStatus g2w_set_egg    (uint8_t* list, int box, int slot, bool egg);

G2WStatus g2w_set_held_item (uint8_t* list, int box, int slot, uint8_t item);
G2WStatus g2w_set_move      (uint8_t* list, int box, int slot, int i, uint8_t move,
                             uint8_t pp, uint8_t pp_ups);
G2WStatus g2w_set_pp        (uint8_t* list, int box, int slot, int i, uint8_t pp,
                             uint8_t pp_ups);
G2WStatus g2w_set_otid      (uint8_t* list, int box, int slot, uint16_t otid);
G2WStatus g2w_set_exp       (uint8_t* list, int box, int slot, uint32_t exp);
G2WStatus g2w_set_friendship(uint8_t* list, int box, int slot, uint8_t f);
G2WStatus g2w_set_pokerus   (uint8_t* list, int box, int slot, uint8_t p);
/* Crystal's capture record. G/S leave it zero; a Crystal mon traded to G/S keeps it. */
G2WStatus g2w_set_caught    (uint8_t* list, int box, int slot, uint8_t time,
                             uint8_t level, uint8_t loc, uint8_t ot_gender);

/* level 1..100. Sets fx->exp_stale — Gen 2 stores the level AND the EXP, and this file
 * owns no growth table, so the caller must set the matching EXP itself. */
G2WStatus g2w_set_level  (uint8_t* list, int box, int slot, uint8_t level, G2WEffects* fx);
/* i = 0..4 (HP, Atk, Def, Spd, Special), 0..65535. */
G2WStatus g2w_set_statexp(uint8_t* list, int box, int slot, int i, uint16_t v, G2WEffects* fx);

/* DVs, 0..15 each, in Atk/Def/Spd/Spc order. `gender_ratio` is the Gen-3 ratio byte
 * (pk_species_gender_ratio) so this file needs no species table; pass 0xFF for a
 * genderless species. *fx reports what the edit did to gender, shininess, the Unown
 * letter and the HP DV. */
G2WStatus g2w_set_dvs(uint8_t* list, int box, int slot, const uint8_t dv[4],
                      uint8_t gender_ratio, G2WEffects* fx);
G2WStatus g2w_set_dv (uint8_t* list, int box, int slot, int which, uint8_t v,
                      uint8_t gender_ratio, G2WEffects* fx);

/* Party-only plaintext. i = 0..5 (MaxHP, Atk, Def, Spd, SpA, SpD), same order as
 * G2Mon.stats. */
G2WStatus g2w_set_party_stats(uint8_t* list, int slot, const uint16_t stats[6],
                              uint16_t cur_hp, uint8_t status);

/* ------------------------------------------------------------- the writer */

/* Put exactly `len` bytes at file offset `off`, or return false. Same pure-C boundary as
 * gen2_save.h's G2ReadFn — the FatFs f_lseek/f_write live in the caller.
 *
 * NOTE TO WHOEVER WRITES THE GBA GLUE, because the symmetry is only half real: the READ
 * side genuinely is shared across generations (G2ReadFn and gen1_save.h's Gen1ReadFn have
 * the same signature, so ONE f_lseek/f_read shim serves both). The WRITE side is not, and
 * cannot be — gen1_write.h declares no write callback at all. Module A is a whole-image
 * API: gen1_write_apply() takes a resident 32 KiB `uint8_t* img`, edits it in RAM, and
 * persisting it is a separate sf_write_verified() pass afterwards. Module B is the
 * opposite by design and never holds the save: it streams every read and every write
 * through these two callbacks, so the FILE is the only copy and the .tmp-then-rename
 * around it IS the rollback (see the CALLER CONTRACT above). Budget for two different
 * pipelines and two very different amounts of RAM, not one shim with a second entry
 * point. */
typedef bool (*G2WriteFn)(void* ctx, uint32_t off, const void* buf, uint32_t len);

typedef struct {
  G2ReadFn  rd;
  G2WriteFn wr;
  void*     ctx;
  uint8_t*  scratch;        /* >= 64 bytes; bigger just means fewer SD ops           */
  uint32_t  scratch_len;
  uint32_t  file_len;       /* including any RTC tail, which is never touched        */
  G2Save    sv;             /* filled by g2w_begin — do not assign to this           */
  int       current_box;    /* filled by g2w_begin — the live/banked duality         */
  /* Deleting or reordering PARTY slots also shifts the mail array in SRAM bank 0,
   * which this file does not know the G/S offsets for. Such a commit is refused with
   * G2W_ERR_PARTY_MAIL unless the caller sets this, having established that no party
   * member is holding Mail. In-place field edits are unaffected. */
  bool      party_mail_ack;
  uint32_t  ready;          /* g2w_begin's seal; any hand-editing of sv trips it     */
} G2Writer;

/* Detect, validate and latch the layout. `expect` may be G2_VER_NONE to accept whatever
 * the file is, or a specific version to REFUSE a mismatch (G2W_ERR_VERSION) — this is
 * how a caller keeps a Crystal edit off a G/S save and vice versa. Refuses anything the
 * reader will not support, including a save whose primary checksum is already bad.
 *
 * After this, the version is sealed: writing to w->sv by hand invalidates `ready` and
 * every later call fails with G2W_ERR_STATE, so there is no path that writes a Crystal
 * layout into a G/S image. */
G2WStatus g2w_begin(G2Writer* w, G2ReadFn rd, G2WriteFn wr, void* ctx, uint32_t file_len,
                    uint8_t* scratch, uint32_t scratch_len, G2Version expect);

/* Stage one box or the party into `list` (>= g2_list_size(box) bytes). For the current
 * box this reads the live copy, which is the one the reader parses. */
G2WStatus g2w_load_list(G2Writer* w, int box, uint8_t* list);

/* Do the current box's two copies agree? They do in any save the game itself wrote
 * (SaveBox runs before the checksum on every save). If they do not, the reader — and so
 * the UI, and so g2w_load_list — is showing the LIVE copy while the game would boot from
 * the BANKED one, and committing that box makes the live copy win. Worth asking before
 * offering to save, because that is a decision, not a detail. */
G2WStatus g2w_current_box_agrees(G2Writer* w, bool* agree);

/* Write a staged list back and prove it. Checks the structure, writes every copy the
 * layout demands (both copies for the current box), mirrors anything inside the
 * checksummed span, refreshes both stored checksums, then re-reads and re-parses the
 * whole file and refuses unless it comes back exactly as intended. */
G2WStatus g2w_commit_list(G2Writer* w, int box, const uint8_t* list);

/* Box names are 8 glyphs in a 9-byte field, inside the checksummed span. Same
 * mirror-and-verify treatment. */
G2WStatus g2w_set_box_name(G2Writer* w, int box, const char* utf8);

/* Generic patch, for the HEADER fields (money, badges, TID, player name, dex flags…)
 * that live in the checksummed span. Mirrors whatever part of the range lies inside it
 * and reads the bytes back. Refuses to touch anything that is not the caller's:
 *   - the RTC tail past 0x8000,
 *   - the two stored checksums and every byte of the backup copy (this file's to maintain),
 *   - any byte of any Pokemon list, party or box, live or banked — Pokemon change through
 *     g2w_commit_list so that they cannot skip the structural gate,
 *   - the current-box number, because moving it without moving the live copy discards a
 *     boxful of Pokemon at the next boot.
 * Does NOT refresh the checksums; call g2w_finish when the patches are done. */
G2WStatus g2w_write_range(G2Writer* w, uint32_t off, const uint8_t* buf, uint32_t len);

/* Recompute and store both checksums, then run the whole-file gate. */
G2WStatus g2w_finish(G2Writer* w);

/* The gate on its own: re-detect the file and require the same version with both
 * checksums valid. `box` >= 0 (or G2_BOX_PARTY) with a non-NULL `expect` additionally
 * re-reads every copy of that list and requires a byte-exact match plus a clean reparse. */
G2WStatus g2w_verify(G2Writer* w, int box, const uint8_t* expect);

/* ------------------------------------------------- mirror / checksum plumbing */

/* Where `off` is mirrored, using g2_mirror_map()'s regions. false = `off` is outside the
 * checksummed span and has no mirror (boxes, the live current-box copy, the RTC tail). */
bool g2w_mirror_of(G2Version ver, uint32_t off, uint32_t* mirror_off);

/* Byte-compare `len` bytes at `off` against their mirror. G2W_ERR_VERIFY if they differ,
 * G2W_OK if they match or lie outside the span. The direct check the checksum cannot
 * make: two different regions can share a 16-bit sum. */
G2WStatus g2w_check_mirror(G2Writer* w, uint32_t off, uint32_t len);

/* Stream the two checksums out of the file without holding it resident. The summed
 * spans are derived from g2_mirror_map() — the sources for the primary, the destinations
 * for the backup — so the map is the single place either can be wrong. */
G2WStatus g2w_calc_checksums(G2Writer* w, uint16_t* primary, uint16_t* backup);

#endif /* GEN2_WRITE_H */
