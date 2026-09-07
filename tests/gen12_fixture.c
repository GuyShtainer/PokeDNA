/* Synthetic Gen-1 / Gen-2 save images — see gen12_fixture.h for why this exists.
 *
 * Everything here is a second, independent transcription of a primary source, on
 * purpose: a fixture that borrows the module under test's own tables can only prove the
 * module is self-consistent.
 *
 *   - Gen-1 internal index -> National Dex: pret/pokered data/pokemon/dex_order.asm
 *     (label PokedexOrder, 190 db entries, 39 of them MissingNo). Fetched and decoded
 *     this pass; spot-checked against the classic landmarks (Rhydon 0x01 -> 112,
 *     Bulbasaur 0x99 -> 1, Gyarados 0x16 -> 130, Mew 0x15 -> 151, Pikachu 0x54 -> 25).
 *   - Character codes: pret/pokered constants/charmap.asm. This also settles the
 *     ambiguity research-gen12.md §3 flagged — the hyphen is 0xE3; 0xED is the
 *     up-arrow glyph, not a hyphen.
 *   - DV derivations (HP DV, shiny pattern, gender threshold, Unown letter):
 *     Bulbapedia "Individual values", Generation I/II section, as quoted in §2.5.
 *   - EXP curves: the four legacy formulas. Gen 3 kept them bit-identical and no
 *     species 1..251 uses Erratic or Fluctuating (research-gen12.md §4), which is why
 *     a level synthesised here reproduces exactly under the Gen-3 tables.
 *
 * Legal footing: an index->number mapping and a code-point table are fact tables, the
 * same footing as PokeDNA's existing name/stat tables (docs/kb/licensing.md; ip-legal
 * verdict on name tables: GREEN). No decompiled code is reproduced.
 */
#include "gen12_fixture.h"

#include <string.h>

/* GB records are big-endian; the Gen-3 side is little-endian, so nothing is shared. */
static void wr16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr24be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

/* ------------------------------------------------------------------ oracles ---- */

static const uint8_t k_gen1_dex[GBF_GEN1_INDEX_MAX] = {
  112, 115,  32,  35,  21, 100,  34,  80,   2, 103,   /* 0x01.. */
  108, 102,  88,  94,  29,  31, 104, 111, 131,  59,   /* 0x0B.. */
  151, 130,  90,  72,  92, 123, 120,   9, 127, 114,   /* 0x15.. */
    0,   0,  58,  95,  22,  16,  79,  64,  75, 113,   /* 0x1F.. */
   67, 122, 106, 107,  24,  47,  54,  96,  76,   0,   /* 0x29.. */
  126,   0, 125,  82, 109,   0,  56,  86,  50, 128,   /* 0x33.. */
    0,   0,   0,  83,  48, 149,   0,   0,   0,  84,   /* 0x3D.. */
   60, 124, 146, 144, 145, 132,  52,  98,   0,   0,   /* 0x47.. */
    0,  37,  38,  25,  26,   0,   0, 147, 148, 140,   /* 0x51.. */
  141, 116, 117,   0,   0,  27,  28, 138, 139,  39,   /* 0x5B.. */
   40, 133, 136, 135, 134,  66,  41,  23,  46,  61,   /* 0x65.. */
   62,  13,  14,  15,   0,  85,  57,  51,  49,  87,   /* 0x6F.. */
    0,   0,  10,  11,  12,  68,   0,  55,  97,  42,   /* 0x79.. */
  150, 143, 129,   0,   0,  89,   0,  99,  91,   0,   /* 0x83.. */
  101,  36, 110,  53, 105,   0,  93,  63,  65,  17,   /* 0x8D.. */
   18, 121,   1,   3,  73,   0, 118, 119,   0,   0,   /* 0x97.. */
    0,   0,  77,  78,  19,  20,  33,  30,  74, 137,   /* 0xA1.. */
  142,   0,  81,   0,   0,   4,   7,   5,   8,   6,   /* 0xAB.. */
    0,   0,   0,   0,  43,  44,  45,  69,  70,  71,   /* 0xB5.. */
};

uint8_t gbf_gen1_dex(uint16_t idx) {
  if (idx == 0 || idx > GBF_GEN1_INDEX_MAX) return 0;
  return k_gen1_dex[idx - 1];
}

uint32_t gbf_exp_for(uint8_t growth, uint8_t level) {
  int32_t n = (int32_t)level, v;
  switch (growth) {
    case GBF_MEDSLOW: v = (6 * n * n * n) / 5 - 15 * n * n + 100 * n - 140; break;
    case GBF_FAST:    v = (4 * n * n * n) / 5; break;
    case GBF_SLOW:    v = (5 * n * n * n) / 4; break;
    default:          v = n * n * n; break;    /* Medium Fast */
  }
  return v < 0 ? 0u : (uint32_t)v;             /* Medium Slow goes negative below L5 */
}

uint8_t gbf_hp_dv(const uint8_t dv[4]) {
  return (uint8_t)(((dv[0] & 1) << 3) | ((dv[1] & 1) << 2) | ((dv[2] & 1) << 1) | (dv[3] & 1));
}

bool gbf_is_shiny(const uint8_t dv[4]) {
  return dv[1] == 10 && dv[2] == 10 && dv[3] == 10 && (dv[0] & 2) != 0;
}

