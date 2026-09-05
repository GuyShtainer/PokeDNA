#include "gen3_legality2.h"
#include "data_tables.h"
#include <string.h>

/* Legality V2 core — every check that needs NO data table beyond what already ships.
 * See gen3_legality2.h for the model; docs/research-legality-v2.md §3 for the
 * catalogue this implements (categories A, D, F, G).
 *
 * Severity policy used throughout, so the choices are auditable:
 *   INVALID  the retail code CANNOT produce this value. Each one below cites the
 *            pokeemerald routine that proves it (R/S/FR/LG share the code).
 *   SUSPECT  no known legitimate path, but an exemption might exist (Colosseum/XD,
 *            an event distribution, a JP cart, a mechanic we have not modelled).
 *   INFO     true but not incriminating.
 * When in doubt the rule is SUSPECT — a false "ILLEGAL" on someone's real Pokemon
 * is the one failure mode this tool must never have (OVERNIGHT-DECISIONS.md §2).
 *
 * Decomp citations are to the pokeemerald checkout under `daycare map/pokeemerald`
 * (line numbers as of 2026-08-08).
 */

/* Gen-3 constants that appear in the checks. */
#define G3_MAX_SPECIES     411   /* internal index; 412 entries incl. slot 0        */
#define G3_MAX_MOVE        354   /* MOVES_COUNT-1                                   */
#define G3_MAX_ITEM        376   /* ITEMS_COUNT-1                                   */
#define G3_EGG_LEVEL         5   /* EGG_HATCH_LEVEL, include/constants/daycare.h:17 */
#define G3_MAX_EGG_CYCLES  120   /* largest eggCycles in the base-stats table       */
#define G3_BALL_POKE         4   /* ITEM_POKE_BALL — every egg gets it              */
#define G3_LANG_JP           1   /* LANGUAGE_JAPANESE, include/constants/global.h:20*/
#define G3_LANG_KO           6   /* LANGUAGE_KOREAN — defined but unused in Gen 3   */
#define G3_LANG_MAX          7   /* LANGUAGE_SPANISH                                */
#define G3_JP_NAME_MAX       5   /* JP carts cap nicknames/player names at 5 glyphs */
#define SPECIES_MEW        151
#define SPECIES_DEOXYS     410   /* INTERNAL index (national 386)                   */
#define SPECIES_SHEDINJA   303   /* CalculateMonStats forces its Max HP to 1        */

/* E4 — Safari Ball <-> Safari Zone. Both games hand out ONLY the Safari Ball inside
 * their Safari Zone (RSE: `GiveMonToPlayer`/ball is unconditionally ITEM_SAFARI_BALL
 * in the safari catching subroutine; FRLG's Safari Zone is the same subroutine
 * ported), and nowhere else legitimately hands one out — so the ball id and the met
 * MAPSEC travel together. Indices confirmed against source/data_tables.c's
 * s_location table (RSE "SAFARI ZONE" at 0x39, FRLG's at 0x88). */
#define G3_BALL_SAFARI      5
#define MAPSEC_SAFARI_RSE   0x39
#define MAPSEC_SAFARI_FRLG  0x88

/* Record offsets used for the fields PkMon does not (and should not) decode:
 * the raw nickname/OT bytes, which we need UNDECODED because the interesting
 * values are outside gen3_decode_char's Latin range. */
#define REC_NICKNAME  0x08   /* 10 bytes, 0xFF-terminated */
#define REC_OTNAME    0x14   /* 7 bytes,  0xFF-terminated */

void pk2_add(Pk2Report* R, uint8_t cat, uint8_t sev, const char* text) {
  if (!R) return;
  if (cat >= PK2_NCAT) cat = PK2_CAT_STRUCT;
  if (sev > PK2_INVALID) sev = PK2_INVALID;
  if (sev == PK2_SUSPECT) R->n_suspect++;
  else if (sev == PK2_INVALID) R->n_invalid++;
  R->cat_n[cat]++;
  if (sev > R->cat_worst[cat]) R->cat_worst[cat] = sev;
  if (R->n >= PK2_MAX_ROWS) { R->truncated = 1; return; }   /* counters still true */
  Pk2Row* r = &R->row[R->n];
  r->cat = cat;
  r->sev = sev;
  int i = 0;
  for (; text[i] && i < PK2_TEXT_LEN - 1; i++) r->text[i] = text[i];
  r->text[i] = 0;
  R->n++;
}

