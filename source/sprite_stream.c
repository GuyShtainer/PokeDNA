/* On-demand sprite streaming — see sprite_stream.h. Compiled to nothing unless the trimmed
 * SD build defines PDNA_STREAM_SPRITES (the NOR build embeds every sprite, so it needs none
 * of this and pays no EWRAM for the read buffer). */
#include "sprite_stream.h"

#ifdef PDNA_STREAM_SPRITES

#include <tonc.h>
#include "sys.h"      /* EWRAM_BSS (after tonc.h so the u8 macro doesn't clash) */
#include "ff.h"

#define PAK_PATH "/PokeDNA/sprites.pak"
#define CBUF_SZ  9216             /* one LZ77-compressed 64x64 sprite is <= ~8 KB; slack to spare */

static EWRAM_BSS uint8_t s_cbuf[CBUF_SZ];      /* compressed sprite lands here, then LZ77-expands to `out` */
static uint32_t          s_last_off = 0xFFFFFFFFu;
static const uint16_t*   s_last_out = 0;

const uint16_t* sprite_stream(uint32_t pak_off, uint16_t* out) {
  if (pak_off == 0xFFFFFFFFu || !out) return 0;
  if (pak_off == s_last_off && out == s_last_out) return out;   /* same sprite re-requested (per-frame anim) */
  FIL f;
  if (f_open(&f, PAK_PATH, FA_READ) != FR_OK) return 0;         /* no pack on the card -> caller falls back */
  const uint16_t* r = 0;
  if (f_lseek(&f, pak_off) == FR_OK) {
    UINT br = 0;
    if (f_read(&f, s_cbuf, CBUF_SZ, &br) == FR_OK && br >= 4) {
      LZ77UnCompWram(s_cbuf, out);                              /* header carries the size; trailing bytes ignored */
      s_last_off = pak_off; s_last_out = out; r = out;
    }
  }
  f_close(&f);
  return r;
}

#endif /* PDNA_STREAM_SPRITES */
