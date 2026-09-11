#ifndef GEN1_WRITE_H
#define GEN1_WRITE_H

#include <stdint.h>
#include <stdbool.h>
#include "gen1_save.h"

/*
 * Generation-I (Red/Blue/Yellow, WESTERN) battery-save WRITER — pure C, no tonc,
 * no FatFs, host-testable. The mirror image of gen1_save.c, which reads.
 *
 * gen1_save.h says "READ ONLY, by design" because until now PokeDNA never wrote a GB
 * save. This module is that decision being revisited, not overturned quietly: nothing
 * here is reachable unless a caller asks for an edit, gen1_save.c is untouched, and the
 * conversion path (gen12_convert.c) is still strictly one-way.
 *
 * PROVENANCE. Every offset, every field and every layout rule below is taken from the
 * pret/pokered disassembly (assets/upstream/pokered, reference only per
 * docs/kb/licensing.md — no decomp code is copied, only facts) and then confirmed
 * against Guy's real Red.sav. Where the two disagreed, the real save won and the
 * disagreement is written down. Citations are file:line into that checkout.
 *
 * ============================ WHY THIS IS SHIPPABLE ============================
 *
 * A Gen-1 box is NOT an array of self-contained records. It is FOUR PARALLEL
 * STRUCTURES that only mean anything together (pokered ram/wram.asm:2224-2248):
 *
 *      [0]                     count byte
 *      [1 .. cap+1]            species index list, 0xFF-terminated at index `count`
 *      [rec_off  ..]           cap fixed-size records   (33 box / 44 party)
 *      [ot_off   ..]           cap 11-byte OT-name fields
 *      [nick_off ..]           cap 11-byte nickname fields
 *
 * A nickname lives ~900 bytes away from its Pokemon. Deleting one means compacting all
 * four and moving the terminator. A count byte that is one too high does not corrupt
 * one Pokemon — it makes the game read the NAME array as records.
 *
 * So the safety property is not "be careful". It is: every write is REBUILT IN A
 * SCRATCH BUFFER, RUN BACK THROUGH gen1_save.c's OWN PARSER PLUS A STRUCTURAL GATE
 * PLUS A SLOT-BY-SLOT BYTE COMPARISON AGAINST THE PRE-EDIT BLOB, AND REFUSED IF IT
 * DOES NOT COME BACK EXACTLY AS INTENDED. That turns "did I lay out four parallel
 * arrays correctly" into something the code proves to itself on every single write.
 * After the bytes land in the image it is verified a SECOND time, from the image, at
 * every destination — and rolled back if that fails. See gen1_write_apply().
 *
 * ============================ THE CURRENT-BOX DUALITY ==========================
 *
 * The open box exists twice: a live copy in the checksummed main block at 0x30C0
 * (sCurBoxData) and a slot in SRAM bank 2/3. The decomp settles which one matters:
 *
 *   - the game LOADS the current box only from 0x30C0 (pokered engine/menus/save.asm:100
 *     LoadCurrentBoxData) and SAVES it only to 0x30C0 (ibid.:246 SaveCurrentBoxData);
 *   - the banked slot of the box you are standing in is deliberately marked EMPTY when
 *     you switch into it (ibid.:408-425 CopyBoxToOrFromSRAM, "mark the source box as an
 *     empty box"), and is not read again until you switch away.
 *
 * So 0x30C0 is authoritative and a write that lands ONLY in the bank slot vanishes.
 * This module therefore treats 0x30C0 as the primary and the bank slot as a MIRROR, and
 * writes both — because Guy's real Red.sav has them byte-identical (some tool, not the
 * retail ChangeBox path, wrote those boxes), so a reader that trusts the bank slot is a
 * real thing in the wild. Getting this wrong is not left to memory: gen1_write_targets()
 * is the single place that enumerates the destinations, gen1_write_apply() writes the
 * same verified blob to every one of them, and the post-write check re-reads and
 * compares EVERY destination.
 *
 * ============================ WHAT IT REFUSES =================================
 *
 * "Never lose or silently alter a Pokemon" beats "be useful", every time:
 *
 *   - a save whose main checksum does not already validate (we do not understand it);
 *   - any stored box, when bit 7 of 0x284C is clear. That bit is BIT_HAS_CHANGED_BOXES
 *     (pokered constants/ram_constants.asm:51); while it is clear the eleven banked
 *     boxes hold un-erased power-up SRAM and the player's first CHANGE BOX runs
 *     EmptyAllSRAMBoxes (engine/menus/save.asm:367,529) which wipes all twelve. An edit
 *     written there is not corrupt, it is DOOMED, so it is refused instead;
 *   - a banked box whose count byte is out of range (Gen1Save.box_uninit);
 *   - inserting into a full box, editing an empty slot, a species byte outside 1..190;
 *   - a name holding a glyph the Gen-1 charset cannot represent — refused, never
 *     silently substituted;
 *   - anything at all that fails the reparse gate.
 *
 * ============================ MEMORY ==========================================
 *
 * No statics, no allocation. Every buffer is the caller's, because the hardware build
 * has ~1.5 KB of free EWRAM and a post-link guard that rejects an overflow. The only
 * sizeable buffer is Gen1WriteScratch (2244 B) which the GBA glue must take from the
 * arena (app_arena_acquire), NOT from the IWRAM stack. Gen1EditMon (~68 B) is fine as
 * a local.
 */

