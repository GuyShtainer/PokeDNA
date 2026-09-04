/* source/gb_editor.c — the GB mon editor's FIELD MODEL — under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbeditor_test.c \
 *      source/gb_editor.c source/gb_session.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      -o /tmp/hgbe && /tmp/hgbe
 *
 * Drives every editor row over every occupied slot of Guy's real Game Boy saves,
 * exactly the way the screen will (gbe_adjust / gbe_press / gbe_set_text /
 * gbe_set_move), and checks the four things a row must never get wrong:
 *
 *   1. "changed" is TRUE iff the record's bytes actually moved — a screen repaints and
 *      arms the write on that bool, so a lie either way is a real bug;
 *   2. an adjust never leaves the record un-committable: gb_commit_checked into a copy
 *      of the box list succeeds and the copy differs from the original ONLY in that
 *      slot's own bytes (the model must not reach outside gb_edit's setters);
 *   3. a name read off the record and handed straight back changes ZERO bytes (the
 *      NAMES guarantee, exercised through the row the screen actually uses);
 *   4. the refusals refuse: an over-range move id, a duplicate move, an empty move
 *      slot's PP, the derived HP DV.
 *
 * The saves are read only; every mutation happens on a stack copy. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_editor.h"
#include "gb_session.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check, g_fail;
#define CHECK(c, msg) do { g_check++; if (!(c)) { printf("  !! FAIL: %s\n", (msg)); g_fail++; } } while (0)

static uint8_t g_img[65536];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_list[GBS_LIST_BYTES];
static uint8_t g_work[GBS_LIST_BYTES];

static bool load_file(const char* name, uint32_t* len) {
  char path[256];
  snprintf(path, sizeof path, "%s/%s", ROMS, name);
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  size_t n = fread(g_img, 1, sizeof g_img, f);
  fclose(f);
  *len = (uint32_t)n;
  return n > 0;
}

/* Every byte outside this slot's four pieces is untouched. */
static bool only_slot_differs(uint8_t gen, int box, int slot, const uint8_t* a, const uint8_t* b) {
  int n = gb_list_size(gen, box);
  int rs = gb_rec_size(gen, gb_box_is_party(gen, box));
  int os = gb_off_species(gen, box, slot), orc = gb_off_record(gen, box, slot);
  int oo = gb_off_otname(gen, box, slot), on = gb_off_nickname(gen, box, slot);
  for (int i = 0; i < n; i++) {
    if (a[i] == b[i]) continue;
    if (i == os) continue;
    if (i >= orc && i < orc + rs) continue;
    if (i >= oo && i < oo + GB_NAME_BYTES) continue;
    if (i >= on && i < on + GB_NAME_BYTES) continue;
    return false;
  }
  return true;
}

static int g_slots, g_rows, g_changes;

static void one_row(const GbEditMon* base, int box, int slot, int f) {
  GbEditMon e = *base;
  uint8_t gen = e.gen;
  int kind = gbe_kind(f);
  char v0[GBE_VALUE_MAX], v1[GBE_VALUE_MAX];
  gbe_value(&e, f, v0, sizeof v0);
  CHECK(v0[0] != 0, "every row has a value");
  g_rows++;

  if (kind == GBE_K_SHOW) {
    CHECK(!gbe_adjust(&e, f, 1, false) && !gbe_press(&e, f), "a read-only row refuses");
    CHECK(memcmp(&e, base, sizeof e) == 0, "...and changes nothing");
    return;
  }
  if (kind == GBE_K_TEXT) {
    /* 3: the NAMES guarantee through the screen's own path */
    GbEditMon t = *base;
    char fb[GB_GLYPH_MAX];
    bool ok = gbe_set_text(&t, f, v0, fb);
    CHECK(ok, "a name read off the record is accepted back");
    CHECK(memcmp(t.rec, base->rec, GB_MAX_REC) == 0 && memcmp(t.nick, base->nick, GB_NAME_BYTES) == 0
          && memcmp(t.otname, base->otname, GB_NAME_BYTES) == 0,
          "...and changes ZERO bytes");
    return;
  }

  /* 1 + 2: each direction, small and big, then A */
  for (int pass = 0; pass < 5; pass++) {
    GbEditMon t = *base;
    bool changed = (pass < 4) ? gbe_adjust(&t, f, (pass & 1) ? 1 : -1, pass >= 2)
                              : gbe_press(&t, f);
    bool moved = memcmp(t.rec, base->rec, GB_MAX_REC) != 0
              || memcmp(t.nick, base->nick, GB_NAME_BYTES) != 0
              || memcmp(t.otname, base->otname, GB_NAME_BYTES) != 0
              || t.list_species != base->list_species;
    CHECK(changed == moved, "'changed' agrees with the bytes");
    if (!changed) continue;
    g_changes++;
    gbe_value(&t, f, v1, sizeof v1);
    CHECK(strcmp(v0, v1) != 0, "a change is visible in the row's text");
    memcpy(g_work, g_list, GBS_LIST_BYTES);
    CHECK(gb_commit_checked(&t, g_work, box, slot), "an adjusted record commits into the list");
    CHECK(only_slot_differs(gen, box, slot, g_list, g_work), "...touching only its own slot");
  }
}