const char* pk2_grade_name(uint8_t g) {
  return g == PK2_ILLEGAL ? "ILLEGAL" : g == PK2_QUESTIONABLE ? "QUESTIONABLE" : "LEGAL";
}
const char* pk2_cat_name(uint8_t c) {
  switch (c) {
    case PK2_CAT_STRUCT: return "STRUCTURE";
    case PK2_CAT_MOVES:  return "MOVES";
    case PK2_CAT_PID:    return "PID/RNG";
    case PK2_CAT_MET:    return "MET";
    case PK2_CAT_EGG:    return "EGG";
    case PK2_CAT_FLAGS:  return "FLAGS";
    default:             return "?";
  }
}
const char* pk2_sev_name(uint8_t s) {
  return s == PK2_INVALID ? "INVALID" : s == PK2_SUSPECT ? "suspect" : "info";
}

/* ---- weak hook defaults ---------------------------------------------------
 * Absent-by-default: they only record that the family is missing. A module that
 * implements one defines the same symbol strongly and wins at link time
 * (source/art_fallbacks.c uses the identical mechanism for the art modules). */
__attribute__((weak)) void pk2_hook_moves(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  (void)m; (void)f; if (R) R->hooks_absent |= PK2_HOOK_MOVES;
}
__attribute__((weak)) void pk2_hook_encounter(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  (void)m; (void)f; if (R) R->hooks_absent |= PK2_HOOK_ENCOUNTER;
}
__attribute__((weak)) void pk2_hook_pidiv(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  (void)m; (void)f; if (R) R->hooks_absent |= PK2_HOOK_PIDIV;
}
__attribute__((weak)) void pk2_hook_evolution(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  (void)m; (void)f; if (R) R->hooks_absent |= PK2_HOOK_EVO;
}

/* ---- small helpers -------------------------------------------------------- */

/* Visible length of a Gen-3 name field: glyphs before the 0xFF terminator.
 * NOTE the game does NOT clear the tail — SetMonData copies POKEMON_NAME_LENGTH
 * bytes verbatim from the source string, so an egg's nickname is
 * "60 6F 8B FF <whatever followed sJapaneseEggNickname in ROM>". Only the bytes
 * before the terminator carry meaning. */
static int name_len(const uint8_t* p, int cap) {
  int i = 0;
  while (i < cap && p[i] != 0xFF) i++;
  return i;
}

/* A text CONTROL code inside a name. charmap.txt maps every byte to a glyph except
 * F8..FE, which are the text engine's command codes (DYNAMIC/STRING/newline/...).
 * The naming screen cannot emit them, so one in a name is a tell. 0xFF terminates
 * and everything after it is untouched padding, so we stop there. */
static bool name_has_control_byte(const uint8_t* p, int cap) {
  for (int i = 0; i < cap; i++) {
    if (p[i] == 0xFF) return false;
    if (p[i] >= 0xF8) return true;
  }
  return false;
}

/* ---- the catalogue -------------------------------------------------------- */

