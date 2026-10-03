#ifndef ROM_GBITEM_INCLUDED
#define ROM_GBITEM_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#include "gb_sprite_codec.h"   /* GbReadFn */

/*
 * Gen-2 ITEM DESCRIPTIONS, read live out of the user's own Game Boy Color cartridge dump
 * (Gold / Silver / Crystal) -- BACKLOG #340b. Gen 1 carries no item-description text at all
 * (its items have names only), so a Gen-1 ROM never opens here and the caller keeps its
 * honest "no description" string.
 *
 * PokeDNA ships NO Game Freak text: this is a transient read of a file the user already
 * owns, the same posture rom_gbbase.c / rom_gbsprite.c take. PURE C: no tonc, no FatFs, no
 * GBA headers, no statics; every byte comes through the caller's GbReadFn, so
 * tests/host_romgbitem_test.c runs this exact code on the PC against the real dumps.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DATA IS (clean-room: the .sym files gave the ADDRESSES, nothing else is borrowed)
 * ---------------------------------------------------------------------------
 * The game keeps a table of 16-bit little-endian pointers, one per item id (index id-1), each
 * pointing into the SAME 16 KiB ROM bank at a 0x50-terminated string in the Gen-2 charset
 * (the bytes the game itself prints; 0x4E is its in-text line break). The strings are laid
 * out in table order, so entry i+1 points one byte past entry i's terminator. The table's
 * place is the same in every revision of a game family the symbol files cover:
 *   Gold / Silver (+ VC)            bank 0x6E, address 0x4000
 *   Crystal 1.0 / 1.1 / AU (+ VC)   bank 0x72, address 0x4987
 * rom_gbitem_open() tries those two pins and ACCEPTS one only if the bytes there have the
 * table's SHAPE (see below); a hack, translation or unknown revision that moved the table
 * simply fails to open, and the caller falls back to its honest string. Nothing is guessed.
 *
 * Ids ROM_GBITEM_MAX_ID+1.. are the TM/HM block: the game prints the MOVE's description for
 * those, the table holds shared stand-ins, and this module refuses them (returns 0).
 *
 * THE SHAPE rom_gbitem_open() DEMANDS: the pin lies inside the image; the first entry points
 * past the table head (>= table + 2*MAX_ID) and inside the bank window; for the first
 * ROM_GBITEM_PROBE entries AND the last described one, entry i+1 is above entry i by at most
 * ROM_GBITEM_TEXT_MAX bytes and the byte just before entry i+1's text is the 0x50 terminator.
 *
 * BOUNDED: one description is at most ROM_GBITEM_TEXT_MAX raw bytes (read into a stack
 * buffer of that size) and ROM_GBITEM_DESC_MAX decoded bytes; open() makes at most
 * 2 pins x (1 + 2 x ROM_GBITEM_PROBE... see the .c) small reads.
 */

#define ROM_GBITEM_MAX_ID     190u   /* ids 1..190; 191+ are TM/HM (move text, not served) */
#define ROM_GBITEM_TEXT_MAX   80u    /* longest raw description measured is 38 B           */
#define ROM_GBITEM_DESC_MAX   64u    /* decoded buffer the caller supplies (incl NUL)      */
#define ROM_GBITEM_PROBE      16u    /* leading entries chain-checked at open              */

typedef struct RomGbItem {
  GbReadFn read;       /* FIRST (offset 0): tools/stack_edges.txt keys the one dispatch on it   */
  void*    ctx;
  uint32_t bank_off;   /* bank * 0x4000: file offset of the bank; file = bank_off + (addr - 0x4000) */
  uint16_t taddr;      /* the table's address inside the bank window (0x4000..0x7FFF)               */
  uint8_t  ok;         /* 1 = a pin matched the table shape                                         */
  uint8_t  pin;        /* index of the matched pin (#403b: lets a caller cache the probe)           */
} RomGbItem;

/* Try the pins; fill *gi. Returns 1 on a shape match, 0 (gi->ok == 0) otherwise. A read
 * failure is a plain 0. The reader and its ctx are kept in *gi for rom_gbitem_desc(). */
int rom_gbitem_open(RomGbItem* gi, GbReadFn read, void* ctx, uint32_t size);   /* read/ctx must outlive gi */

/* #403b: re-open on a pin index remembered from an earlier rom_gbitem_open() of the SAME file
 * (gi->pin), skipping the 16-entry probe: only the pin's bank bound and the first / last
 * entry links are re-checked (4 small reads instead of ~36). Returns 0 on any doubt -- the
 * caller then falls back to rom_gbitem_open(). */
int rom_gbitem_open_pin(RomGbItem* gi, GbReadFn read, void* ctx, uint32_t size, uint8_t pin);

/* Decode item `id` (1..ROM_GBITEM_MAX_ID) into out (cap >= 2; ROM_GBITEM_DESC_MAX is always
 * enough): the description with the game's line breaks turned into single spaces, trailing
 * spaces trimmed, UTF-8 via gb_char_decode (an unknown byte becomes its "{XX}" escape, a
 * control byte other than the line break refuses the whole description). Returns the
 * length written, or 0 (out[0] = 0) for a bad id/arg, a pointer outside the bank window,
 * an entry whose predecessor is not 0x50-terminated, a text without its terminator inside
 * ROM_GBITEM_TEXT_MAX, or a decode that does not fit `cap`. */
int rom_gbitem_desc(const RomGbItem* gi, uint8_t id, char* out, int cap);

/* Internal: the one read through gi->read. Declared here (not static) so the compiler cannot
 * clone it and lift the hook out of the struct, which would hide the dispatch from
 * tools/stack_budget.py. Do not call from outside rom_gbitem.c. */
bool rom_gbitem_rd(const RomGbItem* gi, uint32_t off, void* dst, uint32_t len);

#endif /* ROM_GBITEM_INCLUDED */
