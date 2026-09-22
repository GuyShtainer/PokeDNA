#ifndef GEN3_MON_H
#define GEN3_MON_H

#include <stdint.h>
#include <stdbool.h>

/* Full-fidelity Gen-3 Pokémon decode (pure C, host-testable). Unlike
 * gen3_save.c's gen3_read_live_party*, this keeps EVERYTHING the viewer needs:
 * IVs, the full 6-EV spread, moves+PP, OT, met data, ability slot, shininess,
 * contest stats and ribbons. Works on both the 100-byte party record (which
 * also carries plaintext battle stats + level) and the 80-byte PC-box record
 * (level + stats must be COMPUTED — see pk_resolve / pk_calc_*).
 *
 * All six-element arrays use the SAVE-NATIVE order: HP, Atk, Def, Spe, SpA, SpD.
 */

enum { PK_HP = 0, PK_ATK, PK_DEF, PK_SPE, PK_SPA, PK_SPD, PK_NSTATS };

/* PkMon.nameFlags bits (BACKLOG #217) -- a field's DECODED string carries a
 * literal '?' standing in for a real glyph (gender sign / e-acute) that could
 * not fit the fixed-width nickname[11]/otName[8] buffer. */
#define PK_NAME_NICK_DEGRADED 0x01u
#define PK_NAME_OT_DEGRADED   0x02u

typedef struct {
  uint16_t species;           /* INTERNAL Gen-3 index (1..411); 0 = none        */
  char     nickname[11];      /* decoded, 10 chars + NUL                         */
  char     otName[8];         /* decoded, 7 chars + NUL                          */
  uint32_t personality;
  uint32_t otId;              /* low 16 = public TID, high 16 = secret SID       */
  uint32_t experience;
  uint8_t  level;             /* party: plaintext; box: computed (0 until set)   */
  uint8_t  nature;            /* personality % 25                                */
  uint8_t  abilityNum;        /* 0 or 1 (which of the species' two abilities)    */
  uint8_t  friendship;
  uint8_t  ppBonuses;         /* 2 bits per move                                 */
  bool     isShiny, isEgg, isBadEgg, isParty;
  uint8_t  ivs[PK_NSTATS];    /* 0..31                                           */
  uint8_t  evs[PK_NSTATS];    /* 0..255                                          */
  uint16_t evSum;
  uint16_t moves[4];
  uint8_t  pp[4];             /* current PP                                      */
  uint16_t heldItem;
  uint8_t  pokerus, metLocation, metLevel, metGame, pokeball, otGender;
  uint8_t  language;          /* record byte 0x12: 1 JP, 2 EN, 3 FR, 4 IT, 5 DE, 7 ES */
  uint8_t  contest[6];        /* cool, beauty, cute, smart, tough, sheen         */
  uint32_t ribbons;
  uint16_t stats[PK_NSTATS];  /* party: plaintext; box: computed                 */
  uint8_t  gender;            /* 0=M, 1=F, 2=genderless (filled by pk_resolve)   */
  uint8_t  form;              /* Unown letter 0..27 (A..?), else 0               */
  /* BACKLOG #217: set by decode_name (source/gen3_mon.c) when a 2/3-byte glyph
   * (the gender sign, or e-acute -- BACKLOG #216) ran out of room in the FIXED
   * nickname[11]/otName[8] field and fell back to a literal '?' in that field's
   * DECODED string. DO NOT widen nickname/otName to fit the worst case: PkMon is
   * copied into g_box[30] (pdna_box.c) and every other per-slot array this app
   * has, all EWRAM, and this project's whole EWRAM budget was 912 B free at the
   * time this bit was added -- a wider PkMon was priced at ~1.1 KB, more than the
   * entire remaining budget. This byte sits in what was PkMon's own trailing
   * padding (host: the 6 bytes between `form` and the 8-byte-aligned `raw`;
   * ON THE GBA, which is what the EWRAM budget counts, sizeof(PkMon) is 112 with
   * `form`@105 and `raw`@108 -- so this byte took one of TWO spare bytes and
   * exactly ONE is left. A second flag byte still fits; a third moves sizeof and
   * costs 30 B per g_box slot. tests/host_gen3_codec_lossy_test.c and every
   * existing PkMon array site were checked against this; if sizeof(PkMon) ever
   * moves off its expected value, something about that padding assumption broke
   * and needs re-deriving, not just re-measuring. */
  uint8_t  nameFlags;
  const uint8_t* raw;         /* back-ref to the 80/100-byte record (edit later).
                               * NULL = the caller's copy is gone; never dereference.
                               * pk_decode_mon() always sets this to whatever buffer it
                               * was handed, even a stack scratch buffer the caller is
                               * about to discard (gen3_edit.c's em_preview, pdna_main.c's
                               * app_create_mon) -- those callers null it back out right
                               * after decoding, on purpose (BACKLOG #46). */
} PkMon;

