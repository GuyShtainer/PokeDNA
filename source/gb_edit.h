#ifndef GB_EDIT_H
#define GB_EDIT_H

#include <stdint.h>
#include <stdbool.h>
#include "gen1_save.h"   /* Gen1Mon, GEN1_* geometry, gen1_dex_from_index, gen1_decode */
#include "gen2_save.h"   /* G2Mon, G2_* geometry, g2_off_*, the derived-DV predicates  */

/* Generation-1/2 Pokemon RECORD editor — the EditMon equivalent for Game Boy saves.
 * Pure C (no tonc, no FatFs, no GBA headers) so tests/host_gbedit_test.c dual-compiles.
 *
 * WHAT THIS OWNS AND WHAT IT DOES NOT
 * -----------------------------------
 * One Pokemon: its record, its OT name, its nickname, and its species-list byte. It
 * patches an OCCUPIED slot IN PLACE and never touches the count byte, the 0xFF
 * terminator, any other slot, or any checksum. Adding/deleting/compacting — the
 * four-parallel-array surgery, which is exactly what moving the count and the terminator
 * is — belongs to gen1_write.c / gen2_write.c, as does the Gen-1 current-box duality and
 * the Gen-2 backup mirror. Nothing here knows a save exists.
 *
 * LOSSLESS BY CONSTRUCTION, like gen3_edit.c: the record and BOTH name fields are held
 * as raw bytes and only the fields a setter touches are rewritten, so gb_load() ->
 * gb_commit() with no setter in between is BYTE-IDENTICAL (gb_roundtrip_ok asserts it
 * per slot). That matters more here than in Gen 3: real saves carry junk after a name's
 * 0x50 terminator (Guy's Red.sav box 2 slot 1 nickname is "KINGLER" 0x50 0x50 0x00 0x00,
 * box 3 slot 2 is "FLAREON" 0x50 0x09 0x02 0x00 — across his five saves 733 of 2738 name
 * fields carry such a tail) and re-encoding a decoded name would quietly rewrite bytes
 * the player never changed. gb_set_nickname/gb_set_otname therefore refuse to write at
 * all when the text handed to them is the text the field already holds — see NAMES.
 *
 * WHY THIS IS SMALLER THAN THE GEN-3 EDITOR
 * -----------------------------------------
 * No encryption, no personality-keyed substruct order, no per-record checksum. The
 * fields are just bytes at fixed offsets — BIG-endian, the opposite of Gen 3.
 *
 * SOURCES. Every offset and formula below is from the decomps at assets/upstream, which
 * outrank any wiki (docs/kb/licensing.md: reference only, clean-room implementation):
 *   Gen-1 record   pokered/macros/ram.asm:7      (box_struct) and :28 (party_struct)
 *   Gen-2 record   pokecrystal/macros/ram.asm:7  (box_struct) and :32 (party_struct)
 *   list append    pokered/engine/pokemon/add_mon.asm:12-28      (_AddPartyMon)
 *                  pokecrystal/engine/pokemon/move_mon.asm:14-35 (TryAddMonToParty)
 *   stat formula   pokered/home/move_mon.asm:54  (CalcStat)
 *                  pokecrystal/engine/pokemon/move_mon.asm:1424 (CalcMonStatC)
 *   ceil(sqrt)     pokecrystal/engine/math/get_square_root.asm:3
 *   PP-Up bonus    pokered/engine/items/item_effects.asm:2418     (AddBonusPP)
 *                  pokecrystal/engine/items/item_effects.asm:2752 (ComputeMaxPP)
 *   base PP        pokered/data/moves/moves.asm  vs  pokecrystal/data/moves/moves.asm
 *   growth curves  pokered/data/growth_rates.asm == pokecrystal/data/growth_rates.asm
 *   Gen-1 types    pokered/constants/type_constants.asm
 *   charsets       pokered/constants/charmap.asm, pokecrystal/constants/charmap.asm
 */

/* ---- generations, indices, sizes ----------------------------------------- */

#define GB_GEN1 1
#define GB_GEN2 2

/* ONE stat order for both generations, and for BOTH the DV and the stat-exp arrays:
 * HP, Attack, Defense, Speed, Special. That is the order the records themselves use
 * (pokered/macros/ram.asm:19 HPExp..SpecialExp, pokecrystal/macros/ram.asm:12 likewise)
 * and it matches gen1_save.h's G1_HP..G1_SPC exactly, so a Gen1Mon's arrays drop in.
 * Gen 2 splits Special into SpA/SpD only in the *computed party stats*: both are derived
 * from this one Special DV and this one Special stat exp (pokecrystal move_mon.asm:1424,
 * .not_spdef rewinds hl by 2 so SpDef reads the Special stat exp). */
enum { GB_HP = 0, GB_ATK, GB_DEF, GB_SPE, GB_SPC, GB_NSTATS };

#define GB_MAX_REC      48   /* Gen-2 party record; the largest of the four kinds */
#define GB_NAME_BYTES   11   /* OT and nickname fields, both generations          */
#define GB_NICK_GLYPHS  10   /* glyphs the games let you enter in a nickname       */
#define GB_OT_GLYPHS     7   /* ... and in a trainer name                          */
#define GB_MAX_STAT    999   /* MAX_STAT_VALUE, both decomps                       */

