#include "gen3_edit.h"
#include "gen3_save.h"     /* gen3_decode_char (for symmetry doc), sizes */
#include "data_tables.h"   /* base stats, nature mods, growth, exp, move PP */
#include "gen3_daycare.h"  /* pk_egg_group — the shipped breedability table   */
#include "learnsets2.h"    /* lg2_levelup_list / lg2_egg_list — real movesets */
#include "evolutions.h"    /* pk_evo_floor — the lowest level a species can stand at */
#include <string.h>

/* --- little-endian helpers --- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* substruct slot order by personality % 24 (same table as gen3_mon.c). */
static const uint8_t k_substruct_pos[24][3] = {
  {0,1,2},{0,1,3},{0,2,1},{0,3,1},{0,2,3},{0,3,2},
  {1,0,2},{1,0,3},{2,0,1},{3,0,1},{2,0,3},{3,0,2},
  {1,2,0},{1,3,0},{2,1,0},{3,1,0},{2,3,0},{3,2,0},
  {1,2,3},{1,3,2},{2,1,3},{3,1,2},{2,3,1},{3,2,1},
};

uint8_t gen3_encode_char(char c) {
  if (c == ' ') return 0x00;
  if (c >= '0' && c <= '9') return (uint8_t)(0xA1 + (c - '0'));
  if (c >= 'A' && c <= 'Z') return (uint8_t)(0xBB + (c - 'A'));
  if (c >= 'a' && c <= 'z') return (uint8_t)(0xD5 + (c - 'a'));
  switch (c) {
    case '!':  return 0xAB;
    case '?':  return 0xAC;
    case '.':  return 0xAD;
    case '-':  return 0xAE;
    /* The real charmap's own compiler aliases a bare apostrophe to 0xB4 (the closing
     * curly quote), not 0xB3 (the opening one, which has no ASCII alias at all) --
     * charmap.txt carries both `'''' = B4` AND `'\''  = B4` for exactly this. Encoding
     * to 0xB3 was the mirror image of the comma/slash bug below: gen3_decode_char folds
     * BOTH 0xB3 and 0xB4 to '\'' for display (a reasonable simplification), but the
     * ENCODE direction has only one real target and it is B4. */
    case '\'': return 0xB4;
    /* Same swap as gen3_decode_char (source/gen3_save.c) had, mirrored: this used to
     * send ',' to 0xBA (the SLASH code point) and had no case for '/' at all, so typing
     * a slash into a nickname silently produced a space (default case), and a comma
     * silently produced a slash. 0xB8 = ',' and 0xBA = '/' in the real Gen-3 charmap. */
    case ',':  return 0xB8;
    case '/':  return 0xBA;
    default:   return 0x00;   /* unknown -> space */
  }
}

void gen3_edit_load(const uint8_t* rec, bool is_party, EditMon* e) {
  memset(e, 0, sizeof(*e));
  e->is_party = is_party;
  memcpy(e->raw, rec, is_party ? 100 : 80);
  e->personality = rd32(rec + 0x00);
  e->otId = rd32(rec + 0x04);
  uint32_t key = e->personality ^ e->otId;

  uint8_t dec[48];
  for (int w = 0; w < 12; w++) wr32(dec + w * 4, rd32(rec + 0x20 + (uint32_t)w * 4) ^ key);

  const uint8_t* pos = k_substruct_pos[e->personality % 24];
  int g = pos[0], a = pos[1], ev = pos[2], m = 6 - (g + a + ev);
  memcpy(e->sub[0], dec + (uint32_t)g  * 12, 12);  /* Growth  */
  memcpy(e->sub[1], dec + (uint32_t)a  * 12, 12);  /* Attacks */
  memcpy(e->sub[2], dec + (uint32_t)ev * 12, 12);  /* EVs     */
  memcpy(e->sub[3], dec + (uint32_t)m  * 12, 12);  /* Misc    */
}

void gen3_edit_commit(const EditMon* e, uint8_t* out) {
  int n = e->is_party ? 100 : 80;
  memcpy(out, e->raw, n);                       /* preserve ALL plaintext (incl. stored stats/padding) */

  uint32_t pers = e->personality, key = pers ^ e->otId;
  wr32(out + 0x00, pers);
  wr32(out + 0x04, e->otId);

  uint8_t dec[48];
  const uint8_t* pos = k_substruct_pos[pers % 24];
  int g = pos[0], a = pos[1], ev = pos[2], m = 6 - (g + a + ev);
  memcpy(dec + (uint32_t)g  * 12, e->sub[0], 12);
  memcpy(dec + (uint32_t)a  * 12, e->sub[1], 12);
  memcpy(dec + (uint32_t)ev * 12, e->sub[2], 12);
  memcpy(dec + (uint32_t)m  * 12, e->sub[3], 12);

  uint16_t sum = 0;                             /* per-mon checksum: sum of the 24 u16 (order-invariant) */
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + rd16(dec + h * 2));
  wr16(out + 0x1C, sum);

  for (int w = 0; w < 12; w++) wr32(out + 0x20 + (uint32_t)w * 4, rd32(dec + w * 4) ^ key);
}