static void one_slot(GbSession* s, int box, int slot) {
  GbEditMon e;
  if (!gb_load(&e, s->gen, g_list, box, slot)) { CHECK(0, "gb_load"); return; }
  g_slots++;
  uint8_t rows[GBE_NUM];
  int n = gbe_fields(&e, rows);
  CHECK(n == (s->gen == GB_GEN2 ? GBE_NUM : GBE_NUM - 2), "Gen 1 hides the two Gen-2 rows");
  char hdr[64];
  gbe_header(&e, hdr, sizeof hdr);
  CHECK(strstr(hdr, "Lv") != 0, "the header names a level");
  if (gb_is_egg(&e))
    CHECK(strcmp(gbe_label_of(&e, GBE_FRIEND), "Egg cycles") == 0,
          "an egg's friendship row is labelled as the hatch counter");
  else
    CHECK(strcmp(gbe_label_of(&e, GBE_FRIEND), gbe_label(GBE_FRIEND)) == 0,
          "a hatched mon keeps the Friendship label");
  for (int i = 0; i < n; i++) one_row(&e, box, slot, rows[i]);

  /* 4: the refusals */
  GbEditMon t = e;
  CHECK(!gbe_set_move(&t, GBE_MV0, (uint16_t)(gb_max_move(s->gen) + 1)), "an over-range move is refused");
  uint8_t m1 = gb_get_move(&e, 1);
  if (m1) CHECK(!gbe_set_move(&t, GBE_MV0, m1), "a move already in slot 2 is refused for slot 1");
  for (int i = 0; i < 4; i++)
    if (!gb_get_move(&e, i)) { CHECK(!gbe_adjust(&t, GBE_PP0 + i, 1, false), "an empty slot's PP refuses"); break; }
  CHECK(memcmp(&t, &e, sizeof t) == 0, "refusals change nothing");
}

static void one_save(const char* name, uint8_t want_gen) {
  uint32_t len = 0;
  printf("\n== %s ==\n", name);
  if (!load_file(name, &len)) { printf("  (missing, skipped)\n"); return; }
  GbSession s;
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "open");
  CHECK(s.gen == want_gen, "generation");
  int nb = gbs_nboxes(&s);
  for (int b = 0; b <= nb; b++) {            /* storage boxes, then the party at index nb */
    int box = (b == nb) ? gbs_party_box(&s) : b;
    if (gbs_load_list(&s, box, g_list) != GBS_OK) continue;
    int cnt = gb_list_count(s.gen, g_list, box);
    for (int sl = 0; sl < cnt; sl++) one_slot(&s, box, sl);
  }
}

int main(void) {
  one_save("Red.sav",     GB_GEN1);
  one_save("Yellow.sav",  GB_GEN1);
  one_save("Gold.sav",    GB_GEN2);
  one_save("Crystal.sav", GB_GEN2);
  printf("\n%d slots, %d rows, %d mutations; %d checks, %d failed\n",
         g_slots, g_rows, g_changes, g_check, g_fail);
  return g_fail ? 1 : 0;
}
