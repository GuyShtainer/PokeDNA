#ifndef ARTBUF_INCLUDED
#define ARTBUF_INCLUDED

#include <stdint.h>

/*
 * The ONE 8 KiB EWRAM staging buffer shared by every single-frame Gen-3 sprite
 * decoder in the app: mon_front_for/mon_back_for (compiled art, source/mon_front.c
 * + source/mon_back.c) and rom_sprite_pic/rom_sprite_to_rgb15 (ROM-streamed art,
 * source/rom_sprite.c, via pdna_origin_art.c's rom_portrait()). It is also the
 * scratch rom_itemart.h's type-badge sheet loader recommends (rom_itemart.h:145-149).
 *
 * DEFINED HERE, in artbuf.c, NOT in mon_front.c where it used to live -- mon_front.c
 * is one of the 21 generated art files the artless build moves aside
 * (docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 0(b) / Phase 0). An artless build
 * with no mon_decomp symbol at all makes every "recommended buffer: mon_decomp" note
 * in this codebase false, and rom_sprite_to_rgb15 -- which REQUIRES cap >= 8192
 * (rom_sprite.c:261) -- simply refuses every call, so the summary portrait can never
 * light up. artbuf.c is NOT part of the moved art set, so it is always compiled, in
 * both builds.
 *
 * Full build: this buffer already existed (defined in mon_front.c); moving it here
 * changes nothing about the full build's EWRAM total -- it is still exactly one
 * 8,192 B EWRAM_BSS symbol. Artless build: this is now EWRAM's ONLY static buffer
 * (a grep for EWRAM_BSS|EWRAM_DATA across the whole moved art set returns nothing
 * else), so it is the whole story of the artless build's EWRAM budget.
 */
#define MON_DECOMP_BYTES 8192      /* 64*64 pixels * 2 bytes == rom_sprite.h's
                                    * ROM_SPRITE_BUF_BYTES == gb_sprite_codec.h's
                                    * worst-case scratch need */
extern uint16_t mon_decomp[MON_DECOMP_BYTES / 2];

#endif /* ARTBUF_INCLUDED */
