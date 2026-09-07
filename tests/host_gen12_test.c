/* Host test for the Gen 1/2 import: synthetic saves -> gen1_save/gen2_save ->
 * gen12_convert -> a real 80-byte Gen-3 PC record.
 *
 *   cc -std=c11 -I source -I tests -DPDNA_GEN12_HOST tests/host_gen12_test.c \
 *      tests/gen12_fixture.c source/gen1_save.c source/gen2_save.c \
 *      source/gen12_convert.c source/pdna_gen12.c source/gen3_mon.c source/gen3_edit.c source/gen3_daycare.c \
 *      source/gen3_save.c source/gen3_box.c source/data_tables.c -o /tmp/hg12 && /tmp/hg12
 *
 * Guy owns no Gen-1/2 saves, so tests/gen12_fixture.c builds the corpus in memory. The
 * fixture is deliberately an INDEPENDENT transcription of the same primary sources the
 * modules were written from (pret/pokered's PokedexOrder and charmap, Bulbapedia's DV
 * rules, the four legacy EXP curves), so "the fixture and the module agree" is evidence
 * rather than tautology.
 *
 * Takes no arguments and reads no files.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gen12_fixture.h"
#include "gen1_save.h"
#include "gen2_save.h"
#include "gen12_convert.h"
#include "pdna_gen12.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "data_tables.h"

static int g_fail = 0, g_checks = 0;

#define CHECK(c, ...) do { g_checks++; if (!(c)) { \
    printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define CHECK_EQ(got, want, ...) do { g_checks++; \
    long long _g = (long long)(got), _w = (long long)(want); \
    if (_g != _w) { printf("  !! FAIL: "); printf(__VA_ARGS__); \
      printf("  (got %lld, want %lld)\n", _g, _w); g_fail++; } } while (0)

#define CHECK_STR(got, want, ...) do { g_checks++; \
    if (strcmp((got), (want)) != 0) { printf("  !! FAIL: "); printf(__VA_ARGS__); \
      printf("  (got \"%s\", want \"%s\")\n", (got), (want)); g_fail++; } } while (0)

static uint8_t g_img[GBF_MAX_BYTES];
static uint8_t g_img2[GBF_MAX_BYTES];

/* Glyphs, not bytes: a decoded GB name is UTF-8 (the two gender signs are 3 bytes). */
static int utf8_glyphs(const char* s) {
  int n = 0;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++)
    if ((*p & 0xC0u) != 0x80u) n++;
  return n;
}

/* ------------------------------------------------------------------------------
 * (0) The oracles must agree before anything built on them means anything.
 * ---------------------------------------------------------------------------- */

/* Translate the SHIPPED Gen-3 growth id into the fixture's enum by what its level-100
 * total actually is, so nothing depends on remembering pokeemerald's enum order. */
static uint8_t growth_shipped(uint16_t dex) {
  switch (pk_exp_for_level(pk_species_growth(dex), 100)) {
    case 1000000u: return GBF_MEDFAST;
    case 1059860u: return GBF_MEDSLOW;
    case  800000u: return GBF_FAST;
    case 1250000u: return GBF_SLOW;
    default:       return 0xFFu;      /* Erratic / Fluctuating */
  }
}

static void part0_oracles(void) {
  printf("\n(0) oracles\n");

  /* The whole "raw EXP reproduces the mon's level exactly" claim (research §4) rests on
   * Gen 3 keeping the four legacy curves bit-identical. Check every level, not just 100. */
  int curve_diffs = 0;
  const uint8_t fx[4] = { GBF_MEDFAST, GBF_MEDSLOW, GBF_FAST, GBF_SLOW };
  const uint32_t l100[4] = { 1000000u, 1059860u, 800000u, 1250000u };
  for (int c = 0; c < 4; c++) {
    int g3 = -1;
    for (int g = 0; g < 6; g++) if (pk_exp_for_level((uint8_t)g, 100) == l100[c]) { g3 = g; break; }
    CHECK(g3 >= 0, "no Gen-3 growth id has a level-100 total of %u", l100[c]);
    if (g3 < 0) continue;
    for (int lv = 2; lv <= 100; lv++)
      if (pk_exp_for_level((uint8_t)g3, (uint8_t)lv) != gbf_exp_for(fx[c], (uint8_t)lv))
        curve_diffs++;
  }
  CHECK_EQ(curve_diffs, 0, "Gen-3 EXP tables differ from the legacy curves");

  /* If any species 1..251 used Erratic or Fluctuating, a carried EXP value would land on
   * the wrong level and the import would silently mis-level Pokemon. */
  int exotic = 0;
  for (uint16_t d = 1; d <= 251; d++) if (growth_shipped(d) == 0xFFu) exotic++;
  CHECK_EQ(exotic, 0, "species 1..251 using a Gen-3-only EXP curve");

  /* Two independent transcriptions of pret/pokered's PokedexOrder. */
  int map_diffs = 0;
  for (uint16_t i = 1; i <= GBF_GEN1_INDEX_MAX; i++)
    if (gen1_dex_from_index((uint8_t)i) != gbf_gen1_dex(i)) map_diffs++;
  CHECK_EQ(map_diffs, 0, "internal->dex map disagrees with the fixture's copy");
  CHECK_EQ(gen1_dex_from_index(0), 0, "index 0 must not map to a species");
  CHECK_EQ(gen1_dex_from_index(0xBF), 0, "index 191 is past the table");
  CHECK_EQ(gen1_dex_from_index(0x99), 1, "Bulbasaur is internal 0x99");
  CHECK_EQ(gen1_dex_from_index(0x01), 112, "Rhydon is internal 0x01");

  /* Every roster species' Gen-3 ratio byte must be the one its Gen-2 threshold implies,
   * or the gender assertions later on are meaningless. */
  int ratio_diffs = 0;
  for (int gi = 0; gi < 3; gi++) {
    GbfGame g = (GbfGame)gi;
    for (int b = -1; b < gbf_nboxes(g); b++) {
      int n = 0; const GbfMon* r = gbf_roster(g, b, &n);
      for (int i = 0; i < n; i++) {
        if (!r[i].dex) continue;
        uint8_t want = gbf_gen3_ratio(r[i].gb_female_thresh);
        uint8_t got  = pk_species_gender_ratio(r[i].dex);
        if (want != got) {
          ratio_diffs++;
          printf("     ratio mismatch dex %u (%s): fixture %u, data_tables %u\n",
                 r[i].dex, pk_species_name(r[i].dex), want, got);
        }
      }
    }
  }
  CHECK_EQ(ratio_diffs, 0, "fixture gender ratios disagree with data_tables");

  /* The three derivations, fixture vs shipped, over the whole DV space (65536 combos). */
  int shiny_d = 0, letter_d = 0, hpdv_d = 0;
  for (int a = 0; a < 16; a++) for (int d = 0; d < 16; d++)
  for (int s = 0; s < 16; s++) for (int c = 0; c < 16; c++) {
    uint8_t dv[4] = { (uint8_t)a, (uint8_t)d, (uint8_t)s, (uint8_t)c };
    if (gbf_is_shiny(dv)     != gen12_is_shiny(dv[0], dv[1], dv[2], dv[3]))     shiny_d++;
    if (gbf_unown_letter(dv) != gen12_unown_letter(dv[0], dv[1], dv[2], dv[3])) letter_d++;
    if (gbf_hp_dv(dv)        != gen12_hp_dv(dv[0], dv[1], dv[2], dv[3]))        hpdv_d++;
    if (gbf_is_shiny(dv)     != g2_dv_shiny(dv))                               shiny_d++;
    if (gbf_unown_letter(dv) != g2_unown_letter(dv))                           letter_d++;
    if (gbf_hp_dv(dv)        != g2_hp_dv(dv))                                  hpdv_d++;
  }
  CHECK_EQ(shiny_d, 0,  "shiny rule differs across 65536 DV combinations");
  CHECK_EQ(letter_d, 0, "Unown letter differs across 65536 DV combinations");
  CHECK_EQ(hpdv_d, 0,   "HP DV differs across 65536 DV combinations");

  /* DV -> IV, pinned to the number rather than to the module's own constant. */
  for (uint8_t dv = 0; dv <= 15; dv++) {
    CHECK_EQ(gen12_iv_from_dv(dv), dv * 2, "IV for DV %u", dv);
    CHECK_EQ(gen12_iv_from_dv(dv) / 2, dv, "DV %u must be recoverable from its IV", dv);
    CHECK(gen12_iv_from_dv(dv) <= 30, "DV %u must never produce a perfect 31", dv);
  }

  printf("    curves, dex map, ratios and DV derivations agree (%d checks so far)\n", g_checks);
}

