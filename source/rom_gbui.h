#ifndef ROM_GBUI_INCLUDED
#define ROM_GBUI_INCLUDED

#include <stdint.h>
#include <stdbool.h>

#include "gb_sprite_codec.h"   /* GbReadFn */

/*
 * The Gen-1/Gen-2 trainer-card and bag UI GRAPHICS (font, text-box frame,
 * card-frame block, badges, gym-leader faces, the player's card pic, the
 * pack pocket art) -- located BY SHAPE in the user's own Game Boy cartridge
 * dump, the same posture rom_gbsprite.c / rom_gbicon.c already carry for the
 * Pokemon sprites and menu icons. Design: docs/GB-GAME-SCREENS-DESIGN.md
 * (BACKLOG #66/#67), section 2 for every signature + its verification rule,
 * Appendix A for the offsets these locators land on in Guy's four dumps
 * (recorded for a human to eyeball -- NEVER an input to this code).
 *
 * PokeDNA ships NO Game Freak art. Every graphic here is a transient read of
 * a file the user already owns; nothing is cached to disk except a small
 * table of FILE OFFSETS AND their CODE ANCHORS (RomGbUiLoc, 108 B --
 * BACKLOG #71 grew it from 68 B by adding anchor[ROM_GBUI_ANCH_COUNT], see
 * that struct's own comment). A cache hit costs a small constant number of
 * re-reads (one per stored anchor, plus the existing structural checks):
 * measured on the corpus, Gen 1 26 reads (was 21 before #71), Gen 2 Gold 17
 * (was 13), Gen 2 Crystal 18 (was 13) -- vs several hundred for a full scan.
 * PDNA_GB_UI_NEED is NOT unaffected by this growth (correcting an earlier
 * assumption): it moves from 5,320 to 5,384 (+64 B), because
 * rom_gbui_open_loc()'s own stack frame grows from 120 to 176 B once
 * revalidate_loc() and its anchor_*() helpers are inlined into it (each has
 * exactly one call site inside open_loc(), so -O2 folds them in rather than
 * keeping revalidate_loc() as a separate frame on the chain) -- see
 * PDNA_GB_UI_NEED's own comment below for the full -fstack-usage proof and
 * the two real call sites' margins (both stay >6 KB free after this move).
 *
 * PURE C: no tonc, no FatFs, no GBA headers, no mutable globals. All I/O goes
 * through the caller's GbReadFn, so tests/host_romgbui_test.c runs this exact
 * code on the PC against roms/gb/{Red,Yellow,Gold,Crystal}.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DATA IS, AND HOW THIS MODULE FINDS IT
 * ---------------------------------------------------------------------------
 * Nothing is pinned to a per-game constant unless it is CODE rather than data.
 * Where the graphic itself has no scannable shape (a font is just pixels),
 * the locator anchors on the LOADER ROUTINE's own opcode bytes -- exactly how
 * rom_gbsprite.c finds PokemonPalettes from _GetMonPalettePointer. Signatures
 * are short opcode runs (<= 33 B) or structural fingerprints only, never a
 * glyph/pixel bitmap.
 *
 * GEN 1 (pokered -- Red/Blue/Yellow)
 *   G1-F Font              LoadFontTilePatterns's LCD-off arm (home/load_font.asm),
 *                           128 tiles x 8 B, 1bpp. 1 hit required.
 *   G1-T TextBoxGraphics    same routine's other LCD-off arm, 32 tiles x 16 B, 2bpp.
 *   G1-C Card-frame block   DrawTrainerInfo's three consecutive far-copies
 *                           (engine/menus/start_sub_menus.asm), pinned by the
 *                           source-address relation C-A==144, E-A==512; the
 *                           BANK is then the unique one of banks[0..N) whose
 *                           9 frame tiles are all distinct, whose 22
 *                           leader-name tiles are ALL blank (erased in the
 *                           English releases) and whose 8 badge-number tiles
 *                           are non-degenerate.
 *   G1-B Badges + faces     the 8-byte .FaceBadgeTiles table
 *                           {20,28,30,38,40,48,50,58}; graphics start at
 *                           anchor+8, 64 tiles x 16 B, 2bpp.
 *   G1-P Player pic         DrawTrainerInfo's own opening `11 lo hi 01 01
 *                           bank`; of the ~70 raw hits, only the candidates
 *                           whose target byte is 0x77 (a Gen-1 pic's own
 *                           w<<4|h header) survive -- must collapse to
 *                           exactly one.
 *
 * GEN 2 (pokecrystal/pokegold -- Gold/Silver/Crystal)
 *   G2-R Text-box frames    LoadFrame (engine/gfx/load_font.asm), BYTE-IDENTICAL
 *                           in Gold and Crystal; 9 frames x 6 tiles, 1bpp.
 *   G2-F Font               DERIVED: Font = Frames - 1536 (gfx/font.asm's fixed
 *                           file order: FontExtra, Font, FontBattleExtra, Frames).
 *   G2-E FontExtra          DERIVED: FontExtra = Font - 512.
 *   G2-B/L Badges/Leaders   TrainerCard_Page2_LoadGFX; TWO hits in each ROM
 *                           (page-2 and the unreferenced page-3 INCBIN), and
 *                           the rule is "all hits byte-identical", not
 *                           "exactly one hit" (R4).
 *   G2-P Card pic           Crystal: GetCardPic (Chris + Kris, Kris = Chris +
 *                           0x230 exactly) + TrainerCardGFX. Gold has no
 *                           gender branch: ChrisPicAndTrainerCardGFX (35 + 6
 *                           tiles, no Kris pic at all).
 *   G2-K Pack pocket art    pure SHAPE, no code anchor: four consecutive LE
 *                           pointers in [0x4000,0x8000) satisfying
 *                           p0=base+240, p1=base+720, p2=base, p3=base+480.
 *                           Gold: exactly 1 hit. Crystal: exactly 2 (the 2nd
 *                           is PackFGFXPointers, Kris's pack).
 *
 * R5 (design doc): Crystal's CardStatusGFX offset is only PROVEN for Gold (it
 * is read straight out of that ROM's own loader); the Crystal relationship
 * (leaders - 96) is a derivation from Gold's layout applied to a DIFFERENT
 * loader and is UNPROVEN. This module therefore exposes no `cardstatus`
 * field at all -- U3 must either verify it independently or draw the page-1
 * status strip from FontExtra's own literal tiles instead.
 *
 * ---------------------------------------------------------------------------
 * FAIL CLOSED
 * ---------------------------------------------------------------------------
 * `ok` is 1 only when every graphic THIS GEN REQUIRES was located and passed
 * its structural verification:
 *   Gen 1: font, textbox, cardframe, badges, playerpic.
 *   Gen 2: frames, font, fontextra, badges, leaders, cardpic_m, cardgfx,
 *          pack_m -- and, when the ROM is Crystal-shaped (its GetCardPic
 *          signature hit rather than Gold's), additionally cardpic_f, pack_f.
 * A ROM that matches neither gen's full requirement set gets `gen = 0`,
 * every offset 0, `ok = 0`.
 * ONE slot is deliberately OUTSIDE this requirement set: BACKLOG #99's Gen-1
 * key-item bit table (RomGbUi.g1_keyitems / rom_gbui_g1_key_item()). A miss
 * there never fails `ok` -- the caller (source/pdna_gbbag.c) falls back to
 * gbb_is_g1_key_item()'s factual id list and says so on its reason line.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT COSTS
 * ---------------------------------------------------------------------------
 * No statics, no globals: RomGbUi is 88 B, entirely caller-owned (stack or
 * app_arena_acquire()). open()'s scan window is a caller-owned scratch buffer
 * (>= ROM_GBUI_SCRATCH_MIN, 2048 B -- the longest pattern here is 33 B, same
 * generous margin rom_gbsprite.c keeps) used ONLY during open()/open_loc();
 * it is not retained in the struct and may be released or reused afterward.
 * PDNA_GB_UI_NEED (below) is open()'s own measured stack-frame gate.
 */

