/* fusedgb_probe.c -- BACKLOG #68b review D1's own probe, kept as a permanent test
 * helper: loads a built fused .gba, points fused_gb.c's FUSED_GB_TEST harness at its
 * REAL bytes (not a synthetic fixture), and prints what the C reader itself sees --
 * used by tests/host_fusegb_test.py to prove the real delta-gb recipe (Red+Gold+
 * Crystal, ROM+SAV+LOC each) round-trips through the ACTUAL runtime parser, not just
 * fuse_gb.py's own Python-side verification (the review's whole point: --check can
 * look consistent while the C reader still silently drops entries past its cache cap).
 *
 *   cc -std=c11 -Wall -DPDNA_DELTA -DFUSED_GB_TEST -I source \
 *      tests/fusedgb_probe.c source/fused_gb.c source/pdna_romver.c -o probe
 *   ./probe <fused.gba> <rec_off_decimal>
 *
 * Prints one line: "entry_count=<n> save_count=<n> saves=<name1>,<name2>,..."
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fused_gb.h"

int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: probe <fused.gba> <rec_off>\n"); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { perror("fopen"); return 2; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf(stderr, "read failed\n");
    return 2;
  }
  fclose(f);

  long rec_off = strtol(argv[2], NULL, 10);
  if (rec_off < 0 || rec_off + 16 > sz) { fprintf(stderr, "bad rec_off\n"); return 2; }

  uint32_t off, size;
  memcpy(&off, buf + rec_off + 8, 4);
  memcpy(&size, buf + rec_off + 12, 4);

  fused_gb_test_reset();
  g_fused_gb_test_base = buf;
  memcpy((void*)g_pdna_gbd.magic, "PDNAGBD1", 8);
  g_pdna_gbd.offset = off;
  g_pdna_gbd.size = size;

  int ec = fused_gb_entry_count();
  int sc = fused_gb_save_count();
  printf("entry_count=%d save_count=%d saves=", ec, sc);
  for (int i = 0; i < sc; i++) {
    const char* name = 0;
    fused_gb_save(i, &name, 0, 0);
    printf("%s%s", i ? "," : "", name ? name : "?");
  }
  printf("\n");
  return 0;
}