static void check_structure(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  /* Level / EXP. A party record stores the level in plaintext, so it can disagree
   * with EXP; a box record has no stored level (we derive it), so only the cap
   * below can catch a hacked box EXP — doc §3 A13. */
  if (m->isParty && (m->level < 1 || m->level > 100))
    pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "Level out of 1..100");
  else if (m->isParty && pk_level_from_exp(f->growth, m->experience) != m->level)
    pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "Level does not match EXP");
  if (m->experience > pk_exp_for_level(f->growth, 100))
    pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "EXP above the level-100 cap");

  /* EVs. Only the 510 SUM is a real rule in Gen 3: the per-stat field is a u8 and
   * 255 in one stat is legal here (the 252 cap arrives in Gen 6), so "EV <= 255"
   * is structurally unviolable and no check exists for it — doc §3 G6. */
  int evsum = 0;
  for (int i = 0; i < PK_NSTATS; i++) evsum += m->evs[i];
  if (evsum > 510) pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "EV total over 510");

  /* IVs are 5-bit fields in the record, so a DECODED mon can never exceed 31. This
   * is a guard for hand-built PkMon structs (the editor's preview path and the
   * host tests construct them directly) — it costs six compares and turns a
   * silently-wrong value into a visible one. Contest stats and friendship are u8
   * with the full range legal, so they have no equivalent bound. */
  for (int i = 0; i < PK_NSTATS; i++)
    if (m->ivs[i] > 31) { pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "IV over 31"); break; }

  /* Held item. The id bound is hard; the "exists in some cart" test is softer
   * because the item table has placeholder gaps — doc §3 A14. An item can come
   * from any of the five games a mon has visited, so there is no per-origin gate. */
  if (m->heldItem > G3_MAX_ITEM)
    pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "Held item id out of range");
  else if (m->heldItem && pk_item_games(m->heldItem) == 0)
    pk2_add(R, PK2_CAT_STRUCT, PK2_SUSPECT, "Held item exists in no Gen-3 game");

  /* Ability slot. CreateBoxMon writes the slot bit ONLY when the species has a
   * second ability: "if (gSpeciesInfo[species].abilities[1]) { value = personality
   * & 1; SetBoxMonData(MON_DATA_ABILITY_NUM, &value); }" (src/pokemon.c:2296-2300).
   * The bit also survives evolution unchanged — and no Gen-3 line changes ability
   * COUNT across an evolution (verified by walking evolution.h against the ability
   * table: zero 2-ability -> 1-ability and zero 1-ability -> 2-ability edges). So
   * on a one-ability species the bit is unreachable => INVALID.
   * Colosseum/XD build their mons with their own code, so origin 15 gets SUSPECT.
   * Granbull/Vibrava/Flygon list the SAME ability twice; abilities[1] is non-zero
   * there, so the bit is legitimately settable — compare against 0, not slot 0
   * (V1's `ability(1)==ability(0)` test was wrong for exactly this reason: it fired
   * only on the duplicate-ability species and never on the real case). */
  if (m->abilityNum == 1 && pk_species_ability(m->species, 1) == 0)
    pk2_add(R, PK2_CAT_STRUCT, f->is_gc ? PK2_SUSPECT : PK2_INVALID,
            "2nd ability but species has one");

  /* ...and when the species DOES have two slots, the same line fixes which one:
   * abilityNum = personality & 1. Nothing in Gen 3 changes it afterwards, so on a
   * two-ability species the slot is a genuine PID correlation — one of the very few
   * the save can contradict (nature/gender/shininess are all recomputed from the
   * PID at display time and cannot disagree with it — doc §2.1).
   *
   * TWO exemptions, both found by running this over the corpus before shipping it:
   *  - in-game trades overwrite the slot from a fixed template and stamp met
   *    location METLOC_IN_GAME_TRADE (src/trade.c:4570,4577). Guy's Emerald SEEDOT
   *    and FireRed FARFETCH'D are exactly this and would otherwise be flagged.
   *  - Colosseum/XD build their mons with their own code.
   * With those, 187 two-ability mons across the five saves match. SUSPECT, not
   * INVALID: it is a soft correlation and an unmodelled event could break it. */
  if (pk_species_ability(m->species, 1) != 0 && !f->is_gc && m->metLocation != 0xFE &&
      m->abilityNum != (uint8_t)(m->personality & 1))
    pk2_add(R, PK2_CAT_PID, PK2_SUSPECT, "Ability slot does not match the PID");

  /* Party stat formula (gen3_mon.h:72-75 flags this exact cross-check as the reason
   * pk_calc_hp/pk_calc_stat take explicit args instead of reading a PkMon). A BOX
   * record's m->stats[] is something WE computed in pk_resolve — comparing it back
   * to the same formula would be tautological. A PARTY record's m->stats[] is
   * plaintext the GAME wrote (CalculateMonStats, src/pokemon.c:2823-2862): every
   * level-up, every stat-EV gain and every load of a saved party recomputes and
   * overwrites it from base stats + IVs + EVs + nature + level. There is no retail
   * path that leaves it holding anything else, so a mismatch is a structural tell —
   * a hex-edited stat, or a record moved to a level/EV combination that was never
   * recalculated. Shedinja is the one documented exception: CalculateMonStats hard-
   * codes its Max HP to 1 regardless of the formula (its other five stats still use
   * it normally). Colosseum/XD share pokeemerald's mon-creation code but run it on
   * different hardware we have not bit-verified, so that origin is SUSPECT instead
   * of INVALID — everywhere else the formula is unconditional. */
  if (m->isParty) {
    uint8_t base[6];
    pk_base_stats(m->species, base);
    uint8_t sev = f->is_gc ? PK2_SUSPECT : PK2_INVALID;
    uint16_t want_hp = (m->species == SPECIES_SHEDINJA) ? 1
        : pk_calc_hp(base[PK_HP], m->ivs[PK_HP], m->evs[PK_HP], f->level);
    if (m->stats[PK_HP] != want_hp)
      pk2_add(R, PK2_CAT_STRUCT, sev, "Max HP does not match the stat formula");
    int nb = pk_nature_boost(m->nature), nh = pk_nature_hinder(m->nature);
    for (int s = PK_ATK; s <= PK_SPD; s++) {
      int mod = (s == nb) ? 1 : (s == nh) ? -1 : 0;
      uint16_t want = pk_calc_stat(base[s], m->ivs[s], m->evs[s], f->level, mod);
      if (m->stats[s] != want) {
        pk2_add(R, PK2_CAT_STRUCT, sev, "A stat does not match the formula");
        break;   /* one row says it; four more would just be noise */
      }
    }
  }
}

