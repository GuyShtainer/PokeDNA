#ifndef GB_FIELDS_H
#define GB_FIELDS_H

#include <stdint.h>

/* Per-game Gen-1/2 field-offset table (BACKLOG #49 P0, docs/GEN12-PARITY-DESIGN.md §4.0).
 *
 * "Gold/Silver and Crystal share no field offset past 0x2050 — a single 'Gen 2' constant
 * is a bug" (the design doc's own headline #3). This is the single place a wrong offset
 * can live: every P1-P5 screen reads a field through gbf_off(game, field), never a
 * hardcoded 0x23DB. The table backing these three accessors is GENERATED —
 * tools/gen_gbfields.py reads the four pinned .sym files plus the WRAM->SRAM save-copy
 * maps of §1.0 and emits source/gb_fields.c (git-ignored, like source/learnsets2.c;
 * regenerate with `python3 tools/gen_gbfields.py`). source/gb_fields_fallback.c is the
 * COMMITTED weak fallback: a clone that never ran the generator still links (every field
 * reports "the game lacks it" — 0 offset, 0 length — rather than a missing symbol).
 *
 * Licensing (docs/kb/licensing.md): a byte offset is a fact, not copyrightable
 * expression; this header and its generator emit our own table and our own API, never
 * decomp code, comments or identifiers.
 */

typedef enum {
  GBF_G_RED = 0,   /* stands for Red/Blue -- byte-identical save layout, §1.10 */
  GBF_G_YELLOW,
  GBF_G_GS,        /* stands for Gold/Silver -- byte-identical save layout, §1.0 */
  GBF_G_CRYSTAL,
  GBF_G_COUNT
} GbGame;

/* How to decode the bytes at gbf_off()..+gbf_len(). Multi-byte GB fields are BIG-endian
 * (the opposite of the Gen-3 side) except the two stored Gen-2 checksums, which are not
 * fields this table carries at all (gen2_save.h/gen2_write.h own those directly). */
typedef enum {
  GBFK_U8,        /* one byte                                                        */
  GBFK_U16BE,     /* 2 bytes, big-endian                                             */
  GBFK_U24BE,     /* 3 bytes, big-endian binary (Gen-2 money/coins)                   */
  GBFK_BCD24BE,   /* 3 bytes, big-endian BCD (Gen-1 money) or 2 bytes (Gen-1 coins)   */
  GBFK_TEXT,      /* GB charset name field, 0x50-terminated                          */
  GBFK_BYTES,     /* an opaque blob (a list, a record, an 8-byte options block, ...)  */
  GBFK_BITFIELD   /* a bitfield; bit n = flag/dex-entry/spawn n, per the field's own doc */
} GbFieldKind;

/* Every field this design tracks (§1.1 trainer card .. §1.8 clock). Not every game has
 * every field — gbf_off() returns 0 for one that does not (Gen 1 has no GENDER field;
 * Gen 2 has no MONEY in BCD, only MONEY_BIN; only Crystal has GS_BALL_FLAG; ...). A
 * caller must always treat a 0 offset as "not on this game", never as a real address —
 * nothing this design tracks legitimately lives at file offset 0. */
