/* Host test for source/pdna_origin_art.c — origin detection + the art router.
 *
 *   cc -std=c11 -I source tests/host_originart_test.c source/pdna_origin_art.c \
 *      source/gen12_convert.c source/gen3_mon.c source/gen3_edit.c source/gen3_daycare.c source/gen3_save.c \
 *      source/gen3_box.c source/gen1_save.c source/gen2_save.c source/data_tables.c -o /tmp/hoa
 *   /tmp/hoa /path/to/Emerald.sav /path/to/Ruby.sav ...
 *
 * Every argument is a real Gen-3 save; with none, part D is skipped. (tests/
 * run_host_tests.py hands a test the whole 5-save corpus only if the source mentions
 * argv[1] — hence the literal reference here and in main().) Part E reads Guy's Game
 * Boy saves from their fixed path and SKIPS if they are not there.
 *
 * Six parts:
 *   A. POSITIVE — records produced by gen12_convert.c itself must come back as GB
 *      imports, and the era must be claimed ONLY where it is provable. Not a
 *      re-implementation checked against itself: the fixture drives the REAL
 *      converter and the detector reads its output.
 *   B. FALSIFICATION — every clause is broken ONE AT A TIME on a known-good import
 *      and must flip the verdict to native with its OWN miss code. This is the part
 *      that proves the test can fail; a detector that always said "GB import" would
 *      pass part A and fail all of these.
 *   C. THE ART ROUTER — the degrade ladder, the pixel-exact "no GB source registered
 *      => identical to the old two lines" property, the Unown letter reaching the
 *      source, and the fetch memo that keeps an animating portrait from re-reading
 *      the SD card every frame.
 *   D. FALSE POSITIVES ON REAL GEN-3 DATA — every mon in Guy's own cartridge saves
 *      must come back NATIVE. Those saves contain the one retail population that
 *      shares the stamped fields (in-game trades: SID 0 + met 0xFE + Poke Ball).
 *   F. THE PARALLEL BANK GRID — the cell scaler that puts a 56x56 Game Boy pic in a
 *      24x22 box cell without losing its silhouette, and the cache-only door the grid
 *      opens 30 times per repaint to decide which cells wear era art.
 *   E. ERA FALSE POSITIVES ON REAL GAME BOY DATA — every Pokemon in Guy's Red.sav /
 *      Gold.sav / Crystal.sav is converted by the real converter (so its generation
 *      is GROUND TRUTH), then put through three things ordinary Gen-3 play does to a
 *      mon. A Gen-1 record that comes back as a CONFIDENT "GB2" is the defect this
 *      part exists to count; the target is 0 of N and the number is printed.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "pdna_origin_art.h"
#include "gen12_convert.h"
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_save.h"
#include "gen3_box.h"
#include "gen1_save.h"
#include "gen2_save.h"
#include "data_tables.h"

/* Guy's Game Boy dumps live outside the repo and are never copied into it, so part E
 * skips rather than fails when they are absent (same contract as host_gbreal_test.c). */
#define GBROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_checks = 0;

#define CHECK(c, ...) do { g_checks++; if (!(c)) { \
    printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define CHECK_EQ(got, want, ...) do { g_checks++; \
    long long _g = (long long)(got), _w = (long long)(want); \
    if (_g != _w) { printf("  !! FAIL: "); printf(__VA_ARGS__); \
      printf("  (got %lld, want %lld)\n", _g, _w); g_fail++; } } while (0)

/* ---- art stubs -------------------------------------------------------------------
 * mon_front.c / mon_back.c are generated ripped art and are gitignored, so on the host
 * they do not exist at all — exactly like the artless HARDWARE build, whose weak
 * fallbacks in art_fallbacks.c return NULL. Defining them here lets the test drive the
 * ladder deterministically in both states: g_art_on = 0 reproduces the artless build,
 * g_art_on = 1 reproduces a build with baked Gen-3 art. */
static int g_art_on = 0;
static uint16_t g_front[64 * 64], g_back[64 * 64], g_egg[64 * 64];
static int g_front_calls, g_back_calls;

const uint16_t* mon_front_for_form(uint16_t s, bool shiny, uint8_t form) {
  (void)s; (void)shiny; (void)form; g_front_calls++;
  return g_art_on ? g_front : 0;
}
const uint16_t* mon_back_for_form(uint16_t s, bool shiny, uint8_t form) {
  (void)s; (void)shiny; (void)form; g_back_calls++;
  return g_art_on ? g_back : 0;
}
const uint16_t* mon_front_egg(void) { return g_art_on ? g_egg : 0; }
/* not called by this module, but mon_front.h/mon_back.h declare them */
const uint16_t* mon_front_for(uint16_t s, bool sh) { (void)s; (void)sh; return 0; }
const uint16_t* mon_back_for(uint16_t s, bool sh) { (void)s; (void)sh; return 0; }

/* ---- a fake Game Boy art source -------------------------------------------------- */
static uint16_t g_gbpix[56 * 56];
static int g_gb1_on = 1, g_gb2_on = 1, g_gb_back_on = 0;
static int g_gb_calls, g_gb_last_gen, g_gb_last_dex, g_gb_last_form,
           g_gb_last_back, g_gb_last_shiny;

static const uint16_t* fake_pic(void* ctx, uint8_t gen, uint16_t dex, uint8_t form,
                                uint8_t back, uint8_t shiny, uint8_t* w, uint8_t* h) {
  (void)ctx;
  g_gb_calls++; g_gb_last_gen = gen; g_gb_last_dex = dex; g_gb_last_form = form;
  g_gb_last_back = back; g_gb_last_shiny = shiny;
  if (back && !g_gb_back_on) return 0;
  if (gen == 1 && !g_gb1_on) return 0;
  if (gen == 2 && !g_gb2_on) return 0;
  *w = 56; *h = 56;
  return g_gbpix;
}
static int fake_have(void* ctx, uint8_t gen) {
  (void)ctx;
  return (gen == 1) ? g_gb1_on : (gen == 2) ? g_gb2_on : 0;
}
static void gb_source_on(void) {
  PdnaGbArtSource s; memset(&s, 0, sizeof s);
  s.pic = fake_pic; s.have = fake_have; s.ctx = 0;
  pdna_origin_art_register(&s);
}
static void gb_source_off(void) { pdna_origin_art_register(0); }

/* ---- fixture: drive the REAL converter ------------------------------------------- */

/* A plain Gen-1 record: Kanto species, Gen-1 moves only, no Gen-2 fields at all. */
static void gb1_mon(Gb12Mon* g, uint16_t dex) {
  memset(g, 0, sizeof *g);
  g->gen = 1;
  g->species_dex = dex;
  g->level = 30;
  g->exp = 27000;
  g->dv_atk = 9; g->dv_def = 6; g->dv_spd = 12; g->dv_spc = 3;
  g->moves[0] = 33; g->moves[1] = 45; g->moves[2] = 22; g->moves[3] = 0;
  g->ot_id = 41123;
  strcpy(g->ot_name, "GUY");
  strcpy(g->nickname, "BULBY");
  g->slot_salt = 7;
}

/* A Gen-2 record. Every Gen-2-only tell is a separate knob so each can be tested on
 * its own — that is what makes the "no tell fired" blind spot measurable rather than
 * hidden. */
static void gb2_mon(Gb12Mon* g, uint16_t dex) {
  gb1_mon(g, dex);
  g->gen = 2;
  g->friendship = 70;          /* deliberately the Gen-1 value: no TELL_FRIEND       */
  g->pokerus = 0;
  g->has_caught_data = true;
  g->ot_gender = 0;
}

/* Convert and decode in one step. Returns the 80-byte record through `rec`. */
static bool make(const Gb12Mon* g, uint8_t rec[80], PkMon* m) {
  Gb12Target t; memset(&t, 0, sizeof t); t.met_game = 3;   /* Emerald */
  Gb12Notes n;
  Gb12Result r = gen12_convert(g, &t, rec, &n);
  if (r != GB12_OK) { printf("  !! FAIL: gen12_convert -> %s\n", gen12_reason_text(r)); g_fail++; return false; }
  if (!pk_decode_mon(rec, false, m)) { printf("  !! FAIL: import decoded as empty\n"); g_fail++; return false; }
  pk_resolve(m);
  return true;
}

/* Re-decode a record after an edit. */
static bool redecode(const uint8_t rec[80], PkMon* m) {
  if (!pk_decode_mon(rec, false, m)) return false;
  pk_resolve(m);
  return true;
}

/* ---- A. POSITIVE ------------------------------------------------------------------ */

