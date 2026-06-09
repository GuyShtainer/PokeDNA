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

/* Gen-3-PC-style action menu shown on A: SUMMARY / ITEM / MOVES / COPY / PASTE /
 * DUPLICATE / RELEASE on Omega (an empty slot offers PASTE only); straight to the
 * read-only summary on Everdrive. Slot-aware ops use box+slot; party callers pass
 * box = -1 and slot = the party index. Returns true iff a write happened (the
 * caller should refresh its list/grid). */
bool app_mon_menu(uint8_t* rec, bool is_party, int sect_lo, int sect_hi, uint8_t* block, int box, int slot);

/* true if the one-slot mon clipboard holds a copied mon (for the box grid to
 * allow PASTE onto an empty slot). */
bool app_clip_occupied(void);

/* The A-menu's MOVE action sets a one-shot flag; the box grid consumes it to
 * enter "pick up + reposition" mode. Returns true once per MOVE pick. */
bool app_take_move_request(void);

/* Commit the loaded save's SaveBlock2 (section 0 — the trainer block) or
 * SaveBlock1 (sections 1..4 — where money lives) after an in-place edit of the
 * shared g_sb2 / g_sb1 buffers. Same verified-write+backup path as the editors.
 * Returns true iff a write happened. Used by the editable trainer card. */
bool app_commit_sb2(void);
bool app_commit_sb1(void);
bool app_commit_pc(void);                 /* PC storage (sections 5..13): box name/wallpaper */

/* Emerald "Walda" secret wallpaper: the graphic shown by box wallpaper 16. Pattern
 * is 0..15 (sWaldaWallpapers index) in SaveBlock1. app_walda_pattern returns -1 on
 * non-Emerald; app_set_walda edits g_sb1 in place (commit via app_commit_sb1). */
int  app_walda_pattern(void);
bool app_set_walda(uint8_t pattern);

/* Shared framed yes/no confirm (A = yes, B = no). */
bool app_confirm(const char* title, const char* l1);

#endif /* PKVIEW_APP_H */