/* ------------------------------------------------------------------------------
 * (1) Gen 1
 * ---------------------------------------------------------------------------- */

/* Gen1ReadFn over a resident image, for the streaming-only entry points. */
static bool img_read_cb(void* ctx, uint32_t off, void* buf, uint32_t len) {
  if (off + len > GBF_SAVE_BYTES) return false;
  memcpy(buf, (const uint8_t*)ctx + off, len);
  return true;
}

static void part1_gen1(void) {
  printf("\n(1) Gen 1 (R/B/Y)\n");
  uint32_t len = gbf_build_ex(GBF_RBY, g_img, 0, growth_shipped);
  CHECK_EQ(len, GBF_SAVE_BYTES, "Gen-1 image length");

  Gen1Save sv;
  Gen1Status st = gen1_open(g_img, len, &sv);
  CHECK_EQ(st, GEN1_OK, "gen1_open: %s", gen1_status_text(st));
  if (st != GEN1_OK) return;

  CHECK_EQ(sv.checksum_stored, sv.checksum_calc, "stored vs computed checksum");
  CHECK_EQ(sv.checksum_stored, gbf_gen1_checksum(g_img), "fixture and module compute the same sum");
  CHECK_EQ(sv.trainer_id, gbf_player_tid(), "trainer id (big-endian)");
  CHECK_STR(sv.player_name, gbf_player_name(), "player name");
  CHECK_EQ(sv.current_box, gbf_current_box(GBF_RBY), "current box (bit 7 must be masked off)");
  printf("    open ok: TID %u, player \"%s\", current box %d\n",
         sv.trainer_id, sv.player_name, sv.current_box);

  /* Occupancy, including the empty boxes whose species list carries decoy bytes past
   * the 0xFF terminator. */
  for (int b = 0; b < GEN1_NUM_BOXES; b++) {
    int want = 0; (void)gbf_roster(GBF_RBY, b, &want);
    CHECK_EQ(gen1_count(&sv, b), want, "box %d occupancy", b);
  }
  int pn = 0; (void)gbf_roster(GBF_RBY, -1, &pn);
  CHECK_EQ(gen1_count(&sv, GEN1_PARTY_BOX), pn, "party occupancy");

  /* Every planted mon, field by field. */
  for (int b = -1; b < GEN1_NUM_BOXES; b++) {
    int n = 0; const GbfMon* r = gbf_roster(GBF_RBY, b, &n);
    if (!r) continue;
    int box = (b < 0) ? GEN1_PARTY_BOX : b;
    for (int i = 0; i < n; i++) {
      Gen1Mon m;
      if (!gen1_decode_image(&sv, g_img, box, i, &m)) {
        CHECK(false, "box %d slot %d failed to decode (%s)", box, i, r[i].why);
        continue;
      }
      CHECK_EQ(m.species_idx, r[i].species, "box %d slot %d internal index", box, i);
      CHECK_EQ(m.dex, r[i].dex, "box %d slot %d dex (%s)", box, i, r[i].why);
      CHECK_EQ(m.level, r[i].level, "box %d slot %d level", box, i);
      CHECK_EQ(m.otId, r[i].otid, "box %d slot %d OT id", box, i);
      CHECK(!m.list_mismatch, "box %d slot %d: species list vs record disagree", box, i);
      CHECK_EQ(m.dv[G1_ATK], r[i].dv[0], "box %d slot %d Atk DV", box, i);
      CHECK_EQ(m.dv[G1_DEF], r[i].dv[1], "box %d slot %d Def DV", box, i);
      CHECK_EQ(m.dv[G1_SPE], r[i].dv[2], "box %d slot %d Spd DV", box, i);
      CHECK_EQ(m.dv[G1_SPC], r[i].dv[3], "box %d slot %d Spc DV", box, i);
      CHECK_EQ(m.dv[G1_HP], gbf_hp_dv(r[i].dv), "box %d slot %d derived HP DV", box, i);
      for (int k = 0; k < 4; k++) {
        CHECK_EQ(m.moves[k], r[i].moves[k], "box %d slot %d move %d", box, i, k);
        CHECK_EQ(m.ppups[k], r[i].moves[k] ? r[i].ppup[k] : 0,
                 "box %d slot %d PP Ups on move %d", box, i, k);
      }
      /* Gen1Mon.statexp is in record order (G1_HP..G1_SPC), the same order the fixture
       * writes — no PK_* remapping, which is exactly the mistake to avoid here. */
      for (int k = 0; k < G1_NSTATS; k++)
        CHECK_EQ(m.statexp[k], r[i].statexp[k], "box %d slot %d stat exp %d", box, i, k);
      CHECK_EQ(m.catch_rate, r[i].item, "box %d slot %d catch-rate byte", box, i);
      /* Names: junk follows the 0x50 terminator in every field, so a decoder that keeps
       * reading produces something longer than this. */
      if (utf8_glyphs(r[i].nick) == (int)strlen(r[i].nick))
        CHECK_STR(m.nickname, r[i].nick, "box %d slot %d nickname", box, i);
      else
        CHECK_EQ(utf8_glyphs(m.nickname), utf8_glyphs(r[i].nick),
                 "box %d slot %d nickname glyph count (non-ASCII)", box, i);
      CHECK_STR(m.otName, r[i].ot, "box %d slot %d OT name", box, i);
      /* Nothing anywhere may come from the stale banked copy of the current box. */
      CHECK(strcmp(m.nickname, gbf_stale_marker()) != 0,
            "box %d slot %d read the STALE banked copy", box, i);
    }
    /* One slot past the count: the species list holds a plausible byte there on purpose. */
    Gen1Mon extra;
    int cap = gen1_list_capacity(box);
    if (n < cap) CHECK(!gen1_decode_image(&sv, g_img, box, n, &extra),
                       "box %d slot %d is past the count and must not decode", box, n);
  }

  /* Level 100 and level 1 must survive the 24-bit big-endian EXP field. */
  {
    Gen1Mon hi, lo;
    gen1_decode_image(&sv, g_img, 0, 2, &hi);   /* PIKACHU, level 100 */
    gen1_decode_image(&sv, g_img, 0, 3, &lo);   /* MAGIKARP, level 1  */
    CHECK_EQ(hi.exp, 1000000u, "level-100 Medium Fast EXP");
    CHECK_EQ(pk_level_from_exp(pk_species_growth(hi.dex), hi.exp), 100, "L100 from EXP");
    CHECK_EQ(pk_level_from_exp(pk_species_growth(lo.dex), lo.exp), 1, "L1 from EXP");
    printf("    L100 exp %u -> level %u; L1 exp %u -> level %u\n",
           hi.exp, pk_level_from_exp(pk_species_growth(hi.dex), hi.exp),
           lo.exp, pk_level_from_exp(pk_species_growth(lo.dex), lo.exp));
  }

  /* The current box must come from main data, never from its stale banked copy. */
  {
    int cur = gbf_current_box(GBF_RBY);
    CHECK_EQ(gen1_list_offset(&sv, cur), GEN1_OFF_CURRENT_BOX,
             "current box resolves to the live copy");
    CHECK(gen1_list_offset(&sv, cur == 0 ? 1 : 0) != GEN1_OFF_CURRENT_BOX,
          "a non-current box must resolve into a bank");
  }

  /* Advisory only, and weaker evidence than the rest of this file: the per-bank and
   * per-box checksum ranges are the least certain part of the documented Gen-1 layout
   * (research-gen12.md §1.2 marks them VERIFY), so this only shows the fixture and the
   * module read that paragraph the same way — it cannot show either is right. Nothing
   * gates on it: gen1_open never calls gen1_check_banks. */
  {
    Gen1Save adv = sv;
    Gen1Status ast = gen1_check_banks(img_read_cb, g_img, &adv);
    CHECK_EQ(ast, GEN1_OK, "gen1_check_banks ran");
    CHECK(adv.banks_checked, "banks_checked set");
    CHECK(adv.bank_ok[0] && adv.bank_ok[1],
          "both box banks' advisory checksums match the fixture's");
  }

  /* A corrupt image must be refused, not parsed into plausible nonsense. */
  {
    memcpy(g_img2, g_img, len);
    gbf_break_primary(GBF_RBY, g_img2);
    Gen1Save bad;
    Gen1Status bst = gen1_open(g_img2, len, &bad);
    CHECK_EQ(bst, GEN1_ERR_CHECKSUM, "a flipped byte must fail the checksum");
    /* ...and putting it back must make it good again, or the "failure" proved nothing. */
    gbf_break_primary(GBF_RBY, g_img2);
    CHECK_EQ(gen1_open(g_img2, len, &bad), GEN1_OK, "restoring the byte re-validates");
  }
  /* A Gen-2 image is the same 32 KiB size and has a 1-in-256 chance of passing an 8-bit
   * checksum, which is why gen1_open also checks the structure. */
  {
    static uint8_t g2img[GBF_MAX_BYTES];
    gbf_build(GBF_CRYSTAL, g2img, 0);
    Gen1Save bad;
    CHECK(gen1_open(g2img, GBF_SAVE_BYTES, &bad) != GEN1_OK,
          "a Crystal save must not open as Gen 1");
  }
}

