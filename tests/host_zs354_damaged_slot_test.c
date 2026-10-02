/* BACKLOG #354 -- prefer the coherent slot over a damaged one (saves the old #353 bug already damaged).
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_zs354_damaged_slot_test.c \
 *      source/gen3_save.c -o /tmp/hzs354 && /tmp/hzs354
 *
 * Fixture: the first 64 KiB of LeafGreen.sav over Ruby.sav (what the pre-#353 build wrote). Slot B is then
 * a mix of two saves (duplicate/missing section ids, mixed counters) with the HIGHER counter, so the old
 * counter rule opened a foreign trainer. Corpus (Guy's own dumps, outside the repo): a missing corpus SKIPS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gen3_save.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"
#define HEAD 0x10000u
#define NG 5

static int g_fail = 0, g_check = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static const char* const NAMES[NG] = {"Emerald", "FireRed", "LeafGreen", "Ruby", "Sapphire"};
static uint8_t sav[NG][G3_SAVE_FILE_SIZE], img[G3_SAVE_FILE_SIZE];
static uint8_t sb1[G3_SAVEBLOCK1_BYTES];

static int slurp(const char* name, uint8_t* dst) {
  char p[512]; snprintf(p, sizeof p, "%s/%s.sav", ROMS, name);
  FILE* f = fopen(p, "rb"); if (!f) return 0;
  size_t n = fread(dst, 1, G3_SAVE_FILE_SIZE, f); fclose(f);
  return n == G3_SAVE_FILE_SIZE;
}
static bool parse(const uint8_t* b, uint32_t sz, Gen3SaveInfo* in) {
  memset(in, 0, sizeof *in);
  return gen3_parse_into(b, sz, in, sb1) && in->valid;
}
static int same_trainer(const Gen3SaveInfo* a, const Gen3SaveInfo* b) {
  return strcmp(a->trainer_name, b->trainer_name) == 0 && a->tid_public == b->tid_public && a->tid_secret == b->tid_secret;
}

int main(void) {
  for (int g = 0; g < NG; g++)
    if (!slurp(NAMES[g], sav[g])) { printf("SKIP (corpus %s.sav not present under %s)\n", NAMES[g], ROMS); return 0; }
  enum { EM, FR, LG, RU, SA };

  /* 1. the reported fixture */
  memcpy(img, sav[RU], sizeof img); memcpy(img, sav[LG], HEAD);
  Gen3SaveInfo lg64, d;
  CHECK(parse(sav[LG], HEAD, &lg64), "LeafGreen 64K head parses");
  CHECK(parse(img, G3_SAVE_FILE_SIZE, &d), "damaged image parses");
  CHECK(strcmp(d.trainer_name, "poop") == 0, "damaged image opens on poop, got '%s'", d.trainer_name);
  CHECK(d.damaged_fallback, "damaged_fallback set");
  CHECK(d.slot == 0 && !d.slot_damaged[0] && d.slot_damaged[1], "slot A intact/chosen, slot B flagged (slot %d dmg %d%d)", d.slot, d.slot_damaged[0], d.slot_damaged[1]);
  CHECK(d.counter[1] > d.counter[0], "fixture really has the higher damaged counter (%u vs %u)", (unsigned)d.counter[1], (unsigned)d.counter[0]);
  printf("  fixture: slot=%d ctr=[%u,%u] name=%s fallback=%d\n", d.slot, (unsigned)d.counter[0], (unsigned)d.counter[1], d.trainer_name, d.damaged_fallback);

  /* 2. pristine corpus: counter rule unchanged, nothing flagged */
  for (int g = 0; g < NG; g++) {
    Gen3SaveInfo in;
    CHECK(parse(sav[g], G3_SAVE_FILE_SIZE, &in), "%s parses", NAMES[g]);
    int want = (in.counter[1] > in.counter[0]) ? 1 : 0;
    CHECK(in.slot == want, "%s: slot %d, counter rule says %d", NAMES[g], in.slot, want);
    CHECK(!in.damaged_fallback && !in.slot_damaged[0] && !in.slot_damaged[1], "%s: flags clear (%d%d fb %d)", NAMES[g], in.slot_damaged[0], in.slot_damaged[1], in.damaged_fallback);
    CHECK(gen3_slot_consistent(sav[g], G3_SAVE_FILE_SIZE, 0) && gen3_slot_consistent(sav[g], G3_SAVE_FILE_SIZE, 1), "%s: both slots consistent", NAMES[g]);
    Gen3SaveInfo h;   /* 64 KiB view: slot B absent, never flagged */
    CHECK(parse(sav[g], HEAD, &h) && h.slot == 0 && !h.damaged_fallback && !h.slot_damaged[1], "%s 64K: blank/absent slot B not flagged", NAMES[g]);
  }

  /* 3. hand-built torn slot: bump ONE sector's counter in the newer slot */
  for (int g = 0; g < NG; g++) {
    Gen3SaveInfo in, t;
    parse(sav[g], G3_SAVE_FILE_SIZE, &in);
    memcpy(img, sav[g], sizeof img);
    uint32_t off = (uint32_t)in.slot * G3_SLOT_BYTES + 5u * G3_SECTOR_SIZE + G3_OFF_COUNTER;
    img[off]++;
    CHECK(parse(img, G3_SAVE_FILE_SIZE, &t), "%s torn parses", NAMES[g]);
    CHECK(t.slot_damaged[in.slot] && !t.slot_damaged[1 - in.slot], "%s torn: newer slot flagged only", NAMES[g]);
    CHECK(t.slot == 1 - in.slot && t.damaged_fallback, "%s torn: falls back to the older intact slot (slot %d)", NAMES[g], t.slot);
  }

  /* 3b. duplicate id with ONE counter throughout (counters alone cannot see it): sector 5 relabelled 6 */
  for (int g = 0; g < NG; g++) {
    Gen3SaveInfo in, t;
    parse(sav[g], G3_SAVE_FILE_SIZE, &in);
    memcpy(img, sav[g], sizeof img);
    uint32_t off = (uint32_t)in.slot * G3_SLOT_BYTES + 5u * G3_SECTOR_SIZE + G3_OFF_ID;
    CHECK(img[off] == 5 || img[off] == 6 || img[off] < 14, "%s: id byte located", NAMES[g]);
    img[off] = 6;   /* little-endian low byte; high byte stays 0 */
    CHECK(parse(img, G3_SAVE_FILE_SIZE, &t), "%s dup-id parses", NAMES[g]);
    CHECK(!gen3_slot_consistent(img, G3_SAVE_FILE_SIZE, in.slot), "%s dup-id: slot not consistent", NAMES[g]);
    CHECK(t.slot == 1 - in.slot && t.damaged_fallback, "%s dup-id: falls back (slot %d)", NAMES[g], t.slot);
  }

  /* 4. all 20 head-over-tail pairs open on the head's trainer */
  int pairs = 0;
  for (int h = 0; h < NG; h++) for (int t = 0; t < NG; t++) {
    if (h == t) continue;
    Gen3SaveInfo ref, o;
    memcpy(img, sav[t], sizeof img); memcpy(img, sav[h], HEAD);
    CHECK(parse(sav[h], HEAD, &ref) && parse(img, G3_SAVE_FILE_SIZE, &o), "%s over %s parses", NAMES[h], NAMES[t]);
    CHECK(same_trainer(&ref, &o) && o.slot == 0, "%s head over %s tail: opened '%s' slot %d, want '%s'", NAMES[h], NAMES[t], o.trainer_name, o.slot, ref.trainer_name);
    pairs++;
  }
  CHECK(pairs == 20, "20 pairs");

  printf("host_zs354: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