typedef enum {
  ROM_GBUI_NONE = 0,
  ROM_GBUI_GEN1 = 1,          /* Red / Blue / Yellow                         */
  ROM_GBUI_GEN2 = 2           /* Gold / Silver / Crystal                     */
} RomGbUiGen;

/* open()'s scan window. The longest pattern (the card-frame/card-pic
 * signatures) is 33 B; 2048 halves the number of whole-ROM read passes the
 * same way ROM_GBSPRITE_SCRATCH_MIN does. */
#define ROM_GBUI_SCRATCH_MIN  2048u

/* MEASURED (arm-none-eabi-gcc -mcpu=arm7tdmi -mtune=arm7tdmi -O2
 * -mthumb-interwork -mthumb -fstack-usage -c source/rom_gbui.c, 2026-09-09,
 * after the BACKLOG #71 anchor-revalidation batch). rom_gbui.c is a plain
 * .c file, not an .iwram.c one, so it is built with the Makefile's ARCH
 * flags (-mthumb-interwork -mthumb), NEVER IARCH (-mthumb-interwork -marm
 * -mlong-calls, reserved for .iwram.c fast-path files) -- an earlier version
 * of this note measured with IARCH by mistake, which is the wrong compiler
 * flags for the file that actually ships.
 *
 * BACKLOG #99 (2026-09-10) re-measured with the same exact command after
 * adding the optional Gen-1 key-item locate step: rom_gbui_open grew 1160 ->
 * 1224 (+64: the new try_g1_keyitems()/g1_keyitems_verify() call, plus `ki`/
 * `ki_anchor` bookkeeping, folds into locate()'s already-inlined frame the
 * same way every other try_*() does); rom_gbui_open_loc held at 176 B exactly
 * (anchor_g1_keyitems()'s own frame folds into the same inlined budget
 * revalidate_loc()'s other anchor_*() helpers already use -- one more `if`
 * block costs no extra stack once inlined). g1_keyitems_verify itself is a
 * new 32-B own-frame (single call site inside try_g1_keyitems(), which is
 * itself single-call-site inside locate() -- both fold into rom_gbui_open()).
 *
 * Per-function own-frame sizes (bytes): rd 16, font_verify 160, block_eq
 * 160, distinct_tiles 1568, rom_gbui_open 1224, rom_gbui_open_loc 176,
 * rom_gbui_save_loc 16, rom_gbui_tile 64, rom_gbui_glyph 32, parse_header
 * 104, anchor_pack 24, try_badgeleader_hit 40, g1_keyitems_verify 32.
 * sizeof(RomGbUi) = 144 on ARM (152 on the host: two 8-B pointers vs ARM's
 * two 4-B ones) -- BACKLOG #99 grew this from 120/128 (+24: g1_keyitems
 * uint32_t + g1_keyitems_bits[15], rounded up to a 4-B struct boundary;
 * MEASURED via sizeof() on both a host build and an arm-none-eabi-gcc
 * compile, not assumed),
 * sizeof(RomGbUiLoc) = 116 (BACKLOG #99 grew this from 108: one new
 * off[]/anchor[] slot pair, 4+4 B; BACKLOG #71 grew both RomGbUi/RomGbUiLoc
 * from 88/68: RomGbUi
 * carries an anchor[ROM_GBUI_ANCH_COUNT] the same shape RomGbUiLoc does, see
 * the struct comments). locate(), try_font/try_textbox/try_cardframe/
 * try_badges/try_playerpic/try_frame/try_cardpic_crystal/try_cardpic_gold,
 * revalidate_loc(), and anchor_addr_bank/anchor_cardframe/anchor_g1_badges/
 * anchor_badgeleader/anchor_cardpic no longer appear as their OWN frames --
 * each has exactly one call site (inside rom_gbui_open() or
 * rom_gbui_open_loc() respectively) so -O2 inlines them; their stack cost is
 * folded into rom_gbui_open()'s and rom_gbui_open_loc()'s own-frame numbers
 * above instead. (anchor_pack and try_badgeleader_hit stayed separate: each
 * has TWO call sites -- pack_m/pack_f, hit0/hit1 -- so GCC declined to
 * inline both.)
 *
 * The deepest call chain is rom_gbui_open() -> distinct_tiles() -> rd():
 * 1,160 + 1,568 + 16 = 2,744 B own-frames-summed (locate() is now INSIDE
 * rom_gbui_open()'s 1,160, per the inlining note above -- this replaces the
 * old rom_gbui_open() -> locate() -> distinct_tiles() -> rd() = 2,720 B
 * three-frame chain with an equivalent two-frame one, 8 B worse from the
 * anchor bookkeeping locate() now does). font_verify is NOT on this path
 * (1,160 + 160 + 16 = 1,336 is smaller). NOTE (review, 2026-09-09): these
 * numbers are measured WITH -ffast-math -fno-strict-aliasing, which
 * Makefile:237 adds to every build of this file; the pre-#71 frames were
 * insensitive to those two flags, this file's are not (1,144 without them).
 *
 * rom_gbui_open_loc()'s own fall-to-a-full-scan path (open_loc -> open ->
 * distinct_tiles -> rd; verified by objdump that this is a real `bl`, not a
 * sibling/tail call -- open_loc's own frame is still live on the stack while
 * open() runs) adds its 176-B frame (up from 120: revalidate_loc() and every
 * anchor_*() helper it calls are now inlined into it) on top of the 2,728
 * above: 176 + 2,744 = 2,920 B -- 80 B worse than the pre-#71 2,840, entirely
 * from open_loc()'s own frame growing by the same 56 B (120 -> 176) that the
 * newly-inlined anchor re-derivation logic costs.
 *
 * FIRST MEASUREMENT WAS 5,120 B WORSE (pre-U1): locate()'s own frame was
 * 5,376 B, not ~1,000 B, because ScanJob's hit array (SCAN_MAX_HITS=128,
 * sized for the ONE signature that needs it -- G1-P's player-pic anchor,
 * 71-97 raw hits across the corpus before the 0x77 filter) was applied
 * uniformly to all 10 job slots: 10 * 128 * 4 B = 5,120 B, when the other 9
 * signatures are unique or near-unique in a real ROM and never need more
 * than a handful of slots. FIXED by giving each job a caller-sized
 * pointer+cap instead of a fixed inline array: 9 jobs at SCAN_SMALL_CAP (8
 * slots, 288 B total) + one at SCAN_PLAYERPIC_CAP (96 slots, 384 B) = 672 B,
 * with IDENTICAL located-offset output on all four of Guy's ROMs
 * (re-verified: tests/host_romgbui_test.c, and again after #71 via
 * tools/gbui_dump.py against the same corpus).
 *
 * NOTE (b99 review NIT-1): the three chain sums below are main's PRE-#99 numbers,
 * deliberately not updated here -- BACKLOG #84b owns this constant and re-measures
 * it with tools/stack_budget.py on the merged tree (measured 5,544 + 64 ISR = 5,608
 * once #99's larger RomGbUiLoc lands). The per-function frames above ARE current.
 * PDNA_GB_UI_NEED = 2,920 (open_loc's full-scan-fallback chain, the worse of
 * the two entry points, UP from 2,840 -- see the #71 delta above) + 1,576
 * (source/pdna_gbscreen.c's gbscr_open_inner() OWN frame, UNCHANGED by #71:
 * pdna_gbscreen.c was not touched by this batch) = 4,480 -- PLUS the leg the
 * call-graph walk cannot follow (U2b review D1, 2026-09-09): distinct_tiles()
 * calls the GbReadFn INDIRECTLY, and on the SD build that is gbscr_sd_read
 * 24 -> f_lseek 80 -> create_chain 40 -> fill_last_frag 16 -> put_fat 32 ->
 * move_window 16 -> disk_read 32 -> flashcartio_read_sector 40 -> diskRead
 * 24 -> ed_sd_dma_rd 48 -> ed_sd_dma_to_rom 552 = 904 B (measured,
 * -fstack-usage, UNCHANGED by #71). 4,480 + 904 = 5,384. Both real call
 * sites' OLD margins (measured at 5,320) were Settings 6,680 free and the
 * nav-menu chain 7,792 free; PDNA_GB_UI_NEED moving by +64 moves each margin
 * by the same -64 (6,616 / 7,728 free) -- neither call site was touched by
 * this batch, so this is arithmetic, not a re-measurement of them.
 *
 * gbscr_cache_block() (the U2b item 1 bulk-copy of FONT + need_mask's blocks
 * into `tail`, called AFTER rom_gbui_open_loc() returns, never nested inside
 * it) adds only 16 B of its own frame plus whichever GbReadFn it calls
 * (gbscr_sd_read: 24 B) -- 40 B, far under the 2,920-B rom_gbui_open_loc chain
 * this replaces as the deepest path, so it does not move PDNA_GB_UI_NEED.
 *
 * D1/D2 fix (U2a review, 2026-09-09): the gate used to live INSIDE the frame
 * it was supposed to be measuring the room FOR (gbscr_open()'s own
 * stack_room() call ran after that frame already existed), which counts the
 * room LEFT UNDER the frame instead of the room the frame NEEDS -- on the
 * artless/SD build's Settings path this made the gate refuse EVERY time on
 * real hardware. The gate lives in a thin `gbscr_open()` wrapper that runs
 * BEFORE the frame exists (the old body is `gbscr_open_inner()`), so
 * PDNA_GB_UI_NEED is the number above taken AS MEASURED -- 5,400 --
 * deliberately NOT rounded up to a 256-B boundary: rounding up here only ever
 * makes the gate MORE conservative than the real chain, and the whole point
 * of this fix is to stop over-refusing on a build that is already tight on
 * stack.
 * This module IS linked and the gate IS live: source/pdna_gbscreen.c gates on
 * PDNA_GB_UI_NEED before gbscr_open_inner, and rom_gbui_open/_open_loc/_tile/
 * _glyph are present in PokeDNA-artless.elf (review, 2026-09-09). The
 * artless/delta "EWRAM ok" lines say nothing about STACK; only the measured
 * chain above does. Margins at 5,400: Settings 6,600 B free, nav chain 7,712.
 *
 * RE-MEASURED (BACKLOG #84b, FOURTH pass, 2026-09-10) after D4's whole-graph
 * blind-spot sweep + its walker fixes (tools/stack_budget.py's own trap #1/#5/#6/
 * #7/#8): `python3 tools/stack_budget.py --elf PokeDNA-artless.elf --builddir
 * "$(pwd)/build-artless" --root gbscr_open_inner --top 1` reported 5,672 B
 * (+64 ISR = 5,736), 336 B above the 5,400 this constant held. Unlike
 * PDNA_PARTY_STRIP_NEED's re-measurement in source/pdna_box.c (a clean,
 * previously-blind-spot branch this same pass declared), this chain's own
 * components were ALREADY fully declared before this pass -- RomGbUi.read@0's
 * gb_art_read/fused_gb_slice_read/gbscr_sd_read union predates it -- so the +336
 * was NOT attributable to a blind spot this pass closed; gbscr_open_inner's own
 * measured frame moved from the 1,576 the #71 batch measured to 1,656 today,
 * for a reason this pass did not track down (an unrelated source change between
 * the two measurements is the likely explanation, not a walker defect).
 *
 * RE-MEASURED AGAIN (BACKLOG #84b, SEVENTH pass, 2026-09-10) after D5a's per-
 * caller field-declaration rewrite (this same tools/stack_budget.py commit):
 * the same command now reports 5,464 B (+64 ISR = 5,528), 208 B BELOW the
 * 5,736 the fourth pass measured. This is a genuine drop, not a regression in
 * the walker's own soundness -- the fourth-pass number was measured against a
 * tree where several offset classes were still globally unioned (D5a's whole
 * point); once RomGbUi.read@0 stopped inheriting implementations that belong
 * to an unrelated offset-0 struct sharing that bare number with it elsewhere
 * in the file, this chain's own credited implementation set (and therefore
 * its measured deepest continuation through rom_gbui_open/distinct_tiles) got
 * narrower and honest, not wider. AS MEASURED, matching this constant's own
 * established convention -- the constant equals its own derivation:
 * 5,464 + 64 = 5,528. The Settings-path/nav-menu-chain DERIVED margins two
 * paragraphs up are stale from the fourth pass and NOT re-verified here;
 * do not trust them without independently re-running this same command
 * against those two call sites before relying on either number. */
