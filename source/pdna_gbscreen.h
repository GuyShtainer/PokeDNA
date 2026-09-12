#ifndef PDNA_GBSCREEN_H
#define PDNA_GBSCREEN_H

#include <stdint.h>
#include <stdbool.h>

#include "rom_gbui.h"
#include "gb_art_source.h"   /* GB_ROM_PATH_MAX -- pure (stdint/stdbool only) */

/*
 * pdna_gbscreen -- the SHARED GB-screen shell (U2a, docs/GB-GAME-SCREENS-DESIGN.md
 * sec 1.5/3.1-3.5). Owns the 20x18 tile canvas, the ROM UI-graphics source
 * (rom_gbui.h), the SELECT 1:1<->stretched toggle, the border/legend, and the
 * dirty-cell repaint. A screen (the U2b trainer card, later the bag/map) only
 * fills the tilemap through gbscr_cell()/gbscr_text()/gbscr_raw() and calls
 * gbscr_flush() -- it never touches VRAM or the ROM source directly.
 *
 * PURE-C BOUNDARY: this header and the tilemap-filling half of pdna_gbscreen.c
 * (gbscr_cell/gbscr_text/gbscr_raw, the charmap glyph-vs-blank decision, and the
 * two stretch LUTs) touch no tonc/FatFs -- tests/host_gbscreen_test.c compiles and
 * runs that half on the PC (it links source/gb_edit.c for gb_char_encode() and
 * source/rom_gbui.c for the RomGbUi type, same as every other pure host test in
 * this tree). Only gbscr_open()/gbscr_close()/gbscr_flush() (the ROM-file I/O and
 * the VRAM blit) need tonc/FatFs, and they live in the SAME .c file behind their
 * own #include block -- see pdna_gbscreen.c's own top-of-file note.
 *
 * MEMORY: GbScreen itself carries no FIL/FusedGbSlice (that would force this
 * header to pull in ff.h and make the pure host test impossible) -- it stores
 * only plain data (the located RomGbUi, the tilemap, and -- U2b item 1 -- the
 * GbscrCache table describing where in a CALLER-OWNED `tail` buffer each
 * cached block lives). It no longer stores a ROM path or fused base/size at
 * all: since item 1, the FIL/fused slice is opened ONCE inside gbscr_open()
 * (to bulk-copy FONT + need_mask's blocks into `tail`) and never reopened --
 * gbscr_flush() reads the tail buffer only. gbscr_flush() rebinds a LOCAL
 * RomGbUi copy's `ctx`/`read` to `&gs->cache` + gbscr_mem_read() for the
 * duration of that one call, exactly the way rom_gbui_tile()/glyph()'s own
 * caller-owned-ctx contract expects -- but that ctx is now RAM, not the SD
 * card. Nothing here is EWRAM_BSS: GbScreen is meant to live on ONE caller's
 * own stack frame (a `noinline` screen/demo function), the same "one noinline
 * frame" posture gb_art_source.c's gb_art_open_and_identify() and
 * gb_art_fetch() already use; `tail` itself is a SEPARATE caller-owned buffer
 * (gb12_arena_tail() inside a GB session, or the caller's own
 * app_arena_acquire() standalone -- see gbscr_open()'s own doc comment).
 *
 * MEASURED (arm-none-eabi-gcc -mcpu=arm7tdmi -mtune=arm7tdmi -O2 -mthumb-interwork
 * -mthumb -fstack-usage, 2026-09-09, U2c 2nd re-verify D3 fix): the SD/artless
 * and delta builds compile pdna_gbscreen.c/pdna_gbtrainer.c under DIFFERENT
 * flags (artless: -DPDNA_STREAM_SPRITES + the PDNA_ARTLESS art gates, real
 * FIL-backed I/O in gbscr_open_inner; delta: -DPDNA_DELTA, fused-save reads,
 * no FIL at all) and that changes several frames enough to need BOTH numbers,
 * not one shared figure -- the table below replaces the old single-column
 * write-up (which was already stale: it undercounted GbScreen and quoted a
 * since-superseded PDNA_GB_UI_NEED derivation).
 *
 *   own-frame size (bytes)      SD/artless   delta
 *   gbscr_open                  40           40
 *   gbscr_open_inner            1,656        128     (SD keeps the FIL +
 *                                                      f_open/f_read locals;
 *                                                      delta reads straight
 *                                                      out of the fused slice)
 *   gbscr_close                 8            8
 *   gbscr_flush                 8            8
 *   gbscr_flush.part.0          448          448     (gcc splits the real
 *                                                      body out; identical --
 *                                                      no FIL on this path in
 *                                                      either build)
 *   gbscr_run_demo               1,008        1,008   (unchanged by D1/D2 --
 *                                                      no gbscr_set_legend()
 *                                                      call, see its own doc)
 *   gbscr_decode_pic_gen1        768          56      (SD's tile-cache locals
 *                                                      vs delta's direct
 *                                                      fused-slice read)
 *   pdna_gbtrainer_gen1_card     1,056        1,056   (DOWN from a pre-D2
 *                                                      1,144: D2 removed the
 *                                                      local `char hdr[96]` +
 *                                                      its sniprintf() call --
 *                                                      the header/reason split
 *                                                      is now two `const
 *                                                      char*` args, no local
 *                                                      buffer at all)
 *   pdna_gbtrainer               472          472     (unchanged by D1/D2)
 *   pdna_gbtrainer_plain         120          120     (gbtr_plain_render
 *                                                      inlines into this; the
 *                                                      D2 header2/row_y0 locals
 *                                                      add 2 ints, no measurable
 *                                                      change at this frame)
 *
 * Every other pure-half function (gbscr_cell 16, gbscr_text 56, gbscr_raw 56,
 * gbscr_mark_all_dirty 8, gbscr_toggle_scale 8, gbscr_mem_read 40,
 * gbscr_cache_plan 32, gbscr_tail_need 20, gbscr_pack_pic 88,
 * gbscr_unpack_pic_px 8, gbscr_cell_rect 16, gbscr_persist_mode 8,
 * gbscr_set_legend 0) is identical between the two builds and stays under
 * 90 B -- not gated, not worth its own table row.
 *
 * sizeof(GbScreen) = 964 B (RomGbUi 120 + GbscrCache 60 + map 360 + src 360 +
 * dirty 45 + gen/ok/scale_dirty 3, no padding before legend[4] since the
 * struct is already 4-byte aligned at that offset + legend[4] 16 B), SAME for
 * the SD and delta builds (no #ifdef PDNA_DELTA branch in the struct) --
 * measured directly (arm-none-eabi-nm -S on a probe translation unit), not
 * hand-added, and it grew by 60 B from the previously documented 904: the D1
 * fix's `legend[4]` field (this header, above) is the whole delta. GbScreen
 * itself lives on the CALLER's own frame (gbscr_run_demo's 1,008 B above, or
 * a real card screen's), OUTSIDE the gate; it is never counted in
 * PDNA_GB_UI_NEED.
 *
 * D1 fix (U2a review): the stack-room gate used to live INSIDE gbscr_open()'s
 * own frame, so it measured the room LEFT UNDER a frame that already existed
 * by the time the check ran, instead of the room the frame ITSELF needs --
 * this always under-counted by exactly the frame's size and made the gate
 * refuse every time on real hardware. The old body is now `gbscr_open_inner()`
 * (static, noinline); `gbscr_open()` is a thin (40-B) wrapper that checks
 * pdna_origin_art_stack_room(PDNA_GB_UI_NEED) BEFORE calling it, so the gate
 * now runs before gbscr_open_inner()'s frame is ever allocated. PDNA_GB_
 * UI_NEED (rom_gbui.h) is derived from the SD/artless numbers above (the
 * TIGHT build -- delta's smaller gbscr_open_inner/gbscr_decode_pic_gen1
 * frames are never the binding case, so the gate is sized for the build that
 * actually needs it): rom_gbui.h's own comment currently states 5,400 as the
 * measured total (gbscr_open_inner's SD frame + rom_gbui_open_loc's own
 * worst chain + the indirect SD-read leg, taken as measured, not rounded up
 * -- see that header's own note; unaffected by this slice, since neither D1
 * nor D2 touches rom_gbui.c or gbscr_open_inner's own body).
 *
 * Nav-chain margin (U2c's real call path into this gate, freshly re-walked
 * this pass with -fstack-usage on pdna_gen12.c/pdna_gbtrainer.c, SD/artless
 * build): pdna_gen12_show_image 80 -> gb_session_core.part.0 160
 * (gb_nav_from_start inlines into it) -> pdna_gbtrainer 472 ->
 * pdna_gbtrainer_gen1_card 1,056 -> gbscr_open 40 = 1,808 B from that entry
 * point down to the gate call. The OUTER frames above pdna_gen12_show_image
 * (pdna_main.c's own
 * nav-menu dispatch chain) were NOT re-measured this pass -- pdna_main.c is
 * outside this slice's touched files and is 446 KB, so re-walking it end to
 * end is its own task, not a one-line recompute; a prior pass had put that
 * outer chain at roughly 2,368 B, which this pass did not re-verify and is
 * NOT asserted here. What IS verified: D2's hdr[96]+sniprintf removal made
 * pdna_gbtrainer_gen1_card's own frame 88 B SMALLER (1,144 -> 1,056) than
 * whatever number an earlier pass measured, so this slice can only have
 * INCREASED the real margin at the gate, never shrunk it -- D1 added no new
 * per-call stack (kGbscrLegendKeys/kGbscrBaseActions are `static const`, data
 * segment, not stack) and D2's `header2`/`row_y0`/`hline_y` are a handful of
 * extra int locals inside pdna_gbtrainer_plain (120 B total, unaffected per
 * the table above) -- neither touches the gen1_card/gbscr_open chain in the
 * direction that would matter (growing it).
 * gbscr_cache_block()'s own chain (16 + its GbReadFn, <=24 B) runs AFTER
 * rom_gbui_open_loc() returns, never nested inside it, so it does not add to
 * the gate either.
 * gbscr_flush() carries no separate gate -- it never calls
 * pdna_origin_art_stack_room() -- because its own reachable chain
 * (gbscr_flush.part.0's 448 B, plus rom_gbui_tile()/glyph()'s own small
 * per-tile-fetch frames and gbscr_mem_read()'s 40 B, no locate()/
 * distinct_tiles() on that path, no SD/FIL access at all any more) is
 * comfortably smaller and the design (sec 3.4/R9) only requires gating the
 * OPEN path.
 */

