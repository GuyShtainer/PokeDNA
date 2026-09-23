/* mail_inject.c -- BACKLOG #227's shot-chain fixture prep. Given a raw 128 KiB Gen-3
 * save image, sets ONE party slot's held item to a Mail id (121..132, ORANGE MAIL..
 * RETRO MAIL, source/data_tables.c) through the SAME gen3_edit.c/gen3_save.c pipeline
 * every other host tool in this tree trusts (em_set_item + gen3_write_full_section,
 * the exact pattern tests/host_contest_test.c's own full-checksum round trip already
 * proves works), then writes a new save image with that ONE record changed. Reused as
 * the fixture step before tools/g3_shots.py's run_b227_party_mail(): #227's refusal
 * needs a REAL party member holding Mail sitting in the panel's seeded swap slot
 * (index 1, the same slot tools/party_mail_e2e.py's SWAP recipe already displaces) --
 * this tool is what puts it there, without hand-editing an encrypted/checksummed
 * substructure by hex offset.
 *
 * Host-only. Build:
 *   cc -std=c11 -O2 -I ../../source mail_inject.c \
 *      ../../source/gen3_save.c ../../source/gen3_mon.c ../../source/gen3_edit.c \
 *      ../../source/gen3_ivroll.c ../../source/gen3_pidiv.c ../../source/gen3_gen.c \
 *      ../../source/gen3_daycare.c ../../source/data_tables.c \
 *      -o /tmp/mail_inject
 *   /tmp/mail_inject SRC.sav SLOT ITEM_ID OUT.sav
 *     SLOT: 0..5 (live party index); ITEM_ID: 121..132 for a Mail id (any u16 works,
 *     so 0 restores "no mail" if ever needed for a follow-up fixture).
 * Prints "wrote OUT.sav: slot N held item now 0xXXXX, section checksums OK" and
 * exits 0, or an error to stderr and exits 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_edit.h"

int main(int argc, char** argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s <src.sav> <party_slot 0..5> <item_id> <out.sav>\n", argv[0]);
    return 1;
  }
  const char* src_path = argv[1];
  int slot = atoi(argv[2]);
  long item_id = strtol(argv[3], NULL, 0);
  const char* out_path = argv[4];
  if (slot < 0 || slot > 5) { fprintf(stderr, "%s: slot must be 0..5\n", argv[0]); return 1; }
  if (item_id < 0 || item_id > 0xFFFF) { fprintf(stderr, "%s: item id out of u16 range\n", argv[0]); return 1; }

  FILE* f = fopen(src_path, "rb");
  if (!f) { fprintf(stderr, "%s: cannot open %s\n", argv[0], src_path); return 1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fprintf(stderr, "%s: %s is empty\n", argv[0], src_path); fclose(f); return 1; }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf(stderr, "%s: short read on %s\n", argv[0], src_path);
    fclose(f);
    return 1;
  }
  fclose(f);

  Gen3SaveInfo info;
  uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  if (!gen3_parse_into(buf, (uint32_t)sz, &info, sb1)) {
    fprintf(stderr, "%s: gen3_parse_into failed -- not a valid Gen-3 save image\n", argv[0]);
    free(buf);
    return 1;
  }

  uint8_t count = sb1[SB1_OFF_PARTY_COUNT];
  if (slot >= count) {
    fprintf(stderr, "%s: slot %d requested but live party count is %u\n", argv[0], slot, (unsigned)count);
    free(buf);
    return 1;
  }

  uint8_t* rec = sb1 + 0x238 + (uint32_t)slot * 100u;   /* same retail SaveBlock1 layout
    read_party_mail.c already verified against Guy's own save: partyCount is a u8 at
    0x234, party[] itself starts at 0x238 (4-byte aligned). */

  EditMon e;
  gen3_edit_load(rec, true, &e);
  em_set_item(&e, (uint16_t)item_id);
  uint8_t out100[100];
  gen3_edit_commit(&e, out100);
  memcpy(rec, out100, 100);

  /* Sanity check: re-decode the record the SAME way g3_party_rec_has_mail() (source/
   * pdna_main.c) will -- pk_decode_mon's heldItem, not a re-read of the raw bytes --
   * so this tool's own fixture is proven to trip the exact predicate under test. */
  PkMon check;
  if (!pk_decode_mon(rec, true, &check) || check.heldItem != (uint16_t)item_id) {
    fprintf(stderr, "%s: post-write pk_decode_mon heldItem mismatch (want 0x%04X, got %s)\n",
            argv[0], (unsigned)item_id,
            pk_decode_mon(rec, true, &check) ? "different value" : "decode failure");
    free(buf);
    return 1;
  }

  uint8_t* save_copy = (uint8_t*)malloc((size_t)sz);
  if (!save_copy) { fprintf(stderr, "%s: OOM\n", argv[0]); free(buf); return 1; }
  memcpy(save_copy, buf, (size_t)sz);
  for (int id = 1; id <= 4; id++)
    gen3_write_full_section(save_copy, info.slot, id, sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE);

  int fail_id = -1;
  if (!gen3_verify_full_checksums(save_copy, info.slot, &fail_id)) {
    fprintf(stderr, "%s: post-write checksum verify FAILED on section %d -- not writing %s\n",
            argv[0], fail_id, out_path);
    free(buf);
    free(save_copy);
    return 1;
  }

  FILE* fo = fopen(out_path, "wb");
  if (!fo || fwrite(save_copy, 1, (size_t)sz, fo) != (size_t)sz) {
    fprintf(stderr, "%s: failed to write %s\n", argv[0], out_path);
    if (fo) fclose(fo);
    free(buf);
    free(save_copy);
    return 1;
  }
  fclose(fo);

  printf("wrote %s: slot %d held item now 0x%04X, section checksums OK\n",
         out_path, slot, (unsigned)item_id);
  free(buf);
  free(save_copy);
  return 0;
}
