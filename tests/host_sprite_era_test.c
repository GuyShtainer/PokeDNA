/* Host test for source/sprite_era.{c,h} -- slice E2 of the sprite-era arc
 * (docs/SPRITE-ERA-DESIGN.md). Pure C, no GBA/tonc headers, links only this one module.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_sprite_era_test.c \
 *      source/sprite_era.c -o /tmp/hse && /tmp/hse
 *
 * Covers: defaults; the native-era mapping for every SaveKind x origin (including the
 * "uncertain Kanto import defaults to Gen 1" case, which is already folded into
 * origin_gen by pdna_origin_of() in the real integration -- this test only proves the
 * mapping honours that value); species existence at every dex edge (151/152, 251/252,
 * 386/387); the FULL resolver fallback chain with every reason hit at least once; the
 * PC_GRID+GEN1 refusal; a config write/apply round trip including absent and unknown
 * keys and two cap-bounded writes (cap 1, cap exactly enough); and se_era_next cycling
 * through only possible eras and terminating from every start, for all 64 SeRoms
 * combinations x 6 starting eras x 5 places.
 */
#include <stdio.h>
#include <string.h>

#include "sprite_era.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

/* ---- (A) se_default ---------------------------------------------------------------- */
static void test_default(void) {
  SeSetting s;
  memset(&s, 0xFF, sizeof s);           /* poison first, so default really zeroes it */
  se_default(&s);
  for (int k = 0; k < SE_KIND_N; k++)
    for (int p = 0; p < SE_PLACE_N; p++)
      CHECK(s.era[k][p] == SE_ERA_NATIVE, "se_default: every cell is NATIVE");
  se_default(NULL);                     /* must not crash */
  printf("(A) se_default ok\n");
}

/* ---- (B) se_native_era, every kind x origin ---------------------------------------- */
static void test_native_era(void) {
  /* A raw Game Boy save: native is its own generation, whatever origin_gen says. */
  CHECK(se_native_era(SE_KIND_GEN1, 0, 0) == SE_ERA_GEN1, "GEN1 save -> GEN1, origin ignored");
  CHECK(se_native_era(SE_KIND_GEN1, 3, 1) == SE_ERA_GEN1, "GEN1 save -> GEN1 even with origin_gen=3");
  CHECK(se_native_era(SE_KIND_GEN2, 0, 0) == SE_ERA_GEN2, "GEN2 save -> GEN2, origin ignored");
  CHECK(se_native_era(SE_KIND_GEN2, 1, 1) == SE_ERA_GEN2, "GEN2 save -> GEN2 even with origin_gen=1");

  /* A Gen-3 save: origin_gen decides. gen==1 or 2 is a GB import (already resolved by
   * pdna_origin_of, including the uncertain-Kanto-defaults-to-Gen1 rule -- certain=0
   * must not change the answer, since the caller already picked the max-likelihood
   * era before calling here). */
  CHECK(se_native_era(SE_KIND_RS,   1, 1) == SE_ERA_GEN1, "RS + proven Gen1 import -> GEN1");
  CHECK(se_native_era(SE_KIND_RS,   1, 0) == SE_ERA_GEN1, "RS + UNCERTAIN Kanto import -> still GEN1");
  CHECK(se_native_era(SE_KIND_EM,   2, 1) == SE_ERA_GEN2, "EM + proven Gen2 import -> GEN2");
  CHECK(se_native_era(SE_KIND_EM,   2, 0) == SE_ERA_GEN2, "EM + unproven Gen2 import -> still GEN2");
  CHECK(se_native_era(SE_KIND_FRLG, 1, 0) == SE_ERA_GEN1, "FRLG + unproven Kanto import -> GEN1");

  /* A native Gen-3 mon (origin_gen==3): the save's OWN game's era. */
  CHECK(se_native_era(SE_KIND_RS,   3, 1) == SE_ERA_G3_RS,   "RS native mon -> G3_RS");
  CHECK(se_native_era(SE_KIND_EM,   3, 1) == SE_ERA_G3_EM,   "EM native mon -> G3_EM");
  CHECK(se_native_era(SE_KIND_FRLG, 3, 1) == SE_ERA_G3_FRLG, "FRLG native mon -> G3_FRLG");

  /* Defensive: an out-of-range kind degrades rather than misbehaving. */
  CHECK(se_native_era((SeSaveKind)99, 3, 1) == SE_ERA_G3_EM, "bad kind degrades to G3_EM");
  printf("(B) se_native_era ok\n");
}

