/*
 * Fused-GB-corpus reader. See fused_gb.h for the format and why a locator record is
 * used instead of scanning for the trailer at end-of-cartridge. Dependency-free like
 * fused_rom.c/fused_sav.c: no tonc, no FatFs, just bounds-checked reads out of the
 * cartridge address window — pure-C core per the toolkit's golden rules.
 *
 * The parsing/lookup surface is real ONLY under PDNA_DELTA (BACKLOG #62's own delta-gb
 * build is the only place a GB directory is ever fused); everywhere else it is a
 * permanent "not fused" stand-in, same split gb_art_source.c already uses for its own
 * PDNA_DELTA/non-PDNA_DELTA halves. The locator record itself (g_pdna_gbd) is compiled
 * unconditionally so tools/fuse_gb.py can always find and patch it, mirroring
 * fused_rom.c/fused_sav.c's own g_pdna_fuse/g_pdna_sav.
 */
#include <string.h>
#include "fused_gb.h"

#define CART_BASE 0x08000000u
#define CART_SPAN 0x02000000u          /* 32 MiB addressable cartridge window */

#define DIR_MAGIC_LEN 8
#define ENTRY_SIZE    48u             /* type(4) + name(32) + offset(4) + size(4) + crc32(4) */
#define TRAILER_SIZE  16u

/* `used` survives --gc-sections; `aligned(4)` so tools/fuse_gb.py can patch the two u32s
 * in place. Magic built element-by-element on purpose (see fused_rom.c's own comment):
 * a string literal could be pooled/duplicated and the tool requires it exactly once. */
const volatile PdnaGbdRec __attribute__((used, aligned(4))) g_pdna_gbd = {
  { 'P', 'D', 'N', 'A', 'G', 'B', 'D', '1' }, 0u, 0u
};

#ifndef PDNA_DELTA

bool fused_gb_present(void) { return false; }
int  fused_gb_entry_count(void) { return 0; }
bool fused_gb_entry(int i, uint32_t* type, const char** name, uint32_t* size) {
  (void)i; (void)type; (void)name; (void)size;
  return false;
}
bool fused_gb_rom(uint8_t gen, const uint8_t** base, uint32_t* size) {
  (void)gen; (void)base; (void)size;
  return false;
}
int  fused_gb_save_count(void) { return 0; }
bool fused_gb_save(int i, const char** name, const uint8_t** base, uint32_t* size) {
  (void)i; (void)name; (void)base; (void)size;
  return false;
}

#else /* PDNA_DELTA */

/* Cached entries, plain .bss (IWRAM) — NOT EWRAM_BSS, per the brief's EWRAM guard (no
 * new EWRAM statics). A handful of small fixed-size structs; parsed once, on first use. */
typedef struct {
  uint32_t type;
  char     name[FUSED_GB_NAME_MAX];
  uint32_t offset;
  uint32_t size;
} GbdEntry;

static GbdEntry s_entry[FUSED_GB_MAX_ENTRIES];
static int      s_count = -1;          /* -1 = not parsed yet, 0..N = parsed (N clamped) */

static bool dir_present_raw(uint32_t* dir_off, uint32_t* dir_size) {
  uint32_t off = g_pdna_gbd.offset;
  uint32_t sz  = g_pdna_gbd.size;
  if (!sz || !off) return false;                                /* unfused state */
  if (off >= CART_SPAN || sz > CART_SPAN - off) return false;    /* corrupt/hand-edited */
  if (sz < 12u + TRAILER_SIZE) return false;                     /* too small to hold anything */
  if (dir_off) *dir_off = off;
  if (dir_size) *dir_size = sz;
  return true;
}

/* Parses the directory into s_entry[]/s_count exactly once. Never trusts a single field
 * in isolation: the entry count implies an exact directory size, cross-checked against
 * the record's own `size` AND the trailer's restated size/magic, same defense-in-depth
 * tools/fuse_gb.py's own verify pass uses. A malformed directory degrades to
 * s_count == 0 (acts unfused) rather than reading out of bounds. */
