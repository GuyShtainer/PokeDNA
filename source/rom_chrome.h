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
 * BAG: IMPLEMENTED for Emerald + FireRed + LeafGreen. Ruby/Sapphire NOT
 * implemented, and this is now pinned down rather than estimated.
 *
 * The DESIGN.md inventory (Sec 1.4) had Emerald's screen TILESET as "not
 * located" and FireRed/LeafGreen with no tileset row at all — only a tilemap,
 * which cannot be rendered without one. All three were closed the way
 * DESIGN.md itself predicted ("cost to close: minutes, not hours"): the known
 * tilemap pointer is referenced from exactly one code site in every game
 * (found by scanning the ROM for that literal 32-bit pointer value), and the
 * bag screen's own asset-loading routine sits right there as a tight run of
 * LZ77 pointer literals — the tileset is the LZ10 blob immediately BEFORE the
 * tilemap in every one of the three ROMs, verified by decoding it and
 * compositing the result (docs/analysis-2026-08-20-artless-phases/bag-*.png):
 *   Emerald   tileset 0x08D9A620 (1,696 B / 53 tiles), tilemap 0x08D9A88C
 *             (known), palette MALE 0x08D9A588 / FEMALE 0x08D9A5D4 — each a
 *             COMPLETE, independent 32-colour (2-bank) blob, not a partial
 *             override; gender picks which one to fetch, no swap needed.
 *   FireRed   tileset 0x08E830CC (1,760 B / 55 tiles), tilemap 0x08E832C0
 *             (known — DESIGN.md's "bg.bin"), palette MALE 0x08E835B4 (96 B /
 *             3 banks) + FEMALE 0x08E83604 (32 B / 1 bank), a BANK-0-ONLY
 *             override, not a whole separate blob like Emerald's — see below.
 *   LeafGreen the FireRed set, shifted: tileset 0x08E8314C, tilemap 0x08E83340,
 *             palette MALE 0x08E83634 + FEMALE 0x08E83684. Same code shape at
 *             the same relative offsets, confirmed independently (not assumed
 *             from FireRed's addresses).
 * DESIGN.md's "bag.pal" / "bag_window_pal" table entries are NOT part of this
 * screen's background at all — they are referenced from unrelated code sites
 * (bag.pal's only reference sits beside rom_itemart.h's own FireRed item-icon
 * pin, i.e. it is item-icon plumbing, not bag chrome) and were ruled out by
 * that same code-reference check before being used.
 *
 * THE FEMALE ADDRESS WAS RIGHT; THE BANK WAS WRONG (fixed 2026-08-20). A
 * fourth address, 0x08E83604 (DESIGN.md's "bg_female.pal"), decodes cleanly
 * (LZ10, 32 B, 1 bank) and sits right next to the tileset/tilemap/palette
 * pointers in the SAME code — it IS this screen's female recolour. An earlier
 * pass wired it as a BANK-1 override (guessing from the trainer card's
 * tier+female_bg shape, CardPins above) and, on seeing a wrong colour
 * (orange where retail is blue), concluded the address itself was wrong and
 * dropped it — the reasoning stopped one hypothesis short: only the bank
 * index was wrong, not the address. tools/gen_bag_bg.py:118 (this project's
 * own art generator for the SAME screen) already applies this exact blob to
 * BANK 0, `female = load_jasc_pal(bg_female.pal) + male[16:]  # bank 0 only`,
 * citing pokefirered src/item_menu.c:574 — and a byte-compare confirms the
 * ROM's decoded 16 entries at 0x08E83604 are IDENTICAL to
 * assets/bag/frlg/bg_female.pal. Re-measured with Guy's own female
 * FireRed.sav (SaveBlock2+0x08 == 1) AND a synthetic male counterpart of the
 * SAME save (identical trainer/items, only that one byte + its section
 * checksum differ), against the compiled full-art build of each, with the
 * SAME game ROM fused into both sides so item text isn't a confound
 * (docs/analysis-2026-08-20-artless-phases/bagfix-*.png,
 * bagfix_pixeldiff.py): MALE matches the full-art capture at 0/38,400
 * differing pixels outside the animated bag-icon sprite (bag_anim() has no
 * ROM rung — see below — so that ~2,077 px region is expected and was
 * already out of scope before this fix). FEMALE matches at 0/38,400 in every
 * sampled clean interior region (15,836 px checked directly); the only
 * residual difference is 11 px at the extreme screen-edge corners, which sit
 * inside the SAME out-of-scope animated-sprite footprint (its outer
 * decorative trim) and are present because bag_bg.c's own GENERATED closed-
 * bag animation frames for FRLG-female are themselves visibly corrupted in
 * the full-art build (torn/striped, reproducible across repeated captures —
 * a pre-existing bug in tools/gen_bag_bg.py's animation compositing, not in
 * this file, and out of this track's scope; worth its own investigation).
 * LeafGreen measured identically (its bag screen is pixel-for-pixel the same
 * content as FireRed's, just at shifted ROM addresses). Lesson for the next
 * person to touch this: "a pointer decodes, sits in the right place, and
 * produces a plausible-looking image" is still not proof of which BANK it
 * targets — a pixel-diff against real output is required for that, and a
 * wrong-bank result does not itself prove the address is wrong.
 *
 * Every tilemap uses TWO palette banks (measured, not assumed): bank 0 for the
 * bulk of the screen, bank 1 for a small accent set that includes the desc
 * pane's fill colour (Emerald: 5 of 1,024 tilemap entries; FRLG: 180 of
 * 1,024). There is no second (bg_map) layer here — the bag has one tilemap, unlike the
 * card's front/back-over-bg composite — so RomChromeBag.src.bg_map is always 0.
 *
 * RUBY/SAPPHIRE REMAIN UNSUPPORTED, and not for a located-data reason this
 * time: DESIGN.md Sec 1.4 already fully locates Ruby's bag_screen.png tileset,
 * and it is a genuine, unavoidable budget miss — LZ10 decodes to exactly
 * 8,192 B, the WHOLE shared 8 KiB staging buffer (artbuf.h), leaving zero
 * bytes for its own tilemap or palette, before even considering anything else
 * that buffer must hold. Sapphire is not in DESIGN.md's bag table and was not
 * independently probed (Ruby's own number already rules the family out).
 *
 * THE SHARED-BUFFER HAZARD THIS MODULE DOES NOT REMOVE: bag chrome (this file)
 * and item icons (rom_itemart.c, Phase 1, already wired) target the SAME
 * mon_decomp buffer at the SAME offset 0 — app_item_icon() (pdna_main.c) always
 * decodes to `mon_decomp` with no parameter to relocate it, and that call is
 * out of this module's scope (rule: own rom_chrome.c, don't rewire item art).
 * So, unlike the trainer card and Pokeblock case (which share the screen with
 * no other decoder for their whole visit), a decoded RomChromeBag is invalidated
 * by the VERY NEXT app_item_icon() call — which pdna_bag.c's draw_desc() makes
 * on every selection change. bag_screen() therefore cannot decode once and hold
 * across the interactive loop the way card_bg()/pokeblock_bg() do; it must
 * fetch a fresh BgFrame immediately before EVERY bg_restore()/bg_blit_rect()
 * call, "decode fresh, blit immediately" (the same contract art_fallbacks.c:148
 * already names for app_item_icon/app_type_badge) applied at finer grain. See
 * pdna_bag.c's draw_header/draw_cursor/draw_list/draw_desc for the four call
 * sites that each now fetch their own BgFrame instead of reusing one captured
 * at screen entry — reusing one WAS the Pokeblock-case bug this codebase
 * already shipped once. The cost is a redundant tileset+tilemap+palette
 * re-decode on every cursor move and idle description auto-page (a few KB of
 * LZ77, no SD access when the source is a fused/cartridge-bus ROM) — accepted
 * because it is user-driven (a keypress or a 1.5 s idle tick), never per-frame.
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
 * layer — PokeblockPins has no bg field, RomChromeSrc.bg_map is set to 0). Bag
 * is the smallest of the three: Emerald 1,696 + 2,048 + 64 = 3,808 B (female
 * is a whole separate 64 B fetch, same shape, not additive); FireRed/LeafGreen
 * 1,760 + 2,048 + 96 = 3,904 B in scratch, PLUS a 32 B bank-0 female override
 * fetched to a 96 B STACK buffer (never scratch — same shape as the card's
 * female_bg override) when female, not additive to the scratch budget either.
 * All fit the 8,192 B buffer with more
 * than half spare, and — unlike the bag screen's INTERACTIVE LOOP (see the scope
 * note above) — the card and Pokeblock screens never share that buffer with any
 * OTHER concurrent decoder for their whole visit (this module deliberately does
 * not wire card_badge16() or pokeblock_flavor_icon()/pokeblock_hl(), so nothing
 * else touches the buffer while THEIR chrome tileset is resident there).
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
  int bag_style;           /* -1 = none; else the PkGame (1 EMERALD / 2 FRLG)
                             * this ROM's bag chrome renders as — bag_bg.h's own
                             * index. Ruby/Sapphire always -1 (see header note). */
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
  uint16_t     star_pal[16];   /* tools/gen_card_bg.py's star.pal: a FLAT 16-colour
                                 * palette, not a tilemap bank — drawn with
                                 * romchrome_blit_tiles(src.tiles, star_pal, ...) on
                                 * tile STAR_TILE (143 in every game's shared
                                 * tileset, all three families -- gen_card_bg.py's
                                 * own comment: "all three games' tilesets keep the
                                 * star here"), `tier` copies left-to-right from the
                                 * layout's star_xy. Zeroed (all-black, harmlessly
                                 * invisible) when female's fetch below never runs
                                 * or star.pal fails -- same fail-safe posture as
                                 * female_bg. */
} RomChromeCard;

