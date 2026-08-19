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
#include "pdna_summary.h"
#include "pdna_box.h"
#include "gen3_trainer.h"
#include "gen3_record.h"    /* Emerald Battle Record (save sector 31) info + export */
#include "pdna_frontier.h"  /* Battle Frontier win-streak viewer/editor (SaveBlock2) */
#include "pdna_fly.h"       /* Fly-destination (visited-town) flags (SaveBlock1)     */
#include "pdna_map.h"       /* overworld map, read from the user's own Pokemon ROM   */
#include "pdna_trainer.h"
#include "pdna_edit.h"
#include "gen3_edit.h"     /* EditMon, gen3_edit_load/commit, em_set_*, em_preview */
#include "gen3_clip.h"     /* ClipMon, slot ops (copy/paste/dup/release) */
#include "gen3_gen.h"      /* gen3_build_mon_spread — one seed, matched PID + IVs */
#include "pdna_legality.h" /* pdna_legality_show */
#include "pdna_gen12.h"    /* GB import: mount a Gen-1/2 save read-only */
#include "pdna_pk.h"     /* pdna_pk_export (.pk3) */
#include "pdna_bank.h"   /* pdna_bank_show (bank = parallel boxes) */
#include "gen3_flags.h"    /* event flags */
#include "gen3_dex.h"      /* Pokedex seen/owned flags */
#include "gen3_pokeblock.h" /* Pokeblock case (RS/Emerald) */
#include "gen3_daycare.h"  /* daycare breeding compatibility (the man's verdict) */
#include "gen3_items.h"    /* item bags */
#include "pdna_bag.h"      /* real Emerald bag screen (data-editor bag tab) */
#include "bag_bg.h"        /* bag_bg() availability gate (weak NULL when art-free) */
#include "pokeblock_bg.h" /* Pokeblock case chrome (weak NULL when art-free) */
#include "rom_mon.h"       /* phase-1 ROM-gated icons (fused ROM -> real box icons) */
#include "rom_text.h"      /* phase-2: descriptions out of the user's own ROM */
#include "box_oam.h"       /* boxoam_rom_icons registration */
#include "fused_rom.h"
#include "fused_sav.h"    /* a save fused into the image: the emulator build's fallback */
#include "gen3_secretbase.h" /* Secret Base records (RS/Emerald) */
#include "osk.h"           /* osk_search (numeric entry) */
#include "pdna_pick.h"   /* pick_item, pick_move (PC-menu quick editors) */
#include "snd.h"           /* UI sound effects */
#include "rmbl.h"          /* haptic rumble cues (per-cue toggles) */
#include "rumble.h"        /* rumble_io_suspend/resume: mute the motor while a blit reads ROM */
#include "gba_rtc.h"       /* live cartridge RTC reader (clock check & fix) */
#include "pdna_app.h"
#include "savefile.h"
#include "log.h"
#include "pdna_romcheck.h"  /* sampled high-ROM self-check: the incomplete-SD-load guard */
#include "pdna_romfull.h"   /* the FULL-image verifier screen (boot hold R+SELECT / FILE MENU) */
#include "ui.h"
#include "pdna_layout.h"   /* screen geometry + fixed strings, shared with the host text-fit test */

#define PDNA_DIR      "/PokeDNA"            /* all of PokeDNA's on-card files live here (not the SD root) */
#define LOG_PATH      "/PokeDNA/log.txt"
#define PATH_MAX      256
#define MAX_ENTRIES   256
#define NAME_MAX      64
#define LIST_COLS     28          /* display columns for a list row                 */
#define VIS_ROWS      12          /* visible rows in the framed browser panel        */

typedef struct {
  char     name[NAME_MAX];
  uint32_t size;                  /* file size in bytes (0 for folders)              */
  uint32_t dosdt;                 /* (fdate<<16)|ftime, for the date sort            */
  bool     is_dir;
} BrowseEntry;

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
/* Pokemon ROM path per game (PkGame index: RS / Emerald / FRLG). The map screen reads
 * map data out of the user's OWN ROM, and RS/E/FRLG map data differs, so each game
 * remembers its own file. Persisted by cfg_save. */
static char        EWRAM_BSS g_rom_path[3][PATH_MAX];
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

/* wait_keys, plus a 2-frame idle bob. `kind` is an ANIM_* place; while nothing is
 * pressed and that place's toggle is on, *frame flips every `period` vblanks and
 * `redraw` recomposes the screen's sprites at the new frame. The animation runs
 * ONLY on frames with no key pending, so a press or a key-repeat is never delayed
 * behind a multi-sprite repaint (the rule the Pokedex learned). Callers own *ctr
 * and *frame so the phase survives their redraw loop. kind < 0 or redraw == 0
 * makes this exactly the old wait_keys (period is unused in that case). */
static u16 wait_keys_bob_p(u16 mask, int kind, int* ctr, int* frame,
                           void (*redraw)(int), int period) {
  u16 hit, fresh;
  do {
    vsync();
    fresh = key_hit(mask);
    hit = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT));
    if (!hit && redraw && kind >= 0 && app_anim_enabled(kind) && ++*ctr >= period) {
      *ctr = 0; *frame ^= 1; redraw(*frame);
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

static void sort_entries(void) {                  /* stable insertion sort, never mid-transfer */
  for (int i = 1; i < g_count; i++) {
    BrowseEntry tmp = g_entries[i];
    int j = i - 1;
    while (j >= 0 && entry_cmp(&g_entries[j], &tmp) > 0) { g_entries[j + 1] = g_entries[j]; j--; }
    g_entries[j + 1] = tmp;
  }
}

/* Scan g_cwd into g_entries: subdirectories + (by default) *.sav files. The
 * filter (g_show_all / g_show_hidden) and the sort are sd-browser-style. */
static void scan_dir(void) {
  g_count = 0;
  DIR dir;
  FILINFO fno;
  if (f_opendir(&dir, g_cwd) != FR_OK) { log_line("opendir %s failed", g_cwd); return; }
  while (g_count < MAX_ENTRIES && f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    bool is_dir = (fno.fattrib & AM_DIR) != 0;
    if (!g_show_hidden && (fno.fattrib & (AM_HID | AM_SYS))) continue;
    if (!is_dir && !g_show_all && !has_sav_ext(fno.fname)) continue;  /* folders + .sav unless show-all */
    strncpy(g_entries[g_count].name, fno.fname, NAME_MAX - 1);
    g_entries[g_count].name[NAME_MAX - 1] = 0;
    g_entries[g_count].size = is_dir ? 0 : (uint32_t)fno.fsize;
    g_entries[g_count].dosdt = ((uint32_t)fno.fdate << 16) | (uint32_t)fno.ftime;
    g_entries[g_count].is_dir = is_dir;
    g_count++;
  }
  f_closedir(&dir);
  sort_entries();
  log_line("scan %s: %d entries", g_cwd, g_count);
}

/* ---- persistent browser prefs: last folder + sort/filter (#6) ------------ */
#define CFG_PATH PDNA_DIR "/config.cfg"

/* Persist the browser state so the next launch reopens the same folder with the
 * same sort/filter. Writes are EZ-Flash-Omega-only (EverDrive write isn't wired),
 * so this is a no-op on a read-only cart; best-effort, any failure is ignored. */
static void cfg_save(void) {
  if (!app_can_edit()) return;
  char buf[PATH_MAX * 4 + 128];
  int n = siprintf(buf, "dir=%s\nsort=%d\nrev=%d\nall=%d\nhidden=%d\nanim=%u\nrumble=%u\nrstr=%d\nrdur=%d\npcbox=%d\nyard=%d\nbak=%d\n",
                   g_cwd, (int)g_sort, g_sortrev ? 1 : 0, g_show_all ? 1 : 0, g_show_hidden ? 1 : 0,
                   g_anim_mask, rmbl_get_mask(), rmbl_get_strength(), rmbl_get_duration(), g_pc_last_box,
                   g_yard_visitors ? 1 : 0, g_backup_mode);
  /* One ROM path per game — RS/Emerald/FRLG map data differs, so each needs its own
   * ROM file (Guy's requirement). Only non-empty entries are written. */
  static const char* const k_romkey[3] = { "romrs", "romem", "romfr" };
  for (int i = 0; i < 3; i++)
    if (g_rom_path[i][0])
      n += siprintf(buf + n, "%s=%s\n", k_romkey[i], g_rom_path[i]);
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

/* Restore prefs saved by cfg_save (best-effort): a missing/unparsable file just
 * leaves the compiled defaults. The saved folder is adopted only if it still
 * exists, else we fall back to root — a moved SD card can't strand the browser. */
static void cfg_load(void) {
  FIL f;
  if (f_open(&f, CFG_PATH, FA_READ) != FR_OK) return;
  char buf[PATH_MAX * 4 + 128]; UINT br = 0;
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
      else if (!strcmp(k, "bak"))    { int m = v[0] - '0'; if (m >= 0 && m <= 2) g_backup_mode = m; }
      else if (!strcmp(k, "anim"))   { unsigned m = 0; for (const char* d = v; *d >= '0' && *d <= '9'; d++) m = m * 10 + (unsigned)(*d - '0'); g_anim_mask = m & ((1u << ANIM_COUNT) - 1u); }
      else if (!strcmp(k, "rumble")) { unsigned m = 0; for (const char* d = v; *d >= '0' && *d <= '9'; d++) m = m * 10 + (unsigned)(*d - '0'); rmbl_set_mask(m); }
      else if (!strcmp(k, "pcbox"))  { int m = 0; for (const char* d = v; *d >= '0' && *d <= '9'; d++) m = m * 10 + (*d - '0'); g_pc_last_box = m; }
      else if (!strcmp(k, "romrs") && v[0]) { strncpy(g_rom_path[PK_RS], v, PATH_MAX - 1);      g_rom_path[PK_RS][PATH_MAX - 1] = 0; }
      else if (!strcmp(k, "romem") && v[0]) { strncpy(g_rom_path[PK_EMERALD], v, PATH_MAX - 1); g_rom_path[PK_EMERALD][PATH_MAX - 1] = 0; }
      else if (!strcmp(k, "romfr") && v[0]) { strncpy(g_rom_path[PK_FRLG], v, PATH_MAX - 1);    g_rom_path[PK_FRLG][PATH_MAX - 1] = 0; }
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

/* sd-browser-style listing: cwd header, framed panel, (DIR) tags + right-aligned
 * size column, a per-selection detail block, and a green status line. */
static void render_browser(int sel, int top) {
  ui_clear();
  char title[80], cwdc[LIST_COLS * 4 + 1];
  siprintf(title, "Pick .sav: %s", g_cwd);      /* state the goal every frame */
  ui_truncate(cwdc, title, 29);
  ui_text(2, 2, UI_TITLE, cwdc);
  ui_panel(0, 11, UI_SCR_W, 104, UI_PANEL, UI_BORDER);

  if (g_count == 0) {
    ui_text(6, 40, UI_WARN, "(no folders or .sav files here)");
    ui_text(6, 52, UI_DIM,  at_root() ? "Open the folder with your saves."
                                      : "B = go up a folder.");
  } else {
    char row[LIST_COLS * 4 + 1], nm[NAME_MAX + 2], sz[12];
    for (int i = 0; i < VIS_ROWS && top + i < g_count; i++) {
      int idx = top + i;
      int y = 14 + i * UI_ROW_H;
      const BrowseEntry* e = &g_entries[idx];
      if (e->is_dir) {
        ui_truncate(nm, e->name, 21);
        siprintf(row, "%-21s (DIR)", nm);
        ui_text_sel(3, y, UI_SCR_W - 6, idx == sel, UI_DIRCLR, row);
      } else {
        ui_truncate(nm, e->name, 18);
        human_size(e->size, sz);
        siprintf(row, "%-18s %8s", nm, sz);
        ui_text_sel(3, y, UI_SCR_W - 6, idx == sel, UI_SAVECLR, row);
      }
    }
  }

  if (g_count > 0) {                          /* per-selection detail block */
    const BrowseEntry* e = &g_entries[sel];
    char dn[40]; ui_truncate(dn, e->name, 29);
    ui_text(2, 118, UI_SELTEXT, dn);
    char meta[40];
    if (e->is_dir) siprintf(meta, "folder");
    else { char sz[12]; human_size(e->size, sz); siprintf(meta, "save file   %s", sz); }
    ui_text(2, 128, UI_DIM, meta);
  }

  char status[64], stc[40];
  siprintf(status, "%d/%d  %s  %s", g_count ? sel + 1 : 0, g_count,
           sort_label(), g_show_all ? "all" : ".sav");
  ui_truncate(stc, status, 29);
  ui_text(2, 138, UI_OK, stc);

  /* UI_FOOTER_Y, not a hard 150: this row is what every popup is laid out to clear, and
   * a literal here would let the two drift apart (see source/ui_layout.h). */
  ui_hline(0, UI_FOOTER_RULE_Y, UI_SCR_W, UI_BORDER);
  ui_text(2, UI_FOOTER_Y, UI_DIM, "A pick  B up  SEL sort  ST menu");
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
static bool browse_menu(const BrowseEntry* fe) {
  int sel = 0;
  bool changed = false;
  bool can_fileops = (fe && !fe->is_dir);
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "FILE MENU");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char rows[8][40];
    int  act[8];
    int  n = 0;
    enum { A_FILEOPS, A_SORTKEY, A_ORDER, A_FILES, A_HIDDEN, A_VERIFY, A_REBOOT, A_CLOSE };
    if (can_fileops) {
      char nm[24]; ui_truncate(nm, fe->name, 16);
      siprintf(rows[n], "File: %s...", nm); act[n++] = A_FILEOPS;
    }
    siprintf(rows[n], "Sort key:  %s", g_sort == SORT_NAME ? "Name" : g_sort == SORT_SIZE ? "Size" : "Date"); act[n++] = A_SORTKEY;
    siprintf(rows[n], "Order:     %s", g_sortrev ? "descending" : "ascending"); act[n++] = A_ORDER;
    siprintf(rows[n], "Files:     %s", g_show_all ? "all files" : ".sav only"); act[n++] = A_FILES;
    siprintf(rows[n], "Hidden:    %s", g_show_hidden ? "shown" : "hidden"); act[n++] = A_HIDDEN;
    /* Reachable from the browser, i.e. WITHOUT opening a save — the boot hold (R+SELECT)
     * covers the case where even this menu cannot be reached. */
    strcpy(rows[n], "Verify ROM image..."); act[n++] = A_VERIFY;
    strcpy(rows[n], "Reboot to flashcart menu..."); act[n++] = A_REBOOT;
    strcpy(rows[n], "Close"); act[n++] = A_CLOSE;
    for (int i = 0; i < n; i++) {
      int y = 26 + i * 16; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    ui_text(4, 152, UI_DIM, "A change  U/D move  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return changed;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      switch (act[sel]) {
        case A_FILEOPS: if (file_actions(fe)) return true; break;   /* re-scan after a file op */
        case A_SORTKEY: g_sort = (BrSortKey)((g_sort + 1) % 3); changed = true; break;
        case A_ORDER:   g_sortrev = !g_sortrev; changed = true; break;
        case A_FILES:   g_show_all = !g_show_all; changed = true; break;
        case A_HIDDEN:  g_show_hidden = !g_show_hidden; changed = true; break;
        case A_VERIFY:  pdna_romfull_screen(); app_log_flush(); break;  /* its verdict on the card too */
        case A_REBOOT:  do_reboot(); break;            /* returns only if cancelled */
        case A_CLOSE:   return changed;
      }
    }
  }
}

/* Browse the SD for a .sav. Writes the chosen full path to out and returns true;
 * navigation never leaves the browser (A enters a folder, B goes up). */
static bool browse_pick(char* out, int cap) {
  scan_dir();
  int sel = 0, top = 0;
  for (;;) {
    if (sel >= g_count) sel = g_count > 0 ? g_count - 1 : 0;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + VIS_ROWS) top = sel - VIS_ROWS + 1;

    render_browser(sel, top);

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
      sort_entries(); sel = 0; top = 0; cfg_save();           /* remember the sort */
    }
    else if (k & KEY_START) { if (browse_menu(g_count ? &g_entries[sel] : 0)) { scan_dir(); sel = 0; top = 0; cfg_save(); } }
    else if (k & KEY_B)    { if (!at_root()) { path_up(); scan_dir(); sel = 0; top = 0; cfg_save(); } }   /* remember the folder */
    else if (k & KEY_A) {
      if (g_count == 0) continue;
      const BrowseEntry* e = &g_entries[sel];
      char np[PATH_MAX];
      if (!path_join(g_cwd, e->name, np)) continue;
      if (e->is_dir) {
        strcpy(g_cwd, np);
        scan_dir();
        sel = 0; top = 0; cfg_save();                         /* remember the folder */
      } else if ((int)strlen(np) < cap) {
        cfg_save();                                           /* remember where this save was picked from */
        strcpy(out, np);
        return true;
      }
    }
  }
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
static char   g_path[PATH_MAX];                           /* path of the open save (for commit) */
static uint16_t g_item_clip = 0;                          /* held-item move clipboard            */
static bool     g_item_held = false;

/* ===================== edit / commit (V4) =============================== */

#ifdef PDNA_DELTA
/* Emulator build: there is no flashcart to gate on — the save is our own flash chip,
 * which is always writable. */
bool app_can_edit(void) { return !pdna_romcheck_bad(); }
#else
bool app_can_edit(void) { return active_flashcart == EZ_FLASH_OMEGA && !pdna_romcheck_bad(); }
#endif

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
#define PDNA_LOAD_STEPS 12

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
  app_log_flush();
}

