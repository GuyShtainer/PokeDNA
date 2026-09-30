/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Guy Shtainer
 *
 * plant_gen3_record.c -- BACKLOG #299 probe helper (host-only, not in run_host_tests.py).
 *
 *   plant_gen3_record dump <in.sav> <national_species> <out80.bin>   first box record of that species
 *   plant_gen3_record put  <in.sav> <rec80.bin> <box> <slot> <out.sav>
 *
 * `dump` copies one REAL, checksummed 80-byte box record out of a corpus save verbatim (its
 * metGame byte is whatever the game that caught it wrote -- nothing is forged). `put` writes
 * such a record into a COPY of another save's PC box and re-checksums only the touched
 * PC section(s) through gen3_write_full_section(), then re-verifies every full section.
 * This is how the origin-art probe puts a genuine FireRed-caught mon into an Emerald save.
 *
 *   cc -std=c11 -O1 -I ../source plant_gen3_record.c ../source/gen3_mon.c ../source/gen3_save.c \
 *      ../source/gen3_box.c ../source/gen3_clip.c ../source/gen3_edit.c ../source/gen3_daycare.c \
 *      ../source/data_tables.c -o /tmp/plant_gen3_record
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_box.h"
#include "gen3_mon.h"
#include "gen3_clip.h"

#define REC 80
#define SECT_DATA 3968
#define FULL_PC_SECTIONS 8   /* PC sections 5..12 are full; 13 is partial and refused */

static uint8_t g_save[G3_SAVE_FILE_SIZE];
static uint8_t g_pc[G3_PC_BYTES];
static uint8_t g_pc2[G3_PC_BYTES];

static int load_pc(const char* path, size_t* n, Gen3SaveInfo* info) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "%s: cannot open\n", path); return 0; }
  *n = fread(g_save, 1, sizeof g_save, f);
  fclose(f);
  if (!gen3_parse(g_save, (uint32_t)*n, info) || !info->valid) { fprintf(stderr, "%s: not a Gen-3 save\n", path); return 0; }
  if (!gen3_read_pc_storage(g_save, info->slot, g_pc)) { fprintf(stderr, "%s: PC read failed\n", path); return 0; }
  return 1;
}

static int cmd_dump(char** a) {
  size_t n; Gen3SaveInfo info;
  if (!load_pc(a[0], &n, &info)) return 1;
  unsigned want = (unsigned)atoi(a[1]);
  for (int b = 0; b < G3_TOTAL_BOXES; b++)
    for (int s = 0; s < G3_IN_BOX; s++) {
      const uint8_t* rec = pk_box_slot(g_pc, b, s);
      PkMon m;
      if (!pk_decode_mon(rec, false, &m) || m.isBadEgg || m.isEgg || m.species != want) continue;
      FILE* o = fopen(a[2], "wb");
      if (!o || fwrite(rec, 1, REC, o) != REC) { fprintf(stderr, "write failed\n"); return 1; }
      fclose(o);
      printf("dumped box%d slot%d species=%u metGame=%u -> %s\n", b, s, m.species, m.metGame, a[2]);
      return 0;
    }
  fprintf(stderr, "species %u not found\n", want);
  return 1;
}

static int cmd_put(char** a) {
  size_t n; Gen3SaveInfo info;
  if (!load_pc(a[0], &n, &info)) return 1;
  uint8_t rec[REC];
  FILE* f = fopen(a[1], "rb");
  if (!f || fread(rec, 1, REC, f) != REC) { fprintf(stderr, "%s: need 80 bytes\n", a[1]); return 1; }
  fclose(f);
  int box = atoi(a[2]), slot = atoi(a[3]);
  if (box < 0 || box >= G3_TOTAL_BOXES || slot < 0 || slot >= G3_IN_BOX) { fprintf(stderr, "bad box/slot\n"); return 1; }
  uint8_t* dst = pk_box_slot(g_pc, box, slot);
  uint32_t off = (uint32_t)(dst - g_pc);
  memcpy(dst, rec, REC);
  for (uint32_t i = off / SECT_DATA; i <= (off + REC - 1) / SECT_DATA; i++) {
    if (i >= FULL_PC_SECTIONS) { fprintf(stderr, "record touches the partial last PC section\n"); return 1; }
    if (gen3_write_full_section(g_save, info.slot, 5 + (int)i, g_pc + i * SECT_DATA) == 0xFFFFFFFFu) { fprintf(stderr, "section write failed\n"); return 1; }
  }
  int fail = -2;
  if (!gen3_verify_full_checksums(g_save, info.slot, &fail)) { fprintf(stderr, "checksum verify failed (id %d)\n", fail); return 1; }
  PkMon m;
  if (!gen3_read_pc_storage(g_save, info.slot, g_pc2) || !pk_decode_mon(pk_box_slot(g_pc2, box, slot), false, &m) || m.isBadEgg) { fprintf(stderr, "readback failed\n"); return 1; }
  FILE* o = fopen(a[4], "wb");
  if (!o || fwrite(g_save, 1, n, o) != n) { fprintf(stderr, "write failed\n"); return 1; }
  fclose(o);
  printf("planted box%d slot%d species=%u metGame=%u -> %s\n", box, slot, m.species, m.metGame, a[4]);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 5 && !strcmp(argv[1], "dump")) return cmd_dump(argv + 2);
  if (argc == 7 && !strcmp(argv[1], "put")) return cmd_put(argv + 2);
  fprintf(stderr, "usage: %s dump <in.sav> <species> <out80.bin> | put <in.sav> <rec80.bin> <box> <slot> <out.sav>\n", argv[0]);
  return 2;
}
