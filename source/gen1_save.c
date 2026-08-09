#include <string.h>
#include "gen1_save.h"

/* Generation-I (R/B/Y, Western) save reader. See gen1_save.h for the contract and
 * for why this is read-only. Layout from Bulbapedia "Save data structure
 * (Generation I)" + "Pokemon data structure (Generation I)"; clean-room, no decomp
 * code (docs/kb/licensing.md — pret is reference only). */

/* --- big-endian readers. The GB stores multi-byte fields high byte first, which is
 * the reverse of every gen3_* core in this tree, so they get their own helpers rather
 * than a shared one that would be easy to grab by mistake. --- */
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd24be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* Compile-time proof that the documented blob sizes and the field offsets derived
 * below are the same arithmetic; a typo in either breaks the build instead of
 * silently reading the wrong slot. */
typedef char gen1_box_geometry_check[
  (1 + (GEN1_BOX_CAPACITY + 1) + GEN1_BOX_CAPACITY * GEN1_BOX_REC_BYTES
     + 2 * GEN1_BOX_CAPACITY * GEN1_NAME_BYTES) == (int)GEN1_BOX_BYTES ? 1 : -1];
typedef char gen1_party_geometry_check[
  (1 + (GEN1_PARTY_CAPACITY + 1) + GEN1_PARTY_CAPACITY * GEN1_PARTY_REC_BYTES
     + 2 * GEN1_PARTY_CAPACITY * GEN1_NAME_BYTES) == (int)GEN1_PARTY_BYTES ? 1 : -1];

/* ------------------------------------------------------------------------- */
/* Gen-1 internal species index -> National Dex number.                       */
/* ------------------------------------------------------------------------- */

/* Gen 1 numbers its species by an internal index that has nothing to do with dex
 * order (Rhydon is 0x01, Bulbasaur 0x99, Mew 0x15) — the save stores the internal
 * index, so this map is mandatory, not a nicety. Fact table, index -> dex number,
 * same legal footing as the existing name/stat tables.
 *
 * SOURCE / PROVENANCE — read this before trusting a row:
 * The canonical table is pret/pokered `data/pokemon/dex_order.asm` (label
 * `PokedexOrder`, 190 entries). VERIFIED 2026-08-09: pokered is now checked out at
 * assets/upstream/pokered and this table was diffed against it entry by entry —
 * BYTE-IDENTICAL on all 190, 39 MissingNo zeros in the same places. The note below
 * describes how it was originally transcribed, kept for provenance.
 *
 * (superseded) No pokered checkout existed on this machine, so the
 * values below were transcribed from the same public data as documented on
 * Bulbapedia, "List of Pokemon by index number (Generation I)". Two properties the
 * transcription was checked against, both of which a single wrong row would break:
 *   - the 151 non-zero entries are a BIJECTION onto dex numbers 1..151 (no dupes,
 *     no gaps), and
 *   - exactly 39 entries are 0, which is the documented number of MissingNo. slots.
 * That is strong but it is not a diff against pokered. UNVERIFIED against the
 * primary source — worth a second pair of eyes, or a diff once a checkout exists. */
