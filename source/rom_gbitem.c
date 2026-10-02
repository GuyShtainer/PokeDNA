/* Gen-2 item descriptions out of the user's own cartridge -- see rom_gbitem.h for the
 * design, the pins and the shape this demands. Pure C, no statics. */
#include <string.h>

#include "rom_gbitem.h"
#include "gb_edit.h"   /* gb_char_decode: the existing Gen-2 text codec */

#define GB_BANK      0x4000u
#define WIN_LO       0x4000u
#define WIN_HI       0x8000u
#define TERM         0x50u   /* charmap "@" */
#define LINE         0x4Eu   /* charmap "next": the in-text line break */
#define POKE         0x54u   /* charmap "<POKE>": the game prints "POKe" (with the accent) */

/* Description-only glyphs gb_char_decode (a NAME codec) leaves as escapes: the contractions
 * $D4..$D6 of Gen 2's charmap ('s 't 'v; $D0..$D3 it already has), $E9 '&' and the <POKE> macro. */
static const char* desc_glyph(uint8_t c) {
  switch (c) {
    case 0xD4u: return "'s";
    case 0xD5u: return "'t";
    case 0xD6u: return "'v";
    case 0xE9u: return "&";
    case POKE:  return "POK\xC3\xA9";
    default:    return 0;
  }
}

typedef struct { uint8_t bank; uint16_t addr; } GbItemPin;
static const GbItemPin k_pins[] = {
  { 0x6E, 0x4000 },   /* Gold / Silver */
  { 0x72, 0x4987 },   /* Crystal       */
};
#define K_NPINS ((int)(sizeof k_pins / sizeof k_pins[0]))

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

/* The ONE indirect call in this file: every read goes through here, so tools/stack_edges.txt
 * declares exactly one dispatch (`RomGbItem.read @0 in rom_gbitem_rd`) instead of one per caller. */
__attribute__((noinline))
bool rom_gbitem_rd(const RomGbItem* gi, uint32_t off, void* dst, uint32_t len) {   /* external on purpose: no ISRA clone that hoists the hook out of the struct */
  return gi->read(gi->ctx, off, dst, len);
}

/* Entry i's pointer (i in 0..MAX_ID, so entry MAX_ID exists as the end marker of the last
 * described item). Returns 0 on a read failure or a pointer outside the bank window. */
static uint16_t entry(const RomGbItem* gi, unsigned i) {
  uint8_t b[2];
  if (i > ROM_GBITEM_MAX_ID) return 0;
  uint32_t off = gi->bank_off + (uint32_t)gi->taddr - WIN_LO + (uint32_t)i * 2u;
  if (!rom_gbitem_rd(gi, off, b, 2)) return 0;
  uint16_t v = rd16(b);
  return (v >= WIN_LO && v < WIN_HI) ? v : 0;
}

/* entry i+1's text is preceded by entry i's terminator, and the gap is a plausible length. */
static int link_ok(const RomGbItem* gi, unsigned i) {
  uint16_t a = entry(gi, i), n = entry(gi, i + 1u);
  if (!a || !n || n <= a || (unsigned)(n - a) > ROM_GBITEM_TEXT_MAX) return 0;
  uint8_t t;
  if (!rom_gbitem_rd(gi, gi->bank_off + (uint32_t)n - WIN_LO - 1u, &t, 1)) return 0;
  return t == TERM;
}

int rom_gbitem_open(RomGbItem* gi, GbReadFn read, void* ctx, uint32_t size) {
  if (!gi) return 0;
  memset(gi, 0, sizeof *gi);
  if (!read) return 0;
  for (int p = 0; p < K_NPINS; p++) {
    RomGbItem c;
    memset(&c, 0, sizeof c);
    c.read = read; c.ctx = ctx;
    c.bank_off = (uint32_t)k_pins[p].bank * GB_BANK;
    c.taddr = k_pins[p].addr;
    c.ok = 0;
    if (c.bank_off + GB_BANK > size) continue;                 /* the bank must be in the image */
    uint16_t first = entry(&c, 0);
    if (!first || first < (uint32_t)c.taddr + 2u * ROM_GBITEM_MAX_ID) continue;
    int good = 1;
    for (unsigned i = 0; i < ROM_GBITEM_PROBE && good; i++) good = link_ok(&c, i);
    if (good) good = link_ok(&c, ROM_GBITEM_MAX_ID - 1u);
    if (!good) continue;
    c.ok = 1;
    *gi = c;
    return 1;
  }
  return 0;
}

int rom_gbitem_desc(const RomGbItem* gi, uint8_t id, char* out, int cap) {
  if (out && cap > 0) out[0] = 0;
  if (!gi || !gi->ok || !gi->read || !out || cap < 2) return 0;
  if (id < 1u || id > ROM_GBITEM_MAX_ID) return 0;
  uint16_t a = entry(gi, (unsigned)id - 1u), n = entry(gi, id);
  if (!a || !n || n <= a || (unsigned)(n - a) > ROM_GBITEM_TEXT_MAX) return 0;
  uint8_t raw[ROM_GBITEM_TEXT_MAX];
  unsigned len = (unsigned)(n - a);                            /* includes the terminator */
  if (!rom_gbitem_rd(gi, gi->bank_off + (uint32_t)a - WIN_LO, raw, len)) return 0;
  if (raw[len - 1u] != TERM) return 0;
  int w = 0;
  for (unsigned i = 0; i + 1u < len; i++) {
    uint8_t c = raw[i];
    char g[8];   /* >= GB_GLYPH_MAX; room for the 5-byte "POK" + e-acute */
    int gl;
    const char* dg = desc_glyph(c);
    if (c == LINE) {
      /* a line that ends in '-' is the game's own hyphenation: join the halves, no hyphen */
      if (w > 0 && out[w - 1] == '-') { w--; continue; }
      g[0] = ' '; g[1] = 0; gl = 1;
    }
    else if (dg) { gl = (int)strlen(dg); if (gl >= (int)sizeof g) { out[0] = 0; return 0; } memcpy(g, dg, (size_t)gl); }
    else if (c < 0x7Fu) { out[0] = 0; return 0; }              /* any other control byte: refuse */
    else {
      gl = gb_char_decode(2, c, g);
      if (gl <= 0) { out[0] = 0; return 0; }
    }
    if (w + gl >= cap) { out[0] = 0; return 0; }               /* does not fit: refuse, never truncate */
    memcpy(out + w, g, (size_t)gl);
    w += gl;
  }
  while (w > 0 && out[w - 1] == ' ') w--;
  out[w] = 0;
  return w;
}
