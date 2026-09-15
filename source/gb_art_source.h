#ifndef GB_ART_SOURCE_H
#define GB_ART_SOURCE_H

#include <stdint.h>
#include <stdbool.h>

/* Per-gen resident path cap: half of PATH_MAX(256), see the memory note below for why. */
#define GB_ROM_PATH_MAX 128

/* E3 review re-verification (2026-09-06): the worst measured cost of taking the GB
 * rung from pdna_origin_art_portrait() on down (gb_art_pic_cb -> gb_art_fetch ->
 * rom_gbsprite_pic_buf/rom_gbsprite_pal -> gb_art_read -> the SD read tail), read off
 * the call-graph tool as gb_art_pic_cb's own total. Rounded UP from the measured
 * number (not down) -- see gb_art_boot_register()'s stack-room hook for the exact
 * chains it was checked against. D4 (E4 review): pdna_origin_art.c's portrait router
 * also passes this need directly for its two OWN GB-rung call sites (the era==GEN1/
 * GEN2 branch and the native-GB-import branch) -- the hook itself is measured per
 * rung now, not once globally, but both of those rungs cost exactly this GB fetch. */
#define PDNA_GB_FETCH_NEED 6144

/* E5: the Gen-2 menu-ICON rung's own measured need, parallel to PDNA_GB_FETCH_NEED
 * above but for gb_art_icon_cb -> gb_art_fetch_icon -> rom_gbicon_* -> the SD read
 * tail, instead of the portrait rung's rom_gbsprite_* chain.
 *
 * MEASURED (arm-none-eabi-gcc -O2 -fstack-usage, 2026-09-06), and the number is a
 * genuine surprise against this slice's own first-draft comment here (which
 * guessed "far below" PDNA_GB_FETCH_NEED before anything was measured): gb_art_icon_cb's
 * own total is 5,592 B, essentially TIED with gb_art_pic_cb's 5,632 B, not
 * meaningfully smaller. The reason is that neither number is dominated by the
 * decode this slice actually changed -- gb_art_fetch_icon's own frame (3,544 B) is
 * only marginally under gb_art_fetch's (3,672 B), because BOTH chains bottom out in
 * the exact same shared FatFs tail (f_open -> dir_register -> dir_find -> dir_read
 * -> load_xdir -> dir_next -> create_chain -> fill_last_frag, all identical code
 * either way), which is what actually costs the bulk of the ~5.6 KB, not the
 * RomGbIcon-vs-RomGbSprite or the aliasing-trick difference this slice expected to
 * matter. So this is deliberately NOT a smaller gate than PDNA_GB_FETCH_NEED (the
 * measurement does not support one) -- it is its own named constant, currently
 * equal, so that if a future change genuinely shrinks one rung's cost independently
 * of the other, re-measuring only has to touch the constant that actually moved.
 * MEASURED at 4aa9de8 (E5 re-verify): the chain below the gate (icon_cb + fetch_icon +
 * fetch_pic_ex, incl. rom_gbicon_open_loc's 608-B frame after its D3 re-validation
 * window) is 5,960 B -- only 184 B under this constant. One more 256-B buffer anywhere
 * under gb_art_fetch_icon breaks the promise: re-measure before adding one. */
#define PDNA_GB_ICON_NEED 6144

