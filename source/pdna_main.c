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
#include "sys.h"           /* EWRAM_BSS (idempotent; guarded)                          */
#include "ff.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"
#include "mon_icons.h"
#include "pdna_summary.h"
#include "pdna_box.h"
#include "gen3_trainer.h"
#include "pdna_trainer.h"
#include "pdna_edit.h"
#include "gen3_edit.h"     /* EditMon, gen3_edit_load/commit, em_set_*, em_preview */
#include "gen3_clip.h"     /* ClipMon, slot ops (copy/paste/dup/release) */
#include "pdna_legality.h" /* pdna_legality_show */
#include "pdna_pk.h"     /* pdna_pk_export (.pk3) */
#include "pdna_bank.h"   /* pdna_bank_show (bank = parallel boxes) */
#include "gen3_flags.h"    /* event flags */
#include "gen3_items.h"    /* item bags */
#include "osk.h"           /* osk_search (numeric entry) */
#include "pdna_pick.h"   /* pick_item, pick_move (PC-menu quick editors) */
#include "snd.h"           /* UI sound effects */
#include "pdna_app.h"
#include "savefile.h"
#include "log.h"
#include "ui.h"

#define LOG_PATH      "/PokeDNA_log.txt"
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

/* Big buffers live in EWRAM (.bss), never on the IWRAM stack. */
static u8          EWRAM_BSS g_save[G3_SAVE_FILE_SIZE];   /* 128 KiB raw image       */
static u8          EWRAM_BSS g_sb1[G3_SAVEBLOCK1_BYTES];  /* reassembled SaveBlock1  */
static BrowseEntry EWRAM_BSS g_entries[MAX_ENTRIES];      /* current-dir listing     */
static int         g_count = 0;
static char        EWRAM_BSS g_cwd[PATH_MAX];             /* current directory (set in main) */
static PkMon       EWRAM_BSS g_party[6];                  /* decoded party of the open save  */
static u8          EWRAM_BSS g_pc[G3_PC_BYTES];           /* reassembled PC storage (boxes)  */
static u8          EWRAM_BSS g_sb2[G3_SECTOR_DATA_SIZE];   /* SaveBlock2 (trainer card/stats) */
static ClipMon     EWRAM_BSS g_clip;                       /* one-slot mon clipboard          */

/* ---- VBlank / input discipline (key_poll exactly once per frame) -------- */
static void vsync(void) { VBlankIntrWait(); snd_vblank(); key_poll(); }

/* Central input wait — also the app-wide UI-sound chokepoint. A FRESH d-pad press
 * ticks snd_move (NOT key_repeat, so a held scroll doesn't machine-gun); A/B play
 * the confirm/back earcons. Nearly every screen funnels through here. */
static u16 wait_keys(u16 mask) {
  u16 hit, fresh;
  do {
    vsync();
    fresh = key_hit(mask);
    hit = fresh | key_repeat(mask & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT));
  } while (!hit);
  if (fresh & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) snd_move();
  if (fresh & KEY_A) snd_ok();
  else if (fresh & KEY_B) snd_back();
  return hit;
}

