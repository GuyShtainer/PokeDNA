/* source/gb_editor.c — the GB mon editor's FIELD MODEL — under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbeditor_test.c \
 *      source/gb_editor.c source/gb_item_names.c source/gb_session.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/ui_font.c \
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
#include "data_tables.h"  /* pk_species_gender_ratio/pk_move_pp/pk_species_growth/
                            * pk_exp_for_level -- test_gender()'s own record builder */
#include "pdna_layout.h"   /* PDNA_EDIT_LBL_W -- the label column row_paint draws into */
#include "ui_font.h"       /* ui_font_w -- the real proportional-font advance table, for
                            * the gb_issue_text/gbe_stale_note wrap check below. Pure
                            * data (source/ui_font.c), no GBA headers, so it dual-compiles
                            * here same as it does in tests/host_textfit_test.c. */

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
  /* BACKLOG #95 review C1 (blocker): has_caught defaults false out of gb_load (see
   * GbEditMon.has_caught, gb_edit.h) -- a real caller sets it from the SAVE HEADER's
   * own version, `s->g2w.sv.version`, exactly as this test does here, right after the
   * load a real screen would also do it after. On a Gold/Silver mount this stays
   * false and the four GBE_MET* rows must not appear below (record bytes 0x1D/0x1E
   * are Unused1/Unused2 there, not a capture record). */
  if (s->gen == GB_GEN2) gb_set_caught_available(&e, s->g2w.sv.version == G2_VER_CRYSTAL);
  g_slots++;
  uint8_t rows[GBE_NUM];
  int n = gbe_fields(&e, rows);
  /* Gen 1 hides all nine Gen-2-only rows (Item, Friendship, Gender, Shiny, Egg, Met
   * Time/Level/Loc/OT-Gender -- BACKLOG #95 added the last six); Gen 2 hides Gender
   * for a species with no real gender (gbe_has_gender_row is the same gate
   * gbe_fields() applies internally, so this doubles as live coverage of
   * gbe_flip_gender()'s LEFT/RIGHT/A behaviour below against every real Gen-2 mon in
   * Guy's Gold.sav/Crystal.sav that DOES have one), and hides all four Met rows on a
   * Gold/Silver mount (review C1 above). */
  int expect_full = s->gen == GB_GEN2 ? GBE_NUM : GBE_NUM - 9;
  if (!e.is_party) expect_full -= 1;                       /* BACKLOG #231: Cur HP is party-only */
  if (s->gen == GB_GEN2 && !gbe_has_gender_row(&e)) expect_full -= 1;
  if (s->gen == GB_GEN2 && !e.has_caught) expect_full -= 4;
  CHECK(n == expect_full, "Gen 1 hides the Gen-2 rows; Gen 2 hides Gender/Met where there is none");
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

/* How many ui_ptext_wrap(maxw=216) lines `s` needs — mirrors ui_ptext_break/
 * ui_ptext_wrap (source/ui.c) exactly, same as host_textfit_test.c's own wrap_lines(). */
static int wrap_lines(const char* s, int maxw) {
  int n = 0;
  while (*s) {
    const char* p = s;
    int w = 0, last_space = -1;
    while (*p) {
      if (*p == ' ') last_space = (int)(p - s);
      unsigned c = (unsigned char)*p;
      if (c < 32u || c > 127u) c = '?';
      int a = ui_font_w[c - 32];
      if (w + a > maxw) break;
      w += a; p++;
    }
    int i = (int)(p - s), skip;
    if (!s[i]) skip = i;
    else if (last_space > 0) skip = last_space + 1;
    else skip = i ? i : 1;
    s += skip; n++;
  }
  return n;
}

