#ifndef GEN3_EDIT_H
#define GEN3_EDIT_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"   /* PkMon, PK_* stat indices, pk_calc_ + pk_gender_from helpers */

/* Lossless Gen-3 Pokémon EDIT core (pure C, host-testable).
 *
 * The golden rule: editing PATCHES the decrypted block in place and re-encrypts.
 * It never rebuilds a record from a high-level struct, so a no-op edit produces a
 * BYTE-IDENTICAL record (the safety gate for all writes). The 4 substructs are
 * kept whole (12 bytes each, in canonical Growth/Attacks/EVs/Misc order), so all
 * padding and undecoded fields survive untouched.
 *
 * Stat-affecting setters (IV/EV/level/species + a nature reroll) recompute the
 * PARTY plaintext stats; box records have no stored stats (computed on display).
 */
typedef struct {
  uint8_t  raw[100];      /* full record: party 100 / box 80; plaintext fields live here */
  bool     is_party;
  uint8_t  sub[4][12];    /* substructs in CANONICAL order: 0=Growth 1=Attacks 2=EVs 3=Misc */
  uint32_t personality, otId;
} EditMon;

/* ASCII -> Gen-3 charset (inverse of gen3_decode_char); unknown -> space. */
uint8_t gen3_encode_char(char c);

void gen3_edit_load(const uint8_t* rec, bool is_party, EditMon* e);
void gen3_edit_commit(const EditMon* e, uint8_t* rec_out);   /* lossless re-encode */

/* field mutators (clamped). order args use PK_HP..PK_SPD (gen3_mon.h). */
void em_set_iv(EditMon* e, int stat, uint8_t v);             /* 0..31  */
void em_set_ev(EditMon* e, int stat, uint8_t v);             /* 0..255 */
void em_set_contest(EditMon* e, int i, uint8_t v);          /* condition i=0..5: cool/beauty/cute/smart/tough/sheen */

/* Contest RIBBON rank (BACKLOG #60) — the Misc substruct's ribbons word, bytes
 * +0x08..+0x0B of sub[3]. category = GC_COOL..GC_TOUGH (gen3_contest.h); rank is
 * clamped 0..4 (GC_RANK_NONE..MASTER) by gen3_contest.h's gc_ribbon_set (a static
 * inline there, not gen3_contest.c — see its `if (rank > GC_RANK_MASTER)` line),
 * which owns the bit layout. Cosmetic (no stat recompute), same as em_set_contest. */
void     em_set_ribbon_rank(EditMon* e, int category, uint8_t rank);
uint8_t  em_get_ribbon_rank(const EditMon* e, int category);
/* Named toggle ribbons (Champion/Winning/.../World) — same word, gen3_contest.h's
 * GC_RFLAG_*. */
void     em_set_ribbon_flag(EditMon* e, int flagbit, bool on);
bool     em_get_ribbon_flag(const EditMon* e, int flagbit);

/* The level a NEWLY CREATED Pokémon of this species should start at: 5 — the level a
 * Gen-3 egg hatches at — unless the species cannot stand there, in which case its own
 * pk_evo_floor (evolutions.h). A created Charizard is L36, not a L5 Charizard the
 * checker then has to flag. Returns 5 when source/evolutions.c is not generated
 * (pk_evo_floor = PK_EVO_NO_DATA), i.e. it fails open to the old behaviour.
 * See gen3_edit.c for WHY it is pk_evo_floor and not pk_evo_min_level. */
uint8_t gen3_build_level(uint16_t species);

/* Build a default VALID 80-byte box record for `species` at `lvl` from scratch (the
 * "create a Pokémon" flow). Pure / host-testable. otName <=7 chars; metgame 1..15.
 * `lvl` 0 means "use gen3_build_level(species)"; any explicit level is honoured as
 * given (the Gen-1/2 importer must keep the level its Pokémon really had).
 *
 * "Valid" now means it passes PokeDNA's own Legality-V2 checker unaided: real level-up
 * moves from the species' own learnset, the ability slot CreateBoxMon would derive from
 * the PID, and — for anything that can come from an egg — met level 0, which is the one
 * origin whose PID/IV pair is legitimately unconstrained. It never writes an exemption
 * marker (met 0xFD/0xFE/0xFF or the fateful ribbon bit): those would mute the checker
 * by claiming a provenance the tool cannot have. See gen3_edit.c for the full rationale
 * and tests/host_legalbuild_test.c for the all-species measurement. */
void gen3_build_mon(uint16_t species, uint8_t lvl, uint32_t pid, uint32_t otId,
                    const char* otName, uint8_t metgame, uint8_t out[80]);

/* Can this species legitimately have come out of an egg — i.e. will gen3_build_mon be
 * able to give it an origin the checker accepts? False for the legendaries and Unown,
 * for which Gen 3's only real origin is a static encounter PokeDNA ships no table for;
 * those are still built, honestly, as "met here at this level", and the caller should
 * say so rather than pretend. */
bool gen3_species_can_hatch(uint16_t species);
void em_set_species(EditMon* e, uint16_t species);           /* re-derive stats; caller re-checks gender/ability */
void em_set_item(EditMon* e, uint16_t item);
void em_set_move(EditMon* e, int i, uint16_t move);          /* also sets PP to the move's base PP,
                                                              * and clears the slot's PP Ups */