#define PDNA_GB_UI_NEED 5528

/* Each entry is the SCAN HIT file offset (where the locator's ScanCb pattern
 * matched), never a located block itself. gen 1 uses FONT/TEXTBOX/CARDFRAME/
 * BADGES/PLAYERPIC; gen 2 uses FRAMES/BADGELEADER/CARDPIC/PACK_M/PACK_F
 * (PACK_F only when cardpic_f != 0, i.e. Crystal-shaped). The two sets never
 * overlap in use (a loc is one gen or the other), but both live in the same
 * array so ROM_GBUI_ANCH_COUNT is a fixed, gen-independent size. See
 * RomGbUiLoc's own comment below (BACKLOG #71) for why this array exists. */
enum {
  ROM_GBUI_ANCH_FONT = 0, ROM_GBUI_ANCH_TEXTBOX, ROM_GBUI_ANCH_CARDFRAME,
  ROM_GBUI_ANCH_BADGES, ROM_GBUI_ANCH_PLAYERPIC,
  ROM_GBUI_ANCH_FRAMES, ROM_GBUI_ANCH_BADGELEADER, ROM_GBUI_ANCH_CARDPIC,
  ROM_GBUI_ANCH_PACK_M, ROM_GBUI_ANCH_PACK_F,
  /* BACKLOG #99: IsKeyItem_'s own `cp HM01` / `push af` / `ld hl,KeyItemFlags`
   * scan hit (Gen 1 only). OPTIONAL -- unlike every other slot, a miss here
   * does not fail Gen 1's `ok` (see ROM_GBUI_OFF_G1_KEYITEMS below); the
   * caller falls back to gbb_is_g1_key_item()'s factual id list. */
  ROM_GBUI_ANCH_G1_KEYITEMS,
  ROM_GBUI_ANCH_COUNT
};