/* ------------------------------------------------------------------------- */
/* Status                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum {
  GEN1W_OK = 0,
  GEN1W_ERR_ARG,       /* NULL pointer, box/slot out of range, box/party kind mismatch */
  GEN1W_ERR_SIZE,      /* image shorter than 32 KiB                                    */
  GEN1W_ERR_SAVE,      /* the image does not parse as a Western R/B/Y save             */
  GEN1W_ERR_VIRGIN,    /* the stored boxes are still un-erased SRAM (see above)        */
  GEN1W_ERR_UNINIT,    /* this banked box's count byte is out of range                 */
  GEN1W_ERR_FULL,      /* insert into a box with no free slot                          */
  GEN1W_ERR_EMPTY,     /* replace/delete an unoccupied slot                            */
  GEN1W_ERR_SPECIES,   /* species byte outside 1..190                                  */
  GEN1W_ERR_TEXT,      /* a name held a glyph the Gen-1 charset cannot represent       */
  GEN1W_ERR_STRUCT,    /* count / terminator / species list is not self-consistent     */
  GEN1W_ERR_VERIFY,    /* it did not read back as intended — WRITE REFUSED             */
  GEN1W_ERR_RANGE,     /* the write lands outside the header span, or on bytes this    */
                       /* module reserves for gen1_write_apply / the checksum (P0)     */
} Gen1WStatus;

const char* gen1_write_status_text(Gen1WStatus st);

/* ------------------------------------------------------------------------- */
/* Text: ASCII/UTF-8 -> the Gen-1 charset                                     */
/* ------------------------------------------------------------------------- */

/* Terminator ("@"). Retail pads the rest of a name field with it too — every nickname
 * field in Guy's Red.sav reads e.g. "MAGMAR" as 8C 80 86 8C 80 91 50 50 50 50 50. */
#define GEN1_TEXT_TERM   0x50u

/* Species-list terminator (pokered home/move_mon.asm:377 "write new sentinel"). */
#define GEN1_LIST_TERM   0xFFu

/* Glyph budgets, from the naming screen: it draws NAME_LENGTH-1 underscores for a
 * Pokemon and PLAYER_NAME_LENGTH-1 for a trainer (pokered
 * engine/menus/naming_screen.asm:384-387, constants/text_constants.asm:1,3). An OT
 * field is 11 bytes wide but can only ever have been filled from a player name, so
 * this module holds it to 7 — matching what the game can produce. */
#define GEN1_NICK_GLYPHS  10
#define GEN1_OT_GLYPHS    7

/* Encode ONE glyph. `s` points into a NUL-terminated UTF-8 string; on success *out gets
 * the GB byte and the return value is how many INPUT bytes were consumed (1..3).
 * Returns 0 for a glyph the charset cannot represent — the caller must refuse, never
 * substitute.
 *
 * The inverse of gen1_char_ascii(), with two deliberate asymmetries:
 *   - it consumes the two-character contractions ('d 'l 's 't 'v 'r 'm) as the single GB
 *     byte the game uses, and the UTF-8 gender signs U+2642/U+2640 as 0xEF/0xF5;
 *   - it does NOT re-fold "PK"/"MN" into the 0xE1/0xE2 ligatures. Those are not on the
 *     naming screen, so a user typing P then K means two letters; both spellings decode
 *     to the same text anyway.
 * Where the decoder is many-to-one it picks the letter, not the decoration: 'e' encodes
 * to 0xA4 (lowercase e), never 0xBA (e-acute), and '.' to 0xE8, never 0xF2. */
