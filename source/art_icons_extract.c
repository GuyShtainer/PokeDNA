/* art_icons_extract.c — see art_icons_extract.h. Pure C, no tonc/FatFs: every I/O goes
 * through the RomMon's RomReadFn callback, so this dual-compiles on the host
 * (tests/host_artextract_test.c, against Guy's real ROM dumps) and on the GBA build
 * unchanged, exactly like rom_mon.c / rom_sprite.c / rom_itemart.c already do. */
#include "art_icons_extract.h"

#include <string.h>

#include "art_cache.h" /* art_fnv1a */

void art_icons_gen_init(ArtIconsGen* g, const RomMon* rm, ArtIconsProgressFn progress,
                        void* progress_ctx) {
  memset(g, 0, sizeof *g);
  g->rm = rm;
  g->progress = progress;
  g->progress_ctx = progress_ctx;
  g->fnv = ART_FNV1A_INIT;
}

/* Little-endian byte writers, matching the rest of the codebase's explicit-endianness
 * convention (rom_mon.c's rd32le, art_cache.c's wr16/wr32). */
static void put_u16le(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static bool fill_row(const RomMon* rm, uint16_t row, uint8_t out[ART_ICONS_ROW_BYTES],
                     bool* rom_error) {
  RomMonLoc loc;
  int unstable = 0;
  /* The locate step (pointer + palette id, 5 bytes) is verified HERE, independently
   * of the frame payload's own verification (see the two-pass note below) -- the
   * documented subtle case (rom_mon.h) is a garbled pointer that still lands inside
   * the image and decodes IDENTICALLY every time, which a payload-only double-read
   * cannot catch because both reads would then hit the same wrong offset and agree.
   * rom_mon_locate_row_verified double-reads the tiny locate fields specifically to
   * close that gap, at a cost of 10 extra bytes per row. */
  if (!rom_mon_locate_row_verified(rm, row, &loc, 3, &unstable)) {
    if (rom_error) *rom_error = true;
    return false;
  }
  if (!rom_mon_icon_at(rm, &loc, 0, out) || !rom_mon_icon_at(rm, &loc, 1, out + 512)) {
    if (rom_error) *rom_error = true;
    return false;
  }
  return true;
}

static bool fill_tail(const RomMon* rm, uint8_t out[ART_ICONS_PAL_IDS_BYTES +
                                                    ART_ICONS_PALS * ART_ICONS_PAL_BYTES],
                      bool* rom_error) {
  /* gMonIconPaletteIndices is ART_ICONS_ROWS contiguous bytes -- one forward read. */
  if (!rm->rc->read(rm->rc->ctx, rm->pal_ids, out, ART_ICONS_PAL_IDS_BYTES)) {
    if (rom_error) *rom_error = true;
    return false;
  }
  uint8_t* p = out + ART_ICONS_PAL_IDS_BYTES;
  for (int i = 0; i < (int)ART_ICONS_PALS; i++) {
    uint16_t pal[16];
    if (!rom_mon_icon_pal(rm, i, pal)) {
      if (rom_error) *rom_error = true;
      return false;
    }
    for (int c = 0; c < 16; c++) put_u16le(p + c * 2, pal[c]);
    p += ART_ICONS_PAL_BYTES;
  }
  return true;
}

bool art_icons_stream(void* ctx, uint32_t off, uint8_t* dst, uint32_t want) {
  ArtIconsGen* g = (ArtIconsGen*)ctx;
  if (!g || !g->rm) return false;
  if (g->cancelled || g->rom_error) return false; /* a prior call already gave up */

  /* off resets to 0 exactly once per forward pass (write, then verify) -- see the
   * header comment. Pass 1 is the write pass: it drives progress, the cancel check,
   * and the running FNV over exactly the bytes that get written. Pass 2 re-derives
   * the SAME bytes fresh from the ROM a second time; sf_write_verified_stream
   * compares them against what pass 1 actually put on the card, which is the
   * read-twice-and-compare verification on the frame payload (DESIGN.md Sec 5 risk
   * 4) -- for zero extra ROM reads beyond what the streaming write already costs. */
  if (off == 0) g->pass++;

  int row_total = (int)ART_ICONS_ROWS;
  bool ok;
  int rows_done;

  if (off < ART_ICONS_TILES_BYTES) {
    /* Row region: the caller MUST drive this with chunk == ART_ICONS_ROW_BYTES, so
     * every call here is exactly one row, offset-aligned. Anything else is a caller
     * bug, not a data problem, and must fail loudly rather than mis-slice a row. */
    if (want != ART_ICONS_ROW_BYTES || (off % ART_ICONS_ROW_BYTES) != 0) return false;
    uint16_t row = (uint16_t)(off / ART_ICONS_ROW_BYTES);
    rows_done = row;
    ok = fill_row(g->rm, row, dst, &g->rom_error);
  } else {
    /* The tail: pal ids + palettes, always exactly one combined call (see the header
     * layout comment — the two regions total 536 B, less than one row's worth). */
    uint32_t tail_bytes =
        ART_ICONS_PAL_IDS_BYTES + ART_ICONS_PALS * ART_ICONS_PAL_BYTES;
    if (off != ART_ICONS_PAL_IDS_OFF || want != tail_bytes) return false;
    rows_done = row_total;
    ok = fill_tail(g->rm, dst, &g->rom_error);
  }
  if (!ok) return false;

  if (g->pass == 1) {
    g->fnv = art_fnv1a(g->fnv, dst, want);
    if (g->progress && !g->progress(g->progress_ctx, g->pass, rows_done, row_total)) {
      g->cancelled = true;
      return false;
    }
  } else if (g->progress) {
    /* Verify pass: progress is still reported (a UI may want to show "verifying"),
     * but a cancel here is exactly as safe as one during the write pass -- either
     * way sf_write_verified_stream discards the .tmp and never promotes it. */
    if (!g->progress(g->progress_ctx, g->pass, rows_done, row_total)) {
      g->cancelled = true;
      return false;
    }
  }
  return true;
}

uint32_t art_icons_gen_fnv(const ArtIconsGen* g) { return g->fnv; }
