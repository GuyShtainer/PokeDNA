#ifndef PKVIEW_APP_H
#define PKVIEW_APP_H

#include <stdint.h>
#include <stdbool.h>

/* Shared app glue so the party list and box grid can open the editor and persist
 * safely. Implemented in pkview_main.c (which owns the loaded save + path). */

/* Writes are EZ-Flash-Omega-only. */
bool app_can_edit(void);

/* Edit the record `rec` (which lives inside `block`, the reassembled save-block
 * sections [sect_lo..sect_hi] of the loaded save). Runs the field editor; on
 * commit it rewrites those sections into the in-RAM save image (recomputing each
 * checksum), makes an immutable backup, and does a verified write. Shows the
 * result and returns true iff a write actually happened. Gated to Omega. */
bool app_edit_commit(uint8_t* rec, bool is_party, int sect_lo, int sect_hi, uint8_t* block);

/* Gen-3-PC-style action menu shown on A (SUMMARY / ITEM / MOVES / CANCEL on Omega;
 * straight to the read-only summary on Everdrive). Runs the chosen editor through
 * the same safe commit path as app_edit_commit. Returns true iff a write happened
 * (the caller should refresh its list/grid). */
bool app_mon_menu(uint8_t* rec, bool is_party, int sect_lo, int sect_hi, uint8_t* block);

#endif /* PKVIEW_APP_H */