typedef enum {
  /* -- 1.1 trainer card -- */
  GBF_PLAYER_NAME = 0, GBF_TRAINER_ID, GBF_MONEY, GBF_MONEY_BIN, GBF_COINS, GBF_COINS_BIN,
  GBF_MOMS_MONEY, GBF_MOM_SAVING_FLAG, GBF_BADGES, GBF_BADGES_JOHTO, GBF_BADGES_KANTO,
  GBF_RIVAL_NAME, GBF_MOTHERS_NAME, GBF_PLAYTIME_HOURS, GBF_PLAYTIME_MAXED,
  GBF_PLAYTIME_MINUTES, GBF_PLAYTIME_SECONDS, GBF_PLAYTIME_FRAMES, GBF_GENDER, GBF_OPTIONS,
  /* BACKLOG #96 D10: wStatusFlags/wStatusFlags1 (bit 0 = STATUSFLAGS_POKEDEX_F) --
   * Gold 0x23D9, Crystal 0x23DA; Gen 1 ABSENT (gb_trainer.c's has_pokedex gate). */
  GBF_STATUS_FLAGS,
  /* -- 1.2 bag / PC item store -- */
  GBF_BAG_COUNT, GBF_BAG_BODY, GBF_KEY_ITEMS_COUNT, GBF_KEY_ITEMS_BODY,
  GBF_BALLS_COUNT, GBF_BALLS_BODY, GBF_TMHM_COUNTS, GBF_PC_COUNT, GBF_PC_BODY,
  /* -- 1.3 event flags / counters base offsets -- */
  GBF_EVENT_FLAGS_BASE, GBF_EVENT_FLAGS_BASE_G2, GBF_HIDDEN_ITEM_FLAGS,
  GBF_HIDDEN_COIN_FLAGS, GBF_TOGGLE_OBJ_FLAGS, GBF_BIKE_FLAGS, GBF_UNLOCKED_UNOWN,
  GBF_GS_BALL_FLAG, GBF_MYSTERY_GIFT_ITEM, GBF_MYSTERY_GIFT_UNLOCKED,
  /* -- 1.4 fly destinations -- */
  GBF_FLY_FLAGS, GBF_FLY_FLAGS_G2,
  /* -- 1.5 map position / warp -- */
  GBF_MAP_ID, GBF_MAP_GROUP, GBF_MAP_NUMBER, GBF_POS_X, GBF_POS_Y,
  GBF_POS_XBLOCK, GBF_POS_YBLOCK, GBF_WARP_NUMBER, GBF_LAST_MAP,
  GBF_ESCAPE_WARP, GBF_ESCAPE_GROUP, GBF_ESCAPE_NUMBER,
  GBF_BACKUP_WARP, GBF_BACKUP_GROUP, GBF_BACKUP_NUMBER,
  GBF_LAST_SPAWN_MAP, GBF_LAST_SPAWN_GROUP, GBF_LAST_SPAWN_NUMBER,
  /* -- 1.6 pokedex -- */
  GBF_DEX_OWNED, GBF_DEX_SEEN,
  /* -- 1.7 day-care -- */
  GBF_DAYCARE_FLAG, GBF_DAYCARE_NICK, GBF_DAYCARE_OT, GBF_DAYCARE_REC,
  GBF_DAYCARE_LADY_FLAG, GBF_DAYCARE_STEPS,
  GBF_DAYCARE2_NICK, GBF_DAYCARE2_OT, GBF_DAYCARE2_REC,
  GBF_DAYCARE_EGG_NICK, GBF_DAYCARE_EGG_REC,
  /* -- 1.8 Gen-2 clock ("Clock fix" analogue) -- */
  GBF_RTC_START_DAY, GBF_RTC_START_HOUR, GBF_RTC_START_MINUTE, GBF_RTC_START_SECOND,
  GBF_RTC_SNAPSHOT, GBF_RTC_DST, GBF_GAMETIME_HOURS, GBF_GAMETIME_MINUTES,
  GBF_GAMETIME_SECONDS, GBF_GAMETIME_FRAMES, GBF_CUR_DAY,

  /* P1a review D6, appended (not inserted among the others above -- these ids are
   * an API): Gen 2's wGameTimeCap, the exact analogue of Gen 1's PLAYTIME_MAXED
   * (§1.1) that gb_trainer.h used to claim did not exist for Gen 2. GS/Crystal
   * only; bit GAME_TIME_CAPPED (0) of the byte immediately before GBF_GAMETIME_HOURS. */
  GBF_GAMETIME_CAP,

  /* BACKLOG #94, appended (same API-id rule as GBF_GAMETIME_CAP above): sBoxNames, the
   * 14 x 9-byte GB-encoded box-name table. GS/Crystal only -- Gen 1's boxes are unnamed
   * (banner always "BOX n"). One field covers all 14 names (126 B); gb_boxnames.c
   * indexes into it per box rather than the generator emitting 14 separate field ids. */
  GBF_BOXNAMES,

  /* BACKLOG #86, appended (same rule): sRTCStatusFlags, the "RTC has been reset / clock
   * is unreliable" flag §1.8 documents -- SRAM bank 0, addr 0xAC60, OUTSIDE both
   * checksummed spans (design doc §1.8: "sRTCStatusFlags ... SRAM bank 0, OUTSIDE both
   * checksummed spans"), so it cannot be a D() region field the way the rest of the
   * clock block is; resolved via sram_file_off() like GBF_GENDER/GBF_GS_BALL_FLAG.
   * GS/Crystal only; Gen 1 has no RTC at all. */
  GBF_RTC_STATUS_FLAGS,

  /* P1a review D3, appended (same API-id rule as GBF_GAMETIME_CAP/GBF_BOXNAMES/
   * GBF_RTC_STATUS_FLAGS above): wEggMonOT, the day-care egg's own OT name -- without
   * this a withdrawn egg lost its OT (and, via gbd_withdraw_egg's list_species, its
   * EGG-ness). GS/Crystal only, sits between wEggMonNickname and wEggMon (the record)
   * in both games' .sym, same as GBF_DAYCARE_OT does for the boarded mon. */
  GBF_DAYCARE_EGG_OT,

  /* BACKLOG #88, appended (same API-id rule as the fields above): wSafariSteps
   * (Gen 1 only -- Gen 2 has no Safari-Zone step counter) and wLuckyNumberShowFlag
   * (Gen 2 only -- Gold/Silver + Crystal's "today's lucky-number radio segment
   * already heard" byte). See docs/briefs/88-gb-flags-brief.md's data section. */
  GBF_SAFARI_STEPS,
  GBF_LUCKY_NUMBER_SHOW_FLAG,

  GBF_FIELD_COUNT
} GbField;

/* File offset of `f` on game `g`, or 0 if `g` does not have that field (a genderless
 * Gen-1 save asking for GBF_GENDER, a Gen-2 save asking for GBF_MONEY instead of
 * GBF_MONEY_BIN, ...). */
uint32_t gbf_off(GbGame g, GbField f);
/* Size in bytes of `f` on `g` (may differ per game — GBF_EVENT_FLAGS_BASE is 320 B on
 * Gen 1, GBF_EVENT_FLAGS_BASE_G2 is 256 B on Gen 2). 0 iff gbf_off() would be 0. */
uint16_t gbf_len(GbGame g, GbField f);
/* How to decode the bytes. Meaningless (returns GBFK_U8) if gbf_off() is 0. */
GbFieldKind gbf_kind(GbGame g, GbField f);
/* Our own debug/UI label for `f` (its C identifier, e.g. "MONEY") -- game-independent,
 * for logging and picker rows; never a decomp string. */
const char* gbf_field_name(GbField f);

#endif /* GB_FIELDS_H */