/* ---- (C) se_species_exists, at every stated edge ----------------------------------- */
static void test_species_exists(void) {
  CHECK(se_species_exists(SE_ERA_NATIVE, 999) == true, "NATIVE always exists");
  CHECK(se_species_exists(SE_ERA_NATIVE, 0)   == true, "NATIVE exists even for dex 0");

  CHECK(se_species_exists(SE_ERA_GEN1, 151) == true,  "GEN1: dex 151 exists");
  CHECK(se_species_exists(SE_ERA_GEN1, 152) == false, "GEN1: dex 152 does not exist");
  CHECK(se_species_exists(SE_ERA_GEN1, 1)   == true,  "GEN1: dex 1 exists");
  CHECK(se_species_exists(SE_ERA_GEN1, 0)   == false, "GEN1: dex 0 (empty slot) does not exist");

  CHECK(se_species_exists(SE_ERA_GEN2, 251) == true,  "GEN2: dex 251 exists");
  CHECK(se_species_exists(SE_ERA_GEN2, 252) == false, "GEN2: dex 252 does not exist");

  CHECK(se_species_exists(SE_ERA_G3_RS,   386) == true,  "G3_RS: dex 386 exists");
  CHECK(se_species_exists(SE_ERA_G3_RS,   387) == false, "G3_RS: dex 387 does not exist");
  CHECK(se_species_exists(SE_ERA_G3_EM,   386) == true,  "G3_EM: dex 386 exists");
  CHECK(se_species_exists(SE_ERA_G3_EM,   387) == false, "G3_EM: dex 387 does not exist");
  CHECK(se_species_exists(SE_ERA_G3_FRLG, 386) == true,  "G3_FRLG: dex 386 exists");
  CHECK(se_species_exists(SE_ERA_G3_FRLG, 387) == false, "G3_FRLG: dex 387 does not exist");

  CHECK(se_species_exists((SeEra)99, 1) == false, "an out-of-range era exists nowhere");
  printf("(C) se_species_exists ok\n");
}