static void part_a(void) {
  printf("A. imports produced by gen12_convert.c\n");
  uint8_t rec[80]; PkMon m; PdnaOrigin o; Gb12Mon g;

  /* A1: a plain Gen-1 mon. Nothing PROVES the era, so the honest answer is "GB?" —
   * with the Gen-1 picture, which is the max-likelihood era, and no era branding. */
  gb1_mon(&g, 1);                                    /* Bulbasaur */
  if (make(&g, rec, &m)) {
    pdna_origin_of(&m, &o);
    CHECK_EQ(o.verdict, PDNA_ORIGIN_GB, "A1 plain Gen-1 import not detected (miss=%d)", o.miss);
    CHECK_EQ(o.gen, PDNA_GEN1, "A1 wrong era");
    CHECK_EQ(o.gen_certain, 0, "A1 must admit it cannot separate Gen 1 from Gen 2 here");
    CHECK_EQ(o.tells, 0, "A1 no tell should fire");
    CHECK(strcmp(pdna_origin_tag(&o), "GB?") == 0, "A1 tag = %s", pdna_origin_tag(&o));
    CHECK_EQ(pdna_origin_mark(&o), '?', "A1 mark");
    /* Neutral presentation: an unproven era gets a tint (so it still reads as an
     * import) but never EITHER era's colour, or the picture's guess would be branded
     * as a fact. */
    {
      PdnaOrigin q = o;
      q.gen_certain = 1; q.gen = PDNA_GEN1; uint16_t c1 = pdna_origin_color(&q);
      q.gen = PDNA_GEN2;                    uint16_t c2 = pdna_origin_color(&q);
      CHECK(pdna_origin_color(&o) != 0, "A1 an unproven import still gets a tint");
      CHECK(pdna_origin_color(&o) != c1 && pdna_origin_color(&o) != c2,
            "A1 an unproven era must not wear an era's colour");
    }
    /* the DVs must survive the round trip through IVs */
    CHECK_EQ(o.dv[0], g.dv_atk, "A1 Atk DV round trip");
    CHECK_EQ(o.dv[1], g.dv_def, "A1 Def DV round trip");
    CHECK_EQ(o.dv[2], g.dv_spd, "A1 Spd DV round trip");
    CHECK_EQ(o.dv[3], g.dv_spc, "A1 Spc DV round trip");
    printf("    Gen-1 Bulbasaur -> %-24s tag %-5s DVs %u/%u/%u/%u\n",
           pdna_origin_text(&o), pdna_origin_tag(&o), o.dv[0], o.dv[1], o.dv[2], o.dv[3]);
  }

  /* A2: PROOF vs EVIDENCE, one signal at a time. `certain` is the whole assertion:
   * only the two signals Gen 3 cannot manufacture may set it. */
  struct { const char* what; uint16_t dex; uint16_t move; uint8_t prus, fem, friend_;
           uint8_t want_tell, want_certain, want_gen; }
  t2[] = {
    /* --- proof ------------------------------------------------------------------ */
    { "Johto species",  158, 0,   0,    0, 70,  PDNA_TELL_JOHTO,    1, PDNA_GEN2 },
    { "female OT",       25, 0,   0,    1, 70,  PDNA_TELL_OTFEM,    1, PDNA_GEN2 },
    /* --- evidence: every one of these is reachable in a Gen-3 game --------------- */
    { "Gen-2 move",      25, 202, 0,    0, 70,  PDNA_TELL_MOVE,     0, PDNA_GEN1 },
    { "Pokerus",         25, 0,   0x14, 0, 70,  PDNA_TELL_POKERUS,  0, PDNA_GEN1 },
    { "friendship!=70",  25, 0,   0,    0, 200, PDNA_TELL_FRIEND,   0, PDNA_GEN1 },
  };
  for (unsigned i = 0; i < sizeof t2 / sizeof t2[0]; i++) {
    gb2_mon(&g, t2[i].dex);
    if (t2[i].move) g.moves[1] = t2[i].move;
    g.pokerus = t2[i].prus;
    g.ot_gender = t2[i].fem;
    g.friendship = t2[i].friend_;
    if (!make(&g, rec, &m)) continue;
    pdna_origin_of(&m, &o);
    CHECK_EQ(o.verdict, PDNA_ORIGIN_GB, "A2 %s: not detected (miss=%d)", t2[i].what, o.miss);
    CHECK(o.tells & t2[i].want_tell, "A2 %s: expected tell 0x%02x, got 0x%02x",
          t2[i].what, t2[i].want_tell, o.tells);
    CHECK_EQ(o.gen_certain, t2[i].want_certain, "A2 %s: certainty", t2[i].what);
    CHECK_EQ(o.gen, t2[i].want_gen, "A2 %s: era", t2[i].what);
    CHECK(strcmp(pdna_origin_tag(&o), t2[i].want_certain ? "GB2" : "GB?") == 0,
          "A2 %s: tag = %s", t2[i].what, pdna_origin_tag(&o));
    printf("    %-16s -> %-24s tells 0x%02x  %s\n", t2[i].what, pdna_origin_text(&o),
           o.tells, t2[i].want_certain ? "PROOF" : "evidence only");
  }

  /* A3: evidence does not accumulate into proof. All three weak signals at once is
   * still not a claim — that is the difference between "likely" and "known". */
  gb2_mon(&g, 25);
  g.moves[1] = 202; g.pokerus = 0x14; g.friendship = 200;
  if (make(&g, rec, &m)) {
    pdna_origin_of(&m, &o);
    CHECK_EQ(o.gen_certain, 0, "A3 three weak signals must still not be proof");
    CHECK_EQ(o.tells & PDNA_TELL_PROOF, 0, "A3 no proof bit may be set");
    CHECK_EQ(o.tells & PDNA_TELL_WEAK, PDNA_TELL_MOVE | PDNA_TELL_POKERUS | PDNA_TELL_FRIEND,
             "A3 all three weak tells reported");
    printf("    all three weak   -> %-24s tells 0x%02x  still unproven\n",
           pdna_origin_text(&o), o.tells);
  }

  /* A4: THE FAILURE THIS FIX EXISTS FOR. Take a genuine GEN-1 import and do to it
   * exactly what a Gen-3 game does — walk it (friendship), let it catch Pokerus, and
   * teach it an HM. Nothing about its provenance changed, so nothing may claim Gen 2. */
  {
    static const struct { const char* what; int kind; } PLAY[] = {
      { "walked 128 steps",  0 }, { "caught Pokerus", 1 }, { "taught Rock Smash", 2 },
    };
    gb1_mon(&g, 1);
    if (make(&g, rec, &m)) {
      for (unsigned i = 0; i < sizeof PLAY / sizeof PLAY[0]; i++) {
        uint8_t out[100]; EditMon e; PkMon pm;
        gen3_edit_load(rec, false, &e);
        if (PLAY[i].kind == 0) em_set_friendship(&e, 71);
        if (PLAY[i].kind == 1) e.sub[3][0] = 0x11;        /* strain 1, 1 day left    */
        if (PLAY[i].kind == 2) em_set_move(&e, 3, 249);   /* Rock Smash, HM06 in Gen 3 */
        memset(out, 0, sizeof out);
        gen3_edit_commit(&e, out);
        if (!redecode(out, &pm)) continue;
        PdnaOrigin po; pdna_origin_of(&pm, &po);
        CHECK_EQ(po.verdict, PDNA_ORIGIN_GB, "A4 %s: still a GB import", PLAY[i].what);
        CHECK_EQ(po.gen_certain, 0, "A4 %s: MUST NOT become certain", PLAY[i].what);
        CHECK_EQ(po.gen, PDNA_GEN1, "A4 %s: must not be redrawn as Gen 2", PLAY[i].what);
        printf("    Gen-1 mon %-18s -> %s\n", PLAY[i].what, pdna_origin_tag(&po));
      }
    }
  }

  /* A5: a Johto species reachable by EVOLVING a Kanto one. A Gen-1 Gloom that met a
   * Sun Stone in Gen 3 is a Bellossom with its whole import signature intact, so the
   * Johto number is evidence, not proof — but the PICTURE has to be Gen 2, because
   * Gen 1 has no Bellossom to draw. The two are different questions and this asserts
   * both answers at once. */
  {
    gb1_mon(&g, 44);                                   /* Gloom */
    if (make(&g, rec, &m)) {
      uint8_t out[100]; EditMon e; PkMon em2;
      gen3_edit_load(rec, false, &e);
      em_set_species(&e, 182);                         /* Bellossom */
      memset(out, 0, sizeof out);
      gen3_edit_commit(&e, out);
      if (redecode(out, &em2)) {
        PdnaOrigin eo; pdna_origin_of(&em2, &eo);
        CHECK_EQ(eo.verdict, PDNA_ORIGIN_GB, "A5 evolved import still detected (miss=%d)", eo.miss);
        CHECK_EQ(eo.tells & PDNA_TELL_EVOJOHTO, PDNA_TELL_EVOJOHTO, "A5 evo-Johto tell");
        CHECK_EQ(eo.tells & PDNA_TELL_JOHTO, 0, "A5 must NOT be the proof-grade Johto tell");
        CHECK_EQ(eo.gen_certain, 0, "A5 an evolvable Johto id is not proof");
        CHECK_EQ(eo.gen, PDNA_GEN2, "A5 the PICTURE must still be Gen 2 — Gen 1 has none");
        printf("    Gloom -> Bellossom -> %-24s tag %s (picture Gen 2, claim unproven)\n",
               pdna_origin_text(&eo), pdna_origin_tag(&eo));
      }
    }
  }

  /* A6: THE KNOWN BLIND SPOT, asserted rather than hidden. A Gen-2 Kanto mon with
   * Gen-1 moves, friendship 70, no Pokerus and a male OT is byte-indistinguishable
   * from a Gen-1 import. It must report the Gen-1 picture and admit uncertainty. */
  gb2_mon(&g, 25);                                   /* Pikachu, Gen-2 save, no tells */
  if (make(&g, rec, &m)) {
    pdna_origin_of(&m, &o);
    CHECK_EQ(o.verdict, PDNA_ORIGIN_GB, "A6 not detected");
    CHECK_EQ(o.gen, PDNA_GEN1, "A6 max-likelihood picture is Gen 1");
    CHECK_EQ(o.gen_certain, 0, "A6 must NOT claim certainty");
    printf("    blind spot       -> %-24s (a Gen-2 mon, drawn as Gen 1, claimed as neither)\n",
           pdna_origin_text(&o));
  }

  /* A7: the hint overrides the guess outright — it is the caller telling us, not us
   * inferring. It is also the ONLY way "GB1" is ever said out loud. */
  if (make(&g, rec, &m)) {
    pdna_origin_of_hint(&m, PDNA_GEN2, &o);
    CHECK_EQ(o.gen, PDNA_GEN2, "A7 hint ignored");
    CHECK_EQ(o.gen_certain, 1, "A7 hint must be certain");
    CHECK_EQ(o.tells, PDNA_TELL_HINT, "A7 hint tell");
    CHECK(o.dv[0] == g.dv_atk, "A7 hint still recovers the DVs");
    pdna_origin_of_hint(&m, PDNA_GEN1, &o);
    CHECK(strcmp(pdna_origin_tag(&o), "GB1") == 0, "A7 hinted Gen 1 tag = %s",
          pdna_origin_tag(&o));
    pdna_origin_of_hint(&m, 0, &o);                  /* 0 = no hint -> plain inference */
    CHECK_EQ(o.gen, PDNA_GEN1, "A7 hint 0 must behave as pdna_origin_of");
    CHECK_EQ(o.gen_certain, 0, "A7 hint 0 must not be certain");
  }

  /* A8: THE HINT MUST CONSULT THE RECORD. pdna_gen12.c fills a 30-cell grid from a
   * 20-slot GB box and draws eggs / damaged records as Gen-3 stand-ins, so a hint that
   * answered from the hint alone put a confident "GB2" on empty cells. */
  {
    PkMon empty; memset(&empty, 0, sizeof empty);
    pdna_origin_of_hint(&empty, PDNA_GEN2, &o);
    CHECK_EQ(o.verdict, PDNA_ORIGIN_NATIVE, "A8 an EMPTY slot is not a Gen-2 import");
    CHECK_EQ(pdna_origin_mark(&o), 0, "A8 an empty slot draws no marker");

    pdna_origin_of_hint(0, PDNA_GEN2, &o);
    CHECK_EQ(o.verdict, PDNA_ORIGIN_NATIVE, "A8 a NULL mon is not a Gen-2 import");

    gb1_mon(&g, 1);
    if (make(&g, rec, &m)) {
      uint8_t out[100]; EditMon e; PkMon eggm;
      gen3_edit_load(rec, false, &e);
      em_set_egg(&e, true);
      memset(out, 0, sizeof out);
      gen3_edit_commit(&e, out);
      if (redecode(out, &eggm)) {
        pdna_origin_of_hint(&eggm, PDNA_GEN2, &o);
        CHECK_EQ(o.verdict, PDNA_ORIGIN_NATIVE, "A8 an EGG is not a Gen-2 import "
                 "(the converter refuses eggs; what is drawn is a stand-in)");
      }
    }
    /* a species no GB record can hold, hinted anyway */
    {
      uint8_t nat[80]; PkMon nm;
      gen3_build_mon(300, 40, 0x1234ABCDu, 0x00010002u, "GUY", 3, nat);
      if (redecode(nat, &nm)) {
        pdna_origin_of_hint(&nm, PDNA_GEN1, &o);
        CHECK_EQ(o.verdict, PDNA_ORIGIN_NATIVE, "A8 internal species 300 cannot be a GB import");
      }
    }
    printf("    hint on empty / NULL / egg / Hoenn species -> native, no marker\n");
  }

  /* A9: a SHINY import. Shininess is never relaxed by the converter, so the detector's
   * shiny clause has to agree in the positive direction too, not just reject. */
  gb1_mon(&g, 25);
  g.dv_atk = 3; g.dv_def = 10; g.dv_spd = 10; g.dv_spc = 10;   /* the Gen-2 shiny DVs */
  if (make(&g, rec, &m)) {
    CHECK(m.isShiny, "A9 the converter should have produced a shiny");
    pdna_origin_of(&m, &o);
    CHECK_EQ(o.verdict, PDNA_ORIGIN_GB, "A9 shiny import not detected (miss=%d)", o.miss);
  }

  /* A10: MEW. gen12_convert.c sets the modernFatefulEncounter ribbon bit on an
   * imported Mew (put_fateful) so the game will obey it — and the ribbons-are-zero
   * clause used to reject every one of them. Three real Mews in Guy's Red.sav were
   * being reported as native Gen-3 Pokemon because of it. */
  gb1_mon(&g, 151);
  if (make(&g, rec, &m)) {
    CHECK(m.ribbons != 0, "A10 the converter should have set Mew's fateful bit");
    pdna_origin_of(&m, &o);
    CHECK_EQ(o.verdict, PDNA_ORIGIN_GB, "A10 imported Mew not detected (miss=%d)", o.miss);
    printf("    Gen-1 Mew        -> %-24s (fateful bit 0x%08X allowed)\n",
           pdna_origin_text(&o), (unsigned)m.ribbons);
  }
}