static const uint8_t k_dex_from_index[190] = {
  112,115, 32, 35, 21,100, 34, 80,  2,103,   /* 0x01 Rhydon .. 0x0A Exeggutor    */
  108,102, 88, 94, 29, 31,104,111,131, 59,   /* 0x0B                             */
  151,130, 90, 72, 92,123,120,  9,127,114,   /* 0x15 Mew ..                      */
    0,  0, 58, 95, 22, 16, 79, 64, 75,113,   /* 0x1F 0x20 = MissingNo            */
   67,122,106,107, 24, 47, 54, 96, 76,  0,   /* 0x29                             */
  126,  0,125, 82,109,  0, 56, 86, 50,128,   /* 0x33                             */
    0,  0,  0, 83, 48,149,  0,  0,  0, 84,   /* 0x3D                             */
   60,124,146,144,145,132, 52, 98,  0,  0,   /* 0x47                             */
    0, 37, 38, 25, 26,  0,  0,147,148,140,   /* 0x51 (0x54 Pikachu)              */
  141,116,117,  0,  0, 27, 28,138,139, 39,   /* 0x5B                             */
   40,133,136,135,134, 66, 41, 23, 46, 61,   /* 0x65                             */
   62, 13, 14, 15,  0, 85, 57, 51, 49, 87,   /* 0x6F                             */
    0,  0, 10, 11, 12, 68,  0, 55, 97, 42,   /* 0x79                             */
  150,143,129,  0,  0, 89,  0, 99, 91,  0,   /* 0x83 Mewtwo ..                   */
  101, 36,110, 53,105,  0, 93, 63, 65, 17,   /* 0x8D                             */
   18,121,  1,  3, 73,  0,118,119,  0,  0,   /* 0x97 (0x99 Bulbasaur)            */
    0,  0, 77, 78, 19, 20, 33, 30, 74,137,   /* 0xA1                             */
  142,  0, 81,  0,  0,  4,  7,  5,  8,  6,   /* 0xAB (0xB0 Charmander)           */
    0,  0,  0,  0, 43, 44, 45, 69, 70, 71,   /* 0xB5 .. 0xBE Victreebel (last)   */
};

uint16_t gen1_dex_from_index(uint8_t internal) {
  if (internal == 0 || internal > 190) return 0;
  return k_dex_from_index[internal - 1];
}

/* ------------------------------------------------------------------------- */
/* Text                                                                       */
/* ------------------------------------------------------------------------- */

int gen1_char_ascii(uint8_t c, char out[3]) {
  /* 0x50 is the string terminator ("@"). 0x00 is a text control code that cannot
   * legally appear inside a fixed-width name field, so treat it as an end too rather
   * than emit a stray glyph for a corrupt byte. */
  if (c == 0x50u || c == 0x00u) return 0;
  if (c == 0x7Fu)                    { out[0] = ' ';                        return 1; }
  if (c >= 0x80u && c <= 0x99u)      { out[0] = (char)('A' + (c - 0x80u));  return 1; }
  if (c >= 0xA0u && c <= 0xB9u)      { out[0] = (char)('a' + (c - 0xA0u));  return 1; }
  if (c >= 0xF6u)                    { out[0] = (char)('0' + (c - 0xF6u));  return 1; }
  switch (c) {
    case 0x9Au: out[0] = '(';  return 1;
    case 0x9Bu: out[0] = ')';  return 1;
    case 0x9Cu: out[0] = ':';  return 1;
    case 0x9Du: out[0] = ';';  return 1;
    case 0x9Eu: out[0] = '[';  return 1;
    case 0x9Fu: out[0] = ']';  return 1;
    case 0xBAu: out[0] = 'e';  return 1;   /* e-acute; ASCII has no accent to keep */
    /* The five lowercase contractions ('d 'l 's 't 'v — "you'd", "he'll", "it's",
     * "don't", "we've") are ONE byte on the GB and two characters in ASCII, so
     * decoding a name can produce more characters than it read. The Gen1Mon name
     * buffers are sized for that, and module C has to re-clamp to Gen 3's limits. */
    case 0xBBu: out[0] = '\''; out[1] = 'd'; return 2;
    case 0xBCu: out[0] = '\''; out[1] = 'l'; return 2;
    case 0xBDu: out[0] = '\''; out[1] = 's'; return 2;
    case 0xBEu: out[0] = '\''; out[1] = 't'; return 2;
    case 0xBFu: out[0] = '\''; out[1] = 'v'; return 2;
    case 0xE0u: out[0] = '\''; return 1;
    case 0xE1u: out[0] = 'P';  out[1] = 'K'; return 2;
    case 0xE2u: out[0] = 'M';  out[1] = 'N'; return 2;
    case 0xE3u: out[0] = '-';  return 1;
    case 0xE4u: out[0] = '\''; out[1] = 'r'; return 2;
    case 0xE5u: out[0] = '\''; out[1] = 'm'; return 2;
    case 0xE6u: out[0] = '?';  return 1;
    case 0xE7u: out[0] = '!';  return 1;
    case 0xE8u: out[0] = '.';  return 1;
    /* Gender signs come out as UTF-8 U+2642 / U+2640, the same spelling
     * gen3_record.c:269's encode_name already round-trips into the Gen-3 charset
     * (0xB5 / 0xB6). Mapping them to 'M'/'F' would invent letters that are not in
     * the name; '?' would throw away a glyph Gen 3 can represent exactly. */
    case 0xEFu: out[0] = (char)0xE2; out[1] = (char)0x99; out[2] = (char)0x82; return 3;
    case 0xF5u: out[0] = (char)0xE2; out[1] = (char)0x99; out[2] = (char)0x80; return 3;
    case 0xF2u: out[0] = '.';  return 1;
    case 0xF3u: out[0] = '/';  return 1;
    case 0xF4u: out[0] = ',';  return 1;
    default:    out[0] = '?';  return 1;   /* no ASCII spelling — say so, don't guess */
  }
}

