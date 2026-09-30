#ifndef ROM_MON_H
#define ROM_MON_H

#include <stdint.h>
#include "rom_map.h"   /* RomCtx / RomReadFn / ROM_BASE */

/*
 * Pokemon ICONS read out of the user's own retail ROM at runtime.
 *
 * This is phase 1 of the ROM-gated build (docs/research-rom-gated-build.md): the
 * shipped binary carries no icon art; when the user's ROM is available (fused into
 * the image, or later a registered SD file), the box screen streams the real 32x32
 * icons straight from it. Nothing is copied anywhere — reads are transient, the
 * posture the map viewer has always used.
 *
 * WHERE THE DATA LIVES (verified in the pret decomps + dumped from Guy's carts):
 * Game Freak's own index block `GFRomHeader` sits at ROM+0x100 in Emerald and
 * FireRed/LeafGreen (Ruby/Sapphire predate it — they are served from PINNED addresses,
 * AXVE rev 2 + AXPE rev 1, BACKLOG #293; any other R/S revision fails closed):
 *     +0x38  const u8* const* monIcons            (species -> raw 4bpp, 2 frames of
 *                                                  512 B each, tonc tile order)
 *     +0x3C  const u8*        monIconPaletteIds   (species -> palette 0..2)
 *     +0x40  const struct SpritePalette* monIconPalettes  ({const u16* data; u16 tag}
 *                                                  x3, data = 32 B raw, uncompressed)
 * The tables extend past the 411 normal species: 412 = the Egg icon, 413..439 = the
 * Unown letters B..? (letter A is species 201 itself), so every form is reachable.
 *
 * Pure C (no tonc/FatFs): everything goes through the RomCtx's RomReadFn, so the
 * host tests run this against the real cartridge dumps on the PC.
 */

typedef struct RomMon {
  const RomCtx* rc;
  uint32_t icons;         /* FILE offset of gMonIconTable                  */
  uint32_t pal_ids;       /* FILE offset of gMonIconPaletteIndices         */
  uint32_t pals;          /* FILE offset of gMonIconPaletteTable           */
  int      ok;            /* 1 = header parsed and pointers sane           */
} RomMon;

#define ROM_MON_ICON_BYTES   512   /* one 32x32 4bpp frame                 */
#define ROM_MON_ICON_FRAMES  2
#define ROM_MON_PALS         3

/* Parse the GF header + sanity-check the three icon pointers. Returns 1 on
 * success; 0 (fail closed) for an unpinned Ruby/Sapphire revision, an unknown ROM, or
 * any pointer that does not stay inside the image. */
int rom_mon_open(RomMon* rm, const RomCtx* rc);

/* Where one species' icon lives: the answer to the two SMALL table lookups (the
 * gMonIconTable pointer and the palette index) that every frame read had to redo.
 * Splitting locate from read matters because a caller that reads the same frame
 * twice to verify it — box_oam does — otherwise paid those lookups twice as well:
 * six RomCtx reads per icon instead of two. On the SD-file source every read is an
 * f_lseek + f_read, and the offsets ping-pong across the whole image, so seek cost is
 * the cost.
 *
 * TWO CORRECTIONS to what this comment used to claim (2026-08-23, measured, not read
 * off the source). First, FF_USE_FASTSEEK is now 1, and it is OPT-IN PER HANDLE, not a
 * global switch on the save write path -- source/fastseek.h has the proof and
 * tests/host_fastseek_test.c holds it to account; the icon handle is armed with a
 * cluster link map, so its far seeks are an in-RAM table lookup. Second, even WITHOUT
 * a map a backward seek on a CONTIGUOUS 16 MB file is about 4 disk_read calls, not
 * hundreds: FatFs caches one FAT sector and 128 FAT32 entries live in each, so the
 * walk is mostly CPU (~400 get_fat iterations, ~0.7 ms), and its cost tracks the seek
 * TARGET's cluster index rather than the distance jumped -- f_lseek(1024) from EOF
 * costs zero reads. The expensive case is a FRAGMENTED file, which is exactly what
 * fastseek_arm's logged fragment count now measures on the user's own card.
 *
 * Since 2026-08-23 the app does not use this path per icon at all: source/icon_store.c
 * calls rom_mon_read_tables() (below) once per registration and every later locate is
 * a RAM index. rom_mon_locate_row_verified survives as the fallback for a session whose
 * table load failed, and as the only path that still needs the per-field verify. */
typedef struct RomMonLoc {
  uint32_t tiles;         /* FILE offset of frame 0 (frame 1 sits +512 after it) */
  uint8_t  pal;           /* palette index 0..2                                  */
  uint8_t  ok;            /* 1 = usable; 0 = never located / lookup failed        */
} RomMonLoc;

/* Resolve (species, form) to its icon offset + palette index. Two small reads.
 * `form` handles Unown (species 201, form 0..27); pass 0 otherwise. Species 412
 * = the Egg. Returns 1 on success, 0 on any bounds/read failure (and *out is
 * left with ok = 0, so a memoised RomMonLoc self-invalidates). */
int rom_mon_locate(const RomMon* rm, uint16_t species, uint8_t form, RomMonLoc* out);