/* ---- B. FALSIFICATION -------------------------------------------------------------
 * Break exactly one clause of a known-good import and require the SPECIFIC miss code.
 * Each case also asserts the unmodified control is still detected, so a mutation that
 * silently failed to apply cannot pass. */

typedef void (*Breaker)(EditMon* e);

static void brk_sid(EditMon* e)      { e->otId |= 0x00010000u; }
static void brk_metloc(EditMon* e)   { em_set_metloc(e, 0x39); }        /* a route */
static void brk_ball(EditMon* e)     { em_set_ball(e, 3); }             /* Great Ball */
static void brk_lang(EditMon* e)     { e->raw[0x12] = 1; }              /* Japanese */
static void brk_iv_odd(EditMon* e)   { em_set_iv(e, PK_ATK, 19); }
static void brk_iv_split(EditMon* e) { em_set_iv(e, PK_SPD, 8); }       /* SpD != SpA */
static void brk_iv_hp(EditMon* e)    { em_set_iv(e, PK_HP, 30); }
static void brk_ev(EditMon* e)       { em_set_ev(e, PK_ATK, 4); }
static void brk_egg(EditMon* e)      { em_set_egg(e, true); }
static void brk_species(EditMon* e)  { em_set_species(e, 300); }        /* internal 300 = Hoenn */
static void brk_fateful(EditMon* e)  { e->sub[3][11] |= 0x80u; }        /* not a Mew */

static struct { const char* what; Breaker fn; uint8_t want_miss; } BREAK[] = {
  { "secret id != 0",        brk_sid,      PDNA_SIG_SID      },
  { "met location",          brk_metloc,   PDNA_SIG_METLOC   },
  { "not a Poke Ball",       brk_ball,     PDNA_SIG_BALL     },
  { "not English",           brk_lang,     PDNA_SIG_LANG     },
  { "an odd IV",             brk_iv_odd,   PDNA_SIG_IV_ODD   },
  { "SpA IV != SpD IV",      brk_iv_split, PDNA_SIG_IV_SPLIT },
  { "HP IV off the lattice", brk_iv_hp,    PDNA_SIG_IV_HP    },
  { "a non-zero EV",         brk_ev,       PDNA_SIG_EV       },
  { "marked as an egg",      brk_egg,      PDNA_SIG_SPECIES  },
  { "a Hoenn species",       brk_species,  PDNA_SIG_SPECIES  },
  { "fateful bit, not Mew",  brk_fateful,  PDNA_SIG_CONTEST  },
};