int gen1_decode_name(char* out, int cap, const uint8_t* src, int nbytes) {
  int n = 0;
  if (cap <= 0) return 0;
  for (int i = 0; i < nbytes; i++) {
    char g[3];
    int w = gen1_char_ascii(src[i], g);
    if (w == 0) break;
    if (n + w >= cap) break;          /* truncate on a glyph boundary, never split UTF-8 */
    for (int k = 0; k < w; k++) out[n++] = g[k];
  }
  out[n] = 0;
  return n;
}

/* ------------------------------------------------------------------------- */
/* Checksum                                                                   */
/* ------------------------------------------------------------------------- */

/* "Initialize the checksum to 0, add every byte in the range, invert the bits."
 * One 8-bit checksum guards the whole main data block. */
void gen1_sum_init(Gen1Sum* s) { s->sum = 0; }
void gen1_sum_feed(Gen1Sum* s, const uint8_t* p, uint32_t n) {
  uint8_t acc = s->sum;
  for (uint32_t i = 0; i < n; i++) acc = (uint8_t)(acc + p[i]);
  s->sum = acc;
}
uint8_t gen1_sum_final(const Gen1Sum* s) { return (uint8_t)(~s->sum); }

/* ------------------------------------------------------------------------- */
/* Box geometry                                                               */
/* ------------------------------------------------------------------------- */

uint32_t gen1_list_bytes(int box)    { return box == GEN1_PARTY_BOX ? GEN1_PARTY_BYTES : GEN1_BOX_BYTES; }
int      gen1_list_capacity(int box) { return box == GEN1_PARTY_BOX ? GEN1_PARTY_CAPACITY : GEN1_BOX_CAPACITY; }

/* WHICH COPY OF THE CURRENT BOX WE READ, AND WHY.
 * The box the player has open lives twice: a live working copy in bank 1 at 0x30C0,
 * and a slot in bank 2/3 that the game only rewrites when the player SWITCHES boxes.
 * Deposit a Pokemon and save without switching and the banked slot still shows the
 * box as it was before the deposit. So the bank-1 copy is authoritative for the
 * current box and the banked one is stale; we always resolve `current_box` to
 * 0x30C0. (Every other box has no live copy, so its bank slot is the only copy.) */
uint32_t gen1_list_offset(const Gen1Save* s, int box) {
  if (box == GEN1_PARTY_BOX) return GEN1_OFF_PARTY;
  if (box < 0 || box >= GEN1_NUM_BOXES) return 0;
  if (s && box == s->current_box) return GEN1_OFF_CURRENT_BOX;
  return (box < 6 ? GEN1_OFF_BANK2 : GEN1_OFF_BANK3) + (uint32_t)(box % 6) * GEN1_BOX_BYTES;
}

int gen1_count(const Gen1Save* s, int box) {
  if (!s) return -1;
  if (box == GEN1_PARTY_BOX) return s->party_count;
  if (box < 0 || box >= GEN1_NUM_BOXES) return -1;
  return s->box_count[box];
}

int gen1_list_count(const uint8_t* list, int box) {
  int cap = gen1_list_capacity(box);
  int n;
  if (!list) return 0;
  n = list[0];
  return n > cap ? cap : n;   /* a corrupt count must never index past the blob */
}