/* confirm()'s dynamic prose (source/pdna_gbedit.c) wraps every gb_issue_text() sentence
 * and both gbe_stale_note() sentences at PDNA_GBEDIT_CONFIRM_W (216px), clamped to
 * PDNA_GBEDIT_CONFIRM_MAXLN (2) lines via ui_ptext_wrap, whose fixed-size line buffer
 * (source/ui.c) is 64 bytes. A string that needs a 3rd line loses it silently (the
 * max_lines clamp just stops drawing); a string >= 64 bytes overflows that buffer. Both
 * would be real bugs, so enumerate every string these two functions can produce --
 * every gb_issue_text() branch (one GbIssues field set at a time is how gb_issue_text
 * picks its "first problem") and both gbe_stale_note() outcomes (Gen 1 / Gen 2) -- and
 * check both budgets against the real font table, not a re-typed guess. */
static void issue_and_stale_text_fits(void) {
  /* One GbIssues per branch, each with exactly the field gb_issue_text() checks for
   * that branch set -- explicit designated initializers rather than indexing the
   * struct as an array of bool, so this does not depend on GbIssues' member layout. */
  const struct { const char* name; GbIssues iss; } cases[] = {
    { "species_bad",   { .species_bad   = true } },
    { "list_mismatch", { .list_mismatch = true } },
    { "level_range",   { .level_range   = true } },
    { "level_exp_bad", { .level_exp_bad = true } },
    { "move_empty",    { .move_empty    = true } },
    { "move_hole",     { .move_hole     = true } },
    { "move_range",    { .move_range    = true } },
    { "move_dup",      { .move_dup      = true } },
    { "pp_over",       { .pp_over       = true } },
    { "pp_on_empty",   { .pp_on_empty   = true } },
    { "gen1_type_bad", { .gen1_type_bad = true } },
    { "stats_stale",   { .stats_stale   = true } },
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const char* fname = cases[i].name;
    const char* txt = gb_issue_text(&cases[i].iss);
    g_check++;
    if (!txt) { g_fail++; printf("  !! FAIL: gb_issue_text has no text for %s\n", fname); continue; }
    g_check++;
    if ((int)strlen(txt) >= 64) {
      g_fail++;
      printf("  !! FAIL: gb_issue_text(%s) = '%s' is >= 64 bytes (ui_ptext_wrap's line buffer)\n",
             fname, txt);
    }
    g_check++;
    int lines = wrap_lines(txt, PDNA_GBEDIT_CONFIRM_W);
    if (lines > PDNA_GBEDIT_CONFIRM_MAXLN) {
      g_fail++;
      printf("  !! FAIL: gb_issue_text(%s) = '%s' needs %d lines at %dpx, only %d shown\n",
             fname, txt, lines, PDNA_GBEDIT_CONFIRM_W, PDNA_GBEDIT_CONFIRM_MAXLN);
    }
  }

  GbEditMon e;
  memset(&e, 0, sizeof e);
  e.is_party = true;
  e.stats_stale = true;
  const uint8_t gens[] = { GB_GEN1, GB_GEN2 };
  for (unsigned i = 0; i < sizeof gens / sizeof gens[0]; i++) {
    e.gen = gens[i];
    const char* note = gbe_stale_note(&e);
    g_check++;
    if (!note) { g_fail++; printf("  !! FAIL: gbe_stale_note has no text for gen %u\n", gens[i]); continue; }
    g_check++;
    if ((int)strlen(note) >= 64) {
      g_fail++;
      printf("  !! FAIL: gbe_stale_note(gen %u) = '%s' is >= 64 bytes\n", gens[i], note);
    }
    g_check++;
    int lines = wrap_lines(note, PDNA_GBEDIT_CONFIRM_W);
    if (lines > PDNA_GBEDIT_CONFIRM_MAXLN) {
      g_fail++;
      printf("  !! FAIL: gbe_stale_note(gen %u) = '%s' needs %d lines at %dpx, only %d shown\n",
             gens[i], note, lines, PDNA_GBEDIT_CONFIRM_W, PDNA_GBEDIT_CONFIRM_MAXLN);
    }
  }
}

/* Every row label must fit pdna_gbedit.c's label column (source/pdna_layout.h
 * PDNA_EDIT_LBL_W, 112px at 8px/glyph — sys8, the same face row_paint draws labels
 * with). Checked here rather than in tests/host_textfit_test.c because that file only
 * links source/ui_font.c; pulling in gb_editor.c there would drag gen1/gen2 write +
 * data_tables along for one geometry number. "Egg cycles" (gbe_label_of's own addition,
 * F9) is checked alongside the table gbe_label() reads from. */
