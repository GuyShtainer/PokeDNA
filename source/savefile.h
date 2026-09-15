#ifndef SAVEFILE_H
#define SAVEFILE_H

#include <stdint.h>
#include <stdbool.h>

/* review #84a P1: the byte-compare buffer is 1024 B, so every chunked stream write is capped here;
 * savefile.c asserts sizeof(s_cmp) >= this and the icon extractor asserts its row size <= this. */
#define SF_STREAM_CHUNK_MAX 1024u

/* Safe save-file I/O primitives (the never-corrupt-user-data layer).
 *
 * Trimmed from the record-mixer's savefile.* to the generic, reusable core:
 * full reads, immutable backups, and the verified-write pattern. The
 * record-mix-specific paths (sf_mix_bidir / sf_self_test and their SecretBase
 * scratch) were dropped; the edit-mode round-trip self-test (M5) will be a new
 * pk_self_test built over gen3_edit.* instead. */

/* Max save path length. Sibling-file scratch (.tmp/.bak) adds a short suffix,
 * so internal buffers are sized SF_PATH_MAX. */
#define SF_PATH_MAX 272

typedef enum {
  SF_OK = 0,
  SF_ERR_OPEN,     /* could not open a file                          */
  SF_ERR_READ,     /* read error / short read                        */
  SF_ERR_WRITE,    /* write error / short write                      */
  SF_ERR_SIZE,     /* file too small / wrong size                    */
  SF_ERR_VERIFY,   /* written bytes != intended bytes (round-trip)   */
  SF_ERR_BACKUP,   /* backup copy failed or mismatched               */
  SF_ERR_PARSE,    /* save did not validate                          */
  SF_ERR_RENAME,   /* final rename failed (temp left for recovery)   */
  SF_ERR_LAYOUT    /* internal: bad offsets / sector math            */
} SfStatus;

const char* sf_status_str(SfStatus s);

/* Read an entire file into buf (<= cap). Reports byte count via *out_size. */
SfStatus sf_read_full(const char* path, uint8_t* buf, uint32_t cap,
                      uint32_t* out_size);

/* Make an IMMUTABLE backup of src_path. Copies to "<src>.bak", or .bak1/.bak2…
 * if earlier ones exist (never overwrites an existing backup), then verifies
 * the copy byte-for-byte. The chosen path is written to out_bak. */
SfStatus sf_backup(const char* src_path, char* out_bak, unsigned out_bak_cap);

/* Single ROLLING backup: overwrite "<src>.bak" each time (so backups don't pile up). */
SfStatus sf_backup_rolling(const char* src_path, char* out_bak, unsigned out_bak_cap);

/* Delete every "<src>.bak" / ".bakN" file for a save; returns the count removed. */
int sf_clear_backups(const char* src_path);

/* Verified copy: copy src -> dst (4 KiB chunks) then byte-compare the two. dst is
 * overwritten if it exists, so callers that must not clobber choose a free name. */
SfStatus sf_copy(const char* src_path, const char* dst_path);

/* Write buf(len) to path safely:
 *   write "<path>.tmp" -> re-read & byte-compare to buf -> unlink(path)
 *   -> rename(tmp -> path) -> READ THE CARD BACK to confirm the swap landed.
 * The original is untouched unless verification passed, so a failure never
 * corrupts it. SF_ERR_RENAME means the swap could not be confirmed: nothing was
 * deleted, and sf_where_are_the_bytes() says what the user is left holding. */
SfStatus sf_write_verified(const char* path, const uint8_t* buf, uint32_t len);

/* A source of bytes for a STREAMED verified write: fill dst(want) with the bytes that
 * belong at logical offset `off` (0-based) of the content being written. Must be able
 * to reproduce the SAME bytes for the SAME offset on a second call (this function
 * calls it once per chunk in the write pass and again in the verify pass) — the
 * generator's own re-readable source (e.g. a ROM) is what makes that possible without
 * holding the whole content in RAM. Returns false to abort the whole write (a read
 * failure, or a caller-driven cancel — either way sf_write_verified_stream discards
 * the .tmp and reports failure, exactly as if the write itself had failed). */
typedef bool (*SfStreamFn)(void* ctx, uint32_t off, uint8_t* dst, uint32_t want);