static void part_b(void) {
  printf("B. falsification: break one clause at a time\n");
  Gb12Mon g; uint8_t rec[80]; PkMon m; PdnaOrigin o;
  gb1_mon(&g, 1);
  if (!make(&g, rec, &m)) return;
  pdna_origin_of(&m, &o);
  CHECK_EQ(o.verdict, PDNA_ORIGIN_GB, "B control must be detected before we break it");

  for (unsigned i = 0; i < sizeof BREAK / sizeof BREAK[0]; i++) {
    uint8_t out[100]; EditMon e; PkMon bm;
    gen3_edit_load(rec, false, &e);
    BREAK[i].fn(&e);
    memset(out, 0, sizeof out);
    gen3_edit_commit(&e, out);
    if (!pk_decode_mon(out, false, &bm)) {
      /* an egg still decodes; anything that does not is itself a pass for "not GB" */
      printf("    %-24s -> record no longer decodes (still not a GB import)\n", BREAK[i].what);
      g_checks++;
      continue;
    }
    pk_resolve(&bm);
    PdnaOrigin bo; pdna_origin_of(&bm, &bo);
    CHECK_EQ(bo.verdict, PDNA_ORIGIN_NATIVE, "B %s: still reported as a GB import",
             BREAK[i].what);
    CHECK_EQ(bo.miss, BREAK[i].want_miss, "B %s: wrong miss code", BREAK[i].what);
    printf("    %-24s -> native, miss=%d\n", BREAK[i].what, bo.miss);
  }

  /* Mew's exemption is exactly ONE bit wide: any OTHER ribbon on a Mew still rejects,
   * so the exemption cannot be used as a hole to walk a decorated mon through. */
  {
    Gb12Mon mg; gb1_mon(&mg, 151);
    uint8_t mrec[80]; PkMon mm;
    if (make(&mg, mrec, &mm)) {
      uint8_t out[100]; EditMon e; PkMon bm;
      gen3_edit_load(mrec, false, &e);
      e.sub[3][8] |= 0x01u;                  /* the Cool-contest ribbon, bit 0 */
      memset(out, 0, sizeof out);
      gen3_edit_commit(&e, out);
      if (pk_decode_mon(out, false, &bm)) {
        pk_resolve(&bm);
        PdnaOrigin bo; pdna_origin_of(&bm, &bo);
        CHECK_EQ(bo.verdict, PDNA_ORIGIN_NATIVE, "B a Mew with a REAL ribbon is not an import");
        CHECK_EQ(bo.miss, PDNA_SIG_CONTEST, "B Mew + real ribbon: wrong miss code");
        printf("    %-24s -> native, miss=%d\n", "a Mew with a ribbon", bo.miss);
      }
    }
  }

  /* Two clauses that need a plaintext poke rather than an em_* setter. */
  {
    /* nature: bump the stored EXP by one so exp%25 no longer equals pid%25, WITHOUT
     * touching the PID. em_set_level would rewrite exp from the level, so write the
     * Growth substruct's exp word through the EditMon directly. */
    uint8_t out[100]; EditMon e; PkMon bm;
    gen3_edit_load(rec, false, &e);
    uint32_t exp = (uint32_t)e.sub[0][4] | ((uint32_t)e.sub[0][5] << 8) |
                   ((uint32_t)e.sub[0][6] << 16) | ((uint32_t)e.sub[0][7] << 24);
    exp += 1;
    e.sub[0][4] = (uint8_t)exp;         e.sub[0][5] = (uint8_t)(exp >> 8);
    e.sub[0][6] = (uint8_t)(exp >> 16); e.sub[0][7] = (uint8_t)(exp >> 24);
    memset(out, 0, sizeof out);
    gen3_edit_commit(&e, out);
    if (pk_decode_mon(out, false, &bm)) {
      pk_resolve(&bm);
      PdnaOrigin bo; pdna_origin_of(&bm, &bo);
      CHECK_EQ(bo.verdict, PDNA_ORIGIN_NATIVE, "B nature: still a GB import");
      CHECK_EQ(bo.miss, PDNA_SIG_NATURE, "B nature: wrong miss code");
      printf("    %-24s -> native, miss=%d\n", "EXP % 25 != nature", bo.miss);
    }
  }
  {
    /* contest condition: retail in-game trades set these, GB imports never do. */
    uint8_t out[100]; EditMon e; PkMon bm;
    gen3_edit_load(rec, false, &e);
    e.sub[2][6] = 30;                   /* EVs substruct byte 6 = Cool (contest[0]) */
    memset(out, 0, sizeof out);
    gen3_edit_commit(&e, out);
    if (pk_decode_mon(out, false, &bm)) {
      pk_resolve(&bm);
      PdnaOrigin bo; pdna_origin_of(&bm, &bo);
      CHECK_EQ(bo.verdict, PDNA_ORIGIN_NATIVE, "B contest: still a GB import");
      CHECK_EQ(bo.miss, PDNA_SIG_CONTEST, "B contest: wrong miss code");
      printf("    %-24s -> native, miss=%d\n", "a contest condition", bo.miss);
    }
  }
  {
    /* Shininess, isolated. Do NOT touch the PID -- that would move the nature too and
     * the nature clause would trip first, testing the wrong thing. Instead move the
     * DVs onto the Gen-2 SHINY pattern (Def = Spd = Spc = 10, Atk bit 1 set) while
     * leaving the record's non-shiny PID alone: every lattice clause still holds, so
     * the ONLY thing left to reject it is the shiny disagreement.
     *   Atk 3 -> IV 6, Def/Spd/Spc 10 -> IV 20 (both Specials), and the derived HP DV
     *   is (3&1)<<3 = 8 -> IV 16. */
    uint8_t out[100]; EditMon e; PkMon bm;
    gen3_edit_load(rec, false, &e);
    em_set_iv(&e, PK_ATK, 6);  em_set_iv(&e, PK_DEF, 20); em_set_iv(&e, PK_SPE, 20);
    em_set_iv(&e, PK_SPA, 20); em_set_iv(&e, PK_SPD, 20); em_set_iv(&e, PK_HP, 16);
    memset(out, 0, sizeof out);
    gen3_edit_commit(&e, out);
    if (pk_decode_mon(out, false, &bm)) {
      pk_resolve(&bm);
      CHECK(!bm.isShiny, "B shiny: the PID must still be non-shiny");
      PdnaOrigin bo; pdna_origin_of(&bm, &bo);
      CHECK_EQ(bo.verdict, PDNA_ORIGIN_NATIVE, "B shiny: still a GB import");
      CHECK_EQ(bo.miss, PDNA_SIG_SHINY, "B shiny: wrong miss code");
      printf("    %-24s -> native, miss=%d\n", "shiny DVs, non-shiny PID", bo.miss);
    }
  }
}

/* ---- C. THE ART ROUTER ------------------------------------------------------------ */

