/* read_party_mail.c -- BACKLOG #228 (lane s179-a4 item 3). Reads a raw Gen-3 save
 * image (a plain .sav -- 128 KiB, both A/B slots) and prints the mail byte
 * (gen3_save.h's G3_PARTY_MAIL_OFF, 0x55) of one party slot, so a host script can
 * assert 0xFF (G3_MAIL_NONE) vs 0x00 without hand-decoding SaveBlock1's own
 * sector/mirror shuffling itself -- this tool reuses the SAME gen3_save.c parser
 * every other host tool in this tree already trusts, not a second implementation.
 *
 * VERIFIED against Guy's own real Emerald.sav (all 6 live party slots, correct
 * levels and mail=0xFF matching the in-game party) -- this half of BACKLOG #228 is
 * DONE. NOT YET WIRED to a live emulator flash dump: mGBA's Python binding maps
 * `core.memory.sram` to a hardcoded 64 KiB window (mgba/gba.py's own static
 * offset/size table), but this project's delta build is a real 128 KiB Flash1M
 * chip (source/flashsave.c's own header comment) -- SaveBlock1's own logical
 * section 1 (which holds the party count/array near offset 0x234) lands OUTSIDE
 * that 64 KiB window on a live boot (confirmed live: the 64 KiB dump held sections
 * 3-13+0 as valid, sections 1/2 blank), so a straight `core.memory.sram` dump is
 * NOT a complete/valid save image for this reader. Getting the OTHER 64 KiB needs
 * a bank-switch command (Flash1M's own 0xAA/0x5555,0x55/0x2AAA,0xB0/0x5555,
 * bank#/0x0000 protocol) written through mGBA's raw memory-write API, which hit an
 * argument-count mismatch this lane could not resolve in its remaining budget --
 * see the lane's own final report for the exact traceback. An emulator-level
 * runner for BACKLOG #228 is NOT delivered by this lane; this tool is the
 * reusable, already-correct host-side half of it.
 *
 * Host-only. Build:
 *   cc -std=c11 -O2 -I ../../source read_party_mail.c \
 *      ../../source/gen3_save.c ../../source/gen3_mon.c ../../source/data_tables.c \
 *      -o /tmp/read_party_mail
 *   /tmp/read_party_mail save.sav SLOT   (SLOT: 0..5; save.sav must be the full
 *   128 KiB image, both A/B slots -- gen3_parse_into() picks the valid/newer one)
 * Prints one line: "slot N: level=L mail=0xXX (...)" to stdout, exits 1 with a
 * message on stderr if the save does not parse or SLOT is out of range for the
 * live party count.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gen3_save.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <flash_dump.bin> <party_slot 0..5>\n", argv[0]);
    return 1;
  }
  const char* path = argv[1];
  int slot = atoi(argv[2]);
  if (slot < 0 || slot > 5) {
    fprintf(stderr, "%s: slot must be 0..5\n", argv[0]);
    return 1;
  }

  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "%s: cannot open %s\n", argv[0], path); return 1; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fprintf(stderr, "%s: %s is empty\n", argv[0], path); fclose(f); return 1; }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf(stderr, "%s: short read on %s\n", argv[0], path);
    fclose(f);
    return 1;
  }
  fclose(f);

  Gen3SaveInfo info;
  uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  if (!gen3_parse_into(buf, (uint32_t)sz, &info, sb1)) {
    fprintf(stderr, "%s: gen3_parse_into failed -- not a valid Gen-3 save image "
                     "(or the flash dump was taken before the app ever wrote a "
                     "save sector)\n", argv[0]);
    free(buf);
    return 1;
  }

  uint8_t count = sb1[SB1_OFF_PARTY_COUNT];
  if (slot >= count) {
    fprintf(stderr, "%s: slot %d requested but live party count is %u\n",
            argv[0], slot, (unsigned)count);
    free(buf);
    return 1;
  }

  const uint8_t* rec = sb1 + 0x238 + (uint32_t)slot * 100u;   /* retail SaveBlock1
    layout: partyCount is a u8 at 0x234 but the party[] array itself starts at
    0x238 (4-byte aligned, matching pokeemerald's struct SaveBlock1 layout) --
    found live after the naive count+1 offset read level=0 for every slot. */
  uint8_t level = rec[0x54];       /* plaintext battle-stats block, gen3_edit.c em_set_party_flag's own offset */
  uint8_t mail = rec[G3_PARTY_MAIL_OFF];

  printf("slot %d: level=%u mail=0x%02X\n", slot, (unsigned)level, (unsigned)mail);
  free(buf);
  return 0;
}