/* Canvas geometry -- 20x18 tiles, the whole GB screen. */
#define GBSCR_COLS 20
#define GBSCR_ROWS 18
#define GBSCR_CELLS (GBSCR_COLS * GBSCR_ROWS)          /* 360 */
#define GBSCR_DIRTY_BYTES ((GBSCR_CELLS + 7) / 8)      /* 45 -- one bit per cell */

/* Which located ROM block a cell's tile index refers to. BLANK cells are filled
 * with a flat colour and never touch rom_gbui_tile()/glyph() at all -- this is
 * also what a space character (charmap 0x7F) becomes, per the design's own rule
 * ("space is a blank cell, not a font tile"). FONT cells store the GAME's own
 * charmap byte directly (rom_gbui_glyph() takes that byte, not a raw tile index);
 * every other src stores a raw tile index into that block. */
typedef enum {
  GBSCR_SRC_BLANK = 0,
  GBSCR_SRC_FONT,
  GBSCR_SRC_TEXTBOX,
  GBSCR_SRC_CARDFRAME,
  GBSCR_SRC_BADGES,
  /* U3: Gen-2's own trainer-card blocks -- located by rom_gbui already (see
   * RomGbUi.fontextra/leaders/cardgfx/cardpic_m/cardpic_f), just never
   * plumbed into the tail cache/gbscr_cell() pipeline until now. Each is a
   * PLAIN rom_gbui_tile() block like BADGES -- no special codec -- so they
   * slot into the SAME cache-plan/mem-read machinery as every other
   * GbScrSrc; CARDPIC_M/CARDPIC_F are split (rather than one gender-aware
   * src) because gbscr_block_off() takes no gender flag and a single open()
   * only ever needs the ONE the save's own gender picks. */
  GBSCR_SRC_FONTEXTRA,
  GBSCR_SRC_LEADERS,
  GBSCR_SRC_CARDGFX,
  GBSCR_SRC_CARDPIC_M,
  GBSCR_SRC_CARDPIC_F,
  /* U3 D1 (found by BYTE-SEARCHING the real ROM, not guessing): the border
   * notch, the page-1 divider, "ID"/"No", the "STATUS" word, and the
   * play-time colon are ELEVEN CONSECUTIVE tiles immediately before LEADERS
   * in ROM -- confirmed by capturing each tile's own real VRAM pattern
   * bytes from a live Gold.gbc session and grepping those exact 16-byte
   * runs in the ROM file: every one landed at `leaders_addr - (11-i)*16`
   * for its own index i, none of them in FontExtra or TrainerCardGFX (both
   * tried first, both painted wrong content -- FontExtra is a plain
   * sequential glyph set starting 'A','B','C'..., unrelated). Offset is
   * DERIVED from `local->leaders`/`gu->leaders` (not one of RomGbUi's own
   * located fields) since the whole run sits immediately before it: names
   * this the Gen-2 "card misc" block. Index roles (0-10): 0=border notch,
   * 1=divider fill, 2=divider cap, 3="ID", 4="No", 5-9=the 5 "STATUS"-word
   * tiles, 10=the play-time colon. */
  GBSCR_SRC_STATUSWORD,
  /* U5 (BACKLOG #67, Gen 2's OWN Pack): both derived from RomGbUi.pack_m the
   * SAME way STATUSWORD is derived from .leaders above (a byte-search-verified
   * offset, not a scan job of its own -- see pdna_gbscreen.c's own comment on
   * gbscr_block_off()'s GBSCR_SRC_PACKMENU case). PACKMENU is the static
   * background/border/pocket-label art (pack_menu.2bpp, 80 tiles, sits exactly
   * 1,280 B -- 80 tiles * 16 B/tile -- BEFORE pack_m in ROM on both Gold and
   * Crystal, confirmed by a direct VRAM-pixel-bytes-vs-ROM-bytes search, not
   * assumed); PACK_M is the per-pocket picture block itself (PackGFX, already
   * located, 60 tiles = 4 pockets * 15 tiles, ROM order KEY/ITEMS/TM-HM/BALLS
   * per docs/GB-GAME-SCREENS-DESIGN.md sec 1.4 -- gbscr_cell()'s own `tile`
   * argument for a PACK_M cell is `pocket_rom_index*15 + local_tile(0..14)`,
   * the caller's job, not this src's). PACK_F (U5 D-Kris fix, review-opus
   * ac9ffc0): Kris's own PackFGFX, RomGbUi.pack_f, already located by
   * rom_gbui.c (Crystal only, cross-checked against the SAME anchor as
   * pack_m) -- wired exactly the pure-additive way this comment always said
   * it would be: one more GbScrSrc + need bit, same 60-tile/16-B-per-tile
   * shape as PACK_M, mutually exclusive with it at any one gbscr_open() (a
   * Crystal save's own gender picks ONE, same duality CARDPIC_M/CARDPIC_F
   * already established for the trainer card two srcs up). The caller (Kris's
   * OT gender byte) decides which need bit to ask for -- this src never
   * inspects the save itself. */
  GBSCR_SRC_PACKMENU,
  GBSCR_SRC_PACK_M,
  GBSCR_SRC_PACK_F,
  GBSCR_SRC_PIC,
  /* BACKLOG #91 M1: the Gen-1 MAP screen's own tile cache -- raw GB planar
   * 2bpp tiles (16 B/tile, the SAME format rgm1_tile2bpp() reads straight
   * out of the ROM, no repacking), resolved by rom_gbmap.c (a SEPARATE
   * locator from rom_gbui.c -- a map's tile graphics are not a fixed
   * RomGbUi block, they change with the player's current map) and copied by
   * the caller into gs->cache.maptiles BEFORE the first gbscr_cell() call
   * that references this src. Not one of `blocks[]` (never served through
   * gbscr_mem_read()/a rom_off match) -- same "separate cache.pic pointer"
   * shape GBSCR_SRC_PIC already established for a non-rom_gbui source. */
  GBSCR_SRC_MAPTILES,
  /* BACKLOG #125: Crystal's TrainerCard_InitBorder overwrites the card-pic
   * block's own storage tile 4 (used by BOTH right-corner cells) with
   * CardRightCornerGFX after GetCardPic -- a separate 16-B block, RomGbUi.
   * cardcorner (0 on Gold, where storage tile 4 IS the corner already).
   * Single-tile block (index always 0), same plain rom_gbui_tile() shape as
   * every other src here. */
  GBSCR_SRC_CARDCORNER,
  /* Sentinel, always last (append-only enum) -- NOT a real src, never used as a
   * cell's own GbScrSrc. Exists only so the _Static_assert below has something
   * to check: every real GBSCR_SRC_* value's ordinal is used as a `1u <<`
   * shift into `need_mask`, so the highest one must stay < 32 for a uint32_t
   * mask (BACKLOG #125 review: GBSCR_SRC_CARDCORNER was ordinal 16, 1u<<16
   * silently truncated to 0 in the uint16_t need_mask that used to be here --
   * fixed by widening need_mask/gbscr_tail_need/gbscr_cache_plan/gbscr_open to
   * uint32_t everywhere, this assert is the guard against it recurring). */
  GBSCR_SRC_COUNT
} GbScrSrc;