/* The species list ends with this byte in BOTH generations — pokered add_mon.asm:27
 * ("ld a, $ff ; terminator") and pokecrystal move_mon.asm:34 ("ld a, -1"). Spelled here
 * as well as in gen2_save.h so the Gen-1 side has a name for it too. */
#define GB_LIST_TERMINATOR 0xFFu

/* Longest UTF-8 a single GB byte can decode to, plus the NUL: the gender signs are 3
 * bytes and an unrepresentable byte becomes a 4-character "{XX}" escape (see NAMES). */
#define GB_GLYPH_MAX     5
/* A buffer of this size decodes ANY 11-byte name field completely: 11 * 4 + NUL = 45. */
#define GB_TEXT_MAX     48

/* Gen-1 base data for ONE species. The Gen-1 record stores type1/type2/catch_rate
 * INSIDE itself (pokered/macros/ram.asm:12-15) and the game only ever rewrites them on
 * EVOLUTION (engine/pokemon/evos_moves.asm:214 -> SetPartyMonTypes), so the stored bytes
 * are what battle actually uses — a species change that leaves them alone produces a
 * Mewtwo that fights as Grass/Poison.
 *
 * There is no Gen-1 base-stat table in this tree and this module will not invent one.
 * Gen-3's table cannot stand in: Gen 2 split Special, and for 43 of the 151 species the
 * Gen-1 Special base differs from the Gen-3 SpA base (verified by diffing pokered vs
 * pokecrystal vs pokefirered base stats — e.g. dex 4 Charmander is 50 in Gen 1 and 60 in
 * Gen 2/3). Gen-1 TYPE IDS also differ from Gen 3's (Gen 1 numbers FIRE 0x14, Gen 3
 * numbers it 10). So the caller supplies this, or the edit is refused.
 *
 * Gen 2 needs none of it: its record has no types and no catch rate, and its base stats
 * are IDENTICAL to Gen 3's for all 251 species (same diff), so pk_base_stats() serves. */
typedef struct {
  uint8_t base[GB_NSTATS];  /* HP, Atk, Def, Spe, Spc — Gen-1 single-Special base stats */
  uint8_t type1, type2;     /* Gen-1 type ids; type2 == type1 for a single-typed species */
} GbGen1Base;
/* The catch-rate byte (record 0x07) is deliberately NOT part of that struct and is never
 * written implicitly. Two reasons, both from the decomp and both visible in Guy's Red.sav:
 *   - evolution does not update it. pokered/engine/pokemon/evos_moves.asm:160-215 reloads
 *     the base stats, recalculates the stats and calls SetPartyMonTypes, and never touches
 *     CatchRate; no other code writes a party mon's copy either. So the stored byte is a
 *     property of what the Pokemon USED TO BE, not of its species;
 *   - after a Time Capsule trade the byte IS the Gen-2 held item (gen1_save.h:105). Red.sav
 *     holds a Venusaur with 83 and a Charizard with 173 where the species catch rates are
 *     both 45 — those are items, and rewriting them on a species edit would delete them.
 * gb_set_gen1_catch_rate() exists for a caller that really means to change it. */

/* The editable Pokemon. ~90 bytes, CALLER-OWNED — put it on the stack. The hardware
 * build has ~1.5 KB of free EWRAM and a post-link guard, so this module declares no
 * statics of its own beyond const tables (which live in ROM). */
typedef struct {
  uint8_t  gen;                      /* GB_GEN1 / GB_GEN2                              */
  uint8_t  rec_len;                  /* 33/44 (Gen 1) or 32/48 (Gen 2)                 */
  bool     is_party;
  uint8_t  rec[GB_MAX_REC];
  uint8_t  otname[GB_NAME_BYTES];    /* raw GB bytes, 0x50-terminated                  */
  uint8_t  nick[GB_NAME_BYTES];
  /* The species-list byte for this slot. It lives OUTSIDE the record, in the box's
   * parallel species list, and the game's menus read it instead of the record — which
   * is why a species edit has to move both. Gen 2 stores 0xFD here for an Egg while the
   * record keeps the real species. */
  uint8_t  list_species;
  bool     have_g1base;
  GbGen1Base g1base;
  /* A stat-affecting setter ran and gb_recalc_stats() has not been called since. Only
   * meaningful for a party record (box records store no stats). gb_check() reports it. */
  bool     stats_stale;
  /* Whether record bytes 0x1D/0x1E (R2_CAUGHT0/1) are a REAL capture record. They are
   * ONLY that in Crystal — in Gold/Silver the identical two bytes are Unused1/Unused2
   * (pokegold/macros/ram.asm:22-23) and the save format gives a bare record no way to
   * tell the two apart; the SAVE HEADER's version does (gen2_save.h G2Save.version),
   * which gb_load() never sees. FALSE by construction here (gb_load/gb_load_parts both
   * memset the whole struct to 0 first, so a caller that never calls the setter below
   * gets the safe answer) — a caller that HAS mounted a Crystal save and knows it must
   * call gb_set_caught_available(e, true) itself after gb_load, e.g. from
   * `s->g2w.sv.version == G2_VER_CRYSTAL` where `s` is its GbSession. Gates the four
   * GBE_MET* rows (gb_editor.c's gbe_fields) and gb_set_caught() itself, so a Gold/
   * Silver mount that never calls the setter can neither see nor write into its own
   * Unused1/Unused2 bytes through this editor (BACKLOG #95 review C1). */
  bool     has_caught;
  /* Set by gb_editor.c's gbe_flip_shiny() the moment it turns shininess ON by moving
   * the Attack DV to a value that does NOT keep the record's current gender (only
   * possible when every bit-1-set Atk DV maps to the other gender for this species'
   * gender_ratio — some ratios have no shiny combination for one of the two sexes).
   * Cleared by every other write, including OFF. gbe_shiny_note() turns this into the
   * sentence the screen shows (BACKLOG #95 review C2). Not meant to be read or set by
   * anything other than gb_editor.c and gb_load/gb_load_parts (which zero it). */
  bool     shiny_gender_forced;
} GbEditMon;

