#ifndef GB_SESSION_H
#define GB_SESSION_H

#include <stdint.h>
#include <stdbool.h>

#include "gen1_save.h"
#include "gen2_save.h"
#include "gen2_write.h"   /* G2Writer lives in the session: it IS the Gen-2 pipeline */

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
  GBS_ERR_VERIFY      /* the image did not read back as intended (image restored)  */
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
  uint8_t*  img;          /* the resident save; CALLER-OWNED, must outlive the session */
  uint32_t  len;          /* file length INCLUDING any RTC tail, which is never touched */
  uint8_t   gen;          /* GB_GEN1 / GB_GEN2 (gb_edit.h's numbering)                 */
  bool      open;

  uint8_t*  scratch;      /* CALLER-OWNED, >= GBS_SCRATCH_BYTES, outlives the session   */
  uint32_t  scratch_len;

  Gen1Save  g1;           /* gen == GB_GEN1                                            */
  G2Writer  g2w;          /* gen == GB_GEN2: begun once, reused for load and commit    */
} GbSession;

/* Identify and latch. Gen 2 is tried FIRST for the same reason pdna_gen12_mount tries it
 * first: its verdict is a 16-bit sum over ~3 KB, so a Gen-1 image passing it by accident
 * is a ~1/65536 event, where Gen-1's single 8-bit sum would accept a Gen-2 image about
 * once in 256. `img` is retained; do not move or free it while the session is open. */
GbsStatus gbs_open(GbSession* s, uint8_t* img, uint32_t len,
                   uint8_t* scratch, uint32_t scratch_len);

/* Storage boxes, excluding the party pseudo-box (12 for Gen 1, 14 for Gen 2), and the
 * index the party is addressed by. Both 0 / -1 on a closed session. */
int gbs_nboxes(const GbSession* s);
int gbs_party_box(const GbSession* s);

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
 * copies. gen1_write.c's restore-on-failure writes ONE pre-write snapshot (the first
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

#endif /* GB_SESSION_H */