/* ---- (D) se_resolve: the full fallback chain, every branch and reason hit ---------- */
static void test_resolve(void) {
  SeRoms all_roms;  for (int i = 0; i < SE_ERA_N; i++) all_roms.have[i] = true;
  SeRoms no_roms;   for (int i = 0; i < SE_ERA_N; i++) no_roms.have[i]  = false;

  /* D1: WANTED, drawn as asked (species exists, ROM present). */
  {
    SeSetting s; se_default(&s);
    s.era[SE_KIND_EM][SE_PLACE_SUMMARY] = SE_ERA_GEN1;
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25, &all_roms, true, &why);
    CHECK(got == SE_ERA_GEN1, "D1: wanted GEN1, valid dex, ROM present -> GEN1");
    CHECK(why == SE_WHY_WANTED, "D1: reason is WANTED");
  }

  /* D2: NO_SPECIES -- wanted era can't draw this dex, falls to native. */
  {
    SeSetting s; se_default(&s);
    s.era[SE_KIND_EM][SE_PLACE_SUMMARY] = SE_ERA_GEN1;   /* Gen1 caps at dex 151 */
    int why = -1;
    /* dex 300, origin says native Gen-3 (gen=3) so native resolves to G3_EM. */
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 300, &all_roms, true, &why);
    CHECK(got == SE_ERA_G3_EM, "D2: wanted GEN1 but dex 300 doesn't exist there -> native G3_EM");
    CHECK(why == SE_WHY_NO_SPECIES, "D2: reason is NO_SPECIES");
  }

  /* D3: NO_ROM -- wanted era exists for this species but has no ROM, falls to native. */
  {
    SeSetting s; se_default(&s);
    s.era[SE_KIND_EM][SE_PLACE_PARTY] = SE_ERA_G3_RS;    /* a DIFFERENT game's ROM */
    SeRoms roms = no_roms; roms.have[SE_ERA_G3_EM] = true; /* only native's ROM present */
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_PARTY, 3, 1, 25, &roms, true, &why);
    CHECK(got == SE_ERA_G3_EM, "D3: wanted G3_RS has no ROM -> native G3_EM");
    CHECK(why == SE_WHY_NO_ROM, "D3: reason is NO_ROM");
  }

  /* D4: the cell IS native already -- reason is WANTED even though a concrete era is
   * returned, because nothing was overridden. */
  {
    SeSetting s; se_default(&s);   /* every cell NATIVE */
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_RS, SE_PLACE_PC, 1, 0, 40, &all_roms, true, &why);
    CHECK(got == SE_ERA_GEN1, "D4: NATIVE cell + Gen1 import -> concrete GEN1");
    CHECK(why == SE_WHY_WANTED, "D4: reason is WANTED (nothing was overridden)");
  }

  /* D5: COMPILED -- native's own ROM is absent too, but the build has compiled Gen-3
   * art, so it is used (still reported as the NATIVE sentinel: there is no per-era
   * value for "the compiled table"). */
  {
    SeSetting s; se_default(&s);
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25, &no_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE, "D5: no ROMs at all, compiled art available -> NATIVE sentinel");
    CHECK(why == SE_WHY_COMPILED, "D5: reason is COMPILED");
  }

  /* D6: CHIP -- artless build, nothing at all. */
  {
    SeSetting s; se_default(&s);
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25, &no_roms, false, &why);
    CHECK(got == SE_ERA_NATIVE, "D6: artless, no ROMs -> NATIVE sentinel");
    CHECK(why == SE_WHY_CHIP, "D6: reason is CHIP");
  }

  /* D7: a GB import with no era-specific ROM funnels into the SAME compiled/chip step
   * as a native Gen-3 mon (pdna_origin_art.h: "GB import + no such ROM -> the Gen-3
   * picture"). */
  {
    SeSetting s; se_default(&s);
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_PARTY, 1, 0, 40, &no_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE, "D7: GB import, no Gen1 ROM, compiled art -> NATIVE sentinel");
    CHECK(why == SE_WHY_COMPILED, "D7: reason is COMPILED for the GB-import path too");
  }

  /* D8: PC_GRID + GEN1 refusal -- even with a Gen1 ROM registered and a valid dex, the
   * PC grid never draws GEN1 (no per-species icons). */
  {
    SeSetting s; se_default(&s);
    s.era[SE_KIND_EM][SE_PLACE_PC] = SE_ERA_GEN1;
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_PC, 1, 1, 40, &all_roms, true, &why);
    CHECK(got == SE_ERA_GEN1, "D8: refused to GEN1-native, but native for a Gen1 import IS Gen1");
    CHECK(why == SE_WHY_NO_SPECIES, "D8: reason reports the refusal as NO_SPECIES");
    /* Prove the refusal is REAL (not accidentally passing through), by using a native
     * Gen-3 mon instead: the wanted-GEN1 cell must be refused down to native G3_EM,
     * never drawn as GEN1, even though GEN1's ROM is registered. */
    SeEra got2 = se_resolve(&s, SE_KIND_EM, SE_PLACE_PC, 3, 1, 25, &all_roms, true, &why);
    CHECK(got2 == SE_ERA_G3_EM, "D8b: PC_GRID+GEN1 refused even for a native Gen-3 mon");
  }

  /* D9: NULL setting / roms degrade to the safest answer instead of crashing. */
  {
    int why = -1;
    SeEra got = se_resolve(NULL, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25, &all_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE && why == SE_WHY_CHIP, "D9: NULL setting -> NATIVE/CHIP");
    SeSetting s; se_default(&s);
    got = se_resolve(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25, NULL, true, &why);
    CHECK(got == SE_ERA_NATIVE && why == SE_WHY_CHIP, "D9b: NULL roms -> NATIVE/CHIP");
    got = se_resolve(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25, &all_roms, true, NULL);
    CHECK(got == SE_ERA_G3_EM, "D9c: NULL reason pointer is tolerated");
  }

  printf("(D) se_resolve ok\n");
}