typedef struct {
  GbReadFn read;
  void*    ctx;
  uint32_t size;               /* file size in bytes                        */
  uint8_t  gen;                /* RomGbUiGen                                */
  uint8_t  banks;               /* size / 16 KiB                            */
  uint32_t id_hash;             /* FNV-1a of 0x100..0x14F: cache key         */

  /* Gen 1 */
  uint32_t font;                /* G1-F / G2-F: 128 tiles, 1bpp              */
  uint32_t textbox;             /* G1-T: 32 tiles, 2bpp                      */
  uint32_t cardframe;           /* G1-C: 640 B (9+22+1+8 tiles), 2bpp        */
  uint32_t badges;               /* G1-B (64t) / G2-B (44t), 2bpp            */
  uint32_t leaders;              /* G2-L: leader faces, 2bpp (Gen 2 only)    */
  uint32_t playerpic;            /* G1-P: file offset of the Gen-1 pic blob  */

  /* Gen 2 */
  uint32_t frames;               /* G2-R: 9 x 6 tiles, 1bpp                  */
  uint32_t fontextra;            /* G2-E: 32 tiles, 2bpp                     */
  uint32_t cardpic_m;            /* G2-P: Chris, 35 tiles, 2bpp, col-major   */
  uint32_t cardpic_f;            /* G2-P: Kris (Crystal only), else 0        */
  uint32_t cardgfx;              /* G2-P: TrainerCardGFX, 6 tiles, 2bpp      */
  uint32_t pack_m;                /* G2-K: PackGFX, 60 tiles, 2bpp            */
  uint32_t pack_f;                /* G2-K: PackFGFX (Crystal only), else 0    */

  /* BACKLOG #99: Gen 1's key-item bit table (IsKeyItem_'s KeyItemFlags),
   * located BY SHAPE the same way every other slot here is -- NOT a required
   * slot: 0 means "not located this open()", and the caller
   * (source/pdna_gbbag.c) falls back to gbb_is_g1_key_item()'s factual id
   * list. `g1_keyitems` is the table's own file offset (kept for save_loc()/
   * revalidation, mirrors every other off[]-style field); `g1_keyitems_bits`
   * is the CopyData-bounded 15 B (120 bits, ids 1..120 -- ld bc,$000F is
   * part of the anchor signature itself) read out of it once at open()/
   * open_loc() time, so rom_gbui_g1_key_item() below needs no live ROM read
   * (gu->read/ctx are not guaranteed live at every call site that wants a
   * key-item answer -- see pdna_gbscreen.h's own "STALE between..." note). */
  uint32_t g1_keyitems;
  uint8_t  g1_keyitems_bits[15];

  uint8_t  playerpic_bank;       /* the bank playerpic's blob lives in       */
  uint8_t  cardpic_colmajor;     /* 1 = Crystal-shaped (rgbgfx --columns), 0 = Gold (row-major) */
  int      ok;

  /* BACKLOG #71: the scan-hit file offset that DERIVED each off[]-equivalent
   * field above (see ROM_GBUI_ANCH_* / RomGbUiLoc.anchor). Carried on RomGbUi
   * itself -- not a hidden/static shadow -- so rom_gbui_save_loc() can emit
   * an exact loc with no globals and no state that outlives one open() call.
   * Meaningless (0) for a field this gen doesn't use; see the enum. */
  uint32_t anchor[ROM_GBUI_ANCH_COUNT];
} RomGbUi;

