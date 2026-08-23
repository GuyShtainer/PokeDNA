/* art_icons_cache.c — see art_icons_cache.h. Format knowledge only: no handle, no
 * cache, no static storage. */
#include "art_icons_cache.h"

#include <string.h>

static uint16_t rd_u16le(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

bool art_icons_meta_read_fp(FIL* fp, uint8_t pal_ids[ART_ICONS_ROWS],
                            uint16_t pals[ART_ICONS_PALS][16]) {
  if (!fp || !pal_ids || !pals) return false;
  /* The size check is the cheap half of "is this really our icons.bin": the strong
   * half (an FNV over the whole file against art.idx) belongs to art_session.c and has
   * already run this session. */
  if (f_size(fp) != (FSIZE_t)ART_ICONS_TOTAL_BYTES) return false;

  /* 536 B on the caller's stack for the length of this call only. Deliberately not a
   * static: this module holds nothing, and 536 B in one shallow frame is the same
   * order savefile.c already spends on its FIL locals. */
  uint8_t tail[ART_ICONS_PAL_IDS_BYTES + ART_ICONS_PALS * ART_ICONS_PAL_BYTES];
  if (f_lseek(fp, ART_ICONS_PAL_IDS_OFF) != FR_OK) return false;
  UINT br = 0;
  if (f_read(fp, tail, sizeof tail, &br) != FR_OK || br != sizeof tail) return false;

  /* Commit only after the whole tail is in hand, so a short read leaves the caller's
   * tables exactly as they were rather than half-updated. */
  memcpy(pal_ids, tail, ART_ICONS_PAL_IDS_BYTES);
  const uint8_t* p = tail + ART_ICONS_PAL_IDS_BYTES;
  for (unsigned i = 0; i < ART_ICONS_PALS; i++) {
    for (int j = 0; j < 16; j++) pals[i][j] = rd_u16le(p + j * 2);
    p += ART_ICONS_PAL_BYTES;
  }
  return true;
}

bool art_icons_read_rows_fp(FIL* fp, uint16_t first, uint16_t n, void* dst) {
  if (!fp || !dst || n == 0) return false;
  if ((uint32_t)first + n > ART_ICONS_ROWS) return false;
  uint32_t off = (uint32_t)first * ART_ICONS_ROW_BYTES;
  uint32_t len = (uint32_t)n * ART_ICONS_ROW_BYTES;
  /* Seek only when the pointer is not already there -- a sequential sweep (which is
   * what icon_store's sorted plan produces) then costs no seek at all. */
  if (off != fp->fptr && f_lseek(fp, off) != FR_OK) return false;
  UINT br = 0;
  if (f_read(fp, dst, (UINT)len, &br) != FR_OK || br != len) return false;
  return true;
}

uint16_t art_icons_row_for(uint16_t species, uint8_t form) {
  if (species == 201 && form >= 1 && form <= 27) return (uint16_t)(413 + form - 1);
  return species;
}
