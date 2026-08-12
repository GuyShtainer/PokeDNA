#include "gen3_legality_hooks.h"
#include "learnsets2.h"
#include "encounters.h"
#include "statics.h"
#include "evolutions.h"
#include "gen3_pidiv.h"
#include "data_tables.h"

/* Legality V2 hooks — see gen3_legality_hooks.h for the contract and the severity
 * policy. Decomp citations are to the checkouts under `daycare map/pokeemerald`
 * and `reference/pokeemerald_data` (line numbers as of 2026-08-09).
 *
 * Reading order: the pre-evolution chain, then the two small pure helpers each hook
 * needs, then the three hooks (moves / encounter / PIDIV) in check-catalogue order.
 */

/* Internal species ids that get their own rule. NOT National Dex numbers: the
 * Hoenn-ordered internal index is what a record stores (gen3_mon.h:20). */
#define SP_SMEARGLE  235   /* Sketch copies ANY move — no move check can apply     */
#define SP_LATIAS    407   /* the two RSE roamers, whose IVs are truncated in the  */
#define SP_LATIOS    408   /* save by the roamer glitch (gen3_pidiv.h:122-132)     */
#define G3_MAX_MOVE  354   /* MOVES_COUNT-1; the core already flags anything above */

/* The three met-location values that are not places (gen3_legality2.c:220-222). */
#define MET_SPECIAL_EGG   0xFD
#define MET_INGAME_TRADE  0xFE
#define MET_FATEFUL       0xFF

#define CHAIN_MAX  4       /* longest Gen-3 line is 3 (Azurill/Marill/Azumarill);
                            * 4 gives the walk a termination guard with slack     */

/* ---- pre-evolution table ---------------------------------------------------
 * {evolved species, its immediate pre-evolution}, ascending by the first field so
 * the lookup is a binary search. Read out of the decomps' evolution.h — the same
 * file, and the same parse, tools/gen_legality.py:153-172 already uses to build the
 * shipped learnset bitset; every one of the 184 links is a plain numeric fact.
 *
 * Cross-checked in tests/host_legality_hooks_test.c against the species names in
 * data_tables.c (so a mistyped id shows up as "SHEDINJA <- PIKACHU" rather than
 * silently widening a legality check). */
typedef struct { uint16_t evo, pre; } Pk2Evo;

static const Pk2Evo s_preevo[184] = {
  {  2,  1}, {  3,  2}, {  5,  4}, {  6,  5}, {  8,  7}, {  9,  8},
  { 11, 10}, { 12, 11}, { 14, 13}, { 15, 14}, { 17, 16}, { 18, 17},
  { 20, 19}, { 22, 21}, { 24, 23}, { 25,172}, { 26, 25}, { 28, 27},
  { 30, 29}, { 31, 30}, { 33, 32}, { 34, 33}, { 35,173}, { 36, 35},
  { 38, 37}, { 39,174}, { 40, 39}, { 42, 41}, { 44, 43}, { 45, 44},
  { 47, 46}, { 49, 48}, { 51, 50}, { 53, 52}, { 55, 54}, { 57, 56},
  { 59, 58}, { 61, 60}, { 62, 61}, { 64, 63}, { 65, 64}, { 67, 66},
  { 68, 67}, { 70, 69}, { 71, 70}, { 73, 72}, { 75, 74}, { 76, 75},
  { 78, 77}, { 80, 79}, { 82, 81}, { 85, 84}, { 87, 86}, { 89, 88},
  { 91, 90}, { 93, 92}, { 94, 93}, { 97, 96}, { 99, 98}, {101,100},
  {103,102}, {105,104}, {106,236}, {107,236}, {110,109}, {112,111},
  {117,116}, {119,118}, {121,120}, {124,238}, {125,239}, {126,240},
  {130,129}, {134,133}, {135,133}, {136,133}, {139,138}, {141,140},
  {148,147}, {149,148}, {153,152}, {154,153}, {156,155}, {157,156},
  {159,158}, {160,159}, {162,161}, {164,163}, {166,165}, {168,167},
  {169, 42}, {171,170}, {176,175}, {178,177}, {180,179}, {181,180},
  {182, 44}, {183,350}, {184,183}, {186, 61}, {188,187}, {189,188},
  {192,191}, {195,194}, {196,133}, {197,133}, {199, 79}, {202,360},
  {205,204}, {208, 95}, {210,209}, {212,123}, {217,216}, {219,218},
  {221,220}, {224,223}, {229,228}, {230,117}, {232,231}, {233,137},
  {237,236}, {242,113}, {247,246}, {248,247}, {278,277}, {279,278},
  {281,280}, {282,281}, {284,283}, {285,284}, {287,286}, {289,288},
  {291,290}, {292,291}, {293,290}, {294,293}, {296,295}, {297,296},
  {299,298}, {300,299}, {302,301}, {303,301}, {305,304}, {307,306},
  {310,309}, {312,311}, {314,313}, {316,315}, {319,318}, {324,323},
  {327,326}, {329,328}, {331,330}, {333,332}, {334,333}, {336,335},
  {338,337}, {340,339}, {342,341}, {343,342}, {345,344}, {347,346},
  {352,351}, {357,356}, {359,358}, {362,361}, {365,364}, {366,365},
  {368,367}, {371,370}, {372,371}, {374,373}, {375,373}, {378,377},
  {383,382}, {384,383}, {389,388}, {391,390}, {393,392}, {394,393},
  {396,395}, {397,396}, {399,398}, {400,399},
};

