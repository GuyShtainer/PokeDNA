#ifndef GEN12_FIXTURE_H
#define GEN12_FIXTURE_H

#include <stdint.h>
#include <stdbool.h>

/* Synthetic Gen-1 / Gen-2 save images, built byte-by-byte in C.
 *
 * Guy owns no Gen-1/2 saves and no GB/GBC ROMs (docs/research-gen12.md §8 searched the
 * whole toolkit: every .sav on disk is a 131072-byte Gen-3 image), so there is no corpus
 * to test the GB importer against. This module IS the corpus. It writes complete 32 KiB
 * battery-SRAM images with real checksums, so tests/host_gen12_test.c can drive
 * gen12_save + gen12_convert end to end without a single byte of anyone's game data.
 *
 * Two properties make it worth more than scaffolding:
 *
 *  1. It is an INDEPENDENT oracle. The DV derivations (shiny / gender / Unown letter),
 *     the 190-entry Gen-1 internal->dex map and the four legacy EXP curves are
 *     transcribed here from their primary sources — NOT copied from the module under
 *     test — so a test can assert the shipped table against this one and have the
 *     disagreement mean something.
 *  2. It is adversarial by construction. Every unused byte is LCG junk, every name field
 *     carries junk after its 0x50 terminator, the species lists have plausible-looking
 *     bytes past the terminator, and the stale bank copy of the current box holds
 *     DIFFERENT Pokemon from the live copy in main data. A parser that reads the wrong
 *     offset, ignores a count, or forgets that the current box lives in main data
 *     (§1.2 / §2.3) fails loudly instead of returning something plausible.
 *
 * Host-only: <stdint.h>/<string.h> and nothing else. Never linked into the GBA build.
 *
 * Sources for the layouts: docs/research-gen12.md §1-§3, itself from Bulbapedia's
 * "Save data structure (Generation I/II)", "Pokemon data structure (Generation I/II)"
 * and "Individual values" pages. Character codes and the internal->dex map were re-read
 * from pret/pokered this pass (see gen12_fixture.c) because the research doc flags both
 * as VERIFY.
 */

#define GBF_SAVE_BYTES   0x8000u          /* the battery SRAM image itself            */
#define GBF_RTC_TAIL_32  44u              /* MBC3 RTC footer, 32-bit timestamp        */
#define GBF_RTC_TAIL_64  48u              /* ...and the 64-bit variant (bgb/mGBA)     */
#define GBF_MAX_BYTES    (GBF_SAVE_BYTES + GBF_RTC_TAIL_64)

typedef enum { GBF_RBY = 0, GBF_GS, GBF_CRYSTAL } GbfGame;

/* ---- layout constants -----------------------------------------------------------
 * Deliberately a SECOND transcription of the same tables gen12_save.c works from. If
 * the two disagree the fixture stops parsing and someone has to go read the wiki again,
 * which is the correct outcome. */

/* Gen 1 (Western R/B/Y) */
#define GBF1_PLAYER_NAME   0x2598u
#define GBF1_TID           0x2605u        /* 2 bytes, BIG-endian                       */
#define GBF1_CURBOX_NO     0x284Cu        /* bits 0-6 index, bit 7 "box was switched"  */
#define GBF1_PARTY         0x2F2Cu        /* 0x194 bytes                               */
#define GBF1_CURBOX_DATA   0x30C0u        /* 0x462 bytes — the LIVE current box        */
#define GBF1_CKSUM_START   0x2598u
#define GBF1_CKSUM_END     0x3522u        /* inclusive                                 */
#define GBF1_CKSUM         0x3523u        /* 1 byte                                    */
#define GBF1_BANK2         0x4000u        /* boxes 1-6                                 */
#define GBF1_BANK3         0x6000u        /* boxes 7-12                                */
#define GBF1_BOX_BYTES     0x462u         /* 1122 — 20 slots                           */
#define GBF1_PARTY_BYTES   0x194u         /* 404 — 6 slots                             */
#define GBF1_REC_BYTES     33u
#define GBF1_PREC_BYTES    44u
#define GBF1_NBOXES        12
#define GBF1_BOX_CAP       20
/* Per-bank trailer: one whole-bank checksum then six per-box checksums. Marked VERIFY
 * in research-gen12.md §1.2 — v1 only uses them for a "box bank looks sane" warning. */
#define GBF1_BANK2_CKSUM   0x5A4Cu
#define GBF1_BANK3_CKSUM   0x7A4Cu