int gbf_unown_letter(const uint8_t dv[4]) {
  int v = (((dv[0] >> 1) & 3) << 6) | (((dv[1] >> 1) & 3) << 4)
        | (((dv[2] >> 1) & 3) << 2) |  ((dv[3] >> 1) & 3);
  return v / 10;                               /* 0..25; Gen 2 has no ! or ? forms */
}

int gbf_gender(const uint8_t dv[4], int8_t thresh) {
  if (thresh == GBF_G_GENDERLESS) return 2;
  if (thresh == GBF_G_ALL_MALE)   return 0;
  if (thresh == GBF_G_ALL_FEMALE) return 1;
  return dv[0] <= (uint8_t)thresh ? 1 : 0;
}

uint8_t gbf_gen3_ratio(int8_t thresh) {
  switch (thresh) {
    case GBF_G_GENDERLESS: return 0xFF;
    case GBF_G_ALL_MALE:   return 0x00;
    case GBF_G_ALL_FEMALE: return 0xFE;
    case 1:                return 31;   /* 7 male : 1 female */
    case 3:                return 63;   /* 3:1 */
    case 7:                return 127;  /* 1:1 */
    case 11:               return 191;  /* 1:3 */
    default:               return 127;
  }
}

/* ------------------------------------------------------------------ charset ---- */

int gbf_encode_name(uint8_t* dst, int cap, const char* utf8) {
  const unsigned char* p = (const unsigned char*)utf8;
  int i = 0;
  while (i < cap && *p) {
    uint8_t b;
    /* U+2642 male / U+2640 female arrive as three UTF-8 bytes; && short-circuits, so a
     * truncated sequence at the end of the string never reads past the NUL. */
    if (p[0] == 0xE2u && p[1] == 0x99u && (p[2] == 0x82u || p[2] == 0x80u)) {
      b = (p[2] == 0x82u) ? GBF_CH_MALE : GBF_CH_FEMALE;
      p += 3;
    } else if (*p >= 'A' && *p <= 'Z') { b = (uint8_t)(GBF_CH_A + (*p++ - 'A'));
    } else if (*p >= 'a' && *p <= 'z') { b = (uint8_t)(GBF_CH_a + (*p++ - 'a'));
    } else if (*p >= '0' && *p <= '9') { b = (uint8_t)(GBF_CH_0 + (*p++ - '0'));
    } else {
      switch (*p++) {                  /* pret/pokered constants/charmap.asm */
        case ' ':  b = GBF_CH_SPACE; break;
        case '(':  b = 0x9Au; break;
        case ')':  b = 0x9Bu; break;
        case ':':  b = 0x9Cu; break;
        case ';':  b = 0x9Du; break;
        case '[':  b = 0x9Eu; break;
        case ']':  b = 0x9Fu; break;
        case '\'': b = 0xE0u; break;
        case '-':  b = 0xE3u; break;
        case '?':  b = 0xE6u; break;
        case '!':  b = 0xE7u; break;
        case '.':  b = 0xE8u; break;
        case '/':  b = 0xF3u; break;
        case ',':  b = 0xF4u; break;
        default:   b = GBF_CH_SPACE; break;
      }
    }
    dst[i++] = b;
  }
  return i;
}

/* Write an 11-byte GB name field: glyphs, 0x50, then JUNK. Real saves leave the previous
 * name's tail in place, so a decoder that ignores the terminator reads garbage — which
 * is exactly the bug this trap is here to catch. */
static void put_name(uint8_t* dst, int cap, const char* utf8, uint8_t junk_seed) {
  int n = gbf_encode_name(dst, cap, utf8);
  if (n < cap) {
    dst[n++] = GBF_CH_TERM;
    for (uint8_t k = 0; n < cap; n++, k++) dst[n] = (uint8_t)(GBF_CH_A + ((junk_seed + k) % 26));
  }
}

/* ------------------------------------------------------------------ rosters ---- */

#define MV4(a,b,c,d) { (a), (b), (c), (d) }
#define NOPP         { 0, 0, 0, 0 }
#define SE0          { 0, 0, 0, 0, 0 }

#define GBF_OT      "GUY"
#define GBF_OT_MAX  "MAXNAME"          /* 7 glyphs — the OT field's ceiling in both gens */
#define GBF_TID     24601
#define GBF_PLAYER  "GUY"

/* The 10-character punctuation torture name: every glyph exists in BOTH charsets, it
 * uses the glyph area exactly (terminator lands on the field's last byte, no padding),
 * and it mixes case, a digit, a space and three of the scattered punctuation singles. */
#define AWKWARD_ASCII  "Ab-9 .Zz'K"
/* ...and the one that needs the symbols. Gen-3's encoder stores each as a single byte
 * (0xB5/0xB6) only if it is handed UTF-8; a byte-wise decode turns each into three
 * spaces, the documented regression in gen3_edit.c:255. */
/* Split at the escapes so "\x80" does not swallow the '9' as a third hex digit. */
#define AWKWARD_SYM    "\xE2\x99\x82" "Rx " "\xE2\x99\x80" "9-a."

/* ---- Gen 1 --------------------------------------------------------------------- */