static void part_c(void) {
  printf("C. the art router\n");
  Gb12Mon g; uint8_t rec[80]; PkMon m; PdnaArt a; PdnaOrigin o;

  gb1_mon(&g, 1);
  if (!make(&g, rec, &m)) return;

  /* C1: NO GB source + baked Gen-3 art => exactly the old two lines. */
  gb_source_off(); g_art_on = 1; g_gb_calls = 0;
  CHECK(pdna_origin_art_portrait(&m, 0, &a, &o), "C1 should return art");
  CHECK(a.px == g_front, "C1 must be the Gen-3 FRONT sprite");
  CHECK_EQ(a.w, 64, "C1 width");  CHECK_EQ(a.h, 64, "C1 height");
  CHECK_EQ(a.gen, PDNA_GEN3, "C1 pixels are Gen 3");
  CHECK_EQ(a.era, PDNA_GEN1, "C1 era still reports the true provenance");
  CHECK_EQ(a.era_certain, 0, "C1 ...and reports that it is not proven");
  CHECK_EQ(g_gb_calls, 0, "C1 must not touch a GB source that is not registered");

  /* C1b: back requested -> back sprite, exactly as draw_left's `g_back` branch. */
  CHECK(pdna_origin_art_portrait(&m, 1, &a, 0), "C1b should return art");
  CHECK(a.px == g_back, "C1b must be the Gen-3 BACK sprite");

  /* C2: artless build, no GB source => NULL, i.e. the caller paints the name chip. */
  g_art_on = 0;
  CHECK(!pdna_origin_art_portrait(&m, 0, &a, 0), "C2 artless must report no art");
  CHECK(a.px == 0, "C2 px must be NULL so the chip fallback runs");

  /* C3: a GB source registered => Gen-1 art at its native size. */
  gb_source_on(); g_art_on = 1; g_gb_calls = 0;
  CHECK(pdna_origin_art_portrait(&m, 0, &a, &o), "C3 should return art");
  CHECK(a.px == g_gbpix, "C3 must be the Gen-1 picture");
  CHECK_EQ(a.w, 56, "C3 native width");  CHECK_EQ(a.h, 56, "C3 native height");
  CHECK_EQ(a.gen, PDNA_GEN1, "C3 pixels are Gen 1");
  CHECK_EQ(g_gb_last_gen, 1, "C3 asked the right era");
  CHECK_EQ(g_gb_last_dex, 1, "C3 asked for national dex 1");
  CHECK_EQ(g_gb_last_form, 0, "C3 a non-Unown must ask for form 0");
  CHECK_EQ(g_gb_calls, 1, "C3 exactly one fetch");

  /* C3b: THE PER-FRAME COST. pdna_summary.c's portrait_redraw runs this once per FRAME
   * while the summary animates, and on hardware a GB fetch is an SD read plus an RLE
   * decode. Asking for the SAME picture again must not touch the source at all. */
  for (int i = 0; i < 60; i++) CHECK(pdna_origin_art_portrait(&m, 0, &a, 0), "C3b frame");
  CHECK_EQ(g_gb_calls, 1, "C3b 60 more frames must cost ZERO extra fetches");
  CHECK(a.px == g_gbpix, "C3b the memoised answer is the same picture");

  /* C4: back requested, the source has no back art => front, never nothing — and that
   * two-call answer is memoised as ONE answer, so holding the summary on the back
   * sprite does not re-run the failed back fetch every frame either. */
  g_gb_back_on = 0; g_gb_calls = 0;
  CHECK(pdna_origin_art_portrait(&m, 1, &a, 0), "C4 should return art");
  CHECK(a.px == g_gbpix, "C4 must fall back to the GB FRONT picture");
  CHECK_EQ(g_gb_calls, 2, "C4 should have tried back then front");
  CHECK_EQ(g_gb_last_back, 0, "C4 last call was the front");
  for (int i = 0; i < 30; i++) pdna_origin_art_portrait(&m, 1, &a, 0);
  CHECK_EQ(g_gb_calls, 2, "C4 30 more back-side frames must cost ZERO extra fetches");

  /* C4b: the memo is keyed on the request, not on nothing — a different mon, a
   * different side and a re-registration all re-fetch. */
  {
    Gb12Mon g2; uint8_t r2[80]; PkMon m2;
    gb1_mon(&g2, 4); g2.slot_salt = 3;                 /* Charmander */
    if (make(&g2, r2, &m2)) {
      g_gb_calls = 0;
      pdna_origin_art_portrait(&m2, 0, &a, 0);
      CHECK_EQ(g_gb_calls, 1, "C4b a different species must re-fetch");
      pdna_origin_art_portrait(&m, 0, &a, 0);
      CHECK_EQ(g_gb_calls, 2, "C4b going back to the first mon must re-fetch");
      pdna_origin_art_portrait(&m, 0, &a, 0);
      CHECK_EQ(g_gb_calls, 2, "C4b ...and then memoise again");
      gb_source_on();                                  /* re-register = new buffer    */
      pdna_origin_art_portrait(&m, 0, &a, 0);
      CHECK_EQ(g_gb_calls, 3, "C4b re-registering must drop the memo");
    }
  }

  /* C5: the era's ROM is not registered => Gen-3 art, and the tag still tells truth. */
  g_gb1_on = 0; g_gb_calls = 0;
  CHECK(pdna_origin_art_portrait(&m, 0, &a, &o), "C5 should return art");
  CHECK(a.px == g_front, "C5 must fall back to Gen-3 art");
  CHECK_EQ(a.era, PDNA_GEN1, "C5 era still says Gen 1");
  CHECK_EQ(a.gen, PDNA_GEN3, "C5 pixels are Gen 3");
  CHECK(strcmp(pdna_origin_tag(&o), "GB?") == 0, "C5 tag survives the art fallback");
  g_gb1_on = 1;

  /* C6: a NATIVE Gen-3 mon never consults the GB source at all. */
  {
    uint8_t nat[80]; PkMon nm;
    gen3_build_mon(300, 40, 0x1234ABCDu, 0x00010002u, "GUY", 3, nat);
    if (pk_decode_mon(nat, false, &nm)) {
      pk_resolve(&nm);
      g_gb_calls = 0;
      CHECK(pdna_origin_art_portrait(&nm, 0, &a, &o), "C6 should return art");
      CHECK_EQ(o.verdict, PDNA_ORIGIN_NATIVE, "C6 must be native");
      CHECK(a.px == g_front, "C6 must be Gen-3 art");
      CHECK_EQ(a.era_certain, 1, "C6 a native Gen-3 mon IS certain");
      CHECK_EQ(g_gb_calls, 0, "C6 must not decode GB art for a native mon");
      CHECK(strcmp(pdna_origin_tag(&o), "") == 0, "C6 no tag on a native mon");
    }
  }

  /* C7: THE UNOWN LETTER. gen12_convert.c solves the PID so that Gen 3's own
   * pk_unown_form reproduces the Gen-2 letter (gen12_unown_letter). Twenty-six
   * different pictures hang off that byte, so the router has to hand it to the source
   * — a vtable without a form parameter drew letter A for all of them. */
  {
    static const struct { uint8_t atk, def, spd, spc; } LET[] = {
      { 9, 6, 12, 3 },      /* -> F */
      { 2, 2,  2, 2 },      /* -> I */
      { 15, 15, 15, 15 },   /* -> Z, the top of the range */
    };
    for (unsigned i = 0; i < sizeof LET / sizeof LET[0]; i++) {
      Gb12Mon gu; uint8_t ru[80]; PkMon mu;
      gb2_mon(&gu, 201);                               /* Unown, a Gen-2 species */
      gu.dv_atk = LET[i].atk; gu.dv_def = LET[i].def;
      gu.dv_spd = LET[i].spd; gu.dv_spc = LET[i].spc;
      gu.slot_salt = 100 + i;
      if (!make(&gu, ru, &mu)) continue;
      uint8_t want = gen12_unown_letter(LET[i].atk, LET[i].def, LET[i].spd, LET[i].spc);
      CHECK_EQ(mu.form, want, "C7 the import should carry letter %u", want);
      gb_source_on();                                  /* drop the memo */
      g_gb_calls = 0;
      CHECK(pdna_origin_art_portrait(&mu, 0, &a, &o), "C7 should return art");
      CHECK_EQ(o.gen, PDNA_GEN2, "C7 Unown is a Gen-2 species");
      CHECK_EQ(g_gb_last_dex, 201, "C7 asked for Unown");
      CHECK_EQ(g_gb_last_form, want, "C7 the LETTER must reach the source");
      printf("    Unown DVs %2u/%2u/%2u/%2u -> letter %c reached the art source\n",
             LET[i].atk, LET[i].def, LET[i].spd, LET[i].spc, 'A' + want);
    }
    /* A Gen-3-only Unown form (! or ?) has no Gen-2 picture; ask for A rather than a
     * form the source must reject. */
    {
      Gb12Mon gu; uint8_t ru[80]; PkMon mu;
      gb2_mon(&gu, 201); gu.slot_salt = 200;
      if (make(&gu, ru, &mu)) {
        mu.form = 27;                                   /* '?' — Gen 3 only */
        gb_source_on(); g_gb_calls = 0;
        pdna_origin_art_portrait(&mu, 0, &a, 0);
        CHECK_EQ(g_gb_last_form, 0, "C7 a Gen-3-only Unown form falls back to A");
      }
    }
  }

  /* C8: placement. 64x64 in the summary's (12,14,68,64) frame must land on today's
   * literal (14,14); a 56x56 GB pic is centred and stands on the same floor. */
  {
    int x, y;
    PdnaArt p64 = { 0, 64, 64, 3, 3, 1, 0 };
    pdna_origin_art_place(&p64, 12, 14, 68, 64, &x, &y);
    CHECK_EQ(x, 14, "C8 64x64 x");  CHECK_EQ(y, 14, "C8 64x64 y");
    PdnaArt p56 = { 0, 56, 56, 1, 1, 0, 0 };
    pdna_origin_art_place(&p56, 12, 14, 68, 64, &x, &y);
    CHECK_EQ(x, 18, "C8 56x56 x (centred)");
    CHECK_EQ(y, 22, "C8 56x56 y (feet on the same floor, 22+56 == 14+64)");
    PdnaArt big = { 0, 80, 80, 1, 1, 0, 0 };
    pdna_origin_art_place(&big, 12, 14, 68, 64, &x, &y);
    CHECK_EQ(x, 12, "C8 oversized art is clamped, never negative");
    CHECK_EQ(y, 14, "C8 oversized art is clamped, never negative");
  }

  /* C9: the box cache, from RAW records -- pdna_bank.c's door. */
  {
    static uint8_t recs[30 * 80];
    memset(recs, 0, sizeof recs);
    memcpy(recs + 0 * 80, rec, 80);                       /* the Gen-1 import      */
    Gb12Mon g2; gb2_mon(&g2, 158); g2.slot_salt = 11;     /* Totodile -> Gen 2     */
    uint8_t r2[80]; PkMon m2;
    if (make(&g2, r2, &m2)) memcpy(recs + 3 * 80, r2, 80);
    uint8_t nat[80];
    gen3_build_mon(300, 40, 0x1234ABCDu, 0x00010002u, "GUY", 3, nat);
    memcpy(recs + 7 * 80, nat, 80);                       /* a native Gen-3 mon    */

    pdna_origin_box_note_records(recs);
    CHECK_EQ(pdna_origin_box_mark(0), '?', "C9 slot 0 = GB import, era unproven");
    CHECK_EQ(pdna_origin_box_mark(3), '2', "C9 slot 3 = proven Gen-2 import");
    CHECK_EQ(pdna_origin_box_mark(7), 0,   "C9 slot 7 = native, no marker");
    CHECK_EQ(pdna_origin_box_mark(9), 0,   "C9 slot 9 = empty, no marker");
    CHECK_EQ(pdna_origin_box_gen(0), 1, "C9 slot 0 era");
    CHECK_EQ(pdna_origin_box_gen(3), 2, "C9 slot 3 era");
    CHECK_EQ(pdna_origin_box_gen(7), 3, "C9 slot 7 era");
    CHECK_EQ(pdna_origin_box_gen(9), 0, "C9 empty slot has no era");
    CHECK_EQ(pdna_origin_box_count(1), 1, "C9 one Gen-1 cell");
    CHECK_EQ(pdna_origin_box_count(2), 1, "C9 one Gen-2 cell");
    CHECK_EQ(pdna_origin_box_count(3), 1, "C9 one Gen-3 cell");
    /* An unproven cell must not wear an era's colour — that is the "neutral
     * presentation" half of the fix, and it has to hold in the grid too. */
    PdnaOrigin q; memset(&q, 0, sizeof q);
    q.verdict = PDNA_ORIGIN_GB; q.gen = PDNA_GEN1; q.gen_certain = 1;
    uint16_t col_gen1 = pdna_origin_color(&q);
    q.gen = PDNA_GEN2;
    uint16_t col_gen2 = pdna_origin_color(&q);
    CHECK(pdna_origin_box_color(0) != 0, "C9 an unproven cell still gets a tint");
    CHECK(pdna_origin_box_color(0) != col_gen1 && pdna_origin_box_color(0) != col_gen2,
          "C9 an unproven cell must not be branded as either era");
    CHECK_EQ(pdna_origin_box_color(3), col_gen2, "C9 a proven Gen-2 cell IS branded");
    CHECK_EQ(pdna_origin_box_mark(-1), 0, "C9 out-of-range slot is silent");
    CHECK_EQ(pdna_origin_box_mark(30), 0, "C9 out-of-range slot is silent");
    pdna_origin_box_clear();
    CHECK_EQ(pdna_origin_box_mark(0), 0, "C9 cleared cache reports nothing");
    printf("    cache: 30 cells from raw records, markers ? / 2 / (none)\n");
  }
  gb_source_off();
  g_art_on = 0;
}