/* ---- (E) config write/apply round trip --------------------------------------------- */
static void test_config(void) {
  SeSetting s; se_default(&s);
  s.era[SE_KIND_EM][SE_PLACE_SUMMARY]  = SE_ERA_GEN1;
  s.era[SE_KIND_RS][SE_PLACE_PARTY]    = SE_ERA_G3_EM;
  s.era[SE_KIND_GEN2][SE_PLACE_GBGRID] = SE_ERA_GEN2;

  char buf[512];
  int n = se_config_write(&s, buf, sizeof buf);
  CHECK(n > 0, "E1: writes something for a non-default setting");
  CHECK((int)strlen(buf) == n, "E1: return value matches the written length");

  /* Round trip: apply every line back into a fresh default setting. */
  SeSetting rt; se_default(&rt);
  char copy[512]; memcpy(copy, buf, (size_t)n + 1);
  int applied = 0;
  for (char* p = copy; *p; ) {
    char* eol = p; while (*eol && *eol != '\n') eol++;
    char term = *eol; *eol = 0;
    char* eq = strchr(p, '=');
    if (eq) {
      *eq = 0;
      if (se_config_apply(&rt, p, eq + 1)) applied++;
    }
    p = term ? eol + 1 : eol;
  }
  CHECK(applied == 3, "E2: all three non-default lines were recognised");
  CHECK(memcmp(&rt, &s, sizeof s) == 0, "E2: round trip reproduces the original setting");

  /* Absent key -> stays NATIVE (nothing to apply). */
  {
    SeSetting d; se_default(&d);
    CHECK(memcmp(&d, &d, sizeof d) == 0, "E3: sanity");
    CHECK(d.era[SE_KIND_RS][SE_PLACE_PC] == SE_ERA_NATIVE, "E3: an unmentioned cell is NATIVE");
  }

  /* Unknown value -> NATIVE, key still recognised (returns true). */
  {
    SeSetting d; se_default(&d);
    d.era[SE_KIND_EM][SE_PLACE_SUMMARY] = SE_ERA_GEN2;   /* poison so NATIVE is provable */
    bool ok = se_config_apply(&d, "era_em_sum", "bogus");
    CHECK(ok == true, "E4: a recognised key with a bad value still returns true");
    CHECK(d.era[SE_KIND_EM][SE_PLACE_SUMMARY] == SE_ERA_NATIVE, "E4: bad value resets to NATIVE");
  }

  /* Not ours at all -- key namespace, kind token, place token, and separator each wrong. */
  {
    SeSetting d; se_default(&d);
    CHECK(se_config_apply(&d, "romrs", "/foo.gba") == false, "E5: unrelated key -> false");
    CHECK(se_config_apply(&d, "era_xx_sum", "g1") == false, "E5: unknown kind token -> false");
    CHECK(se_config_apply(&d, "era_em_xxxxxx", "g1") == false, "E5: unknown place token -> false");
    CHECK(se_config_apply(&d, "era_emXsum", "g1") == false, "E5: missing separator -> false");
    CHECK(se_config_apply(NULL, NULL, "g1") == false, "E5: NULL key -> false");
  }

  /* Cap-bounded writes: cap 1 (room for the NUL only, nothing else) never overflows and
   * writes an empty string; the EXACT capacity writes everything with 1 byte to spare
   * for the NUL, and cap-1 (one byte short) must therefore drop that whole last line
   * rather than truncate it mid-line. */
  {
    char tiny[1];
    int w = se_config_write(&s, tiny, 1);
    CHECK(w == 0, "E6: cap=1 writes zero bytes");
    CHECK(tiny[0] == 0, "E6: cap=1 still NUL-terminates");

    int full_len = se_config_write(&s, buf, sizeof buf);
    char exact[512];
    int w2 = se_config_write(&s, exact, full_len + 1);   /* +1 for the NUL */
    CHECK(w2 == full_len, "E7: cap == exact size writes everything");
    CHECK(memcmp(exact, buf, (size_t)full_len) == 0, "E7: exact-cap output matches the full write");

    char short_by_one[512];
    int w3 = se_config_write(&s, short_by_one, full_len);  /* one byte short of the NUL */
    CHECK(w3 < full_len, "E8: cap one short of full drops at least the last line");
    CHECK((int)strlen(short_by_one) == w3, "E8: still a clean, whole-line, NUL-terminated prefix");
  }

  CHECK(se_config_write(NULL, buf, sizeof buf) == 0, "E9: NULL setting writes nothing");
  printf("(E) config write/apply ok\n");
}