static const GbfMon k_g1_box0[] = {
  { 0x99,   1, GBF_MEDSLOW, 20, 0, {9,4,13,6},  MV4(33,45,73,22), NOPP, GBF_TID,
    {5000,300,300,300,300}, 45, 0, 0, 0,0,0,0, 1, GBF_F_UNNICKED,
    "BULBASAUR", GBF_OT,
    "internal index 0x99 must map to dex 1 — the entire point of PokedexOrder" },

  { 0x40,  83, GBF_MEDFAST, 15, 0, {5,5,5,5},   MV4(64,31,19,98),  NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 7, GBF_F_UNNICKED,
    "FARFETCH'D", GBF_OT_MAX,
    "10-glyph name + 7-glyph OT: both fields at their ceiling. All DVs odd -> HP DV 15" },

  { 0x54,  25, GBF_MEDFAST, 100, 0, {13,8,15,11}, MV4(84,98,86,85), {3,1,0,2}, GBF_TID,
    {65535,65535,65535,65535,65535}, 45, 0, 0, 0,0,0,0, 7, GBF_F_UNNICKED,
    "PIKACHU", GBF_OT,
    "level 100 at the exact Medium-Fast maximum EXP, maxed stat exp, PP Ups on 3 moves" },

  { 0x85, 129, GBF_SLOW,     1, 0, {1,2,3,4},   MV4(150,0,0,0),   NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 7, 0,
    "SPLASH", GBF_OT,
    "level 1, a single move — everything at its floor" },

  { 0x16, 130, GBF_SLOW,    30, 0, {10,10,10,10}, MV4(44,43,82,56), NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 7, GBF_F_UNNICKED,
    "GYARADOS", GBF_OT,
    "the Gen-2 shiny DV pattern on a Gen-1 record — shininess must survive the import" },

  { 0x03,  32, GBF_MEDSLOW, 10, 0, {0,7,9,3},   MV4(10,45,0,0),   NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, GBF_G_ALL_MALE, GBF_F_UNNICKED,
    "NIDORAN\xE2\x99\x82", GBF_OT,
    "Attack DV 0 on a 100%-male species: the ratio decides, not the DV. Name has a symbol" },

  { 0xA5,  19, GBF_MEDFAST, 40, 0, {15,0,15,0}, MV4(33,98,116,44), NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 7, 0,
    AWKWARD_ASCII, GBF_OT,
    "nickname using the charset's awkward singles (hyphen 0xE3, period 0xE8, quote 0xE0)" },

  { 0x1F,   0, GBF_MEDFAST, 25, 0, {5,5,5,5},   MV4(33,0,0,0),    NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 7, GBF_F_REFUSE,
    "MISSINGNO", GBF_OT,
    "internal index 0x1F is a MissingNo hole (PokedexOrder entry 0) — must be refused" },
};

/* Box 2 is the CURRENT box: the live copy is in main data at 0x30C0 and the bank-2 copy
 * is stale. The stale copy holds different Pokemon on purpose (see gbf_build_ex). */
static const GbfMon k_g1_box2[] = {
  { 0x1C,   9, GBF_MEDSLOW, 36, 0, {12,3,6,9},  MV4(55,57,110,44), NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 1, 0, "SHELLY", GBF_OT,
    "only in the LIVE current-box copy; a reader that takes the banked copy misses it" },
  { 0x24,  16, GBF_MEDSLOW,  5, 0, {2,2,2,2},   MV4(33,0,0,0),    NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 7, 0, "PIDGE", GBF_OT, "second live-copy slot" },
  { 0xB0,   4, GBF_MEDSLOW, 12, 0, {7,7,7,7},   MV4(52,10,0,0),   NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 1, 0, "CHAR", GBF_OT, "third live-copy slot" },
};

/* Box 7 sits in bank 3 — proves the second bank's base address and box stride. */
static const GbfMon k_g1_box7[] = {
  { 0x15, 151, GBF_MEDSLOW,  5, 0, {14,14,14,14}, MV4(1,0,0,0),   NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, GBF_G_GENDERLESS, 0, "MEW", GBF_OT,
    "bank 3, box 7 slot 0" },
  { 0x83, 150, GBF_MEDSLOW, 70, 0, {15,15,15,15}, MV4(94,58,105,115), NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, GBF_G_GENDERLESS, 0, "MEWTWO", GBF_OT,
    "bank 3, box 7 slot 1" },
};

static const GbfMon k_g1_party[] = {
  { 0x9A,   3, GBF_MEDSLOW, 50, 0, {11,6,8,13}, MV4(76,22,74,33), NOPP, GBF_TID,
    {20000,10000,10000,10000,10000}, 45, 0, 0, 0,0,0,0, 1, 0, "VENU", GBF_OT,
    "party slot 0 — 44-byte record, level stored a second time at +33" },
  { 0xB4,   6, GBF_MEDSLOW, 52, 0, {9,9,9,9},   MV4(53,17,44,43), NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 1, 0, "ZARD", GBF_OT, "party slot 1" },
  { 0x1C,   9, GBF_MEDSLOW, 51, 0, {4,12,4,12}, MV4(55,58,110,44), NOPP, GBF_TID,
    SE0, 45, 0, 0, 0,0,0,0, 1, 0, "BLAST", GBF_OT, "party slot 2" },
};

/* ---- Gen 2 --------------------------------------------------------------------- */