static void parse_once(void) {
  if (s_count >= 0) return;
  s_count = 0;

  uint32_t dir_off, dir_size;
  if (!dir_present_raw(&dir_off, &dir_size)) return;

  const uint8_t* blk = (const uint8_t*)(CART_BASE + dir_off);
  if (memcmp(blk, "PDNAGBD1", DIR_MAGIC_LEN) != 0) return;

  uint32_t count;
  memcpy(&count, blk + 8, 4);
  uint64_t expect = 12u + (uint64_t)ENTRY_SIZE * count + TRAILER_SIZE;
  if (expect != dir_size) return;

  uint32_t trailer_off = 12u + ENTRY_SIZE * count;
  uint32_t t_size;
  memcpy(&t_size, blk + trailer_off, 4);
  if (t_size != dir_size) return;
  if (memcmp(blk + trailer_off + 4, "PDNAGBD1", DIR_MAGIC_LEN) != 0) return;

  int n = (int)count;
  if (n > FUSED_GB_MAX_ENTRIES) n = FUSED_GB_MAX_ENTRIES;   /* cache the first N; see .h */

  for (int i = 0; i < n; i++) {
    const uint8_t* e = blk + 12u + (uint32_t)i * ENTRY_SIZE;
    uint32_t type, off, size;
    memcpy(&type, e + 0, 4);
    memcpy(s_entry[i].name, e + 4, FUSED_GB_NAME_MAX);
    s_entry[i].name[FUSED_GB_NAME_MAX - 1] = 0;   /* defensive: force NUL termination */
    memcpy(&off,  e + 36, 4);
    memcpy(&size, e + 40, 4);
    /* Each payload must itself lie fully inside the cartridge window -- a corrupt
     * entry is dropped (not trusted) rather than handed to a caller as real. */
    if (off >= CART_SPAN || size > CART_SPAN - off) continue;
    s_entry[i].type = type;
    s_entry[i].offset = off;
    s_entry[i].size = size;
  }
  s_count = n;
}

bool fused_gb_present(void) {
  parse_once();
  return s_count > 0;
}

int fused_gb_entry_count(void) {
  parse_once();
  return s_count > 0 ? s_count : 0;
}

bool fused_gb_entry(int i, uint32_t* type, const char** name, uint32_t* size) {
  parse_once();
  if (i < 0 || i >= s_count) return false;
  if (type) *type = s_entry[i].type;
  if (name) *name = s_entry[i].name;
  if (size) *size = s_entry[i].size;
  return true;
}

bool fused_gb_rom(uint8_t gen, const uint8_t** base, uint32_t* size) {
  parse_once();
  uint32_t want = (gen == 1) ? FUSED_GB_ROM_GEN1 : (gen == 2) ? FUSED_GB_ROM_GEN2 : 0u;
  if (!want) return false;
  for (int i = 0; i < s_count; i++) {
    if (s_entry[i].type != want) continue;
    if (base) *base = (const uint8_t*)(CART_BASE + s_entry[i].offset);
    if (size) *size = s_entry[i].size;
    return true;
  }
  return false;
}

int fused_gb_save_count(void) {
  parse_once();
  int n = 0;
  for (int i = 0; i < s_count; i++) if (s_entry[i].type == FUSED_GB_SAV) n++;
  return n;
}

bool fused_gb_save(int i, const char** name, const uint8_t** base, uint32_t* size) {
  parse_once();
  if (i < 0) return false;
  for (int k = 0; k < s_count; k++) {
    if (s_entry[k].type != FUSED_GB_SAV) continue;
    if (i-- != 0) continue;
    if (name) *name = s_entry[k].name;
    if (base) *base = (const uint8_t*)(CART_BASE + s_entry[k].offset);
    if (size) *size = s_entry[k].size;
    return true;
  }
  return false;
}

#endif /* PDNA_DELTA */

/* Compiled unconditionally: outside PDNA_DELTA it is simply never reached (nothing
 * calls fused_gb_rom() to obtain a slice to read), but keeping it out of the #ifdef
 * avoids a fifth near-duplicate stub. */
bool fused_gb_slice_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  const FusedGbSlice* s = (const FusedGbSlice*)ctx;
  if (!s || !dst) return false;
  if (off > s->size || len > s->size - off) return false;   /* short read == failure, as FatFs */
  if (!len) return true;
  memcpy(dst, s->base + off, len);
  return true;
}
