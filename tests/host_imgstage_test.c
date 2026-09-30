/* Host test for BACKLOG #234 slice 0: the staging funnel (source/img_stage.c) and the dirty-flag
 * split (source/img_flags.h). pdna_main.c's app_stage_sections / app_mark_pc_dirty / the finalize
 * fold are thin wrappers over exactly these functions -- there is no second copy to drift.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_imgstage_test.c source/img_stage.c \
 *      source/gen3_save.c source/journal.c source/journal_undo.c -o /tmp/himgstage && /tmp/himgstage <saves...>
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
  check("a STAGED PC edit (#234 s2, pc_moved retired): prompt, the arena is FREE (g_pc re-derives from g_save), NO fold, generic wording",
        imgf_exit_prompt(&f) && imgf_arena_ok(&f) && !imgf_fold_needed(&f) &&
        strcmp(imgf_exit_line(&f), "Save the staged changes?") == 0);
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
  img_pc_edited(&f, NULL, g_save, slot, g_pc, true);
  check("after the drop g_save sections 5..13 already decode to g_pc (eager stage)", pc_matches_save(g_pc, slot));
  check("... flags: image dirty + PC edit pending, nothing left to fold",
        f.image_dirty && !f.pc_unstaged && !imgf_fold_needed(&f) && imgf_arena_ok(&f));
  check("... and the funnel kept the checksums valid", gen3_verify_full_checksums(g_save, slot, NULL));

  /* (a) the map-warp/tileset case: the arena lends g_pc out (foreign bytes) while a staged PC
   * edit is pending; a commit's fold must NOT copy g_pc over g_save. */
  uint8_t kept[G3_SAVE_FILE_SIZE]; memcpy(kept, g_save, sizeof kept);
  uint8_t edited_pc[G3_PC_BYTES]; memcpy(edited_pc, g_pc, sizeof edited_pc);
  memset(g_pc, 0xA5, sizeof g_pc);                        /* the borrowed tileset bytes */
  check("fold refuses while g_pc is foreign but the PC edit is already staged",
        !img_fold_pc(&f, NULL, g_save, slot, g_pc));
  check("... g_save is byte-for-byte untouched by that fold", memcmp(kept, g_save, sizeof kept) == 0);
  check("... and still decodes to the edited PC (release re-derives it exactly)", pc_matches_save(edited_pc, slot));

  /* the un-staged fallback: mark when it cannot stage, then the fold pushes it in and clears */
  memcpy(g_save, g_orig, sizeof g_save);
  memset(&f, 0, sizeof f);
  memcpy(g_pc, edited_pc, sizeof g_pc);
  img_pc_edited(&f, NULL, g_save, slot, g_pc, false);
  check("cannot-stage: g_save untouched, pc_unstaged set", memcmp(g_orig, g_save, sizeof g_save) == 0 && f.pc_unstaged);
  check("... the finalize fold then folds", img_fold_pc(&f, NULL, g_save, slot, g_pc) && pc_matches_save(g_pc, slot) && !f.pc_unstaged);

  /* (b) an SB2-only change reaches the exit gate. Section 0 = SaveBlock2. */
  memcpy(g_save, g_orig, sizeof g_save);
  memset(&f, 0, sizeof f);
  uint8_t sb2[G3_SECTOR_DATA_SIZE];
  memset(sb2, 0x77, sizeof sb2);
  check("SB2-only stage succeeds", img_stage_sections(&f, NULL, g_save, slot, 0, 0, sb2));
  check("SB2-only: the exit gate sees it (old 'PC moves or Day-Care' gate would not)",
        imgf_exit_prompt(&f));
  check("SB2-only: the funnel wrote the section and kept the checksums valid",
        gen3_verify_full_checksums(g_save, slot, NULL));

  /* argument validation */
  check("bad range refused", !img_stage_sections(&f, NULL, g_save, slot, 5, 4, sb2) &&
                             !img_stage_sections(&f, NULL, g_save, slot, 0, 14, sb2) &&
                             !img_stage_sections(&f, NULL, g_save, slot, 0, 0, NULL) &&
                             !img_stage_sections(NULL, NULL, g_save, slot, 0, 0, sb2));
  return 0;
}

