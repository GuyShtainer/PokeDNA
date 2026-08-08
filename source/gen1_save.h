#ifndef GEN1_SAVE_H
#define GEN1_SAVE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Generation-I (Red/Blue/Yellow, WESTERN) battery-save reader — pure C, no tonc,
 * no FatFs, host-testable. 32 KiB SRAM image (an emulator ".sav" is that image
 * verbatim). READ ONLY, by design: PokeDNA never writes a GB save, so none of the
 * checksum writers or the box-bank flush logic exist here.
 *
 * Nothing about this is an official transfer path. The Time Capsule was Gen 1 <-> 2
 * only and Gen 3 broke compatibility outright, so anything downstream of this parser
 * is a conversion PokeDNA invents (modelled on Poke Transporter). This file's whole
 * job is to hand module C the GB facts, unembellished.
 *
 * Layout sources: Bulbapedia "Save data structure (Generation I)" and "Pokemon data
 * structure (Generation I)". Multi-byte GB fields are BIG-endian, the opposite of the
 * Gen-3 side.
 *
 * Two ways in, one code path:
 *   - gen1_open()        when the whole 32 KiB image is already in RAM (host tests).
 *   - gen1_open_ranged() when it is not (the GBA build: ~1.5 KB of free EWRAM cannot
 *                        hold a 32 KiB image, so the caller supplies a seek+read
 *                        callback and this file streams).
 * Neither allocates. Every buffer bigger than 128 bytes is the caller's.
 *
 * Japanese R/G/B/Y use different name lengths, 8x30 boxes and shifted offsets. They
 * fail the Western checksum here and are refused (GEN1_ERR_CHECKSUM) rather than
 * misparsed. Out of scope for v1.
 */

/* ---- file geometry ------------------------------------------------------- */

#define GEN1_SAVE_SIZE       0x8000u   /* exact SRAM image size; a longer file is padding */
#define GEN1_NUM_BOXES       12
#define GEN1_BOX_CAPACITY    20
#define GEN1_PARTY_CAPACITY  6
#define GEN1_BOX_BYTES       0x462u    /* 1 + 21 + 20*33 + 20*11 + 20*11 */
#define GEN1_PARTY_BYTES     0x194u    /* 1 +  7 +  6*44 +  6*11 +  6*11 */
#define GEN1_BOX_REC_BYTES   33
#define GEN1_PARTY_REC_BYTES 44
#define GEN1_NAME_BYTES      11        /* both OT and nickname fields */

/* The party is exposed as a 13th pseudo-box so the box UI can page it like any
 * other; pass it anywhere a `box` index is taken. */
#define GEN1_PARTY_BOX       GEN1_NUM_BOXES

/* Main data block (SRAM bank 1). */
#define GEN1_OFF_PLAYER_NAME 0x2598u
#define GEN1_OFF_TRAINER_ID  0x2605u   /* 2 bytes, big-endian */
#define GEN1_OFF_CURRENT_NO  0x284Cu   /* bits 0-6 = current box 0..11, bit 7 = "switched" */
#define GEN1_OFF_PARTY       0x2F2Cu
#define GEN1_OFF_CURRENT_BOX 0x30C0u   /* the LIVE copy of whichever box is open */
#define GEN1_OFF_CHECKSUM    0x3523u
#define GEN1_SUM_FIRST       0x2598u   /* checksum covers [FIRST, LAST] inclusive */
#define GEN1_SUM_LAST        0x3522u

/* Stored boxes: 1-6 in SRAM bank 2, 7-12 in bank 3, packed back to back. */
#define GEN1_OFF_BANK2       0x4000u
#define GEN1_OFF_BANK3       0x6000u

/* Per-bank checksum block, immediately after the 6 boxes (0x4000 + 6*0x462 = 0x5A4C).
 * Byte 0 covers the whole bank, bytes 1-6 cover one box each. ADVISORY ONLY — see
 * gen1_check_banks(). */