/*
 * gb_art_source — the FIRST real caller of rom_gbsprite.c (source/rom_gbsprite.h), and
 * the GB half of pdna_origin_art's `PdnaGbArtSource` vtable (never registered by
 * anyone until this file, per docs/SPRITE-ERA-DESIGN.md Sec 0/2, slice E3).
 *
 * OWNERSHIP: this module is the ONLY thing that opens a FatFs handle over a Gen-1/2
 * ROM. It knows nothing about config.cfg's TEXT FORMAT (pdna_main.c owns that, exactly
 * like the existing romrs/romem/romfr keys) — pdna_main.c hands this module a path via
 * gb_art_register() and this module answers "does it work" / "here is a picture".
 *
 * MEMORY, measured against docs/SPRITE-ERA-DESIGN.md Sec 0's numbers (EWRAM 524 B
 * free, stack ~11.5 KB with the portrait-fetch chain already ~3.2 KB deep):
 *
 *   - E3 REVIEW BLOCKING 1 (2026-09-06), fixed: the first cut of this module put a
 *     3,928 B GbSprite on gb_art_decode()'s own stack frame, nested UNDER
 *     gb_art_fetch()'s (FIL+RomGbSprite+scratch), for 7,640 B of one fetch call --
 *     and the reviewer's call-graph tool (whole-program .su + disassembly, not a
 *     per-function guess) proved that plus the real caller chains overran the
 *     ~11.5 KB user stack by 400-1,800 B depending on which screen. FIXED by decoding
 *     IN PLACE inside mon_decomp (8,192 B) instead: rom_gbsprite_pic_buf() (new,
 *     rom_gbsprite.h) writes the indexed pixels at mon_decomp+ROM_GBSPRITE_MAX_PIXELS
 *     (3,136) and the codec's own work[] scratch right after that
 *     (mon_decomp+ROM_GBSPRITE_RGB15_BYTES, 6,272), and
 *     rom_gbsprite_to_rgb15_inplace() (new) expands those SAME bytes back into
 *     mon_decomp+0 as RGB15 -- see rom_gbsprite.h for the full margin proof (px_off
 *     must be >= the worst-case pixel count, which GB_SPRITE_MAX_PX exactly is) and
 *     tests/host_romgbsprite_test.c's 427-picture cross-check against the original
 *     two-buffer path (every 7x7 sprite in all four of Guy's dumps, the exact
 *     coincidence boundary). There is now no separate GbSprite at all: gb_art_fetch()
 *     is ONE noinline frame (FIL 600 B, RomGbSprite ~336 B, RomGbSpriteLoc ~260 B,
 *     the 2,048 B scan window, the path buffer) -- rom_gbsprite_open()/open_loc()
 *     require that scratch unconditionally regardless of a loc cache hit (Gen-2's
 *     palette verify reads up to 2,016 B through it even then), so it could not be
 *     made scan-path-only the way an earlier estimate assumed.
 *
 *     MEASURED (arm-none-eabi-gcc -O2 -fstack-usage + a disassembly call-graph walk,
 *     2026-09-06, after the fix): gb_art_fetch() 3,672 B own frame (the SD read tail
 *     inside rom_gbsprite_pic_buf -> gb_art_read -> f_lseek/f_read -> disk_read ->
 *     ed_sd_dma_to_rom adds ~1.9 KB more when it's the deepest reachable child --
 *     included below, not a separate number to add). The three real call chains the
 *     review named: era_cells() [box repaint] 5,872 B, draw_left() [box hover] 5,960
 *     B, summary_run() [summary portrait] 6,728 B -- all comfortably under the
 *     ~11.5 KB budget (4.8-5.6 KB margin each), a large improvement on the review's
 *     own ~8.0-8.5 KB targets for the same three chains once gb_art_have()'s "beside
 *     the save" fallback was ALSO made a cheap existence check (gb_rom_path_beside()
 *     alone, 640 B) instead of a full open+identify (2,992 B) -- see gb_art_have()'s
 *     own comment for why a second full validation there bought nothing: gb_art_fetch
 *     re-validates for real a moment later regardless.
 *
 *     ONE CHAIN WAS FOUND OVER BUDGET, independently of the review's three named ones
 *     while proving them: main()'s absolute worst reachable path is Bank -> box ->
 *     the party strip overlay (offered over the Bank because is_bank is false there,
 *     the same "To Day-Care" row app_to_daycare wires up) -> the party menu -> the mon
 *     menu -> Daycare -> inspect-a-deposited-mon -> its summary -> the portrait
 *     fetch. It measures 7,880 B of stack in use with the ORDINARY Gen-3 rung --
 *     i.e. it does NOT overflow today -- and 13,200 B with the GB rung: E3's own
 *     addition (+5,320 B) is the ENTIRE reason this chain would overflow, not any
 *     pre-existing problem in the Daycare/party-strip/mon-menu nesting itself.
 *
 *     FIXED (not flagged) by a stack-headroom GATE rather than by touching that
 *     nesting: pdna_origin_art_stack_room() (pdna_origin_art.h/.c) is a hook
 *     pdna_origin_art_portrait() consults immediately before taking the GB branch,
 *     defaulting to "yes, there is room" (so the host build and every existing host
 *     test keep today's behaviour with zero code changes) unless something registers
 *     a real check. gb_art_boot_register() registers exactly that: gb_art_stack_room()
 *     reads the CPU's own SP against `__iheap_start` (the SAME linker low-water-mark
 *     idiom the build's own post-link EWRAM guard uses at the other end of memory)
 *     and refuses when fewer than PDNA_GB_FETCH_NEED (6,144 B -- gb_art_pic_cb's own
 *     measured worst-case total) bytes remain. On the Daycare chain (7,384 B already
 *     in use by the time the check runs, ~4,112 B left) that refuses and
 *     pdna_origin_art_portrait() falls through to gen3_ladder -- out->era/era_certain
 *     were already set from origin detection before the check, so the tag/pad stay
 *     honest ("this IS a Game Boy import") even though the pixels degrade. On the
 *     three named chains (summary 3,312 B in use, box era-cells ~2,880 B, box hover
 *     ~2,864 B) it allows the GB rung exactly as before. tests/host_originart_test.c's
 *     part C (C5b) proves the default ("always room") and an injected "no room" hook
 *     both behave correctly, including that a refused fetch never touches the GB
 *     source at all. docs/HW-TEST-2026-09-05-GB-ARC.md §J9(b) is the real-hardware
 *     regression check that the gate actually closes this chain in practice.
 *   - The two registered ROM paths are genuine EWRAM_BSS residents, because
 *     cfg_save() rewrites config.cfg FROM RESIDENT STATE on almost every browser
 *     keypress — a path that only round-tripped through a file could not survive
 *     that rewrite. As of E3 review item 3, they live in the SAME array as the three
 *     Gen-3 map-screen ROM paths (pdna_main.c's g_rom_path[5][GB_ROM_PATH_MAX], PkGame
 *     RS/Emerald/FRLG at indices 0-2, PDNA_GEN1/2 at 3-4 via gb_gen_slot()) — merging
 *     two separate arrays (g_rom_path[3][PATH_MAX] + a since-deleted
 *     g_gb_rom_path[2][GB_ROM_PATH_MAX], 1,024 B together) into one, ALL FIVE slots
 *     now capped at GB_ROM_PATH_MAX (128, not PATH_MAX's 256 — the same per-feature-
 *     cap idea gb_sidecar.h already uses, GBSC_PATH_MAX 48), freed 384 B: measured
 *     268 -> 652 B EWRAM free. A path too long for any of the five slots is REFUSED
 *     at set-time (app_rom_path_set()/app_gb_rom_path_set() both return false), never
 *     silently truncated — cfg_load() rejects (and logs) an over-length saved value
 *     the same way, instead of the strncpy(..., PATH_MAX-1) it used to silently clip
 *     with (which would have been an actual out-of-bounds write once the array
 *     shrank to GB_ROM_PATH_MAX-wide rows).
 *   - The located-table cache (RomGbSpriteLoc, ~260 B) is NOT resident anywhere: it is
 *     read from a small per-gen file at EVERY fetch (a session-scoped path is opened
 *     rarely — the router memoises, so this is once per distinct species view, not
 *     once per frame) rather than costing another 520 B of EWRAM for two copies.
 *   - HARDWARE-ONLY PERFORMANCE NOTE (E3 review item 9, not a stack or EWRAM
 *     concern): pdna_box.c's era_cells() calls pdna_origin_box_art() once per GRID
 *     SLOT on a full box repaint — up to 30 times — and every GB-origin slot with no
 *     memo hit is a full gb_art_fetch(): f_open() the registered/fallback ROM, read
 *     its .loc cache, rom_gbsprite_open_loc()'s validation reads, decode, close. In
 *     the worst case (a box full of DIFFERENT GB-origin species, so the router's
 *     one-picture memo cannot help) that is up to 30 file opens and their FAT-walk
 *     cost on ONE repaint. The natural fix — a session-scoped FIL + RomGbSprite kept
 *     open across the whole box visit instead of opened fresh per fetch — is an I/O
 *     win only, not a stack win (it does not change gb_art_fetch's own frame size or
 *     depth), so it is out of scope for this slice's stack-safety fix and left for
 *     whoever next profiles box-repaint latency on real hardware.
 *     docs/HW-TEST-2026-09-05-GB-ARC.md §J's box-grid test should include a box FULL
 *     of distinct GB-origin mons as the worst-case repaint-latency case this note
 *     predicts, not just a couple of cells.
 */