static void check_moves(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  (void)f;
  if (!m->isEgg && m->moves[0] == 0)
    pk2_add(R, PK2_CAT_MOVES, PK2_INVALID, "No moves");
  for (int i = 0; i < 4; i++) {
    uint16_t mv = m->moves[i];
    if (mv == 0) continue;
    if (mv > G3_MAX_MOVE) { pk2_add(R, PK2_CAT_MOVES, PK2_INVALID, "Move id out of range"); continue; }
    for (int j = 0; j < i; j++)
      if (m->moves[j] == mv) { pk2_add(R, PK2_CAT_MOVES, PK2_INVALID, "Duplicate move"); break; }
    uint8_t base  = pk_move_pp(mv);
    uint8_t ups   = (uint8_t)((m->ppBonuses >> (i * 2)) & 3);
    uint8_t maxpp = (uint8_t)(base + base / 5 * ups);
    if (m->pp[i] > maxpp) pk2_add(R, PK2_CAT_MOVES, PK2_INVALID, "PP above maximum");
  }
  /* Which moves the species could actually LEARN (level windows, TM/HM and tutor
   * compatibility, egg-move-only moves) is the moves hook's job — doc §3 C. */
}

static void check_met(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  if (m->metLevel > f->level) pk2_add(R, PK2_CAT_MET, PK2_INVALID, "Met level above current level");
  if (m->metLevel > 100)      pk2_add(R, PK2_CAT_MET, PK2_INVALID, "Met level above 100");
  if (m->pokeball < 1 || m->pokeball > 12)
    pk2_add(R, PK2_CAT_MET, PK2_SUSPECT, "Unusual Poke Ball id");

  /* Origin game: 1 Sapphire, 2 Ruby, 3 Emerald, 4 FireRed, 5 LeafGreen,
   * 15 Colosseum/XD. Anything else cannot occur — but a 0 origin is also the
   * signature of crude old save editors on otherwise-real mons, so SUSPECT. */
  if (m->metGame == 0 || (m->metGame > 5 && m->metGame != 15))
    pk2_add(R, PK2_CAT_MET, PK2_SUSPECT, "Unusual origin game");

  /* Met location, VALIDITY ONLY. Real MAPSEC ids run 0x00..0xD5 plus the three
   * specials 0xFD SPECIAL_EGG / 0xFE IN_GAME_TRADE / 0xFF FATEFUL; 0xD6..0xFC is
   * used by no Gen-3 game. Whether the SPECIES belongs at that location, and
   * whether the location exists in the ORIGIN game, is the encounter hook's job
   * (doc §3 E1/E2). Colosseum/XD met data is nonstandard, so skip it there. */
  if (!f->is_gc && m->metLocation > 0xD5 && m->metLocation < 0xFD)
    pk2_add(R, PK2_CAT_MET, PK2_SUSPECT, "Invalid met location");

  /* E4 — Safari Ball <-> Safari Zone (doc §3 E4). Skip the three met-location
   * markers (egg/trade/fateful aren't places) and Colosseum/XD (nonstandard met
   * data, already skipped above). INVALID direction: a Safari Ball with no
   * legitimate zone it could have come from — ball and met location both travel
   * with the mon, so there is no ordinary path to this combination. The reverse
   * (met in a Safari Zone without a Safari Ball) stays SUSPECT: PokeDNA has not
   * independently confirmed RSE never issues another ball there. */
  if (!f->is_gc && m->metLocation < 0xFD) {
    bool met_safari = (m->metLocation == MAPSEC_SAFARI_RSE ||
                        m->metLocation == MAPSEC_SAFARI_FRLG);
    if (m->pokeball == G3_BALL_SAFARI && !met_safari)
      pk2_add(R, PK2_CAT_MET, PK2_INVALID, "Safari Ball outside a Safari Zone");
    else if (met_safari && m->pokeball != G3_BALL_SAFARI)
      pk2_add(R, PK2_CAT_MET, PK2_SUSPECT, "Safari Zone catch, wrong Ball");
  }
}

