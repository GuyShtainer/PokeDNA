#ifndef SPRITE_STREAM_H
#define SPRITE_STREAM_H
#include <stdint.h>

/* On-demand sprite streaming for the TRIMMED SD build (-DPDNA_STREAM_SPRITES).
 *
 * To fit the EZ-Flash OS-mode PSRAM budget (an SD-accessing homebrew must stay ~<=4 MB or
 * the SD->PSRAM loader hangs), the shiny + back sprite blobs are REMOVED from the ROM and
 * shipped as a companion file on the card at /PokeDNA/sprites.pak. When a shiny portrait or
 * a back-view is actually shown (rare), the accessor streams just that one LZ77-compressed
 * sprite from the pack and decompresses it. The Pokedex/box/party use ICONS (kept in ROM),
 * so nothing streams there and they stay instant.
 *
 * Stream the LZ77 sprite at byte offset `pak_off` in the pack into `out` (a caller EWRAM
 * buffer sized for the sprite). Returns `out`, or 0 on any SD/decode error (caller falls
 * back gracefully). A 1-entry cache skips the SD read when the SAME sprite is re-requested
 * (e.g. the summary portrait animation re-fetching every frame). Follows the OS-mode rule:
 * the read completes into EWRAM before the caller blits. No-op stub in the NOR build. */
const uint16_t* sprite_stream(uint32_t pak_off, uint16_t* out);

#endif /* SPRITE_STREAM_H */
