#ifndef BANK_CELL_H
#define BANK_CELL_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"     /* GbEditMon, GB_GEN1/GB_GEN2, GB_NAME_BYTES, GB_LIST_TERMINATOR */
#include "gen1_write.h"  /* G1R_BOXLEVEL, G1R_LEVEL -- the party->box level sync           */
#include "gen2_save.h"   /* G2_BOX_ENTRY                                                  */
#include "gen12_convert.h" /* Gb12Mon -- bc_view()'s output shape                         */

/* The tagged 80-byte native Bank cell ("GBC1"), pure C, no tonc/sys.h, no FatFs.
 *
 * ZERO CALLERS in this slice (BACKLOG #150 S150-1) -- the codec only. Nothing in the
 * shipped build changes behaviour: this file is linked but unreferenced.
 *
 * Design: docs/BANK-CROSSGEN-DESIGN.md SS11.1 (the layout table below, verbatim) and
 * SS11.12 (the function signatures: bc_pack takes origin_game/rtc_epoch/bank_serial as
 * explicit caller-supplied values rather than deriving them, because a bare per-record
 * codec has no access to the Bank's meta.bank_serial counter or the game's RTC -- that
 * plumbing belongs to the caller in a later slice). `flags` stays an explicit bc_pack
 * argument too: two of its five bits (has-xfer-record, queued-for-PC) are Bank/ledger
 * state a GbEditMon cannot carry, so bank_cell.c never tries to derive any of them and
 * leaves the whole byte to the caller.
 *
 * A cell is a Gen-3 BoxPokemon-shaped 80-byte record whose first four bytes are the
 * magic "GBC1" -- occupying the Gen-3 `personality` field -- and whose next four are an
 * FNV-1a-32 identity hash -- occupying the Gen-3 `otId` field. Nothing else about the
 * 80-byte stride, the box file layout, or any of the 80-byte copy paths in pdna_box.c /
 * pdna_bank.c / gen3_clip.c changes.
 *
 * ---- SS11.1's byte layout, exactly -----------------------------------------------
 * Off  Len  Field
 *   0    4  magic 'G','B','C','1'                    (Gen-3 `personality` field)
 *   4    4  ident32 = bc_ident32() over the immutable identity span (Gen-3 `otId` field)
 *   8    1  gen (1 or 2; never 0)
 *   9    1  rec_len (33 Gen-1 box / 32 Gen-2 box)
 *  10    1  list_species (Gen-2 Egg = 0xFD; never 0xFF, which gb_commit refuses)
 *  11    1  flags: b0 came-from-party, b1 has-xfer-record, b2 queued-for-PC, b3 egg,
 *              b4 holds-item, b5 COPY (S150-12: the GB original still exists; permanent,
 *              never cleared while the cell lives)
 *  12    7  rec[0..6]
 *  19    1  CONSTANT 0x01 -- the Gen-3 BoxPokemon flags byte. NEVER payload. Forces
 *              pk_decode_mon's plaintext isBadEgg bit, so pk3_validate() rejects every
 *              packed cell in a pre-#150 build (the old-build guard).
 *  20   26  rec[7..32] (Gen-1 uses all 26; Gen-2 leaves rec[32] zero)
 *  46   11  otname[11], raw GB bytes
 *  57   11  nick[11], raw GB bytes
 *  68    1  origin_game (0 unknown, 1 RED, 2 BLUE, 3 YELLOW, 4 GOLD, 5 SILVER, 6 CRYSTAL)
 *  69    4  rtc_epoch, u32 LE -- informational only, OUTSIDE the ident32 span (re-stamped
 *              on every re-pack, G-M4)
 *  73    4  bank_serial, u32 LE -- monotonic, allocated by the caller from bank.meta;
 *              makes ident32 unique BY CONSTRUCTION across two packs of one mon
 *  77    3  reserved, zero
 *
 * bc_ident32() hashes bytes 8..10, 12..68 and 73..76 -- i.e. everything from byte 8
 * onward EXCEPT the mutable flags byte (11) and rtc_epoch (69..72), which is what lets a
 * flags-only or epoch-only re-pack of the SAME mon (same bank_serial) keep ident32
 * unchanged (G-M4), while a fresh bank_serial always changes it. NOTE: SS11.1's prose
 * parenthetically calls this span "61 B"; the three explicit ranges it names sum to 64 B
 * (3 + 57 + 4). This header follows the explicit ranges (also the ones consistent with
 * "excludes byte 11 and 69..72" read against the full 8..79 span minus the 3 reserved
 * bytes at 77..79) -- the "(61 B)" figure is a documentation-only arithmetic slip, not a
 * different span; see the S150-1 delivery report for the full reasoning.
 */