int gen1_encode_char(const char* s, uint8_t* out);

/* Encode a whole name into an 11-byte field: glyphs, then GEN1_TEXT_TERM padding to the
 * end. Returns false — leaving `dst` untouched — for an empty string, for more than
 * `max_glyphs` glyphs, or for any glyph gen1_encode_char refuses. An empty name is
 * refused because the naming screen cannot produce one and an empty OT name would never
 * match the player, permanently marking the mon as traded. */
bool gen1_encode_name(uint8_t dst[GEN1_NAME_BYTES], const char* utf8, int max_glyphs);

/* ------------------------------------------------------------------------- */
/* Blob geometry (the four parallel structures)                               */
/* ------------------------------------------------------------------------- */

typedef struct {
  int cap;         /* 20, or 6 for the party                                  */
  int recsz;       /* 33, or 44 for the party                                  */
  int bytes;       /* 0x462, or 0x194                                          */
  int rec_off;     /* 1 + (cap + 1)                                            */
  int ot_off;      /* rec_off + cap * recsz                                    */
  int nick_off;    /* ot_off + cap * GEN1_NAME_BYTES                           */
} Gen1Layout;

void gen1_write_layout(int box, Gen1Layout* out);

/* ------------------------------------------------------------------------- */
/* Species facts this module refuses to guess                                 */
/* ------------------------------------------------------------------------- */

/* Growth-rate ids, pokered constants/pokemon_data_constants.asm:88-93. */
enum {
  GEN1_GROWTH_MEDIUM_FAST = 0,
  GEN1_GROWTH_SLIGHTLY_FAST,
  GEN1_GROWTH_SLIGHTLY_SLOW,
  GEN1_GROWTH_MEDIUM_SLOW,
  GEN1_GROWTH_FAST,
  GEN1_GROWTH_SLOW,
  GEN1_NUM_GROWTH_RATES
};

/* Base stats, growth rate, types and catch rate live in the game's ROM, not in the
 * save, and Gen-1's numbers are not Gen-3's (one combined Special, different growth
 * ids). Rather than carry a second 151-entry table — the GBA build has ~1.5 KB of EWRAM
 * to spare and module B would need the same one — the CALLER supplies them. That also
 * means a wrong table can never be applied silently: the setters that need this refuse
 * when it is NULL. */
typedef struct {
  uint8_t base[G1_NSTATS];   /* HP, Atk, Def, Spe, Spc                          */
  uint8_t growth;            /* GEN1_GROWTH_*                                   */
  uint8_t type1, type2;      /* Gen-1 type ids — STORED PER RECORD, see below   */
  uint8_t catch_rate;        /* record byte 0x07, also stored per record        */
} Gen1SpeciesInfo;

/* ------------------------------------------------------------------------- */
/* Gen-1 arithmetic, from the decomp                                          */
/* ------------------------------------------------------------------------- */

/* One stat, exactly as pokered home/move_mon.asm:54-232 computes it:
 *
 *   t    = ((base + dv) * 2 + ceil(sqrt(statexp))/4) * level / 100
 *   stat = t + 5                (Atk/Def/Spe/Spc)
 *   HP   = t + level + 10
 *   capped at 999
 *
 * `stat` is G1_HP..G1_SPC. The sqrt term counts up and stops at 255, so a maxed 65535
 * stat exp contributes 63, not 64 — that off-by-one is the game's, and reproducing it
 * is the point. */
uint16_t gen1_calc_stat(int stat, uint8_t base, uint8_t dv, uint16_t statexp, uint8_t level);

/* Total EXP at `level` for a growth rate: floor(num*n^3/den) +/- c*n^2 + d*n - e, in
 * 24-bit truncating arithmetic (pokered engine/pokemon/experience.asm CalcExperience +
 * data/growth_rates.asm). Verified against real level-100 party mons in Red.sav:
 * Medium Slow -> 1059860, Slow -> 1250000. */
uint32_t gen1_exp_for_level(uint8_t growth, uint8_t level);

/* The inverse the game uses (CalcLevelFromExperience: count up while the requirement is
 * not greater than the exp). Clamped to 1..100. */
uint8_t gen1_level_from_exp(uint8_t growth, uint32_t exp);