static const GbfMon k_g2_box0[] = {
  { 130, 130, GBF_SLOW,    30, 0, {10,10,10,10}, MV4(44,43,82,56), NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, 7, GBF_F_UNNICKED, "GYARADOS", GBF_OT,
    "shiny: Def=Spd=Spc=10 with bit 1 of the Attack DV set" },

  {  58,  58, GBF_SLOW,    25, 0, {2,5,9,13},  MV4(52,44,46,36),  NOPP, GBF_TID,
    SE0, 0, 120, 0, 0,0,0,0, 3, 0, "LASSIE", GBF_OT,
    "3 male : 1 female, Attack DV 2 <= 3 -> FEMALE" },

  {  58,  58, GBF_SLOW,    25, 0, {12,5,9,13}, MV4(52,44,46,36),  NOPP, GBF_TID,
    SE0, 0, 120, 0, 0,0,0,0, 3, 0, "REX", GBF_OT,
    "same species, Attack DV 12 > 3 -> MALE. Only the DV differs" },

  { 201, 201, GBF_MEDFAST,  5, 0, {2,3,4,5},   MV4(237,0,0,0),    NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, GBF_G_GENDERLESS, GBF_F_UNNICKED, "UNOWN", GBF_OT,
    "DV middle bits -> 90, /10 -> letter 9 = J. Genderless, so the PID search is free" },

  {  66,  66, GBF_MEDSLOW, 20, 0, {6,6,6,6},   MV4(2,43,67,0),    NOPP, GBF_TID,
    SE0, 0x21, 70, 0, 0,0,0,0, 3, GBF_F_REFUSE, "MUSCLE", GBF_OT,
    "holds an item — Poke Transporter refuses these outright (research-gen12.md §4)" },

  {  25,  25, GBF_MEDFAST, 100, 0, {13,8,15,11}, MV4(84,98,86,85), {3,1,0,2}, GBF_TID,
    {65535,65535,65535,65535,65535}, 0, 255, 0x14, 0,0,0,0, 7, GBF_F_UNNICKED,
    "PIKACHU", GBF_OT,
    "level 100, max friendship, Pokerus byte set, PP Ups on 3 moves" },

  { 129, 129, GBF_SLOW,     1, 0, {1,2,3,4},   MV4(150,0,0,0),    NOPP, GBF_TID,
    SE0, 0, 20, 0, 0,0,0,0, 7, 0, "SPLASH", GBF_OT, "level 1" },

  { 196, 196, GBF_MEDFAST, 45, 0, {8,11,4,7},  MV4(98,60,113,105), NOPP, GBF_TID,
    SE0, 0, 200, 0, 0,0,0,0, 1, 0, AWKWARD_SYM, GBF_OT,
    "nickname built from the gender symbols — must not degrade into runs of spaces" },

  {  32,  32, GBF_MEDSLOW, 10, 0, {0,7,9,3},   MV4(10,45,0,0),    NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, GBF_G_ALL_MALE, GBF_F_UNNICKED,
    "NIDORAN\xE2\x99\x82", GBF_OT,
    "Attack DV 0 on a 100%-male species: gender comes from the ratio, not the DV" },

  {  19,  19, GBF_MEDFAST, 100, 0xFFFFFFu, {15,0,15,0}, MV4(33,98,116,44), NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, 7, 0, "OVERFLOW", GBF_OT,
    "EXP 0xFFFFFF (Gen-1 glitch overflow) must clamp to level 100, not wrap" },
};

/* Box 5 is the CURRENT box — live copy in main data, stale decoy in the bank. */
static const GbfMon k_g2_box5[] = {
  { 175, 175, GBF_FAST,     5, 0, {6,6,6,6},   MV4(1,0,0,0),      NOPP, GBF_TID,
    SE0, 0, 10, 0, 0,0,0,0, 1, GBF_F_EGG | GBF_F_REFUSE, "TOGEPI", GBF_OT,
    "EGG: species-list byte 0xFD, friendship byte is the remaining cycles. Never converts" },
  { 152, 152, GBF_MEDSLOW,  5, 0, {9,3,11,5},  MV4(33,45,0,0),    NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, 1, GBF_F_UNNICKED, "CHIKORITA", GBF_OT,
    "an ordinary neighbour of the egg — the egg must not swallow the slot after it" },
  { 252,   0, GBF_MEDFAST, 30, 0, {5,5,5,5},   MV4(33,0,0,0),     NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, 7, GBF_F_REFUSE, "GLITCH", GBF_OT,
    "species 252 is past the Gen-2 dex (251) — must be refused" },
};

static const GbfMon k_g2_box8[] = {
  { 155, 155, GBF_MEDSLOW, 15, 0, {7,7,7,7},   MV4(52,10,0,0),    NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, 1, 0, "CYNDA", GBF_OT, "bank 3, box 8 slot 0" },
  { 158, 158, GBF_MEDSLOW, 15, 0, {3,3,3,3},   MV4(44,10,0,0),    NOPP, GBF_TID,
    SE0, 0, 70, 0, 0,0,0,0, 1, 0, "TOTO", GBF_OT, "bank 3, box 8 slot 1" },
};

