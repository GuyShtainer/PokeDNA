#ifndef GB_SESSION_H
#define GB_SESSION_H

#include <stdint.h>
#include <stdbool.h>

#include "gen1_save.h"
#include "gen2_save.h"
#include "gen2_write.h"   /* G2Writer lives in the session: it IS the Gen-2 pipeline */
#include "gb_edit.h"      /* GbEditMon -- gbs_insert() takes one already built        */

/*
 * gb_session — ONE editing API over a Game Boy save that is RESIDENT IN RAM.
 *
 * Pure C (no tonc, no FatFs, no GBA headers) so tests/host_gbsession_test.c compiles it
 * on the PC against Guy's real cartridge saves. Nothing here opens, reads or writes a
 * file: the caller hands over a buffer that already holds the whole save, and persisting
 * it afterwards is the caller's job — on the GBA that means sf_write_verified() (an
 * immutable backup, then .tmp -> byte-compare re-read -> unlink -> rename -> read the
 * card back). Design: docs/GEN12-EDIT-DESIGN.md section 3.2.
 *
 * WHY A RESIDENT IMAGE, when gen2_write.c is built to stream.
 * ----------------------------------------------------------
 * gen2_write.h's CALLER CONTRACT demands that its write callback point at a WORKING
 * COPY, never at the user's save, because that module cannot roll back — "refuse the
 * rename IS the rollback". A RAM image satisfies that contract in the strongest form
 * available: the user's file is never opened for writing at all, so a failure at any
 * point leaves the card holding the untouched original rather than a half-written .tmp.
 * A Gen-1/2 save is 32 KiB and PokeDNA already has a 128 KiB EWRAM buffer (g_save) that
 * is completely idle in a Game Boy session, so residency costs nothing.
 *
 * THE SEAM THIS MODULE BRIDGES, and why it is not one function.
 * ------------------------------------------------------------
 * Three modules already exist and each refuses to know about the others:
 *   gb_edit.c     edits ONE OCCUPIED SLOT inside a list blob. Cross-generation, lossless
 *                 by construction. "Nothing here knows a save exists."
 *   gen1_write.c  a WHOLE-IMAGE api: it owns the Gen-1 current-box duality, the two
 *                 bank checksums and the main checksum.
 *   gen2_write.c  a STREAMING api over rd/wr callbacks: it owns the Gen-2 backup mirror
 *                 and both stored checksums.
 * gen2_write.h warns the glue author outright: "Budget for two different pipelines and
 * two very different amounts of RAM, not one shim with a second entry point." This
 * module is that budget, honestly paid — one API, two genuinely different bodies:
 *   - Gen 2 goes through a G2Writer whose callbacks are memcpy against the image, and
 *     g2w_commit_list does the mirror + checksums + whole-file re-parse itself.
 *   - Gen 1 has no "commit this blob" entry point, so it is COMPOSED here from the
 *     primitives gen1_write.h exposes for exactly this purpose (gen1_write_targets,
 *     gen1_blob_check, the two checksum fixers, gen1_write_verify_image_box), with a
 *     snapshot taken first so a failed verify restores every byte.
 *
 * WHAT IT DELIBERATELY DOES NOT DO. Adding, deleting and compacting Pokemon — the
 * four-parallel-array surgery that moves a count byte and a 0xFF terminator — is not
 * here. gbs_commit_list() takes a list whose STRUCTURE is unchanged and whose slots were
 * patched in place by gb_edit. List surgery is a separate slice with its own gates.
 */

/* ---- status --------------------------------------------------------------- */

typedef enum {
  GBS_OK = 0,
  GBS_ERR_ARG,        /* NULL / out-of-range argument, or a session never opened   */
  GBS_ERR_NOT_GB,     /* neither parser accepts this image                         */
  GBS_ERR_BOX,        /* box index out of range for the detected generation        */
  GBS_ERR_UNWRITABLE, /* this box cannot be written (Gen-1 virgin/uninitialised)   */
  GBS_ERR_STRUCT,     /* the list handed in is not structurally sound              */
  GBS_ERR_ENGINE,     /* the write engine refused the commit                       */
  GBS_ERR_VERIFY,     /* the image did not read back as intended (image restored)  */
  /* ---- S3: list surgery (gbs_delete / gbs_move) -------------------------- */
  GBS_ERR_FULL,       /* the destination box/party has no free slot                */
  GBS_ERR_PARTY_FLOOR,/* this would leave the party without a usable Pokemon       */
  GBS_ERR_MAIL,       /* a Gen-2 party restructure while a member holds Mail       */
  GBS_ERR_NEEDS_BASE, /* Gen-1 box->party needs base stats this tree does not carry*/
  GBS_ERR_SLOT        /* the source slot is empty, or unreadable                   */
} GbsStatus;