/* Same contract and the SAME four-step invariant as sf_write_verified (".tmp" -> byte-
 * compare re-read -> unlink -> rename -> read-the-card-back-to-confirm), for content
 * that is produced by `src` a chunk at a time instead of held in RAM as one buffer.
 * This exists because a kind file can be hundreds of KB (icons.bin is ~450 KB) while
 * the artless build's EWRAM headroom is a few KB — sf_write_verified's signature
 * requires the WHOLE content already in RAM, which such a file cannot fit.
 *
 * Every byte is fetched from `src` TWICE — once while writing, once while verifying —
 * which is the same "read twice, trust only agreement" discipline sf_write_verified
 * gets for free from comparing the written bytes back against its RAM buffer. When
 * `src` reads from a re-readable ROM, this doubles as read-twice-and-compare
 * verification on the SOURCE side too, for zero extra plumbing.
 *
 * `chunk` bytes at a time, must be > 0 and <= SF_STREAM_CHUNK_MAX (the internal compare buffer's
 * size, matching s_cmp elsewhere in this file); `scratch` must hold at least `chunk`
 * bytes and is the ONLY RAM this function asks the caller for — no buffer here scales
 * with `len`. `src` is called with off = 0, chunk, 2*chunk, ... twice over (once per
 * pass); a caller-side generator MAY use "off == 0" to detect the start of a new pass
 * (see source/art_icons_extract.c). */
SfStatus sf_write_verified_stream(const char* path, SfStreamFn src, void* ctx,
                                  uint32_t len, uint8_t* scratch, uint32_t chunk);

/* After SF_ERR_RENAME: which file on the CARD actually holds buf(len)?
 *
 * A caller that guesses will lie to the user in at least one interleaving, so this
 * asks the card instead -- byte-for-byte, both candidate names. It exists because
 * "the write returned an error" and "your save is gone" are very different pieces
 * of news and only one of them is true at a time. */
typedef enum {
  SF_WHERE_NEITHER = 0,  /* neither name holds it — fall back to the backup       */
  SF_WHERE_TMP_ONLY,     /* "<path>.tmp" holds it and `path` is GONE (the bad one) */
  SF_WHERE_TMP_AND_OLD,  /* "<path>.tmp" holds it, `path` still holds the OLD save */
  SF_WHERE_TARGET        /* `path` holds it after all — the swap did land          */
} SfWhere;
SfWhere sf_where_are_the_bytes(const char* path, const uint8_t* buf, uint32_t len);

/* A rolling-backup write for callers whose file (unlike a .sav) may not exist yet:
 *   f_stat(path) -- absence (FR_NO_FILE/FR_NO_PATH) is fine, nothing to back up, the
 *     write proceeds; any OTHER stat result is a card fault, not an empty slot, and
 *     REFUSES rather than silently writing a present file with no backup
 *   present -> sf_backup_rolling(path) first; its failure refuses too
 *   -> sf_write_verified(path, buf, len)
 * The return is sf_write_verified's raw status, SF_ERR_RENAME included: this function
 * does not decide "success" on the caller's behalf. A caller that wants the same
 * leniency app_commit/gb_persist give a confirmed-but-unconfirmable rename applies it
 * itself -- sf_where_are_the_bytes(path, buf, len) == SF_WHERE_TARGET means the bytes
 * landed and the write should read as a success despite the SF_ERR_RENAME.
 * `out_backed_up` (may be NULL) is set to false at entry and to true only right after a
 * successful sf_backup_rolling -- a caller whose failure message names a .bak (e.g. "use
 * the backup") must not say that for a box that was NEVER written before this call:
 * a virgin file has no backup to fall back to, and the message would send the user
 * chasing a file that does not exist. */
SfStatus sf_save_rolling(const char* path, const uint8_t* buf, uint32_t len, bool* out_backed_up);

/* The one-place verdict for sf_save_rolling's result (BACKLOG #158): plain SF_OK is a
 * yes; SF_ERR_RENAME is a yes only when sf_where_are_the_bytes(path, buf, len) reports
 * SF_WHERE_TARGET (the swap silently landed after all); every other status, and every
 * other SfWhere, is a no. This used to be re-typed by hand inside box_save() (pdna_bank.c)
 * AND, separately, inside its own host test (a copy the mutation testing in F3 already
 * caught drifting once) -- moved here so both call the SAME code and the host test can
 * link the real thing.
 *
 * `out_where` (may be NULL) is left UNTOUCHED unless sf_save_rolling actually returned
 * SF_ERR_RENAME -- a caller that pre-sets it to a sentinel outside the enum's range (e.g.
 * `(SfWhere)-1`) can tell "no rename ambiguity occurred at all" (plain SF_OK, or a hard
 * failure from the backup/write step -- box_save's own triage stays silent on those, same
 * as before this refactor) apart from "SF_ERR_RENAME happened", which is the ONLY case
 * that needs the caller's own UI triage (box_save's msg_wait switch). This mirrors
 * sf_save_rolling's own out_backed_up contract (savefile.c) instead of inventing a new one. */
bool sf_save_rolling_ok(const char* path, const void* buf, uint32_t len, SfWhere* out_where);

#endif /* SAVEFILE_H */
