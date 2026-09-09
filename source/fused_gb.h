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
 * (offset,size,type) triples cached in EWRAM_BSS (#62 review D1 moved this cache out of
 * IWRAM -- the delta build's tight budget is IWRAM/stack headroom, not EWRAM; see
 * fused_gb.c's own comment). FUSED_GB_MAX_ENTRIES bounds it; a build that fuses more
 * payloads than that still round-trips the ones that fit the cache order (first N in
 * directory order), silently dropping the rest -- this module cannot log the drop
 * itself (it is a pure-C core with no log.h/tonc dependency by design, matching
 * fused_rom.c/fused_sav.c); a caller that needs to know would have to compare its own
 * expected payload count against fused_gb_entry_count().
 *
 * #68b review D1: each cached entry stores `name` as a raw pointer straight into
 * cartridge address space (validated at parse time to contain a NUL within its 32-byte
 * field, else the pointer is a shared empty-string literal) rather than a 32-byte
 * local copy -- the directory block itself stays resident in cartridge address space
 * for the program's whole lifetime, so there is nothing to copy FROM defensively. This
 * shrinks one cached entry from 44 bytes (type+name[32]+offset+size) to 16
 * (type+offset+size+name pointer), so raising FUSED_GB_MAX_ENTRIES 12->24 to cover
 * delta-gb's own recipe is a net DECREASE in cache footprint (12*44=528 B -> 24*16=384
 * B), not a cost to budget against.
 *
 * ---- legality / privacy --------------------------------------------------------------
 * Same weight as fused_rom.h/fused_sav.h: fused ROMs are commercial Game Boy games,
 * fused saves are personal play data. Never commit, publish, or transmit a fused image.
 */

#define FUSED_GB_ROM_GEN1 1u
#define FUSED_GB_ROM_GEN2 2u
#define FUSED_GB_SAV      3u
#define FUSED_GB_LOC      4u   /* BACKLOG #68b: a fused rom_gb*_open_loc() record --
                                * see the block comment below and tools/gbloc_driver.c
                                * for the [header+record] payload this directory entry's
                                * offset/size point at */

/* BACKLOG #68b: the FUSED_GB_LOC payload's own sub-selector -- one directory entry per
 * (kind, generation) pair, since a single ROM satisfies up to three different locators
 * (sprite always, icon Gen-2 only, ui always). Matches tools/fuse_gb.py's LOC_KIND
 * constants (LOC_KIND_SPRITE etc, and LOC_KIND_NAMES) and tools/gbloc_driver.c's
 * identical values -- kept in three places on purpose (C header the GBA build links,
 * the Python tool, and the host driver) rather than a generated shared file, same
 * posture FUSED_GB_ROM_GEN1/2/SAV already have across this header and fuse_gb.py. */
#define FUSED_GB_LOC_SPRITE 1u
#define FUSED_GB_LOC_ICON   2u
#define FUSED_GB_LOC_UI     3u

#define FUSED_GB_MAX_ENTRIES 24   /* #68b review D1: headroom over delta-gb's OWN default
                                   * recipe: 3 ROMs x (1 ROM + 1 SAV + up to 3 LOC) = 15
                                   * entries -- the prior 12 silently dropped Crystal's
                                   * save and one LOC record with no error anywhere */
#define FUSED_GB_NAME_MAX    32

typedef struct {
  char     magic[8];
  uint32_t offset;      /* byte offset of the directory block from the start of the file */
  uint32_t size;         /* the directory block's own length in bytes; 0 => not fused    */
} PdnaGbdRec;

#ifdef FUSED_GB_TEST
/* #62 review D9: the host test builds its OWN directory and needs to point this record
 * at it directly -- `const volatile` (below, the real declaration) is exactly right for
 * every other translation unit (tools/fuse_gb.py patches this in the compiled ELF/GBA
 * bytes, never at compile time) but would make that impossible here. Never defined
 * outside tests/host_fusedgb_test.c's own cc line. */
extern PdnaGbdRec g_pdna_gbd;
/* #62 review D9: fused_gb.c's cart_ptr() reads through this instead of CART_BASE+off
 * when set -- points the parser at a plain host buffer instead of unmapped cartridge
 * address space. NULL (its zero-init default) means "use the real CART_BASE path",
 * which is never exercised by this test build at all. */
extern const uint8_t* g_fused_gb_test_base;
/* Forces the NEXT parse_once() to re-parse from g_pdna_gbd/g_fused_gb_test_base
 * instead of serving the previous case's cached result. Test-only. */
void fused_gb_test_reset(void);
#else
extern const volatile PdnaGbdRec g_pdna_gbd;
#endif

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

/* BACKLOG #68b: the FIRST fused LOC record matching (kind, gen) -- `kind` is one of
 * FUSED_GB_LOC_SPRITE/ICON/UI, `gen` is FUSED_GB_ROM_GEN1/2 (same numbering
 * fused_gb_rom() uses). Mirrors fused_gb_rom()'s own "first entry of this generation"
 * convention deliberately: tools/fuse_gb.py appends each ROM's LOC entries
 * immediately after that ROM's own payload, in the same command-line order, so the
 * FIRST (kind,gen) match in directory order always belongs to the SAME ROM
 * fused_gb_rom(gen) itself would return -- e.g. delta-gb's Gold.gbc+Crystal.gbc are
 * both Gen 2, but fused_gb_rom(2) answers Gold (fused first) and this answers Gold's
 * loc too, never Crystal's stale one.
 *
 * `*rec` points directly at the record bytes (RomGbSpriteLoc / RomGbIconLoc /
 * RomGbUiLoc, exactly as tools/gbloc_driver.c wrote them) inside cartridge address
 * space -- read-only, valid for the process lifetime like every other fused_gb_*
 * pointer. `*id_hash`/`*rom_size` are the record's OWN claimed id_hash/ROM-size
 * (redundant cross-checks a caller can compare against the ROM it actually opened
 * before trusting the record, exactly the belt-and-braces posture
 * rom_gbsprite_open_loc()/etc. already apply to loc->id_hash/loc->size themselves --
 * this is one layer further out, catching "wrong ROM's loc" before the record's own
 * fields are even inspected).
 *
 * Returns false (any out param left untouched) if no fused directory, no matching
 * entry, or the entry's own [header+record] framing does not check out (bad magic,
 * rec_size not matching the entry's own directory-recorded size) -- the CRC-32 every
 * directory entry already carries (parse_once()'s own per-entry check, run
 * unconditionally regardless of type) has ALREADY been verified before an entry is
 * even visible here, same as every other fused_gb_* lookup. Either out param may be
 * NULL. */
bool fused_gb_loc(uint8_t kind, uint8_t gen, const uint8_t** rec, uint32_t* rec_len,
                  uint32_t* id_hash, uint32_t* rom_size);

#endif /* FUSED_GB_H */