_Static_assert(GBSCR_SRC_COUNT <= 32,
              "GbScrSrc has grown past 32 values -- a GBSCR_NEED_* bit would "
              "silently truncate to 0 in a uint32_t need_mask; either shrink "
              "the enum or widen need_mask (and every gbscr_tail_need/"
              "gbscr_cache_plan/gbscr_open signature) again");

/* U2b item 1: which extra located ROM blocks (beyond FONT, always cached) a screen
 * wants copied into the tail's RAM tile bank at open -- a bitwise-OR of these,
 * passed as gbscr_open()'s `need_mask`. PIC has no bit here: the Gen-1 player pic
 * is not a RomGbUi block at all (it is gb_sprite_gen1()'s own compressed codec);
 * U2b's own player-pic wiring owns a further slice of the SAME tail buffer, sized
 * by the caller on top of what need_mask asks gbscr_open() to reserve. */
#define GBSCR_NEED_TEXTBOX   (1u << GBSCR_SRC_TEXTBOX)
#define GBSCR_NEED_CARDFRAME (1u << GBSCR_SRC_CARDFRAME)
/* U3: Gen-2's own card, added alongside GBSCR_NEED_BADGES below. */
#define GBSCR_NEED_FONTEXTRA (1u << GBSCR_SRC_FONTEXTRA)
#define GBSCR_NEED_LEADERS   (1u << GBSCR_SRC_LEADERS)
#define GBSCR_NEED_CARDGFX   (1u << GBSCR_SRC_CARDGFX)
#define GBSCR_NEED_CARDPIC_M (1u << GBSCR_SRC_CARDPIC_M)
#define GBSCR_NEED_CARDPIC_F (1u << GBSCR_SRC_CARDPIC_F)
#define GBSCR_NEED_STATUSWORD (1u << GBSCR_SRC_STATUSWORD)
/* U5: both required together by pdna_gbpack.c (BACKLOG #67, Gen 2's own Pack) --
 * see the GBSCR_SRC_PACKMENU/PACK_M enum comment above for what each holds. */