#define GEN1_OFF_BANK2_SUMS  0x5A4Cu
#define GEN1_OFF_BANK3_SUMS  0x7A4Cu

/* Stat order for dv[] / statexp[]. The first four indices deliberately match
 * gen3_mon.h's PK_HP..PK_SPE so module C can copy them straight across; Gen 1 has
 * one combined Special, which feeds both SpA and SpD on the Gen-3 side. */
enum { G1_HP = 0, G1_ATK, G1_DEF, G1_SPE, G1_SPC, G1_NSTATS };

typedef enum {
  GEN1_OK = 0,
  GEN1_ERR_SIZE,       /* file shorter than 32 KiB                                  */
  GEN1_ERR_READ,       /* the caller's read callback failed                         */
  GEN1_ERR_CHECKSUM,   /* main 8-bit checksum mismatch — not a Western R/B/Y save   */
  GEN1_ERR_STRUCTURE,  /* checksum passed but the counts/lists are impossible       */
} Gen1Status;

const char* gen1_status_text(Gen1Status st);

/* ---- decoded records ----------------------------------------------------- */

/* One Pokemon, decoded. ~88 bytes; caller-owned, decode one at a time.
 *
 * Deliberately absent because Gen 1 does not have them: gender, nature, ability,
 * held item, friendship, ball, met data, personality value, shininess. Anything
 * downstream that shows those is inventing them. */
typedef struct {
  uint8_t  species_idx;      /* raw Gen-1 INTERNAL index 1..190 (not dex order)     */
  uint16_t dex;              /* National Dex 1..151; 0 = MissingNo/glitch, reject   */
  bool     list_mismatch;    /* the box species list disagreed with the record byte */
  uint8_t  level;            /* party: the live level byte; box: the stored one     */
  uint32_t exp;              /* 24-bit                                             */
  uint8_t  dv[G1_NSTATS];    /* 0..15; dv[G1_HP] is DERIVED, not stored            */
  uint16_t statexp[G1_NSTATS];
  uint16_t moves[4];         /* Gen-1 move ids 1..165, 0 = empty slot              */
  uint8_t  pp_raw[4];        /* the stored byte, unsplit — what gen12_convert wants */
  uint8_t  pp[4];            /* pp_raw & 0x3F: current PP                           */
  uint8_t  ppups[4];         /* pp_raw >> 6: PP Ups applied, 0..3                   */
  uint16_t otId;             /* the GB "trainer ID"; Gen 3 keeps it as the public TID */
  uint8_t  catch_rate;       /* record byte 0x07; a Gen-2 held item only after a Time
                              * Capsule trade, meaningless as an item in Gen 1      */
  bool     is_party;
  /* Sized for the worst case, not the typical one: one GB text byte can decode to
   * three (the UTF-8 gender signs), so a 7-char OT needs 21+1 and a 10-char nickname
   * 30+1. See gen1_char_ascii. */
  char     otName[24];
  char     nickname[32];
} Gen1Mon;

/* ---- save handle --------------------------------------------------------- */

typedef struct {
  uint8_t  checksum_stored, checksum_calc;
  uint16_t trainer_id;
  char     player_name[16];
  int      current_box;                 /* 0..11 */
  uint8_t  party_count;                 /* 0..6  */
  uint8_t  box_count[GEN1_NUM_BOXES];   /* 0..20 */
  bool     bank_ok[2];                  /* filled by gen1_check_banks(), else false */
  bool     banks_checked;
} Gen1Save;

/* Seek+read callback. Must fill exactly `len` bytes from file offset `off`, or
 * return false. Pure-C boundary: the FatFs f_lseek/f_read live in the caller. */
typedef bool (*Gen1ReadFn)(void* ctx, uint32_t off, void* buf, uint32_t len);

/* Validate + read the header. `len` is the file length: >= 32768 is accepted and any
 * tail ignored (Gen-1 carts have no RTC, so a tail is an emulator artifact, unlike
 * Gen 2's real 44/48-byte MBC3 RTC footer). Reads ~4.1 KB via `rd` for the checksum
 * plus 13 single bytes for the occupancy counts; uses 128 bytes of stack. */