/* ---- field mutators ---- */

/* Recompute the party plaintext stats from the current IVs/EVs/level/nature/species.
 * Called only after a stat-affecting edit, so a true no-op stays byte-identical. */
static void recompute_party_stats(EditMon* e) {
  if (!e->is_party) return;
  uint16_t species = rd16(e->sub[0] + 0);
  uint8_t base[6];
  pk_base_stats(species, base);
  uint32_t iv = rd32(e->sub[3] + 4);
  uint8_t ivs[6] = {
    (uint8_t)(iv & 0x1F), (uint8_t)((iv >> 5) & 0x1F), (uint8_t)((iv >> 10) & 0x1F),
    (uint8_t)((iv >> 15) & 0x1F), (uint8_t)((iv >> 20) & 0x1F), (uint8_t)((iv >> 25) & 0x1F),
  };
  const uint8_t* ev = e->sub[2];
  uint8_t level = e->raw[0x54];
  uint8_t nat = (uint8_t)(e->personality % 25);
  int nb = pk_nature_boost(nat), nh = pk_nature_hinder(nat);
  uint16_t hp = pk_calc_hp(base[PK_HP], ivs[PK_HP], ev[PK_HP], level);
  wr16(e->raw + 0x58, hp);
  wr16(e->raw + 0x56, hp);                      /* current HP = max */
  for (int s = PK_ATK; s <= PK_SPD; s++) {
    int mod = (s == nb) ? 1 : (s == nh) ? -1 : 0;
    wr16(e->raw + 0x58 + s * 2, pk_calc_stat(base[s], ivs[s], ev[s], level, mod));
  }
}

void em_set_iv(EditMon* e, int stat, uint8_t v) {
  if (v > 31) v = 31;
  uint32_t iv = rd32(e->sub[3] + 4);
  iv &= ~(0x1Fu << (stat * 5));
  iv |= ((uint32_t)v << (stat * 5));
  wr32(e->sub[3] + 4, iv);
  recompute_party_stats(e);
}

void em_set_ev(EditMon* e, int stat, uint8_t v) {
  e->sub[2][stat] = v;
  recompute_party_stats(e);
}

/* Contest condition (cool/beauty/cute/smart/tough/sheen) — bytes 6..11 of the EVs
 * substruct; cosmetic (no effect on battle stats), so no stat recompute. i = 0..5. */
void em_set_contest(EditMon* e, int i, uint8_t v) {
  if (i < 0 || i > 5) return;
  e->sub[2][6 + i] = v;
}

/* Met location for a Pokémon PokeDNA creates.
 *
 * It is deliberately NOT 0xFF. 0xFF is METLOC_FATEFUL_ENCOUNTER, the marker the game
 * stamps on an event distribution (data/scripts/gift_pichu.inc:33, rendered as
 * "fateful encounter" by src/pokemon_summary_screen.c:3149). Stamping it did two bad
 * things: every mon this tool made claimed to be a distributed event Pokémon — a false
 * provenance claim written into the user's save by a tool whose whole posture is
 * telling the truth about save data — and it MUTED PokeDNA's own checker, because
 * gen3_legality_hooks.c:195 and :300 exempt every met location >= 0xFD from the move
 * and encounter reasoning. The tool was hiding its output from its own auditor.
 *
 * So: an ordinary place, in the region of the game the record says it came from, that
 * actually HAS wild encounters — so the checker has something real to reason about and
 * a created mon is judged like any other. The first route of each region is the most
 * ordinary place a Gen-3 Pokémon can come from. Ids from the decomps' own
 * src/data/region_map/region_map_sections.json (index == MAPSEC value):
 *   0x10 MAPSEC_ROUTE_101 (Hoenn, pokeemerald)  — POOCHYENA/ZIGZAGOON/WURMPLE L2-3
 *   0x65 MAPSEC_ROUTE_1   (Kanto, pokefirered)  — PIDGEY L2-5 / RATTATA L2-4
 * Both are also where the retail player's own first Pokémon comes from. */
#define G3_METLOC_ROUTE_101  0x10
#define G3_METLOC_ROUTE_1    0x65

/* Origin game (the record's own metGame byte) -> a met location in that game's region.
 * 4/5 are FireRed/LeafGreen and are the only Kanto games; 1/2/3 (Sapphire/Ruby/Emerald)
 * and anything unrecognised fall to Hoenn, which is where the default origin lands too.
 * Origin 15 (Colosseum/XD) is exempted by the checker anyway (gen3_legality2.c:410). */
static uint8_t default_metloc(uint8_t metgame) {
  return (metgame == 4 || metgame == 5) ? G3_METLOC_ROUTE_1 : G3_METLOC_ROUTE_101;
}