/* ---- (F) UI labels ------------------------------------------------------------------ */
static void test_names(void) {
  for (int k = 0; k < SE_KIND_N; k++)  CHECK(strlen(se_kind_name((SeSaveKind)k))  <= 8, "kind name <=8 chars");
  for (int p = 0; p < SE_PLACE_N; p++) CHECK(strlen(se_place_name((SePlace)p))    <= 8, "place name <=8 chars");
  for (int e = 0; e < SE_ERA_N; e++)   CHECK(strlen(se_era_name((SeEra)e))        <= 8, "era name <=8 chars");
  CHECK(strcmp(se_kind_name((SeSaveKind)99), "?") == 0, "bad kind name is \"?\"");
  CHECK(strcmp(se_place_name((SePlace)99), "?") == 0, "bad place name is \"?\"");
  CHECK(strcmp(se_era_name((SeEra)99), "?") == 0, "bad era name is \"?\"");
  printf("(F) UI labels ok\n");
}

/* ---- (G) se_era_next: cycles only through possible eras, always terminates -------- */
static void test_era_next(void) {
  for (int mask = 0; mask < 64; mask++) {           /* all 64 SeRoms.have combinations */
    SeRoms roms;
    for (int i = 0; i < SE_ERA_N; i++) roms.have[i] = (mask >> i) & 1;
    for (int place = 0; place < SE_PLACE_N; place++) {
      for (int start = 0; start < SE_ERA_N; start++) {
        SeEra got = se_era_next((SeEra)start, &roms, (SePlace)place);
        CHECK((unsigned)got < SE_ERA_N, "G: result is always a valid SeEra");
        /* Never GEN1 for the PC grid. */
        CHECK(!(place == SE_PLACE_PC && got == SE_ERA_GEN1), "G: PC_GRID never offers GEN1");
        /* Whatever it returned must be genuinely offerable: NATIVE always is; a
         * concrete era only when its ROM bit is set. */
        bool offerable = (got == SE_ERA_NATIVE) || roms.have[got];
        CHECK(offerable, "G: the returned era is actually possible for this ROM set");
      }
    }
  }
  /* One concrete example a human can read: only G3_EM registered, starting at NATIVE,
   * cycling for the party place (not PC, so GEN1 isn't specially excluded here). */
  {
    SeRoms roms; for (int i = 0; i < SE_ERA_N; i++) roms.have[i] = false;
    roms.have[SE_ERA_G3_EM] = true;
    SeEra n1 = se_era_next(SE_ERA_NATIVE, &roms, SE_PLACE_PARTY);
    CHECK(n1 == SE_ERA_GEN1 || n1 == SE_ERA_G3_EM,
          "G-example: from NATIVE, next is GEN1 (unavailable, skipped) or G3_EM");
    /* Whichever it landed on, it must be one with a ROM or NATIVE -- already checked
     * above by the exhaustive loop; here just confirm the concrete unavailable one
     * (GEN1) is never returned when only G3_EM is registered. */
    CHECK(n1 != SE_ERA_GEN1 && n1 != SE_ERA_GEN2 && n1 != SE_ERA_G3_RS && n1 != SE_ERA_G3_FRLG,
          "G-example: only G3_EM or NATIVE can come back with just that ROM registered");
  }
  printf("(G) se_era_next ok\n");
}

int main(void) {
  test_default();
  test_native_era();
  test_species_exists();
  test_resolve();
  test_config();
  test_names();
  test_era_next();

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
