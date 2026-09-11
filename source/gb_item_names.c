/*
 * Real Gen-1/Gen-2 item name tables -- see gb_item_names.h for the ground
 * rules. Fact sources (reference-only, never copied verbatim -- names typed
 * into THIS file's own array literal format, ids cross-checked against the
 * constants file's own numbering):
 *   Gen 1: assets/upstream/pokered/constants/item_constants.asm (ids 1-97,
 *     TM/HM synthesis 0xC4-0xFA) + assets/upstream/pokered/data/items/
 *     names.asm (the display strings; "?????" entries there are real,
 *     documented-unused ids -- id 0x07 SURFBOARD, 0x2C ITEM_2C -- kept as
 *     NULL holes here, not guessed names).
 *   Gen 2: assets/upstream/pokecrystal/constants/item_constants.asm (ids
 *     1-190, cross-checked against pokegold's -- identical id space, G/S
 *     just lack 4 Crystal-only items: CLEAR_BELL 0x46, GS_BALL 0x73,
 *     BLUE_CARD 0x74, EGG_TICKET 0x81 -- shipped anyway since the byte
 *     never occurs in a real G/S bag) + assets/upstream/pokecrystal/data/
 *     items/names.asm (display strings; "TERU-SAMA"/"?" entries there are
 *     real unused-id placeholders, kept as NULL holes, NOT invented names --
 *     this includes id 0x38, nominally POKE_FLUTE in the constants file but
 *     never actually distributed as an item in Gen 2, hence unused there).
 *     The "#" glyph in pokecrystal's own names.asm is that repo's own
 *     charmap macro for the literal text "POKé" (assets/upstream/
 *     pokecrystal/constants/charmap.asm:26: `charmap "#", $54 ; "POKé"`) --
 *     substituted back to "POKé" here, not shipped as a literal '#'.
 *
 * IMPORTANT -- the Gen-2 TM/HM id ranges below differ from an EARLIER DRAFT
 * of this slice's own brief, which guessed "TM01..TM50 = 0xBF..0xF1, one
 * hole at 0xC3". The constants file (item_constants.asm:220-272) shows the
 * real shape: TM01-TM04 = 0xBF-0xC2, a hole at 0xC3 (ITEM_C3, unused),
 * TM05-TM28 = 0xC4-0xDB, a SECOND hole at 0xDC (ITEM_DC, unused) the draft
 * missed, TM29-TM50 = 0xDD-0xF2 (not 0xF1), then HM01-HM07 = 0xF3-0xF9
 * (this range DID match the draft). Per the brief's own STOP-LICENCE
 * ("report the real one and use it -- do not guess"), this file uses the
 * constants-file-derived mapping, not the draft's.
 *
 * Pure C, no tonc/GBA headers (hard rule 5) -- dual-compiles into
 * tests/host_gbitemnames_test.c.
 */
#include "gb_item_names.h"
#include <string.h>

#define GEN1_ITEM_MAX 0x61
#define GEN2_ITEM_MAX 0xBE