Gen1Status gen1_open_ranged(Gen1ReadFn rd, void* ctx, uint32_t len, Gen1Save* out);
Gen1Status gen1_open(const uint8_t* img, uint32_t len, Gen1Save* out);

/* Optional, advisory, and NOT required for a save to be usable: verify the two box
 * banks' own checksums (whole bank + 6 per-box). Streams 16 KiB. The exact summed
 * ranges are the least certain part of the documented layout, so a failure here must
 * never gate reading — surface it as "this bank looks odd", nothing more. */
Gen1Status gen1_check_banks(Gen1ReadFn rd, void* ctx, Gen1Save* io);

/* ---- box access ---------------------------------------------------------- */

/* File offset + size of one box's list blob. box: 0..11, or GEN1_PARTY_BOX.
 * The CURRENT box resolves to the live copy in bank 1 (0x30C0), not its stale
 * banked copy — see the note in gen1_save.c. */
uint32_t gen1_list_offset(const Gen1Save* s, int box);
uint32_t gen1_list_bytes(int box);      /* 0x462, or 0x194 for the party */
int      gen1_list_capacity(int box);   /* 20, or 6 for the party */

/* Occupancy from the already-opened handle (no I/O). -1 if `box` is out of range. */
int gen1_count(const Gen1Save* s, int box);

/* Occupancy read straight out of a list blob (the count byte, clamped to capacity). */
int gen1_list_count(const uint8_t* list, int box);

/* Decode slot `slot` of a list blob of gen1_list_bytes(box) bytes. Returns false for
 * an empty slot or a record whose species byte is not a real Gen-1 index; `out` is
 * zeroed either way. `out->dex == 0` means the index mapped to MissingNo — the record
 * decodes but must not be converted. */
bool gen1_decode(const uint8_t* list, int box, int slot, Gen1Mon* out);

/* Same, straight from a resident image (host tests). */
bool gen1_decode_image(const Gen1Save* s, const uint8_t* img, int box, int slot, Gen1Mon* out);

/* ---- pieces module B and module C also need ------------------------------ */

/* Gen-1 internal species index -> National Dex number. 0 for the 39 MissingNo slots
 * and for any index outside 1..190. */
uint16_t gen1_dex_from_index(uint8_t internal);

/* Streaming 8-bit checksum (init, feed chunks in order, finish). Exposed so the GBA
 * glue can validate without a resident image. */
typedef struct { uint8_t sum; } Gen1Sum;
void    gen1_sum_init(Gen1Sum* s);
void    gen1_sum_feed(Gen1Sum* s, const uint8_t* p, uint32_t n);
uint8_t gen1_sum_final(const Gen1Sum* s);

/* Decode one GB text byte into up to 3 output chars; returns how many were written
 * (0 = terminator, stop decoding). Expansion is normal, not an edge case:
 *   - the lowercase contraction glyphs ('d 'l 's 't 'v) and PK/MN are ONE byte on the
 *     GB and two characters in ASCII, so a name can decode LONGER than it was stored;
 *   - the gender signs come out as UTF-8 (U+2642 / U+2640), 3 bytes, the same
 *     spelling gen3_record.c's encode_name turns back into the Gen-3 charset.
 * Anything with no representation becomes '?', matching gen3_decode_char's policy —
 * never a silently invented letter.
 *
 * Gen 2 shares these code points for every name-enterable glyph, so module B can call
 * this instead of duplicating the table; it differs only in the decorative range
 * (Gen-2 0xE9 '&', 0xEA 'e-acute', 0xEB-0xEE arrows), which lands on '?' here. */
int gen1_char_ascii(uint8_t c, char out[3]);

/* Decode an `nbytes`-long GB name field into `out` (cap includes the NUL). Stops at
 * the 0x50 terminator. Returns the ASCII length written. */
int gen1_decode_name(char* out, int cap, const uint8_t* src, int nbytes);

#endif /* GEN1_SAVE_H */