/* ------------------------------------------------------------------------- */
/* Record decode                                                              */
/* ------------------------------------------------------------------------- */

bool gen1_decode(const uint8_t* list, int box, int slot, Gen1Mon* out) {
  memset(out, 0, sizeof *out);
  if (!list || slot < 0) return false;

  const bool party  = (box == GEN1_PARTY_BOX);
  const int  cap    = gen1_list_capacity(box);
  const int  recsz  = party ? GEN1_PARTY_REC_BYTES : GEN1_BOX_REC_BYTES;
  if (slot >= gen1_list_count(list, box)) return false;

  /* count(1) + species list(cap+1, 0xFF-terminated) + records + OT names + nicknames */
  const int rec_off  = 1 + (cap + 1);
  const int ot_off   = rec_off + cap * recsz;
  const int nick_off = ot_off + cap * GEN1_NAME_BYTES;

  const uint8_t* rec = list + rec_off + slot * recsz;
  uint8_t idx = rec[0x00];
  if (idx == 0 || idx > 190) return false;   /* not a Gen-1 species index at all */

  out->species_idx   = idx;
  out->dex           = gen1_dex_from_index(idx);
  out->list_mismatch = (list[1 + slot] != idx);
  out->is_party      = party;

  /* Byte 0x03 is the level as of the last deposit and is NOT updated while the mon is
   * in the party; the party record carries the live level at +0x21. Reading 0x03 for a
   * party mon is the classic way to show a stale level. */
  out->level      = party ? rec[0x21] : rec[0x03];
  out->catch_rate = rec[0x07];
  out->otId       = rd16be(rec + 0x0C);
  out->exp        = rd24be(rec + 0x0E);

  for (int i = 0; i < 4; i++) {
    out->moves[i]  = rec[0x08 + i];
    out->pp_raw[i] = rec[0x1D + i];
    out->pp[i]     = (uint8_t)(out->pp_raw[i] & 0x3Fu);   /* current PP  */
    out->ppups[i]  = (uint8_t)(out->pp_raw[i] >> 6);      /* PP Ups 0..3 */
  }
  for (int i = 0; i < 5; i++) out->statexp[i] = rd16be(rec + 0x11 + i * 2);

  /* DVs are 4 nibbles: Atk/Def in one byte, Spe/Spc in the next. There is no stored
   * HP DV — it is the four low bits of the others, in that order. */
  out->dv[G1_ATK] = (uint8_t)(rec[0x1B] >> 4);
  out->dv[G1_DEF] = (uint8_t)(rec[0x1B] & 0x0Fu);
  out->dv[G1_SPE] = (uint8_t)(rec[0x1C] >> 4);
  out->dv[G1_SPC] = (uint8_t)(rec[0x1C] & 0x0Fu);
  out->dv[G1_HP]  = (uint8_t)(((out->dv[G1_ATK] & 1u) << 3) | ((out->dv[G1_DEF] & 1u) << 2) |
                              ((out->dv[G1_SPE] & 1u) << 1) |  (out->dv[G1_SPC] & 1u));

  /* Names are NOT in the record — they sit in two parallel arrays after it. */
  gen1_decode_name(out->otName,   (int)sizeof out->otName,
                   list + ot_off   + slot * GEN1_NAME_BYTES, GEN1_NAME_BYTES);
  gen1_decode_name(out->nickname, (int)sizeof out->nickname,
                   list + nick_off + slot * GEN1_NAME_BYTES, GEN1_NAME_BYTES);
  return true;
}

bool gen1_decode_image(const Gen1Save* s, const uint8_t* img, int box, int slot, Gen1Mon* out) {
  uint32_t off = gen1_list_offset(s, box);
  if (!img || off == 0) { memset(out, 0, sizeof *out); return false; }
  return gen1_decode(img + off, box, slot, out);
}

/* ------------------------------------------------------------------------- */
/* Open / validate                                                            */
/* ------------------------------------------------------------------------- */