uint16_t pk2_preevo(uint16_t species) {
  int lo = 0, hi = (int)(sizeof s_preevo / sizeof s_preevo[0]);
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    if (s_preevo[mid].evo < species) lo = mid + 1;
    else hi = mid;
  }
  if (lo < (int)(sizeof s_preevo / sizeof s_preevo[0]) && s_preevo[lo].evo == species)
    return s_preevo[lo].pre;
  return 0;
}

/* Fill `out` with the species and every pre-evolution; returns how many. The guard
 * is not paranoia about the table (it is acyclic) but about a caller passing a
 * species id the table does not know. */
static int line_of(uint16_t species, uint16_t out[CHAIN_MAX]) {
  int n = 0;
  uint16_t cur = species;
  while (cur && n < CHAIN_MAX) {
    out[n++] = cur;
    cur = pk2_preevo(cur);
  }
  return n;
}

/* ---- tiny text builder ----------------------------------------------------
 * The rows want the move name and the level in them ("SOLAR BEAM: needs L46" tells
 * the player what to fix; "Move learned too early" does not). snprintf is not an
 * option: newlib's printf family allocates its buffer on the heap, and this code
 * runs on a build with ~1.5 KiB of EWRAM left. These three append to a fixed buffer
 * and simply stop at the end — the row is truncated on screen, never overrun. */
typedef struct { char* p; int cap, n; } Fmt;

static void fmt_init(Fmt* f, char* buf, int cap) { f->p = buf; f->cap = cap; f->n = 0; buf[0] = 0; }
static void fmt_s(Fmt* f, const char* s) {
  while (*s && f->n < f->cap - 1) f->p[f->n++] = *s++;
  f->p[f->n] = 0;
}
static void fmt_u(Fmt* f, uint32_t v) {
  char t[12];
  int k = 0;
  do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < (int)sizeof t);
  while (k && f->n < f->cap - 1) f->p[f->n++] = t[--k];
  f->p[f->n] = 0;
}
static void fmt_hex32(Fmt* f, uint32_t v) {
  static const char hx[] = "0123456789ABCDEF";
  fmt_s(f, "0x");
  for (int sh = 28; sh >= 0; sh -= 4)
    if (f->n < f->cap - 1) { f->p[f->n++] = hx[(v >> sh) & 0xF]; f->p[f->n] = 0; }
}

/* ================================ MOVES =====================================
 * docs/research-legality-v2.md §3 C2-C5. */

bool pk2_moves_data_ok(void) {
  if (!lg2_have_data()) return false;
  /* Every one of the four source families must be backed by REAL data in at least
   * one game group. If, say, no egg-move table had been generated, an egg move
   * would look sourceless and C3/C4/C5 would invent a SUSPECT out of a build step
   * somebody skipped. (learnsets2.h:101-110 — RS's TM/tutor rows are approximated
   * from E+FRLG, which only ever over-accepts, so those still count as backed.) */
  uint8_t any = (uint8_t)(lg2_exact_sources(PK_RS) | lg2_exact_sources(PK_EMERALD) |
                          lg2_exact_sources(PK_FRLG));
  return (any & LG2_SRC_ALL) == LG2_SRC_ALL;
}

