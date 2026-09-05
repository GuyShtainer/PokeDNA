#ifndef ROM_GBBASE_INCLUDED
#define ROM_GBBASE_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#include "rom_gbsprite.h"   /* RomGbSprite, GbRomGen, GbReadFn (via gb_sprite_codec.h) */
#include "gb_edit.h"        /* GbGen1Base, GB_HP..GB_SPC                                */

/*
 * Gen-1 BASE STATS, read live out of the user's own Game Boy cartridge dump
 * (Red/Blue/Yellow) -- the table gb_edit.h says does not exist anywhere in this
 * tree ("There is no Gen-1 base-stat table in this tree and this module will not
 * invent one. ... So the caller supplies this, or the edit is refused."). This
 * module is that supply: gen1_write.c's Gen1SpeciesInfo / gb_edit.h's GbGen1Base
 * both want {base[5] HP/Atk/Def/Spe/Spc, type1, type2 (+catch_rate, +growth)} for
 * a species, and the only place those five numbers live for a Gen-1 target is the
 * cartridge the mon is being converted onto -- Gen 1's Special base differs from
 * Gen 3's SpA base for 43 of 151 species (dex 4 Charmander: 50 here, 60 in Gen 3),
 * and Gen-1 type ids are a different numbering entirely (Fire is 0x14, not 10).
 *
 * PokeDNA ships NO Game Freak data. This is a transient read of a file the user
 * already owns, exactly the posture rom_gbsprite.c / rom_sprite.c take.
 *
 * PURE C: no tonc, no FatFs, no GBA headers, no statics. All I/O goes through the
 * caller's GbReadFn, so tests/host_rombase_test.c runs this exact code on the PC
 * against the real cartridge dumps.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DATA IS
 * ---------------------------------------------------------------------------
 * rom_gbsprite_open() has already located and validated BaseStats for this ROM
 * (RomGbSprite.base_stats: file offset; .mew_stats: file offset of the standalone
 * Mew row in Red/Blue, or 0 when Mew is an ordinary 151st row, as in Yellow --
 * see rom_gbsprite.h's header comment, "MEW IS NOT UNIFORM"). This module adds
 * nothing to that search; it only knows how to decode the 28-byte row once
 * rom_gbsprite has found it.
 *
 * BaseStats IS INDEXED BY NATIONAL DEX NUMBER, not by the internal species index
 * that the pic tables use (rom_gbsprite.h lines 51-54: home/pokemon.asm
 * GetMonHeader does `predef IndexToPokedex` before indexing BaseStats). So entry
 * `dex-1` (1-based dex) is read directly -- no PokedexOrder lookup needed here,
 * unlike rom_gbsprite.c's g1_pic() which reads a *picture* and therefore does
 * need the internal index for the bank ladder.
 *
 * ROW LAYOUT (28 bytes, pokered BASE_DATA_SIZE). Verified against the decomp's
 * own struct definition AND three real rows, byte for byte:
 *   assets/upstream/pokered/constants/pokemon_data_constants.asm (the
 *     "base data struct members" rsreset block, BASE_DEX_NO..BASE_DATA_SIZE)
 *   assets/upstream/pokered/data/pokemon/base_stats/bulbasaur.asm  (45/49/49/45/65
 *     Grass/Poison, catch 45, growth Medium Slow -- dex 1)
 *   assets/upstream/pokered/data/pokemon/base_stats/charmander.asm (39/52/43/65/50
 *     Fire/Fire, catch 45, growth Medium Slow -- dex 4; Spc=50 is the value that
 *     differs from Gen 3's 60, exactly the case gb_edit.h names)
 *   assets/upstream/pokered/data/pokemon/base_stats/mew.asm        (100 x5
 *     Psychic/Psychic, catch 45, growth Medium Slow -- dex 151, the standalone row)
 *
 *   +0      dex (self-check: must equal the dex requested)
 *   +1..5   HP, Atk, Def, Spd, Spc                     (BASE_STATS)
 *   +6,7    type1, type2                               (BASE_TYPES) -- raw Gen-1
 *           ids (constants/type_constants.asm: 0x00-0x08 physical, 0x14-0x1A
 *           special), already what gb_edit.h's GbGen1Base and gb_set_gen1_base()
 *           expect -- NOT remapped to Gen-3 numbering.
 *   +8      catch rate                                 (BASE_CATCH_RATE)
 *   +9      base exp                                   (unused here)
 *   +10     sprite dimension byte                       (unused here)
 *   +11,12  front pic pointer (bank-relative)            (unused here)
 *   +13,14  back pic pointer (bank-relative)             (unused here)
 *   +15..18 four level-1-learnset moves                  (unused here)
 *   +19     growth rate (GEN1_GROWTH_* / gen1_write.h's enum, 0..5)
 *   +20..26 seven TM/HM learnset bytes                   (unused here)
 *   +27     padding                                      (unused here)
 * Total 28 bytes == pokered's BASE_DATA_SIZE, and the same 28-byte stride
 * rom_gbsprite.c's own g1_row() reads (source/rom_gbsprite.c, the Gen-1 g1_row/
 * g1_bank/g1_pic block) -- this module mirrors that read, not a new locator.
 *
 * MEW. dex 151 on a ROM where mew_stats != 0 (Red/Blue) is read from mew_stats
 * instead of base_stats + 150*28, the same substitution g1_row() makes. On
 * Yellow (mew_stats == 0) row 151 of the ordinary table already holds Mew.
 *
 * GEN 2 IS DELIBERATELY NOT HERE. gb_edit.h says so directly: "Gen 2 needs none
 * of it: its record has no types and no catch rate, and its base stats are
 * IDENTICAL to Gen 3's for all 251 species ..., so pk_base_stats() serves." A
 * second, unverified 32-byte-row decoder for a table this module has no caller
 * for would be exactly the kind of code Golden Rule 4 warns against carrying
 * "for completeness" -- so it is skipped, not merely deferred.
 */