const char* gen1_status_text(Gen1Status st) {
  switch (st) {
    case GEN1_OK:             return "OK";
    case GEN1_ERR_SIZE:       return "not a 32 KiB Game Boy save";
    case GEN1_ERR_READ:       return "read error";
    case GEN1_ERR_CHECKSUM:   return "checksum mismatch (not a Western R/B/Y save)";
    case GEN1_ERR_STRUCTURE:  return "checksum passed but the save's contents are impossible";
    default:                  return "?";
  }
}

/* Whether an 11-byte name field looks like a name at all. Used only as a
 * false-positive guard, not as a format rule — see the note in gen1_open_ranged. */
static bool name_field_plausible(const uint8_t* f) {
  if (f[0] == 0x50u) return false;                       /* R/B/Y always have a name */
  for (int i = 0; i < GEN1_NAME_BYTES; i++) if (f[i] == 0x50u) return true;
  return false;                                          /* no terminator: not a name field */
}

Gen1Status gen1_open_ranged(Gen1ReadFn rd, void* ctx, uint32_t len, Gen1Save* out) {
  memset(out, 0, sizeof *out);
  if (!rd) return GEN1_ERR_READ;
  /* Gen-1 carts are MBC3/MBC5 without an RTC, so unlike Gen 2 there is no 44/48-byte
   * timestamp footer to expect — but emulators still pad, so accept a tail and
   * ignore it. Anything SHORTER than the image cannot be parsed. */
  if (len < GEN1_SAVE_SIZE) return GEN1_ERR_SIZE;

  uint8_t buf[128];

  Gen1Sum sum;
  gen1_sum_init(&sum);
  for (uint32_t off = GEN1_SUM_FIRST; off <= GEN1_SUM_LAST; ) {
    uint32_t n = GEN1_SUM_LAST - off + 1u;
    if (n > sizeof buf) n = sizeof buf;
    if (!rd(ctx, off, buf, n)) return GEN1_ERR_READ;
    gen1_sum_feed(&sum, buf, n);
    off += n;
  }
  out->checksum_calc = gen1_sum_final(&sum);
  if (!rd(ctx, GEN1_OFF_CHECKSUM, buf, 1)) return GEN1_ERR_READ;
  out->checksum_stored = buf[0];
  if (out->checksum_stored != out->checksum_calc) return GEN1_ERR_CHECKSUM;

  if (!rd(ctx, GEN1_OFF_PLAYER_NAME, buf, GEN1_NAME_BYTES)) return GEN1_ERR_READ;
  bool name_ok = name_field_plausible(buf);
  gen1_decode_name(out->player_name, (int)sizeof out->player_name, buf, GEN1_NAME_BYTES);

  if (!rd(ctx, GEN1_OFF_TRAINER_ID, buf, 2)) return GEN1_ERR_READ;
  out->trainer_id = rd16be(buf);

  if (!rd(ctx, GEN1_OFF_CURRENT_NO, buf, 1)) return GEN1_ERR_READ;
  out->current_box = buf[0] & 0x7Fu;          /* bit 7 = "the player has switched boxes" */

  if (!rd(ctx, GEN1_OFF_PARTY, buf, 1)) return GEN1_ERR_READ;
  out->party_count = buf[0];

  /* THE BOX COUNTS ARE NOT TRUSTWORTHY. pokered never initialises the eleven BANKED box
   * lists: EmptyAllSRAMBoxes runs only when the player first uses CHANGE BOX, so until
   * then those count bytes hold whatever the cartridge's SRAM powered up with — usually
   * 0xFF, sometimes anything. Two failure modes came out of trusting them:
   *   - a count of, say, 0x37 makes this parser hand the importer 55 "Pokemon" decoded
   *     from un-erased SRAM. Inventing Pokemon out of noise is the worst thing a save
   *     tool can do;
   *   - conversely, gating the whole save on "every count <= 20" REFUSED a perfectly
   *     legitimate save whose boxes were simply still virgin.
   * So an out-of-range count is neither trusted nor fatal: that box reads as EMPTY and
   * is flagged uninitialised. The CURRENT box lives in the main checksummed block and
   * IS trustworthy, which is why it still gates. */
  bool counts_ok = (out->current_box < GEN1_NUM_BOXES) && (out->party_count <= GEN1_PARTY_CAPACITY);
  if (counts_ok) {
    for (int b = 0; b < GEN1_NUM_BOXES; b++) {
      if (!rd(ctx, gen1_list_offset(out, b), buf, 1)) return GEN1_ERR_READ;
      if (b == out->current_box) {
        /* the open box's live copy sits in the checksummed main block */
        out->box_count[b] = buf[0];
        if (buf[0] > GEN1_BOX_CAPACITY) counts_ok = false;
      } else if (buf[0] > GEN1_BOX_CAPACITY) {
        out->box_count[b] = 0;
        out->box_uninit |= (uint16_t)(1u << b);
      } else {
        out->box_count[b] = buf[0];
      }
    }
  }

  /* The main checksum is only 8 bits, so one file in 256 that is NOT a Western R/B/Y
   * save passes it by luck — and the file most likely to be offered to this parser is
   * a Gen-2 save, which is the same 32 KiB size. These structural facts cost 13 byte
   * reads and make that coincidence effectively impossible. */
  if (!name_ok || !counts_ok) return GEN1_ERR_STRUCTURE;
  return GEN1_OK;
}