/* Everything open() discovered, in a form a caller can write to the SD and
 * hand back next time to skip the whole-ROM scan. Validate-on-load is
 * automatic: open_loc() re-checks id_hash, size, and re-runs each field's own
 * structural verifier before trusting it.
 * Field order matches RomGbUi's: font, textbox, cardframe, badges, leaders,
 * playerpic, frames, fontextra, cardpic_m, cardpic_f, cardgfx, pack_m, pack_f.
 *
 * BACKLOG #71: `off[]` alone made revalidation STATISTICAL, not exact -- a
 * corrupted-but-recomputed-check loc (e.g. off[FONT]+16, with `check`
 * recomputed to match) still passes font_verify(), which only asks "does
 * 1024 B here look font-shaped", not "is this THE font". `anchor[]` fixes
 * that: each entry is the FILE OFFSET of the code (or, for two data-table
 * signatures, the data) the locator originally matched to DERIVE that
 * offset -- i.e. the scan hit, not the block itself. revalidate_loc()
 * re-reads the signature bytes at the cached anchor, confirms the opcode/
 * data run still matches byte-for-byte, decodes the address/bank operand(s)
 * exactly as locate()'s try_*() functions do, and rejects unless that
 * recomputation lands on the EXACT cached block offset. A shifted-but-
 * structurally-plausible off[] can no longer survive: the anchor's own
 * operand math would have to independently agree with the corruption, which
 * requires forging the ROM's code bytes, not just the cache record.
 * See ROM_GBUI_ANCH_* below for which anchor derives which off[] entr(y/ies)
 * -- some off[] entries have no anchor of their own because they are
 * DERIVED from a sibling's anchor (font=frames-1536, fontextra=font-512,
 * cardpic_f=cardpic_m+0x230, cardgfx from the same cardpic anchor, G1's
 * badges=anchor+8): revalidate_loc() re-derives those from the anchor that
 * does exist rather than storing a redundant anchor for them. */