/* ---- the ORIGIN a built Pokémon claims ------------------------------------------
 *
 * THE EGG IS THE ONE ORIGIN THIS TOOL CAN HONESTLY PRODUCE, and it is also the only
 * one PokeDNA's own checker can be satisfied by without data PokeDNA does not have.
 *
 * A CAUGHT mon's PID and its IVs are not independent: retail CreateBoxMon draws the
 * PID and then both IV words from ONE LCRNG stream (src/pokemon.c:2216, 2277-2293),
 * so a hand-picked PID with hand-picked IVs matches no seed and gen3_pidiv.c says so
 * ("No PID/IV RNG method matches"). A BRED mon has no such correlation to break: the
 * offspring PID is `(Random2() << 16) | ((Random() % 0xfffe) + 1)` (src/daycare.c:466)
 * — the high half comes from gRng2Value, a completely separate generator — and the IVs
 * are then partly overwritten by parent inheritance. PokeDNA agrees and exempts it:
 * pk_pidiv_exempt_reason() returns PK_PIDIV_EX_HATCHED for metLevel == 0
 * (gen3_pidiv.c:152), and Pk2Facts.is_hatched (gen3_legality2.c:409) keys the move
 * level-window and encounter exemptions off the same byte. So metLevel 0 buys free
 * PID, free IVs, free nature, free shininess — with no search and no new table.
 *
 * It is not an exemption LIE either, which is the distinction that matters here.
 * The four escape hatches (met 0xFD special egg / 0xFE in-game trade / 0xFF fateful,
 * and ribbon bit 31) all make the checker skip its reasoning by claiming the record
 * came from somewhere it did not; this tool writes none of them. metLevel 0 claims
 * only "this line came out of an egg", which is true for anything with a breedable
 * line — including the evolved forms, since a bred Charmander that becomes a
 * Charizard keeps metLevel 0 for life.
 *
 * WHO CANNOT COME FROM AN EGG. pk_egg_group() already ships the real table
 * (gen3_daycare.c:42) and returns 15 = UNDISCOVERED for everything unbreedable — but
 * UNDISCOVERED alone is the wrong test, because the BABY forms are in it too: Pichu,
 * Cleffa, Igglybuff, Togepi, Tyrogue, Smoochum, Elekid, Magby and Azurill cannot
 * breed themselves, yet they are precisely what comes OUT of an egg. The game itself
 * settles that: it defines a list of EGG MOVES for exactly those nine and for none of
 * the 21 legendaries or Unown (measured over the generated tables — see the
 * "no false positive" assertion in tests/host_legalbuild_test.c). A species the game
 * gives egg moves to is a species the game hatches, so the two shipped tables answer
 * the question between them and no new data is invented. */
#define G3_EGG_UNDISCOVERED  15

bool gen3_species_can_hatch(uint16_t species) {
  if (species < 1 || species > G3_MAX_SPECIES) return false;
  if (pk_egg_group(species, 0) != G3_EGG_UNDISCOVERED) return true;
  for (int g = 0; g < 3; g++) {
    const uint16_t* eggs;
    if (lg2_egg_list((PkGame)g, species, &eggs) > 0) return true;
  }
  return false;
  /* Undercounts by three and never over-counts: Wynaut (Wobbuffet + Lax Incense) and
   * Nidorina/Nidoqueen (hatch as Nidoran♀, then evolve) really are egg-obtainable,
   * but saying so needs the forward evolution table that this tree does not have yet
   * (docs/research-legal-generator.md §3, T_evo). They fall to the caught origin
   * below, which is the safe direction: a mon that is flagged is better than a mon
   * that lies. */
}

/* Level-up moves this species knows at `lvl` in one game group, newest last, in the
 * shape the record wants. The games' list is in ascending-level order, a mon holds at
 * most four moves and forgets the oldest when it learns a fifth, and re-learning a
 * move it already has is a no-op — so the window below is what a real Pokémon of that
 * species and level actually carries. Returns how many slots were filled. */
static int levelup_moves_in(PkGame g, uint16_t species, uint8_t lvl, uint16_t out[4]) {
  const uint16_t* list;
  int n = lg2_levelup_list(g, species, &list), used = 0;
  for (int i = 0; i < n; i++) {
    if (LG2_LV_LEVEL(list[i]) > (int)lvl) continue;      /* not learned yet */
    uint16_t mv = LG2_LV_MOVE(list[i]);
    if (mv == 0) continue;
    int dup = 0;
    for (int k = 0; k < used; k++) if (out[k] == mv) dup = 1;
    if (dup) continue;                                   /* a duplicate move is INVALID */
    if (used < 4) out[used++] = mv;
    else { out[0] = out[1]; out[1] = out[2]; out[2] = out[3]; out[3] = mv; }
  }
  return used;
}

/* Origin-game byte -> the learnset group that game belongs to (1 Sapphire, 2 Ruby,
 * 3 Emerald, 4 FireRed, 5 LeafGreen; anything else is treated as Emerald, which is
 * also what default_metloc falls back to). */
static PkGame group_of(uint8_t metgame) {
  if (metgame == 1 || metgame == 2) return PK_RS;
  if (metgame == 4 || metgame == 5) return PK_FRLG;
  return PK_EMERALD;
}

