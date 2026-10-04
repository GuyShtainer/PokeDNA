#ifndef ROM_GBLEARN_INCLUDED
#define ROM_GBLEARN_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#include "gb_sprite_codec.h"   /* GbReadFn */
#include "gb_edit.h"           /* GB_GEN1 / GB_GEN2, gb_index_from_dex, gb_max_move */
#include "dc_learn.h"          /* DcLearn: Day-Care learn events */

/*
 * Gen-1/2 LEVEL-UP LEARNSETS, read live out of the user's own Game Boy cartridge
 * dump -- the piece BACKLOG #50 (create a mon from scratch) needs and this tree
 * did not have: a legal moveset for a species at a level, sourced from the game
 * that actually defines it, not guessed or hand-typed. Same posture as
 * rom_gbbase.c / rom_gbsprite.c: PokeDNA ships NO Game Freak data, this is a
 * transient read of a file the user already owns.
 *
 * PURE C: no tonc, no FatFs, no GBA headers, no statics -- all I/O goes through
 * the caller's GbReadFn, so tests/host_romgblearn_test.c runs this exact code on
 * the PC against Red.gb / Yellow.gb / Gold.gbc / Crystal.gbc.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DATA IS, AND HOW THIS MODULE FINDS IT (clean-room: pokered/
 * pokecrystal read on the web for the FORMAT below, never for an address -- see
 * docs/kb/licensing.md and CLAUDE.md's clean-room rule. Every offset this module
 * actually uses comes from locating the shape in the ROM at runtime.)
 * ---------------------------------------------------------------------------
 * GEN 1: pokered's EvosMovesPointerTable, data/pokemon/evos_moves.asm --
 * 190 bank-relative `dw` pointers (one per INTERNAL species index 1..190, the
 * same numbering rom_gbsprite.h's PokedexOrder and gb_edit.h's gb_index_from_dex
 * use -- 151 real species plus 39 MissingNo slots), table and every pointed-to
 * blob living together in ONE bank (the table's own). Each blob is:
 *   evolutions: repeating (method, ...params, target-species), 0-terminated.
 *     method 1 EVOLVE_LEVEL  = 3 B (method, level, species)
 *     method 2 EVOLVE_ITEM   = 4 B (method, item, min level, species)
 *     method 3 EVOLVE_TRADE  = 3 B (method, min level, species)
 *   moves: repeating (level, move) in increasing level order, 0-terminated.
 * Gen 1's LEVEL-1 starting moves are NOT here -- they are the four bytes at
 * BaseStats+15..18 (rom_gbbase.h's RomGb1Species.start, added alongside this
 * module) -- so this table alone under-reports a low-level Gen-1 moveset by
 * design; a caller building a full one merges both (source/gb_new_mon.c does).
 *
 * GEN 2: pokecrystal's EvosAttacksPointers, data/pokemon/evos_attacks_pointers.
 * asm + evos_attacks.asm -- 251 pointers, ONE PER DEX NUMBER (Gen 2 never
 * reordered species internally, so index == national dex, same fact
 * gb_dv_effects_of/gbe_has_gender_row already lean on), same table+data-one-bank
 * layout. Evolutions gain two more methods over Gen 1's three:
 *     method 1 EVOLVE_LEVEL     = 3 B (method, level, species)
 *     method 2 EVOLVE_ITEM      = 3 B (method, item, species)            <-
 *     method 3 EVOLVE_TRADE     = 3 B (method, held item or -1, species)
 *     method 4 EVOLVE_HAPPINESS = 3 B (method, time-of-day const, species)
 *     method 5 EVOLVE_STAT      = 4 B (method, level, Atk-vs-Def const, species)
 * (Gen 2's EVOLVE_ITEM is 3 B, ONE LESS than Gen 1's -- verified against the
 * decomp source, not assumed from Gen 1's shape.) Gen 2's moves list DOES
 * include the level-1 starters (pokecrystal's own BulbasaurEvosAttacks opens
 * "db 1, TACKLE" / "db 4, GROWL"), so nothing needs merging for Gen 2.
 *
 * LOCATING THE TABLE. Neither table's own address is pinned: this module scans
 * the whole ROM for a run of N (190 or 251) consecutive bank-relative pointers
 * (each a plain 2-byte LE address in [0x4000,0x8000), the ROMX window) as a
 * cheap prefilter, then for each survivor tries the table's OWN bank as the
 * shared data bank first (the layout every one of Guy's four dumps actually
 * uses) and, only if that fails to fully decode, brute-forces the rest of the
 * ROM's banks. "Fully decode" means all N entries parse structurally (evolution
 * methods in range, moves 0-terminated with a legal level 1..100 and a legal
 * move id 1..gb_max_move(gen)) AND at least half of them carry a real move --
 * the second bar is what tells a genuine table apart from a coincidental run of
 * addresses over blank/padding ROM space, where every "entry" trivially
 * decodes as "0 evolutions, 0 moves" (measured: this DOES happen, an early
 * version of this locator without that bar found exactly such a false table in
 * Red.gb). Fails, cleanly, unless EXACTLY ONE (offset, bank) pair clears every
 * bar -- the same "unique hit or refuse" doctrine rom_gbsprite.c's locate()
 * uses for every one of its own six tables.
 *
 * SANITY-CHECK ADDRESSES (recorded for a human to eyeball; NEVER read as inputs
 * -- the code above always relocates by shape): on Guy's four dumps, the table
 * starts and the shared data bank both located were Red.gb 0x03B05C bank 0x0E,
 * Yellow.gb 0x03B1E5 bank 0x0E, Gold.gbc 0x0427BD bank 0x10, Crystal.gbc
 * 0x0425B1 bank 0x10 -- and entry 0 (Bulbasaur, both generations) decoded to
 * evolve-at-16-into-Ivysaur then the exact move/level pairs pokecrystal's own
 * BulbasaurEvosAttacks source lists, cross-checked against tests/
 * host_romgblearn_test.c's own independent fixture.
 */