/* ---- list geometry (so the caller never re-derives it) -------------------- */
/* A Gen-1/2 box is NOT an array of records. It is:
 *   count | species list (capacity+1, 0xFF-terminated) | records | OT names | nicknames
 * Gen 1: gen1_save.c:208-211. Gen 2: gen2_save.c:313-322 (g2_off_* are its public form).
 * Pass `box` in the source generation's own numbering — 0..11 or GEN1_PARTY_BOX for
 * Gen 1, 0..13 or G2_BOX_PARTY for Gen 2. Every function returns <0 / false on a bad
 * argument rather than computing a wrong offset. */
bool gb_box_valid(uint8_t gen, int box);
bool gb_box_is_party(uint8_t gen, int box);
int  gb_list_capacity(uint8_t gen, int box);
int  gb_list_size(uint8_t gen, int box);
int  gb_rec_size(uint8_t gen, bool is_party);
int  gb_off_species(uint8_t gen, int box, int slot);
int  gb_off_record(uint8_t gen, int box, int slot);
int  gb_off_otname(uint8_t gen, int box, int slot);
int  gb_off_nickname(uint8_t gen, int box, int slot);

/* How many slots of `list` are OCCUPIED, straight out of the blob's own count byte.
 * -1 when the count cannot be trusted: above the capacity in either generation, or —
 * Gen 2 only — with no 0xFF where the count says the terminator is, which is the exact
 * condition gen2_save.c:334 refuses to read a box under. Slots 0..count-1 hold Pokemon;
 * everything from `count` on is free space, and the byte AT `count` is the terminator. */
int gb_list_count(uint8_t gen, const uint8_t* list, int box);
/* Offset inside `list` of that 0xFF terminator. -1 when the count is unusable. Exposed
 * because the write modules have to move it, and because nothing else may touch it. */
int gb_off_terminator(uint8_t gen, const uint8_t* list, int box);

/* ---- load / commit ------------------------------------------------------- */

/* Load slot `slot` out of one whole list blob (gb_list_size() bytes). Does NOT require
 * the slot to be within the count — the write modules need to read a slot they are about
 * to overwrite, and reading is harmless. false only for bad arguments. */
bool gb_load(GbEditMon* e, uint8_t gen, const uint8_t* list, int box, int slot);

/* Write the editor's four pieces back into their four places in `list`. Nothing else in
 * the blob is touched: not the count, not the terminator, not another slot.
 *
 * ONLY AN OCCUPIED SLOT (0 <= slot < gb_list_count()). Refused otherwise, with nothing
 * written — because the species byte of the FIRST FREE slot is not free space, it is the
 * 0xFF terminator, and a list whose count and terminator disagree is exactly the kind of
 * damage that passes its own checksum while the game reads the box as unreadable
 * (gen2_save.c:334 returns -1 for the whole box) or as a different length.
 *
 * That is also why appending is not this module's job. Both games grow a list in one
 * indivisible move — bump the count, write the species where the terminator was, put the
 * terminator back one slot along (pokered/engine/pokemon/add_mon.asm:12-28,
 * pokecrystal/engine/pokemon/move_mon.asm:14-35, whose comment reads "The terminator is
 * usually here, but it'll be back"). gen1_write.c / gen2_write.c do that, THEN call this
 * for the slot that has just become occupied.
 *
 * A list_species of 0xFF is refused outright: writing the terminator's own value into an
 * occupied slot truncates the box for the game's own walker. */
bool gb_commit(const GbEditMon* e, uint8_t* list, int box, int slot);

/* Same pair for a caller too tight on EWRAM to stage the 1102-byte blob (the paged path
 * gen2_save.h describes): hand over the 32/48-byte record and the two 11-byte names
 * separately. `list_species` may be NULL if the caller does not have it. Both return
 * false — having written NOTHING — on a bad generation, rather than half-filling the
 * caller's buffers and leaving an out-parameter untouched. */
bool gb_load_parts(GbEditMon* e, uint8_t gen, bool is_party, const uint8_t* rec,
                   const uint8_t* otname, const uint8_t* nick, uint8_t list_species);
bool gb_commit_parts(const GbEditMon* e, uint8_t* rec, uint8_t* otname, uint8_t* nick,
                     uint8_t* list_species);

/* load -> commit is byte-identical for this slot. The per-record half of the "a no-op
 * edit must change nothing" gate. */
bool gb_roundtrip_ok(uint8_t gen, const uint8_t* list, int box, int slot);

/* ---- the safety gate, and the all-or-nothing write ----------------------- */