/* The moves a built Pokémon starts with. Its OWN learnset, at its own level, from the
 * game it says it came from — NOT a placeholder. The old hard-coded Tackle was a real
 * legality failure and not a cosmetic one: 263 of the 386 species cannot learn Tackle
 * by any Gen-3 method, so pk2_hook_moves said "TACKLE: no way to learn it", and five
 * species learn it late enough that the level-window rule called it INVALID outright.
 *
 * A level-up move at or below the current level is legal on BOTH origins this builder
 * emits, so there is one code path: a hatched mon may also carry egg moves, but it
 * does not need them, and a caught mon may not. */
static int default_moves(uint16_t species, uint8_t lvl, uint8_t metgame, uint16_t out[4]) {
  int n = levelup_moves_in(group_of(metgame), species, lvl, out);
  /* Fall back across the other groups only if the origin's own table said nothing —
   * every species has a level-1 move somewhere, so this is for a partially generated
   * table rather than for any real species. */
  for (int g = 0; g < 3 && n == 0; g++) n = levelup_moves_in((PkGame)g, species, lvl, out);
  return n;
}

/* ---- THE LEVEL A CREATED POKEMON STARTS AT ---------------------------------------
 *
 * It used to be 5 for everything, and that made the create flow build Pokemon that
 * cannot exist: a level-5 Charizard, which the checker then (correctly, since T_evo
 * shipped) flagged with "Evolves at L36, this one is L5". Guy's words for the bug were
 * "the charizard is lvl 5 ... though it does come out questionable", and his original
 * ask was the fix: "when creating a Charizard, it will have its correct MINIMUM LEVEL
 * (it must evolve to there)". BUILDING IT RIGHT BEATS FLAGGING IT.
 *
 * WHICH floor. evolutions.h exposes two, and this deliberately uses the LOWER one:
 *   - pk_evo_min_level() is the pure evolution walk (Charizard 36, Gyarados 20).
 *   - pk_evo_floor() is min(that, the lowest level the species appears at in any of the
 *     five carts' wild tables), because retail really does hand out evolved forms below
 *     their own evolution level — Sootopolis' Super Rod has L5 Gyarados, FireRed's
 *     Safari Zone has L20 Poliwhirl (evolutions.h, 23 species).
 * pk_evo_floor is also the number pk2_hook_evolution judges against
 * (gen3_legality_hooks.c:355), so building AT it is building at the lowest level this
 * tool's own checker accepts — which is the promise gen3_build_mon makes. Using the
 * higher number instead would hand the user a L20 Gyarados the game itself would have
 * given them at L5, i.e. it would over-correct in the one direction the project forbids.
 *
 * 5 IS THE BASE, not a minimum bound of the data: a Gen-3 egg hatches at level 5
 * (src/egg_hatch.c), and metLevel 0 — "hatched at" — is the origin gen3_build_mon
 * claims for everything with an egg route, so 5 stays the answer whenever the species'
 * own floor is at or below it. Raising the level never touches the met level: the
 * PID/IV exemption keys off metLevel == 0 (gen3_pidiv.c:152), and a bred Charmander
 * that grew into a Charizard keeps metLevel 0 for life, so L36 + met 0 is exactly what
 * a real one looks like.
 *
 * FAILS OPEN. With source/evolutions.c not generated, pk_evo_floor returns
 * PK_EVO_NO_DATA (evolutions.h's weak fallback) and this returns 5 — the old behaviour,
 * unchanged. A missing table must never invent a level. */
#define G3_BUILD_BASE_LVL 5

uint8_t gen3_build_level(uint16_t species) {
  int floor = pk_evo_floor(species);
  if (floor == PK_EVO_NO_DATA || floor <= G3_BUILD_BASE_LVL) return G3_BUILD_BASE_LVL;
  if (floor > 100) return 100;                  /* nothing in Gen 3 reaches this; clamp anyway */
  return (uint8_t)floor;
}

/* Build a default, VALID 80-byte box record for `species` at `lvl` from nothing — for
 * the "create a Pokémon" flow (caller then opens the editor to customise + commit). The
 * record is a real present mon: species set, exp matching the level, hasSpecies flag set
 * (raw[0x13] bit1), not an egg, checksum written by commit. IVs/EVs/condition default 0,
 * friendship 70, a Poké Ball, real level-up moves. Pure (no globals/UI) so it's
 * host-testable. otName <= 7 chars; metgame 1..15 (0 -> Emerald).
 *
 * The aim is that the result passes PokeDNA'S OWN CHECKER with no user input, because
 * a tool that writes records its own auditor rejects has no business auditing anyone
 * else's. Measured by tests/host_legalbuild_test.c over all 386 species. */
