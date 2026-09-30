/* SPDX-License-Identifier: GPL-3.0-or-later
 * img_stage.c -- see img_stage.h. Pure C. */
#include "img_stage.h"
#include "gen3_save.h"

bool img_stage_sections(ImgFlags* f, uint8_t* save, int slot, int sect_lo, int sect_hi,
                        const uint8_t* block) {
  if (!f || !save || !block || sect_lo < 0 || sect_hi < sect_lo || sect_hi > 13) return false;
  for (int id = sect_lo; id <= sect_hi; id++)
    (void)gen3_write_full_section(save, slot, id,
                                  block + (uint32_t)(id - sect_lo) * G3_SECTOR_DATA_SIZE);
  /* slice 2: journal diff here */
  imgf_staged(f);
  return true;
}

void img_pc_edited(ImgFlags* f, uint8_t* save, int slot, const uint8_t* pc, bool can_stage) {
  bool staged = can_stage &&
                img_stage_sections(f, save, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc);
  imgf_pc_edited(f, staged);
}

bool img_fold_pc(ImgFlags* f, uint8_t* save, int slot, const uint8_t* pc) {
  if (!imgf_fold_needed(f)) return false;
  if (!img_stage_sections(f, save, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, pc))
    return false;
  imgf_pc_folded(f);
  return true;
}
