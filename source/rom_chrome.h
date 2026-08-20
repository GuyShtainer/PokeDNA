#ifndef ROM_CHROME_H
#define ROM_CHROME_H

#include <stdint.h>
#include "rom_map.h"    /* RomCtx / RomReadFn */
#include "lzblob.h"      /* RomChromeSrc, BgFrame */

/*
 * The TRAINER CARD and POKeBLOCK CASE full-screen backgrounds, read out of the
 * user's own ROM — the ROM rung for card_bg()/card_bg_back()/pokeblock_bg(),
 * DESIGN.md Sec 4.6 ("composite, don't cache frames"), Phases 5 and 7.
 *
 * Pure C, RomReadFn-based (rule 5): no tonc, no FatFs, no GBA headers, so
 * tests/host_romchrome_test.c runs this exact code against the five retail dumps.
 *
 * ============================================================================
 * SCOPE, STATED HONESTLY (this module does NOT cover everything DESIGN.md located)
 * ============================================================================
 *
 * TRAINER CARD: Emerald and Ruby only. DESIGN.md Sec 1.6 locates FireRed/LeafGreen
 * too, but only as ADDRESS RANGES for the per-tier/gender palettes ("sticker pals
 * x4, star, tier pals" at 0x083CD390..0x083CD3F0 / 0x083CD050..0x083CD290 for
 * FireRed; LeafGreen has NO palette address at all, only tileset/bg/front/back/
 * front_link). Emerald and Ruby's tier palettes are each FIVE INDIVIDUALLY PINNED
 * addresses — the only two games where every byte this module reads has its own
 * verified address, not an inferred stride through an unlabelled range. Sapphire
 * is not in DESIGN.md's card table at all (only Ruby). Extending to FireRed/
 * LeafGreen/Sapphire is real work, not a flag flip: it needs the same kind of
 * per-tier probe DESIGN.md Sec 1.10 costs at "minutes" for the two gaps it DID
 * find, applied to three more.
 *
 * POKeBLOCK CASE: Emerald only, exactly matching DESIGN.md's own coverage
 * (Sec 1.7): Ruby's frame.png/menu.bin were probed and did NOT byte-match the
 * staged reference art (19/1024 tilemap entries differ, per
 * tools/gen_pokeblock_bg.py's own docstring) — Ruby is not "not located", it is
 * "located but the address this module would need is not pinned yet". FRLG
 * correctly has no Pokeblocks at all (no src/pokeblock.c, no `pokeblocks` in
 * global.h) and rom_chrome_pokeblock_have() reports that honestly, matching
 * gen3_pokeblock.c's own pk_pokeblock_offset()==0 for FRLG.
 *
 * BAG: NOT IMPLEMENTED, on purpose, and this is the header's most important
 * finding. Ruby is the only game DESIGN.md locates completely (Emerald's own
 * screen TILESET is explicitly "not located"; FireRed/LeafGreen have no tileset
 * row in the table at all — only a tilemap, which cannot be rendered without one).
 * Even for Ruby, the numbers do not fit the budget: its bag_screen.png tileset
 * decompresses to 8,192 B — the WHOLE 8 KiB shared staging buffer (artbuf.h) —
 * with zero bytes left for the tilemap (2,048 B) or the palette (64 B) it needs
 * AT THE SAME TIME to composite one tile, let alone the item-icon buffer the bag
 * screen ALREADY shares that same 8 KiB with (rom_itemart.h Sec "RECOMMENDED
 * BUFFER"). Holding a resident chrome tileset across the WHOLE bag screen's
 * lifetime (required for cheap partial-rect restores, DESIGN.md Sec 4.6) is
 * fundamentally incompatible with also decoding an item icon into the same RAM on
 * demand mid-screen without one silently clobbering the other. Fixing this needs
 * either new EWRAM (the brief forbids it: "NEW EWRAM MUST BE ZERO") or an
 * on-demand per-tile LZ77 seek scheme (LZ77 cannot be randomly seeked — decoding
 * tile N means decoding tiles 0..N-1 first, which is too slow for a screen
 * redrawn on every cursor move). bag_bg() therefore keeps its EXISTING two-rung
 * behaviour (compiled art, else blob==0 / the plain data-editor bag tab)
 * unchanged; this file adds no bag code at all.
 *
 * ============================================================================
 * MEMORY
 * ============================================================================
 * Every decode writes into a CALLER-SUPPLIED buffer (rule: never a stack local
 * for a pixel-sized buffer) — pass a slice of the shared 8,192 B artbuf.h buffer,
 * the SAME one rom_sprite.c / rom_itemart.c already use. Nothing here defines a
 * static of its own. The card composites a THIRD tilemap underneath front/back
 * (bg.bin — the striped border retail draws behind the sticker, palette bank 1,
 * male/female via female_bg's bank-1 override; see lzblob.h's RomChromeSrc.bg_map
 * and rom_chrome.c's romchrome_blit index-0-transparent composite). Card's worst
 * case is now tileset + front/back tilemap + bg tilemap + palette: Emerald
 * 5,120 + 1,200 + 1,200 + 96 = 7,616 B; Ruby 5,120 + 1,280 + 1,280 + 96 =
 * 7,776 B. Pokeblock's is unchanged, 1,280 + 2,048 = 3,328 B (no background
 * layer — PokeblockPins has no bg field, RomChromeSrc.bg_map is set to 0). All
 * fit the 8,192 B buffer with room to spare, and — unlike the bag — neither
 * screen shares that buffer with any OTHER concurrent decoder (the trainer card
 * shows no item icons or mon sprites; this module deliberately does not wire
 * card_badge16() or pokeblock_flavor_icon()/pokeblock_hl(), so nothing else
 * touches the buffer while a chrome tileset is resident there).
 *
 * A decoded RomChromeSrc (lzblob.h) must stay alive exactly as long as its
 * BgFrame is used — the SAME rule rom_itemart.h's RomTypeSheet.scratch already
 * documents. NEVER memoise a decode across separate calls into this module: the
 * scratch buffer is shared with every other screen's decoder (box wallpaper,
 * mon icons, item icons, the summary portrait), so a key that happens to repeat
 * across a visit does not mean the bytes are still there — see pdna_trainer.c's
 * rom_card_frame() and pdna_main.c's rom_pokeblock_frame() for the two callers
 * that learned this the hard way and now always redecode. Re-decoding on every
 * full repaint (any face flip, tier/gender change, or screen re-entry) is
 * user-driven, not per-frame, so this costs the same class of thing the
 * compiled-art build pays in raw page-cache misses. */

