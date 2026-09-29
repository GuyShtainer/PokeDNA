/* #280 (lane y9-280): host tool for the target-drop restore chains (tools/dgb_shots.py --y9-*).
 *   y9_mkledger ledger N OUT.pds   write case N's ledger file (bank_plant_y9_ledger) to OUT.pds;
 *                                  stdout: the on-card path (/PokeDNA/xfer/<key>.pds)
 *   y9_mkledger cell N OUT.bin     write case N's Bank cell (80 bytes)
 * Built by the chain with the y7_plantcmp source list + -DPDNA_DELTA. Exit 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "bank_plant.h"
#include "gb_sidecar.h"

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  int which = atoi(argv[2]);
  FILE* f;
  if (strcmp(argv[1], "ledger") == 0) {
    uint8_t file[GBSC_FILE_MAX]; uint64_t key = 0;
    int len = bank_plant_y9_ledger(which, file, sizeof file, &key);
    if (len <= 0) return 1;
    char path[GBSC_PATH_MAX];
    if (gbsc_path(path, sizeof path, "/PokeDNA/xfer", key) < 0) return 1;
    f = fopen(argv[3], "wb"); if (!f) return 2;
    fwrite(file, 1, (size_t)len, f); fclose(f);
    printf("%s\n", path);
    return 0;
  }
  if (strcmp(argv[1], "cell") == 0) {
    uint8_t cell[80];
    if (!bank_plant_y9_cell(which, cell)) return 1;
    f = fopen(argv[3], "wb"); if (!f) return 2;
    fwrite(cell, 1, 80, f); fclose(f);
    return 0;
  }
  return 2;
}