/* Everything one slot owns, copied out so a failed write can be undone byte for byte.
 * ~76 bytes, CALLER-OWNED — put it on the stack; this module allocates nothing. It
 * remembers which box and slot it came from, so it can never be restored over the wrong
 * Pokemon. */
typedef struct {
  uint8_t gen, rec_len;
  int     box, slot;
  bool    valid;
  uint8_t rec[GB_MAX_REC];
  uint8_t otname[GB_NAME_BYTES];
  uint8_t nick[GB_NAME_BYTES];
  uint8_t list_species;
} GbSlotSnapshot;

bool gb_slot_save(GbSlotSnapshot* s, uint8_t gen, const uint8_t* list, int box, int slot);
bool gb_slot_restore(const GbSlotSnapshot* s, uint8_t* list);

/* THE SAFETY PROPERTY, at record level: after gb_commit() has patched `list`, confirm
 * that every byte of the slot came back exactly as the editor holds it. Two halves,
 * because either alone has a hole:
 *
 *  1. RAW BYTES at this module's own offsets — the whole record (33/44/32/48 bytes),
 *     both 11-byte name fields including whatever sits past their 0x50 terminators, and
 *     the species-list byte. This half has no blind spots by construction. It has to be
 *     the raw bytes and not the parsers' structs: Gen1Mon carries no stats, no current
 *     HP, no types and no box-level byte, so a struct comparison silently skips 16 of a
 *     Gen-1 party record's 44 bytes — precisely the ones gb_recalc_stats,
 *     gb_set_species/gb_set_gen1_base and gb_set_level write.
 *  2. THE SHIPPING PARSER — gen1_decode / g2_list_mon / g2_list_species, code this
 *     module did not write, which computes the four parallel arrays' offsets ITSELF. If
 *     this file's arithmetic were off by one slot, half 1 would happily compare the
 *     wrong bytes against themselves and only this half would notice.
 *
 * Refuse the write if this returns false. NOTE the parsers only decode slots INSIDE the
 * list's count, so `slot` must already be occupied. */
bool gb_verify_slot(const GbEditMon* e, const uint8_t* list, int box, int slot);

/* THE DOCUMENTED WRITE PATH. gb_commit() followed by a failed gb_verify_slot() leaves
 * the caller's blob already mutated with no way back, which is the wrong shape for a
 * gate: the check runs after the damage. This is the all-or-nothing version — snapshot,
 * commit, verify, and on ANY failure put all four pieces back exactly as they were and
 * return false. On false the blob is byte-identical to what it was on entry.
 *
 * It does not run gb_check(): "is this a sensible Pokemon" is a policy the caller
 * applies first and can knowingly override (a hacked mon the player already owns is
 * still theirs to keep), where "did the bytes land where I put them" never is. */
bool gb_commit_checked(const GbEditMon* e, uint8_t* list, int box, int slot);

/* ---- getters ------------------------------------------------------------- */

uint8_t  gb_get_species_raw(const GbEditMon* e);  /* Gen-1 INTERNAL index / Gen-2 dex no. */
uint16_t gb_get_species_dex(const GbEditMon* e);  /* National Dex; 0 = MissingNo/glitch   */
uint8_t  gb_get_level(const GbEditMon* e);
uint32_t gb_get_exp(const GbEditMon* e);
uint8_t  gb_get_dv(const GbEditMon* e, int stat); /* GB_HP is DERIVED, never stored       */
uint16_t gb_get_statexp(const GbEditMon* e, int stat);
uint8_t  gb_get_move(const GbEditMon* e, int i);
uint8_t  gb_get_pp(const GbEditMon* e, int i);
uint8_t  gb_get_ppup(const GbEditMon* e, int i);
uint16_t gb_get_otid(const GbEditMon* e);
uint8_t  gb_get_held_item(const GbEditMon* e);    /* Gen 2 only; 0 for Gen 1              */
uint8_t  gb_get_friendship(const GbEditMon* e);   /* Gen 2 only; 0 for Gen 1              */
uint16_t gb_get_stat(const GbEditMon* e, int i);  /* party only, i = 0..4 (Gen 1) / 0..5  */
/* Current HP — party only, 0 for a box record. NOT because the byte is absent: a Gen-1
 * box record's struct carries the same current-HP field at the same offset as its party
 * counterpart (pokered/macros/ram.asm's box_struct, verified against R1_CURHP). It reads
 * 0 here because a box record pairs with no COMPUTED max HP (gb_get_stat() is party-only
 * too), and a bare current-HP number with no max to put it over is not worth showing —
 * not because the bytes do not exist. Read-only: no editor setter exists for it, on
 * purpose — real play only ever changes it via battle damage or a heal, neither of which
 * this tree models. */
uint16_t gb_get_current_hp(const GbEditMon* e);
/* Gen-1-only, read-only: the STORED type bytes and status byte (pokered/macros/ram.asm:
 * 12-15 box_struct's Type1/Type2/Status). 0 for a Gen-2 record. Gen-1 type IDS are NOT
 * Gen-3's (see GbGen1Base's own comment) — this tree has no Gen-1-NUMBERED type-name
 * table, but the ids map onto Gen 3's own numbering (identity for 0x00-0x05, -1 for
 * 0x07-0x09, -0x0A for 0x14-0x1B; 0x06 is Gen 1's unused BIRD slot, no equivalent), so a
 * caller can still show the real name via pk_type_name()/ui_type_chip() after mapping —
 * see source/pdna_gbsummary.c's g1_to_g3_type(). */
