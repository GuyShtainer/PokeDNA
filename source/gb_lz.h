#ifndef GB_LZ_H
#define GB_LZ_H

#include <stdint.h>

#include "gb_sprite_codec.h"   /* GbReadFn */

/*
 * Clean-room Game Boy Color "LZ" tile-graphics decompressor -- BACKLOG #91
 * M1-G2 (docs/GB-MAP-DESIGN-G2.md §5.2, implemented from that section's
 * prose alone; docs/briefs/map-g2-brief.md Step 2).
 *
 * Every Gen-2 tileset's GFX is LZ-compressed (zero plain .2bpp tilesets in
 * either Gold or Crystal) -- this decoder is mandatory, not optional.
 *
 * Format (short form): command byte, bits 7-5 = command id (0-7), bits
 * 4-0 = length-1 (length 1..32). Long form (bits 7-5 == 0b111 but the raw
 * byte is not the LZ_END terminator 0xFF): a second header byte follows;
 * the REAL command id lives in bits 4-2 of the FIRST header byte, and the
 * 10-bit length (1..1024) is packed across both bytes.
 *
 *   0 LITERAL    copy `length` raw bytes from input to output
 *   1 ITERATE    read 1 byte, write it `length` times
 *   2 ALTERNATE  read 2 bytes, write them alternating for `length` bytes
 *   3 ZERO       write 0x00 `length` times
 *   4 REPEAT     copy `length` bytes forward from a back-reference
 *   5 FLIP       as REPEAT, but bit-reverses each byte
 *   6 REVERSE    as REPEAT, but reads the back-reference BACKWARD
 *   7 LONG       escape header; if it ever resolves as the real command
 *                (nested long-form), it behaves as REPEAT (hardware
 *                fallthrough -- there is no dedicated opcode for it)
 *
 * Back-reference offset, read immediately after the command header:
 *   - NEGATIVE (bit 7 of the first offset byte set): one byte, magnitude
 *     in the low 7 bits. distance = magnitude + 1 (the "+1" bias is real,
 *     confirmed against corpus data), back from the CURRENT output write
 *     position.
 *   - POSITIVE: two bytes, BIG-ENDIAN (the only big-endian field in the
 *     whole format). byte0 bits 6-0 are the high byte, byte1 the low byte;
 *     the 15-bit result is an ABSOLUTE offset into this blob's own
 *     decompressed output (offset 0 = the blob's first output byte).
 *
 * The terminator LZ_END = 0xFF is tested against the RAW, UNMASKED command
 * byte, strictly BEFORE any command-id extraction -- 0xFF's top 3 bits
 * numerically collide with the long-form escape, so masking first would
 * misparse the terminator as a command.
 *
 * Hardening (all six are reachable states on a mutated/hostile input, not
 * hypothetical):
 *   1. compressed input is capped at one GB bank (16,384 B) -- a missing
 *      LZ_END terminates the decode with failure, not a hang;
 *   2. the output cursor is checked against `out_cap` before every single
 *      byte written (one long-form command can legitimately demand up to
 *      1,024 bytes from ~3 input bytes);
 *   3. `0 <= src < out_len` is checked before every single byte copied --
 *      REPEAT/FLIP self-maintain this (src and out_len advance together),
 *      but REVERSE walks src BACKWARD while out_len only grows, so a
 *      back-reference valid at command start can still run off the front
 *      of the buffer mid-command;
 *   4. a forged nested LONG (id 7 resolving to id 7 again) behaves as
 *      REPEAT, matching hardware, and is still bounds-checked identically;
 *   5. the raw command byte is compared to LZ_END BEFORE masking;
 *   6. LITERAL/ITERATE/ALTERNATE payload bytes are read through the SAME
 *      bounded reader as command bytes -- a raw 0xFF inside a literal
 *      payload is legal data and must never be mistaken for a terminator.
 *
 * Pure C: no tonc, no FatFs, no GBA headers, no recursion (iterative state
 * machine only) -- all input arrives through the caller's GbReadFn, so
 * tests/host_gblz_test.c runs this exact code against real corpus blobs.
 */

#define GB_LZ_BANK_CAP 16384u   /* no real blob crosses a GB bank (measured) */

/* Decodes the LZ stream starting at `src_off` (at most `src_cap` input
 * bytes considered, internally clamped to GB_LZ_BANK_CAP) into `out`
 * (at most `out_cap` bytes). Returns the number of bytes written on a
 * clean LZ_END termination, or 0 on ANY malformed/overlong/out-of-bounds
 * input (fails closed -- never a partial write beyond what it reports,
 * and `out` may hold partial garbage on failure; callers must check the
 * return value before trusting `out`). */
uint32_t gb_lz_decode(GbReadFn read, void* ctx, uint32_t src_off, uint32_t src_cap,
                       uint8_t* out, uint32_t out_cap);

#endif /* GB_LZ_H */