static const char* const kGen1ItemName[GEN1_ITEM_MAX + 1] = {
  NULL, /* 0x00 NO_ITEM */
  "MASTER BALL", /* 0x01 */
  "ULTRA BALL", /* 0x02 */
  "GREAT BALL", /* 0x03 */
  "POKé BALL", /* 0x04 */
  "TOWN MAP", /* 0x05 */
  "BICYCLE", /* 0x06 */
  NULL, /* 0x07 unused */
  "SAFARI BALL", /* 0x08 */
  "POKéDEX", /* 0x09 */
  "MOON STONE", /* 0x0A */
  "ANTIDOTE", /* 0x0B */
  "BURN HEAL", /* 0x0C */
  "ICE HEAL", /* 0x0D */
  "AWAKENING", /* 0x0E */
  "PARLYZ HEAL", /* 0x0F */
  "FULL RESTORE", /* 0x10 */
  "MAX POTION", /* 0x11 */
  "HYPER POTION", /* 0x12 */
  "SUPER POTION", /* 0x13 */
  "POTION", /* 0x14 */
  "BOULDERBADGE", /* 0x15 */
  "CASCADEBADGE", /* 0x16 */
  "THUNDERBADGE", /* 0x17 */
  "RAINBOWBADGE", /* 0x18 */
  "SOULBADGE", /* 0x19 */
  "MARSHBADGE", /* 0x1A */
  "VOLCANOBADGE", /* 0x1B */
  "EARTHBADGE", /* 0x1C */
  "ESCAPE ROPE", /* 0x1D */
  "REPEL", /* 0x1E */
  "OLD AMBER", /* 0x1F */
  "FIRE STONE", /* 0x20 */
  "THUNDERSTONE", /* 0x21 */
  "WATER STONE", /* 0x22 */
  "HP UP", /* 0x23 */
  "PROTEIN", /* 0x24 */
  "IRON", /* 0x25 */
  "CARBOS", /* 0x26 */
  "CALCIUM", /* 0x27 */
  "RARE CANDY", /* 0x28 */
  "DOME FOSSIL", /* 0x29 */
  "HELIX FOSSIL", /* 0x2A */
  "SECRET KEY", /* 0x2B */
  NULL, /* 0x2C unused */
  "BIKE VOUCHER", /* 0x2D */
  "X ACCURACY", /* 0x2E */
  "LEAF STONE", /* 0x2F */
  "CARD KEY", /* 0x30 */
  "NUGGET", /* 0x31 */
  "PP UP", /* 0x32 */
  "POKé DOLL", /* 0x33 */
  "FULL HEAL", /* 0x34 */
  "REVIVE", /* 0x35 */
  "MAX REVIVE", /* 0x36 */
  "GUARD SPEC.", /* 0x37 */
  "SUPER REPEL", /* 0x38 */
  "MAX REPEL", /* 0x39 */
  "DIRE HIT", /* 0x3A */
  "COIN", /* 0x3B */
  "FRESH WATER", /* 0x3C */
  "SODA POP", /* 0x3D */
  "LEMONADE", /* 0x3E */
  "S.S.TICKET", /* 0x3F */
  "GOLD TEETH", /* 0x40 */
  "X ATTACK", /* 0x41 */
  "X DEFEND", /* 0x42 */
  "X SPEED", /* 0x43 */
  "X SPECIAL", /* 0x44 */
  "COIN CASE", /* 0x45 */
  "OAK's PARCEL", /* 0x46 */
  "ITEMFINDER", /* 0x47 */
  "SILPH SCOPE", /* 0x48 */
  "POKé FLUTE", /* 0x49 */
  "LIFT KEY", /* 0x4A */
  "EXP.ALL", /* 0x4B */
  "OLD ROD", /* 0x4C */
  "GOOD ROD", /* 0x4D */
  "SUPER ROD", /* 0x4E */
  "PP UP", /* 0x4F */
  "ETHER", /* 0x50 */
  "MAX ETHER", /* 0x51 */
  "ELIXER", /* 0x52 */
  "MAX ELIXER", /* 0x53 */
  "B2F", /* 0x54 */
  "B1F", /* 0x55 */
  "1F", /* 0x56 */
  "2F", /* 0x57 */
  "3F", /* 0x58 */
  "4F", /* 0x59 */
  "5F", /* 0x5A */
  "6F", /* 0x5B */
  "7F", /* 0x5C */
  "8F", /* 0x5D */
  "9F", /* 0x5E */
  "10F", /* 0x5F */
  "11F", /* 0x60 */
  "B4F", /* 0x61 */
};

