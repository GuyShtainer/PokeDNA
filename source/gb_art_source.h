#ifndef GB_ART_SOURCE_H
#define GB_ART_SOURCE_H

#include <stdint.h>
#include <stdbool.h>

/* Per-gen resident path cap: half of PATH_MAX(256), see the memory note below for why. */
#define GB_ROM_PATH_MAX 128

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
 *   - The GbSprite decode buffer rom_gbsprite.h's own "WHAT IT COSTS" section
 *     recommends app_arena_acquire() for (it and mon_decomp together are ~10 KB) is
 *     UNAVAILABLE here: pdna_gen12_show() holds that exact arena for the ENTIRE GB
 *     save-viewing session (pdna_gen12.c:1895, released only after gb_session_core()
 *     returns), and box_oam.c's OTHER borrowed cache (g_entries, APP_BOX_SWAP_BYTES)
 *     is held for the entire box-screen visit too (boxoam_enter()/exit()) — both of
 *     which are exactly the screens this feature has to work DURING. So the 3,928 B
 *     GbSprite lives on the STACK of the fetch helper instead, alongside the FIL
 *     (600 B), RomGbSprite (~336 B) and the 2,048 B scan window
 *     ROM_GBSPRITE_SCRATCH_MIN rom_gbsprite_open()/open_loc() both require
 *     unconditionally (rom_gbsprite.c:434/468 — NOT only on a cache-miss scan: Gen-2's
 *     own palette verify reads up to G2_PAL_BYTES = 2,016 B through the SAME scratch
 *     even on a loc cache HIT, so this module cannot shrink it to "scan-path only" the
 *     way an earlier design estimate assumed — that assumption did not survive contact
 *     with rom_gbsprite_open_loc()'s real signature and was corrected here).
 *
 *     MEASURED (arm-none-eabi-gcc -O2 -fstack-usage, 2026-09-06): gb_art_fetch() 3,656 B
 *     own frame, gb_art_decode() 3,984 B (the two are nested, not concurrent siblings —
 *     fetch calls decode — so they ADD: 7,640 B for one portrait fetch). Walking the
 *     real box-grid call chain the same way: main() 2,384 B (view_save()/nav_menu()
 *     both single-call-site and inlined into it) -> pdna_box() 488 B -> draw_left()
 *     144 B -> pdna_origin_art_portrait() 128 B (fetch_pic() inlined into it) ->
 *     gb_art_pic_cb() 24 B -> gb_art_fetch()+gb_art_decode() 7,640 B = **10,808 B of
 *     the ~11,600 B user stack** (IWRAM 32,768 B minus the 21,008 B .text/.bss this
 *     build's `main` region uses, minus the linker script's 0xA0 __sp_usr/__sp_irq
 *     reservation; IRQ mode has its own separate 0xA0 B stack per gba_cart.ld, so
 *     interrupts do not add to this). That is a ~792 B margin, not a comfortable one —
 *     summary-screen portrait_redraw() (264 B own frame, shallower callers than
 *     pdna_box()) fares better. **Flagged for hardware-testing-protocol, not asserted
 *     safe from static analysis alone**: this is a per-function sum along the shortest
 *     traced path, not a whole-program worst-case (a deeper nested UI call before the
 *     fetch, or different register allocation at final link, could still eat the
 *     margin). §J of docs/HW-TEST-2026-09-05-GB-ARC.md asks specifically for a box-grid
 *     hover-portrait stress pass (rapid cursor movement across many GB-origin cells)
 *     as the practical test this number can't replace.
 *   - The two registered ROM paths are genuine EWRAM_BSS residents (pdna_main.c),
 *     because cfg_save() rewrites config.cfg FROM RESIDENT STATE on almost every
 *     browser keypress — a path that only round-tripped through a file could not
 *     survive that rewrite. GB_ROM_PATH_MAX is 128, not PATH_MAX's 256: two full-width
 *     slots (512 B) would leave ~12 B of the 524 B EWRAM budget; two half-width slots
 *     (256 B) leave ~268 — the same per-feature-cap idea gb_sidecar.h already uses
 *     (GBSC_PATH_MAX 48). A GB ROM dump living deeper than 128 chars is refused, not
 *     silently truncated (matching e.g. dup_name()'s "no room" convention elsewhere).
 *   - The located-table cache (RomGbSpriteLoc, ~260 B) is NOT resident anywhere: it is
 *     read from a small per-gen file at EVERY fetch (a session-scoped path is opened
 *     rarely — the router memoises, so this is once per distinct species view, not
 *     once per frame) rather than costing another 520 B of EWRAM for two copies.
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
  GB_ART_REG_BAD_ROM        /* opened, but not a valid/locatable Game Boy image        */
} GbArtRegStatus;

GbArtRegStatus gb_art_register(uint8_t gen, const char* path);

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
 * startup — a no-op (source never registered) under PDNA_DELTA. */
void gb_art_boot_register(void);

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
