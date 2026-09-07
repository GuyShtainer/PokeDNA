#ifndef GB_NEW_MON_H
#define GB_NEW_MON_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_editor.h"     /* GbEditMon, GB_NAME_BYTES -- gb_edit.h transitively */
#include "rom_gbbase.h"    /* RomGb1Species -- gb_new_mon_g1_moves()'s own input  */
#include "rom_gblearn.h"   /* RomGbLearn    -- gb_new_mon_g1_moves()'s own input  */

/*
 * BACKLOG #50: create a legal Gen-1/2 Pokemon from scratch. Pure C (no tonc, no
 * FatFs, no GBA headers) so tests/host_newmon_test.c dual-compiles, same posture
 * as gb_edit.c/gb_editor.c. gb_new_mon() itself does NO ROM I/O -- it builds a
 * record purely from the facts `src` already carries, exactly like
 * gen3_gen.c's gen3_build_mon() does for a Gen-3 mon. Assembling those facts
 * (opening the ROM, calling rom_gbbase_gen1/2 + rom_gblearn_moves_at) is the
 * CALLER's job -- source/pdna_gen12.c's CREATE flow (BACKLOG #50's UI half).
 */

/* ROM-derived facts gb_new_mon() needs, plus the save's own trainer. Nothing
 * here is optional padding: every field maps to exactly one requirement below.
 *
 *   base/type1/type2   Gen 1 ONLY (gb_set_gen1_base's own contract; Gen 2's
 *                       base stats/types are not stored in its record at all,
 *                       gb_edit.h's GbGen1Base comment) -- from rom_gbbase_gen1.
 *   growth              BOTH gens: cross-checked against this tree's own
 *                       gb_growth_rate(dex) before anything is built (see
 *                       gb_new_mon's own doc) -- from rom_gbbase_gen1/gen2.
 *   moves[4]             the level-L moveset, ALREADY MERGED for Gen 1 (see
 *                       gb_new_mon_g1_moves below); Gen 2 needs no merge, use
 *                       rom_gblearn_moves_at()'s own output directly. 0 = empty
 *                       slot, LEFT-PACKED (a 0 stops the scan; no 0-then-real
 *                       gaps -- true of everything rom_gblearn/rom_gbbase_gen1
 *                       hand back, so a caller composing this from them for
 *                       real never has to pack it itself).
 *   species_name        the species' name, as the app's own tables spell it
 *                       (pk_species_name -- already uppercase); gb_new_mon()
 *                       uppercases defensively anyway, so a differently-cased
 *                       caller still gets the right nickname.
 *   ot_name              the save's own trainer name, ALREADY DECODED text
 *                       (Gb12Mount.player -- gb_info_page's own field,
 *                       source/pdna_gen12.c; that decoder is the LOSSY one,
 *                       gb_edit.h's NAMES section documents why, so gb_new_mon
 *                       re-encodes it with the LOSSY setter too rather than
 *                       refuse a mon over a trainer name typed on a different
 *                       cartridge). NULL/empty is fine -- leaves the field the
 *                       0x50-filled blank a from-scratch record already has.
 *   ot_id                the save's own trainer id (Gb12Mount.tid).
 */
typedef struct {
  uint8_t     base[GB_NSTATS];
  uint8_t     type1, type2;
  uint8_t     growth;
  uint8_t     moves[4];
  const char* species_name;
  const char* ot_name;
  uint16_t    ot_id;
} GbNewMonSrc;