/* Gen 1 stores no HP DV; it is the low bit of the other four (home/move_mon.asm:109). */
uint8_t gen1_derive_hp_dv(uint8_t atk, uint8_t def, uint8_t spe, uint8_t spc);

/* ------------------------------------------------------------------------- */
/* One Pokemon, loaded for editing                                            */
/* ------------------------------------------------------------------------- */

/* Deliberately raw. It holds the record and the two name fields as STORED BYTES, so
 * loading and committing without touching anything is byte-identical — the same
 * "patch in place, never rebuild from a struct" rule gen3_edit.c follows, and the
 * reason a no-op edit cannot alter a save. (It matters here: the OT field in Red.sav
 * reads 80 92 87 50 89 80 82 8A 50 8D 84 — "ASH" plus the tail of a previous player
 * name that retail never cleared. Re-encoding "ASH" would quietly erase those bytes.)
 *
 * There is no separate species field: the species-list byte IS rec[0], so "the list
 * agrees with the record" is true by construction rather than by remembering. */
typedef struct {
  bool    is_party;
  uint8_t rec[GEN1_PARTY_REC_BYTES];   /* 44; a box record uses the first 33 */
  uint8_t ot[GEN1_NAME_BYTES];
  uint8_t nick[GEN1_NAME_BYTES];
} Gen1EditMon;

/* Record field offsets (pokered macros/ram.asm box_struct / party_struct). Public
 * because the UI and the tests both want to point at exact bytes. */
#define G1R_SPECIES     0x00
#define G1R_HP          0x01   /* current HP, big-endian, box AND party        */
#define G1R_BOXLEVEL    0x03   /* the level as of the last deposit             */
#define G1R_STATUS      0x04
#define G1R_TYPE1       0x05
#define G1R_TYPE2       0x06
#define G1R_CATCH_RATE  0x07
#define G1R_MOVES       0x08   /* 4 bytes                                      */
#define G1R_OTID        0x0C   /* big-endian                                   */
#define G1R_EXP         0x0E   /* 24-bit big-endian                            */
#define G1R_STATEXP     0x11   /* 5 x u16 big-endian, HP/Atk/Def/Spe/Spc       */
#define G1R_DVS         0x1B   /* Atk<<4|Def, then Spe<<4|Spc                  */
#define G1R_PP          0x1D   /* 4 bytes: bits 0-5 current PP, 6-7 PP Ups     */
#define G1R_LEVEL       0x21   /* PARTY ONLY: the live level                   */
#define G1R_STATS       0x22   /* PARTY ONLY: 5 x u16 MaxHP/Atk/Def/Spe/Spc    */

/* Load slot `slot` of a list blob. False (and `e` zeroed) if the slot is unoccupied, or
 * if the species-list byte disagrees with the record's own species byte (the
 * Gen1Mon.list_mismatch case). Such a slot is READ-ONLY here on purpose: committing it
 * back would have to pick a winner, and quietly rewriting one of the two is exactly the
 * "silently alter a Pokemon" failure. Delete it instead, or fix it in the game. */
bool gen1_edit_load(const uint8_t* list, int box, int slot, Gen1EditMon* e);

/* --- setters. Everything is clamped or refused; nothing is silently rounded. --- */

void g1e_set_dv(Gen1EditMon* e, int stat, uint8_t v);        /* G1_ATK..G1_SPC, 0..15 */
void g1e_set_statexp(Gen1EditMon* e, int stat, uint16_t v);  /* G1_HP..G1_SPC         */
void g1e_set_exp(Gen1EditMon* e, uint32_t exp);              /* 24-bit                */
void g1e_set_otid(Gen1EditMon* e, uint16_t id);
void g1e_set_status(Gen1EditMon* e, uint8_t st);
void g1e_set_curhp(Gen1EditMon* e, uint16_t hp);
void g1e_set_catch_rate(Gen1EditMon* e, uint8_t v);

/* Move slot i (0..3). `move` 0 empties the slot. `base_pp` is the move's base PP from
 * the game's move table, which this module does not carry; it becomes the stored PP and
 * the PP Ups are cleared, because a PP Up binds to the move and not to the slot
 * (pokered engine/pokemon/add_mon.asm:251 AddPartyMon_WriteMovePP). */
