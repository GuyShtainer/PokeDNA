#ifndef GEN2_SAVE_H
#define GEN2_SAVE_H

#include <stdint.h>
#include <stdbool.h>

/* Generation-II (Gold/Silver/Crystal) save READER — pure C, host-testable.
 *
 * Read-only by design. PokeDNA never writes a Game Boy save: there was never an
 * official Gen 1/2 -> Gen 3 transfer (the Time Capsule is Gen 1 <-> Gen 2 only,
 * and Gen 3 broke compatibility entirely), so anything we do with these bytes is
 * a one-way import PokeDNA invents. Nothing here mutates a buffer.
 *
 * Everything is offsets + arithmetic on caller-supplied buffers: no statics, no
 * FatFs, no tonc. The GBA glue cannot hold a 32 KiB save resident (~1.5 KB of
 * EWRAM is free), so the API is split so it can PAGE:
 *   1. stream the file through g2_scan_* once -> version + checksum verdict,
 *      without ever holding more than the caller's own read chunk;
 *   2. read the individual header fields at the g2_offsets() positions;
 *   3. per box, seek g2_list_offset() and either read the whole g2_list_size()
 *      (<=1102) blob into a staging buffer -> g2_list_count / g2_list_mon, or,
 *      if 1.1 KB is too much, read 22 bytes of species area + 54 bytes per slot
 *      at the g2_off_* positions -> g2_decode_mon / g2_decode_text.
 * The g2_*_at() whole-image helpers exist for the host tests, which do have the
 * whole file in memory.
 *
 * Sizes/offsets are from Bulbapedia "Save data structure (Generation II)" and
 * "Pokemon data structure (Generation II)" (fetched as raw wikitext 2026-08;
 * every table below was transcribed from that fetch, not from memory), with the
 * Crystal layout cross-checked against pret/pokecrystal ram/sram.asm — reference
 * only, per docs/kb/licensing.md; this file is clean-room from the spec in
 * docs/research-gen12.md.
 *
 * WESTERN (English) SAVES ONLY. Japanese G/S/C use 9 boxes of 30, 6-byte name
 * fields and different checksum ranges; they are DETECTED (so the UI can refuse
 * honestly) but not parsed.
 *
 * Multi-byte values in a GB save are BIG-endian (the Gen-3 side is little-endian)
 * — except the two stored checksums, which the games write little-endian.
 */

/* ---- file geometry ------------------------------------------------------- */
#define G2_SAVE_SIZE        0x8000u   /* 32 KiB of battery SRAM = the .sav image */
/* MBC3+RTC carts: VBA/VBA-M/mGBA/BGB append a 44- or 48-byte clock footer
 * (32- vs 64-bit timestamp; bgb.bircd.org/rtcsave.html). Ignore the tail. */
#define G2_MAX_RTC_TAIL     48u

#define G2_NUM_BOXES        14        /* western; JP has 9                      */
#define G2_BOX_CAPACITY     20        /* western; JP has 30                     */
#define G2_PARTY_CAPACITY   6
#define G2_BOX_ENTRY        32        /* PC record: species..level              */
#define G2_PARTY_ENTRY      48        /* + status, HP and the 5 battle stats    */
#define G2_BOX_LIST_SIZE    1102      /* 2 + 20*(32+23)                         */
#define G2_PARTY_LIST_SIZE  428       /* 2 + 6*(48+23)                          */
#define G2_MAX_LIST_SIZE    G2_BOX_LIST_SIZE   /* staging buffer the glue needs */
#define G2_BOX_PARTY        G2_NUM_BOXES       /* pseudo-box index for the party */

#define G2_NAME_CHARS       10        /* glyphs in an OT/nickname field (of 11) */
#define G2_BOXNAME_CHARS    8         /* glyphs in a box name (of 9)            */
#define G2_NAME_BYTES       36        /* decoded UTF-8 + NUL; see G2Mon.otname  */

#define G2_SPECIES_MAX      251       /* Gen-2 index == National Dex, 1..251    */
#define G2_LIST_TERMINATOR  0xFFu
#define G2_LIST_EGG         0xFDu     /* species-list marker; record keeps the species */

typedef enum {
  G2_VER_NONE = 0,
  G2_VER_GS,          /* western Gold/Silver   */
  G2_VER_CRYSTAL,     /* western Crystal       */
  G2_VER_JP_GS,       /* detected, unsupported */
  G2_VER_JP_CRYSTAL   /* detected, unsupported */
} G2Version;