static void msg_wait(const char* title, u16 col, const char* l1, const char* l2) {
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
   * the arena never touches) is still authoritative. */
  gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc);
}

/* ---- per-game ROM path (the map screen reads map data from the user's own ROM) */
const char* app_rom_path(PkGame game) {
  int i = (int)game;
  return (i >= 0 && i < 3) ? g_rom_path[i] : "";
}
void app_rom_path_set(PkGame game, const char* path) {
  int i = (int)game;
  if (i < 0 || i >= 3 || !path) return;
  strncpy(g_rom_path[i], path, PATH_MAX - 1);
  g_rom_path[i][PATH_MAX - 1] = 0;
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

/* Draw the invented yard visitors? Only when the user asked for them AND owns a ROM.
 * Both halves matter: the setting is the user's choice, the ROM is what makes the
 * choice meaningful. See g_yard_visitors for why they are no longer on by default. */
static bool app_yard_visitors_ok(void) {
  return g_yard_visitors && app_any_rom_registered();
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
void app_bank_flush_deletions(void) { pdna_bank_flush_deletions(); }   /* delete queued Bank sources NOW (after the PC dest is committed) */
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


/* Box summary BROWSER: VIEW/EDIT a box slot, then U/D scroll to the prev/next
 * occupied slot (real-PC style). Edits are saved per-mon (prompted on leave/change)
 * via the owning block's commit. `block` is the pc-layout buffer (box `box`'s 30
 * records); for the bank box buffer pass box = 0. Returns true if any write. */
static bool app_box_browse(uint8_t* block, int box, int start, AppCommitFn commit) {
  int idx = start; bool any = false;
  int card = 0;                                            /* sticky across mon-scroll */
  for (;;) {
    uint8_t* rec = block + 0x0004 + ((uint32_t)box * 30 + idx) * 80;
    uint8_t out[100]; bool saved = false;
    int nav = pdna_inspect(rec, false, app_can_edit(), out, &saved, &card);
    if (saved) { memcpy(rec, out, 80); if (app_commit_with_dex(rec, false, commit, block)) any = true; }
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
    if (saved) { memcpy(rec, out, 100); if (app_commit_with_dex(rec, true, commit, g_sb1)) any = true; }
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

#ifndef PDNA_DELTA
/* The SD-file icon source: the user's registered .gba, held open read-only for the
 * whole session (FF_FS_LOCK is 0, so the map opening the same file is fine). The
 * FIL's ~600 B sector buffer lives in EWRAM like the map's. Every read brackets
 * rmbl_pause per the SD-transfer convention. */
static FIL EWRAM_BSS s_iconrom_fil;
static bool s_iconrom_fil_open = false;

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
#endif

/* Open the best available icon source and register it with the box:
 * 1) a ROM fused into this image (emulator or a fused NOR build);
 * 2) the registered SD .gba for the loaded save's game, then any other game's —
 *    icons are per-species art, so any Pokemon ROM serves them (Deoxys' forme is
 *    the only per-game difference). Ruby/Sapphire have no GF header yet and fail
 *    closed inside rom_mon_open. Call whenever the registration may have changed. */
static void app_icon_rom_open(void) {
  boxoam_rom_icons(0);
  memset(&s_romtext, 0, sizeof s_romtext);
  uint32_t fsz = 0;
  if (fused_rom_present(&fsz) && rom_open(&s_iconrom_ctx, fused_rom_read, 0, fsz)) {
    if (rom_mon_open(&s_iconrom, &s_iconrom_ctx)) {
      boxoam_rom_icons(&s_iconrom);
      rom_text_open(&s_romtext, &s_iconrom_ctx);
      log_line("icons: streaming from fused %s rev%u", rom_kind_name(s_iconrom_ctx.kind),
               s_iconrom_ctx.version);
      return;
    }
    log_line("icons: fused %s has no GF header (R/S) - trying SD", rom_kind_name(s_iconrom_ctx.kind));
  }
#ifndef PDNA_DELTA
  if (s_iconrom_fil_open) { f_close(&s_iconrom_fil); s_iconrom_fil_open = false; }
  static const PkGame k_try[3] = { PK_EMERALD, PK_FRLG, PK_RS };
  PkGame order[3] = { g_game, PK_EMERALD, PK_FRLG };      /* save's game first, then the rest */
  int no = 1;
  for (int i = 0; i < 3; i++) { PkGame c = k_try[i]; if (c != g_game && no < 3) order[no++] = c; }
  for (int i = 0; i < 3; i++) {
    const char* path = app_rom_path(order[i]);
    if (!path || !path[0]) continue;
    if (f_open(&s_iconrom_fil, path, FA_READ) != FR_OK) continue;
    s_iconrom_fil_open = true;
    uint32_t sz = (uint32_t)f_size(&s_iconrom_fil);
    if (rom_open(&s_iconrom_ctx, iconrom_fatfs_read, 0, sz) &&
        rom_mon_open(&s_iconrom, &s_iconrom_ctx)) {
      boxoam_rom_icons(&s_iconrom);
      rom_text_open(&s_romtext, &s_iconrom_ctx);
      log_line("icons: streaming from SD %s (%s)", path, rom_kind_name(s_iconrom_ctx.kind));
      return;
    }
    f_close(&s_iconrom_fil); s_iconrom_fil_open = false;
  }
#endif
}

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
    if (g_pc_dirty) msg_wait("SAVE FIRST", UI_WARN, "Unsaved box moves pending.", "Commit, then retry.");
    return;
  }
  FIL f;
  if (f_open(&f, path, FA_READ) != FR_OK) { msg_wait("CAN'T OPEN", UI_WARN, "File unreadable.", 0); return; }
  /* identify via a throwaway ctx on a temporary reader-less path: reuse the icon FIL */
  if (s_iconrom_fil_open) { f_close(&s_iconrom_fil); s_iconrom_fil_open = false; }
  f_close(&f);
  if (f_open(&s_iconrom_fil, path, FA_READ) != FR_OK) { msg_wait("CAN'T OPEN", UI_WARN, "File unreadable.", 0); return; }
  s_iconrom_fil_open = true;
  uint32_t sz = (uint32_t)f_size(&s_iconrom_fil);
  RomCtx rc;
  if (!rom_open(&rc, iconrom_fatfs_read, 0, sz)) {
    f_close(&s_iconrom_fil); s_iconrom_fil_open = false;
    msg_wait("NOT A POKEMON ROM", UI_WARN, "Retail R/S/E/FR/LG only.", 0);
    return;
  }
  PkGame rg = (rc.kind == ROM_EMERALD) ? PK_EMERALD
            : (rc.kind == ROM_RUBY || rc.kind == ROM_SAPPHIRE) ? PK_RS : PK_FRLG;
  app_rom_path_set(rg, path);
  cfg_save();
  app_icon_rom_open();                           /* light it up now */
  char l1[40]; siprintf(l1, "%s registered.", rom_kind_name(rc.kind));
  msg_wait("GAME ROM", UI_OK, l1,
           boxoam_icons_available() ? "Real art is ON." : "R/S icons come later; map works.");
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
  for (;;) {
    ui_clear();
    char hdr[40]; ui_truncate(hdr, e->name, 29);
    ui_text(4, 4, UI_TITLE, "FILE");
    ui_text(4, 16, UI_SELTEXT, hdr);
    ui_hline(0, 28, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 4; i++) {
      int y = 40 + i * 16; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, L[i]);
    }
    if (!cart_writable()) ui_text(8, 122, UI_DIM, "Read-only: writes need Omega.");
    ui_text(4, 152, UI_DIM, "A do  U/D move  B back");
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

static bool app_copy(uint8_t* rec, bool is_party) {
  clip_copy_from(&g_clip, rec, is_party);
  msg_wait("COPIED", UI_OK, "PASTE places it in a slot.", "(kept until overwritten)");
  return false;                                          /* no save change */
}

static bool app_paste(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block, bool occupied) {
  if (!g_clip.occupied) return false;
  if (occupied && !app_confirm("Overwrite this Pokemon?", "Paste the copied mon here?")) return false;
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

static uint32_t dc_seed(void);   /* PID entropy (RTC + counter); defined below */

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
  ui_ptext_fit_shadow(x + ndx, y + ndy, nw, UI_TEXT, UI_PTY_TEXT_SHADOW,
                      p->nickname[0] ? p->nickname : pk_species_name(p->species));
  char lvl[8]; siprintf(lvl, PDNA_PTY_LVL_FMT, (unsigned)p->level);
  int lvdy = isbox ? PDNA_PTY_BOX_LVL_DY : PDNA_PTY_LVL_DY;
  ui_ptext_shadow(x + ndx, y + lvdy, UI_TEXT, UI_PTY_TEXT_SHADOW, lvl);
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

  /* pk_decode_mon only carries the COMPUTED max stat (stats[PK_HP]) into PkMon;
   * current HP lives at record offset 0x56 (party-only field — gen3_edit.c:105
   * writes the same offset on heal/create) and is read straight off p->raw here. */
  uint16_t maxhp = p->stats[PK_HP];
  uint16_t curhp = p->raw ? (uint16_t)(p->raw[0x56] | ((uint16_t)p->raw[0x57] << 8)) : maxhp;
  if (curhp > maxhp) curhp = maxhp;     /* a torn/edited record must never over-fill the bar */

  int lbldx = isbox ? PDNA_PTY_BOX_HP_LBL_DX : PDNA_PTY_HP_LBL_DX;
  int lbldy = isbox ? PDNA_PTY_BOX_HP_LBL_DY : PDNA_PTY_HP_LBL_DY;
  ui_ptext_shadow(x + lbldx, y + lbldy, UI_PTY_HP_LABEL, UI_PTY_HP_OUTLINE, "HP");

  int bdx = isbox ? PDNA_PTY_BOX_HP_BAR_DX : PDNA_PTY_HP_BAR_DX;
  int bdy = isbox ? PDNA_PTY_BOX_HP_BAR_DY : PDNA_PTY_HP_BAR_DY;
  int bw  = isbox ? PDNA_PTY_BOX_HP_BAR_W  : PDNA_PTY_HP_BAR_W;
  int filled = maxhp ? (int)((uint32_t)curhp * (uint32_t)bw / maxhp) : 0;
  ui_progress(x + bdx, y + bdy, bw, PDNA_PTY_HP_BAR_H, filled,
             UI_PTY_HP_FILL, UI_PTY_HP_TRACK, UI_PTY_HP_OUTLINE);
  if (filled > 2) ui_hline(x + bdx + 1, y + bdy + 1, filled - 2, UI_PTY_HP_SHADE);  /* top-row shading */

  char hpn[16]; siprintf(hpn, PDNA_PTY_HP_NUM_FMT, (unsigned)curhp, (unsigned)maxhp);
  int hndx = isbox ? PDNA_PTY_BOX_HP_NUM_DX : PDNA_PTY_HP_NUM_DX;
  int hndy = isbox ? PDNA_PTY_BOX_HP_NUM_DY : PDNA_PTY_HP_NUM_DY;
  ui_ptext_shadow(x + hndx, y + hndy, UI_TEXT, UI_PTY_TEXT_SHADOW, hpn);
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
    /* The icon repaint just overwrote its own full bounding box, name/level column
     * included — put the text back on top (see party_draw_name_level's comment). */
    party_draw_name_level(i, p);
  }
  rumble_io_resume();
}

int app_party_overlay(const uint8_t* held, int orig_box, int orig_slot, bool orig_bank,
                      bool can_swap, uint8_t grab80[80], int* grab_slot, bool allow_move_to_box) {
  if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); return 0; }
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
  for (;;) {
    int n = party_count(g_sb1, g_frlg);
    if (n < 1) return 0;                              /* shouldn't happen (party never empties) */
    int addslot = (held && n < 6) ? n : -1;          /* PLACE: cell n is the "+ add" target */
    int lastSel = (addslot >= 0) ? addslot : (n - 1); /* last slot before BACK, in UP/DOWN order */
    if (sel != BACK && sel >= n && sel != addslot) sel = n - 1;   /* clamp after a release */
    PkMon pm[6]; pk_read_party_auto(g_sb1, pm, &g_frlg);
    for (int i = 0; i < n; i++) pk_resolve(&pm[i]);

    ui_clear();
    ui_stripe_bg(20, UI_PTY_BG_MARGIN, UI_PTY_BG_A, UI_PTY_BG_B);

    /* Publish what party_draw_all (full redraw AND the bob-tick redraw both call it)
     * needs. `pm` is this iteration's own stack array — see the comment above
     * party_draw_slot_fg for why pointing at it, rather than copying out of it, is
     * safe for the lifetime a redraw callback actually needs. */
    s_pov_pm = pm; s_pov_n = n < 6 ? n : 6;
    s_pov_sel = (sel == BACK) ? -1 : sel;
    s_pov_addslot = addslot;
    party_draw_all((uint8_t)bob);

    /* Bottom message box + CANCEL button, retail's own layout for this band
     * (docs/analysis-2026-08-19-party/MEASUREMENTS.md does not itemise these two —
     * they were measured separately off the same capture for this pass). */
    ui_panel(PDNA_PTY_MSG_X, PDNA_PTY_MSG_Y, PDNA_PTY_MSG_W, PDNA_PTY_MSG_H, UI_TEXT, UI_PTY_BORDER);
    ui_ptext_fit(PDNA_PTY_MSG_X + PDNA_PTY_MSG_PAD, PDNA_PTY_MSG_Y + PDNA_PTY_MSG_PAD,
                PDNA_PTY_MSG_W_BUDGET, UI_PTY_MSG_TEXT,
                held ? PDNA_PTY_MSG_PLACE : PDNA_PTY_MSG_CHOOSE);
    {
      bool bsel = (sel == BACK);
      ui_panel(PDNA_PTY_CANCEL_X, PDNA_PTY_CANCEL_Y, PDNA_PTY_CANCEL_W, PDNA_PTY_CANCEL_H,
              UI_PTY_CANCEL_FILL, bsel ? UI_PTY_CURSOR : UI_PTY_BORDER);
      ui_ptext_fit_shadow(PDNA_PTY_CANCEL_X + PDNA_PTY_MSG_PAD, PDNA_PTY_CANCEL_Y + PDNA_PTY_MSG_PAD,
                         PDNA_PTY_CANCEL_W_BUDGET, UI_TEXT, UI_PTY_TEXT_SHADOW, PDNA_LBL_CANCEL);
    }

    /* No LEFT/RIGHT: retail's own party screen has no horizontal axis (a fixed
     * top-left box plus a single column of rows) — UP/DOWN walks slot 0..lastSel,
     * then BACK; see DIFFERENCES #2/#3 in MEASUREMENTS.md for why the old 3-column
     * grid's L/R paging does not carry over. */
    u16 k = wait_keys_bob_p(KEY_UP | KEY_DOWN | KEY_A | KEY_B,
                            ANIM_PARTY, &bob_ctr, &bob, party_overlay_bob, PARTY_BOB_PERIOD);
    if      (k & KEY_B)     { snd_back(); return 0; }
    else if (k & KEY_UP)    { if (sel == BACK) sel = lastSel; else if (sel > 0) sel--; }
    else if (k & KEY_DOWN)  { if (sel != BACK) sel = (sel < lastSel) ? sel + 1 : BACK; }
    else if (k & KEY_A) {
      if (sel == BACK) { snd_back(); return 0; }
      if (held) {                                    /* PLACE: drop/swap into the party */
        if (party_place_held(held, sel, orig_box, orig_slot, orig_bank, can_swap)) return 1;
      } else if (sel < n) {                          /* BROWSE: the full action menu on this mon */
        uint8_t* rec = g_sb1 + (g_frlg ? 0x038 : 0x238) + (uint32_t)sel * 100;
        g_party_tobox_allowed = allow_move_to_box;
        app_mon_menu(rec, true, false, app_commit_sb1, g_sb1, -1, sel);
        g_party_tobox_allowed = false;
        if (g_party_tobox_req) {                      /* user chose "MOVE TO BOX" */
          g_party_tobox_req = false;
          if (party_count(g_sb1, g_frlg) <= 1) { snd_deny(); msg_wait("CAN'T", UI_WARN, "The party can't be empty.", "Move another mon in first."); }
          else { party_to_box(rec, grab80); if (grab_slot) *grab_slot = sel; return 2; }
        }
        /* else: edit/release/etc. ran -> loop re-reads the party + clamps sel, then redraws */
      } else snd_deny();                             /* empty cell */
    }
  }
}