void g1e_set_move(Gen1EditMon* e, int i, uint8_t move, uint8_t base_pp);
void g1e_set_pp(Gen1EditMon* e, int i, uint8_t cur_pp);      /* 0..63, keeps PP Ups   */
void g1e_set_ppup(Gen1EditMon* e, int i, uint8_t ups);       /* 0..3,  keeps PP       */

/* Names. False (and nothing changed) if the text does not fit the charset or the glyph
 * budget — GEN1_NICK_GLYPHS / GEN1_OT_GLYPHS. */
bool g1e_set_nickname(Gen1EditMon* e, const char* utf8);
bool g1e_set_otname(Gen1EditMon* e, const char* utf8);

/* SPECIES IS FIVE BYTES, NOT ONE, and that is why `si` is not optional.
 *
 * Gen 1 stores a Pokemon's TYPES and CATCH RATE in the record itself, not in the
 * species header (that is what makes the Mew/type glitches possible), so changing
 * rec[0] alone leaves a Pikachu that is still Water-type in battle. This setter writes
 * species + type1 + type2 + catch rate together so the mistake is unavailable. Pass the
 * mon's CURRENT types back in if you are deliberately keeping a type-hacked mon.
 *
 * Stats are NOT recomputed here — call g1e_recalc_stats() (party) once you have also
 * settled the level. False if `si` is NULL or the index is outside 1..190. */
bool g1e_set_species(Gen1EditMon* e, uint8_t internal_index, const Gen1SpeciesInfo* si);

/* LEVEL IS THREE FIELDS, NOT ONE.
 *
 * A party record stores the live level at 0x21, the level as of its last deposit at
 * 0x03, its EXP at 0x0E and its five battle stats at 0x22 — four independent stored
 * values that the game does not derive from one another while the mon is in the party.
 * Setting only 0x21 gives a level-80 Pokemon with a level-20 stat line.
 *
 * So this writes them all: level (0x21 for a party record, 0x03 for a box one), EXP =
 * the minimum for that level on `si->growth`, and — for a party record — a full stat
 * recompute with current HP clamped to the new maximum. It also refreshes 0x03 on a
 * party record, which retail leaves stale (it only refreshes it on deposit, pokered
 * home/move_mon.asm:421-427); doing it now means the mon cannot appear at its old level
 * the moment it is deposited. False if `si` is NULL or level is outside 1..100. */
bool g1e_set_level(Gen1EditMon* e, uint8_t level, const Gen1SpeciesInfo* si);

/* Recompute the five party battle stats from the record's own DVs/stat exp/level and
 * `si->base`, clamping current HP to the new maximum. A no-op for a box record, which
 * stores no battle stats (the game recomputes them on withdrawal, home/move_mon.asm:496-514).
 * Call after any DV or stat-exp edit. */
void g1e_recalc_stats(Gen1EditMon* e, const Gen1SpeciesInfo* si);

/* ------------------------------------------------------------------------- */
/* The edit, and the write                                                    */
/* ------------------------------------------------------------------------- */

/* INSERT appends after the last occupied slot and moves the terminator, which is
 * exactly what the game's own deposit does (pokered home/move_mon.asm:364-377
 * _MoveMon). It does NOT set the Pokedex owned/seen flags the way catching one does
 * (engine/pokemon/add_mon.asm:82-104) — that is save-wide state outside this box, and
 * inventing it would edit something the caller did not ask to edit. */
typedef enum {
  GEN1_OP_REPLACE = 0,   /* overwrite an occupied slot                          */
  GEN1_OP_INSERT,        /* append after the last occupied slot                 */
  GEN1_OP_DELETE,        /* remove a slot and compact all four structures       */
} Gen1OpKind;

typedef struct {
  Gen1OpKind kind;
  int        box;              /* 0..11, or GEN1_PARTY_BOX                      */
  int        slot;             /* REPLACE/DELETE: in. INSERT: OUT (the new slot) */
  const Gen1EditMon* mon;      /* REPLACE/INSERT; ignored by DELETE             */
} Gen1Op;

/* Working memory for one write: the rebuilt blob and the pre-edit blob it is checked
 * against (which is also the rollback copy). 2244 bytes, CALLER-OWNED — on the GBA take
 * it from the arena, never from the IWRAM stack. */
typedef struct {
  uint8_t neu[GEN1_BOX_BYTES];
  uint8_t old[GEN1_BOX_BYTES];
} Gen1WriteScratch;