#define BC_CELL_BYTES 80u

/* ---- byte offsets, exactly SS11.1's table --------------------------------------- */
#define BC_OFF_MAGIC        0u   /* 4 */
#define BC_OFF_IDENT32      4u   /* 4 */
#define BC_OFF_GEN          8u   /* 1 */
#define BC_OFF_REC_LEN      9u   /* 1 */
#define BC_OFF_LIST_SPECIES 10u  /* 1 */
#define BC_OFF_FLAGS        11u  /* 1 */
#define BC_OFF_REC_LO       12u  /* 7  -- rec[0..6]  */
#define BC_OFF_OLDBUILD     19u  /* 1  -- constant 0x01 */
#define BC_OFF_REC_HI       20u  /* 26 -- rec[7..32] */
#define BC_OFF_OTNAME       46u  /* 11 */
#define BC_OFF_NICK         57u  /* 11 */
#define BC_OFF_ORIGIN_GAME  68u  /* 1  */
#define BC_OFF_RTC_EPOCH    69u  /* 4  */
#define BC_OFF_BANK_SERIAL  73u  /* 4  */
#define BC_OFF_RESERVED     77u  /* 3  */

#define BC_MAGIC_0 'G'
#define BC_MAGIC_1 'B'
#define BC_MAGIC_2 'C'
#define BC_MAGIC_3 '1'

/* The Gen-3 BoxPokemon flags byte, held constant so a pre-#150 build's plaintext
 * isBadEgg read (gen3_mon.c:79-80) is true for every packed cell, 100% of the time. */
#define BC_OLDBUILD_BYTE 0x01u

/* flags byte (offset 11) bits */
#define BC_FLAG_FROM_PARTY   0x01u  /* b0 */
#define BC_FLAG_HAS_XFER_REC 0x02u  /* b1 */
#define BC_FLAG_QUEUED_PC    0x04u  /* b2 */
#define BC_FLAG_EGG          0x08u  /* b3 */
#define BC_FLAG_HOLDS_ITEM   0x10u  /* b4 */
#define BC_FLAG_COPY         0x20u  /* b5 -- a COPY whose GB original still exists; DOWN writes NO ledger entry (S150-12, BANK-CROSSGEN-DESIGN.md SS11.7 G-L3) */

/* origin_game values (offset 68) */
#define BC_ORIGIN_UNKNOWN 0u
#define BC_ORIGIN_RED     1u
#define BC_ORIGIN_BLUE    2u
#define BC_ORIGIN_YELLOW  3u
#define BC_ORIGIN_GOLD    4u
#define BC_ORIGIN_SILVER  5u
#define BC_ORIGIN_CRYSTAL 6u

/* ---- shape guards: the codec is only correct if these hold ----------------------- */
_Static_assert(BC_CELL_BYTES == 80u, "the Bank cell stride is fixed at 80 bytes");
_Static_assert(GEN1_BOX_REC_BYTES == 33, "Gen-1 box record must be 33 bytes (7+26)");
_Static_assert(G2_BOX_ENTRY == 32, "Gen-2 box record must be 32 bytes (7+25, rec[32] pad)");
_Static_assert(GB_NAME_BYTES == 11, "OT/nickname fields must be 11 raw GB bytes");
_Static_assert(BC_OFF_OLDBUILD == 19u, "byte 19 must land on the Gen-3 BoxPokemon flags byte");
_Static_assert(BC_OFF_REC_LO + 7u == BC_OFF_OLDBUILD, "rec[0..6] must end exactly at byte 19");
_Static_assert(BC_OFF_REC_HI + 26u == BC_OFF_OTNAME, "rec[7..32] must end exactly at byte 46");
_Static_assert(BC_OFF_RESERVED + 3u == BC_CELL_BYTES, "the reserved tail must end at byte 80");
/* S150-12 BC-COPY-2: the six BC_FLAG_* bits must be pairwise disjoint (each its own bit)
 * and all fit under 0x40 (bits 0..5 of an 8-bit flags byte) -- a compile-time guard
 * that catches a copy/paste bit collision before it ever reaches a packed cell. */
