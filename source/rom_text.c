/* Item / move / ability descriptions out of the user's own ROM -- see rom_text.h. */
#include "rom_text.h"

#include <string.h>

/* GFRomHeader (ROM+0x100). Offsets recomputed field by field from
 * pokeemerald src/rom_header_gf.c:18-94 and confirmed against all three carts that
 * carry the block; rom_mon.c reads +0x38..+0x40 of the same struct. */
#define GFH_OFF           0x100
#define GFH_VERSION       0x00   /* u32, 1..8                                  */
#define GFH_LANGUAGE      0x04   /* u32, 1..8                                  */
#define GFH_GAMENAME      0x08   /* char[32], always starts "pokemon "         */
#define GFH_ABILITY_DESC  0xC4   /* const u8* const* gAbilityDescriptionPointers */
#define GFH_ITEMS         0xC8   /* const struct Item*                         */
#define GFH_MOVE_DESC     0xFC   /* const u8* -- NULL in every retail build     */
#define GFH_BYTES         0x100

/* struct Item: u8 name[14]; u16 itemId; u16 price; u8 holdEffect; u8 holdEffectParam;
 * const u8 *description; ... (item.h in pokeemerald AND pokefirered, ITEM_NAME_LENGTH
 * 14 in both) -- 44 bytes a row with the description pointer at +0x14. Ruby/Sapphire
 * use the identical layout (verified: gItems[k].itemId == k for the first 52 rows of
 * the pinned table, and every description pointer lands on decodable text). */
#define ITEM_STRIDE       44
#define ITEM_DESC_OFF     0x14

/* Entry counts: ITEMS_COUNT is 349 in pokeruby, 375 in pokefirered, 377 in
 * pokeemerald; ABILITIES_COUNT 78 and MOVES_COUNT 355 everywhere.
 * gMoveDescriptionPointers has MOVES_COUNT-1 = 354 rows because MOVE_NONE is skipped. */
#define N_ABILITIES       78
#define N_MOVE_IDS        355                       /* ids 0..354; 0 has no text     */
#define N_MOVE_ROWS       (N_MOVE_IDS - 1)          /* 354 pointers in the table     */

/* Longest retail description is 108 raw bytes; 192 gives a hacked/odd build room to
 * still terminate, and anything that does not terminate inside the window fails. */
#define RAW_MAX           192

/* Per-revision pinned addresses.
 *
 * moveDescriptions is pinned for EVERY game because the header field is NULL in all
 * retail builds (verified on all three carts that have the header). items and
 * ability descriptions are pinned only for Ruby/Sapphire, which have no header at all.
 *
 * Each row here was located by an independent shape-scan of Guy's own dump -- for the
 * pointer tables, the unique 4-aligned run of exactly 354 (or 78) consecutive pointers
 * whose targets all decode as charmap text; for gItems, the unique offset where
 * gItems[k].itemId == k and every description pointer decodes. On Emerald/FireRed/
 * LeafGreen the scan reproduced the header's own items/abilityDescriptions addresses
 * exactly, which is what makes the same method trustworthy for the pinned rows.
 *
 * MISSING ON PURPOSE: BPRE rev 0, BPGE rev 1, AXVE rev 0/1, AXPE rev 0/2. There is no
 * dump of those here, and rom_map.c's precedent is that an address nobody verified is
 * worse than no feature -- so they fail closed (E/FRLG still get item + ability text
 * from their header; R/S get nothing). Adding one is mechanical: run the same scan
 * against the dump and paste the row. */
typedef struct {
  const char* code;          /* 4-char game code at 0xAC */
  uint8_t     version;       /* revision byte at 0xBC    */
  uint32_t    move_desc;     /* gMoveDescriptionPointers                     */
  uint32_t    items;         /* gItems (R/S only; 0 = take it from the header) */
  uint32_t    ability_desc;  /* gAbilityDescriptionPointers (R/S only)       */
} RomTextPins;

static const RomTextPins k_pins[] = {
  { "BPEE", 0, 0x0861C524, 0,          0          },
  { "BPRE", 1, 0x08488748, 0,          0          },
  { "BPGE", 0, 0x08487FC4, 0,          0          },
  { "AXVE", 2, 0x083C09F4, 0x083C5580, 0x081FA128 },
  { "AXPE", 1, 0x083C0A50, 0x083C55DC, 0x081FA0B8 },
};
#define K_NPINS ((int)(sizeof k_pins / sizeof k_pins[0]))