int rom_chrome_card_load(const RomChrome* rch, int g, int back, int tier, int female,
                         uint8_t* scratch, uint32_t cap, RomChromeCard* out);

/* The card's STAR tile id within CardPins.tileset's shared 160-tile (128x80)
 * sheet -- tools/gen_card_bg.py's STAR_TILE, "all three games' tilesets keep
 * the star here". Already resident in `scratch` after rom_chrome_card_load(),
 * so drawing the star row costs nothing beyond the tiny star_pal fetch that
 * function already made -- no separate decode. */
#define ROM_CHROME_STAR_TILE 143

/* ---- card badges (8 gym-badge 16x16 icons, drawn per owned badge) -------- */

/* 1 iff rom_chrome_card_badges_load(game=g, ...) can succeed for THIS rom.
 * Emerald + Ruby only (the same two games the card front/back cover). */
int rom_chrome_card_badges_have(const RomChrome* rch, int g);

typedef struct {
  const uint8_t* tiles;      /* badges.png's own tileset, in scratch          */
  uint16_t       pal[16];    /* badges.png's own flat 16-colour palette       */
} RomChromeCardBadges;

/* Decode badges.png + its own palette into `scratch` (>= cap; worst case
 * 1,056 B: 1,024 B tileset + 32 B palette -- see rom_chrome.h's MEMORY note
 * for why this is a SEPARATE decode from rom_chrome_card_load(), never
 * folded into the same buffer). Returns 1, or 0 with *out untouched. */
