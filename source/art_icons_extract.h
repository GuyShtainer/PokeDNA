#ifndef ART_ICONS_EXTRACT_H
#define ART_ICONS_EXTRACT_H

#include <stdint.h>
#include <stdbool.h>

#include "rom_mon.h"
#include "savefile.h" /* SfStreamFn */

/*
 * icons.bin — the ROM's own icon table (rom_mon.h), mirrored byte-for-byte, plus the
 * two small side tables the loader needs to serve an icon WITHOUT re-opening the ROM
 * (DESIGN.md Sec 4.1: the cache "is the only [rung] that exists in an artless build
 * with no ROM registered this session"). Pure C, no tonc/FatFs — this is the
 * extractor's algorithmic core; tests/host_artextract_test.c runs it against Guy's
 * real ROM dumps exactly like tests/host_rommon_test.c does.
 *
 * LAYOUT (ART_ICONS_TOTAL_BYTES total):
 *   [0 .. 450560)     440 rows x 1024 B: frame 0 (512 B) then frame 1 (512 B), raw
 *                     4bpp tonc-tiled 32x32, in the ROM's own table-index order
 *                     (0..411 species ceiling, 412 Egg, 413..439 Unown B..'?').
 *   [450560 .. 451000) 440 B: gMonIconPaletteIndices, one byte per row, verbatim.
 *   [451000 .. 451096) 3 x 32 B: the 3 shared palettes, 16 raw RGB15 entries each.
 *
 * WHY THE PALETTE-INDEX TABLE IS HERE AT ALL — A GAP DESIGN.md LEFT UNBUDGETED.
 * DESIGN.md Sec 2.3 sizes icons.bin as "440 x 2 x 512 + 3 x 32 palettes" = 450,656 B
 * and never budgets the 440 B gMonIconPaletteIndices table. Without it, a session
 * with the cache present but NO ROM open (exactly the case Sec 4.1 says the cache
 * must serve — "the card outlives the registration") has no way to know which of the
 * 3 palettes species N uses, and the cache-first rung silently cannot work standalone.
 * This is flagged in the phase report as a found-and-filled design gap, not silently
 * absorbed: ART_ICONS_TOTAL_BYTES is 451,096 B, 440 B (0.1%) over the design's number.
 */
#define ART_ICONS_ROWS 440u        /* rom_mon.h's RM_TABLE_ENTRIES, asserted equal   */
#define ART_ICONS_ROW_BYTES 1024u  /* 2 frames x ROM_MON_ICON_BYTES                  */
#define ART_ICONS_PAL_IDS_BYTES ART_ICONS_ROWS
#define ART_ICONS_PALS 3u
#define ART_ICONS_PAL_BYTES 32u
#define ART_ICONS_TILES_BYTES (ART_ICONS_ROWS * ART_ICONS_ROW_BYTES)
#define ART_ICONS_PAL_IDS_OFF ART_ICONS_TILES_BYTES
#define ART_ICONS_PALS_OFF (ART_ICONS_PAL_IDS_OFF + ART_ICONS_PAL_IDS_BYTES)
#define ART_ICONS_TOTAL_BYTES (ART_ICONS_PALS_OFF + ART_ICONS_PALS * ART_ICONS_PAL_BYTES)

/* Called once per stream chunk (i.e. up to twice per row: once in the write pass,
 * once in the verify pass) so a UI can show progress. `rows_done` is how many full
 * rows the stream has produced bytes for so far IN THIS PASS (0..ART_ICONS_ROWS);
 * `pass` is 1 (extracting+writing) or 2 (re-deriving+verifying). Return false to
 * cancel — checked between rows, never mid-row, so a cancel can never leave a torn
 * row on either side of the comparison. */
typedef bool (*ArtIconsProgressFn)(void* ui_ctx, int pass, int rows_done, int rows_total);

typedef struct {
  const RomMon* rm;
  ArtIconsProgressFn progress;
  void* progress_ctx;
  int pass;         /* 0 before the first call; 1 during the write pass; 2 verify   */
  bool cancelled;   /* progress() returned false                                    */
  bool rom_error;   /* a ROM read/locate never verified                             */
  uint32_t fnv;      /* running FNV-1a, accumulated during pass 1 only               */
} ArtIconsGen;

void art_icons_gen_init(ArtIconsGen* g, const RomMon* rm, ArtIconsProgressFn progress,
                        void* progress_ctx);

/* The SfStreamFn to hand to sf_write_verified_stream (len == ART_ICONS_TOTAL_BYTES).
 * `g` (via ctx) must outlive the whole sf_write_verified_stream call. Requires `off`
 * to be called in the two-forward-pass order sf_write_verified_stream itself makes
 * (0, chunk, 2*chunk, ... twice over) -- it uses off==0 to detect the start of a new
 * pass, which is how it tells "the write pass's read" (drives progress + the running
 * FNV) from "the verify pass's read" (re-derives the same bytes fresh from ROM, which
 * IS the read-twice-and-compare check on the frame payload; see the .c file). */
bool art_icons_stream(void* ctx, uint32_t off, uint8_t* dst, uint32_t want);

/* Valid only after a successful sf_write_verified_stream call driven by this g (i.e.
 * after the write pass ran to completion once) -- the running FNV-1a over exactly the
 * bytes that were written, for the caller to put in the kind's ArtIdxKindRow. */
uint32_t art_icons_gen_fnv(const ArtIconsGen* g);

#endif /* ART_ICONS_EXTRACT_H */