static uint32_t rd32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- charset ---------------------------------------------------------------
 * The international Gen-3 charmap (pokeemerald charmap.txt). Only the bytes that
 * retail description text actually contains need a glyph; everything else FAILS the
 * decode rather than printing a guess, because "a byte we do not recognise" is
 * exactly what a wrong table address or a corrupt read looks like.
 *
 * MEASURED, so this is not guesswork: across all five carts and every one of the
 * 3,700 item/move/ability descriptions the only non-alphanumeric bytes that occur are
 *   0x00 space  0xFE line break  0x1B e-acute  0x2D '&'  0x55..0x59 the POKEBLOCK
 *   ligature  0x5B '%'  0x5C '('  0x5D ')'  0xF0 ':'  0xAC '?'  0xAD '.'  0xAE '-'
 *   0xB1 0xB2 curly double quotes  0xB4 apostrophe  0xB8 ','  0xBA '/'
 * The rest of the table below is there so a slightly different build still decodes.
 *
 * NOTE 0xB8 is the comma and 0xBA is the slash -- gen3_save.c's gen3_decode_char maps
 * 0xBA to ',' instead, which is wrong per charmap.txt but only ever sees NAME bytes.
 * This table is the description-text authority and deliberately does not share it. */

/* Glyph string for one raw byte, or NULL when the byte has none (-> fail the decode).
 * Single-character results land in the caller's `tmp` so nothing here needs a mutable
 * static -- this file must stay usable from the arena-borrowing screens. */
static const char* charmap(uint8_t c, char tmp[2]) {
  static const char* const k_alnum_punct[] = {
    /* 0xA0 */ "re",                                   /* SUPER_RE               */
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",  /* 0xA1..0xAA             */
    "!", "?", ".", "-",                                /* 0xAB..0xAE             */
    ".",                                               /* 0xAF middle dot        */
    "...",                                             /* 0xB0 ellipsis          */
    "\"", "\"", "'", "'",                              /* 0xB1..0xB4 quotes      */
    "M", "F",                                          /* 0xB5 male, 0xB6 female */
    "$",                                               /* 0xB7 yen/money         */
    ",",                                               /* 0xB8                   */
    "x",                                               /* 0xB9 multiplication    */
    "/",                                               /* 0xBA                   */
  };
  tmp[1] = 0;
  if (c >= 0xA0 && c <= 0xBA) return k_alnum_punct[c - 0xA0];
  if (c >= 0xBB && c <= 0xD4) { tmp[0] = (char)('A' + (c - 0xBB)); return tmp; }
  if (c >= 0xD5 && c <= 0xEE) { tmp[0] = (char)('a' + (c - 0xD5)); return tmp; }
  switch (c) {
    case 0x00: return " ";
    /* Accented capitals/lowercase, folded to their base letter. US carts never use
     * these (rom_open only accepts the US game codes), but folding costs nothing and
     * keeps a PAL dump readable if that gate is ever widened. */
    case 0x01: case 0x02: case 0x03: return "A";
    case 0x04: return "C";
    case 0x05: case 0x06: case 0x07: case 0x08: return "E";
    case 0x09: case 0x0B: case 0x0C: return "I";
    case 0x0D: case 0x0E: case 0x0F: case 0x10: return "O";
    case 0x11: case 0x12: case 0x13: return "U";
    case 0x14: return "N";
    case 0x15: return "ss";
    case 0x16: case 0x17: return "a";
    case 0x19: return "c";
    case 0x1A: return "e";
    case 0x1B: return "\xC3\xA9";   /* e-acute: the UTF-8 pair ui.c:248 maps to glyph 127 */
    case 0x1C: case 0x1D: return "e";
    case 0x1E: case 0x20: case 0x21: return "i";
    case 0x22: case 0x23: case 0x24: case 0x25: return "o";
    case 0x26: case 0x27: case 0x28: return "u";
    case 0x29: return "n";
    case 0x2A: return "o";
    case 0x2B: return "a";
    case 0x2C: return "er";         /* SUPER_ER */
    case 0x2D: return "&";
    case 0x2E: return "+";
    case 0x34: return "Lv";
    case 0x35: return "=";
    case 0x36: return ";";
    case 0x51: return "?";          /* inverted question mark */
    case 0x52: return "!";          /* inverted exclamation   */
    /* PK / MN ligature tiles; 0x53 0x54 together spell "PKMN". */
    case 0x53: return "PK";
    case 0x54: return "MN";
    /* The POKEBLOCK ligature is five consecutive tiles (charmap.txt: 55 56 57 58 59).
     * Emit the whole word on the first tile and nothing on the other four -- which is
     * strictly better than the embedded table, where the same rows currently read
     * "{POKEBLOCK} ingredient..." because gen_data.py never expanded the macro. */
    case 0x55: return "POK\xC3\xA9" "BLOCK";
    case 0x56: case 0x57: case 0x58: case 0x59: return "";
    case 0x5A: return "I";
    case 0x5B: return "%";
    case 0x5C: return "(";
    case 0x5D: return ")";
    case 0x68: return "a";
    case 0x6F: return "i";
    case 0x84: return "e";          /* SUPER_E */
    case 0x85: return "<";
    case 0x86: return ">";
    case 0xEF: return ">";          /* the black triangle used as a bullet */
    case 0xF0: return ":";
    case 0xF1: return "A"; case 0xF2: return "O"; case 0xF3: return "U";
    case 0xF4: return "a"; case 0xF5: return "o"; case 0xF6: return "u";
    default:   return 0;            /* fail closed */
  }
}