#define GBSCR_NEED_PACKMENU  (1u << GBSCR_SRC_PACKMENU)
#define GBSCR_NEED_PACK      (1u << GBSCR_SRC_PACK_M)
/* U5 D-Kris: Crystal's own female pack picture, requested INSTEAD of
 * GBSCR_NEED_PACK for a female save -- see the GBSCR_SRC_PACK_F enum comment. */
#define GBSCR_NEED_PACK_F    (1u << GBSCR_SRC_PACK_F)

/* U2c: the Gen-1 player pic, gb_sprite_gen1's own 7x7-tile (56x56 px) decode,
 * packed into OUR OWN 2-bit-per-pixel format (16 B/tile, NOT the ROM's planar
 * 2bpp layout -- see gbscr_decode_pic_gen1()'s own comment) so 49 tiles fit
 * 784 B. GBSCR_PIC_DECODE_SCRATCH is the codec's own transient px+work need
 * (gb_sprite_codec.h: GB_SPRITE_MAX_PX + GB_SPRITE_WORK) -- freed the moment
 * gbscr_decode_pic_gen1() returns; a caller sizing one arena-tail slice for
 * BOTH the shell's cache and the pic (U2b review 0b: one slice, carved) adds
 * GBSCR_PIC_TAIL_BYTES on top of gbscr_tail_need()'s own result. */
#define GBSCR_PIC_TILES 49
#define GBSCR_PIC_PACKED_BYTES (GBSCR_PIC_TILES * 16u)                 /* 784 */
#define GBSCR_PIC_DECODE_SCRATCH (GB_SPRITE_MAX_PX + GB_SPRITE_WORK)    /* 3,920 */
#define GBSCR_PIC_TAIL_BYTES (GBSCR_PIC_PACKED_BYTES + GBSCR_PIC_DECODE_SCRATCH) /* 4,704 */
#define GBSCR_NEED_BADGES    (1u << GBSCR_SRC_BADGES)
/* BACKLOG #125: Crystal's own right-corner block -- see the GBSCR_SRC_CARDCORNER
 * enum comment. 0 on Gold (RomGbUi.cardcorner == 0), so callers gate the bit on
 * that, not on gen alone. */
#define GBSCR_NEED_CARDCORNER (1u << GBSCR_SRC_CARDCORNER)

/* BACKLOG #128: "optional" companions to two of the bits above -- passed via
 * gbscr_open()'s/gbscr_tail_need()'s/gbscr_cache_plan()'s NEW `opt_mask`
 * parameter (a SEPARATE mask from `need_mask`, same GbScrSrc bit values).
 * Exactly the two sources that can legitimately resolve to offset 0 on one
 * real game: CARDCORNER (0 on Gold -- see the GBSCR_SRC_CARDCORNER enum
 * comment) and CARDPIC_F (0 on Gold -- no female trainer card exists at all).
 * A bit set in `opt_mask` whose block cannot be located (offset 0) is SKIPPED
 * by gbscr_cache_plan() -- the slot simply stays unused -- instead of failing
 * the whole plan the way the SAME bit set in `need_mask` still does. This is
 * what lets a caller ask for a block "if it's there" in the SAME gbscr_open()
 * that also asks for blocks it truly cannot do without, instead of opening
 * once, inspecting what got located, and retrying with a wider need_mask
 * (the shape BACKLOG #128 removes from pdna_gbtrainer_gen2_card()). No new
 * GbScrSrc values -- only CARDCORNER/CARDPIC_F are ever legitimately optional
 * today; do not add more without re-checking every other src's own "always
 * present on a validated English ROM" assumption first. */
#define GBSCR_OPT_CARDCORNER GBSCR_NEED_CARDCORNER
#define GBSCR_OPT_CARDPIC_F  GBSCR_NEED_CARDPIC_F

/* One located ROM block, bulk-copied into the tail buffer at open: `rom_off` is
 * where rom_gbui found it in the ROM/fused image, `ram_off` is its offset inside
 * the SAME tail buffer gbscr_open() was given, `len` is the block's exact byte
 * length (rom_gbui_tile()'s own tile_count * stride for that block/bpp). */
typedef struct { uint32_t rom_off, ram_off, len; } GbscrBlock;
/* U3: Gen 2's own card caches FONT + FRAMES(=TEXTBOX) + STATUSWORD (the
 * 11-tile run covering the border notch/divider/"ID No"/STATUS word/colon)
 * + LEADERS + BADGES (the page-2 badge-icon overlay) + one of CARDPIC_M/
 * CARDPIC_F simultaneously (both card pages share ONE gbscr_open(), L/R just
 * flips which cells are painted) -- 6 blocks, the new high-water mark
 * (Gen 1's own card only ever needs 4: FONT + TEXTBOX + CARDFRAME + BADGES).
 * BACKLOG #128: the Gen-2 card's SINGLE open now requests CARDPIC_M (always,
 * required) AND CARDPIC_F (optional, only for a female save) TOGETHER, so a
 * female-locate failure can fall back to painting Chris without a second
 * open -- for a Crystal female save with its own corner block also present
 * (need_mask: CARDGFX/STATUSWORD/LEADERS/BADGES/CARDPIC_M, opt_mask:
 * CARDPIC_F+CARDCORNER, both located) that is FONT + 5 + CARDPIC_F +
 * CARDCORNER = 8 blocks -- one more than the old two-open design ever needed
 * at once (it cached only ONE of CARDPIC_M/CARDPIC_F per open). GBSCR_MAX_BLOCKS
 * is raised to 8 for exactly this combination (measured: does NOT move either
 * build's own `STACK ok` deepest-chain total -- neither build's reported
 * deepest chain passes through gbscr_open/GbScreen at all, see BACKLOG #128's
 * own report). tests/host_gbscreen_test.c's own subset-enumeration check (every
 * combination of the 6 Gen-2-card-era bits) can exercise all 6 set at once
 * (7 blocks with FONT) without hitting a cap the real card never reaches --
 * an earlier revision left this at 6 exactly and the real card's own combo
 * (7 blocks then, before STATUSWORD replaced two separate blocks) silently
 * failed closed, falling every card back to the plain page. */