/* Gen 2 (Western G/S and Crystal) */
#define GBF2_TID           0x2009u        /* both games, 2 bytes BE                    */
#define GBF2_PLAYER_NAME   0x200Bu
#define GBF2_GS_PARTY      0x288Au
#define GBF2_C_PARTY       0x2865u
#define GBF2_GS_CURBOX_NO  0x2724u
#define GBF2_C_CURBOX_NO   0x2700u
#define GBF2_GS_BOXNAMES   0x2727u        /* 14 x 9                                    */
#define GBF2_C_BOXNAMES    0x2703u
#define GBF2_GS_CURBOX     0x2D6Cu        /* 1102 bytes — the LIVE current box         */
#define GBF2_C_CURBOX      0x2D10u
#define GBF2_GS_CKSUM      0x2D69u        /* 2 bytes, stored little-endian             */
#define GBF2_C_CKSUM       0x2D0Du
#define GBF2_GS_CKSUM2     0x7E6Du
#define GBF2_C_CKSUM2      0x1F0Du
#define GBF2_CKSUM_START   0x2009u
#define GBF2_GS_CKSUM_END  0x2D68u        /* inclusive                                 */
#define GBF2_C_CKSUM_END   0x2B82u
#define GBF2_C_BACKUP      0x1209u        /* byte-for-byte mirror of the primary block */
#define GBF2_BOX_BYTES     1102u          /* 20 slots, 32-byte records                 */
#define GBF2_PARTY_BYTES   428u
#define GBF2_REC_BYTES     32u
#define GBF2_PREC_BYTES    48u
#define GBF2_NBOXES        14
#define GBF2_BOX_CAP       20
/* Seven boxes per bank at a 0x450 stride (1102 used, 2 padding). */
#define GBF2_BANK2         0x4000u
#define GBF2_BANK3         0x6000u
#define GBF2_BOX_STRIDE    0x450u

/* Shared charset codes (pret/pokered constants/charmap.asm — the same code points serve
 * both generations for every name-enterable glyph). */
#define GBF_CH_TERM        0x50u
#define GBF_CH_SPACE       0x7Fu
#define GBF_CH_A           0x80u          /* A..Z = 0x80..0x99                         */
#define GBF_CH_a           0xA0u          /* a..z = 0xA0..0xB9                         */
#define GBF_CH_0           0xF6u          /* 0..9 = 0xF6..0xFF                         */
#define GBF_CH_MALE        0xEFu
#define GBF_CH_FEMALE      0xF5u

/* ---- roster description -------------------------------------------------------- */

enum { GBF_MEDFAST = 0, GBF_MEDSLOW, GBF_FAST, GBF_SLOW };  /* fixture-local ids      */

/* Gen-2 gender is "female if Attack DV <= threshold" (Bulbapedia, Individual values).
 * These three sentinels cover the species the DV cannot decide for. */
#define GBF_G_GENDERLESS  (-1)
#define GBF_G_ALL_MALE    (-2)
#define GBF_G_ALL_FEMALE  (-3)

#define GBF_F_EGG        0x01u   /* species-list entry becomes 0xFD (Gen 2 only)      */
#define GBF_F_UNNICKED   0x02u   /* nickname is the species' own name                 */
#define GBF_F_REFUSE     0x04u   /* the converter MUST reject this record             */

typedef struct {
  uint16_t species;      /* Gen 1: INTERNAL index. Gen 2: national dex number.        */
  uint16_t dex;          /* national dex the parser must report (0 = glitch species)  */
  uint8_t  growth;       /* GBF_MEDFAST.. — only used to synthesise exp from level    */
  uint8_t  level;
  uint32_t exp_raw;      /* 0 = derive from level+growth; otherwise written verbatim  */
  uint8_t  dv[4];        /* Atk, Def, Spd, Spc (0..15)                                */
  uint16_t moves[4];
  uint8_t  ppup[4];      /* PP Ups applied, 0..3, stored in PP bits 6-7               */
  uint16_t otid;
  uint16_t statexp[5];   /* HP, Atk, Def, Spd, Spc                                    */
  uint8_t  item;         /* Gen 2 held item id; Gen 1 catch-rate byte                 */
  uint8_t  friendship;   /* Gen 2; remaining egg cycles while GBF_F_EGG               */
  uint8_t  pokerus;
  uint8_t  met_level, met_loc, ot_gender, met_time;  /* Crystal caught data           */
  int8_t   gb_female_thresh;  /* Atk-DV threshold, or a GBF_G_* sentinel              */
  uint8_t  flags;
  const char* nick;      /* UTF-8; re-encoded into the GB charset at build time       */
  const char* ot;
  const char* why;       /* what this slot exists to prove — printed by the test      */
} GbfMon;