static void label_widths_fit(void) {
  for (int f = 0; f < GBE_NUM; f++) {
    const char* lbl = gbe_label(f);
    g_check++;
    if ((int)strlen(lbl) * 8 > PDNA_EDIT_LBL_W) {
      g_fail++;
      printf("  !! FAIL: label '%s' (f=%d) is wider than PDNA_EDIT_LBL_W\n", lbl, f);
    }
  }
  g_check++;
  if ((int)strlen("Egg cycles") * 8 > PDNA_EDIT_LBL_W) {
    g_fail++;
    printf("  !! FAIL: label 'Egg cycles' is wider than PDNA_EDIT_LBL_W\n");
  }
}

/* Same 32-byte Gen-2 BOX record layout as tests/host_gbedit_test.c's own mk_g2_rec
 * (species at 0x00, one move at 0x02 so the structural gate is happy, that move's PP
 * at 0x17, the Atk/Def and Spe/Spc DV nibbles at 0x15/0x16, level at 0x1F, EXP at
 * 0x08-0x0A agreeing with it under the species' own growth curve) -- duplicated
 * rather than shared because that helper is `static` in a sibling translation unit
 * this file's own cc line does not link. */
static void mk_g2_rec(uint8_t rec[GB_MAX_REC], uint8_t species, uint8_t lvl,
                      uint8_t a, uint8_t d, uint8_t s, uint8_t c) {
  uint32_t exp;
  memset(rec, 0, GB_MAX_REC);
  rec[0x00] = species;
  rec[0x02] = 1;
  rec[0x17] = pk_move_pp(1);
  rec[0x15] = (uint8_t)((a << 4) | d);
  rec[0x16] = (uint8_t)((s << 4) | c);
  rec[0x1F] = lvl;
  exp = pk_exp_for_level(pk_species_growth(species), lvl);
  rec[0x08] = (uint8_t)(exp >> 16);
  rec[0x09] = (uint8_t)(exp >> 8);
  rec[0x0A] = (uint8_t)(exp);
}

/* BACKLOG #51: GBE_GENDER -- the row, gbe_flip_gender()'s nearest-DV search, and the
 * row's own presence gate (gbe_has_gender_row). one_slot()'s row-count CHECK already
 * sweeps this row's presence and gbe_adjust/gbe_press's generic "changed agrees with
 * the bytes" / "commits touching only its own slot" invariants across every real
 * Gen-2 mon in Guy's Gold.sav/Crystal.sav; this test pins down the SPECIFIC DV
 * arithmetic against species/DV combinations chosen so the expected answer is
 * checkable by hand (ratio 127 = Pikachu, dex 25 -- the same species
 * tests/host_gbedit_test.c already uses for the identical threshold). */