uint8_t  gb_get_gen1_type1(const GbEditMon* e);
uint8_t  gb_get_gen1_type2(const GbEditMon* e);
uint8_t  gb_get_gen1_status(const GbEditMon* e);
/* Gen-2-only: high nibble strain, low nibble days left (0 with a strain set = immune,
 * past infection); 0 = never had it. 0 for a Gen-1 record (no such field exists there).
 * Read-only — this tree has no setter or row for it yet. */
uint8_t  gb_get_pokerus(const GbEditMon* e);
bool     gb_is_egg(const GbEditMon* e);           /* Gen 2 list byte 0xFD                 */
/* Gen-2-only readers for the capture record gb_set_caught() writes (record bytes
 * 0x1D/0x1E, same packing gen2_save.c:493-497 unpacks): 0 for a Gen-1 record (no such
 * field there). BACKLOG #95 (summary-field parity audit): added alongside gb_editor.c's
 * new GBE_MET* rows -- gb_set_caught already existed and is tested (host_gbedit_test.c),
 * these are read-only companions so a row can show the CURRENT value before an edit
 * changes just one of the four packed fields. */
uint8_t  gb_get_caught_time(const GbEditMon* e);       /* 0..3: none/morning/day/night   */
uint8_t  gb_get_caught_level(const GbEditMon* e);      /* 0..63 (1 == "hatched from an Egg" to the Poke Seer) */
uint8_t  gb_get_caught_loc(const GbEditMon* e);        /* 0..127, raw id -- no name table in this tree yet */
uint8_t  gb_get_caught_ot_gender(const GbEditMon* e);  /* 0 = male, 1 = female            */
/* Editable text — see NAMES.
 *
 * USE A GB_TEXT_MAX BUFFER. A smaller `cap` truncates (on a glyph boundary, never
 * mid-UTF-8), and a truncated string is by definition not the text the field holds, so
 * handing it back to gb_set_nickname is a REAL edit that shortens the name. The
 * preserve-on-unchanged rule cannot save a caller from a buffer it made too small. */
int gb_get_nickname(const GbEditMon* e, char* out, int cap);
int gb_get_otname(const GbEditMon* e, char* out, int cap);

/* ---- setters ------------------------------------------------------------- */
/* Every one that can be wrong returns bool and, when it returns false, has changed
 * NOTHING. "When in doubt, refuse the write and say why." */

/* Species by NATIONAL DEX number. Gen 1 stores an internal index instead (Rhydon is 1,
 * Bulbasaur 0x99), so the dex number is mapped back through gen1_dex_from_index()'s
 * table — reused, not duplicated. Also rewrites the species-list byte (except on a
 * Gen-2 Egg, whose list byte stays 0xFD while the record carries the real species).
 *
 * KEEPS THE LEVEL, MOVES THE EXP. The record stores level and EXP separately, so a
 * species whose growth curve differs would leave them disagreeing; the level is what the
 * player sees, so EXP is re-derived for it under the NEW species' curve.
 *
 * `newbase` describes the NEW species and is REQUIRED for Gen 1 (see GbGen1Base); pass
 * NULL for Gen 2. Fails if the dex number has no representation in `e->gen`. */
bool gb_set_species(GbEditMon* e, uint16_t dex, const GbGen1Base* newbase);

/* Level 1..100, and the EXP that must agree with it under the species' growth rate.
 * Fails on a glitch species (no growth rate to use). For a Gen-1 PARTY record this also
 * writes the box-level byte at 0x03, which the game itself syncs on deposit
 * (pokered/engine/pokemon/add_mon.asm:427) and otherwise leaves stale. */
bool gb_set_level(GbEditMon* e, uint8_t level);
/* The other direction: set EXP (clamped to the species' level-100 total) and re-derive
 * the level from it, so the two can never disagree. */
bool gb_set_exp(GbEditMon* e, uint32_t exp);

/* Move slot i (0..3). Setting a move resets its PP to the move's base PP and CLEARS that
 * slot's PP Ups, because a PP Up is bought for a move and not for a slot — the same rule
 * gen3_edit.c's em_set_move follows. move == 0 empties the slot, PP byte and all. */
bool gb_set_move(GbEditMon* e, int i, uint8_t move);
/* Both REFUSE an empty move slot. A slot with no move has no PP and no PP Ups to buy,
 * and the byte is not a free 8 bits: gb_max_pp() is 0 there, so the old gb_set_ppup()
 * stored ups<<6 on top of nothing and invented a record no cartridge produces. All 908
 * empty move slots across Guy's five real saves hold PP byte 0x00, without exception.
 * Use gb_set_move(e, i, 0) to clear a slot. */
bool gb_set_pp(GbEditMon* e, int i, uint8_t pp);     /* clamped to gb_max_pp()        */
bool gb_set_ppup(GbEditMon* e, int i, uint8_t ups);  /* 0..3; re-clamps current PP    */

/* DVs. stat is GB_ATK/GB_DEF/GB_SPE/GB_SPC only: GB_HP is REFUSED because the HP DV is
 * not stored anywhere — it is the parity of the other four (pokered move_mon.asm:54's
 * "get HP IV" branch), so letting a caller set it independently would be a lie. Use
 * gb_preview_dv() first to see what a change does to gender/shininess/Unown letter. */
