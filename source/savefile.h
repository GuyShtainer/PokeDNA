#ifndef SAVEFILE_H
#define SAVEFILE_H

#include <stdint.h>
#include <stdbool.h>

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

#endif /* SAVEFILE_H */
