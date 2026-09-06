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

/*
 * artbuf_epoch — E3 review BLOCKING 2 fix: pdna_origin_art.c's fetch_pic() memoises
 * the LAST Game Boy picture it fetched (dex/form/back/shiny -> a pointer into
 * mon_decomp), and that memo used to trust its key alone. mon_decomp is shared by
 * EVERY decoder in this list, so a memo hit could hand back pixels a LATER decoder
 * already overwrote: box grid hover a GB cell (fetch, memo set) -> hover a native
 * cell (mon_front_for_form decodes into mon_decomp) -> hover the GB cell again ->
 * the memo's key still matches, so it returns the NATIVE mon's pixels read as if
 * they were the GB-sized picture. Bumping a shared counter on every write and
 * storing it alongside the memo turns that into a normal miss.
 *
 * THE RULE: any function that writes to mon_decomp calls artbuf_claim() FIRST,
 * unconditionally, even if the decode that follows might fail (a failed decode may
 * still have partially overwritten the buffer, so the epoch must already say "this
 * content is not what it was" before that happens — the exact reasoning
 * pdna_origin_art.c's own fetch_pic() already applies to ITS memo: "a fetch that
 * fails may still have written into the source's buffer, so the old pointer stops
 * being trustworthy the moment we ask"). Known writers (grep mon_decomp for more
 * before adding a new one): mon_front_for_form/mon_back_for_form (mon_front.c/
 * mon_back.c), rom_portrait (pdna_origin_art.c), app_type_badge/app_item_icon
 * (pdna_main.c), icon_from_cache (art_fallbacks.c), gb_art_source.c's own fetch,
 * the bag/trainer-card chrome decoders (pdna_bag.c, pdna_trainer.c), the box
 * wallpaper tile compare buffer (pdna_box.c) and rom_wallpaper.c.
 *
 * A caller that only ever READS mon_decomp (never decodes into it) does not touch
 * this. 4 bytes, plain (IWRAM) .data — see artbuf.c for why not EWRAM_BSS.
 */
extern uint32_t artbuf_epoch;
static inline void artbuf_claim(void) { artbuf_epoch++; }

#endif /* ARTBUF_INCLUDED */
