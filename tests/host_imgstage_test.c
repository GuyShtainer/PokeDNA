/* Host test for BACKLOG #234 slice 0: the staging funnel (source/img_stage.c) and the dirty-flag
 * split (source/img_flags.h). pdna_main.c's app_stage_sections / app_mark_pc_dirty / the finalize
 * fold are thin wrappers over exactly these functions -- there is no second copy to drift.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_imgstage_test.c source/img_stage.c \
 *      source/gen3_save.c -o /tmp/himgstage && /tmp/himgstage <saves...>
 *
 * The mutation harness (tests/host_g3_stage_sites_test.py) recompiles this file against MUTATED
 * copies of img_flags.h / img_stage.c and requires each mutant to make it fail.
 * Runs on every save it is given (the 5-game corpus, else the tests/fixtures saves). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gen3_save.h"
#include "gen3_box.h"   /* G3_PC_BYTES only (header; no link dependency) */
#include "img_flags.h"
#include "img_stage.h"

static int fails = 0, checks = 0;
static void check(const char* what, int cond) {
  checks++;
  if (!cond) { fails++; printf("  FAIL %s\n", what); }
}

static uint8_t g_save[G3_SAVE_FILE_SIZE];
static uint8_t g_pc[G3_PC_BYTES];
static uint8_t g_dec[G3_PC_BYTES];
static uint8_t g_orig[G3_SAVE_FILE_SIZE];

/* PC storage as the image decodes it: sections 5..13, 3,968 B each, by section ID (the same
 * reassembly gen3_read_pc_storage does, without dragging the mon decoder into this test). */
static int read_pc(const uint8_t* save, int slot, uint8_t* dst) {
  for (int id = G3_SID_PKMN_STORAGE_START; id <= G3_SID_PKMN_STORAGE_END; id++) {
    int idx = gen3_find_section(save, slot, id);
    if (idx < 0) return 0;
    memcpy(dst + (uint32_t)(id - G3_SID_PKMN_STORAGE_START) * G3_SECTOR_DATA_SIZE,
           save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)idx * G3_SECTOR_SIZE, G3_SECTOR_DATA_SIZE);
  }
  return 1;
}
static int pc_matches_save(const uint8_t* pc, int slot) {
  return read_pc(g_save, slot, g_dec) && memcmp(g_dec, pc, G3_PC_BYTES) == 0;
}

static void flag_table(void) {
  ImgFlags f; memset(&f, 0, sizeof f);
  check("fresh: nothing pending", !imgf_exit_prompt(&f) && imgf_arena_ok(&f) && !imgf_fold_needed(&f));
  imgf_staged(&f);
  check("a plain stage (SB2/SB1/dex) makes the exit prompt fire", imgf_exit_prompt(&f));
  check("... without any PC edit pending: no fold, arena still free, 'staged changes' wording",
        !imgf_fold_needed(&f) && imgf_arena_ok(&f) && strcmp(imgf_exit_line(&f), "Save the staged changes?") == 0);
  imgf_pc_edited(&f, true);
  check("a staged PC edit: prompt, arena refused (slice-0 compat), NO fold, 'moved Pokemon' wording",
        imgf_exit_prompt(&f) && !imgf_arena_ok(&f) && !imgf_fold_needed(&f) &&
        strcmp(imgf_exit_line(&f), "Save the moved Pokemon?") == 0);
  imgf_clear(&f);
  check("a successful write clears everything", !imgf_exit_prompt(&f) && imgf_arena_ok(&f) && !imgf_fold_needed(&f));
  imgf_pc_edited(&f, false);
  check("an UNstaged PC edit is the only thing that asks for a fold", imgf_fold_needed(&f) && imgf_exit_prompt(&f));
  imgf_pc_folded(&f);
  check("folding clears pc_unstaged but the image is still dirty", !imgf_fold_needed(&f) && imgf_exit_prompt(&f));
  check("NULL flags are tolerated", !imgf_fold_needed(NULL) && !imgf_exit_prompt(NULL) && !imgf_arena_ok(NULL));
}

