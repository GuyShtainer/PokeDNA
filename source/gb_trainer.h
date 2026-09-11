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
 *     span (§1.9). BACKLOG #96 Kris: still VIEW-ONLY through this UI-facing
 *     gbt_write() (no live-editor row writes it), but WRITABLE by
 *     tests/host_gbsurgery_tool.c's `--op gender 0|1` — a raw gbs_write_field()
 *     poke of the same plain byte this comment used to say "would need a path
 *     this slice does not build", now built as a gate-only tool, not a UI field.
 *   - Pokedex owned/seen counts — popcounts of the dex bitfields, explicitly
 *     read-only per this slice's brief.
 */

/* Gen 1 has an 8-bit hour byte + a separate "maxed" byte (wPlayTimeMaxed). Gen 2 has
 * a 16-bit hour count, but IS ALSO capped: wGameTimeCap (GBF_GAMETIME_CAP, P1a review
 * D6 -- gb_trainer.h used to claim Gen 2 had no maxed flag at all) sits one byte
 * before the hour field in both pokegold and pokecrystal, bit GAME_TIME_CAPPED (0),
 * set once the hour count would overflow (pokegold/pokecrystal home/game_time.asm) --
 * the exact analogue of Gen 1's own flag, just a bitfield instead of a whole byte.
 * One struct carries both shapes: `maxed` reads/writes GBF_PLAYTIME_MAXED whole for
 * Gen 1 and bit 0 of GBF_GAMETIME_CAP for Gen 2. */
typedef struct {
  uint16_t hours;   /* Gen 2: gbt_write() clamps to 999 -- the field is 16 bits wide
                     * but nothing else in the design ever cites Gen 2 play time past
                     * 3 digits, and 999 matches this struct's own on-screen budget */
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
  /* P1a review D7: wMomSavingMoney is not a bool -- pokegold/pokecrystal
   * ram_constants.asm defines it as three INDEPENDENT flag bits (mask 0x07:
   * MOM_SAVING_SOME/HALF/ALL_MONEY_F at bits 0/1/2) plus a separate MOM_ACTIVE_F
   * at bit 7; bits 3-6 are undocumented. mom_saving_bits carries the low three
   * bits exactly as stored (0 = not saving; the game sets one of them per the
   * amount the player chose at the counter, but nothing here assumes they are
   * mutually exclusive); mom_active is bit 7. gbt_write() preserves bits 3-6
   * untouched (read-modify-write, same discipline the old single-bit code used). */
  uint8_t  mom_saving_bits;           /* Gen 2 only; bits 0-2, mask 0x07              */
  bool     mom_active;                /* Gen 2 only; bit 7                            */

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

  /* BACKLOG #96 D10: STATUSFLAGS_POKEDEX_F (GBF_STATUS_FLAGS bit 0, Gen 2 only --
   * Gen 1 has no equivalent gate on its own card). false on a Gen-1 read (the field
   * is ABSENT there, same posture as has_gender/has_mother) so the Gen-2 card's own
   * gate never has to special-case Gen 1 separately. */
  bool     has_pokedex;               /* Gen 2 only                                  */
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
 * A FIELD THAT DID NOT CHANGE IS NEVER REWRITTEN (P1a review D1/D3/D5, re-verified
 * D3) — this is not an optimisation, it is a correctness requirement
 * (docs/GEN12-PARITY-DESIGN.md §5.1): the player name is compared, DECODED, against
 * what a fresh gbt_read of this session would return right now, and the
 * encode/write is skipped entirely when they already match. Every CAPPED field --
 * money, coins, mom's money, and both generations' play-time hours -- gets the same
 * untouched-is-untouched treatment via set_u_capped_unless_same() (source/
 * gb_trainer.c): the field's own CURRENT RAW value is compared against the wanted
 * RAW value FIRST, and only clamped on the way OUT once a real change is already
 * known to be happening -- never the other way around (comparing raw-vs-already-
 * clamped silently rewrites any over-cap stored value to the cap on any unrelated
 * edit; this was shipped once and is the specific defect re-verify D3 caught and
 * fixed). A round-trip that never meant to touch money must not (a) silently
 * rewrite a stored name's post-terminator tail to 0x50 filler, or (b) silently
 * clamp a stored value that happens to already be over its cap down to that cap.
 * money/coins are also skipped outright when money_ok/coins_ok is false (an
 * unreadable field is left exactly as found, never overwritten with the zero
 * gbt_read defaulted it to).
 *
 * Refuses a player name over GB_OT_GLYPHS glyphs or one the target generation's
 * charset cannot store exactly (GBS_ERR_ARG) before a single byte moves. Calls
 * gbs_finish() once at the end of the batch. On any non-GBS_OK return the image
 * may already hold SOME of the intended edits — same contract gb_session.h
 * documents for gbs_write_field/gbs_commit_list: the caller rolls back the whole
 * image from its own pristine copy, this layer does not. */
GbsStatus gbt_write(GbSession* s, const GbTrainer* in);

#endif /* GB_TRAINER_H */