/* Decode one record. is_party => 100-byte (plaintext stats/level); else 80-byte
 * box record. Returns false for a truly empty slot; returns true for real mons
 * AND eggs/bad-eggs (with isEgg/isBadEgg set) so the viewer can surface them. */
bool pk_decode_mon(const uint8_t* mon, bool is_party, PkMon* out);

/* Decode a raw Gen-3 name field (nickname or otName) into UTF-8, gender signs and
 * all -- the same decoder pk_decode_mon uses for PkMon.nickname/otName, exposed so
 * a caller that only has raw bytes (not a whole decoded record) can spelling-
 * compare what actually got written. `maxlen` is the field's Gen-3 byte count (10
 * nickname / 7 otName); `outcap` is out's real capacity in bytes (a gender sign
 * costs 3 UTF-8 bytes for 1 input byte, so this must be the buffer's own size, not
 * maxlen). Always NUL-terminates within outcap. */
void gen3_decode_name(char* out, int outcap, const uint8_t* src, int maxlen);

/* BACKLOG #224: public wrapper around decode_name's own 7-entry umlaut/x table
 * (Ä Ö Ü ä ö ü ×, pokeemerald charmap.txt F1-F6/B9), so a caller outside this
 * file (xfer_rec.c's DOWN-merge glyph guard) can ask "does raw Gen-3 byte `b`
 * spell one of these seven glyphs" WITHOUT re-deriving the table. Returns false
 * (out2 untouched) for any other byte, including 0x1B (e-acute) -- that one is
 * its own single-code special case in decode_name, not part of this table; a
 * caller that also needs it checks `b == 0x1B` itself, same as decode_name does. */
bool gen3_decode_2byte_accent(uint8_t b, char out2[2]);

/* Read the live party from a reassembled SaveBlock1. FRLG moved the party block
 * to the start of SaveBlock1 (count@0x034/data@0x038) vs R/S/E (0x234/0x238). */
int  pk_read_party(const uint8_t* sb1, bool frlg, PkMon out[6]);

/* Auto-detect the party layout (R/S/E 0x234/0x238 vs FRLG 0x034/0x038) by
 * decoding at both offsets and keeping whichever yields valid mons. Robust where
 * SaveBlock2-size game detection is ambiguous. Sets *is_frlg if non-NULL. */
int  pk_read_party_auto(const uint8_t* sb1, PkMon out[6], bool* is_frlg);

/* Derivations (pure, no data tables). */
uint8_t pk_nature(uint32_t personality);
bool    pk_is_shiny(uint32_t personality, uint16_t tid, uint16_t sid);
uint8_t pk_gender_from(uint32_t personality, uint8_t gender_ratio); /* 0xFF genderless, 0xFE F, 0x00 M */
uint8_t pk_unown_form(uint32_t personality);                       /* 0..27 = A..Z ! ? */
/* Deoxys forme is version-based, not stored per-mon. The app sets the display forme for the
 * loaded save (0 Normal / 1 Attack / 2 Defense / 3 Speed); pk_decode_mon tags Deoxys with it. */
void pk_set_deoxys_form(int f);
int  pk_get_deoxys_form(void);

/* Gen-3 stat formulas (for box mons + cross-checking party plaintext).
 * nature_mod: +1 boosted (×1.1), -1 hindered (×0.9), 0 neutral. */
uint16_t pk_calc_hp(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level);
uint16_t pk_calc_stat(uint8_t base, uint8_t iv, uint8_t ev, uint8_t level, int nature_mod);

#endif /* GEN3_MON_H */