const char* gbs_status_text(GbsStatus st);

/* ---- scratch -------------------------------------------------------------- */

/* ONE caller-owned buffer, handed over at gbs_open() and retained for the whole session
 * — not a per-call parameter, because the G2Writer keeps the pointer and reads through it
 * long after g2w_begin returns (its verify, its mirror compare, which splits the buffer in
 * half, and its checksum streaming all use w->scratch). A per-call buffer would dangle.
 * Never used by both pipelines at once:
 *   Gen 1 — the pre-edit blob snapshot, so a failed verify can restore it (0x462 = 1122).
 *   Gen 2 — the G2Writer's own streaming scratch (its own bar is >= 64; more just means
 *           fewer memcpys, and the mirror compare wants room for two halves).
 * 1152 is the larger requirement rounded up to a word, and matches GB12_STAGE_BYTES so a
 * caller can reuse the mount's staging buffer instead of finding another 1.1 KB. */
#define GBS_SCRATCH_BYTES 1152

/* The largest list blob either generation has: Gen-1 box 0x462 (1122), Gen-2 box 1102. */
#define GBS_LIST_BYTES    1152

/* ---- the session ---------------------------------------------------------- */

typedef struct {
  uint8_t*  img;          /* the resident save; CALLER-OWNED, must outlive the session.
                           * INVARIANT (BACKLOG #64): img == NULL means this session is
                           * STREAMED and therefore READ-ONLY -- every function that
                           * would dereference img instead goes through `rd`, and every
                           * function that would WRITE through img is refused outright
                           * (gbs_can_write() in gb_session.c). */
  uint32_t  len;          /* file length INCLUDING any RTC tail, which is never touched */
  uint8_t   gen;          /* GB_GEN1 / GB_GEN2 (gb_edit.h's numbering)                 */
  bool      open;

  uint8_t*  scratch;      /* CALLER-OWNED, >= GBS_SCRATCH_BYTES, outlives the session   */
  uint32_t  scratch_len;

  /* ---- BACKLOG #64: streamed read-only sessions (img == NULL) -------------------
   * Set only by gbs_open_streamed(). Placed HERE (right after scratch_len, before
   * g1/g2w) rather than at the end of the struct for two reasons: it is where the
   * brief puts it, and tools/stack_budget.py's struct-field parser lays a header
   * out top-down and stops trusting offsets once it hits a member type it cannot
   * size from THIS header alone (Gen1Save/G2Writer are opaque here -- their bodies
   * live in gen1_save.h/gen2_write.h) -- putting `rd`/`rdctx` after g1/g2w would
   * make their own offset unresolvable. Typed as gen1_save.h's Gen1ReadFn (not a
   * fresh inline function-pointer field): it is already this exact signature
   * (gb_read()'s own shape, and gen2_save.h's G2ReadFn), so a caller's existing
   * FIL/ROM read callback plugs straight in with no adapter; and the parser only
   * resolves a POINTER FIELD's offset through a known typedef name
   * (_KNOWN_PTR_TYPEDEFS) -- an inline `bool (*rd)(...)` declarator's own
   * parameter-list commas defeat its multi-declarator split, which every other
   * function-pointer struct field in this codebase (GbArtIo.fn, Gb12Mount.rd,
   * RomGbLearn.read, ...) already avoids the same way. */
  Gen1ReadFn rd;
  void* rdctx;

  Gen1Save  g1;           /* gen == GB_GEN1                                            */
  G2Writer  g2w;          /* gen == GB_GEN2: begun once, reused for load and commit    */
} GbSession;