/* Egg coherence — doc §3 F. Every INVALID here cites CreateEgg/SetInitialEggData
 * (src/daycare.c:828-871), which are the ONLY two routines in Gen 3 that make an
 * egg; both R/S and FR/LG share this code, and neither GC game has eggs. */
static void check_egg(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  if (!f->is_egg) {
    /* Hatching rewrites the nickname to the species name and the language to the
     * cart's (src/egg_hatch.c:345, AddHatchedMonToParty), so a non-egg still
     * carrying the Japanese egg name never came out of the hatch path. */
    if (m->raw && m->raw[REC_NICKNAME + 0] == 0x60 && m->raw[REC_NICKNAME + 1] == 0x6F &&
        m->raw[REC_NICKNAME + 2] == 0x8B && m->raw[REC_NICKNAME + 3] == 0xFF)
      pk2_add(R, PK2_CAT_EGG, PK2_SUSPECT, "Not an egg but has the egg name");
    return;
  }

  /* An egg cannot battle or enter a contest, so it cannot have earned either. */
  int evsum = 0, contest = 0;
  for (int i = 0; i < PK_NSTATS; i++) evsum += m->evs[i];
  for (int i = 0; i < 6; i++) contest += m->contest[i];
  if (evsum)   pk2_add(R, PK2_CAT_EGG, PK2_INVALID, "Egg has EVs");
  if (contest) pk2_add(R, PK2_CAT_EGG, PK2_INVALID, "Egg has contest stats");

  /* Level/EXP: CreateMon(..., EGG_HATCH_LEVEL, ...) — daycare.c:836,863. */
  if (f->level != G3_EGG_LEVEL)
    pk2_add(R, PK2_CAT_EGG, PK2_INVALID, "Egg level is not 5");
  else if (m->experience != pk_exp_for_level(f->growth, G3_EGG_LEVEL))
    pk2_add(R, PK2_CAT_EGG, PK2_INVALID, "Egg EXP is not the level-5 value");

  /* metLevel = 0 and ball = Poke Ball are written unconditionally
   * (daycare.c:837-840, 866-869). The ball stays SUSPECT only because a future
   * Colosseum/XD or event egg path would show up here first. */
  if (m->metLevel != 0)
    pk2_add(R, PK2_CAT_EGG, PK2_INVALID, "Egg met level is not 0");
  if (m->pokeball != G3_BALL_POKE)
    pk2_add(R, PK2_CAT_EGG, PK2_SUSPECT, "Egg is not in a Poke Ball");

  /* The friendship byte holds the HATCH COUNTER while the record is an egg
   * (daycare.c:842 sets it from the species' eggCycles; :910-918 counts it down).
   * 120 is the largest eggCycles value in the whole base-stats table, so anything
   * above it is impossible for every species. The exact per-species bound needs
   * the eggCycles table and belongs to the encounter/data module. */
  if (m->friendship > G3_MAX_EGG_CYCLES)
    pk2_add(R, PK2_CAT_EGG, PK2_INVALID, "Egg hatch counter over 120");

  /* PP Ups cannot be applied to an egg (no item use), and no egg-creation path
   * writes ribbons — but the fateful/event bit 31 IS legitimately set on an egg:
   * the Mystery Gift Surf Pichu is handed over as an egg with
   * `setmodernfatefulencounter` (data/scripts/gift_pichu.inc:32). So mask it out
   * before judging the ribbon word, and keep both at SUSPECT. */
  if (m->ppBonuses)
    pk2_add(R, PK2_CAT_EGG, PK2_SUSPECT, "Egg has PP Ups");
  if (m->ribbons & 0x7FFFFFFFu)
    pk2_add(R, PK2_CAT_EGG, PK2_SUSPECT, "Egg has ribbons");

  /* Every Gen-3 egg is Japanese: CreateEgg writes nickname = sJapaneseEggNickname
   * (タマゴ = 60 6F 8B FF) and language = LANGUAGE_JAPANESE regardless of the
   * cart's language (daycare.c:839-844, 866-871) — confirmed on every egg in
   * Guy's own saves (see the em_hatch comment in gen3_edit.c). A non-Japanese egg
   * is a strong tell, but stays SUSPECT: it is a naming/localisation fact, not a
   * structural impossibility, and a foreign-cart oddity would be a false ILLEGAL. */
  if (f->language != G3_LANG_JP)
    pk2_add(R, PK2_CAT_EGG, PK2_SUSPECT, "Egg language is not Japanese");
  if (m->raw && !(m->raw[REC_NICKNAME + 0] == 0x60 && m->raw[REC_NICKNAME + 1] == 0x6F &&
                  m->raw[REC_NICKNAME + 2] == 0x8B && m->raw[REC_NICKNAME + 3] == 0xFF))
    pk2_add(R, PK2_CAT_EGG, PK2_SUSPECT, "Egg name is not the JP egg name");
}