/* ------------------------------------------------------------------------------
 * (2) Gen 2
 * ---------------------------------------------------------------------------- */

static void part2_gen2(GbfGame game, uint32_t rtc_tail) {
  const char* label = (game == GBF_CRYSTAL) ? "Crystal" : "Gold/Silver";
  printf("\n(2) Gen 2 — %s%s\n", label, rtc_tail ? " (+RTC tail)" : "");

  uint32_t len = gbf_build_ex(game, g_img, rtc_tail, growth_shipped);
  CHECK_EQ(len, GBF_SAVE_BYTES + rtc_tail, "image length");

  G2Save sv;
  bool ok = g2_detect(g_img, len, &sv);
  CHECK(ok, "g2_detect: %s", sv.supported ? "supported" : g2_reject_reason(&sv));
  if (!ok) return;
  CHECK_EQ(sv.version, (game == GBF_CRYSTAL) ? G2_VER_CRYSTAL : G2_VER_GS, "version");
  CHECK(sv.primary_ok, "primary checksum");
  CHECK(sv.backup_ok, "backup checksum (%s)",
        game == GBF_CRYSTAL ? "contiguous mirror" : "the five-region scatter map");
  CHECK(!sv.ambiguous, "no other version's checksum collided");

  /* backup_ok above only means "the module's own two halves agree". These compare the
   * fixture's independently transcribed mirror map with the module's, which is what
   * makes it evidence about the real save format rather than self-consistency. */
  {
    const GbfMirror* fx = 0; const G2MirrorRegion* mod = 0;
    int nf = gbf_mirror_map(game, &fx);
    int nm = g2_mirror_map(sv.version, &mod);
    CHECK_EQ(nm, nf, "%s backup mirror region count", label);
    for (int i = 0; i < nf && i < nm; i++) {
      CHECK_EQ(mod[i].from, fx[i].from, "%s mirror region %d start", label, i);
      CHECK_EQ(mod[i].to,   fx[i].to,   "%s mirror region %d end", label, i);
      CHECK_EQ(mod[i].dest, fx[i].dest, "%s mirror region %d destination", label, i);
    }
  }
  CHECK_EQ(sv.tail, rtc_tail, "RTC tail length reported");
  CHECK_EQ(g2_checksum_primary(g_img, sv.version), gbf_gen2_checksum(g_img, game),
           "fixture and module compute the same primary sum");

  G2Header hd;
  CHECK(g2_read_header(g_img, &sv, &hd), "g2_read_header");
  CHECK_EQ(hd.tid, gbf_player_tid(), "trainer id");
  CHECK_STR(hd.player, gbf_player_name(), "player name");
  CHECK_EQ(hd.current_box, gbf_current_box(game), "current box (low nibble only)");
  printf("    %s: TID %u, player \"%s\", current box %d, tail %u\n",
         g2_version_name(sv.version), hd.tid, hd.player, hd.current_box, sv.tail);

  for (int b = 0; b < G2_NUM_BOXES; b++) {
    char nm[G2_NAME_BYTES];
    CHECK(g2_box_name_at(g_img, &sv, b, nm, sizeof nm), "box %d name read", b);
    CHECK_STR(nm, gbf_box_name(game, b), "box %d name", b);
    int want = 0; (void)gbf_roster(game, b, &want);
    CHECK_EQ(g2_box_count_at(g_img, &sv, &hd, b), want, "box %d occupancy", b);
  }
  int pn = 0; (void)gbf_roster(game, -1, &pn);
  CHECK_EQ(g2_box_count_at(g_img, &sv, &hd, G2_BOX_PARTY), pn, "party occupancy");

  for (int b = -1; b < G2_NUM_BOXES; b++) {
    int n = 0; const GbfMon* r = gbf_roster(game, b, &n);
    if (!r) continue;
    int box = (b < 0) ? G2_BOX_PARTY : b;
    for (int i = 0; i < n; i++) {
      G2Mon m;
      if (!g2_box_mon_at(g_img, &sv, &hd, box, i, &m)) {
        /* A species past the dex is allowed to fail to decode — the converter refuses it
         * anyway — but nothing else may. */
        CHECK(r[i].dex == 0, "box %d slot %d failed to decode (%s)", box, i, r[i].why);
        continue;
      }
      CHECK_EQ(m.species, r[i].species, "box %d slot %d species", box, i);
      CHECK_EQ(m.level, r[i].level, "box %d slot %d level", box, i);
      CHECK_EQ(m.otid, r[i].otid, "box %d slot %d OT id", box, i);
      CHECK_EQ(m.held_item, r[i].item, "box %d slot %d held item", box, i);
      CHECK_EQ(m.friendship, r[i].friendship, "box %d slot %d friendship", box, i);
      CHECK_EQ(m.pokerus, r[i].pokerus, "box %d slot %d pokerus", box, i);
      CHECK_EQ(m.is_egg, (r[i].flags & GBF_F_EGG) != 0, "box %d slot %d egg flag (%s)",
               box, i, r[i].why);
      for (int k = 0; k < 4; k++) {
        CHECK_EQ(m.dv[k], r[i].dv[k], "box %d slot %d DV %d", box, i, k);
        CHECK_EQ(m.moves[k], r[i].moves[k], "box %d slot %d move %d", box, i, k);
        CHECK_EQ(m.pp_up[k], r[i].moves[k] ? r[i].ppup[k] : 0,
                 "box %d slot %d PP Ups on move %d", box, i, k);
      }
      CHECK_EQ(m.hp_dv, gbf_hp_dv(r[i].dv), "box %d slot %d derived HP DV", box, i);
      CHECK_EQ(m.is_shiny, gbf_is_shiny(r[i].dv), "box %d slot %d shininess (%s)",
               box, i, r[i].why);
      CHECK_EQ(m.unown_letter, gbf_unown_letter(r[i].dv), "box %d slot %d Unown letter", box, i);
      CHECK_EQ(g2_gender_from_dv(m.dv[0], pk_species_gender_ratio(r[i].dex)),
               gbf_gender(r[i].dv, r[i].gb_female_thresh),
               "box %d slot %d gender (%s)", box, i, r[i].why);
      /* Crystal alone records where a mon was met; G/S must leave it empty rather than
       * inventing a plausible-looking value out of two zero bytes. */
      if (game == GBF_CRYSTAL && (r[i].met_level || r[i].met_loc || r[i].ot_gender)) {
        CHECK(m.caught_valid, "box %d slot %d Crystal caught data present", box, i);
        CHECK_EQ(m.caught_level, r[i].met_level, "box %d slot %d met level", box, i);
        CHECK_EQ(m.caught_loc, r[i].met_loc, "box %d slot %d met location", box, i);
        CHECK_EQ(m.ot_gender, r[i].ot_gender, "box %d slot %d OT gender", box, i);
      } else if (game == GBF_GS) {
        CHECK_EQ(m.ot_gender, 0, "box %d slot %d: G/S stores no OT gender", box, i);
      }
      if (utf8_glyphs(r[i].nick) == (int)strlen(r[i].nick))
        CHECK_STR(m.nickname, r[i].nick, "box %d slot %d nickname", box, i);
      else
        CHECK_EQ(utf8_glyphs(m.nickname), utf8_glyphs(r[i].nick),
                 "box %d slot %d nickname glyph count (non-ASCII)", box, i);
      CHECK_STR(m.otname, r[i].ot, "box %d slot %d OT name", box, i);
      CHECK(strcmp(m.nickname, gbf_stale_marker()) != 0,
            "box %d slot %d read the STALE banked copy", box, i);
    }
    /* The species list carries a plausible byte past its 0xFF terminator on purpose. */
    G2Mon extra;
    if (n < g2_list_capacity(box))
      CHECK(!g2_box_mon_at(g_img, &sv, &hd, box, n, &extra),
            "box %d slot %d is past the count and must not decode", box, n);
  }

  {
    int cur = gbf_current_box(game);
    G2Offsets off;
    CHECK(g2_offsets(sv.version, &off), "g2_offsets");
    CHECK_EQ(g2_list_offset(&sv, cur, cur), off.current_box_list,
             "current box resolves to the live copy in main data");
    CHECK(g2_list_offset(&sv, cur, -1) != off.current_box_list,
          "with no current box known, the banked (stale) copy is returned");
  }

  {
    memcpy(g_img2, g_img, len);
    gbf_break_primary(game, g_img2);
    G2Save bad;
    g2_detect(g_img2, len, &bad);
    CHECK(!bad.primary_ok, "a flipped byte must fail the primary checksum");
    gbf_break_primary(game, g_img2);
    G2Save good;
    CHECK(g2_detect(g_img2, len, &good) && good.primary_ok, "restoring the byte re-validates");
  }
}