bool gb_set_dv(GbEditMon* e, int stat, uint8_t v);      /* 0..15 */
bool gb_set_statexp(GbEditMon* e, int stat, uint16_t v);/* 0..65535, all five */

void gb_set_otid(GbEditMon* e, uint16_t id);

/* ---- NAMES ---------------------------------------------------------------
 * THE GUARANTEE, in one line:
 *
 *     gb_get_nickname() followed by gb_set_nickname() with no edit in between
 *     changes ZERO BYTES — for every name in every slot of every real save.
 *
 * Same for gb_get_otname/gb_set_otname. That is a hard requirement, not a nicety: it is
 * the only thing an editing UI ever does with a name it is not editing — read it into a
 * buffer, show it, hand it back on confirm — so a round trip that is not exact rewrites
 * names the player never touched. Measured over Guy's whole Game Boy corpus — Red.sav,
 * Gold.sav, Crystal.sav, both VC saves and the 100 .pk2 files, 2938 name fields —
 * **0 changed**, against **1201 (40.9%)** through the lossy path it replaced. That is the
 * figure tests/host_gbedit_test.c itself prints, so it is the one to quote.
 *
 * (An earlier revision of this comment claimed "665, 36.7%" over a narrower 1810-field
 * subset. No definition of the measurement reproduces it — the subset figure is 656,
 * 36.2% — and it was caught by a verifier re-deriving the field offsets independently.
 * Recorded rather than quietly corrected, because this block's whole argument is that its
 * numbers are measured rather than asserted, and a number nobody can reproduce undermines
 * that more than a missing one would.)
 *
 * Three mechanisms, because each alone has a hole the next one covers:
 *
 *  1. AN UNCHANGED NAME IS NOT WRITTEN AT ALL. Each setter decodes the field it is about
 *     to overwrite and, if that is the string it was handed, returns true having touched
 *     no byte. This is what preserves the junk past the 0x50 terminator — 733 of the
 *     2738 name fields in Guy's five saves carry some — which no encoder could
 *     reconstruct from text.
 *  2. THE DECODER IS REVERSIBLE PER BYTE. Every one of the 255 non-terminator byte values
 *     survives gb_char_decode -> gb_char_encode in both generations. Where the shipping
 *     READERS lose a glyph — gen1_save.c renders é as "e" and everything unmapped as "?",
 *     gen2_save.c renders <PK>/<MN>/é/×/the umlauts as a space or a lookalike, and both
 *     flatten the decimal point 0xF2 onto "." — this module spells the glyph out (é, ×,
 *     Ä…) or, for a byte with no text spelling at all, emits a four-character hex escape
 *     "{5D}" that gb_char_encode turns straight back into 0x5D. 0x5D is <TRAINER>, the OT
 *     of every in-game trade in Gen 1; without the escape, editing such a mon's nickname
 *     rewrote its OT-name bytes into "?" or a space.
 *  3. AND REVERSIBLE AS A SEQUENCE, which does NOT follow from 2. Seven glyphs spell
 *     themselves with two ASCII characters (<PK>, <MN> and the five contractions), and
 *     those same characters can equally be two one-character glyphs side by side: 'P','K'
 *     and <PK> both read "PK", so an encoder must destroy one of them. gb_name_decode
 *     therefore escapes the byte that would merge with the character before it — 0x8F 0x8A
 *     decodes to "P{8A}", never "PK". The common glyph keeps the readable spelling: <PK>
 *     is the OT "Mattia<PK>" on 645 of Guy's 1810 name fields, where a literal 'P','K'
 *     pair occurs zero times. USE gb_name_decode, NOT a loop over gb_char_decode — only
 *     the former is sequence-safe.
 *
 * So gb_get_nickname() deliberately does NOT agree with gen1_decode_name/g2_decode_text
 * on the glyphs those two throw away. gb_verify_slot still cross-checks against the
 * shipping decoders, so the disagreement is visible rather than assumed.
 *
 * "{50}" is the one escape that is refused (it would plant a terminator mid-name); it
 * falls back to the space policy. The GB charset has no braces, so an escape can never
 * collide with a name a player could type.
 *
 * WHEN THE PLAYER REALLY DOES TYPE SOMETHING THE CARTRIDGE CANNOT HOLD, these REFUSE:
 * they return false and write nothing, rather than silently substituting a space and
 * handing back a name the player did not ask for. Call gb_text_lossy() first to tell them
 * WHICH character is the problem, and use the _lossy variants below once they have agreed
 * to it. By mechanism 2 no name already in a save can trigger this — only new input, so
 * refusing can never strand a Pokemon the player already owns.
 *
 * UTF-8 in and GB charset out, truncated by GLYPH (never mid-UTF-8) at GB_NICK_GLYPHS /
 * GB_OT_GLYPHS, then 0x50-filled to 11 bytes. Truncation past the cap is the one loss that
 * is NOT refused — it is the game's own limit, and gb_get_* would show the short name.
 * The two generations do NOT share a charset above 0xB9 (gen2_save.h documents the
 * divergence), so `e->gen` picks the table. */
