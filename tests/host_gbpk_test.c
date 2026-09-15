/* source/gb_pk.c -- the .pk1/.pk2 pure-C packer/unpacker -- under test (BACKLOG #93).
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbpk_test.c \
 *      source/gb_pk.c source/gb_session.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_save.c source/gen3_daycare.c -o /tmp/hgbpk && /tmp/hgbpk
 *
 * Corpus: Guy's own cartridge dumps (gitignored, gba-toolkit/roms/gb/). A missing
 * corpus SKIPS rather than fails. Acceptance (BACKLOG #93 brief): the .pk1/.pk2
 * round-trip (pack -> unpack -> record/OT/nick byte-identical, sizes 55/54, egg
 * refusal) over every occupied slot of the corpus Red/Gold/Crystal saves.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_pk.h"
#include "gb_session.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECKF(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_list[GBS_LIST_BYTES];

static uint32_t load(const char* file) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(g_img, 1, sizeof g_img, f);
  fclose(f);
  return n;
}

/* Every occupied, non-party, non-Egg slot of every box in `file`: pack -> unpack ->
 * memcmp the three raw pieces (rec/otname/nick) byte-identical, and the packed size
 * matches the generation's fixed constant exactly. */
static void roundtrip(const char* file, uint8_t expect_gen) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (gb_pk round trip)\n", file); return; }
  g_ran++;
  printf("  -- gb_pk round trip: %s\n", file);

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK,
         "%s: session opens", file);
  CHECKF(s.gen == expect_gen, "%s: expected generation", file);

  int expect_size = (s.gen == GB_GEN1) ? GB_PK1_BYTES : GB_PK2_BYTES;
  const char* expect_ext = (s.gen == GB_GEN1) ? ".pk1" : ".pk2";
  CHECKF(strcmp(gb_pk_ext(s.gen), expect_ext) == 0, "%s: gb_pk_ext matches generation", file);

  int nb = gbs_nboxes(&s), pb = gbs_party_box(&s);
  int examined = 0;
  for (int b = 0; b < nb; b++) {
    if (b == pb) continue;                             /* box-shaped slots only */
    if (gbs_load_list(&s, b, g_list) != GBS_OK) continue;
    int n = gb_list_count(s.gen, g_list, b);
    for (int slot = 0; slot < n; slot++) {
      GbEditMon e;
      if (!gb_load(&e, s.gen, g_list, b, slot)) continue;
      if (gb_is_egg(&e)) continue;                     /* covered separately below */
      examined++;

      uint8_t buf[GB_PK2_BYTES > GB_PK1_BYTES ? GB_PK2_BYTES : GB_PK1_BYTES];
      int wrote = gb_pk_pack(&e, buf, sizeof buf);
      CHECKF(wrote == expect_size, "%s box %d slot %d: packed size is the fixed constant",
             file, b, slot);
      if (wrote < 0) continue;

      GbEditMon back;
      CHECKF(gb_pk_unpack(buf, wrote, s.gen, &back), "%s box %d slot %d: unpacks",
             file, b, slot);

      int rec_len = gb_rec_size(s.gen, false);
      CHECKF(memcmp(e.rec, back.rec, (size_t)rec_len) == 0,
             "%s box %d slot %d: record bytes round-trip", file, b, slot);
      CHECKF(memcmp(e.otname, back.otname, GB_NAME_BYTES) == 0,
             "%s box %d slot %d: OT name round-trips", file, b, slot);
      CHECKF(memcmp(e.nick, back.nick, GB_NAME_BYTES) == 0,
             "%s box %d slot %d: nickname round-trips", file, b, slot);
      CHECKF(!gb_is_egg(&back), "%s box %d slot %d: unpacked record never reads as an Egg",
             file, b, slot);
    }
  }
  CHECKF(examined > 0, "%s: at least one occupied non-Egg slot was examined", file);
}

/* A synthetic Gen-2 Egg (list_species = G2_LIST_EGG) is refused by gb_pk_pack with -3,
 * and the cap-too-small / bad-generation / NULL paths each get their own distinct
 * negative code -- decision 5's structural refusal, exercised without needing a real
 * corpus Egg (Crystal.sav may or may not have one on any given day). */
static void structural_refusals(void) {
  GbEditMon e;
  memset(&e, 0, sizeof e);
  e.gen = GB_GEN2;
  e.rec_len = (uint8_t)gb_rec_size(GB_GEN2, false);
  e.list_species = G2_LIST_EGG;

  uint8_t buf[GB_PK2_BYTES];
  CHECKF(gb_pk_pack(&e, buf, sizeof buf) == -3, "Egg: gb_pk_pack refuses (-3)");

  e.list_species = 1;   /* Bulbasaur -- no longer an Egg */
  CHECKF(gb_pk_pack(&e, buf, sizeof buf) == GB_PK2_BYTES,
         "non-Egg: gb_pk_pack succeeds once the Egg marker is gone");

  CHECKF(gb_pk_pack(&e, buf, GB_PK2_BYTES - 1) == -2, "cap too small: -2");
  CHECKF(gb_pk_pack(NULL, buf, sizeof buf) == -1, "NULL GbEditMon: -1");
  CHECKF(gb_pk_pack(&e, NULL, sizeof buf) == -1, "NULL out buffer: -1");

  GbEditMon e3 = e; e3.gen = 3;   /* neither GB_GEN1 nor GB_GEN2 */
  CHECKF(gb_pk_pack(&e3, buf, sizeof buf) == -1, "bad generation: -1");

  GbEditMon out;
  CHECKF(!gb_pk_unpack(buf, GB_PK2_BYTES - 1, GB_GEN2, &out),
         "unpack: wrong length is refused");
  CHECKF(!gb_pk_unpack(buf, GB_PK2_BYTES, 3, &out),
         "unpack: bad generation is refused");
  CHECKF(!gb_pk_unpack(NULL, GB_PK2_BYTES, GB_GEN2, &out),
         "unpack: NULL input is refused");

  CHECKF(gb_pk_ext(GB_GEN1) != NULL && strcmp(gb_pk_ext(GB_GEN1), ".pk1") == 0,
         "gb_pk_ext(GB_GEN1) == .pk1");
  CHECKF(gb_pk_ext(GB_GEN2) != NULL && strcmp(gb_pk_ext(GB_GEN2), ".pk2") == 0,
         "gb_pk_ext(GB_GEN2) == .pk2");
  CHECKF(gb_pk_ext(3) == NULL, "gb_pk_ext(unsupported) == NULL");
}

int main(void) {
  printf("gb_pk (.pk1/.pk2 packer, BACKLOG #93)\n");

  structural_refusals();

  roundtrip("Red.sav",     GB_GEN1);
  roundtrip("Yellow.sav",  GB_GEN1);
  roundtrip("Gold.sav",    GB_GEN2);
  roundtrip("Crystal.sav", GB_GEN2);

  if (!g_ran) printf("  (no corpus present -- structural checks only)\n");
  printf("%s: %d/%d checks passed over %d save(s)\n",
         g_fail ? "FAIL" : "ok", g_check - g_fail, g_check, g_ran);
  return g_fail ? 1 : 0;
}
