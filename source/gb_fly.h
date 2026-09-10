#ifndef GB_FLY_H
#define GB_FLY_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame, GbField, gbf_off/len/kind                          */
#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish */

/* gb_fly — pure-C fly-destination (visited-towns) bitfield core over the generated
 * field table (BACKLOG #90, docs/GEN12-PARITY-DESIGN.md §1.4). Mirrors the shape of
 * gen3_daycare.h's Fly screen, one bit per destination.
 *
 * WHICH FIELD, per generation:
 *   Gen 1 (Red/Blue/Yellow) — GBF_FLY_FLAGS, wTownVisitedFlag, 2 bytes / 16 bits.
 *   Gen 2 (GS/Crystal)      — GBF_FLY_FLAGS_G2, wVisitedSpawns, 4 bytes / 32 bits
 *                             (Kanto + Johto together, per §1.4).
 * Both generations HAVE a fly-flags field (unlike the clock, §1.8, or day-care's
 * second slot, §1.7) — there is no "not applicable" case here.
 *
 * WHAT A "DESTINATION INDEX" MEANS. This core does not curate a named list of fly-able
 * towns (that is BACKLOG #91's map-viewer job, "own design doc" per the backlog entry,
 * and a Town-Map-visited bit is not necessarily the same set as a fly-able spawn point
 * either way — the two decomps do not document that distinction identically). It
 * exposes the bitfield BY BIT INDEX, 0..gbfy_count(game)-1 (== gbf_len(game,field)*8),
 * matching this module's own brief ("set/clear per destination index with the per-game
 * destination count"); a screen that wants names supplies its own table and asks this
 * core only to flip the bit.
 *
 * Endianness: the field decodes/encodes MSB-first byte order (byte 0 holds bits
 * 7..0 of destinations 0..7, byte 1 holds 15..8, ...) — the same big-endian
 * convention every other multi-byte GB field in this table uses (gb_fields.h:
 * "Multi-byte GB fields are BIG-endian"), and the one gb_trainer.c's decode_u/encode_u
 * already establish for GBFK_BITFIELD-adjacent multi-byte reads. Bit order WITHIN a
 * byte is bit 0 = the byte's own LSB, matching how a raw wTownVisitedFlag byte is
 * bit-tested in the decomp (`bit n, [hl]` on the untouched byte, LSB-first per RGBDS'
 * own `bit` convention) — destination 0 is bit 0 of byte 0, not bit 7. */

/* Bits present for this game's fly-flags field (0 on a malformed game/session). */
int gbfy_count(GbGame game);

GbGame gbfy_game(const GbSession* s);

/* Read the whole bitfield into `bits`, MSB-first byte order, LSB-first within each
 * byte (see the header note). `cap` must be >= gbf_len(game, field) or the read is
 * refused (false) rather than truncated silently. False on a malformed session too. */
bool gbfy_read(const GbSession* s, uint8_t* bits, int cap);

/* Is destination `index` visited? False on an out-of-range index or a read failure --
 * never distinguishable from "not visited" by design (a caller that needs to tell
 * those apart uses gbfy_read() and gbfy_count() directly). */
bool gbfy_get(const GbSession* s, int index);

/* Set/clear ONE bit, writing only the one byte it lives in, and only when that byte's
 * value actually changes (same "untouched is untouched" discipline every other core in
 * this family uses). GBS_ERR_ARG on a bad index or malformed session, before any byte
 * moves. Calls gbs_finish() only when a write actually happened. */
GbsStatus gbfy_set(GbSession* s, int index, bool visited);

#endif /* GB_FLY_H */
