/*
 * Fused-ROM reader. See fused_rom.h for why this exists and what it deliberately does not
 * prove. Deliberately dependency-free: no tonc, no FatFs — it is a bounds-checked memcpy
 * out of the cartridge window.
 */
#include <string.h>
#include "fused_rom.h"

#define CART_BASE 0x08000000u
#define CART_SPAN 0x02000000u          /* 32 MiB addressable cartridge window */

/* `used` survives --gc-sections (gba.specs turns it on), which would otherwise drop a
 * record nothing in the program references at link time. `aligned(4)` because the fuse
 * tool requires the magic to be 4-byte aligned so it can patch the two u32s in place. */
const volatile PdnaFuseRec __attribute__((used, aligned(4))) g_pdna_fuse = {
  { 'P', 'D', 'N', 'A', 'F', 'U', 'S', 'E' }, 0u, 0u
};

bool fused_rom_present(uint32_t* size) {
  uint32_t off = g_pdna_fuse.offset;
  uint32_t sz  = g_pdna_fuse.size;
  /* Both zero is the unfused state and the signal to fall back to the SD path. Anything
   * that would not fit the cartridge window is a corrupt or hand-edited record — refuse
   * rather than read off the end of the address space. */
  if (!sz || !off) return false;
  if (off >= CART_SPAN || sz > CART_SPAN - off) return false;
  if (size) *size = sz;
  return true;
}

bool fused_rom_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  uint32_t base, sz;
  if (!dst) return false;
  if (!fused_rom_present(&sz)) return false;
  if (off > sz || len > sz - off) return false;      /* short read == failure, as FatFs */
  if (!len) return true;

  base = CART_BASE + g_pdna_fuse.offset + off;
  /* Cartridge ROM is readable by 8/16/32-bit loads (only WRITES are the constrained
   * direction on this bus), so a plain memcpy is correct for any alignment. No OS-mode
   * concern here at all: nothing is unmapped, because there is no flashcart in the loop. */
  memcpy(dst, (const void*)base, len);
  return true;
}