int pk2_line_min_levelup(uint16_t species, uint16_t move) {
  uint16_t line[CHAIN_MAX];
  int n = line_of(species, line), best = -1;
  for (int i = 0; i < n; i++)
    for (int g = 0; g < 3; g++) {
      int L = lg2_min_levelup_level((PkGame)g, line[i], move);
      if (L >= 0 && (best < 0 || L < best)) best = L;
    }
  return best;
}

uint8_t pk2_line_sources(uint16_t species, uint16_t move) {
  uint16_t line[CHAIN_MAX];
  int n = line_of(species, line);
  uint8_t srcs = 0;
  for (int i = 0; i < n; i++)
    for (int g = 0; g < 3; g++) {
      /* level 0 = "ignore the level window" (learnsets2.h:73-75): C2 wants the
       * level as a number, not as a source bit that has already been judged. */
      srcs |= (uint8_t)lg2_move_sources((PkGame)g, line[i], move, 0, 0);
    }
  return srcs;
}

void pk2_hook_moves(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  if (!m || !f || !R) return;
  if (!pk2_moves_data_ok()) { R->hooks_absent |= PK2_HOOK_MOVES; return; }

  /* Whole-mon exemptions, cheapest first.
   *  - Smeargle SKETCHES any move in the game, so no move it holds is evidence of
   *    anything (its own learnset is Sketch and nothing else).
   *  - Colosseum/XD build movesets with their own code (Shadow moves, purification
   *    rewards), which no GBA learnset models. */
  if (m->species == SP_SMEARGLE || f->is_gc) return;

  /* THE breeding exemption, and the reason C2 is safe to call INVALID at all.
   * A hatched Pokemon can legitimately know a level-up move far above its level:
   * BuildEggMoveset gives the baby any move BOTH parents know that appears
   * ANYWHERE in the baby's level-up learnset — GetLevelUpMovesBySpecies returns the
   * whole list with the levels stripped (src/daycare.c:651,704-717 and
   * src/pokemon.c:6328-6336). So the level window simply does not apply to anything
   * that came out of an egg. metLevel == 0 is how the save records that. */
  bool bred = f->is_egg || f->is_hatched;
  /* Movesets that no learnset table describes. The ribbon bit is the usual event
   * marker, but it is NOT reliable on its own: real distributions that reached
   * players WITHOUT it are documented (the FRLG disobeying-Mew reports, research doc
   * §11). The met location is the second, independent marker — the game stamps
   * METLOC_FATEFUL_ENCOUNTER 0xFF on event mons (data/scripts/gift_pichu.inc:33,
   * displayed as "fateful encounter", src/pokemon_summary_screen.c:3149),
   * METLOC_IN_GAME_TRADE 0xFE on NPC trades whose moves come from a fixed template
   * (src/trade.c:4570) and METLOC_SPECIAL_EGG 0xFD on the gift eggs. None of the
   * three is an ordinary encounter, and judging one by ordinary rules is how a real
   * event Pokemon gets called illegal — the one mistake this checker must not make. */
  bool templated = f->fateful || m->metLocation >= MET_SPECIAL_EGG;

  for (int i = 0; i < 4; i++) {
    uint16_t mv = m->moves[i];
    if (mv == 0 || mv > G3_MAX_MOVE) continue;   /* range is the core's row */

    uint8_t srcs = pk2_line_sources(m->species, mv);
    char t[PK2_TEXT_LEN];
    Fmt fb;

    /* C3/C4/C5 catch-all — nothing in Gen 3 teaches this move to this line: not
     * level-up, not a TM/HM it is compatible with, not a tutor, not an egg move.
     * SUSPECT, never INVALID: the tables are wide but not omniscient (RS TM/tutor
     * rows are approximated, and event movesets exist). */
    if (srcs == 0) {
      if (templated) continue;
      fmt_init(&fb, t, sizeof t);
      fmt_s(&fb, pk_move_name(mv));
      fmt_s(&fb, ": no way to learn it");
      pk2_add(R, PK2_CAT_MOVES, PK2_SUSPECT, t);
      continue;
    }

    /* C2 — the level window. Only a move that the line can get from NOTHING but
     * level-up is testable this way; if a TM, a tutor or an egg could have supplied
     * it, the level says nothing. The minimum is taken over the whole chain and all
     * three game groups because a mon may have visited every cart and the Move
     * Reminder reteaches from the current game's list (research doc §2.2, §3 C2). */
    if (srcs == LG2_SRC_LEVELUP && !bred && !templated) {
      int need = pk2_line_min_levelup(m->species, mv);
      if (need > 0 && need > (int)f->level) {
        fmt_init(&fb, t, sizeof t);
        fmt_s(&fb, pk_move_name(mv));
        fmt_s(&fb, ": needs L");
        fmt_u(&fb, (uint32_t)need);
        fmt_s(&fb, ", mon is L");
        fmt_u(&fb, f->level);
        pk2_add(R, PK2_CAT_MOVES, PK2_INVALID, t);
        continue;
      }
    }

    /* C5 — an egg-ONLY move on a Pokemon that was never an egg. The move reminder
     * cannot teach egg moves in Gen 3 (it reteaches level-up moves), so the only
     * door is breeding, and metLevel > 0 says this one came through another. */
    if (srcs == LG2_SRC_EGG && !bred && !templated) {
      fmt_init(&fb, t, sizeof t);
      fmt_s(&fb, pk_move_name(mv));
      fmt_s(&fb, ": egg move, was not bred");
      pk2_add(R, PK2_CAT_MOVES, PK2_SUSPECT, t);
    }
  }
}

