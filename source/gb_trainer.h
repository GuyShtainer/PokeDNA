#ifndef GB_TRAINER_H
#define GB_TRAINER_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame, GbField, gbf_off/len/kind                          */
#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish */
#include "gb_edit.h"      /* GB_NAME_BYTES, GB_OT_GLYPHS, GB_GLYPH_MAX                   */

/* gb_trainer — pure-C trainer-card core over the generated field table
 * (BACKLOG #49 P1a, docs/GEN12-PARITY-DESIGN.md §1.1 + §4.1 P1).
 *
 * Shape mirrors gen3_trainer.h on purpose: a read that fills a plain struct, and a
 * write that goes through the session's field-write primitive (gbs_write_field /
 * gbs_finish, source/gb_session.h) exactly the way P1b's screen (pdna_gbtrainer.c,
 * not part of this slice) will call it. Every offset is looked up through
 * gbf_off/gbf_len/gbf_kind — never a literal — so a wrong constant can only ever
 * live in the one generated table (source/gb_fields.c).
 *
 * SCOPE OF THIS SLICE (view vs. edit). gbt_write() only ever touches the fields the
 * design's P1 row lists as "get/set": player name, trainer ID, money, coins, mom's
 * money + saving flag, badges, play time. Three fields gbt_read() fills are
 * DELIBERATELY VIEW-ONLY here and gbt_write() never writes them:
 *   - rival name / mother's name — not part of the P1 slice's edit list.
 *   - gender (Crystal's sCrystalData bit 0) — the design settles this explicitly
 *     ("P1's trainer-card gender row is display-only for Crystal (0x3E3D) — the
 *     *mon* gender work is g1's", §4.1). It also lives OUTSIDE every checksummed
 *     span (§1.9), so writing it would need a plain-byte path this slice does not
 *     build; refused by omission rather than half-built.
 *   - Pokedex owned/seen counts — popcounts of the dex bitfields, explicitly
 *     read-only per this slice's brief.
 */

/* Gen 1 has one byte + a boolean "maxed" flag; Gen 2 (both GS and Crystal) has a
 * 16-bit hour count and no maxed flag at all (the design's own table, §1.1) — one
 * struct carries both shapes; `maxed` is always false when read from a Gen-2 save. */
typedef struct {
  uint16_t hours;
  bool     maxed;
  uint8_t  minutes;
  uint8_t  seconds;
  uint8_t  frames;
} GbPlayTime;

typedef struct {
  uint8_t  name_raw[GB_NAME_BYTES];   /* raw GB-encoded field, 0x50-padded           */
  char     name[GB_TEXT_MAX];         /* UTF-8 decoded, <= GB_OT_GLYPHS glyphs       */
  uint16_t trainer_id;

  uint32_t money;                     /* decoded RAW (may exceed the cap if the stored
                                        * bytes do); the WRITE caps to 999999 only when
                                        * the value actually changes, see gbt_write     */
  bool     money_ok;                  /* false: Gen-1 BCD decode failed (non-decimal
                                        * nibble) -- the stored bytes are not readable
                                        * as a number; P1b must show "?", and gbt_write
                                        * never touches this row while it is false      */
  uint16_t coins;                     /* decoded RAW; the WRITE caps to 9999 only when
                                        * the value actually changes, see gbt_write     */
  bool     coins_ok;                  /* same meaning as money_ok, for the coins field */

  bool     has_mom;                   /* Gen 2 only                                  */
  uint32_t moms_money;                /* Gen 2 only; capped at 999999                */
  bool     mom_saving;                /* Gen 2 only                                  */

  uint8_t  badges;                    /* Gen 1: 8 badge bits, one byte               */
  uint8_t  badges_johto;              /* Gen 2                                       */
  uint8_t  badges_kanto;              /* Gen 2                                       */

  uint8_t  rival_name_raw[GB_NAME_BYTES];
  char     rival_name[GB_TEXT_MAX];   /* VIEW-ONLY, see the scope note above          */

  bool     has_mother;                /* Gen 2 only                                  */
  uint8_t  mothers_name_raw[GB_NAME_BYTES];
  char     mothers_name[GB_TEXT_MAX]; /* VIEW-ONLY                                    */

  GbPlayTime playtime;

  bool     has_gender;                /* Crystal only                                */
  uint8_t  gender;                    /* 0 male, 1 female; VIEW-ONLY, see scope note */

  uint16_t dex_owned;                 /* popcount; VIEW-ONLY                         */
  uint16_t dex_seen;                  /* popcount; VIEW-ONLY                         */
} GbTrainer;

/* Which of the four field-table games (GBF_G_RED/YELLOW/GS/CRYSTAL) this open
 * session actually is. Gen 1 always answers GBF_G_RED — Yellow's save is
 * byte-identical to Red/Blue's (§1.10), so the field table carries one Gen-1 row
 * and this never needs to tell them apart. Gen 2 reads the version g2w_begin
 * already latched at gbs_open() time (s->g2w.sv.version). */
GbGame gbt_game(const GbSession* s);

/* Mirrors gbf_off(game, field) != 0, so a screen can hide a row this game does not
 * have without repeating the field table's own rule. */
bool gbt_field_present(GbGame game, GbField field);

/* Fill `out` from the session's resident image. False only on a malformed session
 * (never opened) or a read that could not even reach the player-name field — the
 * one field every supported game has. Every other field defaults to zero and is
 * left absent (has_mom/has_mother/has_gender false, badges fields left at 0 for a
 * game generation that lacks them) rather than causing a hard failure — including
 * an unreadable money/coins field (P1a review D4): a Gen-1 save whose money bytes
 * are not valid BCD sets money_ok/coins_ok false and money/coins to 0 rather than
 * failing the whole read, exactly as this header always documented for "every
 * other field". */
bool gbt_read(const GbSession* s, GbTrainer* out);

/* Write the EDITABLE subset of `in` back into the session (see the scope note
 * above for what is and is not written).
 *
 * A FIELD THAT DID NOT CHANGE IS NEVER REWRITTEN (P1a review D1/D3/D5) — this is
 * not an optimisation, it is a correctness requirement (docs/GEN12-PARITY-DESIGN.md
 * §5.1): the player name is compared, DECODED, against what a fresh gbt_read of
 * this session would return right now, and the encode/write is skipped entirely
 * when they already match. Money and coins get the same untouched-is-untouched
 * treatment: each is compared against its own CURRENT raw value before any cap is
 * applied, and only clamped-then-written when it actually differs; a round-trip
 * that never meant to touch money must not (a) silently rewrite a stored name's
 * post-terminator tail to 0x50 filler, or (b) silently clamp a stored value that
 * happens to already be over 999999/9999 down to the cap. money/coins are also
 * skipped outright when money_ok/coins_ok is false (an unreadable field is left
 * exactly as found, never overwritten with the zero gbt_read defaulted it to).
 *
 * Refuses a player name over GB_OT_GLYPHS glyphs or one the target generation's
 * charset cannot store exactly (GBS_ERR_ARG) before a single byte moves. Calls
 * gbs_finish() once at the end of the batch. On any non-GBS_OK return the image
 * may already hold SOME of the intended edits — same contract gb_session.h
 * documents for gbs_write_field/gbs_commit_list: the caller rolls back the whole
 * image from its own pristine copy, this layer does not. */
GbsStatus gbt_write(GbSession* s, const GbTrainer* in);

#endif /* GB_TRAINER_H */