static void test_gender(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES];
  GbEditMon e;
  char val[GBE_VALUE_MAX];
  memset(nm, 0x50, sizeof nm);

  /* ---- the value text reuses gb_dv_effects_of, the same derivation gbe_header()
   * already trusted (gb_editor.c:277-278's fx.gender), not a second copy of the rule. */
  mk_g2_rec(rec, 25, 20, 7, 0, 0, 0);
  CHECK(gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25), "load Pikachu, Atk DV 7");
  CHECK(gbe_has_gender_row(&e), "Pikachu (a real gender ratio) shows the Gender row");
  gbe_value(&e, GBE_GENDER, val, sizeof val);
  CHECK(strcmp(val, "F") == 0, "ratio 127: Atk DV 7 reads F");

  mk_g2_rec(rec, 25, 20, 8, 0, 0, 0);
  gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25);
  gbe_value(&e, GBE_GENDER, val, sizeof val);
  CHECK(strcmp(val, "M") == 0, "ratio 127: Atk DV 8 reads M");

  /* ---- #340a (review-zq F6): the Item row prints the item NAME on a Gen-2 mon, "#n" only for a hole id,
   * "None" for 0 -- red if gbe_value loses its gb_item_label call. */
  CHECK(gb_set_held_item(&e, 3), "hold id 3");
  gbe_value(&e, GBE_ITEM, val, sizeof val);
  CHECK(strcmp(val, "BRIGHTPOWDER") == 0, "Item row: id 3 reads BRIGHTPOWDER, not #3");
  CHECK(gb_set_held_item(&e, 6), "hold hole id 6");
  gbe_value(&e, GBE_ITEM, val, sizeof val);
  CHECK(strcmp(val, "#6") == 0, "Item row: unused id 6 falls back to #6");
  CHECK(gb_set_held_item(&e, 0), "clear held item");
  gbe_value(&e, GBE_ITEM, val, sizeof val);
  CHECK(strcmp(val, "None") == 0, "Item row: id 0 reads None");

  /* ---- the flip itself: LEFT/RIGHT/A all just call gbe_flip_gender, so one exercises
   * gbe_adjust and the other gbe_press rather than testing the same call twice. */
  mk_g2_rec(rec, 25, 20, 7, 0, 0, 0);
  gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25);
  CHECK(gbe_adjust(&e, GBE_GENDER, 1, false), "RIGHT on Gender changes the record");
  CHECK(gb_get_dv(&e, GB_ATK) == 8, "F->M from Atk 7 picks the nearest male DV, 8");
  CHECK(gbe_press(&e, GBE_GENDER), "A flips it back");
  CHECK(gb_get_dv(&e, GB_ATK) == 7, "M->F from Atk 8 picks the nearest female DV, 7");

  /* ---- shininess survives the flip. Shiny female is Atk in {2,3,6,7} with
   * Def=Spe=Spc=10 (gen2_save.c's g2_dv_shiny); the nearest SHINY male Atk
   * (10/11/14/15) to every one of those four is 10, and back from 10 is 7 -- verified
   * against the real gbe_flip_gender() search, not asserted from the rule alone. */
  {
    const uint8_t shiny_f_atk[] = { 2, 3, 6, 7 };
    for (unsigned i = 0; i < sizeof shiny_f_atk / sizeof shiny_f_atk[0]; i++) {
      GbDvEffects fx;
      mk_g2_rec(rec, 25, 20, shiny_f_atk[i], 10, 10, 10);
      gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25);
      gb_dv_effects_of(&e, &fx);
      CHECK(fx.shiny && fx.gender == 1, "fixture: shiny female to start");
      CHECK(gbe_press(&e, GBE_GENDER), "A flips a shiny female");
      gb_dv_effects_of(&e, &fx);
      CHECK(fx.shiny && fx.gender == 0, "...lands on a STILL-SHINY male");
      CHECK(gb_get_dv(&e, GB_ATK) == 10, "...specifically Atk DV 10, the nearest shiny male");
    }
    GbDvEffects fxb;
    mk_g2_rec(rec, 25, 20, 10, 10, 10, 10);
    gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25);
    CHECK(gbe_press(&e, GBE_GENDER), "A flips a shiny male back");
    gb_dv_effects_of(&e, &fxb);
    CHECK(fxb.shiny && fxb.gender == 1, "...lands on a STILL-SHINY female");
    CHECK(gb_get_dv(&e, GB_ATK) == 7, "...specifically Atk DV 7, the nearest shiny female");
  }

  /* ---- a NON-shiny mon with Def=Spe=Spc=10 must never land on a shiny Atk value: Atk
   * 9 is male and non-shiny (bit 1 clear); the closest female Atk overall is 7 (a
   * shiny value, distance 2) but that would CREATE a shiny, so the search must skip it
   * for Atk 5 (distance 4, still non-shiny) instead. */
  {
    GbDvEffects fx9;
    mk_g2_rec(rec, 25, 20, 9, 10, 10, 10);
    gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25);
    gb_dv_effects_of(&e, &fx9);
    CHECK(!fx9.shiny && fx9.gender == 0, "fixture: non-shiny male, Def/Spe/Spc = 10/10/10");
    CHECK(gbe_adjust(&e, GBE_GENDER, -1, false), "LEFT flips a near-shiny non-shiny mon");
    CHECK(gb_get_dv(&e, GB_ATK) == 5, "...never lands on Atk 7 (would create a shiny); picks 5");
    gb_dv_effects_of(&e, &fx9);
    CHECK(!fx9.shiny && fx9.gender == 1, "...result is a non-shiny female, not a shiny");
  }

  /* ---- row presence: a species with no real gender hides the row entirely, both
   * through the dedicated predicate and through gbe_fields()'s own row list, which
   * calls the same predicate. All-male (Nidoran-M) / all-female (Nidoran-F) /
   * genderless (Magnemite) -- there is no DV threshold to flip when the whole species
   * is one gender or none. */
  {
    const struct { uint8_t dex; const char* name; } fixed[] = {
      { 32, "Nidoran-M (all male, ratio 0x00)" },
      { 29, "Nidoran-F (all female, ratio 0xFE)" },
      { 81, "Magnemite (genderless, ratio 0xFF)" },
    };
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
      uint8_t rows[GBE_NUM]; int n; bool present = false;
      mk_g2_rec(rec, fixed[i].dex, 20, 5, 5, 5, 5);
      gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, fixed[i].dex);
      CHECK(!gbe_has_gender_row(&e), fixed[i].name);
      n = gbe_fields(&e, rows);
      for (int k = 0; k < n; k++) if (rows[k] == GBE_GENDER) present = true;
      CHECK(!present, "...and gbe_fields() agrees it is absent");
    }
  }

  /* ---- Gen 1 never shows it, on REAL Gen-1 party data (Red.sav): the game itself has
   * no gender concept (gb_editor.h's own comment), so gbe_has_gender_row()'s very
   * first check (e->gen != GB_GEN2) refuses before it ever looks at a species. */
  {
    uint32_t len = 0;
    if (load_file("Red.sav", &len)) {
      GbSession s1;
      int box1 = -1, cnt1 = 0;
      if (gbs_open(&s1, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK) box1 = gbs_party_box(&s1);
      if (box1 >= 0 && gbs_load_list(&s1, box1, g_list) == GBS_OK)
        cnt1 = gb_list_count(s1.gen, g_list, box1);
      if (cnt1 > 0) {
        GbEditMon e1;
        uint8_t rows1[GBE_NUM]; int n1; bool present1 = false;
        CHECK(gb_load(&e1, s1.gen, g_list, box1, 0), "load Red.sav's own party slot 0");
        CHECK(!gbe_has_gender_row(&e1), "Gen 1 never shows Gender, even on real data");
        n1 = gbe_fields(&e1, rows1);
        for (int k = 0; k < n1; k++) if (rows1[k] == GBE_GENDER) present1 = true;
        CHECK(!present1, "...gbe_fields() agrees");
      } else {
        printf("  (Red.sav party empty, skipping the Gen-1 row-absence check)\n");
      }
    } else {
      printf("  (Red.sav missing, skipping the Gen-1 row-absence check)\n");
    }
  }
}

