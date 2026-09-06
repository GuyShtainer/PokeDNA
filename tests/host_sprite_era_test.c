/* Host test for source/sprite_era.{c,h} -- slice E2 of the sprite-era arc
 * (docs/SPRITE-ERA-DESIGN.md). Pure C, no GBA/tonc headers, links only this one module
 * (which privately includes gen3_trainer.h for se_kind_from_game's PkGame mapping).
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_sprite_era_test.c \
 *      source/sprite_era.c -o /tmp/hse && /tmp/hse
 *
 * Covers: defaults; the native-era mapping for every SaveKind x origin (including the
 * "uncertain Kanto import defaults to Gen 1" case); species existence at every dex edge
 * (151/152, 251/252, 386/387); the FULL resolver fallback chain with every SeWhy reason
 * hit at least once, INCLUDING the fix a review caught before this shipped -- the
 * PC_GRID+GEN1 icon refusal and the species-existence gate are applied to the RESOLVED
 * NATIVE era, not just to the user's WANTED cell, so a Gen-1 import sitting in the
 * DEFAULT (untouched) PC-grid cell of an Emerald box is refused down to compiled/chip
 * art exactly like an explicit GEN1 request would be; a config write/apply round trip
 * including absent/unknown keys, dead (non-applicable) cells, and cap-bounded writes
 * (cap 1, cap exact, cap one short) with the `truncated` out-param; se_era_next's
 * per-call invariants AND a full-cycle closure/coverage assertion designed to be
 * MUTATION-SENSITIVE (a body that ignores its input and always returns NATIVE fails the
 * cycle-coverage check the moment more than one era is offerable); and the five
 * RECOMMENDED helpers (se_era_available, se_cell_applies, se_store_era,
 * se_kind_from_game).
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
   * returned, because nothing was overridden. Deliberately at SE_PLACE_PARTY, NOT
   * SE_PLACE_PC: at PC, a Gen-1-import native era is exactly the case step 6 (the
   * native-side icon refusal) must veto -- see D8/D8b below. Using PARTY here isolates
   * "native cell resolves to the mon's real era" from that separate refusal. */
  {
    SeSetting s; se_default(&s);   /* every cell NATIVE */
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_RS, SE_PLACE_PARTY, 1, 0, 40, &all_roms, true, &why);
    CHECK(got == SE_ERA_GEN1, "D4: NATIVE cell + Gen1 import (party) -> concrete GEN1");
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

  /* D8: PC_GRID + GEN1 refusal on an EXPLICIT wanted cell -- with a Gen1 ROM registered
   * and a valid dex, the PC grid still never draws GEN1 (no per-species icons). This
   * mon's OWN native era is ALSO Gen1 (it really is a Gen-1 import), so the refusal
   * must carry all the way through to compiled/chip -- NOT quietly re-admit GEN1 by a
   * back door, which is exactly the bug a review caught: an earlier version of this
   * resolver checked the icon refusal only against `wanted`, then handed `native`
   * (which happened to compute to GEN1 too) straight through unchecked. */
  {
    SeSetting s; se_default(&s);
    s.era[SE_KIND_EM][SE_PLACE_PC] = SE_ERA_GEN1;
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_PC, 1, 1, 40, &all_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE, "D8: PC+GEN1 refused, native is ALSO Gen1 -> compiled, not GEN1");
    CHECK(why == SE_WHY_COMPILED, "D8: reason is COMPILED (fell all the way through)");
  }

  /* D8b: the SAME wanted-GEN1-on-PC cell, but this record is a NATIVE Gen-3 mon
   * (origin_gen=3). Now native resolves to G3_EM, which is NOT Gen1, so it clears the
   * icon-refusal gate on its own merits and is drawn normally -- proving the refusal is
   * about the PLACE+ERA combination, not a blanket veto on the whole cell. */
  {
    SeSetting s; se_default(&s);
    s.era[SE_KIND_EM][SE_PLACE_PC] = SE_ERA_GEN1;
    int why = -1;
    SeEra got2 = se_resolve(&s, SE_KIND_EM, SE_PLACE_PC, 3, 1, 25, &all_roms, true, &why);
    CHECK(got2 == SE_ERA_G3_EM, "D8b: PC_GRID+GEN1 refused, but native G3_EM is unaffected");
    CHECK(why == SE_WHY_NO_ICONS, "D8b: reason still names the wanted-cell refusal");
  }

  /* D8c: the DEFAULT path, no user override at all -- a Gen-1 import sitting in an
   * Emerald PC box cell that was NEVER touched (still NATIVE). This is the scenario the
   * review flagged BY NAME: the cell being NATIVE must not let a Gen-1-native mon slip
   * past the PC-grid icon refusal just because nothing was "wanted". */
  {
    SeSetting s; se_default(&s);   /* PC cell left at NATIVE -- no override */
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_PC, 1, 0, 40, &all_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE, "D8c: default PC cell + Gen1-native mon -> refused to compiled");
    CHECK(why == SE_WHY_COMPILED, "D8c: reason is COMPILED (native itself failed the icon gate)");
  }

  /* D8d: a caller hint forcing origin_gen=1 onto a JOHTO species (dex 152..251, e.g. a
   * mis-hinted Tyranitar at 248) -- the other half of the bug report. Gen 1 never had
   * this species, so even off the PC grid (SUMMARY here, where the icon refusal does
   * not apply) native must be refused on species grounds and fall through, never
   * silently draw "Gen-1 Tyranitar". */
  {
    SeSetting s; se_default(&s);
    int why = -1;
    SeEra got = se_resolve(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 1, 1, 248, &all_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE, "D8d: origin_gen=1 hint on a Johto dex -> refused, not GEN1");
    CHECK(why == SE_WHY_COMPILED, "D8d: reason is COMPILED (native failed se_species_exists)");
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

/* ---- (Da) se_resolve_for_router: D1's "a native answer must be reported as NATIVE"
 * fix. A default (untouched) grid cell resolves (via se_resolve's own step 6) to the
 * CONCRETE native era the instant that era's ROM is registered -- D4 above proved that
 * shape. Fed straight to the art router, that concrete answer skips gen3_ladder's
 * compiled/same-game path and opens a SECOND ROM (the cross-game rung) for a picture
 * the old pipeline already drew for free. se_resolve_for_router() is the fix: it
 * collapses a concrete answer that matches se_native_era()'s own answer back to the
 * NATIVE sentinel, and leaves everything else (including explicit overrides) alone. */
static void test_resolve_for_router(void) {
  SeRoms all_roms; for (int i = 0; i < SE_ERA_N; i++) all_roms.have[i] = true;
  SeRoms no_roms;  for (int i = 0; i < SE_ERA_N; i++) no_roms.have[i]  = false;

  /* Da1: default grid, Emerald save, SUMMARY place, a native Emerald mon, Emerald ROM
   * registered -- se_resolve() itself answers concrete G3_EM (same shape as D4); the
   * router wrapper must report NATIVE instead. */
  {
    SeSetting s; se_default(&s);
    int why = -1;
    SeEra got = se_resolve_for_router(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25,
                                       &all_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE,
          "Da1: default grid + Emerald mon on an Emerald save -> NATIVE, not G3_EM");
    CHECK(why == SE_WHY_WANTED, "Da1: reason is left exactly as se_resolve() set it");
  }

  /* Da2: an EXPLICIT override that differs from the native answer stays concrete -- an
   * Emerald save's SUMMARY cell set to G3_FRLG, with FRLG registered. */
  {
    SeSetting s; se_default(&s);
    s.era[SE_KIND_EM][SE_PLACE_SUMMARY] = SE_ERA_G3_FRLG;
    int why = -1;
    SeEra got = se_resolve_for_router(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25,
                                       &all_roms, true, &why);
    CHECK(got == SE_ERA_G3_FRLG,
          "Da2: an explicit G3_FRLG override on an Emerald save stays concrete");
  }

  /* Da3: se_resolve() already answers the NATIVE sentinel (no ROMs at all, compiled
   * art available) -- the wrapper must pass that through unchanged. */
  {
    SeSetting s; se_default(&s);
    int why = -1;
    SeEra got = se_resolve_for_router(&s, SE_KIND_EM, SE_PLACE_SUMMARY, 3, 1, 25,
                                       &no_roms, true, &why);
    CHECK(got == SE_ERA_NATIVE, "Da3: se_resolve's own NATIVE sentinel passes through");
  }

  printf("(Da) se_resolve_for_router ok\n");
}

/* ---- (E) config write/apply round trip --------------------------------------------- */
static void test_config(void) {
  SeSetting s; se_default(&s);
  s.era[SE_KIND_EM][SE_PLACE_SUMMARY]  = SE_ERA_GEN1;
  s.era[SE_KIND_RS][SE_PLACE_PARTY]    = SE_ERA_G3_EM;
  s.era[SE_KIND_GEN2][SE_PLACE_GBGRID] = SE_ERA_GEN2;   /* a LIVE cell: GBGRID is what a
                                                          * GB save's own box grid IS */

  char buf[512]; bool trunc = true;   /* poison -- must come back false */
  int n = se_config_write(&s, buf, sizeof buf, &trunc);
  CHECK(n > 0, "E1: writes something for a non-default setting");
  CHECK((int)strlen(buf) == n, "E1: return value matches the written length");
  CHECK(trunc == false, "E1: plenty of room -> not truncated");

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

  /* Dead cells (se_cell_applies false) are never written even if poked directly --
   * these two are exactly the rule's two families: a Gen-3 kind's GBGRID and a Game
   * Boy kind's PC. */
  {
    SeSetting d; se_default(&d);
    d.era[SE_KIND_RS][SE_PLACE_GBGRID] = SE_ERA_GEN1;    /* dead: G3 kind x GBGRID */
    d.era[SE_KIND_GEN1][SE_PLACE_PC]   = SE_ERA_GEN2;    /* dead: GB kind x PC     */
    char out[128]; bool dead_trunc = true;
    int dn = se_config_write(&d, out, sizeof out, &dead_trunc);
    CHECK(dn == 0, "E5b: dead cells are never written, even set directly");
    CHECK(dead_trunc == false, "E5b: nothing to truncate when nothing applies");
  }

  /* Cap-bounded writes: cap 1 (room for the NUL only, nothing else) never overflows and
   * writes an empty string; the EXACT capacity writes everything with 1 byte to spare
   * for the NUL, and cap-1 (one byte short) must therefore drop that whole last line
   * rather than truncate it mid-line. `truncated` reports each case correctly. */
  {
    char tiny[1]; bool t1 = false;
    int w = se_config_write(&s, tiny, 1, &t1);
    CHECK(w == 0, "E6: cap=1 writes zero bytes");
    CHECK(tiny[0] == 0, "E6: cap=1 still NUL-terminates");
    CHECK(t1 == true, "E6: cap=1 reports truncation (there WAS something to write)");

    int full_len = se_config_write(&s, buf, sizeof buf, NULL);   /* NULL is tolerated */
    char exact[512]; bool t2 = true;
    int w2 = se_config_write(&s, exact, full_len + 1, &t2);      /* +1 for the NUL */
    CHECK(w2 == full_len, "E7: cap == exact size writes everything");
    CHECK(memcmp(exact, buf, (size_t)full_len) == 0, "E7: exact-cap output matches the full write");
    CHECK(t2 == false, "E7: exact-fit is NOT truncated");

    char short_by_one[512]; bool t3 = false;
    int w3 = se_config_write(&s, short_by_one, full_len, &t3);   /* one byte short of the NUL */
    CHECK(w3 < full_len, "E8: cap one short of full drops at least the last line");
    CHECK((int)strlen(short_by_one) == w3, "E8: still a clean, whole-line, NUL-terminated prefix");
    CHECK(t3 == true, "E8: one byte short IS truncation");
  }

  CHECK(se_config_write(NULL, buf, sizeof buf, NULL) == 0, "E9: NULL setting writes nothing");
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

/* ---- (G) se_era_next: per-call invariants + a MUTATION-SENSITIVE full-cycle check -- */
static void test_era_next(void) {
  for (int mask = 0; mask < 64; mask++) {           /* all 64 SeRoms.have combinations */
    SeRoms roms;
    for (int i = 0; i < SE_ERA_N; i++) roms.have[i] = (mask >> i) & 1;

    for (int place = 0; place < SE_PLACE_N; place++) {
      /* The offerable set for this (roms, place): NATIVE always, any concrete era with
       * a registered ROM, minus GEN1 when place is the PC grid, minus the three
       * concrete Gen-3 eras when place is BANK or GBGRID (D8: the box grid's per-mon
       * overlay has no path to the cross-game rung at those two places, so offering
       * one there would be a setting that never changes a pixel). NATIVE guarantees
       * count >= 1 always. */
      bool offerable[SE_ERA_N];
      int count = 0;
      for (int e = 0; e < SE_ERA_N; e++) {
        bool ok = (e == SE_ERA_NATIVE) || roms.have[e];
        if (place == SE_PLACE_PC && e == SE_ERA_GEN1) ok = false;
        bool g3 = (e == SE_ERA_G3_RS || e == SE_ERA_G3_EM || e == SE_ERA_G3_FRLG);
        if ((place == SE_PLACE_BANK || place == SE_PLACE_GBGRID) && g3) ok = false;
        offerable[e] = ok;
        if (ok) count++;
      }
      CHECK(count >= 1, "G: NATIVE alone guarantees at least one offerable era");

      for (int start = 0; start < SE_ERA_N; start++) {
        /* Per-call invariants, for every possible starting value (not just offerable
         * ones -- se_era_next must behave sanely even fed a currently-unofferable
         * era, e.g. the very cell a ROM registration just revoked). */
        SeEra got = se_era_next((SeEra)start, &roms, (SePlace)place);
        CHECK((unsigned)got < SE_ERA_N, "G: result is always a valid SeEra");
        CHECK(!(place == SE_PLACE_PC && got == SE_ERA_GEN1), "G: PC_GRID never offers GEN1");
        CHECK(!((place == SE_PLACE_BANK || place == SE_PLACE_GBGRID) &&
                (got == SE_ERA_G3_RS || got == SE_ERA_G3_EM || got == SE_ERA_G3_FRLG)),
              "G (D8): BANK/GBGRID never offer a concrete Gen-3 era");
        CHECK(offerable[got], "G: the returned era is actually possible for this ROM set");
        if (count == 1) {
          CHECK(got == SE_ERA_NATIVE, "G: with only one era offerable, it must be NATIVE, "
                                       "and every start maps to that fixed point");
        } else {
          CHECK((int)got != start, "G: advances (never returns its own input) once >1 era is offerable");
        }
      }

      /* MUTATION-SENSITIVE full-cycle check. NATIVE is always offerable, so walking the
       * cycle starting there is well-defined regardless of `roms`/`place`: repeated
       * se_era_next calls must visit EXACTLY the offerable set and close back to NATIVE
       * after precisely `count` steps. A stub body that ignores its argument and always
       * returns NATIVE would still "close after 1 step" -- but the visited-set check
       * below catches that immediately whenever count > 1, because the stub never
       * visits anything else. Bounded to count+1 steps (slack of exactly one, to detect
       * "never closes" as a failure rather than a hang, per the codebase's loop-bound
       * convention). */
      bool visited[SE_ERA_N] = { false };
      SeEra cur = SE_ERA_NATIVE;
      int steps = 0;
      for (; steps < count + 1; steps++) {
        cur = se_era_next(cur, &roms, (SePlace)place);
        if ((unsigned)cur < SE_ERA_N) visited[cur] = true;
        if (cur == SE_ERA_NATIVE) { steps++; break; }
      }
      CHECK(steps == count, "G: the cycle from NATIVE closes back to NATIVE after exactly |offerable| steps");
      bool visited_matches = true;
      for (int e = 0; e < SE_ERA_N; e++)
        if (visited[e] != offerable[e]) visited_matches = false;
      CHECK(visited_matches, "G: the cycle visits EXACTLY the offerable set, nothing more, nothing less");
    }
  }

  /* One concrete example a human can read: only G3_EM registered, starting at NATIVE,
   * cycling for the party place (not PC, so GEN1 isn't specially excluded here) --
   * GEN1/GEN2/G3_RS/G3_FRLG all have no ROM, so the ONLY possible next value is G3_EM. */
  {
    SeRoms roms; for (int i = 0; i < SE_ERA_N; i++) roms.have[i] = false;
    roms.have[SE_ERA_G3_EM] = true;
    SeEra n1 = se_era_next(SE_ERA_NATIVE, &roms, SE_PLACE_PARTY);
    CHECK(n1 == SE_ERA_G3_EM, "G-example: from NATIVE, next SKIPS every unregistered era and lands on G3_EM");
  }
  printf("(G) se_era_next ok\n");
}

/* ---- (H) se_era_available: the exported ROM-registration check -------------------- */
static void test_era_available(void) {
  SeRoms roms; for (int i = 0; i < SE_ERA_N; i++) roms.have[i] = false;
  roms.have[SE_ERA_GEN2] = true;

  CHECK(se_era_available(SE_ERA_NATIVE, &roms) == true,  "NATIVE is always available");
  CHECK(se_era_available(SE_ERA_GEN1, &roms)   == false, "GEN1 unregistered -> false");
  CHECK(se_era_available(SE_ERA_GEN2, &roms)   == true,  "GEN2 registered -> true");
  CHECK(se_era_available((SeEra)99, &roms)     == false, "an out-of-range era is never available");
  CHECK(se_era_available(SE_ERA_GEN1, NULL)    == false, "NULL roms -> false for a concrete era");
  CHECK(se_era_available(SE_ERA_NATIVE, NULL)  == true,  "NULL roms -> NATIVE is still true");
  printf("(H) se_era_available ok\n");
}

/* ---- (I) se_cell_applies: exactly the 5 dead cells, out of 25, are refused --------- */
static void test_cell_applies(void) {
  int dead = 0, live = 0;
  for (int k = 0; k < SE_KIND_N; k++) {
    for (int p = 0; p < SE_PLACE_N; p++) {
      bool g3_kind = (k == SE_KIND_RS || k == SE_KIND_EM || k == SE_KIND_FRLG);
      bool gb_kind = (k == SE_KIND_GEN1 || k == SE_KIND_GEN2);
      bool expect_dead = (g3_kind && p == SE_PLACE_GBGRID) || (gb_kind && p == SE_PLACE_PC);
      bool got = se_cell_applies((SeSaveKind)k, (SePlace)p);
      CHECK(got == !expect_dead, "se_cell_applies matches the dead-cell rule for every (kind, place)");
      if (expect_dead) dead++; else live++;
    }
  }
  CHECK(dead == 5,  "exactly 5 dead cells: 3 (G3 kind x GBGRID) + 2 (GB kind x PC)");
  CHECK(live == 20, "exactly 20 applicable cells out of the 25-cell grid");
  CHECK(se_cell_applies((SeSaveKind)99, SE_PLACE_PC) == false, "an out-of-range kind never applies");
  CHECK(se_cell_applies(SE_KIND_RS, (SePlace)99)     == false, "an out-of-range place never applies");
  printf("(I) se_cell_applies ok\n");
}

/* ---- (J) se_store_era: the icon-store ROM choice ----------------------------------- */
static void test_store_era(void) {
  SeSetting s; se_default(&s);
  SeRoms roms; for (int i = 0; i < SE_ERA_N; i++) roms.have[i] = false;

  CHECK(se_store_era(&s, SE_KIND_EM, SE_PLACE_PC, &roms) == SE_ERA_G3_EM,
        "J1: default (NATIVE) cell -> native(kind,3,1) = G3_EM");

  s.era[SE_KIND_EM][SE_PLACE_PC] = SE_ERA_G3_RS;
  roms.have[SE_ERA_G3_RS] = true;
  CHECK(se_store_era(&s, SE_KIND_EM, SE_PLACE_PC, &roms) == SE_ERA_G3_RS,
        "J2: a concrete Gen-3 cell WITH its ROM -> that cell, even though it differs from kind");

  roms.have[SE_ERA_G3_RS] = false;
  CHECK(se_store_era(&s, SE_KIND_EM, SE_PLACE_PC, &roms) == SE_ERA_G3_EM,
        "J3: same cell, ROM now absent -> falls back to native(kind,3,1)");

  s.era[SE_KIND_EM][SE_PLACE_PC] = SE_ERA_GEN1;
  CHECK(se_store_era(&s, SE_KIND_EM, SE_PLACE_PC, &roms) == SE_ERA_G3_EM,
        "J4: a GEN1 cell names no Gen-3 ROM -> falls back to native(kind,3,1)");

  CHECK(se_store_era(NULL, SE_KIND_RS, SE_PLACE_PC, &roms) == SE_ERA_G3_RS,
        "J5: NULL setting -> native(kind,3,1)");
  CHECK(se_store_era(&s, SE_KIND_FRLG, SE_PLACE_PC, NULL) == SE_ERA_G3_FRLG,
        "J6: NULL roms -> native(kind,3,1)");

  CHECK(se_store_era(&s, SE_KIND_GEN1, SE_PLACE_GBGRID, &roms) == SE_ERA_GEN1,
        "J7: a Game Boy save kind's own generation, via the same native() fallback");
  printf("(J) se_store_era ok\n");
}

/* ---- (K) se_kind_from_game: PkGame -> SeSaveKind ----------------------------------- */
static void test_kind_from_game(void) {
  /* Literal 0/1/2, matching gen3_trainer.h's PK_RS=0/PK_EMERALD=1/PK_FRLG=2 -- the .c
   * file's _Static_assert is what actually guards this numbering; this test only
   * proves the mapping table built on top of it. */
  CHECK(se_kind_from_game(0) == SE_KIND_RS,   "PK_RS (0) -> SE_KIND_RS");
  CHECK(se_kind_from_game(1) == SE_KIND_EM,   "PK_EMERALD (1) -> SE_KIND_EM");
  CHECK(se_kind_from_game(2) == SE_KIND_FRLG, "PK_FRLG (2) -> SE_KIND_FRLG");
  CHECK(se_kind_from_game(99) == SE_KIND_EM,  "an out-of-range game degrades to SE_KIND_EM");
  CHECK(se_kind_from_game(-1) == SE_KIND_EM,  "a negative game degrades to SE_KIND_EM");
  printf("(K) se_kind_from_game ok\n");
}

int main(void) {
  test_default();
  test_native_era();
  test_species_exists();
  test_resolve();
  test_resolve_for_router();
  test_config();
  test_names();
  test_era_next();
  test_era_available();
  test_cell_applies();
  test_store_era();
  test_kind_from_game();

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}