/* Identify and latch. Gen 2 is tried FIRST for the same reason pdna_gen12_mount tries it
 * first: its verdict is a 16-bit sum over ~3 KB, so a Gen-1 image passing it by accident
 * is a ~1/65536 event, where Gen-1's single 8-bit sum would accept a Gen-2 image about
 * once in 256. `img` is retained; do not move or free it while the session is open. */
GbsStatus gbs_open(GbSession* s, uint8_t* img, uint32_t len,
                   uint8_t* scratch, uint32_t scratch_len);

/* BACKLOG #64: same identification, over a caller's read callback instead of a
 * resident buffer -- for a session that must stay READ-ONLY (no room for an image).
 * `img` stays NULL for the life of this session: every write entry point below
 * refuses with GBS_ERR_UNWRITABLE (gbs_can_write(), gb_session.c), and every read
 * goes through `rd`/`rdctx` instead of a direct memcpy. `scratch`/`scratch_len` are
 * still required (the Gen-2 writer's own streaming scratch), same contract as
 * gbs_open(). */
GbsStatus gbs_open_streamed(GbSession* s,
                            bool (*rd)(void*, uint32_t, void*, uint32_t), void* ctx,
                            uint32_t len, uint8_t* scratch, uint32_t scratch_len);

/* Storage boxes, excluding the party pseudo-box (12 for Gen 1, 14 for Gen 2), and the
 * index the party is addressed by. Both 0 / -1 on a closed session. */
int gbs_nboxes(const GbSession* s);
int gbs_party_box(const GbSession* s);

/* BACKLOG #95 (gbmon C11/C2 wiring): is this an open Gen-2 session on western Crystal —
 * the only Gen-2 version with a capture record (0x1D/0x1E)? False on a closed session,
 * a Gen-1 session, or Gold/Silver. The single source of truth for "does this save get a
 * synthetic Met record": both the live editor (pdna_gen12.c's gb_mark_caught) and any
 * tool driving gb_session directly (tools/gb_retail_gate.py's --op caught case) call
 * this rather than re-deriving the version check, so they cannot diverge. */
bool gb_session_is_crystal(const GbSession* s);

/* May this box be committed to at all? Answers BEFORE the user starts editing, so a UI
 * can grey the box out rather than refusing after the work. The Gen-1 answer is the
 * interesting one: with bit 7 of 0x284C clear the eleven banked boxes are un-erased SRAM
 * noise and the game's first in-game box switch runs EmptyAllSRAMBoxes over all twelve —
 * an edit written there would be DESTROYED, not corrupted. */
GbsStatus gbs_box_writable(GbSession* s, int box);

/* Stage one box (or the party) into `list`, which must hold GBS_LIST_BYTES. For Gen-2's
 * current box this reads the LIVE copy, which is the one the reader parses. */
GbsStatus gbs_load_list(GbSession* s, int box, uint8_t* list);

/* Write a staged list back into the image and prove it took.
 *
 * Both pipelines end with the engine re-reading what it wrote and refusing unless it
 * comes back exactly as intended -- but a refusal does NOT mean the image is left
 * byte-identical to how it arrived. Before the verify, gen2_write.c's write_patch()
 * has already landed the new bytes in every copy; on a verify failure it restores
 * NOTHING, so the image can be left partially written across the primary/backup/banked
 * copies. This file's Gen-1 commit (gb_session.c gen1_commit) restores ONE snapshot (the first
 * target's own original bytes) into every target, which is only a true restore if all
 * targets already agreed before the edit -- not a guarantee this layer makes. Either
 * way, a caller that needs "no edit ever visible after a refused commit" must keep its
 * own pristine copy of the image and roll back to it itself on any non-GBS_OK status
 * (see pdna_gen12.c's gb_edit_rollback(), called from every failure branch of its
 * gb_edit_hook / gb_edit_persist). That is the LAYOUT half of the safety story; the
 * CARD half is the caller's sf_write_verified() afterwards. gen1_write.h names the
 * split exactly: "This is the check that the layout is right; that one is the check
 * that the card is." -- but "the layout is right" is a refusal signal, not a promise
 * that a refusal leaves the layout untouched.
 *
 * A commit whose list is byte-identical to what the image already holds writes NOTHING
 * and returns GBS_OK — "open a box and back out" must not rewrite a save, checksums
 * included. */
GbsStatus gbs_commit_list(GbSession* s, int box, const uint8_t* list);