/* ---- F. THE PARALLEL BANK GRID ----------------------------------------------------
 * The half of Guy's request the grid actually draws: every cell in the art of its own
 * generation, all at the same time. Part C proved the ROUTER picks the right picture;
 * this part proves the two things the GRID adds on top of it —
 *
 *   the CELL SCALER (pdna_origin_cell_render): a Game Boy pic is 56x56 and a box cell
 *   is 24x22, so the grid can only show era art if the reduction keeps the aspect
 *   ratio, keeps every era standing on the same floor, and above all keeps the
 *   SILHOUETTE, which is the whole thing that makes a Gen-1 sprite recognisable; and
 *
 *   the CHEAP DOOR (pdna_origin_box_art_wanted): the grid asks it 30 times on every
 *   repaint to decide whether a cell's Gen-3 icon gets hidden, so it must answer from
 *   the cache and the source's have() probe WITHOUT decoding anything. If it ever
 *   reached the card, a box flip would cost 30 SD decodes on a save with no Game Boy
 *   Pokemon in it at all.
 */

/* Fill an art buffer, in the RGB15 + 0x8000 form the sources hand back. `fn` gets
 * (x, y) and returns 0 for transparent. */
static void art_fill(uint16_t* px, int w, int h, uint16_t (*fn)(int, int)) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) px[y * w + x] = fn(x, y);
}
static uint16_t fill_solid(int x, int y)  { (void)x; (void)y; return 0x8000u | 0x1234u; }
/* Each ROW a distinct constant. The stride trap: read with the codec's fixed 56 instead
 * of the art's own width and a row stops being constant. */
static uint16_t fill_rows(int x, int y)   { (void)x; return (uint16_t)(0x8000u | (y + 1)); }
/* One pixel wide, full height: the feature plain point sampling deletes. */
static uint16_t fill_hair(int x, int y)   { (void)y; return x == 28 ? (uint16_t)(0x8000u | 0x7FFFu) : 0u; }

#define CELL_W 24
#define CELL_H 22