/* Crystal alone stores caught data; in G/S those two bytes stay zero. */
static const GbfMon k_g2_party[] = {
  { 157, 157, GBF_MEDSLOW, 50, 0, {14,2,13,9}, MV4(53,44,46,89),  NOPP, GBF_TID,
    {30000,25000,25000,25000,25000}, 0, 200, 0, 5, 3, 1, 2, 1, 0, "TYPHLO", GBF_OT,
    "party slot 0 — 48-byte record; Crystal caught data (met L5, loc 3, OT female)" },
  { 181, 181, GBF_MEDSLOW, 40, 0, {6,10,6,10}, MV4(84,86,113,105), NOPP, GBF_TID,
    SE0, 0, 150, 0, 20, 7, 0, 1, 7, 0, "AMPY", GBF_OT, "party slot 1" },
  { 201, 201, GBF_MEDFAST,  5, 0, {8,9,0,1},   MV4(237,0,0,0),    NOPP, GBF_TID,
    SE0, 0, 70, 0, 5, 3, 0, 0, GBF_G_GENDERLESS, GBF_F_UNNICKED, "UNOWN", GBF_OT,
    "DV middle bits -> 0, /10 -> letter 0 = A. Pairs with the J in box 0" },
};

static const char* k_g2_boxnames[GBF2_NBOXES] = {
  "MAIN", "SPARE", "TRADES", "EVS", "DITTOS", "LIVE", "OLD",
  "BUGS", "BANK3", "TENTH", "ELEVEN", "TWELVE", "THIRT", "LAST",
};

#define STALE_MARKER "STALEBOX"

const char* gbf_stale_marker(void) { return STALE_MARKER; }
uint16_t    gbf_player_tid(void)   { return GBF_TID; }
const char* gbf_player_name(void)  { return GBF_PLAYER; }

int gbf_nboxes(GbfGame g)       { return g == GBF_RBY ? GBF1_NBOXES : GBF2_NBOXES; }
int gbf_box_capacity(GbfGame g) { return g == GBF_RBY ? GBF1_BOX_CAP : GBF2_BOX_CAP; }
int gbf_current_box(GbfGame g)  { return g == GBF_RBY ? 2 : 5; }

const char* gbf_box_name(GbfGame g, int box) {
  if (g == GBF_RBY || box < 0 || box >= GBF2_NBOXES) return 0;
  return k_g2_boxnames[box];
}

const GbfMon* gbf_roster(GbfGame g, int box, int* count) {
#define R(arr) do { if (count) *count = (int)(sizeof(arr) / sizeof((arr)[0])); return (arr); } while (0)
  if (g == GBF_RBY) {
    if (box <  0) R(k_g1_party);
    if (box == 0) R(k_g1_box0);
    if (box == 2) R(k_g1_box2);
    if (box == 7) R(k_g1_box7);
  } else {
    if (box <  0) R(k_g2_party);
    if (box == 0) R(k_g2_box0);
    if (box == 5) R(k_g2_box5);
    if (box == 8) R(k_g2_box8);
  }
#undef R
  if (count) *count = 0;
  return 0;
}

/* ------------------------------------------------------------------ writers ---- */

/* Deterministic junk. Real SRAM is never zeroed, and zero-filled padding hides
 * off-by-one reads that a byte pattern exposes immediately. */
static uint32_t lcg(uint32_t* s) { *s = *s * 1103515245u + 12345u; return (*s >> 16) & 0xFFu; }

static void fill_junk(uint8_t* p, uint32_t n, uint32_t seed) {
  uint32_t s = seed;
  for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)lcg(&s);
}

/* Battle stats are NOT game-accurate. The conversion drops HP/Atk/... entirely (Gen 3
 * recomputes them from IVs/EVs/level), and making them exact would mean carrying a
 * Gen-1/2 base-stat table this fixture has no other use for. They are plausible,
 * level-scaled placeholders so no record reads as obviously empty. */
static uint16_t placeholder_stat(const GbfMon* m, int which) {
  return (uint16_t)(5 + (uint32_t)m->level * 2 + m->dv[which & 3] + which * 3);
}

static uint32_t roster_exp(const GbfMon* m, GbfGrowthFn growth) {
  if (m->exp_raw) return m->exp_raw;
  uint8_t g = m->growth;
  if (growth && m->dex) {                     /* let the caller supply the shipped table */
    uint8_t shipped = growth(m->dex);
    if (shipped <= GBF_SLOW) g = shipped;
  }
  return gbf_exp_for(g, m->level);
}

/* PP byte: bits 0-5 current PP, bits 6-7 PP Ups applied. 30 is above every Gen-1/2 base
 * PP, so a converter that copies the raw byte instead of re-deriving base PP from the
 * Gen-3 tables produces a visibly out-of-range value. */
static uint8_t pp_byte(const GbfMon* m, int i) {
  if (!m->moves[i]) return 0;
  return (uint8_t)((m->ppup[i] << 6) | 30u);
}