static const char* const kGen2ItemName[GEN2_ITEM_MAX + 1] = {
  NULL, /* 0x00 NO_ITEM */
  "MASTER BALL", /* 0x01 */
  "ULTRA BALL", /* 0x02 */
  "BRIGHTPOWDER", /* 0x03 */
  "GREAT BALL", /* 0x04 */
  "POKé BALL", /* 0x05 */
  NULL, /* 0x06 unused */
  "BICYCLE", /* 0x07 */
  "MOON STONE", /* 0x08 */
  "ANTIDOTE", /* 0x09 */
  "BURN HEAL", /* 0x0A */
  "ICE HEAL", /* 0x0B */
  "AWAKENING", /* 0x0C */
  "PARLYZ HEAL", /* 0x0D */
  "FULL RESTORE", /* 0x0E */
  "MAX POTION", /* 0x0F */
  "HYPER POTION", /* 0x10 */
  "SUPER POTION", /* 0x11 */
  "POTION", /* 0x12 */
  "ESCAPE ROPE", /* 0x13 */
  "REPEL", /* 0x14 */
  "MAX ELIXER", /* 0x15 */
  "FIRE STONE", /* 0x16 */
  "THUNDERSTONE", /* 0x17 */
  "WATER STONE", /* 0x18 */
  NULL, /* 0x19 unused */
  "HP UP", /* 0x1A */
  "PROTEIN", /* 0x1B */
  "IRON", /* 0x1C */
  "CARBOS", /* 0x1D */
  "LUCKY PUNCH", /* 0x1E */
  "CALCIUM", /* 0x1F */
  "RARE CANDY", /* 0x20 */
  "X ACCURACY", /* 0x21 */
  "LEAF STONE", /* 0x22 */
  "METAL POWDER", /* 0x23 */
  "NUGGET", /* 0x24 */
  "POKé DOLL", /* 0x25 */
  "FULL HEAL", /* 0x26 */
  "REVIVE", /* 0x27 */
  "MAX REVIVE", /* 0x28 */
  "GUARD SPEC.", /* 0x29 */
  "SUPER REPEL", /* 0x2A */
  "MAX REPEL", /* 0x2B */
  "DIRE HIT", /* 0x2C */
  NULL, /* 0x2D unused */
  "FRESH WATER", /* 0x2E */
  "SODA POP", /* 0x2F */
  "LEMONADE", /* 0x30 */
  "X ATTACK", /* 0x31 */
  NULL, /* 0x32 unused */
  "X DEFEND", /* 0x33 */
  "X SPEED", /* 0x34 */
  "X SPECIAL", /* 0x35 */
  "COIN CASE", /* 0x36 */
  "ITEMFINDER", /* 0x37 */
  NULL, /* 0x38 unused */
  "EXP.SHARE", /* 0x39 */
  "OLD ROD", /* 0x3A */
  "GOOD ROD", /* 0x3B */
  "SILVER LEAF", /* 0x3C */
  "SUPER ROD", /* 0x3D */
  "PP UP", /* 0x3E */
  "ETHER", /* 0x3F */
  "MAX ETHER", /* 0x40 */
  "ELIXER", /* 0x41 */
  "RED SCALE", /* 0x42 */
  "SECRETPOTION", /* 0x43 */
  "S.S.TICKET", /* 0x44 */
  "MYSTERY EGG", /* 0x45 */
  "CLEAR BELL", /* 0x46 */
  "SILVER WING", /* 0x47 */
  "MOOMOO MILK", /* 0x48 */
  "QUICK CLAW", /* 0x49 */
  "PSNCUREBERRY", /* 0x4A */
  "GOLD LEAF", /* 0x4B */
  "SOFT SAND", /* 0x4C */
  "SHARP BEAK", /* 0x4D */
  "PRZCUREBERRY", /* 0x4E */
  "BURNT BERRY", /* 0x4F */
  "ICE BERRY", /* 0x50 */
  "POISON BARB", /* 0x51 */
  "KING'S ROCK", /* 0x52 */
  "BITTER BERRY", /* 0x53 */
  "MINT BERRY", /* 0x54 */
  "RED APRICORN", /* 0x55 */
  "TINYMUSHROOM", /* 0x56 */
  "BIG MUSHROOM", /* 0x57 */
  "SILVERPOWDER", /* 0x58 */
  "BLU APRICORN", /* 0x59 */
  NULL, /* 0x5A unused */
  "AMULET COIN", /* 0x5B */
  "YLW APRICORN", /* 0x5C */
  "GRN APRICORN", /* 0x5D */
  "CLEANSE TAG", /* 0x5E */
  "MYSTIC WATER", /* 0x5F */
  "TWISTEDSPOON", /* 0x60 */
  "WHT APRICORN", /* 0x61 */
  "BLACKBELT", /* 0x62 */
  "BLK APRICORN", /* 0x63 */
  NULL, /* 0x64 unused */
  "PNK APRICORN", /* 0x65 */
  "BLACKGLASSES", /* 0x66 */
  "SLOWPOKETAIL", /* 0x67 */
  "PINK BOW", /* 0x68 */
  "STICK", /* 0x69 */
  "SMOKE BALL", /* 0x6A */
  "NEVERMELTICE", /* 0x6B */
  "MAGNET", /* 0x6C */
  "MIRACLEBERRY", /* 0x6D */
  "PEARL", /* 0x6E */
  "BIG PEARL", /* 0x6F */
  "EVERSTONE", /* 0x70 */
  "SPELL TAG", /* 0x71 */
  "RAGECANDYBAR", /* 0x72 */
  "GS BALL", /* 0x73 */
  "BLUE CARD", /* 0x74 */
  "MIRACLE SEED", /* 0x75 */
  "THICK CLUB", /* 0x76 */
  "FOCUS BAND", /* 0x77 */
  NULL, /* 0x78 unused */
  "ENERGYPOWDER", /* 0x79 */
  "ENERGY ROOT", /* 0x7A */
  "HEAL POWDER", /* 0x7B */
  "REVIVAL HERB", /* 0x7C */
  "HARD STONE", /* 0x7D */
  "LUCKY EGG", /* 0x7E */
  "CARD KEY", /* 0x7F */
  "MACHINE PART", /* 0x80 */
  "EGG TICKET", /* 0x81 */
  "LOST ITEM", /* 0x82 */
  "STARDUST", /* 0x83 */
  "STAR PIECE", /* 0x84 */
  "BASEMENT KEY", /* 0x85 */
  "PASS", /* 0x86 */
  NULL, /* 0x87 unused */
  NULL, /* 0x88 unused */
  NULL, /* 0x89 unused */
  "CHARCOAL", /* 0x8A */
  "BERRY JUICE", /* 0x8B */
  "SCOPE LENS", /* 0x8C */
  NULL, /* 0x8D unused */
  NULL, /* 0x8E unused */
  "METAL COAT", /* 0x8F */
  "DRAGON FANG", /* 0x90 */
  NULL, /* 0x91 unused */
  "LEFTOVERS", /* 0x92 */
  NULL, /* 0x93 unused */
  NULL, /* 0x94 unused */
  NULL, /* 0x95 unused */
  "MYSTERYBERRY", /* 0x96 */
  "DRAGON SCALE", /* 0x97 */
  "BERSERK GENE", /* 0x98 */
  NULL, /* 0x99 unused */
  NULL, /* 0x9A unused */
  NULL, /* 0x9B unused */
  "SACRED ASH", /* 0x9C */
  "HEAVY BALL", /* 0x9D */
  "FLOWER MAIL", /* 0x9E */
  "LEVEL BALL", /* 0x9F */
  "LURE BALL", /* 0xA0 */
  "FAST BALL", /* 0xA1 */
  NULL, /* 0xA2 unused */
  "LIGHT BALL", /* 0xA3 */
  "FRIEND BALL", /* 0xA4 */
  "MOON BALL", /* 0xA5 */
  "LOVE BALL", /* 0xA6 */
  "NORMAL BOX", /* 0xA7 */
  "GORGEOUS BOX", /* 0xA8 */
  "SUN STONE", /* 0xA9 */
  "POLKADOT BOW", /* 0xAA */
  NULL, /* 0xAB unused */
  "UP-GRADE", /* 0xAC */
  "BERRY", /* 0xAD */
  "GOLD BERRY", /* 0xAE */
  "SQUIRTBOTTLE", /* 0xAF */
  NULL, /* 0xB0 unused */
  "PARK BALL", /* 0xB1 */
  "RAINBOW WING", /* 0xB2 */
  NULL, /* 0xB3 unused */
  "BRICK PIECE", /* 0xB4 */
  "SURF MAIL", /* 0xB5 */
  "LITEBLUEMAIL", /* 0xB6 */
  "PORTRAITMAIL", /* 0xB7 */
  "LOVELY MAIL", /* 0xB8 */
  "EON MAIL", /* 0xB9 */
  "MORPH MAIL", /* 0xBA */
  "BLUESKY MAIL", /* 0xBB */
  "MUSIC MAIL", /* 0xBC */
  "MIRAGE MAIL", /* 0xBD */
  NULL, /* 0xBE unused */
};

