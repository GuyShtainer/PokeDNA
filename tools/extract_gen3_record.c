/* extract_gen3_record.c — dump one real, checksummed 80-byte Gen-3 PC box record out
 * of an actual .sav, for tools/fuse_sav.py --clip (D1's PASTE(GB)-on-empty-cell shot).
 *
 * A synthetic/hand-built record would still need the real per-mon encryption + the
 * 16-bit checksum gen3_to_gb()'s pk_decode_mon() checks before showing anything, so
 * this pulls a genuine slot out of Guy's own corpus instead of re-deriving that math.
 * Picks the first non-empty box slot with < 4 moves and a held item (so the shot's
 * loss screen has something in both the "moves truncate" and "item drops" rows) and
 * writes its 80 raw bytes verbatim — no re-encoding, no editing.
 *
 * Host-only, not part of tests/run_host_tests.py. Build:
 *   cc -std=c11 -O2 -I ../../source extract_gen3_record.c \
 *      ../../source/gen3_mon.c ../../source/gen3_save.c ../../source/gen3_box.c \
 *      ../../source/data_tables.c -o /tmp/extract_gen3_record
 *   /tmp/extract_gen3_record /path/to/Emerald.sav out.bin
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_box.h"
#include "gen3_mon.h"

#define PC_OFF_BOXES 0x0004
#define BOX_MON      80

static uint8_t* box_slot(uint8_t* pc, int box, int slot) {
  return pc + PC_OFF_BOXES + (uint32_t)(box * G3_IN_BOX + slot) * BOX_MON;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <gen3.sav> <out80.bin>\n", argv[0]);
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "%s: cannot open\n", argv[1]); return 1; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);

  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info) || !info.valid) {
    fprintf(stderr, "%s: not a valid Gen-3 save\n", argv[1]);
    return 1;
  }

  static uint8_t pc[G3_PC_BYTES];
  if (!gen3_read_pc_storage(save, info.slot, pc)) {
    fprintf(stderr, "%s: could not reassemble PC storage\n", argv[1]);
    return 1;
  }

  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    for (int s = 0; s < G3_IN_BOX; s++) {
      uint8_t* rec = box_slot(pc, b, s);
      PkMon m;
      if (!pk_decode_mon(rec, false, &m)) continue;      /* empty/undecodable slot */
      /* <= 251 (Gen 1/2's own dex): gen3_to_gb() refuses "this species has no Game Boy
       * form" for anything Hoenn-only, and this record exists to reach PASTE (GB)'s
       * loss screen, not that earlier refusal. */
      if (m.species < 1 || m.species > 251) continue;
      if (m.isEgg) continue;
      int nmoves = 0;
      for (int i = 0; i < 4; i++) if (m.moves[i]) nmoves++;
      if (nmoves >= 4) continue;                          /* want a truncation row */
      if (!m.heldItem) continue;                          /* want an item-drop row */

      FILE* out = fopen(argv[2], "wb");
      if (!out) { fprintf(stderr, "%s: cannot write\n", argv[2]); return 1; }
      size_t w = fwrite(rec, 1, BOX_MON, out);
      fclose(out);
      if (w != BOX_MON) { fprintf(stderr, "%s: short write\n", argv[2]); return 1; }
      fprintf(stderr, "box %2d slot %2d: species %3d, %d move(s), held item %d -> %s\n",
              b, s, m.species, nmoves, m.heldItem, argv[2]);
      return 0;
    }
  }
  fprintf(stderr, "%s: no box slot matched (species valid, <4 moves, held item)\n", argv[1]);
  return 1;
}