static void write_g1_record(uint8_t* r, const GbfMon* m, GbfGrowthFn growth, bool party) {
  memset(r, 0, party ? GBF1_PREC_BYTES : GBF1_REC_BYTES);
  r[0x00] = (uint8_t)m->species;
  wr16be(r + 0x01, placeholder_stat(m, 0));         /* current HP */
  r[0x03] = m->level;                               /* the box copy's cached level */
  r[0x04] = 0;                                      /* status: box storage clears it */
  r[0x05] = 0; r[0x06] = 0;                         /* types — display-only, dropped */
  r[0x07] = m->item;                                /* catch rate (a pseudo-item only after
                                                     * a Time Capsule trade — NOT a held
                                                     * item, and must not trigger a refusal) */
  for (int i = 0; i < 4; i++) r[0x08 + i] = (uint8_t)m->moves[i];
  wr16be(r + 0x0C, m->otid);
  wr24be(r + 0x0E, roster_exp(m, growth));
  for (int i = 0; i < 5; i++) wr16be(r + 0x11 + i * 2, m->statexp[i]);
  r[0x1B] = (uint8_t)((m->dv[0] << 4) | m->dv[1]);
  r[0x1C] = (uint8_t)((m->dv[2] << 4) | m->dv[3]);
  for (int i = 0; i < 4; i++) r[0x1D + i] = pp_byte(m, i);
  if (party) {
    r[33] = m->level;                               /* level, stored a second time */
    /* maxHP, Atk, Def, Spd, Spc — five 16-bit stats, NOT six: Gen 1 has one Special. */
    for (int i = 0; i < 5; i++) wr16be(r + 34 + i * 2, placeholder_stat(m, i));
  }
}

static void write_g2_record(uint8_t* r, const GbfMon* m, GbfGrowthFn growth,
                            bool party, bool crystal) {
  memset(r, 0, party ? GBF2_PREC_BYTES : GBF2_REC_BYTES);
  r[0x00] = (uint8_t)m->species;
  r[0x01] = m->item;
  for (int i = 0; i < 4; i++) r[0x02 + i] = (uint8_t)m->moves[i];
  wr16be(r + 0x06, m->otid);
  wr24be(r + 0x08, roster_exp(m, growth));
  for (int i = 0; i < 5; i++) wr16be(r + 0x0B + i * 2, m->statexp[i]);
  r[0x15] = (uint8_t)((m->dv[0] << 4) | m->dv[1]);
  r[0x16] = (uint8_t)((m->dv[2] << 4) | m->dv[3]);
  for (int i = 0; i < 4; i++) r[0x17 + i] = pp_byte(m, i);
  r[0x1B] = m->friendship;
  r[0x1C] = m->pokerus;
  if (crystal) {
    r[0x1D] = (uint8_t)((m->met_time << 6) | (m->met_level & 0x3F));
    r[0x1E] = (uint8_t)((m->ot_gender << 7) | (m->met_loc & 0x7F));
  }
  r[0x1F] = m->level;
  if (party) {
    r[0x20] = 0;                                    /* status */
    /* curHP, maxHP, Atk, Def, Spd, SpA, SpD — Gen 2 split Special into two stats. */
    for (int i = 0; i < 7; i++) wr16be(r + 0x22 + i * 2, placeholder_stat(m, i));
  }
}

/* One box or party list: count, 0xFF-terminated species list, records, OT names,
 * nicknames. `cap` picks box (20) vs party (6). */
static void write_list(uint8_t* dst, GbfGame game, const GbfMon* mons, int n, int cap,
                       GbfGrowthFn growth, uint32_t junk_seed) {
  bool g1 = (game == GBF_RBY);
  bool party = (cap == 6);
  uint32_t rsz = g1 ? (party ? GBF1_PREC_BYTES : GBF1_REC_BYTES)
                    : (party ? GBF2_PREC_BYTES : GBF2_REC_BYTES);

  uint32_t off_species = 1;
  uint32_t off_recs    = off_species + (uint32_t)cap + 1u;
  uint32_t off_ot      = off_recs + (uint32_t)cap * rsz;
  uint32_t off_nick    = off_ot + (uint32_t)cap * 11u;
  uint32_t total       = off_nick + (uint32_t)cap * 11u;

  fill_junk(dst, total, junk_seed);
  if (n > cap) n = cap;
  dst[0] = (uint8_t)n;

  for (int i = 0; i <= cap; i++) {
    if (i < n) {
      dst[off_species + i] = (mons[i].flags & GBF_F_EGG) ? 0xFDu : (uint8_t)mons[i].species;
    } else if (i == n) {
      dst[off_species + i] = 0xFFu;
    } else {
      /* Past the terminator: a plausible species byte, not 0xFF. A reader that trusts
       * neither the count nor the terminator invents Pokemon out of this. */
      dst[off_species + i] = 0x99u;
    }
  }

  for (int i = 0; i < cap; i++) {
    uint8_t* r = dst + off_recs + (uint32_t)i * rsz;
    if (i < n) {
      if (g1) write_g1_record(r, &mons[i], growth, party);
      else    write_g2_record(r, &mons[i], growth, party, game == GBF_CRYSTAL);
    } else {
      fill_junk(r, rsz, junk_seed + 0x1000u + (uint32_t)i);
    }
    put_name(dst + off_ot   + (uint32_t)i * 11u, 11,
             (i < n) ? mons[i].ot   : "NOBODY", (uint8_t)(junk_seed + i));
    put_name(dst + off_nick + (uint32_t)i * 11u, 11,
             (i < n) ? mons[i].nick : "GHOST",  (uint8_t)(junk_seed + i + 7));
  }
}

/* ---------------------------------------------------------------- checksums ---- */