/* BACKLOG #95 review C2/C3: gbe_flip_shiny() (gb_editor.c). C2 -- turning shiny ON
 * can be forced to move gender when no shiny Atk DV shares the current gender; the
 * flag (GbEditMon.shiny_gender_forced) and its screen-facing reader
 * (gbe_shiny_forced_gender) must say so, and must NOT say so when a gender-preserving
 * shiny Atk exists. C3 -- turning shiny OFF must nudge Def 10 -> 8, not 10 -> 9, so
 * the derived HP DV (bit 0 of each stored DV, g2_hp_dv) is left alone.
 *
 * Every expected Atk DV below is HAND-COMPUTED from g2_gender_from_dv's own formula
 * (gen2_save.c: female iff ((atk_dv&0xF)<<4 | 0xF) <= gender_ratio), not re-derived
 * through gbe_flip_shiny itself -- a self-referential check would pass the exact
 * mutations this test exists to catch:
 *   mutation 1 ("remove gender preservation" -- search all 8 shiny Atk values for the
 *     nearest to `cur`, ignoring which gender each yields): Pikachu fixture below
 *     starts at Atk 8 (male, ratio 127); mutation-1's nearest-overall answer is Atk 7
 *     (distance 1, but FEMALE), while the real gender-preserving search must land on
 *     Atk 10 (distance 2, the nearest MALE shiny value) -- these two answers differ,
 *     so the test only passes for the real code.
 *   mutation 2 (10 -> 9 instead of 10 -> 8 for OFF): asserted directly via the HP DV
 *     check after the OFF flip below. */