/* ------------------------------------------------------------------------------
 * (3) Conversion
 * ---------------------------------------------------------------------------- */

#define TARGET_GAME 3   /* Emerald */

/* The 10 nickname bytes gen3's own encoder would write for `s`, so an "unnicknamed"
 * assertion compares bytes and not two different decodings. */
static void gen3_nick_bytes(const char* s, uint8_t out[10]) {
  uint8_t rec[80];
  EditMon e;
  gen3_build_mon(1, 5, 0x12345678u, 0x1234u, "X", TARGET_GAME, rec);
  gen3_edit_load(rec, false, &e);
  em_set_nickname(&e, s);
  memcpy(out, e.raw + 0x08, 10);
}

static void expect_refusal(const Gb12Mon* in, Gb12Result want, const char* what) {
  Gb12Target tgt = { TARGET_GAME };
  uint8_t rec[80];
  memset(rec, 0xA5, sizeof rec);
  uint8_t before[80]; memcpy(before, rec, sizeof rec);
  CHECK_EQ(gen12_can_convert(in), want, "can_convert(%s) -> %s", what, gen12_reason_text(want));
  Gb12Result r = gen12_convert(in, &tgt, rec, 0);
  CHECK_EQ(r, want, "convert(%s) -> %s", what, gen12_reason_text(want));
  CHECK(memcmp(rec, before, sizeof rec) == 0, "a refused convert must not touch `out` (%s)", what);
}