static void init_system(void) {
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

/* Writes (edit mode, later) are Omega-only; surface it from M0. */
static bool cart_writable(void) { return active_flashcart == EZ_FLASH_OMEGA; }

static void halt_msg(const char* msg) {
  log_line("HALT: %s", msg);
  log_flush_to_sd(LOG_PATH);
  ui_clear();
  ui_panel(8, 48, 224, 44, UI_PANEL, UI_WARN);
  ui_text(20, 58, UI_WARN, "HALT");
  ui_text(20, 74, UI_TEXT, msg);
  snd_error();
  while (1) vsync();
}

static int has_sav_ext(const char* n) {
  unsigned L = (unsigned)strlen(n);
  if (L < 4 || n[L - 4] != '.') return 0;
  return (n[L - 3] == 's' || n[L - 3] == 'S') &&
         (n[L - 2] == 'a' || n[L - 2] == 'A') &&
         (n[L - 1] == 'v' || n[L - 1] == 'V');
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

static const char* base_name(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
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
    ui_text(6, 52, UI_DIM,  at_root() ? "Open a folder where your saves live."
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

  ui_hline(0, 147, UI_SCR_W, UI_BORDER);
  ui_text(2, 150, UI_DIM, "A pick  B up  SEL sort  ST menu");
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
  log_flush_to_sd(LOG_PATH);
  VBlankIntrWait();
  flashcartio_reboot();                 /* never returns */
}

/* START menu over the browser: sort key/order, file filter, show-hidden, reboot.
 * Returns true if a setting changed that needs a re-scan. */
static bool browse_menu(void) {
  int sel = 0;
  bool changed = false;
  for (;;) {
    ui_clear();
    ui_text(4, 4, UI_TITLE, "FILE MENU");
    ui_hline(0, 14, UI_SCR_W, UI_BORDER);
    char rows[6][40];
    siprintf(rows[0], "Sort key:  %s", g_sort == SORT_NAME ? "Name" : g_sort == SORT_SIZE ? "Size" : "Date");
    siprintf(rows[1], "Order:     %s", g_sortrev ? "descending" : "ascending");
    siprintf(rows[2], "Files:     %s", g_show_all ? "all files" : ".sav only");
    siprintf(rows[3], "Hidden:    %s", g_show_hidden ? "shown" : "hidden");
    strcpy(rows[4], "Reboot to flashcart menu...");
    strcpy(rows[5], "Close");
    for (int i = 0; i < 6; i++) {
      int y = 26 + i * 16; bool s = (i == sel);
      if (s) ui_panel(2, y - 2, 236, 13, UI_SEL, UI_TITLE);
      ui_text(10, y, s ? UI_SELTEXT : UI_TEXT, rows[i]);
    }
    ui_text(4, 152, UI_DIM, "A change  U/D move  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return changed;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 5;
    else if (k & KEY_DOWN) sel = (sel + 1) % 6;
    else if (k & KEY_A) {
      switch (sel) {
        case 0: g_sort = (BrSortKey)((g_sort + 1) % 3); changed = true; break;
        case 1: g_sortrev = !g_sortrev; changed = true; break;
        case 2: g_show_all = !g_show_all; changed = true; break;
        case 3: g_show_hidden = !g_show_hidden; changed = true; break;
        case 4: do_reboot(); break;            /* returns only if cancelled */
        case 5: return changed;
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
      sort_entries(); sel = 0; top = 0;
    }
    else if (k & KEY_START) { if (browse_menu()) { scan_dir(); sel = 0; top = 0; } }
    else if (k & KEY_B)    { if (!at_root()) { path_up(); scan_dir(); sel = 0; top = 0; } }
    else if (k & KEY_A) {
      if (g_count == 0) continue;
      const BrowseEntry* e = &g_entries[sel];
      char np[PATH_MAX];
      if (!path_join(g_cwd, e->name, np)) continue;
      if (e->is_dir) {
        strcpy(g_cwd, np);
        scan_dir();
        sel = 0; top = 0;
      } else if ((int)strlen(np) < cap) {
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

bool app_can_edit(void) { return active_flashcart == EZ_FLASH_OMEGA; }

static void msg_wait(const char* title, u16 col, const char* l1, const char* l2) {
  ui_clear();
  ui_panel(16, 48, 208, 70, UI_PANEL, col);        /* framed so it reads as a dialog */
  ui_text(28, 58, col, title);
  if (l1) ui_text(28, 80, UI_TEXT, l1);
  if (l2) ui_text(28, 92, UI_DIM, l2);
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

/* Persist `block` (already-patched save-block sections [lo..hi]) into the in-RAM
 * image with fresh checksums, an immutable backup, and a verified write. The
 * single safe write path shared by the full editor and the PC-menu quick edits. */
static bool app_commit_block(int sect_lo, int sect_hi, uint8_t* block) {
  for (int id = sect_lo; id <= sect_hi; id++)
    gen3_write_full_section(g_save, g_vinfo.slot, id,
                            block + (uint32_t)(id - sect_lo) * G3_SECTOR_DATA_SIZE);

  int fail = -1;
  if (!gen3_verify_full_checksums(g_save, g_vinfo.slot, &fail)) {
    log_line("edit: checksum FAIL at section %d", fail);
    snd_error();
    msg_wait("CHECKSUM ERROR", UI_WARN, "Edited image failed checks.", "NOT written.");
    return false;
  }

  log_line("=== edit commit -> %s ===", g_path);
  busy_panel("Backing up original...");           /* safe point: before SD copy */
  char bak[SF_PATH_MAX];
  SfStatus st = sf_backup(g_path, bak, sizeof(bak));
  if (st != SF_OK) {
    log_line("edit: backup failed (%s)", sf_status_str(st));
    log_flush_to_sd(LOG_PATH);
    snd_error();
    msg_wait("BACKUP FAILED", UI_WARN, sf_status_str(st), "Save NOT modified.");
    return false;
  }
  busy_panel("Writing + verifying...");            /* safe point: before SD write */
  st = sf_write_verified(g_path, g_save, G3_SAVE_FILE_SIZE);
  log_line("edit: write %s (backup %s)", st == SF_OK ? "OK" : sf_status_str(st), bak);
  log_flush_to_sd(LOG_PATH);
  if (st != SF_OK) {
    snd_error();
    msg_wait("WRITE FAILED", UI_WARN, sf_status_str(st), "Backup kept; .tmp may remain.");
    return false;
  }
  snd_save();
  grow_in(UI_OK);                                  /* brief success flourish */
  msg_wait("SAVED", UI_OK, "Edit written + verified.", "Original backed up first.");
  return true;
}

/* Commit the trainer block (SaveBlock2 = section 0) / the money block (SaveBlock1
 * = sections 1..4) after the trainer card edits g_sb2 / g_sb1 in place. */
bool app_commit_sb2(void) { return app_commit_block(0, 0, g_sb2); }
bool app_commit_sb1(void) { return app_commit_block(1, 4, g_sb1); }

/* PC-storage dirty flag: set by deferred move-mode swaps, cleared by any successful
 * PC write (which flushes the whole g_pc) or an explicit revert. */
static bool g_pc_dirty = false;
void app_mark_pc_dirty(void) { g_pc_dirty = true; }
bool app_pc_dirty(void)      { return g_pc_dirty; }

bool app_commit_pc(void)  {
  bool ok = app_commit_block(G3_SID_PKMN_STORAGE_START, G3_SID_PKMN_STORAGE_END, g_pc);
  if (ok) g_pc_dirty = false;                       /* the write persisted everything */
  return ok;
}

/* Emerald "Walda" secret-wallpaper pattern (the graphic shown by box wallpaper 16),
 * stored in SaveBlock1. -1 / no-op on the other games. */
int  app_walda_pattern(void) { return (g_game == PK_EMERALD) ? (int)pk_walda_pattern(g_sb1) : -1; }
bool app_set_walda(uint8_t pattern) {
  if (g_game != PK_EMERALD) return false;
  pk_set_walda_pattern(g_sb1, pattern);
  return true;
}

bool app_edit_commit(uint8_t* rec, bool is_party, AppCommitFn commit) {
  uint8_t out[100]; bool saved = false; int card = 0;
  pdna_inspect(rec, is_party, true, out, &saved, &card);   /* party: nav ignored */
  if (!saved) return false;                                  /* viewed only / discarded */
  memcpy(rec, out, is_party ? 100 : 80);                     /* patch in place (rec is inside block) */
  return commit ? commit() : false;
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
    if (saved) { memcpy(rec, out, 80); if (commit && commit()) any = true; }
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

bool app_clip_occupied(void) { return g_clip.occupied; }

/* MOVE (box reposition) request: the A-menu sets this; the box loop consumes it. */
static bool g_move_req = false;
bool app_take_move_request(void) { bool r = g_move_req; g_move_req = false; return r; }

bool app_confirm(const char* title, const char* l1) {
  ui_clear();
  ui_panel(16, 44, 208, 74, UI_PANEL, UI_WARN);    /* framed destructive-confirm */
  ui_text(28, 54, UI_WARN, title);
  if (l1) ui_text(28, 76, UI_TEXT, l1);
  ui_text(28, 98, UI_TEXT, "A = yes");
  ui_text(28, 110, UI_DIM, "B = no");
  u16 k; do { vsync(); k = key_hit(KEY_A | KEY_B); } while (!k);
  bool yes = (k & KEY_A) != 0;
  if (yes) snd_ok(); else snd_back();
  return yes;
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
    if (s >= 0) { memcpy(pk_box_slot(g_pc, b, s), rec80, 80); return app_commit_pc(); }
  }
  snd_deny();
  msg_wait("PC FULL", UI_WARN, "No empty PC box slot in the", "loaded game.");
  return false;
}

static bool app_copy(uint8_t* rec, bool is_party) {
  clip_copy_from(&g_clip, rec, is_party);
  msg_wait("COPIED", UI_OK, "PASTE places it in a slot.", "(it survives until overwritten)");
  return false;                                          /* no save change */
}

static bool app_paste(uint8_t* rec, bool is_party, AppCommitFn commit, bool occupied) {
  if (!g_clip.occupied) return false;
  if (occupied && !app_confirm("Overwrite this Pokemon?", "Paste the copied mon here?")) return false;
  uint8_t out[100];
  if (!clip_to_record(&g_clip, is_party, out)) return false;
  memcpy(rec, out, is_party ? 100 : 80);
  return commit ? commit() : false;
}

static bool app_duplicate(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block, int box) {
  if (is_party) {
    if (party_count(block, g_frlg) >= 6) { snd_deny(); msg_wait("PARTY FULL", UI_WARN, "Release a mon first.", 0); return false; }
    uint8_t out[100]; memcpy(out, rec, 100);
    party_append(block, g_frlg, out);
  } else {
    int fs = box_free_slot(block, box);
    if (fs < 0) { snd_deny(); msg_wait("BOX FULL", UI_WARN, "No empty slot in this box.", 0); return false; }
    memcpy(pk_box_slot(block, box, fs), rec, 80);
  }
  return commit ? commit() : false;
}

static bool app_release(uint8_t* rec, bool is_party, AppCommitFn commit, uint8_t* block, int box, int slot) {
  (void)rec;
  if (is_party && party_count(block, g_frlg) <= 1) {
    snd_deny();
    msg_wait("CAN'T RELEASE", UI_WARN, "The party can't be empty.", 0);
    return false;
  }
  if (!app_confirm("Release this Pokemon?", "It is permanently deleted.")) return false;
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
bool app_mon_menu(uint8_t* rec, bool is_party, bool is_bank, AppCommitFn commit, uint8_t* block, int box, int slot) {
  PkMon m0;
  bool occupied = pk_decode_mon(rec, is_party, &m0);
  if (occupied) pk_resolve(&m0);

  if (!app_can_edit()) {                                 /* read-only carts: view only */
    if (occupied) { uint8_t d[100]; int card = 0; pdna_inspect(rec, is_party, false, d, 0, &card); }
    return false;
  }

  enum { A_SUMMARY, A_ITEM, A_MOVES, A_LEGAL, A_MOVE, A_COPY, A_PASTE, A_DUP, A_EXPORT, A_TOGAME, A_RELEASE, A_TAKEITEM, A_GIVEITEM, A_CANCEL };
  int act[16]; const char* lab[16]; int n = 0;
  if (occupied) {
    lab[n]="VIEW / EDIT"; act[n++]=A_SUMMARY;     /* opens the editable summary (moves edited there) */
    lab[n]="ITEM";    act[n++]=A_ITEM;
    lab[n]="LEGALITY"; act[n++]=A_LEGAL;
    if (!is_party) { lab[n]="MOVE"; act[n++]=A_MOVE; }   /* box: pick up + reposition */
    lab[n]="COPY";    act[n++]=A_COPY;
    if (g_clip.occupied) { lab[n]="PASTE"; act[n++]=A_PASTE; }
    lab[n]="DUPLICATE"; act[n++]=A_DUP;
    if (is_bank) { lab[n]="TO GAME"; act[n++]=A_TOGAME; }   /* bank: inject into the loaded save */
    else         { lab[n]="EXPORT .pk"; act[n++]=A_EXPORT; }/* PC/party: write a .pk3 to the bank dir */
    if (m0.heldItem && !g_item_held) { lab[n]="TAKE ITEM"; act[n++]=A_TAKEITEM; }
    if (g_item_held)                 { lab[n]="GIVE ITEM"; act[n++]=A_GIVEITEM; }
    lab[n]="RELEASE";   act[n++]=A_RELEASE;
  } else if (g_clip.occupied) {
    lab[n]="PASTE HERE"; act[n++]=A_PASTE;
  } else {
    return false;                                        /* empty + nothing to paste */
  }
  lab[n]="CANCEL"; act[n++]=A_CANCEL;

  char title[16];
  ui_truncate(title, occupied ? (m0.nickname[0] ? m0.nickname : pk_species_name(m0.species)) : "EMPTY", 11);
  const int vis = n < 9 ? n : 9;                    /* scroll if more actions than fit */
  const int mx = 138, mw = 100, mh = 18 + vis * 13 + 11, my = 80 - mh / 2;
  int sel = 0, top = 0;
  for (;;) {
    if (sel < top) top = sel;
    if (sel >= top + vis) top = sel - vis + 1;
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, title);
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < vis && top + i < n; i++) {
      int y = my + 18 + i * 13; bool s = (top + i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 12, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, lab[top + i]);
    }
    ui_text(mx + 6, my + mh - 9, UI_DIM, "A pick  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return false;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : n - 1;
    else if (k & KEY_DOWN) sel = (sel + 1) % n;
    else if (k & KEY_A) {
      switch (act[sel]) {
        case A_SUMMARY: return is_party ? app_edit_commit(rec, is_party, commit)
                                        : app_box_browse(block, box, slot, commit);   /* box: scroll mons */
        case A_ITEM:    return app_quick_item (rec, is_party, commit);
        case A_LEGAL:   pdna_legality_show(&m0); return false;
        case A_MOVE:    g_move_req = true; return false;            /* box loop handles the move */
        case A_EXPORT:  pdna_pk_export(rec, &m0); return false;   /* writes a .pk3, not the save */
        case A_TOGAME:  return app_inject_to_game(rec);           /* bank -> loaded save's PC */
        case A_COPY:    return app_copy(rec, is_party);
        case A_PASTE:   return app_paste(rec, is_party, commit, occupied);
        case A_DUP:     return app_duplicate(rec, is_party, commit, block, box);
        case A_RELEASE: return app_release(rec, is_party, commit, block, box, slot);
        case A_TAKEITEM:return app_take_item(rec, is_party, commit);
        case A_GIVEITEM:return app_give_item(rec, is_party, commit);
        default:        return false;                    /* CANCEL */
      }
    }
  }
}

/* Party list view. A opens the summary; SELECT switches to PC boxes; B exits.
 * Returns 0 (back to file browser) or 1 (switch to boxes). */
static int party_list(void) {
  int sel = 0;
  for (;;) {
    ui_clear();
    char line[48];
    siprintf(line, "%s  -  %s", g_vinfo.trainer_name, ver_label(g_vinfo.version_guess, g_frlg));
    ui_text(4, 2, UI_TITLE, line);
    ui_hline(0, 11, UI_SCR_W, UI_BORDER);

    if (g_nparty == 0) ui_text(6, 40, UI_WARN, "No Pokemon in party.");
    for (int i = 0; i < g_nparty; i++) {
      int y = 16 + i * 22;
      PkMon* p = &g_party[i];
      if (i == sel) ui_panel(2, y - 2, 236, 20, UI_SEL, UI_TITLE);
      ui_icon_sub(6, y, mon_icon_for_form(p->species, p->form));   /* 16x16 from the 32x32 icon */
      char nm[16];
      ui_truncate(nm, p->nickname[0] ? p->nickname : pk_species_name(p->species), 11);
      siprintf(line, "%-11s Lv%u", nm, (unsigned)p->level);
      ui_text(26, y, i == sel ? UI_SELTEXT : UI_TEXT, line);
      siprintf(line, "%s%s%s", pk_species_name(p->species),
               p->isShiny ? "  *" : "", p->isEgg ? "  EGG" : "");
      ui_text(26, y + 9, UI_DIM, line);
    }

    ui_hline(0, 151, UI_SCR_W, UI_BORDER);
    ui_text(4, 152, UI_DIM, g_have_pc ? "A actions  SEL boxes  START card  B back"
                                      : "A actions  START card  B back");

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B | KEY_SELECT | KEY_START);
    if      (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN) { if (sel < g_nparty - 1) sel++; }
    else if (k & KEY_B)    return 0;
    else if (k & KEY_START) return 2;
    else if (k & KEY_SELECT) { if (g_have_pc) return 1; }
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
    ui_text(6, 14, UI_WARN, "Editing raw flags can break a save");
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
static bool data_editor(void) {
  int tab = 0;                                   /* 0=counters 1=bag 2=flags */
  int sel = 0, top = 0, pocket = 0;
  bool dirty = false, flag_warned = false;

  for (;;) {
    ui_clear();
    static const char* const TAB[3] = { "COUNTERS", "BAG", "FLAGS" };
    for (int t = 0; t < 3; t++) {
      int x = 4 + t * 80; bool s = (t == tab);
      if (s) ui_panel(x, 0, 76, 12, UI_SEL, UI_TITLE);
      ui_text(x + 6, 2, s ? UI_SELTEXT : UI_DIM, TAB[t]);
    }
    ui_hline(0, 13, UI_SCR_W, UI_BORDER);

    if (tab == 0) {                              /* ---- counters (row 0 = Money) ---- */
      int N = pk_game_stat_count(g_game) + 1;
      if (sel >= N) sel = N - 1;
      if (sel < top) top = sel; if (sel >= top + 14) top = sel - 13;
      char row[44];
      for (int i = 0; i < 14 && top + i < N; i++) {
        int r = top + i, y = 16 + i * 9; bool s = (r == sel);
        if (r == 0) siprintf(row, "%-20s %lu", "Money", (unsigned long)pk_money(g_sb1, g_sb2, g_game));
        else        siprintf(row, "%-20s %lu", pk_game_stat_name(r - 1), (unsigned long)pk_game_stat(g_sb1, g_sb2, g_game, r - 1));
        char rt[44]; ui_truncate(rt, row, 29);
        if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
        ui_text(4, y, s ? UI_SELTEXT : UI_TEXT, rt);
      }
      ui_text(4, 152, UI_DIM, "A edit  U/D  L/R tab  B done");
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
      ui_text(4, 152, UI_DIM, "A edit  SEL pocket  L/R tab  B done");
    } else {                                     /* ---- flags (named list) ---- */
      const NamedFlag* nf; int nc = pk_named_flags(g_game, &nf);
      int total = nc + 1;                         /* + trailing raw-browser row */
      while (sel < nc && nf[sel].num == NAMED_FLAG_HEADER && sel < total - 1) sel++;
      if (sel >= total) sel = total - 1; if (sel < 0) sel = 0;
      if (sel < top) top = sel; if (sel >= top + 14) top = sel - 13;
      ui_text(6, 15, UI_DIRCLR, "Named flags");
      char row[40];
      for (int i = 0; i < 14 && top + i < total; i++) {
        int r = top + i, y = 26 + i * 9; bool s = (r == sel);
        if (r == nc) {                            /* trailing: drill to raw view */
          if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
          ui_text(8, y, s ? UI_SELTEXT : UI_DIM, "Raw flag browser (#N)...");
        } else if (nf[r].num == NAMED_FLAG_HEADER) {
          ui_text(4, y, UI_DIRCLR, nf[r].name);   /* category header */
        } else {
          bool on = pk_flag_get(g_sb1, g_game, nf[r].num);
          siprintf(row, "%-22s %s", nf[r].name, on ? "ON" : "off");
          char rt[40]; ui_truncate(rt, row, 29);
          if (s) ui_panel(2, y - 1, 236, 9, UI_SEL, UI_TITLE);
          ui_text(8, y, s ? UI_SELTEXT : (on ? UI_OK : UI_DIM), rt);
        }
      }
      ui_text(4, 152, UI_DIM, "A toggle  SEL jump cat  L/R tab  B done");
    }

    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A | KEY_B | KEY_SELECT);
    if (k & KEY_B) break;
    else if (k & KEY_L) { snd_tab(); tab = (tab + 2) % 3; sel = (tab == 2) ? 1 : 0; top = 0; }
    else if (k & KEY_R) { snd_tab(); tab = (tab + 1) % 3; sel = (tab == 2) ? 1 : 0; top = 0; }
    else if (tab == 2) {                         /* named flags nav/toggle */
      const NamedFlag* nf; int nc = pk_named_flags(g_game, &nf);
      int total = nc + 1;
      if (k & KEY_UP)   { do { if (sel > 0) sel--; else break; } while (sel < nc && nf[sel].num == NAMED_FLAG_HEADER); }
      else if (k & KEY_DOWN) { do { if (sel < total - 1) sel++; else break; } while (sel < nc && nf[sel].num == NAMED_FLAG_HEADER); }
      else if (k & KEY_SELECT) {                  /* jump to the next category header's first flag */
        snd_tab();
        int s = sel;
        for (int step = 0; step < total; step++) {
          s = (s + 1) % total;
          if (s < nc && nf[s].num == NAMED_FLAG_HEADER) { sel = (s + 1 < nc) ? s + 1 : s; break; }
          if (s == nc) { sel = nc; break; }       /* wrapped to the raw-browser row */
        }
        top = 0;
      }
      else if (k & KEY_A) {
        if (sel == nc) flags_raw_view(&dirty, &flag_warned);   /* drill into raw */
        else if (nf[sel].num != NAMED_FLAG_HEADER) {
          if (!flag_warned) { msg_wait("CAUTION", UI_WARN, "Toggling story flags can", "soft-lock the save."); flag_warned = true; }
          pk_flag_set(g_sb1, g_game, nf[sel].num, !pk_flag_get(g_sb1, g_game, nf[sel].num)); dirty = true;
        }
      }
    } else if (k & KEY_UP)   { if (sel > 0) sel--; }
    else if (k & KEY_DOWN)   sel++;
    else if (tab == 1 && (k & KEY_SELECT)) { snd_tab(); pocket = (pocket + 1) % POCKET_COUNT; sel = top = 0; }
    else if (k & KEY_A) {
      if (tab == 0) {                            /* edit money (row 0) or a counter */
        if (sel == 0) {
          uint32_t v = osk_number("MONEY", pk_money(g_sb1, g_sb2, g_game), 999999);
          pk_set_money(g_sb1, g_sb2, g_game, v); dirty = true;
        } else {
          int st = sel - 1;
          uint32_t v = osk_number("STAT VALUE", pk_game_stat(g_sb1, g_sb2, g_game, st), 0xFFFFFFFFu);
          pk_set_game_stat(g_sb1, g_sb2, g_game, st, v); dirty = true;
        }
      } else if (tab == 1) {                     /* edit a bag slot */
        uint16_t cur = pk_bag_item(g_sb1, g_game, pocket, sel);
        uint16_t id = pick_item(cur);
        if (id != 0xFFFF) {
          uint16_t q = 0;
          if (id != 0) q = (uint16_t)osk_number("QUANTITY", pk_bag_qty(g_sb1, g_sb2, g_game, pocket, sel) ? pk_bag_qty(g_sb1, g_sb2, g_game, pocket, sel) : 1, 999);
          pk_bag_set(g_sb1, g_sb2, g_game, pocket, sel, id, q); dirty = true;
        }
      }
    }
  }

  if (dirty) {                                       /* confirm before the silent write */
    if (!app_confirm("Save data changes?", "Money, bag and flag edits write now."))
      return false;
    return app_commit_block(1, 4, g_sb1);            /* one verified write on exit */
  }
  return false;
}

/* START menu from the box/party: pick a destination screen. */
static int nav_menu(void) {                            /* 0=card 1=bank 2=data 3=back */
  static const char* const L[4] = { "Trainer card", "Bank", "Data editor", "Back" };
  const int mx = 60, my = 48, mw = 120, mh = 18 + 4 * 14 + 11;
  int sel = 0;
  for (;;) {
    ui_panel(mx, my, mw, mh, UI_PANEL, UI_BORDER);
    ui_text(mx + 6, my + 4, UI_TITLE, "MENU");
    ui_hline(mx + 2, my + 15, mw - 4, UI_BORDER);
    for (int i = 0; i < 4; i++) {
      int y = my + 18 + i * 14; bool s = (i == sel);
      if (s) ui_panel(mx + 2, y - 1, mw - 4, 13, UI_SEL, UI_TITLE);
      ui_text(mx + 10, y, s ? UI_SELTEXT : UI_TEXT, L[i]);
    }
    ui_text(mx + 6, my + mh - 9, UI_DIM, "A pick  B back");
    u16 k = wait_keys(KEY_UP | KEY_DOWN | KEY_A | KEY_B);
    if (k & KEY_B) return 3;
    else if (k & KEY_UP)   sel = (sel > 0) ? sel - 1 : 3;
    else if (k & KEY_DOWN) sel = (sel + 1) % 4;
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
  s.start_box  = pk_current_box(g_pc);
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
  return s;
}

/* Leaving the open save: if move-mode left unsaved repositions, ask once. A writes
 * them (the verified PC commit); B discards by reloading g_pc from the untouched
 * in-RAM save image (we never wrote those moves to g_save). */
static void flush_pc_on_exit(void) {
  if (!app_pc_dirty()) return;
  if (app_confirm("Save box changes?", "Save the Pokemon you moved?")) {
    app_commit_pc();
  } else {
    gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc);   /* revert uncommitted moves */
    g_pc_dirty = false;
  }
}

/* Load the picked save and show it: start in the PC boxes; SELECT toggles to the
 * party list and back; B from either returns to the file browser. */
static void view_save(const char* path) {
  g_pc_dirty = false;                          /* fresh save: no pending moves */
  strncpy(g_path, path, sizeof(g_path) - 1);
  g_path[sizeof(g_path) - 1] = 0;
  uint32_t sz = 0;
  SfStatus st = sf_read_full(path, g_save, G3_SAVE_FILE_SIZE, &sz);
  if (st != SF_OK || sz < (uint32_t)G3_SLOT_BYTES ||
      !gen3_parse(g_save, sz, &g_vinfo) || !g_vinfo.valid || !g_vinfo.sb1_ok ||
      gen3_read_saveblock1(g_save, g_vinfo.slot, g_sb1) != G3_SAVEBLOCK1_BYTES) {
    ui_clear();
    ui_text(6, 60, UI_WARN, "Cannot read this save.");
    ui_text(6, 76, UI_DIM, st != SF_OK ? sf_status_str(st) : "not a valid Gen-3 .sav");
    ui_text(4, 150, UI_DIM, "B=back");
    wait_keys(KEY_B);
    return;
  }

  g_frlg = false;
  g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);
  for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
  g_have_pc = (gen3_read_pc_storage(g_save, g_vinfo.slot, g_pc) == G3_PC_BYTES);

  /* SaveBlock2 (section 0) for the trainer card + per-game layout for stats */
  int s0 = gen3_find_section(g_save, g_vinfo.slot, 0);
  if (s0 >= 0)
    memcpy(g_sb2, g_save + (uint32_t)g_vinfo.slot * G3_SLOT_BYTES + (uint32_t)s0 * G3_SECTOR_SIZE,
           G3_SECTOR_DATA_SIZE);
  g_game = g_frlg ? PK_FRLG : (g_vinfo.version_guess == G3_VER_RS ? PK_RS : PK_EMERALD);

  int mode = g_have_pc ? 0 : 1;          /* start in PC boxes (per request) */
  BoxSource pcs = pc_box_source();
  for (;;) {
    int r = (mode == 0) ? pdna_box(&pcs) : party_list();
    if (r == 0) { flush_pc_on_exit(); return; }  /* B -> file browser (prompt deferred moves) */
    if (r == 2) {                                /* START -> nav menu */
      int dest = nav_menu();
      if (dest == 0) pdna_trainer(g_sb1, g_sb2, &g_vinfo, g_game);
      else if (dest == 1) {                       /* bank: parallel boxes; copy/paste moves mons */
        pdna_bank_show();
        g_nparty = pk_read_party_auto(g_sb1, g_party, &g_frlg);   /* a paste may have hit the party */
        for (int i = 0; i < g_nparty; i++) pk_resolve(&g_party[i]);
      }
      else if (dest == 2) {                      /* data editor (edits the save) */
        if (app_can_edit()) data_editor();
        else { snd_deny(); msg_wait("READ-ONLY", UI_WARN, "Editing needs EZ-Flash Omega.", 0); }
      }
      continue;
    }
    if (!g_have_pc) { flush_pc_on_exit(); return; }   /* nothing to toggle to */
    mode ^= 1;                                    /* SELECT -> toggle box/party */
  }
}

int main(void) {
  init_system();
  log_init();
  log_line("=== PokeDNA (M0) ===");
  log_line("mGBA debug log: %s", log_under_mgba() ? "active" : "absent");

  ui_clear();
  ui_text(6, 70, UI_TITLE, "Detecting flashcart...");
  if (!flashcartio_activate()) halt_msg("No flashcart detected!");
  log_line("flashcart: %s", flashcart_name());

  FATFS fs;                                  /* lives forever (main never returns) */
  FRESULT fr = f_mount(&fs, "", 1);
  if (fr != FR_OK) { log_line("f_mount failed (fr=%d)", fr); halt_msg("SD mount failed!"); }
  log_line("SD mounted OK");
  log_flush_to_sd(LOG_PATH);
  snd_boot();                                /* welcome chime = audio self-test */

  strcpy(g_cwd, "/");
  for (;;) {
    char path[PATH_MAX];
    if (browse_pick(path, sizeof(path))) view_save(path);
  }
  return 0;
}
