#ifndef GEN3_CONTEST_H
#define GEN3_CONTEST_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/* Contests: per-mon ribbon ranks (Cool/Beauty/Cute/Smart/Tough) and the SaveBlock1
 * contest-winner records (the Contest Hall's recent winners + the Lilycove Art
 * Museum's 5 paintings). Pure C, host-testable — BACKLOG #60.
 *
 * All offsets below were cross-derived TWICE, independently: once from this repo's
 * assets/upstream/{pokeemerald,pokeruby}/include/{pokemon,global}.h (reference only,
 * clean-room — no code or data copied, only the byte layout the struct comments and
 * bitfields describe), and once from this codebase's OWN gen3_stars.c (the trainer-
 * card star achievement, shipped and already exercising the very same bytes for its
 * "5 museum paintings" star). Both agree; gen3_stars.c's museum_off()/museum_fill()
 * are cited inline below because they are the closest thing this repo has to an
 * existing regression fixture for this layout.
 *
 * ---- per-mon ribbon rank (Misc substruct +0x08, 32-bit LE "ribbons" word) --------
 * pokeemerald include/pokemon.h:150-154 (struct PokemonSubstruct3 bitfields, LSB
 * first): coolRibbon:3 @ bit 0, beautyRibbon:3 @ bit 3, cuteRibbon:3 @ bit 6,
 * smartRibbon:3 @ bit 9, toughRibbon:3 @ bit 12. Each field stores the HIGHEST rank
 * won so far (0 none, 1 Normal, 2 Super, 3 Hyper, 4 Master) — cumulative by
 * construction, not by a separate bitmask: pokeemerald src/contest_util.c
 * GiveMonContestRibbon only increments the field when `ribbonData <= contestRank`,
 * so a Master win (field becomes 4) can only happen after Hyper (field was already
 * 3) — there is no way to hold Master without every rank below it, and no extra
 * bookkeeping this code needs to do to keep that true: a rank picker that just
 * writes the target field value IS the cumulative award.
 *
 * ---- named ribbons, same word, bits 15..26 (pokemon.h:155-166) -------------------
 * championRibbon:1@15, winningRibbon:1@16, victoryRibbon:1@17, artistRibbon:1@18,
 * effortRibbon:1@19, marineRibbon:1@20, landRibbon:1@21, skyRibbon:1@22,
 * countryRibbon:1@23, nationalRibbon:1@24, earthRibbon:1@25, worldRibbon:1@26.
 * Bits 27-30 (unusedRibbons) and bit 31 (modernFatefulEncounter, owned by
 * pdna_origin_art.h's RIBBON_FATEFUL) are never touched by this file.
 *
 * ---- struct ContestWinner (pokeemerald/pokeruby include/global.h) ----------------
 * personality u32@0, otId(trainerId) u32@4, species u16@8, contestCategory u8@10,
 * monName[11]@11 (Gen-3 encoded, 0xFF-padded, no NUL), trainerName[8]@22. Emerald
 * adds contestRank u8@30 (RS has no such field — its museum entries are always an
 * implicit Master win, per pokeemerald's own ShouldReadyContestArtist gate: master
 * rank + >=800 points is what offers a museum portrait in the first place). Struct
 * alignment (a u32 member) rounds every record up to a 32-byte stride in both games
 * — confirmed by the next field's own documented SaveBlock1 offset in each decomp
 * (Emerald daycare @ 0x3030 - contestWinners @ 0x2E90 = 0x1A0 = 13*0x20; Ruby
 * daycare @ 0x2F9C - museumPortraits @ 0x2EFC = 0xA0 = 5*0x20).
 *
 * ---- per-game layout ---------------------------------------------------------
 * Emerald: ONE array, contestWinners[13] @ SB1 0x2E90, stride 0x20.
 *   idx 0..5  = CONTEST_WINNER_HALL_1..6 (6 Contest Hall winners)
 *   idx 6..7  = HALL_UNUSED_1/2 (dead slots, the Hall only shows 6 paintings)
 *   idx 8..12 = CONTEST_WINNER_MUSEUM_COOL..TOUGH (== gen3_stars.c's museum_off()
 *               0x2F90 = 0x2E90 + 8*0x20)
 * Ruby/Sapphire: TWO arrays (pokeruby include/global.h:741-742).
 *   contestWinners[8] @ SB1 0x2DFC (8 Contest Hall winners, no unused slots)
 *   museumPortraits[5] @ SB1 0x2EFC (== gen3_stars.c's museum_off() for PK_RS)
 * FireRed/LeafGreen: no Contests at all (gc_supported() false; every other
 * function here either no-ops or returns a documented empty/failure value). */

enum { GC_COOL = 0, GC_BEAUTY, GC_CUTE, GC_SMART, GC_TOUGH, GC_CATEGORY_COUNT };

enum { GC_RANK_NONE = 0, GC_RANK_NORMAL, GC_RANK_SUPER, GC_RANK_HYPER, GC_RANK_MASTER };

/* ---- per-mon ribbon word helpers (operate on the raw 32-bit Misc-substruct word;
 * gen3_edit.c wraps these around EditMon.sub[3] for the summary editor). ---------- */
uint8_t  gc_ribbon_get(uint32_t ribbons, int category);               /* 0..4, clamped */
uint32_t gc_ribbon_set(uint32_t ribbons, int category, uint8_t rank);  /* rank clamped 0..4 */

enum {
  GC_RFLAG_CHAMPION = 15, GC_RFLAG_WINNING, GC_RFLAG_VICTORY, GC_RFLAG_ARTIST,
  GC_RFLAG_EFFORT, GC_RFLAG_MARINE, GC_RFLAG_LAND, GC_RFLAG_SKY, GC_RFLAG_COUNTRY,
  GC_RFLAG_NATIONAL, GC_RFLAG_EARTH, GC_RFLAG_WORLD
};
bool     gc_ribbon_flag_get(uint32_t ribbons, int flagbit);
uint32_t gc_ribbon_flag_set(uint32_t ribbons, int flagbit, bool on);

/* ---- SaveBlock1 contest-winner records ------------------------------------------ */
typedef struct {
  uint32_t personality;
  uint32_t otId;
  uint16_t species;      /* 0 = empty slot                                    */
  uint8_t  category;     /* GC_COOL..GC_TOUGH                                  */
  char     monName[11];  /* decoded ASCII, NUL-terminated, <=10 chars          */
  char     trainerName[8];
  uint8_t  rank;         /* CONTEST_RANK_*; always GC_RANK_MASTER on RS (no field) */
} GcWinner;

bool gc_supported(PkGame g);          /* false for FRLG only                        */
int  gc_hall_count(PkGame g);         /* 6 (Emerald), 8 (RS), 0 (FRLG)               */
#define GC_MUSEUM_COUNT 5             /* Cool/Beauty/Cute/Smart/Tough, every RSE game */

/* idx 0..gc_hall_count(g)-1. Returns false on an out-of-range idx or FRLG. */
bool gc_hall_get(const uint8_t* sb1, PkGame g, int idx, GcWinner* out);

/* cat = GC_COOL..GC_TOUGH. Returns false on FRLG; species==0 in *out means the
 * painting slot is empty (no win yet). */
bool gc_museum_get(const uint8_t* sb1, PkGame g, int cat, GcWinner* out);

/* Write a museum painting: species/personality/otId/names from the chosen donor,
 * category fixed to the slot's own `cat`, rank forced to GC_RANK_MASTER (Emerald
 * only — matches ShouldReadyContestArtist: only a Master win with 800+ points ever
 * reaches the museum). Names are given as ASCII (<=10 / <=7 chars) and encoded here.
 * A byte-identical write (donor already occupies this slot) is a true no-op — the
 * caller compares before/after and skips the commit, same as every other editor in
 * this codebase. Returns false on FRLG or a bad `cat`. */
bool gc_museum_set(uint8_t* sb1, PkGame g, int cat, uint16_t species, uint32_t personality,
                   uint32_t otId, const char* monName_ascii, const char* trainerName_ascii);

/* SB1 byte offset + length (always GC_STRIDE, 32) of one museum slot's record — so a
 * caller can snapshot just that record before a gc_museum_set() call and memcmp after,
 * instead of copying the whole 15,872-byte SaveBlock1 (which would not fit the 32 KiB
 * IWRAM stack anyway). Returns 0 on FRLG or a bad `cat` (never a valid record there). */
uint32_t gc_museum_offset(PkGame g, int cat);
#define GC_RECORD_BYTES 32

#endif /* GEN3_CONTEST_H */