static void part3_convert(void) {
  printf("\n(3) conversion (Gen 1/2 -> Gen 3)\n");
  printf("      (* = shiny, U = Unown. A '?' in a nickname is gen3_decode_char printing"
         " the\n       gender signs; the stored bytes are 0xB5/0xB6 and are asserted"
         " as such.)\n");
  Gb12Target tgt = { TARGET_GAME };

  /* --- Gen-2 side: build every box-0 and box-5 mon and check it end to end. --- */
  gbf_build_ex(GBF_CRYSTAL, g_img, 0, growth_shipped);
  G2Save sv; G2Header hd;
  g2_detect(g_img, GBF_SAVE_BYTES, &sv);
  g2_read_header(g_img, &sv, &hd);

  const int boxes[2] = { 0, 5 };
  for (int bi = 0; bi < 2; bi++) {
    int b = boxes[bi], n = 0;
    const GbfMon* r = gbf_roster(GBF_CRYSTAL, b, &n);
    for (int i = 0; i < n; i++) {
      G2Mon m;
      uint32_t salt = (uint32_t)(b * G2_BOX_CAPACITY + i);
      Gb12Mon in;
      if (!g2_box_mon_at(g_img, &sv, &hd, b, i, &m)) {
        /* Species 252 never decodes; synthesise the struct so the refusal is still tested. */
        memset(&in, 0, sizeof in);
        in.gen = 2; in.species_dex = r[i].species; in.level = r[i].level;
        in.moves[0] = r[i].moves[0]; in.slot_salt = salt;
        expect_refusal(&in, GB12_ERR_SPECIES, r[i].why);
        continue;
      }
      gen12_from_gen2(&m, salt, &in);

      if (r[i].flags & GBF_F_REFUSE) {
        Gb12Result want = in.is_egg ? GB12_ERR_EGG
                        : in.held_item ? GB12_ERR_HELD_ITEM : GB12_ERR_SPECIES;
        expect_refusal(&in, want, r[i].why);
        continue;
      }

      uint8_t rec[80];
      Gb12Notes notes;
      Gb12Result res = gen12_convert(&in, &tgt, rec, &notes);
      if (res != GB12_OK) {
        CHECK(false, "box %d slot %d refused (%s): %s", b, i, r[i].why, gen12_reason_text(res));
        continue;
      }
      if (b == 0) {
        PkMon ev;
        pk_decode_mon(rec, false, &ev);
        static const char* k_g[3] = { "M", "F", "-" };
        printf("      %-10s L%-3u %-9s %s%s%s IV %2u/%2u/%2u/%2u/%2u/%2u  \"%s\"\n",
               pk_species_name(ev.species),
               pk_level_from_exp(pk_species_growth(ev.species), ev.experience),
               pk_nature_name(ev.nature),
               k_g[pk_gender_from(ev.personality, pk_species_gender_ratio(ev.species)) % 3],
               ev.isShiny ? " *" : "  ",
               ev.species == 201 ? "U" : " ",
               ev.ivs[PK_HP], ev.ivs[PK_ATK], ev.ivs[PK_DEF],
               ev.ivs[PK_SPE], ev.ivs[PK_SPA], ev.ivs[PK_SPD], ev.nickname);
      }

      /* Determinism: same input twice, and the same input re-derived from a freshly
       * built image, must all produce the same 80 bytes. This is what keeps the
       * clipboard/bank identity match (first 8 bytes) working across page-ins. */
      uint8_t again[80];
      CHECK_EQ(gen12_convert(&in, &tgt, again, 0), GB12_OK, "second convert");
      CHECK(memcmp(rec, again, 80) == 0, "box %d slot %d: convert is not deterministic", b, i);

      static uint8_t reimg[GBF_MAX_BYTES];
      gbf_build_ex(GBF_CRYSTAL, reimg, 0, growth_shipped);
      G2Save sv2; G2Header hd2; G2Mon m2; Gb12Mon in2; uint8_t rec2[80];
      g2_detect(reimg, GBF_SAVE_BYTES, &sv2);
      g2_read_header(reimg, &sv2, &hd2);
      g2_box_mon_at(reimg, &sv2, &hd2, b, i, &m2);
      gen12_from_gen2(&m2, salt, &in2);
      CHECK_EQ(gen12_convert(&in2, &tgt, rec2, 0), GB12_OK, "convert after a re-parse");
      CHECK(memcmp(rec, rec2, 80) == 0,
            "box %d slot %d: re-parsing the save changed the record (page-in would break)", b, i);

      /* The output has to be a real Gen-3 record, not just 80 plausible bytes. */
      PkMon pk;
      CHECK(pk_decode_mon(rec, false, &pk), "box %d slot %d decodes as Gen 3", b, i);
      CHECK(!pk.isBadEgg, "box %d slot %d has a valid Gen-3 checksum", b, i);
      CHECK(gen3_edit_roundtrip_ok(rec, false), "box %d slot %d round-trips losslessly", b, i);

      CHECK_EQ(pk.species, r[i].dex, "box %d slot %d species survives", b, i);
      CHECK_EQ(pk.otId & 0xFFFFu, r[i].otid, "box %d slot %d TID kept", b, i);
      CHECK_EQ(pk.otId >> 16, 0, "box %d slot %d SID must be 0 (the import signature)", b, i);
      CHECK_EQ(pk.metLocation, 0xFE, "box %d slot %d met location = in a trade", b, i);
      CHECK_EQ(pk.pokeball, 4, "box %d slot %d Poke Ball", b, i);
      CHECK_EQ(pk.metGame, TARGET_GAME, "box %d slot %d origin = the loaded game", b, i);
      CHECK(!pk.isEgg, "box %d slot %d is not an egg", b, i);

      /* Nature is EXP % 25 (Transporter's rule) taken from the EXP the record actually
       * CARRIES, not the raw GB value: a glitch-overflow EXP is clamped first, and a
       * record whose stored nature disagreed with its stored EXP would be internally
       * inconsistent to anyone who recomputed it. Both readings are checked. */
      CHECK_EQ(pk.nature, gen12_nature(pk.experience),
               "box %d slot %d nature matches the EXP the record carries", b, i);
      if (!notes.exp_clamped)
        CHECK_EQ(pk.nature, gen12_nature(m.exp),
                 "box %d slot %d nature = source EXP %% 25", b, i);
      CHECK_EQ(pk.nature, pk.personality % 25, "box %d slot %d nature agrees with the PID", b, i);

      CHECK_EQ(pk.isShiny, gbf_is_shiny(r[i].dv),
               "box %d slot %d shininess (%s)", b, i, r[i].why);
      CHECK_EQ(pk_is_shiny(pk.personality, (uint16_t)(pk.otId & 0xFFFFu),
                           (uint16_t)(pk.otId >> 16)), gbf_is_shiny(r[i].dv),
               "box %d slot %d shiny against the DESTINATION TID/SID", b, i);

      CHECK_EQ(pk_gender_from(pk.personality, pk_species_gender_ratio(pk.species)),
               gbf_gender(r[i].dv, r[i].gb_female_thresh),
               "box %d slot %d gender (%s)", b, i, r[i].why);

      /* IV = DV * 2, spelled out rather than fetched from gen12_iv_from_dv — asserting a
       * function against itself would let the low-bit toggle change the shipped IVs with
       * the test still green. The x2 rule is what makes DV recoverable (DV = IV / 2) and
       * what stops a re-page-in from showing different IVs (OVERNIGHT-DECISIONS.md). */
      CHECK_EQ(pk.ivs[PK_ATK], r[i].dv[0] * 2, "box %d slot %d Atk IV", b, i);
      CHECK_EQ(pk.ivs[PK_DEF], r[i].dv[1] * 2, "box %d slot %d Def IV", b, i);
      CHECK_EQ(pk.ivs[PK_SPE], r[i].dv[2] * 2, "box %d slot %d Spe IV", b, i);
      CHECK_EQ(pk.ivs[PK_SPA], r[i].dv[3] * 2, "box %d slot %d SpA IV", b, i);
      CHECK_EQ(pk.ivs[PK_SPD], r[i].dv[3] * 2,
               "box %d slot %d SpD IV (one Special feeds both)", b, i);
      CHECK_EQ(pk.ivs[PK_HP], gbf_hp_dv(r[i].dv) * 2,
               "box %d slot %d HP IV from the derived HP DV", b, i);

      CHECK_EQ(pk.evSum, 0, "box %d slot %d: stat exp is erased, EVs are 0", b, i);

      uint8_t lvl = pk_level_from_exp(pk_species_growth(pk.species), pk.experience);
      CHECK_EQ(lvl, r[i].level, "box %d slot %d level recomputed from EXP (%s)", b, i, r[i].why);
      CHECK_EQ(notes.exp_clamped, r[i].exp_raw != 0,
               "box %d slot %d exp_clamped note (%s)", b, i, r[i].why);
      CHECK(!notes.gender_relaxed, "box %d slot %d: gender constraint was dropped", b, i);
      CHECK(!notes.letter_relaxed, "box %d slot %d: Unown letter was dropped", b, i);

      if (pk.species == 201)
        CHECK_EQ(pk_unown_form(pk.personality), gbf_unown_letter(r[i].dv),
                 "box %d slot %d Unown letter survives the PID search", b, i);

      CHECK_EQ(pk.heldItem, 0, "box %d slot %d holds nothing", b, i);
      CHECK_EQ(pk.pokerus, r[i].pokerus, "box %d slot %d pokerus carried", b, i);
      CHECK_EQ(pk.friendship, r[i].friendship, "box %d slot %d friendship carried", b, i);

      for (int k = 0; k < 4; k++) {
        CHECK_EQ(pk.moves[k], r[i].moves[k], "box %d slot %d move %d", b, i, k);
        if (!r[i].moves[k]) continue;
        uint8_t base = pk_move_pp(r[i].moves[k]);
        uint8_t want = (uint8_t)(base + base / 5 * r[i].ppup[k]);
        CHECK_EQ(pk.pp[k], want, "box %d slot %d PP for move %d (base %u + %u PP Ups)",
                 b, i, k, base, r[i].ppup[k]);
        CHECK_EQ((pk.ppBonuses >> (k * 2)) & 3, r[i].ppup[k],
                 "box %d slot %d PP Up bits for move %d", b, i, k);
      }

      if (r[i].flags & GBF_F_UNNICKED) {
        uint8_t want[10];
        gen3_nick_bytes(pk_species_name(pk.species), want);
        CHECK(memcmp(rec + 0x08, want, 10) == 0,
              "box %d slot %d unnicknamed -> the Gen-3 species name \"%s\"",
              b, i, pk_species_name(pk.species));
      } else if (utf8_glyphs(r[i].nick) == (int)strlen(r[i].nick)) {
        CHECK_STR(pk.nickname, r[i].nick, "box %d slot %d nickname", b, i);
      } else {
        /* The symbols must land as the Gen-3 charset's own single bytes (0xB5/0xB6),
         * not as three spaces each — see gen3_edit.c:255. */
        CHECK_EQ(rec[0x08], 0xB5, "box %d slot %d nickname starts with the male sign", b, i);
        CHECK_EQ(rec[0x0C], 0xB6, "box %d slot %d nickname keeps the female sign", b, i);
      }
    }
  }

  /* --- Gen-1 side: same record, converted from the other generation. --- */
  {
    gbf_build_ex(GBF_RBY, g_img, 0, growth_shipped);
    Gen1Save s1;
    CHECK_EQ(gen1_open(g_img, GBF_SAVE_BYTES, &s1), GEN1_OK, "Gen-1 open for conversion");
    int n = 0; const GbfMon* r = gbf_roster(GBF_RBY, 0, &n);
    for (int i = 0; i < n; i++) {
      Gen1Mon m;
      if (!gen1_decode_image(&s1, g_img, 0, i, &m)) continue;
      Gb12Mon in; gen12_from_gen1(&m, (uint32_t)i, &in);

      /* The adapter's two documented traps, asserted directly rather than inferred from
       * the finished record: today gen12_convert drops every held item anyway, so a
       * catch-rate byte leaking into held_item would be invisible downstream until some
       * future change started honouring the field — and then every Gen-1 mon with a
       * non-zero catch rate would be refused. */
      CHECK(m.catch_rate != 0, "gen1 slot %d fixture must carry a catch rate to test this", i);
      CHECK_EQ(in.held_item, 0, "gen1 slot %d: catch rate must not become a held item", i);
      CHECK_EQ(in.species_dex, m.dex, "gen1 slot %d: adapter uses the mapped dex", i);
      CHECK(in.species_dex != m.species_idx || m.dex == m.species_idx,
            "gen1 slot %d: adapter must not pass the raw internal index through", i);
      CHECK(!in.is_egg, "gen1 slot %d: Gen 1 has no eggs", i);

      if (r[i].flags & GBF_F_REFUSE) { expect_refusal(&in, GB12_ERR_SPECIES, r[i].why); continue; }

      uint8_t rec[80]; Gb12Notes notes;
      Gb12Result res = gen12_convert(&in, &tgt, rec, &notes);
      CHECK_EQ(res, GB12_OK, "gen1 slot %d converts (%s): %s", i, r[i].why,
               gen12_reason_text(res));
      if (res != GB12_OK) continue;

      uint8_t again[80];
      gen12_convert(&in, &tgt, again, 0);
      CHECK(memcmp(rec, again, 80) == 0, "gen1 slot %d: convert is not deterministic", i);

      PkMon pk;
      CHECK(pk_decode_mon(rec, false, &pk) && !pk.isBadEgg, "gen1 slot %d valid record", i);
      CHECK_EQ(pk.species, r[i].dex, "gen1 slot %d: internal 0x%02X -> dex %u (%s)",
               i, r[i].species, r[i].dex, r[i].why);
      CHECK_EQ(pk.otId >> 16, 0, "gen1 slot %d SID = 0", i);
      CHECK_EQ(pk.isShiny, gbf_is_shiny(r[i].dv), "gen1 slot %d shininess (%s)", i, r[i].why);
      CHECK_EQ(pk_gender_from(pk.personality, pk_species_gender_ratio(pk.species)),
               gbf_gender(r[i].dv, r[i].gb_female_thresh), "gen1 slot %d gender", i);
      CHECK_EQ(pk.friendship, 70, "gen1 slot %d gets the default friendship (Gen 1 stores none)", i);
      CHECK_EQ(pk.heldItem, 0, "gen1 slot %d: the catch-rate byte is not an item", i);
      CHECK_EQ(pk.evSum, 0, "gen1 slot %d EVs are 0", i);
      CHECK_EQ(pk_level_from_exp(pk_species_growth(pk.species), pk.experience), r[i].level,
               "gen1 slot %d level", i);
    }
    printf("    Gen-1 box 0: %d records converted or refused as specified\n", n);
  }

  /* --- Two byte-identical GB mons are two Pokemon, not one. --- */
  {
    Gb12Mon a;
    memset(&a, 0, sizeof a);
    a.gen = 2; a.species_dex = 25; a.exp = 125000; a.level = 50;
    a.dv_atk = 9; a.dv_def = 4; a.dv_spd = 13; a.dv_spc = 6;
    a.moves[0] = 84; a.ot_id = 24601;
    snprintf(a.ot_name, sizeof a.ot_name, "GUY");
    snprintf(a.nickname, sizeof a.nickname, "TWIN");
    Gb12Mon b = a;
    a.slot_salt = 0; b.slot_salt = 1;
    uint8_t ra[80], rb[80];
    CHECK_EQ(gen12_convert(&a, &tgt, ra, 0), GB12_OK, "twin A converts");
    CHECK_EQ(gen12_convert(&b, &tgt, rb, 0), GB12_OK, "twin B converts");
    CHECK(memcmp(ra, rb, 8) != 0,
          "two identical GB mons in different slots must not share an identity key");
    /* ...but each is still stable on its own. */
    uint8_t ra2[80];
    gen12_convert(&a, &tgt, ra2, 0);
    CHECK(memcmp(ra, ra2, 80) == 0, "the salt must not make a single mon unstable");
  }

  /* --- Refusals that need a hand-built record. --- */
  {
    Gb12Mon e;
    memset(&e, 0, sizeof e);
    e.gen = 2; e.species_dex = 25; e.exp = 125000; e.level = 50;
    e.moves[0] = 84; e.ot_id = 1;
    Gb12Mon t = e; t.level = 101;  expect_refusal(&t, GB12_ERR_LEVEL,   "level 101");
    t = e; t.level = 0;            expect_refusal(&t, GB12_ERR_LEVEL,   "level 0");
    t = e; t.moves[0] = 0;         expect_refusal(&t, GB12_ERR_MOVE,    "no first move");
    t = e; t.moves[1] = 400;       expect_refusal(&t, GB12_ERR_MOVE,    "move id past Gen 3");
    t = e; t.species_dex = 0;      expect_refusal(&t, GB12_ERR_SPECIES, "dex 0");
    t = e; t.species_dex = 252;    expect_refusal(&t, GB12_ERR_SPECIES, "dex 252");
    t = e; t.is_egg = true;        expect_refusal(&t, GB12_ERR_EGG,     "egg");
    t = e; t.held_item = 1;        expect_refusal(&t, GB12_ERR_HELD_ITEM, "held item");
  }
}