/* Register (or, with an empty/NULL path, clear) generation `gen`'s (1 or 2) art
 * source. Opens `path`, identifies it as a Gen-1 or Gen-2 Game Boy ROM (fail closed:
 * a wrong generation, a non-GB image, or unreadable tables all refuse) and, on
 * success, writes a location cache for gb_art_fetch's later per-fetch scratch-saving
 * reads and lights up gb_art_have(gen). Idempotent: safe to call again with the same
 * or a different path (e.g. on every boot, or every time Settings changes it).
 *
 * Does NOT touch config.cfg and shows no UI — the caller (pdna_main.c) persists the
 * path (or "") and reports the result. A no-op under PDNA_DELTA (no SD to open),
 * where gb_art_have() always answers false. */
typedef enum {
  GB_ART_REG_OK = 0,        /* opened, identified as generation `gen`, cached          */
  GB_ART_REG_EMPTY,         /* path was "" or NULL -- gen `gen` cleared, not an error   */
  GB_ART_REG_CANT_OPEN,     /* the file would not open for read                        */
  GB_ART_REG_WRONG_GEN,     /* opened and identified, but as the OTHER generation       */
  GB_ART_REG_BAD_ROM,       /* opened, but not a valid/locatable Game Boy image        */
  GB_ART_REG_CANCELLED,     /* the progress callback said stop (B) -- nothing persisted */
  GB_ART_REG_TIMEOUT,       /* one locator exceeded GB_ART_SCAN_LIMIT_S -- nothing persisted */
  GB_ART_REG_READ_ERR       /* an SD read failed (GbArtRegInfo has fr/err/offset) --  *
                             * nothing persisted                                       */
} GbArtRegStatus;