static void test_shiny(void) {
  uint8_t rec[GB_MAX_REC], nm[GB_NAME_BYTES];
  GbEditMon e;
  GbDvEffects fx;
  memset(nm, 0x50, sizeof nm);
  printf("\n== gbe_flip_shiny: gender-preserving where possible, forced-and-reported "
         "where not, OFF leaves the HP DV alone ==\n");

  /* ---- ON, gender preserved: Pikachu (ratio 127), starting male at Atk 8. The
   * male-only shiny candidates are {10,11,14,15} (composites 175/191/239/255, all
   * > 127); nearest to 8 is 10. Mutation 1's nearest-OVERALL answer among all eight
   * shiny values {2,3,6,7,10,11,14,15} would be 7 (distance 1, but female) -- this
   * is exactly the case that tells the two answers apart. */
  mk_g2_rec(rec, 25, 20, 8, 3, 4, 5);
  CHECK(gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25), "load Pikachu, Atk DV 8");
  gb_dv_effects_of(&e, &fx);
  CHECK(!fx.shiny && fx.gender == 0, "fixture: non-shiny male to start");
  CHECK(gbe_press(&e, GBE_SHINY), "A turns Shiny on");
  gb_dv_effects_of(&e, &fx);
  CHECK(fx.shiny && fx.gender == 0, "now shiny, gender preserved (male)");
  CHECK(gb_get_dv(&e, GB_ATK) == 10, "...Atk DV 10 -- the nearest MALE shiny value, not 7");
  CHECK(gb_get_dv(&e, GB_DEF) == 10 && gb_get_dv(&e, GB_SPE) == 10 && gb_get_dv(&e, GB_SPC) == 10,
        "Def/Spe/Spc forced to 10");
  CHECK(!e.shiny_gender_forced, "gender was NOT forced -- a male shiny existed");
  CHECK(gbe_shiny_forced_gender(&e) == -1, "...and the screen-facing reader agrees");

  /* ---- ON, gender FORCED: Bulbasaur, ratio 31 (12.5% female) -- composite(atk) =
   * (atk<<4)|0xF, and the smallest composite among the 8 shiny Atk values is 47
   * (atk=2); 47 > 31, so EVERY shiny Atk value is male. A female Bulbasaur turning
   * shiny therefore has no shiny-female Atk to search for at all -- must be forced
   * to male, and shiny_gender_forced must report exactly that. */
  {
    /* female iff ((atk<<4)|0xF) <= 31: atk=0 -> 0x0F=15 <= 31 (female); atk=1 ->
     * 0x1F=31 <= 31 (female, boundary); atk=2 -> 0x2F=47 > 31 (male). Use Atk 1. */
    uint8_t atk_f = 1;
    GbDvEffects f0;
    mk_g2_rec(rec, 1, 20, atk_f, 3, 4, 5);
    CHECK(gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 1), "load Bulbasaur, Atk DV 1 (female)");
    gb_dv_effects_of(&e, &f0);
    CHECK(!f0.shiny && f0.gender == 1, "fixture: non-shiny female to start (ratio 31)");
    CHECK(gbe_adjust(&e, GBE_SHINY, 1, false), "RIGHT turns Shiny on");
    gb_dv_effects_of(&e, &fx);
    CHECK(fx.shiny, "now shiny");
    CHECK(fx.gender == 0, "...but forced to MALE -- no shiny female exists at ratio 31");
    CHECK(e.shiny_gender_forced, "shiny_gender_forced is set");
    CHECK(gbe_shiny_forced_gender(&e) == 0, "...and reports MALE, matching the record");

    /* gbmon C9 re-verify, sticky-flag case: the forced flip above set the flag; an
     * UNRELATED edit (a plain level bump, not another GBE_SHINY) must clear it on
     * entry (gbe_adjust's own "clear on every entry" comment above) so pdna_gbedit.c's
     * post-edit popup check cannot re-fire the "gender forced" message on a row that
     * has nothing to do with shininess or gender. */
    CHECK(gbe_adjust(&e, GBE_LEVEL, 1, false), "an unrelated level bump");
    CHECK(!e.shiny_gender_forced, "the unrelated edit cleared shiny_gender_forced");
    CHECK(gbe_shiny_forced_gender(&e) == -1, "...and the reader agrees: nothing to report");
  }

  /* ---- OFF leaves the HP DV alone (review C3): the derived HP DV is bit 0 of each
   * of Atk/Def/Spe/Spc (g2_hp_dv, gen2_save.c); Def 10 (0b1010, bit0=0) must go to 8
   * (0b1000, bit0=0), NOT 9 (0b1001, bit0=1) -- mutation 2 flips this exact bit. */
  {
    uint8_t hp_before, hp_after;
    mk_g2_rec(rec, 25, 20, 10, 10, 10, 10);   /* shiny male, Atk bit1 set */
    CHECK(gb_load_parts(&e, GB_GEN2, false, rec, nm, nm, 25), "load a shiny Pikachu");
    gb_dv_effects_of(&e, &fx);
    CHECK(fx.shiny, "fixture: shiny to start");
    hp_before = gb_get_dv(&e, GB_HP);
    CHECK(gbe_press(&e, GBE_SHINY), "A turns Shiny off");
    gb_dv_effects_of(&e, &fx);
    CHECK(!fx.shiny, "no longer shiny");
    CHECK(gb_get_dv(&e, GB_DEF) == 8, "Def DV is 8, not 9 (mutation 2's own value)");
    hp_after = gb_get_dv(&e, GB_HP);
    CHECK(hp_after == hp_before, "the derived HP DV did not move");
    CHECK(!e.shiny_gender_forced, "OFF never forces gender; the flag is cleared");
    CHECK(gbe_shiny_forced_gender(&e) == -1, "...and the reader agrees");
  }
}