int rom_chrome_card_badges_load(const RomChrome* rch, int g,
                                uint8_t* scratch, uint32_t cap, RomChromeCardBadges* out);

/* Badge `i` (0..7)'s four tile ids (TL, TR, BL, BR) into `ids[4]`, already
 * translated into `out->tiles`' own local numbering (Ruby's badges_map.bin
 * absolute VRAM ids, based at 164, are subtracted down to a local tile index
 * here -- the caller never sees the VRAM convention). Emerald: badge i is the
 * plain 2x2 block at {2i, 2i+1, 16+2i, 17+2i} (a 16-tile-wide strip). Ruby:
 * indirected through badges_map.bin (8 x 4 absolute u16 ids). Returns 1, or 0
 * (game not wired / i out of range) with `ids` untouched. */
int rom_chrome_card_badge_ids(const RomChrome* rch, int g, int i, int16_t ids[4]);

/* ---- card trainer photo (the gendered 64x64 portrait) -------------------- */

int rom_chrome_card_photo_have(const RomChrome* rch, int g);

typedef struct {
  const uint8_t* tiles;      /* 64 tiles (8x8 grid), the WHOLE 64x64 picture  */
  uint16_t       pal[16];    /* the photo's own flat 16-colour palette        */
} RomChromeCardPhoto;