#define GEN1_WRITE_MAX_TARGETS 2

/* Every place box `box` is stored, primary first. 0x30C0 for the party; for the open
 * box, 0x30C0 AND its bank slot; for any other box, its bank slot alone. Returns the
 * count in *n, or a refusal (VIRGIN / UNINIT / ARG) that the UI can use to grey a box
 * out before the user starts editing. `img` is needed only for bit 7 of 0x284C. */
Gen1WStatus gen1_write_targets(const uint8_t* img, const Gen1Save* s, int box,
                               uint32_t out[GEN1_WRITE_MAX_TARGETS], int* n);

/* --- the three gates, callable on their own --------------------------------
 *
 * gen1_blob_check     the STRUCTURAL gate: the invariants the game's own code depends
 *                     on. count <= capacity; the 0xFF terminator sits at exactly index
 *                     `count`; no 0xFF or 0x00 inside the used part of the species
 *                     list. It deliberately does NOT re-litigate slots this write did
 *                     not author (a save may legitimately contain a MissingNo or a
 *                     species-list mismatch, and refusing to edit box 3 because box 3
 *                     slot 9 is a glitch mon would be useless) — that job belongs to
 *                     gen1_write_verify_op, which is stricter.
 *
 * gen1_write_verify_op  the SEMANTIC gate: for every slot of every one of the four
 *                     structures, `after` must equal what `op` says it should — the
 *                     matching `before` slot, or the edited record, byte for byte. It
 *                     is written as index arithmetic over `before`, deliberately not
 *                     sharing code with the memmoves that produced `after`, so the two
 *                     can disagree. `op->slot` must be filled in (INSERT included).
 *
 * gen1_write_verify_image_box  the DESTINATION gate: the image parses, and EVERY write
 *                     target for `box` holds exactly `expect`. This is what catches a
 *                     mirror that did not land. `s` may be NULL; when it is not, its
 *                     current_box must still agree with the re-opened image.
 */
Gen1WStatus gen1_blob_check(const uint8_t* blob, int box);
bool        gen1_write_verify_op(const uint8_t* before, const uint8_t* after,
                                 int box, const Gen1Op* op);
Gen1WStatus gen1_write_verify_image_box(const uint8_t* img, uint32_t len,
                                        const Gen1Save* s, int box, const uint8_t* expect);

/* Apply `op` to a blob in place (no image, no checksums, no verification). Exposed so a
 * caller — or a test — can build a candidate and inspect it. Every reason to refuse is
 * checked BEFORE the first byte is written, so a non-OK return leaves `blob` exactly as
 * it was; there is no half-finished edit to clean up. */
Gen1WStatus gen1_blob_apply(uint8_t* blob, Gen1Op* op);

/* Checksums. The main one is the only one the game verifies (engine/menus/save.asm:42-57);
 * the fourteen box-bank bytes are written by the game but never checked, and in Guy's
 * real Red.sav ten of them are wrong. We still rewrite the bank we touched, exactly as
 * CopyBoxToOrFromSRAM does (ibid.:427-431), because "the state the game itself would
 * leave" is the safest state to leave. `bank` is 0 for boxes 1-6, 1 for boxes 7-12. */
void gen1_write_fix_main_checksum(uint8_t* img);
void gen1_write_fix_bank_checksums(uint8_t* img, int bank);

/*
 * THE WRITE. Applies `op` to `img` and re-reads it to prove it took.
 *
 *   1. re-open the image: it must still be a valid Western R/B/Y save;
 *   2. resolve every destination (refusing a doomed or uninitialised box);
 *   3. copy the primary blob into scratch->old, build the candidate in scratch->neu;
 *   4. gate it: gen1_blob_check + gen1_write_verify_op;
 *   5. if the candidate is byte-identical to the original, WRITE NOTHING and return OK
 *      — a no-op must leave the file untouched, checksums included;
 *   6. otherwise write the candidate to every destination and fix the checksums;
 *   7. verify from the image (gen1_open + gen1_write_verify_image_box + the count);
 *   8. on any failure in 7, restore the original blob everywhere, redo the checksums,
 *      and return GEN1W_ERR_VERIFY. Nothing partial is ever left behind.
 *
 * `s` is refreshed from the image on success. On INSERT, op->slot receives the slot the
 * Pokemon landed in. This function writes to memory only; persisting the image is the
 * caller's job, and on the GBA that means the existing sf_write_verified() pipeline
 * (temp -> byte-compare re-read -> rename, immutable backup first). This is the check
 * that the layout is right; that one is the check that the card is.
 */