/* Same, but VERIFIED: every table field is read twice back to back and accepted only
 * when both passes agree, retried up to `attempts` times (minimum 2).
 *
 * WHY a caller that memoises the location MUST use this. The two fields locate reads
 * are the icon's ADDRESS and its palette bank. Garble the pointer and ptr_ok still
 * accepts it as long as it lands inside the image with 1 KiB of room, so the frame
 * read succeeds — off the WRONG offset. A caller that verifies the 512 B frame by
 * reading it twice cannot catch that: with the location located once, both of its
 * passes read the same wrong offset, the two frames are identical, the verify says
 * "stable", and the slot shows another species' art with nothing logged. The same
 * goes for the palette byte: a garbled id that still lands in 0..2 picks the wrong
 * OBJ palette bank. Before the memo existed, box_oam re-did these lookups inside
 * every verify pass, and that is the coverage this restores — cheaply, because the
 * second read of a 1..4 byte field is a backward seek that stays inside the current
 * cluster (see read_small in rom_mon.c) and so costs no extra FAR seek.
 *
 * Returns 1 with *out usable; 0 on failure with out->ok = 0 (so a memoised RomMonLoc
 * self-invalidates). `unstable` (may be NULL) is set to 1 only for the case worth
 * logging: the reads themselves succeeded but never agreed. A bounds/read failure
 * leaves it 0, so a corrupt species id in a save cannot spam the log. */
int rom_mon_locate_verified(const RomMon* rm, uint16_t species, uint8_t form,
                            RomMonLoc* out, int attempts, int* unstable);

/* Same as rom_mon_locate_verified, but by the icon table's own row index (0..439:
 * 0..411 the species axis's raw ceiling, 412 the Egg, 413..439 Unown B..'?') instead
 * of a (species, form) pair. This is the axis source/art_icons_extract.c's cache
 * mirrors 1:1 (icons.bin row r == this table's row r) and the axis
 * source/art_icons_cache.c's loader indexes by, so both sides of the cache agree with
 * rom_mon.c's own layout by construction, not by re-deriving table_species(). Total
 * row count is RM_TABLE_ENTRIES (440), exposed as rom_mon_table_rows(). */
int rom_mon_locate_row_verified(const RomMon* rm, uint16_t row, RomMonLoc* out,
                                int attempts, int* unstable);
uint16_t rom_mon_table_rows(void);

/* ---- the WHOLE index, once, verified -- which is what makes the ROM rung bulk ----
 *
 * The three tables are read in full and checked, instead of two 1-4 byte fields being
 * re-read per icon forever. That is a performance change and a SAFETY change, and the
 * safety half is the bigger one.
 *
 * WHY IT IS SAFER, not just faster. rom_mon_locate_verified's own header states the
 * hazard it cannot close: a garbled 4-byte pointer that still passes ptr_ok makes BOTH
 * frame-verify passes read the same wrong offset, agree with each other, and paint
 * another species confidently with nothing logged. Verifying the field itself is the
 * only place that error is visible -- and once every field has been verified ONCE, at
 * load, there is no per-icon locate path left to get it wrong and no single-entry memo
 * left to poison. (source/box_oam.c and source/art_fallbacks.c used to keep one such
 * memo EACH, over the same open ROM, with different verification policies.)
 *
 * WHAT IT COSTS. 12 RomReadFn calls total, once per registration: each of the three
 * tables is read twice back to back and accepted only when the two passes agree, and
 * the palette DATA (3 x 32 B at scattered pointers) the same way. Against 4 calls of
 * locate per icon that pays for itself after 1.5 icons, and every icon after that
 * locates for ZERO I/O. The tables are ADJACENT on every GF ROM measured (icons + 1760
 * = pal ids, + 440 = the palette structs, on Guy's Emerald/FireRed/LeafGreen dumps) but
 * that is NOT relied on -- each is read at its own header pointer, so a ROM that lays
 * them out differently still works.
 *
 * PER-ROW FAILURE IS PER-ROW. A row whose pointer fails ptr_ok, or whose palette id is
 * not 0..2, is marked ROM_MON_OFF_NONE / 0xFF and simply has no icon -- exactly what
 * locate did for that row before. One bad row does not cost the other 439 their table.
 *
 * `off` receives FILE offsets (already ROM_BASE-relative), `palid` the raw bank ids,
 * `pals` the 3 x 16 RGB15 entries. All three are caller-owned and are only written on
 * success. `bad_rows` (may be NULL) receives how many rows were marked unusable.
 * Returns 1 with all three tables usable; 0 if any of them could not be read or never
 * verified -- in which case NOTHING is written and the caller must keep using
 * rom_mon_locate_row_verified per icon. */
#define ROM_MON_OFF_NONE 0xFFFFFFFFu
int rom_mon_read_tables(const RomMon* rm, uint32_t off[], uint8_t palid[],
                        uint16_t pals[][16], int* bad_rows);

/* Read one 512 B frame of an already-located icon. One read, no lookups. */
int rom_mon_icon_at(const RomMon* rm, const RomMonLoc* loc, uint8_t frame,
                    uint8_t dst[ROM_MON_ICON_BYTES]);

/* Read one 512 B icon frame for an internal species id into dst, and that
 * species' palette index (0..2) into *pal_index (may be NULL). `form` handles
 * Unown (species 201, form 0..27); pass 0 otherwise. Species 412 = the Egg.
 * Returns 1 on success, 0 on any bounds/read failure.
 * Convenience wrapper over rom_mon_locate + rom_mon_icon_at — a caller that reads
 * more than one frame or verifies by re-reading should use those directly. */
int rom_mon_icon(const RomMon* rm, uint16_t species, uint8_t form, uint8_t frame,
                 uint8_t dst[ROM_MON_ICON_BYTES], uint8_t* pal_index);

/* Read shared icon palette `pal_index` (0..2) as 16 raw RGB15 entries. */
int rom_mon_icon_pal(const RomMon* rm, int pal_index, uint16_t dst[16]);

#endif /* ROM_MON_H */