/* CREATE a Pokémon from nothing into the empty slot `rec`: pick a species, build a
 * default valid record (Lv5, the save's OT/TID, Poké Ball), open the SIX-CARD SUMMARY
 * to customise, then write + commit. Returns true if the user kept it. Omega-only.
 * The summary — not the flat field list — is deliberate: making a Pokémon should look
 * like inspecting one, and the summary reaches all 40 editable fields anyway. */
static bool app_create_mon(uint8_t* rec, AppCommitFn commit, uint8_t* block) {
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
   * the slot is 50/50 — an improvement, but a change to created-mon output. */
  gen3_build_mon_spread(sp, lvl, dc_seed(), otId, g_vinfo.trainer_name, mg, &want, tmp, 0);

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

void app_src_readonly_set(const char* (*why_locked)(const uint8_t* rec80), const char* note) {
  g_src_why = why_locked; g_src_note = note; g_src_ro = true;
}
void app_src_readonly_clear(void) { g_src_why = 0; g_src_note = 0; g_src_ro = false; }
bool app_src_readonly(void) { return g_src_ro; }

/* The action menu for a source that is read-only in ITSELF: only actions that cannot
 * touch it. VIEW opens the summary with editing off; COPY just fills the 80-byte
 * clipboard, so the mon leaves through the existing clipboard -> PC/Bank path and
 * nothing new writes anything. Deliberately absent: PASTE / RELEASE / DUPLICATE /
 * CREATE / MOVE (they mutate a buffer whose commit() cannot persist it) and TO GAME
 * (it writes g_pc, which a mounted foreign source may currently be borrowing as its
 * EWRAM arena). `why_locked` can additionally veto COPY for one record and say why —
 * a Pokemon that is visible but cannot travel is far better than one that vanishes. */
static bool app_mon_menu_readonly(uint8_t* rec, bool is_party, const PkMon* m0) {
  const char* locked = g_src_why ? g_src_why(rec) : 0;
  enum { RO_VIEW, RO_LEGAL, RO_COPY, RO_CANCEL };
  int act[PDNA_ROMENU_MAX]; const char* lab[PDNA_ROMENU_MAX]; int n = 0;
  lab[n] = PDNA_LBL_VIEW;     act[n++] = RO_VIEW;
  lab[n] = PDNA_LBL_LEGALITY; act[n++] = RO_LEGAL;
  if (!locked) { lab[n] = PDNA_LBL_COPY; act[n++] = RO_COPY; }
  lab[n] = PDNA_LBL_CANCEL;   act[n++] = RO_CANCEL;

  /* 48, not the 16 the menu below uses: ui_truncate documents max_cols*4+1, and a
   * 10-glyph nickname of gender signs really is 30 UTF-8 bytes. GB nicknames hit this
   * (NIDORAN-male is in the test corpus), so this buffer is sized for it. */
  char title[48];
  ui_truncate(title, m0->nickname[0] ? m0->nickname : pk_species_name(m0->species), 11);
  const int hdr = PDNA_ROMENU_HDR + (g_src_note ? PDNA_ROMENU_LINE : 0)
                                  + (locked     ? PDNA_ROMENU_LINE : 0);
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
    if (g_src_note) { ui_ptext_fit(mx + PDNA_MONMENU_PAD, y, PDNA_MONMENU_PROSE_W, UI_WARN, g_src_note); y += PDNA_ROMENU_LINE; }
    if (locked)     { ui_ptext_fit(mx + PDNA_MONMENU_PAD, y, PDNA_MONMENU_PROSE_W, UI_WARN, locked);     y += PDNA_ROMENU_LINE; }
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
      switch (act[sel]) {
        case RO_VIEW:  { uint8_t d[100]; int card = 0;
                         pdna_inspect(rec, is_party, false, d, 0, &card); return false; }
        case RO_LEGAL: pdna_legality_show(m0); return false;
        case RO_COPY:  return app_copy(rec, is_party);
        default:       return false;
      }
    }
  }
}