enum {
  ROM_GBUI_OFF_FONT = 0, ROM_GBUI_OFF_TEXTBOX, ROM_GBUI_OFF_CARDFRAME,
  ROM_GBUI_OFF_BADGES, ROM_GBUI_OFF_LEADERS, ROM_GBUI_OFF_PLAYERPIC,
  ROM_GBUI_OFF_FRAMES, ROM_GBUI_OFF_FONTEXTRA, ROM_GBUI_OFF_CARDPIC_M,
  ROM_GBUI_OFF_CARDPIC_F, ROM_GBUI_OFF_CARDGFX, ROM_GBUI_OFF_PACK_M,
  ROM_GBUI_OFF_PACK_F,
  /* BACKLOG #99: Gen 1's key-item bit table, optional (see RomGbUi.g1_keyitems
   * above -- a miss does not fail Gen 1's `ok`). */
  ROM_GBUI_OFF_G1_KEYITEMS,
  ROM_GBUI_OFF_COUNT
};

typedef struct {
  uint32_t id_hash, size;
  uint8_t  gen, pad[3];
  uint32_t off[ROM_GBUI_OFF_COUNT];     /* 56 B (BACKLOG #99 grew this from
                                          * 52: one new optional G1_KEYITEMS
                                          * slot) */
  uint32_t anchor[ROM_GBUI_ANCH_COUNT];  /* 44 B (BACKLOG #99 grew this from
                                           * 40); unused (gen-inapplicable)
                                           * slots are 0 */
  uint32_t check;                /* FNV-1a over off[] THEN anchor[]: catches a
                                   * corrupted cached offset OR a corrupted
                                   * cached anchor before either is trusted */
} RomGbUiLoc;