typedef struct {
  uint8_t  gen;          /* GB_GEN1 / GB_GEN2 */
  bool     ok;
  GbReadFn read;
  void*    ctx;
  uint32_t size;
  uint8_t  banks;         /* size / 0x4000, capped to fit a byte like rom_gbsprite.h's */
  uint32_t table_off;     /* file offset of the N-pointer table                        */
  uint8_t  data_bank;     /* the bank every pointer in it resolves against              */
} RomGbLearn;

/* Locate and verify `gen`'s learnset table in the ROM `read`/`ctx` exposes
 * (`size` bytes). Returns 1 and an opened `*rl` on a unique, fully-decodable
 * hit; 0 (with `*rl` zeroed, `.ok` false) for a bad argument, a `size` that is
 * not a plausible Game Boy ROM (not a positive multiple of 0x4000, or more than
 * 255 banks), or a ROM that does not contain exactly one such table for `gen`
 * -- including, cleanly, a Gen-1 ROM asked for as Gen 2 or the reverse: the
 * wrong entry count and evolution-method shape simply never produces a unique
 * decodable hit. */
int rom_gblearn_open(RomGbLearn* rl, uint8_t gen, GbReadFn read, void* ctx, uint32_t size);

/* The (up to) four level-up moves a mon of `dex` would know at `level`,
 * straight out of the table `rl` located -- the SAME rule the games use to cap
 * a moveset at four: walk the learnset in increasing level order, keep only
 * entries with level <= `level`, and once a fifth would-be slot arrives drop
 * the OLDEST kept one. A move ALREADY among the currently-kept four is not
 * re-added (the real games skip re-teaching a move already known rather than
 * duplicate a slot) -- verified against real data: Metapod and Kakuna both
 * relist Harden, Smeargle relists Sketch four times, in both Gold.gbc and
 * Crystal.gbc. `out4` is filled in LEARN ORDER (oldest of the four first, most
 * recent last -- move-slot 1..4 order on a real cartridge); an unused
 * trailing slot is 0, which is also gb_edit.h's own "empty slot" value, so a
 * caller never needs a separate count.
 *
 * Returns the number of slots filled (0..4) on success -- 0 is a legitimate
 * answer (a species with no table entry at or under `level`; Gen 1's starting
 * moves live elsewhere, see this file's own header) -- or -1 for a bad
 * argument: a NULL `rl`/`out4`, an `rl` that is not `.ok`, or a `dex` outside
 * 1..gb_max_species(rl->gen). Does not look at Gen-1's base-stats starting
 * moves at all; see gb_new_mon_g1_moves (source/gb_new_mon.h) for the merge --
 * which is also why THAT merge cannot just concatenate this function's output
 * with the starters: the same "already known" rule has to apply ACROSS the
 * two sources too (a species whose table relists a starting move at a higher
 * level would otherwise come back with that move twice), which is exactly
 * what rom_gblearn_moves_at_seeded() below is for. */
int rom_gblearn_moves_at(RomGbLearn* rl, uint16_t dex, uint8_t level, uint8_t out4[4]);

/* Same contract as rom_gblearn_moves_at(), except the 4-slot FIFO starts
 * PRE-LOADED with `seed4` (0 = unused slot, left-packed, same convention as
 * `out4`) as if those moves were already known before the table's own walk
 * begins -- so a table entry that matches one of them is skipped exactly like
 * a within-table repeat is. `rom_gblearn_moves_at(rl, dex, level, out4)` is
 * this function with `seed4 = NULL`. gb_new_mon_g1_moves() (source/
 * gb_new_mon.h) is the one real caller, seeding with Gen 1's base-stats
 * starters; nothing else in this tree needs a seed. */
