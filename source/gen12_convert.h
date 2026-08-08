#ifndef GEN12_CONVERT_H
#define GEN12_CONVERT_H

#include <stdint.h>
#include <stdbool.h>

/* Gen 1/2 -> Gen 3 record conversion (pure C, host-testable).
 *
 * THERE WAS NEVER AN OFFICIAL GEN 1/2 -> GEN 3 TRANSFER. The Time Capsule is
 * Gen 1 <-> Gen 2 only; Gen 3 broke compatibility outright (new hardware link, a
 * personality value the old games never stored). The only official forward path out
 * of R/B/Y/G/S/C is 3DS Virtual Console -> Poke Transporter -> Pokemon Bank, which
 * *converts* the Pokemon and skips Gen 3 entirely.
 *
 * So this is a conversion PokeDNA INVENTS, modelled on Poke Transporter's documented
 * rules. The output is NOT legitimate in Gen 3's own terms and any legality checker
 * may flag it. It therefore self-identifies rather than disguising itself:
 *
 *     SID == 0   +   metLocation == 0xFE ("TRADE")   +   Poke Ball   +   metGame = the
 *     loaded save's own game
 *
 * is the "PokeDNA GB import" fingerprint (docs/research-gen12.md 5.10). Never describe
 * this as a supported game mechanism.
 *
 * ONE-WAY ONLY. Nothing here ever writes to a GB save (docs/OVERNIGHT-DECISIONS.md).
 *
 * DETERMINISM IS A HARD REQUIREMENT, not a nicety. A mounted GB save is not held in
 * RAM; each box is re-converted every time it is paged in, and the box/clipboard/bank
 * machinery matches records by their first 8 bytes (personality + otId). The same GB
 * mon must therefore always produce the same 80 bytes, so every derived value here --
 * PID included -- is a pure function of the input struct. Nothing is randomised and
 * nothing is seeded from a clock.
 */

/* Input: one decoded Gen-1 or Gen-2 box/party entry, generation-neutral.
 *
 * gen1_save.c / gen2_save.c own the save layouts, the charsets and (Gen 1) the
 * internal-index -> National Dex map; they fill this struct, and this module never
 * sees a raw GB save. Fields a generation does not have stay 0 (see `gen` below).
 *
 * Everything in here is a value the GB record actually stores, with one deliberate
 * exception: `slot_salt`, which is positional (see below). */
typedef struct {
  uint8_t  gen;             /* 1 = R/B/Y, 2 = G/S/C. Selects the Gen-1 defaults below. */
  uint16_t species_dex;     /* NATIONAL dex no. 1..251. Gen 1: apply PokedexOrder first --
                             * the record's species byte is an internal index, and its
                             * glitch entries map to 0, which this module refuses. */
  uint32_t exp;             /* raw 24-bit EXP, big-endian in the record, host order here */
  uint8_t  level;           /* the record's stored level; a sanity gate only -- the level
                             * that ships is recomputed from EXP (Gen-3 box records store
                             * no level at all, only EXP). */
  uint8_t  dv_atk, dv_def, dv_spd, dv_spc;  /* 0..15. The HP DV is derived here, not stored. */
  uint16_t moves[4];        /* Gen-1/2 move ids; 0 = empty slot. Slot 0 must be non-zero. */
  uint8_t  pp_ups[4];       /* PP Ups applied per move, 0..3 (record PP byte bits 6-7).
                             * Only the Ups carry: current PP is rebuilt from Gen 3's own
                             * base PP. Gen1Mon.ppups[] and G2Mon.pp_up[] drop straight in. */
  uint16_t ot_id;           /* the GB 16-bit trainer id -> Gen-3 public TID (SID becomes 0) */
  /* Decoded text, NUL-terminated, UTF-8 -- one GB byte can decode to several (the
   * gender signs, the 'd/'l/'s contractions, PK/MN), so these are sized for the worst
   * case exactly as Gen1Mon and G2Mon are, and truncation happens by GLYPH downstream
   * in gen3_edit.c's encode_name (7 and 10 glyphs), never by byte here. */
  char     ot_name[36];     /* <=7 glyphs  */
  char     nickname[36];    /* <=10 glyphs */
  uint8_t  held_item;       /* Gen 2 only, 0 = none -- a non-zero value REFUSES the mon.
                             * Gen 1: leave 0. The catch-rate byte at record+0x07 only
                             * becomes an item when the Time Capsule converts it, and
                             * Gen 3 has no home for the Gen-2 item ids anyway. */
  uint8_t  friendship;      /* Gen 2 only; Gen 1 mons get 70, as gen3_build_mon does */
  uint8_t  pokerus;         /* Gen 2 only; same 4-bit strain / 4-bit days layout as Gen 3 */
  bool     is_egg;          /* Gen 2: species-list byte 0xFD. Refused (Transporter too). */
  bool     has_caught_data; /* Crystal only -- gates ot_gender below */
  uint8_t  ot_gender;       /* Crystal caught data: 0 male, 1 female */
  uint32_t slot_salt;       /* Distinguishes two byte-identical GB mons. Gen 1/2 records
                             * carry no unique id, so genuine twins (same species, DVs,
                             * EXP, OT and nickname) hash alike and would convert to the
                             * same 80 bytes -- which the 8-byte identity matching would
                             * then treat as one mon. Pass something STABLE per source
                             * slot, e.g. box * 20 + slot (party: 20 * nboxes + slot).
                             * Stability is what determinism needs; uniqueness is what
                             * the twin case needs. 0 is legal and gives back the pure
                             * record-only hash. */
} Gb12Mon;