/* Which locator a progress report is about. Gen 1 only ever runs the first. */
#define GB_ART_LOC_SPRITES 1
#define GB_ART_LOC_ICONS   2

/* Per-locator wall-clock limit. The clock is perf_ticks() (a hardware timer that
 * keeps counting while the EZ-Flash driver holds IRQs off), not a VBlank frame count
 * -- no VBlank IRQ is delivered during a transfer, which is exactly when a stuck
 * card needs the limit to fire. See gb_scan_guard.h. */
#define GB_ART_SCAN_LIMIT_S 60u

/* Progress callback, invoked from INSIDE the ROM read shim between SD reads (never
 * during a transfer): every 8th read, and on the first. `done`/`total` are bytes
 * (the high-water mark of the file covered so far, and the ROM size). Return false
 * to cancel: the current read is not performed, the locator unwinds, and
 * gb_art_register() answers GB_ART_REG_CANCELLED having written nothing. The
 * callback may poll input and draw -- both are safe between transfers -- but it
 * runs at the bottom of the locator's call chain, so keep its own frame small
 * (pdna_main.c's gb_reg_progress is the measured reference). */
typedef bool (*GbArtProgressFn)(void* ctx, uint8_t locator, uint32_t done, uint32_t total,
                                uint32_t elapsed_ms);

/* What happened, for the caller's message and the log. Filled on every outcome. */
typedef struct GbArtRegInfo {
  uint32_t reads;        /* SD reads performed, both locators together              */
  uint32_t elapsed_ms;   /* wall time from open to close                             */
  uint32_t covered;      /* bytes of the ROM covered when it stopped (progress `done`) */
  uint32_t fail_off;     /* file offset of the failing read (READ_ERR only)          */
  uint8_t  locator;      /* GB_ART_LOC_* that was running when it finished/stopped   */
  uint8_t  fr;           /* FatFs FRESULT of the failing call (READ_ERR only)        */
  uint8_t  err;          /* FIL.err latched after it (READ_ERR only)                 */
  uint8_t  stop;         /* gb_scan_guard.h GB_SCAN_* reason (0 = none)              */
} GbArtRegInfo;

