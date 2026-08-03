#ifndef FUSED_ROM_H
#define FUSED_ROM_H

#include <stdint.h>
#include <stdbool.h>

/*
 * "Fused" Pokemon ROM — the emulator path for the map viewer.
 *
 * The map screen works by opening the user's own Pokemon .gba as a FILE on the flashcart
 * microSD. An emulated GBA (Delta on a phone, mGBA on a PC) has no flashcart and no
 * filesystem, so that path simply cannot run there — which is why the emulator build used
 * to compile the map menu entry out entirely.
 *
 * The way round it: append the Pokemon ROM to PokeDNA's own image so both live in one
 * cartridge, and read it straight out of cartridge address space with a memcpy. The GBA
 * cart window is 32 MiB (0x08000000..0x09FFFFFF); PokeDNA is ~11.7 MiB and a Gen-3 ROM is
 * exactly 16 MiB, so a fused image is ~27.7 MiB and fits with room to spare. Verified
 * loading and reading its own tail correctly under mGBA.
 *
 * tools/fuse_rom.py does the appending and patches the locator record below.
 *
 * ---- what this path does NOT test -------------------------------------------
 * Reading from cartridge space bypasses FatFs and the flashcart driver COMPLETELY. Every
 * SD-only defect is therefore invisible here — including the disk_read DMA32 alignment bug
 * that corrupted Emerald's metatile tables. A map that renders perfectly in an emulator
 * proves the parser, the renderer and the UI; it proves nothing about the card. Hardware
 * sign-off is still required for anything touching the SD.
 *
 * ---- legality ---------------------------------------------------------------
 * A fused image contains a complete commercial game ROM. It is a local build artifact for
 * the machine that made it: never commit it, never publish it, never transmit it.
 */

/* The locator record the fuse tool finds and patches.
 *
 * `magic` is a char[8] with NO terminator, initialised element-by-element on purpose: a
 * string literal would be placed in the constant pool and could be merged or duplicated,
 * and the fuse tool requires the magic to appear EXACTLY ONCE in the image.
 *
 * The whole record is `const volatile`. `const` puts it in ROM so it is part of the image
 * the tool patches; `volatile` stops the compiler constant-folding the 0/0 it was compiled
 * with, which would optimise the entire fused path away. */
typedef struct {
  char     magic[8];
  uint32_t offset;      /* byte offset of the appended ROM from the start of the file */
  uint32_t size;        /* its length in bytes; 0 => not fused                        */
} PdnaFuseRec;

extern const volatile PdnaFuseRec g_pdna_fuse;

/* Is a Pokemon ROM appended to this image? Fills *size with its length when so. */
bool fused_rom_present(uint32_t* size);

/* A RomReadFn (see rom_map.h) over the appended ROM. `ctx` is ignored; `off` is a file
 * offset within the appended ROM, exactly as the FatFs reader receives it, so the two are
 * drop-in interchangeable. Bounds-checked against the recorded size. */
bool fused_rom_read(void* ctx, uint32_t off, void* dst, uint32_t len);

#endif /* FUSED_ROM_H */