static void check_flags(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  /* Language. 0 and >7 are not language codes at all. 6 (Korean) is defined in the
   * header but "goes unused" in Gen 3 (include/constants/global.h:25) — no Korean
   * Gen-3 cart exists, so it is a tell, but it stays SUSPECT because it is a
   * plaintext byte that a legitimate-but-odd path could carry. A PkMon built by
   * hand (no backing record) has no language byte, so only judge decoded ones. */
  if (m->raw || f->language) {
    if (f->language == 0 || f->language > G3_LANG_MAX)
      pk2_add(R, PK2_CAT_FLAGS, PK2_INVALID, "Language byte invalid");
    else if (f->language == G3_LANG_KO)
      pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "Language 6 (Korean) unused in Gen 3");
  }

  if (m->raw) {
    const uint8_t* nick = m->raw + REC_NICKNAME;
    const uint8_t* otn  = m->raw + REC_OTNAME;
    int nlen = name_len(nick, 10), olen = name_len(otn, 7);

    /* Every mon is named at creation (CreateBoxMon copies the species name) and
     * every OT name comes from the save's player name, so an empty field means
     * something wrote the record that was not the game. */
    if (nlen == 0) pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "Nickname is empty");
    if (olen == 0) pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "OT name is empty");

    if (name_has_control_byte(nick, 10))
      pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "Control byte in nickname");
    if (name_has_control_byte(otn, 7))
      pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "Control byte in OT name");

    /* Japanese carts cap both nicknames and player names at 5 glyphs, and the
     * name-rater only serves the original trainer, so a 6+ glyph name on a
     * language=Japanese mon has no ordinary path. SUSPECT rather than INVALID:
     * the field itself is 10/7 bytes wide and cross-language trading plus the
     * GC games leave room for a case we have not seen. */
    if (f->language == G3_LANG_JP && (nlen > G3_JP_NAME_MAX || olen > G3_JP_NAME_MAX))
      pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "Name too long for a JP-language mon");
  }

  /* The fateful/"modern fateful encounter" bit (ribbons bit 31). The game's own
   * name for a Mew or Deoxys without it is "illegal" (src/battle_util.c:3934) —
   * such a mon disobeys, and the trade menu refuses to accept it
   * (src/trade.c:1577-1581). We still report SUSPECT, not INVALID: distributions
   * that reached players without the bit are documented (the FRLG disobeying-Mew
   * reports in docs/research-legality-v2.md §11), and calling a real event mon
   * illegal is the one mistake this checker must not make.
   *
   * The bit on any OTHER species is INFO, not a warning: it is exactly what an
   * event mon looks like, and it is set on ordinary in-game gifts too — the
   * Mystery Gift Surf Pichu egg gets it (data/scripts/gift_pichu.inc:32). */
  if (m->species == SPECIES_MEW || m->species == SPECIES_DEOXYS) {
    if (!f->fateful)
      pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "Mew/Deoxys lacks the event flag");
  } else if (f->fateful) {
    pk2_add(R, PK2_CAT_FLAGS, PK2_INFO, "Event/fateful flag is set");
  }

  /* Pokerus. RandomlyGivePartyPokerus (src/pokemon.c:6063-6096) is the only source:
   *   do { r = Random(); } while ((r & 7) == 0);   // low 3 bits non-zero
   *   if (r & 0xF0) r &= 7;                        // -> 1..7
   *   r |= r << 4;  r &= 0xF3;  r++;               // strain in both nibbles, then
   *                                                // low nibble := (strain&3)+1
   * so strain = high nibble is drawn from {1..7, 9..15} (never 0, never 8) and the
   * day counter starts at (strain & 3) + 1 and only ever counts DOWN
   * (UpdatePartyPokerusTime, :6156-6178, which also clears it to strain<<4 when it
   * expires). Days above that start value therefore cannot exist. */
  {
    uint8_t strain = (uint8_t)(m->pokerus >> 4);
    uint8_t days   = (uint8_t)(m->pokerus & 0x0F);
    if (strain == 0 && days != 0)
      pk2_add(R, PK2_CAT_FLAGS, PK2_INVALID, "Pokerus days but no strain");
    else if (days > (uint8_t)((strain & 3) + 1))
      pk2_add(R, PK2_CAT_FLAGS, PK2_INVALID, "Pokerus days above the strain max");
    else if (strain == 8)
      pk2_add(R, PK2_CAT_FLAGS, PK2_SUSPECT, "Pokerus strain 8 cannot be rolled");
  }
}

