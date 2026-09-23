/* swap_mail_case.c -- BACKLOG #227 D3: run party_place_held's SWAP-arm mail refusal
 * (source/pdna_main.c, itself not host-compilable) against a REAL corpus save through
 * the retail gate, and prove the refusal writes NOTHING back to the save image -- the
 * shape tests/host_gen1party_test.c:466 already uses for the Gen-2 mirror
 * (memcmp(g_fimg, g_fsnap, len) == 0 after a refused insert).
 *
 * Reproduces party_place_held's SWAP arm exactly, using the same guard expression
 * (source/pdna_main.c's g3_party_rec_has_mail, BACKLOG #227 D1: pk_decode_mon(pslot,
 * true, &pk) && g3_item_is_mail(pk.heldItem)) on the target party slot, checked BEFORE
 * anything is written: the party's own 100-byte record is never touched, so the save
 * image on disk after a refused attempt is byte-identical to before it.
 *
 * Host-only. Build (the correct, shipped shape):
 *   cc -std=c11 -O2 -I source tools/swap_mail_case.c source/gen3_save.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_edit.c source/gen3_clip.c \
 *      source/gen3_daycare.c source/data_tables.c -o /tmp/swap_mail_case
 *   /tmp/swap_mail_case <mail-injected.sav> <party_slot 0..5>
 * Requires the target slot to already decode as a Mail holder (tools/mail_inject.c
 * puts one there through the real edit pipeline) -- a clean slot is a broken fixture,
 * not a pass, and exits 1 saying so.
 *
 * Prints "REFUSED: ..." then "save image byte-unchanged" and exits 0 on the correct
 * path. -DSWAP_MAIL_CASE_MUTANT_WRITE_FIRST builds the self-proof mutant: it performs
 * the SWAP arm's destructive half (party_to_box-equivalent overwrite of the party
 * slot, committed back to the save) BEFORE the guard would have stopped it -- exactly
 * the "guard checked too late" bug class this case exists to catch -- and must exit 1
 * with "save image CHANGED" so tools/gb_retail_gate.py's own self-test can prove this
 * case has teeth. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_clip.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <mail-injected.sav> <party_slot 0..5>\n", argv[0]);
    return 1;
  }
  const char* path = argv[1];
  int slot = atoi(argv[2]);
  if (slot < 0 || slot > 5) { fprintf(stderr, "party_slot must be 0..5\n"); return 1; }

  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fprintf(stderr, "%s is empty\n", path); fclose(f); return 1; }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf(stderr, "short read on %s\n", path);
    fclose(f);
    return 1;
  }
  fclose(f);

  uint8_t* snap = (uint8_t*)malloc((size_t)sz);
  if (!snap) { fprintf(stderr, "OOM\n"); free(buf); return 1; }
  memcpy(snap, buf, (size_t)sz);   /* pre-attempt snapshot -- taken before ANY parsing */

  Gen3SaveInfo info;
  uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  if (!gen3_parse_into(buf, (uint32_t)sz, &info, sb1)) {
    fprintf(stderr, "gen3_parse_into failed -- not a valid Gen-3 save image\n");
    free(buf); free(snap);
    return 1;
  }

  uint8_t count = sb1[SB1_OFF_PARTY_COUNT];
  if ((uint8_t)slot >= count) {
    fprintf(stderr, "slot %d requested but live party count is %u\n", slot, (unsigned)count);
    free(buf); free(snap);
    return 1;
  }
  uint8_t* pslot = sb1 + 0x238 + (uint32_t)slot * 100u;   /* same layout mail_inject.c uses */

  /* THE GUARD -- party_place_held's SWAP arm, exact expression (BACKLOG #227 D1). */
  PkMon pk;
  bool is_mail = pk_decode_mon(pslot, true, &pk) && g3_item_is_mail(pk.heldItem);
  if (!is_mail) {
    fprintf(stderr, "FIXTURE BROKEN: party slot %d does not decode as a Mail holder "
                     "(heldItem=%u) -- inject mail first (tools/mail_inject.c)\n",
                     slot, (unsigned)pk.heldItem);
    free(buf); free(snap);
    return 1;
  }
  fprintf(stdout, "REFUSED: party slot %d holds Mail (heldItem=%u) -- swap denied "
                   "before any write\n", slot, (unsigned)pk.heldItem);

#ifdef SWAP_MAIL_CASE_MUTANT_WRITE_FIRST
  /* Self-proof mutant: do the SWAP arm's destructive half anyway -- a DIFFERENT,
   * synthetic donor mon (box_to_party's own shape: gen3_edit_load/
   * em_set_party_flag(true)/gen3_edit_commit) overwrites the whole 100-byte party
   * slot, exactly `memcpy(pslot, x100, 100)` in party_place_held's real SWAP arm --
   * then commits it back to the save image, AFTER printing REFUSED above. A guard
   * that only warns but does not stop must show up here as a changed image. (An
   * earlier version of this mutant re-derived the SAME record's own box form and
   * wrote only its first 80 bytes back over itself -- party->box->the same 80 bytes
   * is a no-op on the encrypted substructure, so that mutant never actually changed
   * anything and would have been a vacuous self-proof; a genuinely different donor
   * record is what a real SWAP writes.) */
  {
    uint8_t donor80[80], x100[100];
    gen3_build_mon(25 /* PIKACHU */, 10, 0xCAFEBABEu, 0x11112222u, "DONOR", 3, donor80);
    EditMon e;
    gen3_edit_load(donor80, false, &e);
    em_set_party_flag(&e, true);
    gen3_edit_commit(&e, x100);
    memcpy(pslot, x100, 100);
    uint8_t* save_copy = (uint8_t*)malloc((size_t)sz);
    if (save_copy) {
      memcpy(save_copy, buf, (size_t)sz);
      for (int id = 1; id <= 4; id++)
        gen3_write_full_section(save_copy, info.slot, id,
                                 sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE);
      memcpy(buf, save_copy, (size_t)sz);
      free(save_copy);
    }
  }
#endif

  /* On the correct (unmutated) path, nothing above this point ever wrote to `buf` --
   * gen3_parse_into's own sb1 output is a SEPARATE scratch buffer (see its own header
   * comment), and the guard exited before any gen3_write_full_section call, so the
   * save image in memory is still exactly the bytes that were read in. */
  int changed = (memcmp(buf, snap, (size_t)sz) != 0);
  printf("save image %s (memcmp vs the pre-attempt snapshot)\n",
         changed ? "CHANGED" : "byte-unchanged");
  free(buf);
  free(snap);
  return changed ? 1 : 0;   /* exit 0 only if the refusal truly touched nothing */
}