/* ============================== ENCOUNTERS ==================================
 * docs/research-legality-v2.md §3 E2 + the flat "wild anywhere" gate. */

/* BOTH tables, deliberately. The wild table alone is half a picture: it says where a
 * species can be met in the grass and at what level, and says nothing about the species
 * a map script places or hands over. Reasoning from half of it is exactly what flagged
 * three Devon-Scope Kecleon that Guy caught in normal play (statics.h). A build that has
 * encounters.c but not statics.c cannot tell those apart from forgeries, so it reports
 * this whole family ABSENT rather than guessing — hooks_absent is displayed, a wrong
 * SUSPECT is not retractable. */
bool pk2_encounter_data_ok(void) { return pk_wild_have_data() && pk_static_have_data(); }

int pk2_line_scripted(uint8_t game, uint16_t species) {
  uint16_t line[CHAIN_MAX];
  int n = line_of(species, line), found = PK_STATIC_NO;
  for (int i = 0; i < n; i++) {
    /* The whole CHAIN, for the same reason pk2_line_wild_at walks it: New Mauville's
     * static VOLTORB is met at L25 and its owner may be showing you an ELECTRODE. The
     * met data belongs to whatever form was actually obtained. */
    int r = pk_static_any(game, line[i]);
    if (r == PK_STATIC_NO_DATA) return PK_STATIC_NO_DATA;   /* absent table wins */
    if (r == PK_STATIC_YES) found = PK_STATIC_YES;
  }
  return found;
}

int pk2_line_wild_at(uint8_t game, uint8_t mapsec, uint16_t species,
                     uint8_t* lo, uint8_t* hi) {
  uint16_t line[CHAIN_MAX];
  int n = line_of(species, line), found = PK_WILD_NO;
  uint8_t l = 255, h = 0;
  for (int i = 0; i < n; i++) {
    PkWildEntry e;
    int r = pk_wild_at((PkEncGame)game, mapsec, line[i], &e);
    if (r == PK_WILD_NO_DATA) return PK_WILD_NO_DATA;   /* absent table wins */
    if (r != PK_WILD_YES) continue;
    found = PK_WILD_YES;
    /* Widest window over the chain: a Mightyena's met level belongs to whichever
     * form was actually caught, and we do not know which — so accept either. */
    if (e.minlvl < l) l = e.minlvl;
    if (e.maxlvl > h) h = e.maxlvl;
  }
  if (found == PK_WILD_YES) { if (lo) *lo = l; if (hi) *hi = h; }
  return found;
}

int pk2_line_wild_anywhere(uint8_t game, uint16_t species) {
  uint16_t line[CHAIN_MAX];
  int n = line_of(species, line), found = PK_WILD_NO;
  for (int i = 0; i < n; i++) {
    int r = pk_wild_anywhere((PkEncGame)game, line[i]);
    if (r == PK_WILD_NO_DATA) return PK_WILD_NO_DATA;
    if (r == PK_WILD_YES) found = PK_WILD_YES;
  }
  return found;
}