/* Identify the ROM and locate its UI tables. `scratch` is the caller's scan
 * window (>= ROM_GBUI_SCRATCH_MIN; bigger scans faster) and is used ONLY
 * inside this call -- it may be released or reused immediately afterward.
 * Returns 1, or 0 (fail closed) for a non-GB image, a truncated one, or a ROM
 * whose required tables (see the "FAIL CLOSED" note above) are missing,
 * ambiguous, or fail their structural verification. */
int rom_gbui_open(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                  uint8_t* scratch, uint32_t scratch_len);

/* Same, but start from a cached RomGbUiLoc. The header is still parsed and
 * the cache is REJECTED (falling back to a full scan) unless its id_hash and
 * size still match and every offset it names still passes that field's own
 * structural re-verification. */
int rom_gbui_open_loc(RomGbUi* gu, GbReadFn read, void* ctx, uint32_t size,
                      uint8_t* scratch, uint32_t scratch_len,
                      const RomGbUiLoc* loc);

/* Snapshot what open() found. Safe to call only when gu->ok. */
void rom_gbui_save_loc(const RomGbUi* gu, RomGbUiLoc* out);

/* Expand one 8x8 tile at ROM `off + index*stride` (stride = 16 for bpp==2,
 * 8 for bpp==1) into `out[64]`, row-major, RGB15, the DMG 4-shade ramp
 * {0xF8,0xA8,0x58,0x10} (8-bit grey, expanded to RGB15 by >>3 per channel).
 *
 * grid_w/grid_h = the block's tile grid; 0,0 = raw storage index, no bound.
 * colmajor: `index` is a ROW-MAJOR DISPLAY index (row*grid_w+col) and the
 * storage index is col*grid_h+row (rgbgfx --columns); requires the grid.
 * Only Crystal's card pics (chris_card.2bpp/kris_card.2bpp, 35 tiles = 5x7)
 * are stored column-major -- Gold's ChrisPicAndTrainerCardGFX pic shares the
 * same 5x7 shape but is plain ROW-major (see RomGbUi.cardpic_colmajor,
 * docs/GB-GAME-SCREENS-DESIGN.md 2.2 G2-P). Every other block this module
 * locates (font, textbox, frames, badges, leaders, pack) is row-major.
 * Returns 1, or 0 for a NULL/unopened `gu`, a bad bpp, an out-of-grid
 * `index`, colmajor requested without a grid, or a read past the ROM. */