void gen3_build_mon(uint16_t species, uint8_t lvl, uint32_t pid, uint32_t otId,
                    const char* otName, uint8_t metgame, uint8_t out[80]) {
  /* lvl 0 = "you pick" -> the species' own floor. An EXPLICIT level is honoured exactly,
   * even below the floor, because the other caller is gen12_convert.c: a Gen-1/2 import
   * must land at the level the imported Pokemon actually had, and silently promoting it
   * would be rewriting the user's own data. The create flow asks for 0 (or passes
   * gen3_build_level itself); the importer never does. */
  if (lvl < 1) lvl = gen3_build_level(species);
  if (lvl > 100) lvl = 100;
  EditMon e; memset(&e, 0, sizeof e);
  e.is_party = false;
  e.personality = pid ? pid : 0x1234ABCDu;
  e.otId = otId;
  e.raw[0x12] = 2;                              /* language: English */
  e.raw[0x13] = 0x02;                           /* flags: hasSpecies (present; not egg/bad-egg) */
  e.sub[0][0] = (uint8_t)species;               /* Growth substruct: species (LE) */
  e.sub[0][1] = (uint8_t)(species >> 8);
  uint8_t mg = metgame ? metgame : 3;           /* default Emerald */
  /* 70 is the CAUGHT base friendship and stays the builder's contract: it is what a
   * Gen-1 import keeps (gen12_convert.c has no friendship of its own for Gen 1) and
   * pdna_origin_art.c reads it back as part of that import's signature. Nothing in the
   * legality catalogue judges a non-egg's friendship, and 70 is a value a hatched mon
   * reaches anyway (fainting lowers it), so this is honest as well as compatible — the
   * fresh-from-the-egg 120 is applied by app_create_mon, which knows it just made one. */
  em_set_friendship(&e, 70);
  em_set_level(&e, lvl);                        /* exp for the level (uses species growth rate) */
  em_set_nickname(&e, pk_species_name(species));
  em_set_otname(&e, otName ? otName : "");
  em_set_ball(&e, 4);                           /* Poké Ball */
  em_set_metgame(&e, mg);
  em_set_metloc(&e, default_metloc(mg));        /* an ordinary place, not "event mon" */

  /* metLevel 0 = "hatched at" (src/egg_hatch.c:384-386). For a species with no egg
   * route the record instead says, plainly, that it was met at its current level in an
   * ordinary place — the truth about what this tool did, with no exemption claimed.
   * It is also the flagged case: with PK2_RUN_PIDIV the checker correctly reports "No
   * PID/IV RNG method matches", because a static encounter's spread cannot be
   * fabricated without the static table (research-legal-generator.md §4). Callers can
   * ask gen3_species_can_hatch() first and warn; app_create_mon does. */
  em_set_metlevel(&e, gen3_species_can_hatch(species) ? 0 : lvl);

  /* Ability slot. CreateBoxMon writes `value = personality & 1` and ONLY when the
   * species has a second ability (src/pokemon.c:2296-2300); leaving the bit at 0 on an
   * odd PID is what made 128 of the 386 species report "Ability slot does not match
   * the PID". On a one-ability species the bit must stay 0 — there it is INVALID. */
  if (pk_species_ability(species, 1) != 0)
    em_set_ability(&e, (uint8_t)(e.personality & 1u));

  uint16_t mv[4];
  int nm = default_moves(species, lvl, mg, mv);
  /* Only reachable when source/learnsets2.c was never generated (its weak fallbacks
   * return an empty list, learnsets2.h:137). A move-less mon is INVALID by the core's
   * own rule, so fail towards a legal-looking record rather than an illegal one; the
   * checker's move hook is switched off in that build anyway (pk2_moves_data_ok). */
  if (nm == 0) mv[nm++] = 33;                   /* TACKLE */
  for (int i = 0; i < nm; i++) em_set_move(&e, i, mv[i]);

  gen3_edit_commit(&e, out);
}

void em_set_species(EditMon* e, uint16_t species) {
  wr16(e->sub[0] + 0, species);
  recompute_party_stats(e);
}

void em_set_item(EditMon* e, uint16_t item) { wr16(e->sub[0] + 2, item); }

void em_set_move(EditMon* e, int i, uint16_t move) {
  wr16(e->sub[1] + i * 2, move);
  e->sub[1][8 + i] = pk_move_pp(move);          /* reset PP to base */
  e->sub[0][8] &= ~(0x3u << (i * 2));           /* PP Ups bind to the move, not the slot:
                                                 * clear this slot's PP-Up bonus (Growth byte 8),
                                                 * mirroring the game's RemoveMonPPBonus. */
}

/* Deliberately NOT clamped to em_pp_max: gen12_convert.c writes the PP-Up'd current PP
 * before it restores the bonus byte (em_set_move clears it), so a clamp here would cut
 * every transferred move back to its base PP. The ceiling is enforced where the ceiling
 * is chosen — em_set_ppups below, and the editor's PP row. */
void em_set_pp(EditMon* e, int i, uint8_t pp) { e->sub[1][8 + i] = pp; }

/* ---- the move's MAXIMUM PP ------------------------------------------------
 * Gen 3 does not STORE a maximum. It derives one, every time it needs it, from the
 * move's base PP and the 2-bit PP-Up count for that SLOT (Growth byte 8, bits 2*slot):
 *
 *     CalculatePPWithBonus(move, ppBonuses, slot)          pokeemerald src/pokemon.c
 *       = gBattleMoves[move].pp + gBattleMoves[move].pp * 20 * ups / 100
 *
 * That product-then-divide is reproduced exactly rather than as `base / 5 * ups`: every
 * retail base PP is a multiple of 5 so the two agree today, but they diverge the moment
 * one is not, and the game's own order is the one a legality checker has to match.
 *
 * So "raise a move's maximum PP" means "give it a PP Up", and the only legal maxima are
 * the four the item can reach. Nothing here can write a fifth. */