#define GBSCR_MAX_BLOCKS 8

/* U2b item 1: repaints are SD-free. `gbscr_mem_read()` (pdna_gbscreen.c) is a
 * GbReadFn that serves rom_gbui_tile()/rom_gbui_glyph()'s reads out of `tail`
 * (the caller-owned buffer gbscr_open() was given) via this table instead of the
 * SD card -- built ONCE at open, from the located blocks need_mask asked for; the
 * FIL (SD build) or FusedGbSlice (delta build) is never touched again after
 * gbscr_open() returns. A read for a ROM range this table has no entry for fails
 * (returns 0/false) -- the caller's own need_mask must cover every GbScrSrc it
 * paints, same contract app_arena_acquire()'s callers already carry for sizing. */
typedef struct {
  const uint8_t* tail;
  GbscrBlock blocks[GBSCR_MAX_BLOCKS];
  int        nblocks;
  /* U2c: the decoded, packed Gen-1 player pic (GBSCR_PIC_PACKED_BYTES, or NULL
   * if never decoded / decode failed) -- set by gbscr_decode_pic_gen1(), read
   * by gbscr_tile_pixels()'s GBSCR_SRC_PIC case. NOT one of `blocks` above (it
   * is not a rom_gbui block and is not served through gbscr_mem_read()). */
  const uint8_t* pic;
  /* BACKLOG #91 M1: the Gen-1 MAP screen's own raw-2bpp tile cache (see
   * GBSCR_SRC_MAPTILES's enum comment) -- caller-owned, caller-filled, NULL
   * until a map screen sets it. `maptiles_n` bounds a cell's `tile` index
   * (gbscr_tile_pixels() falls back to flat BLANK, same as every other
   * unavailable src, for an out-of-range index or a NULL pointer). */
  const uint8_t* maptiles;
  uint16_t        maptiles_n;
  /* M1-G2 (BACKLOG #91) colour, design doc §7.8: caller-owned, caller-filled,
   * NULL until the Gen-2 MAP screen sets them (both NULL = the fixed 4-shade
   * grey ramp, same as today -- a runtime safety net, not a design tier, see
   * pdna_gbmap2.c's own anchor-miss fallback). `maptiles_pal` is up to 8
   * palettes x 4 RGB15 colours (already GBA-native, no conversion, design
   * §7.3); `maptiles_palidx` is ONE byte per screen cell (GBSCR_COLS*
   * GBSCR_ROWS == 360 B), indexed the SAME way gbscr_tile_pixels()'s own
   * `idx` parameter already is (cy*GBSCR_COLS+cx) -- selects which of the up
   * to 8 palettes that cell's MAPTILES tile paints with. Touches ONLY the
   * GBSCR_SRC_MAPTILES case in gbscr_tile_pixels(); every other GbScrSrc is
   * unaffected (containment rule, design §7.8). */
  const uint16_t* maptiles_pal;
  const uint8_t*  maptiles_palidx;
} GbscrCache;

/* U2b item 1: pure (no tonc/FatFs) -- host-testable directly (tests/
 * host_gbscreen_test.c). gbscr_block_bytes()/gbscr_block_off() are the exact
 * byte-length/located-offset lookup gbscr_open() uses to size and fill the tail
 * cache; gbscr_mem_read() is the GbReadFn gbscr_flush() binds every repaint to. */
uint32_t gbscr_block_bytes(uint8_t gen, GbScrSrc src);
uint32_t gbscr_block_off(const RomGbUi* gu, uint8_t gen, GbScrSrc src);
bool     gbscr_mem_read(void* ctx, uint32_t off, void* buf, uint32_t len);

/* U2b/U2c review item 0c: gbscr_tail_need()'s own byte arithmetic (SCRATCH_MIN +
 * FONT + every need_mask/opt_mask block), exported so a caller sizing its OWN
 * arena-tail request (on top of gbscr_open()'s own needs) uses the identical
 * formula gbscr_open_inner() gates on, rather than re-deriving it. gbscr_cache_plan()
 * is the pure (no I/O) half of what used to be gbscr_cache_block()'s loop: given an
 * already-LOCATED RomGbUi (offsets set; .ok/.read/.ctx unused), it decides the
 * byte layout (rom_off/ram_off/len, in the fixed FONT-then-need_mask-then-opt_mask
 * order) a real open() would use, with no ROM read at all -- exactly what
 * tests/host_gbscreen_test.c needs to catch a shifted-glyph layout bug the shot
 * harness cannot see. Returns false (fail closed) if a NEED_MASK block has no
 * located offset/size, or the plan would overrun GBSCR_MAX_BLOCKS.
 *
 * BACKLOG #128: `opt_mask` (a bitwise-OR of GBSCR_OPT_* -- disjoint in PURPOSE
 * from `need_mask`, though the underlying GbScrSrc bit values are shared) is a
 * SECOND set of blocks the caller wants IF they are there. A block in opt_mask
 * whose located offset is 0 is SKIPPED (its slot simply is not planned) rather
 * than failing the whole plan -- the caller checks whether it got what it
 * asked for afterward, e.g. via gbscr_has_block(). gbscr_tail_need() reserves
 * bytes for an opt_mask block the SAME worst-case way it reserves a need_mask
 * block (the RomGbUi is not located yet at gbscr_tail_need()'s own call site --
 * before the ROM scan -- so whether the block will actually resolve to a
 * nonzero offset is not yet knowable; reserving less and hoping is not an
 * option gbscr_open_inner()'s single caller-owned tail buffer affords). A bit
 * set in BOTH masks is treated as need_mask (required) -- callers should not
 * do this, but it fails safe rather than silently downgrading a requirement. */
uint32_t gbscr_tail_need(uint8_t gen, uint32_t need_mask, uint32_t opt_mask);
bool     gbscr_cache_plan(uint8_t gen, uint32_t need_mask, uint32_t opt_mask,
                          const RomGbUi* gu, uint32_t tail_len, GbscrCache* out);