typedef struct {
  const RomCtx* rc;
  int card_style;         /* -1 = none; else the PkGame (0 RS / 1 EMERALD) this
                             * ROM can render — see the header comment: only
                             * Emerald and Ruby are wired, and Ruby renders as
                             * PK_RS style (card_bg's own game index 0) */
  int pokeblock_ok;       /* 1 iff this ROM is Emerald (the only one wired)    */
  int verify;             /* fetch-twice-and-compare, default ON (rom_itemart's
                             * "silent-garbage hazard" posture) — see
                             * rom_chrome_set_verify() */
} RomChrome;

/* Identify what this ROM can serve. Always safe to call (cheap: a couple of
 * RomCtx.kind comparisons, no I/O) — call it once per ROM open, right beside
 * rom_sprite_open()/rom_itemart_open() in app_icon_rom_open(). */
void rom_chrome_open(RomChrome* rch, const RomCtx* rc);

/* Turn fetch-twice-and-compare verification off (fused / cartridge-bus source,
 * which needs none of it) or back on. Default ON — an SD read can return
 * RES_OK while holding garbage (DESIGN.md Sec 5 risk 4). */
void rom_chrome_set_verify(RomChrome* rch, int on);

/* 1 iff rom_chrome_card_load(game=g, ...) can succeed for THIS rom. g is card_bg's
 * own convention: 0 = RS, 1 = Emerald, 2 = FRLG (FRLG is always 0 here). */
int rom_chrome_card_have(const RomChrome* rch, int g);

/* 1 iff this ROM is Emerald (the only Pokeblock case this module serves). */
int rom_chrome_pokeblock_have(const RomChrome* rch, int g);

/*
 * Decode ONE card face's tileset + tilemap + tier/gender palette into `scratch`
 * (>= cap bytes; the header comment above states the worst case) and fill *out
 * ready to hand to a BgFrame as `.blob = &out->as_lzblob, .off = 0, .sw =
 * CARD_BG_W`. `back` selects front (0) or back (1) of the card; `tier` 0..4,
 * `female` 0/1. Returns 1, or 0 with *out untouched (caller keeps whatever rung
 * it already had — the existing "blob == 0" fallback).
 */
typedef struct {
  RomChromeSrc src;
  LzBlob       as_lzblob;   /* { 0,0,0,0, &src } — the BgFrame.blob to hand out */
} RomChromeCard;

int rom_chrome_card_load(const RomChrome* rch, int g, int back, int tier, int female,
                         uint8_t* scratch, uint32_t cap, RomChromeCard* out);

typedef struct {
  RomChromeSrc src;
  LzBlob       as_lzblob;
} RomChromePokeblock;

int rom_chrome_pokeblock_load(const RomChrome* rch, int g,
                              uint8_t* scratch, uint32_t cap, RomChromePokeblock* out);

#endif /* ROM_CHROME_H */
