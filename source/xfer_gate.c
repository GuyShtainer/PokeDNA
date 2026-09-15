#include "xfer_gate.h"

#define XG_SCOPE_GB 2u

bool xg_pc_live(bool vinfo_valid, bool arena_held) { return vinfo_valid && !arena_held; }

bool xg_togame_row(bool is_bank, bool pc_live, bool have_pc) { return is_bank && pc_live && have_pc; }

bool xg_paste_row(bool clip_occupied, bool pc_live) { return clip_occupied && pc_live; }

bool xg_create_row(bool is_bank, bool pc_live) { return !is_bank || pc_live; }

bool xg_inject_refuse(bool arena_held, bool vinfo_valid) { return arena_held || !vinfo_valid; }

bool xg_clear_carry_on_gb_exit(bool carry_is_gb) { return carry_is_gb; }

bool xg_drop_denied(uint8_t dst_scope, uint8_t src_scope) {
  return dst_scope == XG_SCOPE_GB || src_scope == XG_SCOPE_GB;
}
