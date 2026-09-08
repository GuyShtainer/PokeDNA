#ifndef FUSED_GB_H
#define FUSED_GB_H

#include <stdint.h>
#include <stdbool.h>

/*
 * "Fused" Game Boy corpus — BACKLOG #62, the delta-gb build for Guy's iPhone (Delta
 * emulator). Delta hands the running program exactly one cartridge image and nothing
 * else, same constraint fused_rom.h/fused_sav.h already document for the Gen-3 ROM/save.
 * Under PDNA_DELTA the GB half of PokeDNA has no SD card at all (gb_art_source.c's own
 * PDNA_DELTA half is fully stubbed) — this module is what lets it read real Game Boy
 * ROMs and saves out of cartridge address space instead.
 *
 * tools/fuse_gb.py appends N typed payloads (ROMs and/or battery saves) to the image,
 * plus ONE small directory block listing all of them, and patches g_pdna_gbd below to
 * point at that block. See the tool's own module docstring for the exact byte layout.
 *
 * WHY A LOCATOR RECORD, NOT "read the last 16 bytes of the cartridge"
 * ---------------------------------------------------------------------
 * The directory's own trailer (dir_size + magic again) is deliberately also the literal
 * last 16 bytes of the fused file, so a tool that only has the raw bytes (fuse_gb.py
 * itself, or a human hex-dumping the image) can find it without knowing anything else.
 * But the RUNNING program cannot reliably rediscover "how many bytes long is my own
 * cartridge" at runtime: the GBA flashcart maps a fixed 32 MiB window regardless of the
 * ROM's real size, the header carries no total-image-size field, and reads past the
 * physical ROM's end return unspecified data (open bus or wraparound) that differs by
 * flashcart/emulator — "scan backward from the mapped end" has no safe stopping rule.
 * So, like fused_rom.h and fused_sav.h before it, the C side finds the directory through
 * a locator record (magic + offset + size) that fuse_gb.py patches directly, and never
 * needs to know the image's total size at all.
 *
 * ONE fixed-size record, however many payloads are fused: `offset`/`size` here point at
 * the DIRECTORY BLOCK, not at any one payload — the directory itself carries each
 * payload's own offset/size/type, unbounded in count without a new locator per payload.
 *
 * ---- what a directory entry contains -----------------------------------------------
 * type   : FUSED_GB_ROM_GEN1 (1) / FUSED_GB_ROM_GEN2 (2) / FUSED_GB_SAV (3)
 * name   : original filename, e.g. "Red.gb", "Gold.sav" — ASCII, NUL-padded, <=31 chars
 * offset : byte offset of the payload from the START of the fused file (same convention
 *          as PdnaFuseRec/PdnaSavRec, so "CART_BASE + offset" is a plain memcpy)
 * size   : payload length in bytes
 *
 * ---- caching (EWRAM guard) -----------------------------------------------------------
 * The directory is parsed ONCE, on first use, into a small fixed-size array of plain
 * (offset,size,type) triples cached in .bss (IWRAM, NOT EWRAM_BSS — no new EWRAM static
 * budget spent, per the delta-gb brief). FUSED_GB_MAX_ENTRIES bounds it; a build that
 * fuses more payloads than that still round-trips the ones that fit the cache order
 * (first N in directory order) but fused_gb_present() logs if the directory holds more.
 *
 * ---- legality / privacy --------------------------------------------------------------
 * Same weight as fused_rom.h/fused_sav.h: fused ROMs are commercial Game Boy games,
 * fused saves are personal play data. Never commit, publish, or transmit a fused image.
 */

#define FUSED_GB_ROM_GEN1 1u
#define FUSED_GB_ROM_GEN2 2u
#define FUSED_GB_SAV      3u

#define FUSED_GB_MAX_ENTRIES 12   /* generous headroom over the default 6-payload recipe */
#define FUSED_GB_NAME_MAX    32

typedef struct {
  char     magic[8];
  uint32_t offset;      /* byte offset of the directory block from the start of the file */
  uint32_t size;         /* the directory block's own length in bytes; 0 => not fused    */
} PdnaGbdRec;

extern const volatile PdnaGbdRec g_pdna_gbd;

/* Is a GB directory fused into this image? Parses it (once, cached) if so. */
bool fused_gb_present(void);

/* How many entries the (already-parsed) directory holds, clamped to
 * FUSED_GB_MAX_ENTRIES. 0 if fused_gb_present() is false. */
int fused_gb_entry_count(void);

/* The i'th directory entry (0..fused_gb_entry_count()-1). Any out param may be NULL. */
bool fused_gb_entry(int i, uint32_t* type, const char** name, uint32_t* size);

/* The FIRST fused ROM of generation `gen` (FUSED_GB_ROM_GEN1/2 — same numeric value as
 * PDNA_GEN1/PDNA_GEN2, see pdna_origin_art.h). `*base` is a pointer directly into
 * cartridge address space (0x08000000 + offset); `*size` its length. Either out param
 * may be NULL. */
bool fused_gb_rom(uint8_t gen, const uint8_t** base, uint32_t* size);

/* Number of fused SAV payloads. */
int fused_gb_save_count(void);

/* The i'th fused SAV (0..fused_gb_save_count()-1): its original filename, a pointer
 * into cartridge space, and its length. Any out param may be NULL. */
bool fused_gb_save(int i, const char** name, const uint8_t** base, uint32_t* size);

/* A GbReadFn (gb_sprite_codec.h) / RomReadFn-shaped reader over a fused GB ROM slice.
 * `ctx` must be a `const FusedGbSlice*` (below) describing the exact base/size pair
 * fused_gb_rom() returned — bounds-checked against it, so a caller cannot read past the
 * one payload it asked for even though the underlying memory is plain cartridge space. */
typedef struct {
  const uint8_t* base;
  uint32_t       size;
} FusedGbSlice;

bool fused_gb_slice_read(void* ctx, uint32_t off, void* dst, uint32_t len);

#endif /* FUSED_GB_H */
