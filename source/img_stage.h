/* SPDX-License-Identifier: GPL-3.0-or-later
 * img_stage.h -- BACKLOG #234 slice 0: the pure-C body of the staging funnel.
 *
 * pdna_main.c's app_stage_sections()/app_mark_pc_dirty()/app_save_finalize fold are thin
 * wrappers over these, so tests/host_imgstage_test.c pins the REAL logic (not a copy) on the
 * PC. tonc/FatFs-free: only gen3_save + img_flags. */
#ifndef IMG_STAGE_H
#define IMG_STAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "img_flags.h"

/* Copy `block` (sections lo..hi, 3,968 B each, addressed by section ID) into `save`'s slot via
 * gen3_write_full_section and mark the image dirty. Returns false (and touches nothing) on a
 * NULL argument or a bad section range. Slice 2 hangs the journal diff on this one function. */
bool img_stage_sections(ImgFlags* f, uint8_t* save, int slot, int sect_lo, int sect_hi,
                        const uint8_t* block);

/* A PC box edit reached `pc` (30 boxes' worth, sections 5..13). When `can_stage` the whole PC
 * is staged into `save` at once (the eager stage at the drop); otherwise only pc_unstaged is
 * recorded. Either way the image is dirty and a PC edit is pending. */
void img_pc_edited(ImgFlags* f, uint8_t* save, int slot, const uint8_t* pc, bool can_stage);

/* The finalize fold: stage `pc` into `save` ONLY if it is genuinely ahead (pc_unstaged), then
 * clear that flag. Returns true iff it folded. */
bool img_fold_pc(ImgFlags* f, uint8_t* save, int slot, const uint8_t* pc);

#endif /* IMG_STAGE_H */