/* One species' worth of what a Gen-1 target needs, read straight off the
 * cartridge. `base`/`type1`/`type2` slot directly into gb_set_gen1_base() and
 * gen1_write.h's Gen1SpeciesInfo (base[GB_NSTATS] is the same HP/Atk/Def/Spe/Spc
 * order both structs use). `catch_rate` and `growth` are carried alongside
 * because a caller building a Gen1SpeciesInfo or calling g1e_set_species /
 * g1e_set_level needs them too, and they come from the exact same 28-byte row --
 * splitting the read in two would just be two chances to disagree. */
typedef struct {
  GbGen1Base base;      /* base[GB_HP..GB_SPC], type1, type2 -- Gen-1 raw ids */
  uint8_t    catch_rate;
  uint8_t    growth;     /* GEN1_GROWTH_* (gen1_write.h), 0..5                */
} RomGb1Species;

/*
 * Read national dex `dex` (1..151) out of a Gen-1 ROM whose tables rom_gbsprite
 * has already located (`gs->ok` and `gs->gen == GB_ROM_GEN1`). Mew lives outside
 * the table in Red/Blue (`gs->mew_stats != 0`); this substitutes that offset for
 * dex 151 automatically, exactly as rom_gbsprite.c's own picture reader does.
 *
 * `read`/`ctx` are the caller's GbReadFn pair -- this module takes them
 * explicitly, the same way module A's gb_sprite_gen1()/gb_sprite_gen2() do,
 * rather than reaching into `gs->read`/`gs->ctx`, so it never assumes those
 * fields survived whatever the caller did with `gs` between open() and here.
 *
 * Returns false, with `*out` UNTOUCHED, for: a NULL argument; a ROM that is not
 * an opened Gen-1 ROM; a dex outside 1..151; a read failure; or a row whose own
 * dex byte does not equal `dex` (the self-check g1_row() also performs -- catches
 * a wrong offset or a corrupted/foreign ROM before a wrong Pokemon's stats are
 * ever handed back). Returns true otherwise. */
bool rom_gbbase_gen1(const RomGbSprite* gs, GbReadFn read, void* ctx, uint16_t dex,
                     RomGb1Species* out);

#endif /* ROM_GBBASE_INCLUDED */
