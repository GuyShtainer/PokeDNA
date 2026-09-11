/* Throwaway D10 verification probe (BACKLOG #96) -- reads GbTrainer.has_pokedex
 * off a real save file and prints it.
 *
 *   cc -std=c11 -I source tests/d10_has_pokedex_probe.c source/gb_session.c \
 *      source/gb_edit.c source/gen1_save.c source/gen1_write.c source/gen2_save.c \
 *      source/gen2_write.c source/gb_trainer.c source/gb_fields.c \
 *      -o /tmp/d10probe && /tmp/d10probe <save>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb_session.h"
#include "gb_trainer.h"

static uint8_t g_img[65536];
static uint8_t g_scratch[4096];

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <save>\n", argv[0]); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "open fail\n"); return 2; }
  size_t len = fread(g_img, 1, sizeof g_img, f);
  fclose(f);

  GbSession s;
  if (gbs_open(&s, g_img, (uint32_t)len, g_scratch, sizeof g_scratch) != GBS_OK) {
    fprintf(stderr, "gbs_open failed\n"); return 1;
  }
  GbTrainer t; memset(&t, 0, sizeof t);
  if (!gbt_read(&s, &t)) { fprintf(stderr, "gbt_read failed\n"); return 1; }
  printf("gen=%d has_pokedex=%d dex_owned=%u\n", s.gen, (int)t.has_pokedex, t.dex_owned);
  return 0;
}
