#ifndef ROM_TEXT_H
#define ROM_TEXT_H

#include <stdint.h>
#include "rom_map.h"   /* RomCtx / RomReadFn / ROM_BASE */

/*
 * Item / move / ability DESCRIPTIONS read out of the user's own retail ROM.
 *
 * This is phase 2 of the ROM-gated build (docs/research-rom-gated-build.md §10) and
 * it is the one that matters legally: PokeDNA's binary currently embeds 741 verbatim
 * description strings copied out of the games (309 item + 354 move + 78 ability, in
 * data_tables.c's s_itemdesc/s_mvdesc/s_abilitydesc). Reading them from the player's
 * own cartridge instead deletes that liability outright. Nothing is copied anywhere —
 * the reads are transient, the same posture the map viewer has always used.
 *
 * WHERE THE DATA LIVES (dumped and cross-checked on Guy's five retail carts; every
 * address below was found twice, once by following the header and once by an
 * independent shape-scan of the whole image, and the two agreed):
 *
 *   Emerald / FireRed / LeafGreen carry Game Freak's own index block `GFRomHeader`
 *   at ROM+0x100 (struct in pokeemerald src/rom_header_gf.c:18-94, field-identical
 *   in pokefirered src/rom_header_gf.c:10-86):
 *       +0xC4  const u8* const* abilityDescriptions   (78 pointers)
 *       +0xC8  const struct Item* items               (44 B/entry, description at +0x14)
 *       +0xFC  const u8* moveDescriptions             -- NULL in ALL retail builds
 *
 *   `moveDescriptions` being NULL is not a bug in our reader: pokeemerald sets
 *   `.moveDescriptions = NULL` (rom_header_gf.c:175) and only the fan expansion fills
 *   it in. gMoveDescriptionPointers (354 entries, [0] = Pound) therefore needs a
 *   pinned address per revision, exactly like rom_map.c's k_versions table.
 *
 *   Ruby / Sapphire predate the header entirely (ARM code sits at 0x100), so all
 *   three of their tables are pinned. Only the two R/S revisions there are dumps for
 *   are listed; every other revision fails closed rather than guessing an address.
 *
 * WHICH ROM MAY ANSWER (measured across all five carts, every string decoded):
 *   - ABILITY text is byte-identical in all five games (78/78 on every pair), so it
 *     may be borrowed from ANY registered ROM.
 *   - ITEM and MOVE text is per game group. Ruby == Sapphire exactly (0 differences)
 *     and FireRed == LeafGreen exactly (0), but Emerald differs from Ruby in 95 of
 *     349 item and 7 of 354 move descriptions, and from FireRed in 307 of 375 item
 *     and 352 of 354 move descriptions. So item/move text MUST come from a ROM in
 *     the save's own group -- `rom_text_group()` is what the caller compares.
 *     (This is the study's ROM_SAME_GROUP rule, §6.)
 *
 * Pure C (no tonc, no FatFs, no GBA headers): all I/O goes through the RomCtx's
 * RomReadFn, so tests/host_romtext_test.c runs this exact code against the real
 * cartridge dumps on the PC.
 */

typedef enum {
  ROM_TEXT_ITEM = 0,
  ROM_TEXT_MOVE,
  ROM_TEXT_ABILITY,
  ROM_TEXT_KINDS
} RomTextKind;

/* Which wording this ROM speaks. Compare against the loaded save's game group before
 * showing ROM_TEXT_ITEM / ROM_TEXT_MOVE text; ROM_TEXT_ABILITY is group-independent. */
typedef enum {
  ROM_TEXT_GROUP_RS = 0,
  ROM_TEXT_GROUP_E,
  ROM_TEXT_GROUP_FRLG,
  ROM_TEXT_GROUP_NONE
} RomTextGroup;

/* Longest retail description measured over all five carts and all 3,700+ strings is
 * 108 bytes decoded (FireRed's item text is the wordiest); 192 leaves room without
 * putting anything big on a stack. A description that would not fit FAILS rather
 * than arriving truncated. */
#define ROM_TEXT_MAX      192

typedef struct RomText {
  const RomCtx* rc;
  uint32_t table[ROM_TEXT_KINDS];   /* ROM address of each table; 0 = not available */
  uint16_t count[ROM_TEXT_KINDS];   /* one past the largest id served (see below)    */
  uint8_t  group;                   /* RomTextGroup                                  */
  int      ok;                      /* 1 = at least one table opened and validated    */
} RomText;

/* Identify the tables and validate each one by decoding a probe entry. Returns 1 if
 * ANY kind is available (check per-kind with rom_text_have), 0 if none is — which is
 * what an unlisted Ruby/Sapphire revision, a relocated hack or a truncated dump gets.
 * A kind that fails validation is switched off on its own; the others still work. */
int rom_text_open(RomText* rt, const RomCtx* rc);

/* 1 if this ROM can answer for `kind`. */
int rom_text_have(const RomText* rt, RomTextKind kind);

/* One past the largest id served: items 349 (R/S) / 375 (FR/LG) / 377 (E), abilities
 * 78, moves 355. Returns 0 when the kind is unavailable. */
int rom_text_count(const RomText* rt, RomTextKind kind);

/* RomTextGroup for this ROM, or ROM_TEXT_GROUP_NONE if it never opened. */
int rom_text_group(const RomText* rt);

/*
 * Fetch one description into `dst` (`cap` includes the NUL), decoded from the Gen-3
 * charset into what ui_ptext can draw: ASCII 32..126 plus the one two-byte UTF-8
 * sequence for e-acute (C3 A9), which is the glyph PokeDNA's font parks at code 127
 * (source/ui.c:248) and the same spelling the embedded tables already use.
 *
 * `id` is the ordinary Gen-3 id in PokeDNA's own id space:
 *   ROM_TEXT_ITEM     0..count-1   (id 0 and the unused slots really do read "?????")
 *   ROM_TEXT_ABILITY  0..count-1
 *   ROM_TEXT_MOVE     1..count-1   -- id 0 is MOVE_NONE and has NO description; the
 *                                     game's table is gMoveDescriptionPointers[id-1],
 *                                     which this function indexes for you.
 *
 * The games' text also carries a 0xFE line break (rendered as a single space, so the
 * caller keeps using ui_ptext_wrap for its own line breaking -- exactly what
 * tools/gen_data.py does when it bakes the embedded strings today) and, in principle,
 * 0xFC/0xFD control codes, which are skipped by their documented lengths.
 *
 * Returns 1 with a NUL-terminated string, or 0 -- and dst[0] = 0 -- for an id this
 * ROM does not serve, a read failure, a byte outside the charset, an unterminated
 * string, or a decode that would not fit `cap`. It never returns a half-decoded or
 * truncated string: callers fall back to their art-free copy on 0.
 */
int rom_text_get(const RomText* rt, RomTextKind kind, uint16_t id, char* dst, uint32_t cap);

#endif /* ROM_TEXT_H */