/* #300b: the word-wise checksum must equal the byte-wise reference on aligned AND unaligned buffers. */
static uint16_t csum_ref(const uint8_t* p, unsigned size) {
  uint32_t c = 0;
  for (unsigned i = 0; i + 4 <= size; i += 4)
    c += (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
  return (uint16_t)((c >> 16) + c);
}
static void checksum_pins(void) {
  static uint8_t buf[G3_SECTOR_DATA_SIZE + 8] __attribute__((aligned(4)));
  uint32_t x = 0x1234567u;
  for (unsigned i = 0; i < sizeof buf; i++) { x = x * 1664525u + 1013904223u; buf[i] = (uint8_t)(x >> 24); }
  check("checksum: aligned word path == byte reference", gen3_checksum(buf, G3_SECTOR_DATA_SIZE) == csum_ref(buf, G3_SECTOR_DATA_SIZE));
  check("checksum: unaligned fallback == byte reference",
        gen3_checksum(buf + 1, G3_SECTOR_DATA_SIZE) == csum_ref(buf + 1, G3_SECTOR_DATA_SIZE));
  check("checksum: a size that is not a multiple of 4 ignores the tail like the reference",
        gen3_checksum(buf, 3966) == csum_ref(buf, 3966));
}

/* The scope: deferral, ONE write per section, and the flags a deferred PC edit leaves behind. */
static int scope_pins(const char* path) {
  FILE* fp = fopen(path, "rb");
  if (!fp) return 1;
  size_t n = fread(g_save, 1, sizeof g_save, fp);
  fclose(fp);
  Gen3SaveInfo info;
  if (n < (size_t)G3_SLOT_BYTES || !gen3_parse(g_save, (uint32_t)n, &info) || !info.valid) return 0;
  int slot = info.slot;
  if (!read_pc(g_save, slot, g_pc)) return 1;
  memcpy(g_orig, g_save, sizeof g_save);
  ImgFlags f; memset(&f, 0, sizeof f);
  ImgRec r; memset(&r, 0, sizeof r);
  g_pc[0x40] ^= 0x3C;
  img_scope_open(&r, "Box move");
  img_pc_edited(&f, &r, g_save, slot, g_pc, true);
  check("scope: a deferred drop leaves g_save untouched until the close", memcmp(g_orig, g_save, sizeof g_save) == 0);
  check("scope: ... g_pc is honestly ahead (pc_unstaged), the image is dirty", f.pc_unstaged && f.image_dirty && !imgf_arena_ok(&f));
  g_pc[0x50] ^= 0x11;                                      /* the second stage of the same drop (clear_origin) */
  img_pc_edited(&f, &r, g_save, slot, g_pc, true);
  check("scope: the close applies it ONCE and folds", img_scope_close(&f, &r, g_save, slot) &&
        pc_matches_save(g_pc, slot) && !f.pc_unstaged && !r.mask && !r.depth);
  check("scope: ... checksums valid after the deferred write", gen3_verify_full_checksums(g_save, slot, NULL));
  /* nested scopes: only the outermost close applies */
  memcpy(g_save, g_orig, sizeof g_save); memset(&f, 0, sizeof f);
  uint8_t sb2[G3_SECTOR_DATA_SIZE]; memset(sb2, 0x66, sizeof sb2);
  img_scope_open(&r, "outer"); img_scope_open(&r, "inner");
  (void)img_stage_sections(&f, &r, g_save, slot, 0, 0, sb2);
  (void)img_scope_close(&f, &r, g_save, slot);
  check("scope: an inner close applies nothing", memcmp(g_orig, g_save, sizeof g_save) == 0 && r.mask == 1u);
  (void)img_scope_close(&f, &r, g_save, slot);
  const uint8_t* s0 = g_save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)gen3_find_section(g_save, slot, 0) * G3_SECTOR_SIZE;
  check("scope: the outer close applies it", memcmp(s0, sb2, G3_SECTOR_DATA_SIZE) == 0 && !r.mask);
  /* #300: an IDENTICAL section with a GOOD checksum is not rewritten (a drop changes one or two of nine);
   * a stale one is healed (review D3); a changed one is always written */
  memcpy(g_save, g_orig, sizeof g_save); memset(&f, 0, sizeof f);
  {
    uint8_t* sec5 = g_save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)gen3_find_section(g_save, slot, 5) * G3_SECTOR_SIZE;
    uint8_t keep[2];
    sec5[G3_OFF_CHECKSUM] ^= 0x55;                                   /* a deliberately stale checksum */
    memcpy(keep, sec5 + G3_OFF_CHECKSUM, 2);
    read_pc(g_save, slot, g_pc);
    (void)img_stage_sections(&f, NULL, g_save, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, g_pc);
    (void)keep; check("stage: an identical section with a STALE checksum is healed", gen3_section_checksum_ok(g_save, slot, 5, G3_SECTOR_DATA_SIZE));
    g_pc[3] ^= 0x01;
    (void)img_stage_sections(&f, NULL, g_save, slot, G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, g_pc);
    check("stage: a changed section IS written and its checksum recomputed",
          sec5[3] == g_pc[3] && gen3_checksum(sec5, G3_SECTOR_DATA_SIZE) ==
          (uint16_t)(sec5[G3_OFF_CHECKSUM] | (sec5[G3_OFF_CHECKSUM + 1] << 8)));
  }
  /* a finalize inside a scope: flush applies the deferred sections; the fold stays immediate */
  memcpy(g_save, g_orig, sizeof g_save); memset(&f, 0, sizeof f);
  memset(sb2, 0x67, sizeof sb2);
  img_scope_open(&r, "commit");
  (void)img_stage_sections(&f, &r, g_save, slot, 0, 0, sb2);
  check("scope: a flush inside an open scope applies the pending sections", img_scope_flush(&f, &r, g_save, slot) &&
        memcmp(g_save + (uint32_t)slot * G3_SLOT_BYTES + (uint32_t)gen3_find_section(g_save, slot, 0) * G3_SECTOR_SIZE, sb2, G3_SECTOR_DATA_SIZE) == 0);
  (void)img_scope_close(&f, &r, g_save, slot);
  return 0;
}

int main(int argc, char** argv) {
  flag_table();
  checksum_pins();
  if (argc > 1 && scope_pins(argv[1])) return 2;
  int used = 0;
  for (int i = 1; i < argc; i++) { if (run_one(argv[i])) return 2; used++; }
  if (!used) printf("SKIP (no save supplied): only the pure flag table ran\n");
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
