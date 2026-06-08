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
void em_set_species(EditMon* e, uint16_t species);           /* re-derive stats; caller re-checks gender/ability */
void em_set_item(EditMon* e, uint16_t item);
void em_set_move(EditMon* e, int i, uint16_t move);          /* also sets PP to the move's base PP */
void em_set_pp(EditMon* e, int i, uint8_t pp);
void em_set_friendship(EditMon* e, uint8_t f);
void em_set_ability(EditMon* e, uint8_t n);                  /* 0 or 1 */
void em_set_level(EditMon* e, uint8_t level);                /* sets exp (+ party level + stats) */
void em_set_party_flag(EditMon* e, bool is_party);          /* box<->party kind (derives/drops plaintext stats) */
void em_set_nickname(EditMon* e, const char* s);            /* <=10 chars */
void em_set_otname(EditMon* e, const char* s);              /* <=7 chars  */

/* nature / shininess / gender are all derived from the personality value, so set
 * them by rerolling the PID to match the chosen combo (want_*<0 = don't care).
 * gender_ratio is the species' ratio (data_tables). Returns false if no PID found
 * within the search budget (caller may relax constraints). */
bool em_reroll(EditMon* e, int want_nature, int want_shiny, int want_gender, uint8_t gender_ratio);

/* Decode the current edit state into a PkMon for live preview (party stats are
 * plaintext; box stats need pk_resolve by the caller). */
void em_preview(const EditMon* e, PkMon* out);

/* Lossless round-trip check for one record (load -> commit -> byte-compare). The
 * pre-write safety gate; caller runs it over a save's valid party + box mons. */
bool gen3_edit_roundtrip_ok(const uint8_t* rec, bool is_party);

#endif /* GEN3_EDIT_H */