void em_set_pp(EditMon* e, int i, uint8_t pp);               /* CURRENT PP, raw (see the .c) */

/* ---- maximum PP: derived, never stored ------------------------------------
 * A Gen-3 record has no "max PP" field. The maximum is computed from the move's base PP
 * and the slot's 2-bit PP-Up count (Growth byte 8, bits 2*slot) with the game's own
 * CalculatePPWithBonus: base + base * 20 * ups / 100. So the ONLY way to raise a move's
 * maximum is to give it PP Ups, and the only reachable maxima are the four that item
 * can produce — a max above them is impossible on a cartridge and impossible here.
 *
 * em_set_ppups mirrors the item: it moves the counter AND carries current PP up by the
 * PP the bump just bought, and it clamps current PP down when the counter is lowered.
 * An empty move slot is forced to 0 Ups. */
uint8_t em_pp_max(uint16_t move, uint8_t ppBonuses, int slot);   /* slot 0..3 */
uint8_t em_get_ppups(const EditMon* e, int i);                   /* 0..3 */
void    em_set_ppups(EditMon* e, int i, uint8_t ups);            /* clamped 0..3 */
void em_set_friendship(EditMon* e, uint8_t f);
void em_set_egg(EditMon* e, bool egg);                      /* flags byte bit2 + Misc IV-word bit30 */
/* clear egg + nickname:=species + language:=English + friendship 120 + metLevel 0 + level 5
 * (the last two are what retail's hatch path writes; metLevel 0 is also how every reader,
 * PokeDNA's own included, recognises a mon as hatched). */
void em_hatch(EditMon* e);
void em_set_ability(EditMon* e, uint8_t n);                  /* 0 or 1 */
void em_set_level(EditMon* e, uint8_t level);                /* sets exp (+ party level + stats) */
void em_set_party_flag(EditMon* e, bool is_party);          /* box<->party kind (derives/drops plaintext stats) */
void em_set_nickname(EditMon* e, const char* s);            /* <=10 chars */

/* BACKLOG #224: is the UTF-8 sequence starting at `p` one of the 8 non-ASCII
 * glyphs Gen 3's own charset can actually spell -- e-acute (encode_name's own
 * "\xC3\xA9" special case) plus the 7-entry umlaut/x table encode_2byte_accent
 * owns (Ä Ö Ü ä ö ü ×)? On true, `*adv` is the sequence's byte length (always 2
 * here) and the caller should treat the glyph as representable; on false the
 * caller should treat it as unrepresentable (the SAME conservative "refuse,
 * keep the original" posture already used for '[' ']' '$'). `p[0]` must be a
 * UTF-8 lead byte (>= 0x80) -- this function is never the right call for plain
 * ASCII. Exported so a caller outside this file (gb_sidecar.c's UP-merge glyph
 * guard) routes through the SAME table gen3's own encoder uses, rather than
 * re-deriving it. */
bool gen3_utf8_storable(const char* p, int* adv);
void em_set_otname(EditMon* e, const char* s);              /* <=7 chars  */
void em_set_metloc(EditMon* e, uint8_t loc);                /* caught/met location id      */
void em_set_ball(EditMon* e, uint8_t ball);                 /* Poke Ball 1..12             */
void em_set_metlevel(EditMon* e, uint8_t lvl);              /* level it was met at (0..100)*/
void em_set_metgame(EditMon* e, uint8_t game);              /* origin game 1..15           */

/* nature / shininess / gender are all derived from the personality value, so set
 * them by rerolling the PID to match the chosen combo (want_*<0 = don't care).
 * gender_ratio is the species' ratio (data_tables). Returns false if no PID found
 * within the search budget (caller may relax constraints). */
bool em_reroll(EditMon* e, int want_nature, int want_shiny, int want_gender, uint8_t gender_ratio);
/* Set the Unown letter (0..27 = A..?) by adjusting the PID; keeps nature + shiny. */
bool em_set_unown_form(EditMon* e, int form);

/* Set the personality value to a KNOWN one. em_reroll/em_set_unown_form SEARCH for a PID;
 * this is TOLD which — the reroll history has to be able to put back exactly the PID it
 * took away. Nature, sex, ability slot, shininess and Unown's letter all move with it. */
void em_set_pid(EditMon* e, uint32_t pid);

/* The Misc substruct's 32-bit IV word EXACTLY as stored: bits 0-29 are the six 5-bit IVs
 * in PkMon order, bit 30 is the egg flag (em_set_egg) and bit 31 the ability slot
 * (em_set_ability). Exposed as ONE word because a caller that snapshots and restores an IV
 * set must round-trip all three: six em_set_iv calls quietly leave the ability bit pointing
 * at a PID that no longer exists, and drop the egg flag on the floor. */
uint32_t em_get_ivword(const EditMon* e);
void     em_set_ivword(EditMon* e, uint32_t w);

/* Decode the current edit state into a PkMon for live preview (party stats are
 * plaintext; box stats need pk_resolve by the caller). */
void em_preview(const EditMon* e, PkMon* out);

/* Lossless round-trip check for one record (load -> commit -> byte-compare). The
 * pre-write safety gate; caller runs it over a save's valid party + box mons. */
bool gen3_edit_roundtrip_ok(const uint8_t* rec, bool is_party);

#endif /* GEN3_EDIT_H */