bool app_mon_menu(uint8_t* rec, bool is_party, bool is_bank, AppCommitFn commit, uint8_t* block, int box, int slot) {
  PkMon m0;
  bool occupied = pk_decode_mon(rec, is_party, &m0);
  if (occupied) pk_resolve(&m0);

  if (!app_can_edit()) {                                 /* read-only carts: view only */
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
    if (!occupied) return false;                         /* nothing to create or paste into */
    return app_mon_menu_readonly(rec, is_party, &m0);
  }

  enum { A_SUMMARY, A_ITEM, A_MOVES, A_LEGAL, A_MOVE, A_TOBOX, A_COPY, A_PASTE, A_DUP, A_EXPORT, A_TOGAME, A_DAYCARE, A_RELEASE, A_TAKEITEM, A_GIVEITEM, A_CREATE, A_HATCH, A_CANCEL };
  /* Labels come from pdna_layout.h: they are 8 px/glyph inside a 100 px panel, so their
   * WIDTH is a real constraint (host_textfit_test.c measures every one of them). */
  int act[PDNA_MONMENU_MAX]; const char* lab[PDNA_MONMENU_MAX]; int n = 0;
  if (occupied) {
    lab[n]=PDNA_LBL_VIEW_EDIT; act[n++]=A_SUMMARY;     /* opens the editable summary (moves edited there) */
    lab[n]=PDNA_LBL_ITEM;    act[n++]=A_ITEM;
    lab[n]=PDNA_LBL_LEGALITY; act[n++]=A_LEGAL;
    if (m0.isEgg && !m0.isBadEgg) { lab[n]=PDNA_LBL_HATCH; act[n++]=A_HATCH; }   /* eggs only: reveal + level 5 */
    if (!is_party) { lab[n]=PDNA_LBL_MOVE; act[n++]=A_MOVE; }                    /* box: pick up + reposition */
    else if (g_party_tobox_allowed) { lab[n]=PDNA_LBL_MOVE_TO_BOX; act[n++]=A_TOBOX; }  /* party popup: carry out to a box */
    lab[n]=PDNA_LBL_COPY;    act[n++]=A_COPY;
    if (g_clip.occupied) { lab[n]=PDNA_LBL_PASTE; act[n++]=A_PASTE; }
    lab[n]=PDNA_LBL_DUPLICATE; act[n++]=A_DUP;
    if (!is_bank) { lab[n]=PDNA_LBL_TO_DAYCARE; act[n++]=A_DAYCARE; }  /* deposit into the daycare (all games incl. FR/LG) */
    if (is_bank) { lab[n]=PDNA_LBL_TO_GAME; act[n++]=A_TOGAME; }   /* bank: inject into the loaded save */
    else         { lab[n]=PDNA_LBL_EXPORT_PK; act[n++]=A_EXPORT; }/* PC/party: write a .pk3 to the bank dir */
    if (m0.heldItem && !g_item_held) { lab[n]=PDNA_LBL_TAKE_ITEM; act[n++]=A_TAKEITEM; }
    if (g_item_held)                 { lab[n]=PDNA_LBL_GIVE_ITEM; act[n++]=A_GIVEITEM; }
    lab[n]=PDNA_LBL_RELEASE;   act[n++]=A_RELEASE;
  } else {                                              /* empty slot */
    if (!is_party) { lab[n]=PDNA_LBL_CREATE; act[n++]=A_CREATE; }   /* build a mon from nothing (box/bank) */
    if (g_clip.occupied) { lab[n]=PDNA_LBL_PASTE_HERE; act[n++]=A_PASTE; }
    if (n == 0) return false;                            /* empty party slot, nothing to paste */
  }
  lab[n]=PDNA_LBL_CANCEL; act[n++]=A_CANCEL;

  char title[16];
  ui_truncate(title, occupied ? (m0.nickname[0] ? m0.nickname : pk_species_name(m0.species)) : "EMPTY", 11);
  /* Scroll if more actions than fit, and fit is measured against the FOOTER, not the
   * screen: this popup is drawn over the box/party screen while that screen's own hints
   * are still on the bottom row, and its 146 px panel used to end at y=152 — shearing the
   * top off "A menu UP SEL B" / "U/D/L/R B" underneath it. */
  int my, mh;
  const int vis = ui_popup_vfit(n, PDNA_MONMENU_ROW_H, PDNA_MONMENU_HEAD,
                                PDNA_MONMENU_FOOT, &my, &mh);
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

/* Recompose the whole party icon COLUMN (x 2..35) for the current bob frame, one
 * scanline at a time: fill each line with that row's background (the selection bar is
 * UI_SEL, others UI_BG), then overlay every icon that crosses the line IN ORDER so the
 * row overlap layers exactly like the static transparent draw — no solid bg square, no
 * cut-off feet. compose-then-DMA (no erase) => flicker-free; runs only on idle frames. */
static u16 __attribute__((aligned(4))) s_pcol[34];
static void party_bob_recompose(int n, int sel, int frame) {
  /* Resolve every icon ONCE, up front, for two reasons the per-scanline version got
   * wrong — it called the accessor 139 times per mon per tick and never checked it.
   *
   * 1. NULL. mon_icon_for_form_frame returns 0 for the whole artless build
   *    (art_fallbacks.c's weak stub), for internal ids 0 and 252..276, and for
   *    anything past 411 — which a bad-egg slot can hold, and Guy's saves carry
   *    deliberate ACE glitch mons. Adding a row offset to NULL and reading through it
   *    lands in BIOS space; on hardware that is open bus, not the zeroes an emulator
   *    tends to return, so roughly half the halfwords pass the `p & 0x8000` test and
   *    speckle garbage over the icon column every 0.5 s. Every other consumer in the
   *    app guards this (ui.c:99, ui.c:182, and the static party draw below).
   * 2. Eggs. The static draw branches on isEgg and uses mon_icon_egg_frame; this did
   *    not, so an egg in the party visibly turned into the hatched species half a
   *    second after the last keypress and turned back on the next press. The day-care
   *    bob gets this right — the party bob was the outlier. */
  const u16* base[6];
  int have = 0;
  if (n > 6) n = 6;
  for (int i = 0; i < n; i++) {
    base[i] = (g_party[i].isEgg && !g_party[i].isBadEgg)
                ? mon_icon_egg_frame((uint8_t)frame)
                : mon_icon_for_form_frame(g_party[i].species, g_party[i].form, (uint8_t)frame);
    if (base[i]) have = 1;
  }
  if (!have) return;   /* artless, or nothing drawable: leave the column exactly as drawn */

  rumble_io_suspend();   /* composes from mon_icon ROM data; mute the cart-bus motor toggle */
  int sry = (n && sel >= 0 && sel < n) ? 17 + sel * 21 : -100;   /* selected panel y..y+20 */
  for (int y = 12; y <= 150; y++) {
    u16 bg = (y >= sry && y <= sry + 20) ? UI_SEL : UI_BG;
    for (int dx = 0; dx < 34; dx++) s_pcol[dx] = bg;
    for (int i = 0; i < n; i++) {
      int ry, iy; party_icon_y(i, &ry, &iy);
      if (y < iy || y >= iy + MON_ICON_H) continue;
      if (!base[i]) continue;
      const u16* row = base[i] + (y - iy) * MON_ICON_W;
      for (int dx = 0; dx < MON_ICON_W; dx++) { u16 p = row[dx]; if (p & 0x8000) s_pcol[1 + dx] = (u16)(p & 0x7FFF); }
    }
    dma3_cpy(&vid_mem[y * 240 + 2], s_pcol, 34 * 2);   /* x=2 even, 68 bytes -> word-aligned */
  }
  rumble_io_resume();
}

static int party_list(void) {
  int sel = 0, anim_ctr = 0, frame = 0;
  for (;;) {
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
    for (int i = 0; i < g_nparty; i++) {
      int ry, iy; party_icon_y(i, &ry, &iy);
      PkMon* p = &g_party[i];
      if (p->isEgg && !p->isBadEgg) ui_sprite(3, iy, MON_ICON_W, MON_ICON_H, mon_icon_egg_frame((uint8_t)frame));
      else ui_sprite(3, iy, MON_ICON_W, MON_ICON_H, mon_icon_for_form_frame(p->species, p->form, (uint8_t)frame));
      char nm[16];
      ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
      siprintf(line, "%-11s Lv%u", nm, (unsigned)p->level);
      ui_text(40, ry + 3, i == sel ? UI_SELTEXT : UI_TEXT, line);
      siprintf(line, "%s%s%s", pk_species_name(p->species),
               p->isShiny ? "  *" : "", p->isEgg ? "  EGG" : "");
      ui_text(40, ry + 12, UI_DIM, line);
    }

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, "A actions  START menu  B back");

    /* idle 2-frame bob (compose-over-DMA, no erase). Animate only on frames with NO key
     * pending so navigation never stutters; recompose every icon in order so the row
     * overlap layers exactly like the static draw (each over its own row's background). */
    u16 k, fresh;
    const u16 mask = KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_START;
    do {
      vsync();
      fresh = key_hit(mask);
      k = fresh | key_repeat(KEY_UP | KEY_DOWN);
      if (!k && app_anim_enabled(ANIM_PARTY) && g_nparty && ++anim_ctr >= 30) {
        anim_ctr = 0; frame ^= 1;
        party_bob_recompose(g_nparty, sel, frame);
      }
    } while (!k);
    if      (fresh & (KEY_UP | KEY_DOWN)) snd_move();
    else if (fresh & KEY_A)               snd_ok();
    else if (fresh & KEY_B)               snd_back();
    else if (fresh & KEY_START)           snd_tab();

    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < g_nparty - 1) sel++; }
    else if (k & KEY_B)    return 0;
    else if (k & KEY_START) return 2;
    else if ((k & KEY_A) && g_nparty > 0) {
      uint16_t doff = g_frlg ? 0x0038 : 0x0238;
      uint8_t* rec = g_sb1 + doff + (uint32_t)sel * 100;     /* party lives in SaveBlock1 (ids 1..4) */
      if (app_mon_menu(rec, true, false, app_commit_sb1, g_sb1, -1, sel)) {   /* party -> editor -> commit */
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
static void flags_raw_view(bool* dirty, bool* warned) {
  int N = pk_flags_count(g_game), flagn = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "RAW FLAGS");
    ui_text(6, 14, UI_WARN, "Raw flags can break a save!");
    ui_hline(0, 24, UI_SCR_W, UI_BORDER);
    if (flagn >= N) flagn = N - 1; if (flagn < 0) flagn = 0;
    char row[40];
    for (int i = -6; i <= 6; i++) {
      int fn = flagn + i; if (fn < 0 || fn >= N) continue;
      int y = 84 + i * 9; bool s = (i == 0);
      siprintf(row, "Flag 0x%03X (%d)  %s", fn, fn, pk_flag_get(g_sb1, g_game, fn) ? "ON" : "off");
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      ui_text(8, y, s ? UI_SELTEXT : (pk_flag_get(g_sb1, g_game, fn) ? UI_OK : UI_DIM), row);
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, "A toggle  U/D  SEL jump#  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) return;
    else if (k & KEY_UP)   { if (flagn > 0) flagn--; }
    else if (k & KEY_DOWN) flagn++;
    else if (k & KEY_SELECT) flagn = (int)osk_number("FLAG #", flagn, N - 1);
    else if (k & KEY_A) {
      if (!*warned) { msg_wait("CAUTION", UI_WARN, "Toggling story flags can", "soft-lock the save."); *warned = true; }
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
static uint32_t s_flags_folded = 0xFFFFFFFFu;
#define NF_MAX_HDRS 32                       /* bits available in s_flags_folded */

/* Owning-header ordinal per row, cached once per table — the naive rescan made
 * nf_visible O(row) and cursor moves near the bottom of the ~490-row Emerald
 * table O(n^2) per keypress (Guy: "really slow toward the bottom").
 * SIZE RULE: NF_ORD_MAX must stay >= the largest per-game NamedFlag row count in
 * data_tables.c (Emerald ~492 before "Fly destinations", ~530 after). A row past
 * the cap gets ordinal 0, folds under the FIRST header and renders in the wrong
 * section — silently, with no assert. Grow this when a table grows. */
#define NF_ORD_MAX 768
static const NamedFlag* s_nf_for = 0;
static uint8_t s_nf_ord[NF_ORD_MAX];
static void nf_cache(const NamedFlag* nf, int nc) {
  if (s_nf_for == nf) return;
  int o = -1;
  for (int i = 0; i < nc && i < NF_ORD_MAX; i++) {
    if (nf[i].num == NAMED_FLAG_HEADER) o++;
    s_nf_ord[i] = (uint8_t)(o < 0 ? 0 : (o >= NF_MAX_HDRS ? NF_MAX_HDRS - 1 : o));
  }
  s_nf_for = nf;
}
static int nf_hdr_ord(const NamedFlag* nf, int r) {    /* ordinal of row r's owning header */
  (void)nf;
  return (r >= 0 && r < NF_ORD_MAX) ? s_nf_ord[r] : 0;
}
static bool nf_visible(const NamedFlag* nf, int nc, int r) {
  if (r >= nc || nf[r].num == NAMED_FLAG_HEADER) return true;   /* raw row + headers always */
  return !((s_flags_folded >> nf_hdr_ord(nf, r)) & 1u);
}
static int nf_step(const NamedFlag* nf, int nc, int total, int r, int dir) {
  for (int i = r + dir; i >= 0 && i < total; i += dir)
    if (nf_visible(nf, nc, i)) return i;
  return r;                                             /* top/bottom stop */
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
  for (;;) {
    /* Flags tab computes its layout FIRST: when only the cursor moved (same window,
     * same folds) we repaint just the two affected rows instead of the whole screen
     * (the full-refresh flicker Guy flagged; same idea as pick_species). */
    int nc = 0, total = 0; bool part = false;
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
    } else f_valid = false;

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
      int N = pk_game_stat_count(g_game) + 2;
      if (sel >= N) sel = N - 1;
      if (sel < top) top = sel; if (sel >= top + 14) top = sel - 13;
      char row[44];
      for (int i = 0; i < 14 && top + i < N; i++) {
        int r = top + i, y = 16 + i * 9; bool s = (r == sel);
        if (r == 0)      siprintf(row, "%-20s %lu", "Money", (unsigned long)pk_money(g_sb1, g_sb2, g_game));
        else if (r == 1) siprintf(row, "%-20s %u",  "Coins", (unsigned)pk_coins(g_sb1, g_sb2, g_game));
        else             siprintf(row, "%-20s %lu", pk_game_stat_name(r - 2), (unsigned long)pk_game_stat(g_sb1, g_sb2, g_game, r - 2));
        char rt[44]; ui_truncate(rt, row, 29);
        if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
        ui_text(4, y, s ? UI_SELTEXT : UI_TEXT, rt);
      }
      ui_text(4, 152, UI_DIM, lock ? "A edit  U/D  B done" : "A edit  U/D  L/R tab  B done");
    } else if (tab == 1) {                       /* ---- bag ---- */
      int cap = pk_pocket_cap(g_game, pocket);
      if (sel >= cap) sel = cap - 1;
      if (sel < top) top = sel; if (sel >= top + 13) top = sel - 12;
      char hh[40]; siprintf(hh, "%s  (%d)", pk_pocket_name(pocket), cap);
      ui_text(6, 15, UI_DIRCLR, hh);
      char row[44];
      for (int i = 0; i < 12 && top + i < cap; i++) {
        int sl = top + i, y = 26 + i * 9; bool s = (sl == sel);
        uint16_t id = pk_bag_item(g_sb1, g_game, pocket, sl);
        uint16_t q  = pk_bag_qty(g_sb1, g_sb2, g_game, pocket, sl);
        if (id) siprintf(row, "%-16s x%u", pk_item_name(id), q);
        else    strcpy(row, "-");
        char rt[44]; ui_truncate(rt, row, 29);
        if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
        ui_text(4, y, s ? UI_SELTEXT : UI_TEXT, rt);
      }
      ui_text(4, 152, UI_DIM, "A edit  SEL pocket  B done");
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

/* Preset colour picker: the 15 named Pokéblock colours with swatches (no free text). */
static int pick_pokeblock_color(uint8_t cur) {
  int sel = cur < 15 ? cur : 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "BLOCK COLOR");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 15; i++) {
      int col = i / 8, row = i % 8, x = 6 + col * 118, y = 18 + row * 16; bool s = (i == sel);
      if (s) ui_panel(x - 2, y - 1, 114, 15, UI_SEL, UI_TITLE);
      pokeblock_swatch(x, y, 12, (uint8_t)i);
      ui_text(x + 18, y + 2, s ? UI_SELTEXT : UI_TEXT, pk_pokeblock_color_name((uint8_t)i));
    }
    ui_text(4, 152, UI_DIM, "A pick  U/D/L/R  B cancel");
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
static bool pokeblock_edit(int idx) {
  static const char* const FL[5] = { "Spicy", "Dry", "Sweet", "Bitter", "Sour" };
  PkPokeblock pb; pk_pokeblock_get(g_sb1, g_game, idx, &pb);
  uint8_t* fv[5] = { &pb.spicy, &pb.dry, &pb.sweet, &pb.bitter, &pb.sour };
  int sel = 0; bool changed = false;                          /* rows: 0 colour, 1..5 flavours, 6 feel, 7 delete */
  for (;;) {
    ui_clear();
    char t[24]; siprintf(t, "POKEBLOCK %d", idx + 1); ui_text(4, 2, UI_TITLE, t);
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    bool s0 = (sel == 0);
    if (s0) ui_panel(2, 17, 236, 20, UI_SEL, UI_TITLE);
    pokeblock_swatch(8, 19, 16, pb.color);
    ui_text(30, 23, s0 ? UI_SELTEXT : UI_TEXT, pk_pokeblock_color_name(pb.color));
    ui_text(150, 23, s0 ? UI_SELTEXT : UI_DIM, "A: pick");
    for (int i = 0; i < 5; i++) {
      int y = 42 + i * 13; bool s = (sel == 1 + i); int fb = *fv[i]; if (fb > 99) fb = 99;
      if (s) ui_panel(2, y - 1, 236, 12, UI_SEL, UI_TITLE);
      ui_text(8, y, s ? UI_SELTEXT : UI_DIM, FL[i]);
      char b[8]; siprintf(b, "%3u", (unsigned)*fv[i]); ui_text(56, y, s ? UI_SELTEXT : UI_TEXT, b);
      ui_progress(84, y + 1, 150, 6, fb * 150 / 99, UI_OK, UI_PANEL, UI_BORDER);
    }
    { int y = 42 + 5 * 13; bool s = (sel == 6); int fb = pb.feel; if (fb > 99) fb = 99;
      if (s) ui_panel(2, y - 1, 236, 12, UI_SEL, UI_TITLE);
      ui_text(8, y, s ? UI_SELTEXT : UI_DIM, "Feel");
      char b[8]; siprintf(b, "%3u", (unsigned)pb.feel); ui_text(56, y, s ? UI_SELTEXT : UI_TEXT, b);
      ui_progress(84, y + 1, 150, 6, fb * 150 / 99, UI_DIRCLR, UI_PANEL, UI_BORDER); }
    { int y = 42 + 6 * 13 + 4; bool s = (sel == 7);
      if (s) ui_panel(2, y - 1, 236, 11, UI_SEL, UI_TITLE);
      ui_text(8, y, s ? UI_SELTEXT : UI_WARN, "Delete this block"); }
    ui_text(4, 152, UI_DIM, "A edit/pick  <> +/-  U/D  B done");
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
__attribute__((weak)) BgFrame pokeblock_bg(int game) { (void)game; BgFrame f = { 0, 0, PB_BG_W }; return f; }
__attribute__((weak)) const uint16_t* pokeblock_hl(int game, int state) {
  (void)game; (void)state; return 0;
}
__attribute__((weak)) const uint16_t* pokeblock_flavor_icon(int game, int flavor) {
  (void)game; (void)flavor; return 0;
}

static void pdna_pokeblock(void) {
  if (pk_pokeblock_offset(g_game) == 0) { msg_wait("NO POKEBLOCKS", UI_DIM, "This game lacks contests.", 0); return; }
  bool dirty = false; int sel = 0, top = 0;
  /* The real case chrome, if the art was generated. NULL -> the plain list below,
   * unchanged, exactly as an art-free clone has always drawn it. */
  BgFrame chrome = pokeblock_bg((int)g_game);
  const int VIS = chrome.blob ? PB_ROWS : 13;               /* retail's panel holds 9 rows */
  for (;;) {
    if (chrome.blob) bg_restore(chrome, 0, 0, PB_BG_W, PB_BG_H);  /* 20 LZ77 pages */
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
    if (sel < top) top = sel; if (sel >= top + VIS) top = sel - VIS + 1;
    for (int i = 0; i < VIS && top + i < PK_POKEBLOCK_COUNT; i++) {
      int idx = top + i, y = chrome.blob ? (PB_LIST_Y + i * PB_ROW_H) : (16 + i * 10);
      bool s = (idx == sel);
      PkPokeblock pb; pk_pokeblock_get(g_sb1, g_game, idx, &pb);
      bool occ = pk_pokeblock_occupied(&pb);
      if (chrome.blob) {
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
        if (s) ui_panel(2, y - 1, 236, 10, UI_SEL, UI_TITLE);
        char num[6]; siprintf(num, "%2d", idx + 1); ui_text(6, y + 1, s ? UI_SELTEXT : UI_DIM, num);
        if (occ) {
          pokeblock_swatch(28, y, 8, pb.color);
          char row[40]; siprintf(row, "%-8s  feel %u", pk_pokeblock_color_name(pb.color), (unsigned)pb.feel);
          ui_text(42, y + 1, s ? UI_SELTEXT : UI_TEXT, row);
        } else ui_text(28, y + 1, s ? UI_SELTEXT : UI_DIM, "(empty)");
      }
    }
    if (chrome.blob) {
      /* The bottom-left white panel is the game's FLAVOR/FEEL board — retail fills
       * it for the selected block and ours sat blank (Guy, HW round 2). Same
       * geometry as retail (labels at (16/64, 104/120/136), the has-flavor icon one
       * tile left of each label, FEEL's value right-aligned at (88,136)) — but as
       * an editor we also print each flavor's VALUE after its label. Dark ink: the
       * panel is white. The chrome re-blits every pass, so no erase bookkeeping. */
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

static void pdna_events(void) {
  const EventTicket* T; int n = event_tickets(&T);
  int mgf = mg_enable_flag();
  bool dirty = false; int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 2, UI_TITLE, "EVENT TICKETS");
    bool mg = (mgf >= 0) && pk_flag_get(g_sb1, g_game, mgf);
    if (mgf >= 0) ui_text(150, 2, mg ? UI_OK : UI_DIM, mg ? "MG: on" : "MG: off");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < n; i++) {
      int y = 20 + i * 18; bool s = (i == sel), on = ticket_granted(&T[i]);
      if (s) ui_panel(2, y - 1, 236, 17, UI_SEL, UI_TITLE);
      ui_text(8, y, s ? UI_SELTEXT : UI_TEXT, T[i].name);
      ui_text(150, y, on ? UI_OK : UI_DIM, on ? "READY" : "-");
      char sub[40]; siprintf(sub, "-> %s", T[i].mon);
      ui_text(12, y + 8, s ? UI_SELTEXT : UI_DIM, sub);
    }
    ui_text(4, 152, UI_DIM, "A grant  U/D  B done");
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
/* ---- cute procedural Day-Care scene ---- */

/* Optional generated yard background (tools/gen_daycare_bg.py): a Gen-3-style yard
 * with lava / ponds / a rocky hill / a power line / the day-care house. Git-ignored;
 * when absent the daycare falls back to the procedural scene helpers below. */
#if defined(__has_include) && __has_include("daycare_bg_data.h")
#  include "daycare_bg_data.h"
#  define HAVE_DAYCARE_BG 1
#endif

#ifndef HAVE_DAYCARE_BG    /* procedural-scene helpers — only built without a yard bg */
static void dc_tri_roof(int cx, int top, int halfBase, int h, u16 fill, u16 edge) {
  for (int j = 0; j <= h; j++) { int w = halfBase * j / h; m3_line(cx - w, top + j, cx + w, top + j, fill); }
  m3_line(cx, top, cx - halfBase, top + h, edge);
  m3_line(cx, top, cx + halfBase, top + h, edge);
}

static void dc_house(void) {
  const int hx = 166, ww = 60, wallTop = 50, wallBot = 90, cx = hx + ww / 2;
  dc_tri_roof(cx, 34, ww / 2 + 4, 16, RGB15(26, 7, 5), RGB15(16, 3, 2));        /* red roof */
  ui_fill_rect(hx, wallTop, ww, wallBot - wallTop, RGB15(27, 22, 15));           /* tan walls */
  m3_frame(hx, wallTop, hx + ww - 1, wallBot - 1, RGB15(14, 10, 4));
  ui_fill_rect(cx - 8, wallBot - 20, 16, 20, RGB15(15, 9, 3));                   /* door */
  m3_frame(cx - 8, wallBot - 20, cx + 7, wallBot - 1, RGB15(9, 5, 1));
  m3_plot(cx + 4, wallBot - 10, RGB15(31, 28, 8));                               /* knob */
  ui_fill_rect(hx + 6, wallTop + 6, 13, 13, RGB15(16, 26, 31));                  /* window */
  m3_frame(hx + 6, wallTop + 6, hx + 18, wallTop + 18, RGB15(9, 5, 1));
  m3_line(hx + 12, wallTop + 6, hx + 12, wallTop + 18, RGB15(9, 5, 1));
  m3_line(hx + 6, wallTop + 12, hx + 18, wallTop + 12, RGB15(9, 5, 1));
  ui_fill_rect(hx + 6, wallTop + 23, 48, 11, RGB15(30, 27, 14));                 /* sign */
  m3_frame(hx + 6, wallTop + 23, hx + 53, wallTop + 33, RGB15(14, 9, 0));
  ui_text(hx + 9, wallTop + 25, RGB15(8, 5, 0), "DAYCARE");
}

static void dc_fence(int x0, int x1, int y) {
  const u16 w = RGB15(30, 30, 28), d = RGB15(18, 18, 16);
  m3_line(x0, y + 3, x1, y + 3, w);
  m3_line(x0, y + 6, x1, y + 6, d);
  for (int x = x0; x < x1; x += 10) { ui_fill_rect(x, y, 3, 9, w); m3_plot(x, y, d); m3_plot(x + 2, y, d); }
}
#endif /* !HAVE_DAYCARE_BG */

static void dc_pointer(int cx, int y) {           /* small downward arrow over the picked mon */
  const u16 c = RGB15(31, 28, 8);
  for (int j = 0; j < 5; j++) { int w = 4 - j; m3_line(cx - w, y + j, cx + w, y + j, c); }
}

/* Session RNG (a counter + the cart RTC when present). Used for created-mon PIDs
 * (app_create_mon) and, when the yard art is compiled in, the Day-Care visitors —
 * so it must live OUTSIDE the HAVE_DAYCARE_BG block: the artless build still
 * creates Pokemon. */
static uint32_t dc_seed(void) {
  static uint32_t ctr = 0;
  ctr += 0x9E3779B9u;
  uint32_t e = ctr ^ ((uint32_t)g_vinfo.tid_public << 13);
  GbaRtcTime t;
  if (gba_rtc_get(&t)) e ^= (uint32_t)(t.second + t.minute * 60 + t.hour * 3600) * 2654435761u;
  return e | 1u;
}

#ifdef HAVE_DAYCARE_BG
/* Areas on the daycare map bg (icon-CENTRE SCREEN coords; bg blitted at screen y 12),
 * TWO mon slots each so two mons in the same area don't overlap. Mirrors the SLOTS in
 * tools/gen_daycare_img.py (aligned to the lava/water/trees/yellow/flowers in the map). */
enum { DR_LAVA, DR_WATER, DR_SKY, DR_ELEC, DR_GRASS, DR_EMPTY, DR_COUNT };
/* Two slots per area, spaced so the 32x32 icons don't overlap (>=32px apart in one
 * axis) — the lava pit is tall so its two slots stack vertically, the others spread
 * horizontally. cx/cy are CENTERS; the icon is drawn at (cx-16, cy-16). */
static const struct { int cx, cy; } DC_SPOT[DR_COUNT][2] = {
  { { 34, 28}, { 52, 64} },    /* LAVA  - rock/ground/fire (red area; stacked) */
  { {154, 82}, {208, 96} },    /* WATER - water (below the waterfall)      */
  { {156, 28}, {206, 36} },    /* SKY   - flying (the trees, top-right)    */
  { { 88, 34}, {124, 52} },    /* ELEC  - electric (yellow diamond area)   */
  { { 26, 94}, { 66,108} },    /* GRASS - grass/bug (the flower beds)      */
  { { 96, 84}, {134,104} },    /* EMPTY - everyone else (plain grass)      */
};
#define DC_MAXDECO 5
static int      s_deco_x[DC_MAXDECO], s_deco_y[DC_MAXDECO];
static uint16_t s_deco_sp[DC_MAXDECO];
static int      s_ndeco = 0;                  /* decoration mons actually placed       */
static uint16_t s_deco_roll[DC_MAXDECO];      /* the 2..5 random species this visit     */
static int      s_ndeco_roll = 0;
static uint32_t s_dc_visit_rng = 1;           /* per-visit stream for dual-type area picks */
/* Areas a species may live in, by the user's type rules: FIRE -> lava (so a rock/
 * ground mon is only in the lava if it's also fire); water -> water; flying -> trees;
 * electric -> yellow; grass/bug -> grass. A dual-type with two qualifying types gets
 * BOTH areas and *rng picks one (so e.g. a Water/Flying mon randomly sits in the water
 * or the trees each visit). No qualifying type (incl. non-fire rock/ground) -> empty. */
static int dc_region_pick(uint16_t species, uint32_t* rng) {
  /* Each of the mon's TWO types can place it: fire->lava, water->water, flying->sky,
   * electric->elec, grass/bug->grass. A type with no dedicated area (dragon, normal,
   * rock/ground when not also fire, ...) instead allows the fallback spots: grass + empty.
   * So e.g. Dragonite (dragon+flying) can be in the sky OR the grass/empty, and a plain
   * rock/ground mon stays in grass/empty (only fire puts a rock/ground mon in the lava). */
  uint8_t t[2] = { pk_species_type1(species), pk_species_type2(species) };
  int cand[DR_COUNT], nc = 0;
  bool fallback = false;
  for (int i = 0; i < 2; i++) {
    if (i == 1 && t[1] == t[0]) break;                          /* mono-type: count once */
    int area;
    switch (t[i]) {
      case 10: area = DR_LAVA;  break;                          /* fire    */
      case 11: area = DR_WATER; break;                          /* water   */
      case 2:  area = DR_SKY;   break;                          /* flying  */
      case 13: area = DR_ELEC;  break;                          /* electric*/
      case 12: case 6: area = DR_GRASS; break;                  /* grass/bug */
      default: fallback = true; continue;                       /* no area -> grass + empty */
    }
    int dup = 0; for (int k = 0; k < nc; k++) if (cand[k] == area) dup = 1;
    if (!dup) cand[nc++] = area;
  }
  if (fallback) {
    int g = 0, e = 0; for (int k = 0; k < nc; k++) { if (cand[k] == DR_GRASS) g = 1; if (cand[k] == DR_EMPTY) e = 1; }
    if (!g) cand[nc++] = DR_GRASS;
    if (!e) cand[nc++] = DR_EMPTY;
  }
  if (nc == 0) return DR_EMPTY;
  if (nc == 1) return cand[0];
  *rng = *rng * 1103515245u + 12345u;
  return cand[(*rng >> 16) % (uint32_t)nc];
}
/* Reserve a slot in area `rg` (2 per area); if full, fall back to the EMPTY area.
 * Returns rg*2+slot, or -1 if even EMPTY is full. */
static int dc_take_slot(int used[DR_COUNT][2], int rg, uint32_t* rng) {
  int s0 = 0;                                  /* when both slots free, pick one at random */
  if (!used[rg][0] && !used[rg][1]) { *rng = *rng * 1103515245u + 12345u; s0 = (int)((*rng >> 16) & 1u); }
  if (!used[rg][s0])     { used[rg][s0] = 1;     return rg * 2 + s0; }
  if (!used[rg][1 - s0]) { used[rg][1 - s0] = 1; return rg * 2 + (1 - s0); }
  if (!used[DR_EMPTY][0]) { used[DR_EMPTY][0] = 1; return DR_EMPTY * 2 + 0; }
  if (!used[DR_EMPTY][1]) { used[DR_EMPTY][1] = 1; return DR_EMPTY * 2 + 1; }
  return -1;
}
/* Roll this visit's YARD VISITORS (called once on entry).
 *
 * READ THIS BEFORE BELIEVING THE YARD. The Gen-3 Day-Care holds EXACTLY TWO
 * Pokemon: dc_rescan reads slots 0 and 1 at `g_sb1 + base + i*stride` and nothing
 * else. Every other mon on this screen is scenery THIS VIEWER INVENTS for the
 * picture — 2..5 random species, re-rolled on every entry, seeded from a session
 * counter and the cart RTC. They are never read from or written to the save, they
 * occupy no slot, and they can never be selected (`sel` only indexes the boarders,
 * L/R only swaps when n == 2, and the A-menu acts on recs[sel]).
 *
 * They are always internal species 1..251, i.e. Kanto/Johto only — never a Hoenn
 * mon — which is itself the tell that they are not save data. The screen now says
 * so out loud (pk_daycare_yard_note), draws them hazed and behind the real pair,
 * and Settings > Yard visitors turns them off. */
static void dc_roll_decos(void) {
  uint32_t rng = dc_seed();
  rng = rng * 1103515245u + 12345u;
  s_ndeco_roll = 2 + (int)((rng >> 16) % 4);            /* 2..5 */
  for (int i = 0; i < s_ndeco_roll; i++) {
    rng = rng * 1103515245u + 12345u;
    s_deco_roll[i] = (uint16_t)(1 + (rng >> 9) % 251);   /* internal species 1..251 */
  }
  s_dc_visit_rng = rng | 1u;                              /* seed the area-pick stream for this visit */
}
/* Compose a 32x32 icon over the bg image at screen (x,y) and DMA each scanline (no
 * separate erase => no flicker on the single Mode-3 buffer). x is forced even for
 * the word-aligned DMA. */
/* `num` is the icon's weight out of 8 when it is blended with the yard behind it:
 * 8 = fully opaque (the player's own boarders), lower = hazed into the background
 * (the invented yard visitors, so they read as scenery). Same blend as
 * ui_panel_alpha. Deliberately NOT greyscale — grey already means "not seen yet"
 * in the Pokedex and would say the wrong thing here. */
static u16 __attribute__((aligned(4))) s_dcline[MON_ICON_W];   /* 32-bit DMA needs word align */
static void dc_icon_over_bg(int x, int y, const u16* icon, int num) {
  if (!icon) return;
  rumble_io_suspend();   /* reads icon + daycare_bg from ROM per pixel; mute the motor toggle */
  x &= ~1;
  for (int j = 0; j < MON_ICON_H; j++) {
    int yy = y + j;
    if (yy < 12 || yy >= 12 + DAYCARE_BG_H) continue;
    const u16* bg = &daycare_bg[(yy - 12) * DAYCARE_BG_W];
    for (int i = 0; i < MON_ICON_W; i++) {
      int xx = x + i;
      u16 c = (xx >= 0 && xx < DAYCARE_BG_W) ? bg[xx] : 0;
      u16 pxl = icon[j * MON_ICON_W + i];
      if (pxl & 0x8000) {
        u16 s = (u16)(pxl & 0x7FFF);
        if (num >= 8) c = s;
        else {
          int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
          r += ((( s        & 31) - r) * num) >> 3;
          g += ((((s >>  5) & 31) - g) * num) >> 3;
          b += ((((s >> 10) & 31) - b) * num) >> 3;
          c = (u16)(r | (g << 5) | (b << 10));
        }
      }
      s_dcline[i] = c;
    }
    dma3_cpy(&vid_mem[yy * 240 + x], s_dcline, MON_ICON_W * 2);
  }
  rumble_io_resume();
}
#endif /* HAVE_DAYCARE_BG */

static void dc_scene(void) {
#ifdef HAVE_DAYCARE_BG
  rumble_io_suspend();   /* ~37 KB daycare_bg ROM->VRAM DMA; mute the cart-bus motor toggle */
  dma3_cpy(&vid_mem[12 * 240], daycare_bg, DAYCARE_BG_W * DAYCARE_BG_H * 2);  /* yard bg -> scene region */
  rumble_io_resume();
#else
  ui_fill_rect(0, 12, UI_SCR_W, 78, RGB15(16, 25, 31));        /* sky */
  for (int j = -5; j <= 5; j++) for (int i = -5; i <= 5; i++)  /* sun */
    if (i * i + j * j <= 25) m3_plot(22 + i, 26 + j, RGB15(31, 30, 14));
  ui_fill_rect(150, 20, 28, 6, RGB15(30, 31, 31)); ui_fill_rect(158, 16, 14, 5, RGB15(30, 31, 31));
  ui_fill_rect(58, 28, 24, 6, RGB15(30, 31, 31));
  ui_fill_rect(0, 90, UI_SCR_W, 70, RGB15(13, 22, 9));         /* grass (uniform: the bob erases to this) */
  dc_house();
  dc_fence(6, 150, 82);
#endif
}

/* (Re)scan the daycare: fill recs[]/dc[]/phys[] for the up to 2 boarders, place each by
 * terrain, and read the shared egg/step state. Used on entry and after a put/take so the
 * scene always matches g_sb1. Returns the boarder count. phys[k] = physical slot 0/1. */
static int dc_rescan(uint32_t base, uint32_t stride, uint8_t* recs[2], PkMon dc[2], int phys[2],
                     int dcx[2], int dcy[2], bool* egg, int* to_check) {
  int n = 0;
  for (int i = 0; i < 2; i++) {
    uint8_t* rec = g_sb1 + base + (uint32_t)i * stride; PkMon m;
    if (pk_decode_mon(rec, false, &m) && m.species >= 1 && m.species <= 411 && !m.isBadEgg) {
      pk_resolve(&m); dc[n] = m; recs[n] = rec; phys[n] = i; n++;
    }
  }
  /* Egg word @ base+280 in ALL games (u32 on E; u16 on FRLG AND RS), counter @282 (E @284).
   * Verified vs pret/pokeruby: DayCareMail.names @ +0x24 => MailStruct=36 => mail=56 B each;
   * sizeof(DayCare)=0x30B8-0x2F9C=284 => RS: mons 160 + mail 112 + steps@272 + egg@280.
   * (The old RS 276/278 came from a wrong 54-byte-mail assumption.) */
  const uint8_t* op = g_sb1 + base + 280;
  *egg = (g_game == PK_EMERALD) ? ((op[0] | op[1] | op[2] | op[3]) != 0) : ((op[0] | op[1]) != 0);
  int stepc = (g_game == PK_EMERALD) ? g_sb1[base + 284] : g_sb1[base + 282];
  int tc = (g_game == PK_RS) ? stepc : (256 - stepc); if (tc < 1 || tc > 256) tc = 256; *to_check = tc;
#ifdef HAVE_DAYCARE_BG
  int used[DR_COUNT][2] = {{0}};
  uint32_t arng = s_dc_visit_rng;               /* per-visit stream: stable within a visit, varies across */
  for (int i = 0; i < n; i++) {                  /* each boarder -> a slot in its type area (random for dual-type) */
    int slot = dc_take_slot(used, dc_region_pick(dc[i].species, &arng), &arng);
    int rg = (slot < 0) ? DR_EMPTY : slot / 2, sp = (slot < 0) ? 0 : slot % 2;
    int cx = DC_SPOT[rg][sp].cx - 16, cy = DC_SPOT[rg][sp].cy - 16;
    cx &= ~1; if (cx < 2) cx = 2; else if (cx > UI_SCR_W - 34) cx = UI_SCR_W - 34;
    if (cy < 12) cy = 12; else if (cy > 12 + DAYCARE_BG_H - MON_ICON_H) cy = 12 + DAYCARE_BG_H - MON_ICON_H;
    dcx[i] = cx; dcy[i] = cy;
  }
  /* the 2..5 random decoration mons (rolled once per visit) -> their type areas */
  s_ndeco = 0;
  for (int d = 0; d < s_ndeco_roll && s_ndeco < DC_MAXDECO; d++) {
    int slot = dc_take_slot(used, dc_region_pick(s_deco_roll[d], &arng), &arng);
    if (slot < 0) continue;                      /* every slot full -> drop this deco */
    int rg = slot / 2, sp = slot % 2;
    int cx = DC_SPOT[rg][sp].cx - 16, cy = DC_SPOT[rg][sp].cy - 16;
    cx &= ~1; if (cx < 2) cx = 2; else if (cx > UI_SCR_W - 34) cx = UI_SCR_W - 34;
    if (cy < 12) cy = 12; else if (cy > 12 + DAYCARE_BG_H - MON_ICON_H) cy = 12 + DAYCARE_BG_H - MON_ICON_H;
    s_deco_sp[s_ndeco] = s_deco_roll[d]; s_deco_x[s_ndeco] = cx; s_deco_y[s_ndeco] = cy; s_ndeco++;
  }
#else
  for (int i = 0; i < n; i++) { dcx[i] = (i == 0) ? 70 : 108; dcy[i] = 90; }
#endif
  return n;
}

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
  if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); return false; }
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
  if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); return false; }
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
#ifdef HAVE_DAYCARE_BG
  /* Fresh yard visitors each visit — or none, if the user turned them off. BOTH
   * counters must be zeroed: they are file-scope statics that survive the previous
   * visit (dc_rescan places from s_ndeco_roll, the draw loops read s_ndeco), so
   * zeroing one would leave ghosts from last time. */
  if (app_yard_visitors_ok()) dc_roll_decos();
  else { s_ndeco_roll = 0; s_ndeco = 0; s_dc_visit_rng = dc_seed(); }
#endif
  int n = dc_rescan(base, stride, recs, dc, phys, dcx, dcy, &off, &to_check);
  int sel = 0, frame = 0, ctr = 0;
  bool redraw = true, rescan = false;
  for (;;) {
    if (rescan) {                                /* after a put/take: re-read the daycare */
      n = dc_rescan(base, stride, recs, dc, phys, dcx, dcy, &off, &to_check);
      if (sel >= n) sel = n ? n - 1 : 0;
      rescan = false; redraw = true;
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
#ifdef HAVE_DAYCARE_BG
      /* Visitors FIRST and hazed, so a piece of scenery can never paint over one of
       * the player's own Pokemon (four of the slot pairs overlap). */
      for (int i = 0; i < s_ndeco; i++)
        dc_icon_over_bg(s_deco_x[i], s_deco_y[i],
                        mon_icon_for_form_frame(s_deco_sp[i], 0, (uint8_t)(frame & 1)), 6);
      nshow += s_ndeco;
#endif
      for (int i = 0; i < n; i++) {
        const u16* ic = (dc[i].isEgg && !dc[i].isBadEgg) ? mon_icon_egg_frame((uint8_t)(frame & 1))
                                : mon_icon_for_form_frame(dc[i].species, dc[i].form, (uint8_t)(frame & 1));
#ifdef HAVE_DAYCARE_BG
        dc_icon_over_bg(dcx[i], dcy[i], ic, 8);               /* the real pair: fully opaque */
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
#ifdef HAVE_DAYCARE_BG
      ui_ptext(dcx, dcy0 + 2 * dcyp, UI_DIM, pk_daycare_yard_note(n, s_ndeco));
#else
      /* no yard art -> no invented visitors, so the note only states the slot count */
      ui_ptext(dcx, dcy0 + 2 * dcyp, UI_DIM, pk_daycare_yard_note(n, 0));
#endif
      ui_fill_rect(0, PDNA_DCY_FOOTER_Y, UI_SCR_W, 8, UI_BG);
      /* "your 2" is doing the disambiguating work: only the boarders are pickable. */
      ui_ptext(4, PDNA_DCY_FOOTER_Y, UI_DIM, n ? PDNA_DCY_HINT_PAIR
                                 : (g_clip.occupied ? PDNA_DCY_HINT_PUT : "B back"));
    }
    u16 k, fresh;
    do { VBlankIntrWait(); snd_vblank(); key_poll();
         int anim_any = n;
#ifdef HAVE_DAYCARE_BG
         if (s_ndeco) anim_any = 1;
#endif
         if (app_anim_enabled(ANIM_DAYCARE) && anim_any && ++ctr >= 30) { /* idle 2-frame bob (flicker-free) */
           ctr = 0; frame ^= 1;
#ifdef HAVE_DAYCARE_BG
           for (int i = 0; i < s_ndeco; i++)                    /* visitors first + hazed, as on redraw */
             dc_icon_over_bg(s_deco_x[i], s_deco_y[i],
                             mon_icon_for_form_frame(s_deco_sp[i], 0, (uint8_t)(frame & 1)), 6);
#endif
           for (int i = 0; i < n; i++) {                        /* compose icon over the bg + DMA (no erase) */
             const u16* ic = (dc[i].isEgg && !dc[i].isBadEgg) ? mon_icon_egg_frame((uint8_t)(frame & 1))
                                : mon_icon_for_form_frame(dc[i].species, dc[i].form, (uint8_t)(frame & 1));
#ifdef HAVE_DAYCARE_BG
             dc_icon_over_bg(dcx[i], dcy[i], ic, 8);
#else
             ui_blit_over(dcx[i], dcy[i], MON_ICON_W, MON_ICON_H, ic, GRASS);
#endif
           }
           if (n) { int py = dcy[sel] - 7; if (py < 12) py = 12; dc_pointer(dcx[sel] + 16, py); }  /* selection arrow */
         }
         fresh = key_hit(KEY_FULL); k = fresh; } while (!k);
    if      (fresh & KEY_B) snd_back();
    else if (fresh & KEY_A) snd_ok();
    else if (fresh & (KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) snd_move();
    if (k & KEY_B) break;
    else if ((k & (KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R)) && n == 2) { sel ^= 1; redraw = true; }
    else if (k & KEY_A) {
      if (n == 0) {                              /* empty day-care: A = put a copied mon in */
        if (dc_deposit(base, stride)) rescan = true; else redraw = true;
      } else {
        bool can_put = app_can_edit() && g_clip.occupied && n < 2;
        int a = dc_menu(app_can_edit(), can_put);
        if (a == 1)      { if (dc_withdraw(base, stride, recs[sel], phys[sel])) { if (g_pickup_slot >= 0) return; rescan = true; } else redraw = true; }   /* withdraw->PC sets a pickup: close so the box carries it */
        else if (a == 2) { if (dc_deposit(base, stride)) rescan = true; else redraw = true; }
        else if (a == 0) {
          /* Editable summary, exactly like the box/party: a kept edit is written back into
           * the daycare BoxPokemon and SB1 is verified-written. U/D scrolls the pair. */
          uint8_t out[100]; int card = 0; bool saved; int nav;
          do {
            saved = false;
            nav = pdna_inspect(recs[sel], false, app_can_edit(), out, &saved, &card);
            if (saved) {
              memcpy(recs[sel], out, 80);                      /* daycare mons are 80-byte BoxPokemon in SB1 */
              app_stage_sb1();                                 /* deferred: saved when you leave the save */
              if (pk_decode_mon(recs[sel], false, &dc[sel])) pk_resolve(&dc[sel]);  /* refresh the scene copy */
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

/* Change a friend's base owner to one of the 10 NPC presets (writes gender + trainerId[0]
 * into g_sb1; sets *dirty). Omega + non-own only — gated by the caller. */
static void sb_owner_pick(SbRecord* b, uint32_t off, bool* dirty) {
  int cur = sb_owner_class(g_sb1, off, b->slot);
  int sel = (cur >= 0 && cur < 10) ? cur : 0;
  for (;;) {
    ui_clear();
    ui_text(4, 3, UI_TITLE, "OWNER APPEARANCE");
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    ui_text(6, 17, UI_DIM, "Overworld look + battle class");
    ui_text(6, 26, UI_DIM, "(one of 10 presets).");
    for (int i = 0; i < 10; i++) {
      int y = 40 + i * 11; bool s = (i == sel);
      if (s) ui_panel(2, y - 1, 236, 11, UI_SEL, UI_TITLE);
      char r[32]; siprintf(r, "%-13s %s", SB_CLASS_NAME[i], i < 5 ? "(M)" : "(F)");
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, r);
    }
    ui_text(4, 152, UI_DIM, "A set  U/D move  B cancel");
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

/* Secret-base detail: owner info + the boarding party, with a cursor to view/edit each
 * mon (A) and change the owner's overworld look (SELECT). Returns true if g_sb1 was
 * edited (the caller commits or reverts SB1). */
static bool sb_detail(SbRecord* b, uint32_t off) {
  bool dirty = false, can = app_can_edit();
  int psel = 0;
  for (;;) {
    ui_clear();
    char hdr[40]; ui_truncate(hdr, b->trainerName[0] ? b->trainerName : "?", 14);
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

    for (int i = 0; i < SB_PARTY; i++) {
      int col = i % 3, row = i / 3;
      int cx = 4 + col * 78, cy = 56 + row * 46;
      uint16_t sp = b->party.species[i];
      bool s = (i == psel) && (i < b->partyCount);
      if (s) ui_panel(cx - 2, cy - 2, 76, 45, UI_SEL, UI_TITLE);
      if (!sp) { ui_text(cx + 26, cy + 14, UI_DIM, "-"); continue; }
      { uint8_t fo = (sp == 201) ? pk_unown_form(b->party.personality[i])
                   : (sp == 410) ? (uint8_t)pk_get_deoxys_form() : 0;   /* letter/forme, not form 0 (Deoxys internal 410) */
        ui_sprite(cx + 22, cy, MON_ICON_W, MON_ICON_H, mon_icon_for_form(sp, fo)); }
      char nm[16]; ui_truncate(nm, pk_species_name(sp), 9); ui_text(cx, cy + 32, s ? UI_SELTEXT : UI_TEXT, nm);
      siprintf(line, "Lv%u", (unsigned)b->party.level[i]); ui_text(cx, cy + 40, UI_DIRCLR, line);
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, (can && !b->own) ? "A edit mon  SEL owner  B back"
                          : can               ? "A edit mon  B back"
                                              : "A view mon  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) return dirty;
    else if (k & KEY_A) { if (b->partyCount > 0) sb_mon_edit(b, off, psel, &dirty); }
    else if (k & KEY_SELECT) {
      if (!can)        { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); }
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

static void pdna_secretbase(void) {
  Gen3Version v = (g_game == PK_RS) ? G3_VER_RS : (g_game == PK_EMERALD) ? G3_VER_EMERALD : G3_VER_UNKNOWN;
  uint32_t off = gen3_secret_base_offset(v);
  if (off == 0) { msg_wait("SECRET BASES", UI_DIM, "FireRed/LeafGreen has no", "Secret Bases."); return; }
  int n = sb_read_all(g_sb1, off, g_sb_recs);
  if (n == 0) { msg_wait("SECRET BASES", UI_DIM, "None registered yet.", "Set one up or mix first."); return; }

  int sel = 0, top = 0;
  const int VIS = 8;
  for (;;) {
    if (sel < 0) sel = 0; if (sel >= n) sel = n - 1;
    if (sel < top) top = sel; if (sel >= top + VIS) top = sel - VIS + 1;
    ui_clear();
    ui_text(4, 3, UI_TITLE, "SECRET BASES");
    char cnt[16]; siprintf(cnt, "%d/%d", n, SB_COUNT); ui_text(196, 3, UI_DIM, cnt);
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < VIS && top + i < n; i++) {
      const SbRecord* b = &g_sb_recs[top + i];
      int y = 18 + i * 15; bool s = (top + i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      char nm[12]; ui_truncate(nm, b->trainerName[0] ? b->trainerName : "?", 8);
      char row[44];
      siprintf(row, "%-8s %s  Lv%-3d  %dmon", nm, b->own ? "YOU" : (b->gender ? "(F)" : "(M)"),
               sb_party_maxlevel(b), b->partyCount);
      ui_text(8, y, s ? UI_SELTEXT : UI_TEXT, row);
      if (b->battledToday) ui_text(228, y, UI_WARN, "*");
    }
    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, app_can_edit() ? "A view  U/D move  SEL clear  B back"
                                            : "A view  U/D move  B back");
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
      if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
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

/* Dial in the date/time the game should believe is "now"; on A, write + commit. */
static void clock_manual_entry(GbaRtcTime live) {
  enum { F_Y, F_MO, F_D, F_H, F_MI, F_N };
  static const char* const LBL[F_N] = { "Year", "Month", "Day", "Hour", "Minute" };
  int y = live.year, mo = live.month, d = live.day, h = live.hour, mi = live.minute, f = 0;
  for (;;) {
    int dim = days_in_month_ui(y, mo); if (d > dim) d = dim; if (d < 1) d = 1;
    int v[F_N] = { y, mo, d, h, mi };
    ui_clear();
    ui_text(4, 4, UI_TITLE, "SET IN-GAME CLOCK");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < F_N; i++) {
      char r[40]; siprintf(r, "%-7s %0*d", LBL[i], i == F_Y ? 4 : 2, v[i]);
      int yy = 30 + i * 16; bool s = (i == f);
      if (s) ui_panel(2, yy - 2, 150, 13, UI_SEL, UI_TITLE);
      ui_text(10, yy, s ? UI_SELTEXT : UI_TEXT, r);
    }
    ui_text(6, 120, UI_DIM, "Becomes the game's current");
    ui_text(6, 130, UI_DIM, "date/time; resumes events.");
    ui_text(4, 152, UI_DIM, "U/D change  L/R field  A set  B");
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
static void pdna_mirage(void) {
  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "MIRAGE ISLAND");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);

    if (g_game == PK_FRLG) {
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

    u16 k = wait_keys(KEY_A | KEY_UP | KEY_DOWN | KEY_B);
    if (k & KEY_B) return;
    if (!n) continue;
    if (k & KEY_UP)   { sel = (sel > 0) ? sel - 1 : n - 1; continue; }
    if (k & KEY_DOWN) { sel = (sel + 1) % n;               continue; }

    if (!app_can_edit()) { msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
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

static void pdna_clock(void) {
  bool can = app_can_edit();
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

    ui_clear();
    ui_text(4, 4, UI_TITLE, "SAVE CLOCK / RTC");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char b[44];
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
      wait_keys(KEY_B);
      return;
    }
    if (!can) {
      ui_text(6, 102, UI_DIM, "Read-only cart - fixing needs");
      ui_text(6, 112, UI_DIM, "an EZ-Flash Omega.");
      ui_text(4, 152, UI_DIM, "B back");
      wait_keys(KEY_B);
      return;
    }
    ui_text(6, 102, UI_DIM, "Set the cart clock correctly");
    ui_text(6, 112, UI_DIM, "first, then sync.");
    ui_text(4, 152, UI_DIM, "A sync to cart  SEL set  B back");
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

/* Settings > Animations: a per-place On/Off list (the 2-frame bobs + the summary
 * portrait wiggle), each its own toggle. Persists on exit. */
static void anim_settings(void) {
  static const char* const NM[ANIM_COUNT] = { "Box icons", "Party", "Pokedex", "Daycare", "Summary" };
  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "ANIMATIONS");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < ANIM_COUNT; i++) {
      char r[40]; siprintf(r, "%-10s %s", NM[i], app_anim_enabled(i) ? "On" : "Off");
      int y = 30 + i * 16; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, r);
    }
    ui_text(8, 124, UI_DIM, "Moving sprites, per screen.");
    ui_text(4, 152, UI_DIM, "A toggle  U/D move  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) { cfg_save(); return; }
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : ANIM_COUNT - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % ANIM_COUNT;
    else if (k & KEY_A)    g_anim_mask ^= (1u << sel);
  }
}

/* Settings > Rumble: global Strength + Duration steppers (the motor can read as
 * "nothing" on a weak unit, so these are tunable), then a per-cue On/Off list. <>
 * adjust strength/duration (with a live buzz preview); A toggles a cue (+preview).
 * Persists on exit. */
static void rumble_settings(void) {
  enum { R_STR, R_DUR, R_CUE0 };
  _Static_assert(R_CUE0 == PDNA_RMB_STEPPERS, "rumble stepper count out of sync");
  const int NROW = R_CUE0 + RCUE_COUNT;
  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "RUMBLE");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < NROW; i++) {
      char r[40];
      if      (i == R_STR) siprintf(r, "Strength    %d/5  < >", rmbl_get_strength());
      else if (i == R_DUR) siprintf(r, "Duration    %d/5  < >", rmbl_get_duration());
      else { int c = i - R_CUE0; siprintf(r, "%-12s %s", rmbl_cue_name(c), rmbl_cue_enabled(c) ? "On" : "Off"); }
      int y = PDNA_RMB_ROW0_Y + i * PDNA_RMB_ROW_PITCH; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      ui_text(PDNA_SET_ROW_X, y, s ? UI_SELTEXT : UI_TEXT, r);
    }
    /* The help sentence and the control hints are two INDEPENDENT strings: they used to
     * share the y=150 footer row as "GAME RTC on. <>adj A toggle B", which is 29 sys8
     * columns — the row was full, so "back" had to be dropped from the hint to make it
     * fit. Give each its own row. (Strings + rows in pdna_layout.h — the host test
     * measures them from there.) */
    ui_text(PDNA_RMB_HELP_X, PDNA_RMB_HELP_Y1, UI_DIM, PDNA_RMB_HELP1);
    ui_text(PDNA_RMB_HELP_X, PDNA_RMB_HELP_Y2, UI_DIM, PDNA_RMB_HELP2);
    ui_text(PDNA_SET_FOOT_X, PDNA_SET_FOOTER_Y, UI_DIM, PDNA_RMB_FOOT);
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

static void pdna_settings(void) {
  /* Row strings live in pdna_layout.h: sys8 does not clip at the right margin, it WRAPS
   * onto the row below, so their LENGTH is load-bearing and the host test checks it. */
#define SET_MODE_ONE(s) s,
  static const char* const MODE[3] = { PDNA_SET_BACKUP_MODES(SET_MODE_ONE) };
  enum { S_BACKUP, S_ANIM, S_YARD, S_ROM, S_RUMBLE, S_CLEAR, S_CLOSE, S_N };
  _Static_assert(S_N == PDNA_SET_ROWS, "settings row count out of sync with pdna_layout.h");
  int sel = 0;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "SETTINGS");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
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
    char r2[44]; siprintf(r2, "Game ROM:  %s",
                          s_iconrom.ok ? rom_kind_name(s_iconrom_ctx.kind) : "not set");
    const char* rows[S_N] = { r0, "Animations  >", r1, r2, "Rumble  >", PDNA_SET_ROW_CLEAR, "Close" };
    for (int i = 0; i < S_N; i++) {
      /* 7 rows @14px: last band 106..119, clear of the help text */
      int y = PDNA_SET_ROW0_Y + i * PDNA_SET_ROW_PITCH; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      ui_text(PDNA_SET_ROW_X, y, s ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    /* Rebalanced across the two rows: the first was 29 sys8 columns at x=8, i.e. ending
     * on the last pixel column of the screen with no margin at all. */
    ui_text(PDNA_SET_HELP_X, PDNA_SET_HELP_Y1, UI_DIM, PDNA_SET_HELP1);
    ui_text(PDNA_SET_HELP_X, PDNA_SET_HELP_Y2, UI_DIM, PDNA_SET_HELP2);
    ui_ptext(PDNA_SET_HELP_X, PDNA_SET_NOTE_Y, UI_DIM, PDNA_SET_NOTE);
    ui_text(PDNA_SET_FOOT_X, PDNA_SET_FOOTER_Y, UI_DIM, PDNA_SET_FOOT);
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) { cfg_save(); return; }
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
        snd_deny(); msg_wait("NO SD HERE", UI_DIM, "Fuse a ROM into this build", "with tools/fuse_rom.py.");
#else
        app_register_rom();
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

/* ---- Import a .rec from /PokeDNA/battles back into the save (sector 31) -----------
 * The game gates the Frontier Pass "BATTLE RECORD" purely on the sector's own validity
 * (sentinel + battleFlags + byte-sum checksum — pokeemerald CanCopyRecordedBattleSaveData;
 * no other flag), so a valid imported record replays exactly like one just recorded.
 * The save's current record is overwritten (confirmed first); persistence rides the
 * standard backup + verified full-save commit (the whole g_save image is written, and
 * sector 31 is part of it). Omega-only (it is a save edit). */
static bool rec_import(void) {
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
  if (!n) { snd_deny(); msg_wait("IMPORT RECORD", UI_DIM, "No .rec files in", "/PokeDNA/battles."); return false; }

  int sel = 0, top = 0;                                 /* pick one */
  for (;;) {
    if (sel < top) top = sel; if (sel >= top + 14) top = sel - 13;
    ui_clear();
    ui_text(4, 4, UI_TITLE, "IMPORT RECORD");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    for (int i = 0; i < 14 && top + i < n; i++) {
      int r = top + i, y = 20 + i * 9; bool s = (r == sel);
      if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
      char rt[40]; ui_truncate(rt, names[r], 29);
      ui_text(6, y, s ? UI_SELTEXT : UI_TEXT, rt);
    }
    ui_text(4, 152, UI_DIM, "A import  B cancel");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) break;
  }
  char path[64];
  sniprintf(path, sizeof path, PDNA_DIR "/battles/%s", names[sel]);

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
  log_line("record: import %s", names[sel]);
  /* persist: re-committing the (unchanged) SB2 runs the standard backup + verified
   * full-save write, which carries the new sector 31 with it */
  if (!app_commit_sb2()) {
    snd_error(); msg_wait("IMPORT", UI_WARN, "Imported in RAM only —", "save write failed/declined.");
    return true;                                        /* screen still shows it (RAM) */
  }
  snd_save(); msg_wait("IMPORTED", UI_OK, "This is now the save's", "last recorded battle.");
  return true;
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
  while (!g3_record_scan(g_save, g_save_size, &ri)) {  /* none yet -> still offer import */
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
    ui_text(4, 152, UI_DIM, "SEL import  B back");
    u16 k = wait_keys(KEY_SELECT | KEY_B);
    if (k & KEY_B) { snd_back(); return; }
    if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
    rec_import();                                      /* success -> the rescan shows it */
  }
  rmbl_fire(RCUE_ROOM);
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "BATTLE RECORD");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char l[40];
    siprintf(l, "%s  %s", g3_record_facility_name(ri.facility),
             ri.lvl_mode ? "Open Level" : "Level 50");
    ui_text(4, 20, UI_TEXT, l);
    const char* who = ri.names[ri.multiplayer_id][0] ? ri.names[ri.multiplayer_id] : "?";
    siprintf(l, "Recorded by %s (%s)", who, ri.genders[ri.multiplayer_id] ? "F" : "M");
    ui_text(4, 30, UI_TEXT, l);
    siprintf(l, "Seed %08lx  Opp #%u", (unsigned long)ri.rng_seed, ri.opponent_a);
    ui_text(4, 40, UI_DIM, l);
    if (!ri.checksum_ok) ui_text(130, 40, UI_WARN, "CHECKSUM BAD");

    ui_text(4, 54, UI_TITLE, "YOUR TEAM");
    ui_text(124, 54, UI_TITLE, "OPPONENT");
    for (int side = 0; side < 2; side++) {
      const uint8_t* party = g3_record_party(g_save, side);
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
    ui_text(4, 152, UI_DIM, "A export  SEL import  B back");
    u16 k = wait_keys(KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) { snd_back(); return; }
    if (k & KEY_SELECT) {                              /* import an older .rec over this one */
      if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
      if (rec_import()) g3_record_scan(g_save, g_save_size, &ri);   /* show the imported battle */
      continue;
    }
    /* ---- A: export the raw 4 KiB sector (verified write, Omega-only) ---- */
    if (!app_can_edit()) { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); continue; }
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
       * itself cannot carry — all facilities' current streaks, player identity,
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
 * The entries AND the menu geometry live in pdna_layout.h, because
 * tests/host_textfit_test.c asserts on both (panel height, and every label against the
 * 90 px selection band). A copy of them in the test would guard nothing. */
#define NV_ENUM_ONE(id, label) id,
enum { PDNA_NAV_ITEMS(NV_ENUM_ONE) NV_COUNT };
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

static int nav_menu(void) {
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
    ui_ptext(mx + PDNA_NAV_PAD + col * cw, my + PDNA_NAV_HEAD + row * rh, UI_TEXT, L[i]);
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
    else if (k & KEY_A)    return sel;
  }
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
  if (!app_pc_dirty() && !g_sb1_deferred) { pdna_bank_flush_deletions(); return; }  /* PC already saved; still delete carried bank originals */
  if (app_confirm("Save changes?", "Save the moved Pokemon?")) {
    /* GATE the Bank-source deletion on the PC write SUCCEEDING. app_commit_pc() can fail (EZ
     * writes have no retry / verify mismatch / backup-full); on failure the moved mons live only
     * in volatile g_pc and are NOT on the .sav, so deleting their Bank originals would LOSE them
     * (a Bank->PC move — the multi-select chunk amplifies this to a whole box at once). Flush the
     * deletions ONLY after the destination (PC) is verified on disk -> worst case a recoverable
     * duplicate (mons kept in the Bank), never a loss. g_pc_dirty stays set on failure, so the
     * moves are still pending and can be retried. */
    if (app_commit_pc()) pdna_bank_flush_deletions();
  } else {
    gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc);   /* revert PC moves */
    g_pc_dirty = false; g_sb1_deferred = false;          /* drop staged Day-Care (disk untouched) */
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

/* Load the picked save and show it: start in the PC boxes; SELECT toggles to the
 * party list and back; B from either returns to the file browser. */
static void view_save(const char* path) {
  g_pc_dirty = false;                          /* fresh save: no pending moves */
  g_sb1_deferred = false;
  strncpy(g_path, path, sizeof(g_path) - 1);
  g_path[sizeof(g_path) - 1] = 0;
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
    uint32_t fsz = 0;
    if (!flash_ok && fused_sav_present(&fsz) && fsz == (uint32_t)G3_SAVE_FILE_SIZE &&
        fused_sav_read(g_save, fsz)) {
      sz = fsz; err = 0;
      log_line("save: flash blank/invalid -> using the fused save (%lu B)", (unsigned long)fsz);
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

  /* SaveBlock2 (section 0) for the trainer card + per-game layout for stats */
  load_phase_n(8, "saveblock2");
  int s0 = gen3_find_section(g_save, g_vinfo.slot, 0);
  if (s0 >= 0)
    memcpy(g_sb2, g_save + (uint32_t)g_vinfo.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE,
           G3_SECTOR_DATA_SIZE);
  g_game = g_frlg ? PK_FRLG : (g_vinfo.version_guess == G3_VER_RS ? PK_RS : PK_EMERALD);
  /* The FIRST SD access after the read: f_open of the registered game ROM (artless) or
   * a fused-ROM scan. Named apart from the decode steps because it is the only step
   * here that can touch the card, and therefore the only one whose freeze would mean
   * the cart rather than the CPU. */
  load_phase_n(9, "art: open rom");
  app_icon_rom_open();                           /* fused or registered-SD icon source */
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
      if (app_confirm("ADD YOUR GAME ROM?", "Unlocks the real art. B = later"))
        app_register_rom();
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

  load_phase_n(11, "box source");
  BoxSource pcs = pc_box_source();
  pdna_box_clear_carry();                          /* no mon in hand when a save opens */
  pdna_bank_clear_deletions();                     /* no stale Bank->PC deletions from a prior save */
  rmbl_fire(RCUE_ROOM);                            /* entering the save's home "room" */
  /* app_icon_rom_open() above may have opened the user's ROM off the SD; from here on
   * it is the box paint (wallpaper staging + 30 verified icon copies + tile uploads).
   * Breadcrumb #3 fires from inside that paint -- see app_crumb_shown(). */
  load_phase_n(12, g_have_pc ? "first paint: box" : "first paint: party");
  /* Hand the VBlank handler back BEFORE the first full-screen paint: from here the
   * screen is being drawn every frame anyway, so a spinner would only be 144 pixels
   * of a real screen that the heartbeat has no business owning. A freeze from this
   * point on shows a half-painted box, which localises itself. */
  hb_off();
  /* The PC box is "home"; Party / Bank / Daycare / etc. all hang off the START menu.
   * (Saves with no PC fall back to the party list as home.) */
  for (;;) {
    reload_saveblocks();                         /* editors share g_sb1/g_sb2 + commit all SB1 — keep them == the saved image so a declined edit can't ride along */
    int r = g_have_pc ? pdna_box(&pcs) : party_list();
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
      switch (nav_menu()) {
        case NV_PARTY:   { rmbl_fire(RCUE_ROOM); uint8_t dmy[80];   /* the same party popup (no box carry from here) */
                           app_party_overlay(0, 0, 0, false, false, dmy, 0, false); refresh_party = 1; } break;
        case NV_BANK:    rmbl_fire(RCUE_ROOM); if (pdna_bank_show() == 5) app_box_start_set(1); refresh_party = 1; break;   /* bottom-out -> PC tabs; a paste may hit the party */
        case NV_DAYCARE: pdna_daycare(); break;
        case NV_TRAINER: pdna_trainer(g_sb1, g_sb2, &g_vinfo, g_game); break;
        case NV_CLOCK:   pdna_clock(); break;
        case NV_MIRAGE:  pdna_mirage(); break;
        case NV_DEX:     pdna_dex_edit(); break;
        case NV_BAG:     if (app_can_edit()) bag_entry();
                         else msg_wait("BAG", UI_WARN, "Read-only cart.", "Writes need an Omega.");
                         break;
        case NV_DATA:    if (app_can_edit()) data_editor();
                         else { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); } break;
        case NV_SECRET:  rmbl_fire(RCUE_ROOM); pdna_secretbase(); break;
        case NV_POKEBLOCK: if (app_can_edit()) pdna_pokeblock();
                           else { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); } break;
        case NV_EVENTS:   if (app_can_edit()) pdna_events();
                           else { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Needs EZ-Flash Omega.", 0); } break;
        case NV_BATTLEREC: pdna_battle_record(); break;  /* viewing is free; export gates on Omega inside */
        case NV_FRONTIER: pdna_frontier(g_sb1, g_sb2, g_game); break;   /* viewing free; editing gates on Omega inside */
        case NV_FLY:      pdna_fly(g_sb1, g_game); break;        /* viewing free; editing gates on Omega inside */
        case NV_MAP:      pdna_map(g_sb1, g_sb2, g_game); break;  /* the user's own ROM: SD file, or fused into this image */
        case NV_GB: {
#ifdef PDNA_DELTA
          msg_wait("GB IMPORT", UI_DIM, "Needs the SD card.", "Not available in this build.");
#else
          /* Browse for a Gen-1/2 .sav and mount it READ-ONLY as a box source. The
           * loaded save's game is stamped as the origin on anything copied out, so a
           * converted mon claims the cartridge it is actually going into. */
          char gp[PATH_MAX];
          if (app_pick_gb_save(gp, sizeof gp))
            pdna_gen12_show(gp, (uint8_t)(g_game == PK_RS ? 1 : g_game == PK_FRLG ? 4 : 3));
#endif
          break;
        }
        case NV_SETTINGS: pdna_settings(); break;
        default: break;                          /* NV_BACK */
      }
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
  /* Before anything goes deep: paint the unused IWRAM stack so the low-water mark is
   * measurable for the rest of the session. main's own frame is already on the stack,
   * so this covers exactly the region every call below is about to spend. */
  stack_paint();
  log_init();
  log_line("=== PokeDNA (M0) ===");
  log_line("build " __DATE__ " " __TIME__);   /* stamp: proves WHICH binary produced this log
                                               * (stale flashes have faked "still broken" before) */
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
  for (int a = 0; a < 8 && !(active = flashcartio_activate()); a++)
    for (int v = 0; v < 8; v++) vsync();    /* ~130 ms settle, then re-detect */
  if (!active) halt_msg("No flashcart detected! Reseat cart & reboot.");
  log_line("flashcart: %s", flashcart_name());

  FATFS fs;                                  /* lives forever (main never returns) */
  FRESULT fr = FR_NOT_READY;
  for (int a = 0; a < 8; a++) {
    fr = f_mount(&fs, "", 1);                /* opt=1: mount now (reads the FS) */
    if (fr == FR_OK) break;
    log_line("f_mount attempt %d failed (fr=%d)", a, fr);
    ui_clear();
    ui_text(6, 70, UI_TITLE, "Mounting SD card...");
    char rb[32]; siprintf(rb, "retry %d/7", a + 1);
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
  for (;;) {
    char path[PATH_MAX];
    if (browse_pick(path, sizeof(path))) view_save(path);
  }
  return 0;
#endif /* PDNA_DELTA */
}
