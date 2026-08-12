/* Host test for the egg flag + hatch edit core (pure C, no hardware):
 *   cc -std=c11 -I source tests/host_hatch_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/gen3_pidiv.c source/data_tables.c \
 *      -o /tmp/hh && /tmp/hh
 *
 * Verifies em_set_egg toggles BOTH egg locations (flags byte bit2 + Misc IV-word bit30) and
 * that em_hatch clears the egg, writes the two fields retail's hatch path writes
 * (friendship 120, met level 0), and keeps the species/IVs — i.e. the revealed Pokemon is
 * intact and is RECOGNISED AS HATCHED by PokeDNA's own readers. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gen3_edit.h"
#include "gen3_mon.h"
#include "gen3_pidiv.h"
#include "data_tables.h"

static int fails = 0;
static void chk(const char* what, int cond) { if (!cond) { printf("FAIL %s\n", what); fails++; } }

int main(void) {
  uint8_t rec[80];
  gen3_build_mon(1, 5, 0x12345678u, 0xAABBCCDDu, "TEST", 3, rec);   /* Bulbasaur (species 1), Lv5 */

  /* make it an egg with a hatch-cycle counter (stored in the friendship byte) */
  EditMon e; gen3_edit_load(rec, false, &e);
  em_set_egg(&e, true);
  em_set_iv(&e, PK_ATK, 31);                 /* a distinctive IV to prove it survives the hatch */
  em_set_friendship(&e, 20);
  uint8_t egg[80]; gen3_edit_commit(&e, egg);

  /* Make it look like a REAL egg before testing the hatch rename. gen3_build_mon already
   * seeds the nickname with the species name, so asserting "hatched name == species"
   * without this would pass vacuously and prove nothing.
   *
   * A genuine Gen-3 egg carries the Japanese nickname タマゴ (60 6F 8B FF) and
   * language 1 — verified on every egg in roms/{Ruby,Emerald}.sav. gen3_decode_char has
   * no case for those bytes, which is exactly why the viewer showed "???". The nickname
   * lives at record offset 0x08, outside the encrypted/checksummed substruct block, so
   * patching the committed record directly is valid. */
  static const uint8_t k_jp_egg[4] = { 0x60, 0x6F, 0x8B, 0xFF };
  memcpy(egg + 0x08, k_jp_egg, 4);
  egg[0x12] = 1;                                    /* LANGUAGE_JAPANESE */

  PkMon m; pk_decode_mon(egg, false, &m);
  chk("egg nickname decodes as ???", strcmp(m.nickname, "???") == 0);
  chk("is egg after set_egg", m.isEgg);
  chk("egg keeps species", m.species == 1);
  chk("egg keeps IV", m.ivs[PK_ATK] == 31);

  /* clearing the egg flag again should un-egg it (round-trip both bits) */
  gen3_edit_load(egg, false, &e); em_set_egg(&e, false);
  uint8_t un[80]; gen3_edit_commit(&e, un);
  pk_decode_mon(un, false, &m);
  chk("not egg after clear", !m.isEgg);

  /* HATCH the egg */
  gen3_edit_load(egg, false, &e);
  em_hatch(&e);
  uint8_t hatched[80]; gen3_edit_commit(&e, hatched);
  pk_decode_mon(hatched, false, &m);
  chk("hatched: not an egg", !m.isEgg && !m.isBadEgg);
  chk("hatched: species intact", m.species == 1);
  chk("hatched: IV intact", m.ivs[PK_ATK] == 31);
  chk("hatched: exp = level 5", m.experience == pk_exp_for_level(pk_species_growth(1), 5));

  /* The two fields retail's hatch path writes and this one used to get wrong.
   *
   * friendship: CreateHatchedMon hard-codes `friendship = 120` (src/egg_hatch.c:350-351).
   * 70 is the CAUGHT base — the value gen3_build_mon still uses — and it appears nowhere
   * in the hatch path. Grounded in Guy's five saves: over the 141 mons with metLevel 0,
   * 120 is the largest single bucket (43 of them) and 126 sit at 120 or above.
   *
   * met level: AddHatchedMonToParty writes 0, with the decomp's own comment "A met level
   * of 0 is interpreted on the summary screen as 'hatched at'" (src/egg_hatch.c:384-386),
   * and all 17 eggs in the corpus already carry it. */
  chk("hatched: friendship 120 (retail's hatch value, not the caught base 70)",
      m.friendship == 120);
  chk("hatched: met level 0 (\"hatched at\")", m.metLevel == 0);

  /* ...and the reason met level 0 is not cosmetic: it is the ONLY thing that records
   * "this was an egg", so PokeDNA's own hatched exemptions key off it. A hatch that
   * leaves the byte alone produces a mon PokeDNA does not recognise as hatched. */
  chk("hatched: PokeDNA's own reader calls it hatched",
      pk_pidiv_exempt_reason(&m) == PK_PIDIV_EX_HATCHED);

  /* The case the old code actually broke: an egg PokeDNA MADE itself. gen3_build_mon
   * stamps met level = the build level, so nothing else in the record ever set it to 0
   * and em_hatch was the only place it could happen.
   *
   * THE FIXTURE MOVED (and the guard below is what caught it, as its own note said it
   * would). gen3_build_mon now writes met level 0 for any species with an egg route —
   * the whole point being that a created Pokemon should already read as bred — so
   * BULBASAUR no longer starts non-zero. GROUDON does: no pair can produce it, so the
   * builder gives it a caught origin, which is exactly the record that still reaches
   * em_hatch with a non-zero byte. Same test, a species where it can still fail. */
  {
    uint8_t brec[80], begg[80], bhat[80];
    gen3_build_mon(405 /* GROUDON */, 5, 0x0BADF00Du, 0xAABBCCDDu, "TEST", 3, brec);
    EditMon be; gen3_edit_load(brec, false, &be);
    em_set_egg(&be, true); gen3_edit_commit(&be, begg);
    PkMon bm; pk_decode_mon(begg, false, &bm);
    /* Non-vacuity guard: gen3_build_mon writes metLevel = the build level and em_set_egg
     * does not touch it, so the next two checks are only meaningful while this holds. If
     * this ever fails, em_hatch is no longer the thing that zeroes the byte — move the
     * fixture, do not delete the assertion. */
    chk("built egg: starts with a NON-zero met level (else the next check is vacuous)",
        bm.metLevel == 5);
    gen3_edit_load(begg, false, &be); em_hatch(&be); gen3_edit_commit(&be, bhat);
    pk_decode_mon(bhat, false, &bm);
    chk("built egg hatched: met level 0", bm.metLevel == 0);
    chk("built egg hatched: friendship 120", bm.friendship == 120);
    chk("built egg hatched: recognised as hatched",
        pk_pidiv_exempt_reason(&bm) == PK_PIDIV_EX_HATCHED);
  }

  /* #26: an unnicknamed Gen-3 mon simply has its species name in the nickname field. */
  chk("hatched: nickname = species name", strcmp(m.nickname, pk_species_name(1)) == 0);
  chk("hatched: nickname terminated", hatched[0x08 + 9] == 0xFF);
  chk("hatched: language = English", hatched[0x12] == 2);

  /* The two non-ASCII species names. The table stores NIDORAN♀/♂ as UTF-8 U+2640/U+2642;
   * retail stores the single charset byte 0xB6/0xB5 (a NIDORAN♂ caught in FireRed reads
   * C8 C3 BE C9 CC BB C8 B5 FF). A byte-wise encoder wrote three SPACES instead. */
  {
    uint8_t nrec[80], negg[80], nhat[80];
    static const uint8_t k_nidoran[7] = { 0xC8, 0xC3, 0xBE, 0xC9, 0xCC, 0xBB, 0xC8 };  /* NIDORAN */
    const struct { uint16_t sp; uint8_t sym; const char* label; } cases[] = {
      { 29, 0xB6, "NIDORAN-F" },     /* internal 29 = NIDORAN♀ */
      { 32, 0xB5, "NIDORAN-M" },     /* internal 32 = NIDORAN♂ */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      gen3_build_mon(cases[i].sp, 5, 0x2468ACE1u, 0xAABBCCDDu, "TEST", 3, nrec);
      gen3_edit_load(nrec, false, &e); em_set_egg(&e, true); gen3_edit_commit(&e, negg);
      memcpy(negg + 0x08, k_jp_egg, 4); negg[0x12] = 1;
      gen3_edit_load(negg, false, &e); em_hatch(&e); gen3_edit_commit(&e, nhat);
      char w[48];
      sprintf(w, "%s: 'NIDORAN' prefix", cases[i].label);
      chk(w, memcmp(nhat + 0x08, k_nidoran, 7) == 0);
      sprintf(w, "%s: gender symbol byte 0x%02X", cases[i].label, cases[i].sym);
      chk(w, nhat[0x08 + 7] == cases[i].sym);
      sprintf(w, "%s: terminated at byte 8", cases[i].label);
      chk(w, nhat[0x08 + 8] == 0xFF);
    }
  }

  /* A 10-character species name fills all 10 nickname bytes with NO terminator — same as
   * retail's POKEMON_NAME_LENGTH copy. Do not "fix" this by reserving a terminator: that
   * would truncate to 9 and diverge from the real game. CHARMANDER = internal 4. */
  {
    uint8_t crec[80], cegg[80], chat[80];
    gen3_build_mon(4, 5, 0x13571357u, 0xAABBCCDDu, "TEST", 3, crec);
    gen3_edit_load(crec, false, &e); em_set_egg(&e, true); gen3_edit_commit(&e, cegg);
    memcpy(cegg + 0x08, k_jp_egg, 4); cegg[0x12] = 1;
    gen3_edit_load(cegg, false, &e); em_hatch(&e); gen3_edit_commit(&e, chat);
    chk("CHARMANDER: 10 bytes, no terminator", chat[0x08 + 9] != 0xFF);
    PkMon cm; pk_decode_mon(chat, false, &cm);
    chk("CHARMANDER: decodes to the full name", strcmp(cm.nickname, "CHARMANDER") == 0);
  }

  /* A corrupt egg (species out of range) must keep whatever it had, not become "??????????". */
  {
    uint8_t brec[80];
    memcpy(brec, egg, 80);
    EditMon be; gen3_edit_load(brec, false, &be);
    be.sub[0][0] = 0xFF; be.sub[0][1] = 0xFF;      /* species 65535 */
    uint8_t bcommit[80]; gen3_edit_commit(&be, bcommit);
    gen3_edit_load(bcommit, false, &be);
    em_hatch(&be);
    uint8_t bhat[80]; gen3_edit_commit(&be, bhat);
    chk("corrupt egg: nickname untouched", memcmp(bhat + 0x08, k_jp_egg, 4) == 0);
    chk("corrupt egg: language untouched", bhat[0x12] == 1);
  }

  printf("hatch test: %d failure(s)\n", fails);
  return fails ? 1 : 0;
}