void pk2_hook_encounter(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  if (!m || !f || !R) return;
  if (!pk2_encounter_data_ok()) { R->hooks_absent |= PK2_HOOK_ENCOUNTER; return; }

  /* Exemptions. Each one is a case where the met fields legitimately describe
   * something other than a wild encounter, so comparing them to a wild table would
   * be judging the wrong thing:
   *  - an egg's met location is where it HATCHED and its met level is 0
   *    (src/daycare.c:837-840); nothing about that is an encounter;
   *  - Colosseum/XD met data is nonstandard (the core already skips it too);
   *  - the fateful bit marks an event distribution, and by design PokeDNA ships no
   *    event table to check one against (OVERNIGHT-DECISIONS.md §4);
   *  - 0xFD special egg / 0xFE in-game trade / 0xFF fateful encounter are markers,
   *    not places, so no wild table can have anything to say about them. */
  if (f->is_egg || f->is_hatched || f->is_gc || f->fateful) return;
  if (m->metLocation >= MET_SPECIAL_EGG) return;
  if (m->metGame < PK_ENC_SAPPHIRE || m->metGame > PK_ENC_LEAFGREEN) return;

  /* THE FIFTH EXEMPTION, and the one that was missing.
   *
   * Everything below this line reasons from the WILD TABLE — "the grass here holds these
   * species at these levels". That reasoning is only sound if the wild table describes
   * how this species is obtained. For a species a MAP SCRIPT places or gives, it does
   * not, and the failure is not hypothetical: Routes 119/120 carry an ordinary wild
   * KECLEON row at L25-25, but the Kecleon a player actually catches there is the
   * invisible one revealed with the Devon Scope, which `setwildbattle SPECIES_KECLEON,
   * 30` starts at L30 (statics.h). Comparing the second against the first accused five
   * legitimately-caught Kecleon across Guy's Emerald and Ruby, and a Berry Forest HYPNO
   * for the same reason.
   *
   * So the rule is about what this hook is ENTITLED to say, not about tuning: the wild
   * table cannot speak for a script-placed species, therefore neither can the level
   * window below NOR the "not found wild here" row — a static or a gift can be at a
   * mapsec whose wild list has never heard of it (Emerald's Aqua Hideout ELECTRODE is
   * exactly that). One return covers both because one premise fails for both.
   *
   * It is the SPECIES, not the level, that is checked, even though statics.h stores the
   * levels. The table is complete over the two script commands, but not over every way
   * the games hand a Pokemon out (C-code routes exist — statics.h says which), and it
   * carries no map. Accepting only the exact static level would be claiming a
   * completeness that has not been measured, and the cost of being wrong there is
   * another false accusation. A missing verdict is the cheap error; this is the
   * expensive one. pk_static_at_level is there for the day the map is attributable.
   *
   * PK_STATIC_NO_DATA cannot be reached from here (pk2_encounter_data_ok already
   * required the table) but is treated as silence anyway: an absent table must never be
   * the thing that produces a verdict. */
  if (pk2_line_scripted(m->metGame, m->species) != PK_STATIC_NO) return;

  uint8_t lo = 0, hi = 0;
  int at = pk2_line_wild_at(m->metGame, m->metLocation, m->species, &lo, &hi);
  if (at == PK_WILD_NO_DATA) return;      /* tri-state: no table, no verdict */

  if (at == PK_WILD_YES) {
    /* E2, second half: was it caught at a level this place can produce? */
    if (m->metLevel < lo || m->metLevel > hi) {
      char t[PK2_TEXT_LEN];
      Fmt fb;
      fmt_init(&fb, t, sizeof t);
      fmt_s(&fb, "Met at L");
      fmt_u(&fb, m->metLevel);
      fmt_s(&fb, ", wild there is L");
      fmt_u(&fb, lo);
      fmt_s(&fb, "-");
      fmt_u(&fb, hi);
      pk2_add(R, PK2_CAT_MET, PK2_SUSPECT, t);
    }
    return;
  }

  /* Not in that section's wild table. That is only evidence if the species is wild
   * SOMEWHERE in the origin game — the flat bitmap fast path. Every starter, fossil and
   * roamer is wild nowhere, so for those the honest answer is silence rather than a
   * SUSPECT nobody can act on. (This gate no longer carries the script-placed species
   * — the exemption above retired them earlier and on a better reason. It still carries
   * the routes statics.h cannot see: the roamers and the FRLG Game Corner prizes.) */
  int anywhere = pk2_line_wild_anywhere(m->metGame, m->species);
  if (anywhere != PK_WILD_YES) return;    /* covers NO and NO_DATA */

  /* ...and the section itself must HAVE wild encounters. A mapsec with no slots at
   * all (Silph Co, most towns) tells us nothing by omission: everything obtained
   * there is a gift, a trade or a prize, and "not found wild in a building" is a
   * statement about the building, not about the Pokemon. Guy's FireRed gift Lapras,
   * met in Silph Co, is exactly this case. */
  if (pk_wild_mapsec_list((PkEncGame)m->metGame, m->metLocation, 0) == 0) return;

  pk2_add(R, PK2_CAT_MET, PK2_SUSPECT, "Not found wild at its met location");
}