_Static_assert((BC_FLAG_FROM_PARTY | BC_FLAG_HAS_XFER_REC | BC_FLAG_QUEUED_PC | BC_FLAG_EGG |
                BC_FLAG_HOLDS_ITEM | BC_FLAG_COPY) ==
               (BC_FLAG_FROM_PARTY + BC_FLAG_HAS_XFER_REC + BC_FLAG_QUEUED_PC + BC_FLAG_EGG +
                BC_FLAG_HOLDS_ITEM + BC_FLAG_COPY),
               "BC_FLAG_* bits must be pairwise disjoint (OR must equal sum)");
_Static_assert(BC_FLAG_FROM_PARTY < 0x40u && BC_FLAG_HAS_XFER_REC < 0x40u &&
               BC_FLAG_QUEUED_PC < 0x40u && BC_FLAG_EGG < 0x40u &&
               BC_FLAG_HOLDS_ITEM < 0x40u && BC_FLAG_COPY < 0x40u,
               "every BC_FLAG_* value must fit under 0x40 (bits 0..5 of the flags byte)");

/* Everything bc_unpack() hands back beyond the GbEditMon itself -- the cell's own
 * bookkeeping fields, which have no home in GbEditMon (a pure Gen-1/2 record editor that
 * knows nothing about the Bank, the ledger, or an RTC). */
typedef struct {
  uint8_t  gen;            /* GB_GEN1 / GB_GEN2, straight from offset 8               */
  uint8_t  flags;          /* offset 11, the BC_FLAG_* bits, verbatim                  */
  uint8_t  origin_game;    /* offset 68, one of BC_ORIGIN_*                            */
  uint32_t rtc_epoch;      /* offset 69..72, informational only                        */
  uint32_t bank_serial;    /* offset 73..76, the allocation that made ident32 unique   */
} BcMeta;

/* Magic + ident32 + the byte-19 old-build guard, in that order (bc_is_native() checks
 * all three, per SS11.1's false-positive analysis). A memset(0) cell (magic 0) and every
 * real Gen-3 record fail here -- see the header comment above. */
bool bc_is_native(const uint8_t rec80[BC_CELL_BYTES]);

/* 0 when !bc_is_native(); otherwise the gen byte (1 or 2) at offset 8. A convenience
 * so a caller does not have to re-read offset 8 itself after checking is_native. */
uint8_t bc_kind(const uint8_t rec80[BC_CELL_BYTES]);

/* FNV-1a-32 over bytes 8..10, 12..68 and 73..76 of `rec80` -- the immutable identity
 * span, EXCLUDING the flags byte (11) and rtc_epoch (69..72). Works on ANY 80-byte
 * buffer, native or not: bc_pack() calls this on the cell it just built (after every
 * other byte, including bank_serial, is in place) to fill offset 4..7, and bc_is_native()
 * calls it again to check that stored value still matches. */
uint32_t bc_ident32(const uint8_t rec80[BC_CELL_BYTES]);

/* Pack `mon` (a box-shape OR party-shape GbEditMon) into a native 80-byte cell.
 *
 * `mon->is_party` triggers the box-shape truncation gbs_move() itself performs
 * (gb_session.c): Gen-1 syncs the box-level byte (G1R_BOXLEVEL) to the live level
 * (G1R_LEVEL) before dropping the party-only tail; Gen-2 keeps the first 32 bytes and
 * leaves byte 32 zero (g2w_slot_to_box's own rule) -- a box-shape mon never had that
 * byte in the first place, so the same copy covers both cases.
 *
 * `flags`, `origin_game`, `rtc_epoch` and `bank_serial` are the caller's -- see the
 * file header comment for why bc_pack() cannot derive them itself. Two packs of the
 * SAME mon with two DIFFERENT `bank_serial` values are guaranteed to differ in bytes
 * 0..7 (ident32 changes because bank_serial is inside its hashed span); the SAME
 * bank_serial with a different `flags` or `rtc_epoch` leaves ident32 unchanged (both are
 * outside the hashed span), which is what lets a flags-only re-pack keep the cell's
 * identity (G-M4).
 *
 * Returns 0 on success. Returns <0, writing nothing to `out80`, when: `mon` or `out80`
 * is NULL; `mon->gen` is not GB_GEN1/GB_GEN2; or `mon->list_species` is the 0xFF list
 * terminator (gb_commit's own refusal, gb_edit.h). */