const char* gb1_item_name(uint8_t id) {
  if (id == 0u || id > GEN1_ITEM_MAX) return NULL;
  return kGen1ItemName[id];
}

const char* gb2_item_name(uint8_t id) {
  if (id == 0u || id > GEN2_ITEM_MAX) return NULL;
  return kGen2ItemName[id];
}

/* Two-digit "TM%02u"/"HM%02u" without pulling in stdio -- num is always
 * 1..50 for a TM, 1..7 for an HM (both callers below only ever pass a
 * value their own range check already bounded), so a fixed two-digit
 * field is always enough; `cap` is still checked (hard rule 7: every
 * caller-supplied bound gets honoured, not assumed). */
static bool put_tmhm(char* out, int cap, const char* prefix, unsigned num) {
  if (!out || cap < 5) return false;   /* "TM50\0" is 5 bytes, the tightest caller */
  out[0] = prefix[0];
  out[1] = prefix[1];
  out[2] = (char)('0' + (num / 10u) % 10u);
  out[3] = (char)('0' + num % 10u);
  out[4] = '\0';
  return true;
}

/* Gen 1: HM01-HM05 = 0xC4-0xC8, TM01-TM50 = 0xC9-0xFA -- no holes, matches
 * the brief's own stated ranges exactly (constants file: item_constants.asm
 * lines 140-211). */