/* ---- entry point ---------------------------------------------------------- */

void pk_check_legality2_ex(const PkMon* m, Pk2Report* R, uint8_t flags) {
  if (!R) return;
  memset(R, 0, sizeof(*R));
  if (!m) return;

  /* Fatal, self-explanatory states short-circuit: every later check would be
   * reading garbage and would bury the real finding under noise. */
  if (m->isBadEgg) {
    pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "Bad egg (checksum failed)");
    R->grade = PK2_ILLEGAL;
    return;
  }
  if (m->species < 1 || m->species > G3_MAX_SPECIES) {
    pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "Species id out of range");
    R->grade = PK2_ILLEGAL;
    return;
  }
  /* Internal indices 252..276 are the 25 unused slots between Celebi and Treecko
   * (the "?" rows in the generated tables — they have no national number, no base
   * stats and no name). No Gen-3 game can create one. */
  if (pk_national_no(m->species) == 0) {
    pk2_add(R, PK2_CAT_STRUCT, PK2_INVALID, "Unused species slot");
    R->grade = PK2_ILLEGAL;
    return;
  }

  Pk2Facts f;
  f.growth     = pk_species_growth(m->species);
  /* Party records carry the level in plaintext; box records do not store one at
   * all, so derive it here instead of trusting m->level (which is 0 until the
   * caller runs pk_resolve — trusting it would false-flag every unresolved mon). */
  f.level      = m->isParty ? m->level : pk_level_from_exp(f.growth, m->experience);
  f.tid        = (uint16_t)(m->otId & 0xFFFF);
  f.sid        = (uint16_t)(m->otId >> 16);
  f.language   = m->language;
  f.species_ok = true;
  f.is_egg     = m->isEgg;
  f.is_hatched = !m->isEgg && m->metLevel == 0;
  f.is_gc      = (m->metGame == 15);
  f.fateful    = (m->ribbons >> 31) & 1;
  /* Eggs get their PID from two different RNG streams and inherit part of their
   * IVs (src/daycare.c:466-483, 551-563), and the GC games use a different LCG —
   * so no Method-1/2/4 correlation exists to look for. Doc §2.3 / §5. */
  f.pidiv_exempt = f.is_egg || f.is_hatched || f.is_gc;

  check_structure(m, &f, R);
  check_moves(m, &f, R);
  check_met(m, &f, R);
  check_egg(m, &f, R);
  check_flags(m, &f, R);

  /* Optional families. Absent ones only mark themselves in R->hooks_absent. */
  pk2_hook_moves(m, &f, R);
  pk2_hook_encounter(m, &f, R);
  pk2_hook_evolution(m, &f, R);
  if (flags & PK2_RUN_PIDIV) pk2_hook_pidiv(m, &f, R);

  R->grade = R->n_invalid ? PK2_ILLEGAL : R->n_suspect ? PK2_QUESTIONABLE : PK2_LEGAL;
}

void pk_check_legality2(const PkMon* m, Pk2Report* R) { pk_check_legality2_ex(m, R, 0); }
