#ifndef ART_ICONS_CACHE_H
#define ART_ICONS_CACHE_H

#include <stdint.h>
#include <stdbool.h>

#include "art_icons_extract.h" /* ART_ICONS_* layout constants */

/*
 * The icons.bin CACHE READER — the box grid's and the RGB15 consumers' (dex/party/
 * pickers) cache rung, ONE seek+read per icon frame instead of the ROM path's
 * per-icon table lookups scattered across a 12+ MB file (DESIGN.md Sec 4.1/4.7).
 *
 * Uses the real FatFs, so it is NOT pure-C the way art_icons_extract.c is — but it
 * dual-compiles against tests/hostfat's real lib/fatfs-over-a-RAM-disk exactly like
 * source/savefile.c already does.
 *
 * MEMORY SHAPE, AND WHY IT LOOKS LIKE THIS. The first cut of this module held a
 * session-long FIL (~600 B) PER caller (box_oam.c and the RGB15 wrapper each had
 * their own) plus 536 B of preloaded metadata each — 1,136 B x 2 = 2,272 B of new
 * STATIC storage, and arm-none-eabi-nm confirmed both landed in IWRAM .bss (0x0300xxxx),
 * not EWRAM. That is comfortably past the ~1,232 B of new IWRAM .bss this build is
 * KNOWN to crash on at boot (docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 5 risk 2,
 * and rule 7 of this phase's brief) — caught by actually measuring symbol sizes on the
 * built ELF before shipping it, not by assuming a "small struct" was small enough.
 *
 * The fix: NO session-held FIL anywhere. Every frame read opens, seeks, reads, and
 * closes its own FIL as a STACK-local (noinline, one shallow frame, the same order of
 * magnitude savefile.c already spends on FIL locals throughout this codebase) — zero
 * persistent cost, traded for one extra directory lookup per icon versus a held-open
 * file (a box load is ~30 icons; the ROM rung it replaces pays worse than this
 * already). The only STATIC storage left is the 536 B palette-id/palette-colour
 * metadata block, held ONCE (module-level, shared by every caller — box_oam.c and
 * the RGB15 wrapper both call the SAME functions below, there is no per-caller
 * instance anymore), loaded lazily on first use and cleared when the session
 * resolver's verdict flips to not-ready. 536 B is well under the 1,232 B threshold,
 * and it is the ONLY new static byte this whole cache mechanism spends.
 */

/* Load the 536 B metadata (pal ids + 3 palettes) from `path`'s tail, if not already
 * loaded for this path. Idempotent — a second call for the SAME path while already
 * loaded is a no-op; a call for a DIFFERENT path reloads. Callers do not need to call
 * this explicitly: art_icons_read_frame / art_icons_meta_pal* do it lazily. Exposed
 * so a caller (box_oam's palette upload) can force it once up front and check the
 * result. Returns false (leaving any prior metadata cleared) on any I/O failure. */
bool art_icons_meta_load(const char* path);
/* Discard the metadata — call when the session resolver's verdict flips to
 * not-ready, so a stale palette table can never be served. */
void art_icons_meta_clear(void);

/* Row `row` (0..ART_ICONS_ROWS-1, the SAME table-index axis rom_mon.h's RomMonLoc and
 * art_icons_extract.h use), ONE frame (0 or 1), 512 B. Self-contained: opens `path`,
 * seeks to the frame's exact byte offset, reads 512 B, closes. */
bool art_icons_read_frame(const char* path, uint16_t row, uint8_t frame, uint8_t out[512]);

/* Row `row`'s palette (16 RGB15 entries), from the preloaded metadata — no I/O.
 * Lazily loads the metadata from `path` first if it is not already loaded. */
bool art_icons_meta_pal(const char* path, uint16_t row, uint16_t out[16]);
/* Palette `pal_index` (0..ART_ICONS_PALS-1) directly — for a caller uploading all of
 * the cache's shared palettes up front (box_oam.c's OBJ bank upload). No I/O beyond
 * the lazy metadata load. */
bool art_icons_meta_pal_at(const char* path, int pal_index, uint16_t out[16]);
/* Row `row`'s palette id (0..ART_ICONS_PALS-1), or 0xFF on failure — the OBJ bank
 * index box_oam.c's cache rung needs directly, without expanding to colours. */
uint8_t art_icons_meta_pal_id(const char* path, uint16_t row);

/* species/form -> table row, the SAME mapping rom_mon.c's (private) table_species()
 * implements — species 201 + form 1..27 -> the Unown B..'?' rows (413..439), species
 * 412 IS the Egg row, everything else maps to itself. Kept in sync by
 * tests/host_iconscache_test.c cross-checking it against rom_mon's own table for
 * every row on a real ROM. */
uint16_t art_icons_row_for(uint16_t species, uint8_t form);

#endif /* ART_ICONS_CACHE_H */