typedef struct {
  G2Version version;
  bool     supported;    /* western G/S or Crystal: safe to parse              */
  bool     primary_ok;   /* checksum 1 over the live player block matches      */
  bool     backup_ok;    /* checksum 2 over the duplicated copy matches        */
  bool     ambiguous;    /* >1 version's checksum matched (1-in-65536 collision);
                          * break the tie with g2_list_plausible() on a box     */
  bool     short_file;   /* fewer than 32 KiB of save data were ever fed        */
  uint32_t tail;         /* bytes past 0x8000 in the file (RTC footer)         */
} G2Save;

/* One decoded Pokemon. Level/species/EXP are stored, everything the Gen-2 games
 * DERIVE at runtime (gender, shininess, Unown letter, the HP DV) is computed on
 * decode — a Gen-2 mon has no gender/shiny byte anywhere in the save. */
typedef struct {
  uint8_t  species;        /* 1..251 National Dex; 0 = empty/invalid           */
  bool     is_egg;         /* from the LIST byte 0xFD, not from the record     */
  bool     is_party;       /* 48-byte record: stats[] and status are valid     */
  uint8_t  held_item;      /* Gen-2 item id (no Gen-3 equivalent for many)     */
  uint8_t  moves[4];
  uint8_t  pp[4];          /* current PP (record bits 0-5)                     */
  uint8_t  pp_up[4];       /* PP Ups applied, 0..3 (record bits 6-7)           */
  uint16_t otid;           /* public TID only; Gen 2 has no secret ID          */
  uint32_t exp;            /* 24-bit                                           */
  uint16_t statexp[5];     /* "EV data": HP, Atk, Def, Spd, Special            */
  uint8_t  dv[4];          /* Atk, Def, Spd, Special (0..15)                   */
  uint8_t  hp_dv;          /* DERIVED from the other four                      */
  uint8_t  friendship;     /* remaining egg cycles while is_egg                */
  uint8_t  pokerus;        /* high nibble strain, low nibble days left         */
  uint8_t  level;
  /* Crystal-only capture record; zero in G/S (the fields still round-trip if a
   * mon was traded from Crystal to G/S). */
  bool     caught_valid;
  uint8_t  caught_time;    /* 1 morning, 2 day, 3 night; 0 = not recorded      */
  uint8_t  caught_level;   /* 1 means "hatched from an Egg" to the Poke Seer   */
  uint8_t  caught_loc;
  uint8_t  ot_gender;      /* 0 male, 1 female                                 */
  /* Decoded text is UTF-8, not ASCII: the male/female signs decode to "♂"/"♀"
   * exactly as data_tables.c spells them, so module C can compare a nickname
   * against pk_species_name() to spot "not nicknamed" for the Nidoran lines.
   * 10 glyphs can therefore need more than 10 bytes — truncate by GLYPH. */
  char     otname[G2_NAME_BYTES];
  char     nickname[G2_NAME_BYTES];
  bool     is_shiny;       /* DERIVED from the DVs                             */
  uint8_t  unown_letter;   /* DERIVED, 0..25 = A..Z; meaningful only for Unown */
  uint16_t stats[6];       /* party only: HP, Atk, Def, Spd, SpA, SpD          */
  uint16_t cur_hp;         /* party only                                       */
  uint8_t  status;         /* party only                                       */
} G2Mon;

/* Header fields worth showing above a box grid. */
typedef struct {
  uint16_t tid;
  char     player[G2_NAME_BYTES];   /* <=7 glyphs, UTF-8 (see G2Mon.otname)    */
  uint32_t money;
  uint8_t  johto_badges, kanto_badges;   /* bitfields, one bit per badge       */
  int      dex_owned, dex_seen;          /* popcounts over the 251 valid bits  */
  int      current_box;                  /* 0..13                              */
  int      player_gender;                /* 0 M, 1 F; -1 = not stored (G/S)    */
} G2Header;

/* ---- detection ----------------------------------------------------------- */

/* Streaming detector: the GBA glue reads the file in chunks (any size, any
 * order, but each byte at most once) and gets a verdict without ever holding
 * 32 KiB. sizeof(G2Scan) is small enough to live on the stack. */
typedef struct {
  uint32_t sum[8];       /* one running byte-sum per candidate checksum region */
  uint8_t  stored[5][2]; /* the checksum words captured where they lie         */
  uint8_t  have[5];
  uint8_t  nonzero;      /* bit per sum: the region held at least one set bit  */
  uint32_t fed;
} G2Scan;

