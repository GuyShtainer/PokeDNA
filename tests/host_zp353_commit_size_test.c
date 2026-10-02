/* BACKLOG #353 -- a 64 KiB save must be committed as 64 KiB, not as 128 KiB with the PREVIOUS save's tail.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_zp353_commit_size_test.c \
 *      source/gen3_save.c -o /tmp/hzp353 && /tmp/hzp353
 *
 * Models the three states of g_save after view_save loads a 64 KiB dump over a buffer that still holds a
 * DIFFERENT 128 KiB save (the real g_save is EWRAM and sf_read_full never touches the unread tail):
 *   (a) OLD  -- write all G3_SAVE_FILE_SIZE bytes: the stale tail becomes slot B, the reopen picks another trainer;
 *   (b) NEW  -- write g_save_size bytes: reopen gives the same trainer, slot A byte-identical;
 *   (c) PAD  -- the load-time 0xFF backstop: the tail holds no foreign bytes (a padded 128 KiB parse never names another trainer).
 * Corpus: Guy's own dumps (gitignored, outside the repo). LeafGreen's first 64 KiB is the fixture because
 * its head parses as an active slot A, and a stale Ruby tail makes slot 1 (counters [8,9]) win in the OLD world.
 * A missing corpus SKIPS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gen3_save.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"
#define HEAD 0x10000u

static int g_fail = 0, g_check = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t fix[G3_SAVE_FILE_SIZE], stale[G3_SAVE_FILE_SIZE], gsave[G3_SAVE_FILE_SIZE], disk[G3_SAVE_FILE_SIZE];
static uint8_t sb1[G3_SAVEBLOCK1_BYTES];

static int slurp(const char* name, uint8_t* dst) {
  char p[512]; snprintf(p, sizeof p, "%s/%s", ROMS, name);
  FILE* f = fopen(p, "rb"); if (!f) return 0;
  size_t n = fread(dst, 1, G3_SAVE_FILE_SIZE, f); fclose(f);
  return n == G3_SAVE_FILE_SIZE;
}
static bool parse(const uint8_t* img, uint32_t sz, Gen3SaveInfo* in) {
  memset(in, 0, sizeof *in);
  return gen3_parse_into(img, sz, in, sb1) && in->valid;
}

int main(void) {
  if (!slurp("LeafGreen.sav", fix) || !slurp("Ruby.sav", stale)) {
    printf("SKIP (corpus LeafGreen.sav/Ruby.sav not present under %s)\n", ROMS);
    return 0;
  }
  Gen3SaveInfo ref;
  CHECK(parse(fix, HEAD, &ref) && ref.slot == 0, "fixture head must parse as an active slot A at 64 KiB");

  /* g_save after the load: stale 128 KiB, then the 64 KiB dump read over its head. */
  memcpy(gsave, stale, sizeof gsave);
  memcpy(gsave, fix, HEAD);

  /* (a) OLD: the whole cap goes to the card. */
  Gen3SaveInfo old;
  bool old_ok = parse(gsave, G3_SAVE_FILE_SIZE, &old);
  int old_same = old_ok && old.slot == ref.slot && strcmp(old.trainer_name, ref.trainer_name) == 0 &&
                 old.tid_public == ref.tid_public && old.tid_secret == ref.tid_secret;
  CHECK(!old_same, "OLD behaviour must NOT reproduce the fixture trainer (test would not bite): slot %d name %s", old.slot, old.trainer_name);
  printf("  old (128K write): ok=%d slot=%d ctr=[%u,%u] name=%s (fixture: %s)\n", old_ok, old.slot,
         (unsigned)old.counter[0], (unsigned)old.counter[1], old.trainer_name, ref.trainer_name);

  /* (b) NEW: exactly g_save_size bytes reach the card. */
  uint32_t g_save_size = HEAD;
  memset(disk, 0xEE, sizeof disk); memcpy(disk, gsave, g_save_size);
  Gen3SaveInfo nw;
  CHECK(parse(disk, g_save_size, &nw), "NEW image must parse");
  CHECK(nw.slot == ref.slot && strcmp(nw.trainer_name, ref.trainer_name) == 0 &&
        nw.tid_public == ref.tid_public && nw.tid_secret == ref.tid_secret,
        "NEW image: same trainer/slot as the fixture (slot %d name %s)", nw.slot, nw.trainer_name);
  CHECK(memcmp(disk, fix, G3_SLOT_BYTES) == 0, "NEW image: slot A byte-identical to the fixture");
  CHECK(memcmp(disk, fix, HEAD) == 0, "NEW image: all %u written bytes equal the fixture head", (unsigned)HEAD);

  /* (c) PAD backstop: view_save's memset(g_save + sz, 0xFF, cap - sz). Padding ALONE does not restore the old
   * reading (the dump's own slot-B sectors 14/15 still carry a valid signature and a higher counter, so a
   * 128 KiB parse picks the half-erased slot 1 and reports unreadable) -- it only guarantees that NO FOREIGN
   * TRAINER'S BYTES survive in the tail. That is what is pinned: every tail byte is 0xFF, and a 128 KiB parse
   * of the padded image never yields a trainer other than the fixture's. */
  memset(gsave + g_save_size, 0xFF, G3_SAVE_FILE_SIZE - g_save_size);
  int tail_ff = 1;
  for (uint32_t i = g_save_size; i < G3_SAVE_FILE_SIZE; i++) if (gsave[i] != 0xFF) { tail_ff = 0; break; }
  CHECK(tail_ff, "padded tail is all 0xFF");
  CHECK(memcmp(gsave, fix, HEAD) == 0, "pad leaves the loaded head untouched");
  Gen3SaveInfo pd;
  bool pd_ok = parse(gsave, G3_SAVE_FILE_SIZE, &pd);
  CHECK(!pd_ok || (strcmp(pd.trainer_name, ref.trainer_name) == 0 && pd.tid_public == ref.tid_public),
        "padded 128K parse must not name a foreign trainer (name %s)", pd.trainer_name);
  printf("  padded 128K parse: ok=%d slot=%d ctr=[%u,%u]\n", pd_ok, pd.slot, (unsigned)pd.counter[0], (unsigned)pd.counter[1]);

  printf("host_zp353: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