/* `progress`/`progress_ctx` may be NULL (silent: boot, tests); `info` may be NULL.
 * Reads the existing /PokeDNA/gbart<gen>.loc FIRST and only scans the whole ROM
 * when that cache does not describe this exact file (a fresh path, a swapped
 * ROM, or no cache yet) -- so re-registering the same ROM, and every boot, costs a
 * few dozen reads, not a 2 MB scan. For a Gen-2 ROM the menu-icon tables are
 * located too (locator 2/2) and /PokeDNA/gbicon2.loc written alongside, so the
 * first party/box icon after registration does not pay a scan of its own. */
GbArtRegStatus gb_art_register(uint8_t gen, const char* path, GbArtProgressFn progress,
                               void* progress_ctx, GbArtRegInfo* info);

/* Does generation `gen` (1 or 2) currently have working art? An explicit
 * gb_art_register() (Settings/boot) always wins outright once it has run. Failing
 * that, and only while a raw GB save of some kind is open, this ALSO lazily tries the
 * "beside the save" fallback exactly once per save-open (gb_rom_path_beside() +
 * gb_art_register()-equivalent validation) and caches that verdict until
 * gb_art_session_reset() clears it for the next save — so this is cheap on every call
 * AFTER the first for a given save, but the first call for a fallback-served save
 * really does open and scan the ROM. This is what pdna_origin_art's vtable `have`
 * callback answers, and what app_gb_rom_registered() (pdna_app.h) exposes for E4's
 * SeRoms — that accessor reports only the EXPLICIT-registration half, not a fallback
 * that could flip per save. */
bool gb_art_have(uint8_t gen);

/* Wires this module into pdna_origin_art's GB rung (pdna_origin_art_register()) and,
 * for each of romgb1/romgb2 that cfg_load() restored a non-empty path for
 * (app_gb_rom_path()), re-validates it via gb_art_register() so a fresh boot has
 * working art with no user action. Call once, after cfg_load(), from main()'s
 * startup — a no-op (source never registered) under PDNA_DELTA. `progress` is
 * shown for the (rare) boot that has to rescan -- a missing or stale .loc; the
 * common boot validates the cache in a few dozen reads and the screen barely
 * flashes. NULL = silent. */
void gb_art_boot_register(GbArtProgressFn progress, void* progress_ctx);

/* Reset the "beside the save" fallback's per-save cache. Call from view_save()
 * (pdna_main.c) the moment a save is opened, BEFORE anything asks gb_art_have() —
 * otherwise a fallback hit/miss cached against the PREVIOUS save would leak into this
 * one. Explicit Settings/boot registrations (gb_art_register()) are untouched: they
 * don't depend on which save is open. */
void gb_art_session_reset(void);

/* The session-only "ROM beside the save" fallback (docs/GEN12... S5-C's rule, reused
 * here rather than refactored out of pdna_gen12.c's gb_gen1_locate_rom() — that
 * function is tested, arena-resident, paste-path code this slice deliberately does not
 * touch; this is a parallel, independent implementation of the same two-extension
 * probe). Tries "<save_path's dir+basename>.gb" then "...gbc" (same order regardless
 * of `gen`, since a Gen-1 dump can carry either extension), opens+closes each candidate
 * just to prove it exists and is readable, and copies whichever wins into `out[cap]`.
 * Returns false (out untouched) if neither exists. Does no ROM validation itself —
 * gb_art_register()/gb_art_fetch() still decide whether the file is actually a
 * generation-`gen` Game Boy image. A no-op (returns false) under PDNA_DELTA. */
bool gb_rom_path_beside(const char* save_path, uint8_t gen, char* out, int cap);

#endif /* GB_ART_SOURCE_H */