/* Bytes consumed by an extended control code, INCLUDING the code byte itself
 * (pokeemerald src/string_util.c:657-691 GetExtCtrlCodeLength). 0 = unknown, which we
 * treat as unrecoverable because we could not tell where the code ends. */
static uint8_t ext_ctrl_len(uint8_t code) {
  static const uint8_t k_len[0x19] = {
    1,       /* 0x00 (the table's own [0] entry)      */
    2, 2, 2, /* COLOR, HIGHLIGHT, SHADOW              */
    4,       /* COLOR_HIGHLIGHT_SHADOW                */
    2, 2,    /* PALETTE, FONT                         */
    1,       /* RESET_FONT                            */
    2,       /* PAUSE                                 */
    1, 1,    /* PAUSE_UNTIL_PRESS, WAIT_SE            */
    3,       /* PLAY_BGM                              */
    2, 2, 2, /* ESCAPE, SHIFT_RIGHT, SHIFT_DOWN       */
    1,       /* FILL_WINDOW                           */
    3,       /* PLAY_SE                               */
    2, 2, 2, /* CLEAR, SKIP, CLEAR_TO                 */
    2,       /* MIN_LETTER_SPACING                    */
    1, 1,    /* JPN, ENG                              */
    1, 1,    /* PAUSE_MUSIC, RESUME_MUSIC             */
  };
  return (code < (uint8_t)sizeof k_len) ? k_len[code] : 0;
}

/* Decode `n` raw bytes into dst. Returns 1 only when a 0xFF terminator was reached and
 * everything before it fitted; 0 leaves dst empty. Leading/trailing spaces are trimmed
 * and 0xFE becomes ONE space -- byte-for-byte the normalisation tools/gen_data.py
 * applies when it bakes the embedded strings, so a ROM-read description and the
 * embedded one are the same string. */
static int decode_desc(const uint8_t* raw, uint32_t n, char* dst, uint32_t cap) {
  uint32_t w = 0;
  int done = 0;
  char tmp[2];
  for (uint32_t i = 0; i < n && !done; i++) {
    uint8_t c = raw[i];
    const char* g;
    switch (c) {
      case 0xFF: done = 1; continue;                    /* EOS                       */
      case 0xFE:                                        /* CHAR_NEWLINE              */
      case 0xFA:                                        /* CHAR_PROMPT_SCROLL (\p)   */
      case 0xFB: g = " "; break;                        /* CHAR_PROMPT_CLEAR  (\l)   */
      case 0xFC: {                                      /* EXT_CTRL_CODE_BEGIN       */
        if (i + 1 >= n) return 0;
        uint8_t len = ext_ctrl_len(raw[i + 1]);
        if (!len || i + len >= n) return 0;             /* cannot resync -> give up  */
        i += len;                                       /* the for-loop eats the 0xFC */
        continue;
      }
      case 0xFD:                                        /* PLACEHOLDER_BEGIN + 1 arg */
      case 0xF8:                                        /* CHAR_KEYPAD_ICON  + 1 arg */
      case 0xF9:                                        /* CHAR_EXTRA_SYMBOL + 1 arg */
        if (i + 1 >= n) return 0;
        i++;
        continue;
      case 0xF7: continue;                              /* CHAR_DYNAMIC, no arg      */
      default:
        g = charmap(c, tmp);
        if (!g) return 0;                               /* not description text      */
        break;
    }
    if (w == 0 && g[0] == ' ' && g[1] == 0) continue;   /* swallow leading spaces    */
    for (const char* p = g; *p; p++) {
      if (w + 1 >= cap) return 0;                       /* never truncate            */
      dst[w++] = *p;
    }
  }
  if (!done) return 0;                                  /* no terminator in range    */
  while (w && dst[w - 1] == ' ') w--;                   /* trim the trailing spaces  */
  dst[w] = 0;
  /* An EMPTY result is a success, not a failure: Ruby/Sapphire really do store an
   * empty description for item 0 and for the unused slots past their table. Callers
   * distinguish "" (the ROM has nothing to say) from a 0 return (we could not read
   * it), and only the second one is a reason to fall back. */
  return 1;
}