int bc_pack(const GbEditMon* mon, uint8_t flags, uint8_t origin_game,
            uint32_t rtc_epoch, uint32_t bank_serial, uint8_t out80[BC_CELL_BYTES]);

/* Unpack a native cell back into a box-shape GbEditMon (`mon->is_party` is always false
 * -- a cell only ever holds the box shape) plus its bookkeeping in `meta`. Returns false,
 * writing nothing to either out-parameter, when `rec80`/`mon`/`meta` is NULL or
 * !bc_is_native(rec80). */
bool bc_unpack(const uint8_t rec80[BC_CELL_BYTES], GbEditMon* mon, BcMeta* meta);

/* BACKLOG #271 review D1/D6: DUPLICATE's pure re-pack. Re-packs a native `cell` in place with
 * `serial` (must be non-zero; the caller allocates it) and BC_FLAG_COPY set, BC_FLAG_HAS_XFER_REC
 * and BC_FLAG_QUEUED_PC cleared. ident32 changes (bank_serial is inside the hashed span); species,
 * DVs, moves, names, origin_game and rtc_epoch are untouched. Returns false and leaves `cell`
 * byte-for-byte unchanged when cell is NULL, serial == 0, or the cell is not native. */
bool bc_restamp_copy(uint8_t cell[BC_CELL_BYTES], uint32_t serial);

/* A native cell's Gb12Mon VIEW (BACKLOG #150 S150-2, docs/BANK-CROSSGEN-DESIGN.md
 * SS11.5/SS11.13 row S150-2) -- a pure field copy off `mon`'s own getters, so a native
 * cell renders THE SAME as the identical mon in a GB session (both read the same
 * fields through the same accessors). NOT gen12_convert's input pipeline: this never
 * calls gen12_can_convert/gen12_convert itself -- the caller (box_native_decode,
 * pdna_box.c) hands the result to gb12_render_rec, exactly as the GB grid does.
 *
 * Names are decoded with gen1_decode_name()/g2_decode_text() -- the SAME lossy
 * decoders gen12_from_gen1/gen12_from_gen2 use -- never gb_get_nickname/gb_get_otname
 * (the reversible `{XX}`-escaping decoders, gb_edit.h:425-430): using the parser's own
 * decoder is what makes a native cell and the same mon in a GB session render the
 * identical string.
 *
 * `has_caught_data` is the converter's own rule (gen12_from_gen2's caught_valid,
 * gen2_save.c:535): Gen 2 with `(rec[0x1D] | rec[0x1E]) != 0`, else false. NOT gated
 * on `meta->origin_game` -- a genuine Crystal record can have legitimately all-zero
 * capture bytes (a traded-in or in-game-gift mon), and the parity sweep in
 * tests/host_bankcell_test.c pins five such real records from Guy's own Crystal.sav.
 * Gold/Silver's identical two bytes ARE Unused1/Unused2 there (gb_edit.h:138-145),
 * but the byte-level predicate ("do these bytes hold real capture data") is exactly
 * what gen12_convert.c:415's put_ot_gender keys off, so it is the correct one here
 * too, independent of which game the bytes came from.
 *
 * `out->slot_salt = id_salt` (the caller's bc_ident32(), so a native cell's
 * placeholder PID is stable per-cell rather than per-grid-position). Returns false,
 * writing nothing, only when `mon`, `meta` or `out` is NULL. */
bool bc_view(const GbEditMon* mon, const BcMeta* meta, uint32_t id_salt, Gb12Mon* out);

#endif /* BANK_CELL_H */