/* Gen-1 only: merge `base`'s start[] (the level-1 starters, rom_gbbase.h) with
 * `rl`'s table walk into the actual four moves a level-`level` Gen-1 mon of
 * `dex` would know -- a thin wrapper over rom_gblearn_moves_at_seeded()
 * (rom_gblearn.h), seeding its dedup-aware 4-slot FIFO with the starters so a
 * table entry that relists one of them (Nidoqueen, Nidoking and Kabutops all
 * do, in Guy's own Red.gb) is skipped rather than duplicated -- exactly the
 * same "already known" rule the table's own within-table walk already applies
 * to itself. Gen 2 needs no such merge -- its own table already includes the
 * starters (rom_gblearn.h), so a Gen-2 caller uses rom_gblearn_moves_at()
 * directly and never calls this.
 *
 * Returns the count filled in `out4` (0..4, left-packed), or -1 for a NULL
 * `base`/`rl`/`out4`, an `rl` that is not `.ok` or not Gen 1, or a `dex`/
 * `level` rom_gblearn_moves_at_seeded() itself would refuse. */
int gb_new_mon_g1_moves(const RomGb1Species* base, RomGbLearn* rl, uint16_t dex,
                        uint8_t level, uint8_t out4[4]);

/* Build a fresh, legal Pokemon: species `dex` at `level`, in `gen`'s format,
 * from ROM-sourced facts (`src`) and the save's own trainer. `out` is always
 * built as a PARTY record (`out->is_party = true`, stats already computed via
 * gbe_settle_stats) -- inserting it into a STORAGE box instead is the
 * caller's own conversion, same as any other party<->box move this tree does
 * elsewhere. DVs come from `seed`: a small, explicit, DOCUMENTED LCG (NOT
 * cryptographic -- this only needs "looks random enough and is reproducible
 * for a test", never unpredictability), advanced once per stat in Atk, Def,
 * Spe, Spc order, each draw taking the LCG's high nibble so consecutive draws
 * do not correlate the way its low bits would. The HP DV, gender, shininess
 * and (for an Unown) letter all fall out of those four exactly like they do
 * for any other record (gb_dv_effects) -- nothing here targets a particular
 * one, a fresh mon is exactly as likely to be shiny as the games make it.
 *
 * Returns false, `*out` UNSPECIFIED, for: a NULL `src`/`out`; a `gen` that is
 * not GB_GEN1/GB_GEN2; a `level` outside 1..100; a `dex` this `gen` does not
 * have (gb_index_from_dex refuses); or `src->growth` DISAGREEING with this
 * tree's own gb_growth_rate(dex) -- the cross-check rom_gbbase_gen1/2's ROM
 * read exists for (rom_gbbase.h): if the generated table and the live
 * cartridge disagree, this refuses rather than build a record on a growth
 * curve nobody here can actually name. `src->type1`/`type2` (Gen 1) are
 * stored as given, unvalidated -- rom_gbbase_gen1 always hands back real Gen-1
 * type ids read off the cartridge, so this is never reached in practice; a
 * caller that assembles `src` some other way and gets it wrong will see it as
 * gb_check()'s own `gen1_type_bad` issue on the built record, not a refusal
 * here.
 *
 * Fields NOT sourced from `src`/`seed`, because a brand-new capture never has
 * them: stat exp 0, PP-Ups 0, held item 0, caught-data 0 and not-an-egg
 * (Gen 2), current HP = full (gbe_settle_stats's own carry rule, a fresh max
 * counts as "was already full"). Gen 2 friendship starts at 70, the games'
 * own default (gen3_build_mon's Gen-3 twin agrees). PP is each move's base PP
 * -- gb_set_move() already sets it from gb_move_base_pp(), the SAME per-
 * generation table gb_edit.h documents (identical to Gen 3's pk_move_pp for
 * every move except Struggle, which this tree's table already knows about;
 * the retail gate, item 4, is what actually proves it in the real games).
 * Nickname is `src->species_name` UPPERCASED and encoded with this
 * generation's own GB charset (gb_set_nickname) -- what a fresh capture shows
 * before the player renames it; a species name this tree's own tables produce
 * is always representable, so this step cannot fail. */
bool gb_new_mon(uint8_t gen, uint16_t dex, uint8_t level, const GbNewMonSrc* src,
               uint32_t seed, GbEditMon* out);

#endif /* GB_NEW_MON_H */