/* Read a NUL-less Gen-3 string starting at ROM address `addr`. Reads one bounded
 * window (the games' longest description is 108 bytes) and lets decode_desc find the
 * terminator; clamps at the end of the image so a table pointing at the last bytes of
 * the ROM shortens the read instead of failing it. */
static int fetch(const RomText* rt, uint32_t addr, char* dst, uint32_t cap) {
  if (cap) dst[0] = 0;
  if (!rom_ptr_ok(rt->rc, addr) || !cap) return 0;
  uint32_t off = addr - ROM_BASE;
  uint32_t len = rt->rc->size - off;
  if (len > RAW_MAX) len = RAW_MAX;
  uint8_t raw[RAW_MAX];
  if (!rom_read_at(rt->rc, addr, raw, len)) return 0;
  if (!decode_desc(raw, len, dst, cap)) { dst[0] = 0; return 0; }
  return 1;
}

/* ROM address of entry `id` in one of the pointer tables, or the item struct's
 * description pointer. Returns 0 on any out-of-range id or unreadable pointer. */
static uint32_t entry_addr(const RomText* rt, RomTextKind kind, uint16_t id) {
  uint32_t base = rt->table[kind];
  if (!base) return 0;
  uint8_t p[4];
  if (kind == ROM_TEXT_ITEM) {
    if (id >= rt->count[kind]) return 0;
    if (!rom_read_at(rt->rc, base + (uint32_t)id * ITEM_STRIDE + ITEM_DESC_OFF, p, 4)) return 0;
  } else if (kind == ROM_TEXT_MOVE) {
    /* MOVE_NONE has no row: the game itself indexes gMoveDescriptionPointers[move - 1]
     * (pokeemerald src/pokemon_summary_screen.c:3670). */
    if (id == 0 || id >= rt->count[kind]) return 0;
    if (!rom_read_at(rt->rc, base + ((uint32_t)id - 1) * 4, p, 4)) return 0;
  } else {
    if (id >= rt->count[kind]) return 0;
    if (!rom_read_at(rt->rc, base + (uint32_t)id * 4, p, 4)) return 0;
  }
  return rd32le(p);
}

/* Does the whole table fit inside the image? */
static int table_fits(const RomCtx* rc, uint32_t addr, uint32_t bytes) {
  if (addr < ROM_BASE) return 0;
  uint32_t off = addr - ROM_BASE;
  return off < rc->size && bytes <= rc->size - off;
}

/* Accept a kind only if a probe entry really decodes. This is the fail-closed gate
 * that keeps a pinned address from a ROM hack -- or a silently-garbled SD read -- from
 * printing nonsense: the charset is narrow enough that random bytes lose immediately. */
static void validate(RomText* rt, RomTextKind kind, uint16_t probe_id, uint32_t bytes) {
  char buf[ROM_TEXT_MAX];
  if (!table_fits(rt->rc, rt->table[kind], bytes)) { rt->table[kind] = 0; rt->count[kind] = 0; return; }
  uint32_t a = entry_addr(rt, kind, probe_id);
  /* The probe must produce REAL text: a table of pointers into a run of 0xFF would
   * "decode" to the empty string, so an empty probe is treated as a failure even
   * though rom_text_get is allowed to return one later. */
  if (!a || !fetch(rt, a, buf, sizeof buf) || buf[0] == 0) { rt->table[kind] = 0; rt->count[kind] = 0; }
}

