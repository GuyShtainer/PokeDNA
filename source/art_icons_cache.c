/* art_icons_cache.c — see art_icons_cache.h. */
#include "art_icons_cache.h"

#include <string.h>

#include "ff.h"

/* The ONLY static storage this cache mechanism spends: 440 + 96 + 64 + 1 = 601 B,
 * well under the ~1,232 B new-IWRAM-.bss threshold this build is known to crash on
 * (see the header comment for the measurement that caught the first, too-large cut
 * of this module). Shared by every caller — there is no per-consumer instance. */
static uint8_t s_pal_ids[ART_ICONS_ROWS];
static uint16_t s_pals[ART_ICONS_PALS][16];
static char s_meta_path[64];
static bool s_meta_loaded = false;

static uint16_t rd_u16le(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

void art_icons_meta_clear(void) {
  s_meta_loaded = false;
  s_meta_path[0] = 0;
}

/* Own frame (FatFs' FIL is ~500-600 B): noinline keeps it a single shallow stack
 * frame that is gone before the caller's own frame grows, the same discipline
 * savefile.c documents for its own FIL locals. */
__attribute__((noinline))
static bool load_meta(const char* path) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  bool ok = f_size(&f) == (FSIZE_t)ART_ICONS_TOTAL_BYTES;
  uint8_t tail[ART_ICONS_PAL_IDS_BYTES + ART_ICONS_PALS * ART_ICONS_PAL_BYTES];
  if (ok) {
    ok = f_lseek(&f, ART_ICONS_PAL_IDS_OFF) == FR_OK;
    if (ok) {
      UINT br = 0;
      ok = f_read(&f, tail, sizeof tail, &br) == FR_OK && br == sizeof tail;
    }
  }
  f_close(&f);
  if (!ok) return false;
  memcpy(s_pal_ids, tail, ART_ICONS_PAL_IDS_BYTES);
  const uint8_t* p = tail + ART_ICONS_PAL_IDS_BYTES;
  for (int i = 0; i < (int)ART_ICONS_PALS; i++) {
    for (int j = 0; j < 16; j++) s_pals[i][j] = rd_u16le(p + j * 2);
    p += ART_ICONS_PAL_BYTES;
  }
  return true;
}

bool art_icons_meta_load(const char* path) {
  if (s_meta_loaded && strncmp(s_meta_path, path, sizeof s_meta_path) == 0) return true;
  art_icons_meta_clear();
  if (!load_meta(path)) return false;
  strncpy(s_meta_path, path, sizeof s_meta_path - 1);
  s_meta_path[sizeof s_meta_path - 1] = 0;
  s_meta_loaded = true;
  return true;
}

__attribute__((noinline))
static bool read_frame(const char* path, uint16_t row, uint8_t frame, uint8_t out[512]) {
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) return false;
  uint32_t off = (uint32_t)row * ART_ICONS_ROW_BYTES + (uint32_t)frame * 512u;
  bool ok = f_lseek(&f, off) == FR_OK;
  if (ok) {
    UINT br = 0;
    ok = f_read(&f, out, 512, &br) == FR_OK && br == 512;
  }
  f_close(&f);
  return ok;
}

bool art_icons_read_frame(const char* path, uint16_t row, uint8_t frame, uint8_t out[512]) {
  if (!path || row >= ART_ICONS_ROWS || frame >= 2) return false;
  return read_frame(path, row, frame, out);
}

bool art_icons_meta_pal(const char* path, uint16_t row, uint16_t out[16]) {
  if (!art_icons_meta_load(path) || row >= ART_ICONS_ROWS) return false;
  uint8_t id = s_pal_ids[row];
  if (id >= ART_ICONS_PALS) return false;
  memcpy(out, s_pals[id], 32);
  return true;
}

bool art_icons_meta_pal_at(const char* path, int pal_index, uint16_t out[16]) {
  if (!art_icons_meta_load(path) || pal_index < 0 || pal_index >= (int)ART_ICONS_PALS)
    return false;
  memcpy(out, s_pals[pal_index], 32);
  return true;
}

uint8_t art_icons_meta_pal_id(const char* path, uint16_t row) {
  if (!art_icons_meta_load(path) || row >= ART_ICONS_ROWS) return 0xFF;
  return s_pal_ids[row];
}

uint16_t art_icons_row_for(uint16_t species, uint8_t form) {
  if (species == 201 && form >= 1 && form <= 27) return (uint16_t)(413 + form - 1);
  return species;
}