/* BACKLOG #231: current HP is an editable PARTY row and no other edit revives. */
static int g_hp_slots;
static void curhp_slot(GbSession* s, int box, int slot) {
  GbEditMon e;
  if (!gb_load(&e, s->gen, g_list, box, slot)) { CHECK(0, "gb_load"); return; }
  if (gb_is_egg(&e) || gb_get_stat(&e, GB_HP) == 0 || gb_get_level(&e) < 1) return;
  g_hp_slots++;
  uint16_t mx = gb_get_stat(&e, GB_HP);
  /* Gen 1 needs base data for a recalc; without it stats stay stale and no HP carry runs,
   * so the revive check below is only meaningful where gb_recalc_stats can run. */
  CHECK(gbe_kind(GBE_CURHP) == GBE_K_NUM, "Cur HP is a numeric row");
  GbEditMon t = e;
  CHECK(gbe_press(&t, GBE_CURHP) == (gb_get_current_hp(&e) != 0), "A: full<->0 toggles unless already 0");
  gb_set_current_hp(&t, 0);
  CHECK(gb_get_current_hp(&t) == 0, "set 0 writes 0 (a fainted mon)");
  CHECK(gbe_adjust(&t, GBE_CURHP, +1, false) && gb_get_current_hp(&t) == 1, "RIGHT raises 0 -> 1 (the reviving edit)");
  CHECK(gbe_adjust(&t, GBE_CURHP, +1, true) && gb_get_current_hp(&t) == mx, "R jumps to max");
  CHECK(!gbe_adjust(&t, GBE_CURHP, +1, false), "RIGHT at max changes nothing");
  CHECK(!gb_set_current_hp(&t, 60000) || gb_get_current_hp(&t) == mx, "over-max clamps to max");
  CHECK(gbe_adjust(&t, GBE_CURHP, -1, true) && gb_get_current_hp(&t) == 0, "L jumps to 0");
  /* no other edit revives a fainted mon */
  gb_set_statexp(&t, GB_HP, 40000);
  gb_set_dv(&t, GB_ATK, 3);
  gb_set_level(&t, (uint8_t)(gb_get_level(&t) < 60 ? gb_get_level(&t) + 7 : gb_get_level(&t) - 7));
  (void)gbe_settle_stats(&t);
  CHECK(gb_get_current_hp(&t) == 0, "fainted stays fainted through statexp/DV/level edits + recalc");
  /* a healthy mon is never handed max HP by an edit either. (This range check is weak on its own
   * -- any value in 1..max passes; the no-max-handout behaviour is pinned by test_carry_hp in host_gbedit_test.c.) */
  GbEditMon h = e;
  gb_set_current_hp(&h, 1);
  gb_set_statexp(&h, GB_HP, 65535);
  (void)gbe_settle_stats(&h);
  CHECK(gb_get_current_hp(&h) >= 1 && gb_get_current_hp(&h) <= gb_get_stat(&h, GB_HP), "damaged mon stays in 1..max");
  /* G2: Cur HP typed AFTER a level edit is edited against the fresh max and saves as shown */
  GbEditMon st = e;
  gb_set_level(&st, (uint8_t)(gb_get_level(&st) > 10 ? 5 : 50));
  gbe_adjust(&st, GBE_CURHP, -1, true); gbe_adjust(&st, GBE_CURHP, +1, false); gbe_adjust(&st, GBE_CURHP, +1, false);
  if (gbe_settle_stats(&st)) CHECK(gb_get_current_hp(&st) == 2, "Cur HP set after a level edit saves as shown");
}
static void test_curhp(void) {
  const char* names[4] = { "Red.sav", "Yellow.sav", "Gold.sav", "Crystal.sav" };
  printf("\n== Cur HP row (#231) ==\n");
  for (int i = 0; i < 4; i++) {
    uint32_t len = 0;
    if (!load_file(names[i], &len)) continue;
    GbSession s;
    if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK) continue;
    int pb = gbs_party_box(&s);
    if (gbs_load_list(&s, pb, g_list) != GBS_OK) continue;
    int cnt = gb_list_count(s.gen, g_list, pb);
    for (int sl = 0; sl < cnt; sl++) curhp_slot(&s, pb, sl);
    /* a box record: no row, and the setter refuses */
    if (gbs_load_list(&s, 0, g_list) == GBS_OK && gb_list_count(s.gen, g_list, 0) > 0) {
      GbEditMon b;
      if (gb_load(&b, s.gen, g_list, 0, 0)) {
        uint8_t rows[GBE_NUM]; int n = gbe_fields(&b, rows); bool has = false;
        for (int k = 0; k < n; k++) if (rows[k] == GBE_CURHP) has = true;
        CHECK(!has, "a box record shows no Cur HP row");
        GbEditMon c = b;
        CHECK(!gb_set_current_hp(&c, 5) && memcmp(&c, &b, sizeof c) == 0, "box setter refuses and writes nothing (refused by the !is_party guard; the mx == 0 guard is a second)");
      }
    }
  }
  CHECK(g_hp_slots > 0, "at least one party slot was exercised");
}

int main(void) {
  one_save("Red.sav",     GB_GEN1);
  one_save("Yellow.sav",  GB_GEN1);
  one_save("Gold.sav",    GB_GEN2);
  one_save("Crystal.sav", GB_GEN2);
  label_widths_fit();
  issue_and_stale_text_fits();
  test_gender();
  test_shiny();
  test_curhp();
  printf("\n%d slots, %d rows, %d mutations; %d checks, %d failed\n",
         g_slots, g_rows, g_changes, g_check, g_fail);
  return g_fail ? 1 : 0;
}