/* ------------------------------------------------------------------------------
 * (4) The BoxSource: a mounted GB save driven through the EXACT hooks pdna_box
 *     calls, over the synthetic images from part 1/2.
 *
 * The load-bearing property here is DETERMINISM. A mounted GB save is never held in
 * RAM: pdna_box asks records(box) for 30 x 80 bytes every time the user flips a box,
 * and the whole box is re-converted on the spot. Meanwhile the clipboard, the bank's
 * deferred-delete queue and app_pc_release_slot all identify a Pokemon by the FIRST
 * EIGHT BYTES of its record (personality + OT id). So if a second page-in of the same
 * box produced even one different byte in that range, "the mon I copied" and "the mon
 * in that slot" would stop being the same mon — silently, and only for GB imports.
 * That is why this is asserted three ways: same mount in a different order, and a
 * COMPLETELY FRESH mount with fresh buffers.
 * ---------------------------------------------------------------------------- */

/* The read callback both builds use. On the GBA it is f_lseek + f_read; here it is a
 * memcpy — the only difference between the hardware path and this test. */
typedef struct { const uint8_t* img; uint32_t len; int reads; } MemFile;

static bool mem_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  MemFile* f = (MemFile*)ctx;
  if (!f || off > f->len || len > f->len - off) return false;
  memcpy(buf, f->img + off, len);
  f->reads++;
  return true;
}

#define BS_MAXBOX 16
#define BS_BOXREC (30 * 80)
static uint8_t g_snap[2][BS_MAXBOX][BS_BOXREC];

/* Page every box through the BoxSource hook and keep the bytes. `reverse` walks the
 * boxes the other way round, so the second pass is not just a repeat of the first:
 * a cache that returned the previously converted box would pass a forward repeat. */
static void page_all(BoxSource* s, int nb, int which, bool reverse) {
  for (int i = 0; i < nb; i++) {
    int b = reverse ? nb - 1 - i : i;
    uint8_t* r = s->records(b);
    CHECK(r != 0, "records(%d) returned NULL", b);
    if (r) memcpy(g_snap[which][b], r, BS_BOXREC);
  }
}