Gen1WStatus gen1_write_apply(uint8_t* img, uint32_t len, Gen1Save* s,
                             Gen1Op* op, Gen1WriteScratch* scratch);

/* ------------------------------------------------------------------------- */
/* The generic HEADER patch (BACKLOG #49 P0)                                  */
/* ------------------------------------------------------------------------- */

/* The Gen-1 twin of gen2_write.h's g2w_write_range — money, badges, the trainer ID, the
 * player name, the dex flags, an event flag: anything that lives in the main checksummed
 * block and is NOT a Pokemon. Nothing here ever reaches a box or the party; that stays
 * gen1_write_apply's job (through gen1_blob_check + gen1_write_verify_op), so a header
 * patch can never be the thing that skips the structural gate a Pokemon edit goes
 * through.
 *
 * TWO ENTRY POINTS, one contract, P0 review D6 (an earlier draft's cap justification
 * here was false: source/gb_fields.c has fields up to 320 B — GBF_EVENT_FLAGS_BASE — in
 * this SAME slice, not "~41 bytes" as this comment used to claim):
 *
 *   gen1_write_range_ex(img, len, s, off, buf, n, snap, snap_len)
 *     The real primitive. `snap`/`snap_len` is the CALLER'S rollback buffer — this file
 *     owns no scratch of its own, so the cap on `n` is simply `n <= snap_len`, whatever
 *     the caller can supply. gb_session.c's gbs_write_field passes the session's own
 *     GBS_SCRATCH_BYTES (1152 B) buffer — already used, at a different time, as
 *     gen1_commit's box-list rollback copy; the two never run at once on the same
 *     session, so this is a reused resource, not a new allocation — which comfortably
 *     covers every field this design defines with room to spare.
 *   gen1_write_range(img, len, s, off, buf, n)
 *     A 64-byte convenience wrapper over the above, for a caller who does not want to
 *     find its own buffer (GEN1_WRITE_RANGE_MAX): supplies a small fixed local array
 *     and calls gen1_write_range_ex. A caller writing something wider refuses with
 *     GEN1W_ERR_RANGE and must call _ex directly with a bigger buffer — this file will
 *     not put a buffer bigger than GEN1_WRITE_RANGE_MAX on the IWRAM stack itself (hard
 *     convention #2), but it no longer pretends nothing needs one.
 *
 * Refuses, on EITHER entry point (docs/GEN12-PARITY-DESIGN.md §4.0, g2w_write_range's
 * refusal set translated):
 *   - an image that does not already parse as a Western R/B/Y save (GEN1W_ERR_SAVE);
 *   - `off`/`off+n` outside [0x2598, 0x3522] (GEN1_OFF_PLAYER_NAME..the tile-animation
 *     byte, i.e. GEN1_SUM_FIRST..GEN1_SUM_LAST — everything the main checksum covers,
 *     nothing past it) — this alone already excludes both SRAM banks of stored boxes;
 *   - any overlap with the party blob [GEN1_OFF_PARTY, +GEN1_PARTY_BYTES) or the open
 *     box's live copy [GEN1_OFF_CURRENT_BOX, +GEN1_BOX_BYTES) — Pokemon change ONLY
 *     through gen1_write_apply;
 *   - any overlap with GEN1_OFF_CURRENT_NO — moving the open-box number without moving
 *     its live copy silently discards a boxful at the next boot (the same reason
 *     gen2_write.c refuses its current-box-number byte);
 *   - `n` wider than the rollback buffer in play (snap_len for _ex, GEN1_WRITE_RANGE_MAX
 *     for the wrapper).
 *
 * On acceptance: snapshot `img[off..off+n)` AND the stored checksum byte into `snap`
 * (ATOMIC: both restored together, or neither), write `buf` to `img[off..off+n)`,
 * refresh the main checksum (gen1_write_fix_main_checksum — Gen 1 has no backup mirror
 * to refresh, unlike Gen 2), re-open the image, and refuse (restoring both the bytes
 * and the checksum from `snap`, GEN1W_ERR_VERIFY) unless it still parses — the same
 * snapshot-and-restore shape gb_session.c's gen1_commit already uses for a box commit,
 * applied to a plain byte range instead of a list blob. `s` is refreshed on success,
 * exactly like gen1_write_apply. A no-op (the bytes already read back as `buf`) writes
 * nothing at all, checksum included — and never touches `snap`. */