bool gb_set_nickname(GbEditMon* e, const char* s);
bool gb_set_otname(GbEditMon* e, const char* s);
/* Substitute-and-write-anyway, for a caller that has shown the player gb_text_lossy()'s
 * verdict and had them accept it. Unrepresentable glyphs become spaces. */
bool gb_set_nickname_lossy(GbEditMon* e, const char* s);
bool gb_set_otname_lossy(GbEditMon* e, const char* s);
/* How many of the first `max_glyphs` glyphs of `s` this generation cannot store exactly —
 * 0 means gb_set_nickname/gb_set_otname will accept it. `first_bad` (may be NULL) receives
 * the first offending glyph as NUL-terminated text, so the UI can say which one. Counts
 * both outright substitutions (a euro sign, '&' in Gen 1) and the Gen-1 transliterations
 * (ä -> 'a'), because neither is what the player typed. */
int gb_text_lossy(uint8_t gen, const char* s, int max_glyphs, char first_bad[GB_GLYPH_MAX]);
/* Byte-exact alternative, for a caller copying a name between saves: no decode, no
 * encode, no chance of a charset round-trip degrading a glyph. */
void gb_set_nickname_raw(GbEditMon* e, const uint8_t b[GB_NAME_BYTES]);
void gb_set_otname_raw(GbEditMon* e, const uint8_t b[GB_NAME_BYTES]);

/* Gen-2-only fields. Each returns false on a Gen-1 record rather than writing to the
 * byte that happens to sit at that offset — in Gen 1, record+0x01 is current HP and
 * +0x07 is the catch rate, not an item. Also refuses a non-zero item on an Egg (the
 * games refuse this too, pack.asm AnEggCantHoldAnItemText) — clearing to 0 is always
 * allowed, an Egg just cannot hold anything real. */
bool gb_set_held_item(GbEditMon* e, uint8_t item);
bool gb_set_friendship(GbEditMon* e, uint8_t f);
bool gb_set_pokerus(GbEditMon* e, uint8_t p);
/* Crystal's capture record: time 0..3 (0 none, 1 morning, 2 day, 3 night), level 0..63
 * (1 means "hatched from an Egg" to the Poke Seer), location 0..127, ot_gender 0/1.
 * Packed into record bytes 0x1D/0x1E exactly as gen2_save.c:493-497 unpacks them.
 * Refuses unless e->has_caught — on Gold/Silver those same two bytes are Unused1/
 * Unused2, not a capture record, and this module has no way to tell the two apart
 * from the record alone (see GbEditMon.has_caught, gb_edit.h). */
bool gb_set_caught(GbEditMon* e, uint8_t time, uint8_t level, uint8_t loc, uint8_t ot_gender);
/* Whether e->has_caught is set — see GbEditMon.has_caught's own comment for who is
 * meant to call this and when (a caller that knows its mount is Crystal, right after
 * gb_load). Always legal to call, including on a Gen-1 record (where it is simply
 * inert: gb_set_caught still refuses Gen 1 outright on the generation check alone). */
void gb_set_caught_available(GbEditMon* e, bool available);
/* Egg-ness is the species-LIST byte, not a record field. Turning it ON is refused
 * while the record holds a non-zero item — an Egg cannot hold one (see
 * gb_set_held_item) — rather than silently clearing it out from under the player;
 * remove the item first. Turning it OFF is unaffected (never touches the item). */
bool gb_set_egg(GbEditMon* e, bool egg);

/* Gen-1-only: install base data for the CURRENT species (writes type1/type2 into the
 * record and enables gb_recalc_stats). Returns false on a Gen-2 record. */
bool gb_set_gen1_base(GbEditMon* e, const GbGen1Base* b);
/* Gen-1-only, and never called for you — see the note above GbGen1Base. */
bool gb_set_gen1_catch_rate(GbEditMon* e, uint8_t v);

/* ---- derived party stats -------------------------------------------------- */

/* Recompute the stored party stats from base stats + DVs + stat exp + level, using the
 * games' own formula, and carry current HP across by the SAME rule the game uses when a
 * mon's maximum changes under it: new_cur = cur + (new_max - old_max)
 * (pokered/engine/pokemon/evos_moves.asm:186-203 does exactly that arithmetic on
 * evolution). So a full-HP mon stays full, a damaged one keeps its damage, and the result
 * is clamped into 1..new_max; a fainted mon (0 HP) stays fainted. Never a free heal.
 *
 * Box records store no stats, so this is a no-op success for them. Gen 2 gets its base
 * stats from pk_base_stats() (identical to pokecrystal's for all 251, verified). Gen 1
 * needs gb_set_gen1_base() first and returns false without it, leaving stats_stale set —
 * stale stats are visible to the player, so the caller must decide, not this file. */
bool gb_recalc_stats(GbEditMon* e);

/* ---- what a DV change will do (before you commit it) ---------------------- */

/* Gen 2 stores no gender byte, no shiny byte and no Unown form: all three are DERIVED
 * from the DVs, so changing a DV silently changes what the Pokemon IS. Expose that.
 * (A Gen-1 mon has none of these properties in its own game, but the same numbers are
 * exactly what a Virtual Console transfer reads out of it, so they are still shown.) */
typedef struct {
  uint8_t hp_dv;         /* the derived HP DV                                         */
  bool    shiny;         /* Gen-2 rule: Def==Spe==Spc==10 and Atk in {2,3,6,7,10,...}  */
  int     gender;        /* 0 male, 1 female, 2 genderless                             */
  int     unown_letter;  /* 0..25 = A..Z; only meaningful for dex 201                  */
  bool    unown;         /* this record actually IS an Unown                           */
} GbDvEffects;