void g2_scan_begin(G2Scan* s);
/* `off` is the byte offset of buf[0] inside the file. Bytes past G2_SAVE_SIZE
 * (the RTC footer) are ignored. */
void g2_scan_feed(G2Scan* s, uint32_t off, const uint8_t* buf, uint32_t len);
/* `total` = the file's real size, so the RTC tail can be reported. Returns
 * out->supported. Fills *out in every case. */
bool g2_scan_finish(const G2Scan* s, uint32_t total, G2Save* out);

/* One-shot wrapper for callers that already hold the whole image. */
bool g2_detect(const uint8_t* sav, uint32_t size, G2Save* out);

/* Seek+read callback, same boundary as gen1_save.h's Gen1ReadFn so the GBA glue
 * writes ONE f_lseek/f_read shim for both generations: fill exactly `len` bytes
 * from file offset `off`, or return false.
 *
 * `scratch`/`scratch_len` is the caller's read buffer (>= 64 bytes; bigger means
 * fewer SD reads). Nothing is retained after the call. This module allocates
 * nothing, which is why the buffer is yours and not ours. */
typedef bool (*G2ReadFn)(void* ctx, uint32_t off, void* buf, uint32_t len);
bool g2_detect_ranged(G2ReadFn rd, void* ctx, uint32_t len,
                      uint8_t* scratch, uint32_t scratch_len, G2Save* out);

const char* g2_version_name(G2Version v);
/* Why a save was refused, for the UI ("Japanese saves are not supported yet",
 * "both checksums failed", ...); NULL when sv->supported. */
const char* g2_reject_reason(const G2Save* sv);

/* ---- layout -------------------------------------------------------------- */

/* File offsets of the main-data fields, per version. 0 = the version does not
 * store that field (player_gender in G/S). Lets the glue read a narrow window
 * instead of the whole block. */
typedef struct {
  uint32_t tid, player_name, money, johto_badges, kanto_badges;
  uint32_t dex_owned, dex_seen, current_box_no, box_names;
  uint32_t party_list, current_box_list, player_gender;
} G2Offsets;
bool g2_offsets(G2Version ver, G2Offsets* out);

/* Where box `box` (0..13, or G2_BOX_PARTY) actually lives RIGHT NOW.
 * The banked copy of the *current* box is stale — the live copy is in main data
 * — so this returns the main-data offset for box == current_box. Pass the
 * current_box from G2Header (or -1 if unknown, which then always returns the
 * banked copy). 0 = bad arguments. */
uint32_t g2_list_offset(const G2Save* sv, int box, int current_box);
int g2_list_size(int box);        /* 1102 / 428 */
int g2_list_capacity(int box);    /* 20 / 6     */
int g2_list_entry_size(int box);  /* 32 / 48    */

/* ---- list + record decoding (buffer-local: `list` is one g2_list_size() blob) */

/* Mons actually stored, 0..capacity. -1 if the list is malformed (count above
 * capacity, or the 0xFF terminator missing from the species list). */
int  g2_list_count(const uint8_t* list, int box);
/* Cheap "does this look like a box list at all" test — a structural second
 * opinion when a checksum is inconclusive. */
bool g2_list_plausible(const uint8_t* list, int box);
/* Species list entry for a slot: `species` gets the LIST byte's species and
 * `is_egg` reflects the 0xFD marker. */
bool g2_list_species(const uint8_t* list, int box, int slot,
                     uint8_t* species, bool* is_egg);
/* Full decode of one slot (record + its OT name + nickname + the derived
 * properties). false for slots past the count or with a glitch species. */
bool g2_list_mon(const uint8_t* list, int box, int slot, G2Mon* out);

/* Decode a bare 32/48-byte record with no list around it (the OT name, the
 * nickname and the egg flag live outside the record, so they come out empty). */
bool g2_decode_mon(const uint8_t* rec, bool is_party, G2Mon* out);

/* Byte offsets of the pieces INSIDE a list, for a caller too tight on EWRAM to
 * stage all 1102 bytes: read the 22-byte count+species area once per box, then
 * 32 + 11 + 11 bytes per slot (~54 B live instead of 1.1 KB) and feed them to
 * g2_decode_mon + g2_decode_text yourself. Same numbers g2_list_mon uses. */
int g2_off_species_area(int box);          /* +1; capacity+1 bytes, 0xFF-ended */
int g2_off_record(int box, int slot);
int g2_off_otname(int box, int slot);      /* 11 bytes */
int g2_off_nickname(int box, int slot);    /* 11 bytes */

