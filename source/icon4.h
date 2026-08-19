#ifndef ICON4_H
#define ICON4_H

#include <stdint.h>
#include <stdbool.h>

/*
 * icon4_to_rgb15 — expand one 32x32 4bpp tonc-tiled icon frame (ROM_MON_ICON_BYTES ==
 * 512 B, rom_mon.h's own format) into RGB15 (0 = transparent, 0x8000|rgb = opaque),
 * the format mon_icon_for()'s callers (ui_icon_scaled, the Pokedex grid, the party
 * overlay, the pickers) already expect.
 *
 * A 32x32-scale analogue of rom_sprite.c's rom_sprite_to_rgb15 (DESIGN.md Sec 4.7),
 * reusing the SAME in-place staging trick for the same reason: no new stack or static
 * buffer. `buf` must hold ART_ICON_RGB15_BYTES (2048); the raw 512 B frame goes in the
 * FIRST 512 bytes, the de-tiled/expanded RGB15 picture comes out covering the WHOLE
 * 2048. The de-tiled staging copy lives in buf's own TAIL 512 bytes (buf[1536..2047])
 * while expansion writes forward from buf[0] — for every source index i<512 the write
 * at byte 4*i is always before the read at byte 1536+i (3i<1536 <=> i<512), so the
 * write can never catch up to a byte the read has not consumed yet.
 *
 * Pure C — no tonc/FatFs — so tests/host_icon4_test.c runs it on the host.
 */
#define ART_ICON_RGB15_BYTES 2048u

/* buf[0..511] in: raw ROM_MON_ICON_BYTES 4bpp frame. buf[0..2047] out: RGB15.
 * Returns false (leaving buf untouched) on a NULL arg or a short buffer. */
bool icon4_to_rgb15(uint8_t* buf, uint32_t cap, const uint16_t pal[16]);

#endif /* ICON4_H */