static bool gb1_tmhm_label(uint8_t id, char* out, int cap) {
  if (id >= 0xC4u && id <= 0xC8u) return put_tmhm(out, cap, "HM", (unsigned)(id - 0xC4u + 1u));
  if (id >= 0xC9u && id <= 0xFAu) return put_tmhm(out, cap, "TM", (unsigned)(id - 0xC9u + 1u));
  return false;
}

/* Gen 2: see this file's header comment for the full derivation -- two
 * holes (0xC3, 0xDC) inside the TM run, TM01-TM50 = 0xBF-0xF2 overall,
 * HM01-HM07 = 0xF3-0xF9 (item_constants.asm lines 219-293). */
static bool gb2_tmhm_label(uint8_t id, char* out, int cap) {
  if (id >= 0xBFu && id <= 0xC2u) return put_tmhm(out, cap, "TM", (unsigned)(id - 0xBFu + 1u));
  if (id == 0xC3u) return false;                                    /* ITEM_C3, unused */
  if (id >= 0xC4u && id <= 0xDBu) return put_tmhm(out, cap, "TM", (unsigned)(id - 0xC4u + 5u));
  if (id == 0xDCu) return false;                                    /* ITEM_DC, unused */
  if (id >= 0xDDu && id <= 0xF2u) return put_tmhm(out, cap, "TM", (unsigned)(id - 0xDDu + 29u));
  if (id >= 0xF3u && id <= 0xF9u) return put_tmhm(out, cap, "HM", (unsigned)(id - 0xF3u + 1u));
  return false;
}

bool gb_item_label(int gen, uint8_t id, char* out, int cap) {
  if (!out || cap <= 0) return false;
  const char* n = (gen == GBIN_GEN2) ? gb2_item_name(id) : gb1_item_name(id);
  if (n) {
    size_t len = strlen(n);
    if (len >= (size_t)cap) return false;   /* never truncate a real name silently */
    memcpy(out, n, len + 1u);
    return true;
  }
  return (gen == GBIN_GEN2) ? gb2_tmhm_label(id, out, cap) : gb1_tmhm_label(id, out, cap);
}