static void part_f(void) {
  printf("F. the parallel bank grid\n");
  static uint16_t src[64 * 64];
  uint16_t cell[CELL_W * CELL_H];
  PdnaArt a; memset(&a, 0, sizeof a);

  /* F1: a 56x56 Game Boy pic in a 24x22 cell. Aspect kept (22x22), centred (x0 = 1),
   * standing on the cell floor (y0 = 0 here, because it is as tall as the cell). */
  art_fill(src, 56, 56, fill_solid);
  a.px = src; a.w = 56; a.h = 56; a.gen = PDNA_GEN1;
  CHECK(pdna_origin_cell_render(&a, cell, CELL_W, CELL_H), "F1 should render");
  {
    int minx = CELL_W, maxx = -1, miny = CELL_H, maxy = -1;
    for (int y = 0; y < CELL_H; y++)
      for (int x = 0; x < CELL_W; x++)
        if (cell[y * CELL_W + x] & 0x8000) {
          if (x < minx) minx = x;  if (x > maxx) maxx = x;
          if (y < miny) miny = y;  if (y > maxy) maxy = y;
        }
    CHECK_EQ(maxx - minx + 1, 22, "F1 width kept square");
    CHECK_EQ(maxy - miny + 1, 22, "F1 height kept square");
    CHECK_EQ(minx, 1, "F1 centred horizontally");
    CHECK_EQ(maxy, CELL_H - 1, "F1 feet on the cell floor");
    printf("    56x56 -> %dx%d at (%d,%d) in a %dx%d cell\n",
           maxx - minx + 1, maxy - miny + 1, minx, miny, CELL_W, CELL_H);
  }

  /* F2: BOTTOM-ANCHORED, not centred vertically. A short, wide picture must stand on
   * the floor beside a tall one -- that is what stops two eras' art floating at
   * different heights in neighbouring cells. */
  art_fill(src, 40, 20, fill_solid);
  a.px = src; a.w = 40; a.h = 20;
  CHECK(pdna_origin_cell_render(&a, cell, CELL_W, CELL_H), "F2 should render");
  {
    int first = -1;
    for (int y = 0; y < CELL_H && first < 0; y++)
      for (int x = 0; x < CELL_W; x++)
        if (cell[y * CELL_W + x] & 0x8000) { first = y; break; }
    CHECK_EQ(first, 10, "F2 40x20 fills 24x12 and sits on the floor (rows 10..21)");
    for (int x = 0; x < CELL_W; x++)
      CHECK_EQ(cell[(CELL_H - 1) * CELL_W + x] & 0x8000, 0x8000, "F2 bottom row opaque");
  }

  /* F3: THE STRIDE TRAP. a->px is the PACKED RGB15 expansion, stride a->w -- NOT the
   * codec's fixed GB_SPRITE_MAX_W of 56. Reading it with 56 shears every Gen-1 pic that
   * is not 7x7 tiles, which is most of them (5x5 and 6x6 are 40 and 48 px wide). Each
   * SOURCE row here is one constant colour, so a sheared read is exactly the thing that
   * makes a rendered row stop being constant. */
  art_fill(src, 40, 40, fill_rows);
  a.px = src; a.w = 40; a.h = 40;
  CHECK(pdna_origin_cell_render(&a, cell, CELL_W, CELL_H), "F3 should render");
  {
    int sheared = 0;
    for (int y = 0; y < CELL_H; y++) {
      uint16_t seen = 0;
      for (int x = 0; x < CELL_W; x++) {
        uint16_t p = cell[y * CELL_W + x];
        if (!(p & 0x8000)) continue;
        if (!seen) seen = p;
        else if (p != seen) sheared++;
      }
    }
    CHECK_EQ(sheared, 0, "F3 a 40px-wide pic read with a 56 stride would shear "
                         "(%d pixels came from the wrong row)", sheared);
  }

  /* F4: THE SILHOUETTE SURVIVES. A 1px feature at 2.5x reduction is exactly what plain
   * point sampling throws away -- a tail, an ear, Pikachu's bolt. Every destination row
   * must still carry it. */
  memset(src, 0, sizeof src);
  art_fill(src, 56, 56, fill_hair);
  a.px = src; a.w = 56; a.h = 56;
  CHECK(pdna_origin_cell_render(&a, cell, CELL_W, CELL_H), "F4 should render");
  {
    int rows_with_ink = 0;
    for (int y = 0; y < CELL_H; y++)
      for (int x = 0; x < CELL_W; x++)
        if (cell[y * CELL_W + x] & 0x8000) { rows_with_ink++; break; }
    CHECK_EQ(rows_with_ink, 22, "F4 a one-pixel feature must survive the reduction "
                                "(%d of 22 rows kept it)", rows_with_ink);
  }

  /* F5: refusals write nothing. `cell` keeps a sentinel so a scaler that cleared the
   * buffer before validating would be caught. */
  cell[0] = 0xDEAD;
  a.px = 0; a.w = 56; a.h = 56;
  CHECK(!pdna_origin_cell_render(&a, cell, CELL_W, CELL_H), "F5 no art -> no render");
  a.px = src; a.w = 0;
  CHECK(!pdna_origin_cell_render(&a, cell, CELL_W, CELL_H), "F5 zero width -> no render");
  a.w = 56;
  CHECK(!pdna_origin_cell_render(&a, cell, 0, CELL_H), "F5 zero cell -> no render");
  CHECK(!pdna_origin_cell_render(&a, 0, CELL_W, CELL_H), "F5 NULL dst -> no render");
  CHECK_EQ(cell[0], 0xDEAD, "F5 a refused render must not touch the caller's buffer");

  /* F6: the cheap door. This is what the grid asks 30 times per repaint. */
  {
    Gb12Mon g; uint8_t rec[80]; PkMon m;
    gb1_mon(&g, 1);
    if (!make(&g, rec, &m)) return;
    static uint8_t recs[30 * 80];
    memset(recs, 0, sizeof recs);
    memcpy(recs + 0 * 80, rec, 80);                        /* Gen-1 import, unproven */
    Gb12Mon g2; gb2_mon(&g2, 158); g2.slot_salt = 11;
    uint8_t r2[80]; PkMon m2;
    if (make(&g2, r2, &m2)) memcpy(recs + 3 * 80, r2, 80); /* proven Gen-2 import    */
    uint8_t nat[80];
    gen3_build_mon(300, 40, 0x1234ABCDu, 0x00010002u, "GUY", 3, nat);
    memcpy(recs + 7 * 80, nat, 80);                        /* a native Gen-3 mon     */
    pdna_origin_box_note_records(recs);

    CHECK_EQ(pdna_origin_box_gb(0), 1, "F6 slot 0 is a GB import");
    CHECK_EQ(pdna_origin_box_gb(3), 1, "F6 slot 3 is a GB import");
    CHECK_EQ(pdna_origin_box_gb(7), 0, "F6 slot 7 is native");
    CHECK_EQ(pdna_origin_box_gb(9), 0, "F6 slot 9 is empty");
    CHECK_EQ(pdna_origin_box_gb(-1), 0, "F6 out-of-range is silent");
    CHECK_EQ(pdna_origin_box_gb(30), 0, "F6 out-of-range is silent");

    gb_source_off();
    CHECK_EQ(pdna_origin_box_art_wanted(0), 0, "F6 no source -> no cell wants art");
    CHECK_EQ(pdna_origin_box_art_wanted(3), 0, "F6 no source -> no cell wants art");

    gb_source_on(); g_gb1_on = 1; g_gb2_on = 1;
    g_gb_calls = 0;
    CHECK_EQ(pdna_origin_box_art_wanted(0), 1, "F6 Gen-1 cell wants Gen-1 art");
    CHECK_EQ(pdna_origin_box_art_wanted(3), 1, "F6 Gen-2 cell wants Gen-2 art");
    CHECK_EQ(pdna_origin_box_art_wanted(7), 0, "F6 a native cell never wants GB art");
    CHECK_EQ(pdna_origin_box_art_wanted(9), 0, "F6 an empty cell never wants GB art");
    CHECK_EQ(pdna_origin_box_art_wanted(-1), 0, "F6 out-of-range is silent");
    CHECK_EQ(pdna_origin_box_art_wanted(30), 0, "F6 out-of-range is silent");
    /* THE COST CLAIM, asserted: 30 of these must not read one byte off the card. */
    for (int s = 0; s < 30; s++) (void)pdna_origin_box_art_wanted(s);
    CHECK_EQ(g_gb_calls, 0, "F6 art_wanted must never decode a picture -- a box flip on "
                            "a save with no GB mons has to cost zero card reads");

    /* THE DEGRADE, cell by cell: with the Gen-1 ROM gone that cell stops wanting art
     * but is still a Gen-1 import wearing its marker. That is layer 2 doing its job. */
    g_gb1_on = 0;
    CHECK_EQ(pdna_origin_box_art_wanted(0), 0, "F6 no Gen-1 ROM -> no Gen-1 art");
    CHECK_EQ(pdna_origin_box_gb(0), 1, "F6 ...but it is still a GB import");
    CHECK_EQ(pdna_origin_box_mark(0), '?', "F6 ...and it still wears its marker");
    CHECK_EQ(pdna_origin_box_art_wanted(3), 1, "F6 the Gen-2 cell is unaffected");
    g_gb1_on = 1;

    /* F7: the memo, and the grid's escape hatch from it. The router caches the last
     * fetched pointer INTO THE SOURCE'S OWN BUFFER; the box grid then streams a Gen-3
     * portrait through what may be that same buffer, so it calls invalidate() and a
     * later identical request must go back to the source instead of handing out
     * pixels that are no longer there. */
    g_gb_calls = 0;
    PdnaArt art;
    CHECK(pdna_origin_box_art(0, &m, &art), "F7 should serve era art");
    CHECK_EQ(art.gen, PDNA_GEN1, "F7 Gen-1 cell gets Gen-1 pixels");
    CHECK_EQ(g_gb_calls, 1, "F7 one fetch");
    CHECK(pdna_origin_box_art(0, &m, &art), "F7 repeat should serve art");
    CHECK_EQ(g_gb_calls, 1, "F7 a repeat is memoised, not re-fetched");
    pdna_origin_art_invalidate();
    CHECK(pdna_origin_box_art(0, &m, &art), "F7 post-invalidate should serve art");
    CHECK_EQ(g_gb_calls, 2, "F7 invalidate must force a re-fetch");

    /* F8: end to end -- the cell the grid actually blits, for a real cell of the cache. */
    art_fill(g_gbpix, 56, 56, fill_rows);
    pdna_origin_art_invalidate();
    if (pdna_origin_box_art(0, &m, &art) && art.gen == PDNA_GEN1) {
      CHECK(pdna_origin_cell_render(&art, cell, CELL_W, CELL_H),
            "F8 the grid's cell image renders");
      int opaque = 0;
      for (int i = 0; i < CELL_W * CELL_H; i++) if (cell[i] & 0x8000) opaque++;
      CHECK_EQ(opaque, 22 * 22, "F8 a 56x56 pic fills exactly 22x22 of the cell");
      printf("    cell pipeline: cache -> era art -> %d opaque px in a %dx%d cell\n",
             opaque, CELL_W, CELL_H);
    }
    pdna_origin_box_clear();
    gb_source_off();
  }
}

/* ---- D. FALSE POSITIVES ON REAL GEN-3 CARTRIDGE DATA ------------------------------ */

static int g_real_mons = 0, g_real_fp = 0, g_real_saves = 0;
static int g_real_trades = 0, g_real_sid0 = 0;

/* Where every real mon stops. Two things this measures that a pass/fail cannot:
 * how much work the detector actually does on a native mon (the box-flip cost), and
 * WHICH clause is carrying the load on the ambiguous population. */
static const char* const MISSNAME[] = {
  "OK(GB import)", "species/egg", "secret id", "met location", "ball", "language",
  "odd IV", "SpA!=SpD", "HP IV lattice", "EV", "contest/ribbon", "nature", "shiny"
};
static int g_miss_all[13], g_miss_hard[13];

static void visit_real(const PkMon* m, const char* save) {
  if (m->species == 0) return;
  g_real_mons++;
  int hard = 0;
  if (m->metLocation == 0xFE) { g_real_trades++; hard = 1; }
  if ((m->otId >> 16) == 0)   { g_real_sid0++;   hard = 1; }
  PdnaOrigin o;
  pdna_origin_of(m, &o);
  if (o.miss < 13) { g_miss_all[o.miss]++; if (hard) g_miss_hard[o.miss]++; }
  /* NEAR MISSES: a real mon that satisfied all four stamped fields and had to be
   * rejected by a later clause. These are the whole safety margin, so name them
   * rather than only counting them. */
  if (o.miss >= PDNA_SIG_IV_ODD)
    printf("    near miss in %s: %-11s met %u ball %u SID %u IVs %u/%u/%u/%u/%u/%u"
           " -> rejected by %s\n",
           save, pk_species_name(m->species), m->metLocation, m->pokeball,
           (unsigned)(m->otId >> 16), m->ivs[0], m->ivs[1], m->ivs[2], m->ivs[3],
           m->ivs[4], m->ivs[5], MISSNAME[o.miss]);
  if (o.verdict == PDNA_ORIGIN_GB) {
    g_real_fp++;
    printf("    FALSE POSITIVE in %s: %-11s met %u ball %u SID %u IVs %u/%u/%u/%u/%u/%u\n",
           save, pk_species_name(m->species), m->metLocation, m->pokeball,
           (unsigned)(m->otId >> 16), m->ivs[0], m->ivs[1], m->ivs[2], m->ivs[3],
           m->ivs[4], m->ivs[5]);
  }
}