static int run_one(const char* path) {
  FILE* fp = fopen(path, "rb");
  if (!fp) { printf("cannot open %s\n", path); return 1; }
  size_t n = fread(g_save, 1, sizeof g_save, fp);
  fclose(fp);
  Gen3SaveInfo info;
  if (n < (size_t)G3_SLOT_BYTES || !gen3_parse(g_save, (uint32_t)n, &info) || !info.valid) {
    printf("  skip %s (not a parsable Gen-3 save)\n", path);
    return 0;
  }
  int slot = info.slot;
  memcpy(g_orig, g_save, sizeof g_save);
  check("PC reassembles", read_pc(g_save, slot, g_pc));

  /* (c) box-drop eager staging: a drop edits g_pc, marks it, and g_save ALREADY matches. */
  ImgFlags f; memset(&f, 0, sizeof f);
  g_pc[0x10] ^= 0x5A; g_pc[G3_PC_BYTES - 5] ^= 0xA5;      /* a byte in the first + last section */
  check("before the drop g_save does NOT decode to the edited g_pc", !pc_matches_save(g_pc, slot));
  img_pc_edited(&f, g_save, slot, g_pc, true);
  check("after the drop g_save sections 5..13 already decode to g_pc (eager stage)", pc_matches_save(g_pc, slot));
  check("... flags: image dirty + PC edit pending, nothing left to fold",
        f.image_dirty && f.pc_moved && !f.pc_unstaged && !imgf_fold_needed(&f));
  check("... and the funnel kept the checksums valid", gen3_verify_full_checksums(g_save, slot, NULL));

  /* (a) the map-warp/tileset case: the arena lends g_pc out (foreign bytes) while a staged PC
   * edit is pending; a commit's fold must NOT copy g_pc over g_save. */
  uint8_t kept[G3_SAVE_FILE_SIZE]; memcpy(kept, g_save, sizeof kept);
  uint8_t edited_pc[G3_PC_BYTES]; memcpy(edited_pc, g_pc, sizeof edited_pc);
  memset(g_pc, 0xA5, sizeof g_pc);                        /* the borrowed tileset bytes */
  check("fold refuses while g_pc is foreign but the PC edit is already staged",
        !img_fold_pc(&f, g_save, slot, g_pc));
  check("... g_save is byte-for-byte untouched by that fold", memcmp(kept, g_save, sizeof kept) == 0);
  check("... and still decodes to the edited PC (release re-derives it exactly)", pc_matches_save(edited_pc, slot));

  /* the un-staged fallback: mark when it cannot stage, then the fold pushes it in and clears */
  memcpy(g_save, g_orig, sizeof g_save);
  memset(&f, 0, sizeof f);
  memcpy(g_pc, edited_pc, sizeof g_pc);
  img_pc_edited(&f, g_save, slot, g_pc, false);
  check("cannot-stage: g_save untouched, pc_unstaged set", memcmp(g_orig, g_save, sizeof g_save) == 0 && f.pc_unstaged);
  check("... the finalize fold then folds", img_fold_pc(&f, g_save, slot, g_pc) && pc_matches_save(g_pc, slot) && !f.pc_unstaged);

  /* (b) an SB2-only change reaches the exit gate. Section 0 = SaveBlock2. */
  memcpy(g_save, g_orig, sizeof g_save);
  memset(&f, 0, sizeof f);
  uint8_t sb2[G3_SECTOR_DATA_SIZE];
  memset(sb2, 0x77, sizeof sb2);
  check("SB2-only stage succeeds", img_stage_sections(&f, g_save, slot, 0, 0, sb2));
  check("SB2-only: the exit gate sees it (old 'PC moves or Day-Care' gate would not)",
        imgf_exit_prompt(&f) && !f.pc_moved);
  check("SB2-only: the funnel wrote the section and kept the checksums valid",
        gen3_verify_full_checksums(g_save, slot, NULL));

  /* argument validation */
  check("bad range refused", !img_stage_sections(&f, g_save, slot, 5, 4, sb2) &&
                             !img_stage_sections(&f, g_save, slot, 0, 14, sb2) &&
                             !img_stage_sections(&f, g_save, slot, 0, 0, NULL) &&
                             !img_stage_sections(NULL, g_save, slot, 0, 0, sb2));
  return 0;
}

int main(int argc, char** argv) {
  flag_table();
  int used = 0;
  for (int i = 1; i < argc; i++) { if (run_one(argv[i])) return 2; used++; }
  if (!used) printf("SKIP (no save supplied): only the pure flag table ran\n");
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