static void part4_boxsource(GbfGame game, uint32_t rtc_tail) {
  const char* label = (game == GBF_RBY) ? "R/B/Y"
                    : (game == GBF_CRYSTAL) ? "Crystal" : "Gold/Silver";
  printf("\n(4) BoxSource mount — %s%s\n", label, rtc_tail ? " (+RTC tail)" : "");

  uint32_t len = gbf_build_ex(game, g_img, rtc_tail, growth_shipped);
  MemFile f = { g_img, len, 0 };

  static uint8_t recs[GB12_RECS_BYTES], stage[GB12_STAGE_BYTES];
  Gb12Mount m;
  const char* why = 0;

  CHECK(pdna_gen12_size_is_gb(len), "%u bytes reads as a GB save size", (unsigned)len);
  CHECK(!pdna_gen12_size_is_gb(131072u), "a 131072-byte Gen-3 save does not");
  CHECK(!pdna_gen12_size_is_gb(GBF_SAVE_BYTES - 1u), "31 KiB does not");

  bool ok = pdna_gen12_mount(&m, mem_read, &f, len, recs, stage, 3 /* Emerald */, &why);
  CHECK(ok, "mount: %s", why ? why : "?");
  if (!ok) return;

  Gb12SaveKind want_kind = (game == GBF_RBY)     ? GB12_SAVE_RBY
                         : (game == GBF_CRYSTAL) ? GB12_SAVE_CRYSTAL : GB12_SAVE_GS;
  CHECK_EQ(m.kind, want_kind, "identified as %s", pdna_gen12_kind_name(want_kind));
  CHECK_EQ(m.current_box, gbf_current_box(game), "current box (live copy, not the stale bank one)");
  CHECK_EQ(m.tid, gbf_player_tid(), "trainer id");
  CHECK_STR(m.player, gbf_player_name(), "player name");

  /* ---- the census the info screen prints, against the roster ---- */
  const int nb_real = gbf_nboxes(game);
  int roster_total = 0, roster_refused = 0;
  for (int b = -1; b < nb_real; b++) {
    int n = 0; const GbfMon* r = gbf_roster(game, b, &n);
    if (!r) continue;
    roster_total += n;
    for (int i = 0; i < n; i++) if (r[i].flags & GBF_F_REFUSE) roster_refused++;
  }
  CHECK_EQ(m.nstored, roster_total, "census counted every planted Pokemon");
  CHECK_EQ(m.nready, roster_total - roster_refused, "census counted every convertible one");
  CHECK_EQ(m.nblocked + m.nunreadable, roster_refused, "census counted every refusal");
  CHECK_EQ(m.nreport, roster_refused, "every refusal is in the report the user is shown");
  printf("    %s: %d stored, %d ready, %d locked, %d unreadable, %d SD reads at mount\n",
         pdna_gen12_kind_name(m.kind), m.nstored, m.nready, m.nblocked, m.nunreadable, f.reads);

  /* ---- BACKLOG #45: gb_census() (just ran, inside the mount above) walked every
   * box through gb_list_read() and left m.stage holding the LAST one it scanned
   * (m.party_box), while m.loaded is forced to -1 -- the two fields deliberately
   * disagree here, which is exactly the gap m.staged exists to make checkable
   * instead of assumed. ---- */
  CHECK_EQ(m.loaded, -1, "gb_census leaves loaded invalidated (recs no longer trustworthy)");
  CHECK_EQ(m.staged, m.party_box, "gb_census leaves stage holding the LAST box it scanned");
  CHECK(m.staged != m.loaded, "staged and loaded disagree right after gb_census -- by design");
  {
    /* gb_list_read() itself is static to pdna_gen12.c; pdna_gen12_page() is the
     * exported entry point that calls it as part of paging a box's records in, so
     * exercising it through there proves the same contract: after paging box 0,
     * m.staged must equal 0 (gb_list_read ran and succeeded for exactly that box). */
    int box0 = 0;
    (void)pdna_gen12_page(&m, box0);
    CHECK_EQ(m.staged, box0, "gb_list_read (via pdna_gen12_page) sets staged to the box it staged");
    CHECK_EQ(m.loaded, box0, "pdna_gen12_page also updates loaded to the same box");
  }

  /* ---- the BoxSource contract (source/pdna_box.h) ---- */
  BoxSource s = pdna_gen12_source(&m);
  CHECK_EQ(s.nboxes, nb_real + 1, "boxes exposed = storage boxes + the party pseudo-box");
  CHECK_EQ(s.start_box, gbf_current_box(game), "opens on the box the player left open");
  CHECK(s.is_bank, "is_bank: removes the PARTY tab / START / PC hand-off from pdna_box");
  CHECK(s.records && s.get_name && s.set_name && s.get_wp && s.set_wp &&
        s.can_edit && s.commit && s.mark_dirty, "every hook pdna_box calls is filled");
  CHECK(s.menu_block == recs, "menu_block is the pc-layout buffer (records at +4)");
  CHECK(s.note_add == 0, "no note_add: nothing ever lands in a GB save");

  /* ---- BACKLOG #56: a box switch survives a re-derived BoxSource ---- */
  {
    CHECK(s.note_box != 0, "GB source wires note_box (PC/Bank leave it NULL)");
    int start = gbf_current_box(game);
    int other = (start + 1) % nb_real;      /* provably a DIFFERENT box */
    CHECK_EQ(m.ui_box, -1, "ui_box unset before any box switch is ever reported");
    s.note_box(other);
    CHECK_EQ(m.ui_box, other, "note_box records the box the grid put on screen");
    CHECK_EQ(m.current_box, start, "note_box must NOT touch current_box (the save's "
                                    "own live-copy bookmark, a different field)");
    BoxSource s3 = pdna_gen12_source(&m);   /* "re-entry": gb_session_core's own fix */
    CHECK_EQ(s3.start_box, other, "re-deriving the BoxSource lands back on that box, "
                                   "not the box the session originally opened on");
    s.note_box(start);                      /* leave ui_box as the test found it */
    pdna_gen12_source(&m);                  /* g_m is already &m; keeps hooks consistent */
  }
  CHECK(!s.can_edit(), "can_edit() is false");
  /* commit() MUST be false, not just harmless: pdna_box's cross-scope drop writes the
   * record, calls commit(), and reverts the destination when it fails. */
  CHECK(!s.commit(), "commit() returns false so a cross-scope drop reverts itself");

  /* the write-shaped hooks are no-ops */
  {
    char n0[12], n1[12];
    s.get_name(0, n0);
    s.set_name(0, "HACKED");
    s.get_name(0, n1);
    CHECK_STR(n1, n0, "set_name is a no-op");
    int w0 = s.get_wp(0);
    s.set_wp(0, (w0 + 1) % 16);
    CHECK_EQ(s.get_wp(0), w0, "set_wp is a no-op");
    s.mark_dirty();                        /* must not explode or change anything */
    CHECK(!s.commit(), "commit() still false after mark_dirty()");
  }

  /* box names: prefixed so the banner can never be mistaken for a Gen-3 box */
  {
    char nm[12];
    s.get_name(0, nm);
    CHECK(strncmp(nm, "GB ", 3) == 0, "box 0 name is GB-prefixed (got \"%s\")", nm);
    s.get_name(m.party_box, nm);
    CHECK_STR(nm, "GB PARTY", "the party pseudo-box names itself");
    s.get_name(nb_real - 1, nm);
    CHECK(nm[0] != 0, "the last storage box has a name");
  }

  /* ---- DETERMINISM: two independent page-ins of every box ---- */
  page_all(&s, s.nboxes, 0, false);
  page_all(&s, s.nboxes, 1, true);          /* reverse order: no page can be a repeat */
  {
    int diff = 0, firstbox = -1, firstbyte = -1;
    for (int b = 0; b < s.nboxes; b++) {
      if (memcmp(g_snap[0][b], g_snap[1][b], BS_BOXREC) == 0) continue;
      diff++;
      if (firstbox < 0) {
        firstbox = b;
        for (int i = 0; i < BS_BOXREC; i++)
          if (g_snap[0][b][i] != g_snap[1][b][i]) { firstbyte = i; break; }
      }
    }
    CHECK_EQ(diff, 0, "every box is byte-identical across two page-ins "
                      "(first difference: box %d byte %d)", firstbox, firstbyte);
  }

  /* ---- DETERMINISM across a whole re-open: a fresh mount, fresh buffers ---- */
  {
    static uint8_t recs2[GB12_RECS_BYTES], stage2[GB12_STAGE_BYTES];
    Gb12Mount m2;
    MemFile f2 = { g_img, len, 0 };
    memset(recs2, 0xA5, sizeof recs2);      /* dirty buffers: a page-in must not inherit */
    memset(stage2, 0x5A, sizeof stage2);
    CHECK(pdna_gen12_mount(&m2, mem_read, &f2, len, recs2, stage2, 3, 0), "re-mount");
    BoxSource s2 = pdna_gen12_source(&m2);
    int diff = 0;
    for (int b = 0; b < s2.nboxes; b++) {
      uint8_t* r = s2.records(b);
      if (!r || memcmp(g_snap[0][b], r, BS_BOXREC) != 0) diff++;
    }
    CHECK_EQ(diff, 0, "a fresh mount reproduces the same records "
                      "(clipboard/bank identity matching depends on this)");
    pdna_gen12_source(&m);                  /* restore the first mount as the active one */
  }

  /* ---- contents: every planted mon is findable, refusals included ---- */
  for (int b = -1; b < nb_real; b++) {
    int n = 0; const GbfMon* r = gbf_roster(game, b, &n);
    if (!r) continue;
    int box = (b < 0) ? m.party_box : b;
    uint8_t* rp = s.records(box);
    if (!rp) { CHECK(false, "box %d did not page in", box); continue; }

    for (int i = 0; i < n; i++) {
      const uint8_t* rec = rp + (size_t)i * 80;
      PkMon pm;
      bool occ = pk_decode_mon(rec, false, &pm);
      uint8_t reason = pdna_gen12_slot_reason(&m, i);
      const char* veto = pdna_gen12_why_locked(rec);

      if (!(r[i].flags & GBF_F_REFUSE)) {
        CHECK(occ, "box %d slot %d must be in the grid (%s)", box, i, r[i].why);
        CHECK_EQ(reason, GB12_OK, "box %d slot %d has no refusal reason", box, i);
        CHECK(veto == 0, "box %d slot %d: COPY is allowed", box, i);
        if (occ) CHECK_EQ(pk_national_no(pm.species), r[i].dex,
                          "box %d slot %d species (%s)", box, i, r[i].why);
        continue;
      }

      /* A refusal must never be silent: either the Pokemon is visible and marked, or
       * it is in the report the info screen shows. */
      CHECK(reason != GB12_OK, "box %d slot %d: refusal recorded (%s)", box, i, r[i].why);
      CHECK(veto != 0, "box %d slot %d: COPY vetoed with a reason", box, i);
      int in_report = 0;
      for (int k = 0; k < m.nreport; k++)
        if (m.report[k].box == box && m.report[k].slot == i) in_report = 1;
      CHECK(in_report, "box %d slot %d appears in the report (%s)", box, i, r[i].why);

      if (reason == GB12_ERR_EGG) {
        CHECK(occ, "box %d slot %d: an Egg must still be VISIBLE, not hidden", box, i);
        if (occ) {
          CHECK(pm.isEgg, "box %d slot %d draws as an Egg", box, i);
          CHECK_EQ(pk_national_no(pm.species), r[i].dex, "box %d slot %d keeps its species", box, i);
        }
      } else if (reason == GB12_ERR_HELD_ITEM) {
        CHECK(occ, "box %d slot %d: an item holder must still be VISIBLE", box, i);
        if (occ) CHECK_EQ(pk_national_no(pm.species), r[i].dex,
                          "box %d slot %d keeps its species", box, i);
      } else {
        /* A glitch species has no Gen-3 form at all — nothing can be drawn for it, so
         * the report is the ONLY place it can be surfaced. */
        CHECK_EQ(reason, GB12_ERR_SPECIES, "box %d slot %d refusal kind", box, i);
      }
    }

    /* Everything past the box's occupancy is an empty cell, all the way to 30 (a GB
     * box holds 20 and the Gen-3 grid draws 30). */
    for (int sl = n; sl < 30; sl++) {
      PkMon pm;
      CHECK(!pk_decode_mon(rp + (size_t)sl * 80, false, &pm),
            "box %d slot %d must read as empty", box, sl);
    }
  }

  /* A box index the source does not expose must not page anything in. */
  CHECK(pdna_gen12_page(&m, s.nboxes) == 0, "a box past the last one is refused");
  CHECK(pdna_gen12_page(&m, -1) == 0, "a negative box is refused");

  pdna_gen12_source(0);                     /* unmount */
  CHECK(pdna_gen12_why_locked(recs + 4) == 0, "no veto once unmounted");
}