static void part_d_save(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  (skip, cannot open %s)\n", path); return; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("  parse FAILED: %s\n", path); g_fail++; return; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES], pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);
  g_real_saves++;

  const char* base = strrchr(path, '/');
  base = base ? base + 1 : path;
  int before_m = g_real_mons, before_fp = g_real_fp, before_t = g_real_trades;

  PkMon party[6]; bool frlg = false;
  int np = pk_read_party_auto(sb1, party, &frlg);
  for (int i = 0; i < np; i++) { pk_resolve(&party[i]); visit_real(&party[i], base); }
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    PkMon box[30];
    pk_read_box(pc, b, box);
    for (int s = 0; s < 30; s++) if (box[s].species) { pk_resolve(&box[s]); visit_real(&box[s], base); }
  }
  printf("    %-14s %4d mons, %2d met \"in a trade\", %d false positives\n",
         base, g_real_mons - before_m, g_real_trades - before_t, g_real_fp - before_fp);
}

static void part_d(int argc, char** argv) {
  printf("D. false positives over Guy's real Gen-3 cartridge saves\n");
  if (argc <= 1) { printf("    (no saves given -- pass them as argv[1..]; SKIPPED)\n"); return; }
  for (int i = 1; i < argc; i++) part_d_save(argv[i]);
  printf("    TOTAL %d saves, %d mons, %d with SID 0, %d met \"in a trade\", "
         "%d FALSE POSITIVES\n", g_real_saves, g_real_mons, g_real_sid0,
         g_real_trades, g_real_fp);
  printf("    where they stop      all    of which SID-0 or in-a-trade\n");
  for (int i = 0; i < 13; i++)
    if (g_miss_all[i])
      printf("      %-16s %5d    %d\n", MISSNAME[i], g_miss_all[i], g_miss_hard[i]);
  CHECK_EQ(g_real_fp, 0, "D a real Pokemon was called a Game Boy import");
  /* The gate is only meaningful if the saves really do contain the ambiguous
   * population. If they do not, say so rather than claiming a clean sweep. */
  if (g_real_saves && g_real_trades == 0)
    printf("    NOTE: no in-game-trade mon was present, so the hardest case went untested.\n");
}

/* ---- E. ERA FALSE POSITIVES OVER GUY'S REAL GAME BOY SAVES ------------------------
 *
 * Ground truth for once: every record here IS from the generation the file came from,
 * and the REAL converter produces the Gen-3 record. Then three things an ordinary
 * Gen-3 game does to a Pokemon are applied, in every combination, and the label is
 * re-read. A Gen-1 mon that comes back as a CONFIDENT "GB2" is the defect. */

enum { MUT_WALK = 1, MUT_PRUS = 2, MUT_TM = 4 };
static const int E_MASK[8] = { 0, MUT_WALK, MUT_PRUS, MUT_TM, MUT_WALK | MUT_PRUS,
                               MUT_WALK | MUT_TM, MUT_PRUS | MUT_TM,
                               MUT_WALK | MUT_PRUS | MUT_TM };
static const char* const E_NAME[8] = {
  "fresh import", "+walked", "+Pokerus", "+taught an HM", "+walk+prus",
  "+walk+HM", "+prus+HM", "+all three"
};

static uint8_t e_img[64 * 1024];
static int e_saves, e_conv;
static int e_cert2[8], e_cert1[8], e_unsure[8], e_native[8];
static int e_wrong;                     /* confident labels contradicting the truth */

static uint32_t e_load(const char* file) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", GBROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(e_img, 1, sizeof e_img, f);
  fclose(f);
  return n;
}

static void e_one(const Gb12Mon* g, int truth_gen) {
  uint8_t rec[80];
  Gb12Target t; memset(&t, 0, sizeof t); t.met_game = 3;
  if (gen12_convert(g, &t, rec, 0) != GB12_OK) return;
  e_conv++;
  for (int i = 0; i < 8; i++) {
    uint8_t r[80]; memcpy(r, rec, 80);
    if (E_MASK[i]) {
      EditMon e;
      gen3_edit_load(r, false, &e);
      if (E_MASK[i] & MUT_WALK) {       /* 128 steps in the party moves friendship  */
        PkMon pm;
        if (pk_decode_mon(r, false, &pm))
          em_set_friendship(&e, (uint8_t)(pm.friendship < 255 ? pm.friendship + 1 : 254));
      }
      if (E_MASK[i] & MUT_PRUS) e.sub[3][0] = 0x11;      /* caught it in Gen 3       */
      if (E_MASK[i] & MUT_TM)   em_set_move(&e, 3, 249); /* Rock Smash, HM06         */
      gen3_edit_commit(&e, r);
    }
    PkMon m;
    if (!pk_decode_mon(r, false, &m)) continue;
    pk_resolve(&m);
    PdnaOrigin o;
    pdna_origin_of(&m, &o);
    if      (o.verdict != PDNA_ORIGIN_GB) e_native[i]++;
    else if (!o.gen_certain)              e_unsure[i]++;
    else if (o.gen == PDNA_GEN2)        { e_cert2[i]++; if (truth_gen == 1) e_wrong++; }
    else                                { e_cert1[i]++; if (truth_gen == 2) e_wrong++; }
  }
}

static void e_gen1(const char* file) {
  uint32_t n = e_load(file);
  if (!n) { printf("    SKIP %s (not present)\n", file); return; }
  Gen1Save s;
  if (gen1_open(e_img, n, &s) != GEN1_OK) { printf("    %s: does not parse\n", file); return; }
  e_saves++;
  for (int b = 0; b <= GEN1_PARTY_BOX; b++) {
    int cnt = gen1_count(&s, b);
    for (int i = 0; i < cnt; i++) {
      Gen1Mon gm;
      if (!gen1_decode_image(&s, e_img, b, i, &gm) || gm.dex == 0) continue;
      Gb12Mon g;
      gen12_from_gen1(&gm, (uint32_t)b * 20u + (uint32_t)i, &g);
      e_one(&g, 1);
    }
  }
}

static void e_gen2(const char* file) {
  uint32_t n = e_load(file);
  if (!n) { printf("    SKIP %s (not present)\n", file); return; }
  G2Scan sc; g2_scan_begin(&sc);
  for (uint32_t o = 0; o < n; o += 256) {
    uint32_t c = (n - o) < 256 ? (n - o) : 256;
    g2_scan_feed(&sc, o, e_img + o, c);
  }
  G2Save sv; g2_scan_finish(&sc, n, &sv);
  G2Header hd;
  if (!sv.supported || !g2_read_header(e_img, &sv, &hd)) {
    printf("    %s: does not parse\n", file); return;
  }
  e_saves++;
  for (int b = 0; b <= G2_BOX_PARTY; b++) {
    int cnt = g2_box_count_at(e_img, &sv, &hd, b);
    for (int i = 0; i < cnt; i++) {
      G2Mon gm;
      if (!g2_box_mon_at(e_img, &sv, &hd, b, i, &gm)) continue;
      Gb12Mon g;
      gen12_from_gen2(&gm, (uint32_t)b * 20u + (uint32_t)i, &g);
      e_one(&g, 2);
    }
  }
}

static void e_report(const char* what, int truth_gen) {
  printf("    %s: %d converted records x 8 play states\n", what, e_conv);
  printf("      %-14s %7s %7s %7s %7s\n", "state", "GB1", "GB2", "GB?", "native");
  for (int i = 0; i < 8; i++) {
    int bad = (truth_gen == 1) ? e_cert2[i] : e_cert1[i];
    printf("      %-14s %7d %7d %7d %7d%s\n", E_NAME[i], e_cert1[i], e_cert2[i],
           e_unsure[i], e_native[i], bad ? "   <-- WRONG-ERA CLAIMS" : "");
  }
}

static void e_reset(void) {
  e_conv = 0;
  memset(e_cert1, 0, sizeof e_cert1); memset(e_cert2, 0, sizeof e_cert2);
  memset(e_unsure, 0, sizeof e_unsure); memset(e_native, 0, sizeof e_native);
}

static void part_e(void) {
  printf("E. era false positives over Guy's real Game Boy saves\n");
  e_wrong = 0;
  e_reset(); e_gen1("Red.sav");
  int n1 = e_conv;
  if (n1) e_report("GEN 1 (Red.sav)", 1);
  e_reset(); e_gen2("Gold.sav"); e_gen2("Crystal.sav");
  int n2 = e_conv;
  if (n2) e_report("GEN 2 (Gold.sav + Crystal.sav)", 2);
  if (!e_saves) { printf("    (no Game Boy corpus present -- SKIPPED)\n"); return; }
  printf("    TOTAL %d saves, %d Gen-1 + %d Gen-2 records, %d WRONG-ERA CONFIDENT "
         "LABELS out of %d judgements\n", e_saves, n1, n2, e_wrong, (n1 + n2) * 8);
  CHECK_EQ(e_wrong, 0, "E a Pokemon was confidently labelled with the wrong Game Boy era");
}

int main(int argc, char** argv) {
  printf("== pdna_origin_art host test ==\n");
  part_a();
  part_b();
  part_c();
  part_f();             /* the grid the bank draws */
  part_d(argc, argv);   /* argv[1..] = saves */
  part_e();
  printf("== %d checks, %d failures ==\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