uint8_t em_pp_max(uint16_t move, uint8_t ppBonuses, int slot) {
  if (slot < 0 || slot > 3) return 0;
  uint32_t base = pk_move_pp(move);
  uint32_t ups  = (uint32_t)((ppBonuses >> (slot * 2)) & 3u);
  return (uint8_t)(base + base * 20u * ups / 100u);
}

uint8_t em_get_ppups(const EditMon* e, int i) {
  if (i < 0 || i > 3) return 0;
  return (uint8_t)((e->sub[0][8] >> (i * 2)) & 3u);
}

/* Applying a PP Up in game does two things (ItemUseCB_PPUp -> pokemon_item_effect):
 * it bumps the slot's 2-bit counter AND adds the PP the bump just bought to the CURRENT
 * PP. Both are done here, so raising the maximum behaves like the item and lowering it
 * can never leave current PP stranded above the new ceiling.
 *
 * An empty slot is forced to 0 Ups: the game clears a slot's bonus with the move
 * (RemoveMonPPBonus), and "PP Ups on a move that isn't there" is a state no cartridge
 * can produce. */
void em_set_ppups(EditMon* e, int i, uint8_t ups) {
  if (i < 0 || i > 3) return;
  uint16_t move = rd16(e->sub[1] + i * 2);
  if (ups > 3) ups = 3;
  if (move == 0) ups = 0;
  uint8_t old_max = em_pp_max(move, e->sub[0][8], i);
  e->sub[0][8] = (uint8_t)((e->sub[0][8] & ~(3u << (i * 2))) | ((uint32_t)ups << (i * 2)));
  uint8_t new_max = em_pp_max(move, e->sub[0][8], i);
  int cur = (int)e->sub[1][8 + i] + ((int)new_max - (int)old_max);
  if (cur < 0) cur = 0;
  if (cur > (int)new_max) cur = new_max;
  e->sub[1][8 + i] = (uint8_t)cur;
}

void em_set_friendship(EditMon* e, uint8_t f) { e->sub[0][9] = f; }

/* The egg flag lives in TWO places: the record flags byte (raw[0x13] bit2) and the Misc
 * substruct's IV/egg word (bit 30 == Misc byte 7 bit 6). Set/clear both together. */
void em_set_egg(EditMon* e, bool egg) {
  if (egg) { e->raw[0x13] |= 0x04; e->sub[3][7] |= 0x40; }
  else     { e->raw[0x13] &= (uint8_t)~0x04; e->sub[3][7] &= (uint8_t)~0x40; }
}

/* Hatch an egg: clear the egg flag, replace the stored hatch-cycle counter (which lives in
 * the friendship byte) with the friendship retail gives a newly hatched mon, stamp met
 * level 0, and set level 5 — Gen-3 eggs hatch at level 5, and em_set_level recomputes exp +
 * the party plaintext stats. The species/IVs/moves/nature inside the egg are already
 * present, so the revealed Pokemon is complete.
 *
 * FRIENDSHIP IS 120, NOT 70. CreateHatchedMon hard-codes it: `friendship = 120;
 * SetMonData(temp, MON_DATA_FRIENDSHIP, &friendship);` (src/egg_hatch.c:350-351) — there
 * is no species term and no other path. Measured over Guy's five saves: of the 141 mons
 * with metLevel 0, 120 is the single largest bucket (43) and 126 of them sit at 120 or
 * above, the spread upwards being the walking/level-up gains that follow. The ones below
 * are ordinary friendship losses (fainting) on mons hatched long ago. 70 appeared nowhere
 * in the hatch path — it is the base friendship a CAUGHT mon gets, which is why
 * gen3_build_mon still uses it and this no longer does.
 *
 * MET LEVEL IS 0. AddHatchedMonToParty writes it explicitly, with the decomp's own
 * comment: "A met level of 0 is interpreted on the summary screen as 'hatched at'"
 * (src/egg_hatch.c:384-386). Both egg-creation routines already write 0 as well
 * (src/daycare.c:836-843, 862-870) and all 17 eggs in Guy's saves carry it, so for a real
 * egg this is a no-op — it matters for a record that reached em_hatch some other way (an
 * egg PokeDNA itself made from gen3_build_mon, whose met level is the build level). Not
 * cosmetic: metLevel == 0 IS how "was an egg" is recorded, and PokeDNA's own
 * pk_pidiv_exempt_reason (gen3_pidiv.c:152) and Pk2Facts.is_hatched (gen3_legality2.c:409)
 * both key the hatched exemptions off exactly that byte. Without it PokeDNA's "hatched"
 * mon was not recognised as hatched by PokeDNA.
 *
 * Met LOCATION is deliberately left alone. Retail overwrites it with wherever the player
 * was standing when the egg hatched (GetCurrentRegionMapSectionId, src/egg_hatch.c:388-389)
 * and PokeDNA cannot know that; the egg's own location is a real place the record already
 * carried, so keeping it invents nothing.
 *
 * ALSO renames it to its species. A Gen-3 egg is not "unnamed": it carries the Japanese
 * nickname タマゴ (bytes 60 6F 8B FF) with language=1, on every egg in every one of Guy's
 * saves — and gen3_decode_char has no case for 60/6F/8B, so the viewer honestly rendered
 * three '?'. That is where "???" came from. Gen 3 has no "is nicknamed" bit: an unnicknamed
 * mon simply has its species name sitting in the nickname field, so hatching must write it,
 * and must move the language byte to English to match (the retail game renders a
 * language=Japanese mon's name through a different, 5-character path). */
