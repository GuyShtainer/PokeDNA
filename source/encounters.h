#ifndef ENCOUNTERS_H
#define ENCOUNTERS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Wild-encounter tables — the data behind the Legality V2 "could this Pokemon have
 * been caught where it says it was caught?" checks (research-legality-v2.md E2/E8).
 *
 * source/encounters.c is GENERATED and git-ignored. `python3 tools/gen_encounters.py
 * --from-rom` locates gWildMonHeaders in each of the five retail cartridge dumps and
 * emits it. Reading the carts rather than a decomp is what makes this table shippable
 * (plain numbers measured from hardware the user owns; the pret JSONs are used only as
 * a cross-check inside the generator, never as the source) AND is the only way to get
 * Ruby/Sapphire data at all — no pokeruby wild_encounters data exists in this tree.
 *
 * ---- why the key is a MAPSEC, not a map -------------------------------------
 * The record stores a region-map SECTION as its met location, not a map id:
 * `value = GetCurrentRegionMapSectionId(); SetBoxMonData(..., MON_DATA_MET_LOCATION,
 * &value)` (pokeemerald src/pokemon.c:2257-2258), and that function just returns the
 * map header's regionMapSectionId (src/overworld.c:1391-1393). Several maps share one
 * MAPSEC (Granite Cave's three floors, Victory Road, every Route 119 chunk), so the
 * generator merges them: one row per (mapsec, species) with the widest level range and
 * the OR of the methods seen. Merging is the safe direction for a checker whose first
 * rule is never to call a legitimate Pokemon illegal.
 *
 * ---- Ruby and Sapphire are separate tables ---------------------------------
 * The origin byte already distinguishes them (1 = Sapphire, 2 = Ruby), so keeping them
 * apart buys a real version-exclusive check (Seedot line in Ruby vs Lotad line in
 * Sapphire) for ~2.3 KiB. OVERNIGHT-DECISIONS.md item 5.
 *
 * Pure C — no tonc, no GBA headers, no FatFs — so tests/host_encounters_test.c runs
 * this against the real ROMs on the PC. All tables are `const` (ROM, read in place);
 * this module adds zero EWRAM.
 */

/* Game ids are the record's own origin/metGame byte, so a caller passes
 * `m->metGame` straight through. Values from the decomps' constants/game_version.h. */
typedef enum {
  PK_ENC_SAPPHIRE  = 1,
  PK_ENC_RUBY      = 2,
  PK_ENC_EMERALD   = 3,
  PK_ENC_FIRERED   = 4,
  PK_ENC_LEAFGREEN = 5,
} PkEncGame;

/* Which encounter methods reach this species here (a bitmask; the four slot arrays of
 * struct WildPokemonHeader, in order). Kept because the ball/met checks care: a Rock
 * Smash or fishing-only species can never be met by a Safari-zone grass encounter. */
#define PK_ENC_LAND   0x01u   /* 12 grass/cave slots */
#define PK_ENC_WATER  0x02u   /*  5 surfing slots    */
#define PK_ENC_ROCK   0x04u   /*  5 rock-smash slots */
#define PK_ENC_FISH   0x08u   /* 10 fishing slots (old/good/super rod share the array) */

typedef struct {
  uint16_t species;   /* Gen-3 INTERNAL id, 1..411 — same numbering as PkMon.species */
  uint8_t  mapsec;    /* region-map section == the record's met location             */
  uint8_t  minlvl;    /* lowest min over every slot merged into this row             */
  uint8_t  maxlvl;    /* highest max over every slot merged into this row            */
  uint8_t  methods;   /* PK_ENC_* bitmask                                            */
} PkWildEntry;        /* 6 bytes, no padding */

/* Tri-state, because "no encounter table is linked in" and "this species is genuinely
 * not there" must never collapse into the same answer — the first must produce no
 * verdict at all, the second is what feeds a SUSPECT row. */
#define PK_WILD_NO_DATA (-1)
#define PK_WILD_NO      0
#define PK_WILD_YES     1

/* False when source/encounters.c was not generated. The UI should say so rather than
 * silently running met checks with an empty table. */
bool pk_wild_have_data(void);

/* Rows in one game's table (0 if that ROM was missing when the table was generated). */
int pk_wild_count(PkEncGame g);

/* Can `species` be encountered at `mapsec` in game `g`? On PK_WILD_YES, *out (if
 * non-NULL) gets the merged row — check `minlvl <= metLevel <= maxlvl` for E2. */
int pk_wild_at(PkEncGame g, uint8_t mapsec, uint16_t species, PkWildEntry* out);

/* The flat fast path: is `species` wild-obtainable ANYWHERE in game `g`? One bitmap
 * lookup, no search — this is what a box-wide sweep calls per mon. */
int pk_wild_anywhere(PkEncGame g, uint16_t species);

/* Bitmask of the games in which `species` is wild-obtainable, bit (g-1) per PkEncGame
 * (bit0 Sapphire ... bit4 LeafGreen).
 *
 * `*unknown` (may be NULL) receives the mask of games whose table is ABSENT. Callers
 * MUST NOT read a clear bit as "not obtainable here" while its unknown bit is set —
 * that is how a missing table manufactures a legality verdict, which the tri-state
 * exists to prevent. A caller that ignores `unknown` is asserting it only cares about
 * positive evidence. */
uint8_t pk_wild_games(uint16_t species, uint8_t* unknown);

/* Every row for one mapsec, as a contiguous slice (the table is sorted by
 * (mapsec, species)). Returns the row count; *out is set only when that is > 0.
 * For the "what lives here" UI list and for the host tests. */
int pk_wild_mapsec_list(PkEncGame g, uint8_t mapsec, const PkWildEntry** out);

#if defined(__GNUC__) && !defined(PK_ENCOUNTERS_IMPL)
/*
 * Weak no-data fallbacks, so a clone with no generated tables still links and behaves
 * sanely — the same posture as source/art_fallbacks.c for the art modules. The
 * generated encounters.c defines PK_ENCOUNTERS_IMPL before including this header, so
 * it never collides with its own strong definitions; every other translation unit
 * carries a weak copy that the linker discards the moment the real table exists.
 *
 * Every fallback answers "I do not know", never "no" — a missing table must not be
 * able to manufacture a legality complaint.
 */
__attribute__((weak)) bool pk_wild_have_data(void) { return false; }
__attribute__((weak)) int  pk_wild_count(PkEncGame g) { (void)g; return 0; }
__attribute__((weak)) int  pk_wild_at(PkEncGame g, uint8_t mapsec, uint16_t species,
                                      PkWildEntry* out) {
  (void)g; (void)mapsec; (void)species; (void)out; return PK_WILD_NO_DATA;
}
__attribute__((weak)) int  pk_wild_anywhere(PkEncGame g, uint16_t species) {
  (void)g; (void)species; return PK_WILD_NO_DATA;
}
__attribute__((weak)) uint8_t pk_wild_games(uint16_t species, uint8_t* unknown) {
  (void)species; if (unknown) *unknown = 0x1F;   /* no data at all: every game unknown */
  return 0;
}
__attribute__((weak)) int  pk_wild_mapsec_list(PkEncGame g, uint8_t mapsec,
                                               const PkWildEntry** out) {
  (void)g; (void)mapsec; (void)out; return 0;
}
#endif

#endif /* ENCOUNTERS_H */