int rom_gblearn_moves_at_seeded(RomGbLearn* rl, uint16_t dex, uint8_t level,
                                const uint8_t seed4[4], uint8_t out4[4]);

/* Day-Care growth (BACKLOG #373): the FIFO of rom_gblearn_moves_at_seeded(), seeded with
 * the mon's CURRENT four moves (`cur4`, left-packed, 0 = unused), but taking ONLY entries
 * with `lv_prev < lvl <= lv_new` (retail FillMoves' wPrevPartyLevel rule: what the mon
 * already stood above was taught then). Writes the resulting four to `out4` and one
 * DcLearn per move that actually changed them to `ev[0..7]` (`*n_ev` counts all, past 8;
 * `*overflow` latches then). Returns the number of moves held (0..4) or -1. */
int rom_gblearn_moves_between(RomGbLearn* rl, uint16_t dex, uint8_t lv_prev, uint8_t lv_new,
                              const uint8_t cur4[4], uint8_t out4[4],
                              DcLearn ev[DC_LEARN_MAX], int* n_ev, bool* overflow);

/* The lowest level at which `dex` can legally exist from nothing -- Gen 3's
 * own create flow answer (gen3_edit.c's gen3_build_level, "5 for a Bulbasaur,
 * 36 for a Charizard") for THIS generation's own evolution data, read live
 * off the same ROM this whole file already locates a table in (BACKLOG #50
 * UX-parity, Guy 2026-09-07: "it must evolve to there" applies just as much
 * here as it does to a Gen-3 create).
 *
 * ALGORITHM. Walk backward from `dex`: find the ONE species (if any) whose
 * evolution list targets it -- a linear scan of the whole table, since
 * neither generation's format offers a reverse index -- and recurse onto
 * that predecessor. A LEVEL edge (method 1 in both gens, or Gen 2's method 5
 * EVOLVE_STAT, Tyrogue's fixed level-20 Atk-vs-Def split, which carries an
 * explicit level requirement the same shape a plain level-up does) folds its
 * own level requirement into a running MAX; every other method (ITEM/TRADE/
 * Gen 2's HAPPINESS) carries no real level floor of its own and simply
 * PROPAGATES the predecessor's floor unchanged, even where the row format
 * still reserves a level-shaped byte (Gen 1's own ITEM entry keeps one --
 * verified unused by the item-evolution routine itself, not merely assumed).
 * A species nothing evolves into is a base form: floor 5, the same base
 * gen3_build_level uses (a Gen-2 hatch level; Gen 1 has no eggs, but 5 still
 * mirrors the Gen-3 answer for a base form, which is the parity this exists
 * for) -- and taking the max over the WHOLE backward walk (not just the
 * nearest level edge) is deliberately more permissive of a hand-crafted/
 * corrupted ROM than a strict nearest-edge reading would be, while being
 * IDENTICAL for every real chain, since levels only ever increase going
 * forward up a real evolution line.
 *
 * VERIFIED (clean-room: pokered's data/pokemon/evos_moves.asm and
 * pokecrystal's data/pokemon/evos_attacks.asm read on the web for these
 * FACTS, never for an address -- every byte this function actually reads
 * still comes from the live ROM `rl` already located) against real game
 * data, both gens where the species exists in both: Bulbasaur 5 (base),
 * Ivysaur 16, Venusaur 32 (Ivysaur evolves at 16, Venusaur at 32), Charizard
 * 36 (Charmeleon 16, Charizard 36), Raichu 5 (Pikachu's own predecessor --
 * Gen 1 none, Gen 2 Pichu by HAPPINESS -- propagates; Pikachu's OWN evolution
 * into Raichu is an ITEM, Thunder Stone, which also only propagates), Golem
 * 25 (Geodude->Graveler LEVEL 25, Graveler->Golem TRADE propagates), Crobat
 * 22 (Gen 2 only: Zubat->Golbat LEVEL 22, Golbat->Crobat HAPPINESS
 * propagates) -- tests/host_romgblearn_test.c pins all seven.
 *
 * Returns a legal level 1..100, defaulting to 5 whenever `rl` is not `.ok`,
 * `dex` is outside 1..gb_max_species(rl->gen), or a read fails mid-scan
 * (fails open -- see gen3_edit.c's own G3_BUILD_BASE_LVL comment for why a
 * missing/unreadable answer must never invent a HIGHER level than the
 * default, only ever the safe base). Never refuses: unlike
 * rom_gblearn_moves_at, there is no "bad argument" sentinel, because a
 * created mon's level always needs SOME legal answer to reach gb_new_mon(). */
uint8_t rom_gblearn_min_level(RomGbLearn* rl, uint16_t dex);

#endif /* ROM_GBLEARN_INCLUDED */