Gen1WStatus gen1_write_range_ex(uint8_t* img, uint32_t len, Gen1Save* s,
                                uint32_t off, const uint8_t* buf, uint32_t n,
                                uint8_t* snap, uint32_t snap_len);

#define GEN1_WRITE_RANGE_MAX 64u
Gen1WStatus gen1_write_range(uint8_t* img, uint32_t len, Gen1Save* s,
                             uint32_t off, const uint8_t* buf, uint32_t n);

/* ---- gen1_write_outside_sum: the Hall-of-Fame allowlist (BACKLOG #89) -----------
 *
 * gen1_write_range_ex above refuses ANYTHING outside [GEN1_SUM_FIRST, GEN1_SUM_LAST]
 * -- by design, since that window is the only span its main-checksum refresh covers.
 * sHallOfFame (GEN1_OFF_HOF, GEN1_HOF_BYTES = 4800 B) sits entirely BELOW that window,
 * so it needs its own primitive rather than a widened bound on the checksummed one --
 * widening gen1_write_range_ex's bound would let an unrelated caller reach bytes the
 * checksum has never covered and never will, for reasons that have nothing to do with
 * the Hall of Fame. This is a NAMED ALLOWLIST of exactly the one blob, not a general
 * "anything outside the sum window" door.
 *
 * `n` may be LARGER than `snap_len` (unlike gen1_write_range_ex, which refuses that
 * outright): this function chunks the write into pieces of at most `snap_len` bytes,
 * reusing the caller's rollback buffer chunk-by-chunk -- the same GBS_SCRATCH_BYTES
 * (1152 B) buffer gb_session.c already hands gen1_write_range_ex through
 * gbs_write_field, since a session never runs a field write and a Hall-of-Fame clear
 * in the same call. Each chunk gets the SAME verified-write shape as
 * gen1_write_range_ex: snapshot the chunk into `snap`, write it, re-open the image
 * (gen1_open) to confirm it still parses, and on failure restore JUST THAT CHUNK from
 * `snap` before returning GEN1W_ERR_VERIFY.
 *
 * WHAT THIS FUNCTION DOES NOT DO: restore chunks written by EARLIER, already-succeeded
 * calls to this loop. If chunk 3 of 5 fails, chunks 1-2 stay written in `img` -- this
 * function returns the error and stops there. Multi-chunk atomicity is NOT reinvented
 * here; it is the caller's existing responsibility, exactly like every other
 * multi-step gb_session edit (gb_session.h:152's own convention: "the caller keeps
 * its own pristine copy of the image and rolls back to it itself on any non-GBS_OK
 * status"). gb_hof.c's gbh_clear() calls this in a loop and returns failure on the
 * first bad chunk; the screen (pdna_gbhof.c), like every other GB edit screen, calls
 * gb_rollback() on any non-GBS_OK, which restores the WHOLE image from the pristine
 * copy it already keeps -- discarding every chunk this call wrote, good or bad. That
 * is where "restore every chunk written so far" actually happens.
 *
 * Refuses (GEN1W_ERR_RANGE) unless [off, off+n) lies FULLY inside
 * [GEN1_OFF_HOF, GEN1_OFF_HOF + GEN1_HOF_BYTES) -- the allowlist. Also refuses
 * GEN1W_ERR_SIZE/GEN1W_ERR_SAVE/GEN1W_ERR_ARG exactly like gen1_write_range_ex. Does
 * NOT touch the main checksum (GEN1_SUM_FIRST..LAST never overlaps the HoF blob, so
 * gen1_write_fix_main_checksum would recompute the same byte) -- callers that also
 * change GEN1_OFF_HOF_COUNT (inside the checksummed span) do that through the normal
 * gen1_write_range/gbs_write_field path, separately. A no-op chunk (bytes already
 * read back as `buf`) writes nothing, per chunk, same as gen1_write_range_ex. `s` is
 * refreshed after every chunk that changes the image. */
Gen1WStatus gen1_write_outside_sum(uint8_t* img, uint32_t len, Gen1Save* s,
                                   uint32_t off, const uint8_t* buf, uint32_t n,
                                   uint8_t* snap, uint32_t snap_len);

#endif /* GEN1_WRITE_H */