/* U2c: the Gen-1 player pic pack/unpack pair -- pure arithmetic (no tonc/
 * FatFs), exported (moved above this module's own tonc/FatFs boundary,
 * minor U2c review) so tests/host_gbscreen_test.c can round-trip them
 * directly instead of only exercising them through the shipped ROM-decode
 * path. `px` is gb_sprite_gen1_buf()'s own output shape (index 0..3 per
 * pixel, row-major, stride `w`) -- see pdna_gbscreen.c's own top-of-block
 * comment for the exact packed layout (4 px/byte, 2 bits each, LSB-first,
 * 16 B/tile). `out`/`packed` must be >= GBSCR_PIC_PACKED_BYTES.
 * gbscr_unpack_pic_px() returns 0 (the same "lightest" value pack's own
 * zero-fill leaves for an undecoded tile) for any (px_x, px_y) outside the
 * tiles_w*8 x tiles_h*8 grid, rather than reading past the buffer. */
void    gbscr_pack_pic(const uint8_t* px, int w, int h, int tiles_w, int tiles_h, uint8_t* out);
uint8_t gbscr_unpack_pic_px(const uint8_t* packed, int tiles_w, int tiles_h, int px_x, int px_y);

typedef struct {
  RomGbUi   gu;                 /* located offsets; .ctx/.read are STALE between
                                  * calls -- gbscr_flush() rebinds a local copy to
                                  * `cache` below, never the ROM/FIL again        */
  GbscrCache cache;              /* U2b item 1: the RAM tile bank                */
  uint8_t   map[GBSCR_CELLS];   /* per-cell tile index / charmap byte           */
  uint8_t   src[GBSCR_CELLS];   /* per-cell GbScrSrc                            */
  uint8_t   dirty[GBSCR_DIRTY_BYTES];
  uint8_t   gen;                /* PDNA_GEN1 / PDNA_GEN2                        */
  bool      ok;                 /* gbscr_open() succeeded; gbscr_* are no-ops otherwise */
  bool      scale_dirty;        /* U2b item 3: gb_scale_mode changed during THIS
                                  * screen's visit -- gbscr_persist_mode()/close()
                                  * writes config.cfg once iff this is set        */
  /* D9 (U2c review): all-NULL (gbscr_open()'s own memset-to-0) means "no
   * override" -- gbscr_flush() paints its own base legend ("A OK  B BACK
   * SEL SIZE") plus the caller's `legend_extra`, unchanged (every existing
   * caller, e.g. gbscr_run_demo's "EXIT", keeps this look). A screen that
   * calls gbscr_set_legend() REPLACES the base entirely with up to 4 slots
   * (NULL entries are skipped) -- for a screen like the Gen-1 trainer card
   * where B does not mean "back" and the base legend would contradict the
   * screen's own key line ("B BACK" next to "B SAVE"). See gbscr_set_legend().
   *
   * D1 fix (U2c 2nd re-verify): a slot holds the ACTION WORD ONLY now ("EDIT",
   * not "A EDIT") -- the shell paints the fixed key name (row order A/B/SEL/
   * START, kGbscrLegendKeys in pdna_gbscreen.c) in the LEFT side bar and this
   * word in the RIGHT side bar, at the same y. Joining "KEY WORD" into one
   * string never fit the 36-px bar for "START MORE"/"SEL SIZE" and truncated
   * with a tilde; two columns give each half its own 36-px budget instead. */
  const char* legend[4];
} GbScreen;

/* gb_scale_mode -- the ONE new EWRAM byte this whole shell adds (design sec 1.5/
 * 3.4). 0 = 1:1 centred (default), 1 = stretched. Owned by the shell, not any one
 * screen; persisted as config.cfg's "gbscale=0|1" key next to romgb1/romgb2. */
extern uint8_t gb_scale_mode;

/* D4 fix (U2a review): the tables blit_stretched() (pdna_gbscreen.c) ACTUALLY
 * reads, exposed here (not `static`) so tests/host_gbscreen_test.c checks the
 * real thing instead of a pair of unused "destination -> source" LUTs the
 * blit never touched. For source row r (0..143), gbscr_y_dst0[r] is the FIRST
 * destination row it shows on and gbscr_y_dst_count[r] (1 or 2) how many
 * consecutive destination rows -- every 9th source row maps to two
 * destination rows, 16 duplicates, covering destination 0..159 exactly once.
 * x needs no table: the blit's own period-2 shift/mask formula (dx0 = 3*(s>>1)
 * + (s&1), dxn = (s&1) ? 2 : 1, for source column s = 0..159) is exact and
 * division-free already, covering destination 0..239 exactly once. */
extern const uint8_t gbscr_y_dst0[144];
extern const uint8_t gbscr_y_dst_count[144];

/* 1:1 canvas origin in the Mode-3 framebuffer (design sec 1.5). */
#define GBSCR_ORIGIN_X 40
#define GBSCR_ORIGIN_Y 8

/* Open the shell for generation `gen` (PDNA_GEN1/PDNA_GEN2): stack-room gate
 * (PDNA_GB_UI_NEED) -> resolve the ROM (SD: app_gb_rom_path()/gb_rom_path_beside(),
 * same order as gb_art_source.c's gb_art_resolve_path; delta: fused_gb_rom()) ->
 * rom_gbui_open_loc() against /PokeDNA/gbui<gen>.loc (written on a miss, Omega-
 * only, SD build only) -> English-release check (rom_gbui_open()'s own G1-C
 * BlankLeaderNames / G2 structural checks already fail closed on a JP ROM, per
 * design R1 -- this function adds no separate check on top) -> U2b item 1: bulk-
 * copies FONT plus every block `need_mask` names (GBSCR_NEED_*) into `tail`, then
 * closes the FIL/drops the fused slice -- gbscr_flush() never touches the SD card
 * again for the life of this `gs`.
 *
 * `tail`/`tail_len`: a caller-owned buffer (e.g. gb12_arena_tail(), or the
 * caller's own app_arena_acquire()) used for BOTH the 2,048-B rom_gbui scan
 * scratch (first, during the ROM scan/re-validate) and the RAM tile bank the scan
 * result is then copied into (same bytes, reused after the scan is done with
 * them) -- required size is 2,048 + FONT(1,024) + the byte length of every block
 * `need_mask`/`opt_mask` requests (TEXTBOX 512/432, CARDFRAME 640, BADGES 1,024/704,
 * Gen1/Gen2 respectively). A `tail_len` too small for that, or a NULL `tail`, is
 * a clean refusal (kReasonNoTail) -- there is no smaller/slower fallback path
 * inside gbscr_flush() any more (U2b item 1 deleted the per-tile FIL read): the
 * screen is expected to fall back to its own plain page instead, per design 3.5.
 *
 * BACKLOG #128: `opt_mask` (a bitwise-OR of GBSCR_OPT_*, see that macro's own
 * comment) names blocks the caller wants CACHED IF THE ROM HAS THEM -- a bit's
 * block resolving to offset 0 (Gold's cardcorner, or any ROM's cardpic_f on a
 * non-female visit) does not fail this open the way the same bit in `need_mask`
 * would. Check gbscr_has_block() afterward to learn whether a requested
 * optional block actually got cached. A caller that needs nothing optional
 * passes 0.
 *
 * On success: zeroes the tilemap (every cell BLANK), returns true.
 * On refusal: `*reason` (may be NULL) is set to a short, static, user-facing
 * string ("no ROM registered" / "not enough stack" / "could not open ROM" /
 * "not an English release" / "not a Game Boy ROM" / "no tile-bank memory"),
 * `gs->ok` is false, and every other gbscr_* call on `gs` is a safe no-op. */