int rom_text_open(RomText* rt, const RomCtx* rc) {
  if (!rt) return 0;
  memset(rt, 0, sizeof *rt);
  rt->group = ROM_TEXT_GROUP_NONE;
  rt->rc = rc;
  if (!rc || !rc->read) return 0;

  switch (rc->kind) {
    case ROM_RUBY: case ROM_SAPPHIRE:  rt->group = ROM_TEXT_GROUP_RS;   rt->count[ROM_TEXT_ITEM] = 349; break;
    case ROM_EMERALD:                  rt->group = ROM_TEXT_GROUP_E;    rt->count[ROM_TEXT_ITEM] = 377; break;
    case ROM_FIRERED: case ROM_LEAFGREEN: rt->group = ROM_TEXT_GROUP_FRLG; rt->count[ROM_TEXT_ITEM] = 375; break;
    default: return 0;
  }
  rt->count[ROM_TEXT_ABILITY] = N_ABILITIES;
  rt->count[ROM_TEXT_MOVE]    = N_MOVE_IDS;

  const RomTextPins* pin = 0;
  for (int i = 0; i < K_NPINS; i++)
    if (memcmp(k_pins[i].code, rc->code, 4) == 0 && k_pins[i].version == rc->version) {
      pin = &k_pins[i]; break;
    }

  /* Emerald/FRLG: items + ability descriptions come out of the GF header, so they work
   * even on a revision this file has no pinned row for. Ruby/Sapphire have no header
   * (ARM code sits at 0x100) and depend entirely on the pin. */
  uint8_t h[GFH_BYTES];
  int have_hdr = 0;
  if (rc->read(rc->ctx, GFH_OFF, h, sizeof h)) {
    uint32_t ver = rd32le(h + GFH_VERSION), lang = rd32le(h + GFH_LANGUAGE);
    have_hdr = (ver >= 1 && ver <= 8 && lang >= 1 && lang <= 8 &&
                memcmp(h + GFH_GAMENAME, "pokemon ", 8) == 0);
  }
  if (have_hdr) {
    rt->table[ROM_TEXT_ITEM]    = rd32le(h + GFH_ITEMS);
    rt->table[ROM_TEXT_ABILITY] = rd32le(h + GFH_ABILITY_DESC);
    /* Retail always stores NULL here (rom_header_gf.c:173); read it anyway so a build
     * that DOES populate it -- pokeemerald-expansion, a translation patch -- needs no
     * new pin. The validate() probe below decides whether to believe it. */
    rt->table[ROM_TEXT_MOVE]    = rd32le(h + GFH_MOVE_DESC);
  }
  if (pin) {
    if (pin->items && !rt->table[ROM_TEXT_ITEM])          rt->table[ROM_TEXT_ITEM]    = pin->items;
    if (pin->ability_desc && !rt->table[ROM_TEXT_ABILITY]) rt->table[ROM_TEXT_ABILITY] = pin->ability_desc;
    if (!rt->table[ROM_TEXT_MOVE])                        rt->table[ROM_TEXT_MOVE]    = pin->move_desc;
  }

  /* Probe ids: item 1 (Master Ball -- id 0 and the unused slots are the "?????"
   * placeholder, real text but useless as a signal), ability 1 and move 1, both of
   * which every retail build fills in. */
  validate(rt, ROM_TEXT_ITEM,    1, (uint32_t)rt->count[ROM_TEXT_ITEM] * ITEM_STRIDE);
  validate(rt, ROM_TEXT_ABILITY, 1, (uint32_t)N_ABILITIES * 4);
  validate(rt, ROM_TEXT_MOVE,    1, (uint32_t)N_MOVE_ROWS * 4);

  rt->ok = (rt->table[ROM_TEXT_ITEM] || rt->table[ROM_TEXT_MOVE] || rt->table[ROM_TEXT_ABILITY]) ? 1 : 0;
  if (!rt->ok) rt->group = ROM_TEXT_GROUP_NONE;
  return rt->ok;
}

int rom_text_have(const RomText* rt, RomTextKind kind) {
  if (!rt || !rt->ok || (unsigned)kind >= ROM_TEXT_KINDS) return 0;
  return rt->table[kind] != 0;
}

int rom_text_count(const RomText* rt, RomTextKind kind) {
  return rom_text_have(rt, kind) ? (int)rt->count[kind] : 0;
}

int rom_text_group(const RomText* rt) {
  return (rt && rt->ok) ? (int)rt->group : ROM_TEXT_GROUP_NONE;
}

int rom_text_get(const RomText* rt, RomTextKind kind, uint16_t id, char* dst, uint32_t cap) {
  if (dst && cap) dst[0] = 0;
  if (!dst || !cap || !rom_text_have(rt, kind)) return 0;
  uint32_t a = entry_addr(rt, kind, id);
  if (!a) return 0;
  return fetch(rt, a, dst, cap);
}
