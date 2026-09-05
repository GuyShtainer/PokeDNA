/*
 * Fused-save reader. See fused_sav.h for the precedence rule that keeps this from ever
 * being able to clobber a real save. Dependency-free like fused_rom.c: no tonc, no FatFs,
 * just a bounds-checked memcpy out of the cartridge window.
 */
#include <string.h>
#include "fused_sav.h"

#define CART_BASE 0x08000000u
#define CART_SPAN 0x02000000u          /* 32 MiB addressable cartridge window */

/* `used` survives --gc-sections; `aligned(4)` so tools/fuse_sav.py can patch the two u32s
 * in place. The magic is built element-by-element on purpose — a string literal could be
 * pooled or duplicated, and the tool requires it to appear EXACTLY ONCE in the image. */
const volatile PdnaSavRec __attribute__((used, aligned(4))) g_pdna_sav = {
  { 'P', 'D', 'N', 'A', 'S', 'A', 'V', '1' }, 0u, 0u
};

bool fused_sav_present(uint32_t* size) {
  uint32_t off = g_pdna_sav.offset;
  uint32_t sz  = g_pdna_sav.size;
  if (!sz || !off) return false;                       /* the unfused state */
  if (off >= CART_SPAN || sz > CART_SPAN - off) return false;   /* corrupt/hand-edited */
  if (size) *size = sz;
  return true;
}

bool fused_sav_read(void* dst, uint32_t len) {
  uint32_t sz;
  if (!dst || !len) return false;
  if (!fused_sav_present(&sz)) return false;
  if (len != sz) return false;                         /* caller must ask for exactly it */
  memcpy(dst, (const void*)(CART_BASE + g_pdna_sav.offset), len);
  return true;
}

/* Third, optional payload: a bare 80-byte Gen-3 box record (tools/fuse_sav.py --clip).
 * Own locator record/magic so a build with only a fused save (the common case) never
 * has to carry this at all -- fuse_sav.py leaves it zeroed when --clip is not passed. */
const volatile PdnaClipRec __attribute__((used, aligned(4))) g_pdna_clip = {
  { 'P', 'D', 'N', 'A', 'C', 'L', 'P', '1' }, 0u, 0u
};

bool fused_clip_present(uint32_t* size) {
  uint32_t off = g_pdna_clip.offset;
  uint32_t sz  = g_pdna_clip.size;
  if (!sz || !off) return false;
  if (off >= CART_SPAN || sz > CART_SPAN - off) return false;
  if (size) *size = sz;
  return true;
}

bool fused_clip_read(void* dst, uint32_t len) {
  uint32_t sz;
  if (!dst || !len) return false;
  if (!fused_clip_present(&sz)) return false;
  if (len != sz) return false;
  memcpy(dst, (const void*)(CART_BASE + g_pdna_clip.offset), len);
  return true;
}