/* Gen 1: "initialise the checksum to 0, add every byte from 0x2598 to 0x3522, invert
 * the bits of the result." */
static uint8_t sum8_inv(const uint8_t* p, uint32_t n) {
  uint8_t s = 0;
  for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
  return (uint8_t)~s;
}

uint8_t gbf_gen1_checksum(const uint8_t* save) {
  return sum8_inv(save + GBF1_CKSUM_START, GBF1_CKSUM_END - GBF1_CKSUM_START + 1u);
}

static uint16_t sum16(const uint8_t* p, uint32_t n) {
  uint16_t s = 0;
  for (uint32_t i = 0; i < n; i++) s = (uint16_t)(s + p[i]);
  return s;
}

uint16_t gbf_gen2_checksum(const uint8_t* save, GbfGame game) {
  uint32_t end = (game == GBF_CRYSTAL) ? GBF2_C_CKSUM_END : GBF2_GS_CKSUM_END;
  return sum16(save + GBF2_CKSUM_START, end - GBF2_CKSUM_START + 1u);
}

/* G/S scatters its backup over five regions instead of mirroring one block; Crystal
 * mirrors the whole primary range contiguously. Both maps are from research-gen12.md
 * §2.2, and every region's source and destination lengths were checked to agree. */
static const GbfMirror k_gs_mirror[5] = {
  { 0x2009u, 0x222Eu, 0x15C7u },
  { 0x222Fu, 0x23D8u, 0x3D96u },   /* sBackupPlayerData2 — see gen2_save.c's k_gs_mirror note */
  { 0x23D9u, 0x2855u, 0x0C6Bu },
  { 0x2856u, 0x2889u, 0x7E39u },
  { 0x288Au, 0x2D68u, 0x10E8u },
};
static const GbfMirror k_c_mirror[1] = {
  { GBF2_CKSUM_START, GBF2_C_CKSUM_END, GBF2_C_BACKUP },
};

int gbf_mirror_map(GbfGame game, const GbfMirror** out) {
  if (game == GBF_GS)      { if (out) *out = k_gs_mirror; return 5; }
  if (game == GBF_CRYSTAL) { if (out) *out = k_c_mirror;  return 1; }
  if (out) *out = 0;
  return 0;
}
/* Three of those destinations are contiguous (0x0C6B..0x17EC), which is why the backup
 * checksum covers three ranges rather than five. Flagged VERIFY in §2.2 — v1 only needs
 * the PRIMARY checksum to validate, so a disagreement here degrades a fallback, not a
 * load. */
static const struct { uint32_t start, len; } k_gs_backup_sum[3] = {
  { 0x0C6Bu, 0x17ECu - 0x0C6Bu + 1u },
  { 0x3D96u, 0x3F3Fu - 0x3D96u + 1u },
  { 0x7E39u, 0x7E6Cu - 0x7E39u + 1u },
};

static void finish_gen2(uint8_t* s, GbfGame game) {
  uint16_t c1 = gbf_gen2_checksum(s, game);
  uint32_t at = (game == GBF_CRYSTAL) ? GBF2_C_CKSUM : GBF2_GS_CKSUM;
  s[at] = (uint8_t)c1; s[at + 1] = (uint8_t)(c1 >> 8);     /* stored little-endian */

  if (game == GBF_CRYSTAL) {
    uint32_t len = GBF2_C_CKSUM_END - GBF2_CKSUM_START + 1u;
    memcpy(s + GBF2_C_BACKUP, s + GBF2_CKSUM_START, len);
    uint16_t c2 = sum16(s + GBF2_C_BACKUP, len);
    s[GBF2_C_CKSUM2] = (uint8_t)c2; s[GBF2_C_CKSUM2 + 1] = (uint8_t)(c2 >> 8);
  } else {
    for (int i = 0; i < 5; i++)
      memcpy(s + k_gs_mirror[i].dest, s + k_gs_mirror[i].from,
             k_gs_mirror[i].to - k_gs_mirror[i].from + 1u);
    uint16_t c2 = 0;
    for (int i = 0; i < 3; i++)
      c2 = (uint16_t)(c2 + sum16(s + k_gs_backup_sum[i].start, k_gs_backup_sum[i].len));
    s[GBF2_GS_CKSUM2] = (uint8_t)c2; s[GBF2_GS_CKSUM2 + 1] = (uint8_t)(c2 >> 8);
  }
}

void gbf_break_primary(GbfGame game, uint8_t* save) {
  /* Land the flip on the player name: inside every game's primary checksum range and
   * nowhere near a stored checksum or a backup destination. */
  uint32_t at = (game == GBF_RBY) ? GBF1_PLAYER_NAME : GBF2_PLAYER_NAME;
  save[at] ^= 0xFFu;
}

/* ------------------------------------------------------------------- build ----- */

uint32_t gbf_build(GbfGame game, uint8_t* out, uint32_t rtc_tail) {
  return gbf_build_ex(game, out, rtc_tail, 0);
}