/* Where the mon is going. */
typedef struct {
  uint8_t met_game;         /* Gen-3 origin id of the LOADED save: 1 Sapphire, 2 Ruby,
                             * 3 Emerald, 4 FireRed, 5 LeafGreen. 0 -> Emerald.
                             * Gen 3's origin field is 4 bits with no value for a GB
                             * game, so no honest encoding of "came from Gold" exists;
                             * the honesty lives in the met-location signature instead. */
} Gb12Target;

/* Why a record cannot convert. The UI greys a row out and prints gen12_reason_text(). */
typedef enum {
  GB12_OK = 0,
  GB12_ERR_EMPTY,       /* no Pokemon in this slot                                    */
  GB12_ERR_SPECIES,     /* dex 0 (MissingNo / glitch index) or > 251                   */
  GB12_ERR_EGG,         /* eggs cannot be transferred -- Transporter refuses them too  */
  GB12_ERR_HELD_ITEM,   /* holding an item; refused rather than silently losing it     */
  GB12_ERR_MOVE,        /* empty first move slot, or a move id no Gen-3 game has       */
  GB12_ERR_LEVEL,       /* stored level 0 or > 100 (glitched record)                   */
  GB12_ERR_PID,         /* no PID satisfied the constraints (see gen12_convert)        */
} Gb12Result;

const char* gen12_reason_text(Gb12Result r);

/* Honest side-notes about one conversion, for the UI to surface. All false/0 is the
 * normal case; the host test asserts that across every synthetic mon it builds. */
typedef struct {
  bool exp_clamped;      /* EXP exceeded the species' level-100 total and was capped   */
  bool gender_relaxed;   /* PID search dropped the gender constraint (should not happen)*/
  bool letter_relaxed;   /* PID search dropped the Unown letter (should not happen)     */
} Gb12Notes;

/* Can this record convert, and if not why? Pure, cheap (no PID search) -- safe to call
 * for all 20 slots while drawing a box. */
Gb12Result gen12_can_convert(const Gb12Mon* in);

/* Convert one record into a real 80-byte Gen-3 PC-box record: encrypted, checksummed,
 * substructs in the personality-derived order, ready for the clipboard, the bank or
 * app_inject_to_game. Built with gen3_build_mon + the em_* setters, so none of that
 * encoding is re-implemented here.
 *
 * `notes` may be NULL. On any result other than GB12_OK, `out` is untouched. */
Gb12Result gen12_convert(const Gb12Mon* in, const Gb12Target* tgt,
                         uint8_t out[80], Gb12Notes* notes);

/* The derived properties, exposed because the box grid wants them before (or without)
 * a full conversion, and because the host test pins them independently of the record.
 * All are pure functions of the DVs / EXP.
 *
 * gen12_hp_dv        Gen 1/2 store no HP DV; it is the four DVs' low bits.
 * gen12_is_shiny     the Gen-2 DV pattern (Gen-1 mons are testable against it too --
 *                    that is exactly what a VC transfer does).
 * gen12_gender       0 male, 1 female, 2 genderless -- the same three values, for the
 *                    same ratio byte, that pk_gender_from returns on the Gen-3 side.
 * gen12_unown_letter 0..25 = A..Z; Gen 2 has no ! or ? forms.
 * gen12_nature       Transporter's rule: EXP % 25. */
uint8_t gen12_hp_dv(uint8_t atk, uint8_t def, uint8_t spd, uint8_t spc);
bool    gen12_is_shiny(uint8_t atk, uint8_t def, uint8_t spd, uint8_t spc);
uint8_t gen12_gender(uint8_t dv_atk, uint8_t gen3_gender_ratio);
uint8_t gen12_unown_letter(uint8_t atk, uint8_t def, uint8_t spd, uint8_t spc);
uint8_t gen12_nature(uint32_t exp);

/* DV -> IV, deterministically: IV = DV * 2 + GEN12_IV_LOW_BIT.
 *
 * Poke Transporter randomises IVs (three guaranteed 31). We deliberately do not: this
 * viewer re-converts a box on every page-in, so randomness would make a Pokemon's IVs
 * change while you look at it, and would break the record-identity matching. DV*2 is
 * also transparent (exactly invertible: DV = IV / 2) and never fabricates a perfect
 * stat the mon did not earn. The low bit is DEFINED as 0 to keep that invertibility;
 * it is a named constant so a future "Transporter-style" toggle has one place to go. */
#define GEN12_IV_LOW_BIT 0
uint8_t gen12_iv_from_dv(uint8_t dv);

/* ---- adapters from the two save readers ------------------------------------------
 *
 * Field-copy only, no I/O, no allocation -- they exist so the mapping lives in ONE
 * place instead of being re-guessed in pdna_gb.c. `slot_salt` is Gb12Mon.slot_salt:
 * pass something stable per source slot (box * 20 + slot).
 *
 * Two mappings are deliberate and easy to get wrong the other way:
 *   - Gen1Mon.catch_rate does NOT become held_item. That byte is only an item after a
 *     Time Capsule trade, and treating it as one would refuse most Gen-1 mons.
 *   - Gen1Mon.dex (already run through the internal-index map), never species_idx. */
#include "gen1_save.h"
#include "gen2_save.h"
void gen12_from_gen1(const Gen1Mon* m, uint32_t slot_salt, Gb12Mon* out);
void gen12_from_gen2(const G2Mon* m, uint32_t slot_salt, Gb12Mon* out);

#endif /* GEN12_CONVERT_H */