/* ---- S3: list surgery (docs/GEN12-EDIT-DESIGN.md section 6) ---------------
 *
 * DELETE and MOVE, over the primitives gen1_write.h / gen2_write.h expose for exactly
 * this: gen1_blob_apply's GEN1_OP_DELETE/GEN1_OP_INSERT for Gen 1, g2w_delete/g2w_append
 * for Gen 2. Both stage into a CALLER-OWNED GBS_LIST_BYTES buffer and go through the same
 * gbs_commit_list() this file already has, so the destination gate (a virgin Gen-1 bank,
 * a failed verify) is exactly the gate every other commit gets — no second copy of it.
 *
 * ATOMICITY CONTRACT. Every refusal listed on a function below happens BEFORE that
 * function's first byte moves. gbs_move() is the one exception worth spelling out: it is
 * two RAM commits (destination, then source), and if the SECOND one (the source delete)
 * fails, the destination commit has already landed in `s->img` — a duplicate. This layer
 * does not roll that back itself (there is nothing left in this file to roll back FROM:
 * unlike gb_session.c's own pristine-copy convention, gbs_move never sees the session's
 * pristine buffer). The caller MUST treat any non-GBS_OK from gbs_move() as "restore the
 * whole image from your own pristine copy", exactly the same contract gbs_commit_list's
 * own header comment already documents for a bare commit (see pdna_gen12.c's
 * gb_edit_rollback()). A duplicate is therefore never actually persisted: it can only
 * ever exist in RAM, for the instant between the two commits, and only on a failure path
 * the caller is required to roll back. */

/* Remove slot `slot` of `box` (or the party). `list` is the caller's GBS_LIST_BYTES
 * staging buffer, loaded and rewritten in place.
 *   GBS_ERR_SLOT         slot >= the box's occupied count.
 *   GBS_ERR_PARTY_FLOOR  `box` is the party and this would leave it with no usable
 *                        Pokemon — Gen 1: the count would hit 0; Gen 2: no non-Egg slot
 *                        other than `slot` would remain. DELIBERATELY STRICTER than
 *                        retail here: pokecrystal's own deposit gate
 *                        (bills_pc.asm:1594-1632 BillsPC_CheckMail_PreventBlackout) is a
 *                        plain party-count check with no species test, so retail itself
 *                        lets a player carry an Egg alone. (Retail's Egg rule runs the
 *                        OTHER way — bills_pc.asm:1634-1650 BillsPC_IsMonAnEgg refuses
 *                        to RELEASE an Egg at all; this function does not reproduce
 *                        that refusal, so releasing an Egg through S3 is allowed.)
 *   GBS_ERR_MAIL         Gen-2 party only: some OTHER party member holds Mail (the mail
 *                        array in SRAM bank 0 is indexed by party slot and shifts with
 *                        a delete; this file does not know its G/S offsets).
 *   GBS_ERR_UNWRITABLE   `box` is a virgin Gen-1 bank (gbs_box_writable's own rule).
 * Otherwise whatever gbs_commit_list(s, box, list) returns. */
GbsStatus gbs_delete(GbSession* s, int box, int slot, uint8_t* list);

/* Move slot `from_slot` of `from_box` into `to_box`, landing it at `*to_slot` (out
 * parameter, valid only on GBS_OK). `src_list`/`dst_list` are the caller's two
 * GBS_LIST_BYTES staging buffers — TWO, because both boxes are loaded and rewritten
 * independently and neither commit may see the other's half-built bytes.
 *   GBS_ERR_ARG          from_box == to_box.
 *   GBS_ERR_SLOT         from_slot is not occupied, OR (Gen 1 only) the slot's
 *                        species-list byte disagrees with its record (gen1_edit_load's
 *                        own refusal) — such a slot may be gbs_delete()d but not moved.
 *   GBS_ERR_FULL         `to_box` already holds gb_list_capacity() Pokemon.
 *   GBS_ERR_PARTY_FLOOR  see gbs_delete — checked when `from_box` is the party.
 *   GBS_ERR_MAIL         Gen-2 only: some member of whichever party box is INVOLVED
 *                        (source or destination) holds Mail. An append into the party
 *                        changes its species/count area exactly as a delete does, so
 *                        the same refusal applies in both directions.
 *   GBS_ERR_NEEDS_BASE   Gen-1 box -> party: the party record needs computed stats and
 *                        this tree carries no Gen-1 base-stat table (see gb_edit.h's
 *                        GbGen1Base). Nothing is moved. Gen-1 party -> box always works
 *                        (drop the extra bytes, copy the live level into the box-level
 *                        field, exactly what the game's own deposit does).
 * Record-kind conversion when crossing box<->party is automatic and lossless in the
 * directions this returns GBS_OK for; see gb_session.c for exactly what each direction
 * does. Otherwise whatever the two gbs_commit_list() calls returned — see the atomicity
 * contract above. */