/* Decode the gendered photo (brendan.png/red.png = male, may.png/leaf.png =
 * female) + its own palette into `scratch` (>= cap; worst case 2,080 B:
 * 2,048 B tileset + 32 B palette). Returns 1, or 0 with *out untouched. */
int rom_chrome_card_photo_load(const RomChrome* rch, int g, int female,
                               uint8_t* scratch, uint32_t cap, RomChromeCardPhoto* out);

typedef struct {
  RomChromeSrc src;
  LzBlob       as_lzblob;
  const uint8_t* device_tiles;   /* device.png: 64 tiles (8x8 grid), 64x64,
                                   * pointing INTO `scratch` right after the
                                   * screen's own tileset/tilemap/palette --
                                   * folded into the SAME decode+buffer
                                   * (fits: 3,328 + 2,080 = 5,408 B of 8,192 B) */
  uint16_t       device_pal[16]; /* device.png's own flat 16-colour palette   */
} RomChromePokeblock;

int rom_chrome_pokeblock_load(const RomChrome* rch, int g,
                              uint8_t* scratch, uint32_t cap, RomChromePokeblock* out);

/* Where the case device sprite sits on the 240x160 screen (gen_pokeblock_bg.py
 * DEVICE_XY, pokeemerald/pokeruby CreatePokeblockCaseSprite(56,64,0) minus the
 * 64x64 sprite's half-extent). Composite with romchrome_blit_tiles(
 * device_tiles, device_pal, NULL, 8, 8, ROM_CHROME_POKEBLOCK_DEVICE_X,
 * ROM_CHROME_POKEBLOCK_DEVICE_Y) right after the screen's own bg_restore(). */
#define ROM_CHROME_POKEBLOCK_DEVICE_X 24
#define ROM_CHROME_POKEBLOCK_DEVICE_Y 32

/* ---- bag ---------------------------------------------------------------- */

/* 1 iff rom_chrome_bag_load(game=g, ...) can succeed for THIS rom. g is
 * bag_bg()'s own convention (bag_bg.h): 0 = RS (never true here), 1 = Emerald,
 * 2 = FRLG (FireRed and LeafGreen both report g==2; rom_chrome_bag_load() picks
 * the right pin table from rch->rc->kind). */
int rom_chrome_bag_have(const RomChrome* rch, int g);

typedef struct {
  RomChromeSrc src;
  LzBlob       as_lzblob;
} RomChromeBag;