/* ============================== EVOLUTION ===================================
 * docs/research-legal-generator.md §3 T_evo, and §1's measured hole: "a level-5
 * Charizard grades LEGAL — no evolution-level rule exists". This is that rule.
 *
 * The claim is narrow on purpose: a Pokemon cannot be BELOW the lowest level its own
 * species can occupy. Everything else about evolution (did it have the stone, was the
 * friendship high enough, was it traded) leaves no trace in the save and is not
 * checkable. */

bool pk2_evolution_data_ok(void) { return pk_evo_have_data(); }

int pk2_evo_floor(uint16_t species) { return pk_evo_floor(species); }

void pk2_hook_evolution(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  if (!m || !f || !R) return;
  if (!pk2_evolution_data_ok()) { R->hooks_absent |= PK2_HOOK_EVO; return; }
  if (!f->species_ok) return;      /* the core already has a row for a bad species */

  /* Exemptions. Two of them are not shared with the other hooks, and both are real:
   *
   *  - AN IN-GAME TRADE HANDS YOU A POKEMON AT THE LEVEL OF THE ONE YOU GAVE AWAY.
   *    CreateInGameTradePokemonInternal reads `u8 level = GetMonData(&gPlayerParty[
   *    whichPlayerMon], MON_DATA_LEVEL)` and passes it straight to CreateMon
   *    (src/trade.c:4552,4559). So FireRed's Poliwhirl-for-Jynx trade really does
   *    produce a Jynx below L30, which is where Smoochum evolves. Without this
   *    exemption the rule would call a stock in-game trade illegal. Met 0xFE is how
   *    the game marks them (src/trade.c:4555).
   *  - Colosseum/XD build their rosters with their own code; no GBA table describes
   *    the levels they hand out, so judging them here is judging the wrong thing.
   *
   * 0xFD (special egg) and 0xFF (fateful/event) join them for the same reason they do
   * in every other hook: they are markers, not places, and PokeDNA ships no event
   * table to check an event distribution against. */
  if (f->is_gc || f->fateful) return;
  if (m->metLocation >= MET_SPECIAL_EGG) return;

  int need = pk2_evo_floor(m->species);
  if (need <= 1) return;                     /* base form, or PK_EVO_NO_DATA (-1) */
  if ((int)f->level >= need) return;

  /* SUSPECT, not INVALID. source/statics.h now enumerates the script placements instead
   * of leaving them to memory, and it agrees with what this comment used to only assert:
   * of the 71 rows across the five carts, exactly three species are not base forms —
   * ELECTRODE (L30 in RSE, L34 in FRLG, floor 26), HYPNO (L30, floor 26) and MAROWAK
   * (L30, floor 28) — and every one of them is placed at or above its own floor, so none
   * is a counterexample. That is still not the
   * measured-zero calibration the INVALID bar asks for: the table covers two script
   * commands, not the C-code routes (statics.h), and the wild relaxation inside
   * pk_evo_floor already had to absorb 23 species the naive rule would have called
   * illegal (evolutions.h) — exactly the kind of surprise that argues for caution.
   * Promoting this needs the C-code gift routes too, and a re-run of the corpus gate in
   * tests/host_legality_hooks_test.c part (C). */
  char t[PK2_TEXT_LEN];
  Fmt fb;
  fmt_init(&fb, t, sizeof t);
  fmt_s(&fb, "Evolves at L");
  fmt_u(&fb, (uint32_t)need);
  fmt_s(&fb, ", this one is L");
  fmt_u(&fb, f->level);
  pk2_add(R, PK2_CAT_STRUCT, PK2_SUSPECT, t);
}