bool gbscr_open(uint8_t gen, GbScreen* gs, uint8_t* tail, uint32_t tail_len,
                uint32_t need_mask, uint32_t opt_mask, const char** reason);

/* BACKLOG #128: did `gs`'s open actually cache `src` (whether requested via
 * need_mask -- always true after a successful open -- or opt_mask, where it
 * depends on whether the ROM has the block)? Pure lookup, no I/O: a src is
 * cached iff its RomGbUi offset is nonzero (gbscr_cache_plan()'s own "skip an
 * opt_mask block at offset 0" rule, mirrored here rather than re-scanning
 * gs->cache.blocks[]). False on a `gs` that never opened successfully. The
 * caller that asked for GBSCR_OPT_CARDPIC_F/GBSCR_OPT_CARDCORNER uses this
 * instead of re-deriving the same check from gs->gu's raw fields, so the one
 * place that knows "offset 0 means absent" stays gbscr_block_off(). */
bool gbscr_has_block(const GbScreen* gs, GbScrSrc src);

/* Release any resources gbscr_open() took (the SD build's FIL is already closed
 * by the time gbscr_open() returns -- this exists for symmetry/future-proofing
 * and to make the shell's own lifetime explicit at call sites). Safe on a `gs`
 * that never opened successfully. U2b item 3: also calls gbscr_persist_mode()
 * (below), so a scale change made anywhere during this screen's visit survives
 * leaving it, without every caller having to remember to call cfg_save() itself. */
void gbscr_close(GbScreen* gs);

/* U2c: decode the Gen-1 player pic ONCE (never per repaint) and expose it to
 * gbscr_tile_pixels()'s GBSCR_SRC_PIC case via gs->cache.pic. Call this AFTER
 * a successful gbscr_open(PDNA_GEN1, ...) -- it is a no-op (false) on Gen 2, on
 * a `gs` that never opened, or when the ROM never located a player-pic offset.
 *
 * `buf` (>= GBSCR_PIC_TAIL_BYTES) is caller-owned (the SAME gb12_arena_tail()
 * slice as `tail`, past the bytes gbscr_open() used -- U2b review item 0b: one
 * slice, carved, not a second borrow): buf[0 .. GBSCR_PIC_PACKED_BYTES) becomes
 * the packed pic (retained -- gs->cache.pic points into it and it must stay
 * alive for the screen's whole visit); the rest is transient codec scratch
 * (GBSCR_PIC_DECODE_SCRATCH), free to be reused for anything else once this
 * call returns.
 *
 * This does its OWN small ROM read (gbscr_open() already closed its FIL / let
 * go of the fused slice by the time it returns) -- re-resolves the path (SD
 * build) or re-slices the fused image (delta build), same helpers gbscr_open()
 * itself uses. One-time cost at open, never a repaint cost.
 *
 * Returns false (gs->cache.pic left NULL -- GBSCR_SRC_PIC cells then paint the
 * same flat BLANK colour they always did) on any failure: not Gen 1, `gs` not
 * open, no located playerpic offset, `buf` too small, the ROM could not be
 * re-opened/re-sliced, or the codec itself failed (a malformed/corrupt pic
 * blob -- GB_SPRITE_E_HEADER/E_DATA/E_READ/E_ARGS). Never crashes on a bad ROM;
 * the shell simply shows no pic, same posture as every other GBSCR_SRC_*'s own
 * fail-closed path. */
bool gbscr_decode_pic_gen1(GbScreen* gs, uint8_t* buf, uint32_t buf_len);

/* U2b item 3: writes config.cfg (via app_cfg_save(), pdna_app.h) NOW iff
 * gb_scale_mode changed at least once during `gs`'s visit (gs->scale_dirty, set
 * by gbscr_toggle_scale()) -- then clears the flag, so a screen that calls this
 * mid-session (rather than waiting for gbscr_close()) never double-writes for
 * the same toggle. Safe on a `gs` that never opened / never toggled (a no-op).
 * The shell owns this so entering/leaving a GB screen from the nav menu
 * persists the mode -- Settings' own B path is unaffected (it already called
 * the same writer directly, before this existed, and still does). */
void gbscr_persist_mode(GbScreen* gs);

/* Write one cell. `tile` is a raw tile index for TEXTBOX/CARDFRAME/BADGES/PIC, or
 * the game's own charmap byte for FONT (0x80..0xFF), ignored for BLANK. Out-of-
 * range (x,y) or a `gs` that never opened is a no-op. Marks the cell dirty only
 * when it actually changed. */
void gbscr_cell(GbScreen* gs, int x, int y, GbScrSrc src, uint8_t tile);

/* The GB-text renderer (design sec 3.2): each GLYPH of `ascii` (gb_char_encode()'s
 * unit -- "PK"/"MN"/the gender signs/the 'd apostrophe-contractions are ONE glyph,
 * one cell) lands in one cell starting at (x,y), advancing x by one cell per
 * glyph. A glyph that encodes to the space byte (0x7F) becomes a BLANK cell, per
 * the design's own rule -- never a font tile. Cells past column GBSCR_COLS-1 are
 * silently dropped (no wrap), matching PlaceString's own tile-boundary behaviour
 * closely enough for a shell that never itself decides line width. */
void gbscr_text(GbScreen* gs, int x, int y, const char* ascii);

