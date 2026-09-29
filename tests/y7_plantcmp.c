/* #270 (lane y7-bankpass): host byte-check used by tools/dgb_shots.py's run_y7_bank_passthrough.
 * argv[1] = a box00.box pulled out of the --vsd image. Exit 0 iff bank slot 24 holds the
 * planted slot-29 record (bank_plant_xfer_seed_all()[0]) byte-for-byte -- i.e. the PC->Bank
 * drop stored the Gen-3 record AS IT WAS (pass-through), no native cell, no merge.
 * Built by the chain with the host_bankcell_test.c source list + -DPDNA_DELTA. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "bank_plant.h"
int main(int argc, char** argv) {
  if (argc < 2) return 2;
  uint8_t g3[4][80]; int n = bank_plant_xfer_seed_all(g3);
  FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
  uint8_t box[2400]; size_t got = fread(box, 1, 2400, f); fclose(f);
  if (n < 1 || got != 2400) return 2;
  int same = memcmp(box + 24 * 80, g3[0], 80) == 0;
  printf("bank slot 24 == planted slot-29 record (all 80 bytes): %s\n", same ? "YES" : "NO");
  return same ? 0 : 1;
}