/* ================================ PIDIV =====================================
 * docs/research-legality-v2.md §3 B1/B2. On demand only: pk_check_legality2_ex
 * calls this hook solely under PK2_RUN_PIDIV, because 30 x a 65,536-iteration
 * LCRNG search is a visible stall in a box sweep (OVERNIGHT-DECISIONS.md §3). */

uint8_t pk2_pidiv_report_method(uint8_t method) {
  switch (method) {
    case PK_PIDIV_M1: return 1;
    case PK_PIDIV_M2: return 2;
    case PK_PIDIV_M4: return 4;
    case PK_PIDIV_M1_REV:
    case PK_PIDIV_M2_REV:
    case PK_PIDIV_M4_REV: return 5;   /* "reversed order", gen3_legality2.h:79-80 */
    default: return 0;
  }
}

void pk2_hook_pidiv(const PkMon* m, const Pk2Facts* f, Pk2Report* R) {
  if (!m || !f || !R) return;
  char t[PK2_TEXT_LEN];
  Fmt fb;

  PkPidiv r;
  pk_pidiv_check(m, &r);      /* applies egg / hatched / fateful / GC itself */
  R->pidiv_ran = 1;
  R->pidiv_method = 0;
  R->pidiv_seed = 0;

  /* B3, the one exemption gen3_pidiv.c cannot apply itself (gen3_pidiv.h:140-143):
   * an in-game trade's PID is FIXED in the trade template, so it has no relation to
   * any RNG stream. The game stamps METLOC_IN_GAME_TRADE on them (src/trade.c:4577),
   * which is all the identification this needs — the fixed-PID table is not required
   * to know that a traded mon must not be searched. Measured over Guy's five saves:
   * four of the five mons that failed the search were NPC trades (SEEDOT, PLUSLE,
   * FARFETCH'D, ...), so this exemption removes four false SUSPECTs. The fifth is a
   * genuinely wild Emerald SANDSHREW and stays — Method 3 is deliberately not
   * searched (gen3_pidiv.h:56-61), and a SUSPECT is what that costs. */
  if (r.exempt == PK_PIDIV_EX_NONE && m->metLocation == MET_INGAME_TRADE)
    r.exempt = PK_PIDIV_EX_TRADE;

  if (r.exempt != PK_PIDIV_EX_NONE) {
    /* Say WHY rather than showing nothing: "not testable (hatched)" is a different
     * statement from "tested and passed", and the screen must not blur them. */
    fmt_init(&fb, t, sizeof t);
    fmt_s(&fb, "PID/IV not testable (");
    fmt_s(&fb, pk_pidiv_exempt_name(r.exempt));
    fmt_s(&fb, ")");
    pk2_add(R, PK2_CAT_PID, PK2_INFO, t);
    return;
  }

  if (r.method != PK_PIDIV_NONE) {
    R->pidiv_method = pk2_pidiv_report_method(r.method);
    R->pidiv_seed = r.seed;
    fmt_init(&fb, t, sizeof t);
    fmt_s(&fb, pk_pidiv_method_name(r.method));
    fmt_s(&fb, ", seed ");
    fmt_hex32(&fb, r.seed);
    pk2_add(R, PK2_CAT_PID, PK2_INFO, t);
    return;
  }

  /* The RSE roaming Latias/Latios store only the low 8 bits of the first IV word
   * (gen3_pidiv.h:122-132), so the normal 30-bit compare cannot match them. Both of
   * Guy's Ruby roamers are exactly this case and would otherwise be the only
   * legitimately-caught mons in the corpus flagged here. The retry is deliberately
   * INFO, not a pass: an 8-bit match is close to no evidence at all. */
  if ((m->species == SP_LATIAS || m->species == SP_LATIOS) && m->metGame <= PK_ENC_EMERALD) {
    PkPidiv rr;
    pk_pidiv_search(m->personality, m->ivs, PK_PIDIV_OPT_ROAMER, &rr);
    if (rr.method != PK_PIDIV_NONE) {
      R->pidiv_method = pk2_pidiv_report_method(rr.method);
      R->pidiv_seed = rr.seed;
      pk2_add(R, PK2_CAT_PID, PK2_INFO, "PID/IV roamer match (weak evidence)");
      return;
    }
  }

  /* SUSPECT and never worse: a non-match is a negative filter (gen3_pidiv.h:62-67).
   * Unmodelled distributions, Method 3, and honest oddities all land here. */
  pk2_add(R, PK2_CAT_PID, PK2_SUSPECT, "No PID/IV RNG method matches");
}
