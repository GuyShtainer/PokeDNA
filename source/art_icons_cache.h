#ifndef ART_ICONS_CACHE_H
#define ART_ICONS_CACHE_H

#include <stdint.h>
#include <stdbool.h>

#include "ff.h"                /* FIL -- this module reads through a handle it does
                                * not own; see below */
#include "art_icons_extract.h" /* ART_ICONS_* layout constants */

/*
 * The icons.bin CACHE READER — pure FORMAT knowledge, and nothing else.
 *
 * This module answers "where in icons.bin does row R live, and what is in the tail?".
 * It owns no file handle, no cache, and no static byte. The caller passes an
 * already-open FIL and an already-allocated destination; source/icon_store.c is the
 * only caller in the app, and it is the module that owns the handle, the link map, the
 * palettes and the row pool.
 *
 * WHY IT LOOKS LIKE THIS NOW (2026-08-23). The previous cut was path-based and
 * stateless in the worst way: every 512 B frame did its OWN f_open + f_lseek + f_read +
 * f_close. Measured over the real lib/fatfs on a RAM disk, with the file three
 * directory levels down, that is 5 disk_read calls to deliver 512 B of payload -- 20 %
 * efficiency -- and on a card whose ROOT DIRECTORY holds ~60 or ~120 entries it is 20
 * and 35, because f_open re-walks and re-matches every level with LFN comparison. So
 * 86-94 % of every icon access was re-finding a file that had been found a millisecond
 * earlier, and the two costs THRASH each other: f_open's directory walk evicts the FAT
 * sector from FatFs' single window, and the FAT walk then evicts the directory sector.
 * That is the structural reason the cost never amortised, and it is why Guy measured
 * the "fast" cache rung as no faster than the ROM rung it was built to replace.
 *
 * The header this replaces defended that design with an IWRAM measurement (a held-open
 * FIL PER CALLER measured 2,272 B of new IWRAM .bss and crashed the ~1,232 B boot
 * threshold this build has) and with the claim that "a box load is ~30 icons; the ROM
 * rung it replaces pays worse than this already". The first was true of that cut and is
 * now moot -- there is ONE shared handle and it lives in EWRAM_BSS (600 B), which is a
 * different linker region entirely. The second was simply INVERTED on real cards: the
 * ROM rung measures 17-20 sectors per icon and the cache rung measured 20-35.
 *
 * TWO FRAMES, ONE READ. A row is 1024 B: frame 0 then frame 1, adjacent, by
 * construction (art_icons_extract.h). Reading 1024 B instead of 512 B costs the SAME
 * one disk_read, the same one EZ-Flash chunk and the same 24 fixed halfword cart
 * writes -- only ~46 us of extra DMA. That is what makes a bob flip cost zero SD
 * transactions, and box_oam.c's comment has claimed this was already being done since
 * before it was true.
 *
 * CONSECUTIVE ROWS ARE CONSECUTIVE BYTES, so art_icons_read_rows_fp can pull a whole
 * span in one transfer. Under the Pokedex's default filter+sort, 12 of 18 full pages
 * are exactly one such span.
 *
 * Still dual-compiles against tests/hostfat's real lib/fatfs-over-a-RAM-disk, exactly
 * like source/savefile.c -- see tests/host_iconscache_test.c.
 */

/* Read the metadata tail (palette ids + the 3 shared palettes) off an already-open
 * handle, and check the file is exactly ART_ICONS_TOTAL_BYTES first. Both outputs are
 * caller-owned; nothing is retained here. One f_lseek + one f_read.
 * Returns false, having written nothing, on a wrong size or any I/O failure. */
bool art_icons_meta_read_fp(FIL* fp, uint8_t pal_ids[ART_ICONS_ROWS],
                            uint16_t pals[ART_ICONS_PALS][16]);

/* `n` CONSECUTIVE rows starting at `first`, ART_ICONS_ROW_BYTES each (both bob frames
 * per row), straight into `dst` -- ONE f_lseek + ONE f_read of n * 1024 B.
 *
 * Every offset it seeks to is a multiple of 512 by construction, which matters: an
 * aligned f_read skips FatFs' per-FIL sector buffer entirely and issues one
 * multi-sector disk_read straight into `dst`, while an UNALIGNED one is routed through
 * lib/fatfs/diskio.c's bounce buffer and capped at 4 sectors per driver call. `dst`
 * must therefore also be 4-byte aligned (diskio.c's DMA32 requirement).
 *
 * Refuses n == 0 and any span running past ART_ICONS_ROWS. Returns false on a short
 * read WITHOUT telling the caller how far it got -- a partial span is not usable,
 * because we do not know where the bytes stopped. */
bool art_icons_read_rows_fp(FIL* fp, uint16_t first, uint16_t n, void* dst);

/* species/form -> table row, the SAME mapping rom_mon.c's (private) table_species()
 * implements — species 201 + form 1..27 -> the Unown B..'?' rows (413..439), species
 * 412 IS the Egg row, everything else maps to itself. Kept in sync by
 * tests/host_iconscache_test.c cross-checking it against rom_mon's own table for
 * every row on a real ROM. */
uint16_t art_icons_row_for(uint16_t species, uint8_t form);

#endif /* ART_ICONS_CACHE_H */