/* ---- building ------------------------------------------------------------------ */

/* Growth-rate lookup used to turn a roster level into an EXP value. gbf_build uses the
 * fixture's own small table; gbf_build_ex lets the test inject the SHIPPED table
 * (pk_species_growth) so a level assertion tests exp encoding rather than re-testing a
 * growth table the fixture and the converter would otherwise have to agree on twice. */
typedef uint8_t (*GbfGrowthFn)(uint16_t dex);

/* Write a complete image into `out` (>= GBF_MAX_BYTES). rtc_tail must be 0,
 * GBF_RTC_TAIL_32 or GBF_RTC_TAIL_64. Returns the image length in bytes. */
uint32_t gbf_build(GbfGame game, uint8_t* out, uint32_t rtc_tail);
uint32_t gbf_build_ex(GbfGame game, uint8_t* out, uint32_t rtc_tail, GbfGrowthFn growth);

/* The planted roster. box < 0 asks for the party. Returns NULL for an empty box. */
const GbfMon* gbf_roster(GbfGame game, int box, int* count);
int  gbf_nboxes(GbfGame game);
int  gbf_current_box(GbfGame game);       /* the box whose live copy is in main data  */
int  gbf_box_capacity(GbfGame game);
uint16_t gbf_player_tid(void);
const char* gbf_player_name(void);
/* Box names are Gen-2 only; returns NULL for GBF_RBY. */
const char* gbf_box_name(GbfGame game, int box);
/* The nickname a decoy in the stale bank copy of the current box carries. No parser
 * that reads the live copy should ever see it. */
const char* gbf_stale_marker(void);

/* ---- oracles (independent of anything under test) ------------------------------ */

uint32_t gbf_exp_for(uint8_t growth, uint8_t level);   /* the four legacy curves      */
uint8_t  gbf_hp_dv(const uint8_t dv[4]);               /* LSBs of Atk/Def/Spd/Spc     */
bool     gbf_is_shiny(const uint8_t dv[4]);            /* Def=Spd=Spc=10, Atk bit1    */
int      gbf_unown_letter(const uint8_t dv[4]);        /* 0..25 = A..Z                */
/* 0 male, 1 female, 2 genderless. */
int      gbf_gender(const uint8_t dv[4], int8_t gb_female_thresh);
/* The Gen-3 gender-ratio byte a Gen-2 threshold corresponds to, for cross-checking
 * pk_species_gender_ratio(). 0xFF genderless / 0xFE all-female / 0x00 all-male. */
uint8_t  gbf_gen3_ratio(int8_t gb_female_thresh);
/* Gen-1 internal index (1..190) -> national dex; 0 = MissingNo / out of range. */
uint8_t  gbf_gen1_dex(uint16_t internal_index);
#define GBF_GEN1_INDEX_MAX 190

/* Encode UTF-8 (ASCII plus the two gender symbols) into the GB charset. Writes at most
 * `cap` bytes and terminates with 0x50 when there is room. Returns glyphs written. */
int gbf_encode_name(uint8_t* dst, int cap, const char* utf8);

/* ---- checksums (also the mutators a negative test needs) ----------------------- */

uint8_t  gbf_gen1_checksum(const uint8_t* save);          /* main block, 8-bit        */
uint16_t gbf_gen2_checksum(const uint8_t* save, GbfGame game);  /* primary, 16-bit    */

/* Where the fixture mirrors the primary player block to build the backup copy: one
 * region for Crystal, five for G/S. `from`..`to` inclusive. Exposed because the G/S
 * scatter map is the single value research-gen12.md §2.2 flags as most likely to have
 * been garbled in transcription, and two independent copies agreeing is the only
 * evidence available without a real save. Returns the region count (0 for GBF_RBY). */
typedef struct { uint32_t from, to, dest; } GbfMirror;
int gbf_mirror_map(GbfGame game, const GbfMirror** out);
/* Corrupt the image so a loader MUST reject it (or fall back to the backup copy):
 * flips one byte inside the checksummed range, leaving the stored checksum alone. */
void gbf_break_primary(GbfGame game, uint8_t* save);

#endif /* GEN12_FIXTURE_H */