typedef struct { const uint8_t* img; uint32_t len; } Gen1ImgCtx;

static bool img_read(void* ctx, uint32_t off, void* buf, uint32_t len) {
  const Gen1ImgCtx* c = (const Gen1ImgCtx*)ctx;
  if (off > c->len || len > c->len - off) return false;
  memcpy(buf, c->img + off, len);
  return true;
}

Gen1Status gen1_open(const uint8_t* img, uint32_t len, Gen1Save* out) {
  if (!img) { memset(out, 0, sizeof *out); return GEN1_ERR_READ; }
  Gen1ImgCtx c = { img, len };
  return gen1_open_ranged(img_read, &c, len, out);
}

/* ------------------------------------------------------------------------- */
/* Box-bank checksums (advisory)                                              */
/* ------------------------------------------------------------------------- */

/* Each box bank ends with 7 checksum bytes: one over the whole bank's box area, then
 * one per box, all using the same inverted-sum algorithm as the main block.
 *
 * These are the least certain numbers in the documented layout — the exact summed
 * ranges were flagged VERIFY during research and no real R/B/Y save exists on this
 * machine to settle it. So a mismatch here NEVER blocks reading: gen1_open() does not
 * call this, the result is advisory, and the caller should present it as "this bank
 * looks odd" at most. If it turns out to be wrong it is wrong in a harmless place. */
Gen1Status gen1_check_banks(Gen1ReadFn rd, void* ctx, Gen1Save* io) {
  if (!rd || !io) return GEN1_ERR_READ;
  uint8_t buf[128];

  for (int bank = 0; bank < 2; bank++) {
    uint32_t base  = bank == 0 ? GEN1_OFF_BANK2 : GEN1_OFF_BANK3;
    uint32_t sums  = bank == 0 ? GEN1_OFF_BANK2_SUMS : GEN1_OFF_BANK3_SUMS;
    uint8_t  want[7];
    if (!rd(ctx, sums, want, 7)) return GEN1_ERR_READ;

    Gen1Sum whole;
    gen1_sum_init(&whole);
    bool ok = true;

    for (int b = 0; b < 6; b++) {
      Gen1Sum one;
      gen1_sum_init(&one);
      uint32_t start = base + (uint32_t)b * GEN1_BOX_BYTES;
      for (uint32_t off = 0; off < GEN1_BOX_BYTES; ) {
        uint32_t n = GEN1_BOX_BYTES - off;
        if (n > sizeof buf) n = sizeof buf;
        if (!rd(ctx, start + off, buf, n)) return GEN1_ERR_READ;
        gen1_sum_feed(&one,   buf, n);
        gen1_sum_feed(&whole, buf, n);
        off += n;
      }
      if (gen1_sum_final(&one) != want[1 + b]) ok = false;
    }
    if (gen1_sum_final(&whole) != want[0]) ok = false;
    io->bank_ok[bank] = ok;
  }
  io->banks_checked = true;
  return GEN1_OK;
}