/* dv4 is Atk,Def,Spe,Spc — gen2_save.c's own DV order, so its predicates are reused
 * rather than re-derived. */
void gb_dv_effects(const uint8_t dv4[4], uint16_t dex, uint8_t gender_ratio, GbDvEffects* out);
void gb_dv_effects_of(const GbEditMon* e, GbDvEffects* out);
/* What the record WOULD become if gb_set_dv(e, stat, v) were called. Does not mutate. */
bool gb_preview_dv(const GbEditMon* e, int stat, uint8_t v, GbDvEffects* out);

/* ---- shared facts, exposed because the UI and the write modules want them --- */

uint8_t  gb_hp_dv(const uint8_t dv4[4]);            /* == g2_hp_dv, one spelling      */
uint8_t  gb_index_from_dex(uint8_t gen, uint16_t dex);  /* 0 if the gen has no such mon */
uint16_t gb_dex_from_index(uint8_t gen, uint8_t raw);
uint8_t  gb_growth_rate(uint16_t dex);              /* GROWTH_* 0..5                  */
uint32_t gb_exp_for_level(uint16_t dex, uint8_t level);
uint8_t  gb_level_from_exp(uint16_t dex, uint32_t exp);
/* Base PP IS PER GENERATION, for exactly one move. Diffing all 165 shared entries of
 * pokered/data/moves/moves.asm against pokecrystal/data/moves/moves.asm gives a single
 * mismatch: 165 STRUGGLE is 10 PP in Gen 1 (pokered moves.asm:178) and 1 PP in Gen 2
 * (pokecrystal moves.asm:181, which agrees with Gen 3). Struggle is a battle-only
 * fallback that is never stored in a save, but a hacked record can hold it, so it gets
 * the right maximum in each game rather than one guess for both. 0 for move 0 and for
 * any id past gb_max_move(gen). */
uint8_t  gb_move_base_pp(uint8_t gen, uint8_t move);
uint8_t  gb_max_pp(uint8_t gen, uint8_t move, uint8_t ups); /* base + ups*min(base/5,7) */
uint8_t  gb_max_species(uint8_t gen);               /* 151 / 251                      */
uint8_t  gb_max_move(uint8_t gen);                  /* 165 / 251                      */
/* Encode UTF-8 into a GB name field: writes exactly `cap` bytes (0x50-filled) and stops
 * after `max_glyphs` glyphs. Returns the number of glyphs written. */
int gb_name_encode(uint8_t gen, uint8_t* dst, int cap, int max_glyphs, const char* s);
/* Encode ONE glyph; returns how many input BYTES were consumed and stores the GB byte.
 * Two-character sequences ("PK", "MN", "'s", ...) and the "{XX}" escape are single GB
 * glyphs, so this is not a per-char mapping. Consumed==0 means end of string. */
int gb_char_encode(uint8_t gen, const char* s, uint8_t* out);
/* The inverses. gb_char_decode writes a NUL-terminated glyph (up to GB_GLYPH_MAX bytes)
 * and returns its length; 0 means the byte was the 0x50 terminator, stop.
 *
 * gb_char_decode is reversible PER BYTE ONLY. Concatenating its output over a field is
 * NOT reversible — see mechanism 3 under NAMES — so decode whole fields with
 * gb_name_decode, which adds the escaping that makes the sequence reversible too.
 * gb_name_decode fills `out` (cap includes the NUL) and returns the length written,
 * truncating only on a glyph boundary. */
int gb_char_decode(uint8_t gen, uint8_t c, char out[GB_GLYPH_MAX]);
int gb_name_decode(uint8_t gen, char* out, int cap, const uint8_t* src, int nbytes);

/* ---- the structural gate -------------------------------------------------- */

/* Everything that can be structurally wrong about ONE record. All false == clean.
 * gen1_write.c / gen2_write.c call this before committing; the UI can show it live. */
typedef struct {
  bool species_bad;     /* raw species not a real index, or maps to MissingNo          */
  bool list_mismatch;   /* the species-LIST byte disagrees with the record's species   */
  bool level_range;     /* level not 1..100                                            */
  bool level_exp_bad;   /* the stored level is not the level the stored EXP gives      */
  bool move_empty;      /* move slot 0 is empty — the game has no such Pokemon         */
  bool move_hole;       /* an occupied move slot after an empty one                    */
  bool move_range;      /* a move id this generation does not have                     */
  bool move_dup;        /* the same move in two slots                                  */
  bool pp_over;         /* current PP above that move's maximum with its PP Ups        */
  bool pp_on_empty;     /* an empty move slot whose PP byte is not 0 (see gb_set_ppup) */
  bool gen1_type_bad;   /* Gen 1: a type id no Game Boy game has (see g1_type_ok)      */
  bool stats_stale;     /* a stat-affecting edit has not been followed by a recalc     */
} GbIssues;

bool gb_check(const GbEditMon* e, GbIssues* out);   /* true == clean */
/* The first problem, as a sentence for the UI; NULL when clean. */
const char* gb_issue_text(const GbIssues* i);

#endif /* GB_EDIT_H */
