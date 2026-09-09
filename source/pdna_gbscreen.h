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
 * only plain data (the located RomGbUi, the tilemap, the ROM's resolved path or
 * fused base/size) and gbscr_open()/gbscr_flush() each rebind a LOCAL RomGbUi
 * copy's `ctx`/`read` to a transient FIL (SD build) or FusedGbSlice (delta build)
 * for the duration of that one call, exactly the way rom_gbui_tile()/glyph()'s own
 * caller-owned-ctx contract expects. Nothing here is EWRAM_BSS: GbScreen is meant
 * to live on ONE caller's own stack frame (a `noinline` screen/demo function), the
 * same "one noinline frame" posture gb_art_source.c's gb_art_open_and_identify()
 * and gb_art_fetch() already use.
 *
 * MEASURED (arm-none-eabi-gcc -mcpu=arm7tdmi -mtune=arm7tdmi -O2 -mthumb-interwork
 * -mthumb -fstack-usage -c source/pdna_gbscreen.c, 2026-09-09, U2a fix batch
 * D1/D2/D8): own-frame sizes (bytes) -- gbscr_mark_all_dirty 8, gbscr_cell 16,
 * gbscr_text 56, gbscr_raw 48, gbscr_toggle_scale 8, gbscr_sd_read 24,
 * gbscr_open 16, gbscr_open_inner 3624, gbscr_close 0, gbscr_flush 8 (gcc
 * split the real body into a separate gbscr_flush.part.0, 952 -- corrected
 * from an earlier note's 944), gbscr_run_demo 1016. sizeof(GbScreen) = 976 B
 * (SD build) / 856 B (delta build) -- GbScreen itself lives on the CALLER's
 * own frame (gbscr_run_demo's 1,016 B above, or a future U2b card screen's),
 * OUTSIDE the gate; it is never counted in PDNA_GB_UI_NEED.
 *
 * D1 fix (review): the stack-room gate used to live INSIDE gbscr_open()'s own
 * frame, so it measured the room LEFT UNDER a 3,624-B frame that already
 * existed by the time the check ran, instead of the room the frame ITSELF
 * needs -- on the artless/SD build this always under-counted by exactly this
 * frame's size and made the gate refuse every time on real hardware. The old
 * body is now `gbscr_open_inner()` (static, noinline, unchanged except the
 * stack_room() call is gone); `gbscr_open()` is a thin (16-B) wrapper that
 * checks pdna_origin_art_stack_room(PDNA_GB_UI_NEED) BEFORE calling it, so the
 * gate now runs before gbscr_open_inner()'s frame is ever allocated. PDNA_GB_
 * UI_NEED (rom_gbui.h) is measured from gbscr_open_inner()'s OWN entry down --
 * 3,624 (this frame) + 2,840 (rom_gbui_open_loc's own worst nested chain,
 * rom_gbui.h's own measurement) = 6,464 (D2: taken as measured, NOT rounded
 * up -- see rom_gbui.h's own note for why) -- NOT gbscr_run_demo's 1,016 B on
 * top (a different caller, e.g. U2b's real card screen, will have a different
 * frame of its own; the gate is caller-independent by design, exactly like
 * PDNA_GB_FETCH_NEED/PDNA_GB_ICON_NEED are each measured per-rung rather than
 * accumulated across every possible caller).
 * gbscr_flush() carries no separate gate -- it never calls
 * pdna_origin_art_stack_room() -- because its own reachable chain (952 B, plus
 * rom_gbui_tile()/glyph()'s own small per-tile-fetch frames, no locate()/
 * distinct_tiles() on that path) is comfortably smaller and the design (sec
 * 3.4/R9) only requires gating the OPEN path.
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
  GBSCR_SRC_PIC
} GbScrSrc;

typedef struct {
  RomGbUi  gu;                 /* located offsets; .ctx/.read are STALE between
                                 * calls -- gbscr_flush() rebinds a local copy */
  uint8_t  map[GBSCR_CELLS];   /* per-cell tile index / charmap byte           */
  uint8_t  src[GBSCR_CELLS];   /* per-cell GbScrSrc                            */
  uint8_t  dirty[GBSCR_DIRTY_BYTES];
  uint8_t  gen;                /* PDNA_GEN1 / PDNA_GEN2                        */
  bool     ok;                 /* gbscr_open() succeeded; gbscr_* are no-ops otherwise */
#ifndef PDNA_DELTA
  char     rom_path[GB_ROM_PATH_MAX];
#else
  const uint8_t* rom_base;
  uint32_t rom_size;
#endif
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
 * design R1 -- this function adds no separate check on top).
 * On success: zeroes the tilemap (every cell BLANK), returns true.
 * On refusal: `*reason` (may be NULL) is set to a short, static, user-facing
 * string ("no ROM registered" / "not enough stack" / "could not open ROM" /
 * "not an English release" / "not a Game Boy ROM"), `gs->ok` is false, and every
 * other gbscr_* call on `gs` is a safe no-op. */
bool gbscr_open(uint8_t gen, GbScreen* gs, const char** reason);

/* Release any resources gbscr_open() took (the SD build's FIL is already closed
 * by the time gbscr_open() returns -- this exists for symmetry/future-proofing
 * and to make the shell's own lifetime explicit at call sites). Safe on a `gs`
 * that never opened successfully. */
void gbscr_close(GbScreen* gs);

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

/* Names already stored in GB encoding (gb_trainer's name_raw) go straight in, one
 * byte per cell, no ASCII step -- same 0x7F-is-blank rule as gbscr_text(). Stops
 * at `n` bytes (the caller's own field width); does NOT stop at the 0x50
 * terminator (a raw name field is exactly `n` bytes on a real cartridge, callers
 * that need to stop early should pass the trimmed length). */
void gbscr_raw(GbScreen* gs, int x, int y, const uint8_t* bytes, int n);

/* Mark every cell dirty (a full repaint) -- used after gbscr_toggle_scale() and
 * on a screen's first flush. */
void gbscr_mark_all_dirty(GbScreen* gs);

/* Flip gb_scale_mode, persist it is the CALLER's job (pdna_main.c's cfg_save());
 * this only flips the in-memory flag and marks the whole canvas dirty so the very
 * next gbscr_flush() repaints everything in the new mode. */
void gbscr_toggle_scale(GbScreen* gs);

/* Repaint every dirty cell into the Mode-3 framebuffer in the CURRENT gb_scale_mode,
 * then draw the legend (design sec 1.5: two stacked side-bar columns at 1:1, a
 * bottom scrim overlay when stretched) and clear the dirty bitmap. `legend_extra`
 * (may be NULL) is one more screen-supplied key line appended after the shell's
 * own A OK / B BACK / SEL SIZE rows. GBA-only (touches vid_mem/tonc); a no-op if
 * `gs` never opened. */
void gbscr_flush(GbScreen* gs, const char* legend_extra);

/* U2a's own DEMO screen: opens the shell for `gen`, draws the located font's
 * whole 128-glyph sheet inside a text-box-tile border plus one line of text, and
 * lets A-OK/B-BACK/SELECT-toggle-scale drive it -- so the shell can be shot
 * standalone (both scale modes) before any real card (U2b) exists. Reachable
 * from Settings (SELECT, a hidden key -- see pdna_main.c's pdna_settings()) in
 * every build; refuses with a message box on gbscr_open()'s own reason string
 * when no ROM/stack/English release is available. GBA-only (drives wait_keys/
 * msg_wait), not part of the pure host-testable half. */
void gbscr_run_demo(uint8_t gen);

#endif /* PDNA_GBSCREEN_H */
