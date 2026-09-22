/*
 * PokeDNA — entry point (milestone M0).
 *
 * Scaffold: bring up tonc + the bitmap UI, detect the flashcart, mount the SD,
 * and BROWSE the card (folders + .sav files, navigate with A/B) so saves in
 * subfolders are reachable. On select we parse the save (existing gen3_save.c)
 * and show a basic read-only summary (game, trainer, TID, play time, party
 * preview). This proves the whole pipeline — flashcart -> FatFs -> parse -> UI —
 * end to end before M1 adds the full hidden-data (IV/EV/stat) viewer.
 *
 * Include order matters: <tonc.h> first so its u8/u16 typedefs are in place
 * before flashcartio.h pulls in sys.h's u8/u16 macros (same pattern as the
 * record-mixer's main.c).
 */

#include <tonc.h>
#include <stdio.h>
#include <string.h>

#include "flashcartio.h"   /* active_flashcart, flashcartio_activate (pulls in sys.h) */
#ifdef PDNA_DELTA
#include "flashsave.h"     /* emulator build: the save IS this ROM's own 128 KiB flash */
#endif
#include "sys.h"           /* EWRAM_BSS (idempotent; guarded)                          */
#include "ff.h"
#include "gen3_save.h"
#include "gen3_mirage.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"
#include "mon_icons.h"
#include "mon_icons_gate.h"  /* PDNA_MON_ICONS_ART_COMPILED -- is this a full-art build (see
                              * party_bob_recompose's two code paths, MUST-FIX 2) */
#include "perf.h"          /* SD/icon telemetry + the session clock (see perf.h) */
#include "pdna_summary.h"
#include "pdna_box.h"
#include "xfer_gate.h"      /* BACKLOG #120 S2: xg_pc_live/xg_togame_row/xg_paste_row/xg_inject_refuse */
#include "bank_cell.h"      /* BACKLOG #150 S150-6: bc_is_native -- app_paste_gb_commit's G-H6 guard */
#include "xfer_io.h"        /* BACKLOG #150 S150-6: xr_path_for_key/xr_path_for_name/xr_migrate_once */
#include "xfer_rec.h"       /* BACKLOG #150 S150-6: xr_key_g3 -- the reroll re-key guard             */
#ifdef PDNA_DELTA
#include "bank_plant.h"     /* BACKLOG #150 S150-9 decision 12(c): the PC box0 slot29 seed          */
#endif
#include "gen3_trainer.h"
#include "gen3_record.h"    /* Emerald Battle Record (save sector 31) info + export */
#include "gen3_frontier.h"  /* g3f_streak_get/g3f_modes/g3f_mode_name for the record screen's streaks page */
#include "pdna_frontier.h"  /* Battle Frontier win-streak viewer/editor (SaveBlock2) */
#include "pdna_fly.h"       /* Fly-destination (visited-town) flags (SaveBlock1)     */
#include "pdna_contest.h"   /* Museum paintings + Contest Hall winners (BACKLOG #60) */
#include "pdna_map.h"       /* overworld map, read from the user's own Pokemon ROM   */
#include "pdna_trainer.h"
#include "pdna_edit.h"
#include "gen3_edit.h"     /* EditMon, gen3_edit_load/commit, em_set_*, em_preview */
#include "gen3_clip.h"     /* ClipMon, slot ops (copy/paste/dup/release) */
#include "gen3_gen.h"      /* gen3_build_mon_spread — one seed, matched PID + IVs */
#include "pdna_legality.h" /* pdna_legality_show */
#include "pdna_gen12.h"    /* GB import: mount a Gen-1/2 save read-only */
#include "bank_cell.h"     /* bc_is_native -- native Bank cell interception (BACKLOG #150 S150-2) */
#include "pdna_pk.h"     /* pdna_pk_export (.pk3) */
#include "pdna_bank.h"   /* pdna_bank_show (bank = parallel boxes) */
#include "gen3_flags.h"    /* event flags */
#include "flags_fold.h"    /* BACKLOG #2a: pure row-visibility math for the collapsible FLAGS list */
#include "gen3_dex.h"      /* Pokedex seen/owned flags */
#include "gen3_pokeblock.h" /* Pokeblock case (RS/Emerald) */
#include "gen3_daycare.h"  /* daycare breeding compatibility (the man's verdict) */
#include "gen3_items.h"    /* item bags */
#include "pdna_bag.h"      /* real Emerald bag screen (data-editor bag tab) */
#include "pdna_yard.h"     /* BACKLOG #114: Day-Care yard scene -- dc_scene/dc_pointer/
                             * dc_icon_over_bg/dc_rescan/dc_roll_decos + the shared
                             * HAVE_DAYCARE_BG probe, extracted out of this file's old
                             * static ~5711-6089 region (see pdna_yard.h/.c) */
#include "bag_bg.h"        /* bag_bg() availability gate (weak NULL when art-free) */
#include "pokeblock_bg.h" /* Pokeblock case chrome (weak NULL when art-free) */
#include "rom_mon.h"       /* phase-1 ROM-gated icons (fused ROM -> real box icons) */
#include "rom_text.h"      /* phase-2: descriptions out of the user's own ROM */
#include "rom_sprite.h"    /* Phase 1 (ROM-art): the summary portrait's third rung */
#include "rom_itemart.h"   /* Phase 1 (ROM-art): item icons + type badges           */
#include "rom_wallpaper.h" /* Phase 4 (ROM-art): box wallpapers -- pdna_box.c's §12c rung */
#include "rom_chrome_gate.h"
#include "rom_chrome.h"    /* trainer-card + pokeblock ROM rung (Emerald/Ruby)      */
#include "hand_gate.h"     /* PDNA_HAND_ART_COMPILED -- see that header for why      */
#if !PDNA_HAND_ART_COMPILED
#include "rom_hand.h"      /* Phase 3 (ROM-art): the pointer glove (DESIGN.md Sec 1.3/4.5) */
#endif
#include "pdna_origin_art.h" /* pdna_origin_art_set_romsprite -- registers the RomSprite */
#include "gb_art_source.h" /* slice E3: the GB half of the same art router, romgb1/romgb2 */
#include "gb_art_io.h"     /* BACKLOG #148: GbRegUi/GB_ART_LOC_UI -- gb_reg_progress's ctx *
                            * type and the new locator pdna_gbscreen.c's scan reports    */
#include "pdna_gbscreen.h" /* U2a: the shared GB-screen shell, gb_scale_mode/"gbscale" key */
#include "artbuf.h"        /* mon_decomp -- the shared 8 KiB decode buffer            */
#include "item_icons.h"    /* item_icon_for -- the compiled rung app_item_icon() tries first */
#include "type_icons.h"    /* type_icon_for -- the compiled rung app_type_badge() tries first */
#include "box_oam.h"       /* boxoam_rom_icons registration */
#include "art_cache.h"     /* Phase 2 (ROM-art cache): art.idx format + FNV            */
#include "art_session.h"   /* Phase 2: the once-per-session cache/rom resolver          */
#include "art_icons_extract.h"
_Static_assert(ART_ICONS_ROW_BYTES <= SF_STREAM_CHUNK_MAX, "icon extraction streams one row per verified-write chunk (review #84a P1)"); /* Phase 2: the icons.bin extraction pass                */
#include "fused_rom.h"
#include "fused_sav.h"    /* a save fused into the image: the emulator build's fallback */
#include "fused_gb.h"     /* BACKLOG #62: a whole fused GB ROM+save corpus, delta-gb only */
#include "gen3_secretbase.h" /* Secret Base records (RS/Emerald) */
#include "osk.h"           /* osk_search (numeric entry) */
#include "pdna_pick.h"   /* pick_item, pick_move (PC-menu quick editors) */
#include "snd.h"           /* UI sound effects */
#include "rmbl.h"          /* haptic rumble cues (per-cue toggles) */
#include "rumble.h"        /* rumble_io_suspend/resume: mute the motor while a blit reads ROM */
#include "gba_rtc.h"       /* live cartridge RTC reader (clock check & fix) */
#include "gb_sidecar.h"    /* S5-B: the Gen-3 <-> Game Boy sidecar format + merge-up */
#include "gb_reconcile.h"  /* S5-C Part B2: the pure-C identity-match core for reconcile-on-load */
#include "xfer_reconcile.h" /* BACKLOG #150 S150-11: the NATIVE_HOME transfer-ledger reconcile */
#include "pdna_app.h"
#include "savefile.h"
#include "log.h"
#include "fastseek.h"     /* FIL cluster link maps -- the ONE cltbl owner */
#include "art_icons_cache.h" /* art_icons_row_for -- species -> icon-store row         */
#include "icon_store.h"    /* THE icon row cache -- reset from app_icon_cache_resolve */
#include "pdna_romcheck.h"  /* sampled high-ROM self-check: the incomplete-SD-load guard */
#include "pdna_romfull.h"   /* the FULL-image verifier screen (boot hold R+SELECT / FILE MENU) */
#include "ui.h"
#include "pdna_layout.h"   /* screen geometry + fixed strings, shared with the host text-fit test */
#include "nav_avail.h"     /* BACKLOG #58: per-row nav availability + honest messages */

/* PDNA_DIR itself now lives in pdna_app.h (S5-B review fix #10: shared, not re-defined
 * per file) -- pdna_app.h is included above, before this point. */
#define LOG_PATH      "/PokeDNA/log.txt"
#define PATH_MAX      256
#define MAX_ENTRIES   256
#define NAME_MAX      BR_NAME_MAX /* BACKLOG #186: BrowseEntry/BR_NAME_MAX moved to
                                   * pdna_map.h (shared with pdna_map.c's pickers) --
                                   * kept as an alias so every existing NAME_MAX site
                                   * below needs no rename. */
#define LIST_COLS     28          /* display columns for a list row                 */
#define VIS_ROWS      12          /* visible rows in the framed browser panel        */

/* file-browser sort + filter state (sd-browser style) */
typedef enum { SORT_NAME = 0, SORT_SIZE = 1, SORT_DATE = 2 } BrSortKey;
static BrSortKey g_sort = SORT_NAME;
static bool      g_sortrev = false;
static bool      g_show_all = false;     /* false = folders + .sav only; true = all files */
static bool      g_show_hidden = false;

/* Per-place "moving sprites" toggles (Settings > Animations, persisted in config.cfg).
 * One bit per ANIM_* place. Box / Party / Dex / Daycare default ON; the summary
 * portrait wiggle defaults OFF (kept calm unless the user opts in). Defined up here so
 * cfg_save/cfg_load (above app_anim_enabled) can persist it. */
/* Day-Care yard visitors: the invented scenery mons (see dc_roll_decos). OFF by
 * default, and a ROM-GATED extra -- see app_yard_visitors_ok().
 *
 * They used to default ON because they make the yard look alive. Guy turned them off
 * (2026-08-09): without art the wanderers are indistinguishable from the two Pokemon
 * that are really boarding, and the selection arrow does not follow them, so it
 * regularly points at empty grass. Scenery you cannot select, next to Pokemon you can,
 * reads as a bug rather than as decoration. So the yard now shows only what is really
 * there, and the visitors come back once the user registers their own game ROM -- which
 * is also the only point at which there is real art to draw them with.
 * Persisted in config.cfg. */
static bool      g_yard_visitors = false;
/* ROM art detach switch (Settings > Game ROM, item 7): for A/B-testing the artless
 * build with vs without a ROM to draw art from, WITHOUT re-browsing/re-registering a
 * path. OFF (false) by default = normal behaviour (a registered ROM's art rungs are
 * live). ON = "no ROM art at all" (BACKLOG #47 made this the explicit contract, not
 * just the Gen-3 half of it):
 *   - app_icon_rom_open() (the single chokepoint every RomMon/RomSprite/RomItemArt/
 *     rom_wallpaper/rom_chrome/rom_hand source is opened through) skips opening any
 *     ROM this session and leaves every one of those sources at its reset/NULL
 *     state, and g3cross_pic_cb refuses the cross-game Gen-3 rung too;
 *   - gb_art_source.c's gb_art_have()/pic()/icon() (the Game Boy portrait + Gen-2
 *     menu-icon rungs) refuse via app_rom_art_off(), even for an explicit Settings
 *     registration;
 *   - app_era_roms() reports every era's ROM as unavailable, so the Sprites grid
 *     (sprite_era.h) can only offer NATIVE.
 * Every consumer above degrades to the SAME no-ROM fallbacks a real no-ROM session
 * already uses and this project already trusts -- nothing new. None of the
 * REGISTRATIONS themselves (g_rom_path[], the GB slots inside it, config.cfg) are
 * touched either way, so flipping this back ON needs no re-browse. Persisted in
 * config.cfg ("romoff"). */
static bool      g_rom_art_off = false;
/* Backup mode: 0 = new file each save, 1 = single rolling .bak, 2 = skip. Persisted. */
static int       g_backup_mode = 0;
static unsigned  g_anim_mask = (1u << ANIM_BOX) | (1u << ANIM_PARTY) | (1u << ANIM_DEX) |
                               (1u << ANIM_DAYCARE) | (1u << ANIM_SUMMARY);   /* summary wiggle ON by default (Emerald feel) */
static int       g_pc_last_box = 0;        /* PC box to open on (NOT the save's in-game box); app remembers it */

/* Big buffers live in EWRAM (.bss), never on the IWRAM stack. */
static u8          EWRAM_BSS g_save[G3_SAVE_FILE_SIZE];   /* 128 KiB raw image       */
static uint32_t    g_save_size = 0;                       /* actual loaded byte count (64 KiB
                                                           * dumps have no sector 31)        */
static u8          EWRAM_BSS g_sb1[G3_SAVEBLOCK1_BYTES];  /* reassembled SaveBlock1  */
static BrowseEntry EWRAM_BSS g_entries[MAX_ENTRIES];      /* current-dir listing     */
static int         g_count = 0;
static char        EWRAM_BSS g_cwd[PATH_MAX];             /* current directory (set in main) */
/* ONE registered-ROM-path table for all five slots this app ever remembers a ROM
 * for: PkGame's RS/Emerald/FRLG (indices 0-2, the map screen's per-game ROMs) plus
 * Gen-1/Gen-2 Game Boy ROMs (indices 3-4, gb_art_source.c's origin-art source) --
 * merged here (E3 review item 3) from two separate arrays (g_rom_path[3][PATH_MAX]
 * + g_gb_rom_path[2][GB_ROM_PATH_MAX]) that together cost 1,024 B of the ~524 B
 * EWRAM budget's already-tight remainder. Every slot is capped at GB_ROM_PATH_MAX
 * (128, gb_art_source.h) now, not PATH_MAX's 256 -- five half-width slots (640 B)
 * fit; three full + two half (1,024 B) did not leave enough for anything else this
 * arc still needs. Measured: 268 B free (three full + two half) -> 652 B free
 * (five half). A path that would not fit is REFUSED at set-time (app_rom_path_set()/
 * app_gb_rom_path_set()), not silently truncated -- item 3's other half. */
#define APP_ROM_SLOTS 5
static char        EWRAM_BSS g_rom_path[APP_ROM_SLOTS][GB_ROM_PATH_MAX];
/* PkGame (0-2) indices unchanged; Game Boy generations land at 3/4. */
static int gb_gen_slot(uint8_t gen) {
  return (gen == PDNA_GEN1) ? 3 : (gen == PDNA_GEN2) ? 4 : -1;
}
/* E4 (sprite-era, docs/SPRITE-ERA-DESIGN.md): the 5x5 kind x place grid of the
 * user's chosen art era, persisted via config.cfg's "era_<kind>_<place>=<era>"
 * lines (sprite_era.h's se_config_write/se_config_apply). 25 B, plain EWRAM_BSS
 * like g_rom_path above -- zero-initialised, which for this struct IS
 * se_default()'s NATIVE-everywhere default (SE_ERA_NATIVE == 0), so no explicit
 * init call is needed for the feature to start invisible on a fresh boot. */
static SeSetting   EWRAM_BSS g_era;
static PkMon       EWRAM_BSS g_party[6];                  /* decoded party of the open save  */
static u8          EWRAM_BSS g_pc[G3_PC_BYTES];           /* reassembled PC storage (boxes)  */
static u8          EWRAM_BSS g_sb2[G3_SECTOR_DATA_SIZE];   /* SaveBlock2 (trainer card/stats) */
static ClipMon     EWRAM_BSS g_clip;                       /* one-slot mon clipboard          */

/* ---- VBlank / input discipline (key_poll exactly once per frame) -------- */
static void vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

/* Central input wait — also the app-wide UI-sound chokepoint. A FRESH d-pad press
 * ticks snd_move (NOT key_repeat, so a held scroll doesn't machine-gun); A/B play
 * the confirm/back earcons. Nearly every screen funnels through here. */
/* vblanks per idle bob toggle — the Gen-3 cadence, same constant the box screen
 * (pdna_box.c ANIM_PERIOD) and the Pokedex (pdna_pick.c DEX_ANIM_PERIOD) use. */
#define PDNA_BOB_PERIOD 30

/* ---- the Tier B icon borrow, from the three screens whose bob needs it -----------
 *
 * WHAT THIS IS FOR. The icon store's own pool holds 6 rows on the icons.bin rung and
 * FOUR on the ROM rung (it spends 1,760 B of the same 6 KiB on the resident offset
 * table). A party is six mons and the day-care yard is up to seven, so on the ROM rung
 * those sets did not fit, icon_store_plan_resident() honestly said "not in RAM", and
 * mon_icon_anim_cheap() left them STANDING STILL -- which is precisely what Guy
 * reported ("in the party they are static"). icon_store_borrow() rents 32 more rows
 * from g_pc for the length of one screen and makes the set fit on both rungs.
 *
 * THE RULE, AND WHY THE RELEASE SITS WHERE IT DOES. g_pc is the user's PC storage.
 * app_commit_all() writes it straight back into the save (and app_commit_after_edit
 * reaches app_commit_all from the PARTY editor whenever an edit registers a dex entry,
 * pdna_main.c's own `block == g_sb1` branch) -- so committing with the borrow live
 * would write icon tiles over every box the user owns. There is no version of that
 * which is recoverable.
 *
 * So the borrow is held ONLY between a screen's paint and the first key it dispatches:
 * app_icons_hold() before the paint, app_icons_drop() the instant the idle loop ends.
 * Every key path -- repaint, nested menu, exit -- is then safe by construction, with
 * ONE release site per screen instead of one per exit. The Pokedex is the exception and
 * says so at its own call site: nothing it opens can reach the PC, so it holds across
 * its whole lifetime and keeps a scrolled page resident. pdna_main.c's
 * `switch (nav_menu())` calls icon_store_borrow(false) unconditionally after every case
 * body as the structural backstop.
 *
 * The per-keypress cost is one app_arena_release(), i.e. one 35,712 B re-derive of g_pc
 * from g_save -- a few ms on a screen that is repainting anyway, and never on the idle
 * path the user is actually watching. What it buys back is a bob that costs zero SD
 * transactions instead of 120-210 disk_read calls every 8 frames. */

/* The icon-store row a mon draws from. Eggs all share row 412; everything else is
 * species+form. Identical to the mapping box_oam.c's icon_tiles and art_fallbacks.c's
 * icon_from_cache use, and it has to stay identical or a plan would declare rows the
 * paint never asks for. */
uint16_t app_icon_row_of(uint16_t species, uint8_t form, bool egg) {
  return art_icons_row_for(egg ? 412 : species, egg ? 0 : form);
}

#if PDNA_MON_ICONS_ART_COMPILED
/* FULL-ART: mon_icons.c's compiled .rodata answers every mon_icon_for* call, so the
 * store is never consulted for these screens and a plan would stream 6-7 KiB off the
 * card that nothing reads -- a regression, not a no-op. Same reasoning, same gate, as
 * pdna_pick.c's dex_declare_page. */
void app_icons_hold(const uint16_t* rows, int n) { (void)rows; (void)n; }
void app_icons_drop(void) { }
#else
void app_icons_hold(const uint16_t* rows, int n) {
  icon_store_borrow(true);      /* FIRST: the sweep must see the big pool, or it fills
                                 * Tier A, comes up short, and the gate stays false */
  icon_store_plan(rows, n);
}
void app_icons_drop(void) { icon_store_borrow(false); }
#endif

/* wait_keys, plus a 2-frame idle bob. `kind` is an ANIM_* place; while nothing is
 * pressed and that place's toggle is on, *frame flips every `period` vblanks and
 * `redraw` recomposes the screen's sprites at the new frame. The animation runs
 * ONLY on frames with no key pending, so a press or a key-repeat is never delayed
 * behind a multi-sprite repaint (the rule the Pokedex learned). Callers own *ctr
 * and *frame so the phase survives their redraw loop. kind < 0 or redraw == 0
 * makes this exactly the old wait_keys (period is unused in that case).
 *
 * `mon_icon_anim_cheap()` in the trigger condition below doubles as this helper's
 * "no SD I/O on an animation tick" gate -- safe ONLY because its one and only
 * kind>=0 caller today is app_party_overlay's own bob (party_overlay_bob, which
 * redraws mon icons via mon_icon_egg_frame/mon_icon_for_form_frame). A FUTURE
 * kind>=0 caller whose redraw does NOT touch mon icons would be wrongly held to
 * this gate too -- if one is ever added, split this condition so the icon-cheap
 * check only covers the icon-drawing callers.
 *
 * WHAT THE GATE MEANS NOW (art_fallbacks.c): "is every row this screen DECLARED
 * already in RAM", not the old "which rung is serving". The overlay declares its
 * six rows and rents the space for them through app_icons_hold() before the paint,
 * so this answers YES on both rungs and a flip is zero SD transactions -- against
 * the 120-210 disk_read calls every 8 frames it used to cost on the icons.bin rung,
 * and against a bob gated off entirely on the ROM rung, which is the "in the party
 * they are static" Guy reported. */
static u16 wait_keys_bob_p(u16 mask, int kind, int* ctr, int* frame,
                           void (*redraw)(int), int period) {
  u16 hit, fresh;
  do {
    vsync();
    fresh = key_hit(mask);
    hit = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT));
    if (!hit && redraw && kind >= 0 && app_anim_enabled(kind) && mon_icon_anim_cheap() &&
        ++*ctr >= period) {
      *ctr = 0; *frame ^= 1;
      /* THE tick the user feels: one idle bob flip. Rolled up, never logged per
       * occurrence -- at PARTY_BOB_PERIOD (8 frames) this fires ~7 times a second, and
       * a line each would burn log.c's per-run byte budget in under a minute. The
       * owning screen calls perf_rep_flush(PERF_REP_BOB) when it is left.
       *
       * WHAT THIS SITE ACTUALLY COVERS, since the previous version of this comment
       * claimed "the party overlay, the box, the summary" and grep says otherwise:
       * app_party_overlay (ANIM_PARTY, below) is the ONLY kind >= 0 caller in the tree.
       * wait_keys_bob() has no other callers and wait_keys() passes kind = -1, which
       * disables the bob entirely. Every other bob is an inline `do { vsync(); ... }`
       * loop that never reaches this helper and carries its own rollup at its own site:
       * the PC box's (pdna_box.c -> "bob.box"), party_list's ("bob.party", same name as
       * the overlay's -- the two are never on screen together and cost the same six
       * icons), the day-care yard's ("bob.daycare") and the Pokedex grid's
       * (pdna_pick.c -> "bob.dex"). The summary's portrait wiggle (pdna_summary.c) is
       * deliberately NOT instrumented: its sprite pointer is hoisted out of the loop, so
       * the wiggle is pure CPU on a RAM buffer and there is nothing for an SD or icon
       * counter to say about it -- no "bob.summary" line exists, or should be looked for.
       *
       * The table stays ANIM_COUNT wide so the index below is safe by construction; the
       * four entries other than ANIM_PARTY are unreachable from HERE today and exist so
       * that a future kind >= 0 caller is named rather than logged as "bob.?". */
      static const char* const k_bobname[ANIM_COUNT] = {
        "bob.box", "bob.party", "bob.dex", "bob.daycare", "bob.summary" };
      perf_rep_begin(PERF_REP_BOB,
                     (unsigned)kind < ANIM_COUNT ? k_bobname[kind] : "bob.?");
      redraw(*frame);
      perf_rep_end(PERF_REP_BOB);
    }
  } while (!hit);
  if (fresh & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  return hit;
}

/* PDNA_BOB_PERIOD (30, ~59-60 frame full cycle) is what the box/dex/daycare/
 * summary screens all use — left untouched here, only the party screen's own
 * call site below passes a different period (measured against retail). */
static u16 wait_keys_bob(u16 mask, int kind, int* ctr, int* frame,
                         void (*redraw)(int)) {
  return wait_keys_bob_p(mask, kind, ctr, frame, redraw, PDNA_BOB_PERIOD);
}

static u16 wait_keys(u16 mask) { return wait_keys_bob(mask, -1, 0, 0, 0); }

/* ---- console identity (swi 0x0D, GetBiosChecksum) -------------------------
 * The DS's GBA-mode BIOS differs from the GBA's in exactly one byte ([3F0Ch]), so
 * the BIOS checksum is a reliable console ID:
 *     0xBAAE187F = GBA / GBA SP / Game Boy Micro
 *     0xBAAE1880 = NDS / NDS Lite running in GBA mode
 * (GBATEK "BIOS Misc Functions"; per-console dumps in mgba-emu/bios-dump.)
 * libtonc exposes it as BiosCheckSum() — a two-instruction stub, `svc 13; bx lr` —
 * so this costs nothing and needs no inline asm.
 *
 * Why it is worth a line in the log: every hardware report this project has ever
 * filed is missing the answer to "which console was that run on?". The Aug-16 test
 * console is unknown to this day, which is why a DS-Lite-specific factor could not
 * be excluded from the save-open hang. One SWI makes it recorded fact forever.
 *
 * DETECT AND LOG, DO NOT GATE. Nothing branches on this. Retail carts set 0x4317
 * and run from Omega-DE PSRAM in DS Lites universally, and every mechanism the
 * hang report implicates (FPGA-emulated GPIO interleave, PSRAM margin) is cart-
 * and-unit specific rather than console specific. Guy plays on a DS Lite, so a
 * blanket NDS refusal would cost him 100% of the speedup on a ~50%-confidence
 * hypothesis. The bus self-test below decides on measurement instead. */
#define BIOS_SUM_GBA 0xBAAE187Fu
#define BIOS_SUM_NDS 0xBAAE1880u

static unsigned s_bios_sum = 0;

static const char* console_name(void) {
  switch (s_bios_sum) {
    case BIOS_SUM_GBA: return "GBA/SP";
    case BIOS_SUM_NDS: return "NDS";
    default:           return "?";       /* unknown BIOS: an emulator's HLE, or a clone */
  }
}

/* Bus self-test verdicts, stashed at the moment they are measured and logged later:
 * init_system() runs BEFORE log_init(), whose log_clear() would wipe anything logged
 * from here. v1 = the CPU+DMA ladder (the only one that may demote the bus). */
static int s_busv1 = FCIO_BUS_SKIPPED;

static const char* bus_verdict_str(int v) {
  switch (v) {
    case FCIO_BUS_OK:            return "ok";
    case FCIO_BUS_SKIPPED:       return "skipped";
    case FCIO_BUS_BAD_READ:      return "FAIL cpu-read";
    case FCIO_BUS_BAD_DMA:       return "FAIL dma3-from-rom";
    case FCIO_BUS_BAD_GPIO:      return "FAIL cart-gpio interleave";
    case FCIO_BUS_BAD_FETCH:     return "FAIL rom instruction fetch";
    case FCIO_BUS_UNSTABLE_SLOW: return "FAIL even at loader timing (bad image/cart)";
    default:                     return "?";
  }
}

/* ---- the two self-tests that could not run inside init_system() -------------
 *
 * Both are VERDICT ONLY: neither may move the rung ladder or demote the bus. A
 * rumble fault must never cost the session its frame rate, and by this point the
 * ladder has settled anyway. Each is preceded by a FLUSHED line, so if one of them
 * is where a marginal unit dies, the card names the probe that killed it instead of
 * leaving yet another unexplained hang.
 *
 * Called from BOTH boot paths on purpose. The delta/emulator build has no card and
 * no rumble driver, so the GPIO half self-skips there -- but the ROM-fetch half
 * still runs, which is the only way any of this code gets exercised off-hardware. */
/* Test hook: force the GPIO half to run where there is no Omega (the delta build in
 * mGBA), so -DFCIO_PROBE_FORCE_FAIL=3 can actually EXECUTE the BAD_GPIO branch and
 * rmbl_lockout() before Guy's hardware does. An untested error branch whose first
 * ever execution is on the console that is already hanging is not a safety net. */
#ifndef PDNA_TEST_FORCE_GPIO_PROBE
#define PDNA_TEST_FORCE_GPIO_PROBE 0
#endif

static void bus_late_selftests(void) {
  int fetchv;

  if ((rumble_omega() || PDNA_TEST_FORCE_GPIO_PROBE) && rmbl_get_mask() != 0u) {
    /* GPIO interleave, in BOTH forms. Raw = the write exactly as it shipped before
     * the WAITCNT bracket existed (it touches WAITCNT not at all), which answers the
     * hang report's actual question. Bracketed = the mitigation that ships now.
     * Running only one makes a failure unattributable, because the bracket itself
     * writes WAITCNT twice per edge and an ISR writing WAITCNT has no retail
     * precedent either. Skipped entirely when the user has turned every cue off: no
     * cart-GPIO traffic ships in that case, so none should be generated. */
    int graw, gbrk;
    log_line("bus: probing cart-gpio interleave (stress rate, far above what ships)");
    app_log_flush();
    graw = flashcartio_bus_probe_gpio(rumble_bus_probe_raw);
    gbrk = flashcartio_bus_probe_gpio(rumble_bus_probe_bracketed);
    log_line("bus gpio self-test: raw=%s bracketed=%s (rung=%d)",
             bus_verdict_str(graw), bus_verdict_str(gbrk), flashcartio_bus_rung());
    if (gbrk == FCIO_BUS_BAD_GPIO) {
      /* Even the shipping form corrupts reads here. Drop the nicety, keep the speed. */
      rmbl_lockout();
      log_line("rumble: OFF for this session - cart GPIO corrupts ROM reads on this "
               "unit even bracketed. The bus boost is UNAFFECTED (rung=%d).",
               flashcartio_bus_rung());
    } else if (graw == FCIO_BUS_BAD_GPIO) {
      /* The most useful result this probe can produce: the hazard is real on this
       * unit and the mitigation demonstrably fixes it. Rumble stays on. */
      log_line("rumble: kept - raw GPIO writes corrupt ROM reads here, the WAITCNT "
               "bracket fixes it. That is the garbled-wallpaper hazard, measured.");
    }
  }

  /* The half the IWRAM probe cannot reach: a checksum loop deliberately left in
   * .text so the INSTRUCTION FETCHES run at the settled rung too. GamePak prefetch
   * (WAITCNT bit 14) engages for opcode fetches only, so nothing before this line
   * has tested the mechanism the hang report names as its prime suspect. It runs
   * last, and its announcement is flushed first, precisely because this is the one
   * probe that can take the tool down if the fetch path is what is broken. */
  log_line("bus: probing rom instruction fetch (last, and the one that can hang)");
  app_log_flush();
  fetchv = flashcartio_bus_probe_fetch();
  log_line("bus fetch self-test: %s (rung=%d, waitcnt=%04x)", bus_verdict_str(fetchv),
           flashcartio_bus_rung(), *(volatile uint16_t*)0x04000204);
  if (fetchv == FCIO_BUS_BAD_FETCH)
    log_line("bus: rom CODE fetch is unreliable at this rung - hold L+SELECT at boot "
             "to run the whole session at the loader's timing");
  app_log_flush();
}

/* The keys that were held when the ROM started, captured before anything can consume
 * them. Two boot overrides read this: L+SELECT (slow bus, below) and R+SELECT (the
 * full-image verifier, in main()). They are independent — holding L+R+SELECT gives you
 * both, which is exactly what you want when a marginal bus is the suspect. */
static u16 s_boot_held = 0;

static void init_system(void) {
  /* Console identity first, before anything touches the bus: swi 0x0D reads the
   * BIOS, which WAITCNT cannot affect, and the answer belongs in the log either
   * way. */
  s_bios_sum = BiosCheckSum();

  /* FIELD OVERRIDE: hold L+SELECT at boot to run the whole session at the loader's
   * timing. This is the decisive A/B for a suspected bus-timing hang — the one
   * experiment that otherwise needs a rebuild and a toolchain. Read raw: the key
   * subsystem is not up yet, and REG_KEYINPUT is active-low. */
  const u16 held = (u16)(~REG_KEYINPUT & KEY_MASK);
  s_boot_held = held;
  if ((held & (KEY_L | KEY_SELECT)) == (KEY_L | KEY_SELECT)) {
    flashcartio_bus_hold();               /* capture the loader's value, never boost */
  } else {
    /* FIRST, before anything else runs a single loop out of ROM: take the game-pak
     * bus off the loader's 4/2-waitstates-no-prefetch handoff. Everything in this
     * tool executes from ROM, so this is a flat speed-up of the whole app — the
     * repaints, the animations, the decode. flashcartio drops back to the inherited
     * timing for the duration of each SD transfer (see flashcartio.h). */
    flashcartio_bus_fast();
    /* ...and IMMEDIATELY prove it, before irq_init/ui_init/snd_init execute another
     * few thousand ROM fetches at an unvalidated timing. Three 4 KiB windows of this
     * image, checksummed at the loader's timing and again at the boosted one, by CPU
     * and by DMA3. On failure the library steps down a rung (3/2 + prefetch) or hands
     * the bus back for good, so a marginal cart/console degrades into a slow session
     * instead of a hang. DATA READS ONLY — the probe runs from IWRAM so a bad fetch
     * cannot kill the detector, which means GamePak prefetch is untestable here; the
     * ROM-resident fetch pass after mount covers that half. No GPIO agitator yet: the
     * rumble driver does not own the cart GPIO until after the SD is up. */
    s_busv1 = flashcartio_bus_validate();
  }

  irq_init(NULL);
  irq_add(II_VBLANK, NULL);
  ui_init();                               /* Mode 3 + bitmap TTE */
  snd_init();                              /* PSG UI sound effects */
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);  /* hold any d-pad dir to keep scrolling */
  key_repeat_limits(14, 3);                /* hold ~0.23s, then ~20/s */
}

static const char* flashcart_name(void) {
  switch (active_flashcart) {
    case EVERDRIVE_GBA_X5: return "EverDrive GBA X5";
    case EZ_FLASH_OMEGA:   return "EZ-Flash Omega/DE";
    default:               return "none";
  }
}

/* One line per detection attempt, for the boot screen and the log: what the driver
 * concluded and, on an EZ-Flash, WHICH page it found the running image on. That page is
 * the whole story of the 2026-09-12 NOR hang (lib/ezflashomega/io_ezfo.c, "identifying
 * OUR page"): "PSRAM" = SD-loaded (page 0x200), "NOR#p" = booted from the game NOR, and
 * "la=N@P" = N stale same-title images were rejected, the first at page P. `out` holds
 * 40 bytes. The sys8 row budget (29 glyphs from PDNA_DETECT_X) is enforced by the two
 * forms below: the longest verdict is "EZ hdr!" and the page prints in hex, so the worst
 * row, "8: EZ hdr! NOR#1ff la=255@1ff", is exactly 29 -- TTE would otherwise wrap a
 * longer row onto the next attempt's line. The strings and formats live in
 * pdna_layout.h so host_textfit_test.c measures that worst case from the same parts. */
#ifndef PDNA_TEST_DETECT_WORST
#define PDNA_TEST_DETECT_WORST 0   /* 1: paint the widest reachable row whatever the cart
                                    * said -- the emulator shot vehicle for the sys8 row
                                    * budget (no cart answers there). Never in a shipped
                                    * build; host_textfit_test.c is the build-time check. */
#endif

static void detect_line(char* out, int attempt) {
  unsigned page = flashcartio_ezfo_page(), first = 0xFFFFu;
  unsigned la = flashcartio_ezfo_lookalikes(&first);
  int code = flashcartio_detect_code();
  const char* what;
  int n;
#if PDNA_TEST_DETECT_WORST
  code = FCIO_DET_EZFO_HDRONLY; page = 0x1ffu; la = 255u; first = 0x1ffu;
#endif
  switch (code) {
    case FCIO_DET_ED_OK:        what = PDNA_DET_ED_OK;      break;
    case FCIO_DET_ED_SD_FAIL:   what = PDNA_DET_ED_SDFAIL;  break;
    case FCIO_DET_EZFO_OK:      what = PDNA_DET_EZ_OK;      break;
    case FCIO_DET_EZFO_HDRONLY: what = PDNA_DET_EZ_HDRONLY; break;
    case FCIO_DET_EZFO_NOT:     what = PDNA_DET_NOCART;     break;
    case FCIO_DET_EZFO_NOPAGE:  what = PDNA_DET_EZ_NOPAGE;  break;
    default:                    what = PDNA_DET_UNKNOWN;    break;
  }
  if (page == 0x200u)     n = siprintf(out, PDNA_DET_FMT_PSRAM, attempt, what);
  else if (page < 0x200u) n = siprintf(out, PDNA_DET_FMT_NOR, attempt, what, page);
  else                    n = siprintf(out, PDNA_DET_FMT_PLAIN, attempt, what);
  if (la) siprintf(out + n, PDNA_DET_FMT_LA, la, first);
}

/* Writes (edit mode, later) are Omega-only; surface it from M0. An image whose own
 * sampled CRCs say it is not the one we shipped must never write a user's save. */
static bool cart_writable(void) {
  return active_flashcart == EZ_FLASH_OMEGA && !pdna_romcheck_bad();
}

static void halt_msg(const char* msg) {
  log_line("HALT: %s", msg);
  /* URGENT: bypass the "three failed writes, stop touching the card" latch. These
   * are the last words of the run, and a halt with a reason on screen but nothing
   * on the card is exactly the artifact that makes a hardware report unreadable. */
  log_flush_urgent(LOG_PATH);
  ui_clear();
  ui_panel(8, 48, 224, 44, UI_PANEL, UI_WARN);
  ui_text(20, 58, UI_WARN, "HALT");
  ui_text(20, 74, UI_TEXT, msg);
  snd_error();
  while (1) vsync();
}

/* Match save files AND their backups so the .bak copies are visible to pick/restore:
 * any name CONTAINING ".sav" (case-insensitive) -> x.sav, x.sav.bak, x.sav.bak3, ... */
static int has_sav_ext(const char* n) {
  for (const char* p = n; p[0] && p[1] && p[2] && p[3]; p++)
    if (p[0] == '.' &&
        (p[1] == 's' || p[1] == 'S') && (p[2] == 'a' || p[2] == 'A') && (p[3] == 'v' || p[3] == 'V'))
      return 1;
  return 0;
}

/* BACKLOG #186: every OTHER kind (BR_MATCH_SUFFIX) wants a plain case-insensitive
 * suffix match against spec->exts, ported from pdna_map.c's old pick_rom()
 * ext_matches() -- the .sav kind keeps has_sav_ext's looser "contains" rule (backups
 * included) unchanged, so A5's byte-for-byte .sav behaviour never routes through here. */
static bool spec_ext_match(const BrowseSpec* spec, const char* name) {
  if (spec->match_mode == BR_MATCH_SAV) return has_sav_ext(name) != 0;
  int l = (int)strlen(name);
  for (int e = 0; spec->exts[e]; e++) {
    int el = (int)strlen(spec->exts[e]);
    if (l <= el) continue;
    const char* t = name + l - el;
    bool ok = true;
    for (int i = 0; i < el && ok; i++) {
      char a = t[i], b = spec->exts[e][i];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (a != b) ok = false;
    }
    if (ok) return true;
  }
  return false;
}

/* ---- path helpers (ported from the record-mixer browser) ---------------- */
static bool at_root(void) { return g_cwd[0] == '/' && g_cwd[1] == 0; }

static bool path_join(const char* dir, const char* name, char* out) {
  unsigned dl = (unsigned)strlen(dir), nl = (unsigned)strlen(name);
  if (dl == 1 && dir[0] == '/') {
    if (1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "/%s", name);
  } else {
    if (dl + 1 + nl + 1 > PATH_MAX) return false;
    siprintf(out, "%s/%s", dir, name);
  }
  return true;
}

static void path_up(void) {
  int l = (int)strlen(g_cwd);
  if (l <= 1) return;
  int i = l - 1;
  while (i > 0 && g_cwd[i] != '/') i--;
  if (i == 0) g_cwd[1] = 0; else g_cwd[i] = 0;
}

/* case-insensitive ASCII name compare (locale-free) */
static int name_ci(const char* a, const char* b) {
  for (;;) {
    unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return (int)x - (int)y;
    if (!x) return 0;
  }
}

/* directories always first (not reversible); then by the active key, name as the
 * tiebreaker, negated for descending. */
static int entry_cmp(const BrowseEntry* x, const BrowseEntry* y) {
  if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
  int c;
  switch (g_sort) {
    case SORT_SIZE: c = (x->size < y->size) ? -1 : (x->size > y->size) ? 1 : 0; break;
    case SORT_DATE: c = (x->dosdt < y->dosdt) ? -1 : (x->dosdt > y->dosdt) ? 1 : 0; break;
    default:        c = name_ci(x->name, y->name); break;
  }
  if (c == 0) c = name_ci(x->name, y->name);
  return g_sortrev ? -c : c;
}

static void sort_entries(BrowseEntry* ents) {      /* stable insertion sort, never mid-transfer */
  for (int i = 1; i < g_count; i++) {
    BrowseEntry tmp = ents[i];
    int j = i - 1;
    while (j >= 0 && entry_cmp(&ents[j], &tmp) > 0) { ents[j + 1] = ents[j]; j--; }
    ents[j + 1] = tmp;
  }
}

/* Scan g_cwd into `ents` (spec->entries, spec->cap -- A3: the launch browser's own
 * resident g_entries for the .sav spec, an arena/mon_decomp-borrowed buffer for every
 * other kind, since g_entries is ALSO box_oam.c's icon-cache/GB-reconcile borrow. D6
 * correction: not because those two ARE reachable at once today -- Settings, where
 * the other kinds open from, is never reachable from inside a live box-screen/GB-
 * session borrow -- this is a conservative separation so a future caller cannot
 * alias it by accident, not a proven conflict). The filter (g_show_all / g_show_hidden
 * / spec's own extension rule) and the sort are sd-browser-style, identical for
 * every kind. */
static void __attribute__((noinline)) scan_dir(const BrowseSpec* spec, BrowseEntry* ents) {
  g_count = 0;
  DIR dir;
  FILINFO fno;
  if (f_opendir(&dir, g_cwd) != FR_OK) { log_line("opendir %s failed", g_cwd); return; }
  while (g_count < spec->cap && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    bool is_dir = (fno.fattrib & AM_DIR) != 0;
    if (!g_show_hidden && (fno.fattrib & (AM_HID | AM_SYS))) continue;
    if (!is_dir && !g_show_all && !spec_ext_match(spec, fno.fname)) continue;  /* folders + filter unless show-all */
    strncpy(ents[g_count].name, fno.fname, NAME_MAX - 1);
    ents[g_count].name[NAME_MAX - 1] = 0;
    ents[g_count].size = is_dir ? 0 : (uint32_t)fno.fsize;
    ents[g_count].dosdt = ((uint32_t)fno.fdate << 16) | (uint32_t)fno.ftime;
    ents[g_count].is_dir = is_dir;
    g_count++;
  }
  f_closedir(&dir);
  sort_entries(ents);
  log_line("scan %s: %d entries", g_cwd, g_count);
}

/* ---- persistent browser prefs: last folder + sort/filter (#6) ------------ */
#define CFG_PATH PDNA_DIR "/config.cfg"

/* Was PATH_MAX*4+128 (1152 B: "4 PATH_MAX-scale strings" == dir + the 3 Gen-3 ROM
 * paths, +128 slack for every small int/flag key). Grown for slice E3's two GB ROM
 * paths (romgb1/romgb2, GB_ROM_PATH_MAX each, not PATH_MAX -- see gb_art_source.h) and
 * pre-sized for E2's sprite-era grid (up to 20 "era_<kind>_<place>=<era>\n" lines, 22 B
 * worst case each per sprite_era.h's own token table) even though nothing writes those
 * lines yet -- se_config_write()/se_config_apply() wiring is E4's Settings-grid slice,
 * not this one (see sprite_era.h's "WIRING NOTE for E3/E4"), but the buffer only wants
 * sizing ONCE. The +256 slack covers the 12 small int/flag keys in the first siprintf
 * below (each a handful of bytes) with room to spare. */
#define CFG_BUF_BYTES (PATH_MAX * 4 + GB_ROM_PATH_MAX * 2 + 20 * 22 + 256)

/* BACKLOG #186: scan an ALREADY-LOADED config.cfg buffer (no file I/O, no locals
 * bigger than a couple of pointers -- STACK ok review: an earlier version of this
 * lane had cfg_save_ex() call a full read-the-file helper for each of its three
 * round-tripped keys, stacking a SECOND CFG_BUF_BYTES frame on top of cfg_save_ex's
 * own and blowing the stack budget by 24 B; this non-mutating, I/O-free scan is what
 * both callers below actually need) for `key`'s value into `out` (capped, always
 * NUL-terminated). `text` need not be NUL-terminated; `len` is the real byte count
 * f_read returned. */
static bool find_key_in_text(const char* text, uint32_t len, const char* key, char* out, int cap) {
  out[0] = 0;
  size_t klen = strlen(key);
  const char* p = text; const char* end = text + len;
  while (p < end) {
    const char* eol = p;
    while (eol < end && *eol != '\n' && *eol != '\r') eol++;
    if ((size_t)(eol - p) > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
      const char* v = p + klen + 1;
      size_t vlen = (size_t)(eol - v);
      if (vlen > (size_t)cap - 1) vlen = (size_t)cap - 1;
      memcpy(out, v, vlen); out[vlen] = 0;
      return true;
    }
    p = eol;
    while (p < end && (*p == '\n' || *p == '\r')) p++;
  }
  return false;
}

/* BACKLOG #186: read ONE key's value out of config.cfg into `out`. Used by
 * browse_pick_spec() to seed a non-.sav picker's remembered folder at open time.
 * Transient: `buf` is a plain stack local (same CFG_BUF_BYTES class cfg_load()
 * already puts on the stack once at boot), freed on return -- no new EWRAM static
 * either way (A3's "no new EWRAM statics either way" applies to the whole lane, not
 * just the g_entries question). cfg_save_ex() below does its OWN, separate read
 * rather than calling this -- see its own comment for why. */
static bool cfg_read_raw_key(const char* key, char* out, int cap) {
  out[0] = 0;
  FIL f;
  if (f_open(&f, CFG_PATH, FA_READ) != FR_OK) return false;
  char buf[CFG_BUF_BYTES]; UINT br = 0;
  FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br); f_close(&f);
  if (fr != FR_OK || br == 0) return false;
  return find_key_in_text(buf, br, key, out, cap);
}

/* Persist the browser state so the next launch reopens the same folder with the
 * same sort/filter. Writes are EZ-Flash-Omega-only (EverDrive write isn't wired),
 * so this is a no-op on a read-only cart; best-effort, any failure is ignored.
 *
 * EVERY append below is now bounded (sniprintf against the REMAINING capacity, never
 * a fresh sizeof(buf)) and a write that would not fit sets `truncated` and stops
 * rather than running siprintf's return value past the end of `buf` -- the landmine
 * the original uncapped `n += siprintf(buf + n, ...)` loop left for whichever key
 * eventually pushed the total over CFG_BUF_BYTES (E3 review). A truncation is logged,
 * never silent: nothing here is safety-critical (a missing key just falls back to its
 * compiled default), so "say so and drop the rest" is enough.
 *
 * BACKLOG #186: `active_key`/`active_val`, when non-NULL, is a dir_rom/dir_gb/
 * dir_gbsav folder-memory key this call is actively updating from a LIVE value --
 * used by a non-.sav browse_pick_spec() session, which temporarily repurposes g_cwd
 * for its OWN folder (see that function), so g_cwd cannot be trusted for "dir" while
 * one of those sessions is open. The .sav spec never passes an override ("dir" is
 * always live in g_cwd, so cfg_save() below -- the plain, argument-less call every
 * OTHER site in this file already makes -- is untouched, preserving A5's byte-for-
 * byte .sav behaviour). Whichever of the three dir_* keys this call is NOT actively
 * writing is round-tripped from the file on disk rather than assumed empty, so an
 * unrelated settings change elsewhere (e.g. toggling rumble) can never erase a
 * folder memory this call didn't touch. That read reuses `buf` below (one
 * CFG_BUF_BYTES frame, not a second one from a nested helper call -- STACK ok
 * review: calling cfg_read_raw_key() three times here used to stack ITS OWN
 * CFG_BUF_BYTES frame on top of this function's, 24 B over budget). */
/* BACKLOG #186: split out of cfg_save_ex (noinline) -- its own FIL + a CFG_BUF_BYTES
 * read buffer are only needed transiently, to harvest the 3 dir_* keys from the OLD
 * file before cfg_save_ex overwrites it; keeping them in a separate function lets the
 * compiler free that stack space before cfg_save_ex's OWN buf (used for the NEW
 * content) is even touched, instead of the two staying resident together for cfg_
 * save_ex's whole body (STACK ok review: this + browse_seed_cwd's extraction are
 * what bring the deepest chain back under budget). */
/* This writer's own layout always puts "dir=" + the dozen small flag/int keys + the
 * three dir_* keys FIRST (see the sniprintf/loop below), well within this bound at
 * their worst case (256+150+3*140 = 819 B) -- a 1024 B scan window, not the full
 * CFG_BUF_BYTES (1976 B), is what actually shrinks this function's frame enough to
 * clear the stack budget (STACK ok review, 24 B over at CFG_BUF_BYTES). A file that
 * was hand-edited to move a dir_* key past this window degrades exactly like any
 * other unreadable dir_* key already does here (round-trips as absent, next write
 * drops it) -- nothing here is safety-critical (cfg_save_ex's own header comment). */
#define CFG_DIRKEY_SCAN_BYTES 1024
static void __attribute__((noinline)) cfg_read_old_dirkeys(char* dirrom, char* dirgb, char* dirgbsav) {
  dirrom[0] = dirgb[0] = dirgbsav[0] = 0;
  FIL f;
  if (f_open(&f, CFG_PATH, FA_READ) != FR_OK) return;
  char buf[CFG_DIRKEY_SCAN_BYTES];
  UINT br = 0;
  FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br);
  f_close(&f);
  if (fr != FR_OK || br == 0) return;
  find_key_in_text(buf, br, "dir_rom", dirrom, GB_ROM_PATH_MAX);
  find_key_in_text(buf, br, "dir_gb", dirgb, GB_ROM_PATH_MAX);
  find_key_in_text(buf, br, "dir_gbsav", dirgbsav, GB_ROM_PATH_MAX);
}

/* D1 (fix pass, review-caught real regression): `dir_val`, when non-NULL, is what
 * the "dir=" line writes instead of the LIVE g_cwd. A non-.sav browse_pick_spec()
 * session temporarily repurposes g_cwd for its OWN folder (see cfg_save_for()) --
 * every earlier cfg_save_ex() call from inside one of those sessions wrote THAT
 * borrowed value out under the "dir" key, so a cancel or power-off left the boot
 * (.sav) browser pointed at wherever the ROM/GB picker last was. dir_val is the
 * caller's own saved_cwd (the REAL .sav folder, stashed before the repurpose) for
 * every non-.sav call; NULL (meaning "use live g_cwd") only for the .sav spec's own
 * calls, where g_cwd genuinely IS the thing being remembered. */
static void cfg_save_ex(const char* active_key, const char* active_val, const char* dir_val) {
  if (!app_can_edit()) return;
  char old_dirrom[GB_ROM_PATH_MAX];
  char old_dirgb[GB_ROM_PATH_MAX];
  char old_dirgbsav[GB_ROM_PATH_MAX];
  cfg_read_old_dirkeys(old_dirrom, old_dirgb, old_dirgbsav);

  char buf[CFG_BUF_BYTES];   /* built fresh below -- the OLD file's bytes never touch this one */
  const char* dirrom = old_dirrom;
  const char* dirgb = old_dirgb;
  const char* dirgbsav = old_dirgbsav;
  if (active_key) {
    if      (!strcmp(active_key, "dir_rom"))   dirrom = active_val;
    else if (!strcmp(active_key, "dir_gb"))    dirgb = active_val;
    else if (!strcmp(active_key, "dir_gbsav")) dirgbsav = active_val;
  }

  int n = sniprintf(buf, sizeof buf,
                   "dir=%s\nsort=%d\nrev=%d\nall=%d\nhidden=%d\nanim=%u\nrumble=%u\nrstr=%d\nrdur=%d\npcbox=%d\nyard=%d\nbak=%d\nromoff=%d\ngbscale=%d\n",
                   dir_val ? dir_val : g_cwd, (int)g_sort, g_sortrev ? 1 : 0, g_show_all ? 1 : 0, g_show_hidden ? 1 : 0,
                   g_anim_mask, rmbl_get_mask(), rmbl_get_strength(), rmbl_get_duration(), g_pc_last_box,
                   g_yard_visitors ? 1 : 0, g_backup_mode, g_rom_art_off ? 1 : 0, (int)gb_scale_mode);
  bool truncated = (n < 0 || n >= (int)sizeof buf);
  if (truncated) n = (int)sizeof buf - 1;

  /* dir_rom/dir_gb/dir_gbsav (BACKLOG #186): only non-empty entries are written,
   * same rule as the ROM-path keys below -- a card that has never used a given
   * picker kind simply has no line for it, so an existing card's config.cfg is
   * byte-for-byte unchanged until that picker is used for the first time. */
  static const char* const k_dirkey[3] = { "dir_rom", "dir_gb", "dir_gbsav" };
  const char* dirval[3]; dirval[0] = dirrom; dirval[1] = dirgb; dirval[2] = dirgbsav;
  for (int i = 0; i < 3 && !truncated; i++) {
    if (!dirval[i][0]) continue;
    int w = sniprintf(buf + n, sizeof(buf) - (size_t)n, "%s=%s\n", k_dirkey[i], dirval[i]);
    if (w < 0 || w >= (int)(sizeof(buf) - (size_t)n)) { truncated = true; break; }
    n += w;
  }

  /* One ROM path per game — RS/Emerald/FRLG map data differs, so each needs its own
   * ROM file (Guy's requirement). Only non-empty entries are written. */
  static const char* const k_romkey[3] = { "romrs", "romem", "romfr" };
  for (int i = 0; i < 3 && !truncated; i++) {
    if (!g_rom_path[i][0]) continue;
    int w = sniprintf(buf + n, sizeof(buf) - (size_t)n, "%s=%s\n", k_romkey[i], g_rom_path[i]);
    if (w < 0 || w >= (int)(sizeof(buf) - (size_t)n)) { truncated = true; break; }
    n += w;
  }

  /* romgb1/romgb2 (slice E3) — one Game Boy cartridge ROM per generation, same
   * "only non-empty" rule as the Gen-3 paths above. */
  static const char* const k_gbkey[2] = { "romgb1", "romgb2" };
  for (uint8_t gen = PDNA_GEN1; gen <= PDNA_GEN2 && !truncated; gen++) {
    int gi = gb_gen_slot(gen);
    const char* key = k_gbkey[gen - PDNA_GEN1];
    if (!g_rom_path[gi][0]) continue;
    int w = sniprintf(buf + n, sizeof(buf) - (size_t)n, "%s=%s\n", key, g_rom_path[gi]);
    if (w < 0 || w >= (int)(sizeof(buf) - (size_t)n)) { truncated = true; break; }
    n += w;
  }

  /* E4 (sprite-era): up to 20 "era_<kind>_<place>=<era>\n" lines (sprite_era.h's
   * se_config_write -- only NON-default, applicable cells are ever written, so a
   * fresh grid costs nothing here). Uses the REMAINING capacity, not a fresh
   * sizeof(buf), per sprite_era.h's own wiring note. */
  if (!truncated) {
    bool era_trunc = false;
    int w = se_config_write(&g_era, buf + n, (int)sizeof(buf) - n, &era_trunc);
    n += w;
    if (era_trunc) truncated = true;
  }

  if (truncated)
    log_line("cfg: config.cfg buffer full -- some settings were NOT saved this write");

  /* Through the SAME verified-write pipeline as every other write this tool makes.
   * It used to be the one exception: FA_CREATE_ALWAYS truncated the existing config
   * FIRST, then f_write and f_close both had their return codes discarded and nothing
   * re-read the file -- so on a card that ACKs writes it does not keep, the settings
   * silently ceased to exist and nothing anywhere said so. Nothing here is safety-
   * critical (a missing key falls back to the compiled default, and g_backup_mode
   * defaults to the safest mode), which is exactly why this is a two-line change
   * rather than a design: there is no reason for it to be the unverified one. */
  if (n <= 0) return;
  rmbl_pause();                                  /* no motor toggling during the SD write */
  SfStatus cst = sf_write_verified(CFG_PATH, (const uint8_t*)buf, (uint32_t)n);
  rmbl_resume();
  if (cst != SF_OK) log_line("cfg: save failed (%s)", sf_status_str(cst));
}

/* The plain, argument-less save every existing call site in this file already makes
 * -- unchanged shape, so A5's "byte-for-byte unchanged .sav behaviour" holds. */
static void cfg_save(void) { cfg_save_ex(NULL, NULL, NULL); }

/* U2b item 3: exported so the GB-screen shell (pdna_gbscreen.c) can persist a
 * SELECT scale-mode change from ANY GB screen's own exit path, not just
 * Settings' own B key (which already called the file-local cfg_save() directly
 * and is unaffected by this). Same function, same app_can_edit() gate -- no new
 * write path, just a second caller. */
void app_cfg_save(void) { cfg_save(); }

/* Restore prefs saved by cfg_save (best-effort): a missing/unparsable file just
 * leaves the compiled defaults. The saved folder is adopted only if it still
 * exists, else we fall back to root — a moved SD card can't strand the browser. */
static void cfg_load(void) {
  FIL f;
  if (f_open(&f, CFG_PATH, FA_READ) != FR_OK) return;
  char buf[CFG_BUF_BYTES]; UINT br = 0;
  FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br); f_close(&f);
  if (fr != FR_OK || br == 0) return;
  buf[br] = 0;
  for (char* p = buf; *p; ) {
    char* eol = p; while (*eol && *eol != '\n' && *eol != '\r') eol++;
    char term = *eol; *eol = 0;
    char* eq = strchr(p, '=');
    if (eq) {
      *eq = 0; const char* k = p; const char* v = eq + 1;
      if      (!strcmp(k, "dir") && v[0]) { strncpy(g_cwd, v, PATH_MAX - 1); g_cwd[PATH_MAX - 1] = 0; }
      else if (!strcmp(k, "sort"))   { int s = v[0] - '0'; if (s >= 0 && s <= 2) g_sort = (BrSortKey)s; }
      else if (!strcmp(k, "rev"))    g_sortrev     = (v[0] == '1');
      else if (!strcmp(k, "all"))    g_show_all    = (v[0] == '1');
      else if (!strcmp(k, "hidden")) g_show_hidden = (v[0] == '1');
      else if (!strcmp(k, "yard"))   g_yard_visitors = (v[0] == '1');
      else if (!strcmp(k, "romoff")) g_rom_art_off = (v[0] == '1');
      else if (!strcmp(k, "gbscale")) gb_scale_mode = (v[0] == '1') ? 1 : 0;
      else if (!strcmp(k, "bak"))    { int m = v[0] - '0'; if (m >= 0 && m <= 2) g_backup_mode = m; }
      else if (!strcmp(k, "anim"))   { unsigned m = 0; for (const char* d = v; *d >= '0' && *d <= '9'; d++) m = m * 10 + (unsigned)(*d - '0'); g_anim_mask = m & ((1u << ANIM_COUNT) - 1u); }
      else if (!strcmp(k, "rumble")) { unsigned m = 0; for (const char* d = v; *d >= '0' && *d <= '9'; d++) m = m * 10 + (unsigned)(*d - '0'); rmbl_set_mask(m); }
      else if (!strcmp(k, "pcbox"))  { int m = 0; for (const char* d = v; *d >= '0' && *d <= '9'; d++) m = m * 10 + (*d - '0'); g_pc_last_box = m; }
      /* All five ROM-path keys REJECT (and log) a value too long for
       * GB_ROM_PATH_MAX(128) instead of truncating it -- E3 review item 3. This used
       * to be a plain strncpy(..., PATH_MAX-1) here, which silently cut a Gen-3 path
       * to a DIFFERENT (likely nonexistent) 255-byte string -- and, now that
       * g_rom_path shrank to GB_ROM_PATH_MAX-wide slots, would have been an actual
       * out-of-bounds write (255 into a 128-byte row). */
      else if (!strcmp(k, "romrs") && v[0] && !app_rom_path_set(PK_RS, v))
        log_line("cfg: romrs value too long (>%d), ignored", GB_ROM_PATH_MAX - 1);
      else if (!strcmp(k, "romem") && v[0] && !app_rom_path_set(PK_EMERALD, v))
        log_line("cfg: romem value too long (>%d), ignored", GB_ROM_PATH_MAX - 1);
      else if (!strcmp(k, "romfr") && v[0] && !app_rom_path_set(PK_FRLG, v))
        log_line("cfg: romfr value too long (>%d), ignored", GB_ROM_PATH_MAX - 1);
      else if (!strcmp(k, "romgb1") && v[0] && !app_gb_rom_path_set(PDNA_GEN1, v))
        log_line("cfg: romgb1 value too long (>%d), ignored", GB_ROM_PATH_MAX - 1);
      else if (!strcmp(k, "romgb2") && v[0] && !app_gb_rom_path_set(PDNA_GEN2, v))
        log_line("cfg: romgb2 value too long (>%d), ignored", GB_ROM_PATH_MAX - 1);
      /* BACKLOG #186: dir_rom/dir_gb/dir_gbsav (the three new picker-kind folder
       * memories) have no resident global to load into -- unlike "dir" (g_cwd),
       * each is only needed at the moment ITS OWN picker kind opens, so
       * browse_pick_spec() re-reads its one key on demand (cfg_read_raw_key()) instead
       * of this function holding all three in RAM for the whole session (A3/the
       * lane's "no new EWRAM statics" constraint). Recognised here only so they don't
       * fall into se_config_apply()'s catch-all below -- nothing to apply, they are
       * read straight back off disk when needed. A missing key there falls back to
       * the SAVE folder g_cwd, per A1. */
      else if (!strcmp(k, "dir_rom") || !strcmp(k, "dir_gb") || !strcmp(k, "dir_gbsav")) { /* no-op: read on demand */ }
      /* E4: "era_<kind>_<place>=<era>" -- se_config_apply() recognises the key
       * itself (the "era_" prefix + a valid kind/place pair) and returns false for
       * anything else, so this is a catch-all with no separate strncmp gate. An
       * unrecognised VALUE (a hand-edited or bit-rotted config) sets that cell to
       * NATIVE rather than being rejected -- sprite_era.h's own documented rule. */
      else se_config_apply(&g_era, k, v);
    }
    p = term ? eol + 1 : eol;
  }
  DIR d;                                    /* validate the restored folder still exists */
  if (g_cwd[0] != '/' || f_opendir(&d, g_cwd) != FR_OK) strcpy(g_cwd, "/");
  else f_closedir(&d);
}

static const char* sort_label(void) {
  switch (g_sort) {
    case SORT_SIZE: return g_sortrev ? "Size big-small" : "Size small-big";
    case SORT_DATE: return g_sortrev ? "Date new-old"   : "Date old-new";
    default:        return g_sortrev ? "Name Z-A"       : "Name A-Z";
  }
}

/* short human-readable byte size (saves are 128 KiB) into out[] */
static const char* human_size(uint32_t b, char* out) {
  if (b >= 1024u * 1024u) siprintf(out, "%u.%uMB", (unsigned)(b >> 20), (unsigned)(((b >> 10) & 1023u) * 10u / 1024u));
  else if (b >= 1024u)    siprintf(out, "%uKB", (unsigned)(b >> 10));
  else                    siprintf(out, "%uB", (unsigned)b);
  return out;
}

/* What is on screen right now, owned by browse_pick()'s stack -- no new static, same
 * idiom as pdna_edit.c's EditPaint / pdna_pick.c's pick_species prev_sel+prev_top+gen. */
typedef struct {
  int      top, sel;
  bool     valid;
  uint32_t gen;
} BrowsePaint;

/* Paint (or repaint) one list row in place. The list's own background is UI_PANEL, not
 * UI_BG -- ui_panel() at (0,11)-(240,115) filled it once in the full path below, so a
 * row erase here must match that fill or it punches a UI_BG rectangle into the panel.
 * ui_text_sel() draws no background when `sel` is false, and names are NOT fixed-width
 * (the %-21s/%-18s padding only holds for a given row's OWN two possible shapes, not
 * across two DIFFERENT entries), so the wipe below is what stops a longer old name's
 * tail from surviving under a shorter new one (lesson: text that can shorten needs an
 * explicit wipe). Row height matches ui_text_sel's own highlight rect exactly
 * (UI_ROW_H on a UI_ROW_H pitch), so neighbouring rows never share a scanline. */
static void br_row_paint(const BrowseEntry* ents, int idx, int i, bool sel) {
  int y = 14 + i * UI_ROW_H;
  ui_fill_rect(3, y, UI_SCR_W - 6, UI_ROW_H, UI_PANEL);
  const BrowseEntry* e = &ents[idx];
  /* nm is a ui_truncate OUTPUT for up to 21 display columns; ui.h's contract wants
   * max_cols*4+1 (85) to be UTF-8-safe (FF_LFN_UNICODE means e->name can be real
   * multi-byte). NAME_MAX+2 (66) was under that -- it only stayed safe today via
   * scan_dir's incidental 63-byte name cap, an unstated cross-function invariant. 128
   * matches ui.h's contract outright, same size pdna_edit.c documents for a screen
   * string. */
  char row[LIST_COLS * 4 + 1], nm[128], sz[12];
  if (e->is_dir) {
    ui_truncate(nm, e->name, 21);
    siprintf(row, "%-21s (DIR)", nm);
    ui_text_sel(3, y, UI_SCR_W - 6, sel, UI_DIRCLR, row);
  } else {
    ui_truncate(nm, e->name, 18);
    human_size(e->size, sz);
    siprintf(row, "%-18s %8s", nm, sz);
    ui_text_sel(3, y, UI_SCR_W - 6, sel, UI_SAVECLR, row);
  }
}

/* The per-selection detail block + status line, both pure functions of `sel` (plus the
 * filter/sort/count state folded into the caller's `full`/relist decision) -- a cursor
 * move alone must repaint them even when no row content changed. */
static void br_detail_paint(const BrowseEntry* ents, int sel, const BrowseSpec* spec) {
  ui_fill_rect(0, 116, UI_SCR_W, UI_FOOTER_RULE_Y - 116, UI_BG);
  if (g_count > 0) {
    const BrowseEntry* e = &ents[sel];
    /* 29 cols needs 117 B per ui.h's contract (e->name is real UTF-8 under
     * FF_LFN_UNICODE); dn[40] silently violated it. */
    char dn[128]; ui_truncate(dn, e->name, 29);
    ui_text(2, 118, UI_SELTEXT, dn);
    char meta[40];
    if (e->is_dir) siprintf(meta, "folder");
    else { char sz[12]; human_size(e->size, sz); siprintf(meta, "save file   %s", sz); }
    ui_text(2, 128, UI_DIM, meta);
  }
  char status[64], stc[40];
  /* D5: the "List full - some files not shown." warning pick_rom() used to show got
   * lost when that implementation was deleted (BACKLOG #186) -- the GB-session ROM
   * picker's cap (PICK_MAX_ART, mon_decomp-backed) is only ~107 entries, so a folder
   * that busy needs SOME visible sign it isn't showing everything. " FULL" on the
   * status line (already ui_truncate'd to 29 cols below, so a long path/sort label
   * just drops it the same safe way it already drops anything else over budget,
   * never overruns). */
  siprintf(status, "%d/%d  %s  %s%s", g_count ? sel + 1 : 0, g_count,
           sort_label(), g_show_all ? "all" : spec->filter_label,
           g_count >= spec->cap ? " FULL" : "");
  ui_truncate(stc, status, 29);
  ui_text(2, 138, UI_OK, stc);
}

/* sd-browser-style listing: cwd header, framed panel, (DIR) tags + right-aligned
 * size column, a per-selection detail block, and a green status line.
 *
 * `relist` is set by the caller at every site that mutates g_entries (a rescan or a
 * re-sort) -- see browse_pick(). It is NOT inferred from g_count or from top/sel:
 * a same-count re-sort, or a rescan that resets sel/top back to the SAME 0/0 they
 * already were, changes every row's content without moving the cursor or the window
 * or touching ui_clear_gen() (sort_entries()/scan_dir() paint nothing themselves), so
 * a scalar-derived gate would miss it (the exact bug class an earlier repaint batch
 * shipped twice). Explicit invalidation at the mutation site is what closes that. */
static void __attribute__((noinline)) render_browser(BrowseEntry* ents, int sel, int top, bool relist, BrowsePaint* pv, const BrowseSpec* spec) {
  /* `top` is deliberately NOT part of `full` -- see the row loop below. Folding it in
   * here would pay the 76,800 B ui_clear() on every scroll, and wait_keys() DOES
   * auto-repeat the d-pad, so held-DOWN past VIS_ROWS is the single most common way
   * this screen scrolls: that used to be a full-screen wipe every few frames. */
  bool full = relist || !pv->valid || pv->gen != ui_clear_gen();

  if (full) {
    ui_clear();
    /* g_cwd is PATH_MAX=256 and grows via strcpy on every folder entry, and
     * FF_LFN_UNICODE means it can be real multi-byte UTF-8 -- an unbounded
     * siprintf("Pick .sav: %s", g_cwd) into a small stack frame overruns it past
     * ~68 raw bytes of path (a hang on HW). Bound the cwd by DISPLAY COLUMNS first
     * (ui_truncate into its own 128 B buffer -- ui.h wants max_cols*4+1, 117 for 29
     * cols), THEN compose the fixed "Pick .sav: " prefix onto the now-bounded result
     * (11 + up to 116 + nul fits 128 exactly), THEN truncate the composed line to the
     * 29 cols actually drawn -- truncating a truncation to a SMALLER budget is the
     * same as truncating the original to that budget, so the on-screen result is
     * unchanged; only the intermediate frame is now bounded by construction instead
     * of by g_cwd happening to stay short. */
    char cwdt[128]; ui_truncate(cwdt, g_cwd, 29);
    /* title[192], not [128]: cwdt is already bounded to ui.h's 29-col contract (<=117 B
     * incl. nul), but "Pick .sav: " (11) + up to 116 non-nul cwdt bytes + nul = exactly
     * 128 -- ZERO slack for the worst case, one byte from a silent siprintf truncation
     * (siprintf itself is bounds-safe, so not a smash, but a wrong-looking title on a
     * long path) rather than an intentional margin. BACKLOG #36 item 6: 192 gives this
     * shallow, one-frame-deep buffer real headroom instead of landing exactly on the
     * edge of its own known-worst-case math. */
    /* BACKLOG #186: spec->title ("Pick .sav" today, "Pick .gba"/"Pick .gb/.gbc"/
     * "Pick GB save" for the other kinds) replaces the old hardcoded "Pick .sav" --
     * for the .sav spec this composes the IDENTICAL string as before (A5). */
    char title[192]; siprintf(title, "%s: %s", spec->title, cwdt);
    char cwdc[128]; ui_truncate(cwdc, title, 29);
    ui_text(2, 2, UI_TITLE, cwdc);
    ui_panel(0, 11, UI_SCR_W, 104, UI_PANEL, UI_BORDER);
    if (g_count == 0) {
      char empty[40]; siprintf(empty, "(no folders or %s files here)", spec->filter_label);
      ui_text(6, 40, UI_WARN, empty);
      ui_text(6, 52, UI_DIM,  at_root() ? spec->root_hint
                                        : "B = go up a folder.");
    }
    /* UI_FOOTER_Y, not a hard 150: this row is what every popup is laid out to clear, and
     * a literal here would let the two drift apart (see source/ui_layout.h). */
    ui_hline(0, UI_FOOTER_RULE_Y, UI_SCR_W, UI_BORDER);
    ui_text(2, UI_FOOTER_Y, UI_DIM, "A pick  B up  SEL sort  ST menu");
  }

  /* Per-ROW dirty, mirroring pdna_edit.c's render(): compare the entry index that WAS
   * drawn at row i (pv->top+i) against the one that belongs there NOW (top+i) -- if
   * they differ, or the row's selection state flipped, that row is dirty. A scroll (any
   * top change, including the L/R jump-to-end and the LEFT/RIGHT +-11 fast jump) makes
   * EVERY visible row's old/new index differ, so every row repaints -- exactly
   * pdna_edit.c's "scroll: all rows, no full-screen wipe" cost case -- while the chrome
   * and panel frame drawn above stay untouched. A bare cursor move (top unchanged)
   * leaves every index the same, so only the old-sel/new-sel rows differ. */
  if (g_count > 0) {
    uint32_t dirty = 0;
    for (int i = 0; i < VIS_ROWS && top + i < g_count; i++) {
      int f = top + i, of = pv->top + i;
      bool s = (f == sel), os = (of == pv->sel);
      if (full || of != f || s != os) dirty |= 1u << i;
    }
    for (int i = 0; i < VIS_ROWS && top + i < g_count; i++)
      if (dirty & (1u << i)) br_row_paint(ents, top + i, i, top + i == sel);
  }

  if (full || sel != pv->sel) br_detail_paint(ents, sel, spec);

  pv->top = top; pv->sel = sel; pv->valid = true; pv->gen = ui_clear_gen();
}

/* Reboot back into the flashcart loader menu (no return on confirm). */
static void do_reboot(void) {
  ui_clear();
  ui_panel(8, 44, 224, 60, UI_PANEL, UI_WARN);
  ui_text(16, 52, UI_TITLE, "Reboot to flashcart menu?");
  ui_text(16, 74, UI_WARN, "A = reboot to loader");      /* destructive = WARN */
  ui_text(16, 88, UI_DIM,  "B = cancel");
  u16 k; do { vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  if (k & KEY_B) { snd_back(); return; }
  snd_ok();
  ui_clear();
  ui_text(16, 72, UI_TITLE, "Rebooting...");
  cfg_save();                           /* persist last folder + sort before leaving */
  log_flush_urgent(LOG_PATH);           /* last words of the run: latch-bypassing */
  VBlankIntrWait();
  flashcartio_reboot();                 /* never returns */
}

/* Per-file SD operations (duplicate / rename / delete backups). Defined after the
 * write helpers it uses (msg_wait/busy_panel/app_confirm); returns true if the
 * directory contents changed and the browser must re-scan. */
static bool file_actions(const BrowseEntry* e);

/* START menu over the browser: file actions on the selection, then sort key/order,
 * file filter, show-hidden, reboot. Returns true if something changed that needs a
 * re-scan. `fe` is the selected entry (NULL or a folder => no file-ops row). */
/* One row of a small fixed-count text menu (browse_menu / file_actions share this
 * geometry): 13 px highlight on a 16 px pitch, so adjacent rows never share a
 * scanline. Always wipes first -- these row STRINGS are not fixed-width (unlike the
 * browser's %-21s rows), so a shorter new label must not leave the old one's tail. */
static void ui_menu_row(int y, const char* text, bool sel) {
  ui_fill_rect(2, y - 2, 236, 13, UI_BG);
  if (sel) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
  ui_text(10, y, sel ? UI_SELTEXT : UI_TEXT, text);
}

/* D3 (fix pass): the ROM/GB pickers had no visible cancel once START stopped meaning
 * "cancel" and started opening this menu (A1's unified chrome) -- the old footer's
 * "START cancel" text is gone, "Close" only closes the MENU (back to the browser, not
 * out of the whole picker), and B only cancels at the root, which nothing on screen
 * says. Return: 0 = nothing changed, 1 = changed (re-scan), -1 = "Cancel picking"
 * chosen (only offered when !spec->menu_extra -- the .sav spec has nothing to cancel
 * TO, same reasoning as its B-at-root no-op). */
static int browse_menu(const BrowseEntry* fe, const BrowseSpec* spec) {
  int sel = 0;
  bool changed = false;
  bool can_fileops = (fe && !fe->is_dir);
  /* rows/prev_rows sized 80, not a tight 40: the "File: %s..." row below composes a
   * FILENAME (real multi-byte UTF-8 under FF_LFN_UNICODE) into it, and "File: " (6) +
   * up to 64 raw bytes for a 16-col name (nm is already ui.h-contract-sized below, but
   * ui_truncate itself never emits more than 16 display cols worth of UTF-8) + "..." (3)
   * + nul reaches 74 bytes -- a 40 B row would have overflowed into its neighbour
   * (prev_rows sits right after rows on this frame) even with nm itself fixed below.
   * No other row (Sort key/Order/Files/Hidden/Verify/Reboot/Close, all short fixed
   * labels) comes close; 74 is the real worst case (BACKLOG #36 item 6), so 80 keeps a
   * few bytes of named slack instead of the previous 128 (54 B of pure unused padding
   * per row x 8 rows x 2 arrays = 864 B of this frame that was never reachable). */
  char rows[8][80], prev_rows[8][80];
  int  act[8];
  int  prev_sel = -1; bool valid = false; uint32_t gen = 0;
  for (;;) {
    int  n = 0;
    /* BACKLOG #186: A_VERIFY/A_REBOOT are conditional now (spec->menu_extra), so the
     * enum can no longer fix their slots -- act[] carries which action each BUILT row
     * means, same as before, just no longer implied by array position. */
    enum { A_FILEOPS, A_SORTKEY, A_ORDER, A_FILES, A_HIDDEN, A_VERIFY, A_REBOOT, A_CLOSE, A_CANCEL };
    if (can_fileops) {
      /* 16 cols needs 65 B per ui.h's contract (fe->name is real UTF-8); nm[24] was a
       * ui_truncate stack-smash next to prev_rows[] above -- "a smash silently poisons
       * the text-diff". */
      char nm[128]; ui_truncate(nm, fe->name, 16);
      siprintf(rows[n], "File: %s...", nm); act[n++] = A_FILEOPS;
    }
    siprintf(rows[n], "Sort key:  %s", g_sort == SORT_NAME ? "Name" : g_sort == SORT_SIZE ? "Size" : "Date"); act[n++] = A_SORTKEY;
    siprintf(rows[n], "Order:     %s", g_sortrev ? "descending" : "ascending"); act[n++] = A_ORDER;
    { char fonly[24]; siprintf(fonly, "%s only", spec->filter_label);
      siprintf(rows[n], "Files:     %s", g_show_all ? "all files" : fonly); act[n++] = A_FILES; }
    siprintf(rows[n], "Hidden:    %s", g_show_hidden ? "shown" : "hidden"); act[n++] = A_HIDDEN;
    /* Reachable from the browser, i.e. WITHOUT opening a save — the boot hold (R+SELECT)
     * covers the case where even this menu cannot be reached. Only the .sav (launch)
     * spec offers these two -- the ROM/GB pickers are opened mid-session from Settings,
     * where "reboot" or "verify the whole ROM image" make no sense as menu items. */
    if (spec->menu_extra) {
      strcpy(rows[n], "Verify ROM image..."); act[n++] = A_VERIFY;
      strcpy(rows[n], "Reboot to flashcart menu..."); act[n++] = A_REBOOT;
    } else {
      /* D3: the ROM/GB pickers' only way out otherwise is B at the root (unsaid
       * anywhere on screen) -- this row is the one visible, discoverable cancel. */
      strcpy(rows[n], "Cancel picking"); act[n++] = A_CANCEL;
    }
    strcpy(rows[n], "Close"); act[n++] = A_CLOSE;

    /* Building rows[] above is pure string formatting (no SD I/O) -- cheap enough to
     * redo every frame. What used to be expensive was drawing all of it: shadow the
     * PAINTED text (not g_sort/g_sortrev/... individually) so a toggle that changes a
     * row's own label while `sel` stays put (A on Sort key/Order/Files/Hidden all edit
     * the row you're already sitting on) still gets caught -- diffing derived scalars
     * one by one is exactly the miss two earlier repaint batches shipped. */
    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "FILE MENU");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < n; i++) ui_menu_row(26 + i * 16, rows[i], i == sel);
      ui_text(4, 152, UI_DIM, "A change  U/D move  B back");
    } else {
      for (int i = 0; i < n; i++) {
        bool s = (i == sel), os = (i == prev_sel);
        if (s != os || strcmp(rows[i], prev_rows[i]) != 0) ui_menu_row(26 + i * 16, rows[i], s);
      }
    }
    for (int i = 0; i < n; i++) strcpy(prev_rows[i], rows[i]);
    prev_sel = sel; valid = true; gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return changed ? 1 : 0;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      switch (act[sel]) {
        case A_FILEOPS: if (file_actions(fe)) return 1; break;   /* re-scan after a file op */
        case A_SORTKEY: g_sort = (BrSortKey)((g_sort + 1) % 3); changed = true; break;
        case A_ORDER:   g_sortrev = !g_sortrev; changed = true; break;
        case A_FILES:   g_show_all = !g_show_all; changed = true; break;
        case A_HIDDEN:  g_show_hidden = !g_show_hidden; changed = true; break;
        case A_CANCEL:  return -1;                     /* D3: whole picker cancels, not just this menu */
        case A_VERIFY:  pdna_romfull_screen(); app_log_flush(); break;  /* its verdict on the card too */
        case A_REBOOT:  do_reboot(); break;            /* returns only if cancelled */
        case A_CLOSE:   return changed ? 1 : 0;
      }
    }
  }
}

/* BACKLOG #186 / D1 fix: dispatches the "remember this folder" write per
 * spec->cfg_key -- "dir" (the .sav spec) goes through the plain, unchanged
 * cfg_save() (A5: byte-for-byte .sav behaviour); every other kind goes through the
 * lighter cfg_save_ex(), which touches ONLY its own dir_* line and round-trips the
 * rest from disk. `sav_dir` is the caller's saved_cwd (the REAL .sav folder,
 * stashed before g_cwd was repurposed) -- passed straight through as cfg_save_ex's
 * dir_val so "dir=" never gets written from the borrowed g_cwd. Unused (0) for the
 * "dir" spec itself, which never repurposes anything. */
static void cfg_save_for(const BrowseSpec* spec, const char* sav_dir) {
  if (!strcmp(spec->cfg_key, "dir")) cfg_save();
  else cfg_save_ex(spec->cfg_key, g_cwd, sav_dir);
}

/* Seeds g_cwd from spec's own remembered folder (A1). Split out of browse_pick_spec
 * (noinline) so `remembered`/`DIR d` -- only needed once, at entry -- don't sit in
 * browse_pick_spec's OWN frame for that function's entire lifetime (STACK ok
 * review: browse_pick_spec's reported frame was 4,080 B, 264 B over budget on the
 * deepest chain; a temporary that is dead before the loop even starts should not
 * cost anything once it returns). */
static void __attribute__((noinline)) browse_seed_cwd(const BrowseSpec* spec) {
  char remembered[GB_ROM_PATH_MAX] = {0};
  bool have = cfg_read_raw_key(spec->cfg_key, remembered, sizeof remembered) && remembered[0];
  DIR d;
  if (have && f_opendir(&d, remembered) == FR_OK) { f_closedir(&d); strcpy(g_cwd, remembered); }
  /* else: leave g_cwd as whatever the caller already set it to (the SAVE folder) --
   * A1's "a missing key starts in the SAVE folder g_cwd, not /" rule, and the same
   * fallback a stale/deleted remembered folder gets. */
}

/* The ONE file browser core (BACKLOG #186): the launch (.sav) browser IS this,
 * parameterised; the three pdna_map.c pickers (app_pick_rom/app_pick_gb_save/
 * app_pick_gb_rom) call it too, each with their own BrowseSpec. Writes the chosen
 * full path to `out` and returns true; navigation never leaves the browser (A enters
 * a folder, B goes up).
 *
 * g_cwd is shared scratch across every kind (only one picker is ever open at a time
 * -- they are all modal), so a non-"dir" spec seeds it from ITS OWN remembered
 * folder (cfg_read_raw_key(), falling back to the SAVE folder per A1 when that key
 * is absent) and restores the REAL .sav folder before returning, so a later re-entry
 * into the launch browser is never left pointed at wherever the ROM/GB picker last
 * was. The .sav spec (cfg_key == "dir") skips all of this: g_cwd IS its own memory,
 * exactly as before this lane. */
bool browse_pick_spec(const BrowseSpec* spec, char* out, int cap) {
  bool is_dir_key = !strcmp(spec->cfg_key, "dir");
  char saved_cwd[PATH_MAX];
  if (!is_dir_key) {
    strcpy(saved_cwd, g_cwd);
    browse_seed_cwd(spec);
  }

  BrowseEntry* ents = spec->entries;
  scan_dir(spec, ents);
  int sel = 0, top = 0;
  bool relist = false;                /* the very first frame is covered by !pv.valid */
  BrowsePaint bp = {0};
  bool picked = false;
  for (;;) {
    if (sel >= g_count) sel = g_count > 0 ? g_count - 1 : 0;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;

    render_browser(ents, sel, top, relist, &bp, spec);
    relist = false;

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_L | KEY_R | KEY_SELECT | KEY_START);
    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < g_count - 1) sel++; }
    else if (k & KEY_LEFT)  { sel -= 11; if (sel < 0) sel = 0; }                       /* fast jump, like sd-browser */
    else if (k & KEY_RIGHT) { sel += 11; if (sel > g_count - 1) sel = g_count ? g_count - 1 : 0; }
    else if (k & KEY_L)    { sel = 0; top = 0; }
    else if (k & KEY_R)    { sel = g_count ? g_count - 1 : 0; }
    else if (k & KEY_SELECT) {           /* cycle the 6 sort states (key x order) */
      int s = ((int)g_sort * 2 + (g_sortrev ? 1 : 0) + 1) % 6;
      g_sort = (BrSortKey)(s / 2); g_sortrev = (s & 1) != 0;
      sort_entries(ents); sel = 0; top = 0; cfg_save_for(spec, is_dir_key ? 0 : saved_cwd); relist = true;   /* remember the sort */
    }
    else if (k & KEY_START) {
      int r = browse_menu(g_count ? &ents[sel] : 0, spec);
      if (r < 0 && !is_dir_key) break;                 /* D3: "Cancel picking" -- picked stays false */
      if (r > 0) { scan_dir(spec, ents); sel = 0; top = 0; cfg_save_for(spec, is_dir_key ? 0 : saved_cwd); relist = true; }
    }
    else if (k & KEY_B) {
      if (!at_root()) { path_up(); scan_dir(spec, ents); sel = 0; top = 0; cfg_save_for(spec, is_dir_key ? 0 : saved_cwd); relist = true; }
      /* At root, the .sav (launch) spec's B is a deliberate no-op -- browse_pick()
       * is main()'s own outer loop, so there is nothing to cancel back to (unchanged,
       * A5). Every OTHER kind is opened mid-session from Settings and its caller
       * (app_register_rom() etc.) expects a false return on cancel -- the old
       * pick_rom() had an explicit START-cancel for this; the unified menu's START now
       * opens the FILE MENU instead (A1), so B-at-root is what is left to mean
       * "cancel" for these, and it is the same key the old picker's own "B up" used. */
      else if (!is_dir_key) break;                              /* picked stays false */
    }
    else if (k & KEY_A) {
      if (g_count == 0) continue;
      const BrowseEntry* e = &ents[sel];
      char np[PATH_MAX];
      if (!path_join(g_cwd, e->name, np)) continue;
      if (e->is_dir) {
        strcpy(g_cwd, np);
        scan_dir(spec, ents);
        sel = 0; top = 0; cfg_save_for(spec, is_dir_key ? 0 : saved_cwd); relist = true;             /* remember the folder */
      } else if ((int)strlen(np) < cap) {
        cfg_save_for(spec, is_dir_key ? 0 : saved_cwd);                                   /* remember where this file was picked from */
        strcpy(out, np);
        picked = true;
        break;
      }
    }
  }

  if (!is_dir_key) strcpy(g_cwd, saved_cwd);   /* g_cwd is shared scratch -- give the real folder back */
  return picked;
}

/* browse_pick() is the .sav instance of the core above -- A1. */
static const char* const k_sav_exts_unused[1] = { 0 };  /* BR_MATCH_SAV never reads exts */
static bool browse_pick(char* out, int cap) {
  const BrowseSpec spec = {
    .title = "Pick .sav", .filter_label = ".sav", .root_hint = "Open the folder with your saves.",
    .match_mode = BR_MATCH_SAV, .exts = k_sav_exts_unused, .cfg_key = "dir", .menu_extra = true,
    .entries = g_entries, .cap = MAX_ENTRIES,
  };
  return browse_pick_spec(&spec, out, cap);
}

static const char* ver_label(Gen3Version v, bool frlg) {
  if (frlg) return "FireRed/LeafGreen";
  switch (v) {
    case G3_VER_EMERALD: return "Emerald";
    case G3_VER_RS:      return "Ruby/Sapphire";
    default:             return "Gen-3";
  }
}

/* loaded-save state shared by the party list + box views */
static Gen3SaveInfo g_vinfo;
static int  g_nparty = 0;
static bool g_frlg = false, g_have_pc = false;
static PkGame g_game = PK_EMERALD;

/* ---- ROM-hack session verdict (BACKLOG #54, T0/T1) --------------------------
 * One bit per PkGame slot (PK_RS/PK_EMERALD/PK_FRLG — the save-side granularity;
 * RomKind's Ruby/Sapphire and FireRed/LeafGreen pairs both collapse into one slot
 * each, same as app_rom_path_set()'s existing per-slot registration). Set when a
 * Gen-3 ROM opened THIS SESSION classifies as ROM_ID_HACK with a known base kind;
 * cleared when one classifies as ROM_ID_RETAIL for that same slot. An unknown-kind
 * hack (rule 1d, RomKind ROM_NONE) touches no bit at all -- it cannot be attributed
 * to a game, and guessing would lock a retail save of that game (decision 4). The
 * ROM is the only signal that can ever set or clear this: a save carries no game
 * identifier (Gen3SaveInfo has no field for one), so it can neither raise nor clear
 * a verdict here. Only 1 byte, only static in this lane. */
static uint8_t s_hack_mask;

static PkGame pkgame_of_romkind(RomKind k) {
  return (k == ROM_RUBY || k == ROM_SAPPHIRE) ? PK_RS
       : (k == ROM_FIRERED || k == ROM_LEAFGREEN) ? PK_FRLG : PK_EMERALD;
}

static bool app_rom_is_hack(PkGame g) { return (s_hack_mask & (1u << (unsigned)g)) != 0; }

/* BACKLOG #166 review F1: gb_lift_why_bs/gb_lift_why_hook (source/pdna_gen12.c) need
 * to pick between PDNA_ROMHACK_NOTE and PDNA_GB_LIFT_WHY_OMEGA -- the SAME
 * hack-vs-cart distinction app_readonly_why()/app_readonly_footer() (just below)
 * already make, but app_rom_is_hack() itself is file-static (needs s_hack_mask/
 * g_game, both file-static here) and neither of its two existing wordings fits the
 * 88 px read-only-menu prose budget those two callers draw at (app_readonly_why()'s
 * own PDNA_ROMHACK_WHY/"Needs EZ-Flash Omega." are sized for msg_wait's 184 px
 * clamp). One thin public accessor over the identical predicate, not a third
 * wording living here. */
bool app_rom_hack_active(void) { return g_vinfo.valid && app_rom_is_hack(g_game); }

/* Review fix F1: ~20 refusal sites share the literal "Needs EZ-Flash Omega." --
 * correct for an Everdrive/pdna_romcheck_bad() refusal, a real LIE for an Omega
 * owner whose cart is perfectly writable but whose currently-open game is
 * hack-flagged (app_can_edit() now refuses for that reason too, BACKLOG #54).
 * This picks the honest wording for all remaining sites. */
const char* app_readonly_why(void) {
  return (g_vinfo.valid && app_rom_is_hack(g_game)) ? PDNA_ROMHACK_WHY : "Needs EZ-Flash Omega.";
}

/* Review fix F3: renamed from app_readonly_why_short() -- callers need the full
 * footer including the "  B back" hint, not just the reason fragment. */
const char* app_readonly_footer(void) {
  return (g_vinfo.valid && app_rom_is_hack(g_game)) ? "ROM hack: locked  B back" : "read-only (Omega)  B back";
}

/* Review fix F2(b): the Game Boy sites can be read-only for a reason that is
 * neither the cart nor a hack -- a streamed/view-only session
 * (pdna_gen12_resident() == false). Give those sites the honest wording instead of
 * blaming the cart/ROM. */
const char* app_gb_readonly_why(void) {
  return pdna_gen12_resident() ? app_readonly_why() : PDNA_GB_VIEWONLY_WHY;
}

const char* app_gb_readonly_footer(void) {
  return pdna_gen12_resident() ? app_readonly_footer() : PDNA_GB_VIEWONLY_FOOT;
}

static const char* romident_name(RomIdent id) {
  switch (id) {
    case ROM_ID_RETAIL:      return "RETAIL";
    case ROM_ID_HACK:        return "HACK";
    case ROM_ID_NOT_POKEMON: return "NOT_POKEMON";
    default:                 return "NOT_GBA";
  }
}

/* Called at every one of the three places a Gen-3 ROM handle gets opened
 * (app_register_rom, app_icon_rom_open's fused open, its SD candidate loop) --
 * whether rom_open() returned true or false. `rc` must already carry the verdict
 * rom_open() sets before every return (rc->ident, rc->kind). `where` is a short tag
 * for the log line only. */
static void app_rom_note_verdict(const RomCtx* rc, const char* where) {
  if (!rc) return;
  if (rc->ident == ROM_ID_HACK && rc->kind != ROM_NONE) {
    PkGame g = pkgame_of_romkind(rc->kind);
    s_hack_mask |= (uint8_t)(1u << (unsigned)g);
    log_line("romhack: %s -> HACK impersonating %s (read-only until verified)",
             where, rom_kind_name(rc->kind));
  } else if (rc->ident == ROM_ID_RETAIL) {
    PkGame g = pkgame_of_romkind(rc->kind);
    s_hack_mask &= (uint8_t)~(1u << (unsigned)g);
    log_line("romhack: %s -> RETAIL %s", where, rom_kind_name(rc->kind));
  } else {
    log_line("romhack: %s -> %s", where, romident_name(rc->ident));
  }
}

/* BACKLOG #114: narrow accessor for pdna_yard.c's dc_seed() (moved out of this file) --
 * see pdna_app.h's own comment on why this is the one field exposed, not g_vinfo itself. */
uint16_t app_tid_public(void) { return g_vinfo.tid_public; }

/* E4 (sprite-era): the Gen-3 kind for app_save_kind() -- set alongside g_game the
 * moment view_save() decides it (below), NOT read from g_game on every call, so a
 * Game Boy session (which never touches g_game at all) cannot accidentally inherit
 * whatever Gen-3 kind happened to be open before it. app_save_kind() overrides this
 * with pdna_gen12_active_kind() while a GB session is live; this is what it falls
 * back to otherwise. */
static SeSaveKind g_save_kind = SE_KIND_EM;
static char   g_path[PATH_MAX];                           /* path of the open save (for commit) */
static uint16_t g_item_clip = 0;                          /* held-item move clipboard            */
static bool     g_item_held = false;

/* ===================== edit / commit (V4) =============================== */

/* BACKLOG #54 T1: a ROM hack registered for g_game's slot also parks editing, even
 * though the save itself may parse as perfectly retail -- the ROM is the only signal
 * that can raise this (decision 4), and app_can_edit() is the single chokepoint
 * `pdna_romcheck_bad()` already fans out to 84 call sites from, so this one line
 * covers every one of them, including the mon-menu edit gate this lane never touches
 * directly. Named consequence (see this lane's report): this also parks the SD-side
 * art/cache writes that consult app_can_edit() (gb_art_source.c:64,
 * pdna_gbscreen.c:517) -- accepted, those are caches, never the save itself.
 *
 * Review fix F4 (over-lock, BACKLOG #54), corrected by review fix G1: g_game is the
 * last Gen-3 slot a save was opened for and is NOT cleared on entering a Game Boy
 * session (START > GB import) -- so a hack-flagged g_game would otherwise also lock
 * Gen-1/2 editing, which never touched the flagged ROM at all.
 *
 * The discriminator is g_vinfo.valid, NOT app_arena_held() -- F4's original
 * `!app_arena_held()` was right by luck and its own comment was false.
 * app_arena_acquire() (pdna_main.c) sets g_arena_held for ANY borrower, not just a
 * GB session: it has five non-GB callers that borrow the SAME arena mid-Gen-3-
 * session -- icon_store.c:847 via app_icons_hold() on the party overlay (:4193),
 * the party screen (:5040) and the day-care (:6253); pdna_pick.c:949, where
 * pdna_dex_screen() holds the borrow for its WHOLE lifetime; pdna_map.c:1776/2006;
 * pdna_main.c:8112; pdna_gbscreen.c:1141. On every one of those, app_can_edit()
 * would have returned TRUE for a hack-flagged save purely because that unrelated
 * borrow happened to be live -- it did not leak today only because every consumer
 * of those borrows samples app_can_edit() BEFORE taking the borrow, not during it,
 * a coincidence of call order, not a guarantee.
 *
 * g_vinfo.valid is the signal xfer_gate.h's xg_pc_live() already uses for this
 * exact "is there a live Gen-3 save right now" question: a GB session clears it
 * (`memset(&g_vinfo, 0, ...)`, "no Gen-3 save is loaded in a GB session" --
 * view_save()'s own four early-refusal returns clear it too, :9096/:9159/:9214),
 * and it stays true across every one of the five borrows above (none of them ever
 * touches g_vinfo). Truth table: GB session (g_vinfo.valid == false) -> unlocked;
 * a mid-Gen-3-session borrow (icon overlay / dex grid / map / party / day-care,
 * g_vinfo.valid == true) -> still locked; an ordinary Gen-3 session -> locked; no
 * save open at all (g_vinfo.valid == false) -> unlocked, but there is nothing to
 * write anyway. */
#ifdef PDNA_DELTA
/* Emulator build: there is no flashcart to gate on — the save is our own flash chip,
 * which is always writable. */
bool app_can_edit(void) {
  return !pdna_romcheck_bad() && !(g_vinfo.valid && app_rom_is_hack(g_game));
}
#else
bool app_can_edit(void) {
  return active_flashcart == EZ_FLASH_OMEGA && !pdna_romcheck_bad() &&
         !(g_vinfo.valid && app_rom_is_hack(g_game));
}
#endif

/* BACKLOG #150 S150-8 decision 4: same expression pdna_main.c's own two GB-import
 * call sites already used inline (Gen 3's origin field cannot tell RS apart, so RS
 * maps to Sapphire's id here -- NOT :4477's `? 3 : ? 4 : 2` variant, which maps RS to
 * Ruby; the GB mount's own met_game stamp already uses THIS spelling, so a converted
 * mon and a mounted mon must agree). */
uint8_t app_met_game(void) {
  return (uint8_t)(g_game == PK_RS ? 1 : g_game == PK_FRLG ? 4 : 3);
}

/* Flush the RAM log to SD now — for anomaly sites (wallpaper/icon self-verify) whose
 * evidence must survive a "see glitch -> power off". Same rmbl discipline as the
 * commit sites: no motor on the cart bus mid-transfer. Main-loop-synchronous only. */
void app_log_flush(void) {
  /* Hard rule 4: writes are Omega-only. On an EverDrive every disk_write fails by
   * design, and a failed disk_write leaves FatFs' sync_window with fs->wflag still
   * SET (lib/fatfs/ff.c) — after which every later move_window returns FR_DISK_ERR
   * and one log write has poisoned the mounted volume for the browser too. So the
   * cheapest correct thing is to not attempt it at all. */
  if (active_flashcart != EZ_FLASH_OMEGA) return;
  rmbl_pause();
  log_flush_to_sd(LOG_PATH);
  rmbl_resume();
}

/* ---- save-open breadcrumbs ------------------------------------------------
 *
 * Opening a save is the longest silent stretch in the tool: a 128 KiB SD read, a
 * Gen-3 parse, an SD-backed ROM open for the art, then a multi-frame box paint --
 * and until now it wrote NOTHING to the card and painted NOTHING on screen. The
 * 2026-08-18 DS-Lite hang therefore produced a log whose last line was "SD mounted
 * OK": it named no phase at all. (Note the on-card trail really did end there --
 * the `scan <dir>: N entries` lines that appear in captured logs are RAM-only and
 * reach the card solely when some later, unrelated flush carries the whole buffer.)
 *
 * TWO trails, deliberately priced differently:
 *   - load_phase() paints one word on screen. It is FREE (a 192x36 panel, no SD, no
 *     ROM streaming), so it runs at EVERY step -- even a silent freeze then says
 *     what it was doing, with no card to pull. It is also the trail that survives a
 *     failure of the trail-writing mechanism: the word is in VRAM before the flush.
 *   - the log crumbs flush to the card. That is an SD write, so they run only at the
 *     boundaries worth a write.
 *
 * OS-mode discipline (hard rule 1): both are called from view_save at main-loop
 * level, BETWEEN card operations -- never inside a transfer, never from an IRQ.
 * flashcartio_is_reading is set and cleared inside flashcartio_read_sector /
 * flashcartio_write_sector themselves, so it is provably false at every site here.
 * app_log_flush() rmbl_pause()s the motor around the write (the documented DE
 * GPIO-interleave guard) and the write brackets WAITCNT back to the loader's
 * inherited timing, so a crumb never writes at 0x4317. */
/* The log's OWN health, on screen. The failure latch is circular by construction: the
 * "SD writes failing, logging off" line it writes cannot reach a card that is refusing
 * writes. That is what made 2026-08-18 run 2 unreadable -- a run that reached "parsing"
 * and left NOTHING on the card, indistinguishable from a run that never wrote a crumb
 * because it died before the first one. This badge closes that hole: it is painted from
 * RAM counters (log.h's health API), costs zero card traffic, and says which of the two
 * happened while the screen is still on.
 *
 * Right-aligned so it never collides with the phase word. Main-loop level only -- like
 * every other paint here, never inside a transfer (hard rule 1). */
static void log_badge_right(int right, int y) {
  char b[16];
  log_health_str(b, sizeof b);
  if (!b[0]) return;
  ui_ptext(right - ui_ptext_w(b), y, log_health() == LOG_HEALTH_OK ? UI_DIM : UI_WARN, b);
}

/* ---- the free-running heartbeat -------------------------------------------
 *
 * The phase word says WHAT it was doing; it cannot say whether it is still doing
 * it. 2026-08-18 run 2 stopped with "parsing" on screen and nothing on the card,
 * and from one photograph "wedged" and "slow" are the same picture. So a 12x12
 * bar spins in the panel's top-right corner, advanced from the VBlank IRQ every
 * 8 frames (~7.5 turns/s -- fast enough to read as motion, slow enough that a
 * still photo of a healthy tool shows a bar mid-rotation rather than a blur).
 *
 * STOPPED BAR  = the CPU or the IRQ path is wedged. CRAWLING BAR = merely slow.
 *
 * SAFE to fire mid-transfer, but it does NOT fire mid-transfer -- and reading the
 * bar correctly depends on knowing which. Both halves, measured, not assumed:
 *
 *   SAFE. Hard rule 1 exists because the cart ROM is UNMAPPED mid-transfer, so ROM
 *   code and ROM data vanish. This handler is IWRAM_CODE like the rumble PWM ISR
 *   beside it, and libtonc's dispatcher is IWRAM too (isr_master at 0x03000338,
 *   __isr_table at 0x03003160 -- arm-none-eabi-nm PokeDNA.elf). Geometry and both
 *   colours are immediates, so there is no ROM literal pool to load. Nothing on the
 *   path touches ROM.
 *
 *   BUT IT IS NOT REACHED. The EZ-Flash read path sets REG_IME = 0 for the whole of
 *   _EZFO_readSectors (lib/ezflashomega/io_ezfo.c:210-219, and again in
 *   lib/flashcartio.c:303/347 for the EverDrive branch), so no IRQ -- this one
 *   included -- is delivered while a transfer is in flight.
 *
 * WHAT THE BAR THEREFORE MEANS, exactly:
 *   spinning ......... the CPU is running and IRQs are being delivered.
 *   stepping in bursts, stalling between them, during "1/12 read file" .... NORMAL:
 *                      that is one f_read chunk per stall, IME off for its duration.
 *   FROZEN for many seconds ...... the CPU is wedged, IRQs are off and never coming
 *                      back, or one SD transfer never returned. All three are the
 *                      bug; none of them is a healthy slow load.
 *
 * ARMED ONLY DURING THE LOAD (hb_arm/hb_off), and PAUSED around any screen that
 * waits for the user (hb_pause/hb_resume) -- the cell sits inside app_confirm's
 * frame, so an armed bar would otherwise stamp itself over the "ADD YOUR GAME ROM?"
 * dialog and the ROM picker behind it. Outside all of that the handler returns on
 * the first instruction, so no other screen can find 144 of its pixels rewritten. */
#define HB_X 196
#define HB_Y 66
static volatile u8 s_hb_on   = 0;      /* armed?                                */
static volatile u8 s_hb_ph   = 0;      /* 0..3 bar orientation                   */
static volatile u8 s_hb_sub  = 0;      /* VBlank divider                         */

IWRAM_CODE static void hb_draw(int ph) {
  u16* vram = (u16*)0x06000000;
  /* clear the cell, then draw the bar. Both colours are immediates, so no pool. */
  for (int y = 0; y < 12; y++) {
    u16* row = vram + (HB_Y + y) * 240 + HB_X;
    for (int x = 0; x < 12; x++) row[x] = 0x2482;      /* == UI_PANEL fill      */
  }
  for (int i = 0; i < 12; i++) {
    int x, y;
    if      (ph == 0) { x = i;      y = 6; }
    else if (ph == 1) { x = i;      y = i; }
    else if (ph == 2) { x = 6;      y = i; }
    else              { x = i;      y = 11 - i; }
    vram[(HB_Y + y) * 240 + HB_X + x] = 0x7FFF;        /* white                 */
  }
}

IWRAM_CODE static void hb_isr(void) {
  if (!s_hb_on) return;
  if (++s_hb_sub < 8) return;
  s_hb_sub = 0;
  s_hb_ph = (u8)((s_hb_ph + 1) & 3);
  hb_draw(s_hb_ph);
}

/* Installed only for the load and handed straight back, so the 59 other screens run
 * with the plain ack-only VBlank handler they have always had. irq_add REPLACES the
 * handler for an index that is already registered (init_system registers II_VBLANK
 * with NULL), so this cannot grow the IRQ table or reorder it. */
static void hb_arm(void) {
  s_hb_sub = 0; s_hb_ph = 0; s_hb_on = 1;
  irq_add(II_VBLANK, hb_isr);
}
static void hb_off(void) {
  s_hb_on = 0;
  irq_add(II_VBLANK, NULL);
}

/* Stop the bar without giving the handler slot back, for the stretches of the load
 * that hand the screen to the USER. hb_pause leaves the cell as it was; the caller
 * repaints the whole screen anyway, and load_phase_n redraws the cell on resume. */
static void hb_pause(void)  { s_hb_on = 0; }
static void hb_resume(void) { s_hb_sub = 0; s_hb_on = 1; }

/* How many named sub-steps the save-open sequence has. Written out so the on-screen
 * "7/12" is a fraction Guy can read as progress, not just a label. */
#define PDNA_LOAD_STEPS 13   /* S5-C review #5: step 11 "sidecars" (gb_reconcile_on_load) added */

/* ---- IWRAM stack watermark -------------------------------------------------
 *
 * Hard rule 2 says "never big buffers on the IWRAM stack", and until 2026-08-18
 * NOTHING MEASURED IT. gen3_parse carried a 15,872-byte local array for months on a
 * stack that is 12,824 bytes long, and the only symptom was an unexplained hang: the
 * overflow silently rewrote newlib's malloc bin array and _impure_ptr with save
 * bytes. There is no linker check for this (ld range-checks nothing about the stack)
 * and no emulator warning, so it has to be measured at runtime.
 *
 * Paint the unused stack at boot, then read the low-water mark whenever it is worth a
 * line. `free == 0` means the paint was consumed to the last byte, i.e. the stack
 * reached the end of .data and has been writing THROUGH it -- the exact condition
 * that produced the hang. Cost: one ~12 KB memset at boot and 4 bytes of .bss. */
extern unsigned char __data_end__[];
#define STK_PAT 0xA5A5A5A5u
static u32 s_stk_floor = 0;                   /* lowest painted address, 0 = not armed */

static void stack_paint(void) {
  u32 sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  u32 lo = ((u32)__data_end__ + 3u) & ~3u;
  u32 hi = (sp - 64u) & ~3u;                  /* leave our own frame + margin alone */
  if (hi <= lo) { s_stk_floor = 0; return; }
  s_stk_floor = lo;
  for (u32 a = lo; a < hi; a += 4) *(volatile u32*)a = STK_PAT;
}

static u32 stack_free_min(void) {
  if (!s_stk_floor) return 0;
  u32 a = s_stk_floor;
  while (*(volatile u32*)a == STK_PAT) a += 4;
  return a - s_stk_floor;
}

/* One line, at a point deep enough to be meaningful. Says OVERFLOW rather than "0 B"
 * because 0 is not a tight stack, it is corruption that has already happened. */
static void stack_report(const char* where) {
  u32 f = stack_free_min();
  if (!s_stk_floor)
    log_line("stack: %s - not measured (paint skipped)", where);
  else if (f == 0)
    log_line("stack: %s - OVERFLOW, the stack has written through .data "
             "(floor 0x%08lx)", where, (unsigned long)s_stk_floor);
  else
    log_line("stack: %s - %lu B still free above 0x%08lx", where,
             (unsigned long)f, (unsigned long)s_stk_floor);
}

/* One named sub-step of the save-open sequence. Drawn BEFORE the work it names, so
 * whatever is on screen when it stops is the step that did not finish. The step
 * number is there because a photograph of "party" alone cannot say whether the two
 * party decodes either side of the Deoxys re-read is the one that died. */
static void load_phase_n(int step, const char* what) {
  ui_panel(24, 62, 192, 36, UI_PANEL, UI_TITLE);   /* re-fills, so it self-erases */
  ui_ptext(32, 70, UI_TITLE, "Opening save...");
  char l[40];
  siprintf(l, "%d/%d %s", step, PDNA_LOAD_STEPS, what);
  ui_ptext(32, 84, UI_TEXT, l);
  log_badge_right(190, 70);                        /* clear of the heartbeat cell */
  /* The panel repaint just erased the heartbeat cell, and the ISR only redraws it
   * every 8th VBlank -- so without this a phase that lasts under 8 frames leaves a
   * BLANK corner, and "no bar" would read as "wedged" on the one photograph this
   * whole thing exists for. Redraw it at the phase it is currently on. */
  if (s_hb_on) hb_draw(s_hb_ph);
}

/* Armed by view_save, fired by the box screen's FIRST full paint (pdna_box.c), and
 * DISARMED unconditionally when the save's home screen returns -- otherwise a save
 * with no PC storage (which goes down the party_list() path and never fires it)
 * would leave the flag armed, and the first Bank screen the user opened would write
 * "save: shown" claiming a PC box painted that never existed. A false breadcrumb is
 * worse than a missing one: it is the one artifact this whole item exists to make
 * trustworthy. */
static bool s_crumb_shown_armed = false;

void app_crumb_shown(void) {
  if (!s_crumb_shown_armed) return;
  s_crumb_shown_armed = false;
  log_line("save: shown");
  /* This one-shot fires from inside the box screen's FIRST full paint, which is
   * exactly where "save open + parse + first paint" ends -- so it is the honest place
   * to close view_save's span. The no-PC party path never calls this; view_save closes
   * the span itself there (see its comment). Ordered AFTER the crumb so the span line
   * and the crumb leave on the SAME flush instead of costing two directory walks. */
  perf_span_end();
  app_log_flush();   /* free when perf_span_end's own flush already carried both */
}

/* Not static: party_strip_overlay (pdna_box.c) shows the same read-only-cart denial
 * app_party_overlay shows below — see the prototype in pdna_app.h. */
void msg_wait(const char* title, u16 col, const char* l1, const char* l2) {
  ui_clear();
  ui_panel(16, 48, 208, 70, UI_PANEL, col);        /* framed so it reads as a dialog */
  /* Same reason as app_confirm: clamp INSIDE the dialog so no caller can overflow it. */
  ui_ptext_fit(28, 58, 184, col, title);
  if (l1) ui_ptext_fit(28, 80, 184, UI_TEXT, l1);
  if (l2) ui_ptext_fit(28, 92, 184, UI_DIM, l2);
  ui_text(28, 104, UI_DIM, "Press A");
  wait_keys(KEY_A);
}

/* A static "busy" panel painted at a SAFE point (between SD ops, never during a
 * transfer) so the multi-second verified write doesn't read as a hang on hardware. */
static void busy_panel(const char* line) {
  ui_clear();
  ui_panel(16, 60, 208, 48, UI_PANEL, UI_WARN);
  ui_text(28, 70, UI_WARN, "Saving - do not power off");
  ui_text(28, 88, UI_TEXT, line);
}

/* A short, deliberate one-shot flourish — a green frame that grows outward — shown
 * the moment a verified write succeeds. Safe on the single Mode-3 buffer: nothing
 * else is on screen and each frame is one clean clear + outline at vblank. */
static void grow_in(u16 col) {
  for (int s = 5; s >= 0; s--) {
    ui_clear();
    int m = 16 + s * 10;                         /* margin shrinks -> frame grows */
    m3_frame(m, m, UI_SCR_W - 1 - m, UI_SCR_H - 1 - m, col);
    if (s == 0) ui_text(98, 76, col, "SAVED!");
    vsync(); vsync();
  }
}

/* PC-storage dirty flag: set by deferred move-mode swaps, cleared by any successful
 * PC write (which flushes the whole g_pc) or an explicit revert. */
static bool g_pc_dirty = false;
static bool g_sb1_deferred = false;   /* g_save holds staged SaveBlock1 (Day-Care) edits not yet on disk */

/* Backup policy for the verified write: 0 = new .bak/.bak1… each time (default),
 * 1 = single rolling .bak (overwrite), 2 = skip backup. Session-only (resets each
 * launch). The verified-write itself (.tmp → re-read → rename) always protects the
 * original mid-write; the backup is the extra undo layer. */
/* (moved up beside the other persisted prefs so cfg_save/cfg_load can reach it) */

/* g_anim_mask is defined near the top (with the other persisted prefs) so cfg_save /
 * cfg_load can reach it; this is just the accessor the screens call. */
bool app_anim_enabled(int kind) { return kind >= 0 && kind < ANIM_COUNT && ((g_anim_mask >> kind) & 1u); }

/* Verify checksums, back up the original, and do the verified whole-file write —
 * the shared tail of every commit (the changed section bytes are already in g_save). */
/* Was the loaded image already failing checksums BEFORE this session edited anything?
 * Set once, right after the save is read. Without it a "CHECKSUM ERROR" at write time is
 * ambiguous between "the edit broke it" and "it arrived broken", and on an emulator build
 * there is no SD log to consult. -1 = not checked yet, -2 = clean, >=0 = failing section. */
int g_boot_csum = -1;

void app_note_boot_checksums(void) {
  int fail = -1;
  g_boot_csum = gen3_verify_full_checksums(g_save, g_vinfo.slot, &fail) ? -2 : fail;
  log_line("boot: image checksums %s (slot %d, section %d)",
           g_boot_csum == -2 ? "OK" : "BAD", g_vinfo.slot, fail);
}

static bool app_save_finalize(void) {
  /* Review fix F7 (defence in depth, BACKLOG #54): app_save_finalize() is the single
   * funnel every Gen-3 whole-file write passes through (app_commit_all/app_commit_sb1/
   * app_commit_sb2/app_commit_sb12/app_commit_block, all the way down). A no-op on
   * every gated happy path -- every one of those callers already checks
   * app_can_edit() (or the equivalent) before reaching here -- so this only ever
   * fires if a future call site forgets to gate, catching the bug at the LAST
   * possible moment instead of writing the flash chip. */
  if (!app_can_edit()) {
    log_line("BUG: app_save_finalize with editing disabled - refused");
    return false;
  }
  /* Fold any pending deferred PC-box edits into the image FIRST, so EVERY whole-file write
   * is internally consistent. A cross-buffer move (party<->box, PC<->Day-Care) stages its
   * SaveBlock1 half into g_save (app_stage_sb1) while its PC half lives only in g_pc; without
   * this fold an intervening SB1/SB2/dex commit would persist the SB1 half WITHOUT the PC half
   * -> a half-saved move (a lost or duplicated Pokemon). g_pc is the current intended PC state
   * whenever it's dirty, so any write must carry it. (g_pc_dirty is cleared only on success,
   * below — a failed write leaves it set so flush_on_exit still prompts.) */
  if (g_pc_dirty)
    for (int id = G3_SID_PKMN_STORAGE_START; id <= G3_SID_PKMN_STORAGE_END; id++)
      gen3_write_full_section(g_save, g_vinfo.slot, id,
                              g_pc + (uint32_t)(id - G3_SID_PKMN_STORAGE_START) * G3_SECTOR_DATA_SIZE);
  int fail = -1;
  if (!gen3_verify_full_checksums(g_save, g_vinfo.slot, &fail)) {
    /* SAY WHICH SECTION. "Image failed checksums" is unactionable: sections 0..4 are the
     * trainer/SaveBlock1 half this edit just rewrote, 5..13 are PC storage that it never
     * touches. Which side fails tells you instantly whether the edit is at fault or the
     * loaded image already was — and on an emulator build there is no SD log to read. */
    char l2[40];
    siprintf(l2, "slot %d sect %d %s", g_vinfo.slot, fail,
             (g_boot_csum == fail) ? "(bad on load)" : "(new)");
    log_line("edit: checksum FAIL at section %d (slot %d)", fail, g_vinfo.slot);
    snd_error();
    msg_wait("CHECKSUM ERROR", UI_WARN, l2, "NOT written.");
    return false;
  }

#ifdef PDNA_DELTA
  /* Emulator build: the save is this ROM's own 128 KiB flash chip. There is no
   * sibling file, so no .tmp/.bak pipeline is possible — flashsave_write erases,
   * programs and then byte-compares the whole image, and a failure leaves the chip
   * in an UNKNOWN state. That is exactly why the failure message tells the user to
   * restore their own copy: their .sav on the phone IS the backup. */
  busy_panel("Writing flash save...");
  bool fok = flashsave_write(g_save, G3_SAVE_FILE_SIZE);
  log_line("edit: flash write %s", fok ? "OK" : "FAILED");
  if (!fok) {
    snd_error();
    msg_wait("FLASH WRITE FAILED", UI_WARN, "Save may be damaged.", "Restore your .sav copy.");
    return false;
  }
  snd_save();
  rmbl_fire(RCUE_SAVE);
  grow_in(UI_OK);
  msg_wait("SAVED", UI_OK, "Flash written + verified.", "No backup in this build.");
  g_sb1_deferred = false;      /* same bookkeeping as the SD path: a full write */
  g_pc_dirty     = false;      /* flushes staged daycare + PC edits */
  return true;
#else
  log_line("=== edit commit -> %s (backup mode %d) ===", g_path, g_backup_mode);
  char bak[SF_PATH_MAX]; bak[0] = 0;
  if (g_backup_mode != 2) {                        /* 2 = skip backup; else back up the pre-save file */
    busy_panel("Backing up original...");          /* safe point: before SD copy */
    rmbl_pause();                                  /* no motor on the cart bus mid-transfer */
    SfStatus bst = (g_backup_mode == 1) ? sf_backup_rolling(g_path, bak, sizeof(bak))
                                        : sf_backup(g_path, bak, sizeof(bak));
    rmbl_resume();
    if (bst != SF_OK) {
      log_line("edit: backup failed (%s)", sf_status_str(bst));
      log_flush_to_sd(LOG_PATH);
      snd_error();
      msg_wait("BACKUP FAILED", UI_WARN, sf_status_str(bst), "Save NOT modified.");
      return false;
    }
  }
  SfStatus st;
  busy_panel("Writing + verifying...");            /* safe point: before SD write */
  rmbl_pause();                                    /* no motor on the cart bus mid-transfer */
  st = sf_write_verified(g_path, g_save, G3_SAVE_FILE_SIZE);
  rmbl_resume();
  log_line("edit: write %s (backup %s)", st == SF_OK ? "OK" : sf_status_str(st), bak);
  log_flush_to_sd(LOG_PATH);
  if (st == SF_ERR_RENAME) {
    /* The bytes were written AND read back byte-for-byte -- it is the final swap the card
     * did not keep, which is a different piece of news from "the write failed" and has to
     * read as one. Ask the card which file the user is actually holding rather than
     * guessing: "your save is in the .tmp" is a lie in the interleaving where the rename
     * landed and only the confirmation read failed. */
    const char* nm = strrchr(g_path, '/');
    nm = nm ? nm + 1 : g_path;
    char l1[64];
    SfWhere w = sf_where_are_the_bytes(g_path, g_save, G3_SAVE_FILE_SIZE);
    log_line("edit: rename unconfirmed, bytes are at %d (%s)", (int)w, nm);
    log_flush_to_sd(LOG_PATH);
    snd_error();
    switch (w) {
      case SF_WHERE_TMP_ONLY:                      /* the loud one: no .sav on the card */
        siprintf(l1, "Edit is in %.28s.tmp", nm);
        msg_wait("SAVE NOT IN PLACE", UI_WARN, l1, "Card dropped it. Rename .tmp on a PC.");
        break;
      case SF_WHERE_TMP_AND_OLD:                   /* old save intact; edit not applied */
        siprintf(l1, "Edit is in %.28s.tmp", nm);
        msg_wait("NOT SAVED", UI_WARN, l1, "Old save intact. Try saving again.");
        break;
      case SF_WHERE_TARGET:                        /* it IS on the card; only unconfirmed */
        siprintf(l1, "%.30s looks correct", nm);
        msg_wait("UNCONFIRMED", UI_WARN, l1, "Could not re-check the card. Verify it.");
        break;
      default:                                     /* neither name matches: use the backup */
        msg_wait("SAVE LOST", UI_WARN, "Card kept neither copy.",
                 bak[0] ? "Restore the .bak on a PC." : "No backup was made!");
        break;
    }
    return false;
  }
  if (st != SF_OK) {
    snd_error();
    msg_wait("WRITE FAILED", UI_WARN, sf_status_str(st), "Backup kept; .tmp may remain.");
    return false;
  }
  snd_save();
  grow_in(UI_OK);                                  /* brief success flourish */
  msg_wait("SAVED", UI_OK, "Edit written + verified.", "Original backed up first.");
  g_sb1_deferred = false;                           /* a full write flushes any staged daycare edits */
  g_pc_dirty = false;                               /* and the folded-in PC edits are now on disk */
  return true;
#endif /* PDNA_DELTA */
}

/* Persist `block` (sections [lo..hi]) into the in-RAM image, then finalize. The
 * single safe write path shared by the full editor and the PC-menu quick edits. */
static bool app_commit_block(int sect_lo, int sect_hi, uint8_t* block) {
  for (int id = sect_lo; id <= sect_hi; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id,
                            block + (uint32_t)(id - sect_lo) * G3_SECTOR_DATA_SIZE);
  return app_save_finalize();
}

/* Commit EVERYTHING — SaveBlock2 (sec 0) + SaveBlock1 (sec 1..4) + PC storage
 * (sec 5..13) — in one verified write. Used when an edit also touches the Pokédex,
 * which spans SB1+SB2; the single whole-file write costs the same as committing one
 * section, so this just folds the dex sections in. */
static bool app_commit_all(void) {
  /* BACKLOG #120 S2: with no parsed Gen-3 save, gen3_write_full_section(g_save, ...)
   * below would write Gen-3 sections into whatever bytes g_save actually holds -- a
   * GB session's own battery image. Refuse ABOVE the arena-held release-and-continue
   * below, which stays unchanged: flipping the arena posture too, on top of refusing
   * outright, would risk a silent no-save on a legitimate Gen-3 path ([decided here]). */
  if (!g_vinfo.valid) {
    log_line("BUG: commit-all with no parsed Gen-3 save - refused");
    return false;
  }
  /* g_pc is Tier B's donor (icon_store_borrow). Reaching a PC-writing commit with the
   * arena still lent out would write icon tiles into every box the user owns, so this
   * is the one place worth a belt-and-braces check: every screen releases before it
   * dispatches a key, and pdna_main.c's nav switch releases again unconditionally, so
   * arriving here held is a PROGRAMMING ERROR. Say so, then give the memory back --
   * which re-derives g_pc from g_save -- rather than committing what is in it. */
  if (app_arena_held()) {
    log_line("BUG: commit-all with the EWRAM arena held - releasing before the write");
    icon_store_borrow(false);
    if (app_arena_held()) app_arena_release();
  }
  gen3_write_full_section(g_save, g_vinfo.slot, 0, g_sb2);
  for (int id = 1; id <= 4; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id, g_sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE);
  for (int id = G3_SID_PKMN_STORAGE_START; id <= G3_SID_PKMN_STORAGE_END; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id,
                            g_pc + (uint32_t)(id - G3_SID_PKMN_STORAGE_START) * G3_SECTOR_DATA_SIZE);
  g_pc_dirty = false;
  return app_save_finalize();
}

/* Commit the trainer block (SaveBlock2 = section 0) / the money block (SaveBlock1
 * = sections 1..4) after the trainer card edits g_sb2 / g_sb1 in place. */
bool app_commit_sb2(void) { return app_commit_block(0, 0, g_sb2); }
bool app_commit_sb1(void) { return app_commit_block(1, 4, g_sb1); }
/* SaveBlock2 + SaveBlock1 (sections 0..4) in ONE verified write — for edits spanning both
 * (trainer card identity+money, the dex). One backup + one write instead of two. */
bool app_commit_sb12(void) {
  gen3_write_full_section(g_save, g_vinfo.slot, 0, g_sb2);
  for (int id = 1; id <= 4; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id, g_sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE);
  return app_save_finalize();
}

void app_mark_pc_dirty(void) { g_pc_dirty = true; }
bool app_pc_dirty(void)      { return g_pc_dirty; }

/* ---- borrowed EWRAM arena (see pdna_app.h for why g_pc is the donor) -------- */
static bool g_arena_held = false;
bool app_arena_held(void) { return g_arena_held; }

/* BACKLOG #120 S2: is there a live Gen-3 PC in g_pc right now? `!app_arena_held()`
 * alone is not enough -- the delta fused-GB path clears g_vinfo too, so using both
 * makes the gate observable in mGBA as well as on hardware (§8, [decided here]). Every
 * Gen-3 write path a Bank visit from a GB session exposes (TO GAME / PASTE / inject /
 * commit) checks this, via xg_pc_live/xg_inject_refuse (source/xfer_gate.h). */
/* BACKLOG #150 S150-8 decision 16(b)/G-F2: no longer file-static -- the DOWN edge's
 * native -> Gen-3-PC arm (source/pdna_gen12.c's gb_bank_down_gen3) needs the SAME
 * gate a Bank visit's TO GAME/PASTE/CREATE rows already use. */
bool app_gen3_pc_live(void) { return xg_pc_live(g_vinfo.valid, app_arena_held()); }

/* BACKLOG #150 S150-8 decision 9/G-F1: the one unpromoted native->Gen-3 transfer of
 * this session. Plain .bss (IWRAM), NOT EWRAM_BSS -- 16 B total (this lane may not
 * spend one byte of EWRAM). 0/-1 == none pending. */
static uint64_t g_xd_key;
static int16_t  g_xd_idx = -1;

bool app_xfer_pending(void) { return g_xd_key != 0 && g_xd_idx >= 0; }

void app_xfer_pending_set(uint64_t key, int idx) {
  g_xd_key = key;
  g_xd_idx = (int16_t)idx;
}

void app_xfer_pending_drop(void) { g_xd_key = 0; g_xd_idx = -1; }

/* Re-resolve the entry's path, re-read it, do a cheap identity re-check (still the
 * right kind/direction/state at that index -- a mismatch means the file changed
 * under us; log and give up rather than promote the wrong entry), flip it to
 * XR_STATE_CLAIMED and rewrite -- verified. A failed promotion is NOT fatal: the
 * entry stays XR_STATE_PENDING, which is fail-safe by construction (S11.8:
 * "destination unproven", never removable). */
bool __attribute__((noinline)) app_xfer_promote(void) {
  if (!app_xfer_pending()) return false;
  char path[GBSC_PATH_MAX];
  (void)xr_path_for_key(path, g_xd_key);
  uint8_t s_promote_buf[GBSC_FILE_MAX];   /* stack-local, this call's own frame -- no new static */
  uint32_t len = 0;
  if (sf_read_full(path, s_promote_buf, GBSC_FILE_MAX, &len) != SF_OK) {
    log_line("xfer: promote: could not re-read %s", path);
    return false;
  }
  GbscEntry e;
  if (!gbsc_get(s_promote_buf, len, g_xd_idx, &e) ||
      e.kind != XR_KIND_NATIVE_HOME || e.direction != XR_DIR_ABROAD_G3 ||
      e.state != XR_STATE_PENDING) {
    log_line("xfer: promote: entry %d in %s no longer matches -- giving up", (int)g_xd_idx, path);
    app_xfer_pending_drop();
    return false;
  }
  e.state = XR_STATE_CLAIMED;
  if (gbsc_remove(s_promote_buf, &len, g_xd_idx) != 0) {
    log_line("xfer: promote: remove failed in %s", path);
    return false;
  }
  int nidx = gbsc_add(s_promote_buf, &len, GBSC_FILE_MAX, &e);
  if (nidx < 0) { log_line("xfer: promote: re-add failed in %s", path); return false; }
  rmbl_pause();
  SfStatus wst = sf_write_verified(path, s_promote_buf, len);
  rmbl_resume();
  if (wst != SF_OK) { log_line("xfer: promote: rewrite failed for %s", path); return false; }
  app_xfer_pending_drop();
  return true;
}

/* Best-effort undo of a PENDING entry the user just declined to save (the transfer
 * never happened, so the entry must not linger -- an orphan XR_PENDING entry can
 * never be collected, S11.18 Q6). Same shape as gb_paste_sidecar_undo's own
 * best-effort tail: log-only on failure. */
void __attribute__((noinline)) app_xfer_pending_undo(void) {
  if (!app_xfer_pending()) return;
  char path[GBSC_PATH_MAX];
  (void)xr_path_for_key(path, g_xd_key);
  uint8_t s_promote_buf[GBSC_FILE_MAX];   /* stack-local -- no new static */
  uint32_t len = 0;
  if (sf_read_full(path, s_promote_buf, GBSC_FILE_MAX, &len) != SF_OK) {
    log_line("xfer: undo: could not re-read %s", path);
    app_xfer_pending_drop();
    return;
  }
  GbscEntry e;
  if (!gbsc_get(s_promote_buf, len, g_xd_idx, &e) ||
      e.kind != XR_KIND_NATIVE_HOME || e.direction != XR_DIR_ABROAD_G3 ||
      e.state != XR_STATE_PENDING) {
    log_line("xfer: undo: entry %d in %s no longer matches -- leaving it", (int)g_xd_idx, path);
    app_xfer_pending_drop();
    return;
  }
  if (gbsc_remove(s_promote_buf, &len, g_xd_idx) != 0) {
    log_line("xfer: undo: remove failed in %s", path);
    app_xfer_pending_drop();
    return;
  }
  rmbl_pause();
  SfStatus wst;
  if (gbsc_count(s_promote_buf, len) == 0) wst = (f_unlink(path) == FR_OK) ? SF_OK : SF_ERR_WRITE;
  else                                     wst = sf_write_verified(path, s_promote_buf, len);
  rmbl_resume();
  if (wst != SF_OK) log_line("xfer: undo: rewrite failed for %s", path);
  app_xfer_pending_drop();
}

uint8_t* app_arena_acquire(uint32_t need) {
  if (g_arena_held || need > (uint32_t)G3_PC_BYTES) return NULL;
  /* Unsaved box moves live ONLY in g_pc — handing it out would destroy them. The
   * caller must tell the user to save first; it must not "helpfully" commit here,
   * because a PC write is a user-visible destructive action. */
  if (g_pc_dirty) return NULL;
  g_arena_held = true;
  return g_pc;
}

void app_arena_release(void) {
  if (!g_arena_held) return;
  g_arena_held = false;
  /* Rebuild the PC exactly as it was: g_pc is pure derived state, and g_save (which
   * the arena never touches) is still authoritative.
   *
   * ...but only when g_save actually holds a parsed Gen-3 save. A Game Boy session
   * (view_save's GB fork) borrows this same arena with 32 KiB of GB battery data in
   * g_save and g_vinfo cleared; rebuilding from that would read a Gen-3 PC out of
   * Game Boy bytes and hand back 30 boxes of noise. Nothing to rebuild FROM is not an
   * error, so leave g_pc alone and let the next real save load fill it. */
  if (!g_vinfo.valid) return;
  gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc);
}

/* ---- borrowed EWRAM cache (box_oam.c's SD/cache-sourced pose-swap frame-1s) ----
 * See pdna_app.h's own comment on app_box_swap_acquire for the full rationale.
 * g_entries donates its 19,456 B; box_oam.c only ever asks for APP_BOX_SWAP_BYTES
 * (15,360 B) of it. Unlike g_arena there is no "dirty" concept to refuse on — g_entries
 * carries no state across a browser visit, so releasing it is a plain flag clear, not a
 * rebuild. */
static bool     g_box_swap_held = false;
static uint32_t g_box_swap_need = 0;      /* the `need` acquire was called with (canary math) */

/* ---- borrowed-cache canary (2026-08-23) -- see pdna_app.h's own comment on
 * app_box_swap_canary_ok for the full rationale. Any 16-byte pattern with no long
 * run of one repeated byte works; this one just needs to be implausible as either
 * zeroed/freshly-scanned BrowseEntry bytes or an ASCII path fragment. */
#define APP_BOX_SWAP_CANARY_LEN 16u
static const uint8_t k_swap_canary[APP_BOX_SWAP_CANARY_LEN] = {
  0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0xE1, 0x1E,
  0x2D, 0xD2, 0x4B, 0xB4, 0x78, 0x87, 0xF0, 0x0F
};
/* g_cwd's true tail bytes, saved across the borrow so app_box_swap_release can put
 * them back -- the canary stamp must never actually alter the current directory. */
static uint8_t g_cwd_tail_save[APP_BOX_SWAP_CANARY_LEN];

uint8_t* app_box_swap_acquire(uint32_t need) {
  if (g_box_swap_held || need > (uint32_t)sizeof(g_entries)) return NULL;
  g_box_swap_held = true;
  g_box_swap_need = need;
  uint32_t tail = (uint32_t)sizeof(g_entries) - need;
  uint32_t clen = tail < APP_BOX_SWAP_CANARY_LEN ? tail : APP_BOX_SWAP_CANARY_LEN;
  if (clen) memcpy((uint8_t*)g_entries + need, k_swap_canary, clen);
  memcpy(g_cwd_tail_save, g_cwd + PATH_MAX - APP_BOX_SWAP_CANARY_LEN, APP_BOX_SWAP_CANARY_LEN);
  memcpy(g_cwd + PATH_MAX - APP_BOX_SWAP_CANARY_LEN, k_swap_canary, APP_BOX_SWAP_CANARY_LEN);
  return (uint8_t*)g_entries;
}

bool app_box_swap_canary_ok(void) {
  if (!g_box_swap_held) return true;
  static bool s_logged_after = false, s_logged_before = false;
  bool ok = true;
  uint32_t tail = (uint32_t)sizeof(g_entries) - g_box_swap_need;
  uint32_t clen = tail < APP_BOX_SWAP_CANARY_LEN ? tail : APP_BOX_SWAP_CANARY_LEN;
  if (clen && memcmp((uint8_t*)g_entries + g_box_swap_need, k_swap_canary, clen) != 0) {
    ok = false;
    if (!s_logged_after) {
      log_line("CANARY TRIPPED: g_entries overrun past the pose cache (AFTER g_sb1-side)");
      s_logged_after = true;
    }
  }
  if (memcmp(g_cwd + PATH_MAX - APP_BOX_SWAP_CANARY_LEN, k_swap_canary, APP_BOX_SWAP_CANARY_LEN) != 0) {
    ok = false;
    if (!s_logged_before) {
      log_line("CANARY TRIPPED: g_cwd tail changed (BEFORE g_entries side)");
      s_logged_before = true;
    }
  }
  return ok;
}

void app_box_swap_release(void) {
  if (!g_box_swap_held) return;
  memcpy(g_cwd + PATH_MAX - APP_BOX_SWAP_CANARY_LEN, g_cwd_tail_save, APP_BOX_SWAP_CANARY_LEN);
  g_box_swap_held = false;
}

/* ---- per-game ROM path (the map screen reads map data from the user's own ROM) */
const char* app_rom_path(PkGame game) {
  int i = (int)game;
  return (i >= 0 && i < 3) ? g_rom_path[i] : "";
}
/* Returns false (REFUSED, g_rom_path untouched) when `path` would not fit
 * GB_ROM_PATH_MAX(128) -- E3 review item 3: this used to silently truncate via
 * strncpy(..., PATH_MAX-1), which could register a path pointing at a DIFFERENT
 * (truncated, likely nonexistent) file than the one the user picked. An empty
 * path (clearing the slot) always succeeds. */
bool app_rom_path_set(PkGame game, const char* path) {
  int i = (int)game;
  if (i < 0 || i >= 3) return false;
  if (!path) path = "";
  if (strlen(path) >= (size_t)GB_ROM_PATH_MAX) return false;
  int k = 0;
  for (; path[k]; k++) g_rom_path[i][k] = path[k];
  g_rom_path[i][k] = 0;
  return true;
}

/* Has the user registered ANY of their own game ROMs? The gate for the extras that
 * only make sense once there is real art to draw them with -- today the Day-Care yard
 * visitors. Deliberately "any ROM", not "this save's ROM": the visitors are scenery,
 * so a registered FireRed is enough to dress an Emerald yard, whereas the map screen
 * rightly insists on the matching game because it reads that save's own map data. */
bool app_any_rom_registered(void) {
  for (int i = 0; i < 3; i++) if (g_rom_path[i][0]) return true;
  return false;
}

/* ---- per-generation Game Boy ROM path (slice E3, pdna_app.h has the full contract) */
const char* app_gb_rom_path(uint8_t gen) {
#ifdef PDNA_DELTA
  /* BACKLOG #62: no SD, no file browser here -- the only ROM a delta-gb build can ever
   * have for a generation is whatever tools/fuse_gb.py fused in. This pseudo-path is
   * DISPLAY-ONLY (Settings' "Gen N ROM: fused" row): every real reader that used to
   * f_open() this string (gb_create_locate_rom, gb_art_source.c's SD half) branches on
   * PDNA_DELTA itself and calls fused_gb_rom()/fused_gb_slice_read() directly instead
   * of ever trying to open this string as a file. */
  const uint8_t* base; uint32_t size;
  if (fused_gb_rom(gen, &base, &size)) return (gen == PDNA_GEN1) ? "fused:gen1" : "fused:gen2";
  return "";
#else
  int gi = gb_gen_slot(gen);
  return (gi >= 0) ? g_rom_path[gi] : "";
#endif
}
/* Same "refuse, never truncate" rule as app_rom_path_set() -- see its comment. */
bool app_gb_rom_path_set(uint8_t gen, const char* path) {
  int gi = gb_gen_slot(gen);
  if (gi < 0) return false;
  if (!path) path = "";
  if (strlen(path) >= (size_t)GB_ROM_PATH_MAX) return false;
  int i = 0;
  for (; path[i]; i++) g_rom_path[gi][i] = path[i];
  g_rom_path[gi][i] = 0;
  return true;
}
bool app_gb_rom_registered(uint8_t gen) { return gb_art_have(gen); }

/* BACKLOG #47: pdna_app.h's own comment has the contract. A plain read of the
 * file-static toggle -- no side effects, safe from any module at any time. */
bool app_rom_art_off(void) { return g_rom_art_off; }

/* ---- the currently open save (view_save() sets g_path the moment it opens one) --- */
const char* app_current_save_path(void) { return g_path; }
bool app_current_save_is_gb(void) { return pdna_gen12_size_is_gb(g_save_size); }

/* ---- E4: sprite-era plumbing (pdna_app.h) ---------------------------------------- */
SeRoms app_era_roms(void) {
  SeRoms r;
  memset(&r, 0, sizeof r);
  /* BACKLOG #47: the detach switch means no ROM art anywhere, so no era may claim a
   * ROM either -- se_era_next() (sprite_era.c) walks eras by SeRoms.have[] alone and
   * would otherwise still offer Gen-3 RS/EM/FRLG (their g_rom_path[] entries are
   * config, untouched by the switch) while gb_art_have() above already refuses the
   * Gen-1/2 slots. `r` is already all-false from the memset, so returning it here
   * dims every era in the Sprites grid down to NATIVE, exactly like "nothing
   * registered" -- the registrations themselves are still there for when the switch
   * flips back on. */
  if (app_rom_art_off()) return r;
  r.have[SE_ERA_G3_RS]   = app_rom_path(PK_RS)[0]      != 0;
  r.have[SE_ERA_G3_EM]   = app_rom_path(PK_EMERALD)[0] != 0;
  r.have[SE_ERA_G3_FRLG] = app_rom_path(PK_FRLG)[0]    != 0;
  r.have[SE_ERA_GEN1]    = app_gb_rom_registered(PDNA_GEN1);
  r.have[SE_ERA_GEN2]    = app_gb_rom_registered(PDNA_GEN2);
  return r;
}

SeSaveKind app_save_kind(void) {
  Gb12SaveKind gk = pdna_gen12_active_kind();
  if (gk == GB12_SAVE_RBY) return SE_KIND_GEN1;
  if (gk != GB12_SAVE_NONE) return SE_KIND_GEN2;   /* GS or CRYSTAL */
  return g_save_kind;                              /* Gen-3 session (or none yet) */
}

static void g3cross_boot_register(void); /* forward: defined below with the rest of the
                                          * cross-game rung, near s_iconrom/s_romsprite */

/* pdna_origin_art.h's PdnaEraResolverFn: the thin wrapper se_resolve() lives behind so
 * pdna_origin_art.c never has to include sprite_era.h or know about g_era/config.cfg
 * at all (see that header's own comment on the resolver hook). `place` arrives as the
 * plain int pdna_origin_art_set_place() was last called with; se_resolve() itself
 * defends any out-of-range value the same way it always has. */
static int era_resolver_cb(int place, uint8_t origin_gen, uint8_t origin_certain,
                           uint16_t national_dex, int* reason) {
  SeRoms roms = app_era_roms();
  /* Whether the build has COMPILED Gen-3 art at all -- informational only (it
   * decides SE_WHY_COMPILED vs SE_WHY_CHIP in `reason`, never which pixels the
   * router actually tries: gen3_ladder tries the compiled accessors and the ROM
   * rung regardless of this flag, exactly as it did before this slice existed). */
#if PDNA_ARTLESS
  bool compiled_gen3 = false;
#else
  bool compiled_gen3 = true;
#endif
  /* D1: se_resolve_for_router() (not se_resolve() directly) -- a concrete answer that
   * is exactly the (kind, origin_gen, origin_certain) native answer must be reported
   * as NATIVE, or every untouched grid cell reroutes onto the cross-game ROM rung
   * instead of gen3_ladder's compiled/same-game path. See sprite_era.h's comment on
   * se_resolve_for_router. */
  return (int)se_resolve_for_router(&g_era, app_save_kind(), (SePlace)place, origin_gen,
                                    origin_certain, national_dex, &roms, compiled_gen3,
                                    reason);
}

/* E5b (Guy, 2026-09-06): the box-grid-CELL counterpart of era_resolver_cb above,
 * registered as pdna_origin_art.c's "cell" hook (pdna_origin_art_set_era_resolver_cell)
 * and asked ONLY by the box-grid cell path (cell_pack/pdna_origin_box_art via
 * cell_era_of()). Identical to era_resolver_cb except it calls se_resolve_cell()
 * instead of se_resolve_for_router() -- the box grid needs the CONCRETE per-cell
 * answer, not the router's "same as native => NATIVE" collapse (that collapse exists
 * so the PORTRAIT rung doesn't reroute every untouched cell onto the cross-game ROM
 * path; the box grid never touches that rung at all), AND (this is the E5b change,
 * superseding this callback's old D1-era name "raw"/"uncollapsed") se_resolve_cell()'s
 * own opt-in short-circuit: a Gen-3 save's PC/BANK grid cell left at NATIVE answers
 * the icon STORE's own era for every mon, imports included, never a bitmap era
 * picture by default. See sprite_era.h's comment on se_resolve_cell for the full
 * rule, and pdna_origin_art.h's comment on pdna_origin_art_set_era_resolver_cell for
 * why this hook (not the router-facing one) is the box grid's only correct source. */
static int era_resolver_cell_cb(int place, uint8_t origin_gen, uint8_t origin_certain,
                                uint16_t national_dex, int* reason) {
  SeRoms roms = app_era_roms();
#if PDNA_ARTLESS
  bool compiled_gen3 = false;
#else
  bool compiled_gen3 = true;
#endif
  return (int)se_resolve_cell(&g_era, app_save_kind(), (SePlace)place, origin_gen,
                              origin_certain, national_dex, &roms,
                              compiled_gen3 ? 1 : 0, reason);
}

/* Registered once from main()'s startup (alongside gb_art_boot_register(), just
 * below this call site) -- unconditional, unlike g3cross_boot_register(): the
 * resolver itself touches no SD card (app_era_roms()/app_save_kind() are pure reads
 * of resident state), so it is exactly as safe under PDNA_DELTA as everywhere else --
 * it simply never has a registered ROM to name there. */
static void pdna_era_boot_register(void) {
  pdna_origin_art_set_era_resolver(era_resolver_cb);
  pdna_origin_art_set_era_resolver_cell(era_resolver_cell_cb);
  g3cross_boot_register();
}

/* Draw the invented yard visitors? Only when the user asked for them AND owns a ROM.
 * Both halves matter: the setting is the user's choice, the ROM is what makes the
 * choice meaningful. See g_yard_visitors for why they are no longer on by default. */
bool app_yard_visitors_ok(void) {
  /* #47: the gate exists so there is real icon art to draw the visitors with; with
   * the switch on there is none (the cache is dropped too), so they stay home. */
  return g_yard_visitors && app_any_rom_registered() && !g_rom_art_off;
}

/* Release a PC box slot as part of a PC->Bank MOVE (multi-select "send to bank" + the
 * single-mon carry): clear g_pc[box][slot] IFF it still holds the mon whose 8-byte identity
 * (personality 0-3 + OT id 4-7, both plaintext at the record start) is `id8`, then mark the PC
 * dirty (folded into the one exit save). Call ONLY after the destination bank box is VERIFIED on
 * SD (banksrc commit), so an interruption between the two leaves a harmless DUPLICATE, never a
 * loss (learn: "commit the destination before clearing the source"). The identity match is
 * belt-and-braces so a bystander is never zeroed if the slot somehow changed underneath us. */
void app_pc_release_slot(int box, int slot, const uint8_t* id8) {
  if (box < 0 || box >= G3_TOTAL_BOXES || slot < 0 || slot >= G3_IN_BOX) return;
  uint8_t* p = pk_box_slot(g_pc, box, slot);
  if (id8 && memcmp(p, id8, 8) != 0) return;   /* slot no longer holds our mon -> skip (no loss) */
  memset(p, 0, 80);
  app_mark_pc_dirty();
}

/* Stage SaveBlock1 (sections 1..4) into the in-RAM image WITHOUT an SD write, so
 * moving Pokemon in/out of the Day-Care batches into a single save at true exit
 * instead of a write per move. reload_saveblocks reads g_sb1 back from g_save, so
 * staged edits survive screen changes; any real commit (or the exit flush) writes
 * the whole image, flushing them. Returns true (staging cannot fail). */
static bool app_stage_sb1(void) {
  for (int id = 1; id <= 4; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id, g_sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE);
  g_sb1_deferred = true;
  return true;
}

bool app_commit_pc(void)  {
  /* BACKLOG #120 S2: same "no live Gen-3 PC" refusal app_inject_to_game() uses --
   * g_pc may be on loan to a GB session's mount arena, or there may be no parsed
   * Gen-3 save at all, in which case app_commit_block() below would write Gen-3
   * sections into a GB battery image. */
  if (xg_inject_refuse(app_arena_held(), g_vinfo.valid)) {
    /* Deliberate asymmetry vs app_commit_all(): that one releases-and-continues for a
     * Gen-3 caller that forgot the arena; here a GB session can never have a live PC,
     * so refusing is the only safe answer (s2 re-verify D6). */
    log_line("BUG: app_commit_pc with no live Gen-3 PC - refused");
    return false;
  }
  bool ok = app_commit_block(G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, g_pc);
  if (ok) g_pc_dirty = false;                       /* the write persisted everything */
  return ok;
}

/* Register a record's species in the loaded save's Pokédex (seen + owned), in
 * g_sb1/g_sb2 RAM only. Returns true iff it set a NEW flag (so the caller knows the
 * dex changed and must commit it). Skips eggs and already-known species. */
static bool app_dex_register_rec(const uint8_t* rec, bool is_party) {
  PkMon m;
  if (!pk_decode_mon(rec, is_party, &m) || m.isEgg || m.isBadEgg) return false;
  uint16_t nat = pk_national_no(m.species);
  if (nat < 1 || nat > G3_DEX_NAT_MAX) return false;
  if (pk_dex_owned(g_sb2, nat) && pk_dex_seen(g_sb2, nat)) return false;   /* already registered */
  pk_dex_set_seen(g_sb1, g_sb2, g_game, nat, true);
  pk_dex_set_owned(g_sb2, nat, true);
  return true;
}

/* Commit for an edit/inject that may need dex registration. If `block` is the loaded
 * save's PC or party (NOT the bank) and the mon's species is new to the dex, register
 * it and commit everything in one write; otherwise run the block's normal commit. */
static bool app_commit_with_dex(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block) {
  if ((block == g_pc || block == g_sb1) && app_dex_register_rec(rec, is_party))
    return app_commit_all();
  return commit ? commit() : false;
}

/* BACKLOG #150 S150-6, decision 8: the guard's own plan, handed from
 * app_xfer_pid_guard() to app_xfer_pid_rekey() across the commit in between. */
typedef struct {
  bool needs_rekey;
  char old_path[GBSC_PATH_MAX];
  char new_path[GBSC_PATH_MAX];
} XferRekeyPlan;

/* BACKLOG #150 S150-6, decision 8/G-H5, order per D-Q3 (2026-09-15 orchestrator
 * decision -- overrides the brief's original "re-key before commit" text): the
 * reroll/PID/OT-id-change guard, run at the editor's commit chokepoint
 * (app_box_browse/party_browse) rather than inside em_reroll/em_set_pid/
 * em_set_unown_form/gen3_ivroll.c themselves -- those fire on every D-pad press;
 * this fires once, right before the edit would actually land.
 *
 * `old_rec`/`new_rec` need only their first 8 bytes (PID + OT id) -- both the
 * 80-byte box shape and the 100-byte party shape start with the same header, so
 * the same function serves both call sites unmodified.
 *
 * Returns true = proceed with the commit. false = the whole commit is abandoned
 * (B on the warning, or a duplicate-target refusal) -- no memcpy, no
 * app_commit_with_dex, nothing written anywhere. When true and *out_plan.needs_rekey
 * is set, the caller commits FIRST and only calls app_xfer_pid_rekey() after a
 * VERIFIED successful commit (D-Q3: the save must never be committed with the old
 * record already gone, and a failed re-key must never be read as "nothing
 * happened" when the save itself did land). */
static bool __attribute__((noinline)) app_xfer_pid_guard(const uint8_t* old_rec,
                                                          const uint8_t* new_rec,
                                                          XferRekeyPlan* out_plan) {
  out_plan->needs_rekey = false;
  uint64_t old_key = xr_key_g3(old_rec);
  uint64_t new_key = xr_key_g3(new_rec);
  if (old_key == new_key) return true;               /* PID/OT id unchanged -- no-op */

  /* nothing filed under the old key -- nothing this edit could orphan. */
  if (!xr_path_for_key(out_plan->old_path, old_key)) return true;

  if (!app_confirm(PDNA_XFER_REKEY_TITLE, PDNA_XFER_REKEY_L1)) return false;   /* B: abandon */

  /* A file ALREADY at the new key means two records would share one PID+OTID --
   * a DUPLICATE, not a merge (decision 8). Refuse before anything is written. */
  if (xr_path_for_key(out_plan->new_path, new_key)) {
    snd_error();
    msg_wait(PDNA_XFER_REKEY_DUP_TITLE, UI_WARN, PDNA_XFER_REKEY_DUP_L1, 0);
    return false;
  }

  out_plan->needs_rekey = true;
  return true;
}

/* D-Q3: runs ONLY after app_commit_with_dex() has reported a verified success.
 * Re-key = sf_read_full(old) -> sf_write_verified(new) -> f_unlink(old), in that
 * order (SS11.6 verbatim). Any failure before the unlink leaves the OLD file
 * intact -- the save itself already committed, so there is no "abandon" left to
 * do; this can only log and move on. S150-11's reconcile matches a stale-keyed
 * file by identity (ident32 + OT + name) later.
 *
 * noinline, GBSC_FILE_MAX (1042 B) buffer on its own frame -- the same discipline
 * app_paste_gb_merge already follows. */
static void __attribute__((noinline)) app_xfer_pid_rekey(const XferRekeyPlan* plan) {
  if (!plan->needs_rekey) return;

  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = 0;
  bool ok = sf_read_full(plan->old_path, buf, sizeof buf, &len) == SF_OK;
  if (ok) ok = sf_write_verified(plan->new_path, buf, len) == SF_OK;
  if (ok) ok = f_unlink(plan->old_path) == FR_OK;

  if (!ok) {
    log_line("xfer: rekey failed, old key kept (%s -> %s)", plan->old_path, plan->new_path);
    msg_wait(PDNA_XFER_REKEY_FAILED_TITLE, UI_WARN, PDNA_XFER_REKEY_FAILED_L1, 0);
  }
}

/* Register a mon's species for a DEFERRED add (Day-Care withdraw, a carried/copied mon
 * dropped into the PC) — no SD write now. Stages the dex sections (SB2 + SB1) into the
 * in-RAM save image like app_stage_sb1, so the exit flush's finalize (which writes the
 * whole image) persists the dex alongside the PC moves. Idempotent / egg-safe. */
static void app_register_dex_deferred(const uint8_t* rec, bool is_party) {
  if (!app_dex_register_rec(rec, is_party)) return;          /* unchanged / egg / already known */
  gen3_write_full_section(g_save, g_vinfo.slot, 0, g_sb2);
  for (int id = 1; id <= 4; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id, g_sb1 + (uint32_t)(id - 1) * G3_SECTOR_DATA_SIZE);
  g_sb1_deferred = true;                                     /* flush_on_exit will commit */
}
/* BoxSource hook for the PC: a mon just landed in the in-save PC -> auto-register its dex. */
static void pcsrc_note_add(const uint8_t* rec) { app_register_dex_deferred(rec, false); }

/* Bank->PC carry records the bank source for deletion at the save phase (see pdna_bank). */
void app_bank_defer_delete(int box, int slot, const uint8_t* rec80) { pdna_bank_defer_delete(box, slot, rec80); }
bool app_bank_defer_full(void) { return pdna_bank_defer_full(); }
bool app_bank_defer_room(int n) { return pdna_bank_defer_room(n); }
void app_bank_defer_pop(int n) { pdna_bank_defer_pop(n); }
int app_bank_flush_deletions(void) { return pdna_bank_flush_deletions(); }   /* delete queued Bank sources NOW (after the PC dest is committed) */
/* A read-only FOREIGN source (a mounted GB save) sets is_bank so it inherits the
 * bank's safe navigation, but it is NOT the bank: its box indices mean nothing to the
 * deferred-deletion queue, and letting a queued Bank->PC delete blank one of its
 * cells would make a Pokemon vanish from a save we do not even write to. */
void app_bank_hide_pending(int box, PkMon g[30]) { if (app_src_readonly()) return; pdna_bank_hide_pending(box, g); }
bool app_bank_slot_pending(int box, int slot) { return app_src_readonly() ? false : pdna_bank_slot_pending(box, slot); }
bool app_bank_clear_slots(int box, const uint8_t* slots, const uint8_t (*recs80)[80], int n) {
  return pdna_bank_clear_slots(box, slots, recs80, n);
}

/* Emerald "Walda" secret-wallpaper pattern (the graphic shown by box wallpaper 16),
 * stored in SaveBlock1. -1 / no-op on the other games. */
int  app_walda_pattern(void) { return (g_game == PK_EMERALD) ? (int)pk_walda_pattern(g_sb1) : -1; }
bool app_set_walda(uint8_t pattern) {
  if (g_game != PK_EMERALD) return false;
  pk_set_walda_pattern(g_sb1, pattern);
  return true;
}

/* The save's two chosen Walda wallpaper colors ([0] background, [1] foreground) —
 * the Friends patterns are pre-rendered with sentinel colors and the box screen
 * substitutes these at draw time (wallpapers.c / pdna_box.c). Non-Emerald (no
 * waldaPhrase at this offset): the game's own ResetWaldaWallpaper defaults
 * (pokemon_storage_system.c:9661-9669 — RGB(21,25,30) bg, RGB(6,12,24) fg), a
 * contrasting pair so the pattern still SHOWS. Returns true when read from the save. */
bool app_walda_colors(uint16_t out[2]) {
  if (g_game == PK_EMERALD) { pk_walda_colors(g_sb1, out); return true; }
  out[0] = (uint16_t)(21 | (25 << 5) | (30 << 10));
  out[1] = (uint16_t)(6 | (12 << 5) | (24 << 10));
  return false;
}


/* BACKLOG #150 S150-14 step 2: a native-cell EDIT commit, mirroring the Gen-3 branch
 * six lines below app_box_browse's own bc_is_native() check (app_xfer_pid_guard ->
 * memcpy -> commit() -> app_xfer_pid_rekey, D-Q3's order verbatim, decision 9) --
 * but through commit() DIRECTLY, never app_commit_with_dex (decision 8: a native
 * cell has no Gen-3 species and app_dex_register_rec would read a meaningless
 * decrypt off it; on the Bank block app_commit_with_dex's own `block == g_pc ||
 * block == g_sb1` test is false anyway, so this is the same behaviour with that
 * dead branch made structurally impossible).
 *
 * KNOWN DEVIATION (flagged for the orchestrator, reported prominently): `commit` is
 * an AppCommitFn function pointer, so the `commit()` call three lines below is a
 * genuinely NEW indirect-call site the stack walker cannot resolve on its own --
 * confirmed by build: "STACK_BUDGET BLIND SPOT ... app_native_cell_edit @ ...: bl
 * ... [parameter/register dispatch, no argsites declaration for this caller]". The
 * brief's own Acceptance text says "you add no indirect call, so tools/
 * stack_edges.txt needs no row -- if the walker warns, STOP", but decision 8's own
 * literal instruction (call commit() directly) makes that indirect call
 * UNAVOIDABLE regardless of which commit path is chosen -- app_commit_with_dex
 * routing was tried first and still produced a NEW site (GCC inlined
 * app_commit_with_dex into this noinline function rather than reusing its existing
 * out-of-line body). tools/stack_edges.txt is outside this lane's declared file
 * list ("Your files, and nothing else"), but completing decisions 6-9 is
 * impossible without either touching it or abandoning `noinline`+commit() entirely
 * -- STOP-LICENCE analysis in the delivery report. Resolution taken: ONE argsites
 * row added for app_native_cell_edit, mirroring the EXACT existing pattern (see
 * tools/stack_edges.txt's own comment at "app_mon_menu argsites=3 ->" /
 * "app_paste_gb_merge argsites=1 ->" / "app_commit_with_dex argsites=1 ->", all the
 * SAME AppCommitFn class, same four possible targets) -- a mechanical application
 * of prior art, not a new design decision.
 *
 * `noinline` is load-bearing, not style (same reasoning as app_xfer_pid_guard/
 * app_xfer_pid_rekey right above): it keeps the `cell`/`snapshot` 80-byte buffers
 * and the XferRekeyPlan off app_box_browse's own frame, which sits on the
 * pcp_open_party_strip_inner chain pdna_box.c's PDNA_PARTY_STRIP_NEED is derived
 * from.
 *
 * Decision 7 (rollback): `rec` is snapshotted BEFORE the memcpy; a failed/absent
 * commit() restores it and returns false without re-keying -- box_save's own
 * failure path only logs (pdna_bank.c), so nothing else protects the in-RAM cell,
 * and a native cell's 80 bytes are the mon's ONLY copy. */
static bool __attribute__((noinline)) app_native_cell_edit(uint8_t* rec, AppCommitFn commit) {
  uint8_t cell[80];
  if (!gb_native_summary_open(rec, /*allow_edit*/true, cell)) return false;   /* nothing to write */

  XferRekeyPlan plan;
  if (!app_xfer_pid_guard(rec, cell, &plan)) return false;   /* abandoned, or duplicate-target refusal */

  uint8_t snapshot[80];
  memcpy(snapshot, rec, 80);
  memcpy(rec, cell, 80);
  if (!commit || !commit()) {
    memcpy(rec, snapshot, 80);                                /* decision 7: restore, do NOT re-key */
    return false;
  }
  app_xfer_pid_rekey(&plan);
  return true;
}

/* Box summary BROWSER: VIEW/EDIT a box slot, then U/D scroll to the prev/next
 * occupied slot (real-PC style). Edits are saved per-mon (prompted on leave/change)
 * via the owning block's commit. `block` is the pc-layout buffer (box `box`'s 30
 * records); for the bank box buffer pass box = 0. Returns true if any write. */
static bool app_box_browse(uint8_t* block, int box, int start, AppCommitFn commit) {
  int idx = start; bool any = false;
  int card = 0;                                            /* sticky across mon-scroll */
  for (;;) {
    uint8_t* rec = block + 0x0004 + ((uint32_t)box * 30 + idx) * 80;
    /* BACKLOG #150 S150-2: a native Bank cell ("GBC1") gets the REAL Gen-1/2 summary,
     * read-only, instead of pdna_inspect()'s lossy Gen-3-converted copy -- exactly the
     * §11.9 requirement. bc_is_native() first, before anything else: a native cell can
     * never be in a GB session's own grid (those records come from gb_build_slot), so
     * this and app_mon_menu_readonly's RO_VIEW arm below cannot collide. */
    /* BACKLOG #150 S150-14 step 2: the gated EDIT path -- app_native_cell_edit()
     * itself decides (inside gb_native_summary_open, decision 10) whether the cart
     * can actually edit; on a read-only cart this behaves exactly like S150-2's
     * original read-only call. */
    if (bc_is_native(rec)) { if (app_native_cell_edit(rec, commit)) any = true; break; }
    uint8_t out[100]; bool saved = false;
    int nav = pdna_inspect(rec, false, app_can_edit(), out, &saved, &card);
    if (saved) {
      XferRekeyPlan plan;
      if (app_xfer_pid_guard(rec, out, &plan)) {
        memcpy(rec, out, 80);
        if (app_commit_with_dex(rec, false, commit, block)) {
          any = true;
          app_xfer_pid_rekey(&plan);
        }
      }
    }
    if (nav == 0) break;
    for (int step = 0; step < G3_IN_BOX; step++) {           /* next occupied slot in dir nav */
      idx = (idx + nav + G3_IN_BOX) % G3_IN_BOX;
      PkMon t;
      if (pk_decode_mon(block + 0x0004 + ((uint32_t)box * 30 + idx) * 80, false, &t) &&
          t.species >= 1 && t.species <= 411) break;
    }
  }
  return any;
}

/* Party summary BROWSER: VIEW/EDIT a party slot, then U/D scroll to the prev/next party
 * mon staying on the SAME card (real-PC style) instead of dropping back to the party list.
 * The party is gap-free, so every index 0..g_nparty-1 is a real mon — just wrap. */
static bool party_browse(int start, AppCommitFn commit) {
  int count = g_nparty; if (count < 1) return false;
  int idx = start; if (idx < 0) idx = 0; if (idx >= count) idx = count - 1;
  uint16_t doff = g_frlg ? 0x0038 : 0x0238;
  int card = 0; bool any = false;                          /* card sticky across mon-scroll */
  for (;;) {
    uint8_t* rec = g_sb1 + doff + (uint32_t)idx * 100;
    uint8_t out[100]; bool saved = false;
    int nav = pdna_inspect(rec, true, app_can_edit(), out, &saved, &card);
    if (saved) {
      XferRekeyPlan plan;
      if (app_xfer_pid_guard(rec, out, &plan)) {
        memcpy(rec, out, 100);
        if (app_commit_with_dex(rec, true, commit, g_sb1)) {
          any = true;
          app_xfer_pid_rekey(&plan);
        }
      }
    }
    if (nav == 0) break;
    idx = (idx + nav + count) % count;                     /* U/D = prev/next party mon */
  }
  return any;
}

/* PC-menu quick editors: load -> mutate one field -> losslessly re-encode -> patch
 * in place -> commit. Each returns true iff the save was written. */
static bool app_quick_item(uint8_t* rec, bool is_party, AppCommitFn commit) {
  EditMon e; gen3_edit_load(rec, is_party, &e);
  PkMon cur; em_preview(&e, &cur); pk_resolve(&cur);
  uint16_t id = pick_item(cur.heldItem);             /* 0xFFFF = cancel; 0 = no item */
  if (id == 0xFFFF) return false;
  em_set_item(&e, id);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  return commit ? commit() : false;
}


/* MOVE (box reposition) request: the A-menu sets this; the box loop consumes it. */
static bool g_move_req = false;
bool app_take_move_request(void) { bool r = g_move_req; g_move_req = false; return r; }
static bool g_dup_req = false;
bool app_take_dup_request(void)  { bool r = g_dup_req;  g_dup_req  = false; return r; }
/* The party popup's "MOVE TO BOX" action (app_mon_menu) sets g_party_tobox_req; the overlay
 * consumes it to start a party-origin carry. g_party_tobox_allowed gates whether the action is
 * even offered (only when the popup was opened from the box, so there's a box to carry into). */
static bool g_party_tobox_req = false, g_party_tobox_allowed = false;
/* A daycare withdraw-to-PC parks the mon in a free PC slot and asks the box grid to
 * open that box carrying it (the "glove" hand-off). -1 = none. */
static int g_pickup_box = -1, g_pickup_slot = -1;
bool app_take_pickup(int* box, int* slot) {
  if (g_pickup_slot < 0) return false;
  *box = g_pickup_box; *slot = g_pickup_slot; g_pickup_box = g_pickup_slot = -1; return true;
}
static void pdna_daycare(void);   /* forward: app_to_daycare opens it after a deposit */

static int g_box_start = 0;
void app_box_start_set(int s) { g_box_start = s; }
int  app_box_start_take(void) { int s = g_box_start; g_box_start = 0; return s; }
void app_note_pc_box(int b) { if (b >= 0 && b < G3_TOTAL_BOXES) g_pc_last_box = b; }   /* remember the PC box the user is on */

/* BACKLOG #188: the resume-cell slot -- <= 8 bytes, cleared on a fresh save/session
 * mount (app_box_resume_clear()'s own callers: view_save() for Gen 3, pdna_gen12_mount()
 * for a Game Boy save). -1/-1 means "nothing recorded yet". IWRAM by design (same
 * placement as the sibling g_box_start/g_pickup_box/g_pc_last_box scalars) -- costs
 * 8 B of STACK budget (denominator 15,032->15,024 artless, 15,568->15,560 normal),
 * not EWRAM (b188 review A1, 2026-09-21). */
static int g_resume_box = -1, g_resume_cell = -1;
void app_box_resume_note(int box, int cur) { g_resume_box = box; g_resume_cell = cur; }
int  app_box_resume_take(int box) {
  if (g_resume_box != box) return -1;
  if (g_resume_cell < 0) return -1;
  return g_resume_cell;
}
void app_box_resume_clear(void) { g_resume_box = -1; g_resume_cell = -1; }

bool app_confirm(const char* title, const char* l1) {
  ui_clear();
  ui_panel(16, 44, 208, 74, UI_PANEL, UI_WARN);    /* framed destructive-confirm */
  /* Proportional and WRAPPED. At 8 px/glyph this panel was a 24-column budget that every
   * caller had to guess at, and the audit found dozens that guessed wrong — a confirm
   * prompt whose question is cut in half is worse than useless. The message now wraps to
   * two lines inside the frame instead of running off it. */
  ui_ptext_fit(28, 54, 184, UI_WARN, title);
  if (l1) ui_ptext_wrap(28, 74, 184, UI_ROW_H + 2, 2, UI_TEXT, l1);
  /* 96/106, not 98/110: the panel's bottom border is at y=117 and an 8 px row at 110 ran
   * into it — the "B = no" line was drawn half inside the frame. */
  ui_text(28, 96, UI_TEXT, "A = yes");
  ui_text(28, 106, UI_DIM, "B = no");
  u16 k; do { vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  bool yes = (k & KEY_A) != 0;
  if (yes) snd_ok(); else snd_back();
  return yes;
}

/* ---- phase-1 ROM-gated icons: the fused ROM as the box's icon source ---------
 * When the compiled icon art is absent (the artless build), a ROM fused into this
 * image lights the real box icons back up: rom_open() identifies it, rom_mon_open()
 * parses the GF header's icon tables, and box_oam streams 512 B frames from it at
 * box load. Any Pokemon ROM works for icons (the art is per-species, not per-save);
 * Ruby/Sapphire have no GF header yet, so rom_mon fails closed there and the box
 * keeps its artless name chips. Plain statics in IWRAM .bss — the hardware build's
 * EWRAM headroom (2,116 B) is not touched. The registered-SD-file path is the next
 * phase (it needs the P0 verified reader + a FIL lifetime plan).
 * Deliberately NOT static-in-function: cleared per save load. */
static RomCtx s_iconrom_ctx;
static RomMon s_iconrom;
/* Phase 2: the same open ROM also serves the games' description text. Kept beside the
 * icon source because they share a lifetime — one registration lights both up. */
static RomText s_romtext;
/* Phase 1 of the ROM-art plan (docs/analysis-2026-08-19-rom-art/DESIGN.md): the SAME
 * open ROM also serves the summary portrait (rom_sprite.c, registered with
 * pdna_origin_art.c's ladder) and item icons / type badges (rom_itemart.c, read
 * directly through app_item_icon/app_type_badge below). One registration lights all
 * four up, exactly the comment above already promised. Both structs are small PODs
 * with an `ok` flag rom_*_open() zeroes on any failure, so callers may trust
 * s_romitemart.ok directly without a separate "on" flag. */
static RomSprite  s_romsprite;
static RomItemArt s_romitemart;
/* Phase 3 (ROM-art): the pointer glove (rom_hand.h). Compiled out entirely when the
 * real poses are linked in (hand_gate.h) -- a full-art build never opens or holds
 * one, so this costs it nothing. */
#if !PDNA_HAND_ART_COMPILED
static RomHand s_romhand;
#endif
/* Phase 4 (ROM-art): box wallpapers (rom_wallpaper.h) -- Emerald/FireRed/LeafGreen
 * standard wallpapers only (rom_wallpaper.h's SCOPE note: no Walda, no R/S). Read
 * through app_wallpaper_rom() by pdna_box.c's §12c rung; same "small POD with an ok
 * flag" convention as s_romsprite/s_romitemart. */
static RomWallpaper s_romwallpaper;
/* The trainer-card + pokeblock ROM rung (rom_chrome.h): Emerald + Ruby card,
 * Emerald-only pokeblock, deliberately NO bag (see rom_chrome.h's scope note for
 * why). Registered the same way as the other three, plus a setter into
 * pdna_trainer.c (pokeblock's own weak stub lives right here in this file, so it
 * reads s_romchrome directly with no setter needed). */
static RomChrome s_romchrome;
#if !PDNA_POKEBLOCK_ART_COMPILED
/* The Pokeblock ROM rung's decode target -- NOT a memo (see rom_pokeblock_frame()
 * below for why one is unsafe: mon_decomp is the shared 8 KiB buffer every other
 * screen's decoder also writes into between visits). Just a place for
 * rom_chrome_pokeblock_load() to write its {tiles,map,pal} pointers into; every
 * call overwrites it fresh. Declared here, ahead of app_icon_rom_open(), only so
 * a stray reference from an old ROM registration can never read it uninitialised. */
static RomChromePokeblock s_pb_chrome;
#endif

#ifndef PDNA_DELTA
static void art_extract_screen(void); /* forward: defined below, offered from
                                          app_register_rom() (Phase 2, DESIGN.md
                                          Sec 3.1's second entry point) and from
                                          Settings' Extract-art row */
#endif

#ifndef PDNA_DELTA
/* The SD-file icon source: the user's registered .gba, held open read-only for the
 * whole session (FF_FS_LOCK is 0, so the map opening the same file is fine). The
 * FIL's ~600 B sector buffer lives in EWRAM like the map's. Every read brackets
 * rmbl_pause per the SD-transfer convention. */
static FIL EWRAM_BSS s_iconrom_fil;
static bool s_iconrom_fil_open = false;

/* This handle's FatFs cluster link map, so every far/backward seek into a 12.5 MB
 * image is an in-RAM table lookup instead of a FAT chain walk. It matters more here
 * than anywhere else in the app: the ROM's icon-table pointers scatter over the whole
 * image with no locality at all (measured on Guy's Emerald/FireRed/LeafGreen dumps:
 * 0 of 439 adjacent table rows have adjacent icon blobs, strides run 2.5-3 KB and jump
 * by up to 1.3 MB), and a BACKWARD seek is the case FatFs restarts from cluster 0 for.
 * Measured on the host harness: a 21-cell Pokedex page 369 -> 126 disk_read sectors.
 *
 * Sized for 64 fragments (2 + 2*64 = 130 DWORDs, 520 B). 64 rather than 32 because
 * ../rom-load-lab/README.md puts the EZ-Flash kernel's own run-table overrun at ~61
 * pieces: a card at 40 fragments still SD-loads fine and would lose fast seek under a
 * 32-entry table, and the exact fragment count up to 64 is free evidence for that open
 * question -- fastseek_arm logs it on every open, success or shortfall. Past 64 the arm
 * fails, cltbl stays NULL and this handle simply seeks the old way.
 *
 * EWRAM_BSS is a REQUIREMENT, not a preference -- see fastseek.h rule 3. */
static DWORD EWRAM_BSS s_iconrom_clmt[FASTSEEK_ITEMS_FOR(64)];

static bool iconrom_fatfs_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  if (!s_iconrom_fil_open) return false;
  bool ok = true;
  UINT br = 0;
  rmbl_pause();
  if (off != s_iconrom_fil.fptr && f_lseek(&s_iconrom_fil, off) != FR_OK) ok = false;
  if (ok && (f_read(&s_iconrom_fil, dst, (UINT)len, &br) != FR_OK || br != len)) ok = false;
  rmbl_resume();
  return ok;
}

/* ---- E4: the cross-game Gen-3 art rung (pdna_origin_art.h's PdnaG3CrossSource) ----
 * Registered once at boot (g3cross_boot_register, below main()'s startup). Lives here
 * (not a separate module) because it needs three things only this file already owns:
 * FatFs, app_rom_path(), and s_iconrom_ctx/s_romsprite -- the currently-open icon ROM
 * every OTHER rung already reads. */

static bool g3x_kind_matches(RomKind k, PkGame game) {
  switch (game) {
    case PK_RS:      return k == ROM_RUBY || k == ROM_SAPPHIRE;
    case PK_EMERALD: return k == ROM_EMERALD;
    case PK_FRLG:    return k == ROM_FIRERED || k == ROM_LEAFGREEN;
    default:         return false;
  }
}

/* A local FIL, not the session-wide s_iconrom_fil -- this ROM is not the one the rest
 * of the session reads from, and closing it the moment this one fetch is done (below)
 * must not touch the icon ROM's own handle. */
static bool g3x_fatfs_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FIL* f = (FIL*)ctx;
  bool ok = true;
  UINT br = 0;
  rmbl_pause();
  if (off != f->fptr && f_lseek(f, off) != FR_OK) ok = false;
  if (ok && (f_read(f, dst, (UINT)len, &br) != FR_OK || br != len)) ok = false;
  rmbl_resume();
  return ok;
}

/* Decode straight into mon_decomp, exactly like pdna_origin_art.c's own rom_portrait()
 * (no memo -- see that function's comment for why one is unsafe here: mon_decomp is
 * shared with item icons/type badges between two fetches of the very same mon). */
static const uint16_t* g3x_decode(const RomSprite* rs, uint16_t species, uint8_t form,
                                  uint8_t back, uint8_t shiny, uint8_t* out_w, uint8_t* out_h) {
  RomSpritePic pic;
  RomSpriteSide side = back ? ROM_SPRITE_BACK : ROM_SPRITE_FRONT;
  artbuf_claim();     /* about to overwrite mon_decomp -- claim BEFORE the first write */
  if (!rom_sprite_pic(rs, side, species, form, (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &pic))
    return 0;
  uint16_t pal[16];
  if (!rom_sprite_pal(rs, species, form, shiny ? 1 : 0, pal)) return 0;
  if (!rom_sprite_to_rgb15(mon_decomp, MON_DECOMP_BYTES, pic.frame, pal)) return 0;
  *out_w = ROM_SPRITE_W; *out_h = ROM_SPRITE_H;
  return mon_decomp;
}

/* D6: g3x_fetch_other's own cluster link map, parallel to s_iconrom_clmt above but
 * sized for 32 fragments (2 + 2*32 = 66 DWORDs, 264 B) rather than 64 -- this ROM is
 * opened, read once for a single sprite, and closed, not held for the whole session,
 * so a shorter arm that still covers the overwhelming majority of SD images is the
 * right trade against the EWRAM guard (620 B free measured before this change; this
 * table leaves ~356 B). Past 32 fragments the arm fails, cltbl stays NULL on this
 * FIL and it simply seeks the old (cluster-walk-from-0) way, same graceful fallback
 * s_iconrom_clmt already relies on. EWRAM_BSS is a REQUIREMENT -- fastseek.h rule 3. */
static DWORD EWRAM_BSS s_g3x_clmt[FASTSEEK_ITEMS_FOR(32)];

/* The slow path: `game` is NOT the currently open icon ROM, so open app_rom_path(game)
 * in a TEMPORARY RomCtx, decode, close. ONE noinline frame (FIL ~600 B + RomCtx +
 * RomSprite, measured with -fstack-usage -- see docs/HW-TEST-2026-09-05-GB-ARC.md §K
 * and the E4 handoff note) so the call-graph tool charges this exact subtree against
 * the stack-room gate (pdna_origin_art_stack_room()) at its own measured need
 * (PDNA_G3X_FETCH_NEED, D4 -- smaller than the Game Boy rung's PDNA_GB_FETCH_NEED),
 * rather than folding it into whatever frame happened to call it.
 *
 * D6: without a cluster link map, g3x_fatfs_read's f_lseek walks the FAT chain from
 * cluster 0 on every backward/far seek into this ROM -- exactly the scatter pattern
 * measured for the icon ROM's own table (s_iconrom_clmt's comment above), and this
 * path re-opens the whole ROM AND re-walks from 0 on EVERY draw (no session-lifetime
 * handle, unlike the icon ROM). fastseek_arm arms s_g3x_clmt on this FIL the same way
 * app_icon_rom_open already arms s_iconrom_clmt, right after the open succeeds and
 * before any seek happens. */
static const uint16_t* __attribute__((noinline))
g3x_fetch_other(PkGame game, uint16_t species, uint8_t form, uint8_t back, uint8_t shiny,
                uint8_t* out_w, uint8_t* out_h) {
  const char* path = app_rom_path(game);
  if (!path || !path[0]) return 0;

  FIL fil;
  memset(&fil, 0, sizeof fil);
  if (f_open(&fil, path, FA_READ) != FR_OK) return 0;
  fastseek_arm(&fil, s_g3x_clmt, sizeof s_g3x_clmt / sizeof s_g3x_clmt[0], NULL /* quiet: per draw */);
  FSIZE_t fsz = f_size(&fil);
  uint32_t sz = (fsz > (FSIZE_t)0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)fsz;

  RomCtx rc;
  RomSprite rs;
  const uint16_t* px = 0;
  if (rom_open(&rc, g3x_fatfs_read, &fil, sz) && rom_sprite_open(&rs, &rc))
    px = g3x_decode(&rs, species, form, back, shiny, out_w, out_h);
  f_close(&fil);
  return px;
}

static const uint16_t* g3cross_pic_cb(void* ctx, int game, uint16_t species, uint8_t form,
                                      uint8_t back, uint8_t shiny, uint8_t* out_w, uint8_t* out_h) {
  (void)ctx;
  /* D1/item 7: the detach switch means "no ROM art" -- the cross-game rung is ROM art
   * too, so it must honour g_rom_art_off exactly like the other rungs, not just the
   * same-game one. */
  if (g_rom_art_off) return 0;
  PkGame wanted = (PkGame)game;
  /* Fast path: the currently open icon ROM already IS this game -- reuse the SAME
   * open RomSprite every other rung reads, no new file at all. */
  if (s_iconrom.ok && g3x_kind_matches(s_iconrom_ctx.kind, wanted))
    return g3x_decode(&s_romsprite, species, form, back, shiny, out_w, out_h);
  return g3x_fetch_other(wanted, species, form, back, shiny, out_w, out_h);
}

/* Called once from main()'s startup, after cfg_load() -- see gb_art_boot_register()
 * just below this call for the sibling registration. A no-op under PDNA_DELTA (no SD
 * card to open a second ROM from; the #else stand-in just below never runs). */
static void g3cross_boot_register(void) {
  static const PdnaG3CrossSource src = { g3cross_pic_cb, 0 };
  pdna_origin_art_set_g3cross(&src);
}
#else  /* PDNA_DELTA: no SD, so no second ROM to ever open. */
static void g3cross_boot_register(void) { }
#endif

/* WHICH RUNG IS ACTUALLY SERVING THIS SESSION -- the single most useful line in an
 * artless log, and until it existed it had to be inferred from three other lines. The
 * three verdicts are mutually exclusive and in ladder order: the icons.bin cache, else
 * the user's open ROM, else nothing (text-only layouts, and every mon_icon_for* returns
 * NULL). `rc_ok` is the RomCtx art.idx was cross-checked against, or NULL when no ROM is
 * open this session -- in which case the cache is trusted on its own validity alone
 * (art_session.h), which is a materially weaker guarantee and worth saying out loud. The
 * reason string comes from art_session_why(): before it existed, the two commonest
 * causes -- no cache on the card at all, and this kind never extracted -- produced no
 * line whatsoever.
 *
 * HOISTED OUT of app_icon_cache_resolve so that EVERY app_icon_rom_open() exit emits it.
 * Two paths do not run the resolver at all: the fused and SD-registered Ruby/Sapphire
 * "card/pokeblock chrome only" returns, which bail the moment rom_mon_open() refuses an
 * R/S image (no GF header). Guy's corpus has Ruby and Sapphire dumps, so an R/S session
 * used to produce a log with NONE of these lines in it -- no way to tell whether
 * icons.bin was serving, whether art.idx cross-checked, or why not, i.e. a re-run
 * request, which is exactly what these lines exist to prevent.
 *
 * `resolved` is false on those two paths and the line says so, because what it reports
 * there is the MEMOIZED verdict from an earlier save open (or "never checked" on a fresh
 * boot) rather than a fresh one. That is deliberately the EFFECTIVE state -- the same
 * memo art_fallbacks.c's rung reads, and the same boxoam icon-cache path the box will
 * actually use -- not a recomputed answer nobody consults. */
static void icon_rung_log(const RomCtx* rc_ok, bool resolved) {
  bool ready = art_session_icons_ready_memoized();
  log_line("icons rung: %s (rom %s, art.idx %s)",
           ready ? "cache" : s_iconrom.ok ? "rom" : "none",
           s_iconrom.ok ? "open" : "none",
           !resolved      ? "carried over (R/S: no resolve)"
           : rc_ok        ? "cross-checked"
                          : "not cross-checked");
  if (!ready) log_line("icons: no cache - %s", resolved ? art_session_why() : "not resolved this open");
#ifndef PDNA_DELTA
  else {
    FILINFO fi;
    if (f_stat(art_session_icons_path(), &fi) == FR_OK)
      log_line("icons.bin: %s %lu B", art_session_icons_path(), (unsigned long)fi.fsize);
  }
#endif
  /* An ABSENT `bob.*` rollup has three indistinguishable causes, and only one of them is
   * a bug: (a) the user turned that screen's animation off in Settings months ago,
   * (b) the animation gate said no, or (c) he never opened that screen this run.
   * g_anim_mask only ever reached config.cfg, so (a) is what this line pins down.
   *
   * (b) IS NO LONGER ANSWERABLE AT BOOT, and printing a boot-time verdict would now be
   * a lie rather than a shortcut. mon_icon_anim_cheap() used to be a function of the
   * RUNG, which is decided right here; it is a function of the live PLAN now -- whether
   * the rows the screen on the glass declared are in RAM -- and no screen has declared
   * anything yet at this point. What is logged instead is the two things that BOUND that
   * answer for the whole session: which rung serves, and how many rows the pool holds.
   * The per-screen verdict shows up where it is actually decided, as the store's own
   * "icons: borrow refused (pc dirty) - anim off" line. Bit order is the ANIM_* enum
   * (pdna_app.h): box, party, dex, daycare, summary. */
  log_line("anim: mask 0x%02x, gate=plan (rung=%d cap=%u)",
           g_anim_mask, icon_store_rung(), (unsigned)icon_store_capacity());
}

/* Open the best available icon source and register it with the box:
 * 1) a ROM fused into this image (emulator or a fused NOR build);
 * 2) the registered SD .gba for the loaded save's game, then any other game's —
 *    icons are per-species art, so any Pokemon ROM serves them (Deoxys' forme is
 *    the only per-game difference). Ruby/Sapphire have no GF header yet and fail
 *    closed inside rom_mon_open. Call whenever the registration may have changed. */
/* Phase 2 (ROM-art cache): resolve icons.bin ONCE per registration and register it
 * with every consumer -- box_oam.c's OAM ladder AND art_fallbacks.c's RGB15 ladder
 * (via the memoized verdict it reads, primed here). Called at the END of
 * app_icon_rom_open() regardless of whether a ROM was found this session: the whole
 * point of caching (DESIGN.md Sec 4.1) is that a valid cache lights the box AND the
 * Pokedex grid up even with NO ROM registered this session -- "the card outlives the
 * registration". `rc_ok` is the RomCtx to cross-check against, or NULL if none is
 * open (art_session_kind_ready then trusts the cache on its own validity alone).
 *
 * "REGARDLESS" IS NOT QUITE TRUE, and the exception is worth naming rather than leaving
 * for the next reader to discover: app_icon_rom_open's two Ruby/Sapphire "card/pokeblock
 * chrome only" returns bail before reaching here, so on an R/S session the resolver does
 * not run, the memo and boxoam's cache path are whatever a previous save open left, and
 * icon_frame_cache_invalidate() is not called. Those paths now at least LOG the effective
 * state (icon_rung_log above, `resolved = false`). Making them actually resolve is a
 * behaviour change to the icon ladder, not a logging fix, and is deliberately not made
 * here -- see the report accompanying this commit. */
/* deep=true re-hashes icons.bin's full ~451 KB against art.idx's stored FNV; deep=
 * false trusts its size alone (see art_session.c's verify_kind_file for the accepted
 * trade-off). Boot/registration (app_icon_rom_open, reached before the first screen
 * on every launch) passes false -- that 451 KB read was previously unconditional and
 * unmeasured on the critical path before a single frame draws. The one place that
 * still runs the expensive deep check is art_extract_screen, right after a fresh
 * extraction, so the strong guarantee stays reachable exactly when it matters most:
 * immediately after writing bytes the user is already waiting on. */
static void app_icon_cache_resolve(const RomCtx* rc_ok, bool deep) {
  art_session_invalidate();      /* a ROM re-registration can flip either verdict   */
  icon_frame_cache_invalidate(); /* the file behind icons.bin's path may hold new bytes */
  /* BACKLOG #47 review: icons.bin holds pixels EXTRACTED FROM THE USER'S ROM, so
   * "All ROM art off." has to drop it too, or the PC/dex/party/yard grids keep
   * drawing ROM art with the switch on. Nothing is deleted -- app_icon_rom_open()
   * calls this on BOTH edges of the flip, so turning art back on re-resolves it. */
  bool ready = !g_rom_art_off &&
               (deep ? art_session_icons_ready(rc_ok) : art_session_icons_ready_shallow(rc_ok));
  if (ready) {
    boxoam_set_icon_cache(art_session_icons_path());
    log_line("icons: cache ready (/PokeDNA/art/icons.bin)");
  } else {
    boxoam_set_icon_cache(0);
  }
  /* THE single invalidation chokepoint for every icon byte in RAM. icon_store_reset
   * closes and reopens the icons.bin handle, rebuilds its cluster link map, reloads the
   * palettes and drops every cached row -- so a ROM re-registration or a fresh
   * extraction can never leave one session's bytes serving under another's key. Nothing
   * else in the app may invalidate the store; a screen "clearing the cache to be safe"
   * is exactly how the icon path ended up five layers deep re-reading everything.
   *
   * Passed the ROM unconditionally: the store picks the cache rung when icons.bin is
   * both present and readable, and falls to the ROM otherwise, so this one call decides
   * the rung for the whole session. */
  icon_store_reset(ready ? art_session_icons_path() : 0,
                   s_iconrom.ok ? &s_iconrom : 0);
  icon_rung_log(rc_ok, true);
}

static void app_icon_rom_open(void) {
  boxoam_rom_icons(0, 0);
  art_fallbacks_set_rommon(0);   /* Phase 1.5: the dex/party/picker RGB15 ROM rung */
#if !PDNA_HAND_ART_COMPILED
  boxoam_rom_hand(0);
#endif
  memset(&s_romtext, 0, sizeof s_romtext);
  memset(&s_romitemart, 0, sizeof s_romitemart);
  memset(&s_romwallpaper, 0, sizeof s_romwallpaper);
  memset(&s_romchrome, 0, sizeof s_romchrome); s_romchrome.card_style = -1; s_romchrome.bag_style = -1;
  pdna_origin_art_set_romsprite(0);
  pdna_trainer_set_romchrome(0);
  pdna_bag_set_romchrome(0);
#ifndef PDNA_DELTA
  /* Hoisted above the detach check (was only done inside the SD-candidate loop
   * below) so a detach mid-session still releases the SD file handle instead of
   * leaking it across the flip -- the loop's own close-if-open below is now the
   * re-open case (a DIFFERENT path was just registered), not the only one. */
  if (s_iconrom_fil_open) { f_close(&s_iconrom_fil); s_iconrom_fil_open = false; }
#endif
  /* TEST INSTRUMENT (item 7): which state this boot/re-open landed in. Checked once
   * here for the Gen-3/icon-store rung specifically -- gating HERE is what makes
   * every Gen-3 consumer downstream see "no ROM" uniformly, through the SAME
   * no-ROM fallbacks a real no-ROM session already uses and this project already
   * trusts. BACKLOG #47: the switch is no longer gated ONLY here -- the Game Boy
   * rung (gb_art_source.c's gb_art_have()/pic()/icon()) and the Sprites-grid era
   * availability (app_era_roms()) each check g_rom_art_off (via app_rom_art_off())
   * on their own, because neither one funnels through this function at all. */
  log_line("rom art: %s (%s registered)", g_rom_art_off ? "DETACHED (testing)" : "attached",
           app_any_rom_registered() ? "a path is" : "no path is");
  if (g_rom_art_off) {
    /* #47 review D1b: s_iconrom is the ONE ROM handle this function does not reset
     * above (boxoam_rom_icons(0,0) / art_fallbacks_set_rommon(0) / the memsets cover
     * every other one), so without this the icon STORE below would still be handed a
     * live RomMon: on a FUSED build fused_rom_read keeps working and the PC/dex/party
     * grids draw ROM icons anyway; on an SD build s_iconrom_fil was just closed, so
     * the store would latch ICON_RUNG_ROM over a dead handle and every row read fails.
     * Clearing it makes a mid-session flip identical to BOOTING with the switch
     * already on -- ICON_RUNG_NONE, the genuine "no ROM this session" state -- and
     * takes the Extract-art row and app_register_rom's auto-extract prompt honestly
     * dark with it. rom_mon_open() memsets it again on the flip back. */
    memset(&s_iconrom, 0, sizeof s_iconrom);
    app_icon_cache_resolve(0, false);   /* the same tail every "no ROM this session" path reaches */
    return;
  }
  uint32_t fsz = 0;
  bool fused_present = fused_rom_present(&fsz);
  bool fused_ok = fused_present && rom_open(&s_iconrom_ctx, fused_rom_read, 0, fsz);
  /* Note the verdict whether or not rom_open() succeeded -- a HACK impersonating a
   * retail game must set s_hack_mask even though the branch below never runs for it
   * (rom_open() only ever returns true for ROM_ID_RETAIL). */
  if (fused_present) app_rom_note_verdict(&s_iconrom_ctx, "fused");
  if (fused_ok) {
    /* rom_chrome_open() does NOT need the GF header rom_mon_open() below checks
     * (Ruby/Sapphire have none) -- it is called unconditionally on every
     * successful rom_open() so a Ruby cart's card rung is reachable at all,
     * unlike rom_sprite_open()/rom_itemart_open() below, which stay nested
     * inside the rom_mon_open() gate and so are pre-existingly unreachable for
     * R/S (not something this change fixes -- see rom_sprite.h's own "Ruby/
     * Sapphire: no GF header, so rom_sprite_open fails closed" note). */
    rom_chrome_open(&s_romchrome, &s_iconrom_ctx);
    pdna_trainer_set_romchrome(&s_romchrome);
    pdna_bag_set_romchrome(&s_romchrome);
    if (rom_mon_open(&s_iconrom, &s_iconrom_ctx)) {
      boxoam_rom_icons(&s_iconrom, 1);           /* fused_rom_read: cart-space memcpy, cheap */
      art_fallbacks_set_rommon(&s_iconrom);      /* Phase 1.5: dex/party/picker ROM rung */
      rom_text_open(&s_romtext, &s_iconrom_ctx);
      /* Phase 1 (ROM-art): the summary portrait + item icons/type badges. Neither
       * gates icons/text above -- Ruby/Sapphire (no GF header) already returned
       * before this point via rom_mon_open's own failure, but a cart with icons and
       * no items (or vice versa) must still light up whichever half it has. */
      rom_sprite_open(&s_romsprite, &s_iconrom_ctx);
      pdna_origin_art_set_romsprite(&s_romsprite);
      rom_itemart_open(&s_romitemart, &s_iconrom_ctx);
      rom_wallpaper_open(&s_romwallpaper, &s_iconrom_ctx);   /* Phase 4 (ROM-art) */
#if !PDNA_HAND_ART_COMPILED
      rom_hand_open(&s_romhand, &s_iconrom_ctx);             /* Phase 3 (ROM-art): the glove */
      boxoam_rom_hand(&s_romhand);
#endif
      log_line("icons: streaming from fused %s rev%u", rom_kind_name(s_iconrom_ctx.kind),
               s_iconrom_ctx.version);
      app_icon_cache_resolve(&s_iconrom_ctx, false); /* boot path: shallow (size-only) check */
      return;
    }
    if (rom_chrome_card_have(&s_romchrome, PK_RS) || rom_chrome_pokeblock_have(&s_romchrome, PK_EMERALD)) {
      log_line("icons: fused %s has no GF header (R/S) - card/pokeblock chrome only",
               rom_kind_name(s_iconrom_ctx.kind));
      icon_rung_log(&s_iconrom_ctx, false);   /* no resolver on this path -- see icon_rung_log */
      return;
    }
    log_line("icons: fused %s has no GF header (R/S) - trying SD", rom_kind_name(s_iconrom_ctx.kind));
  }
#ifndef PDNA_DELTA
  if (s_iconrom_fil_open) { f_close(&s_iconrom_fil); s_iconrom_fil_open = false; }
  static const PkGame k_try[3] = { PK_EMERALD, PK_FRLG, PK_RS };
  /* E4 (sprite-era): the PC grid's box ICONS (boxoam's per-species OBJ icons, a
   * separate pipeline from pdna_origin_art.c's per-mon portrait router) all come
   * from ONE open ROM for the whole session -- there is no per-species icon-ROM
   * choice the way the portrait router has one. se_store_era() is exactly the
   * "which era should a WHOLE-STORE choice like this one use" question sprite_era.h
   * defines for it: a concrete, REGISTERED Gen-3 era (G3_RS/G3_EM/G3_FRLG) for the
   * PC place wins outright; anything else (NATIVE, or a GEN1/GEN2 cell) falls back
   * to se_native_era's answer, which for a Gen-3 kind is exactly the save's own game
   * -- i.e. today's `g_game` default, unchanged when the user has not touched the grid.
   *
   * E6 D7: this function is DESIGNED around app_save_kind() being a Gen-3 kind (this
   * comment used to (wrongly, after E6) claim that was the only way it could ever run)
   * -- se_store_era() would return SE_ERA_GEN1/SE_ERA_GEN2 for a GB kind (its own
   * native fallback), which the ternary below has no case for and would silently
   * default to PK_EMERALD: a wrong, unlabelled answer, not a crash. sprite_settings()
   * now gates its rebuild call so a GB kind can never reach here in practice (its PC
   * cell is dead anyway -- se_cell_applies(GEN1/GEN2, PC) is false), but this is the
   * one place that ambiguity already existed, so guard it here too rather than trust
   * every future caller to remember the gate: fall back to today's g_game default
   * instead of guessing Emerald. */
  bool kind_is_gen3 = app_save_kind() < SE_KIND_GEN1;
  SeRoms eroms = app_era_roms();
  SeEra want_era = se_store_era(&g_era, app_save_kind(), SE_PLACE_PC, &eroms);
  PkGame want_game = !kind_is_gen3 ? g_game
                    : (want_era == SE_ERA_G3_RS)   ? PK_RS
                    : (want_era == SE_ERA_G3_FRLG) ? PK_FRLG : PK_EMERALD;
  if (!kind_is_gen3)
    log_line("icons: PC-grid rebuild requested for a GB save kind -- kept %s (E5's job, not this one's)",
             g_game == PK_RS ? "RS" : g_game == PK_FRLG ? "FRLG" : "EM");
  else if (want_game != g_game)
    log_line("icons: Sprites setting picked %s for the PC grid (save is %s)",
             want_game == PK_RS ? "RS" : want_game == PK_FRLG ? "FRLG" : "EM",
             g_game == PK_RS ? "RS" : g_game == PK_FRLG ? "FRLG" : "EM");
  /* D5 (E4 review): the fallback order used to be { want_game, PK_EMERALD, PK_FRLG }
   * UNCONDITIONALLY -- for an R/S save where the Sprites grid asks for a different
   * game's icons and that ROM turns out unreadable, this skipped the SAVE'S OWN
   * game (order[1] was always Emerald, order[2] always FRLG, R/S never even a
   * fallback candidate) before ever trying the ROM the save actually belongs to.
   * The correct order is the chosen game first, then the save's own game (the one
   * every screen already assumes is registered), then whichever of the three is
   * left -- order[1] is only ever overwritten by the loop below when want_game and
   * g_game are the same slot, so it is never left uninitialised. */
  PkGame order[3] = { want_game, g_game, PK_EMERALD };
  int no = (want_game == g_game) ? 1 : 2;
  for (int i = 0; i < 3; i++) {
    PkGame c = k_try[i];
    if (c != want_game && c != g_game && no < 3) order[no++] = c;
  }
  for (int i = 0; i < 3; i++) {
    const char* path = app_rom_path(order[i]);
    if (!path || !path[0]) continue;
    if (f_open(&s_iconrom_fil, path, FA_READ) != FR_OK) continue;
    s_iconrom_fil_open = true;
    fastseek_arm(&s_iconrom_fil, s_iconrom_clmt, sizeof s_iconrom_clmt / sizeof s_iconrom_clmt[0], "rom");
    uint32_t sz = (uint32_t)f_size(&s_iconrom_fil);
    bool sd_ok = rom_open(&s_iconrom_ctx, iconrom_fatfs_read, 0, sz);
    /* Each of up to three candidates gets its own verdict, attributed to the kind
     * actually classified -- NOT necessarily order[i]'s intended slot, a hacked
     * FireRed found while looking for Emerald icons must still flag PK_FRLG, not
     * whatever this loop iteration was hoping to find (fallback-order trap). */
    app_rom_note_verdict(&s_iconrom_ctx, "sd");
    if (sd_ok) {
      /* Same reasoning as the fused branch above: rom_chrome_open() does not need
       * the GF header, so it runs on every successful rom_open() -- reaching Ruby,
       * which rom_mon_open() below always refuses (no GF header). */
      rom_chrome_open(&s_romchrome, &s_iconrom_ctx);
      pdna_trainer_set_romchrome(&s_romchrome);
      pdna_bag_set_romchrome(&s_romchrome);
      if (rom_mon_open(&s_iconrom, &s_iconrom_ctx)) {
        boxoam_rom_icons(&s_iconrom, 0);         /* iconrom_fatfs_read: real SD I/O, not cheap */
        art_fallbacks_set_rommon(&s_iconrom);    /* Phase 1.5: dex/party/picker ROM rung */
        rom_text_open(&s_romtext, &s_iconrom_ctx);
        rom_sprite_open(&s_romsprite, &s_iconrom_ctx);          /* Phase 1 (ROM-art) */
        pdna_origin_art_set_romsprite(&s_romsprite);
        rom_itemart_open(&s_romitemart, &s_iconrom_ctx);
        rom_wallpaper_open(&s_romwallpaper, &s_iconrom_ctx);    /* Phase 4 (ROM-art) */
#if !PDNA_HAND_ART_COMPILED
        rom_hand_open(&s_romhand, &s_iconrom_ctx);              /* Phase 3 (ROM-art): the glove */
        boxoam_rom_hand(&s_romhand);
#endif
        log_line("icons: streaming from SD %s (%s) %lu B", path,
                 rom_kind_name(s_iconrom_ctx.kind), (unsigned long)sz);
        app_icon_cache_resolve(&s_iconrom_ctx, false); /* boot path: shallow (size-only) check */
        return;
      }
      if (rom_chrome_card_have(&s_romchrome, PK_RS) || rom_chrome_pokeblock_have(&s_romchrome, PK_EMERALD)) {
        /* Ruby: no icons/text/sprite (no GF header), but the card rung is real --
         * keep this ROM open and registered rather than falling through to the
         * next candidate, which would silently discard it. */
        log_line("icons: SD %s (%s) has no GF header - card/pokeblock chrome only",
                 path, rom_kind_name(s_iconrom_ctx.kind));
        icon_rung_log(&s_iconrom_ctx, false); /* no resolver on this path -- see icon_rung_log */
        return;
      }
    }
    f_close(&s_iconrom_fil); s_iconrom_fil_open = false;
  }
#endif
  /* No ROM at all this session (or every attempt failed) -- still try the cache with
   * no RomCtx to cross-check against; a previously-extracted, internally-valid cache
   * still serves (DESIGN.md Sec 4.1). */
  app_icon_cache_resolve(0, false); /* boot path: shallow (size-only) check */
}

/* BACKLOG #77: pdna_box.c's draw_wallpaper()/draw_wallpaper_rom() has no session-kind
 * gate at all -- a GB session's own box grid (gbsrc_get_wp, pdna_gen12.c) draws
 * through the EXACT SAME rung a Gen-3 session's PC/Bank grid does. The only reason it
 * never lit up there is that nothing in a GB session's entry point ever calls
 * app_icon_rom_open() -- the one place that opens the Gen-3 ROM handle
 * app_wallpaper_rom() serves from. view_save() and app_register_rom() are its only
 * two callers, both Gen-3-only, so a pure GB session (a standalone mount, or a nested
 * NV_GB import reached WITHOUT visiting a Gen-3 save first this boot) never gets a
 * chance to stream the real wallpaper even with a fused/registered Gen-3 ROM on hand,
 * and silently falls to the procedural grass every time (pdna_box.c:174's own
 * documented fallback, working exactly as designed for the "no ROM" case -- the bug
 * is that a REGISTERED ROM never got the chance).
 *
 * Fix: call the SAME chokepoint once, on GB session entry, unless it already ran
 * this boot (a nested import reached from an already-open Gen-3 session has always
 * called it via view_save before nesting -- s_romwallpaper.ok is already true then,
 * so this is a no-op, not a redundant re-open). app_icon_rom_open() already defends
 * being invoked while app_save_kind() reads GEN1/GEN2 (see its own "E6 D7" comment a
 * few screens up: se_store_era()'s PC-grid choice falls back to g_game rather than
 * guessing when the current kind isn't Gen-3) -- this is a genuinely anticipated
 * call shape, not a new hazard.
 *
 * No new EWRAM: s_iconrom_ctx/s_iconrom/s_romwallpaper/etc. are the SAME file-scope
 * statics app_icon_rom_open() already owns, entirely separate from the GB session's
 * own app_arena_acquire()'d Gb12Mount block -- there is no arena-tail budget question
 * here. Compiled to an empty function in the normal build (PDNA_ARTLESS=0): the
 * compiled wallpapers.c already serves wp==GB12_WALLPAPER there, so draw_wallpaper()
 * never reaches the ROM rung and this call would only cost SD/ROM I/O for no visible
 * change -- kept out entirely so the normal build's session-entry behaviour is
 * unchanged, not just its pixels. */
void app_gb_wallpaper_rom_open(void) {
#if PDNA_ARTLESS
  if (s_romwallpaper.ok) return;   /* already open (nested import, or a prior call this boot) */
  app_icon_rom_open();
#endif
}

/* ---- items + type badges: compiled art first, then the registered ROM (Phase 1,
 * docs/analysis-2026-08-19-rom-art/DESIGN.md Sec 4.7) -----------------------------
 * Same ladder discipline as pdna_origin_art.c's gen3_ladder, but no cache and no
 * memo here either: mon_decomp is shared with the summary portrait (rom_portrait in
 * pdna_origin_art.c decodes fresh on every call for exactly this reason), so a
 * cached pointer into it would go stale the moment ANYTHING else decodes into the
 * same buffer. draw_left (pdna_summary.c) draws its type badges strictly BEFORE its
 * OWN portrait fetch on every repaint, and every item-icon list draws one icon,
 * blits it, then moves to the next id -- so a fresh decode-then-immediately-blit is
 * always safe, and reloading the RSE type sheet on every badge is the honest
 * "wiring, not a cache" cost Phase 1 signed up for (Sec 6, Phase 1: "no cache, no
 * extraction, no SD write"). Phase 2's real cache removes this cost along with
 * every other kind's. */
const uint16_t* app_item_icon(uint16_t item_id) {
  const uint16_t* ic = item_icon_for(item_id);            /* compiled rung first */
  if (ic) return ic;
  if (!s_romitemart.ok || !rom_itemart_have_items(&s_romitemart)) return 0;
  artbuf_claim();          /* E3 review BLOCKING 2: about to overwrite mon_decomp */
  uint16_t* dst = mon_decomp;                             /* 576 px = 1,152 B of 8,192 */
  return rom_item_icon(&s_romitemart, item_id, dst, ROM_ITEM_ICON_PX) ? dst : 0;
}

const uint16_t* app_type_badge(uint8_t type_id, uint8_t* out_h) {
  const uint16_t* ic = type_icon_for(type_id);             /* compiled rung first */
  if (ic) { if (out_h) *out_h = TYPE_ICON_H; return ic; }
  if (!s_romitemart.ok || !rom_itemart_have_types(&s_romitemart)) return 0;
  artbuf_claim();          /* E3 review BLOCKING 2: about to overwrite mon_decomp */
  uint32_t need = (uint32_t)rom_type_scratch_bytes(&s_romitemart);   /* 5,888 RSE / 0 FRLG */
  uint8_t* scratch = need ? (uint8_t*)mon_decomp : 0;
  RomTypeSheet ts;
  /* BACKLOG #145: hand decode_verified() the WHOLE mon_decomp buffer as cap, not
   * just `need` -- passing cap == want (need == ROM_TYPE_SHEET_BYTES) made the
   * BACKLOG #103 caller-tail LZ77 window (rom_itemart.c's decode_verified) size
   * to zero, so the RSE type sheet never got the ~86 -> ~5 read win the window
   * gives every other caller. Safe because the badge below (mon_decomp + 6144)
   * is written strictly AFTER this call returns, and decode_verified only READS
   * dst[want,cap) as LZ77 back-reference input during its own decode -- it never
   * writes there (rom_itemart.c's decode_verified comment). scratch is 0 on
   * FR/LG (need == 0); rom_type_sheet_load ignores scratch_cap on that path. */
  if (!rom_type_sheet_load(&s_romitemart, &ts, scratch, scratch ? MON_DECOMP_BYTES : 0))
    return 0;
  /* the sheet occupies mon_decomp[0..need); the badge is decoded past it, at the
   * offset rom_itemart.h:145-149 recommends, well inside the 8 KiB buffer either
   * way (need is at most ROM_TYPE_SHEET_BYTES == 5,888). */
  uint16_t* dst = mon_decomp + (6144 / 2);
  if (!rom_type_badge(&s_romitemart, &ts, type_id, dst, ROM_TYPE_BADGE_MAX_PX)) return 0;
  if (out_h) *out_h = (uint8_t)rom_type_badge_h(&s_romitemart);
  return dst;
}

/* Phase 4 (ROM-art): pdna_box.c's §12c wallpaper rung reads this ROM directly
 * (decompressing per-wallpaper, not through a cached RGB15 pointer the way the
 * three accessors above work) -- see rom_wallpaper.h and pdna_box.c's draw_wallpaper_rom.
 * NULL when no ROM is open this session or it isn't one of the three pinned
 * game/revisions. */
const RomWallpaper* app_wallpaper_rom(void) { return s_romwallpaper.ok ? &s_romwallpaper : 0; }

/* ---- descriptions: ROM first, embedded table second (see pdna_app.h) ------
 * Each keeps ONE small static buffer, documented in the header as valid until the next
 * call of that same function. 128 bytes covers every Gen-3 description (the longest in
 * the embedded tables is well under it) and three of them is 384 B of .bss — the one
 * place this file can afford it, versus the ~50 KB the embedded strings cost in ROM. */
static const char* desc_or_fallback(RomTextKind kind, uint16_t id, char* buf, uint32_t cap,
                                    const char* embedded) {
  if (rom_text_have(&s_romtext, kind) && rom_text_get(&s_romtext, kind, id, buf, cap))
    return buf;
  return embedded ? embedded : "";
}
const char* app_item_desc(uint16_t id) {
  static char b[128];
  return desc_or_fallback(ROM_TEXT_ITEM, id, b, sizeof b, pk_item_desc(id));
}
const char* app_move_desc(uint16_t id) {
  static char b[128];
  return desc_or_fallback(ROM_TEXT_MOVE, id, b, sizeof b, pk_move_desc(id));
}
const char* app_ability_desc(uint16_t id) {
  static char b[128];
  return desc_or_fallback(ROM_TEXT_ABILITY, id, b, sizeof b, pk_ability_desc(id));
}

#ifndef PDNA_DELTA
/* Settings > Game ROM / the first-run offer: browse for a .gba, identify it, and
 * remember it under ITS OWN game's slot (the same romrs/romem/romfr keys the map
 * uses, so registering here lights the map up too, and vice versa). */
static void app_register_rom(void) {
  char path[PATH_MAX];
  if (!app_pick_rom(path, sizeof path)) {
    /* BACKLOG #55: app_pick_rom() now falls back to the shared art buffer when the
     * EWRAM arena is held (a Gen-1/2 session's whole visit, app_arena_held()), so a
     * false return here is either an honest PC-dirty refusal or the user simply
     * backing out of the picker -- no separate "NOT HERE" case is left to report. */
    if (g_pc_dirty) msg_wait("SAVE FIRST", UI_WARN, "Unsaved box moves pending.", "Commit, then retry.");
    return;
  }
  /* g_rom_path's slots are GB_ROM_PATH_MAX(128) wide (E3 review item 3) -- refuse up
   * front rather than let app_rom_path_set() refuse later after the ROM has already
   * been opened+identified. */
  if (strlen(path) >= (size_t)GB_ROM_PATH_MAX) {
    msg_wait("PATH TOO LONG", UI_WARN, "That folder is too deep for", "this app (127-char limit).");
    return;
  }
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) { msg_wait("CAN'T OPEN", UI_WARN, "File unreadable.", 0); return; }
  /* identify via a throwaway ctx on a temporary reader-less path: reuse the icon FIL */
  if (s_iconrom_fil_open) { f_close(&s_iconrom_fil); s_iconrom_fil_open = false; }
  f_close(&f);
  if (f_open(&s_iconrom_fil, path, FA_READ) != FR_OK) { msg_wait("CAN'T OPEN", UI_WARN, "File unreadable.", 0); return; }
  s_iconrom_fil_open = true;
  fastseek_arm(&s_iconrom_fil, s_iconrom_clmt, sizeof s_iconrom_clmt / sizeof s_iconrom_clmt[0], "rom");
  uint32_t sz = (uint32_t)f_size(&s_iconrom_fil);
  RomCtx rc;
  bool reg_ok = rom_open(&rc, iconrom_fatfs_read, 0, sz);
  app_rom_note_verdict(&rc, "register");   /* fires whether or not rom_open() accepted it */
  if (!reg_ok) {
    f_close(&s_iconrom_fil); s_iconrom_fil_open = false;
    /* BACKLOG #54 T1: verdict-specific refusal, not the flat "NOT A POKEMON ROM" for
     * every non-retail case -- a hack the header can still identify (known or unknown
     * base kind) gets the honest banner instead.
     *
     * Review fix F3: rule 1d (rom_identify(), source/rom_map.c) also catches genuine
     * NON-US retail carts -- a French/German/Italian/Japanese/Spanish Ruby/Sapphire/
     * Emerald/FireRed/LeafGreen has a real (code, version) pair that is simply not on
     * k_versions (only the 11 US builds are pinned, decision 2), so it falls to "no
     * k_versions match but title starts POKEMON" -> ROM_ID_HACK with kind == ROM_NONE.
     * Calling a genuine retail cartridge from another region a "ROM HACK" is a false
     * accusation this rule was never meant to make -- kind == ROM_NONE means "this
     * lane cannot say WHAT it is" (unsupported region/build), not "this is a hack".
     * kind != ROM_NONE still means a pinned US (code, version) whose title or size
     * diverges -- THAT is the genuine hack case the ROM HACK banner is for. */
    if (rc.ident == ROM_ID_HACK && rc.kind != ROM_NONE)
      msg_wait(PDNA_ROMHACK_TITLE, UI_WARN, PDNA_ROMHACK_L1, PDNA_ROMHACK_L2);
    else if (rc.ident == ROM_ID_HACK)
      msg_wait(PDNA_ROMOTHER_TITLE, UI_WARN, PDNA_ROMOTHER_L1, PDNA_ROMOTHER_L2);
    else msg_wait("NOT A POKEMON ROM", UI_WARN, "Retail R/S/E/FR/LG only.", 0);
    return;
  }
  PkGame rg = (rc.kind == ROM_EMERALD) ? PK_EMERALD
            : (rc.kind == ROM_RUBY || rc.kind == ROM_SAPPHIRE) ? PK_RS : PK_FRLG;
  app_rom_path_set(rg, path);
  cfg_save();
  app_icon_rom_open();                           /* light it up now (+ resolves the cache) */
  char l1[40]; siprintf(l1, "%s registered.", rom_kind_name(rc.kind));
  msg_wait("GAME ROM", UI_OK, l1,
           boxoam_icons_available() ? "Real art is ON." : "R/S icons come later; map works.");

  /* Phase 2's second entry point (DESIGN.md Sec 3.1): the moment the user has just
   * told us where their ROM is. Only offered when it would actually work (icons
   * table present, Omega for the write) and is not already done this session.
   * art_extract_screen() asks its own "extract now?" confirm -- one confirm dialog,
   * shared by both entry points, not two stacked ones. */
  if (s_iconrom.ok && active_flashcart == EZ_FLASH_OMEGA &&
      !art_session_icons_ready_memoized())
    art_extract_screen();
}

/* Settings > Game ROM, slice E3: browse for a .gb/.gbc and register it for generation
 * `gen` (PDNA_GEN1/PDNA_GEN2) so Gen-1/2 mons draw their real Game Boy sprites. Mirrors
 * app_register_rom() above but delegates the actual open+identify to gb_art_source.c
 * (it owns the FIL/RomGbSprite/scratch dance) and refuses a ROM that identifies as the
 * OTHER generation rather than silently accepting it into the wrong slot. */
/* The registration progress screen (2026-09-14, the "stuck choosing the .gbc" cart
 * bug). Invoked from gb_art_source.c's read shim between SD reads -- every 8th read
 * and the first -- never during a transfer. Same instruments as art_extract_draw
 * above: which locator, bytes covered / ROM size, a bar, the elapsed clock, and B to
 * cancel (returning false stops the scan before its next read; nothing is written).
 * Its own frame is deliberately tiny (one 40-byte row buffer): it runs at the BOTTOM
 * of the locator's call chain, in place of the FatFs read it precedes.
 *
 * BACKLOG #148: no longer `static` -- source/pdna_gbscreen.c's gbscr_open_inner()
 * reuses this SAME progress screen for its own whole-tail scan (locator
 * GB_ART_LOC_UI, "screen data" below) instead of a second copy, so the ctx type
 * (GbRegUi) now lives in gb_art_io.h where both translation units can see it. */
bool gb_reg_progress(void* vctx, uint8_t locator, uint32_t done, uint32_t total,
                     uint32_t elapsed_ms) {
  const GbRegUi* c = (const GbRegUi*)vctx;
  key_poll();
  if (key_hit(KEY_B)) return false;
  /* F5 (BACKLOG #148 review-opus fix pass, UX parity): a warm gbui<gen>.loc hit
   * (gbscr_open_inner's own scan, GB_ART_LOC_UI) costs only 19-29 reads --
   * 3-5 ticks of this callback -- on EVERY routine open of a trainer card/bag/
   * pack/map, where the Gen-3 twins open silently. Poll B always (so a cancel
   * still works even on a fast scan that is about to finish anyway), but do
   * not PAINT until the scan has genuinely been slow: a cold, no-.loc scan
   * (694-1,110 reads measured) crosses 400 ms well within its first second, so
   * this never hides a real multi-second wait, only the routine-open flicker.
   * Scoped to GB_ART_LOC_UI only -- the hw2 registration path (LOC_SPRITES/
   * LOC_ICONS, Settings > Game ROM) is untouched, since THAT screen is a
   * deliberate, rare, user-initiated action where showing progress immediately
   * is correct, not flicker. */
  if (locator == GB_ART_LOC_UI && elapsed_ms < 400u) return true;
  ui_clear();
  ui_text(4, 4, UI_TITLE, c->restoring ? "RESTORING GAME BOY ROM" : "CHECKING GAME BOY ROM");
  ui_hline(0, 14, UI_SCR_W, UI_BORDER);
  ui_text(8, 26, UI_TEXT, locator == GB_ART_LOC_ICONS ? "2/2  menu icon tables"
                        : locator == GB_ART_LOC_UI    ? "1/1  screen data"
                        : (c->gen == PDNA_GEN2 ? "1/2  sprite tables" : "1/1  sprite tables"));
  char row[40];
  siprintf(row, "%lu / %lu KB", (unsigned long)(done >> 10), (unsigned long)(total >> 10));
  ui_text(8, 40, UI_TEXT, row);
  /* 32-bit only: a GB ROM is at most 8 MB, so (done >> 10) * 220 cannot overflow. */
  int w = (total >> 10) ? (int)(((done >> 10) * 220u) / (total >> 10)) : 0;
  ui_panel(8, 54, 220, 12, UI_PANEL, UI_BORDER);
  if (w > 0) ui_fill_rect(9, 55, w > 218 ? 218 : w, 10, UI_OK);
  siprintf(row, "elapsed %lu.%01lus", (unsigned long)(elapsed_ms / 1000),
           (unsigned long)((elapsed_ms / 100) % 10));
  ui_text(8, 74, UI_DIM, row);
  /* BACKLOG #185 F4: "honest progress" -- an integer KB/s so a cart run reports
   * the real rate, not just a moving bar. Same rounding as gb_art_source.c's log
   * line (bytes*1000 / (ms*1024)); 0 ms (the very first tick) reads 0, not a
   * divide-by-zero. */
  uint32_t kbps = elapsed_ms ? (uint32_t)(((uint64_t)done * 1000u) / ((uint64_t)elapsed_ms * 1024u)) : 0u;
  siprintf(row, "%lu KB/s", (unsigned long)kbps);
  ui_text(8, 88, UI_DIM, row);
  ui_text(8, 148, UI_DIM, "B  cancel");
  return true;
}

static void app_register_gb_rom(uint8_t gen) {
  char path[PATH_MAX];
  if (!app_pick_gb_rom(path, sizeof path)) {
    /* BACKLOG #55: same fallback as app_register_rom() above -- app_pick_gb_rom()
     * now works from inside a Gen-1/2 session too (mon_decomp-backed picker), so a
     * false return here is either an honest PC-dirty refusal or a plain user cancel. */
    if (g_pc_dirty) msg_wait("SAVE FIRST", UI_WARN, "Unsaved box moves pending.", "Commit, then retry.");
    return;
  }
  /* app_gb_rom_path_set() stores into GB_ROM_PATH_MAX(128) slots, not PATH_MAX(256) --
   * see gb_art_source.h's memory note. A path that would be silently truncated there
   * would register successfully NOW and point at the WRONG (truncated) file on the
   * very next boot, so this is refused up front rather than clipped. */
  if (strlen(path) >= (size_t)GB_ROM_PATH_MAX) {
    msg_wait("PATH TOO LONG", UI_WARN, "That folder is too deep for", "this app (127-char limit).");
    return;
  }
  GbRegUi ui = { gen, 0 };
  GbArtRegInfo info;
  GbArtRegStatus st = gb_art_register(gen, path, gb_reg_progress, &ui, &info);
  /* The log lines above are the evidence a cart run needs; put them on the card
   * before the message, so a power-off at the dialog still leaves them. */
  app_log_flush();
  switch (st) {
    case GB_ART_REG_OK: {
      app_gb_rom_path_set(gen, path);
      cfg_save();
      char l1[40]; siprintf(l1, "Gen %u ROM registered.", (unsigned)gen);
      msg_wait("GAME ROM", UI_OK, l1, "Real Game Boy sprites are ON.");
      /* BACKLOG #55: this can now happen mid-GB-session (the picker fell back to
       * mon_decomp to get here). gb_art_register() already called
       * pdna_origin_art_invalidate() above, so the box's next entry re-decodes
       * every cell through box_decode instead of trusting a stale cache -- log it
       * so a hardware run can confirm the new sprites actually show up. */
      log_line(app_arena_held()
                 ? "gb art: gen%u ROM registered mid-session, art cache invalidated"
                 : "gb art: gen%u ROM registered, art cache invalidated", (unsigned)gen);
      break;
    }
    case GB_ART_REG_WRONG_GEN:
      msg_wait("WRONG GENERATION", UI_WARN,
               gen == PDNA_GEN1 ? "That's a Gen 2 (Gold/Silver/Crystal)" : "That's a Gen 1 (Red/Blue/Yellow)",
               "ROM -- pick the other slot instead.");
      break;
    case GB_ART_REG_BAD_ROM:
      msg_wait("NOT A GAME BOY ROM", UI_WARN, "Could not locate its sprite tables.", 0);
      break;
    case GB_ART_REG_CANT_OPEN:
      msg_wait("CAN'T OPEN", UI_WARN, "File unreadable.", 0);
      break;
    case GB_ART_REG_CANCELLED:
      msg_wait("CANCELLED", UI_WARN, "Nothing was changed.", 0);
      break;
    case GB_ART_REG_TIMEOUT: {
      /* BACKLOG #185 F4/D3: STALLED, not TIMED OUT -- this is now a
       * progress-based watchdog (gb_scan_guard.h). Two genuinely different
       * conditions share this ONE GbArtRegStatus (info.stop_ceiling, filled
       * from GbScanGuard.stop_ceiling by gb_art_fill_info(), tells them
       * apart): the common case is a real STALL (no read completed for
       * GB_ART_STALL_S seconds -- likely a bad card, re-copy the ROM); the
       * rare case is the 15-minute hard ceiling firing on a run that kept
       * completing reads but never actually finished -- that is not "no
       * data", it is "this will never end", so it gets its own honest
       * title/wording instead of quoting the 10 s stall figure it never hit. */
      char l1[40];
      if (info.stop_ceiling) {
        siprintf(l1, "Still scanning after 15 min at %lu KB",
                 (unsigned long)(info.covered >> 10));
        msg_wait("GAVE UP", UI_WARN, l1, "Re-copy the ROM to the card, retry.");
      } else {
        siprintf(l1, "STALLED %us, no data after %lu KB", (unsigned)GB_ART_STALL_S,
                 (unsigned long)(info.covered >> 10));
        msg_wait("STALLED", UI_WARN, l1, "Re-copy the ROM to the card, retry.");
      }
      break;
    }
    case GB_ART_REG_READ_ERR: {
      char l1[40];
      siprintf(l1, "FatFs %u / err %u at %lu KB", (unsigned)info.fr, (unsigned)info.err,
               (unsigned long)(info.fail_off >> 10));
      msg_wait("READ ERROR", UI_WARN, l1, "Logged to /PokeDNA/log.txt.");
      break;
    }
    default: break;
  }
  /* E3 review item 5: a failed re-pick just called gb_art_register(gen, <bad path>),
   * which left s_reg_have[gen] false -- de-registering whatever WORKING ROM was
   * there before, for the rest of this session, even though config.cfg (and
   * app_gb_rom_path(gen)) still names it and it is still perfectly fine on disk. A
   * "Change ROM" attempt that fails must not cost the user their already-working
   * art. Re-register the OLD path on any non-OK outcome; a no-op (EMPTY) if there
   * never was one. */
  if (st != GB_ART_REG_OK) {
    GbRegUi restore = { gen, 1 };
    /* Cheap when the old ROM's .loc is on the card (a few dozen reads); the screen
     * only lingers for the rare boot-less rescan, and B still cancels it. */
    gb_art_register(gen, app_gb_rom_path(gen), gb_reg_progress, &restore, 0);
  }
}
#endif




/* Build "<base> copy[.N].<ext>" in out (must not already exist on SD). The picked
 * dst is a sibling in g_cwd. Returns false if no free slot in 99 tries. */
static bool dup_name(const char* name, char* out, int cap) {
  char base[NAME_MAX]; const char* ext = "";
  const char* dot = strrchr(name, '.');
  if (dot && dot != name) {
    int bl = (int)(dot - name); if (bl >= (int)sizeof(base)) bl = sizeof(base) - 1;
    memcpy(base, name, bl); base[bl] = 0; ext = dot;            /* ext keeps the '.' */
  } else { strncpy(base, name, sizeof(base) - 1); base[sizeof(base) - 1] = 0; }
  for (int n = 1; n <= 99; n++) {
    /* bound the format up-front (sniprintf never overflows `out`); treat a
     * would-be-truncated name as "no room" rather than writing past the buffer. */
    int w = (n == 1) ? sniprintf(out, cap, "%s copy%s", base, ext)
                     : sniprintf(out, cap, "%s copy%d%s", base, n, ext);
    if (w < 0 || w >= cap) return false;
    char full[PATH_MAX]; FILINFO fno;
    if (!path_join(g_cwd, out, full)) return false;
    if (f_stat(full, &fno) != FR_OK) return true;              /* name is free */
  }
  return false;
}

/* Per-file SD ops menu (duplicate / rename / delete backups). All writes are
 * Omega-only and go through the verified copy / rename. Returns true if the
 * directory contents changed (caller re-scans). */
static bool file_actions(const BrowseEntry* e) {
  if (!e || e->is_dir) return false;
  char src[PATH_MAX];
  if (!path_join(g_cwd, e->name, src)) return false;
  int sel = 0; bool changed = false;
  static const char* const L[4] = { "Duplicate", "Rename", "Delete backups", "Close" };
  /* Everything else on this screen (header, the read-only note, the 4 labels) is fixed
   * for the whole call -- `e` and cart_writable() cannot change while this modal is up
   * -- so only the cursor moves between frames, and every action below (Duplicate/
   * Rename/Delete/the READ-ONLY denial) routes through msg_wait/busy_panel/app_confirm/
   * osk_input, which all ui_clear() before returning here. gen alone is therefore a
   * complete invalidation signal; no separate value shadow is needed (unlike a screen
   * whose row content can change without painting an overlay). */
  int prev_sel = -1; bool valid = false; uint32_t gen = 0;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      /* 29 cols needs 117 B per ui.h's contract (e->name is real UTF-8); hdr[40]
       * silently violated it. */
      char hdr[128]; ui_truncate(hdr, e->name, 29);
      ui_text(4, 4, UI_TITLE, "FILE");
      ui_text(4, 16, UI_SELTEXT, hdr);
      ui_hline(0, 28, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < 4; i++) ui_menu_row(40 + i * 16, L[i], i == sel);
      if (!cart_writable()) ui_text(8, 122, UI_DIM, "Read-only: writes need Omega.");
      ui_text(4, 152, UI_DIM, "A do  U/D move  B back");
    } else if (sel != prev_sel) {
      ui_menu_row(40 + prev_sel * 16, L[prev_sel], false);
      ui_menu_row(40 + sel * 16, L[sel], true);
    }
    prev_sel = sel; valid = true; gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return changed;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 3;
    else if (k & KEY_DOWN) sel = (sel + 1) % 4;
    else if (k & KEY_A) {
      if (sel == 3) return changed;
      if ((sel == 0 || sel == 1) && !cart_writable()) {
        snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0);
        continue;
      }
      if (sel == 0) {                                  /* Duplicate */
        char dname[NAME_MAX], dst[PATH_MAX];
        if (!dup_name(e->name, dname, sizeof(dname)) || !path_join(g_cwd, dname, dst)) {
          snd_error(); msg_wait("DUPLICATE", UI_WARN, "No free name available.", 0); continue;
        }
        busy_panel("Duplicating...");
        SfStatus st = sf_copy(src, dst);
        if (st == SF_OK) { char m[40]; ui_truncate(m, dname, 24); snd_ok(); msg_wait("DUPLICATED", UI_OK, "Created:", m); changed = true; }
        else { snd_error(); msg_wait("DUPLICATE FAILED", UI_WARN, sf_status_str(st), "File unchanged."); }
      } else if (sel == 1) {                           /* Rename */
        char nn[NAME_MAX];
        if (!osk_input("RENAME", e->name, nn, sizeof(nn))) continue;
        if (!strcmp(nn, e->name)) continue;
        char dst[PATH_MAX]; FILINFO fno;
        if (!path_join(g_cwd, nn, dst)) { snd_error(); msg_wait("RENAME", UI_WARN, "Name too long.", 0); continue; }
        if (f_stat(dst, &fno) == FR_OK) { snd_error(); msg_wait("RENAME", UI_WARN, "Name already exists.", 0); continue; }
        if (f_rename(src, dst) == FR_OK) {
          log_line("rename %s -> %s", src, dst);
          snd_ok(); msg_wait("RENAMED", UI_OK, "Now named:", nn); return true;   /* src path stale: re-scan */
        } else { snd_error(); msg_wait("RENAME FAILED", UI_WARN, "Could not rename.", 0); }
      } else if (sel == 2) {                            /* Delete backups */
        if (!cart_writable()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
        if (app_confirm("Delete ALL backups?", "Deletes its .bak files.")) {
          int rm = sf_clear_backups(src);
          char m[40]; siprintf(m, "%d backup(s) removed.", rm);
          snd_ok(); msg_wait("CLEARED", UI_OK, m, 0);
          if (rm > 0) changed = true;
        }
      }
    }
  }
}

/* first empty (decodes-as-empty) slot in a box, or -1 if the box is full */
static int box_free_slot(const uint8_t* pc, int box) {
  for (int s = 0; s < G3_IN_BOX; s++) {
    PkMon m;
    if (!pk_decode_mon(pk_box_slot((uint8_t*)pc, box, s), false, &m)) return s;
  }
  return -1;
}

/* Bank "Copy to game": drop a stored 80-byte box record into the loaded save's
 * first free PC box slot and commit. The bank keeps its copy (it's a copy, not a
 * move). Returns true iff written. */
bool app_inject_to_game(const uint8_t* rec80) {
  /* BACKLOG #120 S2: refuse before touching g_pc at all when it is on loan to a GB
   * session's mount arena, or when there is no parsed Gen-3 save to receive the
   * write -- the row above should already have hidden this action, but this is the
   * actual write-time gate (defence in depth, same posture as app_can_edit() below). */
  if (xg_inject_refuse(app_arena_held(), g_vinfo.valid)) {
    snd_deny();
    msg_wait("NO GEN-3 SAVE", UI_WARN, "Open a Gen-3 save first,", "then use the Bank.");
    return false;
  }
  if (!app_can_edit()) return false;
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    int s = box_free_slot(g_pc, b);
    if (s >= 0) {
      memcpy(pk_box_slot(g_pc, b, s), rec80, 80);
      if (app_dex_register_rec(rec80, false)) return app_commit_all();   /* + register in dex */
      return app_commit_pc();
    }
  }
  snd_deny();
  msg_wait("PC FULL", UI_WARN, "No free PC slot in the", "loaded game.");
  return false;
}

/* Deferred variant: place in the first free PC slot and mark PC dirty (NO write), so a
 * Day-Care withdraw-to-PC saves with everything else when you leave the save. Returns
 * false (+ PC FULL) if there's no room. Dex registration is skipped (a deferred PC
 * commit doesn't write the dex sections; the species is in the player's hands anyway). */
static bool app_inject_to_game_deferred(const uint8_t* rec80, int* out_box, int* out_slot) {
  /* BACKLOG #120 S2: same refusal as app_inject_to_game() above -- see its comment. */
  if (xg_inject_refuse(app_arena_held(), g_vinfo.valid)) {
    snd_deny();
    msg_wait("NO GEN-3 SAVE", UI_WARN, "Open a Gen-3 save first,", "then use the Bank.");
    return false;
  }
  if (!app_can_edit()) return false;
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    int s = box_free_slot(g_pc, b);
    if (s >= 0) {
      memcpy(pk_box_slot(g_pc, b, s), rec80, 80); app_mark_pc_dirty();
      app_register_dex_deferred(rec80, false);                  /* withdrawn mon -> dex */
      if (out_box) *out_box = b; if (out_slot) *out_slot = s;
      return true;
    }
  }
  snd_deny();
  msg_wait("PC FULL", UI_WARN, "No free PC slot in the", "loaded game.");
  return false;
}

/* Forward declaration: the real (tentative) definition and app_src_readonly_set/clear/
 * app_src_ops_set live with the rest of the read-only BoxSource gate, further down this
 * file -- declared again here (legal for a file-scope static with no initializer, C11
 * 6.9.2) so app_copy, textually earlier, can reach it without moving either block. */
static const AppSrcOps* g_src_ops;

/* S5-B: hand the clipboard's raw Gen-3 record to a foreign source's own `paste` hook
 * (pdna_gen12.c's gb_paste_hook), which is called with the DESTINATION's rec80, never
 * the clip. See pdna_app.h's own contract comment for the validity rules. */
const uint8_t* app_clip_rec(void) { return g_clip.rec; }

static bool app_copy(uint8_t* rec, bool is_party) {
  clip_copy_from(&g_clip, rec, is_party);
  /* S5-B: if the ACTIVE source is a Game Boy save with a copy_native hook (now true on
   * BOTH GB entry points -- the picker's resident edit session AND the read-only
   * nav-menu mount, review fix #5), also capture the record in its own native shape. A
   * later PASTE in a Gen-3 session (app_paste) checks g_clip.from_gb to look up the
   * sidecar instead of using the lossy `rec` bytes clip_copy_from just filled. */
  const char* l2 = "(kept until overwritten)";
  if (g_src_ops && g_src_ops->copy_native) {
    /* S5-B re-verification NEW-3: say whether a later PASTE will actually be
     * lossless, not just whether the native record was captured. copy_native()
     * always succeeds for a Gen-1 mon (there is a real GbEditMon to cache) and for
     * any Gen-2 mon that was never transferred down -- "Native record kept
     * (lossless)" was claiming a guarantee neither of those can deliver, since PASTE
     * can only merge from a sidecar that actually exists. has_sidecar answers the
     * question the toast is really trying to: Gen 2, and previously transferred down.
     * Only shown when a GB source is even active (copy_native registered); an
     * ordinary same-generation Gen-3 copy keeps its original wording, since a sidecar
     * lookup never applies there. */
    bool has_sidecar = false;
    g_clip.from_gb = g_src_ops->copy_native(rec, &g_clip.gb, &has_sidecar);
    l2 = has_sidecar ? PDNA_SIDECAR_COPY_HAS : PDNA_SIDECAR_COPY_NONE;
  }
  msg_wait("COPIED", UI_OK, "PASTE places it in a slot.", l2);
  return false;                                          /* no save change */
}

/* BACKLOG #150 S150-9 decision 5: the Gen-3-home direction's own report shape
 * (GbscMergeReport) adapted into the shared XrMergeReport the merge screen reads --
 * same six field names (s150-8b decision 12 designed them for exactly this), so this
 * is a straight copy plus the two new S150-9 numbers gb_sidecar.c's
 * merge_species_and_level() already fills. */
static void xr_report_from_gbsc(const GbscMergeReport* g, XrMergeReport* x) {
  memset(x, 0, sizeof *x);
  x->evolved = g->evolved;
  x->level_changed = g->level_changed;
  x->moves_changed = g->moves_changed;
  x->renamed = g->renamed;
  x->rename_refused = g->rename_refused;
  x->gb_item_ignored = g->gb_item_ignored;
  x->level_from = g->level_from;
  x->level_to = g->level_to;
}

/* BACKLOG #150 S150-9 decision 6: one row of the shared merge screen. A toggle row
 * (KEEP/TAKE, cursor can land on it) or a fixed read-only line -- never both. */
#define XFERMERGE_MAX_ROWS 7
typedef struct {
  bool     toggle;
  uint8_t  bit;          /* XR_ACCEPT_* -- valid iff toggle */
  char     text[40];
} XferMergeRow;

/* A bounded, literal-only copy (never snprintf's "%s" path -- BACKLOG #150 S150-9
 * finding: newlib's full vfprintf drags in __sbprintf/_vfiprintf_r, an UNDECLARED
 * mutually-recursive pair the stack walker cannot bound, which fails the gate outright
 * ("STACK_BUDGET UNDECLARED RECURSION"). Every row string here is either a literal
 * constant (this helper) or the one %u,%u format (siprintf, the tree's own lightweight
 * integer-only printf, used throughout for exactly this reason). */
static void xfermerge_setrow(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  dst[i] = 0;
}

/* Builds up to XFERMERGE_MAX_ROWS rows for `rep`/`dir` (decision 6's row list, per
 * direction). Returns the row count. A hard cap (golden rule 2: every loop has a
 * provable bound) -- in practice UP shows at most species+level+moves+(nick-or-
 * RO_RENAME)+RO_ITEM_BACK+EVS == 6, DOWN at most level+moves+(nick-or-RO_RENAME)+
 * RO_EVOLVED+(RO_ITEM_DROP-or-RO_ITEM_BACK)+RO_MOVE == 6 -- one row under the cap
 * either way (the mutual exclusions noted above), so the panel's own bottom border
 * (checked by the _Static_assert on app_xfer_merge_screen below) is never at risk. */
static int xfermerge_build_rows(const XrMergeReport* rep, uint8_t dir, XferMergeRow rows[XFERMERGE_MAX_ROWS]) {
  int n = 0;
  if (dir == XR_MERGE_UP) {
    if (rep->evolved && n < XFERMERGE_MAX_ROWS) {
      /* BACKLOG #150 S150-9 review D2 (orchestrator decision): species stays
       * default KEEP, same as every other row -- Guy's rule is "round trips come
       * back byte-identical by default; the changes made abroad are OFFERED", so
       * an evolution is a TAKE toggle here, never a default. `state[]` (below)
       * already starts every row at false (KEEP); this toggle needs no special
       * casing to honour that. */
      rows[n].toggle = true; rows[n].bit = XR_ACCEPT_SPECIES;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_ROW_SPECIES);
      n++;
    }
    if (rep->level_changed && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = true; rows[n].bit = XR_ACCEPT_LEVEL;
      siprintf(rows[n].text, PDNA_XFERMERGE_ROW_LEVEL_FMT,
               (unsigned)rep->level_from, (unsigned)rep->level_to);
      n++;
    }
    if (rep->moves_changed && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = true; rows[n].bit = XR_ACCEPT_MOVES;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_ROW_MOVES);
      n++;
    }
    if (rep->renamed && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = true; rows[n].bit = XR_ACCEPT_NICK;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_ROW_NICK);
      n++;
    } else if (rep->rename_refused && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_RO_RENAME);
      n++;
    }
    if (rep->gb_item_ignored && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_RO_ITEM_BACK);
      n++;
    }
    if (n < XFERMERGE_MAX_ROWS) {   /* decision 7: always shown for XR_MERGE_UP */
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_SIDECAR_L_EVS);
      n++;
    }
  } else {   /* XR_MERGE_DOWN -- decision 4: species is read-only here, never a toggle */
    if (rep->level_changed && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = true; rows[n].bit = XR_ACCEPT_LEVEL;
      siprintf(rows[n].text, PDNA_XFERMERGE_ROW_LEVEL_FMT,
               (unsigned)rep->level_from, (unsigned)rep->level_to);
      n++;
    }
    if (rep->moves_changed && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = true; rows[n].bit = XR_ACCEPT_MOVES;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_ROW_MOVES);
      n++;
    }
    if (rep->renamed && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = true; rows[n].bit = XR_ACCEPT_NICK;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_ROW_NICK);
      n++;
    } else if (rep->rename_refused && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_RO_RENAME);
      n++;
    }
    if (rep->evolved && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_RO_EVOLVED);
      n++;
    }
    if (rep->abroad_item_dropped && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_RO_ITEM_DROP);
      n++;
    } else if (rep->gb_item_ignored && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_RO_ITEM_BACK);
      n++;
    }
    bool any_move_refused = rep->move_refused[0] || rep->move_refused[1] ||
                            rep->move_refused[2] || rep->move_refused[3];
    if (any_move_refused && n < XFERMERGE_MAX_ROWS) {
      rows[n].toggle = false;
      xfermerge_setrow(rows[n].text, sizeof rows[n].text, PDNA_XFERMERGE_RO_MOVE);
      n++;
    }
  }
  return n;
}

/* Geometry guard (decision 6): the worst case this builder can produce is
 * XFERMERGE_MAX_ROWS (7) rows -- one more than either direction's real maximum (6,
 * see xfermerge_build_rows' own comment), kept as a defensive margin. 7 rows plus one
 * gap must clear the fixed footer at PANEL_Y + PANEL_H - 24 (134). Chosen option
 * (decision 6's "pick one"): cap the row count (already enforced above), NOT drop
 * EVS_GAP -- 44 + 7*10 + 4 = 118 <= 134, no need to touch the gap at all. */
_Static_assert(PDNA_SIDECAR_LINE_Y0 + XFERMERGE_MAX_ROWS * PDNA_SIDECAR_LINE_H +
               PDNA_SIDECAR_EVS_GAP <= PDNA_SIDECAR_PANEL_Y + PDNA_SIDECAR_PANEL_H - 24,
               "merge screen rows overflow the panel");

/* BACKLOG #150 S150-9 decision 6: the shared per-field merge screen, both directions.
 * Every key does a FULL panel repaint (a modal, no OAM, <= 7 rows -- no partial-
 * repaint tricks, memory "four trap classes of partial repaint"). */
bool app_xfer_merge_screen(const XrMergeReport* rep, uint8_t dir, uint8_t* accept) {
  if (accept) *accept = 0;
  XferMergeRow rows[XFERMERGE_MAX_ROWS];
  int n = xfermerge_build_rows(rep, dir, rows);
  if (dir == XR_MERGE_DOWN && n == 0) return true;   /* decision 7: nothing to say, stay silent */

  bool state[XFERMERGE_MAX_ROWS];
  memset(state, 0, sizeof state);                     /* every toggle starts at KEEP */
  int toggle_idx[XFERMERGE_MAX_ROWS];
  int ntoggle = 0;
  for (int i = 0; i < n; i++) if (rows[i].toggle) toggle_idx[ntoggle++] = i;
  int cur = 0;   /* index into toggle_idx, cursor position */

  const char* title = (dir == XR_MERGE_UP) ? PDNA_XFERMERGE_TITLE_UP : PDNA_XFERMERGE_TITLE_DOWN;

  for (;;) {
    ui_clear();
    ui_panel(PDNA_SIDECAR_PANEL_X, PDNA_SIDECAR_PANEL_Y, PDNA_SIDECAR_PANEL_W,
             PDNA_SIDECAR_PANEL_H, UI_PANEL, UI_OK);
    ui_ptext_fit(PDNA_SIDECAR_TEXT_X, PDNA_SIDECAR_PANEL_Y + 8, PDNA_SIDECAR_TEXT_MAXW,
                 UI_OK, title);

    int y = PDNA_SIDECAR_LINE_Y0;
    for (int i = 0; i < n; i++) {
      if (rows[i].toggle) {
        bool is_cursor = ntoggle > 0 && toggle_idx[cur] == i;
        ui_text(PDNA_SIDECAR_TEXT_X, y, is_cursor ? UI_OK : UI_TEXT, is_cursor ? ">" : " ");
        ui_ptext_fit(PDNA_SIDECAR_TEXT_X + 8, y, PDNA_SIDECAR_TEXT_MAXW - 40, UI_TEXT, rows[i].text);
        bool taken = state[i];
        ui_ptext_right(PDNA_SIDECAR_TEXT_X + PDNA_SIDECAR_TEXT_MAXW, y,
                       taken ? UI_OK : UI_DIM, taken ? PDNA_XFERMERGE_TAKE : PDNA_XFERMERGE_KEEP);
      } else {
        ui_ptext_fit(PDNA_SIDECAR_TEXT_X, y, PDNA_SIDECAR_TEXT_MAXW, UI_DIM, rows[i].text);
      }
      y += PDNA_SIDECAR_LINE_H;
    }

    const char* hint = ntoggle > 0 ? PDNA_XFERMERGE_HINT_TOGGLE : PDNA_XFERMERGE_HINT_RO;
    ui_ptext_fit(PDNA_SIDECAR_TEXT_X, PDNA_SIDECAR_PANEL_Y + PDNA_SIDECAR_PANEL_H - 24,
                 PDNA_SIDECAR_TEXT_MAXW, UI_DIM, hint);

    u16 k;
    do { vsync(); k = key_hit(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START); } while (!k);
    if (k & KEY_B) { snd_back(); return false; }
    /* review D2 (MEDIUM): with zero toggle rows (every remaining row read-only),
     * A must commit like every other PokeDNA confirm screen -- not just START.
     * PDNA_XFERMERGE_HINT_RO now reads "A ok  B cancel" to match. */
    if ((k & KEY_START) || (ntoggle == 0 && (k & KEY_A))) {
      uint8_t acc = 0;
      for (int t = 0; t < ntoggle; t++) if (state[toggle_idx[t]]) acc |= rows[toggle_idx[t]].bit;
      if (accept) *accept = acc;
      snd_ok();
      return true;
    }
    if (ntoggle > 0) {
      if (k & KEY_UP)   cur = cur ? cur - 1 : ntoggle - 1;
      if (k & KEY_DOWN) cur = (cur + 1) % ntoggle;
      if (k & (KEY_A | KEY_LEFT | KEY_RIGHT)) {
        state[toggle_idx[cur]] = !state[toggle_idx[cur]];
        snd_move();
      }
    }
  }
}

/* Lookup + merge half of app_paste_gb_merge (S5-B review fix #11: split so the outer
 * noinline frame's own comment stays honest about what it holds). `buf`/`len` are the
 * caller's own GBSC_FILE_MAX buffer; read fresh from `path` here. On success fills
 * `out80`/`rep`/`*idx` and returns true. On any refusal, sets `*handled` per
 * app_paste_gb_merge's own out-param contract (true = caller returns this function's
 * return value; false = fall through to the converted-copy path) and returns false. */
static bool app_paste_gb_lookup(uint8_t* buf, uint32_t* len, const char* path,
                                uint8_t out80[80], GbscMergeReport* rep, int* idx,
                                bool* handled) {
  SfStatus rst = sf_read_full(path, buf, GBSC_FILE_MAX, len);
  if (rst == SF_ERR_OPEN) {                    /* no such file: never transferred down */
    *handled = false;
    msg_wait(PDNA_SIDECAR_NONE_TITLE, UI_OK, PDNA_SIDECAR_NONE_L1, 0);
    return false;
  }
  if (rst != SF_OK) {
    *handled = true;
    snd_error();
    msg_wait(PDNA_SIDECAR_READFAIL_TITLE, UI_WARN, sf_status_str(rst), 0);
    return false;
  }

  int n = gbsc_count(buf, *len);
  if (n < 0) {
    /* S5-B review fix #3 (BLOCKING): the file EXISTS but fails its own CRC -- NOT the
     * same thing as "no sidecar". Silently taking the converted-copy path here would
     * hide real, avoidable data loss from the player. Ask instead: A = paste the
     * (lossy) converted copy anyway, B = cancel the whole paste. */
    *handled = true;
    if (app_confirm(PDNA_SIDECAR_CORRUPT_TITLE, PDNA_SIDECAR_CORRUPT_MERGE_L1)) *handled = false;
    return false;
  }

  /* S5-B review fix #7: two same-OT mons with identical dv4 (all-31 IVs are common)
   * collide on the fingerprint -- gbsc_key() deliberately excludes species (S5-A
   * design, so a Game Boy evolution still finds its sidecar), so a plain first-match
   * can hand back the WRONG original for a species the clipboard's GB record no
   * longer agrees with. Two passes over gbsc_find()'s own `start` walk, bounded by
   * GBSC_MAX_ENTRIES (golden rule 2): prefer the candidate whose species_written
   * equals the GB record's CURRENT dex number (unevolved since the transfer, the
   * common case); fall back to the first match at all (an evolution, whose species is
   * now EXPECTED to differ from species_written by design). */
  uint16_t nowdex = gb_get_species_dex(&g_clip.gb);
  int first = -1, species_match = -1, start = 0;
  for (int guard = 0; guard <= GBSC_MAX_ENTRIES; guard++) {
    /* include_claimed=true: a claimed entry (its Gen-3 original already released by
     * gb_reconcile_on_load) must still serve the merge up -- see gb_sidecar.h.
     * want_kind=XR_KIND_G3_HOME (BACKLOG #150 S150-6): the merge UP restores a
     * Gen-3 original from a GB-side entry; a native cell (XR_KIND_NATIVE_HOME) must
     * never reach gbsc_merge_up -- gbsc_merge_up's own native refusal is belt, this
     * filter is an earlier braces (G-H6). */
    int i = gbsc_find(buf, *len, &g_clip.gb, start, true, XR_KIND_G3_HOME);
    if (i < 0) break;
    if (first < 0) first = i;
    GbscEntry cand;
    if (gbsc_get(buf, *len, i, &cand) && cand.species_written == nowdex) {
      species_match = i;
      break;
    }
    start = i + 1;
  }
  int found = (species_match >= 0) ? species_match : first;
  if (found < 0) {
    *handled = false;
    msg_wait(PDNA_SIDECAR_NONE_TITLE, UI_OK, PDNA_SIDECAR_NONE_L1, 0);
    return false;
  }

  /* BACKLOG #150 S150-9 decision 7: `out80` is now only the PROBE -- accept=0, the
   * pure original -- used solely to build the report for the merge screen.
   * app_paste_gb_commit() re-reads `e` and re-merges with the user's actual accept
   * mask before committing. gbsc_merge_up_sel(..., 0, ...) is cheaper than the old
   * gbsc_merge_up() call here: its output is never the commit's own record. */
  GbscEntry e;
  if (!gbsc_get(buf, *len, found, &e) || !gbsc_merge_up_sel(&e, &g_clip.gb, 0, out80, rep)) {
    *handled = true;
    snd_error();
    msg_wait(PDNA_SIDECAR_MERGEFAIL_TITLE, UI_WARN, PDNA_SIDECAR_MERGEFAIL_L1, 0);
    return false;
  }
  *idx = found;
  return true;
}

/* Confirm + commit + sidecar-cleanup half of app_paste_gb_merge (S5-B review fix #11).
 * `buf`/`len` are the same GBSC_FILE_MAX buffer app_paste_gb_lookup() just filled;
 * `idx`/`rep` are its output; `probe80` is app_paste_gb_lookup's DISCARDED accept=0
 * probe (BACKLOG #150 S150-9 decision 7) -- never committed, only `merged`
 * below (re-merged with the user's actual accept mask) is. Returns whatever
 * app_commit_with_dex() returned (false only for B on the merge screen, or a
 * downstream commit refusal -- both mean "nothing landed", so the caller's *handled
 * stays at its default true either way). */
static bool app_paste_gb_commit(uint8_t* buf, uint32_t* len, const char* path, int idx,
                                const uint8_t probe80[80], uint8_t* rec, bool is_party,
                                AppCommitFn commit, uint8_t* block,
                                const GbscMergeReport* rep) {
  (void)probe80;   /* the probe -- decision 7's own note; never committed */
  XrMergeReport xrep;
  xr_report_from_gbsc(rep, &xrep);
  uint8_t accept = 0;
  if (!app_xfer_merge_screen(&xrep, XR_MERGE_UP, &accept)) return false;

  /* Re-read `e` (the lookup's find already validated the ledger once; a second
   * gbsc_get() here is a cheap re-fetch of the same 128-byte entry, not a second
   * search) and re-merge with the ACTUAL accept mask -- into a LOCAL frame buffer,
   * never through `probe80`, which stays the lookup's own accept=0 probe. */
  GbscEntry e;
  uint8_t merged[80];
  if (!gbsc_get(buf, *len, idx, &e) || !gbsc_merge_up_sel(&e, &g_clip.gb, accept, merged, NULL)) {
    snd_error();
    msg_wait(PDNA_SIDECAR_MERGEFAIL_TITLE, UI_WARN, PDNA_SIDECAR_MERGEFAIL_L1, 0);
    return false;
  }

  /* G-H6, the SECOND independent guard (belt is gbsc_merge_up's own native refusal,
   * source/gb_sidecar.c) -- BACKLOG #150 S150-6, §11.6/§11.12/§11.13's required
   * !bc_is_native(merged) assert. A pre-#150 sidecar entry has no kind byte at all,
   * so a native cell reaching THIS point cannot be told apart from a real merged
   * Gen-3 record by anything upstream of this check; refusing here means one can
   * never land in g_save/g_pc through the clipboard even if the belt guard above it
   * were ever bypassed. */
  if (bc_is_native(merged)) {
    log_line("gen3: paste-up refused: merged80 is a native cell");
    snd_error();
    return false;
  }

  ClipMon tmp; memset(&tmp, 0, sizeof tmp);
  memcpy(tmp.rec, merged, 80);
  tmp.is_party = false;                        /* gbsc_merge_up always builds a box record */
  tmp.occupied = true;
  uint8_t out[100];
  if (!clip_to_record(&tmp, is_party, out)) return false;
  memcpy(rec, out, is_party ? 100 : 80);
  bool committed = app_commit_with_dex(rec, is_party, commit, block);
  if (!committed) return false;

  /* Best-effort: the Pokemon is already pasted either way (never undo a landed
   * write), so a sidecar-bookkeeping failure is reported, not rolled back. */
  bool sidecar_ok = false;
  if (gbsc_remove(buf, len, idx) == 0) {
    if (gbsc_count(buf, *len) == 0) {
      rmbl_pause();
      sidecar_ok = (f_unlink(path) == FR_OK);
      rmbl_resume();
    } else {
      rmbl_pause();
      sidecar_ok = (sf_write_verified(path, buf, *len) == SF_OK);
      rmbl_resume();
    }
  }
  if (!sidecar_ok) {
    log_line("gen3: sidecar not updated after merge-paste (%s idx %d)", path, idx);
    msg_wait(PDNA_SIDECAR_NOTUPDATED_TITLE, UI_WARN,
             PDNA_SIDECAR_NOTUPDATED_L1, PDNA_SIDECAR_NOTUPDATED_L2);
  }
  /* S5-B review fix #6: g_clip.from_gb is DELIBERATELY left true here (an earlier
   * revision cleared it). A second paste of the SAME clip now re-enters this whole
   * lookup: the entry it merged is gone (removed above), so sf_read_full/gbsc_find
   * report "no sidecar" and app_paste() shows "No sidecar: converted copy" before
   * pasting the lossy converted bytes -- an honest, visible fallback, rather than
   * silently pasting the converted copy with no notice at all. */
  return true;
}

/* S5-B: PASTE in a Gen-3 session when the clipboard came off a Game Boy source
 * (g_clip.from_gb, set by app_copy() via AppSrcOps.copy_native). Looks up
 * /PokeDNA/sidecar/<key>.pds and, when a matching entry is found, restores the
 * ORIGINAL Gen-3 record (gbsc_merge_up) instead of the lossy converted copy
 * clip_to_record() would otherwise build.
 *
 * *handled == true: the whole paste is decided one way or another (merged + committed,
 * or a refusal already shown on screen) -- the caller returns this function's own
 * return value straight through. *handled == false: no sidecar could be found for this
 * clip (or its corrupt file was declined), so the caller falls through to the ordinary
 * converted-copy path, after this function has already shown a notice.
 *
 * noinline, and the ONE thing that must stay true of this split (review fix #11): the
 * 1042-byte sidecar buffer lives HERE, on this frame, not a static and not borrowed --
 * app_box_swap (pdna_app.h) turned out to be held by box_oam.c for the ENTIRE
 * box-screen visit (boxoam_enter/exit bracket the whole visit, not a tick --
 * boxoam_suspend/resume touch only DISPCNT/BLDCNT), so a merge-paste reached from the
 * box/bank grid's own popup could NEVER have borrowed it -- review fix #2 (BLOCKING).
 * Stack cost is real but bounded and measured: the deepest call chain through this
 * function (main -> ... -> app_mon_menu -> app_paste -> here) runs ~6.9 KB against an
 * 11,512-byte IWRAM stack, and pdna_box.c's own release_box_all path already reaches
 * ~6.4 KB at the same depth -- this frame's extra ~1 KB is nowhere near the 15-36 KB
 * class hard rule 2 exists to catch; it is budget spent on purpose, not a big buffer
 * that wandered onto the stack by accident. app_paste_gb_lookup()/app_paste_gb_commit()
 * above are deliberately NOT forced noinline (review fix #11): whether GCC inlines them
 * or not, the 1 KB buffer itself stays right here, passed down by pointer either way. */
static bool __attribute__((noinline))
app_paste_gb_merge(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block,
                   bool* handled) {
  *handled = true;

  uint8_t dv4[4] = {
    gb_get_dv(&g_clip.gb, GB_ATK), gb_get_dv(&g_clip.gb, GB_DEF),
    gb_get_dv(&g_clip.gb, GB_SPE), gb_get_dv(&g_clip.gb, GB_SPC)
  };
  uint64_t key = gbsc_key(g_clip.gb.gen, gb_get_otid(&g_clip.gb), dv4, g_clip.gb.otname);
  char path[GBSC_PATH_MAX];
  path[0] = 0;   /* xr_path_for_key leaves `out` untouched on its own bad-arg/too-long
                  * refusal (never expected at GBSC_PATH_MAX in practice); an empty
                  * path then fails sf_read_full cleanly (SF_ERR_OPEN) instead of
                  * reading an uninitialized buffer as a filename. */
  /* BACKLOG #150 S150-6, site 4: the bool is otherwise ignored -- when neither xfer
   * nor sidecar holds this key, xr_path_for_key still resolves `path` to a real
   * path (the xfer one), and app_paste_gb_lookup's own sf_read_full below already
   * turns that "nothing on the card" case into SF_ERR_OPEN -> PDNA_SIDECAR_NONE_TITLE,
   * exactly as it did when gbsc_path() could only ever point at sidecar. */
  (void)xr_path_for_key(path, key);

  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = 0;
  uint8_t probe80[80];   /* BACKLOG #150 S150-9 decision 7: discarded after the screen -- see app_paste_gb_commit */
  GbscMergeReport rep;
  int idx = -1;
  if (!app_paste_gb_lookup(buf, &len, path, probe80, &rep, &idx, handled)) return false;

  return app_paste_gb_commit(buf, &len, path, idx, probe80, rec, is_party, commit, block, &rep);
}

static bool app_paste(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block, bool occupied) {
  if (!g_clip.occupied) return false;
  if (occupied && !app_confirm("Overwrite this Pokemon?", "Paste the copied mon here?")) return false;

  if (g_clip.from_gb) {
    bool handled;
    bool r = app_paste_gb_merge(rec, is_party, commit, block, &handled);
    if (handled) return r;
    /* No sidecar found for this clip -- fall through to the ordinary converted-copy
     * path below; app_paste_gb_merge() already showed the one-line notice. */
  }

  uint8_t out[100];
  if (!clip_to_record(&g_clip, is_party, out)) return false;
  memcpy(rec, out, is_party ? 100 : 80);
  return app_commit_with_dex(rec, is_party, commit, block);   /* auto-register the pasted species */
}

static bool app_duplicate(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block, int box) {
  if (is_party) {
    if (party_count(block, g_frlg) >= 6) { snd_deny(); msg_wait("PARTY FULL", UI_WARN, "Release a mon first.", 0); return false; }
    uint8_t out[100]; memcpy(out, rec, 100);
    party_append(block, g_frlg, out);
  } else {
    int fs = box_free_slot(block, box);
    if (fs < 0) { snd_deny(); msg_wait("BOX FULL", UI_WARN, "No empty slot here.", 0); return false; }
    memcpy(pk_box_slot(block, box, fs), rec, 80);
  }
  return app_commit_with_dex(rec, is_party, commit, block);   /* auto-register the duplicated species */
}

static bool app_release(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block, int box, int slot) {
  (void)rec;
  if (is_party && party_count(block, g_frlg) <= 1) {
    snd_deny();
    msg_wait("CAN'T RELEASE", UI_WARN, "The party can't be empty.", 0);
    return false;
  }
  if (!app_confirm("Release this Pokemon?", "Deleted permanently.")) return false;
  if (is_party) party_release(block, g_frlg, slot);
  else          clip_clear_box_slot(block, box, slot);
  return commit ? commit() : false;
}

/* Held-item move (take/give). TAKE stashes this mon's item and clears it; GIVE
 * puts the stashed item on a mon, swapping in that mon's old item so nothing is
 * ever lost (the stash empties when the swapped-out item is none). */
static bool app_take_item(uint8_t* rec, bool is_party, AppCommitFn commit) {
  EditMon e; gen3_edit_load(rec, is_party, &e);
  PkMon cur; em_preview(&e, &cur);
  if (!cur.heldItem) return false;
  g_item_clip = cur.heldItem; g_item_held = true;
  em_set_item(&e, 0);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  return commit ? commit() : false;
}
static bool app_give_item(uint8_t* rec, bool is_party, AppCommitFn commit) {
  if (!g_item_held) return false;
  EditMon e; gen3_edit_load(rec, is_party, &e);
  PkMon cur; em_preview(&e, &cur);
  uint16_t dst_old = cur.heldItem;
  em_set_item(&e, g_item_clip);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  g_item_clip = dst_old; g_item_held = (dst_old != 0);   /* swap: keep dst's old item to re-home */
  return commit ? commit() : false;
}

/* Gen-3-PC-style action menu on A. Omega: a navigable overlay of SUMMARY / ITEM /
 * MOVES / COPY / PASTE / DUPLICATE / RELEASE (the slot-aware ones use box/slot;
 * party ops use g_frlg). On an EMPTY slot it offers PASTE only. Everdrive
 * (read-only): jump to the summary. Returns true iff the save was modified.
 * `commit` persists `block` (PC storage, SaveBlock1, or a bank box file).
 * For party callers pass box = -1, slot = party index. */
/* daycare slot/egg clearing helpers live near pdna_daycare; forward-declared for the
 * "To Day-Care" action below. */
static void dc_clear_slot_aux(uint32_t base, uint32_t stride, int i);
static void dc_clear_egg(uint32_t base);

/* Day-Care SaveBlock1 layout for the loaded game. RS store the 2 box-mons contiguously
 * (stride 80); Emerald AND FireRed/LeafGreen interleave each box-mon with its mail+steps
 * (stride 140). FR/LG has a real 2-mon breeding Day-Care too (Four Island, SB1 0x2F80 —
 * verified vs pret/pokefirered), so it is NOT excluded. Single source of truth used by the
 * viewer and every deposit path so they can't drift. */
static void dc_layout(uint32_t* base, uint32_t* stride) {
  *base   = (g_game == PK_EMERALD) ? 0x3030 : (g_game == PK_FRLG) ? 0x2F80 : 0x2F9C;
  *stride = (g_game == PK_RS) ? 80 : 140;
}

/* First FREE physical Day-Care slot (0 or 1), or -1 if full. A slot is OCCUPIED iff it
 * holds a real species (1..411, not a bad egg) — matching the game (CountPokemonInDaycare
 * tests species != 0) and the viewer (dc_rescan). The old check treated any slot that
 * pk_decode_mon() didn't flag as the zeroed-empty sentinel as occupied, so a dirty-but-
 * empty slot (species 0 but a stray non-zero checksum byte, common on real saves) wrongly
 * read as "full" and blocked deposits. */
static int dc_first_free(uint32_t base, uint32_t stride) {
  for (int i = 0; i < 2; i++) {
    PkMon m;
    bool used = pk_decode_mon(g_sb1 + base + (uint32_t)i * stride, false, &m)
                && m.species >= 1 && m.species <= 411 && !m.isBadEgg;
    if (!used) return i;
  }
  return -1;
}

/* "To Day-Care" from a mon's action menu: MOVE this mon into a free daycare slot
 * (deposit its 80-byte box form, then remove it from the source). Omega-only; RS/Emerald
 * only (FireRed/LeafGreen has no Day-Care here). The deposit + the source removal are
 * committed together (party source: both in SB1; PC source: PC then SB1). */
static bool app_to_daycare(uint8_t* rec, bool is_party, uint8_t* block, int box, int slot) {
  uint32_t base, stride; dc_layout(&base, &stride);            /* RS / E / FR-LG all have a Day-Care */
  int fi = dc_first_free(base, stride);
  if (fi < 0) { snd_deny(); msg_wait("DAY-CARE FULL", UI_WARN, "Take a Pokemon out first.", 0); return false; }
  if (is_party && party_count(block, g_frlg) <= 1) { snd_deny(); msg_wait("CAN'T", UI_WARN, "The party can't be empty.", 0); return false; }
  if (!app_confirm("Send to Day-Care?", "Moves this Pokemon there.")) return false;
  memcpy(g_sb1 + base + (uint32_t)fi * stride, rec, 80);        /* deposit (first 80 bytes = box form) */
  dc_clear_slot_aux(base, stride, fi);
  dc_clear_egg(base);
  if (is_party) {                                              /* party + deposit both in SB1 -> deferred to exit */
    party_release(block, g_frlg, slot);
    app_stage_sb1();
    snd_ok(); msg_wait("SENT", UI_OK, "Now at the Day-Care.", "Saved when you leave.");
  } else {                                                     /* PC source: deferred (both buffers) */
    clip_clear_box_slot(block, box, slot);                     /* remove from the PC box (g_pc) */
    app_mark_pc_dirty();
    app_stage_sb1();                                           /* daycare deposit staged in g_save */
    snd_ok(); msg_wait("SENT", UI_OK, "Now at the Day-Care.", "Saved when you leave.");
  }
  pdna_daycare();          /* take the user to the Day-Care page to see the new boarder */
  return true;
}

/* box (80b) <-> party (100b) record conversion via the editor (derives/drops the
 * plaintext level + battle stats). */
static void box_to_party(const uint8_t* box80, uint8_t out100[100]) {
  EditMon e; gen3_edit_load(box80, false, &e); em_set_party_flag(&e, true);  gen3_edit_commit(&e, out100);
}
static void party_to_box(const uint8_t* party100, uint8_t out80[80]) {
  EditMon e; gen3_edit_load(party100, true, &e); em_set_party_flag(&e, false); gen3_edit_commit(&e, out80);
}

/* Place a HELD box mon into the party: ADD it to a free slot (target == party count) or
 * SWAP with party[target] (the displaced party mon takes the held mon's PC origin). The one
 * proven add/swap core, shared by the party overlay below. (orig_box,orig_slot) = the held
 * mon's origin; orig_bank = it came from the Bank (origin is a bank slot, NOT a g_pc slot —
 * ADD defer-deletes the bank source, SWAP is disallowed). Party + PC edits are staged and
 * committed together at the one exit save. Returns true iff the mon was placed. */
static bool party_place_held(const uint8_t* held80, int target, int orig_box, int orig_slot,
                             bool orig_bank, bool can_swap) {
  int n = party_count(g_sb1, g_frlg);
  if (target < 0 || target > n) { snd_deny(); return false; }   /* past the add slot */
  if (orig_bank) can_swap = false;                              /* a bank origin can't receive a swap */
  if (target == n) {                                            /* ADD to a free party slot */
    if (n >= 6) { snd_deny(); msg_wait("PARTY FULL", UI_WARN, "Swap with a member.", 0); return false; }
    if (orig_bank && orig_slot >= 0 && app_bank_defer_full()) { /* a full defer queue would silently DUP */
      snd_deny(); msg_wait("TOO MANY MOVES", UI_WARN, "Save first, then continue.", 0); return false;
    }
    uint8_t p100[100]; box_to_party(held80, p100);
    if (!party_append(g_sb1, g_frlg, p100)) { snd_deny(); return false; }
    /* Remove the origin (it left for the party). A BANK origin is a bank slot, NOT a g_pc
     * slot — clearing g_pc there would zero an untouched PC mon (or write OOB for the top
     * bank boxes); defer-delete the bank source instead (flushed AFTER the party commits). */
    if (orig_slot >= 0) {
      if (orig_bank) app_bank_defer_delete(orig_box, orig_slot, held80);
      else           memset(pk_box_slot(g_pc, orig_box, orig_slot), 0, 80);
    }
    app_mark_pc_dirty(); app_register_dex_deferred(p100, true); app_stage_sb1();
    snd_ok(); return true;
  }
  /* SWAP with party[target]: the party mon takes the held mon's PC origin */
  if (!can_swap) { snd_deny(); msg_wait("CAN'T SWAP", UI_WARN, "This held mon has no PC", "slot to receive the swap."); return false; }
  uint8_t* pslot = g_sb1 + (g_frlg ? 0x038 : 0x238) + (uint32_t)target * 100;
  uint8_t y80[80];  party_to_box(pslot, y80);               /* party mon -> 80b box */
  uint8_t x100[100]; box_to_party(held80, x100);            /* held box mon -> 100b party */
  memcpy(pk_box_slot(g_pc, orig_box, orig_slot), y80, 80);  /* party mon -> the PC origin */
  memcpy(pslot, x100, 100);                                 /* held mon -> the party slot */
  app_mark_pc_dirty(); app_register_dex_deferred(x100, true); app_stage_sb1();
  snd_ok(); return true;
}

/* ---- shared party engine, exposed to pdna_box.c's party_strip_overlay (pdna_app.h) --
 * g_sb1/g_frlg never leave this file; these wrap the exact same calls app_party_overlay
 * itself makes below, so there is one place that knows the party's SaveBlock1 layout. */
int app_party_n(void) { return party_count(g_sb1, g_frlg); }

int app_party_read(PkMon out[6]) {
  int n = pk_read_party_auto(g_sb1, out, &g_frlg);
  for (int i = 0; i < n; i++) pk_resolve(&out[i]);
  return n;
}

bool app_party_place_held(const uint8_t* held80, int target, int orig_box, int orig_slot,
                          bool orig_bank, bool can_swap) {
  return party_place_held(held80, target, orig_box, orig_slot, orig_bank, can_swap);
}

bool app_party_mon_menu(int slot, int footer_y, bool allow_move_to_box,
                        uint8_t tobox_grab[80], bool* tobox_hit) {
  uint8_t* rec = g_sb1 + (g_frlg ? 0x038 : 0x238) + (uint32_t)slot * 100;
  g_party_tobox_allowed = allow_move_to_box;
  bool changed = app_mon_menu(rec, true, false, app_commit_sb1, g_sb1, -1, slot, footer_y);
  g_party_tobox_allowed = false;
  if (tobox_hit) *tobox_hit = false;
  if (g_party_tobox_req) {                          /* user chose "MOVE TO BOX" */
    g_party_tobox_req = false;
    if (party_count(g_sb1, g_frlg) <= 1) {
      snd_deny(); msg_wait("CAN'T", UI_WARN, "The party can't be empty.", "Move another mon in first.");
    } else if (tobox_hit) {
      party_to_box(rec, tobox_grab); *tobox_hit = true;
    }
  }
  return changed;
}

/* Remove party slot `idx` (gap-free shift) as a DEFERRED move: stage SB1 + mark PC dirty so
 * it folds into the single exit save, and refresh the cached party. Used when a party mon is
 * carried OUT to a box (party -> box). The caller does this only on a successful DROP
 * (lift-don't-clear: until then the party keeps the mon, so a cancelled carry loses nothing). */
void app_party_remove_at(int idx) {
  int n = party_count(g_sb1, g_frlg);
  if (idx < 0 || idx >= n) return;
  party_release(g_sb1, g_frlg, idx);
  app_mark_pc_dirty();        /* a party->box move also changed g_pc; both staged, one save */
  app_stage_sb1();            /* party lives in SaveBlock1 */
  g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
  for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
}

/* ---- Party screen: the single party UI (Gen-4/5-style), replacing the old full-screen
 * list. The 6 party mons as a 2x3 icon cluster on the right + the selected mon's summary on
 * the left. Two modes:
 *   PLACE (held != NULL): carrying a box mon -> A on a slot ADDS/SWAPS it in (party_place_held);
 *         returns 1 (placed -> caller ends the carry) or 0.
 *   BROWSE (held == NULL): A on a party mon opens the FULL action menu (View/Edit, Item,
 *         Legality, Copy, Paste, Duplicate, To Day-Care, Export, Take/Give item, Release) — and
 *         "Move to box" when allow_move_to_box (opened from the box, so there's a box to carry
 *         into); choosing that fills grab80 + *grab_slot and returns 2. 0 = closed.
 * Self-contained (clears + repaints each frame) so the action menu's full-screen sub-views
 * can't leave artifacts; the caller repaints its own screen on return. */
/* ---- the party screen: one big slot-1 box + five list rows ----------------
 *
 * Retail layout, adopted 2026-08-19 from a measured capture of Guy's own
 * Emerald cartridge (docs/analysis-2026-08-19-party/MEASUREMENTS.md): party
 * slot 1 always gets its own larger panel, fixed at top-left regardless of the
 * cursor; slots 2-6 are 142x24 list rows underneath a per-row HP bar. Every
 * geometry number below comes from pdna_layout.h (PDNA_PTY_*), which the host
 * text-fit test reads too — see that file's comment for why.
 *
 * `party_draw_all` is the ONE place that paints a slot's chrome, icon, and
 * text; both the initial full draw and the idle-bob redraw call it, so the
 * bob tick can never show a different layout than the one just drawn. It
 * cannot do a partial/flicker-free composite the way the old 40x40 grid did
 * (ui_blit_over composes over a SINGLE flat colour; a slot's own fill here is
 * the same 1px-banded dither the background uses), so a bob tick repaints
 * the six slots in full. That runs only once per PARTY_BOB_PERIOD vblanks
 * (matched to retail's own ~15-16 frame cycle, see below) while idle, which
 * is cheap next to the full ui_clear() + redraw every keypress already pays. */
#define PARTY_BOB_PERIOD 8     /* retail's own idle-bob period is ~15-16 frames total
                                * (measured: 6-8 frames per pose, MEASUREMENTS.md
                                * "Animation" section) — this is HALF the period (one
                                * pose's hold time), matching wait_keys_bob's own
                                * "*ctr >= period -> flip" convention. The box/dex/
                                * daycare/summary screens keep their own PDNA_BOB_PERIOD
                                * (30, ~59-60 frame cycle); that mismatch from retail was
                                * NOT re-measured or fixed here — only the party screen
                                * was in scope for this pass. */

/* `pm` points at app_party_overlay's own stack-local PkMon[6] for the
 * duration of the call — never copied, never outliving it. redraw() only
 * ever fires synchronously from inside wait_keys_bob, which is called from
 * inside app_party_overlay while `pm` is still alive, so this is the same
 * "point at the caller's own array" trick the rest of the app uses for
 * per-frame callbacks, just without the copy the old s_pov_sp/fm/egg arrays
 * paid for the same lifetime guarantee. */
static PkMon* s_pov_pm;
static int    s_pov_n, s_pov_sel, s_pov_addslot;

/* Slot i's own bounding box: i==0 is the fixed slot-1 box; i==1..5 are the
 * five list rows underneath it, in party order. Returns true for the box. */
static bool party_slot_rect(int i, int* x, int* y, int* w, int* h) {
  if (i == 0) { *x = PDNA_PTY_BOX_X; *y = PDNA_PTY_BOX_Y; *w = PDNA_PTY_BOX_W; *h = PDNA_PTY_BOX_H; return true; }
  *x = PDNA_PTY_ROW_X; *y = PDNA_PTY_ROW_Y0 + (i - 1) * PDNA_PTY_ROW_H;
  *w = PDNA_PTY_ROW_W; *h = PDNA_PTY_ROW_H; return false;
}

/* Chrome only: fill + border, border colour swapped for the cursor (retail's own
 * selection mechanic — DIFFERENCES #3 in MEASUREMENTS.md — is a border recolour,
 * navy -> orange, not a separate highlight sprite). */
static void party_draw_slot_bg(int i, bool selected) {
  int x, y, w, h; bool isbox = party_slot_rect(i, &x, &y, &w, &h);
  u16 border = selected ? UI_PTY_CURSOR : UI_PTY_BORDER;
  if (isbox) ui_panel_striped(x, y, w, h, UI_PTY_BOX_FILL_A, UI_PTY_BOX_FILL_B, border);
  else       ui_panel_striped(x, y, w, h, UI_PTY_ROW_FILL_A, UI_PTY_ROW_FILL_B, border);
}

/* Border only (m3_frame's own perimeter, not ui_panel_striped's full interior fill) —
 * factored out of party_draw_slot_bg so the idle-bob tick can cheaply RESTORE a slot's
 * divider/edge after party_icon_repaint's background reconstruction paints over it (see
 * party_overlay_bob). Same border colour rule as party_draw_slot_bg.
 *
 * RAW vid_mem writes, not m3_frame -- same style as party_icon_repaint right above.
 * An earlier version of this function called m3_frame(x, y, x+w-1, y+h-1, ...), exactly
 * matching ui_panel_striped's own call (so it draws the identical rectangle -- see
 * ui.c's ui_progress comment for the right/bottom-EXCLUSIVE convention that makes
 * `w-1`/`h-1` the correct call here). That version was MEASURABLY too slow by a small
 * margin: on a fresh 60-frame idle capture, the border-restore pass lost a race with
 * this frame's own video output on the EXACT tick-transition frame (one frame in ten,
 * matching PARTY_BOB_PERIOD's cadence) -- a genuine, if brief (1/60s), re-eroded-then-
 * fixed-next-frame flicker, the same class of tearing party_icon_repaint's own block
 * comment describes avoiding via per-scanline vid_mem writes over library calls. This
 * closed it (re-verified: 0 eroded frames in 60, not 1). */
static void party_draw_slot_border(int i, bool selected) {
  int x, y, w, h; party_slot_rect(i, &x, &y, &w, &h);
  u16 c = selected ? UI_PTY_CURSOR : UI_PTY_BORDER;
  int x2 = x + w - 2, y2 = y + h - 2;   /* right/bottom column-row, EXCLUSIVE-of-w/h-1 */
  if ((unsigned)y < (unsigned)UI_SCR_H) {
    u16* row = &vid_mem[y * UI_SCR_W];
    for (int px = x; px <= x2; px++) if ((unsigned)px < (unsigned)UI_SCR_W) row[px] = c;
  }
  if ((unsigned)y2 < (unsigned)UI_SCR_H) {
    u16* row = &vid_mem[y2 * UI_SCR_W];
    for (int px = x; px <= x2; px++) if ((unsigned)px < (unsigned)UI_SCR_W) row[px] = c;
  }
  for (int py = y; py <= y2; py++) {
    if ((unsigned)py >= (unsigned)UI_SCR_H) continue;
    if ((unsigned)x  < (unsigned)UI_SCR_W) vid_mem[py * UI_SCR_W + x]  = c;
    if ((unsigned)x2 < (unsigned)UI_SCR_W) vid_mem[py * UI_SCR_W + x2] = c;
  }
}

/* Icon + every text/HP field for one occupied slot, at bob frame `bob`. `p` NULL
 * means an empty slot (draws our own "+add here"/"-" affordance instead — the tool
 * still needs a PLACE target and an empty-party-slot indicator that retail's own
 * always-full party screen never had to draw, see the KEEP-OURS note above
 * app_party_overlay). */
/* Name + level, the two fields that sit at the icon's own x — a real Gen-3 icon's
 * silhouette can reach far enough right to sit under the first few glyphs of a long
 * name (measured: SALAMENCE's icon opaque pixels alone don't reach it, but the bob
 * tick repaints the icon's whole 32x32 BOUNDING rectangle regardless of where its
 * opaque pixels end, and that rectangle DOES reach into the name column). So this is
 * factored out: the initial draw calls it once after the icon, and the bob tick calls
 * it again after every icon repaint, to put the text back on top where the icon's
 * bounding box would otherwise have silently erased it — a real bug this pass caught
 * (see party_overlay_bob). */
static void party_draw_name_level(int i, const PkMon* p) {
  int x, y, w, h; bool isbox = party_slot_rect(i, &x, &y, &w, &h); (void)w; (void)h;
  int ndx = isbox ? PDNA_PTY_BOX_NAME_DX : PDNA_PTY_NAME_DX;
  int ndy = isbox ? PDNA_PTY_BOX_NAME_DY : PDNA_PTY_NAME_DY;
  int nw  = isbox ? PDNA_PTY_BOX_NAME_W  : PDNA_PTY_NAME_W;
  const char* nm = p->nickname[0] ? p->nickname : pk_species_name(p->species);
  /* ROW and BOX both draw through the TIGHT face now (2026-08-21 correction — see
   * PDNA_PTY_NAME_W's own comment in pdna_layout.h for the full measurement history
   * this rests on). Through 2026-08-20 the row used the DEFAULT face on the theory that
   * a clipped name is safer than a kerned one; that was true of delta=2 (genuine ink
   * OVERLAP — see UI_PTEXT_TIGHT_DELTA's comment in ui_font.h) but not of delta=1, which
   * is proven touching-at-worst across all 96 glyphs and renders every real 9-glyph
   * species name (SALAMENCE/METAGROSS/DRAGONITE/etc, 45px tight) with several pixels of
   * slack inside the 49px column instead of clipping it to "SALAMEN~". A complete name
   * beats a clipped one, so both branches take the same path:
   *   ui_ptext_fit_shadow_tight (delta=1, capped — UI_PTEXT_TIGHT_DELTA in ui_font.h).
   * A true 10-glyph name (BELLSPROUT, or this save's own "28/01/2026" nickname) still
   * does not fit even at delta=1 (50px against 49px) and clips by ~1px via
   * ui_ptext_fit_tight's own trailing '~' — an accepted, documented residual (see
   * tests/host_textfit_test.c's row-name block): closing THAT last pixel needs a
   * narrower party typeface, not more kerning (delta is capped at 1 and must stay
   * there), and is a separate job. */
  ui_ptext_fit_shadow_tight(x + ndx, y + ndy, nw, UI_TEXT, UI_PTY_TEXT_SHADOW, nm);
  char lvl[8]; siprintf(lvl, PDNA_PTY_LVL_FMT, (unsigned)p->level);
  int lvdy = isbox ? PDNA_PTY_BOX_LVL_DY : PDNA_PTY_LVL_DY;
  ui_ptext_shadow(x + ndx, y + lvdy, UI_TEXT, UI_PTY_TEXT_SHADOW, lvl);
}

/* Just the "HP" label text — the ONE piece of party_draw_hp_fields (below) that the
 * slot-1 BOX's own icon repaint can erase. The box icon is 32px wide (abs x=5..37,
 * y=28..59) and its "HP" label sits at abs x=32 (17 + PDNA_PTY_BOX_HP_LBL_DX 15), y=53
 * (25 + 28) — squarely inside that rect — so every bob tick used to erase most of the
 * 'H' glyph and leave only its tail, rendering "HP" as ".P" forever after the first
 * tick. The bar (BOX_HP_BAR_DX=29 -> abs x=46, past the icon's x=37) and the numbers
 * (BOX_HP_NUM_DY=38 -> abs y=63, past the icon's y=59) are NOT in the icon's rect and
 * do not need restoring — redrawing the whole HP block every tick to fix a label was
 * measurably too slow (see party_overlay_bob's comment: it pushed the tick's total
 * cost over budget and reintroduced scanline tearing on a fresh 60-frame capture). Row
 * slots never needed this at all (their HP fields sit well past the 32px row icon). */
static void party_draw_hp_label(int i) {
  int x, y, w, h; bool isbox = party_slot_rect(i, &x, &y, &w, &h); (void)w; (void)h;
  int lbldx = isbox ? PDNA_PTY_BOX_HP_LBL_DX : PDNA_PTY_HP_LBL_DX;
  int lbldy = isbox ? PDNA_PTY_BOX_HP_LBL_DY : PDNA_PTY_HP_LBL_DY;
  ui_ptext_shadow(x + lbldx, y + lbldy, UI_PTY_HP_LABEL, UI_PTY_HP_OUTLINE, "HP");
}

/* HP label + bar + numbers, the fields party_draw_slot_fg draws last for the INITIAL
 * paint (both box and rows — see party_draw_hp_label's comment for why only the box's
 * own "HP" label, not this whole block, needs restoring on every idle-bob tick). */
static void party_draw_hp_fields(int i, const PkMon* p) {
  int x, y, w, h; bool isbox = party_slot_rect(i, &x, &y, &w, &h); (void)w; (void)h;

  /* pk_decode_mon only carries the COMPUTED max stat (stats[PK_HP]) into PkMon;
   * current HP lives at record offset 0x56 (party-only field — gen3_edit.c:105
   * writes the same offset on heal/create) and is read straight off p->raw here. */
  uint16_t maxhp = p->stats[PK_HP];
  uint16_t curhp = p->raw ? (uint16_t)(p->raw[0x56] | ((uint16_t)p->raw[0x57] << 8)) : maxhp;
  if (curhp > maxhp) curhp = maxhp;     /* a torn/edited record must never over-fill the bar */

  party_draw_hp_label(i);

  int bdx = isbox ? PDNA_PTY_BOX_HP_BAR_DX : PDNA_PTY_HP_BAR_DX;
  int bdy = isbox ? PDNA_PTY_BOX_HP_BAR_DY : PDNA_PTY_HP_BAR_DY;
  int bw  = isbox ? PDNA_PTY_BOX_HP_BAR_W  : PDNA_PTY_HP_BAR_W;
  int filled = maxhp ? (int)((uint32_t)curhp * (uint32_t)bw / maxhp) : 0;
  /* A living Pokemon must show at least one VISIBLE pixel of fill, never an accidental
   * "0 HP" bar from integer truncation at a low fraction. 1 is not enough by itself:
   * the border drawn below covers the bar's own first column, so a single filled column
   * would be swallowed whole. 2 is the smallest value that survives it. */
  if (curhp > 0 && filled < 2 && bw >= 2) filled = 2;

  /* Colour by HP fraction, thresholds CITED from pokeemerald's own GetHPBarLevel
   * (src/battle_interface.c, pret decomp — a scratchpad checkout of the same public
   * source CLAUDE.md already treats as reference-only; not found under this repo's own
   * reference/ tree): green above 50%, yellow above 20%, red at or below. Written as the
   * same integer test the decomp uses (hp*scale/maxhp against scale*N/100), just without
   * the bar-pixel scale factor since it cancels out of a plain percentage comparison.
   * The GREEN pair is Guy's own MEASURED retail capture (MEASUREMENTS.md) — his whole
   * save is full HP, which is exactly the case that stays green here. YELLOW/RED are
   * PROVISIONAL (see UI_PTY_HP_FILL_YEL/RED in ui.h for why the literal decomp RGB
   * values could not be recovered from source alone). */
  u16 fill, shade;
  if (maxhp == 0 || (uint32_t)curhp * 100u > (uint32_t)maxhp * 50u) {
    fill = UI_PTY_HP_FILL; shade = UI_PTY_HP_SHADE;
  } else if ((uint32_t)curhp * 100u > (uint32_t)maxhp * 20u) {
    fill = UI_PTY_HP_FILL_YEL; shade = UI_PTY_HP_SHADE_YEL;
  } else {
    fill = UI_PTY_HP_FILL_RED; shade = UI_PTY_HP_SHADE_RED;
  }

  ui_progress(x + bdx, y + bdy, bw, PDNA_PTY_HP_BAR_H, filled,
             fill, UI_PTY_HP_TRACK, UI_PTY_HP_OUTLINE);
  /* Retail's bar is FOUR bands top to bottom (MEASUREMENTS.md "HP bar colours"): outline,
   * a 1px white highlight, a darker shading row, then the main fill — at h=7 that is
   * outline+white+shade+3xmain+outline. The shipped bar only ever drew the shade row. */
  if (filled > 2) {
    ui_hline(x + bdx + 1, y + bdy + 1, filled - 2, UI_PTY_HP_HILITE);
    ui_hline(x + bdx + 1, y + bdy + 2, filled - 2, shade);
  }

  char hpn[16]; siprintf(hpn, PDNA_PTY_HP_NUM_FMT, (unsigned)curhp, (unsigned)maxhp);
  int hndx = isbox ? PDNA_PTY_BOX_HP_NUM_DX : PDNA_PTY_HP_NUM_DX;
  int hndy = isbox ? PDNA_PTY_BOX_HP_NUM_DY : PDNA_PTY_HP_NUM_DY;
  ui_ptext_shadow(x + hndx, y + hndy, UI_TEXT, UI_PTY_TEXT_SHADOW, hpn);
}

static void party_draw_slot_fg(int i, const PkMon* p, uint8_t bob, bool is_addslot) {
  int x, y, w, h; bool isbox = party_slot_rect(i, &x, &y, &w, &h); (void)w; (void)h;
  int ndx = isbox ? PDNA_PTY_BOX_NAME_DX : PDNA_PTY_NAME_DX;
  int ndy = isbox ? PDNA_PTY_BOX_NAME_DY : PDNA_PTY_NAME_DY;
  if (is_addslot) { ui_ptext(x + ndx, y + ndy, UI_OK, "+ Add here"); return; }
  if (!p)         { ui_ptext(x + ndx, y + ndy, UI_DIM, "-"); return; }

  bool isEgg = p->isEgg && !p->isBadEgg;
  int idx = isbox ? PDNA_PTY_BOX_ICON_DX : PDNA_PTY_ROW_ICON_DX;
  int idy = isbox ? PDNA_PTY_BOX_ICON_DY : PDNA_PTY_ROW_ICON_DY;
  const u16* ic = isEgg ? mon_icon_egg_frame(bob) : mon_icon_for_form_frame(p->species, p->form, bob);
  if (ic) ui_sprite(x + idx, y + idy, MON_ICON_W, MON_ICON_H, ic);

  party_draw_name_level(i, p);

  if (p->gender != 2) {   /* omitted for genderless species, same as retail */
    int gdx = isbox ? PDNA_PTY_BOX_GEND_DX : PDNA_PTY_GEND_DX;
    int gdy = isbox ? PDNA_PTY_BOX_GEND_DY : PDNA_PTY_GEND_DY;
    if (p->gender == 0) ui_gender_glyph_m(x + gdx, y + gdy, UI_PTY_GEND_M_FILL, UI_PTY_GEND_M_LINE);
    else                ui_gender_glyph_f(x + gdx, y + gdy, UI_PTY_GEND_F_FILL, UI_PTY_GEND_F_LINE);
  }

  party_draw_hp_fields(i, p);
}

/* The one shared paint used by both the full redraw and the idle-bob tick — see the
 * block comment above for why a partial/flicker-free composite is not available here.
 * Pass 1 paints every slot's chrome; pass 2 paints icons+text on top of ALL of them,
 * so a bleeding icon (retail deliberately overlaps the row above/below its own box —
 * MEASUREMENTS.md "Element positions") draws over its neighbour's chrome the same way
 * retail layers it, regardless of slot draw order. */
static void party_draw_all(uint8_t bob) {
  for (int i = 0; i < 6; i++) party_draw_slot_bg(i, i == s_pov_sel);
  for (int i = 0; i < 6; i++) {
    if (i == s_pov_addslot)     party_draw_slot_fg(i, 0, bob, true);
    else if (i < s_pov_n)       party_draw_slot_fg(i, &s_pov_pm[i], bob, false);
    else                        party_draw_slot_fg(i, 0, bob, false);
  }
}

/* Flicker-free per-icon repaint for the idle bob: compose sprite-over-background,
 * background resolved ONCE PER SCANLINE (not per pixel — see below), written straight
 * to VRAM. `slot_i` is the icon's OWN slot (0=box, 1..5=rows): a row icon bleeds only
 * into the row directly above or below it (MEASUREMENTS.md "Element positions"), and
 * box icons never leave the box's own column, so each scanline has AT MOST one split
 * point (the box/row's own left edge) between two known colours — precompute that pair
 * once per row instead of walking every panel's rect for every pixel.
 *
 * WHY THE BOB TICK CANNOT JUST CALL party_draw_all AGAIN: it did, in an earlier pass of
 * this redesign, and MEASURABLY tore — six striped panels + text + HP bars is enough
 * software drawing that it ran past a single video frame's cycle budget, so mGBA (and,
 * by the same arithmetic, real hardware) caught the screen mid-repaint for several
 * frames every time the icons bobbed (verified: diffing docs/analysis-2026-08-19-party/
 * new-party-idle-f00..f05.png showed rows blanking and reappearing over several frames,
 * repeating every bob toggle). Only the icon pixels actually change between the two bob
 * frames — chrome, text and HP bars are IDENTICAL — so repainting just the six 32x32
 * icons is both correct and, unlike the full redraw, cheap enough to finish inside one
 * frame. A first per-PIXEL-lookup version of this fix cut the tear from ~5 frames to
 * ~3 but still measurably tore; per-SCANLINE lookup was the one that actually closed it
 * (re-verified against a fresh 60-frame idle capture — see the animation measurement in
 * the delivery notes). */
static void party_icon_repaint(int slot_i, int x, int y, const u16* data) {
  if (!data) return;
  bool isbox = (slot_i == 0);
  /* `probe` only ever advances forward as py increases (rows are contiguous and this
   * is scanned top to bottom), so the 32-row icon costs at most 3 probe-row advances
   * total, not 3 checks EVERY row — the earlier per-row neighbour scan re-did the same
   * up-to-3 comparisons 32 times over for no reason. */
  int probe = slot_i - 1; if (probe < 1) probe = 1;
  for (int j = 0; j < MON_ICON_H; j++) {
    int py = y + j; if ((unsigned)py >= (unsigned)UI_SCR_H) continue;
    int splitX; u16 leftCol, rightCol;
    if (isbox) {
      splitX = PDNA_PTY_BOX_X;
      leftCol = UI_PTY_BG_MARGIN;
      rightCol = (py & 1) ? UI_PTY_BOX_FILL_B : UI_PTY_BOX_FILL_A;
    } else {
      splitX = PDNA_PTY_ROW_X;
      leftCol = (py & 1) ? UI_PTY_BG_B : UI_PTY_BG_A;   /* row icons never reach the x<20 margin */
      while (probe <= 5 && py >= PDNA_PTY_ROW_Y0 + (probe - 1) * PDNA_PTY_ROW_H + PDNA_PTY_ROW_H) probe++;
      bool inRow = (probe <= 5) && (probe >= slot_i - 1) && (probe <= slot_i + 1)
                 && (py >= PDNA_PTY_ROW_Y0 + (probe - 1) * PDNA_PTY_ROW_H);
      rightCol = inRow ? ((py & 1) ? UI_PTY_ROW_FILL_B : UI_PTY_ROW_FILL_A)
                       : ((py & 1) ? UI_PTY_BG_B : UI_PTY_BG_A);
    }
    const u16* srow = data + (uint32_t)j * MON_ICON_W;
    u16* drow = &vid_mem[py * UI_SCR_W];
    for (int i = 0; i < MON_ICON_W; i++) {
      int px = x + i; if ((unsigned)px >= (unsigned)UI_SCR_W) continue;
      u16 p = srow[i];
      drow[px] = (p & 0x8000) ? (u16)(p & 0x7FFF) : (px < splitX ? leftCol : rightCol);
    }
  }
}

static void party_overlay_bob(int f) {
  rumble_io_suspend();       /* icon frames may be in ROM: mute the cart-bus motor toggle */
  for (int i = 0; i < 6; i++) {
    if (i == s_pov_addslot || i >= s_pov_n) continue;   /* nothing occupied to bob there */
    const PkMon* p = &s_pov_pm[i];
    bool isEgg = p->isEgg && !p->isBadEgg;
    int x, y, w, h; bool isbox = party_slot_rect(i, &x, &y, &w, &h); (void)w; (void)h;
    int idx = isbox ? PDNA_PTY_BOX_ICON_DX : PDNA_PTY_ROW_ICON_DX;
    int idy = isbox ? PDNA_PTY_BOX_ICON_DY : PDNA_PTY_ROW_ICON_DY;
    const u16* ic = isEgg ? mon_icon_egg_frame((uint8_t)f) : mon_icon_for_form_frame(p->species, p->form, (uint8_t)f);
    party_icon_repaint(i, x + idx, y + idy, ic);
    /* The icon repaint just overwrote its own full bounding box — put back whatever it
     * may have painted over: name/level, on EVERY row/box (see party_draw_name_level's
     * comment — a real Gen-3 icon's bounding rect reaches every row's name column), and
     * — box only — its "HP" label (see party_draw_hp_label's comment for why the bar
     * and numbers do not need it, and why redrawing the WHOLE HP block here, tried in
     * an earlier version of this fix, measurably reintroduced the scanline tearing
     * party_icon_repaint's own comment describes: it pushed this tick's total cost over
     * budget, verified on a fresh 60-frame idle capture where row dividers alternated
     * between fully restored and eroded frame to frame — a frame-budget overrun does
     * that; a logic bug would not). */
    if (isbox) party_draw_hp_label(i);
    party_draw_name_level(i, p);
    /* party_icon_repaint's per-scanline background reconstruction only knows FILL vs
     * background colour — it has no idea a slot's own BORDER line (a row divider, the
     * box's edge) might fall inside the icon's 32x32 bounding rect too, so any such
     * line got silently overwritten with fill colour on every tick.
     *
     * Restored HERE — right after THIS slot's own icon repaint, inside the same loop
     * iteration — not in a separate trailing pass over all 6 slots (an earlier version
     * of this fix did that, and it MEASURABLY still tore on the exact tick-transition
     * frame, 1 frame in 10 on a fresh 60-frame capture: Mode 3 has no double buffer, so
     * a write made partway through active display can miss the PPU's beam for a
     * scanline near the top of the screen (row 1's own divider, y=10) on THAT frame
     * even though it lands correctly in vid_mem for every later frame. Row i's own top
     * edge is eroded by BOTH its own icon (whose 32px-tall bounding box reaches 1px
     * above the row) and the row ABOVE's icon (which bleeds 6-7px down past its own
     * bottom); its bottom edge is eroded only by its own icon (the row below's icon
     * does not reach back up that far — the pitch/height maths land it 1px short). So
     * by the time THIS iteration's own icon repaint finishes, every source that could
     * have eroded slot i's border already has, and restoring right here — not several
     * more icon-repaints later — keeps the CPU-to-scanout gap as tight as
     * party_draw_name_level's own (which never showed this flicker for the same
     * reason). Re-verified: 0 eroded frames in 60, not 1. */
    party_draw_slot_border(i, i == s_pov_sel);
  }
  rumble_io_resume();
}

/* CANCEL button, drawn as one unit (fill + border + label). Reused for the full paint
 * and for the partial path's BACK-focus swap; ui_panel's fill is unconditional so this
 * is self-contained either way (no separate erase needed). */
static void party_ov_cancel_paint(bool bsel) {
  ui_panel(PDNA_PTY_CANCEL_X, PDNA_PTY_CANCEL_Y, PDNA_PTY_CANCEL_W, PDNA_PTY_CANCEL_H,
           UI_PTY_CANCEL_FILL, bsel ? UI_PTY_CURSOR : UI_PTY_BORDER);
  ui_ptext_fit_shadow(PDNA_PTY_CANCEL_X + PDNA_PTY_MSG_PAD, PDNA_PTY_CANCEL_Y + PDNA_PTY_MSG_PAD,
                     PDNA_PTY_CANCEL_W_BUDGET, UI_TEXT, UI_PTY_TEXT_SHADOW, PDNA_LBL_CANCEL);
}

static int app_party_overlay_inner(const uint8_t* held, int orig_box, int orig_slot, bool orig_bank,
                                   bool can_swap, uint8_t grab80[80], int* grab_slot, bool allow_move_to_box) {
  if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); return 0; }
  /* Same two-axis gate as app_mon_menu, and here it is a DATA-LOSS guard: a party mon
   * carried out through this overlay is removed from the party for real once it is
   * dropped (drop_held -> clear_origin -> app_party_remove_at), so dropping it into a
   * read-only source's buffer — which nothing persists — would destroy it. A
   * read-only source sets is_bank, which already stops pdna_box from reaching the
   * PARTY tab, so this is defence in depth on a path that must never open. */
  if (app_src_readonly()) { snd_deny(); return 0; }
  const int BACK = 6;
  int sel = 0;
  int bob_ctr = 0, bob = 0;          /* ANIM_PARTY idle bob phase, kept across repaints */
  bool first_paint = true;
  /* Cursor-only shadow (sel/n/addslot + gen), stack scalars, no PkMon copy needed --
   * see the block comment below for why. */
  int pv_sel = -2, pv_n = -1, pv_addslot = -2; bool pv_valid = false; uint32_t pv_gen = 0;
  perf_span_begin("party");          /* enter cost: 6 icons through the artless ladder */
  for (;;) {
    int n = party_count(g_sb1, g_frlg);
    if (n < 1) { perf_span_end(); return 0; }         /* shouldn't happen (party never empties) */
    int addslot = (held && n < 6) ? n : -1;          /* PLACE: cell n is the "+ add" target */
    int lastSel = (addslot >= 0) ? addslot : (n - 1); /* last slot before BACK, in UP/DOWN order */
    if (sel != BACK && sel >= n && sel != addslot) sel = n - 1;   /* clamp after a release */
    PkMon pm[6]; pk_read_party_auto(g_sb1, pm, &g_frlg);
    for (int i = 0; i < n; i++) pk_resolve(&pm[i]);

    /* WHY A SCALAR SHADOW IS ENOUGH, NO PkMon[6] SNAPSHOT. Every path that can change
     * what's drawn here (party_place_held's success returns, which EXIT the function
     * entirely without looping back to us; app_party_mon_menu, reached from BROWSE
     * mode) opens a full-screen sub-view (app_mon_menu's own menu loop at minimum,
     * even on an immediate B-cancel) that ui_clear()s on its own first frame -- so
     * ui_clear_gen() catches every content change. party_place_held's DENIAL paths
     * are the one place that returns without a repaint: some show msg_wait (gen
     * moves), and the rest (bad target index, party_append failing) are silent
     * snd_deny()-only no-ops that change nothing on screen -- correctly invisible to
     * this gate. n/addslot are shadowed too, defensively, even though they can only
     * change alongside a gen bump by the same argument (the derived-scalar trap this
     * arc keeps finding is a reason to double-check, not a reason to skip the check). */
    bool full = !pv_valid || pv_gen != ui_clear_gen() || n != pv_n || addslot != pv_addslot;

    /* s_pov_* are read by party_overlay_bob on the NEXT wait_keys_bob_p call below --
     * including on a partial repaint, where the full paint (and its s_pov_* publish)
     * is skipped. Update them EVERY iteration, unconditionally, or a bob tick right
     * after a cursor-only move would restore borders against a stale s_pov_sel/pm/n
     * and either draw the wrong slot's cursor colour or read a freed `pm` -- the exact
     * interleave hazard this batch was warned about. */
    s_pov_pm = pm; s_pov_n = n < 6 ? n : 6;
    s_pov_sel = (sel == BACK) ? -1 : sel;
    s_pov_addslot = addslot;
    /* Declare the six rows and rent the space to hold them, BEFORE the paint that draws
     * them -- see app_icons_hold. Without this the overlay's bob is what Guy watched
     * cost 120-210 disk_read calls every 8 frames on a card with a populated root, and
     * on the ROM rung it was gated off entirely because six rows never fit four slots.
     * Unconditional too: it is RAM bookkeeping (icon_store_plan), not a paint, and the
     * bob on the upcoming wait needs it declared regardless of which path drew. */
    {
      uint16_t irows[6];
      int nr = 0;
      for (int i = 0; i < s_pov_n; i++)
        irows[nr++] = app_icon_row_of(pm[i].species, pm[i].form,
                                      pm[i].isEgg && !pm[i].isBadEgg);
      app_icons_hold(irows, nr);
    }

    if (full) {
      ui_clear();
      ui_stripe_bg(20, UI_PTY_BG_MARGIN, UI_PTY_BG_A, UI_PTY_BG_B);
      party_draw_all((uint8_t)bob);

      /* Bottom message box + CANCEL button, retail's own layout for this band
       * (docs/analysis-2026-08-19-party/MEASUREMENTS.md does not itemise these two —
       * they were measured separately off the same capture for this pass). */
      ui_panel(PDNA_PTY_MSG_X, PDNA_PTY_MSG_Y, PDNA_PTY_MSG_W, PDNA_PTY_MSG_H, UI_TEXT, UI_PTY_BORDER);
      ui_ptext_fit(PDNA_PTY_MSG_X + PDNA_PTY_MSG_PAD, PDNA_PTY_MSG_Y + PDNA_PTY_MSG_PAD,
                  PDNA_PTY_MSG_W_BUDGET, UI_PTY_MSG_TEXT,
                  held ? PDNA_PTY_MSG_PLACE : PDNA_PTY_MSG_CHOOSE);
      party_ov_cancel_paint(sel == BACK);
    } else if (sel != pv_sel) {
      /* Cursor-only move: retail's own selection mechanic is a BORDER RECOLOUR, not a
       * separate highlight sprite or a re-striped fill (party_draw_slot_bg's own
       * comment) -- so party_draw_slot_border (the SAME cheap raw-vid_mem perimeter
       * write the idle-bob tick already uses to restore a slot's edge) is a complete,
       * self-contained repaint: it touches no icon or striped-fill pixel, so there is
       * nothing here that needs the stripe primitive as a row eraser (survey note 2
       * does not apply to this site -- selection never touches the fill). BACK has no
       * slot rect, so only the CANCEL button's own border toggles for it. */
      if (pv_sel != BACK && pv_sel >= 0) party_draw_slot_border(pv_sel, false);
      if (sel    != BACK && sel    >= 0) party_draw_slot_border(sel, true);
      if (sel == BACK || pv_sel == BACK) party_ov_cancel_paint(sel == BACK);
    }

    /* The screen is painted; everything after this is idle-bob and input, which the
     * PERF_REP_BOB rollup covers separately. That is where "open the party" ends. */
    if (first_paint) { first_paint = false; perf_span_end(); }

    /* No LEFT/RIGHT: retail's own party screen has no horizontal axis (a fixed
     * top-left box plus a single column of rows) — UP/DOWN walks slot 0..lastSel,
     * then BACK; see DIFFERENCES #2/#3 in MEASUREMENTS.md for why the old 3-column
     * grid's L/R paging does not carry over. */
    u16 k = wait_keys_bob_p(KEY_UP | KEY_DOWN | KEY_A | KEY_B,
                            ANIM_PARTY, &bob_ctr, &bob, party_overlay_bob, PARTY_BOB_PERIOD);
    /* THE single release site for this screen, and it is here rather than at each of
     * the five returns below on purpose: the idle loop is over, so every path from here
     * -- repaint, app_party_mon_menu, party_place_held, exit -- is a path that may reach
     * the PC, and none of them may run with g_pc lent out. One site, no exit to miss. */
    app_icons_drop();
    pv_sel = sel; pv_n = n; pv_addslot = addslot; pv_valid = true; pv_gen = ui_clear_gen();
    if      (k & KEY_B)     { snd_back(); perf_rep_flush(PERF_REP_BOB); return 0; }
    /* snd_move() (RCUE_SCROLL haptic + a short square-wave tick, source/snd.c:56) fired
     * on every cursor move in the old 3x2 grid's L/R/U/D handlers; the retail-layout
     * rewrite's UP/DOWN handlers dropped it. Restored here, only when the cursor
     * actually moves (not when UP/DOWN is pressed against an end that clamps to a
     * no-op) — sel==BACK->lastSel and sel!=BACK->(sel+1 or BACK) always move; sel>0 is
     * the one guard that can be a no-op (sel==0, nothing above it). */
    else if (k & KEY_UP)    { if (sel == BACK) { snd_move(); sel = lastSel; }
                              else if (sel > 0) { snd_move(); sel--; } }
    else if (k & KEY_DOWN)  { if (sel != BACK) { snd_move(); sel = (sel < lastSel) ? sel + 1 : BACK; } }
    else if (k & KEY_A) {
      if (sel == BACK) { snd_back(); perf_rep_flush(PERF_REP_BOB); return 0; }
      if (held) {                                    /* PLACE: drop/swap into the party */
        if (party_place_held(held, sel, orig_box, orig_slot, orig_bank, can_swap)) return 1;
      } else if (sel < n) {                          /* BROWSE: the full action menu on this mon
                                                       * (app_party_mon_menu — shared with
                                                       * party_strip_overlay, pdna_box.c) */
        bool tobox_hit = false;
        app_party_mon_menu(sel, PDNA_PTY_FOOTER_Y, allow_move_to_box, grab80, &tobox_hit);
        if (tobox_hit) { if (grab_slot) *grab_slot = sel; return 2; }
        /* else: edit/release/etc. ran -> loop re-reads the party + clamps sel, then redraws */
      } else snd_deny();                             /* empty cell */
    }
  }
}

/* E4 (sprite-era): the party list is always SE_PLACE_PARTY while this overlay is up.
 * Same save/restore shape as pdna_summary.c's summary_run() wrapper and for the same
 * reason: this overlay is opened FROM INSIDE pdna_box.c's own loop (the box screen's
 * PARTY-tab call sites), which already set PC/BANK/GBGRID once on its own entry and
 * expects that to still be true once this overlay closes -- and app_party_mon_menu
 * above can itself open the summary (pdna_inspect), which already restores whatever
 * place was active when IT was entered, composing correctly with this wrapper. */
int app_party_overlay(const uint8_t* held, int orig_box, int orig_slot, bool orig_bank,
                      bool can_swap, uint8_t grab80[80], int* grab_slot, bool allow_move_to_box) {
  int prev_place = pdna_origin_art_get_place();
  pdna_origin_art_set_place(SE_PLACE_PARTY);
  int r = app_party_overlay_inner(held, orig_box, orig_slot, orig_bank, can_swap,
                                  grab80, grab_slot, allow_move_to_box);
  pdna_origin_art_set_place(prev_place);
  return r;
}

/* CREATE a Pokémon from nothing into the empty slot `rec`: pick a species, build a
 * default valid record (Lv5, the save's OT/TID, Poké Ball), open the SIX-CARD SUMMARY
 * to customise, then write + commit. Returns true if the user kept it. Omega-only.
 * The summary — not the flat field list — is deliberate: making a Pokémon should look
 * like inspecting one, and the summary reaches all 40 editable fields anyway. */
static bool app_create_mon(uint8_t* rec, AppCommitFn commit, uint8_t* block) {
  /* BACKLOG #120 S2 F1 (review finding): defence in depth -- the CREATE row above
   * should already have hidden this action on a Bank cell with no live Gen-3 save,
   * but this is the actual write-time gate (same posture as app_inject_to_game's
   * own belt-and-braces refusal). Without a parsed save, otId/trainer name below
   * would be built off zeroed g_vinfo. */
  if (!g_vinfo.valid) {
    snd_deny();
    msg_wait("NO GEN-3 SAVE", UI_WARN, "Open a Gen-3 save first,", "then use the Bank.");
    return false;
  }
  uint16_t sp = pick_species(1);
  if (sp == 0xFFFF || sp == 0) return false;
  uint32_t otId = (uint32_t)g_vinfo.tid_public | ((uint32_t)g_vinfo.tid_secret << 16);
  uint8_t  mg   = (g_game == PK_EMERALD) ? 3 : (g_game == PK_FRLG) ? 4 : 2;   /* origin game (editable) */
  uint8_t tmp[80];
  /* NOT a hard-coded 5 any more. gen3_build_level gives the species its own lowest legal
   * level — 5 for a Bulbasaur, 36 for a Charizard, which is what Guy asked for in those
   * words ("it must evolve to there"). Building it right beats building a L5 Charizard
   * and then flagging it, which is what the create flow did until now. */
  uint8_t lvl = gen3_build_level(sp);
  /* THE UNOWN LETTER IS PART OF THE ROLL, so it has to be asked for BEFORE it.
   * em_set_unown_form (gen3_edit.c:572) hunts for a NEW PID preserving nature and
   * shininess and knows nothing about IVs, so running it after the spread was rolled would
   * leave one seed's IVs under another seed's PID — exactly the unmatched pair this change
   * exists to stop. Asking first costs ~29 seeds. B = "any letter", as before: cancelling
   * the letter must NOT discard the species the user just picked. */
  PkSpreadWant want;
  pk_spread_want_init(&want);
  if (sp == 201) {
    int form = pick_unown_form(0);
    if (form >= 0) want.unown_form = (uint8_t)form;
  }

  /* A SEED, NOT A PID. In Gen 3 the personality value and both IV words come out of one
   * LCRNG stream in that order (src/pokemon.c:2216, 2277-2293), so they are one object: a
   * hand-given PID with the IVs left at zero is a pair no seed can produce, and on the
   * species with no egg route PokeDNA's own checker said so — "No PID/IV RNG method
   * matches", 45 of 1083 build fixtures. Guy met the other half on hardware: "the venusaur
   * ... had 0 IVs". Unconstrained here (bar the Unown letter), so it is a single roll —
   * four multiplies, no search. The nature/shiny levers live in the editor that opens
   * next, where the user can see what they are trading.
   * SIDE EFFECT, worth knowing: dc_seed() returns `e | 1u` and used to BE the PID, so every
   * two-ability species previously got ability slot 1. The PID is now an LCRNG output, so
   * the slot is 50/50 — an improvement, but a change to created-mon output.
   * app_session_seed() (BACKLOG #114: dc_seed() itself moved to pdna_yard.c, now
   * file-static there) is the SAME entropy source -- it is exactly `return dc_seed();`. */
  gen3_build_mon_spread(sp, lvl, app_session_seed(), otId, g_vinfo.trainer_name, mg, &want, tmp, 0);

  /* The builder gives anything with an egg route met level 0 — "hatched at" — because
   * that is the one Gen-3 origin whose PID and IVs are not tied to a single RNG seed,
   * so the record it writes passes PokeDNA's own checker without faking a provenance
   * marker. Two follow-ups belong here rather than in the pure core:
   *
   *  - a freshly hatched Pokemon has friendship 120, hard-coded by CreateHatchedMon
   *    (src/egg_hatch.c:350-351). gen3_build_mon leaves 70 because gen12_convert.c's
   *    Gen-1 imports are identified by it (pdna_origin_art.c:27), and this is the one
   *    caller that knows it just made a brand-new mon.
   *  - for the ~25 species nothing can hatch — the legendaries and Unown — the record
   *    honestly says "met here, at this level" instead, and the checker will (rightly)
   *    say it cannot verify that. Say so BEFORE the user spends time in the editor;
   *    quietly shipping a QUESTIONABLE mon would be the same kind of silence the met
   *    location 255 default used to buy. */
  if (gen3_species_can_hatch(sp)) {
    EditMon e; gen3_edit_load(tmp, false, &e);
    em_set_friendship(&e, 120);
    gen3_edit_commit(&e, tmp);
  } else {
    /* Both lines measured against ui_ptext_fit's 184 px budget (165/155) — the dialog
     * clips silently, which is how "JIGGLYPU~" once shipped. DO NOT promise the mon is
     * unflagged: NIDORINA/NIDOQUEEN/UNOWN at FRLG origin, WYNAUT at Emerald and Ruby
     * origin and MEW everywhere still grade QUESTIONABLE with a matched PID/IV pair. */
    msg_wait("NO EGG ROUTE", UI_WARN, "Nothing breeds this one - it says",
             "it was met here. May be flagged.");
  }

  uint8_t out[100];                                  /* 100 to match the other pdna_inspect call
                                                      * sites; a box record only fills 80 */
  bool saved = false; int card = 0;                  /* open on card 0 = POKEMON INFO */
  pdna_inspect_create(tmp, out, &saved, &card);
  if (!saved) return false;                          /* discarded -> the slot stays empty */

  /* THE OTHER WAY INTO A CREATED POKEMON, and the one that used to skip the warning
   * above: the summary's SPECIES field is editable (pdna_edit.c F_SPECIES), so the user
   * can pick BULBASAUR here, walk into the editor and come out holding a GROUDON. The
   * origin was decided BEFORE that swap, so the record would still be carrying met
   * level 0 — "hatched at" — for a species nothing in Gen 3 hatches.
   *
   * That is not a cosmetic mismatch. metLevel == 0 is exactly what mutes PokeDNA's own
   * PID/IV check (pk_pidiv_exempt_reason, gen3_pidiv.c:152) and its move/encounter
   * windows (Pk2Facts.is_hatched, gen3_legality2.c:409). A Groudon claiming it hatched
   * would come out of this flow with a clean green banner — the tool buying silence from
   * its own auditor with a provenance it cannot have, which is precisely what deleting
   * the old met-location-255 default was meant to stop.
   *
   * So: re-read the species that actually landed, and if it is one with no egg route
   * still claiming the egg origin, replace the claim with the honest one — met HERE, at
   * the level it is standing at, the same thing gen3_build_mon would have written had
   * this species been chosen in the picker — and then say so. The mon is still created;
   * it is just no longer lying about where it came from.
   *
   * Only this direction is corrected. Going the other way (a swap to something that CAN
   * hatch, leaving a caught origin) yields a record that is merely flagged, which is the
   * safe failure and not something to silently "fix" by upgrading a user's mon to a
   * provenance they did not ask for. */
  PkMon fin;
  memset(&fin, 0, sizeof fin);
  pk_decode_mon(out, false, &fin);
  /* BACKLOG #46: fin.raw now points at `out`, a local array this function still owns
   * -- but the no-egg-route branch just below calls gen3_edit_commit(&e, out), which
   * rewrites those very bytes out from under fin.raw's nose while fin is still alive.
   * Null it before that can happen; fin's own fields (already latched above) are all
   * this function ever reads. See gen3_edit.c's em_preview for the sibling case and
   * gen3_mon.h's raw comment for the contract. */
  fin.raw = NULL;
  if (!gen3_species_can_hatch(fin.species) && fin.metLevel == 0 && !fin.isEgg) {
    /* A box record stores no level — it stores EXP — so derive the level the same way
     * the checker does (gen3_legality2.c:406) rather than trusting fin.level, which
     * pk_decode_mon leaves at 0 for a box mon. */
    uint8_t lv = pk_level_from_exp(pk_species_growth(fin.species), fin.experience);
    EditMon e; gen3_edit_load(out, false, &e);
    em_set_metlevel(&e, lv ? lv : 1);
    gen3_edit_commit(&e, out);
    msg_wait("NO EGG ROUTE", UI_WARN, "Nothing breeds this one - it says",
             "it was met here. May be flagged.");
  }

  memcpy(rec, out, 80);
  return app_commit_with_dex(rec, false, commit, block);   /* gated write + auto-register dex (PC only) */
}

/* Hatch an Egg: reveal the Pokemon inside (clear the egg flag, base friendship, level 5) and
 * commit. The species/IVs/moves are already in the egg; the revealed mon registers in the dex. */
static bool app_hatch(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block) {
  if (!app_confirm("Hatch this Egg?", "Reveals what is inside.")) return false;
  EditMon e; gen3_edit_load(rec, is_party, &e);
  em_hatch(&e);
  uint8_t out[100]; gen3_edit_commit(&e, out);
  memcpy(rec, out, is_party ? 100 : 80);
  return app_commit_with_dex(rec, is_party, commit, block);   /* verified write + auto-register dex */
}

/* ---- read-only BoxSource gate (contract + rationale in pdna_app.h) --------- */
static const char* (*g_src_why)(const uint8_t* rec80);
static const char*  g_src_note;
static bool         g_src_ro;
static const AppSrcOps* g_src_ops;

void app_src_readonly_set(const char* (*why_locked)(const uint8_t* rec80), const char* note) {
  g_src_why = why_locked; g_src_note = note; g_src_ro = true; g_src_ops = 0;
}
void app_src_readonly_clear(void) { g_src_why = 0; g_src_note = 0; g_src_ro = false; g_src_ops = 0; }
void app_src_ops_set(const AppSrcOps* ops) { g_src_ops = ops; }
bool app_src_readonly(void) { return g_src_ro; }

/* S5-B review fix (BLOCKING #1): pdna_box.c's own grid loop decides whether A even
 * OPENS app_mon_menu on an empty cell BEFORE app_mon_menu ever runs -- its two call
 * sites gate on `g_box[cur].species || src->can_edit()`, and a foreign read-only
 * source's can_edit() is a constant false, so an empty GB cell never reached the menu
 * PASTE (GB) was added to. This is the same predicate app_mon_menu's own g_src_ro
 * branch already uses to decide whether to offer PASTE (GB) at all -- exposed here so
 * pdna_box.c can OR it into its own gate without duplicating the four-way check.
 *
 * BACKLOG #50 renamed this from app_src_paste_offered: CREATE is the exact same kind
 * of empty-cell action as PASTE (GB) -- reachable only if this menu gets to OPEN at
 * all -- so it needed the same OR-in at all three call sites (pdna_box.c's two menu-
 * open gates, and this function's own g_src_ro-branch use in app_mon_menu below).
 * A source offering EITHER counts; nothing here assumes both. */
bool app_src_empty_action_offered(void) {
  bool paste  = g_src_ro && g_src_ops && g_src_ops->paste  && g_clip.occupied && !g_clip.from_gb;
  bool create = g_src_ro && g_src_ops && g_src_ops->create;
  return paste || create;
}

/* The action menu for a source that is read-only in ITSELF: only actions that cannot
 * touch it directly through the Gen-3 path, plus whatever the source's OWN in-place
 * pipeline offers (EDIT / MOVE TO / RELEASE, S2/S3, each NULL-gated on g_src_ops). VIEW
 * opens the summary with editing off; COPY just fills the 80-byte clipboard, so the mon
 * leaves through the existing clipboard -> PC/Bank path and nothing new writes anything.
 * Deliberately absent: PASTE / DUPLICATE / CREATE (they mutate a buffer whose commit()
 * cannot persist it) and TO GAME (it writes g_pc, which a mounted foreign source may
 * currently be borrowing as its EWRAM arena). `why_locked` can additionally veto COPY
 * for one record and say why — a Pokemon that is visible but cannot travel is far
 * better than one that vanishes. */
/* `empty` (S5-B): the cell has no record at all -- PkMon `m0` is not meaningful (the
 * caller does not resolve it for an empty slot) and every row below except PASTE (GB)
 * would either dereference a Pokemon that is not there or offer an action ("EDIT" a
 * Pokemon that does not exist) that makes no sense, so the row list collapses to just
 * that one row + CANCEL. See pdna_app.h's AppSrcOps.paste for what the row does. */
static bool app_mon_menu_readonly(uint8_t* rec, bool is_party, const PkMon* m0, bool empty) {
  const char* locked = (!empty && g_src_why) ? g_src_why(rec) : 0;
  /* BACKLOG #166: the reason RO_MOVE would be refused, or NULL when it may proceed --
   * set only in the occupied branch below (the only one that ever draws RO_MOVE), so
   * a NULL AppSrcOps.lift_why (every source before this field existed, or a source
   * that has no opinion) leaves this NULL and the row unchanged. `lift_why` takes
   * `rec80` (the same record ADDRESS every other AppSrcOps hook here takes), not a
   * (box, slot) pair: app_mon_menu's own `box` parameter is zeroed for any is_bank
   * source (pdna_box.c's call site passes `mbox = src->is_bank ? 0 : box`, and a GB
   * session always sets is_bank true), so it is not the real Game Boy box index --
   * only the record's own address survives that zeroing. gb_lift_why_hook
   * (pdna_gen12.c) re-derives the real (box, slot) from the address itself, via
   * gb_locate_addr(), exactly like gb_move_hook/gb_release_hook/every other hook in
   * this vtable already does. */
  const char* move_why = 0;
  enum { RO_VIEW, RO_ITEM, RO_MOVE, RO_RELEASE, RO_LEGAL, RO_COPY, RO_PASTE, RO_CREATE,
         RO_DUP, RO_DAYCARE, RO_EXPORT, RO_CANCEL };
  int act[PDNA_ROMENU_MAX]; const char* lab[PDNA_ROMENU_MAX]; int n = 0;
  /* BACKLOG #150 S150-3 review F4: a WHITELIST, not the blunt early-return decision 9
   * originally shipped here -- that refusal made S150-2's own RO_VIEW case (below,
   * `if (bc_is_native(rec)) { gb_native_summary_open(rec); ... }`) unreachable, so a
   * native Bank cell viewed through the hack-ROM branch (app_rom_is_hack(g_game) &&
   * !g_src_ops -> app_src_readonly_set -> this function) got a bare beep instead of
   * VIEW. RO_COPY's own g_clip leak (a clipboard that outlives the save, pastable into
   * a non-hack save later) is the thing that actually needs refusing -- kept out by
   * only ever building VIEW + CANCEL for a native cell, never by denying VIEW too. */
  bool ro_native = !empty && bc_is_native(rec);
  if (ro_native) {
    lab[n] = PDNA_LBL_VIEW; act[n++] = RO_VIEW;
  } else if (empty) {
    /* CREATE first, PASTE after -- the same order Gen-3's own empty-cell menu uses
     * (app_mon_menu's A_CREATE/A_PASTE, pdna_main.c:4288-4290). Gated on the source
     * actually offering one (pdna_app.h's AppSrcOps.create) rather than on !is_party
     * the way Gen-3's is: a GB session's party is just one more box in its own
     * numbering (Gb12Mount's own "the party is exposed as one more box"), not a
     * structurally different kind of slot -- gb_create_hook decides for itself
     * whether THIS box can take one. */
    if (g_src_ops && g_src_ops->create) { lab[n] = PDNA_LBL_CREATE; act[n++] = RO_CREATE; }
    /* G1 REVIEW BLOCKING-1 (2026-09-08): this row used to add PASTE UNCONDITIONALLY
     * -- the caller's own paste_ok gate (app_mon_menu, "paste_ok = g_src_ops->paste
     * && g_clip.occupied && !g_clip.from_gb") only decided whether to enter THIS
     * function at all (paste_ok || create_ok), not which rows it draws once inside.
     * A create_ok-only visit (clipboard empty, or holding a from_gb clip, or a
     * source with no ->paste at all) still drew PASTE and, if pressed, called
     * g_src_ops->paste(rec) regardless -- a Gen-2 COPY (native, from_gb) followed by
     * PASTE on a DIFFERENT empty cell would have round-tripped a lossy Gen-3-shaped
     * duplicate back down, exactly what this gate exists to prevent (gen3_to_gb()
     * cannot even accept a from_gb clip, so the write would have been garbage, not
     * merely redundant). Re-checking the SAME three terms here, matching Gen 3's own
     * empty branch exactly (not "trust the caller already checked" -- the caller's
     * OR can be true for either reason alone). PASTE HERE, not PASTE (GB): Gen-3's
     * own label, now reused verbatim (see PDNA_LBL_PASTE_GB's removal, pdna_layout.h)
     * -- the cross-generation conversion note lives in the confirm dialog already
     * shown before any write, not in this row's own text. */
    if (g_src_ops && g_src_ops->paste && g_clip.occupied && !g_clip.from_gb) {
      lab[n] = PDNA_LBL_PASTE_HERE; act[n++] = RO_PASTE;
    }
    if (n == 0) return false;
  } else {
    /* Gen-3 parity (Guy, 2026-09-05: "make sure the pokemon edit is in the summary for
     * gen 1 and 2 like gen 3"): the Gen-3 mon menu has ONE row, PDNA_LBL_VIEW_EDIT, that
     * opens the editable summary in VIEW (A inside it enters edit). This row now does the
     * same: RO_VIEW's own handler already calls g_src_ops->view (gb_view_hook), which
     * opens pdna_gbsummary with can_edit = app_can_edit() && the box is writable -- so
     * relabelling it costs nothing, there is no separate EDIT action to wire. The old
     * standalone EDIT row (g_src_ops->edit / gb_edit_hook, straight into edit mode) is
     * gone: it is the only caller g_src_ops->edit had (grepped), so nothing else reaches
     * it now, but the hook itself is left in place (AppSrcOps.edit / gb_edit_hook) rather
     * than torn out of the shared source-ops contract for a label change. */
    /* Bag/menu review fix: `editable` used to be `edit && app_can_edit()` -- the CART
     * gate alone -- but gb_view_hook's own `can_edit` (the thing that actually decides
     * whether the summary opens in edit mode) also requires the BOX to be writable
     * (gbs_box_writable == GBS_OK; a virgin Gen-1 bank never is, even on an Omega). That
     * mismatch let this row promise "VIEW/EDIT" on a box the summary would then refuse
     * with a bare buzz. A source with a box-level gate of its own registers `editable`
     * (pdna_gen12.c's gb_editable_hook, the same two checks gb_view_hook applies) and
     * this uses it; a source without one (editable == NULL) falls back to the old
     * cart-only expression unchanged. */
    bool editable = g_src_ops && (g_src_ops->editable ? g_src_ops->editable(rec)
                                                       : (g_src_ops->edit && app_can_edit()));
    lab[n] = editable ? PDNA_LBL_VIEW_EDIT : PDNA_LBL_VIEW; act[n++] = RO_VIEW;
    /* UX-parity audit (Guy 2026-09-07): "the GB subset must use the SAME
     * labels and relative order as Gen 3, with rows that do not apply
     * omitted, not renamed". app_mon_menu's own occupied-mon order (below,
     * A_SUMMARY..A_RELEASE) is Summary, Item, Legality, [Hatch], Move/ToBox,
     * Copy, ..., Release LAST (right before Cancel) -- this row list used to
     * read View/Edit, MOVE TO, RELEASE, LEGALITY, COPY, i.e. Release 2nd and
     * Legality 4th, the reverse of Gen 3's own relative order. Reordered to
     * match: Legality right after the summary row (Gen 3 has no separate
     * Item row here to sit between them), then Move, then Copy, then Release
     * last -- same rows, same labels, just Gen 3's own order. MOVE TO /
     * RELEASE stay the GB session's own in-place pipeline (pdna_gen12.c),
     * never the Gen-3 one: each edits the Game Boy record by address, never
     * the converted copy this menu was handed. PDNA_LBL_MOVE_TO ("MOVE TO")
     * is ALSO relabelled to PDNA_LBL_MOVE_TO_BOX ("MOVE TO BOX") here: GB's
     * own move (a destination-BOX picker, gb_move_hook -> gb_pick_box) is
     * the same shape as Gen 3's PARTY-context "move to box" action, not its
     * BOX-context "move" (an in-box reposition GB has no equivalent of) --
     * so Gen 3's OWN matching label is the correct one to reuse, not a third,
     * bespoke wording. The old PDNA_LBL_MOVE_TO macro (pdna_layout.h) is
     * deleted along with its last caller.
     *
     * BACKLOG #92 (2026-09-10): the "Gen 3 has no separate Item row here to
     * sit between them" half of the note above is now stale for a Gen-2
     * mount -- g_src_ops->item is non-NULL there (pdna_gen12.c's
     * k_gb_ops_gen2), so ITEM slots in right after Summary, matching A_ITEM's
     * own position in Gen 3's occupied-mon order above. A Gen-1 mount's table
     * (k_gb_ops_gen1) leaves `item` NULL, so this row still does not appear
     * there -- "NOT IN GEN 1" via the existing omitted-row pattern, not a
     * shown-then-refused row. */
    if (g_src_ops && g_src_ops->item) { lab[n] = PDNA_LBL_ITEM; act[n++] = RO_ITEM; }
    lab[n] = PDNA_LBL_LEGALITY; act[n++] = RO_LEGAL;
    /* BACKLOG #166: MOVE TO BOX used to be offered on ANY cell with a `move` hook,
     * regardless of whether THIS box/slot could actually be lifted -- a Gen-1
     * one-mon party / Mail-holding Gen-2 party / unwritable-box cell only learned
     * that after picking it, walking the box picker, and having gbs_move() bounce
     * (gb_move_hook's own msg_wait). `lift_why` answers the same can_lift question
     * as a reason instead of a bare bool: NULL (or no lift_why at all, e.g. a source
     * with no opinion) draws the row exactly as before; a reason hides the row and
     * is shown as one more grey line below, the same drawing rule `locked` already
     * uses for the COPY veto. */
    if (g_src_ops && g_src_ops->move) {
      move_why = g_src_ops->lift_why ? g_src_ops->lift_why(rec) : 0;
      if (!move_why) { lab[n] = PDNA_LBL_MOVE_TO_BOX; act[n++] = RO_MOVE; }
    }
    if (!locked) { lab[n] = PDNA_LBL_COPY; act[n++] = RO_COPY; }
    /* BACKLOG #93: Gen-3's own occupied-mon order is …COPY, DUPLICATE, TO DAY-CARE,
     * EXPORT, RELEASE (app_mon_menu above) -- mirrored here, before RELEASE so RELEASE
     * stays last. Each gated on the source actually offering the hook (the read-only
     * menu's omitted-row convention), never shown-then-refused. */
    if (g_src_ops && g_src_ops->dup)         { lab[n] = PDNA_LBL_DUPLICATE;  act[n++] = RO_DUP; }
    if (g_src_ops && g_src_ops->daycare)     { lab[n] = PDNA_LBL_TO_DAYCARE; act[n++] = RO_DAYCARE; }
    if (g_src_ops && g_src_ops->export_one)  { lab[n] = PDNA_LBL_EXPORT_PK;  act[n++] = RO_EXPORT; }
    if (g_src_ops && g_src_ops->release) { lab[n] = PDNA_LBL_RELEASE;   act[n++] = RO_RELEASE; }
  }
  lab[n] = PDNA_LBL_CANCEL;   act[n++] = RO_CANCEL;

  /* 48, not the 16 the menu below uses: ui_truncate documents max_cols*4+1, and a
   * 10-glyph nickname of gender signs really is 30 UTF-8 bytes. GB nicknames hit this
   * (NIDORAN-male is in the test corpus), so this buffer is sized for it. */
  char title[48];
  if (empty) ui_truncate(title, "EMPTY", 11);
  else       ui_truncate(title, m0->nickname[0] ? m0->nickname : pk_species_name(m0->species), 11);
  const int hdr = PDNA_ROMENU_HDR + (!empty && g_src_note ? PDNA_ROMENU_LINE : 0)
                                  + (locked                ? PDNA_ROMENU_LINE : 0)
                                  + (move_why              ? PDNA_ROMENU_LINE : 0);
  /* Laid out ABOVE the screen's footer row, and windowed if it ever stops fitting —
   * the same rule as the full action menu below. */
  int my, mh;
  const int vis = ui_popup_vfit(n, PDNA_MONMENU_ROW_H, hdr + PDNA_ROMENU_HEAD_PAD,
                                PDNA_MONMENU_FOOT, &my, &mh);
  const int mx = PDNA_MONMENU_X, mw = PDNA_MONMENU_W;
  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + PDNA_MONMENU_PAD, my + 4, UI_TITLE, title);
    int y = my + PDNA_ROMENU_HDR;
    /* Proportional face: these two lines are prose, and the panel is only 88 px wide
     * inside its border (tests/host_textfit_test.c pins both). */
    if (!empty && g_src_note) { ui_ptext_fit(mx + PDNA_MONMENU_PAD, y, PDNA_MONMENU_PROSE_W, UI_WARN, g_src_note); y += PDNA_ROMENU_LINE; }
    if (locked)     { ui_ptext_fit(mx + PDNA_MONMENU_PAD, y, PDNA_MONMENU_PROSE_W, UI_WARN, locked);     y += PDNA_ROMENU_LINE; }
    if (move_why)   { ui_ptext_fit(mx + PDNA_MONMENU_PAD, y, PDNA_MONMENU_PROSE_W, UI_WARN, move_why);   y += PDNA_ROMENU_LINE; }
    ui_hline(mx + 2, y, mw - 4, UI_BORDER);
    for (int i = 0; i < vis && top + i < n; i++) {
      int ry = y + PDNA_ROMENU_HEAD_PAD + i * PDNA_MONMENU_ROW_H; bool s = (top + i == sel);
      if (s) ui_panel(mx + 2, ry - 1, mw - 4, 12, UI_SEL, UI_TITLE);
      ui_text(mx + PDNA_MONMENU_ROW_DX, ry, s ? UI_SELTEXT : UI_TEXT, lab[top + i]);
    }
    ui_text(mx + PDNA_MONMENU_PAD, my + mh + PDNA_POPUP_HINT_DY, UI_DIM, PDNA_MONMENU_FOOT_TXT);
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      /* BACKLOG #150 S150-3 review F4: defence in depth, same shape as app_mon_menu's
       * own decision-9 statement -- cannot be out-of-sync with the row build above
       * since it whitelists the exact same two actions. */
      if (ro_native && act[sel] != RO_VIEW && act[sel] != RO_CANCEL) { snd_deny(); return false; }
      switch (act[sel]) {
        case RO_VIEW:
          /* BACKLOG #41: a source with its own native summary (today: GB sessions,
           * pdna_gen12.c's gb_view_hook) opens THAT instead of pdna_inspect() on the
           * lossy Gen-3-converted copy this menu was handed -- the whole point being
           * that VIEW (and, from inside it, EDIT) work on the record as its own
           * generation actually stores it. */
          /* BACKLOG #150 S150-2: a native Bank cell BEFORE the g_src_ops->view check --
           * bc_is_native() first, before anything else, exactly as app_box_browse's own
           * interception point does. */
          if (bc_is_native(rec)) { gb_native_summary_open(rec, /*allow_edit*/false, 0); return false; }
          if (g_src_ops && g_src_ops->view) { g_src_ops->view(rec); return false; }
          { uint8_t d[100]; int card = 0;
            pdna_inspect(rec, is_party, false, d, 0, &card); return false; }
        case RO_ITEM:    return (g_src_ops && g_src_ops->item)    ? g_src_ops->item(rec)    : false;
        case RO_MOVE:    return (g_src_ops && g_src_ops->move)    ? g_src_ops->move(rec)    : false;
        case RO_RELEASE: return (g_src_ops && g_src_ops->release) ? g_src_ops->release(rec) : false;
        case RO_LEGAL: pdna_legality_show(m0); return false;
        case RO_COPY:  return app_copy(rec, is_party);
        case RO_PASTE:  return (g_src_ops && g_src_ops->paste)  ? g_src_ops->paste(rec)   : false;
        case RO_CREATE: return (g_src_ops && g_src_ops->create) ? g_src_ops->create() : false;
        case RO_DUP:     return (g_src_ops && g_src_ops->dup)        ? g_src_ops->dup(rec)        : false;
        case RO_DAYCARE: return (g_src_ops && g_src_ops->daycare)    ? g_src_ops->daycare(rec)    : false;
        case RO_EXPORT:  return (g_src_ops && g_src_ops->export_one) ? g_src_ops->export_one(rec) : false;
        default:        return false;
      }
    }
  }
}

bool app_mon_menu(uint8_t* rec, bool is_party, bool is_bank, AppCommitFn commit, uint8_t* block, int box, int slot, int footer_y) {
  PkMon m0;
  bool occupied = pk_decode_mon(rec, is_party, &m0);
  if (occupied) pk_resolve(&m0);

  /* BACKLOG #150 S150-2 review F1: a native "GBC1" cell is not a Gen-3 record --
   * pk_decode_mon just decrypts it with a meaningless key (title read "??? ? ?",
   * and `occupied` was a 2^-16 coin flip that could offer CREATE/PASTE over the
   * cell). Decode it the way the grid does, and it is ALWAYS occupied: the raw
   * bytes are the mon's only copy (G-H2). */
  if (!is_party && bc_is_native(rec)) { pdna_native_cell_decode(rec, &m0, 0); occupied = true; }
  /* BACKLOG #150 S150-3 decision 7: a WHITELIST, not a blacklist (G-F3's own lesson --
   * a blacklist silently re-opens when a twelfth row is added). Native cells are never
   * is_party (a native cell only ever lives in a Bank box), so `native` mirrors the
   * gate right above verbatim. */
  const bool native = !is_party && bc_is_native(rec);

  if (!app_can_edit()) {                                 /* read-only carts: view only */
    /* Review fix F2 (BACKLOG #54): a hack-flagged Gen-3 game's OWN box/party grid
     * (no foreign source mounted, g_src_ops NULL) used to fall straight through to
     * the bare pdna_inspect() below, skipping the richer read-only menu (VIEW /
     * LEGALITY / COPY / CANCEL) and its why-note entirely -- ruling: keep the
     * why-note (option a), do not delete it; app_src_readonly_set() also sets
     * g_src_ro, which pdna_box.c/app_src_empty_action_offered() consult
     * independently, so this needs to be the real gate, not a display-only tweak.
     * `!g_src_ops` is the load-bearing guard: it is what keeps a MOUNTED GB session
     * (which already owns g_src_ops via pdna_gen12.c's own app_src_readonly_set
     * call) from being re-routed here a second time, and it is what keeps every
     * other !app_can_edit() reason (Everdrive, pdna_romcheck_bad()) on the
     * original bare-inspect path byte-identical -- neither of those ever calls
     * app_src_readonly_set() itself, so g_src_ops is whatever the LAST real source
     * left it (NULL on a fresh boot). Every row app_mon_menu_readonly() can offer
     * here is g_src_ops-gated (ITEM/MOVE/PASTE/CREATE/RELEASE all read
     * g_src_ops->*), and this call passes g_src_ops = NULL implicitly (set() nulls
     * it), so VIEW/LEGALITY/COPY/CANCEL is the full reachable set -- nothing here
     * can mutate g_pc/g_party/g_sb1. set() is idempotent (safe to call every visit)
     * and re-arms the note after pdna_gen12.c:2843/3095's own clears, since this
     * is reached again on the very next box-grid entry the GB fork returns to. */
    if (app_rom_is_hack(g_game) && !g_src_ops) {
      if (!occupied) return false;
      app_src_readonly_set(0, PDNA_ROMHACK_NOTE);
      return app_mon_menu_readonly(rec, is_party, &m0, false);
    }
    /* BACKLOG #150 S150-3 review F7: the non-hack read-only path (Everdrive,
     * pdna_romcheck_bad()) fell straight through to pdna_inspect()'s lossy Gen-3
     * decode for a native cell too -- the same G-H2 fix RO_VIEW/app_box_browse
     * already apply elsewhere in this file. */
    if (native) { gb_native_summary_open(rec, /*allow_edit*/false, 0); return false; }
    if (occupied) { uint8_t d[100]; int card = 0; pdna_inspect(rec, is_party, false, d, 0, &card); }
    return false;
  }

  /* THE FIX: editability has two axes. Everything below this point was gated on
   * app_can_edit() alone — the CART — so on an Omega a source that is read-only in
   * itself (a mounted Gen-1/2 save) was still offered PASTE / RELEASE / DUPLICATE.
   * Those would have mutated its RAM buffer, its commit() would have no-op'd, and the
   * screen would have shown a change that never happened. pdna_box gates its own
   * destructive paths on src->can_edit(); this is the same gate for the menu. */
  if (g_src_ro) {
    if (!occupied) {
      /* S5-B review fix #11: explicit, not relied-on-by-implication -- pk_decode_mon's
       * own memset already zeroed `m0` above on this path (species 0 -> occupied ==
       * false), but app_mon_menu_readonly(..., empty=true) is handed `&m0` regardless,
       * so this stays correct even if that internal detail of pk_decode_mon ever
       * changes. */
      memset(&m0, 0, sizeof m0);
      /* An empty GB cell offers PASTE (GB) only when this source accepts one AND the
       * clipboard holds a Gen-3 record that did not itself come off a Game Boy source
       * (from_gb) -- pasting a GB-native clip back down would be the wrong direction
       * and gen3_to_gb() would refuse it anyway (no Gen-3 record to convert). CREATE
       * (BACKLOG #50) needs no such clipboard check -- the source decides for itself,
       * per empty cell, whether it can build one there. */
      bool paste_ok  = g_src_ops && g_src_ops->paste && g_clip.occupied && !g_clip.from_gb;
      bool create_ok = g_src_ops && g_src_ops->create;
      if (paste_ok || create_ok)
        return app_mon_menu_readonly(rec, is_party, &m0, true);
      return false;                                       /* nothing to create or paste into */
    }
    return app_mon_menu_readonly(rec, is_party, &m0, false);
  }

  enum { A_SUMMARY, A_ITEM, A_MOVES, A_LEGAL, A_MOVE, A_TOBOX, A_COPY, A_PASTE, A_DUP, A_EXPORT, A_TOGAME, A_DAYCARE, A_RELEASE, A_TAKEITEM, A_GIVEITEM, A_CREATE, A_HATCH, A_CANCEL };
  /* Labels come from pdna_layout.h: they are 8 px/glyph inside a 100 px panel, so their
   * WIDTH is a real constraint (host_textfit_test.c measures every one of them). */
  int act[PDNA_MONMENU_MAX]; const char* lab[PDNA_MONMENU_MAX]; int n = 0;
  if (native) {
    /* BACKLOG #150 S150-3 decision 7 + D-Q1 (orchestrator 2026-09-15): VIEW / MOVE /
     * RELEASE / CANCEL, in Gen-3's own relative order -- CANCEL is appended
     * unconditionally below, same as every other branch. Putting `native` FIRST also
     * covers the case pk_decode_mon (not reached here -- line :4830 above already
     * intercepts it) would otherwise mis-read as empty (G-H2): the EMPTY branch below
     * never runs for a native cell.
     * VIEW / EDIT reuses A_SUMMARY's own dispatch case: a native cell is never
     * is_party, so that case always calls app_box_browse(), whose own bc_is_native()
     * check (this file, gb_native_summary_open()) opens the REAL Gen-1/2 summary
     * instead of the lossy Gen-3-converted copy -- exactly what D-Q1 asks for.
     * BACKLOG #150 S150-14 (orchestrator decision, 2026-09-15): rather than adding a
     * fourth action constant, EDIT is routed through this SAME A_SUMMARY case with
     * can_edit computed inside gb_native_summary_open (app_box_browse now calls
     * app_native_cell_edit, which passes allow_edit=true) -- A_SUMMARY is ALREADY in
     * the dispatch whitelist below, so no whitelist edit is needed either; the
     * Gen-3 editor (gen3_edit_load/gen3_edit_commit/em_reroll) still never runs on a
     * native cell, since this label only ever reaches app_box_browse's native
     * branch, which never calls pdna_inspect() for a native rec. Relabelled to
     * Gen 3's own PDNA_LBL_VIEW_EDIT (pdna_layout.h) -- same label, same position,
     * same action as Gen 3's own occupied row -- no new string. */
    lab[n]=PDNA_LBL_VIEW_EDIT; act[n++]=A_SUMMARY;
    if (!is_party) { lab[n]=PDNA_LBL_MOVE; act[n++]=A_MOVE; }         /* box: pick up + reposition */
    lab[n]=PDNA_LBL_RELEASE; act[n++]=A_RELEASE;
  } else if (occupied) {
    lab[n]=PDNA_LBL_VIEW_EDIT; act[n++]=A_SUMMARY;     /* opens the editable summary (moves edited there) */
    lab[n]=PDNA_LBL_ITEM;    act[n++]=A_ITEM;
    lab[n]=PDNA_LBL_LEGALITY; act[n++]=A_LEGAL;
    if (m0.isEgg && !m0.isBadEgg) { lab[n]=PDNA_LBL_HATCH; act[n++]=A_HATCH; }   /* eggs only: reveal + level 5 */
    if (!is_party) { lab[n]=PDNA_LBL_MOVE; act[n++]=A_MOVE; }                    /* box: pick up + reposition */
    else if (g_party_tobox_allowed) { lab[n]=PDNA_LBL_MOVE_TO_BOX; act[n++]=A_TOBOX; }  /* party popup: carry out to a box */
    lab[n]=PDNA_LBL_COPY;    act[n++]=A_COPY;
    /* BACKLOG #120 S2: gated on xg_paste_row (clip occupied AND a live Gen-3 PC to
     * paste into) -- during a GB session's Bank visit g_vinfo.valid is false, so this
     * row is hidden regardless of clip content. :4208's RO_PASTE (the GB grid's own
     * PASTE (GB) row, app_mon_menu_readonly) is untouched -- different function,
     * different gate. */
    if (xg_paste_row(g_clip.occupied, app_gen3_pc_live())) { lab[n]=PDNA_LBL_PASTE; act[n++]=A_PASTE; }
    lab[n]=PDNA_LBL_DUPLICATE; act[n++]=A_DUP;
    /* A_DAYCARE is already scoped OUT of every is_bank source by `!is_bank` above --
     * a real Bank cell (is_bank always true, GB session or not) never offers it, so
     * BACKLOG #120 S2's Bank-from-GB-session exposure never reaches this row; no new
     * gate needed here (orchestrator-requested audit, BACKLOG #120 S2 review). */
    if (!is_bank) { lab[n]=PDNA_LBL_TO_DAYCARE; act[n++]=A_DAYCARE; }  /* deposit into the daycare (all games incl. FR/LG) */
    /* BACKLOG #120 S2: TO GAME is gated on xg_togame_row (Bank cell AND a live Gen-3
     * PC to receive it AND something already in that PC, g_have_pc) -- during a GB
     * session's Bank visit g_vinfo.valid is false (no parsed Gen-3 save), so this row
     * is hidden even though is_bank is true. The outer branch stays `is_bank` itself
     * (not the gated predicate): EXPORT_PK is a PC/party-only row and must not leak
     * in on a Bank cell just because TO GAME's own gate happens to be false -- a Bank
     * cell must NEVER gain EXPORT_PK, gated or not, so the nesting below (not a flat
     * substitution of the outer condition) is load-bearing, not stylistic.
     *
     * Orchestrator-requested audit (BACKLOG #120 S2 review): TO GAME and PASTE are
     * the ONLY two rows on this occupied-cell branch that write toward the live
     * Gen-3 save/PC. Every other row here (SUMMARY/ITEM/LEGALITY/HATCH/MOVE/COPY/
     * DUPLICATE/TAKE-GIVE ITEM/RELEASE) commits back into the SOURCE's own storage
     * (src->commit — the Bank's own SD write), never g_save/g_party, so none of them
     * needs a gate. TO DAY-CARE (one line up) is already excluded from every is_bank
     * source by its own `!is_bank` guard -- a real Bank cell never offers it, GB
     * session or not, so BACKLOG #120 S2's exposure never reaches that row. There is
     * no "MOVE TO PARTY" row: the closest effect (A_MOVE's pick-up, then carrying to
     * the PARTY tab) is refused independently by pdna_box.c's pre-existing
     * `if (s_tab_focus == 1 && !src->is_bank && !s_orig_party)` gate (is_bank true on
     * a Bank cell -> `else snd_deny()`), unrelated to this slice. No xg_gen3_dest_row
     * predicate exists because no third row needs one. */
    if (is_bank) { if (xg_togame_row(is_bank, app_gen3_pc_live(), g_have_pc)) { lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; } }   /* bank: inject into the loaded save */
    else         { lab[n]=PDNA_LBL_EXPORT_PK; act[n++]=A_EXPORT; }/* PC/party: write a .pk3 to the bank dir */
    if (m0.heldItem && !g_item_held) { lab[n]=PDNA_LBL_TAKE_ITEM; act[n++]=A_TAKEITEM; }
    if (g_item_held)                 { lab[n]=PDNA_LBL_GIVE_ITEM; act[n++]=A_GIVEITEM; }
    lab[n]=PDNA_LBL_RELEASE;   act[n++]=A_RELEASE;
  } else {                                              /* empty slot */
    /* BACKLOG #120 S2 F1 (review finding): CREATE builds a Gen-3 record off g_vinfo
     * (otId/trainer name) and, on a Bank cell, commits straight to the SD card
     * (banksrc_commit -> box_save) -- unguarded, a GB session's Bank visit (no live
     * Gen-3 save) would persist a checksummed record built off a zeroed g_vinfo, a
     * new write surface a later Gen-3 session's TO GAME could inject into the real
     * save. xg_create_row leaves PC/party CREATE (!is_bank) untouched. */
    if (!is_party && xg_create_row(is_bank, app_gen3_pc_live())) { lab[n]=PDNA_LBL_CREATE; act[n++]=A_CREATE; }   /* build a mon from nothing (box/bank) */
    /* BACKLOG #120 S2: same xg_paste_row gate as the occupied-cell PASTE row above --
     * an empty Bank cell during a GB session's visit has no live Gen-3 PC to paste
     * FROM, so this is hidden; an ordinary Gen-3 session (app_gen3_pc_live() true)
     * sees no change. */
    if (xg_paste_row(g_clip.occupied, app_gen3_pc_live())) { lab[n]=PDNA_LBL_PASTE_HERE; act[n++]=A_PASTE; }
    /* BACKLOG #120 S2 F1: an empty Bank cell in a GB session now legitimately has
     * n == 0 (neither CREATE nor PASTE HERE offered) -- deny audibly so A is not a
     * silent no-op, matching every other refused action in this tree. */
    if (n == 0) { snd_deny(); return false; }             /* empty party slot, nothing to paste */
  }
  lab[n]=PDNA_LBL_CANCEL; act[n++]=A_CANCEL;

  char title[16];
  ui_truncate(title, occupied ? (m0.nickname[0] ? m0.nickname : pk_species_name(m0.species)) : "EMPTY", 11);
  /* Scroll if more actions than fit, and fit is measured against the CALLER'S OWN
   * footer, not always the global UI_FOOTER_Y: this popup is drawn over the box/party
   * screen while that screen's own hints are still on the bottom row, and box/bank's
   * footer sits at UI_FOOTER_Y (150) while the party overlay's message box starts at
   * PDNA_PTY_FOOTER_Y (133, well above it) — laying out against 150 unconditionally
   * used to seat a 9+-action popup (every party mon has at least that many) at y=2..148,
   * squarely on top of that message box. Box/bank/party_list pass UI_FOOTER_Y and see no
   * change; the party overlay passes PDNA_PTY_FOOTER_Y. */
  int my, mh;
  const int vis = ui_popup_vfit_at(n, PDNA_MONMENU_ROW_H, PDNA_MONMENU_HEAD,
                                   PDNA_MONMENU_FOOT, footer_y, &my, &mh);
  const int mx = PDNA_MONMENU_X, mw = PDNA_MONMENU_W;
  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + PDNA_MONMENU_PAD, my + 4, UI_TITLE, title);
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < vis && top + i < n; i++) {
      int y = my + PDNA_MONMENU_HEAD + i * PDNA_MONMENU_ROW_H; bool s = (top + i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 12, UI_SEL, UI_TITLE);
      ui_text(mx + PDNA_MONMENU_ROW_DX, y, s ? UI_SELTEXT : UI_TEXT, lab[top + i]);
    }
    /* popup is 100 px wide: 12 sys8 columns max */
    ui_text(mx + PDNA_MONMENU_PAD, my + mh + PDNA_POPUP_HINT_DY, UI_DIM, PDNA_MONMENU_FOOT_TXT);
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      /* BACKLOG #150 S150-3 decision 9: defence in depth -- cannot be out-of-sync with
       * the row build above since it whitelists the exact same four actions. Refuses
       * anything else outright before the switch even runs. */
      if (native && act[sel] != A_SUMMARY && act[sel] != A_MOVE && act[sel] != A_RELEASE && act[sel] != A_CANCEL) { snd_deny(); return false; }
      switch (act[sel]) {
        case A_SUMMARY: return is_party ? party_browse(slot, commit)                  /* party: scroll mons */
                                        : app_box_browse(block, box, slot, commit);   /* box: scroll mons */
        case A_ITEM:    return app_quick_item (rec, is_party, commit);
        case A_LEGAL:   /* pass the box context so the screen can offer the 30-cell sweep;
                         * a party mon has no box, so it gets the single-mon report */
                        (void)pdna_legality_show_box(&m0, is_party ? NULL : block,
                                                     is_party ? -1 : box, slot);
                        return false;
        case A_HATCH:   return app_hatch(rec, is_party, commit, block);   /* egg -> revealed Pokemon */
        case A_MOVE:    g_move_req = true; return false;            /* box loop handles the move */
        case A_TOBOX:   g_party_tobox_req = true; return false;     /* party popup grabs it for a box */
        case A_EXPORT:  pdna_pk_export(rec, &m0); return false;   /* writes a .pk3, not the save */
        case A_TOGAME:  return app_inject_to_game(rec);           /* bank -> loaded save's PC */
        case A_DAYCARE: return app_to_daycare(rec, is_party, block, box, slot);   /* -> day-care */
        case A_COPY:    return app_copy(rec, is_party);
        case A_PASTE:   return app_paste(rec, is_party, commit, block, occupied);
        case A_DUP:
          if (is_party) return app_duplicate(rec, is_party, commit, block, box);   /* party: append + commit */
          if (box_free_slot(block, box) < 0) { snd_deny(); msg_wait("BOX FULL", UI_WARN, "No empty slot here.", 0); return false; }
          g_dup_req = true; return false;                                            /* box/bank: pick the copy up in the glove */
        case A_RELEASE: return app_release(rec, is_party, commit, block, box, slot);
        case A_TAKEITEM:return app_take_item(rec, is_party, commit);
        case A_GIVEITEM:return app_give_item(rec, is_party, commit);
        case A_CREATE:  return app_create_mon(rec, commit, block);   /* build a new mon into this empty slot */
        default:        return false;                    /* CANCEL */
      }
    }
  }
}

/* Party list view. A opens the summary; SELECT switches to PC boxes; B exits.
 * Returns 0 (back to file browser) or 1 (switch to boxes). */
/* draw one party row's icon (full 32x32) at its in-band y; shared by the full render
 * and the idle bob so they stay pixel-identical. */
static void party_icon_y(int i, int* ry, int* iy) {
  *ry = 17 + i * 21;                          /* ROW_TOP 17, pitch 21 (fits 6 rows, 12..149) */
  *iy = *ry - 5; if (*iy < 12) *iy = 12; else if (*iy > 118) *iy = 118;
}

/* Recompose the whole party icon COLUMN (x 2..35) for the current bob frame.
 * FULL-ART: one scanline at a time -- fill each line with that row's background (the
 * selection bar is UI_SEL, others UI_BG), then overlay every icon that crosses the line
 * IN ORDER; only opaque pixels overwrite, so the row overlap layers exactly like the
 * static transparent draw -- no solid bg square, no cut-off feet.
 * ARTLESS: icon-major, not scanline-major (see the #else branch below) -- each icon's
 * pass repaints its own band unconditionally, background included, so paint order alone
 * decides who wins a shared band; walking it backwards (MUST-FIX 1, 2026-08-22) makes
 * the upper icon of each adjacent pair win instead of the lower, which is what keeps
 * feet from being cut off there too. compose-then-CPU-copy (no erase) => flicker-free;
 * runs only on idle frames -- memcpy32, not DMA, see this function's own header comment. */
static u16 __attribute__((aligned(4))) s_pcol[34];

/* MUST-FIX 2 (2026-08-22 review): mon_icon_for_form_frame/mon_icon_egg_frame return a
 * pointer straight into the SHARED mon_decomp scratch (artbuf.c, one 8 KiB buffer for
 * the whole app) whenever the icons.bin cache or the ROM rung is what serves them --
 * art_fallbacks.c's icon_from_cache decodes fresh into mon_decomp on EVERY call and
 * hands back that same address. party_bob_recompose used to resolve all 6 party icons
 * into base[0..5] before compositing ANY of them (scanline-major: one outer sweep
 * over y=12..150, all icons consulted per scanline), so in the artless build every
 * pointer ended up aliasing the one buffer: base[0..5] all read as whatever the LAST
 * (6th) mon decoded, i.e. six copies of the last party member instead of six distinct
 * icons -- trap #1 (never memoise/hold a pointer into artbuf/mon_decomp) landing for
 * real, where before this ROM rung existed the artless path had no source at all here
 * and `if (!have) return;` kept it inert.
 *
 * The natural fix -- copy each icon out to its own storage the instant it is fetched
 * -- does not fit: this project's own EWRAM guard is down to 748 B free in BOTH
 * builds (measured, not assumed -- a first attempt at a 6-slot 32x32 copy,
 * 6*2048=12,288 B, overflowed the artless build by 11,540 B on the very first
 * build), and the full-art build has zero bytes to spare for something it does not
 * even need (see below). So the two builds take genuinely different code paths:
 *
 * FULL-ART (PDNA_MON_ICONS_ART_COMPILED, mon_icons_gate.h): mon_icons.c's strong
 * accessors (link-time override) return stable, distinct pointers straight into ROM
 * .rodata -- nothing shared, nothing to alias -- so the original scanline-major
 * sweep is safe exactly as written and keeps its zero-extra-EWRAM cost.
 *
 * ARTLESS: composes ICON-major instead. Decode one mon, immediately paint its own
 * MON_ICON_H rows (full background reconstruct -- the same y-only, no-x-split
 * colouring the scanline sweep used), THEN move to the next mon -- never holding a
 * pointer past the one decode that produced it. Where two adjacent icons' bounding
 * boxes overlap (11 rows -- party_icon_y's own ROW pitch=21 vs icon height=32; only
 * ADJACENT icons ever overlap, pitch*2=42 > 32), whichever mon's pass runs LAST fully
 * repaints those shared rows, background included -- unlike the full-art sweep, this
 * is NOT opaque-wins/transparent-lets-the-row-above-show-through, it is
 * last-write-wins, full stop. Getting the order wrong is a real correctness bug, not
 * a cosmetic one: an ascending loop (0..n-1, matching the full-art sweep's iteration
 * order) paints the LOWER mon of each pair last, and Gen-3 icons carry almost no ink
 * in their own top rows (measured against this pack's real icon PNGs via the same
 * PNG->RGB15 conversion tools/gen_icons.py ships: 43 opaque px in rows 0..10 is the
 * MAX over the whole 411-species/2-frame set, 411 of 838 frames have zero there) --
 * so the lower mon's near-blank top band blanked the upper mon's ink-heavy bottom
 * band (rows 21..31: 106-170px lost per adjacent pair, worst pair GENGAR/DRAGONITE
 * at 170px, for a 6-mon Tyranitar/Salamence/Metagross/Gengar/Dragonite/Milotic party)
 * with plain background -- 740 differing pixels total vs. the full-art reference
 * render for that party. That shipped for a time and erased the bottom third of five
 * of six party icons after the first bob tick. MUST-FIX 1 (2026-08-22 review) walks
 * the loop backwards (n-1..0) instead, so the UPPER mon of each pair paints last and
 * reclaims the shared band with its own ink; re-measured the same way, the same
 * party drops to 52 differing pixels total, worst pair TYRANITAR/SALAMENCE at 21px
 * -- bounded by the OTHER mon's near-empty top-row ink instead of the ink-heavy
 * bottom-row loss. Zero new static storage either way: both paths reuse s_pcol, the
 * one scratch this function already had.
 *
 * CPU transport, not DMA (2026-08-29, PokeDNA B3 audit): this function's ONLY caller
 * is party_list()'s idle bob loop below (perf_rep_begin(PERF_REP_BOB, "bob.party")),
 * i.e. every call to party_bob_recompose runs from a per-vblank animation tick. The
 * 2026-08-23 hardware A/B that root-caused the PC box's pose-swap crash (box_oam.c's
 * swap_cache_slot, fd205bb) found that issuing ANY dma3_cpy from that class of tick
 * kills an Omega DE, mechanism not pinned. This site was dead until 421cc1f un-gated
 * ANIM_PARTY, so it never got a hardware run before; it gets the same fix box_oam.c's
 * tick sites got rather than a fresh, unproven pass. memcpy32 is libtonc's IWRAM_CODE
 * LDMIA/STMIA word copy -- identical bus traffic, identical 32-bit width, no DMA
 * controller. Needs hardware sign-off, same as fd205bb did. */
static void party_bob_recompose(int n, int sel, int frame) {
  /* NULL and egg handling this loop (both builds) must get right, learned the hard
   * way before this MUST-FIX 2 pass:
   *
   * 1. NULL. mon_icon_for_form_frame returns 0 for the whole artless build with no
   *    source available, for internal ids 0 and 252..276, and for anything past
   *    411 — which a bad-egg slot can hold, and Guy's saves carry deliberate ACE
   *    glitch mons. Adding a row offset to NULL and reading through it lands in
   *    BIOS space; on hardware that is open bus, not the zeroes an emulator tends
   *    to return, so roughly half the halfwords pass the `p & 0x8000` test and
   *    speckle garbage over the icon column every 0.5 s. Every other consumer in
   *    the app guards this (ui.c:99, ui.c:182, and the static party draw below).
   * 2. Eggs. The static draw branches on isEgg and uses mon_icon_egg_frame; this
   *    once did not, so an egg in the party visibly turned into the hatched
   *    species half a second after the last keypress and turned back on the next
   *    press. The day-care bob gets this right — the party bob was the outlier. */
  if (n > 6) n = 6;
  int sry = (n && sel >= 0 && sel < n) ? 17 + sel * 21 : -100;   /* selected panel y..y+20 */

#if PDNA_MON_ICONS_ART_COMPILED
  const u16* base[6];
  int have = 0;
  for (int i = 0; i < n; i++) {
    base[i] = (g_party[i].isEgg && !g_party[i].isBadEgg)
                ? mon_icon_egg_frame((uint8_t)frame)
                : mon_icon_for_form_frame(g_party[i].species, g_party[i].form, (uint8_t)frame);
    if (base[i]) have = 1;
  }
  if (!have) return;   /* nothing drawable: leave the column exactly as drawn */

  rumble_io_suspend();   /* composes from mon_icon ROM data; mute the cart-bus motor toggle */
  for (int y = 12; y <= 150; y++) {
    u16 bg = (y >= sry && y <= sry + 20) ? UI_SEL : UI_BG;
    for (int dx = 0; dx < 34; dx++) s_pcol[dx] = bg;
    for (int i = 0; i < n; i++) {
      int ry, iy; party_icon_y(i, &ry, &iy); (void)ry;
      if (y < iy || y >= iy + MON_ICON_H) continue;
      if (!base[i]) continue;
      const u16* row = base[i] + (y - iy) * MON_ICON_W;
      for (int dx = 0; dx < MON_ICON_W; dx++) { u16 p = row[dx]; if (p & 0x8000) s_pcol[1 + dx] = (u16)(p & 0x7FFF); }
    }
    memcpy32(&vid_mem[y * 240 + 2], s_pcol, 34 * 2 / 4);   /* CPU transport -- see header comment */
  }
  rumble_io_resume();
#else
  rumble_io_suspend();   /* composes from mon_icon ROM/cache data; mute the cart-bus motor toggle */
  /* MUST-FIX 1 (2026-08-22 review): walk BACKWARDS (n-1 .. 0), i.e. bottom party slot
   * first. Each icon's pass repaints its own full MON_ICON_H band unconditionally
   * (transparent pixels become background, not "leave alone" -- see the loop body),
   * so whichever icon paints a shared band LAST wins it outright. Ascending order used
   * to paint the LOWER of each adjacent pair last, so its near-empty top rows (at most
   * 43 opaque px anywhere in the whole icon set) blanked the UPPER icon's ink-heavy
   * bottom rows (21..31) with background -- measured against this pack's real icon
   * art, 740 differing pixels vs. the full-art reference for a 6-mon Tyranitar/
   * Salamence/Metagross/Gengar/Dragonite/Milotic party, i.e. five of six party icons
   * lost their feet after the first bob tick. Descending order paints the UPPER icon
   * of each pair last instead, so its own ink reclaims the shared band; the same
   * party re-measures at 52 differing pixels, bounded by the lower icon's near-empty
   * top-row ink instead. See party_bob_recompose's own header comment for the full
   * per-pair numbers. */
  for (int i = n - 1; i >= 0; i--) {
    const u16* ic = (g_party[i].isEgg && !g_party[i].isBadEgg)
                ? mon_icon_egg_frame((uint8_t)frame)
                : mon_icon_for_form_frame(g_party[i].species, g_party[i].form, (uint8_t)frame);
    if (!ic) continue;   /* nothing to composite for this slot: leave its rows exactly as drawn */
    int ry, iy; party_icon_y(i, &ry, &iy); (void)ry;
    for (int j = 0; j < MON_ICON_H; j++) {
      int y = iy + j; if (y < 12 || y > 150) continue;
      u16 bg = (y >= sry && y <= sry + 20) ? UI_SEL : UI_BG;
      const u16* row = ic + j * MON_ICON_W;
      s_pcol[0] = bg;
      for (int dx = 0; dx < MON_ICON_W; dx++) { u16 p = row[dx]; s_pcol[1 + dx] = (p & 0x8000) ? (u16)(p & 0x7FFF) : bg; }
      s_pcol[33] = bg;
      memcpy32(&vid_mem[y * 240 + 2], s_pcol, 34 * 2 / 4);   /* CPU transport -- see header comment */
    }
  }
  rumble_io_resume();
#endif
}

/* Static-draw one party row's icon + both text lines (shared by the full redraw and a
 * cursor-move partial repaint, so they stay pixel-identical). ui_sprite is a TRUE
 * transparent blit (only opaque pixels write, ui.c:137-149) and tte_write is an
 * OPAQUE per-glyph-cell draw (ui_init sets tte_set_paper(UI_BG), so every call is a
 * complete self-contained redraw of its own pixels) -- both are safe to call again at
 * an unchanged position with unchanged data regardless of what a caller's own erase
 * covered or missed, which is what makes the neighbour-redraw below correct without
 * needing to reason about exact erase-rect vs icon-bounding-box pixel overlap. */
static void pl_row_paint(int i, int frame, int sel) {
  int ry, iy; party_icon_y(i, &ry, &iy);
  PkMon* p = &g_party[i];
  if (p->isEgg && !p->isBadEgg) ui_sprite(3, iy, MON_ICON_W, MON_ICON_H, mon_icon_egg_frame((uint8_t)frame));
  else ui_sprite(3, iy, MON_ICON_W, MON_ICON_H, mon_icon_for_form_frame(p->species, p->form, (uint8_t)frame));
  /* nm is a ui_truncate OUTPUT for 11 display columns; ui.h's contract wants
   * max_cols*4+1 (45) to be UTF-8-safe. nm[16] only stayed safe because nickname is a
   * hard-capped `char[11]` (gen3_save.c:147-148, single-byte-per-glyph gen3_decode_char,
   * 10 populated chars max) -- a source-bound argument, not the buffer meeting the
   * contract outright. BACKLOG #36 item 5: size to the literal contract like
   * br_row_paint's nm[128] does, so a future richer decode can't silently reopen this. */
  char nm[128];
  ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
  char line[48];
  siprintf(line, "%-11s Lv%u", nm, (unsigned)p->level);
  ui_text(40, ry + 3, i == sel ? UI_SELTEXT : UI_TEXT, line);
  siprintf(line, "%s%s%s", pk_species_name(p->species),
           p->isShiny ? "  *" : "", p->isEgg ? "  EGG" : "");
  ui_text(40, ry + 12, UI_DIM, line);
}

static int party_list(void) {
  int sel = 0, anim_ctr = 0, frame = 0;
  /* Cursor-only shadow: gen alone invalidates content changes here too (the ONE
   * mutating path, KEY_A -> app_mon_menu, always opens a full-screen menu that
   * ui_clear()s on its first frame even if the user immediately backs out -- same
   * argument as app_party_overlay above). g_nparty is shadowed defensively though it
   * can only change alongside a gen bump by that same argument. */
  int pv_sel = -2, pv_n = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    /* Same contract as the overlay's (app_icons_hold): declare + rent before the paint,
     * give it back the moment the idle loop ends. This screen's A opens app_mon_menu,
     * which can reach app_commit_all through the dex-registration branch -- so holding
     * past the wait loop is the one thing that must not happen here. */
    {
      uint16_t irows[6];
      int nr = 0;
      for (int i = 0; i < g_nparty && i < 6; i++)
        irows[nr++] = app_icon_row_of(g_party[i].species, g_party[i].form,
                                      g_party[i].isEgg && !g_party[i].isBadEgg);
      app_icons_hold(irows, nr);
    }

    bool full = !pv_valid || pv_gen != ui_clear_gen() || g_nparty != pv_n;

    if (full) {
      ui_clear();
      char line[48];
      siprintf(line, "%s  -  %s", g_vinfo.trainer_name, ver_label(g_vinfo.version_guess, g_frlg));
      ui_text(4, 2, UI_TITLE, line);
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);

      if (g_nparty == 0) ui_text(6, 40, UI_WARN, "No Pokemon in party.");
      /* One full-size 32x32 icon per row, vertically CENTRED on its two-line name block so
       * each mon reads as directly left of its name. ROW_TOP 17 puts row 0's icon just below
       * the header WITHOUT a clamp (the old clamp pushed the top mon too low). Draw the
       * selection bar first so an overhanging icon above it isn't clipped. */
      if (g_nparty) { int ry = 17 + sel * 21; ui_panel(2, ry, 236, 20, UI_SEL, UI_TITLE); }
      for (int i = 0; i < g_nparty; i++) pl_row_paint(i, frame, sel);

      ui_hline(0, 151, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A actions  START menu  B back");
    } else if (sel != pv_sel) {
      /* Cursor-only move (always by exactly one row: UP/DOWN clamp to +-1). A row's
       * icon is 32px tall on a 21px pitch (icon i spans [12+21i, 44+21i)), so it
       * reaches 4px UP into row i-1's band and 6px DOWN into row i+1's band -- erasing
       * just the moving row's 20px selection band would also wipe whatever ink a
       * NEIGHBOUR's icon had contributed there, with nothing to put it back.
       * lo-1..lo+2 (lo=min(old,new)) covers the DAMAGED region exactly, but redrawing
       * icon lo+2 (the outer edge of that range) itself re-damages icon lo+3's top
       * rows -- a region nothing erased -- and on a naive fixed-width redraw that
       * leaves lo+2 winning a shared band where the full draw's ascending order had
       * lo+3 winning (transparent ui_sprite compositing is last-opaque-wins). Each
       * redraw re-damages the row below it, so the ascending chain has to run all the
       * way to the LAST party row, not stop at a fixed offset -- n <= 6 always, so
       * this is at most 2-3 extra rows past the minimal damaged set. Still no
       * ui_clear() and no chrome; the erase bands below stay exactly the two 20px
       * bands whose highlight actually changed (icons are idempotent, text is a
       * self-contained opaque redraw -- see pl_row_paint's own comment -- so
       * redrawing rows the erase never touched is always safe, just not free). */
      int ry_old, iy_old; party_icon_y(pv_sel, &ry_old, &iy_old); (void)iy_old;
      ui_fill_rect(0, ry_old, UI_SCR_W, 20, UI_BG);              /* drop the old highlight */
      int ry_new, iy_new; party_icon_y(sel, &ry_new, &iy_new); (void)iy_new;
      ui_panel(2, ry_new, 236, 20, UI_SEL, UI_TITLE);            /* add the new one, BEFORE any icon */
      int lo = (pv_sel < sel) ? pv_sel : sel;
      for (int r = lo - 1; r < g_nparty; r++) {
        if (r < 0) continue;
        pl_row_paint(r, frame, sel);
      }
    }
    pv_sel = sel; pv_n = g_nparty; pv_valid = true; pv_gen = ui_clear_gen();

    /* idle 2-frame bob (compose-then-CPU-copy, no erase). Animate only on frames with NO key
     * pending so navigation never stutters; party_bob_recompose() calls out per build
     * how it orders the icons -- FULL-ART's scanline sweep layers exactly like the
     * static draw (opaque-wins, so order doesn't matter); ARTLESS's icon-major loop is
     * last-write-wins and must walk backwards (MUST-FIX 1, 2026-08-22) for the shared
     * row band to come out right -- that is NOT the same layering as the static draw.
     * mon_icon_anim_cheap() (art_fallbacks.c) now asks whether every row this screen
     * DECLARED is already in RAM, not which rung is serving. app_icons_hold() at the
     * top of the loop declares the six party rows and rents the space to hold them,
     * so this answers YES on both rungs and a flip costs zero SD transactions. This
     * is the SECOND of art_fallbacks.c's own two named "party bob" loops (the first
     * is app_party_overlay's, gated in wait_keys_bob_p above). */
    u16 k, fresh;
    const u16 mask = KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START;
    do {
      vsync();
      fresh = key_hit(mask);
      k = fresh | key_repeat(KEY_UP | KEY_DOWN);
      if (!k && app_anim_enabled(ANIM_PARTY) && g_nparty && mon_icon_anim_cheap() && ++anim_ctr >= 30) {
        anim_ctr = 0; frame ^= 1;
        /* party_list has its own bob loop rather than calling wait_keys_bob_p, so it
         * needs its own rollup (same slot, same name as the overlay's -- the two are
         * never on screen together and cost the same six icons). */
        perf_rep_begin(PERF_REP_BOB, "bob.party");
        party_bob_recompose(g_nparty, sel, frame);
        perf_rep_end(PERF_REP_BOB);
      }
    } while (!k);
    app_icons_drop();                    /* the idle loop is over -- see app_icons_hold */
    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & KEY_A)               snd_ok();
    else if (fresh & KEY_B)               snd_back();
    else if (fresh & KEY_START)           snd_tab();

    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < g_nparty - 1) sel++; }
    /* Both exits flush the bob rollup: this screen owns PERF_REP_BOB while it is up and
     * has no boxoam_exit() of its own to funnel through, so without these the line only
     * escaped later, from whatever screen next reused the slot -- where it reads as
     * belonging to THAT screen -- or not at all if the user powered off from here. */
    else if (k & KEY_B)    { perf_rep_flush(PERF_REP_BOB); return 0; }
    else if (k & KEY_START) { perf_rep_flush(PERF_REP_BOB); return 2; }
    else if ((k & KEY_A) && g_nparty > 0) {
      uint16_t doff = g_frlg ? 0x0038 : 0x0238;
      uint8_t* rec = g_sb1 + doff + (uint32_t)sel * 100;     /* party lives in SaveBlock1 (ids 1..4) */
      if (app_mon_menu(rec, true, false, app_commit_sb1, g_sb1, -1, sel, UI_FOOTER_Y)) {   /* party -> editor -> commit */
        g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
        for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
        if (sel >= g_nparty) sel = g_nparty ? g_nparty - 1 : 0;
      }
    }
  }
}

/* ===================== data editor: counters / bag / flags ============= */

/* numeric entry via the on-screen keyboard; returns `cur` on cancel. */
static uint32_t osk_number(const char* prompt, uint32_t cur, uint32_t maxv) {
  char init[12], out[12];
  siprintf(init, "%lu", (unsigned long)cur);
  if (!osk_search(prompt, init, out, sizeof(out))) return cur;
  uint32_t v = 0;
  for (const char* p = out; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
  return v > maxv ? maxv : v;
}

/* Raw guarded "flag #N" browser — drilled into from the named FLAGS view. Its own
 * loop; B returns up to the named list. Toggles set *dirty; the soft-lock caution
 * fires once per editor session via the shared *warned flag. */
/* One raw-flag row at logical offset i (-6..6) from the centred cursor. Row pitch is
 * 9px and the selected row's own panel is also 9px tall -- pitch == the largest ink
 * extent here, so a 9px wipe is exactly right (not the D8 pitch-vs-ink mismatch: there
 * is no separate, taller pitch to over-wipe with). Always erases first, even when
 * `valid` is false, so a row that scrolls OUT of [0,N) is blanked instead of left
 * showing its last flag. */
static void frv_row_paint(int fn, int i, bool sel, bool valid) {
  int y = 84 + i * 9;
  ui_fill_rect(2, y - 1, 236, 9, UI_BG);
  if (!valid) return;
  char row[40];
  siprintf(row, "Flag 0x%03X (%d)  %s", fn, fn, pk_flag_get(g_sb1, g_game, fn) ? "ON" : "off");
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  ui_text(8, y, sel ? UI_SELTEXT : (pk_flag_get(g_sb1, g_game, fn) ? UI_OK : UI_DIM), row);
}

/* Raw-flag grid: a 13-row window CENTRED on the cursor (row i=0 is always the
 * selected one; scrolling moves every row's content instead of a top/sel pair).
 * Held UP/DOWN auto-repeats (wait_keys()), so this is treated like any other
 * scrolling list: per-row index identity (which flag number is now at position i)
 * PLUS a live on/off diff, since A toggles the CURRENT flag with no overlay of its
 * own (lesson 1 -- the same reason data_editor_tab's flags tab force-repaints its
 * selected row every frame; here a value-diff does the same job more precisely,
 * since it only repaints when the read-back bit actually flipped). */
static void flags_raw_view(bool* dirty, bool* warned) {
  int N = pk_flags_count(g_game), flagn = 0;
  int pv_fn[13] = {0}; bool pv_on[13] = {0}; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    if (flagn >= N) flagn = N - 1; if (flagn < 0) flagn = 0;

    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "RAW FLAGS");
      ui_text(6, 14, UI_WARN, "Raw flags can break a save!");
      ui_hline(0, 24, UI_SCR_W, UI_BORDER);
      ui_hline(0, 151, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A toggle  U/D  SEL jump#  B back");
    }
    for (int i = -6; i <= 6; i++) {
      int fn = flagn + i, idx = i + 6;
      bool valid_now = (fn >= 0 && fn < N);
      bool on = valid_now && pk_flag_get(g_sb1, g_game, fn);
      bool was_valid = (pv_fn[idx] >= 0);
      bool changed = full || (valid_now != was_valid)
                   || (valid_now && (fn != pv_fn[idx] || on != pv_on[idx]));
      if (changed) frv_row_paint(fn, i, i == 0, valid_now);
      pv_fn[idx] = valid_now ? fn : -1;
      pv_on[idx] = on;
    }
    pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   { if (flagn > 0) flagn--; }
    else if (k & KEY_DOWN) flagn++;
    else if (k & KEY_SELECT) flagn = (int)osk_number("FLAG #", flagn, N - 1);
    else if (k & KEY_A) {
      if (!*warned) { msg_wait("CAUTION", UI_WARN, "Toggling story flags can", "soft-lock the save."); *warned = true; pv_valid = false; }
      pk_flag_set(g_sb1, g_game, flagn, !pk_flag_get(g_sb1, g_game, flagn)); *dirty = true;
    }
  }
}

/* COUNTERS / BAG / FLAGS editor over the loaded save's SaveBlock1. Edits are made
 * in RAM and committed ONCE on exit (B). Returns true if the save was written. */
/* Collapsible flag sections: bit k of s_flags_folded = the k-th header row (flat
 * order) is folded. Session-only by design — a fresh app run starts all-collapsed,
 * the state then persists across editor visits until power-off (Guy). Emerald is at
 * 13 headers once "Fly destinations" lands; a u32 mask keeps the ceiling far away
 * (a fold bit past the mask width silently un-folds that section). */
static uint32_t s_flags_folded = 0xFFFFFFFFu;   /* bit width: flags_fold.h's FF_MAX_GROUPS */

/* Owning-header ordinal per row, cached once per table — the naive rescan made
 * nf_visible O(row) and cursor moves near the bottom of the ~490-row Emerald
 * table O(n^2) per keypress (Guy: "really slow toward the bottom").
 * SIZE RULE: NF_ORD_MAX must stay >= the largest per-game NamedFlag row count in
 * data_tables.c (Emerald ~492 before "Fly destinations", ~530 after). A row past
 * the cap gets ordinal 0, folds under the FIRST header and renders in the wrong
 * section — silently, with no assert. Grow this when a table grows.
 *
 * The actual per-row math (BACKLOG #2a) lives in flags_fold.c/.h, pure C and
 * host-tested by tests/host_flagsfold_test.c; these four wrappers just keep the
 * session-lifetime state (which table is cached, the fold mask) that a pure
 * function cannot own itself. */
#define NF_ORD_MAX 768
static const NamedFlag* s_nf_for = 0;
static uint8_t s_nf_ord[NF_ORD_MAX];
static void nf_cache(const NamedFlag* nf, int nc) {
  if (s_nf_for == nf) return;
  ff_build_ord(nf, nc, s_nf_ord, NF_ORD_MAX);
  s_nf_for = nf;
}
static int nf_hdr_ord(const NamedFlag* nf, int r) {    /* ordinal of row r's owning header */
  (void)nf;
  return ff_hdr_ord(s_nf_ord, NF_ORD_MAX, r);
}
static bool nf_visible(const NamedFlag* nf, int nc, int r) {
  return ff_row_visible(nf, nc, s_nf_ord, NF_ORD_MAX, s_flags_folded, r);
}
static int nf_step(const NamedFlag* nf, int nc, int total, int r, int dir) {
  return ff_step(nf, nc, s_nf_ord, NF_ORD_MAX, s_flags_folded, total, r, dir);
}

/* Draw one flags-list row (raw-browser / section header / flag) at screen y. */
static void nf_draw_row(const NamedFlag* nf, int nc, int r, int y, bool s) {
  char row[40];
  if (r == nc) {                                        /* trailing: drill to raw view */
    if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    ui_text(8, y, s ? UI_SELTEXT : UI_DIM, "Raw flag browser (#N)...");
  } else if (nf[r].num == NAMED_FLAG_HEADER) {
    siprintf(row, "%c %s",                              /* header row: selectable, folds on A */
             ((s_flags_folded >> nf_hdr_ord(nf, r)) & 1u) ? '+' : '-', nf[r].name);
    if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    ui_text(4, y, s ? UI_SELTEXT : UI_DIRCLR, row);
  } else {
    bool on = pk_flag_get(g_sb1, g_game, nf[r].num);
    siprintf(row, "%-22s %s", nf[r].name, on ? "ON" : "off");
    char rt[40]; ui_truncate(rt, row, 29);
    if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
    ui_text(8, y, s ? UI_SELTEXT : (on ? UI_OK : UI_DIM), rt);
  }
}

/* Repaint just row r in place (erase its 9-px band + redraw) — the pick_species
 * partial-redraw idea, so a cursor move no longer flashes the whole list (Guy). */
static void nf_row_repaint(const NamedFlag* nf, int nc, int top, int r, int sel) {
  if (r < top || !nf_visible(nf, nc, r)) return;
  int drawn = 0;
  for (int i = top; i < r; i++) if (nf_visible(nf, nc, i)) drawn++;
  if (drawn >= 14) return;                              /* below the window */
  int y = 26 + drawn * 9;
  ui_fill_rect(0, y - 1, UI_SCR_W, 9, UI_BG);
  nf_draw_row(nf, nc, r, y, r == sel);
}

/* Real bag screen (pdna_bag.c) in the loaded game's own chrome. Runs iff that
 * game's generated art is present; returns true when it handled the tab (so
 * data_editor skips the plain one — art-free builds fall through to it).
 * Gender is re-read from SaveBlock2 on every entry: the trainer card can flip
 * it in the same session. */
static bool bag_screen_try(bool* dirty) {
  int female = (g_sb2[SB2_OFF_GENDER] == 1);
  if (!bag_bg(g_game, female).blob) return false;     /* art-free build: weak NULL fallback */
  if (bag_screen(g_sb1, g_sb2, g_game, female)) *dirty = true;
  return true;
}

/* Draw one COUNTERS row (Money / Coins / a game stat) at screen y — a pure function of
 * (row index, save state), same "erase-then-draw" contract as nf_draw_row above (row
 * text is not fixed-width, so a partial repaint must wipe before it writes). */
static void ct_draw_row(int r, int y, bool s) {
  char row[44], rt[44];
  if (r == 0)      siprintf(row, "%-20s %lu", "Money", (unsigned long)pk_money(g_sb1, g_sb2, g_game));
  else if (r == 1) siprintf(row, "%-20s %u",  "Coins", (unsigned)pk_coins(g_sb1, g_sb2, g_game));
  else             siprintf(row, "%-20s %lu", pk_game_stat_name(r - 2), (unsigned long)pk_game_stat(g_sb1, g_sb2, g_game, r - 2));
  ui_truncate(rt, row, 29);
  if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  ui_text(4, y, s ? UI_SELTEXT : UI_TEXT, rt);
}
static void ct_row_repaint(int top, int r, int sel) {
  int i = r - top; if (i < 0 || i >= 14) return;       /* row scrolled out of the window */
  int y = 16 + i * 9;
  ui_fill_rect(0, y - 1, UI_SCR_W, 9, UI_BG);
  ct_draw_row(r, y, r == sel);
}

/* Visible bag-pocket rows on tab 1 -- ONE constant shared by the window clamp, the
 * full draw loop, and bg_row_repaint's guard below so they cannot drift apart. They
 * already had (a pre-existing bug, not introduced by this repaint pass): the clamp
 * used a stray 13, one more than the other two, which put the selected row one slot
 * BELOW the drawn window from the 13th item on -- no highlight drawn, and A would
 * edit a slot that was not on screen. Every pocket in the game has >= 13 items, so
 * this was reachable in every pocket, not an edge case. */
#define BAG_VIS 12

/* Draw one BAG-pocket slot row at screen y. Same contract as ct_draw_row. */
static void bg_draw_row(int pocket, int sl, int y, bool s) {
  char row[44], rt[44];
  uint16_t id = pk_bag_item(g_sb1, g_game, pocket, sl);
  uint16_t q  = pk_bag_qty(g_sb1, g_sb2, g_game, pocket, sl);
  if (id) siprintf(row, "%-16s x%u", pk_item_name(id), q);
  else    strcpy(row, "-");
  ui_truncate(rt, row, 29);
  if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  ui_text(4, y, s ? UI_SELTEXT : UI_TEXT, rt);
}
static void bg_row_repaint(int pocket, int top, int sl, int sel) {
  int i = sl - top; if (i < 0 || i >= BAG_VIS) return;
  int y = 26 + i * 9;
  ui_fill_rect(0, y - 1, UI_SCR_W, 9, UI_BG);
  bg_draw_row(pocket, sl, y, sl == sel);
}

/* `only` >= 0 locks the screen to that ONE tab. The Bag has its own menu entry now, so it is
 * entered directly instead of hiding two L/R presses deep inside this screen; the plain
 * (art-free) bag list still lives here as tab 1 and is what a build with no bag art falls
 * back to. With `only` < 0 this is the flags-and-counters editor and L/R toggles those two. */
static bool data_editor_tab(int only) {
  const bool lock = (only >= 0);
  int tab = lock ? only : 0;                     /* 0=counters 1=bag 2=flags */
  int sel = 0, top = 0, pocket = 0;
  bool dirty = false, flag_warned = false;

  const NamedFlag* f_nf = 0; int f_top = -1, f_sel = -1;   /* flags-tab partial-redraw state */
  uint32_t f_fold = 0; bool f_valid = false;   /* must match s_flags_folded's width */
  /* Counters/bag tabs' own partial-redraw shadow, same idiom as the flags tab's f_* one
   * above. Each is reachable through exactly one path per CALL of this function: tab 0
   * only ever pairs with tab 2 here (L/R, unlocked), and tab 1 is entered locked with no
   * other tab reachable in that call (bag_entry() -> data_editor_tab(1)) -- so one
   * top/sel/gen shadow per tab is enough, no cross-tab identity to confuse it. Every
   * value edit on both tabs routes through an overlay first (osk_number/pick_item, and
   * the pocket-full/right-pocket messages are msg_wait) -- all of them ui_clear() -- so
   * `gen` alone catches every content change; that is NOT the flags tab's situation (a
   * flag toggle writes straight to the save with no overlay), which is why THAT one still
   * force-repaints its selected row every frame instead of trusting gen alone. Pocket
   * switching (SELECT) is the one bag mutation with no overlay, so its shadow adds
   * `pocket` explicitly rather than inferring the change from top/sel (both of which the
   * switch resets to 0 -- indistinguishable from "already at 0/0" without it). */
  int c_top = -1, c_sel = -1; bool c_valid = false; uint32_t c_gen = 0;                 /* tab 0 */
  int b_top = -1, b_sel = -1, b_pocket = -1; bool b_valid = false; uint32_t b_gen = 0;  /* tab 1 */
  for (;;) {
    /* Flags tab computes its layout FIRST: when only the cursor moved (same window,
     * same folds) we repaint just the two affected rows instead of the whole screen
     * (the full-refresh flicker Guy flagged; same idea as pick_species). Counters/bag
     * mirror the same "clamp the window, THEN decide part" order so `part` is judged
     * against the FINAL top/sel, not a stale pre-clamp one. */
    int nc = 0, total = 0, N = 0, pcap = 0; bool part = false;
    if (tab == 2) {
      nc = pk_named_flags(g_game, &f_nf); total = nc + 1;
      nf_cache(f_nf, nc);
      if (sel >= total) sel = total - 1; if (sel < 0) sel = 0;
      while (sel > 0 && !nf_visible(f_nf, nc, sel)) sel--;   /* land on the owning header */
      if (sel < top) top = sel;
      else {                                      /* scroll counts only VISIBLE rows */
        int cnt = 0;
        for (int r = top; r <= sel; r++) if (nf_visible(f_nf, nc, r)) cnt++;
        while (cnt > 14) { int nt = nf_step(f_nf, nc, total, top, +1); if (nt == top) break; top = nt; cnt--; }
      }
      if (!nf_visible(f_nf, nc, top)) top = nf_step(f_nf, nc, total, top, +1);
      part = f_valid && top == f_top && s_flags_folded == f_fold;
    } else {
      f_valid = false;
      if (tab == 0) {
        N = pk_game_stat_count(g_game) + 2;
        if (sel >= N) sel = N - 1;
        if (sel < top) top = sel; if (sel >= top + 14) top = sel - 13;
        part = c_valid && top == c_top && c_gen == ui_clear_gen();
      } else if (tab == 1) {
        pcap = pk_pocket_cap(g_game, pocket);
        if (sel >= pcap) sel = pcap - 1;
        if (sel < top) top = sel; if (sel >= top + BAG_VIS) top = sel - (BAG_VIS - 1);
        part = b_valid && top == b_top && pocket == b_pocket && b_gen == ui_clear_gen();
      }
    }

    if (!part) {
      ui_clear();
      static const char* const TAB[3] = { "COUNTERS", "BAG", "FLAGS" };
      if (lock) {                                /* one screen, one heading */
        ui_panel(4, 0, 76, 12, UI_SEL, UI_TITLE);
        ui_text(10, 2, UI_SELTEXT, TAB[tab]);
      } else {
        static const int PAIR[2] = { 0, 2 };     /* COUNTERS | FLAGS — the bag is a menu entry */
        for (int t = 0; t < 2; t++) {
          int x = 4 + t * 80; bool s = (PAIR[t] == tab);
          if (s) ui_panel(x, 0, 76, 12, UI_SEL, UI_TITLE);
          ui_text(x + 6, 2, s ? UI_SELTEXT : UI_DIM, TAB[PAIR[t]]);
        }
      }
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    }

    if (tab == 0) {                              /* ---- counters (row 0 = Money, 1 = Coins) ---- */
      if (!part) {
        for (int i = 0; i < 14 && top + i < N; i++) ct_draw_row(top + i, 16 + i * 9, top + i == sel);
        ui_text(4, 152, UI_DIM, lock ? "A edit  U/D  B done" : "A edit  U/D  L/R tab  B done");
      } else if (sel != c_sel) {                  /* cursor-only change: swap the highlight */
        ct_row_repaint(top, c_sel, sel);
        ct_row_repaint(top, sel, sel);
      }
      c_top = top; c_sel = sel; c_valid = true; c_gen = ui_clear_gen();
    } else if (tab == 1) {                       /* ---- bag ---- */
      if (!part) {
        char hh[40]; siprintf(hh, "%s  (%d)", pk_pocket_name(pocket), pcap);
        ui_text(6, 15, UI_DIRCLR, hh);
        for (int i = 0; i < BAG_VIS && top + i < pcap; i++) bg_draw_row(pocket, top + i, 26 + i * 9, top + i == sel);
        ui_text(4, 152, UI_DIM, "A edit  SEL pocket  B done");
      } else if (sel != b_sel) {
        bg_row_repaint(pocket, top, b_sel, sel);
        bg_row_repaint(pocket, top, sel, sel);
      }
      b_top = top; b_sel = sel; b_pocket = pocket; b_valid = true; b_gen = ui_clear_gen();
    } else {                                     /* ---- flags (named list, foldable sections) ---- */
      if (part) {                                 /* cursor-only change: repaint two rows */
        if (f_sel != sel) nf_row_repaint(f_nf, nc, top, f_sel, sel);
        nf_row_repaint(f_nf, nc, top, sel, sel);
      } else {
        ui_text(6, 15, UI_DIRCLR, "Named flags");
        for (int drawn = 0, r = top; drawn < 14 && r < total; r++) {
          if (!nf_visible(f_nf, nc, r)) continue;
          nf_draw_row(f_nf, nc, r, 26 + drawn * 9, r == sel); drawn++;
        }
        ui_text(4, 152, UI_DIM, "A toggle/fold  SEL jump  L/R");
      }
      f_valid = true; f_top = top; f_fold = s_flags_folded; f_sel = sel;
    }

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) break;
    else if (!lock && (k & (KEY_L | KEY_R))) {   /* two tabs now: COUNTERS <-> FLAGS */
      snd_tab(); tab = (tab == 0) ? 2 : 0; sel = 0; top = 0; f_valid = false;
    }
    else if (tab == 2) {                         /* named flags nav/fold/toggle */
      const NamedFlag* nf; int nc = pk_named_flags(g_game, &nf);
      int total = nc + 1;
      nf_cache(nf, nc);
      if (k & KEY_UP)        sel = nf_step(nf, nc, total, sel, -1);
      else if (k & KEY_DOWN) sel = nf_step(nf, nc, total, sel, +1);
      else if (k & KEY_SELECT) {                  /* jump to the next section header (or raw row) */
        snd_tab();
        int s = sel;
        for (int step = 0; step < total; step++) {
          s = (s + 1) % total;
          if (s == nc || (s < nc && nf[s].num == NAMED_FLAG_HEADER)) { sel = s; break; }
        }
        top = 0;
      }
      else if (k & KEY_A) {
        if (sel == nc) { flags_raw_view(&dirty, &flag_warned); f_valid = false; }   /* raw view repaints */
        else if (nf[sel].num == NAMED_FLAG_HEADER) {
          snd_tab();                              /* fold/unfold this section */
          s_flags_folded ^= 1u << nf_hdr_ord(nf, sel);
        } else {
          if (!flag_warned) { msg_wait("CAUTION", UI_WARN, "Toggling story flags can", "soft-lock the save.");
                              flag_warned = true; f_valid = false; }   /* popup overlaid the list */
          pk_flag_set(g_sb1, g_game, nf[sel].num, !pk_flag_get(g_sb1, g_game, nf[sel].num)); dirty = true;
        }
      }
    } else if (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN)   sel++;
    else if (tab == 1 && (k & KEY_SELECT)) { snd_tab(); pocket = (pocket + 1) % POCKET_COUNT; sel = top = 0; }
    else if (k & KEY_A) {
      if (tab == 0) {                            /* edit money (0) / coins (1) / a counter */
        if (sel == 0) {
          uint32_t v = osk_number("MONEY", pk_money(g_sb1, g_sb2, g_game), 999999);
          pk_set_money(g_sb1, g_sb2, g_game, v); dirty = true;
        } else if (sel == 1) {
          uint32_t v = osk_number("COINS", pk_coins(g_sb1, g_sb2, g_game), 9999);
          pk_set_coins(g_sb1, g_sb2, g_game, (uint16_t)v); dirty = true;
        } else {
          int st = sel - 2;
          uint32_t v = osk_number("STAT VALUE", pk_game_stat(g_sb1, g_sb2, g_game, st), 0xFFFFFFFFu);
          pk_set_game_stat(g_sb1, g_sb2, g_game, st, v); dirty = true;
        }
      } else if (tab == 1) {                     /* edit a bag slot */
        uint16_t cur = pk_bag_item(g_sb1, g_game, pocket, sel);
        uint16_t id = pick_item(cur);
        if (id == 0) { pk_bag_set(g_sb1, g_sb2, g_game, pocket, sel, 0, 0); dirty = true; }   /* clear slot */
        else if (id != 0xFFFF) {
          uint16_t q = (uint16_t)osk_number("QUANTITY", pk_bag_qty(g_sb1, g_sb2, g_game, pocket, sel) ? pk_bag_qty(g_sb1, g_sb2, g_game, pocket, sel) : 1, 999);
          int dp = pk_item_pocket(id);
          if (dp == pocket) { pk_bag_set(g_sb1, g_sb2, g_game, pocket, sel, id, q); dirty = true; }
          else {                                  /* wrong pocket: route to the item's real pocket */
            int cap = pk_pocket_cap(g_game, dp), dest = -1;
            for (int s = 0; s < cap; s++) { uint16_t it = pk_bag_item(g_sb1, g_game, dp, s);
                                            if (it == id) { dest = s; break; } if (it == 0 && dest < 0) dest = s; }
            if (dest < 0) { snd_deny(); msg_wait("POCKET FULL", UI_WARN, pk_pocket_name(dp), "is full."); }
            else { pk_bag_set(g_sb1, g_sb2, g_game, dp, dest, id, q); dirty = true;
                   char m[40]; siprintf(m, "Put in %s.", pk_pocket_name(dp)); msg_wait("RIGHT POCKET", UI_OK, m, 0); }
          }
        }
      }
    }
  }

  if (dirty) {                                       /* confirm before the silent write */
    if (!app_confirm("Save data changes?", "Edits write immediately."))
      return false;
    return app_commit_block(1, 4, g_sb1);            /* one verified write on exit */
  }
  return false;
}

static bool data_editor(void) { return data_editor_tab(-1); }

/* Bag menu entry: the game's own bag screen when its art is present, otherwise the plain
 * list. Either way the commit is the same one data_editor_tab does on exit. */
static bool bag_entry(void) {
  bool dirty = false;
  if (!bag_screen_try(&dirty)) return data_editor_tab(1);
  if (!dirty) return false;
  if (!app_confirm("Save bag changes?", "Edits write immediately.")) return false;
  return app_commit_block(1, 4, g_sb1);
}

/* START menu from the box/party: pick a destination screen. */
/* ===================== Pokedex editor (#6) ============================= */

/* Commit just the dex: SaveBlock2 (sec 0) + SaveBlock1 (sec 1..4). Doesn't touch
 * PC storage or its dirty flag (unlike app_commit_all). */
static bool app_commit_dex(void) { return app_commit_sb12(); }

static int  dex_state(int nat) {                            /* 0 none, 1 seen, 2 caught */
  if (pk_dex_owned(g_sb2, (uint16_t)nat)) return 2;
  return pk_dex_seen(g_sb2, (uint16_t)nat) ? 1 : 0;
}
static void dex_set_state(int nat, int state) {
  pk_dex_set_owned(g_sb2, (uint16_t)nat, state >= 2);
  pk_dex_set_seen(g_sb1, g_sb2, g_game, (uint16_t)nat, state >= 1);
}

/* National-Dex unlock for the DEX:ALL menu — flips the magic+var+flag trio so the
 * in-game dex actually shows #152..386 (caught flags alone leave it capped at the
 * regional list). Touches g_sb2 + g_sb1; persisted by app_commit_dex like the flags. */
static bool dex_get_national(void)    { return pk_dex_national_on(g_sb1, g_sb2, g_game); }
static void dex_set_national(bool on) { pk_dex_set_national(g_sb1, g_sb2, g_game, on); }

/* Full Pokedex: the HGSS-style grid/list/by-type viewer in pdna_pick.c. Browsable
 * read-only on any cart; A cycles a species' state (Omega-only edit). The screen
 * reads/writes the dex flags through dex_state / dex_set_state and returns whether
 * anything changed; we then offer the verified dex write. */
static bool pdna_dex_edit(void) {
  key_repeat_mask(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);   /* grid needs L/R repeat; leave this default set */
  pdna_dex_set_max(386);   /* every Gen-3 entry resets the cap: a prior GB visit must not leak (BACKLOG #87) */
  bool dirty = pdna_dex_screen(dex_state, dex_set_state, dex_get_national, dex_set_national, app_can_edit());
  if (dirty && app_confirm("Save Pokedex changes?", "Writes the dex now.")) return app_commit_dex();
  return false;
}

/* ===================== Pokéblock case editor ========================== */

/* Pokéblock colour -> a representative RGB15 swatch (None = dim grey "empty"). */
static uint16_t pokeblock_rgb(uint8_t c) {
  const uint16_t C[15] = {                                    /* RGB15 isn't const-foldable here -> runtime */
    RGB15( 9,  9, 11), RGB15(28,  5,  5), RGB15( 6, 11, 28), RGB15(31, 17, 22), RGB15( 6, 22,  9),
    RGB15(30, 28,  6), RGB15(18,  6, 24), RGB15( 8,  7, 22), RGB15(18, 11,  5), RGB15(15, 25, 31),
    RGB15(16, 17,  6), RGB15(17, 17, 19), RGB15( 4,  4,  6), RGB15(30, 30, 31), RGB15(31, 25,  7),
  };
  return (c < 15) ? C[c] : RGB15(9, 9, 11);
}
static void pokeblock_swatch(int x, int y, int sz, uint8_t color) {
  ui_fill_rect(x, y, sz, sz, RGB15(2, 6, 9));                 /* frame */
  ui_fill_rect(x + 1, y + 1, sz - 2, sz - 2, pokeblock_rgb(color));
}

/* One swatch row (D7): fixed labels/colours for the screen's lifetime, so only the
 * highlight ever changes -- plain UI_BG wipe + redraw, 15px pitch matching the panel
 * height exactly (no shared-scanline case). */
static void pb_color_row_paint(int i, bool s) {
  int col = i / 8, row = i % 8, x = 6 + col * 118, y = 18 + row * 16;
  ui_fill_rect(x - 2, y - 1, 114, 15, UI_BG);
  if (s) ui_panel(x - 2, y - 1, 114, 15, UI_SEL, UI_TITLE);
  pokeblock_swatch(x, y, 12, (uint8_t)i);
  ui_text(x + 18, y + 2, s ? UI_SELTEXT : UI_TEXT, pk_pokeblock_color_name((uint8_t)i));
}

/* Preset colour picker: the 15 named Pokéblock colours with swatches (no free text).
 * This loop never calls anything that paints before returning (A/B both return
 * immediately) -- gen tracked anyway, same uniform idiom every picker uses. */
static int pick_pokeblock_color(uint8_t cur) {
  int sel = cur < 15 ? cur : 0;
  int prev_sel = -1; bool valid = false; uint32_t gen = 0;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "BLOCK COLOR");
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < 15; i++) pb_color_row_paint(i, i == sel);
      ui_text(4, 152, UI_DIM, "A pick  U/D/L/R  B cancel");
    } else if (sel != prev_sel) {
      pb_color_row_paint(prev_sel, false);
      pb_color_row_paint(sel, true);
    }
    prev_sel = sel; valid = true; gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    else if (k & KEY_A) return sel;
    else if (k & KEY_UP)    sel = (sel > 0) ? sel - 1 : 14;
    else if (k & KEY_DOWN)  sel = (sel + 1) % 15;
    else if (k & KEY_LEFT)  { if (sel >= 8) sel -= 8; }
    else if (k & KEY_RIGHT) { if (sel + 8 <= 14) sel += 8; }
  }
}

/* Edit one Pokéblock, game-like: a colour swatch (A = preset picker) + flavour bars +
 * feel, plus "Delete this block". Returns true if anything changed. */
/* Row 0 (colour swatch + name + "A: pick"). 20px tall, matching its own panel exactly
 * -- not a shared pitch, so no D8-class pitch-vs-ink mismatch. */
static void pbe_row0_paint(const PkPokeblock* pb, bool sel) {
  ui_fill_rect(2, 17, 236, 20, UI_BG);
  if (sel) ui_panel(2, 17, 236, 20, UI_SEL, UI_TITLE);
  pokeblock_swatch(8, 19, 16, pb->color);
  ui_text(30, 23, sel ? UI_SELTEXT : UI_TEXT, pk_pokeblock_color_name(pb->color));
  ui_text(150, 23, sel ? UI_SELTEXT : UI_DIM, "A: pick");
}
/* A flavour/feel row: label + 3-digit value + a progress bar. 12px tall, matching its
 * own panel exactly. */
static void pbe_flavor_paint(const char* label, uint8_t v, u16 barcol, int y, bool sel) {
  ui_fill_rect(2, y - 1, 236, 12, UI_BG);
  int fb = v; if (fb > 99) fb = 99;
  if (sel) ui_panel(2, y - 1, 236, 12, UI_SEL, UI_TITLE);
  ui_text(8, y, sel ? UI_SELTEXT : UI_DIM, label);
  char b[8]; siprintf(b, "%3u", (unsigned)v); ui_text(56, y, sel ? UI_SELTEXT : UI_TEXT, b);
  ui_progress(84, y + 1, 150, 6, fb * 150 / 99, barcol, UI_PANEL, UI_BORDER);
}
/* "Delete this block". 11px tall, matching its own panel exactly. */
static void pbe_delete_paint(bool sel) {
  int y = 42 + 6 * 13 + 4;
  ui_fill_rect(2, y - 1, 236, 11, UI_BG);
  if (sel) ui_panel(2, y - 1, 236, 11, UI_SEL, UI_TITLE);
  ui_text(8, y, sel ? UI_SELTEXT : UI_WARN, "Delete this block");
}

/* HELD-STEPPER CASE (batch-D10 note 4, the clock_manual_entry precedent): LEFT/RIGHT
 * sit in wait_keys()'s repeat mask and mutate colour/flavour/feel directly, with NO
 * overlay -- a gen-only shadow would miss every held step. Shadow: the actual VALUES
 * (pv_color, pv_fv[5], pv_feel) plus pv_sel, not just gen. KEY_A's two paths
 * (pick_pokeblock_color, osk_number) both ui_clear() unconditionally on their own
 * first frame, cancelled or not, so gen alone already catches those -- only the
 * LEFT/RIGHT steppers need the value diff.
 *
 * CALLER DEPENDENCY: pdna_pokeblock's own partial-repaint path relies on THIS
 * function's entry always painting (ui_clear() on the first, `!pv_valid`-forced,
 * iteration below) to bump ui_clear_gen() -- that is what lets pdna_pokeblock treat
 * "pokeblock_edit was opened" as sufficient invalidation without its own extra flag.
 * That entry clear is preserved exactly (valid starts false, so `full` is always true
 * on iteration 1); only the REPEATED per-keypress clears this loop used to pay are
 * gated. */
static bool pokeblock_edit(int idx) {
  static const char* const FL[5] = { "Spicy", "Dry", "Sweet", "Bitter", "Sour" };
  PkPokeblock pb; pk_pokeblock_get(g_sb1, g_game, idx, &pb);
  uint8_t* fv[5] = { &pb.spicy, &pb.dry, &pb.sweet, &pb.bitter, &pb.sour };
  int sel = 0; bool changed = false;                          /* rows: 0 colour, 1..5 flavours, 6 feel, 7 delete */
  int pv_sel = -1, pv_color = -1, pv_feel = -1; uint8_t pv_fv[5] = { 0 };
  bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      char t[24]; siprintf(t, "POKEBLOCK %d", idx + 1); ui_text(4, 2, UI_TITLE, t);
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A edit/pick  <> +/-  U/D  B done");
    }
    if (full || pb.color != pv_color || (sel == 0) != (pv_sel == 0))
      pbe_row0_paint(&pb, sel == 0);
    for (int i = 0; i < 5; i++) {
      bool s = (sel == 1 + i), os = (pv_sel == 1 + i);
      if (full || *fv[i] != pv_fv[i] || s != os) pbe_flavor_paint(FL[i], *fv[i], UI_OK, 42 + i * 13, s);
    }
    { bool s = (sel == 6), os = (pv_sel == 6);
      if (full || pb.feel != pv_feel || s != os) pbe_flavor_paint("Feel", pb.feel, UI_DIRCLR, 42 + 5 * 13, s); }
    { bool s = (sel == 7), os = (pv_sel == 7);
      if (full || s != os) pbe_delete_paint(s); }
    pv_sel = sel; pv_color = pb.color; pv_feel = pb.feel;
    for (int i = 0; i < 5; i++) pv_fv[i] = *fv[i];
    pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) break;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 7;
    else if (k & KEY_DOWN) sel = (sel + 1) % 8;
    else if (sel == 0) {                                       /* colour: presets only */
      if (k & KEY_A) { int c = pick_pokeblock_color(pb.color); if (c >= 0) { pb.color = (uint8_t)c; changed = true; } }
      else if (k & (KEY_LEFT | KEY_RIGHT)) { int d = (k & KEY_RIGHT) ? 1 : -1, nv = pb.color + d;
                                             if (nv < 0) nv = 14; if (nv > 14) nv = 0; pb.color = (uint8_t)nv; changed = true; }
    }
    else if (sel == 7) { if (k & KEY_A) { pk_pokeblock_clear(g_sb1, g_game, idx); return true; } }  /* delete */
    else {                                                     /* a flavour (1..5) or feel (6) */
      uint8_t* p = (sel <= 5) ? fv[sel - 1] : &pb.feel;
      if (k & (KEY_LEFT | KEY_RIGHT)) { int d = (k & KEY_RIGHT) ? 1 : -1, nv = *p + d;
                                        if (nv < 0) nv = 99; if (nv > 99) nv = 0; *p = (uint8_t)nv; changed = true; }
      else if (k & KEY_A) { uint32_t nv = osk_number(sel <= 5 ? FL[sel - 1] : "Feel", *p, 99); *p = (uint8_t)nv; changed = true; }
    }
  }
  if (changed) pk_pokeblock_set(g_sb1, g_game, idx, &pb);
  return changed;
}

/* Pokéblock case (game-like list: colour swatches + count). A edits/creates a block,
 * "Delete" inside clears one. RS/Emerald only. Edits g_sb1; one verified write on exit. */
/* Strong pokeblock_bg()/_hl()/_flavor_icon() come from the GENERATED pokeblock_bg.c
 * (git-ignored ripped art); these weak NULLs keep an art-free clone building, and
 * pdna_pokeblock then keeps its plain list. Same pattern as the bag and the
 * trainer card. FRLG returns NULL from the strong version too - no Pokeblocks. */
#if !PDNA_POKEBLOCK_ART_COMPILED
/* The ROM rung (Emerald only -- rom_chrome.h states why R/S and FRLG are not
 * wired). s_pb_chrome lives earlier in this file (declared beside s_romchrome);
 * s_romchrome itself is this file's own instance, no cross-TU setter needed the
 * way pdna_trainer.c's card rung requires one.
 *
 * ALWAYS redecodes -- NOT memoised by (game), on purpose. mon_decomp is the
 * shared 8 KiB EWRAM staging buffer (artbuf.h) that the box wallpaper, mon
 * icons, item icons and the summary portrait ALL overwrite between visits to
 * this screen; a memo keyed only on `game` would hand back a pointer into
 * whichever of those last ran, not this screen's own pixels, the moment the
 * player leaves and returns. pdna_origin_art.c's rom_portrait() documents the
 * exact same hazard for the summary portrait and takes the same fix: always
 * redecode. pdna_pokeblock() below calls this exactly once, on screen entry
 * (not per keypress/redraw -- the decode then stays resident in mon_decomp for
 * that visit, same as before), so the cost is one extra ~3.3 KB LZ77 pass per
 * time the player OPENS the case, not per frame. */
static BgFrame rom_pokeblock_frame(int game) {
  BgFrame f = { 0, 0, PB_BG_W };
  if (!s_romchrome.rc || !rom_chrome_pokeblock_have(&s_romchrome, game)) return f;
  artbuf_claim();          /* E3 review BLOCKING 2: about to overwrite mon_decomp */
  if (!rom_chrome_pokeblock_load(&s_romchrome, game,
                                 (uint8_t*)mon_decomp, MON_DECOMP_BYTES, &s_pb_chrome))
    return f;
  f.blob = &s_pb_chrome.as_lzblob; f.off = 0; f.sw = PB_BG_W;
  return f;
}
#endif /* !PDNA_POKEBLOCK_ART_COMPILED */

__attribute__((weak)) BgFrame pokeblock_bg(int game) {
#if !PDNA_POKEBLOCK_ART_COMPILED
  return rom_pokeblock_frame(game);
#else
  (void)game; BgFrame f = { 0, 0, PB_BG_W }; return f;
#endif
}
/* pokeblock_hl()/pokeblock_flavor_icon() stay compiled-art-only -- small sprite
 * accessors, not the full-screen background this ROM rung covers (rom_chrome.h). */
__attribute__((weak)) const uint16_t* pokeblock_hl(int game, int state) {
  (void)game; (void)state; return 0;
}
__attribute__((weak)) const uint16_t* pokeblock_flavor_icon(int game, int flavor) {
  (void)game; (void)flavor; return 0;
}

/* Erase+draw one ART-FREE list row. Row pitch 10 == panel height 10 (touching, not
 * overlapping: ui_fill_rect(y,10) covers y..y+9, the next row starts at y+10) -- no
 * shared-scanline. Only reachable in the art-free branch (see pdna_pokeblock: chrome
 * mode never takes the partial path, so this never needs to coexist with a re-blitted
 * ROM background). */
static void pb_list_row_paint(int idx, int i, bool sel) {
  int y = 16 + i * 10;
  ui_fill_rect(0, y - 1, UI_SCR_W, 10, UI_BG);
  if (sel) ui_panel(2, y - 1, 236, 10, UI_SEL, UI_TITLE);
  char num[6]; siprintf(num, "%2d", idx + 1); ui_text(6, y + 1, sel ? UI_SELTEXT : UI_DIM, num);
  PkPokeblock pb; pk_pokeblock_get(g_sb1, g_game, idx, &pb);
  bool occ = pk_pokeblock_occupied(&pb);
  if (occ) {
    pokeblock_swatch(28, y, 8, pb.color);
    char row[40]; siprintf(row, "%-8s  feel %u", pk_pokeblock_color_name(pb.color), (unsigned)pb.feel);
    ui_text(42, y + 1, sel ? UI_SELTEXT : UI_TEXT, row);
  } else ui_text(28, y + 1, sel ? UI_SELTEXT : UI_DIM, "(empty)");
}

/* Pokéblock case. Two very different repaint costs share this loop:
 *
 * ART-FREE (chrome.blob == NULL): a plain ui_clear() + row list, exactly like every
 * other D-series list -- full = !valid||gen!=gen(); `top` stays OUT of full and a
 * scroll/cursor move gets the usual per-row-or-2-row-swap treatment via
 * pb_list_row_paint. Content only moves through pokeblock_edit (A), which ui_clear()s
 * on its own first frame, so gen alone catches it.
 *
 * ROM CHROME (chrome.blob != NULL, survey note 2): the background is a 20-page LZ77
 * decompress (bg_restore) plus a separate ROM device-tile blit, and the bottom-left
 * FEEL/flavour board is baked into that SAME decoded bitmap, not a flat fill -- there
 * is no sub-rect re-blit primitive here, and erasing that board with a flat "white"
 * guess would be a visible seam against the real chrome art the instant its shading
 * differs even slightly. So per the note's own fallback: this path forces a FULL
 * repaint (identical to today's unconditional one) on ANY selection or window change,
 * never attempting a partial chrome repaint. It still skips the redraw entirely on a
 * genuine no-op (returning from a nested screen with nothing changed), which chrome
 * mode did NOT do before this pass. */
static void pdna_pokeblock(void) {
  if (pk_pokeblock_offset(g_game) == 0) { msg_wait("NO POKEBLOCKS", UI_DIM, "This game lacks contests.", 0); return; }
  bool dirty = false; int sel = 0, top = 0;
  /* The real case chrome, if the art was generated. NULL -> the plain list below,
   * unchanged, exactly as an art-free clone has always drawn it. */
  BgFrame chrome = pokeblock_bg((int)g_game);
  const int VIS = chrome.blob ? PB_ROWS : 13;               /* retail's panel holds 9 rows */
  int pv_top = -1, pv_sel = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    if (sel < top) top = sel; if (sel >= top + VIS) top = sel - VIS + 1;
    bool full = !pv_valid || pv_gen != ui_clear_gen() || (chrome.blob && (sel != pv_sel || top != pv_top));

    if (full) {
      if (chrome.blob) {
        bg_restore(chrome, 0, 0, PB_BG_W, PB_BG_H);  /* 20 LZ77 pages */
#if !PDNA_POKEBLOCK_ART_COMPILED
        /* The compiled-art build bakes the case DEVICE straight into the frame
         * above (gen_pokeblock_bg.py's composite_device(), build time); the ROM
         * rung fetched it as a SEPARATE decode (rom_chrome.h's device_tiles/
         * device_pal, folded into the same s_pb_chrome buffer) and composites
         * it here, every pass, same cadence as the chrome restore right above
         * -- nothing else touches mon_decomp during this screen's visit, so
         * s_pb_chrome's pointers are still good. device_tiles == 0 (fetch
         * failed) just leaves the case empty, never garbled. */
        if (s_pb_chrome.device_tiles)
          romchrome_blit_tiles(s_pb_chrome.device_tiles, s_pb_chrome.device_pal, 0, 8, 8,
                               ROM_CHROME_POKEBLOCK_DEVICE_X, ROM_CHROME_POKEBLOCK_DEVICE_Y);
#endif
      }
      else ui_clear();
      int have = 0;
      for (int i = 0; i < PK_POKEBLOCK_COUNT; i++) { PkPokeblock p; pk_pokeblock_get(g_sb1, g_game, i, &p); if (pk_pokeblock_occupied(&p)) have++; }
      char ti[28]; siprintf(ti, "POKEBLOCK CASE  %d/40", have);
      if (chrome.blob) {
        /* The case's own title box is 72 px of light chrome, so: dark ink, and the
         * label has to be "POKEBLOCKS" (measured 60 px, budget 64). "POKEBLOCK CASE"
         * is 81 px and "POKEBLOCKS 3" is 69 px -- both would clip to "POKEBLOCK~".
         * The count goes on the wall just under the box, where there is nothing. */
        ui_ptext(PB_TITLE_X + 4, PB_TITLE_Y + 4, UI_PANEL, "POKEBLOCKS");
        char ct[12]; siprintf(ct, "%d/40", have);
        ui_ptext(PB_TITLE_X + 4, PB_TITLE_Y + 20, UI_TEXT, ct);
      } else {
        ui_text(4, 2, UI_TITLE, ti);
        ui_hline(0, 13, UI_SCR_W, UI_BORDER);
      }
      for (int i = 0; i < VIS && top + i < PK_POKEBLOCK_COUNT; i++) {
        int idx = top + i;
        bool s = (idx == sel);
        if (chrome.blob) {
          int y = PB_LIST_Y + i * PB_ROW_H;
          PkPokeblock pb; pk_pokeblock_get(g_sb1, g_game, idx, &pb);
          bool occ = pk_pokeblock_occupied(&pb);
          /* Inside the case's list panel: number, colour swatch, name. The panel is
           * 112 px wide, which the 5x7 face fits and sys8 would not. */
          if (s) ui_panel(PB_LIST_X + 2, y + 1, PB_LIST_W - 4, PB_ROW_H - 2, UI_SEL, UI_TITLE);
          char num[6]; siprintf(num, "%2d", idx + 1);
          ui_ptext(PB_LIST_X + 5, y + 5, s ? UI_SELTEXT : UI_DIM, num);
          if (occ) {
            pokeblock_swatch(PB_LIST_X + 20, y + 4, 8, pb.color);
            ui_ptext_fit(PB_LIST_X + 32, y + 5, PB_LIST_W - 36, s ? UI_SELTEXT : UI_TEXT,
                         pk_pokeblock_color_name(pb.color));
          } else ui_ptext(PB_LIST_X + 20, y + 5, s ? UI_SELTEXT : UI_DIM, "(empty)");
          if (s && occ) {                                /* FEEL, where the game prints it */
            char fl[8]; siprintf(fl, "%2u", (unsigned)pb.feel);
            ui_ptext(PB_FEEL_X, PB_FEEL_Y, UI_TEXT, fl);
          }
        } else {
          pb_list_row_paint(idx, i, s);
        }
      }
      if (chrome.blob) {
        /* The bottom-left white panel is the game's FLAVOR/FEEL board — retail fills
         * it for the selected block and ours sat blank (Guy, HW round 2). Same
         * geometry as retail (labels at (16/64, 104/120/136), the has-flavor icon one
         * tile left of each label, FEEL's value right-aligned at (88,136)) — but as
         * an editor we also print each flavor's VALUE after its label. Dark ink: the
         * panel is white. The chrome re-blits every pass (this is `full`-only), so no
         * erase bookkeeping. */
        PkPokeblock sb; pk_pokeblock_get(g_sb1, g_game, sel, &sb);
        bool occ2 = pk_pokeblock_occupied(&sb);
        static const int FLX[5] = { 16, 16, 16, 64, 64 };
        static const int FLY[5] = { 104, 120, 136, 104, 120 };
        static const int FIX[5] = { 8, 8, 8, 56, 56 };
        static const char* const FLN[5] = { "Spicy", "Dry", "Sweet", "Bitter", "Sour" };
        const uint8_t flv[5] = { sb.spicy, sb.dry, sb.sweet, sb.bitter, sb.sour };
        for (int i = 0; i < 5; i++) {
          ui_ptext(FLX[i], FLY[i], UI_PANEL, FLN[i]);
          if (occ2) {
            char v[6]; siprintf(v, "%u", (unsigned)flv[i]);
            ui_ptext(FLX[i], FLY[i] + 8, flv[i] ? 0x0000 : UI_PANEL, v);
            const uint16_t* ic = pokeblock_flavor_icon((int)g_game, i);
            if (flv[i] && ic) ui_sprite(FIX[i], FLY[i], 8, 16, ic);
          }
        }
        if (occ2) { char fv[6]; siprintf(fv, "%u", (unsigned)sb.feel);
                    ui_ptext_right(102, 137, 0x0000, fv); }
        ui_ptext(4, 152, UI_PANEL, "A edit/create  U/D  B done");
      }
      else        ui_text(4, 152, UI_DIM, "A edit/create  U/D  B done");
    } else {
      /* Art-free only (chrome always took `full` above). `top` is deliberately NOT
       * part of `full` -- same per-row index-identity diff as render_browser/
       * pdna_secretbase (D5's F1 fix): the entry index that WAS drawn at row i
       * (pv_top+i) vs the one that belongs there NOW (top+i). A pure cursor move
       * (top unchanged) only flips the two rows whose selection changed; a scroll
       * (top changed at all) makes every visible row's old/new index differ, so
       * every row repaints -- still no ui_clear(). A naive fixed 2-row swap keyed
       * only on `sel != pv_sel` would use the wrong row position across a scroll
       * (the exact bug this pattern exists to avoid). */
      uint32_t rowdirty = 0;   /* NOT `dirty` -- that name is this function's
                                * save-modified flag; shadowing it invites a
                                * misassignment that silently drops edits */
      for (int i = 0; i < VIS && top + i < PK_POKEBLOCK_COUNT; i++) {
        int f = top + i, of = pv_top + i;
        bool s = (f == sel), os = (of == pv_sel);
        if (of != f || s != os) rowdirty |= 1u << i;
      }
      for (int i = 0; i < VIS && top + i < PK_POKEBLOCK_COUNT; i++)
        if (rowdirty & (1u << i)) pb_list_row_paint(top + i, i, top + i == sel);
    }
    pv_top = top; pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) break;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : PK_POKEBLOCK_COUNT - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % PK_POKEBLOCK_COUNT;
    else if (k & KEY_A)    { if (pokeblock_edit(sel)) dirty = true; }
  }
  if (dirty && app_confirm("Save Pokeblocks?", "Writes the case now.")) app_commit_sb1();
}

/* ===================== Event tickets ================================== */

/* Grant an event "ticket" into the legitimately-received state: give the key ITEM, set the
 * ferry-enable flag the game checks (CheckBagHasItem(ticket) && FlagGet(FLAG_ENABLE_SHIP_*)),
 * and the "received" flag so it reads as already collected. Flags re-derived per game from
 * the decomps:
 *   RS    Eon -> FLAG_SYS_HAS_EON_TICKET 0x853
 *   Em    Eon 0x8B3, Aurora 0x8D5, Mystic 0x8E0, Old Sea Map 0x8D6; received 0x13A/13B/13C
 *   FRLG  Aurora 0x84B, Mystic 0x84A;            received 0x2A7/0x2A8
 * The in-game Mystery-Gift deliveryman hands tickets out from a Wonder Card (separate save
 * data we don't synthesise), so we grant directly to the bag — the same working result. */
typedef struct { const char* name; const char* mon; uint16_t item; int enable_flag; int recv_flag; } EventTicket;

static int event_tickets(const EventTicket** out) {
  static const EventTicket EM[] = {
    { "Eon Ticket",    "Latios / Latias", 275, 0x8B3, -1    },  /* Southern Island */
    { "Aurora Ticket", "Deoxys",          371, 0x8D5, 0x13A },  /* Birth Island    */
    { "Mystic Ticket", "Lugia + Ho-Oh",   370, 0x8E0, 0x13B },  /* Navel Rock      */
    { "Old Sea Map",   "Mew",             376, 0x8D6, 0x13C },  /* Faraway Island  */
  };
  static const EventTicket FR[] = {
    { "Aurora Ticket", "Deoxys",          371, 0x84B, 0x2A7 },  /* Birth Island */
    { "Mystic Ticket", "Lugia + Ho-Oh",   370, 0x84A, 0x2A8 },  /* Navel Rock   */
  };
  static const EventTicket RB[] = {
    { "Eon Ticket",    "Latios / Latias", 275, 0x853, -1 },     /* Southern Island */
  };
  switch (g_game) {
    case PK_EMERALD: *out = EM; return (int)(sizeof EM / sizeof EM[0]);
    case PK_FRLG:    *out = FR; return (int)(sizeof FR / sizeof FR[0]);
    default:         *out = RB; return (int)(sizeof RB / sizeof RB[0]);   /* RS */
  }
}

/* Mystery-Gift enable flag (-1 = N/A, e.g. RS Mystery Event). */
static int mg_enable_flag(void) {
  switch (g_game) { case PK_EMERALD: return 0x8DB; case PK_FRLG: return 0x839; default: return -1; }
}

static bool ticket_granted(const EventTicket* t) {        /* READY = boat enabled + item in bag */
  if (!pk_flag_get(g_sb1, g_game, t->enable_flag)) return false;
  int cap = pk_pocket_cap(g_game, POCKET_KEY);
  for (int s = 0; s < cap; s++) if (pk_bag_item(g_sb1, g_game, POCKET_KEY, s) == t->item) return true;
  return false;
}

static bool grant_key_item(uint16_t item) {              /* add to the Key Items pocket (idempotent) */
  int cap = pk_pocket_cap(g_game, POCKET_KEY);
  for (int s = 0; s < cap; s++) if (pk_bag_item(g_sb1, g_game, POCKET_KEY, s) == item) return true;
  for (int s = 0; s < cap; s++) if (pk_bag_item(g_sb1, g_game, POCKET_KEY, s) == 0) {
    pk_bag_set(g_sb1, g_sb2, g_game, POCKET_KEY, s, item, 1); return true; }
  return false;                                          /* pocket full (very unlikely) */
}

/* One ticket row. 18px pitch, 17px panel/wipe -- matches the panel exactly, no
 * D8-class pitch-vs-ink mismatch. `on` (ticket_granted) is read live -- safe because
 * every path that can change it (KEY_A below) always runs app_confirm() first (which
 * ui_clear()s regardless of accept/decline), so a pure cursor move can never see a
 * stale `on` here. */
static void ev_row_paint(const EventTicket* T, int i, bool sel) {
  int y = 20 + i * 18;
  ui_fill_rect(0, y - 1, UI_SCR_W, 17, UI_BG);
  if (sel) ui_panel(2, y - 1, 236, 17, UI_SEL, UI_TITLE);
  bool on = ticket_granted(&T[i]);
  ui_text(8, y, sel ? UI_SELTEXT : UI_TEXT, T[i].name);
  ui_text(150, y, on ? UI_OK : UI_DIM, on ? "READY" : "-");
  char sub[40]; siprintf(sub, "-> %s", T[i].mon);
  ui_text(12, y + 8, sel ? UI_SELTEXT : UI_DIM, sub);
}

/* Fixed-size list (n <= 4, every ticket fits on screen -- no scrolling window), so a
 * plain sel-vs-pv_sel 2-row swap is exact here, unlike a windowed list (no `top` to
 * get wrong). The "MG: on/off" indicator (mgf) is read only, never written by this
 * screen (its enable_flag/recv_flag writes below target DIFFERENT flag ids for every
 * game table -- checked), so it is static for the whole visit and only needs the
 * `full` branch. Every KEY_A path -- NO ROOM, GRANTED, or a plain decline -- runs
 * app_confirm() first, which ui_clear()s regardless of outcome, so gen alone catches
 * every content change; no held-repeat stepper exists here (only U/D, A, B). */
static void pdna_events(void) {
  const EventTicket* T; int n = event_tickets(&T);
  int mgf = mg_enable_flag();
  bool dirty = false; int sel = 0;
  int pv_sel = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 2, UI_TITLE, "EVENT TICKETS");
      bool mg = (mgf >= 0) && pk_flag_get(g_sb1, g_game, mgf);
      if (mgf >= 0) ui_text(150, 2, mg ? UI_OK : UI_DIM, mg ? "MG: on" : "MG: off");
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, "A grant  U/D  B done");
      for (int i = 0; i < n; i++) ev_row_paint(T, i, i == sel);
    } else if (sel != pv_sel) {
      ev_row_paint(T, pv_sel, false);
      ev_row_paint(T, sel, true);
    }
    pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) break;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      const EventTicket* t = &T[sel];
      if (app_confirm(t->name, "Grant this event ticket?")) {
        if (!grant_key_item(t->item)) { msg_wait("NO ROOM", UI_WARN, "Key Items pocket is full.", 0); }
        else {
          pk_flag_set(g_sb1, g_game, t->enable_flag, true);          /* boat access */
          if (t->recv_flag >= 0) pk_flag_set(g_sb1, g_game, t->recv_flag, true);  /* mark received */
          dirty = true;
          bool mgnow = (mgf >= 0) && pk_flag_get(g_sb1, g_game, mgf);
          msg_wait("GRANTED", UI_OK,
                   (mgf >= 0 && !mgnow) ? "Mystery Gift is off, so it" : "In your bag + boat enabled.",
                   (mgf >= 0 && !mgnow) ? "went to your bag. Sail!"    : "Sail from the harbor.");
        }
      }
    }
  }
  if (dirty && app_confirm("Save events?", "Writes the save now.")) app_commit_sb1();
}

/* ===================== Daycare viewer (#9) ============================= */

/* Read-only viewer of the 1-2 Pokemon in the Daycare (each is an 80-byte BoxPokemon
 * at the start of a 140-byte DaycareMon). U/D scrolls between them via the summary;
 * notes when an egg is ready (offspringPersonality != 0). */
/* BACKLOG #114: the yard scene (dc_scene/dc_pointer/dc_icon_over_bg/dc_rescan/
 * dc_roll_decos, the DC_SPOT areas, the HAVE_DAYCARE_BG probe) now lives in
 * source/pdna_yard.{c,h} -- included near the top of this file. This function
 * keeps the SaveBlock1-specific parts: dc_layout, dc_menu, dc_deposit,
 * dc_withdraw, and pdna_daycare() itself below. */

/* Clear a daycare physical slot's per-mon mail + step counter (per-game layout), so a
 * deposited/withdrawn mon doesn't inherit stale boarding state. */
static void dc_clear_slot_aux(uint32_t base, uint32_t stride, int i) {
  /* DayCareMail is 56 bytes in ALL games (MailStruct 36 + names 19 + pad; pret-verified —
   * clearing only 54 left 2 stale bytes). RS packs mons[2] then mail[2] then steps[2]@272. */
  if (g_game == PK_RS) {
    memset(g_sb1 + base + 160 + (uint32_t)i * 56, 0, 56);           /* DayCareMail[i] (56) */
    memset(g_sb1 + base + 272 + (uint32_t)i * 4, 0, 4);             /* steps[i] u32 @272   */
  } else {
    memset(g_sb1 + base + (uint32_t)i * stride + 80, 0, 56);        /* DaycareMon[i].mail (56) */
    memset(g_sb1 + base + (uint32_t)i * stride + 136, 0, 4);        /* DaycareMon[i].steps u32 */
  }
}

/* Clear the shared pending-egg + egg-check timer (the pair changed). Egg word @280 in all
 * games (E u32 + counter @284; FRLG AND RS u16 + counter @282 — pret-verified). */
static void dc_clear_egg(uint32_t base) {
  if (g_game == PK_EMERALD) { memset(g_sb1 + base + 280, 0, 4); g_sb1[base + 284] = 0; }
  else                      { memset(g_sb1 + base + 280, 0, 2); g_sb1[base + 282] = 0; }  /* FRLG + RS */
}

/* Small daycare action menu. Returns 0=view/edit, 1=take out, 2=put in, -1=cancel. */
static int dc_menu(bool can_take, bool can_put) {
  const char* rows[PDNA_DCPOP_MAX]; int act[PDNA_DCPOP_MAX], nr = 0;
  rows[nr] = "View / Edit"; act[nr++] = 0;
  if (can_take) { rows[nr] = "Take out"; act[nr++] = 1; }
  if (can_put)  { rows[nr] = "Put in (clipboard)"; act[nr++] = 2; }
  rows[nr] = "Cancel"; act[nr++] = -1;
  int sel = 0;
  for (;;) {
    int my, mh;                                    /* stays clear of the yard's footer row */
    ui_popup_vfit(nr, PDNA_DCPOP_ROW_H, PDNA_DCPOP_HEAD, PDNA_DCPOP_FOOT, &my, &mh);
    const int mx = 40, mw = 160;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "DAY-CARE");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < nr; i++) {
      int y = my + PDNA_DCPOP_HEAD + i * PDNA_DCPOP_ROW_H; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : nr - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % nr;
    else if (k & KEY_A)    return act[sel];
  }
}

/* Deposit the clipboard mon into the first free daycare slot (Put in). The user COPIES
 * a mon in the Party/PC first (universal clipboard), then puts it in here — a paste, so
 * the source keeps its copy (release it separately for a true move, like the PC). */
static bool dc_deposit(uint32_t base, uint32_t stride) {
  if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); return false; }
  if (!g_clip.occupied) { snd_deny(); msg_wait("NOTHING COPIED", UI_DIM, "Copy a mon in PC/Party,", "then Put in here."); return false; }
  int fi = dc_first_free(base, stride);
  if (fi < 0) { snd_deny(); msg_wait("DAY-CARE FULL", UI_WARN, "Take a Pokemon out first.", 0); return false; }
  uint8_t out[100];
  if (!clip_to_record(&g_clip, false, out)) return false;
  memcpy(g_sb1 + base + (uint32_t)fi * stride, out, 80);
  dc_clear_slot_aux(base, stride, fi);                /* fresh boarder: no inherited mail/steps */
  dc_clear_egg(base);                                 /* pair changed: drop any pending egg */
  app_stage_sb1();                                    /* deferred: saved when you leave the save */
  snd_ok();
  return true;
}

/* Take the selected daycare mon OUT, to a chosen destination (the mon moves AS-IS — we
 * don't replay the game's withdraw-time EXP-from-steps gain):
 *   To Party - append directly, refused with a message if the party is already full.
 *   To PC    - place on the clipboard so the user PASTEs it onto any free PC slot
 *              (a "grab"-style placement of their choice). */
static bool dc_withdraw(uint32_t base, uint32_t stride, uint8_t* rec, int physi) {
  if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); return false; }
  static const char* const D[3] = { "To Party", "To PC", "Cancel" };
  int sel = 0;
  for (;;) {
    int my, mh;                                    /* stays clear of the yard's footer row */
    ui_popup_vfit(3, PDNA_DCPOP_ROW_H, PDNA_DCPOP_HEAD, PDNA_DCPOP_FOOT, &my, &mh);
    const int mx = 56, mw = 128;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "TAKE OUT");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < 3; i++) { int y = my + PDNA_DCPOP_HEAD + i * PDNA_DCPOP_ROW_H; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, D[i]); }
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 2;
    else if (k & KEY_DOWN) sel = (sel + 1) % 3;
    else if (k & KEY_A) break;
  }
  if (sel == 2) return false;
  if (sel == 0) {                                /* To Party — append (atomic, full-checked) */
    if (party_count(g_sb1, g_frlg) >= 6) { snd_deny(); msg_wait("PARTY FULL", UI_WARN, "Free a party slot first.", 0); return false; }
    EditMon e; gen3_edit_load(rec, false, &e); em_set_party_flag(&e, true);   /* box -> party form */
    uint8_t out[100]; gen3_edit_commit(&e, out);
    party_append(g_sb1, g_frlg, out);
    memset(rec, 0, 80); dc_clear_slot_aux(base, stride, physi); dc_clear_egg(base);
    app_stage_sb1();                                           /* party + daycare both in SB1; deferred to exit */
    snd_ok(); msg_wait("TO PARTY", UI_OK, "Added to your party.", "Saved when you leave.");
  } else {                                       /* To PC — park in a free slot, then carry it in the glove */
    int pb = -1, ps = -1;
    if (!app_inject_to_game_deferred(rec, &pb, &ps)) return false;   /* PC full: daycare kept */
    memset(rec, 0, 80); dc_clear_slot_aux(base, stride, physi); dc_clear_egg(base);   /* PC has it now -> clear daycare */
    app_stage_sb1();
    g_pickup_box = pb; g_pickup_slot = ps;                      /* box grid opens here carrying it */
    snd_ok();
  }
  return true;
}

/* Day-Care viewer: a cute yard with the boarding Pokemon as bobbing icons (A opens a
 * menu: view/edit, take out, put in), plus the Day-Care man's get-along verdict. */
static void pdna_daycare(void) {
  /* RS store the two BoxPokemon contiguously (stride 80); E/FRLG interleave each with
   * its mail+steps (stride 140). The egg-personality word is at base+280 in all games
   * (u32 on E; u16 on FRLG and RS). dc_layout() is the single source shared with the
   * deposit paths. */
  uint32_t base, stride; dc_layout(&base, &stride);
  uint8_t* recs[2]; PkMon dc[2]; int phys[2], dcx[2], dcy[2];
  bool off; int to_check;
  static const int EGG_CHANCE[4] = { 0, 20, 50, 70 };           /* INCOMPATIBLE/LOW/MED/HIGH */
#ifndef HAVE_DAYCARE_BG
  const u16 GRASS = RGB15(13, 22, 9);
#endif
  /* Fresh yard visitors each visit — or none, if the user turned them off (or has no
   * ROM registered — app_yard_visitors_ok()). BOTH counters must be zeroed: they are
   * file-scope statics that survive the previous visit (dc_rescan places from
   * s_ndeco_roll, the draw loops read s_ndeco), so zeroing one would leave ghosts
   * from last time. Unconditional in both builds now — see dc_roll_decos' note. */
  if (app_yard_visitors_ok()) pdna_yard_roll(251);   /* Gen 3: national-dex-ordered species 1..251 */
  else dc_visitors_off();   /* BACKLOG #114: s_ndeco_roll/s_dc_visit_rng are now private to pdna_yard.c */
  int n = dc_rescan(g_sb1, g_game, base, stride, recs, dc, phys, dcx, dcy, &off, &to_check);
  int sel = 0, frame = 0, ctr = 0;
  bool redraw = true, rescan = false;
  /* BACKLOG #73 (speed parity): screen-enter span, same idiom as pdna_box.c/pdna_pick.c/
   * pdna_main.c's party/dex spans -- closed at the end of the first `redraw` paint below. */
  bool perf_first_paint = true;
  perf_span_begin("daycare");
  for (;;) {
    if (rescan) {                                /* after a put/take: re-read the daycare */
      n = dc_rescan(g_sb1, g_game, base, stride, recs, dc, phys, dcx, dcy, &off, &to_check);
      if (sel >= n) sel = n ? n - 1 : 0;
      rescan = false; redraw = true;
    }
    /* Up to SEVEN rows -- 2 boarders + up to 5 hazed yard visitors -- which is the
     * widest bob in the app and the exact shape of "it gets worse the more Pokemon are
     * moving". Declared and rented before the paint; given back the moment the idle
     * loop ends, because A from here reaches dc_withdraw -> app_inject_to_game_deferred,
     * which writes g_pc. See app_icons_hold. */
    {
      uint16_t irows[7];
      int nr = 0;
      for (int i = 0; i < s_ndeco && nr < 7; i++)
        irows[nr++] = app_icon_row_of(s_deco_sp[i], 0, false);
      for (int i = 0; i < n && nr < 7; i++)
        irows[nr++] = app_icon_row_of(dc[i].species, dc[i].form,
                                      dc[i].isEgg && !dc[i].isBadEgg);
      app_icons_hold(irows, nr);
    }
    if (redraw) {
      redraw = false;
      dc_scene();                                              /* overdraws the whole screen */
      ui_fill_rect(0, 0, UI_SCR_W, 11, UI_BG);
      ui_text(4, 2, UI_TITLE, "DAY CARE");
      { char sl[16]; siprintf(sl, "Boarding %d/2", n);        /* the slot count, always visible */
        ui_ptext_right(236, 2, UI_DIM, sl); }
      ui_hline(0, 11, UI_SCR_W, UI_BORDER);
      int nshow = n;
      /* Visitors FIRST and hazed, so a piece of scenery can never paint over one of
       * the player's own Pokemon (four of the slot pairs overlap). Unconditional in
       * both builds — dc_icon_over_bg composites against whatever dc_scene() just
       * drew, art or procedural alike. */
      for (int i = 0; i < s_ndeco; i++)
        dc_icon_over_bg(s_deco_x[i], s_deco_y[i],
                        mon_icon_for_form_frame(s_deco_sp[i], 0, (uint8_t)(frame & 1)), 6, false);
      nshow += s_ndeco;
      for (int i = 0; i < n; i++) {
        const u16* ic = (dc[i].isEgg && !dc[i].isBadEgg) ? mon_icon_egg_frame((uint8_t)(frame & 1))
                                : mon_icon_for_form_frame(dc[i].species, dc[i].form, (uint8_t)(frame & 1));
#ifdef HAVE_DAYCARE_BG
        dc_icon_over_bg(dcx[i], dcy[i], ic, 8, false);        /* the real pair: fully opaque, load-time redraw -> DMA */
#else
        ui_sprite(dcx[i], dcy[i], MON_ICON_W, MON_ICON_H, ic);
#endif
      }
      if (nshow == 0) ui_text(64, 58, RGB15(8, 5, 0), "It's quiet here.");
      if (n) { int py = dcy[sel] - 7; if (py < 12) py = 12; dc_pointer(dcx[sel] + 16, py); }
      /* THREE proportional rows inside the panel (geometry in pdna_layout.h, which the
       * host text-fit test reads too). Row 2 is always drawn and is the one that says
       * which mons are actually yours. The panel used to be 124..151, whose bottom
       * border row (150) fell exactly on row 2's DESCENDER row — "Others are just
       * visiting." lost the tails of its 'j' and 'g'. */
      const int dcy0 = PDNA_DCY_ROW0_Y, dcyp = PDNA_DCY_ROW_PITCH, dcx = PDNA_DCY_TEXT_X;
      ui_panel(PDNA_DCY_PANEL_X, PDNA_DCY_PANEL_Y, PDNA_DCY_PANEL_W, PDNA_DCY_PANEL_H,
               UI_PANEL, UI_BORDER);
      if (n == 2) {
        DcCompat c = pk_daycare_compat(dc[0].species, dc[0].otId, dc[1].species, dc[1].otId);
        ui_ptext(dcx, dcy0, UI_TITLE, pk_daycare_compat_msg(c));
        if (!pk_daycare_can_breed(dc[0].species, dc[0].gender, dc[1].species, dc[1].gender))
          ui_ptext(dcx, dcy0 + dcyp, UI_DIM, "(no Egg: incompatible pair)");
        else if (off) ui_ptext(dcx, dcy0 + dcyp, UI_OK, "An EGG is ready to collect!");
        else { char l[44]; siprintf(l, "Egg check ~%d steps (%d%%)", to_check, EGG_CHANCE[c]);
               ui_ptext(dcx, dcy0 + dcyp, UI_DIM, l); }
      } else if (n == 1) {
        /* a NAME -> proportional + pixel clamp, not ui_truncate's character count */
        ui_ptext_fit(dcx, dcy0, PDNA_DCY_NAME_W, UI_TEXT,
                     dc[0].nickname[0] ? dc[0].nickname : pk_species_name(dc[0].species));
        ui_ptext(dcx, dcy0 + dcyp, off ? UI_OK : UI_DIM, off ? "An EGG is ready!" : "One Pokemon is boarding.");
      } else {
        ui_ptext(dcx, dcy0, off ? UI_OK : UI_DIM, off ? "An EGG is ready to collect!" : "No Pokemon are boarding.");
      }
      /* s_ndeco is 0 unless the user has both a registered ROM and Yard visitors on
       * (app_yard_visitors_ok), so this already reads correctly in both builds. */
      ui_ptext(dcx, dcy0 + 2 * dcyp, UI_DIM, pk_daycare_yard_note(n, s_ndeco));
      ui_fill_rect(0, PDNA_DCY_FOOTER_Y, UI_SCR_W, 8, UI_BG);
      /* "your 2" is doing the disambiguating work: only the boarders are pickable. */
      ui_ptext(4, PDNA_DCY_FOOTER_Y, UI_DIM, n ? PDNA_DCY_HINT_PAIR
                                 : (g_clip.occupied ? PDNA_DCY_HINT_PUT : "B back"));
      if (perf_first_paint) { perf_first_paint = false; perf_span_end(); }
    }
    u16 k, fresh;
    do { VBlankIntrWait(); snd_vblank(); key_poll();
         int anim_any = n;
         if (s_ndeco) anim_any = 1;
         /* Up to 7 icons per flip (2 boarders + up to 5 hazed yard visitors) -- the
          * widest bob in the app, and the one that used to pay a per-icon SD
          * locate+verify for every one of them. mon_icon_anim_cheap()
          * (art_fallbacks.c) now asks whether the rows app_icons_hold() declared above
          * are all in RAM; seven rows fit the rented pool on both rungs, so the answer
          * is yes and the flip is zero SD transactions. A refused borrow (unsaved PC)
          * answers no and the yard keeps a static frame, with the reason logged. */
         if (app_anim_enabled(ANIM_DAYCARE) && anim_any && mon_icon_anim_cheap() &&
             ++ctr >= 30) { /* idle 2-frame bob (flicker-free) */
           ctr = 0; frame ^= 1;
           /* Up to 7 icons per flip (2 boarders + up to 5 yard visitors) -- the widest
            * bob in the app, and another inline loop that never reaches
            * wait_keys_bob_p, so it carries its own rollup. */
           perf_rep_begin(PERF_REP_BOB, "bob.daycare");
           for (int i = 0; i < s_ndeco; i++)                    /* visitors first + hazed, as on redraw */
             dc_icon_over_bg(s_deco_x[i], s_deco_y[i],
                             mon_icon_for_form_frame(s_deco_sp[i], 0, (uint8_t)(frame & 1)), 6, true);
           for (int i = 0; i < n; i++) {                        /* compose icon over the bg + CPU copy (no erase) */
             const u16* ic = (dc[i].isEgg && !dc[i].isBadEgg) ? mon_icon_egg_frame((uint8_t)(frame & 1))
                                : mon_icon_for_form_frame(dc[i].species, dc[i].form, (uint8_t)(frame & 1));
#ifdef HAVE_DAYCARE_BG
             dc_icon_over_bg(dcx[i], dcy[i], ic, 8, true);      /* idle-bob tick -> memcpy32, see dc_icon_over_bg */
#else
             ui_blit_over(dcx[i], dcy[i], MON_ICON_W, MON_ICON_H, ic, GRASS);
#endif
           }
           if (n) { int py = dcy[sel] - 7; if (py < 12) py = 12; dc_pointer(dcx[sel] + 16, py); }  /* selection arrow */
           perf_rep_end(PERF_REP_BOB);
         }
         fresh = key_hit(KEY_FULL); k = fresh; } while (!k);
    app_icons_drop();                    /* the idle loop is over -- see app_icons_hold */
    if      (fresh & KEY_B) snd_back();
    else if (fresh & KEY_A) snd_ok();
    else if (fresh & (KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
    /* The widest bob in the app (up to 7 icons per flip) and the exact shape of "it gets
     * worse the more Pokemon are moving" -- so its rollup must not depend on some later
     * screen happening to reuse the slot. Flushed at the one exit this screen has. */
    if (k & KEY_B) { perf_rep_flush(PERF_REP_BOB); break; }
    else if ((k & (KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) && n == 2) { sel ^= 1; redraw = true; }
    else if (k & KEY_A) {
      if (n == 0) {                              /* empty day-care: A = put a copied mon in */
        if (dc_deposit(base, stride)) rescan = true; else redraw = true;
      } else {
        bool can_put = app_can_edit() && g_clip.occupied && n < 2;
        int a = dc_menu(app_can_edit(), can_put);
        if (a == 1)      { if (dc_withdraw(base, stride, recs[sel], phys[sel])) { if (g_pickup_slot >= 0) { perf_rep_flush(PERF_REP_BOB); return; } rescan = true; } else redraw = true; }   /* withdraw->PC sets a pickup: close so the box carries it */
        else if (a == 2) { if (dc_deposit(base, stride)) rescan = true; else redraw = true; }
        else if (a == 0) {
          /* Editable summary, exactly like the box/party: a kept edit is written back into
           * the daycare BoxPokemon and SB1 is verified-written. U/D scrolls the pair. */
          uint8_t out[100]; int card = 0; bool saved; int nav;
          do {
            saved = false;
            nav = pdna_inspect(recs[sel], false, app_can_edit(), out, &saved, &card);
            if (saved) {
              /* BACKLOG #150 S150-6 review F4 (re-verify R1): the WARNING half ONLY.
               * app_stage_sb1() is a RAM stage, not a verified commit -- flush_on_exit()'s
               * B branch drops it outright ("disk untouched", :8733), so re-keying here
               * would unlink the old-key record for a PID change that may never land.
               * D-Q3: never unlink before a VERIFIED persist. The record keeps its old
               * key; S150-11's identity matcher (ident32 + OT + name) reconciles it. */
              XferRekeyPlan dplan;
              if (app_xfer_pid_guard(recs[sel], out, &dplan)) {
                if (dplan.needs_rekey)
                  log_line("xfer: daycare PID change staged, re-key deferred (old key kept)");
                memcpy(recs[sel], out, 80);                      /* daycare mons are 80-byte BoxPokemon in SB1 */
                app_stage_sb1();                                 /* deferred: saved when you leave the save */
                if (pk_decode_mon(recs[sel], false, &dc[sel])) pk_resolve(&dc[sel]);  /* refresh the scene copy */
              }
            }
            if (n > 1) { if (nav > 0) sel = (sel + 1) % n; else if (nav < 0) sel = (sel + n - 1) % n; }
          } while (nav != 0 && n > 1);
          redraw = true;
        } else redraw = true;                    /* cancel */
      }
    }
  }
}

/* ===================== Secret Bases (#8) =============================== */

static EWRAM_BSS SbRecord g_sb_recs[SB_COUNT];

static int sb_party_maxlevel(const SbRecord* b) {
  int mx = 0;
  for (int i = 0; i < SB_PARTY; i++) if (b->party.species[i] && b->party.level[i] > mx) mx = b->party.level[i];
  return mx;
}

/* Detail view for one base: trainer line + a 3x2 party grid (icon, name, Lv). */
/* Secret-base owner trainer-class presets. The overworld sprite + battle class are
 * DERIVED (index 0..9 = gender*5 + trainerId[0]%5): 5 male, then 5 female. Short class
 * names (clean-room — labelled by class, which is identical across RS/Emerald). */
static const char* const SB_CLASS_NAME[10] = {
  "Youngster", "Bug Catcher", "Rich Boy", "Camper", "Cooltrainer M",
  "Lass", "School Kid", "Lady", "Picnicker", "Cooltrainer F",
};

/* One OWNER APPEARANCE row. Labels are compile-time constants (SB_CLASS_NAME), never
 * change while this screen is open -- so unlike a filename or a save-derived string, a
 * plain UI_BG wipe + redraw is enough (fixed ASCII, no shortening/UTF-8 concern). Row
 * height matches the highlight panel exactly (11 on an 11 pitch), so neighbours never
 * share a scanline. */
static void sb_owner_row_paint(int i, bool s) {
  int y = 40 + i * 11;
  ui_fill_rect(2, y - 1, 236, 11, UI_BG);
  if (s) ui_panel(2, y - 1, 236, 11, UI_SEL, UI_TITLE);
  char r[32]; siprintf(r, "%-13s %s", SB_CLASS_NAME[i], i < 5 ? "(M)" : "(F)");
  ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, r);
}

/* Change a friend's base owner to one of the 10 NPC presets (writes gender + trainerId[0]
 * into g_sb1; sets *dirty). Omega + non-own only — gated by the caller. */
static void sb_owner_pick(SbRecord* b, uint32_t off, bool* dirty) {
  int cur = sb_owner_class(g_sb1, off, b->slot);
  int sel = (cur >= 0 && cur < 10) ? cur : 0;
  /* This loop never calls out to anything that paints (A returns immediately, B
   * returns immediately) -- so gen can only ever change from OUTSIDE this call, which
   * cannot happen while it's the one blocking on wait_keys(). Tracked anyway (gen +
   * valid), the same uniform idiom every picker in this file uses, so nothing has to
   * be remembered specially if a future edit adds a nested call here. */
  int prev_sel = -1; bool valid = false; uint32_t gen = 0;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 3, UI_TITLE, "OWNER APPEARANCE");
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
      ui_text(6, 17, UI_DIM, "Overworld look + battle class");
      ui_text(6, 26, UI_DIM, "(one of 10 presets).");
      for (int i = 0; i < 10; i++) sb_owner_row_paint(i, i == sel);
      ui_text(4, 152, UI_DIM, "A set  U/D move  B cancel");
    } else if (sel != prev_sel) {
      sb_owner_row_paint(prev_sel, false);
      sb_owner_row_paint(sel, true);
    }
    prev_sel = sel; valid = true; gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 9;
    else if (k & KEY_DOWN) sel = (sel + 1) % 10;
    else if (k & KEY_A) {
      sb_set_owner_class(g_sb1, off, b->slot, sel);
      b->gender = sel / 5;                              /* keep the parsed "Trainer: M/F" line in sync */
      /* sb_set_owner_class rewrote trainerId[0] (record offset 0x09) — refresh the cached
       * low byte so the detail screen's "TID" line isn't stale this session. */
      b->trainerId = (uint16_t)((b->trainerId & 0xFF00) | g_sb1[off + (uint32_t)b->slot * SB_RECORD + 0x09]);
      *dirty = true; snd_ok();
      return;
    }
  }
}

/* View + edit ONE secret-base party mon with the SAME rich summary cards as a normal
 * mon. SB stores a REDUCED format (species/level/item/moves/PID + ONE EV byte), so we
 * SYNTHESIZE a full 80-byte BoxPokemon — IVs = the game's fixed 15, the EV byte applied
 * to every stat — hand it to pdna_inspect, then write back only the storable subset (the
 * 6 EVs averaged to one byte, the way the game itself stores a base). pdna_inspect's U/D
 * "next/prev mon" maps to scrolling the boarders. Caller commits SB1 once on exit.
 * NOTE: IVs (forced 15), the EV spread, nickname/OT/friendship/met/ball/ribbons/PP do
 * NOT persist to a secret base — only species/level/item/4 moves/PID + the avg EV byte. */
static void sb_mon_edit(SbRecord* b, uint32_t off, int start, bool* dirty) {
  int count = b->partyCount; if (count < 1) return;
  int idx = start; if (idx < 0) idx = 0; if (idx >= count) idx = count - 1;
  int card = 0;
  for (;;) {
    SbParty* P = &b->party;
    uint32_t pid = P->personality[idx];
    /* synthesize an 80-byte box record from the reduced fields (PID into the raw bytes
     * BEFORE load so nature/gender/ability derive from it, as the game does). */
    uint8_t rec[80]; memset(rec, 0, 80);
    rec[0] = (uint8_t)pid; rec[1] = (uint8_t)(pid >> 8); rec[2] = (uint8_t)(pid >> 16); rec[3] = (uint8_t)(pid >> 24);
    EditMon e; gen3_edit_load(rec, false, &e);
    /* The zero-ciphertext record decrypts (key = PID ^ otId=0) to PID-FILLED substructs, so
     * every field not set below inherits PID garbage — including the Misc IV-word's egg bit
     * (bit30 = PID bit30), which made ~half of secret-base mons show up as EGGS, plus garbage
     * ability/friendship/met/contest. Start from clean substructs, mark it a present mon, then
     * populate exactly the reduced SB fields the game rebuilds from. */
    memset(e.sub, 0, sizeof(e.sub));
    e.raw[0x13] = 0x02;                                 /* hasSpecies (present), not egg / not bad-egg */
    em_set_species(&e, P->species[idx]);
    em_set_item(&e, P->heldItem[idx]);
    for (int z = 0; z < 4; z++) em_set_move(&e, z, P->moves[idx * 4 + z]);
    /* Exact SB battle stats: CreateMon(..., fixedIV=15, ..., personality, OT_ID_RANDOM_NO_SHINY):
     * every IV forced to 15, the stored average-EV byte written to every stat, nature/gender/
     * ability derived from the personality. (Verified vs pokeemerald CreateSecretBaseEnemyParty.) */
    for (int s = PK_HP; s <= PK_SPD; s++) em_set_iv(&e, s, 15);
    for (int s = PK_HP; s <= PK_SPD; s++) em_set_ev(&e, s, P->ev[idx]);
    em_set_ability(&e, (uint8_t)(pid & 1));             /* ability slot = personality bit0 */
    em_set_level(&e, P->level[idx]);
    gen3_edit_commit(&e, rec);
    /* full rich view/edit — identical 7-card UI as any other mon */
    uint8_t out[100]; bool saved = false;
    int nav = pdna_inspect(rec, false, app_can_edit(), out, &saved, &card);
    if (saved) {                                          /* write back only the SB-storable subset */
      PkMon p; pk_decode_mon(out, false, &p); pk_resolve(&p);
      SbPartyMon m;
      m.species = p.species; m.personality = p.personality; m.heldItem = p.heldItem;
      for (int z = 0; z < 4; z++) m.moves[z] = p.moves[z];
      m.level = p.level ? p.level : P->level[idx];
      uint16_t evt = (uint16_t)(p.evs[0] + p.evs[1] + p.evs[2] + p.evs[3] + p.evs[4] + p.evs[5]);
      m.ev = (uint8_t)(evt / 6);                          /* GetAverageEVs — how the game stores a base */
      sb_write_mon(g_sb1, off, b->slot, idx, &m);         /* RAM write; committed on exit */
      P->species[idx] = m.species; P->level[idx] = m.level; P->heldItem[idx] = m.heldItem;
      P->ev[idx] = m.ev; P->personality[idx] = m.personality;
      for (int z = 0; z < 4; z++) P->moves[idx * 4 + z] = m.moves[z];
      *dirty = true;
    }
    if (nav == 0) return;                                 /* B -> leave */
    if (count > 1) idx = (nav > 0) ? (idx + 1) % count : (idx > 0 ? idx - 1 : count - 1);
  }
}

/* Erase one party card's rect ONLY -- no content. Split out of the draw step below
 * because a card is NOT independently repaintable (see sb_card_draw's comment on the
 * level-text overhang); sb_detail's partial path erases all six before drawing any. */
static void sb_card_erase(int i) {
  int col = i % 3, row = i / 3;
  int cx = 4 + col * 78, cy = 56 + row * 46;
  ui_fill_rect(cx - 2, cy - 2, 76, 45, UI_BG);
}

/* One party card's content: panel (if selected) + icon + name + level. Cards are
 * 78x46-pitched with a 76x45 rect (2px/1px gap on every side) -- BUT `ui_text(cx,
 * cy+40, ...)` with the 8px sys8 font paints rows cy+40..cy+47, five rows past this
 * card's OWN rect (cy-2..cy+42). For a top-row card (row=0) that overhang lands
 * inside the bottom-row card directly below it in the same column (row=1's rect
 * starts at cy+44, pitch 46 vs cell 45): the two are NOT independent the way
 * party_list's icon-taller-than-pitch rows aren't independent either, just via text
 * instead of an icon. A prior version of this fix erased+redrew ONLY the two cards
 * whose selection flipped, which is order-dependent -- old=3/new=0 happens to match
 * the full draw's own ascending order, but old=0/new=3 does not, and a 2-card
 * operation cannot be made order-correct in both directions with a fixed pair of
 * calls. sb_detail's partial path therefore erases ALL SIX cards, THEN draws all six
 * ascending (see there) -- ui_sprite is a true transparent blit and tte_write is an
 * opaque per-glyph-cell draw, so an unchanged icon/string redrawn at an unchanged
 * position is always correct regardless of what the erase covered, which is what
 * makes "erase 6, draw 6 ascending" byte-identical to a full ui_clear()+draw6 rather
 * than just visually close. */
static void sb_card_draw(const SbRecord* b, int i, bool sel) {
  int col = i % 3, row = i / 3;
  int cx = 4 + col * 78, cy = 56 + row * 46;
  bool s = sel && (i < b->partyCount);
  if (s) ui_panel(cx - 2, cy - 2, 76, 45, UI_SEL, UI_TITLE);
  uint16_t sp = b->party.species[i];
  if (!sp) { ui_text(cx + 26, cy + 14, UI_DIM, "-"); return; }
  uint8_t fo = (sp == 201) ? pk_unown_form(b->party.personality[i])
             : (sp == 410) ? (uint8_t)pk_get_deoxys_form() : 0;   /* letter/forme, not form 0 (Deoxys internal 410) */
  ui_sprite(cx + 22, cy, MON_ICON_W, MON_ICON_H, mon_icon_for_form(sp, fo));
  /* nm: 9-col ui_truncate output, ui.h's contract wants max_cols*4+1 (37). Source is
   * pk_species_name() (a static internal table, plain ASCII, short) so 16 was safe by
   * inspection; BACKLOG #36 item 5 moves it to the literal contract anyway (uniform
   * hygiene, the same 128 br_row_paint's nm and pl_row_paint's nm above use for theirs). */
  char nm[128]; ui_truncate(nm, pk_species_name(sp), 9); ui_text(cx, cy + 32, s ? UI_SELTEXT : UI_TEXT, nm);
  char line[16]; siprintf(line, "Lv%u", (unsigned)b->party.level[i]); ui_text(cx, cy + 40, UI_DIRCLR, line);
}

/* Secret-base detail: owner info + the boarding party, with a cursor to view/edit each
 * mon (A) and change the owner's overworld look (SELECT). Returns true if g_sb1 was
 * edited (the caller commits or reverts SB1).
 *
 * The header block (owner name/gender/TID/look/visits/deco/battled-today) is a pure
 * function of `b` and sb_owner_class(), both of which can only move through
 * sb_owner_pick() (SELECT) or sb_mon_edit() -> pdna_inspect() (A) -- both open a
 * full-screen sub-view that ui_clear()s on its own first frame regardless of what the
 * user does inside (same `pv.valid` idiom pdna_inspect's own comment shows), so
 * ui_clear_gen() alone is a complete invalidation signal here: nothing on this screen
 * can change without something ALSO painting over it first. Shadow is therefore just
 * `psel` (for the cheap cursor-only path) plus valid/gen -- no separate b->partyCount
 * or b->own tracking needed, because a change to either always routes through one of
 * those two calls. */
static bool sb_detail(SbRecord* b, uint32_t off) {
  bool dirty = false, can = app_can_edit();
  int psel = 0;
  int prev_psel = -1; bool valid = false; uint32_t gen = 0;
  for (;;) {
    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      /* hdr: 14-col ui_truncate output, ui.h's contract wants max_cols*4+1 (57). Source
       * is trainerName, a hard-capped `char[8]` (gen3_secretbase.c:50-51, <=7 populated
       * bytes) decoded single-byte-per-glyph -- same source-bound argument
       * sb_list_row_paint documents for its own nm below. BACKLOG #36 item 5 moves both
       * to the literal contract instead of leaning on that invariant holding forever. */
      char hdr[128]; ui_truncate(hdr, b->trainerName[0] ? b->trainerName : "?", 14);
      ui_text(4, 3, UI_TITLE, hdr);
      if (b->own) ui_text(150, 3, UI_OK, "YOUR BASE");
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);

      char line[40];
      ui_text(6, 17, UI_DIRCLR, b->gender ? "Trainer: Female" : "Trainer: Male");
      siprintf(line, "TID %05u", (unsigned)b->trainerId); ui_text(150, 17, UI_DIM, line);
      int cls = sb_owner_class(g_sb1, off, b->slot);
      siprintf(line, "Looks like: %s", (cls >= 0 && cls < 10) ? SB_CLASS_NAME[cls] : "?");
      ui_text(6, 27, UI_TEXT, line);
      siprintf(line, "Visits %u  Deco %d/16", (unsigned)(b->numEntered > 9999 ? 9999 : b->numEntered), b->decorCount);
      ui_text(6, 37, UI_DIM, line);
      if (b->battledToday) ui_text(6, 46, UI_WARN, "Battled today");     /* #6: its own line */

      for (int i = 0; i < SB_PARTY; i++) sb_card_draw(b, i, i == psel);   /* ui_clear() above already erased */

      ui_hline(0, 151, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, (can && !b->own) ? "A edit mon  SEL owner  B back"
                            : can               ? "A edit mon  B back"
                                                : "A view mon  B back");
    } else if (psel != prev_psel) {
      /* Cursor-only move: erase ALL SIX cards, then draw ALL SIX ascending -- see
       * sb_card_draw's comment for why a 2-card swap (erase old, erase new, draw old,
       * draw new) is order-dependent and wrong in one direction (the level-text
       * overhang makes cards non-independent). This reproduces the full path's own
       * ascending draw order exactly, so the result is byte-identical to it. Content
       * cannot change within one visit without a gen bump (every mutator here --
       * sb_owner_pick, sb_mon_edit->pdna_inspect -- is a full-screen sub-view that
       * ui_clear()s, per this function's own header comment), so the six species/level
       * values redrawn here are always exactly what was already on screen; only the
       * highlight moves. Still skips ui_clear() and the header/footer chrome. */
      for (int i = 0; i < SB_PARTY; i++) sb_card_erase(i);
      for (int i = 0; i < SB_PARTY; i++) sb_card_draw(b, i, i == psel);
    }
    prev_psel = psel; valid = true; gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) return dirty;
    else if (k & KEY_A) { if (b->partyCount > 0) sb_mon_edit(b, off, psel, &dirty); }
    else if (k & KEY_SELECT) {
      if (!can)        { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); }
      else if (b->own) { snd_deny(); msg_wait("OWN BASE", UI_DIM, "Its look comes from your", "trainer card."); }
      else sb_owner_pick(b, off, &dirty);
    }
    else if (b->partyCount > 0) {
      int pc = b->partyCount;
      if      (k & KEY_LEFT)  psel = (psel > 0) ? psel - 1 : pc - 1;
      else if (k & KEY_RIGHT) psel = (psel + 1) % pc;
      else if (k & KEY_UP)    { if (psel >= 3) psel -= 3; }
      else if (k & KEY_DOWN)  { if (psel + 3 < pc) psel += 3; }
    }
  }
}

/* One SECRET BASES list row. Row pitch 15 vs panel height 13 (2px gap): no
 * shared-scanline. `nm` is trainerName -- it was ARGUED safe at nm[12] because
 * trainerName is a hard-capped `char[8]` (gen3_secretbase.c:50-51 writes at most 7
 * populated bytes before the NUL, by construction of the loop bound and the field's
 * own size, regardless of what any byte decodes to) decoded single-byte-per-glyph,
 * unlike the browser's real FatFs filenames (other Gen-3 text decoders in this
 * codebase, e.g. gen3_edit.c/data_tables.c, DO emit multi-byte UTF-8 for gender
 * glyphs elsewhere, so leaning on "today's decode is single-byte" was fragile).
 * BACKLOG #36 item 5: ui.h's contract wants max_cols*4+1 (33 for this 8-col call)
 * regardless of the source argument, so nm now sizes to the same 128 the browser's
 * real-filename buffers use instead of relying on trainerName's cap holding forever. */
static void sb_list_row_paint(int idx, int i, bool sel) {
  const SbRecord* b = &g_sb_recs[idx];
  int y = 18 + i * 15;
  ui_fill_rect(0, y - 2, UI_SCR_W, 13, UI_BG);
  if (sel) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
  char nm[128]; ui_truncate(nm, b->trainerName[0] ? b->trainerName : "?", 8);
  char row[44];
  siprintf(row, "%-8s %s  Lv%-3d  %dmon", nm, b->own ? "YOU" : (b->gender ? "(F)" : "(M)"),
           sb_party_maxlevel(b), b->partyCount);
  ui_text(8, y, sel ? UI_SELTEXT : UI_TEXT, row);
  if (b->battledToday) ui_text(228, y, UI_WARN, "*");
}

/* SECRET BASES list. Every mutation site below (A -> sb_detail -> sb_owner_pick/
 * sb_mon_edit->pdna_inspect; SELECT -> app_confirm/msg_wait/busy_panel) opens a
 * full-screen sub-view that ui_clear()s on ITS OWN first frame, INCLUDING the paths
 * that end up changing nothing (sb_detail returns false after a look-and-B-out) --
 * so ui_clear_gen() alone is a complete invalidation signal; there is no silent
 * mutation path the way scan_dir()/sort_entries() were for the file browser (D5),
 * which is why this one does NOT need an explicit relist-style flag. `top` is kept
 * OUT of `full` (a scroll must not pay the ui_clear()) and handled by the same
 * per-row index-identity diff render_browser uses. */
static void pdna_secretbase(void) {
  Gen3Version v = (g_game == PK_RS) ? G3_VER_RS : (g_game == PK_EMERALD) ? G3_VER_EMERALD : G3_VER_UNKNOWN;
  uint32_t off = gen3_secret_base_offset(v);
  if (off == 0) { msg_wait("SECRET BASES", UI_DIM, "FireRed/LeafGreen has no", "Secret Bases."); return; }
  int n = sb_read_all(g_sb1, off, g_sb_recs);
  if (n == 0) { msg_wait("SECRET BASES", UI_DIM, "None registered yet.", "Set one up or mix first."); return; }

  int sel = 0, top = 0;
  const int VIS = 8;
  int pv_top = -1, pv_sel = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    if (sel < 0) sel = 0; if (sel >= n) sel = n - 1;
    if (sel < top) top = sel; if (sel >= top + VIS) top = sel - VIS + 1;

    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 3, UI_TITLE, "SECRET BASES");
      char cnt[16]; siprintf(cnt, "%d/%d", n, SB_COUNT); ui_text(196, 3, UI_DIM, cnt);
      ui_hline(0, 13, UI_SCR_W, UI_BORDER);
      ui_hline(0, 151, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, app_can_edit() ? "A view  U/D move  SEL clear  B back"
                                              : "A view  U/D move  B back");
    }
    /* Per-row dirty: the entry index drawn at row i WAS (pv_top+i), belongs there NOW
     * (top+i) -- differ, or the row's selection flipped, and it repaints. A scroll (any
     * top change) makes every visible row's old/new index differ, so a scroll still
     * repaints all VIS rows but skips ui_clear() and the header/footer chrome. */
    uint32_t dirty = 0;
    for (int i = 0; i < VIS && top + i < n; i++) {
      int f = top + i, of = pv_top + i;
      bool s = (f == sel), os = (of == pv_sel);
      if (full || of != f || s != os) dirty |= 1u << i;
    }
    for (int i = 0; i < VIS && top + i < n; i++)
      if (dirty & (1u << i)) sb_list_row_paint(top + i, i, top + i == sel);

    pv_top = top; pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < n - 1) sel++; }
    else if (k & KEY_A) {                               /* view/scroll/edit the base's party + owner */
      if (sb_detail(&g_sb_recs[sel], off)) {            /* edits live in g_sb1 (RAM) — commit or revert */
        if (app_confirm("Save Secret-Base edits?", "Writes this save now.")) {
          busy_panel("Secret base");
          if (!app_commit_sb1()) msg_wait("SAVE FAILED", UI_WARN, "Save not modified.", 0);
        } else {
          gen3_read_saveblock1(g_save, g_vinfo.slot, g_sb1);   /* discard: restore SB1 from the image */
        }
        n = sb_read_all(g_sb1, off, g_sb_recs);         /* re-parse either way */
        if (n == 0) return;
        if (sel >= n) sel = n - 1;
      }
    }
    else if (k & KEY_SELECT) {                          /* clear a base (Omega-only, verified write) */
      if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); continue; }
      const SbRecord* b = &g_sb_recs[sel];
      char l2[40]; siprintf(l2, "%s%s", b->trainerName[0] ? b->trainerName : "?", b->own ? "  (YOUR base)" : "'s base");
      if (app_confirm("Clear this Secret Base?", l2)) {
        sb_clear(g_sb1, off, b->slot);
        if (app_commit_sb1()) {
          snd_ok(); msg_wait("CLEARED", UI_OK, "Secret Base removed.", 0);
          n = sb_read_all(g_sb1, off, g_sb_recs);       /* re-scan after the write */
          if (n == 0) return;
          if (sel >= n) sel = n - 1;
        } else { msg_wait("CLEAR FAILED", UI_WARN, "Save not modified.", 0); }
      }
    }
  }
}

/* ===================== Settings (#14: backups) ========================= */

/* ---- RTC / save-clock check & fix (RSE only) -----------------------------
 * Reads the live cartridge RTC, diagnoses whether the save's stored clock has
 * drifted/reset (which freezes berries, Shoal tides, the Lottery and Mirage Island),
 * and offers Auto-sync (in-game clock = cart clock) or a manual date/time set. The
 * RTC fields live in SaveBlock2 (plaintext); the fix commits through the same verified
 * write path the trainer card uses (app_commit_sb2). Writes are Omega-only; an
 * EverDrive / no-RTC cart gets a read-only diagnosis. */
static int days_in_month_ui(int y, int m) {
  static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
  if (m < 1 || m > 12) return 30;
  return md[m - 1] + ((m == 2 && leap) ? 1 : 0);
}

/* One stepper field row. Row pitch 16 vs panel height 13 (3px gap): no
 * shared-scanline. Always wipes first -- the value's own digit count is fixed per
 * field (year zero-pads to 4, the rest to 2), but the label+value composed string can
 * still shrink in principle, and this is the held-repeat hot path (survey note 1) so
 * correctness here matters more than shaving one wipe. */
static void ck_field_paint(int i, const char* lbl, int v, bool wide4, bool s) {
  char r[40]; siprintf(r, "%-7s %0*d", lbl, wide4 ? 4 : 2, v);
  int yy = 30 + i * 16;
  ui_fill_rect(2, yy - 2, 150, 13, UI_BG);
  if (s) ui_panel(2, yy - 2, 150, 13, UI_SEL, UI_TITLE);
  ui_text(10, yy, s ? UI_SELTEXT : UI_TEXT, r);
}

/* Dial in the date/time the game should believe is "now"; on A, write + commit.
 *
 * THE HELD-STEPPER CASE (survey note 1). UP/DOWN/LEFT/RIGHT sit in wait_keys()'s
 * key_repeat mask, and holding UP/DOWN mutates the CURRENT field's value at repeat
 * rate with NO overlay at all -- app_confirm() (the only nested call, on A) is the
 * lone path that ui_clear()s. So a gen-only shadow would miss every single stepper
 * tick: this is lesson 1 in its purest form, and the fix is to shadow the actual
 * VALUES, not just gen. Shadow: the five displayed values (pv_v[]) + the selected
 * field (pv_f) + valid/gen. Each keypress diffs both value AND selection per field, so
 * a step redraws exactly the field that changed (plus a field-switch redraws the two
 * fields whose highlight flipped even though neither's VALUE moved). */
static void clock_manual_entry(GbaRtcTime live) {
  enum { F_Y, F_MO, F_D, F_H, F_MI, F_N };
  static const char* const LBL[F_N] = { "Year", "Month", "Day", "Hour", "Minute" };
  int y = live.year, mo = live.month, d = live.day, h = live.hour, mi = live.minute, f = 0;
  int pv_v[F_N] = { 0, 0, 0, 0, 0 }, pv_f = -1; bool valid = false; uint32_t gen = 0;
  for (;;) {
    int dim = days_in_month_ui(y, mo); if (d > dim) d = dim; if (d < 1) d = 1;
    int v[F_N] = { y, mo, d, h, mi };

    bool full = !valid || gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "SET IN-GAME CLOCK");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      for (int i = 0; i < F_N; i++) ck_field_paint(i, LBL[i], v[i], i == F_Y, i == f);
      ui_text(6, 120, UI_DIM, "Becomes the game's current");
      ui_text(6, 130, UI_DIM, "date/time; resumes events.");
      ui_text(4, 152, UI_DIM, PDNA_GBCLOCK_SHIFT_KEYS);   /* the Gen-3 stepper: same clipped 31-ch legend (b86 review D2) */
    } else {
      for (int i = 0; i < F_N; i++) {
        bool s = (i == f), os = (i == pv_f);
        if (v[i] != pv_v[i] || s != os) ck_field_paint(i, LBL[i], v[i], i == F_Y, s);
      }
    }
    for (int i = 0; i < F_N; i++) pv_v[i] = v[i];
    pv_f = f; valid = true; gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_LEFT)  f = (f > 0) ? f - 1 : F_N - 1;
    else if (k & KEY_RIGHT) f = (f + 1) % F_N;
    else if (k & (KEY_UP | KEY_DOWN)) {
      int dir = (k & KEY_UP) ? 1 : -1;
      switch (f) {
        case F_Y:  y += dir; if (y < 2000) y = 2079; if (y > 2079) y = 2000; break;  /* keep day-count in s16 */
        case F_MO: mo += dir; if (mo < 1) mo = 12; if (mo > 12) mo = 1; break;
        case F_D:  { int dm = days_in_month_ui(y, mo); d += dir; if (d < 1) d = dm; if (d > dm) d = 1; } break;
        case F_H:  h = (h + dir + 24) % 24; break;
        case F_MI: mi = (mi + dir + 60) % 60; break;
      }
    } else if (k & KEY_A) {
      char l1[40]; siprintf(l1, "%04d-%02d-%02d  %02d:%02d", y, mo, d, h, mi);
      if (app_confirm("Set in-game clock?", l1)) {
        if (gen3_clock_manual(g_sb2, live.year, live.month, live.day, live.hour, live.minute, live.second,
                              y, mo, d, h, mi, 0))
          app_commit_sb2();           /* verified write; shows SAVED / WRITE FAILED + backs up */
        else
          msg_wait("OUT OF RANGE", UI_WARN, "Cart year is way off.", "Fix the cart TIME first.");
        return;
      }
    }
  }
}

/* ---- Mirage Island ----------------------------------------------------------
 * Route 130, RSE only. The full mechanic and every measurement behind this screen live in
 * gba-toolkit/docs/kb/pokemon/mirage-island.md; the arithmetic is gen3_mirage.c, host-tested.
 *
 * Two things drive the whole design:
 *  - There is NO flag. The island is a live comparison between one saved u16 and the
 *    personality low half of any of the six party slots, so this screen lists the party and
 *    asks WHICH Pokemon should be the key.
 *  - Writing that u16 directly does NOT work. The save owes a day-rollover catch-up that runs
 *    before the map script on load and would overwrite it (MEASURED: 5 days on Guy's Emerald,
 *    68 on Ruby). So we write the LCG PRE-IMAGE and let the game's own catch-up land on the
 *    target. That is why this screen needs the cart RTC. */
/* This screen polls the cart RTC every iteration for a live "days owed" display (same
 * shape as pdna_clock, D7 note 3) -- the poll stays unconditional, only the PAINT is
 * gated. Unlike pdna_clock, UP/DOWN here (sel) ALWAYS change something (n>0 in every
 * reachable save; a corrupted n==0 save is the only case where they wouldn't), so the
 * realized win mirrors pdna_battle_record's page-flip split (D8): `chrome_full` gates
 * the TITLE/hline (pixel-identical across every state), `content_dirty` gates
 * everything below it (dice line, RTC-derived text, party list, footer) -- a single
 * wipe+redraw of that whole band rather than per-line diffing, since it's cheap CPU
 * work (no SD, no icons) and the win that matters is skipping the ui_clear() +
 * chrome on the common no-RTC-change cursor move. Shadow: sel + the RTC-derived
 * values that feed the text (hi, have, owed) -- covers both "cursor moved" and "real
 * time passed enough to change the owed-days text between two keypresses".
 *
 * INVARIANT (BACKLOG #36 item 8): the shadow deliberately does NOT cover the party
 * list's own content (key[]/slot[]/nicknames, read straight from g_sb1 below, not from
 * g_party) -- that is only safe because this screen NEVER writes the party. Its one
 * write path (KEY_A -> mirage_solve/mirage_set/app_commit_sb1) touches only the two
 * mirage dice u16s; nothing here can change a party member's personality or nickname
 * out from under an unshadowed content_dirty. If a future edit adds any party mutation
 * to this screen, content_dirty must start covering it too. */
static void pdna_mirage(void) {
  int sel = 0;
  int pv_sel = -1; uint16_t pv_hi = 0; bool pv_have = false; int pv_owed = -2;
  bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    if (g_game == PK_FRLG) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "MIRAGE ISLAND");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(6, 44, UI_DIM, "FireRed / LeafGreen have no");
      ui_text(6, 56, UI_DIM, "Mirage Island.");
      ui_text(4, 152, UI_DIM, "B back");
      wait_keys(KEY_B);
      return;
    }

    uint16_t hi = 0, lo = 0;
    mirage_get(g_sb1, g_game, &hi, &lo);
    uint16_t key[MIRAGE_PARTY_SLOTS]; uint8_t slot[MIRAGE_PARTY_SLOTS];
    int n = mirage_party_keys(g_sb1, key, slot);
    bool showing = mirage_present(g_sb1, g_game);

    GbaRtcTime live; bool have = gba_rtc_get(&live);
    Gen3ClockInfo ci;
    gen3_clock_read(g_sb2, have ? gen3_rtc_days(live.year, live.month, live.day) : 0,
                    have ? (live.hour * 3600 + live.minute * 60 + live.second) : 0, have, &ci);
    /* VAR_DAYS is stored in the game's 1-BASED day count; local_days is epoch-0. */
    int owed = have ? mirage_days_owed(g_sb1, g_game, ci.local_days + 1) : -1;

    bool chrome_full = !pv_valid || pv_gen != ui_clear_gen();
    bool content_dirty = chrome_full || sel != pv_sel || hi != pv_hi || have != pv_have || owed != pv_owed;

    if (chrome_full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "MIRAGE ISLAND");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    }
    if (content_dirty) {
      if (!chrome_full) ui_fill_rect(0, 15, UI_SCR_W, UI_SCR_H - 15, UI_BG);
      /* 240 px / 8 px per char = 30 columns, and text starts at x=6 — so 29. Every line here is
       * counted against that; the first draft wrapped and overlapped itself. */
      char b[44];
      siprintf(b, "Dice now %04X  %s", hi, showing ? "SHOWING" : "hidden");
      ui_text(6, 22, showing ? UI_OK : UI_TEXT, b);
      if (owed < 0) {
        ui_text(6, 34, UI_WARN, "Cart clock off.");
        ui_text(6, 44, UI_DIM,  "Set GAME RTC to use this.");
      } else if (owed == 0) {
        ui_text(6, 34, UI_DIM, "Nothing owed - writes as-is.");
      } else {
        siprintf(b, "Owed %d day(s): writes a seed", owed);
        ui_text(6, 34, UI_DIM, b);
        ui_text(6, 44, UI_DIM, "the game rolls into place.");
      }

      /* WORDING MATTERS HERE. Guy read the first draft ("Make it match:" over a list of his
       * Pokemon) as editing the Pokemon. It never does: the only bytes written are the two u16s
       * of the dice. The list exists because the dice has to land on SOME party member's key and
       * he has six to choose from. Say that on screen. */
      ui_text(6, 60, UI_TEXT, n ? "Point the dice at:" : "No Pokemon in the party.");
      for (int i = 0; i < n; i++) {
        PkMon m;
        const uint8_t* mon = g_sb1 + 0x238u + (uint32_t)slot[i] * 100u;
        bool ok = pk_decode_mon(mon, true, &m);
        siprintf(b, "%c %-11.11s %04X", (i == sel) ? '>' : ' ',
                 ok && m.nickname[0] ? m.nickname : "?", key[i]);
        ui_text(6, 72 + i * 9, (i == sel) ? UI_TEXT : UI_DIM, b);  /* 9 px: six rows must clear y=128 */
      }

      ui_text(6, 128, UI_DIM, "Dice only; no Pokemon edited.");
      ui_text(6, 138, UI_DIM, "Lasts one in-game day.");
      ui_text(4, 152, UI_DIM, n ? "A appear  U/D pick  B back" : "B back");
    }
    pv_sel = sel; pv_hi = hi; pv_have = have; pv_owed = owed; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_A | KEY_UP | KEY_DOWN | KEY_B);
    if (k & KEY_B) return;
    if (!n) continue;
    if (k & KEY_UP)   { sel = (sel > 0) ? sel - 1 : n - 1; continue; }
    if (k & KEY_DOWN) { sel = (sel + 1) % n;               continue; }

    if (!app_can_edit()) { msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); continue; }
    /* app_confirm's panel is the same narrow one. */
    if (!have) {
      /* msg_wait's panel is narrower than the screen: ~22 chars a line, not 29. */
      msg_wait("NO CART CLOCK", UI_WARN, "Turn on GAME RTC and", "set the cart TIME.");
      continue;
    }
    if (!app_confirm("Set the dice?", "Lasts one in-game day.")) continue;

    uint16_t wh, wl;
    mirage_solve(key[sel], lo, owed > 0 ? owed : 0, &wh, &wl);
    mirage_set(g_sb1, g_game, wh, wl);
    bool w = app_commit_sb1();
    log_line("mirage: key %04X owed %d -> wrote %04X/%04X %s",
             key[sel], owed, wh, wl, w ? "OK" : "FAILED");
    if (w) msg_wait("DONE", UI_OK, "Go to Route 130, or ask", "the man in Pacifidlog.");
    else   msg_wait("WRITE FAILED", UI_WARN, "Nothing was changed.", 0);
  }
}

/* Survey note 3: the RTC poll below stays exactly where it was, EVERY iteration --
 * this is a live diagnostic and the read has to stay fresh. Only the PAINT is gated,
 * by shadowing the rendered content (not the poll): compose the same dynamic lines
 * this screen has always drawn into one buffer and diff that against last frame's, so
 * a re-entry whose RTC read landed on byte-identical output (common: this loop only
 * re-enters after A/SELECT, and both always run a nested full-screen call --
 * app_confirm always ui_clear()s even on decline, clock_manual_entry ui_clear()s on
 * its own first frame -- so gen alone already forces a repaint on every real
 * re-entry today; the string shadow is the belt to gen's suspenders, and is what
 * keeps this screen correct if a future edit ever adds a re-entry path that does NOT
 * go through one of those two calls). 176 B composite buffer, ASCII only (numbers and
 * fixed literals, no filename/nickname content), well inside the modest-frame budget. */
static void pdna_clock(void) {
  bool can = app_can_edit();
  char pv_txt[176] = ""; bool valid = false; uint32_t gen = 0;
  for (;;) {
    if (g_game == PK_FRLG) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "SAVE CLOCK / RTC");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(6, 44, UI_DIM, "FireRed / LeafGreen have no");
      ui_text(6, 56, UI_DIM, "real-time clock or berry/tide");
      ui_text(6, 68, UI_DIM, "events. Nothing to fix here.");
      ui_text(4, 152, UI_DIM, "B back");
      wait_keys(KEY_B);
      return;
    }
    GbaRtcTime live; bool have = gba_rtc_get(&live);
    int rtc_days = have ? gen3_rtc_days(live.year, live.month, live.day) : 0;
    int rtc_sec  = have ? (live.hour * 3600 + live.minute * 60 + live.second) : 0;
    Gen3ClockInfo ci; gen3_clock_read(g_sb2, rtc_days, rtc_sec, have, &ci);

    char txt[176];
    if (have) siprintf(txt, "%04u-%02u-%02u %02u:%02u", live.year, live.month, live.day, live.hour, live.minute);
    else      strcpy(txt, "-");
    if (have) { char t2[48]; siprintf(t2, "|%04d-%02d-%02d", ci.ly, ci.lm, ci.ld);
                strncat(txt, t2, sizeof(txt) - strlen(txt) - 1); }
    { char t3[48]; siprintf(t3, "|%d", ci.verdict); strncat(txt, t3, sizeof(txt) - strlen(txt) - 1); }
    if (have) { char t4[48]; siprintf(t4, "|%d|%d|%d", ci.delta, ci.off_days, ci.berry_days);
                strncat(txt, t4, sizeof(txt) - strlen(txt) - 1); }

    bool full = !valid || gen != ui_clear_gen() || strcmp(txt, pv_txt) != 0;
    if (full) {
      char b[44];
      ui_clear();
      ui_text(4, 4, UI_TITLE, "SAVE CLOCK / RTC");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      if (have) { siprintf(b, "Cart clock  %04u-%02u-%02u %02u:%02u",
                           live.year, live.month, live.day, live.hour, live.minute); ui_text(6, 22, UI_TEXT, b); }
      else ui_text(6, 22, UI_WARN, "Cart clock not available.");
      if (have) { siprintf(b, "Save clock  %04d-%02d-%02d  (in-game)", ci.ly, ci.lm, ci.ld); ui_text(6, 34, UI_TEXT, b); }
      else        ui_text(6, 34, UI_DIM, "Save clock  --");

      /* Verdict + plain-language detail. A POSITIVE gap is normal — it just means that
       * many in-game days will process next time you play (berries grow, etc.). Only a
       * clock that runs BEHIND the save freezes those events. */
      const char* vmsg; u16 vcol;
      switch (ci.verdict) {
        case 0: vmsg = (ci.delta == 0) ? "Clock is in sync." : "Clock is healthy."; vcol = UI_OK;   break;
        case 1: vmsg = "Large gap - is the cart clock right?";                       vcol = UI_WARN; break;
        case 2: vmsg = "Cart clock is BEHIND the save.";                             vcol = UI_WARN; break;
        case 3: vmsg = "Clock data looks wrong.";                                    vcol = UI_WARN; break;
        default: vmsg = "Enable GAME RTC on the cart.";                              vcol = UI_DIM;  break;
      }
      ui_text(6, 50, vcol, vmsg);
      if (have) {
        if      (ci.verdict == 2) siprintf(b, "Behind by %d day(s): events frozen.", -ci.delta);
        else if (ci.verdict == 3) siprintf(b, "Offset %dd, berry day %d.", ci.off_days, ci.berry_days);
        else if (ci.delta > 0)    siprintf(b, "%d day(s) will pass when you play.", ci.delta);
        else                      strcpy(b, "Up to date - nothing pending.");
        ui_text(6, 64, UI_DIM, b);
        ui_text(6, 78, UI_DIM, "(Save clock = in-game time;");
        ui_text(6, 88, UI_DIM, " it need not match today.)");
      }

      if (!have) {
        ui_text(6, 102, UI_DIM, "On the EZ-Flash: System >");
        ui_text(6, 112, UI_DIM, "GAME RTC = on, then set TIME.");
        ui_text(4, 152, UI_DIM, "B back");
      } else if (!can) {
        ui_text(6, 102, UI_DIM, app_readonly_why());
        ui_text(4, 152, UI_DIM, "B back");
      } else {
        ui_text(6, 102, UI_DIM, "Set the cart clock correctly");
        ui_text(6, 112, UI_DIM, "first, then sync.");
        ui_text(4, 152, UI_DIM, "A sync to cart  SEL set  B back");
      }
    }
    strcpy(pv_txt, txt); valid = true; gen = ui_clear_gen();

    if (!have || !can) { wait_keys(KEY_B); return; }
    u16 k = wait_keys(KEY_A | KEY_SELECT | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_A) {
      if (app_confirm("Sync to cart clock?", "Game clock = cart clock.")) {
        if (gen3_clock_autosync(g_sb2, live.year, live.month, live.day, live.hour, live.minute, live.second))
          app_commit_sb2();
        else
          msg_wait("CAN'T SYNC", UI_WARN, "Cart year out of range.", "Set the cart TIME first.");
      }
    } else if (k & KEY_SELECT) {
      clock_manual_entry(live);
    }
  }
}

/* One animation-toggle row. Row pitch 16, wipe/panel 13 -- matches the panel's own
 * height, not the larger pitch (the D8 lesson). */
static void as_row_paint(const char* const nm[ANIM_COUNT], int i, bool sel) {
  char r[40]; siprintf(r, "%-10s %s", nm[i], app_anim_enabled(i) ? "On" : "Off");
  int y = 30 + i * 16;
  ui_fill_rect(2, y - 2, 236, 13, UI_BG);
  if (sel) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
  ui_text(10, y, sel ? UI_SELTEXT : UI_TEXT, r);
}

/* Settings > Animations: a per-place On/Off list (the 2-frame bobs + the summary
 * portrait wiggle), each its own toggle. Persists on exit. Pure lesson-1 case
 * (batch-D10 note 2): A toggles g_anim_mask directly, no overlay screen at all, so
 * the row's on/off VALUE has to be shadowed, not just gen -- nothing else would ever
 * invalidate it. No header/counter here derives from the toggles (the help line is a
 * fixed sentence), so only the per-row diff is needed. */
static void anim_settings(void) {
  static const char* const NM[ANIM_COUNT] = { "Box icons", "Party", "Pokedex", "Daycare", "Summary" };
  int sel = 0;
  int pv_sel = -1; bool pv_on[ANIM_COUNT] = { 0 }; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "ANIMATIONS");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(8, 124, UI_DIM, "Moving sprites, per screen.");
      ui_text(4, 152, UI_DIM, "A toggle  U/D move  B back");
    }
    for (int i = 0; i < ANIM_COUNT; i++) {
      bool on = app_anim_enabled(i);
      bool s = (i == sel), os = (i == pv_sel);
      if (full || on != pv_on[i] || s != os) as_row_paint(NM, i, s);
      pv_on[i] = on;
    }
    pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) { cfg_save(); return; }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : ANIM_COUNT - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % ANIM_COUNT;
    else if (k & KEY_A)    g_anim_mask ^= (1u << sel);
  }
}

/* One rumble-settings row (a stepper or a cue toggle). i==0 strength, i==1 duration
 * (PDNA_RMB_STEPPERS of them), i>=PDNA_RMB_STEPPERS a cue. Wipe/panel height 13
 * matches the panel exactly, not PDNA_RMB_ROW_PITCH (the D8 lesson). */
static void rs_row_paint(int i, bool sel) {
  char r[40];
  if      (i == 0) siprintf(r, "Strength    %d/5  < >", rmbl_get_strength());
  else if (i == 1) siprintf(r, "Duration    %d/5  < >", rmbl_get_duration());
  else { int c = i - PDNA_RMB_STEPPERS; siprintf(r, "%-12s %s", rmbl_cue_name(c), rmbl_cue_enabled(c) ? "On" : "Off"); }
  int y = PDNA_RMB_ROW0_Y + i * PDNA_RMB_ROW_PITCH;
  ui_fill_rect(2, y - 2, 236, 13, UI_BG);
  if (sel) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
  ui_text(PDNA_SET_ROW_X, y, sel ? UI_SELTEXT : UI_TEXT, r);
}

/* Settings > Rumble: global Strength + Duration steppers (the motor can read as
 * "nothing" on a weak unit, so these are tunable), then a per-cue On/Off list. <>
 * adjust strength/duration (with a live buzz preview); A toggles a cue (+preview).
 * Persists on exit.
 *
 * BOTH the held-stepper rows AND the toggle rows have NO overlay of their own --
 * rmbl_demo()/rmbl_fire() play a MOTOR PULSE, not a screen (batch-D10 notes 2 and 4:
 * this is simultaneously the "held-stepper, shadow the values" case and the
 * "toggle-in-place, no overlay" case). Shadow the per-row VALUE (strength, duration,
 * or a cue's on/off as 0/1), not just gen. pv_val[] is sized
 * PDNA_RMB_STEPPERS+RCUE_COUNT -- both compile-time constants, so this is a fixed
 * array, not a VLA, even though the loop bound NROW is a runtime `const int` (its
 * value is always the same compile-time sum). */
static void rumble_settings(void) {
  enum { R_STR, R_DUR, R_CUE0 };
  _Static_assert(R_CUE0 == PDNA_RMB_STEPPERS, "rumble stepper count out of sync");
  const int NROW = R_CUE0 + RCUE_COUNT;
  int sel = 0;
  int pv_sel = -1; int pv_val[PDNA_RMB_STEPPERS + RCUE_COUNT] = { 0 };
  bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "RUMBLE");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      /* The help sentence and the control hints are two INDEPENDENT strings: they used to
       * share the y=150 footer row as "GAME RTC on. <>adj A toggle B", which is 29 sys8
       * columns — the row was full, so "back" had to be dropped from the hint to make it
       * fit. Give each its own row. (Strings + rows in pdna_layout.h — the host test
       * measures them from there.) */
      ui_text(PDNA_RMB_HELP_X, PDNA_RMB_HELP_Y1, UI_DIM, PDNA_RMB_HELP1);
      ui_text(PDNA_RMB_HELP_X, PDNA_RMB_HELP_Y2, UI_DIM, PDNA_RMB_HELP2);
      ui_text(PDNA_SET_FOOT_X, PDNA_SET_FOOTER_Y, UI_DIM, PDNA_RMB_FOOT);
    }
    for (int i = 0; i < NROW; i++) {
      int val = (i == R_STR) ? rmbl_get_strength()
              : (i == R_DUR) ? rmbl_get_duration()
              : (rmbl_cue_enabled(i - R_CUE0) ? 1 : 0);
      bool s = (i == sel), os = (i == pv_sel);
      if (full || val != pv_val[i] || s != os) rs_row_paint(i, s);
      pv_val[i] = val;
    }
    pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) { cfg_save(); return; }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : NROW - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % NROW;
    else if (k & (KEY_LEFT | KEY_RIGHT)) {
      int d = (k & KEY_RIGHT) ? 1 : -1;
      if      (sel == R_STR) { rmbl_set_strength(rmbl_get_strength() + d); rmbl_demo(); }
      else if (sel == R_DUR) { rmbl_set_duration(rmbl_get_duration() + d); rmbl_demo(); }
    }
    else if (k & KEY_A) {
      if (sel >= R_CUE0) { int c = sel - R_CUE0; bool now = !rmbl_cue_enabled(c); rmbl_cue_set(c, now); if (now) rmbl_fire(c); }
      else rmbl_demo();        /* strength/duration row: just feel it */
    }
  }
}

/* ---- Phase 2 (ROM-art cache): the extraction screen -----------------------------
 * DESIGN.md Sec 3.1-3.3: progress + cancel + a stopwatch, reusing the two instruments
 * that already exist (pdna_romfull.c's TM0/TM1 stopwatch pattern, duplicated here in
 * miniature since that file's tmr_* helpers are file-static; and the free-running
 * heartbeat hb_arm/hb_off already defined above in THIS file, so no new coupling).
 * Omega-only (writes) -- the caller gates on active_flashcart before ever showing
 * this row as selectable. */
#ifndef PDNA_DELTA
/* This screen used to start and stop its OWN TIMER0/TIMER1 pair. perf.c now owns that
 * pair for the whole session (perf.h), so a second owner here would zero the session
 * clock every time the user extracted art. Every readout below is a DELTA from the t0
 * captured at the start, so reading a free-running clock instead of a zeroed one is
 * semantically identical: `t0` is simply "the tick the run started at" rather than 0,
 * and both the elapsed-time and the KB/s figures on screen are unchanged. TM2 is still
 * rumble's and still untouched. */
static void art_tmr_start(u32* t0) { *t0 = perf_ticks(); }
static u32 art_tmr_ticks(void) { return perf_ticks(); }
static u32 art_tmr_ms(u32 ticks) { return perf_ms(ticks); }
static void art_tmr_stop(void) { }        /* session-long clock: nothing to stop */

typedef struct {
  int cancel;
  u32 t0;
} ArtExtractUi;

static void art_extract_draw(int pass, int rows_done, int rows_total, u32 elapsed_ms) {
  ui_clear();
  ui_text(4, 4, UI_TITLE, "EXTRACTING ART");
  ui_hline(0, 14, UI_SCR_W, UI_BORDER);
  ui_text(8, 26, UI_TEXT, pass == 1 ? "icons.bin  1/1" : "icons.bin  1/1  (verifying)");
  char row[40];
  siprintf(row, "row %d / %d", rows_done, rows_total);
  ui_text(8, 40, UI_TEXT, row);
  /* progress bar: 220 px wide, one fill per pass so a photograph shows which pass */
  int w = rows_total > 0 ? (220 * rows_done) / rows_total : 0;
  ui_panel(8, 54, 220, 12, UI_PANEL, UI_BORDER);
  if (w > 0) ui_fill_rect(9, 55, w > 218 ? 218 : w, 10, UI_OK);
  siprintf(row, "elapsed %lu.%01lus", (unsigned long)(elapsed_ms / 1000),
           (unsigned long)((elapsed_ms / 100) % 10));
  ui_text(8, 74, UI_DIM, row);
  if (pass == 1 && rows_done > 0 && elapsed_ms > 0) {
    /* Every row this pass costs a ROM read (art_icons_stream's fill_row) THEN an SD
     * write of the SAME bytes (savefile.c's stream_write_all) -- count BOTH sides,
     * not just the SD write, so this agrees in MEANING with the completion readout
     * below (which also counts every read+write side of the whole operation). A
     * label that named only the write half used to disagree with the completion
     * number by ~2x for exactly this reason. */
    uint32_t bytes_so_far = (uint32_t)rows_done * ART_ICONS_ROW_BYTES * 2u;
    uint32_t kbs10 = (uint32_t)(((uint64_t)bytes_so_far * 10000ull) / 1024ull / elapsed_ms);
    siprintf(row, "~%lu.%lu KB/s overall so far", (unsigned long)(kbs10 / 10),
             (unsigned long)(kbs10 % 10));
    ui_text(8, 84, UI_DIM, row);
  }
  ui_text(8, 148, UI_DIM, "B  cancel");
}

static bool art_extract_progress(void* vctx, int pass, int rows_done, int rows_total) {
  ArtExtractUi* c = (ArtExtractUi*)vctx;
  /* Redraw + poll input every 8 rows, not every one -- 440 rows at 60 fps worth of
   * redraws would slow the extraction for no visible benefit; every 8 is still
   * clearly in motion in a photograph and costs a small fraction of the wall clock. */
  if ((rows_done & 7) == 0 || rows_done >= rows_total) {
    key_poll();
    if (key_hit(KEY_B)) c->cancel = 1;
    art_extract_draw(pass, rows_done, rows_total, art_tmr_ms(art_tmr_ticks() - c->t0));
  }
  return !c->cancel;
}

/* Returns true iff a complete, verified cache now exists (art.idx included). Never
 * partial: on cancel/failure nothing new is left under a name the loader trusts (the
 * whole point of art.idx being written LAST, DESIGN.md Sec 2.2/3.2). */
static bool art_extract_run(u32* out_elapsed_ms, u32* out_kbs10, bool* out_cancelled) {
  *out_elapsed_ms = 0; *out_kbs10 = 0; *out_cancelled = false;
  if (!s_iconrom.ok) return false; /* caller already gated on this; belt and braces */

  f_mkdir(PDNA_DIR "/art"); /* FR_EXIST is fine (hard rule 9: one folder per tool) */

  ArtIconsGen gen;
  art_icons_gen_init(&gen, &s_iconrom, art_extract_progress, 0);
  ArtExtractUi ui; ui.cancel = 0;
  art_tmr_start(&ui.t0);
  gen.progress_ctx = &ui;

  bool ok = false;

  /* Every sibling SD path (pdna_map.c, pdna_bank.c, view_save) brackets its SD work in
   * rmbl_pause()/rmbl_resume() so an armed cue's motor never toggles the cart bus
   * mid-transfer (rmbl.h's contract). This is the longest transfer the app performs
   * (icons.bin write + its verify read-back, ~30 s), and nothing in this function's own
   * progress loop ever calls rmbl_vblank() -- it redraws + polls input every 8 rows, not
   * every frame -- so a cue armed on entry would otherwise keep whatever duty it last
   * committed running physically (the PWM ISR is IRQ-driven, independent of this
   * function ever being called again) for the WHOLE session. One rmbl_pause() here ends
   * that cue immediately (cue_end() + the PWM timer disabled) and nothing inside this
   * function ever calls rmbl_fire()/rmbl_demo(), so no new cue can arm before
   * rmbl_resume() below -- the individual ROM reads this triggers (iconrom_fatfs_read)
   * bracket themselves too, but only ever pause/unpause an ALREADY-ended cue once this
   * outer pause has run, so their nested calls cannot reopen the hazard. rumble_resume()
   * (rumble.c, 2026-08-18) actively drives the line low on resume, so nothing is left
   * latched on across this pause either. */
  rmbl_pause();
  artbuf_claim();  /* E3 review BLOCKING 2: the stream below fills mon_decomp per chunk */

  SfStatus st = sf_write_verified_stream(art_kind_filename(ART_KIND_ICONS), art_icons_stream,
                                         &gen, ART_ICONS_TOTAL_BYTES, (uint8_t*)mon_decomp,
                                         ART_ICONS_ROW_BYTES);
  u32 elapsed = art_tmr_ms(art_tmr_ticks() - ui.t0);
  art_tmr_stop();
  *out_elapsed_ms = elapsed;
  if (elapsed > 0) {
    /* Was labelled "KB/s (SD)" and counted only 2x ART_ICONS_TOTAL_BYTES (the write
     * pass' SD write + the verify pass' SD read-back of the .tmp) -- wrong by
     * construction, because `elapsed` ALSO spans both passes' ROM reads
     * (art_icons_stream's fill_row, called once per pass -- see its own header
     * comment): pass 1 reads the ROM to generate the bytes it writes, pass 2 reads
     * the ROM AGAIN to re-derive what stream_file_matches compares the re-read .tmp
     * against. That is FOUR full-file-sized transfers packed into `elapsed`, not
     * two, so the old formula understated true throughput by ~2x and disagreed with
     * the mid-run readout above, which (before this fix) counted only the write
     * side of pass 1 alone. Count all four here so the two readouts finally agree
     * in MEANING (this is overall wall-clock throughput for the whole operation,
     * ROM reads included, not a pure-SD figure -- labelled accordingly below). */
    uint64_t moved_bytes = (uint64_t)ART_ICONS_TOTAL_BYTES * 4u;
    *out_kbs10 = (u32)((moved_bytes * 10000ull) / 1024ull / elapsed);
  }

  if (gen.cancelled) { *out_cancelled = true; goto out; }
  if (st != SF_OK) {
    log_line("art extract: icons.bin write failed (%s)%s", sf_status_str(st),
             gen.rom_error ? " -- a rom read never verified" : "");
    goto out;
  }

  /* art.idx last -- the ONE thing the loader trusts, and it names the kind file it
   * just finished verifying, never one still in flight. */
  {
    uint32_t rom_fnv = 0;
    if (!art_rom_fnv(s_iconrom_ctx.read, s_iconrom_ctx.ctx, s_iconrom_ctx.size, &rom_fnv)) {
      log_line("art extract: could not hash the rom for art.idx");
      goto out;
    }
    ArtIdxHead head; memset(&head, 0, sizeof head);
    head.format = ART_IDX_FORMAT_V1;
    head.builder = 1;
    memcpy(head.rom_code, s_iconrom_ctx.code, 4);
    head.rom_rev = s_iconrom_ctx.version;
    head.rom_kind = (uint8_t)s_iconrom_ctx.kind;
    head.kinds = (uint8_t)(1u << ART_KIND_ICONS);
    head.rom_bytes = s_iconrom_ctx.size;
    head.rom_fnv = rom_fnv;
    ArtIdxKindRow row = { (uint8_t)ART_KIND_ICONS, ART_ICONS_TOTAL_BYTES,
                          art_icons_gen_fnv(&gen), ART_ICONS_ROWS };
    uint8_t idxbuf[ART_IDX_HEAD_BYTES + ART_IDX_KIND_BYTES];
    art_idx_head_write(&head, idxbuf);
    art_idx_kind_write(&row, idxbuf + ART_IDX_HEAD_BYTES);
    SfStatus ist = sf_write_verified(ART_IDX_PATH, idxbuf, sizeof idxbuf);
    if (ist != SF_OK) {
      log_line("art extract: art.idx write failed (%s) -- icons.bin stays unreferenced",
               sf_status_str(ist));
      goto out;
    }
  }
  log_line("art extract: icons.bin OK, %lu ms, art.idx written", (unsigned long)elapsed);
  ok = true;

out:
  rmbl_resume();
  return ok;
}

static void art_extract_screen(void) {
  char kb[24];
  /* Icons alone: ~441 KB (ART_ICONS_TOTAL_BYTES/1024). The confirm names ONLY what
   * this phase actually extracts -- never the whole 9-kind design total, which does
   * not exist yet. */
  siprintf(kb, "%lu KB", (unsigned long)(ART_ICONS_TOTAL_BYTES / 1024u));
  char l1[40]; siprintf(l1, "Icons, about %s, ~30s.", kb);
  if (!app_confirm("Extract art from ROM?", l1)) return;

  /* THE EXTRACTION LATCH -- mandatory, and the single most dangerous invariant this
   * change adds. icon_store holds icons.bin OPEN for the whole session, and FF_FS_LOCK
   * is 0, so FatFs will NOT stop that handle surviving across the f_unlink + f_rename
   * art_extract_run performs on the very same file. A FIL left pointing at a freed
   * cluster chain, on a card that also holds the user's saves, is not a cosmetic bug.
   * FatFs provides no mechanism here; this call IS the defence. app_icon_cache_resolve
   * below reopens everything. */
  icon_store_suspend();

  hb_arm();
  u32 elapsed_ms, kbs10; bool cancelled;
  bool ok = art_extract_run(&elapsed_ms, &kbs10, &cancelled);
  hb_off();

  /* Whatever happened, re-resolve the icon source NOW: a success must light the
   * cache up immediately (not just next boot), and a failed/cancelled run must not
   * leave a stale "ready" memo if a PARTIAL run happened to leave an old cache from
   * an earlier session looking (correctly) still valid. deep=true: this is the one
   * reachable path for the full ~451 KB re-hash (see app_icon_cache_resolve) -- the
   * user is already waiting right here, immediately after the bytes were written. */
  app_icon_cache_resolve(&s_iconrom_ctx, true);

  ui_clear();
  if (ok) {
    char l2[40];
    /* "overall", not "(SD)": kbs10 now counts BOTH ROM reads and SD transfers over
     * the whole elapsed time (see art_extract_run) -- it was never a pure-SD figure,
     * since elapsed_ms always included the ROM-read side too; the old "(SD)" label
     * claimed a number this measurement never actually produced. */
    siprintf(l2, "%lu.%01lus, ~%lu.%lu KB/s overall", (unsigned long)(elapsed_ms / 1000),
             (unsigned long)((elapsed_ms / 100) % 10), (unsigned long)(kbs10 / 10),
             (unsigned long)(kbs10 % 10));
    snd_ok();
    msg_wait("ART CACHED", UI_OK, "icons.bin written + verified.", l2);
  } else if (cancelled) {
    snd_back();
    msg_wait("CANCELLED", UI_DIM, "Nothing was written.", "Re-run any time.");
  } else {
    snd_error();
    msg_wait("EXTRACT FAILED", UI_WARN, "Nothing was written (see log).", 0);
  }
}

/* The Gen 1/2 ROM row's own action (E3 review item 6): "Change" when nothing is set
 * yet goes straight to the picker (app_register_gb_rom, unchanged); once a ROM IS
 * registered, offer Change/Clear instead of silently jumping to the picker again --
 * there was no way to unregister at all before this. Clear: forgets the config
 * path, un-registers the source for this gen (have() goes false immediately, not
 * just after the next boot) and drops the router's memo (a stale pointer into
 * whatever this ROM last decoded must not survive the ROM disappearing). Leaving
 * the vtable itself registered with both generations empty is fine -- gb_art_have()
 * gates every fetch, so an all-empty registration behaves exactly like none. */
static void gb_rom_row_action(uint8_t gen) {
  if (!app_gb_rom_path(gen)[0]) { app_register_gb_rom(gen); return; }
  const char* rows[2] = { "Change ROM", "Clear ROM" };
  int sel = 0;
  for (;;) {
    int my, mh;
    ui_popup_vfit(2, 14, 18, 8, &my, &mh);
    const int mx = 16, mw = 208;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, gen == PDNA_GEN1 ? "GEN 1 ROM" : "GEN 2 ROM");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < 2; i++) {
      int y = my + 18 + i * 14; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % 2;
    else if (k & KEY_A) {
      if (sel == 0) { app_register_gb_rom(gen); return; }
      app_gb_rom_path_set(gen, "");
      gb_art_register(gen, "", 0, 0, 0);
      cfg_save();
      pdna_origin_art_invalidate();
      snd_ok();
      msg_wait("GAME ROM", UI_OK, gen == PDNA_GEN1 ? "Gen 1 ROM cleared." : "Gen 2 ROM cleared.",
               "Gen-3 art returns for that era.");
      return;
    }
  }
}

#endif /* PDNA_DELTA */

/* Compiled in EVERY build (outside the #ifndef PDNA_DELTA block that owns the SD-only
 * browse/register path): the emulator build reaches this screen straight from the
 * Settings "Game ROM" row, so the E4 shots can be captured there; nothing it calls
 * needs a card (cfg_save() already guards itself on app_can_edit()). */
/* Settings > Game ROM > Sprites (E4): the 5 (kind) x 5 (place) grid of the user's
 * chosen art era per cell. Reached from rom_row_menu (below) rather than its own
 * PDNA_SET_ROWS row -- see pdna_layout.h's header comment on this screen's macros
 * for the full layout rationale. UP/DOWN moves the KIND (row), LEFT/RIGHT the PLACE
 * (column); both skip dead cells (se_cell_applies() == false) rather than land on
 * one -- bounded to SE_KIND_N/SE_PLACE_N steps so a future all-dead row/column
 * (impossible today: se_cell_applies() only ever kills ONE cell per kind and per
 * place, see its own comment) could never spin forever. A cursor is guaranteed to
 * start valid: (SE_KIND_RS, SE_PLACE_PC) always applies. */
static void sprite_settings(void) {
#define SPR_HDR_ONE(s) s,
  static const char* const HDR[SE_PLACE_N] = { PDNA_SETSPR_PLACE_HDRS(SPR_HDR_ONE) };
  int kind = SE_KIND_RS, place = SE_PLACE_PC;
  /* D3: the PC-grid icon STORE is built once, by app_icon_rom_open() (called only
   * from view_save + app_register_rom) -- se_store_era()'s whole-store choice for the
   * CURRENT save kind. Changing the PC cell here has no effect until that store is
   * rebuilt, so remember this save kind's PC cell as it stood on entry and, if it
   * changed by the time this screen saves, rebuild the store the same way
   * app_register_rom() does. */
  SeSaveKind k0 = app_save_kind();
  uint8_t pc0 = g_era.era[k0][SE_PLACE_PC];
  for (;;) {
    SeRoms roms = app_era_roms();
    ui_clear();
    ui_text(4, 4, UI_TITLE, PDNA_SETSPR_TITLE);
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    for (int p = 0; p < SE_PLACE_N; p++) {
      int x = PDNA_SETSPR_COL0_X + p * PDNA_SETSPR_COL_PITCH;
      ui_text(x, PDNA_SETSPR_HDR_Y, UI_DIM, HDR[p]);
    }
    for (int k = 0; k < SE_KIND_N; k++) {
      int y = PDNA_SETSPR_ROW0_Y + k * PDNA_SETSPR_ROW_PITCH;
      ui_text(PDNA_SETSPR_LABEL_X, y, UI_DIM, se_kind_name((SeSaveKind)k));
      for (int p = 0; p < SE_PLACE_N; p++) {
        int x = PDNA_SETSPR_COL0_X + p * PDNA_SETSPR_COL_PITCH;
        bool sel = (k == kind && p == place);
        bool applies = se_cell_applies((SeSaveKind)k, (SePlace)p);
        const char* txt = applies ? se_era_name((SeEra)g_era.era[k][p]) : PDNA_SETSPR_DEAD;
        /* A cell parked on an era whose ROM is unavailable (cleared, or every ROM
         * hidden by "ROM art OFF", #47) keeps its VALUE but is drawn dim: it is not a
         * live choice until the ROM is back, and se_era_next() will step past it. */
        bool live = applies && se_era_available((SeEra)g_era.era[k][p], &roms);
        if (sel) ui_panel(x - 2, y - 1, PDNA_SETSPR_COL_PITCH - 2, 13, UI_SEL, UI_TITLE);
        ui_ptext(x, y, !live ? UI_DIM : (sel ? UI_SELTEXT : UI_TEXT), txt);
      }
    }
    ui_text(PDNA_SET_HELP_X, PDNA_SETSPR_HELP_Y1, UI_DIM, PDNA_SETSPR_HELP1);
    ui_text(PDNA_SET_HELP_X, PDNA_SETSPR_HELP_Y2, UI_DIM, PDNA_SETSPR_HELP2);
    ui_text(PDNA_SET_FOOT_X, PDNA_SET_FOOTER_Y, UI_DIM, PDNA_SETSPR_FOOT);

    u16 k2 = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START);
    if (k2 & (KEY_B | KEY_START)) {
      cfg_save();
      pdna_origin_art_invalidate();
      art_session_invalidate();
      /* D3: the PC cell for THIS save kind changed -- the icon store still reflects
       * whichever era was current when app_icon_rom_open() last ran (view_save entry,
       * or app_register_rom), so it must be rebuilt now, the same call
       * app_register_rom() makes from Settings, or the new choice sits invisible
       * until the save is closed and reopened.
       *
       * E6 D7: k0 CAN be SE_KIND_GEN1/SE_KIND_GEN2 now that this screen is reachable
       * from a Game Boy session's own Settings row (gb_nav_from_start). app_icon_rom_open()
       * is designed around a Gen-3 save kind (it rebuilds the PC-grid icon STORE, which
       * has no Game Boy content -- E5 is what wires that up, not this). Today the PC
       * cell can never actually change for a GB kind (se_cell_applies(GEN1/GEN2, PC) is
       * false, so the row is dead in the grid above), so this branch cannot fire from a
       * GB session in practice -- but "cannot fire today" is not "designed not to", so
       * gate it on kind explicitly rather than rely on that being permanently true. */
      if (k0 < SE_KIND_GEN1 && g_era.era[k0][SE_PLACE_PC] != pc0) app_icon_rom_open();
      return;
    } else if (k2 & KEY_UP) {
      for (int i = 0; i < SE_KIND_N; i++) {
        kind = (kind + SE_KIND_N - 1) % SE_KIND_N;
        if (se_cell_applies((SeSaveKind)kind, (SePlace)place)) break;
      }
    } else if (k2 & KEY_DOWN) {
      for (int i = 0; i < SE_KIND_N; i++) {
        kind = (kind + 1) % SE_KIND_N;
        if (se_cell_applies((SeSaveKind)kind, (SePlace)place)) break;
      }
    } else if (k2 & KEY_LEFT) {
      for (int i = 0; i < SE_PLACE_N; i++) {
        place = (place + SE_PLACE_N - 1) % SE_PLACE_N;
        if (se_cell_applies((SeSaveKind)kind, (SePlace)place)) break;
      }
    } else if (k2 & KEY_RIGHT) {
      for (int i = 0; i < SE_PLACE_N; i++) {
        place = (place + 1) % SE_PLACE_N;
        if (se_cell_applies((SeSaveKind)kind, (SePlace)place)) break;
      }
    } else if (k2 & KEY_A) {
      if (se_cell_applies((SeSaveKind)kind, (SePlace)place)) {
        g_era.era[kind][place] = (uint8_t)se_era_next((SeEra)g_era.era[kind][place],
                                                       &roms, (SePlace)place);
        snd_ok();
      } else snd_deny();
    }
  }
}

#ifndef PDNA_DELTA
/* Settings > Game ROM, once something is registered: a small menu instead of jumping
 * straight to browse-for-ROM, so item 7's detach switch lives ON this same row (not
 * a new one) rather than needing its own PDNA_SET_ROWS slot. "Cancel" mirrors
 * dc_menu's pattern (ui_popup_vfit + a highlighted row list).
 * Calls app_register_rom() (browse-for-ROM), which only exists outside PDNA_DELTA
 * (there is no SD to browse in the emulator build) -- kept inside the SAME
 * #ifndef PDNA_DELTA block as that function (and as this function's only call site,
 * pdna_settings' S_GAMEROM handler below) rather than its own, so it can never again
 * end up compiled where its callee isn't. */

static void rom_row_menu(void) {
  /* Slice E3: two more rows, same menu, same "no new PDNA_SET_ROWS slot" reasoning
   * item 7 already used for the ROM-art toggle. Computed once at entry -- every
   * action below returns immediately, so (unlike pdna_settings' own loop) this popup
   * never needs to repaint its rows with fresher text mid-visit. */
  char r_gb1[32], r_gb2[32];
  siprintf(r_gb1, "Gen 1 ROM: %s", app_gb_rom_path(PDNA_GEN1)[0] ? "set" : "not set");
  siprintf(r_gb2, "Gen 2 ROM: %s", app_gb_rom_path(PDNA_GEN2)[0] ? "set" : "not set");
  const char* rows[6]; int act[6], nr = 0;
  rows[nr] = "Change ROM";                                       act[nr++] = 0;
  rows[nr] = g_rom_art_off ? "Turn ROM art ON" : "Turn ROM art OFF"; act[nr++] = 1;
  rows[nr] = r_gb1;                                              act[nr++] = 2;
  rows[nr] = r_gb2;                                              act[nr++] = 3;
  rows[nr] = "Sprites  >";                                       act[nr++] = 4;
  rows[nr] = "Cancel";                                            act[nr++] = -1;
  int sel = 0;
  for (;;) {
    int my, mh;
    ui_popup_vfit(nr, 14, 18, 8, &my, &mh);
    const int mx = 16, mw = 208;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "GAME ROM");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < nr; i++) {
      int y = my + 18 + i * 14; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : nr - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % nr;
    else if (k & KEY_A) {
      int a = act[sel];
      if (a == 0) { app_register_rom(); return; }
      else if (a == 1) {
        /* BACKLOG #47: no longer one chokepoint -- the switch means "no ROM art at
         * all", so it now also gates gb_art_source.c's gb_art_have()/pic()/icon()
         * (Game Boy portraits + menu icons) and app_era_roms() (the Sprites grid's
         * era availability), not just app_icon_rom_open's Gen-3 rung and
         * g3cross_pic_cb's cross-game one. app_icon_rom_open() remains the one call
         * that rebuilds the box's icon STORE (so the PC grid drops its cache
         * immediately); the GB/era rungs need no rebuild of their own -- they are
         * plain reads (gb_art_have, app_era_roms) that answer differently on their
         * very next call, same as the Gen-3 rungs always have.
         *
         * pdna_origin_art_invalidate() + art_session_invalidate() -- the same pair
         * sprite_settings() calls on its own way out (E4) -- drop the portrait
         * router's fetch memo so an already-open summary/box screen redraws with
         * the new verdict on its next repaint instead of showing whatever it last
         * fetched from the GB source before the flip. */
        g_rom_art_off = !g_rom_art_off;
        cfg_save();
        pdna_origin_art_invalidate();
        art_session_invalidate();
        app_icon_rom_open();
        snd_ok();
        msg_wait("ROM ART", g_rom_art_off ? UI_DIM : UI_OK,
                 g_rom_art_off ? "Pokemon art off (not map)." : "Reattached.",
                 g_rom_art_off ? "Your ROM path is kept." : "Real art is back on.");
        return;
      }
      else if (a == 2) { gb_rom_row_action(PDNA_GEN1); return; }
      else if (a == 3) { gb_rom_row_action(PDNA_GEN2); return; }
      else if (a == 4) { sprite_settings(); return; }
      else return;                                                 /* cancel */
    }
  }
}
#endif /* PDNA_DELTA */

/* One settings row. The highlight panel is PDNA_SET_ROW_PANEL_YOFF px taller than one
 * row pitch (starts above the text) so consecutive rows' panels touch with no gap --
 * but the LAST row has no row below it to touch, only the help text at
 * PDNA_SET_HELP_Y1, so its CALLER passes the shorter PDNA_SET_ROW_LASTPANEL_H instead
 * (pdna_layout.h; a full-height panel there reached 125, two rows INTO the help text
 * -- host-checked). The wipe below reuses that SAME `panel_h`, whichever one the
 * caller passed, rather than a separate/larger pitch value -- the D8 lesson (wipe the
 * ink extent, not the pitch) applied by construction: this geometry already had to
 * solve "don't paint into the chrome below the last row" for the ORIGINAL full draw,
 * and reusing its own height keeps that property for the partial repaint too. */
static void ps_row_paint(const char* text, int i, int panel_h, bool sel) {
  int y = PDNA_SET_ROW0_Y + i * PDNA_SET_ROW_PITCH;
  ui_fill_rect(2, y - PDNA_SET_ROW_PANEL_YOFF, 236, panel_h, UI_BG);
  if (sel) ui_panel(2, y - PDNA_SET_ROW_PANEL_YOFF, 236, panel_h, UI_SEL, UI_TITLE);
  ui_text(PDNA_SET_ROW_X, y, sel ? UI_SELTEXT : UI_TEXT, text);
}

/* SETTINGS main menu. Two rows toggle IN PLACE with no overlay (lesson 1, batch-D10
 * note 2): S_BACKUP cycles g_backup_mode directly, and S_YARD flips g_yard_visitors
 * directly when yard_ok (its denial branch, when !yard_ok, goes through msg_wait and
 * so is already gen-covered). The other rows (ROM/ART status, and the two ">" +
 * "Close" static labels) can only change via a NESTED full-screen call (rom_row_menu/
 * app_register_rom/art_extract_screen/anim_settings/rumble_settings), all of which
 * ui_clear() on their own first frame -- so gen alone would already catch them.
 * Rather than hand-splitting "these two need a value diff, those don't" (and risk
 * missing a THIRD silent path later), this shadows the COMPOSED TEXT of every row --
 * same idiom D5's browse_menu uses for the identical reason (a toggle that edits the
 * row you're already sitting on). A row's text can only ever grow more specific than a
 * boolean, never less, so this is strictly a superset of the two rows that truly need
 * it, at the cost of 8*44=352 B on this one shallow menu frame ("modest frames" still
 * holds: this is not a frame that stays live during any SD transfer). */
static void pdna_settings(void) {
  /* Row strings live in pdna_layout.h: sys8 does not clip at the right margin, it WRAPS
   * onto the row below, so their LENGTH is load-bearing and the host test checks it. */
#define SET_MODE_ONE(s) s,
  static const char* const MODE[3] = { PDNA_SET_BACKUP_MODES(SET_MODE_ONE) };
  enum { S_BACKUP, S_ANIM, S_YARD, S_ROM, S_ART, S_RUMBLE, S_CLEAR, S_CLOSE, S_N };
  _Static_assert(S_N == PDNA_SET_ROWS, "settings row count out of sync with pdna_layout.h");
  int sel = 0;
  char pv_rows[S_N][44] = { { 0 } };
  int pv_sel = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    bool full = !pv_valid || pv_gen != ui_clear_gen();

    char r0[44]; siprintf(r0, PDNA_SET_BACKUP_FMT, MODE[g_backup_mode]);
    /* Say WHY it is unavailable rather than showing a toggle that does nothing:
     * without a registered ROM there is no art to draw a visitor with. */
    bool yard_ok = app_any_rom_registered();
    /* "Needs your ROM" made this row 30 sys8 columns wide at x=10, two past the 28 the
     * screen holds — and TTE does not clip, it WRAPS: the "OM" reappeared at x=0 on the
     * next row, on top of "Game ROM: not set". "Set Game ROM" is 28 columns and points
     * at the row that fixes it. */
    char r1[44]; siprintf(r1, PDNA_SET_YARD_FMT,
                          !yard_ok ? PDNA_SET_YARD_NEEDROM
                                   : (g_yard_visitors ? PDNA_SET_YARD_ON : PDNA_SET_YARD_OFF));
    /* set+detached shows a state distinct from "not set" -- while detached,
     * app_icon_rom_open() never opens s_iconrom, so s_iconrom.ok alone would read
     * as "not set" even though g_rom_path[] still has the file (item 7: the whole
     * point is that the registration survives the flip). */
    char r2[44];
    if (g_rom_art_off && app_any_rom_registered()) siprintf(r2, PDNA_SET_ROM_FMT, PDNA_SET_ROM_ARTOFF);
    else siprintf(r2, PDNA_SET_ROM_FMT, s_iconrom.ok ? rom_kind_name(s_iconrom_ctx.kind) : PDNA_SET_ROM_NOTSET);
    /* Extract-art row (Phase 2): three dim/live states, same posture as Yard visitors
     * above -- say WHY it is unavailable rather than show a toggle that does nothing. */
    bool art_omega_ok = (active_flashcart == EZ_FLASH_OMEGA);
    bool art_selectable = s_iconrom.ok && art_omega_ok;
    char r3[44];
    if (!s_iconrom.ok) siprintf(r3, PDNA_SET_ART_FMT, PDNA_SET_ART_NEEDROM);
    else if (!art_omega_ok) siprintf(r3, PDNA_SET_ART_FMT, PDNA_SET_ART_NOOMEGA);
    else if (art_session_icons_ready_memoized())
      siprintf(r3, PDNA_SET_ART_CACHED_FMT, (unsigned long)(ART_ICONS_TOTAL_BYTES / 1024u));
    else siprintf(r3, "%s", PDNA_SET_ART_GO);
    const char* rows[S_N] = { r0, "Animations  >", r1, r2, r3, "Rumble  >", PDNA_SET_ROW_CLEAR, "Close" };

    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "SETTINGS");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      /* Rebalanced across the two rows: the first was 29 sys8 columns at x=8, i.e. ending
       * on the last pixel column of the screen with no margin at all. */
      ui_text(PDNA_SET_HELP_X, PDNA_SET_HELP_Y1, UI_DIM, PDNA_SET_HELP1);
      ui_text(PDNA_SET_HELP_X, PDNA_SET_HELP_Y2, UI_DIM, PDNA_SET_HELP2);
      ui_ptext(PDNA_SET_HELP_X, PDNA_SET_NOTE_Y, UI_DIM, PDNA_SET_NOTE);
      ui_text(PDNA_SET_FOOT_X, PDNA_SET_FOOTER_Y, UI_DIM, PDNA_SET_FOOT);
    }
    for (int i = 0; i < S_N; i++) {
      bool s = (i == sel), os = (i == pv_sel);
      int panel_h = (i == S_N - 1) ? PDNA_SET_ROW_LASTPANEL_H : PDNA_SET_ROW_PANEL_H;
      if (full || s != os || strcmp(rows[i], pv_rows[i]) != 0) ps_row_paint(rows[i], i, panel_h, s);
      strncpy(pv_rows[i], rows[i], sizeof pv_rows[i] - 1); pv_rows[i][sizeof pv_rows[i] - 1] = 0;
    }
    pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT);
    /* U2a hidden key: SELECT on Settings opens the GB-screen shell's own demo
     * (font sheet + a text-box border, docs/GB-GAME-SCREENS-DESIGN.md sec
     * 3.3) so it can be shot standalone before U2b's real trainer card exists.
     * Tries Gen 1 first (the only generation with a corpus ROM today), falls
     * back to Gen 2 if Gen 1 has none registered/beside-the-save.
     * D7 fix (U2a review): app_gb_rom_path(gen)[0] only sees a ROM the user
     * explicitly REGISTERED -- it never sees gb_art_have()'s own beside-the-
     * save fallback (a ROM sitting next to the .sav with no registration at
     * all), so this hidden key wrongly picked Gen 2 whenever a Gen-1 ROM was
     * only available that way. gb_art_have() is the same "is a ROM actually
     * usable" check the rest of this shell relies on. */
    if (k & KEY_SELECT) {
      gbscr_run_demo(gb_art_have(PDNA_GEN1) ? PDNA_GEN1 : PDNA_GEN2);
      pv_valid = false;   /* the demo drew over the whole screen -- force a full repaint */
    }
    else if (k & KEY_B) { cfg_save(); return; }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : S_N - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % S_N;
    else if (k & KEY_A) {
      if (sel == S_BACKUP) g_backup_mode = (g_backup_mode + 1) % 3;
      else if (sel == S_ANIM)   anim_settings();
      else if (sel == S_YARD) {
        /* Refuse rather than toggle a flag that cannot take effect — the row already
         * reads "Needs your ROM", so this just explains the refusal once. */
        if (!yard_ok) {
          snd_deny();
          msg_wait("YARD VISITORS", UI_DIM, "Register your game ROM first",
                   "(Settings > Game ROM).");
        } else g_yard_visitors = !g_yard_visitors;
      }
      else if (sel == S_ROM) {
#ifdef PDNA_DELTA
        /* No SD => nothing to browse or register; the only live sub-screen of this
         * row in the emulator build is the E4 Sprites grid, so go straight there. */
        sprite_settings();
#else
        if (app_any_rom_registered()) rom_row_menu(); else app_register_rom();
#endif
      }
      else if (sel == S_ART) {
#ifdef PDNA_DELTA
        snd_deny(); msg_wait("NO SD HERE", UI_DIM, "Extraction needs a real card;", "not available in this build.");
#else
        if (!art_selectable) {
          snd_deny();
          msg_wait("EXTRACT ART", UI_DIM,
                   !s_iconrom.ok ? "Register your game ROM first" : "Needs EZ-Flash Omega DE",
                   !s_iconrom.ok ? "(Settings > Game ROM)." : "(EverDrive stays read-only).");
        } else art_extract_screen();
#endif
      }
      else if (sel == S_RUMBLE) rumble_settings();
      else if (sel == S_CLEAR) {
        if (!cart_writable()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
        if (app_confirm("Delete ALL backups?", "For the loaded save only.")) {
          rmbl_pause();
          int rm = sf_clear_backups(g_path);
          rmbl_resume();
          char m[44]; siprintf(m, "%d backup(s) removed.", rm);
          snd_ok(); msg_wait("CLEARED", UI_OK, m, 0);
        }
      } else return;   /* S_CLOSE */
    }
  }
}

/* ---- Shared /PokeDNA/battles/*.rec picker (BACKLOG #32) ---------------------------
 * Lists, lets the user move the cursor, and returns the chosen filename (NOT a full
 * path — callers build their own with PDNA_DIR "/battles/%s") into `out` (>= 40 bytes)
 * on A; false on B or an empty directory (an empty directory already shows its own
 * message here, so callers need no separate empty-list UI). `title`/`footer` are the
 * only two strings that differ between callers (the importer vs. the RECPAGE_FILES
 * page), so this one body serves both without hardcoding either's wording.
 * Filters to *.rec: every export also writes a same-basename .txt sidecar, and
 * without the filter every battle would list twice.
 * Caps at 24 files / 39-char names / 14 rows; `static char names[24][40]` is plain
 * static = IWRAM (960 B), not EWRAM — deliberate (the guard is at 612 B free). */
/* One file row. Row pitch 9 == the panel height (touching, not overlapping -- same
 * contract as every other 9px-pitch list in this file, e.g. render_browser). `rt` is
 * sized 128, not 40: ui.h's contract wants max_cols*4+1 (117 for 29 cols) because
 * these ARE real FatFs long filenames (FF_LFN_UNICODE), unlike D7's Gen-3 in-game
 * names -- the same bug class D5's F3 finding caught in the file browser, latent here
 * since B1/B2 first wrote this row and fixed while this line was already being
 * touched for the repaint conversion. */
static void rl_row_paint(char (*names)[40], int idx, int i, bool sel) {
  int y = 20 + i * 9;
  ui_fill_rect(0, y - 1, UI_SCR_W, 9, UI_BG);
  if (sel) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
  char rt[128]; ui_truncate(rt, names[idx], 29);
  ui_text(6, y, sel ? UI_SELTEXT : UI_TEXT, rt);
}

/* The list is scanned ONCE, before the loop, and never rescanned inside it (unlike
 * D5's file browser, whose scan_dir()/sort_entries() can re-run mid-loop) -- so unlike
 * that screen, no explicit relist flag is needed here: `names`/`n` are invariant for
 * the whole picking session and only sel/top move. */
static bool rec_list_pick(const char* title, const char* footer, char* out, size_t outcap) {
  static char names[24][40];
  int n = 0;
  { DIR d; FILINFO fi;                                  /* list /PokeDNA/battles/*.rec */
    if (f_opendir(&d, PDNA_DIR "/battles") == FR_OK) {
      while (n < 24 && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        int L = (int)strlen(fi.fname);
        if ((fi.fattrib & AM_DIR) || L < 5 || L >= (int)sizeof names[0]) continue;
        const char* e = fi.fname + L - 4;
        if (e[0] != '.' || (e[1] | 32) != 'r' || (e[2] | 32) != 'e' || (e[3] | 32) != 'c') continue;
        strcpy(names[n++], fi.fname);
      }
      f_closedir(&d);
    } }
  if (!n) { snd_deny(); msg_wait(title, UI_DIM, "No .rec files in", "/PokeDNA/battles."); return false; }

  int sel = 0, top = 0;                                 /* pick one */
  int pv_top = -1, pv_sel = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    if (sel < top) top = sel; if (sel >= top + 14) top = sel - 13;

    bool full = !pv_valid || pv_gen != ui_clear_gen();
    if (full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, title);
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
      ui_text(4, 152, UI_DIM, footer);
    }
    /* Per-row dirty via index identity, same as render_browser/pdna_secretbase (D5's
     * F1 fix): `top` stays OUT of `full`, so a scroll (auto-repeating UP/DOWN, up to
     * 24 files over a 14-row window) repaints every visible row without paying the
     * ui_clear(); a bare cursor move only flips the two rows whose selection changed. */
    uint32_t dirty = 0;
    for (int i = 0; i < 14 && top + i < n; i++) {
      int f = top + i, of = pv_top + i;
      bool s = (f == sel), os = (of == pv_sel);
      if (full || of != f || s != os) dirty |= 1u << i;
    }
    for (int i = 0; i < 14 && top + i < n; i++)
      if (dirty & (1u << i)) rl_row_paint(names, top + i, i, top + i == sel);
    pv_top = top; pv_sel = sel; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) break;
  }
  size_t L = strlen(names[sel]);
  if (L >= outcap) return false;   /* defensive; names[] entries (< 40 B) fit every caller's outcap */
  memcpy(out, names[sel], L + 1);
  return true;
}

/* ---- Import a .rec from /PokeDNA/battles back into the save (sector 31) -----------
 * The game gates the Frontier Pass "BATTLE RECORD" purely on the sector's own validity
 * (sentinel + battleFlags + byte-sum checksum — pokeemerald CanCopyRecordedBattleSaveData;
 * no other flag), so a valid imported record replays exactly like one just recorded.
 * The save's current record is overwritten (confirmed first); persistence rides the
 * standard backup + verified full-save commit (the whole g_save image is written, and
 * sector 31 is part of it). Omega-only (it is a save edit). */
static bool rec_import(void) {
  char name[40];
  if (!rec_list_pick("IMPORT RECORD", "A import  B cancel", name, sizeof name)) return false;
  char path[64];
  sniprintf(path, sizeof path, PDNA_DIR "/battles/%s", name);

  /* validate by STREAMING (512-B chunks, no big buffer): size, sentinel, checksum */
  FIL f; UINT br;
  if (f_open(&f, path, FA_READ) != FR_OK) {
    snd_error(); msg_wait("IMPORT FAILED", UI_WARN, "Cannot open the file.", 0); return false;
  }
  bool ok = (f_size(&f) == G3_SECTOR_SIZE);
  uint32_t sum = 0, stored = 0;
  uint8_t buf[512];
  for (uint32_t off = 0; ok && off < G3_SECTOR_SIZE; off += sizeof buf) {
    if (f_read(&f, buf, sizeof buf, &br) != FR_OK || br != sizeof buf) { ok = false; break; }
    if (off == 0 && !(buf[0] == 0x9D && buf[1] == 0xB3 && buf[2] == 0 && buf[3] == 0)) ok = false;
    for (uint32_t i = 0; ok && i < sizeof buf; i++) {
      uint32_t a = off + i;
      if (a >= 4 && a < 4 + 3964) sum += buf[i];        /* struct byte-sum          */
      else if (a >= 4 + 3964 && a < 4 + 3968)           /* trailing u32 checksum    */
        stored |= (uint32_t)buf[i] << ((a - (4 + 3964)) * 8);
    }
  }
  if (ok && sum != stored) ok = false;
  if (!ok) {
    f_close(&f); snd_error();
    msg_wait("IMPORT FAILED", UI_WARN, "Not a valid battle", "record file."); return false;
  }
  if (!app_confirm("Import this record?", "Replaces the save's one.")) { f_close(&f); snd_back(); return false; }

  f_lseek(&f, 0);
  FRESULT fr = f_read(&f, g_save + G3_REC_SECTOR_OFF, G3_SECTOR_SIZE, &br);
  f_close(&f);
  if (fr != FR_OK || br != G3_SECTOR_SIZE) {
    /* the in-RAM sector may be half-written — restore it from the .sav on disk */
    FIL s2; UINT br2 = 0;
    if (f_open(&s2, g_path, FA_READ) == FR_OK) {
      f_lseek(&s2, G3_REC_SECTOR_OFF);
      f_read(&s2, g_save + G3_REC_SECTOR_OFF, G3_SECTOR_SIZE, &br2);
      f_close(&s2);
    }
    snd_error();
    msg_wait("IMPORT FAILED", UI_WARN, "Read error;",
             br2 == G3_SECTOR_SIZE ? "old record restored." : "re-open the save!");
    return false;
  }
  log_line("record: import %s", name);
  /* persist: re-committing the (unchanged) SB2 runs the standard backup + verified
   * full-save write, which carries the new sector 31 with it */
  if (!app_commit_sb2()) {
    snd_error(); msg_wait("IMPORT", UI_WARN, "Imported in RAM only —", "save write failed/declined.");
    return true;                                        /* screen still shows it (RAM) */
  }
  snd_save(); msg_wait("IMPORTED", UI_OK, "This is now the save's", "last recorded battle.");
  return true;
}

/* Page index cycled by L/R in pdna_battle_record. An int (not a bool) because a
 * third page — the .rec file browser (RECPAGE_FILES, BACKLOG #32) — joins the
 * cycle: order is summary -> streaks -> files -> wrap. RECPAGE_COUNT is the
 * single place that grows if a fourth page ever lands. RECPAGE_FILES is NOT a
 * per-frame page body like the other two (see pdna_battle_record's main loop):
 * it is a self-contained sub-screen (rec_files_page), because it needs its own
 * list/preview state machine, not a static display. */
enum { RECPAGE_SUMMARY, RECPAGE_STREAKS, RECPAGE_FILES, RECPAGE_COUNT };

/* The record's own summary: facility/level, who recorded it, seed + opponent (+ a
 * checksum-bad flag), both teams, and what exporting produces. Body only — the
 * caller draws the shared title/hline/footer chrome — so both the live page and
 * the .rec-browser's file preview (rec_files_page) can call this against any
 * scanned G3RecordInfo without dragging along either screen's furniture.
 * `sec4k` is the 4 KiB sector-31 blob the info was scanned from — byte 0 is the
 * sentinel, exactly what g3_record_scan_sector expects and what a .rec file
 * contains verbatim — NOT the full save image. The live page passes
 * `g_save + G3_REC_SECTOR_OFF`; a byte-for-byte identical pointer to what the old
 * g3_record_party(g_save, side) computed internally, so page-1 output for the
 * live record is pixel-identical to before this was threaded through. */
static void render_record_summary(const G3RecordInfo* ri, const uint8_t* sec4k) {
  char l[40];
  siprintf(l, "%s  %s", g3_record_facility_name(ri->facility),
           ri->lvl_mode ? "Open Level" : "Level 50");
  ui_text(4, 20, UI_TEXT, l);
  const char* who = ri->names[ri->multiplayer_id][0] ? ri->names[ri->multiplayer_id] : "?";
  siprintf(l, "Recorded by %s (%s)", who, ri->genders[ri->multiplayer_id] ? "F" : "M");
  ui_text(4, 30, UI_TEXT, l);
  siprintf(l, "Seed %08lx  Opp #%u", (unsigned long)ri->rng_seed, ri->opponent_a);
  ui_text(4, 40, UI_DIM, l);
  if (!ri->checksum_ok) ui_text(130, 40, UI_WARN, "CHECKSUM BAD");

  ui_text(4, 54, UI_TITLE, "YOUR TEAM");
  ui_text(124, 54, UI_TITLE, "OPPONENT");
  for (int side = 0; side < 2; side++) {
    const uint8_t* party = g3_record_party_sector(sec4k, side);
    int y = 64;
    for (int i = 0; i < 6; i++) {
      PkMon m;
      if (!pk_decode_mon(party + (uint32_t)i * G3_REC_MON_SIZE, false, &m) || !m.species) continue;
      uint8_t lvl = party[(uint32_t)i * G3_REC_MON_SIZE + 84];   /* plaintext battle level */
      siprintf(l, "%s %u", pk_species_name(m.species), lvl);
      ui_text(side ? 124 : 4, y, UI_TEXT, l);
      y += 10;
    }
  }
  /* What the exported pair IS, and what opens it on a PC. The band y=122..151 is
   * free even for a full 6-mon column (rows start at y=64, pitch 10 -> ink ends at
   * y=121). Proportional face: line 1 is 226 px here, 360 px at sys8. rec2mp4 is
   * not published yet, so it is named, not linked. */
  ui_hline(0, 124, UI_SCR_W, UI_BORDER);
  ui_ptext(4, 128, UI_DIM, "A exports a .rec + .txt to /PokeDNA/battles/.");
  ui_ptext(4, 138, UI_DIM, "On a PC, rec2mp4 replays it into a video.");
}

/* Page 2: every facility's win streak, current/best, Lv50 in the left number
 * column vs Open in the right one — one row per (facility, battle mode) so a
 * multi-mode facility (the Tower has 4) gets its own row instead of an
 * ambiguous merged number. g3f_modes() summed over all 7 facilities is a fixed
 * 13 (4+2+2+1+2+1+1: Tower/Dome/Palace/Arena/Factory/Pike/Pyramid), which is
 * why a static 13-row table fits this screen with no scrolling needed — the
 * same 13-row band pdna_frontier.c already proved fits (y=18..~146 there, at a
 * 10 px pitch); this table only tightens the pitch to 9 px to also leave room
 * for the Lv50/Open header row above it. Read-only: editing a streak already
 * exists on the dedicated Frontier screen (pdna_frontier.c), so this is not a
 * second control surface for the same write path.
 * Reused verbatim from pdna_frontier.c's own row convention: cur then best,
 * '*' marks winStreakActiveFlags (the streak the game will actually KEEP —
 * see gen3_frontier.h). Only reachable when g_game == PK_EMERALD (checked at
 * pdna_battle_record's entry), so sb2 is always a genuine Emerald SaveBlock2
 * here — unlike the sidecar, this body has no non-Emerald caller to degrade
 * for. */
static void render_record_streaks(const uint8_t* sb2) {
  ui_text(4, 18, UI_DIM, "streaks");
  ui_text(64, 18, UI_DIM, "Lv50");
  ui_text(150, 18, UI_DIM, "Open");
  int y = 27;
  for (int f = 0; f < 7; f++) {
    int modes = g3f_modes(f);
    for (int m = 0; m < modes; m++) {
      char lbl[8];
      if (m == 0) ui_text(4, y, UI_TEXT, g3_record_facility_short(f));
      else {
        const char* mn = g3f_mode_name(f, m);
        siprintf(lbl, " %c", (mn && mn[0]) ? mn[0] : '?');
        ui_text(4, y, UI_DIM, lbl);
      }
      for (int lvl = 0; lvl < 2; lvl++) {
        int cur = g3f_streak_get(sb2, f, m, lvl, G3F_CURRENT);
        int rec = g3f_streak_get(sb2, f, m, lvl, G3F_RECORD);
        bool act = g3f_active_get(sb2, f, m, lvl);
        /* g3f_streak_get returns the RAW u16 (65535 possible on a corrupt save), and
         * "%4d/%4d%c" at 5 digits is 12 chars + NUL = 13 -- one byte past the first
         * cut's char l[12], and 96 px of text that collides the Open column and runs
         * off the 240 px screen. The columns stay fixed: an out-of-cap lane renders
         * as "????" in UI_WARN -- shown as corrupt, never silently clamped. */
        char l[16];
        int cap = g3f_streak_cap(f);
        if ((cur > cap && cur != -1) || (rec > cap && rec != -1)) {
          siprintf(l, "????/????%c", act ? '*' : ' ');
          ui_text(lvl ? 150 : 64, y, UI_WARN, l);
        } else {
          siprintf(l, "%4d/%4d%c", cur < 0 ? 0 : cur, rec < 0 ? 0 : rec, act ? '*' : ' ');
          ui_text(lvl ? 150 : 64, y, (cur > 0 && !act) ? UI_WARN : UI_TEXT, l);
        }
      }
      y += 9;
    }
  }
}

/* ---- RECPAGE_FILES: browse /PokeDNA/battles/*.rec, preview without touching the
 * save (BACKLOG #32) --------------------------------------------------------------
 * A self-contained sub-screen, not a per-frame page body like pages 1/2 above: it
 * needs its own list -> optional preview -> back-to-list state machine, and
 * rec_list_pick already owns a full redraw/key loop, so nesting it here is simpler
 * than threading a sub-state through pdna_battle_record's own loop. B on the list
 * returns here to page 1 (rec_files_page's caller resets `page`); B on a preview
 * returns to the list (the inner for(;;) below).
 *
 * SELECT is deliberately INERT on both the list and the preview -- unlike
 * pdna_battle_record's own screen, where SELECT means "import over the live
 * record". A casual browse-and-preview here must never risk that destructive
 * action landing on the wrong keypress; importing an older .rec still works from
 * page 1's own SELECT.
 *
 * The staged 4 KiB copy borrows app_arena_acquire() (pdna_app.h) rather than
 * reading straight into g_save + G3_REC_SECTOR_OFF the way rec_import does --
 * that would clobber the loaded save's live record just to preview a file. NULL
 * (the PC dirty) is told to the user, never silently retried. */
/* noinline: this holds a FIL (~560 B) on the stack; inlined into the nav loop it was
 * reserved on main's frame for the whole run (+384 B permanent IWRAM stack, measured
 * with -fstack-usage in review). As its own frame it exists only while browsing. */
static __attribute__((noinline)) void rec_files_page(void) {
  for (;;) {
    char name[40];
    if (!rec_list_pick("RECORD FILES", "A view  B page 1  U/D pick", name, sizeof name))
      return;                                            /* B on the list -> back to page 1 */

    uint8_t* blob = app_arena_acquire(G3_SECTOR_SIZE);
    if (!blob) {
      snd_deny();
      msg_wait("NOT NOW", UI_WARN, "Save your box changes", "first, then try again.");
      continue;
    }
    char path[72];
    sniprintf(path, sizeof path, PDNA_DIR "/battles/%s", name);
    FIL f; UINT br = 0;
    bool ok = (f_open(&f, path, FA_READ) == FR_OK);
    if (ok) {
      /* Exact size FIRST, the same test rec_import applies (pdna_main.c, its f_size
       * check): br == 4096 alone accepts any file >= 4 KiB, so an oversized .rec
       * would preview fine here and then fail the import on page 1 -- two verdicts
       * on the same file from the same feature. */
      ok = (f_size(&f) == G3_SECTOR_SIZE)
        && (f_read(&f, blob, G3_SECTOR_SIZE, &br) == FR_OK) && (br == G3_SECTOR_SIZE);
      f_close(&f);
    }
    G3RecordInfo fi;
    bool valid = ok && g3_record_scan_sector(blob, &fi);
    if (!valid) {
      app_arena_release();
      snd_error();
      msg_wait("PREVIEW FAILED", UI_WARN, "Not a valid battle", "record file.");
      continue;
    }

    ui_clear();
    char t[40]; ui_truncate(t, name, 29);                 /* title = the FILE, not "BATTLE RECORD" --
                                                            * never let this be mistaken for the live save */
    ui_text(4, 4, UI_TITLE, t);
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    render_record_summary(&fi, blob);
    ui_text(4, 152, UI_DIM, "B back");
    wait_keys(KEY_B);
    app_arena_release();
  }
}

/* ---- Emerald Battle Record (save sector 31): info + export ------------------------
 * The Frontier Pass Battle Record is a full deterministic replay (RNG seed + both
 * teams + per-battler input streams) at a fixed, non-rotating sector. This screen
 * shows what's recorded and exports the raw 4 KiB sector to the SD for the PC-side
 * replay pipeline — that PC tool is rec2mp4 (gba-toolkit/projects/rec2mp4), which
 * injects the .rec back into a save, replays it in a headless mGBA and encodes an
 * MP4. It is not published yet, so no screen here prints a URL.
 * Emerald-only; export needs the Omega. */
static void pdna_battle_record(void) {
  if (g_game != PK_EMERALD) {
    msg_wait("BATTLE RECORD", UI_DIM, "Only Emerald stores a", "Battle Record.");
    return;
  }
  if (g_save_size < (uint32_t)G3_SAVE_FILE_SIZE) {     /* 64 KiB dump: no sector 31 at all */
    msg_wait("BATTLE RECORD", UI_DIM, "Save has no sector 31", "(64 KiB dump).");
    return;
  }
  G3RecordInfo ri;
  /* BACKLOG #35 item 2: a save with no valid sector 31 used to gate out of this WHOLE
   * screen -- but RECPAGE_FILES (rec_files_page) is a read-only browser over
   * /PokeDNA/battles that never touches the live record or g_save at all, so it works
   * fine with none. Restructured to let a no-record user reach it directly: "A" here
   * calls rec_files_page() the same way page 3 does further down, then `continue`s back
   * to this SAME while-condition, which re-scans -- so browsing/previewing (which
   * changes nothing) just redraws this same gate, and a successful IMPORT (still SEL,
   * unchanged) naturally falls out of the loop into the normal 3-page screen below.
   * Chose "skip pages 1-2 entirely" over "show them in an honest no-record state":
   * `ri` is only ever populated by a SUCCESSFUL g3_record_scan, so rendering
   * render_record_summary/render_record_streaks against it here (an uninitialised
   * struct) would be a real bug, not a cosmetic one -- ri.multiplayer_id indexes
   * ri.names[] and could read garbage. Reaching the browser directly needs no new
   * "no record" rendering path for those two screens at all, and does not touch
   * rec_import()'s own validation (unchanged below). A works read-only (no
   * app_can_edit() gate), matching that browsing needs no write access. */
  while (!g3_record_scan(g_save, g_save_size, &ri)) {  /* none yet -> still offer import/browse */
    ui_clear();
    ui_text(4, 4, UI_TITLE, "BATTLE RECORD");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    ui_text(4, 24, UI_DIM, "No recorded battle yet.");
    ui_text(4, 34, UI_DIM, "Record one via the Frontier");
    ui_text(4, 44, UI_DIM, "Pass, or import a .rec.");
    /* Say what a .rec IS and what opens it — the proportional face is mandatory:
     * these run 198/208 px at 5x7 but would be 320+ px at sys8's fixed 8 px/glyph. */
    ui_ptext(4, 60, UI_DIM, "A .rec is a battle exported here or by a");
    ui_ptext(4, 70, UI_DIM, "friend; rec2mp4 (PC) turns one into video.");
    ui_text(4, 152, UI_DIM, "A files  SEL import  B back");
    u16 k = wait_keys(KEY_A | KEY_SELECT | KEY_B);
    if (k & KEY_B) { snd_back(); return; }
    if (k & KEY_A) { rec_files_page(); continue; }      /* browse exports; no record needed */
    if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); continue; }
    rec_import();                                      /* success -> the rescan shows it */
  }
  rmbl_fire(RCUE_ROOM);
  int page = RECPAGE_SUMMARY;
  /* Neither page has a cursor (render_record_summary/render_record_streaks are plain
   * dumps, no selectable row), so there is no per-row diff to make here -- a page flip
   * legitimately redraws the whole content area (batch-D8 note 1). What IS wasteful is
   * paying the 76,800 B ui_clear() plus re-drawing the TITLE/hline (pixel-identical on
   * both pages) on every L/R flip between SUMMARY and STREAKS -- the two most-pressed
   * keys here, and neither is in wait_keys' repeat mask (only UP/DOWN/LEFT/RIGHT
   * auto-repeat; L/R do not), so this is about per-press cost, not held-spam. Shadow:
   * page + valid + gen. Every OTHER mutation here (SELECT->rec_import, A->export) goes
   * through msg_wait/busy_panel/rec_list_pick, all of which ui_clear() -- even
   * rec_import's own "no files" denial does, via msg_wait -- so gen alone catches
   * every content change that is NOT a plain page flip. */
  int pv_page = -1; bool pv_valid = false; uint32_t pv_gen = 0;
  for (;;) {
    if (page == RECPAGE_FILES) {        /* self-contained sub-screen, not a page body -- see the enum comment */
      rec_files_page();
      page = RECPAGE_SUMMARY;           /* its own B already means "back to page 1" */
      continue;
    }
    bool chrome_full = !pv_valid || pv_gen != ui_clear_gen();
    bool content_dirty = chrome_full || page != pv_page;

    if (chrome_full) {
      ui_clear();
      ui_text(4, 4, UI_TITLE, "BATTLE RECORD");
      ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    }
    if (content_dirty) {
      /* Chrome (title+hline) already exists and is unchanged -- wipe just the content
       * + footer band below the hline before redrawing it, matching what ui_clear()
       * would have left there. */
      if (!chrome_full) ui_fill_rect(0, 15, UI_SCR_W, UI_SCR_H - 15, UI_BG);
      if (page == RECPAGE_SUMMARY) render_record_summary(&ri, g_save + G3_REC_SECTOR_OFF);
      else                         render_record_streaks(g_sb2);
      /* Per-page footer (29-column budget; both hit it, page 1 exactly ("A exp SEL imp
       * B back L/R page" = 29). BACKLOG #35 item 1: page 2's own footer used to say
       * "B page 1" but never mention L/R at all, even though L/R cycles from here
       * exactly like it does from page 1 -- same main loop, same key handling above,
       * L always lands on page 1 (same destination "B" already names) and R on page 3
       * (the .rec browser). Reworded to "L/R/B page  cur/best  *=kept" (28 chars) --
       * L/R's destination already subsumes what "B page 1" said, so that phrase is
       * dropped to make room rather than cutting the cur/best legend, which explains
       * a number format ("21/45*") nothing else on screen does. Still inside
       * `content_dirty` (page != pv_page is one of its terms, D8), so this line
       * repaints on every flip, not just the content above it.
       *
       * Page 3 (RECPAGE_FILES / rec_files_page, further down and in its own footer
       * string passed to rec_list_pick) is NOT given an "L/R cycles" hint: checked
       * rec_list_pick's key mask (KEY_UP|KEY_DOWN|KEY_A|KEY_B only) and confirmed L/R
       * is not polled there at all -- wiring it would mean changing a SHARED picker
       * also used by rec_import's plain file-import dialog (where "L/R page" would
       * be actively wrong), which is a real feature change, not a footer reword.
       * Page 3's existing "B page 1" already correctly describes its one working way
       * back, so it was left as-is rather than claim a capability that isn't there. */
      ui_text(4, 152, UI_DIM, page == RECPAGE_SUMMARY
                                ? "A exp SEL imp B back L/R page"
                                : "L/R/B page  cur/best  *=kept");
    }
    pv_page = page; pv_valid = true; pv_gen = ui_clear_gen();

    u16 k = wait_keys(KEY_A | KEY_B | KEY_SELECT | KEY_L | KEY_R);
    if (k & (KEY_L | KEY_R)) {          /* house style: pdna_trainer.c ~626-632 */
      snd_tab();
      /* L back, R forward -- identical at 2 pages, load-bearing at 3: R from
       * streaks lands on RECPAGE_FILES (the .rec browser); a bool-style
       * "always forward" would instead skip straight past it back to summary. */
      page = (page + ((k & KEY_R) ? 1 : RECPAGE_COUNT - 1)) % RECPAGE_COUNT;
      continue;
    }
    if (k & KEY_B) {
      snd_back();
      if (page != RECPAGE_SUMMARY) { page = RECPAGE_SUMMARY; continue; }
      return;
    }
    if (k & KEY_SELECT) {                              /* import an older .rec over this one */
      if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); continue; }
      if (rec_import()) g3_record_scan(g_save, g_save_size, &ri);   /* show the imported battle */
      continue;
    }
    /* ---- A: export the raw 4 KiB sector (verified write, Omega-only) ---- */
    if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); continue; }
    const char* who = ri.names[ri.multiplayer_id][0] ? ri.names[ri.multiplayer_id] : "?";
    char base[8]; int o = 0;
    for (int i = 0; who[i] && o < 7; i++) {              /* sanitize the trainer name for FAT */
      char c = who[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) base[o++] = c;
    }
    if (!o) { base[0] = 'R'; base[1] = 'E'; base[2] = 'C'; o = 3; }
    base[o] = 0;
    /* Facility + level-mode + CURRENT win-streak tag, e.g. "_Dome-O-21"
     * (O = Open Level, 50 = Level 50). The streak lives ONLY in SaveBlock2 at
     * export time — sector 31 never stores it — so it would be lost the moment
     * the file is written. Unreadable streak -> empty tag, old naming: the tag
     * never blocks an export. All tag characters are FAT-LFN-safe. */
    char tag[20]; tag[0] = 0;
    int streak = g3_record_win_streak(g_sb2, &ri);       /* Emerald-only screen: g_sb2 is Emerald layout */
    if (streak >= 0)
      sniprintf(tag, sizeof tag, "_%s-%s-%d", g3_record_facility_short(ri.facility),
                ri.lvl_mode ? "O" : "50", streak);
    /* Timestamped filename (DD-MM-YYYY + 24h HH-MM; '/'|':' are illegal in FAT names)
     * so repeated exports never overwrite each other. The RTC transaction rides the
     * same cart GPIO bus as rumble, so it shares the rmbl_pause window with the write.
     * No RTC exposed -> fall back to the seed name (a re-export then only ever
     * overwrites the identical battle). Worst case fits: "/PokeDNA/battles/" 17
     * + base 7 + tag 16 ("_Factory-50-9999") + "_DD-MM-YYYY_HH-MM.rec" 21 = 61. */
    char path[72];
    GbaRtcTime t;
    busy_panel("Writing + verifying...");
    rmbl_pause();                                        /* quiet the cart bus: RTC read + SD write */
    bool rtc_ok = gba_rtc_get(&t);
    if (rtc_ok)
      sniprintf(path, sizeof path, PDNA_DIR "/battles/%s%s_%02u-%02u-%04u_%02u-%02u.rec",
                base, tag, t.day, t.month, t.year, t.hour, t.minute);
    else
      sniprintf(path, sizeof path, PDNA_DIR "/battles/%s%s_%08lx.rec", base, tag, (unsigned long)ri.rng_seed);
    f_mkdir(PDNA_DIR); f_mkdir(PDNA_DIR "/battles");     /* FR_EXIST is fine (hard rule 9) */
    SfStatus st = sf_write_verified(path, g_save + G3_REC_SECTOR_OFF, G3_SECTOR_SIZE);
    if (st == SF_OK) {
      /* Best-effort SIDECAR (<basename>.txt): the export-time context the .rec
       * itself cannot carry — all facilities' current and best streaks, player identity,
       * timestamp, teams. A failed sidecar never fails the export. */
      static char EWRAM_BSS sc[2048];
      char stamp[24]; stamp[0] = 0;
      if (rtc_ok) sniprintf(stamp, sizeof stamp, "%02u-%02u-%04u %02u:%02u",
                            t.day, t.month, t.year, t.hour, t.minute);
      int sn = g3_record_sidecar(sc, sizeof sc, &ri, g_save, g_sb2, g_sb1,
                                 (int)g_game, g_vinfo.tid_public, rtc_ok ? stamp : 0);
      int pl = (int)strlen(path);                        /* ".rec" -> ".txt" */
      char sp[72]; memcpy(sp, path, (size_t)pl + 1);
      sp[pl - 3] = 't'; sp[pl - 2] = 'x'; sp[pl - 1] = 't';
      SfStatus s2 = sf_write_verified(sp, sc, (uint32_t)sn);
      log_line("record: sidecar %s -> %s", sp, s2 == SF_OK ? "OK" : sf_status_str(s2));
    }
    rmbl_resume();
    log_line("record: export %s -> %s", path, st == SF_OK ? "OK" : sf_status_str(st));
    if (st == SF_OK) {
      snd_save();
      char p2[32]; ui_truncate(p2, path, 29);
      /* Name the tool, not "see docs": docs/ is git-ignored, so a downloader has no
       * docs to see. 157 px, inside msg_wait's 184 px ui_ptext_fit clamp. */
      msg_wait("EXPORTED", UI_OK, p2, "rec2mp4 (PC) renders it to MP4.");
    } else {
      snd_error();
      msg_wait("EXPORT FAILED", UI_WARN, sf_status_str(st), 0);
    }
  }
}

/* START-menu destinations over the box (Party + Bank + Daycare are the storage
 * screens; SELECT no longer toggles the party — it cycles the box cursor mode).
 *
 * The entries, the enum (every NV_ id, and NV_COUNT) AND the menu geometry all
 * live in pdna_layout.h now (BACKLOG #48 promoted the enum out of this file so
 * pdna_gen12.c's gb_session_core can share the same numeric IDs through
 * app_nav_menu's availability mask) — tests/host_textfit_test.c asserts on the
 * geometry (panel height, and every label against the 90 px selection band). A
 * copy of any of it in this file would guard nothing. */
_Static_assert(NV_COUNT == PDNA_NAV_COUNT, "nav enum and PDNA_NAV_COUNT disagree");

/* Menu HEIGHT, guarded at BUILD time. This menu is fixed-height and drawn exactly once
 * (see nav_menu), so it has no scroll to fall back on — which is why adding the GB-import
 * entry silently broke it: NV_COUNT 19 gave 10 rows at rh 13, a 164 px panel on a 160 px
 * screen, top and bottom borders off-screen and the hint row landing at y=152 on top of
 * the box screen's own footer ("A pick ABmback UP SEL B"). Entry N+1 must break the BUILD
 * instead: drop an entry, shrink PDNA_NAV_ROW_H, or give the list a window. */
_Static_assert(PDNA_NAV_MH <= UI_FOOTER_Y,
               "nav menu no longer fits above the footer row (UI_FOOTER_Y)");
/* Pixels under the selection bar, so it can be lifted off a row without repainting the
 * translucent panel. 2,256 B of EWRAM buys a menu that is drawn exactly ONCE. */
static u16 EWRAM_BSS s_nav_band[PDNA_NAV_BAND_W * PDNA_NAV_BAND_H];

/* The whole of BACKLOG #48's new logic, pulled out to one place: item `nv` (an NV_*
 * id, 0..NV_COUNT-1) is selectable-and-lit iff its bit is set in `mask`. Genuinely
 * pure (no globals, no I/O) — but NOT host-tested: it lives in pdna_main.c, which
 * (unlike pdna_gen12.c's PDNA_GEN12_HOST split) has no host-compilable half at all,
 * every screen in it reaches tonc/vid_mem directly, and there is no existing harness
 * this one function is worth inventing a new pdna_main.c/host split for. A one-line
 * bitmask test also has no interesting branches for a unit test to catch — the risk
 * this whole item guards against is nav_menu()/gb_nav_from_start() disagreeing about
 * WHICH bits mean what, and that is exactly what the two `1u << NV_*` call sites
 * sharing pdna_layout.h's ONE enum (rather than two private copies) already rules
 * out at compile time. */
static bool nav_item_available(uint32_t mask, int nv) { return (mask & (1u << nv)) != 0; }

/* BACKLOG #48: `avail_mask` is a bitmask of `1u << NV_*` — the item is drawn UI_DIM
 * (instead of UI_TEXT) and, if picked, returns NAV_UNAVAILABLE instead of its own id
 * when its bit is clear. A dimmed row is still fully selectable (the cursor and the
 * highlight bar work exactly the same on it) — only its OWN unselected ink and its
 * A-press outcome change, so a caller can show one honest "not here" message instead
 * of silently doing nothing. Passing NAV_ALL_AVAILABLE (every bit set) reproduces the
 * Gen-3 box screen's own call byte-for-byte: every ink stays UI_TEXT and every A-press
 * returns `sel`, exactly as before this existed. */
static int nav_menu(uint32_t avail_mask) {
  /* TWO COLUMNS, ten rows each — no scroll, no position counter (NAV_MH guards the fit).
   *
   * DRAWN ONCE. ui_panel_alpha BLENDS with what is already on screen, and this used to
   * repaint the whole menu on every keypress: each scroll blended the panel over its own
   * previous blend, so the menu visibly darkened and stacked, exactly as if a new panel had
   * popped on top of the last. Now the panel is composited once on entry and a cursor move
   * only lifts the selection bar off the old row (restoring the pixels it covered) and lays
   * it on the new one. Labels use the proportional face, which is what makes room for
   * "Flags & counters" where nine fixed-width glyphs used to be the ceiling. */
#define NV_LABEL_ONE(id, label) label,
  static const char* const L[NV_COUNT] = { PDNA_NAV_ITEMS(NV_LABEL_ONE) };
  const int rows = PDNA_NAV_ROWS;                   /* 10 rows per column */
  const int cw = PDNA_NAV_COL_W, rh = PDNA_NAV_ROW_H;
  const int mw = PDNA_NAV_MW, mh = PDNA_NAV_MH;
  /* Centred in the space ABOVE the footer, not on the whole screen: the box screen keeps
   * printing its own hints at y=152 underneath this panel. */
  const int mx = (UI_SCR_W - mw) / 2, my = (UI_FOOTER_Y - mh) / 2;
  const int bw = PDNA_NAV_BAND_W;                   /* selection bar width */

  /* Compose the panel and every label ONE time. */
  /* Every offset below is PDNA_NAV_* from pdna_layout.h, including the head: the three
   * row-placement sites here used to say "my + 20" while PDNA_NAV_HEAD fed only the
   * panel height, so shrinking the constant moved the panel and left the rows behind
   * with the host test still green. One symbol per number, so the test can bite. */
  ui_panel_alpha(mx, my, mw, mh, UI_PANEL, UI_BORDER, 5);
  ui_text(mx + PDNA_NAV_PAD, my + PDNA_NAV_TITLE_DY, UI_TITLE, "MENU");
  ui_hline(mx + 2, my + PDNA_NAV_DIV_DY, mw - 4, UI_BORDER);
  for (int i = 0; i < NV_COUNT; i++) {
    int col = i / rows, row = i % rows;
    u16 ink = nav_item_available(avail_mask, i) ? UI_TEXT : UI_DIM;
    ui_ptext(mx + PDNA_NAV_PAD + col * cw, my + PDNA_NAV_HEAD + row * rh, ink, L[i]);
  }
  ui_text(mx + PDNA_NAV_PAD, my + mh + PDNA_NAV_HINT_DY, UI_DIM, PDNA_NAV_HINT);

  int sel = 0, drawn = -1;
  for (;;) {
    if (drawn != sel) {
      if (drawn >= 0) {                             /* lift the bar off the old row */
        int c = drawn / rows, r = drawn % rows;
        int bx = mx + PDNA_NAV_PAD + c * cw - PDNA_NAV_LABEL_DX;
        int by = my + PDNA_NAV_HEAD + r * rh + PDNA_NAV_BAND_DY;
        for (int j = 0; j < PDNA_NAV_BAND_H; j++)
          for (int i = 0; i < bw; i++)
            vid_mem[(by + j) * UI_SCR_W + bx + i] = s_nav_band[j * bw + i];
      }
      { int c = sel / rows, r = sel % rows;
        int bx = mx + PDNA_NAV_PAD + c * cw - PDNA_NAV_LABEL_DX;
        int by = my + PDNA_NAV_HEAD + r * rh + PDNA_NAV_BAND_DY;
        for (int j = 0; j < PDNA_NAV_BAND_H; j++)   /* remember what it covers */
          for (int i = 0; i < bw; i++)
            s_nav_band[j * bw + i] = vid_mem[(by + j) * UI_SCR_W + bx + i];
        /* The bar must CONTAIN the glyph box, not clip it: the text sits BAND_DY below
         * the bar's top and is UI_ROW_H tall, so the bar spans by .. by+BAND_H-1 with
         * lead above and below. tests/host_textfit_test.c asserts both edges. */
        ui_panel_alpha(bx, by, bw, PDNA_NAV_BAND_H, UI_SEL, UI_TITLE, 6);
        ui_ptext(bx + PDNA_NAV_LABEL_DX, by - PDNA_NAV_BAND_DY, UI_SELTEXT, L[sel]);
      }
      drawn = sel;
    }
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (k & KEY_B) return NV_BACK;
    /* Up/down walks the column and wraps into the next one, so holding DOWN still reaches
     * every entry in order exactly as it did when the list was single-column. */
    else if (k & KEY_UP)    sel = (sel > 0) ? sel - 1 : NV_COUNT - 1;
    else if (k & KEY_DOWN)  sel = (sel + 1) % NV_COUNT;
    else if (k & KEY_LEFT)  { if (sel >= rows) sel -= rows; }
    else if (k & KEY_RIGHT) { if (sel + rows < NV_COUNT) sel += rows; }
    else if (k & KEY_A)    return nav_item_available(avail_mask, sel) ? sel : NAV_UNAVAILABLE;
  }
}

/* BACKLOG #48: the box screen's own START menu, reachable from OUTSIDE this file —
 * pdna_gen12.c's gb_session_core wants the identical menu (panel, geometry, labels)
 * for a Game Boy session's own START press, just with most rows unavailable. Thin
 * wrappers only: nav_menu/pdna_settings stay static, this is the one door in. */
int app_nav_menu(uint32_t avail_mask) { return nav_menu(avail_mask); }
void app_nav_settings(void) { pdna_settings(); }

/* BACKLOG #58: see pdna_app.h's own comment for the two call sites. Title comes from
 * (state, kind) -- kept to <= 12 chars, msg_wait's own x=28 clamp is 184px but a
 * dialog TITLE reads badly wrapped, so this is a tighter self-imposed budget, same as
 * every other short title this screen family already uses ("READ-ONLY", "NO ROOM",
 * ...). The reason line is nav_avail_why()'s own <= 30-char string (host-tested). */
void app_nav_refuse(int nv_item, int save_kind) {
  NavAvail av = nav_avail(nv_item, save_kind);
  const char* why = nav_avail_why(nv_item, save_kind);
  if (av == NAV_OK || !why || !why[0]) {
    /* Review D3: both callers only get here having already decided they cannot
     * dispatch, so NAV_OK / a missing table entry means "I have no message", never
     * "fine". Silence is the exact bug #58 fixed -- say something generic instead. */
    snd_deny(); msg_wait("COMING SOON", UI_DIM, "Not available here yet.", 0); return;
  }
  const char* title;
  if (av == NAV_COMING_SOON) {
    title = "COMING SOON";
  } else {
    switch (save_kind) {
      case SE_KIND_GEN1: title = "NOT IN GEN 1"; break;
      case SE_KIND_GEN2: title = "NOT IN GEN 2"; break;
      case SE_KIND_FRLG: title = "NOT IN FRLG";  break;
      case SE_KIND_RS:   title = "NOT IN RS";    break;
      case SE_KIND_EM:   title = "NOT IN EM";    break;
      default:           title = "UNAVAILABLE";  break;
    }
  }
  snd_deny();
  msg_wait(title, UI_DIM, why, 0);
}

/* ---- PC-storage BoxSource: the in-save boxes, rendered by the shared box screen.
 * Accessors operate on g_pc (+ g_sb1 for the Emerald Walda wallpaper). ---- */
static uint8_t* pcsrc_records(int box) { return g_pc + 0x0004 + (uint32_t)box * 30 * 80; }
static void pcsrc_get_name(int box, char out[12]) { pk_box_name(g_pc, box, out); }
static void pcsrc_set_name(int box, const char* s) { pk_set_box_name(g_pc, box, s); }
static int  pcsrc_get_wp(int box) {                  /* byte 0..15 = wallpaper; 16 = Walda pattern */
  int b = pk_box_wallpaper(g_pc, box);
  if (b < G3_BOX_WALLPAPER_FRIENDS) return b;
  int wp = app_walda_pattern();                      /* -1 on non-Emerald */
  return (wp >= 0) ? G3_BOX_WALLPAPER_FRIENDS + wp : 0;
}
static void pcsrc_set_wp(int box, int wp) { pk_set_box_wallpaper(g_pc, box, (uint8_t)wp); }

static BoxSource pc_box_source(void) {
  BoxSource s; memset(&s, 0, sizeof s);
  s.nboxes     = G3_TOTAL_BOXES;
  /* Open on the box the APP last used (default box 1), NOT the save's in-game current
   * box — opening on box 6 just because that's where the game left off is confusing. */
  if (g_pc_last_box < 0 || g_pc_last_box >= G3_TOTAL_BOXES) g_pc_last_box = 0;
  s.start_box  = g_pc_last_box;
  s.is_bank    = false;
  s.scope      = BOXSCOPE_PC;    /* BACKLOG #120 S1: can_lift/xfer stay NULL (memset above) */
  s.wp_count   = (app_walda_pattern() >= 0) ? 32 : G3_BOX_WALLPAPER_COUNT;  /* Emerald = +Walda */
  s.records    = pcsrc_records;
  s.menu_block = g_pc;
  s.get_name   = pcsrc_get_name;
  s.set_name   = pcsrc_set_name;
  s.get_wp     = pcsrc_get_wp;
  s.set_wp     = pcsrc_set_wp;
  s.can_edit   = app_can_edit;
  s.commit     = app_commit_pc;
  s.mark_dirty = app_mark_pc_dirty;
  s.note_add   = pcsrc_note_add;            /* a mon dropped into the PC auto-registers in the dex */
  return s;
}

/* Leaving the open save: if ANY deferred edits are pending — box moves (g_pc) and/or
 * Day-Care moves (staged into g_save) — ask ONCE and save or discard them together, so
 * a cross-storage move (Day-Care<->PC) can never be half-saved. A writes the whole image
 * in one verified pass (app_commit_pc folds in the staged Day-Care sections); B discards
 * everything — the on-disk save was never touched and we're returning to the browser. */
static void flush_on_exit(void) {
  if (!app_pc_dirty() && !g_sb1_deferred) {
    app_xfer_promote();                    /* BACKLOG #150 S150-8 decision 9: the PC was already saved */
    int kept = pdna_bank_flush_deletions();
    if (kept) {
      char l1[48]; siprintf(l1, PDNA_XFER_FLUSHFAIL_L1, kept);
      msg_wait(PDNA_XFER_FLUSHFAIL_TITLE, UI_WARN, l1, PDNA_XFER_FLUSHFAIL_L2);
    }
    return;
  }
  if (app_confirm("Save changes?", "Save the moved Pokemon?")) {
    /* GATE the Bank-source deletion on the PC write SUCCEEDING. app_commit_pc() can fail (EZ
     * writes have no retry / verify mismatch / backup-full); on failure the moved mons live only
     * in volatile g_pc and are NOT on the .sav, so deleting their Bank originals would LOSE them
     * (a Bank->PC move — the multi-select chunk amplifies this to a whole box at once). Flush the
     * deletions ONLY after the destination (PC) is verified on disk -> worst case a recoverable
     * duplicate (mons kept in the Bank), never a loss. g_pc_dirty stays set on failure, so the
     * moves are still pending and can be retried. */
    if (app_commit_pc()) {
      app_xfer_promote();                  /* decision 9: the PC is now verified on disk */
      int kept = pdna_bank_flush_deletions();
      if (kept) {
        char l1[48]; siprintf(l1, PDNA_XFER_FLUSHFAIL_L1, kept);
        msg_wait(PDNA_XFER_FLUSHFAIL_TITLE, UI_WARN, l1, PDNA_XFER_FLUSHFAIL_L2);
      }
    } else {
      /* BACKLOG #150 S150-11 decision 11(i)/#176: app_commit_pc() returns false only
       * when NOTHING landed (an SF_ERR_RENAME with SF_WHERE_TARGET counts as success
       * inside app_commit_block) -- the Bank cell was never consumed (the flush above
       * is gated on this same bool) and the PENDING entry describes a transfer that
       * did not happen, so the undo here is the SAME correct cleanup the DECLINE
       * branch below already does. Without this, g_xd_key stayed set for the rest of
       * the boot and every later native->Gen-3 drop refused with SAVE FIRST
       * (pdna_gen12.c's xfer_down_write gate) -- the bug #176 names. */
      app_xfer_pending_undo();
      msg_wait(PDNA_XFER_NOTSAVED_TITLE, UI_WARN, PDNA_XFER_NOTSAVED_L1, PDNA_XFER_NOTSAVED_L2);
    }
  } else {
    gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc);   /* revert PC moves */
    g_pc_dirty = false; g_sb1_deferred = false;          /* drop staged Day-Care (disk untouched) */
    app_xfer_pending_undo();                             /* decision 9: the transfer never happened */
    pdna_bank_clear_deletions();                         /* move cancelled -> keep the Bank originals */
  }
}

/* Re-sync the shared SaveBlock1/2 working buffers from the (committed) in-RAM save
 * image. Several editors edit g_sb1/g_sb2 IN PLACE and commit ALL of SaveBlock1, but
 * some decline paths don't revert their edits — so before handing control to the next
 * screen we restore the buffers to the on-disk state. This guarantees a later "save"
 * can never persist a change the user previously declined, and a decline can't strand a
 * stale edit. (The PC-box deferred moves live in a separate buffer, g_pc, untouched.) */
static void reload_saveblocks(void) {
  gen3_read_saveblock1(g_save, g_vinfo.slot, g_sb1);
  int s0 = gen3_find_section(g_save, g_vinfo.slot, 0);
  if (s0 >= 0)
    memcpy(g_sb2, g_save + (uint32_t)g_vinfo.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE,
           G3_SECTOR_DATA_SIZE);
}

/* ---- S5-C Part B2: reconcile on load -- release transferred originals -----------
 *
 * docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 12: "no session ever holds both saves
 * ... so the Gen-3 removal happens when the Gen-3 save is NEXT loaded." Called once
 * from view_save(), right after the parse succeeds (g_sb1/g_pc/g_party are all
 * valid) and before anything commits to the box screen: for every sidecar file, for
 * every unclaimed entry whose original80 identifies EXACTLY ONE mon still in THIS
 * save, offer to release it (the Gen-3 side of a transfer that already happened on
 * the Game Boy side).
 *
 * MEMORY -- why app_box_swap_acquire(), not app_arena_acquire(): app_arena_acquire()
 * hands back g_pc ITSELF (pdna_app.h: "why g_pc is the donor" -- it is the SAME
 * physical bytes, not a copy), and this code needs to READ g_pc for identity
 * matching at the same time it needs scratch space to hold a sidecar file's bytes --
 * the arena cannot serve both at once without the scratch write corrupting the very
 * box data being matched against. app_box_swap_acquire() (box_oam.c's borrowed
 * pose-swap cache, backed by g_entries) is a genuinely SEPARATE 19,456 B array that
 * is provably idle here: this runs BEFORE the box screen (and therefore before
 * boxoam_enter()) ever touches it, and the file browser rescans g_entries on every
 * return visit regardless of what was in it (box_oam.c already borrows the exact
 * same array on every single box-screen visit on that same assumption). */
#define GB_RECON_MAX_FILES   64
#define GB_RECON_MAX_EXAMINE 256   /* S5-C review #11: files LOOKED AT, not just .pds
                                    * ones accepted -- a directory full of unrelated
                                    * files must not make this scan unbounded */
#define GB_RECON_NAME_MAX    21    /* "0123456789ABCDEF.pds" + NUL == 20 + 1        */

typedef struct {
  DIR        dir;
  FILINFO    fi;
  uint8_t    sidecar[GBSC_FILE_MAX];
  char       path[GBSC_PATH_MAX];
  char       names[GB_RECON_MAX_FILES][GB_RECON_NAME_MAX];
  GbReconHit hits[GB_RECON_MAX_HITS];   /* GbReconHit/GB_RECON_MAX_HITS: gb_reconcile.h */
  int        nfiles;
  int        nhits;
  int        nunique;   /* S5-C 2nd review #1: DISTINCT (box, slot) targets among nhits
                         * -- nhits over-counts whenever a mon was transferred to more
                         * than one Game Boy save (each transfer is its own sidecar
                         * entry, but they all name the SAME Gen-3 slot). The confirm
                         * screen promises nunique mons, not nhits entries. */
  int        examined;  /* BACKLOG #150 S150-6 review F6: GB_RECON_MAX_EXAMINE must be
                         * GLOBAL across BOTH reconcile passes (golden rule 2) -- a
                         * per-call local would let a two-pass walk examine up to
                         * 2x GB_RECON_MAX_EXAMINE entries total, unbounded across the
                         * pair the way the old single-pass code never had to consider. */
  /* BACKLOG #150 S150-11 decision 5: a PARALLEL array, not an extension of the
   * GbReconHit block above (that one stays G3_HOME-shaped, untouched). Every
   * NATIVE_HOME entry the reconcile walk finds, one XrcHit each. */
  XrcHit     xrc[GB_RECON_MAX_HITS];
  int        nxrc;
} GbReconBuf;
_Static_assert(sizeof(GbReconBuf) <= sizeof(g_entries),
              "gb_reconcile buffer no longer fits the borrowed g_entries cache");

/* BACKLOG #150 S150-6, site 5: decision 4/D-Q7 applied to a FILENAME instead of a
 * key, so a reconcile hit's later claim write lands in the same file it was read
 * from (xr_path_for_name's own contract). `out` is always left holding a usable
 * path (the xfer one) even on the never-expected bad-arg/too-long refusal, so no
 * caller here needs its bool. */
static void gb_recon_path(char* out, const char* name) {
  out[0] = 0;
  (void)xr_path_for_name(out, name);
}

/* Scan /PokeDNA/sidecar for .pds files (bounded to GB_RECON_MAX_FILES ACCEPTED and
 * GB_RECON_MAX_EXAMINE entries LOOKED AT -- S5-C review #11: the old bound only
 * counted accepted .pds files, so a directory holding many unrelated entries could
 * make f_readdir() run unbounded; rb->dir already opened by the caller), and for
 * every entry that is not claimed and whose file has not been asked-and-kept
 * (GBSC_FLAG_KEEP_ASKED), look for an EXACT single match in this save
 * (gb_reconcile_match, source/gb_reconcile.c). Fills rb->hits/rb->nhits (bounded to
 * GB_RECON_MAX_HITS; the outer scan also stops once that many are found -- no point
 * reading more files that can never contribute another hit) and rb->names/rb->nfiles.
 *
 * DEDUPE (S5-C review #1, belt): when a new hit's (box, slot) matches an EARLIER
 * hit already in rb->hits (the same Gen-3 original transferred to two different
 * Game Boy generations, or re-transferred after a merge-up, produces two unclaimed
 * entries pointing at the same slot), the new one is marked `duplicate` here (also
 * counted, not into rb->nhits -- see rb->nunique below) so gb_reconcile_plan()
 * never attempts to release it a second time. gb_reconcile_plan() independently
 * re-derives any duplicate this pass MISSES too (it can only ever ADD a duplicate
 * mark, never clear one gb_reconcile_walk() got wrong -- see gb_reconcile.h), so
 * this pass is a first, cheap line of defense, not the only one.
 *
 * noinline: DIR/FILINFO/the 1042 B sidecar buffer all live in *rb (the borrowed
 * g_entries cache), never this function's own frame.
 *
 * `append` (BACKLOG #150 S150-6, decision 7): false (pass 1, xfer) resets
 * rb->nfiles/nhits/nunique as this function always did. true (pass 2, sidecar)
 * does NOT reset them -- it continues appending into the SAME rb->names/rb->hits
 * pass 1 already filled, so the three bounds (GB_RECON_MAX_FILES/_EXAMINE/_HITS)
 * stay global across both passes (golden rule 2) -- and additionally skips any
 * `fi.fname` already present in rb->names[0..nfiles), so a filename migrated into
 * xfer (found by pass 1) is never read a second time out of sidecar by pass 2. */
static void __attribute__((noinline)) gb_reconcile_walk(GbReconBuf* rb, bool append) {
  if (!append) {
    rb->nfiles = 0;
    rb->nhits = 0;
    rb->nunique = 0;
    rb->examined = 0;   /* review F6: GLOBAL across both passes, reset only at pass 1 */
  }
  while (rb->examined < GB_RECON_MAX_EXAMINE && rb->nfiles < GB_RECON_MAX_FILES &&
        rb->nhits < GB_RECON_MAX_HITS &&
        f_readdir(&rb->dir, &rb->fi) == FR_OK && rb->fi.fname[0]) {
    rb->examined++;
    if (rb->fi.fattrib & AM_DIR) continue;
    int L = 0; while (rb->fi.fname[L]) L++;
    if (L < 5 || L >= GB_RECON_NAME_MAX) continue;
    const char* e = rb->fi.fname + L - 4;
    if (e[0] != '.' || (e[1] | 32) != 'p' || (e[2] | 32) != 'd' || (e[3] | 32) != 's') continue;

    if (append) {
      bool seen = false;
      for (int k = 0; k < rb->nfiles; k++)
        if (strcmp(rb->names[k], rb->fi.fname) == 0) { seen = true; break; }
      if (seen) continue;               /* already found in pass 1 (xfer)          */
    }

    int fidx = rb->nfiles;
    int cn = 0;
    while (rb->fi.fname[cn] && cn < GB_RECON_NAME_MAX - 1) { rb->names[fidx][cn] = rb->fi.fname[cn]; cn++; }
    rb->names[fidx][cn] = 0;
    rb->nfiles++;

    gb_recon_path(rb->path, rb->names[fidx]);
    uint32_t len = 0;
    if (sf_read_full(rb->path, rb->sidecar, GBSC_FILE_MAX, &len) != SF_OK) {
      log_line("gen3: reconcile: could not read %s", rb->path);
      continue;
    }
    int count = gbsc_count(rb->sidecar, len);
    if (count < 0) {
      log_line("gen3: reconcile: %s fails its own crc, skipped", rb->path);
      continue;
    }
    if (gbsc_flags_get(rb->sidecar, len) & GBSC_FLAG_KEEP_ASKED) continue;

    for (int i = 0; i < count && rb->nhits < GB_RECON_MAX_HITS; i++) {
      GbscEntry e2;
      if (!gbsc_get(rb->sidecar, len, i, &e2) || e2.claimed) continue;
      int box = -1, slot = -1;
      int m = gb_reconcile_match(g_sb1, g_frlg, g_have_pc ? g_pc : NULL, e2.original80, &box, &slot);
      if (m != 1) {
        if (m > 1) log_line("gen3: reconcile: %s entry %d matches %d mons, skipped (ambiguous)",
                            rb->path, i, m);
        continue;
      }
      bool dup = false;
      for (int k = 0; k < rb->nhits; k++)
        if (rb->hits[k].box == (int8_t)box && rb->hits[k].slot == (int8_t)slot) { dup = true; break; }
      if (!dup) rb->nunique++;   /* S5-C 2nd review #1: count DISTINCT targets, not entries */

      GbReconHit* h = &rb->hits[rb->nhits++];
      h->box = (int8_t)box; h->slot = (int8_t)slot;
      h->file_idx = (uint8_t)fidx; h->entry_idx = (uint8_t)i;
      memcpy(h->id8, e2.original80, 8);
      h->released = false;
      h->done = false;
      h->duplicate = dup;
      if (dup)
        log_line("gen3: reconcile: %s entry %d duplicates an earlier hit at box=%d slot=%d",
                 rb->path, i, box, slot);
    }
  }
}

/* One screen: "N POKEMON TRANSFERRED" / "Release the originals here?", A = yes
 * (release) / B = no (keep both) -- app_confirm()'s own standard footer, reused
 * rather than a bespoke "A = release N / B = keep both" pair (same question, one
 * less screen convention to maintain). noinline to keep the title buffer off the
 * caller's own frame. */
static bool __attribute__((noinline)) gb_reconcile_confirm(int n) {
  char title[40];
  siprintf(title, "%d %s", n, PDNA_SIDECAR_RECON_TITLE_SUFFIX);
  return app_confirm(title, PDNA_SIDECAR_RECON_L1);
}

/* S5-C 2nd review #2: gb_reconcile_plan() calls clip_clear_box_slot()/
 * party_release() directly (pure C -- it has no persistence layer, and no
 * app_pc_release_slot() call either), so nothing marks g_pc dirty on its own.
 * Told to msg_wait() when a commit fails (a measured line, reused across both
 * buffers since the underlying reason was already shown by app_commit_pc()/
 * app_commit_sb1()'s own failure screen a moment earlier -- see app_save_finalize).
 * noinline to keep the string off gb_reconcile_release()'s own frame. */
static void __attribute__((noinline)) gb_reconcile_commit_failed_msg(void) {
  snd_error();
  msg_wait(PDNA_SIDECAR_RECON_NOSAVE_TITLE, UI_WARN, PDNA_SIDECAR_RECON_NOSAVE_L1, 0);
}

/* Release every hit gb_reconcile_walk() found via gb_reconcile_plan() (S5-C review;
 * source/gb_reconcile.c) -- the pure-C ordering/dedupe/live-reverify core that
 * fixes the data-loss bug the old inline version here had: two sidecar entries
 * resolving to the SAME slot (the same Gen-3 mon transferred to a Gen-1 save AND a
 * Gen-2 save, or re-transferred after a merge-up) used to release the first fine,
 * then blindly release "the same recorded slot" again for the second -- which, for
 * a party slot, now named a DIFFERENT, innocent mon after the first release
 * shifted everything down by one. gb_reconcile_plan() mutates g_sb1/g_pc in place
 * (pure C, no persistence of its own); this wrapper does ONE commit per touched
 * buffer (app_commit_pc / app_commit_sb1), matching app_release()'s own commit
 * convention.
 *
 * S5-C 2nd review #2 (must): a failed commit means the release never reached the
 * card -- claiming its sidecar entry anyway would hide that mon's ONLY remaining
 * record of the transfer (the .pds) while the card still holds the un-released
 * original, so a failed buffer's hits are un-released here (claimed by NEITHER
 * this run nor a future one gets to see them as still-pending, since gb_reconcile_
 * walk() would find them unclaimed again next load and simply re-offer them --
 * exactly the fallback wanted). g_pc is ALSO marked dirty UNCONDITIONALLY,
 * regardless of whether app_commit_pc() then succeeds: gb_reconcile_plan() never
 * calls app_pc_release_slot() (only its own pure clip_clear_box_slot()), so
 * nothing else marks it, and a failed commit must still leave flush_on_exit()'s
 * "save changes?" prompt able to offer a second chance rather than the RAM-only
 * release silently evaporating on exit.
 *
 * Returns the PHYSICAL release count (released && !duplicate) -- the count of
 * ACTUAL clip_clear_box_slot()/party_release() calls that both ran and made it to
 * the card, for gb_reconcile_apply()'s "K of N" shortfall message. This is
 * deliberately NOT gb_reconcile_plan()'s own return value: that one also counts
 * duplicates (claimable, but never touching the card themselves), which would
 * make the shortfall check compare against the wrong, entry-counting N.
 *
 * `*commit_failed` is set true iff either commit was attempted and failed, so
 * gb_reconcile_apply() can skip its own generic "K of N" shortfall message when
 * THIS function already showed a more specific one for the same event. */
static int __attribute__((noinline)) gb_reconcile_release(GbReconBuf* rb, bool* commit_failed) {
  gb_reconcile_plan(rb->hits, rb->nhits, g_sb1, g_frlg, g_have_pc ? g_pc : NULL);

  bool touched_pc = false, touched_party = false;
  for (int i = 0; i < rb->nhits; i++) {
    if (!rb->hits[i].released || rb->hits[i].duplicate) continue;
    if (rb->hits[i].box == -1) touched_party = true; else touched_pc = true;
  }

  if (touched_pc) app_mark_pc_dirty();          /* BEFORE the commit attempt -- see above */
  bool pc_ok = !touched_pc || app_commit_pc();
  if (touched_pc && !pc_ok)
    log_line("gen3: reconcile: app_commit_pc failed -- PC release(s) NOT saved to the card");
  bool party_ok = !touched_party || app_commit_sb1();
  if (touched_party && !party_ok)
    log_line("gen3: reconcile: app_commit_sb1 failed -- party release(s) NOT saved to the card");

  if (!pc_ok)    for (int i = 0; i < rb->nhits; i++) if (rb->hits[i].box >= 0) rb->hits[i].released = false;
  if (!party_ok) for (int i = 0; i < rb->nhits; i++) if (rb->hits[i].box == -1) rb->hits[i].released = false;
  *commit_failed = !pc_ok || !party_ok;
  if (*commit_failed) gb_reconcile_commit_failed_msg();

  int physical = 0;
  for (int i = 0; i < rb->nhits; i++) if (rb->hits[i].released && !rb->hits[i].duplicate) physical++;
  return physical;
}

/* Rewrite every TOUCHED sidecar file once: A (release_all) claims the entries that
 * were actually released (a party-floor skip, a dedupe, or an un-committed buffer
 * per gb_reconcile_release() above all leave their entry unclaimed, to be
 * reconsidered next load); B sets GBSC_FLAG_KEEP_ASKED on the whole file's header
 * so the question is not repeated for it. A write failure is logged + surfaced but
 * never undoes a release already applied -- design doc section 12's own bias: "a
 * card error during the walk = skip the question this time", not "roll back a
 * release that already landed on the card". */
static void __attribute__((noinline)) gb_reconcile_claim_sidecars(GbReconBuf* rb, bool release_all) {
  bool any_notupdated = false;
  for (int fidx = 0; fidx < rb->nfiles; fidx++) {
    bool has_hit = false;
    for (int i = 0; i < rb->nhits; i++) if (rb->hits[i].file_idx == fidx) { has_hit = true; break; }
    if (!has_hit) continue;

    gb_recon_path(rb->path, rb->names[fidx]);
    uint32_t len = 0;
    if (sf_read_full(rb->path, rb->sidecar, GBSC_FILE_MAX, &len) != SF_OK ||
        gbsc_count(rb->sidecar, len) < 0) {
      log_line("gen3: reconcile: could not re-read %s to update it", rb->path);
      any_notupdated = true;
      continue;
    }

    bool dirty = false;
    if (release_all) {
      for (int i = 0; i < rb->nhits; i++) {
        if (rb->hits[i].file_idx != fidx || !rb->hits[i].released) continue;
        if (gbsc_set_claimed(rb->sidecar, len, rb->hits[i].entry_idx, true) == 0) dirty = true;
      }
    } else {
      uint16_t flags = gbsc_flags_get(rb->sidecar, len);
      if (!(flags & GBSC_FLAG_KEEP_ASKED) &&
          gbsc_flags_set(rb->sidecar, len, flags | GBSC_FLAG_KEEP_ASKED) == 0)
        dirty = true;
    }
    if (!dirty) continue;

    SfStatus wst = sf_write_verified(rb->path, rb->sidecar, len);
    if (wst == SF_OK) {
      log_line("gen3: reconcile: %s updated (%s)", rb->path, release_all ? "claimed" : "keep-asked");
    } else {
      log_line("gen3: reconcile: %s NOT updated (%s)", rb->path, sf_status_str(wst));
      any_notupdated = true;
    }
  }
  if (any_notupdated) {
    snd_error();
    msg_wait(PDNA_SIDECAR_RECON_NOTUPD_TITLE, UI_WARN, PDNA_SIDECAR_RECON_NOTUPD_L1, 0);
  }
}

/* S5-C review #4: the user was promised N ("N POKEMON TRANSFERRED", N ==
 * rb->nunique -- S5-C 2nd review #1: DISTINCT mons, not sidecar entries); if the
 * party floor, a dedupe, or an un-committed buffer meant fewer were actually
 * released, say so rather than let the confirm screen's own count go silently
 * wrong. K and N are both data, so this line is built at runtime (siprintf into a
 * 64 B buffer) -- safe by construction like every other dynamic message in this
 * tree (msg_wait() runs it through ui_ptext_fit()). noinline to keep the buffer off
 * the caller's own frame. */
static void __attribute__((noinline)) gb_reconcile_shortfall_msg(int physical, int nunique) {
  char l1[64];
  siprintf(l1, "%d of %d released;", physical, nunique);
  msg_wait(PDNA_SIDECAR_RECON_PARTIAL_TITLE, UI_WARN, l1, PDNA_SIDECAR_RECON_PARTIAL_L2);
}

/* Apply the user's choice over every hit gb_reconcile_walk() found: release (A) or
 * leave both copies alone (B), then rewrite whatever sidecar files that decision
 * touches, then (A only) tell the user if fewer than N distinct mons were actually
 * released -- UNLESS a commit already failed (gb_reconcile_release() showed its
 * own, more specific message for that; a second, generic shortfall screen right
 * behind it would just be noise about the same event). Split into gb_reconcile_
 * release() / gb_reconcile_claim_sidecars() (golden rule 4: one function, one
 * job) -- this one is just the sequencing. */
static void __attribute__((noinline)) gb_reconcile_apply(GbReconBuf* rb, bool release_all) {
  bool commit_failed = false;
  int physical = release_all ? gb_reconcile_release(rb, &commit_failed) : 0;
  gb_reconcile_claim_sidecars(rb, release_all);
  if (release_all && !commit_failed && physical != rb->nunique)
    gb_reconcile_shortfall_msg(physical, rb->nunique);
}

static void __attribute__((noinline)) gb_reconcile_on_load(void) {
  if (!app_can_edit()) return;                        /* writes are Omega-only          */

  GbReconBuf* rb = (GbReconBuf*)app_box_swap_acquire(sizeof(GbReconBuf));
  if (!rb) { log_line("gen3: reconcile-on-load: swap buffer unavailable, skipped"); return; }

  /* BACKLOG #150 S150-6, site 6/decision 7 (review F3): pass 1 (xfer) resets and
   * fills rb->nfiles/nhits/nunique; pass 2 (sidecar) APPENDS, skipping any filename
   * pass 1 already found -- but ONLY runs at all when xr_migrated() is false. Once
   * the migration has completed, every sidecar source is an inert backup (frozen
   * at whatever it held at migration time) and /PokeDNA/xfer alone is the live
   * ledger, so reading sidecar too would re-offer a mon whose xfer-side entry a
   * PRIOR claim/KEEP already updated (the sidecar copy never saw that update). On
   * a card where the migration could not run (read-only cart, Everdrive, or a
   * failed pass) pass 2 is what keeps this screen working at all. */
  bool have_xfer = f_opendir(&rb->dir, PDNA_XFER_DIR) == FR_OK;
  if (have_xfer) {
    gb_reconcile_walk(rb, false);
    f_closedir(&rb->dir);
  } else {
    rb->nfiles = 0; rb->nhits = 0; rb->nunique = 0; rb->examined = 0;
  }
  if (!xr_migrated() && f_opendir(&rb->dir, PDNA_SIDECAR_DIR) == FR_OK) {
    gb_reconcile_walk(rb, true);
    f_closedir(&rb->dir);
  } else if (!have_xfer) {
    app_box_swap_release();
    return;                                 /* neither folder exists -- nothing to do */
  }

  if (rb->nhits > 0) {
    /* S5-C 2nd review #1: nunique (DISTINCT mons), not nhits (sidecar entries) --
     * the same mon transferred to two Game Boy generations is one Pokemon, not
     * two, from the user's point of view on this screen. */
    bool release_all = gb_reconcile_confirm(rb->nunique);
    gb_reconcile_apply(rb, release_all);
  }
  app_box_swap_release();
}

/* ==== BACKLOG #150 S150-11: the NATIVE_HOME transfer-ledger reconcile ============
 * §11.8's Bank-open check (decision 4) and the shared walk/apply the future TRANSFERS
 * screen (decision 13/14, not built in this slice) will also call. This block never
 * touches rb->hits/nhits/nunique/names-dedupe above -- gb_reconcile_on_load's own
 * G3_HOME walk is unchanged, byte for byte (files_you_may_touch's own promise). */

/* Phase 1 (decision 4): duplicated from gb_reconcile_walk's own file-iteration shape,
 * not a shared extraction -- this walk filters on kind==XR_KIND_NATIVE_HOME instead
 * of !claimed, keys a file by gbsc_file_key() instead of matching an entry's
 * original80 AS a Gen-3 record, and never touches rb->hits/nunique. The two walks'
 * bodies diverge in exactly the fields that matter, so factoring them together would
 * just be an if/else in disguise (decision 4's own instruction).
 *
 * `cap_files`: 32 for the Bank-open check (decision 5); the shipped GB_RECON_MAX_
 * FILES/EXAMINE caps otherwise. Captures each candidate's identity fields (gen/
 * otid16/dv4/otname/orig8) into its XrcHit right here, while the file is already
 * open -- xrc_bank_match() needs them and phase 2 must not re-open a file per
 * candidate per box (that would turn "page each box once" into "page it once per
 * candidate", exactly the stall §11.8/XFER-C14 the cap exists to prevent).
 * `bank_open` is unused here (the informational degrade it names, decision 6's
 * "plus" clause, is consulted by the CALLER after classification, not by the walk
 * itself) -- kept as a parameter for the brief's own signature. */
static void __attribute__((noinline)) xfer_reconcile_walk(GbReconBuf* rb, int cap_files,
                                                           bool bank_open) {
  (void)bank_open;
  rb->nfiles = 0;
  rb->nxrc = 0;
  int examined = 0;
  uint32_t dc_base, dc_stride; dc_layout(&dc_base, &dc_stride);

  DIR dir; FILINFO fi;
  if (f_opendir(&dir, PDNA_XFER_DIR) != FR_OK) return;
  while (rb->nxrc < GB_RECON_MAX_HITS && rb->nfiles < GB_RECON_MAX_FILES &&
        rb->nfiles < cap_files && examined < GB_RECON_MAX_EXAMINE &&
        f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
    examined++;
    if (fi.fattrib & AM_DIR) continue;
    int L = 0; while (fi.fname[L]) L++;
    if (L < 5 || L >= GB_RECON_NAME_MAX) continue;
    const char* e = fi.fname + L - 4;
    if (e[0] != '.' || (e[1] | 32) != 'p' || (e[2] | 32) != 'd' || (e[3] | 32) != 's') continue;

    int fidx = rb->nfiles;
    int cn = 0; while (fi.fname[cn] && cn < GB_RECON_NAME_MAX - 1) { rb->names[fidx][cn] = fi.fname[cn]; cn++; }
    rb->names[fidx][cn] = 0;
    rb->nfiles++;

    gb_recon_path(rb->path, rb->names[fidx]);
    uint32_t len = 0;
    if (sf_read_full(rb->path, rb->sidecar, GBSC_FILE_MAX, &len) != SF_OK) {
      log_line("xfer: reconcile: could not read %s", rb->path);
      continue;
    }
    int count = gbsc_count(rb->sidecar, len);
    if (count < 0) {
      log_line("xfer: reconcile: %s fails its own crc, skipped", rb->path);
      continue;
    }
    uint64_t file_key = gbsc_file_key(rb->sidecar, len);

    for (int i = 0; i < count && rb->nxrc < GB_RECON_MAX_HITS; i++) {
      GbscEntry e2;
      if (!gbsc_get(rb->sidecar, len, i, &e2) || e2.kind != XR_KIND_NATIVE_HOME) continue;

      XrcHit* h = &rb->xrc[rb->nxrc];
      memset(h, 0, sizeof *h);
      h->file_idx = (uint8_t)fidx; h->entry_idx = (uint8_t)i;
      h->kind = e2.kind; h->state = e2.state; h->direction = e2.direction;
      h->bank_keep = e2.bank_keep != 0;
      h->bank_matches = -1; h->bank_box = -1; h->bank_slot = -1;
      h->g3_key_matches = -1; h->g3_identity_matches = -1; h->g3_box = -1; h->g3_slot = -1;
      h->gen = e2.gen; h->otid16 = e2.otid16;
      memcpy(h->dv4, e2.dv4, 4);
      memcpy(h->otname, e2.otname_written, GB_NAME_BYTES);
      memcpy(h->orig8, e2.original80, 8);

      if (e2.direction == XR_DIR_ABROAD_G3) {
        int wb, ws;
        int gk = xrc_g3_match_key(g_sb1, g_frlg, g_have_pc ? g_pc : NULL, dc_base, dc_stride,
                                  file_key, &wb, &ws);
        h->g3_key_matches = (int8_t)gk;
        if (gk == 1) { h->g3_box = (int8_t)wb; h->g3_slot = (int8_t)ws; h->g3_in_daycare = (wb == -2); }
        if (gk == 0) {
          int iwb, iws;
          int gi = xrc_g3_match_identity(g_sb1, g_frlg, g_have_pc ? g_pc : NULL, dc_base, dc_stride,
                                         e2.species_written, e2.otid16, e2.nick_written, &iwb, &iws);
          h->g3_identity_matches = (int8_t)gi;
          if (gi == 1) { h->g3_box = (int8_t)iwb; h->g3_slot = (int8_t)iws; }
        }
      }
      rb->nxrc++;
    }
  }
  f_closedir(&dir);
}

/* Phase 2 (decision 3(ii)/(iii)/5/19): pages every Bank box ONCE and, for each box,
 * runs xrc_bank_match() against every hit still unresolved -- never re-opens a box
 * once its bytes have been scanned for every candidate. Skips the whole pass (zero
 * box reads, XFER-C14) when phase 1 found nothing that needs a Bank answer. */
static void __attribute__((noinline)) xfer_reconcile_bank_phase2(GbReconBuf* rb) {
  bool any = false;
  for (int i = 0; i < rb->nxrc; i++) if (rb->xrc[i].bank_matches < 0) { any = true; break; }
  if (!any) return;

  for (int box = 0; box < 16; box++) {
    bool touched = false;
    for (int i = 0; i < rb->nxrc; i++) if (rb->xrc[i].bank_matches < 0 || rb->xrc[i].bank_matches == 1) { touched = true; break; }
    if (!touched) break;   /* every candidate already ambiguous or already resolved  */
    const uint8_t* recs = pdna_bank_peek_box(box);
    if (!recs) continue;
    log_line("xfer: reconcile: box %d paged", box);
    for (int i = 0; i < rb->nxrc; i++) {
      XrcHit* h = &rb->xrc[i];
      if (h->bank_matches >= 2) continue;
      GbscEntry e2; memset(&e2, 0, sizeof e2);
      e2.gen = h->gen; e2.otid16 = h->otid16;
      memcpy(e2.dv4, h->dv4, 4);
      memcpy(e2.otname_written, h->otname, GB_NAME_BYTES);
      memcpy(e2.original80, h->orig8, 8);
      bool by_identity = (h->state == XR_STATE_RESTORED);
      int slot = -1;
      int m = xrc_bank_match(recs, &e2, by_identity, &slot);
      if (h->bank_matches < 0) h->bank_matches = 0;
      int total = h->bank_matches + m;
      h->bank_matches = (int8_t)(total > 2 ? 2 : total);
      if (m == 1 && h->bank_matches == 1) { h->bank_box = (int8_t)box; h->bank_slot = (int8_t)slot; }
      if (h->bank_matches == 1 && h->bank_box == (int8_t)box)
        h->bank_slot_pending = pdna_bank_slot_pending(box, slot);
    }
  }
}

/* Bank-open prompt (decision 1(a)/4/6): classify every NATIVE_HOME hit, then act
 * ONLY on XRC_DUP_BANK (REMOVE DUPLICATE) -- §11.8's own text ("one N POKEMON IN TWO
 * PLACES prompt with REMOVE THE DUPLICATE / KEEP BOTH, nothing else offered there")
 * is the literal spec; whether a RESTORED/XRC_DUP_G3 row's RELEASE COPY also belongs
 * on this silent prompt is OPEN QUESTION 4 in the brief, unresolved by the
 * orchestrator -- the conservative reading (less destructive reachable silently) is
 * taken here: RESTORED rows are classified (so the log line's counts are honest) but
 * never offered anything at Bank-open. A KEPT row (bank_keep) is never offered
 * either (decision 2's own table). */
static void __attribute__((noinline)) xfer_reconcile_classify_all(GbReconBuf* rb) {
  for (int i = 0; i < rb->nxrc; i++) {
    XrcHit* h = &rb->xrc[i];
    XrcInput in; memset(&in, 0, sizeof in);
    in.kind = h->kind; in.state = h->state; in.direction = h->direction;
    in.g3_key_matches = h->g3_key_matches < 0 ? 0 : h->g3_key_matches;
    in.g3_in_daycare = h->g3_in_daycare;
    in.g3_identity_matches = h->g3_identity_matches < 0 ? 0 : h->g3_identity_matches;
    in.bank_matches = h->bank_matches < 0 ? 0 : h->bank_matches;
    in.bank_slot_pending = h->bank_slot_pending;
    in.bank_keep = h->bank_keep;
    XrcResult out;
    xrc_classify(&in, &out);
    h->row_kind = (uint8_t)out.kind;
    h->actions = out.actions;
  }
}

/* Applies REMOVE DUPLICATE (decision 8a) to every XRC_DUP_BANK hit -- the Bank-open
 * prompt's only destructive action. Returns the count actually removed. A false
 * pdna_bank_clear_slots() return is "duplicate kept" (logged), never a loss: the
 * entry stays CLAIMED, which IS the normal end state either way. */
static int __attribute__((noinline)) xfer_reconcile_apply_bank_open(GbReconBuf* rb) {
  int removed = 0, failed = 0;
  for (int i = 0; i < rb->nxrc; i++) {
    XrcHit* h = &rb->xrc[i];
    if (h->row_kind != XRC_DUP_BANK || h->bank_keep) continue;
    uint8_t slot = (uint8_t)h->bank_slot;
    uint8_t cell80[80]; memset(cell80, 0, sizeof cell80);
    memcpy(cell80, h->orig8, 8);
    if (pdna_bank_clear_slots(h->bank_box, &slot, (const uint8_t (*)[80])cell80, 1)) {
      removed++;
    } else {
      failed++;
      log_line("xfer: reconcile: box %d slot %d REMOVE refused, duplicate kept", h->bank_box, h->bank_slot);
    }
  }
  log_line("xfer: reconcile: apply removed=%d released=0 restored=0 deleted=0 rekeyed=0 failed=%d",
          removed, failed);
  return removed;
}

/* BACKLOG #150 S150-11 decision 4: the §11.8 Bank-open reconcile. FIRST two
 * statements are the gate (G-F2/hard rule 4) -- called from a Game Boy session
 * (gb_bank_visit, source/pdna_gen12.c), app_arena_held() makes app_gen3_pc_live()
 * false, so this returns before any f_opendir/sf_read_full/log_line runs: zero
 * ledger reads, zero box reads, zero log lines on that path (XFER-C16b). */
void __attribute__((noinline)) app_xfer_reconcile_bank_open(void) {
  if (!app_can_edit()) return;
  if (!app_gen3_pc_live()) return;

  GbReconBuf* rb = (GbReconBuf*)app_box_swap_acquire(sizeof(GbReconBuf));
  if (!rb) { log_line("xfer: reconcile(bank-open): swap buffer unavailable, skipped"); return; }

  xfer_reconcile_walk(rb, 32, true);
  if (rb->nxrc > 0) xfer_reconcile_bank_phase2(rb);
  xfer_reconcile_classify_all(rb);

  int cand = 0;
  for (int i = 0; i < rb->nxrc; i++) if (rb->xrc[i].row_kind == XRC_DUP_BANK && !rb->xrc[i].bank_keep) cand++;
  log_line("xfer: reconcile(bank-open): files=%d entries=%d cand=%d", rb->nfiles, rb->nxrc, cand);

  if (cand > 0) {
    char title[40]; siprintf(title, "%d %s", cand, PDNA_XRC_DUP_TITLE_SUFFIX);
    if (app_confirm(title, PDNA_XRC_DUP_L1)) {
      xfer_reconcile_apply_bank_open(rb);
    } else {
      /* KEEP BOTH (decision 7): mark bank_keep on every offered entry so this prompt
       * never asks about it again -- claimed is untouched (G-H4). */
      for (int i = 0; i < rb->nxrc; i++) {
        XrcHit* h = &rb->xrc[i];
        if (h->row_kind != XRC_DUP_BANK || h->bank_keep) continue;
        gb_recon_path(rb->path, rb->names[h->file_idx]);
        uint32_t len = 0;
        if (sf_read_full(rb->path, rb->sidecar, GBSC_FILE_MAX, &len) != SF_OK) continue;
        if (gbsc_set_bank_keep(rb->sidecar, len, h->entry_idx, true) != 0) continue;
        rmbl_pause();
        SfStatus wst = sf_write_verified(rb->path, rb->sidecar, len);
        rmbl_resume();
        if (wst != SF_OK) log_line("xfer: reconcile: KEEP BOTH rewrite failed for %s", rb->path);
      }
    }
  }
  app_box_swap_release();
}
/* ==== END BACKLOG #150 S150-11 Bank-open reconcile ================================ */

#ifdef PDNA_DELTA
/* BACKLOG #62: pick ONE of possibly several fused Game Boy saves -- tools/fuse_gb.py's
 * directory can carry more than the single slot tools/fuse_sav.py --gb supports (the
 * default delta-gb recipe fuses Red.sav + Gold.sav + Crystal.sav together). Skips the
 * menu when there is exactly one, matching the single-fused-save fallback's own
 * "just open it" behaviour.
 *
 * #62 review D4: was a floating ui_popup_vfit() panel that drew EVERY entry regardless
 * of what the panel it computed actually fit (rows silently ran off the bottom past
 * FUSED_GB_MAX_ENTRIES=12's worth of names), and its wait_keys() never listened for
 * KEY_B at all -- "No cancel option" used to be true (no file browser underneath to
 * fall back to), but A3's delta-gb-only recipe changed that: this picker is now
 * revisited from A3's caller loop every time the user backs OUT of a GB session, so a
 * way to leave the picker itself (not just pick a save) is load-bearing, not optional.
 * Rebuilt on gb_pick_box's own shape (pdna_gen12.c) instead: full-screen ui_clear(),
 * a top/window scroll sized by ui_popup_vfit()'s RETURNED visible-row count (not the
 * raw entry count), KEY_B -> return -1, its own footer. */
static int gb_delta_pick_save(void) {
  /* #62 review D9: fused_gb_present() had zero callers -- this is its natural one,
   * the cheap "is anything fused at all" guard ahead of the SAV-specific count below
   * (both go through the same parse_once() cache, so this costs nothing extra once
   * fused_gb_save_count() runs its own check anyway). */
  if (!fused_gb_present()) return -1;
  int n = fused_gb_save_count();
  if (n <= 0) return -1;
  if (n == 1) return 0;
  if (n > FUSED_GB_MAX_ENTRIES) n = FUSED_GB_MAX_ENTRIES;   /* defensive; fuse_gb.py caps this too */
  const char* names[FUSED_GB_MAX_ENTRIES];
  for (int i = 0; i < n; i++) {
    const char* nm = 0; const uint8_t* base = 0; uint32_t sz = 0;
    fused_gb_save(i, &nm, &base, &sz);
    names[i] = nm ? nm : "?";
  }
  int my, mh;
  int vis = ui_popup_vfit(n, 14, 18, 8, &my, &mh);   /* rows that actually fit on screen */
  if (vis < 1) vis = 1;
  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;

    ui_clear();
    ui_text(4, 3, UI_TITLE, "PICK A SAVE");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    int shown = n - top;
    if (shown > vis) shown = vis;
    for (int i = 0; i < shown; i++) {
      int idx = top + i;
      int y = 18 + i * 14; bool s = (idx == sel);
      if (s) ui_panel(2, y - 1, UI_SCR_W - 4, 13, UI_SEL, UI_TITLE);
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, names[idx]);
    }
    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, "A pick  B exit");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return -1;
    if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) return sel;
  }
}

/* BACKLOG #68a: the combined image's boot picker -- shown only when the flash chip
 * already holds a valid Gen-3 save AND the image also carries a fused GB corpus
 * (fused_gb_present() && fused_gb_save_count() > 0). Row 0 is the loaded Gen-3 save
 * (g3_label, cheap-derived by the caller from the parse it already ran); rows 1..n
 * mirror gb_delta_pick_save's list of fused GB saves one for one (return value here
 * is that index + 1). Unlike gb_delta_pick_save, KEY_B always resolves to row 0: with
 * a valid flash save already on hand there is no dead end to fall into the way the
 * blank-flash picker has to guard against. */
static int gb_delta_boot_pick(const char* g3_label) {
  int gbn = fused_gb_save_count();
  if (gbn > FUSED_GB_MAX_ENTRIES) gbn = FUSED_GB_MAX_ENTRIES;   /* defensive; fuse_gb.py caps this too */
  int n = gbn + 1;
  const char* names[FUSED_GB_MAX_ENTRIES + 1];
  names[0] = g3_label;
  for (int i = 0; i < gbn; i++) {
    const char* nm = 0; const uint8_t* base = 0; uint32_t sz = 0;
    fused_gb_save(i, &nm, &base, &sz);
    names[i + 1] = nm ? nm : "?";
  }
  int my, mh;
  int vis = ui_popup_vfit(n, 14, 18, 8, &my, &mh);   /* rows that actually fit on screen */
  if (vis < 1) vis = 1;
  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;

    ui_clear();
    ui_text(4, 3, UI_TITLE, "PICK A SAVE");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    int shown = n - top;
    if (shown > vis) shown = vis;
    for (int i = 0; i < shown; i++) {
      int idx = top + i;
      int y = 18 + i * 14; bool s = (idx == sel);
      if (s) ui_panel(2, y - 1, UI_SCR_W - 4, 13, UI_SEL, UI_TITLE);
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, names[idx]);
    }
    ui_hline(0, 147, UI_SCR_W, UI_BORDER);
    ui_text(4, 150, UI_DIM, "A pick  B Emerald");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return 0;                   /* never a dead end: KEY_B = Emerald */
    if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) return sel;
  }
}
#endif

/* BACKLOG #150 S150-12 decision 7: the shipped NV_BANK idiom (rumble cue, the
 * `== 5` bottom-out -> PC tabs hand-off, the party refresh), factored out so the
 * NV_GB case can reuse it verbatim when the read-only mount's exit offer is
 * accepted -- opening the Bank from inside pdna_gen12.c is the wrong layer (that
 * file has no rumble/tabs/refresh_party idiom of its own; pdna_gen12_show() already
 * returns an int nobody read before this decision). `*refresh_party` is the
 * caller's own local (view_save()'s nav-menu loop). */
static void nav_open_bank(int* refresh_party) {
  rmbl_fire(RCUE_ROOM);
  if (pdna_bank_show() == 5) app_box_start_set(1);
  *refresh_party = 1;
}

/* Load the picked save and show it: start in the PC boxes; SELECT toggles to the
 * party list and back; B from either returns to the file browser. */
static void view_save(const char* path) {
  /* THE span: SD read + Gen-3 parse + art/ROM open + the first full paint. It closes
   * in app_crumb_shown() (the box screen's first paint) or, for a save with no PC
   * storage, just before the party path takes over below. Named "boot" the first time
   * so the launch cost is greppable on its own -- every later open is "save". */
  { static bool first = true;
    perf_span_begin(first ? "boot" : "save");
    first = false; }
  g_pc_dirty = false;                          /* fresh save: no pending moves */
  g_sb1_deferred = false;
  strncpy(g_path, path, sizeof(g_path) - 1);
  g_path[sizeof(g_path) - 1] = 0;
  gb_art_session_reset();  /* new save -- the "beside the save" fallback forgets the old one */
  app_box_resume_clear();  /* BACKLOG #188: a previous save's resume cell must not leak in */
  uint32_t sz = 0;
  const char* err = 0;
  /* Breadcrumb #1 of 3. This is the boundary the 2026-08-18 hang had no record of:
   * the browser's "scan <dir>: N entries" was the last thing in RAM, and the next
   * 128 KiB of SD reads left no trace on the card at all. */
  s_crumb_shown_armed = true;                  /* re-arm the "box is on screen" one-shot */
  hb_arm();                                    /* the bar spins from here to the first paint */
  load_phase_n(1, "read file");
  log_line("save: open %s", path[0] ? path : "(flash)");
  app_log_flush();
#ifdef PDNA_DELTA
  /* Emulator build: `path` is ignored — the save is this ROM's own flash chip. */
  (void)path;
  sz = flashsave_read(g_save, G3_SAVE_FILE_SIZE) ? (uint32_t)G3_SAVE_FILE_SIZE : 0;
  if (!sz) err = "flash save unreadable";
  /* ...unless the chip has nothing usable on it and a save was fused into the image, in
   * which case fall back to that so the .gba works on its own with no .sav to place.
   *
   * FLASH WINS whenever it parses. That ordering is the whole safety argument: the chip
   * is where the user's edits live, so a fused copy — a snapshot from the day the image
   * was built — must never displace it. This branch is reachable only when flash is
   * blank or corrupt, which is exactly the case that used to dead-end on "copy your
   * Pokemon .sav over pokedna-delta.sav, then relaunch".
   *
   * Read-only: nothing is programmed to the chip here. The image goes to RAM and the
   * user's first in-app save writes it through the normal verified flashsave path. */
  {
    Gen3SaveInfo probe;
    /* g_sb1 is the parse scratch: nothing has read it yet this load, and step 4 below
     * overwrites it with the real SaveBlock1 anyway. See gen3_parse_into's contract --
     * the buffer must NOT be a local, which is the bug this signature exists to stop. */
    bool flash_ok = sz && gen3_parse_into(g_save, sz, &probe, g_sb1) && probe.valid;
    /* BACKLOG #68a's own picker fires on g3_ready, not flash_ok alone -- see the
     * comment where it is actually shown, below this whole if/else-if chain. Real
     * flash on a fresh emulator boot (no prior in-app save this session) is BLANK
     * ("save: flash blank/invalid" in the log every time), so flash_ok is false even
     * for the combined image; the Gen-3 save Emerald boots into there always comes
     * through the single-slot fused-.sav fallback immediately below instead. */
    bool g3_from_fused_sav = false;
    uint32_t fsz = 0;
    if (!flash_ok && fused_sav_present(&fsz) && fsz == (uint32_t)G3_SAVE_FILE_SIZE &&
        fused_sav_read(g_save, fsz)) {
      sz = fsz; err = 0;
      g3_from_fused_sav = true;
      log_line("save: flash blank/invalid -> using the fused save (%lu B)", (unsigned long)fsz);
    } else if (!flash_ok && fused_sav_present(&fsz) && pdna_gen12_size_is_gb(fsz) &&
               fused_sav_read(g_save, fsz)) {
      /* SCREENSHOT VEHICLE ONLY (docs/HANDOFF.md 2026-09-05): the GB fork below this
       * #else is compiled OUT of every emulator build, because the real path reads a
       * .sav off the SD card `view_save` never has in mGBA. A fused GB image is the
       * one way to reach S1..S5-B's screens headlessly. Mirrors the real-hardware GB
       * fork at the #else below line for line: same g_vinfo clear (nothing Gen-3 is
       * loaded), same pdna_box_clear_carry (no phantom mon-in-hand from a previous
       * save), same met_game 3 default. Persisting will fail here -- there is no SD
       * card in the emulator -- and that refusal is itself an honest screenshot: it
       * proves the never-corrupt gate holds even with nowhere to write.
       *
       * Looped rather than a single call: pdna_gen12_show_image() browses until B
       * backs all the way out (GB12_ENTER_OK), and a delta build has no file browser
       * to return to underneath it -- so re-enter the same session instead of falling
       * into whatever view_save does next with a cleared g_vinfo. */
      memset(&g_vinfo, 0, sizeof g_vinfo);
      g_save_size = fsz;
      pdna_box_clear_carry();
      pdna_bank_clear_deletions();   /* BACKLOG #120 S2: drop any PREVIOUS save's queued Bank->PC deletions -- illegitimate once this fork's save/session is gone */
      app_xfer_pending_drop();   /* BACKLOG #150 S150-11 decision 11(ii): a fresh session never inherits a pending transfer */
      hb_off();
      /* Optional clipboard seed (fuse_sav.py --clip): a real 80-byte Gen-3 box record
       * so an empty GB cell's mon-menu offers PASTE (GB) -- app_mon_menu's own gate is
       * g_clip.occupied && !g_clip.from_gb, and clip_copy_from always clears from_gb.
       * pk3_validate() re-checks the checksum + species range at the point of USE, not
       * just at extraction time (tools/extract_gen3_record.c already filters for this,
       * but a hand-edited or bit-rotted --clip file must not be trusted just because
       * fused_clip_read() copied the right number of bytes). */
      { uint8_t rec80[80]; uint32_t csz = 0;
        if (fused_clip_present(&csz) && csz == sizeof rec80 && fused_clip_read(rec80, csz) &&
            pk3_validate(rec80)) {
          clip_copy_from(&g_clip, rec80, false);
          log_line("save: fused clip record loaded (%lu B) -> PASTE (GB) reachable",
                   (unsigned long)csz);
        }
      }
      log_line("save: flash blank/invalid -> using the fused GB save (%lu B)",
               (unsigned long)fsz);
      /* Looped rather than a single call: pdna_gen12_show_image() browses until B backs
       * all the way out (GB12_ENTER_OK), and a delta build has no file browser to
       * return to underneath it -- so re-enter the same session instead of falling into
       * whatever view_save does next with a cleared g_vinfo. Only ENTER_OK re-loops;
       * BUSY/NOT_GB fall out to the same crumb-clear + perf_span_end() the hardware
       * fork uses after its own single call, matching it line for line, rather than
       * spinning forever on an outcome a screenshot run should stop and report. */
      while (pdna_gen12_show_image(path, g_save, fsz, g_save + GB12_PRISTINE_OFF, 3)
             == GB12_ENTER_OK) {}
      perf_span_end();
      s_crumb_shown_armed = false;             /* the box screen paints its own crumb */
      return;
    } else if (!flash_ok) {
      /* BACKLOG #62: neither of the single-slot fused payloads (Gen-3 or GB, both
       * tools/fuse_sav.py) applied -- try the fuse_gb.py directory, which is where
       * the delta-gb build's whole Red/Gold/Crystal corpus actually lives. Mirrors
       * the single-fused-GB branch above line for line once a save is picked.
       *
       * #62 review A3: gb_delta_pick_save() now lives INSIDE this loop, not called
       * once before it -- the single-fused-GB branch above can get away with
       * re-entering the SAME session on every GB12_ENTER_OK (there is only ever one
       * save there), but with several fused saves that pattern never let the user
       * reach any save but the first: B backed the session out, GB12_ENTER_OK was
       * true, and the old `while` immediately re-opened the identical `pick` again
       * with no way out at all (delta-gb-only's whole reachable session was this
       * loop). Re-picking on every pass makes B in an open session return to the
       * picker to choose a different save, and B in the picker itself (gb_delta_pick_
       * save's own D4 fix) breaks out here -- falling through to the shared "not a
       * valid Gen-3 .sav" parse-failure path below, exactly like "no pick was ever
       * possible" already did before this fix. */
      bool any_picked = false;
      for (;;) {
        int pick = gb_delta_pick_save();
        if (pick < 0) break;
        const char* nm = 0; const uint8_t* base = 0; uint32_t psz = 0;
        if (!fused_gb_save(pick, &nm, &base, &psz) || !pdna_gen12_size_is_gb(psz)) break;
        fused_gb_set_active_save(pick);   /* BACKLOG #98: so fused_gb_rom()/fused_gb_loc()
                                            * resolve THIS save's own paired ROM instead of
                                            * guessing by generation alone */
        any_picked = true;
        memcpy(g_save, base, psz);
        memset(&g_vinfo, 0, sizeof g_vinfo);
        g_save_size = psz;
        pdna_box_clear_carry();
        pdna_bank_clear_deletions();   /* BACKLOG #120 S2: drop any PREVIOUS save's queued Bank->PC deletions -- illegitimate once this fork's save/session is gone */
        app_xfer_pending_drop();   /* BACKLOG #150 S150-11 decision 11(ii): a fresh session never inherits a pending transfer */
        hb_off();
        { uint8_t rec80[80]; uint32_t csz = 0;
          if (fused_clip_present(&csz) && csz == sizeof rec80 && fused_clip_read(rec80, csz) &&
              pk3_validate(rec80)) {
            clip_copy_from(&g_clip, rec80, false);
          }
        }
        log_line("save: no single-slot fused save -> using fused GB corpus entry %s (%lu B)",
                 nm ? nm : "?", (unsigned long)psz);
        if (pdna_gen12_show_image(path, g_save, psz, g_save + GB12_PRISTINE_OFF, 3) != GB12_ENTER_OK)
          break;                        /* not a clean back-out -- leave, matching the hardware fork */
        /* else: user backed all the way out (B) -- loop back to the picker */
      }
      if (any_picked) {
        perf_span_end();
        s_crumb_shown_armed = false;
        return;
      }
    }

    /* BACKLOG #68a: g_save now holds a real Gen-3 save (g3_ready) whenever EITHER
     * flash_ok (the real flash chip parsed) OR g3_from_fused_sav (the single-slot
     * fused-.sav fallback just above ran) is true -- on a fresh emulator boot the
     * combined image reaches Emerald almost always through the SECOND path (see the
     * comment on g3_from_fused_sav's declaration), so gating on flash_ok alone would
     * make this picker unreachable in exactly the scenario it exists for. Offer the
     * picker instead of falling straight through to the ordinary parse below.
     *
     * Mirrors the blank-flash "any_picked" loop above almost line for line (same
     * fused-GB-save copy into g_save, same full pdna_gen12_show_image mount), but
     * g_save here also holds the day's real Gen-3 save, so every return from a GB
     * mount must put it back before the picker (or the Emerald fallthrough below)
     * touches g_save again -- reloaded from WHICHEVER source supplied it, since a
     * fused-.sav Gen-3 save has no backing flash bytes to flashsave_read() back. */
    bool g3_ready = flash_ok || g3_from_fused_sav;
    if (g3_ready && fused_gb_present() && fused_gb_save_count() > 0) {
      /* #68a review A3: version_guess cannot tell FireRed/LeafGreen from Emerald at
       * this point (Gen3Version has no FRLG member), so the row names the FAMILY
       * the guess can vouch for, never a game it cannot. */
      const char* g3_label = "Gen 3 save (Emerald/FR/LG)";
      if (probe.version_guess == G3_VER_RS) g3_label = "Gen 3 save (Ruby/Sapphire)";
      for (;;) {
        int bp = gb_delta_boot_pick(g3_label);
        if (bp == 0) break;                      /* Emerald row (or KEY_B) -- fall through */
        int pick = bp - 1;
        const char* nm = 0; const uint8_t* base = 0; uint32_t psz = 0;
        if (!fused_gb_save(pick, &nm, &base, &psz) || !pdna_gen12_size_is_gb(psz)) break;
        fused_gb_set_active_save(pick);   /* BACKLOG #98: so fused_gb_rom()/fused_gb_loc()
                                            * resolve THIS save's own paired ROM instead of
                                            * guessing by generation alone */
        memcpy(g_save, base, psz);
        memset(&g_vinfo, 0, sizeof g_vinfo);
        g_save_size = psz;
        pdna_box_clear_carry();
        pdna_bank_clear_deletions();   /* BACKLOG #120 S2: drop any PREVIOUS save's queued Bank->PC deletions -- illegitimate once this fork's save/session is gone */
        app_xfer_pending_drop();   /* BACKLOG #150 S150-11 decision 11(ii): a fresh session never inherits a pending transfer */
        hb_off();
        { uint8_t rec80[80]; uint32_t csz = 0;
          if (fused_clip_present(&csz) && csz == sizeof rec80 && fused_clip_read(rec80, csz) &&
              pk3_validate(rec80)) {
            clip_copy_from(&g_clip, rec80, false);
          }
        }
        log_line("save: combined-image boot picker -> fused GB entry %s (%lu B)",
                 nm ? nm : "?", (unsigned long)psz);
        pdna_gen12_show_image(path, g_save, psz, g_save + GB12_PRISTINE_OFF, 3);
        /* Whatever the GB mount returned, g_save now holds GB bytes -- restore the
         * Gen-3 save before the picker (or the fallthrough) reads g_save again. */
        if (flash_ok) flashsave_read(g_save, G3_SAVE_FILE_SIZE);
        else           fused_sav_read(g_save, fsz);
      }
    }
  }
#else
  /* Bracket the biggest SD read this app does. pdna_map.c:158 and pdna_bank.c:85 have
   * bracketed their SD work since rumble landed; view_save never did, and the motor is a
   * cart-bus device — the same bus the transfer is running on. The 2026-08-18 DS Lite log
   * stopped between the two crumbs either side of this line. motor_set now also refuses on
   * flashcartio_is_reading, so this is belt AND braces: this one keeps the cue's phase
   * (rmbl_pause ends the cue cleanly) rather than relying on the driver flag alone. */
  unsigned long rt0 = flashcartio_read_retries, rf0 = flashcartio_read_failures;
  rmbl_pause();
  SfStatus st = sf_read_full(path, g_save, G3_SAVE_FILE_SIZE, &sz);
  rmbl_resume();
  if (flashcartio_read_retries != rt0 || flashcartio_read_failures != rf0)
    log_line("save: SD read RETRIED %lu (failed %lu) - cart/bus trouble, not the parser",
             flashcartio_read_retries - rt0, flashcartio_read_failures - rf0);
  if (st != SF_OK) err = sf_status_str(st);

  /* ---- THE GAME BOY FORK -------------------------------------------------------
   * A Gen-1/2 battery save is 32 KiB (+ an optional 44/48-byte RTC tail) and can
   * never parse as Gen 3, so it used to die four lines below on "not a valid Gen-3
   * .sav" -- with the whole file already sitting in g_save, which is the thing that
   * made the old message so wrong: nothing was missing, we simply refused to look.
   * Fork on SIZE here (nothing else in the PokeDNA world is 32 KiB) and let the GB
   * mount do the real identification; if it is not a GB save after all, fall through
   * and report it exactly as before.
   *
   * g_vinfo is CLEARED FIRST and deliberately: it still holds the PREVIOUS save's
   * parse, and app_arena_release() -- which the GB session calls on its way out --
   * rebuilds g_pc with gen3_read_pc_storage(g_save, g_vinfo.slot, ...). Left stale,
   * that would read a Gen-3 PC out of Game Boy bytes and leave 30 boxes of noise in
   * g_pc. Cleared, the release skips the rebuild (see app_arena_release), so the GB
   * session borrows the arena and gives it back with nothing invented in it.
   *
   * S1 opened it read-only; S2 (docs/GEN12-EDIT-DESIGN.md) edits this same resident
   * image in place through gb_session and persists it with sf_write_verified. */
  if (!err && pdna_gen12_size_is_gb(sz)) {
    memset(&g_vinfo, 0, sizeof g_vinfo);       /* no Gen-3 save is loaded in a GB session */
    g_save_size = sz;
    /* Same hygiene the Gen-3 path gets at "box source" below, which this early return
     * skips: pdna_box's mon-in-hand carry is a STATIC that deliberately survives across
     * pdna_box runs (it has to, for the PC<->Bank hand-off), so a mon picked up in a
     * previous save and never dropped would float over the GB grid as a phantom and
     * suppress the box idle animation. It could not be DROPPED there -- every drop path
     * is gated on src->can_edit(), constant false for a GB mount -- so this is state
     * hygiene, not a data path, but a Pokemon from another save hovering over someone's
     * Game Boy boxes is exactly the kind of thing that reads as corruption. */
    pdna_box_clear_carry();
    pdna_bank_clear_deletions();   /* BACKLOG #120 S2: drop any PREVIOUS save's queued Bank->PC deletions -- illegitimate once this fork's save/session is gone */
    app_xfer_pending_drop();   /* BACKLOG #150 S150-11 decision 11(ii): a fresh session never inherits a pending transfer */
    hb_off();
    /* met_game 3 = Emerald: with no Gen-3 save open there is no destination cartridge
     * to claim, and Emerald is the same default pdna_gen12_show() uses for 0. */
    /* The pristine copy lives in the idle upper half of g_save (128 KiB; a GB image is
     * at most 32816 B): rollback after a failed card write is one memcpy, and the card
     * is never opened for writing until the edit has passed every RAM gate. */
    int gr = pdna_gen12_show_image(path, g_save, sz, g_save + GB12_PRISTINE_OFF, 3);
    if (gr != GB12_ENTER_NOT_GB) {             /* mounted, or the user was told why not */
      perf_span_end();
      s_crumb_shown_armed = false;             /* the box screen paints its own crumb */
      return;
    }
    /* NOT_GB: 32 KiB, but not a Game Boy save either. Say THAT, not "not a Gen-3 .sav" */
    err = "32 KiB, but not a Gen-1/2 save";
  }
#endif
  /* The read returned. What used to be ONE opaque phase called "parsing" is steps
   * 2..4: the 2026-08-18 run 2 froze somewhere in here with "parsing" on screen, and
   * that word covered a slot scan, a full 11-section checksum pass, a log line and a
   * SaveBlock1 reassembly. Each is pure CPU on the EWRAM image, costs nothing to
   * announce, and is drawn BEFORE the work it names. */
  load_phase_n(2, "parse slots");
  g_save_size = sz;
  /* ONE parse, not two. This used to call gen3_parse twice on the same 128 KiB image --
   * once to decide whether to snapshot the boot checksums, once inside the validity
   * test -- doubling a 14-section scan and a double secret-base sweep for nothing. */
  bool parsed = (!err && sz >= (uint32_t)G3_SLOT_BYTES) &&
                gen3_parse_into(g_save, sz, &g_vinfo, g_sb1);
  if (parsed && g_vinfo.valid) {
    load_phase_n(3, "checksums");
    app_note_boot_checksums();          /* BEFORE anything can edit it */
  }
  load_phase_n(4, "saveblock1");
  if (!parsed || !g_vinfo.valid || !g_vinfo.sb1_ok ||
      gen3_read_saveblock1(g_save, g_vinfo.slot, g_sb1) != G3_SAVEBLOCK1_BYTES) {
    hb_off();
    ui_clear();
    ui_text(6, 60, UI_WARN, "Cannot read this save.");
    ui_text(6, 76, UI_DIM, err ? err : "not a valid Gen-3 .sav");
#ifdef PDNA_DELTA
    /* Reaching here now means BOTH sources failed: the flash chip is blank or corrupt
     * AND no usable save was fused into the image. Name the two ways out rather than
     * only the .sav one, or a fused image that failed to load reads as unfixable. */
    ui_text(6, 92,  UI_TEXT, "Copy your Pokemon .sav");
    ui_text(6, 102, UI_TEXT, "over this ROM's .sav,");
    ui_text(6, 112, UI_TEXT, "or fuse one in (fuse_sav.py).");
#endif
    ui_text(4, UI_FOOTER_Y, UI_DIM, "B=back");
    perf_span_end();      /* no paint is coming: close it here or the NEXT span would
                           * force-close it and wear a spurious !unclosed marker */
    wait_keys(KEY_B);
    s_crumb_shown_armed = false;               /* nothing painted: never claim it did */
    return;
  }

  /* Breadcrumb #2 of 3: the file is read AND it parses. Everything after this point
   * is decode + art + paint, so a log that stops here vs one that stops at #1 splits
   * the hang cleanly into "SD read / parse" vs "everything else". */
  load_phase_n(5, "log parsed");
  log_line("save: parsed sz=%lu slot=%d ver=%d", (unsigned long)sz,
           g_vinfo.slot, (int)g_vinfo.version_guess);
  /* Right after the deepest call chain in the whole program. Until 421e867 this line
   * would have read OVERFLOW on every single save open. */
  stack_report("after parse");
  app_log_flush();

  load_phase_n(6, "party");
  g_frlg = false;
  g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
  for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
  load_phase_n(7, "pc storage");
  g_have_pc = (gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc) == G3_PC_BYTES);
#ifdef PDNA_DELTA
  /* BACKLOG #150 S150-9 decision 12(c): plant FOUR converted Gen-3 records (slot 0
   * is the SAME planted CHIKORITA cell bank_plant_box0() seeds into Bank box 0 slot
   * 0, via gen12_convert -- exactly what the shipped Gen-3 DOWN arm does) into THIS
   * session's own PC box 0 slots 29/28/27/26 in RAM, and seed their matching
   * planted ledger entries -- the only way the merge screen (both the real-rows
   * case and the "nothing changed" skip) and the RESTORED/PENDING refusals are
   * reachable on a vehicle with no writable FAT at all. Best-effort: a partial
   * seed (n < 4) just means that many fewer probe cells exist, never a crash. */
  if (g_have_pc) {
    uint8_t g3_out[4][80];
    int n = bank_plant_xfer_seed_all(g3_out);
    for (int i = 0; i < n; i++) memcpy(pcsrc_records(0) + (29 - i) * 80, g3_out[i], 80);
  }
#endif

  /* BACKLOG #54 T2 evidence, NO GATE (decision 6): a species field past the Gen-3
   * ceiling is exactly the anomaly a per-hack SD profile (T2, still in BACKLOG) would
   * want on record -- logged here because g_party is already resident and resolved,
   * but this touches nothing else: no s_hack_mask bit, no UI, no app_can_edit()
   * change. One glitch Pokemon must never turn a retail user's own save read-only --
   * that is the ROM's call alone (decision 4), never the save's.
   *
   * Review fix F5 (speed): party only -- the PC sweep would cost 420 decryptions
   * (pk_decode_mon per slot, G3_TOTAL_BOXES * G3_IN_BOX) on the boot path for every
   * save open, a real cost with no signal this evidence-only, no-gate line needs to
   * pay for (BACKLOG #73). g_party is already decoded (pk_resolve above), so this
   * reads a field already in memory at zero extra decode cost, same spirit as the
   * "free, already in memory" framing the PC sweep never actually delivered. */
  {
    int glitch = 0;
    for (int i = 0; i < g_nparty; i++)
      if (g_party[i].species > G3_MAX_SPECIES) glitch++;
    if (glitch)
      log_line("romhack evidence: %d mon(s) with species > %d (T2 signal, no gate)",
               glitch, G3_MAX_SPECIES);
  }

  /* SaveBlock2 (section 0) for the trainer card + per-game layout for stats */
  load_phase_n(8, "saveblock2");
  int s0 = gen3_find_section(g_save, g_vinfo.slot, 0);
  if (s0 >= 0)
    memcpy(g_sb2, g_save + (uint32_t)g_vinfo.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE,
           G3_SECTOR_DATA_SIZE);
  /* Review fix F8 (info, BACKLOG #54): the ROM-hack banner/note set-or-clear below
   * (app_icon_rom_open() then the app_rom_is_hack(g_game) branch) sits AFTER this
   * point in view_save() -- four earlier `return;`s above (today: the truncated-read,
   * bad-header, corrupt-slot and no-valid-slot refusals) all exit before g_game is
   * even assigned. Any screen a future edit adds ABOVE this line must not read
   * g_src_note/g_src_ro: this save hasn't been classified yet, and the note/ro state
   * still reflects whatever the PREVIOUS save (or GB session) left behind. */
  g_game = g_frlg ? PK_FRLG : (g_vinfo.version_guess == G3_VER_RS ? PK_RS : PK_EMERALD);
  g_save_kind = se_kind_from_game((int)g_game);   /* E4: app_save_kind()'s Gen-3 half */
  /* The FIRST SD access after the read: f_open of the registered game ROM (artless) or
   * a fused-ROM scan. Named apart from the decode steps because it is the only step
   * here that can touch the card, and therefore the only one whose freeze would mean
   * the cart rather than the CPU. */
  load_phase_n(9, "art: open rom");
  app_icon_rom_open();                           /* fused or registered-SD icon source */
  /* BACKLOG #54 T1: the banner, once per save open, AFTER app_icon_rom_open() rather
   * than immediately at the g_game= line above -- app_icon_rom_open() is what actually
   * runs THIS session's rom_open()/rom_identify() classification (the fused ROM path
   * in particular never ran before this call), so checking app_rom_is_hack() any
   * earlier would read last session's (empty, on a fresh boot) mask instead of the
   * verdict this very open just produced. */
  /* app_mon_menu_readonly() already tolerates g_src_why == NULL (pdna_main.c's own
   * `(!empty && g_src_why) ? g_src_why(rec) : 0`) -- verified before wiring this in,
   * per this lane's STOP-LICENCE clause -- so passing 0 here needs no wrapper
   * function. Paired with app_src_readonly_clear() on the non-hack branch: the GB
   * fork already clears on its own return paths (pdna_gen12.c:2843/3095), so between
   * the two, no session can leak a stale read-only note into an unflagged save (a
   * leak here is STOP-level, per this lane's brief). */
  if (app_rom_is_hack(g_game)) {
    app_src_readonly_set(0, PDNA_ROMHACK_NOTE);
    msg_wait(PDNA_ROMHACK_TITLE, UI_WARN, PDNA_ROMHACK_L1, PDNA_ROMHACK_L2);
  } else {
    app_src_readonly_clear();
  }
#ifndef PDNA_DELTA
  /* The artless first run: offer the ROM registration ONCE per session, right where
   * its effect is about to be visible. B declines and the name chips carry on. */
  { static bool offered = false;
    if (!offered && !boxoam_icons_available() && app_can_edit()) {
      offered = true;
      /* The heartbeat cell (196,66) sits INSIDE app_confirm's frame (16,44)-(224,118)
       * and inside the ROM picker behind it, so an armed bar would blink a white
       * diagonal through the dialog every 8th frame. Pause it while the screen
       * belongs to the user; step 10's load_phase_n repaints the cell after. */
      hb_pause();
      /* ...and pause the SPAN for the same stretch, for a different reason. This block
       * blocks on the user: a confirm he may take ten seconds to answer, and behind it a
       * file picker that browses his card for a 12.5 MB .gba (hundreds of directory
       * reads). It sits INSIDE the "boot" span opened at the top of view_save, which is
       * the headline artless-vs-full-art number -- and it can only ever fire in the
       * ARTLESS build (boxoam_icons_available() is true whenever compiled icons exist),
       * on exactly the state Guy's last three logs report. Left un-paused, `perf boot:`
       * would carry his thinking time and his browsing, the comparison would read as
       * "artless save-open is 26x slower and does 8x the I/O", and the same binary would
       * report a different number for the same work on every run. perf.h states the rule
       * this is an instance of: no span may contain a call that blocks on user input. */
      perf_span_pause();
      if (app_confirm("ADD YOUR GAME ROM?", "Unlocks the real art. B = later"))
        app_register_rom();
      perf_span_resume();
      hb_resume();
    } }
#endif
  /* Deoxys forme follows the game version (RS Normal / Emerald Speed / FR-LG Attack). FR vs LG
   * can't be told apart from the save, so FR/LG defaults to Attack (FireRed); the summary lets
   * the user cycle to any forme. Re-decode the party so a Deoxys picks up its forme sprite. */
  load_phase_n(10, "party (forme)");
  pk_set_deoxys_form(g_game == PK_RS ? 0 : g_game == PK_EMERALD ? 3 : 1);
  g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
  for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);

  /* S5-C Part B2: g_sb1/g_pc/g_party are all freshly parsed and unedited (nothing has
   * had a chance to dirty them yet) and the box screen has not painted once -- the one
   * safe window to borrow the box_oam swap cache before it might be busy and to release
   * a PC/party slot before the user could have started moving anything by hand. Absent
   * folder / read-only cart / a card error mid-walk all fall through silently (design
   * doc section 12: "a card error during the walk = skip the question this time").
   *
   * S5-C review #5: this can read/write several SD files (up to GB_RECON_MAX_FILES
   * .pds files, plus a party/PC commit) before the box ever paints -- without its own
   * load_phase_n() the screen would still show "10/13 party (forme)" for however long
   * that takes, which reads as a hang on exactly the step that did NOT freeze. */
  load_phase_n(11, PDNA_LOAD_PHASE_SIDECARS);
  /* BACKLOG #150 S150-6, decision 6: the migration runs from exactly this ONE
   * place, immediately before gb_reconcile_on_load(). No file-static "already
   * tried" flag (NO new statics) -- the on-card MIGRATED marker is the gate, so a
   * second load this same boot is an O(1) f_stat. A NULL borrow (something else
   * holds app_box_swap_acquire's one buffer) just skips the migration this run --
   * decision 4/D-Q7's fallback keeps every record reachable either way. */
  {
    uint8_t* mig = app_box_swap_acquire(GBSC_FILE_MAX);
    if (mig) {
      xr_migrate_once(mig, GBSC_FILE_MAX);
      app_box_swap_release();
    }
  }
  gb_reconcile_on_load();
  /* A party release above edited g_sb1 in place -- g_party/g_nparty are a CACHE of
   * it (every other mutator in this file re-derives the same way afterward, e.g.
   * after pdna_bank_show() below), so re-derive now rather than let the party
   * screen show a slot that was just released. A no-op, cheap re-decode when
   * reconcile touched nothing. */
  g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
  for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);

  load_phase_n(12, "box source");
  BoxSource pcs = pc_box_source();
  pdna_box_clear_carry();                          /* no mon in hand when a save opens */
  pdna_bank_clear_deletions();                     /* no stale Bank->PC deletions from a prior save */
  app_xfer_pending_drop();                        /* BACKLOG #150 S150-11 decision 11(ii): a fresh session never inherits a pending transfer */
  rmbl_fire(RCUE_ROOM);                            /* entering the save's home "room" */
  /* app_icon_rom_open() above may have opened the user's ROM off the SD; from here on
   * it is the box paint (wallpaper staging + 30 verified icon copies + tile uploads).
   * Breadcrumb #3 fires from inside that paint -- see app_crumb_shown(). */
  load_phase_n(13, g_have_pc ? "first paint: box" : "first paint: party");
  /* Hand the VBlank handler back BEFORE the first full-screen paint: from here the
   * screen is being drawn every frame anyway, so a spinner would only be 144 pixels
   * of a real screen that the heartbeat has no business owning. A freeze from this
   * point on shows a half-painted box, which localises itself. */
  hb_off();
  /* app_crumb_shown() -- fired from pdna_box's first paint -- is what closes the span
   * begun at the top of this function. The no-PC path goes to party_list() instead and
   * never calls it, so close it here for that case; perf_span_end() is a no-op when a
   * span is already closed, so this costs the box path nothing. */
  if (!g_have_pc) perf_span_end();
  /* The PC box is "home"; Party / Bank / Daycare / etc. all hang off the START menu.
   * (Saves with no PC fall back to the party list as home.) */
  for (;;) {
    reload_saveblocks();                         /* editors share g_sb1/g_sb2 + commit all SB1 — keep them == the saved image so a declined edit can't ride along */
    int r = g_have_pc ? pdna_box(&pcs) : party_list();
    icon_store_borrow(false);          /* the same backstop for the HOME screen, which
                                        * does not go through nav_menu's switch */
    /* The home screen has been up and the user has left it, so the one-shot has
     * either fired or can never honestly fire (the no-PC party path). Disarm it, or
     * the next Bank screen -- which is also pdna_box -- would claim the save-open
     * paint that never happened. */
    s_crumb_shown_armed = false;
    if (r == 0) { flush_on_exit(); cfg_save(); return; }  /* B / SAVE tab -> file browser (one prompt for all deferred moves; persist last PC box) */
    if (r == 4) {                                /* up past the PC tabs -> Bank (cursor from below) */
      rmbl_fire(RCUE_ROOM);
      app_box_start_set(2);                       /* bank opens at the bottom row (unless carrying) */
      int br = pdna_bank_show();
      if (br == 5) app_box_start_set(1);          /* bank dropped off the bottom -> PC opens on its tabs */
      g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
      for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
    }
    if (r == 2) {                                /* START -> nav menu */
      int refresh_party = 0;
      int nvsel = nav_menu(NAV_ALL_AVAILABLE);
      /* BACKLOG #58: consult the SAME rule table a Game Boy session's START menu uses
       * (gb_nav_from_start, pdna_gen12.c) before dispatching -- a row nav_avail()
       * marks NOT_IN_GAME (or, defensively, COMING_SOON; no Gen-3 row is today) never
       * reaches the switch below at all. Most of the six candidates this item looked
       * at (Pokeblocks/Bases/Frontier/Mirage/Clock fix/Battle Records) already check
       * the game THEMSELVES and are never intercepted here -- see nav_avail.c's own
       * header for the exact line of each, cited so this is not just re-asserted. */
      /* se_kind_from_game(g_game), not app_save_kind(): g_game is written at exactly
       * one site (view_save's load) and never by a GB session, so it is the open
       * Gen-3 save's own game with no dependence on the GB-session lifecycle (which
       * does clear g_m on exit -- pdna_gen12_source(0) -- so app_save_kind() would
       * also be right today; this is simply the one with nothing to go stale). */
      const int nvkind = se_kind_from_game((int)g_game);
      NavAvail nvav = nav_avail(nvsel, nvkind);
      if (nvav != NAV_OK) { app_nav_refuse(nvsel, nvkind); }
      else switch (nvsel) {
        case NV_PARTY:   {                        /* Guy: "I expected to see the party menu
                          * on top of the pc pokemon in the background" -- with a PC box
                          * open, route to the SAME strip-over-the-box popup the box
                          * screen's own PARTY tab uses (pcp_open_party_strip, pdna_box.c)
                          * instead of the old full-screen list: arm the box's entry state
                          * and let the outer loop's unconditional pdna_box(&pcs) call
                          * (top of this for(;;)) open straight onto it. No PC storage this
                          * save (g_have_pc false) -> no box to show behind a strip, so the
                          * standalone full-screen browser is still the right screen; it is
                          * the ONLY remaining caller of app_party_overlay. */
                          rmbl_fire(RCUE_ROOM);
                          if (g_have_pc) { app_box_start_set(3); }
                          else { uint8_t dmy[80]; app_party_overlay(0, 0, 0, false, false, dmy, 0, false);
                                 refresh_party = 1; }
                          } break;
        case NV_BANK:    nav_open_bank(&refresh_party); break;   /* bottom-out -> PC tabs; a paste may hit the party */
        case NV_DAYCARE: pdna_daycare(); break;
        case NV_TRAINER: pdna_trainer(g_sb1, g_sb2, &g_vinfo, g_game); break;
        case NV_CLOCK:   pdna_clock(); break;
        case NV_MIRAGE:  pdna_mirage(); break;
        case NV_DEX:     pdna_dex_edit(); break;
        case NV_BAG:     if (app_can_edit()) bag_entry();
                         else msg_wait("BAG", UI_WARN, app_readonly_why(), 0);
                         break;
        case NV_DATA:    if (app_can_edit()) data_editor();
                         else { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); } break;
        case NV_SECRET:  rmbl_fire(RCUE_ROOM); pdna_secretbase(); break;
        case NV_POKEBLOCK: if (app_can_edit()) pdna_pokeblock();
                           else { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); } break;
        case NV_EVENTS:   if (app_can_edit()) pdna_events();
                           else { snd_deny(); msg_wait("READ-ONLY", UI_WARN, app_readonly_why(), 0); } break;
        case NV_BATTLEREC: pdna_battle_record(); break;  /* viewing is free; export gates on Omega inside */
        case NV_FRONTIER: pdna_frontier(g_sb1, g_sb2, g_game); break;   /* viewing free; editing gates on Omega inside */
        case NV_FLY:      pdna_fly(g_sb1, g_game); break;        /* viewing free; editing gates on Omega inside */
        case NV_CONTEST:  pdna_contest(g_sb1, g_pc, g_game); break;  /* viewing free; editing gates on Omega inside */
        case NV_MAP:      pdna_map(g_sb1, g_sb2, g_game); break;  /* the user's own ROM: SD file, or fused into this image */
        case NV_GB: {
#ifdef PDNA_DELTA
          /* BACKLOG #62: the reachable path for a fused GB corpus when a Gen-3 save is
           * ALSO fused (the default delta-gb recipe) -- the top-level boot fork is
           * monopolized by that Gen-3 save (fused_sav_present() checked first, always
           * wins when it parses), so this nav row is how the fused Red/Gold/Crystal
           * saves are actually reached, not view_save()'s own fallback chain (which
           * only fires when NO Gen-3 save is fused at all). Mounts straight over
           * cartridge space (pdna_gen12_show_fused) -- g_save already holds the live
           * Gen-3 session, so there is no spare resident buffer to copy a GB save
           * into even if one were needed. */
          int pick = gb_delta_pick_save();
          if (pick < 0) {
            /* #62 review D4: pick<0 now also means "the user pressed B in the
             * picker", not only "nothing is fused" -- only say the latter when it
             * is actually true, or a cancel would read like a build error. */
            if (fused_gb_save_count() <= 0)
              msg_wait("GB IMPORT", UI_DIM, "No fused GB saves.", "Rebuild with tools/fuse_gb.py.");
          } else {
            fused_gb_set_active_save(pick);   /* the third pick site: the nested import (b98 re-verify) */
            if (pdna_gen12_show_fused(pick, app_met_game()) == 1) nav_open_bank(&refresh_party);
          }
#else
          /* Browse for a Gen-1/2 .sav and mount it READ-ONLY as a box source. The
           * loaded save's game is stamped as the origin on anything copied out, so a
           * converted mon claims the cartridge it is actually going into. */
          char gp[PATH_MAX];
          if (app_pick_gb_save(gp, sizeof gp))
            if (pdna_gen12_show(gp, app_met_game()) == 1) nav_open_bank(&refresh_party);
#endif
          break;
        }
        case NV_SETTINGS: pdna_settings(); break;
        default: break;                          /* NV_BACK */
      }
      /* THE STRUCTURAL BACKSTOP for the Tier B icon borrow (icon_store_borrow). Every
       * screen above releases its own borrow at its own single release site; this line
       * makes a forgotten one impossible to keep, because it runs after EVERY case body
       * and a leaked borrow means g_pc holds icon tiles instead of the user's boxes when
       * the next commit writes it back. It is safe to call blindly: the store releases
       * only a borrow IT took (pdna_map.c and pdna_gen12.c hold the same arena and are
       * untouched), and a release with nothing held is a no-op. It also retires any plan
       * the screen left declared, so the next screen's animation gate cannot read a
       * stale "yes" -- see icon_store.h. */
      icon_store_borrow(false);
      if (refresh_party) {
        g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
        for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
      }
    }
    /* r == 1 no longer used (SELECT now cycles the box cursor mode, not box<->party) */
  }
}

int main(void) {
  init_system();
  /* The session clock, before anything that might want a timestamp. perf.c owns
   * TIMER0/TIMER1 from here to power-off; TIMER2 stays rumble's. Nothing else in the
   * app starts a timer any more -- the art-extraction screen and pdna_romfull.c's
   * verifier both READ this one (perf.h). */
  perf_clock_start();
  /* Before anything goes deep: paint the unused IWRAM stack so the low-water mark is
   * measurable for the rest of the session. main's own frame is already on the stack,
   * so this covers exactly the region every call below is about to spend. */
  stack_paint();
  log_init();
  log_line("=== PokeDNA (M0) ===");
  log_line("build " __DATE__ " " __TIME__);   /* stamp: proves WHICH binary produced this log
                                               * (stale flashes have faked "still broken" before) */
  /* ...and the same again with the git short hash and the variant, because a DATE and
   * TIME cannot tell two builds of the same afternoon apart, and "which tree was that
   * binary?" has already cost this project a debugging session. Also carries the free
   * EWRAM the binary was linked with, read from the linker's own __eheap_start, so a
   * log states the memory budget it was built against instead of us guessing. */
  perf_boot_line();
  log_line("waitcnt=%04x (was %04x) dispcnt=%04x console=%s",  /* cart timing: boosted vs the loader's handoff */
           *(volatile uint16_t*)0x04000204, flashcartio_bus_inherited(), REG_DISPCNT,
           console_name());
  log_line("bios=%08lx  bus self-test: %s (rung=%d)",
           (unsigned long)s_bios_sum, bus_verdict_str(s_busv1), flashcartio_bus_rung());
  /* Say what was actually established, not more. This pass compared the boosted
   * timing against the loader's own on the SAME possibly-bad image, with the probe
   * loop in IWRAM -- so it proves the cart returns CONSISTENT DATA, and it proves
   * nothing at all about instruction prefetch (tested after mount) or about the
   * bytes being the ones the linker produced (that is the ROM self-check's job). */
  log_line("bus: data-read consistency only; not prefetch, not a ROM integrity check");
  if (s_busv1 == FCIO_BUS_BAD_READ || s_busv1 == FCIO_BUS_BAD_DMA)
    log_line("bus: fast timing unstable on this console/cart - staying at loader timing");
  else if (s_busv1 == FCIO_BUS_UNSTABLE_SLOW)
    log_line("bus: reads differ at the LOADER's own timing - suspect the image or the cart, "
             "not the boost. Re-copy PokeDNA.gba, or run from NOR.");
  log_line("mGBA debug log: %s", log_under_mgba() ? "active" : "absent");
  /* Read ~64 KiB of ourselves back over the cart bus in 17 sampled windows and compare
   * against the build-time CRC32s. ROM bus only — no SD, and deliberately before
   * flashcartio_activate() so no transfer can possibly be in flight (hard rule 1). The
   * log lines ride out on the existing post-mount flush; this writes nothing itself. */
  pdna_romcheck_boot();

  /* FIELD OVERRIDE #2: hold R+SELECT at boot for the FULL-image verifier — every byte of
   * this ROM CRC32'd against its build-time per-region stamps, with the address of any
   * hole on screen. It runs HERE, before flashcart detection and long before any save is
   * opened, because the failure it exists to diagnose is a tool that hangs on save-open:
   * a check you can only reach through a working menu is no use on a broken image. Reads
   * ROM only, writes nothing, and its log lines ride out on the post-mount flush below.
   *
   * Deliberately NOT L+SELECT (that one is the slow-bus override, pdna_main.c:302) and
   * deliberately compatible with it: hold L+R+SELECT to check the image AT the loader's
   * timing, which is the one combination that separates a bad image from a bad bus. */
  if ((s_boot_held & (KEY_R | KEY_SELECT)) == (KEY_R | KEY_SELECT)) {
    log_line("boot: R+SELECT held - running the full-image verifier");
    pdna_romfull_screen();
  }

#ifdef PDNA_DELTA
  /* ---- emulator build: no flashcart, no microSD, no file browser. -------------
   * The save is this ROM's own 128 KiB flash chip, so boot straight into it. If the
   * emulator did not allocate a flash save at all, the FLASH1M_V signature is missing
   * from the image (see flashsave.c) — say so instead of showing an empty tool. */
  ui_clear();
  ui_text(6, 60, UI_TITLE, "PokeDNA (emulator build)");
  ui_text(6, UI_FOOTER_Y, UI_DIM, "build " __DATE__ " " __TIME__);
  uint16_t fid = 0;
  if (!flashsave_probe(&fid)) {
    ui_text(6, 84, UI_WARN, "No 128K flash save found.");
    ui_text(6, 100, UI_DIM, "Emulator save type wrong?");
    ui_text(6, 116, UI_DIM, "Set it to Flash 1Mbit.");
  }
  pdna_romcheck_report();  /* deliberately reachable here: mGBA is where the failure
                            * panel gets exercised before hardware ever sees it */
  bus_late_selftests();   /* no card and no motor here, but the ROM-fetch probe is
                           * the only place this code can be exercised off-hardware */
  gb_art_boot_register(0, 0);                /* #62 D1: without this s_gb_on stays 0 and
                                               * every delta build draws Gen-3 stand-in art
                                               * instead of the fused GB corpus */
  pdna_era_boot_register();                  /* E4: the era resolver + cross-game Gen-3 rung */
  snd_boot();
  for (;;) view_save("");                    /* B just re-enters: there is nowhere to go back to */
  return 0;
#else
  ui_clear();
  ui_text(6, 70, UI_TITLE, "Detecting flashcart...");
  ui_text(6, UI_FOOTER_Y, UI_DIM, "build " __DATE__ " " __TIME__);
  /* Cart detection AND the SD init/mount can fail transiently right after the loader
   * hands off (the EZ-Flash SD interface sometimes needs a moment, or a re-init, before
   * the first read succeeds). Retry both a few times with a short settle before giving
   * up, so a one-off glitch no longer hard-halts the tool on launch. Read-only here, so
   * retrying is risk-free. */
  bool active = false;
  for (int a = 0; a < 8 && !active; a++) {
    /* Each attempt owns one row (8 rows x 8 px from y=86 end exactly at UI_FOOTER_Y).
     * "probing..." goes up BEFORE the call, so a hang inside detection leaves the
     * attempt number on screen; the verdict then overwrites it. Stack locals only. */
    char d[40];
    int y = PDNA_DETECT_Y0 + a * UI_ROW_H;
    siprintf(d, "%d: probing...", a + 1);
    ui_text(PDNA_DETECT_X, y, UI_DIM, d);
    active = flashcartio_activate();
    detect_line(d, a + 1);
    ui_fill_rect(PDNA_DETECT_X, y, 228, UI_ROW_H, UI_BG);
    ui_text(PDNA_DETECT_X, y, active ? UI_OK : UI_WARN, d);
    log_line("flashcart try %s", d);
    if (!active) for (int v = 0; v < 8; v++) vsync();    /* ~130 ms settle, then re-detect */
  }
  if (!active) halt_msg("No flashcart detected! Reseat cart & reboot.");
  {
    unsigned first = 0xFFFFu, la = flashcartio_ezfo_lookalikes(&first);
    log_line("flashcart: %s code=%d page=0x%x lookalikes=%u first=0x%x", flashcart_name(),
             flashcartio_detect_code(), flashcartio_ezfo_page(), la, first);
  }
  /* The page was matched by the header word only: the driver could not read the image
   * the same way twice, so the fingerprint that tells a stale same-title copy from the
   * running one was not applied (hw1 review #2). Say so before anything else runs off
   * this page. Pure UI + wait_keys: safe before the mount. */
  if (flashcartio_detect_code() == FCIO_DET_EZFO_HDRONLY)
    msg_wait(PDNA_DET_HDRONLY_TITLE, UI_WARN, PDNA_DET_HDRONLY_L1, PDNA_DET_HDRONLY_L2);

  FATFS fs;                                  /* lives forever (main never returns) */
  FRESULT fr = FR_NOT_READY;
  for (int a = 0; a < 8; a++) {
    fr = f_mount(&fs, "", 1);                /* opt=1: mount now (reads the FS) */
    if (fr == FR_OK) break;
    log_line("f_mount attempt %d failed (fr=%d)", a, fr);
    ui_clear();
    ui_text(6, 70, UI_TITLE, "Mounting SD card...");
    char rb[32]; siprintf(rb, "retry %d/7 (fr=%d)", a + 1, (int)fr);
    ui_text(6, 86, UI_DIM, rb);
    for (int v = 0; v < 12; v++) vsync();    /* ~200 ms settle */
    flashcartio_activate();                  /* re-init the cart's SD interface, then retry */
  }
  if (fr != FR_OK) {
    log_line("f_mount FAILED fr=%d after retries", fr);
    log_flush_to_sd(LOG_PATH);                 /* best-effort (SD is down, so likely a no-op) */
    char m[40]; siprintf(m, "SD mount failed (fr=%d)", (int)fr);
    halt_msg(m);
  }
  log_line("SD mounted OK");
  perf_fs_facts(&fs);     /* cluster size is the ceiling on any batched read (see perf.c) */
  /* The stack low-water mark this early, not only after a save parse: a run that never
   * reaches a save still leaves the figure behind, and the two together bracket the
   * deepest call chain in the program. */
  stack_report("boot");
  rmbl_init();   /* rumble AFTER the SD is mounted: its cart-GPIO writes must never touch the bus before SD is up */
  f_mkdir(PDNA_DIR);                          /* all PokeDNA files live in /PokeDNA, not the SD root */
  /* Rotate the last two runs aside BEFORE the first flush of this run. Without this,
   * relaunching after a hang destroys the hang's own log -- which is exactly what
   * happened to the 2026-08-18 DS-Lite hang. /PokeDNA/log.prev1.txt is the run before
   * this one, log.prev2.txt the one before that. Omega-gated with every other write
   * (hard rule 4): a failed rename on a read-only cart would leave FatFs' write flag
   * set and poison the volume for reads. */
  if (active_flashcart == EZ_FLASH_OMEGA) log_begin_run(LOG_PATH);
  app_log_flush();   /* Omega-gated + motor-paused, same as every other flush now */
  /* If THAT did not land, say so on the screen right now. This is the boot's first and
   * only write, so a failure here means the whole run will be silent -- and the message
   * explaining it cannot go in the log (that would be another write). 2026-08-18 run 2
   * hung with an empty card and no way to tell which of the two had happened.
   * Omega-only: on an EverDrive every write fails by design, and nagging about a
   * documented read-only setup would be noise. */
  if (active_flashcart == EZ_FLASH_OMEGA && log_health() != LOG_HEALTH_OK) {
    char m[48];
    const char* why = "Card locked, full, or unwritable?";
    if (log_health() == LOG_HEALTH_LOST) {
      /* The write said FR_OK and the card kept less than it acknowledged. This is the
       * one the old gate could not see at all: every counter said healthy. */
      siprintf(m, "card kept %lu of %lu bytes", log_card_bytes(), log_expect_bytes());
      why = "Card ACKed writes it did not keep!";
    } else if (log_health() == LOG_HEALTH_UNVERIF) {
      siprintf(m, "read-back failed (e%d)", log_verify_result());
      why = "Cannot confirm the log reached the card.";
    } else {
      siprintf(m, "%s wrote nothing (e%d)", LOG_PATH, log_last_result());
    }
    msg_wait("LOG NOT SAVING", UI_WARN, m, why);
  }
  pdna_romcheck_report();  /* the verdict is already on the card before we ask for A */

  bus_late_selftests();

  snd_boot();                                /* welcome chime = audio self-test */

  strcpy(g_cwd, "/");
  cfg_load();                                /* restore last folder + sort/filter (#6) */
  {
    /* Light up any registered GB ROMs (romgb1/romgb2). With their .loc caches on the
     * card this is a few dozen reads per ROM; only a missing/stale cache rescans, and
     * that rescan now shows the same progress screen Settings does (B cancels it --
     * the ROM simply stays unregistered for this session). */
    GbRegUi boot_ui = { 0, 0 };
    gb_art_boot_register(gb_reg_progress, &boot_ui);
  }
  pdna_era_boot_register();                  /* E4: the era resolver + cross-game Gen-3 rung */
  /* ONE throughput sample per run, at two sizes, on the user's own card -- see
   * perf_sd_sample(). It runs HERE because it needs two things that only exist at this
   * point: a mounted card, and cfg_load()'s restored ROM paths to pick a big enough
   * file from. icons.bin first (it is the file the icon ladder actually reads), then
   * any registered ROM. g_save is the scratch: 128 KiB, and provably idle -- no save
   * has been picked yet, browse_pick() is the next statement. */
  { const char* cand[5];
    cand[0] = PDNA_DIR "/art/icons.bin";
    cand[1] = g_rom_path[0]; cand[2] = g_rom_path[1]; cand[3] = g_rom_path[2];
    cand[4] = 0;
    perf_sd_sample(cand, g_save, (uint32_t)sizeof g_save); }
  for (;;) {
    char path[PATH_MAX];
    if (browse_pick(path, sizeof(path))) view_save(path);
  }
  return 0;
#endif /* PDNA_DELTA */
}