int rom_gbui_tile(RomGbUi* gu, uint32_t off, uint32_t index, uint8_t bpp,
                  uint32_t grid_w, uint32_t grid_h, int colmajor, uint16_t out[64]);

/* One glyph of the located font: `ch` is the GAME's own charmap code (A=0x80,
 * a=0xA0, 0=0xF6 -- constants/charmap.asm), tile = ch - 0x80. Returns 1, or 0
 * if `gu` has no font (Gen 1 & Gen 2 both do when `ok`) or `ch` is outside
 * 0x80..0xFF. */
int rom_gbui_glyph(RomGbUi* gu, uint8_t ch, uint16_t out[64]);

/* BACKLOG #99. True iff `id` (1-based Gen-1 item id) sets its bit in the
 * LOCATED KeyItemFlags table (see RomGbUi.g1_keyitems_bits above). Needs no
 * live ROM read (the table was cached at open()/open_loc() time) -- the byte
 * test is exactly IsKeyItem_'s own shape: bit (id-1)&7 of byte (id-1)>>3,
 * LSB-first. Returns false (never a guess) when `gu` is NULL, not Gen 1, not
 * `ok`, has no located table (g1_keyitems == 0 -- the caller's own job is to
 * fall back to gbb_is_g1_key_item() in that case, not this function), or `id`
 * is 0 or beyond the table's own code-bound length (120 = 15 B * 8 bits --
 * ld bc,$000F in IsKeyItem_ itself, not the "11 bytes actually used" comment
 * NUM_ITEMS derives: this locator trusts what the ROM's own CopyData call
 * bounds, not a public-knowledge item count). This function makes no HM
 * exception: ids 0xC4..0xC8 (HM01..HM05) are past the 120-id bound and so
 * always return false here too, even though the real game treats them as
 * key items via a path that never touches KeyItemFlags at all (IsKeyItem_
 * branches to IsItemHM first, pokered engine/items/item_effects.asm:~2616).
 * Deciding the HM range is the CALLER's job (see source/gb_bag.c's
 * gbb_g1_key_item_compose(), which checks it before ever asking this
 * function) -- the table is silent above id 120 by construction and this
 * locator does not special-case anything it didn't locate. */
bool rom_gbui_g1_key_item(const RomGbUi* gu, uint8_t id);

#endif /* ROM_GBUI_INCLUDED */