/* How many CELLS gbscr_text(gen, ..., ascii) would actually paint -- one per GLYPH
 * (gb_char_encode()'s unit), NOT one per byte of `ascii`. gbnames review A3: a
 * caller that advances its own cursor/blank-sweep by strlen(ascii) instead is
 * counting UTF-8 BYTES, and every multi-byte glyph this codec accepts (the two-
 * byte "e"+combining-acute -> UTF-8 0xC3 0xA9 for e-acute, and the two-ASCII-char
 * apostrophe-contractions "'d"/"'s"/...) then overcounts by one cell per glyph --
 * the sweep starts one column too far right and leaves the previous name's last
 * glyph on screen (docs/shots/gb/gbnames_crystal_02_balls_real_names.png: "POKé
 * BALLE" -- the stray 'E' at column 17 is Ultra Ball's trailing L that the
 * strlen-based sweep never reached because "POKé BALL" is 9 glyphs but strlen()
 * of its UTF-8 spelling is 10 bytes). Callers doing their own column arithmetic
 * (a blank-sweep bound, not a straight gbscr_text() paint) must use THIS, not
 * strlen(), to compute where the text actually ends. Same truncation-at-
 * GBSCR_COLS behaviour as gbscr_text() itself. */
int gbscr_text_cols(uint8_t gen, const char* ascii);

/* Names already stored in GB encoding (gb_trainer's name_raw) go straight in, one
 * byte per cell, no ASCII step -- same 0x7F-is-blank rule as gbscr_text(). Paints
 * exactly `n` cells (the caller's own field width) -- but once it sees the 0x50
 * terminator, or any other byte that is not a real font tile (every real GB
 * font tile is 0x7F or >= 0x80; the terminator and every other control byte is
 * < 0x80 and != 0x7F), that byte AND every byte after it paint BLANK instead of
 * whatever value they actually hold. This clears stale tail bytes left over
 * from a longer PREVIOUS name on a shorter repaint (PlaceString itself stops
 * at 0x50 and never touches the tail, so on real hardware a shorter new name
 * only looks right because VRAM was already blank there once; this shell
 * reuses cells across repaints and must blank the tail itself). */
void gbscr_raw(GbScreen* gs, int x, int y, const uint8_t* bytes, int n);

/* Mark every cell dirty (a full repaint) -- used after gbscr_toggle_scale() and
 * on a screen's first flush. */
void gbscr_mark_all_dirty(GbScreen* gs);

/* Flip gb_scale_mode and mark the whole canvas dirty so the very next
 * gbscr_flush() repaints everything in the new mode. U2b item 3: also sets
 * `gs->scale_dirty` so gbscr_close()/gbscr_persist_mode() know to write
 * config.cfg -- persisting the value itself still happens later (at close, or
 * whenever a screen calls gbscr_persist_mode()), never on every single
 * keypress. */
void gbscr_toggle_scale(GbScreen* gs);

/* Repaint every dirty cell into the Mode-3 framebuffer in the CURRENT gb_scale_mode,
 * then draw the legend (design sec 1.5: two stacked side-bar columns at 1:1, a
 * bottom scrim overlay when stretched) and clear the dirty bitmap. `legend_extra`
 * (may be NULL) is one more screen-supplied key line appended after the shell's
 * own A OK / B BACK / SEL SIZE rows, UNLESS the screen called gbscr_set_legend()
 * (below), in which case `legend_extra` is ignored and the override lines are
 * painted instead -- see gbscr_set_legend()'s own doc comment. GBA-only
 * (touches vid_mem/tonc); a no-op if `gs` never opened. */
void gbscr_flush(GbScreen* gs, const char* legend_extra);

/* D9 (U2c review): let a screen REPLACE the shell's base legend instead of
 * only appending to it -- for a screen whose own key line contradicts the
 * base (e.g. the Gen-1 trainer card's "B SAVE" vs. the base's unconditional
 * "B BACK"). `lines[0..3]` is fixed-slot: row0=A, row1=B, row2=SEL, row3=
 * START (kGbscrLegendKeys, pdna_gbscreen.c) -- pass the ACTION WORD ALONE
 * ("EDIT", not "A EDIT"; NULL entries skip that whole row, key included, e.g.
 * a read-only visit's unused A row). D1 fix (U2c 2nd re-verify): at 1:1 the
 * shell paints the key name in the LEFT side bar and this word in the RIGHT
 * side bar (each its own 36-px budget -- the old single joined string, up to
 * 57 px for "START MORE", truncated); stretched, the shell re-attaches the
 * key name and joins every row with "  " into ONE bottom-scrim string (the
 * caller is responsible for keeping that joined width under the scrim, same
 * as any other stretched-mode legend text -- measure with ui_ptext_w()).
 * Pass an all-NULL array (or never call this) to keep the shell's own base
 * legend, e.g. gbscr_run_demo's "EXIT" line. Takes effect on the very next
 * gbscr_flush(); does not itself mark anything dirty (call
 * gbscr_mark_all_dirty() too if the legend must repaint before anything else
 * does). */
void gbscr_set_legend(GbScreen* gs, const char* const lines[4]);

/* U2c: pixel bounds (x1/y1 one past the last covered pixel) of a `w`x`h` group
 * of cells at (cx,cy), in the CURRENT gb_scale_mode -- for a screen's own
 * cursor frame, drawn on the framebuffer directly after gbscr_flush(). Any of
 * the four out-pointers may be NULL. GBA-only (reads gb_scale_mode/the LUTs
 * this shell already carries; not part of the pure host-testable half). */
void gbscr_cell_rect(int cx, int cy, int w, int h, int* x0, int* y0, int* x1, int* y1);

/* U2a's own DEMO screen: opens the shell for `gen`, draws the located font's
 * whole 128-glyph sheet inside a text-box-tile border plus one line of text, and
 * lets A-OK/B-BACK/SELECT-toggle-scale drive it -- so the shell can be shot
 * standalone (both scale modes) before any real card (U2b) exists. Reachable
 * from Settings (SELECT, a hidden key -- see pdna_main.c's pdna_settings()) in
 * every build; refuses with a message box on gbscr_open()'s own reason string
 * when no ROM/stack/English release/tile-bank memory is available. GBA-only
 * (drives wait_keys/msg_wait), not part of the pure host-testable half.
 *
 * U2b item 1: this is the standalone (no GB session, no g_ed) caller, so it
 * takes its own tail buffer via app_arena_acquire()/app_arena_release() rather
 * than gb12_arena_tail() (that one only ever returns non-NULL inside a resident-
 * image GB session, which Settings' hidden key is not) -- refuses the same way
 * a real screen would if the arena is already held or the PC is dirty. */
void gbscr_run_demo(uint8_t gen);

#endif /* PDNA_GBSCREEN_H */