/* Box name (14 x 9 bytes, <=8 glyphs + terminator). `cap` >= G2_NAME_BYTES to be
 * safe. `names` points at the 126-byte block (g2_offsets().box_names). */
bool g2_box_name(const uint8_t* names, int box, char* out, int cap);

/* ---- whole-image helpers (host tests; the GBA glue uses the paged path) ---- */
bool g2_read_header(const uint8_t* sav, const G2Save* sv, G2Header* out);
int  g2_box_count_at(const uint8_t* sav, const G2Save* sv, const G2Header* hd, int box);
bool g2_box_mon_at(const uint8_t* sav, const G2Save* sv, const G2Header* hd,
                   int box, int slot, G2Mon* out);
bool g2_box_name_at(const uint8_t* sav, const G2Save* sv, int box, char* out, int cap);

/* ---- derived properties (pure, no data tables) ---------------------------- */

/* Gen 1/2 store only four DVs; the HP DV is the LSBs of Atk,Def,Spd,Spc as one
 * nibble (Bulbapedia "Individual values"). */
uint8_t g2_hp_dv(const uint8_t dv[4]);
/* Shiny iff Def == Spd == Spc == 10 and Atk in {2,3,6,7,10,11,14,15}. */
bool    g2_dv_shiny(const uint8_t dv[4]);
/* Unown letter 0..25 = A..Z: the middle two bits of each DV nibble in
 * Atk,Def,Spd,Spc order form a byte, divided by 10. Gen 2 has no ! or ?. */
int     g2_unown_letter(const uint8_t dv[4]);
/* Inverse of g2_unown_letter() (G1 review LOW-5, 2026-09-08: CREATE picking a
 * specific Unown letter the same way Gen 3 does, pick_unown_form() BEFORE the
 * roll -- not by taking whatever letter the seed's own random DVs happen to
 * land on). Fills `dv` with ONE quad -- deterministically, not searched or
 * randomised -- that decodes back to `letter` (0..25 = A..Z; false and `dv`
 * untouched for anything else, including 26/27 -- Gen 2 has no !/? forms).
 * Each DV's bit 0 (and, at the letter's OWN v-range boundary, occasionally
 * bit 3) is free and NOT chosen for any other property -- this does not try
 * to also land on a shiny-capable quad (g2_dv_shiny), only on the letter. */
bool    g2_unown_dv_for_letter(uint8_t letter, uint8_t dv[4]);
/* Gender from the Attack DV. `gender_ratio` is the Gen-3 ratio byte
 * (pk_species_gender_ratio): 0 = always male, 0xFE = always female,
 * 0xFF = genderless, else the female threshold in 256ths — the five ratios in
 * use (31/63/127/191) map 1:1 onto Gen 2's DV thresholds, so one table serves
 * both. Returns 0 male, 1 female, 2 genderless (same encoding as PkMon.gender). */
int     g2_gender_from_dv(uint8_t atk_dv, uint8_t gender_ratio);

/* Gen-2 text -> ASCII. Stops at the 0x50 terminator; unmapped glyphs become
 * spaces (the policy gen3_decode_char already uses). Returns the length written.
 * NOTE: this is the GEN-2 table. Gen 1's differs at 0xBA-0xBF, 0xC0-0xC5,
 * 0xD0-0xD6, 0xE4-0xE5 and 0xE9-0xEB, so gen1_save.c must keep its own. */
int g2_decode_text(const uint8_t* src, int max, char* out, int cap);

/* ---- checksums ----------------------------------------------------------- */

/* 16-bit sum of bytes, stored little-endian. The player block is saved twice;
 * in Crystal the copy is contiguous, in G/S it is scattered over five regions. */
uint16_t g2_checksum_primary(const uint8_t* sav, G2Version ver);
uint16_t g2_checksum_backup(const uint8_t* sav, G2Version ver);
uint32_t g2_checksum_primary_off(G2Version ver);
uint32_t g2_checksum_backup_off(G2Version ver);

/* Where the primary player block is mirrored for the backup copy. Crystal has
 * one region, G/S five. Exposed (rather than acted on) so tests can build a save
 * whose backup validates without this module ever writing anything. */
typedef struct { uint32_t from, to, dest; } G2MirrorRegion;  /* from..to inclusive */
int g2_mirror_map(G2Version ver, const G2MirrorRegion** out);

#endif /* GEN2_SAVE_H */