/* Decode the bag screen's tileset + tilemap + (gendered) palette into
 * `scratch` (>= cap bytes; worst case 3,904 B, the header comment's MEMORY
 * section) and fill *out ready to hand to a BgFrame as `.blob =
 * &out->as_lzblob, .off = 0, .sw = BAG_BG_W`. `female` selects the palette on
 * BOTH families: Emerald swaps in a whole separate 2-bank blob; FRLG (fixed
 * 2026-08-20, see the header comment's "THE FEMALE ADDRESS WAS RIGHT" section)
 * overrides bank 0 only with its own measured female blob (FireRed
 * 0x08E83604 / LeafGreen 0x08E83684). Passing `female` as 0 on FRLG renders
 * the male base, exactly like every other gendered rung in this module — it
 * does NOT ignore the flag. Returns 1, or 0 with *out untouched (caller keeps
 * the existing "blob == 0" fallback: the plain data-editor bag tab). NEVER
 * memoise the result across calls — see the header's shared-buffer hazard
 * note: it is invalidated by the very next item-icon decode. */
int rom_chrome_bag_load(const RomChrome* rch, int g, int female,
                        uint8_t* scratch, uint32_t cap, RomChromeBag* out);

/* ---- bag SPRITE (the drawn bag itself, gendered, one frame per pocket) --- *
 *
 * Emerald + FireRed + LeafGreen ONLY -- narrower than rom_chrome_bag_have()'s
 * game coverage, for a hard reason, not a located-data one: the ROM stores
 * every gender's whole animation sheet as ONE monolithic LZ10 blob (closed +
 * one open frame per pocket, all under a SINGLE header), and mr_lz77() can only
 * decode a blob whose caller-supplied capacity covers its FULL declared size --
 * there is no partial/seek decode (confirmed against the actual decoder,
 * source/map_render.c's mr_lz77(): `if (size > dst_cap) return 0;` gates on the
 * WHOLE blob before a single byte is written, and the loop always runs to
 * `size`). FireRed/LeafGreen's declared size is 8,192 B, which fits the shared
 * 8,192 B buffer ALONE (after the screen chrome's own decode has already been
 * consumed by that visit's bg_restore() and the buffer is free again -- see
 * pdna_bag.c's bag_rest()/pocket_anim(), which already call through bag_anim()
 * at exactly that point). Emerald's is 12,288 B -- bigger than the ENTIRE
 * shared buffer, for EVERY frame including the closed one, because getting
 * ANY frame means decoding the whole stream from byte 0. rom_chrome_bag_sprite_have()
 * reports this honestly: 1 for FireRed/LeafGreen, 0 for Emerald (and for every
 * game rom_chrome_bag_have() already refuses). Closing the Emerald gap needs
 * either a bigger scratch buffer or a decoder that can discard already-consumed
 * output while decoding forward (neither exists today) -- not a wiring gap. */
int rom_chrome_bag_sprite_have(const RomChrome* rch, int g);

typedef struct {
  const uint8_t* tiles;      /* frame_count x 64 tiles (8x8 grid each), in scratch */
  uint16_t       pal[16];    /* the sheet's own flat 16-colour palette        */
  uint8_t        frame_count;
} RomChromeBagSprite;

/* Decode the WHOLE gendered animation sheet (see above -- there is no way to
 * ask for less) + its own 32 B palette into `scratch` (>= cap; FireRed/
 * LeafGreen worst case 8,224 B). `out->tiles + frame*64*32` is frame `frame`'s
 * 64 tiles (8x8 grid); frame 0 = closed, frame 1..frame_count-1 = pocket p's
 * OPEN frame in bag_bg.h's own pocket_frame order (sAnims_Bag: PokeBalls/
 * Items/KeyItems). Composite with romchrome_blit_tiles(out->tiles + frame*2048,
 * out->pal, NULL, 8, 8, x, y). Returns 1, or 0 (game not FireRed/LeafGreen, no
 * ROM, or `cap` too small) with *out untouched -- caller keeps whatever it had
 * (bag_anim()'s existing "blob == 0 -> no animation, screen still works"). */
int rom_chrome_bag_sprite_load(const RomChrome* rch, int g, int female,
                               uint8_t* scratch, uint32_t cap, RomChromeBagSprite* out);

#endif /* ROM_CHROME_H */
