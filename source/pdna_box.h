#ifndef PDNA_BOX_H
#define PDNA_BOX_H

#include <stdint.h>
#include <stdbool.h>
#include "pdna_app.h"      /* AppCommitFn */

/* A box "container" the box screen renders/edits, abstracted so the same screen
 * drives both the in-save PC storage and the external bank. Each source supplies:
 *  - records(box): pointer to that box's 30*80 raw records, in a buffer laid out
 *    pc-style (records at +0x0004) — for paged sources (the bank) this loads the
 *    box, flushing the previously-loaded one if dirty.
 *  - menu_block: the pc-layout buffer records() lives in (passed to app_mon_menu);
 *    the loaded box's index within it is `box` for the PC, 0 for the (paged) bank.
 *  - name/wallpaper get/set, commit (immediate verified write of the current box),
 *    and mark_dirty (deferred path — PC defers moves to save-file exit; the bank
 *    persists quietly there and then).
 * All function pointers act on module-singleton state, so they take no `self`. */
typedef struct {
  int  nboxes;
  bool last_box_is_party;    /* GB sources expose the party as one extra pseudo-box at
                              * nboxes-1 ("GB PARTY"); it has no ordinal, so the banner
                              * must not invent one ("13:" on a 12-box RBY save). All
                              * sources memset their BoxSource, so this defaults false. */
  int  start_box;
  bool is_bank;
  int  wp_count;                            /* selectable wallpapers: 16 or 32 (PC Emerald) */
  uint8_t* (*records)(int box);             /* -> 30*80 records (loads/flushes for the bank) */
  uint8_t* menu_block;                      /* pc-layout buffer (records at +0x0004)        */
  void (*get_name)(int box, char out[12]);
  void (*set_name)(int box, const char* s);
  int  (*get_wp)(int box);                  /* wallpaper RENDER id (0..31)                  */
  void (*set_wp)(int box, int wp);          /* store wallpaper id                            */
  bool (*can_edit)(void);
  AppCommitFn commit;                       /* persist the current box now                   */
  void (*mark_dirty)(void);                 /* deferred persist (move-mode drop)             */
  void (*note_add)(const uint8_t* rec);     /* opt: a mon landed here -> register its dex (PC only; NULL on bank) */
  /* BACKLOG #40(a): the box banner's occupancy denominator. NULL (every source before
   * #40) means the Gen-3 grid's own 30 -- the PC/bank's real capacity, unchanged. A GB
   * source's box holds fewer (20 for Gen 2, 12-99 for Gen 1 depending on version) than
   * the 30-cell grid it is drawn into; without this the banner read "20/30" for a full
   * Gen-2 box, 10 short of full. */
  int  (*capacity)(int box);
} BoxSource;

/* Game-faithful box screen over `src`: a left PKMN DATA panel + a 6x5 icon grid on
 * a wallpaper, with a ◄ box-name ► banner. D-pad moves the cursor, L/R (and LEFT/
 * RIGHT on the title) change box, A opens the action menu / 6-card summary.
 * Returns 0 if the user backed out (B), 1 to switch to party (SELECT), or 2 for the
 * trainer/menu (START). */
int pdna_box(BoxSource* src);

/* Reset the mon-in-hand carry state — call once when a save is (re)opened, since the
 * carry persists across pdna_box runs to survive the PC<->Bank hand-off. */
void pdna_box_clear_carry(void);

#endif /* PDNA_BOX_H */