uint32_t gbf_build_ex(GbfGame game, uint8_t* out, uint32_t rtc_tail, GbfGrowthFn growth) {
  const GbfMon* mons; int n;
  int cur = gbf_current_box(game);

  fill_junk(out, GBF_SAVE_BYTES, 0x5EED0001u);

  if (game == GBF_RBY) {
    put_name(out + GBF1_PLAYER_NAME, 11, GBF_PLAYER, 3);
    wr16be(out + GBF1_TID, GBF_TID);
    /* Bit 7 is the "box has been switched" flag; a reader that takes the whole byte as
     * the index lands on box 130. */
    out[GBF1_CURBOX_NO] = (uint8_t)(0x80u | (uint32_t)cur);

    mons = gbf_roster(game, -1, &n);
    write_list(out + GBF1_PARTY, game, mons, n, 6, growth, 0x11u);

    mons = gbf_roster(game, cur, &n);
    write_list(out + GBF1_CURBOX_DATA, game, mons, n, GBF1_BOX_CAP, growth, 0x22u);

    for (int b = 0; b < GBF1_NBOXES; b++) {
      uint32_t base = (b < 6) ? GBF1_BANK2 + (uint32_t)b * GBF1_BOX_BYTES
                              : GBF1_BANK3 + (uint32_t)(b - 6) * GBF1_BOX_BYTES;
      if (b == cur) {
        /* The stale copy the game has not written back yet. Different Pokemon on
         * purpose: anything that surfaces STALEBOX read the wrong copy. */
        GbfMon decoy = k_g1_box0[0];
        decoy.nick = STALE_MARKER;
        decoy.flags = 0;
        write_list(out + base, game, &decoy, 1, GBF1_BOX_CAP, growth, 0x33u);
        continue;
      }
      mons = gbf_roster(game, b, &n);
      if (mons) write_list(out + base, game, mons, n, GBF1_BOX_CAP, growth, 0x40u + (uint32_t)b);
      else      write_list(out + base, game, 0,    0, GBF1_BOX_CAP, growth, 0x40u + (uint32_t)b);
    }

    out[GBF1_CKSUM] = gbf_gen1_checksum(out);
    for (int bank = 0; bank < 2; bank++) {
      uint32_t base    = bank ? GBF1_BANK3 : GBF1_BANK2;
      uint32_t trailer = bank ? GBF1_BANK3_CKSUM : GBF1_BANK2_CKSUM;
      out[trailer] = sum8_inv(out + base, 6u * GBF1_BOX_BYTES);
      for (int i = 0; i < 6; i++)
        out[trailer + 1 + i] = sum8_inv(out + base + (uint32_t)i * GBF1_BOX_BYTES,
                                        GBF1_BOX_BYTES);
    }
  } else {
    bool cry = (game == GBF_CRYSTAL);
    uint32_t party_at  = cry ? GBF2_C_PARTY     : GBF2_GS_PARTY;
    uint32_t curbox_no = cry ? GBF2_C_CURBOX_NO : GBF2_GS_CURBOX_NO;
    uint32_t boxnames  = cry ? GBF2_C_BOXNAMES  : GBF2_GS_BOXNAMES;
    uint32_t curbox_at = cry ? GBF2_C_CURBOX    : GBF2_GS_CURBOX;

    put_name(out + GBF2_PLAYER_NAME, 11, GBF_PLAYER, 3);
    wr16be(out + GBF2_TID, GBF_TID);
    /* Only the LOW NIBBLE is the box index. The high nibble is filled so that a mask
     * even one bit too wide (0x1F instead of 0x0F) yields an out-of-range box. */
    out[curbox_no] = (uint8_t)(0xF0u | (uint32_t)cur);
    for (int b = 0; b < GBF2_NBOXES; b++)
      put_name(out + boxnames + (uint32_t)b * 9u, 9, k_g2_boxnames[b], (uint8_t)(b * 5));

    mons = gbf_roster(game, -1, &n);
    write_list(out + party_at, game, mons, n, 6, growth, 0x11u);

    mons = gbf_roster(game, cur, &n);
    write_list(out + curbox_at, game, mons, n, GBF2_BOX_CAP, growth, 0x22u);

    for (int b = 0; b < GBF2_NBOXES; b++) {
      uint32_t base = (b < 7) ? GBF2_BANK2 + (uint32_t)b * GBF2_BOX_STRIDE
                              : GBF2_BANK3 + (uint32_t)(b - 7) * GBF2_BOX_STRIDE;
      if (b == cur) {
        GbfMon decoy = k_g2_box0[0];
        decoy.nick = STALE_MARKER;
        decoy.flags = 0;
        write_list(out + base, game, &decoy, 1, GBF2_BOX_CAP, growth, 0x33u);
        continue;
      }
      mons = gbf_roster(game, b, &n);
      if (mons) write_list(out + base, game, mons, n, GBF2_BOX_CAP, growth, 0x40u + (uint32_t)b);
      else      write_list(out + base, game, 0,    0, GBF2_BOX_CAP, growth, 0x40u + (uint32_t)b);
    }

    finish_gen2(out, game);
  }

  /* GSC carts are MBC3+RTC, so emulator .sav files are routinely 44 or 48 bytes longer
   * than the SRAM image (bgb.bircd.org/rtcsave.html). The tail is junk to the loader and
   * must be ignored, not parsed. */
  if (rtc_tail) fill_junk(out + GBF_SAVE_BYTES, rtc_tail, 0x2C0C0000u);
  return GBF_SAVE_BYTES + rtc_tail;
}