void em_hatch(EditMon* e) {
  em_set_egg(e, false);
  uint16_t sp = rd16(e->sub[0] + 0);                  /* Growth substruct: species */
  if (sp >= 1 && sp <= G3_MAX_SPECIES) {              /* corrupt/bad egg: keep what it had */
    em_set_nickname(e, pk_species_name(sp));
    e->raw[0x12] = 2;                                 /* language: English, as gen3_build_mon */
  }
  em_set_friendship(e, 120);                          /* src/egg_hatch.c:350-351 */
  em_set_metlevel(e, 0);                              /* "hatched at", src/egg_hatch.c:384-386 */
  em_set_level(e, 5);
}

void em_set_ability(EditMon* e, uint8_t n) {
  uint32_t iv = rd32(e->sub[3] + 4);
  iv = (iv & ~(1u << 31)) | (((uint32_t)(n & 1)) << 31);
  wr32(e->sub[3] + 4, iv);
}

/* Misc substruct (sub[3]): [1] = metLocation; [2..3] = origins u16
 * (bits 0-6 metLevel, 7-10 originGame, 11-14 pokeBall, 15 otGender). */
void em_set_metloc(EditMon* e, uint8_t loc) { e->sub[3][1] = loc; }
void em_set_ball(EditMon* e, uint8_t ball) {
  uint16_t o = rd16(e->sub[3] + 2);
  wr16(e->sub[3] + 2, (uint16_t)((o & ~(0xFu << 11)) | (((uint16_t)ball & 0xF) << 11)));
}
void em_set_metlevel(EditMon* e, uint8_t lvl) {
  uint16_t o = rd16(e->sub[3] + 2);
  wr16(e->sub[3] + 2, (uint16_t)((o & ~0x7Fu) | (lvl & 0x7F)));
}
void em_set_metgame(EditMon* e, uint8_t game) {
  uint16_t o = rd16(e->sub[3] + 2);
  wr16(e->sub[3] + 2, (uint16_t)((o & ~(0xFu << 7)) | (((uint16_t)game & 0xF) << 7)));
}

void em_set_level(EditMon* e, uint8_t level) {
  if (level < 1) level = 1;
  if (level > 100) level = 100;
  uint16_t species = rd16(e->sub[0] + 0);
  wr32(e->sub[0] + 4, pk_exp_for_level(pk_species_growth(species), level));
  if (e->is_party) {
    e->raw[0x54] = level;
    recompute_party_stats(e);
  }
}

/* Switch a loaded record between box(80) and party(100) kinds. Converting a box
 * record TO party derives the plaintext level (from exp) + battle stats; commit
 * then writes 100 bytes. Converting party->box just drops the plaintext block
 * (commit writes 80). Used by the clipboard to paste across containers. */
void em_set_party_flag(EditMon* e, bool is_party) {
  e->is_party = is_party;
  if (is_party) {
    uint16_t species = rd16(e->sub[0] + 0);
    uint32_t exp = rd32(e->sub[0] + 4);
    e->raw[0x54] = pk_level_from_exp(pk_species_growth(species), exp);
    recompute_party_stats(e);
  }
}

/* Encode an ASCII/UTF-8 string into the Gen-3 charset.
 *
 * UTF-8 aware because two species names are NOT ASCII: the table stores NIDORAN♀ and
 * NIDORAN♂ as "NIDORAN" + U+2640 / U+2642 (three UTF-8 bytes each). Byte-wise
 * encoding sent each of those bytes through gen3_encode_char's `default: 0x00`, i.e.
 * three trailing SPACES instead of the single charset byte. Retail stores the symbol:
 * a NIDORAN♂ caught in FireRed reads C8 C3 BE C9 CC BB C8 B5 FF — "NIDORAN" then 0xB5.
 * (0xB5 = ♂, 0xB6 = ♀.) */