/* Files that are not GB saves must be refused with a REASON, not misparsed. */
static void part5_rejection(void) {
  printf("\n(5) refusing what is not a western GB save\n");
  static uint8_t recs[GB12_RECS_BYTES], stage[GB12_STAGE_BYTES];
  Gb12Mount m;
  const char* why;

  uint32_t len = gbf_build_ex(GBF_CRYSTAL, g_img, 0, growth_shipped);

  /* corrupt the checksummed range: the stored sum no longer matches */
  memcpy(g_img2, g_img, len);
  gbf_break_primary(GBF_CRYSTAL, g_img2);
  {
    MemFile f = { g_img2, len, 0 };
    why = 0;
    CHECK(!pdna_gen12_mount(&m, mem_read, &f, len, recs, stage, 3, &why),
          "a corrupt save is refused");
    CHECK(why != 0 && strstr(why, "checksum") != 0, "...and says why (got \"%s\")", why ? why : "");
  }

  /* wrong size */
  {
    MemFile f = { g_img, 131072u, 0 };
    why = 0;
    CHECK(!pdna_gen12_mount(&m, mem_read, &f, 131072u, recs, stage, 3, &why),
          "a Gen-3-sized file is refused");
    CHECK(why != 0, "...with a reason (\"%s\")", why ? why : "");
  }

  /* all zeroes: no checksum can match */
  {
    memset(g_img2, 0, GBF_SAVE_BYTES);
    MemFile f = { g_img2, GBF_SAVE_BYTES, 0 };
    CHECK(!pdna_gen12_mount(&m, mem_read, &f, GBF_SAVE_BYTES, recs, stage, 3, 0),
          "a blank 32 KiB file is refused");
  }

  /* a read callback that always fails must not crash or half-mount */
  {
    MemFile f = { g_img, 0, 0 };            /* len 0 -> every read fails the bounds test */
    CHECK(!pdna_gen12_mount(&m, mem_read, &f, GBF_SAVE_BYTES, recs, stage, 3, 0),
          "unreadable media is refused");
  }
}

int main(void) {
  printf("== gen 1/2 import: synthetic saves + conversion ==\n");
  part0_oracles();
  part1_gen1();
  part2_gen2(GBF_GS, 0);
  part2_gen2(GBF_GS, GBF_RTC_TAIL_32);
  part2_gen2(GBF_CRYSTAL, 0);
  part2_gen2(GBF_CRYSTAL, GBF_RTC_TAIL_64);
  part3_convert();
  part4_boxsource(GBF_RBY, 0);
  part4_boxsource(GBF_GS, 0);
  part4_boxsource(GBF_CRYSTAL, GBF_RTC_TAIL_64);
  part5_rejection();

  printf("\n%s: %d checks, %d failure(s)\n", g_fail ? "FAIL" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