GbsStatus gbs_move(GbSession* s, int from_box, int from_slot, int to_box, int* to_slot,
                   uint8_t* src_list, uint8_t* dst_list);

/* ---- S5-B: insert an ALREADY-BUILT record (docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 5)
 *
 * gbs_move() above composes a box<->party CONVERSION with an APPEND; this is just the
 * append half, exposed for a caller that already holds a finished BOX-kind GbEditMon —
 * the Gen-3 -> Game Boy transfer (gen3_to_gb()) builds exactly that, and never a party
 * record (see gen3_to_gb.h: "into a brand-new Game Boy BOX record"). So `mon` may only
 * land in a STORAGE box, never the party: PARTY IS DELIBERATELY REFUSED WITH
 * GBS_ERR_ARG, because landing a converted mon in the party (species-limit checks,
 * live-stat computation, the Gen-2 Mail-shift rule) is gbs_move()'s job, not this one's
 * — a caller that wants the party target still has to go through gbs_move() with `mon`
 * inserted into a scratch box first, exactly as today.
 *
 *   GBS_ERR_BOX          `box` is not a valid box for this session's generation.
 *   GBS_ERR_UNWRITABLE   gbs_box_writable()'s own refusal (a virgin Gen-1 bank, ...).
 *   GBS_ERR_ARG          `box` is the party pseudo-box, OR `mon->gen` disagrees with
 *                        the session's generation, OR `mon->is_party` is true (a
 *                        party-shaped record handed to a box destination).
 *   GBS_ERR_FULL         `box` already holds gb_list_capacity() Pokemon.
 * Otherwise whatever gbs_commit_list(s, box, list) returns. `*slot_out` is valid only on
 * GBS_OK. `list` is the caller's GBS_LIST_BYTES staging buffer (loaded here, then
 * rewritten in place) — same convention as gbs_delete()'s `list` parameter.
 *
 * No Gen-2 Mail check: Mail only shifts OTHER party members, and this never targets the
 * party. */
GbsStatus gbs_insert(GbSession* s, int box, const GbEditMon* mon, int* slot_out,
                     uint8_t* list);

/* ---- generic field read/write (BACKLOG #49 P0, docs/GEN12-PARITY-DESIGN.md §4.0) ----
 *
 * The one primitive P1-P5's non-Pokemon screens (trainer card, bag, flags, fly, dex,
 * the Gen-2 clock) all need and neither engine offered on its own: a byte range that is
 * NOT a Pokemon list. Gen 2 already had it (gen2_write.h's g2w_write_range); Gen 1 did
 * not, so gen1_write_range (source/gen1_write.h) is this slice's other half. This is
 * that split, lifted to the one API every later slice calls through — same shape as
 * gbs_load_list / gbs_commit_list above, but for header fields instead of box lists. */

/* Read `n` bytes at file offset `off` straight out of the resident image. Unrestricted
 * (read-only, so there is nothing to corrupt) beyond a bounds check against `s->len` —
 * a caller wanting to know whether `off` is something this session will let it WRITE
 * asks gbs_write_field, not this. */
GbsStatus gbs_read_field(GbSession* s, uint32_t off, void* buf, uint32_t n);