static void encode_name(uint8_t* dst, int cap, const char* s) {
  const unsigned char* p = (const unsigned char*)s;
  int i = 0;
  while (i < cap && *p) {
    uint8_t b;
    /* && short-circuits, so a string ending in a bare 0xE2 never reads past the NUL. */
    if (p[0] == 0xE2u && p[1] == 0x99u && (p[2] == 0x80u || p[2] == 0x82u)) {
      b = (p[2] == 0x80u) ? 0xB6u : 0xB5u;       /* U+2640 ♀ / U+2642 ♂ */
      p += 3;
    } else if (p[0] >= 0x80u) {                  /* any other non-ASCII -> space */
      b = 0x00u; p++;
      while ((*p & 0xC0u) == 0x80u) p++;         /* skip its continuation bytes */
    } else {
      b = gen3_encode_char((char)*p++);
    }
    dst[i++] = b;
  }
  for (; i < cap; i++) dst[i] = 0xFF;            /* terminator + padding */
}

void em_set_nickname(EditMon* e, const char* s) { encode_name(e->raw + 0x08, 10, s); }
void em_set_otname(EditMon* e, const char* s)   { encode_name(e->raw + 0x14, 7, s); }

void em_set_pid(EditMon* e, uint32_t pid) {
  e->personality = pid;
  wr32(e->raw + 0x00, pid);
  recompute_party_stats(e);                      /* nature may have changed -> stats shift */
}

uint32_t em_get_ivword(const EditMon* e) { return rd32(e->sub[3] + 4); }
void em_set_ivword(EditMon* e, uint32_t w) { wr32(e->sub[3] + 4, w); recompute_party_stats(e); }

bool em_reroll(EditMon* e, int want_nature, int want_shiny, int want_gender, uint8_t gender_ratio) {
  uint16_t tid = (uint16_t)(e->otId & 0xFFFF), sid = (uint16_t)(e->otId >> 16);

  if (want_shiny == 1) {
    /* CONSTRUCT shiny PIDs (bounded): for each low half, the high half is forced
     * so (tid^sid^lo^hi) < 8 — far faster than brute force. */
    for (uint32_t lo = 0; lo < 0x10000u; lo++) {
      for (int k = 0; k < 8; k++) {
        uint16_t hi = (uint16_t)(tid ^ sid ^ (uint16_t)lo ^ k);
        uint32_t pid = ((uint32_t)hi << 16) | lo;
        if (want_nature >= 0 && (int)(pid % 25) != want_nature) continue;
        if (want_gender >= 0 && pk_gender_from(pid, gender_ratio) != want_gender) continue;
        em_set_pid(e, pid);
        return true;
      }
    }
    return false;
  }

  /* non-shiny / don't-care: short capped sequential search (~a frame or two) */
  uint32_t pid = e->personality;
  for (uint32_t i = 0; i < 400000u; i++) {
    pid = pid * 1103515245u + 12345u;
    if (want_nature >= 0 && (int)(pid % 25) != want_nature) continue;
    if (want_shiny == 0) {
      int shiny = ((uint16_t)(tid ^ sid ^ (uint16_t)(pid & 0xFFFF) ^ (uint16_t)(pid >> 16)) < 8);
      if (shiny) continue;
    }
    if (want_gender >= 0 && pk_gender_from(pid, gender_ratio) != want_gender) continue;
    em_set_pid(e, pid);
    return true;
  }
  return false;
}

/* Set the Unown letter (0..27) by finding a PID that yields it while preserving
 * the current nature + shininess (Unown is genderless, so gender is fixed). */
bool em_set_unown_form(EditMon* e, int form) {
  if (form < 0 || form > 27) return false;
  uint16_t tid = (uint16_t)(e->otId & 0xFFFF), sid = (uint16_t)(e->otId >> 16);
  int cur_nat = (int)(e->personality % 25);
  int cur_shiny = ((uint16_t)(tid ^ sid ^ (uint16_t)(e->personality & 0xFFFF) ^ (uint16_t)(e->personality >> 16)) < 8);
  if (pk_unown_form(e->personality) == form) return true;
  uint32_t pid = e->personality;
  for (uint32_t i = 0; i < 4000000u; i++) {
    pid = pid * 1103515245u + 12345u;
    if (pk_unown_form(pid) != form) continue;
    if ((int)(pid % 25) != cur_nat) continue;
    int shiny = ((uint16_t)(tid ^ sid ^ (uint16_t)(pid & 0xFFFF) ^ (uint16_t)(pid >> 16)) < 8);
    if (shiny != cur_shiny) continue;
    em_set_pid(e, pid);
    return true;
  }
  return false;
}

void em_preview(const EditMon* e, PkMon* out) {
  uint8_t scratch[100];
  gen3_edit_commit(e, scratch);
  pk_decode_mon(scratch, e->is_party, out);
  /* BACKLOG #46: pk_decode_mon() always sets out->raw = the buffer it was handed --
   * here, `scratch`, whose frame is gone the instant this function returns. Every
   * caller only ever reads out's VALUE fields (nature, IVs, PP, ...), never raw, but
   * "never today" is not a contract; NULL is, and gen3_mon.h documents it that way
   * next to the field. */
  out->raw = NULL;
}

bool gen3_edit_roundtrip_ok(const uint8_t* rec, bool is_party) {
  EditMon e;
  uint8_t out[100];
  gen3_edit_load(rec, is_party, &e);
  gen3_edit_commit(&e, out);
  return memcmp(rec, out, is_party ? 100 : 80) == 0;
}