/* Write `n` bytes at file offset `off`. Dispatches to whichever engine owns this
 * session's generation and inherits that engine's refusal set exactly:
 *   Gen 1 — gen1_write_range_ex (not the 64-byte gen1_write_range convenience
 *           wrapper — P0 review D6): confined to the main checksummed block, never
 *           the party, the open box's live copy, or the current-box-number byte
 *           (Pokemon change only through gbs_commit_list). Fixes the main checksum
 *           and re-verifies the image on EVERY call — Gen 1 has no backup mirror to
 *           defer, so there is nothing for gbs_finish to do afterward. NO PER-
 *           GENERATION CAP DIFFERENCE IS LEFT HERE: the rollback buffer is this
 *           session's own `scratch` (>= GBS_SCRATCH_BYTES, 1152) — the SAME buffer
 *           gbs_commit_list's Gen-1 path already borrows for a box-list snapshot,
 *           reused rather than newly allocated, since a session never runs a box
 *           commit and a field write in the same call — so a Gen-1 field write
 *           through THIS function is capped at 1152 bytes, comfortably past every
 *           field source/gb_fields.c defines (widest: 320 B), not gen1_write_range's
 *           own 64-byte convenience cap.
 *   Gen 2 — g2w_write_range: confined to the checksummed span, never a Pokemon list,
 *           the current-box-number byte, or either stored checksum / the backup copy.
 *           Mirrors whatever part of the range lies inside the checksummed span
 *           immediately, but deliberately does NOT refresh the stored checksums itself
 *           (gen2_write.h's own contract) — call gbs_finish() once after the LAST field
 *           write in a batch, not after each one, so N field edits cost one whole-file
 *           reparse instead of N. Capped only by s->len (the whole image).
 * A no-op (the bytes already read back as `buf`) writes nothing at all, on either
 * engine. */
GbsStatus gbs_write_field(GbSession* s, uint32_t off, const void* buf, uint32_t n);

/* Write `n` bytes at file offset `off`, ALLOWING a span OUTSIDE either generation's
 * checksummed window -- BACKLOG #89, the Hall-of-Fame allowlist. Only ever needed on
 * Gen 1: sHallOfFame sits entirely below GEN1_SUM_FIRST, so gbs_write_field's Gen-1
 * path (gen1_write_range_ex) refuses it outright. Dispatches to
 * gen1_write_outside_sum (chunked over the session's own scratch, GBS_SCRATCH_BYTES —
 * see gen1_write.h for the chunk-rollback contract and its multi-chunk-atomicity
 * caveat: on a mid-clear failure the CALLER must discard the whole image via its own
 * pristine copy, same as every other gb_session edit) on Gen 1. On Gen 2 this is
 * IDENTICAL to gbs_write_field — g2w_write_range is already capped only by s->len, and
 * Gen 2's sHallOfFame sits inside the checksummed+mirrored span, so there is no
 * separate restriction to route around; kept as one call so gb_hof.c never branches
 * on generation itself. */
GbsStatus gbs_write_outside_sum(GbSession* s, uint32_t off, const void* buf, uint32_t n);

/* Close out a batch of gbs_write_field() calls.
 *   Gen 2 — g2w_finish(): recompute and store BOTH checksums, then re-read and re-parse
 *           the whole file, refusing (image restored to whatever g2w_write_range already
 *           landed — see that module's CALLER CONTRACT: point it at a working copy, not
 *           the user's save) unless it comes back exactly as intended.
 *   Gen 1 — a no-op that returns GBS_OK: every gen1_write_range_ex call already fixed
 *           the main checksum and re-verified the image itself, so there is nothing
 *           left to close out. Safe (and cheap) to call unconditionally after a
 *           field-write batch regardless of which generation the session turned out
 *           to be. */
GbsStatus gbs_finish(GbSession* s);

/* Gen-2 Mail item ids, from the decomp (assets/upstream/pokecrystal/constants/
 * item_constants.asm) -- NOT one contiguous range: FLOWER_MAIL sits alone at 0x9e
 * (line 166, between HEAVY_BALL and LEVEL_BALL), and the other nine (SURF_MAIL ..
 * MIRAGE_MAIL) are contiguous at 0xb5..0xbd (lines 189-197). Exposed (not file-static)
 * so a caller checking "does this held item mean gbs_delete/gbs_move will refuse with
 * GBS_ERR_MAIL" — a test, or a future UI hint — asks the exact same predicate this file
 * refuses on, rather than a second hand-copied guess at the ranges. */
bool gbs_is_mail_item(uint8_t item);

#endif /* GB_SESSION_H */
